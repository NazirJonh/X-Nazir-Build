/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared shape creation machine; see #paint_shape_create.hh.
 */

#include "paint_shape_create.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "UI_resources.hh"

#include "WM_types.hh"

#include "paint_intern.hh"
#include "paint_shape_draw.hh"

namespace blender::ed::sculpt_paint::shape {

/** Cursor travel (UI-scaled pixels) past which a press counts as a drag rather than a click. */
constexpr float SHAPE_CLICK_DRAG_THRESHOLD = 3.0f;

ShapeCreateGesture::ShapeCreateGesture(const ePaintShapeType type, const ShapeStyle &style)
    : type_(type), style_(style)
{
}

void ShapeCreateGesture::set_drag_axes(const float2 &axis_x, const float2 &axis_y)
{
  const float axis_x_length = math::length(axis_x);
  const float axis_y_length = math::length(axis_y);
  drag_axis_x_ = axis_x_length > 1e-8f ? axis_x / axis_x_length : float2(1.0f, 0.0f);
  drag_axis_y_ = axis_y_length > 1e-8f ? axis_y / axis_y_length : float2(0.0f, 1.0f);
  drag_rotation_ = math::atan2(drag_axis_x_.y, drag_axis_x_.x);
  use_drag_axes_ = true;
}

void ShapeCreateGesture::begin_value_drag(const bool is_width,
                                          const int2 &mval,
                                          const float value)
{
  value_drag_active_ = true;
  value_drag_is_width_ = is_width;
  value_drag_start_value_ = value;
  value_drag_start_mval_ = mval;
}

float ShapeCreateGesture::update_value_drag(const int2 &mval)
{
  const float dx = float(mval.x - value_drag_start_mval_.x);
  if (value_drag_is_width_) {
    style_.stroke_width = math::max(0.0f, value_drag_start_value_ + dx);
    return style_.stroke_width;
  }
  /* The opacity drag uses the same sensitivity as the brush radial control. */
  const float opacity = math::clamp(value_drag_start_value_ + dx * 0.005f, 0.0f, 1.0f);
  style_.stroke_opacity = opacity;
  style_.fill_opacity = opacity;
  return opacity;
}

void ShapeCreateGesture::end_value_drag()
{
  value_drag_active_ = false;
}

bool ShapeCreateGesture::value_drag_active() const
{
  return value_drag_active_;
}

bool ShapeCreateGesture::value_drag_is_width() const
{
  return value_drag_is_width_;
}

void ShapeCreateGesture::begin(const wmEvent &event, const float2 &event_p, const bool button_held)
{
  drag_active_ = button_held;
  press_p_ = event_p;
  cursor_p_ = event_p;
  press_mval_ = int2(event.mval[0], event.mval[1]);
  move_prev_p_ = event_p;
  update_drag(false, false, false);
}

void ShapeCreateGesture::update_drag(const bool modifier_shift,
                                     const bool modifier_alt,
                                     const bool modifier_ctrl)
{
  const float2 delta = cursor_p_ - press_p_;
  const float2 p0 = use_drag_axes_ ? float2(0.0f) : press_p_;
  const float2 p1 = use_drag_axes_ ?
                  p0 + float2(math::dot(delta, drag_axis_x_), math::dot(delta, drag_axis_y_)) :
                  cursor_p_;
  const bool from_center = modifier_alt || style_.use_from_center();
  /* Ctrl draws a circle with the Ellipse tool and a square with the Rectangle tool. */
  const bool keep_aspect = modifier_shift ||
                           (modifier_ctrl && ELEM(type_, PAINT_SHAPE_ELLIPSE, PAINT_SHAPE_RECT)) ||
                           style_.use_keep_aspect();

  switch (type_) {
    case PAINT_SHAPE_RECT:
      shape_ = shape_rect_from_drag(p0, p1, from_center, keep_aspect, style_);
      break;
    case PAINT_SHAPE_ELLIPSE:
      shape_ = shape_ellipse_from_drag(p0, p1, from_center, keep_aspect);
      break;
    default:
      break;
  }
  if (use_drag_axes_) {
    const float c = drag_axis_x_.x;
    const float s = drag_axis_x_.y;
    shape_.center = press_p_ + float2(c * shape_.center.x - s * shape_.center.y,
                                      s * shape_.center.x + c * shape_.center.y);
    shape_.rotation += drag_rotation_;
  }
  has_shape_ = true;
}

PaintShape ShapeCreateGesture::shape_at_center(const float2 &center) const
{
  PaintShape shape;
  switch (type_) {
    case PAINT_SHAPE_RECT:
      shape = shape_rect_at_center(center, style_);
      break;
    default:
      shape = shape_ellipse_at_center(center, style_);
      break;
  }
  return shape;
}

ShapeCreateResult ShapeCreateGesture::handle_event(const wmEvent &event, const float2 &event_p)
{
  const bool shift = (event.modifier & KM_SHIFT) != 0;
  const bool alt = (event.modifier & KM_ALT) != 0;
  const bool ctrl = (event.modifier & KM_CTRL) != 0;

  switch (event.type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      cursor_p_ = event_p;
      if (math::distance(float2(event.mval[0], event.mval[1]), float2(press_mval_)) >
          SHAPE_CLICK_DRAG_THRESHOLD * UI_SCALE_FAC)
      {
        moved_ = true;
      }
      if (move_shape_ && has_shape_) {
        shape_translate(shape_, cursor_p_ - move_prev_p_);
        /* Keep the drag anchored to the moved shape once Space is released. */
        press_p_ += cursor_p_ - move_prev_p_;
      }
      else if (drag_active_) {
        update_drag(shift, alt, ctrl);
      }
      move_prev_p_ = cursor_p_;
      return ShapeCreateResult::Changed;
    }

    case LEFTMOUSE: {
      if (event.val == KM_PRESS) {
        if (!drag_active_) {
          /* A keyboard/menu invocation starts the drag on the first press. */
          drag_active_ = true;
          press_p_ = event_p;
          cursor_p_ = event_p;
          press_mval_ = int2(event.mval[0], event.mval[1]);
          moved_ = false;
          move_prev_p_ = event_p;
          update_drag(false, false, false);
          return ShapeCreateResult::Changed;
        }
        return ShapeCreateResult::None;
      }
      if (event.val == KM_RELEASE) {
        if (!drag_active_ || !has_shape_) {
          return ShapeCreateResult::None;
        }
        if (!moved_) {
          /* A click without a drag draws the default-size shape at the click point. */
          shape_ = shape_at_center(press_p_);
        }
        return ShapeCreateResult::Confirmed;
      }
      return ShapeCreateResult::None;
    }

    case EVT_SPACEKEY: {
      if (event.val == KM_PRESS) {
        move_shape_ = true;
        move_prev_p_ = cursor_p_;
      }
      else if (event.val == KM_RELEASE) {
        move_shape_ = false;
      }
      return ShapeCreateResult::None;
    }

    case EVT_LEFTSHIFTKEY:
    case EVT_RIGHTSHIFTKEY:
    case EVT_LEFTALTKEY:
    case EVT_RIGHTALTKEY:
    case EVT_LEFTCTRLKEY:
    case EVT_RIGHTCTRLKEY: {
      /* Snap, from-center and circle change the shape while the cursor stands still. */
      if (drag_active_ && !move_shape_) {
        update_drag(shift, alt, ctrl);
        return ShapeCreateResult::Changed;
      }
      return ShapeCreateResult::None;
    }

    case EVT_MODAL_MAP: {
      /* Creation drags have no Apply step: confirm is a no-op so a remapped confirm key never
       * finishes a half-dragged shape. */
      if (event.val == PAINT_SHAPE_MODAL_CANCEL) {
        return ShapeCreateResult::Cancelled;
      }
      return ShapeCreateResult::None;
    }

    default:
      return ShapeCreateResult::None;
  }
}

void shape_status_set_creation(bContext *C, const ePaintShapeType type)
{
  WorkspaceStatus status(C);
  status.item(IFACE_("Drag"), ICON_MOUSE_LMB);
  status.item(IFACE_("Snap"), ICON_EVENT_SHIFT);
  if (type == PAINT_SHAPE_ELLIPSE) {
    status.item(IFACE_("Circle"), ICON_EVENT_CTRL);
  }
  else if (type == PAINT_SHAPE_RECT) {
    status.item(IFACE_("Square"), ICON_EVENT_CTRL);
  }
  status.item(IFACE_("From Center"), ICON_EVENT_ALT);
  status.item(IFACE_("Move"), ICON_EVENT_SPACEKEY);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
}

/** Offset the polyline by \a dist along its left normal (mitered joins, clamped so a sharp corner
 * cannot shoot far away). Duplicate points are dropped, so no zero-length segment has a normal. */
static Vector<float2> polyline_offset(Span<float2> points, const bool cyclic, const float dist)
{
  Vector<float2> pts;
  for (const float2 &p : points) {
    if (pts.is_empty() || math::distance(pts.last(), p) > 1e-4f) {
      pts.append(p);
    }
  }
  if (cyclic && pts.size() > 1 && math::distance(pts.first(), pts.last()) <= 1e-4f) {
    pts.remove_last();
  }
  const int num = pts.size();
  Vector<float2> out;
  if (num < 2) {
    return out;
  }
  auto segment_normal = [&](const int a, const int b) {
    const float2 d = math::normalize(pts[b] - pts[a]);
    return float2(-d.y, d.x);
  };
  for (const int i : IndexRange(num)) {
    const bool has_prev = cyclic || i > 0;
    const bool has_next = cyclic || i < num - 1;
    const float2 n_prev = has_prev ? segment_normal((i + num - 1) % num, i) : float2(0.0f);
    const float2 n_next = has_next ? segment_normal(i, (i + 1) % num) : float2(0.0f);
    float2 n = !has_prev ? n_next : (!has_next ? n_prev : n_prev + n_next);
    if (has_prev && has_next) {
      /* Miter scale is 1 / cos(half the turn angle) = 2 / |n_prev + n_next|. */
      const float len = math::length(n);
      n = len > 1e-4f ? n * (2.0f / (len * len)) : n_next;
      const float miter = math::length(n);
      if (miter > 3.0f) {
        n *= 3.0f / miter;
      }
    }
    out.append(pts[i] + n * dist);
  }

  /* Where the offset exceeds the local radius of curvature (a rounded corner narrower than the
   * width) the mitered points cross over and form a loop. The stroke's real edge there is a sharp
   * corner (the SDF iso-line), so points closer to the contour than the offset are dropped and each
   * gap is closed at the crossing of the two straight edges around it. */
  const float min_dist = std::abs(dist) * 0.98f;
  auto distance_to_contour = [&](const float2 &p) {
    float best = FLT_MAX;
    const int segment_num = cyclic ? num : num - 1;
    for (const int i : IndexRange(segment_num)) {
      const float2 &a = pts[i];
      const float2 &b = pts[(i + 1) % num];
      const float2 ab = b - a;
      const float len_sq = math::length_squared(ab);
      const float t = len_sq > 1e-12f ? math::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f) :
                                        0.0f;
      best = std::min(best, math::distance(p, a + ab * t));
    }
    return best;
  };
  Vector<bool> valid(num);
  int valid_num = 0;
  for (const int i : IndexRange(num)) {
    valid[i] = distance_to_contour(out[i]) >= min_dist;
    valid_num += valid[i] ? 1 : 0;
  }
  if (valid_num == num) {
    return out;
  }
  Vector<float2> result;
  if (valid_num < 2) {
    return result;
  }
  auto intersect = [](const float2 &a0,
                      const float2 &a1,
                      const float2 &b0,
                      const float2 &b1,
                      float2 &r_point) {
    const float2 d1 = a1 - a0;
    const float2 d2 = b1 - b0;
    const float denom = d1.x * d2.y - d1.y * d2.x;
    if (std::abs(denom) < 1e-8f) {
      return false;
    }
    const float2 delta = b0 - a0;
    const float t = (delta.x * d2.y - delta.y * d2.x) / denom;
    r_point = a0 + d1 * t;
    return true;
  };
  int first = 0;
  while (!valid[first]) {
    first++;
  }
  bool in_run = false;
  /* An open outline never wraps: it walks from the first valid point to the end. */
  const int step_num = cyclic ? num + 1 : num - first;
  for (const int k : IndexRange(step_num)) {
    const int i = (first + k) % num;
    if (!valid[i]) {
      in_run = true;
      continue;
    }
    if (in_run && result.size() >= 1) {
      /* Close the gap: prev edge (end of the result) meets the next edge (from \a i). */
      const float2 a1 = result.last();
      const float2 a0 = result.size() >= 2 ? result[result.size() - 2] : a1;
      const float2 b0 = out[i];
      const float2 b1 = out[(i + 1) % num];
      float2 corner;
      if (a0 != a1 && b0 != b1 && intersect(a0, a1, b0, b1, corner) &&
          math::distance(corner, a1) < 4.0f * std::abs(dist) + math::distance(a0, a1))
      {
        result.append(corner);
      }
    }
    in_run = false;
    if (k < num) {
      result.append(out[i]);
    }
  }
  return result;
}

void shape_draw_creation_preview(const PaintShape &shape,
                                 const float error_px,
                                 const std::function<float2(const float2 &)> &to_region,
                                 const ShapeStyle *width_guide)
{
  const Vector<ShapePolyline> outlines = shape_flatten(shape, error_px);
  Vector<float2> region_points;
  auto draw_points = [&](const Span<float2> points, const bool cyclic) {
    region_points.clear();
    region_points.reserve(points.size());
    for (const float2 &p : points) {
      region_points.append(to_region(p));
    }
    shape_draw_dashed_outline(region_points, cyclic);
  };
  for (const ShapePolyline &poly : outlines) {
    draw_points(poly.points, poly.cyclic);

    if (width_guide == nullptr || poly.points.size() < 2 || width_guide->stroke_width <= 0.0f) {
      continue;
    }
    /* The band the stroke will cover: the outlines its edges sit on. The Align mode decides which
     * side of the contour they fall on; an open outline has no inside, so it shows both edges. */
    const float w = width_guide->stroke_width;
    float side = 0.0f;
    if (poly.cyclic) {
      float area = 0.0f;
      for (const int i : poly.points.index_range()) {
        const float2 &a = poly.points[i];
        const float2 &b = poly.points[(i + 1) % poly.points.size()];
        area += a.x * b.y - b.x * a.y;
      }
      /* The left normal points inward for a counter-clockwise outline. */
      side = area >= 0.0f ? 1.0f : -1.0f;
    }
    Vector<float> offsets;
    if (!poly.cyclic || width_guide->stroke_align == PAINT_SHAPE_STROKE_ALIGN_CENTER) {
      offsets = {-0.5f * w, 0.5f * w};
    }
    else if (width_guide->stroke_align == PAINT_SHAPE_STROKE_ALIGN_INSIDE) {
      offsets = {side * w};
    }
    else {
      offsets = {-side * w};
    }
    for (const float offset : offsets) {
      const Vector<float2> edge = polyline_offset(poly.points, poly.cyclic, offset);
      if (edge.size() > 1) {
        draw_points(edge, poly.cyclic);
      }
    }
  }
}

}  // namespace blender::ed::sculpt_paint::shape
