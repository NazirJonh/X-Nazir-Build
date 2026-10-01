/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared interactive Bézier outline input (#BezierInput); see the header
 * for the interaction model. Factored out of the Select Curve operator so shape drawing tools
 * reuse the exact same placement, editing and drawing behavior.
 */

#include "paint_bezier_input.hh"

#include <cmath>

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"

#include "DNA_screen_types.h"
#include "DNA_theme_types.h" /* UI_SCALE_FAC */
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_curve.hh"
#include "BKE_global.hh" /* U */

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

#include "WM_api.hh"

namespace blender::ed::sculpt_paint::bezier_input {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Cursor distance (UI-scaled pixels) that snaps a click to the first point and closes the
 * curve. Same order of magnitude as #wm::gesture::POLYLINE_CLICK_RADIUS for the polyline. */
constexpr float CLOSE_RADIUS = 14.0f;
/** Cursor travel (UI-scaled pixels) while placing a point beyond which its handles become
 * user-set instead of auto. Matches #IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX. */
constexpr float HANDLE_DRAG_THRESHOLD = 3.0f;
/** Evaluated points per Bézier segment in the live overlay preview. */
constexpr int EVAL_SEGMENTS_PREVIEW = 24;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bézier evaluation
 * \{ */

/**
 * Catmull-Rom style auto handles: the tangent at the point is the centered neighbor difference,
 * and each handle extends a third of the tangent, i.e. one sixth of the neighbor distance.
 */
static void auto_handles(const float2 &prev_co,
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

void BezierInput::point_handles_get(const int index,
                                    const bool closed,
                                    float2 &r_handle_left,
                                    float2 &r_handle_right) const
{
  const BezierInputPoint &point = points_[index];
  if (point.handles_set) {
    r_handle_left = point.handle_left;
    r_handle_right = point.handle_right;
    return;
  }
  if (points_.size() < 2) {
    r_handle_left = point.co;
    r_handle_right = point.co;
    return;
  }

  const int last = points_.size() - 1;
  const float2 prev_co = (index > 0) ? points_[index - 1].co :
                                       (closed ? points_[last].co : point.co * 2.0f - points_[1].co);
  const float2 next_co = (index < last) ? points_[index + 1].co :
                                          (closed ? points_[0].co : point.co * 2.0f - points_[last - 1].co);
  auto_handles(prev_co, point.co, next_co, r_handle_left, r_handle_right);
}

void BezierInput::eval_segments(const int eval_segments,
                                const bool include_closing,
                                const bool close_straight,
                                Vector<float2> &r_points) const
{
  r_points.clear();
  const int count = points_.size();
  if (count < 2) {
    return;
  }
  const bool force_straight = flag_is_set(flags_, Flag::ForceStraight);

  const int segment_count = include_closing ? count : count - 1;
  r_points.reserve(segment_count * eval_segments);
  Array<float, 64> data((eval_segments + 1) * 2);
  for (const int segment : IndexRange(segment_count)) {
    const int i = segment;
    const int j = (segment + 1) % count;
    /* The closing segment (j wraps to the first point) is straight when Ctrl is held at close
     * time, in addition to the first point's own flag. */
    const bool straight = force_straight ||
                          ((j == 0) ? (points_[j].straight_prev || close_straight) :
                                      points_[j].straight_prev);
    float2 handle_left_i, handle_right_i;
    float2 handle_left_j, handle_right_j;
    point_handles_get(i, include_closing, handle_left_i, handle_right_i);
    point_handles_get(j, include_closing, handle_left_j, handle_right_j);
    if (straight) {
      /* Degenerate Bézier: control handles on the endpoints collapse the segment to a line. */
      handle_right_i = points_[i].co;
      handle_left_j = points_[j].co;
    }

    BKE_curve_forward_diff_bezier(points_[i].co.x,
                                  handle_right_i.x,
                                  handle_left_j.x,
                                  points_[j].co.x,
                                  data.data(),
                                  eval_segments,
                                  sizeof(float[2]));
    BKE_curve_forward_diff_bezier(points_[i].co.y,
                                  handle_right_i.y,
                                  handle_left_j.y,
                                  points_[j].co.y,
                                  data.data() + 1,
                                  eval_segments,
                                  sizeof(float[2]));

    /* Skip each segment's end point (it starts the next segment); the closing segment's end
     * point is the first control point, appended so the drawn loop and the committed polygon
     * both close explicitly. */
    const int step_count = (include_closing && segment == segment_count - 1) ? eval_segments + 1 :
                                                                               eval_segments;
    for (const int step : IndexRange(step_count)) {
      r_points.append(float2(data[step * 2], data[step * 2 + 1]));
    }
  }
}

void BezierInput::flatten(const int eval_segments,
                          const bool include_closing,
                          Vector<float2> &r_points) const
{
  /* Qualified: the \a eval_segments parameter shadows the member of the same name. */
  BezierInput::eval_segments(eval_segments, include_closing, closed_straight_, r_points);
}

Vector<float2> BezierInput::flatten(const int eval_segments, const bool include_closing) const
{
  Vector<float2> points;
  flatten(eval_segments, include_closing, points);
  return points;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lifetime and cursor helpers
 * \{ */

BezierInput::BezierInput(const Flag flags) : flags_(flags) {}

void BezierInput::begin(const ARegion &region,
                        const wmEvent &event,
                        const Mapping &mapping,
                        const bool button_held)
{
  cursor_user_ = mapping.event_to_user(region, event);
  BezierInputPoint first;
  first.co = cursor_user_;
  /* A forced-straight input (Polyline) draws its closing segment straight as well. */
  first.straight_prev = flag_is_set(flags_, Flag::ForceStraight);
  points_.append(first);
  /* A mouse-button invocation keeps the press down: dragging from here shapes the first point's
   * handles. A keyboard/menu invocation (search) starts idle instead. */
  handle_drag_active_ = button_held && !flag_is_set(flags_, Flag::ForceStraight);
  /* Seed the double-click tracker with the invoking press: it placed the first point, so a
   * double-click that lands right after begin drops it again (a bare double-click on the canvas
   * starts and cancels the input). */
  last_press_time_ = BLI_time_now_seconds();
  last_press_xy_ = int2(event.xy[0], event.xy[1]);
  last_press_added_point_ = true;
}

float BezierInput::point_distance_px(const ARegion &region,
                                     const Mapping &mapping,
                                     const float2 &user,
                                     const wmEvent &event) const
{
  const float2 region_pos = mapping.to_region(region, user);
  const float dx = float(event.mval[0]) - region_pos.x;
  const float dy = float(event.mval[1]) - region_pos.y;
  return math::sqrt(dx * dx + dy * dy);
}

void BezierInput::hover_update(const ARegion &region,
                               const wmEvent &event,
                               const Mapping &mapping)
{
  hover_close_ = false;
  if (points_.size() < 3) {
    return;
  }
  const float radius = CLOSE_RADIUS * UI_SCALE_FAC;
  hover_close_ = point_distance_px(region, mapping, points_[0].co, event) <= radius;
}

void BezierInput::undo_last()
{
  if (points_.size() <= 1) {
    return;
  }
  points_.remove_last();
  last_press_added_point_ = false;
  handle_drag_active_ = false;
  move_point_active_ = false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event handling
 * \{ */

Result BezierInput::handle_event(const ARegion &region,
                                 const wmEvent &event,
                                 const Mapping &mapping)
{
  if (points_.is_empty()) {
    return Result::None;
  }

  /* Modifier state is tracked on every event so the drawing callback can preview the straight
   * closing segment while Ctrl is held, even between clicks. */
  modifier_ctrl_ = (event.modifier & KM_CTRL) != 0;

  switch (event.type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      cursor_user_ = mapping.event_to_user(region, event);
      if (handle_drag_active_ && !flag_is_set(flags_, Flag::ForceStraight)) {
        BezierInputPoint &active = points_.last();
        const float threshold = HANDLE_DRAG_THRESHOLD * UI_SCALE_FAC;
        if (point_distance_px(region, mapping, active.co, event) > threshold) {
          active.handles_set = true;
          /* Holding Ctrl while shaping collapses the incoming segment to a straight line: it
           * stays straight for the rest of the session (Ctrl+Z removes the point wholesale).
           * The right handle keeps following the cursor and shapes the outgoing segment. */
          active.handle_right = cursor_user_;
          if (modifier_ctrl_) {
            active.straight_prev = true;
            active.handle_left = active.co;
          }
          else if (!active.straight_prev) {
            /* Mirrored handles, like #PAINTCURVE_OT_slide with `align`: the right handle follows
             * the cursor, the left one keeps the opposite side of the point. */
            active.handle_left = active.co * 2.0f - cursor_user_;
          }
        }
      }
      else if (move_point_active_) {
        /* Reposition the last placed point; user-set handles travel with it, auto handles are
         * recomputed from the neighbors anyway. */
        BezierInputPoint &active = points_.last();
        const float2 delta = cursor_user_ - active.co;
        active.co = cursor_user_;
        if (active.handles_set) {
          active.handle_left += delta;
          active.handle_right += delta;
        }
      }
      hover_update(region, event, mapping);
      return Result::Changed;
    }

    case LEFTMOUSE: {
      if (event.val == KM_PRESS) {
        /* Second press of a double-click: the first press just placed a point, drop it and close
         * with auto handles on the ends it was about to connect. */
        const bool is_double_click = (event.prev_type == LEFTMOUSE) &&
                                     (event.prev_val == KM_RELEASE) &&
                                     !WM_event_drag_test(&event, last_press_xy_) &&
                                     ((BLI_time_now_seconds() - last_press_time_) * 1e3 <
                                      double(U.dbl_click_time));
        last_press_time_ = BLI_time_now_seconds();
        last_press_xy_ = int2(event.xy[0], event.xy[1]);

        cursor_user_ = mapping.event_to_user(region, event);
        hover_update(region, event, mapping);
        if (is_double_click) {
          /* Drop the point the first press of the double-click placed (if it placed one at all:
           * a press on the last point started a move instead). */
          if (last_press_added_point_ && !points_.is_empty()) {
            points_.remove_last();
          }
          last_press_added_point_ = false;
          handle_drag_active_ = false;
          move_point_active_ = false;
          if (flag_is_set(flags_, Flag::AllowOpen) && points_.size() >= 2) {
            /* An open-capable input ends the path where it is instead of dropping the points. */
            closed_ = false;
            closed_straight_ = false;
            return Result::Confirmed;
          }
          if (points_.size() >= 3) {
            closed_ = true;
            /* A double-click close never draws the closing segment straight. */
            closed_straight_ = false;
            return Result::Closed;
          }
          /* Not enough points to close: a bare double-click does nothing. */
          return Result::Cancelled;
        }
        if (hover_close_) {
          /* Ctrl at close time draws the closing segment straight. */
          closed_ = true;
          closed_straight_ = modifier_ctrl_;
          return Result::Closed;
        }
        const float grab_radius = CLOSE_RADIUS * UI_SCALE_FAC;
        if (point_distance_px(region, mapping, points_.last().co, event) <= grab_radius) {
          /* Press on the last placed point: drag it to a new position instead of adding a
           * duplicate. */
          move_point_active_ = true;
          last_press_added_point_ = false;
          return Result::Changed;
        }
        BezierInputPoint point;
        point.co = cursor_user_;
        /* Ctrl+click: the segment leading into this point is a straight line, letting straight
         * and curved segments be mixed freely. */
        point.straight_prev = modifier_ctrl_ || flag_is_set(flags_, Flag::ForceStraight);
        points_.append(point);
        handle_drag_active_ = !flag_is_set(flags_, Flag::ForceStraight);
        last_press_added_point_ = true;
        return Result::Changed;
      }
      if (event.val == KM_RELEASE) {
        handle_drag_active_ = false;
        move_point_active_ = false;
        return Result::Changed;
      }
      return Result::None;
    }

    case EVT_ZKEY: {
      /* Ctrl+Z: undo the last placed point (and any straight-line flag placed with it). */
      if (event.val == KM_PRESS && (event.modifier & KM_CTRL)) {
        if (points_.size() > 1) {
          undo_last();
          hover_update(region, event, mapping);
          return Result::Changed;
        }
      }
      return Result::None;
    }

    case EVT_BACKSPACEKEY: {
      /* Backspace removes the last placed point, like Ctrl+Z. */
      if (event.val == KM_PRESS) {
        if (points_.size() > 1) {
          undo_last();
          hover_update(region, event, mapping);
          return Result::Changed;
        }
      }
      return Result::None;
    }

    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event.val == KM_PRESS) {
        const bool can_confirm_open = flag_is_set(flags_, Flag::AllowOpen) && points_.size() >= 2;
        if (points_.size() >= 3 || can_confirm_open) {
          closed_ = !flag_is_set(flags_, Flag::AllowOpen);
          closed_straight_ = modifier_ctrl_;
          return Result::Confirmed;
        }
        return Result::Cancelled;
      }
      return Result::None;
    }

    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      if (event.val == KM_PRESS) {
        return Result::Cancelled;
      }
      return Result::None;
    }

    default:
      return Result::None;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

/** Axis-aligned square, the control-point shape of Stroke Method: Curve. */
static void square_draw(const uint pos, const float2 &center, const float half)
{
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, center.x - half, center.y - half);
  immVertex2f(pos, center.x + half, center.y - half);
  immVertex2f(pos, center.x + half, center.y + half);
  immVertex2f(pos, center.x - half, center.y + half);
  immEnd();
}

void BezierInput::draw(const ARegion &region, const Mapping &mapping, const DrawStyle &style) const
{
  if (points_.is_empty()) {
    return;
  }

  auto to_region = [&](const float2 &user) { return mapping.to_region(region, user); };

  const bool can_close = hover_close_ && points_.size() >= 3;

  /* Outline through the placed points (plus the closing segment while it is being closed; the
   * closing preview goes straight while Ctrl is held, matching what a close click would do). */
  Vector<float2> preview_user;
  eval_segments(EVAL_SEGMENTS_PREVIEW, can_close, can_close && modifier_ctrl_, preview_user);
  Vector<float2> preview_region(preview_user.size());
  for (const int i : preview_user.index_range()) {
    preview_region[i] = to_region(preview_user[i]);
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
  immUniform4f("color", style.outline.x, style.outline.y, style.outline.z, style.outline.w);
  immUniform4f("color2", style.outline2.x, style.outline2.y, style.outline2.z, style.outline2.w);
  immUniform1f("dash_width", style.dash_width);
  immUniform1f("udash_factor", style.udash_factor);
  immUniform1f("dash_phase", float(fmod(BLI_time_now_seconds(), 1.0)));
  GPU_line_width(1.0f);
  if (preview_region.size() >= 2) {
    immBegin(GPU_PRIM_LINE_STRIP, int(preview_region.size()));
    for (const float2 &co : preview_region) {
      immVertex2f(pos, co.x, co.y);
    }
    immEnd();
  }

  /* Rubber band from the last point to the cursor while the curve stays open. */
  if (!can_close && !handle_drag_active_) {
    const float2 rubber[2] = {to_region(points_.last().co), to_region(cursor_user_)};
    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(pos, rubber[0].x, rubber[0].y);
    immVertex2f(pos, rubber[1].x, rubber[1].y);
    immEnd();
  }
  immUnbindProgram();

  /* Control points and the active point's handles, in plain uniform color. */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  Vector<float2> point_region(points_.size());
  for (const int i : points_.index_range()) {
    point_region[i] = to_region(points_[i].co);
  }

  /* Handles only for the active (most recently placed) point, and only once the user has shaped
   * them: every other point stays on auto handles and shows none. */
  const bool show_handles = points_.last().handles_set && !flag_is_set(flags_, Flag::ForceStraight);
  if (show_handles) {
    const float2 &co = point_region.last();
    const float2 handles[2] = {to_region(points_.last().handle_left),
                               to_region(points_.last().handle_right)};

    GPU_line_width(3.0f);
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
    for (const float2 &handle : handles) {
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos, co.x, co.y);
      immVertex2f(pos, handle.x, handle.y);
      immEnd();
    }
    GPU_line_width(1.5f);
    immUniformColor4f(style.handle.x, style.handle.y, style.handle.z, style.handle.w);
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
    immUniformColor4f(style.handle.x, style.handle.y, style.handle.z, style.handle.w);
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
    immUniformColor4f(
        style.point_shadow.x, style.point_shadow.y, style.point_shadow.z, style.point_shadow.w);
    square_draw(pos, co, half);
    if (is_first && can_close) {
      immUniformColor4f(style.close.x, style.close.y, style.close.z, style.close.w);
    }
    else {
      immUniformColor4f(style.point.x, style.point.y, style.point.z, style.point.w);
    }
    square_draw(pos, co, half - 1.5f);
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::bezier_input
