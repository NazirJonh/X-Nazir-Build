/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers_composite.hh: the description -> CPU layer stack bridge.
 */

/* The math conversion macros come first: a header on the include chain below undefines them,
 * and the facing bake below reads them. */
#include "BLI_math_constants.h"

#include "BKE_paint_layers_composite.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_constants.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_task.hh"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_generator_registry.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_layers_node_group.hh"
#include "BKE_paint_layers_shader_bake.hh"
#include "BKE_paint_layers_source_graph.hh"
#include "BKE_paint_material_decal_map.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_report.hh"

#include "../paint_material_composite_internal.hh"
#include "paint_layers_composite_intern.hh"
#include "paint_layers_effect_cache_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/**
 * Restore the unit length of an encoded tangent-space normal the partial factors shortened.
 *
 * A byte preview of Normal is only meaningful once this runs: the shader normalizes the combined
 * normal, so an export that skipped it would disagree with the render.
 */
static void composite_normal_channel_normalize(ImBuf &ibuf, const rcti *region)
{
  if (ibuf.byte_buffer.data == nullptr) {
    return;
  }
  int xmin = 0, xmax = ibuf.x, ymin = 0, ymax = ibuf.y;
  if (region != nullptr) {
    xmin = max_ii(0, region->xmin);
    xmax = min_ii(ibuf.x, region->xmax);
    ymin = max_ii(0, region->ymin);
    ymax = min_ii(ibuf.y, region->ymax);
    if (xmin >= xmax || ymin >= ymax) {
      return;
    }
  }
  uchar *pixels = ibuf.byte_data_for_write();
  for (int y = ymin; y < ymax; y++) {
    for (int x = xmin; x < xmax; x++) {
      uchar *p = pixels + (int64_t(y) * ibuf.x + x) * 4;
      float normal[3] = {
          float(p[0]) / 127.5f - 1.0f, float(p[1]) / 127.5f - 1.0f, float(p[2]) / 127.5f - 1.0f};
      const float length = len_v3(normal);
      if (length <= 0.0f) {
        continue;
      }
      mul_v3_fl(normal, 1.0f / length);
      for (const int i : IndexRange(3)) {
        p[i] = uchar(clamp_i(int((normal[i] * 0.5f + 0.5f) * 255.0f + 0.5f), 0, 255));
      }
    }
  }
}

/**
 * The CPU blend a row takes in \a channel.
 *
 * The Normal channel is combined rather than mixed -- interpolating encoded normals would flatten
 * the relief -- unless its channel is set to Replace, which lets the upper normal cover the lower
 * one with a plain Mix, as the generator builds it. Every other channel uses its own blend, or the
 * row's while it inherits, through the one table the generator reads too.
 */
static CompositeBlend layer_channel_blend(const MaterialPaintLayer &layer, const int channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return BKE_paint_layers_normal_replace(layer) ? CompositeBlend::Mix :
                                                    CompositeBlend::NormalCombine;
  }
  return CompositeBlend(BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
      BKE_paint_layers_channel_blend_effective(layer, channel))));
}

/**
 * Evaluate image_layers of channel into r_linear as straight scene-linear RGBA, starting
 * from the channel's neutral value, so a partially covered row fades towards what the material
 * shows where nothing covers it rather than towards zero.
 */
static bool composite_channel_linear(const Span<PaintMaterialCompositeImageLayer> image_layers,
                                     const int channel,
                                     const PaintMaterialCompositeLinearTarget &target,
                                     const rcti *region)
{
  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
  return BKE_paint_material_composite_eval_images_linear(
      image_layers, target, region, nullptr, bottom_color);
}

/**
 * Describe \a row's projection to the evaluator: the frame the bake reads the row's maps (or a UV
 * gradient) through. A space that does not apply is left disabled. The one place a projection is
 * described, so a map read and a projected gradient line cannot drift apart.
 */
static void composite_projection_describe(const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const int tile_number,
                                          const Image *source_image,
                                          PaintLayerSurfaceRef painted_object,
                                          PaintMaterialCompositeProjection &out)
{
  /* The tile the caller composites, written whether or not the row projects: the evaluator's
   * fallback reads every map of the row through it, so the bake or export of tile N sees tile N's
   * pixels of a tiled map instead of the base tile's. A row whose projection applies rasterizes
   * the same tile, so the field serves both reads. */
  out.tile_number = tile_number;
  if (!BKE_paint_layers_projection_applies(ma, row)) {
    out.present = false;
    return;
  }
  const MaterialPaintLayerProjection &projection = *row.projection;
  out.present = true;
  out.space = row.mapping.space;
  out.axis = projection.axis;
  out.cube_layout = projection.cube_layout;
  copy_v3_v3(out.center, projection.center);
  /* Same effective extents as the generated graph: the inline size reads one while the row takes
   * its scale from the controller. */
  BKE_paint_layers_projection_size_effective(row, out.size);
  /* Euler derived from normal+roll, matching the graph's ROLE_PROJECTION_ROTATION value. */
  float frame_normal[3];
  BKE_paint_layers_projection_normal_get(row, frame_normal);
  BKE_paint_layers_projection_frame_euler(frame_normal, projection.roll, out.rotation);
  out.blend = projection.blend;
  out.clip = BKE_paint_layers_projection_clip_get(row);
  out.depth = BKE_paint_layers_projection_depth_get(row);
  out.facing = BKE_paint_layers_projection_facing_get(row);
  /* The facing weight's scale and offset from the limit and the flip, through the one helper the
   * graph's value sync bakes with, so a bake reads the frame the viewport shows. */
  BKE_paint_layers_projection_facing_weights(BKE_paint_layers_projection_angle_get(row),
                                             BKE_paint_layers_projection_facing_invert_get(row),
                                             out.facing_scale,
                                             out.facing_offset);
  /* The aspect fit lands on the row's two footprint axes, the depth axis keeps one, exactly as
   * the generated graph's Aspect input does. */
  copy_v3_fl(out.aspect, 1.0f);
  if (BKE_paint_layers_projection_fit_aspect_get(row)) {
    float factors[2];
    BKE_paint_layers_projection_aspect_factors(source_image, factors);
    const int8_t axis = BKE_paint_layers_projection_axis_get(row);
    const int u_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_X) ? 1 : 0;
    const int v_index = (axis == MA_PAINT_LAYER_PROJECTION_AXIS_Z) ? 1 : 2;
    out.aspect[u_index] = factors[0];
    out.aspect[v_index] = factors[1];
  }
  /* The row's mapping scale is its tiling here, like the graph's Tiling input. */
  out.tiling[0] = BKE_paint_layers_mapping_scale_normalize(row.mapping.scale[0]);
  out.tiling[1] = BKE_paint_layers_mapping_scale_normalize(row.mapping.scale[1]);
  out.tile_number = tile_number;
  out.source_tiled = source_image != nullptr && source_image->source == IMA_SRC_TILED;
  out.object = painted_object.object;
  out.snapshot = painted_object.snapshot;
  out.controller = row.projection_object;
  out.material = &ma;
}

/**
 * Describe \a row's projection and UV mapping to the evaluator for the map it reads in \a channel.
 * A space that does not apply is left disabled, so the evaluator reads the map as it is. A
 * projection takes precedence over the UV adjust; the bake applies both in the same order as the
 * generated graph (project, then rotate/scale/offset).
 */
static void composite_mapping_describe(const Material &ma,
                                       const MaterialPaintLayer &row,
                                       const int channel,
                                       PaintMaterialCompositeMapping &out,
                                       const int tile_number,
                                       PaintLayerSurfaceRef painted_object,
                                       const Image *source_image = nullptr)
{
  composite_projection_describe(ma, row, tile_number, source_image, painted_object, out.projection);
  /* The wrap is read even when the 2D adjust is off: a projected lookup still follows it, as the
   * generated Image Texture's extension does. */
  out.wrap = BKE_paint_layers_mapping_wrap_get(row);
  if (!BKE_paint_layers_mapping_remaps(ma, row)) {
    return;
  }
  out.enabled = true;
  copy_v2_v2(out.offset, row.mapping.offset);
  copy_v2_v2(out.scale, row.mapping.scale);
  out.rotation = row.mapping.rotation;
  /* A content row never owns the fold: the axes here are always off, and the center is read only
   * alongside them. The fold is the Mirror effect's own step (see
   * #composite_transform_step_numbers_get). */
  out.mirror_axes = BKE_paint_layers_mapping_mirror_get(row);
  BKE_paint_layers_mapping_mirror_center_get(row, out.mirror_center);
  out.tangent_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
}

/**
 * Describe the Decal frame of a transform effect's step: the frame a Decal-space step reads its
 * owner's map through (see paint_material_composite_effect_space.cc). The user does not place it:
 * \a frame is derived from the effect's Mirror center / pivot, a UV point (see
 * #BKE_paint_material_decal_frame_from_uv), so the same handle as in UV drives it. It lies on the
 * painted object's surface, so it has no controller, depth slab, facing fade or aspect fit. It does
 * not clip: past the patch (which reaches the whole object) the owner's map is read as it is.
 */
static void composite_effect_decal_describe(const Material &ma,
                                            const PaintMaterialDecalUvFrame &frame,
                                            const int tile_number,
                                            PaintLayerSurfaceRef painted_object,
                                            PaintMaterialCompositeProjection &out)
{
  out.present = true;
  out.space = MA_PAINT_LAYER_MAPPING_SPACE_DECAL;
  out.axis = MA_PAINT_LAYER_PROJECTION_AXIS_Z;
  copy_v3_v3(out.center, frame.center);
  /* The size is the surface length of one UV unit per axis, so one unit of the step's coordinate is
   * one UV unit at the frame's center. */
  out.size[0] = 2.0f * frame.half_size.x;
  out.size[1] = 2.0f * frame.half_size.y;
  out.size[2] = 1.0f;
  copy_v3_v3(out.rotation, frame.rotation);
  out.patch_radius = frame.radius;
  out.clip = false;
  out.depth = false;
  out.facing = false;
  copy_v3_fl(out.aspect, 1.0f);
  out.tiling[0] = 1.0f;
  out.tiling[1] = 1.0f;
  out.tile_number = tile_number;
  out.object = painted_object.object;
  out.snapshot = painted_object.snapshot;
  out.controller = nullptr;
  out.material = &ma;
}

/**
 * The numbers of the transform effect \a effect as one UV step: a Mirror's fold, or a Transform
 * Mapping's offset, rotation, scale and wrap. The one place both the content effects' steps and a
 * mask item's step are read, so the two cannot move a map differently.
 */
static void composite_transform_step_numbers_get(const MaterialPaintLayer &effect,
                                                 PaintMaterialCompositeMapping &step)
{
  if (effect.kind_subtype == MA_PAINT_LAYER_FILTER_MIRROR) {
    /* The fold alone is the step: a Mirror reads its mapping's axes and center and nothing else,
     * and its read repeats: those two are its whole parameter set, so it has no wrap. */
    step.mirror_axes = BKE_paint_layers_mapping_mirror_get(effect);
    step.mirror_center[0] = effect.mapping.mirror_center[0];
    step.mirror_center[1] = effect.mapping.mirror_center[1];
  }
  else {
    /* A Transform Mapping reads the offset, rotation, scale and wrap; the fold stays the
     * Mirror's alone. The numbers are the shared vocabulary's, so a zero scale axis reads back
     * as one where the helpers use them. */
    BKE_paint_layers_transform_effective_offset(effect, step.offset);
    copy_v2_v2(step.scale, effect.mapping.scale);
    step.rotation = effect.mapping.rotation;
    step.wrap = BKE_paint_layers_mapping_wrap_get(effect);
  }
}

/**
 * The numbers of the transform effect \a effect as one step in the Decal frame derived from the UV
 * point \a frame_uv. The frame's center is that UV point, and the step coordinate is the frame's
 * `c = 0.5 + 0.5 q` (one unit is one UV unit at the center), so the effect's UV point \a p lands at
 * `0.5 + (p - frame_uv)`: exactly the frame's center for the effect that placed the frame, and the
 * local UV offset from it for a later Decal effect with its own center (exact where the UV is
 * locally an isometry, an approximation across seams). The Mirror's fold and the Transform
 * Mapping's pivot use that point instead of the UV coordinate, which would be a position in the
 * frame's own coordinates.
 */
static void composite_decal_step_numbers_get(const MaterialPaintLayer &effect,
                                             const float frame_uv[2],
                                             PaintMaterialCompositeMapping &step)
{
  const float pivot[2] = {0.5f + (effect.mapping.mirror_center[0] - frame_uv[0]),
                          0.5f + (effect.mapping.mirror_center[1] - frame_uv[1])};
  if (effect.kind_subtype == MA_PAINT_LAYER_FILTER_MIRROR) {
    step.mirror_axes = BKE_paint_layers_mapping_mirror_get(effect);
    step.mirror_center[0] = pivot[0];
    step.mirror_center[1] = pivot[1];
    return;
  }
  copy_v2_v2(step.scale, effect.mapping.scale);
  step.rotation = effect.mapping.rotation;
  step.wrap = BKE_paint_layers_mapping_wrap_get(effect);
  /* `S * R * (c - p) + p + offset` folded into the plain `S * R * c + offset'` the step evaluates,
   * as #BKE_paint_layers_transform_effective_offset does for the UV pivot. */
  const float cos_rot = cosf(step.rotation);
  const float sin_rot = sinf(step.rotation);
  const float scale_x = BKE_paint_layers_mapping_scale_normalize(step.scale[0]);
  const float scale_y = BKE_paint_layers_mapping_scale_normalize(step.scale[1]);
  step.offset[0] = effect.mapping.offset[0] +
                   (pivot[0] - scale_x * (cos_rot * pivot[0] - sin_rot * pivot[1]));
  step.offset[1] = effect.mapping.offset[1] +
                   (pivot[1] - scale_y * (sin_rot * pivot[0] + cos_rot * pivot[1]));
}

void BKE_paint_layers_input_transforms_get(const Material &ma,
                                           const MaterialPaintLayer &row,
                                           const int channel,
                                           Vector<PaintMaterialCompositeMapping> &r_steps,
                                           const int tile_number,
                                           PaintLayerSurfaceRef painted_object)
{
  /* One Decal frame per row: the first Decal effect's Mirror center / pivot places it, and a later
   * Decal effect composes over the same map coordinate, so consecutive Decal steps need no
   * conversion between them. */
  bool frame_derived = false;
  bool frame_valid = false;
  PaintMaterialDecalUvFrame frame;
  float frame_uv[2] = {0.5f, 0.5f};
  for (const MaterialPaintLayer *effect_ptr : BKE_paint_layers_active_transform_effects(ma, row)) {
    const MaterialPaintLayer &effect = *effect_ptr;
    PaintMaterialCompositeMapping step;
    step.enabled = true;
    step.tangent_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    if (BKE_paint_layers_effect_decal_applies(ma, row, effect)) {
      if (painted_object == nullptr || painted_object.snapshot != nullptr) {
        /* No surface to unfold (a material preview with no paint context): the effect has no
         * meaning in UV, so it is left out rather than read as a UV fold. A snapshot carries no
         * Decal map either: the evaluated mesh behind it is the main thread's, and a row with
         * such an effect is baked there. */
        continue;
      }
      if (!frame_derived) {
        frame_derived = true;
        copy_v2_v2(frame_uv, effect.mapping.mirror_center);
        frame_valid = BKE_paint_material_decal_frame_from_uv(
            painted_object, &ma, tile_number, float2(frame_uv[0], frame_uv[1]), frame);
      }
      if (!frame_valid) {
        /* The center lies on no triangle the material uses: there is no surface to walk. */
        continue;
      }
      composite_effect_decal_describe(ma, frame, tile_number, painted_object, step.projection);
      composite_decal_step_numbers_get(effect, frame_uv, step);
    }
    else {
      composite_transform_step_numbers_get(effect, step);
    }
    r_steps.append(step);
  }
}

bool paint_layer_mask_transform_step_get(const MaterialPaintLayer &item,
                                         PaintMaterialCompositeMapping &r_step)
{
  if (!paint_layer_mask_item_is_transform(item)) {
    return false;
  }
  /* UV space only, so no frame is described: the grey is resampled on the canvas. */
  r_step = PaintMaterialCompositeMapping{};
  r_step.enabled = true;
  composite_transform_step_numbers_get(item, r_step);
  return true;
}

/**
 * The per-parameter override lookup a chain instance's unfolding reads: given a member schema
 * position, answer whether the instance carries a stored value for it, filling \a r_value with it.
 * A scalar parameter fills the first slot, a Color or a Vector the first three -- the same three
 * slots the packing below widens them to, alpha and the fourth left to the typed accessor. An empty
 * lookup (the ordinary filter packing) answers for nothing.
 */
using CompositeFilterParamOverride = FunctionRef<bool(int index, float r_value[3])>;

/**
 * The parameters of \a row's kind in the schema order the evaluator reads them in. A colour is not
 * a scalar: its three channels lay across the three slots at its place in the schema order, the
 * alpha left to the typed accessor (no kernel reads it from this fold). A vector packs the same
 * way -- three consecutive slots, no fourth -- so the two non-scalar schemas read by index alike.
 * That is the packing the GPU effect pass plan reads the Glow's tint by, and the one consumer of
 * a colour in the array; every reader of the other kinds' numbers reads them by schema index,
 * which the colour does not move.
 *
 * \a overrides is the chain instance's hook: where it answers, the stored override replaces the
 * member's own value, at the member schema's packed position, whatever the parameter's type. The
 * lookup runs inside the same walk as the packing, so the Color and Vector widening -- the one
 * subtlety of the layout -- cannot drift apart between the plain and the overridden read. The
 * payload folds above the walk (a Color Balance, a Channel Mixer) take no overrides: their values
 * are payloads the member owns whole, which the chain instance does not carry (v1).
 */
template<size_t N>
static void composite_filter_params(const MaterialPaintLayer &row,
                                    float (&r_params)[N],
                                    const CompositeFilterParamOverride &overrides = {})
{
  if (paint_layer_kind_has_color_balance_param(row)) {
    /* The balance is not a scalar schema: its payload folds into the 12 numbers the composite, the
     * graph and the GPU share. A caller whose array is shorter would truncate parameters, so
     * compilation fails if N is too small. */
    static_assert(N >= PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM,
                  "filter_params array is too short for the Color Balance fold");
    paint_layer_color_balance_fold(paint_layer_color_balance_or_neutral(row), r_params);
    return;
  }
  if (paint_layer_kind_has_channel_mixer_param(row)) {
    /* The matrix folds into the same 12-float layout, the monochrome switch included, so a null
     * payload reads as the neutral identity matrix. No payload pointer travels with the filter: the
     * composite, the graph and the GPU read these numbers alone. */
    if constexpr (N >= PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM) {
      static const MaterialPaintLayerChannelMixer neutral = {};
      paint_layer_channel_mixer_fold(
          row.channel_mixer != nullptr ? *row.channel_mixer : neutral,
          paint_layer_kind_param_bool_get(row, PAINT_LAYER_CHANNEL_MIXER_MONOCHROME),
          r_params);
    }
    return;
  }
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(row)) {
    int pos = 0;
    for (const int i : IndexRange(kind->params_num)) {
      const PaintLayerKindParamType type = kind->params[i].type;
      /* The override is asked once per parameter, before either packing branch: the same answer
       * feeds the scalar slot and the triple, so an instance override of a tint cannot land in the
       * slots the scalar read kept. */
      float override_value[3] = {0.0f, 0.0f, 0.0f};
      const bool overridden = overrides && overrides(i, override_value);
      if (type == PaintLayerKindParamType::Color || type == PaintLayerKindParamType::Vector) {
        /* Three consecutive floats, the schema order kept: the plan reads the Glow's tint here,
         * and a vector parameter rides the same way a colour does. The fourth slot stays zero for
         * a vector, so the array is stable whatever the frame held. */
        float packed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (overridden) {
          copy_v3_v3(packed, override_value);
        }
        else if (type == PaintLayerKindParamType::Color) {
          paint_layer_kind_param_color_get(row, i, packed);
        }
        else {
          paint_layer_kind_param_vector_get(row, i, packed);
        }
        for (const int c : IndexRange(3)) {
          if (pos + c < int(N)) {
            r_params[pos + c] = packed[c];
          }
        }
        pos += 3;
        continue;
      }
      if (pos < int(N)) {
        r_params[pos] = overridden ? override_value[0] : paint_layer_kind_param_get(row, i);
      }
      pos++;
    }
  }
}

void paint_layer_effect_filter_params(
    const MaterialPaintLayer &effect,
    float (&r_params)[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM])
{
  composite_filter_params(effect, r_params);
}

void BKE_paint_layers_effect_mask_filter_params(
    const MaterialPaintLayer &effect,
    float (&r_params)[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM])
{
  composite_filter_params(effect, r_params);
  /* The `for_mask` branch of the CPU kernel reads the border from the luminance for these four
   * kinds (see #paint_layer_effect_kernel_apply): a mask's grey has no alpha to read, so the alpha
   * source would see a canvas of ones. The override has to land before the plan is built, so the
   * seed / silhouette / border passes read the same channel the CPU branch does, and the Drop
   * Shadow's tail folds its under-mode off. */
  switch (eMaterialPaintLayerFilter(effect.kind_subtype)) {
    case MA_PAINT_LAYER_FILTER_DROP_SHADOW:
      r_params[PAINT_LAYER_DROP_SHADOW_SOURCE] = float(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE);
      break;
    case MA_PAINT_LAYER_FILTER_MASK_OUTLINE:
      /* The Outline's source reads as a #PaintLayerDropShadowSource, the same two values. */
      r_params[PAINT_LAYER_MASK_OUTLINE_SOURCE] = float(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE);
      break;
    case MA_PAINT_LAYER_FILTER_FILL_AREA:
      r_params[PAINT_LAYER_FILL_AREA_SOURCE] = float(PAINT_LAYER_FILL_AREA_SOURCE_LUMINANCE);
      break;
    case MA_PAINT_LAYER_FILTER_BEVEL:
      r_params[PAINT_LAYER_BEVEL_SOURCE] = float(PAINT_LAYER_BEVEL_SOURCE_LUMINANCE);
      break;
    default:
      break;
  }
}

/**
 * Describe \a row's generator in \a channel: the line and ramp of a Gradient, the pattern, the
 * placement and the colour band of the others, the channel's scalar ends and, for an
 * Object/Generated coordinate, the surface that gives the texel its position. The one place a
 * generator row, a generator correction and a generator mask item are described, so the evaluator
 * cannot read them differently.
 *
 * \a projection_row is the row whose frame a UV line reads when it projects: the row itself for a
 * generator row, its owner for a mask item, null for a correction (which owns no frame).
 */
static bool composite_generator_describe(
    const Material &ma,
    const MaterialPaintLayer &row,
    const int channel,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    PaintMaterialCompositeGradient &out,
    const MaterialPaintLayer *projection_row,
    const int tile_number,
    PaintLayerSurfaceRef painted_object = nullptr)
{
  const int kind = int(row.kind_subtype);
  const bool is_gradient = kind == MA_PAINT_LAYER_GENERATOR_GRADIENT;
  /* The registry knows the kind before its CPU factor lands: a row whose generator has no table
   * entry yet lays nothing, the same answer the build's dispatch gives it, rather than reading as
   * the Gradient its default subtype names. */
  if (BKE_paint_layers_generator_of(row) == nullptr) {
    return false;
  }
  if (is_gradient && row.gradient == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  out.enabled = true;
  /* The subtype rides with the payload, so the composite's sample dispatches through the
   * generator table the way the generated graph does. A generator the schema does not know is
   * refused by the kind lookup before its row is described. */
  out.subtype = int16_t(kind);
  out.color_channel = info.is_color;
  out.params = {};
  if (is_gradient) {
    /* The Gradient keeps its whole description in its payload; it has no schema. */
    out.payload = dna::shallow_copy(*row.gradient);
    out.space = int16_t(row.gradient->space);
  }
  else {
    out.space = int16_t(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_SPACE));
    if (const PaintLayerKindInfo *schema = paint_layer_kind_of(row)) {
      /* The Ramp-typed slot reads as zero: the band itself comes from the row below. */
      for (const int i : IndexRange(std::min(schema->params_num, int(out.params.size())))) {
        out.params[i] = paint_layer_kind_param_get(row, i);
      }
    }
    if (const ColorBand *band = BKE_paint_layers_ramp_get(row); band != nullptr) {
      out.color_ramp = *band;
      out.color_ramp_valid = true;
    }
  }
  const int size = (ma.paint_layers_image_size > 0) ? int(ma.paint_layers_image_size) : 4096;
  out.width = size;
  out.height = size;
  out.projected = false;
  if (out.space == MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
    /* A UV line already reads the texel's own UV: no surface and no approximation. Under a
     * projected row the frame replaces that UV, and the evaluators that reach the painted surface
     * render the line through the frame; the rest keeps the plain UV read. */
    out.surface = nullptr;
    out.surface_key = 0;
    out.approximate = false;
    if (projection_row != nullptr) {
      composite_projection_describe(
          ma, *projection_row, tile_number, nullptr, painted_object, out.projection);
      out.projected = out.projection.present;
    }
  }
  else {
    /* The 2D preview needs the texel's object-space position. Without one the evaluator falls
     * back to `(u, v, 0)`, the approximate view. Whether to hand out a surface while the active
     * row is being dragged is the caller's call (#ED_space_image_gradient_preview_sync): it passes
     * none then, so this stays the single place that reads "no surface". The row holds a reference
     * to the surface, so a cache replacement cannot pull it out from under the evaluation. A
     * Generated row has a coordinate with stored CD_ORCO samples or a usable bbox; with neither it
     * falls back to the approximate preview rather than an empty layer. */
    const bool coordinate_usable = out.space != MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED ||
                                   (context_surface &&
                                    (context_surface->generated_bbox_valid ||
                                     !context_surface->generated_positions.is_empty()));
    out.surface = context_surface;
    out.surface_key = context_surface ? context_surface->key : 0;
    out.approximate = !context_surface || !coordinate_usable;
  }
  if (!info.is_color) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
    if (record == nullptr) {
      return false;
    }
    /* A generator authors its two ends as relief (for Height, -1..1), while the composite works in
     * storage. The value ramp path needs no conversion: its grey stops are the normalized
     * parameter of the channel's range, which for Height is the stored value itself. */
    out.scalar_start = BKE_paint_material_channel_value_encode(eMaterialPaintChannel(channel),
                                                               record->value[0]);
    out.scalar_end = BKE_paint_material_channel_value_encode(eMaterialPaintChannel(channel),
                                                             record->value[1]);
    if (record->gradient_ramp != nullptr) {
      out.scalar_ramp = *record->gradient_ramp;
      out.scalar_ramp_valid = true;
    }
  }
  return true;
}

/**
 * The size of \a effect's slot image (#paint_layer_effect_slot_image) on \a tile_number, the one
 * place both a row's description and an effect's buffer ask it.
 * \return false when the effect has no slot image or it has no readable size.
 */
static bool paint_layer_effect_slot_size(const MaterialPaintLayer &effect,
                                         const int tile_number,
                                         int &r_width,
                                         int &r_height)
{
  Image *slot = paint_layer_effect_slot_image(effect);
  if (slot == nullptr) {
    return false;
  }
  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.tile = tile_number;
  r_width = 0;
  r_height = 0;
  BKE_image_get_size(slot, &iuser, &r_width, &r_height);
  return r_width > 0 && r_height > 0;
}

/**
 * The shader-bake's answer for a node-group row: the generated image the bake fills, at the
 * material's canvas size, or null while there is no backend or no definition. The one place the
 * two describes (the row's content and a laying correction's map) read it.
 */
static Image *composite_node_group_bake_image(const Material &ma,
                                              const MaterialPaintLayer &layer,
                                              const int bake_channel,
                                              const int tile_number,
                                              const PaintLayerSurfaceRef painted_object)
{
  Main *bmain = BKE_paint_layers_shader_bake_main();
  if (bmain == nullptr) {
    return nullptr;
  }
  const PaintLayerTileContext tile{.painted_object = painted_object,
                                   .tile_number = tile_number};
  const int size = ma.paint_layers_image_size;
  return BKE_paint_layers_shader_bake_image_get(
      *bmain, ma, layer, bake_channel, tile, size, size);
}

/**
 * The node-group layer's source: the shader-bake's answer is the row's content, laid like a map
 * at the bake's own alpha. The size is the material's canvas -- the bake is a generated image of
 * that side, resampled by the mapping like any other map. A row whose bake has no answer yet (no
 * backend, no bake landed) takes no part, the way a row with no map does.
 */
static bool composite_node_group_source_describe(const Material &ma,
                                                 const MaterialPaintLayer &layer,
                                                 const int channel,
                                                 const int tile_number,
                                                 PaintLayerSurfaceRef painted_object,
                                                 PaintMaterialCompositeImageLayer &out)
{
  /* A per-channel source takes part in the channels its tree names alone: a channel it does not
   * answer is the row not laying there, the way a record switched off is. The one-answer contract
   * (`Result`-only) reads the same map in every channel, as it always has. */
  bool per_channel[PAINT_MATERIAL_CHANNEL_NUM];
  const bool multi = BKE_paint_layers_node_group_source_channels(
      layer.kind_tree, MutableSpan<bool>(per_channel, PAINT_MATERIAL_CHANNEL_NUM));
  if (multi && (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || !per_channel[channel])) {
    return false;
  }
  /* -1 is the one-answer contract the backend lays `Result` by: zero would name Base Color and
   * miss a tree that only exposes `Result`. */
  const int bake_channel = multi ? channel : -1;
  Image *bake = composite_node_group_bake_image(ma, layer, bake_channel, tile_number, painted_object);
  if (bake == nullptr) {
    return false;
  }
  composite_mapping_describe(ma, layer, channel, out.mapping, tile_number, painted_object, bake);
  /* The row's transform effects move what the row itself lays, the row's own mapping last. */
  BKE_paint_layers_input_transforms_get(
      ma, layer, channel, out.input_transforms, tile_number, painted_object);
  out.color_image = bake;
  out.color_iuser = nullptr;
  /* The bake's alpha is straight and covers where the group lays, the way a painted map's does. */
  out.color_alpha_coverage = true;
  return true;
}

/**
 * The node-group generator's source: the shader-bake's answer is the row's content in \a channel,
 * laid like a map at the bake's own alpha -- the same shape the source describe gives the
 * node-group source, reached per channel because a generator answers one map per channel. The
 * pattern emission (the placement chain into the row's tree, the `Fac` through the channel's ramp
 * tail) is rendered by the backend the way the viewport's graph builds it, so the bake map, the
 * CPU preview and the exports carry the pattern the viewport shows.
 *
 * A generator takes part in the channels its live records keep -- the same predicate the graph's
 * participation reads, whose Normal exclusion is the registry's #PAINT_LAYER_GENERATOR_NO_NORMAL
 * (the channel-add rule already keeps a Normal record off the row). A channel whose bake has no
 * answer yet (no backend, no bake landed) takes no part, the way a row with no map does: leaving
 * it out, not describing a zero, keeps the stack below visible while the bake is due.
 */
static bool composite_node_group_generator_describe(const Material &ma,
                                                    const MaterialPaintLayer &layer,
                                                    const int channel,
                                                    const int tile_number,
                                                    PaintLayerSurfaceRef painted_object,
                                                    PaintMaterialCompositeImageLayer &out)
{
  /* A subtype this build's table does not know reads as no generator: the same skip the
   * #composite_generator_describe gives, rather than a payload read as another kind's. */
  if (BKE_paint_layers_generator_of(layer) == nullptr) {
    return false;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (!paint_layer_channel_contributes(ma, layer, record)) {
    return false;
  }
  Image *bake = composite_node_group_bake_image(ma, layer, channel, tile_number, painted_object);
  if (bake == nullptr) {
    return false;
  }
  /* The bake already carries the row's placement (the backend builds the same chain the generated
   * graph does), so the row mapping is not laid on top of it: doing so would move a pattern that
   * the CPU generator path leaves at its placed coordinate. The generator's own transform effects
   * are refused by the kind rules, and `out.input_transforms` stays empty for the same reason. */
  out.color_image = bake;
  out.color_iuser = nullptr;
  /* The bake's alpha is the tail's coverage -- a colour channel's band alpha, an opaque one on a
   * scalar channel -- and covers the way a painted map's does. */
  out.color_alpha_coverage = true;
  return true;
}

/**
 * Describe the source of the row \a layer (not a folder) in \a channel to the evaluator: the
 * counterpart of the generator's #PaintLayersTreeBuilder::row_source_emit, which builds the same
 * thing as nodes. False when the row takes no part, which the generator leaves out as well.
 */
static bool composite_row_source_describe(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const int channel,
                                          const bool isolating,
                                          const std::shared_ptr<const PaintMaterialCompositeSurface>
                                              &context_surface,
                                          const int tile_number,
                                          PaintLayerSurfaceRef painted_object,
                                          PaintMaterialCompositeImageLayer &out)
{
  if (paint_layer_row_is_adjustment(layer)) {
    /* The filter of the stack below. Inside an isolating folder the generator leaves it out, as
     * the accumulation there is partly covered and pre-multiplied. */
    if (isolating || !paint_layer_adjustment_acts(ma, layer, channel)) {
      return false;
    }
    /* A NEEDS_BAKE kind computes itself against the row's own bake through the effect cache, which
     * an adjustment has no path to: a corrupted row of one is left out, not silently described with
     * the parameters the cache never sees -- those carry no colour. */
    BLI_assert(!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_NEEDS_BAKE));
    if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_NEEDS_BAKE)) {
      return false;
    }
    out.adjustment_filter = layer.kind_subtype;
    composite_filter_params(layer, out.adjustment_params);
    out.adjustment_ramp = BKE_paint_layers_ramp_get(layer);
    out.adjustment_curves = BKE_paint_layers_curves_get(layer);
    return true;
  }
  if (!paint_layer_row_participates(ma, layer, channel)) {
    return false;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
    /* The user's node group answers for the row's content, through the shader-bake's map. */
    return composite_node_group_source_describe(ma, layer, channel, tile_number, painted_object, out);
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES)) {
    if (layer.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP) {
      /* A node-group pattern has no CPU factor to evaluate: the CPU composite answers it from the
       * shader-bake instead, one bake of the full pattern emission over the map's UV, one map per
       * channel. The bake and the viewport render the same node chain -- the placement chain into
       * the row's tree, the `Fac` through the channel's ramp tail -- so the parity is by
       * construction; the describe reads the bake's map like any other. */
      return composite_node_group_generator_describe(
          ma, layer, channel, tile_number, painted_object, out);
    }
    /* A generator: no map and no constant, only its pattern, colour ramp and the channel's own
     * value ramp. A projected row reads its UV line through its own frame. */
    return composite_generator_describe(ma,
                                        layer,
                                        channel,
                                        context_surface,
                                        out.gradient,
                                        &layer,
                                        tile_number,
                                        painted_object);
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* The same decision the generator makes (#PaintLayersTreeBuilder::material_source_emit): the
     * source's plain value, its plain texture or the map rendered from it for the colour, and its
     * transparency, which is not the colour's alpha and so does not cover through it, as the
     * coverage. A plain-value coverage is folded into the opacity by the caller. */
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, layer, channel, PaintLayerMaterialUse::Composite);
    const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
        ma, layer, PaintLayerMaterialUse::Composite);
    if (source.kind == PaintLayerMaterialSourceKind::None ||
        coverage.kind == PaintLayerMaterialCoverageKind::Pending)
    {
      return false;
    }
    /* The source is read at the row's UV, the same transform the source wrapper and its coverage
     * chain apply in the generated graph. The mapping is described for the colour map and reused
     * for the source's separate coverage map; a constant colour still needs it when only the
     * coverage is a map. */
    const Image *mapping_image = (source.image != nullptr) ? source.image : coverage.image;
    composite_mapping_describe(
        ma, layer, channel, out.mapping, tile_number, painted_object, mapping_image);
    /* The row's transform effects move what the row itself lays: the source's maps go through
     * their steps first, the row's own mapping last, wherever the acquisition reads them. */
    BKE_paint_layers_input_transforms_get(
        ma, layer, channel, out.input_transforms, tile_number, painted_object);
    /* A clipping wrap leaves the map transparent outside the unit square, but a Material row
     * covers by its source's transparency rather than by the map's alpha, so the clip has to
     * reach the coverage instead. The acquisition folds it in; a Fill or painted row keeps its
     * usual clipping through the map's own alpha. */
    out.mapping_clips_coverage = out.mapping.enabled &&
                                 BKE_paint_layers_mapping_wrap_get(layer) !=
                                     MA_PAINT_LAYER_MAPPING_WRAP_REPEAT;
    if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
      copy_v4_v4(out.constant_color, source.constant);
      out.has_constant_color = true;
    }
    else {
      out.color_image = source.image;
      out.color_iuser = (source.kind == PaintLayerMaterialSourceKind::LiveImage) ? source.iuser :
                                                                                   nullptr;
    }
    out.color_alpha_coverage = false;
    if (coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha ||
        coverage.kind == PaintLayerMaterialCoverageKind::Baked)
    {
      out.mask_image = coverage.image;
      out.mask_iuser = (coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha) ?
                           coverage.iuser :
                           nullptr;
      out.mask_from_alpha = coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha;
      out.mask_influence = 1.0f;
    }
    return true;
  }
  if (!paint_layer_channel_contributes(ma, layer, paint_layer_channel_find(layer, channel)) &&
      paint_layer_effects_derive(ma, layer, channel))
  {
    /* No normal of its own: the row takes part for a Height To Normal, which replaces this flat
     * tangent-space "up" with the normal of the Height. The row covers whole, as a Fill does. */
    const float flat_normal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
    copy_v4_v4(out.constant_color, flat_normal);
    out.has_constant_color = true;
    /* The row has no map to size the stack by: the Height the effect reads is its canvas. */
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (!paint_layer_effect_reads_height(effect) ||
          !paint_layer_effect_filters(ma, effect, channel))
      {
        continue;
      }
      PaintMaterialCompositeImageLayer input;
      int input_width = 0;
      int input_height = 0;
      if (BKE_paint_layers_effect_input_describe(
              ma,
              layer,
              effect,
              channel,
              /*dimensions_only=*/true,
              /*describe_only=*/false,
              input,
              PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}) &&
          BKE_paint_material_composite_stack_dimensions(
              Span<PaintMaterialCompositeImageLayer>(&input, 1), input_width, input_height))
      {
        out.empty_base_size = input_width;
        break;
      }
    }
    return true;
  }
  if (paint_layer_row_empty_base(ma, layer, channel)) {
    /* Only its corrections lay here: its own constant covers nothing, so the row starts from zero
     * coverage and the corrections bring it in. */
    out.has_constant_color = true;
    out.empty_base = true;
    /* The row has no map to size the stack by: a correction that reads a slot image is the canvas,
     * at the image's own size. */
    for (const MaterialPaintLayer &effect : layer.effects) {
      int slot_width = 0;
      int slot_height = 0;
      if (paint_layer_effect_filters(ma, effect, channel) &&
          paint_layer_effect_slot_size(effect, tile_number, slot_width, slot_height))
      {
        out.empty_base_size = slot_width;
        break;
      }
      /* A kind that reads another channel of the row (a Normal to Mask) is sized by that input:
       * the canvas is the Normal the row lays, as the Height is a Height To Normal's. */
      if (paint_layer_effect_reads_channel(effect) >= 0 &&
          paint_layer_effect_filters(ma, effect, channel))
      {
        PaintMaterialCompositeImageLayer input;
        int input_width = 0;
        int input_height = 0;
        if (BKE_paint_layers_effect_input_describe(
                ma,
                layer,
                effect,
                channel,
                /*dimensions_only=*/true,
                /*describe_only=*/false,
                input,
                PaintLayerTileContext{.painted_object = painted_object,
                                      .tile_number = tile_number}) &&
            BKE_paint_material_composite_stack_dimensions(
                Span<PaintMaterialCompositeImageLayer>(&input, 1), input_width, input_height))
        {
          out.empty_base_size = input_width;
          break;
        }
      }
    }
    return true;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* The material's shared atlas, already in the stack's UV space: no Mapping, and its alpha is
     * data rather than painted coverage. */
    Image *atlas = paint_layer_mesh_map_image(ma, layer);
    if (atlas == nullptr) {
      return false;
    }
    out.color_image = atlas;
    out.color_iuser = nullptr;
    out.color_alpha_coverage = false;
    out.is_mesh_map = true;
    out.is_mesh_map_scalar = paint_layer_mesh_map_is_scalar(layer.mesh_map_type);
    return true;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (record->image != nullptr) {
    out.color_image = record->image;
    /* The description carries no per-node #ImageUser; the default one is what a plain map uses. */
    out.color_iuser = nullptr;
    /* A fresh paint map is transparent where nothing was painted; that texel must show the rows
     * below rather than cover them with the map's black. An encoding that reads the alpha as
     * data (the spec-gloss packing) takes the coverage away: a glossy texel must not make the
     * row transparent. The conversion itself happens where the map is read. */
    out.color_alpha_coverage = !paint_layer_map_alpha_is_data(record);
    out.normal_directx = channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
                         (record->flag & MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) != 0;
    out.map_encoding = int8_t(paint_layer_map_encoding_get(*record));
    composite_mapping_describe(
        ma, layer, channel, out.mapping, tile_number, painted_object, record->image);
    /* The same for a map of the row's own: a painted buffer or a Fill's image reads through the
     * transform effects' steps first, the row's own mapping last. */
    BKE_paint_layers_input_transforms_get(
        ma, layer, channel, out.input_transforms, tile_number, painted_object);
    return true;
  }
  /* A channel with no map: a flat value (it has some alpha, or the row would not take part). */
  copy_v4_v4(out.constant_color, record->value);
  out.has_constant_color = true;
  return true;
}

/** What every mask item description of one #composite_mask_items_describe call shares. */
struct MaskItemDescribeContext {
  const Material &ma;
  const MaterialPaintLayer &layer;
  const int tile_number;
  const int *slice_rect;
  const bool dimensions_only;
  const bool describe_only;
  const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface;
  PaintLayerSurfaceRef painted_object;
};

static CompositeBlend composite_mask_item_blend(const MaterialPaintLayer &mask)
{
  return CompositeBlend(BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
}

/** A mask item that reads an Anchor Point slice. */
static void composite_mask_item_anchor_point(const MaskItemDescribeContext &context,
                                             const MaterialPaintLayer &mask,
                                             Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  const Material &ma = context.ma;
  /* The slice a mask item reads: a mask point gives its scalar, a content point its coverage.
   * Both are grey buffers in canvas space, read like a painted map without a mapping. */
  if (!paint_layer_mask_item_lays(ma, mask)) {
    return;
  }
  PaintMaterialCompositeMaskItem item;
  item.opacity = BKE_paint_layers_effective_opacity(mask);
  item.contrast = BKE_paint_layers_mask_contrast_get(mask);
  item.anchor_point_reader = mask.marker;
  const PaintLayerAnchorPointRef mask_ref = paint_layer_anchor_point_resolve(ma, mask);
  item.anchor_point_target = int8_t(mask_ref.target);
  item.anchor_point_point = (mask_ref.point != nullptr) ? mask_ref.point->marker : bUUID{};
  const int read_channel = paint_layer_anchor_point_mask_read_channel(ma, mask);
  item.anchor_point_read_channel = read_channel;
  const bool mask_target = item.anchor_point_target == int8_t(PaintLayerAnchorPointTarget::Mask);
  const bool baked = paint_layer_anchor_point_reader_baked(ma, mask) && mask.bake != nullptr &&
                     (mask_target ? mask.bake->images[read_channel] != nullptr :
                                    mask.bake->coverage != nullptr);
  if (baked) {
    /* A mask point's scalar is the grey the bake holds in its colour map; a content point's
     * coverage is the coverage map, which already holds the reader's opacity, so only its
     * visibility stays live (as for a substituted row). Read like a painted grey map. */
    if (mask_target) {
      item.image = mask.bake->images[read_channel];
    }
    else {
      item.image = mask.bake->coverage;
      item.opacity = (mask.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f;
    }
    item.iuser = nullptr;
    item.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, mask, read_channel);
    BKE_paint_layers_anchor_point_slice_images(ma, mask, item.grey_input_images);
  }
  else {
    /* In describe-only mode the reader keeps its descriptor, hash and subscriptions; the
     * slice buffers belong to the GPU slot pass instead of the CPU cache. */
    if (!context.describe_only) {
      std::shared_ptr<ImBuf> slice_color;
      std::shared_ptr<ImBuf> slice_coverage;
      if (!BKE_paint_layers_slice_render(
              ma,
              mask,
              read_channel,
              &slice_color,
              &slice_coverage,
              context.slice_rect,
              context.dimensions_only,
              PaintLayerTileContext{.painted_object = context.painted_object,
                                    .tile_number = context.tile_number}))
      {
        return;
      }
      const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, mask);
      item.grey_buffer = (ref.target == PaintLayerAnchorPointTarget::Mask) ?
                             std::move(slice_color) :
                             std::move(slice_coverage);
    }
    item.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, mask, read_channel);
    BKE_paint_layers_anchor_point_slice_images(ma, mask, item.grey_input_images);
  }
  item.blend = composite_mask_item_blend(mask);
  item.invert = paint_layer_mask_item_inverted(mask);
  r_items.append(item);
}

/** A map-driven coverage operator. */
static void composite_mask_item_map_coverage(const MaskItemDescribeContext &context,
                                             const MaterialPaintLayer &mask,
                                             Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  /* A map-driven coverage operator: it lays its map's `match` over the coverage below,
   * whether or not it has a map yet (a missing map is `match = 0`, not a pass-through). Its
   * Base Color record is structural, so the map is read from it directly. */
  PaintMaterialCompositeMaskItem item;
  item.opacity = BKE_paint_layers_effective_opacity(mask);
  item.contrast = BKE_paint_layers_mask_contrast_get(mask);
  item.map_coverage = mask.kind_subtype;
  item.invert = paint_layer_mask_item_inverted(mask);
  item.coverage_tolerance = paint_layer_kind_param_get(mask, PAINT_LAYER_COVERAGE_TOLERANCE);
  item.coverage_softness = paint_layer_kind_param_get(mask, PAINT_LAYER_COVERAGE_SOFTNESS);
  item.coverage_output = BKE_paint_layers_mask_item_output_value(mask);
  if (paint_layer_map_coverage_has_map(mask)) {
    item.coverage_image = paint_layer_map_coverage_record(mask)->image;
    composite_mapping_describe(context.ma,
                               mask,
                               PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                               item.coverage_mapping,
                               context.tile_number,
                               context.painted_object,
                               item.coverage_image);
  }
  for (const MaterialPaintLayerIdColor &color : mask.id_colors) {
    PaintMaterialCompositeIdColor out_color;
    copy_v3_v3(out_color.color, color.color);
    out_color.tolerance = color.tolerance;
    out_color.own_tolerance = paint_layer_mask_item_color_id_advanced(mask);
    out_color.enabled = (color.flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0;
    item.coverage_colors.append(out_color);
  }
  r_items.append(item);
}

/** A filter mask item, or the CPU-computed kind that lays its own grey. */
/* Defined with the chain unfolding below; the mask-chain unfolding above it reads the same override
 * view, so the declaration stands here. */
static bool composite_chain_member_override(const IDProperty &system_properties,
                                            const MaterialPaintLayer &member,
                                            const PaintLayerKindInfo &member_kind,
                                            int index,
                                            float r_value[3]);

/**
 * Unfold the chain instance \a mask into the mask filters of its definition: each pointwise
 * member is appended as the ordinary mask filter it would be if the user had added it by hand,
 * folding the mask grey the members build up over, with the instance's exposed values read over
 * the member's own (the same override view the content form unfolds with). The definition gone
 * lays nothing, as every reader of a missing definition does; a member that fails the pointwise
 * test the Smart Filter portability saved it with is skipped.
 *
 * A CPU-computed member the relaxed contract lets into a chain (a Blur, a node-group filter) is
 * skipped here on purpose: the content form lays it as an effect correction whose result an effect
 * stage reads back, but a mask item has no effect stage of its own -- the mask pass has no result
 * texture to read, which is the same reason the refusal matrix keeps refusing CPU-computed mask
 * kinds -- and laying its kind as a pointwise mask filter would shade a blur as a no-op. The
 * save-time contract admits the shape, so this skip is the mask form's defensive half of it; a
 * definition with such a member still unfolds fully as a content correction.
 *
 * The chain item's own opacity scales every member's, so the item's strength is the strength of
 * the whole chain. Its contrast and invert ride the LAST member alone: both reshape the grey the
 * chain hands back, and the last member's output is that grey, so the answer is the chain's own
 * contrast and invert rather than one per member. A chain whose members all fail the test lays
 * nothing.
 */
static void composite_mask_item_chain(const MaskItemDescribeContext &context,
                                      const MaterialPaintLayer &mask,
                                      Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  const Material *definition = mask.material;
  if (definition == nullptr) {
    return;
  }
  const IDProperty *system_properties = (mask.system_properties != nullptr &&
                                         mask.system_properties->type == IDP_GROUP) ?
                                            mask.system_properties :
                                            nullptr;
  const float chain_opacity = BKE_paint_layers_effective_opacity(mask);
  const float chain_contrast = BKE_paint_layers_mask_contrast_get(mask);
  const bool chain_invert = paint_layer_mask_item_inverted(mask);
  int64_t last_appended = -1;
  for (const MaterialPaintLayer &member : definition->paint_layers) {
    if (!paint_layer_effect_is_smart_filter_member(member)) {
      continue;
    }
    if (paint_layer_effect_is_cpu_computed(member)) {
      /* A GPU-computed or node-group member has no mask-item answer: see the comment above. */
      continue;
    }
    const PaintLayerKindInfo *member_kind = paint_layer_kind_of(member);
    if (member_kind == nullptr) {
      continue;
    }
    PaintMaterialCompositeMaskItem filter;
    filter.filter = member.kind_subtype;
    composite_filter_params(member, filter.filter_params, [&](const int index, float r_value[3]) {
      return system_properties != nullptr &&
             composite_chain_member_override(
                 *system_properties, member, *member_kind, index, r_value);
    });
    filter.filter_ramp = BKE_paint_layers_ramp_get(member);
    filter.filter_curves = BKE_paint_layers_curves_get(member);
    filter.opacity = BKE_paint_layers_effective_opacity(member) * chain_opacity;
    r_items.append(filter);
    last_appended = int64_t(r_items.size()) - 1;
  }
  if (last_appended >= 0) {
    /* The chain's own reshape of the grey it hands back, exactly once: contrast and invert after
     * the last member's filter are the contrast and invert of the chain's answer. */
    r_items[last_appended].contrast = chain_contrast;
    r_items[last_appended].invert = chain_invert;
  }
}

static void composite_mask_item_filter(const MaskItemDescribeContext &context,
                                       const MaterialPaintLayer &mask,
                                       Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  const Material &ma = context.ma;
  /* A filter changes the coverage so far; it owns no map or grey and no record. Its contrast
   * still applies: the graph wraps the filter's result in the same chain. A CPU-computed
   * kind lays its own grey instead: the mask effect cache computes the mask built up to the
   * item with the kernel over it, and the item reads like a painted map with its invert, blend
   * and opacity. In describe-only the grey is left out and the kind named, so the combined
   * preview refuses the stack instead of shading the item as a no-op. */
  if (paint_layer_kind_transforms_input(mask) && !paint_layer_mask_item_bakes_effect(mask)) {
    /* A transform that moves nothing leaves the coverage as it is. */
    return;
  }
  if (paint_layer_mask_item_bakes_effect(mask)) {
    PaintMaterialCompositeMaskItem effect;
    effect.opacity = BKE_paint_layers_effective_opacity(mask);
    effect.contrast = BKE_paint_layers_mask_contrast_get(mask);
    effect.blend = composite_mask_item_blend(mask);
    effect.invert = paint_layer_mask_item_inverted(mask);
    effect.effect_filter = mask.kind_subtype;
    /* A fresh bake stands in, the way a substituted row's does: its map is read like a painted
     * item's, the item's own mapping over it. Without one the grey is computed through the mask
     * effect cache from the mask built up to the item. */
    if (Image *bake_map = paint_layer_mask_effect_bake_map(ma, mask)) {
      effect.image = bake_map;
      composite_mapping_describe(ma,
                                 mask,
                                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                 effect.mapping,
                                 context.tile_number,
                                 context.painted_object,
                                 effect.image);
    }
    else {
      effect.grey_buffer = paint_layer_mask_effect_buffer_get(ma,
                                                             context.layer,
                                                             mask,
                                                             context.describe_only,
                                                             &effect.grey_input_images,
                                                             context.tile_number,
                                                             context.painted_object);
      if (!context.describe_only && effect.grey_buffer == nullptr) {
        /* The input could not be rendered: the item is left out, as the generator leaves a
         * filter it cannot express out of the live graph. */
        return;
      }
    }
    r_items.append(effect);
    return;
  }
  PaintMaterialCompositeMaskItem filter;
  filter.opacity = BKE_paint_layers_effective_opacity(mask);
  filter.contrast = BKE_paint_layers_mask_contrast_get(mask);
  filter.filter = mask.kind_subtype;
  composite_filter_params(mask, filter.filter_params);
  filter.filter_ramp = BKE_paint_layers_ramp_get(mask);
  filter.filter_curves = BKE_paint_layers_curves_get(mask);
  r_items.append(filter);
}

/** A generator mask item. */
static void composite_mask_item_generator(const MaskItemDescribeContext &context,
                                          const MaterialPaintLayer &mask,
                                          Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  PaintMaterialCompositeMaskItem item;
  item.opacity = BKE_paint_layers_effective_opacity(mask);
  item.contrast = BKE_paint_layers_mask_contrast_get(mask);
  if (mask.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP) {
    /* A node-group pattern has no CPU factor to evaluate, so the item carries the shader-bake's
     * answer instead: the map of a live record of the item, read as the generator mask item's
     * grey and coverage. Base Color when the item has it (the default add lays it), otherwise the
     * first record the mask keeps -- a narrowed mask correction may carry only, say, Roughness,
     * and asking Base Color would silently drop the item. The item is laid by its blend, invert
     * and opacity like any painted item's, which the combined preview lays too (no gradient
     * payload to refuse). */
    int bake_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    if (!paint_layer_channel_contributes(context.ma,
                                         mask,
                                         paint_layer_channel_find(mask, bake_channel)))
    {
      for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
        if (paint_layer_channel_contributes(
                context.ma, mask, paint_layer_channel_find(mask, channel)))
        {
          bake_channel = channel;
          break;
        }
      }
    }
    Image *bake = composite_node_group_bake_image(context.ma,
                                                  mask,
                                                  bake_channel,
                                                  context.tile_number,
                                                  context.painted_object);
    if (bake == nullptr) {
      return;
    }
    item.image = bake;
    item.iuser = nullptr;
    /* The bake already carries the placement, like a source row's map; the item's mapping stays
     * identity so the pattern is not moved a second time (see the row form). */
    item.blend = composite_mask_item_blend(mask);
    item.invert = paint_layer_mask_item_inverted(mask);
    r_items.append(item);
    return;
  }
  /* A generator mask item: the ramp's colour is the grey and its alpha the coverage, read from
   * the item's own pattern rather than from a map. A UV line follows the row's frame when the
   * row projects, so a decal's gradient mask moves with its frame. */
  if (!composite_generator_describe(context.ma,
                                    mask,
                                    PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                    context.context_surface,
                                    item.gradient,
                                    &context.layer,
                                    context.tile_number,
                                    context.painted_object))
  {
    return;
  }
  item.blend = composite_mask_item_blend(mask);
  item.invert = paint_layer_mask_item_inverted(mask);
  r_items.append(item);
}

/** A mask correction that reads a material. */
static void composite_mask_item_reads_source(const MaskItemDescribeContext &context,
                                             const MaterialPaintLayer &mask,
                                             Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  const Material &ma = context.ma;
  /* The grey of a mask correction that reads a material is the mean of the source's Base
   * Color, read as a painted item's map is: a plain value, a plain texture or the map rendered
   * from it. A graph with no map yet is the generator's to show alone, so nothing is carried
   * until the render lands. A plain transparency folds into the opacity. */
  if (!paint_layer_mask_item_lays(ma, mask)) {
    return;
  }
  const int source_channel = paint_layer_mask_source_channel(mask);
  const PaintLayerMaterialSource source = paint_layer_material_channel_source(
      ma, mask, source_channel, PaintLayerMaterialUse::Composite);
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
      ma, mask, PaintLayerMaterialUse::Composite);
  if (source.kind == PaintLayerMaterialSourceKind::None ||
      coverage.kind == PaintLayerMaterialCoverageKind::Pending)
  {
    return;
  }
  PaintMaterialCompositeMaskItem item;
  item.opacity = BKE_paint_layers_effective_opacity(mask);
  item.contrast = BKE_paint_layers_mask_contrast_get(mask);
  if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
    item.constant_gray = (source.constant[0] + source.constant[1] + source.constant[2]) / 3.0f;
  }
  else {
    item.image = source.image;
    item.iuser = (source.kind == PaintLayerMaterialSourceKind::LiveImage) ? source.iuser :
                                                                            nullptr;
    /* Read at the item's UV like a painted item's map is, so the Mapping and its gizmo move
     * the mask in the Image Editor's composite too. */
    composite_mapping_describe(ma,
                               mask,
                               source_channel,
                               item.mapping,
                               context.tile_number,
                               context.painted_object,
                               (source.image != nullptr) ? source.image : coverage.image);
  }
  if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
    item.opacity *= clamp_f(coverage.constant, 0.0f, 1.0f);
  }
  item.blend = composite_mask_item_blend(mask);
  item.invert = paint_layer_mask_item_inverted(mask);
  r_items.append(item);
}

/** A mask item that reads the material's shared mesh-map atlas. */
static void composite_mask_item_mesh_map(const MaskItemDescribeContext &context,
                                         const MaterialPaintLayer &mask,
                                         Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  /* The atlas' R is the grey; it is read with Extend, no Mapping, and its alpha is data. */
  if (!paint_layer_mask_item_lays(context.ma, mask)) {
    return;
  }
  Image *atlas = paint_layer_mesh_map_image(context.ma, mask);
  if (atlas == nullptr) {
    return;
  }
  PaintMaterialCompositeMaskItem item;
  item.opacity = BKE_paint_layers_effective_opacity(mask);
  item.image = atlas;
  item.is_mesh_map = true;
  item.is_mesh_map_scalar = paint_layer_mesh_map_is_scalar(mask.mesh_map_type);
  item.blend = composite_mask_item_blend(mask);
  item.invert = paint_layer_mask_item_inverted(mask);
  r_items.append(item);
}

/** A painted mask item: its Base Color map, or its flat grey. */
static void composite_mask_item_painted(const MaskItemDescribeContext &context,
                                        const MaterialPaintLayer &mask,
                                        Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  const MaterialPaintLayerChannel *mask_record = paint_layer_channel_find(
      mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (mask_record == nullptr || !paint_layer_mask_item_lays(mask)) {
    return;
  }
  PaintMaterialCompositeMaskItem item;
  item.opacity = BKE_paint_layers_effective_opacity(mask);
  item.contrast = BKE_paint_layers_mask_contrast_get(mask);
  item.image = mask_record->image;
  item.iuser = nullptr;
  item.constant_gray = paint_layer_mask_constant(*mask_record);
  item.blend = composite_mask_item_blend(mask);
  item.invert = paint_layer_mask_item_inverted(mask);
  if (item.image != nullptr) {
    composite_mapping_describe(context.ma,
                               mask,
                               PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                               item.mapping,
                               context.tile_number,
                               context.painted_object,
                               item.image);
  }
  r_items.append(item);
}

/**
 * Describe \a layer's mask stack to the evaluator, bottom to top: the base first, then the
 * corrections over it in the order they were added. One that lays nothing yet is not carried, as
 * the generator builds nothing for it. The mask is one for all channels of the row, so the same
 * description reaches every channel's stack.
 *
 * The one place the description is read for the mask, so the CPU composite and the strokes that
 * sample the mask themselves (#PaintLayersMaskSampler) cannot drift apart.
 *
 * \param hidden_with_base: the base's switch carries the whole stack -- with it off, no item of the
 * stack lays and the mask reads as one, the same mix-out the graph's gate applies over the chain
 * (see #PaintLayersTreeBuilder::mask_factor_ensure). The items themselves are left untouched, so
 * switching the base back restores every correction at once. A straight slice read passes false: a
 * tap publishes the stack as it stands, independent of the row's visibility.
 */
static void composite_mask_items_describe(
    const Material &ma,
    const MaterialPaintLayer &layer,
    Vector<PaintMaterialCompositeMaskItem> &r_items,
    const int tile_number,
    const bUUID *stop_before = nullptr,
    const int *slice_rect = nullptr,
    const bool dimensions_only = false,
    const bool describe_only = false,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface = nullptr,
    const bool hidden_with_base = true,
    PaintLayerSurfaceRef painted_object = nullptr)
{
  const MaterialPaintLayer *base = BKE_paint_layers_mask_base(layer);
  if (base == nullptr) {
    return;
  }
  if (hidden_with_base && (base->flag & MA_PAINT_LAYER_ENABLED) == 0) {
    return;
  }
  const MaskItemDescribeContext context{ma,
                                        layer,
                                        tile_number,
                                        slice_rect,
                                        dimensions_only,
                                        describe_only,
                                        context_surface,
                                        painted_object};
  for (const MaterialPaintLayer &mask : layer.mask_stack) {
    if (stop_before != nullptr && BLI_uuid_equal(mask.marker, *stop_before)) {
      break;
    }
    if (paint_layer_mask_item_bakes_effect(mask) &&
        (paint_layer_effect_is_filter(mask) || paint_layer_kind_transforms_input(mask)))
    {
      /* A CPU-computed effect as a mask item: the grey comes from the mask effect cache, whatever
       * else the item reads. The branch stands ahead of the Anchor Point one, because a Normal to
       * Mask is both a reader (its grey's input is the slice it references, which the cache's
       * compute renders) and a baked effect: the anchor branch would lay the slice's coverage, a
       * grey the kernel never made. A plain reader is neither a filter nor a transform and falls
       * through to it as before. */
      composite_mask_item_filter(context, mask, r_items);
    }
    else if (paint_layer_kind_reads_anchor_point(mask)) {
      composite_mask_item_anchor_point(context, mask, r_items);
    }
    else if (mask.kind_subtype == MA_PAINT_LAYER_FILTER_CHAIN) {
      /* The chain unfolds into the mask filters of its definition, ahead of the plain filter
       * branch, which would describe it as one filter of a kind no branch answers. */
      composite_mask_item_chain(context, mask, r_items);
    }
    else if (paint_layer_kind_is_map_coverage(mask)) {
      composite_mask_item_map_coverage(context, mask, r_items);
    }
    else if (paint_layer_effect_is_filter(mask) || paint_layer_kind_transforms_input(mask)) {
      composite_mask_item_filter(context, mask, r_items);
    }
    else if (paint_layer_kind_has_flag(mask, PAINT_LAYER_KIND_GENERATES)) {
      composite_mask_item_generator(context, mask, r_items);
    }
    else if (paint_layer_kind_has_flag(mask, PAINT_LAYER_KIND_READS_SOURCE)) {
      composite_mask_item_reads_source(context, mask, r_items);
    }
    else if (mask.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
      composite_mask_item_mesh_map(context, mask, r_items);
    }
    else {
      composite_mask_item_painted(context, mask, r_items);
    }
  }
}

/**
 * The mask of \a row, rendered once per build pass when \a cache says it is shared.
 *
 * A null mask is cached as well: "this row cannot be compared" is as stable an answer as a
 * rendered one, and re-deriving it for every channel would pay the description walk each time.
 */
static std::shared_ptr<ImBuf> composite_height_won_get(
    const Material &ma,
    const ListBaseT<MaterialPaintLayer> &prefix,
    const MaterialPaintLayer &row,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    const bool isolating,
    PaintLayersHeightWonCache *cache,
    const int tile_number)
{
  if (cache == nullptr) {
    return composite_height_won_render(
        ma, prefix, row, context_surface, isolating, nullptr, tile_number);
  }
  const PaintLayersHeightWonKey key{&row, isolating, tile_number};
  if (const std::shared_ptr<ImBuf> *found = cache->masks.lookup_ptr(key)) {
    return *found;
  }
  std::shared_ptr<ImBuf> mask = composite_height_won_render(
      ma, prefix, row, context_surface, isolating, cache, tile_number);
  cache->masks.add_new(key, mask);
  return mask;
}

static bool anchor_slice_dimensions(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    const int channel,
                                    const PaintLayerAnchorPointRef &ref,
                                    int &r_width,
                                    int &r_height,
                                    PaintLayerSurfaceRef painted_object,
                                    int tile_number);

/** The effect cache, defined beside the slice cache it shares the machinery of; the hash is
 * declared with the key in #paint_layers_effect_cache_intern.hh. */
static std::shared_ptr<ImBuf> paint_layer_effect_buffer_get(const Material &ma,
                                                            const MaterialPaintLayer &row,
                                                            const MaterialPaintLayer &effect,
                                                            const int channel,
                                                            const int tile_number,
                                                            const bool describe_only,
                                                            Vector<Image *> *r_images,
                                                            Image **r_bake_image = nullptr,
                                                            PaintLayerSurfaceRef painted_object =
                                                                nullptr,
                                                            const rcti *changed_region = nullptr,
                                                            rcti *r_valid_region = nullptr,
                                                            const rcti *compute_region = nullptr,
                                                            int *r_effect_origin = nullptr,
                                                            bool *r_effect_cropped = nullptr);

/** The chain member's twin, defined beside it: the same cache with the prefix-aware input and the
 * composite-marker key the unfolding of a GPU-computed member keys its correction on. */
static std::shared_ptr<ImBuf> paint_layer_chain_member_effect_buffer_get(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &chain,
    const MaterialPaintLayer &member,
    const int channel,
    const int tile_number,
    const bool describe_only,
    Vector<Image *> *r_images,
    PaintLayerSurfaceRef painted_object = nullptr);

/**
 * Append the CPU-computed effect \a effect to \a out as a correction carrying the map the effect
 * cache computed, or the map of the effect's own bake when it owns its result and that bake stands.
 * Nothing is appended while the effect lays nothing or its input cannot be rendered.
 */
static void composite_effect_append_cpu_computed(const Material &ma,
                                                 const MaterialPaintLayer *owner_row,
                                                 const MaterialPaintLayer &effect,
                                                 const int channel,
                                                 const float pass_through_scale,
                                                 const int *slice_rect,
                                                 const bool dimensions_only,
                                                 const int tile_number,
                                                 const bool describe_only,
                                                 PaintMaterialCompositeImageLayer &out,
                                                 PaintLayerSurfaceRef painted_object)
{
  /* A CPU-computed effect reads the row's own content, so it needs the row it hangs on: the
   * standalone corrections-row description has none and leaves it out. */
  if (owner_row == nullptr || !paint_layer_effect_filters(ma, effect, channel)) {
    return;
  }
  /* A CPU-computed effect carries its result as a map the effect cache computed: the kind and
   * the parameters travel like a filter's, the buffer beside them. In describe_only only the
   * kind is known -- the buffer is not computed, and the combined preview falls back to the CPU
   * when it sees the kind. */
  PaintMaterialCompositeCorrection effect_correction;
  effect_correction.filter = effect.kind_subtype;
  composite_filter_params(effect, effect_correction.filter_params);
  effect_correction.effect_hash = paint_layer_effect_hash(effect, channel);
  /* The GPU path keys its result texture on the effect instance, which a parameter hash cannot do
   * across rows: name the marker the correction was made from. */
  effect_correction.effect_marker = effect.marker;
  effect_correction.effect_result_has_alpha = paint_layer_kind_result_has_alpha(effect);
  /* A bake render that knows the canvas rectangle it needs offers it: the folded effect may then
   * compute that rectangle grown by its halo and hand the cropped buffer over instead of a
   * whole-canvas one. Null for a full render, the preview and every nested input description. */
  rcti effect_region;
  const rcti *compute_region = nullptr;
  if (slice_rect != nullptr) {
    /* `slice_rect` is {xmin, ymin, xmax, ymax}; `BLI_rcti_init` takes the axes paired. */
    BLI_rcti_init(&effect_region, slice_rect[0], slice_rect[2], slice_rect[1], slice_rect[3]);
    compute_region = &effect_region;
  }
  int effect_origin[2] = {0, 0};
  bool effect_cropped = false;
  /* A dimensions-only pass learns sizes and keys, never pixels: the effect's buffer would be the
   * whole canvas, and a bake render only narrows it later. Describing the correction without its
   * buffer keeps its kind, parameters and hashes in the entry the key folds. */
  const bool compute_buffer = !describe_only && !dimensions_only;
  /* An effect that owns its result reads the map of its bake while that stands: switching it off and
   * on moves no pixel, and a stroke elsewhere costs it no recompute. */
  {
    /* Which description path pays for a CPU effect: a row's content bake or the live composite.
     * Only under `BLENDER_PBR_PERF`, so the dedicated label costs nothing in production. */
    bke::paint_perf::ScopedTimer perf_effect("effect.cpu.row_content");
    effect_correction.effect_color = paint_layer_effect_buffer_get(
        ma,
        *owner_row,
        effect,
        channel,
        tile_number,
        !compute_buffer,
        &effect_correction.effect_images,
        paint_layer_effect_owns_result(ma, *owner_row, effect) ? &effect_correction.effect_image :
                                                                 nullptr,
        painted_object,
        nullptr,
        nullptr,
        compute_region,
        effect_origin,
        &effect_cropped);
    perf_effect.note(
        "kind=%d channel=%d describe=%d", int(effect.kind_subtype), channel, describe_only ? 1 : 0);
  }
  if (compute_buffer && effect_correction.effect_color == nullptr &&
      effect_correction.effect_image == nullptr)
  {
    /* The input could not be rendered: the correction is left out, as the generator leaves it
     * out of the live graph. */
    return;
  }
  /* A cropped result names where its (0, 0) sits; a whole-canvas one keeps the default zero. The
   * flag rides beside it, since a crop that happens to start at the canvas origin has a zero
   * origin too. */
  effect_correction.effect_origin[0] = effect_origin[0];
  effect_correction.effect_origin[1] = effect_origin[1];
  effect_correction.effect_cropped =
      compute_buffer && effect_correction.effect_color != nullptr && effect_cropped;
  effect_correction.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                              pass_through_scale;
  out.content_corrections.append(effect_correction);
}

/**
 * Describe an Anchor Point reader correction into \a correction. False when the correction is left
 * out because its slice cannot be rendered. The slice is the one of \a tile_number: every map of
 * the source row is read on that tile.
 */
static bool composite_effect_anchor_point_describe(
    const Material &ma,
    const MaterialPaintLayer &effect,
    const int channel,
    const float pass_through_scale,
    const int *slice_rect,
    const bool dimensions_only,
    const bool describe_only,
    const int tile_number,
    PaintLayerSurfaceRef painted_object,
    PaintMaterialCompositeCorrection &correction)
{
  correction.anchor_point_reader = effect.marker;
  const PaintLayerAnchorPointRef effect_ref = paint_layer_anchor_point_resolve(ma, effect);
  correction.anchor_point_target = int8_t(effect_ref.target);
  correction.anchor_point_point = (effect_ref.point != nullptr) ? effect_ref.point->marker :
                                                                    bUUID{};
  /* What the slice shows, laid by the reader's own blend and opacity. A baked reader reads the
   * maps its slice was rendered into. Only the row whose bake is being rendered ignores its own
   * bake; a correction nested in it describes its own bake, which is valid and cheaper than
   * rendering its slice live again. */
  const bool baked = paint_layer_anchor_point_reader_baked(ma, effect) && effect.bake != nullptr &&
                     effect.bake->images[channel] != nullptr && effect.bake->coverage != nullptr;
  if (baked) {
    correction.image = effect.bake->images[channel];
    correction.iuser = nullptr;
    correction.anchor_point_coverage_image = effect.bake->coverage;
    correction.anchor_point_coverage_iuser = nullptr;
    correction.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, effect, channel);
    /* The coverage map already holds the reader's opacity; only its visibility stays live. */
    correction.opacity = ((effect.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                         pass_through_scale;
    return true;
  }
  if (!describe_only && !BKE_paint_layers_slice_render(ma,
                                                       effect,
                                                       channel,
                                                       &correction.grey_buffer,
                                                       &correction.anchor_point_coverage,
                                                       slice_rect,
                                                       dimensions_only,
                                                       PaintLayerTileContext{.painted_object =
                                                                                 painted_object,
                                                                             .tile_number =
                                                                                 tile_number}))
  {
    return false;
  }
  correction.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, effect, channel);
  BKE_paint_layers_anchor_point_slice_images(ma, effect, correction.grey_input_images);
  return true;
}

/**
 * Describe a correction that reads a material into \a correction: what the source supplies, as a
 * Material row's colour. False while there is nothing to carry yet.
 */
static bool composite_effect_source_describe(const Material &ma,
                                             const MaterialPaintLayer &effect,
                                             const int channel,
                                             const int tile_number,
                                             PaintLayerSurfaceRef painted_object,
                                             PaintMaterialCompositeCorrection &correction)
{
  /* What the source supplies, as a Material row's colour: its plain value, its plain texture
   * or the map rendered from it. A graph with no map yet is the generator's to show alone, so
   * nothing is carried until the render lands. The correction's transparency is a plain value
   * or none (see #paint_layer_correction_reads_source), and a value folds into the opacity. */
  const PaintLayerMaterialSource source = paint_layer_material_channel_source(
      ma, effect, channel, PaintLayerMaterialUse::Composite);
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
      ma, effect, PaintLayerMaterialUse::Composite);
  if (source.kind == PaintLayerMaterialSourceKind::None ||
      coverage.kind == PaintLayerMaterialCoverageKind::Pending)
  {
    return false;
  }
  if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
    copy_v4_v4(correction.constant_color, source.constant);
    correction.has_constant_color = true;
  }
  else {
    correction.image = source.image;
    correction.iuser = (source.kind == PaintLayerMaterialSourceKind::LiveImage) ? source.iuser :
                                                                                  nullptr;
    /* The source is read at the correction's UV, as the generator's source wrapper does and as
     * a Material row's map is: the Image Editor's composite and the gizmo go through this. The
     * image is handed over too, so a projected canvas correction with FIT_ASPECT takes the
     * canvas's own width and height, the way the generated graph reads its map. */
    composite_mapping_describe(
        ma, effect, channel, correction.mapping, tile_number, painted_object, source.image);
  }
  if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
    correction.opacity *= clamp_f(coverage.constant, 0.0f, 1.0f);
  }
  return true;
}

/** Describe a plain correction into \a correction: a mesh map, a painted map or a flat colour. */
static bool composite_effect_map_describe(const Material &ma,
                                          const MaterialPaintLayer &effect,
                                          const int channel,
                                          const int tile_number,
                                          PaintLayerSurfaceRef painted_object,
                                          PaintMaterialCompositeCorrection &correction)
{
  const MaterialPaintLayerChannel *effect_record = paint_layer_channel_find(effect, channel);
  if (effect.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* A mesh-map correction reads the material's shared atlas; no Mapping, alpha is data. */
    Image *atlas = paint_layer_mesh_map_image(ma, effect);
    if (atlas == nullptr) {
      return false;
    }
    correction.image = atlas;
    correction.is_mesh_map = true;
    correction.is_mesh_map_scalar = paint_layer_mesh_map_is_scalar(effect.mesh_map_type);
  }
  else if (effect_record->image != nullptr) {
    correction.image = effect_record->image;
    composite_mapping_describe(ma, effect, channel, correction.mapping, tile_number, painted_object);
  }
  else {
    /* Only a Fill correction lays without a map: its flat colour. */
    copy_v4_v4(correction.constant_color, effect_record->value);
    correction.has_constant_color = true;
  }
  if (effect.source == MA_PAINT_LAYER_SOURCE_TEXT && effect.role == MA_PAINT_LAYER_ROLE_EFFECT &&
      effect.text_decal != nullptr && effect.text_decal->coverage != nullptr)
  {
    /* The text is only the decal's mask: it scales whatever the channel lays, flat or textured.
     * The mapping is described even for a flat value, as the coverage is read through it. */
    correction.coverage_image = effect.text_decal->coverage;
    correction.coverage_iuser = nullptr;
    if (effect_record->image == nullptr) {
      composite_mapping_describe(ma,
                                 effect,
                                 channel,
                                 correction.mapping,
                                 tile_number,
                                 painted_object,
                                 correction.coverage_image);
    }
  }
  return true;
}

/**
 * The instance's stored override of the member parameter at \a index of \a member_kind, read out of
 * the chain row's system properties: the key the dynamic descriptor names
 * (#paint_layer_chain_member_param_key) selects the property, whose shape mirrors what the member's
 * own accessors store -- a float for a Float, an int for an Int or a choice, a boolean for a switch,
 * a float array of four (or three) for a Color or a Vector. Anything else -- no property, a payload
 * parameter (the ramp, the curves, the balance, the mixer), a stale value of the wrong shape -- is
 * no override at all: the member's own value stands.
 */
static bool composite_chain_member_override(const IDProperty &system_properties,
                                            const MaterialPaintLayer &member,
                                            const PaintLayerKindInfo &member_kind,
                                            const int index,
                                            float r_value[3])
{
  if (index < 0 || index >= member_kind.params_num) {
    return false;
  }
  const PaintLayerKindParam &param = member_kind.params[index];
  switch (param.type) {
    case PaintLayerKindParamType::Ramp:
    case PaintLayerKindParamType::Curves:
    case PaintLayerKindParamType::ColorBalance:
    case PaintLayerKindParamType::ChannelMixer:
      /* Payload parameters do not travel to the instance (v1): their values are the member's own,
       * and the unfolding passes the payloads through whole beside this hook. */
      return false;
    default:
      break;
  }
  const std::string key = paint_layer_chain_member_param_key(member.marker, param.name);
  const IDProperty *prop = IDP_GetPropertyFromGroup(&system_properties, key.c_str());
  if (prop == nullptr) {
    return false;
  }
  switch (param.type) {
    case PaintLayerKindParamType::Int:
    case PaintLayerKindParamType::Enum:
      if (prop->type != IDP_INT) {
        return false;
      }
      r_value[0] = float(IDP_int_get(prop));
      return true;
    case PaintLayerKindParamType::Bool:
      if (prop->type != IDP_BOOLEAN) {
        return false;
      }
      r_value[0] = IDP_bool_get(prop) ? 1.0f : 0.0f;
      return true;
    case PaintLayerKindParamType::Color:
      if (prop->type != IDP_ARRAY || prop->subtype != IDP_FLOAT || prop->len != 4) {
        return false;
      }
      copy_v3_v3(r_value, IDP_array_float_get(prop));
      return true;
    case PaintLayerKindParamType::Vector:
      if (prop->type != IDP_ARRAY || prop->subtype != IDP_FLOAT || prop->len != 3) {
        return false;
      }
      copy_v3_v3(r_value, IDP_array_float_get(prop));
      return true;
    default:
      /* A Float, the only remaining type: the seed writes the same IDP_FLOAT the member's own
       * setter does, so a property of another shape is a stale one and reads as absent. */
      if (prop->type != IDP_FLOAT) {
        return false;
      }
      r_value[0] = IDP_float_get(prop);
      return true;
  }
}

/**
 * The composite marker of the chain member \a member of the chain instance \a chain: a fresh
 * #bUUID built deterministically from the two rows' own markers, so the correction a GPU-computed
 * member lays as an effect correction is stable per (instance, member) -- the cache keys survive
 * the description being rebuilt every frame -- and unique per instance, because the chain row's
 * marker is one of the two inputs and no two rows of a material share one. Two instances of the
 * same asset in one material therefore key apart (each carries its own chain row), while one
 * instance re-asks across frames and across channels for the same marker the cache filed the
 * answer under.
 *
 * The construction is the task's contract: the two markers' packed bytes are folded by two
 * independent FNV-1a walks (a plain one and one over a salted stream, so a collision in one fold
 * is not a collision in the marker), and the two 64-bit answers lay out the #bUUID's fields --
 * #bUUID::time_low, #bUUID::time_mid and #bUUID::time_hi_and_version from the first, the clock
 * sequence and the six node bytes from the second. No random generator is involved: the marker is
 * a pure function of the definition's shape, and nothing about it is stored in the file (it is a
 * session name for a cache entry, the way the effect markers the description carries are names the
 * description derives rather than data it owns). An all-zero answer -- two nil markers, or a
 * collision the birthday bound puts far past any real definition -- is bumped to a non-nil one:
 * a nil marker reads as "no effect" to #paint_correction_is_effect, and a correction that carries
 * one is a plain filter, not the member's cache key.
 */
static bUUID composite_chain_member_marker(const MaterialPaintLayer &chain,
                                           const MaterialPaintLayer &member)
{
  const auto uuid_halves = [](const bUUID &uuid) {
    static_assert(sizeof(bUUID) == sizeof(uint64_t[2]),
                  "the fold reads the uuid's packed 16 bytes");
    std::array<uint64_t, 2> halves;
    memcpy(halves.data(), &uuid, sizeof(halves));
    return halves;
  };
  const std::array<uint64_t, 2> chain_halves = uuid_halves(chain.marker);
  const std::array<uint64_t, 2> member_halves = uuid_halves(member.marker);
  uint64_t hash_a = fnv1a_offset_basis;
  uint64_t hash_b = fnv1a_offset_basis ^ 0x9e3779b97f4a7c15ull;
  for (const int i : IndexRange(2)) {
    hash_a = fnv1a_mix(hash_a, chain_halves[i]);
    hash_a = fnv1a_mix(hash_a, member_halves[i]);
    hash_b = fnv1a_mix(hash_b, chain_halves[i] ^ 0x2545f4914f6cdd1dull);
    hash_b = fnv1a_mix(hash_b, member_halves[i] ^ 0x2545f4914f6cdd1dull);
  }
  bUUID marker;
  marker.time_low = uint32_t(hash_a);
  marker.time_mid = uint16_t(hash_a >> 32);
  marker.time_hi_and_version = uint16_t(hash_a >> 48);
  marker.clock_seq_hi_and_reserved = uint8_t(hash_b);
  marker.clock_seq_low = uint8_t(hash_b >> 8);
  for (const int i : IndexRange(6)) {
    marker.node[i] = uint8_t(hash_b >> (16 + 8 * i));
  }
  if (BLI_uuid_is_nil(marker)) {
    marker.time_low = 1;
  }
  return marker;
}

/**
 * A fingerprint of the instance's stored overrides of \a member's parameters: the chain row's
 * system properties under the member keys (#paint_layer_chain_member_param_key), name and value
 * both, folded in the definition order the group holds. The member's own values ride the base
 * effect hash (its packed parameters fold), so this fold is what makes an exposed-value edit
 * retire the cached result of a member whose kernel reads no packed schema -- the node-group
 * filter takes its parameters from its IDProperties directly, which no packed walk sees. A
 * property of a shape this fold does not name (a group, an id) still moves the hash by its name
 * and type, which is enough: the kernel-side merge below replaces by name whatever the member's
 * schema reads.
 */
static uint64_t composite_chain_member_overrides_hash(const MaterialPaintLayer &chain,
                                                      const MaterialPaintLayer &member)
{
  uint64_t hash = fnv1a_offset_basis;
  const IDProperty *system_properties = (chain.system_properties != nullptr &&
                                         chain.system_properties->type == IDP_GROUP) ?
                                            chain.system_properties :
                                            nullptr;
  if (system_properties == nullptr) {
    return hash;
  }
  char member_prefix[UUID_STRING_SIZE];
  BLI_uuid_format(member_prefix, member.marker);
  const blender::StringRef member_prefix_key(member_prefix);
  const auto mix = [&](const uint64_t value) {
    hash = fnv1a_mix(hash, value);
  };
  for (const IDProperty *prop = static_cast<const IDProperty *>(
           system_properties->data.group.first);
       prop != nullptr;
       prop = prop->next)
  {
    const blender::StringRef prop_name(prop->name);
    if (!prop_name.startswith(member_prefix_key)) {
      continue;
    }
    const blender::StringRef name = prop_name.drop_prefix(member_prefix_key.size());
    /* #DefaultHash has no #StringRef specialization, and this fold is part of a cache key: keep
     * hashing through #StringRefNull, safe here because the name points into the property's
     * null-terminated name buffer. */
    mix(get_default_hash(blender::StringRefNull(name.data())));
    mix(uint64_t(prop->type));
    switch (prop->type) {
      case IDP_INT:
        mix(uint64_t(IDP_int_get(prop)));
        break;
      case IDP_FLOAT: {
        uint32_t bits;
        const float value = IDP_float_get(prop);
        memcpy(&bits, &value, sizeof(bits));
        mix(bits);
        break;
      }
      case IDP_DOUBLE:
        mix(uint64_t(IDP_double_get(prop)));
        break;
      case IDP_BOOLEAN:
        mix(uint64_t(IDP_bool_get(prop) ? 1 : 0));
        break;
      case IDP_ARRAY: {
        mix(uint64_t(prop->len));
        if (prop->subtype == IDP_FLOAT || prop->subtype == IDP_DOUBLE) {
          const size_t stride = (prop->subtype == IDP_FLOAT) ? sizeof(float) : sizeof(double);
          const char *data = static_cast<const char *>(prop->data.pointer);
          for (const int i : IndexRange(prop->len)) {
            uint64_t bits = 0;
            memcpy(&bits, data + i * stride, stride);
            mix(bits);
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return hash;
}

/**
 * The parameters the node-group kernel reads for the chain member \a member of the instance \a
 * chain: the member's own properties, with the instance's stored overrides folded in under the
 * plain parameter names the member's schema reads. A chain member is not a row of the material the
 * composite runs on, so the kernel cannot read the instance's exposed values where a direct
 * effect's kernel reads its row's own; the unfolding hands them over with this merged group
 * instead. The copy carries the member's payloads (a ramp the interface laid down) whole -- the
 * override keys only ever name schema parameters, the same names the packed override hook reads.
 *
 * \return a group the caller owns and frees with #IDP_FreeProperty, or null when the member has no
 * parameters at all and the instance stores no override for it: the kernel then reads the member's
 * own (absent) properties, exactly as an untouched direct effect does.
 */
static IDProperty *composite_chain_member_parameters(const MaterialPaintLayer &chain,
                                                     const MaterialPaintLayer &member)
{
  IDProperty *merged = nullptr;
  if (member.system_properties != nullptr && member.system_properties->type == IDP_GROUP) {
    merged = IDP_CopyProperty(member.system_properties);
  }
  if (merged == nullptr) {
    IDPropertyTemplate empty = {0};
    merged = IDP_New(IDP_GROUP, &empty, "chain-member-parameters");
  }
  const IDProperty *system_properties = (chain.system_properties != nullptr &&
                                         chain.system_properties->type == IDP_GROUP) ?
                                            chain.system_properties :
                                            nullptr;
  if (system_properties != nullptr) {
    char member_prefix[UUID_STRING_SIZE];
    BLI_uuid_format(member_prefix, member.marker);
    const blender::StringRef member_prefix_key(member_prefix);
    for (const IDProperty *prop = static_cast<const IDProperty *>(
             system_properties->data.group.first);
         prop != nullptr;
         prop = prop->next)
    {
      const blender::StringRef prop_name(prop->name);
      if (!prop_name.startswith(member_prefix_key)) {
        continue;
      }
      /* The stored value replaces the member's own under the plain parameter name: the same
       * replacement the packed override hook makes for the schema-driven kinds, one level shallower
       * because the kernel reads the group directly. */
      IDProperty *copy = IDP_CopyProperty(prop);
      /* The key is `<member uuid>:<parameter name>`; the colon is the separator, not part of the
       * parameter name the kernel and the backend's identifier bind read. */
      blender::StringRef param_name = prop_name.drop_prefix(member_prefix_key.size());
      if (param_name.startswith(":")) {
        param_name = param_name.drop_prefix(1);
      }
      /* #BLI_strncpy reads a C string, and a StringRef carries no terminator of its own; the
       * name points into the property's null-terminated name buffer, so its data() is one. */
      BLI_strncpy(copy->name, param_name.data(), sizeof(copy->name));
      IDP_ReplaceInGroup(merged, copy);
    }
  }
  return merged;
}

/**
 * The instance-unique hash of the chain member \a member of the instance \a chain: the member's
 * own effect hash, folded with the composite marker the correction is keyed under (two instances
 * of one asset in one material carry the same definition member, so only the marker separates
 * their keys) and with the instance's stored overrides (an exposed-value edit retires the cached
 * result, and moves the stack hash and the GPU chain hash with it). The one hash the member's
 * correction carries and its cache entry keys on, so the CPU and the GPU caches -- which read the
 * same field -- can never disagree about what the member's answer is a function of.
 */
static uint64_t paint_layer_chain_member_effect_hash(const MaterialPaintLayer &chain,
                                                     const MaterialPaintLayer &member,
                                                     const int channel)
{
  const uint64_t hash = paint_layer_effect_hash(
      member, channel, composite_chain_member_marker(chain, member));
  return fnv1a_mix(hash, composite_chain_member_overrides_hash(chain, member));
}

/**
 * Append one GPU-computed chain member's effect correction to \a out: the member's kind and packed
 * parameters (the instance's overrides included), the composite marker the effect caches key the
 * answer under, and the member/chain pair the GPU job walk resolves it by. \a describe_only names
 * the maps and computes nothing. Shared by the unfolding and by the member input's describe, which
 * names the member beside its prefix.
 */
static void composite_chain_member_effect_append(const Material &ma,
                                                 const MaterialPaintLayer &owner_row,
                                                 const MaterialPaintLayer &chain,
                                                 const MaterialPaintLayer &member,
                                                 const PaintLayerKindInfo &member_kind,
                                                 const IDProperty *system_properties,
                                                 const int channel,
                                                 const float pass_through_scale,
                                                 const bool describe_only,
                                                 const int tile_number,
                                                 PaintLayerSurfaceRef painted_object,
                                                 PaintMaterialCompositeImageLayer &out)
{
  const auto override_lookup = [&](const int index, float r_value[3]) {
    return system_properties != nullptr &&
           composite_chain_member_override(
               *system_properties, member, member_kind, index, r_value);
  };
  PaintMaterialCompositeCorrection effect_correction;
  effect_correction.filter = member.kind_subtype;
  composite_filter_params(member, effect_correction.filter_params, override_lookup);
  effect_correction.filter_ramp = BKE_paint_layers_ramp_get(member);
  effect_correction.filter_curves = BKE_paint_layers_curves_get(member);
  effect_correction.effect_marker = composite_chain_member_marker(chain, member);
  effect_correction.effect_hash = paint_layer_chain_member_effect_hash(chain, member, channel);
  effect_correction.effect_result_has_alpha = paint_layer_kind_result_has_alpha(member);
  /* The pair the GPU job walk resolves the member by: a composite marker is no row of the material,
   * so the walk cannot look it up the way it looks a direct effect up. Both pointers borrow into
   * live DNA for the description's lifetime, as the ramp and the curves above do. */
  effect_correction.effect_chain = &chain;
  effect_correction.effect_member = &member;
  /* The result the correction mixes: the member's kernel over the chain's content with the members
   * before this one folded in, cached under the composite marker. Describe-only leaves the buffer
   * out and names the maps instead -- the combined preview falls back to nothing for the plan kinds
   * it computes itself, and reads the same maps for its own input describe. The member never owns
   * its result (a definition member is not baked; the row's bake holds the chain whole), so no bake
   * map is asked for. */
  effect_correction.effect_color = paint_layer_chain_member_effect_buffer_get(ma,
                                                                              owner_row,
                                                                              chain,
                                                                              member,
                                                                              channel,
                                                                              tile_number,
                                                                              describe_only,
                                                                              &effect_correction.effect_images,
                                                                              painted_object);
  if (!describe_only && effect_correction.effect_color == nullptr) {
    /* The input could not be rendered or the kernel gave no answer: the correction is left out,
     * as a direct effect's is, and the members after it lay over the content without it. */
    return;
  }
  effect_correction.opacity = BKE_paint_layers_channel_opacity_effective(member, channel) *
                              pass_through_scale;
  out.content_corrections.append(effect_correction);
}

/**
 * Unfold the chain instance \a chain into the corrections of its definition: the definition's
 * effect rows are appended in stack order, each as the ordinary correction it would be if the user
 * had added it by hand -- a pointwise filter for the pointwise members, an effect correction keyed
 * on a composite marker for the GPU-computed ones (#paint_layer_effect_is_smart_filter_member
 * judges the shape, and the portability saved the definition with the same test, so the two can
 * never disagree about what travels) -- with the instance's exposed values read over the member's
 * own.
 *
 * This is the design's "chain as one plan-job" realized at the description level. The unfolding
 * does not build a concatenated plan of its own: each GPU-computed member becomes an effect
 * correction the machinery already knows how to collect, cache, plan and dispatch, exactly like a
 * built-in GPU effect on the row. The ordering the chain promises comes from the definition order
 * the corrections are appended in, and the one-input-render economy the design wants comes from
 * each member's input folding the corrections of the members before it (the prefix, below), whose
 * results the input render reads from the caches the earlier members published. A literal single
 * plan-job -- one job whose pass plan is the concatenation of its members' plans, one input render
 * for the whole chain -- remains possible later, but it needs no shader change and no plan-structure
 * change to be approximated by the per-member jobs, so v3 ships those: the same mechanism the
 * built-in GPU effects run on, with no new pass kind and no new dispatch path.
 *
 * The definition is the material the row reads (#MaterialPaintLayer::material); null -- a
 * definition gone from the file -- lays nothing, the same answer a Material row with no source
 * gives, rather than a chain that filters with nothing. Every member is judged by the same test
 * the Smart Filter portability saved it with (#paint_layer_effect_is_smart_filter_member), so a
 * definition edited past its contract cannot lay a correction the branches below cannot answer:
 * the member is skipped, the way a row whose kind this build does not know is. A chain with a
 * member this build refuses (a NEEDS_BAKE kind with neither a GPU pass nor a precomputed answer)
 * previews without it -- the portability still blocks the shape at save time, so the skip is the
 * defensive tail of the contract, not the path a conforming definition takes.
 *
 * The instance's values ride the member's own packing: the packed walk is handed the lookup that
 * reads #MaterialPaintLayer::system_properties under the member key the descriptor named
 * (#paint_layer_chain_member_param_key), so an override lands at the member schema's packed
 * position and a Color or a Vector widens the same three slots the plain read widens. The member's
 * payloads travel whole -- the ramp and the curves are the member's, as are a Color Balance's and a
 * Channel Mixer's folds, which the override hook deliberately never answers (v1 exposes their
 * values to the instance not at all).
 *
 * The member's own effective opacity is what scales it, times the \a pass_through_scale the chain
 * row hangs under; the chain row's own opacity is deliberately not folded in (v1: a master opacity
 * would want a multiplicative pass over every member's strength, a refinement the design leaves for
 * later -- toggling the chain row still hides it, through the enabled flag the effective opacity
 * reads).
 *
 * \a stop_before_member bounds the walk at the definition member it names (null unfolds the whole
 * chain): the members before it are exactly the prefix a GPU-computed member's input folds, and
 * the same function lays both the description's corrections and the prefix's, so the input a
 * member is computed from is the chain as the composite lays it, never a re-derivation that could
 * drift. The prefix ask runs at a pass-through of one -- the effect input's own scale -- and is
 * the reason the GPU member branch needs \a ma, \a describe_only and the tile context at all: the
 * member's result is a buffer the effect cache computes (or, in describe-only, a correction that
 * only names its maps), which the pointwise branch never needs.
 */
static void composite_effect_append_chain_impl(const Material &ma,
                                               const MaterialPaintLayer *owner_row,
                                               const MaterialPaintLayer &chain,
                                               const int channel,
                                               const float pass_through_scale,
                                               const bool describe_only,
                                               const int tile_number,
                                               PaintLayerSurfaceRef painted_object,
                                               const MaterialPaintLayer *stop_before_member,
                                               PaintMaterialCompositeImageLayer &out)
{
  const Material *definition = chain.material;
  if (definition == nullptr) {
    return;
  }
  /* The instance's overrides live at the top level of the row's system properties, keyed by the
   * member key; the group is borrowed once and asked per member parameter below. A missing
   * property is not an override: the member's own value stands, which is what an unexposed or
   * untouched parameter reads as. */
  const IDProperty *system_properties = (chain.system_properties != nullptr &&
                                         chain.system_properties->type == IDP_GROUP) ?
                                            chain.system_properties :
                                            nullptr;
  for (const MaterialPaintLayer &member : definition->paint_layers) {
    if (stop_before_member != nullptr &&
        BLI_uuid_equal(member.marker, stop_before_member->marker))
    {
      break;
    }
    if (!paint_layer_effect_is_smart_filter_member(member)) {
      continue;
    }
    if (!paint_layer_effect_filters(*definition, member, channel)) {
      continue;
    }
    const PaintLayerKindInfo *member_kind = paint_layer_kind_of(member);
    if (member_kind == nullptr) {
      continue;
    }
    const auto override_lookup = [&](const int index, float r_value[3]) {
      return system_properties != nullptr &&
             composite_chain_member_override(
                 *system_properties, member, *member_kind, index, r_value);
    };
    if (!paint_layer_effect_is_cpu_computed(member)) {
      /* A pointwise member rides the plain path, which keys nothing per instance: its correction
       * is the filter the evaluator's own branch lays. */
      PaintMaterialCompositeCorrection filter;
      filter.filter = member.kind_subtype;
      composite_filter_params(member, filter.filter_params, override_lookup);
      filter.filter_ramp = BKE_paint_layers_ramp_get(member);
      filter.filter_curves = BKE_paint_layers_curves_get(member);
      filter.opacity = BKE_paint_layers_channel_opacity_effective(member, channel) *
                       pass_through_scale;
      out.content_corrections.append(filter);
      continue;
    }
    /* A GPU-computed member (a Blur, or the node-group filter the effect stage serves from the CPU
     * cache) becomes an effect correction, the shape #composite_effect_append_cpu_computed lays
     * for a direct effect -- the correction carries the member's kind and packed parameters (the
     * instance's overrides included), the result beside them, and the composite marker the effect
     * caches key the answer under. The correction pass mixes it in the order the definition holds,
     * and the GPU job walk collects it like any other marked correction: one effect job per
     * member, its plan the member's own. */
    if (owner_row == nullptr) {
      /* The input is the owner row's content up to the chain: a description without one cannot
       * name it, so the member is left out, exactly as a CPU-computed effect of a standalone
       * corrections-row description is. */
      continue;
    }
    composite_chain_member_effect_append(ma,
                                         *owner_row,
                                         chain,
                                         member,
                                         *member_kind,
                                         system_properties,
                                         channel,
                                         pass_through_scale,
                                         describe_only,
                                         tile_number,
                                         painted_object,
                                         out);
  }
}

/**
 * Unfold the chain instance \a chain into the corrections of its definition, for the composite's
 * description of \a owner_row (null from a standalone corrections-row description, where a
 * GPU-computed member has no owner content to read and is left out): the whole chain, every member
 * the contract lets through, in definition order. See #composite_effect_append_chain_impl for the
 * shape of the corrections.
 */
static void composite_effect_append_chain(const Material &ma,
                                          const MaterialPaintLayer *owner_row,
                                          const MaterialPaintLayer &chain,
                                          const int channel,
                                          const float pass_through_scale,
                                          const bool describe_only,
                                          const int tile_number,
                                          PaintLayerSurfaceRef painted_object,
                                          PaintMaterialCompositeImageLayer &out)
{
  composite_effect_append_chain_impl(ma,
                                     owner_row,
                                     chain,
                                     channel,
                                     pass_through_scale,
                                     describe_only,
                                     tile_number,
                                     painted_object,
                                     /*stop_before_member=*/nullptr,
                                     out);
}

/**
 * Append the content correction \a effect to \a out as the evaluator reads it in \a channel: a filter,
 * or a map, flat colour, source or slice laid by the correction's own blend and opacity. Nothing is
 * appended for a correction that lays nothing here, as the generator builds nothing for it.
 */
static void composite_effect_append(
    const Material &ma,
    const MaterialPaintLayer *owner_row,
    const MaterialPaintLayer &effect,
    const int channel,
    const float pass_through_scale,
    const int *slice_rect,
    const bool dimensions_only,
    const bool describe_only,
    const int tile_number,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    PaintMaterialCompositeImageLayer &out,
    PaintLayerSurfaceRef painted_object = nullptr)
{
  if (effect.kind_subtype == MA_PAINT_LAYER_FILTER_CHAIN) {
    /* The chain's members are the corrections: the definition's effect rows unfold into the
     * corrections the branches below describe one by one -- pointwise filters, and effect
     * corrections for the GPU-computed members -- each carrying the instance's exposed values (the
     * composite-level expansion the design names). The unfolding runs before every other branch on
     * purpose: a chain is none of the shapes below, and describing it as one would wrong the
     * colour or the coverage. */
    composite_effect_append_chain(ma,
                                  owner_row,
                                  effect,
                                  channel,
                                  pass_through_scale,
                                  describe_only,
                                  tile_number,
                                  painted_object,
                                  out);
    return;
  }
  if (effect.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
    /* A node-group source lays the shader-bake's answer over the content it reads, at the row's
     * blend and opacity -- the same maps the row lays. The kind carries NEEDS_BAKE (the live graph
     * lays the row bake that holds it), so this branch stands before the CPU-computed one, whose
     * kernel path is a filter's, not a laying source's. No lays gate of the other kinds' here:
     * what a source lays is what its definition supplies, and the bake image is that answer's
     * presence. */
    PaintMaterialCompositeCorrection correction;
    correction.iuser = nullptr;
    correction.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                         pass_through_scale;
    Image *bake = composite_node_group_bake_image(
        ma, effect, /*bake_channel=*/-1, tile_number, painted_object);
    if (bake == nullptr) {
      return;
    }
    composite_mapping_describe(
        ma, effect, channel, correction.mapping, tile_number, painted_object, bake);
    correction.image = bake;
    correction.blend = layer_channel_blend(effect, channel);
    out.content_corrections.append(correction);
    return;
  }
  if (paint_layer_effect_is_cpu_computed(effect)) {
    composite_effect_append_cpu_computed(ma,
                                         owner_row,
                                         effect,
                                         channel,
                                         pass_through_scale,
                                         slice_rect,
                                         dimensions_only,
                                         tile_number,
                                         describe_only,
                                         out,
                                         painted_object);
    return;
  }
  if (paint_layer_effect_filters(ma, effect, channel)) {
    PaintMaterialCompositeCorrection filter;
    filter.filter = effect.kind_subtype;
    composite_filter_params(effect, filter.filter_params);
    filter.filter_ramp = BKE_paint_layers_ramp_get(effect);
    filter.filter_curves = BKE_paint_layers_curves_get(effect);
    filter.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                     pass_through_scale;
    out.content_corrections.append(filter);
    return;
  }
  if (!paint_layer_effect_lays(ma, effect, channel)) {
    return;
  }
  PaintMaterialCompositeCorrection correction;
  correction.iuser = nullptr;
  correction.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                       pass_through_scale;
  bool described;
  if (paint_layer_kind_reads_anchor_point(effect)) {
    described = composite_effect_anchor_point_describe(ma,
                                                       effect,
                                                       channel,
                                                       pass_through_scale,
                                                       slice_rect,
                                                       dimensions_only,
                                                       describe_only,
                                                       tile_number,
                                                       painted_object,
                                                       correction);
  }
  else if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_GENERATES)) {
    if (effect.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP) {
      /* A node-group pattern has no CPU factor to evaluate, so the correction carries the
       * shader-bake's answer instead: the bake map laid by the member's blend and opacity, the
       * same answer the row form carries. The laid-map shape is what the GPU correction pass
       * reads -- the way a node-group source correction lays -- so the combined preview no
       * longer has to refuse the member to the CPU. */
      Image *bake = composite_node_group_bake_image(ma, effect, channel, tile_number, painted_object);
      if (bake != nullptr) {
        composite_mapping_describe(
            ma, effect, channel, correction.mapping, tile_number, painted_object, bake);
        correction.image = bake;
        described = true;
      }
    }
    else {
      /* A generator correction has no map: its ramp is laid by its own blend and opacity. */
      described = composite_generator_describe(ma,
                                               effect,
                                               channel,
                                               context_surface,
                                               correction.gradient,
                                               /*projection_row=*/nullptr,
                                               tile_number,
                                               painted_object);
    }
  }
  else if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE)) {
    described = composite_effect_source_describe(
        ma, effect, channel, tile_number, painted_object, correction);
  }
  else {
    described = composite_effect_map_describe(
        ma, effect, channel, tile_number, painted_object, correction);
  }
  if (!described) {
    return;
  }
  correction.blend = layer_channel_blend(effect, channel);
  out.content_corrections.append(correction);
}

bool composite_image_layer_build(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 const int channel,
                                 const float pass_through_scale,
                                 const bool isolating,
                                 const bool ignore_own_bake,
                                 PaintMaterialCompositeImageLayer &out,
                                 const int tile_number,
                                 const bUUID *stop_before,
                                 const int *slice_rect,
                                 const bool dimensions_only,
                                 const bool describe_only,
                                 const std::shared_ptr<const PaintMaterialCompositeSurface>
                                     &context_surface,
                                 PaintLayersHeightWonCache *height_won_cache,
                                 const PaintLayerBakeScope scope,
                                 PaintLayerSurfaceRef painted_object)
{
  /* A reader lays the slice its point publishes, described as a substituted row: the slice's
   * colour, its coverage and its content alpha, with the reader's own blend. Its own mapping does
   * not remap the slice. */
  bool reader_live = false;
  /* The mode the description runs in: which of it a content bake leaves out. One mode where two
   * flags would have to be kept exclusive by hand. */
  enum class ContentMode {
    /** A live row, or a reader over its live slice: the source, the corrections, the mask and the
     * opacity are all described. */
    Live,
    /** The bake render of a content bake: its mask and its opacity are laid live over the map, so
     * neither is folded into these pixels. */
    RenderContentBake,
    /** The description of a row standing in over its content bake: the source and the corrections
     * are inside the bake, so neither is described again. */
    SubstituteContentBake,
  };
  ContentMode content_mode = scope == PaintLayerBakeScope::ContentOnly ?
                                 ContentMode::RenderContentBake :
                                 ContentMode::Live;
  if (paint_layer_kind_reads_anchor_point(layer)) {
    if (!paint_layer_row_reads_anchor_point(ma, layer, channel)) {
      return false;
    }
    const PaintLayerAnchorPointRef reader_ref = paint_layer_anchor_point_resolve(ma, layer);
    out.anchor_point_reader = layer.marker;
    out.anchor_point_target = int8_t(reader_ref.target);
    out.anchor_point_point = (reader_ref.point != nullptr) ? reader_ref.point->marker : bUUID{};
    /* A baked reader reads the maps its slice was rendered into; the row itself is never
     * substituted, since the description decides what the slice is. The bake renders from the live
     * description (\a ignore_own_bake), so it never reads its own stale maps. */
    if (!ignore_own_bake && paint_layer_anchor_point_reader_baked(ma, layer) &&
        layer.bake != nullptr && layer.bake->images[channel] != nullptr &&
        layer.bake->coverage != nullptr)
    {
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
      out.color_image = layer.bake->images[channel];
      out.color_iuser = nullptr;
      out.mask_image = layer.bake->coverage;
      out.mask_iuser = nullptr;
      out.mask_from_alpha = false;
      out.mask_influence = 1.0f;
      /* The slice's bake is read back the way a substituted row's is (the shared predicate). */
      out.color_alpha_coverage = paint_layer_bake_color_alpha_is_coverage(layer, record);
      out.blend = layer_channel_blend(layer, channel);
      composite_height_settings_describe(layer, channel, out);
      out.opacity = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                    pass_through_scale;
      out.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, layer, channel);
      out.enabled = true;
      out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
          eMaterialPaintChannel(channel));
      /* The bake already holds the reader's corrections, mask and opacity, as a substituted row's
       * does, so none of them is described again. */
      return true;
    }
    if (!describe_only &&
        !BKE_paint_layers_slice_render(ma,
                                       layer,
                                       channel,
                                       &out.anchor_point_color,
                                       &out.anchor_point_coverage,
                                       slice_rect,
                                       dimensions_only,
                                       PaintLayerTileContext{.painted_object = painted_object,
                                                             .tile_number = tile_number}))
    {
      return false;
    }
    if (dimensions_only) {
      /* Sizing only: the buffers stay unrendered, but the slice's side is known and is what the
       * bake of the reader is rendered at. Carried the way an empty-base row's canvas is, so the
       * stack's dimensions find the reader without rendering a single nested slice. */
      int slice_width = 0;
      int slice_height = 0;
      if (anchor_slice_dimensions(ma,
                                  layer,
                                  channel,
                                  reader_ref,
                                  slice_width,
                                  slice_height,
                                  painted_object,
                                  tile_number))
      {
        out.empty_base = true;
        out.empty_base_size = slice_width;
      }
    }
    /* Describe-only keeps the reader even where #BKE_paint_layers_slice_render would refuse it for
     * a zero-sized source. The GPU plan captures the slot and the screen-space pass needs no source
     * size; a caller that later falls back to the CPU rebuilds the full description, which refuses
     * it there. */
    out.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, layer, channel);
    BKE_paint_layers_anchor_point_slice_images(ma, layer, out.grey_input_images);
    /* The reader's own corrections and mask go on below like any row's, so a point inside this
     * row slices them too. */
    reader_live = true;
  }
  if (!reader_live && !ignore_own_bake && paint_layer_row_substituted(ma, layer)) {
    Image *baked_color = nullptr;
    if (!paint_layer_row_baked_channel(ma, layer, channel, &baked_color)) {
      return false;
    }
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    out.color_image = baked_color;
    out.color_iuser = nullptr;
    out.mask_image = layer.bake->coverage;
    out.mask_iuser = nullptr;
    out.mask_from_alpha = false;
    out.mask_influence = 1.0f;
    /* The colour map's alpha is the content alpha the live row had (the shared predicate). */
    out.color_alpha_coverage = paint_layer_bake_color_alpha_is_coverage(layer, record);
    out.blend = layer_channel_blend(layer, channel);
    composite_height_settings_describe(layer, channel, out);
    /* The coverage map holds what the content covers rather than the mask times the opacity; the
     * map keeps its place as the pass's mask, since for a row whose colour carries its own alpha
     * the quotient the bake stored is one wherever the content reaches, so nothing is counted
     * twice, and a row that carries none is covered by the map alone. */
    if (paint_layer_bake_scope(layer, false) == PaintLayerBakeScope::ContentOnly) {
      content_mode = ContentMode::SubstituteContentBake;
    }
    else {
      /* The coverage map already holds the row's opacity and mask; only visibility stays live, the
       * same 0/1 the generated graph multiplies in. */
      out.opacity = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                    pass_through_scale;
      out.enabled = true;
      out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
          eMaterialPaintChannel(channel));
      return true;
    }
  }
  /* A live reader's slice is described above; every other row describes its own source. */
  if (!reader_live && content_mode != ContentMode::SubstituteContentBake) {
    if (BKE_paint_layers_is_folder(layer)) {
      Vector<PaintMaterialCompositeImageLayer> children;
      composite_image_layers_collect(ma,
                                     layer.children,
                                     channel,
                                     pass_through_scale,
                                     context_surface,
                                     children,
                                     tile_number,
                                     true,
                                     slice_rect,
                                     dimensions_only,
                                     describe_only,
                                     height_won_cache,
                                     painted_object);
      /* An emptied folder is left out, as the generator leaves it; one with a correction that lays
       * here is not: the correction brings the coverage in over nothing. */
      if (children.is_empty() && !paint_layer_effects_lay(ma, layer, channel)) {
        return false;
      }
      out.is_folder = true;
      out.children.assign(std::make_move_iterator(children.begin()),
                          std::make_move_iterator(children.end()));
    }
    else if (!composite_row_source_describe(
                 ma, layer, channel, isolating, context_surface, tile_number, painted_object, out))
    {
      return false;
    }
  }
  out.blend = layer_channel_blend(layer, channel);
  composite_height_settings_describe(layer, channel, out);
  /* The content bake holds the row's content alone, so its own opacity -- the visibility with it --
   * is not folded in: the substitution lays it live over the map. What the row's source gives to the
   * transparency stays, being part of the content itself. */
  const float live_opacity = BKE_paint_layers_channel_opacity_effective(layer, channel) *
                             pass_through_scale;
  out.opacity = content_mode == ContentMode::RenderContentBake ? 1.0f : live_opacity;
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
        ma, layer, PaintLayerMaterialUse::Composite);
    if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
      out.opacity *= clamp_f(coverage.constant, 0.0f, 1.0f);
    }
  }
  out.enabled = true;
  out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
      eMaterialPaintChannel(channel));
  /* The corrections lay over the row's colour, in the order they were added. A correction that
   * lays nothing in this channel is not carried, as the generator builds nothing for it. A row
   * substituted by its content bake has its corrections inside that bake already, and a content
   * bake renders them, being the content the cache holds. */
  if (content_mode != ContentMode::SubstituteContentBake) {
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (stop_before != nullptr && BLI_uuid_equal(effect.marker, *stop_before)) {
        break;
      }
      composite_effect_append(ma,
                              &layer,
                              effect,
                              channel,
                              pass_through_scale,
                              slice_rect,
                              dimensions_only,
                              describe_only,
                              tile_number,
                              context_surface,
                              out,
                              painted_object);
    }
  }
  /* The mask is one for all channels of the row, so the same item reaches every channel's stack,
   * exactly as the generator reads the one set of mask inputs in every channel chain. A content
   * bake renders none of it: the mask stays live over the map, where its own mapping is read as
   * well. */
  if (content_mode != ContentMode::RenderContentBake) {
    composite_mask_items_describe(ma,
                                 layer,
                                 out.mask_items,
                                 tile_number,
                                 stop_before,
                                 slice_rect,
                                 dimensions_only,
                                 describe_only,
                                 context_surface,
                                 true,
                                 painted_object);
  }
  /* Mark the positions the GPU has to capture: a marker is a producer only while at least one
   * reader lays its slice. A content marker sits in the effects, a mask marker in the mask stack. */
  if (content_mode != ContentMode::SubstituteContentBake) {
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (paint_layer_kind_is_anchor_point_marker(effect) &&
          paint_layer_anchor_point_is_read(ma, effect.marker))
      {
        out.anchor_point_content_producers.append(effect.marker);
      }
    }
  }
  if (content_mode != ContentMode::RenderContentBake) {
    for (const MaterialPaintLayer &item : layer.mask_stack) {
      if (paint_layer_kind_is_anchor_point_marker(item) &&
          paint_layer_anchor_point_is_read(ma, item.marker))
      {
        out.anchor_point_mask_producers.append(item.marker);
      }
    }
  }
  return true;
}

void composite_image_layers_collect(const Material &ma,
                                           const ListBaseT<MaterialPaintLayer> &list,
                                           const int channel,
                                           const float pass_through_scale,
                                           const std::shared_ptr<const PaintMaterialCompositeSurface>
                                               &context_surface,
                                           Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                           const int tile_number,
                                           const bool isolating,
                                           const int *slice_rect,
                                           const bool dimensions_only,
                                    const bool describe_only,
                                    PaintLayersHeightWonCache *height_won_cache,
                                    PaintLayerSurfaceRef painted_object)
{
  for (const MaterialPaintLayer &layer : list) {
    /* A Pass Through folder is flattened into this list, the way the generator expands it in the
     * chain. It has no row to carry its visibility, so a hidden one hands its rows a zero factor
     * on their opacity, accumulated on the way down (the same rule as #paint_layers_form_map). */
    if (BKE_paint_layers_folder_is_pass_through(layer)) {
      const float scale = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? pass_through_scale : 0.0f;
      composite_image_layers_collect(ma,
                                     layer.children,
                                     channel,
                                     scale,
                                     context_surface,
                                     r_layers,
                                     tile_number,
                                     isolating,
                                     slice_rect,
                                     dimensions_only,
                                     describe_only,
                                     height_won_cache,
                                     painted_object);
      if (&layer == list.last) {
        break;
      }
      continue;
    }
    PaintMaterialCompositeImageLayer out;
    if (composite_image_layer_build(ma,
                                   layer,
                                   channel,
                                   pass_through_scale,
                                   isolating,
                                   false,
                                   out,
                                   tile_number,
                                   nullptr,
                                   slice_rect,
                                   dimensions_only,
                                   describe_only,
                                   context_surface,
                                   height_won_cache,
                                   PaintLayerBakeScope::WholeRow,
                                   painted_object))
    {
      /* Where the row's Height record Compares, its win or loss gates every other channel of the
       * row; the mask is rendered once here, from the Height description of the rows below it in
       * this very list (pass-through folders flattened, isolation boundaries respected by the
       * recursion that got here). The Height channel itself never reads it back. */
      if (channel != PAINT_MATERIAL_CHANNEL_HEIGHT && !dimensions_only && !describe_only &&
          out.height_compare)
      {
        ListBaseT<MaterialPaintLayer> prefix;
        prefix.first = (list.first == &layer) ? nullptr : list.first;
        prefix.last = layer.prev;
        out.height_won = composite_height_won_get(
            ma, prefix, layer, context_surface, isolating, height_won_cache, tile_number);
      }
      r_layers.append(std::move(out));
    }
    /* A Compare row's prefix is a sub-range of its siblings whose `last` is the row below it; the
     * range-for walks `next` to the end of the whole list, so the bound is checked here. */
    if (&layer == list.last) {
      break;
    }
  }
}

bool BKE_paint_layers_composite_image_layers(
    const Material &ma,
    const int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    const bool describe_only,
    std::shared_ptr<const PaintMaterialCompositeSurface> surface,
    PaintLayersHeightWonCache *height_won_cache,
    const int tile_number,
    PaintLayerSurfaceRef painted_object)
{
  r_layers.clear();
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  /* The source-graph cache as the chain's bottom, laid first so every row lands over it: the rows
   * blend over the graph's own map instead of over the neutral constant, which the evaluator still
   * seeds underneath and this layer then covers outright. Described here -- the one place every
   * composite consumer gathers its stack from (the channel view, the composite cache, the Combined
   * preview on both devices) -- so the paths cannot drift apart. The layer covers at full opacity
   * and carries the channel's content alpha like any row does; its image alpha is the AOV render's,
   * not a coverage, so it blends by opacity alone. A stack with no row of its own still composites:
   * the cache is what it shows. The canvas-size walk skips this layer while a row map exists, so a
   * cache at its own resolution never shrinks the stack. */
  if (Image *cache = BKE_paint_layers_source_cache_image_get(ma, channel)) {
    PaintMaterialCompositeImageLayer bottom;
    bottom.color_image = cache;
    bottom.color_iuser = nullptr;
    bottom.is_source_cache = true;
    bottom.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
        eMaterialPaintChannel(channel));
    r_layers.append(std::move(bottom));
  }
  composite_image_layers_collect(ma,
                                 ma.paint_layers,
                                 channel,
                                 1.0f,
                                 surface,
                                 r_layers,
                                 tile_number,
                                 false,
                                 nullptr,
                                 false,
                                 describe_only,
                                 height_won_cache,
                                 painted_object);
  return !r_layers.is_empty();
}

bool BKE_paint_layers_composite_channel(const Material &ma,
                                        const int channel,
                                        ImBuf &r_dst,
                                        const rcti *region,
                                        const int tile_number,
                                        PaintLayerSurfaceRef painted_object)
{
  Vector<PaintMaterialCompositeImageLayer> image_layers;
  if (!BKE_paint_layers_composite_image_layers(
          ma, channel, image_layers, false, nullptr, nullptr, tile_number, painted_object))
  {
    return false;
  }
  int width = 0;
  int height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, width, height) || width <= 0 ||
      height <= 0 || r_dst.byte_buffer.data == nullptr || r_dst.x != width || r_dst.y != height ||
      !ELEM(r_dst.channels, 0, 4))
  {
    return false;
  }

  rcti area;
  if (!BKE_paint_material_composite_area_get(width, height, region, area)) {
    /* Nothing of the tagged region is inside the buffer; the destination is already correct. */
    return true;
  }

  /* Scratch the size of the area, never of the frame: a small refresh costs the same on any map. */
  const int64_t area_width = BLI_rcti_size_x(&area);
  Vector<float> linear(area_width * BLI_rcti_size_y(&area) * 4);
  if (!composite_channel_linear(image_layers,
                                channel,
                                PaintMaterialCompositeLinearTarget::packed(linear, area),
                                region))
  {
    return false;
  }

  /* The byte result is written in the channel's own colorspace: sRGB for a colour channel, data
   * for a scalar or Normal one. A caller that already set one -- the cache, from the map it was
   * built on -- keeps it. Set only now, so a refused composite leaves the destination untouched. */
  if (r_dst.byte_buffer.colorspace == nullptr) {
    const bool is_color = BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).is_color;
    const int role = is_color ? COLOR_ROLE_DEFAULT_BYTE : COLOR_ROLE_DATA;
    IMB_colormanagement_assign_byte_colorspace(
        &r_dst, IMB_colormanagement_role_colorspace_name_get(role));
  }

  BKE_paint_material_composite_linear_to_byte(linear, area, r_dst);

  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    composite_normal_channel_normalize(r_dst, region);
  }
  return true;
}

bool BKE_paint_layers_composite_image(const Material &ma,
                                      const int channel,
                                      Image &dst,
                                      const rcti *region,
                                      ReportList *reports,
                                      const int tile_number,
                                      PaintLayerSurfaceRef painted_object)
{
  Vector<PaintMaterialCompositeImageLayer> image_layers;
  if (!BKE_paint_layers_composite_image_layers(
          ma, channel, image_layers, false, nullptr, nullptr, tile_number, painted_object))
  {
    BKE_reportf(reports, RPT_ERROR, "Channel %d is not a compositable layer stack", channel);
    return false;
  }
  int width = 0, height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, width, height) || width <= 0 ||
      height <= 0)
  {
    BKE_report(reports, RPT_ERROR, "The layer stack has no dimensions to composite into");
    return false;
  }

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&dst, &iuser, &lock);
  if (ibuf == nullptr) {
    BKE_report(reports, RPT_ERROR, "The destination image has no buffer");
    return false;
  }
  if (ibuf->x != width || ibuf->y != height) {
    BKE_image_release_ibuf(&dst, ibuf, lock);
    BKE_report(reports,
               RPT_ERROR,
               "The destination image does not have the dimensions of the layer stack");
    return false;
  }

  if (ibuf->float_buffer.data != nullptr && !ELEM(ibuf->channels, 0, 4)) {
    /* The write below is RGBA; a 1- or 3-channel float buffer would be overrun. */
    BKE_image_release_ibuf(&dst, ibuf, lock);
    BKE_report(reports, RPT_ERROR, "The destination image must be RGBA");
    return false;
  }

  bool ok = false;
  if (ibuf->float_buffer.data != nullptr) {
    /* A float destination takes the linear result directly, premultiplied: that is what a float
     * ImBuf holds, and what the shader's own output compares against. */
    rcti area;
    if (!BKE_paint_material_composite_area_get(width, height, region, area)) {
      /* Nothing of the tagged region is inside the image; it is already correct. */
      ok = true;
    }
    else {
      /* Straight into the destination, no second frame-sized buffer: the area is evaluated in
       * place and premultiplied there, and the rest of the image is never touched. */
      float *dst_pixels = ibuf->float_data_for_write();
      const MutableSpan<float> storage(dst_pixels, int64_t(width) * height * 4);
      ok = composite_channel_linear(
          image_layers,
          channel,
          PaintMaterialCompositeLinearTarget::in_frame(storage, width, area),
          region);
      if (ok) {
        for (int y = area.ymin; y < area.ymax; y++) {
          float *row = dst_pixels + (int64_t(y) * width + area.xmin) * 4;
          for (int x = 0; x < BLI_rcti_size_x(&area); x++) {
            float *p = row + int64_t(x) * 4;
            p[0] *= p[3];
            p[1] *= p[3];
            p[2] *= p[3];
          }
        }
      }
    }
  }
  else {
    ok = BKE_paint_layers_composite_channel(ma, channel, *ibuf, region, tile_number);
  }
  if (ok) {
    /* The write bypasses the paint path, so nothing else tells the display buffer or the GPU copy
     * of \a dst that its pixels changed. */
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(&dst, ibuf);
    BKE_image_partial_update_mark_full_update(&dst);
  }
  BKE_image_release_ibuf(&dst, ibuf, lock);

  if (!ok) {
    BKE_report(reports, RPT_ERROR, "Failed to composite the channel");
  }
  return ok;
}

bool BKE_paint_layers_mask_items(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 Vector<PaintMaterialCompositeMaskItem> &r_items,
                                 const bUUID *stop_before,
                                 const int tile_number,
                                 PaintLayerSurfaceRef painted_object)
{
  r_items.clear();
  composite_mask_items_describe(ma,
                                layer,
                                r_items,
                                tile_number,
                                stop_before,
                                nullptr,
                                false,
                                false,
                                nullptr,
                                true,
                                painted_object);
  return !r_items.is_empty();
}

PaintLayersMaskSampler::PaintLayersMaskSampler(const Material &ma,
                                               const MaterialPaintLayer &layer,
                                               const bUUID *stop_before,
                                               const bool hidden_with_base,
                                               PaintLayerSurfaceRef painted_object)
{
  composite_mask_items_describe(ma,
                                layer,
                                items_,
                                1001,
                                stop_before,
                                nullptr,
                                false,
                                false,
                                nullptr,
                                hidden_with_base,
                                painted_object);
  /* The evaluator's own form of the description, so #BKE_paint_material_mask_factor_eval reads the
   * same opacity/blend/invert/filter/coverage fields the composite reads. */
  buffers_.resize(items_.size());
  for (const int i : items_.index_range()) {
    const PaintMaterialCompositeMaskItem &item = items_[i];
    PaintMaterialCompositeMaskBuffer &buffer = buffers_[i];
    /* A MESH_MAP atlas' alpha is data: the shared mask arithmetic (#BKE_paint_material_mask_factor_eval)
     * weights the item by its opacity alone and reads the scalar atlas' R, like the composite does. */
    buffer.is_mesh_map = item.is_mesh_map;
    buffer.is_mesh_map_scalar = item.is_mesh_map_scalar;
    buffer.constant_gray = item.constant_gray;
    buffer.opacity = item.opacity;
    buffer.contrast = item.contrast;
    buffer.blend = item.blend;
    buffer.invert = item.invert;
    buffer.filter = item.filter;
    buffer.effect_filter = item.effect_filter;
    buffer.filter_ramp = item.filter_ramp;
    buffer.filter_curves = item.filter_curves;
    std::copy(std::begin(item.filter_params),
              std::end(item.filter_params),
              std::begin(buffer.filter_params));
    buffer.map_coverage = item.map_coverage;
    buffer.coverage_tolerance = item.coverage_tolerance;
    buffer.coverage_softness = item.coverage_softness;
    buffer.coverage_output = item.coverage_output;
    buffer.coverage_colors = item.coverage_colors;
    buffer.gradient = item.gradient;
    if (buffer.gradient.enabled && buffer.gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV)
    {
      /* An Object/Generated coordinate needs the object's surface to turn a texel into the position
       * the render reads, which a stroke has no way to get here. Reading the texel's UV instead
       * would hide other texels than the render does, so the item takes no part in the stroke's
       * mask: the render still applies it.
       * TODO: hand the sampler the surface once the strokes can reach one. */
      buffer.opacity = 0.0f;
    }
  }
}

PaintLayersMaskSampler::~PaintLayersMaskSampler()
{
  for (Tile &tile : tiles_) {
    for (ImBuf *ibuf : tile.items) {
      if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
    for (ImBuf *ibuf : tile.coverage) {
      if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
  }
}

bool PaintLayersMaskSampler::is_empty() const
{
  return items_.is_empty();
}

void PaintLayersMaskSampler::tile_acquire(const int tile_number,
                                          const int ref_width,
                                          const int ref_height)
{
  for (const int i : tiles_.index_range()) {
    if (tiles_[i].tile_number == tile_number) {
      return;
    }
  }

  Tile tile;
  tile.tile_number = tile_number;
  tile.width = ref_width;
  tile.height = ref_height;
  const int item_count = items_.size();
  tile.items.resize(item_count, nullptr);
  tile.coverage.resize(item_count, nullptr);
  tile.item_samples.resize(item_count, nullptr);
  tile.coverage_samples.resize(item_count, nullptr);
  for (const int i : items_.index_range()) {
    const PaintMaterialCompositeMaskItem &item = items_[i];
    if (item.grey_buffer != nullptr) {
      /* A slice is already in canvas space and the description keeps its buffer alive: take an
       * ImBuf reference so the tile's destructor can release it like any other acquired map. A
       * slice of another size than the tile is not sampled, as for a painted map below. */
      if (item.grey_buffer->x == ref_width && item.grey_buffer->y == ref_height) {
        IMB_refImBuf(item.grey_buffer.get());
        tile.items[i] = item.grey_buffer.get();
        tile.item_samples[i] = item.grey_buffer->float_data();
      }
    }
    else if (item.image != nullptr) {
      ImBuf *ibuf = BKE_paint_material_mask_map_acquire(
          item.image, tile_number, &item.mapping, ref_width, ref_height);
      /* A buffer that does not match the tile the stroke paints on is not sampled: an item of
       * another size would be read out of bounds, and the composite refuses such a stack too. */
      if (ibuf != nullptr && ibuf->x == ref_width && ibuf->y == ref_height) {
        tile.items[i] = ibuf;
        tile.item_samples[i] = ibuf->float_data();
      }
      else if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
    else if (item.gradient.enabled && item.gradient.projected) {
      /* A UV line under a projected row reads the frame: rendered once through it into the tile,
       * the walk a projected map's resample takes, and read like a map from here. A render that
       * cannot be made keeps the plain UV read. */
      ImBuf *projected = BKE_paint_material_gradient_projected_render(
          item.gradient, ref_width, ref_height, tile_number);
      if (projected != nullptr && projected->x == ref_width && projected->y == ref_height) {
        tile.items[i] = projected;
        tile.item_samples[i] = projected->float_data();
        buffers_[i].gradient.enabled = false;
      }
      else if (projected != nullptr) {
        IMB_freeImBuf(projected);
      }
    }
    if (item.coverage_image != nullptr) {
      ImBuf *ibuf = BKE_paint_material_mask_map_acquire(
          item.coverage_image, tile_number, &item.coverage_mapping, ref_width, ref_height);
      if (ibuf != nullptr && ibuf->x == ref_width && ibuf->y == ref_height) {
        tile.coverage[i] = ibuf;
        tile.coverage_samples[i] = ibuf->float_data();
      }
      else if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
  }

  tiles_.append(std::move(tile));
}

float PaintLayersMaskSampler::sample(const int tile_number, const int x, const int y) const
{
  if (items_.is_empty()) {
    return 1.0f;
  }
  const Tile *found = nullptr;
  for (const Tile &candidate : tiles_) {
    if (candidate.tile_number == tile_number) {
      found = &candidate;
      break;
    }
  }
  if (found == nullptr) {
    return 1.0f;
  }
  const Tile &tile = *found;
  if (x < 0 || y < 0 || x >= tile.width || y >= tile.height) {
    return 1.0f;
  }
  const int64_t pixel_index = int64_t(y) * tile.width + x;
  return BKE_paint_material_mask_factor_eval(buffers_.as_span(),
                                             tile.item_samples,
                                             tile.coverage_samples,
                                             pixel_index,
                                             {x, y, tile.width, tile.height});
}

/**
 * The slice cache: one pair of buffers per reader, channel and description hash. The description
 * hash holds no pixels, so each entry also remembers the version of every map the slice was
 * rendered from (see #ImageRuntime::pixel_version). A lookup compares them: unchanged is a hit, an
 * edit of a rectangle re-renders that rectangle into the buffers, and anything else renders the
 * whole slice again. The cache holds a reference, never the only one: the description that renders
 * a slice shares the buffers for its own lifetime, so clearing or evicting an entry can never pull
 * pixels out from under an evaluation, and an update of an entry someone still holds works on a
 * copy. The byte budget accounts for what the cache itself retains; a buffer still referenced
 * elsewhere temporarily exceeds it, which is allowed.
 */
struct AnchorSliceCacheKey {
  uint32_t material_uid = 0;
  bUUID reader = {};
  int channel = 0;
  /** The UDIM tile the slice was rendered for; each tile is its own entry. */
  int tile_number = PAINT_TILE_NUMBER_BASE;
  uint64_t hash = 0;
};

/**
 * A map a slice was last made from, and the version of its pixels at that time. The same pair the
 * GPU effect cache keys its staleness on (see #PaintLayerEffectSource), so one snapshot serves both.
 */
using AnchorSliceSource = PaintLayerEffectSource;

/** The maps the slice of \a reader is rendered from, once each and none of them null. */
static void anchor_slice_images_get(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    Vector<Image *> &r_images)
{
  Vector<Image *> all;
  BKE_paint_layers_anchor_point_slice_images(ma, reader, all);
  for (Image *image : all) {
    if (image != nullptr && image->runtime != nullptr && !r_images.contains(image)) {
      r_images.append(image);
    }
  }
}

/**
 * Total memory the slice cache may hold, counted as it is allocated: a colour and a coverage buffer
 * of four floats per pixel each, so one 4K pair is 512 MiB. A composite of the shaded channels
 * needs one pair per channel of a reader at once, and an entry evicted between two channels is
 * rendered whole again; 4 GiB holds the working set of a reader at 4K and keeps the few most
 * recent others. A pair larger than the budget is never cached.
 */
constexpr size_t PAINT_LAYER_ANCHOR_SLICE_CACHE_BYTES = size_t(4096) * 1024 * 1024;

/** The live budget; only tests override it, to make eviction observable without 4K fixtures. */
static size_t g_anchor_slice_cache_budget = PAINT_LAYER_ANCHOR_SLICE_CACHE_BYTES;

struct AnchorSliceCacheEntry {
  AnchorSliceCacheKey key;
  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  /** The maps the buffers were rendered from, with the versions they had. */
  Vector<AnchorSliceSource> synced;
  size_t bytes = 0;
};

static size_t anchor_slice_color_bytes(const ImBuf *ibuf)
{
  return ibuf != nullptr ? size_t(ibuf->x) * size_t(ibuf->y) * sizeof(float[4]) : 0;
}

/** Coverage is stored as RGBA too: consumers read every channel. */
static size_t anchor_slice_coverage_bytes(const ImBuf *ibuf)
{
  return ibuf != nullptr ? size_t(ibuf->x) * size_t(ibuf->y) * sizeof(float[4]) : 0;
}

/** The cache owns references only; dropping an entry simply releases them. */
struct AnchorSliceCache {
  Vector<AnchorSliceCacheEntry> entries;
  size_t total_bytes = 0;
};

/**
 * The cache lives in a function rather than at file scope: a file-scope static is built before
 * `main` and torn down after the memory leak detector has run, so whatever it holds is reported as
 * a leak on every exit ("Not freed memory blocks") and freed while the CRT is already coming apart
 * -- which crashed the exit. A function-local static is built on first use, after the detector was
 * registered, and destroyed before it, so its frees are ordinary ones.
 */
static AnchorSliceCache &anchor_slice_cache()
{
  static AnchorSliceCache cache;
  return cache;
}
/**
 * Guards the container and the byte count only. A bake job renders on a worker thread while the
 * main thread describes the composite, and both reach the cache. It is never held while a slice
 * renders, so nested renders cannot deadlock on it.
 */
static std::mutex g_anchor_slice_cache_mutex;

void paint_layer_anchor_point_slice_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  /* The buffer goes too: the cache is a static, so a kept buffer would outlive the leak check that
   * runs when the program shuts down, and a cleared cache has nothing to reuse it for. */
  anchor_slice_cache().entries.clear_and_shrink();
  anchor_slice_cache().total_bytes = 0;
}

void BKE_paint_layers_slice_cache_free_all()
{
  paint_layer_anchor_point_slice_cache_clear();
}

void BKE_paint_layers_slice_cache_free_material(const Material &ma)
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  for (int i = int(anchor_slice_cache().entries.size()) - 1; i >= 0; i--) {
    if (anchor_slice_cache().entries[i].key.material_uid == ma.id.session_uid) {
      anchor_slice_cache().total_bytes -= anchor_slice_cache().entries[i].bytes;
      anchor_slice_cache().entries.remove(i);
    }
  }
}

size_t paint_layer_anchor_point_slice_cache_bytes()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  return anchor_slice_cache().total_bytes;
}

void paint_layer_anchor_point_slice_cache_set_budget(const size_t bytes)
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  g_anchor_slice_cache_budget = bytes;
}

size_t paint_layer_anchor_point_slice_cache_budget()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  return g_anchor_slice_cache_budget;
}

int paint_layer_anchor_point_slice_cache_entry_count()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  return int(anchor_slice_cache().entries.size());
}

static bool anchor_slice_key_equal(const AnchorSliceCacheKey &a, const AnchorSliceCacheKey &b)
{
  return a.material_uid == b.material_uid && BLI_uuid_equal(a.reader, b.reader) &&
         a.channel == b.channel && a.tile_number == b.tile_number && a.hash == b.hash;
}

bool paint_layer_effect_dimensions_resolve(Span<PaintMaterialCompositeImageLayer> input,
                                           const MaterialPaintLayer &effect,
                                           const int tile_number,
                                           int &r_width,
                                           int &r_height,
                                           bool &r_from_slot)
{
  r_from_slot = false;
  if (BKE_paint_material_composite_stack_dimensions(input, r_width, r_height) && r_width > 0 &&
      r_height > 0)
  {
    return true;
  }
  r_from_slot = paint_layer_effect_slot_size(effect, tile_number, r_width, r_height);
  return r_from_slot;
}

/**
 * The size a chain member's input is computed at: the described input's own size, or -- when the
 * owner row carries no map to measure (a flat Fill) -- the material's canvas, the size the row's
 * own bake uses. False when neither names a positive size.
 */
static bool paint_layer_chain_member_dimensions_resolve(
    const Material &ma,
    Span<PaintMaterialCompositeImageLayer> input,
    const MaterialPaintLayer &member,
    const int tile_number,
    int &r_width,
    int &r_height,
    bool &r_from_slot)
{
  if (paint_layer_effect_dimensions_resolve(
          input, member, tile_number, r_width, r_height, r_from_slot))
  {
    return true;
  }
  const int canvas = int(ma.paint_layers_image_size);
  if (canvas <= 0) {
    return false;
  }
  r_width = r_height = canvas;
  r_from_slot = false;
  return true;
}

/** An RGBA float buffer of \a width x \a height, owned by the returned shared pointer. */
static std::shared_ptr<ImBuf> anchor_slice_buffer_new(const int width, const int height)
{
  ImBuf *ibuf = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    return {};
  }
  return std::shared_ptr<ImBuf>(ibuf, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

/**
 * The pixels a slice render writes: the whole buffer for a complete render, else \a dst_rect
 * (x0, y0, x1, y1) clamped to it. False when nothing is left to render.
 */
static bool anchor_slice_region_get(const int *dst_rect,
                                    const int width,
                                    const int height,
                                    int &r_x0,
                                    int &r_y0,
                                    int &r_x1,
                                    int &r_y1)
{
  r_x0 = 0;
  r_y0 = 0;
  r_x1 = width;
  r_y1 = height;
  if (dst_rect != nullptr) {
    r_x0 = clamp_i(dst_rect[0], 0, width);
    r_y0 = clamp_i(dst_rect[1], 0, height);
    r_x1 = clamp_i(dst_rect[2], 0, width);
    r_y1 = clamp_i(dst_rect[3], 0, height);
  }
  return r_x1 > r_x0 && r_y1 > r_y0;
}

/** The unwritten part of a partial render is defined as zero, never uninitialized memory. A
 * complete render writes every pixel, so it skips the pass. */
static void anchor_slice_buffers_clear_if_partial(const int *dst_rect,
                                                  ImBuf &color,
                                                  ImBuf &coverage)
{
  if (dst_rect == nullptr) {
    return;
  }
  const size_t size = size_t(color.x) * color.y * sizeof(float[4]);
  memset(color.float_data_for_write(), 0, size);
  memset(coverage.float_data_for_write(), 0, size);
}

int BKE_paint_layers_tile_resolve(const int tile_number,
                                  const PaintMaterialCompositeSurface *surface,
                                  PaintLayerSurfaceRef painted_object)
{
  if (tile_number != PAINT_TILE_NUMBER_AUTO) {
    return tile_number;
  }
  /* The surface knows the UDIM tile of the UVs it was rasterized from; without one the painted
   * object's own UVs decide, and the base tile stays the answer for everything else. */
  if (surface != nullptr) {
    return surface->tile_number;
  }
  if (painted_object.snapshot != nullptr) {
    /* Taken on the main thread: the object itself is not read from a worker. */
    return painted_object.snapshot->uv_tile;
  }
  return BKE_paint_layers_gradient_surface_object_tile(painted_object);
}

bool BKE_paint_layers_slice_describe(const Material &ma,
                                     const MaterialPaintLayer &reader,
                                     const int channel,
                                     PaintMaterialCompositeImageLayer &r_entry,
                                     const int *slice_rect,
                                     const bool dimensions_only,
                                     const bool allow_empty_mask,
                                     const PaintLayerTileContext &context)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  /* One resolution of the unset tile here, so everything below keys on the concrete number. */
  const int tile_number = BKE_paint_layers_tile_resolve(
      context.tile_number, nullptr, context.painted_object);
  PaintLayerSurfaceRef painted_object = context.painted_object;
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr ||
      ref.point == nullptr)
  {
    return false;
  }
  r_entry = PaintMaterialCompositeImageLayer{};
  if (ref.target == PaintLayerAnchorPointTarget::Mask) {
    /* A mask slice is the source row's mask stack up to the point, the base mask included as its
     * first item. It has no opacity, blend or mask of its own, and reads the stack straight: the
     * base's switch hides the mask from the row, not from a reader of the slice. */
    composite_mask_items_describe(ma,
                                  *ref.source_row,
                                  r_entry.mask_items,
                                  tile_number,
                                  &ref.point->marker,
                                  slice_rect,
                                  dimensions_only,
                                  /*describe_only=*/false,
                                  /*context_surface=*/nullptr,
                                  /*hidden_with_base=*/false,
                                  painted_object);
    if (r_entry.mask_items.is_empty() && !allow_empty_mask) {
      return false;
    }
  }
  else {
    if (!composite_image_layer_build(ma,
                                     *ref.source_row,
                                     channel,
                                     1.0f,
                                     false,
                                     true,
                                     r_entry,
                                     tile_number,
                                     &ref.point->marker,
                                     slice_rect,
                                     dimensions_only,
                                     /*describe_only=*/false,
                                     /*context_surface=*/nullptr,
                                     /*height_won_cache=*/nullptr,
                                     PaintLayerBakeScope::WholeRow,
                                     painted_object))
    {
      return false;
    }
    /* The slice is the row's content before its opacity, mask, object mask and blend: only what
     * the corrections above the marker left. */
    r_entry.mask_items.clear();
    r_entry.mask_image = nullptr;
    r_entry.mask_iuser = nullptr;
    r_entry.mask_from_alpha = false;
    r_entry.mask_influence = 0.0f;
  }
  r_entry.opacity = 1.0f;
  r_entry.enabled = true;
  return true;
}

/** The size of the slice of \a reader on \a tile_number: the source row's, like a bake of that
 * row. */
static bool anchor_slice_dimensions(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    const int channel,
                                    const PaintLayerAnchorPointRef &ref,
                                    int &r_width,
                                    int &r_height,
                                    PaintLayerSurfaceRef painted_object,
                                    const int tile_number)
{
  r_width = 0;
  r_height = 0;
  if (ref.target == PaintLayerAnchorPointTarget::Mask) {
    return BKE_paint_layers_row_dimensions(
               ma,
               *ref.source_row,
               PAINT_MATERIAL_CHANNEL_BASE_COLOR,
               r_width,
               r_height,
               PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}) &&
           r_width > 0 && r_height > 0;
  }
  /* Described without rendering, so the nested slices this one reads are not rendered (or
   * allocated) just to learn a size. */
  PaintMaterialCompositeImageLayer entry;
  return BKE_paint_layers_slice_describe(
             ma,
             reader,
             channel,
             entry,
             nullptr,
             true,
             false,
             PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}) &&
         BKE_paint_material_composite_stack_dimensions(
             Span<PaintMaterialCompositeImageLayer>(&entry, 1), r_width, r_height) &&
         r_width > 0 && r_height > 0;
}

/**
 * Render the slice of \a reader on \a tile_number into the buffers, which are already of the
 * slice's size: the whole of them when \a rect is null, else the rectangle (x0, y0, x1, y1),
 * leaving the rest as it is. False when the slice cannot be rendered there.
 */
static bool anchor_slice_fill(const Material &ma,
                              const MaterialPaintLayer &reader,
                              const int channel,
                              const PaintLayerAnchorPointRef &ref,
                              const int *rect,
                              ImBuf &color,
                              ImBuf &coverage,
                              PaintLayerSurfaceRef painted_object,
                              const int tile_number)
{
  const int width = color.x;
  const int height = color.y;
  int rx0, ry0, rx1, ry1;
  if (!anchor_slice_region_get(rect, width, height, rx0, ry0, rx1, ry1)) {
    return false;
  }
  float *color_pixels = color.float_data_for_write();
  float *coverage_pixels = coverage.float_data_for_write();

  if (ref.target == PaintLayerAnchorPointTarget::Mask) {
    /* The slice reads the stack straight, the base's switch included (see
     * #BKE_paint_layers_slice_describe). The tile is the one the slice is rendered for: the
     * stack's own maps are acquired on it, so a tiled mask item contributes the tile's pixels. */
    PaintLayersMaskSampler sampler(
        ma, *ref.source_row, &ref.point->marker, false, painted_object);
    sampler.tile_acquire(tile_number, width, height);
    for (int y = ry0; y < ry1; y++) {
      for (int x = rx0; x < rx1; x++) {
        const int64_t index = int64_t(y) * width + x;
        const float gray = sampler.sample(tile_number, x, y);
        color_pixels[index * 4 + 0] = gray;
        color_pixels[index * 4 + 1] = gray;
        color_pixels[index * 4 + 2] = gray;
        /* A mask point read as content is opaque grey; read as a mask item it is the scalar. */
        color_pixels[index * 4 + 3] = 1.0f;
        coverage_pixels[index * 4 + 0] = 1.0f;
        coverage_pixels[index * 4 + 1] = 1.0f;
        coverage_pixels[index * 4 + 2] = 1.0f;
        coverage_pixels[index * 4 + 3] = 1.0f;
      }
    }
    return true;
  }

  PaintMaterialCompositeImageLayer entry;
  /* Nested readers inside the content slice must see the same rectangle: without it every nested
   * slice would render fully even when only a rectangle is wanted. */
  if (!BKE_paint_layers_slice_describe(
          ma,
          reader,
          channel,
          entry,
          rect,
          false,
          false,
          PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}))
  {
    return false;
  }
  int entry_width = 0;
  int entry_height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(
          Span<PaintMaterialCompositeImageLayer>(&entry, 1), entry_width, entry_height) ||
      entry_width != width || entry_height != height)
  {
    return false;
  }
  const int region_w = rx1 - rx0;
  const int region_h = ry1 - ry0;
  const bool full = rect == nullptr;
  /* A complete render evaluates straight into the colour buffer; only a partial one needs a
   * region-sized scratch to copy from. The coverage is a scalar plane either way, widened below. */
  Vector<float> region_color;
  rcti region;
  BLI_rcti_init(&region, rx0, rx1, ry0, ry1);
  float *color_out = color_pixels;
  if (!full) {
    region_color.resize(int64_t(region_w) * region_h * 4);
    color_out = region_color.data();
  }
  Vector<float> region_coverage(int64_t(region_w) * region_h);
  if (!BKE_paint_material_composite_eval_row_content(
          entry, color_out, region_coverage.data(), full ? nullptr : &region, channel))
  {
    return false;
  }
  for (int y = ry0; y < ry1; y++) {
    for (int x = rx0; x < rx1; x++) {
      const int64_t src = int64_t(y - ry0) * region_w + (x - rx0);
      const int64_t dst = int64_t(y) * width + x;
      if (!full) {
        memcpy(color_pixels + dst * 4, region_color.data() + src * 4, sizeof(float[4]));
      }
      /* The consumers read the coverage buffer as RGBA with the value in each colour channel. */
      const float value = region_coverage[src];
      coverage_pixels[dst * 4 + 0] = value;
      coverage_pixels[dst * 4 + 1] = value;
      coverage_pixels[dst * 4 + 2] = value;
      coverage_pixels[dst * 4 + 3] = 1.0f;
    }
  }
  return true;
}

/** Render the slice of \a reader on \a tile_number into buffers of its own: all of it for a null
 * \a dst_rect, else that rectangle with the rest zero. */
static bool anchor_slice_render_new(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    const int channel,
                                    const PaintLayerAnchorPointRef &ref,
                                    const int *dst_rect,
                                    std::shared_ptr<ImBuf> &r_color,
                                    std::shared_ptr<ImBuf> &r_coverage,
                                    PaintLayerSurfaceRef painted_object,
                                    const int tile_number)
{
  int width = 0;
  int height = 0;
  if (!anchor_slice_dimensions(
          ma, reader, channel, ref, width, height, painted_object, tile_number))
  {
    return false;
  }
  int rx0, ry0, rx1, ry1;
  if (!anchor_slice_region_get(dst_rect, width, height, rx0, ry0, rx1, ry1)) {
    return false;
  }
  std::shared_ptr<ImBuf> color = anchor_slice_buffer_new(width, height);
  std::shared_ptr<ImBuf> coverage = anchor_slice_buffer_new(width, height);
  if (color == nullptr || coverage == nullptr) {
    return false;
  }
  anchor_slice_buffers_clear_if_partial(dst_rect, *color, *coverage);
  if (!anchor_slice_fill(
          ma, reader, channel, ref, dst_rect, *color, *coverage, painted_object, tile_number))
  {
    return false;
  }
  r_color = std::move(color);
  r_coverage = std::move(coverage);
  return true;
}

/**
 * Whether a rectangle reported on one of \a images can be applied to the slice as it is: the slice
 * is described without remapping any map, and no map that was ever edited has another size than the
 * slice. Anything else is a full render, which is always right.
 */
static bool anchor_slice_region_is_exact(const Material &ma,
                                         const MaterialPaintLayer &reader,
                                         const int channel,
                                         const Span<Image *> images,
                                         const int width,
                                         const int height,
                                         const int tile_number)
{
  PaintMaterialCompositeImageLayer entry;
  if (!BKE_paint_layers_slice_describe(
          ma,
          reader,
          channel,
          entry,
          nullptr,
          true,
          false,
          PaintLayerTileContext{.tile_number = tile_number}) ||
      !anchor_slice_layer_is_texel_exact(entry))
  {
    return false;
  }
  for (const Image *image : images) {
    int image_width = 0;
    int image_height = 0;
    BKE_image_pixel_journal_size(*image, image_width, image_height);
    /* An image no edit was marked on is unchanged, whatever its size. */
    if (image_width > 0 && (image_width != width || image_height != height)) {
      return false;
    }
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Paint Layer Effect Cache
 *
 * A CPU-computed effect (a Blur, a Noise) cannot be built as nodes, and its result is not a
 * pointwise function of one texel: the whole input map is read. The cache holds the result, one
 * float buffer per (material, row, channel, parameters), with the input rendered through the same
 * description the bake renders -- the row's content up to the effect, before its mask and opacity
 * -- and the kernel applied over it. Freshness follows the slice cache: the entry remembers the
 * version of every map the input was rendered from, and an edit of one recomputes what moved --
 * the whole buffer when the change cannot be narrowed (a reload, a remapping between a map and
 * the canvas), or a region patch when it can: the input re-rendered on the changed rectangle
 * grown by the kernel's halo, the kernel run over the small buffer whole, and the interior copied
 * back into the cached result. The region arithmetic is the GPU plan's own
 * (#BKE_paint_layers_effect_halo, #BKE_paint_layers_effect_region_is_exact), so both paths name
 * the same valid rectangle for the same stroke.
 * \{ */

/**
 * Total memory the effect cache may hold: a 4K RGBA float buffer is 256 MiB per effect, and this
 * cache stands beside the slice cache (4 GiB) and the composite cache, whose working sets overlap
 * on the same strokes. One GiB holds a couple of recent effects at 4K and the small ones at lower
 * sizes; an entry evicted between two evaluations is computed whole again, and an entry larger than
 * the budget is never cached.
 */
constexpr size_t PAINT_LAYER_EFFECT_CACHE_BYTES = size_t(1024) * 1024 * 1024;

/** The live budget; only tests override it, to make eviction observable without 4K fixtures. */
static size_t g_paint_effect_cache_budget = PAINT_LAYER_EFFECT_CACHE_BYTES;

/** The cache owns references only; dropping an entry simply releases them. The release callable of
 * the shared store is a no-op here: the buffer's last reference dies with the entry. */
static PaintLayerEffectCacheStore<EffectCacheEntry> g_paint_effect_cache;
/**
 * Guards the container and the byte count only. A bake job computes effects on a worker thread
 * while the main thread describes the composite, and both reach the cache. It is never held while
 * an effect computes, so the input's own nested renders cannot deadlock on it.
 */
static std::mutex g_paint_effect_cache_mutex;

/** Cropped partial computes since the process started; see #paint_layer_effect_partial_compute. */
static std::atomic<size_t> g_paint_layer_effect_partial_compute_count{0};

/**
 * A description hash of the effect: the kind and the parameters in its schema order. The input's
 * pixels are not part of it -- the sources' versions stand in for those -- so an edit of a value
 * moves it and an edit of a map does not. \a instance_marker is the composite marker a Smart Filter
 * chain's member is keyed under (see the declaration in `paint_layers_effect_cache_intern.hh`):
 * the member's kind, parameters and DNA marker are the definition's own, so the marker is the one
 * fold that tells two instances of one asset apart in both effect caches. Nil folds nothing.
 */
uint64_t paint_layer_effect_hash(const MaterialPaintLayer &effect,
                                 const int channel,
                                 const bUUID &instance_marker)
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix_u32 = [&](const uint32_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto mix_float = [&](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    mix_u32(bits);
  };
  mix_u32(uint32_t(effect.kind_subtype));
  mix_u32(uint32_t(channel));
  /* A kind that reads an Anchor Point's input is the slice the element references (as a mask
   * item) or the channel the row lays (as a correction, its reference unset): the reference
   * belongs in the key, so a retarget cannot keep the grey the old target made. */
  if (paint_layer_kind_reads_anchor_point(effect)) {
    const uint64_t source_hash = UUID(effect.anchor_point_source).hash();
    mix_u32(uint32_t(source_hash));
    mix_u32(uint32_t(source_hash >> 32));
  }
  /* The instance fold comes before the parameters, so the plain hash (a nil marker) and the
   * instanced one never collide on the same parameters: the marker's halves join the fold here,
   * the same two-u64 shape the GPU plan's fingerprint folds a uuid by. */
  if (!BLI_uuid_is_nil(instance_marker)) {
    uint64_t marker_halves[2];
    memcpy(marker_halves, &instance_marker, sizeof(marker_halves));
    mix_u32(uint32_t(marker_halves[0]));
    mix_u32(uint32_t(marker_halves[0] >> 32));
    mix_u32(uint32_t(marker_halves[1]));
    mix_u32(uint32_t(marker_halves[1] >> 32));
  }
  float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  composite_filter_params(effect, params);
  for (const float param : params) {
    mix_float(param);
  }
  /* A transform effect has no schema parameters: its numbers are its mapping, which a mask item's
   * cached grey is the result of. They are read through the accessors the step reads them by, so a
   * stored but unused bit (a direction without its axis, a Mirror that is off) never re-bakes. */
  if (paint_layer_kind_transforms_input(effect)) {
    float offset[2];
    BKE_paint_layers_transform_effective_offset(effect, offset);
    mix_u32(BKE_paint_layers_mapping_enabled_get(effect) ? 1u : 0u);
    mix_u32(uint32_t(BKE_paint_layers_mapping_wrap_get(effect)));
    mix_u32(uint32_t(BKE_paint_layers_mapping_mirror_get(effect)));
    mix_float(offset[0]);
    mix_float(offset[1]);
    mix_float(effect.mapping.scale[0]);
    mix_float(effect.mapping.scale[1]);
    mix_float(effect.mapping.rotation);
    mix_float(effect.mapping.mirror_center[0]);
    mix_float(effect.mapping.mirror_center[1]);
  }
  /* A colour's three channels join the scalar fold through the packing now; its alpha is what no
   * kernel reads from the packed array, so it joins the hash here -- a Glow's or a Color Match's
   * colour edit still retires the cached result either way. */
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(effect)) {
    for (const int i : IndexRange(kind->params_num)) {
      if (ELEM(kind->params[i].type,
               PaintLayerKindParamType::Color,
               PaintLayerKindParamType::Vector))
      {
        /* The fourth slot stays zero for a vector (three rides the packing, no fourth), so the
         * fold is stable whatever the stack frame held. */
        float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (kind->params[i].type == PaintLayerKindParamType::Color) {
          paint_layer_kind_param_color_get(effect, i, color);
        }
        else {
          paint_layer_kind_param_vector_get(effect, i, color);
        }
        for (const float component : color) {
          mix_float(component);
        }
      }
    }
    /* The fold above reads the packed layout, which is a fixed
     * #PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM slots. A dynamic schema has no such bound, so a
     * scalar parameter past the window would never move the key and the cache would serve the old
     * pixels after its edit. Fold the dynamic kind's whole schema; the colors and vectors were
     * already folded above and are skipped here. */
    if ((kind->flags & PAINT_LAYER_KIND_DYNAMIC_SCHEMA) != 0) {
      for (const int i : IndexRange(kind->params_num)) {
        const PaintLayerKindParamType type = kind->params[i].type;
        if (ELEM(type,
                 PaintLayerKindParamType::Color,
                 PaintLayerKindParamType::Vector,
                 PaintLayerKindParamType::Ramp,
                 PaintLayerKindParamType::Curves,
                 PaintLayerKindParamType::ColorBalance,
                 PaintLayerKindParamType::ChannelMixer))
        {
          continue;
        }
        mix_float(paint_layer_kind_param_get(effect, i));
      }
    }
  }
  /* A dynamic kind's definition is part of its answer: an edit of the tree (a node-group filter's
   * interface or graph, a chain definition's members) retires every result the old definition
   * made. Zero for every kind that answers from its own schema. */
  const uint64_t definition_hash = BKE_paint_layers_node_group_definition_hash(effect);
  if (definition_hash != 0) {
    mix_u32(uint32_t(definition_hash));
    mix_u32(uint32_t(definition_hash >> 32));
  }
  /* The kind's own slot (a Channel Split's image) is the one map the kernel samples itself, so its
   * identity joins the hash: assigning another image makes a key the old entry never answers. The
   * pixels are the subscriptions' business, the way the input's maps' are. */
  paint_layer_effect_hash_mix_slot(hash, effect);
  return hash;
}

bool operator==(const PaintLayerEffectKeyExtras &a, const PaintLayerEffectKeyExtras &b)
{
  return a.chain_hash == b.chain_hash && a.input_scalar_hash == b.input_scalar_hash;
}

bool paint_correction_is_effect(const PaintMaterialCompositeCorrection &correction)
{
  return !BLI_uuid_is_nil(correction.effect_marker);
}

bool BKE_paint_layers_effect_input_describe(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            const MaterialPaintLayer &effect,
                                            const int channel,
                                            const bool dimensions_only,
                                            const bool describe_only,
                                            PaintMaterialCompositeImageLayer &r_entry,
                                            const PaintLayerTileContext &context)
{
  /* A Height To Normal effect reads the Height the row lays, whichever channel it hangs on: its
   * input is the Height content, and the normal its kernel computes lays into the channel the
   * effect itself works on. The other kinds read their own channel. */
  const int input_channel = paint_layer_effect_input_channel(effect, channel);
  r_entry = PaintMaterialCompositeImageLayer{};
  const int tile_number = BKE_paint_layers_tile_resolve(
      context.tile_number, nullptr, context.painted_object);
  if (!composite_image_layer_build(ma,
                                   row,
                                   input_channel,
                                   1.0f,
                                   false,
                                   true,
                                   r_entry,
                                   tile_number,
                                   &effect.marker,
                                   nullptr,
                                   dimensions_only,
                                   describe_only,
                                   nullptr,
                                   nullptr,
                                   PaintLayerBakeScope::WholeRow,
                                   context.painted_object))
  {
    return false;
  }
  /* The input is the row's content before its opacity, mask, object mask and blend: only what the
   * corrections below the effect left. */
  r_entry.mask_items.clear();
  r_entry.mask_image = nullptr;
  r_entry.mask_iuser = nullptr;
  r_entry.mask_from_alpha = false;
  r_entry.mask_influence = 0.0f;
  r_entry.opacity = 1.0f;
  r_entry.enabled = true;
  /* A Channel Split's slot is the one map its kernel samples itself, outside the effect's own
   * ping-pong buffers: the gather binds it for the slot-fetch pass and keys the GPU entry on its
   * identity, which no map version of the input carries. The kind table decides, so an effect whose
   * kind reads no slot names none, whatever a stale pointer on the row holds. */
  r_entry.slot_image = paint_layer_effect_slot_image(effect);
  return true;
}

void BKE_paint_layers_effect_input_images(const PaintMaterialCompositeImageLayer &layer,
                                          Vector<Image *> &r_images)
{
  const auto add = [&](Image *image) {
    if (image != nullptr && image->runtime != nullptr && !r_images.contains(image)) {
      r_images.append(image);
    }
  };
  add(layer.color_image);
  add(layer.mask_image);
  /* The slot map a Channel Split samples itself is part of what the result reads, so a stroke on
   * it makes the entry stale the way a stroke on the input's own maps does. */
  add(layer.slot_image);
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    add(correction.image);
    add(correction.coverage_image);
    add(correction.anchor_point_coverage_image);
    for (Image *image : correction.grey_input_images) {
      add(image);
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    add(item.image);
    add(item.coverage_image);
    for (Image *image : item.grey_input_images) {
      add(image);
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    BKE_paint_layers_effect_input_images(child, r_images);
  }
}

/** Render the input of \a effect on \a tile_number and apply its kernel, into a buffer of the
 * input's own size. */
static bool paint_layer_effect_compute(const Material &ma,
                                       const MaterialPaintLayer &row,
                                       const MaterialPaintLayer &effect,
                                       const int channel,
                                       const int tile_number,
                                       std::shared_ptr<ImBuf> &r_color,
                                       Vector<AnchorSliceSource> &r_synced,
                                       PaintLayerSurfaceRef painted_object)
{
  /* Timed in two halves because the GPU plan replaces them differently: the kernel is what an
   * effect pass computes instead, while the input render is what the gather lays with its own
   * passes. The whole recompute is measured too, so a phase's win is this line stopping to print.
   * The region is always the full buffer: see the note above the cache this feeds. */
  bke::paint_perf::ScopedTimer perf_total("effect.cpu.total");
  PaintMaterialCompositeImageLayer entry;
  std::shared_ptr<ImBuf> color;
  int width = 0;
  int height = 0;
  {
    bke::paint_perf::ScopedTimer perf_input("effect.cpu.input");
    if (!BKE_paint_layers_effect_input_describe(
            ma,
            row,
            effect,
            channel,
            /*dimensions_only=*/false,
            /*describe_only=*/false,
            entry,
            PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}))
    {
      return false;
    }
    bool from_slot = false;
    if (!paint_layer_effect_dimensions_resolve(Span<PaintMaterialCompositeImageLayer>(&entry, 1),
                                               effect,
                                               tile_number,
                                               width,
                                               height,
                                               from_slot))
    {
      return false;
    }
    color = anchor_slice_buffer_new(width, height);
    if (color == nullptr) {
      return false;
    }
    /* A row with no map has no content to render: the slot's own size gave the canvas, and the
     * kernel lays its result over the empty buffer. */
    if (!from_slot) {
      Vector<float> coverage(int64_t(width) * height);
      if (!BKE_paint_material_composite_eval_row_content(
              entry, color->float_data_for_write(), coverage.data(), nullptr, channel))
      {
        return false;
      }
    }
    perf_input.note("kind=%d size=%dx%d region=full", int(effect.kind_subtype), width, height);
  }
  /* The kernel reads the buffer the block above filled and leaves the alpha alone -- unless the kind
   * lays a layer of its own (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA); the scope ends before it, so the
   * two halves of the recompute are not timed together. A kernel that answers false -- a node-group
   * filter with no backend, or one asked off the main thread -- leaves no answer to cache: the
   * caller above keeps the entry unrendered, so a bake worker's ask never files the input back as
   * the effect's output. */
  if (!paint_layer_effect_kernel_apply(*color, effect, /*for_mask=*/false, tile_number)) {
    return false;
  }
  /* The versions are taken after the render: an edit that landed while it ran is read next time.
   * The kind's own slot joins the input's maps: the kernel samples it directly, so an edit of its
   * pixels must reach the entry the same way an edit of the input's maps does. */
  Vector<Image *> images;
  paint_layer_effect_input_images_append(
      effect, images, [&](Vector<Image *> &r_input_images) {
        BKE_paint_layers_effect_input_images(entry, r_input_images);
      });
  r_synced = map_sources_snapshot(images);
  r_color = std::move(color);
  perf_total.note("kind=%d size=%dx%d region=full", int(effect.kind_subtype), width, height);
  return true;
}

/**
 * The input of the Smart Filter chain member \a member of the instance \a chain: the owner row's
 * content up to the chain row -- the same base the chain row's own input describe builds, the
 * chain row being a DNA effect of its owner and stopping the walk there -- with the corrections of
 * the members before \a member folded in, so the input of member k is the chain's content with
 * members 0..k-1 laid. The prefix is laid by the same unfolding the description lays
 * (#composite_effect_append_chain_impl, bounded at this member), at the effect input's own
 * pass-through of one, so the canvas a member is computed from is the chain as the composite lays
 * it and never a re-derivation that could drift from the description the CPU composite mixes and
 * the GPU preview renders. A pointwise prefix member lays its filter correction; a GPU-computed
 * one lays its effect correction, whose result comes from the cache under its own composite
 * marker (computed on a miss, named by its maps in describe-only) -- the same recursion the plain
 * path runs when a row chains one CPU-computed effect into the next, terminating because the
 * prefix shrinks by one member per level.
 *
 * This is the one description the member's CPU effect cache renders from and the GPU preview's
 * member job lays from (#BKE_paint_layers_chain_member_input_describe is its public form), so the
 * two cannot disagree about what a chain member is computed on -- the contract
 * #BKE_paint_layers_effect_input_describe holds for a direct effect, held here for a member.
 *
 * The input channel is the chain row's own (#paint_layer_effect_input_channel of the chain): the
 * chain works in the channel the description works in, and a member's kernel runs over that
 * canvas. A cross-channel member (a Height To Normal) reads the chain's working canvas rather than
 * the row's map of its input channel -- the chain has no cross-channel plumbing (v3 keeps the one
 * canvas the chain input names); such a member on a colour channel is refused by the gates the
 * unfolding runs before this point anyway.
 */
static bool paint_layer_chain_member_input_describe(const Material &ma,
                                                    const MaterialPaintLayer &row,
                                                    const MaterialPaintLayer &chain,
                                                    const MaterialPaintLayer &member,
                                                    const int channel,
                                                    const bool dimensions_only,
                                                    const bool describe_only,
                                                    PaintMaterialCompositeImageLayer &r_entry,
                                                    const PaintLayerTileContext &context)
{
  /* The base is the chain's own input, described exactly as the chain row's would be: the row's
   * content up to the chain's marker, the row's opacity, mask and blend cleared. */
  if (!BKE_paint_layers_effect_input_describe(
          ma, row, chain, channel, dimensions_only, describe_only, r_entry, context))
  {
    return false;
  }
  /* The prefix, laid into the same entry: the members before this one, as the description carries
   * them. The tile is resolved once here, the same way the base describe resolved it. */
  const int tile_number = BKE_paint_layers_tile_resolve(
      context.tile_number, nullptr, context.painted_object);
  composite_effect_append_chain_impl(ma,
                                     &row,
                                     chain,
                                     channel,
                                     /*pass_through_scale=*/1.0f,
                                     describe_only,
                                     tile_number,
                                     context.painted_object,
                                     &member,
                                     r_entry);
  /* The member's own correction, describe-only: the GPU job's input describe names it beside the
   * prefix (the unfolding above is bounded before the member). The compute path leaves it out --
   * the member's kernel is applied to the rendered input after this description. */
  if (describe_only) {
    const Material *definition = chain.material;
    const PaintLayerKindInfo *member_kind = paint_layer_kind_of(member);
    if (definition != nullptr && member_kind != nullptr &&
        paint_layer_effect_is_cpu_computed(member) &&
        paint_layer_effect_is_smart_filter_member(member) &&
        paint_layer_effect_filters(*definition, member, channel))
    {
      const IDProperty *system_properties =
          (chain.system_properties != nullptr && chain.system_properties->type == IDP_GROUP) ?
              chain.system_properties :
              nullptr;
      composite_chain_member_effect_append(ma,
                                           row,
                                           chain,
                                           member,
                                           *member_kind,
                                           system_properties,
                                           channel,
                                           /*pass_through_scale=*/1.0f,
                                           /*describe_only=*/true,
                                           tile_number,
                                           context.painted_object,
                                           r_entry);
    }
  }
  return true;
}

/**
 * Render the input of the chain member \a member and apply its kernel, into a buffer of the
 * input's own size. The twin of #paint_layer_effect_compute for a Smart Filter chain's
 * GPU-computed member: the same input render, the same region contract -- the whole buffer, a
 * neighbourhood reads past an edited rectangle -- and the member's kernel over it, the parameters
 * a node-group member reads taken from the merged view the instance's overrides make
 * (#composite_chain_member_parameters). A kernel that answers false leaves no answer to cache, the
 * same rule the plain path holds.
 */
static bool paint_layer_chain_member_effect_compute(const Material &ma,
                                                    const MaterialPaintLayer &row,
                                                    const MaterialPaintLayer &chain,
                                                    const MaterialPaintLayer &member,
                                                    const int channel,
                                                    const int tile_number,
                                                    std::shared_ptr<ImBuf> &r_color,
                                                    Vector<AnchorSliceSource> &r_synced,
                                                    PaintLayerSurfaceRef painted_object)
{
  bke::paint_perf::ScopedTimer perf_total("effect.cpu.total");
  PaintMaterialCompositeImageLayer entry;
  std::shared_ptr<ImBuf> color;
  int width = 0;
  int height = 0;
  {
    bke::paint_perf::ScopedTimer perf_input("effect.cpu.input");
    if (!paint_layer_chain_member_input_describe(
            ma,
            row,
            chain,
            member,
            channel,
            /*dimensions_only=*/false,
            /*describe_only=*/false,
            entry,
            PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}))
    {
      return false;
    }
    bool from_slot = false;
    if (!paint_layer_chain_member_dimensions_resolve(
            ma,
            Span<PaintMaterialCompositeImageLayer>(&entry, 1),
            member,
            tile_number,
            width,
            height,
            from_slot))
    {
      return false;
    }
    color = anchor_slice_buffer_new(width, height);
    if (color == nullptr) {
      return false;
    }
    /* A row with no map has no content to render: the slot's own size gave the canvas, and the
     * kernel lays its result over the empty buffer. */
    if (!from_slot) {
      Vector<float> coverage(int64_t(width) * height);
      if (!BKE_paint_material_composite_eval_row_content(
              entry, color->float_data_for_write(), coverage.data(), nullptr, channel))
      {
        return false;
      }
    }
    perf_input.note("kind=%d size=%dx%d region=full", int(member.kind_subtype), width, height);
  }
  /* The member's kernel, over the chain's content with the prefix laid. A node-group member reads
   * the instance's exposed values, which live on the chain row under the member keys: the merged
   * group is built for the kernel call and freed when it is done, the lifetime the backend's
   * synchronous render covers. Every other kind reads no parameters_override and runs on the
   * member's own schema exactly as a direct effect of the same kind does. */
  /* Every CPU-computed member reads its schema parameters through the row's group, and the
   * instance's exposed values live on the chain row: the merge is handed to the kernel for all of
   * them (the kernel applies it to the kind's own accessors, not only the node-group branch). A
   * pointwise member never reaches here; its values ride the packed description instead. */
  IDProperty *kernel_parameters = composite_chain_member_parameters(chain, member);
  const bool answered = paint_layer_effect_kernel_apply(
      *color, member, /*for_mask=*/false, tile_number, kernel_parameters);
  if (kernel_parameters != nullptr) {
    IDP_FreeProperty(kernel_parameters);
  }
  if (!answered) {
    return false;
  }
  /* The versions are taken after the render, the plain path's rule. The member reads no slot of
   * its own -- the contract refuses the kinds that do -- so the append folds nothing beside the
   * input's maps, and the maps include the prefix's own corrections' maps. */
  Vector<Image *> images;
  paint_layer_effect_input_images_append(
      member, images, [&](Vector<Image *> &r_input_images) {
        BKE_paint_layers_effect_input_images(entry, r_input_images);
      });
  r_synced = map_sources_snapshot(images);
  r_color = std::move(color);
  perf_total.note("kind=%d size=%dx%d region=full", int(member.kind_subtype), width, height);
  return true;
}

/**
 * Whether the CPU kernel of \a effect is a local function of the input's neighbourhood, so a
 * region patch can run the whole kernel over a small buffer and copy the interior back. The kinds
 * refused first are the ones the kind table names with no
 * #PAINT_LAYER_KIND_EFFECT_REGION_EXACT -- the single place a new in-local kind registers its
 * refusal -- because their result reads more than the halo names; the halo
 * (#BKE_paint_layers_effect_halo) and the region arithmetic are the GPU plan's, and the plan's
 * reaches do not bound these reads:
 * - A Noise grains by the hash of the texel's absolute coordinates: the small buffer's texel
 *   (x, y) stands for the canvas's (x + xmin, y + ymin), and the grain would land shifted.
 * - A Pixelate cuts the buffer into a grid of the amount's blocks per side -- the block size and
 *   the grid origin both derive from the buffer's own size, which the small buffer does not share.
 * - A Channel Split samples its slot map at the texel's own UV, `x / buffer width`, which on the
 *   small buffer is a different axis of the map than the canvas texel's.
 * - A Blur Slope walks the gradient of a driver it builds itself: the noise mode tiles it by
 *   `x / buffer width` like the Channel Split, and the previous-input mode's taps span the
 *   intensity across the original, farther than the plan's one-texel reach names.
 * The parameterizations a kind cannot name in the table stay as the checks below: a Color Match
 * with a picked source and the variation on jitters by the absolute texel (the non-picked source
 * is a whole-map statistic, whose whole-canvas halo degenerates the region path on its own, and a
 * picked one without the variation is pointwise), and a Height To Normal in world units scales the
 * slope by the buffer's own max dimension (`surface_size / max(width, height)`), which the small
 * buffer does not share.
 * The whole-canvas kinds (a Fill Area's flood, a Bevel's or a Mask Outline's distance field, a
 * non-picked Color Match's reduce) are not named: their plan reaches the whole canvas on both
 * axes, the halo grows the region past it, and the region path refuses itself below -- the full
 * compute is the same work without the clone.
 */
bool paint_layer_effect_region_update_supported(const MaterialPaintLayer &effect)
{
  if (!paint_layer_effect_region_exact(effect)) {
    return false;
  }
  switch (eMaterialPaintLayerFilter(effect.kind_subtype)) {
    case MA_PAINT_LAYER_FILTER_COLOR_MATCH:
      return !(paint_layer_kind_param_bool_get(effect, PAINT_LAYER_COLOR_MATCH_VARIATION) &&
               paint_layer_kind_param_bool_get(effect, PAINT_LAYER_COLOR_MATCH_SOURCE_PICKED));
    case MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL:
      return !paint_layer_kind_param_bool_get(effect, PAINT_LAYER_HEIGHT_TO_NORMAL_WORLD_UNITS);
    default:
      return true;
  }
}

/**
 * The regional update the effect cache runs instead of #paint_layer_effect_compute when a live
 * entry's maps moved by one rectangle: the input re-rendered on the changed region grown by the
 * halo twice, the whole kernel run over the small buffer, and the interior -- the changed region
 * grown by the halo once, the rectangle the patch makes correct -- copied line by line, RGB and
 * alpha, into a private copy of the cached result. The kernel is not changed: it reads and writes
 * a whole #ImBuf, and the small buffer is one, so the full kernel over it is the full compute
 * restricted to the region -- the reads of every texel of the interior stay inside the small
 * buffer (or clamp at an edge the canvas bounds, which the full compute clamps the same way).
 * \a cached is the entry's own buffer, and stays untouched: the patch lands in a clone, so a
 * frame still laying the cached result keeps its pixels.
 *
 * False refuses the patch -- a kind the gate above names, a remapping between a map and the
 * canvas, a whole-canvas halo -- and the caller falls back to the full compute. The versions are
 * taken after the small render, the same rule the full compute stamps by.
 */
static bool paint_layer_effect_region_compute(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &effect,
    const int channel,
    const int tile_number,
    const PaintLayerSurfaceRef painted_object,
    const PaintMaterialCompositeImageLayer &dims_entry,
    const Span<Image *> images,
    const bool from_slot,
    const std::shared_ptr<ImBuf> &cached,
    const rcti &changed,
    std::shared_ptr<ImBuf> &r_patched,
    Vector<AnchorSliceSource> &r_synced,
    rcti &r_valid)
{
  bke::paint_perf::ScopedTimer perf_total("effect.cpu.region");
  const int width = cached->x;
  const int height = cached->y;
  if (!paint_layer_effect_region_update_supported(effect) ||
      !BKE_paint_layers_effect_region_is_exact(dims_entry, images, width, height))
  {
    return false;
  }

  float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  composite_filter_params(effect, params);
  const int halo = BKE_paint_layers_effect_halo(
      eMaterialPaintLayerFilter(effect.kind_subtype), params, width, height);
  rcti valid;
  BKE_paint_layers_effect_region_clip(changed, halo, width, height, valid);
  if (BLI_rcti_is_empty(&valid)) {
    return false;
  }
  if (valid.xmin == 0 && valid.ymin == 0 && valid.xmax >= width && valid.ymax >= height) {
    /* The halo of a whole-canvas kind grew the region past the canvas: the patch would re-render
     * and re-kernel everything, which the full compute does without the clone and the copy. */
    return false;
  }
  /* The small buffer covers the valid region grown by the halo once more: the neighbourhood every
   * texel of the interior reads stays inside it, so the kernel over the small buffer computes the
   * interior exactly as the full compute would. */
  rcti region_small;
  BKE_paint_layers_effect_region_clip(valid, halo, width, height, region_small);
  const int region_width = BLI_rcti_size_x(&region_small);
  const int region_height = BLI_rcti_size_y(&region_small);
  std::shared_ptr<ImBuf> small = anchor_slice_buffer_new(region_width, region_height);
  if (small == nullptr) {
    return false;
  }
  {
    bke::paint_perf::ScopedTimer perf_input("effect.cpu.region.input");
    /* A row with no map has no content to render, the way the full compute renders none: the
     * slot's own size gave the canvas, and the kernel lays its result over the empty buffer. */
    if (!from_slot) {
      /* The input's own describe, rendered the way the full compute renders it: the region only
       * narrows which texels the evaluation walks, the description stays the whole canvas's, so a
       * gradient or a projection lands where the full render would put it. */
      PaintMaterialCompositeImageLayer entry;
      if (!BKE_paint_layers_effect_input_describe(
              ma,
              row,
              effect,
              channel,
              /*dimensions_only=*/false,
              /*describe_only=*/false,
              entry,
              PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}))
      {
        return false;
      }
      Vector<float> coverage(int64_t(region_width) * region_height);
      if (!BKE_paint_material_composite_eval_row_content(entry,
                                                         small->float_data_for_write(),
                                                         coverage.data(),
                                                         &region_small,
                                                         channel,
                                                         width,
                                                         height))
      {
        return false;
      }
    }
    perf_input.note("kind=%d size=%dx%d region=%d,%d-%d,%d",
                    int(effect.kind_subtype),
                    region_width,
                    region_height,
                    region_small.xmin,
                    region_small.ymin,
                    region_small.xmax,
                    region_small.ymax);
  }
  /* The whole kernel over the whole small buffer: the kind's own reading of the buffer's texel
   * grid is the local one the gate above vouched for, and the interior it writes is the interior
   * the copy below takes. */
  if (!paint_layer_effect_kernel_apply(*small, effect, /*for_mask=*/false, tile_number)) {
    /* The kernel refused the render (a kind whose backend cannot answer on this path), so the
     * small buffer holds no trustworthy answer: refuse the patch rather than copy its texels into
     * the cache, which would serve them as a rendered result. */
    return false;
  }

  std::shared_ptr<ImBuf> patched = paint_layer_anchor_slice_buffer_clone(*cached);
  if (patched == nullptr) {
    return false;
  }
  const float *src = small->float_buffer.data;
  float *dst = patched->float_data_for_write();
  for (const int y : IndexRange(valid.ymin, BLI_rcti_size_y(&valid))) {
    const float *src_row = src +
                           (int64_t(y - region_small.ymin) * region_width +
                            int64_t(valid.xmin - region_small.xmin)) *
                               4;
    float *dst_row = dst + (int64_t(y) * width + valid.xmin) * 4;
    memcpy(dst_row, src_row, size_t(BLI_rcti_size_x(&valid)) * sizeof(float[4]));
  }
  /* The versions are taken after the render: an edit that landed while it ran is read next time.
   * The same maps the entry was described from, the slot included. */
  r_synced = map_sources_snapshot(images);
  r_patched = std::move(patched);
  r_valid = valid;
  perf_total.note("kind=%d size=%dx%d region=%d,%d-%d,%d",
                  int(effect.kind_subtype),
                  width,
                  height,
                  valid.xmin,
                  valid.ymin,
                  valid.xmax,
                  valid.ymax);
  return true;
}

/**
 * Whether every correction folded into \a entry is a local function of the input's neighbourhood, so
 * rendering the entry over a rectangle is the full render restricted to it. A whole-map correction
 * below the effect -- a Color Match's reduce, a Fill Area's flood -- reads the whole canvas, and an
 * input rendered over a rectangle would not be the input the full compute sees. Recursive: a folder
 * carries its children's corrections.
 */
static bool paint_layer_effect_input_reach_is_local(const PaintMaterialCompositeImageLayer &entry)
{
  for (const PaintMaterialCompositeCorrection &correction : entry.content_corrections) {
    if (correction.filter == 0) {
      continue;
    }
    const float *params = correction.filter_params;
    if (BKE_paint_layers_effect_reach(
            eMaterialPaintLayerFilter(correction.filter), params) == EffectReach::WholeMap)
    {
      return false;
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : entry.children) {
    if (!paint_layer_effect_input_reach_is_local(child)) {
      return false;
    }
  }
  return true;
}

/**
 * The CPU-computed \a effect on a bake render that only needs \a compute_region of the canvas: the
 * input rendered over that rectangle grown by the kernel's halo, the whole kernel run over the small
 * buffer, and the buffer handed back with its origin. No cache entry is made -- the crop belongs to
 * this bake alone.
 *
 * False refuses the crop and the caller takes the whole-canvas path: a kind whose kernel is not a
 * local function of the neighbourhood, a slot-fed input (its canvas is the slot's own size), a
 * whole-map correction below the effect, an empty or whole-canvas rectangle, or an allocation that
 * fails. The interior a caller samples is \a compute_region; the halo border the kernel needed is
 * part of the buffer but is not the region's own answer.
 *
 * The input is described whole, as the full compute describes it: the rectangle only narrows which
 * texels the evaluation walks, so a mapping or a projection lands where the full render would put
 * it. A mapping is per-texel and does not refuse the crop.
 */
static bool paint_layer_effect_partial_compute(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &effect,
    const int channel,
    const int tile_number,
    const PaintLayerSurfaceRef painted_object,
    const PaintMaterialCompositeImageLayer &dims_entry,
    const bool from_slot,
    const int width,
    const int height,
    const rcti &compute_region,
    std::shared_ptr<ImBuf> &r_color,
    int r_effect_origin[2])
{
  if (!paint_layer_effect_region_update_supported(effect) || from_slot ||
      !paint_layer_effect_input_reach_is_local(dims_entry))
  {
    return false;
  }
  float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  composite_filter_params(effect, params);
  const int halo = BKE_paint_layers_effect_halo(
      eMaterialPaintLayerFilter(effect.kind_subtype), params, width, height);
  rcti valid;
  BKE_paint_layers_effect_region_clip(compute_region, 0, width, height, valid);
  if (BLI_rcti_is_empty(&valid)) {
    return false;
  }
  /* The small buffer covers the requested rectangle grown by the halo once: every texel the kernel
   * reads for the requested rectangle stays inside it. */
  rcti region_small;
  BKE_paint_layers_effect_region_clip(valid, halo, width, height, region_small);
  if (region_small.xmin == 0 && region_small.ymin == 0 && region_small.xmax >= width &&
      region_small.ymax >= height)
  {
    /* No smaller than the canvas: the full compute is the same work without the extra copy. */
    return false;
  }
  const int region_width = BLI_rcti_size_x(&region_small);
  const int region_height = BLI_rcti_size_y(&region_small);
  if (region_width <= 0 || region_height <= 0) {
    return false;
  }
  std::shared_ptr<ImBuf> small = anchor_slice_buffer_new(region_width, region_height);
  if (small == nullptr) {
    return false;
  }
  {
    bke::paint_perf::ScopedTimer perf_partial("effect.cpu.partial");
    PaintMaterialCompositeImageLayer entry;
    if (!BKE_paint_layers_effect_input_describe(
            ma,
            row,
            effect,
            channel,
            /*dimensions_only=*/false,
            /*describe_only=*/false,
            entry,
            PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}))
    {
      return false;
    }
    Vector<float> coverage(int64_t(region_width) * region_height);
    if (!BKE_paint_material_composite_eval_row_content(entry,
                                                       small->float_data_for_write(),
                                                       coverage.data(),
                                                       &region_small,
                                                       channel,
                                                       width,
                                                       height))
    {
      return false;
    }
    if (!paint_layer_effect_kernel_apply(*small, effect, /*for_mask=*/false, tile_number)) {
      /* The kernel refused the render, so the cropped buffer holds no trustworthy answer: refuse
       * the partial compute and let the caller fall back to the full one. */
      return false;
    }
    r_effect_origin[0] = region_small.xmin;
    r_effect_origin[1] = region_small.ymin;
    r_color = std::move(small);
    g_paint_layer_effect_partial_compute_count.fetch_add(1, std::memory_order_relaxed);
    perf_partial.note("kind=%d size=%dx%d region=%d,%d-%d,%d",
                      int(effect.kind_subtype),
                      region_width,
                      region_height,
                      region_small.xmin,
                      region_small.ymin,
                      region_small.xmax,
                      region_small.ymax);
  }
  return true;
}

/**
 * How many cropped partial computes the effect path has run; for a test that has to prove a bake
 * took the partial path rather than the whole-canvas one.
 */
size_t paint_layer_effect_partial_compute_count()
{
  return g_paint_layer_effect_partial_compute_count.load(std::memory_order_relaxed);
}

/** Drop every cached effect buffer, releasing the canvases (a static would outlive the leak check). */
static void paint_layer_effect_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  const auto no_release = [](EffectCacheEntry &) {};
  g_paint_effect_cache.free_all(no_release);
}

void BKE_paint_layers_effect_cache_free_all()
{
  paint_layer_effect_cache_clear();
}

void BKE_paint_layers_effect_cache_free_material(const Material &ma)
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  const auto no_release = [](EffectCacheEntry &) {};
  g_paint_effect_cache.free_material(ma.id.session_uid, no_release);
}

size_t paint_layer_effect_cache_bytes()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  return g_paint_effect_cache.total_bytes;
}

void paint_layer_effect_cache_set_budget(const size_t bytes)
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  g_paint_effect_cache_budget = bytes;
}

size_t paint_layer_effect_cache_budget()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  return g_paint_effect_cache_budget;
}

int paint_layer_effect_cache_entry_count()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  return int(g_paint_effect_cache.entries.size());
}

/**
 * The result of the CPU-computed \a effect on \a row's content in \a channel, as a float RGBA
 * buffer at the input's size, scene linear, the input's content alpha in the fourth component.
 * Cached; the kernel and the input's maps decide when it is computed again, and an edit of the
 * maps that moved by one rectangle costs a region patch instead of the whole recompute (see
 * #paint_layer_effect_region_compute).
 *
 * \param describe_only: learn nothing but the maps the input reads (into \a r_images) -- the
 *        combined preview asks for this, since it falls back to the CPU for these kinds anyway.
 * \param r_images: the maps the input was rendered from; the composite cache subscribes to them,
 *        so a stroke on any of them reaches it even though the effect reads no Image of its own.
 * \param changed_region: the rectangle a caller (a bake that knows the stroke's region) offers for
 *        the regional update; null keeps the old terms, where any map change recomputes the whole
 *        buffer. The rectangle actually patched is the one the entry's own staleness names, which
 *        the cache reads from the maps themselves; the offered band decides what the caller learns
 *        about the answer, not what is patched.
 * \param r_valid_region: the rectangle of the answer that a map-writing caller can lay from it.
 *        The answer is current as a whole whichever way the cycle answered, so an offered band
 *        comes back grown by the halo and clamped to the canvas -- the band the caller's map is
 *        missing, plus what the kernel reads past it -- a standing result with an offered band
 *        included. The whole canvas when no band was offered, when the band cannot be narrowed
 *        (the maps reach the canvas through a remapping), or after a full compute. Empty when the
 *        offered band was empty (nothing moved that a write could narrow) or the compute failed.
 * \param compute_region: the canvas rectangle a bake render needs, or null for a whole-canvas
 *        answer. When the effect's kernel is a local function of the input's neighbourhood and the
 *        input holds no whole-map correction, the result is computed over that rectangle grown by
 *        the halo and returned cropped, with \a r_effect_origin naming where its (0, 0) sits. A
 *        cropped answer never enters the effect cache. Any refusal falls back to the whole canvas.
 * \param r_effect_origin: the crop's canvas origin, zero when the answer is the whole canvas.
 * \param r_effect_cropped: whether the answer is cropped; the origin alone cannot tell, since a
 *        crop that starts at the canvas origin has a zero origin.
 * \return null when the input could not be rendered or \a describe_only was asked.
 */
static std::shared_ptr<ImBuf> paint_layer_effect_buffer_get(const Material &ma,
                                                            const MaterialPaintLayer &row,
                                                            const MaterialPaintLayer &effect,
                                                            const int channel,
                                                            const int tile_number,
                                                            const bool describe_only,
                                                            Vector<Image *> *r_images,
                                                            Image **r_bake_image,
                                                            PaintLayerSurfaceRef painted_object,
                                                            const rcti *changed_region,
                                                            rcti *r_valid_region,
                                                            const rcti *compute_region,
                                                            int *r_effect_origin,
                                                            bool *r_effect_cropped)
{
  /* The maps the input reads and its size, described without rendering anything: the maps go to
   * the composite cache's subscriptions, the size decides hits. */
  PaintMaterialCompositeImageLayer dims_entry;
  Vector<Image *> images;
  int width = 0;
  int height = 0;
  bool dims_from_slot = false;
  if (!BKE_paint_layers_effect_input_describe(
          ma,
          row,
          effect,
          channel,
          true,
          false,
          dims_entry,
          PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}) ||
      !paint_layer_effect_dimensions_resolve(
          Span<PaintMaterialCompositeImageLayer>(&dims_entry, 1),
          effect,
          tile_number,
          width,
          height,
          dims_from_slot))
  {
    return {};
  }
  paint_layer_effect_input_images_append(
      effect, images, [&](Vector<Image *> &r_input_images) {
        BKE_paint_layers_effect_input_images(dims_entry, r_input_images);
      });
  if (r_images != nullptr) {
    *r_images = images;
  }
  if (describe_only) {
    return {};
  }
  if (r_bake_image != nullptr &&
      ELEM(tile_number, PAINT_TILE_NUMBER_BASE, PAINT_TILE_NUMBER_AUTO))
  {
    /* The bake of an effect that owns its result is the answer while it stands: nothing is computed,
     * and the maps above stay the subscriptions of the caller, who is told when they change. The
     * bake is laid for the base tile only, so another tile keeps the cache. */
    if (Image *bake_map = paint_layer_owned_effect_bake_map(ma, effect, channel)) {
      *r_bake_image = bake_map;
      return {};
    }
  }

  /* A bake render that knows its canvas rectangle offers it here: the folded effect then computes
   * that rectangle grown by its halo and returns the cropped buffer with its origin, outside the
   * cache. Every refusal falls through to the whole-canvas path below, which is the answer that is
   * always right. */
  if (compute_region != nullptr) {
    std::shared_ptr<ImBuf> partial;
    int origin[2] = {0, 0};
    if (paint_layer_effect_partial_compute(ma,
                                           row,
                                           effect,
                                           channel,
                                           tile_number,
                                           painted_object,
                                           dims_entry,
                                           dims_from_slot,
                                           width,
                                           height,
                                           *compute_region,
                                           partial,
                                           origin))
    {
      if (r_effect_origin != nullptr) {
        r_effect_origin[0] = origin[0];
        r_effect_origin[1] = origin[1];
      }
      if (r_effect_cropped != nullptr) {
        *r_effect_cropped = true;
      }
      if (r_valid_region != nullptr) {
        BKE_paint_layers_effect_region_clip(*compute_region, 0, width, height, *r_valid_region);
      }
      return partial;
    }
  }

  EffectCacheKey key{ma.id.session_uid,
                     effect.marker,
                     channel,
                     tile_number,
                     paint_layer_effect_hash(effect, channel)};
  /* The input's scalar content (frame, mapping, flat colour, effects below) rides no map version,
   * so the key carries it; the very helper the GPU key uses, transform steps included. With an
   * object the description carries the projection, whose hash holds the surface identity (mesh,
   * slots), so two objects sharing the material keep separate entries.
   * The chain fingerprint joins them the same way the GPU key builds it: the described input's
   * corrections are exactly the effects below this one, and #BKE_paint_layers_effect_chain_hash
   * stops at the effect's own marker, which the description does not carry -- so the fold names
   * the same data source the preview's GPU jobs name. A memory-only key: nothing here reaches the
   * file. */
  key.key_extras.input_scalar_hash = BKE_paint_layers_effect_input_scalar_hash(dims_entry);
  key.key_extras.chain_hash = BKE_paint_layers_effect_chain_hash(
      Span<const PaintMaterialCompositeCorrection>(dims_entry.content_corrections.data(),
                                                   int64_t(dims_entry.content_corrections.size())),
      effect.marker);
  std::shared_ptr<ImBuf> color = paint_layer_effect_cache_get_or_compute_region(
      g_paint_effect_cache,
      g_paint_effect_cache_mutex,
      key,
      width,
      height,
      images,
      g_paint_effect_cache_budget,
      [&](std::shared_ptr<ImBuf> &r_color, Vector<PaintLayerEffectSource> &r_synced) {
        return paint_layer_effect_compute(
            ma, row, effect, channel, tile_number, r_color, r_synced, painted_object);
      },
      changed_region,
      r_valid_region,
      [&](const std::shared_ptr<ImBuf> &cached,
          const rcti &maps_region,
          std::shared_ptr<ImBuf> &r_patched,
          Vector<PaintLayerEffectSource> &r_patched_synced,
          rcti &r_region_valid) {
        return paint_layer_effect_region_compute(ma,
                                                 row,
                                                 effect,
                                                 channel,
                                                 tile_number,
                                                 painted_object,
                                                 dims_entry,
                                                 images,
                                                 dims_from_slot,
                                                 cached,
                                                 maps_region,
                                                 r_patched,
                                                 r_patched_synced,
                                                 r_region_valid);
      });
  if (color != nullptr && r_valid_region != nullptr && changed_region != nullptr &&
      !BLI_rcti_is_empty(changed_region))
  {
    /* The caller offered the band its own subscription saw move -- the staleness its map carries,
     * in the maps' texel grid. The answer is current as a whole whichever way the cycle answered
     * (a standing result, a patch, a whole compute), so the band it is missing can be written from
     * it: the offered band, united with the rectangle the cycle named, grown by the halo -- the
     * kernel reads past the band it invalidated -- and clamped to the canvas. A band the maps
     * reach through a remapping is not the input's own texel grid, and no band can be narrowed
     * there: the whole canvas answers instead. */
    if (BKE_paint_layers_effect_region_is_exact(dims_entry, images, width, height)) {
      rcti united = *r_valid_region;
      BLI_rcti_union(&united, changed_region);
      float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
      composite_filter_params(effect, params);
      const int halo = BKE_paint_layers_effect_halo(
          eMaterialPaintLayerFilter(effect.kind_subtype), params, width, height);
      BKE_paint_layers_effect_region_clip(united, halo, width, height, *r_valid_region);
    }
    else {
      BLI_rcti_init(r_valid_region, 0, width, 0, height);
    }
  }
  return color;
}

/**
 * The result of the GPU-computed \a member of the Smart Filter chain instance \a chain on its
 * owner row's content in \a channel, as a float RGBA buffer at the input's size, scene linear, the
 * input's content alpha in the fourth component. The twin of #paint_layer_effect_buffer_get for a
 * chain member: the same cache, the same get-or-compute cycle, keyed on the member's composite
 * marker and the instance-unique hash, with the input described through the prefix the members
 * before it lay (#paint_layer_chain_member_input_describe). Describe-only answers nothing but the
 * maps, the plain path's contract.
 *
 * The key's chain half folds the input's preceding effect corrections' markers and hashes -- the
 * DNA effects before the chain row, and the members before this one -- which is the same fold the
 * GPU key carries (#BKE_paint_layers_effect_chain_hash): an edit of an upstream effect's
 * definition moves no packed parameter, so the scalar hash alone would keep serving a result
 * computed from the old answer, and the chain hash is what retires it. The plain path leaves the
 * half at zero (a direct effect's key holds its own hash, and the composite re-describes under it);
 * a member's input is built from other effects' results by construction, so the half is its key.
 */
static std::shared_ptr<ImBuf> paint_layer_chain_member_effect_buffer_get(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &chain,
    const MaterialPaintLayer &member,
    const int channel,
    const int tile_number,
    const bool describe_only,
    Vector<Image *> *r_images,
    PaintLayerSurfaceRef painted_object)
{
  /* The maps the input reads and its size, described without rendering anything: the maps go to
   * the composite cache's subscriptions, the size decides hits. */
  PaintMaterialCompositeImageLayer dims_entry;
  Vector<Image *> images;
  int width = 0;
  int height = 0;
  bool dims_from_slot = false;
  if (!paint_layer_chain_member_input_describe(
          ma,
          row,
          chain,
          member,
          channel,
          /*dimensions_only=*/true,
          /*describe_only=*/false,
          dims_entry,
          PaintLayerTileContext{.painted_object = painted_object, .tile_number = tile_number}) ||
      !paint_layer_chain_member_dimensions_resolve(
          ma,
          Span<PaintMaterialCompositeImageLayer>(&dims_entry, 1),
          member,
          tile_number,
          width,
          height,
          dims_from_slot))
  {
    return {};
  }
  paint_layer_effect_input_images_append(
      member, images, [&](Vector<Image *> &r_input_images) {
        BKE_paint_layers_effect_input_images(dims_entry, r_input_images);
      });
  if (r_images != nullptr) {
    *r_images = images;
  }
  if (describe_only) {
    return {};
  }

  const bUUID member_marker = composite_chain_member_marker(chain, member);
  EffectCacheKey key{ma.id.session_uid,
                     member_marker,
                     channel,
                     tile_number,
                     paint_layer_chain_member_effect_hash(chain, member, channel)};
  /* The two fingerprints the GPU key carries, from the same description: the scalar content the
   * input's maps do not cover, and the ordered fingerprint of the effects whose results feed this
   * input. See the comment above the function for why the chain half is the member's key and the
   * plain path's zero. */
  key.key_extras.input_scalar_hash = BKE_paint_layers_effect_input_scalar_hash(dims_entry);
  key.key_extras.chain_hash = BKE_paint_layers_effect_chain_hash(
      Span<const PaintMaterialCompositeCorrection>(dims_entry.content_corrections.data(),
                                                   dims_entry.content_corrections.size()),
      member_marker);
  std::shared_ptr<ImBuf> result = paint_layer_effect_cache_get_or_compute(
      g_paint_effect_cache,
      g_paint_effect_cache_mutex,
      key,
      width,
      height,
      images,
      g_paint_effect_cache_budget,
      [&](std::shared_ptr<ImBuf> &r_color, Vector<PaintLayerEffectSource> &r_synced) {
        return paint_layer_chain_member_effect_compute(
            ma, row, chain, member, channel, tile_number, r_color, r_synced, painted_object);
      });
  return result;
}

void paint_layer_owned_effect_cache_drop(const Material &ma,
                                         const MaterialPaintLayer &effect,
                                         const int channel,
                                         const int tile_number)
{
  const EffectCacheKey key{ma.id.session_uid,
                           effect.marker,
                           channel,
                           tile_number,
                           paint_layer_effect_hash(effect, channel)};
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  const auto no_release = [](EffectCacheEntry &) {};
  g_paint_effect_cache.drop_instance(key, no_release);
}

/* The editors' GPU answer for an owned effect, registered while their GPU pipeline can serve one
 * and null whenever it cannot. Reads happen during a bake; the plain pointer keeps no guard for
 * that, the same terms the regenerate pre-hook keeps. */
static PaintLayerOwnedEffectGpuProvider g_owned_effect_gpu_provider = nullptr;

void BKE_paint_layers_owned_effect_gpu_provider_set(PaintLayerOwnedEffectGpuProvider provider)
{
  /* The contract the header pins: registered on the main thread before the first bake's worker
   * starts reading the hook, so no bake can observe the registration land. */
  BLI_assert(BLI_thread_is_main());
  g_owned_effect_gpu_provider = provider;
}

PaintLayerOwnedEffectGpuProvider BKE_paint_layers_owned_effect_gpu_provider_get()
{
  return g_owned_effect_gpu_provider;
}

/**
 * Whether the band a caller offers for \a effect's input is in the input's own texel grid, the rule
 * #BKE_paint_layers_effect_region_is_exact answers for the CPU path (#paint_layer_effect_buffer_get
 * describes the same input before it reaches that check). The GPU provider's texture is valid as a
 * whole once it returns, but the map it is written into is already correct past the offered band
 * only when the band is in the map's grid; through a remapping no band can be narrowed, and the
 * whole canvas has to answer instead.
 */
static bool paint_layer_effect_input_region_is_exact(const Material &ma,
                                                     const MaterialPaintLayer &row,
                                                     const MaterialPaintLayer &effect,
                                                     const int channel,
                                                     const int tile_number)
{
  PaintMaterialCompositeImageLayer dims_entry;
  Vector<Image *> images;
  int width = 0;
  int height = 0;
  bool dims_from_slot = false;
  if (!BKE_paint_layers_effect_input_describe(
          ma,
          row,
          effect,
          channel,
          true,
          false,
          dims_entry,
          PaintLayerTileContext{.painted_object = nullptr, .tile_number = tile_number}) ||
      !paint_layer_effect_dimensions_resolve(
          Span<PaintMaterialCompositeImageLayer>(&dims_entry, 1),
          effect,
          tile_number,
          width,
          height,
          dims_from_slot))
  {
    return false;
  }
  paint_layer_effect_input_images_append(
      effect, images, [&](Vector<Image *> &r_input_images) {
        BKE_paint_layers_effect_input_images(dims_entry, r_input_images);
      });
  return BKE_paint_layers_effect_region_is_exact(dims_entry, images, width, height);
}

void BKE_paint_layers_effect_valid_region(const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const MaterialPaintLayer &effect,
                                          const int channel,
                                          const int tile_number,
                                          const rcti *changed_region,
                                          const int width,
                                          const int height,
                                          rcti &r_valid_region)
{
  if (changed_region != nullptr && !BLI_rcti_is_empty(changed_region) &&
      paint_layer_effect_input_region_is_exact(ma, row, effect, channel, tile_number))
  {
    float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
    composite_filter_params(effect, params);
    const int halo = BKE_paint_layers_effect_halo(
        eMaterialPaintLayerFilter(effect.kind_subtype), params, width, height);
    BKE_paint_layers_effect_region_clip(*changed_region, halo, width, height, r_valid_region);
  }
  else {
    /* No offered band, an empty one, or a band a remapping takes out of the input's own texel
     * grid: the whole canvas is the answer, exactly the CPU path's rule. */
    BLI_rcti_init(&r_valid_region, 0, width, 0, height);
  }
}

std::shared_ptr<ImBuf> paint_layer_owned_effect_result_compute(const Material &ma,
                                                               const MaterialPaintLayer &row,
                                                               const MaterialPaintLayer &effect,
                                                               const int channel,
                                                               const int tile_number,
                                                               const rcti *changed_region,
                                                               rcti *r_valid_region)
{
  /* The GPU provider first: an effect the combined preview computes in a pass of its own is asked
   * of it, so a bake reads the result the screen keeps instead of recomputing it on the CPU. The
   * gates are the provider's own terms -- it shares the preview's GPU effect cache, which holds
   * base-tile entries only, and it needs the main thread's GPU context, which the bake job's
   * worker does not have and so falls back below. A null answer falls through to the CPU too. The
   * changed region rides along for a regional dispatch; the provider's texture is valid as a
   * whole once it returns, but the map it is written into is already correct past the changed
   * band, so the valid rectangle is the band grown by the halo -- the whole canvas when no region
   * was offered, the same arithmetic the CPU path answers with. */
  if (g_owned_effect_gpu_provider != nullptr && tile_number == PAINT_TILE_NUMBER_BASE &&
      BKE_paint_layers_effect_is_gpu_computed(effect) && BLI_thread_is_main())
  {
    bke::paint_perf::ScopedTimer perf_gpu("effect.own_bake.gpu");
    std::shared_ptr<ImBuf> gpu_result = g_owned_effect_gpu_provider(
        ma, row, effect, channel, tile_number, changed_region);
    perf_gpu.note("kind=%d hit=%s",
                  int(effect.kind_subtype),
                  gpu_result != nullptr ? "yes" : "fallback");
    if (gpu_result != nullptr) {
      if (r_valid_region != nullptr) {
        BKE_paint_layers_effect_valid_region(ma,
                                             row,
                                             effect,
                                             channel,
                                             tile_number,
                                             changed_region,
                                             gpu_result->x,
                                             gpu_result->y,
                                             *r_valid_region);
      }
      return gpu_result;
    }
  }

  /* No forced drop here any more: the reason it stood -- the cache's key not seeing the row's
   * mapping or the effects below, which the bake's hash does -- is covered by the key itself now,
   * whose input scalar hash and chain hash fold the same description the bake's hash reads, and
   * by the region-exact rule the regional update checks. An entry kept from before such an edit
   * answers a different key, misses, and is computed whole from the description as it is now. */
  bke::paint_perf::ScopedTimer perf_cpu("effect.cpu.owned_fallback");
  std::shared_ptr<ImBuf> result = paint_layer_effect_buffer_get(ma,
                                                                row,
                                                                effect,
                                                                channel,
                                                                tile_number,
                                                                /*describe_only=*/false,
                                                                nullptr,
                                                                nullptr,
                                                                nullptr,
                                                                changed_region,
                                                                r_valid_region);
  perf_cpu.note("kind=%d channel=%d", int(effect.kind_subtype), channel);
  return result;
}

void paint_layer_owned_effect_input_images(const Material &ma,
                                           const MaterialPaintLayer &row,
                                           const MaterialPaintLayer &effect,
                                           Vector<Image *> &r_images)
{
  /* The kind's own slot is beside the input's maps: the bake reads the kernel's sampling of it, so
   * an edit of its pixels must reach the bake the way an edit of the input's maps does. The common
   * append takes it once for every channel below. */
  paint_layer_effect_input_images_append(
      effect, r_images, [&](Vector<Image *> &r_input_images) {
        for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
          if (!paint_layer_effect_filters(ma, effect, channel)) {
            continue;
          }
          PaintMaterialCompositeImageLayer entry;
          if (!BKE_paint_layers_effect_input_describe(
                  ma,
                  row,
                  effect,
                  channel,
                  /*dimensions_only=*/true,
                  /*describe_only=*/false,
                  entry,
                  PaintLayerTileContext{.tile_number = PAINT_TILE_NUMBER_BASE}))
          {
            continue;
          }
          BKE_paint_layers_effect_input_images(entry, r_input_images);
        }
      });
}

uint64_t paint_layer_owned_effect_input_hash(const Material &ma,
                                             const MaterialPaintLayer &row,
                                             const MaterialPaintLayer &effect)
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&](const uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!paint_layer_effect_filters(ma, effect, channel)) {
      continue;
    }
    mix(uint64_t(channel) + 1);
    /* Only described, never rendered: a lower effect that is not baked yet must not compute inside a
     * hash. The description is the input the effect is computed from -- the row's source, its
     * mapping and transform steps, the effects below -- so the hash moves exactly when the input
     * does, and the maps' pixels stay the subscription's business. */
    PaintMaterialCompositeImageLayer entry;
    if (!BKE_paint_layers_effect_input_describe(
            ma,
            row,
            effect,
            channel,
            /*dimensions_only=*/true,
            /*describe_only=*/true,
            entry,
            PaintLayerTileContext{.tile_number = PAINT_TILE_NUMBER_BASE}))
    {
      /* An input that cannot be described yet (the source is not rendered) hashes as nothing. */
      mix(0);
      continue;
    }
    mix(BKE_paint_material_composite_stack_hash(Span<PaintMaterialCompositeImageLayer>(&entry, 1)));
  }
  /* The kind's own slot moves the bake the way the description does: another image in it is
   * another result the bake must hold. The pixels stay the subscriptions' business. */
  paint_layer_effect_hash_mix_slot(hash, effect);
  return hash;
}

uint64_t paint_layer_mask_prefix_projection_hash(const Material &ma,
                                                 const MaterialPaintLayer &row,
                                                 const MaterialPaintLayer &stop,
                                                 const int tile_number,
                                                 PaintLayerSurfaceRef painted_object)
{
  uint64_t hash = 0;
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (&item == &stop) {
      break;
    }
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    /* A canvas Material mask item owns no record: its projection reads the canvas map its bake
     * holds, so that image is what the aspect and the tiled read are hashed against. */
    const Image *item_image =
        (record != nullptr) ?
            record->image :
            paint_layer_material_canvas_image(ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    PaintMaterialCompositeProjection projection;
    composite_projection_describe(ma,
                                  item,
                                  tile_number,
                                  item_image,
                                  painted_object,
                                  projection);
    if (projection.present) {
      /* Folded in order, so two projected items trade places only with a different grey. */
      hash = composite_projection_hash(hash + 1, projection);
    }
  }
  return hash;
}

uint64_t paint_layer_row_projection_hash(const Material &ma,
                                         const MaterialPaintLayer &row,
                                         const int channel,
                                         const int tile_number,
                                         PaintLayerSurfaceRef painted_object)
{
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
  /* A canvas Material row owns no record: its projection reads the canvas map its bake holds, the
   * same image the composer's source describe takes. */
  const Image *row_image =
      (record != nullptr) ? record->image :
                            paint_layer_material_canvas_image(ma, row, channel);
  PaintMaterialCompositeProjection projection;
  composite_projection_describe(ma,
                                row,
                                tile_number,
                                row_image,
                                painted_object,
                                projection);
  return projection.present ? composite_projection_hash(1, projection) : 0;
}

/** \} */

bool BKE_paint_layers_slice_render(const Material &ma,
                                   const MaterialPaintLayer &reader,
                                   const int channel,
                                   std::shared_ptr<ImBuf> *r_color,
                                   std::shared_ptr<ImBuf> *r_coverage,
                                   const int *dst_rect,
                                   const bool dimensions_only,
                                   const PaintLayerTileContext &context)
{
  *r_color = {};
  *r_coverage = {};
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  /* One resolution of the unset tile here, so everything below keys on the concrete number. */
  const int tile_number = BKE_paint_layers_tile_resolve(
      context.tile_number, nullptr, context.painted_object);
  PaintLayerSurfaceRef painted_object = context.painted_object;
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr) {
    return false;
  }
  if (dimensions_only) {
    /* Sizing only: every nested slice is left unrendered and unallocated, and the buffers are not
     * needed. */
    int width = 0;
    int height = 0;
    return anchor_slice_dimensions(
        ma, reader, channel, ref, width, height, painted_object, tile_number);
  }
  if (dst_rect != nullptr) {
    /* A partial render is not a complete slice and is never cached: it is only produced for a bake
     * job that writes the requested rectangle into its own full-size map. */
    if (!anchor_slice_render_new(ma,
                                 reader,
                                 channel,
                                 ref,
                                 dst_rect,
                                 *r_color,
                                 *r_coverage,
                                 painted_object,
                                 tile_number)) {
      return false;
    }
    return true;
  }

  AnchorSliceCacheKey key;
  Vector<Image *> images;
  key.material_uid = ma.id.session_uid;
  key.reader = reader.marker;
  /* A mask slice is the same grey for every channel: one cache entry per reader and tile. */
  key.channel = (ref.target == PaintLayerAnchorPointTarget::Mask) ? -1 : channel;
  key.tile_number = tile_number;
  key.hash = paint_layer_anchor_point_slice_hash(ma, reader, channel);
  anchor_slice_images_get(ma, reader, images);
  /* Read before anything is rendered: an edit that lands while the slice renders is then seen as
   * newer than the entry, and costs one more update rather than a stale slice. */
  Vector<AnchorSliceSource> sources_now = map_sources_snapshot(images);

  /* An entry that needs work is taken out of the cache while it is worked on, so no other thread
   * can pick up buffers that are being written; it goes back afterwards. */
  AnchorSliceCacheEntry held;
  bool have_entry = false;
  ePixelChange change = ePixelChange::Full;
  rcti region;
  BLI_rcti_init(&region, 0, 0, 0, 0);
  {
    std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
    for (const int i : anchor_slice_cache().entries.index_range()) {
      if (!anchor_slice_key_equal(anchor_slice_cache().entries[i].key, key)) {
        continue;
      }
      change = map_sources_changes(anchor_slice_cache().entries[i].synced, images, region);
      if (change == ePixelChange::None) {
        /* LRU: move the hit to the newest end. The buffers stay alive through the shared pointers
         * the caller receives, whatever the cache does next. */
        AnchorSliceCacheEntry hit = std::move(anchor_slice_cache().entries[i]);
        anchor_slice_cache().entries.remove(i);
        *r_color = hit.color;
        *r_coverage = hit.coverage;
        anchor_slice_cache().entries.append(std::move(hit));
        return true;
      }
      held = std::move(anchor_slice_cache().entries[i]);
      anchor_slice_cache().total_bytes -= held.bytes;
      anchor_slice_cache().entries.remove(i);
      have_entry = true;
      break;
    }
  }

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  bool updated = false;
  if (have_entry && change == ePixelChange::Region && held.color != nullptr &&
      held.coverage != nullptr &&
      anchor_slice_region_is_exact(
          ma, reader, channel, images, held.color->x, held.color->y, tile_number))
  {
    /* Only the edited rectangle is rendered again. The buffers are written in place when the cache
     * is their only owner; a description that still reads them gets them unchanged, and the
     * update works on a copy. */
    color = std::move(held.color);
    coverage = std::move(held.coverage);
    if (color.use_count() > 1) {
      color = paint_layer_anchor_slice_buffer_clone(*color);
    }
    if (coverage.use_count() > 1) {
      coverage = paint_layer_anchor_slice_buffer_clone(*coverage);
    }
    if (color != nullptr && coverage != nullptr) {
      int rx0, ry0, rx1, ry1;
      const int rect[4] = {region.xmin, region.ymin, region.xmax, region.ymax};
      if (!anchor_slice_region_get(rect, color->x, color->y, rx0, ry0, rx1, ry1)) {
        /* The edit lies outside the slice: nothing of it is in there to change. */
        updated = true;
      }
      else {
        updated = anchor_slice_fill(
            ma, reader, channel, ref, rect, *color, *coverage, painted_object, tile_number);
      }
    }
    if (!updated) {
      /* A half-written buffer is not reused. */
      color = {};
      coverage = {};
    }
  }
  if (!updated && !anchor_slice_render_new(ma,
                                           reader,
                                           channel,
                                           ref,
                                           nullptr,
                                           color,
                                           coverage,
                                           painted_object,
                                           tile_number))
  {
    return false;
  }

  {
    AnchorSliceCacheEntry new_entry;
    new_entry.key = key;
    new_entry.color = color;
    new_entry.coverage = coverage;
    new_entry.synced = std::move(sources_now);
    new_entry.bytes = anchor_slice_color_bytes(color.get()) +
                      anchor_slice_coverage_bytes(coverage.get());
    std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
    /* Bound the cache by memory, not by a number of entries: one 4K pair is far larger than a
     * handful of small slices. The oldest entry is the LRU one; a hit moves to the newest end
     * above. Evicting only drops the cache's reference: a description or a caller that still holds
     * the buffers keeps them alive. Another thread may have rendered the same slice meanwhile; the
     * newer one replaces it. */
    for (int i = int(anchor_slice_cache().entries.size()) - 1; i >= 0; i--) {
      if (anchor_slice_key_equal(anchor_slice_cache().entries[i].key, key)) {
        anchor_slice_cache().total_bytes -= anchor_slice_cache().entries[i].bytes;
        anchor_slice_cache().entries.remove(i);
      }
    }
    /* A slice larger than the whole budget is not kept: it would evict everything and then stay
     * pinned after the description that needed it is gone. The caller still receives it. */
    if (new_entry.bytes <= g_anchor_slice_cache_budget) {
      while (!anchor_slice_cache().entries.is_empty() &&
             anchor_slice_cache().total_bytes + new_entry.bytes > g_anchor_slice_cache_budget)
      {
        anchor_slice_cache().total_bytes -= anchor_slice_cache().entries[0].bytes;
        anchor_slice_cache().entries.remove(0);
      }
      anchor_slice_cache().total_bytes += new_entry.bytes;
      anchor_slice_cache().entries.append(std::move(new_entry));
    }
  }
  *r_color = std::move(color);
  *r_coverage = std::move(coverage);
  return true;
}

/**
 * The live description of \a layer in \a channel, as the evaluator reads one row: what a bake of
 * the row renders. A Pass Through folder has no row of its own to describe.
 *
 * \a scope is what the bake holds (#PaintLayerBakeScope): a content bake renders the row's content
 * alone, its own opacity and its mask left out to be laid live over the map.
 */
static bool composite_bake_entry(
    const Material &ma,
    const MaterialPaintLayer &layer,
    const int channel,
    PaintMaterialCompositeImageLayer &r_entry,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &surface,
    const int *slice_rect,
    const bool dimensions_only,
    const PaintLayerBakeScope scope,
    PaintLayerSurfaceRef painted_object,
    const int tile_number)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      (BKE_paint_layers_is_folder(layer) && BKE_paint_layers_folder_is_pass_through(layer)))
  {
    return false;
  }
  /* One resolution of the unset tile: the surface knows the UDIM tile of the UVs it was rasterized
   * from, then the painted object's own UVs decide, and the base tile stays the answer for
   * everything else. */
  const int effective_tile = BKE_paint_layers_tile_resolve(
      tile_number, surface.get(), painted_object);
  /* The surface is a row input of the description, not a stop point: it rides the context
   * arguments after the size ones. */
  return composite_image_layer_build(ma,
                                    layer,
                                    channel,
                                    1.0f,
                                    false,
                                    true,
                                    r_entry,
                                    effective_tile,
                                    nullptr,
                                    slice_rect,
                                    dimensions_only,
                                    false,
                                    surface,
                                    nullptr,
                                    scope,
                                    painted_object);
}

bool BKE_paint_layers_bake_render_node(
    const Material &ma,
    const MaterialPaintLayer &layer,
    const int channel,
    const int size,
    float *r_color_rgba,
    float *r_coverage_gray,
    const int *dst_rect,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &surface,
    const PaintLayerTileContext &context,
    const bool whole_row,
    const int out_height)
{
  PaintLayerSurfaceRef painted_object = context.painted_object;
  const int tile_number = context.tile_number;
  /* The result's own shape: a square `size` for every caller that asks no height, the caller's
   * canvas for a non-square one. The `size` stays the width of the result either way. */
  const int result_height = (out_height > 0) ? out_height : size;
  if (size <= 0 || result_height <= 0 || r_color_rgba == nullptr || r_coverage_gray == nullptr) {
    return false;
  }
  /* A subtree whose generators the bake cannot evaluate is never a cache. */
  if (!BKE_paint_layers_generator_subtree_is_bakeable(layer)) {
    return false;
  }
  /* An Object or Generated pattern reads the geometry of the object it is shaded on, which only
   * the surface of that object carries: without one the texel's own UV would be the approximate
   * preview, not a bake, so the render is refused rather than written as a wrong picture. A reader
   * is exempt: its cache is the slice of the row it points at, rendered by the slice machinery. */
  if (surface == nullptr && !BKE_paint_layers_generator_subtree_is_shared_bakeable(layer) &&
      !paint_layer_kind_reads_anchor_point(layer))
  {
    return false;
  }
  /* Learn the stack dimensions without rendering or allocating a single nested slice. A row
   * carrying a CPU-computed effect is rendered as its content alone: its mask and its opacity are
   * laid live over the map the result becomes, so a stroke into the mask never asks for this. */
  /* A caller that wants the finished picture asks for the whole row: the live description already
   * renders a CPU effect through the effect cache (as it does for a child of another row's bake),
   * so the mask and the opacity come along without a separate pass. */
  const PaintLayerBakeScope scope = whole_row ? PaintLayerBakeScope::WholeRow :
                                                paint_layer_bake_scope(layer, false);
  PaintMaterialCompositeImageLayer dims_entry;
  if (!composite_bake_entry(ma,
                            layer,
                            channel,
                            dims_entry,
                            surface,
                            nullptr,
                            true,
                            scope,
                            painted_object,
                            tile_number))
  {
    return false;
  }
  /* The row's content, straight and with its own factor, from the one shared isolated-group
   * formula -- for a leaf and a folder alike. */
  int width = 0;
  int height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(
          Span<PaintMaterialCompositeImageLayer>(&dims_entry, 1), width, height) ||
      width <= 0 || height <= 0)
  {
    return false;
  }
  /* A result smaller than the stack (the drag-reduced bake of a Decal row, while the drag lasts)
   * composites on a grid of its own size: the Decal resample and every map read cost by texels, and
   * a 4K grid would only be thrown away by the nearest resample below. A partial rectangle keeps
   * the stack grid, as the nested slice rectangle is in its pixels. The CPU-computed effects are
   * not part of this smaller grid: their input is always described, rendered and filtered at the
   * stack's own resolution, so a texel parameter (a blur radius, an offset, an outline width) keeps
   * its meaning and the reduced frame is a resample of the full result. It is deliberately
   * approximate while the drag lasts; the full bake is owed once it ends. */
  int eval_width = 0;
  int eval_height = 0;
  if (dst_rect == nullptr && (width > size || height > result_height) &&
      BKE_paint_layers_bake_drag_is_active(ma.id.session_uid))
  {
    eval_width = min_ii(width, size);
    eval_height = min_ii(height, result_height);
    width = eval_width;
    height = eval_height;
  }
  /* The rectangle of the `size` x `result_height` result to produce, and the source pixels it
   * samples. A full render produces all of it; a partial re-bake passes the changed tile so the
   * composite runs over that source rectangle alone. */
  int dx0 = 0, dy0 = 0, dx1 = size, dy1 = result_height;
  if (dst_rect != nullptr) {
    dx0 = clamp_i(dst_rect[0], 0, size);
    dy0 = clamp_i(dst_rect[1], 0, result_height);
    dx1 = clamp_i(dst_rect[2], 0, size);
    dy1 = clamp_i(dst_rect[3], 0, result_height);
  }
  if (dx0 >= dx1 || dy0 >= dy1) {
    return false;
  }
  /* The source pixels nearest-sampled into `[dx0, dx1) x [dy0, dy1)`: the first and last output
   * pixel of the rectangle map to the source span that has to be composited. */
  const int sx0 = int(int64_t(dx0) * width / size);
  const int sx1 = (dx1 >= size) ? width : int((int64_t(dx1) * width + size - 1) / size);
  const int sy0 = int(int64_t(dy0) * height / result_height);
  const int sy1 = (dy1 >= result_height) ? height :
                                           int((int64_t(dy1) * height + result_height - 1) /
                                               result_height);
  rcti area;
  BLI_rcti_init(&area, max_ii(0, sx0), min_ii(width, sx1), max_ii(0, sy0), min_ii(height, sy1));
  const int area_w = BLI_rcti_size_x(&area);
  const int area_h = BLI_rcti_size_y(&area);
  if (area_w <= 0 || area_h <= 0) {
    return false;
  }
  /* A partial re-bake only needs the nested slices over the source rectangle the area maps to; a
   * full one renders them completely. The area is only known from the sizes found above, which is
   * why the row is described a second time. */
  const int slice_rect[4] = {area.xmin, area.ymin, area.xmax, area.ymax};
  PaintMaterialCompositeImageLayer entry;
  if (!composite_bake_entry(ma,
                            layer,
                            channel,
                            entry,
                            surface,
                            dst_rect != nullptr ? slice_rect : nullptr,
                            false,
                            scope,
                            painted_object,
                            tile_number))
  {
    return false;
  }
  Vector<float> content_color(int64_t(area_w) * area_h * 4);
  Vector<float> content_coverage(int64_t(area_w) * area_h);
  const rcti region = area;
  if (!BKE_paint_material_composite_eval_row_content(
          entry, content_color.data(), content_coverage.data(), &region, channel, eval_width, eval_height))
  {
    return false;
  }
  /* Resample only the requested rectangle: each output pixel reads the composited source span. */
  for (int y = dy0; y < dy1; y++) {
    const int src_y = min_ii(int(int64_t(y) * height / result_height), height - 1);
    const int local_y = src_y - area.ymin;
    for (int x = dx0; x < dx1; x++) {
      const int src_x = min_ii(int(int64_t(x) * width / size), width - 1);
      const int64_t src = int64_t(local_y) * area_w + (src_x - area.xmin);
      const int64_t dst = int64_t(y) * size + x;
      r_color_rgba[dst * 4 + 0] = content_color[src * 4 + 0];
      r_color_rgba[dst * 4 + 1] = content_color[src * 4 + 1];
      r_color_rgba[dst * 4 + 2] = content_color[src * 4 + 2];
      /* The content alpha travels with the colour, so a substituted row reads its own transparency
       * back instead of turning opaque. Over a content bake the coverage is that same quantity, the
       * mask and the opacity having been left out: what the quotient below keeps is the texel's
       * coverage by the content alone. */
      r_color_rgba[dst * 4 + 3] = content_color[src * 4 + 3];
      r_coverage_gray[dst] = content_coverage[src];
    }
  }
  return true;
}

bool BKE_paint_layers_gpu_row_entry(const Material &ma,
                                    const MaterialPaintLayer &layer,
                                    const int channel,
                                    PaintMaterialCompositeImageLayer &r_entry,
                                    PaintLayerSurfaceRef painted_object)
{
  /* The CPU render's own refusals, so a row the GPU takes is one the CPU would have rendered. With
   * no surface, a pattern that reads the object's geometry is the approximate preview, not a bake.
   */
  if (!BKE_paint_layers_generator_subtree_is_bakeable(layer) ||
      (!BKE_paint_layers_generator_subtree_is_shared_bakeable(layer) &&
       !paint_layer_kind_reads_anchor_point(layer)))
  {
    return false;
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      (BKE_paint_layers_is_folder(layer) && BKE_paint_layers_folder_is_pass_through(layer)))
  {
    return false;
  }
  return composite_image_layer_build(ma,
                                     layer,
                                     channel,
                                     1.0f,
                                     false,
                                     true,
                                     r_entry,
                                     PAINT_TILE_NUMBER_BASE,
                                     nullptr,
                                     nullptr,
                                     false,
                                     /*describe_only=*/true,
                                     nullptr,
                                     nullptr,
                                     PaintLayerBakeScope::WholeRow,
                                     painted_object);
}

bool BKE_paint_layers_row_dimensions(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     const int channel,
                                     int &r_width,
                                     int &r_height,
                                     const PaintLayerTileContext &context)
{
  PaintMaterialCompositeImageLayer entry;
  /* Only the size is wanted: nested readers must not render and cache their slices for it. */
  if (!composite_bake_entry(ma,
                            layer,
                            channel,
                            entry,
                            nullptr,
                            nullptr,
                            true,
                            PaintLayerBakeScope::WholeRow,
                            context.painted_object,
                            context.tile_number))
  {
    return false;
  }
  return BKE_paint_material_composite_stack_dimensions(
      Span<PaintMaterialCompositeImageLayer>(&entry, 1), r_width, r_height);
}

/**
 * The fallback reason a CPU-computed effect gives: its kind's name, with the article a name in
 * English takes. The text is built from the registry rather than listing every effect a second
 * time, and cached so the returned pointer stays valid like a literal.
 */
static const char *paint_layer_cpu_computed_reason(const PaintLayerKindInfo &kind)
{
  /* One string per kind, so two reasons held at once never share storage; the map's nodes keep
   * their address while it grows. */
  static thread_local std::unordered_map<const PaintLayerKindInfo *, std::string> reasons;
  auto it = reasons.find(&kind);
  if (it == reasons.end()) {
    const bool vowel = ELEM(kind.name[0], 'A', 'E', 'I', 'O', 'U');
    it = reasons
             .emplace(&kind,
                      fmt::format("{} {} correction is read", vowel ? "an" : "a", kind.name))
             .first;
  }
  return it->second.c_str();
}

/**
 * Whether the combined preview shader has no branch for the pointwise \a filter: the registry's
 * #PAINT_LAYER_KIND_NO_GPU_BRANCH flag says so, so a new kind is marked where it is described and
 * cannot be silently shaded as a no-op here.
 */
static bool gpu_filter_lacks_branch(const int16_t filter)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER, filter);
  return kind != nullptr && (kind->flags & PAINT_LAYER_KIND_NO_GPU_BRANCH) != 0;
}

bool BKE_paint_layers_effect_is_gpu_computed(const MaterialPaintLayer &effect)
{
  return paint_layer_effect_is_gpu_computed(effect);
}

bool BKE_paint_layers_effect_is_precomputed_result(const MaterialPaintLayer &effect)
{
  return paint_layer_effect_is_precomputed_result(effect);
}

bool BKE_paint_layers_effect_node_group_can_compute(const MaterialPaintLayer &effect)
{
  return paint_layer_effect_node_group_can_compute(effect);
}

const char *BKE_paint_layers_gpu_unsupported_reason(const PaintMaterialCompositeImageLayer &layer,
                                                    const bool mask_slice)
{
  if (!layer.enabled || layer.opacity <= 0.0f) {
    /* The composite skips it as well. */
    return nullptr;
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    if (child.adjustment_filter != 0) {
      return "an adjustment row is inside a folder";
    }
    if (const char *reason = BKE_paint_layers_gpu_unsupported_reason(child, mask_slice)) {
      return reason;
    }
  }
  const bool reader = !BLI_uuid_is_nil(layer.anchor_point_reader);
  const bool mask_only_slice = mask_slice && !layer.mask_items.is_empty();
  /* A Gradient generator has no map and no flat colour of its own: its pixels come from the ramp at
   * a coordinate the CPU evaluator computes. Stated explicitly rather than left to the map/constant
   * check below, so a future shader that gains a Gradient branch cannot silently shade a different
   * approximation than the CPU exact preview. The caller falls back to the CPU display override. */
  if (layer.gradient.enabled) {
    return "a Gradient generator row is not laid by the GPU path";
  }
  /* The transform effects' steps are composed by the shader up to a fixed number; a row that
   * carries more falls to the CPU rather than shading a step as if it were not there. */
  if (layer.input_transforms.size() > PAINT_MATERIAL_COMBINED_INPUT_STEP_MAX) {
    return "a row with too many transform effects is not laid by the GPU path";
  }
  /* A step in the effect's own Decal frame walks the unfolded surface: the shader has no inverse
   * map of the frame, so the whole row stays with the CPU rather than folding the lookup in UV. */
  for (const PaintMaterialCompositeMapping &step : layer.input_transforms) {
    if (step.projection.present && step.projection.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL) {
      return "a transform effect in a Decal frame is not laid by the GPU path";
    }
  }
  /* A projected map row is laid by the layer pass through the painted surface's map, in the six
   * modes (#composite_resample_projected is its CPU twin). What stays with the CPU: a projected row
   * that is not a plain map row (a folder, a flat colour, a slice reader), and a tiled map, whose
   * GPU texture is not one tile the projected UV could wrap in. */
  if (layer.mapping.projection.present) {
    /* A Decal row is laid too: the host uploads its unfolded patch in place of the surface
     * position (#combined_projection_gpu_make), and the Decal map itself is built by the CPU. */
    if (layer.color_image == nullptr || reader) {
      return "a projected row without a map is not laid by the GPU path";
    }
    if (layer.color_image->source == IMA_SRC_TILED) {
      return "a projected row reads a tiled map, which the GPU path does not lay";
    }
    /* The transform effects' steps are laid in every projection space, Cube inside its atlas cell.
     * A projected tangent-space Normal row is laid in full: the steps' per-tap covariant remap and
     * the frame-to-tangent conversion run in the projected read, with the controller's space. */
  }
  /* A projected correction or mask item map is laid through the same frame as a row's. The CPU only
   * projects a plain map (never a mesh map, a slice or a filter), so those carry no projection to
   * lay; a tiled map stays with the CPU for the reason a row's does. */
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.opacity > 0.0f && correction.mapping.projection.present &&
        correction.image != nullptr && !correction.is_mesh_map &&
        correction.image->source == IMA_SRC_TILED)
    {
      return "a projected correction reads a tiled map, which the GPU path does not lay";
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    if (item.opacity <= 0.0f) {
      continue;
    }
    if (item.mapping.projection.present && item.image != nullptr && !item.is_mesh_map &&
        item.image->source == IMA_SRC_TILED)
    {
      return "a projected mask item reads a tiled map, which the GPU path does not lay";
    }
    if (item.coverage_mapping.projection.present && item.coverage_image != nullptr &&
        item.coverage_image->source == IMA_SRC_TILED)
    {
      return "a projected coverage map is tiled, which the GPU path does not lay";
    }
  }
  /* The height modes are a delta-space step no shader branch reproduces, and a Compare row's win
   * or loss gates every channel of the row through a rendered mask. The description carries the
   * effective mode, so an Inherit row over Add, Subtract or a non-Mix colour blend is refused
   * here too. Until the preview shader grows both, a material using either falls back to the CPU
   * composite instead of shading a different layer than the one the bake and the Image Editor
   * show. */
  if (layer.height_blend != MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT || layer.height_compare) {
    return "a row blends Height with a mode of its own";
  }
  if (!layer.is_folder && layer.adjustment_filter == 0 && layer.color_image == nullptr &&
      layer.anchor_point_color == nullptr && !layer.has_constant_color && !reader &&
      !mask_only_slice)
  {
    return "a row has neither a map, a flat colour nor an Anchor Point slice";
  }
  /* The combined pass knows the pointwise filters it can express in its shader, and reads a Color
   * Ramp or RGB Curves from a table the gather bakes into a texture. What still has no shader
   * branch is a Gradient, a baked Anchor Point correction's coverage map, and the row shapes
   * refused above; those fall back to the CPU rather than shading something different. */
  /* Corrections and mask items first, because a Gradient one is not merely unsupported by a pass
   * that would otherwise lay its map: there is no map, so the gather's own "a correction that
   * changes something" check does not even count it and the row would be shaded without it. The
   * refusal has to come from here for the CPU preview to take over. Gated on the opacity that
   * decides the CPU lays it too, so a zero-opacity Gradient element does not cost the GPU path. */
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.opacity > 0.0f && correction.gradient.enabled) {
      return "a Gradient correction is not laid by the GPU path";
    }
    if (correction.opacity > 0.0f && correction.coverage_image != nullptr) {
      /* NOTE: the generated shader samples no coverage map, so this row falls back to the CPU
       * composite instead of shading a picture without the text. */
      return "a Text Decal covers by a coverage map the GPU path does not read";
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    /* A CPU-computed mask effect's grey is computed on the CPU: no pass lays it yet, so the stack
     * falls back rather than shading the item as a no-op. The field names the kind, so a new one
     * needs no edit here. */
    if (item.opacity > 0.0f && item.effect_filter != 0) {
      const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                             item.effect_filter);
      if (kind != nullptr) {
        if ((kind->flags & PAINT_LAYER_KIND_TRANSFORMS_INPUT) != 0) {
          return "a transform effect in a mask is not laid by the GPU path";
        }
        return paint_layer_cpu_computed_reason(*kind);
      }
    }
    if (item.opacity > 0.0f && item.gradient.enabled) {
      return "a Gradient mask item is not laid by the GPU path";
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    /* A sliced correction reads a captured slot. A baked one has no slice buffer but covers by a
     * coverage map the GPU correction pass does not read. */
    if (correction.anchor_point_coverage_image != nullptr) {
      return "a baked Anchor Point correction is read (no coverage map on the GPU)";
    }
    /* A CPU-computed effect has no pointwise shader branch: its result is a map the effect cache
     * computes on the CPU, so the whole stack falls back rather than shading without it. The
     * registry's #PAINT_LAYER_KIND_NEEDS_BAKE flag decides, so a new one needs no edit here -- except
     * for a kind the preview now computes on the GPU (#PAINT_LAYER_KIND_GPU_COMPUTED): the gather lays
     * an effect pass into the GPU result cache and reads the texture as a correction's map, which is
     * the same picture the CPU cache holds. A node-group filter takes the third way out: it stays
     * CPU-computed with no GPU branch, but its result is the CPU effect cache's own buffer, which
     * the effect stage uploads as the cache texture -- the same parity, reached by publishing the
     * CPU answer instead of recomputing it. Only the content-correction form is allowed here: a
     * mask item's grey has no effect stage of its own (the mask pass has no result texture to
     * read), so the mask-item loop above keeps refusing the kind. Whether the shader-bake backend
     * that computes the answer is registered is not asked here -- this function reads the
     * description only -- the effect stage refuses the job when it is not. Gated on the opacity
     * that decides the CPU lays it too.
     *
     * A Smart Filter chain's GPU-computed member rides these clauses unchanged: its correction
     * carries the member's own kind in #PaintMaterialCompositeCorrection::filter, so a Blur member
     * is exempt by the GPU-computed clause and a node-group member by the precomputed one, exactly
     * as a direct effect of the same kind is. A member the contract refuses (a NEEDS_BAKE kind
     * with neither a GPU pass nor a precomputed answer) never reaches a description at all -- the
     * unfolding skips it -- so there is nothing here to refuse; the stack previews without such a
     * member, and the portability blocks the shape at save time. */
    if (correction.opacity > 0.0f && paint_layer_filter_is_cpu_computed(correction.filter) &&
        !paint_layer_filter_is_gpu_computed(correction.filter) &&
        !paint_layer_filter_is_precomputed_result(correction.filter))
    {
      const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                             correction.filter);
      if (kind != nullptr) {
        return paint_layer_cpu_computed_reason(*kind);
      }
    }
    if (correction.opacity > 0.0f && gpu_filter_lacks_branch(correction.filter) &&
        !paint_layer_filter_is_precomputed_result(correction.filter))
    {
      return "a correction filter the preview shader has no branch for is read";
    }
  }
  /* An adjustment row has no per-kind refusal above, so a filter without a shader branch would
   * otherwise be shaded as a silent no-op. */
  if (gpu_filter_lacks_branch(layer.adjustment_filter)) {
    return "an adjustment filter the preview shader has no branch for is read";
  }
  return nullptr;
}

std::shared_ptr<ImBuf> BKE_paint_layers_effect_buffer_result(const Material &ma,
                                                             const MaterialPaintLayer &effect,
                                                             const int channel,
                                                             const int tile_number,
                                                             int &r_width,
                                                             int &r_height)
{
  r_width = 0;
  r_height = 0;
  /* A CPU-computed effect only: the GPU preview asks for the node-group filter, whose answer is
   * this buffer verbatim, but every #PAINT_LAYER_KIND_NEEDS_BAKE kind is served by the same cache
   * and the question is the same one #composite_effect_append_cpu_computed answers for the CPU
   * composite. A GPU-computed-only or non-effect row has no buffer here to stand for. */
  if (effect.role != MA_PAINT_LAYER_ROLE_EFFECT || !paint_layer_effect_is_cpu_computed(effect)) {
    return {};
  }
  /* The effect reads the row it hangs on: the cache's key does not name it, but the input describe
   * does (#BKE_paint_layers_effect_input_describe takes row and effect apart). The same lookup the
   * GPU job walk does, so both callers resolve one marker to one row. */
  const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, effect);
  if (row == nullptr) {
    return {};
  }
  /* The cached buffer, computed on a miss: the cache's mutex is never held while an effect
   * computes, so the compute below may render nested slices and nested effects of its own. The
   * preview's gather calls this on the main thread, the thread the bake computes on too, so the
   * nested renders run under the same terms as every other effect compute. */
  std::shared_ptr<ImBuf> buffer = paint_layer_effect_buffer_get(
      ma, *row, effect, channel, tile_number, /*describe_only=*/false, nullptr, nullptr);
  if (buffer == nullptr) {
    return {};
  }
  r_width = buffer->x;
  r_height = buffer->y;
  return buffer;
}

bool BKE_paint_layers_chain_member_input_describe(const Material &ma,
                                                  const MaterialPaintLayer &row,
                                                  const MaterialPaintLayer &chain,
                                                  const MaterialPaintLayer &member,
                                                  const int channel,
                                                  const bool dimensions_only,
                                                  const bool describe_only,
                                                  PaintMaterialCompositeImageLayer &r_entry,
                                                  const PaintLayerTileContext &context)
{
  /* The same describe the member's CPU effect cache renders from: the owner row's content up to
   * the chain, with the members before this one folded in. The GPU preview's member job asks it
   * describe-only, so its input render lays the prefix's effect results from the GPU caches the
   * earlier members' jobs published, exactly as the CPU compute lays them from the CPU cache's
   * buffers -- one describe, two backends. */
  return paint_layer_chain_member_input_describe(
      ma, row, chain, member, channel, dimensions_only, describe_only, r_entry, context);
}

std::shared_ptr<ImBuf> BKE_paint_layers_chain_member_effect_buffer_result(
    const Material &ma,
    const MaterialPaintLayer &row,
    const MaterialPaintLayer &chain,
    const MaterialPaintLayer &member,
    const int channel,
    const int tile_number,
    int &r_width,
    int &r_height)
{
  r_width = 0;
  r_height = 0;
  /* A CPU-computed member only: the GPU preview asks for the node-group filter, whose answer is
   * this buffer verbatim, but every member kind the contract lets into a chain is served by the
   * same cache. A pointwise or non-effect row has no buffer here to stand for, and a chain row
   * that is not the member's own is refused, so a stray pointer cannot key a foreign entry. */
  if (member.role != MA_PAINT_LAYER_ROLE_EFFECT || !paint_layer_effect_is_cpu_computed(member) ||
      chain.kind_subtype != MA_PAINT_LAYER_FILTER_CHAIN)
  {
    return {};
  }
  /* The cached buffer, computed on a miss under the composite marker: the cache's mutex is never
   * held while an effect computes, so the compute below may render nested slices and nested
   * effects of its own. The preview's gather calls this on the main thread, the thread the bake
   * computes on too, so the nested renders run under the same terms as every other effect
   * compute. */
  std::shared_ptr<ImBuf> buffer = paint_layer_chain_member_effect_buffer_get(
      ma, row, chain, member, channel, tile_number, /*describe_only=*/false, nullptr);
  if (buffer == nullptr) {
    return {};
  }
  r_width = buffer->x;
  r_height = buffer->y;
  return buffer;
}

/* -------------------------------------------------------------------- */
/** \name GPU Anchor Point Capture Plan
 * \{ */

namespace {

bool gpu_entry_has_adjustment(const PaintMaterialCompositeImageLayer &entry)
{
  if (entry.adjustment_filter != 0) {
    return true;
  }
  for (const PaintMaterialCompositeImageLayer &child : entry.children) {
    if (gpu_entry_has_adjustment(child)) {
      return true;
    }
  }
  return false;
}

/** A correction that lays a live Anchor Point slice. */
bool gpu_correction_is_live_reader(const PaintMaterialCompositeCorrection &correction)
{
  return !BLI_uuid_is_nil(correction.anchor_point_reader) && correction.image == nullptr &&
         !correction.has_constant_color && correction.filter == 0;
}

/** A mask item that lays a live Anchor Point slice. */
bool gpu_mask_item_is_live_reader(const PaintMaterialCompositeMaskItem &item)
{
  return item.opacity > 0.0f && item.image == nullptr && item.filter == 0 &&
         !BLI_uuid_is_nil(item.anchor_point_reader);
}

/** The capture a row or correction reader needs: a mask point read as content is a grey pair. */
PaintLayersGpuCaptureKind gpu_content_capture_kind(const int8_t target)
{
  return target == int8_t(PaintLayerAnchorPointTarget::Mask) ?
             PaintLayersGpuCaptureKind::MaskContent :
             PaintLayersGpuCaptureKind::Content;
}

}  // namespace

namespace {

/** The jobs of #BKE_paint_layers_gpu_capture_plan, discovered from the stacks and then described. */
class GpuCapturePlanner {
 public:
  GpuCapturePlanner(const Material &ma,
                    const int channel_num,
                    Vector<PaintLayersGpuCaptureJob> &r_jobs,
                    const char **r_reason,
                    const PaintLayerTileContext &context)
      : ma_(ma),
        channel_num_(channel_num),
        jobs_(r_jobs),
        reason_(r_reason),
        painted_object_(context.painted_object),
        tile_number_(BKE_paint_layers_tile_resolve(
            context.tile_number, nullptr, context.painted_object))
  {
  }

  /** Collect every reader the passes of \a stacks lay. False when the plan cannot be built. */
  bool collect_stacks(const Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
                      const Span<int> channels)
  {
    for (const int stack_index : stacks.index_range()) {
      const int channel = channels[stack_index];
      if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
        continue;
      }
      for (const PaintMaterialCompositeImageLayer &layer : stacks[stack_index]) {
        if (!this->collect_layer(layer, channel, 0)) {
          return false;
        }
      }
    }
    return true;
  }

  /**
   * Describe each job once. Jobs discovered while describing are appended and described by the
   * same loop; the entry stays on the job, so the gather never describes a slice twice.
   */
  bool describe_jobs()
  {
    for (int index = 0; index < int(jobs_.size()); index++) {
      const bUUID reader_marker = jobs_[index].reader;
      const int channel = jobs_[index].channel;
      const PaintLayersGpuCaptureKind kind = jobs_[index].kind;
      const int depth = depths_[index];
      Material &ma_mut = const_cast<Material &>(ma_);
      MaterialPaintLayer *reader = BKE_paint_layers_find(ma_mut, reader_marker);
      if (reader == nullptr) {
        return this->fail("an Anchor Point reader cannot be found");
      }
      const bool mask_kind = kind != PaintLayersGpuCaptureKind::Content;
      PaintMaterialCompositeImageLayer entry;
      if (!BKE_paint_layers_slice_describe(
              ma_,
              *reader,
              channel,
              entry,
              nullptr,
              true,
              mask_kind,
              PaintLayerTileContext{.painted_object = painted_object_, .tile_number = tile_number_}))
      {
        return this->fail("an Anchor Point slice cannot be described");
      }
      const bool clear_to_one = mask_kind && entry.mask_items.is_empty();
      if (gpu_entry_has_adjustment(entry)) {
        return this->fail("an adjustment row is read as an Anchor Point source");
      }
      if (!clear_to_one) {
        if (const char *reason = BKE_paint_layers_gpu_unsupported_reason(entry, mask_kind)) {
          return this->fail(reason);
        }
      }
      /* Nested readers are collected from the description before it is moved onto the job: the job
       * vector may reallocate while they are appended. */
      if (!this->collect_nested_entry(entry, channel, depth + 1, reader_marker)) {
        return false;
      }
      jobs_[index].clear_to_one = clear_to_one;
      jobs_[index].entry = std::move(entry);
    }
    return true;
  }

 private:
  const Material &ma_;
  const int channel_num_;
  Vector<PaintLayersGpuCaptureJob> &jobs_;
  const char **reason_;
  /** Describes a slice's projection against the painted surface; null leaves it without one. */
  PaintLayerSurfaceRef painted_object_;
  /** The UDIM tile the plan's slices describe and the slots capture for. */
  int tile_number_ = PAINT_TILE_NUMBER_BASE;
  /** One depth per job, parallel to #jobs_: a job discovered inside another's description is one
   * level deeper. The hard limit bounds both recursion and work on a malicious description. */
  Vector<int> depths_;

  bool fail(const char *reason)
  {
    if (reason_ != nullptr) {
      *reason_ = reason;
    }
    return false;
  }

  /** Add \a reader's job unless the same slot is already planned. Returns false only when the plan
   * cannot be built. */
  bool add_job(const bUUID &reader,
               const bUUID &point,
               const int channel,
               const PaintLayersGpuCaptureKind kind,
               const int depth)
  {
    if (BLI_uuid_is_nil(reader) || BLI_uuid_is_nil(point)) {
      return true;
    }
    const bool mask_item = kind == PaintLayersGpuCaptureKind::MaskItem;
    for (PaintLayersGpuCaptureJob &job : jobs_) {
      if (job.kind != kind || !BLI_uuid_equal(job.point, point)) {
        continue;
      }
      if (!mask_item && job.channel != channel) {
        continue;
      }
      /* A shared slot is one capture; only count a reader that was not already named for it. */
      bool seen = false;
      for (const bUUID &known : job.readers) {
        if (BLI_uuid_equal(known, reader)) {
          seen = true;
          break;
        }
      }
      if (!seen) {
        job.readers.append(reader);
        job.reader_count = int(job.readers.size());
      }
      return true;
    }
    if (depth > PAINT_LAYER_ANCHOR_POINT_DEPTH_HARD) {
      return this->fail("the Anchor Point chain is too deep for the GPU path");
    }
    if (int(jobs_.size()) >= PAINT_LAYER_ANCHOR_POINT_LIMIT * channel_num_) {
      return this->fail("too many Anchor Point slices for the GPU path");
    }
    PaintLayersGpuCaptureJob job;
    job.reader = reader;
    job.point = point;
    job.channel = channel;
    job.kind = kind;
    job.rank = paint_layer_anchor_point_rank(ma_, point);
    job.readers.append(reader);
    jobs_.append(std::move(job));
    depths_.append(depth);
    return true;
  }

  /** The readers a row (or a described slice entry) lays through its corrections and mask items. */
  bool collect_corrections_and_mask(const PaintMaterialCompositeImageLayer &row,
                                    const int channel,
                                    const int depth)
  {
    for (const PaintMaterialCompositeCorrection &correction : row.content_corrections) {
      if (correction.opacity <= 0.0f || !gpu_correction_is_live_reader(correction)) {
        continue;
      }
      if (!this->add_job(correction.anchor_point_reader,
                         correction.anchor_point_point,
                         channel,
                         gpu_content_capture_kind(correction.anchor_point_target),
                         depth))
      {
        return false;
      }
    }
    for (const PaintMaterialCompositeMaskItem &item : row.mask_items) {
      if (!gpu_mask_item_is_live_reader(item)) {
        continue;
      }
      const bool mask_target = item.anchor_point_target ==
                               int8_t(PaintLayerAnchorPointTarget::Mask);
      /* A content point read by a mask item is captured in the channel its coverage comes from. */
      if (!this->add_job(item.anchor_point_reader,
                         item.anchor_point_point,
                         mask_target ? channel : item.anchor_point_read_channel,
                         mask_target ? PaintLayersGpuCaptureKind::MaskItem :
                                       PaintLayersGpuCaptureKind::Content,
                         depth))
      {
        return false;
      }
    }
    return true;
  }

  /** Collect a row that the passes will lay, including its own reader. A hidden or zero-opacity row
   * is skipped: nothing reads it unless another reader's slice contains it, which
   * #collect_nested_entry handles. */
  bool collect_layer(const PaintMaterialCompositeImageLayer &layer,
                     const int channel,
                     const int depth)
  {
    if (!layer.enabled || layer.opacity <= 0.0f) {
      return true;
    }
    if (!BLI_uuid_is_nil(layer.anchor_point_reader) && layer.color_image == nullptr &&
        !this->add_job(layer.anchor_point_reader,
                       layer.anchor_point_point,
                       channel,
                       gpu_content_capture_kind(layer.anchor_point_target),
                       depth))
    {
      return false;
    }
    if (!this->collect_corrections_and_mask(layer, channel, depth)) {
      return false;
    }
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      if (!this->collect_layer(child, channel, depth)) {
        return false;
      }
    }
    return true;
  }

  /** The readers inside an already described slice. The entry's own reader is the source row's own
   * reader, which is a nested reader unless it is the one this job was made for. */
  bool collect_nested_entry(const PaintMaterialCompositeImageLayer &entry,
                            const int channel,
                            const int depth,
                            const bUUID &job_reader)
  {
    if (!BLI_uuid_is_nil(entry.anchor_point_reader) && entry.color_image == nullptr &&
        !BLI_uuid_equal(entry.anchor_point_reader, job_reader) &&
        !this->add_job(entry.anchor_point_reader,
                       entry.anchor_point_point,
                       channel,
                       gpu_content_capture_kind(entry.anchor_point_target),
                       depth))
    {
      return false;
    }
    if (!this->collect_corrections_and_mask(entry, channel, depth)) {
      return false;
    }
    for (const PaintMaterialCompositeImageLayer &child : entry.children) {
      if (!this->collect_layer(child, channel, depth)) {
        return false;
      }
    }
    return true;
  }
};

}  // namespace

bool BKE_paint_layers_gpu_capture_plan(
    const Material &ma,
    const Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    const Span<int> channels,
    Vector<PaintLayersGpuCaptureJob> &r_jobs,
    const char **r_reason,
    const PaintLayerTileContext &context)
{
  r_jobs.clear();
  if (r_reason != nullptr) {
    *r_reason = nullptr;
  }
  if (stacks.size() != channels.size()) {
    if (r_reason != nullptr) {
      *r_reason = "the capture plan's stacks and channels differ in size";
    }
    return false;
  }

  GpuCapturePlanner planner(ma, int(channels.size()), r_jobs, r_reason, context);
  if (!planner.collect_stacks(stacks, channels) || !planner.describe_jobs()) {
    return false;
  }

  /* A reader nested in a slice sits above the point it names, so its own point must be captured
   * first. The rank of the point, not of the reader, orders the passes. */
  std::stable_sort(r_jobs.begin(),
                   r_jobs.end(),
                   [](const PaintLayersGpuCaptureJob &a, const PaintLayersGpuCaptureJob &b) {
                     return a.rank < b.rank;
                   });
  return true;
}

bool BKE_paint_layers_gpu_slot_key_equal(const PaintLayersGpuSlotKey &a,
                                         const PaintLayersGpuSlotKey &b)
{
  /* The channel is the #eMaterialPaintChannel value, never the combined-stack index: Alpha and
   * Emission occupy the same position in one order and none in the other. */
  return a.channel == b.channel && a.tile_number == b.tile_number &&
         BLI_uuid_equal(a.point, b.point);
}

int BKE_paint_layers_gpu_slot_find(const Span<PaintLayersGpuSlotKey> slots,
                                   const bUUID &point,
                                   const int channel,
                                   const int tile_number)
{
  /* A caller that named no tile means the base one, the tile its slots were captured for. */
  const PaintLayersGpuSlotKey wanted{
      point, channel, BKE_paint_layers_tile_resolve(tile_number, nullptr, nullptr)};
  for (const int i : slots.index_range()) {
    if (BKE_paint_layers_gpu_slot_key_equal(slots[i], wanted)) {
      return i;
    }
  }
  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Verification
 * \{ */

PaintLayersGpuVerifyDiff BKE_paint_layers_gpu_verify_compare_slot(const float *gpu,
                                                                  const float *cpu,
                                                                  const int64_t sample_count,
                                                                  const int components,
                                                                  const float tolerance)
{
  PaintLayersGpuVerifyDiff result;
  result.ok = true;
  result.max_diff = 0.0f;
  result.samples = 0;
  if (gpu == nullptr || cpu == nullptr || sample_count <= 0 || components <= 0) {
    return result;
  }
  result.samples = sample_count;
  double sample_diff_sum = 0.0;
  for (const int64_t sample : IndexRange(sample_count)) {
    float sample_diff = 0.0f;
    for (const int component : IndexRange(components)) {
      const int64_t index = sample * components + component;
      const float diff = std::abs(gpu[index] - cpu[index]);
      if (!(diff == diff)) {
        /* A NaN compares false with everything, so std::max would drop it: a broken slot must not
         * pass. */
        result.has_nan = true;
        continue;
      }
      sample_diff = std::max(sample_diff, diff);
    }
    result.max_diff = std::max(result.max_diff, sample_diff);
    sample_diff_sum += double(sample_diff);
  }
  /* The GPU slot is sampled bilinearly in screen space and the CPU slice is read at the nearest
   * texel, so at the edge of a stroke a lone sample can differ by a lot while the slot is right.
   * Judge the mean of the per-sample worst component; keep the maximum for the report. */
  result.mean_diff = float(sample_diff_sum / double(sample_count));
  result.ok = !result.has_nan && result.mean_diff <= tolerance;
  return result;
}

PaintLayersGpuSlotGuard::PaintLayersGpuSlotGuard(PaintLayersGpuSlotGuard &&other) noexcept
    : release_(other.release_),
      release_data_(other.release_data_),
      resources_(std::move(other.resources_))
{
  other.release_ = nullptr;
  other.release_data_ = nullptr;
}

PaintLayersGpuSlotGuard &PaintLayersGpuSlotGuard::operator=(
    PaintLayersGpuSlotGuard &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  release();
  release_ = other.release_;
  release_data_ = other.release_data_;
  resources_ = std::move(other.resources_);
  other.release_ = nullptr;
  other.release_data_ = nullptr;
  return *this;
}

PaintLayersGpuSlotGuard::~PaintLayersGpuSlotGuard()
{
  release();
}

void PaintLayersGpuSlotGuard::set_release(const ReleaseFn fn, void *release_data)
{
  release();
  release_ = fn;
  release_data_ = release_data;
}

void PaintLayersGpuSlotGuard::own(void *resource)
{
  if (resource != nullptr) {
    resources_.append(resource);
  }
}

void PaintLayersGpuSlotGuard::release()
{
  if (release_ != nullptr) {
    for (void *resource : resources_) {
      release_(release_data_, resource);
    }
  }
  resources_.clear();
}

/** \} */

bool BKE_paint_layers_correction_blend_is_normal(const MaterialPaintLayer &effect,
                                                 const int channel)
{
  const CompositeBlend blend = layer_channel_blend(effect, channel);
  /* Normal has no Mix of its own: combining the normals is what its plain blend means. */
  return ELEM(blend, CompositeBlend::Mix, CompositeBlend::NormalCombine);
}

bool BKE_paint_layers_corrections_row_describe(const Material &ma,
                                               const Span<MaterialPaintLayer *> corrections,
                                               const int channel,
                                               const int size,
                                               const ImageUser *iuser,
                                               const int tile_number,
                                               PaintMaterialCompositeImageLayer &r_row,
                                               const std::shared_ptr<const PaintMaterialCompositeSurface>
                                                   &context_surface,
                                               PaintLayerSurfaceRef painted_object)
{
  r_row = PaintMaterialCompositeImageLayer();
  /* The same shape a Paint row kept only for its corrections takes (#composite_row_source_describe):
   * it covers nothing of its own, so the corrections bring the coverage in. */
  r_row.enabled = true;
  r_row.has_constant_color = true;
  r_row.empty_base = true;
  r_row.empty_base_size = size;
  r_row.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
      eMaterialPaintChannel(channel));
  for (const MaterialPaintLayer *effect : corrections) {
    const int64_t before = r_row.content_corrections.size();
    composite_effect_append(ma,
                            nullptr,
                            *effect,
                            channel,
                            1.0f,
                            nullptr,
                            false,
                            false,
                            tile_number,
                            context_surface,
                            r_row,
                            painted_object);
    if (r_row.content_corrections.size() == before) {
      continue;
    }
    PaintMaterialCompositeCorrection &correction = r_row.content_corrections.last();
    if (correction.image == nullptr) {
      continue;
    }
    /* A map without this tile lays nothing on it. */
    const bool has_tile = (correction.image->source == IMA_SRC_TILED) ?
                              BKE_image_get_tile(correction.image, tile_number) != nullptr :
                              tile_number == 1001;
    if (has_tile) {
      /* A Material correction reads the map its source was rendered into, at the render's own side:
       * unlike a painted map it need not be the canvas's. An identity mapping resamples it to the
       * canvas, where the evaluator would refuse the mismatch. */
      if (paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_READS_SOURCE) &&
          !correction.mapping.enabled)
      {
        ImageUser probe_user;
        BKE_imageuser_default(&probe_user);
        probe_user.tile = tile_number;
        void *lock = nullptr;
        const ImBuf *probe = BKE_image_acquire_ibuf(correction.image, &probe_user, &lock);
        if (probe != nullptr && (probe->x != size || probe->y != size)) {
          correction.mapping.enabled = true;
          correction.mapping.tangent_normal = false;
        }
        BKE_image_release_ibuf(correction.image, const_cast<ImBuf *>(probe), lock);
      }
      correction.iuser = iuser;
    }
    else {
      r_row.content_corrections.remove_last();
    }
  }
  return !r_row.content_corrections.is_empty();
}

}  // namespace blender
