/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Unit-tests for the Curve gradient polyline (`paint_gradient_curve.hh`).
 * Run via ctest or `blender_test --gtest_filter=sculpt_paint_gradient_curve*`.
 */

#include <cmath>
#include <limits>

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "paint_gradient_curve.hh"

namespace blender::ed::sculpt_paint::gradient_curve::tests {

static constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
static constexpr float INF = std::numeric_limits<float>::infinity();

/** Densify a polyline so consecutive points are at most \a step apart. With the builder's spline
 * resolution target of one point per spacing step, a dense input evaluates as a plain polyline
 * (resolution 1), which keeps the shape-sensitive tests independent of the spline. */
static Vector<float2> densify(const Span<float2> points, const float step)
{
  Vector<float2> out;
  for (const int i : IndexRange(1, points.size() - 1)) {
    const float2 a = points[i - 1];
    const float2 b = points[i];
    const int sub = std::max(1, int(std::ceil(math::distance(a, b) / step)));
    for (const int k : IndexRange(sub)) {
      out.append(math::interpolate(a, b, float(k) / float(sub)));
    }
  }
  out.append(points.last());
  return out;
}

/* A straight polyline resampled by the curve builder must behave like the Linear gradient:
 * t follows the distance along the line, unclamped. */
TEST(sculpt_paint_gradient_curve, straight_line_matches_linear)
{
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build({float2(0, 0), float2(10, 0)}, {1.0f, 0.0f, 0}));

  EXPECT_NEAR(curve.length(), 10.0f, 1e-4f);
  /* The resampled ends are the input ends. */
  EXPECT_V2_NEAR(curve.points().first(), float2(0, 0), 1e-5f);
  EXPECT_V2_NEAR(curve.points().last(), float2(10, 0), 1e-5f);

  const CurveProjection mid = curve.project(float2(4.5f, 0));
  EXPECT_NEAR(mid.s, 4.5f, 1e-4f);
  EXPECT_NEAR(mid.distance, 0.0f, 1e-5f);

  /* Off-axis points project onto the line, matching the Linear gradient's signed projection. */
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(3, 7)), 0.3f, 1e-4f);
  /* Past the ends the parameter extrapolates (unclamped) along the end tangents. */
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(-5, 0)), -0.5f, 1e-4f);
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(15, 0)), 1.5f, 1e-4f);
}

/* On a circular arc the parameter must grow monotonically with the angle. */
TEST(sculpt_paint_gradient_curve, arc_angle_is_monotonic)
{
  const int steps = 64;
  const float radius = 5.0f;
  Array<float2> raw(steps + 1);
  for (const int i : raw.index_range()) {
    const float angle = float(i) / float(steps) * float(M_PI); /* Half circle. */
    raw[i] = float2(std::cos(angle), std::sin(angle)) * radius;
  }
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build(raw, {0.05f, 0.0f, 0}));

  EXPECT_NEAR(curve.length(), radius * float(M_PI), 0.1f);

  float prev_t = -1.0f;
  for (const int i : IndexRange(steps + 1)) {
    const float angle = float(i) / float(steps) * float(M_PI);
    const float2 sample = float2(std::cos(angle), std::sin(angle)) * radius;
    const float t = curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, sample);
    EXPECT_FALSE(std::isnan(t));
    EXPECT_NEAR(t, float(i) / float(steps), 0.02f) << "angle step " << i;
    EXPECT_GT(t, prev_t - 1e-5f);
    prev_t = t;
  }
}

/* Points on the two branches of a U-shaped curve sit at very different arc-length positions even
 * though they are close in space -- the whole point of the Curve mode over Radial. */
TEST(sculpt_paint_gradient_curve, u_shape_branches_differ)
{
  /* U with arms down: (0,0) -> (0,4) -> (4,4) -> (4,0), densified so the spline resolution is 1
   * and the corners stay sharp. */
  const Vector<float2> corners = {float2(0, 0), float2(0, 4), float2(4, 4), float2(4, 0)};
  const Vector<float2> raw = densify(corners, 0.2f);
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build(raw, {0.2f, 0.0f, 0}));

  EXPECT_NEAR(curve.length(), 12.0f, 0.2f);

  /* Mid-arm points: near the start of the curve vs near its end, both 0.5 from the curve. */
  const float t_start_branch = curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(-0.5f, 1));
  const float t_end_branch = curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(4.5f, 1));
  EXPECT_NEAR(t_start_branch, 1.0f / 12.0f, 0.03f);
  EXPECT_NEAR(t_end_branch, 11.0f / 12.0f, 0.03f);

  /* A Radial-style nearest-endpoint interpretation would give both the same t. */
  EXPECT_GT(std::abs(t_end_branch - t_start_branch), 0.5f);
}

/* Across mode: t is the distance ratio, unbounded past the width (the caller clamps / repeats
 * it), exactly like Radial's `distance / radius`. */
TEST(sculpt_paint_gradient_curve, across_strip)
{
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build({float2(0, 0), float2(10, 0)}, {1.0f, 0.0f, 0}));

  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 4.0f, float2(5, 0)), 0.0f, 1e-6f);
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 4.0f, float2(5, 1)), 0.25f, 1e-5f);
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 4.0f, float2(5, -4)), 1.0f, 1e-5f);
  /* Past the width the parameter keeps growing instead of becoming NaN, so the ramp's end color
   * (Repeat None) is painted out to the canvas edge. */
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 4.0f, float2(5, 4.1f)), 1.025f, 1e-5f);
  /* Zero width means automatic for Across: a quarter of the curve's length (10 -> 2.5). */
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 0.0f, float2(5, 1)), 0.4f, 1e-5f);
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 0.0f, float2(5, 3)), 1.2f, 1e-5f);
}

/* Along mode with a width limit: NaN beyond the limit, valid inside; 0 disables the limit. */
TEST(sculpt_paint_gradient_curve, along_width_limit)
{
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build({float2(0, 0), float2(10, 0)}, {1.0f, 0.0f, 0}));

  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 2.0f, float2(5, 1)), 0.5f, 1e-5f);
  EXPECT_TRUE(std::isnan(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 2.0f, float2(5, 2.1f))));
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(5, 100)), 0.5f, 1e-5f);
}

/* B1: Across must not follow the extrapolated end tangent; beyond the ends the true distance to
 * the endpoint grows, so the parameter grows with it instead of shooting off to infinity. */
TEST(sculpt_paint_gradient_curve, across_does_not_extend_past_ends)
{
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build({float2(0, 0), float2(10, 0)}, {1.0f, 0.0f, 0}));

  /* Ten units past the end and half a unit off-axis: the true distance to the endpoint is
   * sqrt(10^2 + 0.5^2) ~ 10.012, not the old tangent extrapolation's 0.5. */
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 4.0f, float2(20, 0.5f)),
              std::sqrt(100.25f) / 4.0f,
              1e-4f);
  /* Just past the end on the axis: distance 1 -> t = 1 / 4. */
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 4.0f, float2(11, 0)), 0.25f, 1e-4f);
  EXPECT_NEAR(curve.project(float2(11, 0)).distance, 1.0f, 1e-4f);
}

/* B1: Along with a width uses the true distance too; without a width it extrapolates. */
TEST(sculpt_paint_gradient_curve, along_extrapolates_but_distance_is_true)
{
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build({float2(0, 0), float2(10, 0)}, {1.0f, 0.0f, 0}));

  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(15, 0)), 1.5f, 1e-4f);
  EXPECT_NEAR(curve.project(float2(15, 0)).distance, 5.0f, 1e-4f);
  EXPECT_TRUE(std::isnan(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 4.0f, float2(15, 0))));
}

/* B2: a point near an inner corner must use the interior projection, not the continuation of the
 * first segment which would report a shorter, wrong distance. */
TEST(sculpt_paint_gradient_curve, inner_corner_uses_true_segment)
{
  /* L: (0,0) -> (4,0) -> (4,4), densified so the corner is preserved. */
  const Vector<float2> corners = {float2(0, 0), float2(4, 0), float2(4, 4)};
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build(densify(corners, 0.2f), {0.2f, 0.0f, 0}));

  const CurveProjection proj = curve.project(float2(3, 1));
  EXPECT_NEAR(proj.distance, 1.0f, 1e-4f);
  EXPECT_GE(proj.s, 3.0f);
  EXPECT_LE(proj.s, 5.0f);
}

/* Resampling keeps the endpoints and an even step. The input is a smooth parabola so the chord
 * length between consecutive resampled points matches their arc-length step closely enough for
 * the evenness assertion (on a polyline with sharp corners the chord would legitimately fall
 * short of the step at every corner it spans). */
TEST(sculpt_paint_gradient_curve, resample_even_spacing_preserves_ends)
{
  Array<float2> raw(17);
  for (const int i : raw.index_range()) {
    const float x = float(i) * 0.25f;
    raw[i] = float2(x, x * x * 0.25f);
  }
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build(raw, {0.25f, 0.0f, 0}));

  EXPECT_V2_NEAR(curve.points().first(), float2(0, 0), 1e-5f);
  EXPECT_V2_NEAR(curve.points().last(), float2(4, 4), 1e-5f);

  /* Even spacing: consecutive resampled points are equally far apart. */
  Span<float2> pts = curve.points();
  ASSERT_GT(pts.size(), 8);
  float prev_len = math::distance(pts[0], pts[1]);
  for (const int i : IndexRange(1, pts.size() - 2)) {
    const float len = math::distance(pts[i], pts[i + 1]);
    EXPECT_NEAR(len, prev_len, 0.01f * prev_len + 1e-4f) << "segment " << i;
    prev_len = len;
  }
}

/* Smoothing pins the endpoints and pulls interior points toward their neighbors, flattening a
 * jagged input. */
TEST(sculpt_paint_gradient_curve, smooth_preserves_ends_and_flattens)
{
  const Vector<float2> raw = {
      float2(0, 0), float2(1, 0.5f), float2(2, -0.5f), float2(3, 0.5f), float2(4, 0)};
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build(raw, {0.25f, 0.5f, 4}));

  EXPECT_V2_NEAR(curve.points().first(), float2(0, 0), 1e-5f);
  EXPECT_V2_NEAR(curve.points().last(), float2(4, 0), 1e-5f);

  /* The zig-zag (raw amplitude 0.5 around the baseline) is flattened: the middle of the curve
   * sits clearly closer to the baseline than the input's nearest point at (2, -0.5). */
  const CurveProjection middle = curve.project(float2(2.0f, 0.0f));
  EXPECT_LT(middle.distance, 0.45f);
}

/* Degenerate input never produces a valid curve and never crashes. */
TEST(sculpt_paint_gradient_curve, degenerate_inputs)
{
  GradientCurve2D curve;

  EXPECT_FALSE(curve.build({}, {1.0f, 0.5f, 4}));
  EXPECT_FALSE(curve.build({float2(1, 1)}, {1.0f, 0.5f, 4}));
  EXPECT_FALSE(curve.build({float2(1, 1), float2(1, 1), float2(1, 1)}, {1.0f, 0.5f, 4}));
  EXPECT_FALSE(curve.is_valid());

  /* Evaluation on an invalid curve is NaN ("leave untouched"), never a crash. */
  EXPECT_TRUE(std::isnan(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float2(0, 0))));
  EXPECT_TRUE(std::isnan(curve.eval_t(GRADIENT_CURVE_MODE_ACROSS, 1.0f, float2(0, 0))));
}

/* Non-finite input points are rejected instead of poisoning the polyline. */
TEST(sculpt_paint_gradient_curve, non_finite_inputs)
{
  GradientCurve2D curve;
  EXPECT_FALSE(curve.build({float2(0, 0), float2(INF, 0), float2(2, 0)}, {0.5f, 0.0f, 0}));
  EXPECT_FALSE(curve.build({float2(0, 0), float2(NaN, 1)}, {0.5f, 0.0f, 0}));
}

/* The same math in 3D: straight world-space line, projection and Along parameter. */
TEST(sculpt_paint_gradient_curve, three_d_line)
{
  GradientCurve3D curve;
  ASSERT_TRUE(curve.build({float3(0, 0, 0), float3(0, 0, 8)}, {1.0f, 0.0f, 0}));

  EXPECT_NEAR(curve.length(), 8.0f, 1e-4f);
  const CurveProjection proj = curve.project(float3(1, 2, 5));
  EXPECT_NEAR(proj.s, 5.0f, 1e-4f);
  EXPECT_NEAR(proj.distance, std::sqrt(5.0f), 1e-5f);
  EXPECT_NEAR(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 0.0f, float3(1, 2, 5)), 0.625f, 1e-4f);
  EXPECT_TRUE(std::isnan(curve.eval_t(GRADIENT_CURVE_MODE_ALONG, 1.0f, float3(1, 2, 5))));
}

/* Position lookup along the arc length: endpoints and the middle. */
TEST(sculpt_paint_gradient_curve, position_at_s)
{
  GradientCurve2D curve;
  ASSERT_TRUE(
      curve.build(densify({float2(0, 0), float2(6, 0), float2(6, 4)}, 0.2f), {0.2f, 0.0f, 0}));

  EXPECT_V2_NEAR(curve.position_at_s(0.0f), float2(0, 0), 1e-5f);
  EXPECT_V2_NEAR(curve.position_at_s(8.0f), float2(6, 2), 1e-4f);
  EXPECT_V2_NEAR(curve.position_at_s(10.0f), float2(6, 4), 1e-5f);
  /* Out-of-range positions clamp to the ends. */
  EXPECT_V2_NEAR(curve.position_at_s(-1.0f), float2(0, 0), 1e-5f);
  EXPECT_V2_NEAR(curve.position_at_s(100.0f), float2(6, 4), 1e-5f);
}

/* RDP on a straight run of collinear points keeps only the two ends. */
TEST(sculpt_paint_gradient_curve, simplify_straight_line_keeps_ends)
{
  Array<float2> raw(100);
  for (const int i : raw.index_range()) {
    raw[i] = float2(float(i), 0.0f);
  }
  const Vector<int> kept = simplify_control_points<float2>(raw, 0.1f, 32);
  ASSERT_EQ(kept.size(), 2);
  EXPECT_EQ(kept[0], 0);
  EXPECT_EQ(kept[1], 99);
}

/* RDP keeps the sharp corner of an L-shaped stroke. */
TEST(sculpt_paint_gradient_curve, simplify_keeps_corner)
{
  Array<float2> raw(100);
  for (const int i : IndexRange(50)) {
    raw[i] = float2(float(i), 0.0f);
  }
  for (const int i : IndexRange(50)) {
    raw[50 + i] = float2(50.0f, float(i) * 2.0f);
  }
  /* The corner is at index 49 (last point of the horizontal leg). */
  const Vector<int> kept = simplify_control_points<float2>(raw, 0.2f, 32);
  ASSERT_GE(kept.size(), 3);
  EXPECT_EQ(kept.first(), 0);
  EXPECT_EQ(kept.last(), 99);
  bool has_corner = false;
  for (const int index : kept) {
    if (index >= 48 && index <= 50) {
      has_corner = true;
    }
  }
  EXPECT_TRUE(has_corner);
}

/* The point cap is enforced even on a wiggly stroke; the ends survive. */
TEST(sculpt_paint_gradient_curve, simplify_respects_max_points)
{
  Array<float2> raw(1000);
  for (const int i : raw.index_range()) {
    raw[i] = float2(float(i) * 0.1f, std::sin(float(i) * 0.3f));
  }
  const Vector<int> kept = simplify_control_points<float2>(raw, 0.001f, 16);
  EXPECT_LE(kept.size(), 16);
  EXPECT_EQ(kept.first(), 0);
  EXPECT_EQ(kept.last(), 999);
}

/* A closed loop whose chord is degenerate keeps its farthest point. */
TEST(sculpt_paint_gradient_curve, simplify_closed_loop)
{
  const int steps = 64;
  Array<float2> raw(steps + 1);
  for (const int i : raw.index_range()) {
    const float angle = float(i) / float(steps) * 2.0f * float(M_PI);
    raw[i] = float2(std::cos(angle), std::sin(angle));
  }
  const Vector<int> kept = simplify_control_points<float2>(raw, 0.05f, 32);
  ASSERT_GE(kept.size(), 3);
  EXPECT_EQ(kept.first(), 0);
  EXPECT_EQ(kept.last(), steps);
  /* At least one interior point far from the (degenerate) chord is kept. */
  EXPECT_GT(kept[1], 0);
}

/* Catmull-Rom interpolates its control points: the built curve passes through each of them. */
TEST(sculpt_paint_gradient_curve, catmull_rom_passes_through_controls)
{
  const Vector<float2> controls = {
      float2(0, 0), float2(2, 3), float2(5, -1), float2(8, 2), float2(10, 0)};
  GradientCurve2D curve;
  ASSERT_TRUE(curve.build(controls, {0.1f, 0.0f, 0}));

  for (const float2 &control : controls) {
    EXPECT_LT(curve.project(control).distance, 5e-3f) << "control " << control;
  }
}

}  // namespace blender::ed::sculpt_paint::gradient_curve::tests
