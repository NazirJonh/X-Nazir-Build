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

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"

#include "BLI_fileops.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"

#include "BKE_appdir.hh"
#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

namespace blender {
/* Internal to blenkernel and not exposed by a public header; the test needs a mesh created in Main
 * with its face offsets allocated, which #BKE_mesh_new_nomain does but cannot attach to Main. */
void BKE_mesh_face_offsets_ensure_alloc(Mesh *mesh);
}  // namespace blender

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

  /** A quad in the XY plane with a UVMap and closed edges, ready to be hashed. */
  Mesh *add_quad_mesh(const char *name)
  {
    Mesh *mesh = BKE_mesh_add(bmain, name);
    mesh->verts_num = 4;
    mesh->edges_num = 4;
    mesh->faces_num = 1;
    mesh->corners_num = 4;
    BKE_mesh_face_offsets_ensure_alloc(mesh);
    bke::mesh_ensure_required_data_layers(*mesh);

    const float3 co[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    mesh->vert_positions_for_write().copy_from(Span<float3>(co, 4));
    MutableSpan<int> offsets = mesh->face_offsets_for_write();
    offsets[0] = 0;
    offsets[1] = 4;
    MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
    MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
    for (int i = 0; i < 4; i++) {
      corner_verts[i] = i;
      corner_edges[i] = i;
    }
    MutableSpan<int2> edges = mesh->edges_for_write();
    edges[0] = {0, 1};
    edges[1] = {1, 2};
    edges[2] = {2, 3};
    edges[3] = {3, 0};

    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_span<float2>(
        "UVMap", bke::AttrDomain::Corner);
    for (int i = 0; i < 4; i++) {
      uv.span[i] = float2(co[i].x, co[i].y);
    }
    uv.finish();
    mesh->uv_maps_active_set("UVMap");

    mesh->tag_topology_changed();
    mesh->tag_positions_changed();
    return mesh;
  }

  Object *add_mesh_object(const char *name, Mesh *mesh)
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, name);
    ob->data = &mesh->id;
    id_us_plus(&mesh->id);
    return ob;
  }

  static bool hashes_equal(const uint32_t a[2], const uint32_t b[2])
  {
    return memcmp(a, b, sizeof(uint32_t) * 2) == 0;
  }

  static void hash_of(const Object &ob, const Material &ma, int8_t type, uint32_t r_hash[2])
  {
    BKE_mesh_maps_object_hash(ob, ma, type, r_hash);
  }

  /** Add a second corner UV layer named `Second`, leaving `UVMap` the active one. */
  static void add_second_uv_layer(Mesh &mesh)
  {
    bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
    bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_span<float2>(
        "Second", bke::AttrDomain::Corner);
    for (int i = 0; i < int(uv.span.size()); i++) {
      uv.span[i] = float2(0.1f * float(i), 0.2f);
    }
    uv.finish();
  }

  static void set_uv_pixel(Mesh &mesh, const char *name, const int index, const float2 value)
  {
    bke::AttributeWriter<float2> uv = mesh.attributes_for_write().lookup_for_write<float2>(name);
    ASSERT_TRUE(uv);
    uv.varray.set(index, value);
    uv.finish();
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

/* A file always has non-mesh objects (camera, light, empty); they have no material count, and the
 * source scan once dereferenced it and crashed the bake commit. */
TEST_F(MeshMapsTest, source_collect_skips_objects_without_materials)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  BKE_object_add_only_object(bmain, OB_EMPTY, "Empty");
  BKE_object_add_only_object(bmain, OB_CAMERA, "Camera");
  Set<const Object *> sources;
  BKE_mesh_maps_source_collect_for_material(*bmain, *ma, sources);
  EXPECT_TRUE(sources.is_empty());
}

/* The settings belong to the Material; they were once registered on the slot struct by mistake, which
 * the UI only caught at draw time. */
TEST_F(MeshMapsTest, rna_mesh_map_settings_is_a_material_property)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  ma->mesh_map_settings.resolution = 512;
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PropertyRNA *settings_prop = RNA_struct_find_property(&ma_ptr, "mesh_map_settings");
  ASSERT_NE(settings_prop, nullptr);
  PointerRNA settings_ptr = RNA_property_pointer_get(&ma_ptr, settings_prop);
  ASSERT_NE(settings_ptr.data, nullptr);
  EXPECT_EQ(RNA_int_get(&settings_ptr, "resolution"), 512);

  BKE_mesh_maps_slot_ensure(*ma, MA_MESH_MAP_AO);
  PointerRNA slot_ptr = RNA_pointer_create_discrete(
      &ma->id, RNA_MaterialMeshMapSlot, ma->mesh_map_slots.first);
  EXPECT_EQ(RNA_struct_find_property(&slot_ptr, "mesh_map_settings"), nullptr);
}

TEST_F(MeshMapsTest, rna_source_ensure_and_high_poly_poll)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *ob = BKE_object_add_only_object(bmain, OB_EMPTY, "Ob");
  Object *mesh_ob = BKE_object_add_only_object(bmain, OB_MESH, "SrcMesh");
  Object *empty_ob = BKE_object_add_only_object(bmain, OB_EMPTY, "SrcEmpty");
  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*ob, *ma);
  ASSERT_NE(source, nullptr);

  PointerRNA ob_ptr = RNA_id_pointer_create(&ob->id);
  PropertyRNA *coll_prop = RNA_struct_find_property(&ob_ptr, "mesh_map_sources");
  ASSERT_NE(coll_prop, nullptr);
  PointerRNA coll_ptr = PointerRNA_NULL;
  RNA_property_collection_type_get(&ob_ptr, coll_prop, &coll_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);
  PointerRNA source_ptr = RNA_pointer_create_with_parent(
      coll_ptr, RNA_ObjectMeshMapSource, source);

  /* The material is the source's key: read-only, sources are made by ensure(material). */
  PropertyRNA *material_prop = RNA_struct_find_property(&source_ptr, "material");
  ASSERT_NE(material_prop, nullptr);
  EXPECT_FALSE(RNA_property_editable(&source_ptr, material_prop));

  PropertyRNA *high_prop = RNA_struct_find_property(&source_ptr, "high_poly");
  ASSERT_NE(high_prop, nullptr);
  EXPECT_TRUE(RNA_property_editable(&source_ptr, high_prop));

  PointerRNA self_ptr = RNA_id_pointer_create(&ob->id);
  PointerRNA mesh_ptr = RNA_id_pointer_create(&mesh_ob->id);
  PointerRNA empty_ptr = RNA_id_pointer_create(&empty_ob->id);
  EXPECT_FALSE(RNA_property_pointer_poll(&source_ptr, high_prop, &self_ptr));
  EXPECT_FALSE(RNA_property_pointer_poll(&source_ptr, high_prop, &empty_ptr));
  EXPECT_TRUE(RNA_property_pointer_poll(&source_ptr, high_prop, &mesh_ptr));

  /* The setter refuses self even when the poll is bypassed, and accepts a mesh object. */
  RNA_property_pointer_set(&source_ptr, high_prop, self_ptr, nullptr);
  EXPECT_EQ(source->high_poly, nullptr);
  RNA_property_pointer_set(&source_ptr, high_prop, mesh_ptr, nullptr);
  EXPECT_EQ(source->high_poly, mesh_ob);

  /* At most one of the object and the collection: setting one clears the other. */
  Collection *coll = BKE_collection_add(bmain, nullptr, "SrcColl");
  PropertyRNA *coll_item_prop = RNA_struct_find_property(&source_ptr, "high_poly_collection");
  ASSERT_NE(coll_item_prop, nullptr);
  PointerRNA coll_value = RNA_id_pointer_create(&coll->id);
  RNA_property_pointer_set(&source_ptr, coll_item_prop, coll_value, nullptr);
  EXPECT_EQ(source->high_poly_collection, coll);
  EXPECT_EQ(source->high_poly, nullptr);

  EXPECT_TRUE(BKE_mesh_maps_source_remove(*ob, *ma));
  EXPECT_EQ(ob->mesh_map_sources.first, nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Phase 1: content hash
 * \{ */

TEST_F(MeshMapsTest, hash_is_stable_across_calls_and_copies)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t first[2], second[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, first);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, second);
  EXPECT_TRUE(hashes_equal(first, second));

  /* A deep copy of the mesh and its object hashes the same: the hash reads content, not identity. */
  Mesh *copy = id_cast<Mesh *>(BKE_id_copy(bmain, &mesh->id));
  ASSERT_NE(copy, nullptr);
  Object *ob_copy = add_mesh_object("ObCopy", copy);
  uint32_t copied[2];
  hash_of(*ob_copy, *ma, MA_MESH_MAP_AO, copied);
  EXPECT_TRUE(hashes_equal(first, copied));
}

TEST_F(MeshMapsTest, hash_changes_when_a_vertex_moves)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t before[2], after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, before);
  mesh->vert_positions_for_write()[0].x += 0.5f;
  mesh->tag_positions_changed();
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, after);
  EXPECT_FALSE(hashes_equal(before, after));
}

TEST_F(MeshMapsTest, hash_changes_when_a_uv_changes)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t before[2], after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, before);
  bke::AttributeWriter<float2> uv = mesh->attributes_for_write().lookup_for_write<float2>("UVMap");
  ASSERT_TRUE(uv);
  uv.varray.set(0, float2(0.9f, 0.9f));
  uv.finish();
  hash_of(*ob, *ma, MA_MESH_MAP_AO, after);
  EXPECT_FALSE(hashes_equal(before, after));
}

TEST_F(MeshMapsTest, hash_uses_the_named_uv_layer_over_the_active_one)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  add_second_uv_layer(*mesh);
  Object *ob = add_mesh_object("Ob", mesh);
  BLI_strncpy(ma->paint_layers_uv_map, "Second", sizeof(ma->paint_layers_uv_map));

  uint32_t before[2], after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, before);

  /* The active layer is not the named one: repainting it must not move the hash. */
  set_uv_pixel(*mesh, "UVMap", 0, float2(0.9f, 0.9f));
  hash_of(*ob, *ma, MA_MESH_MAP_AO, after);
  EXPECT_TRUE(hashes_equal(before, after));

  /* The named layer moves the hash. */
  set_uv_pixel(*mesh, "Second", 0, float2(0.7f, 0.7f));
  hash_of(*ob, *ma, MA_MESH_MAP_AO, after);
  EXPECT_FALSE(hashes_equal(before, after));
}

TEST_F(MeshMapsTest, hash_missing_named_uv_differs_and_reports_missing)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  BLI_strncpy(ma->paint_layers_uv_map, "NoSuchLayer", sizeof(ma->paint_layers_uv_map));
  EXPECT_TRUE(BKE_mesh_maps_object_uv_missing(*ob, *ma));
  uint32_t missing[2], valid[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, missing);

  /* The active UV map is a valid layer; its hash must differ from the missing marker. */
  BLI_strncpy(ma->paint_layers_uv_map, "", sizeof(ma->paint_layers_uv_map));
  EXPECT_FALSE(BKE_mesh_maps_object_uv_missing(*ob, *ma));
  hash_of(*ob, *ma, MA_MESH_MAP_AO, valid);
  EXPECT_FALSE(hashes_equal(missing, valid));
}

TEST_F(MeshMapsTest, hash_changes_when_topology_changes)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t before[2], after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, before);
  /* Same counts, different connectivity: only the corner-to-vertex map moves. */
  mesh->corner_verts_for_write()[0] = 2;
  mesh->tag_topology_changed();
  hash_of(*ob, *ma, MA_MESH_MAP_AO, after);
  EXPECT_FALSE(hashes_equal(before, after));
}

TEST_F(MeshMapsTest, hash_reads_only_its_own_settings)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t ao_before[2], ao_after[2], normal_before[2], normal_after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_before);
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, normal_before);

  /* Edge radius is an Edge setting: it must not move AO or the normal map. */
  ma->mesh_map_settings.edge_radius += 1.0f;
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_after);
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, normal_after);
  EXPECT_TRUE(hashes_equal(ao_before, ao_after));
  EXPECT_TRUE(hashes_equal(normal_before, normal_after));

  /* Samples belong to AO, so changing them moves AO (and not the normal map). */
  ma->mesh_map_settings.samples += 1;
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_after);
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, normal_after);
  EXPECT_FALSE(hashes_equal(ao_before, ao_after));
  EXPECT_TRUE(hashes_equal(normal_before, normal_after));

  /* Edge radius does move the Edge map. */
  uint32_t edge_before[2], edge_after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_EDGE, edge_before);
  ma->mesh_map_settings.edge_radius += 1.0f;
  hash_of(*ob, *ma, MA_MESH_MAP_EDGE, edge_after);
  EXPECT_FALSE(hashes_equal(edge_before, edge_after));
}

TEST_F(MeshMapsTest, hash_transform_moves_only_normal_world)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t world_before[2], world_after[2];
  uint32_t object_before[2], object_after[2];
  uint32_t ao_before[2], ao_after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_WORLD, world_before);
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, object_before);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_before);

  ob->runtime->object_to_world[3][0] = 5.0f;

  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_WORLD, world_after);
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, object_after);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_after);
  EXPECT_FALSE(hashes_equal(world_before, world_after));
  EXPECT_TRUE(hashes_equal(object_before, object_after));
  EXPECT_TRUE(hashes_equal(ao_before, ao_after));
}

TEST_F(MeshMapsTest, hash_material_index_moves_only_id_material)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  ASSERT_TRUE(mesh->attributes_for_write().add<int>(
      "material_index", bke::AttrDomain::Face, bke::AttributeInitConstruct()));

  uint32_t id_before[2], id_after[2], ao_before[2], ao_after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_ID_MATERIAL, id_before);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_before);

  bke::AttributeWriter<int> indices = mesh->attributes_for_write().lookup_for_write<int>(
      "material_index");
  ASSERT_TRUE(indices);
  indices.varray.set(0, 3);
  indices.finish();

  hash_of(*ob, *ma, MA_MESH_MAP_ID_MATERIAL, id_after);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_after);
  EXPECT_FALSE(hashes_equal(id_before, id_after));
  EXPECT_TRUE(hashes_equal(ao_before, ao_after));
}

TEST_F(MeshMapsTest, hash_differs_between_types)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t hashes[MA_MESH_MAP_TYPE_NUM][2];
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    hash_of(*ob, *ma, int8_t(type), hashes[type]);
  }
  for (int a = 0; a < MA_MESH_MAP_TYPE_NUM; a++) {
    for (int b = a + 1; b < MA_MESH_MAP_TYPE_NUM; b++) {
      EXPECT_FALSE(hashes_equal(hashes[a], hashes[b])) << a << " vs " << b;
    }
  }
}

TEST_F(MeshMapsTest, hash_normal_maps_follow_split_normals)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t normal_before[2], normal_after[2], ao_before[2], ao_after[2];
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, normal_before);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_before);

  /* Mark the face sharp: that changes the evaluated normals, but not the AO bake. */
  bke::SpanAttributeWriter<bool> sharp = mesh->attributes_for_write().lookup_or_add_for_write_span<
      bool>("sharp_face", bke::AttrDomain::Face);
  ASSERT_TRUE(sharp);
  sharp.span[0] = true;
  sharp.finish();

  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, normal_after);
  hash_of(*ob, *ma, MA_MESH_MAP_AO, ao_after);
  EXPECT_FALSE(hashes_equal(normal_before, normal_after));
  EXPECT_TRUE(hashes_equal(ao_before, ao_after));
}

TEST_F(MeshMapsTest, hash_is_zero_for_non_mesh_and_invalid_type)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *empty = BKE_object_add_only_object(bmain, OB_EMPTY, "Empty");
  uint32_t hash[2] = {1, 2};
  hash_of(*empty, *ma, MA_MESH_MAP_AO, hash);
  EXPECT_EQ(hash[0], 0u);
  EXPECT_EQ(hash[1], 0u);

  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  hash_of(*ob, *ma, int8_t(MA_MESH_MAP_TYPE_NUM), hash);
  EXPECT_EQ(hash[0], 0u);
  EXPECT_EQ(hash[1], 0u);
  hash_of(*ob, *ma, int8_t(-1), hash);
  EXPECT_EQ(hash[0], 0u);
  EXPECT_EQ(hash[1], 0u);
}

TEST_F(MeshMapsTest, hash_large_mesh_is_fast)
{
  /* No hard threshold: this documents the order of magnitude for the report. */
  constexpr int verts_num = 1000000;
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = BKE_mesh_add(bmain, "Big");
  mesh->verts_num = verts_num;
  bke::mesh_ensure_required_data_layers(*mesh);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (int i = 0; i < verts_num; i++) {
    positions[i] = float3(float(i), float(i % 17), 0.0f);
  }
  mesh->tag_positions_changed();
  Object *ob = add_mesh_object("Ob", mesh);

  uint32_t first[2], second[2];
  const auto start = std::chrono::steady_clock::now();
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, first);
  const auto end = std::chrono::steady_clock::now();
  hash_of(*ob, *ma, MA_MESH_MAP_NORMAL_OBJECT, second);
  const double ms = std::chrono::duration<double, std::milli>(end - start).count();
  std::cout << "[mesh_maps] hash of " << verts_num << " vertices: " << ms << " ms" << std::endl;
  EXPECT_TRUE(hashes_equal(first, second));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Phase 2: bake staleness
 * \{ */

TEST_F(MeshMapsTest, state_refresh_valid_to_stale_and_back)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  uint32_t hash[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, hash);
  BKE_mesh_maps_object_state_mark_baked(*state, hash, 123);
  ASSERT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_TRUE(BKE_mesh_maps_object_state_is_current(*state, *ob, *ma));

  mesh->vert_positions_for_write()[0].x += 0.25f;
  mesh->tag_positions_changed();
  EXPECT_EQ(BKE_mesh_maps_object_state_refresh(*state, *ob, *ma), OB_MESH_MAP_STATUS_STALE);
  EXPECT_FALSE(BKE_mesh_maps_object_state_is_current(*state, *ob, *ma));
  EXPECT_EQ(state->baked_time, 123);

  mesh->vert_positions_for_write()[0].x -= 0.25f;
  mesh->tag_positions_changed();
  EXPECT_EQ(BKE_mesh_maps_object_state_refresh(*state, *ob, *ma), OB_MESH_MAP_STATUS_VALID);
  EXPECT_TRUE(BKE_mesh_maps_object_state_is_current(*state, *ob, *ma));
}

TEST_F(MeshMapsTest, state_refresh_leaves_terminal_states)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  const int8_t terminal[] = {
      OB_MESH_MAP_STATUS_NONE, OB_MESH_MAP_STATUS_BAKING, OB_MESH_MAP_STATUS_ERROR};
  for (const int8_t status : terminal) {
    state->status = status;
    state->hash[0] = 0;
    state->hash[1] = 0;
    EXPECT_EQ(BKE_mesh_maps_object_state_refresh(*state, *ob, *ma), status);
    EXPECT_EQ(state->status, status);
  }
}

TEST_F(MeshMapsTest, mark_baked_sets_valid_hash_and_time)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *ob = BKE_object_add_only_object(bmain, OB_EMPTY, "Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  state->status = OB_MESH_MAP_STATUS_STALE;
  const uint32_t hash[2] = {0x11223344u, 0x55667788u};

  BKE_mesh_maps_object_state_mark_baked(*state, hash, 999);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(state->hash[0], 0x11223344u);
  EXPECT_EQ(state->hash[1], 0x55667788u);
  EXPECT_EQ(state->baked_time, 999);
}

TEST_F(MeshMapsTest, refresh_all_counts_changed_states)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  ObjectMeshMapState *ao = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *edge = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_EDGE);
  ASSERT_NE(ao, nullptr);
  ASSERT_NE(edge, nullptr);

  uint32_t hash[2];
  hash_of(*ob, *ma, MA_MESH_MAP_AO, hash);
  BKE_mesh_maps_object_state_mark_baked(*ao, hash, 1);
  hash_of(*ob, *ma, MA_MESH_MAP_EDGE, hash);
  BKE_mesh_maps_object_state_mark_baked(*edge, hash, 1);
  EXPECT_EQ(BKE_mesh_maps_object_refresh_all(*ob, *ob), 0);

  mesh->vert_positions_for_write()[0].y += 0.5f;
  mesh->tag_positions_changed();
  EXPECT_EQ(BKE_mesh_maps_object_refresh_all(*ob, *ob), 2);
  EXPECT_EQ(ao->status, OB_MESH_MAP_STATUS_STALE);
  EXPECT_EQ(edge->status, OB_MESH_MAP_STATUS_STALE);
  EXPECT_EQ(BKE_mesh_maps_object_refresh_all(*ob, *ob), 0);

  mesh->vert_positions_for_write()[0].y -= 0.5f;
  mesh->tag_positions_changed();
  EXPECT_EQ(BKE_mesh_maps_object_refresh_all(*ob, *ob), 2);
  EXPECT_EQ(ao->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(edge->status, OB_MESH_MAP_STATUS_VALID);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RNA path (depsgraph)
 * \{ */

namespace {

/** Call a registered no-argument function on \a ptr with one Depsgraph argument set. */
void rna_call_with_depsgraph(PointerRNA &ptr,
                             const char *name,
                             Depsgraph *depsgraph,
                             ParameterList &parms,
                             FunctionRNA *&r_func)
{
  r_func = RNA_struct_find_function(ptr.type, name);
  ASSERT_NE(r_func, nullptr);
  RNA_parameter_list_create(&parms, &ptr, r_func);
  Depsgraph *dg = depsgraph;
  RNA_parameter_set_lookup(&parms, "depsgraph", &dg);
}

template<typename T> T rna_get_result(ParameterList &parms, const char *name, const T fallback)
{
  void *ret = nullptr;
  RNA_parameter_get_lookup(&parms, name, &ret);
  return ret != nullptr ? *static_cast<T *>(ret) : fallback;
}

}  // namespace

TEST_F(MeshMapsTest, rna_refresh_is_current_and_compute_hash)
{
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ASSERT_NE(scene, nullptr);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  ASSERT_NE(view_layer, nullptr);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  ASSERT_NE(depsgraph, nullptr);

  Material *ma = BKE_material_add(bmain, "MapsMat");
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  BKE_collection_object_add(bmain, scene->master_collection, ob);
  BKE_scene_graph_update_tagged(depsgraph, bmain);

  Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  ASSERT_NE(ob_eval, nullptr);
  ASSERT_NE(ob_eval, ob);

  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  uint32_t hash[2];
  hash_of(*ob_eval, *ma, MA_MESH_MAP_AO, hash);
  BKE_mesh_maps_object_state_mark_baked(*state, hash, 7);

  PointerRNA ob_ptr = RNA_id_pointer_create(&ob->id);
  PropertyRNA *coll_prop = RNA_struct_find_property(&ob_ptr, "mesh_map_states");
  ASSERT_NE(coll_prop, nullptr);
  PointerRNA coll_ptr = PointerRNA_NULL;
  RNA_property_collection_type_get(&ob_ptr, coll_prop, &coll_ptr);
  PointerRNA state_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_ObjectMeshMapState, state);

  /* is_current(depsgraph) is true right after the bake. */
  {
    ParameterList parms;
    FunctionRNA *func = nullptr;
    rna_call_with_depsgraph(state_ptr, "is_current", depsgraph, parms, func);
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    RNA_function_call(nullptr, &reports, &state_ptr, func, &parms);
    BKE_reports_free(&reports);
    EXPECT_TRUE(rna_get_result<bool>(parms, "result", false));
    RNA_parameter_list_free(&parms);
  }

  /* compute_hash(depsgraph) reproduces the stored hash string. */
  {
    ParameterList parms;
    FunctionRNA *func = nullptr;
    rna_call_with_depsgraph(state_ptr, "compute_hash", depsgraph, parms, func);
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    RNA_function_call(nullptr, &reports, &state_ptr, func, &parms);
    BKE_reports_free(&reports);
    void *ret = nullptr;
    RNA_parameter_get_lookup(&parms, "result", &ret);
    /* A thick-wrapped string output is handed to the C function as a `char *`, so the lookup
     * yields that buffer directly. */
    const std::string computed = ret != nullptr ? std::string(static_cast<const char *>(ret)) :
                                                  std::string();
    RNA_parameter_list_free(&parms);

    char expected[17];
    BLI_snprintf(expected, sizeof(expected), "%08x%08x", state->hash[0], state->hash[1]);
    EXPECT_EQ(computed, expected);
  }

  /* Moving the mesh makes refresh(depsgraph) report STALE. */
  mesh->vert_positions_for_write()[0].x += 1.0f;
  mesh->tag_positions_changed();
  DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  BKE_scene_graph_update_tagged(depsgraph, bmain);
  ASSERT_NE(DEG_get_evaluated(depsgraph, ob), ob);

  {
    ParameterList parms;
    FunctionRNA *func = nullptr;
    rna_call_with_depsgraph(state_ptr, "refresh", depsgraph, parms, func);
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    RNA_function_call(nullptr, &reports, &state_ptr, func, &parms);
    BKE_reports_free(&reports);
    EXPECT_EQ(rna_get_result<int>(parms, "result", -1), int(OB_MESH_MAP_STATUS_STALE));
    RNA_parameter_list_free(&parms);
  }
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_STALE);

  /* refresh_all(depsgraph) on the collection sees the one staleness. */
  {
    FunctionRNA *func = RNA_struct_find_function(coll_ptr.type, "refresh_all");
    ASSERT_NE(func, nullptr);
    ParameterList parms;
    RNA_parameter_list_create(&parms, &coll_ptr, func);
    Depsgraph *dg = depsgraph;
    RNA_parameter_set_lookup(&parms, "depsgraph", &dg);
    ReportList reports;
    BKE_reports_init(&reports, RPT_STORE);
    RNA_function_call(nullptr, &reports, &coll_ptr, func, &parms);
    BKE_reports_free(&reports);
    void *ret = nullptr;
    RNA_parameter_get_lookup(&parms, "result", &ret);
    EXPECT_EQ(ret != nullptr ? *static_cast<int *>(ret) : -1, 0);
    RNA_parameter_list_free(&parms);
  }
}

TEST_F(MeshMapsTest, rna_reports_an_object_outside_the_depsgraph)
{
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  ASSERT_NE(view_layer, nullptr);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  ASSERT_NE(depsgraph, nullptr);

  Material *ma = BKE_material_add(bmain, "MapsMat");
  /* Not linked to the scene, so not part of the depsgraph. */
  Mesh *mesh = add_quad_mesh("Quad");
  Object *ob = add_mesh_object("Ob", mesh);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  PointerRNA ob_ptr = RNA_id_pointer_create(&ob->id);
  PropertyRNA *coll_prop = RNA_struct_find_property(&ob_ptr, "mesh_map_states");
  PointerRNA coll_ptr = PointerRNA_NULL;
  RNA_property_collection_type_get(&ob_ptr, coll_prop, &coll_ptr);
  PointerRNA state_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_ObjectMeshMapState, state);

  ParameterList parms;
  FunctionRNA *func = nullptr;
  rna_call_with_depsgraph(state_ptr, "is_current", depsgraph, parms, func);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  RNA_function_call(nullptr, &reports, &state_ptr, func, &parms);
  EXPECT_TRUE(BKE_reports_contain(&reports, RPT_ERROR));
  BKE_reports_free(&reports);
  EXPECT_FALSE(rna_get_result<bool>(parms, "result", true));
  RNA_parameter_list_free(&parms);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name High-poly sources
 * \{ */

TEST_F(MeshMapsTest, source_ensure_find_remove_and_prune)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Material *other = BKE_material_add(bmain, "OtherMat");
  Object *ob = add_mesh_object("Low", add_quad_mesh("LowMesh"));

  EXPECT_EQ(BKE_mesh_maps_source_find(*ob, *ma), nullptr);
  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*ob, *ma);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->material, ma);
  EXPECT_EQ(BKE_mesh_maps_source_find(*ob, *ma), source);
  EXPECT_EQ(BKE_mesh_maps_source_find(*ob, *other), nullptr);

  ObjectMeshMapSource *other_source = BKE_mesh_maps_source_ensure(*ob, *other);
  ASSERT_NE(other_source, nullptr);
  EXPECT_NE(other_source, source);

  /* A record whose material was removed is dropped by the next find/prune. */
  source->material = nullptr;
  EXPECT_EQ(BKE_mesh_maps_sources_prune(*ob), 1);
  EXPECT_EQ(BKE_mesh_maps_source_find(*ob, *ma), nullptr);

  EXPECT_TRUE(BKE_mesh_maps_source_remove(*ob, *other));
  EXPECT_EQ(ob->mesh_map_sources.first, nullptr);
  EXPECT_FALSE(BKE_mesh_maps_source_remove(*ob, *other));
}

TEST_F(MeshMapsTest, source_resolve_single_collection_and_cage)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *low = add_mesh_object("Low", add_quad_mesh("LowMesh"));
  Object *high = add_mesh_object("High", add_quad_mesh("HighMesh"));
  Object *cage = add_mesh_object("Cage", add_quad_mesh("CageMesh"));
  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);

  Vector<Object *> objects;
  Object *out_cage = nullptr;

  source->high_poly = high;
  source->cage = cage;
  BKE_mesh_maps_source_resolve(*low, *ma, objects, &out_cage);
  ASSERT_EQ(objects.size(), 1u);
  EXPECT_EQ(objects[0], high);
  EXPECT_EQ(out_cage, cage);

  /* The low-poly itself and the cage are never sources. */
  source->high_poly = low;
  BKE_mesh_maps_source_resolve(*low, *ma, objects, &out_cage);
  EXPECT_TRUE(objects.is_empty());
  source->high_poly = cage;
  BKE_mesh_maps_source_resolve(*low, *ma, objects, &out_cage);
  EXPECT_TRUE(objects.is_empty());

  /* A collection recurses, keeps only mesh objects, and still excludes the low-poly and cage. */
  Collection *coll = BKE_collection_add(bmain, nullptr, "Coll");
  BKE_collection_object_add(bmain, coll, high);
  BKE_collection_object_add(bmain, coll, low);
  BKE_collection_object_add(bmain, coll, cage);
  BKE_collection_object_add(bmain, coll, BKE_object_add_only_object(bmain, OB_EMPTY, "Empty"));
  Object *high2 = add_mesh_object("High2", add_quad_mesh("High2Mesh"));
  Collection *child = BKE_collection_add(bmain, coll, "Child");
  BKE_collection_object_add(bmain, child, high2);

  source->high_poly = nullptr;
  source->high_poly_collection = coll;
  BKE_mesh_maps_source_resolve(*low, *ma, objects, &out_cage);
  ASSERT_EQ(objects.size(), 2u);
  EXPECT_TRUE(objects.contains(high));
  EXPECT_TRUE(objects.contains(high2));
}

TEST_F(MeshMapsTest, source_hash_tracks_geometry_transform_cage_and_extrusion)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *low = add_mesh_object("Low", add_quad_mesh("LowMesh"));
  Object *high = add_mesh_object("High", add_quad_mesh("HighMesh"));

  uint32_t base[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, base);

  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  uint32_t no_source[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, no_source);
  /* Merely having a source record is part of the content. */
  EXPECT_FALSE(hashes_equal(base, no_source));

  source->high_poly = high;
  uint32_t with_source[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, with_source);
  EXPECT_FALSE(hashes_equal(no_source, with_source));

  /* High-poly geometry moves the hash. */
  id_cast<Mesh *>(high->data)->vert_positions_for_write().first() += float3(0.5f, 0.0f, 0.0f);
  id_cast<Mesh *>(high->data)->tag_positions_changed();
  uint32_t moved_mesh[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, moved_mesh);
  EXPECT_FALSE(hashes_equal(with_source, moved_mesh));

  /* A cage, and the auto-cage controls. */
  Object *cage = add_mesh_object("Cage", add_quad_mesh("CageMesh"));
  source->cage = cage;
  uint32_t with_cage[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, with_cage);
  EXPECT_FALSE(hashes_equal(moved_mesh, with_cage));

  source->cage_extrusion = 0.5f;
  uint32_t with_extrusion[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, with_extrusion);
  EXPECT_FALSE(hashes_equal(with_cage, with_extrusion));

  /* Stable when nothing changes. */
  uint32_t repeat[2];
  hash_of(*low, *ma, MA_MESH_MAP_AO, repeat);
  EXPECT_TRUE(hashes_equal(with_extrusion, repeat));
}

TEST_F(MeshMapsTest, source_hash_tracks_relative_transform)
{
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ASSERT_NE(scene, nullptr);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  ASSERT_NE(view_layer, nullptr);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  ASSERT_NE(depsgraph, nullptr);

  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *low = add_mesh_object("Low", add_quad_mesh("LowMesh"));
  Object *high = add_mesh_object("High", add_quad_mesh("HighMesh"));
  BKE_collection_object_add(bmain, scene->master_collection, low);
  BKE_collection_object_add(bmain, scene->master_collection, high);
  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  source->high_poly = high;

  BKE_scene_graph_update_tagged(depsgraph, bmain);
  Object *low_eval = DEG_get_evaluated(depsgraph, low);
  ASSERT_NE(low_eval, nullptr);
  ASSERT_NE(low_eval, low);
  uint32_t before[2];
  hash_of(*low_eval, *ma, MA_MESH_MAP_AO, before);

  /* Moving the high-poly changes the relative transform the rays are cast in. */
  high->loc[0] += 2.0f;
  DEG_id_tag_update(&high->id, ID_RECALC_TRANSFORM);
  BKE_scene_graph_update_tagged(depsgraph, bmain);
  low_eval = DEG_get_evaluated(depsgraph, low);
  uint32_t after[2];
  hash_of(*low_eval, *ma, MA_MESH_MAP_AO, after);
  EXPECT_FALSE(hashes_equal(before, after));
}

TEST_F(MeshMapsTest, source_lifecycle_copy_and_remap)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *low = add_mesh_object("Low", add_quad_mesh("LowMesh"));
  Object *high = add_mesh_object("High", add_quad_mesh("HighMesh"));
  Object *cage = add_mesh_object("Cage", add_quad_mesh("CageMesh"));
  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  source->high_poly = high;
  source->cage = cage;
  source->cage_extrusion = 0.25f;
  source->max_ray_distance = 1.5f;

  Object *copy = id_cast<Object *>(BKE_id_copy(bmain, &low->id));
  ASSERT_NE(copy, nullptr);
  ObjectMeshMapSource *copy_source = BKE_mesh_maps_source_find(*copy, *ma);
  ASSERT_NE(copy_source, nullptr);
  EXPECT_EQ(copy_source->high_poly, high);
  EXPECT_EQ(copy_source->cage, cage);
  EXPECT_FLOAT_EQ(copy_source->cage_extrusion, 0.25f);
  EXPECT_FLOAT_EQ(copy_source->max_ray_distance, 1.5f);

  /* Deleting a referenced object nulls the pointer in both the original and its copy. */
  BKE_id_delete(bmain, &high->id);
  EXPECT_EQ(source->high_poly, nullptr);
  EXPECT_EQ(copy_source->high_poly, nullptr);
  BKE_id_delete(bmain, &cage->id);
  EXPECT_EQ(source->cage, nullptr);
  EXPECT_EQ(copy_source->cage, nullptr);
}

TEST_F(MeshMapsTest, sources_round_trip_through_a_blend_file)
{
  Material *ma = BKE_material_add(bmain, "MapsMat");
  Object *low = add_mesh_object("Low", add_quad_mesh("LowMesh"));
  Object *high = add_mesh_object("High", add_quad_mesh("HighMesh"));
  Object *cage = add_mesh_object("Cage", add_quad_mesh("CageMesh"));
  /* Link the objects so they have a user and are written to the file. */
  Collection *collection = BKE_collection_add(bmain, nullptr, "SourceColl");
  BKE_collection_object_add(bmain, collection, low);
  BKE_collection_object_add(bmain, collection, high);
  BKE_collection_object_add(bmain, collection, cage);
  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  source->high_poly = high;
  source->cage = cage;
  source->cage_extrusion = 0.25f;
  source->max_ray_distance = 2.0f;

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "mesh_maps_sources_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *loaded_ma = nullptr;
  Object *loaded_low = nullptr;
  Object *loaded_high = nullptr;
  Object *loaded_cage = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "MapsMat")) {
      loaded_ma = &candidate;
    }
  }
  for (Object &candidate : bfd->main->objects) {
    if (STREQ(candidate.id.name + 2, "Low")) {
      loaded_low = &candidate;
    }
    else if (STREQ(candidate.id.name + 2, "High")) {
      loaded_high = &candidate;
    }
    else if (STREQ(candidate.id.name + 2, "Cage")) {
      loaded_cage = &candidate;
    }
  }
  ASSERT_NE(loaded_ma, nullptr);
  ASSERT_NE(loaded_low, nullptr);
  ASSERT_NE(loaded_high, nullptr);
  ASSERT_NE(loaded_cage, nullptr);

  const ObjectMeshMapSource *loaded = BKE_mesh_maps_source_find(*loaded_low, *loaded_ma);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->high_poly, loaded_high);
  EXPECT_EQ(loaded->cage, loaded_cage);
  EXPECT_FLOAT_EQ(loaded->cage_extrusion, 0.25f);
  EXPECT_FLOAT_EQ(loaded->max_ray_distance, 2.0f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

/** \} */

}  // namespace blender::bke::tests
