/* SPDX-FileCopyrightText: 2026 Blender Authors
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
 * The parameters and the derived geometry live in `BKE_curve_patch.hh` instead, so that both can be
 * driven without a brush, a sculpt session or an operator. What stays here is everything that
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

#include "paint_curve_patch_effect.hh"

struct Brush;
struct Depsgraph;
struct Object;
struct PaintModeSettings;
struct ReportList;
struct Scene;
struct Sculpt;
struct bContext;

namespace blender::ed::sculpt_paint {

/* Curve Patch core geometry and parameters live in blenkernel. These aliases keep call sites in
 * this module unqualified; the free `curve_patch_*` functions are not covered and carry an explicit
 * `bke::` at their call sites. */
using bke::CURVE_PATCH_MAX_FRAMES;
using bke::CurvePatchEndFalloff;
using bke::CurvePatchFrame;
using bke::CurvePatchFrameRange;
using bke::CurvePatchFramesParams;
using bke::CurvePatchFrameSet;
using bke::CurvePatchGeometry;
using bke::CurvePatchLengthMode;
using bke::CurvePatchParams;
using bke::CurvePatchRibbonLut;
using bke::CurvePatchSpline;
using bke::CurvePatchStamp;
using bke::CurvePatchStampMode;
using bke::CurvePatchStampProjection;
using bke::CurvePatchSurfaceSnapshot;
using bke::CurvePatchTextureZone;
using bke::CurvePatchTextureZoneSample;

/** One patch's contribution to an undo snapshot. */
struct CurvePatchEditStepItem {
  bke::CurvesGeometry curve;
  bool swap_axis = false;
  /* Snapshotted for the same reason as `swap_axis`: the Reseed action changes the visible relief
   * without touching `curve`, so without this Ctrl+Z could not walk back over a reseed. */
  uint32_t stamp_seed = 0;
};

/** One snapshot of the session-local undo stack: everything the user edits inside a live Curve
 * Patch that the relief is derived from. The control curve carries positions, handles, handle
 * types, radii and `cyclic` internally, so nothing else about it needs storing.
 *
 * Every patch is snapshotted, not just the active one: `swap_axis` and `stamp_seed` are per-patch
 * (they are `CurvePatchParams` fields), and an undo that restored one curve while leaving its
 * neighbors at their current state would be unpredictable the moment curves can be switched. */
struct CurvePatchEditStep {
  Vector<CurvePatchEditStepItem> items;
  int active_patch = 0;
};

/** Resolved texture slots for one build. Stays in the editor layer because `MTex` is DNA and the
 * core must not depend on it. */
struct CurvePatchTextureBinding {
  /** Resolved texture variants for this restamp: copies of `Brush::mtex` with only `tex` swapped, so
   * every mapping setting (Size / Offset / Angle / Swap Axis) stays shared. Rebuilt on every restamp
   * because the texture source toggles are live-synced like `CurvePatchParams::stamp_mode`. Empty in
   * SINGLE mode, where the relief reads `brush.mtex` directly.
   *
   * The `tex` inside a variant is a raw pointer, not a registered ID reference. Safe within
   * Blender's event model: this array is rebuilt at the top of every restamp from the brush's own
   * (registered) fields, and a restamp is not interrupted, so there is no window in which a deleted
   * texture could be read from here.
   *
   * #Array rather than #Vector: `MTex` carries #DNA_DEFINE_CXX_METHODS, which DELETES its copy and
   * move constructors so a DNA struct cannot be duplicated except through the explicit
   * shallow-copy path. `Vector` relocates its elements when it grows, which needs a move
   * constructor; `Array` only ever default-constructs in place, so it is the container this type
   * can actually live in. Sized once per restamp and filled via `dna::shallow_copy()`. */
  Array<MTex> stamp_texture_variants;
  /** Cumulative weight table over `stamp_texture_variants`, one entry per variant, non-decreasing.
   * Empty unless STAMPS + LIST is active with at least one positive weight. */
  Vector<float> stamp_texture_weights_cdf;
  /** Ribbon CAPS variants indexed by #CurvePatchTextureZone. An entry with a null `tex` marks a zone
   * the user left empty; the relief returns no displacement there. Left default-constructed rather
   * than `= {}`-initialized: `MTex`'s deleted copy assignment (see #DNA_DEFINE_CXX_METHODS) makes
   * `std::array::fill()` and any copy-based reset ill-formed, so entries are reset element-wise
   * through `dna::shallow_zero_initialize()` instead. */
  std::array<MTex, 3> ribbon_zone_variants;
  /** Whether the Ribbon texture source is MULTI this restamp -- NOT "the caps are being drawn". It
   * is set purely from `BrushCurvePatchSettings::ribbon_texture_source`, deliberately without also
   * testing
   * `stamp_mode`, so it is true in Stamps mode too even though Stamps has no caps. Adding that test
   * would create a second place obliged to stay in sync with the relief's branch selector, which is
   * exactly the drift `CurvePatchGeometry::stamp_search_reach` was burned by. Instead the invariant
   * is one-way: ONLY the Ribbon branch of `branch_relief()` reads this field (and
   * `ribbon_zone_variants` / `world_cap_*` with it), and that branch runs only when `stamp_mode` is
   * not STAMPS.
   *
   * The two cap lengths are already resolved from brush diameters into world units. */
  bool caps_enabled = false;
  float world_cap_start = 0.0f;
  float world_cap_end = 0.0f;
};

/** The document the user edits: the point last interacted with and the session-local undo stack.
 * The control curves themselves live one per #CurvePatchItem. */
struct CurvePatchEditState {
  /** Index into `patches[active_patch].control_curve` of the point the user last interacted with,
   * or -1 for none. The patch it belongs to is `CurvePatchSession::active_patch`: the pair is what
   * identifies a point, because two patches index their points independently.
   *
   * Owned conceptually by the live-edit modal (`SCULPT_OT_curve_patch_edit`), but stored here rather
   * than in its `op->customdata` so the small operators the modal's context menu invokes -- which
   * cannot reach a running modal's customdata -- can act on the clicked point. Because this session
   * outlives the modal, the index is reset to -1 whenever an edit session ends or a new control
   * curve is built; consumers must still validate it against `control_curve.points_num()`. */
  int active_point = -1;

  /** Session-local undo stack, owned by the live-edit modal (`SCULPT_OT_curve_patch_edit`).
   *
   * Blender's own undo systems cannot cover this: a sculpt undo step stores mesh attributes only
   * (see #undo::Type) and has no slot for the control curve, so an official step would restore
   * vertex positions and leave the curve untouched; and the paint-curve undo system refuses Sculpt
   * Mode outright (`paintcurve_undosys_poll`) and wants a real `PaintCurve` ID, which this runtime
   * curve is not. A modal that owns runtime state no undo type describes keeps its own stack --
   * exactly what the knife tool does with `KnifeUndoFrame` / `kcd->undostack`
   * (`editmesh_knife.cc`).
   *
   * Holds STATES, not deltas, with `undo_step_current` as the cursor: entry 0 is the state the
   * anchor stroke produced, and a new snapshot truncates any redo branch above the cursor. */
  Vector<CurvePatchEditStep> undo_steps;
  int undo_step_current = -1;
};

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
   * the effect's snapshot, this does NOT persist for the patch's whole life -- cleared at the start
   * of every `curve_patch_restore_and_restamp()`, since blending is only meaningful between passes
   * of the SAME restamp. */
  Map<int, float2> pass_weight_accum;

  /** PBVH node indices displaced by the PREVIOUS restamp (one bit per `bke::pbvh::Tree` node).
   * `curve_patch_restore_only()` tags exactly these nodes so `bke::pbvh::update_normals()` reverts
   * only their normals -- the footprint that just moved away, which a "current node mask only" tag
   * would miss. `curve_patch_restore_and_restamp()` clears it before each restamp; the relief action
   * ORs each symmetry pass's node mask back in, so it always describes what the last restamp touched.
   * Tracking this precisely is what keeps a restore/re-stamp O(patch footprint) rather than
   * O(whole mesh) on every interactive drag event -- see `paint_curve_patch_session.cc`. */
  BitVector<> last_restamp_nodes;

  /** Union of EVERY restamp's node mask over the patch's whole life (one bit per
   * `bke::pbvh::Tree` node), as opposed to #last_restamp_nodes which describes only the latest
   * one. Sized alongside it but never cleared.
   *
   * This is the mask the commit-time undo step is built from, and the wider set is required, not a
   * safety margin: `ReliefEffect::smooth_relief()` writes to every key of the effect's snapshot, which
   * accumulates across the patch's whole life. A vertex touched by an early restamp and left alone
   * by the final one sits in that map holding a zero displacement, and smoothing averages it with
   * its displaced neighbors into a non-zero one -- so it moves at commit time even though its
   * node is absent from #last_restamp_nodes. Every such key was written by SOME restamp, whose
   * node mask this set contains, so the coverage holds by construction. */
  BitVector<> all_touched_nodes;
};

/**
 * One patch: a curve the user drew, the brush values frozen when it started, and everything built
 * from the two.
 *
 * `params` is per-item rather than per-session because a curve started later can have been drawn
 * with a different brush size: `radius`, `plane_normal`, `stamp_seed` and `swap_axis` are frozen at
 * the moment that curve begins. What is NOT per-item is anything describing the TARGET -- the
 * effect, the texture binding and `CurvePatchApplyState` all describe the mesh, which is shared.
 */
struct CurvePatchItem {
  /** Index of the source PaintCurve spline. -1 for procedurally-created curves. */
  int source_curve_index = -1;

  /** The user-editable control curve. Not attached to any `Brush`/datablock -- a standalone
   * runtime `CurvesGeometry`, built fresh via `paintcurve_geometry_init_bezier()` (see
   * `paint_curve_geometry.cc:546`) at Curve Patch start. */
  bke::CurvesGeometry control_curve;

  /** Brush/texture parameter values frozen at the moment this patch's stroke started. Re-stamp
   * reads these instead of the live `Brush`/`MTex`, so a mid-edit change to the brush panel's
   * sliders cannot alter an already-started Curve Patch.
   *
   * NOTE: brush *strength* is deliberately NOT frozen here -- it is read live from the brush on
   * every re-stamp (see `curve_patch_restore_and_restamp()`, which passes `std::nullopt` as
   * `do_symmetrical_brush_actions()`'s `forced_bstrength` so it recomputes `brush_strength()` from
   * the current UI slider), and the modal editor re-stamps whenever the slider changes. `radius`,
   * `radius_per_size`, `swap_axis`, `plane_normal` and `stamp_seed` stay frozen; every other field
   * is re-synced LIVE from the brush by `curve_patch_edit_modal()`'s poll, so changing e.g. the
   * Length mode or the Stamps randomization re-projects the texture immediately. */
  bke::CurvePatchParams params;

  bke::CurvePatchGeometry geometry;
};

/** A live Curve Patch editing session. Published on `SculptSession::curve_patch_session`. */
struct CurvePatchSession {
  /** Every patch this session applies, in creation order. Applied one after another within each
   * symmetry pass; see `curve_patch_apply_effect_action()`.
   *
   * The interactive editor currently publishes exactly one. The plural exists because the headless
   * apply path can be asked for every spline of a paint curve at once, and because switching
   * between curves in the modal is the next step. */
  Vector<CurvePatchItem> patches;

  /** Index into #patches the modal editor is acting on. Consumers must validate it against
   * `patches.size()`: the session outlives the modal. */
  int active_patch = 0;

  CurvePatchTextureBinding texture;
  CurvePatchEditState edit;
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

  /** Suppresses repeating the window-cap warning: `curve_patch_restore_and_restamp()` runs on EVERY
   * mouse event, so an unguarded message would flood the info line. */
  bool reported_frame_cap = false;

  /** Whether #active_item may be dereferenced. False only on a half-built session: every publish
   * path appends a patch before publishing. */
  bool has_active_item() const
  {
    return this->patches.index_range().contains(this->active_patch);
  }

  /** The patch the modal editor acts on. */
  CurvePatchItem &active_item()
  {
    BLI_assert(this->has_active_item());
    return this->patches[this->active_patch];
  }
  const CurvePatchItem &active_item() const
  {
    BLI_assert(this->has_active_item());
    return this->patches[this->active_patch];
  }
};

/** Return the patch corresponding to a PaintCurve spline, or -1 when no exact match exists. */
int curve_patch_index_for_source_curve(const CurvePatchSession &session, int source_curve_index);

/**
 * Set the build-quality switch on every patch.
 *
 * `final_quality` describes the BUILD, not one curve -- every patch of a session is always
 * rebuilt at the same quality -- but it lives in the per-patch #bke::CurvePatchParams because that
 * is what the core build takes. Readers take it off #CurvePatchSession::active_item.
 */
inline void curve_patch_set_final_quality(CurvePatchSession &session, const bool value)
{
  for (CurvePatchItem &item : session.patches) {
    item.params.final_quality = value;
  }
}

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
 * `plane_normal` and `stamp_seed` are frozen when the patch starts, and `swap_axis` is owned by the
 * session as well -- the modal's S hotkey and its undo stack both write it without touching the
 * brush, so reading it back from the brush unconditionally would undo them on the next poll.
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
 * The half of starting a session that owes nothing to a stroke: reset the per-session bookkeeping,
 * build the pristine surface snapshot, create the effect of `effect_type` and publish the session
 * on `ob`.
 *
 * `session->patches` must already hold at least one fully built item -- curve and `params` both
 * (the surface snapshot ignores the parameters, but every later consumer does not). Nothing is
 * stamped here and no viewport is touched, which is what lets the headless apply path share it with
 * the two interactive entry points.
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
 * window manager can drive it -- which is the whole point of the headless apply path. `reports` may
 * be null; the two conditions it carries (a mesh changed underneath the session, a curve too
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
 * `CurvePatchEditState::undo_steps` for why this cannot go through Blender's own undo systems).
 * Called after an action completes -- once per action, not once per event, so a drag is a single
 * step. Defined in `paint_curve_patch_edit.cc`; declared here (rather than kept file-static)
 * because `paint_curve_patch_session.cc`'s #ED_curve_patch_session_undo_push also needs it, for
 * the Transform system's G/R/S handle drags. */
void curve_patch_undo_push(CurvePatchSession &patch);

/** Restores every vertex in the effect's snapshot and does *not* re-stamp. Used for Esc-cancel. */
void curve_patch_restore_only(Object &ob, const CurvePatchSession &session);

/**
 * Commit a live patch, as an Enter-commit would, because the `SculptSession` holding it is about to
 * be destroyed. Returns true when the patch was actually written; no-op returning false when no
 * patch is live. Frees the session and the `SculptSession::cache` it took over.
 *
 * Call from a mode-exit path that still has a `bContext` and has not torn anything down yet -- the
 * final re-stamp needs an evaluated depsgraph, and the undo step this pushes is left parked for the
 * calling operator's `OPTYPE_UNDO` exactly as `curve_patch_finish_commit()` requires.
 *
 * The modal that owns the patch (`SCULPT_OT_curve_patch_edit`) is deliberately left running: it
 * sees the freed session on its next event and releases its own operator state through the liveness
 * guard at the top of `curve_patch_edit_modal()`.
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
 * `SculptSession::curve_patch_session` is owned by `SCULPT_OT_curve_patch_edit` and freed when that
 * modal commits or cancels, but leaving sculpt mode frees the session without consulting the modal
 * -- so without this the session (and the `SculptSession::cache` the patch took over) leaked, and
 * the mesh kept the uncommitted relief. #SculptSession's destructor cannot do it: `CurvePatchSession`
 * is an editor type that blenkernel only forward-declares.
 *
 * Cancel rather than commit is deliberate. Committing needs a `bContext` to re-stamp and push an
 * undo step, and the session is also freed from paths that have none -- object deletion
 * (#BKE_object_free) among them -- so committing here would make the outcome depend on HOW the
 * session ended. The modal itself is left to notice the freed session on its next event and tear
 * its own operator state down (see the liveness guard in `curve_patch_edit_modal()`).
 */
void curve_patch_discard_on_session_end(Object &ob);

/**
 * Finish a committed Curve Patch edit: close the patch's position undo step, and -- if the brush's
 * `curve_patch.face_set` flag is set and the relief actually raised anything -- assign a fresh face
 * set to the raised faces in an undo step of its own.
 *
 * Call ONCE, from the commit branch of `SCULPT_OT_curve_patch_edit`, INSTEAD of a bare
 * `undo::push_end()`, and after the final-quality re-stamp so the threshold is measured against the
 * smoothed profile the user will actually keep. Never called on cancel.
 *
 * Closing the undo step belongs here rather than in the caller because HOW it must be closed
 * depends on whether a face set follows: an undo step carries exactly one `undo::Type`, so the face
 * set needs a second step, and opening one would destroy the position step unless that step was
 * force-pushed first (see the implementation). When no face set follows, the position step must be
 * left parked instead, so that the operator's own `OPTYPE_UNDO` push is the one that files it and
 * the edit costs exactly one Ctrl+Z. That is only knowable after the raised faces have been
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

/** Plane the patch is projected onto, fitted to the control curve itself. */
float3 curve_patch_plane_normal_from_curve(const bke::CurvesGeometry &curve);

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
bool curve_patch_start_from_anchor(const Depsgraph &depsgraph,
                                   Object &ob,
                                   Sculpt &sd,
                                   const Brush &brush,
                                   const ViewContext &vc);

/**
 * Publish a Curve Patch session from a finished #BRUSH_STROKE_ROLL stroke. Builds the control
 * curve from the stroke's resampled contour (`control_positions`, object space, with a per-point
 * `control_radii`), after undoing the live roll relief back to a pristine baseline. From there the
 * handoff is identical to #curve_patch_start_from_anchor, including the returned "launch the modal
 * yourself" contract: it takes over ownership of `SculptSession::cache` but NOT the stroke's undo
 * transaction, which the caller discards once the modal has started; and on any early bail (Dynamic
 * Topology, degenerate input) it frees `ss.cache` itself and returns false.
 * `control_positions.size()` must equal `control_radii.size()`. `plane_normal` is the roll's frozen
 * projection normal, used as the patch's projection plane.
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
