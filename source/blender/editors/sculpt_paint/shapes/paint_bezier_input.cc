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
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"

#include "DNA_screen_types.h"
#include "DNA_theme_types.h" /* UI_SCALE_FAC */
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_curve.hh"
#include "BKE_global.hh" /* U */

#include "ED_control_points.hh"
#include "ED_shape_session_overlay.hh"

#include "GPU_immediate.hh"
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

/**
 * The placed points as the shared control-point layer sees them: region-space positions, handles
 * resolved with the same auto rule the outline uses, and the active point's handles exposed.
 * Every other point keeps auto handles hidden, matching how the input has always looked: only
 * the point being shaped shows its tangent.
 */
struct BezierInput::ScreenSource : control_points::Source {
  ScreenSource(const BezierInput &input,
               const ARegion &region,
               const Mapping &mapping,
               const bool can_close)
      : input_(input), region_(region), mapping_(mapping), can_close_(can_close)
  {
  }

  int points_num() const override
  {
    return input_.points_.size();
  }

  float2 point_position(const int point) const override
  {
    return mapping_.to_region(region_, input_.points_[point].co);
  }

  void point_handles(const int point, float2 &r_left, float2 &r_right) const override
  {
    float2 left, right;
    input_.point_handles_get(point, can_close_, left, right);
    r_left = mapping_.to_region(region_, left);
    r_right = mapping_.to_region(region_, right);
  }

  int8_t handle_type(const int point, bool /*left*/) const override
  {
    if (flag_is_set(input_.flags_, Flag::ForceStraight)) {
      return BEZIER_HANDLE_VECTOR;
    }
    const BezierInputPoint &input_point = input_.points_[point];
    if (point == input_.points_.size() - 1 && input_point.handles_set) {
      /* The dragged handles stay mirrored unless Ctrl collapsed the incoming segment. */
      return input_point.straight_prev ? BEZIER_HANDLE_VECTOR : BEZIER_HANDLE_ALIGN;
    }
    return BEZIER_HANDLE_AUTO;
  }

  uint8_t selection(const int point) const override
  {
    /* The first point doubles as the closing affordance: while a click would close the outline,
     * it is highlighted like a selected control point. */
    return (point == 0 && can_close_) ? SEL_F2 : 0;
  }

  bool handle_visible(const int point, const bool left) const override
  {
    if (flag_is_set(input_.flags_, Flag::ForceStraight)) {
      return false;
    }
    const int last = input_.points_.size() - 1;
    const BezierInputPoint &input_point = input_.points_[point];
    if (point != last || !input_point.handles_set) {
      return false;
    }
    float2 handle_left, handle_right;
    input_.point_handles_get(point, can_close_, handle_left, handle_right);
    return !control_points::handle_is_collapsed(left ? handle_left : handle_right, input_point.co);
  }

  int segments_num() const override
  {
    /* The dashed outline is the input's own contour, drawn by its region callback. */
    return 0;
  }

  void segment_polyline(int /*segment*/, Vector<float2> & /*r_out*/) const override {}

 private:
  const BezierInput &input_;
  const ARegion &region_;
  const Mapping &mapping_;
  bool can_close_;
};

void BezierInput::overlay_sync(const ARegion &region, const Mapping &mapping)
{
  if (points_.is_empty()) {
    this->overlay_release();
    return;
  }

  const bool can_close = hover_close_ && points_.size() >= 3;
  ScreenSource source(*this, region, mapping, can_close);

  control_points::BuildOptions options;
  options.cursor = mapping.to_region(region, cursor_user_);
  options.hover_point = true;
  options.hover_metric = control_points::Metric::Euclidean;

  PaintCurveScreenHandles handles;
  control_points::handles_build(source, options, handles);
  if (!overlay_owner_) {
    overlay_owner_ = std::make_shared<bool>(true);
  }
  shape_overlay::bezier_overlay::handles_set(region, overlay_owner_, handles);

  /* A modal input belongs to one region for its whole life; moving to another region would
   * leave a stale snapshot behind, so drop the old one first. */
  if (overlay_region_ != nullptr && overlay_region_ != &region) {
    shape_overlay::bezier_overlay::handles_clear(*overlay_region_);
  }
  overlay_region_ = &region;
}

void BezierInput::overlay_release()
{
  if (overlay_region_ != nullptr) {
    shape_overlay::bezier_overlay::handles_clear(*overlay_region_);
    overlay_region_ = nullptr;
    overlay_owner_.reset();
  }
}

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
  this->overlay_sync(region, mapping);
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
  const Result result = this->handle_event_impl(region, event, mapping);
  /* Publish on every event: motion changes the hover, the rubber band and the dragged handles
   * even when the result says nothing was committed. */
  this->overlay_sync(region, mapping);
  return result;
}

Result BezierInput::handle_event_impl(const ARegion &region,
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
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region pixel mapping
 * \{ */

static float2 region_pixel_to_region(const ARegion & /*region*/, const float2 &co)
{
  return co;
}

static float2 region_pixel_event_to_user(const ARegion & /*region*/, const wmEvent &event)
{
  return float2(event.mval[0], event.mval[1]);
}

Mapping region_pixel_mapping()
{
  return {region_pixel_to_region, region_pixel_event_to_user};
}

/** \} */

}  // namespace blender::ed::sculpt_paint::bezier_input

/* -------------------------------------------------------------------- */
/** \name Overlay snapshot registry
 * \{ */

namespace blender::ed::sculpt_paint::shape_overlay::bezier_overlay {

/** The snapshot of one region's modal input, alive as long as that input is. */
struct RegistryEntry {
  std::weak_ptr<bool> owner;
  PaintCurveScreenHandles handles;
};

/** The active input of every region. There is at most one modal Bézier input per region, and
 * the modal replaces its snapshot on every state change. */
static Map<const ARegion *, RegistryEntry> g_entries;

void handles_set(const ARegion &region,
                 const OwnerToken &owner,
                 const PaintCurveScreenHandles &handles)
{
  RegistryEntry &entry = g_entries.lookup_or_add_default(&region);
  entry.owner = owner;
  entry.handles = handles;
}

static void entry_remove(const ARegion &region)
{
  g_entries.remove(&region);
  /* A static container must not keep a heap buffer alive to exit (the leak check at shutdown
   * would report it); #Map::clear frees the table. */
  if (g_entries.is_empty()) {
    g_entries.clear();
  }
}

void handles_clear(const ARegion &region)
{
  entry_remove(region);
}

const PaintCurveScreenHandles *handles_find(const ARegion &region)
{
  RegistryEntry *entry = g_entries.lookup_ptr(&region);
  if (entry == nullptr) {
    return nullptr;
  }
  if (entry->owner.expired()) {
    entry_remove(region);
    return nullptr;
  }
  return &entry->handles;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape_overlay::bezier_overlay
