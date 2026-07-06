/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector_types.hh"

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"

#include "DNA_mesh_types.h"

#include "ED_sculpt.hh"

#include "GEO_mesh_primitive_cuboid.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::face_set::tests {

static void set_first_face_set_id(Mesh &mesh, const int id)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<int> face_sets = attributes.lookup_or_add_for_write_span<int>(
      ".sculpt_face_set", bke::AttrDomain::Face);
  face_sets.span[0] = id;
  face_sets.finish();
}

class FindSharedNextAvailableIdTest : public bke::BlenderGTestBase {
 public:
  Mesh *mesh_a;
  Mesh *mesh_b;

  void SetUp() override
  {
    mesh_a = geometry::create_cuboid_mesh(float3(1, 1, 1), 2, 2, 2);
    mesh_b = geometry::create_cuboid_mesh(float3(1, 1, 1), 2, 2, 2);
  }

  void TearDown() override
  {
    BKE_id_free(nullptr, mesh_a);
    BKE_id_free(nullptr, mesh_b);
  }
};

TEST_F(FindSharedNextAvailableIdTest, MaxAcrossMeshes)
{
  ASSERT_GT(mesh_a->faces_num, 0);
  ASSERT_GT(mesh_b->faces_num, 0);

  set_first_face_set_id(*mesh_a, 3);
  set_first_face_set_id(*mesh_b, 7);

  EXPECT_EQ(find_next_available_id(*mesh_a), 4);
  EXPECT_EQ(find_next_available_id(*mesh_b), 8);

  const Mesh *meshes[2] = {mesh_a, mesh_b};
  EXPECT_EQ(find_shared_next_available_id(Span(meshes)), 8);

  const Mesh *meshes_swapped[2] = {mesh_b, mesh_a};
  EXPECT_EQ(find_shared_next_available_id(Span(meshes_swapped)), 8);
}

TEST_F(FindSharedNextAvailableIdTest, SingleMeshMatchesFindNextAvailableId)
{
  set_first_face_set_id(*mesh_a, 5);
  const Mesh *meshes[1] = {mesh_a};
  EXPECT_EQ(find_shared_next_available_id(Span(meshes)), find_next_available_id(*mesh_a));
}

TEST_F(FindSharedNextAvailableIdTest, EmptySpanReturnsZero)
{
  EXPECT_EQ(find_shared_next_available_id(Span<const Mesh *>()), 0);
}

}  // namespace blender::ed::sculpt_paint::face_set::tests
