/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>

#include "BLI_math_base.h"
#include "BLI_math_vector.hh"

#include "paint_curve_patch_ribbon.hh"
#include "paint_curve_patch_spline.hh"

namespace blender::ed::sculpt_paint::tests {

/* Tolerances are loose on purpose: the ribbon runs two Catmull-Clark passes, Laplacian smoothing,
 * and a rasterized LUT, so exact analytic values only survive up to grid/LUT resolution. */

/** Hairpin whose two legs run `leg_separation` apart in Y, joined by a semicircle on the right. */
static void build_hairpin(CurvePatchSpline &spline, const float leg_separation)
{
  const float half = leg_separation * 0.5f;
  Vector<float3> points;
  for (int i = 0; i <= 18; i++) {
    points.append(float3(-1.0f + float(i) * 0.1f, -half, 0.0f));
  }
  for (int i = 1; i < 12; i++) {
    const float a = -float(M_PI_2) + float(M_PI) * float(i) / 12.0f;
    points.append(float3(0.8f + half * std::cos(a), half * std::sin(a), 0.0f));
  }
  for (int i = 0; i <= 18; i++) {
    points.append(float3(0.8f - float(i) * 0.1f, half, 0.0f));
  }
  spline.build_from_positions(points.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);
}

TEST(paint_curve_patch_ribbon, straight_line_uv)
{
  CurvePatchSpline spline;
  Vector<float3> points;
  for (int i = 0; i <= 20; i++) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
  }
  spline.build_from_positions(points.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.5f, lut);
  ASSERT_TRUE(lut.ready);

  /* A curve that never approaches itself yields exactly one branch everywhere. */
  float2 uv[2];
  ASSERT_EQ(lut.sample(float3(1.0f, 0.0f, 0.0f), uv), 1);
  /* On the center line: u ~ 0, v ~ arc length. */
  EXPECT_NEAR(uv[0].x, 0.0f, 0.1f);
  EXPECT_NEAR(uv[0].y, 1.0f, 0.1f);

  /* Halfway toward the `cross(tangent, plane_normal)` border (y = -R/2 for a +X curve under a +Z
   * plane normal): u ~ +0.5, matching the old `-lateral / radius` sign convention. */
  ASSERT_EQ(lut.sample(float3(1.0f, -0.25f, 0.0f), uv), 1);
  EXPECT_NEAR(uv[0].x, 0.5f, 0.15f);
  EXPECT_NEAR(uv[0].y, 1.0f, 0.1f);

  /* Opposite side mirrors the sign. */
  ASSERT_EQ(lut.sample(float3(1.0f, 0.25f, 0.0f), uv), 1);
  EXPECT_NEAR(uv[0].x, -0.5f, 0.15f);

  /* Far off the strip, and far past the curve's end: no branch at all. */
  EXPECT_EQ(lut.sample(float3(1.0f, 5.0f, 0.0f), uv), 0);
  EXPECT_EQ(lut.sample(float3(10.0f, 0.0f, 0.0f), uv), 0);
}

TEST(paint_curve_patch_ribbon, sharp_corner_single_valued)
{
  /* L-shaped curve with a 90 degree corner at (1, 0). The old closest-point parameterization was
   * multi-valued for points on the inner (concave) side of the corner; the ribbon must return one
   * valid, in-range UV there. */
  CurvePatchSpline spline;
  Vector<float3> points;
  for (int i = 0; i < 10; i++) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
  }
  for (int i = 0; i <= 10; i++) {
    points.append(float3(1.0f, float(i) * 0.1f, 0.0f));
  }
  spline.build_from_positions(points.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.3f, lut);
  ASSERT_TRUE(lut.ready);

  /* Inner-side point near the corner, within R of both legs. */
  float2 uv[2];
  ASSERT_GT(lut.sample(float3(0.9f, 0.1f, 0.0f), uv), 0);
  EXPECT_TRUE(std::isfinite(uv[0].x));
  EXPECT_TRUE(std::isfinite(uv[0].y));
  EXPECT_GE(uv[0].y, 0.0f);
  EXPECT_LE(uv[0].y, spline.total_length() + 1e-4f);
  EXPECT_GE(uv[0].x, -1.0f - 1e-4f);
  EXPECT_LE(uv[0].x, 1.0f + 1e-4f);
}

TEST(paint_curve_patch_ribbon, hairpin_inner_side_collapsed)
{
  /* Turn radius (0.3) smaller than the strip half-width (0.5), so the inner border self-intersects
   * and must be collapsed. Points inside the fold must still map to a valid UV. */
  CurvePatchSpline spline;
  build_hairpin(spline, 0.6f);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.5f, lut);
  ASSERT_TRUE(lut.ready);

  float2 uv[2];
  ASSERT_GT(lut.sample(float3(0.8f, 0.0f, 0.0f), uv), 0);
  EXPECT_TRUE(std::isfinite(uv[0].x));
  EXPECT_TRUE(std::isfinite(uv[0].y));
  EXPECT_GE(uv[0].y, 0.0f);
  EXPECT_LE(uv[0].y, spline.total_length() + 1e-4f);
}

TEST(paint_curve_patch_ribbon, self_approach_prefers_the_more_central_leg)
{
  /* Legs 0.4 apart while the strip is 1.0 wide, so each leg's ribbon covers the other leg's center
   * line. A point sitting exactly on the SECOND leg's center line is also inside the FIRST leg's
   * ribbon, near that ribbon's outer edge.
   *
   * Arbitrating such a pixel by age (Roll's rule) hands it to the first leg, which reports an
   * across-strip coordinate of ~0.8 -- driving the brush falloff to ~0 and punching a relief-free
   * hole into the second leg. The primary branch must be the leg that covers it centrally. */
  CurvePatchSpline spline;
  build_hairpin(spline, 0.4f);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.5f, lut);
  ASSERT_TRUE(lut.ready);

  float2 uv[2];
  ASSERT_GT(lut.sample(float3(0.0f, 0.2f, 0.0f), uv), 0);
  /* Central on the second leg, not near the first leg's edge. */
  EXPECT_LT(std::abs(uv[0].x), 0.4f);
  /* And its arc length belongs to the second leg (past the hairpin), not the first. */
  EXPECT_GT(uv[0].y, 0.5f * spline.total_length());
}

TEST(paint_curve_patch_ribbon, parallel_legs_report_both_branches)
{
  /* On the medial axis between two parallel legs both stretches cover the point equally. Reporting
   * only one of them is what leaves a hard seam there: the along-length coordinate jumps from one
   * leg's arc length to the other's as the winner flips. Both must come back so the relief can
   * merge them. */
  CurvePatchSpline spline;
  build_hairpin(spline, 0.4f);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.5f, lut);
  ASSERT_TRUE(lut.ready);

  float2 uv[2];
  ASSERT_EQ(lut.sample(float3(0.0f, 0.0f, 0.0f), uv), 2);
  /* The two branches are genuinely different stretches of the curve, not the same one twice. */
  EXPECT_GT(std::abs(uv[0].y - uv[1].y), 1.0f);
  /* Both sit within the strip and within the curve. */
  for (const int b : IndexRange(2)) {
    EXPECT_LE(std::abs(uv[b].x), 1.0f + 1e-4f);
    EXPECT_GE(uv[b].y, 0.0f);
    EXPECT_LE(uv[b].y, spline.total_length() + 1e-4f);
  }
}

}  // namespace blender::ed::sculpt_paint::tests
