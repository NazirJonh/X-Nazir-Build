/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 */

#include "ED_material_combined.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_image.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"
#include "BLI_rect.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "ED_material_bake.hh"

namespace blender::ed::material_combined {

/* -------------------------------------------------------------------- */
/** \name Channel Set
 * \{ */

/**
 * The channels the Combined preview shades, and the only ones it gathers.
 *
 * Custom and Height are absent: Custom is a vertex-paint attribute with no shading meaning, and
 * Height has no part in phase 1. Listing them would put entries in `inputs_hash` that can never
 * change the pixels.
 */
static constexpr std::array<eMaterialPaintChannel, 8> combined_shading_channels = {
    PAINT_MATERIAL_CHANNEL_BASE_COLOR,
    PAINT_MATERIAL_CHANNEL_METALLIC,
    PAINT_MATERIAL_CHANNEL_ROUGHNESS,
    PAINT_MATERIAL_CHANNEL_SPECULAR,
    PAINT_MATERIAL_CHANNEL_NORMAL,
    PAINT_MATERIAL_CHANNEL_AO,
    PAINT_MATERIAL_CHANNEL_ALPHA,
    PAINT_MATERIAL_CHANNEL_EMISSION,
};

/**
 * Principled's Emission Strength for \a ma, or 1 when it cannot be read.
 *
 * The Emission channel maps to the *Emission Color* socket, which defaults to white; the strength
 * beside it defaults to zero. Reading only the colour therefore adds full white emission to every
 * ordinary material and washes the preview out completely, so the strength has to be resolved
 * alongside it even though it is not a paint channel and has no descriptor row.
 *
 * A linked strength is reported as 1 rather than evaluated: driving it is a deliberate act, 1 is
 * the value Blender's own tooltip calls the reference ("the object has the exact same color as the
 * Emission Color"), and evaluating the link would mean a bake for a scalar.
 */
static float combined_emission_strength(const Material &ma)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return 1.0f;
  }
  const bNodeSocket *strength = bke::node_find_socket(
      *principled, SOCK_IN, "Emission Strength"_ustr);
  if (strength == nullptr || strength->type != SOCK_FLOAT) {
    return 1.0f;
  }
  if (BKE_paint_material_source_socket(*strength) != nullptr) {
    return 1.0f;
  }
  return static_cast<const bNodeSocketValueFloat *>(strength->default_value)->value;
}

/** Channels whose values are colours and therefore may need linearizing. */
static bool combined_channel_is_color(const eMaterialPaintChannel channel)
{
  return channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR ||
         channel == PAINT_MATERIAL_CHANNEL_EMISSION;
}

/**
 * The bake resolution the preview asks for.
 *
 * The same value #BrushMaterialPaint.source_bake_size defaults to, and deliberately a constant:
 * #material_source_bake_get and #material_source_bake_ensure are keyed on the resolution, so a
 * preview asking for one size while the paint path bakes another would keep two bakes of the same
 * material alive and never find the one it started.
 */
static constexpr int combined_bake_resolution = 1024;

/** Where a channel's value came from. Folded into `inputs_hash`, so it is an explicit enum. */
enum class CombinedChannelSource : uint8_t {
  Default = 0,
  Constant,
  LayerStack,
  DirectImage,
  Bake,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Acquisition Scope
 * \{ */

/**
 * Holds everything a gather acquired, and releases it when the gather ends.
 *
 * The evaluator reads raw #ImBuf pointers, so every one of them has to stay alive across the
 * synchronous `cache_ensure` call and be released immediately afterwards. A guard rather than
 * hand-written releases because the gather has a dozen early exits, and one missed release leaks
 * an image lock for the rest of the session.
 */
class GatherScope {
  struct Acquired {
    Image *image;
    ImBuf *ibuf;
    void *lock;
  };
  Vector<Acquired> acquired_;
  /**
   * Buffers this gather holds a reference to: the scratch it allocated for a resampled bake, and
   * every composite it read.
   *
   * The composites need it because #BKE_paint_material_composite_cache_ensure enforces the
   * composite cache's size budget on the way out, evicting every entry but the one it just made --
   * so gathering the eighth channel can free the buffer the first channel handed over. #ImBuf is
   * reference counted, so a reference held for the length of the gather is enough.
   */
  Vector<ImBuf *> owned_;
  /* Keeps the bake's buffers alive: #MaterialSourceBake owns them and the cache may drop its own
   * entry while this gather is reading it. */
  std::shared_ptr<const material_bake::MaterialSourceBake> bake_;

 public:
  ~GatherScope()
  {
    for (ImBuf *ibuf : owned_) {
      IMB_freeImBuf(ibuf);
    }
    for (const Acquired &entry : acquired_) {
      BKE_image_release_ibuf(entry.image, entry.ibuf, entry.lock);
    }
  }

  /** Acquire with a **copy** of \a iuser: acquisition writes tile and frame state into it. */
  ImBuf *acquire(Image &image, const ImageUser *iuser)
  {
    ImageUser iuser_copy;
    ImageUser *iuser_arg = nullptr;
    if (iuser != nullptr) {
      iuser_copy = *iuser;
      iuser_arg = &iuser_copy;
    }
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, iuser_arg, &lock);
    if (ibuf == nullptr) {
      /* The lock is taken even when there is no buffer, so it still has to go back. */
      BKE_image_release_ibuf(&image, nullptr, lock);
      return nullptr;
    }
    acquired_.append({&image, ibuf, lock});
    return ibuf;
  }

  void hold_bake(std::shared_ptr<const material_bake::MaterialSourceBake> bake)
  {
    bake_ = std::move(bake);
  }

  const material_bake::MaterialSourceBake *bake() const
  {
    return bake_.get();
  }

  /** Take ownership of a buffer this gather allocated. */
  ImBuf *own(ImBuf *ibuf)
  {
    if (ibuf != nullptr) {
      owned_.append(ibuf);
    }
    return ibuf;
  }

  /** Hold a reference to a buffer this gather does not own, for the length of the gather. */
  ImBuf *hold_ref(ImBuf *ibuf)
  {
    if (ibuf != nullptr) {
      IMB_refImBuf(ibuf);
      owned_.append(ibuf);
    }
    return ibuf;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gathering
 * \{ */

/**
 * Nearest-sample \a src into a newly allocated buffer of \a width x \a height.
 *
 * Only ever applied to a bake. A bake is produced at its own square resolution, is always rebuilt
 * whole, and never reports a dirty rectangle, so sampling it costs nothing in correctness --
 * whereas resampling a layer stack or a directly linked image would mean transforming every dirty
 * rectangle between two coordinate systems, which is how stale pixels get left on screen.
 *
 * The caller owns the result.
 */
static ImBuf *combined_resample_nearest(const ImBuf &src, const int width, const int height)
{
  if (src.x <= 0 || src.y <= 0 || width <= 0 || height <= 0) {
    return nullptr;
  }
  const float *src_floats = src.float_data();
  const uchar *src_bytes = src.byte_data();
  if (src_floats == nullptr && src_bytes == nullptr) {
    return nullptr;
  }

  ImBuf *dst = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::FloatData);
  if (dst == nullptr) {
    return nullptr;
  }
  dst->channels = 4;
  float *dst_floats = dst->float_data_for_write();
  if (dst_floats == nullptr) {
    IMB_freeImBuf(dst);
    return nullptr;
  }

  for (const int y : IndexRange(height)) {
    const int src_y = std::min(int((int64_t(y) * src.y) / height), src.y - 1);
    for (const int x : IndexRange(width)) {
      const int src_x = std::min(int((int64_t(x) * src.x) / width), src.x - 1);
      const int64_t src_texel = int64_t(src_y) * src.x + src_x;
      float *out = dst_floats + (int64_t(y) * width + x) * 4;
      if (src_floats != nullptr) {
        copy_v4_v4(out, src_floats + src_texel * 4);
      }
      else {
        const uchar *p = src_bytes + src_texel * 4;
        out[0] = float(p[0]) * (1.0f / 255.0f);
        out[1] = float(p[1]) * (1.0f / 255.0f);
        out[2] = float(p[2]) * (1.0f / 255.0f);
        out[3] = float(p[3]) * (1.0f / 255.0f);
      }
    }
  }
  return dst;
}

/**
 * Whether \a ibuf's byte pixels are sRGB-encoded and therefore need linearizing.
 *
 * Read from the buffer's assigned colorspace rather than assumed: a layer stack hands on whichever
 * space its bottom layer decided, and a directly linked image carries its own settings. A float
 * buffer is already linear by the time it reaches the evaluator, so only byte buffers answer true.
 */
static bool combined_ibuf_is_srgb(const ImBuf &ibuf)
{
  if (ibuf.byte_data() == nullptr || ibuf.float_data() != nullptr) {
    return false;
  }
  const char *colorspace = IMB_colormanagement_get_byte_colorspace(&ibuf);
  return colorspace != nullptr && IMB_colormanagement_space_name_is_srgb(colorspace);
}

ImBuf *combined_preview_ensure(Main &bmain,
                               const Material &ma,
                               const CombinedPreviewLighting &lighting,
                               const CombinedPreviewRequest &request,
                               uint64_t *r_revision,
                               rcti *r_changed_region,
                               CombinedEvalStats *r_stats)
{
  if (r_revision != nullptr) {
    *r_revision = 0;
  }
  if (r_changed_region != nullptr) {
    BLI_rcti_init(r_changed_region, 0, 0, 0, 0);
  }
  bke::paint_perf::ScopedTimer perf_total("combined_preview_ensure");
  GatherScope scope;

  /* A layered material answers from its description alone, and every channel it lacks rows for
   * keeps the neutral default below. Its generated tree is not a source material: resolving it
   * would read the layer group as a procedural graph, bake it, and let the bake stand in for rows
   * the stack already describes. */
  const MaterialSourceResolve resolve = BKE_paint_layers_is_layered(ma) ?
                                            MaterialSourceResolve{} :
                                            BKE_paint_material_source_resolve(&ma);

  bool any_baked = false;
  for (const eMaterialPaintChannel channel : combined_shading_channels) {
    any_baked |= resolve.channels[int(channel)] == ChannelResolution::Baked;
  }
  if (any_baked) {
    /* Whatever is cached right now: the bake runs in a job, and a preview must never wait on a
     * render. A channel with no bake yet shows its default until one lands, at which point the
     * bake's identity moves in `inputs_hash` and the preview rebuilds. */
    scope.hold_bake(material_bake::material_source_bake_get(ma, combined_bake_resolution));
  }

  /* Every channel's layer stack, derived once.
   *
   * The walk is not free -- it follows reroutes, muted nodes and group instances from the
   * Principled socket for every channel -- and the size decision below and the gather beneath it
   * both need the answer. Deriving it twice per channel doubled the cost of a redraw that shades
   * nothing at all. */
  std::array<Vector<PaintMaterialCompositeImageLayer>, combined_shading_channels.size()> stacks;
  std::array<bool, combined_shading_channels.size()> has_stack;
  {
    bke::paint_perf::ScopedTimer perf_stacks("gather.stacks");
    int stacks_found = 0;
    for (const int i : IndexRange(int64_t(combined_shading_channels.size()))) {
      /* A layered material is read from its description: its generated tree is not a Mix chain
       * the graph walk could follow. */
      /* Without the pixels of its Anchor Point slices: the hash and the subscriptions are all a
       * frame that changed nothing needs, and the cache asks for the full stack when it has to
       * evaluate (see the `full_layers` of #BKE_paint_material_composite_cache_ensure). */
      has_stack[i] = BKE_paint_layers_is_layered(ma) ?
                         BKE_paint_layers_composite_image_layers(
                             ma, combined_shading_channels[i], stacks[i], /*describe_only=*/true) :
                         BKE_paint_material_composite_stack_from_material(
                             bmain, ma, combined_shading_channels[i], stacks[i]);
      stacks_found += has_stack[i] ? 1 : 0;
    }
    perf_stacks.note("channels with a stack=%d of %d",
                     stacks_found,
                     int(combined_shading_channels.size()));
  }

  /* The canonical size is the first channel that resolves to a layer stack or to a direct image,
   * Base Color first. Everything that disagrees with it falls back to its constant rather than
   * being resampled -- both of those report their edits as rectangles in their own coordinates,
   * and transforming every dirty rectangle between two spaces is how stale pixels survive. */
  int width = 0;
  int height = 0;
  for (const int channel_index : IndexRange(int64_t(combined_shading_channels.size()))) {
    const eMaterialPaintChannel channel = combined_shading_channels[channel_index];
    if (has_stack[channel_index]) {
      int stack_width = 0;
      int stack_height = 0;
      if (BKE_paint_material_composite_stack_dimensions(
              stacks[channel_index], stack_width, stack_height))
      {
        width = stack_width;
        height = stack_height;
        break;
      }
    }
    if (resolve.channels[int(channel)] == ChannelResolution::Image &&
        resolve.images[int(channel)].image != nullptr)
    {
      if (const ImBuf *ibuf = scope.acquire(*resolve.images[int(channel)].image,
                                            resolve.images[int(channel)].iuser))
      {
        width = ibuf->x;
        height = ibuf->y;
        break;
      }
    }
  }

  if (width <= 0 || height <= 0) {
    return nullptr;
  }

  /* The output resolution: the largest power-of-two fraction of the canvas that still covers what
   * the caller can display.
   *
   * Quantised to octaves rather than fitted exactly, because a changed output size is a full
   * rebuild -- an exact fit would rebuild the whole preview on every pixel of zoom, which costs
   * more than the shading it saves. Powers of two also keep the box filter's footprint whole. */
  int output_width = width;
  int output_height = height;

  /* The octave already in use, when it is still a power-of-two fraction of this canvas.
   *
   * Every caller starts from it, not only the one that can say how much it will display. The
   * resolution belongs to whoever is *drawing* the preview; a caller that only reads pixels -- the
   * scopes, the eyedropper, a save -- passes no cap, and deriving one from the canvas there would
   * reset the viewport's octave on every panel redraw. That is a full rebuild per frame, for a
   * reader that is served correctly either way: the buffer covers the whole canvas at any octave.
   */
  int previous_width = 0;
  int previous_height = 0;
  const bool previous_is_usable = BKE_paint_material_combined_cache_output_size_get(
                                      ma, previous_width, previous_height) &&
                                  previous_width > 0 && previous_height > 0 &&
                                  width % previous_width == 0 && height % previous_height == 0 &&
                                  is_power_of_2_i(width / previous_width) &&
                                  is_power_of_2_i(height / previous_height);
  if (previous_is_usable) {
    output_width = previous_width;
    output_height = previous_height;
  }

  if (request.max_output.x > 0 && request.max_output.y > 0) {
    /* An octave that no longer covers what has to be shown is not a starting point: climb back to
     * the canvas and let the halving below settle on the right one. */
    if (previous_is_usable &&
        (previous_width < request.max_output.x || previous_height < request.max_output.y))
    {
      output_width = width;
      output_height = height;
    }
    /* Halve only while the result would still cover the display with room to spare. That margin is
     * the hysteresis: the octave drops at 1.4x the display and only climbs back when it no longer
     * covers at all, so it takes a real change of zoom rather than a pixel of jitter to move it.
     */
    constexpr float octave_margin = 1.4f;
    while (output_width % 2 == 0 && output_height % 2 == 0 &&
           float(output_width / 2) >= float(request.max_output.x) * octave_margin &&
           float(output_height / 2) >= float(request.max_output.y) * octave_margin)
    {
      output_width /= 2;
      output_height /= 2;
    }
  }

  CombinedInputs inputs;
  inputs.width = width;
  inputs.height = height;
  inputs.output_width = output_width == width ? 0 : output_width;
  inputs.output_height = output_height == height ? 0 : output_height;
  inputs.emission_strength = combined_emission_strength(ma);
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    inputs.channels[i].constant = BKE_paint_material_combined_default_value(
        eMaterialPaintChannel(i));
  }

  Vector<Image *> dependency_images;
  rcti changed_region;
  BLI_rcti_init(&changed_region, 0, 0, 0, 0);

  /* Structural identity only, never pixels: composite revisions are deliberately excluded, since
   * they are what `changed_region` is for and folding them in would turn every dab into a full
   * rebuild. */
  uint64_t inputs_hash = get_default_hash(width, height, inputs.emission_strength);

  for (const int channel_index : IndexRange(int64_t(combined_shading_channels.size()))) {
    const eMaterialPaintChannel channel = combined_shading_channels[channel_index];
    CombinedChannelInput &input = inputs.channels[int(channel)];
    CombinedChannelSource source = CombinedChannelSource::Default;
    uint64_t source_detail = 0;

    /* 1. Layer stack, from the single derivation above. */
    if (has_stack[channel_index]) {
      const Span<PaintMaterialCompositeImageLayer> layers = stacks[channel_index];
      const uint64_t stack_hash = BKE_paint_material_composite_stack_hash(layers);
      rcti channel_changed;
      ImBuf *composite = nullptr;
      {
        bke::paint_perf::ScopedTimer perf_channel("gather.channel_composite");
        const auto describe_full = [&](Vector<PaintMaterialCompositeImageLayer> &r_full) {
          return BKE_paint_layers_composite_image_layers(ma, channel, r_full);
        };
        composite = BKE_paint_material_composite_cache_ensure(
            ma,
            channel,
            layers,
            stack_hash,
            nullptr,
            nullptr,
            &channel_changed,
            BKE_paint_layers_is_layered(ma) ?
                FunctionRef<bool(Vector<PaintMaterialCompositeImageLayer> &)>(describe_full) :
                FunctionRef<bool(Vector<PaintMaterialCompositeImageLayer> &)>());
        perf_channel.note("channel=%d layers=%d changed=%dx%d",
                          int(channel),
                          int(layers.size()),
                          BLI_rcti_size_x(&channel_changed),
                          BLI_rcti_size_y(&channel_changed));
      }
      if (composite != nullptr && composite->x == width && composite->y == height) {
        /* Referenced, not merely pointed at: the next channel's `cache_ensure` may evict this very
         * entry to stay inside the composite cache's budget. */
        input.ibuf = scope.hold_ref(composite);
        input.is_srgb = combined_channel_is_color(channel) && combined_ibuf_is_srgb(*composite);
        source = CombinedChannelSource::LayerStack;
        if (!BLI_rcti_is_empty(&channel_changed)) {
          if (BLI_rcti_is_empty(&changed_region)) {
            changed_region = channel_changed;
          }
          else {
            BLI_rcti_union(&changed_region, &channel_changed);
          }
        }
      }
    }

    /* 2. Direct image. */
    if (source == CombinedChannelSource::Default &&
        resolve.channels[int(channel)] == ChannelResolution::Image &&
        resolve.images[int(channel)].image != nullptr)
    {
      Image &image = *resolve.images[int(channel)].image;
      if (ImBuf *ibuf = scope.acquire(image, resolve.images[int(channel)].iuser)) {
        if (ibuf->x == width && ibuf->y == height) {
          input.ibuf = ibuf;
          input.is_srgb = combined_channel_is_color(channel) && combined_ibuf_is_srgb(*ibuf);
          source = CombinedChannelSource::DirectImage;
          source_detail = image.id.session_uid;
          dependency_images.append_non_duplicates(&image);
        }
      }
    }

    /* 3. Cached bake, nearest-sampled to the canonical size. */
    const material_bake::MaterialSourceBake *bake = scope.bake();
    if (source == CombinedChannelSource::Default && bake != nullptr &&
        bake->resolution(channel) == ChannelResolution::Baked)
    {
      if (const ImBuf *baked = bake->channel_image(channel)) {
        const ImBuf *usable = baked;
        if (baked->x != width || baked->y != height) {
          usable = scope.own(combined_resample_nearest(*baked, width, height));
        }
        if (usable != nullptr) {
          input.ibuf = usable;
          /* The bake is produced through the render pipeline and is already scene linear; the
           * evaluator honours #is_srgb for byte buffers only, so this stays false either way. */
          input.is_srgb = false;
          source = CombinedChannelSource::Bake;
          /* The bake's identity, not its pixels: when a fresher one lands the address or the
           * resolution moves, the hash changes and the preview rebuilds in full. That is also the
           * only way bake completion is noticed at all. */
          source_detail = get_default_hash(reinterpret_cast<const void *>(bake),
                                           combined_bake_resolution,
                                           int(bake->resolution(channel)));
        }
      }
    }

    /* 4. Constant. */
    if (source == CombinedChannelSource::Default) {
      if (bake != nullptr && bake->resolution(channel) == ChannelResolution::Constant) {
        input.constant = bake->channel_constant(channel);
        source = CombinedChannelSource::Constant;
      }
      else if (resolve.channels[int(channel)] == ChannelResolution::Constant) {
        input.constant = resolve.constants[int(channel)];
        source = CombinedChannelSource::Constant;
      }
    }

    /* 5. Default: #inputs.channels was pre-filled with it above. */

    inputs_hash = get_default_hash(
        inputs_hash, int(channel), int(source), source_detail, input.constant, int(input.is_srgb));
    if (bke::paint_perf::min_ms() == 0.0) {
      /* Only at threshold zero: a source or detail that differs between two frames is what turns a
       * cache hit into a full reshade. */
      printf("[PBR-PERF]   input channel=%d source=%d detail=%llu srgb=%d hash=%llu\n",
             int(channel),
             int(source),
             static_cast<unsigned long long>(source_detail),
             int(input.is_srgb),
             static_cast<unsigned long long>(inputs_hash));
    }

  }

  /* Both rectangles arrive in canvas texels and the cache works in output pixels. Rounded
   * outwards, because a dab that lost its edge to rounding would leave a stale fringe. */
  CombinedCacheRequest cache_request;
  cache_request.inputs_hash = inputs_hash;
  cache_request.dependency_images = dependency_images;
  cache_request.changed_region = changed_region;
  cache_request.clip = request.clip;
  if (inputs.output_width > 0) {
    const float scale_x = float(output_width) / float(width);
    const float scale_y = float(output_height) / float(height);
    const auto scale_rect = [&](rcti &rect) {
      if (BLI_rcti_is_empty(&rect)) {
        return;
      }
      BLI_rcti_init(&rect,
                    int(floorf(float(rect.xmin) * scale_x)),
                    int(ceilf(float(rect.xmax) * scale_x)),
                    int(floorf(float(rect.ymin) * scale_y)),
                    int(ceilf(float(rect.ymax) * scale_y)));
    };
    scale_rect(cache_request.changed_region);
    scale_rect(cache_request.clip);
  }

  /* Collected unconditionally, because the perf harness reports the evaluator separately from
   * everything else and the caller usually has no interest in the numbers. */
  /* Reported in the returned buffer's own texels, which is what the cache already works in. Not
   * scaled back up to the canvas: the buffer covers the whole canvas whatever its resolution, so a
   * consumer turning the rectangle into UV divides by the dimensions of the buffer it is reading
   * -- and handing it canvas texels would be wrong by exactly the octave. */
  CombinedEvalStats stats;
  bke::paint_perf::ScopedTimer perf_shade("gather.combined_cache_ensure");
  ImBuf *result = BKE_paint_material_combined_cache_ensure(
      ma, inputs, lighting, cache_request, r_revision, r_changed_region, &stats);
  perf_shade.note("canvas=%dx%d out=%dx%d eval=%.3f ms pixels=%lld dep_images=%d",
                  width,
                  height,
                  output_width,
                  output_height,
                  stats.elapsed_seconds * 1000.0,
                  long long(stats.pixels_processed),
                  int(dependency_images.size()));
  perf_total.note("canvas=%dx%d out=%dx%d", width, height, output_width, output_height);
  /* The output resolution, not the canvas: it is what the numbers have to be read against. */
  if (r_stats != nullptr) {
    *r_stats = stats;
  }
  return result;
}

void combined_preview_bake_ensure(const bContext &C, Material &ma)
{
  /* Nothing else would ever start this bake: #material_source_bake_ensure is reached from the
   * paint cursor and from RNA updates, neither of which fires for a user who merely opened the
   * Combined preview. Without this the procedural channels of such a material would show their
   * defaults for as long as the editor stays open. */
  if (BKE_paint_layers_is_layered(ma)) {
    /* Nothing to bake: see #combined_preview_ensure. */
    return;
  }
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&ma);
  bool needs_bake = false;
  for (const eMaterialPaintChannel channel : combined_shading_channels) {
    if (resolve.channels[int(channel)] == ChannelResolution::Baked) {
      needs_bake = true;
      break;
    }
  }
  if (!needs_bake) {
    /* A plain layer-stack material must not poke the bake machinery on every redraw. */
    return;
  }
  material_bake::material_source_bake_ensure(C, ma, combined_bake_resolution);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Combined Preview
 *
 * The Combined preview of a layered material evaluated by compute shaders straight into
 * the Image Editor's display texture. Mirrors `composite_layer_render` and
 * `BKE_paint_material_combined_eval` for the rows it can lay; anything else is refused up front so
 * the caller falls back to the CPU path instead of drawing something different.
 * \{ */

/** Mirrors the flags of `image_engine_combined.bsl.hh`. */
static constexpr int gpu_layer_init = 1;
static constexpr int gpu_layer_has_map = 2;
static constexpr int gpu_layer_alpha_coverage = 4;
static constexpr int gpu_layer_premultiplied = 8;
static constexpr int gpu_layer_mapped = 32;
static constexpr int gpu_layer_repeat_u = 64;
static constexpr int gpu_layer_repeat_v = 128;
static constexpr int gpu_layer_tangent_normal = 256;
static constexpr int gpu_layer_normal_directx = 262144;
static constexpr int gpu_layer_has_mask = 2048;
static constexpr int gpu_layer_row_store = 4096;
static constexpr int gpu_layer_row_use = 8192;
static constexpr int gpu_layer_empty_base = 16384;
static constexpr int gpu_layer_tracks_alpha = 32768;
static constexpr int gpu_layer_adjustment = 65536;
/* The row reads an Anchor Point slice from a screen-space slot. Mirrors the shader. */
static constexpr int gpu_layer_slot = 524288;
/* The steps of the folder pass, and how many folders may nest. */
static constexpr int gpu_folder_init = 0;
static constexpr int gpu_folder_blend = 1;
static constexpr int gpu_folder_resolve = 2;
static constexpr int gpu_folder_levels_max = 4;
/* The mask pass shares the layer pass's map flags; these are its own. */
static constexpr int gpu_mask_invert = 512;
static constexpr int gpu_mask_from_alpha = 1024;
/* The steps of the mask pass. */
static constexpr int gpu_mask_init = 0;
static constexpr int gpu_mask_item = 1;
static constexpr int gpu_mask_map = 2;
/* A map-driven coverage item: begin, one pass per colour (or one scalar), end. */
static constexpr int gpu_mask_cov_begin = 3;
static constexpr int gpu_mask_cov_color = 4;
static constexpr int gpu_mask_cov_scalar = 5;
static constexpr int gpu_mask_cov_end = 6;
/* Write the built mask factor into a screen-space slot for a mask Anchor Point slice. */
static constexpr int gpu_mask_slot_store = 7;
/* A mask item reads an Anchor Point slice from a screen-space slot. */
static constexpr int gpu_mask_slot = 2048;
/* The ID map is read at its nearest texel. */
static constexpr int gpu_map_nearest = 131072;

/** A content slice captured for the channel passes: screen-space colour and coverage. */
struct GpuSliceSlot {
  bUUID point;
  int channel;
  /** The first reader that asked for it, and how many readers share the slot, for the developer
   * verification. */
  bUUID reader;
  int reader_count = 1;
  gpu::Texture *color = nullptr;
  gpu::Texture *coverage = nullptr;
  /** Releases both textures however far the slot got; see #PaintLayersGpuSlotGuard. */
  PaintLayersGpuSlotGuard guard;
};

/** A mask slice captured for the channel passes: the factor in `.x`, grey duplicated in rgb. */
struct GpuMaskSlot {
  bUUID point;
  bUUID reader;
  int reader_count = 1;
  gpu::Texture *texture = nullptr;
  PaintLayersGpuSlotGuard guard;
};

/** What a capture job produces: a content pair, a mask rendered as content, or a mask slot. The
 * plan itself lives in BKE, so its ordering and dedup rules are testable without a GPU. */
using GpuCaptureKind = PaintLayersGpuCaptureKind;
using GpuCaptureJob = PaintLayersGpuCaptureJob;

/**
 * How much memory the Anchor Point slots may take. A 4K RGBA16F pair is about 132 MiB, so this
 * holds a few nested readers; past it the pass refuses and the CPU preview takes over.
 */
static constexpr size_t gpu_slot_budget_bytes = size_t(512) * 1024 * 1024;

bool combined_preview_gpu_enabled(const Material *ma)
{
  if (ma == nullptr || ma->paint_layers_composite_device != MA_PAINT_COMPOSITE_GPU) {
    return false;
  }
  /* `BLENDER_PBR_GPU_COMBINED=0` forces the CPU path for every material, a developer comparison. */
  static const bool env_allows = []() {
    const char *env = std::getenv("BLENDER_PBR_GPU_COMBINED");
    return env == nullptr || std::atoi(env) != 0;
  }();
  return env_allows;
}

/**
 * Report why the GPU path refused, once per change of reason rather than once per frame, and
 * return false for the caller to hand on. Printed through the perf log, so it follows
 * `BLENDER_PBR_PERF`.
 */
static const char *gpu_last_reason = nullptr;

/* The developer check of combined_preview_gpu_verify: whether the frame being shaded is one it will
 * compare, and the channel accumulators kept for it so a difference can be traced to a channel. Off
 * unless BLENDER_PBR_GPU_VERIFY=1: it shades the whole canvas on the CPU. */
static bool gpu_verify_due = false;
static std::array<Vector<float>, int(combined_shading_channels.size())> gpu_verify_channels;

static bool gpu_verify_enabled()
{
  static const bool enabled = []() {
    const char *env = std::getenv("BLENDER_PBR_GPU_VERIFY");
    return env != nullptr && std::atoi(env) != 0;
  }();
  return enabled;
}

static bool gpu_refuse(const char *reason)
{
  if (bke::paint_perf::enabled() && reason != gpu_last_reason) {
    printf("[PBR-PERF] gpu_combined refused: %s\n", reason);
    fflush(stdout);
  }
  gpu_last_reason = reason;
  return false;
}

/** Report, once after a refusal or at the first frame, that the GPU path is shading the preview. */
static void gpu_report_active()
{
  static const char *active = "active";
  if (bke::paint_perf::enabled() && gpu_last_reason != active) {
    printf("[PBR-PERF] gpu_combined active\n");
    fflush(stdout);
  }
  gpu_last_reason = active;
}

/**
 * Why the GPU path cannot lay \a layer the way the CPU composite does, or null when it can.
 *
 * The rule lives in BKE so it is testable without a GPU: see
 * #BKE_paint_layers_gpu_unsupported_reason. Anchor Point slices are supported through the
 * screen-space slots captured before the channel passes; a *baked* correction whose coverage lives
 * in a map at the reader's own UV remains impossible for the correction pass.
 */
static const char *gpu_layer_unsupported_reason(const PaintMaterialCompositeImageLayer &layer)
{
  return BKE_paint_layers_gpu_unsupported_reason(layer);
}

/**
 * The GPU texture of \a image read through \a image_user (null selects the default), and whether it
 * stores premultiplied colour, by the rule the texture upload used. The upload premultiplies a byte
 * buffer (assumed straight) and straightens a float one (assumed premultiplied) exactly when it
 * stores premultiplied, so dividing by alpha gives the straight colour `composite_decode_tile`
 * decodes. Null when the image has no GPU texture.
 */
static gpu::Texture *gpu_map_texture(Image *image,
                                     const ImageUser *image_user,
                                     bool &r_premultiplied)
{
  ImageUser iuser;
  if (image_user != nullptr) {
    iuser = *image_user;
  }
  else {
    BKE_imageuser_default(&iuser);
  }
  gpu::Texture *texture = BKE_image_get_gpu_texture(image, &iuser);
  if (texture == nullptr) {
    return nullptr;
  }
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  r_premultiplied = BKE_image_has_gpu_texture_premultiplied_alpha(image, ibuf);
  BKE_image_release_ibuf(image, ibuf, nullptr);
  return texture;
}

/**
 * Turn a row's UV \a mapping into the flags and values the shaders read, when it applies. The same
 * values the CPU resample reads, the scale through the shared normalization.
 */
static void gpu_mapping_setup(const PaintMaterialCompositeMapping &mapping,
                              int &r_flags,
                              float2 &r_offset,
                              float2 &r_scale,
                              float2 &r_rotation)
{
  if (!mapping.enabled) {
    return;
  }
  bool repeat_u = true;
  bool repeat_v = true;
  BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  r_flags |= gpu_layer_mapped;
  if (repeat_u) {
    r_flags |= gpu_layer_repeat_u;
  }
  if (repeat_v) {
    r_flags |= gpu_layer_repeat_v;
  }
  if (mapping.tangent_normal) {
    r_flags |= gpu_layer_tangent_normal;
  }
  r_offset = float2(mapping.offset[0], mapping.offset[1]);
  r_scale = float2(BKE_paint_layers_mapping_scale_normalize(mapping.scale[0]),
                   BKE_paint_layers_mapping_scale_normalize(mapping.scale[1]));
  r_rotation = float2(std::cos(mapping.rotation), std::sin(mapping.rotation));
}

/* Plain pointers rather than #gpu::StaticShader: that frees from a static destructor, after the GPU
 * context is gone. The image engine frees these while its context is still current. */
static struct {
  gpu::Shader *layer = nullptr;
  gpu::Shader *mask = nullptr;
  gpu::Shader *correction = nullptr;
  gpu::Shader *folder = nullptr;
  gpu::Shader *display = nullptr;
  gpu::Shader *shade = nullptr;
  bool created = false;
} gpu_shaders;

void combined_preview_gpu_free()
{
  GPU_SHADER_FREE_SAFE(gpu_shaders.layer);
  GPU_SHADER_FREE_SAFE(gpu_shaders.mask);
  GPU_SHADER_FREE_SAFE(gpu_shaders.correction);
  GPU_SHADER_FREE_SAFE(gpu_shaders.folder);
  GPU_SHADER_FREE_SAFE(gpu_shaders.display);
  GPU_SHADER_FREE_SAFE(gpu_shaders.shade);
  gpu_shaders.created = false;
}

bool combined_preview_gpu_pass_supported(const int pass)
{
  return pass == PAINT_LAYER_PASS_COMBINED ||
         std::find(combined_shading_channels.begin(),
                   combined_shading_channels.end(),
                   eMaterialPaintChannel(pass)) != combined_shading_channels.end();
}

bool combined_preview_gpu_update(const Material &ma,
                                 const CombinedPreviewLighting &lighting,
                                 const int pass,
                                 gpu::Texture *output,
                                 const float3x3 &texel_to_uv)
{
  if (output == nullptr) {
    return gpu_refuse("no display texture");
  }
  if (!BKE_paint_layers_is_layered(ma)) {
    return gpu_refuse("the material is not a layered stack");
  }
  if ((GPU_texture_usage(output) & GPU_TEXTURE_USAGE_SHADER_WRITE) == 0) {
    return gpu_refuse("the display texture has no SHADER_WRITE usage");
  }
  if (!combined_preview_gpu_pass_supported(pass)) {
    return gpu_refuse("the pass has no channel the GPU path lays");
  }
  bke::paint_perf::ScopedTimer perf_total("gpu_combined.total");
  /* A CPU shading of the whole canvas per check, so only every so often; of the Combined pass only,
   * the one the CPU preview can be compared with. */
  static int verify_calls = 0;
  gpu_verify_due = pass == PAINT_LAYER_PASS_COMBINED && gpu_verify_enabled() &&
                   (verify_calls++ % 120) == 0;
  /* The channel a channel pass shows, or -1 for the Combined preview, which lays them all. */
  int wanted_channel = -1;
  if (pass != PAINT_LAYER_PASS_COMBINED) {
    wanted_channel = int(std::find(combined_shading_channels.begin(),
                                   combined_shading_channels.end(),
                                   eMaterialPaintChannel(pass)) -
                         combined_shading_channels.begin());
  }

  constexpr int channels_num = int(combined_shading_channels.size());
  std::array<Vector<PaintMaterialCompositeImageLayer>, channels_num> stacks;
  std::array<bool, channels_num> has_stack;
  for (const int i : IndexRange(channels_num)) {
    if (wanted_channel >= 0 && i != wanted_channel) {
      has_stack[i] = false;
      continue;
    }
    has_stack[i] = BKE_paint_layers_composite_image_layers(
        ma, combined_shading_channels[i], stacks[i], /*describe_only=*/true);
    if (!has_stack[i]) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : stacks[i]) {
      if (const char *reason = gpu_layer_unsupported_reason(layer)) {
        return gpu_refuse(reason);
      }
    }
  }

  /* Created once, successful or not: a shader that fails to compile would otherwise be compiled
   * and logged again on every frame. */
  if (!gpu_shaders.created) {
    gpu_shaders.created = true;
    gpu_shaders.layer = GPU_shader_create_from_info_name("image_engine_combined_layer");
    gpu_shaders.mask = GPU_shader_create_from_info_name("image_engine_combined_mask");
    gpu_shaders.correction = GPU_shader_create_from_info_name("image_engine_combined_correction");
    gpu_shaders.folder = GPU_shader_create_from_info_name("image_engine_combined_folder");
    gpu_shaders.display = GPU_shader_create_from_info_name("image_engine_combined_display");
    gpu_shaders.shade = GPU_shader_create_from_info_name("image_engine_combined_shade");
  }
  gpu::Shader *layer_shader = gpu_shaders.layer;
  gpu::Shader *mask_shader = gpu_shaders.mask;
  gpu::Shader *correction_shader = gpu_shaders.correction;
  gpu::Shader *folder_shader = gpu_shaders.folder;
  gpu::Shader *display_shader = gpu_shaders.display;
  gpu::Shader *shade_shader = gpu_shaders.shade;
  if (layer_shader == nullptr || mask_shader == nullptr || correction_shader == nullptr ||
      folder_shader == nullptr || display_shader == nullptr || shade_shader == nullptr)
  {
    return gpu_refuse("a compute shader failed to compile");
  }

  const int2 size(GPU_texture_width(output), GPU_texture_height(output));
  const int2 groups = math::divide_ceil(size, int2(16));

  gpu::TexturePool &pool = gpu::TexturePool::get();
  /* Host read as well: the developer check reads the accumulators back. */
  const eGPUTextureUsage accum_usage = GPU_TEXTURE_USAGE_SHADER_READ |
                                       GPU_TEXTURE_USAGE_SHADER_WRITE |
                                       GPU_TEXTURE_USAGE_HOST_READ;
  std::array<gpu::Texture *, channels_num> accum;
  accum.fill(nullptr);
  gpu::Texture *dummy = pool.acquire_texture_2d(
      int2(1),
      1,
      gpu::TextureFormat::SFLOAT_16_16_16_16,
      eGPUTextureUsage(GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE),
      "combined_dummy");
  /* One factor, rebuilt for every row that has a mask. */
  gpu::Texture *mask_accum = nullptr;
  /* A placeholder for the mask pass's slot image: never written when no slot is asked for, but a
   * sampler and an image of one dispatch must not be the same texture. */
  gpu::Texture *slot_dummy = nullptr;
  /* The colour and coverage of a row while its corrections are laid over it. */
  gpu::Texture *row_color = nullptr;
  gpu::Texture *row_factor = nullptr;
  /* The pre-multiplied colour and coverage of the folders being laid, one pair per nesting level,
   * made when a folder first needs them. */
  std::array<gpu::Texture *, gpu_folder_levels_max> folder_premul;
  std::array<gpu::Texture *, gpu_folder_levels_max> folder_cover;
  folder_premul.fill(nullptr);
  folder_cover.fill(nullptr);
  /* Anchor Point slices captured before the channel passes, and the mask slices beside them. */
  Vector<GpuSliceSlot> slice_slots;
  Vector<GpuMaskSlot> mask_slots;
  BLI_SCOPED_DEFER([&]() {
    for (gpu::Texture *texture : accum) {
      if (texture != nullptr) {
        pool.release_texture(texture);
      }
    }
    if (mask_accum != nullptr) {
      pool.release_texture(mask_accum);
    }
    if (row_color != nullptr) {
      pool.release_texture(row_color);
    }
    if (row_factor != nullptr) {
      pool.release_texture(row_factor);
    }
    for (gpu::Texture *texture : folder_premul) {
      if (texture != nullptr) {
        pool.release_texture(texture);
      }
    }
    for (gpu::Texture *texture : folder_cover) {
      if (texture != nullptr) {
        pool.release_texture(texture);
      }
    }
    /* The captured slots release their own textures in their guards. */
    if (dummy != nullptr) {
      pool.release_texture(dummy);
    }
    if (slot_dummy != nullptr) {
      pool.release_texture(slot_dummy);
    }
  });
  mask_accum = pool.acquire_texture_2d(
      size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_mask");
  row_color = pool.acquire_texture_2d(
      size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_row_color");
  row_factor = pool.acquire_texture_2d(
      size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_row_factor");
  if (mask_accum == nullptr || row_color == nullptr || row_factor == nullptr) {
    return gpu_refuse("a row or mask texture could not be allocated");
  }
  for (gpu::Texture *&texture : accum) {
    texture = pool.acquire_texture_2d(
        size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_accum");
    if (texture == nullptr) {
      return gpu_refuse("an accumulator texture could not be allocated");
    }
  }
  if (dummy == nullptr) {
    return gpu_refuse("the placeholder texture could not be allocated");
  }
  slot_dummy = pool.acquire_texture_2d(
      int2(1),
      1,
      gpu::TextureFormat::SFLOAT_16_16_16_16,
      eGPUTextureUsage(GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE),
      "combined_slot_dummy");
  if (slot_dummy == nullptr) {
    return gpu_refuse("the placeholder slot texture could not be allocated");
  }

  /* An affine 2D map as the mat4 the push constants take: GLSL multiplies columns, so the
   * translation sits in the fourth one. */
  float4x4 texel_to_uv_4x4 = float4x4::identity();
  texel_to_uv_4x4[0] = float4(texel_to_uv[0].x, texel_to_uv[0].y, 0.0f, 0.0f);
  texel_to_uv_4x4[1] = float4(texel_to_uv[1].x, texel_to_uv[1].y, 0.0f, 0.0f);
  texel_to_uv_4x4[3] = float4(texel_to_uv[2].x, texel_to_uv[2].y, 0.0f, 1.0f);

  int dispatches = 0;
  /* The mip level to read a map at: the texels it has per screen pixel, as a power of two. */
  const auto map_lod = [&](gpu::Texture *map, const int flags, const float2 &mapping_scale) {
    if (map == nullptr || (flags & gpu_layer_alpha_coverage) != 0 ||
        (flags & gpu_layer_slot) != 0)
    {
      /* A painted map stores straight colour with black where it is transparent, and its mip
       * levels average that black in: a stroke edge seen from afar would come out several times
       * darker than the CPU composite reads it. The base level has no such bleed. A slice slot is
       * screen-space and has no mip levels at all. */
      return 0.0f;
    }
    const float2 map_size(GPU_texture_width(map), GPU_texture_height(map));
    float texels_per_pixel = std::max(
        math::length(float2(texel_to_uv[0].x, texel_to_uv[0].y) * map_size),
        math::length(float2(texel_to_uv[1].x, texel_to_uv[1].y) * map_size));
    if ((flags & gpu_layer_mapped) != 0) {
      /* A scaled mapping reads the map denser or sparser than the canvas does. */
      texels_per_pixel *= std::max(std::abs(mapping_scale.x), std::abs(mapping_scale.y));
    }
    return std::max(0.0f, std::log2(std::max(texels_per_pixel, 1e-6f)));
  };

  const auto lay = [&](const int channel_index,
                       gpu::Texture *map,
                       const int flags,
                       const float4 &layer_color,
                       const float4 &init_color,
                       const float opacity,
                       const float2 &mapping_offset = float2(0.0f),
                       const float2 &mapping_scale = float2(1.0f),
                       const float2 &mapping_rotation = float2(1.0f, 0.0f),
                       const int blend = int(CompositeBlend::Mix),
                       gpu::Texture *mask = nullptr,
                       const int filter_kind = 0,
                       const float *filter_params = nullptr,
                       gpu::Texture *slot_coverage = nullptr) {
    gpu::Texture *sampled = map != nullptr ? map : dummy;
    gpu::Texture *mask_sampled = mask != nullptr ? mask : dummy;
    gpu::Texture *slot_cov_sampled = slot_coverage != nullptr ? slot_coverage : dummy;
    const float lod = map_lod(map, flags, mapping_scale);
    GPU_shader_bind(layer_shader);
    GPU_shader_uniform_mat4(layer_shader, "texel_to_uv", texel_to_uv_4x4.ptr());
    GPU_shader_uniform_2iv(layer_shader, "texture_size", &size.x);
    GPU_shader_uniform_4fv(layer_shader, "layer_color", &layer_color.x);
    GPU_shader_uniform_4fv(layer_shader, "init_color", &init_color.x);
    GPU_shader_uniform_2fv(layer_shader, "mapping_offset", &mapping_offset.x);
    GPU_shader_uniform_2fv(layer_shader, "mapping_scale", &mapping_scale.x);
    GPU_shader_uniform_2fv(layer_shader, "mapping_rotation", &mapping_rotation.x);
    GPU_shader_uniform_1f(layer_shader, "opacity", opacity);
    GPU_shader_uniform_1f(layer_shader, "lod", lod);
    GPU_shader_uniform_1i(layer_shader, "flags", flags);
    GPU_shader_uniform_1i(layer_shader, "blend", blend);
    GPU_shader_uniform_1i(layer_shader, "filter_kind", filter_kind);
    const float4 adjustment_0 = filter_params != nullptr ?
                                    float4(filter_params[0], filter_params[1], filter_params[2], filter_params[3]) :
                                    float4(0.0f);
    const float4 adjustment_1 = filter_params != nullptr ?
                                    float4(filter_params[4], filter_params[5], filter_params[6], filter_params[7]) :
                                    float4(0.0f);
    GPU_shader_uniform_4fv(layer_shader, "filter_params_0", &adjustment_0.x);
    GPU_shader_uniform_4fv(layer_shader, "filter_params_1", &adjustment_1.x);

    /* The previous pass wrote this accumulator through an image. */
    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(layer_shader, "layer_tx"));
    GPU_texture_bind(mask_sampled, GPU_shader_get_sampler_binding(layer_shader, "mask_tx"));
    GPU_texture_bind(slot_cov_sampled,
                     GPU_shader_get_sampler_binding(layer_shader, "slot_cov_tx"));
    GPU_texture_image_bind(accum[channel_index],
                           GPU_shader_get_sampler_binding(layer_shader, "accum_img"));
    GPU_texture_image_bind(row_color, GPU_shader_get_sampler_binding(layer_shader, "row_color_img"));
    GPU_texture_image_bind(row_factor,
                           GPU_shader_get_sampler_binding(layer_shader, "row_factor_img"));
    GPU_compute_dispatch(layer_shader, uint(groups.x), uint(groups.y), 1);
    GPU_texture_image_unbind(row_factor);
    GPU_texture_image_unbind(row_color);
    GPU_texture_image_unbind(accum[channel_index]);
    GPU_texture_unbind(slot_cov_sampled);
    GPU_texture_unbind(mask_sampled);
    GPU_texture_unbind(sampled);
    dispatches++;
  };

  /* One step of a row's mask factor, into the mask accumulator. */
  const auto mask_step = [&](const int mode,
                             gpu::Texture *map,
                             const int flags,
                             const float constant_gray,
                             const float opacity,
                             const float influence,
                             const int blend,
                             const int filter,
                             const float *filter_params,
                             const PaintMaterialCompositeMapping *mapping,
                             gpu::Texture *slot_dst = nullptr,
                             gpu::Texture *mask_source = nullptr) {
    gpu::Texture *sampled = map != nullptr ? map : dummy;
    int step_flags = flags;
    float2 mapping_offset(0.0f);
    float2 mapping_scale(1.0f);
    float2 mapping_rotation(1.0f, 0.0f);
    if (mapping != nullptr && map != nullptr) {
      gpu_mapping_setup(*mapping, step_flags, mapping_offset, mapping_scale, mapping_rotation);
    }
    float4 params_0(0.0f);
    float4 params_1(0.0f);
    if (filter_params != nullptr) {
      params_0 = float4(filter_params[0], filter_params[1], filter_params[2], filter_params[3]);
      params_1 = float4(filter_params[4], filter_params[5], filter_params[6], filter_params[7]);
    }
    /* A mask map is painted like a layer's: its base level, for the same reason. */
    const float lod = 0.0f;
    GPU_shader_bind(mask_shader);
    GPU_shader_uniform_mat4(mask_shader, "texel_to_uv", texel_to_uv_4x4.ptr());
    GPU_shader_uniform_2iv(mask_shader, "texture_size", &size.x);
    GPU_shader_uniform_2fv(mask_shader, "mapping_offset", &mapping_offset.x);
    GPU_shader_uniform_2fv(mask_shader, "mapping_scale", &mapping_scale.x);
    GPU_shader_uniform_2fv(mask_shader, "mapping_rotation", &mapping_rotation.x);
    GPU_shader_uniform_4fv(mask_shader, "filter_params_0", &params_0.x);
    GPU_shader_uniform_4fv(mask_shader, "filter_params_1", &params_1.x);
    GPU_shader_uniform_1f(mask_shader, "constant_gray", constant_gray);
    GPU_shader_uniform_1f(mask_shader, "opacity", opacity);
    GPU_shader_uniform_1f(mask_shader, "influence", influence);
    GPU_shader_uniform_1f(mask_shader, "lod", lod);
    GPU_shader_uniform_1i(mask_shader, "flags", step_flags);
    GPU_shader_uniform_1i(mask_shader, "mode", mode);
    GPU_shader_uniform_1i(mask_shader, "blend", blend);
    GPU_shader_uniform_1i(mask_shader, "filter_kind", filter);

    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    gpu::Texture *mask_target = mask_source != nullptr ? mask_source : mask_accum;
    GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(mask_shader, "mask_tx"));
    GPU_texture_image_bind(mask_target, GPU_shader_get_sampler_binding(mask_shader, "mask_img"));
    GPU_texture_image_bind(slot_dst != nullptr ? slot_dst : slot_dummy,
                           GPU_shader_get_sampler_binding(mask_shader, "slot_img"));
    GPU_compute_dispatch(mask_shader, uint(groups.x), uint(groups.y), 1);
    GPU_texture_image_unbind(slot_dst != nullptr ? slot_dst : slot_dummy);
    GPU_texture_image_unbind(mask_target);
    GPU_texture_unbind(sampled);
    dispatches++;
  };

  /* One correction laid over the row's colour and coverage. False when its map has no GPU texture. */
  const auto correction_step = [&](const PaintMaterialCompositeCorrection &correction,
                                   const bool tracks_alpha,
                                   gpu::Texture *slot_color = nullptr,
                                   gpu::Texture *slot_coverage = nullptr) {
    gpu::Texture *map = nullptr;
    int step_flags = tracks_alpha ? gpu_layer_tracks_alpha : 0;
    if (slot_color != nullptr) {
      /* A live Anchor Point slice: screen-space, no mapping, coverage in the paired slot. */
      map = slot_color;
      step_flags |= gpu_layer_slot;
    }
    else if (correction.filter == 0 && correction.image != nullptr) {
      bool premultiplied = false;
      map = gpu_map_texture(correction.image, correction.iuser, premultiplied);
      if (map == nullptr) {
        return false;
      }
      step_flags |= gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
    }
    float2 mapping_offset(0.0f);
    float2 mapping_scale(1.0f);
    float2 mapping_rotation(1.0f, 0.0f);
    if (map != nullptr && slot_color == nullptr) {
      gpu_mapping_setup(correction.mapping, step_flags, mapping_offset, mapping_scale, mapping_rotation);
    }
    gpu::Texture *sampled = map != nullptr ? map : dummy;
    gpu::Texture *slot_sampled = slot_coverage != nullptr ? slot_coverage : dummy;
    const float4 constant_color(correction.constant_color);
    const float4 params_0(correction.filter_params[0],
                          correction.filter_params[1],
                          correction.filter_params[2],
                          correction.filter_params[3]);
    const float4 params_1(correction.filter_params[4],
                          correction.filter_params[5],
                          correction.filter_params[6],
                          correction.filter_params[7]);
    /* A correction map is painted like a layer's: its base level, for the same reason. */
    const float lod = 0.0f;
    GPU_shader_bind(correction_shader);
    GPU_shader_uniform_mat4(correction_shader, "texel_to_uv", texel_to_uv_4x4.ptr());
    GPU_shader_uniform_2iv(correction_shader, "texture_size", &size.x);
    GPU_shader_uniform_4fv(correction_shader, "constant_color", &constant_color.x);
    GPU_shader_uniform_2fv(correction_shader, "mapping_offset", &mapping_offset.x);
    GPU_shader_uniform_2fv(correction_shader, "mapping_scale", &mapping_scale.x);
    GPU_shader_uniform_2fv(correction_shader, "mapping_rotation", &mapping_rotation.x);
    GPU_shader_uniform_4fv(correction_shader, "filter_params_0", &params_0.x);
    GPU_shader_uniform_4fv(correction_shader, "filter_params_1", &params_1.x);
    GPU_shader_uniform_1f(correction_shader, "opacity", correction.opacity);
    GPU_shader_uniform_1f(correction_shader, "lod", lod);
    GPU_shader_uniform_1i(correction_shader, "flags", step_flags);
    GPU_shader_uniform_1i(correction_shader, "blend", int(correction.blend));
    GPU_shader_uniform_1i(correction_shader, "filter_kind", int(correction.filter));

    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    GPU_texture_bind(sampled, GPU_shader_get_sampler_binding(correction_shader, "correction_tx"));
    GPU_texture_bind(slot_sampled,
                     GPU_shader_get_sampler_binding(correction_shader, "slot_cov_tx"));
    GPU_texture_image_bind(row_color,
                           GPU_shader_get_sampler_binding(correction_shader, "row_color_img"));
    GPU_texture_image_bind(row_factor,
                           GPU_shader_get_sampler_binding(correction_shader, "row_factor_img"));
    GPU_compute_dispatch(correction_shader, uint(groups.x), uint(groups.y), 1);
    GPU_texture_image_unbind(row_factor);
    GPU_texture_image_unbind(row_color);
    GPU_texture_unbind(slot_sampled);
    GPU_texture_unbind(sampled);
    dispatches++;
    return true;
  };
  /* One step of a folder: seed its accumulators, lay a child over them, or resolve them into the
   * row textures as the folder's own row. */
  const auto folder_step = [&](const int mode,
                               const int level,
                               const int flags,
                               const float opacity,
                               const int blend,
                               gpu::Texture *mask) {
    gpu::Texture *mask_sampled = mask != nullptr ? mask : dummy;
    GPU_shader_bind(folder_shader);
    GPU_shader_uniform_mat4(folder_shader, "texel_to_uv", texel_to_uv_4x4.ptr());
    GPU_shader_uniform_2iv(folder_shader, "texture_size", &size.x);
    GPU_shader_uniform_1f(folder_shader, "opacity", opacity);
    GPU_shader_uniform_1i(folder_shader, "flags", flags);
    GPU_shader_uniform_1i(folder_shader, "blend", blend);
    GPU_shader_uniform_1i(folder_shader, "mode", mode);

    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    GPU_texture_bind(mask_sampled, GPU_shader_get_sampler_binding(folder_shader, "mask_tx"));
    GPU_texture_image_bind(row_color, GPU_shader_get_sampler_binding(folder_shader, "row_color_img"));
    GPU_texture_image_bind(row_factor,
                           GPU_shader_get_sampler_binding(folder_shader, "row_factor_img"));
    GPU_texture_image_bind(folder_premul[level],
                           GPU_shader_get_sampler_binding(folder_shader, "premul_img"));
    GPU_texture_image_bind(folder_cover[level],
                           GPU_shader_get_sampler_binding(folder_shader, "cover_img"));
    GPU_compute_dispatch(folder_shader, uint(groups.x), uint(groups.y), 1);
    GPU_texture_image_unbind(folder_cover[level]);
    GPU_texture_image_unbind(folder_premul[level]);
    GPU_texture_image_unbind(row_factor);
    GPU_texture_image_unbind(row_color);
    GPU_texture_unbind(mask_sampled);
    dispatches++;
  };

  /* The accumulators of the folders at nesting \a level, made when first needed. */
  const auto folder_levels_ensure = [&](const int level) {
    if (level < 0 || level >= gpu_folder_levels_max) {
      return false;
    }
    if (folder_premul[level] == nullptr) {
      folder_premul[level] = pool.acquire_texture_2d(
          size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_folder_premul");
    }
    if (folder_cover[level] == nullptr) {
      folder_cover[level] = pool.acquire_texture_2d(
          size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_folder_cover");
    }
    return folder_premul[level] != nullptr && folder_cover[level] != nullptr;
  };

  /* The slots captured before the passes. Linear: there are at most PAINT_LAYER_ANCHOR_POINT_LIMIT
   * points times the shaded channels, so a search is cheaper than a hash of pairs. */
  const auto slice_slot_find = [&](const bUUID &point, const int channel) -> GpuSliceSlot * {
    for (GpuSliceSlot &slot : slice_slots) {
      if (BKE_paint_layers_gpu_slot_key_equal({slot.point, slot.channel}, {point, channel})) {
        return &slot;
      }
    }
    return nullptr;
  };
  const auto mask_slot_find = [&](const bUUID &point) -> GpuMaskSlot * {
    for (GpuMaskSlot &slot : mask_slots) {
      if (BLI_uuid_equal(slot.point, point)) {
        return &slot;
      }
    }
    return nullptr;
  };

  /* Build the mask factor of \a layer in the mask accumulator: seeded with one, each item laid over
   * it, then the row's own mask map. False when a mask map has no GPU texture. */
  const auto mask_build = [&](const PaintMaterialCompositeImageLayer &layer, bool &r_has_mask) {
    r_has_mask = layer.mask_image != nullptr;
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      r_has_mask |= item.opacity > 0.0f;
    }
    if (!r_has_mask) {
      return true;
    }
    mask_step(gpu_mask_init, nullptr, 0, 1.0f, 1.0f, 1.0f, int(CompositeBlend::Mix), 0, nullptr, nullptr);
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      if (item.opacity <= 0.0f) {
        continue;
      }
      int item_flags = item.invert ? gpu_mask_invert : 0;
      if (item.map_coverage != 0) {
        /* The match accumulates over the passes; a missing or unreadable map leaves it at zero, the
         * full restriction the CPU composite gives. */
        mask_step(gpu_mask_cov_begin, nullptr, 0, 1.0f, 1.0f, 1.0f, int(CompositeBlend::Mix), 0, nullptr, nullptr);
        bool premultiplied = false;
        gpu::Texture *coverage_map = item.coverage_image != nullptr ?
                                         gpu_map_texture(
                                             item.coverage_image, item.coverage_iuser, premultiplied) :
                                         nullptr;
        if (coverage_map != nullptr) {
          const int map_flags = gpu_layer_has_map | gpu_map_nearest |
                                (premultiplied ? gpu_layer_premultiplied : 0);
          if (item.map_coverage == MA_PAINT_LAYER_FILTER_UV_ISLAND) {
            mask_step(gpu_mask_cov_scalar, coverage_map, map_flags, 1.0f, 1.0f, 1.0f,
                      int(CompositeBlend::Mix), 0, nullptr, &item.coverage_mapping);
          }
          else if (item.map_coverage == MA_PAINT_LAYER_FILTER_COLOR_ID) {
            const float shared_tol = std::max(0.0f, item.coverage_tolerance);
            const float shared_soft = std::max(0.0f, item.coverage_softness);
            for (const PaintMaterialCompositeIdColor &entry : item.coverage_colors) {
              if (!entry.enabled) {
                continue;
              }
              /* Advanced gives each colour its own tolerance and a hard edge. */
              const float params[8] = {entry.color[0],
                                       entry.color[1],
                                       entry.color[2],
                                       entry.own_tolerance ? std::max(0.0f, entry.tolerance) : shared_tol,
                                       entry.own_tolerance ? 0.0f : shared_soft,
                                       0.0f,
                                       0.0f,
                                       0.0f};
              mask_step(gpu_mask_cov_color, coverage_map, map_flags, 1.0f, 1.0f, 1.0f,
                        int(CompositeBlend::Mix), 0, params, &item.coverage_mapping);
            }
          }
        }
        mask_step(gpu_mask_cov_end, nullptr, item_flags, 1.0f, item.opacity, item.coverage_output,
                  int(CompositeBlend::Mix), 0, nullptr, nullptr);
        continue;
      }
      gpu::Texture *item_map = nullptr;
      const bool live_reader = !BLI_uuid_is_nil(item.anchor_point_reader) &&
                               item.image == nullptr && item.filter == 0;
      if (live_reader) {
        if (item.anchor_point_target == int8_t(PaintLayerAnchorPointTarget::Mask)) {
          GpuMaskSlot *slot = mask_slot_find(item.anchor_point_point);
          if (slot == nullptr) {
            return gpu_refuse("an Anchor Point mask slice has no captured slot");
          }
          item_map = slot->texture;
        }
        else {
          GpuSliceSlot *slot = slice_slot_find(item.anchor_point_point,
                                               item.anchor_point_read_channel);
          if (slot == nullptr) {
            return gpu_refuse("an Anchor Point Content slice has no captured slot");
          }
          /* A content slice read as a mask is its coverage, which the coverage slot holds. */
          item_map = slot->coverage;
        }
        item_flags |= gpu_mask_slot;
      }
      else if (item.filter == 0 && item.image != nullptr) {
        bool premultiplied = false;
        item_map = gpu_map_texture(item.image, item.iuser, premultiplied);
        if (item_map == nullptr) {
          return gpu_refuse("a mask map has no GPU texture");
        }
        item_flags |= gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
      }
      mask_step(gpu_mask_item,
                item_map,
                item_flags,
                item.constant_gray,
                item.opacity,
                1.0f,
                int(item.blend),
                int(item.filter),
                item.filter_params,
                live_reader ? nullptr : &item.mapping);
    }
    if (layer.mask_image != nullptr) {
      bool premultiplied = false;
      gpu::Texture *mask_map = gpu_map_texture(layer.mask_image, layer.mask_iuser, premultiplied);
      if (mask_map == nullptr) {
        return gpu_refuse("a row's mask map has no GPU texture");
      }
      int map_flags = gpu_layer_has_map | (premultiplied ? gpu_layer_premultiplied : 0);
      if (layer.mask_from_alpha) {
        map_flags |= gpu_mask_from_alpha;
      }
      mask_step(gpu_mask_map,
                mask_map,
                map_flags,
                1.0f,
                1.0f,
                layer.mask_influence,
                int(CompositeBlend::Mix),
                0,
                nullptr,
                nullptr);
    }
    return true;
  };

  /* Lay \a layer on channel \a i. \a level is -1 for a row of the stack, which blends onto the
   * channel's accumulator, and the nesting level of the folder holding it otherwise, whose
   * accumulators the row then goes into. A folder lays its children into accumulators of its own
   * first and then goes on as a row that reads them back like a map. Returns false when the GPU
   * path has to give up, with the reason already reported.
   *
   * \a capture_color and \a capture_coverage, when given, ask for the row's content before its own
   * opacity, mask and blend: the slice a reader of a point in this row lays. */
  std::function<bool(const PaintMaterialCompositeImageLayer &,
                     int,
                     int,
                     gpu::Texture *,
                     gpu::Texture *)>
      row_emit;
  row_emit = [&](const PaintMaterialCompositeImageLayer &layer,
                 const int i,
                 const int level,
                 gpu::Texture *capture_color,
                 gpu::Texture *capture_coverage) {
    if (!layer.enabled || layer.opacity <= 0.0f) {
      /* The composite skips it as well. */
      return true;
    }
    const bool in_folder = level >= 0;

    /* The corrections that change anything. */
    bool has_corrections = false;
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      if (correction.opacity <= 0.0f) {
        continue;
      }
      has_corrections |= correction.filter != 0 || correction.image != nullptr ||
                         correction.has_constant_color ||
                         !BLI_uuid_is_nil(correction.anchor_point_reader);
    }

    /* Where the row's colour and coverage are decided: the row textures for a folder, for a row in
     * a folder (its parent reads them there) and for a row with corrections; the map itself
     * otherwise. A capture reads the row textures even when nothing else would. */
    bool row_in_textures = layer.is_folder || in_folder || has_corrections ||
                           capture_color != nullptr;
    int flags = 0;
    gpu::Texture *map = nullptr;
    GpuSliceSlot *reader_slot = nullptr;
    if (!BLI_uuid_is_nil(layer.anchor_point_reader) && layer.color_image == nullptr) {
      /* A live reader row: its slice is in a captured slot. A baked reader has a colour map and
       * no slot. */
      /* Slots are keyed by the channel enum, which only equals the index in the shaded set for the
       * first channels. */
      reader_slot = slice_slot_find(layer.anchor_point_point, int(combined_shading_channels[i]));
      if (reader_slot == nullptr) {
        return gpu_refuse("an Anchor Point row slice has no captured slot");
      }
    }
    float4 layer_color(layer.constant_color);
    float2 mapping_offset(0.0f);
    float2 mapping_scale(1.0f);
    float2 mapping_rotation(1.0f, 0.0f);
    bool has_mask = false;

    if (layer.is_folder) {
      const int own_level = level + 1;
      if (!folder_levels_ensure(own_level)) {
        return gpu_refuse("folders are nested too deep for the GPU path");
      }
      folder_step(gpu_folder_init, own_level, 0, 1.0f, int(CompositeBlend::Mix), nullptr);
      for (const PaintMaterialCompositeImageLayer &child : layer.children) {
        if (!row_emit(child, i, own_level, nullptr, nullptr)) {
          return false;
        }
      }
      /* After the children: the mask accumulator is theirs until they are laid. */
      if (!mask_build(layer, has_mask)) {
        return false;
      }
      folder_step(gpu_folder_resolve, own_level, 0, 1.0f, int(CompositeBlend::Mix), nullptr);
    }
    else {
      if (!mask_build(layer, has_mask)) {
        return false;
      }
      /* A row covers by its own alpha when it says so and no mask map takes the alpha's place. */
      const bool own_alpha = layer.mask_image == nullptr && layer.mask_from_alpha;
      if (reader_slot != nullptr) {
        map = reader_slot->color;
        flags |= gpu_layer_slot;
      }
      else if (layer.color_image != nullptr) {
        bool premultiplied = false;
        map = gpu_map_texture(layer.color_image, layer.color_iuser, premultiplied);
        if (map == nullptr) {
          return gpu_refuse("a layer map has no GPU texture");
        }
        flags |= gpu_layer_has_map;
        if (layer.normal_directx) {
          flags |= gpu_layer_normal_directx;
        }
        if (premultiplied) {
          flags |= gpu_layer_premultiplied;
        }
        if (layer.color_alpha_coverage || own_alpha) {
          flags |= gpu_layer_alpha_coverage;
        }
      }
      else if (own_alpha) {
        /* A flat colour covers by its own alpha when the row says so. */
        flags |= gpu_layer_alpha_coverage;
      }
      if (layer.empty_base) {
        flags |= gpu_layer_empty_base;
      }
      if (layer.adjustment_filter != 0) {
        flags |= gpu_layer_adjustment;
      }
      if (map != nullptr && reader_slot == nullptr) {
        gpu_mapping_setup(layer.mapping, flags, mapping_offset, mapping_scale, mapping_rotation);
      }
      if (row_in_textures) {
        lay(i,
            map,
            flags | gpu_layer_row_store,
            layer_color,
            float4(0.0f),
            1.0f,
            mapping_offset,
            mapping_scale,
            mapping_rotation,
            int(CompositeBlend::Mix),
            nullptr,
            int(layer.adjustment_filter),
            layer.adjustment_params,
            reader_slot != nullptr ? reader_slot->coverage : nullptr);
      }
    }

    if (row_in_textures) {
      for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
        if (correction.opacity <= 0.0f) {
          continue;
        }
        const bool live_reader = !BLI_uuid_is_nil(correction.anchor_point_reader) &&
                                 correction.image == nullptr &&
                                 !correction.has_constant_color && correction.filter == 0;
        if (correction.filter == 0 && correction.image == nullptr &&
            !correction.has_constant_color && !live_reader)
        {
          continue;
        }
        gpu::Texture *slot_color = nullptr;
        gpu::Texture *slot_coverage = nullptr;
        if (live_reader) {
          GpuSliceSlot *slot = slice_slot_find(correction.anchor_point_point,
                                               int(combined_shading_channels[i]));
          if (slot == nullptr) {
            return gpu_refuse("an Anchor Point correction slice has no captured slot");
          }
          slot_color = slot->color;
          slot_coverage = slot->coverage;
        }
        if (!correction_step(correction, layer.tracks_content_alpha, slot_color, slot_coverage)) {
          return gpu_refuse("a correction map has no GPU texture");
        }
      }
    }

    if (capture_color != nullptr) {
      /* The row's content before its own opacity, mask and blend: what a reader of this point lays.
       * The row textures hold the corrections already, so only a straight copy is needed. */
      GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_UPDATE);
      GPU_texture_copy(capture_color, row_color);
      /* The coverage slot is grey in rgb and opaque in alpha; the mask store pass does that from
       * the row factor's `x`. */
      mask_step(gpu_mask_slot_store,
                nullptr,
                0,
                1.0f,
                1.0f,
                1.0f,
                int(CompositeBlend::Mix),
                0,
                nullptr,
                nullptr,
                capture_coverage,
                row_factor);
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
      return true;
    }

    const int mask_flags = has_mask ? gpu_layer_has_mask : 0;
    if (in_folder) {
      folder_step(gpu_folder_blend,
                  level,
                  mask_flags,
                  layer.opacity,
                  int(layer.blend),
                  has_mask ? mask_accum : nullptr);
      return true;
    }
    lay(i,
        row_in_textures ? nullptr : map,
        (row_in_textures ? gpu_layer_row_use : flags) | mask_flags,
        layer_color,
        float4(0.0f),
        layer.opacity,
        mapping_offset,
        mapping_scale,
        mapping_rotation,
        int(layer.blend),
        has_mask ? mask_accum : nullptr,
        int(layer.adjustment_filter),
        layer.adjustment_params,
        reader_slot != nullptr ? reader_slot->coverage : nullptr);
    return true;
  };

  /* -------------------------------------------------------------------- */
  /* Anchor Point slices: capture the producers before the channel passes. The plan is built in
   * BKE: it collects the readers nested in each described slice, dedups shared points, skips
   * readers on rows nothing lays, and orders the captures by point rank. */
  std::array<Span<const PaintMaterialCompositeImageLayer>, channels_num> stack_spans;
  std::array<int, channels_num> channel_enums;
  for (const int i : IndexRange(channels_num)) {
    stack_spans[i] = Span<const PaintMaterialCompositeImageLayer>(stacks[i].data(),
                                                                int64_t(stacks[i].size()));
    channel_enums[i] = int(combined_shading_channels[i]);
  }
  Vector<GpuCaptureJob> capture_jobs;
  const char *capture_reason = nullptr;
  const Span<const Span<const PaintMaterialCompositeImageLayer>> stack_view(
      stack_spans.data(), int64_t(stack_spans.size()));
  const Span<const int> channel_view(channel_enums.data(), int64_t(channel_enums.size()));
  if (!BKE_paint_layers_gpu_capture_plan(
          ma, stack_view, channel_view, capture_jobs, &capture_reason))
  {
    return gpu_refuse(capture_reason != nullptr ? capture_reason :
                                                 "the Anchor Point capture plan cannot be built");
  }
  for (const GpuCaptureJob &job : capture_jobs) {
    if (std::find(combined_shading_channels.begin(),
                  combined_shading_channels.end(),
                  eMaterialPaintChannel(job.channel)) == combined_shading_channels.end())
    {
      return gpu_refuse("an Anchor Point reads a channel the GPU path does not shade");
    }
  }

  if (!capture_jobs.is_empty()) {
    size_t textures = 0;
    for (const GpuCaptureJob &job : capture_jobs) {
      textures += job.kind == GpuCaptureKind::MaskItem ? 1 : 2;
    }
    if (textures * size_t(size.x) * size.y * 8 > gpu_slot_budget_bytes) {
      return gpu_refuse("the Anchor Point slots exceed the GPU budget");
    }
    /* The guard releases whatever a slot allocated, even when its second texture fails: the slot
     * does not have to be fully built to be cleaned up. */
    const auto release_texture = [](void *data, void *resource) {
      static_cast<gpu::TexturePool *>(data)->release_texture(static_cast<gpu::Texture *>(resource));
    };
    for (const GpuCaptureJob &job : capture_jobs) {
      if (job.kind == GpuCaptureKind::MaskItem) {
        GpuMaskSlot slot;
        slot.point = job.point;
        slot.reader = job.reader;
        slot.reader_count = job.reader_count;
        slot.guard.set_release(release_texture, &pool);
        slot.texture = pool.acquire_texture_2d(
            size, 1, gpu::TextureFormat::SFLOAT_16_16_16_16, accum_usage, "combined_anchor_mask");
        slot.guard.own(slot.texture);
        if (slot.texture == nullptr) {
          return gpu_refuse("an Anchor Point slot could not be allocated");
        }
        mask_slots.append(std::move(slot));
      }
      else {
        GpuSliceSlot slot;
        slot.point = job.point;
        slot.channel = job.channel;
        slot.reader = job.reader;
        slot.reader_count = job.reader_count;
        slot.guard.set_release(release_texture, &pool);
        slot.color = pool.acquire_texture_2d(size,
                                             1,
                                             gpu::TextureFormat::SFLOAT_16_16_16_16,
                                             accum_usage,
                                             "combined_anchor_slice");
        slot.guard.own(slot.color);
        slot.coverage = pool.acquire_texture_2d(size,
                                                1,
                                                gpu::TextureFormat::SFLOAT_16_16_16_16,
                                                accum_usage,
                                                "combined_anchor_coverage");
        slot.guard.own(slot.coverage);
        if (slot.color == nullptr || slot.coverage == nullptr) {
          return gpu_refuse("an Anchor Point slot could not be allocated");
        }
        slice_slots.append(std::move(slot));
      }
    }

    for (const GpuCaptureJob &job : capture_jobs) {
      const PaintMaterialCompositeImageLayer &entry = job.entry;
      if (job.kind == GpuCaptureKind::MaskItem) {
        bool has_mask = false;
        if (!mask_build(entry, has_mask)) {
          return false;
        }
        GpuMaskSlot *slot = mask_slot_find(job.point);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point mask slot is missing");
        }
        if (has_mask) {
          mask_step(gpu_mask_slot_store,
                    nullptr,
                    0,
                    1.0f,
                    1.0f,
                    1.0f,
                    int(CompositeBlend::Mix),
                    0,
                    nullptr,
                    nullptr,
                    slot->texture);
        }
        else {
          /* A slice of items that all lay nothing is a factor of one, as on the CPU. */
          const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
          GPU_texture_clear(slot->texture, GPU_DATA_FLOAT, white);
        }
        GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
      }
      else if (job.kind == GpuCaptureKind::MaskContent) {
        /* A mask point read as content: the CPU slice is the mask value as opaque grey with full
         * coverage. */
        bool has_mask = false;
        if (!mask_build(entry, has_mask)) {
          return false;
        }
        GpuSliceSlot *slot = slice_slot_find(job.point, job.channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point slot is missing");
        }
        if (has_mask) {
          mask_step(gpu_mask_slot_store,
                    nullptr,
                    0,
                    1.0f,
                    1.0f,
                    1.0f,
                    int(CompositeBlend::Mix),
                    0,
                    nullptr,
                    nullptr,
                    slot->color);
        }
        else {
          const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
          GPU_texture_clear(slot->color, GPU_DATA_FLOAT, white);
        }
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        GPU_texture_clear(slot->coverage, GPU_DATA_FLOAT, one);
        GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE | GPU_BARRIER_TEXTURE_FETCH);
      }
      else {
        GpuSliceSlot *slot = slice_slot_find(job.point, job.channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point slot is missing");
        }
        const int emit_index = std::max(
            0,
            int(std::find(combined_shading_channels.begin(),
                          combined_shading_channels.end(),
                          eMaterialPaintChannel(job.channel)) -
                combined_shading_channels.begin()));
        if (!row_emit(entry, emit_index, -1, slot->color, slot->coverage)) {
          return false;
        }
      }
    }
  }

  for (const int i : IndexRange(channels_num)) {
    if (wanted_channel >= 0 && i != wanted_channel) {
      continue;
    }
    const eMaterialPaintChannel channel = combined_shading_channels[i];
    float4 init_color = BKE_paint_material_combined_default_value(channel);
    if (has_stack[i]) {
      BKE_paint_layers_channel_bottom_color(channel, &init_color.x);
    }
    /* Seeds the accumulator: with no opacity the layer below changes nothing. */
    lay(i, nullptr, gpu_layer_init, float4(0.0f), init_color, 0.0f);
    if (!has_stack[i]) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : stacks[i]) {
      if (!row_emit(layer, i, -1, nullptr, nullptr)) {
        return false;
      }
    }
  }

  if (wanted_channel >= 0) {
    /* A channel pass shows the channel's accumulator as it is: the lighting is the Combined
     * preview's. */
    GPU_shader_bind(display_shader);
    GPU_shader_uniform_mat4(display_shader, "texel_to_uv", texel_to_uv_4x4.ptr());
    GPU_shader_uniform_2iv(display_shader, "texture_size", &size.x);
    GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    GPU_texture_bind(accum[wanted_channel],
                     GPU_shader_get_sampler_binding(display_shader, "channel_tx"));
    GPU_texture_image_bind(output, GPU_shader_get_sampler_binding(display_shader, "output_img"));
    GPU_compute_dispatch(display_shader, uint(groups.x), uint(groups.y), 1);
    GPU_texture_image_unbind(output);
    GPU_texture_unbind(accum[wanted_channel]);
    dispatches++;
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    if (bke::paint_perf::enabled()) {
      GPU_finish();
    }
    perf_total.note("pass=%d texture=%dx%d dispatches=%d", pass, size.x, size.y, dispatches);
    gpu_report_active();
    return true;
  }

  if (gpu_verify_due) {
    for (const int i : IndexRange(channels_num)) {
      float *pixels = static_cast<float *>(GPU_texture_read(accum[i], GPU_DATA_FLOAT, 0));
      if (pixels == nullptr) {
        gpu_verify_channels[i].clear();
        continue;
      }
      gpu_verify_channels[i].resize(int64_t(size.x) * size.y * 4);
      memcpy(gpu_verify_channels[i].data(),
             pixels,
             size_t(gpu_verify_channels[i].size()) * sizeof(float));
      MEM_delete(pixels);
    }

    /* The captured Anchor Point slots, against the CPU slice of the same reader. The slots are in
     * screen space, so a texel maps through the same affine as the passes. The comparison walks a
     * stride but also the four corners and the centre, and reports every slot, matched or not, so a
     * run of the developer check shows that it actually ran. */
    Material &ma_verify = const_cast<Material &>(ma);
    const auto report = [](const bUUID &point_marker,
                           const bUUID &reader_marker,
                           const int channel,
                           const int reader_count,
                           const char *label,
                           const char *reason,
                           const int64_t samples,
                           const PaintLayersGpuVerifyDiff &diff) {
      char point_uuid[64];
      char reader_uuid[64];
      BLI_uuid_format(point_uuid, point_marker);
      BLI_uuid_format(reader_uuid, reader_marker);
      if (reason != nullptr) {
        printf("[PBR-PERF] gpu_verify slot: point=%s reader=%s channel=%d readers=%d %s: "
               "skipped: %s\n",
               point_uuid,
               reader_uuid,
               channel,
               reader_count,
               label,
               reason);
        fflush(stdout);
        return;
      }
      const char *verdict = (samples <= 0) ? "NO SAMPLES" :
                            diff.has_nan   ? "MISMATCH (NaN)" :
                            diff.ok        ? "ok" :
                                             "MISMATCH";
      printf(
          "[PBR-PERF] gpu_verify slot: point=%s reader=%s channel=%d readers=%d %s samples=%lld "
          "mean=%.4f max=%.4f %s\n",
          point_uuid,
          reader_uuid,
          channel,
          reader_count,
          label,
          (long long)samples,
          diff.mean_diff,
          diff.max_diff,
          verdict);
      fflush(stdout);
    };

    /* Compare one captured screen-space texture with \a cpu_ibuf. The caller reads the GPU texture
     * once for both its colour and coverage halves; \a gray takes the CPU grey of an RGBA texel. */
    const auto compare_texture = [&](const Vector<float> &gpu_pixels,
                                     const ImBuf &cpu_ibuf,
                                     const bool gray,
                                     const bUUID &point_marker,
                                     const bUUID &reader_marker,
                                     const int channel,
                                     const int reader_count,
                                     const char *label) {
      if (cpu_ibuf.float_data() == nullptr || cpu_ibuf.x <= 0 || cpu_ibuf.y <= 0) {
        report(point_marker, reader_marker, channel, reader_count, label, "no CPU slice buffer", 0, {});
        return;
      }
      Vector<float> gpu_samples;
      Vector<float> cpu_samples;
      const int components = gray ? 1 : 3;
      const auto add_sample = [&](const int x, const int y) {
        if (x < 0 || y < 0 || x >= size.x || y >= size.y) {
          return;
        }
        const float2 uv = math::transform_point(texel_to_uv, float2(x + 0.5f, y + 0.5f));
        if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) {
          return;
        }
        const int cx = std::clamp(int(uv.x * cpu_ibuf.x), 0, cpu_ibuf.x - 1);
        const int cy = std::clamp(int(uv.y * cpu_ibuf.y), 0, cpu_ibuf.y - 1);
        const float *gpu = &gpu_pixels[(int64_t(y) * size.x + x) * 4];
        const float *cpu = cpu_ibuf.float_data() + (int64_t(cy) * cpu_ibuf.x + cx) * 4;
        if (gray) {
          gpu_samples.append(gpu[0]);
          cpu_samples.append((cpu[0] + cpu[1] + cpu[2]) / 3.0f);
        }
        else {
          for (const int k : IndexRange(3)) {
            gpu_samples.append(gpu[k]);
            cpu_samples.append(cpu[k]);
          }
        }
      };
      for (int y = 0; y < size.y; y += 8) {
        for (int x = 0; x < size.x; x += 8) {
          add_sample(x, y);
        }
      }
      /* The stride can miss a wrong corner or a wrong centre of a small map. */
      add_sample(0, 0);
      add_sample(size.x - 1, 0);
      add_sample(0, size.y - 1);
      add_sample(size.x - 1, size.y - 1);
      add_sample(size.x / 2, size.y / 2);

      const int64_t samples = components > 0 ? int64_t(gpu_samples.size()) / components : 0;
      const PaintLayersGpuVerifyDiff diff = BKE_paint_layers_gpu_verify_compare_slot(
          gpu_samples.data(), cpu_samples.data(), samples, components, 1e-2f);
      report(point_marker, reader_marker, channel, reader_count, label, nullptr, samples, diff);
    };

    const auto slot_check = [&](const bUUID &point_marker,
                                const bUUID &reader_marker,
                                const int channel,
                                const gpu::Texture *color_texture,
                                const gpu::Texture *coverage_texture,
                                const bool mask_slot,
                                const int reader_count) {
      MaterialPaintLayer *reader = BKE_paint_layers_find(ma_verify, reader_marker);
      if (reader == nullptr) {
        report(point_marker,
               reader_marker,
               channel,
               reader_count,
               "(color)",
               "reader not found",
               0,
               {});
        return;
      }
      if (color_texture == nullptr) {
        report(point_marker,
               reader_marker,
               channel,
               reader_count,
               "(color)",
               "no captured texture",
               0,
               {});
        return;
      }
      std::shared_ptr<ImBuf> color;
      std::shared_ptr<ImBuf> coverage;
      if (!BKE_paint_layers_slice_render(ma, *reader, channel, &color, &coverage)) {
        report(point_marker,
               reader_marker,
               channel,
               reader_count,
               "(color)",
               "the CPU slice cannot be rendered",
               0,
               {});
        return;
      }
      if (color == nullptr || color->float_data() == nullptr) {
        report(point_marker,
               reader_marker,
               channel,
               reader_count,
               "(color)",
               "the CPU slice has no float buffer",
               0,
               {});
        return;
      }

      /* One read of each captured texture; one CPU render for the whole slot. */
      Vector<float> gpu_color(int64_t(size.x) * size.y * 4);
      GPU_texture_read(
          const_cast<gpu::Texture *>(color_texture), GPU_DATA_FLOAT, 0, gpu_color.data());
      compare_texture(gpu_color,
                      *color,
                      mask_slot,
                      point_marker,
                      reader_marker,
                      channel,
                      reader_count,
                      mask_slot ? "(mask)" : "(color)");

      if (coverage_texture != nullptr) {
        if (coverage == nullptr || coverage->float_data() == nullptr) {
          report(point_marker,
                 reader_marker,
                 channel,
                 reader_count,
                 "(coverage)",
                 "the CPU slice has no coverage buffer",
                 0,
                 {});
          return;
        }
        if (coverage->x != color->x || coverage->y != color->y) {
          report(point_marker,
                 reader_marker,
                 channel,
                 reader_count,
                 "(coverage)",
                 "the CPU coverage has a different size",
                 0,
                 {});
          return;
        }
        Vector<float> gpu_coverage(int64_t(size.x) * size.y * 4);
        GPU_texture_read(
            const_cast<gpu::Texture *>(coverage_texture), GPU_DATA_FLOAT, 0, gpu_coverage.data());
        compare_texture(gpu_coverage,
                        *coverage,
                        true,
                        point_marker,
                        reader_marker,
                        channel,
                        reader_count,
                        "(coverage)");
      }
    };
    for (const GpuSliceSlot &slot : slice_slots) {
      slot_check(slot.point,
                 slot.reader,
                 slot.channel,
                 slot.color,
                 slot.coverage,
                 false,
                 slot.reader_count);
    }
    for (const GpuMaskSlot &slot : mask_slots) {
      slot_check(slot.point,
                 slot.reader,
                 int(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                 slot.texture,
                 nullptr,
                 true,
                 slot.reader_count);
    }
  }

  /* The lighting pass. Lights that cannot change a pixel are dropped, as on the CPU. */
  /* One light per column: the shader declares matrices because it has no push constant array. The
   * columns past `lights_num` are never read. */
  float4x4 light_direction = float4x4::identity();
  float4x4 light_diffuse = float4x4::identity();
  float4x4 light_specular = float4x4::identity();
  int lights_num = 0;
  for (const int i : IndexRange(std::clamp(lighting.lights_num, 0, int(lighting.lights.size())))) {
    const CombinedPreviewLight &light = lighting.lights[i];
    if (math::is_zero(light.diffuse_color) && math::is_zero(light.specular_color)) {
      continue;
    }
    light_direction[lights_num] = float4(light.direction, 0.0f);
    light_diffuse[lights_num] = float4(light.diffuse_color, 0.0f);
    light_specular[lights_num] = float4(light.specular_color, light.wrap);
    lights_num++;
  }
  const float4 ambient_color(lighting.ambient_color, 0.0f);
  const int normal_index = int(
      std::find(combined_shading_channels.begin(),
                combined_shading_channels.end(),
                PAINT_MATERIAL_CHANNEL_NORMAL) -
      combined_shading_channels.begin());
  static const char *sampler_names[channels_num] = {"base_tx",
                                                    "metallic_tx",
                                                    "roughness_tx",
                                                    "specular_tx",
                                                    "normal_tx",
                                                    "ao_tx",
                                                    "alpha_tx",
                                                    "emission_tx"};
  GPU_shader_bind(shade_shader);
  GPU_shader_uniform_mat4(shade_shader, "texel_to_uv", texel_to_uv_4x4.ptr());
  GPU_shader_uniform_2iv(shade_shader, "texture_size", &size.x);
  GPU_shader_uniform_mat4(shade_shader, "light_direction", light_direction.ptr());
  GPU_shader_uniform_mat4(shade_shader, "light_diffuse", light_diffuse.ptr());
  GPU_shader_uniform_mat4(shade_shader, "light_specular", light_specular.ptr());
  GPU_shader_uniform_4fv(shade_shader, "ambient_color", &ambient_color.x);
  GPU_shader_uniform_1i(shade_shader, "lights_num", lights_num);
  GPU_shader_uniform_1i(shade_shader, "normal_is_encoded", has_stack[normal_index] ? 1 : 0);
  GPU_shader_uniform_1f(shade_shader, "exposure", lighting.exposure);
  GPU_shader_uniform_1f(shade_shader, "ao_influence", lighting.ao_influence);
  GPU_shader_uniform_1f(shade_shader, "emission_strength", combined_emission_strength(ma));

  GPU_memory_barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
  for (const int i : IndexRange(channels_num)) {
    GPU_texture_bind(accum[i], GPU_shader_get_sampler_binding(shade_shader, sampler_names[i]));
  }
  GPU_texture_image_bind(output, GPU_shader_get_sampler_binding(shade_shader, "output_img"));
  GPU_compute_dispatch(shade_shader, uint(groups.x), uint(groups.y), 1);
  GPU_texture_image_unbind(output);
  for (const int i : IndexRange(channels_num)) {
    GPU_texture_unbind(accum[i]);
  }
  dispatches++;
  /* The image engine samples the result next. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);

  if (bke::paint_perf::enabled()) {
    /* Without a wait the timer would read only the time it took to record the commands. */
    GPU_finish();
  }
  perf_total.note("texture=%dx%d dispatches=%d", size.x, size.y, dispatches);
  gpu_report_active();
  return true;
}

void combined_preview_gpu_verify(Main &bmain,
                                 const Material &ma,
                                 const CombinedPreviewLighting &lighting,
                                 gpu::Texture *output,
                                 const float3x3 &texel_to_uv)
{
  /* Decided by the update that just shaded this frame, which also kept the channel accumulators. */
  if (!gpu_verify_due || output == nullptr) {
    return;
  }
  gpu_verify_due = false;

  const int2 size(GPU_texture_width(output), GPU_texture_height(output));
  Vector<float> gpu_pixels(int64_t(size.x) * size.y * 4);
  GPU_texture_read(output, GPU_DATA_FLOAT, 0, gpu_pixels.data());

  const ImBuf *cpu = combined_preview_ensure(bmain, ma, lighting);
  if (cpu == nullptr || cpu->float_data() == nullptr || cpu->x <= 0 || cpu->y <= 0) {
    printf("[PBR-PERF] gpu_verify: the CPU preview produced nothing to compare with\n");
    return;
  }

  /* Compared as block means. The CPU reads 8-bit composites at the nearest texel and the GPU the
   * maps with bilinear filtering at the base level, so in a detailed map -- a normal map above all
   * -- two single pixels can differ a lot without either being wrong. Averaged over a block both
   * sides measure the same thing, and what remains is a difference in the result. Relative above
   * one, where a highlight's absolute error means little. */
  constexpr int block = 8;
  int64_t blocks = 0;
  int64_t bad = 0;
  double sum = 0.0;
  float worst = 0.0f;
  float2 worst_uv(0.0f);
  int2 worst_texel(0);
  float3 worst_gpu(0.0f);
  float3 worst_cpu(0.0f);
  for (int by = 0; by + block <= size.y; by += block) {
    for (int bx = 0; bx + block <= size.x; bx += block) {
      float3 gpu_sum(0.0f);
      float3 cpu_sum(0.0f);
      int inside = 0;
      for (int y = by; y < by + block; y++) {
        for (int x = bx; x < bx + block; x++) {
          const float2 uv = math::transform_point(texel_to_uv, float2(x + 0.5f, y + 0.5f));
          if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) {
            continue;
          }
          const float *g = &gpu_pixels[(int64_t(y) * size.x + x) * 4];
          const float *c = cpu->float_data() + (int64_t(int(uv.y * cpu->y)) * cpu->x +
                                                int(uv.x * cpu->x)) * 4;
          gpu_sum += float3(g[0], g[1], g[2]);
          cpu_sum += float3(c[0], c[1], c[2]);
          inside++;
        }
      }
      /* A block cut by the canvas edge is left out: its mean mixes transparent texels in. */
      if (inside != block * block) {
        continue;
      }
      const float3 gpu_mean = gpu_sum / float(inside);
      const float3 cpu_mean = cpu_sum / float(inside);
      float diff = 0.0f;
      for (const int k : IndexRange(3)) {
        diff = std::max(diff,
                        std::abs(gpu_mean[k] - cpu_mean[k]) /
                            std::max(1.0f, std::max(gpu_mean[k], cpu_mean[k])));
      }
      blocks++;
      sum += diff;
      if (diff > 0.05f) {
        bad++;
      }
      if (diff > worst) {
        worst = diff;
        worst_texel = int2(bx + block / 2, by + block / 2);
        worst_uv = math::transform_point(
            texel_to_uv, float2(float(worst_texel.x) + 0.5f, float(worst_texel.y) + 0.5f));
        worst_gpu = gpu_mean;
        worst_cpu = cpu_mean;
      }
    }
  }
  if (blocks == 0) {
    return;
  }
  printf(
      "[PBR-PERF] gpu_verify: blocks=%lld (%dx%d) mean=%.4f max=%.4f over 0.05=%.2f%% worst at "
      "uv=(%.3f, %.3f) gpu=(%.3f, %.3f, %.3f) cpu=(%.3f, %.3f, %.3f)\n",
      long long(blocks),
      block,
      block,
      sum / double(blocks),
      worst,
      100.0 * double(bad) / double(blocks),
      worst_uv.x,
      worst_uv.y,
      worst_gpu.x,
      worst_gpu.y,
      worst_gpu.z,
      worst_cpu.x,
      worst_cpu.y,
      worst_cpu.z);  /* Where the difference comes from: each channel at the centre of the worst block, the accumulator the GPU laid
   * against the CPU composite of the same channel. A channel that reads alike on both sides and a
   * pixel that still differs points at the lighting; one that differs points at its stack. */
  for (const int i : IndexRange(int(combined_shading_channels.size()))) {
    const eMaterialPaintChannel channel = combined_shading_channels[i];
    float4 gpu_value(0.0f);
    if (!gpu_verify_channels[i].is_empty()) {
      const float *p = &gpu_verify_channels[i][(int64_t(worst_texel.y) * size.x + worst_texel.x) *
                                               4];
      gpu_value = float4(p[0], p[1], p[2], p[3]);
    }
    bool cpu_known = false;
    float4 cpu_value(0.0f);
    Vector<PaintMaterialCompositeImageLayer> layers;
    if (BKE_paint_layers_composite_image_layers(ma, channel, layers)) {
      const ImBuf *composite = BKE_paint_material_composite_cache_ensure(
          ma,
          channel,
          layers,
          BKE_paint_material_composite_stack_hash(layers),
          nullptr,
          nullptr,
          nullptr);
      if (composite != nullptr && composite->byte_data() != nullptr && composite->x > 0) {
        const uchar *p = composite->byte_data() +
                         (int64_t(int(worst_uv.y * composite->y)) * composite->x +
                          int(worst_uv.x * composite->x)) *
                             4;
        cpu_value = float4(float(p[0]), float(p[1]), float(p[2]), float(p[3])) * (1.0f / 255.0f);
        if (combined_channel_is_color(channel) && combined_ibuf_is_srgb(*composite)) {
          cpu_value.x = srgb_to_linearrgb(cpu_value.x);
          cpu_value.y = srgb_to_linearrgb(cpu_value.y);
          cpu_value.z = srgb_to_linearrgb(cpu_value.z);
        }
        cpu_known = true;
      }
    }
    if (cpu_known) {
      printf("[PBR-PERF]   channel %d: gpu=(%.3f, %.3f, %.3f, %.3f) cpu=(%.3f, %.3f, %.3f, %.3f)\n",
             int(channel),
             gpu_value.x,
             gpu_value.y,
             gpu_value.z,
             gpu_value.w,
             cpu_value.x,
             cpu_value.y,
             cpu_value.z,
             cpu_value.w);
    }
    else {
      printf("[PBR-PERF]   channel %d: gpu=(%.3f, %.3f, %.3f, %.3f) cpu=no stack\n",
             int(channel),
             gpu_value.x,
             gpu_value.y,
             gpu_value.z,
             gpu_value.w);
    }
  }  fflush(stdout);
}

/** \} */

}  // namespace blender::ed::material_combined
