/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 */

#include "BLI_listbase.h"
#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_curves_weight_paint.hh"
#include "BKE_brush.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"
#include "BKE_deform.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_curves.hh"
#include "ED_view3d.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_paint.hh"
#include "ED_image.hh"
#include "ED_object_vgroup.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"
#include "WM_types.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"

#include "../sculpt_paint/paint_intern.hh"
#include "curves_weight_paint_intern.hh"

namespace blender::ed::sculpt_paint {

static Object *curves_weight_paint_original_object_get(bContext *C)
{
  Object *object = CTX_data_active_object(C);
  if (object == nullptr) {
    return nullptr;
  }
  if (ID *original_id = DEG_get_original_id(&object->id)) {
    return reinterpret_cast<Object *>(original_id);
  }
  return object;
}

/* -------------------------------------------------------------------- */
/** \name Paint Mode Data Wrapper
 * \{ */

class PaintModeDataWrapper : public PaintModeData {
 private:
  std::unique_ptr<CurvesWeightPaintStrokeOperation> operation_;

 public:
  PaintModeDataWrapper(std::unique_ptr<CurvesWeightPaintStrokeOperation> operation)
      : operation_(std::move(operation))
  {
  }

  CurvesWeightPaintStrokeOperation *operation()
  {
    return operation_.get();
  }
};

/* -------------------------------------------------------------------- */
/** \name Weight Paint Mode Toggle
 * \{ */

static void curves_weight_paint_mode_enter(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  Object *ob = CTX_data_active_object(C);
  
  /* Ensure weight paint data exists */
  BKE_paint_ensure(scene->toolsettings, (Paint **)&scene->toolsettings->curves_weight_paint);
  CurvesWeightPaint *curves_weight_paint = scene->toolsettings->curves_weight_paint;
  
  /* Set object mode */
  ob->mode = OB_MODE_WEIGHT_CURVES;
  
  /* Set paint mode */
  Paint *paint = BKE_paint_get_active_from_paintmode(scene, PaintMode::WeightCurves);
  
  /* Ensure brushes exist */
  BKE_paint_brushes_ensure(CTX_data_main(C), paint);
  
  /* Start paint cursor */
  ED_paint_cursor_start(&curves_weight_paint->paint, 
                        curves_weight_paint_poll);
  paint_init_pivot(ob, scene, paint);
  
  /* Ensure deform verts exist for curves */
  bke::curves::ensure_deform_verts(ob);
  
  /* Ensure at least one vertex group exists. */
  const ListBaseT<bDeformGroup> *defbase = BKE_object_defgroup_list(ob);
  if (BLI_listbase_is_empty(defbase)) {
    bDeformGroup *defgroup = BKE_object_defgroup_add_name(ob, "Group");
    if (defgroup) {
      const int defgroup_index = 0;  /* First vertex group has index 0 */
      BKE_object_defgroup_active_index_set(ob, 1);

      /* Initialize all points with weight 1.0 for the new vertex group */
      const int total_points = bke::curves::get_curves_vertex_count(ob);
      for (int i = 0; i < total_points; i++) {
        /* Use WEIGHT_REPLACE mode (value = 1) */
        bke::curves::set_vertex_group_weight(ob, i, defgroup_index, 1.0f, 1);
      }

      DEG_relations_tag_update(CTX_data_main(C));
    }
  }
  else {
    /* Ensure there's an active vertex group */
    const int active_index = BKE_object_defgroup_active_index_get(ob);
    if (active_index == 0) {
      BKE_object_defgroup_active_index_set(ob, 1);
    }
  }
  
  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
}

static void curves_weight_paint_mode_exit(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  
  /* Set object mode back to object */
  ob->mode = OB_MODE_OBJECT;
  
  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
}

static bool curves_weight_paint_toggle_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->type == OB_CURVES && ob->data;
}

static wmOperatorStatus curves_weight_paint_toggle_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  
  const bool is_mode_set = ob->mode == OB_MODE_WEIGHT_CURVES;
  
  if (!is_mode_set) {
    if (!blender::ed::object::mode_compat_set(C, ob, OB_MODE_WEIGHT_CURVES, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }
  
  if (is_mode_set) {
    curves_weight_paint_mode_exit(C);
  }
  else {
    curves_weight_paint_mode_enter(C);
  }
  
  WM_toolsystem_update_from_context_view3d(C);
  
  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  
  return OPERATOR_FINISHED;
}

void CURVES_OT_weight_paint_toggle(wmOperatorType *ot)
{
  ot->name = "Curves Weight Paint Mode";
  ot->idname = "CURVES_OT_weight_paint_toggle";
  ot->description = "Toggle curves weight paint mode in 3D view";

  ot->exec = curves_weight_paint_toggle_exec;
  ot->poll = curves_weight_paint_toggle_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Add Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  Curves *curves = id_cast<Curves *>(ob->data);
  if (curves == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const ListBaseT<bDeformGroup> *defbase = BKE_object_defgroup_list(ob);
  const int vgroup_count_before = BLI_listbase_count(defbase);
  printf("[DEBUG] CURVES_OT_vertex_group_add: before add, groups=%d\n", vgroup_count_before);
  
  /* Ensure we have deform verts */
  bke::curves::ensure_deform_verts(ob);
  
  /* Add vertex group to object */
  bDeformGroup *defgroup = BKE_object_defgroup_add(ob);
  if (!defgroup) {
    printf("[ERROR] CURVES_OT_vertex_group_add: BKE_object_defgroup_add returned nullptr\n");
    BKE_report(op->reports, RPT_ERROR, "Could not add vertex group");
    return OPERATOR_CANCELLED;
  }

  /* Set as active group */
  const int defgroup_index = BKE_object_defgroup_count(ob) - 1;
  BKE_object_defgroup_active_index_set(ob, defgroup_index + 1);

  printf("[DEBUG] CURVES_OT_vertex_group_add: added='%s', groups=%d, active_index=%d\n",
         defgroup->name,
         BKE_object_defgroup_count(ob),
         BKE_object_defgroup_active_index_get(ob));
  
  /* Initialize all points with weight 1.0 for the new vertex group */
  const int total_points = bke::curves::get_curves_vertex_count(ob);
  for (int i = 0; i < total_points; i++) {
    /* Use WEIGHT_REPLACE mode (value = 1) */
    bke::curves::set_vertex_group_weight(ob, i, defgroup_index, 1.0f, 1);
  }

  /* Update dependency graph. Vertex groups are stored on the object, so notify object geometry
   * and vertex-group channels explicitly (same pattern as OBJECT_OT_vertex_group_add). */
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  DEG_relations_tag_update(CTX_data_main(C));

  /* Send notifications */
  WM_event_add_notifier(C, NC_GEOM | ND_VERTEX_GROUP, ob->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_add(wmOperatorType *ot)
{
  ot->name = "Add Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_add";
  ot->description = "Add a new vertex group to the active curves object";

  ot->exec = curves_vertex_group_add_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Remove Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_remove_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const ListBaseT<bDeformGroup> *defbase = BKE_object_defgroup_list(ob);
  bDeformGroup *defgroup = static_cast<bDeformGroup *>(
      BLI_findlink(defbase, BKE_object_defgroup_active_index_get(ob) - 1));
  if (!defgroup) {
    BKE_report(op->reports, RPT_ERROR, "No active vertex group to remove");
    return OPERATOR_CANCELLED;
  }

  Curves *curves = id_cast<Curves *>(ob->data);
  if (curves == nullptr) {
    return OPERATOR_CANCELLED;
  }
  
  /* Remove weights from all points for this group */
  if (bke::curves::has_deform_verts(ob)) {
    const int defgroup_index = BKE_object_defgroup_active_index_get(ob) - 1;
    const int total_points = bke::curves::get_curves_vertex_count(ob);
    
    for (int i = 0; i < total_points; i++) {
      bke::curves::remove_vertex_from_group(ob, i, defgroup_index);
    }
  }

  /* Remove vertex group from object */
  BKE_object_defgroup_remove(ob, defgroup);

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);
  DEG_relations_tag_update(CTX_data_main(C));

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_remove(wmOperatorType *ot)
{
  ot->name = "Remove Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_remove";
  ot->description = "Remove the active vertex group from the curves object";

  ot->exec = curves_vertex_group_remove_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Assign Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_assign_exec(bContext *C, wmOperator *op)
{
  Object *ob = curves_weight_paint_original_object_get(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const int defgroup_index = BKE_object_defgroup_active_index_get(ob) - 1;
  if (defgroup_index < 0) {
    BKE_report(op->reports, RPT_ERROR, "No active vertex group");
    return OPERATOR_CANCELLED;
  }

  const bDeformGroup *active_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(ob), defgroup_index));
  if (active_defgroup && (active_defgroup->flag & DG_LOCK_WEIGHT) != 0) {
    BKE_report(op->reports, RPT_WARNING, "Active vertex group is locked");
    return OPERATOR_CANCELLED;
  }

  Curves *curves = id_cast<Curves *>(ob->data);
  if (curves == nullptr) {
    return OPERATOR_CANCELLED;
  }
  
  /* Ensure we have deform verts */
  bke::curves::ensure_deform_verts(ob);
  
  /* Get weight value */
  const float weight = RNA_float_get(op->ptr, "weight");

  LinearAllocator<> memory;
  const IndexMask selected_points = ed::curves::retrieve_selected_points(*curves, memory);
  if (selected_points.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "No selected points");
    return OPERATOR_CANCELLED;
  }

  /* Assign weight to selected points only (point domain and selected curves). */
  selected_points.foreach_index([&](const int i) {
    /* Use WEIGHT_REPLACE mode (value = 1) */
    bke::curves::set_vertex_group_weight(ob, i, defgroup_index, weight, 1);
  });

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_assign(wmOperatorType *ot)
{
  ot->name = "Assign Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_assign";
  ot->description = "Assign selected points to the active vertex group";

  ot->exec = curves_vertex_group_assign_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  
  RNA_def_float(ot->srna, "weight", 1.0f, 0.0f, 1.0f, "Weight", "Weight to assign", 0.0f, 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Remove From Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_remove_from_exec(bContext *C, wmOperator *op)
{
  Object *ob = curves_weight_paint_original_object_get(C);
  
  if (!ob || ob->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const int defgroup_index = BKE_object_defgroup_active_index_get(ob) - 1;
  if (defgroup_index < 0) {
    BKE_report(op->reports, RPT_ERROR, "No active vertex group");
    return OPERATOR_CANCELLED;
  }

  const bDeformGroup *active_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(ob), defgroup_index));
  if (active_defgroup && (active_defgroup->flag & DG_LOCK_WEIGHT) != 0) {
    BKE_report(op->reports, RPT_WARNING, "Active vertex group is locked");
    return OPERATOR_CANCELLED;
  }

  Curves *curves = id_cast<Curves *>(ob->data);
  if (curves == nullptr) {
    return OPERATOR_CANCELLED;
  }
  
  if (!bke::curves::has_deform_verts(ob)) {
    return OPERATOR_CANCELLED;
  }
  
  LinearAllocator<> memory;
  const IndexMask selected_points = ed::curves::retrieve_selected_points(*curves, memory);
  if (selected_points.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "No selected points");
    return OPERATOR_CANCELLED;
  }

  /* Remove only from selected points (point domain and selected curves). */
  selected_points.foreach_index(
      [&](const int i) { bke::curves::remove_vertex_from_group(ob, i, defgroup_index); });

  /* Update dependency graph */
  DEG_id_tag_update(&curves->id, ID_RECALC_GEOMETRY);

  /* Send notifications */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_remove_from(wmOperatorType *ot)
{
  ot->name = "Remove from Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_remove_from";
  ot->description = "Remove selected points from the active vertex group";

  ot->exec = curves_vertex_group_remove_from_exec;
  ot->poll = curves_weight_paint_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Weight Sample Operator
 * \{ */

static wmOperatorStatus curves_weight_sample_invoke(bContext *C,
                                                    wmOperator * /*op*/,
                                                    const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  if (vc.rv3d == nullptr) {
    return OPERATOR_CANCELLED;
  }

  if (!vc.obact || vc.obact->type != OB_CURVES || vc.obact->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Object *object_orig = reinterpret_cast<const Object *>(DEG_get_original_id(&vc.obact->id));
  if (object_orig == nullptr || object_orig->type != OB_CURVES || object_orig->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Object *object_eval = DEG_get_evaluated(vc.depsgraph, object_orig);
  if (object_eval == nullptr || object_eval->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int object_defgroup_nr = BKE_object_defgroup_active_index_get(object_orig) - 1;
  if (object_defgroup_nr < 0) {
    return OPERATOR_CANCELLED;
  }

  const Curves *curves_id = id_cast<const Curves *>(object_orig->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  if (curves.points_num() == 0) {
    return OPERATOR_CANCELLED;
  }

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(object_eval, *object_orig);

  const Span<float3> deformed_positions = deformation.positions.is_empty() ? curves.positions() :
                                                                            deformation.positions;
  const float4x4 projection = ED_view3d_ob_project_mat_get(vc.rv3d, object_eval);
  const IndexMask points_mask(curves.points_num());

  const std::optional<ed::curves::FindClosestData> closest = ed::curves::closest_elem_find_screen_space(
      vc,
      curves.points_by_curve(),
      deformed_positions,
      curves.cyclic(),
      projection,
      points_mask,
      bke::AttrDomain::Point,
      event->mval,
      {});

  if (!closest) {
    return OPERATOR_CANCELLED;
  }

  const float sampled_weight = bke::curves::get_vertex_group_weight(
      object_orig, closest->index, object_defgroup_nr);
  if (sampled_weight < 0.0f) {
    return OPERATOR_CANCELLED;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  BKE_brush_weight_set(paint, brush, sampled_weight);
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

void CURVES_OT_weight_sample(wmOperatorType *ot)
{
  ot->name = "Weight Paint Sample Weight";
  ot->idname = "CURVES_OT_weight_sample";
  ot->description =
      "Set the weight of the Draw tool to the weight of the vertex under the mouse cursor";

  ot->poll = curves_weight_paint_mode_poll;
  ot->invoke = curves_weight_sample_invoke;

  ot->flag = OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Weight Toggle Direction Operator
 * \{ */

static wmOperatorStatus curves_weight_toggle_direction_exec(bContext *C, wmOperator * /*op*/)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return OPERATOR_CANCELLED;
  }

  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  brush->flag ^= BRUSH_DIR_IN;
  BKE_brush_tag_unsaved_changes(brush);
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static bool curves_weight_toggle_direction_poll(bContext *C)
{
  if (!curves_weight_paint_mode_poll(C)) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return false;
  }

  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr) {
    return false;
  }

  return brush->weight_brush_type == WPAINT_BRUSH_TYPE_DRAW;
}

void CURVES_OT_weight_toggle_direction(wmOperatorType *ot)
{
  ot->name = "Weight Paint Toggle Direction";
  ot->idname = "CURVES_OT_weight_toggle_direction";
  ot->description = "Toggle Add/Subtract for the weight paint draw tool";

  ot->poll = curves_weight_toggle_direction_poll;
  ot->exec = curves_weight_toggle_direction_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Weight Invert Operator
 * \{ */

static wmOperatorStatus curves_weight_invert_exec(bContext *C, wmOperator *op)
{
  Object *ob = curves_weight_paint_original_object_get(C);
  if (!ob || ob->type != OB_CURVES || ob->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int active_defgroup_nr = BKE_object_defgroup_active_index_get(ob) - 1;
  if (active_defgroup_nr < 0) {
    return OPERATOR_CANCELLED;
  }

  const bDeformGroup *active_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(ob), active_defgroup_nr));
  if (active_defgroup && (active_defgroup->flag & DG_LOCK_WEIGHT) != 0) {
    BKE_report(op->reports, RPT_WARNING, "Active vertex group is locked");
    return OPERATOR_CANCELLED;
  }

  Curves *curves_id = id_cast<Curves *>(ob->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bke::curves::ensure_deform_verts(ob);
  const int total_points = bke::curves::get_curves_vertex_count(ob);

  for (const int point_i : IndexRange(total_points)) {
    const float old_weight = bke::curves::get_vertex_group_weight(ob, point_i, active_defgroup_nr);
    if (old_weight < 0.0f) {
      continue;
    }
    const float new_weight = 1.0f - old_weight;
    bke::curves::set_vertex_group_weight(ob, point_i, active_defgroup_nr, new_weight, WEIGHT_REPLACE);
  }

  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);

  return OPERATOR_FINISHED;
}

void CURVES_OT_weight_invert(wmOperatorType *ot)
{
  ot->name = "Invert Weight";
  ot->idname = "CURVES_OT_weight_invert";
  ot->description = "Invert the weight of active vertex group";

  ot->poll = curves_weight_paint_mode_poll;
  ot->exec = curves_weight_invert_exec;

  ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Smooth Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_smooth_exec(bContext *C, wmOperator *op)
{
  Object *ob = curves_weight_paint_original_object_get(C);
  if (!ob || ob->type != OB_CURVES || ob->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int active_defgroup_nr = BKE_object_defgroup_active_index_get(ob) - 1;
  if (active_defgroup_nr < 0) {
    return OPERATOR_CANCELLED;
  }

  const bDeformGroup *active_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(ob), active_defgroup_nr));
  if (active_defgroup && (active_defgroup->flag & DG_LOCK_WEIGHT) != 0) {
    BKE_report(op->reports, RPT_WARNING, "Active vertex group is locked");
    return OPERATOR_CANCELLED;
  }

  Curves *curves_id = id_cast<Curves *>(ob->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  const int total_points = curves.points_num();
  if (total_points == 0) {
    return OPERATOR_CANCELLED;
  }

  const float factor = RNA_float_get(op->ptr, "factor");
  const int repeat = RNA_int_get(op->ptr, "repeat");

  bke::curves::ensure_deform_verts(ob);

  Array<float> weights(total_points);
  for (const int point_i : IndexRange(total_points)) {
    const float weight = bke::curves::get_vertex_group_weight(ob, point_i, active_defgroup_nr);
    weights[point_i] = (weight < 0.0f) ? 0.0f : weight;
  }

  Array<float> next_weights(total_points);
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<bool> cyclic = curves.cyclic();

  for ([[maybe_unused]] const int iteration : IndexRange(repeat)) {
    next_weights.as_mutable_span().copy_from(weights.as_span());

    for (const int curve_i : curves.curves_range()) {
      const IndexRange curve_points = points_by_curve[curve_i];
      if (curve_points.size() <= 1) {
        continue;
      }

      const bool is_cyclic = cyclic[curve_i];
      for (const int i : curve_points.index_range()) {
        const int point_i = curve_points[i];
        float neighbor_sum = 0.0f;
        int neighbor_count = 0;

        if (i > 0) {
          neighbor_sum += weights[curve_points[i - 1]];
          neighbor_count++;
        }
        else if (is_cyclic) {
          neighbor_sum += weights[curve_points.last()];
          neighbor_count++;
        }

        if (i + 1 < curve_points.size()) {
          neighbor_sum += weights[curve_points[i + 1]];
          neighbor_count++;
        }
        else if (is_cyclic) {
          neighbor_sum += weights[curve_points.first()];
          neighbor_count++;
        }

        if (neighbor_count == 0) {
          continue;
        }

        const float smoothed = neighbor_sum / float(neighbor_count);
        const float old_weight = weights[point_i];
        next_weights[point_i] = old_weight + (smoothed - old_weight) * factor;
      }
    }

    weights.as_mutable_span().copy_from(next_weights.as_span());
  }

  for (const int point_i : IndexRange(total_points)) {
    bke::curves::set_vertex_group_weight(ob,
                                         point_i,
                                         active_defgroup_nr,
                                         clamp_f(weights[point_i], 0.0f, 1.0f),
                                         WEIGHT_REPLACE);
  }

  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_smooth(wmOperatorType *ot)
{
  ot->name = "Smooth Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_smooth";
  ot->description = "Smooth the weights of the active vertex group";

  ot->poll = curves_weight_paint_mode_poll;
  ot->exec = curves_vertex_group_smooth_exec;

  ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;

  RNA_def_float(ot->srna, "factor", 0.5f, 0.0f, 1.0f, "Factor", "", 0.0f, 1.0f);
  RNA_def_int(ot->srna, "repeat", 1, 1, 10000, "Iterations", "", 1, 200);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Normalize Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_normalize_exec(bContext *C, wmOperator *op)
{
  Object *ob = curves_weight_paint_original_object_get(C);
  if (!ob || ob->type != OB_CURVES || ob->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int active_defgroup_nr = BKE_object_defgroup_active_index_get(ob) - 1;
  if (active_defgroup_nr < 0) {
    return OPERATOR_CANCELLED;
  }

  const bDeformGroup *active_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(ob), active_defgroup_nr));
  if (active_defgroup && (active_defgroup->flag & DG_LOCK_WEIGHT) != 0) {
    BKE_report(op->reports, RPT_WARNING, "Active vertex group is locked");
    return OPERATOR_CANCELLED;
  }

  Curves *curves_id = id_cast<Curves *>(ob->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bke::curves::ensure_deform_verts(ob);
  const int total_points = bke::curves::get_curves_vertex_count(ob);
  if (total_points == 0) {
    return OPERATOR_CANCELLED;
  }

  float max_weight = 0.0f;
  for (const int point_i : IndexRange(total_points)) {
    const float weight = bke::curves::get_vertex_group_weight(ob, point_i, active_defgroup_nr);
    if (weight > max_weight) {
      max_weight = weight;
    }
  }

  if (ELEM(max_weight, 0.0f, 1.0f)) {
    return OPERATOR_FINISHED;
  }

  for (const int point_i : IndexRange(total_points)) {
    const float weight = bke::curves::get_vertex_group_weight(ob, point_i, active_defgroup_nr);
    if (weight < 0.0f) {
      continue;
    }
    bke::curves::set_vertex_group_weight(
        ob, point_i, active_defgroup_nr, clamp_f(weight / max_weight, 0.0f, 1.0f), WEIGHT_REPLACE);
  }

  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_normalize(wmOperatorType *ot)
{
  ot->name = "Normalize Vertex Group";
  ot->idname = "CURVES_OT_vertex_group_normalize";
  ot->description = "Normalize weights of the active vertex group";

  ot->poll = curves_weight_paint_mode_poll;
  ot->exec = curves_vertex_group_normalize_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Group Normalize All Operator
 * \{ */

static wmOperatorStatus curves_vertex_group_normalize_all_exec(bContext *C, wmOperator *op)
{
  Object *ob = curves_weight_paint_original_object_get(C);
  if (!ob || ob->type != OB_CURVES || ob->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  Curves *curves_id = id_cast<Curves *>(ob->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }

  bke::curves::ensure_deform_verts(ob);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  MutableSpan<MDeformVert> deform_verts = curves.deform_verts_for_write();
  if (deform_verts.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  const int curves_vgroup_count = BLI_listbase_count(&curves.vertex_group_names);
  if (curves_vgroup_count <= 0) {
    return OPERATOR_CANCELLED;
  }

  const int active_defgroup_nr = BKE_object_defgroup_active_index_get(ob) - 1;
  const bDeformGroup *active_defgroup = static_cast<const bDeformGroup *>(
      BLI_findlink(BKE_object_defgroup_list(ob), active_defgroup_nr));
  const bool lock_active = RNA_boolean_get(op->ptr, "lock_active");

  Vector<bool> subset_flags(curves_vgroup_count, true);
  Vector<bool> lock_flags(curves_vgroup_count, false);
  Vector<bool> soft_lock_flags(curves_vgroup_count, false);

  int def_nr = 0;
  for (const bDeformGroup &dg : curves.vertex_group_names) {
    const bDeformGroup *object_dg = BKE_object_defgroup_find_name(ob, dg.name);
    if (object_dg && (object_dg->flag & DG_LOCK_WEIGHT) != 0) {
      lock_flags[def_nr] = true;
    }
    def_nr++;
  }

  if (lock_active && active_defgroup != nullptr) {
    const int active_curves_def_nr = BKE_defgroup_name_index(&curves.vertex_group_names,
                                                              active_defgroup->name);
    if (active_curves_def_nr >= 0 && active_curves_def_nr < curves_vgroup_count &&
        !lock_flags[active_curves_def_nr])
    {
      /* Match GP policy: prefer to keep active group unchanged, but allow adjusting it
       * when needed to make normalization possible. */
      soft_lock_flags[active_curves_def_nr] = true;
    }
  }

  for (MDeformVert &dvert : deform_verts) {
    BKE_defvert_normalize_ex(
        dvert, subset_flags.as_span(), lock_flags.as_span(), soft_lock_flags.as_span());
  }

  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_group_normalize_all(wmOperatorType *ot)
{
  ot->name = "Normalize All Vertex Groups";
  ot->idname = "CURVES_OT_vertex_group_normalize_all";
  ot->description =
      "Normalize the weights of all vertex groups, so that for each vertex, the sum of all "
      "weights is 1.0";

  ot->poll = curves_weight_paint_mode_poll;
  ot->exec = curves_vertex_group_normalize_all_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "lock_active",
                  true,
                  "Lock Active",
                  "Keep the values of the active group while normalizing others");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Stroke Operators
 * \{ */

static std::unique_ptr<CurvesWeightPaintStrokeOperation> start_stroke_operation(
    const BrushStrokeMode brush_mode, const BrushSwitchMode brush_switch_mode, const bContext &C)
{
  const Object *object = CTX_data_active_object(&C);
  if (!object || object->type != OB_CURVES) {
    return nullptr;
  }

  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (!brush) {
    return nullptr;
  }

  if (brush_switch_mode == BrushSwitchMode::Smooth) {
    return new_weight_paint_blur_operation();
  }

  switch (eBrushWeightPaintType(brush->weight_brush_type)) {
    case WPAINT_BRUSH_TYPE_DRAW:
      return new_weight_paint_draw_operation(brush_mode);
    case WPAINT_BRUSH_TYPE_BLUR:
      return new_weight_paint_blur_operation();
    case WPAINT_BRUSH_TYPE_AVERAGE:
      return new_weight_paint_average_operation();
    case WPAINT_BRUSH_TYPE_SMEAR:
      return new_weight_paint_smear_operation();
  }
  
  return nullptr;
}

struct CurvesWeightPaintBrushStroke final : public PaintStroke {
  CurvesWeightPaintBrushStroke(bContext *C, wmOperator *op, const int event_type)
      : PaintStroke(C, op, event_type)
  {
  }

  bool get_location(float out[3], const float mouse[2], bool /*force_original*/) override
  {
    out[0] = mouse[0];
    out[1] = mouse[1];
    out[2] = 0.0f;
    return true;
  }

  bool test_start(wmOperator * /*op*/, const float /*mouse*/[2]) override
  {
    return true;
  }

  void update_step(wmOperator *op, PointerRNA *stroke_element) override
  {
    StrokeExtension stroke_extension;
    RNA_float_get_array(stroke_element, "mouse", stroke_extension.mouse_position);
    stroke_extension.pressure = RNA_float_get(stroke_element, "pressure");
    stroke_extension.reports = op->reports;

    if (!operation_) {
      stroke_extension.is_first = true;
      operation_ = start_stroke_operation(
          BrushStrokeMode(RNA_enum_get(op->ptr, "mode")),
          BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle")),
          *this->evil_C);
      if (!operation_) {
        return;
      }
      operation_->on_stroke_begin(*this->evil_C, stroke_extension);
    }
    else {
      stroke_extension.is_first = false;
    }

    operation_->on_stroke_extended(*this->evil_C, stroke_extension);
  }

  void redraw(bool /*final*/) override {}

  bool test_cancel() override
  {
    return false;
  }

  void done(bool /*is_cancel*/, bool /*stroke_started*/) override
  {
    if (operation_) {
      operation_->on_stroke_done(*this->evil_C);
    }
  }

 private:
  std::unique_ptr<CurvesWeightPaintStrokeOperation> operation_;
};

static bool curves_weight_paint_brush_stroke_poll(bContext *C)
{
  const bool mode_ok = curves_weight_paint_poll(C);
  const bool tool_ok = WM_toolsystem_active_tool_is_brush(C);
  static int debug_poll_count = 0;
  debug_poll_count++;

  if (debug_poll_count <= 40 || (debug_poll_count % 200) == 0) {
    const Object *object = CTX_data_active_object(C);
    printf("[DEBUG] CURVES_OT_weight_paint_brush_stroke poll: mode_ok=%d tool_ok=%d object=%s type=%d "
           "mode=%d\n",
           mode_ok,
           tool_ok,
           object ? object->id.name + 2 : "NULL",
           object ? object->type : -1,
           object ? object->mode : -1);
  }

  if (!mode_ok || !tool_ok) {
    static int debug_poll_reject_count = 0;
    debug_poll_reject_count++;
    if (debug_poll_reject_count <= 20 || (debug_poll_reject_count % 200) == 0) {
      const Object *object = CTX_data_active_object(C);
      printf("[DEBUG] CURVES_OT_weight_paint_brush_stroke poll rejected: mode_ok=%d tool_ok=%d "
             "object=%s type=%d mode=%d\n",
             mode_ok,
             tool_ok,
             object ? object->id.name + 2 : "NULL",
             object ? object->type : -1,
             object ? object->mode : -1);
    }
  }

  if (!mode_ok) {
    return false;
  }
  if (!tool_ok) {
    return false;
  }
  return true;
}

static wmOperatorStatus curves_weight_paint_brush_stroke_invoke(bContext *C,
                                                                 wmOperator *op,
                                                                 const wmEvent *event)
{
  const Object *object = curves_weight_paint_original_object_get(C);
  if (!object || object->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const ListBase *defbase = BKE_object_defgroup_list(object);
  const int active_defgroup_1based = BKE_object_defgroup_active_index_get(object);
  const int active_defgroup_nr = active_defgroup_1based - 1;

  const bDeformGroup *active_defgroup = (active_defgroup_nr >= 0) ?
                                             static_cast<const bDeformGroup *>(
                                                 BLI_findlink(defbase, active_defgroup_nr)) :
                                             nullptr;
  const bool active_group_locked = active_defgroup && ((active_defgroup->flag & DG_LOCK_WEIGHT) != 0);

  if (active_group_locked) {
    BKE_report(op->reports, RPT_WARNING, "Active group is locked, aborting");
    return OPERATOR_CANCELLED;
  }

  CurvesWeightPaintBrushStroke *stroke = MEM_new<CurvesWeightPaintBrushStroke>(
      __func__, C, op, event->type);
  op->customdata = stroke;

  const wmOperatorStatus retval = op->type->modal(C, op, event);
  OPERATOR_RETVAL_CHECK(retval);

  if (retval == OPERATOR_FINISHED) {
    MEM_delete(stroke);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus curves_weight_paint_brush_stroke_modal(bContext *C,
                                                               wmOperator *op,
                                                               const wmEvent *event)
{
  CurvesWeightPaintBrushStroke *stroke = static_cast<CurvesWeightPaintBrushStroke *>(
      op->customdata);
  const wmOperatorStatus retval = stroke->modal(C, op, event);
  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;
  }
  return retval;
}

static void curves_weight_paint_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    CurvesWeightPaintBrushStroke *stroke = static_cast<CurvesWeightPaintBrushStroke *>(
        op->customdata);
    stroke->cancel(C);
    MEM_delete(stroke);
    op->customdata = nullptr;
  }
}

static void CURVES_OT_weight_paint_brush_stroke(wmOperatorType *ot)
{
  ot->name = "Curves Weight Paint Brush Stroke";
  ot->idname = "CURVES_OT_weight_paint_brush_stroke";
  ot->description = "Paint weight on curves points";

  ot->poll = curves_weight_paint_brush_stroke_poll;
  ot->invoke = curves_weight_paint_brush_stroke_invoke;
  ot->modal = curves_weight_paint_brush_stroke_modal;
  ot->cancel = curves_weight_paint_brush_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}

/** \} */

}  // namespace blender::ed::sculpt_paint

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void ED_operatortypes_curves_weight_paint()
{
  using namespace blender::ed::sculpt_paint;
  WM_operatortype_append(CURVES_OT_weight_paint_toggle);
  WM_operatortype_append(CURVES_OT_vertex_group_add);
  WM_operatortype_append(CURVES_OT_vertex_group_remove);
  WM_operatortype_append(CURVES_OT_vertex_group_assign);
  WM_operatortype_append(CURVES_OT_vertex_group_remove_from);
  WM_operatortype_append(CURVES_OT_weight_sample);
  WM_operatortype_append(CURVES_OT_weight_toggle_direction);
  WM_operatortype_append(CURVES_OT_weight_invert);
  WM_operatortype_append(CURVES_OT_vertex_group_smooth);
  WM_operatortype_append(CURVES_OT_vertex_group_normalize);
  WM_operatortype_append(CURVES_OT_vertex_group_normalize_all);
  WM_operatortype_append(CURVES_OT_weight_paint_brush_stroke);
}
