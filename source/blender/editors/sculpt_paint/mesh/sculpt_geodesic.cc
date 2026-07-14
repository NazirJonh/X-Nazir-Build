/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <bit>
#include <cstdlib>
#include <type_traits>

#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "atomic_ops.h"

#include "sculpt_geodesic.hh"

#define SCULPT_GEODESIC_VERTEX_NONE -1

namespace blender::ed::sculpt_paint::geodesic {

/**
 * The threads relaxing one level of the front share `dists` and `edge_tag`, but only the updates
 * have to be atomic. Distances are never raised, and a naturally aligned float is read in a single
 * instruction on every platform Blender targets, so a plain concurrent read observes the value from
 * either before or after another thread's update. Both are usable: a stale, larger distance can
 * only produce a larger candidate, which the minimum discards, and the thread that lowered it
 * queues the edges around it anyway. `atomic_add_and_fetch_fl` in `atomic_ops_ext.h` reads the same
 * way. Reading through `atomic_load_uint32` instead would be a severe pessimization: on MSVC it
 * expands to a full `MemoryBarrier()` in front of every load, and #geodesic_dist_propagate loads
 * distances several times per face corner.
 *
 * The updates use `atomic_ops.h` rather than #std::atomic_ref, both because libc++ only gained the
 * latter in LLVM 19 (newer than the tool-chains Blender still supports) and because its operations
 * are sequentially consistent, which this algorithm needs. A thread lowers the distance of a vertex
 * before it reads the distance of the vertices across its edges, to decide which of them to queue.
 * Two threads lowering the two endpoints of one edge must not both read the other endpoint as
 * unreached, or neither would queue the edge and it would drop out of the traversal for good. The
 * full barrier of the compare-and-swap rules that out: it makes each store globally visible before
 * the same thread's following loads, so the interleaving that loses the edge would require a cycle
 * in the total order. A relaxed compare-and-swap would not, which is only safe by accident on x86.
 *
 * Both are templated on whether the caller runs concurrently, so the single-threaded path that
 * relaxes small fronts does not pay for any of it.
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
    /* The exchange compares bit patterns rather than floats, because two distinct bit patterns can
     * compare equal as floats and a failed exchange would then look like a successful one. */
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

/** Claim `edge` for the next level, returning true only for the caller that claimed it first. */
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
 * Propagate the distance from `v1`, and across the face containing `v2` when it is given, to `v0`.
 */
template<bool UseAtomics>
static bool geodesic_dist_propagate(const Span<float3> vert_positions,
                                    const int v0,
                                    const int v1,
                                    const int v2,
                                    MutableSpan<float> dists,
                                    const BitSpan initial_verts)
{
  if (initial_verts[v0]) {
    return false;
  }

  /* The vertices propagated from have been reached already, otherwise the edge would not be in the
   * queue. Another thread may lower them while this runs, but a stale (larger) distance can only
   * produce a larger candidate, which the atomic minimum discards; whichever thread lowers them
   * re-queues this edge, so the final relaxation always sees the converged values. */
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

  Array<float> dists(vert_positions.size(), FLT_MAX);

  /* Every relaxation checks whether the target is an initial vertex, so use a tag per vertex
   * instead of hashing into the set in the hot loop. */
  BitVector<> initial_vert_tags(vert_positions.size());
  for (const int vert : initial_verts) {
    dists[vert] = 0.0f;
    initial_vert_tags[vert].set();
  }

  /* Masks vertices that are further than limit radius from an initial vertex. As there is no need
   * to define a distance to them the algorithm can stop earlier by skipping them. */
  BitVector<> affected_vert(vert_positions.size());

  if (limit_radius == FLT_MAX) {
    /* In this case, no need to loop through all initial vertices to check distances as they are
     * all going to be affected. */
    affected_vert.fill(true);
  }
  else {
    /* This is an O(n^2) loop used to limit the geodesic distance calculation to a radius. When
     * this optimization is needed, it is expected for the tool to request the distance to a low
     * number of vertices (usually just 1 or 2). */
    for (const int v : initial_verts) {
      const float *v_co = vert_positions[v];
      for (const int i : vert_positions.index_range()) {
        if (len_squared_v3v3(v_co, vert_positions[i]) <= limit_radius_sq) {
          affected_vert[i].set();
        }
      }
    }
  }

  /* Deduplicates the edges queued for the next level. A full byte per edge rather than a #BitVector:
   * a single bit cannot be claimed atomically without also writing the bits packed next to it. */
  Array<uint8_t> edge_tag(edges.size(), 0);

  Vector<int> queue;
  Vector<int> queue_next;

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

  /* Everything that can be reached through a vertex whose distance just improved has to be relaxed
   * again with the new value, so queue the edges around it.
   *
   * The edge the vertex improved from is never a candidate here: a vertex only improves as the
   * opposite corner of a face of that edge, so it is not one of its endpoints and the edge is not
   * in its edge map. */
  const auto expand_vert =
      [&](const auto use_atomics, const int vert, Vector<int> &r_queue_next) {
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

  /* Relax one edge of the front, queueing the edges around every vertex it improves. Safe to run
   * concurrently on distinct edges of the same level. */
  const auto relax_edge = [&](const auto use_atomics, const int edge, Vector<int> &r_queue_next) {
    constexpr bool atomics = decltype(use_atomics)::value;

    int v1 = edges[edge][0];
    int v2 = edges[edge][1];

    const float d1 = dists[v1];
    const float d2 = dists[v2];

    if (d1 == FLT_MAX || d2 == FLT_MAX) {
      /* Only one of the endpoints has been reached: propagate along the edge rather than across a
       * face, from the endpoint that has a distance to the one that does not. */
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

  /* Label-correcting traversal, one level of the edge front at a time. Relaxation is monotonic (a
   * smaller distance at a neighbor can only produce a smaller propagated distance) and an edge is
   * re-queued whenever one of its endpoints improves, so the traversal converges to the same
   * fixpoint no matter in which order the threads relax the level. */

  /* Sculpt tools request geodesic distances around a brush-sized region, so the first levels hold
   * no more than the handful of edges touching the initial vertices. Threading a front that small
   * costs more than relaxing it directly. */
  constexpr int64_t parallel_threshold = 1024;
  constexpr int64_t grain_size = 256;

  threading::EnumerableThreadSpecific<Vector<int>> queue_next_by_thread;

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

      /* The parallel loop above joins its workers, so the per-thread queues are complete here. */
      for (Vector<int> &local_queue_next : queue_next_by_thread) {
        queue_next.extend(local_queue_next);
        local_queue_next.clear();
      }
    }

    /* Tags only deduplicate the appends within one level, so they are cleared before the level
     * they guarded becomes the front. */
    for (const int edge : queue_next) {
      edge_tag[edge] = 0;
    }

    queue.clear();
    std::swap(queue, queue_next);
  }

  return dists;
}

}  // namespace blender::ed::sculpt_paint::geodesic
