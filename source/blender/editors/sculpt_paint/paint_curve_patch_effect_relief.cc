/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The relief target of a Curve Patch session: displaces element positions along their own normals
 * by the magnitude `CurvePatchSampler` reports, and owns everything needed to take that back --
 * the pre-patch snapshot, the per-restamp restore, the commit-time undo step and the face set.
 */

#include <optional>

#include "paint_curve_patch_effect.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.h"
#include "BLI_task.hh"

#include "ED_sculpt.hh"

#include "paint_curve_patch_cache.hh"
#include "paint_curve_patch_sampler.hh"
#include "paint_intern.hh"

#include "mesh/mesh_brush_common.hh"
#include "mesh/sculpt_face_set.hh"
#include "mesh/sculpt_intern.hh"
#include "mesh/sculpt_undo.hh"

namespace blender::ed::sculpt_paint {

namespace {

class ReliefEffect : public CurvePatchEffect {
 public:
  int64_t element_num(Object &ob) const override;
  void restore(Object &ob, const CurvePatchCache &patch) override;
  void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchCache &patch) override;
  void apply_pass(const Depsgraph &depsgraph,
                  Object &ob,
                  const Brush &brush,
                  CurvePatchCache &patch) override;
  void end_restamp(bContext &C, Object &ob, CurvePatchCache &patch) override;
  void commit(bContext &C, Object &ob, const CurvePatchCache &patch) override;
  int64_t snapshot_size() const override;

 private:
  void smooth_relief(Object &ob, const CurvePatchCache &patch);
  void push_position_step(bContext &C,
                          Object &ob,
                          const CurvePatchCache &patch,
                          bool force_push);
  bool face_set_masks(Object &ob,
                      const CurvePatchCache &patch,
                      IndexMaskMemory &memory,
                      IndexMask &r_face_mask,
                      IndexMask &r_node_mask);

  /** Lazily-grown snapshot of original (pre-patch) element positions, keyed by a flat index into
   * whichever position array is authoritative for the object's current `bke::pbvh::Type` -- a
   * mesh vertex index into `Mesh::vert_positions()` for `Type::Mesh`, or a flat CCG element index
   * into `SubdivCCG::positions` (`grid * grid_area + in-grid offset`) for `Type::Grids`. Never
   * both at once: an object's pbvh type does not change while a patch is alive. An element is
   * inserted the first time it is about to be touched by a re-stamp, never removed until the
   * whole patch is destroyed.
   *
   * It is NOT merely undo bookkeeping: `CurvePatchSampler` reads it as the pre-patch position, so
   * a later symmetry pass of the same restamp sees the true original rather than the result an
   * earlier pass already wrote. */
  Map<int, float3> orig_positions_;
};

/** Number of elements the patch's `orig_positions` keys index into, for the object's CURRENT pbvh
 * type. Returns 0 for `Type::BMesh`, which a patch never runs on. */
int64_t ReliefEffect::element_num(Object &ob) const
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      return id_cast<Mesh *>(ob.data)->verts_num;
    case bke::pbvh::Type::Grids:
      return ob.runtime->sculpt_session->subdiv_ccg->positions.size();
    case bke::pbvh::Type::BMesh:
      return 0;
  }
  return 0;
}

void ReliefEffect::restore(Object &ob, const CurvePatchCache &patch)
{
  if (this->element_num(ob) != patch.element_num) {
    /* See `CurvePatchCache::element_num`: writing the snapshot back would corrupt an unrelated
     * mesh. This also makes the ordinary cancel path safe for an invalidated patch. */
    return;
  }
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  /* `orig_positions_` is keyed by whichever flat index `ReliefEffect::apply_pass()`
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
    for (const auto item : orig_positions_.items()) {
      subdiv_ccg.positions[item.key] = item.value;
    }
    /* `BKE_subdiv_ccg_average_grids()` was tried here to re-stitch duplicate boundary/corner
     * elements, but it *averages* every duplicate pair rather than copying the known-good side
     * into the stale one: whenever only one side of a pair is a key in `orig_positions` (true at
     * the edge of the patch's touched footprint, which shifts every restamp during interactive
     * dragging), it blends the value just restored above with the neighboring, not-yet-restored
     * duplicate's stale value -- corrupting the very position this loop just fixed. Every element
     * of every grid this patch has ever touched already has its own exact entry in
     * `orig_positions` (`ReliefEffect::apply_pass()`'s `lookup_or_add` runs unconditionally
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
  for (const auto item : orig_positions_.items()) {
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

/** Matches the raw `BrushActionFunc` signature `do_symmetrical_brush_actions()` expects
 * (`mesh/sculpt_intern.hh:855`) so it can be passed as its `action` callback. Called once per
 * enabled symmetry pass (mirror x radial x tile) with `StrokeCache::location_symm`/`radius`
 * already set (by `cache_calc_brushdata_symm()`, invoked internally by
 * `do_symmetrical_brush_actions()`) to the whole curve's encompassing sphere in that pass's
 * transformed space. Unlike a normal brush dab, this walks every vertex the sphere query returns
 * and applies the direct texture-driven relief formula to it directly -- no `sculpt_brush_type`
 * dispatch, no `do_brush_action()` call. */
void ReliefEffect::apply_pass(const Depsgraph &depsgraph,
                              Object &ob,
                              const Brush &brush,
                              CurvePatchCache &patch)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  StrokeCache &cache = *ss.cache;

  IndexMaskMemory memory;
  const brushes::CursorSampleResult cursor_sample_result = calc_brush_node_mask(
      depsgraph, ob, brush, memory);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* Curve Patch bypasses `do_brush_action()`'s own per-`bke::pbvh::Type` node dispatch (see the
   * direct-write comment at the end of this function), so it picks its own flat position/normal
   * arrays here instead: `mesh.vert_positions_for_write()`/`mesh.vert_normals()` for a regular
   * mesh, or `SubdivCCG::positions`/`::normals` for Multires -- both indexed by the same flat
   * "grid * grid_area + in-grid offset" scheme `bke::ccg::grid_range()` uses below. Either way,
   * `orig_positions_` ends up keyed by whichever flat index this array uses, not necessarily
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

  const CurvePatchSourceGeometry source{positions, normals, &orig_positions_};
  const CurvePatchSampler sampler(patch, cache, brush, source, mask, ss.tex_pool);

  const float max_radius = curve_patch_max_radius(patch);

  IndexMaskMemory culled_memory;
  const IndexMask node_mask = curve_patch_cull_nodes(
      patch, cache, pbvh, cursor_sample_result.node_mask, max_radius, culled_memory);

  BitVector<> grid_keep;
  if (subdiv_ccg) {
    grid_keep = curve_patch_cull_grids(
        patch, cache, pbvh, *subdiv_ccg, key, positions, node_mask, max_radius);
  }

  /* PHASE 1 (parallel, read-only): each pbvh node is processed on a worker thread; surviving
   * vertices and their displaced target positions are gathered into a thread-local buffer. No
   * position or snapshot-map writes happen here, so the reads inside `compute_vertex()` are
   * race-free across threads (texture sampling uses the per-thread pool slot `thread_id`). */

  /* Per-vertex outcome of `CurvePatchSampler::sample()`: the true pre-patch position (so PHASE 2
   * never has to re-derive it from a `positions` array another pass may have already written to),
   * the raw relief height along the vertex's own normal, and the falloff weight this pass claims the
   * vertex with. PHASE 2 blends `height`/`weight` across every pass that claims the same vertex
   * within this restamp (see `patch.pass_weight_accum`) rather than letting the last pass to run
   * unconditionally overwrite an earlier pass's result -- the fix for a patch straddling a
   * mirror/radial symmetry plane, where the direct and mirrored passes can both legitimately claim
   * the same real vertex and previously fought over which one's displacement "won". */
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
              if (const std::optional<CurvePatchSample> relief = sampler.sample(vert, thread_id)) {
                local.writes.append({vert, relief->orig, relief->value, relief->weight});
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
            if (const std::optional<CurvePatchSample> relief = sampler.sample(idx, thread_id)) {
              local.writes.append({idx, relief->orig, relief->value, relief->weight});
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
          orig_positions_.lookup_or_add(idx, positions[idx]);
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

  for (LocalData &local : all_tls) {
    for (const ReliefWrite &write : local.writes) {
      /* On a regular mesh this is the sole snapshot of `idx`; on Multires it was already recorded by
       * the whole-grid pass above, so `lookup_or_add` just returns the existing original. Uses
       * `write.orig` (computed once in PHASE 1) rather than re-reading `positions[write.idx]` here,
       * since an earlier symmetry pass of THIS restamp may already have written a blended result to
       * it below. */
      orig_positions_.lookup_or_add(write.idx, write.orig);

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

  /* The same bits also accumulate into the patch's lifetime union, which is never cleared and is
   * what the commit-time undo step is pushed over -- see `CurvePatchCache::all_touched_nodes`. */
  tag_mask.set_bits(patch.all_touched_nodes);
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
void ReliefEffect::smooth_relief(Object &ob, const CurvePatchCache &patch)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if (pbvh.type() != bke::pbvh::Type::Mesh || orig_positions_.is_empty()) {
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
  for (const auto item : orig_positions_.items()) {
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
    for (const auto item : orig_positions_.items()) {
      const int vert = item.key;
      if (neighbor_num[vert] > 0) {
        disp[vert] = math::interpolate(
            disp[vert], accum[vert] / float(neighbor_num[vert]), mix);
      }
    }
  }

  for (const auto item : orig_positions_.items()) {
    positions[item.key] = item.value + disp[item.key];
  }

  IndexMaskMemory memory;
  /* The wide mask, not `last_restamp_nodes`: this function writes every key of `orig_positions`,
   * including ones whose nodes the last restamp never touched -- the same reason
   * `CurvePatchCache::all_touched_nodes` exists. A narrower tag leaves a fringe node drawing its
   * pre-smoothing positions. */
  pbvh.tag_positions_changed(IndexMask::from_bits(patch.all_touched_nodes, memory));
  mesh.tag_positions_changed_no_normals();
}

/** Largest displacement any snapshotted element has undergone, in scene units. `positions` must be
 * the array `orig_positions_` is keyed into for the object's current `bke::pbvh::Type`. */
static float curve_patch_max_displacement(const Span<float3> positions,
                                          const Map<int, float3> &orig_positions)
{
  float max_disp = 0.0f;
  for (const auto item : orig_positions.items()) {
    max_disp = math::max(max_disp, math::distance(positions[item.key], item.value));
  }
  return max_disp;
}

/**
 * Record the patch's whole footprint as ONE position undo step.
 *
 * `undo::push_nodes()` stores each node's CURRENT positions as the state to restore, but by the
 * time a patch commits the mesh already carries the finished relief. Recomputing the pristine
 * surface would mean a second final-quality re-stamp, which is expensive on a dense mesh and shows
 * up as a stall on Enter. Instead the snapshot the patch has kept all along is written back, the
 * nodes are pushed against it, and the relief is restored from a saved copy -- two passes over the
 * touched elements and no relief evaluation at all.
 */
void ReliefEffect::push_position_step(bContext &C,
                                      Object &ob,
                                      const CurvePatchCache &patch,
                                      const bool force_push)
{
  const Scene &scene = *CTX_data_scene(&C);
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(&C);
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* The array `orig_positions` is keyed into, and the same one `ReliefEffect::apply_pass()`
   * writes the relief to -- there is no intermediate buffer on either path. */
  MutableSpan<float3> positions;
  if (pbvh.type() == bke::pbvh::Type::Grids) {
    positions = ob.runtime->sculpt_session->subdiv_ccg->positions;
  }
  else {
    positions = id_cast<Mesh *>(ob.data)->vert_positions_for_write();
  }

  /* Saved in the map's own iteration order. The write-back loop below walks it the same way, which
   * is well-defined precisely because nothing between the two loops mutates the map. */
  Vector<float3> relief;
  relief.reserve(orig_positions_.size());
  for (const auto item : orig_positions_.items()) {
    relief.append(positions[item.key]);
    positions[item.key] = item.value;
  }

  IndexMaskMemory memory;
  undo::push_begin_ex(scene, ob, "Curve Patch");
  undo::push_nodes(depsgraph,
                   ob,
                   IndexMask::from_bits(patch.all_touched_nodes, memory),
                   undo::Type::Position);

  int i = 0;
  for (const auto item : orig_positions_.items()) {
    positions[item.key] = relief[i++];
  }

  /* Forced into the stack only when a face-set step follows: that step's `push_begin_ex()` would
   * free this one out of `ustack->step_init` (this operator carries `OPTYPE_UNDO`, so
   * `wm->op_undo_depth` is non-zero here and a plain `push_end()` parks rather than pushes).
   * Forcing is safe because the step was opened a few lines above and no foreign code runs in
   * between, so it is always ours -- that is why the old `patch_step_is_ours` test is gone.
   * Precedent for the forced form: `mesh/sculpt_dyntopo.cc`.
   *
   * When no face-set step follows, the step MUST stay parked instead: `wm_operator_finished()`
   * pushes it for us, whereas an already-pushed step leaves `step_init` empty and makes that call
   * allocate an EXTRA, empty step -- one dead Ctrl+Z press before the relief is undone. */
  undo::push_end_ex(ob, force_push);
}

/**
 * Decide whether the commit will really burn a face set, and if so hand back the masks it needs.
 *
 * Runs BEFORE the position step is closed, because whether that step may be forced into the stack
 * depends on whether a second step follows it -- see `curve_patch_push_position_step()`. Every
 * "no face set after all" outcome is reported as `false` rather than by an early return from the
 * commit, so the position step is still built on those paths.
 *
 * `memory` must outlive both returned masks.
 */
bool ReliefEffect::face_set_masks(Object &ob,
                                  const CurvePatchCache & /*patch*/,
                                  IndexMaskMemory &memory,
                                  IndexMask &r_face_mask,
                                  IndexMask &r_node_mask)
{
  const SculptSession &ss = *ob.runtime->sculpt_session;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* Read the LIVE brush rather than `patch.frozen_params`: this toggle is a commit-time behavior
   * switch, not a relief parameter, so the user may flip it while the patch is being edited. Same
   * live-sync pattern the texture-source toggles use in `curve_patch_restore_and_restamp()`. */
  const Brush *brush = ss.cache ? BKE_paint_brush_for_read(ss.cache->paint) : nullptr;
  if (brush == nullptr || brush->curve_patch_face_set == 0) {
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  BitVector<> raised_faces(mesh.faces_num, false);

  if (pbvh.type() == bke::pbvh::Type::Grids) {
    const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
    const Span<float3> positions = subdiv_ccg.positions;

    const float max_disp = curve_patch_max_displacement(positions, orig_positions_);
    if (max_disp <= 0.0f) {
      /* Exactly zero rather than an epsilon, for the same reason as the Mesh branch below. */
      return false;
    }
    const float threshold = max_disp * 0.005f;

    /* Every grid belongs to exactly one base-mesh face, so a raised grid element marks its face
     * directly -- no vertex indirection and no `raised_verts` step on this path. Sequential rather
     * than parallel: this walks a `Map`, whose own slot iteration is single-threaded anyway (the
     * same reason `curve_patch_restore_only()` gave up on parallelizing its equivalent loop). */
    for (const auto item : orig_positions_.items()) {
      if (math::distance(positions[item.key], item.value) > threshold) {
        raised_faces[subdiv_ccg.grid_to_face_map[item.key / subdiv_ccg.grid_area]].set();
      }
    }
  }
  else {
    const Span<float3> positions = mesh.vert_positions();

    /* Pass 1: the patch's own maximum displacement, which the threshold is a fraction of. */
    const float max_disp = curve_patch_max_displacement(positions, orig_positions_);
    if (max_disp <= 0.0f) {
      /* A patch that displaced nothing (fully transparent texture, zero strength) must not burn a
       * face set ID. Compared against exactly zero rather than an epsilon: an absolute epsilon in
       * scene units would false-trigger on a heavily scaled-down object, and the relative threshold
       * derived from `max_disp` below already handles anything that did move but only barely. */
      return false;
    }
    const float threshold = max_disp * 0.005f;

    /* Pass 2: which vertices cleared the threshold. Displacement is applied along the vertex normal,
     * so this distance IS the relief height. */
    BitVector<> raised_verts(mesh.verts_num, false);
    for (const auto item : orig_positions_.items()) {
      if (math::distance(positions[item.key], item.value) > threshold) {
        raised_verts[item.key].set();
      }
    }

    const OffsetIndices<int> faces = mesh.faces();
    const Span<int> corner_verts = mesh.corner_verts();

    /* A face joins the set if ANY of its vertices was raised, so the set covers the relief's slopes
     * as well as its plateau rather than stopping short of them. Kept as a bit vector rather than
     * going straight to an `IndexMask`, because the node mask below has to test membership per face.
     * Filled through `parallel_for_aligned()` rather than plain `parallel_for()`: a `BitVector` bit
     * write is a non-atomic `*int_ |= mask_` read-modify-write on the 64-bit word the bit lives in
     * (`BLI_bit_ref.hh`), so two threads landing on faces whose indices share a word would race --
     * `BLI_bit_vector.hh` documents this directly ("Writing to separate bits in the same int is not
     * thread-safe"). Aligning sub-ranges to `bits::BitsPerInt` guarantees every thread's slice starts
     * and ends on a word boundary, so no two threads ever touch the same word. Same pattern
     * `enabled_state_to_bitmap()` in `sculpt_expand.cc` uses to fill a per-vertex `BitVector` in
     * parallel. */
    threading::parallel_for_aligned(
        faces.index_range(), 2048, bits::BitsPerInt, [&](const IndexRange range) {
          for (const int face : range) {
            for (const int vert : corner_verts.slice(faces[face])) {
              if (raised_verts[vert]) {
                raised_faces[face].set();
                break;
              }
            }
          }
        });
  }

  r_face_mask = IndexMask::from_bits(raised_faces, memory);
  if (r_face_mask.is_empty()) {
    return false;
  }

  /* Derived from the faces actually being written, NOT from `patch.last_restamp_nodes`. A node
   * enters that set only when one of its OWN unique vertices moved, but a face joins the face mask
   * when any of its corners moved -- and the faces around a vertex are spread across neighboring
   * nodes. A node owning such a face would otherwise be missing here, leaving that face's previous
   * face set unsaved (so Ctrl+Z could not restore it) and its draw buffers untagged. On the Mesh
   * path there is a second way the two could disagree: committing also runs
   * `curve_patch_smooth_relief()`, which widens the displaced set without widening
   * `last_restamp_nodes`. Deriving the node mask from the face mask makes them agree by
   * construction, on both paths. */
  if (pbvh.type() == bke::pbvh::Type::Grids) {
    const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
    const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
    /* A `GridsNode` carries grids, not faces, and one face's grids can straddle two nodes -- so
     * every node holding ANY grid of a raised face is included, which is what keeps the undo record
     * and the redraw tag complete on both sides of such a split. */
    r_node_mask = IndexMask::from_predicate(
        nodes.index_range(),
        memory,
        [&](const int i) {
          for (const int grid : nodes[i].grids()) {
            if (raised_faces[subdiv_ccg.grid_to_face_map[grid]]) {
              return true;
            }
          }
          return false;
        },
        exec_mode::grain_size(64));
  }
  else {
    const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
    r_node_mask = IndexMask::from_predicate(
        nodes.index_range(),
        memory,
        [&](const int i) {
          for (const int face : nodes[i].faces()) {
            if (raised_faces[face]) {
              return true;
            }
          }
          return false;
        },
        exec_mode::grain_size(64));
  }

  return true;
}

void ReliefEffect::commit(bContext &C, Object &ob, const CurvePatchCache &patch)
{
  /* See `CurvePatchCache::element_num`. Nothing is pushed: an undo step built from a stale
   * snapshot would be worse than no step at all. */
  if (this->element_num(ob) != patch.element_num) {
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  /* Nothing was ever displaced: there is no state to record and no face set to burn. */
  if (orig_positions_.is_empty()) {
    return;
  }
  /* Dynamic Topology has no stable element index, so `orig_positions` cannot describe it and the
   * write-back in `curve_patch_push_position_step()` would index into an unrelated array. The
   * patch is refused outright at `curve_patch_start_from_anchor()`; guarded anyway so this stays
   * correct if that ever changes. */
  if (pbvh.type() == bke::pbvh::Type::BMesh) {
    return;
  }

  /* Computed FIRST, because how the position step below has to be closed depends on whether a
   * face-set step follows it. `memory` is declared out here so it outlives both masks. */
  IndexMaskMemory memory;
  IndexMask face_mask;
  IndexMask node_mask;
  const bool write_face_set = this->face_set_masks(ob, patch, memory, face_mask, node_mask);

  /* The patch's own undo step, built and closed before anything else can open one. Built on every
   * path that displaced something, including the ones that burn no face set. */
  this->push_position_step(C, ob, patch, write_face_set);

  if (!write_face_set) {
    return;
  }

  Mesh &mesh = *id_cast<Mesh *>(ob.data);

  /* A Face Set write cannot share the patch's step: a step carries exactly one `undo::Type`, and
   * appending `FaceSet` to a `Position` step would retype it, silently stripping the position
   * storage Ctrl+Z needs. The cost is that undoing the commit takes two presses -- face set first,
   * then relief -- which is the tradeoff that was chosen deliberately. */
  const Scene &scene = *CTX_data_scene(&C);
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(&C);
  undo::push_begin_ex(scene, ob, "Curve Patch Face Set");
  undo::push_nodes(depsgraph, ob, node_mask, undo::Type::FaceSet);

  /* Order matters: the attribute has to EXIST before the next available ID is queried. On a mesh
   * that never had face sets, `find_next_available_id()` reads an empty span and answers 1, while
   * `ensure_face_sets_mesh()` then creates every face at 1 -- so querying first would put the whole
   * mesh and this patch in the same set. Creating first makes the answer 2. Same ordering the trim
   * gesture relies on (`sculpt_trim.cc` creates in `gesture_begin`). */
  face_set::create_face_sets_mesh(ob);
  const int face_set_id = face_set::find_next_available_id(ob);

  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(mesh);
  face_mask.foreach_index(
      [&](const int face) { face_sets.span[face] = face_set_id; }, exec_mode::grain_size(4096));
  /* Values changed, topology did not, so the nodes only need their face-set data refreshed -- no
   * PBVH rebuild, no `islands::invalidate()`. Tagged before the writer is finished, matching the
   * order `face_sets_update()` uses in `mesh/sculpt_face_set.cc`. */
  pbvh.tag_face_sets_changed(node_mask);

  /* `ensure_face_sets_mesh()` hands back an open writer -- unlike `create_face_sets_mesh()`, it
   * MUST be finished explicitly (see `sculpt_face_set.hh`). */
  face_sets.finish();

  /* Left parked in `ustack->step_init` on purpose: this is the last step of the operator, so
   * `wm_operator_finished()` pushes it via the operator's own `OPTYPE_UNDO`. */
  undo::push_end(ob);
}

void ReliefEffect::begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchCache & /*patch*/)
{
  /* A normal interactive stroke keeps the vertex-normals #SharedCache fresh via the Paint BVH
   * draw engine between dabs; this recompute runs synchronously with no redraw in between, so it
   * must refresh normals itself before reading `mesh.vert_normals()` inside the relief action
   * above. */
  bke::pbvh::update_normals(depsgraph, ob, *bke::object::pbvh_get(ob));
}

void ReliefEffect::end_restamp(bContext & /*C*/, Object &ob, CurvePatchCache &patch)
{
  /* Commit only: soften the finished profile once every symmetry pass has contributed. Deliberately
   * after `do_symmetrical_brush_actions()` rather than inside the relief action -- smoothing a
   * single pass's result would fight the cross-pass blend the next pass performs. */
  if (patch.final_quality) {
    this->smooth_relief(ob, patch);
  }

  /* A normal interactive stroke calls this once per step (`SculptPaintStroke::update_step()`,
   * `mesh/sculpt.cc:6032`) -- without it here, nothing tells the depsgraph the object needs
   * re-shading, sets `RV3D_PAINTING` (the fast redraw path the PBVH-draw viewport relies on), or
   * refreshes the mesh's eager bounds, so the re-stamped positions never actually reach the
   * screen even though the underlying vertex data is correct. */
  flush_update_step(patch.view_context, ob, UpdateType::Position);
}

int64_t ReliefEffect::snapshot_size() const
{
  return orig_positions_.size();
}

}  // namespace

std::unique_ptr<CurvePatchEffect> curve_patch_effect_create(const Brush &brush, const Object &ob)
{
  if (!bke::brush::supports_curve_patch(brush)) {
    return nullptr;
  }
  /* Dynamic Topology has no stable per-element index for the snapshot to key into; the session
   * entry points refuse it outright, and this mirrors that refusal. */
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() == bke::pbvh::Type::BMesh) {
    return nullptr;
  }
  return std::make_unique<ReliefEffect>();
}

}  // namespace blender::ed::sculpt_paint
