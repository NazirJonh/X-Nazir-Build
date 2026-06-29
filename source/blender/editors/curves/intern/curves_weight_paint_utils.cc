/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Weight paint specific utilities and base class implementation for Curves Weight Paint operations.
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

#include "ED_object_vgroup.hh"
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
/** \name CurvesWeightPaintOperationBase - Weight Paint Specific
 * \{ */

void CurvesWeightPaintOperationBase::get_brush_settings(const bContext &C,
                                                         const StrokeExtension &stroke_extension)
{
  /* Resolve object/curves/brush and the common brush parameters (radius, strength, falloff). */
  CurvesPaintOperationBase::get_brush_settings(C, stroke_extension);

  if (!object || !curves_id || !curves || !brush) {
    return;
  }

  const Paint *paint = BKE_paint_get_active_from_context(&C);
  if (paint == nullptr) {
    return;
  }

  /* Weight paint specific settings. */
  brush_weight = BKE_brush_weight_get(paint, brush);

  /* Auto-normalize is only meaningful when more than one vertex group exists; with a single
   * group every painted weight would be normalized straight back to 1.0, making the brush
   * appear to do nothing. */
  const ToolSettings *ts = CTX_data_tool_settings(&C);
  auto_normalize = ts->auto_normalize && (BKE_object_defgroup_count(object) > 1);

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
/** \name CurvesWeightPaintOperationBase - Paint Operation Override
 * \{ */

void CurvesWeightPaintOperationBase::apply_operation_to_point(const CurvesBrushPoint &point)
{
  /* Default weight paint behavior: apply brush weight to points. */
  apply_weight_to_point(point.point_index, brush_weight, point.influence);
}

void CurvesWeightPaintOperationBase::init_paint_mode(const bContext & /*C*/)
{
  /* This will be called by the base class on_stroke_begin. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesWeightPaintOperationBase - Stroke Callbacks
 * \{ */

void CurvesWeightPaintOperationBase::on_stroke_begin(const bContext &C,
                                                      const StrokeExtension &start_extension)
{
  /* Call base class implementation. */
  CurvesPaintOperationBase::on_stroke_begin(C, start_extension);

  if (!object || !curves_id || !curves || !brush) {
    return;
  }

  /* Ensure vertex group infrastructure is set up. */
  ensure_active_vertex_group_in_object();
  get_locked_vertex_groups();

  /* Ensure deform verts exist in the curves geometry. */
  bke::curves::ensure_deform_verts(object);
}

void CurvesWeightPaintOperationBase::on_stroke_done(const bContext &C)
{
  /* Call base class implementation. */
  CurvesPaintOperationBase::on_stroke_done(C);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
