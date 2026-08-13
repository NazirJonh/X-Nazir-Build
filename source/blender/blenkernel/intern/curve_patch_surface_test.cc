/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BKE_curve_patch.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

namespace blender::bke::tests {

/**
 * Six vertices, one triangle on the first three. The last three sit far away so an in-place
 * rewrite of `.corner_vert` can retarget the live mesh without changing `verts_num` / `faces_num`
 * / `corners_num`.
 */
static Mesh *make_origin_triangle_with_far_spare_verts()
{
  Mesh *mesh = BKE_mesh_new_nomain(6, 0, 1, 3);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  positions[0] = float3(0.0f, 0.0f, 0.0f);
  positions[1] = float3(2.0f, 0.0f, 0.0f);
  positions[2] = float3(0.0f, 2.0f, 0.0f);
  positions[3] = float3(100.0f, 100.0f, 0.0f);
  positions[4] = float3(102.0f, 100.0f, 0.0f);
  positions[5] = float3(100.0f, 102.0f, 0.0f);

  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  offsets[0] = 0;
  offsets[1] = 3;

  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  corner_verts[0] = 0;
  corner_verts[1] = 1;
  corner_verts[2] = 2;

  mesh->tag_topology_changed();
  mesh->tag_positions_changed();
  return mesh;
}

/**
 * Production change that fails this test: building the snapshot BVH from `mesh.faces()` /
 * `mesh.corner_verts()` / `mesh.corner_tris()` so the nearest callback reads the live mesh. After
 * an in-place rewrite of `.corner_vert` (same counts, new connectivity) shrinkwrap follows the
 * live triangle and misses the snapshotted origin face.
 */
TEST(paint_curve_patch_surface, shrinkwrap_uses_snapshotted_topology_not_live_mesh)
{
  Mesh *mesh = make_origin_triangle_with_far_spare_verts();

  CurvePatchSurfaceSnapshot snapshot;
  ASSERT_TRUE(curve_patch_surface_snapshot_build(*mesh, snapshot));
  ASSERT_TRUE(snapshot.ready);

  /* Same counts, different connectivity: the class of edit the session's `verts_num` check cannot
   * see. Do not tag topology here -- that would reallocate the corner-tri cache and turn this into
   * a use-after-free rather than a wrong hit. */
  MutableSpan<int> live_corners = mesh->corner_verts_for_write();
  live_corners[0] = 3;
  live_corners[1] = 4;
  live_corners[2] = 5;

  Array<float3> query = {float3(0.4f, 0.4f, 1.0f)};
  Array<float3> normals = {float3(0.0f)};
  curve_patch_surface_shrinkwrap(snapshot, 10.0f, query, normals);

  EXPECT_NEAR(query[0].x, 0.4f, 1e-4f);
  EXPECT_NEAR(query[0].y, 0.4f, 1e-4f);
  EXPECT_NEAR(query[0].z, 0.0f, 1e-4f);
  EXPECT_NEAR(normals[0].z, 1.0f, 1e-3f);

  snapshot.clear();
  BKE_id_free(nullptr, mesh);
}

TEST(paint_curve_patch_surface, fill_invalid_normals_all_zero_uses_fallback)
{
  Array<float3> normals = {float3(0.0f), float3(0.0f), float3(0.0f)};
  const float3 fallback(0.0f, 1.0f, 0.0f);
  curve_patch_surface_fill_invalid_normals(normals, fallback);
  for (const float3 &n : normals) {
    EXPECT_EQ(n, fallback);
  }
}

TEST(paint_curve_patch_surface, fill_invalid_normals_copies_first_and_last_valid)
{
  Array<float3> normals = {float3(0.0f), float3(0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f)};
  curve_patch_surface_fill_invalid_normals(normals, float3(1.0f, 0.0f, 0.0f));
  EXPECT_NEAR(normals[0].z, 1.0f, 1e-5f);
  EXPECT_NEAR(normals[1].z, 1.0f, 1e-5f);
  EXPECT_NEAR(normals[2].z, 1.0f, 1e-5f);
  EXPECT_NEAR(normals[3].z, 1.0f, 1e-5f);
}

TEST(paint_curve_patch_surface, fill_invalid_normals_interpolates_a_gap)
{
  Array<float3> normals = {float3(1.0f, 0.0f, 0.0f), float3(0.0f), float3(0.0f, 1.0f, 0.0f)};
  curve_patch_surface_fill_invalid_normals(normals, float3(0.0f, 0.0f, 1.0f));
  EXPECT_NEAR(normals[1].x, 0.5f * float(M_SQRT2), 1e-4f);
  EXPECT_NEAR(normals[1].y, 0.5f * float(M_SQRT2), 1e-4f);
}

TEST(paint_curve_patch_surface, fill_invalid_normals_opposed_neighbours_keep_previous)
{
  /* Midpoint of (0,0,1) and (0,0,-1) is zero; the fill must stay finite. */
  Array<float3> normals = {float3(0.0f, 0.0f, 1.0f), float3(0.0f), float3(0.0f, 0.0f, -1.0f)};
  curve_patch_surface_fill_invalid_normals(normals, float3(1.0f, 0.0f, 0.0f));
  EXPECT_NEAR(normals[1].z, 1.0f, 1e-5f);
}

}  // namespace blender::bke::tests
