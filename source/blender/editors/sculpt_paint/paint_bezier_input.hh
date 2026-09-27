/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared interactive Bézier outline input for Image Editor painting tools.
 *
 * The input models the brush Stroke Method: Curve flow, factored out of the Select Curve
 * operator so shape drawing tools (Polyline, Curve Patch) reuse the exact same placement,
 * editing and drawing behavior:
 *
 * - A click places a point; while the mouse button stays held, moving the cursor pulls out that
 *   point's handles (mirrored). Releasing without moving keeps the point on auto handles.
 * - Ctrl+click places a point whose incoming segment is a straight line.
 * - Ctrl+Z removes the last placed point, hovering the first point and clicking closes the
 *   outline, a double-click closes the same way, Enter confirms, ESC or right-click cancels.
 * - The in-progress outline is drawn as an animated dashed line; handles are drawn only for the
 *   active (most recently placed) point.
 *
 * Control points live in the caller's coordinate space (UV for image paint); the mapping between
 * that space and region pixels -- needed for cursor hit tests and drawing -- is supplied through
 * #Mapping, so the input works for any 2D space.
 */

#pragma once

#include "BLI_enum_flags.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {

struct ARegion;
struct wmEvent;

}  // namespace blender

namespace blender::ed::sculpt_paint::bezier_input {

/** One placed Bézier control point. */
struct BezierInputPoint {
  /** Control point position, caller space. */
  float2 co;
  /** Handles, caller space. Only meaningful while #handles_set. */
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

enum class Result {
  /** Event not relevant to the input; the owning operator keeps running unchanged. */
  None,
  /** Input state changed; the overlay needs a redraw. */
  Changed,
  /** The outline just closed through the first point (close click or double-click). */
  Closed,
  /** Enter confirmed: closed, or open when #Flag::AllowOpen is set. */
  Confirmed,
  /** ESC / right-click / abandoned double-click: discard the input. */
  Cancelled,
};

enum class Flag {
  None = 0,
  /** Enter may confirm an open curve (Polyline) once it has two or more points. */
  AllowOpen = (1 << 0),
  /** Every placed segment is a straight line (Polyline); handles are never shaped. */
  ForceStraight = (1 << 1),
};
ENUM_OPERATORS(Flag)

/**
 * Coordinate mapping between the caller's space (where control points live, e.g. UV) and region
 * pixels. Both directions are pure functions of the region, so plain function pointers suffice.
 */
struct Mapping {
  /** Caller-space point to region pixels; used for hover tests and drawing. */
  float2 (*to_region)(const ARegion &region, const float2 &user);
  /** Event position to caller space. */
  float2 (*event_to_user)(const ARegion &region, const wmEvent &event);
};

/** Overlay colors (and dash length) the input draws itself with. */
struct DrawStyle {
  /** Animated dashed outline, alternating between the two colors. */
  float4 outline = float4(0.4f, 0.4f, 0.4f, 1.0f);
  float4 outline2 = float4(1.0f, 1.0f, 1.0f, 1.0f);
  float dash_width = 8.0f;
  float udash_factor = 0.5f;
  /** Active point's handles (thin line over a darker fat line) and handle dots. */
  float4 handle = float4(1.0f, 0.85f, 0.0f, 0.95f);
  /** First point while a click would close the outline. */
  float4 close = float4(0.2f, 0.9f, 1.0f, 0.95f);
  /** Control point squares (white fill over a darker backing square). */
  float4 point = float4(1.0f, 1.0f, 1.0f, 0.95f);
  float4 point_shadow = float4(0.0f, 0.0f, 0.0f, 0.7f);
};

/**
 * Modal Bézier outline input: place, edit and close a curve one click at a time.
 *
 * The owning operator seeds it with #begin on invoke, feeds it every modal event through
 * #handle_event, registers its own region draw callback that forwards to #draw, and acts on the
 * returned #Result (commit the outline on `Closed`/`Confirmed`, tear down on `Cancelled`).
 */
class BezierInput {
 public:
  explicit BezierInput(Flag flags = Flag::None);

  /**
   * Place the first point from the invoking event.
   *
   * \param button_held: true when the invocation is the mouse button still being held down
   * (LMB press); dragging then shapes the first point's handles. A keyboard or menu invocation
   * passes false and the input starts idle.
   */
  void begin(const ARegion &region,
             const wmEvent &event,
             const Mapping &mapping,
             bool button_held);

  /** Feed a modal event; see #Result for the outcomes. */
  Result handle_event(const ARegion &region, const wmEvent &event, const Mapping &mapping);

  /** Draw the in-progress outline, rubber band, control points and active handles. */
  void draw(const ARegion &region, const Mapping &mapping, const DrawStyle &style) const;

  /** Placed control points. */
  const Vector<BezierInputPoint> &points() const
  {
    return points_;
  }

  bool is_empty() const
  {
    return points_.is_empty();
  }

  /** True once the outline closed (close click, double-click, or a closed confirm). */
  bool is_closed() const
  {
    return closed_;
  }

  /** True when the closing segment (last point back to first) is straight: Ctrl was held at
   * close time. Only meaningful once #is_closed(). */
  bool closed_straight() const
  {
    return closed_straight_;
  }

  /** True while the cursor hovers the first point and a click would close the outline. */
  bool hover_close() const
  {
    return hover_close_;
  }

  /** Remove the last placed point (Ctrl+Z), if there is more than one. */
  void undo_last();

  /**
   * Evaluate the outline into a dense polyline in caller space.
   *
   * \param eval_segments: evaluated points per Bézier segment.
   * \param include_closing: append the closing segment (last point back to first, evaluated with
   * wrap-around neighbors), closing the loop explicitly. Without it the open chain of placed
   * segments is returned.
   */
  void flatten(int eval_segments, bool include_closing, Vector<float2> &r_points) const;
  Vector<float2> flatten(int eval_segments, bool include_closing) const;

 private:
  /** Resolve both handles of point #index (user-set ones, or auto ones from the neighbors). */
  void point_handles_get(int index, bool closed, float2 &r_handle_left, float2 &r_handle_right) const;
  /** Evaluate placed segments (and optionally the closing one) into \a r_points. */
  void eval_segments(int eval_segments,
                     bool include_closing,
                     bool close_straight,
                     Vector<float2> &r_points) const;
  /** Cursor distance to a caller-space point in region pixels. */
  float point_distance_px(const ARegion &region,
                          const Mapping &mapping,
                          const float2 &user,
                          const wmEvent &event) const;
  /** Refresh #hover_close_ from the cursor position. */
  void hover_update(const ARegion &region, const wmEvent &event, const Mapping &mapping);

  Flag flags_ = Flag::None;
  Vector<BezierInputPoint> points_;
  /** Current cursor position, caller space (rubber band target, handle drag source). */
  float2 cursor_user_ = float2(0.0f);
  /** LMB is held over the last placed point's drag; moving adjusts its handles. */
  bool handle_drag_active_ = false;
  /** LMB is held on the last placed point; moving repositions it (handles travel along). */
  bool move_point_active_ = false;
  /** Cursor hovers the first point (with enough points placed): a click closes the curve. */
  bool hover_close_ = false;
  /** Ctrl is currently held: straight-line placement, straight closing segment. */
  bool modifier_ctrl_ = false;
  /** True once the outline closed; #closed_straight_ records Ctrl at close time. */
  bool closed_ = false;
  bool closed_straight_ = false;

  /* Double-click detection. Modal operators receive the second press of a double-click as a
   * plain #KM_PRESS (the window manager converts #KM_DBL_CLICK away before dispatch and only
   * restores it for handlers further down the chain), so the pair is recognized here with the
   * same test #wm_event_is_double_click applies: release in between, no drag in between, and
   * both presses within the user's double-click speed. */
  double last_press_time_ = 0.0;
  int2 last_press_xy_ = int2(0);
  /** The previous press actually placed a point: only then does the closing press of a
   * double-click drop it again. A press that started a move or closed the curve did not. */
  bool last_press_added_point_ = false;
};

}  // namespace blender::ed::sculpt_paint::bezier_input
