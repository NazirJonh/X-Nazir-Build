/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <cfloat>
#include <cmath>

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "DNA_curves_types.h"

#include "BKE_curves.hh"

#include "BLT_translation.hh"

#include "ED_screen.hh"

#include "UI_resources.hh"

#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_curve_patch_actions.hh"
#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_editor.hh"
#include "paint_curve_patch_host.hh"

namespace blender::ed::sculpt_paint {

bool CurvePatchCurveEditor::radius_drag_begin(bContext &C,
                                              CurvePatchEditorHost &host,
                                              CurvePatchScreenAdapter &adapter)
{
  const int active_point = host.document().active_point;
  bke::CurvesGeometry &curve = host.curve();
  if (active_point < 0 || active_point >= curve.points_num()) {
    return false;
  }
  bke::CurvesGeometry projected;
  CurvePatchPickSpace space;
  Vector<PaintCurvePoint> screen_points;
  if (!screen_points_get(C, adapter, curve, projected, space, screen_points)) {
    return false;
  }
  paintcurve_radius_handle_screen_get_from_geometry(
      curve, screen_points.data(), active_point, &radius_handle_);
  dragging_radius_ = true;
  return true;
}

void CurvePatchCurveEditor::drag_end()
{
  dragging_point_ = false;
  dragging_handle_ = false;
  dragging_radius_ = false;
  dragging_segment_ = false;
}

/* -------------------------------------------------------------------- */
/** \name Keyboard Transform (G / R / S)
 *
 * See #CurvePatchXform for why these are owned here instead of handed to `transform.translate`.
 * All three act on the active point and its two handles, which is exactly the scope the Transform
 * hand-off had -- the conversion it used built `TransData` for those three and nothing else.
 * \{ */

bool CurvePatchCurveEditor::xform_begin(bContext &C,
                                        const CurvePatchXform mode,
                                        const wmEvent &event,
                                        CurvePatchEditorHost &host,
                                        CurvePatchScreenAdapter &adapter)
{
  if (mode == CurvePatchXform::None || this->is_dragging()) {
    return false;
  }
  bke::CurvesGeometry &curve = host.curve();
  const int point = host.document().active_point;
  if (point < 0 || !paintcurve_geometry_is_valid(curve) || point >= curve.points_num()) {
    return false;
  }
  for (int h = 0; h < 3; h++) {
    xform_initial_[h] = paintcurve_geom_co(curve, point, h);
  }
  /* Rotation and scale need the point's own screen position to measure the cursor against; a point
   * that does not project (behind the camera) cannot be transformed this way at all. */
  if (!adapter.project_to_screen(C, xform_initial_[1], xform_pivot_mval_)) {
    return false;
  }
  xform_mode_ = mode;
  xform_point_ = point;
  xform_axis_ = -1;
  xform_plane_ = false;
  xform_start_mval_ = float2(float(event.mval[0]), float(event.mval[1]));
  this->xform_status_set(C);
  return true;
}

/* The status bar is the ONLY thing that tells the user a transform is running: it consumes every
 * event until it ends, so without this a transform started by accident reads as a frozen
 * interface -- which is exactly how one reached the user, via a modified G/R/S press the canvas
 * keymap owns. */
void CurvePatchCurveEditor::xform_status_set(bContext &C) const
{
  const bool is_rotate = xform_mode_ == CurvePatchXform::Rotate;

  WorkspaceStatus status(&C);
  status.item((xform_mode_ == CurvePatchXform::Translate) ? IFACE_("Move") :
              is_rotate                                   ? IFACE_("Rotate") :
                                                            IFACE_("Resize"),
              ICON_MOUSE_MOVE);
  status.item(IFACE_("Confirm"), ICON_MOUSE_LMB, ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
  /* One entry per axis rather than a range, so the entry for the LOCKED axis is the one that
   * lights up -- which axis is active is the single thing the user most needs back from this bar,
   * and a range cannot show it. */
  status.item_bool(IFACE_("X Axis"), xform_axis_ == 0, ICON_EVENT_X);
  status.item_bool(IFACE_("Y Axis"), xform_axis_ == 1, ICON_EVENT_Y);
  status.item_bool(IFACE_("Z Axis"), xform_axis_ == 2, ICON_EVENT_Z);
  if (!is_rotate) {
    /* A rotation turns around ONE axis, so there is no plane for Shift to select there -- see
     * #xform_apply, which ignores `xform_plane_` in that mode. Advertising it would be a lie. */
    status.item_bool(IFACE_("Plane"), xform_plane_, ICON_EVENT_SHIFT);
  }
}

void CurvePatchCurveEditor::xform_end()
{
  xform_mode_ = CurvePatchXform::None;
  xform_point_ = -1;
  xform_axis_ = -1;
  xform_plane_ = false;
}

void CurvePatchCurveEditor::xform_restore(CurvePatchEditorHost &host)
{
  bke::CurvesGeometry &curve = host.curve();
  if (xform_point_ < 0 || !paintcurve_geometry_is_valid(curve) ||
      xform_point_ >= curve.points_num())
  {
    return;
  }
  for (int h = 0; h < 3; h++) {
    paintcurve_geom_co(curve, xform_point_, h) = xform_initial_[h];
  }
  curve.tag_positions_changed();
}

void CurvePatchCurveEditor::xform_apply(bContext &C,
                                        const wmEvent &event,
                                        CurvePatchEditorHost &host,
                                        CurvePatchScreenAdapter &adapter)
{
  bke::CurvesGeometry &curve = host.curve();
  if (xform_point_ < 0 || !paintcurve_geometry_is_valid(curve) ||
      xform_point_ >= curve.points_num())
  {
    return;
  }
  const float2 mval(float(event.mval[0]), float(event.mval[1]));

  float3 axes[3];
  float3 view_axis;
  adapter.constraint_axes_get(C, axes, view_axis);

  /* The pivot is the point itself, so it never moves under rotation or scale -- only its handles
   * do. That is the "rotate/scale the active point" affordance the hand-off had, where the pivot
   * was the local center of the very three elements being transformed. */
  float3 result[3];
  switch (xform_mode_) {
    case CurvePatchXform::Translate: {
      float3 delta = adapter.space_delta_from_screen(
          C, xform_initial_[1], xform_start_mval_, mval);
      if (xform_axis_ >= 0) {
        const float3 &axis = axes[xform_axis_];
        const float3 along = axis * math::dot(delta, axis);
        delta = xform_plane_ ? (delta - along) : along;
      }
      for (int h = 0; h < 3; h++) {
        result[h] = xform_initial_[h] + delta;
      }
      break;
    }
    case CurvePatchXform::Rotate: {
      const float2 from = xform_start_mval_ - xform_pivot_mval_;
      const float2 to = mval - xform_pivot_mval_;
      const float angle = std::atan2(to.y, to.x) - std::atan2(from.y, from.x);
      /* An unconstrained rotation turns around the view direction, which is what makes it follow
       * the cursor on screen. `xform_plane_` names no second axis to turn around, so a plane
       * modifier is simply ignored here. */
      const float3 axis = (xform_axis_ >= 0) ? axes[xform_axis_] : view_axis;
      float rot[3][3];
      axis_angle_to_mat3(rot, axis, angle);
      for (int h = 0; h < 3; h++) {
        const float3 rel = xform_initial_[h] - xform_initial_[1];
        float3 turned;
        mul_v3_m3v3(turned, rot, rel);
        result[h] = xform_initial_[1] + turned;
      }
      break;
    }
    case CurvePatchXform::Scale: {
      const float from = math::length(xform_start_mval_ - xform_pivot_mval_);
      const float to = math::length(mval - xform_pivot_mval_);
      const float factor = (from > 1e-4f) ? (to / from) : 1.0f;
      for (int h = 0; h < 3; h++) {
        const float3 rel = xform_initial_[h] - xform_initial_[1];
        float3 scaled;
        if (xform_axis_ < 0) {
          scaled = rel * factor;
        }
        else {
          const float3 &axis = axes[xform_axis_];
          const float3 along = axis * math::dot(rel, axis);
          const float3 across = rel - along;
          /* Locking an axis scales along it alone; locking its plane scales everything else. */
          scaled = xform_plane_ ? (along + across * factor) : (along * factor + across);
        }
        result[h] = xform_initial_[1] + scaled;
      }
      break;
    }
    case CurvePatchXform::None:
      return;
  }

  for (int h = 0; h < 3; h++) {
    paintcurve_geom_co(curve, xform_point_, h) = result[h];
  }
  /* Only tagged, never re-derived: the hand-off wrote the same three handles through
   * #ED_curve_patch_session_active_point_handle_set and likewise recomputed nothing, so a point
   * with computed handle types behaves exactly as it did before. */
  curve.tag_positions_changed();
  host.restamp(C);
}

CurvePatchCurveEditor::Status CurvePatchCurveEditor::xform_handle_event(
    bContext &C,
    const wmEvent &event,
    CurvePatchEditorHost &host,
    CurvePatchScreenAdapter &adapter)
{
  /* Everything reaches here while a transform is in flight, so each key means what it means DURING
   * a transform: Esc aborts the transform rather than the patch, X locks an axis rather than
   * deleting the point. Nothing falls through to the editor's own switch. */
  switch (event.type) {
    case MOUSEMOVE:
      this->xform_apply(C, event, host, adapter);
      return Status::Running;

    case LEFTMOUSE:
    case EVT_RETKEY:
    case EVT_PADENTER:
      if (event.val == KM_PRESS) {
        this->xform_end();
        /* One history step for the whole transform, like every other completed edit. */
        host.after_curve_change(C);
        host.status_refresh(C);
      }
      return Status::Running;

    case EVT_ESCKEY:
    case RIGHTMOUSE:
      if (event.val == KM_PRESS) {
        this->xform_restore(host);
        this->xform_end();
        /* Re-stamped but NOT pushed: a cancelled transform is not an edit. */
        host.restamp(C);
        host.redraw(C);
        host.status_refresh(C);
      }
      return Status::Running;

    case EVT_XKEY:
    case EVT_YKEY:
    case EVT_ZKEY: {
      if (event.val != KM_PRESS) {
        return Status::Running;
      }
      const int axis = (event.type == EVT_XKEY) ? 0 : (event.type == EVT_YKEY) ? 1 : 2;
      const bool plane = (event.modifier & KM_SHIFT) != 0;
      /* Pressing the same key again releases the constraint, the way Transform's own axis keys
       * toggle. */
      if (xform_axis_ == axis && xform_plane_ == plane) {
        xform_axis_ = -1;
        xform_plane_ = false;
      }
      else {
        xform_axis_ = axis;
        xform_plane_ = plane;
      }
      this->xform_apply(C, event, host, adapter);
      this->xform_status_set(C);
      return Status::Running;
    }

    default:
      /* Consumed rather than passed through: a transform is modal, and letting unrelated operators
       * run against a half-transformed point is what the hand-off's `OPTYPE_BLOCKING` prevented.
       */
      return Status::Running;
  }
}

/** \} */

/* Build the screen-point array for one curve, however this space wants it handed over. Returns
 * false when the curve is not pickable. */
bool CurvePatchCurveEditor::screen_points_get(bContext &C,
                                              CurvePatchScreenAdapter &adapter,
                                              const bke::CurvesGeometry &source,
                                              bke::CurvesGeometry &r_projected,
                                              CurvePatchPickSpace &r_space,
                                              Vector<PaintCurvePoint> &r_screen_points)
{
  if (!adapter.pick_space_get(C, source, r_projected, r_space)) {
    return false;
  }
  if (r_space.geom == nullptr || !paintcurve_geometry_is_valid(*r_space.geom)) {
    return false;
  }
  paintcurve_build_screen_points_from_geometry(
      *r_space.geom, r_space.use_3d_space, r_space.vc, r_screen_points);
  return !r_screen_points.is_empty();
}

int CurvePatchCurveEditor::pick_point_or_active(bContext &C,
                                                const wmEvent &event,
                                                CurvePatchEditorHost &host,
                                                CurvePatchScreenAdapter &adapter)
{
  bke::CurvesGeometry projected;
  CurvePatchPickSpace space;
  Vector<PaintCurvePoint> screen_points;
  if (screen_points_get(C, adapter, host.curve(), projected, space, screen_points)) {
    const float pos[2] = {float(event.mval[0]), float(event.mval[1])};
    char selflag = 0;
    const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                     pos,
                                                     /*ignore_pivot=*/false,
                                                     PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                     &selflag);
    if (hit >= 0) {
      return hit;
    }
  }
  return host.document().active_point;
}

/* LMB press: pick a radius handle, then a point / tangent handle, then the wire.
 *
 * Every stage scans ALL pickable curves and keeps the CLOSEST hit, not the first one found:
 * overlapping patches would otherwise give the click an arbitrary priority based on list order
 * rather than on what the user actually pointed at. With a single curve the scan collapses to the
 * plain pick. False on a miss. */
bool CurvePatchCurveEditor::drag_start_from_press(bContext &C,
                                                  const wmEvent &event,
                                                  CurvePatchEditorHost &host,
                                                  CurvePatchScreenAdapter &adapter)
{
  Vector<bke::CurvesGeometry *> curves;
  host.pickable_curves(curves);
  const float pos[2] = {float(event.mval[0]), float(event.mval[1])};

  /* Radius handles take priority: they sit off the wire, so a hit there is unambiguous. */
  int best_curve = -1;
  int best_point = -1;
  float best_dist = FLT_MAX;
  PaintCurveRadiusHandleScreen best_handle = {};
  for (const int i : curves.index_range()) {
    bke::CurvesGeometry projected;
    CurvePatchPickSpace space;
    Vector<PaintCurvePoint> screen_points;
    if (!screen_points_get(C, adapter, *curves[i], projected, space, screen_points)) {
      continue;
    }
    const int hit = paintcurve_find_radius_handle_at_pos_from_geometry(
        *curves[i], screen_points.as_span(), pos, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
    if (hit < 0) {
      continue;
    }
    PaintCurveRadiusHandleScreen handle;
    paintcurve_radius_handle_screen_get_from_geometry(
        *curves[i], screen_points.data(), hit, &handle);
    const float end[2] = {handle.end.x, handle.end.y};
    const float dist = len_v2v2(pos, end);
    if (dist < best_dist) {
      best_dist = dist;
      best_curve = i;
      best_point = hit;
      best_handle = handle;
    }
  }
  if (best_curve >= 0) {
    host.pickable_curve_activate(best_curve);
    host.active_point_set(best_point, 0x07);
    dragging_radius_ = true;
    radius_handle_ = best_handle;
    return true;
  }

  /* Tests the pivot AND both Bezier tangent handles. */
  char best_selflag = 0;
  best_dist = FLT_MAX;
  for (const int i : curves.index_range()) {
    bke::CurvesGeometry projected;
    CurvePatchPickSpace space;
    Vector<PaintCurvePoint> screen_points;
    if (!screen_points_get(C, adapter, *curves[i], projected, space, screen_points)) {
      continue;
    }
    char selflag = 0;
    const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                     pos,
                                                     /*ignore_pivot=*/false,
                                                     PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                     &selflag);
    if (hit < 0) {
      continue;
    }
    const float *hit_co = (selflag == SEL_F1) ? screen_points[hit].bez.vec[0] :
                          (selflag == SEL_F3) ? screen_points[hit].bez.vec[2] :
                                                screen_points[hit].bez.vec[1];
    const float dist = len_v2v2(pos, hit_co);
    if (dist < best_dist) {
      best_dist = dist;
      best_curve = i;
      best_point = hit;
      best_selflag = selflag;
    }
  }
  if (best_curve >= 0) {
    host.pickable_curve_activate(best_curve);
    const uint8_t sel_bit = (best_selflag == SEL_F1) ? 0x01 :
                            (best_selflag == SEL_F3) ? 0x04 :
                                                       0x02;
    host.active_point_set(best_point, sel_bit);

    if (ELEM(best_selflag, SEL_F1, SEL_F3)) {
      dragging_handle_ = true;
      handle_is_left_ = (best_selflag == SEL_F1);
    }
    else {
      dragging_point_ = true;
      drag_start_mval_ = float2(pos[0], pos[1]);
      /* Non-const: #paintcurve_geom_co has only a mutable accessor, the same way every other
       * caller in this module binds it. */
      bke::CurvesGeometry &curve = host.curve();
      for (int h = 0; h < 3; h++) {
        point_initial_[h] = paintcurve_geom_co(curve, best_point, h);
      }
    }
    return true;
  }

  /* Nothing on a point or handle: try the curve wire itself, which reshapes the segment's two
   * shared handles. Same affordance Stroke Method: Curve has. */
  int best_segment_a = -1;
  int best_segment_b = -1;
  float best_segment_t = 0.0f;
  float best_dist_sq = FLT_MAX;
  for (const int i : curves.index_range()) {
    bke::CurvesGeometry projected;
    CurvePatchPickSpace space;
    Vector<PaintCurvePoint> screen_points;
    if (!screen_points_get(C, adapter, *curves[i], projected, space, screen_points)) {
      continue;
    }
    int segment_index = -1;
    int segment_index_next = -1;
    float edge_t = 0.0f;
    float dist_sq = FLT_MAX;
    /* The screen points are REQUIRED here, not an optimization: with `use_3d_space` false --
     * which is how the flat canvas presents its curve --
     * #paintcurve_build_screen_segment_polyline_from_geometry builds the polyline from this span
     * ALONE and never looks at the geometry. Passing an empty one left every segment with an empty
     * polyline, so no wire was ever hit and segment dragging did nothing on the canvas. A 3D
     * target evaluates from the geometry and ignores the span, which is why the same code worked
     * in the viewport. */
    if (paintcurve_find_closest_segment_from_geometry(*space.geom,
                                                      space.use_3d_space,
                                                      space.vc,
                                                      screen_points.as_span(),
                                                      pos,
                                                      PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                      &segment_index,
                                                      &segment_index_next,
                                                      &edge_t,
                                                      &dist_sq) &&
        dist_sq < best_dist_sq)
    {
      best_dist_sq = dist_sq;
      best_curve = i;
      best_segment_a = segment_index;
      best_segment_b = segment_index_next;
      best_segment_t = edge_t;
    }
  }
  if (best_curve >= 0) {
    host.pickable_curve_activate(best_curve);
    dragging_segment_ = true;
    segment_index_a_ = best_segment_a;
    segment_index_b_ = best_segment_b;
    segment_t_ = best_segment_t;
    return true;
  }

  return false;
}

/* MOUSEMOVE while a drag is in flight. False when nothing was dragging. */
bool CurvePatchCurveEditor::drag_apply_move(bContext &C,
                                            const wmEvent &event,
                                            CurvePatchEditorHost &host,
                                            CurvePatchScreenAdapter &adapter)
{
  bke::CurvesGeometry &curve = host.curve();
  const int active_point = host.document().active_point;
  const float2 mval(float(event.mval[0]), float(event.mval[1]));

  if (dragging_segment_) {
    const int point_num = curve.points_num();
    if (segment_index_a_ < 0 || segment_index_a_ >= point_num || segment_index_b_ < 0 ||
        segment_index_b_ >= point_num)
    {
      return false;
    }
    adapter.drag_segment_apply(C, curve, segment_index_a_, segment_index_b_, segment_t_, mval);
    host.restamp(C);
    return true;
  }

  if (active_point < 0 || active_point >= curve.points_num()) {
    return false;
  }

  if (dragging_point_) {
    adapter.drag_point_apply(C, curve, active_point, point_initial_, drag_start_mval_, mval);
    host.restamp(C);
    return true;
  }

  if (dragging_handle_) {
    /* Dragging a handle directly makes the point's shape freely adjustable, so a COMPUTED type
     * (Auto/Vector, whose positions #calculate_bezier_auto_handles would immediately overwrite) is
     * promoted to Align. A type the user chose explicitly (Free) is left alone -- overwriting it
     * would silently re-couple both handles of a Free point.
     *
     * Promoted BEFORE the space moves the handle, so that whatever handle recomputation the space
     * does afterwards sees the final types. */
    for (MutableSpan<int8_t> types :
         {curve.handle_types_left_for_write(), curve.handle_types_right_for_write()})
    {
      if (ELEM(types[active_point], BEZIER_HANDLE_AUTO, BEZIER_HANDLE_VECTOR)) {
        types[active_point] = BEZIER_HANDLE_ALIGN;
      }
    }
    adapter.drag_handle_apply(C, curve, active_point, handle_is_left_, mval);
    host.restamp(C);
    return true;
  }

  if (dragging_radius_) {
    /* The drag axis was fixed in region pixels at drag start, so every move just re-projects the
     * cursor onto it -- the same inversion the Paint Curve radius slide uses. Screen-space in both
     * spaces, so no adapter primitive is needed. */
    const float pos[2] = {mval.x, mval.y};
    curve.radius_for_write()[active_point] = paintcurve_radius_from_handle_screen_pos(
        &radius_handle_, pos);
    curve.tag_positions_changed();
    host.restamp(C);
    return true;
  }

  return false;
}

/* Ctrl+RMB: subdivide the segment under the cursor, or append a point at the cursor. */
void CurvePatchCurveEditor::insert_or_append_point(bContext &C,
                                                   const wmEvent &event,
                                                   CurvePatchEditorHost &host,
                                                   CurvePatchScreenAdapter &adapter)
{
  const float pos[2] = {float(event.mval[0]), float(event.mval[1])};

  /* Closest insert segment across every pickable curve, for the same reason the press hit-test
   * scans them all. */
  Vector<bke::CurvesGeometry *> curves;
  host.pickable_curves(curves);
  int best_curve = -1;
  int best_segment_a = -1;
  int best_segment_b = -1;
  float best_segment_t = 0.0f;
  float best_dist_sq = FLT_MAX;
  for (const int i : curves.index_range()) {
    bke::CurvesGeometry projected;
    CurvePatchPickSpace space;
    Vector<PaintCurvePoint> screen_points;
    if (!screen_points_get(C, adapter, *curves[i], projected, space, screen_points)) {
      continue;
    }
    int segment_index = -1;
    int segment_index_next = -1;
    float edge_t = 0.0f;
    float dist_sq = FLT_MAX;
    if (paintcurve_find_insert_segment_from_geometry(*space.geom,
                                                     space.use_3d_space,
                                                     space.vc,
                                                     pos,
                                                     &segment_index,
                                                     &segment_index_next,
                                                     &edge_t,
                                                     &dist_sq) &&
        dist_sq < best_dist_sq)
    {
      best_dist_sq = dist_sq;
      best_curve = i;
      best_segment_a = segment_index;
      best_segment_b = segment_index_next;
      best_segment_t = edge_t;
    }
  }

  if (best_curve >= 0) {
    host.pickable_curve_activate(best_curve);
    /* The hit may have been found on a projected copy, but the projection is affine within a
     * segment, so the same `edge_t` subdivides the canonical Bezier at the same place. */
    const int insert_index = paintcurve_geometry_insert_point_at_segment(
        host.curve(), best_segment_a, best_segment_b, /*active_curve=*/0, best_segment_t);
    if (insert_index >= 0) {
      host.active_point_set(insert_index, 0x02);
      return;
    }
  }

  /* No segment under the cursor: extend the active spline at its end. */
  bke::CurvesGeometry &curve = host.curve();
  float3 position(0.0f);
  float3 normal(0.0f, 0.0f, 1.0f);
  adapter.place_new_point(C, event.mval, position, normal);
  /* Both are in/out: #paintcurve_geometry_add_point updates them to name the point it appended. */
  int active_curve = 0;
  int add_index = paintcurve_geometry_is_valid(curve) ? int(curve.points_by_curve()[0].size()) : 0;
  paintcurve_geometry_add_point(curve,
                                position,
                                normal,
                                /*create_new_spline=*/curve.points_num() == 0,
                                active_curve,
                                add_index);
  host.active_point_set(curve.points_num() - 1, 0x02);
}

CurvePatchCurveEditor::Status CurvePatchCurveEditor::handle_event(bContext &C,
                                                                  const wmEvent &event,
                                                                  CurvePatchEditorHost &host,
                                                                  CurvePatchScreenAdapter &adapter)
{
  /* Live brush sync. The Curve Patch panels write straight into the brush and RNA only broadcasts
   * `NC_BRUSH | NA_EDITED`, so the target would keep showing the old patch until the next curve
   * edit. Deliberately ABOVE the event switch (and above every pass-through return) so a slider
   * dragged in a sidebar is caught even though the cursor never enters the region. */
  if (host.sync_live_brush(C)) {
    if (!host.is_alive(C)) {
      /* The re-stamp ended the session under us. */
      return Status::Cancelled;
    }
  }

  /* A running G/R/S owns every event until it confirms or aborts -- ahead of `in_region`, which a
   * keyboard transform does not care about, and ahead of the switch below, where Esc and X mean
   * something else entirely. */
  if (this->is_xform_active()) {
    return this->xform_handle_event(C, event, host, adapter);
  }

  /* The region gate, for the whole switch below rather than case by case.
   *
   * A modal handler is registered at WINDOW level, so it sees every event in the window before the
   * area under the cursor does. Everything the switch handles acts on the curve the cursor is
   * over, so an event from outside this editor's region belongs to whatever editor IS there and
   * must reach it untouched. Written once here because the per-case version kept growing a new
   * hole every time a case was added: a swallowed `LEFTMOUSE` release left a header colour swatch
   * stuck mid-click, and `X` over a panel deleted a point the user was not pointing at.
   *
   * 3D Sculpt Mode gates the same way, one step earlier -- its modal tests the cursor against
   * every 3D viewport before the core is called at all -- so its adapter reports `true`
   * unconditionally and this costs it nothing.
   *
   * Three exemptions, matching that gate's own:
   * - an in-flight drag, so dragging a point past the canvas edge is neither orphaned nor robbed
   *   of the release that ends it (a running G/R/S already returned above);
   * - the session terminators, the only way to leave a patch with the cursor elsewhere;
   * - the undo chord, which must be swallowed WHEREVER it is pressed: letting it through reaches
   *   global undo, which pops a step out from under the live session. */
  const bool in_region = adapter.event_in_region(C, event);
  if (!in_region && !this->is_dragging()) {
    const bool ends_session = ELEM(event.type, EVT_RETKEY, EVT_PADENTER, EVT_SPACEKEY, EVT_ESCKEY);
    const bool is_undo_chord = event.type == EVT_ZKEY && event.val == KM_PRESS &&
                               (event.modifier & (KM_CTRL | KM_OSKEY)) != 0;
    if (!ends_session && !is_undo_chord) {
      if (event.type == MOUSEMOVE) {
        /* Keep the overlay's hover state current even while the cursor is away, exactly as the
         * per-case version did before this gate existed. */
        host.redraw(C);
      }
      return Status::PassThrough;
    }
  }

  switch (event.type) {
    case EVT_RETKEY:
    case EVT_PADENTER:
    case EVT_SPACEKEY:
      if (event.val == KM_PRESS) {
        host.commit(C);
        return Status::Finished;
      }
      break;

    case EVT_ESCKEY:
      if (event.val == KM_PRESS) {
        host.cancel(C);
        return Status::Cancelled;
      }
      break;

    case LEFTMOUSE:
      if (event.val == KM_PRESS) {
        if ((event.modifier & KM_ALT) != 0) {
          /* Alt+LMB is reserved for view navigation (#IMAGE_OT_view_rotate_interactive in the 2D
           * canvas, orbit in the 3D viewport) -- never a curve-patch action -- so it must reach
           * the keymap underneath untouched, rather than being swallowed as a "miss" below. */
          return Status::PassThrough;
        }
        if (!this->drag_start_from_press(C, event, host, adapter)) {
          /* A miss inside the region is swallowed rather than passed through: underneath sits the
           * paint keymap, whose stroke operator would start painting on top of the live patch.
           * Applying the patch is Enter's job alone. */
          return Status::Running;
        }
        host.redraw(C);
      }
      else if (event.val == KM_RELEASE) {
        if (this->is_dragging()) {
          this->drag_end();
          /* One history step per drag, recorded on release -- not one per #MOUSEMOVE, which would
           * fill the history with intermediate positions nobody wants to step through. The gate
           * above lets this reach us even with the cursor off the region, because the release ends
           * OUR drag. */
          host.after_curve_change(C);
          break;
        }
        /* A release with no drag behind it can only be in-region here; the gate passed the
         * off-region ones on to the widget that is waiting for them. */
        host.redraw(C);
      }
      break;

    case MOUSEMOVE:
      if (!this->drag_apply_move(C, event, host, adapter)) {
        /* Not dragging: keep the overlay's hover state current, but let the event reach the editor
         * underneath so cursor drawing and the region header keep updating. */
        host.redraw(C);
        return Status::PassThrough;
      }
      break;

    case RIGHTMOUSE:
      if (event.val == KM_PRESS && (event.modifier & KM_CTRL)) {
        insert_or_append_point(C, event, host, adapter);
        host.after_curve_change(C);
        break;
      }
      if (event.val == KM_PRESS) {
        /* Plain RMB over a control point opens the context menu; a hit on either tangent handle
         * counts as a hit on its point, since every entry acts on the whole point. */
        bke::CurvesGeometry projected;
        CurvePatchPickSpace space;
        Vector<PaintCurvePoint> screen_points;
        if (screen_points_get(C, adapter, host.curve(), projected, space, screen_points)) {
          const float pos[2] = {float(event.mval[0]), float(event.mval[1])};
          char selflag = 0;
          const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                           pos,
                                                           /*ignore_pivot=*/false,
                                                           PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                           &selflag);
          if (hit >= 0) {
            host.active_point_set(hit, 0x02);
            host.context_menu_open(C);
            break;
          }
        }
      }
      /* Plain RMB away from a point must reach `BRUSH_OT_stencil_control` and the rest of the
       * paint keymap -- consuming it on a miss would leave stencil adjustment dead for the whole
       * session. */
      return Status::PassThrough;

    case EVT_XKEY:
    case EVT_DELKEY: {
      /* The gate above is what keeps this off other editors: it deletes the ACTIVE point, which
       * #pick_point_or_active falls back to when the cursor is over nothing pickable -- so an
       * ungated X pressed over a header removed a point from a curve the user was not pointing
       * at. */
      if (event.val != KM_PRESS) {
        return Status::PassThrough;
      }
      const int target = pick_point_or_active(C, event, host, adapter);
      if (!curve_patch_action_delete_point(C, host, target)) {
        /* Away from the curve (or at the two-point floor), X belongs to the paint keymap's
         * primary/secondary color swap. */
        return Status::PassThrough;
      }
      /* The removed point may have been under an in-flight drag; drop the drag state so the next
       * #MOUSEMOVE cannot index into geometry that no longer holds it. */
      this->drag_end();
      break;
    }

    /* G / R / S. A target that binds them on a modal keymap of its own starts the transform from
     * there instead (3D Sculpt Mode does) and never reaches this case; the flat canvas, which has
     * no modal map, gets them here.
     *
     * ANY modifier disqualifies the press, not just Alt. The canvas keymap binds modified variants
     * of these very keys -- `Alt+S` is the radius drag and `Shift+S` toggles `use_smooth_stroke`
     * (`blender_default.py`, "Image Paint") -- and a transform started by one of those is
     * invisible to the user while it swallows every following event, `N` included, until Esc.
     * Requiring a bare press keeps this to the three keys Blender means by G/R/S everywhere else.
     */
    case EVT_GKEY:
    case EVT_RKEY:
    case EVT_SKEY: {
      if (event.val != KM_PRESS || event.modifier != 0) {
        return Status::PassThrough;
      }
      const CurvePatchXform mode = (event.type == EVT_GKEY) ? CurvePatchXform::Translate :
                                   (event.type == EVT_RKEY) ? CurvePatchXform::Rotate :
                                                              CurvePatchXform::Scale;
      if (!this->xform_begin(C, mode, event, host, adapter)) {
        return Status::PassThrough;
      }
      break;
    }

    /* All three follow #EVT_XKEY above: a press the editor cannot act on is passed through rather
     * than swallowed. A modal handler receives the whole window's events, so consuming these
     * unconditionally would kill the same keys over a sidebar, a header or another area for as
     * long as the patch is live. */
    case EVT_CKEY:
      if (event.val != KM_PRESS || !curve_patch_action_toggle_cyclic(C, host)) {
        return Status::PassThrough;
      }
      break;

    case EVT_YKEY:
      if (event.val != KM_PRESS || !curve_patch_action_switch_direction(C, host)) {
        return Status::PassThrough;
      }
      break;

    case EVT_VKEY:
      if (event.val != KM_PRESS ||
          !curve_patch_action_cycle_handle_type(C, host, host.document().active_point))
      {
        return Status::PassThrough;
      }
      break;

    case EVT_ZKEY:
      /* Swallow the undo/redo chord. Letting it through would reach global undo and pop a step out
       * from under the live session; plain Z still passes.
       *
       * Swallowing is all that happens here. A target that wants Ctrl+Z to step its OWN session
       * history binds it before the event reaches the core -- 3D Sculpt Mode does, through
       * #CURVE_PATCH_MODAL_UNDO. A target that has no session history (the flat canvas, see the
       * note in `paint_curve_patch_host.hh`) therefore has no undo inside a live patch at all: the
       * chord is consumed and nothing happens. That is a gap, not a design. */
      if (event.val == KM_PRESS && (event.modifier & (KM_CTRL | KM_OSKEY))) {
        return Status::Running;
      }
      return Status::PassThrough;

    default:
      return Status::Unhandled;
  }

  return Status::Running;
}

}  // namespace blender::ed::sculpt_paint
