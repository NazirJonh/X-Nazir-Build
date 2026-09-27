/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "testing/testing.h"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

namespace blender::bke::tests {

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

TEST(paint_curve_patch_build, straight_curve_builds_a_ribbon)
{
  const CurvesGeometry curve = make_poly_control_curve(3);
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, geometry);

  EXPECT_FALSE(geometry.spline.is_empty());
  EXPECT_GT(geometry.spline.total_length(), 0.0f);
  EXPECT_GT(geometry.ribbon_radius, 0.0f);
}

TEST(paint_curve_patch_build, single_point_curve_yields_an_empty_spline)
{
  const CurvesGeometry curve = make_poly_control_curve(1);
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(
      curve, make_params(), {}, CurvePatchBuildMode::SurfaceWindowed, geometry);

  EXPECT_TRUE(geometry.spline.is_empty());
}

TEST(paint_curve_patch_build, coincident_points_yield_a_zero_length_spline)
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

TEST(paint_curve_patch_build, cyclic_curve_is_carried_into_the_spline)
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

TEST(paint_curve_patch_build, an_unready_surface_leaves_the_curve_in_its_own_plane)
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

TEST(paint_curve_patch_build, a_weight_table_spreads_stamps_over_several_slots)
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

TEST(paint_curve_patch_points, one_stamp_per_control_point_open)
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

TEST(paint_curve_patch_points, cyclic_curve_has_no_seam_duplicate)
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

TEST(paint_curve_patch_points, per_point_strength_texture_and_angle_are_consumed)
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

TEST(paint_curve_patch_points, zero_strength_point_is_skipped)
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

TEST(paint_curve_patch_points, per_point_radius_sizes_the_stamp_and_the_strip)
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

TEST(paint_curve_patch_points, negative_strength_point_is_skipped)
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

TEST(paint_curve_patch_points, missing_radius_attribute_uses_the_brush_radius)
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

TEST(paint_curve_patch_points, stamp_seed_override_drives_randomization)
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

TEST(paint_curve_patch_points, fill_layout_is_untouched_by_the_points_fields)
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

TEST(paint_curve_patch_build, empty_normals_span_forces_ribbon_not_frames)
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

TEST(paint_curve_patch_build, build_mode_selects_normals_and_strip_path)
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

}  // namespace blender::bke::tests
