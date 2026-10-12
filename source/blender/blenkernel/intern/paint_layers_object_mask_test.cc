/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tests for #BKE_paint_layers_object_mask.hh.
 */

#include "testing/testing.h"

#include <cctype>
#include <string>

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_layers_target.hh"

#include "paint_layers_intern.hh"

namespace blender::bke::tests {

class PaintLayersObjectMaskTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;
  Object *ob = nullptr;
  Mesh *me = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "Layered");
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    me = BKE_mesh_add(bmain, "Me");
    ob = BKE_object_add_only_object(bmain, OB_MESH, "Ob");
    ob->data = &me->id;
    id_us_plus(&me->id);
    BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBDATA);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  MaterialPaintLayer *add_layer(const char *name)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    return layer;
  }

  Object *add_object_with(Material *mat)
  {
    Mesh *mesh = BKE_mesh_add(bmain, "TwinMesh");
    Object *new_ob = BKE_object_add_only_object(bmain, OB_MESH, "TwinOb");
    new_ob->data = &mesh->id;
    id_us_plus(&mesh->id);
    BKE_object_material_assign(bmain, new_ob, mat, 1, BKE_MAT_ASSIGN_OBDATA);
    return new_ob;
  }

  Image *add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  /** Give \a layer a map and a saved bake whose hash matches, so it is substituted until gated. */
  void bake_setup(MaterialPaintLayer &layer)
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, &layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    ASSERT_NE(record, nullptr);
    record->image = add_image("Map");
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }

  /** Store a bake on \a layer whose hash matches the current content. */
  void bake_store(MaterialPaintLayer &layer)
  {
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
    ASSERT_NE(bake, nullptr);
    bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("Baked");
    bake->coverage = add_image("Coverage");
    uint32_t hash[2];
    BKE_paint_layers_bake_hash(*ma, layer, hash);
    bake->hash[0] = hash[0];
    bake->hash[1] = hash[1];
  }
};

TEST_F(PaintLayersObjectMaskTest, key_is_short_lowercase_hex)
{
  const bUUID a = BLI_uuid_generate_random();
  const bUUID b = BLI_uuid_generate_random();
  const auto key = BKE_paint_layer_object_mask_key(a, b);
  EXPECT_EQ(strlen(key.data()), size_t(PAINT_LAYER_OBJECT_MASK_KEY_LEN));
  for (int i = 0; i < PAINT_LAYER_OBJECT_MASK_KEY_LEN; i++) {
    EXPECT_TRUE(isxdigit(key[i]) && !isupper(key[i]));
  }
  /* Swapping the two uids is another row of another material: the key must differ. */
  EXPECT_STRNE(key.data(), BKE_paint_layer_object_mask_key(b, a).data());
}

TEST_F(PaintLayersObjectMaskTest, attribute_path_fits_gpu_uniform_attribute_name)
{
  /* `GPUUniformAttr::name` holds 67 characters; a longer path is cut and never resolves. */
  MaterialPaintLayer *layer = add_layer("A");
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_LE(BKE_paint_layer_object_mask_attribute_path(*ma, *layer).size(), size_t(67));
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_LE(BKE_paint_layer_object_mask_attribute_path(*ma, *layer).size(), size_t(67));
}

TEST_F(PaintLayersObjectMaskTest, no_record_means_visible)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

TEST_F(PaintLayersObjectMaskTest, mesh_hide_then_show_removes_record)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
  const ObjectPaintLayerMask *rec = static_cast<const ObjectPaintLayerMask *>(
      ob->paint_layer_masks.first);
  ASSERT_NE(rec, nullptr);
  EXPECT_EQ(rec->excluded_value, 1.0f);
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, true));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

TEST_F(PaintLayersObjectMaskTest, modes_are_independent)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  /* Mesh flag is not read in UV Tiles mode. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1001));
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, tile_bits_edges)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1001, false));
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1024, false));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1025, false));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1001));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1024));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1002));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1025));
  EXPECT_EQ(BKE_paint_layer_object_mask_excluded_tiles(*ob, *ma, *layer),
            (1u << 0) | (1u << 23));
  const ObjectPaintLayerMask *rec = static_cast<const ObjectPaintLayerMask *>(
      ob->paint_layer_masks.first);
  EXPECT_EQ(rec->excluded_tiles_value, float((1u << 0) | (1u << 23)));
  for (int tile = 1001; tile <= 1024; tile++) {
    BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, tile, false);
  }
  EXPECT_EQ(BKE_paint_layer_object_mask_excluded_tiles(*ob, *ma, *layer), 0xFFFFFFu);
}

TEST_F(PaintLayersObjectMaskTest, visible_nested_folder_parent_mesh_child_tiles)
{
  MaterialPaintLayer *child = add_layer("Child");
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>{child});
  ASSERT_NE(folder, nullptr);
  folder->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  folder->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  child->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  child->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *child, 1002, false);
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *child, 1001));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *child, 1002));
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *folder, false);
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *child, 1001));
  EXPECT_TRUE(BKE_paint_layers_object_mask_blocks_bake(*folder));
  EXPECT_FALSE(BKE_paint_layers_object_mask_blocks_bake(*child));
}

TEST_F(PaintLayersObjectMaskTest, two_materials_same_marker_are_independent)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  MaterialPaintLayer *copy_layer = static_cast<MaterialPaintLayer *>(copy->paint_layers.first);
  ASSERT_TRUE(BLI_uuid_equal(copy_layer->marker, layer->marker));
  EXPECT_FALSE(BLI_uuid_equal(copy->paint_layers_owner_uid, ma->paint_layers_owner_uid));
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *copy, *copy_layer, 0));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, copy_with_nil_uid_or_no_tree_gets_fresh_uid)
{
  add_layer("A");
  ma->paint_layers_owner_uid = {};
  ASSERT_EQ(ma->paint_layers_tree, nullptr);
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  EXPECT_FALSE(BLI_uuid_is_nil(copy->paint_layers_owner_uid));
  EXPECT_FALSE(BLI_uuid_equal(copy->paint_layers_owner_uid, ma->paint_layers_owner_uid));
}

TEST_F(PaintLayersObjectMaskTest, ensure_unique_regenerates_and_remaps)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  /* Simulate a second append: same uid, its own object. */
  Material *twin = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  twin->paint_layers_owner_uid = ma->paint_layers_owner_uid;
  Object *ob2 = add_object_with(twin);
  MaterialPaintLayer *twin_layer = static_cast<MaterialPaintLayer *>(twin->paint_layers.first);
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob2, *twin, *twin_layer, false);

  BKE_paint_layers_owner_uid_ensure_unique(*bmain);
  EXPECT_FALSE(BLI_uuid_equal(twin->paint_layers_owner_uid, ma->paint_layers_owner_uid));
  /* Each object kept its own record under its own material. */
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob2, *twin, *twin_layer, 0));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *twin, *twin_layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, cleanup_drops_dead_records)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  BKE_paint_layers_remove(*ma, layer);
  BKE_paint_layer_object_masks_cleanup(*bmain, *ob);
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

TEST_F(PaintLayersObjectMaskTest, object_copy_copies_records)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  Object *dup = id_cast<Object *>(BKE_id_copy(bmain, &ob->id));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*dup, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, folder_with_masked_child_is_not_substituted)
{
  MaterialPaintLayer *child = add_layer("Child");
  bake_setup(*child);
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>{child});
  ASSERT_NE(folder, nullptr);
  bake_store(*folder);
  Image *baked = nullptr;
  ASSERT_TRUE(BKE_paint_layers_bake_substitute(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));

  child->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  child->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  /* The mask type must not move the bake hash, otherwise this checks staleness, not the gate. */
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  EXPECT_FALSE(BKE_paint_layers_bake_substitute(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
}

TEST_F(PaintLayersObjectMaskTest, masked_row_itself_may_stay_substituted)
{
  MaterialPaintLayer *row = add_layer("Row");
  bake_setup(*row);
  bake_store(*row);
  row->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  row->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  Image *baked = nullptr;
  EXPECT_TRUE(BKE_paint_layers_bake_substitute(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
}

TEST_F(PaintLayersObjectMaskTest, stroke_refused_on_hidden_object)
{
  MaterialPaintLayer *layer = add_layer("A");
  BKE_paint_layers_active_set(*ma, layer->marker);
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_EQ(BKE_paint_layers_target_refusal(target), nullptr);
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_NE(BKE_paint_layers_target_refusal(target), nullptr);
  EXPECT_EQ(target.excluded_tiles, 0u);
}

TEST_F(PaintLayersObjectMaskTest, target_carries_excluded_tiles)
{
  MaterialPaintLayer *layer = add_layer("A");
  BKE_paint_layers_active_set(*ma, layer->marker);
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1002, false);
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
  EXPECT_EQ(BKE_paint_layers_target_refusal(target), nullptr);
  EXPECT_EQ(target.excluded_tiles, 1u << 1);
}

/** True when \a tree or any group nested in it has an Object-type Attribute node reading \a path. */
static bool tree_has_object_attribute(const bNodeTree &tree, const std::string &path)
{
  for (const bNode *node : tree.all_nodes()) {
    if (node->type_legacy == SH_NODE_ATTRIBUTE) {
      const NodeShaderAttribute *data = static_cast<const NodeShaderAttribute *>(node->storage);
      if (data->type == SHD_ATTRIBUTE_OBJECT && STREQ(data->name, path.c_str())) {
        return true;
      }
    }
    else if (node->type_legacy == NODE_GROUP && node->id != nullptr &&
             tree_has_object_attribute(*id_cast<const bNodeTree *>(node->id), path))
    {
      return true;
    }
  }
  return false;
}

TEST_F(PaintLayersObjectMaskTest, mesh_mask_adds_object_attribute_node)
{
  MaterialPaintLayer *layer = add_layer("A");
  /* A row without content is not built, so give it a map. */
  bake_setup(*layer);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t hash_none = paint_layers_layer_topology_hash(*ma, *layer, Span<int>());
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  EXPECT_NE(paint_layers_layer_topology_hash(*ma, *layer, Span<int>()), hash_none);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const std::string path = BKE_paint_layer_object_mask_attribute_path(*ma, *layer);
  EXPECT_TRUE(tree_has_object_attribute(*ma->paint_layers_tree, path));
}

TEST_F(PaintLayersObjectMaskTest, folder_mask_adds_object_attribute_node)
{
  MaterialPaintLayer *child = add_layer("Child");
  bake_setup(*child);
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>{child});
  ASSERT_NE(folder, nullptr);
  /* A plain folder is inlined (Pass Through) and has no row of its own for a mask to act on. */
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  folder->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  folder->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  /* A masked folder must keep its own row, so the mask can multiply the folder factor. */
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const std::string path = BKE_paint_layer_object_mask_attribute_path(*ma, *folder);
  EXPECT_TRUE(tree_has_object_attribute(*ma->paint_layers_tree, path));
}

TEST_F(PaintLayersObjectMaskTest, mask_toggle_does_not_move_topology)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  const uint64_t before = paint_layers_layer_topology_hash(*ma, *layer, Span<int>());
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1002, false);
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *layer, Span<int>()), before);
}

TEST_F(PaintLayersObjectMaskTest, bke_setter_drives_active_flag_alone)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  EXPECT_EQ(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  /* A no-op show on a fresh row creates no deviation and no topology flag. */
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, true));
  EXPECT_EQ(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
  /* First deviation sets the flag, so a BKE-only caller builds the same graph as RNA. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false));
  EXPECT_NE(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  /* Showing the last hidden record keeps the flag: toggling must not rebuild the graph. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, true));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
  EXPECT_NE(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  /* Hiding again changes only the value, not the flag. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, true));
  /* The pre-save cleanup drops the unused flag and with it the dead Attribute nodes. */
  BKE_paint_layer_object_masks_cleanup_all(*bmain);
  EXPECT_EQ(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
}

TEST_F(PaintLayersObjectMaskTest, cleanup_all_keeps_flag_of_used_row)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  BKE_paint_layer_object_masks_cleanup_all(*bmain);
  EXPECT_NE(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, duplicate_object_and_material_keeps_mask)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  ASSERT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
  /* Shift+D with Duplicate Data -> Material: fresh material uid, records still name the old one. */
  Object *ob_dup = id_cast<Object *>(BKE_id_copy(bmain, &ob->id));
  Material *ma_dup = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  /* Shift+D with Duplicate Data copies the mesh too; sharing it would put the copy on both. */
  ob_dup->data = BKE_id_copy(bmain, &me->id);
  ASSERT_NE(ob_dup, nullptr);
  ASSERT_NE(ma_dup, nullptr);
  EXPECT_FALSE(BLI_uuid_equal(ma_dup->paint_layers_owner_uid, ma->paint_layers_owner_uid));
  BKE_object_material_assign(bmain, ob_dup, ma_dup, 1, BKE_MAT_ASSIGN_EXISTING);
  BKE_paint_layer_object_masks_duplicate_remap(*ob_dup, *ob);
  MaterialPaintLayer *dup_layer = static_cast<MaterialPaintLayer *>(ma_dup->paint_layers.first);
  ASSERT_NE(dup_layer, nullptr);
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob_dup, *ma_dup, *dup_layer, 0));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, show_without_record_does_not_touch_uid)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  ma->paint_layers_owner_uid = {};
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1001, true));
  EXPECT_TRUE(BLI_uuid_is_nil(ma->paint_layers_owner_uid));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

TEST_F(PaintLayersObjectMaskTest, remap_from_newid_follows_duplicated_material)
{
  MaterialPaintLayer *layer = add_layer("A");
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  Object *ob_dup = id_cast<Object *>(BKE_id_copy(bmain, &ob->id));
  Material *ma_dup = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  /* Shift+D with Duplicate Data copies the mesh too; sharing it would put the copy on both. */
  ob_dup->data = BKE_id_copy(bmain, &me->id);
  BKE_object_material_assign(bmain, ob_dup, ma_dup, 1, BKE_MAT_ASSIGN_EXISTING);
  ob->id.newid = &ob_dup->id;
  BKE_paint_layer_object_masks_remap_from_newid(*bmain);
  ob->id.newid = nullptr;
  MaterialPaintLayer *dup_layer = static_cast<MaterialPaintLayer *>(ma_dup->paint_layers.first);
  ASSERT_NE(dup_layer, nullptr);
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob_dup, *ma_dup, *dup_layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, tile_excluded_helper_edges)
{
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_excluded(1u << 0, 1001));
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_excluded(1u << 23, 1024));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_excluded(1u << 0, 1002));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_excluded(0xFFFFFFu, 1000));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_excluded(0xFFFFFFu, 1025));
}

TEST_F(PaintLayersObjectMaskTest, tiles_cache_fills_once_and_refresh_replaces)
{
  ASSERT_NE(ob->runtime, nullptr);
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 0);
  EXPECT_TRUE(BKE_paint_layer_object_mask_object_tiles_cached(*ob, *ma, false).is_empty());
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 1);
  /* A second call and a refresh reuse the same key rather than adding entries. */
  BKE_paint_layer_object_mask_object_tiles_cached(*ob, *ma, false);
  BKE_paint_layer_object_mask_object_tiles_cached(*ob, *ma, true);
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 1);
}

TEST_F(PaintLayersObjectMaskTest, tiles_prefetch_fills_cache_of_layered_material_users)
{
  /* The material has no owner uid until a mask is stored, so it is skipped. */
  BKE_paint_layer_object_mask_tiles_prefetch(*bmain);
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 0);
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  BKE_paint_layer_object_mask_tiles_prefetch(*bmain);
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 1);
}
}  // namespace blender::bke::tests
