/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Curve-based paint selection for the Image Editor ("Select Curve").
 *
 * The user draws a closed Bézier outline one control point at a time, modeled on the brush
 * Stroke Method: Curve editing flow:
 *
 * - The click that starts the tool places the first point.
 * - Every further click places the next point; while the mouse button stays held, moving the
 *   cursor pulls out that point's handles (mirrored, like #PAINTCURVE_OT_slide with `align`),
 *   shaping the segments on both sides of it. Releasing without moving keeps the point on auto
 *   handles, so a plain sequence of clicks yields a smooth closed curve.
 * - Clicking on the last placed point and dragging repositions it (its handles travel along);
 *   Ctrl+Z removes the last placed point.
 * - Ctrl+click places a point whose incoming segment is a straight line, and Ctrl+click on the
 *   first point closes the outline with a straight closing segment -- so straight and curved
 *   segments can be mixed freely to build the wanted shape.
 * - The in-progress outline is drawn as an animated dashed line, matching the committed
 *   selection outline. Handles are drawn only for the active (most recently placed) point.
 * - Hovering the first point highlights it; clicking it closes the curve and applies the
 *   selection. A double-click anywhere closes the curve the same way (dropping the point the
 *   first click of the double-click just placed) with auto handles on the unclosed ends. Enter
 *   confirms without closing through the cursor, ESC or right-click cancels.
 *
 * On close the Bézier outline is flattened into a dense UV polygon and committed through the
 * shared gesture-selection sequence (#image_select_gesture_exec_generic), exactly like box,
 * lasso, polyline and circle -- including the face/island expansion and the image-editor-space
 * mirror.
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_image_select_gesture.hh"
#include "paint_image_select_intern.hh"
/* #image_select_move_delegate_to_move_operator only. */
#include "paint_image_select_move_intern.hh"

namespace blender {

namespace {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Evaluated points per Bézier segment when flattening the closed curve into a mask polygon. */
constexpr int IMAGE_SELECT_CURVE_EVAL_SEGMENTS = 24;
/** Cursor distance (UI-scaled pixels) that snaps a click to the first point and closes the
 * curve. Same order of magnitude as #wm::gesture::POLYLINE_CLICK_RADIUS for the polyline. */
constexpr float IMAGE_SELECT_CURVE_CLOSE_RADIUS = 14.0f;
/** Cursor travel (UI-scaled pixels) while placing a point beyond which its handles become
 * user-set instead of auto. Matches #IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX. */
constexpr float IMAGE_SELECT_CURVE_HANDLE_DRAG_THRESHOLD = 3.0f;

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
/** \name Curve state
 * \{ */

struct ImageSelectCurvePoint {
  /** Control point position, UV space. */
  float2 co;
  /** Handles, UV space. Only meaningful while #handles_set. */
  float2 handle_left;
  float2 handle_right;
  /** True once the user dragged the handles out; auto handles apply otherwise. */
  bool handles_set = false;
  /**
   * The segment leading into this point is a straight line (Ctrl+click), overriding both this
   * point's and the previous point's handles. For the first point this is the closing segment
   * (last point back to first).
   */
  bool straight_prev = false;
};

struct ImageSelectCurveState {
  SpaceImage *owner_sima = nullptr;
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;

  Vector<ImageSelectCurvePoint> points;
  /** Current cursor position, UV space (rubber band target, handle drag source). */
  float2 cursor_uv = float2(0.0f);
  /** LMB is held over the last placed point's drag; moving adjusts its handles. */
  bool handle_drag_active = false;
  /** LMB is held on the last placed point; moving repositions it (handles travel along). */
  bool move_point_active = false;
  /** Cursor hovers the first point (with enough points placed): a click closes the curve. */
  bool hover_close = false;
  /** Ctrl is currently held: straight-line placement, straight closing segment. */
  bool modifier_ctrl = false;

  /* Double-click detection. Modal operators receive the second press of a double-click as a
   * plain #KM_PRESS (the window manager converts #KM_DBL_CLICK away before dispatch and only
   * restores it for handlers further down the chain), so the pair is recognized here with the
   * same test #wm_event_is_double_click applies: release in between, no drag in between, and
   * both presses within the user's double-click speed. */
  double last_press_time = 0.0;
  int2 last_press_xy = int2(0);
  /** The previous press actually placed a point: only then does the closing press of a
   * double-click drop it again. A press that started a move or closed the curve did not. */
  bool last_press_added_point = false;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bézier evaluation
 * \{ */

/**
 * Catmull-Rom style auto handles: the tangent at the point is the centered neighbor difference,
 * and each handle extends a third of the tangent, i.e. one sixth of the neighbor distance.
 */
static void image_select_curve_auto_handles(const float2 &prev_co,
                                            const float2 &co,
                                            const float2 &next_co,
                                            float2 &r_handle_left,
                                            float2 &r_handle_right)
{
  const float2 diff = next_co - prev_co;
  if (math::length_squared(diff) < 1e-20f) {
    r_handle_left = co;
    r_handle_right = co;
    return;
  }
  r_handle_right = co + diff * (1.0f / 6.0f);
  r_handle_left = co - diff * (1.0f / 6.0f);
}

/**
 * Resolve both handles of point #index: the user-set ones when present, auto ones otherwise.
 *
 * With \a closed set, the neighbors wrap around (the outline will close through the first
 * point). Without it, the open ends mirror their only neighbor, so the first segment keeps
 * a smooth continuation while the curve is still being placed.
 */
static void image_select_curve_point_handles_get(const Span<ImageSelectCurvePoint> points,
                                                 const int index,
                                                 const bool closed,
                                                 float2 &r_handle_left,
                                                 float2 &r_handle_right)
{
  const ImageSelectCurvePoint &point = points[index];
  if (point.handles_set) {
    r_handle_left = point.handle_left;
    r_handle_right = point.handle_right;
    return;
  }
  if (points.size() < 2) {
    r_handle_left = point.co;
    r_handle_right = point.co;
    return;
  }

  const int last = points.size() - 1;
  const float2 prev_co = (index > 0) ? points[index - 1].co :
                                       (closed ? points[last].co :
                                                 point.co * 2.0f - points[1].co);
  const float2 next_co = (index < last) ? points[index + 1].co :
                                          (closed ? points[0].co :
                                                    point.co * 2.0f - points[last - 1].co);
  image_select_curve_auto_handles(prev_co, point.co, next_co, r_handle_left, r_handle_right);
}

/**
 * Evaluate the outline through every placed point into \a r_uv_points.
 *
 * Without \a include_closing this is the open preview: the segments between consecutive points
 * only. With it, the closing segment (last point back to the first) is appended as well -- this
 * is the polygon the selection commits, evaluated with wrap-around neighbors.
 *
 * A segment leading into a point flagged #ImageSelectCurvePoint::straight_prev collapses to a
 * straight line regardless of the handles. \a close_straight additionally forces the closing
 * segment straight (Ctrl held while closing); it ORs with the first point's flag.
 */
static void image_select_curve_eval_segments(const Span<ImageSelectCurvePoint> points,
                                             Vector<float2> &r_uv_points,
                                             const bool include_closing,
                                             const bool close_straight)
{
  r_uv_points.clear();
  const int count = points.size();
  if (count < 2) {
    return;
  }

  const int segment_count = include_closing ? count : count - 1;
  r_uv_points.reserve(segment_count * IMAGE_SELECT_CURVE_EVAL_SEGMENTS);
  float data[(IMAGE_SELECT_CURVE_EVAL_SEGMENTS + 1) * 2];
  for (int segment = 0; segment < segment_count; segment++) {
    const int i = segment;
    const int j = (segment + 1) % count;
    /* The closing segment (j wraps to the first point) is straight when Ctrl is held at close
     * time, in addition to the first point's own flag. */
    const bool straight = (j == 0) ? (points[j].straight_prev || close_straight) :
                                     points[j].straight_prev;
    float2 handle_left_i, handle_right_i;
    float2 handle_left_j, handle_right_j;
    image_select_curve_point_handles_get(points, i, include_closing, handle_left_i, handle_right_i);
    image_select_curve_point_handles_get(points, j, include_closing, handle_left_j, handle_right_j);
    if (straight) {
      /* Degenerate Bézier: control handles on the endpoints collapse the segment to a line. */
      handle_right_i = points[i].co;
      handle_left_j = points[j].co;
    }

    BKE_curve_forward_diff_bezier(points[i].co.x,
                                  handle_right_i.x,
                                  handle_left_j.x,
                                  points[j].co.x,
                                  data,
                                  IMAGE_SELECT_CURVE_EVAL_SEGMENTS,
                                  sizeof(float[2]));
    BKE_curve_forward_diff_bezier(points[i].co.y,
                                  handle_right_i.y,
                                  handle_left_j.y,
                                  points[j].co.y,
                                  data + 1,
                                  IMAGE_SELECT_CURVE_EVAL_SEGMENTS,
                                  sizeof(float[2]));

    /* Skip each segment's end point (it starts the next segment); the closing segment's end
     * point is the first control point, appended so the drawn loop and the committed polygon
     * both close explicitly. */
    const int step_count = (include_closing && segment == segment_count - 1) ?
                               IMAGE_SELECT_CURVE_EVAL_SEGMENTS + 1 :
                               IMAGE_SELECT_CURVE_EVAL_SEGMENTS;
    for (int step = 0; step < step_count; step++) {
      r_uv_points.append(float2(data[step * 2], data[step * 2 + 1]));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

static void image_select_curve_line_strip_draw(const uint pos,
                                               const Span<float2> region_points,
                                               const int vertex_count)
{
  if (vertex_count < 2) {
    return;
  }
  immBegin(GPU_PRIM_LINE_STRIP, vertex_count);
  for (const int i : IndexRange(vertex_count)) {
    immVertex2f(pos, region_points[i].x, region_points[i].y);
  }
  immEnd();
}

/** Axis-aligned square, the control-point shape of Stroke Method: Curve. */
static void image_select_curve_square_draw(const uint pos, const float2 &center, const float half)
{
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, center.x - half, center.y - half);
  immVertex2f(pos, center.x + half, center.y - half);
  immVertex2f(pos, center.x + half, center.y + half);
  immVertex2f(pos, center.x - half, center.y + half);
  immEnd();
}

static void image_select_curve_draw(const bContext *C, ARegion *region, void *arg)
{
  ImageSelectCurveState *state = static_cast<ImageSelectCurveState *>(arg);
  if (state == nullptr || state->points.is_empty()) {
    return;
  }
  /* The callback is registered on the shared Image Editor region type; only draw for the editor
   * that owns this curve (a second Image Editor must not draw the overlay). */
  if (CTX_wm_space_image(C) != state->owner_sima) {
    return;
  }

  auto to_region = [&](const float2 &uv) {
    float sx, sy;
    ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &sx, &sy);
    return float2(sx, sy);
  };

  const bool can_close = state->hover_close && state->points.size() >= 3;

  /* Outline through the placed points (plus the closing segment while it is being closed; the
   * closing preview goes straight while Ctrl is held, matching what a close click would do). */
  Vector<float2> preview_uv;
  image_select_curve_eval_segments(state->points, preview_uv, can_close, can_close && state->modifier_ctrl);
  Vector<float2> preview_region(preview_uv.size());
  for (const int i : preview_uv.index_range()) {
    preview_region[i] = to_region(preview_uv[i]);
  }

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  /* The animated dashed line matches the committed selection outline's marching-ants look. */
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR_ANIMATED);
  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2] / UI_SCALE_FAC, viewport_size[3] / UI_SCALE_FAC);
  immUniform1i("colors_len", 2);
  immUniform4f("color", 0.4f, 0.4f, 0.4f, 1.0f);
  immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
  immUniform1f("dash_width", 8.0f);
  immUniform1f("udash_factor", 0.5f);
  immUniform1f("dash_phase", float(fmod(BLI_time_now_seconds(), 1.0)));
  GPU_line_width(1.0f);
  image_select_curve_line_strip_draw(pos, preview_region, int(preview_region.size()));

  /* Rubber band from the last point to the cursor while the curve stays open. */
  if (!can_close && !state->handle_drag_active) {
    const float2 rubber[2] = {to_region(state->points.last().co), to_region(state->cursor_uv)};
    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(pos, rubber[0].x, rubber[0].y);
    immVertex2f(pos, rubber[1].x, rubber[1].y);
    immEnd();
  }
  immUnbindProgram();

  /* Control points and the active point's handles, in plain uniform color. */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  Vector<float2> point_region(state->points.size());
  for (const int i : state->points.index_range()) {
    point_region[i] = to_region(state->points[i].co);
  }

  /* Handles only for the active (most recently placed) point, and only once the user has shaped
   * them: every other point stays on auto handles and shows none. */
  const bool show_handles = state->points.last().handles_set;
  if (show_handles) {
    const float2 &co = point_region.last();
    const float2 handles[2] = {to_region(state->points.last().handle_left),
                               to_region(state->points.last().handle_right)};

    GPU_line_width(3.0f);
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
    for (const float2 &handle : handles) {
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos, co.x, co.y);
      immVertex2f(pos, handle.x, handle.y);
      immEnd();
    }
    GPU_line_width(1.5f);
    immUniformColor4f(1.0f, 0.85f, 0.0f, 0.95f);
    for (const float2 &handle : handles) {
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos, co.x, co.y);
      immVertex2f(pos, handle.x, handle.y);
      immEnd();
    }

    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
    for (const float2 &handle : handles) {
      imm_draw_circle_fill_2d(pos, handle.x, handle.y, 3.5f, 12);
    }
    immUniformColor4f(1.0f, 0.85f, 0.0f, 0.95f);
    for (const float2 &handle : handles) {
      imm_draw_circle_fill_2d(pos, handle.x, handle.y, 2.5f, 12);
    }
  }

  for (const int i : point_region.index_range()) {
    const bool is_first = (i == 0);
    const bool is_active = (i == point_region.index_range().last());
    const float2 &co = point_region[i];
    /* Control points are squares like Stroke Method: Curve. The movable (most recently placed)
     * point is drawn largest, and the first point doubles as the close handle: enlarge and tint
     * it while a click would close the curve so the affordance is visible before committing. */
    float half = 3.5f;
    if (is_active) {
      half = 5.5f;
    }
    else if (is_first) {
      half = can_close ? 5.0f : 4.5f;
    }
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
    image_select_curve_square_draw(pos, co, half);
    if (is_first && can_close) {
      immUniformColor4f(0.2f, 0.9f, 1.0f, 0.95f);
    }
    else {
      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.95f);
    }
    image_select_curve_square_draw(pos, co, half - 1.5f);
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

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
/** \name Cursor helpers
 * \{ */

static float2 image_select_curve_event_uv(const ARegion *region, const wmEvent *event)
{
  float uv[2];
  ui::view2d_region_to_view(&region->v2d, float(event->mval[0]), float(event->mval[1]), &uv[0], &uv[1]);
  return float2(uv[0], uv[1]);
}

/** Cursor distance to a UV point in region pixels. */
static float image_select_curve_point_distance_px(const ARegion *region,
                                                  const wmEvent *event,
                                                  const float2 &uv)
{
  float sx, sy;
  ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &sx, &sy);
  const float dx = float(event->mval[0]) - sx;
  const float dy = float(event->mval[1]) - sy;
  return math::sqrt(dx * dx + dy * dy);
}

/** Refresh #ImageSelectCurveState::hover_close from the cursor position. */
static void image_select_curve_hover_update(ImageSelectCurveState &state,
                                            const ARegion *region,
                                            const wmEvent *event)
{
  state.hover_close = false;
  if (state.points.size() < 3) {
    return;
  }
  const float radius = IMAGE_SELECT_CURVE_CLOSE_RADIUS * UI_SCALE_FAC;
  state.hover_close = image_select_curve_point_distance_px(region, event, state.points[0].co) <=
                      radius;
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
 * \a close_straight forces the closing segment into a straight line (Ctrl held while closing).
 *
 * Frees the modal state first: the overlay must disappear before the commit's redraw, and the
 * undo step the commit opens must not race a live draw callback.
 */
static wmOperatorStatus image_select_curve_apply(bContext *C,
                                                 wmOperator *op,
                                                 const bool close_straight)
{
  ImageSelectCurveState *state = static_cast<ImageSelectCurveState *>(op->customdata);
  BLI_assert(state != nullptr);

  Vector<float2> uv_points;
  image_select_curve_eval_segments(state->points, uv_points, true, close_straight);
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
  state->cursor_uv = image_select_curve_event_uv(region, event);
  ImageSelectCurvePoint first;
  first.co = state->cursor_uv;
  state->points.append(first);
  /* A mouse-button invocation keeps the press down: dragging from here shapes the first point's
   * handles. A keyboard/menu invocation (search) starts idle instead. */
  state->handle_drag_active = (event->type == LEFTMOUSE) && (event->val == KM_PRESS);
  /* Seed the double-click tracker with the invoking press: it placed the first point, so a
   * double-click that lands right after invoke drops it again (a bare double-click on the
   * canvas starts and cancels the tool). */
  state->last_press_time = BLI_time_now_seconds();
  state->last_press_xy = int2(event->xy[0], event->xy[1]);
  state->last_press_added_point = true;

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

  /* Modifier state is tracked on every event so the drawing callback can preview the straight
   * closing segment while Ctrl is held, even between clicks. */
  state->modifier_ctrl = (event->modifier & KM_CTRL) != 0;

  switch (event->type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      state->cursor_uv = image_select_curve_event_uv(region, event);
      if (state->handle_drag_active) {
        ImageSelectCurvePoint &active = state->points.last();
        const float threshold = IMAGE_SELECT_CURVE_HANDLE_DRAG_THRESHOLD * UI_SCALE_FAC;
        if (image_select_curve_point_distance_px(region, event, active.co) > threshold) {
          active.handles_set = true;
          /* Holding Ctrl while shaping collapses the incoming segment to a straight line: it
           * stays straight for the rest of the session (Ctrl+Z removes the point wholesale).
           * The right handle keeps following the cursor and shapes the outgoing segment. */
          active.handle_right = state->cursor_uv;
          if (state->modifier_ctrl) {
            active.straight_prev = true;
            active.handle_left = active.co;
          }
          else if (!active.straight_prev) {
            /* Mirrored handles, like #PAINTCURVE_OT_slide with `align`: the right handle follows
             * the cursor, the left one keeps the opposite side of the point. */
            active.handle_left = active.co * 2.0f - state->cursor_uv;
          }
        }
      }
      else if (state->move_point_active) {
        /* Reposition the last placed point; user-set handles travel with it, auto handles are
         * recomputed from the neighbors anyway. */
        ImageSelectCurvePoint &active = state->points.last();
        const float2 delta = state->cursor_uv - active.co;
        active.co = state->cursor_uv;
        if (active.handles_set) {
          active.handle_left += delta;
          active.handle_right += delta;
        }
      }
      image_select_curve_hover_update(*state, region, event);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }

    case LEFTMOUSE: {
      if (event->val == KM_PRESS) {
        /* Second press of a double-click: the first press just placed a point, drop it and close
         * with auto handles on the ends it was about to connect. */
        const bool is_double_click = (event->prev_type == LEFTMOUSE) &&
                                     (event->prev_val == KM_RELEASE) &&
                                     !WM_event_drag_test(event, state->last_press_xy) &&
                                     ((BLI_time_now_seconds() - state->last_press_time) * 1e3 <
                                      double(U.dbl_click_time));
        state->last_press_time = BLI_time_now_seconds();
        state->last_press_xy = int2(event->xy[0], event->xy[1]);

        state->cursor_uv = image_select_curve_event_uv(region, event);
        image_select_curve_hover_update(*state, region, event);
        if (is_double_click) {
          /* Drop the point the first press of the double-click placed (if it placed one at all:
           * a press on the last point started a move instead). */
          if (state->last_press_added_point && !state->points.is_empty()) {
            state->points.remove_last();
          }
          state->last_press_added_point = false;
          state->handle_drag_active = false;
          state->move_point_active = false;
          if (state->points.size() >= 3) {
            return image_select_curve_apply(C, op, false);
          }
          /* Not enough points to close: a bare double-click does nothing to the selection. */
          image_select_curve_state_free(C, op);
          return OPERATOR_CANCELLED;
        }
        if (state->hover_close) {
          /* Ctrl at close time draws the closing segment straight. */
          return image_select_curve_apply(C, op, state->modifier_ctrl);
        }
        const float grab_radius = IMAGE_SELECT_CURVE_CLOSE_RADIUS * UI_SCALE_FAC;
        if (image_select_curve_point_distance_px(region, event, state->points.last().co) <=
            grab_radius)
        {
          /* Press on the last placed point: drag it to a new position instead of adding a
           * duplicate. */
          state->move_point_active = true;
          state->last_press_added_point = false;
          ED_region_tag_redraw(region);
          return OPERATOR_RUNNING_MODAL;
        }
        ImageSelectCurvePoint point;
        point.co = state->cursor_uv;
        /* Ctrl+click: the segment leading into this point is a straight line, letting straight
         * and curved segments be mixed freely. */
        point.straight_prev = state->modifier_ctrl;
        state->points.append(point);
        state->handle_drag_active = true;
        state->last_press_added_point = true;
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->val == KM_RELEASE) {
        state->handle_drag_active = false;
        state->move_point_active = false;
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    case EVT_ZKEY: {
      /* Ctrl+Z: undo the last placed point (and any straight-line flag placed with it). */
      if (event->val == KM_PRESS && (event->modifier & KM_CTRL)) {
        if (state->points.size() > 1) {
          state->points.remove_last();
          state->last_press_added_point = false;
          state->handle_drag_active = false;
          state->move_point_active = false;
          image_select_curve_hover_update(*state, region, event);
          ED_region_tag_redraw(region);
        }
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        if (state->points.size() >= 3) {
          return image_select_curve_apply(C, op, state->modifier_ctrl);
        }
        image_select_curve_state_free(C, op);
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      if (event->val == KM_PRESS) {
        image_select_curve_state_free(C, op);
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    default:
      return OPERATOR_RUNNING_MODAL;
  }
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
