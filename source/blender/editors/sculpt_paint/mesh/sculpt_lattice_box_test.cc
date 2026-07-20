/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sculpt_lattice_intern.hh"

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

}  // namespace blender::ed::sculpt_paint::lattice::tests
