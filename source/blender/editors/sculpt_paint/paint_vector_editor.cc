/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared Vector editing core; see #paint_vector_editor.hh.
 */

#include "paint_vector_editor.hh"

#include <algorithm>

#include "BLI_utildefines.h"

#include "WM_types.hh"

#include "paint_intern.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Gesture mutation helpers
 * \{ */

void ShapeVectorEditor::move_update(bContext *C,
                                    VectorEditHost &host,
                                    const float2 &px,
                                    const ShapeAxisLock lock)
{
  if (host.edit().drag_update(px, lock)) {
    host.restamp(C);
  }
}

void ShapeVectorEditor::transform_update(bContext *C,
                                         VectorEditHost &host,
                                         const float2 &px,
                                         const ShapeAxisLock lock)
{
  if (host.edit().transform_update(px, lock)) {
    host.restamp(C);
  }
}

void ShapeVectorEditor::point_update(bContext *C, VectorEditHost &host, const float2 &px)
{
  if (host.edit().point_drag_update(px)) {
    host.restamp(C);
  }
}

void ShapeVectorEditor::origin_update(bContext *C, VectorEditHost &host, const float2 &px)
{
  if (host.edit().origin_drag_update(px)) {
    /* Only the overlay changed (the shape geometry is untouched): redraw, do not re-stamp. */
    host.redraw(C);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gesture lifetime
 * \{ */

void ShapeVectorEditor::end_gesture(bContext *C, VectorEditHost &host)
{
  switch (gesture_) {
    case Gesture::Rotate:
    case Gesture::Scale:
      host.edit().transform_end();
      break;
    case Gesture::PointDrag:
      host.edit().point_drag_end();
      break;
    case Gesture::OriginDrag:
      host.edit().origin_drag_end();
      break;
    case Gesture::StrokeWidth:
    case Gesture::StrokeOpacity:
      break;
    case Gesture::MovePick:
    case Gesture::MoveActive:
    case Gesture::None:
      host.edit().drag_end();
      break;
  }
  gesture_ = Gesture::None;
  axis_lock_ = ShapeAxisLock::None;
  host.gesture_ended(C);
  /* A finished gesture may have moved / resized / rotated the active shape: refresh the settings
   * so the header and popovers show it. */
  host.sync_settings_from_active();
}

bool ShapeVectorEditor::begin_gesture_at(VectorEditHost &host, const float2 &px)
{
  const ShapeHandle handle = host.edit().handle_at(px, host.hit_tolerance_px(), host.style());
  switch (handle.type) {
    case ShapeHandleType::Point:
      /* The click picked a control point of the active shape : drag it. */
      if (host.edit().point_drag_begin(px, host.hit_tolerance_px())) {
        gesture_ = Gesture::PointDrag;
        axis_lock_ = ShapeAxisLock::None;
      }
      return true;
    case ShapeHandleType::Move:
      if (host.edit().drag_begin(px, host.style(), host.hit_tolerance_px())) {
        gesture_ = Gesture::MovePick;
        axis_lock_ = ShapeAxisLock::None;
      }
      return true;
    case ShapeHandleType::Origin:
      /* Drag the pivot marker; only its position and the custom flag change. */
      if (host.edit().origin_drag_begin(px)) {
        gesture_ = Gesture::OriginDrag;
        axis_lock_ = ShapeAxisLock::None;
      }
      return true;
    case ShapeHandleType::None:
    default:
      return false;
  }
}

bool ShapeVectorEditor::start_move_pick(bContext *C, const float2 &px, VectorEditHost &host)
{
  (void)C;
  cursor_px_ = px;
  return this->begin_gesture_at(host, px);
}

void ShapeVectorEditor::begin_value_gesture(VectorEditHost &host,
                                            const bool is_width,
                                            const int2 &mval)
{
  value_start_ = is_width ? host.settings().stroke_width : host.overall_opacity();
  gesture_ = is_width ? Gesture::StrokeWidth : Gesture::StrokeOpacity;
  axis_lock_ = ShapeAxisLock::None;
  start_mval_ = mval;
}

void ShapeVectorEditor::cancel_gesture(bContext *C, VectorEditHost &host)
{
  switch (gesture_) {
    case Gesture::Rotate:
    case Gesture::Scale:
      if (host.edit().is_transforming()) {
        host.edit().transform_cancel();
        host.restamp(C);
      }
      break;
    case Gesture::PointDrag:
      if (host.edit().is_point_dragging()) {
        host.edit().point_drag_cancel();
        host.restamp(C);
      }
      break;
    case Gesture::OriginDrag:
      if (host.edit().is_origin_dragging()) {
        host.edit().origin_drag_cancel();
        host.redraw(C);
      }
      break;
    case Gesture::StrokeWidth:
    case Gesture::StrokeOpacity:
      /* Restore the settings value the gesture started from. */
      host.settings_apply(C, gesture_ == Gesture::StrokeWidth, value_start_);
      break;
    case Gesture::MovePick:
    case Gesture::MoveActive:
    case Gesture::None:
      if (host.edit().is_dragging()) {
        host.edit().drag_cancel();
        host.restamp(C);
      }
      break;
  }
  gesture_ = Gesture::None;
  axis_lock_ = ShapeAxisLock::None;
  host.sync_settings_from_active();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session history
 * \{ */

bool ShapeVectorEditor::undo(bContext *C, VectorEditHost &host)
{
  this->cancel_gesture(C, host);
  if (!host.edit().undo_pop()) {
    return false;
  }
  host.restamp(C);
  host.sync_settings_from_active();
  return true;
}

bool ShapeVectorEditor::redo(bContext *C, VectorEditHost &host)
{
  this->cancel_gesture(C, host);
  if (!host.edit().redo_pop()) {
    return false;
  }
  host.restamp(C);
  host.sync_settings_from_active();
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event dispatch
 * \{ */

ShapeVectorEditor::Status ShapeVectorEditor::handle_event(bContext *C,
                                                          const wmEvent &event,
                                                          const float2 &event_p,
                                                          const bool in_owner,
                                                          VectorEditHost &host)
{
  if (in_owner && ELEM(event.type, MOUSEMOVE, INBETWEEN_MOUSEMOVE, LEFTMOUSE, RIGHTMOUSE)) {
    cursor_px_ = event_p;
  }

  if (event.type == EVT_MODAL_MAP) {
    switch (event.val) {
      case PAINT_SHAPE_MODAL_CONFIRM: {
        if (!this->gesture_is_idle(host)) {
          this->end_gesture(C, host);
          return Status::Handled;
        }
        this->end_gesture(C, host);
        /* The operator commits (through its apply operator). */
        return Status::Finished;
      }
      case PAINT_SHAPE_MODAL_CANCEL: {
        if (!this->gesture_is_idle(host)) {
          /* Esc during a gesture undoes just the gesture; the session stays alive. */
          this->cancel_gesture(C, host);
          return Status::Handled;
        }
        return Status::Cancelled;
      }
      case PAINT_SHAPE_MODAL_MOVE:
      case PAINT_SHAPE_MODAL_ROTATE:
      case PAINT_SHAPE_MODAL_SCALE:
      case PAINT_SHAPE_MODAL_STROKE_WIDTH:
      case PAINT_SHAPE_MODAL_STROKE_OPACITY:
      case PAINT_SHAPE_MODAL_EXTRUDE: {
        /* One gesture at a time; keys pressed mid-gesture are ignored. */
        if (!this->gesture_is_idle(host)) {
          return Status::Handled;
        }
        Gesture gesture = Gesture::None;
        bool begun = false;
        switch (event.val) {
          case PAINT_SHAPE_MODAL_MOVE:
            begun = host.edit().move_active_begin(cursor_px_);
            gesture = Gesture::MoveActive;
            break;
          case PAINT_SHAPE_MODAL_ROTATE:
            begun = host.edit().transform_begin(
                ShapeTransformMode::Rotate, cursor_px_, host.style());
            gesture = Gesture::Rotate;
            break;
          case PAINT_SHAPE_MODAL_SCALE:
            begun = host.edit().transform_begin(
                ShapeTransformMode::Scale, cursor_px_, host.style());
            gesture = Gesture::Scale;
            break;
          case PAINT_SHAPE_MODAL_STROKE_WIDTH:
            value_start_ = host.settings().stroke_width;
            begun = true;
            gesture = Gesture::StrokeWidth;
            break;
          case PAINT_SHAPE_MODAL_STROKE_OPACITY:
            value_start_ = host.overall_opacity();
            begun = true;
            gesture = Gesture::StrokeOpacity;
            break;
          case PAINT_SHAPE_MODAL_EXTRUDE:
            begun = host.edit().point_drag_begin(cursor_px_, host.hit_tolerance_px());
            gesture = Gesture::PointDrag;
            break;
          default:
            break;
        }
        if (!begun) {
          return Status::Handled;
        }
        gesture_ = gesture;
        axis_lock_ = ShapeAxisLock::None;
        start_mval_ = int2(event.mval[0], event.mval[1]);
        return Status::Handled;
      }
      case PAINT_SHAPE_MODAL_SELECT_NEXT: {
        if (!this->gesture_is_idle(host)) {
          return Status::Handled;
        }
        host.edit().active_next();
        host.sync_settings_from_active();
        return Status::Handled;
      }
      case PAINT_SHAPE_MODAL_ORIGIN_RESET: {
        if (!this->gesture_is_idle(host)) {
          return Status::Handled;
        }
        /* The pivot returns to the computed mean; only the overlay changes, so redraw only. */
        if (host.edit().origin_reset()) {
          host.redraw(C);
        }
        return Status::Handled;
      }
      case PAINT_SHAPE_MODAL_AXIS_X:
      case PAINT_SHAPE_MODAL_AXIS_Y: {
        const ShapeAxisLock toggled = (event.val == PAINT_SHAPE_MODAL_AXIS_X) ? ShapeAxisLock::X :
                                                                               ShapeAxisLock::Y;
        if (!ELEM(gesture_, Gesture::MovePick, Gesture::MoveActive, Gesture::Scale)) {
          return Status::Handled;
        }
        axis_lock_ = (axis_lock_ == toggled) ? ShapeAxisLock::None : toggled;
        if (gesture_ == Gesture::Scale) {
          this->transform_update(C, host, cursor_px_, axis_lock_);
        }
        else {
          this->move_update(C, host, cursor_px_, axis_lock_);
        }
        return Status::Handled;
      }
      default:
        /* A modal-map event no branch owns (user remapping): consume. */
        return Status::Handled;
    }
  }

  if (!in_owner) {
    /* A release anywhere ends the in-flight gesture (only presses are region-gated). */
    if (event.type == LEFTMOUSE && event.val == KM_RELEASE && !this->gesture_is_idle(host)) {
      this->end_gesture(C, host);
      return Status::Handled;
    }
    /* Value gestures (F / Shift+F) track plain screen deltas, so their moves count outside the
     * owner region too. */
    const bool value_move = ELEM(gesture_, Gesture::StrokeWidth, Gesture::StrokeOpacity) &&
                            ELEM(event.type, MOUSEMOVE, INBETWEEN_MOUSEMOVE);
    if (!value_move) {
      return Status::Unhandled;
    }
  }

  switch (event.type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      switch (gesture_) {
        case Gesture::Rotate:
        case Gesture::Scale:
          this->transform_update(C, host, cursor_px_, axis_lock_);
          break;
        case Gesture::PointDrag:
          this->point_update(C, host, cursor_px_);
          break;
        case Gesture::OriginDrag:
          this->origin_update(C, host, cursor_px_);
          break;
        case Gesture::StrokeWidth:
        case Gesture::StrokeOpacity: {
          const float dx = float(event.mval[0] - start_mval_.x);
          const float value = (gesture_ == Gesture::StrokeWidth) ?
                                  std::max(0.0f, value_start_ + dx) :
                                  std::clamp(value_start_ + dx * 0.005f, 0.0f, 1.0f);
          host.settings_apply(C, gesture_ == Gesture::StrokeWidth, value);
          break;
        }
        case Gesture::MovePick:
        case Gesture::MoveActive:
          this->move_update(C, host, cursor_px_, axis_lock_);
          break;
        case Gesture::None:
          /* A plain move with no gesture: nothing to update (and no preview re-stamp). */
          break;
      }
      return Status::Handled;
    }

    case LEFTMOUSE: {
      if (event.val == KM_PRESS) {
        if (!this->gesture_is_idle(host)) {
          /* Press confirms the in-flight gesture, like Enter. */
          this->end_gesture(C, host);
          return Status::Handled;
        }
        if (this->begin_gesture_at(host, cursor_px_)) {
          host.sync_settings_from_active();
          return Status::Handled;
        }
        /* A press over no handle confirms the session; the operator commits. */
        this->end_gesture(C, host);
        return Status::Finished;
      }
      if (event.val == KM_RELEASE) {
        this->end_gesture(C, host);
        return Status::Handled;
      }
      return Status::Handled;
    }

    case RIGHTMOUSE: {
      /* Ctrl+RMB cuts the contour: inserts a point into the segment under the cursor, like the
       * Curve stroke method's Insert Point. */
      if (event.val != KM_PRESS || (event.modifier & KM_CTRL) == 0) {
        return Status::Unhandled;
      }
      if (this->gesture_is_idle(host) &&
          host.edit().point_insert(cursor_px_,
                                   host.hit_tolerance_px() * SHAPE_INSERT_TOLERANCE_SCALE))
      {
        host.restamp(C);
        host.sync_settings_from_active();
      }
      return Status::Handled;
    }

    default:
      return Status::Unhandled;
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
