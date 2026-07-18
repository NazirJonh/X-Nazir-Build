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

TEST(paint_curve_patch_ribbon, high_quality_refines_without_moving_the_mapping)
{
  /* The commit-time pass rebuilds the LUT at a finer pixel density so its supersampled taps, which
   * sit a few percent of a strip-width apart, are not all resolved against the same coarse cell.
   * Raising the resolution must REFINE the mapping, never shift it. */
  CurvePatchSpline spline;
  Vector<float3> points;
  for (int i = 0; i <= 30; i++) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
  }
  spline.build_from_positions(points.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  const float R = 0.5f;
  CurvePatchRibbonLut fast, fine;
  curve_patch_ribbon_build(spline, R, fast, false);
  curve_patch_ribbon_build(spline, R, fine, true);
  ASSERT_TRUE(fast.ready);
  ASSERT_TRUE(fine.ready);
  EXPECT_GT(fine.res, fast.res);

  /* Same UV within what the coarser table's own quantization allows. */
  for (const float x : {0.5f, 1.5f, 2.5f}) {
    for (const float y : {-0.2f, 0.0f, 0.2f}) {
      float2 uv_fast[2], uv_fine[2];
      const int n_fast = fast.sample(float3(x, y, 0.0f), uv_fast);
      const int n_fine = fine.sample(float3(x, y, 0.0f), uv_fine);
      ASSERT_EQ(n_fast, 1);
      ASSERT_EQ(n_fine, 1);
      EXPECT_NEAR(uv_fine[0].x, uv_fast[0].x, 0.15f);
      EXPECT_NEAR(uv_fine[0].y, uv_fast[0].y, 0.15f);
    }
  }
}

TEST(paint_curve_patch_ribbon, quality_setting_participates_in_the_cache_hash)
{
  /* The build returns early when `source_hash` matches. If the quality setting were left out of it,
   * the commit-time rebuild would be answered with the interactive table -- the very table it exists
   * to replace. */
  CurvePatchSpline spline;
  Vector<float3> points;
  for (int i = 0; i <= 20; i++) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
  }
  spline.build_from_positions(points.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  CurvePatchRibbonLut fast, fine;
  curve_patch_ribbon_build(spline, 0.5f, fast, false);
  curve_patch_ribbon_build(spline, 0.5f, fine, true);
  EXPECT_NE(fast.source_hash, fine.source_hash);

  /* Rebuilding with identical inputs leaves a usable table (the early-out path must not clear it).
   * This does not prove the rebuild was skipped -- the observable state is the same either way. */
  const int res_before = fine.res;
  curve_patch_ribbon_build(spline, 0.5f, fine, true);
  EXPECT_EQ(fine.res, res_before);
  EXPECT_TRUE(fine.ready);
}

TEST(paint_curve_patch_ribbon, closed_loop_strip_survives_the_join)
{
  /* Regression: the border self-intersection collapse used to treat a closed curve's own join as a
   * crossing -- the border's first and last points coincide there, so the segment test reports a
   * hit at (s = 0, t = 1), and its "all loop vertices lie within R of the center curve" validation
   * cannot reject it, because on a closed loop that is true of the WHOLE border. The result was
   * the entire strip collapsing onto one point. */
  const float loop_radius = 2.0f;
  const int segments = 48;
  Vector<float3> points;
  for (int i = 0; i < segments; i++) {
    const float a = 2.0f * float(M_PI) * float(i) / float(segments);
    points.append(float3(loop_radius * std::cos(a), loop_radius * std::sin(a), 0.0f));
  }

  CurvePatchSpline spline;
  /* As the evaluated points of a cyclic curve arrive: the first point is not repeated at the end. */
  spline.build_from_positions(points.as_span(), {}, /*cyclic=*/true);
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.3f, lut);
  ASSERT_TRUE(lut.ready);

  const float total = spline.total_length();

  /* A quarter of the way around, well clear of the join: the strip must still be there, with the
   * sample sitting on its center line. */
  float2 uv[2];
  ASSERT_EQ(lut.sample(float3(0.0f, loop_radius, 0.0f), uv), 1);
  EXPECT_NEAR(uv[0].x, 0.0f, 0.15f);
  EXPECT_NEAR(uv[0].y, total * 0.25f, total * 0.05f);

  /* Three quarters around, i.e. on the far side of the join from the first probe. */
  ASSERT_EQ(lut.sample(float3(0.0f, -loop_radius, 0.0f), uv), 1);
  EXPECT_NEAR(uv[0].x, 0.0f, 0.15f);
  EXPECT_NEAR(uv[0].y, total * 0.75f, total * 0.05f);

  /* The loop's interior is not part of the strip. */
  EXPECT_EQ(lut.sample(float3(0.0f, 0.0f, 0.0f), uv), 0);
}

}  // namespace blender::ed::sculpt_paint::tests
