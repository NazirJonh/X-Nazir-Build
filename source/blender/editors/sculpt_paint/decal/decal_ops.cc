/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The decal operators: add an image as a projected Fill row, place the decal frame from the view,
 * and manage the decal as a block (remove, hide, move, blend, projection space). The row set and
 * the frame semantics live in #decal_target.cc.
 */

#include <cmath>
#include <cstdint>
#include <cstring>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "ED_paint.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "decal_target.hh"
#include "../projection/projection_target.hh"
#include "../selection/paint_image_select_view3d.hh"

namespace blender::ed::sculpt_paint::decal {

/* -------------------------------------------------------------------- */
/** \name Shared helpers
 * \{ */

static bool decal_context_poll(const bContext *C,
                               Object **r_object,
                               Material **r_material)
{
  *r_object = nullptr;
  *r_material = nullptr;
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !decal_mode_supported(*ob)) {
    return false;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma) || !ID_IS_EDITABLE(&ma->id)) {
    return false;
  }
  *r_object = ob;
  *r_material = ma;
  return true;
}

static bool decal_add_poll(bContext *C)
{
  Object *ob = nullptr;
  Material *ma = nullptr;
  return decal_context_poll(C, &ob, &ma);
}

static bool decal_target_poll(bContext *C)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  return decal_target_find(*C, &ma, &ob, name, nullptr);
}

static void decal_done_notify(bContext *C, Material &ma)
{
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma.id);
}

static void decal_mval_get(wmOperator *op, float r_mval[2])
{
  r_mval[0] = float(RNA_int_get(op->ptr, "mouse_x"));
  r_mval[1] = float(RNA_int_get(op->ptr, "mouse_y"));
}

static void decal_mval_store(bContext * /*C*/, wmOperator *op, const wmEvent *event)
{
  RNA_int_set(op->ptr, "mouse_x", event->mval[0]);
  RNA_int_set(op->ptr, "mouse_y", event->mval[1]);
}

/** Whether the row takes a normal map: its blend is the internal combine/replace pair. */
static bool decal_row_is_normal(const MaterialPaintLayer &row)
{
  for (const int i : IndexRange(row.channels_num)) {
    if (row.channels[i].channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      return true;
    }
  }
  return false;
}

/** The projection spaces a decal row can take: shared by the create and the switch operators. */
static const EnumPropertyItem decal_space_items[] = {
    {MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
     "PLANAR",
     0,
     "Planar",
     "Project along the frame's local axis onto its plane"},
    {MA_PAINT_LAYER_MAPPING_SPACE_BOX,
     "BOX",
     0,
     "Box",
     "Project the map onto the six sides of the frame, one map copy per face"},
    {MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR,
     "TRIPLANAR",
     0,
     "Triplanar",
     "Blend three projections by the surface normal"},
    {MA_PAINT_LAYER_MAPPING_SPACE_CUBE, "CUBE", 0, "Cube", "Read the six faces from a cube-cross atlas"},
    {MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER,
     "CYLINDER",
     0,
     "Cylinder",
     "Wrap the map around the frame's local axis"},
    {MA_PAINT_LAYER_MAPPING_SPACE_SPHERE,
     "SPHERE",
     0,
     "Sphere",
     "Project the map from the frame's local origin"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add decal
 * \{ */

static const EnumPropertyItem decal_channel_items[] = {
    {-1,
     "AUTO",
     0,
     "Auto",
     "Base Color, or Normal when the image's file name marks it as a normal map"},
    {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", "The decal's color"},
    {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", "The decal's metallic map"},
    {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", "The decal's roughness map"},
    {PAINT_MATERIAL_CHANNEL_NORMAL, "NORMAL", 0, "Normal", "The decal's tangent-space normal stamp"},
    {0, nullptr, 0, nullptr, nullptr},
};

/**
 * The channel an image maps when the caller asks the file name to decide: the decal packs mark
 * tangent-space normal maps with a `_n` / `_nor` / `_normal` suffix, everything else is colour.
 */
static eMaterialPaintChannel decal_channel_from_image_name(const Image &image)
{
  char name[MAX_NAME];
  STRNCPY(name, image.id.name + 2);
  BLI_path_extension_strip(name);
  const size_t name_len = strlen(name);
  for (const char *suffix : {"_n", "_nor", "_normal"}) {
    const size_t suffix_len = strlen(suffix);
    if (name_len >= suffix_len &&
        BLI_strcasecmp(name + name_len - suffix_len, suffix) == 0)
    {
      return PAINT_MATERIAL_CHANNEL_NORMAL;
    }
  }
  return PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

/**
 * Add the row of \a image to the decal named after it: the shared body of the file-browser add and
 * the drag-and-drop add. \a channel_value is an #eMaterialPaintChannel, or -1 to let the image's
 * file name choose.
 */
static wmOperatorStatus decal_add_rows(bContext *C,
                                       wmOperator *op,
                                       Material &ma,
                                       Object &ob,
                                       Image &image,
                                       const int channel_value,
                                       const int16_t space,
                                       const bool place_from_view,
                                       const float mval[2])
{
  const eMaterialPaintChannel channel = (channel_value < 0) ?
                                            decal_channel_from_image_name(image) :
                                            eMaterialPaintChannel(channel_value);
  if (!BKE_paint_layers_channel_in_set(ma, channel)) {
    BKE_report(op->reports, RPT_ERROR, "The channel is not part of the material's channel set");
    return OPERATOR_CANCELLED;
  }
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* A normal stamp reads its vectors raw, the way the addon's normal-mode decals do. */
    STRNCPY(image.colorspace_settings.name, "Non-Color");
  }

  /* The decal is named after the image, as in the addon; its rows are the channels mapped so far.
   * An existing decal keeps its frame: the new channel row joins it where the others project. */
  char image_name[MAX_NAME];
  STRNCPY(image_name, image.id.name + 2);
  BLI_path_extension_strip(image_name);
  char row_name[MAX_NAME];
  decal_row_name_make(image_name, BKE_paint_material_channel_info(channel).ui_name, row_name);
  /* Read back from the row name: a long image name is cut when the name is formed, and the group
   * is found by the name the rows carry. */
  char decal_name[64];
  if (!decal_name_of_row_name(row_name, decal_name)) {
    BKE_report(op->reports, RPT_ERROR, "Could not name the decal row");
    return OPERATOR_CANCELLED;
  }
  Vector<MaterialPaintLayer *> rows;
  decal_rows_find(ma, decal_name, rows);
  projection::ProjectionFrame frame;
  bool have_frame = false;
  for (MaterialPaintLayer *row : rows) {
    if (row->projection != nullptr) {
      copy_v3_v3(frame.center, row->projection->center);
      copy_v3_v3(frame.size, row->projection->size);
      copy_v3_v3(frame.normal, row->projection->normal);
      frame.roll = row->projection->roll;
      have_frame = true;
      break;
    }
  }
  if (!have_frame) {
    if (place_from_view) {
      decal_frame_from_view(*C, ob, mval, frame);
    }
    else {
      decal_frame_from_bounds(ob, frame);
    }
  }

  /* Added bare, not through the Add policy: the policy gives a Fill the default channel set, and
   * the decal row takes part in the one channel the image maps and no other. */
  MaterialPaintLayer *layer = BKE_paint_layers_add(ma, row_name, nullptr, PaintLayerPlace::Above);
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not add the decal row");
    return OPERATOR_CANCELLED;
  }
  layer->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  if (BKE_paint_layers_channel_add(ma, layer, channel) == nullptr ||
      !BKE_paint_layers_channel_set_image(ma, layer, channel, &image))
  {
    BKE_paint_layers_remove(ma, layer);
    BKE_report(op->reports, RPT_ERROR, "The image could not be assigned to the channel");
    return OPERATOR_CANCELLED;
  }
  BKE_paint_layers_projection_set_space(ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR);
  /* The decal default reads through its front side, like the addon's backface mask: the facing
   * fade is part of the payload's own default flag, so nothing is toggled here. */
  if (space != MA_PAINT_LAYER_MAPPING_SPACE_PLANAR) {
    BKE_paint_layers_projection_set_space(ma, layer, space);
  }

  rows.append(layer);
  decal_frame_apply(ma, rows, frame, true);
  BKE_paint_layers_active_set(ma, layer->marker);
  decal_done_notify(C, ma);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus decal_add_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  Material *ma = nullptr;
  if (!decal_context_poll(C, &ob, &ma)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Enter Sculpt or Texture Paint mode with an editable layered material");
    return OPERATOR_CANCELLED;
  }
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No image file given");
    return OPERATOR_CANCELLED;
  }
  Image *image = BKE_image_load_exists(CTX_data_main(C), filepath, nullptr);
  if (image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not load the image");
    return OPERATOR_CANCELLED;
  }
  float mval[2];
  decal_mval_get(op, mval);
  return decal_add_rows(C,
                        op,
                        *ma,
                        *ob,
                        *image,
                        RNA_enum_get(op->ptr, "channel"),
                        int16_t(RNA_enum_get(op->ptr, "space")),
                        RNA_boolean_get(op->ptr, "place_from_view"),
                        mval);
}

static wmOperatorStatus decal_add_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  decal_mval_store(C, op, event);
  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return decal_add_exec(C, op);
  }
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

/** The image a drop carries, resolved by name: the shared body of #PAINT_OT_decal_add_image. */
static wmOperatorStatus decal_add_image_exec(bContext *C, wmOperator *op)
{
  Object *ob = nullptr;
  Material *ma = nullptr;
  if (!decal_context_poll(C, &ob, &ma)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Enter Sculpt or Texture Paint mode with an editable layered material");
    return OPERATOR_CANCELLED;
  }
  char image_name[MAX_ID_NAME];
  RNA_string_get(op->ptr, "image_name", image_name);
  Image *image = nullptr;
  if (image_name[0] != '\0') {
    image = id_cast<Image *>(BKE_libblock_find_name(CTX_data_main(C), ID_IM, image_name));
  }
  if (image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No image to make the decal from");
    return OPERATOR_CANCELLED;
  }
  float mval[2];
  decal_mval_get(op, mval);
  return decal_add_rows(C,
                        op,
                        *ma,
                        *ob,
                        *image,
                        RNA_enum_get(op->ptr, "channel"),
                        int16_t(RNA_enum_get(op->ptr, "space")),
                        RNA_boolean_get(op->ptr, "place_from_view"),
                        mval);
}

void PAINT_OT_decal_add(wmOperatorType *ot)
{
  ot->name = "Add Decal";
  ot->description =
      "Add an image as a decal: a projected Fill row the frame of the current view maps onto the "
      "surface";
  ot->idname = "PAINT_OT_decal_add";

  ot->invoke = decal_add_invoke;
  ot->exec = decal_add_exec;
  ot->poll = decal_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "filepath",
                 nullptr,
                 FILE_MAX,
                 "Image",
                 "The decal image to project");
  RNA_def_enum(ot->srna,
               "channel",
               decal_channel_items,
               -1,
               "Channel",
               "Channel the image maps; Auto lets the file name choose");
  RNA_def_enum(ot->srna,
               "space",
               decal_space_items,
               MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
               "Space",
               "The space the decal's map reads through");
  RNA_def_boolean(ot->srna,
                  "place_from_view",
                  true,
                  "Place from View",
                  "Frame the decal through the current viewport view");
  RNA_def_int(ot->srna, "mouse_x", -1, INT32_MIN, INT32_MAX, "Mouse X", "", -1, INT32_MAX);
  RNA_def_int(ot->srna, "mouse_y", -1, INT32_MIN, INT32_MAX, "Mouse Y", "", -1, INT32_MAX);
}

void PAINT_OT_decal_add_image(wmOperatorType *ot)
{
  ot->name = "Add Decal from Image";
  ot->description =
      "Add a data-block image as a decal: the drag-and-drop path of the Decal panel, and the "
      "operator the drop target calls";
  ot->idname = "PAINT_OT_decal_add_image";

  ot->exec = decal_add_image_exec;
  ot->poll = decal_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* The image is named, not pointed at: the drop target has the image in hand and the operator
   * resolves it from Main, the pattern the material channel image pickers use. */
  RNA_def_string(ot->srna,
                 "image_name",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Image",
                 "The decal image data-block");
  RNA_def_enum(ot->srna,
               "channel",
               decal_channel_items,
               -1,
               "Channel",
               "Channel the image maps; Auto lets the file name choose");
  RNA_def_enum(ot->srna,
               "space",
               decal_space_items,
               MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
               "Space",
               "The space the decal's map reads through");
  RNA_def_boolean(ot->srna,
                  "place_from_view",
                  true,
                  "Place from View",
                  "Frame the decal through the current viewport view");
  RNA_def_int(ot->srna, "mouse_x", -1, INT32_MIN, INT32_MAX, "Mouse X", "", -1, INT32_MAX);
  RNA_def_int(ot->srna, "mouse_y", -1, INT32_MIN, INT32_MAX, "Mouse Y", "", -1, INT32_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coverage mask
 *
 * The decal group's mask preset: the addon limits a decal to a painted face map, and the branch
 * equivalent is the row's own coverage mask. Adding one to every row of the decal makes the whole
 * group paintable in Mask mode; the rows keep their own copies (masks are per row), which is what
 * the Mask brush can address.
 * \{ */

/** Switch the stroke target the way the layer-target operator does, minus its own undo step: the
 * mask preset and this switch stay one user action. */
static void decal_target_mode_set(bContext *C, Object &ob, const ePaintLayerTargetMode mode)
{
  Scene *scene = CTX_data_scene(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (scene == nullptr || scene->toolsettings == nullptr || paint == nullptr) {
    return;
  }
  BKE_paint_material_layer_target_mode_set(
      *CTX_data_main(C), *scene, *paint, scene->toolsettings->paint_mode, mode);
  /* The slots are what a projection stroke reads: the mask's map now, or the row's. */
  BKE_texpaint_slots_refresh_object(scene, &ob);
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
}

static wmOperatorStatus decal_mask_add_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No decal to mask");
    return OPERATOR_CANCELLED;
  }
  for (MaterialPaintLayer *row : rows) {
    if (BKE_paint_layers_mask_base(*row) != nullptr) {
      continue;
    }
    /* A full coverage mask: the decal looks the same until the user paints the mask away. */
    BKE_paint_layers_mask_add(*ma, row, 1.0f);
  }
  /* The decal becomes the active row so the Mask mode paints this group's coverage. */
  BKE_paint_layers_active_set(*ma, rows[0]->marker);
  if (RNA_boolean_get(op->ptr, "paint_mask")) {
    decal_target_mode_set(C, *ob, PAINT_LAYER_TARGET_MASK);
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_mask_add(wmOperatorType *ot)
{
  ot->name = "Add Decal Mask";
  ot->description =
      "Give every row of the decal a coverage mask, so the decal's extent can be painted in Mask "
      "mode";
  ot->idname = "PAINT_OT_decal_mask_add";

  ot->exec = decal_mask_add_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "paint_mask",
                  true,
                  "Paint Mask",
                  "Switch the stroke target to the mask, with the Mask brush in hand");
}

static wmOperatorStatus decal_mask_remove_exec(bContext *C, wmOperator * /*op*/)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  bool removed = false;
  for (MaterialPaintLayer *row : rows) {
    removed = BKE_paint_layers_mask_remove(*ma, row) || removed;
  }
  if (!removed) {
    return OPERATOR_CANCELLED;
  }
  /* Leaving Mask mode only when this decal's mask was the stroke target: another row's mask keeps
   * the mode it had. */
  Scene *scene = CTX_data_scene(C);
  MaterialPaintLayer *active = BKE_paint_layers_active_layer_get(*ma);
  if (scene != nullptr && scene->toolsettings != nullptr && active != nullptr &&
      scene->toolsettings->paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK &&
      rows.contains(active))
  {
    decal_target_mode_set(C, *ob, PAINT_LAYER_TARGET_CONTENT);
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_mask_remove(wmOperatorType *ot)
{
  ot->name = "Remove Decal Mask";
  ot->description = "Remove the coverage mask of every row of the decal";
  ot->idname = "PAINT_OT_decal_mask_remove";

  ot->exec = decal_mask_remove_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Project from view
 * \{ */

static wmOperatorStatus decal_project_from_view_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No decal to place");
    return OPERATOR_CANCELLED;
  }
  float mval[2];
  decal_mval_get(op, mval);
  projection::ProjectionFrame frame;
  decal_frame_from_view(*C, *ob, mval, frame);
  /* The clip toggle belongs to the rows: placing the frame only moves it. */
  const bool clip = (rows[0]->projection != nullptr) ?
                        BKE_paint_layers_projection_clip_get(*rows[0]) :
                        true;
  decal_frame_apply(*ma, rows, frame, clip);
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus decal_project_from_view_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  decal_mval_store(C, op, event);
  return decal_project_from_view_exec(C, op);
}

void PAINT_OT_decal_project_from_view(wmOperatorType *ot)
{
  ot->name = "Project Decal from View";
  ot->description =
      "Place the decal's frame through the current viewport view: the centre at the surface under "
      "the mouse, the extents over the object";
  ot->idname = "PAINT_OT_decal_project_from_view";

  ot->invoke = decal_project_from_view_invoke;
  ot->exec = decal_project_from_view_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna, "mouse_x", -1, INT32_MIN, INT32_MAX, "Mouse X", "", -1, INT32_MAX);
  RNA_def_int(ot->srna, "mouse_y", -1, INT32_MIN, INT32_MAX, "Mouse Y", "", -1, INT32_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove / visibility
 * \{ */

static wmOperatorStatus decal_remove_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No decal to remove");
    return OPERATOR_CANCELLED;
  }
  for (MaterialPaintLayer *row : rows) {
    BKE_paint_layers_remove(*ma, row);
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_remove(wmOperatorType *ot)
{
  ot->name = "Remove Decal";
  ot->description = "Remove every row of the decal";
  ot->idname = "PAINT_OT_decal_remove";

  ot->exec = decal_remove_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus decal_visibility_set_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  const bool visible = RNA_boolean_get(op->ptr, "visible");
  for (MaterialPaintLayer *row : rows) {
    BKE_paint_layers_set_enabled(*ma, row, visible);
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_visibility_set(wmOperatorType *ot)
{
  ot->name = "Decal Visibility";
  ot->description = "Show or hide every row of the decal";
  ot->idname = "PAINT_OT_decal_visibility_set";

  ot->exec = decal_visibility_set_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "visible", true, "Visible", "Show the decal's rows");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move block
 * \{ */

enum eDecalMoveDirection {
  DECAL_MOVE_UP = 0,
  DECAL_MOVE_DOWN = 1,
};

static const EnumPropertyItem decal_move_direction_items[] = {
    {DECAL_MOVE_UP, "UP", 0, "Up", "Move the decal one row up the stack"},
    {DECAL_MOVE_DOWN, "DOWN", 0, "Down", "Move the decal one row down the stack"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus decal_move_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  /* The stack list runs bottom-to-top, and rows are collected in that order: rows.last() is the
   * decal's highest row. The block moves as one by moving the single neighbour row over it. */
  const int direction = RNA_enum_get(op->ptr, "direction");
  bool moved = false;
  if (direction == DECAL_MOVE_UP) {
    if (MaterialPaintLayer *above = rows.last()->next) {
      moved = BKE_paint_layers_edit_move(*ma, *above, rows[0], PaintLayerPlace::Below);
    }
  }
  else {
    if (MaterialPaintLayer *below = rows[0]->prev) {
      moved = BKE_paint_layers_edit_move(*ma, *below, rows.last(), PaintLayerPlace::Above);
    }
  }
  if (!moved) {
    return OPERATOR_CANCELLED;
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_move(wmOperatorType *ot)
{
  ot->name = "Move Decal";
  ot->description = "Move the decal one row up or down the stack, as a block";
  ot->idname = "PAINT_OT_decal_move";

  ot->exec = decal_move_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "direction",
               decal_move_direction_items,
               DECAL_MOVE_UP,
               "Direction",
               "Where to move the decal");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend mode
 * \{ */

static const EnumPropertyItem decal_blend_items[] = {
    {MA_PAINT_LAYER_BLEND_MIX, "MIX", 0, "Mix", "Mix blend"},
    {MA_PAINT_LAYER_BLEND_MULTIPLY, "MULTIPLY", 0, "Multiply", "Multiply blend"},
    {MA_PAINT_LAYER_BLEND_OVERLAY, "OVERLAY", 0, "Overlay", "Overlay blend"},
    {MA_PAINT_LAYER_BLEND_ADD, "ADD", 0, "Add", "Add blend"},
    {MA_PAINT_LAYER_BLEND_DARKEN, "DARKEN", 0, "Darken", "Darken blend"},
    {MA_PAINT_LAYER_BLEND_BURN, "BURN", 0, "Color Burn", "Color burn blend"},
    {MA_PAINT_LAYER_BLEND_LIGHTEN, "LIGHTEN", 0, "Lighten", "Lighten blend"},
    {MA_PAINT_LAYER_BLEND_SCREEN, "SCREEN", 0, "Screen", "Screen blend"},
    {MA_PAINT_LAYER_BLEND_DODGE, "DODGE", 0, "Color Dodge", "Color dodge blend"},
    {MA_PAINT_LAYER_BLEND_SUBTRACT, "SUBTRACT", 0, "Subtract", "Subtract blend"},
    {MA_PAINT_LAYER_BLEND_DIVIDE, "DIVIDE", 0, "Divide", "Divide blend"},
    {MA_PAINT_LAYER_BLEND_DIFFERENCE, "DIFFERENCE", 0, "Difference", "Difference blend"},
    {MA_PAINT_LAYER_BLEND_EXCLUSION, "EXCLUSION", 0, "Exclusion", "Exclusion blend"},
    {MA_PAINT_LAYER_BLEND_SOFT_LIGHT, "SOFT_LIGHT", 0, "Soft Light", "Soft light blend"},
    {MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, "LINEAR_LIGHT", 0, "Linear Light", "Linear light blend"},
    {MA_PAINT_LAYER_BLEND_HUE, "HUE", 0, "Hue", "Hue blend"},
    {MA_PAINT_LAYER_BLEND_SATURATION, "SATURATION", 0, "Saturation", "Saturation blend"},
    {MA_PAINT_LAYER_BLEND_COLOR, "COLOR", 0, "Color", "Color blend"},
    {MA_PAINT_LAYER_BLEND_VALUE, "VALUE", 0, "Value", "Value blend"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus decal_blend_mode_set_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  const auto blend = eMaterialPaintLayerBlend(RNA_enum_get(op->ptr, "blend"));
  for (MaterialPaintLayer *row : rows) {
    /* The normal stamp keeps its internal combine: the blend modes cover the colour channels. */
    if (!decal_row_is_normal(*row)) {
      BKE_paint_layers_set_blend(*ma, row, blend);
    }
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus decal_blend_mode_set_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "blend")) {
    return decal_blend_mode_set_exec(C, op);
  }
  return WM_operator_props_dialog_popup(C, op, 300);
}

void PAINT_OT_decal_blend_mode_set(wmOperatorType *ot)
{
  ot->name = "Decal Blend Mode";
  ot->description = "Set the blend mode of the decal's colour channels";
  ot->idname = "PAINT_OT_decal_blend_mode_set";

  ot->invoke = decal_blend_mode_set_invoke;
  ot->exec = decal_blend_mode_set_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "blend",
               decal_blend_items,
               MA_PAINT_LAYER_BLEND_MIX,
               "Blend",
               "The blend the decal takes over what is below it");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection space
 * \{ */

static wmOperatorStatus decal_projection_mode_set_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  const int16_t space = int16_t(RNA_enum_get(op->ptr, "space"));
  for (MaterialPaintLayer *row : rows) {
    if (!BKE_paint_layers_projection_set_space(*ma, row, space)) {
      continue;
    }
  }
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus decal_projection_mode_set_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent * /*event*/)
{
  if (RNA_struct_property_is_set(op->ptr, "space")) {
    return decal_projection_mode_set_exec(C, op);
  }
  return WM_operator_props_dialog_popup(C, op, 300);
}

void PAINT_OT_decal_projection_mode_set(wmOperatorType *ot)
{
  ot->name = "Decal Projection Space";
  ot->description = "Set the projection space of every row of the decal";
  ot->idname = "PAINT_OT_decal_projection_mode_set";

  ot->invoke = decal_projection_mode_set_invoke;
  ot->exec = decal_projection_mode_set_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "space",
               decal_space_items,
               MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
               "Space",
               "The space the decal's maps read through");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name World anchor
 *
 * The decal group's frame is handed to a controller Empty at its world place: a material shared
 * by several objects then shows the decal in one world spot, because the shader and the CPU paths
 * both read the frame in the controller's space. Releasing converts the frame back into the
 * active object's space.
 * \{ */

static wmOperatorStatus decal_world_anchor_set_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No decal to anchor");
    return OPERATOR_CANCELLED;
  }
  Object *controller = nullptr;
  if (!decal_group_controller_get(rows, &controller)) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "Decal '%s': its rows disagree about the projection object",
                name);
    return OPERATOR_CANCELLED;
  }

  if (RNA_boolean_get(op->ptr, "attach")) {
    if (controller != nullptr) {
      BKE_report(op->reports, RPT_INFO, "Decal is already anchored to the world");
      return OPERATOR_FINISHED;
    }
    if (!decal_controller_ensure(*C, *ma, *ob, rows, op->reports, nullptr)) {
      return OPERATOR_CANCELLED;
    }
  }
  else {
    if (controller == nullptr) {
      BKE_report(op->reports, RPT_INFO, "Decal is not anchored to the world");
      return OPERATOR_FINISHED;
    }
    if (!decal_controller_detach(
            *C, *ma, *ob, rows, op->reports, RNA_boolean_get(op->ptr, "delete_empty")))
    {
      return OPERATOR_CANCELLED;
    }
  }

  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_world_anchor_set(wmOperatorType *ot)
{
  ot->name = "Anchor Decal to World";
  ot->description =
      "Anchor the decal's frame to the world: a controller Empty takes the frame's place, so a "
      "material shared by several objects shows the decal in one world spot; with Attach off, the "
      "frame is converted back into the active object's space";
  ot->idname = "PAINT_OT_decal_world_anchor_set";

  ot->exec = decal_world_anchor_set_exec;
  ot->poll = decal_target_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "attach",
                  true,
                  "Attach",
                  "Give the decal's rows a controller Empty at the frame's world place");
  RNA_def_boolean(ot->srna,
                  "delete_empty",
                  false,
                  "Delete Empty",
                  "When releasing, delete the controller Empty if nothing else uses it");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Propagate to selected
 *
 * The decal group is copied into the layered material of every other selected object: the copies
 * either keep the source's controller (one edit moves them all) or take the frame converted into
 * each object's own space. Every refusal is a warning naming the object, the rest goes on.
 * \{ */

static const EnumPropertyItem decal_propagate_mode_items[] = {
    {int(DecalCopyMode::LinkController),
     "LINK_CONTROLLER",
     0,
     "Link Controller",
     "Copy the rows with the source's controller, so one frame edit moves the decal everywhere"},
    {int(DecalCopyMode::ConvertFrame),
     "CONVERT_FRAME",
     0,
     "Convert Frame",
     "Copy the rows without a controller; each frame is converted into the target object's space"},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool decal_propagate_poll(bContext *C)
{
  if (!decal_target_poll(C)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  Vector<Object *> candidates;
  decal_propagation_candidates(*C, *ob, candidates);
  return !candidates.is_empty();
}

static wmOperatorStatus decal_propagate_exec(bContext *C, wmOperator *op)
{
  Material *src_ma = nullptr;
  Object *src_ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &src_ma, &src_ob, name, &rows) || rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No decal to propagate");
    return OPERATOR_CANCELLED;
  }
  const auto mode = DecalCopyMode(RNA_enum_get(op->ptr, "mode"));

  Object *controller = nullptr;
  if (!decal_group_controller_get(rows, &controller)) {
    BKE_reportf(
        op->reports, RPT_ERROR, "Decal '%s': its rows disagree about the projection object", name);
    return OPERATOR_CANCELLED;
  }

  /* Pick and report the receivers before touching anything: anchoring the source and then
   * cancelling would leave a new Empty behind an operator that says it did nothing. */
  Vector<Object *> candidates;
  decal_propagation_candidates(*C, *src_ob, candidates);
  if (candidates.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No other selected object to propagate the decal to");
    return OPERATOR_CANCELLED;
  }

  struct Receiver {
    Object *ob;
    Material *ma;
  };
  Vector<Receiver> receivers;
  int skipped = 0;
  Set<Material *> taken;
  for (Object *dst_ob : candidates) {
    Material *dst_ma = BKE_paint_layers_active_material_get(dst_ob);
    if (dst_ma == nullptr) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Decal: object '%s' skipped -- it has no material",
                  dst_ob->id.name + 2);
      skipped++;
      continue;
    }
    if (!BKE_paint_layers_is_layered(*dst_ma) || !ID_IS_EDITABLE(&dst_ma->id)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Decal: object '%s' skipped -- material '%s' is not an editable layered material",
                  dst_ob->id.name + 2,
                  dst_ma->id.name + 2);
      skipped++;
      continue;
    }
    if (dst_ma == src_ma) {
      /* The group is already there; no warning, the user just sees it counted. */
      skipped++;
      continue;
    }
    if (taken.contains(dst_ma)) {
      if (mode == DecalCopyMode::ConvertFrame) {
        /* The frame is one per row of the material: the conversion can only fit the first
         * object it was taken for. */
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Decal: object '%s' skipped -- material '%s' is shared; the frame is "
                    "converted for another object",
                    dst_ob->id.name + 2,
                    dst_ma->id.name + 2);
      }
      skipped++;
      continue;
    }
    Vector<MaterialPaintLayer *> existing;
    decal_rows_find(*dst_ma, name, existing);
    if (!existing.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Decal: object '%s' skipped -- material '%s' already has decal '%s'",
                  dst_ob->id.name + 2,
                  dst_ma->id.name + 2,
                  name);
      skipped++;
      continue;
    }
    taken.add(dst_ma);
    receivers.append(Receiver{dst_ob, dst_ma});
  }
  if (receivers.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No target material to propagate the decal into");
    return OPERATOR_CANCELLED;
  }

  /* A linked copy lives or dies with one controller: the source group is anchored first, then
   * every copy references the same Empty. */
  if (mode == DecalCopyMode::LinkController && controller == nullptr) {
    if (!decal_controller_ensure(*C, *src_ma, *src_ob, rows, op->reports, nullptr)) {
      return OPERATOR_CANCELLED;
    }
    decal_done_notify(C, *src_ma);
  }

  int propagated = 0;
  for (const Receiver &receiver : receivers) {
    Vector<MaterialPaintLayer *> copies;
    decal_rows_copy(*CTX_data_main(C),
                    *C,
                    *src_ma,
                    *src_ob,
                    *receiver.ma,
                    *receiver.ob,
                    rows,
                    mode,
                    op->reports,
                    &copies);
    if (copies.is_empty()) {
      skipped++;
      continue;
    }
    propagated++;
    /* One notifier per touched material: the panels and the viewport refresh from it. */
    decal_done_notify(C, *receiver.ma);
  }

  BKE_reportf(
      op->reports, RPT_INFO, "Decal: propagated to %d object(s), skipped %d", propagated, skipped);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_propagate_to_selected(wmOperatorType *ot)
{
  ot->name = "Propagate Decal to Selected";
  ot->description =
      "Copy the decal into the layered material of every other selected object: the copies share "
      "the decal's image, with the source's controller or with the frame converted into each "
      "object's space";
  ot->idname = "PAINT_OT_decal_propagate_to_selected";

  ot->exec = decal_propagate_exec;
  ot->poll = decal_propagate_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "mode",
               decal_propagate_mode_items,
               int(DecalCopyMode::LinkController),
               "Mode",
               "How the copied frame relates to the source's");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name D6: decal from the live selection-cage frame
 * \{ */

/**
 * True only when there is an active 3D transform session with a live frame: the same condition
 * that makes the selection cage visible in the viewport.
 */
/* TODO(S6): the live selection-cage transform session hands its destination frame over through
 * `image_select_transform3d_projection_frame_get`, which the Selection Mask patch (S6) defines in
 * `paint_image_select_view3d.hh`. Until that patch is applied there is no such session to read, so
 * the operator below stays unavailable. Remove this placeholder together with that patch's
 * declaration landing. */
static bool image_select_transform3d_projection_frame_get(const bContext * /*C*/,
                                                          projection::ProjectionFrame & /*r_frame*/)
{
  return false;
}

static bool decal_from_selection_poll(bContext *C)
{
  if (!decal_target_poll(C)) {
    return false;
  }
  projection::ProjectionFrame frame;
  return image_select_transform3d_projection_frame_get(C, frame);
}

static wmOperatorStatus decal_from_selection_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  Object *ob = nullptr;
  char name[64];
  Vector<MaterialPaintLayer *> rows;
  if (!decal_target_find(*C, &ma, &ob, name, &rows) || rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No decal to place");
    return OPERATOR_CANCELLED;
  }
  projection::ProjectionFrame frame;
  if (!image_select_transform3d_projection_frame_get(C, frame)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "No live selection-cage transform: use the Selection tool in the 3D Viewport first");
    return OPERATOR_CANCELLED;
  }
  /* The clip setting belongs to the rows, not to the frame: carry it over. */
  const bool clip = (rows[0]->projection != nullptr) ?
                        BKE_paint_layers_projection_clip_get(*rows[0]) :
                        true;
  decal_frame_apply(*ma, rows, frame, clip);
  decal_done_notify(C, *ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_decal_from_selection(wmOperatorType *ot)
{
  ot->name = "Decal from Selection";
  ot->description =
      "Place the decal's frame from the live selection-cage transform: the cage's frame becomes "
      "the projection frame, so the decal lands exactly where the selection cage sits";
  ot->idname = "PAINT_OT_decal_from_selection";

  ot->exec = decal_from_selection_exec;
  ot->poll = decal_from_selection_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void operatortypes_register()
{
  WM_operatortype_append(PAINT_OT_decal_add);
  WM_operatortype_append(PAINT_OT_decal_add_image);
  WM_operatortype_append(PAINT_OT_decal_project_from_view);
  WM_operatortype_append(PAINT_OT_decal_remove);
  WM_operatortype_append(PAINT_OT_decal_visibility_set);
  WM_operatortype_append(PAINT_OT_decal_move);
  WM_operatortype_append(PAINT_OT_decal_blend_mode_set);
  WM_operatortype_append(PAINT_OT_decal_projection_mode_set);
  WM_operatortype_append(PAINT_OT_decal_mask_add);
  WM_operatortype_append(PAINT_OT_decal_mask_remove);
  WM_operatortype_append(PAINT_OT_decal_from_selection);
  WM_operatortype_append(PAINT_OT_decal_world_anchor_set);
  WM_operatortype_append(PAINT_OT_decal_propagate_to_selected);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::decal
