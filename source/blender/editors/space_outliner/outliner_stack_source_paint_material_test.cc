/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_assert.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "BKE_gtest_base.hh"
#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_layer_edit.hh"
#include "BKE_scene.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "UI_resources.hh"

#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

bool paint_material_mask_preview_activate(Main &bmain,
                                          Scene &scene,
                                          Paint &paint,
                                          const StackRow &row,
                                          StringRef section_id);

namespace tests {

class OutlinerStackPaintMaterialSourceTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Scene *scene = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    scene = BKE_scene_add(bmain, "MaskPreviewScene");
    Paint *paint = &scene->toolsettings->imapaint.paint;
    BKE_paint_ensure(scene->toolsettings, &paint);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  Image &add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return *BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  Material &add_material_with_texture(Image &image)
  {
    Material *material = BKE_material_add(bmain, "Material");
    bNodeTree &tree = *material->nodetree;
    bNode &principled = *bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
    bNode &output = *bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
    bNode &texture = *bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    texture.id = &image.id;
    bke::node_add_link(tree,
                       principled,
                       *bke::node_find_socket(principled, SOCK_OUT, "BSDF"_ustr),
                       output,
                       *bke::node_find_socket(output, SOCK_IN, "Surface"_ustr));
    bke::node_add_link(tree,
                       texture,
                       *bke::node_find_socket(texture, SOCK_OUT, "Color"_ustr),
                       principled,
                       *bke::node_find_socket(principled, SOCK_IN, "Base Color"_ustr));
    return *material;
  }

  static bNode *find_texture_node(Material &material, const Image &image)
  {
    for (bNode &node : material.nodetree->nodes) {
      if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image.id) {
        return &node;
      }
    }
    return nullptr;
  }

  static bNode *find_principled(Material &material)
  {
    for (bNode &node : material.nodetree->nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        return &node;
      }
    }
    return nullptr;
  }

  static bNodeLink *find_link_into(bNodeTree &tree, bNodeSocket &to_socket)
  {
    for (bNodeLink &link : tree.links) {
      if (link.tosock == &to_socket) {
        return &link;
      }
    }
    return nullptr;
  }

  const StackSource &paint_source()
  {
    const StackSource *source = stack_source_get(SO_STACK_SRC_PAINT_MATERIAL);
    BLI_assert(source != nullptr);
    return *source;
  }
};

TEST_F(OutlinerStackPaintMaterialSourceTest, rewire_changes_state_hash_with_unchanged_counts)
{
  Image &image_a = add_image("FeedA");
  Image &image_b = add_image("FeedB");
  Material &material = add_material_with_texture(image_a);
  bNodeTree &tree = *material.nodetree;
  bNode &texture_b = *bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  texture_b.id = &image_b.id;

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const uint64_t hash_before = paint_source().state_hash(ctx, material.id);

  /* Move the Base Color feed over to the other texture: the node count and the link count stay
   * put, the link's two ends do not. A counts-only hash would call this the same stack, and the
   * rows built from the links would go stale. */
  bNode *texture_a = find_texture_node(material, image_a);
  bNode *principled = find_principled(material);
  ASSERT_NE(texture_a, nullptr);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  bNodeLink *feed = find_link_into(tree, *base_color);
  ASSERT_NE(feed, nullptr);
  bke::node_remove_link(&tree, *feed);
  bke::node_add_link(tree,
                     texture_b,
                     *bke::node_find_socket(texture_b, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *base_color);

  const uint64_t hash_after = paint_source().state_hash(ctx, material.id);
  EXPECT_NE(hash_before, hash_after);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, relinking_the_same_ends_hashes_the_same)
{
  Image &image = add_image("Feed");
  Material &material = add_material_with_texture(image);
  bNodeTree &tree = *material.nodetree;

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const uint64_t hash_before = paint_source().state_hash(ctx, material.id);

  /* Take the map's feed off and put it back on the same two ends: the stack did not change, and
   * the hash may not say it did -- that would re-read the rows on every build. */
  bNode *principled = find_principled(material);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  bNodeLink *feed = find_link_into(tree, *base_color);
  ASSERT_NE(feed, nullptr);
  bNode &from_node = *feed->fromnode;
  bNodeSocket &from_socket = *feed->fromsock;
  bke::node_remove_link(&tree, *feed);
  bke::node_add_link(tree, from_node, from_socket, *principled, *base_color);

  const uint64_t hash_after = paint_source().state_hash(ctx, material.id);
  EXPECT_EQ(hash_before, hash_after);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, adding_a_node_changes_state_hash)
{
  Image &image = add_image("Feed");
  Material &material = add_material_with_texture(image);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const uint64_t hash_before = paint_source().state_hash(ctx, material.id);
  bke::node_add_static_node(nullptr, *material.nodetree, SH_NODE_TEX_IMAGE);
  const uint64_t hash_after = paint_source().state_hash(ctx, material.id);

  EXPECT_NE(hash_before, hash_after);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, stack_past_the_addressable_range_shows_a_stub)
{
  Material &material = add_material_with_texture(add_image("Base"));

  /* Fill the stack just past the addressable range: the bare base at ordinal 0, then added
   * layers up to one ordinal past #STACK_ROW_ORDINAL_MAX. The images stay tiny so the test does
   * not pay for real maps. */
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  for (int i = 0; i < STACK_ROW_ORDINAL_MAX + 1; i++) {
    ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params));
  }

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const StackFocus focus;
  Vector<StackRow> rows;
  ASSERT_TRUE(paint_source().rows_build(ctx, focus, material.id, rows));
  ASSERT_FALSE(rows.is_empty());

  /* Every addressable row is listed as before; the one row above them is the stub. */
  for (const int index : rows.index_range().drop_back(1)) {
    EXPECT_TRUE(rows[index].supported);
    EXPECT_LE(rows[index].ordinal, STACK_ROW_ORDINAL_MAX);
  }
  const StackRow &stub = rows.last();
  EXPECT_EQ(stub.ordinal, STACK_ROW_ORDINAL_MAX + 1);
  EXPECT_FALSE(stub.supported);
  EXPECT_STREQ(stub.unsupported_reason, "Stack is too large to display");
  EXPECT_TRUE(stub.preview_slots.is_empty());
  EXPECT_TRUE(stub.content_sections.is_empty());
}

TEST_F(OutlinerStackPaintMaterialSourceTest, copy_starts_with_a_fresh_paint_revision)
{
  Material &material = add_material_with_texture(add_image("Base"));
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params));
  EXPECT_GT(BKE_material_paint_layer_revision_get(material), 0);

  Material *copy = static_cast<Material *>(BKE_id_copy(bmain, &material.id));
  ASSERT_NE(copy, nullptr);
  /* The revision says "the stack I read is still the stack it was"; a copy has not been read by
   * anyone yet, whatever the original has done since. */
  EXPECT_EQ(BKE_material_paint_layer_revision_get(*copy), 0);
  BKE_id_free(bmain, copy);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, stack_rows_report_layer_kind)
{
  Material &material = add_material_with_texture(add_image("Base"));

  PaintMaterialLayerAddParams fill_params;
  fill_params.image_size = 8;
  fill_params.type = PaintMaterialLayerAddType::Fill;
  const float fill_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  copy_v4_v4(fill_params.fill_color, fill_color);
  fill_params.name = "Filler";
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, fill_params));

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const StackFocus focus;
  Vector<StackRow> rows;
  ASSERT_TRUE(paint_source().rows_build(ctx, focus, material.id, rows));
  ASSERT_EQ(rows.size(), 2);

  /* The bare base carries no kind marker: it reads as a plain Paint layer. */
  EXPECT_EQ(rows[0].icon, ICON_IMAGE_RGB);

  /* The Fill row reads as what it is: the Fill icon, and its colour in the second slot. */
  EXPECT_EQ(rows[1].icon, ICON_GP_DRAW_FILL);
  ASSERT_EQ(rows[1].preview_slots.size(), 2);
  EXPECT_TRUE(rows[1].preview_slots[1].is_color_swatch);
  EXPECT_NEAR(rows[1].preview_slots[1].color[0], 0.25f, 1e-6f);
  EXPECT_NEAR(rows[1].preview_slots[1].color[1], 0.5f, 1e-6f);
  EXPECT_NEAR(rows[1].preview_slots[1].color[2], 0.75f, 1e-6f);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, material_layer_row_resolves_its_source_material)
{
  Material &material = add_material_with_texture(add_image("Base"));

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.name = "MatLayer";
  int ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params, &ordinal));

  /* Mark the new layer as baked from another material, and link its map back to it -- the state
   * the material-bake glue leaves behind. */
  Material &source_material = *BKE_material_add(bmain, "BakedFrom");
  bNode *layer_mix = nullptr;
  for (bNode &node : material.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX) {
      layer_mix = &node;
      break;
    }
  }
  ASSERT_NE(layer_mix, nullptr);
  BKE_paint_material_layer_kind_set(*layer_mix, PaintMaterialLayerKind::Material);

  Image *layer_map = nullptr;
  for (Image &image : bmain->images) {
    if (STREQ(image.id.name + 2, "MatLayer")) {
      layer_map = &image;
      break;
    }
  }
  ASSERT_NE(layer_map, nullptr);
  ImageMaterialSource material_source;
  material_source.material = &source_material;
  material_source.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  material_source.bake_size = 8;
  BKE_image_material_source_set(*layer_map, material_source);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const StackFocus focus;
  Vector<StackRow> rows;
  ASSERT_TRUE(paint_source().rows_build(ctx, focus, material.id, rows));
  ASSERT_EQ(rows.size(), 2);

  /* The row reads as its material: the material icon, and the material's preview beside the map. */
  EXPECT_EQ(rows[1].icon, ICON_MATERIAL);
  bool material_preview_found = false;
  for (const StackRowPreview &slot : rows[1].preview_slots) {
    if (slot.id_type == ID_MA && slot.id_uid == source_material.id.session_uid) {
      material_preview_found = true;
      break;
    }
  }
  EXPECT_TRUE(material_preview_found);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, mask_preview_click_enters_mask_mode)
{
  Material &material = add_material_with_texture(add_image("Base"));
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params, &ordinal));
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, material, ordinal, white, 8, &error))
      << int(error);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const StackFocus focus;
  Vector<StackRow> rows;
  ASSERT_TRUE(paint_source().rows_build(ctx, focus, material.id, rows));
  const StackRow *row = nullptr;
  for (const StackRow &candidate : rows) {
    if (candidate.ordinal == ordinal) {
      row = &candidate;
    }
  }
  ASSERT_NE(row, nullptr);

  Brush *original = BKE_brush_add(bmain, "OriginalBrush", OB_MODE_TEXTURE_PAINT);
  scene->toolsettings->imapaint.paint.brush = original;

  EXPECT_TRUE(paint_material_mask_preview_activate(
      *bmain, *scene, scene->toolsettings->imapaint.paint, *row, "MASK"));
  EXPECT_NE(scene->toolsettings->paint_mode.mask_image_binding.image, nullptr);
  EXPECT_NE(BKE_paint_brush(&scene->toolsettings->imapaint.paint), original);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, channels_preview_click_after_mask_restores_brush)
{
  Material &material = add_material_with_texture(add_image("Base2"));
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params, &ordinal));
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, material, ordinal, white, 8, &error))
      << int(error);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const StackFocus focus;
  Vector<StackRow> rows;
  ASSERT_TRUE(paint_source().rows_build(ctx, focus, material.id, rows));
  const StackRow *row = nullptr;
  for (const StackRow &candidate : rows) {
    if (candidate.ordinal == ordinal) {
      row = &candidate;
    }
  }
  ASSERT_NE(row, nullptr);

  Brush *original = BKE_brush_add(bmain, "OriginalBrush2", OB_MODE_TEXTURE_PAINT);
  scene->toolsettings->imapaint.paint.brush = original;
  ASSERT_TRUE(paint_material_mask_preview_activate(
      *bmain, *scene, scene->toolsettings->imapaint.paint, *row, "MASK"));

  EXPECT_TRUE(paint_material_mask_preview_activate(
      *bmain, *scene, scene->toolsettings->imapaint.paint, *row, "CHANNELS"));
  EXPECT_EQ(scene->toolsettings->paint_mode.mask_image_binding.image, nullptr);
  EXPECT_EQ(BKE_paint_brush(&scene->toolsettings->imapaint.paint), original);
}

TEST_F(OutlinerStackPaintMaterialSourceTest, mask_a_to_b_to_channels_uses_the_right_images)
{
  Material &material = add_material_with_texture(add_image("Base3"));
  PaintMaterialLayerAddParams params_a;
  params_a.image_size = 8;
  int ordinal_a = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params_a, &ordinal_a));
  PaintMaterialLayerAddParams params_b;
  params_b.image_size = 8;
  int ordinal_b = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(bmain, material, params_b, &ordinal_b));

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, material, ordinal_a, white, 8, &error))
      << int(error);
  ASSERT_TRUE(BKE_paint_material_layer_mask_add(*bmain, material, ordinal_b, white, 8, &error))
      << int(error);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const StackFocus focus;
  Vector<StackRow> rows;
  ASSERT_TRUE(paint_source().rows_build(ctx, focus, material.id, rows));
  const StackRow *row_a = nullptr;
  const StackRow *row_b = nullptr;
  for (const StackRow &candidate : rows) {
    if (candidate.ordinal == ordinal_a) {
      row_a = &candidate;
    }
    if (candidate.ordinal == ordinal_b) {
      row_b = &candidate;
    }
  }
  ASSERT_NE(row_a, nullptr);
  ASSERT_NE(row_b, nullptr);

  ASSERT_TRUE(paint_material_mask_preview_activate(
      *bmain, *scene, scene->toolsettings->imapaint.paint, *row_a, "MASK"));
  Image *mask_a = scene->toolsettings->paint_mode.mask_image_binding.image;
  ASSERT_NE(mask_a, nullptr);

  /* A -> B: replaces only the image (brush snapshot covered at the BKE level in Task 7). */
  ASSERT_TRUE(paint_material_mask_preview_activate(
      *bmain, *scene, scene->toolsettings->imapaint.paint, *row_b, "MASK"));
  Image *mask_b = scene->toolsettings->paint_mode.mask_image_binding.image;
  EXPECT_NE(mask_b, mask_a);

  /* Exit: the binding is cleared. */
  EXPECT_TRUE(paint_material_mask_preview_activate(
      *bmain, *scene, scene->toolsettings->imapaint.paint, *row_b, "CHANNELS"));
  EXPECT_EQ(scene->toolsettings->paint_mode.mask_image_binding.image, nullptr);
}

}  // namespace tests
}  // namespace blender::ed::outliner
