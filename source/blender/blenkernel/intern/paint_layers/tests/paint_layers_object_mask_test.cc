/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Tests for #BKE_paint_layers_object_mask.hh: the per-object records of a layered material's rows
 * (Mesh and UV Tiles), their upkeep across copies and files, the Attribute nodes the generator
 * builds for them, and that the generated graph hides a row exactly as the stack without it.
 */

#include <cctype>
#include <string>

#include "paint_layers_graph_eval_test_fixture.hh"

#include "BLI_listbase.h"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_layers_object_mask.hh"
#include "BKE_paint_layers_target.hh"

#include "../paint_layers_generate_intern.hh"

namespace blender::bke::tests {

namespace {

constexpr eMaterialPaintChannel kColor = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

/** True when \a tree or a group nested in it has an Object Attribute node reading \a path. */
bool tree_has_object_attribute(const bNodeTree &tree, const std::string &path)
{
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_ATTRIBUTE) {
      const NodeShaderAttribute *data = static_cast<const NodeShaderAttribute *>(node.storage);
      if (data->type == SHD_ATTRIBUTE_OBJECT && STREQ(data->name, path.c_str())) {
        return true;
      }
    }
    else if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
             tree_has_object_attribute(*id_cast<const bNodeTree *>(node.id), path))
    {
      return true;
    }
  }
  return false;
}

}  // namespace

class PaintLayersObjectMaskTest : public PaintLayersGraphEvalTest {
 public:
  Mesh *me = nullptr;
  Object *ob = nullptr;

  void SetUp() override
  {
    PaintLayersGraphEvalTest::SetUp();
    me = BKE_mesh_add(bmain, "Me");
    ob = BKE_object_add_only_object(bmain, OB_MESH, "Ob");
    ob->data = &me->id;
    id_us_plus(&me->id);
    BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBDATA);
  }

  /** A row with a map, limited per object (Mesh) or per tile (UV Tiles). */
  MaterialPaintLayer *add_masked_layer(const char *name,
                                       const eMaterialPaintLayerObjectMaskType type)
  {
    MaterialPaintLayer *layer = add_layer(name, add_solid_image(name, 30, 160, 220, 255));
    layer->object_mask_type = type;
    layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
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
};

/* -------------------------------------------------------------------- */
/** \name Records
 * \{ */

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
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  EXPECT_LE(BKE_paint_layer_object_mask_attribute_path(*ma, *layer).size(), size_t(67));
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  EXPECT_LE(BKE_paint_layer_object_mask_attribute_path(*ma, *layer).size(), size_t(67));
}

TEST_F(PaintLayersObjectMaskTest, no_record_means_visible)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

TEST_F(PaintLayersObjectMaskTest, mesh_hide_then_show_removes_record)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
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
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  /* The Mesh flag is not read in UV Tiles mode. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1001));
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, tile_bits_edges)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1001, false));
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1024, false));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1025, false));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1001));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1024));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1002));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 1025));
  EXPECT_EQ(BKE_paint_layer_object_mask_excluded_tiles(*ob, *ma, *layer), (1u << 0) | (1u << 23));
  const ObjectPaintLayerMask *rec = static_cast<const ObjectPaintLayerMask *>(
      ob->paint_layer_masks.first);
  EXPECT_EQ(rec->excluded_tiles_value, float((1u << 0) | (1u << 23)));
  for (int tile = 1001; tile <= 1024; tile++) {
    BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, tile, false);
  }
  EXPECT_EQ(BKE_paint_layer_object_mask_excluded_tiles(*ob, *ma, *layer), 0xFFFFFFu);
}

TEST_F(PaintLayersObjectMaskTest, tile_excluded_helper_edges)
{
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_excluded(1u << 0, 1001));
  EXPECT_TRUE(BKE_paint_layer_object_mask_tile_excluded(1u << 23, 1024));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_excluded(1u << 0, 1002));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_excluded(0xFFFFFFu, 1000));
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_excluded(0xFFFFFFu, 1025));
}

TEST_F(PaintLayersObjectMaskTest, visible_nested_folder_parent_mesh_child_tiles)
{
  MaterialPaintLayer *child = add_masked_layer("Child", MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
  MaterialPaintLayer *members[1] = {child};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  folder->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  folder->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *child, 1002, false);
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *ma, *child, 1001));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *child, 1002));
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *folder, false);
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *child, 1001));
}

TEST_F(PaintLayersObjectMaskTest, bke_setter_drives_active_flag_alone)
{
  MaterialPaintLayer *layer = add_layer("A", add_solid_image("A", 30, 160, 220, 255));
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  EXPECT_EQ(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  /* A no-op show on a fresh row creates no deviation and no topology flag. */
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, true));
  EXPECT_EQ(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
  /* The first deviation sets the flag, so a BKE-only caller builds the same graph as RNA. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false));
  EXPECT_NE(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  /* Showing the last hidden record keeps the flag: toggling must not rebuild the graph. */
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, true));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
  EXPECT_NE(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  /* The pre-save cleanup drops the unused flag and with it the dead Attribute nodes. */
  BKE_paint_layer_object_masks_cleanup_all(*bmain);
  EXPECT_EQ(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
}

TEST_F(PaintLayersObjectMaskTest, cleanup_all_keeps_flag_of_used_row)
{
  MaterialPaintLayer *layer = add_layer("A", add_solid_image("A", 30, 160, 220, 255));
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  BKE_paint_layer_object_masks_cleanup_all(*bmain);
  EXPECT_NE(layer->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, cleanup_drops_dead_records)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  BKE_paint_layers_remove(*ma, layer);
  BKE_paint_layer_object_masks_cleanup(*bmain, *ob);
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

TEST_F(PaintLayersObjectMaskTest, show_without_record_does_not_touch_uid)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
  ma->paint_layers_owner_uid = {};
  EXPECT_FALSE(BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1001, true));
  EXPECT_TRUE(BLI_uuid_is_nil(ma->paint_layers_owner_uid));
  EXPECT_TRUE(BLI_listbase_is_empty(&ob->paint_layer_masks));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copies and files
 * \{ */

TEST_F(PaintLayersObjectMaskTest, two_materials_same_marker_are_independent)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  /* The first record gives the material its owner uid; a copy made after it must not share it. */
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  MaterialPaintLayer *copy_layer = static_cast<MaterialPaintLayer *>(copy->paint_layers.first);
  ASSERT_TRUE(BLI_uuid_equal(copy_layer->marker, layer->marker));
  EXPECT_FALSE(BLI_uuid_equal(copy->paint_layers_owner_uid, ma->paint_layers_owner_uid));
  EXPECT_TRUE(BKE_paint_layer_object_mask_visible(*ob, *copy, *copy_layer, 0));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*ob, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, ensure_unique_regenerates_and_remaps)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
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

TEST_F(PaintLayersObjectMaskTest, object_copy_copies_records)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  Object *dup = id_cast<Object *>(BKE_id_copy(bmain, &ob->id));
  EXPECT_FALSE(BKE_paint_layer_object_mask_visible(*dup, *ma, *layer, 0));
}

TEST_F(PaintLayersObjectMaskTest, duplicate_object_and_material_keeps_mask)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
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

TEST_F(PaintLayersObjectMaskTest, remap_from_newid_follows_duplicated_material)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile cache
 * \{ */

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
  ma->paint_layers_owner_uid = {};
  BKE_paint_layer_object_mask_tiles_prefetch(*bmain);
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 0);
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  BKE_paint_layer_object_mask_tiles_prefetch(*bmain);
  EXPECT_EQ(ob->runtime->paint_layer_mask_tiles.size(), 1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke target
 * \{ */

TEST_F(PaintLayersObjectMaskTest, stroke_target_reports_a_hidden_object)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  BKE_paint_layers_active_set(*ma, layer->marker);
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, kColor, PaintLayersTargetMode::Content, target));
  EXPECT_FALSE(target.object_masked);
  BKE_paint_layer_object_mask_visible_set(*bmain, *ob, *ma, *layer, false);
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, kColor, PaintLayersTargetMode::Content, target));
  EXPECT_TRUE(target.object_masked);
  EXPECT_EQ(target.excluded_tiles, 0u);
  /* The material-only resolver knows no object, so it never reports one. */
  ASSERT_TRUE(BKE_paint_layers_target_get(*ma, kColor, target));
  EXPECT_FALSE(target.object_masked);
}

TEST_F(PaintLayersObjectMaskTest, stroke_target_carries_excluded_tiles)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
  BKE_paint_layers_active_set(*ma, layer->marker);
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1002, false);
  PaintLayersTarget target;
  ASSERT_TRUE(BKE_paint_layers_target_get(
      *ob, -1, kColor, PaintLayersTargetMode::Content, target));
  EXPECT_FALSE(target.object_masked);
  EXPECT_EQ(target.excluded_tiles, 1u << 1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generated graph
 * \{ */

TEST_F(PaintLayersObjectMaskTest, mask_adds_an_object_attribute_node_and_moves_the_hash)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *layer = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const std::string path = BKE_paint_layer_object_mask_attribute_path(*ma, *layer);
  EXPECT_FALSE(tree_has_object_attribute(*ma->paint_layers_tree, path));

  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {kColor};
  const uint64_t plain = paint_layers::paint_layers_layer_topology_hash(*ma, *layer, wired, forms);
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  layer->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  const uint64_t masked = paint_layers::paint_layers_layer_topology_hash(*ma, *layer, wired, forms);
  EXPECT_NE(masked, plain);
  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
  EXPECT_NE(paint_layers::paint_layers_layer_topology_hash(*ma, *layer, wired, forms), masked);

  layer->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(tree_has_object_attribute(*ma->paint_layers_tree, path));
  /* The Mesh read is the object's flag; the tile read is its bits. */
  EXPECT_NE(path.find("excluded_value"), std::string::npos);
}

TEST_F(PaintLayersObjectMaskTest, toggling_visibility_does_not_move_the_hash)
{
  MaterialPaintLayer *layer = add_masked_layer("A", MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {kColor};
  /* The first record gives the material an owner uid, which the masked hash mixes in: settle it
   * first, then a toggle is only a value on the Attribute node. */
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1002, false);
  const uint64_t after_first = paint_layers::paint_layers_layer_topology_hash(
      *ma, *layer, wired, forms);
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1003, false);
  BKE_paint_layer_object_mask_tile_set(*bmain, *ob, *ma, *layer, 1002, true);
  EXPECT_EQ(paint_layers::paint_layers_layer_topology_hash(*ma, *layer, wired, forms),
            after_first);
}

TEST_F(PaintLayersObjectMaskTest, masked_folder_keeps_its_own_row)
{
  MaterialPaintLayer *child = add_layer("Child", add_solid_image("Child", 30, 160, 220, 255));
  MaterialPaintLayer *members[1] = {child};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  /* A plain folder is inlined, so it has no row of its own for a mask to act on. */
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  folder->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
  folder->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const std::string path = BKE_paint_layer_object_mask_attribute_path(*ma, *folder);
  EXPECT_TRUE(tree_has_object_attribute(*ma->paint_layers_tree, path));
}

TEST_F(PaintLayersObjectMaskTest, a_visible_masked_row_equals_the_cpu_composite)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_masked_layer("Top", MA_PAINT_LAYER_OBJECT_MASK_MESH);
  ma->paint_layers_owner_uid = BLI_uuid_generate_random();
  /* No record on the object: the Attribute node reads 0, nothing is hidden, and the CPU composite
   * (which knows no object) is the right answer. */
  object_mask_value = 0.0f;
  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersObjectMaskTest, a_hidden_row_equals_the_stack_without_it)
{
  const auto build = [&](const bool with_top, const char *name) {
    ma = BKE_material_add(bmain, name);
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    ma->paint_layers_owner_uid = BLI_uuid_generate_random();
    add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
    if (with_top) {
      add_masked_layer("Top", MA_PAINT_LAYER_OBJECT_MASK_MESH);
    }
  };

  build(false, "Without");
  const RGBA without = cpu_pixel(kColor);

  build(true, "Masked");
  object_mask_value = 1.0f;
  const RGBA hidden = graph_pixel(kColor);
  EXPECT_NEAR(hidden.r, without.r, 2e-3f);
  EXPECT_NEAR(hidden.g, without.g, 2e-3f);
  EXPECT_NEAR(hidden.b, without.b, 2e-3f);
}

TEST_F(PaintLayersObjectMaskTest, a_hidden_folder_hides_what_it_holds)
{
  const auto build = [&](const bool with_folder, const char *name) {
    ma = BKE_material_add(bmain, name);
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    ma->paint_layers_owner_uid = BLI_uuid_generate_random();
    add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
    if (with_folder) {
      MaterialPaintLayer *child = add_layer("Child", add_solid_image("Child", 30, 160, 220, 255));
      MaterialPaintLayer *members[1] = {child};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
      ASSERT_NE(folder, nullptr);
      folder->object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_MESH;
      folder->object_mask_flag = MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
    }
  };

  build(false, "Without");
  const RGBA without = cpu_pixel(kColor);

  build(true, "Folded");
  object_mask_value = 1.0f;
  const RGBA hidden = graph_pixel(kColor);
  EXPECT_NEAR(hidden.r, without.r, 2e-3f);
  EXPECT_NEAR(hidden.g, without.g, 2e-3f);
  EXPECT_NEAR(hidden.b, without.b, 2e-3f);
  object_mask_value = 0.0f;
  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersObjectMaskTest, a_hidden_tile_hides_the_row_only_on_that_tile)
{
  /* The reference texel's UV is inside the first tile (1001), bit 0. */
  const auto build = [&](const bool with_top, const char *name) {
    ma = BKE_material_add(bmain, name);
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    ma->paint_layers_owner_uid = BLI_uuid_generate_random();
    add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
    if (with_top) {
      add_masked_layer("Top", MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
    }
  };

  build(false, "Without");
  const RGBA without = cpu_pixel(kColor);

  build(true, "Tiled");
  object_mask_value = float(1u << 0);
  const RGBA hidden = graph_pixel(kColor);
  EXPECT_NEAR(hidden.r, without.r, 2e-3f);
  EXPECT_NEAR(hidden.g, without.g, 2e-3f);
  EXPECT_NEAR(hidden.b, without.b, 2e-3f);

  /* Another tile hidden: this texel still shows the row, as the CPU composite says. */
  object_mask_value = float(1u << 1);
  expect_graph_matches_cpu(kColor);
}

/** \} */

}  // namespace blender::bke::tests
