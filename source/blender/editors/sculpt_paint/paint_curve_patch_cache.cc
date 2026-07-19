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
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_time.h"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"

#include "ED_view3d.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

#include "mesh/mesh_brush_common.hh"
#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"

/* Temporary performance instrumentation (see previous measurement pass). Times each
 * `curve_patch_restore_and_restamp()` to confirm the C3 node-cull's effect on `relief`. Set to 0 to
 * disable; grep `DEBUG-cpatch` to remove every touch point once measured. */
#define CURVE_PATCH_PROFILING 1

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
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  /* `patch.orig_positions` is keyed by whichever flat index `curve_patch_apply_relief_action()`
   * used for this pbvh's actual type -- see the matching comment there. */
  if (pbvh.type() == bke::pbvh::Type::Grids) {
    SubdivCCG &subdiv_ccg = *ob.runtime->sculpt_session->subdiv_ccg;
    /* Tried parallelizing this loop two different ways (a direct `parallel_for_each()` over
     * `Map::items()`, which does not compile against it -- C2672, `Map::ItemIterator` is not a
     * TBB-compatible Container/Range -- and, after that, flattening into a `Vector` first and
     * `parallel_for()`-ing the writes). Both were measured WORSE than this plain loop: the actual
     * per-entry write is trivially cheap, so it was never the bottleneck, and `Map::items()`'s own
     * single-threaded slot walk -- which the flatten step still has to pay for up front, serially,
     * before any parallel part even starts -- dominates either way. Reverted; left as a plain
     * sequential loop. */
    for (const auto item : patch.orig_positions.items()) {
      subdiv_ccg.positions[item.key] = item.value;
    }
    /* `BKE_subdiv_ccg_average_grids()` was tried here to re-stitch duplicate boundary/corner
     * elements, but it *averages* every duplicate pair rather than copying the known-good side
     * into the stale one: whenever only one side of a pair is a key in `orig_positions` (true at
     * the edge of the patch's touched footprint, which shifts every restamp during interactive
     * dragging), it blends the value just restored above with the neighboring, not-yet-restored
     * duplicate's stale value -- corrupting the very position this loop just fixed. Every element
     * of every grid this patch has ever touched already has its own exact entry in
     * `orig_positions` (`curve_patch_apply_relief_action()`'s `lookup_or_add` runs unconditionally
     * for the whole grid, before any falloff rejection), so the plain per-entry restore above is
     * already exact and needs no further stitching. Mark the nodes the PREVIOUS restamp displaced
     * (tracked in `patch.last_restamp_nodes`) dirty so the `bke::pbvh::update_normals()` call that
     * follows in `curve_patch_restore_and_restamp()` recomputes exactly their normals from these
     * now-correct positions. This is the footprint that just moved away -- the region a "current node
     * mask only" tag would miss, leaving stale normals wherever the touched footprint shifts (the
     * reason an earlier version tagged every node). Tagging only these instead of all nodes is what
     * keeps a restore O(patch footprint) rather than O(whole mesh) on every interactive drag. */
    IndexMaskMemory memory;
    pbvh.tag_positions_changed(IndexMask::from_bits(patch.last_restamp_nodes, memory));
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  for (const auto item : patch.orig_positions.items()) {
    positions[item.key] = item.value;
  }
  /* Tag only the PREVIOUS restamp's displaced nodes -- NOT `mesh.tag_positions_changed()`, whose
   * whole-mesh normals-cache invalidation forces `update_normals_mesh()` into its full-recompute
   * branch (every face + every vertex) on every drag event. That full recompute was the dominant
   * cost of the interactive-edit slowdown. Normals are instead refreshed incrementally per node by
   * the `bke::pbvh::update_normals()` in `curve_patch_restore_and_restamp()`;
   * `tag_positions_changed_no_normals()` still invalidates the bounds / BVH caches without touching
   * normals (whole-mesh re-triangulation is already suppressed for the whole sculpt session by the
   * `corner_tris_cache.freeze()` at sculpt-mode enter). Mirrors sculpt's own
   * `tag_mesh_positions_changed()` fast path (`mesh/sculpt.cc`). */
  IndexMaskMemory memory;
  pbvh.tag_positions_changed(IndexMask::from_bits(patch.last_restamp_nodes, memory));
  mesh.tag_positions_changed_no_normals();
}

namespace {

/** Matches the raw `BrushActionFunc` signature `do_symmetrical_brush_actions()` expects
 * (`mesh/sculpt_intern.hh:855`) so it can be passed as its `action` callback. Called once per
 * enabled symmetry pass (mirror x radial x tile) with `StrokeCache::location_symm`/`radius`
 * already set (by `cache_calc_brushdata_symm()`, invoked internally by
 * `do_symmetrical_brush_actions()`) to the whole curve's encompassing sphere in that pass's
 * transformed space. Unlike a normal brush dab, this walks every vertex the sphere query returns
 * and applies the direct texture-driven relief formula to it directly -- no `sculpt_brush_type`
 * dispatch, no `do_brush_action()` call. */
void curve_patch_apply_relief_action(const Depsgraph &depsgraph,
                                     const Scene & /*scene*/,
                                     Sculpt & /*sd*/,
                                     Object &ob,
                                     const Brush &brush,
                                     PaintModeSettings & /*paint_mode_settings*/)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;
  CurvePatchCache &patch = *ss.curve_patch_cache;

  IndexMaskMemory memory;
  const brushes::CursorSampleResult cursor_sample_result = calc_brush_node_mask(
      depsgraph, ob, brush, memory);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* Curve Patch bypasses `do_brush_action()`'s own per-`bke::pbvh::Type` node dispatch (see the
   * direct-write comment at the end of this function), so it picks its own flat position/normal
   * arrays here instead: `mesh.vert_positions_for_write()`/`mesh.vert_normals()` for a regular
   * mesh, or `SubdivCCG::positions`/`::normals` for Multires -- both indexed by the same flat
   * "grid * grid_area + in-grid offset" scheme `bke::ccg::grid_range()` uses below. Either way,
   * `patch.orig_positions` ends up keyed by whichever flat index this array uses, not necessarily
   * a mesh vertex index -- see its doc comment in `paint_curve_patch_cache.hh`. Dynamic Topology
   * (`Type::BMesh`) has no such stable per-recompute index at all, so
   * `curve_patch_start_from_anchor()` refuses to start a Curve Patch session on one; that case is
   * unreachable here. */
  Mesh *mesh = nullptr;
  SubdivCCG *subdiv_ccg = nullptr;
  MutableSpan<float3> positions;
  Span<float3> normals;
  /* Sculpt mask (painted selection), 0 = fully protected, 1 = fully open to the relief. Empty
   * when the mesh/grids have no mask layer at all, in which case every vertex is unmasked. */
  Span<float> mask;
  /* Valid only when `subdiv_ccg` is set; computed once here so the node cull, the grid cull, and
   * both PHASE 1/2 walks below all agree on the same key instead of each re-deriving it. */
  CCGKey key;
  std::optional<MeshAttributeData> mesh_attribute_data;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      mesh = id_cast<Mesh *>(ob.data);
      positions = mesh->vert_positions_for_write();
      normals = mesh->vert_normals();
      mesh_attribute_data.emplace(*mesh);
      mask = mesh_attribute_data->mask;
      break;
    case bke::pbvh::Type::Grids:
      subdiv_ccg = ss.subdiv_ccg;
      positions = subdiv_ccg->positions;
      normals = subdiv_ccg->normals;
      mask = subdiv_ccg->masks;
      key = BKE_subdiv_ccg_key_top_level(*subdiv_ccg);
      break;
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      return;
  }

  const MTex *mtex = &brush.mtex;
  const float2 mtex_size(mtex->size[0], mtex->size[1]);
  const float2 mtex_ofs(mtex->ofs[0], mtex->ofs[1]);
  const float total_length = patch.spline.total_length();

  /* `BKE_brush_curve_strength()` below reads `brush.curve_distance_falloff`'s lookup table for the
   * CUSTOM preset; initialize it ONCE here so the parallel PHASE 1 loop only ever reads an
   * already-built table (a lazy init inside a worker thread would race). */
  if (brush.curve_distance_falloff) {
    BKE_curvemapping_init(brush.curve_distance_falloff);
  }

  /* Two-phase relief for performance: PHASE 1 evaluates the per-vertex geometry + texture in
   * PARALLEL across pbvh nodes, read-only, collecting only the survivors; PHASE 2 applies them
   * serially -- the only part that mutates the shared `orig_positions` map and writes back
   * positions. Replaces the former single-threaded walk that also inserted EVERY iterated vertex
   * (even rejected ones) into `orig_positions`, which on a dense mesh dominated the edit cost. */

  /* Per-vertex outcome of `compute_vertex()`: the true pre-patch position (so PHASE 2 never has to
   * re-derive it from a `positions` array another pass may have already written to), the raw
   * relief height along the vertex's own normal, and the falloff weight this pass claims the
   * vertex with. PHASE 2 blends `height`/`weight` across every pass that claims the same vertex
   * within this restamp (see `patch.pass_weight_accum`) rather than letting the last pass to run
   * unconditionally overwrite an earlier pass's result -- the fix for a patch straddling a
   * mirror/radial symmetry plane, where the direct and mirrored passes can both legitimately claim
   * the same real vertex and previously fought over which one's displacement "won". */
  struct VertexRelief {
    float3 orig;
    float height;
    float weight;
  };

  /* Largest world-space half-width anywhere on the curve. Drives the node/grid cull tube below, and
   * serves as the fall-back supersampling kernel size for vertices that sit just outside the strip
   * (where no local half-width is available because the ribbon reports no branch for them). */
  float max_radius = 0.0f;
  for (const float r : patch.spline.radii) {
    max_radius = std::max(max_radius, r);
  }
  /* Scaled by the RIBBON's radius, not the (unwidened) frozen one: in Stamps mode a jittered stamp
   * legitimately reaches `jitter_amount` further out than the frozen radius, and a node culled here
   * never gets to run the per-vertex test that would have claimed it. Identical to the frozen radius
   * in Ribbon mode, where the two values are equal. */
  max_radius *= patch.ribbon_radius;

  /* Displaced target position for `idx`, or nullopt if it is rejected. Pure/read-only so it is safe
   * to run concurrently: it never writes `positions` nor mutates the snapshot map. `thread_id`
   * indexes the texture pool's per-thread slot (required for concurrent `paint_get_tex_pixel()`). */
  auto compute_vertex = [&](const int idx, const int thread_id) -> std::optional<VertexRelief> {
    /* Pre-patch position for `idx`, READ-ONLY: from the snapshot map if an earlier symmetry pass of
     * this restamp already recorded it (in that pass's PHASE 2), else the live position -- which
     * `curve_patch_restore_only()` reset to the true original before this action ran and PHASE 1
     * never overwrites, so it is the true original either way. `lookup_ptr` only reads the map. */
    const float3 *orig_ptr = patch.orig_positions.lookup_ptr(idx);
    const float3 orig = orig_ptr ? *orig_ptr : positions[idx];

    /* Map the real vertex back into the canonical (non-mirrored, non-rotated) curve space this
     * pass represents -- the same technique `filter_region_clip_factors()` uses
     * (`sculpt.cc:7434-7438`) to compare a real vertex against pass-0-defined data. */
    float3 symm_co = symmetry_flip(orig, cache.mirror_symmetry_pass);
    if (cache.radial_symmetry_pass) {
      symm_co = math::transform_point(cache.symm_rot_mat_inv, symm_co);
    }

    /* `normal_dist`/`radial_dist` below only reject vertices by POSITION -- true 3D
     * closest-point distance cuts straight through a sharp edge/corner, so a vertex on a face
     * perpendicular to the one the curve was drawn on can still land close enough in 3D to pass
     * both checks even though it does not belong to that face at all (written vertices were
     * confirmed to include ones with `dot(normal, plane_normal) == 0`, i.e. a face turned a full
     * 90 degrees from the curve's own). Reject by SURFACE ORIENTATION instead: a vertex whose own
     * normal has drifted more
     * than ~72 degrees from `plane_normal` cannot be part of the intended face, regardless of
     * how close it measures in raw 3D distance. Mirrors `symm_co` above so the comparison stays
     * in the same canonical space `plane_normal` was frozen in. */
    float3 symm_normal = symmetry_flip(normals[idx], cache.mirror_symmetry_pass);
    if (cache.radial_symmetry_pass) {
      symm_normal = math::transform_point(cache.symm_rot_mat_inv, symm_normal);
    }
    if (math::dot(symm_normal, patch.plane_normal) <= 0.3f) {
      return std::nullopt;
    }

    /* Clip the relief by the sculpt mask, mirroring every dab-based brush's own
     * `fill_factor_from_hide_and_mask()` (`sculpt.cc:7233`): a fully-masked (mask == 1) vertex
     * must stay untouched by the projection, the same way it stays untouched by a normal brush
     * stroke. */
    const float mask_factor = mask.is_empty() ? 1.0f : 1.0f - mask[idx];
    if (mask_factor <= 0.0f) {
      return std::nullopt;
    }

    /* Ribbon LUT instead of `CurvePatchSpline::closest_point()`: the old global nearest-segment
     * search was multi-valued on the concave side of sharp turns (several segments near-equidistant
     * -> neighboring vertices snapping to different `s`, tearing the texture into fans). The ribbon
     * assigns UV from the specific quad covering the vertex, staying single-valued through
     * arbitrarily sharp turns -- the same construction the Roll stroke method uses. No branch at all
     * means the vertex projects outside the ribbon (off the strip, or past the curve's ends, which
     * the borders cut off exactly like the old `s` range rejection did). */
    /* Relief one stretch of the curve contributes at `sample_co`, or nullopt when that stretch does
     * not actually reach it. */
    auto branch_relief = [&](const float3 &sample_co,
                             const float2 ribbon_uv) -> std::optional<VertexRelief> {
      const float s = ribbon_uv.y;
      /* Signed offset from the ribbon plane, measured against the curve point the ribbon mapped this
       * sample to (NOT the globally-closest point -- consistent with the ribbon's own branch
       * choice). */
      const float normal_dist = math::dot(sample_co - patch.spline.evaluate(s), patch.plane_normal);

      /* `CurvePatchSpline::radii` stores the control curve's per-point `radius` attribute
       * verbatim, which is a unitless UI scalar (default 1.0 = "full brush size", see
       * `curve_patch_start_from_anchor()`) -- everywhere else it is read
       * (`paint_curve_sync.cc:paintcurve_radius_handle_screen_get_from_geometry()`) it is only
       * ever turned into a SCREEN-PIXEL handle length, never a world-space distance. Scale it by
       * the frozen brush radius (the same world-space value the old dab-based re-stamp used
       * globally) to get an actual world-space falloff radius comparable to `lateral`/`s` below. */
      const float falloff_radius_at_s = patch.spline.radius_at(s) * patch.frozen_params.radius;
      if (falloff_radius_at_s <= 0.0f) {
        return std::nullopt;
      }

      /* The ribbon's `u` is normalized across the half-width the LUT was BUILT with, which in Stamps
       * mode is widened by the jitter amount (`CurvePatchCache::ribbon_radius`). So turning `u` back
       * into a world-space lateral offset must use that widened scale, while the falloff radius above
       * must stay UNWIDENED -- the widening only exists to give jittered stamps room inside the strip
       * and must not enlarge the brush's actual reach. Using one value for both would simultaneously
       * under-report `lateral` (widening the visible relief band as Jitter rises) and compress the
       * stamp-space `du` below, squashing stamps toward the curve and clipping the very edge stamps
       * the widening was meant to admit. The two are equal in Ribbon mode, where jitter never
       * applies. */
      const float lateral_scale_at_s = patch.spline.radius_at(s) * patch.ribbon_radius;

      /* `normal_dist` is the vertex's signed offset along the anchor plane's normal from the
       * closest point on the curve -- how far it "lifts off" that plane. On a flat surface it is
       * ~0 everywhere. On a faceted surface (e.g. the side faces of a cube when the curve sits on
       * the top face) it grows large, and without rejecting those vertices the relief leaked onto
       * faces that should never receive the projection: `closest_point` picked them up (they ARE
       * close to the curve in 3D) but `lateral` alone only measured the in-plane offset, so they
       * passed the lateral falloff with a small apparent distance. Hybrid handling per the
       * redesign discussion: a hard cutoff rejects far-off-plane vertices outright (clean result
       * on faceted meshes), and the lateral falloff is computed from the TRUE 3D distance
       * `sqrt(lateral^2 + normal_dist^2)` so what survives blends smoothly across edges instead of
       * producing a hard seam at the cutoff. The cutoff is set to one brush diameter: anything
       * farther off-plane than the strip is wide is definitely not part of the target face, while
       * near-edge vertices on the intended face (small `normal_dist` from slight surface curvature)
       * still pass. */
      if (std::abs(normal_dist) > 2.0f * falloff_radius_at_s) {
        return std::nullopt;
      }
      /* In-plane offset reconstructed from the ribbon's normalized across-strip coordinate; the
       * falloff formula below is unchanged. */
      const float lateral = ribbon_uv.x * lateral_scale_at_s;
      const float radial_dist = std::sqrt(lateral * lateral + normal_dist * normal_dist);
      const float lateral_falloff = BKE_brush_curve_strength(
          &brush, radial_dist, falloff_radius_at_s);
      if (lateral_falloff <= 0.0f) {
        return std::nullopt;
      }

      /* The ribbon's first/last rows sit at the curve's ends, extended outward by
       * `ribbon_end_margin` in Stamps mode (0 in Ribbon mode, where the strip stops dead at the
       * ends), so `s` from the LUT is already confined to that range and vertices past it fail
       * `sample()` above. This guard only backstops interpolation slack at the LUT's edge pixels;
       * it must admit the extension, or the overhanging halves of the end stamps would be rejected
       * here and clipped exactly as before. On a closed curve the two "ends" are the same place and
       * the margin is 0, so the range is unchanged -- there is simply nothing outside it. */
      if (s < -patch.ribbon_end_margin || s > total_length + patch.ribbon_end_margin) {
        return std::nullopt;
      }

      /* End falloff: fade the relief in and out over `zone` world-space units at each of the
       * curve's two ends, so the strip does not begin and end with a step. `s` is arc-length along
       * the WHOLE spline (not a per-branch coordinate), so where a curve overlaps itself the
       * interior crossing sits at a mid-range `s` and keeps full amplitude -- only the curve's two
       * true ends fade. The percentage is RNA-clamped to 50, so the two zones can never overlap. */
      float end_falloff = 1.0f;
      if (!patch.spline.cyclic &&
          patch.frozen_params.end_falloff_mode == MTEX_CURVE_PATCH_END_SMOOTH)
      {
        const float zone = float(patch.frozen_params.end_falloff_percent) * 0.01f * total_length;
        if (zone > 1e-8f) {
          /* Clamped at BOTH ends. `s` now runs slightly outside `[0, total_length]` on an extended
           * strip, and a negative `t` would make `t * t * (3 - 2t)` return a small POSITIVE value
           * -- a ghost of relief past the curve's end rather than the intended fade to nothing. */
          const float t = std::clamp(std::min(s, total_length - s) / zone, 0.0f, 1.0f);
          end_falloff = t * t * (3.0f - 2.0f * t);
        }
      }
      if (end_falloff <= 0.0f) {
        return std::nullopt;
      }

      /* `u` (across the strip) is normalized the same way every other brush texture mapping mode
       * normalizes its input by the brush radius -- `MTEX_MAP_MODE_VIEW`/`TILED`/`RANDOM` divide by
       * `pixel_radius`/`start_pixel_radius` (`BKE_brush_sample_tex_3d()`, `brush.cc:1001-1018`), and
       * `MTEX_MAP_MODE_AREA` bakes an equivalent `scale_m4_fl(scale, radius)` into
       * `cache.brush_local_mat` (`calc_brush_local_mat()`, `sculpt.cc:2832`).
       *
       * The ribbon's `u` is already normalized to [-1, 1] across the strip, with its sign chosen at
       * grid construction (`curve_patch_ribbon_build()`: the `cross(tangent, plane_normal)` side
       * carries +1) to match what `-lateral / lateral_scale_at_s` produced before -- the
       * orientation the paint-cursor overlay previews. */
      const float u = ribbon_uv.x;

      /* Both modes end up here with an intensity and a per-stamp amplitude multiplier, so the
       * height formula below stays common to them. Ribbon mode has no per-stamp amplitude and
       * leaves `stamp_strength` at 1.0, which keeps its result bit-for-bit what it was. */
      float tex_value = 1.0f;
      float stamp_strength = 1.0f;
      if (patch.frozen_params.stamp_mode == MTEX_CURVE_PATCH_STAMP_STAMPS) {
        /* Find the stamps whose square could cover this vertex. The list is sorted by `center_v`,
         * so a lower-bound on `s - max_extent` opens a window that closes as soon as a stamp starts
         * past `s`; with the default spacing that window holds one to three stamps.
         *
         * The window is the radius scaled by sqrt(2), not the radius itself: a stamp is a SQUARE
         * rotated by its own `angle`, so its farthest corner sits `half_extent * sqrt(2)` from its
         * center. Sizing the window to the radius alone would drop a rotated stamp whose corner
         * still covers this vertex, clipping the stamp's corners once Random Rotation is non-zero.
         * The per-stamp test below is done in the stamp's own frame and stays exact; this bound
         * only has to be conservative. */
        const float max_extent = curve_patch_stamp_reach(patch.frozen_params.radius);
        auto lower = std::lower_bound(patch.stamps.begin(),
                                      patch.stamps.end(),
                                      s - max_extent,
                                      [](const CurvePatchStamp &stamp, const float value) {
                                        return stamp.center_v < value;
                                      });
        bool hit = false;
        float best_abs = 0.0f;
        for (auto it = lower; it != patch.stamps.end() && it->center_v <= s + max_extent; ++it) {
          const float dv = s - it->center_v;
          /* `u` is the ribbon's normalized lateral coordinate while the stamp centers are in world
           * units, so scale it by the same widened `lateral_scale_at_s` the `lateral` reconstruction
           * above uses -- the scale the ribbon was actually built with, which is what makes a stamp
           * jittered all the way out to `|center_u| == jitter_amount` reachable at all. */
          const float du = u * lateral_scale_at_s - it->center_u;
          /* Rotate the offset INTO the stamp's own frame, hence the negated angle. */
          const float cos_a = std::cos(-it->angle);
          const float sin_a = std::sin(-it->angle);
          const float local_v = dv * cos_a - du * sin_a;
          const float local_u = dv * sin_a + du * cos_a;
          if (std::abs(local_v) > it->half_extent || std::abs(local_u) > it->half_extent) {
            continue;
          }
          /* Map into the texture's [-1, 1] domain, matching what Ribbon mode feeds
           * `paint_get_tex_pixel()`, then apply the same mapping size/offset. */
          float stamp_u = local_u / it->half_extent;
          float stamp_v = local_v / it->half_extent;
          if (patch.frozen_params.swap_axis) {
            std::swap(stamp_u, stamp_v);
          }
          stamp_u = stamp_u * mtex_size.x + mtex_ofs.x;
          stamp_v = stamp_v * mtex_size.y + mtex_ofs.y;

          float sample = 1.0f;
          float sample_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
          if (mtex->tex != nullptr) {
            paint_get_tex_pixel(mtex, stamp_u, stamp_v, ss.tex_pool, thread_id, &sample, sample_rgba);
          }
          /* The brush's own falloff curve fades the stamp toward its rim, so overlapping stamps
           * meet along a smooth seam instead of showing the square's hard edge. */
          const float dist = std::sqrt(local_u * local_u + local_v * local_v);
          const float stamp_falloff = BKE_brush_curve_strength(&brush, dist, it->half_extent);
          /* A square's corners reach `sqrt(2) * half_extent`, past the falloff's own length, and a
           * CUSTOM brush curve is free to return a negative value out there -- which would invert
           * the relief in the corners instead of fading it out. The Ribbon path guards its own
           * falloff the same way. */
          if (stamp_falloff <= 0.0f) {
            continue;
          }
          const float candidate = sample * stamp_falloff * it->strength;
          /* Overlapping stamps merge by the strongest absolute displacement -- the same rule
           * `relief_at()` uses where the curve runs alongside itself. Summing would pile up into
           * spikes wherever the spacing puts stamps on top of each other. */
          if (!hit || std::abs(candidate) > best_abs) {
            hit = true;
            best_abs = std::abs(candidate);
            tex_value = sample;
            stamp_strength = stamp_falloff * it->strength;
          }
        }
        if (!hit) {
          /* Between stamps the surface is simply untouched. */
          return std::nullopt;
        }
      }
      else {
      /* `v` (along the strip) maps `s` onto the texture's [-1, 1] domain, centered on the curve's
       * midpoint so the pattern stays symmetric. `tile_span` is the world-space length of one tile,
       * selected by the frozen length mode (Default hybrid / Repeat N / Stretch) -- see
       * #curve_patch_texture_tile_span. The degenerate `tile_span == 0` (collapsed curve) maps to
       * `v = 0`. */
      const float tile_span = curve_patch_texture_tile_span(patch.frozen_params.length_mode,
                                                            patch.frozen_params.length_repeat,
                                                            total_length,
                                                            falloff_radius_at_s,
                                                            patch.spline.cyclic);
      /* Centering on the curve's midpoint keeps an OPEN strip's pattern symmetric between its two
       * ends. A closed curve has no midpoint to be symmetric about; its anchor is the join at
       * `s == 0`, where a tile has to START -- hence `v = -1` there (the tile domain is [-1, 1],
       * matching `u`), running to `+1` one tile later. At `s == total_length`, which `tile_span`
       * above snapped to a whole tile count `n`, that yields `2n - 1`, congruent to `-1` modulo the
       * texture's period of 2: the pattern closes on itself. Dropping the `- 1` would put the join
       * mid-tile and, in Stretch/Default (which do not wrap `v` below), push the whole loop outside
       * the tile the texture actually occupies. */
      float v = tile_span > 1e-8f ? (patch.spline.cyclic ?
                                         s / tile_span * 2.0f - 1.0f :
                                         (s - total_length * 0.5f) / tile_span * 2.0f) :
                                    0.0f;

      /* REPEAT mode must show a full copy of the texture in every tile regardless of the texture's
       * own extension mode (an image set to Extend/Clip, or a procedural texture with no natural
       * period, would otherwise never visibly repeat -- only the [-1, 1] center tile would carry the
       * pattern and the rest would smear the edge). Wrap the along-length coordinate back into a
       * single tile's [-1, 1) domain (period 2, sawtooth) so each of the N tiles re-samples the whole
       * texture. Default/Stretch keep the continuous coordinate: Stretch is a single tile already,
       * and Default deliberately relies on the texture's own tiling for its hybrid look. */
      if (patch.frozen_params.length_mode == MTEX_CURVE_PATCH_LENGTH_REPEAT) {
        v -= 2.0f * std::floor((v + 1.0f) * 0.5f);
      }

      float tex_u = u;
      float tex_v = v;
      if (patch.frozen_params.swap_axis) {
        std::swap(tex_u, tex_v);
      }
      tex_u = tex_u * mtex_size.x + mtex_ofs.x;
      tex_v = tex_v * mtex_size.y + mtex_ofs.y;

      /* Mirrors the null-texture guard `sculpt_apply_texture()` used to apply before dispatching
       * to the old, now-removed Curve Patch map-mode branch: `RE_texture_evaluate()` returns
       * `false` without writing its intensity output when `mtex->tex` is null, so calling
       * `paint_get_tex_pixel()` unconditionally read an uninitialized `tex_value`. */
      float tex_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      if (mtex->tex != nullptr) {
        paint_get_tex_pixel(mtex, tex_u, tex_v, ss.tex_pool, thread_id, &tex_value, tex_rgba);
      }
      }

      const float height = tex_value * stamp_strength * lateral_falloff * end_falloff * mask_factor *
                           cache.bstrength;
      /* `end_falloff` scales the claim WEIGHT as well as the height. Not for the branch merge just
       * above -- `relief_at()` picks the branch with the largest `|height|` and never reads the
       * weight -- but for the cross-pass blend in PHASE 2 (`pass_weight_accum`), which averages
       * every symmetry pass claiming this vertex as `sum(weight * height) / sum(weight)`. A faded
       * end that kept its full weight would enter that average with the same authority as a
       * mirrored pass at full amplitude and dent the relief along the symmetry plane. */
      return VertexRelief{orig, height, lateral_falloff * end_falloff};
    };

    /* Relief at one sample position: the stretches covering it merged by keeping the strongest
     * displacement rather than averaging them.
     *
     * Where the curve runs alongside itself, both stretches genuinely cover the sample, and each
     * one's own falloff already fades it out with distance from that stretch's center line. Taking
     * the strongest is the union of two embossed strips: the relief keeps full amplitude
     * everywhere, and the transition between them follows the (smoothly varying) line where the
     * two displacements happen to be equal, so it reads as one strip passing over the other.
     * Returns nullopt when no stretch reaches the sample at all.
     *
     * Averaging was the alternative -- and is what `pass_weight_accum` does for symmetry passes
     * below -- but it is right there for a different reason: those passes describe the SAME surface
     * mirrored, so averaging reconstructs it. Two stretches of one curve carry DIFFERENT texture
     * positions, and averaging them cancels the pattern into a visibly flattened band along the
     * overlap. */
    auto relief_at = [&](const float3 &sample_co) -> std::optional<VertexRelief> {
      float2 branch_uv[2];
      const int branch_num = patch.ribbon.sample(sample_co, branch_uv);
      std::optional<VertexRelief> merged;
      for (const int b : IndexRange(branch_num)) {
        const std::optional<VertexRelief> relief = branch_relief(sample_co, branch_uv[b]);
        if (!relief) {
          continue;
        }
        if (!merged || std::abs(relief->height) > std::abs(merged->height)) {
          merged = relief;
        }
      }
      return merged;
    };

    return relief_at(symm_co);
  };

  /* Node cull: `calc_brush_node_mask()` returns every node inside the whole-curve encompassing
   * SPHERE, but the relief only lands in a thin tube along the polyline. On a long curve over a broad
   * surface that sphere holds many nodes whose (up-facing) vertices each pay for `closest_point()`
   * only to be rejected by the lateral falloff. Drop any node whose bounds fall entirely outside the
   * falloff tube before the per-vertex walk. Conservative and result-identical: a displaced vertex
   * must have `radial_dist < falloff_radius_at_s` for a non-zero falloff (`compute_vertex()`), i.e.
   * it sits within `max_radius` of the polyline; the `2.5x` factor is generous margin for the
   * bounding-sphere approximation and for slightly-stale node bounds, and at the tube boundary the
   * relief tapers to ~0 anyway, so nothing visible is ever culled. Node centres are mapped into the
   * same canonical space `compute_vertex()` uses, so symmetry passes cull correctly.
   *
   * The tube is measured from the curve's own polyline, which the ribbon's end extension reaches
   * `ribbon_end_margin` beyond, so that margin is added here -- a node holding only the overhang of
   * an end stamp is otherwise dropped before the per-vertex test can claim it. */
  const float tube_radius = 2.5f * max_radius + patch.ribbon_end_margin;
  BitVector<> keep(pbvh.nodes_num(), false);
  auto cull_nodes = [&](const auto nodes) {
    cursor_sample_result.node_mask.foreach_index([&](const int i) {
      const Bounds<float3> &bounds = nodes[i].bounds();
      const float3 center = (bounds.min + bounds.max) * 0.5f;
      const float node_radius = math::distance(center, bounds.max);
      float3 canonical = symmetry_flip(center, cache.mirror_symmetry_pass);
      if (cache.radial_symmetry_pass) {
        canonical = math::transform_point(cache.symm_rot_mat_inv, canonical);
      }
      const float reach = tube_radius + node_radius;
      if (patch.spline.distance_sq_to(canonical) <= reach * reach) {
        keep[i].set();
      }
    });
  };
  if (mesh) {
    cull_nodes(pbvh.nodes<bke::pbvh::MeshNode>());
  }
  else {
    cull_nodes(pbvh.nodes<bke::pbvh::GridsNode>());
  }
  IndexMaskMemory culled_memory;
  const IndexMask node_mask = IndexMask::from_bits(keep, culled_memory);

  /* Grid cull (Multires only): the node cull above cannot shrink a `bke::pbvh::Tree::from_grids()`
   * leaf below one base-mesh face's worth of grids -- `pbvh.cc`'s `leaf_limit = max(800 /
   * key.grid_area, 1)` hits that floor once `grid_area` (vertices per grid, quadratic in the
   * Multires level) passes ~800, so at high subdivision a single surviving node can still bundle
   * many grids spanning far more surface than the falloff tube actually touches -- unlike a Mesh
   * leaf, whose size is a flat constant (`leaf_limit = 2500`) regardless of density. Cull individual
   * GRIDS within each surviving node the same way `cull_nodes` above culls whole nodes, before
   * either PHASE 1's `compute_vertex()` walk or PHASE 2's whole-grid snapshot touches them. A plain
   * min/max scan per grid is worth doing even though it is itself `O(grid_area)`: it is a handful of
   * comparisons per vertex, versus `compute_vertex()`'s closest-point search, curve-falloff lookup,
   * and texture sample -- culling a whole far-away grid this way is strictly cheaper than walking it
   * with the real relief formula only to have every vertex rejected. */
  BitVector<> grid_keep;
  if (subdiv_ccg) {
    grid_keep.resize(subdiv_ccg->grids_num, false);
    const Span<bke::pbvh::GridsNode> grids_nodes = pbvh.nodes<bke::pbvh::GridsNode>();
    node_mask.foreach_index([&](const int i) {
      for (const int grid : grids_nodes[i].grids()) {
        const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
        float3 grid_min = grid_positions[0];
        float3 grid_max = grid_positions[0];
        for (const float3 &p : grid_positions) {
          grid_min = math::min(grid_min, p);
          grid_max = math::max(grid_max, p);
        }
        const float3 center = (grid_min + grid_max) * 0.5f;
        const float grid_radius = math::distance(center, grid_max);
        float3 canonical = symmetry_flip(center, cache.mirror_symmetry_pass);
        if (cache.radial_symmetry_pass) {
          canonical = math::transform_point(cache.symm_rot_mat_inv, canonical);
        }
        const float reach = tube_radius + grid_radius;
        if (patch.spline.distance_sq_to(canonical) <= reach * reach) {
          grid_keep[grid].set();
        }
      }
    });
  }

  /* PHASE 1 (parallel, read-only): each pbvh node is processed on a worker thread; surviving
   * vertices and their displaced target positions are gathered into a thread-local buffer. No
   * position or snapshot-map writes happen here, so the reads inside `compute_vertex()` are
   * race-free across threads (texture sampling uses the per-thread pool slot `thread_id`). */
  struct ReliefWrite {
    int idx;
    float3 orig;
    float height;
    float weight;
  };
  /* `touched_nodes` records the pbvh nodes that actually received at least one displacement, so the
   * normal recompute / draw invalidation / `last_restamp_nodes` accumulation below can be scoped to
   * that thin strip instead of the whole encompassing-sphere query (~20-30x more nodes on a dense
   * mesh -- the query is only a conservative superset of where relief lands). Populated for a regular
   * mesh only; Multires keeps the full query mask (its boundary stitch in PHASE 2 reaches wider). */
  struct LocalData {
    Vector<ReliefWrite> writes;
    Vector<int> touched_nodes;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            const int thread_id = BLI_task_parallel_thread_id(nullptr);
            LocalData &local = all_tls.local();
            const int64_t before = local.writes.size();
            for (const int vert : nodes[i].verts()) {
              if (const std::optional<VertexRelief> relief = compute_vertex(vert, thread_id)) {
                local.writes.append({vert, relief->orig, relief->height, relief->weight});
              }
            }
            if (local.writes.size() > before) {
              local.touched_nodes.append(i);
            }
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      /* Flatten every `grid_keep`-surviving grid across ALL surviving nodes into one list, so the
       * parallel dispatch below has as many independent work units as there are surviving GRIDS,
       * not just surviving NODES. A per-node dispatch (`node_mask.foreach_index`, as the Mesh case
       * above still uses) tops out at `node_mask.size()` concurrent tasks -- on a typical Multires
       * patch the node cull leaves only a handful of nodes (see its doc comment above), so most of
       * the machine's cores sat idle while a few threads walked each node's own grids serially,
       * regardless of how many grids that node bundled. Grids, unlike Mesh nodes, are a fixed unit
       * of work `bke::ccg::grid_range()` already hands out independently, so flattening the
       * dispatch to per-grid is a free way to reclaim that parallelism. A finer-than-grid (tile)
       * cull was tried here and measured WORSE, not better -- this Multires test's brush radius is
       * large enough relative to one grid's own world-space extent that a kept grid is essentially
       * entirely inside the tube already, so a tile cull only pays its own bounds-scan overhead
       * without rejecting anything; reverted in favor of this dispatch-granularity fix instead. */
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      Vector<int> surviving_grids;
      node_mask.foreach_index([&](const int i) {
        for (const int grid : nodes[i].grids()) {
          if (grid_keep[grid]) {
            surviving_grids.append(grid);
          }
        }
      });
      threading::parallel_for(surviving_grids.index_range(), 1, [&](const IndexRange range) {
        const int thread_id = BLI_task_parallel_thread_id(nullptr);
        LocalData &local = all_tls.local();
        for (const int gi : range) {
          const int grid = surviving_grids[gi];
          for (const int idx : bke::ccg::grid_range(key, grid)) {
            if (const std::optional<VertexRelief> relief = compute_vertex(idx, thread_id)) {
              local.writes.append({idx, relief->orig, relief->height, relief->weight});
            }
          }
        }
      });
      break;
    }
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      break;
  }

  /* PHASE 2 (serial): the sole writer of `positions`/`orig_positions`. For Multires the WHOLE
   * touched-grid footprint is snapshotted first, because `BKE_subdiv_ccg_average_stitch_faces()`
   * below rewrites shared grid-boundary duplicates the relief formula itself may not have displaced
   * -- `curve_patch_restore_only()` must be able to revert every element the stitch can touch,
   * exactly as the original per-grid snapshot guaranteed. A regular mesh has no such stitch, so only
   * the vertices actually displaced are snapshotted there (which is what makes restore O(displaced)
   * rather than O(touched region)). `positions[idx]` is still the true original at snapshot time
   * because PHASE 1 wrote nothing. */
  if (subdiv_ccg) {
    /* Deliberately NOT filtered by `grid_keep`: `BKE_subdiv_ccg_average_stitch_faces()` below can
     * move boundary vertices of a face's OTHER corner grids even when only one of them was actually
     * displaced by relief, so every grid this pass's node selection could reach must still be
     * snapshotted here regardless of the finer per-grid cull -- only PHASE 1's `compute_vertex()`
     * walk (pure extra work with no correctness dependency) skips culled grids. */
    const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
    node_mask.foreach_index([&](const int i) {
      for (const int grid : nodes[i].grids()) {
        for (const int idx : bke::ccg::grid_range(key, grid)) {
          patch.orig_positions.lookup_or_add(idx, positions[idx]);
        }
      }
    });
  }
  /* Scope the position-change tag to the nodes that ACTUALLY received a displacement, not the whole
   * encompassing-sphere query. On a dense mesh the query is ~20-30x larger than the thin strip the
   * relief lands on, and tagging all of it made the following `bke::pbvh::update_normals()` (and the
   * draw-time one) recompute normals for the whole region -- the co-equal remaining cost after
   * parallelization. Multires keeps the full query mask: its `average_stitch_faces()` below rewrites
   * boundary duplicates across the wider region, so those nodes' normals must refresh too. */
  IndexMaskMemory tag_memory;
  IndexMask tag_mask = node_mask;
  if (mesh) {
    BitVector<> touched(pbvh.nodes_num(), false);
    for (const LocalData &local : all_tls) {
      for (const int node : local.touched_nodes) {
        touched[node].set();
      }
    }
    tag_mask = IndexMask::from_bits(touched, tag_memory);
  }

  /* Push the nodes into the active undo step BEFORE anything is displaced. Curve Patch bypasses
   * `do_brush_action()`, which normally handles this for standard strokes; without it, nodes the
   * patch reaches that the initial stroke (anchor dab or Roll stroke) never touched are absent from
   * the step and Ctrl+Z cannot restore them.
   *
   * The ordering is the point: `undo::push_nodes()` records each node's CURRENT positions as the
   * state to restore, and only the first push of a node in a step takes effect. Called after PHASE 2
   * (where it used to sit, contradicting its own comment) it recorded nodes already carrying the
   * relief, so undoing a committed patch restored those nodes to a displaced state instead of the
   * original surface. Here the positions are still pristine: `curve_patch_restore_only()` reset them
   * at the top of this re-stamp, PHASE 1 is read-only, and any node an earlier symmetry pass wrote
   * to was pushed by that pass. */
  undo::push_nodes(depsgraph, ob, tag_mask, undo::Type::Position);

  for (LocalData &local : all_tls) {
    for (const ReliefWrite &write : local.writes) {
      /* On a regular mesh this is the sole snapshot of `idx`; on Multires it was already recorded by
       * the whole-grid pass above, so `lookup_or_add` just returns the existing original. Uses
       * `write.orig` (computed once in PHASE 1) rather than re-reading `positions[write.idx]` here,
       * since an earlier symmetry pass of THIS restamp may already have written a blended result to
       * it below. */
      patch.orig_positions.lookup_or_add(write.idx, write.orig);

      /* Blend this pass's contribution with any earlier symmetry pass of this restamp that also
       * claimed `write.idx` -- a patch straddling a mirror/radial symmetry plane can have both the
       * direct and the mirrored pass land on the same real vertex. Without this, whichever pass's
       * PHASE 2 ran last would unconditionally overwrite the earlier pass's displacement instead of
       * the two surfaces merging, leaving a hard seam where only one side "won". Weighting by each
       * pass's own falloff (rather than a plain average) makes the blend fall off to whichever pass
       * dominates away from the overlap, converging to that pass's own height alone. */
      float2 &accum = patch.pass_weight_accum.lookup_or_add(write.idx, float2(0.0f, 0.0f));
      accum.x += write.weight;
      accum.y += write.weight * write.height;
      const float blended_height = accum.y / accum.x;
      positions[write.idx] = write.orig + normals[write.idx] * blended_height;
    }
  }

  if (subdiv_ccg) {
    /* Adjacent grids duplicate their shared boundary/corner elements, so displacing only the
     * flat indices `node.grids()` reported leaves those duplicate copies -- and any vertex whose
     * own grid was outside this pass's node mask entirely -- stale until reconciled. This recompute
     * writes a whole curve-length region directly in one go rather than per-dab, so deferring the
     * stitch left the surface visibly warped at grid boundaries in between. Scope the stitch to just
     * the faces this pass touched (`nodes_to_face_selection_grids`) instead of
     * `BKE_subdiv_ccg_average_grids()`'s whole-mesh pass, keeping the restamp O(patch footprint) on
     * every interactive drag event. `sculpt_mask_init.cc`/`sculpt_filter_mask.cc`/`sculpt_expand.cc`
     * stitch the same way after their own bulk direct `SubdivCCG` writes. */
    IndexMaskMemory memory;
    const IndexMask faces = bke::pbvh::nodes_to_face_selection_grids(
        *subdiv_ccg, pbvh.nodes<bke::pbvh::GridsNode>(), node_mask, memory);
    BKE_subdiv_ccg_average_stitch_faces(*subdiv_ccg, faces);
  }

  /* Unlike the old N-dab re-stamp (which went through `do_brush_action` and inherited its own
   * `pbvh.tag_positions_changed(node_mask)` at `sculpt.cc:3187`), this direct relief action writes
   * the position array directly. The PBVH caches the vertex positions its draw path reads in each
   * node; without invalidating those caches here, the viewport keeps rendering the pre-stamp
   * positions even though the underlying data is correct -- which was the cause of the "smooth
   * cube, no relief visible" symptom. `do_brush_action`'s own call is the reference. */
  pbvh.tag_positions_changed(tag_mask);

  /* Remember the nodes this pass displaced (accumulated across symmetry passes) so the NEXT
   * restamp's `curve_patch_restore_only()` can revert exactly these nodes' normals.
   * `curve_patch_restore_and_restamp()` sizes and clears this bit set before the first pass runs.
   * Deliberately replaces the former `mesh->tag_positions_changed()` here, whose whole-mesh
   * normals-cache invalidation was the dominant cost of the interactive-edit slowdown. */
  tag_mask.set_bits(patch.last_restamp_nodes);
}

/**
 * Light smoothing of the finished relief, run once when a patch is committed.
 *
 * Averages each displaced vertex's DISPLACEMENT -- its offset from the pre-patch position -- with
 * its mesh neighbours'. Vertices the patch never touched hold a zero displacement and are read but
 * never written, so the strip's edge is pulled toward its undisplaced surroundings (the requested
 * softening of hard transitions) while the patch's footprint stays exactly what it was and
 * `curve_patch_restore_only()` remains able to revert it.
 *
 * Smoothing the displacement rather than a scalar height along the normal is deliberate: the normals
 * the relief displaced along live in a cache that the position writes have already invalidated, and
 * re-fetching it would yield the normals of the DISPLACED surface, not the ones actually used.
 * Working on the offset vectors avoids needing them at all.
 *
 * This replaces a supersampling attempt that sampled the texture several times per vertex. That
 * failed for a structural reason worth recording: a handful of sparse taps is a sum of shifted
 * copies of the texture, not a filter, so unless the offsets are smaller than the texture's own
 * detail the copies stay separately visible and the pattern reads as ghosted. Its offsets were a
 * fraction of the strip width, which is unrelated to the mesh's vertex spacing -- the sampling rate
 * anti-aliasing has to match -- so on a dense mesh they were enormous. Re-weighting the taps does
 * not help; it only changes how strong each copy is.
 *
 * Multires is deliberately not handled: its grids duplicate boundary elements, so it would need the
 * CCG neighbour API plus a re-stitch afterwards. Not worth carrying until the mesh case is proven.
 */
static void curve_patch_smooth_relief(Object &ob, const CurvePatchCache &patch)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if (pbvh.type() != bke::pbvh::Type::Mesh || patch.orig_positions.is_empty()) {
    return;
  }

  /* Deliberately gentle: this is meant to take the hard edge off the profile, not to erase the
   * texture's own detail. */
  constexpr int smooth_iters = 2;
  constexpr float mix = 0.5f;

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  const Span<int2> edges = mesh.edges();
  const int64_t verts_num = positions.size();

  /* Dense rather than a map, so the per-edge gather below is a plain indexed read. */
  Array<float3> disp(verts_num, float3(0.0f));
  for (const auto item : patch.orig_positions.items()) {
    disp[item.key] = positions[item.key] - item.value;
  }

  Array<float3> accum(verts_num);
  Array<int> neighbor_num(verts_num);
  for ([[maybe_unused]] const int iter : IndexRange(smooth_iters)) {
    accum.fill(float3(0.0f));
    neighbor_num.fill(0);
    for (const int2 &edge : edges) {
      accum[edge[0]] += disp[edge[1]];
      neighbor_num[edge[0]]++;
      accum[edge[1]] += disp[edge[0]];
      neighbor_num[edge[1]]++;
    }
    /* `accum` is complete before anything is written back, so every vertex is relaxed against the
     * PREVIOUS iteration's values -- the result does not depend on the order the map is walked. */
    for (const auto item : patch.orig_positions.items()) {
      const int vert = item.key;
      if (neighbor_num[vert] > 0) {
        disp[vert] = math::interpolate(
            disp[vert], accum[vert] / float(neighbor_num[vert]), mix);
      }
    }
  }

  for (const auto item : patch.orig_positions.items()) {
    positions[item.key] = item.value + disp[item.key];
  }

  IndexMaskMemory memory;
  pbvh.tag_positions_changed(IndexMask::from_bits(patch.last_restamp_nodes, memory));
  mesh.tag_positions_changed_no_normals();
}

}  // namespace

void curve_patch_restore_and_restamp(bContext &C, Object &ob, CurvePatchCache &patch)
{
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
  patch.spline.build_from_positions(
      geom.evaluated_positions(), evaluated_radii.as_span(), cyclic);

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
                               patch.stamps);
      /* A closed curve has no ends to extend (see `ribbon_end_margin` below), so the stamp at the
       * join would lose the half that reaches into `v < 0` -- which on a loop is not outside the
       * curve but the stretch just before the join. Wrap those stamps around instead, so both
       * halves are present and meet exactly at the seam. The bound must be the same one the
       * per-vertex search window uses, hence the shared #curve_patch_stamp_reach. */
      if (patch.spline.cyclic) {
        curve_patch_stamps_add_cyclic_wrap(patch.stamps,
                                           patch.spline.total_length(),
                                           curve_patch_stamp_reach(patch.frozen_params.radius));
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
       * further along the curve. The same shared bound the per-vertex search window uses on `v`. */
      patch.ribbon_end_margin = curve_patch_stamp_reach(patch.frozen_params.radius) + jitter_amount;
    }
  }

  /* Rebuild the ribbon UV LUT the relief action samples in place of
   * `CurvePatchSpline::closest_point()` (see `paint_curve_patch_ribbon.hh`). Built once per
   * restamp on the main thread; PHASE 1's parallel `compute_vertex()` walk only reads it. */
  curve_patch_ribbon_build(patch.spline,
                           patch.ribbon_radius,
                           patch.ribbon,
                           patch.final_quality,
                           patch.ribbon_end_margin);
#if CURVE_PATCH_PROFILING
  const double prof_t_ribbon = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  if (!patch.ribbon.ready) {
    return;
  }

  ToolSettings *tool_settings = CTX_data_tool_settings(&C);
  Sculpt &sd = *tool_settings->sculpt;
  PaintModeSettings &paint_mode_settings = tool_settings->paint_mode;
  Scene *scene = CTX_data_scene(&C);
  const Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(&C);

  /* A normal interactive stroke keeps the vertex-normals #SharedCache fresh via the Paint BVH
   * draw engine between dabs; this recompute runs synchronously with no redraw in between, so it
   * must refresh normals itself before reading `mesh.vert_normals()` inside the relief action
   * above. */
#if CURVE_PATCH_PROFILING
  const double prof_t_pre_norm = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif
  bke::pbvh::update_normals(depsgraph, ob, *bke::object::pbvh_get(ob));
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
   * `curve_patch_apply_relief_action()`); scale by `ribbon_radius` here too so this search sphere
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
                               curve_patch_apply_relief_action,
                               paint_mode_settings,
                               std::nullopt);

  /* Commit only: soften the finished profile once every symmetry pass has contributed. Deliberately
   * after `do_symmetrical_brush_actions()` rather than inside the relief action -- smoothing a
   * single pass's result would fight the cross-pass blend the next pass performs. */
  if (patch.final_quality) {
    curve_patch_smooth_relief(ob, patch);
  }
#if CURVE_PATCH_PROFILING
  const double prof_t_relief = BLI_time_now_seconds(); /* DEBUG-cpatch */
#endif

  /* A normal interactive stroke calls this once per step (`SculptPaintStroke::update_step()`,
   * `mesh/sculpt.cc:6032`) -- without it here, nothing tells the depsgraph the object needs
   * re-shading, sets `RV3D_PAINTING` (the fast redraw path the PBVH-draw viewport relies on), or
   * refreshes the mesh's eager bounds, so the re-stamped positions never actually reach the
   * screen even though the underlying vertex data is correct. */
  flush_update_step(patch.view_context, ob, UpdateType::Position);

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
  printf(
      "[DEBUG-cpatch] total=%.2fms | restore=%.2f ribbon=%.2f normals=%.2f relief=%.2f flush=%.2f | "
      "nodes=%lld displaced=%lld snapshot=%lld quality=%d lut=%d interval=%.2fms\n",
      (prof_t_end - prof_t0) * 1000.0,
      (prof_t_restore - prof_t0) * 1000.0,
      (prof_t_ribbon - prof_t_restore) * 1000.0,
      (prof_t_norm - prof_t_pre_norm) * 1000.0,
      (prof_t_relief - prof_t_norm) * 1000.0,
      (prof_t_end - prof_t_relief) * 1000.0,
      (long long)prof_nodes,
      (long long)patch.pass_weight_accum.size(),
      (long long)patch.orig_positions.size(),
      patch.final_quality ? 1 : 0,
      patch.ribbon.res,
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
                                      const float frozen_radius)
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

  /* `cache.vc` otherwise still points at the just-finished stroke's own `ViewContext`, torn down
   * together with that stroke's operator. Repoint it at this patch's owned copy so every re-stamp's
   * `calc_local_from_screen()` (via `cache->vc`) dereferences valid memory for the whole lifetime
   * of the patch (see `CurvePatchCache::view_context`). */
  patch->view_context = vc;
  cache.vc = &patch->view_context;

  ss.curve_patch_cache = patch;

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
   * location. `CurvePatchCache::orig_positions` below only snapshots a vertex's position the
   * first time THIS patch's own re-stamp touches it, so if that vertex is still displaced by the
   * anchor dab right now, the patch would adopt the displaced position as "original" and could
   * never restore past it -- moving curve points would only ever add on top of that permanently
   * baked-in bump instead of describing the whole shape from a clean baseline. Restore the mesh
   * to its pristine pre-stroke state before anything else touches it, while
   * `ss.curve_patch_cache` is still null (see the matching branch in
   * `restore_from_undo_step_if_necessary()`, `mesh/sculpt.cc`). */
  restore_from_undo_step_if_necessary(depsgraph, sd, ob);

  /* Dynamic Topology has no stable per-vertex index across recomputes (`BMVert`s themselves are
   * created/destroyed as topology changes), which `CurvePatchCache::orig_positions` and the
   * relief re-stamp in `curve_patch_apply_relief_action()` both depend on. Refuse rather than
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

  curve_patch_begin_editing(ob, brush, vc, patch, cache.sculpt_normal, cache.initial_radius);
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

  /* Dynamic Topology has no stable per-vertex index for `CurvePatchCache::orig_positions`; refuse
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
  curve_patch_begin_editing(ob, brush, vc, patch, math::normalize(plane), cache.initial_radius);
}

}  // namespace blender::ed::sculpt_paint
