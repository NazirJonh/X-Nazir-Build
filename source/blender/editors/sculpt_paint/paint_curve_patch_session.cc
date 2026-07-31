/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <optional>

#include "paint_curve_patch_session.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_bit_span_ops.hh"
#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_time.h"

#include "MEM_guardedalloc.h"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "paint_curve_intern.hh"
#include "paint_curve_patch_sampler.hh"

#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"

/* `CURVE_PATCH_PROFILING` (the `DEBUG-cpatch` timing below) is defined in
 * `paint_curve_patch_effect.hh`, pulled in via `paint_curve_patch_session.hh`, so one toggle also
 * drives the Image-effect's `DEBUG-cpatch-image` sub-phase breakdown. */

namespace blender::ed::sculpt_paint {

const bke::CurvesGeometry *ED_paint_curve_patch_active_control_curve(const Object *ob)
{
  if (ob == nullptr || ob->runtime->sculpt_session == nullptr ||
      ob->runtime->sculpt_session->curve_patch_session == nullptr)
  {
    return nullptr;
  }
  const CurvePatchSession &session = *ob->runtime->sculpt_session->curve_patch_session;
  if (!session.has_active_item()) {
    return nullptr;
  }
  return &session.active_item().control_curve;
}

void curve_patch_restore_only(Object &ob, const CurvePatchSession &session)
{
  /* Hoisted here from the three effects' own copies. It has to live at this level, not in
   * `curve_patch_restore_and_restamp()`: mode exit (#curve_patch_discard_on_session_end) and the
   * modal's Esc branch both restore WITHOUT re-stamping first, so neither has raised
   * `CurvePatchApplyState::invalidated` even when the mesh changed underneath the session. Writing
   * the snapshot back in that state would put stale values into an unrelated mesh. */
  if (session.effect->element_num(ob) != session.apply.element_num) {
    return;
  }
  session.effect->restore(ob, session);
}

bke::CurvesGeometry curve_patch_control_curve_from_points(const Span<float3> positions,
                                                          const Span<float> radii,
                                                          const bool cyclic)
{
  BLI_assert(positions.size() == radii.size());

  bke::CurvesGeometry curve;
  paintcurve_geometry_init_bezier(curve, int(positions.size()));
  if (positions.is_empty()) {
    return curve;
  }

  curve.positions_for_write().copy_from(positions);
  /* `paintcurve_geometry_init_bezier()` never touches the "radius" attribute, so without this it
   * silently falls back to `bke::CurvesGeometry::radius()`'s generic 0.01 default (meant for
   * hair-curve-style geometry) instead of this codebase's own paint-curve convention of 1.0 =
   * "full brush size" (see `paintcurve_geometry_add_point()`, `paint_curve.cc:621`). At 0.01 the
   * radius-handle's screen offset is only ~10px from the pivot -- barely past its own 10px hit
   * threshold, making it effectively unclickable. */
  curve.radius_for_write().copy_from(radii);
  if (cyclic) {
    curve.cyclic_for_write().fill(true);
  }

  /* `paintcurve_geometry_init_bezier()` sets the handle TYPES to AUTO but never creates the handle
   * POSITION attributes, and `calculate_bezier_auto_handles()`/`calculate_bezier_aligned_handles()`
   * early-out when those attributes are absent (`curves_geometry.cc`). So without materializing them
   * first, the AUTO handle positions are never computed, the evaluated Bezier collapses to the origin
   * (confirmed via `eval_first=(0,0,0) eval_last=(0,0,0) eval_dist=0` in the debug log), and the first
   * re-stamp builds a zero-length spline that writes no relief -- which is why the initial preview was
   * invisible until the user's first point drag. That drag worked only because its handler had already
   * created the handle-position attributes via `_for_write` accessors. Add both attributes here (the
   * `_for_write` accessors create them, zero-filled), then compute the AUTO/aligned handle positions
   * from the control points, in the same order every position mutation in `paint_curve.cc` uses. */
  curve.handle_positions_left_for_write();
  curve.handle_positions_right_for_write();
  curve.calculate_bezier_auto_handles();
  curve.calculate_bezier_aligned_handles();
  if (cyclic) {
    /* `cyclic` is topology, and the init above tagged before it was written -- the same order
     * `SCULPT_OT_curve_patch_toggle_cyclic` uses when it flips the flag on a live curve. */
    curve.tag_topology_changed();
  }
  /* Mirrors every other position mutation in `paint_curve.cc` (e.g. `paintcurve_point_add()`):
   * invalidates cached evaluated data so the first `curve_patch_restore_and_restamp()` call sees
   * the real, just-set positions rather than the freshly-constructed curve's stale/default cache. */
  curve.tag_positions_changed();
  return curve;
}

/* -------------------------------------------------------------------- */
/** \name Restore and Re-Stamp
 * \{ */

/** Matches the raw `BrushActionFunc` signature `do_symmetrical_brush_actions()` expects
 * (`mesh/sculpt_intern.hh:855`) so it can be passed as its `action` callback. Called once per
 * enabled symmetry pass (mirror x radial x tile) with `StrokeCache::location_symm`/`radius`
 * already set (by `cache_calc_brushdata_symm()`, invoked internally by
 * `do_symmetrical_brush_actions()`) to the whole curve's encompassing sphere in that pass's
 * transformed space. Unlike a normal brush dab, this forwards straight to the session's effect --
 * no `sculpt_brush_type` dispatch, no `do_brush_action()` call. */
/* Whether a patch describes a strip at all: the build can leave the spline empty on a degenerate
 * curve, and the structure the sampler reads -- the window set on the surface path, the whole-curve
 * LUT otherwise -- can fail to rasterize even when it does not. Both were separate early returns in
 * `curve_patch_restore_and_restamp()`; with N patches they become a per-patch predicate, because one
 * unbuildable curve must not silence the others. */
static bool session_item_is_buildable(const CurvePatchItem &item)
{
  if (item.geometry.spline.is_empty()) {
    return false;
  }
  return item.geometry.surface.ready ? item.geometry.frames.ready : item.geometry.ribbon.ready;
}

static void curve_patch_apply_effect_action(const Depsgraph &depsgraph,
                                            const Scene & /*scene*/,
                                            Sculpt & /*sd*/,
                                            Object &ob,
                                            const Brush &brush,
                                            PaintModeSettings & /*paint_mode_settings*/)
{
  CurvePatchSession &session = *ob.runtime->sculpt_session->curve_patch_session;
  /* One pass per patch, inside this symmetry pass. Overlapping patches meet in
   * `CurvePatchApplyState::pass_weight_accum` exactly as two symmetry passes do -- which is why
   * this is a plain loop and not a restore between patches: restoring would erase the previous
   * patch's contribution. */
  for (const CurvePatchItem &item : session.patches) {
    if (!session_item_is_buildable(item)) {
      /* Skipped rather than refused: asking for every spline of a curve must not fail because one
       * of them is a stray single point. */
      continue;
    }
    session.effect->apply_pass(depsgraph, ob, brush, session, item);
  }
}

/* Tessellate every patch's control curve, project it onto the shared surface snapshot, and rebuild
 * the structures derived from it. No mesh writes, no PBVH, no reports. */
static void session_rebuild_geometry(CurvePatchSession &session)
{
  for (CurvePatchItem &item : session.patches) {
    bke::curve_patch_build_from_control_curve(item.control_curve,
                                              item.params,
                                              session.texture.stamp_texture_weights_cdf,
                                              item.geometry);
  }
}

/* Restore the effect's snapshot, then re-apply the patch through every symmetry pass. */
static void session_apply(const Scene &scene,
                          const Depsgraph &depsgraph,
                          Sculpt &sd,
                          PaintModeSettings &paint_mode_settings,
                          Object &ob,
                          CurvePatchSession &session)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  /* NOTE on `depsgraph`: the interactive caller resolves it with the plain `CTX_data_depsgraph_
   * pointer()`, NOT `CTX_data_ensure_evaluated_depsgraph()`, which is a full
   * `BKE_scene_graph_update_tagged()` rather than a getter. The two consumers below are the same
   * ones an ordinary sculpt dab feeds, and `SculptPaintStroke::update_step()` (`mesh/sculpt.cc`)
   * hands them a depsgraph captured once at stroke start -- sculpt's only `ensure_evaluated` is the
   * one-off at stroke START. Running a scene evaluation per re-stamp is both needless and costly
   * here: `flush_update_step()` at the end of every re-stamp tags `ID_RECALC_SHADING`
   * unconditionally, so the next re-stamp's `ensure` always found pending work and paid for a full
   * evaluation on every frame of a point drag.
   *
   * It was also actively harmful. That evaluation ran between `curve_patch_restore_only()` and the
   * re-stamp below -- with the mesh at its pre-relief positions -- leaving the evaluated mesh
   * sharing the flat position array, which the re-stamp's `vert_positions_for_write()` then stranded
   * (see the long comment in #curve_patch_commit_on_session_end). */

  /* Effect input preparation, once per restamp and before the first symmetry pass. Relief refreshes
   * the vertex normals here; see `ReliefEffect::begin_restamp()` for why that is needed at all. */
  session.effect->begin_restamp(depsgraph, ob, session);

  /* Whole-session encompassing sphere, in canonical (non-mirrored) object space -- lets
   * `do_symmetrical_brush_actions()`'s existing per-pass node-mask query (driven by
   * `cache.location_symm`/`cache.radius`, set below via `cache.location`/`cache.radius` and
   * transformed per pass by `cache_calc_brushdata_symm()`) cover EVERY patch for the effect
   * action above, instead of one small per-dab circle. Conservative bound: any point within
   * `max_radius` of any point on a curve is within `(bbox half-diagonal + max_radius)` of the
   * bbox center, by the triangle inequality.
   *
   * Patches with no strip are skipped: they contribute nothing, and their `poly_3d` can be empty,
   * so reading a first element would be out of bounds. */
  float3 bbox_min(FLT_MAX);
  float3 bbox_max(-FLT_MAX);
  float max_radius = 0.0f;
  float ribbon_end_margin = 0.0f;
  bool any_patch = false;
  for (const CurvePatchItem &item : session.patches) {
    if (!session_item_is_buildable(item)) {
      continue;
    }
    any_patch = true;
    for (const float3 &p : item.geometry.spline.poly_3d) {
      bbox_min = math::min(bbox_min, p);
      bbox_max = math::max(bbox_max, p);
    }
    /* The same bound the per-node cull uses, so this search sphere stays a genuine superset of the
     * world-space reach actually used below -- including the extra `jitter_amount` a Stamps-mode
     * stamp can be pushed out to. Equals `params.radius` in Ribbon mode. */
    max_radius = math::max(max_radius, curve_patch_max_radius(item.geometry));
    ribbon_end_margin = math::max(ribbon_end_margin, item.geometry.ribbon_end_margin);
  }
  if (!any_patch) {
    return;
  }
  const float3 bbox_center = (bbox_min + bbox_max) * 0.5f;
  cache.location = bbox_center;
  /* `ribbon_end_margin` extends the strip along the end tangents, i.e. up to that far outside the
   * bbox built from the curve's own points, so the sphere has to grow by it as well -- it feeds the
   * node-mask query the cull tube above then narrows, and a node missing from it is never offered
   * to the cull at all. 0 in Ribbon mode, leaving the sphere exactly as it was. */
  cache.radius = math::distance(bbox_max, bbox_center) + max_radius + ribbon_end_margin;
  cache.radius_squared = cache.radius * cache.radius;

  /* Reset the touched-node accumulator for THIS restamp. `curve_patch_restore_only()` already
   * consumed the previous restamp's set (to revert those nodes' normals); each symmetry pass of the
   * effect action below now ORs its own node mask back in, leaving `last_restamp_nodes` describing
   * exactly what this restamp displaced -- ready for the next frame's restore. */
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  session.apply.last_restamp_nodes.resize(pbvh.nodes_num());
  session.apply.last_restamp_nodes.fill(false);

  /* Resized but deliberately NOT filled: this one accumulates across restamps. */
  session.apply.all_touched_nodes.resize(pbvh.nodes_num());

  /* Reset the cross-pass blend accumulator for THIS restamp too -- see `pass_weight_accum`'s doc
   * comment. Unlike the effect's snapshot, it must not persist across restamps: each restamp's
   * passes blend only against each other, not against a previous drag frame's weights. */
  session.apply.pass_weight_accum.clear();

  /* `std::nullopt` (not a frozen value) lets `do_symmetrical_brush_actions()` recompute the
   * strength from the LIVE brush via `brush_strength()` -- so a mid-edit change of the brush's
   * Strength slider is picked up on the next re-stamp. The modal editor
   * (`paint_curve_patch_edit.cc`) watches the slider and triggers a re-stamp when it changes, so
   * this tracks the slider in real time. Radius/axis stay frozen (see `CurvePatchSession::params`). */
  do_symmetrical_brush_actions(depsgraph,
                               scene,
                               sd,
                               ob,
                               curve_patch_apply_effect_action,
                               paint_mode_settings,
                               std::nullopt);

  /* Final-quality smoothing (commit only) and whatever else this target has to close off. */
  session.effect->end_restamp(ob, session);

  /* A normal interactive stroke calls this once per step (`SculptPaintStroke::update_step()`,
   * `mesh/sculpt.cc`) -- without it, nothing tells the depsgraph the object needs re-shading, sets
   * `RV3D_PAINTING` (the fast redraw path the PBVH-draw viewport relies on), or refreshes the
   * mesh's eager bounds, so the re-stamped data never actually reaches the screen even though it is
   * correct underneath. All three effects wanted this and differed only in the kind, which is what
   * `CurvePatchEffect::update_type()` now supplies.
   *
   * Skipped for a session with no viewport behind it (the headless apply path): there is no
   * interactive redraw to arm, and `flush_update_step()` dereferences `vc.region` unconditionally
   * on the `UpdateType::Image` branch. Such a caller tags the ID itself once it is done, the same
   * way #curve_patch_discard_on_session_end does. */
  if (session.view_context.region != nullptr) {
    flush_update_step(session.view_context, ob, session.effect->update_type());
  }
}

/* A separate channel from `CURVE_PATCH_PROFILING`: that one is marked in its own comment as
 * temporary instrumentation and would take the visibility of a quality degradation with it on the
 * first cleanup. Kept ahead of the readiness bail, where it has always been. */
static void session_report_frame_cap(ReportList *reports, CurvePatchSession &session)
{
  if (session.reported_frame_cap) {
    return;
  }
  /* One warning for the session, not one per patch: the message names the quality of the result,
   * and which of several curves ran out of windows is not something the user can act on. */
  const bool any_capped = std::any_of(session.patches.begin(),
                                      session.patches.end(),
                                      [](const CurvePatchItem &item) {
                                        return item.geometry.frames.capped;
                                      });
  if (any_capped) {
    session.reported_frame_cap = true;
    BKE_report(reports,
               RPT_WARNING,
               "Curve Patch: curve too complex for full surface wrap, quality reduced");
  }
}

#if CURVE_PATCH_PROFILING
/* DEBUG-cpatch: one line per interactive restamp. Kept out of the re-stamp's own body so that the
 * hot path reads as the three steps it actually is. */
static void session_report_diagnostics(const CurvePatchSession &session,
                                       const double t_begin,
                                       const double t_restore,
                                       const double t_geometry,
                                       const double t_end)
{
  static double prof_prev_start = 0.0;
  const double prof_interval_ms = (prof_prev_start > 0.0) ? (t_begin - prof_prev_start) * 1000.0 :
                                                            -1.0;
  prof_prev_start = t_begin;
  IndexMaskMemory prof_memory;
  const int64_t prof_nodes =
      IndexMask::from_bits(session.apply.last_restamp_nodes, prof_memory).size();
  /* `ribbon` covers the spline rebuild plus the ribbon/LUT build -- previously an unaccounted gap
   * between `restore` and `normals`, which made `total` look larger than the sum of its parts. */
  /* `displaced` is how many vertices THIS re-stamp actually wrote (`pass_weight_accum` is cleared
   * per re-stamp and gains one entry per written vertex); `snapshot` is how many the patch has ever
   * touched. A re-stamp that repeats an earlier one must report the same `displaced` and leave
   * `snapshot` unchanged -- if the commit re-stamp does not, the two passes are not seeing the same
   * geometry, which is the open question behind the doubled pattern on commit. */
  /* `apply` covers the normal refresh, every symmetry pass, the final-quality smoothing and the
   * viewport flush. The normal refresh used to have a column of its own, but `begin_restamp()` moved
   * inside #session_apply together with the passes it prepares, and there is no longer a point
   * between them the orchestrator can time from -- reporting a column it cannot measure would be
   * worse than not reporting it. */
  printf(
      "[DEBUG-cpatch] total=%.2fms | restore=%.2f ribbon=%.2f apply=%.2f | "
      "nodes=%lld displaced=%lld snapshot=%lld quality=%d lut=%d frames=%d capped=%d "
      "interval=%.2fms\n",
      (t_end - t_begin) * 1000.0,
      (t_restore - t_begin) * 1000.0,
      (t_geometry - t_restore) * 1000.0,
      (t_end - t_geometry) * 1000.0,
      (long long)prof_nodes,
      (long long)session.apply.pass_weight_accum.size(),
      (long long)session.effect->snapshot_size(),
      session.active_item().params.final_quality ? 1 : 0,
      session.active_item().geometry.ribbon.res,
      int(session.active_item().geometry.frames.frames.size()),
      session.active_item().geometry.frames.capped ? 1 : 0,
      prof_interval_ms);
  fflush(stdout);
}
#endif

void curve_patch_restore_and_restamp(const Scene &scene,
                                     const Depsgraph &depsgraph,
                                     Sculpt &sd,
                                     PaintModeSettings &paint_mode_settings,
                                     Object &ob,
                                     CurvePatchSession &session,
                                     ReportList *reports)
{
  /* Staleness guard, hoisted here from the hand-written copies in the effects: something
   * retopologized the object while the patch was live -- see `CurvePatchApplyState::element_num`.
   * Bail before touching positions: the snapshot's keys no longer name the elements they were taken
   * from. */
  if (session.effect->element_num(ob) != session.apply.element_num) {
    session.apply.invalidated = true;
    BKE_report(reports,
               RPT_WARNING,
               "Curve Patch canceled: the mesh changed while it was being edited");
    return;
  }

#if CURVE_PATCH_PROFILING
  const double prof_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  curve_patch_restore_only(ob, session);
#if CURVE_PATCH_PROFILING
  const double prof_t_restore = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif

  /* Resolve which textures this restamp samples. Done here, once, on the main thread: the effects'
   * parallel per-element walk only reads the result. Ahead of the geometry rebuild because the
   * stamp layout draws each stamp's slot from the weight table resolved here.
   *
   * The session owns the only `StrokeCache` for the whole live-edit lifetime, but a sculpt stroke
   * that leaks through in another viewport (before the modal's pass-through gate was widened) could
   * tear it down and leave this null -- bail rather than crash. */
  SculptSession *ss = ob.runtime->sculpt_session;
  if (!ss || !ss->cache) {
    return;
  }
  StrokeCache &cache = *ss->cache;
  if (const Brush *tex_brush = BKE_paint_brush_for_read(cache.paint)) {
    /* The binding is shared by every patch, so it is resolved from the active patch's radius --
     * the only one whose brush the user is currently looking at. */
    curve_patch_texture_binding_from_brush(
        *tex_brush, session.active_item().params.radius, session.texture);
  }
  else {
    curve_patch_texture_binding_clear(session.texture);
  }

  session_rebuild_geometry(session);
  session_report_frame_cap(reports, session);
#if CURVE_PATCH_PROFILING
  const double prof_t_geometry = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  const bool any_buildable = std::any_of(
      session.patches.begin(), session.patches.end(), session_item_is_buildable);
  if (!any_buildable) {
    /* The mesh has already been restored above; flush so the viewport shows that, rather than the
     * relief from the previous re-stamp, until the next event arrives. Guarded exactly as the
     * normal end-of-restamp flush is: a headless session has no region to redraw, and
     * `flush_update_step()` dereferences `vc.region` unconditionally. */
    if (session.view_context.region != nullptr) {
      flush_update_step(session.view_context, ob, session.effect->update_type());
    }
    return;
  }

  session_apply(scene, depsgraph, sd, paint_mode_settings, ob, session);
#if CURVE_PATCH_PROFILING
  session_report_diagnostics(
      session, prof_t0, prof_t_restore, prof_t_geometry, BLI_time_now_seconds());
#endif
}

void curve_patch_restore_and_restamp(bContext &C, Object &ob, CurvePatchSession &session)
{
  ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  curve_patch_restore_and_restamp(*CTX_data_scene(&C),
                                  *CTX_data_depsgraph_pointer(&C),
                                  *tool_settings->sculpt,
                                  tool_settings->paint_mode,
                                  ob,
                                  session,
                                  CTX_wm_reports(&C));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session Teardown
 * \{ */

bool curve_patch_commit_on_session_end(bContext &C, Object &ob)
{
  SculptSession *ss = ob.runtime->sculpt_session;
  if (!ss || !ss->curve_patch_session) {
    return false;
  }
  CurvePatchSession *session = ss->curve_patch_session;

  /* Mirrors the commit branch of `curve_patch_edit_finish()` (`paint_curve_patch_edit.cc`), which
   * cannot be reused directly because it is tied to the modal operator's `customdata`. The modal is
   * left running; it notices the freed session on its next event and tears its own state down
   * through its liveness guard. Keep the two in step when either changes. */
  curve_patch_set_final_quality(*session, true);
  curve_patch_restore_and_restamp(C, ob, *session);
  curve_patch_set_final_quality(*session, false);

  bool committed = false;
  if (session->apply.invalidated) {
    /* A foreign operator changed the element count, so nothing can be written -- and
     * `curve_patch_restore_only()` is a no-op in this state, leaving the mesh as that operator left
     * it. Same outcome the modal reports as canceled. */
    curve_patch_restore_only(ob, *session);
  }
  else {
    curve_patch_finish_commit(C, ob, *session);
    flush_update_done(&C, ob, UpdateType::Position);
    committed = true;
  }

  MEM_delete(ss->cache);
  ss->cache = nullptr;
  MEM_delete(session);
  ss->curve_patch_session = nullptr;

  /* Republish the relief to the EVALUATED mesh, which is what Object Mode draws. Without this the
   * mode exit left the original mesh correct -- the .blend saves and reloads with the relief, and a
   * sculpt-mode re-entry rebuilds the PBVH from it -- while Object Mode kept drawing the surface
   * without it.
   *
   * The cause is implicit sharing. `curve_patch_restore_and_restamp()` restores the pre-relief
   * positions and only then calls `CTX_data_ensure_evaluated_depsgraph()`, which is a full scene
   * evaluation rather than a getter: the evaluated mesh ends up SHARING that flat position array.
   * The re-stamp that follows writes through `Mesh::vert_positions_for_write()`, and with the array
   * shared that hands the original a freshly allocated buffer (`attribute_storage_access.cc`),
   * stranding the evaluated mesh on the old flat one. This is the failure #34473 documents, and the
   * remedy is the one `BKE_sculptsession_bm_to_me()` uses for it (`blenkernel/intern/paint.cc`).
   *
   * Tagging alone is not enough here, which is why the evaluation is forced: the interactive commit
   * path gets its republish for free from the ordinary redraw that follows it, but this one runs
   * inside the mode-exit operator and the session is torn down before any redraw can happen.
   *
   * Order matters twice over. This has to run AFTER the frees above: with `ss->cache` still alive
   * `BKE_sculpt_update_object_before_eval()` takes its keep-PBVH branch and re-copies nothing,
   * whereas a null `cache` makes it free the PBVH so the mesh is genuinely re-copied from the
   * original. And the tag has to precede the evaluation, or there is nothing for it to act on. */
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  BKE_mesh_batch_cache_dirty_tag(&mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  CTX_data_ensure_evaluated_depsgraph(&C);

  return committed;
}

void curve_patch_discard_on_session_end(Object &ob)
{
  SculptSession *ss = ob.runtime->sculpt_session;
  if (!ss || !ss->curve_patch_session) {
    return;
  }
  /* Same order as the cancel branch of `curve_patch_edit_finish()`: put the surface back before
   * anything the restore reads is torn down. Safe even for an invalidated patch -- the element-count
   * guard at the top of `curve_patch_restore_only()` turns it into a no-op. */
  curve_patch_restore_only(ob, *ss->curve_patch_session);
  /* Republish the restored surface for the same reason the commit path does -- see the long comment
   * in #curve_patch_commit_on_session_end -- or the discarded relief keeps being drawn in Object
   * Mode. No forced evaluation is needed here, and none is possible without a `bContext`: unlike the
   * commit, nothing evaluates the graph between the restore and the mode exit's own tag, so there is
   * no flat/stale snapshot to undo -- only the tag to place. */
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  BKE_mesh_batch_cache_dirty_tag(&mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  /* The patch took ownership of the anchor stroke's `StrokeCache` (see
   * #curve_patch_begin_editing), so it dies with the session here too. */
  MEM_delete(ss->cache);
  ss->cache = nullptr;
  MEM_delete(ss->curve_patch_session);
  ss->curve_patch_session = nullptr;
}

void curve_patch_finish_commit(const Scene &scene,
                               const Depsgraph &depsgraph,
                               Object &ob,
                               const CurvePatchSession &session)
{
  session.effect->commit(scene, depsgraph, ob, session);
}

void curve_patch_finish_commit(bContext &C, Object &ob, const CurvePatchSession &session)
{
  curve_patch_finish_commit(
      *CTX_data_scene(&C), *CTX_data_depsgraph_pointer(&C), ob, session);

  /* Both commit paths issue their own `flush_update_done(..., UpdateType::Position)` right after
   * calling this, and that call is SHARED with every effect -- but only `UpdateType::Image` reaches
   * the `SPACE_IMAGE` loop in `flush_update_done()`, so with an Image editor open beside the
   * viewport a committed canvas would stay stale there until some unrelated event repainted it.
   * The extra, effect-typed flush is issued here rather than by changing the update type at those
   * two shared call sites.
   *
   * Issuing both is safe: every `UpdateType::Position`-only step (`store_bounds_orig()`,
   * `fake_neighbors_free()`, `BKE_pbvh_bmesh_after_stroke()`) is skipped by the second one, and
   * everything it does do -- clearing `RV3D_PAINTING`, tagging regions for redraw, and tagging the
   * ID when the mesh has linked duplicates or a non-active 3D region cannot draw from the PBVH --
   * is idempotent. It is also distinct from the `flush_update_step()` a re-stamp ends with, which
   * only arms the interactive paint-redraw path for the duration of the session.
   *
   * Skipped when the session wrote nothing: `all_touched_nodes` gains a bit from the first pass
   * that writes anything, so a set with no bit raised means there is nothing to repaint. */
  const UpdateType update_type = session.effect->update_type();
  if (update_type != UpdateType::Position && bits::any_bit_set(session.apply.all_touched_nodes)) {
    flush_update_done(&C, ob, update_type);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session Start
 * \{ */

bool curve_patch_session_publish(Object &ob,
                                 CurvePatchSession &session,
                                 const CurvePatchEffectType effect_type,
                                 PaintModeSettings &paint_mode_settings)
{
  SculptSession &ss = *ob.runtime->sculpt_session;

  /* A fresh session starts with nothing active; the session object may be reused from a previous
   * patch. */
  session.edit.active_point = -1;
  session.reported_frame_cap = false;

  /* Mesh only: the snapshot and `bvhtree_from_mesh_corner_tris_ex` are tied to mesh topology
   * (faces / corner_verts / corner_tris), which CCG does not have. On Grids `surface.ready` stays
   * false and the relief takes the previous single-window path. */
  /* Cleared rather than assumed empty: this session object may be reused from a previous patch, and
   * a stale window set left over from a Mesh object would otherwise still be sampled on a Grids one.
   * The snapshot holds a copy of every vertex position plus a BVH, so it is also worth tens of
   * megabytes on a dense object.
   *
   * One snapshot PER PATCH, because the core build reads it off `CurvePatchGeometry` and the
   * sampler reads its normals back the same way. They all describe the same pristine mesh, so this
   * is redundant work -- bounded by the fact that the interactive editor publishes exactly one
   * patch, and only the headless every-spline path pays for more than one. */
  const Mesh *mesh = bke::object::pbvh_get(ob)->type() == bke::pbvh::Type::Mesh ?
                         BKE_object_get_original_mesh(&ob) :
                         nullptr;
  for (CurvePatchItem &item : session.patches) {
    item.geometry.frames.clear();
    item.geometry.surface.clear();
    if (mesh != nullptr) {
      bke::curve_patch_surface_snapshot_build(*mesh, item.geometry.surface);
    }
  }

  /* Chosen BEFORE the session is published, so a refusal never leaves a live session whose every
   * entry point would dereference a null effect. */
  session.effect = curve_patch_effect_create(effect_type, ob, paint_mode_settings);
  if (!session.effect) {
    return false;
  }

  ss.curve_patch_session = &session;
  session.apply.element_num = session.effect->element_num(ob);
  return true;
}

/* Shared tail of the Curve Patch handoff: freeze brush params, repoint the ViewContext at the
 * session's owned copy, publish the session and stamp the initial preview.
 * `session->patches` must already hold the active item with its control curve fully built
 * (positions, radii, handles); this fills that item's `params`. Used by
 * both the anchor-drag path and the roll-stroke bridge.
 *
 * Returns true when a session was published on the object. The caller is responsible for launching
 * `SCULPT_OT_curve_patch_edit` -- starting a modal from here inverted the layering: the data layer
 * cannot know whether its caller wants an interactive editor at all, which is exactly what a
 * headless apply path needs. */
static bool curve_patch_begin_editing(Object &ob,
                                      const Brush &brush,
                                      const ViewContext &vc,
                                      CurvePatchSession *session,
                                      const float3 &plane_normal,
                                      const float frozen_radius,
                                      PaintModeSettings &paint_mode_settings)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  /* `frozen_radius` is the world-space radius the anchor (or roll) stroke measured; the Size slider
   * that produced it is in pixels. Record the ratio so a later Size change converts back to world
   * units, and so absolute brush jitter (also pixels) can be converted the same way. `cache.paint`
   * is the same `Paint *` the stroke's own init (`SculptPaintStroke::stroke_cache_init()`, via
   * `sd.paint`) resolved for this stroke -- the accessor the rest of the curve-patch modal uses for
   * `BKE_brush_alpha_get()`/`brush_strength()` (see `paint_curve_patch_edit.cc`) is `sd.paint`
   * reached through `CTX_data_tool_settings()`, which is not in scope here; `cache.paint` is the
   * equivalent already sitting on the `StrokeCache` this function has. */
  const int brush_size = BKE_brush_size_get(cache.paint, &brush);
  const float radius_per_size = brush_size > 0 ? frozen_radius / float(brush_size) : 0.0f;
  /* Rolled once, then frozen -- see `CurvePatchParams::stamp_seed`. This is the only place a real
   * RNG is touched; everything downstream hashes this seed. */
  const uint32_t stamp_seed = RandomNumberGenerator::from_random_seed().get_uint32();
  session->active_item().params = curve_patch_params_from_brush(brush,
                                                                frozen_radius,
                                                                radius_per_size,
                                                                plane_normal,
                                                                stamp_seed,
                                                                brush.curve_patch.swap_axis != 0);

  /* `cache.vc` otherwise still points at the just-finished stroke's own `ViewContext`, torn down
   * together with that stroke's operator. Repoint it at this session's owned copy so every
   * re-stamp's `calc_local_from_screen()` (via `cache->vc`) dereferences valid memory for the whole
   * lifetime of the patch (see `CurvePatchSession::view_context`). */
  session->view_context = vc;
  cache.vc = &session->view_context;

  /* Stage 1's brush gate should have refused this brush long before a session was started; refuse
   * defensively rather than publishing a session with no effect. Both callers hand this function
   * ownership of `ss.cache` (see their doc comments), so the bail frees it the same way their own
   * Dynamic Topology refusals do -- #curve_patch_session_publish deliberately frees nothing. */
  const std::optional<CurvePatchEffectType> effect_type = curve_patch_effect_type_for_brush(
      brush, ob, paint_mode_settings);
  if (!effect_type ||
      !curve_patch_session_publish(ob, *session, *effect_type, paint_mode_settings))
  {
    MEM_delete(session);
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    return false;
  }

  /* Stamp the initial curve right away: neither `curve_patch_edit_invoke()` nor
   * `curve_patch_edit_modal()` re-stamps on its own until the user performs a first edit, so without
   * this the mesh would sit pristine with no visible feedback at all until then. */
  curve_patch_restore_and_restamp(*vc.C, ob, *session);

  /* This re-stamp runs nested inside the just-finishing stroke's `PaintStroke::done()`, which clears
   * `RV3D_PAINTING` the instant this call chain returns -- so the fast paint-redraw path
   * `flush_update_step()` set up is torn down before the viewport redraws. Issue the full
   * finished-stroke redraw (tags every viewport and refreshes bounds independently of
   * `RV3D_PAINTING`) so this first preview reaches the screen immediately. */
  flush_update_done(vc.C, ob, UpdateType::Position);

  return true;
}

bool curve_patch_start_from_anchor(const Depsgraph &depsgraph,
                                   Object &ob,
                                   Sculpt &sd,
                                   const Brush &brush,
                                   const ViewContext &vc)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  /* The anchor stroke's last step already stamped one dab onto the real mesh at the release
   * location. `ReliefEffect::orig_positions_` below only snapshots a vertex's position the
   * first time THIS patch's own re-stamp touches it, so if that vertex is still displaced by the
   * anchor dab right now, the patch would adopt the displaced position as "original" and could
   * never restore past it -- moving curve points would only ever add on top of that permanently
   * baked-in bump instead of describing the whole shape from a clean baseline. Restore the mesh
   * to its pristine pre-stroke state before anything else touches it, while
   * `ss.curve_patch_session` is still null (see the matching branch in
   * `restore_from_undo_step_if_necessary()`, `mesh/sculpt.cc`). */
  restore_from_undo_step_if_necessary(depsgraph, sd, ob);

  /* Dynamic Topology has no stable per-vertex index across recomputes (`BMVert`s themselves are
   * created/destroyed as topology changes), which `ReliefEffect::orig_positions_` and the
   * relief re-stamp in `ReliefEffect::apply_pass()` both depend on. Refuse rather than
   * crashing deep inside the first re-stamp -- mirrors the "refuse + `BKE_report`" idiom already
   * used for the 2-point-minimum case in `paint_curve_patch_edit.cc`. The anchor dab is already
   * undone by `restore_from_undo_step_if_necessary()` above, so bailing here leaves the mesh as if
   * the anchor stroke never happened; `ss.cache` ownership was handed to this function by the
   * caller (see this function's own doc comment), so it must be freed here too. */
  if (bke::object::pbvh_get(ob)->type() == bke::pbvh::Type::BMesh) {
    BKE_report(
        CTX_wm_reports(vc.C), RPT_WARNING, "Curve Patch does not support Dynamic Topology");
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    return false;
  }

  auto *session = MEM_new<CurvePatchSession>(__func__);
  session->patches.resize(1);
  session->active_patch = 0;

  float3 plane_normal = cache.sculpt_normal;
  if (const PaintCurve *paint_curve = brush.paint_curve) {
    bke::CurvesGeometry control_curve = ED_paintcurve_control_curve_for_patch(*paint_curve, -1);
    if (control_curve.points_num() >= 2) {
      session->patches[0].control_curve = std::move(control_curve);
      plane_normal = curve_patch_plane_normal_from_curve(session->patches[0].control_curve);
    }
  }

  if (session->patches[0].control_curve.points_num() < 2) {
    /* The anchor click-to-release screen distance is often just a handful of pixels -- or zero,
     * for a plain click -- which used to seed a control curve far too short to be a useful starting
     * shape, and its direction (the raw drag vector) did not match the brush's texture Angle at all.
     * Instead orient the starting segment along the brush texture Angle (`MTex::rot`) and always span
     * exactly one brush diameter along it, so every anchor stroke starts from a usable, editable
     * two-point curve that lines up with the angle the user set in the brush's texture settings.
     *
     * `MTex::rot` is a screen-space rotation, so convert it into an in-plane object-space direction
     * the same way the brush's own `calc_brush_local_mat()` does: build a screen-space unit vector at
     * the angle, unproject it to a world-space delta at the anchor's depth, rotate that into object
     * space, then flatten it into the anchor plane.
     *
     * The extra -90 degrees aligns the curve with the brush texture's own orientation: `MTex::rot`
     * defines the texture's X (transverse) axis, whereas the curve runs along the strip's LENGTH,
     * which is that axis rotated a quarter turn (matching `calc_brush_local_mat()`'s motion axis
     * being the 90-degree rotation of `[cos(rot), sin(rot)]`). */
    const float angle = brush.mtex.rot + float(M_PI_2);
    const float screen_dir[2] = {std::cos(angle), std::sin(angle)};
    const float3 loc_world = math::transform_point(ob.object_to_world(), cache.initial_location);
    const float zfac = ED_view3d_calc_zfac(vc.rv3d, loc_world);
    float3 dir_world;
    ED_view3d_win_to_delta(vc.region, screen_dir, zfac, dir_world);
    float3 direction = math::transform_direction(ob.world_to_object(), dir_world);
    direction -= cache.sculpt_normal * math::dot(direction, cache.sculpt_normal);
    if (math::length_squared(direction) > 1e-12f) {
      direction = math::normalize(direction);
    }
    else {
      /* The angle direction happened to point straight along the surface normal (a near-edge-on
       * view): fall back to any in-plane direction so the curve is still usable. */
      direction = math::normalize(math::cross(cache.sculpt_normal, cache.view_normal));
      if (math::length_squared(direction) < 1e-12f) {
        direction = float3(1.0f, 0.0f, 0.0f);
      }
    }
    /* `std::array`, not a raw C array: `blender::Span` has no constructor for the latter. */
    const std::array<float3, 2> anchor_positions = {
        cache.initial_location - direction * cache.initial_radius,
        cache.initial_location + direction * cache.initial_radius};
    const std::array<float, 2> anchor_radii = {1.0f, 1.0f};
    session->patches[0].control_curve = curve_patch_control_curve_from_points(
        anchor_positions, anchor_radii, /*cyclic=*/false);
  }

  ToolSettings *tool_settings = CTX_data_tool_settings(vc.C);
  return curve_patch_begin_editing(
      ob, brush, vc, session, plane_normal, cache.initial_radius, tool_settings->paint_mode);
}

bool roll_start_curve_patch_from_stroke(const Depsgraph &depsgraph,
                                        Object &ob,
                                        Sculpt & /*sd*/,
                                        const Brush &brush,
                                        const ViewContext &vc,
                                        const Span<float3> control_positions,
                                        const Span<float> control_radii,
                                        const float3 &plane_normal)
{
  BLI_assert(control_positions.size() == control_radii.size());
  SculptSession &ss = *ob.runtime->sculpt_session;

  /* Dynamic Topology has no stable per-vertex index for `ReliefEffect::orig_positions_`; refuse
   * (mirrors `curve_patch_start_from_anchor()`). Ownership of `ss.cache` was handed to us by the
   * caller, so free it here on every bail path. Checked before the restore below so an unsupported
   * object is left untouched. */
  if (bke::object::pbvh_get(ob)->type() == bke::pbvh::Type::BMesh) {
    BKE_report(
        CTX_wm_reports(vc.C), RPT_WARNING, "Curve Patch does not support Dynamic Topology");
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    return false;
  }

  if (control_positions.size() < 2) {
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    return false;
  }

  /* Undo the entire live roll relief back to pristine so the Curve Patch re-stamp starts from a
   * clean baseline (its `orig_positions` snapshot must not capture already-displaced vertices).
   * Unlike Curve Patch's single anchor dab, roll is an additive multi-dab stroke, so use the full
   * position restore (the default case of the internal per-brush `restore_from_undo_step()`),
   * not the per-dab `restore_from_undo_step_if_necessary()` which does nothing for the roll stroke
   * method. */
  undo::restore_position_from_undo_step(depsgraph, ob);
  bke::pbvh::update_normals(depsgraph, ob, *bke::object::pbvh_get(ob));

  auto *session = MEM_new<CurvePatchSession>(__func__);
  session->patches.resize(1);
  session->active_patch = 0;
  session->patches[0].control_curve = curve_patch_control_curve_from_points(
      control_positions, control_radii, /*cyclic=*/false);

  const StrokeCache &cache = *ss.cache;

  /* Fall back to the stroke's surface/view normal if the roll never froze a projection normal
   * (a stroke too short to paint a single deferred dab). Curve Patch needs a unit plane normal for
   * its lateral decomposition. */
  float3 plane = plane_normal;
  if (math::length_squared(plane) < 1e-8f) {
    plane = (math::length_squared(cache.sculpt_normal) > 1e-8f) ? cache.sculpt_normal :
                                                                  cache.view_normal;
  }
  ToolSettings *tool_settings = CTX_data_tool_settings(vc.C);
  return curve_patch_begin_editing(ob,
                                   brush,
                                   vc,
                                   session,
                                   math::normalize(plane),
                                   cache.initial_radius,
                                   tool_settings->paint_mode);
}

/** \} */

}  // namespace blender::ed::sculpt_paint

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Public Read-Only View of a Running Session
 *
 * Declared in `ED_paint.hh`, which puts the `ED_*` API in `blender` rather than in this file's own
 * `blender::ed::sculpt_paint` -- hence the separate namespace block. The handle is `const void *`
 * so that the RNA layer, which cannot include this module's private header, still has something to
 * hold; every accessor casts it back and tolerates null.
 * \{ */

static const ed::sculpt_paint::CurvePatchSession *session_from_handle(const void *session)
{
  return static_cast<const ed::sculpt_paint::CurvePatchSession *>(session);
}

const void *ED_curve_patch_session_get(const Object &ob)
{
  const SculptSession *ss = ob.runtime ? ob.runtime->sculpt_session : nullptr;
  return ss != nullptr ? ss->curve_patch_session : nullptr;
}

/* Every accessor below reports the ACTIVE patch, which is what a script asking "the running Curve
 * Patch" means: the one the modal editor is acting on. Null when the session is half-built and has
 * no active patch yet. */
static const ed::sculpt_paint::CurvePatchItem *active_item_from_handle(const void *session)
{
  const ed::sculpt_paint::CurvePatchSession *patch = session_from_handle(session);
  if (patch == nullptr || !patch->has_active_item()) {
    return nullptr;
  }
  return &patch->active_item();
}

int ED_curve_patch_session_point_num(const void *session)
{
  const ed::sculpt_paint::CurvePatchItem *item = active_item_from_handle(session);
  return item != nullptr ? item->control_curve.points_num() : 0;
}

int ED_curve_patch_session_active_point(const void *session)
{
  const ed::sculpt_paint::CurvePatchSession *patch = session_from_handle(session);
  const ed::sculpt_paint::CurvePatchItem *item = active_item_from_handle(session);
  if (item == nullptr) {
    return -1;
  }
  /* Validated here rather than trusted: the index outlives the modal that set it, and is only
   * reset when a session ends or a new control curve is built. */
  const int point_num = item->control_curve.points_num();
  return patch->edit.active_point < point_num ? patch->edit.active_point : -1;
}

bool ED_curve_patch_session_is_cyclic(const void *session)
{
  const ed::sculpt_paint::CurvePatchItem *item = active_item_from_handle(session);
  if (item == nullptr || item->control_curve.curves_num() == 0) {
    return false;
  }
  return item->control_curve.cyclic()[0];
}

float ED_curve_patch_session_radius(const void *session)
{
  const ed::sculpt_paint::CurvePatchItem *item = active_item_from_handle(session);
  return item != nullptr ? item->params.radius : 0.0f;
}

int ED_curve_patch_session_stamp_num(const void *session)
{
  const ed::sculpt_paint::CurvePatchItem *item = active_item_from_handle(session);
  return item != nullptr ? int(item->geometry.stamps.size()) : 0;
}

Span<float3> ED_curve_patch_session_positions(const void *session)
{
  const ed::sculpt_paint::CurvePatchItem *item = active_item_from_handle(session);
  return item != nullptr ? item->control_curve.positions() : Span<float3>();
}

/** \} */

}  // namespace blender
