/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
struct Brush;
struct Main;
struct Material;
struct MaterialPaintLayerCurvePatch;
struct Object;
struct PointerRNA;
struct PropertyRNA;
struct ReportList;

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
/** Index of the point the modal editor last acted on, or -1. Already validated against the curve.
 */
int ED_curve_patch_session_active_point(const ed::sculpt_paint::CurvePatchSession *session);
/** Whether the live control curve closes back on itself. */
bool ED_curve_patch_session_is_cyclic(const ed::sculpt_paint::CurvePatchSession *session);
/** World-space brush radius frozen when the patch started. */
float ED_curve_patch_session_radius(const ed::sculpt_paint::CurvePatchSession *session);
/** Per-point opacity of the active control point, in [0, 1]. Returns 1.0 when no point is active. */
float ED_curve_patch_session_active_point_opacity(
    const ed::sculpt_paint::CurvePatchSession *session);
/** Write the active control point's opacity. Returns false when no point is active. Does not
 * itself re-stamp: the RNA update callback does, because it is the caller with a context. */
bool ED_curve_patch_session_active_point_opacity_set(Object &ob, float opacity);
/** Opacity falloff of the ACTIVE control point, as a multiple of the curve's width. Returns 1.0
 * when no point is active. */
float ED_curve_patch_session_active_point_opacity_falloff(
    const ed::sculpt_paint::CurvePatchSession *session);
/** Write the active control point's opacity falloff. Does not itself re-stamp: the RNA update
 * callback does, because it is the caller with a context. */
bool ED_curve_patch_session_active_point_opacity_falloff_set(Object &ob, float falloff);
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

/**
 * Fill \a r_data with the curves \a ob's own Curve Patch target draws, in \a ob's OWN local
 * space: the document for the session's owner object, that object's mirror of the document for a
 * secondary target. Empty when \a ob carries no target of a live session.
 *
 * A multi-object session publishes its one session pointer on every stamped object, so the
 * overlay can resolve per-object curves (the object matrices differ) from the same call shape the
 * single-object path uses.
 */
void ED_curve_patch_overlay_data_get_target(const Object *ob, CurvePatchOverlayData &r_data);

/** The object the session's DOCUMENT lives on (null session: null). Editing/projection always uses
 * this object's space, even when a secondary target is the active object. Mutable: the session's
 * owner is an original Object a caller may have to act on (the Transform system resolves its
 * projection through it). */
Object *ED_curve_patch_session_owner(const ed::sculpt_paint::CurvePatchSession *session);

/**
 * Fill \a r_data from the ACTIVE stack row of \a ob's active material (passive preview, no
 * session): the control curves of every Curve Patch item in the row's PaintVector, `active_index`
 * the row's stored active item. Empty when there is no object, its active row is not a Curve Patch
 * row, or the container holds no curve items -- the overlay then draws nothing, the same way a
 * null session reads.
 *
 * A live session has priority: the overlay caller resolves the session first and only falls back
 * to this when none runs, so the interactive curves are never drawn twice.
 */
void ED_curve_patch_overlay_data_get_stack(const Object *ob, CurvePatchOverlayData &r_data);

/* Mutable point access for the Transform system (`transform_convert_curve_patch.cc`). Every
 * function tolerates "no session"/"no valid active point" by returning false or doing nothing. */

/** Object-space position of the active point's `handle_index` (0 = left handle, 1 = pivot,
 * 2 = right handle). */
bool ED_curve_patch_session_active_point_handle_get(Object &ob, int handle_index, float r_co[3]);
/** Write back one handle of the active point, object space. Does not itself re-stamp -- see
 * #ED_curve_patch_session_restamp, called once per transform step rather than once per handle. */
bool ED_curve_patch_session_active_point_handle_set(Object &ob,
                                                    int handle_index,
                                                    const float co[3]);
/** Re-tessellate and re-stamp after one or more handle writes, so a live transform's
 * `recalc_data` sees the change immediately -- mirrors the modal editor's own
 * `curve_patch_restore_and_restamp()`. */
void ED_curve_patch_session_restamp(bContext &C, Object &ob);
/** Record the session's current state as a new step on its own undo stack, so Ctrl+Z inside a
 * live Curve Patch edit can step back over a finished G/R/S transform. */
void ED_curve_patch_session_undo_push(Object &ob);

/**
 * Replace the brush snapshot of a Curve Patch effect with a duplicate of \a source. The single
 * entry for every UI path (a brush dropped on the slot, the slot's ID browser, the assign
 * operator); the context-free RNA `brush` setter is only for scripts. It validates \a source
 * (#BKE_paint_layers_curve_patch_brush_usable) and wraps the replacement with what it needs:
 * - live layer sessions commit first, since the snapshot is the active brush while one runs and
 *   cannot be freed under it;
 * - every curve of the effect takes the new brush's parameters (stamp mode, layout, spacing,
 *   caps, falloff, randomness, Width and Opacity), because a curve freezes them from the brush
 *   when it is made and would otherwise keep the old brush's look. The geometry, visibility and
 *   mirror axes of a curve stay;
 * - the effect's tool is entered again when it was running, now with the new snapshot;
 * - the notifiers and the one undo step ("Assign Curve Patch Brush") are sent here, so callers
 *   push nothing themselves.
 *
 * Refusals are reported to \a reports (may be null). Returns whether the snapshot changed.
 */
bool ED_curve_patch_layer_brush_assign(bContext &C,
                                       Material &material,
                                       MaterialPaintLayerCurvePatch &payload,
                                       const Brush &source,
                                       ReportList *reports);

/**
 * Radial control hook: while a LAYER session runs on the active object, the brush Size ring is the
 * ACTIVE curve's Width and the Strength ring its Opacity, because the effect's snapshot brush has
 * its own Size and Strength frozen (the per-curve radius belongs to the curve). Keyed on the
 * property \a r_prop the keymap asked for ("size"/"strength"); on a match \a r_ptr and \a r_prop
 * are replaced by the session's `curve_width` / `curve_opacity`. Any other property, or no layer
 * session, leaves them untouched. Returns whether the ring was retargeted.
 */
bool ED_curve_patch_radial_control_retarget(bContext &C, PointerRNA &r_ptr, PropertyRNA *&r_prop);

/**
 * The ACTIVE curve's persisted Width, in the layer panel's slider units (1 unit is 5 px of brush
 * size, so 100 is a 500 px brush), read from the row's container item. Zero when no layer session
 * runs, its active patch has no persisted curve yet, or the row was deleted. This is what the
 * brush Size ring is retargeted to while the session runs: the per-curve radius is the curve's
 * own, so the snapshot brush's Size is frozen and the ring must not touch it.
 */
float ED_curve_patch_session_curve_width(const Object &ob);
/** Write the active curve's persisted Width (\a width in the same slider units). Only the row's
 * container item changes; the modal editor's poll syncs it into the live document (its re-stamp),
 * and the RNA update callback re-stamps once for callers that bypass a running modal. Returns
 * false when there was nothing to write. */
bool ED_curve_patch_session_curve_width_set(const Object &ob, float width);
/** The ACTIVE curve's persisted Opacity, in [0, 1]: the multiplier on the stamp strength that
 * stands in for the snapshot brush's own Strength while a layer session runs. Zero when there is
 * nothing to read, like #ED_curve_patch_session_curve_width. */
float ED_curve_patch_session_curve_opacity(const Object &ob);
/** Write the active curve's persisted Opacity. Same contract as
 * #ED_curve_patch_session_curve_width_set. */
bool ED_curve_patch_session_curve_opacity_set(const Object &ob, float opacity);

/**
 * Exchange every live Curve Patch preview in \a bmain with its pre-patch snapshot; calling it twice
 * is the identity. The memfile undo encoder brackets its write with it, so a global undo step pushed
 * mid-session records the surface without the uncommitted preview.
 */
void ED_curve_patch_sessions_preview_swap(Main &bmain);

/**
 * Write every live Curve Patch session's document into the Stack Layers row's PaintVector it was
 * entered from, so a blend file written right after this carries the curves exactly as the user
 * sees them. A live session edits its curves in runtime memory only -- the container is updated on
 * commit and on the mode exit -- so without this a file saved mid-session keeps the point
 * positions of the session start. Called from #CurvePatchPreviewWriteGuard, which every writer of
 * a blend file brackets the write with.
 *
 * The write may run while a modal gesture (a point drag, a G/R/S) is live: the document then holds
 * the in-progress state, which is the one on screen, and it is self-consistent at every gesture
 * tick. When the gesture is cancelled afterwards, the container follows the document again at the
 * next store (commit, mode exit, or the next save), so the cancelled state reaches no file written
 * from then on.
 *
 * No-op for a session that was not entered from a Stack Layers row (it has nothing persisted) and
 * for a row that was deleted while the session ran. The session itself stays live: the write
 * carries the curves but touches neither the row's bake revision nor its bake (see
 * #curve_patch_layer_store_document_for_save), and it has no undo step and no notifier.
 */
void ED_curve_patch_sessions_store_for_save(Main &bmain);

/**
 * Writes every live Curve Patch session's curves into its row's PaintVector and keeps every live
 * preview out of a blend-file written while the guard is alive, the same way
 * #bke::sculpt_layers::MaskEditSuspendGuard keeps a layer mask session out of it: only committed
 * edits are user data, but a live session's curves are. Must span the write itself. The session
 * stays live, so saving does not cancel the edit.
 */
class CurvePatchPreviewWriteGuard {
  Main &bmain_;

 public:
  explicit CurvePatchPreviewWriteGuard(Main &bmain) : bmain_(bmain)
  {
    /* The curves of a live session are user data, its uncommitted surface is not; the two calls
     * are independent (the store writes the row's PaintVector, the swap the stamped surface), so
     * the order between them carries no meaning. */
    ED_curve_patch_sessions_store_for_save(bmain_);
    ED_curve_patch_sessions_preview_swap(bmain_);
  }
  ~CurvePatchPreviewWriteGuard()
  {
    ED_curve_patch_sessions_preview_swap(bmain_);
  }
  CurvePatchPreviewWriteGuard(const CurvePatchPreviewWriteGuard &) = delete;
  CurvePatchPreviewWriteGuard &operator=(const CurvePatchPreviewWriteGuard &) = delete;
};

}  // namespace blender
