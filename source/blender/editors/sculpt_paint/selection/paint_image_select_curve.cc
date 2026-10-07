/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Curve-based paint selection for the Image Editor ("Select Curve").
 *
 * The user draws a closed Bézier outline one control point at a time, modeled on the brush
 * Stroke Method: Curve editing flow. The interaction itself (click to place points, drag to
 * shape their handles, Ctrl for straight segments, Ctrl+Z, double-click, close and cancel) lives
 * in the shared #blender::ed::sculpt_paint::bezier_input::BezierInput module, reused by the
 * shape drawing tools; this operator only wires it up and commits the result.
 *
 * On close the Bézier outline is flattened into a dense UV polygon and committed through the
 * shared gesture-selection sequence (#image_select_gesture_exec_generic), exactly like box,
 * lasso, polyline and circle -- including the face/island expansion and the image-editor-space
 * mirror.
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../shapes/paint_bezier_input.hh"
#include "paint_image_select_gesture.hh"
#include "paint_image_select_intern.hh"
/* #image_select_move_delegate_to_move_operator only. */
#include "paint_image_select_move_intern.hh"

namespace blender {

namespace bezier_input = ed::sculpt_paint::bezier_input;

namespace {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Evaluated points per Bézier segment when flattening the closed curve into a mask polygon. */
constexpr int IMAGE_SELECT_CURVE_EVAL_SEGMENTS = 24;

/**
 * Flags shared by the gesture selection operators.
 *
 * No OPTYPE_UNDO: the commit opens and closes its own image undo step inside
 * #image_select_gesture_exec_generic. No OPTYPE_BLOCKING: consistent with the other selection
 * gestures. See the comment on #IMAGE_SELECT_GESTURE_OPTYPE_FLAGS in
 * paint_image_select_mask.cc for the full rationale.
 */
constexpr short IMAGE_SELECT_CURVE_OPTYPE_FLAGS = OPTYPE_REGISTER;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coordinate mapping
 * \{ */

/** The curve input works in UV space; hit tests and drawing need region pixels. */
static float2 image_select_curve_to_region(const ARegion &region, const float2 &uv)
{
  float sx, sy;
  ui::view2d_view_to_region_fl(&region.v2d, uv.x, uv.y, &sx, &sy);
  return float2(sx, sy);
}

static float2 image_select_curve_event_to_user(const ARegion &region, const wmEvent &event)
{
  float uv[2];
  ui::view2d_region_to_view(&region.v2d, float(event.mval[0]), float(event.mval[1]), &uv[0], &uv[1]);
  return float2(uv[0], uv[1]);
}

constexpr bezier_input::Mapping IMAGE_SELECT_CURVE_MAPPING = {image_select_curve_to_region,
                                                              image_select_curve_event_to_user};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve state
 * \{ */

struct ImageSelectCurveState {
  SpaceImage *owner_sima = nullptr;
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;

  bezier_input::BezierInput input;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name State lifetime
 * \{ */

static void image_select_curve_state_free(bContext *C, wmOperator *op)
{
  ImageSelectCurveState *state = static_cast<ImageSelectCurveState *>(op->customdata);
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

static void image_select_curve_draw(const bContext *C, ARegion *region, void *arg)
{
  ImageSelectCurveState *state = static_cast<ImageSelectCurveState *>(arg);
  if (state == nullptr || state->input.is_empty()) {
    return;
  }
  /* The callback is registered on the shared Image Editor region type; only draw for the editor
   * that owns this curve (a second Image Editor must not draw the overlay). */
  if (CTX_wm_space_image(C) != state->owner_sima) {
    return;
  }
  state->input.draw(*region, IMAGE_SELECT_CURVE_MAPPING, bezier_input::DrawStyle());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator properties
 * \{ */

/**
 * The flattened UV polygon, stored so `exec` runs the shared gesture sequence both from the
 * modal apply and from repeats of the operator. Same transient treatment as the lasso `path`:
 * hidden from the interface, not carried across invocations.
 */
static void image_select_curve_path_store(wmOperator *op, const Span<float2> uv_points)
{
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "path");
  if (prop == nullptr) {
    return;
  }
  RNA_collection_clear(op->ptr, "path");
  for (const float2 &uv : uv_points) {
    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "path", &itemptr);
    const float loc[2] = {uv.x, uv.y};
    RNA_float_set_array(&itemptr, "loc", loc);
  }
}

static Vector<float2> image_select_curve_path_get(wmOperator *op)
{
  Vector<float2> uv_points;
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "path");
  if (prop == nullptr) {
    return uv_points;
  }
  RNA_PROP_BEGIN (op->ptr, itemptr, prop) {
    float loc[2];
    RNA_float_get_array(&itemptr, "loc", loc);
    uv_points.append(float2(loc[0], loc[1]));
  }
  RNA_PROP_END;
  return uv_points;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit
 * \{ */

class ImageSelectCurveShape : public ImageSelectGestureShape {
  Vector<float2> uv_points_;

 public:
  explicit ImageSelectCurveShape(Vector<float2> uv_points) : uv_points_(std::move(uv_points)) {}

  const char *undo_name() const override
  {
    return "Curve Select";
  }

  bool uv_bounds_calc(const ARegion * /*region*/, wmOperator * /*op*/, rctf &r_uv_bounds) override
  {
    if (uv_points_.size() < 3) {
      return false;
    }
    BLI_rctf_init_minmax(&r_uv_bounds);
    for (const float2 &uv : uv_points_) {
      BLI_rctf_do_minmax_v(&r_uv_bounds, uv);
    }
    return true;
  }

  void rasterize_tile(const float2 &uv_origin,
                      const rctf & /*tile_uv_rect*/,
                      ImBuf *mask,
                      const float fill_value) const override
  {
    image_select_uv_polygon_rasterize_tile(uv_origin, uv_points_, mask, fill_value);
  }

  void rasterize_tile_mirrored(const float2 &uv_origin,
                               const rctf & /*tile_uv_rect*/,
                               ImBuf *mask,
                               const float fill_value,
                               const bool mirror_x,
                               const bool mirror_y) const override
  {
    image_select_uv_polygon_rasterize_tile(
        uv_origin, uv_points_, mask, fill_value, mirror_x, mirror_y);
  }

  Vector<float2> uv_outline() const override
  {
    return uv_points_;
  }

  PaintSelectionEdgePolicy edge_policy() const override
  {
    return BKE_image_paint_selection_edge_policy_feathered();
  }
};

/**
 * Close the curve and commit it through the shared gesture sequence.
 *
 * The straightness of the closing segment was already recorded by the input when it closed
 * (Ctrl held at close time). Frees the modal state first: the overlay must disappear before the
 * commit's redraw, and the undo step the commit opens must not race a live draw callback.
 */
static wmOperatorStatus image_select_curve_apply(bContext *C, wmOperator *op)
{
  ImageSelectCurveState *state = static_cast<ImageSelectCurveState *>(op->customdata);
  BLI_assert(state != nullptr);

  Vector<float2> uv_points;
  state->input.flatten(IMAGE_SELECT_CURVE_EVAL_SEGMENTS, true, uv_points);
  image_select_curve_path_store(op, uv_points);
  image_select_curve_state_free(C, op);

  if (uv_points.size() < 3) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectCurveShape shape(std::move(uv_points));
  return image_select_gesture_exec_generic(C, op, shape);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator callbacks
 * \{ */

static wmOperatorStatus image_select_curve_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  /* Clicking a floating move-selection fragment hands the event to the move operator instead,
   * like every other selection gesture. */
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return OPERATOR_FINISHED;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  if (!sima || !sima->runtime || !region || !region->runtime->type) {
    return OPERATOR_CANCELLED;
  }

  auto *state = MEM_new<ImageSelectCurveState>(__func__);
  state->owner_sima = sima;
  state->input.begin(*region,
                     *event,
                     IMAGE_SELECT_CURVE_MAPPING,
                     /* A mouse-button invocation keeps the press down: dragging from here shapes
                      * the first point's handles. A keyboard/menu invocation (search) starts
                      * idle instead. */
                     (event->type == LEFTMOUSE) && (event->val == KM_PRESS));

  state->owner_region_type = region->runtime->type;
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, image_select_curve_draw, state, REGION_DRAW_POST_PIXEL);

  op->customdata = state;
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus image_select_curve_modal(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  ImageSelectCurveState *state = static_cast<ImageSelectCurveState *>(op->customdata);
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  if (state == nullptr || !sima || !region) {
    /* The session can be torn down under a still-registered modal handler (area closed, image
     * swapped). Returning CANCELLED does not run `ot->cancel`, so free explicitly. */
    image_select_curve_state_free(C, op);
    return OPERATOR_CANCELLED;
  }

  switch (state->input.handle_event(*region, *event, IMAGE_SELECT_CURVE_MAPPING)) {
    case bezier_input::Result::Closed:
    case bezier_input::Result::Confirmed:
      return image_select_curve_apply(C, op);
    case bezier_input::Result::Cancelled:
      image_select_curve_state_free(C, op);
      return OPERATOR_CANCELLED;
    case bezier_input::Result::Changed:
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    case bezier_input::Result::None:
      return OPERATOR_RUNNING_MODAL;
  }
  BLI_assert_unreachable();
  return OPERATOR_RUNNING_MODAL;
}

static void image_select_curve_cancel(bContext *C, wmOperator *op)
{
  image_select_curve_state_free(C, op);
}

static wmOperatorStatus image_select_curve_exec(bContext *C, wmOperator *op)
{
  /* Reached with a stored flattened polygon from the modal apply; a repeat invocation has no
   * path (the property is skip-save) and cancels, like the other gesture operators. */
  Vector<float2> uv_points = image_select_curve_path_get(op);
  if (uv_points.size() < 3) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectCurveShape shape(std::move(uv_points));
  return image_select_gesture_exec_generic(C, op, shape);
}

/** \} */

}  // namespace

void PAINT_OT_image_select_curve(wmOperatorType *ot)
{
  ot->name = "Select Curve";
  ot->idname = "PAINT_OT_image_select_curve";
  ot->description =
      "Select a curved region as a paint mask: click to place points, drag to shape their "
      "handles, click the first point or double-click to close the curve";

  ot->invoke = image_select_curve_invoke;
  ot->modal = image_select_curve_modal;
  ot->exec = image_select_curve_exec;
  ot->cancel = image_select_curve_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = IMAGE_SELECT_CURVE_OPTYPE_FLAGS;

  WM_operator_properties_select_operation_simple(ot);
  /* The shared commit sequence reads `is_simple_click` unconditionally; register the (always
   * false) bookkeeping properties so the lookup does not warn. The curve tool never participates
   * in simple-click deselect. */
  image_select_gesture_properties(ot);
  PropertyRNA *prop = RNA_def_collection_runtime(
      ot->srna, "path", RNA_OperatorMousePath, "Path", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender
