/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"

#include <string>

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

namespace blender::bke::tests {

/**
 * A material whose Base Color is a chain of Mix nodes over Image Textures, which is the shape the
 * whole feature is about. Built by hand rather than by the (not yet written) layer-add function so
 * that the tests do not depend on it.
 */
class PaintMaterialLayerEditTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    material = BKE_material_add(bmain, "Material");
    bNodeTree &tree = *material->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  bNode &principled_node()
  {
    for (bNode &node : material->nodetree->nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        return node;
      }
    }
    BLI_assert_unreachable();
    return *static_cast<bNode *>(material->nodetree->nodes.first);
  }

  bNode &add_image_texture(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    bNode *node = bke::node_add_static_node(nullptr, *material->nodetree, SH_NODE_TEX_IMAGE);
    node->id = &image->id;
    return *node;
  }

  /** \a layer_num layers on Base Color, named "L0" (the bottom) upwards. */
  void build_stack(const int layer_num)
  {
    bNodeTree &tree = *material->nodetree;
    bNode *below = &add_image_texture("L0");
    bNodeSocket *below_out = bke::node_find_socket(*below, SOCK_OUT, "Color"_ustr);

    for (int i = 1; i < layer_num; i++) {
      char name[16];
      SNPRINTF_UTF8(name, "L%d", i);
      bNode &top = add_image_texture(name);
      bNode *mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX_RGB_LEGACY);
      STRNCPY_UTF8(mix->label, name);
      bke::node_add_link(tree,
                         *below,
                         *below_out,
                         *mix,
                         *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
      bke::node_add_link(tree,
                         top,
                         *bke::node_find_socket(top, SOCK_OUT, "Color"_ustr),
                         *mix,
                         *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
      below = mix;
      below_out = bke::node_find_socket(*mix, SOCK_OUT, "Color"_ustr);
    }
    bNode &principled = principled_node();
    bke::node_add_link(tree,
                       *below,
                       *below_out,
                       principled,
                       *bke::node_find_socket(principled, SOCK_IN, "Base Color"_ustr));
  }

  /** The stack as the reader sees it, bottom to top, by layer name. */
  Vector<std::string> layer_names()
  {
    Vector<PaintMaterialLayerStackEntry> entries;
    BKE_paint_material_layer_stack_from_material(*bmain, *material, entries);
    Vector<std::string> names;
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      names.append(entry.name);
    }
    return names;
  }
};

TEST_F(PaintMaterialLayerEditTest, markers_are_assigned_once_and_kept)
{
  build_stack(3);
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));

  Vector<bUUID> first_pass;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
      const bUUID marker = BKE_paint_material_layer_marker_get(node);
      EXPECT_FALSE(BLI_uuid_is_nil(marker));
      first_pass.append(marker);
    }
  }
  ASSERT_EQ(first_pass.size(), 2);
  EXPECT_FALSE(BLI_uuid_equal(first_pass[0], first_pass[1]));

  /* A second call must not re-issue identities, or every edit would break the previous one. */
  ASSERT_TRUE(BKE_paint_material_layer_markers_ensure(*material));
  int index = 0;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
      EXPECT_TRUE(BLI_uuid_equal(BKE_paint_material_layer_marker_get(node), first_pass[index++]));
    }
  }
}

TEST_F(PaintMaterialLayerEditTest, reorder_moves_a_layer_down)
{
  build_stack(4);
  EXPECT_EQ(layer_names()[3], "L3");

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_reorder(*bmain, *material, 3, 1, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);

  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 4);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "L3");
  EXPECT_EQ(names[2], "L1");
  EXPECT_EQ(names[3], "L2");
}

TEST_F(PaintMaterialLayerEditTest, reorder_refuses_the_bottom_layer)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_reorder(*bmain, *material, 0, 2, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  /* Refused means untouched, not partly applied. */
  const Vector<std::string> names = layer_names();
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[2], "L2");
}

TEST_F(PaintMaterialLayerEditTest, reorder_refuses_a_shared_chain)
{
  build_stack(3);
  /* A second consumer of an intermediate result: reordering would change what it receives. */
  bNode *extra = bke::node_add_static_node(
      nullptr, *material->nodetree, SH_NODE_MIX_RGB_LEGACY);
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY && STREQ(node.label, "L1")) {
      bke::node_add_link(*material->nodetree,
                         node,
                         *bke::node_find_socket(node, SOCK_OUT, "Color"_ustr),
                         *extra,
                         *bke::node_find_socket(*extra, SOCK_IN, "Color1"_ustr));
      break;
    }
  }

  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_reorder(*bmain, *material, 2, 1, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChainIsShared);
}

TEST_F(PaintMaterialLayerEditTest, remove_closes_the_chain)
{
  build_stack(4);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_remove(*bmain, *material, 2, &error));

  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 3);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "L1");
  EXPECT_EQ(names[2], "L3");
}

TEST_F(PaintMaterialLayerEditTest, remove_bottom_promotes_the_layer_above)
{
  build_stack(3);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_remove(*bmain, *material, 0, &error));

  /* L1's map becomes the new bottom, so its Mix node is gone and its name with it. */
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 2);
  EXPECT_EQ(names[0], "L1");
  EXPECT_EQ(names[1], "L2");
}

TEST_F(PaintMaterialLayerEditTest, remove_refuses_the_only_layer)
{
  build_stack(1);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_remove(*bmain, *material, 0, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  EXPECT_EQ(layer_names().size(), 1);
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_on_empty_material)
{
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
      << int(error);

  /* The first layer of an empty material is the bottom one: a bare Image Texture on Base Color. */
  EXPECT_EQ(ordinal, 0);
  EXPECT_EQ(layer_names().size(), 1);
  EXPECT_TRUE(BKE_paint_material_has_layer_stack(*material));
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_on_top_of_stack)
{
  build_stack(2);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
      << int(error);

  EXPECT_EQ(ordinal, 2);
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 3);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "L1");
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_at_ordinal)
{
  build_stack(3);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.ordinal = 1;
  params.name = "Inserted";
  int ordinal = -1;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, &error))
      << int(error);

  EXPECT_EQ(ordinal, 1);
  const Vector<std::string> names = layer_names();
  ASSERT_EQ(names.size(), 4);
  EXPECT_EQ(names[0], "L0");
  EXPECT_EQ(names[1], "Inserted");
  EXPECT_EQ(names[2], "L1");
  EXPECT_EQ(names[3], "L2");
}

TEST_F(PaintMaterialLayerEditTest, add_paint_layer_refuses_the_bottom_ordinal)
{
  build_stack(2);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  params.ordinal = 0;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::IsBottomLayer);
  EXPECT_EQ(layer_names().size(), 2);
}

TEST_F(PaintMaterialLayerEditTest, add_layer_rolls_back_on_channel_failure)
{
  build_stack(3);
  /* A second consumer of an intermediate result: the chain cannot be rewritten, and the refusal
   * has to happen before any node is created. */
  bNode *extra = bke::node_add_static_node(nullptr, *material->nodetree, SH_NODE_MIX_RGB_LEGACY);
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY && STREQ(node.label, "L1")) {
      bke::node_add_link(*material->nodetree,
                         node,
                         *bke::node_find_socket(node, SOCK_OUT, "Color"_ustr),
                         *extra,
                         *bke::node_find_socket(*extra, SOCK_IN, "Color1"_ustr));
      break;
    }
  }
  const int node_num_before = BLI_listbase_count(&material->nodetree->nodes);
  const int image_num_before = BLI_listbase_count(&bmain->images);

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::ChainIsShared);
  EXPECT_EQ(BLI_listbase_count(&material->nodetree->nodes), node_num_before);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), image_num_before);
}

TEST_F(PaintMaterialLayerEditTest, add_layer_on_linked_material_cancels)
{
  build_stack(2);
  Library *library = static_cast<Library *>(BKE_id_new(bmain, ID_LI, "Library"));
  material->id.lib = library;

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  EXPECT_FALSE(BKE_paint_material_layer_add(*bmain, *material, params, nullptr, &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::NotEditable);

  /* Leave the material local again so that freeing `bmain` does not trip the linked-ID checks. */
  material->id.lib = nullptr;
}

TEST_F(PaintMaterialLayerEditTest, added_layers_share_one_marker_and_layer_id)
{
  build_stack(2);
  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  int ordinal = -1;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, *material, params, &ordinal, nullptr));

  /* The new layer's Mix node carries a marker, which is what lets a later reorder move it in
   * every channel at once. */
  bool marker_found = false;
  for (bNode &node : material->nodetree->nodes) {
    if (node.type_legacy != SH_NODE_MIX) {
      continue;
    }
    EXPECT_FALSE(BLI_uuid_is_nil(BKE_paint_material_layer_marker_get(node)));
    marker_found = true;
  }
  EXPECT_TRUE(marker_found);
}

}  // namespace blender::bke::tests
