/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "paint_intern.hh"

namespace blender::ed::sculpt_paint::tests {

static RollSpline make_straight_x(const int n)
{
  RollSpline s;
  for (const int i : IndexRange(n)) {
    s.poly_3d.append(float3(float(i), 0.0f, 0.0f));
    s.poly_2d.append(float2(float(i), 0.0f));
    s.pressures.append(1.0f);
    s.normals.append(float3(0.0f, 0.0f, 1.0f));
  }
  s.update_lengths();
  return s;
}

TEST(roll_spline, EmptyBelowTwoKnots)
{
  RollSpline s = make_straight_x(1);
  EXPECT_TRUE(s.is_empty());
}

TEST(roll_spline, ArcLengthOfUnitSegments)
{
  RollSpline s = make_straight_x(4);
  EXPECT_FALSE(s.is_empty());
  EXPECT_NEAR(s.total_length_3d(), 3.0f, 1e-5f);
  EXPECT_NEAR(s.total_length_2d(), 3.0f, 1e-5f);
  EXPECT_NEAR(s.segment_length_3d(1), 1.0f, 1e-5f);
}

TEST(roll_spline, EvaluateMidpoint)
{
  RollSpline s = make_straight_x(4);
  const float3 p = s.evaluate_3d(1.5f);
  EXPECT_NEAR(p.x, 1.5f, 1e-5f);
  EXPECT_NEAR(p.y, 0.0f, 1e-5f);
}

TEST(roll_spline, ClosestPointDistanceAndArcLength)
{
  RollSpline s = make_straight_x(4);
  float arc, dist;
  float3 tan;
  s.closest_point_3d(float3(2.0f, 0.5f, 0.0f), arc, tan, dist);
  EXPECT_NEAR(arc, 2.0f, 1e-4f);
  EXPECT_NEAR(dist, 0.5f, 1e-4f);
  EXPECT_NEAR(tan.x, 1.0f, 1e-4f);
}

}  // namespace blender::ed::sculpt_paint::tests
