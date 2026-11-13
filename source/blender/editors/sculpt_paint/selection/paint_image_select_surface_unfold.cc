/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "paint_image_select_surface_unfold.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Adjacency
 * \{ */

namespace {

struct UnfoldEdgeRecord {
  /** The two vertex indices, smaller one in the high bits. */
  uint64_t key;
  int tri;
  int edge;
};

}  // namespace

SurfaceUnfoldAdjacency surface_unfold_adjacency_build(const Span<int3> corner_tris,
                                                      const Span<int> corner_verts)
{
  SurfaceUnfoldAdjacency result;
  result.neighbors = Array<int3>(corner_tris.size(), int3(-1));

  /* Edges are matched by their vertex pair, not by mesh edge index: a face's internal triangulation
   * diagonals have no mesh edge, but two triangles of the same face still unfold across them. */
  Vector<UnfoldEdgeRecord> records;
  records.reserve(corner_tris.size() * 3);
  for (const int64_t tri_i : corner_tris.index_range()) {
    const int3 tri = corner_tris[tri_i];
    if (tri.x >= corner_verts.size() || tri.y >= corner_verts.size() ||
        tri.z >= corner_verts.size())
    {
      continue;
    }
    for (const int i : IndexRange(3)) {
      const int va = corner_verts[tri[i]];
      const int vb = corner_verts[tri[(i + 1) % 3]];
      if (va == vb) {
        continue;
      }
      const uint64_t lo = uint64_t(std::min(va, vb));
      const uint64_t hi = uint64_t(std::max(va, vb));
      records.append({(lo << 32) | hi, int(tri_i), i});
    }
  }
  std::sort(records.begin(), records.end(), [](const UnfoldEdgeRecord &a, const UnfoldEdgeRecord &b) {
    return a.key < b.key;
  });

  for (int64_t start = 0; start < records.size();) {
    int64_t end = start + 1;
    while (end < records.size() && records[end].key == records[start].key) {
      end++;
    }
    /* Exactly two triangles share the edge: a manifold interior edge. One is a boundary, more than
     * two is non-manifold; neither is crossed. */
    if (end - start == 2 && records[start].tri != records[start + 1].tri) {
      const UnfoldEdgeRecord &a = records[start];
      const UnfoldEdgeRecord &b = records[start + 1];
      result.neighbors[a.tri][a.edge] = b.tri;
      result.neighbors[b.tri][b.edge] = a.tri;
    }
    start = end;
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Unfold
 * \{ */

bool surface_unfold_build(const Span<int3> corner_tris,
                          const Span<int> corner_verts,
                          const Span<float3> vert_positions,
                          const SurfaceUnfoldAdjacency &adjacency,
                          const int seed_tri,
                          const float3 &seed_point,
                          const float3 &tangent,
                          const float radius,
                          const int max_tris,
                          SurfaceUnfoldPatch &r_patch)
{
  r_patch = SurfaceUnfoldPatch();
  if (seed_tri < 0 || seed_tri >= corner_tris.size() || max_tris < 1 ||
      adjacency.neighbors.size() != corner_tris.size())
  {
    return false;
  }

  /* The corner positions of a triangle, false when an index is out of range (a stale span). */
  const auto tri_positions = [&](const int tri_index, float3 r_co[3]) {
    const int3 tri = corner_tris[tri_index];
    for (const int i : IndexRange(3)) {
      if (tri[i] < 0 || tri[i] >= corner_verts.size()) {
        return false;
      }
      const int vert = corner_verts[tri[i]];
      if (vert < 0 || vert >= vert_positions.size()) {
        return false;
      }
      r_co[i] = vert_positions[vert];
    }
    return true;
  };

  SurfaceUnfoldTri seed;
  seed.tri_index = seed_tri;
  if (!tri_positions(seed_tri, seed.co)) {
    return false;
  }
  const float3 seed_normal_raw = math::cross(seed.co[1] - seed.co[0], seed.co[2] - seed.co[0]);
  if (math::length_squared(seed_normal_raw) < 1e-20f) {
    return false;
  }
  const float3 normal = math::normalize(seed_normal_raw);
  float3 axis_x = tangent - normal * math::dot(tangent, normal);
  if (math::length_squared(axis_x) < 1e-12f) {
    /* The tangent is (anti)parallel to the normal: any in-plane direction will do. */
    axis_x = seed.co[1] - seed.co[0];
  }
  axis_x = math::normalize(axis_x);
  const float3 axis_y = math::cross(normal, axis_x);

  for (const int i : IndexRange(3)) {
    const float3 d = seed.co[i] - seed_point;
    seed.plane[i] = float2(math::dot(d, axis_x), math::dot(d, axis_y));
  }

  const float limit = std::max(radius, 0.0f);
  r_patch.radius = limit;
  r_patch.bounds_min = float2(std::numeric_limits<float>::max());
  r_patch.bounds_max = float2(-std::numeric_limits<float>::max());
  const auto include_bounds = [&](const SurfaceUnfoldTri &tri) {
    for (const int i : IndexRange(3)) {
      r_patch.bounds_min = math::min(r_patch.bounds_min, tri.plane[i]);
      r_patch.bounds_max = math::max(r_patch.bounds_max, tri.plane[i]);
    }
  };

  Set<int> placed;
  placed.add(seed_tri);
  r_patch.tris.append(seed);
  include_bounds(seed);

  for (int64_t head = 0; head < r_patch.tris.size(); head++) {
    /* A copy: appending below may reallocate the vector. */
    const SurfaceUnfoldTri current = r_patch.tris[head];
    const int3 current_corners = corner_tris[current.tri_index];
    for (const int k : IndexRange(3)) {
      const int neighbor = adjacency.neighbors[current.tri_index][k];
      if (neighbor < 0 || placed.contains(neighbor)) {
        continue;
      }
      const int3 neighbor_corners = corner_tris[neighbor];
      const int va = corner_verts[current_corners[k]];
      const int vb = corner_verts[current_corners[(k + 1) % 3]];
      int ja = -1;
      int jb = -1;
      for (const int j : IndexRange(3)) {
        const int v = corner_verts[neighbor_corners[j]];
        if (v == va && ja < 0) {
          ja = j;
        }
        else if (v == vb && jb < 0) {
          jb = j;
        }
      }
      if (ja < 0 || jb < 0) {
        continue;
      }
      const int jc = 3 - ja - jb;

      float3 neighbor_co[3];
      if (!tri_positions(neighbor, neighbor_co)) {
        continue;
      }
      const float2 pa = current.plane[k];
      const float2 pb = current.plane[(k + 1) % 3];
      const float2 po = current.plane[(k + 2) % 3];
      const float2 edge = pb - pa;
      const float edge_len = math::length(edge);
      if (edge_len < 1e-12f) {
        continue;
      }
      const float2 edge_dir = edge / edge_len;
      const float2 perp(-edge_dir.y, edge_dir.x);

      /* The third corner from its distances to the shared edge's ends (two circles), on the side
       * of the edge the already placed triangle's own third corner is not on. */
      const float da = math::distance(neighbor_co[jc], current.co[k]);
      const float db = math::distance(neighbor_co[jc], current.co[(k + 1) % 3]);
      const float along = (da * da - db * db + edge_len * edge_len) / (2.0f * edge_len);
      const float across = std::sqrt(std::max(da * da - along * along, 0.0f));
      const float side = math::dot(po - pa, perp) > 0.0f ? -1.0f : 1.0f;
      const float2 pc = pa + edge_dir * along + perp * (across * side);

      SurfaceUnfoldTri placed_tri;
      placed_tri.tri_index = neighbor;
      for (const int i : IndexRange(3)) {
        placed_tri.co[i] = neighbor_co[i];
      }
      placed_tri.plane[ja] = pa;
      placed_tri.plane[jb] = pb;
      placed_tri.plane[jc] = pc;

      /* A triangle wholly outside the square stays out; it is not marked placed, so another
       * neighbor may still reach it from a layout that does overlap the radius. */
      const float2 tri_min = math::min(placed_tri.plane[0],
                                       math::min(placed_tri.plane[1], placed_tri.plane[2]));
      const float2 tri_max = math::max(placed_tri.plane[0],
                                       math::max(placed_tri.plane[1], placed_tri.plane[2]));
      if (tri_max.x < -limit || tri_min.x > limit || tri_max.y < -limit || tri_min.y > limit) {
        continue;
      }
      if (r_patch.tris.size() >= max_tris) {
        r_patch.truncated = true;
        return true;
      }
      placed.add(neighbor);
      r_patch.tris.append(placed_tri);
      include_bounds(placed_tri);
    }
  }
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
