/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BLI_index_mask.hh"
#include "BLI_rand.hh"
#include "BLI_span.hh"

#include "BKE_curves.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "ED_curves.hh"

#include "paint_curve_intern.hh"
#include "paint_curve_patch_actions.hh"
#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_host.hh"

namespace blender::ed::sculpt_paint {

/** True when `point_index` names a point of a curve that can be edited at all. */
static bool action_point_is_valid(const bke::CurvesGeometry &geom, const int point_index)
{
  return point_index >= 0 && paintcurve_geometry_is_valid(geom) && point_index < geom.points_num();
}

bool curve_patch_action_delete_point(bContext &C, CurvePatchHost &host, const int point_index)
{
  bke::CurvesGeometry &geom = host.curve();
  if (!action_point_is_valid(geom, point_index)) {
    return false;
  }
  /* Two points is the floor whether the curve is open or closed -- a 2-point cyclic Bezier is a
   * perfectly good loop (see #curve_patch_action_toggle_cyclic). */
  if (geom.points_num() - 1 < 2) {
    BKE_report(host.reports(),
               RPT_WARNING,
               "Curve Patch needs at least 2 points -- press Esc to cancel instead");
    return false;
  }

  IndexMaskMemory memory;
  const IndexMask delete_mask = IndexMask::from_indices<int>(Span<int>(&point_index, 1), memory);
  paintcurve_geometry_remove_points(geom, delete_mask);
  /* Through the host, not by writing `document().active_point`: a target whose overlay colors the
   * picked point from the curve's own selection bits has to clear those too, or the highlight
   * survives on whatever point inherited the removed one's index. */
  host.active_point_set(-1, 0);

  host.after_curve_change(C);
  return true;
}

bool curve_patch_action_set_handle_type(bContext &C,
                                        CurvePatchHost &host,
                                        const int point_index,
                                        const ed::curves::SetHandleType dst_type)
{
  bke::CurvesGeometry &geom = host.curve();
  if (!action_point_is_valid(geom, point_index)) {
    return false;
  }
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  types_left[point_index] = paintcurve_resolve_handle_type(types_left[point_index], dst_type);
  types_right[point_index] = paintcurve_resolve_handle_type(types_right[point_index], dst_type);
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();

  host.after_curve_change(C);
  return true;
}

bool curve_patch_action_cycle_handle_type(bContext &C, CurvePatchHost &host, const int point_index)
{
  bke::CurvesGeometry &geom = host.curve();
  if (!action_point_is_valid(geom, point_index)) {
    return false;
  }
  /* #paintcurve_cycle_point_handle_type takes a #PaintCurve ID, which a runtime patch curve does
   * not have, so the one-line rotation is repeated rather than the geometry being wrapped in a
   * throwaway ID. */
  const int8_t next = int8_t((geom.handle_types_right()[point_index] + 1) % 4);
  geom.handle_types_left_for_write()[point_index] = next;
  geom.handle_types_right_for_write()[point_index] = next;
  /* The same tail the original does. A computed type (Auto/Vector) only means anything once the
   * handle POSITIONS have been recomputed from it, and without a tag the evaluated cache the
   * re-stamp reads is still the one built from the old handles -- so the action would recolor the
   * handle and change nothing else until some later edit happened to tag the curve. */
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();

  host.after_curve_change(C);
  return true;
}

bool curve_patch_action_toggle_cyclic(bContext &C, CurvePatchHost &host)
{
  /* Two points are enough, as everywhere else in Blender: a cyclic Bezier of N points has N
   * segments (#bke::curves::segments_num), and the two a 2-point loop produces are distinct curves
   * -- one runs through point 0's right handle into point 1's left, the other through point 1's
   * right back into point 0's left -- so they bow to opposite sides and enclose a real shape. Only
   * collinear handles degenerate that, which is equally true of three collinear points and is the
   * user's business, not a reason to refuse. */
  if (!paintcurve_geometry_toggle_cyclic(host.curve(), /*curve_index=*/0)) {
    return false;
  }
  host.status_refresh(C);
  host.after_curve_change(C);
  return true;
}

bool curve_patch_action_switch_direction(bContext &C, CurvePatchHost &host)
{
  bke::CurvesGeometry &geom = host.curve();
  if (!paintcurve_geometry_is_valid(geom) || geom.points_num() == 0) {
    return false;
  }
  /* A patch's control curve is always a single spline (built via
   * `paintcurve_geometry_init_bezier()`), so the curve to reverse is always index 0.
   *
   * `CurvesGeometry::reverse_curves()` is the generic, already-shared implementation -- it
   * reverses point order plus every point-domain attribute (radius, the custom
   * `paintcurve_surface_normal`, handle types/positions with the correct left/right swap so the
   * curve's shape is unchanged) in one pass, and tags topology changed itself. */
  const int curve_index = 0;
  IndexMaskMemory memory;
  const IndexMask reverse_mask = IndexMask::from_indices<int>(Span<int>(&curve_index, 1), memory);
  geom.reverse_curves(reverse_mask);

  /* The active point followed its old index; after a reversal that index names a different point.
   * Re-set through the host rather than by assigning `document().active_point`, so a target that
   * mirrors the active point into the curve's selection bits moves the highlight with it -- the
   * bits themselves were reversed along with every other point attribute, but they name the
   * mirrored point, not the one the user picked. */
  const CurvePatchDocument &document = host.document();
  if (document.active_point >= 0) {
    host.active_point_set(geom.points_num() - 1 - document.active_point, 0x02);
  }

  host.after_curve_change(C);
  return true;
}

bool curve_patch_action_reseed_stamps(bContext &C, CurvePatchHost &host)
{
  CurvePatchDocument &document = host.document();
  if (!document.has_active_item()) {
    return false;
  }
  if (document.active_item().params.stamp_mode != bke::CurvePatchStampMode::Stamps) {
    return false;
  }
  /* Alongside `curve_patch_begin_editing()` the only place a stateful RNG is touched: every
   * per-stamp offset downstream is a pure hash of this seed, so re-rolling it here is what makes
   * the whole layout change while every other input stays put. */
  document.active_item().params.stamp_seed =
      RandomNumberGenerator::from_random_seed().get_uint32();

  host.after_curve_change(C);
  return true;
}

}  // namespace blender::ed::sculpt_paint
