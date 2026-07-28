/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "object_intern.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLI_path_utils.hh"
#include "BLI_string_utf8.h"

#include "DEG_depsgraph.hh"

#include "ED_object.hh"
#include "ED_sculpt.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::object {

/* ------------------------------------------------------------------- */
/** \name Multires Delete Higher Levels Operator
 * \{ */

static bool multires_poll(bContext *C)
{
  return edit_modifier_poll_generic(C, RNA_MultiresModifier, (1 << OB_MESH), true, false);
}

/**
 * Other objects in `active_ob`'s multi-object sculpt session (if any) whose Multires modifier
 * currently has the same `totlvl` as `reference_totlvl` — the set of objects a Subdivide/
 * Unsubdivide/Delete Higher action on `active_ob` should also apply to, so that objects already
 * in sync keep moving together while diverged objects are left untouched. Returns an empty
 * vector when `active_ob` is not in Sculpt Mode or is not part of a multi-object session.
 */
static Vector<Object *> multires_level_action_followers(bContext *C,
                                                        Object *active_ob,
                                                        const int reference_totlvl)
{
  if (!(active_ob->mode & OB_MODE_SCULPT)) {
    return {};
  }
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  const Vector<Object *> group = multires_group_objects(bmain, scene, view_layer, v3d);
  return multires_totlvl_matching_group(group, active_ob, reference_totlvl);
}

/* Refuse topology/resolution-changing multires operations while a sculpt stroke is in progress.
 * Switching resolution mid-stroke silently drops the stroke's recorded sculpt-layer delta, and
 * freeing the PBVH mid-stroke would corrupt the active brush. Returns true if the caller should
 * abort (with a report). */
static bool multires_stroke_in_progress_abort(wmOperator *op, Object *ob)
{
  if (!ob || !(ob->mode & OB_MODE_SCULPT) || !ob->runtime->sculpt_session) {
    return false;
  }
  if (ob->runtime->sculpt_session->cache == nullptr) {
    return false;
  }
  BKE_report(op->reports,
             RPT_ERROR,
             "Cannot change multires resolution while a sculpt stroke is in progress");
  return true;
}

/* Refuse operations that redefine the multires base or its tangent frames while grid-domain
 * sculpt layers exist: the layers store tangent displacement relative to the current base limit
 * surface, so Apply Base / Unsubdivide / Reshape would silently distort every layer. The user
 * bakes the layers first to keep the combined shape. Returns true if the caller should abort. */
static bool multires_sculpt_layers_abort(wmOperator *op, Object *ob)
{
  if (!ob || ob->type != OB_MESH || !ob->data) {
    return false;
  }
  if (!BKE_multires_mesh_has_grid_sculpt_layers(*id_cast<const Mesh *>(ob->data))) {
    return false;
  }
  BKE_report(op->reports,
             RPT_ERROR,
             "Cannot perform this operation while grid sculpt layers exist: bake layers first");
  return true;
}

/* Store an open sculpt-layer weight-mask edit before the subdivision level moves under it. Unlike
 * the two aborts above this does not refuse: the level change is a legitimate thing to do with
 * layers present (#multires_set_tot_level resamples both the layer coefficients and their masks),
 * and only the *session* cannot survive it — its weights live in the #SubdivCCG the change rebuilds.
 * Reported because the user did not ask for the edit to end. */
static void multires_finish_mask_edit(wmOperator *op, Object *ob)
{
  if (!ob || ob->type != OB_MESH || !ob->data) {
    return;
  }
  if (blender::ed::sculpt_paint::layers::finish_mask_edit(*ob)) {
    BKE_report(op->reports,
               RPT_INFO,
               "Applied the sculpt layer weight mask being edited: changing the subdivision level "
               "cannot preserve an open edit");
  }
}

static wmOperatorStatus multires_higher_levels_delete_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *ob = context_active_object(C);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, ob, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }
  if (multires_stroke_in_progress_abort(op, ob)) {
    return OPERATOR_CANCELLED;
  }
  /* After the refusals, so a cancelled operator never ends an edit the user is still working on. */
  multires_finish_mask_edit(op, ob);

  const int old_totlvl = mmd->totlvl;
  const Vector<Object *> followers = multires_level_action_followers(C, ob, old_totlvl);

  multiresModifier_del_levels(mmd, scene, ob, 1);

  iter_other(bmain, ob, true, multires_update_totlevels, &mmd->totlvl);

  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  for (Object *follower_ob : followers) {
    MultiresModifierData *follower_mmd = reinterpret_cast<MultiresModifierData *>(
        BKE_modifiers_findby_type(follower_ob, eModifierType_Multires));

    multiresModifier_del_levels(follower_mmd, scene, follower_ob, 1);

    iter_other(bmain, follower_ob, true, multires_update_totlevels, &follower_mmd->totlvl);

    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, follower_ob);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_higher_levels_delete_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_higher_levels_delete_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_higher_levels_delete(wmOperatorType *ot)
{
  ot->name = "Delete Higher Levels";
  ot->description = "Deletes the higher resolution mesh, potential loss of detail";
  ot->idname = "OBJECT_OT_multires_higher_levels_delete";

  ot->poll = multires_poll;
  ot->invoke = multires_higher_levels_delete_invoke;
  ot->exec = multires_higher_levels_delete_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Subdivide Operator
 * \{ */

static EnumPropertyItem prop_multires_subdivide_mode_type[] = {
    {int8_t(MultiresSubdivideModeType::CatmullClark),
     "CATMULL_CLARK",
     0,
     "Catmull-Clark",
     "Create a new level using Catmull-Clark subdivisions"},
    {int8_t(MultiresSubdivideModeType::Simple),
     "SIMPLE",
     0,
     "Simple",
     "Create a new level using simple subdivisions"},
    {int8_t(MultiresSubdivideModeType::Linear),
     "LINEAR",
     0,
     "Linear",
     "Create a new level using linear interpolation of the sculpted displacement"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus multires_subdivide_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *object = context_active_object(C);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, object, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }
  if (multires_stroke_in_progress_abort(op, object)) {
    return OPERATOR_CANCELLED;
  }
  /* After the refusals, so a cancelled operator never ends an edit the user is still working on. */
  multires_finish_mask_edit(op, object);

  const MultiresSubdivideModeType subdivide_mode = MultiresSubdivideModeType(
      RNA_enum_get(op->ptr, "mode"));
  const int old_totlvl = mmd->totlvl;
  const Vector<Object *> followers = multires_level_action_followers(C, object, old_totlvl);

  multiresModifier_subdivide(object, mmd, subdivide_mode);

  iter_other(bmain, object, true, multires_update_totlevels, &mmd->totlvl);

  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, object);

  if (object->mode & OB_MODE_SCULPT) {
    /* ensure that grid paint mask layer is created */
    BKE_sculpt_mask_layers_ensure(CTX_data_ensure_evaluated_depsgraph(C), bmain, object, mmd);
  }

  for (Object *follower_ob : followers) {
    MultiresModifierData *follower_mmd = reinterpret_cast<MultiresModifierData *>(
        BKE_modifiers_findby_type(follower_ob, eModifierType_Multires));

    multiresModifier_subdivide(follower_ob, follower_mmd, subdivide_mode);

    iter_other(bmain, follower_ob, true, multires_update_totlevels, &follower_mmd->totlvl);

    DEG_id_tag_update(&follower_ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, follower_ob);

    /* `follower_ob` is guaranteed to be in Sculpt Mode (see multires_level_action_followers). */
    BKE_sculpt_mask_layers_ensure(
        CTX_data_ensure_evaluated_depsgraph(C), bmain, follower_ob, follower_mmd);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_subdivide_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_subdivide_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_subdivide(wmOperatorType *ot)
{
  ot->name = "Multires Subdivide";
  ot->description = "Add a new level of subdivision";
  ot->idname = "OBJECT_OT_multires_subdivide";

  ot->poll = multires_poll;
  ot->invoke = multires_subdivide_invoke;
  ot->exec = multires_subdivide_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
  RNA_def_enum(ot->srna,
               "mode",
               prop_multires_subdivide_mode_type,
               int8_t(MultiresSubdivideModeType::CatmullClark),
               "Subdivision Mode",
               "How the mesh is going to be subdivided to create a new level");
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Reshape Operator
 * \{ */

static wmOperatorStatus multires_reshape_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object *ob = context_active_object(C), *secondob = nullptr;
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, ob, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }

  if (multires_sculpt_layers_abort(op, ob)) {
    return OPERATOR_CANCELLED;
  }

  if (mmd->lvl == 0) {
    BKE_report(op->reports, RPT_ERROR, "Reshape can work only with higher levels of subdivisions");
    return OPERATOR_CANCELLED;
  }

  CTX_DATA_BEGIN (C, Object *, selob, selected_editable_objects) {
    if (selob->type == OB_MESH && selob != ob) {
      secondob = selob;
      break;
    }
  }
  CTX_DATA_END;

  if (!secondob) {
    BKE_report(op->reports, RPT_ERROR, "Second selected mesh object required to copy shape from");
    return OPERATOR_CANCELLED;
  }

  if (!multiresModifier_reshapeFromObject(depsgraph, mmd, ob, secondob)) {
    BKE_report(op->reports, RPT_ERROR, "Objects do not have the same number of vertices");
    return OPERATOR_CANCELLED;
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, ob);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_reshape_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_reshape_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_reshape(wmOperatorType *ot)
{
  ot->name = "Multires Reshape";
  ot->description = "Copy vertex coordinates from other object";
  ot->idname = "OBJECT_OT_multires_reshape";

  ot->poll = multires_poll;
  ot->invoke = multires_reshape_invoke;
  ot->exec = multires_reshape_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Save External Operator
 * \{ */

static wmOperatorStatus multires_external_save_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = context_active_object(C);
  Mesh *mesh = (ob) ? id_cast<Mesh *>(ob->data) : static_cast<Mesh *>(op->customdata);
  char filepath[FILE_MAX];
  const bool relative = RNA_boolean_get(op->ptr, "relative_path");

  if (!mesh) {
    return OPERATOR_CANCELLED;
  }

  if (CustomData_external_test(&mesh->corner_data, CD_MDISPS)) {
    return OPERATOR_CANCELLED;
  }

  RNA_string_get(op->ptr, "filepath", filepath);

  if (relative) {
    BLI_path_rel(filepath, BKE_main_blendfile_path(bmain));
  }

  CustomData_external_add(&mesh->corner_data, &mesh->id, CD_MDISPS, mesh->corners_num, filepath);
  CustomData_external_write(
      &mesh->corner_data, &mesh->id, CD_MASK_MESH.lmask, mesh->corners_num, 0);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_external_save_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  Object *ob = context_active_object(C);
  Mesh *mesh = id_cast<Mesh *>(ob->data);
  char filepath[FILE_MAX];

  if (!edit_modifier_invoke_properties(C, op)) {
    return OPERATOR_CANCELLED;
  }

  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, ob, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }

  if (CustomData_external_test(&mesh->corner_data, CD_MDISPS)) {
    return OPERATOR_CANCELLED;
  }

  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return multires_external_save_exec(C, op);
  }

  op->customdata = mesh;

  /* While a filename need not be UTF8, at this point the constructed name should be UTF8. */
  SNPRINTF_UTF8(filepath, "//%s.btx", mesh->id.name + 2);
  RNA_string_set(op->ptr, "filepath", filepath);

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

void OBJECT_OT_multires_external_save(wmOperatorType *ot)
{
  ot->name = "Multires Save External";
  ot->description = "Save displacements to an external file";
  ot->idname = "OBJECT_OT_multires_external_save";

  /* XXX modifier no longer in context after file browser: `ot->poll = multires_poll;`. */
  ot->exec = multires_external_save_exec;
  ot->invoke = multires_external_save_invoke;
  ot->poll = multires_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_BTX,
                                 FILE_SPECIAL,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_RELPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);
  edit_modifier_properties(ot);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Pack Operator
 * \{ */

static wmOperatorStatus multires_external_pack_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = context_active_object(C);
  Mesh *mesh = id_cast<Mesh *>(ob->data);

  if (!CustomData_external_test(&mesh->corner_data, CD_MDISPS)) {
    return OPERATOR_CANCELLED;
  }

  /* XXX don't remove. */
  CustomData_external_remove(&mesh->corner_data, &mesh->id, CD_MDISPS, mesh->corners_num);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_multires_external_pack(wmOperatorType *ot)
{
  ot->name = "Multires Pack External";
  ot->description = "Pack displacements from an external file";
  ot->idname = "OBJECT_OT_multires_external_pack";

  ot->poll = multires_poll;
  ot->exec = multires_external_pack_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Apply Base
 * \{ */

static wmOperatorStatus multires_base_apply_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *object = context_active_object(C);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, object, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }
  if (multires_sculpt_layers_abort(op, object)) {
    return OPERATOR_CANCELLED;
  }

  const ApplyBaseMode mode = RNA_boolean_get(op->ptr, "apply_heuristic") ?
                                 ApplyBaseMode::ForSubdivision :
                                 ApplyBaseMode::Base;

  ed::sculpt_paint::undo::push_multires_mesh_begin(C, op->type->name);

  multiresModifier_base_apply(depsgraph, object, mmd, mode);

  ed::sculpt_paint::undo::push_multires_mesh_end(C, op->type->name);

  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, object);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_base_apply_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_base_apply_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_base_apply(wmOperatorType *ot)
{
  ot->name = "Multires Apply Base";
  ot->description = "Modify the base mesh to conform to the displaced mesh";
  ot->idname = "OBJECT_OT_multires_base_apply";

  ot->poll = multires_poll;
  ot->invoke = multires_base_apply_invoke;
  ot->exec = multires_base_apply_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
  PropertyRNA *prop = RNA_def_boolean(
      ot->srna,
      "apply_heuristic",
      true,
      "Apply Subdivision Heuristic",
      "Whether or not the final base mesh positions will be slightly altered to account for a new "
      "subdivision modifier being added");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Unsubdivide
 * \{ */

static wmOperatorStatus multires_unsubdivide_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *object = context_active_object(C);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, object, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }
  if (multires_sculpt_layers_abort(op, object)) {
    return OPERATOR_CANCELLED;
  }

  const int old_totlvl = mmd->totlvl;
  const Vector<Object *> followers = multires_level_action_followers(C, object, old_totlvl);

  MultiresUnsubdivideInfo info = {};
  int new_levels = multiresModifier_rebuild_subdiv(depsgraph, object, mmd, 1, true, info);
  if (new_levels == 0) {
    BKE_report(op->reports, RPT_ERROR, "No valid subdivisions found to rebuild a lower level");
    return OPERATOR_CANCELLED;
  }
  multiresModifier_unsubdivide_report_if_needed(info, op->reports);

  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, object);

  int skipped_follower_num = 0;
  for (Object *follower_ob : followers) {
    MultiresModifierData *follower_mmd = reinterpret_cast<MultiresModifierData *>(
        BKE_modifiers_findby_type(follower_ob, eModifierType_Multires));

    MultiresUnsubdivideInfo follower_info = {};
    const int follower_new_levels = multiresModifier_rebuild_subdiv(
        depsgraph, follower_ob, follower_mmd, 1, true, follower_info);
    if (follower_new_levels == 0) {
      skipped_follower_num++;
      continue;
    }
    multiresModifier_unsubdivide_report_if_needed(follower_info, op->reports);

    DEG_id_tag_update(&follower_ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, follower_ob);
  }
  if (skipped_follower_num > 0) {
    BKE_reportf(op->reports,
               RPT_WARNING,
               "%d other object(s) had no valid subdivisions to rebuild and were skipped",
               skipped_follower_num);
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_unsubdivide_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_unsubdivide_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_unsubdivide(wmOperatorType *ot)
{
  ot->name = "Unsubdivide";
  ot->description = "Rebuild a lower subdivision level of the current base mesh";
  ot->idname = "OBJECT_OT_multires_unsubdivide";

  ot->poll = multires_poll;
  ot->invoke = multires_unsubdivide_invoke;
  ot->exec = multires_unsubdivide_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Rebuild Subdivisions
 * \{ */

static wmOperatorStatus multires_rebuild_subdiv_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *object = context_active_object(C);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, object, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }
  if (multires_sculpt_layers_abort(op, object)) {
    return OPERATOR_CANCELLED;
  }

  MultiresUnsubdivideInfo info = {};
  int new_levels = multiresModifier_rebuild_subdiv(depsgraph, object, mmd, INT_MAX, false, info);
  if (new_levels == 0) {
    BKE_report(op->reports, RPT_ERROR, "No valid subdivisions found to rebuild lower levels");
    return OPERATOR_CANCELLED;
  }
  multiresModifier_unsubdivide_report_if_needed(info, op->reports);

  BKE_reportf(op->reports, RPT_INFO, "%d new levels rebuilt", new_levels);

  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, object);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_rebuild_subdiv_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_rebuild_subdiv_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_rebuild_subdiv(wmOperatorType *ot)
{
  ot->name = "Rebuild Lower Subdivisions";
  ot->description =
      "Rebuilds all possible subdivisions levels to generate a lower resolution base mesh";
  ot->idname = "OBJECT_OT_multires_rebuild_subdiv";

  ot->poll = multires_poll;
  ot->invoke = multires_rebuild_subdiv_invoke;
  ot->exec = multires_rebuild_subdiv_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Multires Level Sync Operator
 * \{ */

static const EnumPropertyItem prop_multires_level_sync_type_items[] = {
    {int(MultiresLevelType::Viewport),
     "VIEWPORT",
     0,
     "Viewport",
     "Sync the viewport subdivision level"},
    {int(MultiresLevelType::Sculpt), "SCULPT", 0, "Sculpt", "Sync the sculpt subdivision level"},
    {int(MultiresLevelType::Render), "RENDER", 0, "Render", "Sync the render subdivision level"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus multires_level_sync_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  Object *ob = context_active_object(C);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(
      edit_modifier_property_get(op, ob, eModifierType_Multires));

  if (!mmd) {
    return OPERATOR_CANCELLED;
  }

  const MultiresLevelType level_type = MultiresLevelType(RNA_enum_get(op->ptr, "level_type"));
  const int reference_value = multires_level_get(mmd, level_type);

  const Vector<Object *> candidates = multires_group_objects(bmain, scene, view_layer, v3d);
  Vector<Object *> skipped;
  const Vector<Object *> changed = multires_level_group_sync(
      candidates, ob, level_type, reference_value, reference_value, false, &skipped);

  for (Object *changed_ob : changed) {
    DEG_id_tag_update(&changed_ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, changed_ob);
  }

  for (const Object *skipped_ob : skipped) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Sync Subdivision Level: skipping \"%s\" (finish the sculpt stroke or the sculpt "
                "layer weight mask edit on it first)",
                skipped_ob->id.name + 2);
  }

  /* Only claim a match when nothing was held back: a skipped object is still out of sync, and
   * saying otherwise would send the user away from the one thing that fixes it. */
  if (changed.is_empty() && skipped.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "All selected objects already match");
  }

  return OPERATOR_FINISHED;
}

static wmOperatorStatus multires_level_sync_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent * /*event*/)
{
  if (edit_modifier_invoke_properties(C, op)) {
    return multires_level_sync_exec(C, op);
  }
  return OPERATOR_CANCELLED;
}

void OBJECT_OT_multires_level_sync(wmOperatorType *ot)
{
  ot->name = "Sync Subdivision Level";
  ot->description =
      "Apply this subdivision level to every selected sculpt-mode object with a Multires "
      "modifier";
  ot->idname = "OBJECT_OT_multires_level_sync";

  ot->poll = multires_poll;
  ot->invoke = multires_level_sync_invoke;
  ot->exec = multires_level_sync_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
  edit_modifier_properties(ot);
  RNA_def_enum(ot->srna,
              "level_type",
              prop_multires_level_sync_type_items,
              int(MultiresLevelType::Viewport),
              "Level Type",
              "Which subdivision level field to sync");
}

/** \} */

}  // namespace blender::ed::object
