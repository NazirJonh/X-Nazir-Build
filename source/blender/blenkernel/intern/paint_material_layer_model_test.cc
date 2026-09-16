/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"

#include "BLI_listbase.h"
#include "BLI_uuid.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

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
                     bke::node_find_node(*material.nodetree, *base_color),
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
                     bke::node_find_node(tree, *base_color),
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
  EXPECT_TRUE(entries[1].channel_factor_props.contains(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
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
                     bke::node_find_node(tree, *base_color),
                     *base_color);

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, material, entries));
  ASSERT_EQ(entries.size(), 2);
  EXPECT_TRUE(entries[0].supported);
  EXPECT_FALSE(entries[1].supported);
  EXPECT_NE(entries[1].unsupported_reason, nullptr);
}

TEST_F(PaintMaterialLayerModelTest, public_and_baked_roles_are_disjoint)
{
  EXPECT_TRUE(paint_layer_channel_is_public_map_role(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(paint_layer_channel_is_public_map_role(PAINT_LAYER_MAP_MASK));
  EXPECT_FALSE(paint_layer_channel_is_public_map_role(PAINT_LAYER_MAP_NONE));
  EXPECT_FALSE(paint_layer_channel_is_public_map_role(PAINT_LAYER_PASS_COMBINED));
  EXPECT_FALSE(paint_layer_channel_is_public_map_role(PAINT_LAYER_MAP_MASK_BAKED));

  EXPECT_TRUE(paint_layer_channel_is_internal_bake_role(PAINT_LAYER_MAP_MASK_BAKED));
  EXPECT_FALSE(paint_layer_channel_is_internal_bake_role(PAINT_LAYER_MAP_MASK));
  EXPECT_FALSE(paint_layer_channel_is_internal_bake_role(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintMaterialLayerModelTest, baked_mask_role_image_is_not_a_correction_map)
{
  /* The correction-tag path (paint_material_layer_model.cc's tagged_maps loop) attaches a map to
   * a correction row by marker alone, with no role guard of its own. It is therefore where a baked
   * mask -- same marker, internal role -- would leak into the model if the public-role predicate
   * did not keep it out of tagged_maps. The no-corrections path cannot show this: its later
   * ELEM(AO, MASK) drops the baked role on its own. */
  Material &material = add_material();

  PaintMaterialLayerAddParams params;
  params.image_size = 8;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, material, params, nullptr, &error));
  ASSERT_TRUE(BKE_paint_material_layer_add(*bmain, material, params, nullptr, &error));

  bUUID correction = {};
  ASSERT_TRUE(BKE_paint_material_layer_correction_add(*bmain,
                                                       material,
                                                       1,
                                                       PaintMaterialCorrectionSection::Content,
                                                       PaintMaterialCorrectionEffect::Paint,
                                                       nullptr,
                                                       &correction,
                                                       &error));
  EXPECT_EQ(error, PaintMaterialLayerEditError::None);
  material.nodetree->ensure_topology_cache();

  /* The baked mask carries the correction's marker, so only its role keeps it out. */
  Image &baked = add_image("Baked Mask");
  baked.paint_layer_id = correction;
  baked.paint_layer_channel = PAINT_LAYER_MAP_MASK_BAKED;

  Vector<PaintMaterialLayerStackEntry> entries;
  ASSERT_TRUE(BKE_paint_material_layer_stack_from_material(*bmain, material, entries));
  ASSERT_GE(entries.size(), 2);
  const PaintMaterialLayerStackEntry &top = entries[1];
  ASSERT_EQ(top.content_corrections.size(), 1);
  EXPECT_TRUE(BLI_uuid_equal(top.content_corrections[0].marker, correction));
  EXPECT_EQ(top.content_corrections[0].channel_images.lookup_default(
                PAINT_LAYER_MAP_MASK_BAKED, nullptr),
            nullptr);
  for (const auto item : top.content_corrections[0].channel_images.items()) {
    EXPECT_NE(item.value, &baked);
  }
}

TEST_F(PaintMaterialLayerModelTest, baked_mask_role_is_not_a_pass)
{
  EXPECT_FALSE(BKE_paint_material_composite_passes().contains(PAINT_LAYER_MAP_MASK_BAKED));
  EXPECT_FALSE(BKE_paint_material_display_passes().contains(PAINT_LAYER_MAP_MASK_BAKED));
}

}  // namespace blender::bke::tests
