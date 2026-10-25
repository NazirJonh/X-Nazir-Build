/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "testing/testing.h"

#include "BKE_gtest_base.hh"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

namespace blender::bke::tests {

/** Fixture: registers the CPP types and attribute providers CurvesGeometry needs. */
class paint_curve_patch_build : public bke::BlenderGTestBase {};

/** Fixture: registers the CPP types and attribute providers CurvesGeometry needs. */
class paint_curve_patch_points : public bke::BlenderGTestBase {};

/* A poly control curve with `count` points spaced one unit apart along +X, every radius 1.0.
 * Poly rather than bezier so the test pins the build, not the bezier evaluator. */
static CurvesGeometry make_poly_control_curve(const int count, const bool cyclic = false)
{
  CurvesGeometry curves(count, 1);
  curves.offsets_for_write().copy_from({0, count});
  curves.fill_curve_types(CURVE_TYPE_POLY);

  MutableSpan<float3> positions = curves.positions_for_write();
  for (const int i : positions.index_range()) {
    positions[i] = float3(float(i), 0.0f, 0.0f);
  }
  curves.radius_for_write().fill(1.0f);
  curves.cyclic_for_write().fill(cyclic);

  /* A freshly constructed curve carries a stale evaluated cache, exactly as it does in
   * `paintcurve_geometry_init_bezier()`; without both tags the first build would tessellate that
   * rather than the positions just written. */
  curves.tag_topology_changed();
  curves.tag_positions_changed();
  return curves;
}

static CurvePatchParams make_params()
{
  CurvePatchParams params;
  params.radius = 1.0f;
  params.plane_normal = float3(0.0f, 0.0f, 1.0f);
  return params;
}

TEST_F(paint_curve_patch_build, straight_curve_builds_a_ribbon)
{
  const CurvesGeometry curve = make_poly_control_curve(3);
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, geometry);

  EXPECT_FALSE(geometry.spline.is_empty());
  EXPECT_GT(geometry.spline.total_length(), 0.0f);
  EXPECT_GT(geometry.ribbon_radius, 0.0f);
}

TEST_F(paint_curve_patch_build, single_point_curve_yields_an_empty_spline)
{
  const CurvesGeometry curve = make_poly_control_curve(1);
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, geometry);

  EXPECT_TRUE(geometry.spline.is_empty());
}

TEST_F(paint_curve_patch_build, coincident_points_yield_a_zero_length_spline)
{
  CurvesGeometry curve = make_poly_control_curve(4);
  curve.positions_for_write().fill(float3(0.0f));
  curve.tag_positions_changed();

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, geometry);

  /* `CurvePatchSpline::is_empty()` counts points, not extent, so four coincident points still make
   * a non-empty spline -- one of zero arc length, which the ribbon grid then refuses to rasterize.
   * Consumers therefore cannot rely on `is_empty()` alone to reject a degenerate curve. */
  EXPECT_NEAR(geometry.spline.total_length(), 0.0f, 1e-6f);
  EXPECT_FALSE(geometry.ribbon.ready);
}

TEST_F(paint_curve_patch_build, cyclic_curve_is_carried_into_the_spline)
{
  const CurvesGeometry open = make_poly_control_curve(4, /*cyclic*/ false);
  const CurvesGeometry closed = make_poly_control_curve(4, /*cyclic*/ true);

  CurvePatchGeometry open_geometry;
  CurvePatchGeometry closed_geometry;
  curve_patch_build_from_control_curve(
      open, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, open_geometry);
  curve_patch_build_from_control_curve(
      closed, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, closed_geometry);

  /* Closing the loop adds the returning segment, so the arc length must grow. */
  EXPECT_GT(closed_geometry.spline.total_length(), open_geometry.spline.total_length());
}

TEST_F(paint_curve_patch_build, an_unready_surface_leaves_the_curve_in_its_own_plane)
{
  const CurvesGeometry curve = make_poly_control_curve(3);
  CurvePatchGeometry geometry;
  ASSERT_FALSE(geometry.surface.ready);

  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, geometry);

  /* No snapshot to project onto: every polyline point keeps the curve's own Z. */
  for (const float3 &p : geometry.spline.poly_3d) {
    EXPECT_NEAR(p.z, 0.0f, 1e-5f);
  }
}

TEST_F(paint_curve_patch_build, a_weight_table_spreads_stamps_over_several_slots)
{
  const CurvesGeometry curve = make_poly_control_curve(8);
  CurvePatchParams params = make_params();
  params.stamp_mode = CurvePatchStampMode::Stamps;
  params.spacing_frac = 0.25f;

  const float cdf[2] = {0.5f, 1.0f};
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, params, Span(cdf, 2), CurvePatchBuildMode::SurfaceWindowed, geometry);

  ASSERT_FALSE(geometry.stamps.is_empty());
  bool saw_slot_0 = false;
  bool saw_slot_1 = false;
  for (const CurvePatchStamp &stamp : geometry.stamps) {
    saw_slot_0 |= stamp.tex_index == 0;
    saw_slot_1 |= stamp.tex_index == 1;
  }
  EXPECT_TRUE(saw_slot_0);
  EXPECT_TRUE(saw_slot_1);
}

/* -------------------------------------------------------------------- */
/** \name POINTS Stamp Layout
 * \{ */

static CurvePatchParams make_points_params()
{
  CurvePatchParams params = make_params();
  params.stamp_mode = CurvePatchStampMode::Stamps;
  params.stamp_layout = CurvePatchStampLayout::Points;
  return params;
}

TEST_F(paint_curve_patch_points, one_stamp_per_control_point_open)
{
  const CurvesGeometry curve = make_poly_control_curve(5);
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_points_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  ASSERT_EQ(geometry.spline.control_point_lengths.size(), 5);
  ASSERT_EQ(geometry.stamps.size(), 5);
  /* Jitter is zero by default, so each stamp sits exactly on its control point's arc length. */
  for (const int i : geometry.stamps.index_range()) {
    EXPECT_NEAR(geometry.stamps[i].center_v, float(i), 1e-5f);
    /* Every control radius is 1.0 and no size randomization is set: brush radius it is. */
    EXPECT_NEAR(geometry.stamps[i].half_extent, 1.0f, 1e-6f);
    EXPECT_NEAR(geometry.stamps[i].strength, 1.0f, 1e-6f);
  }
}

TEST_F(paint_curve_patch_points, cyclic_curve_has_no_seam_duplicate)
{
  const CurvesGeometry curve = make_poly_control_curve(4, /*cyclic*/ true);
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_points_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  ASSERT_EQ(geometry.spline.control_point_lengths.size(), 4);
  /* Four control points, one stamp each. The seam wrap adds ghosts (copies, not new stamps) for
   * everything within one stamp reach of the join -- the 0/3 stamps straddle it on this loop --
   * so the list holds those two ghosts on top. The POINTS invariant under test is that the REAL
   * stamps stay exactly at the control points' arc lengths, none doubled at the seam. */
  int real_stamps = 0;
  for (const CurvePatchStamp &stamp : geometry.stamps) {
    /* Ghosts sit outside `[0, total)` (at -1 and 4 here); real stamps never can. */
    const bool is_ghost = stamp.center_v < -1e-5f || stamp.center_v > 4.0f - 1e-5f;
    if (!is_ghost) {
      real_stamps++;
      EXPECT_NEAR(stamp.center_v, std::round(stamp.center_v), 1e-5f);
    }
  }
  EXPECT_EQ(real_stamps, 4);
}

TEST_F(paint_curve_patch_points, per_point_strength_texture_and_angle_are_consumed)
{
  CurvesGeometry curve = make_poly_control_curve(3);
  /* Point 1: half strength, its own angle, and a slot assignment. */
  bke::MutableAttributeAccessor attrs = curve.attributes_for_write();
  bke::SpanAttributeWriter<float> strengths = attrs.lookup_or_add_for_write_span<float>(
      CURVE_PATCH_ATTR_STAMP_STRENGTH, AttrDomain::Point);
  strengths.span.fill(1.0f);
  strengths.span[1] = 0.5f;
  strengths.finish();
  bke::SpanAttributeWriter<float> angles = attrs.lookup_or_add_for_write_span<float>(
      CURVE_PATCH_ATTR_STAMP_ANGLE, AttrDomain::Point);
  angles.span.fill(0.0f);
  angles.span[1] = 0.25f;
  angles.finish();
  bke::SpanAttributeWriter<int> textures = attrs.lookup_or_add_for_write_span<int>(
      CURVE_PATCH_ATTR_STAMP_TEXTURE, AttrDomain::Point);
  textures.span.fill(-1);
  textures.span[1] = 1;
  textures.finish();

  CurvePatchParams params = make_points_params();
  params.stamp_use_random_angle = false; /* Per-point angles decide. */

  const float cdf[2] = {0.5f, 1.0f};
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, params, Span(cdf, 2), CurvePatchBuildMode::PlanarSingleWindow, geometry);

  ASSERT_EQ(geometry.stamps.size(), 3);
  /* Stamps come back sorted by center_v, which for this even curve is the control-point order. */
  EXPECT_NEAR(geometry.stamps[1].strength, 0.5f, 1e-6f);
  EXPECT_NEAR(geometry.stamps[1].angle, 0.25f, 1e-6f);
  /* An explicit slot index within the resolved table is taken verbatim. */
  EXPECT_EQ(geometry.stamps[1].tex_index, 1);
  /* Auto (-1) points draw from the weight table -- with this table the hash picks slot 0 or 1. */
  EXPECT_GE(geometry.stamps[0].tex_index, -1);
  EXPECT_LE(geometry.stamps[0].tex_index, 1);
  EXPECT_GE(geometry.stamps[2].tex_index, -1);
  EXPECT_LE(geometry.stamps[2].tex_index, 1);
}

TEST_F(paint_curve_patch_points, zero_strength_point_is_skipped)
{
  CurvesGeometry curve = make_poly_control_curve(3);
  bke::MutableAttributeAccessor attrs = curve.attributes_for_write();
  bke::SpanAttributeWriter<float> strengths = attrs.lookup_or_add_for_write_span<float>(
      CURVE_PATCH_ATTR_STAMP_STRENGTH, AttrDomain::Point);
  strengths.span.fill(1.0f);
  strengths.span[1] = 0.0f;
  strengths.finish();

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_points_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  EXPECT_EQ(geometry.stamps.size(), 2);
  for (const CurvePatchStamp &stamp : geometry.stamps) {
    EXPECT_NE(stamp.center_v, 1.0f);
  }
}

TEST_F(paint_curve_patch_points, per_point_radius_sizes_the_stamp_and_the_strip)
{
  CurvesGeometry curve = make_poly_control_curve(3);
  curve.radius_for_write().fill(1.0f);
  curve.radius_for_write()[1] = 2.0f;

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_points_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  ASSERT_EQ(geometry.stamps.size(), 3);
  EXPECT_NEAR(geometry.stamps[1].half_extent, 2.0f, 1e-6f);
  /* The three shared bounds are sized from the LARGEST stamp (radius 2), not the brush radius. */
  const float expected_reach = curve_patch_stamp_reach(2.0f);
  EXPECT_NEAR(geometry.ribbon_radius, expected_reach, 1e-5f);
  EXPECT_NEAR(geometry.ribbon_end_margin, expected_reach, 1e-5f);
  EXPECT_NEAR(geometry.stamp_search_reach, expected_reach, 1e-5f);
}

TEST_F(paint_curve_patch_points, negative_strength_point_is_skipped)
{
  CurvesGeometry curve = make_poly_control_curve(3);
  bke::MutableAttributeAccessor attrs = curve.attributes_for_write();
  bke::SpanAttributeWriter<float> strengths = attrs.lookup_or_add_for_write_span<float>(
      CURVE_PATCH_ATTR_STAMP_STRENGTH, AttrDomain::Point);
  strengths.span.fill(1.0f);
  strengths.span[1] = -0.5f;
  strengths.finish();

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_points_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  /* A negative per-point strength (only writable from Python; the editor clamps in [0, 1]) reads
   * as zero and drops the stamp. */
  EXPECT_EQ(geometry.stamps.size(), 2);
  for (const CurvePatchStamp &stamp : geometry.stamps) {
    EXPECT_NE(stamp.center_v, 1.0f);
  }
}

TEST_F(paint_curve_patch_points, missing_radius_attribute_uses_the_brush_radius)
{
  CurvesGeometry curve = make_poly_control_curve(3);
  /* A paint-curve point with no `radius` attribute means "the brush radius" (1.0), not
   * `CurvesGeometry::radius()`'s 0.01 fallback. */
  curve.attributes_for_write().remove("radius");

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_points_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  ASSERT_EQ(geometry.stamps.size(), 3);
  for (const CurvePatchStamp &stamp : geometry.stamps) {
    EXPECT_NEAR(stamp.half_extent, 1.0f, 1e-6f);
  }
}

TEST_F(paint_curve_patch_points, stamp_seed_override_drives_randomization)
{
  CurvePatchParams params = make_points_params();
  params.stamp_size_random = 0.5f;
  /* Jitter stays zero so the stamps stay sorted in control-point order and can be compared
   * index-wise. */

  const auto set_seed = [](CurvesGeometry &curve, const Span<int> seeds) {
    bke::SpanAttributeWriter<int> attr = curve.attributes_for_write().lookup_or_add_for_write_span<
        int>(CURVE_PATCH_ATTR_STAMP_SEED, AttrDomain::Point);
    attr.span.copy_from(seeds);
    attr.finish();
  };

  const CurvesGeometry indexed = make_poly_control_curve(4);
  CurvePatchGeometry indexed_geometry;
  curve_patch_build_from_control_curve(
      indexed, params, {}, CurvePatchBuildMode::PlanarSingleWindow, indexed_geometry);

  /* An explicit identity seed must reproduce the index-keyed draws exactly -- the backward
   * compatibility promise for curves that never materialized the attribute. */
  CurvesGeometry identity = make_poly_control_curve(4);
  int identity_seeds[4] = {0, 1, 2, 3};
  set_seed(identity, Span(identity_seeds, 4));
  CurvePatchGeometry identity_geometry;
  curve_patch_build_from_control_curve(
      identity, params, {}, CurvePatchBuildMode::PlanarSingleWindow, identity_geometry);

  ASSERT_EQ(indexed_geometry.stamps.size(), identity_geometry.stamps.size());
  for (const int i : identity_geometry.stamps.index_range()) {
    EXPECT_NEAR(indexed_geometry.stamps[i].half_extent,
                identity_geometry.stamps[i].half_extent,
                1e-6f);
  }

  /* A swapped seed changes a point's randomized size even though its position did not move,
   * proving the hash is keyed by the seed and not the index. */
  CurvesGeometry swapped = make_poly_control_curve(4);
  int swapped_seeds[4] = {0, 2, 1, 3};
  set_seed(swapped, Span(swapped_seeds, 4));
  CurvePatchGeometry swapped_geometry;
  curve_patch_build_from_control_curve(
      swapped, params, {}, CurvePatchBuildMode::PlanarSingleWindow, swapped_geometry);

  ASSERT_EQ(swapped_geometry.stamps.size(), 4);
  bool any_different = false;
  for (const int i : swapped_geometry.stamps.index_range()) {
    if (std::abs(swapped_geometry.stamps[i].half_extent -
                 identity_geometry.stamps[i].half_extent) > 1e-6f)
    {
      any_different = true;
    }
  }
  EXPECT_TRUE(any_different);
}

TEST_F(paint_curve_patch_points, fill_layout_is_untouched_by_the_points_fields)
{
  const CurvesGeometry curve = make_poly_control_curve(6);
  CurvePatchParams params = make_params();
  params.stamp_mode = CurvePatchStampMode::Stamps;
  params.stamp_layout = CurvePatchStampLayout::Fill;
  params.spacing_frac = 0.5f;

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, params, {}, CurvePatchBuildMode::PlanarSingleWindow, geometry);

  /* Curve of length 5, spacing 1.0: six stamps at the integer arc lengths. */
  ASSERT_EQ(geometry.stamps.size(), 6);
  for (const int i : geometry.stamps.index_range()) {
    EXPECT_NEAR(geometry.stamps[i].center_v, float(i), 1e-5f);
    EXPECT_NEAR(geometry.stamps[i].half_extent, 1.0f, 1e-6f);
  }
}

/** \} */

TEST_F(paint_curve_patch_build, empty_normals_span_forces_ribbon_not_frames)
{
  /* The underlying mechanism #CurvePatchBuildMode::PlanarSingleWindow is implemented in terms of:
   * handing the LOW-LEVEL `curve_patch_geometry_build()` an empty `evaluated_normals` span must
   * always take the single-window `ribbon` path, never the multi-window `frames` path --
   * regardless of curvature. The mode is the contract callers rely on; this pins the mechanism
   * underneath it, so a change to `curve_patch_geometry_build()`'s branch condition is caught
   * here rather than as a silently reshaped 2D patch. */
  const CurvesGeometry curve = make_poly_control_curve(3);
  Array<float3> evaluated_positions(curve.evaluated_positions());
  Array<float> evaluated_radii(curve.evaluated_points_num(), 1.0f);

  CurvePatchGeometry geometry;
  curve_patch_geometry_build(evaluated_positions.as_span(),
                             evaluated_radii.as_span(),
                             /*evaluated_normals=*/{},
                             /*cyclic=*/false,
                             make_params(),
                             /*stamp_texture_weights_cdf=*/{},
                             geometry);

  EXPECT_TRUE(geometry.spline.normals_3d.is_empty());
  EXPECT_TRUE(geometry.ribbon.ready);
  EXPECT_TRUE(geometry.frames.frames.is_empty());
}

TEST_F(paint_curve_patch_build, build_mode_selects_normals_and_strip_path)
{
  /* The two modes of the wrapper, pinned side by side on the same input.
   *
   * SurfaceWindowed always synthesizes a per-point normal via
   * `lookup_or_default(CURVE_PATCH_ATTR_SURFACE_NORMAL, ..., (0,0,1))`, even when the control
   * curve carries no such attribute and no surface snapshot exists -- which is what makes windowed
   * frames possible. PlanarSingleWindow builds none, pinning the result to the single-window
   * ribbon a flat canvas needs. Before the mode existed, the only way to reach the planar result
   * was to bypass this wrapper and re-implement its tessellation, which is exactly what the 2D
   * rasterizer used to do. */
  const CurvesGeometry curve = make_poly_control_curve(3);

  CurvePatchGeometry windowed;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, windowed);
  EXPECT_FALSE(windowed.spline.normals_3d.is_empty());

  CurvePatchGeometry planar;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, planar);
  EXPECT_TRUE(planar.spline.normals_3d.is_empty());
  EXPECT_TRUE(planar.ribbon.ready);
  EXPECT_TRUE(planar.frames.frames.is_empty());
}

/* -------------------------------------------------------------------- */
/** \name Per-Point Radius Interpolation Profiles
 * \{ */

/* A bezier control curve (the type a paint curve always is) with `radii.size()` points spaced one
 * unit apart along +X, AUTO handles and a small resolution so profile values are easy to check:
 * every segment holds `res` samples at t = j / res, the anchor of control point i sits at
 * evaluated index i * res, and a non-cyclic curve adds one final sample for the last point. */
static CurvesGeometry make_bezier_control_curve(const Span<float> radii,
                                                const bool cyclic = false,
                                                const int res = 4)
{
  const int count = int(radii.size());
  CurvesGeometry curves(count, 1);
  curves.offsets_for_write().copy_from({0, count});
  curves.fill_curve_types(CURVE_TYPE_BEZIER);
  curves.resolution_for_write().fill(res);

  MutableSpan<float3> positions = curves.positions_for_write();
  for (const int i : positions.index_range()) {
    positions[i] = float3(float(i), 0.0f, 0.0f);
  }
  curves.handle_types_left_for_write().fill(BEZIER_HANDLE_AUTO);
  curves.handle_types_right_for_write().fill(BEZIER_HANDLE_AUTO);
  /* `calculate_bezier_auto_handles()` silently does nothing when the handle POSITION attributes
   * are absent, which would leave every evaluated position at the origin (zero-length curve). */
  curves.handle_positions_left_for_write();
  curves.handle_positions_right_for_write();
  curves.calculate_bezier_auto_handles();
  curves.radius_for_write().copy_from(radii);
  curves.cyclic_for_write().fill(cyclic);

  curves.tag_topology_changed();
  curves.tag_positions_changed();
  return curves;
}

TEST_F(paint_curve_patch_build, radius_interp_anchors_and_midpoints_follow_the_profile)
{
  const float radii[2] = {1.0f, 0.2f};
  const CurvesGeometry curve = make_bezier_control_curve(Span(radii, 2));
  Array<float> evaluated(curve.evaluated_points_num());
  ASSERT_EQ(evaluated.size(), 5); /* 4 samples for the segment + the final point. */

  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Linear, evaluated.as_mutable_span());
  EXPECT_NEAR(evaluated[0], 1.0f, 1e-6f);
  EXPECT_NEAR(evaluated[1], 0.8f, 1e-6f); /* t = 0.25 lerp. */
  EXPECT_NEAR(evaluated[4], 0.2f, 1e-6f);

  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Smooth, evaluated.as_mutable_span());
  /* smoothstep(0.25) = 0.15625, smoothstep(0.75) = 0.84375. */
  EXPECT_NEAR(evaluated[1], math::interpolate(1.0f, 0.2f, 0.15625f), 1e-6f);
  EXPECT_NEAR(evaluated[3], math::interpolate(1.0f, 0.2f, 0.84375f), 1e-6f);
  EXPECT_NEAR(evaluated[4], 0.2f, 1e-6f);

  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Ease, evaluated.as_mutable_span());
  const float ease_quarter = 0.5f - 0.5f * std::cos(float(M_PI) * 0.25f);
  EXPECT_NEAR(evaluated[1], math::interpolate(1.0f, 0.2f, ease_quarter), 1e-6f);

  /* With only two points SPLINE's tangents equal the one secant, so it degenerates to the lerp
   * -- pinned so the degenerate case is a documented equivalence, not an accident. */
  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Spline, evaluated.as_mutable_span());
  EXPECT_NEAR(evaluated[1], 0.8f, 1e-6f);
}

TEST_F(paint_curve_patch_build, radius_interp_spline_rounds_local_extrema)
{
  const float radii[3] = {1.0f, 0.2f, 1.0f};
  const CurvesGeometry curve = make_bezier_control_curve(Span(radii, 3));
  Array<float> evaluated(curve.evaluated_points_num());
  ASSERT_EQ(evaluated.size(), 9); /* 2 segments x 4 samples + the final point. */

  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Spline, evaluated.as_mutable_span());

  /* The middle point is a local minimum, so Fritsch-Carlson zeroes its tangent and the profile
   * dips BELOW the lerp on the way in instead of kinking at the anchor. */
  EXPECT_NEAR(evaluated[4], 0.2f, 1e-6f);
  EXPECT_NEAR(evaluated[2], 0.5f, 1e-5f);
  EXPECT_LT(evaluated[2], 0.6f);
  EXPECT_NEAR(evaluated[6], 0.5f, 1e-5f);
}

TEST_F(paint_curve_patch_build, radius_interp_spline_limiter_prevents_overshoot)
{
  /* The neighbors pull the middle segment's tangents far past its own shallow secant; without
   * the Fritsch-Carlson limiter the cubic would bulge outside [0.5, 0.55]. */
  const float radii[4] = {0.0f, 0.5f, 0.55f, 1.0f};
  const CurvesGeometry curve = make_bezier_control_curve(Span(radii, 4));
  Array<float> evaluated(curve.evaluated_points_num());

  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Spline, evaluated.as_mutable_span());

  /* Segment 1's samples (evaluated indices 4..7) stay within its endpoint radii. */
  for (const int i : IndexRange(4, 4)) {
    EXPECT_GE(evaluated[i], 0.5f - 1e-5f);
    EXPECT_LE(evaluated[i], 0.55f + 1e-5f);
  }
}

TEST_F(paint_curve_patch_build, radius_interp_cyclic_wraps_the_join)
{
  const float radii[3] = {1.0f, 0.4f, 0.7f};
  const CurvesGeometry curve = make_bezier_control_curve(Span(radii, 3), /*cyclic*/ true);
  Array<float> evaluated(curve.evaluated_points_num());
  /* 3 segments x 4 samples; a cyclic evaluation does not duplicate the join point. */
  ASSERT_EQ(evaluated.size(), 12);

  curve_patch_radii_interpolate_to_evaluated(
      curve, CurvePatchRadiusInterp::Smooth, evaluated.as_mutable_span());

  EXPECT_NEAR(evaluated[0], 1.0f, 1e-6f);
  EXPECT_NEAR(evaluated[4], 0.4f, 1e-6f);
  EXPECT_NEAR(evaluated[8], 0.7f, 1e-6f);
  /* The wrap segment interpolates back to radii[0]: its last sample sits at t = 0.75. */
  const float wrap_t = 0.75f * 0.75f * (3.0f - 2.0f * 0.75f);
  EXPECT_NEAR(evaluated[11], math::interpolate(0.7f, 1.0f, wrap_t), 1e-6f);
}

TEST_F(paint_curve_patch_build, radius_interp_flows_into_the_built_spline)
{
  const float radii[3] = {1.0f, 0.2f, 1.0f};
  const CurvesGeometry curve = make_bezier_control_curve(Span(radii, 3));

  CurvePatchGeometry linear_geometry;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::PlanarSingleWindow, linear_geometry);

  CurvePatchParams smooth_params = make_params();
  smooth_params.radius_interp = CurvePatchRadiusInterp::Smooth;
  CurvePatchGeometry smooth_geometry;
  curve_patch_build_from_control_curve(
      curve, smooth_params, {}, CurvePatchBuildMode::PlanarSingleWindow, smooth_geometry);

  ASSERT_FALSE(linear_geometry.spline.radii.is_empty());
  /* A curve collapsed to the origin by missing handle positions would still satisfy the radii
   * comparison; the built ribbon has to have real length for this test to exercise a build. */
  EXPECT_GT(linear_geometry.spline.total_length(), 0.0f);
  /* The profile changes the tessellated radii the whole ribbon is built from; only the anchors
   * coincide, so the two builds must disagree somewhere between them. */
  bool any_different = false;
  for (const int i : linear_geometry.spline.radii.index_range()) {
    if (std::abs(linear_geometry.spline.radii[i] - smooth_geometry.spline.radii[i]) > 1e-6f) {
      any_different = true;
    }
  }
  EXPECT_TRUE(any_different);
}

/** \} */

}  // namespace blender::bke::tests
