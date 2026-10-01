/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Pixel mode of the Image Editor shape drawing tools ("Paint Shape").
 *
 * One modal operator draws a Rectangle or an Ellipse directly into the texture in a single
 * action (parameters are set up front, the release of the drag bakes everything in one undo
 * step). Shift snaps (square, circle), Alt grows the shape from its center, holding Space moves
 * the in-progress shape, and a plain click draws the default size from the tool settings at the
 * click point.
 *
 * The in-progress outline is drawn as an animated dashed overlay while the drag runs. `exec`
 * replays the stored shape and style properties (Python and Redo/F9: the redo panel exposes
 * colors, width and opacities on top of the stored style); the profile and ramp tables and the
 * PBR channel values follow the current settings.
 */

#include <algorithm>
#include <cmath>
#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "../paint_shape_create.hh"
#include "../paint_shape_draw.hh"
#include "../paint_shape_op_props.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_shape.hh"
#include "paint_image_shape_composite.hh"
#include "../paint_shape_raster.hh"

namespace blender {

namespace shape = ed::sculpt_paint::shape;

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Flattening error (canvas pixels) of the drag preview outline. */
constexpr float SHAPE_PREVIEW_ERROR_PX = 2.0f;

/**
 * Undo-step name of the Pixel bake. It must stay equal to #PAINT_OT_image_shape_draw's
 * `ot->name`: the operator deliberately carries no OPTYPE_UNDO, so the compositor's own image
 * undo step is the only step of the bake, and the redo machinery finds it by the operator name.
 */
constexpr char SHAPE_UNDO_NAME[] = "Paint Shape";

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coordinate helpers
 * \{ */

static float2 image_shape_event_uv(const ARegion *region, const wmEvent *event)
{
  float uv[2];
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv[0], &uv[1]);
  return float2(uv[0], uv[1]);
}

/** Shape-space (reference-tile) pixel position of \a event for shapes anchored at \a ref_tile. */
static float2 image_shape_event_to_px(const ARegion *region,
                                      const wmEvent *event,
                                      const int ref_tile,
                                      const int2 ref_tile_size)
{
  const float2 uv = image_shape_event_uv(region, event);
  return (uv - shape::tile_uv_origin(ref_tile)) * float2(ref_tile_size);
}

/** Shape-space pixels to Image Editor region pixels, for the overlay. */
static float2 image_shape_px_to_region(const ARegion *region,
                                       const shape::CanvasTile &tile,
                                       const float2 &px)
{
  const float2 uv = shape::shape_px_to_uv(tile, px);
  float2 region_px;
  ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &region_px.x, &region_px.y);
  return region_px;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator state
 * \{ */

struct ImageShapeState {
  SpaceImage *owner_sima = nullptr;
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;

  /** Reference tile (and its pixel size) the shape coordinates refer to. */
  int ref_tile = 1001;
  int2 ref_tile_size = int2(1024);
  shape::CanvasTile canvas_tile() const
  {
    return {ref_tile, ref_tile_size};
  }

  /** The shared creation machine (drag). */
  std::optional<shape::ShapeCreateGesture> create;
};

static void image_shape_state_free(bContext *C, wmOperator *op)
{
  ImageShapeState *state = static_cast<ImageShapeState *>(op->customdata);
  if (state == nullptr) {
    return;
  }
  if (state->draw_handle && state->owner_region_type) {
    ED_region_draw_cb_exit(state->owner_region_type, state->draw_handle);
    state->draw_handle = nullptr;
  }
  MEM_delete(state);
  op->customdata = nullptr;
  if (wmWindow *win = CTX_wm_window(C)) {
    WM_cursor_modal_restore(win);
  }
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

static void image_shape_draw_drag(const bContext *C, ARegion *region, void *arg)
{
  ImageShapeState *state = static_cast<ImageShapeState *>(arg);
  if (state == nullptr || !state->create || !state->create->has_shape()) {
    return;
  }
  if (CTX_wm_space_image(C) != state->owner_sima) {
    return;
  }
  /* The invoking press builds a degenerate shape at the press point; wait for the first real move
   * so no spurious outline (a dash from the region corner) flashes before the drag starts. */
  if (!state->create->moved()) {
    return;
  }

  const shape::PaintShape &shape = state->create->shape();
  const shape::CanvasTile tile = state->canvas_tile();
  /* While F adjusts the width, the stroke edges are drawn too, so the band is visible. */
  const bool show_width = state->create->value_drag_active() &&
                          state->create->value_drag_is_width();
  shape::shape_draw_creation_preview(
      shape,
      SHAPE_PREVIEW_ERROR_PX,
      [&](const float2 &p) { return image_shape_px_to_region(region, tile, p); },
      show_width ? &state->create->style() : nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit
 * \{ */

/** Bake the current shape and finish the operator. */
static wmOperatorStatus image_shape_apply(bContext *C, wmOperator *op, ImageShapeState *state)
{
  shape::ShapeCreateGesture &create = *state->create;
  const shape::PaintShape shape = create.shape();
  if (shape.is_empty()) {
    image_shape_state_free(C, op);
    return OPERATOR_CANCELLED;
  }

  shape::shape_to_op_props(op, shape);
  /* Read before the state is freed below. */
  const shape::CanvasTile tile = state->canvas_tile();
  shape::canvas_tile_to_op_props(op, tile);
  const shape::ShapeStyle style = create.style();
  shape::style_to_op_props(op, style);
  image_shape_state_free(C, op);

  const Vector<shape::PaintShape> shapes = {shape};
  if (shape::shape_bake(C, shapes, tile, style, true, SHAPE_UNDO_NAME)) {
    return OPERATOR_FINISHED;
  }
  /* Material mode with no writable map: say why instead of silently doing nothing. */
  if (const char *reason = shape::shape_targets_refusal_message(C, CTX_data_active_object(C))) {
    BKE_report(op->reports, RPT_WARNING, reason);
  }
  return OPERATOR_CANCELLED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator callbacks
 * \{ */

static bool image_shape_draw_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima) {
    return false;
  }
  /* Mode, editability, region and "another tool is floating" checks come from the shared
   * selection poll: the same constraints apply to drawing shapes. */
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  return (sima->image != nullptr);
}

static bool image_shape_tile_size_get(Image *image, const int tile_number, int2 &r_size)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf) {
    return false;
  }
  r_size = int2(ibuf->x, ibuf->y);
  BKE_image_release_ibuf(image, ibuf, lock);
  return r_size.x > 0 && r_size.y > 0;
}

static wmOperatorStatus image_shape_draw_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  Scene *scene = CTX_data_scene(C);
  if (!sima || !sima->image || !region || !scene || !scene->toolsettings) {
    return OPERATOR_CANCELLED;
  }

  auto *state = MEM_new<ImageShapeState>(__func__);
  state->owner_sima = sima;
  shape::ShapeStyle style = shape::style_from_settings(
      BKE_paint_shape_settings_get(*scene->toolsettings));
  shape::style_brush_values_from_brush(scene->toolsettings->imapaint.paint, style);

  /* The tool's type property overrides the settings; -1 means "use the settings' type". */
  int type = RNA_enum_get(op->ptr, "type");
  if (type < 0) {
    type = BKE_paint_shape_settings_get(*scene->toolsettings).type;
  }

  /* The one map-creation call of the draw: the PBR channel images are created here instead of
   * inside the bake, which runs outside any undo step that would record the creation (the
   * brush's stroke start resolves its targets the same ensure-free way). */
  if (ToolSettings *toolsettings = scene->toolsettings) {
    shape::composite_targets_ensure_writable(C,
                                             CTX_data_active_object(C),
                                             toolsettings->imapaint.paint,
                                             BKE_paint_shape_settings_get(*toolsettings));
  }

  const float2 uv = image_shape_event_uv(region, event);
  /* UDIM columns wrap every 10 tiles; clamp so UV x >= 10 stays on the last column. */
  const int tile_column = std::clamp(int(math::floor(uv.x)), 0, 9);
  const int tile_row = std::max(int(math::floor(uv.y)), 0);
  state->ref_tile = 1001 + 10 * tile_row + tile_column;
  if (!image_shape_tile_size_get(sima->image, state->ref_tile, state->ref_tile_size)) {
    /* Fall back to the primary tile. */
    state->ref_tile = 1001;
    if (!image_shape_tile_size_get(sima->image, state->ref_tile, state->ref_tile_size)) {
      MEM_delete(state);
      return OPERATOR_CANCELLED;
    }
  }

  state->create.emplace(ePaintShapeType(type), style);
  const float2 start_uv = image_shape_event_uv(region, event);
  const float2 screen_start = float2(event->mval[0], event->mval[1]);
  float screen_x_uv[2], screen_y_uv[2];
  ui::view2d_region_to_view(&region->v2d,
                            screen_start.x + 1.0f,
                            screen_start.y,
                            &screen_x_uv[0],
                            &screen_x_uv[1]);
  ui::view2d_region_to_view(&region->v2d,
                            screen_start.x,
                            screen_start.y + 1.0f,
                            &screen_y_uv[0],
                            &screen_y_uv[1]);
  const float2 drag_axis_x = (float2(screen_x_uv[0], screen_x_uv[1]) - start_uv) *
                             float2(state->ref_tile_size);
  const float2 drag_axis_y = (float2(screen_y_uv[0], screen_y_uv[1]) - start_uv) *
                             float2(state->ref_tile_size);
  state->create->set_drag_axes(drag_axis_x, drag_axis_y);
  state->create->begin(
      *event,
      image_shape_event_to_px(region, event, state->ref_tile, state->ref_tile_size),
      (event->type == LEFTMOUSE) && (event->val == KM_PRESS));

  state->owner_region_type = region->runtime->type;
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, image_shape_draw_drag, state, REGION_DRAW_POST_PIXEL);
  shape::shape_status_set_creation(C, state->create->type());

  op->customdata = state;
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus image_shape_draw_modal(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  ImageShapeState *state = static_cast<ImageShapeState *>(op->customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  if (state == nullptr || !sima || !region) {
    image_shape_state_free(C, op);
    return OPERATOR_CANCELLED;
  }

  shape::ShapeCreateGesture &create = *state->create;

  /* Pixel F / Shift+F: adjust the stroke width / the brush strength (the shape's overall
   * opacity), like the brush radial control. Any other modal action ends an in-flight value
   * drag. */
  Scene *scene = CTX_data_scene(C);
  if (event->type == EVT_MODAL_MAP && scene && scene->toolsettings) {
    if (ELEM(event->val,
             PAINT_SHAPE_MODAL_STROKE_WIDTH,
             PAINT_SHAPE_MODAL_STROKE_OPACITY))
    {
      const bool is_width = (event->val == PAINT_SHAPE_MODAL_STROKE_WIDTH);
      Paint &paint = scene->toolsettings->imapaint.paint;
      float start = create.style().stroke_width;
      if (!is_width) {
        if (Brush *brush = BKE_paint_brush(&paint)) {
          start = BKE_brush_alpha_get(&paint, brush);
        }
      }
      create.begin_value_drag(is_width, int2(event->mval[0], event->mval[1]), start);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->val == PAINT_SHAPE_MODAL_AXIS_X) {
      /* X swaps the shape's own Stroke / Fill colors; rebuild the style so the in-progress shape
       * uses the new pair. */
      WM_operator_name_call(
          C, "PAINT_OT_shape_colors_swap", wm::OpCallContext::ExecDefault, nullptr, nullptr);
      shape::ShapeStyle style = shape::style_from_settings(
          BKE_paint_shape_settings_get(*scene->toolsettings));
      shape::style_brush_values_from_brush(scene->toolsettings->imapaint.paint, style);
      create.set_style(style);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    create.end_value_drag();
  }
  if (create.value_drag_active()) {
    if (event->type == EVT_FKEY && event->val == KM_RELEASE) {
      create.end_value_drag();
      return OPERATOR_RUNNING_MODAL;
    }
    if (ELEM(event->type, MOUSEMOVE, INBETWEEN_MOUSEMOVE) && scene && scene->toolsettings) {
      const float value = create.update_value_drag(int2(event->mval[0], event->mval[1]));
      if (create.value_drag_is_width()) {
        BKE_paint_shape_settings_get(*scene->toolsettings).stroke_width = value;
      }
      else {
        /* The opacity itself is derived from the brush strength in one place
         * (#style_brush_values_from_brush); the drag only writes the strength. */
        Paint &paint = scene->toolsettings->imapaint.paint;
        if (Brush *brush = BKE_paint_brush(&paint)) {
          BKE_brush_alpha_set(&paint, brush, value);
        }
      }
      WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  const float2 event_px = image_shape_event_to_px(
      region, event, state->ref_tile, state->ref_tile_size);
  switch (create.handle_event(*event, event_px)) {
    case shape::ShapeCreateResult::Confirmed:
      return image_shape_apply(C, op, state);
    case shape::ShapeCreateResult::Cancelled:
      image_shape_state_free(C, op);
      return OPERATOR_CANCELLED;
    case shape::ShapeCreateResult::Changed:
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    case shape::ShapeCreateResult::None:
      return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void image_shape_draw_cancel(bContext *C, wmOperator *op)
{
  image_shape_state_free(C, op);
}

static wmOperatorStatus image_shape_draw_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  if (!scene || !scene->toolsettings) {
    return OPERATOR_CANCELLED;
  }
  std::optional<shape::PaintShape> shape = shape::shape_from_op_props(op);
  if (!shape || shape->is_empty()) {
    return OPERATOR_CANCELLED;
  }
  shape::ShapeStyle style = shape::style_from_settings(
      BKE_paint_shape_settings_get(*scene->toolsettings));
  /* Redo (F9) replays the style stored with the operator; a fresh call keeps the settings. */
  if (!shape::style_from_op_props(op, style)) {
    shape::style_brush_values_from_brush(scene->toolsettings->imapaint.paint, style);
  }
  const Vector<shape::PaintShape> shapes = {*shape};
  const shape::CanvasTile tile = shape::canvas_tile_from_op_props(op);
  if (shape::shape_bake(C, shapes, tile, style, true, SHAPE_UNDO_NAME)) {
    return OPERATOR_FINISHED;
  }
  /* Material mode with no writable map: say why instead of silently doing nothing. */
  if (const char *reason = shape::shape_targets_refusal_message(C, CTX_data_active_object(C))) {
    BKE_report(op->reports, RPT_WARNING, reason);
  }
  return OPERATOR_CANCELLED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modal keymap
 * \{ */

wmKeyMap *paint_shape_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {PAINT_SHAPE_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {PAINT_SHAPE_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {PAINT_SHAPE_MODAL_UNDO, "UNDO", 0, "Undo", ""},
      {PAINT_SHAPE_MODAL_REDO, "REDO", 0, "Redo", ""},
      {PAINT_SHAPE_MODAL_MOVE, "MOVE", 0, "Move", ""},
      {PAINT_SHAPE_MODAL_ROTATE, "ROTATE", 0, "Rotate", ""},
      {PAINT_SHAPE_MODAL_SCALE, "SCALE", 0, "Scale", ""},
      {PAINT_SHAPE_MODAL_STROKE_WIDTH, "STROKE_WIDTH", 0, "Stroke Width", ""},
      {PAINT_SHAPE_MODAL_STROKE_OPACITY, "STROKE_OPACITY", 0, "Stroke Opacity", ""},
      {PAINT_SHAPE_MODAL_EXTRUDE, "EXTRUDE", 0, "Extrude", ""},
      {PAINT_SHAPE_MODAL_SELECT_NEXT, "SELECT_NEXT", 0, "Select Next", ""},
      {PAINT_SHAPE_MODAL_AXIS_X, "AXIS_X", 0, "Axis X", ""},
      {PAINT_SHAPE_MODAL_AXIS_Y, "AXIS_Y", 0, "Axis Y", ""},
      /* Reset the active shape's origin to its computed mean (bound to O). */
      {PAINT_SHAPE_MODAL_ORIGIN_RESET, "ORIGIN_RESET", 0, "Reset Origin", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Image Paint Shape Modal";

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);

  /* Called once per space-type; the map and its items only need to be built the first time. */
  if (keymap && keymap->modal_items) {
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);

  /* No default bindings in C. The working layout comes from the Python key configs
   * (`km_image_paint_shape_modal_map` in blender_default.py / industry_compatible_data.py),
   * which also carried every item including ORIGIN_RESET. The C items above only declare the
   * modal enum so the Python bindings resolve; adding bindings here would duplicate them and
   * has drifted from Python on modifiers (see O34). */

  WM_modalkeymap_assign(keymap, "PAINT_OT_image_shape_draw");

  return keymap;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shape color swap
 * \{ */

static wmOperatorStatus shape_colors_swap_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return OPERATOR_CANCELLED;
  }
  PaintShapeSettings &shape = BKE_paint_shape_settings_get(*scene->toolsettings);
  for (int i = 0; i < 4; i++) {
    const float tmp = shape.stroke_color[i];
    shape.stroke_color[i] = shape.fill_color[i];
    shape.fill_color[i] = tmp;
  }
  /* Redraw the headers / editors. */
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, nullptr);
  return OPERATOR_FINISHED;
}

static bool shape_colors_swap_poll(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr;
}

void PAINT_OT_shape_colors_swap(wmOperatorType *ot)
{
  ot->name = "Swap Shape Colors";
  ot->idname = "PAINT_OT_shape_colors_swap";
  ot->description = "Swap the shape tool's Stroke and Fill colors";
  ot->exec = shape_colors_swap_exec;
  ot->poll = shape_colors_swap_poll;
  /* No OPTYPE_UNDO: the swap writes the shared UI settings, so a memfile step in the paint modes
   * is neither needed nor wanted. */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

void PAINT_OT_image_shape_draw(wmOperatorType *ot)
{
  ot->name = "Paint Shape";
  ot->idname = "PAINT_OT_image_shape_draw";
  ot->description =
      "Draw a shape into the texture: drag for rectangle/ellipse; Shift snaps, Alt grows from "
      "the center, Space moves";

  ot->invoke = image_shape_draw_invoke;
  ot->modal = image_shape_draw_modal;
  ot->exec = image_shape_draw_exec;
  ot->cancel = image_shape_draw_cancel;
  ot->poll = image_shape_draw_poll;
  /* No OPTYPE_UNDO: the bake opens and closes its own image undo step named #SHAPE_UNDO_NAME
   * inside the compositor. Letting WM push a second step on top would leave F9 popping the empty
   * WM step and re-baking over the already-baked pixels, and every undo would take two presses
   * (the convention of the image-undo-pushing operators of this feature, see
   * #IMAGE_SELECT_GESTURE_OPTYPE_FLAGS in paint_image_select_mask.cc). OPTYPE_REGISTER is kept
   * for the "Adjust Last Operation" panel; the redo machinery finds the image step by
   * `ot->name`. */
  ot->flag = OPTYPE_REGISTER;

  static const EnumPropertyItem type_items[] = {
      {-1, "DEFAULT", 0, "Default", "Use the type from the tool settings"},
      {PAINT_SHAPE_LINE, "LINE", 0, "Line", "Straight line"},
      {PAINT_SHAPE_RECT, "RECTANGLE", 0, "Rectangle", "Rectangle with rounded corners"},
      {PAINT_SHAPE_ELLIPSE, "ELLIPSE", 0, "Ellipse", "Ellipse"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  PropertyRNA *prop = RNA_def_property(ot->srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, type_items);
  RNA_def_property_enum_default(prop, -1);
  RNA_def_property_ui_text(prop, "Type", "Shape to draw");
  /* Invoke-time selector only: `exec` reads the serialized `shape_type`, so Redo does not need
   * this one persisted. */
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  shape::shape_op_properties_register(ot);
  shape::style_op_properties_register(ot);
}

}  // namespace blender
