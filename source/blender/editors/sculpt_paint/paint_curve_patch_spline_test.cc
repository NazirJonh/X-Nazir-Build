/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "DNA_texture_types.h"

#include "paint_curve_patch_spline.hh"

namespace blender::ed::sculpt_paint::tests {

TEST(paint_curve_patch_spline, empty_when_fewer_than_two_points)
{
  CurvePatchSpline spline;
  const float3 one_point[1] = {float3(0.0f)};
  spline.build_from_positions(Span(one_point, 1));
  EXPECT_TRUE(spline.is_empty());
  EXPECT_EQ(spline.total_length(), 0.0f);
}

TEST(paint_curve_patch_spline, straight_line_total_length_and_evaluate)
{
  CurvePatchSpline spline;
  const float3 points[3] = {
      float3(0.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), float3(2.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 3));

  EXPECT_FALSE(spline.is_empty());
  EXPECT_NEAR(spline.total_length(), 2.0f, 1e-5f);

  const float3 mid = spline.evaluate(1.0f);
  EXPECT_NEAR(mid.x, 1.0f, 1e-5f);
  EXPECT_NEAR(mid.y, 0.0f, 1e-5f);

  /* Out-of-range `s` clamps instead of extrapolating. */
  const float3 past_end = spline.evaluate(100.0f);
  EXPECT_NEAR(past_end.x, 2.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, tangent_points_along_positive_x)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));

  const float3 tan = spline.tangent_at(0.5f);
  EXPECT_NEAR(tan.x, 1.0f, 1e-5f);
  EXPECT_NEAR(tan.y, 0.0f, 1e-5f);
  EXPECT_NEAR(tan.z, 0.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, closest_point_reports_correct_arc_length_and_side)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(2.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  /* Query 1 unit off to the +Y side, above the midpoint. */
  float s, lateral, normal_dist;
  float3 tangent;
  spline.closest_point(float3(1.0f, 1.0f, 0.0f), s, tangent, lateral, &normal_dist);

  EXPECT_NEAR(s, 1.0f, 1e-5f);
  EXPECT_NEAR(tangent.x, 1.0f, 1e-5f);
  /* side_axis = normalize(cross((0,0,1), (1,0,0))) = (0,1,0), so lateral should be +1. */
  EXPECT_NEAR(lateral, 1.0f, 1e-5f);
  /* Query is in the z=0 plane, so normal_dist (along plane_normal=(0,0,1)) is 0. */
  EXPECT_NEAR(normal_dist, 0.0f, 1e-5f);

  /* Query on the opposite side reports the opposite sign. */
  float s2, lateral2, normal_dist2;
  float3 tangent2;
  spline.closest_point(float3(1.0f, -1.0f, 0.0f), s2, tangent2, lateral2, &normal_dist2);
  EXPECT_NEAR(lateral2, -1.0f, 1e-5f);
  EXPECT_NEAR(normal_dist2, 0.0f, 1e-5f);

  /* Query lifted off the plane: lateral stays in-plane (+1 along Y), normal_dist captures the
   * out-of-plane offset along plane_normal (z=+2). This is the decomposition the relief action
   * uses to reject vertices on perpendicular faces. */
  float s3, lateral3, normal_dist3;
  float3 tangent3;
  spline.closest_point(float3(1.0f, 1.0f, 2.0f), s3, tangent3, lateral3, &normal_dist3);
  EXPECT_NEAR(lateral3, 1.0f, 1e-5f);
  EXPECT_NEAR(normal_dist3, 2.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, closest_point_extends_arc_length_past_curve_ends)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(2.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);

  /* Query 1 unit past the far end, exactly on the curve's own line -- `lateral`/`normal_dist` are
   * both ~0, so only `r_s` reaching past `total_length()` can reveal this is outside the curve. */
  float s, lateral, normal_dist;
  float3 tangent;
  spline.closest_point(float3(3.0f, 0.0f, 0.0f), s, tangent, lateral, &normal_dist);
  EXPECT_NEAR(s, 3.0f, 1e-5f);
  EXPECT_NEAR(lateral, 0.0f, 1e-5f);
  EXPECT_NEAR(normal_dist, 0.0f, 1e-5f);

  /* Same, 1 unit before the near end. */
  float s2, lateral2, normal_dist2;
  float3 tangent2;
  spline.closest_point(float3(-1.0f, 0.0f, 0.0f), s2, tangent2, lateral2, &normal_dist2);
  EXPECT_NEAR(s2, -1.0f, 1e-5f);
  EXPECT_NEAR(lateral2, 0.0f, 1e-5f);
  EXPECT_NEAR(normal_dist2, 0.0f, 1e-5f);

  /* A query still inside the curve keeps reporting the ordinary clamped-range arc-length. */
  float s3, lateral3, normal_dist3;
  float3 tangent3;
  spline.closest_point(float3(1.0f, 0.0f, 0.0f), s3, tangent3, lateral3, &normal_dist3);
  EXPECT_NEAR(s3, 1.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, radius_at_interpolates_linearly)
{
  CurvePatchSpline spline;
  const float3 points[3] = {
      float3(0.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), float3(2.0f, 0.0f, 0.0f)};
  const float radii[3] = {1.0f, 2.0f, 4.0f};
  spline.build_from_positions(Span(points, 3), Span(radii, 3));

  EXPECT_NEAR(spline.radius_at(0.0f), 1.0f, 1e-5f);
  EXPECT_NEAR(spline.radius_at(0.5f), 1.5f, 1e-5f);
  EXPECT_NEAR(spline.radius_at(1.0f), 2.0f, 1e-5f);
  EXPECT_NEAR(spline.radius_at(1.5f), 3.0f, 1e-5f);
  EXPECT_NEAR(spline.radius_at(2.0f), 4.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, radius_at_clamps_out_of_range_s)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f)};
  const float radii[2] = {1.0f, 3.0f};
  spline.build_from_positions(Span(points, 2), Span(radii, 2));

  EXPECT_NEAR(spline.radius_at(-5.0f), 1.0f, 1e-5f);
  EXPECT_NEAR(spline.radius_at(100.0f), 3.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, tile_span_default_is_hybrid_min)
{
  /* Short curve (shorter than one diameter): span == total_length. */
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_DEFAULT, 1, 1.0f, 2.0f), 1.0f);
  /* Long curve (longer than one diameter): span == 2 * radius. */
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_DEFAULT, 1, 10.0f, 2.0f), 4.0f);
}

TEST(paint_curve_patch_spline, tile_span_repeat_divides_length)
{
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_REPEAT, 1, 10.0f, 2.0f), 10.0f);
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_REPEAT, 4, 10.0f, 2.0f), 2.5f);
}

TEST(paint_curve_patch_spline, tile_span_repeat_clamps_below_one)
{
  /* A repeat count that somehow bypassed RNA's 1..64 range must not divide by zero. */
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_REPEAT, 0, 10.0f, 2.0f), 10.0f);
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_REPEAT, -5, 10.0f, 2.0f), 10.0f);
}

TEST(paint_curve_patch_spline, tile_span_stretch_is_total_length)
{
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_STRETCH, 1, 10.0f, 2.0f), 10.0f);
  EXPECT_FLOAT_EQ(
      curve_patch_texture_tile_span(MTEX_CURVE_PATCH_LENGTH_STRETCH, 1, 0.5f, 2.0f), 0.5f);
}

TEST(paint_curve_patch_spline, tile_span_unknown_mode_falls_back_to_default)
{
  EXPECT_FLOAT_EQ(curve_patch_texture_tile_span(99, 1, 10.0f, 2.0f), 4.0f);
}

TEST(paint_curve_patch_spline, cyclic_build_closes_the_loop)
{
  /* The four corners of a unit square, as a cyclic curve's evaluated points would arrive: the
   * first corner is NOT repeated at the end. */
  const float3 corners[4] = {float3(0.0f, 0.0f, 0.0f),
                             float3(1.0f, 0.0f, 0.0f),
                             float3(1.0f, 1.0f, 0.0f),
                             float3(0.0f, 1.0f, 0.0f)};

  CurvePatchSpline open_spline;
  open_spline.build_from_positions(Span(corners, 4));
  EXPECT_FALSE(open_spline.cyclic);
  /* Three sides only -- the closing one is not part of an open curve. */
  EXPECT_NEAR(open_spline.total_length(), 3.0f, 1e-5f);

  CurvePatchSpline closed_spline;
  closed_spline.build_from_positions(Span(corners, 4), {}, /*cyclic=*/true);
  EXPECT_TRUE(closed_spline.cyclic);
  EXPECT_EQ(closed_spline.poly_3d.size(), 5);
  EXPECT_EQ(closed_spline.poly_3d.last(), closed_spline.poly_3d.first());
  /* The full perimeter, closing side included. */
  EXPECT_NEAR(closed_spline.total_length(), 4.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, cyclic_build_makes_join_tangent_continuous)
{
  const float3 corners[4] = {float3(0.0f, 0.0f, 0.0f),
                             float3(1.0f, 0.0f, 0.0f),
                             float3(1.0f, 1.0f, 0.0f),
                             float3(0.0f, 1.0f, 0.0f)};
  CurvePatchSpline spline;
  spline.build_from_positions(Span(corners, 4), {}, /*cyclic=*/true);

  /* The join's two coincident vertices must report the SAME tangent, otherwise the ribbon's UV
   * creases exactly where the pattern has to meet itself. */
  const float3 first = spline.tangents_3d.first();
  const float3 last = spline.tangents_3d.last();
  EXPECT_NEAR(first.x, last.x, 1e-5f);
  EXPECT_NEAR(first.y, last.y, 1e-5f);
  EXPECT_NEAR(first.z, last.z, 1e-5f);
}

TEST(paint_curve_patch_spline, tile_span_cyclic_snaps_to_whole_tiles)
{
  /* Default mode on a length of 10 with radius 2 wants a 4.0 tile -- 2.5 tiles around the loop,
   * which would cut the last one mid-pattern. Snapped to 3 whole tiles. */
  EXPECT_FLOAT_EQ(curve_patch_texture_tile_span(
                      MTEX_CURVE_PATCH_LENGTH_DEFAULT, 1, 10.0f, 2.0f, /*cyclic=*/true),
                  10.0f / 3.0f);
  /* Repeat and Stretch already divide the length into whole tiles and must come out unchanged. */
  EXPECT_FLOAT_EQ(curve_patch_texture_tile_span(
                      MTEX_CURVE_PATCH_LENGTH_REPEAT, 4, 10.0f, 2.0f, /*cyclic=*/true),
                  2.5f);
  EXPECT_FLOAT_EQ(curve_patch_texture_tile_span(
                      MTEX_CURVE_PATCH_LENGTH_STRETCH, 1, 10.0f, 2.0f, /*cyclic=*/true),
                  10.0f);
}

}  // namespace blender::ed::sculpt_paint::tests
