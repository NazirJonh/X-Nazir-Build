/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

#include "paint_curve_patch_cache.hh"

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
#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_time.h"

#include "MEM_guardedalloc.h"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"

#include "ED_view3d.hh"

#include "paint_curve_intern.hh"

#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"

/* Temporary performance instrumentation (see previous measurement pass). Times each
 * `curve_patch_restore_and_restamp()` to confirm the C3 node-cull's effect on `relief`. Set to 0 to
 * disable; grep `DEBUG-cpatch` to remove every touch point once measured. */
#define CURVE_PATCH_PROFILING 1

/* TEMPORARY diagnostic for the seam at a surface fold. Forces the wrap to a SINGLE window while
 * leaving the shrinkwrap, the smoothed normal field and the smoothed-binormal ribbon fully active,
 * so the window join is the only variable removed. Seam gone -> the join produces it; seam still
 * there -> it comes from the shrinkwrap or the ribbon, and the join is innocent. Set back to 0
 * once measured; grep `FORCE_SINGLE_FRAME` to remove every touch point. */
#define CURVE_PATCH_FORCE_SINGLE_FRAME 0

namespace blender::ed::sculpt_paint {

const bke::CurvesGeometry *ED_paint_curve_patch_active_control_curve(const Object *ob)
{
  if (ob == nullptr || ob->runtime->sculpt_session == nullptr ||
      ob->runtime->sculpt_session->curve_patch_cache == nullptr)
  {
    return nullptr;
  }
  return &ob->runtime->sculpt_session->curve_patch_cache->control_curve;
}

void curve_patch_restore_only(Object &ob, const CurvePatchCache &patch)
{
  patch.effect->restore(ob, patch);
}

bool curve_patch_commit_on_session_end(bContext &C, Object &ob)
{
  SculptSession *ss = ob.runtime->sculpt_session;
  if (!ss || !ss->curve_patch_cache) {
    return false;
  }
  CurvePatchCache *patch = ss->curve_patch_cache;

  /* Mirrors the commit branch of `curve_patch_edit_finish()` (`paint_curve_patch_edit.cc`), which
   * cannot be reused directly because it is tied to the modal operator's `customdata`. The modal is
   * left running; it notices the freed cache on its next event and tears its own state down through
   * its liveness guard. Keep the two in step when either changes. */
  patch->final_quality = true;
  curve_patch_restore_and_restamp(C, ob, *patch);
  patch->final_quality = false;

  bool committed = false;
  if (patch->invalidated) {
    /* A foreign operator changed the element count, so nothing can be written -- and
     * `curve_patch_restore_only()` is a no-op in this state, leaving the mesh as that operator left
     * it. Same outcome the modal reports as canceled. */
    curve_patch_restore_only(ob, *patch);
  }
  else {
    curve_patch_finish_commit(C, ob, *patch);
    flush_update_done(&C, ob, UpdateType::Position);
    committed = true;
  }

  MEM_delete(ss->cache);
  ss->cache = nullptr;
  MEM_delete(patch);
  ss->curve_patch_cache = nullptr;

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
  if (!ss || !ss->curve_patch_cache) {
    return;
  }
  /* Same order as the cancel branch of `curve_patch_edit_finish()`: put the surface back before
   * anything the restore reads is torn down. Safe even for an invalidated patch -- the element-count
   * guard at the top of `curve_patch_restore_only()` turns it into a no-op. */
  curve_patch_restore_only(ob, *ss->curve_patch_cache);
  /* Republish the restored surface for the same reason the commit path does -- see the long comment
   * in #curve_patch_commit_on_session_end -- or the discarded relief keeps being drawn in Object
   * Mode. No forced evaluation is needed here, and none is possible without a `bContext`: unlike the
   * commit, nothing evaluates the graph between the restore and the mode exit's own tag, so there is
   * no flat/stale snapshot to undo -- only the tag to place. */
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  BKE_mesh_batch_cache_dirty_tag(&mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  /* The patch took ownership of the anchor stroke's `StrokeCache` (see
   * `curve_patch_publish_and_launch_modal()`), so it dies with the patch here too. */
  MEM_delete(ss->cache);
  ss->cache = nullptr;
  MEM_delete(ss->curve_patch_cache);
  ss->curve_patch_cache = nullptr;
}

void curve_patch_finish_commit(bContext &C, Object &ob, const CurvePatchCache &patch)
{
  patch.effect->commit(C, ob, patch);
}

/* Width of the normal-field smoothing. It arbitrates "`u` stays continuous" against "the strip hugs
 * the edge": wider means a smoother texture on an oblique crossing and a looser fit. A fraction of
 * the radius rather than a world-space constant, so the behavior does not depend on scene scale. */
static float curve_patch_smooth_length(const CurvePatchCache &patch)
{
  return patch.frozen_params.radius * (patch.final_quality ? 0.5f : 0.8f);
}

/** Matches the raw `BrushActionFunc` signature `do_symmetrical_brush_actions()` expects
 * (`mesh/sculpt_intern.hh:855`) so it can be passed as its `action` callback. Called once per
 * enabled symmetry pass (mirror x radial x tile) with `StrokeCache::location_symm`/`radius`
 * already set (by `cache_calc_brushdata_symm()`, invoked internally by
 * `do_symmetrical_brush_actions()`) to the whole curve's encompassing sphere in that pass's
 * transformed space. Unlike a normal brush dab, this forwards straight to the session's effect --
 * no `sculpt_brush_type` dispatch, no `do_brush_action()` call. */
static void curve_patch_apply_effect_action(const Depsgraph &depsgraph,
                                            const Scene & /*scene*/,
                                            Sculpt & /*sd*/,
                                            Object &ob,
                                            const Brush &brush,
                                            PaintModeSettings & /*paint_mode_settings*/)
{
  CurvePatchCache &patch = *ob.runtime->sculpt_session->curve_patch_cache;
  patch.effect->apply_pass(depsgraph, ob, brush, patch);
}

void curve_patch_restore_and_restamp(bContext &C, Object &ob, CurvePatchCache &patch)
{
  if (patch.effect->element_num(ob) != patch.element_num) {
    /* Something retopologized the object while the patch was live -- see
     * `CurvePatchCache::element_num`. Bail before touching positions: the snapshot's keys no
     * longer name the elements they were taken from. */
    patch.invalidated = true;
    BKE_report(CTX_wm_reports(&C),
               RPT_WARNING,
               "Curve Patch canceled: the mesh changed while it was being edited");
    return;
  }

#if CURVE_PATCH_PROFILING
  const double prof_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  curve_patch_restore_only(ob, patch);
#if CURVE_PATCH_PROFILING
  const double prof_t_restore = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif

  /* Tessellate the control curve the same way `PaintStroke::curve_end()` does for its own
   * bezier-curve tessellation, then rebuild the arc-length spline used to place dabs -- now also
   * carrying the per-point `radius` attribute, interpolated to the same evaluated resolution. */
  const bke::CurvesGeometry &geom = patch.control_curve;
  patch.spline.plane_normal = patch.plane_normal;

  geom.ensure_can_interpolate_to_evaluated();
  Array<float> evaluated_radii(geom.evaluated_points_num());
  geom.interpolate_to_evaluated(VArraySpan(geom.radius()), evaluated_radii.as_mutable_span());
  /* `control_curve` is always a single spline (see `paintcurve_geometry_init_bezier()`), so curve 0
   * carries the whole patch's cyclic state. */
  const bool cyclic = geom.curves_num() > 0 && geom.cyclic()[0];

  /* The polyline is pulled onto the pristine surface BEFORE the spline is built: arc lengths and
   * tangents are computed inside `build_from_positions`, so shifting the points afterwards would
   * leave them describing the previous curve, the one still hovering above the mesh. */
  Array<float3> evaluated_positions(geom.evaluated_positions());
  Array<float3> evaluated_normals(evaluated_positions.size(), float3(0.0f));
  if (patch.surface.ready) {
    curve_patch_surface_shrinkwrap(patch.surface,
                                   patch.frozen_params.radius,
                                   evaluated_positions.as_mutable_span(),
                                   evaluated_normals.as_mutable_span());
    curve_patch_surface_fill_invalid_normals(evaluated_normals.as_mutable_span(),
                                             patch.plane_normal);
  }
  patch.spline.build_from_positions(evaluated_positions.as_span(),
                                    evaluated_radii.as_span(),
                                    cyclic,
                                    patch.surface.ready ? evaluated_normals.as_span() :
                                                          Span<float3>());

  if (patch.spline.is_empty()) {
    return;
  }

  /* Hoisted ahead of its previous spot (just below the ribbon build) so the stamps block below can
   * reach the brush through `cache.paint` without a second, redundant `CTX_data_tool_settings()`
   * fetch. Both are side-effect-free reference binds and nothing between the two positions can
   * leave `ss.cache` null, so the guards they now sit above were never protecting them. */
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  /* Stamps mode lays its stamps out here, on the main thread, right after the spline: PHASE 1's
   * parallel per-vertex walk only reads the result. Ribbon mode leaves the list empty. */
  patch.stamps.clear();
  /* Recorded on the cache (not a local) because the relief action needs it too -- see
   * `CurvePatchCache::ribbon_radius`. Set unconditionally here, so Ribbon mode and the
   * no-brush fall-back below both leave it at the unwidened frozen radius. */
  patch.ribbon_radius = patch.frozen_params.radius;
  /* Likewise unconditional, so Ribbon mode and the no-brush fall-back leave the strip unextended
   * and therefore bit-for-bit what it was before Stamps mode existed. */
  patch.ribbon_end_margin = 0.0f;
  /* Likewise unconditional: `stamp_mode` is live-synced per restamp, so without this a Stamps ->
   * Ribbon toggle mid-edit would leave the bound holding a stale value from the last Stamps-mode
   * restamp instead of the "no stamps to bound" state Ribbon mode actually has. */
  patch.stamp_search_reach = 0.0f;

  /* Resolve which textures this restamp samples. Done here, once, on the main thread: PHASE 1's
   * parallel per-vertex walk only reads the result. */
  patch.stamp_texture_variants.reinitialize(0);
  patch.stamp_texture_weights_cdf.clear();
  /* Element-wise rather than `std::array::fill()`: `MTex` deletes its copy assignment (see
   * #DNA_DEFINE_CXX_METHODS), so DNA's explicit shallow-copy path is the only way to write one. */
  for (MTex &zone_variant : patch.ribbon_zone_variants) {
    zone_variant = dna::shallow_zero_initialize();
  }
  patch.caps_enabled = false;
  patch.world_cap_start = 0.0f;
  patch.world_cap_end = 0.0f;
  if (const Brush *tex_brush = BKE_paint_brush_for_read(cache.paint)) {
    if (tex_brush->mtex.curve_patch_stamp_texture_source == MTEX_CURVE_PATCH_TEX_MULTI) {
      /* Sized up front because `Array` cannot grow -- the container is fixed-size precisely because
       * `MTex` has no move constructor for a growing one to relocate through. */
      const int slot_num = tex_brush->curve_patch_texture_slots.count();
      patch.stamp_texture_variants.reinitialize(slot_num);
      patch.stamp_texture_weights_cdf.reserve(slot_num);
      float running = 0.0f;
      int slot_index = 0;
      /* Range-for, NOT `LISTBASE_FOREACH`: that macro is private to `listbase.cc` in this branch.
       * `ListBaseT<T>` iterates directly -- see `for (PaletteColor &color : palette->colors)` in
       * `blenkernel/intern/paint.cc`. */
      for (const BrushCurvePatchTextureSlot &slot : tex_brush->curve_patch_texture_slots) {
        /* `dna::shallow_copy()` rather than plain assignment: `MTex` deletes its copy assignment so
         * that DNA structs cannot be duplicated without opting into a shallow (non-owning) copy,
         * which is exactly what is wanted here -- only `tex` differs between variants. */
        MTex &variant = patch.stamp_texture_variants[slot_index];
        variant = dna::shallow_copy(tex_brush->mtex);
        variant.tex = slot.tex;
        /* Negative weights would make the table decrease and break the search; clamp rather than
         * reject, so a Python-set value cannot corrupt the layout. */
        running += std::max(slot.weight, 0.0f);
        patch.stamp_texture_weights_cdf.append(running);
        slot_index++;
      }
    }
    if (tex_brush->mtex.curve_patch_ribbon_texture_source == MTEX_CURVE_PATCH_TEX_MULTI) {
      patch.caps_enabled = true;
      const Tex *zone_textures[3] = {tex_brush->curve_patch_tex_start,
                                     tex_brush->curve_patch_tex_middle,
                                     tex_brush->curve_patch_tex_end};
      for (const int i : IndexRange(3)) {
        patch.ribbon_zone_variants[i] = dna::shallow_copy(tex_brush->mtex);
        patch.ribbon_zone_variants[i].tex = const_cast<Tex *>(zone_textures[i]);
      }
      /* The UI stores cap lengths in brush DIAMETERS while `radius` is the ribbon's half-width,
       * hence the factor of two. The BASE radius, not the per-point one: a zone boundary that moved
       * with `radius_at_s` would not be a boundary at all. */
      patch.world_cap_start = tex_brush->curve_patch_cap_start_length * 2.0f *
                              patch.frozen_params.radius;
      patch.world_cap_end = tex_brush->curve_patch_cap_end_length * 2.0f *
                            patch.frozen_params.radius;
    }
  }

  if (patch.frozen_params.stamp_mode == MTEX_CURVE_PATCH_STAMP_STAMPS) {
    const Brush *stamp_brush = BKE_paint_brush_for_read(cache.paint);
    if (stamp_brush != nullptr) {
      /* Blender stores Spacing as a percentage of the brush DIAMETER, the same convention a normal
       * dab stroke uses, so the stamps land at the same density a freehand stroke of this brush
       * would produce. */
      const float spacing_frac = float(stamp_brush->spacing) / 100.0f;
      /* Relative jitter is already a fraction of the radius, so it converts to world units for
       * free. Absolute jitter is in screen pixels and needs the ratio captured at patch start --
       * `paint_stroke_jitter_pos()` cannot be reused here because it works in screen space while
       * the ribbon's UV is world-space. `brush.curve_jitter`, which modulates jitter over stroke
       * time, has no meaning for a patch: a patch has no stroke timeline. */
      const float jitter_amount = (stamp_brush->flag & BRUSH_ABSOLUTE_JITTER) ?
                                      stamp_brush->jitter_absolute *
                                          patch.frozen_params.radius_per_size :
                                      stamp_brush->jitter * patch.frozen_params.radius;
      curve_patch_stamps_build(patch.spline,
                               patch.frozen_params.radius,
                               spacing_frac,
                               jitter_amount,
                               patch.frozen_params.stamp_size_random,
                               patch.frozen_params.stamp_strength_random,
                               stamp_brush->mtex.rot,
                               stamp_brush->mtex.random_angle,
                               patch.frozen_params.stamp_seed,
                               patch.stamp_texture_weights_cdf,
                               patch.stamps);
      /* PLANAR tests candidate vertices against a rigid WORLD-space frame, but this reach is still
       * an ARC-LENGTH window. On a bend the chord is shorter than the arc, so a vertex inside a
       * stamp's world square can have an `s` outside this window and get silently clipped. The
       * arc/chord ratio for a circular bend of turn angle theta across the stamp's reach is
       * `(theta/2) / sin(theta/2)`; this bound is sized for turns up to a 180-degree hairpin, where
       * the ratio reaches `PI / 2 ~= 1.571`, and 1.6 rounds that up. A curve that spirals tighter
       * than a half-turn within roughly one stamp's reach exceeds what this bound was designed for
       * and is out of scope here. The bound only has to be conservative within that scope: the
       * per-stamp test in the candidate loop below is exact, so an over-wide window just costs a
       * few extra candidates, while a too-narrow one silently clips stamps. */
      constexpr float PLANAR_BEND_SLACK = 1.6f;
      /* Resolve the one bound every consumer below shares. On top of the bend slack above, PLANAR
       * also adds `jitter_amount`: a stamp pushed sideways off the curve keeps a rigid frame, so
       * its square spans more arc length than its own corner reach accounts for. CURVE takes
       * neither term and keeps the historical value, so that projection is unaffected. */
      patch.stamp_search_reach = curve_patch_stamp_reach(patch.frozen_params.radius);
      if (patch.frozen_params.stamp_projection == MTEX_CURVE_PATCH_STAMP_PROJ_PLANAR) {
        patch.stamp_search_reach = patch.stamp_search_reach * PLANAR_BEND_SLACK + jitter_amount;
      }
      /* A closed curve has no ends to extend (see `ribbon_end_margin` below), so the stamp at the
       * join would lose the half that reaches into `v < 0` -- which on a loop is not outside the
       * curve but the stretch just before the join. Wrap those stamps around instead, so both
       * halves are present and meet exactly at the seam. The bound must be the same one the
       * per-vertex search window uses, hence the shared `stamp_search_reach`. */
      if (patch.spline.cyclic) {
        curve_patch_stamps_add_cyclic_wrap(
            patch.stamps, patch.spline.total_length(), patch.stamp_search_reach);
      }
      /* Stamps pushed sideways by jitter would fall outside the ribbon and be clipped by the LUT's
       * edge, so the strip has to cover the widest possible excursion. Only jitter needs this: the
       * size randomization shrinks stamps and never grows them. The widened value flows into
       * `ribbon_source_hash()` as the `brush_radius` argument, so the cached LUT invalidates
       * correctly with no extra hashing. */
      patch.ribbon_radius += jitter_amount;
      /* The layout puts the first stamp's center exactly at `s == 0` and the last one at the last
       * whole step before `total_length`, so an end stamp reaches past the curve's end by its own
       * half-extent -- and the strip, which used to stop dead at that end, gave the overhanging
       * half no UV at all and clipped it along a hard straight edge. Extend the strip by the
       * farthest such reach instead of insetting the stamps, which would leave the ends of the
       * curve visibly bare.
       *
       * #curve_patch_stamp_reach is a stamp's corner reach; `jitter_amount` covers a center jittered
       * further along the curve. Deliberately NOT `stamp_search_reach`: that bound also carries
       * `PLANAR_BEND_SLACK`, which exists to cover a stamp's arc/chord gap on a BEND -- but an end
       * stamp's overhang is not tested against a bend, it is rendered by the ribbon's own straight
       * extrapolation along the end tangent (see `curve_patch_stamps_build()`'s PLANAR frame
       * extrapolation). Slack bought there would only inflate the strip, the PBVH cull tube, and the
       * whole-curve search sphere for no correctness gain, and it would force a full LUT rebuild on
       * every CURVE<->PLANAR toggle since `end_margin` feeds the ribbon's source hash. */
      patch.ribbon_end_margin = curve_patch_stamp_reach(patch.frozen_params.radius) + jitter_amount;
    }
  }

  /* Rebuild the ribbon UV LUT the relief action samples in place of
   * `CurvePatchSpline::closest_point()` (see `paint_curve_patch_ribbon.hh`). Built once per
   * restamp on the main thread; PHASE 1's parallel `compute_vertex()` walk only reads it. */
  if (patch.surface.ready) {
    curve_patch_spline_smooth_normals(patch.spline, curve_patch_smooth_length(patch));
    CurvePatchFramesParams frame_params;
    frame_params.min_window_length = 2.0f * patch.frozen_params.radius;
    frame_params.turn_threshold_rad = patch.final_quality ? float(M_PI) * 12.0f / 180.0f :
                                                            float(M_PI) * 25.0f / 180.0f;
    frame_params.break_threshold_rad = float(M_PI) * 60.0f / 180.0f;
    /* Half a brush radius of shared stretch on each side of an interior join. Enough that the
     * handover happens well inside both windows' tables rather than on their outermost rows, and
     * short enough that a window crossing a break does not rasterize a long stretch of the other
     * face nearly edge-on. */
    frame_params.overlap_length = 0.5f * patch.frozen_params.radius;
#if CURVE_PATCH_FORCE_SINGLE_FRAME
    /* FORCE_SINGLE_FRAME: see the note at the top of this file. */
    frame_params.max_frames = 1;
#else
    frame_params.max_frames = CURVE_PATCH_MAX_FRAMES;
#endif
    curve_patch_frames_build(patch.spline,
                             patch.ribbon_radius,
                             frame_params,
                             patch.final_quality,
                             patch.ribbon_end_margin,
                             patch.frames);
    /* A separate channel, NOT `CURVE_PATCH_PROFILING`: that one is marked in its own comment as
     * temporary instrumentation and would take the visibility of a quality degradation with it on
     * the first cleanup. */
    if (patch.frames.capped && !patch.reported_frame_cap) {
      patch.reported_frame_cap = true;
      BKE_report(CTX_wm_reports(&C),
                 RPT_WARNING,
                 "Curve Patch: curve too complex for full surface wrap, quality reduced");
    }
  }
  else {
    curve_patch_ribbon_build(patch.spline,
                             patch.ribbon_radius,
                             patch.ribbon,
                             patch.final_quality,
                             patch.ribbon_end_margin,
                             patch.ribbon_end_margin);
  }
#if CURVE_PATCH_PROFILING
  const double prof_t_ribbon = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  if (patch.surface.ready ? !patch.frames.ready : !patch.ribbon.ready) {
    return;
  }

  ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  Sculpt &sd = *tool_settings->sculpt;
  PaintModeSettings &paint_mode_settings = tool_settings->paint_mode;
  Scene *scene = CTX_data_scene(&C);
  /* Deliberately the plain pointer, NOT `CTX_data_ensure_evaluated_depsgraph()`, which is a full
   * `BKE_scene_graph_update_tagged()` rather than a getter. The two consumers below are the same
   * ones an ordinary sculpt dab feeds, and `SculptPaintStroke::update_step()` (`mesh/sculpt.cc`)
   * hands them a depsgraph captured once at stroke start -- sculpt's only `ensure_evaluated` is the
   * one-off at stroke START. Running a scene evaluation per re-stamp is both needless and costly
   * here: `flush_update_step()` at the end of every re-stamp tags `ID_RECALC_SHADING`
   * unconditionally, so the next re-stamp's `ensure` always found pending work and paid for a full
   * evaluation on every frame of a point drag.
   *
   * It was also actively harmful. That evaluation ran between `curve_patch_restore_only()` above and
   * the re-stamp below -- with the mesh at its pre-relief positions -- leaving the evaluated mesh
   * sharing the flat position array, which the re-stamp's `vert_positions_for_write()` then stranded
   * (see the long comment in #curve_patch_commit_on_session_end). */
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(&C);

  /* Effect input preparation, once per restamp and before the first symmetry pass. Relief refreshes
   * the vertex normals here; see `ReliefEffect::begin_restamp()` for why that is needed at all. */
#if CURVE_PATCH_PROFILING
  const double prof_t_pre_norm = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  patch.effect->begin_restamp(depsgraph, ob, patch);
#if CURVE_PATCH_PROFILING
  const double prof_t_norm = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif

  /* Whole-curve encompassing sphere, in canonical (non-mirrored) object space -- lets
   * `do_symmetrical_brush_actions()`'s existing per-pass node-mask query (driven by
   * `cache.location_symm`/`cache.radius`, set below via `cache.location`/`cache.radius` and
   * transformed per pass by `cache_calc_brushdata_symm()`) cover the WHOLE curve for the relief
   * action above, instead of one small per-dab circle. Conservative bound: any point within
   * `max_radius` of any point on the curve is within `(bbox half-diagonal + max_radius)` of the
   * bbox center, by the triangle inequality. */
  float3 bbox_min = patch.spline.poly_3d[0];
  float3 bbox_max = patch.spline.poly_3d[0];
  for (const float3 &p : patch.spline.poly_3d) {
    bbox_min = math::min(bbox_min, p);
    bbox_max = math::max(bbox_max, p);
  }
  const float3 bbox_center = (bbox_min + bbox_max) * 0.5f;
  /* `evaluated_radii` entries are the unitless per-point UI scalar (see the matching comment in
   * `ReliefEffect::apply_pass()`); scale by `ribbon_radius` here too so this search sphere
   * stays a genuine superset of the world-space reach actually used below -- including the extra
   * `jitter_amount` a Stamps-mode stamp can be pushed out to. Equals `frozen_params.radius` in
   * Ribbon mode. */
  float max_radius = 0.0f;
  for (const float r : evaluated_radii) {
    max_radius = std::max(max_radius, r);
  }
  max_radius *= patch.ribbon_radius;
  cache.location = bbox_center;
  /* `ribbon_end_margin` extends the strip along the end tangents, i.e. up to that far outside the
   * bbox built from the curve's own points, so the sphere has to grow by it as well -- it feeds the
   * node-mask query the cull tube above then narrows, and a node missing from it is never offered
   * to the cull at all. 0 in Ribbon mode, leaving the sphere exactly as it was. */
  cache.radius = math::distance(bbox_max, bbox_center) + max_radius + patch.ribbon_end_margin;
  cache.radius_squared = cache.radius * cache.radius;

  /* Reset the touched-node accumulator for THIS restamp. `curve_patch_restore_only()` above already
   * consumed the previous restamp's set (to revert those nodes' normals); each symmetry pass of the
   * relief action below now ORs its own node mask back in, leaving `last_restamp_nodes` describing
   * exactly what this restamp displaced -- ready for the next frame's restore. */
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  patch.last_restamp_nodes.resize(pbvh.nodes_num());
  patch.last_restamp_nodes.fill(false);

  /* Resized but deliberately NOT filled: this one accumulates across restamps. */
  patch.all_touched_nodes.resize(pbvh.nodes_num());

  /* Reset the cross-pass blend accumulator for THIS restamp too -- see `pass_weight_accum`'s doc
   * comment. Unlike `orig_positions`, it must not persist across restamps: each restamp's passes
   * blend only against each other, not against a previous drag frame's weights. */
  patch.pass_weight_accum.clear();

  /* `std::nullopt` (not a frozen value) lets `do_symmetrical_brush_actions()` recompute the
   * strength from the LIVE brush via `brush_strength()` -- so a mid-edit change of the brush's
   * Strength slider is picked up on the next re-stamp. The modal editor
   * (`paint_curve_patch_edit.cc`) watches the slider and triggers a re-stamp when it changes, so
   * this tracks the slider in real time. Radius/axis stay frozen (see `CurvePatchFrozenBrushParams`). */
  do_symmetrical_brush_actions(depsgraph,
                               *scene,
                               sd,
                               ob,
                               curve_patch_apply_effect_action,
                               paint_mode_settings,
                               std::nullopt);

  /* Final-quality smoothing (commit only) plus the viewport flush this target needs -- both live in
   * the effect because both are target-specific. */
  patch.effect->end_restamp(C, ob, patch);
#if CURVE_PATCH_PROFILING
  const double prof_t_relief = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif

#if CURVE_PATCH_PROFILING
  /* DEBUG-cpatch: one line per interactive restamp. `nodes` is the culled (tube) node count. */
  const double prof_t_end = BLI_time_now_seconds();
  static double prof_prev_start = 0.0;
  const double prof_interval_ms = (prof_prev_start > 0.0) ? (prof_t0 - prof_prev_start) * 1000.0 :
                                                            -1.0;
  prof_prev_start = prof_t0;
  IndexMaskMemory prof_memory;
  const int64_t prof_nodes = IndexMask::from_bits(patch.last_restamp_nodes, prof_memory).size();
  /* `ribbon` covers the spline rebuild plus the ribbon/LUT build -- previously an unaccounted gap
   * between `restore` and `normals`, which made `total` look larger than the sum of its parts. */
  /* `displaced` is how many vertices THIS re-stamp actually wrote (`pass_weight_accum` is cleared
   * per re-stamp and gains one entry per written vertex); `snapshot` is how many the patch has ever
   * touched. A re-stamp that repeats an earlier one must report the same `displaced` and leave
   * `snapshot` unchanged -- if the commit re-stamp does not, the two passes are not seeing the same
   * geometry, which is the open question behind the doubled pattern on commit. */
  /* `relief` now also covers the smoothing and the viewport flush, which moved together into
   * `CurvePatchEffect::end_restamp()` -- there is no longer a point between them the orchestrator
   * can time from, so the former separate `flush` column is folded in rather than reported as a
   * near-zero remainder. */
  printf(
      "[DEBUG-cpatch] total=%.2fms | restore=%.2f ribbon=%.2f normals=%.2f relief+flush=%.2f | "
      "nodes=%lld displaced=%lld snapshot=%lld quality=%d lut=%d frames=%d capped=%d "
      "interval=%.2fms\n",
      (prof_t_end - prof_t0) * 1000.0,
      (prof_t_restore - prof_t0) * 1000.0,
      (prof_t_ribbon - prof_t_restore) * 1000.0,
      (prof_t_norm - prof_t_pre_norm) * 1000.0,
      (prof_t_relief - prof_t_norm) * 1000.0,
      (long long)prof_nodes,
      (long long)patch.pass_weight_accum.size(),
      (long long)patch.effect->snapshot_size(),
      patch.final_quality ? 1 : 0,
      patch.ribbon.res,
      int(patch.frames.frames.size()),
      patch.frames.capped ? 1 : 0,
      prof_interval_ms);
  fflush(stdout);
#endif
}

/* Shared tail of the Curve Patch handoff: freeze brush params, repoint the ViewContext at the
 * patch's owned copy, publish the cache, stamp the initial preview and launch the modal editor.
 * `patch->control_curve` must already be fully built (positions, radii, handles). Used by both the
 * anchor-drag path and the roll-stroke bridge. */
static void curve_patch_begin_editing(Object &ob,
                                      const Brush &brush,
                                      const ViewContext &vc,
                                      CurvePatchCache *patch,
                                      const float3 &plane_normal,
                                      const float frozen_radius,
                                      PaintModeSettings &paint_mode_settings)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  patch->frozen_params.radius = frozen_radius;
  patch->frozen_params.swap_axis = brush.mtex.use_curve_patch_swap_axis;
  patch->frozen_params.length_mode = brush.mtex.curve_patch_length_mode;
  patch->frozen_params.length_repeat = brush.mtex.curve_patch_length_repeat;
  patch->frozen_params.end_falloff_mode = brush.mtex.curve_patch_end_falloff;
  patch->frozen_params.end_falloff_percent = brush.mtex.curve_patch_end_falloff_percent;
  patch->frozen_params.stamp_mode = brush.mtex.curve_patch_stamp_mode;
  patch->frozen_params.stamp_projection = brush.mtex.curve_patch_stamp_projection;
  patch->frozen_params.stamp_size_random = float(brush.mtex.curve_patch_stamp_size_random) / 100.0f;
  patch->frozen_params.stamp_strength_random = float(brush.mtex.curve_patch_stamp_strength_random) /
                                               100.0f;
  /* Rolled once, then frozen -- see `CurvePatchFrozenBrushParams::stamp_seed`. This is the only
   * place a real RNG is touched; everything downstream hashes this seed. */
  patch->frozen_params.stamp_seed = RandomNumberGenerator::from_random_seed().get_uint32();
  /* `frozen_radius` is the world-space radius the anchor (or roll) stroke measured; the Size slider
   * that produced it is in pixels. Record the ratio so a later Size change converts back to world
   * units, and so absolute brush jitter (also pixels) can be converted the same way. `cache.paint`
   * is the same `Paint *` the stroke's own init (`SculptPaintStroke::stroke_cache_init()`, via
   * `sd.paint`) resolved for this stroke -- the accessor the rest of the curve-patch modal uses for
   * `BKE_brush_alpha_get()`/`brush_strength()` (see `paint_curve_patch_edit.cc`) is `sd.paint`
   * reached through `CTX_data_tool_settings()`, which is not in scope here; `cache.paint` is the
   * equivalent already sitting on the `StrokeCache` this function has. */
  const int brush_size = BKE_brush_size_get(cache.paint, &brush);
  patch->frozen_params.radius_per_size = brush_size > 0 ? frozen_radius / float(brush_size) : 0.0f;
  patch->plane_normal = plane_normal;
  /* A fresh session starts with nothing active; the cache may be reused from a previous patch. */
  patch->active_point = -1;
  patch->reported_frame_cap = false;

  /* Mesh only: the snapshot and `bvhtree_from_mesh_corner_tris_ex` are tied to mesh topology
   * (faces / corner_verts / corner_tris), which CCG does not have. On Grids `surface.ready` stays
   * false and the relief takes the previous single-window path. */
  /* Cleared rather than assumed empty: this cache may be reused from a previous patch, and a stale
   * window set left over from a Mesh object would otherwise still be sampled on a Grids one. The
   * snapshot holds a copy of every vertex position plus a BVH, so it is also worth tens of
   * megabytes on a dense object. */
  patch->frames.clear();
  patch->surface.clear();
  if (bke::object::pbvh_get(ob)->type() == bke::pbvh::Type::Mesh) {
    if (const Mesh *mesh = BKE_object_get_original_mesh(&ob)) {
      curve_patch_surface_snapshot_build(*mesh, patch->surface);
    }
  }

  /* `cache.vc` otherwise still points at the just-finished stroke's own `ViewContext`, torn down
   * together with that stroke's operator. Repoint it at this patch's owned copy so every re-stamp's
   * `calc_local_from_screen()` (via `cache->vc`) dereferences valid memory for the whole lifetime
   * of the patch (see `CurvePatchCache::view_context`). */
  patch->view_context = vc;
  cache.vc = &patch->view_context;

  /* Chosen BEFORE the cache is published, so a refusal never leaves a live session whose every
   * entry point would dereference a null effect. Both callers hand this function ownership of
   * `ss.cache` (see their doc comments), so the bail frees it the same way their own Dynamic
   * Topology refusals do. */
  patch->effect = curve_patch_effect_create(brush, ob, paint_mode_settings);
  if (!patch->effect) {
    /* Stage 1's brush gate should have refused this brush long before a session was started;
     * refuse defensively rather than publishing a cache with no effect. */
    MEM_delete(patch);
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    return;
  }

  ss.curve_patch_cache = patch;

  patch->element_num = patch->effect->element_num(ob);

  /* Stamp the initial curve right away: neither `curve_patch_edit_invoke()` nor
   * `curve_patch_edit_modal()` re-stamps on its own until the user performs a first edit, so without
   * this the mesh would sit pristine with no visible feedback at all until then. */
  curve_patch_restore_and_restamp(*vc.C, ob, *patch);

  /* This re-stamp runs nested inside the just-finishing stroke's `PaintStroke::done()`, which clears
   * `RV3D_PAINTING` the instant this call chain returns -- so the fast paint-redraw path
   * `flush_update_step()` set up is torn down before the viewport redraws. Issue the full
   * finished-stroke redraw (tags every viewport and refreshes bounds independently of
   * `RV3D_PAINTING`) so this first preview reaches the screen immediately. */
  flush_update_done(vc.C, ob, UpdateType::Position);

  /* `vc.C` is the real `bContext` the stroke was invoked with (populated by
   * `ED_view3d_viewcontext_init()`), required so the modal editor's `invoke()` can register its own
   * modal handler via `WM_event_add_modal_handler()`. */
  WM_operator_name_call(
      vc.C, "SCULPT_OT_curve_patch_edit", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
}

void curve_patch_start_from_anchor(const Depsgraph &depsgraph,
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
   * `ss.curve_patch_cache` is still null (see the matching branch in
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
    return;
  }

  auto *patch = MEM_new<CurvePatchCache>(__func__);
  paintcurve_geometry_init_bezier(patch->control_curve, 2);
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
  patch->control_curve.positions_for_write()[0] = cache.initial_location -
                                                   direction * cache.initial_radius;
  patch->control_curve.positions_for_write()[1] = cache.initial_location +
                                                   direction * cache.initial_radius;
  /* `paintcurve_geometry_init_bezier()` never touches the "radius" attribute, so without this it
   * silently falls back to `bke::CurvesGeometry::radius()`'s generic 0.01 default (meant for
   * hair-curve-style geometry) instead of this codebase's own paint-curve convention of 1.0 =
   * "full brush size" (see `paintcurve_geometry_add_point()`, `paint_curve.cc:621`). At 0.01 the
   * radius-handle's screen offset is only ~10px from the pivot -- barely past its own 10px hit
   * threshold, making it effectively unclickable. */
  patch->control_curve.radius_for_write().fill(1.0f);
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
  patch->control_curve.handle_positions_left_for_write();
  patch->control_curve.handle_positions_right_for_write();
  patch->control_curve.calculate_bezier_auto_handles();
  patch->control_curve.calculate_bezier_aligned_handles();
  /* Mirrors every other position mutation in `paint_curve.cc` (e.g. `paintcurve_point_add()`):
   * invalidates cached evaluated data so the first `curve_patch_restore_and_restamp()` call sees
   * the real, just-set positions rather than the freshly-constructed curve's stale/default cache. */
  patch->control_curve.tag_positions_changed();

  ToolSettings *tool_settings = CTX_data_tool_settings(vc.C);
  curve_patch_begin_editing(
      ob, brush, vc, patch, cache.sculpt_normal, cache.initial_radius, tool_settings->paint_mode);
}

void roll_start_curve_patch_from_stroke(const Depsgraph &depsgraph,
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
    return;
  }

  if (control_positions.size() < 2) {
    MEM_delete(ss.cache);
    ss.cache = nullptr;
    return;
  }

  /* Undo the entire live roll relief back to pristine so the Curve Patch re-stamp starts from a
   * clean baseline (its `orig_positions` snapshot must not capture already-displaced vertices).
   * Unlike Curve Patch's single anchor dab, roll is an additive multi-dab stroke, so use the full
   * position restore (the default case of the internal per-brush `restore_from_undo_step()`),
   * not the per-dab `restore_from_undo_step_if_necessary()` which does nothing for the roll stroke
   * method. */
  undo::restore_position_from_undo_step(depsgraph, ob);
  bke::pbvh::update_normals(depsgraph, ob, *bke::object::pbvh_get(ob));

  auto *patch = MEM_new<CurvePatchCache>(__func__);
  const int point_num = int(control_positions.size());
  paintcurve_geometry_init_bezier(patch->control_curve, point_num);

  MutableSpan<float3> positions = patch->control_curve.positions_for_write();
  MutableSpan<float> radii = patch->control_curve.radius_for_write();
  for (const int i : IndexRange(point_num)) {
    positions[i] = control_positions[i];
    radii[i] = control_radii[i];
  }

  /* Materialize + compute the bezier handle positions -- same reason as
   * `curve_patch_start_from_anchor()`: `calculate_bezier_auto_handles()` early-outs when the handle
   * position attributes are absent, so without creating them first the AUTO handles are never
   * computed and the evaluated bezier collapses to the origin. */
  patch->control_curve.handle_positions_left_for_write();
  patch->control_curve.handle_positions_right_for_write();
  patch->control_curve.calculate_bezier_auto_handles();
  patch->control_curve.calculate_bezier_aligned_handles();
  patch->control_curve.tag_positions_changed();

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
  curve_patch_begin_editing(ob,
                            brush,
                            vc,
                            patch,
                            math::normalize(plane),
                            cache.initial_radius,
                            tool_settings->paint_mode);
}

}  // namespace blender::ed::sculpt_paint
