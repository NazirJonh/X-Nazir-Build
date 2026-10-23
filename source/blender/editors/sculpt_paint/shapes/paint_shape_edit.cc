/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the space-agnostic shape edit session and of the shared GPU overlay of the
 * shape drawing tools; see #paint_shape_edit.hh.
 */

#include "paint_shape_edit.hh"

#include <algorithm>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "BKE_curves.hh"

#include "DNA_userdef_types.h" /* UI_SCALE_FAC */

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

#include "ED_paint_curve_draw.hh"

#include "paint_shape_render.hh"

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
    const int clamped = std::clamp(index, 0, int(document_.items.size()) - 1);
    if (clamped != document_.active) {
      this->selected_point_clear();
    }
    document_.active = clamped;
  }
}

void ShapeEditSession::active_next()
{
  if (document_.items.size() > 1) {
    document_.active = (document_.active + 1) % document_.items.size();
    this->selected_point_clear();
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
  handle_dragging_ = false;
  this->selected_point_clear();
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

namespace {

/**
 * Screen-space view of a shape's splines for the shared control-point layer. Points are
 * flattened across the splines; the handles come from the shape's own Catmull-Rom rule, so
 * the shared builder draws and picks exactly what the outline evaluates.
 */
class ShapeSplineScreenSource : public control_points::Source {
 public:
  ShapeSplineScreenSource(const PaintShape &shape,
                          FunctionRef<float2(const PaintShape &, const float2 &)> to_screen,
                          const bool show_handles,
                          const int selected_spline,
                          const int selected_point)
      : shape_(shape),
        to_screen_(to_screen),
        show_handles_(show_handles),
        selected_spline_(selected_spline),
        selected_point_(selected_point)
  {
    for (const int spline_index : shape.splines.index_range()) {
      const ShapeSpline &spline = shape.splines[spline_index];
      for (const int point_index : spline.points.index_range()) {
        point_spline_.append(spline_index);
        point_spline_point_.append(point_index);
      }
    }
  }

  int points_num() const override
  {
    return int(point_spline_.size());
  }

  float2 point_position(const int point) const override
  {
    return to_screen_(shape_, this->shape_point(point).co);
  }

  void point_handles(const int point, float2 &r_left, float2 &r_right) const override
  {
    const ShapeSpline &spline = shape_.splines[point_spline_[point]];
    float2 left, right;
    shape_spline_point_handles_get(spline, point_spline_point_[point], left, right);
    r_left = to_screen_(shape_, left);
    r_right = to_screen_(shape_, right);
  }

  int8_t handle_type(const int point, bool /*left*/) const override
  {
    const ShapePoint &shape_point = this->shape_point(point);
    if (shape_point.auto_handles) {
      return BEZIER_HANDLE_AUTO;
    }
    return shape_point_is_vector(shape_point) ? BEZIER_HANDLE_VECTOR : BEZIER_HANDLE_FREE;
  }

  uint8_t selection(const int point) const override
  {
    if (point_spline_[point] == selected_spline_ &&
        point_spline_point_[point] == selected_point_)
    {
      return SEL_F2;
    }
    return 0;
  }

  bool handle_visible(const int point, const bool left) const override
  {
    if (!show_handles_) {
      return false;
    }
    const ShapeSpline &spline = shape_.splines[point_spline_[point]];
    float2 handle_left, handle_right;
    shape_spline_point_handles_get(
        spline, point_spline_point_[point], handle_left, handle_right);
    const float2 &handle = left ? handle_left : handle_right;
    /* A collapsed handle (a Vector point) is not a grabbable knob; the point itself owns that
     * spot, exactly as the session's own drawing skipped it. */
    return math::distance_squared(handle, this->shape_point(point).co) >= 1e-8f;
  }

  int segments_num() const override
  {
    /* The session keeps its dashed outline; solid wires are not drawn, and the insert hit test
     * has its own exact Bezier parameter. */
    return 0;
  }

  void segment_polyline(int /*segment*/, Vector<float2> & /*r_out*/) const override {}

  int point_spline(const int point) const
  {
    return point_spline_[point];
  }
  int point_index(const int point) const
  {
    return point_spline_point_[point];
  }

 private:
  const ShapePoint &shape_point(const int point) const
  {
    return shape_.splines[point_spline_[point]].points[point_spline_point_[point]];
  }

  const PaintShape &shape_;
  FunctionRef<float2(const PaintShape &, const float2 &)> to_screen_;
  bool show_handles_;
  int selected_spline_;
  int selected_point_;
  Vector<int> point_spline_;
  Vector<int> point_spline_point_;
};

}  // namespace

void shape_control_points_handles_get(
    const PaintShape &shape,
    FunctionRef<float2(const PaintShape &, const float2 &)> to_screen,
    const bool show_handles,
    const int selected_spline,
    const int selected_point,
    const control_points::BuildOptions &options,
    PaintCurveScreenHandles &r_out)
{
  if (shape.splines.is_empty()) {
    r_out.points.clear();
    r_out.segments.clear();
    return;
  }
  ShapeSplineScreenSource source(
      shape, to_screen, show_handles, selected_spline, selected_point);
  control_points::handles_build(source, options, r_out);
}

bool shape_control_point_pick(const PaintShape &shape,
                              const float2 &p,
                              const float tolerance_px,
                              const bool show_handles,
                              const int selected_spline,
                              const int selected_point,
                              int &r_spline,
                              int &r_point,
                              control_points::Part &r_part)
{
  r_spline = -1;
  r_point = -1;
  r_part = control_points::Part::None;
  if (shape.splines.is_empty()) {
    return false;
  }
  const auto identity = [](const PaintShape & /*shape*/, const float2 &co) { return co; };
  ShapeSplineScreenSource source(
      shape, identity, show_handles, selected_spline, selected_point);
  const control_points::Pick pick = control_points::pick_point(
      source, p, tolerance_px, control_points::Metric::Euclidean);
  if (pick.point < 0) {
    return false;
  }
  r_spline = source.point_spline(pick.point);
  r_point = source.point_index(pick.point);
  r_part = pick.part;
  return true;
}

ShapeHandle ShapeEditSession::handle_at(const float2 &p,
                                        const float radius_px,
                                        const ShapeStyle &style) const
{
  ShapeHandle handle;
  const float tol = std::max(radius_px, 0.0f);
  const float tol_sq = tol * tol;

  /* Priority : a control point or Bézier handle of the ACTIVE shape (the shared pick rule
   * decides between the pivot and the two knobs), then its origin, then the whole-shape move
   * handle (which may select another shape by its contour). Points of a non-active shape never
   * win; grabbing one first requires selecting that shape. */
  if (const PaintShape *active_shape = this->active()) {
    const int active_i = std::clamp(document_.active, 0, int(document_.items.size()) - 1);

    int selected_spline, selected_point;
    this->selected_point(selected_spline, selected_point);
    int nearest_spline = -1;
    int nearest_point = -1;
    control_points::Part nearest_part = control_points::Part::None;
    if (shape_control_point_pick(*active_shape,
                                 p,
                                 tol * SHAPE_POINT_HIT_SCALE,
                                 handles_edit_,
                                 selected_spline,
                                 selected_point,
                                 nearest_spline,
                                 nearest_point,
                                 nearest_part))
    {
      handle.shape_index = active_i;
      handle.spline_index = nearest_spline;
      handle.point_index = nearest_point;
      if (nearest_part == control_points::Part::Point) {
        handle.type = ShapeHandleType::Point;
      }
      else {
        handle.type = ShapeHandleType::Handle;
        handle.handle_side = (nearest_part == control_points::Part::Left) ? ShapeHandleSide::Left :
                                                                           ShapeHandleSide::Right;
      }
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
    /* Attempting to edit the points of a generated Polygon bakes it into a plain spline
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
  selected_spline_ = best_spline;
  selected_point_ = best_point;
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
  Vector<float2> polyline;
  for (const int si : shape->splines.index_range()) {
    const ShapeSpline &spline = shape->splines[si];
    const int count = spline.points.size();
    const int segment_num = spline.cyclic ? count : count - 1;
    for (const int i : IndexRange(std::max(segment_num, 0))) {
      const ShapeSegment segment = shape_spline_segment_get(spline, i);
      /* A straight segment is exact with its two end points; a curved one is sampled uniformly in
       * the Bezier parameter, so the hit's polyline parameter is that parameter. */
      polyline.resize(segment.straight ? 1 : SAMPLES);
      if (!segment.straight) {
        bke::curves::bezier::evaluate_segment(segment.start,
                                              segment.handle_start,
                                              segment.handle_end,
                                              segment.end,
                                              polyline.as_mutable_span());
      }
      else {
        polyline[0] = segment.start;
      }
      polyline.append(segment.end);

      PaintCurvePolylineHit hit;
      if (ED_paint_curve_polyline_closest(polyline, p, hit) && hit.dist_sq <= best_dist_sq) {
        best_dist_sq = hit.dist_sq;
        best_spline = si;
        best_segment = i;
        best_t = hit.param_t;
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
  int spline_index, segment_index;
  float t;
  if (!this->insert_spot_find(p, tolerance_px, spline_index, segment_index, t)) {
    return false;
  }
  const ShapeSegment segment = shape_spline_segment_get(active()->splines[spline_index],
                                                        segment_index);
  const float2 chord = segment.end - segment.start;
  if (segment.straight) {
    r_point = math::interpolate(segment.start, segment.end, t);
    r_tangent = chord;
    return true;
  }
  /* The knot insertion yields the split point and the two new handles around it, which are
   * collinear with the curve tangent there. */
  const bke::curves::bezier::Insertion insertion = bke::curves::bezier::insert(
      float3(segment.start, 0.0f),
      float3(segment.handle_start, 0.0f),
      float3(segment.handle_end, 0.0f),
      float3(segment.end, 0.0f),
      t);
  r_point = insertion.position.xy();
  const float2 tangent = insertion.right_handle.xy() - insertion.left_handle.xy();
  /* A degenerate tangent falls back to the chord. */
  r_tangent = math::length_squared(tangent) < 1e-12f ? chord : tangent;
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
  const ShapeSegment segment = shape_spline_segment_get(spline, i);

  ShapePoint inserted;
  inserted.width_factor = math::interpolate(pi.width_factor, pj.width_factor, t);
  if (segment.straight) {
    inserted.co = math::interpolate(pi.co, pj.co, t);
    inserted.handle_left = inserted.co;
    inserted.handle_right = inserted.co;
    /* The new corner keeps both halves straight. */
    inserted.corner = true;
    inserted.auto_handles = false;
  }
  else {
    /* Both halves together trace exactly the original segment. The outer handles of the
     * neighbors are resolved before the points leave auto mode. */
    float2 left_i, right_j, unused;
    shape_spline_point_handles_get(spline, i, left_i, unused);
    shape_spline_point_handles_get(spline, j, unused, right_j);
    const bke::curves::bezier::Insertion insertion = bke::curves::bezier::insert(
        float3(segment.start, 0.0f),
        float3(segment.handle_start, 0.0f),
        float3(segment.handle_end, 0.0f),
        float3(segment.end, 0.0f),
        t);
    inserted.co = insertion.position.xy();
    inserted.handle_left = insertion.left_handle.xy();
    inserted.handle_right = insertion.right_handle.xy();
    inserted.auto_handles = false;
    inserted.corner = false;

    ShapePoint &start = spline.points[i];
    start.handle_left = left_i;
    start.handle_right = insertion.handle_prev.xy();
    start.auto_handles = false;
    ShapePoint &end = spline.points[j];
    end.handle_left = insertion.handle_next.xy();
    end.handle_right = right_j;
    end.auto_handles = false;
  }
  /* The wrap-around segment of a cyclic spline ends at point 0: append instead. */
  const int insert_at = (j == 0) ? int(spline.points.size()) : j;
  spline.points.insert(insert_at, inserted);
  selected_spline_ = best_spline;
  selected_point_ = insert_at;

  undo_push();
  return true;
}

bool ShapeEditSession::point_delete()
{
  int spline_index, point_index;
  if (!this->selected_point(spline_index, point_index)) {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr || spline_index < 0 || spline_index >= shape->splines.size()) {
    return false;
  }
  ShapeSpline &spline = shape->splines[spline_index];
  /* An open spline needs two points to stay an outline, a closed one needs three. */
  const int min_points = spline.cyclic ? 3 : 2;
  if (point_index < 0 || point_index >= spline.points.size() ||
      spline.points.size() <= min_points)
  {
    return false;
  }
  spline.points.remove(point_index);
  if (spline.points.is_empty()) {
    this->selected_point_clear();
  }
  else {
    selected_point_ = std::min(point_index, int(spline.points.size()) - 1);
  }
  undo_push();
  return true;
}

void ShapeEditSession::handles_edit_set(const bool enabled)
{
  if (handles_edit_ == enabled) {
    return;
  }
  handles_edit_ = enabled;
  if (!enabled) {
    this->selected_point_clear();
  }
}

bool ShapeEditSession::selected_point(int &r_spline, int &r_point) const
{
  r_spline = selected_spline_;
  r_point = selected_point_;
  if (selected_spline_ < 0 || selected_point_ < 0) {
    return false;
  }
  const PaintShape *shape = this->active();
  if (shape == nullptr || selected_spline_ >= shape->splines.size()) {
    return false;
  }
  return selected_point_ < shape->splines[selected_spline_].points.size();
}

void ShapeEditSession::selected_point_set(const int spline, const int point)
{
  const PaintShape *shape = this->active();
  if (shape == nullptr || spline < 0 || spline >= shape->splines.size()) {
    return;
  }
  if (point < 0 || point >= shape->splines[spline].points.size()) {
    return;
  }
  selected_spline_ = spline;
  selected_point_ = point;
}

void ShapeEditSession::selected_point_clear()
{
  selected_spline_ = -1;
  selected_point_ = -1;
}

/** Make a non-Bézier spline editable with handles, keeping the current straight outline. The Line
 * builder leaves its points with zero handles; those would pull the curve to the origin once the
 * spline is evaluated as Bézier, so collapse them onto their points instead. */
static void spline_make_bezier_editable(ShapeSpline &spline)
{
  if (spline.is_bezier) {
    return;
  }
  spline.is_bezier = true;
  for (ShapePoint &point : spline.points) {
    if (!point.auto_handles && point.handle_left == float2(0.0f) &&
        point.handle_right == float2(0.0f))
    {
      point.handle_left = point.co;
      point.handle_right = point.co;
    }
  }
}

bool ShapeEditSession::point_type_set(const ShapePointType type)
{
  int spline_index, point_index;
  if (!this->selected_point(spline_index, point_index)) {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr) {
    return false;
  }
  ShapeSpline &spline = shape->splines[spline_index];
  ShapePoint &point = spline.points[point_index];
  /* A non-Bézier spline reports its points as Auto even though its segments are enforced
   * straight, so choosing Auto there must still convert and smooth it. */
  if (shape_point_type_get(point) == type && spline.is_bezier &&
      !(type == ShapePointType::Auto && point.corner))
  {
    return false;
  }
  spline_make_bezier_editable(spline);

  const int count = int(spline.points.size());
  const bool has_next = spline.cyclic || point_index + 1 < count;
  ShapePoint &next = spline.points[(point_index + 1) % count];

  switch (type) {
    case ShapePointType::Auto:
      point.auto_handles = true;
      point.corner = false;
      /* Smooth both adjacent segments: the outgoing one is straight-gated by the next point's
       * incoming corner flag. */
      if (has_next) {
        next.corner = false;
      }
      break;
    case ShapePointType::Vector:
      point.auto_handles = false;
      point.corner = true;
      point.handle_left = point.co;
      point.handle_right = point.co;
      break;
    case ShapePointType::Free: {
      /* Free starts from the handles the point has right now: an Auto point keeps its smooth
       * tangent (stored handles may be zero) and the user then drags them freely. Collapsing to
       * the pivot instead would silently turn the point into a Vector-like corner. */
      if (point.auto_handles) {
        float2 left, right;
        shape_spline_point_handles_get(spline, point_index, left, right);
        point.handle_left = left;
        point.handle_right = right;
      }
      point.auto_handles = false;
      point.corner = false;
      break;
    }
  }
  undo_push();
  return true;
}

bool ShapeEditSession::handle_drag_begin(const ShapeHandle &handle,
                                         const float2 &p,
                                         const float /*tolerance_px*/)
{
  if (!handles_edit_ || handle.type != ShapeHandleType::Handle ||
      handle.handle_side == ShapeHandleSide::None)
  {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr || handle.spline_index < 0 ||
      handle.spline_index >= shape->splines.size())
  {
    return false;
  }
  ShapeSpline &spline = shape->splines[handle.spline_index];
  if (handle.point_index < 0 || handle.point_index >= spline.points.size()) {
    return false;
  }
  spline_make_bezier_editable(spline);

  const ShapePoint &point = spline.points[handle.point_index];
  float2 left, right;
  shape_spline_point_handles_get(spline, handle.point_index, left, right);
  const float2 knob = (handle.handle_side == ShapeHandleSide::Left) ? left : right;

  handle_dragging_ = true;
  gesture_dirty_ = false;
  handle_spline_ = handle.spline_index;
  handle_point_ = handle.point_index;
  handle_side_ = handle.handle_side;
  handle_grab_delta_ = knob - p;
  handle_other_start_ = (handle.handle_side == ShapeHandleSide::Left) ? right : left;
  handle_other_length_ = math::length(handle_other_start_ - point.co);
  this->selected_point_set(handle.spline_index, handle.point_index);
  return true;
}

bool ShapeEditSession::handle_drag_update(const float2 &p, const bool align)
{
  if (!handle_dragging_) {
    return false;
  }
  PaintShape *shape = active_for_write();
  if (shape == nullptr || handle_spline_ < 0 || handle_spline_ >= shape->splines.size()) {
    return false;
  }
  ShapeSpline &spline = shape->splines[handle_spline_];
  if (handle_point_ < 0 || handle_point_ >= spline.points.size()) {
    return false;
  }
  ShapePoint &point = spline.points[handle_point_];
  const float2 target = p + handle_grab_delta_;
  float2 &primary = (handle_side_ == ShapeHandleSide::Left) ? point.handle_left :
                                                              point.handle_right;
  if (math::length_squared(target - primary) < 1e-12f) {
    return false;
  }
  const float2 old_primary = primary;
  primary = target;
  point.auto_handles = false;

  /* The edited handle only curves its own segment: clear the incoming corner of this point for the
   * left side, the next point's incoming corner for the right side. */
  const int count = int(spline.points.size());
  if (handle_side_ == ShapeHandleSide::Left) {
    point.corner = false;
  }
  else if (spline.cyclic || handle_point_ + 1 < count) {
    spline.points[(handle_point_ + 1) % count].corner = false;
  }

  float2 &other = (handle_side_ == ShapeHandleSide::Left) ? point.handle_right :
                                                            point.handle_left;
  const float2 dir = primary - point.co;
  if (align && handle_other_length_ > 1e-6f && math::length_squared(dir) > 1e-12f) {
    other = point.co - math::normalize(dir) * handle_other_length_;
  }
  else {
    /* Free (non-aligned) or a collapsed opposite handle: keep its resolved grab-time pose
     * instead of leaving stale (possibly origin) coordinates. */
    other = handle_other_start_;
  }

  if (primary == old_primary) {
    return false;
  }
  gesture_dirty_ = true;
  return true;
}

void ShapeEditSession::handle_drag_cancel()
{
  handle_dragging_ = false;
  gesture_dirty_ = false;
  undo_pop_no_redo();
}

void ShapeEditSession::handle_drag_end()
{
  handle_dragging_ = false;
  if (gesture_dirty_) {
    undo_push();
  }
  gesture_dirty_ = false;
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

void ShapeEditSession::gesture_abort()
{
  dragging_ = false;
  transforming_ = false;
  point_dragging_ = false;
  origin_dragging_ = false;
  handle_dragging_ = false;
  gesture_dirty_ = false;
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

/* -------------------------------------------------------------------- */
/** \name Dashed outline
 * \{ */

void shape_draw_dashed_outline(Span<float2> region_points, const bool loop)
{
  if (region_points.size() < 2) {
    return;
  }
  /* A non-finite point (a degenerate mapping) would draw a spurious segment from the region
   * corner; drop the whole outline instead. */
  for (const float2 &co : region_points) {
    if (!std::isfinite(co.x) || !std::isfinite(co.y)) {
      return;
    }
  }
  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
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
  immBegin(loop ? GPU_PRIM_LINE_LOOP : GPU_PRIM_LINE_STRIP, int(region_points.size()));
  for (const float2 &co : region_points) {
    immVertex2f(pos, co.x, co.y);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Plain outline
 * \{ */

void shape_draw_plain_outline(Span<float2> region_points, const bool loop)
{
  if (region_points.size() < 2) {
    return;
  }
  for (const float2 &co : region_points) {
    if (!std::isfinite(co.x) || !std::isfinite(co.y)) {
      return;
    }
  }
  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4f(0.5f, 0.5f, 0.5f, 0.35f);
  GPU_line_width(1.0f);
  immBegin(loop ? GPU_PRIM_LINE_LOOP : GPU_PRIM_LINE_STRIP, int(region_points.size()));
  for (const float2 &co : region_points) {
    immVertex2f(pos, co.x, co.y);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Control points
 * \{ */

/** One point square as a triangle fan around \a center. */
static void point_square_draw(const uint pos, const float2 &center, const float half)
{
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, center.x - half, center.y - half);
  immVertex2f(pos, center.x + half, center.y - half);
  immVertex2f(pos, center.x + half, center.y + half);
  immVertex2f(pos, center.x - half, center.y + half);
  immEnd();
}

void shape_draw_control_points(Span<float2> region_cos)
{
  if (region_cos.is_empty()) {
    return;
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* The dark border then the light core, all points per batch (the Vector session's per-point
   * pairs; squares of one point never overlap another's, so the batching is visual-identical). */
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
  for (const float2 &co : region_cos) {
    point_square_draw(pos, co, 4.5f);
  }
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.95f);
  for (const float2 &co : region_cos) {
    point_square_draw(pos, co, 3.0f);
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Origin markers
 * \{ */

/** One upright diamond as a triangle fan around \a center. */
static void origin_diamond_draw(const uint pos, const float2 &center, const float radius)
{
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, center.x, center.y - radius);
  immVertex2f(pos, center.x + radius, center.y);
  immVertex2f(pos, center.x, center.y + radius);
  immVertex2f(pos, center.x - radius, center.y);
  immEnd();
}

void shape_draw_origin_markers(Span<float2> region_cos)
{
  if (region_cos.is_empty()) {
    return;
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Dark diamond outline, then the orange core: distinct from the white control-point squares
   * so the pivot is readable at a glance. */
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.85f);
  for (const float2 &co : region_cos) {
    origin_diamond_draw(pos, co, 6.0f);
  }
  immUniformColor4f(1.0f, 0.55f, 0.1f, 1.0f);
  for (const float2 &co : region_cos) {
    origin_diamond_draw(pos, co, 4.0f);
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session overlay
 * \{ */

/** Flattening error (shape pixels) of the live session outline. */
constexpr float SHAPE_OVERLAY_ERROR_PX = 2.0f;

void shape_draw_session_overlay(
    Span<VectorItem> items,
    const int active_index,
    FunctionRef<float2(const PaintShape &, const float2 &)> to_region)
{
  for (const VectorItem &item : items) {
    for (const ShapePolyline &poly : shape_flatten(item.shape, SHAPE_OVERLAY_ERROR_PX)) {
      Vector<float2> region_points(poly.points.size());
      for (const int i : poly.points.index_range()) {
        region_points[i] = to_region(item.shape, poly.points[i]);
      }
      shape_draw_dashed_outline(region_points, poly.cyclic);
    }
  }

  if (active_index < 0 || active_index >= items.size()) {
    return;
  }
  const PaintShape &active = items[active_index].shape;

  Vector<float2> region_cos;
  for (const ShapeSpline &spline : active.splines) {
    for (const ShapePoint &point : spline.points) {
      region_cos.append(to_region(active, point.co));
    }
  }
  shape_draw_control_points(region_cos);

  float2 origin_region = to_region(active, shape_effective_origin(active));
  shape_draw_origin_markers(Span<float2>(&origin_region, 1));
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
