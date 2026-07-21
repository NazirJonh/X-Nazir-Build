/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Recording the 2D Curve Patch anchor gesture, reducing it to an editable handful of control
 * points, and handing the result to a live #ImageCurvePatchSession.
 *
 * Behind #ImageStrokeMethodHook so the general 2D painting stroke
 * (`mesh/paint_image_ops_paint.cc`) carries one null pointer instead of four fields and eight
 * `stroke_method == BRUSH_STROKE_CURVE_PATCH` branches spread over three responsibilities.
 */

#include "paint_image_curve_patch_anchor.hh"

#include <algorithm>
#include <cfloat>

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"

#include "DNA_brush_types.h"
#include "DNA_screen_types.h"

#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_image_curve_patch.hh"

namespace blender::ed::sculpt_paint {

namespace {

/** Upper bound on control points the anchor may produce, so a slow gesture over a large image
 * still yields a curve a human can actually grab points on. */
constexpr int CURVE_PATCH_ANCHOR_POINTS_MAX = 24;

/** Simplify tolerance in screen pixels; converted to UV through the live `View2D` at the call
 * site so zoom level, not image resolution, decides how dense the control points are. */
constexpr float CURVE_PATCH_ANCHOR_SIMPLIFY_PIXELS = 6.0f;

/**
 * Ramer-Douglas-Peucker simplification of the raw anchor polyline.
 *
 * The anchor records one UV per dab, which for a normal gesture is hundreds of samples. Feeding
 * those verbatim into the edit curve makes it unusable: control points land on top of each other
 * and picking one is a coin flip. RDP keeps the samples that carry the shape (corners, curvature
 * extremes) and drops the ones a straight segment already describes, which is exactly the set a
 * user would have placed by hand.
 *
 * `tolerance` is the maximum perpendicular deviation, in the same units as the points.
 * Recursion is bounded by the sample count; the sub-range always shrinks by at least one point.
 */
static void anchor_uvs_simplify_rdp(const Span<float2> points,
                                    const int first,
                                    const int last,
                                    const float tolerance,
                                    Vector<int> &r_kept_indices)
{
  if (last <= first + 1) {
    return;
  }
  const float2 a = points[first];
  const float2 b = points[last];
  const float2 ab = b - a;
  const float ab_len_sq = math::length_squared(ab);

  int worst_index = -1;
  float worst_dist_sq = 0.0f;
  for (int i = first + 1; i < last; i++) {
    const float2 ap = points[i] - a;
    float dist_sq;
    if (ab_len_sq <= FLT_EPSILON) {
      /* Degenerate segment (the gesture looped back onto its start): fall back to the radial
       * distance so a returning stroke still keeps its far point. */
      dist_sq = math::length_squared(ap);
    }
    else {
      const float t = math::clamp(math::dot(ap, ab) / ab_len_sq, 0.0f, 1.0f);
      dist_sq = math::length_squared(ap - ab * t);
    }
    if (dist_sq > worst_dist_sq) {
      worst_dist_sq = dist_sq;
      worst_index = i;
    }
  }

  if (worst_index < 0 || worst_dist_sq <= tolerance * tolerance) {
    return;
  }
  anchor_uvs_simplify_rdp(points, first, worst_index, tolerance, r_kept_indices);
  r_kept_indices.append(worst_index);
  anchor_uvs_simplify_rdp(points, worst_index, last, tolerance, r_kept_indices);
}

/**
 * Reduce the raw anchor samples to a small, editable set of control points.
 *
 * Runs RDP at `tolerance_uv`, then raises the tolerance until the result fits within
 * #CURVE_PATCH_ANCHOR_POINTS_MAX. Doubling converges quickly because each pass at least halves
 * the detail RDP is allowed to keep, and the loop is additionally bounded so a pathological
 * input cannot spin. The first and last samples are always kept: they are what the user aimed
 * at when pressing and releasing.
 */
static Vector<float2> anchor_uvs_reduce(const Span<float2> anchor_uvs, const float tolerance_uv)
{
  Vector<float2> reduced;
  if (anchor_uvs.size() <= 2) {
    reduced.extend(anchor_uvs);
    return reduced;
  }

  const int last = int(anchor_uvs.size()) - 1;
  Vector<int> kept;
  float tolerance = std::max(tolerance_uv, FLT_EPSILON);
  for (int attempt = 0; attempt < 16; attempt++) {
    kept.clear();
    kept.append(0);
    anchor_uvs_simplify_rdp(anchor_uvs, 0, last, tolerance, kept);
    kept.append(last);
    if (kept.size() <= CURVE_PATCH_ANCHOR_POINTS_MAX) {
      break;
    }
    tolerance *= 2.0f;
  }
  if (kept.size() > CURVE_PATCH_ANCHOR_POINTS_MAX) {
    /* Tolerance growth did not converge (all samples equidistant from every chord). Keep the
     * endpoints plus an evenly spaced subset so the caller always gets an editable curve. */
    Vector<int> capped;
    for (const int i : IndexRange(CURVE_PATCH_ANCHOR_POINTS_MAX)) {
      capped.append(kept[i * (kept.size() - 1) / (CURVE_PATCH_ANCHOR_POINTS_MAX - 1)]);
    }
    kept = std::move(capped);
  }

  reduced.reserve(kept.size());
  for (const int index : kept) {
    reduced.append(anchor_uvs[index]);
  }
  return reduced;
}

/**
 * Build a one-bezierspline `bke::CurvesGeometry` from the anchor gesture's UV sample buffer.
 * The samples are first reduced by #anchor_uvs_reduce so the result is an editable handful of
 * control points rather than one per dab; handles are AUTO-generated and the radius is the
 * blueprint's unit value (1.0 = full brush size). Stage 7's modal operator inserts and removes
 * points on top of this geometry.
 *
 * The z component of every control point and every handle is 0 (canonical image UV is 2D);
 * any third-axis reference from a float3-aware helper is therefore irrelevant.
 *
 * The curve is open: `BRUSH_STROKE_CURVE_PATCH` does not traverse loop anchors. The roller
 * keeps the no-leading-zero convention `bke::CurvesGeometry` uses for bezier samples; per the
 * spec this same convention applies in 2D (it changes nothing observable for a non-arc-length
 * path, but matching the 3D code avoids surprising mapping side-effects later).
 */
static bke::CurvesGeometry texture_paint_build_initial_curve_from_anchor_uvs(
    const Vector<float2> &anchor_uvs, const float tolerance_uv)
{
  bke::CurvesGeometry initial_curve;
  if (anchor_uvs.is_empty()) {
    return initial_curve;
  }

  const Vector<float2> control_uvs = anchor_uvs_reduce(anchor_uvs, tolerance_uv);

  /* Same init the `PAINT_OT_image_paint` Curve path uses for 3D screen-space paint curves
   * (`paintcurve_geometry_init_bezier`). AUTO handle types evaluate to a sensible preview
   * through `curve.evaluate()` even before any user edit. */
  paintcurve_geometry_init_bezier(initial_curve, int(control_uvs.size()));

  MutableSpan<float3> positions = initial_curve.positions_for_write();
  for (const int i : control_uvs.index_range()) {
    positions[i] = float3(control_uvs[i].x, control_uvs[i].y, 0.0f);
  }

  /* Paint curves carry radius on this codebase's own convention (1.0 = full brush size), not
   * the hair-curve 0.01 default `bke::CurvesGeometry::radius()` would otherwise return --
   * mirrors what `curve_patch_control_curve_from_points()` does for the Sculpt sibling.
   *
   * The attribute is created here unconditionally. `paintcurve_geometry_init_bezier()` does not
   * add it, so a `contains("radius")` guard would skip the fill entirely and leave every point
   * on the hair default -- and the first radius-handle drag would then materialize the attribute
   * at 0.01 for every other point at once. */
  initial_curve.radius_for_write().fill(1.0f);

  /* `paintcurve_geometry_init_bezier()` sets handle TYPES to AUTO but does not materialize
   * handle POSITION attributes; the recompute helpers early-out when those attributes are
   * absent. The two `_for_write()` accessors create them, then `calculate_bezier_auto_handles`
   * computes the AUTO positions from the just-set control points. ALIGHNED handles remain
   * zero until the user edits a handle, which is fine for the anchor stub. */
  initial_curve.handle_positions_left_for_write();
  initial_curve.handle_positions_right_for_write();
  initial_curve.calculate_bezier_auto_handles();
  initial_curve.calculate_bezier_aligned_handles();
  initial_curve.tag_topology_changed();
  initial_curve.tag_positions_changed();
  return initial_curve;
}

/**
 * Records the anchor gesture and, on release, turns it into a live session.
 *
 * The UV path is the whole of it. The gesture's pressure used to be recorded alongside (first and
 * last sample, on `PaintOperation`) but was never read by anything: the 2D session freezes its
 * strength from `BKE_brush_alpha_get()` instead. It is left out rather than carried forward dead
 * -- adding it back is two lines if pressure ever reaches the session.
 */
class CurvePatchAnchorHook : public ImageStrokeMethodHook {
 public:
  bool suppresses_dabs() const override
  {
    return true;
  }

  void on_dab(const float2 &uv, float /*pressure*/) override
  {
    anchor_uvs_.append(uv);
  }

  bool on_stroke_end(bContext &C, const bool is_cancel, const bool stroke_started) override;

 private:
  Vector<float2> anchor_uvs_;
};

/**
 * Simplify tolerance in canonical UV, derived from the live `View2D` zoom so the control points
 * stay roughly the same visual distance apart whatever the zoom level or the image resolution.
 * Zero when there is no usable region, which #anchor_uvs_reduce clamps to `FLT_EPSILON`.
 */
static float anchor_tolerance_uv_get(bContext &C)
{
  const ARegion *region = CTX_wm_region(&C);
  if (region == nullptr) {
    return 0.0f;
  }
  const int mask_width = BLI_rcti_size_x(&region->v2d.mask);
  if (mask_width <= 0) {
    return 0.0f;
  }
  const float uv_per_pixel = BLI_rctf_size_x(&region->v2d.cur) / float(mask_width);
  return uv_per_pixel * CURVE_PATCH_ANCHOR_SIMPLIFY_PIXELS;
}

/**
 * Report through a transient list.
 *
 * There is no `wmOperator *` in scope on this path -- `PaintStroke` does not cache one, and both
 * `paint_exec()` and `paint_cancel()` reach the stroke's end without a pointer to it. The
 * user-visible channel for "Curve Patch failed to start" is the modal operator's poll, which
 * surfaces the same condition by finding no active session.
 */
static void anchor_report(const eReportType type, const char *message)
{
  ReportList reports;
  BKE_reports_init(&reports, 0);
  reports.storelevel = type;
  BKE_report(&reports, type, message);
  BKE_reports_free(&reports);
}

bool CurvePatchAnchorHook::on_stroke_end(bContext &C,
                                         const bool is_cancel,
                                         const bool stroke_started)
{
  if (is_cancel || !stroke_started) {
    /* `stroke_started` rather than the sample count: a press without movement must reach the
     * diagnostic below, which the sparse per-dab sampling would otherwise accept silently. */
    return false;
  }

  /* Roll the anchor's own dabs off the canvas, then drop its non-owned step WITHOUT committing it
   * as a history entry.
   *
   * Restoring first is what makes the live session's replay authoritative: the session opens its
   * owned transaction over a canvas with no patch pixels on it, so every `restore_and_restamp()`
   * -- including the first -- renders exactly the current curve. Skip the restore and the
   * anchor's dabs stay baked in underneath, doubling the ribbon and surviving Esc. */
  UndoStack *ustack = CTX_wm_manager(&C)->runtime->undo_stack;
  if (ustack->step_init) {
    ED_image_undo_restore(ustack->step_init);
  }
  BKE_undosys_step_push_init_abort(ustack);

  /* From here on the step is gone either way, so the caller must never close it. */
  if (anchor_uvs_.size() < 2) {
    anchor_report(RPT_WARNING, "Curve Patch requires at least 2 anchor samples; stroke discarded");
    return true;
  }

  const bke::CurvesGeometry initial_curve = texture_paint_build_initial_curve_from_anchor_uvs(
      anchor_uvs_, anchor_tolerance_uv_get(C));

  const Paint *paint = BKE_paint_get_active_from_context(&C);
  Brush *brush = paint ? BKE_paint_brush(const_cast<Paint *>(paint)) : nullptr;

  ReportList reports;
  BKE_reports_init(&reports, 0);
  reports.storelevel = RPT_ERROR;
  const ImageCurvePatchSession *session = image_curve_patch_session_begin(
      &C, &reports, initial_curve, brush, paint);
  BKE_reports_free(&reports);

  if (session != nullptr) {
    ED_paint_curve_overlay_tag_redraw_all(&C);
  }
  /* Whether or not a session opened, the anchor's canvas state is already back to pre-stroke and
   * its step is aborted -- there is nothing left for the caller to close. */
  return true;
}

}  // namespace

std::unique_ptr<ImageStrokeMethodHook> image_curve_patch_anchor_hook_create()
{
  return std::make_unique<CurvePatchAnchorHook>();
}

}  // namespace blender::ed::sculpt_paint
