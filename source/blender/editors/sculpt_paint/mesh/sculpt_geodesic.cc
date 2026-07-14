/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <atomic>
#include <cstdlib>

#include "BLI_bit_vector.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "sculpt_geodesic.hh"

#define SCULPT_GEODESIC_VERTEX_NONE -1

namespace blender::ed::sculpt_paint::geodesic {

/**
 * Distances are shared between the threads relaxing a level of the front, so every access goes
 * through #std::atomic_ref. It is preferred over `atomic_cas_float` from `atomic_ops.h` because it
 * also covers the loads, and because relaxed ordering is all this algorithm needs: the only
 * ordering it relies on is the barrier at the end of each #threading::parallel_for, which already
 * synchronizes the workers with the single-threaded merge that follows.
 *
 * #Array only aligns its buffer to `alignof(T)`, so check that this is enough for a lock-free
 * atomic reference rather than assuming it.
 */
static_assert(alignof(float) >= std::atomic_ref<float>::required_alignment);
static_assert(alignof(uint8_t) >= std::atomic_ref<uint8_t>::required_alignment);

static float dist_load(MutableSpan<float> dists, const int vert)
{
  return std::atomic_ref<float>(dists[vert]).load(std::memory_order_relaxed);
}

/** Lower `dists[vert]` to `dist`, returning true when the distance actually improved. */
static bool dist_min_update(MutableSpan<float> dists, const int vert, const float dist)
{
  std::atomic_ref<float> ref(dists[vert]);
  float current = ref.load(std::memory_order_relaxed);
  while (dist < current) {
    /* A failed exchange refreshes `current` with the value another thread stored meanwhile. */
    if (ref.compare_exchange_weak(current, dist, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

/** Claim `edge` for the next level, returning true only for the thread that claimed it first. */
static bool edge_tag_claim(MutableSpan<uint8_t> edge_tag, const int edge)
{
  return std::atomic_ref<uint8_t>(edge_tag[edge]).exchange(1, std::memory_order_relaxed) == 0;
}

/** Propagate the distance from `v1`, and across the face containing `v2` when it is given, to
 * `v0`. */
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
  const float d1 = dist_load(dists, v1);
  BLI_assert(d1 != FLT_MAX);
  if (dist_load(dists, v0) <= d1) {
    return false;
  }

  float dist0;
  if (v2 != SCULPT_GEODESIC_VERTEX_NONE) {
    const float d2 = dist_load(dists, v2);
    BLI_assert(d2 != FLT_MAX);
    if (dist_load(dists, v0) <= d2) {
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

  return dist_min_update(dists, v0, dist0);
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

  /* A full byte per edge rather than a #BitVector: a single bit cannot be claimed atomically
   * without also writing the bits packed next to it. */
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

  /* Relax every edge of the front, appending the edges that have to be revisited to
   * `r_queue_next`. Safe to run concurrently on distinct edges of the same level. */
  const auto relax_edge = [&](const int edge, Vector<int> &r_queue_next) {
    int v1 = edges[edge][0];
    int v2 = edges[edge][1];

    const float d1 = dist_load(dists, v1);
    const float d2 = dist_load(dists, v2);

    if (d1 == FLT_MAX || d2 == FLT_MAX) {
      /* Only one of the endpoints has been reached: propagate along the edge rather than across a
       * face, from the endpoint that has a distance to the one that does not. */
      if (d1 > d2) {
        std::swap(v1, v2);
      }
      geodesic_dist_propagate(
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
        if (!geodesic_dist_propagate(vert_positions, v_other, v1, v2, dists, initial_vert_tags)) {
          continue;
        }

        /* The distance of `v_other` improved, so everything that can be reached through it has to
         * be relaxed again with the new value. */
        for (const int e_other : vert_to_edge_map[v_other]) {
          if (e_other == edge) {
            continue;
          }
          const int ev_other = edges[e_other][0] == v_other ? edges[e_other][1] :
                                                              edges[e_other][0];
          if (!edge_to_face_map[e_other].is_empty() && dist_load(dists, ev_other) == FLT_MAX) {
            continue;
          }
          if (!affected_vert[v_other] && !affected_vert[ev_other]) {
            continue;
          }
          if (edge_tag_claim(edge_tag, e_other)) {
            r_queue_next.append(e_other);
          }
        }
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
        relax_edge(edge, queue_next);
      }
    }
    else {
      threading::parallel_for(queue.index_range(), grain_size, [&](const IndexRange range) {
        Vector<int> &local_queue_next = queue_next_by_thread.local();
        for (const int i : range) {
          relax_edge(queue[i], local_queue_next);
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
