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
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_image.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_utils.hh"
#include "BLI_rect.h"
#include "BLI_time.h"
#include "BLI_timer.h"
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
#include "ED_material_combined.hh"
#include "paint_material_combined_effects.hh"

#include "WM_api.hh"

namespace blender::ed::material_combined {

/* filter_params_0/1/2 pack 3 float4s (12 floats) for shader uniforms; wide enough for all
 * filter payloads including a Color Balance's 3 folded triples. */
static_assert(PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM >= 3 * 4);

/* -------------------------------------------------------------------- */
/** \name Interactive Gradient Drag
 * \{ */

/**
 * The materials whose Gradient handles are being dragged, with the moment each drag was last seen
 * alive.
 *
 * A timestamp rather than a bare mark, because nothing in this file can observe a drag ending: the
 * gizmo is destroyed, the operator is cancelled, the file is closed, and the clear that should have
 * come with it never runs. Expiry turns that from a permanent loss of resolution into a delay.
 */
static Map<uint32_t, double> &gradient_interactive_marks()
{
  static Map<uint32_t, double> marks;
  return marks;
}

/** How long a mark survives without a fresh #gradient_interactive_begin. Longer than any drag a
 * user completes; short enough that a stuck one is not left to be noticed as a bug. */
static constexpr double GRADIENT_INTERACTIVE_TIMEOUT_SECONDS = 60.0;

/** The times the Gradient preview was last rebuilt, declared here for the release path below. */
static Map<uint32_t, double> &gradient_preview_last_refresh();

void gradient_interactive_begin(const Material &ma)
{
  /* Insert or move the mark forward: a drag re-arms its own stamp on every step. */
  gradient_interactive_marks().add_overwrite(ma.id.session_uid, BLI_time_now_seconds());
}

void gradient_interactive_end(const uint32_t material_session_uid)
{
  gradient_interactive_marks().remove(material_session_uid);
  /* Release always refines to the canvas, and the next drag starts with an immediate first frame. */
  gradient_preview_last_refresh().remove(material_session_uid);
}

bool gradient_interactive_is(const Material &ma)
{
  const double *started = gradient_interactive_marks().lookup_ptr(ma.id.session_uid);
  if (started == nullptr) {
    return false;
  }
  if (BLI_time_now_seconds() - *started > GRADIENT_INTERACTIVE_TIMEOUT_SECONDS) {
    gradient_interactive_marks().remove(ma.id.session_uid);
    return false;
  }
  return true;
}

/**
 * When each material's Gradient preview last rebuilt, by #ID.session_uid.
 *
 * Kept beside the drag mark rather than on the material: it is display state, it must not survive a
 * file load, and the mark it belongs to is cleared on release here as well.
 */
static Map<uint32_t, double> &gradient_preview_last_refresh()
{
  static Map<uint32_t, double> times;
  return times;
}

bool gradient_preview_refresh_due(const double now,
                                  const double last_refresh,
                                  const bool dragging)
{
  if (!dragging) {
    /* Released: the next frame is the one that refines the octave to the canvas. */
    return true;
  }
  if (last_refresh <= 0.0) {
    return true;
  }
  return now - last_refresh >= GRADIENT_PREVIEW_REFRESH_SECONDS;
}

static uintptr_t gradient_preview_trailing_timer_uuid()
{
  static const char id = 0;
  return uintptr_t(&id);
}

bool gradient_preview_refresh_allow(const Material &ma, const double now)
{
  if (!gradient_interactive_is(ma)) {
    return true;
  }
  const double *last = gradient_preview_last_refresh().lookup_ptr(ma.id.session_uid);
  const double last_refresh = (last != nullptr) ? *last : 0.0;
  if (gradient_preview_refresh_due(now, last_refresh, true)) {
    return true;
  }
  /* The mouse has stopped and no event will arrive to ask for the frame the drag still owes: arm
   * the trailing edge of the throttle here, where the decision to skip was made. A timer already
   * running is due no earlier than this one would be, since the last refresh time has not moved. */
  if (!BLI_timer_is_registered(gradient_preview_trailing_timer_uuid())) {
    const double delay = std::max(last_refresh + GRADIENT_PREVIEW_REFRESH_SECONDS - now, 0.0);
    BLI_timer_register(
        gradient_preview_trailing_timer_uuid(),
        [](uintptr_t /*uuid*/, void * /*user_data*/) -> double {
          /* The material is deliberately not resolved: the notifier is the same generic one every
           * stack edit sends, and each Image Editor decides from its own shown material whether the
           * preview is still due. A material freed since the arm cannot be reached this way. */
          WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
          return -1.0;
        },
        nullptr,
        nullptr,
        delay,
        false);
  }
  return false;
}

void gradient_preview_refresh_note(const Material &ma, const double now)
{
  gradient_preview_last_refresh().add_overwrite(ma.id.session_uid, now);
}

/** \} */

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
  /* Descriptor-driven rather than a channel list, so the answer stays right for every color
   * channel. The Combined preview only gathers #combined_shading_channels today (Specular Tint
   * has no shading path there yet); when that set grows this follows it for free. */
  return BKE_paint_material_channel_info(channel).is_color;
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
 * Applied to a bake and to a directly linked image. A bake is produced at its own square
 * resolution, is always rebuilt whole, and never reports a dirty rectangle. A directly linked image
 * does report them, but the cache polls it in canvas texels and maps them to its own buffer, so
 * resampling the input here does not touch that path. Resampling a layer stack is different: its
 * dirty rectangles arrive in the composites' own texels, which is how stale pixels get left on
 * screen when two coordinate systems are mixed up.
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
                               CombinedEvalStats *r_stats,
                               std::shared_ptr<const PaintMaterialCompositeSurface> surface)
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
  /* A row's Compare Height mask gates every channel of the row and is the same picture in each,
   * so one pass renders it once: two canvas-sized evaluations per Compare row per channel
   * otherwise. Good for this gather only -- the description can change between them. */
  PaintLayersHeightWonCache height_won_cache;
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
                             ma,
                             combined_shading_channels[i],
                             stacks[i],
                             /*describe_only=*/true,
                             surface,
                             &height_won_cache) :
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

  /* An interactive Gradient drag of this material shades its channel composites at a display
   * octave, and the preview follows them at the same resolution; without a drag every composite is
   * read and fed on at the canvas. */
  const bool gradient_interactive = BKE_paint_layers_is_layered(ma) && gradient_interactive_is(ma);

  /* The resolution the channels are gathered at, and the one the preview is shaded at.
   *
   * Both are the canvas unless something reduces them, and both are asked of
   * #BKE_paint_material_preview_octave_size_for, so that the rule -- powers of two, a margin, and
   * the hysteresis of the size already in use -- lives in one place and every consumer of the same
   * preview arrives at the same answer. A drag reduces the inputs: the Gradient is what is expensive
   * and every channel composite is laid again on each mouse move, so the octave has to reach the
   * composites themselves. The output is then that octave one texel to one, since reducing it
   * further would compound two reductions for a display that is already satisfied. Without a drag
   * the inputs are the canvas and only the output may be a fraction of it.
   *
   * The previous sizes come from the cache entry, which is the one place they are remembered: a
   * choice derived from the zoom alone makes the step down and the step up share a threshold, so a
   * zoom resting on it alternates between two octaves and rebuilds everything every frame. */
  const int2 canvas_size(width, height);
  int2 previous_input = int2(0);
  int2 previous_output = int2(0);
  BKE_paint_material_combined_cache_sizes_get(ma, previous_input, previous_output);
  const int2 input_size = gradient_interactive ?
                              BKE_paint_material_preview_octave_size_for(
                                  canvas_size, request.max_output, previous_input) :
                              canvas_size;
  const int2 output_size = gradient_interactive ?
                               input_size :
                               BKE_paint_material_preview_octave_size_for(
                                   canvas_size, request.max_output, previous_output);

  CombinedInputs inputs;
  inputs.width = input_size.x;
  inputs.height = input_size.y;
  inputs.output_width = output_size.x == input_size.x ? 0 : output_size.x;
  inputs.output_height = output_size.y == input_size.y ? 0 : output_size.y;
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
          /* The same surface the description for the hash was made with. Without it the layers the
           * cache evaluates are the approximate Object/Generated ones -- `(u, v, 0)` instead of the
           * texel's object-space point -- while the hash that decided they were current carried
           * the exact one: approximate pixels behind an exact identity, and no edit of the surface
           * would ever invalidate them. */
          return BKE_paint_layers_composite_image_layers(
              ma, channel, r_full, /*describe_only=*/false, surface, &height_won_cache);
        };
        /* The octave the gather decided for every channel, or zero for the canvas. Passed as a size
         * rather than as a display cap: the hysteresis is the preview's, not each channel's, or two
         * channels of one material could settle on two octaves. */
        const int2 stack_output_size = gradient_interactive ? input_size : int2(0);
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
                FunctionRef<bool(Vector<PaintMaterialCompositeImageLayer> &)>(),
            stack_output_size);
        perf_channel.note("channel=%d layers=%d changed=%dx%d out=%dx%d",
                          int(channel),
                          int(layers.size()),
                          BLI_rcti_size_x(&channel_changed),
                          BLI_rcti_size_y(&channel_changed),
                          composite != nullptr ? composite->x : 0,
                          composite != nullptr ? composite->y : 0);
      }
      /* One size for every channel, decided above: a composite that disagrees is one whose own
       * canvas is not this one, and it falls to its constant exactly as it does when its size
       * disagrees at full resolution. */
      if (composite != nullptr && composite->x == input_size.x && composite->y == input_size.y) {
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
        /* Accepted at the canvas size as well as the gathered size: during a Gradient drag the
         * gathered size is an octave, and a canvas-size image must not fall to a constant just
         * because the stack is smaller. */
        const bool matches_canvas = ibuf->x == canvas_size.x && ibuf->y == canvas_size.y;
        const bool matches_input = ibuf->x == input_size.x && ibuf->y == input_size.y;
        if (matches_canvas || matches_input) {
          const ImBuf *usable = ibuf;
          if (!matches_input) {
            usable = scope.own(combined_resample_nearest(*ibuf, input_size.x, input_size.y));
          }
          if (usable != nullptr) {
            input.ibuf = usable;
            input.is_srgb = combined_channel_is_color(channel) && combined_ibuf_is_srgb(*ibuf);
            source = CombinedChannelSource::DirectImage;
            source_detail = image.id.session_uid;
            dependency_images.append_non_duplicates(&image);
          }
        }
      }
    }

    /* 3. Cached bake, nearest-sampled to the gathered size. */
    const material_bake::MaterialSourceBake *bake = scope.bake();
    if (source == CombinedChannelSource::Default && bake != nullptr &&
        bake->resolution(channel) == ChannelResolution::Baked)
    {
      if (const ImBuf *baked = bake->channel_image(channel)) {
        const ImBuf *usable = baked;
        if (baked->x != input_size.x || baked->y != input_size.y) {
          usable = scope.own(combined_resample_nearest(*baked, input_size.x, input_size.y));
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

  /* The canvas the stack pixels belong to, for the cache's own canvas report: on every
   * non-octave path it *is* the inputs, but a report must never answer the octave. */
  inputs.canvas_width = width;
  inputs.canvas_height = height;

  /* Two origins, two scalings, both rounded outwards so a dab that lost its edge to rounding does
   * not leave a stale fringe.
   *
   * The clip arrives in canvas texels, so it takes two scalings: canvas -> input, then input ->
   * output. The changed region arrives from the composites already in input texels (the octave
   * while a drag lasts), so it takes only the input -> output scaling. Both are identities on a
   * frame that reduces nothing, which is what every caller but the viewport is. */
  CombinedCacheRequest cache_request;
  cache_request.inputs_hash = inputs_hash;
  cache_request.dependency_images = dependency_images;
  cache_request.changed_region = BKE_paint_material_preview_rect_scale(
      changed_region, input_size, output_size);
  cache_request.clip = BKE_paint_material_preview_rect_scale(
      request.clip, canvas_size, input_size);
  cache_request.clip = BKE_paint_material_preview_rect_scale(
      cache_request.clip, input_size, output_size);

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
  perf_shade.note("canvas=%dx%d in=%dx%d out=%dx%d eval=%.3f ms pixels=%lld dep_images=%d",
                  canvas_size.x,
                  canvas_size.y,
                  input_size.x,
                  input_size.y,
                  output_size.x,
                  output_size.y,
                  stats.elapsed_seconds * 1000.0,
                  static_cast<long long>(stats.pixels_processed),
                  int(dependency_images.size()));
  perf_total.note("canvas=%dx%d in=%dx%d out=%dx%d",
                  canvas_size.x,
                  canvas_size.y,
                  input_size.x,
                  input_size.y,
                  output_size.x,
                  output_size.y);
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

/* The pass kinds the plan builds and the shader's COMBINED_PASS_* defines mirror one to one; a
 * renumber on one side fails the build here instead of dispatching the wrong kernel. The flags and
 * the step constants the passes push live in #paint_material_combined_effects.hh, beside the passes
 * themselves. */
static_assert(int(ePaintLayerEffectPass::Noise) == 0, "COMBINED_PASS_NOISE");
static_assert(int(ePaintLayerEffectPass::Luminance) == 1, "COMBINED_PASS_LUMINANCE");
static_assert(int(ePaintLayerEffectPass::BlurH) == 2, "COMBINED_PASS_BLUR_H");
static_assert(int(ePaintLayerEffectPass::BlurV) == 3, "COMBINED_PASS_BLUR_V");
static_assert(int(ePaintLayerEffectPass::ExtremumH) == 4, "COMBINED_PASS_EXTREMUM_H");
static_assert(int(ePaintLayerEffectPass::ExtremumV) == 5, "COMBINED_PASS_EXTREMUM_V");
static_assert(int(ePaintLayerEffectPass::Edge3x3) == 6, "COMBINED_PASS_EDGE_3X3");
static_assert(int(ePaintLayerEffectPass::Emboss3x3) == 7, "COMBINED_PASS_EMBOSS_3X3");
static_assert(int(ePaintLayerEffectPass::UnsharpTail) == 8, "COMBINED_PASS_UNSHARP_TAIL");
static_assert(int(ePaintLayerEffectPass::HighPassTail) == 9, "COMBINED_PASS_HIGH_PASS_TAIL");

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

bool gpu_refuse(const char *reason)
{
  if (bke::paint_perf::enabled() && reason != gpu_last_reason) {
    printf("[PBR-PERF] gpu_combined refused: %s\n", reason);
    fflush(stdout);
  }
  gpu_last_reason = reason;
  return false;
}

/** Report, once after a refusal or at the first frame, that the GPU path is shading the preview. */
void gpu_report_active()
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

/* Plain pointers rather than #gpu::StaticShader: that frees from a static destructor, after the GPU
 * context is gone. The image engine frees these while its context is still current. */
static struct {
  gpu::Shader *layer = nullptr;
  gpu::Shader *mask = nullptr;
  gpu::Shader *correction = nullptr;
  gpu::Shader *effect = nullptr;
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
  GPU_SHADER_FREE_SAFE(gpu_shaders.effect);
  GPU_SHADER_FREE_SAFE(gpu_shaders.folder);
  GPU_SHADER_FREE_SAFE(gpu_shaders.display);
  GPU_SHADER_FREE_SAFE(gpu_shaders.shade);
  gpu_shaders.created = false;
  /* The GPU effect cache owns textures of its own, which outlive one frame; they have to go while
   * this engine's context is still current, the same window the shaders above free in. The cache
   * drops each texture with the hook that made it, so it needs no GPU type to do so.
   */
  BKE_paint_layers_effect_gpu_cache_free_all();
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
   * the one the CPU preview can be compared with. Offset 60, not 0: the first calls are a cold
   * frame at the workspace switch, before the caches have warmed, and verifying there would compare
   * a preview that is not what the settled display shows. */
  static int verify_calls = 0;
  gpu_verify_due = pass == PAINT_LAYER_PASS_COMBINED && gpu_verify_enabled() &&
                   (verify_calls++ % 120) == 60;
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
    gpu_shaders.effect = GPU_shader_create_from_info_name("image_engine_combined_effect");
    gpu_shaders.folder = GPU_shader_create_from_info_name("image_engine_combined_folder");
    gpu_shaders.display = GPU_shader_create_from_info_name("image_engine_combined_display");
    gpu_shaders.shade = GPU_shader_create_from_info_name("image_engine_combined_shade");
  }
  gpu::Shader *layer_shader = gpu_shaders.layer;
  gpu::Shader *mask_shader = gpu_shaders.mask;
  gpu::Shader *correction_shader = gpu_shaders.correction;
  gpu::Shader *effect_shader = gpu_shaders.effect;
  gpu::Shader *folder_shader = gpu_shaders.folder;
  gpu::Shader *display_shader = gpu_shaders.display;
  gpu::Shader *shade_shader = gpu_shaders.shade;
  if (layer_shader == nullptr || mask_shader == nullptr || correction_shader == nullptr ||
      effect_shader == nullptr || folder_shader == nullptr || display_shader == nullptr ||
      shade_shader == nullptr)
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
  /* The display pass's target: filled once its scratch textures are acquired below, and the folder
   * accumulators it grows are released with the rest by the guard after them. */
  CombinedTarget screen;
  /* The Color Ramp and RGB Curves tables made for this update, released with the other temporaries.
   * No cache yet: a table is cheap next to the dispatch that reads it, and the payload hash would
   * have to be tracked across frames to make one worth its invalidation rules. */
  Vector<gpu::Texture *> filter_luts;
  /* Anchor Point slices captured before the channel passes, and the mask slices beside them. */
  Vector<GpuSliceSlot> slice_slots;
  Vector<GpuMaskSlot> mask_slots;
  /* The effects the preview computes itself, and the results its correction passes read. */
  Vector<GpuEffectJob> effect_jobs;
  Vector<GpuEffectResult> effect_results;
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
    for (gpu::Texture *texture : screen.folder_premul) {
      if (texture != nullptr) {
        pool.release_texture(texture);
      }
    }
    for (gpu::Texture *texture : screen.folder_cover) {
      if (texture != nullptr) {
        pool.release_texture(texture);
      }
    }
    for (gpu::Texture *texture : filter_luts) {
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
    /* The frame's own reference to each effect result goes last: the cache may evict an entry while
     * these passes still bind its texture, and a texture is only destroyed when the last reference
     * goes. */
    for (const GpuEffectJob &job : effect_jobs) {
      if (job.result != nullptr) {
        GPU_texture_free(job.result);
      }
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
  screen.row_color = row_color;
  screen.row_factor = row_factor;
  screen.mask_accum = mask_accum;
  screen.size = size;
  screen.groups = groups;
  screen.texel_to_uv = texel_to_uv;

  /* The dispatchers of the passes, over the frame's shaders and placeholder textures. The channel
   * loop below and the effect stage that follows both call them. */
  CombinedPasses passes(pool,
                        dummy,
                        slot_dummy,
                        layer_shader,
                        mask_shader,
                        correction_shader,
                        folder_shader,
                        effect_shader,
                        filter_luts,
                        slice_slots,
                        mask_slots,
                        effect_results,
                        accum_usage);

  /* An affine 2D map as the mat4 the push constants take: GLSL multiplies columns, so the
   * translation sits in the fourth one. */
  float4x4 texel_to_uv_4x4 = float4x4::identity();
  texel_to_uv_4x4[0] = float4(texel_to_uv[0].x, texel_to_uv[0].y, 0.0f, 0.0f);
  texel_to_uv_4x4[1] = float4(texel_to_uv[1].x, texel_to_uv[1].y, 0.0f, 0.0f);
  texel_to_uv_4x4[3] = float4(texel_to_uv[2].x, texel_to_uv[2].y, 0.0f, 1.0f);
  screen.texel_to_uv_4x4 = texel_to_uv_4x4;

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
  const Span<Span<const PaintMaterialCompositeImageLayer>> stack_view(
      stack_spans.data(), int64_t(stack_spans.size()));
  const Span<int> channel_view(channel_enums.data(), int64_t(channel_enums.size()));
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
        if (!passes.mask_build(screen, entry, has_mask)) {
          return false;
        }
        GpuMaskSlot *slot = passes.mask_slot_find(job.point);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point mask slot is missing");
        }
        if (has_mask) {
          if (!passes.mask_step(screen,
                         gpu_mask_slot_store,
                         nullptr,
                         0,
                         1.0f,
                         1.0f,
                         1.0f,
                         int(CompositeBlend::Mix),
                         0,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr,
                         slot->texture))
          {
            return false;
          }
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
        if (!passes.mask_build(screen, entry, has_mask)) {
          return false;
        }
        GpuSliceSlot *slot = passes.slice_slot_find(job.point, job.channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point slot is missing");
        }
        if (has_mask) {
          if (!passes.mask_step(screen,
                         gpu_mask_slot_store,
                         nullptr,
                         0,
                         1.0f,
                         1.0f,
                         1.0f,
                         int(CompositeBlend::Mix),
                         0,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr,
                         slot->color))
          {
            return false;
          }
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
        GpuSliceSlot *slot = passes.slice_slot_find(job.point, job.channel);
        if (slot == nullptr) {
          return gpu_refuse("an Anchor Point slot is missing");
        }
        const int emit_index = std::max(
            0,
            int(std::find(combined_shading_channels.begin(),
                          combined_shading_channels.end(),
                          eMaterialPaintChannel(job.channel)) -
                combined_shading_channels.begin()));
        /* A capture never blends onto the destination, but the pass still binds it as an image, so it
         * is named here the way the channel loop names it for a laid row. */
        screen.dest = accum[emit_index];
        if (!passes.row_emit(screen, entry, job.channel, -1, slot->color, slot->coverage)) {
          return false;
        }
      }
    }
  }

  /* -------------------------------------------------------------------- */
  /* GPU effects: the content each effect reads, then the effect's own passes into the cache. This
   * runs after the Anchor Point captures -- a content render may read their slots -- and before
   * the channels' stacks, which mix the results in as corrections. The stage fills #effect_jobs
   * and #effect_results; the defer above releases the frame's reference to each result when the
   * frame ends. */
  {
    const Span<bool> has_view(has_stack.data(), int64_t(has_stack.size()));
    if (!combined_preview_gpu_effects_run(
            ma, stack_view, channel_view, has_view, wanted_channel, passes, effect_jobs))
    {
      return false;
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
    screen.dest = accum[i];
    passes.lay(screen, accum[i], nullptr, gpu_layer_init, float4(0.0f), init_color, 0.0f);
    if (!has_stack[i]) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : stacks[i]) {
      if (!passes.row_emit(screen, layer, int(channel), -1, nullptr, nullptr)) {
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
    passes.dispatches++;
    GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);
    if (bke::paint_perf::enabled()) {
      GPU_finish();
    }
    perf_total.note("pass=%d texture=%dx%d dispatches=%d", pass, size.x, size.y, passes.dispatches);
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
      const MaterialPaintLayer *reader = BKE_paint_layers_find(ma, reader_marker);
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
  passes.dispatches++;
  /* The image engine samples the result next. */
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_FETCH);

  if (bke::paint_perf::enabled()) {
    /* Without a wait the timer would read only the time it took to record the commands. */
    GPU_finish();
  }
  perf_total.note("texture=%dx%d dispatches=%d", size.x, size.y, passes.dispatches);
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

  /* Reading a texture without HOST_READ usage is refused by the API assert in a debug build and
   * undefined in the backends, so skip the check rather than read what must not be read. Said once
   * per session: the check runs every 120th Combined frame, the message would repeat with it. */
  static bool host_read_missing_reported = false;
  if ((GPU_texture_usage(output) & GPU_TEXTURE_USAGE_HOST_READ) == 0) {
    if (!host_read_missing_reported) {
      host_read_missing_reported = true;
      printf("[PBR-PERF] gpu_verify: the display texture has no HOST_READ usage, check skipped\n");
      fflush(stdout);
    }
    return;
  }

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
      static_cast<long long>(blocks),
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
