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

#include "DEG_depsgraph_query.hh"

#include "sculpt_geodesic.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::expand {

Array<Array<float3>> world_positions_create(const Depsgraph &depsgraph, Span<Object *> objects)
{
  Array<Array<float3>> result(objects.size());
  for (const int i : objects.index_range()) {
    Object &object = *objects[i];
    const Span<float3> local = bke::pbvh::vert_positions_eval(depsgraph, object);
    const float4x4 to_world = object.object_to_world();
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

void propagate_geodesic(const Span<ObjectTopology> objects,
                        const MultiObjectBridge &bridge,
                        const Span<MultiVertRef> seeds,
                        MutableSpan<Array<float>> r_vert_falloff_per_object)
{
  BLI_assert(objects.size() == r_vert_falloff_per_object.size());

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
  Vector<int2> global_edges;
  global_edges.reserve(total_object_edges + bridge.edges.size());
  for (const int i : objects.index_range()) {
    for (const int2 &edge : objects[i].edges) {
      global_edges.append(int2(edge[0] + vert_offset[i], edge[1] + vert_offset[i]));
    }
  }
  for (const BridgeEdge &edge : bridge.edges) {
    global_edges.append(int2(edge.a.vert + vert_offset[edge.a.object_index],
                             edge.b.vert + vert_offset[edge.b.object_index]));
  }

  /* Step 4: global faces + corner_verts + corner_edges. Face corner-starts are the per-object
   * corner-start shifted by that object's `corner_offset`; the OffsetIndices contract requires the
   * final entry to equal the total corner count. */
  Array<int> global_face_offsets(total_faces + 1);
  Vector<int> global_corner_verts;
  Vector<int> global_corner_edges;
  global_corner_verts.reserve(total_corners);
  global_corner_edges.reserve(total_corners);
  for (const int i : objects.index_range()) {
    const OffsetIndices<int> &faces = objects[i].faces;
    for (const int f : faces.index_range()) {
      global_face_offsets[face_offset[i] + f] = faces[f].start() + corner_offset[i];
    }
    for (const int c : objects[i].corner_verts.index_range()) {
      global_corner_verts.append(objects[i].corner_verts[c] + vert_offset[i]);
      global_corner_edges.append(objects[i].corner_edges[c] + edge_offset[i]);
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
   * #bke::mesh::build_edge_to_face_map naturally leaves every bridge edge with an empty face list. */
  Array<int> edge_to_face_offsets;
  Array<int> edge_to_face_indices;
  const GroupedSpan<int> global_edge_to_face_map = bke::mesh::build_edge_to_face_map(
      global_faces,
      global_corner_edges,
      global_edges.size(),
      edge_to_face_offsets,
      edge_to_face_indices);
  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  const GroupedSpan<int> global_vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      global_edges.as_span(), total_verts, vert_to_edge_offsets, vert_to_edge_indices);

  /* Step 7: seeds, translated into global vertex indices. */
  Set<int> global_initial_verts;
  for (const MultiVertRef &seed : seeds) {
    global_initial_verts.add(seed.vert + vert_offset[seed.object_index]);
  }

  /* Step 8: propagate over the combined topology using the priority-queue (Fast Marching Method)
   * geodesic core, NOT #geodesic::distances_create. The proximity bridge added above is exactly
   * the kind of long-range "shortcut" edge (geometrically close, but many hops away in the
   * concatenated topology) that breaks #distances_create's round-based BFS ordering assumption:
   * measured on a real repro (3 bridged objects, ~1.5M verts each), seeding from a more centrally
   * bridged object drove ~7 redundant re-relaxations per vertex on average and a ~60x slowdown
   * (155s vs 2.5s) versus seeding from an end object, even though the combined graph was
   * identical both times. #distances_create_priority_queue uses the same per-triangle update
   * formula but finalizes each vertex exactly once (true increasing-distance order), which is
   * immune to this pathology regardless of where the bridge sits relative to the seed. For a
   * single object with no bridge this still produces the same distance field as the single-object
   * path (the underlying geodesic distance being approximated is unique); it is simply not
   * guaranteed to match #distances_create's exact intermediate tie-breaking, which does not
   * matter here since this function is only ever used for the multi-object path. */
  const Array<float> dists = geodesic::distances_create_priority_queue(global_positions,
                                                                       global_edges,
                                                                       global_faces,
                                                                       global_corner_verts,
                                                                       global_vert_to_edge_map,
                                                                       global_edge_to_face_map,
                                                                       global_hide_poly,
                                                                       global_initial_verts,
                                                                       FLT_MAX);

  /* Step 9: split the combined result back into one Array per object. */
  for (const int i : objects.index_range()) {
    const int count = objects[i].positions.size();
    r_vert_falloff_per_object[i].reinitialize(count);
    r_vert_falloff_per_object[i].as_mutable_span().copy_from(
        dists.as_span().slice(vert_offset[i], count));
  }
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
                                  const Span<MultiVertRef> seeds,
                                  const MultiObjectBridge &bridge,
                                  const PropagationMode mode,
                                  MutableSpan<Array<float>> r_vert_falloff_per_object)
{
  if (mode == PropagationMode::Geodesic) {
    /* Geodesic: build one #detail::ObjectTopology per object (mesh topology + the caller's
     * WORLD-space positions) and defer to #detail::propagate_geodesic, which concatenates every
     * object into a single combined topology and calls the untouched #geodesic::distances_create
     * core. `depsgraph` is not needed here: `world_positions` already holds the evaluated
     * positions (see #world_positions_create). */
    (void)depsgraph;

    /* Kept alive for the whole call: each #detail::ObjectTopology's `hide_poly` is a view onto the
     * corresponding entry here (empty when the object has no `.hide_poly` attribute). Built via
     * #Vector::append (placement-construct) rather than indexed assignment, since #VArraySpan only
     * exposes a move constructor / move-assignment operator, not a copy-assignment operator. */
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

    detail::propagate_geodesic(topologies, bridge, seeds, r_vert_falloff_per_object);
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
   * this call, since #GroupedSpan only stores a view onto them. */
  Vector<Array<int>> all_offsets(objects.size());
  Vector<Array<int>> all_data(objects.size());
  Array<GroupedSpan<int>> object_neighbors(objects.size());
  for (const int i : objects.index_range()) {
    const Mesh &mesh = *id_cast<const Mesh *>(objects[i]->data);
    object_neighbors[i] = (mode == PropagationMode::UniformDiagonals) ?
                              build_face_diagonal_neighbors(mesh, all_offsets[i], all_data[i]) :
                              build_vert_to_vert_map(mesh, all_offsets[i], all_data[i]);
  }

  detail::propagate_uniform(object_neighbors, bridge, seeds, r_vert_falloff_per_object);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::expand
