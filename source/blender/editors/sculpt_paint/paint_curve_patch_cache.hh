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

#include "BLI_bit_vector.hh"
#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

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
   * curve is not. On top of that the whole session lives inside ONE open sculpt transaction, so
   * per-edit official steps would mean closing and reopening it on every drag. A modal that owns
   * runtime state no undo type describes keeps its own stack -- exactly what the knife tool does
   * with `KnifeUndoFrame` / `kcd->undostack` (`editmesh_knife.cc`).
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
 * Start the Curve Patch modal editor (`SCULPT_OT_curve_patch_edit`) right after a
 * `BRUSH_STROKE_CURVE_PATCH` anchor stroke finishes. Takes over ownership of the just-finished
 * stroke's `SculptSession::cache` and the undo transaction opened by `stroke_undo_begin()` for
 * that stroke; the caller must not tear either down itself when this is invoked. See
 * `paint_curve_patch_edit.cc` (Stage 04) for the modal editor implementation.
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
 * `SculptSession::cache` and the open undo transaction (the caller must not tear either down), and
 * on any early bail (Dynamic Topology, degenerate input) it frees `ss.cache` itself.
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
