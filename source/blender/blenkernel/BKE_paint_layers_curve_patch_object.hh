/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Internal BKE interface of the Curve Patch object-source refresh. These are the pieces the
 * editors drive directly (the refresh's write-back echo, the drag gate and the retry bookkeeping),
 * kept out of the public `BKE_paint_layers.hh` so the layered-material API does not grow with the
 * refresh's private protocol.
 */

#pragma once

#include <cstdint>

namespace blender {

struct Depsgraph;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerCurvePatch;
struct Object;
struct PaintVector;

/**
 * The structural hash of one curve object the refresh compares: the object's identity, its world
 * transform, and the (evaluated, when the caller passes an evaluated object) curves' shape.
 */
uint64_t BKE_paint_layers_curve_patch_object_hash(const Object &curve_object);

/**
 * The hash of everything \a payload reads from external curve objects: per curve with an object,
 * its index, its spline and #BKE_paint_layers_curve_patch_object_hash of the object (the evaluated
 * copy when \a depsgraph is given, the original otherwise). Also what the editors write-back
 * registers as the expected next hash (see
 * #BKE_paint_layers_curve_patch_object_expected_hash_set), so a session's own write to the objects
 * does not echo back as a foreign change.
 */
uint64_t BKE_paint_layers_curve_patch_row_hash(const MaterialPaintLayerCurvePatch &payload,
                                               const Depsgraph *depsgraph);

/**
 * Re-read the external curves of every Curve Patch row of \a ma that has any: when what the row
 * would read (through the evaluated objects) differs from what it read at its last refresh, the
 * row's bake is invalidated and the editors hook runs, so a live session re-stamps from the new
 * curves. Mirrors the Text Decal refresh, which rasterizes from the evaluated Text object the same
 * way.
 */
void BKE_paint_layers_curve_patch_object_refresh(Main &bmain,
                                                 Material &ma,
                                                 const Depsgraph *depsgraph);

/**
 * Record \a hash as the change \a row's write-back is ABOUT to make to its curve object: the next
 * refresh that observes it treats it as the session's own write (the session already stamped the
 * new shape) instead of re-publishing and re-stamping a second time. One slot per row; the next
 * refresh consumes it whatever it compares against, so a hash that never comes back (an edit that
 * did not land) cannot suppress a later real change.
 */
void BKE_paint_layers_curve_patch_object_expected_hash_set(Material &ma,
                                                           const MaterialPaintLayer &row,
                                                           uint64_t hash);

/**
 * Drop any expected hash the row registered: a curve re-pointed at another object invalidates the
 * echo the old object's write-back left behind, so a later real change cannot be swallowed.
 */
void BKE_paint_layers_curve_patch_object_expected_hash_clear(Material &ma,
                                                             const MaterialPaintLayer &row);

/**
 * While \a drag_in_flight is set, the refresh holds every row back that does not own the write:
 * a live session's drag lands its shape in the curve objects on every step, and re-stamping the
 * rows that only read them per step would make the drag pay for effects it does not edit. The
 * rows' stored hashes are left alone, so the shape the drag's release commits is still a change
 * when the next refresh runs -- that refresh rebuilds them. Only the writing row keeps its echo
 * suppression (the expected hash it registers per step). The editors set this around a point
 * drag of a live session and clear it when the drag ends (or the session dies).
 */
void BKE_paint_layers_curve_patch_object_drag_in_flight_set(bool drag_in_flight);

/**
 * Teardown counterpart of #BKE_paint_layers_curve_patch_object_drag_in_flight_set: drops every
 * unmatched begin at once, so a session that dies under a stuck drag cannot leave the refresh
 * holding rows back.
 */
void BKE_paint_layers_curve_patch_object_drag_in_flight_reset();

/**
 * Mark (or unmark) \a row as "a change was detected but the re-stamp has not landed yet". A
 * pending row makes every following refresh run the editors hook again (the retry), until the
 * stamp succeeds or the hook reports a permanent refusal -- so a re-stamp blocked by a busy
 * stamp host (another session's live stroke) is not lost, but retried at the next safe point.
 * Runtime-only state, keyed by the row's marker like the refresh hashes; a removed row drops
 * its entry with its hashes. The editors set it from the hook's outcome
 * (#ED_curve_patch_layer_row_restamp); the refresh itself marks a changed row before calling the
 * hook, so a change survives even a missing hook.
 */
void BKE_paint_layers_curve_patch_object_restamp_pending_set(Material &ma,
                                                             const MaterialPaintLayer &row,
                                                             bool pending);

/**
 * The end of a Curve Patch session that occupied a stamp host is the safe point a blocked
 * re-stamp waited for: tag every layered material of \a bmain that still holds pending rows, so
 * the next scene update runs the refresh and the retry lands the stamp. Cheap when nothing is
 * pending (one cached read per material). Called from the session teardown paths, after the
 * session's own sculpt state is released.
 */
void BKE_paint_layers_curve_patch_object_restamp_pending_session_end_notify(Main &bmain);

/**
 * Whether \a object can be the external curve of a Curve Patch curve: an object of type #OB_CURVES
 * (its #Curves data is what the curve reads) whose data is editable. Null is usable -- it means
 * "no external curve", which gives the curve back to its own geometry.
 */
bool BKE_paint_layers_curve_patch_object_usable(const Object *object);

/** The material and row that own a Curve Patch container, as found by
 * #BKE_paint_layers_curve_patch_row_find_by_vector. */
struct CurvePatchVectorOwner {
  Material *material = nullptr;
  MaterialPaintLayer *row = nullptr;
};

/**
 * Find the Curve Patch row whose payload uses \a pv as its container. A container belongs to at
 * most one row; the walk is the one place that answers the question, shared by the BKE setter and
 * the editors. Null members when no row of \a bmain names \a pv.
 */
CurvePatchVectorOwner BKE_paint_layers_curve_patch_row_find_by_vector(Main &bmain,
                                                                      const PaintVector &pv);

/** The outcome of pointing a Curve Patch curve at an external Curves object. */
enum class CurvePatchObjectSetResult {
  /** The item now reads \a new_object's spline. */
  Ok,
  /** The item already read that object and spline; nothing was written. */
  NoChange,
  /** \a item_index does not name a Curve Patch curve of \a pv. */
  NotCurvePatch,
  /** \a pv is not editable (a library or override container). */
  NotEditable,
  /** \a new_object is set and is not an editable Curves object. */
  NotUsable,
  /** The owning row is in 2D Space; only 3D Space can read object-space curves. */
  WrongSpace,
  /** Another curve of the same effect already reads that object and spline. */
  InUse,
  /** \a new_object is set but \a pv is not attached to any Curve Patch row. */
  NoOwner,
};

/**
 * The single user-facing text of a refused object assignment. Null for #Ok and #NoChange (nothing
 * to report) and for #NotCurvePatch (a caller addressing a non-curve item is programming error,
 * not a user-facing refusal). Every UI that reports a #CurvePatchObjectSetResult takes its text
 * from here, so the wording cannot drift between the RNA setters and any other entry point.
 */
const char *BKE_paint_layers_curve_patch_object_set_result_message(
    CurvePatchObjectSetResult result);

/**
 * Check whether #BKE_paint_layers_curve_patch_item_object_set would accept the assignment,
 * without changing anything. RNA uses it to preserve the old curve's shape before clearing the
 * object, and to translate the refusal into a report.
 */
CurvePatchObjectSetResult BKE_paint_layers_curve_patch_item_object_validate(
    Main &bmain, PaintVector &pv, int item_index, Object *new_object, int spline);

/**
 * Point the curve \a item_index of the container \a pv at \a new_object's spline \a spline as its
 * external control curve, or at no object (null): the curve then reads its own geometry again. The
 * material's depsgraph relations are rebuilt (the reference set changes) and the owning row is
 * tagged edited, so the next scene update runs the refresh, which re-reads the curve.
 *
 * The object mode reads its curves in the row's 3D Space only, so a row in 2D Space (UV) refuses an
 * object -- switch the Space first.
 *
 * A refusal leaves the item unchanged; see #CurvePatchObjectSetResult for the reasons.
 */
CurvePatchObjectSetResult BKE_paint_layers_curve_patch_item_object_set(
    Main &bmain, PaintVector &pv, int item_index, Object *new_object, int spline);

/**
 * Whether a curve of \a pv other than \a skip_item already reads spline \a spline of \a object.
 * One spline drives one curve of an effect: two curves of the same effect on the same spline would
 * write to each other's shape, so the setter refuses it. The check is per effect (per container):
 * different effects may read the same object and spline, and then all of them follow it.
 */
bool BKE_paint_layers_curve_patch_item_object_in_use(const PaintVector &pv,
                                                     int skip_item,
                                                     const Object *object,
                                                     int spline);

/**
 * The ONE test of "this Curve Patch effect reads a curve object": at least one of its curves
 * names an external object, and the effect works in 3D Space (the only one a curves object can
 * carry). A curve without an object reads its own geometry everywhere (the publish, the
 * write-back, the overlays), so every check of the mode goes through this predicate.
 */
bool BKE_paint_layers_curve_patch_reads_object(const MaterialPaintLayerCurvePatch *payload);

/** #BKE_paint_layers_curve_patch_reads_object for the payload of \a row. */
bool BKE_paint_layers_curve_patch_reads_object(const MaterialPaintLayer &row);

/**
 * Register the editors hook the source refresh calls when an external curve object of a Curve
 * Patch row changed: a live tool session of the row re-reads the curves and re-stamps the row's
 * maps through it (\a depsgraph resolves the evaluated objects). Passing null unregisters. The
 * editors register the hook once at startup (see
 * `ED_curve_patch_layer_object_source_hook_register`).
 */
void BKE_paint_layers_curve_patch_object_changed_cb_set(
    void (*cb)(Main &bmain,
               Material &ma,
               MaterialPaintLayer &row,
               const Depsgraph *depsgraph,
               void *user_data),
    void *user_data);

/** #BKE_paint_layers_curve_patch_object_refresh for every layered material of \a bmain. Main
 * thread only (render workers reach the scene update too; their next update picks the work up). */
void BKE_paint_layers_curve_patch_object_refresh_tagged(Main &bmain, const Depsgraph &depsgraph);

/**
 * Forget the cached "this material has no object-sourced Curve Patch row" answer the refresh
 * caches: a copy, an import or an undo can bring one into a material that answered no. The
 * per-row hashes are not touched (a re-read row observes fresh, the way an add does).
 */
void BKE_paint_layers_curve_patch_object_invalidate(Material &ma);

}  // namespace blender
