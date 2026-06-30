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

/* Atomic min update: sets dists[v0] = min(dists[v0], dist0). Returns true if updated. */
static bool atomic_dist_min(MutableSpan<float> dists, const int v0, const float dist0)
{
  std::atomic_ref<float> a(dists[v0]);
  float old = a.load(std::memory_order_relaxed);
  while (dist0 < old) {
    if (a.compare_exchange_weak(old, dist0, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

/* Propagate distance from v1 and v2 to v0 — parallel-safe version. */
static bool sculpt_geodesic_mesh_test_dist_add(const Span<float3> vert_positions,
                                               const int v0,
                                               const int v1,
                                               const int v2,
                                               MutableSpan<float> dists,
                                               const Set<int> &initial_verts)
{
  if (initial_verts.contains(v0)) {
    return false;
  }

  const float d1 = std::atomic_ref<float>(dists[v1]).load(std::memory_order_relaxed);
  if (d1 == FLT_MAX) {
    return false;
  }
  if (std::atomic_ref<float>(dists[v0]).load(std::memory_order_relaxed) <= d1) {
    return false;
  }

  float dist0;
  if (v2 != SCULPT_GEODESIC_VERTEX_NONE) {
    const float d2 = std::atomic_ref<float>(dists[v2]).load(std::memory_order_relaxed);
    if (d2 == FLT_MAX) {
      return false;
    }
    if (std::atomic_ref<float>(dists[v0]).load(std::memory_order_relaxed) <= d2) {
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

  return atomic_dist_min(dists, v0, dist0);
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
  /* Byte array instead of BitVector for lock-free atomic test-and-set per entry. */
  Array<uint8_t> edge_tag(edges.size(), 0);

  Vector<int> queue;
  Vector<int> queue_next;
  queue.reserve(edges.size() / 8);
  queue_next.reserve(edges.size() / 8);

  for (const int i : vert_positions.index_range()) {
    dists[i] = initial_verts.contains(i) ? 0.0f : FLT_MAX;
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

  /* Parallel label-correcting traversal. Each level relaxes the whole current edge front in
   * parallel; distance updates use atomic min, and an edge is re-queued whenever one of its
   * endpoints improves. Because the relaxation is monotonic (a smaller neighbor distance can only
   * produce a smaller propagated distance) the loop converges to the same unique fixpoint as the
   * serial version, independent of the order in which threads process edges. Per-thread output
   * lists are merged once per level to avoid contention on the shared next queue. */
  threading::EnumerableThreadSpecific<Vector<int>> tls_queue_next;

  while (!queue.is_empty()) {
    threading::parallel_for(queue.index_range(), 256, [&](const IndexRange range) {
      Vector<int> &local_next = tls_queue_next.local();
      for (const int qi : range) {
        const int e = queue[qi];
        int v1 = edges[e][0];
        int v2 = edges[e][1];

        const float d1 = std::atomic_ref<float>(dists[v1]).load(std::memory_order_relaxed);
        const float d2 = std::atomic_ref<float>(dists[v2]).load(std::memory_order_relaxed);

        if (d1 == FLT_MAX || d2 == FLT_MAX) {
          /* One endpoint unreached: propagate edge-only distance from known to unknown. */
          if (d1 > d2) {
            std::swap(v1, v2);
          }
          sculpt_geodesic_mesh_test_dist_add(
              vert_positions, v2, v1, SCULPT_GEODESIC_VERTEX_NONE, dists, initial_verts);
        }

        for (const int face : edge_to_face_map[e]) {
          if (!hide_poly.is_empty() && hide_poly[face]) {
            continue;
          }
          for (const int v_other : corner_verts.slice(faces[face])) {
            if (ELEM(v_other, v1, v2)) {
              continue;
            }
            if (sculpt_geodesic_mesh_test_dist_add(
                    vert_positions, v_other, v1, v2, dists, initial_verts))
            {
              for (const int e_other : vert_to_edge_map[v_other]) {
                const int ev_other = edges[e_other][0] == v_other ? edges[e_other][1] :
                                                                    edges[e_other][0];
                if (e_other == e) {
                  continue;
                }
                if (!edge_to_face_map[e_other].is_empty() &&
                    std::atomic_ref<float>(dists[ev_other]).load(std::memory_order_relaxed) ==
                        FLT_MAX)
                {
                  continue;
                }
                if (!affected_vert[v_other] && !affected_vert[ev_other]) {
                  continue;
                }
                /* Atomic test-and-set: only one thread enqueues each edge. */
                if (std::atomic_ref<uint8_t>(edge_tag[e_other])
                        .exchange(1, std::memory_order_relaxed) == 0)
                {
                  local_next.append(e_other);
                }
              }
            }
          }
        }
      }
    });

    /* Merge thread-local next queues. */
    for (Vector<int> &local : tls_queue_next) {
      queue_next.extend(local);
      local.clear();
    }

    /* Reset edge tags for next level. */
    for (const int e : queue_next) {
      edge_tag[e] = 0;
    }

    queue.clear();
    std::swap(queue, queue_next);
  }

  return dists;
}

}  // namespace blender::ed::sculpt_paint::geodesic
