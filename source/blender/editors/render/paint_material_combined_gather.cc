/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 */

#include "ED_material_combined.hh"

#include <algorithm>
#include <array>
#include <memory>

#include "BKE_image.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_material_channel_perf_debug.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "ED_material_bake.hh"

/**
 * Diagnostic tracing of the gather, one line per channel, so a single run shows which source every
 * channel resolved to and why. Set to 1 for a debugging session, then back to 0.
 *
 * A switch of its own rather than #PBR_PAINT_BAKE_DEBUG, for the same reason
 * #PBR_MATERIAL_BAKE_DEBUG has one: `editors/sculpt_paint/mesh/paint_debug.hh` belongs to another
 * module and this file cannot reach it.
 */
#define PBR_COMBINED_GATHER_DEBUG 1
#if PBR_COMBINED_GATHER_DEBUG
#  include <cstdio>
#  define PBR_COMBINED_LOG(...) \
    do { \
      printf("[PBR-COMBINED] " __VA_ARGS__); \
      fflush(stdout); \
    } while (0)
#else
#  define PBR_COMBINED_LOG(...) ((void)0)
#endif

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
                               uint64_t *r_revision,
                               CombinedEvalStats *r_stats)
{
  PAINT_CHANNEL_PERF_COMBINED_SCOPE(Gather);
  if (r_revision != nullptr) {
    *r_revision = 0;
  }
  GatherScope scope;

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&ma);

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
  for (const int i : IndexRange(int64_t(combined_shading_channels.size()))) {
    has_stack[i] = BKE_paint_material_composite_stack_from_material(
        bmain, ma, combined_shading_channels[i], stacks[i]);
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
#if PBR_COMBINED_GATHER_DEBUG
  PBR_COMBINED_LOG("gather: mat='%s' canvas=%dx%d emission_strength=%.3f\n",
                   ma.id.name + 2,
                   width,
                   height,
                   combined_emission_strength(ma));
  for (const int channel_index : IndexRange(int64_t(combined_shading_channels.size()))) {
    const eMaterialPaintChannel channel = combined_shading_channels[channel_index];
    /* Reported from the single derivation above rather than re-walked, so enabling this switch
     * does not itself change what it is measuring. */
    PBR_COMBINED_LOG("  ch=%d resolve=%d reason=%d stack=%d layers=%d image=%s\n",
                     int(channel),
                     int(resolve.channels[int(channel)]),
                     int(resolve.reasons[int(channel)]),
                     int(has_stack[channel_index]),
                     int(stacks[channel_index].size()),
                     resolve.images[int(channel)].image != nullptr ?
                         resolve.images[int(channel)].image->id.name + 2 :
                         "-");
  }
#endif

  if (width <= 0 || height <= 0) {
    PBR_COMBINED_LOG("gather: no canvas size resolved, falling back to the plain image\n");
    return nullptr;
  }

  CombinedInputs inputs;
  inputs.width = width;
  inputs.height = height;
  inputs.emission_strength = combined_emission_strength(ma);
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    inputs.channels[i].constant = BKE_paint_material_combined_default_value(
        eMaterialPaintChannel(i));
  }

  Vector<uint32_t> dependency_image_uids;
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
        PAINT_CHANNEL_PERF_COMBINED_SCOPE(CompositeRefresh);
        composite = BKE_paint_material_composite_cache_ensure(
            ma, channel, layers, stack_hash, nullptr, nullptr, &channel_changed);
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
          dependency_image_uids.append_non_duplicates(image.id.session_uid);
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

    PBR_COMBINED_LOG("  ch=%d source=%d ibuf=%p %dx%d srgb=%d const=(%.3f %.3f %.3f %.3f)\n",
                     int(channel),
                     int(source),
                     (const void *)input.ibuf,
                     input.ibuf != nullptr ? input.ibuf->x : 0,
                     input.ibuf != nullptr ? input.ibuf->y : 0,
                     int(input.is_srgb),
                     input.constant.x,
                     input.constant.y,
                     input.constant.z,
                     input.constant.w);
  }

  /* Collected unconditionally, because the perf harness reports the evaluator separately from
   * everything else and the caller usually has no interest in the numbers. */
  CombinedEvalStats stats;
  ImBuf *result = BKE_paint_material_combined_cache_ensure(ma,
                                                           inputs,
                                                           lighting,
                                                           inputs_hash,
                                                           changed_region,
                                                           dependency_image_uids,
                                                           r_revision,
                                                           &stats);
  PAINT_CHANNEL_PERF_COMBINED_SET_EVAL(
      stats.elapsed_seconds, stats.pixels_processed, width, height);
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

}  // namespace blender::ed::material_combined
