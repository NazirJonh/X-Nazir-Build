/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Utility functions and base class implementation for Curves Weight Paint operations.
 */

#include "curves_weight_paint_intern.hh"

#include "DNA_object_types.h"
#include "DNA_object_enums.h"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_ID.h"

#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_curves_weight_paint.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_deform.hh"
#include "BKE_object_deform.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Poll Functions
 * \{ */

bool curves_weight_paint_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_CURVES) {
    return false;
  }
  return ob->mode == OB_MODE_WEIGHT_CURVES;
}

bool curves_weight_paint_poll_view3d(bContext *C)
{
  return curves_weight_paint_poll(C);
}

bool curves_weight_paint_mode_poll(bContext *C)
{
  return curves_weight_paint_poll(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesWeightPaintCommonContext
 * \{ */

CurvesWeightPaintCommonContext::CurvesWeightPaintCommonContext(const bContext &C)
{
  scene = CTX_data_scene(&C);
  object = CTX_data_active_object(&C);

  if (object && object->type == OB_CURVES) {
    Curves *curves_id = id_cast<Curves *>(object->data);
    curves = &curves_id->geometry.wrap();
  }

  depsgraph = CTX_data_depsgraph_pointer(&C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesWeightPaintOperationBase - Brush Settings
 * \{ */

void CurvesWeightPaintOperationBase::get_brush_settings(const bContext &C,
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
    printf("[DEBUG] CurvesWeightPaint get_brush_settings: invalid active object for stroke "
           "(object=%s type=%d mode=%d data=%p)\n",
           object ? object->id.name + 2 : "NULL",
           object ? object->type : -1,
           object ? object->mode : -1,
           object ? object->data : nullptr);
    curves_id = nullptr;
    curves = nullptr;
    brush = nullptr;
    return;
  }

  ID *object_data_id = static_cast<ID *>(object->data);
  if (GS(object_data_id->name) != ID_CV) {
    printf("[DEBUG] CurvesWeightPaint get_brush_settings: object data type mismatch "
           "(object=%s type=%d mode=%d data_id=%c%c expected=CV)\n",
           object->id.name + 2,
           object->type,
           object->mode,
           object_data_id->name[0],
           object_data_id->name[1]);
    curves_id = nullptr;
    curves = nullptr;
    brush = nullptr;
    return;
  }

  curves_id = reinterpret_cast<Curves *>(object->data);
  curves = &curves_id->geometry.wrap();

  Paint *paint = BKE_paint_get_active_from_context(&C);
  if (paint == nullptr) {
    printf("[DEBUG] CurvesWeightPaint get_brush_settings: active paint is null\n");
    brush = nullptr;
    return;
  }
  brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    printf("[DEBUG] CurvesWeightPaint get_brush_settings: active brush is null\n");
    return;
  }

  /* Get initial brush parameters. */
  initial_brush_radius = BKE_brush_radius_get(paint, brush);
  initial_brush_strength = BKE_brush_alpha_get(paint, brush);
  brush_weight = BKE_brush_weight_get(paint, brush);

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

  /* Get auto-normalize setting. */
  const ToolSettings *ts = CTX_data_tool_settings(&C);
  auto_normalize = ts->auto_normalize;

  /* Get brush add/subtract mode. */
  invert_brush_weight = (brush->flag & BRUSH_DIR_IN) != 0;
  if (stroke_mode == BrushStrokeMode::Invert) {
    invert_brush_weight = !invert_brush_weight;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesWeightPaintOperationBase - Vertex Groups
 * \{ */

void CurvesWeightPaintOperationBase::ensure_active_vertex_group_in_object()
{
  const ListBaseT<bDeformGroup> *defbase = BKE_object_defgroup_list(object);

  int object_defgroup_nr = BKE_object_defgroup_active_index_get(object) - 1;

  if (object_defgroup_nr == -1) {
    const ListBase *defbase = BKE_object_defgroup_list(object);

    if (BLI_listbase_is_empty(defbase)) {
      /* No vertex groups exist, create a default one. */
      BKE_object_defgroup_add(object);
      object_defgroup_nr = 0;
    }
  }

  if (object_defgroup_nr < 0) {
    /* If groups exist but no active group is set, default to the first one. */
    object_defgroup_nr = 0;
    BKE_object_defgroup_active_index_set(object, 1);
  }

  object_defgroup = static_cast<bDeformGroup *>(BLI_findlink(defbase, object_defgroup_nr));
  active_vertex_group = object_defgroup_nr;
}

void CurvesWeightPaintOperationBase::get_locked_vertex_groups()
{
  object_locked_defgroups.clear();

  const ListBaseT<bDeformGroup> &defgroups = *BKE_object_defgroup_list(object);
  for (const bDeformGroup &dg : defgroups) {
    if ((dg.flag & DG_LOCK_WEIGHT) != 0) {
      object_locked_defgroups.add(std::string(dg.name));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesWeightPaintOperationBase - Weight Access
 * \{ */

float CurvesWeightPaintOperationBase::get_vertex_weight(int point_index)
{
  return bke::curves::get_vertex_group_weight(object, point_index, active_vertex_group);
}

void CurvesWeightPaintOperationBase::set_vertex_weight(int point_index, float weight)
{
  /* Use WEIGHT_REPLACE mode (value = 1) */
  bke::curves::set_vertex_group_weight(object, point_index, active_vertex_group, weight, 1);

  if (auto_normalize) {
    bke::curves::normalize_point_weights(
        object, point_index, false /* lock_active */, auto_normalize);
  }
}

void CurvesWeightPaintOperationBase::apply_weight_to_point(int point_index,
                                                           float target_weight,
                                                           float influence)
{
  const float old_weight = get_vertex_weight(point_index);

  /* Calculate weight delta based on invert mode. */
  const float effective_target = invert_brush_weight ? (1.0f - target_weight) : target_weight;
  const float weight_delta = effective_target - old_weight;

  /* Blend current weight with target weight using influence. */
  const float new_weight = math::clamp(
      old_weight + math::interpolate(0.0f, weight_delta, influence), 0.0f, 1.0f);

  set_vertex_weight(point_index, new_weight);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesWeightPaintOperationBase - Mouse Input
 * \{ */

void CurvesWeightPaintOperationBase::get_mouse_input(const StrokeExtension &stroke_extension,
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
/** \name CurvesWeightPaintOperationBase - Brush Sampling
 * \{ */

bool CurvesWeightPaintOperationBase::is_point_in_brush(const float2 &point_position_re)
{
  /* Quick bounding box rejection. */
  if (!BLI_rctf_isect_pt_v(&brush_bbox, point_position_re)) {
    return false;
  }

  /* Precise circle test. */
  const float dist_sq = math::distance_squared(point_position_re, mouse_position);
  return dist_sq <= (brush_radius * brush_radius);
}

float CurvesWeightPaintOperationBase::calculate_brush_falloff(float distance_re)
{
  if (distance_re >= brush_radius) {
    return 0.0f;
  }

  /* Use brush falloff curve for smooth falloff. */
  return BKE_brush_curve_strength(brush, distance_re, brush_radius);
}

void CurvesWeightPaintOperationBase::sample_curves_3d_brush(
    const bContext &C, const StrokeExtension & /*stroke_extension*/)
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
/** \name CurvesWeightPaintOperationBase - Stroke Callbacks
 * \{ */

void CurvesWeightPaintOperationBase::on_stroke_begin(const bContext &C,
                                                      const StrokeExtension &start_extension)
{
  /* Get brush settings from context. */
  get_brush_settings(C, start_extension);

  if (!object || !curves_id || !curves || !brush) {
    printf("[DEBUG] CurvesWeightPaint on_stroke_begin: aborted (invalid context)\n");
    return;
  }

  /* Ensure vertex group infrastructure is set up. */
  ensure_active_vertex_group_in_object();
  get_locked_vertex_groups();

  /* Ensure deform verts exist in the curves geometry. */
  bke::curves::ensure_deform_verts(object);

  const Object *context_object = CTX_data_active_object(&C);
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(&C);
  const Object *eval_object = (depsgraph != nullptr && object != nullptr) ?
                                  DEG_get_evaluated(depsgraph, object) :
                                  nullptr;
  const Curves *context_curves =
      (context_object != nullptr) ? id_cast<const Curves *>(context_object->data) : nullptr;
  const Curves *orig_curves = (object != nullptr) ? id_cast<const Curves *>(object->data) : nullptr;
  const Curves *eval_curves = (eval_object != nullptr) ? id_cast<const Curves *>(eval_object->data) :
                                                         nullptr;

  const int context_dverts_num = (context_curves != nullptr) ? context_curves->geometry.wrap().deform_verts().size() :
                                                               -1;
  const int orig_dverts_num = (orig_curves != nullptr) ? orig_curves->geometry.wrap().deform_verts().size() :
                                                         -1;
  const int eval_dverts_num = (eval_curves != nullptr) ? eval_curves->geometry.wrap().deform_verts().size() :
                                                         -1;

  printf("[DEBUG] CurvesWeightPaint source-of-truth: ctx_ob=%p('%s') write_ob=%p('%s') eval_ob=%p('%s') "
         "ctx_cv=%p orig_cv=%p eval_cv=%p dverts(ctx/orig/eval)=%d/%d/%d\n",
         context_object,
         context_object ? context_object->id.name + 2 : "NULL",
         object,
         object ? object->id.name + 2 : "NULL",
         eval_object,
         eval_object ? eval_object->id.name + 2 : "NULL",
         context_curves,
         orig_curves,
         eval_curves,
         context_dverts_num,
         orig_dverts_num,
         eval_dverts_num);

  printf("[DEBUG] CurvesWeightPaint on_stroke_begin: object='%s' mode=%d active_vgroup_1based=%d "
         "active_vgroup_0based=%d brush_radius=%.3f brush_strength=%.3f brush_weight=%.3f\n",
         object ? object->id.name + 2 : "NULL",
         object ? object->mode : -1,
         object ? BKE_object_defgroup_active_index_get(object) : -1,
         active_vertex_group,
         brush_radius,
         brush_strength,
         brush_weight);

  /* Initialize mouse input. */
  get_mouse_input(start_extension);
}

void CurvesWeightPaintOperationBase::on_stroke_extended(const bContext &C,
                                                         const StrokeExtension &stroke_extension)
{
  /* Update brush settings for this stroke sample. */
  get_brush_settings(C, stroke_extension);

  if (!object || !curves_id || !curves || !brush) {
    return;
  }

  /* Update mouse input and bounding box. */
  get_mouse_input(stroke_extension);

  if (!curves || curves->is_empty()) {
    return;
  }

  /* Sample points under the brush. */
  sample_curves_3d_brush(C, stroke_extension);

  /* Default implementation: apply brush weight to all points under the brush. */
  for (const BrushPoint &point : points_in_brush) {
    apply_weight_to_point(point.point_index, brush_weight, point.influence);
  }

  /* Notify about geometry changes. */
  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
}

void CurvesWeightPaintOperationBase::on_stroke_done(const bContext & /*C*/)
{
  /* Clear collected points. */
  points_in_brush.clear();

  /* Reset state. */
  object = nullptr;
  curves_id = nullptr;
  brush = nullptr;
  curves = nullptr;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
