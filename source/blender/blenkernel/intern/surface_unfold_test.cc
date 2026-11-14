/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_surface_unfold.hh"

#include "BLI_math_vector.hh"
#include "BLI_set.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

/* Corner triangle `t` uses corners `3t..3t+2`, so corner indices differ from vertex indices. */
struct UnfoldMesh {
  Vector<int3> corner_tris;
  Vector<int> corner_verts;
  Vector<float3> positions;

  void add_tri(const int a, const int b, const int c)
  {
    const int corner = int(this->corner_verts.size());
    this->corner_tris.append(int3(corner, corner + 1, corner + 2));
    this->corner_verts.append(a);
    this->corner_verts.append(b);
    this->corner_verts.append(c);
  }

  bool build(const int seed,
             const float3 &seed_point,
             const float3 &tangent,
             const float radius,
             const int max_tris,
             SurfaceUnfoldPatch &r_patch) const
  {
    const SurfaceUnfoldAdjacency adjacency = surface_unfold_adjacency_build(this->corner_tris,
                                                                           this->corner_verts);
    return surface_unfold_build(this->corner_tris,
                                this->corner_verts,
                                this->positions,
                                adjacency,
                                seed,
                                seed_point,
                                tangent,
                                radius,
                                max_tris,
                                r_patch);
  }
};

/* Plane position of mesh vertex \a vert inside patch triangle \a tri. */
static float2 plane_of_vert(const UnfoldMesh &mesh, const SurfaceUnfoldTri &tri, const int vert)
{
  for (const int i : IndexRange(3)) {
    if (mesh.corner_verts[mesh.corner_tris[tri.tri_index][i]] == vert) {
      return tri.plane[i];
    }
  }
  ADD_FAILURE() << "vertex not in triangle";
  return float2(0.0f);
}

static void expect_isometric(const SurfaceUnfoldPatch &patch)
{
  for (const SurfaceUnfoldTri &tri : patch.tris) {
    for (const int i : IndexRange(3)) {
      const int j = (i + 1) % 3;
      EXPECT_NEAR(math::distance(tri.co[i], tri.co[j]),
                  math::distance(tri.plane[i], tri.plane[j]),
                  1e-4f);
    }
  }
}

/* Unit quad in the XY plane: (0,1,2) and (0,2,3). */
static UnfoldMesh make_quad()
{
  UnfoldMesh m;
  m.positions = {float3(0, 0, 0), float3(1, 0, 0), float3(1, 1, 0), float3(0, 1, 0)};
  m.add_tri(0, 1, 2);
  m.add_tri(0, 2, 3);
  return m;
}

/* Bottom quad (z = 0) plus a wall (y = 0) sharing the edge 0-1, like a cube edge. */
static UnfoldMesh make_cube_edge()
{
  UnfoldMesh m;
  m.positions = {float3(0, 0, 0),
                 float3(1, 0, 0),
                 float3(1, 1, 0),
                 float3(0, 1, 0),
                 float3(0, 0, 1),
                 float3(1, 0, 1)};
  m.add_tri(0, 1, 2);
  m.add_tri(0, 2, 3);
  m.add_tri(1, 0, 4);
  m.add_tri(1, 4, 5);
  return m;
}

TEST(SurfaceUnfold, AdjacencyQuad)
{
  const UnfoldMesh m = make_quad();
  const SurfaceUnfoldAdjacency adj = surface_unfold_adjacency_build(m.corner_tris, m.corner_verts);
  ASSERT_EQ(adj.neighbors.size(), 2);
  /* Shared edge 2->0 of tri 0 is edge index 2; edge 0->2 of tri 1 is edge index 0. */
  EXPECT_EQ(adj.neighbors[0][2], 1);
  EXPECT_EQ(adj.neighbors[1][0], 0);
  /* Boundary edges have no neighbor. */
  EXPECT_EQ(adj.neighbors[0][0], -1);
  EXPECT_EQ(adj.neighbors[0][1], -1);
  EXPECT_EQ(adj.neighbors[1][1], -1);
  EXPECT_EQ(adj.neighbors[1][2], -1);
}

TEST(SurfaceUnfold, FlatQuadKeepsLengthsAndOrientation)
{
  const UnfoldMesh m = make_quad();
  SurfaceUnfoldPatch patch;
  const float3 seed_point(0.7f, 0.2f, 0.0f);
  ASSERT_TRUE(m.build(0, seed_point, float3(1, 0, 0), 10.0f, 100, patch));
  ASSERT_EQ(patch.tris.size(), 2);
  EXPECT_EQ(patch.tris[0].tri_index, 0);
  expect_isometric(patch);
  /* Seed normal is +Z and X follows the tangent, so a flat mesh maps to itself shifted by the
   * seed point. */
  for (const SurfaceUnfoldTri &tri : patch.tris) {
    for (const int i : IndexRange(3)) {
      EXPECT_NEAR(tri.plane[i].x, tri.co[i].x - seed_point.x, 1e-4f);
      EXPECT_NEAR(tri.plane[i].y, tri.co[i].y - seed_point.y, 1e-4f);
    }
  }
}

TEST(SurfaceUnfold, CubeEdgeUnfoldsFlat)
{
  const UnfoldMesh m = make_cube_edge();
  SurfaceUnfoldPatch patch;
  const float3 seed_point(0.5f, 0.3f, 0.0f);
  ASSERT_TRUE(m.build(0, seed_point, float3(1, 0, 0), 10.0f, 100, patch));
  ASSERT_EQ(patch.tris.size(), 4);
  expect_isometric(patch);

  const SurfaceUnfoldTri *bottom = nullptr;
  const SurfaceUnfoldTri *wall = nullptr;
  for (const SurfaceUnfoldTri &tri : patch.tris) {
    if (tri.tri_index == 0) {
      bottom = &tri;
    }
    if (tri.tri_index == 2) {
      wall = &tri;
    }
  }
  ASSERT_TRUE(bottom && wall);

  /* Shared edge vertices 0 and 1 coincide in both triangles. */
  for (const int v : {0, 1}) {
    const float2 a = plane_of_vert(m, *bottom, v);
    const float2 b = plane_of_vert(m, *wall, v);
    EXPECT_NEAR(a.x, b.x, 1e-4f);
    EXPECT_NEAR(a.y, b.y, 1e-4f);
  }
  /* The wall continues on the far side of the shared edge (-Y), with its height laid out flat. */
  const float2 e0 = plane_of_vert(m, *wall, 0);
  const float2 top = plane_of_vert(m, *wall, 4);
  EXPECT_NEAR(top.x, e0.x, 1e-4f);
  EXPECT_NEAR(top.y, e0.y - 1.0f, 1e-4f);
  /* The bottom triangle's far vertex lies on the opposite side from the wall. */
  EXPECT_GT(plane_of_vert(m, *bottom, 2).y, e0.y);
  /* Absolute position: seed at the origin. */
  EXPECT_NEAR(e0.x, -0.5f, 1e-4f);
  EXPECT_NEAR(e0.y, -0.3f, 1e-4f);
}

TEST(SurfaceUnfold, RadiusAndMaxTrisLimitGrowth)
{
  const UnfoldMesh m = make_cube_edge();
  SurfaceUnfoldPatch patch;

  /* A tiny radius keeps only the seed neighborhood: the wall tip lies beyond it. */
  ASSERT_TRUE(m.build(0, float3(0.5f, 0.8f, 0.0f), float3(1, 0, 0), 0.05f, 100, patch));
  EXPECT_GE(patch.tris.size(), 1);
  EXPECT_LT(patch.tris.size(), 4);
  EXPECT_EQ(patch.tris[0].tri_index, 0);

  /* max_tris is a ceiling. */
  SurfaceUnfoldPatch capped;
  ASSERT_TRUE(m.build(0, float3(0.5f, 0.5f, 0.0f), float3(1, 0, 0), 10.0f, 2, capped));
  EXPECT_LE(capped.tris.size(), 2);
  EXPECT_TRUE(capped.truncated);
}

TEST(SurfaceUnfold, NonManifoldAndBoundaryEdgesAreNotCrossed)
{
  /* Three triangles on edge 0-1: no neighbor across it. */
  UnfoldMesh m;
  m.positions = {float3(0, 0, 0),
                 float3(1, 0, 0),
                 float3(0.5f, 1, 0),
                 float3(0.5f, -1, 0),
                 float3(0.5f, 0, 1)};
  m.add_tri(0, 1, 2);
  m.add_tri(1, 0, 3);
  m.add_tri(1, 0, 4);
  const SurfaceUnfoldAdjacency adj = surface_unfold_adjacency_build(m.corner_tris, m.corner_verts);
  EXPECT_EQ(adj.neighbors[0][0], -1);
  EXPECT_EQ(adj.neighbors[1][0], -1);
  EXPECT_EQ(adj.neighbors[2][0], -1);
  SurfaceUnfoldPatch patch;
  ASSERT_TRUE(m.build(0, float3(0.5f, 0.3f, 0.0f), float3(1, 0, 0), 10.0f, 100, patch));
  EXPECT_EQ(patch.tris.size(), 1);

  /* A lone triangle is bounded on all sides. */
  UnfoldMesh single;
  single.positions = {float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0)};
  single.add_tri(0, 1, 2);
  SurfaceUnfoldPatch lone;
  ASSERT_TRUE(single.build(0, float3(0.2f, 0.2f, 0.0f), float3(1, 0, 0), 10.0f, 100, lone));
  EXPECT_EQ(lone.tris.size(), 1);
}

TEST(SurfaceUnfold, EachTriangleVisitedOnce)
{
  /* A 4x4 grid of quads. */
  UnfoldMesh m;
  const int n = 5;
  for (const int y : IndexRange(n)) {
    for (const int x : IndexRange(n)) {
      m.positions.append(float3(float(x), float(y), 0.0f));
    }
  }
  for (const int y : IndexRange(n - 1)) {
    for (const int x : IndexRange(n - 1)) {
      const int v0 = y * n + x;
      m.add_tri(v0, v0 + 1, v0 + n + 1);
      m.add_tri(v0, v0 + n + 1, v0 + n);
    }
  }
  SurfaceUnfoldPatch patch;
  ASSERT_TRUE(m.build(0, float3(0.6f, 0.3f, 0.0f), float3(1, 0, 0), 100.0f, 1000, patch));
  EXPECT_EQ(patch.tris.size(), m.corner_tris.size());
  Set<int> seen;
  for (const SurfaceUnfoldTri &tri : patch.tris) {
    EXPECT_TRUE(seen.add(tri.tri_index));
  }
  expect_isometric(patch);
}

}  // namespace blender::bke::tests
