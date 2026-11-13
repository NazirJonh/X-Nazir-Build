/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The projection frame's controller operators (create, select and place the Empty) and the
 * operator-type registration of the whole projection module, so #paint_ops.cc registers the module
 * with one call.
 */

#include "projection_ops.hh"

#include <optional>
#include <string>

#include "DNA_collection_types.h"
#include "DNA_constraint_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "BKE_collection.hh"
#include "BKE_constraint.h"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_object.hh"
#include "ED_paint.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../uv_mapping/uv_mapping_target.hh"

namespace blender::ed::sculpt_paint::projection {

/* -------------------------------------------------------------------- */
/** \name Projection Controller
 *
 * Ucupaint manages its decal through an Empty it creates, selects and places for the user
 * (create_decal_empty, YSelectDecalObject, YSetDecalObjectPositionToCursor). These operators do
 * the same for a row's projection controller, and mark the Empties they create with the
 * `paint_layer_controller` ID property, so a later cleanup can tell them from a user's own.
 * \{ */

/** The shown row and its material, as the Mapping panel and the reset resolve them. */
struct LayerProjectionTarget {
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
};

static std::optional<LayerProjectionTarget> layer_projection_target(bContext *C)
{
  Material *ma = BKE_paint_layers_active_material_get(CTX_data_active_object(C));
  MaterialPaintLayer *row = (ma != nullptr) ? uv_mapping::mapping_shown_row_get(*C, *ma) :
                                              nullptr;
  if (row == nullptr || !BKE_paint_layers_projection_supported(*ma, *row)) {
    return std::nullopt;
  }
  return LayerProjectionTarget{ma, row};
}

/** Ucupaint's set_gizmo_style: the Empty's display shows the frame kind it controls. */
static eObject_EmptyDrawType layer_projection_controller_display_type(
    const MaterialPaintLayer &row)
{
  /* The mode table the RNA setter and this operator share: each mode that lands sets its Empty's
   * look in its row, so it is decided in one place. */
  return eObject_EmptyDrawType(
      BKE_paint_layers_projection_mode_info(row.mapping.space).controller_display);
}

/** The ID property that marks an Empty as a controller this editor created. */
static constexpr const char *layer_projection_controller_mark_name = "paint_layer_controller";

/** Mark the controller as ours, so a later cleanup can tell it from a user's own Empty. */
static void layer_projection_controller_mark(Object &empty)
{
  if (empty.id.properties == nullptr) {
    empty.id.properties = bke::idprop::create_group("properties").release();
  }
  IDProperty *mark = IDP_GetPropertyFromGroup(empty.id.properties,
                                              layer_projection_controller_mark_name);
  if (mark == nullptr) {
    IDP_AddToGroup(empty.id.properties,
                   bke::idprop::create(layer_projection_controller_mark_name, 1).release());
  }
}

/** The constraint Snap to Surface owns on a controller; anything else with another name stays. */
static const char *layer_projection_snap_constraint_name()
{
  return "Paint Layer Snap";
}

static bConstraint *layer_projection_snap_constraint_find(Object &controller)
{
  for (bConstraint *con = static_cast<bConstraint *>(controller.constraints.first); con != nullptr;
       con = con->next)
  {
    if (con->type == CONSTRAINT_TYPE_SHRINKWRAP &&
        STREQ(con->name, layer_projection_snap_constraint_name()))
    {
      return con;
    }
  }
  return nullptr;
}

static void layer_projection_snap_constraint_remove(Object &controller)
{
  /* Removal unlinks the constraint, so walk the list by hand and hold the next link before the
   * call frees the current one. */
  for (bConstraint *con = static_cast<bConstraint *>(controller.constraints.first), *next = nullptr;
       con != nullptr;
       con = next)
  {
    next = con->next;
    if (con->type == CONSTRAINT_TYPE_SHRINKWRAP &&
        STREQ(con->name, layer_projection_snap_constraint_name()))
    {
      BKE_constraint_remove_ex(&controller.constraints, &controller, con);
    }
  }
}

static void layer_projection_snap_constraint_add(Object &controller, Object *target)
{
  bConstraint *con = BKE_constraint_add_for_object(
      &controller, layer_projection_snap_constraint_name(), CONSTRAINT_TYPE_SHRINKWRAP);
  if (con == nullptr) {
    return;
  }
  bShrinkwrapConstraint *data = static_cast<bShrinkwrapConstraint *>(con->data);
  /* Ucupaint's enable_shrinkwrap: sit on the nearest surface and turn the frame's local Z to the
   * target normal, so the projection axis follows the face it lands on. */
  data->shrinkType = MOD_SHRINKWRAP_NEAREST_SURFACE;
  data->shrinkMode = MOD_SHRINKWRAP_ON_SURFACE;
  data->trackAxis = TRACK_Z;
  data->flag |= CON_SHRINKWRAP_TRACK_NORMAL;
  /* A constraint target is not a user reference (shrinkwrap_id_looper reports it as such), so the
   * pointer is set directly. */
  data->target = target;
}

static wmOperatorStatus layer_projection_add_controller_exec(bContext *C, wmOperator *op)
{
  const std::optional<LayerProjectionTarget> target = layer_projection_target(C);
  if (!target.has_value()) {
    BKE_report(op->reports, RPT_ERROR, "The shown layer row takes no projection");
    return OPERATOR_CANCELLED;
  }
  Material &ma = *target->ma;
  MaterialPaintLayer &row = *target->row;
  const int16_t previous_space = row.mapping.space;

  if (row.mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_UV) {
    /* Adding a controller is how a row starts projecting, like Ucupaint's first Decal. */
    if (!BKE_paint_layers_projection_set_space(ma, &row, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR)) {
      BKE_report(op->reports, RPT_ERROR, "The shown layer row cannot project");
      return OPERATOR_CANCELLED;
    }
  }

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *active = CTX_data_active_object(C);

  Object *empty = (active != nullptr) ?
                      BKE_object_add_from(bmain, scene, view_layer, OB_EMPTY, "Controller", active) :
                      BKE_object_add(bmain, scene, view_layer, OB_EMPTY, "Controller");
  empty->empty_drawtype = layer_projection_controller_display_type(row);
  /* Ucupaint puts the decal at the 3D cursor, parented to the active object with the inverse of
   * its world matrix, so moving the painted object carries the frame along. */
  copy_v3_v3(empty->loc, scene->cursor.location);
  copy_v3_v3(empty->rot, scene->cursor.rotation_euler);
  if (active != nullptr) {
    empty->parent = active;
    float parent_inverse[4][4];
    invert_m4_m4(parent_inverse, active->object_to_world().ptr());
    copy_m4_m4(empty->parentinv, parent_inverse);
  }
  layer_projection_controller_mark(*empty);

  if (!BKE_paint_layers_projection_set_object(ma, &row, empty)) {
    BKE_id_delete(bmain, empty);
    /* The Empty was parented and linked into the scene, so the relations built with it go stale. */
    DEG_relations_tag_update(bmain);
    /* A row that got no controller must not be left projecting through an inline frame the user
     * never asked for: the space switch above is the only other change this operator made. */
    BKE_paint_layers_projection_set_space(ma, &row, previous_space);
    BKE_report(op->reports, RPT_ERROR, "The controller could not be assigned to the row");
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma);
    return OPERATOR_CANCELLED;
  }

  /* A re-added controller starts clean: the row's Snap flag, if it was on, asks for its constraint
   * again. */
  ED_paint_layer_projection_controller_snap_update(*empty, row);

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&empty->id, ID_RECALC_TRANSFORM);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, empty);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma);
  return OPERATOR_FINISHED;
}

static bool layer_projection_add_controller_poll(bContext *C)
{
  return layer_projection_target(C).has_value();
}

void PAINT_OT_layer_projection_add_controller(wmOperatorType *ot)
{
  ot->name = "Add Projection Controller";
  ot->idname = "PAINT_OT_layer_projection_add_controller";
  ot->description =
      "Create an Empty at the 3D cursor, parented to the active object, and assign it as the "
      "shown layer row's projection controller";
  ot->exec = layer_projection_add_controller_exec;
  ot->poll = layer_projection_add_controller_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static std::optional<LayerProjectionTarget> layer_projection_controller_target(bContext *C)
{
  std::optional<LayerProjectionTarget> target = layer_projection_target(C);
  if (!target.has_value() || target->row->projection_object == nullptr) {
    return std::nullopt;
  }
  return target;
}

static wmOperatorStatus layer_projection_select_controller_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<LayerProjectionTarget> target = layer_projection_controller_target(C);
  if (!target.has_value()) {
    return OPERATOR_CANCELLED;
  }
  Object *controller = target->row->projection_object;
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  /* Ucupaint's select leaves paint mode first: a selected Empty only shows its transform in
   * object mode. */
  if (CTX_data_mode_enum(C) != CTX_MODE_OBJECT) {
    ed::object::mode_set(C, OB_MODE_OBJECT);
  }

  if (BKE_view_layer_base_find(view_layer, controller) == nullptr) {
    /* The controller drifted out of the scene (unlinked or deleted from another view layer):
     * bring it back into the active collection, like YSelectDecalObject does. */
    BKE_collection_object_add(bmain, CTX_data_collection(C), controller);
    DEG_relations_tag_update(bmain);
  }

  Base *base = BKE_view_layer_base_find(view_layer, controller);
  if (base == nullptr) {
    return OPERATOR_CANCELLED;
  }
  BKE_view_layer_base_deselect_all(*bmain, scene, view_layer);
  BKE_view_layer_base_select_and_set_active(view_layer, base);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, controller);
  return OPERATOR_FINISHED;
}

static bool layer_projection_select_controller_poll(bContext *C)
{
  return layer_projection_controller_target(C).has_value();
}

void PAINT_OT_layer_projection_select_controller(wmOperatorType *ot)
{
  ot->name = "Select Projection Controller";
  ot->idname = "PAINT_OT_layer_projection_select_controller";
  ot->description =
      "Leave paint mode and select the shown layer row's projection controller in the viewport";
  ot->exec = layer_projection_select_controller_exec;
  ot->poll = layer_projection_select_controller_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus layer_projection_controller_to_cursor_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<LayerProjectionTarget> target = layer_projection_controller_target(C);
  if (!target.has_value()) {
    return OPERATOR_CANCELLED;
  }
  Object *controller = target->row->projection_object;
  const Scene *scene = CTX_data_scene(C);
  copy_v3_v3(controller->loc, scene->cursor.location);
  copy_v3_v3(controller->rot, scene->cursor.rotation_euler);
  DEG_id_tag_update(&controller->id, ID_RECALC_TRANSFORM);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, controller);
  return OPERATOR_FINISHED;
}

static bool layer_projection_controller_to_cursor_poll(bContext *C)
{
  return layer_projection_controller_target(C).has_value();
}

void PAINT_OT_layer_projection_controller_to_cursor(wmOperatorType *ot)
{
  ot->name = "Set Controller to Cursor";
  ot->idname = "PAINT_OT_layer_projection_controller_to_cursor";
  ot->description =
      "Move the shown layer row's projection controller to the 3D cursor, keeping its place in "
      "the painted object";
  ot->exec = layer_projection_controller_to_cursor_exec;
  ot->poll = layer_projection_controller_to_cursor_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::projection

namespace blender {

bool ED_paint_layer_projection_controller_is_marked(const Object *object)
{
  if (object == nullptr || object->id.properties == nullptr) {
    return false;
  }
  const IDProperty *mark = IDP_GetPropertyFromGroup(
      object->id.properties,
      ed::sculpt_paint::projection::layer_projection_controller_mark_name);
  return mark != nullptr && mark->type == IDP_INT && mark->data.val != 0;
}

bool ED_paint_layer_projection_controller_display_type_update(Object &empty,
                                                              const MaterialPaintLayer &row)
{
  /* A user's own Empty keeps the display it was given: only a marked controller follows the
   * space, which is what keeps its arrow readable as "this way the decal projects". */
  if (empty.type != OB_EMPTY || !ED_paint_layer_projection_controller_is_marked(&empty)) {
    return false;
  }
  const eObject_EmptyDrawType display_type =
      ed::sculpt_paint::projection::layer_projection_controller_display_type(row);
  if (empty.empty_drawtype == display_type) {
    return false;
  }
  empty.empty_drawtype = display_type;
  return true;
}

void ED_paint_layer_projection_controller_snap_update(Object &controller,
                                                      const MaterialPaintLayer &row)
{
  /* A user's own Empty is not ours to constrain: only the controllers this editor created carry
   * the marker and follow the row's options. */
  if (!ED_paint_layer_projection_controller_is_marked(&controller)) {
    return;
  }
  const bool snap = BKE_paint_layers_projection_snap_get(row);
  const bool present =
      ed::sculpt_paint::projection::layer_projection_snap_constraint_find(controller) != nullptr;
  if (snap == present) {
    return;
  }
  if (snap) {
    /* The Add Controller operator parents the Empty to the painted object; the same parent is the
     * surface the frame should follow. Without one the constraint waits for a target. */
    ed::sculpt_paint::projection::layer_projection_snap_constraint_add(controller,
                                                                      controller.parent);
  }
  else {
    ed::sculpt_paint::projection::layer_projection_snap_constraint_remove(controller);
  }
  DEG_id_tag_update(&controller.id, ID_RECALC_TRANSFORM);
  WM_main_add_notifier(NC_OBJECT | ND_CONSTRAINT, &controller);
}

}  // namespace blender

namespace blender::ed::sculpt_paint::projection {

/* -------------------------------------------------------------------- */
/** \name Projection Frame tool follow
 * \{ */

/** Idname of the tool in the 3D View's toolbar (`space_toolsystem_toolbar.py`). */
static constexpr const char *PROJECTION_FRAME_TOOL_ID = "builtin.projection_frame";

bool ED_paint_projection_frame_tool_is_active(const bContext *C)
{
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref != nullptr && tref->idname != nullptr && STREQ(tref->idname, PROJECTION_FRAME_TOOL_ID);
}

bool ED_paint_projection_frame_tool_activate_if_needed(bContext *C,
                                                       Material &material,
                                                       const MaterialPaintLayer *layer)
{
  /* The RNA update hooks may run without a context: the tool is a convenience that only a real
   * viewport can take. */
  if (C == nullptr) {
    return false;
  }
  Object *object = CTX_data_active_object(C);
  if (object == nullptr || (object->mode & OB_MODE_SCULPT) == 0 ||
      object->runtime->sculpt_session == nullptr)
  {
    return false;
  }
  std::string &saved_tool_id = object->runtime->sculpt_session->layers.projection_saved_tool_id;

  const bool volume_row = (layer != nullptr) &&
                          BKE_paint_layers_projection_applies(material, *layer) &&
                          BKE_paint_layers_projection_mode_info(layer->mapping.space).is_volume;
  /* A transform effect (Transform Mapping, Mirror) places its pivot or fold center on the mesh
   * through the same tool, so the viewport edits it while its row is the active one. */
  const bool transform_row = (layer != nullptr) && BKE_paint_layers_is_transform_effect(*layer);
  if (volume_row || transform_row) {
    if (!ED_paint_projection_frame_tool_is_active(C)) {
      const std::string current_tool_id = ED_paint_view3d_tool_id_get(C);
      if (current_tool_id != PROJECTION_FRAME_TOOL_ID) {
        saved_tool_id = current_tool_id;
      }
      ED_paint_view3d_tool_id_set(C, PROJECTION_FRAME_TOOL_ID);
    }
    return true;
  }
  /* Only restored while the Projection Frame tool is still active: a tool the user picked during
   * a volume row is a choice the row change should not overrule. */
  if (!saved_tool_id.empty() && ED_paint_projection_frame_tool_is_active(C)) {
    ED_paint_view3d_tool_id_set(C, saved_tool_id.c_str());
  }
  saved_tool_id.clear();
  return false;
}

/** \} */

void PAINT_OT_layer_projection_place(wmOperatorType *ot);
void PAINT_OT_layer_projection_flip(wmOperatorType *ot);
void PAINT_OT_layer_projection_move(wmOperatorType *ot);
void PAINT_OT_projection_frame_tool(wmOperatorType *ot);

void operatortypes_register()
{
  WM_operatortype_append(PAINT_OT_layer_projection_add_controller);
  WM_operatortype_append(PAINT_OT_layer_projection_select_controller);
  WM_operatortype_append(PAINT_OT_layer_projection_controller_to_cursor);
  WM_operatortype_append(PAINT_OT_layer_projection_place);
  WM_operatortype_append(PAINT_OT_layer_projection_flip);
  WM_operatortype_append(PAINT_OT_layer_projection_move);
  WM_operatortype_append(PAINT_OT_projection_frame_tool);
}

}  // namespace blender::ed::sculpt_paint::projection
