/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The bodies behind the Curve Patch context menu and its editing hotkeys, written once against
 * #CurvePatchHost.
 *
 * The operators stay per-mode (`SCULPT_OT_curve_patch_*` and `PAINT_OT_image_curve_patch_*`):
 * each resolves its own session and builds its own host, then calls the body here. That much
 * duplication is what a registered operator costs; the ACTION is what must not be duplicated,
 * because two copies of it drift -- which is what happened between the two modes before this file
 * existed.
 *
 * Every body ends in #CurvePatchHost::after_curve_change on success and touches nothing on
 * failure, so a refused action leaves no re-stamp and no undo step behind.
 */

#include "ED_curves.hh"

namespace blender {
struct bContext;
}

namespace blender::ed::sculpt_paint {

class CurvePatchHost;

/**
 * Remove `point_index`.
 *
 * Refuses (reporting through #CurvePatchHost::reports) at the two-point floor: two points is the
 * minimum whether the curve is open or closed. Refusing rather than auto-cancelling the session is
 * deliberate -- the user cancels a patch explicitly with Esc, not by pressing X once too often.
 *
 * Leaves nothing active on success: the index the caller passed names a different point after the
 * removal, so keeping it would silently move the selection.
 */
bool curve_patch_action_delete_point(bContext &C, CurvePatchHost &host, int point_index);

/** Set both handles of `point_index` to `dst_type`, resolving the same way the paint curve does
 * (Auto/Vector promote to Align rather than being overwritten). */
bool curve_patch_action_set_handle_type(bContext &C,
                                        CurvePatchHost &host,
                                        int point_index,
                                        ed::curves::SetHandleType dst_type);

/** Rotate `point_index`'s handle type: Free -> Auto -> Vector -> Align -> Free, the order
 * #paintcurve_cycle_point_handle_type and Curves Pen use. */
bool curve_patch_action_cycle_handle_type(bContext &C, CurvePatchHost &host, int point_index);

/** Close or re-open the control curve. Returns false only when there is no usable curve. */
bool curve_patch_action_toggle_cyclic(bContext &C, CurvePatchHost &host);

/**
 * Reverse the control curve, like Curve Edit Mode's `CURVE_OT_switch_direction`.
 *
 * Reversing the point order swaps which end is `s == 0` and which is `s == total_length`, so it is
 * what actually flips the along-length texture (Start/End caps swap, Default/Repeat tiling runs
 * backward) instead of merely negating a normal vector -- which a Mesh patch's per-restamp
 * shrinkwrap would overwrite with the real surface direction on the next re-stamp anyway.
 */
bool curve_patch_action_switch_direction(bContext &C, CurvePatchHost &host);

/**
 * Roll a new random layout for a Stamps-mode patch. Returns false in Ribbon mode, which has no
 * randomization for a seed to drive.
 */
bool curve_patch_action_reseed_stamps(bContext &C, CurvePatchHost &host);

}  // namespace blender::ed::sculpt_paint
