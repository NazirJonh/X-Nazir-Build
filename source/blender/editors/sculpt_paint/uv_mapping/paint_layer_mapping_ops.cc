/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_layer_mapping_ops.hh"

#include <algorithm>
#include <cfloat>
#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_assert.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"
#include "BKE_context.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "ED_material_combined.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../mesh/paint_image_uv_symmetry.hh"
#include "uv_mapping_target.hh"

namespace blender::ed::sculpt_paint::uv_mapping {

/* -------------------------------------------------------------------- */
/** \name Scale Lock Toggle
 * \{ */

/** Toggle the scale lock of the row the Mapping panel and the gizmo edit. */
static bool layer_mapping_scale_lock_toggle_poll(bContext *C)
{
  PaintLayerRowMappingTarget target;
  return target.resolve(*C);
}

static wmOperatorStatus layer_mapping_scale_lock_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  PaintLayerRowMappingTarget target;
  if (!target.resolve(*C)) {
    return OPERATOR_CANCELLED;
  }
  if (!target.set_scale_locked(!target.scale_locked())) {
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, target.material());
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_scale_lock_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Layer Mapping Scale Lock";
  ot->idname = "PAINT_OT_layer_mapping_scale_lock_toggle";
  ot->description = "Lock or unlock the scale axes of the shown layer row's UV mapping";
  ot->exec = layer_mapping_scale_lock_toggle_exec;
  ot->poll = layer_mapping_scale_lock_toggle_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reset Layer Mapping
 *
 * The Mapping panel's reset: the mapping values of the shown row go back to their defaults, all
 * at once or one part at a time. Scale and Rotation hold the tile's centre on the canvas while
 * they put the size and the angle back, the way the panel's fields and the gizmo do. The offset
 * is the tile's position itself, so resetting it -- or everything at once -- moves the tile back
 * to the UV origin. The enabled flag and the UV wrap are modes, not values, so no reset touches
 * them.
 * \{ */

/** Which parts of the shown row's mapping the reset puts back. */
enum LayerMappingResetMode {
  LAYER_MAPPING_RESET_ALL = 0,
  LAYER_MAPPING_RESET_OFFSET = 1,
  LAYER_MAPPING_RESET_SCALE = 2,
  LAYER_MAPPING_RESET_ROTATION = 3,
};

static const EnumPropertyItem layer_mapping_reset_items[] = {
    {LAYER_MAPPING_RESET_ALL,
     "ALL",
     0,
     "Reset All",
     "Put the offset, the scale and the rotation back to their defaults; the tile returns to the "
     "UV origin"},
    {LAYER_MAPPING_RESET_OFFSET,
     "OFFSET",
     0,
     "Reset Offset",
     "Move the tile back to the UV origin"},
    {LAYER_MAPPING_RESET_SCALE,
     "SCALE",
     0,
     "Reset Scale",
     "Size the tile back to one, keeping its centre on the canvas"},
    {LAYER_MAPPING_RESET_ROTATION,
     "ROTATION",
     0,
     "Reset Rotation",
     "Turn the tile back upright around its centre"},
    {0, nullptr, 0, nullptr, nullptr},
};

struct LayerMappingResetTarget {
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
};

/** The row the Mapping panel edits: the reset answers with it, so it is enabled with the panel. */
static std::optional<LayerMappingResetTarget> layer_mapping_reset_target(bContext *C)
{
  Material *ma = BKE_paint_layers_active_material_get(CTX_data_active_object(C));
  MaterialPaintLayer *row = (ma != nullptr) ? mapping_shown_row_get(*C, *ma) : nullptr;
  if (row == nullptr || !BKE_paint_layers_mapping_available(*ma, *row)) {
    return std::nullopt;
  }
  return LayerMappingResetTarget{ma, row};
}

static bool layer_mapping_reset_poll(bContext *C)
{
  return layer_mapping_reset_target(C).has_value();
}

static wmOperatorStatus layer_mapping_reset_exec(bContext *C, wmOperator *op)
{
  const std::optional<LayerMappingResetTarget> target = layer_mapping_reset_target(C);
  if (!target.has_value()) {
    BKE_report(op->reports, RPT_ERROR, "The shown layer row takes no UV mapping");
    return OPERATOR_CANCELLED;
  }
  Material &ma = *target->ma;
  MaterialPaintLayer &row = *target->row;

  const float zero_offset[2] = {0.0f, 0.0f};
  const float unit_scale[2] = {1.0f, 1.0f};
  switch (RNA_enum_get(op->ptr, "mode")) {
    case LAYER_MAPPING_RESET_OFFSET:
      /* The offset is the tile's position itself: putting it back is what moves the tile home. */
      BKE_paint_layers_mapping_set_offset(ma, &row, zero_offset);
      break;
    case LAYER_MAPPING_RESET_SCALE:
      BKE_paint_layers_mapping_set_scale_holding_tile(ma, &row, unit_scale);
      break;
    case LAYER_MAPPING_RESET_ROTATION:
      BKE_paint_layers_mapping_set_rotation_holding_tile(ma, &row, 0.0f);
      break;
    case LAYER_MAPPING_RESET_ALL:
      /* One raw call for all: the defaults are the UV origin, so the tile returns there, and the
       * setters normalize and follow the scale lock the same way the panel's fields do. */
      BKE_paint_layers_mapping_set_values(ma, &row, zero_offset, unit_scale, 0.0f);
      break;
    default:
      BLI_assert_unreachable();
      return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma);
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_reset(wmOperatorType *ot)
{
  ot->name = "Reset Layer Mapping";
  ot->idname = "PAINT_OT_layer_mapping_reset";
  ot->description =
      "Put the shown layer row's UV mapping values back to their defaults, all at once or one "
      "part at a time";
  ot->exec = layer_mapping_reset_exec;
  ot->poll = layer_mapping_reset_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna,
                          "mode",
                          layer_mapping_reset_items,
                          LAYER_MAPPING_RESET_ALL,
                          "Mode",
                          "Which part of the mapping to reset");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layer Mapping Mirror Point
 * \{ */

/**
 * One Mirror center placement session: the target stores the material and the row's marker, not
 * a pointer, so it survives the rebuilds a value edit may cause. The center is written value-only
 * on every mouse move; one undo step closes the gesture.
 */
struct LayerMappingMirrorPointData {
  PaintLayerRowMappingTarget target;
  float start_center[2] = {0.5f, 0.5f};
  bool changed = false;
};

/**
 * The row marker a panel button passes in \a op, or a nil marker for "the row the context shows".
 */
bUUID layer_mapping_mirror_marker_read(wmOperator *op)
{
  char marker_str[UUID_STRING_SIZE] = "";
  RNA_string_get(op->ptr, "marker", marker_str);
  bUUID marker;
  if (marker_str[0] != '\0' && BLI_uuid_parse_string(&marker, marker_str)) {
    return marker;
  }
  return {};
}

void layer_mapping_mirror_marker_define(wmOperatorType *ot)
{
  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "marker",
                                     nullptr,
                                     UUID_STRING_SIZE,
                                     "Marker",
                                     "Stable identity of the row the panel drew this for");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

bool layer_mapping_mirror_target_poll(bContext *C, const bool need_image_editor)
{
  PaintLayerRowMappingTarget target;
  return need_image_editor ? target.resolve(*C) : target.resolve_row(*C, {});
}

/** Whichever mapped row the context shows: what a keymap entry or a menu gets without a marker. */
static bool layer_mapping_mirror_any_poll(bContext *C)
{
  return layer_mapping_mirror_target_poll(C, false);
}

static bool layer_mapping_mirror_set_point_poll(bContext *C)
{
  return layer_mapping_mirror_target_poll(C, true);
}

static void layer_mapping_mirror_point_status(bContext *C, const float center[2], const bool pick)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    char text[256];
    BLI_snprintf(text,
                 sizeof(text),
                 pick ? IFACE_("Click the mirror point in the image    %.4f  %.4f") :
                        IFACE_("Mirror Center %.4f  %.4f"),
                 center[0],
                 center[1]);
    ED_area_status_text(area, text);
  }
}

static void layer_mapping_mirror_point_finish(bContext *C,
                                              wmOperator *op,
                                              const bool commit)
{
  auto *data = static_cast<LayerMappingMirrorPointData *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  Material *material = data->target.material();
  if (material != nullptr) {
    ed::material_combined::gradient_interactive_end(material->id.session_uid);
  }
  if (!commit) {
    data->target.write_mirror_center(float2(data->start_center[0], data->start_center[1]));
  }
  else if (data->changed) {
    ED_undo_push(C, data->target.mirror_undo_name());
  }
  if (material != nullptr) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  }
  if (region != nullptr) {
    ED_region_tag_redraw(region);
  }
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
  MEM_delete(data);
  op->customdata = nullptr;
}

static wmOperatorStatus layer_mapping_mirror_set_point_invoke(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent * /*event*/)
{
  PaintLayerRowMappingTarget target;
  /* A panel button names its row; a keymap entry follows the Image Editor. Either way the click is
   * read in a 2D view, so an area without one has to say so rather than place a wrong point. */
  const bUUID marker = layer_mapping_mirror_marker_read(op);
  if (!(BLI_uuid_is_nil(marker) ? target.resolve(*C) : target.resolve_row(*C, marker))) {
    BKE_report(op->reports, RPT_WARNING, "Select a mapped layer row first");
    return OPERATOR_CANCELLED;
  }
  if (CTX_wm_region(C) == nullptr || CTX_wm_space_image(C) == nullptr) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Run this from the Image Editor, where the click has an image to land on");
    return OPERATOR_CANCELLED;
  }
  float center[2];
  if (!target.read_mirror_center(center)) {
    return OPERATOR_CANCELLED;
  }
  auto *data = MEM_new<LayerMappingMirrorPointData>(__func__);
  data->target = target;
  copy_v2_v2(data->start_center, center);
  op->customdata = data;

  layer_mapping_mirror_point_status(C, center, true);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus layer_mapping_mirror_set_point_modal(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  auto *data = static_cast<LayerMappingMirrorPointData *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  if (data == nullptr || region == nullptr || event == nullptr) {
    /* Nothing to read the gesture from: close the session instead of leaving its data behind. */
    if (data != nullptr) {
      layer_mapping_mirror_point_finish(C, op, false);
    }
    return OPERATOR_CANCELLED;
  }
  if (event->type == EVT_ESCKEY || (event->type == RIGHTMOUSE && event->val == KM_PRESS)) {
    layer_mapping_mirror_point_finish(C, op, false);
    return OPERATOR_CANCELLED;
  }
  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    layer_mapping_mirror_point_finish(C, op, true);
    return OPERATOR_FINISHED;
  }
  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  float center[2];
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &center[0], &center[1]);
  if ((event->modifier & KM_SHIFT) != 0) {
    /* Shift reads the motion at a tenth, like the gizmo's precision drag. */
    center[0] = data->start_center[0] + (center[0] - data->start_center[0]) * 0.1f;
    center[1] = data->start_center[1] + (center[1] - data->start_center[1]) * 0.1f;
  }
  /* The center is a canvas UV point; keep it inside the tile the fold is authored for. */
  center[0] = std::clamp(center[0], 0.0f, 1.0f);
  center[1] = std::clamp(center[1], 0.0f, 1.0f);
  /* Marked before the write: the first bake after the value changes must already see the drag. */
  if (Material *material = data->target.material()) {
    /* Coarse composites while the point follows the cursor; the finish clears the mark. */
    ed::material_combined::gradient_interactive_begin(*material, true);
  }
  if (!data->target.write_mirror_center(float2(center[0], center[1]))) {
    layer_mapping_mirror_point_finish(C, op, false);
    return OPERATOR_CANCELLED;
  }
  data->changed = true;
  layer_mapping_mirror_point_status(C, center, false);
  if (Material *material = data->target.material()) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  }
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

/** The window manager can abort the gesture outside the modal handler: close the session then. */
static void layer_mapping_mirror_set_point_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    layer_mapping_mirror_point_finish(C, op, false);
  }
}

static wmOperatorStatus layer_mapping_mirror_set_point_exec(bContext *C, wmOperator *op)
{
  PaintLayerRowMappingTarget target;
  const bUUID marker = layer_mapping_mirror_marker_read(op);
  if (!target.resolve_row(*C, marker)) {
    BKE_report(op->reports, RPT_WARNING, "Select a mapped layer row first");
    return OPERATOR_CANCELLED;
  }
  float center[2];
  RNA_float_get_array(op->ptr, "center", center);
  if (!target.write_mirror_center(float2(center[0], center[1]))) {
    return OPERATOR_CANCELLED;
  }
  if (Material *material = target.material()) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_mirror_set_point(wmOperatorType *ot)
{
  ot->name = "Set Layer Mirror Point";
  ot->idname = "PAINT_OT_layer_mapping_mirror_set_point";
  ot->description =
      "Place the shown layer row's Mirror fold center by clicking the image, or at the given UV";

  ot->invoke = layer_mapping_mirror_set_point_invoke;
  ot->modal = layer_mapping_mirror_set_point_modal;
  ot->exec = layer_mapping_mirror_set_point_exec;
  ot->cancel = layer_mapping_mirror_set_point_cancel;
  ot->poll = layer_mapping_mirror_set_point_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  layer_mapping_mirror_marker_define(ot);

  RNA_def_float_vector(ot->srna,
                       "center",
                       2,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Center",
                       "The UV point the Mirror fold reflects around",
                       0.0f,
                       1.0f);
}

/**
 * The row's Mirror fold from the Outliner row button: on turns both axes on (the row button is the
 * one-click symmetry), off turns both off. Per-axis control stays in the Mapping panel, where the
 * center and each axis have their own controls.
 */
static MaterialPaintLayer *layer_mapping_mirror_toggle_row(bContext *C,
                                                           wmOperator *op,
                                                           Material **r_ma)
{
  Material *ma = BKE_paint_layers_active_material_get(CTX_data_active_object(C));
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return nullptr;
  }
  const bUUID marker = layer_mapping_mirror_marker_read(op);
  MaterialPaintLayer *layer = BLI_uuid_is_nil(marker) ? mapping_shown_row_get(*C, *ma) :
                                                        BKE_paint_layers_find(*ma, marker);
  if (layer == nullptr || !BKE_paint_layers_mapping_mirror_available(*ma, *layer)) {
    return nullptr;
  }
  *r_ma = ma;
  return layer;
}

static bool layer_mapping_mirror_toggle_poll(bContext *C)
{
  Material *ma = BKE_paint_layers_active_material_get(CTX_data_active_object(C));
  return ma != nullptr && BKE_paint_layers_is_layered(*ma);
}

static wmOperatorStatus layer_mapping_mirror_toggle_exec(bContext *C, wmOperator *op)
{
  Material *ma = nullptr;
  MaterialPaintLayer *layer = layer_mapping_mirror_toggle_row(C, op, &ma);
  if (layer == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int16_t axes = (BKE_paint_layers_mapping_mirror_get(*layer) != 0) ?
                           int16_t(0) :
                           /* The stored directions stay, so the toggle never loses a chosen "-". */
                           int16_t(MA_PAINT_LAYER_MAPPING_MIRROR_U |
                                   MA_PAINT_LAYER_MAPPING_MIRROR_V |
                                   (BKE_paint_layers_mapping_mirror_axes_get(*layer) &
                                    (MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE |
                                     MA_PAINT_LAYER_MAPPING_MIRROR_V_NEGATIVE)));
  if (axes != 0 && !BKE_paint_layers_mapping_applies(*ma, *layer)) {
    /* One click is the whole gesture: a row whose mapping is off would otherwise switch the fold on
     * invisibly. The fold implies the mapping, so turning it on is part of the toggle. */
    if (!BKE_paint_layers_mapping_set_enabled(*ma, layer, true)) {
      return OPERATOR_CANCELLED;
    }
  }
  if (!BKE_paint_layers_mapping_set_mirror(*ma, layer, axes)) {
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_mirror_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Layer Mirror";
  ot->idname = "PAINT_OT_layer_mapping_mirror_toggle";
  ot->description = "Switch the layer row's Mirror fold on or off, across both axes";
  ot->exec = layer_mapping_mirror_toggle_exec;
  ot->poll = layer_mapping_mirror_toggle_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  layer_mapping_mirror_marker_define(ot);
}

/**
 * The Mirror center from the sculpt 3D cursor: its object-space point is projected onto the active
 * object's nearest surface and the active UV map's coordinate there becomes the center. This is the
 * bridge the 3D Sculpt Gizmo needs -- the cursor is authored in 3D, the row's fold lives in UV.
 */
static wmOperatorStatus layer_mapping_mirror_from_cursor_exec(bContext *C, wmOperator *op)
{
  PaintLayerRowMappingTarget target;
  const bUUID marker = layer_mapping_mirror_marker_read(op);
  if (!target.resolve_row(*C, marker)) {
    BKE_report(op->reports, RPT_WARNING, "Select a mapped layer row first");
    return OPERATOR_CANCELLED;
  }
  Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (ob == nullptr || scene == nullptr || ob->type != OB_MESH) {
    return OPERATOR_CANCELLED;
  }
  const ed::sculpt_paint::cursor::CursorState state =
      ed::sculpt_paint::cursor::state_get(*scene, *ob);
  float uv[2];
  if (!image_paint_symmetry_uv_at_position(ob, state.location, uv)) {
    BKE_report(op->reports,
               RPT_WARNING,
               "The sculpt cursor is not over the active object's UV-mapped surface");
    return OPERATOR_CANCELLED;
  }
  if (!target.write_mirror_center(float2(uv[0], uv[1]))) {
    return OPERATOR_CANCELLED;
  }
  if (Material *material = target.material()) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_layer_mapping_mirror_from_cursor(wmOperatorType *ot)
{
  ot->name = "Mirror Point from Sculpt Cursor";
  ot->idname = "PAINT_OT_layer_mapping_mirror_from_cursor";
  ot->description =
      "Set the shown layer row's Mirror fold center from the sculpt 3D cursor, projected to UV";

  ot->exec = layer_mapping_mirror_from_cursor_exec;
  ot->poll = layer_mapping_mirror_any_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  layer_mapping_mirror_marker_define(ot);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::uv_mapping
