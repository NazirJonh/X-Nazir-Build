/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

namespace blender::bke::tests {

class PaintMaterialLayerModelTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  Material &add_material()
  {
    Material *material = BKE_material_add(bmain, "Material");
    bNodeTree &tree = *material->nodetree;
    bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
    bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(tree,
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                       *output,
                       *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
    return *material;
  }

  Image &add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return *BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  bNode &add_image_texture(Material &material, Image &image)
  {
    bNode *node = bke::node_add_static_node(nullptr, *material.nodetree, SH_NODE_TEX_IMAGE);
    node->id = &image.id;
    return *node;
  }

  bNodeSocket *base_color_socket(Material &material)
  {
    for (bNode &node : material.nodetree->nodes) {
      if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
        return bke::node_find_socket(node, SOCK_IN, "Base Color"_ustr);
      }
    }
    return nullptr;
  }
};

TEST_F(PaintMaterialLayerModelTest, single_image_preserves_node_identity)
{
  Material &material = add_material();
  Image &image = add_image("Base");
  bNode &texture = add_image_texture(material, image);
  bNodeSocket *base_color = base_color_socket(material);
  ASSERT_NE(base_color, nullptr);
  bke::node_add_link(*material.nodetree,
                     texture,
                     *bke::node_find_socket(texture, SOCK_OUT, "Color"_ustr),
                     *base_color);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, material, entries));
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].ordinal, 0);
  EXPECT_EQ(entries[0].node_id, texture.identifier);
  EXPECT_EQ(entries[0].owner_tree, material.nodetree);
  EXPECT_EQ(entries[0].channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr),
            &image);
}

TEST_F(PaintMaterialLayerModelTest, mix_chain_is_bottom_to_top_with_opacity)
{
  Material &material = add_material();
  bNodeTree &tree = *material.nodetree;
  Image &bottom_image = add_image("Bottom");
  Image &top_image = add_image("Top");
  bNode &bottom = add_image_texture(material, bottom_image);
  bNode &top = add_image_texture(material, top_image);
  bNode *mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX_RGB_LEGACY);
  bNodeSocket *base_color = base_color_socket(material);
  ASSERT_NE(base_color, nullptr);

  bke::node_add_link(tree,
                     bottom,
                     *bke::node_find_socket(bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(tree,
                     top,
                     *bke::node_find_socket(top, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  bke::node_add_link(tree,
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_OUT, "Color"_ustr),
                     *base_color);
  bNodeSocket *factor = bke::node_find_socket(*mix, SOCK_IN, "Fac"_ustr);
  static_cast<bNodeSocketValueFloat *>(factor->default_value)->value = 0.25f;

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, material, entries));
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr),
            &bottom_image);
  EXPECT_EQ(entries[1].channel_images.lookup_default(PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr),
            &top_image);
  EXPECT_EQ(entries[1].node_id, mix->identifier);
  EXPECT_FLOAT_EQ(entries[1].opacity, 0.25f);
  EXPECT_TRUE(entries[1].factor_prop.has_value());
}

TEST_F(PaintMaterialLayerModelTest, unsupported_branch_stays_visible)
{
  Material &material = add_material();
  bNodeTree &tree = *material.nodetree;
  Image &bottom_image = add_image("Bottom");
  bNode &bottom = add_image_texture(material, bottom_image);
  bNode *noise = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_NOISE);
  bNode *mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX_RGB_LEGACY);
  bNodeSocket *base_color = base_color_socket(material);
  ASSERT_NE(base_color, nullptr);

  bke::node_add_link(tree,
                     bottom,
                     *bke::node_find_socket(bottom, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color1"_ustr));
  bke::node_add_link(tree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Color"_ustr),
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_IN, "Color2"_ustr));
  bke::node_add_link(tree,
                     *mix,
                     *bke::node_find_socket(*mix, SOCK_OUT, "Color"_ustr),
                     *base_color);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, material, entries));
  ASSERT_EQ(entries.size(), 2);
  EXPECT_TRUE(entries[0].supported);
  EXPECT_FALSE(entries[1].supported);
  EXPECT_NE(entries[1].unsupported_reason, nullptr);
}

}  // namespace blender::bke::tests
