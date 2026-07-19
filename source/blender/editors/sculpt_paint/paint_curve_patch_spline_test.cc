/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>

#include "DNA_texture_types.h"

#include "BLI_math_base.h"

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

/* A 10-unit straight line, radius 1, spacing 0.5 (i.e. 1.0 world units between stamps) and no
 * randomization: stamps land on an exact 1-unit grid. */
TEST(paint_curve_patch_stamps, spacing_controls_count_and_positions)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(10.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));

  Vector<CurvePatchStamp> stamps;
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1u, stamps);

  EXPECT_EQ(stamps.size(), 11);
  EXPECT_NEAR(stamps[0].center_v, 0.0f, 1e-5f);
  EXPECT_NEAR(stamps[1].center_v, 1.0f, 1e-5f);
  EXPECT_NEAR(stamps.last().center_v, 10.0f, 1e-5f);
  for (const CurvePatchStamp &stamp : stamps) {
    EXPECT_NEAR(stamp.center_u, 0.0f, 1e-6f);
    EXPECT_NEAR(stamp.half_extent, 1.0f, 1e-6f);
    EXPECT_NEAR(stamp.strength, 1.0f, 1e-6f);
  }
}

/* The same seed must reproduce the same layout -- the relief is recomputed from scratch on every
 * interactive event, so any drift would make the patch flicker. */
TEST(paint_curve_patch_stamps, same_seed_is_reproducible_different_seed_is_not)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(10.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));

  Vector<CurvePatchStamp> a, b, c;
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 7u, a);
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 7u, b);
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 8u, c);

  ASSERT_EQ(a.size(), b.size());
  ASSERT_EQ(a.size(), c.size());
  bool c_differs = false;
  for (const int i : a.index_range()) {
    EXPECT_FLOAT_EQ(a[i].center_v, b[i].center_v);
    EXPECT_FLOAT_EQ(a[i].center_u, b[i].center_u);
    EXPECT_FLOAT_EQ(a[i].half_extent, b[i].half_extent);
    EXPECT_FLOAT_EQ(a[i].angle, b[i].angle);
    EXPECT_FLOAT_EQ(a[i].strength, b[i].strength);
    c_differs |= (a[i].center_u != c[i].center_u);
  }
  EXPECT_TRUE(c_differs);
}

/* Randomization only ever reduces: a stamp never exceeds the brush radius or full strength, and
 * never drops to zero or below. Jitter stays inside the requested amount. */
TEST(paint_curve_patch_stamps, randomization_stays_in_range)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(20.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));

  for (const uint32_t seed : {1u, 42u, 12345u}) {
    Vector<CurvePatchStamp> stamps;
    curve_patch_stamps_build(spline, 2.0f, 0.25f, 0.75f, 1.0f, 1.0f, 0.0f, float(M_PI), seed, stamps);
    ASSERT_FALSE(stamps.is_empty());
    for (const CurvePatchStamp &stamp : stamps) {
      EXPECT_GT(stamp.half_extent, 0.0f);
      EXPECT_LE(stamp.half_extent, 2.0f + 1e-6f);
      EXPECT_GT(stamp.strength, 0.0f);
      EXPECT_LE(stamp.strength, 1.0f + 1e-6f);
      EXPECT_LE(std::abs(stamp.center_u), 0.75f + 1e-6f);
    }
  }
}

/* Stamps must come out sorted by arc length even after jitter displaces them, because the relief
 * binary-searches this list by `center_v`. */
TEST(paint_curve_patch_stamps, sorted_by_arc_length_after_jitter)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f, 0.0f, 0.0f), float3(20.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));

  Vector<CurvePatchStamp> stamps;
  curve_patch_stamps_build(spline, 2.0f, 0.25f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3u, stamps);
  for (const int i : IndexRange(1, stamps.size() - 1)) {
    EXPECT_GE(stamps[i].center_v, stamps[i - 1].center_v);
  }
}

/* A closed loop gets a whole number of stamps so the seam does not carry two overlapping stamps,
 * mirroring what `curve_patch_texture_tile_span()` does for cyclic tiling. */
TEST(paint_curve_patch_stamps, cyclic_holds_a_whole_number_of_stamps)
{
  CurvePatchSpline spline;
  /* Square loop, perimeter 12: `build_from_positions` appends the closing edge itself. */
  const float3 points[4] = {float3(0.0f, 0.0f, 0.0f),
                            float3(3.0f, 0.0f, 0.0f),
                            float3(3.0f, 3.0f, 0.0f),
                            float3(0.0f, 3.0f, 0.0f)};
  spline.build_from_positions(Span(points, 4), {}, true);
  EXPECT_NEAR(spline.total_length(), 12.0f, 1e-5f);

  Vector<CurvePatchStamp> stamps;
  /* Requested step 2.5 does not divide 12; it is snapped to 2.4 (5 stamps) so the last stamp does
   * not land on top of the first. */
  curve_patch_stamps_build(spline, 1.0f, 1.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1u, stamps);
  EXPECT_EQ(stamps.size(), 5);
  EXPECT_NEAR(stamps[0].center_v, 0.0f, 1e-5f);
  EXPECT_NEAR(stamps.last().center_v, 9.6f, 1e-5f);
}

/* The stamp sitting at a loop's join reaches into `v < 0`, which the ribbon LUT -- spanning
 * `[0, L]` exactly on a cyclic curve -- cannot represent. A ghost copy at `+L` supplies that half
 * from the far side of the join. */
TEST(paint_curve_patch_stamps, cyclic_wrap_ghosts_a_stamp_near_the_start)
{
  Vector<CurvePatchStamp> stamps;
  CurvePatchStamp stamp;
  stamp.center_v = 0.1f;
  stamp.center_u = 0.3f;
  stamp.half_extent = 0.9f;
  stamp.angle = 0.7f;
  stamp.strength = 0.4f;
  stamps.append(stamp);

  curve_patch_stamps_add_cyclic_wrap(stamps, 12.0f, 1.0f);

  ASSERT_EQ(stamps.size(), 2);
  EXPECT_NEAR(stamps[0].center_v, 0.1f, 1e-5f);
  EXPECT_NEAR(stamps[1].center_v, 12.1f, 1e-5f);
  /* The ghost is the same stamp seen from the other side of the join, so everything but its arc
   * length must be identical -- that is what makes the texture meet itself. */
  EXPECT_FLOAT_EQ(stamps[1].center_u, 0.3f);
  EXPECT_FLOAT_EQ(stamps[1].half_extent, 0.9f);
  EXPECT_FLOAT_EQ(stamps[1].angle, 0.7f);
  EXPECT_FLOAT_EQ(stamps[1].strength, 0.4f);
}

/* The mirror case: a stamp just before the join needs its ghost at `-L`, since a vertex at a small
 * `s` searches a window that opens below zero. */
TEST(paint_curve_patch_stamps, cyclic_wrap_ghosts_a_stamp_near_the_end)
{
  Vector<CurvePatchStamp> stamps;
  CurvePatchStamp stamp;
  stamp.center_v = 11.7f;
  stamp.center_u = -0.2f;
  stamp.half_extent = 1.0f;
  stamp.angle = -0.5f;
  stamp.strength = 0.8f;
  stamps.append(stamp);

  curve_patch_stamps_add_cyclic_wrap(stamps, 12.0f, 1.0f);

  ASSERT_EQ(stamps.size(), 2);
  EXPECT_NEAR(stamps[0].center_v, -0.3f, 1e-5f);
  EXPECT_NEAR(stamps[1].center_v, 11.7f, 1e-5f);
  EXPECT_FLOAT_EQ(stamps[0].center_u, -0.2f);
  EXPECT_FLOAT_EQ(stamps[0].half_extent, 1.0f);
  EXPECT_FLOAT_EQ(stamps[0].angle, -0.5f);
  EXPECT_FLOAT_EQ(stamps[0].strength, 0.8f);
}

/* A stamp far from the join is already fully covered by the LUT and must not be duplicated --
 * a ghost there would be a second stamp in the middle of the loop. */
TEST(paint_curve_patch_stamps, cyclic_wrap_leaves_interior_stamps_alone)
{
  Vector<CurvePatchStamp> stamps;
  CurvePatchStamp stamp;
  stamp.center_v = 6.0f;
  stamps.append(stamp);

  curve_patch_stamps_add_cyclic_wrap(stamps, 12.0f, 1.0f);

  ASSERT_EQ(stamps.size(), 1);
  EXPECT_NEAR(stamps[0].center_v, 6.0f, 1e-5f);
}

/* The relief binary-searches the list by `center_v`, so the ghosts -- which land outside `[0, L]`
 * on both sides -- must leave it sorted. */
TEST(paint_curve_patch_stamps, cyclic_wrap_keeps_the_list_sorted)
{
  CurvePatchSpline spline;
  const float3 points[4] = {float3(0.0f, 0.0f, 0.0f),
                            float3(3.0f, 0.0f, 0.0f),
                            float3(3.0f, 3.0f, 0.0f),
                            float3(0.0f, 3.0f, 0.0f)};
  spline.build_from_positions(Span(points, 4), {}, true);

  Vector<CurvePatchStamp> stamps;
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.3f, 0.5f, 0.5f, 0.0f, 1.0f, 11u, stamps);
  const int real_num = stamps.size();
  ASSERT_GT(real_num, 2);

  curve_patch_stamps_add_cyclic_wrap(stamps, spline.total_length(), float(M_SQRT2));

  EXPECT_GT(stamps.size(), real_num);
  for (const int i : IndexRange(1, stamps.size() - 1)) {
    EXPECT_GE(stamps[i].center_v, stamps[i - 1].center_v);
  }
}

}  // namespace blender::ed::sculpt_paint::tests
