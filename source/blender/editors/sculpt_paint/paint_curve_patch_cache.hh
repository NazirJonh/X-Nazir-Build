/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Runtime state for one live Curve Patch (see Curve Patch Stroke design doc). Owned by
 * `SculptSession::curve_patch_cache`, sibling to `StrokeCache`/`filter::Cache`/`expand::Cache`.
 * Created right after the anchor stroke of a `BRUSH_STROKE_CURVE_PATCH` brush finishes; destroyed
 * on commit (Enter) or cancel (Esc). See `paint_curve_patch_cache.cc` for the restore-then-
 * re-stamp recompute that uses it.
 */

#include <array>

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_texture_types.h"

#include "BKE_curves.hh"

#include "ED_view3d.hh"

#include "paint_curve_patch_ribbon.hh"
#include "paint_curve_patch_spline.hh"

struct Brush;
struct Depsgraph;
struct Object;
struct Sculpt;
struct bContext;

namespace blender::ed::sculpt_paint {

/** Brush/texture parameter values frozen at the moment the anchor stroke started. Re-stamp reads
 * these instead of the live `Brush`/`MTex`, so a mid-edit change to the brush panel's sliders
 * cannot alter an already-started Curve Patch.
 *
 * NOTE: brush *strength* is deliberately NOT frozen here -- it is read live from the brush on every
 * re-stamp (see `curve_patch_restore_and_restamp()`, which passes `std::nullopt` as
 * `do_symmetrical_brush_actions()`'s `forced_bstrength` so it recomputes `brush_strength()` from the
 * current UI slider), and the modal editor re-stamps whenever the slider changes. Radius and the
 * texture-axis swap stay frozen. */
struct CurvePatchFrozenBrushParams {
  float radius = 0.0f;
  bool swap_axis = false;
  /** #eMTex_CurvePatchLengthMode and REPEAT-mode repeat count driving the along-length texture
   * mapping (see #curve_patch_texture_tile_span). Seeded from the brush at anchor time, but unlike
   * `radius`/`swap_axis` these are re-synced LIVE from `brush.mtex` by the modal editor's poll
   * (`curve_patch_edit_modal`) so changing the Length mode or Repeats count re-projects the texture
   * immediately. The relief formula reads these fields; the modal writes them before re-stamping. */
  int length_mode = 0;
  int length_repeat = 1;
  /** #eMTex_CurvePatchEndFalloff and the fade length (percentage of the curve's total arc-length)
   * applied at the curve's two ends. Live-synced from `brush.mtex` by the same modal poll as
   * `length_mode`/`length_repeat`. */
  int end_falloff_mode = 0;
  int end_falloff_percent = 0;
  /** #eMTex_CurvePatchStampMode plus the two per-stamp randomization amounts, as fractions in
   * `[0, 1]` (the DNA fields are percentages). Live-synced from `brush.mtex` by the same modal
   * handler as `length_mode`/`end_falloff_mode`. */
  int stamp_mode = 0;
  float stamp_size_random = 0.0f;
  float stamp_strength_random = 0.0f;
  /** #eMTex_CurvePatchStampProjection: which coordinate frame the stamps' texture is sampled in.
   * Live-synced from `brush.mtex` by the same modal handler as `stamp_mode` -- without that the
   * user would flip the Projection toggle mid-edit and see nothing change until the next patch. */
  int stamp_projection = 0;
  /** Seed for the Stamps-mode randomization, rolled ONCE per patch (and re-rolled only by the
   * explicit Reseed action). It is not persisted: the control curve is session-local runtime data,
   * so there is nothing for a stored seed to reproduce. Freezing it here is what keeps the relief
   * stable across the re-stamps that fire on every interactive event. */
  uint32_t stamp_seed = 0;
  /** World-space radius per unit of the brush's Size slider, captured at patch start. Converts a
   * live Size change into a world radius, and absolute (pixel) brush jitter into world units. */
  float radius_per_size = 0.0f;
};

/** One snapshot of the session-local undo stack: everything the user edits inside a live Curve
 * Patch that the relief is derived from. The control curve carries positions, handles, handle
 * types, radii and `cyclic` internally, so nothing else about it needs storing. */
struct CurvePatchEditStep {
  bke::CurvesGeometry curve;
  bool swap_axis = false;
  /* Snapshotted for the same reason as `swap_axis`: the Reseed action changes the visible relief
   * without touching `curve`, so without this Ctrl+Z could not walk back over a reseed. */
  uint32_t stamp_seed = 0;
};

struct CurvePatchCache {
  /** The user-editable control curve. Not attached to any `Brush`/datablock — a standalone
   * runtime `CurvesGeometry`, built fresh via `paintcurve_geometry_init_bezier()` (see
   * `paint_curve_geometry.cc:546`) at Curve Patch start. */
  bke::CurvesGeometry control_curve;

  CurvePatchFrozenBrushParams frozen_params;

  /** Stamps-mode layout, rebuilt once per re-stamp on the main thread right after the spline (see
   * `curve_patch_stamps_build`). PHASE 1's parallel per-vertex walk only reads it. Empty in Ribbon
   * mode. */
  Vector<CurvePatchStamp> stamps;

  /** Resolved texture variants for this restamp: copies of `Brush::mtex` with only `tex` swapped, so
   * every mapping setting (Size / Offset / Angle / Swap Axis) stays shared. Rebuilt on every restamp
   * because the texture source toggles are live-synced like `stamp_mode`. Empty in SINGLE mode,
   * where the relief reads `brush.mtex` directly.
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
   * is set purely from `MTex::curve_patch_ribbon_texture_source`, deliberately without also testing
   * `stamp_mode`, so it is true in Stamps mode too even though Stamps has no caps. Adding that test
   * would create a second place obliged to stay in sync with the relief's branch selector, which is
   * exactly the drift `stamp_search_reach` above was burned by. Instead the invariant is one-way:
   * ONLY the Ribbon branch of `branch_relief()` reads this field (and `ribbon_zone_variants` /
   * `world_cap_*` with it), and that branch runs only when `stamp_mode` is not STAMPS.
   *
   * The two cap lengths are already resolved from brush diameters into world units. */
  bool caps_enabled = false;
  float world_cap_start = 0.0f;
  float world_cap_end = 0.0f;

  /** Index into `control_curve` of the point the user last interacted with, or -1 for none. Owned
   * conceptually by the live-edit modal (`SCULPT_OT_curve_patch_edit`), but stored here rather than
   * in its `op->customdata` so the small operators the modal's context menu invokes -- which cannot
   * reach a running modal's customdata -- can act on the clicked point. Because this cache outlives
   * the modal, the index is reset to -1 whenever an edit session ends or a new control curve is
   * built; consumers must still validate it against `control_curve.points_num()`. */
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

  /** Plane normal frozen at anchor time (e.g. the anchor dab's `sculpt_normal`), passed to
   * `CurvePatchSpline::plane_normal` on every rebuild. */
  float3 plane_normal = float3(0.0f, 0.0f, 1.0f);

  /** Lazily-grown snapshot of original (pre-patch) vertex positions, keyed by a flat index into
   * whichever position array is authoritative for the object's current `bke::pbvh::Type` -- a
   * mesh vertex index into `Mesh::vert_positions()` for `Type::Mesh`, or a flat CCG element index
   * into `SubdivCCG::positions` (`grid * grid_area + in-grid offset`) for `Type::Grids`. Never
   * both at once: an object's pbvh type does not change while a patch is alive. A vertex is
   * inserted the first time it is about to be touched by a re-stamp, never removed until the
   * whole patch is destroyed. Restoring the patch = writing every entry in this map back into
   * that position array before re-stamping. Dynamic Topology (`Type::BMesh`) has no such stable
   * index and is refused outright by `curve_patch_start_from_anchor()`. */
  Map<int, float3> orig_positions;

  /** Number of elements the keys of #orig_positions index into, sampled once when the patch
   * starts: `Mesh::verts_num` for `Type::Mesh`, `SubdivCCG::positions.size()` for `Type::Grids`.
   *
   * The modal passes events through whenever the cursor leaves its region, so an unrelated
   * operator can retopologize the object while a patch is live. Every key in #orig_positions
   * would then name a different element or none at all, and both restoring and committing would
   * write against the wrong surface -- or past the end of the array. Comparing this count is the
   * cheap detection for that. */
  int64_t element_num = 0;

  /** Set when the check above fails. The patch is then unusable: its snapshot describes a mesh
   * that no longer exists, so it must be abandoned WITHOUT restoring (which would write stale
   * positions) and without pushing anything to the undo stack. */
  bool invalidated = false;

  /** Rebuilt every call to `curve_patch_restore_and_restamp()` from `control_curve`; kept here so
   * later calls (dynamic growth) can compare against the previous footprint if needed. */
  CurvePatchSpline spline;

  /** Set for the single re-stamp taken when the patch is committed, cleared the rest of the time.
   * It switches the relief from one texture sample per vertex to a supersampled average and builds
   * the ribbon at a higher resolution, so the mesh keeps a smoother, less aliased profile than the
   * interactive preview could afford. See `docs/superpowers/specs/` for the design. */
  bool final_quality = false;

  /** Whole-curve ribbon UV lookup table, rebuilt alongside `spline` on every restamp. The relief
   * action samples this instead of `CurvePatchSpline::closest_point()` so the parameterization
   * stays single-valued through sharp turns (see `paint_curve_patch_ribbon.hh`). */
  CurvePatchRibbonLut ribbon;

  /** World-space brush radius the `ribbon` above was actually built from, recorded by
   * `curve_patch_restore_and_restamp()` on every restamp right before the build.
   *
   * In Ribbon mode this equals `frozen_params.radius`. In Stamps mode it is WIDENED by the layout's
   * jitter amount so stamps pushed sideways still fall inside the strip. Because the ribbon's `u`
   * is normalized across the half-width it was built with, anything reconstructing a world-space
   * lateral offset from `u` -- and anything bounding how far from the curve the relief can reach --
   * must scale by THIS radius, not by `frozen_params.radius`. It lives here rather than in
   * `frozen_params` because it is not frozen at anchor time: it is re-derived per restamp from the
   * live brush's jitter, exactly like `stamps` next to it. */
  float ribbon_radius = 0.0f;

  /** Conservative arc-length half-window covering one stamp's full footprint, resolved ONCE per
   * restamp right after the stamp layout and read by every consumer that has to find or cover a
   * stamp: the per-vertex search window, the cyclic seam wrap and the ribbon's end extension.
   *
   * Those three were separate call sites of #curve_patch_stamp_reach, and their agreement is a
   * correctness requirement, not a coincidence -- a stamp reaching past any one of them is silently
   * clipped, and a seam ghost placed with a smaller bound than the search window uses is never
   * found at all. The PLANAR projection widens the bound (a laterally jittered stamp covers more
   * arc length once its frame stops following the curve), which is exactly the kind of change that
   * would have had to be repeated in three places. Storing it once makes that impossible.
   *
   * 0 in Ribbon mode, where there are no stamps to bound. */
  float stamp_search_reach = 0.0f;

  /** World-space distance the `ribbon` above was extended PAST each of a non-cyclic curve's two
   * ends, recorded next to `ribbon_radius` and for the same reason: the sites that need it are in
   * other functions than the one that computes it.
   *
   * 0 in Ribbon mode, where the strip stops exactly at the curve's ends. In Stamps mode it is the
   * farthest a stamp centered on an end point can reach beyond that end, so those stamps render
   * whole instead of being clipped by the strip's edge. Everything bounding the relief's reach
   * ALONG the curve must add it: the arc-length range the relief accepts, the PBVH cull tube and
   * the whole-curve search sphere, all of which are otherwise derived from the curve's own points
   * and would cut the overhang off again. */
  float ribbon_end_margin = 0.0f;

  /** Per-restamp accumulator for blending symmetry passes that land on the same real vertex (a
   * patch straddling a mirror/radial symmetry plane can have both the direct and the mirrored
   * pass claim the same vertex). Keyed like `orig_positions`; `x` is the running sum of each
   * claiming pass's falloff weight, `y` the running sum of `weight * height`, so every pass's
   * PHASE 2 can recompute the weighted-average target height so far instead of unconditionally
   * overwriting an earlier pass's contribution (see `curve_patch_apply_relief_action()`). Unlike
   * `orig_positions`, this does NOT persist for the patch's whole life -- cleared at the start of
   * every `curve_patch_restore_and_restamp()`, since blending is only meaningful between passes
   * of the SAME restamp. */
  Map<int, float2> pass_weight_accum;

  /** PBVH node indices displaced by the PREVIOUS restamp (one bit per `bke::pbvh::Tree` node).
   * `curve_patch_restore_only()` tags exactly these nodes so `bke::pbvh::update_normals()` reverts
   * only their normals -- the footprint that just moved away, which a "current node mask only" tag
   * would miss. `curve_patch_restore_and_restamp()` clears it before each restamp; the relief action
   * ORs each symmetry pass's node mask back in, so it always describes what the last restamp touched.
   * Tracking this precisely is what keeps a restore/re-stamp O(patch footprint) rather than
   * O(whole mesh) on every interactive drag event -- see `paint_curve_patch_cache.cc`. */
  BitVector<> last_restamp_nodes;

  /** Union of EVERY restamp's node mask over the patch's whole life (one bit per
   * `bke::pbvh::Tree` node), as opposed to #last_restamp_nodes which describes only the latest
   * one. Sized alongside it but never cleared.
   *
   * This is the mask the commit-time undo step is built from, and the wider set is required, not a
   * safety margin: `curve_patch_smooth_relief()` writes to every key of #orig_positions, which
   * accumulates across the patch's whole life. A vertex touched by an early restamp and left alone
   * by the final one sits in that map holding a zero displacement, and smoothing averages it with
   * its displaced neighbors into a non-zero one -- so it moves at commit time even though its
   * node is absent from #last_restamp_nodes. Every such key was written by SOME restamp, whose
   * node mask this set contains, so the coverage holds by construction. */
  BitVector<> all_touched_nodes;

  /** Owned copy of the anchor stroke's `ViewContext`, set once in `curve_patch_start_from_anchor`.
   * `StrokeCache::vc` is a non-owning pointer that normally points into the interactive stroke's
   * own (stack-lifetime) `ViewContext`; since Curve Patch takes over `SculptSession::cache` after
   * that stroke's operator has already finished and torn its `ViewContext` down, `StrokeCache::vc`
   * is repointed at this member instead so every `curve_patch_restore_and_restamp()` re-stamp
   * (which reads it via `calc_local_from_screen()`/`cache->vc`) dereferences valid memory for the
   * whole lifetime of the patch. */
  ViewContext view_context = {};
};

/**
 * Restore every vertex in `patch.orig_positions` to its snapshotted position, then re-apply the
 * frozen brush along the current `patch.control_curve`, growing `patch.orig_positions` on demand
 * for any newly-touched vertex not yet snapshotted. Call after every control-curve mutation
 * (move/add/remove point, radius change, axis toggle) and once more right before commit.
 */
void curve_patch_restore_and_restamp(bContext &C, Object &ob, CurvePatchCache &patch);

/** Restores every vertex in `patch.orig_positions` and does *not* re-stamp. Used for Esc-cancel. */
void curve_patch_restore_only(Object &ob, const CurvePatchCache &patch);

/**
 * Finish a committed Curve Patch edit: close the patch's position undo step, and -- if the brush's
 * `curve_patch_face_set` flag is set and the relief actually raised anything -- assign a fresh face
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
 * "Raised" is measured as displacement from `patch.orig_positions`, thresholded at a fraction of
 * this patch's own maximum displacement -- relative rather than absolute, because displacement is
 * in scene units and would otherwise behave differently on differently scaled objects.
 */
void curve_patch_finish_commit(bContext &C, Object &ob, const CurvePatchCache &patch);

/**
 * Start the Curve Patch modal editor (`SCULPT_OT_curve_patch_edit`) right after a
 * `BRUSH_STROKE_CURVE_PATCH` anchor stroke finishes. Takes over ownership of the just-finished
 * stroke's `SculptSession::cache`; the caller must not tear that down itself when this is invoked.
 *
 * It does NOT take over the undo transaction `stroke_undo_begin()` opened for that stroke. This
 * function restores the mesh to its pre-stroke state before anything else touches it, so by the
 * time it returns that transaction describes no net change, and the caller discards it
 * (`SculptPaintStroke::done()`, `mesh/sculpt.cc`). The editor records its own single undo step
 * when the patch is committed. Leaving a transaction open for the modal's whole lifetime is what
 * let any unrelated undo push in the application adopt or free it. See `paint_curve_patch_edit.cc`
 * (Stage 04) for the modal editor implementation.
 */
void curve_patch_start_from_anchor(const Depsgraph &depsgraph,
                                    Object &ob,
                                    Sculpt &sd,
                                    const Brush &brush,
                                    const ViewContext &vc);

/**
 * Start the Curve Patch modal editor from a finished #BRUSH_STROKE_ROLL stroke. Builds the control
 * curve from the stroke's resampled contour (`control_positions`, object space, with a per-point
 * `control_radii`), after undoing the live roll relief back to a pristine baseline. From there the
 * handoff is identical to #curve_patch_start_from_anchor: it takes over ownership of
 * `SculptSession::cache` but NOT the stroke's undo transaction, which the caller discards once the
 * modal has started; and on any early bail (Dynamic Topology, degenerate input) it frees
 * `ss.cache` itself.
 * `control_positions.size()` must equal `control_radii.size()`. `plane_normal` is the roll's frozen
 * projection normal, used as the patch's projection plane.
 */
void roll_start_curve_patch_from_stroke(const Depsgraph &depsgraph,
                                        Object &ob,
                                        Sculpt &sd,
                                        const Brush &brush,
                                        const ViewContext &vc,
                                        Span<float3> control_positions,
                                        Span<float> control_radii,
                                        const float3 &plane_normal);

}  // namespace blender::ed::sculpt_paint
