/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sculpt_lattice_intern.hh"
#include "ED_sculpt_lattice_draw.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::lattice::tests {

TEST(SculptLatticeBox, LocalRectEdgeOrigin)
{
  const Bounds<float2> rect = sculpt_lattice_box_local_rect(float2(2.0f, 1.0f), false, false);
  EXPECT_V2_NEAR(rect.min, float2(0.0f, 0.0f), 1e-6f);
  EXPECT_V2_NEAR(rect.max, float2(2.0f, 1.0f), 1e-6f);
}

TEST(SculptLatticeBox, LocalRectEdgeOriginNegative)
{
  const Bounds<float2> rect = sculpt_lattice_box_local_rect(float2(-2.0f, 1.0f), false, false);
  EXPECT_V2_NEAR(rect.min, float2(-2.0f, 0.0f), 1e-6f);
  EXPECT_V2_NEAR(rect.max, float2(0.0f, 1.0f), 1e-6f);
}

TEST(SculptLatticeBox, LocalRectCenterOrigin)
{
  const Bounds<float2> rect = sculpt_lattice_box_local_rect(float2(2.0f, 1.0f), true, false);
  EXPECT_V2_NEAR(rect.min, float2(-2.0f, -1.0f), 1e-6f);
  EXPECT_V2_NEAR(rect.max, float2(2.0f, 1.0f), 1e-6f);
}

TEST(SculptLatticeBox, LocalRectFixedAspectLocksHeightToWidth)
{
  const Bounds<float2> rect = sculpt_lattice_box_local_rect(float2(2.0f, 1.0f), false, true);
  EXPECT_V2_NEAR(rect.min, float2(0.0f, 0.0f), 1e-6f);
  EXPECT_V2_NEAR(rect.max, float2(2.0f, 2.0f), 1e-6f);
}

TEST(SculptLatticeBox, TransformExtendsAlongNegativeNormal)
{
  const Bounds<float2> rect(float2(0.0f, 0.0f), float2(2.0f, 2.0f));
  const LatticeBoxTransform xf = sculpt_lattice_box_transform_from_rect(
      float3(0.0f), float3x3::identity(), rect, float3(0.0f), 4.0f, false);
  EXPECT_V3_NEAR(xf.front_center, float3(1.0f, 1.0f, 0.0f), 1e-5f);
  EXPECT_V3_NEAR(xf.location, float3(1.0f, 1.0f, -2.0f), 1e-5f);
  EXPECT_V3_NEAR(xf.scale, float3(2.0f, 2.0f, 4.0f), 1e-5f);
  EXPECT_V3_NEAR(xf.extrude_dir, float3(0.0f, 0.0f, -1.0f), 1e-5f);
}

TEST(SculptLatticeBox, TransformFlipExtendsAlongPositiveNormal)
{
  const Bounds<float2> rect(float2(0.0f, 0.0f), float2(2.0f, 2.0f));
  const LatticeBoxTransform xf = sculpt_lattice_box_transform_from_rect(
      float3(0.0f), float3x3::identity(), rect, float3(0.0f), 4.0f, true);
  EXPECT_V3_NEAR(xf.location, float3(1.0f, 1.0f, 2.0f), 1e-5f);
  EXPECT_V3_NEAR(xf.extrude_dir, float3(0.0f, 0.0f, 1.0f), 1e-5f);
}

TEST(SculptLatticeBox, ThicknessFromPerpendicularRay)
{
  const std::optional<float> thickness = sculpt_lattice_box_thickness_from_lines(
      float3(0.0f),
      float3(0.0f, 0.0f, -1.0f),
      float3(1.0f, 0.0f, -3.0f),
      float3(1.0f, 1.0f, -3.0f),
      1e-4f);
  ASSERT_TRUE(thickness.has_value());
  EXPECT_NEAR(*thickness, 3.0f, 1e-5f);
}

TEST(SculptLatticeOverlay, CubeHasTwelveEdges)
{
  Vector<int2> edges;
  lattice_cage_edges_build(int3(2, 2, 2), edges);
  EXPECT_EQ(edges.size(), 12);

  Vector<int> points;
  Vector<int2> shell_edges;
  lattice_cage_overlay_topology_build(int3(2, 2, 2), true, points, shell_edges);
  EXPECT_EQ(points.size(), 8);
  EXPECT_EQ(shell_edges.size(), 12);
}

TEST(SculptLatticeOverlay, ShellOmitsInteriorAtHigherRes)
{
  Vector<int> full_points;
  Vector<int2> full_edges;
  lattice_cage_overlay_topology_build(int3(4, 4, 4), false, full_points, full_edges);

  Vector<int> shell_points;
  Vector<int2> shell_edges;
  lattice_cage_overlay_topology_build(int3(4, 4, 4), true, shell_points, shell_edges);

  EXPECT_EQ(full_points.size(), 64);
  EXPECT_EQ(full_edges.size(), 144);
  EXPECT_LT(shell_points.size(), full_points.size());
  EXPECT_LT(shell_edges.size(), full_edges.size());
}

TEST(SculptLatticeUndo, CageXformSwapRoundTrip)
{
  float loc_stored[3] = {1.0f, 2.0f, 3.0f};
  float quat_stored[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float scale_stored[3] = {1.0f, 1.0f, 1.0f};
  float loc_live[3] = {4.0f, 5.0f, 6.0f};
  float quat_live[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  float scale_live[3] = {2.0f, 3.0f, 4.0f};

  sculpt_lattice_cage_xform_swap(
      loc_stored, quat_stored, scale_stored, loc_live, quat_live, scale_live);
  EXPECT_V3_NEAR(loc_stored, float3(4.0f, 5.0f, 6.0f), 1e-6f);
  EXPECT_V3_NEAR(loc_live, float3(1.0f, 2.0f, 3.0f), 1e-6f);
  EXPECT_NEAR(quat_stored[1], 1.0f, 1e-6f);
  EXPECT_NEAR(quat_live[0], 1.0f, 1e-6f);
  EXPECT_V3_NEAR(scale_stored, float3(2.0f, 3.0f, 4.0f), 1e-6f);
  EXPECT_V3_NEAR(scale_live, float3(1.0f, 1.0f, 1.0f), 1e-6f);

  sculpt_lattice_cage_xform_swap(
      loc_stored, quat_stored, scale_stored, loc_live, quat_live, scale_live);
  EXPECT_V3_NEAR(loc_stored, float3(1.0f, 2.0f, 3.0f), 1e-6f);
  EXPECT_V3_NEAR(loc_live, float3(4.0f, 5.0f, 6.0f), 1e-6f);
}

}  // namespace blender::ed::sculpt_paint::lattice::tests
