/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the space-agnostic shape edit session; see #paint_shape_edit.hh.
 */

#include "paint_shape_edit.hh"

#include <algorithm>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"

#include "paint_shape_raster.hh"

namespace blender::ed::sculpt_paint::shape {

/** The 2D space the session shapes live in: a CanvasUV space over \a tile. */
static ShapeSpaceDesc canvas_uv_space_desc(const CanvasTile &tile)
{
  ShapeSpaceDesc desc;
  desc.type = ShapeSpaceType::CanvasUV;
  desc.ref_tile = tile.ref_tile;
  desc.ref_tile_size = tile.ref_tile_size;
  return desc;
}

ShapeEditSession::ShapeEditSession(Vector<PaintShape> shapes, const CanvasTile &tile)
{
  shapes_set(std::move(shapes), tile);
}

Vector<PaintShape> ShapeEditSession::shapes() const
{
  Vector<PaintShape> result;
  result.reserve(document_.items.size());
  for (const VectorItem &item : document_.items) {
    result.append(item.shape);
  }
  return result;
}

CanvasTile ShapeEditSession::canvas_tile() const
{
  CanvasTile tile;
  if (!document_.items.is_empty()) {
    tile.ref_tile = document_.items[0].space.ref_tile;
    tile.ref_tile_size = document_.items[0].space.ref_tile_size;
  }
  return tile;
}

void ShapeEditSession::append_shape(PaintShape shape, const CanvasTile &tile)
{
  const ShapeSpaceDesc space = canvas_uv_space_desc(tile);
  document_.items.append(VectorItem{VectorItemType::Shape, std::move(shape), space});
  /* Adding an element is an undoable change: on a fresh document this seeds entry 0 (so the
   * first gesture stays undoable); on a document that already has history it records the new
   * state, so undo removes exactly the appended shape. */
  vector_document_undo_push(document_);
}

const PaintShape *ShapeEditSession::active() const
{
  if (document_.items.is_empty()) {
    return nullptr;
  }
  return &document_.items[std::clamp(document_.active, 0, int(document_.items.size()) - 1)].shape;
}

PaintShape *ShapeEditSession::active_for_write()
{
  if (document_.items.is_empty()) {
    return nullptr;
  }
  return &document_.items[std::clamp(document_.active, 0, int(document_.items.size()) - 1)].shape;
}

VectorItem *ShapeEditSession::active_item()
{
  if (document_.items.is_empty()) {
    return nullptr;
  }
  return &document_.items[std::clamp(document_.active, 0, int(document_.items.size()) - 1)];
}

const VectorItem *ShapeEditSession::active_item() const
{
  if (document_.items.is_empty()) {
    return nullptr;
  }
  return &document_.items[std::clamp(document_.active, 0, int(document_.items.size()) - 1)];
}

void ShapeEditSession::active_set(const int index)
{
  if (!document_.items.is_empty()) {
    document_.active = std::clamp(index, 0, int(document_.items.size()) - 1);
  }
}

void ShapeEditSession::active_next()
{
  if (document_.items.size() > 1) {
    document_.active = (document_.active + 1) % document_.items.size();
  }
}

void ShapeEditSession::shapes_set(Vector<PaintShape> shapes, const CanvasTile &tile)
{
  Vector<VectorItem> items;
  items.reserve(shapes.size());
  const ShapeSpaceDesc space = canvas_uv_space_desc(tile);
  for (PaintShape &shape : shapes) {
    items.append(VectorItem{VectorItemType::Shape, std::move(shape), space});
  }
  vector_document_set_items(document_, std::move(items));
  dragging_ = false;
  transforming_ = false;
  point_dragging_ = false;
}

/** True when \a p carries fill or stroke coverage of \a shape within \a tolerance_px of its
 * outline. The style is widened by the tolerance (stroke on both sides, feather outward) so the
 * fringe the coverage test accepts is exactly the tolerance ring; dash gaps and butt caps stay
 * misses, which is what contour-accurate grabbing means. */
static bool shape_hit_contour(const PaintShape &shape,
                               const ShapeStyle &style,
                               const float2 &p,
                               const float tolerance_px)
{
  if (shape.is_empty()) {
    return false;
  }
  const float tol = std::max(tolerance_px, 0.0f);
  rctf bounds = shape_bounds_calc(shape, style);
  bounds.xmin -= tol;
  bounds.ymin -= tol;
  bounds.xmax += tol;
  bounds.ymax += tol;
  if (!BLI_rctf_isect_pt_v(&bounds, p)) {
    return false;
  }
  ShapeStyle widened = style;
  widened.stroke_width = style.stroke_width + 2.0f * tol;
  widened.feather = style.feather + tol;
  const ShapeEvaluator evaluator(Span<PaintShape>(&shape, 1),
                                 widened,
                                 bounds,
                                 ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke);
  const ShapeSample sample = evaluator.sample(p);
  return sample.fill > 0.0f || sample.stroke > 0.0f;
}

int ShapeEditSession::hit_shape(const float2 &p,
                                const ShapeStyle &style,
                                const float tolerance_px) const
{
  /* Topmost last: shapes composite in order, so the last one owns the pixel. */
  for (int i = document_.items.size() - 1; i >= 0; i--) {
    if (shape_hit_contour(document_.items[i].shape, style, p, tolerance_px)) {
      return i;
    }
  }
  return -1;
}

bool ShapeEditSession::hit_test(const float2 &p,
                                const ShapeStyle &style,
                                const float tolerance_px) const
{
  return hit_shape(p, style, tolerance_px) >= 0;
}

ShapeHandle ShapeEditSession::handle_at(const float2 &p,
                                        const float radius_px,
                                        const ShapeStyle &style) const
{
  ShapeHandle handle;
  const float tol = std::max(radius_px, 0.0f);
  const float tol_sq = tol * tol;

  /* Priority : a control point of the ACTIVE shape, then its origin, then the
   * whole-shape move handle (which may select another shape by its contour). Points of a
   * non-active shape never win; grabbing one first requires selecting that shape. */
  if (const PaintShape *active_shape = this->active()) {
    const int active_i = std::clamp(document_.active, 0, int(document_.items.size()) - 1);

    int best_spline = -1;
    int best_point = -1;
    float best_dist_sq = tol_sq * SHAPE_POINT_HIT_SCALE * SHAPE_POINT_HIT_SCALE;
    for (const int si : active_shape->splines.index_range()) {
      for (const int pi : active_shape->splines[si].points.index_range()) {
        const float dist_sq = math::distance_squared(active_shape->splines[si].points[pi].co, p);
        if (dist_sq <= best_dist_sq) {
          best_dist_sq = dist_sq;
          best_spline = si;
          best_point = pi;
        }
      }
    }
    if (best_spline >= 0) {
      handle.type = ShapeHandleType::Point;
      handle.shape_index = active_i;
      handle.spline_index = best_spline;
      handle.point_index = best_point;
      return handle;
    }

    if (math::distance_squared(shape_effective_origin(*active_shape), p) <= tol_sq) {
      handle.type = ShapeHandleType::Origin;
      handle.shape_index = active_i;
      return handle;
    }
  }

  handle.shape_index = hit_shape(p, style, tol);
  handle.type = (handle.shape_index >= 0) ? ShapeHandleType::Move : ShapeHandleType::None;
  return handle;
}

bool ShapeEditSession::drag_begin(const float2 &p,
                                  const ShapeStyle &style,
                                  const float tolerance_px)
{
  const int index = hit_shape(p, style, tolerance_px);
  if (index < 0) {
    return false;
  }
  document_.active = index;
  if (!dragging_) {
    dragging_ = true;
    gesture_dirty_ = false;
  }
  drag_last_ = p;
  return true;
}

bool ShapeEditSession::move_active_begin(const float2 &p)
{
  const PaintShape *shape = active();
  if (shape == nullptr || shape->is_empty()) {
    return false;
  }
  if (!dragging_) {
    dragging_ = true;
    gesture_dirty_ = false;
  }
  drag_last_ = p;
  return true;
}

bool ShapeEditSession::drag_update(const float2 &p, const ShapeAxisLock lock)
{
  if (!dragging_) {
    return false;
  }
  float2 delta = p - drag_last_;
  /* The raw cursor is stored (not the locked one), so releasing the lock resumes without a
   * jump. */
  drag_last_ = p;
  if (lock == ShapeAxisLock::X) {
    delta.y = 0.0f;
  }
  else if (lock == ShapeAxisLock::Y) {
    delta.x = 0.0f;
  }
  if (math::length_squared(delta) < 1e-12f) {
    return false;
  }
  if (PaintShape *shape = active_for_write()) {
    shape_translate(*shape, delta);
    gesture_dirty_ = true;
    return true;
  }
  return false;
}

void ShapeEditSession::drag_cancel()
{
  dragging_ = false;
  gesture_dirty_ = false;
  undo_pop_no_redo();
}

void ShapeEditSession::drag_end()
{
  dragging_ = false;
  if (gesture_dirty_) {
    undo_push();
  }
  gesture_dirty_ = false;
}

/**
 * Scale \a shape about \a pivot by \a factor along the axes rotated by \a local_rot. A parametric
 * Rect/Ellipse scales its half size and re-centres; a spline shape maps its points and handles.
 * The local frame makes the cage's independent X/Y follow the shape's own axes; \a local_rot is 0
 * for a uniform scale (where the frame is irrelevant).
 */
static void shape_scale_about(PaintShape &shape,
                              const float2 &pivot,
                              const float2 &factor,
                              const float local_rot)
{
  const float c = math::cos(local_rot);
  const float s = math::sin(local_rot);
  /* v -> pivot + R * diag(factor) * R^-1 * (v - pivot). */
  auto map = [&](const float2 &v) {
    const float2 rel = v - pivot;
    const float2 local(rel.x * c + rel.y * s, -rel.x * s + rel.y * c);
    const float2 scaled(local.x * factor.x, local.y * factor.y);
    return pivot + float2(scaled.x * c - scaled.y * s, scaled.x * s + scaled.y * c);
  };
  if (shape.is_parametric()) {
    shape.center = map(shape.center);
    shape.half_size = float2(shape.half_size.x * std::abs(factor.x),
                             shape.half_size.y * std::abs(factor.y));
    /* Corner radii are per-corner scalars, so they take the mean factor; exact per-axis radii
     * arrive with the corner handles. */
    shape.corner_radius *= 0.5f * (std::abs(factor.x) + std::abs(factor.y));
    return;
  }
  for (ShapeSpline &spline : shape.splines) {
    for (ShapePoint &point : spline.points) {
      point.co = map(point.co);
      point.handle_left = map(point.handle_left);
      point.handle_right = map(point.handle_right);
    }
  }
}

bool ShapeEditSession::transform_begin(const ShapeTransformMode mode,
                                       const float2 &p,
                                       const ShapeStyle & /*style*/,
                                       const ShapeScaleAxis scale_axis)
{
  const PaintShape *shape = active();
  if (shape == nullptr || shape->is_empty()) {
    return false;
  }
  transforming_ = true;
  gesture_dirty_ = false;
  transform_mode_ = mode;
  transform_scale_axis_ = scale_axis;
  /* The pivot is the shape's origin, not its bounds center. */
  transform_pivot_ = shape_effective_origin(*shape);
  transform_start_cursor_ = p;
  transform_start_shape_ = *shape;
  return true;
}

bool ShapeEditSession::transform_update(const float2 &p, const ShapeAxisLock lock)
{
  if (!transforming_) {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr) {
    return false;
  }
  *shape = transform_start_shape_;
  /* Local axes of the independent scale: the shape's own rotation for a parametric Rect/Ellipse,
   * identity for a spline shape (its rotation, if any, is baked into the points). A uniform scale
   * ignores the frame. */
  const float local_rot = transform_start_shape_.is_parametric() ?
                              transform_start_shape_.rotation :
                              0.0f;
  if (transform_mode_ == ShapeTransformMode::Rotate) {
    /* Rotation ignores the axis lock: X/Y only constrain move and scale. */
    const float2 start_rel = transform_start_cursor_ - transform_pivot_;
    const float2 cur_rel = p - transform_pivot_;
    if (math::length_squared(start_rel) < 1e-12f || math::length_squared(cur_rel) < 1e-12f) {
      return false;
    }
    const float dangle = math::atan2(cur_rel.y, cur_rel.x) -
                         math::atan2(start_rel.y, start_rel.x);
    if (std::abs(dangle) < 1e-9f) {
      return false;
    }
    shape_rotate_about(*shape, transform_pivot_, dangle);
    gesture_dirty_ = true;
    return true;
  }
  if (transform_mode_ == ShapeTransformMode::ScaleFree) {
    /* Independent X/Y: project both the grab point and the cursor onto the local axes and scale
     * per axis, so a rotated shape stretches along its own width / height. */
    const float2 start_rel = transform_start_cursor_ - transform_pivot_;
    const float2 cur_rel = p - transform_pivot_;
    const float c = math::cos(local_rot);
    const float s = math::sin(local_rot);
    const float2 start_local(start_rel.x * c + start_rel.y * s,
                             -start_rel.x * s + start_rel.y * c);
    const float2 cur_local(cur_rel.x * c + cur_rel.y * s, -cur_rel.x * s + cur_rel.y * c);
    float2 factor(1.0f);
    if (std::abs(start_local.x) > 1e-6f) {
      factor.x = std::clamp(std::abs(cur_local.x) / std::abs(start_local.x), 0.001f, 1000.0f);
    }
    if (std::abs(start_local.y) > 1e-6f) {
      factor.y = std::clamp(std::abs(cur_local.y) / std::abs(start_local.y), 0.001f, 1000.0f);
    }
    /* An edge handle drives one local axis only (the other stays at 1). */
    if (transform_scale_axis_ == ShapeScaleAxis::X) {
      factor.y = 1.0f;
    }
    else if (transform_scale_axis_ == ShapeScaleAxis::Y) {
      factor.x = 1.0f;
    }
    if (std::abs(factor.x - 1.0f) < 1e-6f && std::abs(factor.y - 1.0f) < 1e-6f) {
      return false;
    }
    shape_scale_about(*shape, transform_pivot_, factor, local_rot);
    gesture_dirty_ = true;
    return true;
  }
  const float dist_start = math::distance(transform_start_cursor_, transform_pivot_);
  if (dist_start < 1e-6f) {
    return false;
  }
  /* Distances stay positive, so the gesture never mirrors; extreme factors are clamped. An
   * axis lock measures along that axis alone, so off-axis cursor travel cannot leak in. */
  float2 factor_xy(1.0f);
  if (lock == ShapeAxisLock::X) {
    const float start_x = std::abs(transform_start_cursor_.x - transform_pivot_.x);
    if (start_x < 1e-6f) {
      return false;
    }
    factor_xy.x = std::clamp(std::abs(p.x - transform_pivot_.x) / start_x, 0.001f, 1000.0f);
  }
  else if (lock == ShapeAxisLock::Y) {
    const float start_y = std::abs(transform_start_cursor_.y - transform_pivot_.y);
    if (start_y < 1e-6f) {
      return false;
    }
    factor_xy.y = std::clamp(std::abs(p.y - transform_pivot_.y) / start_y, 0.001f, 1000.0f);
  }
  else {
    const float factor = std::clamp(
        math::distance(p, transform_pivot_) / dist_start, 0.001f, 1000.0f);
    factor_xy = float2(factor);
  }
  if (std::abs(factor_xy.x - 1.0f) < 1e-6f && std::abs(factor_xy.y - 1.0f) < 1e-6f) {
    return false;
  }
  /* The S gesture (and its X/Y lock) scales about the GLOBAL axes, as before; only the cage's
   * ScaleFree uses the shape's local frame. */
  shape_scale_about(*shape, transform_pivot_, factor_xy, 0.0f);
  gesture_dirty_ = true;
  return true;
}

void ShapeEditSession::transform_end()
{
  transforming_ = false;
  if (gesture_dirty_) {
    undo_push();
  }
  gesture_dirty_ = false;
}

void ShapeEditSession::transform_cancel()
{
  transforming_ = false;
  gesture_dirty_ = false;
  undo_pop_no_redo();
}

bool ShapeEditSession::point_drag_begin(const float2 &p, const float tolerance_px)
{
  PaintShape *active_shape = active_for_write();
  if (active_shape == nullptr) {
    return false;
  }
  if (active_shape->is_parametric() && !active_shape->has_analytic_sdf()) {
    /* Attempting to edit the points of a generated Polygon/Star/Arc bakes it into a plain spline
     * (one undo step), after which it behaves like a hand-authored outline. */
    shape_convert_to_spline(*active_shape);
    undo_push();
  }
  const PaintShape *shape = active();
  if (shape == nullptr) {
    return false;
  }
  const float tol = std::max(tolerance_px, 0.0f);
  int best_spline = -1;
  int best_point = -1;
  float best_dist_sq = tol * tol * SHAPE_POINT_HIT_SCALE * SHAPE_POINT_HIT_SCALE;
  for (const int si : shape->splines.index_range()) {
    for (const int pi : shape->splines[si].points.index_range()) {
      const float dist_sq = math::distance_squared(shape->splines[si].points[pi].co, p);
      if (dist_sq <= best_dist_sq) {
        best_dist_sq = dist_sq;
        best_spline = si;
        best_point = pi;
      }
    }
  }
  if (best_spline < 0) {
    return false;
  }
  point_dragging_ = true;
  gesture_dirty_ = false;
  point_spline_ = best_spline;
  point_index_ = best_point;
  point_grab_delta_ = shape->splines[best_spline].points[best_point].co - p;
  return true;
}

bool ShapeEditSession::point_drag_update(const float2 &p)
{
  if (!point_dragging_) {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr || point_spline_ < 0 || point_spline_ >= shape->splines.size()) {
    return false;
  }
  ShapeSpline &spline = shape->splines[point_spline_];
  if (point_index_ < 0 || point_index_ >= spline.points.size()) {
    return false;
  }
  ShapePoint &point = spline.points[point_index_];
  const float2 target = p + point_grab_delta_;
  const float2 shift = target - point.co;
  if (math::length_squared(shift) < 1e-12f) {
    return false;
  }
  /* The handles follow the point rigidly, preserving the curve's shape. */
  point.co = target;
  point.handle_left += shift;
  point.handle_right += shift;
  gesture_dirty_ = true;
  return true;
}

void ShapeEditSession::point_drag_cancel()
{
  point_dragging_ = false;
  gesture_dirty_ = false;
  undo_pop_no_redo();
}

void ShapeEditSession::point_drag_end()
{
  point_dragging_ = false;
  if (gesture_dirty_) {
    undo_push();
  }
  gesture_dirty_ = false;
}

bool ShapeEditSession::insert_spot_find(const float2 &p,
                                        const float tolerance_px,
                                        int &r_spline,
                                        int &r_segment,
                                        float &r_t) const
{
  const PaintShape *shape = active();
  if (shape == nullptr || shape->is_parametric() || shape->splines.is_empty()) {
    return false;
  }
  constexpr int SAMPLES = 32;
  const float tol = std::max(tolerance_px, 0.0f);

  int best_spline = -1;
  int best_segment = -1;
  float best_t = 0.0f;
  float best_dist_sq = tol * tol;
  for (const int si : shape->splines.index_range()) {
    const ShapeSpline &spline = shape->splines[si];
    const int count = spline.points.size();
    const int segment_num = spline.cyclic ? count : count - 1;
    for (const int i : IndexRange(std::max(segment_num, 0))) {
      const int j = (i + 1) % count;
      const ShapePoint &pi = spline.points[i];
      const ShapePoint &pj = spline.points[j];
      const bool straight = pj.corner || !spline.is_bezier;
      float2 c1 = pi.co;
      float2 c2 = pj.co;
      if (!straight) {
        float2 unused;
        shape_spline_point_handles_get(spline, i, unused, c1);
        shape_spline_point_handles_get(spline, j, c2, unused);
      }
      /* Sample the segment; a straight one is exact with the same loop (its handles sit on the
       * end points). */
      float2 prev = pi.co;
      for (const int s : IndexRange(1, SAMPLES)) {
        const float t = float(s) / float(SAMPLES);
        const float mt = 1.0f - t;
        const float2 cur = pi.co * (mt * mt * mt) + c1 * (3.0f * mt * mt * t) +
                           c2 * (3.0f * mt * t * t) + pj.co * (t * t * t);
        const float2 ab = cur - prev;
        const float len_sq = math::length_squared(ab);
        const float along = len_sq > 1e-20f ? math::clamp(math::dot(p - prev, ab) / len_sq, 0.0f, 1.0f) :
                                              0.0f;
        const float dist_sq = math::distance_squared(p, prev + ab * along);
        if (dist_sq <= best_dist_sq) {
          best_dist_sq = dist_sq;
          best_spline = si;
          best_segment = i;
          best_t = (float(s - 1) + along) / float(SAMPLES);
        }
        prev = cur;
      }
    }
  }
  r_spline = best_spline;
  r_segment = best_segment;
  r_t = best_t;
  return best_spline >= 0;
}

bool ShapeEditSession::insert_preview(const float2 &p,
                                      const float tolerance_px,
                                      float2 &r_point,
                                      float2 &r_tangent) const
{
  int spline_index, segment;
  float t;
  if (!this->insert_spot_find(p, tolerance_px, spline_index, segment, t)) {
    return false;
  }
  const ShapeSpline &spline = active()->splines[spline_index];
  const int j = (segment + 1) % spline.points.size();
  const ShapePoint &pi = spline.points[segment];
  const ShapePoint &pj = spline.points[j];
  float2 c1 = pi.co;
  float2 c2 = pj.co;
  if (!(pj.corner || !spline.is_bezier)) {
    float2 unused;
    shape_spline_point_handles_get(spline, segment, unused, c1);
    shape_spline_point_handles_get(spline, j, c2, unused);
  }
  const float mt = 1.0f - t;
  r_point = pi.co * (mt * mt * mt) + c1 * (3.0f * mt * mt * t) + c2 * (3.0f * mt * t * t) +
            pj.co * (t * t * t);
  /* Derivative of the cubic; a degenerate one falls back to the chord. */
  float2 tangent = (c1 - pi.co) * (3.0f * mt * mt) + (c2 - c1) * (6.0f * mt * t) +
                   (pj.co - c2) * (3.0f * t * t);
  if (math::length_squared(tangent) < 1e-12f) {
    tangent = pj.co - pi.co;
  }
  r_tangent = tangent;
  return true;
}

bool ShapeEditSession::point_insert(const float2 &p, const float tolerance_px)
{
  PaintShape *shape = active_for_write();
  int best_spline, best_segment;
  float best_t;
  if (shape == nullptr ||
      !this->insert_spot_find(p, tolerance_px, best_spline, best_segment, best_t))
  {
    return false;
  }

  ShapeSpline &spline = shape->splines[best_spline];
  const int i = best_segment;
  const int j = (i + 1) % spline.points.size();
  const float t = best_t;
  const ShapePoint pi = spline.points[i];
  const ShapePoint pj = spline.points[j];
  const bool straight = pj.corner || !spline.is_bezier;

  ShapePoint inserted;
  inserted.width_factor = math::interpolate(pi.width_factor, pj.width_factor, t);
  if (straight) {
    inserted.co = math::interpolate(pi.co, pj.co, t);
    inserted.handle_left = inserted.co;
    inserted.handle_right = inserted.co;
    /* The new corner keeps both halves straight. */
    inserted.corner = true;
    inserted.auto_handles = false;
  }
  else {
    /* De Casteljau split: both halves together trace exactly the original segment. */
    float2 left_i, right_i, left_j, right_j;
    shape_spline_point_handles_get(spline, i, left_i, right_i);
    shape_spline_point_handles_get(spline, j, left_j, right_j);
    const float2 q0 = math::interpolate(pi.co, right_i, t);
    const float2 q1 = math::interpolate(right_i, left_j, t);
    const float2 q2 = math::interpolate(left_j, pj.co, t);
    const float2 r0 = math::interpolate(q0, q1, t);
    const float2 r1 = math::interpolate(q1, q2, t);
    inserted.co = math::interpolate(r0, r1, t);
    inserted.handle_left = r0;
    inserted.handle_right = r1;
    inserted.auto_handles = false;
    inserted.corner = false;

    ShapePoint &start = spline.points[i];
    start.handle_left = left_i;
    start.handle_right = q0;
    start.auto_handles = false;
    ShapePoint &end = spline.points[j];
    end.handle_left = q2;
    end.handle_right = right_j;
    end.auto_handles = false;
  }
  /* The wrap-around segment of a cyclic spline ends at point 0: append instead. */
  const int insert_at = (j == 0) ? int(spline.points.size()) : j;
  spline.points.insert(insert_at, inserted);

  undo_push();
  return true;
}

bool ShapeEditSession::origin_drag_begin(const float2 &p)
{
  const PaintShape *shape = active();
  if (shape == nullptr || shape->is_empty()) {
    return false;
  }
  if (!origin_dragging_) {
    origin_dragging_ = true;
    gesture_dirty_ = false;
  }
  /* The pivot may currently be a computed mean (not stored): grab from it so the marker follows
   * the cursor without a jump. */
  origin_grab_delta_ = shape_effective_origin(*shape) - p;
  return true;
}

bool ShapeEditSession::origin_drag_update(const float2 &p)
{
  if (!origin_dragging_) {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr) {
    return false;
  }
  const float2 target = p + origin_grab_delta_;
  if (shape->origin_is_custom && math::length_squared(target - shape->origin) < 1e-12f) {
    return false;
  }
  shape->origin = target;
  shape->origin_is_custom = true;
  gesture_dirty_ = true;
  return true;
}

void ShapeEditSession::origin_drag_cancel()
{
  origin_dragging_ = false;
  gesture_dirty_ = false;
  undo_pop_no_redo();
}

void ShapeEditSession::origin_drag_end()
{
  origin_dragging_ = false;
  if (gesture_dirty_) {
    undo_push();
  }
  gesture_dirty_ = false;
}

bool ShapeEditSession::origin_reset()
{
  PaintShape *shape = active_for_write();
  if (shape == nullptr || !shape->origin_is_custom) {
    return false;
  }
  shape->origin_is_custom = false;
  undo_push();
  return true;
}

void ShapeEditSession::undo_push()
{
  vector_document_undo_push(document_);
}

bool ShapeEditSession::undo_pop()
{
  return vector_document_undo_back(document_);
}

bool ShapeEditSession::redo_pop()
{
  return vector_document_undo_forward(document_);
}

void ShapeEditSession::undo_clear()
{
  document_.undo_steps.clear();
  document_.undo_step_current = -1;
  document_.undo_anchor_trimmed = false;
  /* Re-seed entry 0 with the current state, so the next gesture is undoable. */
  vector_document_undo_push(document_);
}

void ShapeEditSession::undo_pop_no_redo()
{
  /* Restore the entry at the cursor; the history is untouched (a cancelled gesture leaves no
   * entry behind). */
  vector_document_undo_restore(document_);
}

void shape_apply_size_to_active(ShapeEditSession &edit, const float2 &size)
{
  if (PaintShape *shape = edit.active_for_write()) {
    shape_set_size(*shape, size);
  }
}

void shape_apply_corner_radius_to_active(ShapeEditSession &edit, const float4 &radius)
{
  PaintShape *shape = edit.active_for_write();
  if (shape == nullptr || shape->type != PAINT_SHAPE_RECT || !shape->is_parametric()) {
    return;
  }
  shape->corner_radius = radius;
  shape_clamp_corner_radius(*shape);
}

void shape_apply_style_edits_to_active(ShapeEditSession &edit,
                                       const ShapeStyle &previous,
                                       const ShapeStyle &current)
{
  if (!math::is_equal(previous.size, current.size)) {
    shape_apply_size_to_active(edit, current.size);
  }
  if (previous.rotation != current.rotation) {
    shape_apply_rotation_to_active(edit, current.rotation - previous.rotation);
  }
  if (!math::is_equal(previous.corner_radius, current.corner_radius)) {
    shape_apply_corner_radius_to_active(edit, current.corner_radius);
  }
}

void shape_apply_rotation_to_active(ShapeEditSession &edit, const float raw_dangle)
{
  PaintShape *shape = edit.active_for_write();
  if (shape == nullptr || shape->is_empty()) {
    return;
  }
  /* Wrapped so a slider crossing the -pi / pi seam turns the shape by the short arc. */
  const float dangle = angle_wrap_rad(raw_dangle);
  if (dangle == 0.0f) {
    return;
  }
  shape_rotate_about(*shape, shape_effective_origin(*shape), dangle);
}

void shape_sync_settings_from_active(ShapeEditSession &edit,
                                     PaintShapeSettings &settings,
                                     ShapeStyle &style)
{
  const PaintShape *shape = edit.active();
  if (shape == nullptr) {
    return;
  }
  if (shape->is_parametric()) {
    settings.size[0] = shape->half_size.x * 2.0f;
    settings.size[1] = shape->half_size.y * 2.0f;
    settings.rotation = angle_wrap_rad(shape->rotation);
  }
  else {
    settings.rotation = 0.0f;
  }
  style.size = float2(settings.size[0], settings.size[1]);
  style.rotation = settings.rotation;
}

}  // namespace blender::ed::sculpt_paint::shape
