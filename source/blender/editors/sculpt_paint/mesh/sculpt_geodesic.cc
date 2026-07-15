/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <bit>
#include <cstdlib>
#include <functional>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"

#include "atomic_ops.h"

#include "sculpt_geodesic.hh"

#define SCULPT_GEODESIC_VERTEX_NONE -1

namespace blender::ed::sculpt_paint::geodesic {

/* Propagate distance from v1 and v2 to v0. */
static bool sculpt_geodesic_mesh_test_dist_add(Span<float3> vert_positions,
                                               const int v0,
                                               const int v1,
                                               const int v2,
                                               MutableSpan<float> dists,
                                               const Set<int> &initial_verts)
{
  if (initial_verts.contains(v0)) {
    return false;
  }

  BLI_assert(dists[v1] != FLT_MAX);
  if (dists[v0] <= dists[v1]) {
    return false;
  }

  float dist0;
  if (v2 != SCULPT_GEODESIC_VERTEX_NONE) {
    BLI_assert(dists[v2] != FLT_MAX);
    if (dists[v0] <= dists[v2]) {
      return false;
    }
    dist0 = geodesic_distance_propagate_across_triangle(
        vert_positions[v0], vert_positions[v1], vert_positions[v2], dists[v1], dists[v2]);
  }
  else {
    float vec[3];
    sub_v3_v3v3(vec, vert_positions[v1], vert_positions[v0]);
    dist0 = dists[v1] + len_v3(vec);
  }

  if (dist0 < dists[v0]) {
    dists[v0] = dist0;
    return true;
  }

  return false;
}

/**
 * The threads relaxing one level of the front share `dists` and `edge_tag`, but only the
 * updates have to be atomic.
 *
 * Distance value reads are plain. Distances are only ever lowered, so a stale (larger)
 * read only produces a larger candidate, which the atomic minimum discards, and the
 * thread that actually lowered the vertex re-queues the edges around it anyway.
 *
 * The updates use `atomic_ops.h` rather than #std::atomic_ref, both because libc++ only
 * gained the latter in LLVM 19 (newer than the tool-chains Blender still supports) and
 * because its operations are sequentially consistent, which this algorithm needs: a
 * thread lowers a vertex before it reads the vertices across its edges to decide which to
 * queue, and two threads lowering the two endpoints of one edge must not both read the
 * other as unreached, or neither would queue the shared edge and it would drop out of the
 * traversal for good. The full barrier of the compare-and-swap rules that out.
 *
 * Both are templated on whether the caller runs concurrently, so the single-threaded path
 * that relaxes small fronts does not pay for any of it.
 */

/** Lower `dists[vert]` to `dist`, returning true when the distance actually improved. */
template<bool UseAtomics>
static bool dist_min_update(MutableSpan<float> dists, const int vert, const float dist)
{
  if constexpr (!UseAtomics) {
    if (dist < dists[vert]) {
      dists[vert] = dist;
      return true;
    }
    return false;
  }
  else {
    /* The exchange compares bit patterns rather than floats, because two distinct bit
     * patterns can compare equal as floats and a failed exchange would then look like a
     * successful one. */
    uint32_t *bits = reinterpret_cast<uint32_t *>(&dists[vert]);
    const uint32_t dist_bits = std::bit_cast<uint32_t>(dist);
    uint32_t current_bits = std::bit_cast<uint32_t>(dists[vert]);
    while (dist < std::bit_cast<float>(current_bits)) {
      const uint32_t prev_bits = atomic_cas_uint32(bits, current_bits, dist_bits);
      if (prev_bits == current_bits) {
        return true;
      }
      /* A failed exchange hands back the value another thread stored meanwhile. */
      current_bits = prev_bits;
    }
    return false;
  }
}

/** Claim `edge` for the next level, returning true only for the caller that claimed it
 * first. */
template<bool UseAtomics>
static bool edge_tag_claim(MutableSpan<uint8_t> edge_tag, const int edge)
{
  if constexpr (!UseAtomics) {
    if (edge_tag[edge] != 0) {
      return false;
    }
    edge_tag[edge] = 1;
    return true;
  }
  else {
    return atomic_fetch_and_or_uint8(&edge_tag[edge], 1) == 0;
  }
}

/**
 * Propagate the distance from `v1`, and across the face containing `v2` when it is given,
 * to `v0`.
 */
template<bool UseAtomics>
static bool geodesic_dist_propagate(const Span<float3> vert_positions,
                                    const int v0,
                                    const int v1,
                                    const int v2,
                                    MutableSpan<float> dists,
                                    const BitVector<> &initial_vert_tags)
{
  if (initial_vert_tags[v0]) {
    return false;
  }

  /* Read the endpoints' distances once; see the note above on why plain reads are safe. */
  const float d1 = dists[v1];
  BLI_assert(d1 != FLT_MAX);
  if (dists[v0] <= d1) {
    return false;
  }

  float dist0;
  if (v2 != SCULPT_GEODESIC_VERTEX_NONE) {
    const float d2 = dists[v2];
    BLI_assert(d2 != FLT_MAX);
    if (dists[v0] <= d2) {
      return false;
    }
    dist0 = geodesic_distance_propagate_across_triangle(
        vert_positions[v0], vert_positions[v1], vert_positions[v2], d1, d2);
  }
  else {
    float vec[3];
    sub_v3_v3v3(vec, vert_positions[v1], vert_positions[v0]);
    dist0 = d1 + len_v3(vec);
  }

  return dist_min_update<UseAtomics>(dists, v0, dist0);
}

Array<float> distances_create(const Span<float3> vert_positions,
                              const Span<int2> edges,
                              const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              const GroupedSpan<int> vert_to_edge_map,
                              const GroupedSpan<int> edge_to_face_map,
                              const Span<bool> hide_poly,
                              const Set<int> &initial_verts,
                              const float limit_radius)
{
  const float limit_radius_sq = limit_radius * limit_radius;

  Array<float> dists(vert_positions.size());

  /* A branchless O(1) test in the hot loop, replacing #Set::contains. */
  BitVector<> initial_vert_tags(vert_positions.size());

  for (const int i : vert_positions.index_range()) {
    if (initial_verts.contains(i)) {
      dists[i] = 0.0f;
      initial_vert_tags[i].set();
    }
    else {
      dists[i] = FLT_MAX;
    }
  }

  /* Masks vertices further than the limit radius from an initial vertex. As there is no
   * need to define a distance to them, the algorithm can stop earlier by skipping them. */
  BitVector<> affected_vert(vert_positions.size());
  if (limit_radius == FLT_MAX) {
    /* No need to loop through all initial vertices: they are all going to be affected. */
    affected_vert.fill(true);
  }
  else {
    /* O(n^2) loop used to limit the geodesic calculation to a radius. When this is needed,
     * the tool is expected to request the distance to a low number of vertices. */
    for (const int v : initial_verts) {
      const float *v_co = vert_positions[v];
      for (const int i : vert_positions.index_range()) {
        if (len_squared_v3v3(v_co, vert_positions[i]) <= limit_radius_sq) {
          affected_vert[i].set();
        }
      }
    }
  }

  /* Deduplicates the edges queued for the next level. A full byte per edge rather than a
   * #BitVector: a single bit cannot be claimed atomically without also writing the bits
   * packed next to it. */
  Array<uint8_t> edge_tag(edges.size(), 0);

  Vector<int> queue;

  /* Add edges adjacent to an initial vertex to the queue. */
  for (const int i : edges.index_range()) {
    const int v1 = edges[i][0];
    const int v2 = edges[i][1];
    if (!affected_vert[v1] && !affected_vert[v2]) {
      continue;
    }
    if (dists[v1] != FLT_MAX || dists[v2] != FLT_MAX) {
      queue.append(i);
    }
  }

  /* Everything reachable through a vertex whose distance just improved has to be relaxed
   * again with the new value, so queue the edges around it. The edge the vertex improved
   * from is never a candidate: a vertex only improves as the opposite corner of a face of
   * that edge, so it is not one of its endpoints and the edge is not in its edge map. */
  const auto expand_vert = [&](const auto use_atomics, const int vert, Vector<int> &r_queue_next) {
    constexpr bool atomics = decltype(use_atomics)::value;

    for (const int edge : vert_to_edge_map[vert]) {
      const int vert_far = edges[edge][0] == vert ? edges[edge][1] : edges[edge][0];
      if (!edge_to_face_map[edge].is_empty() && dists[vert_far] == FLT_MAX) {
        continue;
      }
      if (!affected_vert[vert] && !affected_vert[vert_far]) {
        continue;
      }
      if (edge_tag_claim<atomics>(edge_tag, edge)) {
        r_queue_next.append(edge);
      }
    }
  };

  /* Relax one edge of the front, queueing the edges around every vertex it improves. Safe
   * to run concurrently on distinct edges of the same level. */
  const auto relax_edge = [&](const auto use_atomics, const int edge, Vector<int> &r_queue_next) {
    constexpr bool atomics = decltype(use_atomics)::value;

    int v1 = edges[edge][0];
    int v2 = edges[edge][1];

    const float d1 = dists[v1];
    const float d2 = dists[v2];

    if (d1 == FLT_MAX || d2 == FLT_MAX) {
      /* Only one endpoint reached: propagate along the edge rather than across a face. */
      if (d1 > d2) {
        std::swap(v1, v2);
      }
      geodesic_dist_propagate<atomics>(
          vert_positions, v2, v1, SCULPT_GEODESIC_VERTEX_NONE, dists, initial_vert_tags);
    }

    for (const int face : edge_to_face_map[edge]) {
      if (!hide_poly.is_empty() && hide_poly[face]) {
        continue;
      }
      for (const int v_other : corner_verts.slice(faces[face])) {
        if (ELEM(v_other, v1, v2)) {
          continue;
        }
        if (!geodesic_dist_propagate<atomics>(
                vert_positions, v_other, v1, v2, dists, initial_vert_tags))
        {
          continue;
        }
        expand_vert(use_atomics, v_other, r_queue_next);
      }
    }
  };

  Vector<int> queue_next;
  threading::EnumerableThreadSpecific<Vector<int>> queue_next_by_thread;

  /* REVIEW: reconstructed tuning constants (not in the patch). Correctness is independent
   * of them; only the crossover between serial and parallel relaxation changes. Fronts
   * below `parallel_threshold` edges are relaxed on the calling thread. */
  const int parallel_threshold = 1024;
  const int grain_size = 256;

  while (!queue.is_empty()) {
    if (queue.size() < parallel_threshold) {
      for (const int edge : queue) {
        relax_edge(std::false_type(), edge, queue_next);
      }
    }
    else {
      threading::parallel_for(queue.index_range(), grain_size, [&](const IndexRange range) {
        Vector<int> &local_queue_next = queue_next_by_thread.local();
        for (const int i : range) {
          relax_edge(std::true_type(), queue[i], local_queue_next);
        }
      });

      /* REVIEW: reconstructed merge tail (not shown by the patch). Gather every thread's
       * local next-queue into `queue_next` after the barrier, then clear the locals for
       * the next level. */
      for (Vector<int> &local : queue_next_by_thread) {
        queue_next.extend(local);
        local.clear();
      }
    }

    /* Reset the tags of the edges queued for the next level so they can be claimed again.
     * The tag only deduplicates queue insertions; it is not a visited marker. */
    for (const int edge : queue_next) {
      edge_tag[edge] = 0;
    }

    queue.clear();
    std::swap(queue, queue_next);
  }

  return dists;
}

Array<float> distances_create_priority_queue(const Span<float3> vert_positions,
                                             const Span<int2> edges,
                                             const OffsetIndices<int> faces,
                                             const Span<int> corner_verts,
                                             const GroupedSpan<int> vert_to_edge_map,
                                             const GroupedSpan<int> edge_to_face_map,
                                             const Span<bool> hide_poly,
                                             const Set<int> &initial_verts,
                                             const float limit_radius)
{
  const float limit_radius_sq = limit_radius * limit_radius;

  Array<float> dists(vert_positions.size());
  BitVector<> visited(vert_positions.size());

  for (const int i : vert_positions.index_range()) {
    dists[i] = initial_verts.contains(i) ? 0.0f : FLT_MAX;
  }

  /* Same "skip vertices further than the requested radius" optimization as #distances_create. */
  BitVector<> affected_vert(vert_positions.size());
  if (limit_radius == FLT_MAX) {
    affected_vert.fill(true);
  }
  else {
    for (const int v : initial_verts) {
      const float3 &v_co = vert_positions[v];
      for (const int i : vert_positions.index_range()) {
        if (len_squared_v3v3(v_co, vert_positions[i]) <= limit_radius_sq) {
          affected_vert[i].set();
        }
      }
    }
  }

  /* Min-heap keyed by tentative distance: unlike #distances_create's round-based BFS (which
   * processes edges in hop-order and can re-relax the same vertex many times as shorter paths
   * arrive in later rounds -- cheap on an ordinary single mesh, but pathological once long-range
   * "shortcut" edges are present, e.g. a cross-object proximity bridge), this always expands the
   * globally closest not-yet-settled vertex next. Once popped, #sculpt_geodesic_mesh_test_dist_add
   * can never find a shorter path for it later (every unsettled candidate has a tentative distance
   * >= the one just popped, and both the straight-edge and triangle-unfold updates only ever
   * produce a result >= the distance they are propagated from), so each vertex is finalized
   * exactly once -- this is the standard Fast Marching Method for triangulated surfaces (Kimmel &
   * Sethian), applied to the same per-triangle update #distances_create already uses. A vertex may
   * be pushed more than once as its tentative distance improves before settling; stale entries
   * (`dist > dists[vert]`, or already-visited) are skipped cheaply on pop rather than removed from
   * the heap, since `std::priority_queue` has no decrease-key. */
  using HeapEntry = std::pair<float, int>;
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> heap;
  for (const int v : initial_verts) {
    heap.push({0.0f, v});
  }

  while (!heap.empty()) {
    const auto [d, v0] = heap.top();
    heap.pop();
    if (visited[v0] || d > dists[v0]) {
      /* Stale entry: `v0` already settled, or a better distance was found after this entry was
       * pushed. */
      continue;
    }
    visited[v0].set();

    for (const int e : vert_to_edge_map[v0]) {
      const int v1 = (edges[e][0] == v0) ? edges[e][1] : edges[e][0];
      if (visited[v1]) {
        continue;
      }
      if (!affected_vert[v0] && !affected_vert[v1]) {
        continue;
      }

      /* Straight edge relax: the only relax path available for a face-less edge (e.g. a bridge
       * stitch), and the fallback for `v1` on its very first touch. Guarantees `dists[v1]` is
       * finite afterwards (any finite candidate beats `v1`'s initial FLT_MAX), which the triangle
       * relax below relies on. */
      if (sculpt_geodesic_mesh_test_dist_add(
              vert_positions, v1, v0, SCULPT_GEODESIC_VERTEX_NONE, dists, initial_verts))
      {
        if (affected_vert[v0] || affected_vert[v1]) {
          heap.push({dists[v1], v1});
        }
      }

      /* Triangle relax: for every face sharing this edge, try to improve the third corner using
       * `v0`/`v1` as the two known corners (mirrors #distances_create's triangle test exactly). */
      for (const int face : edge_to_face_map[e]) {
        if (!hide_poly.is_empty() && hide_poly[face]) {
          continue;
        }
        for (const int v2 : corner_verts.slice(faces[face])) {
          if (ELEM(v2, v0, v1) || visited[v2]) {
            continue;
          }
          if (sculpt_geodesic_mesh_test_dist_add(vert_positions, v2, v0, v1, dists, initial_verts))
          {
            if (affected_vert[v0] || affected_vert[v1] || affected_vert[v2]) {
              heap.push({dists[v2], v2});
            }
          }
        }
      }
    }
  }

  return dists;
}

}  // namespace blender::ed::sculpt_paint::geodesic
