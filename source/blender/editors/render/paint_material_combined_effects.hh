/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edrend
 *
 * The GPU Combined preview's pass machinery and its effect stage, split off
 * #paint_material_combined_gather.cc: the shaders' flag and step constants, the render target the
 * passes lay a stack into, and #CombinedPasses, the dispatcher of one pass of each kind, which both
 * the gather's channel loop and the effect stage call. The effect stage itself -- the job walk, the
 * GPU result cache's loop, the input render and the dispatch of the passes -- lives in
 * #paint_material_combined_effects.cc.
 */

#include <array>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_color_types.h"
#include "DNA_image_types.h"

#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_texture_pool.hh"

namespace blender::ed::material_combined {

/* -------------------------------------------------------------------- */
/** \name Shader ABI constants
 *
 * Mirrors of the flags and the pass steps the compute shaders of `image_engine_combined.bsl.hh`
 * read; a renumber on one side fails the build instead of dispatching the wrong branch.
 * \{ */

constexpr int gpu_layer_init = 1;
constexpr int gpu_layer_has_map = 2;
constexpr int gpu_layer_alpha_coverage = 4;
constexpr int gpu_layer_premultiplied = 8;
constexpr int gpu_layer_mapped = 32;
constexpr int gpu_layer_repeat_u = 64;
constexpr int gpu_layer_repeat_v = 128;
constexpr int gpu_layer_tangent_normal = 256;
constexpr int gpu_layer_normal_directx = 262144;
constexpr int gpu_layer_mirror_u = 1048576;
constexpr int gpu_layer_mirror_v = 2097152;
/* What the row's map stores (#eMaterialPaintLayerMapEncoding): the read-time conversion the layer
 * pass runs on the sampled texel, before the coverage. The spec-gloss alpha packing and the F0
 * colour with gloss alpha are the two `*_alpha` ones; their map alpha is data, so
 * #gpu_layer_alpha_coverage never joins them. */
constexpr int gpu_layer_map_glossiness = 4194304;
constexpr int gpu_layer_map_glossiness_alpha = 8388608;
constexpr int gpu_layer_map_f0 = 16777216;
constexpr int gpu_layer_map_f0_glossiness_alpha = 33554432;
static_assert(gpu_layer_map_glossiness == 4194304, "COMBINED_LAYER_MAP_GLOSSINESS");
static_assert(gpu_layer_map_glossiness_alpha == 8388608, "COMBINED_LAYER_MAP_GLOSSINESS_ALPHA");
static_assert(gpu_layer_map_f0 == 16777216, "COMBINED_LAYER_MAP_F0");
static_assert(gpu_layer_map_f0_glossiness_alpha == 33554432,
              "COMBINED_LAYER_MAP_F0_GLOSSINESS_ALPHA");
constexpr int gpu_layer_has_mask = 2048;
constexpr int gpu_layer_row_store = 4096;
constexpr int gpu_layer_row_use = 8192;
constexpr int gpu_layer_empty_base = 16384;
constexpr int gpu_layer_tracks_alpha = 32768;
constexpr int gpu_layer_adjustment = 65536;
/* The row reads an Anchor Point slice from a screen-space slot. Mirrors the shader. */
constexpr int gpu_layer_slot = 524288;
/* The steps of the folder pass, and how many folders may nest. */
constexpr int gpu_folder_init = 0;
constexpr int gpu_folder_blend = 1;
constexpr int gpu_folder_resolve = 2;
constexpr int gpu_folder_levels_max = 4;
/* The mask pass shares the layer pass's map flags; these are its own. */
constexpr int gpu_mask_invert = 512;
constexpr int gpu_mask_from_alpha = 1024;
/* The steps of the mask pass. */
constexpr int gpu_mask_init = 0;
constexpr int gpu_mask_item = 1;
constexpr int gpu_mask_map = 2;
/* A map-driven coverage item: begin, one pass per colour (or one scalar), end. */
constexpr int gpu_mask_cov_begin = 3;
constexpr int gpu_mask_cov_color = 4;
constexpr int gpu_mask_cov_scalar = 5;
constexpr int gpu_mask_cov_end = 6;
/* Write the built mask factor into a screen-space slot for a mask Anchor Point slice. */
constexpr int gpu_mask_slot_store = 7;
/* A mask item reads an Anchor Point slice from a screen-space slot. */
constexpr int gpu_mask_slot = 2048;
/* The ID map is read at its nearest texel. */
constexpr int gpu_map_nearest = 131072;
/* The correction is a CPU-computed effect the GPU computed: its result is in `effect_tx`. Mirrors
 * the shader's #COMBINED_LAYER_EFFECT_RESULT.
 */
constexpr int gpu_effect_result = 1048576;
static_assert(gpu_effect_result == 1048576, "COMBINED_LAYER_EFFECT_RESULT");

/* The Mirror flags must match #COMBINED_LAYER_MIRROR_U / _V of the shader and stay clear of the
 * other layer-pass flags. Only the correction pass reuses the Mirror U bit, as
 * #gpu_effect_result: an effect correction never gets a mapping, so the two never meet. */
static_assert(gpu_layer_mirror_u == 1048576, "COMBINED_LAYER_MIRROR_U");
static_assert(gpu_layer_mirror_v == 2097152, "COMBINED_LAYER_MIRROR_V");
static_assert(gpu_layer_mirror_u == gpu_effect_result,
              "The effect result flag shares the Mirror U bit by design");
static_assert((gpu_layer_mirror_v &
               (gpu_layer_init | gpu_layer_has_map | gpu_layer_alpha_coverage |
                gpu_layer_premultiplied | gpu_layer_mapped | gpu_layer_repeat_u |
                gpu_layer_repeat_v | gpu_layer_tangent_normal | gpu_layer_normal_directx |
                gpu_layer_mirror_u | gpu_layer_has_mask | gpu_layer_row_store |
                gpu_layer_row_use | gpu_layer_empty_base | gpu_layer_tracks_alpha |
                gpu_layer_adjustment | gpu_layer_slot | gpu_map_nearest)) == 0,
              "Mirror V overlaps another layer flag");
static_assert((gpu_layer_mirror_u &
               (gpu_layer_init | gpu_layer_has_map | gpu_layer_alpha_coverage |
                gpu_layer_premultiplied | gpu_layer_mapped | gpu_layer_repeat_u |
                gpu_layer_repeat_v | gpu_layer_tangent_normal | gpu_layer_normal_directx |
                gpu_layer_has_mask | gpu_layer_row_store | gpu_layer_row_use |
                gpu_layer_empty_base | gpu_layer_tracks_alpha | gpu_layer_adjustment |
                gpu_layer_slot | gpu_map_nearest)) == 0,
              "Mirror U overlaps another layer flag");

/**
 * The UV mapping values a layer, mask or correction pass uploads as uniforms. The flags that go
 * with them stay with the caller, who adds them to the pass's own.
 */
struct GpuMappingUniforms {
  float2 offset = float2(0.0f);
  float2 scale = float2(1.0f);
  float2 rotation = float2(1.0f, 0.0f);
  float2 mirror_center = float2(0.5f, 0.5f);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame slots and effect jobs
 * \{ */

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

/** The GPU result of one effect this frame computed, found by the effect and the channel it was made
 * for. */
struct GpuEffectResult {
  bUUID marker;
  int channel = 0;
  gpu::Texture *texture = nullptr;
};

/**
 * One effect the preview has to have a GPU result for, with everything its pass needs. The stages
 * below fill the textures in: the cache's result first, then -- only when the cache had none or the
 * input moved -- the content the effect reads and the kernel's answer.
 */
struct GpuEffectJob {
  const PaintMaterialCompositeCorrection *correction = nullptr;
  /** The effect row of the description, named by the correction's marker, and the row it corrects. */
  const MaterialPaintLayer *effect = nullptr;
  const MaterialPaintLayer *row = nullptr;
  int channel = 0;
  /** The input's size: the result's size, and the texel space the kernel counts in. */
  int2 input_size = int2(0);
  /** The cache's texture, referenced for this frame: the effect pass writes it, the correction pass
   * reads it. */
  gpu::Texture *result = nullptr;
  /** The input canvas, held from the pool only while a result is being computed. */
  gpu::Texture *input = nullptr;
  /** The maps the input renders from: their versions decide when the result goes stale. */
  Vector<Image *> images;
  /** Texels that moved since the cached result; carried for the GPU pass's region re-render. */
  rcti region = {0, 0, 0, 0};
  /** False when the cache hit and nothing is to be rendered. */
  bool needs_render = false;
  /** Whether the input's fourth component is a content alpha the composite keeps. */
  bool tracks_alpha = false;
  /** The described input (describe-only): its size, the maps it reads, and the region-exactness and
   * scalar fingerprint the GPU key folds in. */
  PaintMaterialCompositeImageLayer input_entry;
  /** The GPU-only key halves: an ordered fingerprint of the effects before this one in its row, and
   * one of the input's non-map scalar content. Computed from the description, without a GPU. */
  uint64_t chain_hash = 0;
  uint64_t input_scalar_hash = 0;
  /** The pass plan of the effect, and whether a partial dispatch may narrow to the region. */
  Vector<PaintLayerEffectPass> passes;
  /** The neighbourhood reach of the whole plan, in input texels. */
  int halo = 0;
  /** The halo of the effects before this one in its row, whose re-rendered bands are part of this
   * effect's input: the dispatch grows the dirty region by it (a Blur followed by a Sharpen must
   * re-render the Sharpen over the Blur's halo too). */
  int predecessor_halo = 0;
  /** True when the two region-accuracy rules hold, so the dispatch narrows to region + halo; false
   * renders the whole canvas (a map of another size, or a remapping between a map and the canvas). */
  bool region_exact = false;
};

/** The arguments of one effect pass dispatch: the job the pass belongs to, one pass of its plan,
 * and the buffers and rectangle that pass runs over. */
struct EffectDispatchArgs {
  const GpuEffectJob &job;
  const PaintLayerEffectPass &pass;
  gpu::Texture *src;
  gpu::Texture *aux;
  gpu::Texture *dst;
  const rcti &rect;
};

/**
 * The scratch and the geometry of one combined-preview render target: the textures the passes lay a
 * stack into, the size they work at, and the map from one of their texels to a canvas UV.
 *
 * The Image Editor's display pass has a target at the region's resolution, because that is what the
 * eye is shown at. A GPU effect needs one at the **input's** resolution instead: its kernel is a
 * function of the input texel -- a Noise hashes the texel coordinates, a Blur's radius counts texels --
 * so computing it at the screen's would be a different picture from the bake and the render. Both
 * are laid by the same passes; only the target differs.
 */
struct CombinedTarget {
  /** The row's colour and coverage while its corrections are laid over them. */
  gpu::Texture *row_color = nullptr;
  gpu::Texture *row_factor = nullptr;
  /** The mask factor of the row being laid, rebuilt by one step per item. */
  gpu::Texture *mask_accum = nullptr;
  /** Where the stack of one channel ends up: one of the frame's accumulators, or the texture an
   * effect's content is captured into. */
  gpu::Texture *dest = nullptr;
  /** The pre-multiplied colour and coverage of the folders being laid, one pair per nesting level.
   * Made by the pass that first needs a level, and owned by the target rather than by the frame: a
   * second target must not share a screen-sized one. */
  std::array<gpu::Texture *, gpu_folder_levels_max> folder_premul = {};
  std::array<gpu::Texture *, gpu_folder_levels_max> folder_cover = {};
  int2 size = int2(0);
  /** #size divided by the passes' 16x16 workgroup, rounded up. */
  int2 groups = int2(0);
  /** Output texel centre to canvas UV, in the two forms the passes push. */
  float3x3 texel_to_uv = float3x3::identity();
  float4x4 texel_to_uv_4x4 = float4x4::identity();
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name CombinedPasses
 * \{ */

/**
 * The dispatchers of the combined preview's passes: one method per pass kind, laid over whatever
 * #CombinedTarget the caller names. Both the gather's channel loop and the effect stage call them,
 * so a pass's uniforms, its barriers and its dispatch order are written once. The frame-level state
 * the passes share -- the shader handles, the placeholder textures, the LUT list, the captured
 * slots and the effect results -- is referenced here; the frame owns it and releases it.
 */
struct CombinedPasses {
  CombinedPasses(gpu::TexturePool &pool,
                 gpu::Texture *dummy,
                 gpu::Texture *slot_dummy,
                 gpu::Shader *layer_shader,
                 gpu::Shader *mask_shader,
                 gpu::Shader *correction_shader,
                 gpu::Shader *folder_shader,
                 gpu::Shader *effect_shader,
                 Vector<gpu::Texture *> &filter_luts,
                 Vector<GpuSliceSlot> &slice_slots,
                 Vector<GpuMaskSlot> &mask_slots,
                 Vector<GpuEffectResult> &effect_results,
                 eGPUTextureUsage accum_usage);

  gpu::TexturePool &pool;
  /** Placeholder sampled where a pass reads no map: a sampler and an image of one dispatch must
   * not be the same texture. */
  gpu::Texture *dummy;
  gpu::Texture *slot_dummy;
  gpu::Shader *layer_shader;
  gpu::Shader *mask_shader;
  gpu::Shader *correction_shader;
  gpu::Shader *folder_shader;
  gpu::Shader *effect_shader;
  /** The Color Ramp and RGB Curves tables made for this update; the frame releases them. */
  Vector<gpu::Texture *> &filter_luts;
  /** The slots captured before the passes, and the effect results the corrections read. */
  Vector<GpuSliceSlot> &slice_slots;
  Vector<GpuMaskSlot> &mask_slots;
  Vector<GpuEffectResult> &effect_results;
  /** The usage every canvas of this frame is acquired with. */
  eGPUTextureUsage accum_usage;
  /** Every dispatch the passes made, for the frame's perf note. */
  int dispatches = 0;

  /** Lay one layer (or folder step) onto \a dest, sampling \a map or a constant colour. */
  bool lay(CombinedTarget &t,
           gpu::Texture *dest,
           gpu::Texture *map,
           const int flags,
           const float4 &layer_color,
           const float4 &init_color,
           const float opacity,
           const GpuMappingUniforms &mapping = {},
           const int blend = int(CompositeBlend::Mix),
           gpu::Texture *mask = nullptr,
           const int filter_kind = 0,
           const float *filter_params = nullptr,
           const ColorBand *filter_ramp = nullptr,
           const CurveMapping *filter_curves = nullptr,
           gpu::Texture *slot_coverage = nullptr);

  /** One step of a row's mask factor, into the mask accumulator. */
  bool mask_step(CombinedTarget &t,
                 const int mode,
                 gpu::Texture *map,
                 const int flags,
                 const float constant_gray,
                 const float opacity,
                 const float influence,
                 const int blend,
                 const int filter,
                 const float *filter_params,
                 const ColorBand *filter_ramp,
                 const CurveMapping *filter_curves,
                 const PaintMaterialCompositeMapping *mapping,
                 gpu::Texture *slot_dst = nullptr,
                 gpu::Texture *mask_source = nullptr);

  /** One correction laid over the row's colour and coverage. False when its map has no GPU texture. */
  bool correction_step(CombinedTarget &t,
                       const PaintMaterialCompositeCorrection &correction,
                       const bool tracks_alpha,
                       gpu::Texture *slot_color = nullptr,
                       gpu::Texture *slot_coverage = nullptr,
                       gpu::Texture *effect_result = nullptr);

  /** One step of a folder: seed its accumulators, lay a child over them, or resolve them into the
   * row textures as the folder's own row. */
  void folder_step(CombinedTarget &t,
                   const int mode,
                   const int level,
                   const int flags,
                   const float opacity,
                   const int blend,
                   gpu::Texture *mask);

  /** The accumulators of the folders at nesting \a level, made when first needed. */
  bool folder_levels_ensure(CombinedTarget &t, const int level);

  /** The slots captured before the passes. Linear: there are at most PAINT_LAYER_ANCHOR_POINT_LIMIT
   * points times the shaded channels, so a search is cheaper than a hash of pairs. */
  GpuSliceSlot *slice_slot_find(const bUUID &point, const int channel);
  GpuMaskSlot *mask_slot_find(const bUUID &point);

  /** The GPU results of the effects this frame computed, found by the effect's marker and channel:
   * a correction of a description names the effect it was made from, and the same effect can be laid
   * in more than one channel. */
  gpu::Texture *effect_result_find(const bUUID &marker, const int channel);

  /** Build the mask factor of \a layer in the mask accumulator: seeded with one, each item laid over
   * it, then the row's own mask map. False when a mask map has no GPU texture. */
  bool mask_build(CombinedTarget &t, const PaintMaterialCompositeImageLayer &layer, bool &r_has_mask);

  /**
   * Lay \a layer on the channel \a channel, into \a t. \a level is -1 for a row of the stack, which
   * blends onto the target's destination, and the nesting level of the folder holding it otherwise,
   * whose accumulators the row then goes into. A folder lays its children into accumulators of its
   * own first and then goes on as a row that reads them back like a map. Returns false when the GPU
   * path has to give up, with the reason already reported.
   *
   * \a capture_color and \a capture_coverage, when given, ask for the row's content before its own
   * opacity, mask and blend: the slice a reader of a point in this row lays, and the input an effect
   * is computed from.
   */
  bool row_emit(CombinedTarget &t,
                const PaintMaterialCompositeImageLayer &layer,
                const int channel,
                const int level,
                gpu::Texture *capture_color,
                gpu::Texture *capture_coverage);

  /**
   * One dispatch of one effect pass: bind the pass's source and (for a tail) its blurred copy, push
   * the pass's kind, radius and region, run it, and leave the barriers set so the next read sees the
   * write. `dst` is a scratch buffer for the middle passes and the cache texture for the last.
   */
  void effect_pass_dispatch(const EffectDispatchArgs &args, const float4 &luma_coefficients);

 private:
  /** Whether a Color Ramp's table steps at its stops instead of interpolating between them. */
  static bool color_ramp_is_constant(const ColorBand &band);

  /**
   * The table the shader's `filter_lut_tx` reads for \a filter_kind, or \a dummy for a filter that
   * needs none. Null when the filter has no payload or the texture could not be allocated, which
   * the caller turns into a refusal; a Color Ramp or RGB Curves without its payload must not be
   * shaded as if it were the identity.
   */
  gpu::Texture *filter_lut_acquire(const int filter_kind,
                                   const ColorBand *ramp,
                                   const CurveMapping *curves);

  /** The mip level to read a map at: the texels it has per screen pixel, as a power of two. */
  float map_lod(CombinedTarget &t, gpu::Texture *map, const int flags, const float2 &mapping_scale);
};

/**
 * The GPU effect stage of the combined preview: collect the effects of the described stacks, key
 * their results in the GPU result cache, render the input of each one that needs it and dispatch its
 * pass plan over the input's own resolution, then publish. Runs after the Anchor Point captures and
 * before the channels' stacks, which mix the results in as corrections.
 *
 * \param stacks: the described stack per shaded channel, in the shading order (the capture plan's
 *        view of the same stacks). Only the channels \a has_stack names and, when \a wanted_channel
 *        is not -1, that one channel are walked.
 * \param r_jobs: the jobs of the frame, in collection order. The caller keeps it and releases each
 *        job's result reference when the frame ends; the results themselves are found through
 *        #CombinedPasses::effect_result_find.
 * \return false when the GPU path has to give up, with the reason already reported.
 */
bool combined_preview_gpu_effects_run(
    const Material &ma,
    Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    Span<int> channel_enums,
    Span<bool> has_stack,
    int wanted_channel,
    CombinedPasses &passes,
    Vector<GpuEffectJob> &r_jobs);

/**
 * Report why the GPU path refused, once per change of reason rather than once per frame, and
 * return false for the caller to hand on. Printed through the perf log, so it follows
 * `BLENDER_PBR_PERF`. Defined in #paint_material_combined_gather.cc, which owns the reported
 * reason.
 */
bool gpu_refuse(const char *reason);

/** Report, once after a refusal or at the first frame, that the GPU path is shading the preview.
 * Defined in #paint_material_combined_gather.cc. */
void gpu_report_active();

/** \} */

}  // namespace blender::ed::material_combined
