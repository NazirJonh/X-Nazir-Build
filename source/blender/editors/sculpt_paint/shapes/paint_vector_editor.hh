/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The modal editing core of a live Vector shape session: the gesture state machine (move drag,
 * rotate / scale, point drag, the F / Shift+F value drags, Tab / X / Y) shared by every frontend.
 * Not an operator: each target registers its own modal operator and forwards `vector_edit` events
 * to #ShapeVectorEditor::handle_event.
 *
 * The core mutates #VectorEditHost::edit() directly -- the session already knows every gesture --
 * and asks the host to re-stamp the preview after a change. Everything else the core cannot know
 * it asks #VectorEditHost: the style, the settings, the coordinate space. Operator status, the
 * session undo / redo pass-through and the apply-operator indirection deliberately stay in the
 * operator; the core returns a coarse #ShapeVectorEditor::Status
 * the operator maps.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "paint_shape.hh"
#include "paint_shape_edit.hh"
#include "paint_vector_document.hh"

namespace blender {

struct ReportList;
struct bContext;
struct wmEvent;
struct wmOperatorType;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

/**
 * What the shared Vector editor needs from its target. Mirrors #CurvePatchHost: the target owns
 * session lifetime and the write backend, the core owns the gestures and edits #edit() in place.
 * A new target (3D Sculpt, Curve Patch) only implements this surface.
 *
 * The `bContext *` arguments are nullable ONLY so unit-test hosts can run without a context (the
 * type is opaque outside `context.cc`); a production host that reads the context must assert it is
 * non-null.
 */
class VectorEditHost {
 public:
  virtual ~VectorEditHost() = default;

  virtual VectorDocument &document() = 0;
  /** The editable session the core mutates. */
  virtual ShapeEditSession &edit() = 0;
  /** Style the gestures build and hit-test with. (The name `space()` is reserved for the
   * runtime ShapeSpace / ShapeSpaceDesc of the session.) */
  virtual const ShapeStyle &style() const = 0;
  virtual PaintShapeSettings &settings() = 0;
  /** Hit tolerance in session pixels, already UI-scaled by the frontend. */
  virtual float hit_tolerance_px() const = 0;
  /** Reference tile (and its pixel size) the session shapes live in, so a frontend can map an
   * event to session pixels. A 2D host reads it from the space description of its first item. */
  virtual int ref_tile() const = 0;
  virtual int2 ref_tile_size() const = 0;
  virtual ReportList *reports()
  {
    return nullptr;
  }

  /** Composite the live preview of the current #edit() state (no undo). The core calls this
   * after every session call it made that reported a change. \a C may be null in tests. */
  virtual void restamp(bContext *C) = 0;
  /** Mirror the active shape's editable parameters (size / angle) back into the settings after a
   * gesture or an active-shape change, for the header / popover feedback. Default no-op; hosts
   * with no shared settings leave it so. Must not trigger a preview refresh. */
  virtual void sync_settings_from_active() {}
  /** Settle whatever #restamp left mid-interaction. No-op for the flat canvas; the 3D targets
   * arm their fast redraw path here (cf. #CurvePatchHost::interaction_end). */
  virtual void interaction_end(bContext * /*C*/) {}
  /** A move / transform / point / origin gesture just ended (kept or cancelled): a host that
   * deferred the live preview while it ran composites the final pose here. Default no-op. */
  virtual void gesture_ended(bContext * /*C*/) {}
  /** Composite a live preview a host left stale while a gesture ran, when no gesture is in flight
   * any more. For gestures that end outside an event handler (a gizmo handle drag settles from the
   * gizmo refresh callback, i.e. during a redraw, where compositing is unsafe): the owner calls it
   * from its next event. Cheap when nothing is pending. Default no-op. */
  virtual void flush_deferred_preview(bContext * /*C*/) {}
  /** Write the F / Shift+F value into the settings and refresh the previews; true when changed. */
  virtual bool settings_apply(bContext *C, bool is_width, float value) = 0;
  /** Current overall opacity the Shift+F drag should start from. Defaults to the settings' stroke
   * opacity; targets whose opacity comes from elsewhere (the brush strength) override it. */
  virtual float overall_opacity()
  {
    return this->settings().stroke_opacity;
  }
  /** Redraw what shows the session, without re-stamping. */
  virtual void redraw(bContext *C) = 0;

  /* Session lifetime (commit / cancel / is_alive) is deliberately NOT part of this interface: the
   * operator fixes or discards the session through its apply operator / cancel function, which
   * own the redo anchor and the unique undo step. The host cannot know those semantics. */
};

class ShapeVectorEditor {
 public:
  enum class Gesture : int8_t {
    None,
    MovePick,
    MoveActive,
    Rotate,
    Scale,
    StrokeWidth,
    StrokeOpacity,
    PointDrag,
    OriginDrag,
  };

  enum class Status {
    /** Handled; the modal keeps running. */
    Handled,
    /** Not ours -- the event must reach the editor underneath (operator returns PASS_THROUGH). */
    Unhandled,
    /** The session was confirmed; the operator tears down and runs its apply operator. */
    Finished,
    /** The session was cancelled; the operator tears down and cancels. */
    Cancelled,
  };

  /**
   * Feed one `vector_edit` event. \a event_p is the event in the session's 2D space (shape pixels)
   * and \a in_owner whether it belongs to the region the modal owns. \a C may be null in tests.
   */
  Status handle_event(bContext *C,
                      const wmEvent &event,
                      const float2 &event_p,
                      bool in_owner,
                      VectorEditHost &host);

  /** Begin the handle gesture under an invoking press (the Vector re-invoke path): a control
   * point starts a point drag, the contour a whole-shape move, the origin only selects. False
   * when the press is over no handle (the operator then confirms the session). Mirrors a
   * #LEFTMOUSE press. */
  bool start_move_pick(bContext *C, const float2 &px, VectorEditHost &host);

  /** Begin the F / Shift+F value gesture (stroke width / overall opacity) when the key that starts
   * it arrived through a keymap instead of the running modal (the modal map translates the key
   * only while the modal runs). \a mval is the cursor in region pixels. */
  void begin_value_gesture(VectorEditHost &host, bool is_width, const int2 &mval);

  /** Cancel an in-flight gesture (Esc during a gesture, an undo step, a press-confirm). */
  void cancel_gesture(bContext *C, VectorEditHost &host);
  /** Finish an in-flight gesture, keeping the new pose. */
  void end_gesture(bContext *C, VectorEditHost &host);

  /** Step the session-local undo / redo after cancelling any in-flight gesture. False when there
   * is nothing to step, so the operator passes the event through to the global undo. */
  bool undo(bContext *C, VectorEditHost &host);
  bool redo(bContext *C, VectorEditHost &host);

  Gesture gesture() const
  {
    return gesture_;
  }
  ShapeAxisLock axis_lock() const
  {
    return axis_lock_;
  }
  bool gesture_is_idle(VectorEditHost &host) const
  {
    return gesture_ == Gesture::None && !host.edit().gesture_active();
  }
  float2 cursor_px() const
  {
    return cursor_px_;
  }

 private:
  /* Mutate the session and re-stamp only when the call reported a change. */
  void move_update(bContext *C, VectorEditHost &host, const float2 &px, ShapeAxisLock lock);
  void transform_update(bContext *C, VectorEditHost &host, const float2 &px, ShapeAxisLock lock);
  void point_update(bContext *C, VectorEditHost &host, const float2 &px);
  /** The origin drag only moves the marker, not the geometry, so it redraws the overlay instead
   * of re-stamping the composited preview. */
  void origin_update(bContext *C, VectorEditHost &host, const float2 &px);

  /** Resolve the handle under \a px and begin the matching gesture: Point starts a point drag,
   * Move a whole-shape drag, Origin only selects. Returns true when the press
   * is owned by the session (a gesture started or the origin was picked), false over no handle. */
  bool begin_gesture_at(VectorEditHost &host, const float2 &px);

  Gesture gesture_ = Gesture::None;
  ShapeAxisLock axis_lock_ = ShapeAxisLock::None;
  /** Last cursor in session pixels (key-initiated gestures use it). */
  float2 cursor_px_ = float2(0.0f);
  /** Cursor at the start of a value drag (region pixels), for the F / Shift+F delta. */
  int2 start_mval_ = int2(0);
  float value_start_ = 0.0f;
};

}  // namespace blender::ed::sculpt_paint::shape
