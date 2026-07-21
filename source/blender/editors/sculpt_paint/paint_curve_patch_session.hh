/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Runtime state for one live Curve Patch (see Curve Patch Stroke design doc). Owned by
 * `SculptSession::curve_patch_session`, sibling to `StrokeCache`/`filter::Cache`/`expand::Cache`.
 * Created right after the anchor stroke of a `BRUSH_STROKE_CURVE_PATCH` brush finishes; destroyed
 * on commit (Enter) or cancel (Esc). See `paint_curve_patch_session.cc` for the restore-then-
 * re-stamp recompute that uses it.
 *
 * The parameters and the derived geometry live in `BKE_curve_patch.hh` instead, so that both can
 * be driven without a brush, a sculpt session or an operator. What stays here is everything that
 * genuinely needs the editor layer: the DNA texture binding, the document the user edits, and the
 * bookkeeping against the live mesh.
 */

#include <array>
#include <memory>

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_bit_vector.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_texture_types.h"

#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

#include "ED_view3d.hh"

#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_effect.hh"

namespace blender {
struct Brush;
struct Depsgraph;
struct Object;
struct PaintModeSettings;
struct ReportList;
struct Scene;
struct Sculpt;
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint {

/** Per-application bookkeeping against the live mesh. */
struct CurvePatchApplyState {
  /** Number of elements the keys of the effect's snapshot index into, sampled once when the patch
   * starts: `Mesh::verts_num` for `Type::Mesh`, `SubdivCCG::positions.size()` for `Type::Grids`.
   *
   * The modal passes events through whenever the cursor leaves its region, so an unrelated
   * operator can retopologize the object while a patch is live. Every key in the effect's snapshot
   * would then name a different element or none at all, and both restoring and committing would
   * write against the wrong surface -- or past the end of the array. #element_num plus
   * #faces_num / #corners_num is the cheap detection for that. */
  int64_t element_num = 0;

  /**
   * Mesh topology counts sampled with #element_num. -1 means "not a mesh" (Grids / no snapshot).
   * `element_num` alone misses Triangulate, Poke, and remesh that keep `verts_num` and change
   * faces/corners -- ColorEffect already documents that the two counts move independently.
   */
  int64_t faces_num = -1;
  int64_t corners_num = -1;

  /** Set when the check above fails. The patch is then unusable: its snapshot describes a mesh
   * that no longer exists, so it must be abandoned WITHOUT restoring (which would write stale
   * positions) and without pushing anything to the undo stack. */
  bool invalidated = false;

  /** Per-restamp accumulator for blending symmetry passes that land on the same real vertex (a
   * patch straddling a mirror/radial symmetry plane can have both the direct and the mirrored
   * pass claim the same vertex). Keyed like the effect's snapshot; `x` is the running sum of each
   * claiming pass's falloff weight, `y` the running sum of `weight * height`, so every pass's
   * PHASE 2 can recompute the weighted-average target height so far instead of unconditionally
   * overwriting an earlier pass's contribution (see `ReliefEffect::apply_pass()`). Unlike
   * the effect's snapshot, this does NOT persist for the patch's whole life -- cleared at the
   * start of every `curve_patch_restore_and_restamp()`, since blending is only meaningful between
   * passes of the SAME restamp.
   *
   * Also shared across every `CurvePatchItem` of this restamp. TODO(I10): that reuse is the
   * Color-vs-Relief overlap policy; options on #curve_patch_blend_across_passes. */
  Map<int, float2> pass_weight_accum;

  /** PBVH node indices displaced by the PREVIOUS restamp (one bit per `bke::pbvh::Tree` node).
   * `curve_patch_restore_only()` tags exactly these nodes so `bke::pbvh::update_normals()` reverts
   * only their normals -- the footprint that just moved away, which a "current node mask only" tag
   * would miss. `curve_patch_restore_and_restamp()` clears it before each restamp; the relief
   * action ORs each symmetry pass's node mask back in, so it always describes what the last
   * restamp touched. Tracking this precisely is what keeps a restore/re-stamp O(patch footprint)
   * rather than O(whole mesh) on every interactive drag event -- see
   * `paint_curve_patch_session.cc`. */
  BitVector<> last_restamp_nodes;

  /** Union of EVERY restamp's node mask over the patch's whole life (one bit per
   * `bke::pbvh::Tree` node), as opposed to #last_restamp_nodes which describes only the latest
   * one. Sized alongside it but never cleared.
   *
   * This is the mask the commit-time undo step is built from, and the wider set is required, not a
   * safety margin: `ReliefEffect::smooth_relief()` writes to every key of the effect's snapshot,
   * which accumulates across the patch's whole life. A vertex touched by an early restamp and left
   * alone by the final one sits in that map holding a zero displacement, and smoothing averages it
   * with its displaced neighbors into a non-zero one -- so it moves at commit time even though its
   * node is absent from #last_restamp_nodes. Every such key was written by SOME restamp, whose
   * node mask this set contains, so the coverage holds by construction. */
  BitVector<> all_touched_nodes;
};

/** A live Curve Patch editing session. Published on `SculptSession::curve_patch_session`. */
struct CurvePatchSession {
  /** What the user edits: the control curves, the texture binding, the session-local undo stack.
   * Target-independent, so the shared editor can act on it without knowing this session owns it.
   */
  CurvePatchDocument doc;

  CurvePatchApplyState apply;

  /** What this session writes, and how it takes it back: the pre-patch snapshot, both application
   * phases, the restore, the commit-time undo step. Chosen once at session start from the active
   * brush (see #curve_patch_effect_create); never null on a published session, because
   * `curve_patch_begin_editing()` refuses to publish one without it.
   *
   * The target-agnostic half of a re-stamp -- where the patch reaches and how strongly -- lives in
   * `paint_curve_patch_sampler.hh` instead and is shared by every effect. */
  std::unique_ptr<CurvePatchEffect> effect;

  /** Owned copy of the anchor stroke's `ViewContext`, set once in `curve_patch_start_from_anchor`.
   * `StrokeCache::vc` is a non-owning pointer that normally points into the interactive stroke's
   * own (stack-lifetime) `ViewContext`; since Curve Patch takes over `SculptSession::cache` after
   * that stroke's operator has already finished and torn its `ViewContext` down, `StrokeCache::vc`
   * is repointed at this member instead so every `curve_patch_restore_and_restamp()` re-stamp
   * (which reads it via `calc_local_from_screen()`/`cache->vc`) dereferences valid memory for the
   * whole lifetime of the patch. */
  ViewContext view_context = {};

  /** Suppresses repeating the window-cap warning: `curve_patch_restore_and_restamp()` runs on
   * EVERY mouse event, so an unguarded message would flood the info line. */
  bool reported_frame_cap = false;

  /** Forwards to #CurvePatchDocument::has_active_item. */
  bool has_active_item() const
  {
    return this->doc.has_active_item();
  }

  /** Forwards to #CurvePatchDocument::active_item. */
  CurvePatchItem &active_item()
  {
    return this->doc.active_item();
  }
  const CurvePatchItem &active_item() const
  {
    return this->doc.active_item();
  }
};

/**
 * Build a Curve Patch control curve from explicit positions and radii.
 *
 * Exists as one function because the sequence it performs is unforgiving. `paintcurve_geometry_
 * init_bezier()` creates the handle TYPES but not the handle POSITION attributes, and
 * `calculate_bezier_auto_handles()` silently EARLY-OUTS when those attributes are absent -- so
 * omitting any step produces no error, just a bezier collapsed to the origin and a silently empty
 * patch. Three callers would otherwise each have to know it.
 *
 * `radii` must match `positions` in size, and carries this codebase's paint-curve convention where
 * 1.0 means "full brush size" -- NOT `bke::CurvesGeometry::radius()`'s hair-oriented 0.01 default.
 *
 * Lives in the editor layer, not in blenkernel, only because `paintcurve_geometry_init_bezier()`
 * does (`paint_curve_intern.hh`); nothing about the curve itself needs an editor.
 */
bke::CurvesGeometry curve_patch_control_curve_from_points(Span<float3> positions,
                                                          Span<float> radii,
                                                          bool cyclic);

/**
 * Resolve one build's worth of core parameters from a brush.
 *
 * The five values the brush cannot supply are passed in instead: `radius`, `radius_per_size`,
 * `plane_normal` and `stamp_seed` are frozen when the patch starts, and `swap_axis` is owned by
 * the session as well -- the modal's S hotkey and its undo stack both write it without touching
 * the brush, so reading it back from the brush unconditionally would undo them on the next poll.
 *
 * `final_quality` is left false; a caller taking the commit-time build sets it itself.
 */
bke::CurvePatchParams curve_patch_params_from_brush(const Brush &brush,
                                                    float radius,
                                                    float radius_per_size,
                                                    const float3 &plane_normal,
                                                    uint32_t stamp_seed,
                                                    bool swap_axis);

/**
 * Live poll overlay: brush-driven fields from `brush`, frozen per-patch fields from `frozen`.
 *
 * This is the live/frozen contract in one place. A new brush field must be classified here:
 *
 * - **Frozen per patch** (`radius` / `radius_per_size`, `plane_normal`, `stamp_seed`,
 *   `final_quality`): copied from `frozen`. `radius` is rebuilt from `radius_per_size *
 * brush_size` so a Size slider still scales a started patch without unfreezing the captured ratio.
 * - **Session-owned** (`swap_axis`): taken from the brush only when `apply_brush_swap_axis` is
 *   true (the brush itself changed). Otherwise kept from `frozen`, so the Y hotkey and session
 *   undo are not overwritten on the next poll.
 * - **Live in params** (length, falloff, stamps mode, spacing, jitter, angles): come from
 *   #curve_patch_params_from_brush. `operator==` on the result triggers a re-stamp.
 * - **Live, not in params** (strength, Add/Subtract, symmetry, textures, color, falloff curve):
 *   stay on the modal's #CurvePatchLiveInputs. Do not add them here.
 *
 * Writes the same overlay onto every patch; only the frozen fields differ per item.
 */
bke::CurvePatchParams curve_patch_params_live_overlay(const Brush &brush,
                                                      const bke::CurvePatchParams &frozen,
                                                      int brush_size,
                                                      bool apply_brush_swap_axis);

/**
 * The half of starting a session that owes nothing to a stroke: reset the per-session bookkeeping,
 * build the pristine surface snapshot, create the effect of `effect_type` and publish the session
 * on `ob`.
 *
 * `session->doc.patches` must already hold at least one fully built item -- curve and `params`
 * both (the surface snapshot ignores the parameters, but every later consumer does not). Nothing
 * is stamped here and no viewport is touched, which is what lets the headless apply path share it
 * with the two interactive entry points.
 *
 * Frees NOTHING on failure -- returning false leaves `session` unpublished and still owned by the
 * caller, whose cleanup differs: the interactive entry points also own the anchor stroke's
 * `SculptSession::cache` and must free that too.
 */
bool curve_patch_session_publish(Object &ob,
                                 CurvePatchSession &session,
                                 CurvePatchEffectType effect_type,
                                 PaintModeSettings &paint_mode_settings);

/**
 * Reset a binding to "no multi-texture": no variants, no caps. The relief then reads `brush.mtex`
 * directly, which is exactly the SINGLE-mode behavior.
 *
 * Separate from the reset #curve_patch_texture_binding_from_brush already performs because a
 * re-stamp can find no active brush at all, and `MTex`'s deleted copy assignment (see
 * #DNA_DEFINE_CXX_METHODS) rules out the obvious `binding = {}`.
 */
void curve_patch_texture_binding_clear(CurvePatchTextureBinding &r_binding);

/**
 * Resolve which textures one build samples. `radius` is the patch's frozen base radius, used to
 * convert the UI's brush-diameter cap lengths into world units.
 *
 * `r_binding` is fully overwritten, including the SINGLE-mode case, where it comes back empty and
 * the relief reads `brush.mtex` directly.
 */
void curve_patch_texture_binding_from_brush(const Brush &brush,
                                            float radius,
                                            CurvePatchTextureBinding &r_binding);

/**
 * Restore every vertex in the effect's snapshot to its snapshotted position, then re-apply the
 * frozen brush along the current control curve, growing the effect's snapshot on demand for any
 * newly-touched vertex not yet snapshotted. Call after every control-curve mutation (move/add/
 * remove point, radius change, axis toggle) and once more right before commit.
 *
 * Takes its dependencies explicitly rather than through a `bContext` so that a caller with no
 * window manager can drive it -- which is the whole point of the headless apply path. `reports`
 * may be null; the two conditions it carries (a mesh changed underneath the session, a curve too
 * complex for a full surface wrap) are then simply not surfaced.
 */
void curve_patch_restore_and_restamp(const Scene &scene,
                                     const Depsgraph &depsgraph,
                                     Sculpt &sd,
                                     PaintModeSettings &paint_mode_settings,
                                     Object &ob,
                                     CurvePatchSession &session,
                                     ReportList *reports);

/** #curve_patch_restore_and_restamp with everything resolved from the context. The form every
 * interactive caller wants; the explicit one above exists for callers that have no context. */
void curve_patch_restore_and_restamp(bContext &C, Object &ob, CurvePatchSession &session);

/**
 * Record the CURRENT state as a new step on the session's own undo stack (see
 * `CurvePatchDocument::undo_steps` for why this cannot go through Blender's own undo systems).
 * Called after an action completes -- once per action, not once per event, so a drag is a single
 * step. Defined in `paint_curve_patch_edit_undo.cc`; declared here (rather than kept file-static)
 * because `paint_curve_patch_session.cc`'s #ED_curve_patch_session_undo_push also needs it, for
 * the Transform system's G/R/S handle drags. */
void curve_patch_undo_push(CurvePatchSession &patch);

/** Restores every vertex in the effect's snapshot and does *not* re-stamp. Used for Esc-cancel. */
void curve_patch_restore_only(Object &ob, const CurvePatchSession &session);

/**
 * Commit a live patch, as an Enter-commit would, because the `SculptSession` holding it is about
 * to be destroyed. Returns true when the patch was actually written; no-op returning false when no
 * patch is live. Frees the session and the `SculptSession::cache` it took over.
 *
 * Call from a mode-exit path that still has a `bContext` and has not torn anything down yet -- the
 * final re-stamp needs an evaluated depsgraph, and the undo step this pushes is left parked for
 * the calling operator's `OPTYPE_UNDO` exactly as `curve_patch_finish_commit()` requires.
 *
 * The modal that owns the patch (`SCULPT_OT_curve_patch_edit`) is deliberately left running: it
 * sees the freed session on its next event and releases its own operator state through the
 * liveness guard at the top of `curve_patch_edit_modal()`.
 */
bool curve_patch_commit_on_session_end(bContext &C, Object &ob);

/**
 * Discard a live patch, as an Esc-cancel would, because the `SculptSession` holding it is about to
 * be destroyed. No-op when no patch is live.
 *
 * The last-resort counterpart to #curve_patch_commit_on_session_end, for the teardown paths that
 * have no `bContext` to commit through. Deliberately still called on the paths that DO commit
 * first, where it is a no-op -- it is what guarantees the session can never outlive the sculpt
 * session.
 *
 * `SculptSession::curve_patch_session` is owned by `SCULPT_OT_curve_patch_edit` and freed when
 * that modal commits or cancels, but leaving sculpt mode -- and `BKE_object_free` -- destroy the
 * sculpt session without consulting the modal. #SculptSession's destructor cannot `MEM_delete` it:
 * `CurvePatchSession` is an editor type that blenkernel only forward-declares. Publish therefore
 * registers this function on `SculptSession::free_curve_patch_session`, and
 * #BKE_sculptsession_free invokes it whenever the pointer is still live.
 *
 * Cancel rather than commit is deliberate. Committing needs a `bContext` to re-stamp and push an
 * undo step, and the session is also freed from paths that have none -- object deletion
 * (#BKE_object_free) among them -- so committing here would make the outcome depend on HOW the
 * session ended. The modal itself is left to notice the freed session on its next event and tear
 * its own operator state down (see the liveness guard in `curve_patch_edit_modal()`).
 */
void curve_patch_discard_on_session_end(Object &ob);

/**
 * Free `SculptSession::cache` and the Curve Patch session itself, and clear both pointers plus
 * the teardown callback (#SculptSession::free_curve_patch_session). The shared tail of every path
 * that ends a live patch -- the modal's own commit/cancel, #curve_patch_commit_on_session_end,
 * #curve_patch_discard_on_session_end -- once that path's own restore/re-stamp/commit has already
 * run. Requires a published session (`ob.runtime->sculpt_session->curve_patch_session !=
 * nullptr`); callers already checked that to reach this point.
 */
void curve_patch_session_free(Object &ob);

/**
 * Finish a committed Curve Patch edit: close the patch's position undo step, and -- if the brush's
 * `curve_patch.face_set` flag is set and the relief actually raised anything -- assign a fresh
 * face set to the raised faces in an undo step of its own.
 *
 * Call ONCE, from the commit branch of `SCULPT_OT_curve_patch_edit`, INSTEAD of a bare
 * `undo::push_end()`, and after the final-quality re-stamp so the threshold is measured against
 * the smoothed profile the user will actually keep. Never called on cancel.
 *
 * Closing the undo step belongs here rather than in the caller because HOW it must be closed
 * depends on whether a face set follows: an undo step carries exactly one `undo::Type`, so the
 * face set needs a second step, and opening one would destroy the position step unless that step
 * was force-pushed first (see the implementation). When no face set follows, the position step
 * must be left parked instead, so that the operator's own `OPTYPE_UNDO` push is the one that files
 * it and the edit costs exactly one Ctrl+Z. That is only knowable after the raised faces have been
 * computed, which is why the implementation computes them before closing the step.
 *
 * "Raised" is measured as displacement from the effect's snapshot, thresholded at a fraction of
 * this patch's own maximum displacement -- relative rather than absolute, because displacement is
 * in scene units and would otherwise behave differently on differently scaled objects.
 */
void curve_patch_finish_commit(const Scene &scene,
                               const Depsgraph &depsgraph,
                               Object &ob,
                               const CurvePatchSession &session);

/** #curve_patch_finish_commit plus the finished-stroke redraw an interactive commit needs. The
 * headless form above deliberately issues none: a caller with no window manager has nothing to
 * repaint, and tags the ID itself instead. */
void curve_patch_finish_commit(bContext &C, Object &ob, const CurvePatchSession &session);

/**
 * Plane the patch is projected onto, fitted to the control curve itself.
 *
 * `fallback` is returned (normalized) when the curve spans no plane -- a straight segment, which
 * every anchor stroke starts as. Pass whatever the caller measured against the real surface (the
 * stroke's `StrokeCache::sculpt_normal`, or a roll's frozen projection normal); a constant would
 * orient those patches arbitrarily.
 */
float3 curve_patch_plane_normal_from_curve(const bke::CurvesGeometry &curve,
                                           const float3 &fallback);

/**
 * Publish a Curve Patch session right after a `BRUSH_STROKE_CURVE_PATCH` anchor stroke finishes.
 * Takes over ownership of the just-finished stroke's `SculptSession::cache`; the caller must not
 * tear that down itself when this is invoked.
 *
 * Returns true when a session was published on the object. The caller is responsible for launching
 * `SCULPT_OT_curve_patch_edit` -- see #curve_patch_begin_editing in `paint_curve_patch_session.cc`
 * for why starting a modal from here would invert the layering.
 *
 * It does NOT take over the undo transaction `stroke_undo_begin()` opened for that stroke. This
 * function restores the mesh to its pre-stroke state before anything else touches it, so by the
 * time it returns that transaction describes no net change, and the caller discards it
 * (`SculptPaintStroke::done()`, `mesh/sculpt.cc`). The editor records its own single undo step
 * when the patch is committed. Leaving a transaction open for the modal's whole lifetime is what
 * let any unrelated undo push in the application adopt or free it. See `paint_curve_patch_edit.cc`
 * (Stage 04) for the modal editor implementation.
 *
 * When the brush already owns a paint curve with at least two points on its active spline (as left
 * behind by Stroke Method: Curve), that curve is adopted as the control curve instead of seeding a
 * fresh two-point segment from the anchor drag.
 */
bool curve_patch_start_from_anchor(
    const Depsgraph &depsgraph, Object &ob, Sculpt &sd, const Brush &brush, const ViewContext &vc);

/**
 * Publish a Curve Patch session from a finished #BRUSH_STROKE_ROLL stroke. Builds the control
 * curve from the stroke's resampled contour (`control_positions`, object space, with a per-point
 * `control_radii`), after undoing the live roll relief back to a pristine baseline. From there the
 * handoff is identical to #curve_patch_start_from_anchor, including the returned "launch the modal
 * yourself" contract: it takes over ownership of `SculptSession::cache` but NOT the stroke's undo
 * transaction, which the caller discards once the modal has started; and on any early bail
 * (Dynamic Topology, degenerate input) it frees `ss.cache` itself and returns false.
 * `control_positions.size()` must equal `control_radii.size()`. `plane_normal` is the roll's
 * frozen projection normal, used as the patch's projection plane.
 */
bool roll_start_curve_patch_from_stroke(const Depsgraph &depsgraph,
                                        Object &ob,
                                        Sculpt &sd,
                                        const Brush &brush,
                                        const ViewContext &vc,
                                        Span<float3> control_positions,
                                        Span<float> control_radii,
                                        const float3 &plane_normal);

}  // namespace blender::ed::sculpt_paint
