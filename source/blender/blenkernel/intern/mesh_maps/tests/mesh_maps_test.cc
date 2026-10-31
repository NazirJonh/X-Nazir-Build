/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_mesh_maps.hh: the material-owned atlas slots, their user counts and their
 * lifecycle (copy, free).
 */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh_maps.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

namespace blender::bke::tests {

class MeshMapsTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
  }
  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }
};

TEST_F(MeshMapsTest, slot_ensure_is_idempotent_and_validates_type)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  EXPECT_EQ(BKE_mesh_maps_slot_find(*ma, MA_MESH_MAP_AO), nullptr);

  MaterialMeshMapSlot *ao = BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_AO);
  ASSERT_NE(ao, nullptr);
  EXPECT_EQ(ao->type, MA_MESH_MAP_AO);
  EXPECT_EQ(ao->image, nullptr);
  /* Ensuring the same type returns the very same slot. */
  EXPECT_EQ(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_AO), ao);
  EXPECT_EQ(BKE_mesh_maps_slot_find(*ma, MA_MESH_MAP_AO), ao);

  /* A reserved type still has a DNA value, so it is valid. */
  EXPECT_NE(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_POSITION), nullptr);
  /* A type outside the enum is refused and creates nothing. */
  EXPECT_EQ(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_TYPE_NUM), nullptr);
  EXPECT_EQ(BKE_mesh_maps_slot_ensure(*ma, int8_t(-1)), nullptr);
}

TEST_F(MeshMapsTest, slot_image_set_maintains_user_counts)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Image *first = add_image("First");
  Image *second = add_image("Second");
  const int first_base = first->id.us;
  const int second_base = second->id.us;

  EXPECT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, first));
  EXPECT_EQ(BKE_mesh_maps_slot_find(*ma, MA_MESH_MAP_AO)->image, first);
  EXPECT_EQ(first->id.us, first_base + 1);

  /* Setting the same image again does not add a second reference. */
  EXPECT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, first));
  EXPECT_EQ(first->id.us, first_base + 1);

  /* Replacing releases the old and takes the new. */
  EXPECT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, second));
  EXPECT_EQ(first->id.us, first_base);
  EXPECT_EQ(second->id.us, second_base + 1);

  /* A second slot may share the same image; each slot is one user. */
  EXPECT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_CURVATURE, second));
  EXPECT_EQ(second->id.us, second_base + 2);

  /* Detaching releases the reference. */
  EXPECT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_CURVATURE, nullptr));
  EXPECT_EQ(second->id.us, second_base + 1);

  /* An invalid type is refused and changes nothing. */
  EXPECT_FALSE(BKE_mesh_maps_slot_image_set(*ma, int8_t(-1), first));
}

TEST_F(MeshMapsTest, material_copy_keeps_slots_and_counts)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Image *image = add_image("Atlas");
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, image));
  ma->mesh_map_settings.resolution = 4096;
  const int us_before = image->id.us;

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy, ma);
  const MaterialMeshMapSlot *copy_slot = BKE_mesh_maps_slot_find(*copy, MA_MESH_MAP_AO);
  ASSERT_NE(copy_slot, nullptr);
  EXPECT_EQ(copy_slot->image, image);
  EXPECT_EQ(copy->mesh_map_settings.resolution, 4096);
  /* The generic copy machinery adds one reference for the copy. */
  EXPECT_EQ(image->id.us, us_before + 1);

  BKE_id_free(bmain, copy);
  EXPECT_EQ(image->id.us, us_before);
}

TEST_F(MeshMapsTest, material_slots_free_empties_the_list)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  ASSERT_NE(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_AO), nullptr);
  ASSERT_NE(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_EDGE), nullptr);
  EXPECT_NE(ma->mesh_map_slots.first, nullptr);
  BKE_mesh_maps_material_slots_free(*ma);
  EXPECT_EQ(ma->mesh_map_slots.first, nullptr);
  EXPECT_EQ(ma->mesh_map_slots.last, nullptr);
}

}  // namespace blender::bke::tests
