/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <cmath>

#include "DNA_texture_types.h"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_span.hh"

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
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1u, {}, stamps);

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
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 7u, {}, a);
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 7u, {}, b);
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f, 8u, {}, c);

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
    curve_patch_stamps_build(
        spline, 2.0f, 0.25f, 0.75f, 1.0f, 1.0f, 0.0f, float(M_PI), seed, {}, stamps);
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
  curve_patch_stamps_build(spline, 2.0f, 0.25f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3u, {}, stamps);
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
  curve_patch_stamps_build(spline, 1.0f, 1.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1u, {}, stamps);
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
  curve_patch_stamps_build(spline, 1.0f, 0.5f, 0.3f, 0.5f, 0.5f, 0.0f, 1.0f, 11u, {}, stamps);
  const int real_num = stamps.size();
  ASSERT_GT(real_num, 2);

  curve_patch_stamps_add_cyclic_wrap(stamps, spline.total_length(), float(M_SQRT2));

  EXPECT_GT(stamps.size(), real_num);
  for (const int i : IndexRange(1, stamps.size() - 1)) {
    EXPECT_GE(stamps[i].center_v, stamps[i - 1].center_v);
  }
}

TEST(paint_curve_patch_spline, stamp_pick_texture_empty_table)
{
  EXPECT_EQ(curve_patch_stamp_pick_texture({}, 0.0f), -1);
  EXPECT_EQ(curve_patch_stamp_pick_texture({}, 0.5f), -1);
}

TEST(paint_curve_patch_spline, stamp_pick_texture_zero_total_falls_back)
{
  /* Every slot disabled by a zero weight must degrade to the brush's own texture (-1), not to an
   * arbitrary slot -- otherwise zeroing every weight would silently keep stamping. */
  const float cdf[3] = {0.0f, 0.0f, 0.0f};
  EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 3), 0.0f), -1);
  EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 3), 0.99f), -1);
}

TEST(paint_curve_patch_spline, stamp_pick_texture_respects_weights)
{
  /* Weights 3:1 -- the cumulative table is {3, 4}. Sampled with a DETERMINISTIC sweep rather than a
   * generator so the assertion cannot flake. */
  const float cdf[2] = {3.0f, 4.0f};
  int first = 0;
  for (const int i : IndexRange(1000)) {
    const float random01 = float(i) / 1000.0f;
    const int index = curve_patch_stamp_pick_texture(Span(cdf, 2), random01);
    EXPECT_GE(index, 0);
    EXPECT_LT(index, 2);
    if (index == 0) {
      first++;
    }
  }
  EXPECT_NEAR(float(first) / 1000.0f, 0.75f, 0.01f);
}

TEST(paint_curve_patch_spline, stamp_pick_texture_clamps_at_both_ends)
{
  const float cdf[2] = {1.0f, 2.0f};
  EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 2), 0.0f), 0);
  /* A `random01` that rounds up to the total must not run past the last slot. */
  EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 2), 1.0f), 1);
  EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 2), 1.5f), 1);
  /* A `random01` below zero must clamp at the LOW end too, landing in the first slot instead of
   * going negative through the table. */
  EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 2), -0.5f), 0);
}

TEST(paint_curve_patch_spline, stamp_pick_texture_single_entry_table_always_zero)
{
  /* A one-slot list is the shape the real caller builds for a single enabled texture. It is also
   * the boundary case where the loop returns on its very first iteration. */
  const float cdf[1] = {1.0f};
  for (const float random01 : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    EXPECT_EQ(curve_patch_stamp_pick_texture(Span(cdf, 1), random01), 0);
  }
}

TEST(paint_curve_patch_spline, stamp_pick_texture_never_picks_zero_weight_slot)
{
  /* Slot 1 has zero width in the table (cumulative 1 -> 1), so no `random01` may land on it. */
  const float cdf[3] = {1.0f, 1.0f, 2.0f};
  for (const int i : IndexRange(1000)) {
    const int index = curve_patch_stamp_pick_texture(Span(cdf, 3), float(i) / 1000.0f);
    EXPECT_NE(index, 1);
  }
}

/* Reference implementation of the pre-caps `v` formula, copied verbatim from the Ribbon branch of
 * `paint_curve_patch_cache.cc` as it stood before this feature. `curve_patch_texture_zone_at()` with
 * `caps_enabled == false` must reproduce it exactly, or the caps feature has silently changed every
 * existing Curve Patch. */
static float reference_ribbon_v(const float s,
                                const float total_length,
                                const float radius,
                                const int length_mode,
                                const int length_repeat,
                                const bool cyclic)
{
  const float tile_span = curve_patch_texture_tile_span(
      length_mode, length_repeat, total_length, radius, cyclic);
  float v = tile_span > 1e-8f ?
                (cyclic ? s / tile_span * 2.0f - 1.0f : (s - total_length * 0.5f) / tile_span * 2.0f) :
                0.0f;
  if (length_mode == MTEX_CURVE_PATCH_LENGTH_REPEAT) {
    v -= 2.0f * std::floor((v + 1.0f) * 0.5f);
  }
  return v;
}

TEST(paint_curve_patch_spline, texture_zone_caps_disabled_matches_reference)
{
  const int modes[3] = {MTEX_CURVE_PATCH_LENGTH_DEFAULT,
                        MTEX_CURVE_PATCH_LENGTH_REPEAT,
                        MTEX_CURVE_PATCH_LENGTH_STRETCH};
  const float total_length = 7.5f;
  const float radius = 0.8f;
  for (const int mode : modes) {
    for (const bool cyclic : {false, true}) {
      for (const int i : IndexRange(50)) {
        const float s = total_length * float(i) / 49.0f;
        const CurvePatchTextureZoneSample sample = curve_patch_texture_zone_at(
            /*s=*/s,
            /*total_length=*/total_length,
            /*radius_for_middle_tile=*/radius,
            /*caps_enabled=*/false,
            /*cap_start_length=*/2.0f,
            /*cap_end_length=*/2.0f,
            /*length_mode=*/mode,
            /*length_repeat=*/3,
            /*cyclic=*/cyclic);
        EXPECT_EQ(sample.zone, CurvePatchTextureZone::Middle);
        EXPECT_TRUE(sample.valid);
        /* NOTE: `1e-5f`, not bit-exact equality -- `sample.v` and `reference_ribbon_v()` are the
         * same formula evaluated in two different translation units, and cross-TU float codegen
         * (e.g. differing use of extended-precision intermediates) is not guaranteed to reproduce
         * identical bits even for textually identical expressions. The CONTRACT is "same formula,
         * same operand order" (verified by inspection -- see the report), not "same bits"; the
         * tolerance is a concession to the compiler, not a loosening of that contract. */
        EXPECT_NEAR(sample.v,
                    reference_ribbon_v(s, total_length, radius, mode, 3, cyclic),
                    1e-5f);
      }
    }
  }
}

TEST(paint_curve_patch_spline, texture_zone_boundaries)
{
  const float total_length = 10.0f;
  const float cap = 2.0f;
  const float eps = 1e-3f;

  const CurvePatchTextureZoneSample at_start = curve_patch_texture_zone_at(
      /*s=*/0.0f,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/cap,
      /*cap_end_length=*/cap,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(at_start.zone, CurvePatchTextureZone::Start);
  EXPECT_NEAR(at_start.v, -1.0f, 1e-5f);

  const CurvePatchTextureZoneSample before_middle = curve_patch_texture_zone_at(
      /*s=*/cap - eps,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/cap,
      /*cap_end_length=*/cap,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(before_middle.zone, CurvePatchTextureZone::Start);
  EXPECT_NEAR(before_middle.v, 1.0f, 1e-2f);

  const CurvePatchTextureZoneSample in_middle = curve_patch_texture_zone_at(
      /*s=*/total_length * 0.5f,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/cap,
      /*cap_end_length=*/cap,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(in_middle.zone, CurvePatchTextureZone::Middle);
  EXPECT_TRUE(in_middle.valid);

  const CurvePatchTextureZoneSample at_end = curve_patch_texture_zone_at(
      /*s=*/total_length,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/cap,
      /*cap_end_length=*/cap,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(at_end.zone, CurvePatchTextureZone::End);
  EXPECT_NEAR(at_end.v, 1.0f, 1e-5f);

  const CurvePatchTextureZoneSample end_begin = curve_patch_texture_zone_at(
      /*s=*/total_length - cap + eps,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/cap,
      /*cap_end_length=*/cap,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(end_begin.zone, CurvePatchTextureZone::End);
  EXPECT_NEAR(end_begin.v, -1.0f, 1e-2f);
}

TEST(paint_curve_patch_spline, texture_zone_middle_value_with_caps)
{
  /* The three lines specific to this task -- `middle_offset = s - start_len`, `middle_length =
   * total - start - end`, `middle_cyclic = false` -- are only exercised by a Middle-zone sample
   * with caps enabled AND an explicit `v` check; `texture_zone_boundaries`'s `in_middle` case only
   * asserts `zone`/`valid`, so it would pass unchanged even if any of the three were dropped or
   * corrupted (e.g. `middle_offset = s`, or `middle_length = total_length`). */
  const CurvePatchTextureZoneSample mid = curve_patch_texture_zone_at(
      /*s=*/3.0f,
      /*total_length=*/10.0f,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/2.0f,
      /*cap_end_length=*/2.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(mid.zone, CurvePatchTextureZone::Middle);
  ASSERT_TRUE(mid.valid);
  /* start_len = end_len = 2, middle_length = 10 - 2 - 2 = 6, middle_offset = 3 - 2 = 1,
   * tile_span = min(middle_length, 2 * radius) = min(6, 2) = 2,
   * v = (middle_offset - middle_length * 0.5) / tile_span * 2 = (1 - 3) / 2 * 2 = -2.0.
   * (`s = 5` -- the curve's exact midpoint -- would be a weaker witness here: symmetry alone forces
   * `v == 0` there regardless of whether `middle_offset`/`middle_length` are computed correctly.) */
  EXPECT_NEAR(mid.v, -2.0f, 1e-5f);

  /* Witness for `middle_cyclic = false`. On the `total_length = 10, cap = 2, radius = 1` numbers
   * above this would be a WEAK witness for the open-tiling forcing alone: with `middle_length = 6`
   * and `radius = 1`, #curve_patch_texture_tile_span's own pre-cyclic span is `min(6, 2) = 2`, and
   * its cyclic whole-tile snap rounds `6 / 2 = 3` (already whole) to the SAME 3 tiles, leaving
   * `tile_span == 2.0` either way -- so a caller that forgot to force `middle_cyclic = false` would
   * still pass by coincidence. Using `total_length = 11` instead makes `middle_length = 7`, whose
   * cyclic snap rounds `7 / 2 = 3.5` UP to 4 whole tiles and shrinks the span to `7 / 4 = 1.75`,
   * `1.75 != 2.0` -- a real, checkable divergence between the open and cyclic tilings. */
  const CurvePatchTextureZoneSample cyclic_off = curve_patch_texture_zone_at(
      /*s=*/3.0f,
      /*total_length=*/11.0f,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/2.0f,
      /*cap_end_length=*/2.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  const CurvePatchTextureZoneSample cyclic_on = curve_patch_texture_zone_at(
      /*s=*/3.0f,
      /*total_length=*/11.0f,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/2.0f,
      /*cap_end_length=*/2.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/true);
  ASSERT_TRUE(cyclic_off.valid);
  ASSERT_TRUE(cyclic_on.valid);
  /* start_len = end_len = 2, middle_length = 11 - 2 - 2 = 7, middle_offset = 3 - 2 = 1. With the
   * middle correctly forced open (`middle_cyclic = false` regardless of the `cyclic` argument),
   * tile_span = min(7, 2) = 2 (no whole-tile snap applies), so both calls above must agree:
   * v = (1 - 7 * 0.5) / 2 * 2 = -2.5. If `middle_cyclic` were the caller's `cyclic` instead of a
   * forced `false`, `cyclic_on.v` would instead come from the CYCLIC branch's
   * `offset / span * 2 - 1` formula over the snapped `span = 1.75`:
   * v = 1 / 1.75 * 2 - 1 = 0.142857..., nowhere close to -2.5. */
  EXPECT_NEAR(cyclic_off.v, -2.5f, 1e-5f);
  EXPECT_NEAR(cyclic_on.v, -2.5f, 1e-5f);
}

TEST(paint_curve_patch_spline, texture_zone_caps_shrink_proportionally)
{
  /* Caps asking for 6 + 3 on a curve of 3 must scale by 1/3 and keep their 2:1 ratio, collapsing the
   * middle rather than letting either cap overrun the curve. */
  const float total_length = 3.0f;
  const CurvePatchTextureZoneSample at_boundary = curve_patch_texture_zone_at(
      /*s=*/2.0f + 1e-3f,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/6.0f,
      /*cap_end_length=*/3.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  /* Start took 2.0 of 3.0, so anything past it belongs to End. */
  EXPECT_EQ(at_boundary.zone, CurvePatchTextureZone::End);

  const CurvePatchTextureZoneSample inside_start = curve_patch_texture_zone_at(
      /*s=*/1.0f,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/6.0f,
      /*cap_end_length=*/3.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(inside_start.zone, CurvePatchTextureZone::Start);
  /* start_len scales to 6 * (3 / 9) = 2.0, so s = 1.0 sits at the midpoint of the Start zone's
   * single tile: v = (1 - 0) / 2 * 2 - 1 = 0.0. This alone pins down start_len's scale factor but
   * says nothing about end_len -- see `inside_end` below for the other half of the ratio. */
  EXPECT_NEAR(inside_start.v, 0.0f, 1e-5f);

  const CurvePatchTextureZoneSample inside_end = curve_patch_texture_zone_at(
      /*s=*/2.5f,
      /*total_length=*/total_length,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/6.0f,
      /*cap_end_length=*/3.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(inside_end.zone, CurvePatchTextureZone::End);
  /* end_len scales to 3 * (3 / 9) = 1.0, end_begin = 3 - 1 = 2.0, so s = 2.5 sits at the midpoint
   * of the End zone's single tile: v = (2.5 - 2.0) / 1.0 * 2 - 1 = 0.0. Without this, a wrong,
   * under-scaled end_len (e.g. 1.5, which would still pass `inside_start` and `at_boundary` above)
   * would go undetected -- `inside_start` only pins down start_len, and `at_boundary` only checks
   * which zone claims a point, not the scale factor `end_len` actually used inside End. */
  EXPECT_NEAR(inside_end.v, 0.0f, 1e-5f);
}

TEST(paint_curve_patch_spline, texture_zone_degenerate_middle_is_invalid)
{
  /* Caps exactly filling the curve leave no middle; a position that would fall there must report
   * invalid instead of dividing by a zero-length span. */
  const CurvePatchTextureZoneSample sample = curve_patch_texture_zone_at(
      /*s=*/5.0f,
      /*total_length=*/10.0f,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/5.0f,
      /*cap_end_length=*/5.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_FALSE(sample.valid);
}

TEST(paint_curve_patch_spline, texture_zone_zero_start_cap_is_unreachable)
{
  /* A zero Start Length is how an old file reads back, and how the UI disables the cap. No `s` may
   * land in that zone. */
  const CurvePatchTextureZoneSample sample = curve_patch_texture_zone_at(
      /*s=*/0.0f,
      /*total_length=*/10.0f,
      /*radius_for_middle_tile=*/1.0f,
      /*caps_enabled=*/true,
      /*cap_start_length=*/0.0f,
      /*cap_end_length=*/2.0f,
      /*length_mode=*/MTEX_CURVE_PATCH_LENGTH_DEFAULT,
      /*length_repeat=*/1,
      /*cyclic=*/false);
  EXPECT_EQ(sample.zone, CurvePatchTextureZone::Middle);
  EXPECT_TRUE(sample.valid);
}

TEST(paint_curve_patch_stamps, stamps_build_without_cdf_uses_brush_texture)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f), float3(10.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));

  Vector<CurvePatchStamp> stamps;
  curve_patch_stamps_build(/*spline=*/spline,
                           /*radius=*/1.0f,
                           /*spacing_frac=*/0.5f,
                           /*jitter_amount=*/0.0f,
                           /*size_random=*/0.0f,
                           /*strength_random=*/0.0f,
                           /*base_angle=*/0.0f,
                           /*random_angle=*/0.0f,
                           /*seed=*/1234u,
                           /*texture_weights_cdf=*/{},
                           /*r_stamps=*/stamps);

  EXPECT_FALSE(stamps.is_empty());
  for (const CurvePatchStamp &stamp : stamps) {
    EXPECT_EQ(stamp.tex_index, -1);
  }
}

TEST(paint_curve_patch_stamps, stamps_build_texture_index_is_deterministic)
{
  CurvePatchSpline spline;
  const float3 points[2] = {float3(0.0f), float3(20.0f, 0.0f, 0.0f)};
  spline.build_from_positions(Span(points, 2));
  const float cdf[3] = {1.0f, 2.0f, 3.0f};

  Vector<CurvePatchStamp> a, b, c;
  curve_patch_stamps_build(/*spline=*/spline,
                           /*radius=*/1.0f,
                           /*spacing_frac=*/0.5f,
                           /*jitter_amount=*/0.0f,
                           /*size_random=*/0.0f,
                           /*strength_random=*/0.0f,
                           /*base_angle=*/0.0f,
                           /*random_angle=*/0.0f,
                           /*seed=*/7u,
                           /*texture_weights_cdf=*/Span(cdf, 3),
                           /*r_stamps=*/a);
  curve_patch_stamps_build(/*spline=*/spline,
                           /*radius=*/1.0f,
                           /*spacing_frac=*/0.5f,
                           /*jitter_amount=*/0.0f,
                           /*size_random=*/0.0f,
                           /*strength_random=*/0.0f,
                           /*base_angle=*/0.0f,
                           /*random_angle=*/0.0f,
                           /*seed=*/7u,
                           /*texture_weights_cdf=*/Span(cdf, 3),
                           /*r_stamps=*/b);
  curve_patch_stamps_build(/*spline=*/spline,
                           /*radius=*/1.0f,
                           /*spacing_frac=*/0.5f,
                           /*jitter_amount=*/0.0f,
                           /*size_random=*/0.0f,
                           /*strength_random=*/0.0f,
                           /*base_angle=*/0.0f,
                           /*random_angle=*/0.0f,
                           /*seed=*/99u,
                           /*texture_weights_cdf=*/Span(cdf, 3),
                           /*r_stamps=*/c);

  ASSERT_EQ(a.size(), b.size());
  ASSERT_EQ(a.size(), c.size());
  bool any_differs = false;
  for (const int i : a.index_range()) {
    EXPECT_EQ(a[i].tex_index, b[i].tex_index);
    EXPECT_GE(a[i].tex_index, 0);
    EXPECT_LT(a[i].tex_index, 3);
    if (a[i].tex_index != c[i].tex_index) {
      any_differs = true;
    }
  }
  /* A different seed must actually reshuffle, or Reseed would be a no-op for textures. */
  EXPECT_TRUE(any_differs);
}

TEST(paint_curve_patch_stamps, cyclic_wrap_ghosts_inherit_texture_index)
{
  CurvePatchSpline spline;
  const float3 points[4] = {float3(0.0f, 0.0f, 0.0f),
                            float3(4.0f, 0.0f, 0.0f),
                            float3(4.0f, 4.0f, 0.0f),
                            float3(0.0f, 4.0f, 0.0f)};
  /* NOTE: the brief's Step 1 listing for this test omits the `radii` argument here (`Span(points,
   * 4), true`), which would try to bind `true` to `Span<float> radii` and fail to compile. Fixed to
   * match the established cyclic-build call pattern used throughout this file (e.g.
   * `cyclic_wrap_keeps_the_list_sorted` above). */
  spline.build_from_positions(Span(points, 4), {}, true);
  const float cdf[2] = {1.0f, 2.0f};

  Vector<CurvePatchStamp> stamps;
  curve_patch_stamps_build(/*spline=*/spline,
                           /*radius=*/0.5f,
                           /*spacing_frac=*/0.5f,
                           /*jitter_amount=*/0.0f,
                           /*size_random=*/0.0f,
                           /*strength_random=*/0.0f,
                           /*base_angle=*/0.0f,
                           /*random_angle=*/0.0f,
                           /*seed=*/3u,
                           /*texture_weights_cdf=*/Span(cdf, 2),
                           /*r_stamps=*/stamps);
  const int real_num = stamps.size();
  /* Snapshot the real stamps' `(center_v, tex_index)` BEFORE the wrap call, since it appends ghosts
   * to this same vector and re-sorts it -- afterwards there is no way to tell a real stamp from a
   * ghost by position in the array alone. */
  const Vector<CurvePatchStamp> real_stamps = stamps;
  const float total_length = spline.total_length();

  curve_patch_stamps_add_cyclic_wrap(stamps, total_length, 1.0f);
  ASSERT_GT(stamps.size(), real_num);

  /* A ghost is a whole-struct copy of a real stamp with `center_v` displaced by exactly
   * `+total_length` (near the start) or `-total_length` (near the end) -- see
   * #curve_patch_stamps_add_cyclic_wrap. Reversing that displacement recovers the source's
   * `center_v` and, matched back into the snapshot above, its `tex_index`: that is the actual claim
   * this test exists to check (a regression where ghosts drew a FRESH `tex_index` instead of
   * inheriting one would still leave every value in range, which is all the old assertion checked).
   * Real stamps stay within `[0, total_length)` (`curve_patch_stamps_build` never places one AT the
   * seam for a cyclic curve), so testing which side of that range a stamp falls on unambiguously
   * tells ghost from real and picks the correct displacement to undo. */
  for (const CurvePatchStamp &stamp : stamps) {
    EXPECT_GE(stamp.tex_index, 0);
    EXPECT_LT(stamp.tex_index, 2);

    float source_v = stamp.center_v;
    if (source_v >= total_length) {
      source_v -= total_length;
    }
    else if (source_v < 0.0f) {
      source_v += total_length;
    }

    int source_tex_index = -2;
    for (const CurvePatchStamp &real : real_stamps) {
      if (std::abs(real.center_v - source_v) < 1e-4f) {
        source_tex_index = real.tex_index;
        break;
      }
    }
    ASSERT_NE(source_tex_index, -2) << "no source stamp found for center_v=" << stamp.center_v;
    EXPECT_EQ(stamp.tex_index, source_tex_index);
  }
}

}  // namespace blender::ed::sculpt_paint::tests
