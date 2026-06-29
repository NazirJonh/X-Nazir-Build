/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Utility functions and base class implementation for Curves Paint operations.
 * Common functionality for Weight Paint, Vertex Paint, and other paint modes.
 */

#include "curves_paint_intern.hh"

#include "DNA_object_types.h"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_ID.h"

#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name CurvesPaintOperationBase - Brush Settings
 * \{ */

void CurvesPaintOperationBase::get_brush_settings(const bContext &C,
                                                   const StrokeExtension &stroke_extension)
{
  Object *object_from_context = CTX_data_active_object(&C);
  if (object_from_context != nullptr) {
    ID *original_id = DEG_get_original_id(&object_from_context->id);
    if (original_id != nullptr && original_id != &object_from_context->id) {
      object = reinterpret_cast<Object *>(original_id);
    }
    else {
      object = object_from_context;
    }
  }
  else {
    object = nullptr;
  }

  if (!object || object->type != OB_CURVES || object->data == nullptr) {
    curves_id = nullptr;
    curves = nullptr;
    brush = nullptr;
    return;
  }

  ID *object_data_id = static_cast<ID *>(object->data);
  if (GS(object_data_id->name) != ID_CV) {
    curves_id = nullptr;
    curves = nullptr;
    brush = nullptr;
    return;
  }

  curves_id = reinterpret_cast<Curves *>(object->data);
  curves = &curves_id->geometry.wrap();

  Paint *paint = BKE_paint_get_active_from_context(&C);
  if (paint == nullptr) {
    brush = nullptr;
    return;
  }
  brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    return;
  }

  /* Get initial brush parameters. */
  initial_brush_radius = BKE_brush_radius_get(paint, brush);
  initial_brush_strength = BKE_brush_alpha_get(paint, brush);

  /* Store previous mouse position before updating. */
  mouse_position_previous = mouse_position;
  mouse_position = stroke_extension.mouse_position;

  /* Update brush radius based on pressure. */
  brush_radius = initial_brush_radius;
  if (BKE_brush_use_size_pressure(brush)) {
    brush_radius *= stroke_extension.pressure;
  }

  /* Update brush strength based on pressure. */
  brush_strength = initial_brush_strength;
  if (BKE_brush_use_alpha_pressure(brush)) {
    brush_strength *= stroke_extension.pressure;
  }

  /* Initialize falloff curve. */
  BKE_curvemapping_init(brush->curve_distance_falloff);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesPaintOperationBase - Mouse Input
 * \{ */

void CurvesPaintOperationBase::get_mouse_input(const StrokeExtension &stroke_extension,
                                                float brush_widen_factor)
{
  mouse_position = stroke_extension.mouse_position;

  /* Calculate effective brush radius. */
  float effective_radius = brush_radius * brush_widen_factor;

  /* Update brush bounding box for quick rejection tests. */
  BLI_rctf_init(&brush_bbox,
                mouse_position.x - effective_radius,
                mouse_position.x + effective_radius,
                mouse_position.y - effective_radius,
                mouse_position.y + effective_radius);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesPaintOperationBase - Curve Neighbor Traversal
 * \{ */

void CurvesPaintOperationBase::foreach_curve_neighbors(const int point_index,
                                                       const OffsetIndices<int> &points_by_curve,
                                                       const Span<int> point_to_curve,
                                                       const VArray<bool> &cyclic,
                                                       const FunctionRef<void(int)> fn)
{
  const int curve_index = point_to_curve[point_index];
  const IndexRange curve_points = points_by_curve[curve_index];
  if (curve_points.size() <= 1) {
    return;
  }
  const int64_t local = int64_t(point_index) - curve_points.first();
  const bool is_cyclic = cyclic[curve_index];

  if (local > 0) {
    fn(int(curve_points[local - 1]));
  }
  else if (is_cyclic) {
    fn(int(curve_points.last()));
  }

  if (local + 1 < curve_points.size()) {
    fn(int(curve_points[local + 1]));
  }
  else if (is_cyclic) {
    fn(int(curve_points.first()));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesPaintOperationBase - Point-in-Brush Tests
 * \{ */

bool CurvesPaintOperationBase::is_point_in_brush(const float2 &point_position_re) const
{
  /* Quick bounding box rejection. */
  if (!BLI_rctf_isect_pt_v(&brush_bbox, point_position_re)) {
    return false;
  }

  /* Precise circle test. */
  const float dist_sq = math::distance_squared(point_position_re, mouse_position);
  return dist_sq <= (brush_radius * brush_radius);
}

float CurvesPaintOperationBase::calculate_brush_falloff(float distance_re) const
{
  if (distance_re >= brush_radius) {
    return 0.0f;
  }

  /* Use brush falloff curve for smooth falloff. */
  return BKE_brush_curve_strength(brush, distance_re, brush_radius);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesPaintOperationBase - Brush Sampling
 * \{ */

void CurvesPaintOperationBase::sample_curves_3d_brush(const bContext &C,
                                                       const StrokeExtension & /*stroke_extension*/)
{
  /* Clear previous points. */
  points_in_brush.clear();

  if (!curves || curves->is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(&C);
  if (!region) {
    return;
  }

  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  const Object *eval_object = (depsgraph != nullptr && object != nullptr) ?
                                  DEG_get_evaluated(depsgraph, object) :
                                  nullptr;
  const float4x4 object_to_world = (eval_object != nullptr) ? eval_object->object_to_world() :
                                                               object->object_to_world();

  bke::crazyspace::GeometryDeformation deformation;
  Span<float3> positions = curves->positions();
  if (depsgraph != nullptr && object != nullptr) {
    deformation = bke::crazyspace::get_evaluated_curves_deformation(*depsgraph, *object);
    if (!deformation.positions.is_empty()) {
      positions = deformation.positions;
    }
  }

  const int points_num = curves->points_num();
  const float brush_radius_sq = brush_radius * brush_radius;

  /* Collect points within brush radius. */
  for (const int point_i : IndexRange(points_num)) {
    /* Project object-space point to screen space in a context-independent way. */
    const float3 point_pos_wo = math::transform_point(object_to_world, positions[point_i]);
    float2 point_pos_re;
    if (ED_view3d_project_float_global(region, point_pos_wo, point_pos_re, V3D_PROJ_TEST_NOP) !=
        V3D_PROJ_RET_OK)
    {
      continue;
    }

    /* Quick bounding box rejection. */
    if (!BLI_rctf_isect_pt_v(&brush_bbox, point_pos_re)) {
      continue;
    }

    /* Check if point is within brush radius. */
    const float distance_sq_re = math::distance_squared(mouse_position, point_pos_re);
    if (distance_sq_re > brush_radius_sq) {
      continue;
    }

    /* Calculate falloff influence. */
    const float distance_re = math::sqrt(distance_sq_re);
    const float falloff = calculate_brush_falloff(distance_re);
    const float influence = brush_strength * falloff;

    if (influence > 0.0f) {
      points_in_brush.append({influence, point_i});
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesPaintOperationBase - Stroke Callbacks
 * \{ */

void CurvesPaintOperationBase::on_stroke_begin(const bContext &C,
                                                const StrokeExtension &start_extension)
{
  /* Get brush settings from context. */
  get_brush_settings(C, start_extension);

  if (!object || !curves_id || !curves || !brush) {
    return;
  }

  /* Initialize mouse input. */
  get_mouse_input(start_extension);

  /* Allow subclasses to initialize their specific paint mode. */
  init_paint_mode(C);
}

void CurvesPaintOperationBase::apply_brush(const bContext & /*C*/,
                                           const StrokeExtension & /*stroke_extension*/)
{
  /* Default behavior: apply the operation independently to every point under the brush. */
  for (const CurvesBrushPoint &point : points_in_brush) {
    apply_operation_to_point(point);
  }
}

void CurvesPaintOperationBase::on_stroke_extended(const bContext &C,
                                                   const StrokeExtension &stroke_extension)
{
  /* Update brush settings for this stroke sample. */
  get_brush_settings(C, stroke_extension);

  if (!object || !curves_id || !curves || !brush) {
    return;
  }

  /* Update mouse input and bounding box (brushes may widen the sampling area). */
  get_mouse_input(stroke_extension, brush_widen_factor());

  if (!curves || curves->is_empty()) {
    return;
  }

  /* Sample points under the brush. */
  sample_curves_3d_brush(C, stroke_extension);

  /* Apply the brush to the collected points. */
  apply_brush(C, stroke_extension);

  /* Notify about geometry changes. */
  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
}

void CurvesPaintOperationBase::on_stroke_done(const bContext &C)
{
  /* Allow subclasses to finalize their specific paint mode. */
  finalize_paint_mode(C);

  /* Clear collected points. */
  points_in_brush.clear();

  /* Reset state. */
  object = nullptr;
  curves_id = nullptr;
  curves = nullptr;
  brush = nullptr;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
