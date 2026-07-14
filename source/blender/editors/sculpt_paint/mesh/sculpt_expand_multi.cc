/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#include "sculpt_expand_multi.hh"

#include <cfloat>
#include <queue>

#include "BLI_bounds.hh"
#include "BLI_map.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph_query.hh"

#include "sculpt_geodesic.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::expand {

Array<Array<float3>> world_positions_create(const Depsgraph &depsgraph, Span<Object *> objects)
{
  Array<Array<float3>> result(objects.size());
  for (const int i : objects.index_range()) {
    Object &object = *objects[i];
    const float4x4 to_world = object.object_to_world();

    Span<float3> local;
    if (bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids) {
      /* Never call vert_positions_eval on a Grids object -- it asserts Type::Mesh (pbvh.cc:997).
       * Grids positions live in the flattened CCG array instead. */
      const SculptSession &ss = *object.runtime->sculpt_session;
      local = ss.subdiv_ccg->positions;
    }
    else {
      local = bke::pbvh::vert_positions_eval(depsgraph, object);
    }

    Array<float3> world(local.size());
    threading::parallel_for(local.index_range(), 2048, [&](const IndexRange range) {
      for (const int v : range) {
        world[v] = math::transform_point(to_world, local[v]);
      }
    });
    result[i] = std::move(world);
  }
  return result;
}

float world_radius_to_local_search_radius(const float4x4 &world_to_object, float world_radius)
{
  /* A world displacement of length `world_radius` maps to at most
   * `world_radius * max_singular_value(world_to_object)` in local space. The linear part's largest
   * per-axis magnitude is a safe (over-inclusive) upper bound on that singular value, so the
   * returned local radius always covers the world sphere. */
  const float3x3 linear = float3x3(world_to_object);
  const float sx = math::length(linear[0]);
  const float sy = math::length(linear[1]);
  const float sz = math::length(linear[2]);
  const float bound = math::max(sx, math::max(sy, sz)) * float(M_SQRT3);
  return world_radius * bound;
}

/* -------------------------------------------------------------------- */
/** \name Multi-Object Proximity Bridge
 * \{ */

namespace detail {

Array<int> canonicalize_duplicates(
    const int vert_count,
    const FunctionRef<void(int vert, Vector<int> &r_duplicates)> duplicates_of)
{
  /* Plain union-find with path halving, no union-by-rank -- `vert_count` is one object's vertex
   * count (at most a few million for a dense Multires object), well within where the simpler
   * scheme is fast enough and easier to read than a ranked version. */
  Array<int> parent(vert_count);
  for (const int v : IndexRange(vert_count)) {
    parent[v] = v;
  }

  auto find = [&](int v) {
    while (parent[v] != v) {
      parent[v] = parent[parent[v]];
      v = parent[v];
    }
    return v;
  };

  Vector<int> duplicates;
  for (const int v : IndexRange(vert_count)) {
    duplicates.clear();
    duplicates_of(v, duplicates);
    for (const int d : duplicates) {
      const int root_v = find(v);
      const int root_d = find(d);
      if (root_v != root_d) {
        /* Smaller index becomes the parent, so the eventual representative is deterministically
         * the smallest index in the class regardless of visit order. */
        if (root_v < root_d) {
          parent[root_d] = root_v;
        }
        else {
          parent[root_v] = root_d;
        }
      }
    }
  }

  Array<int> canonical(vert_count);
  for (const int v : IndexRange(vert_count)) {
    canonical[v] = find(v);
  }
  return canonical;
}

GroupedSpan<int> build_canonical_neighbors(
    const int raw_vert_count,
    const Span<int> canonical_of_raw,
    const FunctionRef<void(int raw_vert, Vector<int> &r_real_neighbors)> real_neighbors_of,
    Array<int> &r_offsets,
    Array<int> &r_data)
{
  BLI_assert(canonical_of_raw.size() == raw_vert_count);

  /* Compact the raw representative values (which are sparse: e.g. {0,1,4,4,4,5} after
   * canonicalization) into a dense 0..N-1 canonical-id space, matching #propagate_uniform's
   * "one array slot per graph vertex" convention. */
  Map<int, int> compact_id;
  for (const int raw : IndexRange(raw_vert_count)) {
    const int rep = canonical_of_raw[raw];
    compact_id.lookup_or_add_cb(rep, [&]() { return int(compact_id.size()); });
  }
  const int canonical_count = compact_id.size();

  Vector<Set<int>> neighbor_sets(canonical_count);
  Vector<int> real;
  for (const int raw : IndexRange(raw_vert_count)) {
    const int from_id = compact_id.lookup(canonical_of_raw[raw]);
    real.clear();
    real_neighbors_of(raw, real);
    for (const int raw_neighbor : real) {
      const int to_id = compact_id.lookup(canonical_of_raw[raw_neighbor]);
      if (to_id == from_id) {
        /* A real neighbor that canonicalizes to the same vertex as the source (e.g. it was
         * itself a duplicate reported as "real" by a misbehaving caller) -- never a self-edge. */
        continue;
      }
      neighbor_sets[from_id].add(to_id);
      neighbor_sets[to_id].add(from_id);
    }
  }

  r_offsets.reinitialize(canonical_count + 1);
  int total = 0;
  for (const int i : IndexRange(canonical_count)) {
    r_offsets[i] = total;
    total += neighbor_sets[i].size();
  }
  r_offsets[canonical_count] = total;

  r_data.reinitialize(total);
  int cursor = 0;
  for (const int i : IndexRange(canonical_count)) {
    for (const int n : neighbor_sets[i]) {
      r_data[cursor++] = n;
    }
  }

  return {OffsetIndices<int>(r_offsets.as_span()), r_data.as_span()};
}

std::array<std::pair<int, int>, 4> interior_diagonal_offsets()
{
  return {{{-1, -1}, {-1, 1}, {1, -1}, {1, 1}}};
}

MultiObjectBridge build_bridge_impl(
    const Span<Span<float3>> object_world_positions,
    const Span<float> mean_world_edge_length,
    const float bridge_factor,
    const FunctionRef<int(int obj, const float3 &world_p, float max_world_dist)> nearest)
{
  BLI_assert(object_world_positions.size() == mean_world_edge_length.size());
  const int object_num = object_world_positions.size();

  /* Step 1: per-object world AABB. `std::nullopt` for an object with no vertices, which then
   * never overlaps any pair below. */
  Array<std::optional<Bounds<float3>>> object_bounds(object_num);
  for (const int i : IndexRange(object_num)) {
    object_bounds[i] = bounds::min_max(object_world_positions[i]);
  }

  /* A candidate stitch found while scanning one unordered object pair, before the cap/dedup
   * policy below decides which candidates actually become #BridgeEdge output. */
  struct Candidate {
    int a_obj;
    int a_vert;
    int b_obj;
    int b_vert;
    float gap;
  };
  Vector<Candidate> candidates;

  /* Step 2: for every unordered object pair (a < b) -- iterated in index order so candidate
   * order, and therefore tie-breaking below, is deterministic -- find mutually-nearest close
   * vertex pairs. Each unordered pair is scanned exactly once, so no (a, b) pair (and therefore no
   * resulting candidate quadruple) can ever be produced twice; the "spatially dedup" requirement
   * is thus satisfied structurally, without extra bookkeeping. */
  for (int a = 0; a < object_num; a++) {
    if (!object_bounds[a]) {
      continue;
    }
    const Span<float3> pos_a = object_world_positions[a];
    for (int b = a + 1; b < object_num; b++) {
      if (!object_bounds[b]) {
        continue;
      }
      const float threshold = bridge_factor *
                              math::min(mean_world_edge_length[a], mean_world_edge_length[b]);
      if (threshold <= 0.0f) {
        /* An edge-less (or zero-factor) pair bridges to nothing. */
        continue;
      }

      /* AABB prefilter (spec section 6.1): skip the whole pair when even the threshold-inflated
       * boxes cannot contain a close-enough vertex pair. */
      Bounds<float3> a_inflated = *object_bounds[a];
      a_inflated.pad(float3(threshold));
      Bounds<float3> b_inflated = *object_bounds[b];
      b_inflated.pad(float3(threshold));
      if (!a_inflated.intersects(b_inflated)) {
        continue;
      }

      const Span<float3> pos_b = object_world_positions[b];
      for (const int va : pos_a.index_range()) {
        const float3 &world_p = pos_a[va];
        if (!b_inflated.contains(world_p)) {
          /* Cheap per-vertex prefilter before paying for an actual nearest-vert query. */
          continue;
        }
        const int vb = nearest(b, world_p, threshold);
        if (vb == -1) {
          continue;
        }
        const float gap = math::distance(world_p, pos_b[vb]);
        if (gap > threshold) {
          continue;
        }
        /* Mutual-nearest: `va` only counts as a genuine stitch partner of `vb` if `va` is ALSO
         * the nearest vertex of `a` to `vb`'s position -- otherwise `va` is merely incidentally
         * close to `vb` while some other vertex of `a` is `vb`'s true nearest partner. */
        const int va2 = nearest(a, pos_b[vb], threshold);
        if (va2 != va) {
          continue;
        }
        candidates.append({a, va, b, vb, gap});
      }
    }
  }

  /* Step 3: cap at one edge per vertex, regardless of which side of a pair it played (the same
   * vertex can appear as the "a" side of one pair and the "b" side of another), keeping only the
   * smallest-gap candidate touching each vertex. A stitch survives only if it is the winner for
   * BOTH of its endpoints. Packing `(object, vert)` into a single 64-bit key lets one map serve
   * both sides. */
  auto vert_key = [](const int obj, const int vert) -> int64_t {
    return (int64_t(obj) << 32) | uint32_t(vert);
  };
  Map<int64_t, int> best_for_vert;
  for (const int i : candidates.index_range()) {
    const Candidate &candidate = candidates[i];
    const int64_t keys[2] = {vert_key(candidate.a_obj, candidate.a_vert),
                             vert_key(candidate.b_obj, candidate.b_vert)};
    for (const int64_t key : keys) {
      const int existing = best_for_vert.lookup_default(key, -1);
      if (existing == -1 || candidates[existing].gap > candidate.gap) {
        best_for_vert.add_overwrite(key, i);
      }
    }
  }

  /* Step 4: emit only the candidates that won the cap on both endpoints. */
  MultiObjectBridge result;
  for (const int i : candidates.index_range()) {
    const Candidate &candidate = candidates[i];
    if (best_for_vert.lookup(vert_key(candidate.a_obj, candidate.a_vert)) != i) {
      continue;
    }
    if (best_for_vert.lookup(vert_key(candidate.b_obj, candidate.b_vert)) != i) {
      continue;
    }
    result.edges.append({{candidate.a_obj, candidate.a_vert},
                         {candidate.b_obj, candidate.b_vert},
                         candidate.gap});
  }
  return result;
}

}  // namespace detail

MultiObjectBridge build_multi_object_bridge(const Depsgraph &depsgraph,
                                            const Span<Object *> objects,
                                            const Span<Array<float3>> world_positions,
                                            const float bridge_factor)
{
  /* Step 1: each object's average WORLD edge length sets its half of the pair threshold, so a
   * denser mesh bridges across a smaller gap than a coarse one. An edge-less object (e.g. a point
   * cloud) gets a mean of 0, which #detail::build_bridge_impl treats as "this object bridges to
   * nothing" (`threshold <= 0`). */
  Array<float> mean_world_edge_length(objects.size());
  for (const int i : objects.index_range()) {
    const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*objects[i]);
    if (pbvh.type() == bke::pbvh::Type::Grids) {
      /* Reuse the same canonical edge adjacency the propagation core builds from, so this mean is
       * computed over the exact same (duplicate-free) edge set the BFS graph uses -- no separate
       * dedup logic needed here. */
      const Array<int> canonical_map = grids_canonical_map_create(*objects[i]);
      Array<int> edge_offsets;
      Array<int> edge_data;
      const GroupedSpan<int> edges = grids_edge_neighbors_create(
          *objects[i], canonical_map, edge_offsets, edge_data);

      /* `edges` is indexed by CANONICAL id, but `world_positions[i]` is indexed by RAW flat CCG
       * index -- map each canonical id back to one representative raw index (any raw vertex
       * canonicalizing to it works, they are all coincident in world space by construction). */
      Array<int> raw_of_canonical(edges.size(), -1);
      {
        Map<int, int> compact_id;
        for (const int raw : canonical_map.index_range()) {
          const int rep = canonical_map[raw];
          const int id = compact_id.lookup_or_add_cb(rep, [&]() { return int(compact_id.size()); });
          if (raw_of_canonical[id] == -1) {
            raw_of_canonical[id] = raw;
          }
        }
      }

      double length_sum = 0.0;
      int64_t edge_count = 0;
      for (const int v : edges.index_range()) {
        for (const int n : edges[v]) {
          if (n <= v) {
            /* Each undirected edge appears at both endpoints; count it once. */
            continue;
          }
          length_sum += double(math::distance(world_positions[i][raw_of_canonical[v]],
                                              world_positions[i][raw_of_canonical[n]]));
          edge_count++;
        }
      }
      mean_world_edge_length[i] = edge_count > 0 ? float(length_sum / double(edge_count)) : 0.0f;
      continue;
    }

    const Mesh &mesh = *id_cast<const Mesh *>(objects[i]->data);
    const Span<int2> edges = mesh.edges();
    if (edges.is_empty()) {
      mean_world_edge_length[i] = 0.0f;
      continue;
    }
    /* Accumulate in double: some meshes have enough edges that a plain float sum would start
     * losing precision well before the loop finishes. */
    double length_sum = 0.0;
    for (const int2 &edge : edges) {
      length_sum += double(
          math::distance(world_positions[i][edge[0]], world_positions[i][edge[1]]));
    }
    mean_world_edge_length[i] = float(length_sum / double(edges.size()));
  }

  Array<Span<float3>> world_pos_spans(objects.size());
  for (const int i : objects.index_range()) {
    world_pos_spans[i] = world_positions[i].as_span();
  }

  /* Nearest-vertex functor: wraps the existing pbvh nearest-vert query, the same pattern as
   * #nearest_multi_vert in sculpt_expand.cc -- convert the world-space query point/radius into
   * object `obj`'s local space before searching, since #nearest_vert_calc_mesh's `max_distance`
   * and `location` are both in object-local units. */
  const auto nearest_fn = [&](const int obj,
                              const float3 &world_p,
                              const float max_world_dist) -> int {
    Object &object = *objects[obj];
    const float4x4 world_to_object = object.world_to_object();
    const float3 local_p = math::transform_point(world_to_object, world_p);
    const float local_radius = world_radius_to_local_search_radius(world_to_object,
                                                                   max_world_dist);
    const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

    if (pbvh.type() == bke::pbvh::Type::Grids) {
      const SculptSession &ss = *object.runtime->sculpt_session;
      const std::optional<SubdivCCGCoord> nearest = nearest_vert_calc_grids(
          pbvh, *ss.subdiv_ccg, local_p, local_radius, false);
      if (!nearest) {
        return -1;
      }
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      return nearest->to_index(key);
    }

    const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    const bke::AttributeAccessor attributes = mesh.attributes();
    const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
    const std::optional<int> nearest = nearest_vert_calc_mesh(
        pbvh, positions, hide_vert, local_p, local_radius, false);
    return nearest.value_or(-1);
  };

  return detail::build_bridge_impl(
      world_pos_spans, mean_world_edge_length, bridge_factor, nearest_fn);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Grids (Multires) Adapter
 * \{ */

Array<int> grids_canonical_map_create(const Object &object)
{
  BLI_assert(bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int vert_count = BKE_sculpt_get_grid_num_verts(object);

  return detail::canonicalize_duplicates(
      vert_count, [&](const int vert, Vector<int> &r_duplicates) {
        const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vert);
        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, true, neighbors);
        for (const SubdivCCGCoord &dup : neighbors.duplicates()) {
          r_duplicates.append(dup.to_index(key));
        }
      });
}

GroupedSpan<int> grids_edge_neighbors_create(const Object &object,
                                             const Span<int> canonical_map,
                                             Array<int> &r_offsets,
                                             Array<int> &r_data)
{
  BLI_assert(bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int vert_count = BKE_sculpt_get_grid_num_verts(object);

  return detail::build_canonical_neighbors(
      vert_count,
      canonical_map,
      [&](const int vert, Vector<int> &r_real_neighbors) {
        const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vert);
        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, true, neighbors);
        for (const SubdivCCGCoord &real : neighbors.unique()) {
          r_real_neighbors.append(real.to_index(key));
        }
      },
      r_offsets,
      r_data);
}

GroupedSpan<int> grids_diagonal_neighbors_create(const Object &object,
                                                 const Span<int> canonical_map,
                                                 Array<int> &r_offsets,
                                                 Array<int> &r_data)
{
  BLI_assert(bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int vert_count = BKE_sculpt_get_grid_num_verts(object);
  const std::array<std::pair<int, int>, 4> diag_offsets = detail::interior_diagonal_offsets();

  return detail::build_canonical_neighbors(
      vert_count,
      canonical_map,
      [&](const int vert, Vector<int> &r_real_neighbors) {
        const SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vert);

        /* Edge (real) neighbors are always same-quad neighbors too. */
        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, true, neighbors);
        for (const SubdivCCGCoord &real : neighbors.unique()) {
          r_real_neighbors.append(real.to_index(key));
        }

        /* Diagonal same-quad neighbors: interior coordinates only (documented scope narrowing --
         * see #grids_diagonal_neighbors_create's declaration comment). A coordinate is interior
         * here iff both its diagonal-offset x and y stay within [0, grid_size) of the SAME grid
         * for every one of the 4 offsets -- boundary/corner coordinates fail this for at least one
         * offset and simply get no diagonal edges added, keeping only the edge neighbors already
         * appended above. */
        for (const auto &[dx, dy] : diag_offsets) {
          const int x = int(coord.x) + dx;
          const int y = int(coord.y) + dy;
          if (x < 0 || x >= key.grid_size || y < 0 || y >= key.grid_size) {
            continue;
          }
          SubdivCCGCoord diag{};
          diag.grid_index = coord.grid_index;
          diag.x = short(x);
          diag.y = short(y);
          r_real_neighbors.append(diag.to_index(key));
        }
      },
      r_offsets,
      r_data);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multi-Object Graph Propagation
 * \{ */

namespace detail {

void propagate_uniform(const Span<GroupedSpan<int>> object_neighbors,
                       const MultiObjectBridge &bridge,
                       const Span<MultiVertRef> seeds,
                       MutableSpan<Array<float>> r_vert_falloff_per_object)
{
  BLI_assert(object_neighbors.size() == r_vert_falloff_per_object.size());

  /* Step 1: allocate one falloff array per object and initialize to "unreached", matching
   * #geodesic::distances_create's convention. */
  for (const int i : object_neighbors.index_range()) {
    r_vert_falloff_per_object[i].reinitialize(object_neighbors[i].size());
    r_vert_falloff_per_object[i].fill(FLT_MAX);
  }

  /* Step 2: build a per-object, per-vertex list of cross-object bridge partners. Bridge edges are
   * undirected, so each edge is recorded on both endpoints. */
  Array<Array<Vector<MultiVertRef>>> partners(object_neighbors.size());
  for (const int i : object_neighbors.index_range()) {
    partners[i] = Array<Vector<MultiVertRef>>(object_neighbors[i].size());
  }
  for (const BridgeEdge &edge : bridge.edges) {
    partners[edge.a.object_index][edge.a.vert].append(edge.b);
    partners[edge.b.object_index][edge.b.vert].append(edge.a);
  }

  /* Step 3: seed the BFS frontier at distance 0. */
  std::queue<MultiVertRef> queue;
  for (const MultiVertRef &seed : seeds) {
    float &falloff = r_vert_falloff_per_object[seed.object_index][seed.vert];
    if (falloff == 0.0f) {
      /* Already a seed (duplicate); skip re-enqueuing it. */
      continue;
    }
    falloff = 0.0f;
    queue.push(seed);
  }

  /* Step 4: plain FIFO BFS. Every edge (intra-object neighbor or cross-object bridge) has unit
   * weight, so BFS visitation order already gives correct shortest hop counts; each vertex is
   * relaxed (and re-enqueued) only when a shorter distance is found. */
  while (!queue.empty()) {
    const MultiVertRef cur = queue.front();
    queue.pop();
    const float d = r_vert_falloff_per_object[cur.object_index][cur.vert];

    for (const int neighbor : object_neighbors[cur.object_index][cur.vert]) {
      float &neighbor_falloff = r_vert_falloff_per_object[cur.object_index][neighbor];
      if (neighbor_falloff > d + 1.0f) {
        neighbor_falloff = d + 1.0f;
        queue.push({cur.object_index, neighbor});
      }
    }

    for (const MultiVertRef &partner : partners[cur.object_index][cur.vert]) {
      float &partner_falloff = r_vert_falloff_per_object[partner.object_index][partner.vert];
      if (partner_falloff > d + 1.0f) {
        partner_falloff = d + 1.0f;
        queue.push(partner);
      }
    }
  }
}

GlobalGeodesicTopology build_global_geodesic_topology(const Span<ObjectTopology> objects,
                                                       const MultiObjectBridge &bridge)
{
  /* Step 1: running per-object offsets into each global (concatenated) array. */
  Array<int> vert_offset(objects.size() + 1, 0);
  Array<int> edge_offset(objects.size() + 1, 0);
  Array<int> corner_offset(objects.size() + 1, 0);
  Array<int> face_offset(objects.size() + 1, 0);
  for (const int i : objects.index_range()) {
    vert_offset[i + 1] = vert_offset[i] + objects[i].positions.size();
    edge_offset[i + 1] = edge_offset[i] + objects[i].edges.size();
    corner_offset[i + 1] = corner_offset[i] + objects[i].corner_verts.size();
    face_offset[i + 1] = face_offset[i] + objects[i].faces.size();
  }
  const int total_verts = vert_offset[objects.size()];
  const int total_object_edges = edge_offset[objects.size()];
  const int total_corners = corner_offset[objects.size()];
  const int total_faces = face_offset[objects.size()];

  /* Step 2: global vertex positions (world space), one contiguous block per object. */
  Array<float3> global_positions(total_verts);
  for (const int i : objects.index_range()) {
    global_positions.as_mutable_span()
        .slice(vert_offset[i], objects[i].positions.size())
        .copy_from(objects[i].positions);
  }

  /* Step 3: global edges = every object's local edges (vertex indices shifted by `vert_offset`)
   * followed by the bridge edges, translated the same way via each endpoint's own object offset.
   * Bridge edges are appended LAST and referenced by no `corner_edges` entry, so the edge-to-face
   * map built below leaves them face-less; #geodesic::distances_create already treats a face-less
   * edge as a plain Euclidean-length relaxation (`sculpt_geodesic.cc`), which is exactly the
   * intended bridge behavior. */
  Array<int2> global_edges(total_object_edges + bridge.edges.size());
  {
    MutableSpan<int2> dst = global_edges.as_mutable_span();
    int edge_i = 0;
    for (const int i : objects.index_range()) {
      for (const int2 &edge : objects[i].edges) {
        dst[edge_i++] = int2(edge[0] + vert_offset[i], edge[1] + vert_offset[i]);
      }
    }
    for (const BridgeEdge &edge : bridge.edges) {
      dst[edge_i++] = int2(edge.a.vert + vert_offset[edge.a.object_index],
                           edge.b.vert + vert_offset[edge.b.object_index]);
    }
  }

  /* Step 4: global faces + corner_verts + corner_edges. Face corner-starts are the per-object
   * corner-start shifted by that object's `corner_offset`; the OffsetIndices contract requires the
   * final entry to equal the total corner count. `global_corner_edges` is only needed transiently
   * to build the edge-to-face map in step 6 below, so it is not retained past this function. */
  Array<int> global_face_offsets(total_faces + 1);
  Array<int> global_corner_verts(total_corners);
  Array<int> global_corner_edges(total_corners);
  {
    int corner_i = 0;
    for (const int i : objects.index_range()) {
      const OffsetIndices<int> &faces = objects[i].faces;
      for (const int f : faces.index_range()) {
        global_face_offsets[face_offset[i] + f] = faces[f].start() + corner_offset[i];
      }
      for (const int c : objects[i].corner_verts.index_range()) {
        global_corner_verts[corner_i] = objects[i].corner_verts[c] + vert_offset[i];
        global_corner_edges[corner_i] = objects[i].corner_edges[c] + edge_offset[i];
        corner_i++;
      }
    }
  }
  global_face_offsets[total_faces] = total_corners;
  const OffsetIndices<int> global_faces(global_face_offsets);

  /* Step 5: global hide_poly. Only allocated if at least one object actually hides faces; an empty
   * span means "nothing hidden" to #geodesic::distances_create. */
  bool any_hidden = false;
  for (const ObjectTopology &object : objects) {
    if (!object.hide_poly.is_empty()) {
      any_hidden = true;
      break;
    }
  }
  Array<bool> global_hide_poly;
  if (any_hidden) {
    global_hide_poly.reinitialize(total_faces);
    global_hide_poly.fill(false);
    for (const int i : objects.index_range()) {
      if (!objects[i].hide_poly.is_empty()) {
        global_hide_poly.as_mutable_span()
            .slice(face_offset[i], objects[i].faces.size())
            .copy_from(objects[i].hide_poly);
      }
    }
  }

  /* Step 6: adjacency maps over the GLOBAL arrays. `global_edges.size()` includes the bridge
   * edges, but `global_corner_edges` only ever references object edges, so
   * #bke::mesh::build_edge_to_face_map naturally leaves every bridge edge with an empty face list.
   * The returned #GroupedSpan views (over `global_faces` / the not-yet-moved offsets/indices
   * below) are discarded -- only the owning arrays are kept in the result, and the views are
   * reconstructed from them fresh at each use site (see #GlobalGeodesicTopology's doc-comment). */
  GlobalGeodesicTopology result;
  bke::mesh::build_edge_to_face_map(global_faces,
                                    global_corner_edges,
                                    global_edges.size(),
                                    result.edge_to_face_offsets,
                                    result.edge_to_face_indices);
  bke::mesh::build_vert_to_edge_map(
      global_edges.as_span(), total_verts, result.vert_to_edge_offsets, result.vert_to_edge_indices);

  result.positions = std::move(global_positions);
  result.edges = std::move(global_edges);
  result.face_offset_data = std::move(global_face_offsets);
  result.corner_verts = std::move(global_corner_verts);
  result.hide_poly = std::move(global_hide_poly);
  result.vert_offset = std::move(vert_offset);
  return result;
}

void propagate_geodesic_from_topology(const GlobalGeodesicTopology &topology,
                                      const Span<MultiVertRef> seeds,
                                      MutableSpan<Array<float>> r_vert_falloff_per_object)
{
  const int object_num = topology.vert_offset.size() - 1;
  BLI_assert(object_num == r_vert_falloff_per_object.size());

  /* Reconstruct the grouped-span / offset-indices views fresh from the owning arrays -- see
   * #GlobalGeodesicTopology's doc-comment for why these must not be cached alongside them. */
  const OffsetIndices<int> global_faces(topology.face_offset_data);
  const GroupedSpan<int> global_edge_to_face_map(OffsetIndices<int>(topology.edge_to_face_offsets),
                                                 topology.edge_to_face_indices.as_span());
  const GroupedSpan<int> global_vert_to_edge_map(OffsetIndices<int>(topology.vert_to_edge_offsets),
                                                 topology.vert_to_edge_indices.as_span());

  /* Seeds, translated into global vertex indices. */
  Set<int> global_initial_verts;
  for (const MultiVertRef &seed : seeds) {
    global_initial_verts.add(seed.vert + topology.vert_offset[seed.object_index]);
  }

  /* Propagate over the combined topology using the priority-queue (Fast Marching Method) geodesic
   * core, NOT #geodesic::distances_create. The proximity bridge is exactly the kind of long-range
   * "shortcut" edge (geometrically close, but many hops away in the concatenated topology) that
   * breaks #distances_create's round-based BFS ordering assumption: measured on a real repro (3
   * bridged objects, ~1.5M verts each), seeding from a more centrally bridged object drove ~7
   * redundant re-relaxations per vertex on average and a ~60x slowdown (155s vs 2.5s) versus
   * seeding from an end object, even though the combined graph was identical both times.
   * #distances_create_priority_queue uses the same per-triangle update formula but finalizes each
   * vertex exactly once (true increasing-distance order), which is immune to this pathology
   * regardless of where the bridge sits relative to the seed. For a single object with no bridge
   * this still produces the same distance field as the single-object path (the underlying
   * geodesic distance being approximated is unique); it is simply not guaranteed to match
   * #distances_create's exact intermediate tie-breaking, which does not matter here since this
   * function is only ever used for the multi-object path. */
  const Array<float> dists = geodesic::distances_create_priority_queue(topology.positions,
                                                                       topology.edges,
                                                                       global_faces,
                                                                       topology.corner_verts,
                                                                       global_vert_to_edge_map,
                                                                       global_edge_to_face_map,
                                                                       topology.hide_poly,
                                                                       global_initial_verts,
                                                                       FLT_MAX);

  /* Split the combined result back into one Array per object. */
  for (const int i : IndexRange(object_num)) {
    const int count = topology.vert_offset[i + 1] - topology.vert_offset[i];
    r_vert_falloff_per_object[i].reinitialize(count);
    r_vert_falloff_per_object[i].as_mutable_span().copy_from(
        dists.as_span().slice(topology.vert_offset[i], count));
  }
}

void propagate_geodesic(const Span<ObjectTopology> objects,
                        const MultiObjectBridge &bridge,
                        const Span<MultiVertRef> seeds,
                        MutableSpan<Array<float>> r_vert_falloff_per_object)
{
  BLI_assert(objects.size() == r_vert_falloff_per_object.size());
  /* One-shot / test entry point: rebuilds the concatenated topology every call. The hot
   * (per-mouse-move) multi-object path instead builds a #GlobalGeodesicTopology once and reuses
   * it across calls -- see #multi_object_graph_propagate. */
  const GlobalGeodesicTopology topology = build_global_geodesic_topology(objects, bridge);
  propagate_geodesic_from_topology(topology, seeds, r_vert_falloff_per_object);
}

}  // namespace detail

/* Builds a vertex-to-vertex adjacency #GroupedSpan for the Uniform (topology) BFS arm from the
 * mesh's edges. Reuses #bke::mesh::build_vert_to_edge_map for the vertex offsets (vertex degree
 * counted in edges) and remaps each incident edge to its "other" endpoint. `r_offsets`/`r_data`
 * back the returned #GroupedSpan and must outlive it. */
static GroupedSpan<int> build_vert_to_vert_map(const Mesh &mesh,
                                               Array<int> &r_offsets,
                                               Array<int> &r_data)
{
  const Span<int2> edges = mesh.edges();
  Array<int> edge_indices;
  const GroupedSpan<int> vert_to_edge = bke::mesh::build_vert_to_edge_map(
      edges, mesh.verts_num, r_offsets, edge_indices);
  const OffsetIndices<int> offsets(r_offsets);
  r_data.reinitialize(edge_indices.size());
  for (const int vert : offsets.index_range()) {
    const Span<int> vert_edges = vert_to_edge[vert];
    MutableSpan<int> vert_neighbors = r_data.as_mutable_span().slice(offsets[vert]);
    for (const int i : vert_edges.index_range()) {
      const int2 &edge = edges[vert_edges[i]];
      vert_neighbors[i] = (edge[0] == vert) ? edge[1] : edge[0];
    }
  }
  return {offsets, r_data};
}

/* Builds a vertex-to-vertex adjacency #GroupedSpan for the UniformDiagonals (topology diagonals)
 * BFS arm, mirroring #diagonals_falloff_create's (sculpt_expand.cc) neighbor enumeration: a
 * vertex's neighbors are every vertex sharing ANY face with it (this crosses quad diagonals),
 * found via `mesh.vert_to_face_map()` + `mesh.faces()` + `mesh.corner_verts()`. Unlike
 * #build_vert_to_vert_map this does not dedupe or exclude the vertex itself -- both are harmless
 * for the unit-weight BFS in #detail::propagate_uniform (a duplicate/self relaxation can never
 * beat an already-equal-or-shorter distance). `r_offsets`/`r_data` back the returned #GroupedSpan
 * and must outlive it. */
static GroupedSpan<int> build_face_diagonal_neighbors(const Mesh &mesh,
                                                      Array<int> &r_offsets,
                                                      Array<int> &r_data)
{
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();

  r_offsets.reinitialize(mesh.verts_num + 1);
  r_offsets[0] = 0;
  for (const int vert : IndexRange(mesh.verts_num)) {
    int neighbor_num = 0;
    for (const int face : vert_to_face_map[vert]) {
      neighbor_num += faces[face].size();
    }
    r_offsets[vert + 1] = r_offsets[vert] + neighbor_num;
  }

  const OffsetIndices<int> offsets(r_offsets);
  r_data.reinitialize(offsets.total_size());
  for (const int vert : IndexRange(mesh.verts_num)) {
    MutableSpan<int> vert_neighbors = r_data.as_mutable_span().slice(offsets[vert]);
    int i = 0;
    for (const int face : vert_to_face_map[vert]) {
      for (const int neighbor : corner_verts.slice(faces[face])) {
        vert_neighbors[i] = neighbor;
        i++;
      }
    }
  }
  return {offsets, r_data};
}

void multi_object_graph_propagate(const Depsgraph &depsgraph,
                                  const Span<Object *> objects,
                                  const Span<Array<float3>> world_positions,
                                  const Span<Array<int>> grids_canonical_maps,
                                  const Span<MultiVertRef> seeds,
                                  const MultiObjectBridge &bridge,
                                  const PropagationMode mode,
                                  std::unique_ptr<detail::GlobalGeodesicTopology> &geodesic_topology_cache,
                                  MutableSpan<Array<float>> r_vert_falloff_per_object)
{
  if (mode == PropagationMode::Geodesic) {
    /* Geodesic: `depsgraph` is not needed here -- `world_positions` already holds the evaluated
     * positions (see #world_positions_create). */
    (void)depsgraph;

    /* The concatenated topology + adjacency maps only depend on the object set / mesh topology /
     * bridge, all fixed for the whole Expand modal operation (Expand never mutates geometry).
     * Build it once into `geodesic_topology_cache` and reuse it on every later call (e.g. one per
     * mouse-move while moving the origin) instead of repeating the O(verts+edges+faces)
     * concatenation + #bke::mesh::build_edge_to_face_map / #build_vert_to_edge_map work -- see
     * `Architecture_Refactoring_Analysis.md` 5.2. */
    if (!geodesic_topology_cache) {
      /* Kept alive only for this build: each #detail::ObjectTopology's `hide_poly` is a view onto
       * the corresponding entry here (empty when the object has no `.hide_poly` attribute). Built
       * via #Vector::append (placement-construct) rather than indexed assignment, since
       * #VArraySpan only exposes a move constructor / move-assignment operator, not a
       * copy-assignment operator. Not needed once #build_global_geodesic_topology has copied
       * everything it needs into the cached, owning #GlobalGeodesicTopology. */
      Vector<VArraySpan<bool>> hide_poly_storage;
      hide_poly_storage.reserve(objects.size());
      Vector<detail::ObjectTopology> topologies;
      topologies.reserve(objects.size());
      for (const int i : objects.index_range()) {
        const Mesh &mesh = *id_cast<const Mesh *>(objects[i]->data);
        const bke::AttributeAccessor attributes = mesh.attributes();
        hide_poly_storage.append(*attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face));
        topologies.append({world_positions[i],
                           mesh.edges(),
                           mesh.faces(),
                           mesh.corner_verts(),
                           mesh.corner_edges(),
                           hide_poly_storage.last()});
      }
      geodesic_topology_cache = std::make_unique<detail::GlobalGeodesicTopology>(
          detail::build_global_geodesic_topology(topologies, bridge));
    }

    detail::propagate_geodesic_from_topology(
        *geodesic_topology_cache, seeds, r_vert_falloff_per_object);
    return;
  }

  /* Uniform / UniformDiagonals: `depsgraph` / `world_positions` are only needed by the Geodesic
   * arm (world-space distances); the topology BFS below only needs mesh connectivity. */
  (void)depsgraph;
  (void)world_positions;

  /* Build a vertex-to-vertex neighbor #GroupedSpan per object -- from mesh EDGES for Uniform, or
   * from shared-face-corners (diagonals included) for UniformDiagonals -- then defer to the same
   * #detail::propagate_uniform BFS core for both; the two modes differ only in this neighbor
   * build step. The backing offsets/data live in `all_offsets`/`all_data` for the remainder of
   * this call, since #GroupedSpan only stores a view onto them. For a Grids object the graph is
   * built over CANONICAL (duplicate-seam-merged) vertex ids instead of raw mesh vertices -- see
   * #grids_canonical_map_create. */
  Vector<Array<int>> all_offsets(objects.size());
  Vector<Array<int>> all_data(objects.size());
  Array<GroupedSpan<int>> object_neighbors(objects.size());
  for (const int i : objects.index_range()) {
    const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*objects[i]);
    if (pbvh.type() == bke::pbvh::Type::Grids) {
      object_neighbors[i] = (mode == PropagationMode::UniformDiagonals) ?
                                grids_diagonal_neighbors_create(
                                    *objects[i], grids_canonical_maps[i], all_offsets[i], all_data[i]) :
                                grids_edge_neighbors_create(
                                    *objects[i], grids_canonical_maps[i], all_offsets[i], all_data[i]);
      continue;
    }
    const Mesh &mesh = *id_cast<const Mesh *>(objects[i]->data);
    object_neighbors[i] = (mode == PropagationMode::UniformDiagonals) ?
                              build_face_diagonal_neighbors(mesh, all_offsets[i], all_data[i]) :
                              build_vert_to_vert_map(mesh, all_offsets[i], all_data[i]);
  }

  /* Translate seeds and bridge endpoints from raw to canonical indices for every Grids object
   * before they touch the canonical-indexed graph built above. Mesh objects pass through
   * unchanged (empty canonical map). */
  auto to_canonical = [&](const MultiVertRef &v) -> MultiVertRef {
    const Array<int> &map = grids_canonical_maps[v.object_index];
    if (map.is_empty()) {
      return v;
    }
    /* The canonical id space #object_neighbors was built over is a DENSE compaction (see
     * #detail::build_canonical_neighbors); recompute the same compaction here so seed/bridge
     * translation lands on the same ids the graph itself uses. Cheap relative to the graph build
     * already performed above (one more pass over this one object's raw vertex count). */
    Map<int, int> compact_id;
    for (const int raw : map.index_range()) {
      compact_id.lookup_or_add_cb(map[raw], [&]() { return int(compact_id.size()); });
    }
    return {v.object_index, compact_id.lookup(map[v.vert])};
  };

  Vector<MultiVertRef> canonical_seeds;
  for (const MultiVertRef &s : seeds) {
    canonical_seeds.append(to_canonical(s));
  }

  MultiObjectBridge canonical_bridge;
  for (const BridgeEdge &edge : bridge.edges) {
    canonical_bridge.edges.append(
        {to_canonical(edge.a), to_canonical(edge.b), edge.world_distance});
  }

  detail::propagate_uniform(
      object_neighbors, canonical_bridge, canonical_seeds, r_vert_falloff_per_object);

  /* Broadcast each canonical vertex's falloff back out to every raw index in its equivalence
   * class, so `r_vert_falloff_per_object[i]` ends up raw-index-sized like every Mesh object's,
   * matching what #ObjectState::vert_falloff's other consumers (enabled_state_to_bitmap,
   * gradient_value_get, update_mask_grids, ...) expect. */
  for (const int i : objects.index_range()) {
    const Array<int> &map = grids_canonical_maps[i];
    if (map.is_empty()) {
      continue;
    }
    Map<int, int> compact_id;
    for (const int raw : map.index_range()) {
      compact_id.lookup_or_add_cb(map[raw], [&]() { return int(compact_id.size()); });
    }
    Array<float> raw_falloff(map.size());
    for (const int raw : map.index_range()) {
      raw_falloff[raw] = r_vert_falloff_per_object[i][compact_id.lookup(map[raw])];
    }
    r_vert_falloff_per_object[i] = std::move(raw_falloff);
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::expand
