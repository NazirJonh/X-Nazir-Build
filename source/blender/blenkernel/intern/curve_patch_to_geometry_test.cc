/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

namespace blender::bke::tests {

static CurvesGeometry make_poly_control_curve(const int count)
{
  CurvesGeometry curves(count, 1);
  curves.offsets_for_write().copy_from({0, count});
  curves.fill_curve_types(CURVE_TYPE_POLY);
  MutableSpan<float3> positions = curves.positions_for_write();
  for (const int i : positions.index_range()) {
    positions[i] = float3(float(i), 0.0f, 0.0f);
  }
  curves.radius_for_write().fill(1.0f);
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

TEST(paint_curve_patch_to_geometry, empty_spline_yields_null_mesh)
{
  CurvePatchGeometry geometry;
  EXPECT_EQ(curve_patch_geometry_to_mesh(geometry, make_params(), false, 0.0f, 0.0f), nullptr);
}

TEST(paint_curve_patch_to_geometry, ribbon_mesh_is_owned_nomain_with_uvs)
{
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(make_poly_control_curve(3), make_params(), {}, geometry);
  ASSERT_FALSE(geometry.spline.is_empty());

  Mesh *mesh = curve_patch_geometry_to_mesh(geometry, make_params(), false, 0.0f, 0.0f);
  ASSERT_NE(mesh, nullptr);
  EXPECT_GT(mesh->verts_num, 0);
  EXPECT_GT(mesh->faces_num, 0);
  EXPECT_TRUE(mesh->attributes().contains("UVMap"));

  /* Caller owns the nomain ID: freeing it must not depend on `geometry` still being alive. */
  geometry.clear();
  EXPECT_GT(mesh->verts_num, 0);
  BKE_id_free(nullptr, mesh);
}

TEST(paint_curve_patch_to_geometry, ribbon_mode_yields_null_stamp_points)
{
  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(make_poly_control_curve(3), make_params(), {}, geometry);
  EXPECT_TRUE(geometry.stamps.is_empty());
  EXPECT_EQ(curve_patch_geometry_to_stamp_points(geometry), nullptr);
}

TEST(paint_curve_patch_to_geometry, stamps_pointcloud_is_owned_nomain)
{
  CurvePatchParams params = make_params();
  params.stamp_mode = CurvePatchStampMode::Stamps;
  params.spacing_frac = 0.5f;

  CurvePatchGeometry geometry;
  curve_patch_build_from_control_curve(make_poly_control_curve(4), params, {}, geometry);
  ASSERT_FALSE(geometry.stamps.is_empty());

  PointCloud *points = curve_patch_geometry_to_stamp_points(geometry);
  ASSERT_NE(points, nullptr);
  EXPECT_EQ(points->totpoint, int(geometry.stamps.size()));

  geometry.clear();
  EXPECT_GT(points->totpoint, 0);
  BKE_id_free(nullptr, points);
}

}  // namespace blender::bke::tests
