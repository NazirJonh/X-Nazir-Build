/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"

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
