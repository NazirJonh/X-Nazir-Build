/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Hover (pre-stroke) brush zone preview for Sculpt Curves.
 *
 * Computes the weights the first step of a deform-brush stroke at pressure 1.0 would publish
 * (see #sculpt_influence_viz.cc), without mutating any geometry and without touching the
 * depsgraph or the draw batch cache. The paint cursor callback draws the result in immediate
 * mode. Only deform brushes are previewed (comb, snake hook, pinch, puff, smooth, slide,
 * grow/shrink); Add, Delete, Density and Selection Paint have no preview.
 *
 * The preview is an approximation, not a simulation of the stroke:
 * - Projected (tube) brushes are tested against the cursor point, while the stroke tests
 *   against the previous-to-current mouse segment (there is no previous position on hover).
 * - Spherical brushes freshly sample the 3D brush position on every compute, while
 *   pinch/puff/smooth freeze it when the stroke starts and comb/snake hook resample every step.
 * - Slide tests the current deformed positions, while the stroke freezes them at its start.
 * - Pressure is fixed at 1.0, so the preview matches a full-pressure first step.
 */

#include "sculpt_intern.hh"
#include "sculpt_multi_object.hh"

#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"

#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_colortools.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "UI_resources.hh"

#include "GEO_reverse_uv_sampler.hh"

#include "editors/sculpt_paint/paint_cursor.hh"

namespace blender::ed::sculpt_paint {

using geometry::ReverseUVSampler;

/** Above this point count the preview is skipped to keep cursor movement responsive. */
constexpr int hover_preview_max_points = 2'000'000;

struct HoverContext {
  const Depsgraph *depsgraph = nullptr;
  const ARegion *region = nullptr;
  const View3D *v3d = nullptr;
  const RegionView3D *rv3d = nullptr;
  const Object *object = nullptr;
  const Curves *curves_id = nullptr;
  const CurvesGeometry *curves = nullptr;
  const Brush *brush = nullptr;
  float radius_factor = 1.0f;
  float radius_base_re = 0.0f;
  float radius_re = 0.0f;
  float2 mouse_re;
};

static float hover_brush_radius_factor(const Brush &brush)
{
  /* Same as #brush_radius_factor with pressure 1.0 (see #sculpt_ops.cc). */
  if (BKE_brush_use_size_pressure(&brush)) {
    return BKE_curvemapping_evaluateF(brush.curve_size, 0, 1.0f);
  }
  return 1.0f;
}

static void hover_preview_compact(Array<float> &zone,
                                  const OffsetIndices<int> points_by_curve,
                                  CurvesHoverPreview &r_preview)
{
  r_preview.hit_curves.clear();
  for (const int curve_i : points_by_curve.index_range()) {
    for (const int point_i : points_by_curve[curve_i]) {
      if (zone[point_i] > 0.0f) {
        r_preview.hit_curves.append(curve_i);
        break;
      }
    }
  }
  /* Kept whole (not compacted): the line strips address points in curve order. */
  r_preview.zone_weights = std::move(zone);
}

static void hover_preview_comb(const HoverContext &ctx,
                               const VArray<float> &point_factors,
                               const IndexMask &curve_selection,
                               MutableSpan<float> r_zone)
{
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);

  /* Curve lengths replace the constraint solver totals of the stroke (see #CombOperation). */
  Array<float> seg_lengths(ctx.curves->points_num(), 0.0f);
  Array<float> curve_lengths(ctx.curves->curves_num(), 0.0f);
  curve_selection.foreach_index([&](const int64_t curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    float total = 0.0f;
    for (const int segment_i : points.drop_back(1)) {
      const float length = math::distance(deformation.positions[segment_i],
                                          deformation.positions[segment_i + 1]);
      seg_lengths[segment_i] = length;
      total += length;
    }
    curve_lengths[curve_i] = total;
  });

  CurveMapping &curve_parameter_falloff_mapping =
      *ctx.brush->curves_sculpt_settings->curve_parameter_falloff;
  BKE_curvemapping_init(&curve_parameter_falloff_mapping);

  /* Traversal follows the falloff shape only: unlike the stroke, orbit-around-selection
   * does not change the hover path (it only relocates the stroke rotation center). */
  const bool use_spherical = ctx.brush->falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE;
  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(ctx.curves_id->symmetry));

  if (!use_spherical) {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx.rv3d, ctx.object);
    const float radius_sq_re = pow2f(ctx.radius_re);
    for (const float4x4 &brush_transform : symmetry_transforms) {
      const float4x4 brush_transform_inv = math::invert(brush_transform);
      curve_selection.foreach_index([&](const int64_t curve_i) {
        const IndexRange points = points_by_curve[curve_i];
        const float total_length_inv = math::safe_rcp(curve_lengths[curve_i]);
        float current_length = 0.0f;
        for (const int point_i : points.drop_front(1)) {
          current_length += seg_lengths[point_i - 1];
          /* Tested against the cursor point: the stroke tests against the mouse segment. */
          const float2 pos_re = ED_view3d_project_float_v2_m4(
              ctx.region,
              math::transform_point(brush_transform_inv, deformation.positions[point_i]),
              projection);
          const float dist_sq_re = math::distance_squared(pos_re, ctx.mouse_re);
          if (dist_sq_re > radius_sq_re) {
            continue;
          }
          const float radius_falloff = BKE_brush_curve_strength(
              ctx.brush, std::sqrt(dist_sq_re), ctx.radius_re);
          const float curve_falloff = BKE_curvemapping_evaluateF(
              &curve_parameter_falloff_mapping, 0, current_length * total_length_inv);
          math::max_inplace(r_zone[point_i],
                            comb_point_zone_weight(
                                curve_falloff, radius_falloff, point_factors[point_i]));
        }
      });
    }
    return;
  }

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx.depsgraph,
                                                                       *ctx.region,
                                                                       *ctx.v3d,
                                                                       *ctx.rv3d,
                                                                       *ctx.object,
                                                                       ctx.mouse_re,
                                                                       ctx.radius_base_re);
  if (!brush_3d.has_value()) {
    return;
  }
  const float radius_cu = brush_3d->radius_cu * ctx.radius_factor;
  const float radius_sq_cu = pow2f(radius_cu);
  for (const float4x4 &brush_transform : symmetry_transforms) {
    /* Single sampled position: the stroke tests against the resampled mouse segment. */
    const float3 brush_pos_cu = math::transform_point(brush_transform, brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      const float total_length_inv = math::safe_rcp(curve_lengths[curve_i]);
      float current_length = 0.0f;
      for (const int point_i : points.drop_front(1)) {
        current_length += seg_lengths[point_i - 1];
        const float dist_sq_cu = math::distance_squared(deformation.positions[point_i],
                                                        brush_pos_cu);
        if (dist_sq_cu > radius_sq_cu) {
          continue;
        }
        const float radius_falloff = BKE_brush_curve_strength(
            ctx.brush, std::sqrt(dist_sq_cu), radius_cu);
        const float curve_falloff = BKE_curvemapping_evaluateF(
            &curve_parameter_falloff_mapping, 0, current_length * total_length_inv);
        math::max_inplace(r_zone[point_i],
                          comb_point_zone_weight(
                              curve_falloff, radius_falloff, point_factors[point_i]));
      }
    });
  }
}

static void hover_preview_snake_hook(const HoverContext &ctx,
                                     const VArray<float> &curve_factors,
                                     const IndexMask &curve_selection,
                                     MutableSpan<float> r_zone)
{
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);
  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(ctx.curves_id->symmetry));

  /* The stroke tests the last (tip) point of each curve; the rest of the curve follows it. */
  if (ctx.brush->falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE) {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx.rv3d, ctx.object);
    const float radius_sq_re = pow2f(ctx.radius_re);
    for (const float4x4 &brush_transform : symmetry_transforms) {
      const float4x4 brush_transform_inv = math::invert(brush_transform);
      curve_selection.foreach_index([&](const int64_t curve_i) {
        const int last_point_i = points_by_curve[curve_i].last();
        const float2 pos_re = ED_view3d_project_float_v2_m4(
            ctx.region,
            math::transform_point(brush_transform_inv, deformation.positions[last_point_i]),
            projection);
        const float dist_sq_re = math::distance_squared(pos_re, ctx.mouse_re);
        if (dist_sq_re > radius_sq_re) {
          return;
        }
        math::max_inplace(r_zone[last_point_i],
                          snake_hook_curve_zone_weight(
                              BKE_brush_curve_strength(
                                  ctx.brush, std::sqrt(dist_sq_re), ctx.radius_re),
                              curve_factors[curve_i]));
      });
    }
    return;
  }

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx.depsgraph,
                                                                       *ctx.region,
                                                                       *ctx.v3d,
                                                                       *ctx.rv3d,
                                                                       *ctx.object,
                                                                       ctx.mouse_re,
                                                                       ctx.radius_base_re);
  if (!brush_3d.has_value()) {
    return;
  }
  const float radius_cu = brush_3d->radius_cu * ctx.radius_factor;
  const float radius_sq_cu = pow2f(radius_cu);
  for (const float4x4 &brush_transform : symmetry_transforms) {
    const float3 brush_pos_cu = math::transform_point(brush_transform, brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      const int last_point_i = points_by_curve[curve_i].last();
      const float dist_sq_cu = math::distance_squared(deformation.positions[last_point_i],
                                                      brush_pos_cu);
      if (dist_sq_cu > radius_sq_cu) {
        return;
      }
      math::max_inplace(r_zone[last_point_i],
                        snake_hook_curve_zone_weight(
                            BKE_brush_curve_strength(
                                ctx.brush, std::sqrt(dist_sq_cu), radius_cu),
                            curve_factors[curve_i]));
    });
  }
}

static void hover_preview_pinch(const HoverContext &ctx,
                                const VArray<float> &point_factors,
                                const IndexMask &curve_selection,
                                MutableSpan<float> r_zone)
{
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);
  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(ctx.curves_id->symmetry));

  if (ctx.brush->falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE) {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx.rv3d, ctx.object);
    const float radius_sq_re = pow2f(ctx.radius_re);
    for (const float4x4 &brush_transform : symmetry_transforms) {
      const float4x4 brush_transform_inv = math::invert(brush_transform);
      curve_selection.foreach_index([&](const int64_t curve_i) {
        for (const int point_i : points_by_curve[curve_i].drop_front(1)) {
          const float2 pos_re = ED_view3d_project_float_v2_m4(
              ctx.region,
              math::transform_point(brush_transform_inv, deformation.positions[point_i]),
              projection);
          const float dist_sq_re = math::distance_squared(pos_re, ctx.mouse_re);
          if (dist_sq_re > radius_sq_re) {
            continue;
          }
          const float t = math::safe_divide(std::sqrt(dist_sq_re), ctx.radius_re);
          const float radius_falloff = t * BKE_brush_curve_strength(ctx.brush, t, 1.0f);
          math::max_inplace(r_zone[point_i],
                            pinch_point_zone_weight(radius_falloff, point_factors[point_i]));
        }
      });
    }
    return;
  }

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx.depsgraph,
                                                                       *ctx.region,
                                                                       *ctx.v3d,
                                                                       *ctx.rv3d,
                                                                       *ctx.object,
                                                                       ctx.mouse_re,
                                                                       ctx.radius_base_re);
  if (!brush_3d.has_value()) {
    return;
  }
  const float radius_cu = brush_3d->radius_cu * ctx.radius_factor;
  const float radius_sq_cu = pow2f(radius_cu);
  for (const float4x4 &brush_transform : symmetry_transforms) {
    const float3 brush_pos_cu = math::transform_point(brush_transform, brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      for (const int point_i : points_by_curve[curve_i].drop_front(1)) {
        const float dist_sq_cu = math::distance_squared(deformation.positions[point_i],
                                                        brush_pos_cu);
        if (dist_sq_cu > radius_sq_cu) {
          continue;
        }
        const float t = math::safe_divide(std::sqrt(dist_sq_cu), radius_cu);
        const float radius_falloff = t * BKE_brush_curve_strength(ctx.brush, t, 1.0f);
        math::max_inplace(r_zone[point_i],
                          pinch_point_zone_weight(radius_falloff, point_factors[point_i]));
      }
    });
  }
}

static void hover_preview_smooth(const HoverContext &ctx,
                                 const VArray<float> &point_factors,
                                 const IndexMask &curve_selection,
                                 MutableSpan<float> r_zone)
{
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);
  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(ctx.curves_id->symmetry));

  if (ctx.brush->falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE) {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx.rv3d, ctx.object);
    const float radius_sq_re = pow2f(ctx.radius_re);
    for (const float4x4 &brush_transform : symmetry_transforms) {
      const float4x4 brush_transform_inv = math::invert(brush_transform);
      curve_selection.foreach_index([&](const int64_t curve_i) {
        for (const int point_i : points_by_curve[curve_i]) {
          const float2 pos_re = ED_view3d_project_float_v2_m4(
              ctx.region,
              math::transform_point(brush_transform_inv, deformation.positions[point_i]),
              projection);
          const float dist_sq_re = math::distance_squared(pos_re, ctx.mouse_re);
          if (dist_sq_re > radius_sq_re) {
            continue;
          }
          math::max_inplace(r_zone[point_i],
                            smooth_point_zone_weight(
                                BKE_brush_curve_strength(
                                    ctx.brush, std::sqrt(dist_sq_re), ctx.radius_re),
                                point_factors[point_i]));
        }
      });
    }
    return;
  }

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx.depsgraph,
                                                                       *ctx.region,
                                                                       *ctx.v3d,
                                                                       *ctx.rv3d,
                                                                       *ctx.object,
                                                                       ctx.mouse_re,
                                                                       ctx.radius_base_re);
  if (!brush_3d.has_value()) {
    return;
  }
  const float radius_cu = brush_3d->radius_cu * ctx.radius_factor;
  const float radius_sq_cu = pow2f(radius_cu);
  for (const float4x4 &brush_transform : symmetry_transforms) {
    const float3 brush_pos_cu = math::transform_point(brush_transform, brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      for (const int point_i : points_by_curve[curve_i]) {
        const float dist_sq_cu = math::distance_squared(deformation.positions[point_i],
                                                        brush_pos_cu);
        if (dist_sq_cu > radius_sq_cu) {
          continue;
        }
        math::max_inplace(r_zone[point_i],
                          smooth_point_zone_weight(
                              BKE_brush_curve_strength(
                                  ctx.brush, std::sqrt(dist_sq_cu), radius_cu),
                              point_factors[point_i]));
      }
    });
  }
}

static void hover_preview_puff(const HoverContext &ctx,
                               const IndexMask &curve_selection,
                               MutableSpan<float> r_zone)
{
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);
  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(ctx.curves_id->symmetry));

  /* Segment endpoints in the radius carry the segment weight; shared endpoints keep the
   * maximum, so curves passing through the zone read as a gradient along the passing part. */
  if (ctx.brush->falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE) {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx.rv3d, ctx.object);
    const float radius_sq_re = pow2f(ctx.radius_re);
    for (const float4x4 &brush_transform : symmetry_transforms) {
      const float4x4 brush_transform_inv = math::invert(brush_transform);
      curve_selection.foreach_index([&](const int64_t curve_i) {
        const IndexRange points = points_by_curve[curve_i];
        float2 prev_pos_re = ED_view3d_project_float_v2_m4(
            ctx.region,
            math::transform_point(brush_transform_inv, deformation.positions[points[0]]),
            projection);
        for (const int point_i : points.drop_front(1)) {
          const float2 pos_re = ED_view3d_project_float_v2_m4(
              ctx.region,
              math::transform_point(brush_transform_inv, deformation.positions[point_i]),
              projection);
          const float dist_sq_re = dist_squared_to_line_segment_v2(
              ctx.mouse_re, prev_pos_re, pos_re);
          /* Carried to the next segment exactly like the stroke updates it. */
          prev_pos_re = pos_re;
          if (dist_sq_re > radius_sq_re) {
            continue;
          }
          const float weight = puff_segment_zone_weight(
              ctx.brush, std::sqrt(dist_sq_re), ctx.radius_re);
          math::max_inplace(r_zone[point_i - 1], weight);
          math::max_inplace(r_zone[point_i], weight);
        }
      });
    }
    return;
  }

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx.depsgraph,
                                                                       *ctx.region,
                                                                       *ctx.v3d,
                                                                       *ctx.rv3d,
                                                                       *ctx.object,
                                                                       ctx.mouse_re,
                                                                       ctx.radius_base_re);
  if (!brush_3d.has_value()) {
    return;
  }
  const float radius_cu = brush_3d->radius_cu * ctx.radius_factor;
  const float radius_sq_cu = pow2f(radius_cu);
  for (const float4x4 &brush_transform : symmetry_transforms) {
    const float3 brush_pos_cu = math::transform_point(brush_transform, brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int segment_i : points.drop_back(1)) {
        const float dist_sq_cu = dist_squared_to_line_segment_v3(
            brush_pos_cu, deformation.positions[segment_i], deformation.positions[segment_i + 1]);
        if (dist_sq_cu > radius_sq_cu) {
          continue;
        }
        const float weight = puff_segment_zone_weight(
            ctx.brush, std::sqrt(dist_sq_cu), radius_cu);
        math::max_inplace(r_zone[segment_i], weight);
        math::max_inplace(r_zone[segment_i + 1], weight);
      }
    });
  }
}

static void hover_preview_slide(const HoverContext &ctx,
                                const VArray<float> &curve_factors,
                                const IndexMask &curve_selection,
                                MutableSpan<float> r_zone)
{
  const Curves *curves_id = ctx.curves_id;
  if (curves_id->surface == nullptr || curves_id->surface->type != OB_MESH) {
    return;
  }
  if (curves_id->surface_uv_map == nullptr || !ctx.curves->surface_uv_coords()) {
    return;
  }
  const StringRefNull uv_map_name = curves_id->surface_uv_map;

  const bke::CurvesSurfaceTransforms transforms(*ctx.object, curves_id->surface);

  const Mesh *surface_orig = id_cast<const Mesh *>(curves_id->surface->data);
  if (surface_orig->faces_num == 0) {
    return;
  }
  const Span<int3> surface_corner_tris_orig = surface_orig->corner_tris();
  const VArraySpan<float2> surface_uv_map_orig = *surface_orig->attributes().lookup<float2>(
      uv_map_name, bke::AttrDomain::Corner);
  if (surface_uv_map_orig.is_empty()) {
    return;
  }
  Object *surface_ob_eval = DEG_get_evaluated(ctx.depsgraph, curves_id->surface);
  if (surface_ob_eval == nullptr) {
    return;
  }
  Mesh *surface_eval = BKE_object_get_evaluated_mesh(surface_ob_eval);
  if (surface_eval == nullptr || surface_eval->faces_num == 0) {
    return;
  }
  const bke::BVHTreeFromMesh surface_bvh = surface_eval->bvh_corner_tris();

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_surface_3d_brush(
      *ctx.depsgraph, *ctx.region, *ctx.v3d, transforms, surface_bvh, ctx.mouse_re, ctx.radius_re);
  if (!brush_3d.has_value()) {
    return;
  }

  /* The stroke ignores the pressure radius factor here; mirror it (see #find_curves_to_slide). */
  const float radius_sq_cu = pow2f(brush_3d->radius_cu);
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const Span<int> offsets = ctx.curves->offsets();
  const Span<float2> surface_uv_coords = *ctx.curves->surface_uv_coords();
  const ReverseUVSampler reverse_uv_sampler(surface_uv_map_orig, surface_corner_tris_orig);

  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(curves_id->symmetry));

  /* Pre-pass: determine which symmetry brush is closest to each curve root. */
  Array<int> best_brush(ctx.curves->curves_num(), -1);
  Array<float> min_dist_sq(ctx.curves->curves_num(), FLT_MAX);
  for (const int brush_i : symmetry_transforms.index_range()) {
    const float3 brush_pos_cu = math::transform_point(symmetry_transforms[brush_i],
                                                      brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      /* Current deformed positions: the stroke freezes them at its start. */
      const float dist_sq = math::distance_squared(deformation.positions[offsets[curve_i]],
                                                   brush_pos_cu);
      if (dist_sq <= radius_sq_cu && dist_sq < min_dist_sq[curve_i]) {
        min_dist_sq[curve_i] = dist_sq;
        best_brush[curve_i] = brush_i;
      }
    });
  }

  /* Main pass: gather the curves each symmetry brush would slide. */
  for (const int brush_i : symmetry_transforms.index_range()) {
    const float3 brush_pos_cu = math::transform_point(symmetry_transforms[brush_i],
                                                      brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      if (best_brush[curve_i] != brush_i) {
        return;
      }
      const int first_point_i = offsets[curve_i];
      const float dist_cu = math::distance(deformation.positions[first_point_i], brush_pos_cu);
      if (reverse_uv_sampler.sample(surface_uv_coords[curve_i]).type !=
          ReverseUVSampler::ResultType::Ok)
      {
        /* The curve has no valid surface attachment and would not slide. */
        return;
      }
      math::max_inplace(r_zone[first_point_i],
                        slide_curve_zone_weight(
                            BKE_brush_curve_strength(ctx.brush, dist_cu, brush_3d->radius_cu),
                            curve_factors[curve_i]));
    });
  }
}

static void hover_preview_grow_shrink(const HoverContext &ctx,
                                      const VArray<float> &curve_selection_factors,
                                      const IndexMask &curve_selection,
                                      MutableSpan<float> r_zone)
{
  const OffsetIndices<int> points_by_curve = ctx.curves->points_by_curve();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(*ctx.depsgraph, *ctx.object);
  const Vector<float4x4> symmetry_transforms = get_symmetry_brush_transforms(
      eCurvesSymmetryType(ctx.curves_id->symmetry));

  /* Segment endpoints in the radius carry the segment weight (see #hover_preview_puff). */
  if (ctx.brush->falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE) {
    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx.rv3d, ctx.object);
    const float radius_sq_re = pow2f(ctx.radius_re);
    Vector<float4x4> symmetry_transforms_inv;
    for (const float4x4 &brush_transform : symmetry_transforms) {
      /* Use explicit template call as MSVC 2019 has issues deducing the right template. */
      symmetry_transforms_inv.append(math::invert<float, 4>(brush_transform));
    }
    curve_selection.foreach_index([&](const int64_t curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      const float curve_selection_factor = curve_selection_factors[curve_i];
      for (const float4x4 &brush_transform_inv : symmetry_transforms_inv) {
        for (const int segment_i : points.drop_back(1)) {
          const float2 p1_re = ED_view3d_project_float_v2_m4(
              ctx.region,
              math::transform_point(brush_transform_inv, deformation.positions[segment_i]),
              projection);
          const float2 p2_re = ED_view3d_project_float_v2_m4(
              ctx.region,
              math::transform_point(brush_transform_inv, deformation.positions[segment_i + 1]),
              projection);
          /* Tested against the cursor point: the stroke tests segment-to-segment. */
          const float dist_sq_re = dist_squared_to_line_segment_v2(ctx.mouse_re, p1_re, p2_re);
          if (dist_sq_re > radius_sq_re) {
            continue;
          }
          const float weight = grow_shrink_curve_zone_weight(
              BKE_brush_curve_strength(ctx.brush, std::sqrt(dist_sq_re), ctx.radius_re),
              curve_selection_factor);
          math::max_inplace(r_zone[segment_i], weight);
          math::max_inplace(r_zone[segment_i + 1], weight);
        }
      }
    });
    return;
  }

  const std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx.depsgraph,
                                                                       *ctx.region,
                                                                       *ctx.v3d,
                                                                       *ctx.rv3d,
                                                                       *ctx.object,
                                                                       ctx.mouse_re,
                                                                       ctx.radius_base_re);
  if (!brush_3d.has_value()) {
    return;
  }
  const float radius_cu = brush_3d->radius_cu * ctx.radius_factor;
  const float radius_sq_cu = pow2f(radius_cu);
  for (const float4x4 &brush_transform : symmetry_transforms) {
    const float3 brush_pos_cu = math::transform_point(brush_transform, brush_3d->position_cu);
    curve_selection.foreach_index([&](const int64_t curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      const float curve_selection_factor = curve_selection_factors[curve_i];
      for (const int segment_i : points.drop_back(1)) {
        const float dist_sq_cu = dist_squared_to_line_segment_v3(
            brush_pos_cu, deformation.positions[segment_i], deformation.positions[segment_i + 1]);
        if (dist_sq_cu > radius_sq_cu) {
          continue;
        }
        const float weight = grow_shrink_curve_zone_weight(
            BKE_brush_curve_strength(ctx.brush, std::sqrt(dist_sq_cu), radius_cu),
            curve_selection_factor);
        math::max_inplace(r_zone[segment_i], weight);
        math::max_inplace(r_zone[segment_i + 1], weight);
      }
    });
  }
}

bool curves_sculpt_hover_preview_compute(const Depsgraph &depsgraph,
                                         const ARegion &region,
                                         const View3D &v3d,
                                         const RegionView3D &rv3d,
                                         const Object &curves_ob_orig,
                                         Brush &brush,
                                         float radius_base_re,
                                         float radius_re,
                                         const float2 &mouse_re,
                                         CurvesHoverPreview &r_preview)
{
  r_preview.hit_curves.clear();
  r_preview.zone_weights = Array<float>();

  switch (eBrushCurvesSculptType(brush.curves_sculpt_brush_type)) {
    case CURVES_SCULPT_BRUSH_TYPE_COMB:
    case CURVES_SCULPT_BRUSH_TYPE_SNAKE_HOOK:
    case CURVES_SCULPT_BRUSH_TYPE_GROW_SHRINK:
    case CURVES_SCULPT_BRUSH_TYPE_PINCH:
    case CURVES_SCULPT_BRUSH_TYPE_SMOOTH:
    case CURVES_SCULPT_BRUSH_TYPE_PUFF:
    case CURVES_SCULPT_BRUSH_TYPE_SLIDE:
      break;
    default:
      return false;
  }

  /* The stroke gets this from the #PaintStroke constructor; the hover has no stroke, so a brush
   * with custom pressure/falloff curves would evaluate an uninitialized table (see the assert
   * in #BKE_curvemap_evaluateF). The init only refreshes the evaluation cache: no notifiers,
   * no undo, same as the stroke. Safe to do from the cursor draw callback: this function runs
   * only on a #HoverDrawCache miss (see #hover_draw_cache_preview), so it is not per-redraw, and
   * it mutates no geometry or depsgraph state -- only the brush's private curve cache. */
  bke::brush::common_pressure_curves_init(brush);

  if (curves_ob_orig.type != OB_CURVES) {
    return false;
  }
  const Curves *curves_id = id_cast<const Curves *>(curves_ob_orig.data);
  if (curves_id == nullptr) {
    return false;
  }
  const CurvesGeometry &curves = curves_id->geometry.wrap();
  if (curves.is_empty() || curves.points_num() > hover_preview_max_points) {
    return false;
  }

  HoverContext ctx;
  ctx.depsgraph = &depsgraph;
  ctx.region = &region;
  ctx.v3d = &v3d;
  ctx.rv3d = &rv3d;
  ctx.object = &curves_ob_orig;
  ctx.curves_id = curves_id;
  ctx.curves = &curves;
  ctx.brush = &brush;
  ctx.radius_factor = hover_brush_radius_factor(brush);
  ctx.radius_base_re = radius_base_re;
  ctx.radius_re = radius_re;
  ctx.mouse_re = mouse_re;

  Array<float> zone(curves.points_num(), 0.0f);
  IndexMaskMemory selection_memory;
  const IndexMask curve_selection = curves::retrieve_selected_curves(*curves_id, selection_memory);

  switch (eBrushCurvesSculptType(brush.curves_sculpt_brush_type)) {
    case CURVES_SCULPT_BRUSH_TYPE_COMB: {
      const VArray<float> point_factors = *curves.attributes().lookup_or_default<float>(
          ".selection", bke::AttrDomain::Point, 1.0f);
      hover_preview_comb(ctx, point_factors, curve_selection, zone);
      break;
    }
    case CURVES_SCULPT_BRUSH_TYPE_SNAKE_HOOK: {
      const VArray<float> curve_factors = *curves.attributes().lookup_or_default<float>(
          ".selection", bke::AttrDomain::Curve, 1.0f);
      hover_preview_snake_hook(ctx, curve_factors, curve_selection, zone);
      break;
    }
    case CURVES_SCULPT_BRUSH_TYPE_PINCH: {
      const VArray<float> point_factors = *curves.attributes().lookup_or_default<float>(
          ".selection", bke::AttrDomain::Point, 1.0f);
      hover_preview_pinch(ctx, point_factors, curve_selection, zone);
      break;
    }
    case CURVES_SCULPT_BRUSH_TYPE_SMOOTH: {
      const VArray<float> point_factors = *curves.attributes().lookup_or_default<float>(
          ".selection", bke::AttrDomain::Point, 1.0f);
      hover_preview_smooth(ctx, point_factors, curve_selection, zone);
      break;
    }
    case CURVES_SCULPT_BRUSH_TYPE_PUFF: {
      hover_preview_puff(ctx, curve_selection, zone);
      break;
    }
    case CURVES_SCULPT_BRUSH_TYPE_SLIDE: {
      const VArray<float> curve_factors = *curves.attributes().lookup_or_default<float>(
          ".selection", bke::AttrDomain::Curve, 1.0f);
      hover_preview_slide(ctx, curve_factors, curve_selection, zone);
      break;
    }
    case CURVES_SCULPT_BRUSH_TYPE_GROW_SHRINK: {
      const VArray<float> curve_selection_factors = *curves.attributes().lookup_or_default<float>(
          ".selection", bke::AttrDomain::Curve, 1.0f);
      hover_preview_grow_shrink(ctx, curve_selection_factors, curve_selection, zone);
      break;
    }
    default:
      return false;
  }

  hover_preview_compact(zone, curves.points_by_curve(), r_preview);
  return !r_preview.hit_curves.is_empty();
}

/* -------------------------------------------------------------------- */
/** \name Cursor Overlay Drawing
 * \{ */

/**
 * Cached hover computation for the cursor overlay. Weights are recomputed only when the key
 * below changes; positions are re-read from the (cheap) deformation query on every draw.
 */
struct HoverDrawCache {
  bool valid = false;
  const Object *object = nullptr;
  const Brush *brush = nullptr;
  int falloff_shape = 0;
  int symmetry = 0;
  int points_num = 0;
  const float3 *positions_ptr = nullptr;
  float2 mouse_re = float2(0.0f);
  float radius_re = 0.0f;
  float4x4 obmat = float4x4::identity();
  float4x4 viewmat = float4x4::identity();
  CurvesHoverPreview preview;
};

static Vector<HoverDrawCache> &hover_draw_caches()
{
  static Vector<HoverDrawCache> caches;
  return caches;
}

static bool hover_brush_is_deform_preview(const Brush &brush)
{
  switch (eBrushCurvesSculptType(brush.curves_sculpt_brush_type)) {
    case CURVES_SCULPT_BRUSH_TYPE_COMB:
    case CURVES_SCULPT_BRUSH_TYPE_SNAKE_HOOK:
    case CURVES_SCULPT_BRUSH_TYPE_GROW_SHRINK:
    case CURVES_SCULPT_BRUSH_TYPE_PINCH:
    case CURVES_SCULPT_BRUSH_TYPE_SMOOTH:
    case CURVES_SCULPT_BRUSH_TYPE_PUFF:
    case CURVES_SCULPT_BRUSH_TYPE_SLIDE:
      return true;
    default:
      return false;
  }
}

static CurvesHoverPreview *hover_draw_cache_preview(const Depsgraph &depsgraph,
                                                     const ARegion &region,
                                                     const View3D &v3d,
                                                     const RegionView3D &rv3d,
                                                     const Object &object,
                                                     const Curves &curves_id,
                                                     Brush &brush,
                                                     float radius_base_re,
                                                     float radius_re,
                                                     const float2 mouse_re,
                                                     bool &r_recomputed)
{
  const CurvesGeometry &curves = curves_id.geometry.wrap();
  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(depsgraph, object);
  const float4x4 obmat = object.object_to_world();
  const float4x4 viewmat(rv3d.viewmat);

  Vector<HoverDrawCache> &caches = hover_draw_caches();
  HoverDrawCache *entry = nullptr;
  for (HoverDrawCache &candidate : caches) {
    if (candidate.valid && candidate.object == &object) {
      entry = &candidate;
      break;
    }
  }
  if (entry != nullptr && entry->brush == &brush &&
      entry->falloff_shape == brush.falloff_shape && entry->symmetry == curves_id.symmetry &&
      entry->points_num == curves.points_num() &&
      entry->positions_ptr == deformation.positions.data() && entry->mouse_re == mouse_re &&
      entry->radius_re == radius_re && entry->obmat == obmat && entry->viewmat == viewmat)
  {
    r_recomputed = false;
    return entry->preview.hit_curves.is_empty() ? nullptr : &entry->preview;
  }

  r_recomputed = true;

  if (entry == nullptr) {
    /* Hover targets are few (usually one); drop everything rather than tracking ages. */
    if (caches.size() >= 4) {
      caches.clear();
    }
    caches.append_as();
    entry = &caches.last();
  }
  /* Fill the key before computing so even an empty result stays cached as valid: a brush
   * over empty space must not re-run the full traversal on every redraw. */
  entry->valid = true;
  entry->object = &object;
  entry->brush = &brush;
  entry->falloff_shape = brush.falloff_shape;
  entry->symmetry = curves_id.symmetry;
  entry->points_num = curves.points_num();
  entry->positions_ptr = deformation.positions.data();
  entry->mouse_re = mouse_re;
  entry->radius_re = radius_re;
  entry->obmat = obmat;
  entry->viewmat = viewmat;
  if (!curves_sculpt_hover_preview_compute(depsgraph,
                                           region,
                                           v3d,
                                           rv3d,
                                           object,
                                           brush,
                                           radius_base_re,
                                           radius_re,
                                           mouse_re,
                                           entry->preview))
  {
    return nullptr;
  }
  return entry->preview.hit_curves.is_empty() ? nullptr : &entry->preview;
}

void curves_sculpt_hover_preview_cache_clear()
{
  hover_draw_caches().clear();
}

void curves_sculpt_hover_preview_draw(PaintCursorContext &pcontext)
{
  /* The hover weights are published into the same registry the stroke uses and drawn by the
   * overlay draw engine (#overlay_sculpt.hh). Publishing here, in the paint-cursor callback,
   * means the highlight lags the cursor by one frame: the draw engine of the current frame has
   * already synced before this runs. That is acceptable -- the cursor moves continuously and the
   * region is tagged redrawn -- and it is what lets hover and stroke share one pass (and one
   * linear/AA pipeline) instead of being immediate-drawn on top of the finished frame. */
  auto deactivate = [&]() {
    if (curves_sculpt_hover_viz_clear() && pcontext.region != nullptr) {
      ED_region_tag_redraw(pcontext.region);
    }
  };

  if (pcontext.mode != PaintMode::SculptCurves) {
    return;
  }
  /* During a stroke the stroke record owns the visualization. */
  if (pcontext.is_stroke_active) {
    deactivate();
    return;
  }
  const View3D *v3d = pcontext.vc.v3d;
  if (v3d == nullptr) {
    deactivate();
    return;
  }
  const View3DOverlay &overlay = v3d->overlay;
  if ((v3d->flag2 & V3D_HIDE_OVERLAYS) != 0) {
    deactivate();
    return;
  }
  /* Master toggle of the whole Sculpt Curves display options group. */
  if ((overlay.flag & V3D_OVERLAY_SCULPT_CURVES_HIDE_DISPLAY_OPTIONS) != 0) {
    deactivate();
    return;
  }
  /* The hover shows only the flat hit color ("Curves Hover Hit"); the gradient is drawn during
   * the stroke by the brush influence. */
  if ((overlay.flag & V3D_OVERLAY_SCULPT_CURVES_HIDE_HOVER_CURVES) != 0) {
    deactivate();
    return;
  }
  if (pcontext.brush == nullptr || !hover_brush_is_deform_preview(*pcontext.brush)) {
    deactivate();
    return;
  }
  if (pcontext.region == nullptr || pcontext.depsgraph == nullptr || pcontext.scene == nullptr) {
    deactivate();
    return;
  }
  const CurvesSculpt *curves_sculpt = pcontext.vc.scene != nullptr ?
                                          pcontext.vc.scene->toolsettings->curves_sculpt :
                                          nullptr;
  if (curves_sculpt == nullptr) {
    deactivate();
    return;
  }

  /* Same target scope as the stroke (see #CurvesMultiObjectStrokeContext). */
  CurvesMultiObjectStrokeContext scope;
  scope.resolve(pcontext.vc, curves_sculpt);
  if (scope.deform_targets.is_empty()) {
    deactivate();
    return;
  }

  const float2 mouse_re = {float(pcontext.mval.x - pcontext.region->winrct.xmin),
                           float(pcontext.mval.y - pcontext.region->winrct.ymin)};
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(pcontext.region->regiondata);
  if (rv3d == nullptr) {
    deactivate();
    return;
  }

  const float radius_base_re = BKE_brush_radius_get(pcontext.paint, pcontext.brush);
  const float radius_re = radius_base_re * hover_brush_radius_factor(*pcontext.brush);

  bool recomputed_any = false;
  for (const CurvesSculptTarget &target : scope.deform_targets) {
    if (target.object == nullptr || target.object->type != OB_CURVES) {
      continue;
    }
    Curves *curves_id = target.curves_id;
    if (curves_id == nullptr) {
      continue;
    }
    bool recomputed = false;
    CurvesHoverPreview *preview = hover_draw_cache_preview(*pcontext.depsgraph,
                                                            *pcontext.region,
                                                            *pcontext.vc.v3d,
                                                            *rv3d,
                                                            *target.object,
                                                            *curves_id,
                                                            *pcontext.brush,
                                                            radius_base_re,
                                                            radius_re,
                                                            mouse_re,
                                                            recomputed);
    if (!recomputed) {
      /* The cache hit means neither the cursor nor the geometry changed: the registry already
       * holds this result, so nothing to publish and nothing to redraw. */
      continue;
    }
    /* Publish only on a cache miss: the weights then change at most once per cursor move. */
    if (preview == nullptr) {
      curves_sculpt_hover_viz_publish(*curves_id, Array<float>());
    }
    else {
      curves_sculpt_hover_viz_publish(*curves_id, preview->zone_weights);
    }
    recomputed_any = true;
  }
  if (recomputed_any) {
    ED_region_tag_redraw(pcontext.region);
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint
