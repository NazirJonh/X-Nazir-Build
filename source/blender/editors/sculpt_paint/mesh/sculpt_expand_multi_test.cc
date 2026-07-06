/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include <cfloat>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"

#include "BKE_mesh_mapping.hh"

#include "sculpt_expand_multi.hh"
#include "sculpt_geodesic.hh"

namespace blender::ed::sculpt_paint::expand::tests {

TEST(sculpt_expand_multi, world_transform_identity)
{
  const float4x4 identity = float4x4::identity();
  const float3 p(1.0f, 2.0f, 3.0f);
  EXPECT_EQ(math::transform_point(identity, p), p);
}

/* Builds the 4-neighborhood (no diagonals) adjacency of a 3x3 row-major vertex grid:
 *   0 1 2
 *   3 4 5
 *   6 7 8
 * `r_offsets`/`r_data` back the returned #GroupedSpan and must outlive it. */
static GroupedSpan<int> build_3x3_grid_neighbors(Vector<int> &r_offsets, Vector<int> &r_data)
{
  r_offsets = {0, 2, 5, 7, 10, 14, 17, 19, 22, 24};
  r_data = {
      1, 3,       /* vert 0 */
      0, 2, 4,    /* vert 1 */
      1, 5,       /* vert 2 */
      0, 4, 6,    /* vert 3 */
      1, 3, 5, 7, /* vert 4 */
      2, 4, 8,    /* vert 5 */
      3, 7,       /* vert 6 */
      4, 6, 8,    /* vert 7 */
      5, 7,       /* vert 8 */
  };
  return {OffsetIndices<int>(r_offsets.as_span()), r_data.as_span()};
}

TEST(sculpt_expand_multi, uniform_single_grid_matches_bfs)
{
  Vector<int> offsets;
  Vector<int> data;
  const GroupedSpan<int> grid = build_3x3_grid_neighbors(offsets, data);

  const MultiObjectBridge bridge;
  const MultiVertRef seed{0, 4};
  Array<Array<float>> falloff(1);
  detail::propagate_uniform({grid}, bridge, {seed}, falloff);

  /* Hand-traced BFS from the center vertex 4: corners are 2 hops away (e.g. 4->1->0), edge
   * mid-points are 1 hop away, the center itself is 0. */
  const float expected[9] = {2, 1, 2, 1, 0, 1, 2, 1, 2};
  for (const int i : IndexRange(9)) {
    EXPECT_FLOAT_EQ(falloff[0][i], expected[i]);
  }
}

TEST(sculpt_expand_multi, uniform_bridge_continuity)
{
  Vector<int> offsets_a;
  Vector<int> data_a;
  const GroupedSpan<int> grid_a = build_3x3_grid_neighbors(offsets_a, data_a);
  Vector<int> offsets_b;
  Vector<int> data_b;
  const GroupedSpan<int> grid_b = build_3x3_grid_neighbors(offsets_b, data_b);

  /* Bridge corner 8 of object 0 to corner 0 of object 1 (undirected, unit weight). */
  MultiObjectBridge bridge;
  bridge.edges.append({{0, 8}, {1, 0}, 0.0f});

  const MultiVertRef seed{0, 4};
  Array<Array<float>> falloff(2);
  detail::propagate_uniform({grid_a, grid_b}, bridge, {seed}, falloff);

  /* Object 0 is a plain single-source BFS from its own center, unaffected by the bridge (its own
   * internal distances to vertex 8 are already shorter than any path that would loop through
   * object 1 and back). */
  const float expected_a[9] = {2, 1, 2, 1, 0, 1, 2, 1, 2};
  for (const int i : IndexRange(9)) {
    EXPECT_FLOAT_EQ(falloff[0][i], expected_a[i]);
  }

  /* Object 1: obj0 vert 4 -> vert 8 is 2 hops; +1 for the bridge; so obj1 vert 0 starts at
   * distance 3. From there it is a BFS over the same grid, i.e. `3 + manhattan_distance(0, v)`
   * where the grid's row/col are `v / 3` and `v % 3` (corner-seeded 4-neighborhood BFS distance
   * on a grid equals Manhattan distance). */
  const float expected_b[9] = {3, 4, 5, 4, 5, 6, 5, 6, 7};
  for (const int i : IndexRange(9)) {
    EXPECT_FLOAT_EQ(falloff[1][i], expected_b[i]);
  }
}

/* Exercises the same #detail::propagate_uniform core used by both the Uniform (edge) and
 * UniformDiagonals (shared-face-corner) arms of #multi_object_graph_propagate, with two hand-built
 * adjacency graphs for a single quad face (verts 0,1,2,3 in face-corner order): a plain 4-cycle
 * (edge adjacency, as #build_vert_to_vert_map would produce) vs. "every vert is every other vert's
 * neighbor" (diagonal/shared-face adjacency, as #build_face_diagonal_neighbors -- mirroring
 * #diagonals_falloff_create's enumeration -- would produce for one quad). This proves the key
 * property that motivates TopologyNormals ("Topology Diagonals"): the two OPPOSITE corners of a
 * quad are 2 hops apart under edge adjacency, but only 1 hop apart once face-diagonal adjacency is
 * used, even though both runs go through the identical BFS core. */
TEST(sculpt_expand_multi, diagonal_quad_reaches_opposite_corner_in_one_hop)
{
  const MultiObjectBridge bridge;
  const MultiVertRef seed{0, 0};

  /* Edge adjacency: 0-1-2-3-0 cycle, so vert 2 (opposite vert 0) is 2 hops away. */
  Vector<int> edge_offsets = {0, 2, 4, 6, 8};
  Vector<int> edge_data = {1, 3, 0, 2, 1, 3, 0, 2};
  const GroupedSpan<int> edge_adjacency(OffsetIndices<int>(edge_offsets.as_span()), edge_data);
  Array<Array<float>> edge_falloff(1);
  detail::propagate_uniform({edge_adjacency}, bridge, {seed}, edge_falloff);
  EXPECT_FLOAT_EQ(edge_falloff[0][2], 2.0f);

  /* Diagonal (shared-face) adjacency: every vert of the quad neighbors every other vert (including
   * itself, harmlessly), so vert 2 is only 1 hop away. */
  Vector<int> diag_offsets = {0, 4, 8, 12, 16};
  Vector<int> diag_data = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
  const GroupedSpan<int> diag_adjacency(OffsetIndices<int>(diag_offsets.as_span()), diag_data);
  Array<Array<float>> diag_falloff(1);
  detail::propagate_uniform({diag_adjacency}, bridge, {seed}, diag_falloff);
  EXPECT_FLOAT_EQ(diag_falloff[0][2], 1.0f);
}

/* Owned backing storage for a small hand-built triangulated mesh, used by the Geodesic-arm tests
 * below. Must outlive any #detail::ObjectTopology built from it (which only stores views). */
struct GridMeshData {
  Array<float3> positions;
  Vector<int2> edges;
  Array<int> face_offsets;
  Vector<int> corner_verts;
  Vector<int> corner_edges;
};

/* Builds a flat (constant Z), triangulated 3x3-vertex / 2x2-quad grid: vertex `r*3+c` sits at
 * world position `origin + (c, r, 0)`, i.e. the same row-major layout as #build_3x3_grid_neighbors
 * above but as an actual (planar) triangle mesh instead of a plain adjacency graph:
 *   0 1 2
 *   3 4 5
 *   6 7 8
 * Each of the 4 unit quads is split along its top-left -> bottom-right diagonal into 2 triangles
 * (8 triangles total), giving 16 edges: the 12 grid edges (6 horizontal + 6 vertical) plus the 4
 * diagonals. Being perfectly flat, every pair of triangles sharing an edge is already "unfolded"
 * (coplanar), so the mesh's surface geodesic distance between any two vertices in the same
 * quad-connected region equals plain Euclidean distance -- this keeps the hand-derived expected
 * values in the tests below tractable. */
static GridMeshData build_flat_grid_mesh(const float3 &origin)
{
  GridMeshData mesh;

  mesh.positions.reinitialize(9);
  for (const int r : IndexRange(3)) {
    for (const int c : IndexRange(3)) {
      mesh.positions[r * 3 + c] = origin + float3(float(c), float(r), 0.0f);
    }
  }

  /* Edge list: 6 horizontal, 6 vertical, then 4 diagonals (one per quad, top-left -> bottom-right).
   * Indices below (`e01` etc.) mirror this array's order 1:1. */
  mesh.edges = {
      {0, 1}, {1, 4}, {0, 4}, {3, 4}, {0, 3}, {1, 2},
      {2, 5}, {1, 5}, {4, 5}, {4, 7}, {3, 7}, {6, 7},
      {3, 6}, {5, 8}, {4, 8}, {7, 8},
  };
  constexpr int e01 = 0, e14 = 1, e04 = 2, e34 = 3, e03 = 4, e12 = 5, e25 = 6, e15 = 7, e45 = 8,
               e47 = 9, e37 = 10, e67 = 11, e36 = 12, e58 = 13, e48 = 14, e78 = 15;

  /* 8 triangles, each quad split as {top-left, top-right, bottom-right} + {top-left, bottom-right,
   * bottom-left}. */
  mesh.face_offsets = {0, 3, 6, 9, 12, 15, 18, 21, 24};
  mesh.corner_verts = {
      0, 1, 4, /* quad(0,0) upper: (0,1,4) */
      0, 4, 3, /* quad(0,0) lower: (0,4,3) */
      1, 2, 5, /* quad(0,1) upper: (1,2,5) */
      1, 5, 4, /* quad(0,1) lower: (1,5,4) */
      3, 4, 7, /* quad(1,0) upper: (3,4,7) */
      3, 7, 6, /* quad(1,0) lower: (3,7,6) */
      4, 5, 8, /* quad(1,1) upper: (4,5,8) */
      4, 8, 7, /* quad(1,1) lower: (4,8,7) */
  };
  /* `corner_edges[c]` is the edge from `corner_verts[c]` to the next corner in the same face. */
  mesh.corner_edges = {
      e01, e14, e04, /* (0,1,4) */
      e04, e34, e03, /* (0,4,3) */
      e12, e25, e15, /* (1,2,5) */
      e15, e45, e14, /* (1,5,4) */
      e34, e47, e37, /* (3,4,7) */
      e37, e67, e36, /* (3,7,6) */
      e45, e58, e48, /* (4,5,8) */
      e48, e78, e47, /* (4,8,7) */
  };
  return mesh;
}

static detail::ObjectTopology topology_of(const GridMeshData &mesh)
{
  return {mesh.positions,
         mesh.edges,
         OffsetIndices<int>(mesh.face_offsets.as_span()),
         mesh.corner_verts,
         mesh.corner_edges,
         {}};
}

TEST(sculpt_expand_multi, geodesic_single_mesh_matches_distances_create)
{
  const GridMeshData mesh = build_flat_grid_mesh(float3(0.0f));
  const OffsetIndices<int> faces(mesh.face_offsets.as_span());

  Array<int> edge_to_face_offsets;
  Array<int> edge_to_face_indices;
  const GroupedSpan<int> edge_to_face_map = bke::mesh::build_edge_to_face_map(
      faces, mesh.corner_edges, mesh.edges.size(), edge_to_face_offsets, edge_to_face_indices);
  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  const GroupedSpan<int> vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      mesh.edges, mesh.positions.size(), vert_to_edge_offsets, vert_to_edge_indices);

  const Array<float> reference = geodesic::distances_create(mesh.positions,
                                                            mesh.edges,
                                                            faces,
                                                            mesh.corner_verts,
                                                            vert_to_edge_map,
                                                            edge_to_face_map,
                                                            {},
                                                            Set<int>{4},
                                                            FLT_MAX);

  /* NOTE on scope: this parity holds on identity/raw topology only. With ONE object and NO
   * bridge, every global array #detail::propagate_geodesic builds is identical to the arrays
   * above (at offset 0), so it necessarily calls #geodesic::distances_create with the same
   * inputs and must return an elementwise-identical result -- this is what is asserted below. It
   * is NOT a claim that the world-space multi-object core matches the LOCAL-space single-object
   * *runtime* path under an arbitrary object transform: that runtime path
   * (`geodesic_falloff_create` in sculpt_expand.cc) is untouched old code and out of scope here. */
  const MultiObjectBridge bridge;
  const MultiVertRef seed{0, 4};
  Array<Array<float>> result(1);
  detail::propagate_geodesic({topology_of(mesh)}, bridge, {seed}, result);

  ASSERT_EQ(result[0].size(), reference.size());
  for (const int i : reference.index_range()) {
    EXPECT_FLOAT_EQ(result[0][i], reference[i]);
  }
}

TEST(sculpt_expand_multi, geodesic_bridge_adds_length)
{
  constexpr float bridge_length = 5.0f;
  /* Object 0's rightmost top vertex (2) sits at local (2, 0, 0). Object 1 is the same grid shape,
   * translated so its vertex 0 sits exactly `bridge_length` further along X -- i.e. the bridge's
   * two endpoints (object 0 vertex 2, object 1 vertex 0) are `bridge_length` apart in world
   * space, with no other connection between the two objects. */
  const GridMeshData mesh_a = build_flat_grid_mesh(float3(0.0f));
  const GridMeshData mesh_b = build_flat_grid_mesh(float3(2.0f + bridge_length, 0.0f, 0.0f));

  MultiObjectBridge bridge;
  bridge.edges.append({{0, 2}, {1, 0}, bridge_length});

  const MultiVertRef seed{0, 4};
  Array<Array<float>> result(2);
  detail::propagate_geodesic({topology_of(mesh_a), topology_of(mesh_b)}, bridge, {seed}, result);

  /* Ground truth #1: object 0's OWN geodesic distance from the seed to the bridge endpoint
   * (vertex 2), via a direct call to the untouched single-mesh core -- since the bridge is the
   * ONLY connection between the two objects, no path from the seed to object 0 vertex 2 can
   * benefit from leaving and re-entering object 0, so this must equal the corresponding distance
   * inside the combined (multi-object) result too. */
  const OffsetIndices<int> faces_a(mesh_a.face_offsets.as_span());
  Array<int> a_edge_to_face_offsets;
  Array<int> a_edge_to_face_indices;
  const GroupedSpan<int> a_edge_to_face_map = bke::mesh::build_edge_to_face_map(
      faces_a,
      mesh_a.corner_edges,
      mesh_a.edges.size(),
      a_edge_to_face_offsets,
      a_edge_to_face_indices);
  Array<int> a_vert_to_edge_offsets;
  Array<int> a_vert_to_edge_indices;
  const GroupedSpan<int> a_vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      mesh_a.edges, mesh_a.positions.size(), a_vert_to_edge_offsets, a_vert_to_edge_indices);
  const Array<float> dist_from_seed_a = geodesic::distances_create(mesh_a.positions,
                                                                   mesh_a.edges,
                                                                   faces_a,
                                                                   mesh_a.corner_verts,
                                                                   a_vert_to_edge_map,
                                                                   a_edge_to_face_map,
                                                                   {},
                                                                   Set<int>{4},
                                                                   FLT_MAX);
  const float seed_to_bridge_endpoint = dist_from_seed_a[2];

  /* Ground truth #2: object 1's OWN local geodesic distances, seeded at its bridge vertex (0). By
   * the same single-connection argument, the combined result's object-1 distances must equal
   * this local distance offset by whatever it costs to reach the bridge vertex from the seed. */
  const OffsetIndices<int> faces_b(mesh_b.face_offsets.as_span());
  Array<int> b_edge_to_face_offsets;
  Array<int> b_edge_to_face_indices;
  const GroupedSpan<int> b_edge_to_face_map = bke::mesh::build_edge_to_face_map(
      faces_b,
      mesh_b.corner_edges,
      mesh_b.edges.size(),
      b_edge_to_face_offsets,
      b_edge_to_face_indices);
  Array<int> b_vert_to_edge_offsets;
  Array<int> b_vert_to_edge_indices;
  const GroupedSpan<int> b_vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      mesh_b.edges, mesh_b.positions.size(), b_vert_to_edge_offsets, b_vert_to_edge_indices);
  const Array<float> local_dist_from_bridge_vert_b = geodesic::distances_create(
      mesh_b.positions,
      mesh_b.edges,
      faces_b,
      mesh_b.corner_verts,
      b_vert_to_edge_map,
      b_edge_to_face_map,
      {},
      Set<int>{0},
      FLT_MAX);

  /* The bridge endpoint is a single hop past object 0's exact distance, so this should be
   * near-exact (only float accumulation noise, no unfolding ambiguity). */
  EXPECT_NEAR(result[1][0], seed_to_bridge_endpoint + bridge_length, 1e-4f);

  /* Every other object-1 vertex's distance through the bridge equals its own local geodesic
   * distance from the bridge vertex, offset by the same amount. Triangle-unfolding across two
   * independently-computed meshes makes exact equality fragile, hence the looser epsilon. */
  for (const int v : IndexRange(9)) {
    EXPECT_NEAR(result[1][v],
               seed_to_bridge_endpoint + bridge_length + local_dist_from_bridge_vert_b[v],
               1e-3f);
  }
}

/* Brute-force nearest-vertex functor for #detail::build_bridge_impl -- the GTest equivalent of
 * production's pbvh nearest-vert query, but a plain O(n) scan since these tests use only a
 * handful of vertices per object. Returns -1 if no vertex of `positions[obj]` is within
 * `max_dist`. */
static int brute_force_nearest(const Span<Span<float3>> positions,
                               const int obj,
                               const float3 &p,
                               const float max_dist)
{
  int best = -1;
  float best_dist_sq = max_dist * max_dist;
  for (const int v : positions[obj].index_range()) {
    const float dist_sq = math::distance_squared(positions[obj][v], p);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = v;
    }
  }
  return best;
}

TEST(sculpt_expand_multi, bridge_two_grids_near)
{
  /* Object 0: 3x3 grid at z=0, vertex (row r, col c) = r*3+c at world (c, r, 0). Rightmost column
   * (c=2: verts 2, 5, 8) faces object 1's leftmost column across a small gap. */
  Array<float3> pos_a(9);
  for (const int r : IndexRange(3)) {
    for (const int c : IndexRange(3)) {
      pos_a[r * 3 + c] = float3(float(c), float(r), 0.0f);
    }
  }

  constexpr float gap = 0.2f;
  /* Object 1: same grid shape, translated so its leftmost column (c=0: verts 0, 3, 6) sits
   * exactly `gap` past object 0's rightmost column (world x = 2 + gap). */
  Array<float3> pos_b(9);
  for (const int r : IndexRange(3)) {
    for (const int c : IndexRange(3)) {
      pos_b[r * 3 + c] = float3(2.0f + gap + float(c), float(r), 0.0f);
    }
  }

  /* Mean edge length 1 (matching the grid's 1-unit spacing) with bridge_factor 1 gives a
   * threshold of 1.0 -- comfortably bigger than `gap` (0.2) but smaller than the >= 1.2 distance
   * from any non-facing (c=0 or c=1) vertex of object 0 to its nearest object-1 vertex. */
  const Array<float> mean_edge = {1.0f, 1.0f};
  const Array<Span<float3>> positions = {pos_a.as_span(), pos_b.as_span()};

  const MultiObjectBridge bridge = detail::build_bridge_impl(
      positions, mean_edge, 1.0f, [&](const int obj, const float3 &p, const float max_dist) {
        return brute_force_nearest(positions, obj, p, max_dist);
      });

  /* Hand-check: only the 3 aligned facing corners (same row) are mutually nearest within
   * threshold 1.0; every other vertex of object 0 is at least `1 + gap` = 1.2 away from its
   * nearest object-1 vertex, which exceeds the threshold, so it is excluded by either the
   * per-vertex AABB prefilter or the threshold check before mutual-nearest is even considered. */
  ASSERT_EQ(bridge.edges.size(), 3);
  const int expected_a_vert[3] = {2, 5, 8};
  const int expected_b_vert[3] = {0, 3, 6};
  for (const int i : IndexRange(3)) {
    EXPECT_EQ(bridge.edges[i].a.object_index, 0);
    EXPECT_EQ(bridge.edges[i].a.vert, expected_a_vert[i]);
    EXPECT_EQ(bridge.edges[i].b.object_index, 1);
    EXPECT_EQ(bridge.edges[i].b.vert, expected_b_vert[i]);
    EXPECT_NEAR(bridge.edges[i].world_distance, gap, 1e-5f);
  }
}

TEST(sculpt_expand_multi, bridge_far_apart_empty)
{
  Array<float3> pos_a(9);
  for (const int r : IndexRange(3)) {
    for (const int c : IndexRange(3)) {
      pos_a[r * 3 + c] = float3(float(c), float(r), 0.0f);
    }
  }

  /* Same threshold as #bridge_two_grids_near (mean edge 1, bridge_factor 1 -> threshold 1.0), but
   * object 1 is moved far enough away (world x = 2 + far_gap, far_gap = 3.0) that even the
   * threshold-inflated AABBs cannot touch: object 0's inflated max x is 2 + 1 = 3, object 1's
   * inflated min x is (2 + 3) - 1 = 4 > 3. The AABB prefilter must reject the whole pair before
   * any per-vertex nearest query runs. */
  constexpr float far_gap = 3.0f;
  Array<float3> pos_b(9);
  for (const int r : IndexRange(3)) {
    for (const int c : IndexRange(3)) {
      pos_b[r * 3 + c] = float3(2.0f + far_gap + float(c), float(r), 0.0f);
    }
  }

  const Array<float> mean_edge = {1.0f, 1.0f};
  const Array<Span<float3>> positions = {pos_a.as_span(), pos_b.as_span()};

  const MultiObjectBridge bridge = detail::build_bridge_impl(
      positions, mean_edge, 1.0f, [&](const int obj, const float3 &p, const float max_dist) {
        return brute_force_nearest(positions, obj, p, max_dist);
      });

  EXPECT_TRUE(bridge.edges.is_empty());
}

TEST(sculpt_expand_multi, bridge_dense_overlap_capped)
{
  /* Object 0: 5 vertices crowded around the world origin -- deliberately NOT symmetric around
   * object 1's single vertex below, so there is a unique (tie-free) closest one. */
  const Array<float3> pos_a = {
      float3(0.0f, 0.0f, 0.0f),
      float3(0.01f, 0.0f, 0.0f),
      float3(-0.01f, 0.0f, 0.0f),
      float3(0.0f, 0.01f, 0.0f),
      float3(0.0f, -0.01f, 0.0f),
  };
  /* Object 1: a single vertex, close enough to every object-0 vertex above (all within threshold
   * 1.0) that a naive (non-mutual, uncapped) match would wire all 5 of them to it. */
  const Array<float3> pos_b = {float3(0.003f, 0.0f, 0.0f)};

  const Array<float> mean_edge = {1.0f, 1.0f};
  const Array<Span<float3>> positions = {pos_a.as_span(), pos_b.as_span()};

  const MultiObjectBridge bridge = detail::build_bridge_impl(
      positions, mean_edge, 1.0f, [&](const int obj, const float3 &p, const float max_dist) {
        return brute_force_nearest(positions, obj, p, max_dist);
      });

  /* Hand-check: object-0 vertex 0 is at world distance 0.003 from object 1's only vertex, vertex 1
   * is 0.007 away, vertex 2 is 0.013, vertices 3/4 are ~0.01044 -- vertex 0 is the UNIQUE closest,
   * so it is the only one for which the mutual-nearest check (`nearest(0, pos_b[0], ...) == va`)
   * holds. Mutual-nearest alone already collapses the many-to-one candidates to this single
   * stitch here; the per-vertex cap is what keeps that guarantee in the general case where
   * multiple object PAIRS could otherwise each independently claim the same vertex. */
  ASSERT_EQ(bridge.edges.size(), 1);
  EXPECT_EQ(bridge.edges[0].a.object_index, 0);
  EXPECT_EQ(bridge.edges[0].a.vert, 0);
  EXPECT_EQ(bridge.edges[0].b.object_index, 1);
  EXPECT_EQ(bridge.edges[0].b.vert, 0);
  EXPECT_NEAR(bridge.edges[0].world_distance, 0.003f, 1e-6f);

  /* No vertex of either object appears in more than one edge (trivially true with a single
   * surviving edge here, but this is the property the cap policy exists to guarantee). */
  Set<int> a_verts_used;
  Set<int> b_verts_used;
  for (const BridgeEdge &edge : bridge.edges) {
    EXPECT_TRUE(a_verts_used.add(edge.a.vert));
    EXPECT_TRUE(b_verts_used.add(edge.b.vert));
  }
}

TEST(sculpt_expand_multi, bridge_cap_evicts_cross_pair_candidate)
{
  /* Unlike #bridge_dense_overlap_capped (2 objects, where mutual-nearest alone already collapses
   * the candidates to 1 survivor -- the cap's reject branch is never taken there), this uses 3
   * objects A, B, C, each with a SINGLE vertex. With only one vertex per object, mutual-nearest is
   * unconditionally satisfied for every pair that passes the threshold/AABB checks (there is no
   * other vertex to lose to WITHIN a pair), so all 3 unordered pairs -- (A,B), (A,C), (B,C) --
   * independently produce a mutually-nearest candidate. That means, unlike the 2-object test,
   * mutual-nearest alone yields 3 candidates here; only the per-vertex CAP (which spans across
   * pairs) can narrow that down, so its eviction branch is unavoidably exercised. */
  const Array<float3> pos_a = {float3(0.0f, 0.0f, 0.0f)};
  const Array<float3> pos_b = {float3(0.02f, 0.0f, 0.0f)};
  /* C sits so that `gap(B, C)` (0.01) < `gap(A, B)` (0.02) < `gap(A, C)` (`sqrt(0.0005) ~=
   * 0.02236`) -- 3 distinct gaps, no ties, chosen so the cap map's winner changes as candidates
   * are scanned in (A,B), (A,C), (B,C) order (see the hand-trace below). */
  const Array<float3> pos_c = {float3(0.02f, 0.01f, 0.0f)};

  const float gap_ab = 0.02f;
  const float gap_bc = 0.01f;
  const float gap_ac = 0.0223606798f; /* = sqrt(0.0005), i.e. sqrt(0.02^2 + 0.01^2). */

  const Array<float> mean_edge = {1.0f, 1.0f, 1.0f};
  const Array<Span<float3>> positions = {pos_a.as_span(), pos_b.as_span(), pos_c.as_span()};

  const MultiObjectBridge bridge = detail::build_bridge_impl(
      positions, mean_edge, 1.0f, [&](const int obj, const float3 &p, const float max_dist) {
        return brute_force_nearest(positions, obj, p, max_dist);
      });

  /* Hand-trace of "Step 3: cap + dedup" (`sculpt_expand_multi.cc`), in pair-scan order
   * (a < b, so (A,B) then (A,C) then (B,C)):
   * - i=0 (A,B), gap 0.02: both keys unset -> `best_for_vert[A] = 0`, `best_for_vert[B] = 0`.
   * - i=1 (A,C), gap 0.02236: key A already holds i=0 (gap 0.02); `0.02 > 0.02236` is FALSE, so
   *   i=0 is kept for A. Key C unset -> `best_for_vert[C] = 1`.
   * - i=2 (B,C), gap 0.01: key B already holds i=0 (gap 0.02); `0.02 > 0.01` is TRUE, so B is
   *   overwritten to `best_for_vert[B] = 2`. Key C already holds i=1 (gap 0.02236); `0.02236 >
   *   0.01` is TRUE, so C is overwritten to `best_for_vert[C] = 2`.
   * Final map: `best_for_vert[A] = 0`, `best_for_vert[B] = 2`, `best_for_vert[C] = 2`.
   *
   * Step 4 emission then evicts two of the three candidates:
   * - i=0 (A,B) is REJECTED on its B endpoint: `best_for_vert[B] == 2 != 0`. This is the
   *   candidate whose eviction actually depends on the cap: its A endpoint alone would have
   *   passed (`best_for_vert[A] == 0`), so if the B-endpoint check were ever dropped, (A,B) would
   *   incorrectly survive alongside (B,C) below, and this test's `ASSERT_EQ(edges.size(), 1)`
   *   would fail.
   * - i=1 (A,C) is REJECTED on its A endpoint: `best_for_vert[A] == 0 != 1`.
   * - i=2 (B,C) SURVIVES: both endpoints match (`best_for_vert[B] == 2`, `best_for_vert[C] ==
   *   2`).
   * If the entire cap step were deleted (every mutual-nearest candidate emitted unconditionally),
   * all 3 candidates above would survive instead of 1, so the size assertion below would also
   * catch that. */
  ASSERT_EQ(bridge.edges.size(), 1);
  EXPECT_EQ(bridge.edges[0].a.object_index, 1);
  EXPECT_EQ(bridge.edges[0].a.vert, 0);
  EXPECT_EQ(bridge.edges[0].b.object_index, 2);
  EXPECT_EQ(bridge.edges[0].b.vert, 0);
  EXPECT_NEAR(bridge.edges[0].world_distance, gap_bc, 1e-6f);

  /* Object A's single vertex is evicted from BOTH of its candidate stitches (not merely capped to
   * one) -- it never appears as an endpoint of any surviving edge, since (A,B) lost to (B,C) on
   * B's endpoint and (A,C) lost to (A,B) on A's own endpoint. */
  for (const BridgeEdge &edge : bridge.edges) {
    EXPECT_FALSE(edge.a.object_index == 0);
    EXPECT_FALSE(edge.b.object_index == 0);
  }

  /* Sanity-check the hand-derived gaps referenced above actually match what #build_bridge_impl
   * used, so the trace is verifying the real inputs rather than a stale mental model. */
  EXPECT_NEAR(math::distance(pos_a[0], pos_b[0]), gap_ab, 1e-6f);
  EXPECT_NEAR(math::distance(pos_a[0], pos_c[0]), gap_ac, 1e-6f);
  EXPECT_NEAR(math::distance(pos_b[0], pos_c[0]), gap_bc, 1e-6f);
}

}  // namespace blender::ed::sculpt_paint::expand::tests
