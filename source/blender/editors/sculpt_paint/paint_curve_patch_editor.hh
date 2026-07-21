/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The modal editing core of a live Curve Patch: hit testing, the four drags, insert / append, the
 * hotkeys and the context menu -- everything that is the same wherever the patch is being stamped.
 *
 * Not an operator. Each target registers its own modal operator, puts one of these in
 * `op->customdata`, and forwards events to #CurvePatchCurveEditor::handle_event. What the core
 * cannot know it asks its two collaborators: #CurvePatchEditorHost for the target (what the curve
 * is, how to re-stamp it, how the session ends) and #CurvePatchScreenAdapter for the space the
 * cursor lives in (how to project the curve, where a click lands).
 *
 * A target keeps whatever the core returns #Status::Unhandled for. That is how 3D Sculpt Mode
 * keeps its surface snapping, its Transform G/R/S hand-off and its modal keymap without any of
 * that leaking in here.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BKE_curves.hh"

#include "paint_curve_intern.hh"

struct ARegion;
struct ViewContext;
struct wmEvent;

namespace blender {
struct bContext;
}

namespace blender::ed::sculpt_paint {

class CurvePatchEditorHost;

/**
 * How one space presents a control curve to the shared paint-curve pickers.
 *
 * The pickers all take the same four arguments -- a geometry, `use_3d_space`, a #ViewContext and a
 * screen-point array -- but the two spaces fill them differently: the flat canvas projects a
 * throwaway copy of the curve into region pixels and passes that with `use_3d_space = false`, 3D
 * passes the object-space curve itself with a live #ViewContext. Describing that once here is what
 * lets the hit tests be written once.
 */
struct CurvePatchPickSpace {
  /** The geometry to hand the pickers. Never null on success; may point either at the caller's
   * projection storage or straight at the source curve. */
  const bke::CurvesGeometry *geom = nullptr;
  bool use_3d_space = false;
  const ViewContext *vc = nullptr;
};

/**
 * Everything the editing core needs to know about the space the cursor lives in.
 *
 * Every method here is a place where the two spaces genuinely differ. What does NOT belong here is
 * anything the core can decide on its own -- which drag is in flight, the order of the hit tests,
 * when a history step is recorded, how a computed handle type is promoted.
 */
class CurvePatchScreenAdapter {
 public:
  virtual ~CurvePatchScreenAdapter() = default;

  /**
   * Describe how `source` must be handed to the pickers, projecting it into `r_projected_storage`
   * where the space needs a projected copy. Returns false when there is nothing pickable.
   *
   * Note that the RADIUS a picker reads always comes from `source`, never from the projection: the
   * flat canvas copies the radius attribute across unchanged when it projects, so both spaces
   * agree.
   */
  virtual bool pick_space_get(bContext &C,
                              const bke::CurvesGeometry &source,
                              bke::CurvesGeometry &r_projected_storage,
                              CurvePatchPickSpace &r_space) const = 0;

  /** Where a newly created point goes, and the normal to store with it. A 3D target places the
   * point against the real surface when it can, and falls back to the patch plane when it
   * cannot. */
  virtual void place_new_point(bContext &C,
                               const int mval[2],
                               float3 &r_position,
                               float3 &r_normal) const = 0;

  /**
   * True when `event` belongs to the region this editor owns.
   *
   * Not a formality: a modal handler's region stays frozen at the one it was registered with, and
   * `event->mval` is derived from that region no matter where the cursor really is -- so a click
   * on a sidebar produces canvas-looking coordinates that the hit tests would happily match.
   */
  virtual bool event_in_region(bContext &C, const wmEvent &event) const = 0;

  /**
   * Move a whole control point (pivot plus both handles) under the cursor.
   *
   * The whole mutation belongs to the space, recomputed handles and `tag_positions_changed()`
   * included: 3D snaps the point onto the surface when the ray hits and only falls back to a
   * screen-space delta when it misses, and the two branches leave the handles differently. `mval_
   * start` and `initial` are the cursor and the point's three positions as they were when the drag
   * began -- applying the delta to those rather than to the live values keeps a long drag from
   * accumulating rounding drift.
   */
  virtual void drag_point_apply(bContext &C,
                                bke::CurvesGeometry &curve,
                                int point_index,
                                const float3 initial[3],
                                const float2 &mval_start,
                                const float2 &mval_now) const = 0;

  /** Put one Bezier tangent handle under the cursor. The core has already promoted a computed
   * handle type to Align, so this may recompute handles freely. */
  virtual void drag_handle_apply(bContext &C,
                                 bke::CurvesGeometry &curve,
                                 int point_index,
                                 bool handle_is_left,
                                 const float2 &mval) const = 0;

  /** Reshape the segment between `point_a` and `point_b` so its point at `segment_t` follows the
   * cursor. */
  virtual void drag_segment_apply(bContext &C,
                                  bke::CurvesGeometry &curve,
                                  int point_a,
                                  int point_b,
                                  float segment_t,
                                  const float2 &mval) const = 0;

  /* The three below exist for the keyboard transforms (G/R/S). They deliberately expose the space
   * rather than the operation: the algebra of translate/rotate/scale is identical in both spaces
   * and lives in the core, so only the screen<->space conversions are asked for here. */

  /**
   * The cursor's movement expressed as a delta in the curve's own space.
   *
   * `reference` fixes the depth the screen delta is measured at -- in a viewport a screen delta
   * means nothing without one. On a flat canvas it is ignored.
   */
  virtual float3 space_delta_from_screen(bContext &C,
                                         const float3 &reference,
                                         const float2 &mval_start,
                                         const float2 &mval_now) const = 0;

  /** Project one point of the curve's space into region pixels. False when it does not project. */
  virtual bool project_to_screen(bContext &C, const float3 &co, float2 &r_mval) const = 0;

  /**
   * The axes an X/Y/Z constraint locks to, expressed in the curve's own space, plus the axis an
   * unconstrained rotation turns around (the view direction in a viewport, +Z on a flat canvas).
   *
   * Unit length, but not necessarily orthogonal to each other once a non-uniformly scaled object
   * transform is folded in -- which is why the core projects onto them rather than assuming a
   * basis.
   */
  virtual void constraint_axes_get(bContext &C, float3 r_axes[3], float3 &r_view_axis) const = 0;
};

/**
 * The keyboard transforms, G / R / S.
 *
 * Implemented here rather than by handing off to `transform.translate` and friends, which is what
 * this used to do. Those operators carry `OPTYPE_UNDO` and finish on a LATER event than the one
 * that spawned them -- outside the `wm->op_undo_depth` bracket the window manager puts around a
 * modal callback -- so each G/R/S press pushed a GLOBAL undo step. A live patch is one operation:
 * the user got a history full of intermediate point positions to step back through, and on the
 * image canvas each foreign push additionally adopted the session's open image transaction and
 * split its pixels across several steps. Owning the interaction is what makes the patch a single
 * step again.
 *
 * What is deliberately NOT reproduced from Transform: numeric input, the transform gizmos and
 * per-transform snapping. Point-drag surface snapping is untouched -- it lives on the plain mouse
 * drag, which never went through Transform.
 */
enum class CurvePatchXform {
  None,
  Translate,
  Rotate,
  Scale,
};

class CurvePatchCurveEditor {
 public:
  enum class Status {
    /** Handled; the modal keeps running. */
    Running,
    /** Not ours -- the event must reach the editor underneath. */
    PassThrough,
    /** The host committed the session; the operator should tear down and finish. */
    Finished,
    /** The host cancelled the session; the operator should tear down and cancel. */
    Cancelled,
    /** The core has no opinion. The target may handle the event itself. */
    Unhandled,
  };

  /* Holds only the drag state. The target and the space are passed in per event rather than
   * stored: a target whose host or adapter is built on the stack per event (3D Sculpt Mode, whose
   * object and session are re-resolved from the context every time) would otherwise leave dangling
   * references behind between events. */
  Status handle_event(bContext &C,
                      const wmEvent &event,
                      CurvePatchEditorHost &host,
                      CurvePatchScreenAdapter &adapter);

  /** True while any of the four drags is in flight. Targets consult it before starting an action
   * of their own (3D refuses to hand off to Transform mid-drag). */
  bool is_dragging() const
  {
    return dragging_point_ || dragging_handle_ || dragging_radius_ || dragging_segment_ ||
           this->is_xform_active();
  }

  /**
   * Start a radius drag on the active point with no click behind it, for a target that also binds
   * the radius to a key. The drag then ends the way the target decides -- there is no button held
   * down for a release to arrive on.
   *
   * False when there is no pickable curve or no valid active point.
   */
  bool radius_drag_begin(bContext &C,
                         CurvePatchEditorHost &host,
                         CurvePatchScreenAdapter &adapter);

  /** Drop the drag state, for a target that mutated the curve behind the core's back. */
  void drag_end();

  /**
   * Start a G / R / S transform of the active point and its two handles.
   *
   * While one is in flight the core owns EVERY event -- #handle_event routes them all to the
   * transform before its own switch, so Esc means "cancel the transform" rather than "cancel the
   * patch" and X means "lock to the X axis" rather than "delete the point". A target that
   * dispatches some events itself must therefore consult #is_xform_active first and forward
   * unconditionally while it is true.
   *
   * False when there is no valid active point, or when a mouse drag is already in flight.
   */
  bool xform_begin(bContext &C,
                   CurvePatchXform mode,
                   const wmEvent &event,
                   CurvePatchEditorHost &host,
                   CurvePatchScreenAdapter &adapter);

  /** True between #xform_begin and the confirm/cancel that ends it. */
  bool is_xform_active() const
  {
    return xform_mode_ != CurvePatchXform::None;
  }

 private:
  static bool screen_points_get(bContext &C,
                                CurvePatchScreenAdapter &adapter,
                                const bke::CurvesGeometry &source,
                                bke::CurvesGeometry &r_projected,
                                CurvePatchPickSpace &r_space,
                                Vector<PaintCurvePoint> &r_screen_points);
  bool drag_start_from_press(bContext &C,
                             const wmEvent &event,
                             CurvePatchEditorHost &host,
                             CurvePatchScreenAdapter &adapter);
  bool drag_apply_move(bContext &C,
                       const wmEvent &event,
                       CurvePatchEditorHost &host,
                       CurvePatchScreenAdapter &adapter);
  static int pick_point_or_active(bContext &C,
                                  const wmEvent &event,
                                  CurvePatchEditorHost &host,
                                  CurvePatchScreenAdapter &adapter);
  static void insert_or_append_point(bContext &C,
                                     const wmEvent &event,
                                     CurvePatchEditorHost &host,
                                     CurvePatchScreenAdapter &adapter);

  /* Exactly one drag kind can be in flight at a time; kept as separate booleans rather than an
   * enum because the delete path clears them wholesale when the dragged point disappears from
   * under it. */
  bool dragging_point_ = false;
  bool dragging_handle_ = false;
  bool dragging_radius_ = false;
  bool dragging_segment_ = false;
  /** Which Bezier tangent handle #dragging_handle_ is moving. */
  bool handle_is_left_ = false;

  /** Endpoints and Bezier parameter of the segment #dragging_segment_ is reshaping. */
  int segment_index_a_ = -1;
  int segment_index_b_ = -1;
  float segment_t_ = 0.0f;

  /** Cursor position, in region pixels, where a whole-point drag began. */
  float2 drag_start_mval_ = float2(0.0f);
  /** Control point and both handles as they were when the drag started. Applying the delta to
   * these rather than to the live values keeps a long drag from accumulating rounding drift. */
  float3 point_initial_[3] = {float3(0.0f), float3(0.0f), float3(0.0f)};

  /** Radius drag axis, fixed in region pixels at drag start. */
  PaintCurveRadiusHandleScreen radius_handle_ = {};

  /* -------------------------------------------------------------------- */
  /** \name Keyboard Transform (G / R / S)
   * \{ */

  Status xform_handle_event(bContext &C,
                            const wmEvent &event,
                            CurvePatchEditorHost &host,
                            CurvePatchScreenAdapter &adapter);
  void xform_apply(bContext &C,
                   const wmEvent &event,
                   CurvePatchEditorHost &host,
                   CurvePatchScreenAdapter &adapter);
  /** Put the three handles back exactly as #xform_begin found them. */
  void xform_restore(CurvePatchEditorHost &host);
  /** Announce the running transform and its axis lock in the status bar. */
  void xform_status_set(bContext &C) const;
  void xform_end();

  CurvePatchXform xform_mode_ = CurvePatchXform::None;
  /** Point the transform acts on, captured at begin: the active point may move under it. */
  int xform_point_ = -1;
  /** 0/1/2 for a locked X/Y/Z axis, -1 for unconstrained. */
  int xform_axis_ = -1;
  /** With #xform_axis_ set, lock the PLANE perpendicular to it instead of the axis itself. */
  bool xform_plane_ = false;
  float2 xform_start_mval_ = float2(0.0f);
  /** The point's own position in region pixels, the center rotation and scale work around. */
  float2 xform_pivot_mval_ = float2(0.0f);
  /** Pivot and both handles as they were when the transform began. */
  float3 xform_initial_[3] = {float3(0.0f), float3(0.0f), float3(0.0f)};

  /** \} */
};

}  // namespace blender::ed::sculpt_paint
