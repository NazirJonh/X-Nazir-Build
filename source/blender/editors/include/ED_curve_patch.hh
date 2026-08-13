/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Public view of a running Curve Patch edit session. The full type lives in the sculpt_paint
 * private header; this file only forward-declares it so RNA, transform, and overlay can hold a
 * typed pointer instead of `void *`.
 *
 * Read accessors take the session pointer. Mutation (handle write, restamp, undo push) stays
 * keyed by `Object`: those callers already have the object, and the editor owns the write.
 * Overlay consumes #CurvePatchOverlayData, not the session type.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct bContext;
struct Object;

namespace bke {
class CurvesGeometry;
}

namespace ed::sculpt_paint {
struct CurvePatchSession;
}

/**
 * The live session published on `ob`, or null when no Curve Patch edit is running on it.
 */
const ed::sculpt_paint::CurvePatchSession *ED_curve_patch_session_get(const Object &ob);

/** Control point count of the session's live control curve. Null session yields 0. */
int ED_curve_patch_session_point_num(const ed::sculpt_paint::CurvePatchSession *session);
/** Index of the point the modal editor last acted on, or -1. Already validated against the curve. */
int ED_curve_patch_session_active_point(const ed::sculpt_paint::CurvePatchSession *session);
/** Whether the live control curve closes back on itself. */
bool ED_curve_patch_session_is_cyclic(const ed::sculpt_paint::CurvePatchSession *session);
/** World-space brush radius frozen when the patch started. */
float ED_curve_patch_session_radius(const ed::sculpt_paint::CurvePatchSession *session);
/** Stamp count of the last build; zero in Ribbon mode, which lays out none. */
int ED_curve_patch_session_stamp_num(const ed::sculpt_paint::CurvePatchSession *session);

/**
 * The live control point positions, in object space.
 *
 * The span points INTO the session and is invalidated by the next re-stamp, so a caller must copy
 * anything it intends to keep. Empty for a null handle.
 */
Span<float3> ED_curve_patch_session_positions(const ed::sculpt_paint::CurvePatchSession *session);

/**
 * What the overlay needs to draw a live Curve Patch: the control curves, in session order, and
 * which of them is active. Empty `splines` means there is no session. Overlay iterates this
 * struct; it does not call session accessors.
 *
 * Reuse the same instance across redraws: #ED_curve_patch_overlay_data_get clears and refills
 * `splines`, so the vector's capacity is kept.
 */
struct CurvePatchOverlayData {
  Vector<const bke::CurvesGeometry *> splines;
  int active_index = -1;
};

/** Fill \a r_data from \a session. Null session yields empty `splines` and `active_index` -1. */
void ED_curve_patch_overlay_data_get(const ed::sculpt_paint::CurvePatchSession *session,
                                     CurvePatchOverlayData &r_data);
/** #ED_curve_patch_session_get then #ED_curve_patch_overlay_data_get. Null `ob` is empty. */
void ED_curve_patch_overlay_data_get(const Object *ob, CurvePatchOverlayData &r_data);

/* Mutable point access for the Transform system (`transform_convert_curve_patch.cc`). Every
 * function tolerates "no session"/"no valid active point" by returning false or doing nothing. */

/** Object-space position of the active point's `handle_index` (0 = left handle, 1 = pivot,
 * 2 = right handle). */
bool ED_curve_patch_session_active_point_handle_get(Object &ob, int handle_index, float r_co[3]);
/** Write back one handle of the active point, object space. Does not itself re-stamp -- see
 * #ED_curve_patch_session_restamp, called once per transform step rather than once per handle. */
bool ED_curve_patch_session_active_point_handle_set(Object &ob, int handle_index, const float co[3]);
/** Re-tessellate and re-stamp after one or more handle writes, so a live transform's
 * `recalc_data` sees the change immediately -- mirrors the modal editor's own
 * `curve_patch_restore_and_restamp()`. */
void ED_curve_patch_session_restamp(bContext &C, Object &ob);
/** Record the session's current state as a new step on its own undo stack, so Ctrl+Z inside a
 * live Curve Patch edit can step back over a finished G/R/S transform. */
void ED_curve_patch_session_undo_push(Object &ob);

}  // namespace blender
