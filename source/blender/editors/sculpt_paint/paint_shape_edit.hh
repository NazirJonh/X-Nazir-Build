/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Space-agnostic editable shape document: the list of shapes, the active shape, a session undo
 * stack and the move-drag gesture. Frontends (the Image Editor Vector session today, the 3D
 * Sculpt PBR session later) only supply the 2D space the shapes live in -- region pixels of
 * their view -- and decide what to do with the edits (live ImBuf preview vs. attribute
 * preview). Coordinates are plain 2D pixels; the session never touches SpaceImage, ImBuf or
 * the mesh.
 *
 * Handle editing (cage, corners, points, widths) builds on #ShapeHandle; the gestures
 * below (move drag, whole-shape rotate/scale, single-point drag) are the first ones on top of
 * it.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "paint_shape.hh"
#include "paint_vector_document.hh"

namespace blender::ed::sculpt_paint::shape {

/** Base grab tolerance of contour hit-tests, in session pixels. Frontends scale it by
 * #UI_SCALE_FAC before passing it down, so the tolerance follows the interface scale while the
 * session itself stays UI-agnostic. */
constexpr float SHAPE_HIT_TOLERANCE_PX = 4.0f;

/** Control points are small targets, so they are grabbed from a wider ring than the contour
 * (which is matched against the drawn stroke); the larger radius keeps a near-miss from falling
 * through to a whole-shape move or a session confirm. */
constexpr float SHAPE_POINT_HIT_SCALE = 3.0f;

/** The Ctrl+RMB cut is forgiving too: it snaps to the nearest segment within this multiple of the
 * base tolerance, and the insert preview shows exactly what that snap picks. */
constexpr float SHAPE_INSERT_TOLERANCE_SCALE = 8.0f;

/** Editable handle of a shape. */
enum class ShapeHandleType : int8_t {
  None = 0,
  /** Whole-shape move (grabbed on the contour). */
  Move = 1,
  /** A control point of the active shape. */
  Point = 2,
  /** The active shape's origin (the Move/Rotate/Scale pivot marker). */
  Origin = 3,
};

struct ShapeHandle {
  ShapeHandleType type = ShapeHandleType::None;
  /** Index into the session shapes; -1 when no handle is active. */
  int shape_index = -1;
  /** Spline of a #ShapeHandleType::Point handle; -1 otherwise. */
  int spline_index = -1;
  /** Control point of a #ShapeHandleType::Point handle; -1 otherwise. */
  int point_index = -1;
};

/** Whole-shape transform of the active shape (the R/S gestures and the cage gizmo). */
enum class ShapeTransformMode : int8_t {
  Rotate = 0,
  /** Uniform scale (the S gesture and the cage's proportional drag). */
  Scale = 1,
  /** Independent X/Y scale along the shape's local axes (the cage's Shift drag and edge
   * handles). */
  ScaleFree = 2,
};

/** Which local axis a #ShapeTransformMode::ScaleFree drag may change; the other stays at 1. */
enum class ShapeScaleAxis : int8_t {
  Both = 0,
  X = 1,
  Y = 2,
};

/** Axis lock of the move/scale gestures (the X/Y keys). */
enum class ShapeAxisLock : int8_t {
  None = 0,
  X = 1,
  Y = 2,
};

/**
 * Editable list of shapes with an active shape, a session-local undo stack and the move,
 * transform and point-drag gestures. Not thread-safe: the frontend drives it from the main
 * thread only.
 *
 * Every mutating gesture follows the same protocol: `*_begin` pushes the pre-gesture pose for
 * #undo_pop and clears the redo stack, `*_update` rewrites the pose from the gesture start (no
 * error accumulation), `*_end` keeps the pose, `*_cancel` restores the pre-gesture pose without
 * touching the redo stack. #undo_pop/#redo_pop require no gesture to be active.
 */
class ShapeEditSession {
 public:
  ShapeEditSession() = default;
  /** \a tile is the reference tile every shape's pixels refer to (the default tile for hosts
   * that do not live on a UDIM canvas, e.g. the 3D viewport). */
  explicit ShapeEditSession(Vector<PaintShape> shapes, const CanvasTile &tile = {});

  bool is_empty() const
  {
    return document_.items.is_empty();
  }
  int shapes_num() const
  {
    return document_.items.size();
  }
  /** Copy of the item shapes, for the bake / symmetry paths (items are not contiguous). */
  Vector<PaintShape> shapes() const;
  /** The reference tile of the first item's 2D space: the tile all session shapes share. The
   * default tile when the session is empty. */
  CanvasTile canvas_tile() const;
  /** The document's items, for iteration (overlay, host). */
  const Vector<VectorItem> &items() const
  {
    return document_.items;
  }
  /** Append a shape as a new item (session setup). */
  void append_shape(PaintShape shape, const CanvasTile &tile = {});
  /** Replace the edited shapes (session setup); clears the undo stack and the drag. */
  void shapes_set(Vector<PaintShape> shapes, const CanvasTile &tile = {});
  /** Active shape for single-shape gestures; null when empty. */
  const PaintShape *active() const;
  PaintShape *active_for_write();
  /** The active item (shape + space), for origin access. */
  VectorItem *active_item();
  const VectorItem *active_item() const;
  int active_index() const
  {
    return document_.active;
  }
  void active_set(int index);
  /** Activate the next shape, wrapping around (the Tab gesture). */
  void active_next();
  /** The document (undo history included). */
  VectorDocument &document()
  {
    return document_;
  }
  const VectorDocument &document() const
  {
    return document_;
  }

  /** Contour hit-test of \a p (session pixels) against every shape, topmost last; -1 when over
   * none. A hit is fill or stroke coverage within \a tolerance_px of the outline, evaluated
   * through #ShapeEvaluator instead of the bounds, so empty bbox corners never grab. */
  int hit_shape(const float2 &p, const ShapeStyle &style, float tolerance_px) const;
  /** True when \a p is over any shape. */
  bool hit_test(const float2 &p, const ShapeStyle &style, float tolerance_px) const;
  /** Handle under \a p within \a radius_px. Priority : a control point of the
   * active shape, then its origin, then the whole-shape move handle (which may pick another
   * shape). */
  ShapeHandle handle_at(const float2 &p, float radius_px, const ShapeStyle &style) const;

  /** Move drag of the shape under \a p (it becomes active); false when over no shape. */
  bool drag_begin(const float2 &p, const ShapeStyle &style, float tolerance_px);
  /** Move drag of the active shape without a hit-test (the G gesture); \a p is the cursor
   * the drag measures from. */
  bool move_active_begin(const float2 &p);
  /** Translate the dragged shape so the cursor reaches \a p; false when nothing changed. */
  bool drag_update(const float2 &p, ShapeAxisLock lock = ShapeAxisLock::None);
  /** Restore the drag-start pose, leaving the session alive. */
  void drag_cancel();
  /** Finish the drag, keeping the moved pose. */
  void drag_end();
  bool is_dragging() const
  {
    return dragging_;
  }

  /** Rotate/scale drag of the active shape around its origin; false without an active shape.
   * \a scale_axis restricts a #ShapeTransformMode::ScaleFree drag to one local axis. */
  bool transform_begin(ShapeTransformMode mode,
                       const float2 &p,
                       const ShapeStyle &style,
                       ShapeScaleAxis scale_axis = ShapeScaleAxis::Both);
  /** Recompute the active shape from the gesture start; false when nothing changed. */
  bool transform_update(const float2 &p, ShapeAxisLock lock = ShapeAxisLock::None);
  /** Finish the transform, keeping the pose. */
  void transform_end();
  /** Restore the transform-start pose, leaving the session alive. */
  void transform_cancel();
  bool is_transforming() const
  {
    return transforming_;
  }
  ShapeTransformMode transform_mode() const
  {
    return transform_mode_;
  }

  /** Drag the active shape's spline point nearest to \a p (the E gesture); false when \a p is
   * outside \a tolerance_px of every point or the active shape has no splines. */
  bool point_drag_begin(const float2 &p, float tolerance_px);
  /** Move the grabbed point (with its handles) so the cursor reaches \a p. */
  bool point_drag_update(const float2 &p);
  /** Restore the point-drag-start pose, leaving the session alive. */
  void point_drag_cancel();
  /** Finish the point drag, keeping the pose. */
  void point_drag_end();
  bool is_point_dragging() const
  {
    return point_dragging_;
  }

  /** Ctrl+RMB cut: insert a control point into the active spline shape's segment nearest to \a p
   * (within \a tolerance_px), splitting a Bézier segment without changing its curve. Records an
   * undo step; false when \a p is not over a segment or the shape has no splines. */
  bool point_insert(const float2 &p, float tolerance_px);
  /** Where #point_insert would cut for \a p: the point on the segment and its (unnormalized)
   * tangent, both in session pixels. False when nothing would be inserted. */
  bool insert_preview(const float2 &p, float tolerance_px, float2 &r_point, float2 &r_tangent) const;

  /** Drag the active shape's origin (the R/S pivot marker); false without an active
   * shape. The origin follows the cursor and #PaintShape::origin_is_custom is set. */
  bool origin_drag_begin(const float2 &p);
  /** Move the grabbed origin so the cursor reaches \a p; false when nothing changed. */
  bool origin_drag_update(const float2 &p);
  /** Restore the origin-drag-start pose, leaving the session alive. */
  void origin_drag_cancel();
  /** Finish the origin drag, keeping the pose. */
  void origin_drag_end();
  bool is_origin_dragging() const
  {
    return origin_dragging_;
  }
  /** Drop a custom origin so it returns to the computed mean. False (and no undo step)
   * when the origin was not custom. */
  bool origin_reset();

  /** True when any gesture (move, transform, point drag or origin drag) is in flight. */
  bool gesture_active() const
  {
    return dragging_ || transforming_ || point_dragging_ || origin_dragging_;
  }

  /** Push the current shapes for #undo_pop (call before each mutating gesture); clears redo. */
  void undo_push();
  /** Restore the last pushed shapes, moving the current pose to the redo stack; false when the
   * stack is empty. */
  bool undo_pop();
  /** Restore the last undone pose, moving the current pose back to the undo stack; false when
   * the redo stack is empty. */
  bool redo_pop();
  bool undo_can() const
  {
    return document_.undo_step_current > 0;
  }
  bool redo_can() const
  {
    return document_.undo_step_current + 1 < int(document_.undo_steps.size());
  }
  void undo_clear();

 private:
  /** The segment of the active spline shape nearest to \a p within \a tolerance_px, and the
   * Bézier parameter of the closest point on it. */
  bool insert_spot_find(
      const float2 &p, float tolerance_px, int &r_spline, int &r_segment, float &r_t) const;

  /** Restore the last pushed shapes without recording a redo entry (gesture cancels). */
  void undo_pop_no_redo();

  VectorDocument document_;
  /** Set once a `*_update` actually changed the shape; `*_end` then records an undo entry. */
  bool gesture_dirty_ = false;

  bool dragging_ = false;
  float2 drag_last_ = float2(0.0f);

  bool transforming_ = false;
  ShapeTransformMode transform_mode_ = ShapeTransformMode::Rotate;
  /** Axis restriction of a #ShapeTransformMode::ScaleFree drag. */
  ShapeScaleAxis transform_scale_axis_ = ShapeScaleAxis::Both;
  /** The active shape's origin at gesture start (the pivot). */
  float2 transform_pivot_ = float2(0.0f);
  float2 transform_start_cursor_ = float2(0.0f);
  /** Active shape at gesture start; updates rewrite the live shape from this copy. */
  PaintShape transform_start_shape_;

  bool point_dragging_ = false;
  int point_spline_ = -1;
  int point_index_ = -1;
  /** Cursor-to-point offset at grab time, so the point does not jump to the cursor. */
  float2 point_grab_delta_ = float2(0.0f);

  bool origin_dragging_ = false;
  /** Cursor-to-origin offset at grab time, so the marker does not jump to the cursor. */
  float2 origin_grab_delta_ = float2(0.0f);
};

/**
 * The settings <-> active-shape glue shared by the 2D and 3D Vector hosts, so their "previous
 * value" scheme in `prepare_input` is identical.
 *
 * The header/popover direction (settings -> active shape): each is a no-op on a shape that does
 * not carry the parameter (splines, or a non-Rect corner radius).
 * \{ */
void shape_apply_size_to_active(ShapeEditSession &edit, const float2 &size);
void shape_apply_corner_radius_to_active(ShapeEditSession &edit, const float4 &radius);
void shape_apply_rotation_to_active(ShapeEditSession &edit, float raw_dangle);
/**
 * The "previous value" scheme of both hosts' `prepare_input`: Size, Angle and Corner Radius live on
 * the shape, not the style, so a settings edit is pushed onto the active shape only when the value
 * differs from what the last refresh used (\a previous), and a shape edit (drag / gizmo) is not
 * overwritten by an unchanged setting.
 */
void shape_apply_style_edits_to_active(ShapeEditSession &edit,
                                       const ShapeStyle &previous,
                                       const ShapeStyle &current);
/** Mirror the active shape's size and (wrapped) rotation back into \a settings and \a style (the
 * reverse direction, after a gesture / active-shape change). Parametric shapes report their size
 * and angle; a spline-backed shape has no stored angle, so it resets to 0. */
void shape_sync_settings_from_active(ShapeEditSession &edit,
                                     PaintShapeSettings &settings,
                                     ShapeStyle &style);
/** \} */

}  // namespace blender::ed::sculpt_paint::shape
