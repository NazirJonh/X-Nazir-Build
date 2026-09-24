/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_mesh_maps.hh: the material-owned atlas slots and the per-object bake states,
 * their user counts and their lifecycle (copy, prune).
 */

#include "testing/testing.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_object.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

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

TEST_F(MeshMapsTest, object_state_ensure_find_and_status)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *ob = BKE_object_add_only_object(bmain, OB_EMPTY, "Ob");

  EXPECT_EQ(BKE_mesh_maps_object_state_find(*ob, *ma, MA_MESH_MAP_AO), nullptr);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->material, ma);
  EXPECT_EQ(state->type, MA_MESH_MAP_AO);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_NONE);
  EXPECT_EQ(BKE_mesh_maps_object_state_find(*ob, *ma, MA_MESH_MAP_AO), state);
  /* A different type is a different state. */
  EXPECT_NE(BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_EDGE), state);
  /* A type outside the enum is refused. */
  EXPECT_EQ(BKE_mesh_maps_object_state_ensure(*ob, *ma, int8_t(-1)), nullptr);

  BKE_mesh_maps_object_state_status_set(*state, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);
}

TEST_F(MeshMapsTest, object_state_getter_prunes_a_removed_material)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *ob = BKE_object_add_only_object(bmain, OB_EMPTY, "Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(static_cast<ObjectMeshMapState *>(ob->mesh_map_states.first)->material, ma);

  /* Simulate what remap does when the material is removed: the key is nulled. The getter must not
   * hand back a shadow entry for it, and the prune must drop it. */
  state->material = nullptr;
  EXPECT_EQ(BKE_mesh_maps_object_state_find(*ob, *ma, MA_MESH_MAP_AO), nullptr);
  EXPECT_EQ(ob->mesh_map_states.first, nullptr);
}

TEST_F(MeshMapsTest, object_copy_keeps_states)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *ob = BKE_object_add_only_object(bmain, OB_EMPTY, "Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_NORMAL_WORLD);
  ASSERT_NE(state, nullptr);
  state->hash[0] = 0x1234;
  state->hash[1] = 0x5678;
  BKE_mesh_maps_object_state_status_set(*state, OB_MESH_MAP_STATUS_STALE);

  Object *copy = id_cast<Object *>(BKE_id_copy(bmain, &ob->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy, ob);
  ObjectMeshMapState *copy_state = BKE_mesh_maps_object_state_find(
      *copy, *ma, MA_MESH_MAP_NORMAL_WORLD);
  ASSERT_NE(copy_state, nullptr);
  EXPECT_EQ(copy_state->material, ma);
  EXPECT_EQ(copy_state->hash[0], 0x1234u);
  EXPECT_EQ(copy_state->hash[1], 0x5678u);
  EXPECT_EQ(copy_state->status, OB_MESH_MAP_STATUS_STALE);

  BKE_id_free(bmain, copy);
}

TEST_F(MeshMapsTest, material_slots_free_empties_the_list)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  ASSERT_NE(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_AO), nullptr);
  ASSERT_NE(BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_EDGE), nullptr);
  EXPECT_EQ(ma->mesh_map_slots.first != nullptr, true);
  BKE_mesh_maps_material_slots_free(*ma);
  EXPECT_EQ(ma->mesh_map_slots.first, nullptr);
  EXPECT_EQ(ma->mesh_map_slots.last, nullptr);
}

TEST_F(MeshMapsTest, deleting_the_material_clears_the_object_state)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = BKE_mesh_add(bmain, "ObMesh");
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "Ob");
  ob->data = &mesh->id;
  id_us_plus(&mesh->id);
  ASSERT_TRUE(BKE_object_material_slot_add(bmain, ob));
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBJECT);
  ASSERT_NE(BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO), nullptr);

  /* A real deletion remaps every registered pointer to the removed ID, `IDWALK_CB_NOP` included. */
  BKE_id_delete(bmain, &ma->id);
  EXPECT_EQ(ob->mat[0], nullptr);

  /* The key cannot dangle: either remap already nulled it, or the getter's prune drops it. */
  ObjectMeshMapState *state = static_cast<ObjectMeshMapState *>(ob->mesh_map_states.first);
  if (state != nullptr) {
    EXPECT_EQ(state->material, nullptr);
  }
  BKE_mesh_maps_object_states_prune(*ob);
  EXPECT_EQ(ob->mesh_map_states.first, nullptr);
}

TEST_F(MeshMapsTest, rna_state_material_read_only_and_source_object_poll)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *ob = BKE_object_add_only_object(bmain, OB_EMPTY, "Ob");
  Object *mesh_ob = BKE_object_add_only_object(bmain, OB_MESH, "SrcMesh");
  Object *empty_ob = BKE_object_add_only_object(bmain, OB_EMPTY, "SrcEmpty");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  PointerRNA ob_ptr = RNA_id_pointer_create(&ob->id);
  PropertyRNA *coll_prop = RNA_struct_find_property(&ob_ptr, "mesh_map_states");
  ASSERT_NE(coll_prop, nullptr);
  PointerRNA coll_ptr = PointerRNA_NULL;
  RNA_property_collection_type_get(&ob_ptr, coll_prop, &coll_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);
  PointerRNA state_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_ObjectMeshMapState, state);

  /* The material is the state's key: read-only, states are made by ensure(material, type). */
  PropertyRNA *material_prop = RNA_struct_find_property(&state_ptr, "material");
  ASSERT_NE(material_prop, nullptr);
  EXPECT_FALSE(RNA_property_editable(&state_ptr, material_prop));

  PropertyRNA *source_prop = RNA_struct_find_property(&state_ptr, "source_object");
  ASSERT_NE(source_prop, nullptr);
  EXPECT_TRUE(RNA_property_editable(&state_ptr, source_prop));

  PointerRNA self_ptr = RNA_id_pointer_create(&ob->id);
  PointerRNA mesh_ptr = RNA_id_pointer_create(&mesh_ob->id);
  PointerRNA empty_ptr = RNA_id_pointer_create(&empty_ob->id);
  EXPECT_FALSE(RNA_property_pointer_poll(&state_ptr, source_prop, &self_ptr));
  EXPECT_FALSE(RNA_property_pointer_poll(&state_ptr, source_prop, &empty_ptr));
  EXPECT_TRUE(RNA_property_pointer_poll(&state_ptr, source_prop, &mesh_ptr));

  /* The setter refuses self even when the poll is bypassed, and accepts a mesh object. */
  RNA_property_pointer_set(&state_ptr, source_prop, self_ptr, nullptr);
  EXPECT_EQ(state->source_object, nullptr);
  RNA_property_pointer_set(&state_ptr, source_prop, mesh_ptr, nullptr);
  EXPECT_EQ(state->source_object, mesh_ob);
}

}  // namespace blender::bke::tests
