/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: the pure build, regeneration into a material, the ownership of the
 * generated groups across a copy, and the undo hooks.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_node_tree_interface.hh"

#include "BKE_paint_layers_composite.hh"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_generate_layout.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Pure build
 * \{ */

TEST_F(PaintLayersGenerateTest, color_chain_builds_one_mix_over_two_maps)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));

  bNodeTree *tree = make_tree("PBR Layers A");
  ASSERT_NE(tree, nullptr);
  build_into(*tree);

  /* One Color channel is wired here (Base Color). The two maps are two Image Textures, each over
   * what is below through one Mix of its own, plus the two alpha-model Mix nodes of the channel
   * (the mixed alpha and the selector). */
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 2);
  EXPECT_EQ(count_type(*tree, SH_NODE_MIX), 6);
  EXPECT_TRUE(interface_has_socket(*tree, "Result Base Color", true));
}

TEST_F(PaintLayersGenerateTest, build_is_deterministic)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));

  bNodeTree *first = make_tree("PBR Layers First");
  bNodeTree *second = make_tree("PBR Layers Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  build_into(*first);
  build_into(*second);

  EXPECT_EQ(count_type(*first, SH_NODE_TEX_IMAGE), count_type(*second, SH_NODE_TEX_IMAGE));
  EXPECT_EQ(count_type(*first, SH_NODE_MIX), count_type(*second, SH_NODE_MIX));
  EXPECT_EQ(count_type(*first, SH_NODE_MATH), count_type(*second, SH_NODE_MATH));
}

TEST_F(PaintLayersGenerateTest, pure_build_instantiates_normal_combine_with_links)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));

  bNodeTree *tree = make_tree("PBR Layers Normal");
  ASSERT_NE(tree, nullptr);
  bNodeTree *normal_combine = BKE_paint_material_normal_combine_group_ensure(*bmain);
  ASSERT_NE(normal_combine, nullptr);
  /* No #Main reachable from the build itself: the group sockets come from the node declaration. */
  build_into(*tree, normal_combine);

  /* The combine lives inside the layer group, so walk the tree recursively. */
  int combines = 0;
  auto visit = [&](auto &self, bNodeTree &t) -> void {
    t.ensure_topology_cache();
    for (bNode &node : t.nodes) {
      if (BKE_paint_material_is_normal_combine_group(node)) {
        combines++;
        bNodeSocket *a = bke::node_find_socket(
            node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
        ASSERT_NE(a, nullptr);
        EXPECT_FALSE(a->directly_linked_links().is_empty());
      }
      else if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
        self(self, *reinterpret_cast<bNodeTree *>(node.id));
      }
    }
  };
  visit(visit, *tree);
  EXPECT_EQ(combines, 1);
}

TEST_F(PaintLayersGenerateTest, pure_build_matches_regenerate_topology)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*bottom, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  add_channel(*top, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("TopNormal"));
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  add_constant_channel(*top, PAINT_MATERIAL_CHANNEL_ROUGHNESS, grey);

  bNodeTree *pure = make_tree("PBR Layers Pure");
  ASSERT_NE(pure, nullptr);
  build_into(*pure, BKE_paint_material_normal_combine_group_ensure(*bmain));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  /* The multiset of node types is the same, group by group, down the whole tree. */
  auto idnames = [](auto &self, const bNodeTree &tree, Vector<std::string> &r_names) -> void {
    for (const bNode &node : tree.nodes) {
      r_names.append(std::string(node.idname));
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        self(self, *reinterpret_cast<const bNodeTree *>(node.id), r_names);
      }
    }
  };
  Vector<std::string> pure_names;
  Vector<std::string> regen_names;
  idnames(idnames, *pure, pure_names);
  idnames(idnames, *ma->paint_layers_tree, regen_names);
  std::sort(pure_names.begin(), pure_names.end());
  std::sort(regen_names.begin(), regen_names.end());
  EXPECT_EQ(pure_names, regen_names);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Regeneration into a material
 * \{ */

TEST_F(PaintLayersGenerateTest, regenerate_builds_group_instance_and_principled)
{
  add_paint_layer("Bottom", add_image("Bottom"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_TRUE(report.created_principled);

  ASSERT_NE(instance_find(), nullptr);
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Result Base Color", true));

  bNodeSocket *base_color = principled_input("Base Color");
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_FALSE(BLI_uuid_is_nil(ma->paint_layers_owner_uid));
}

TEST_F(PaintLayersGenerateTest, unpainted_default_channels_are_not_generated)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *layer);
  ASSERT_GT(layer->channels_num, 0);

  /* Records with no map and no coverage lay nothing down, so nothing is wired and the material
   * keeps what the user had on its Principled inputs. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ma->paint_layers_tree->ensure_interface_cache();
  EXPECT_TRUE(ma->paint_layers_tree->interface_outputs().is_empty());
  EXPECT_EQ(layer_tree_count(*bmain), 0);
  bNodeSocket *base_color = principled_input("Base Color");
  ASSERT_NE(base_color, nullptr);
  EXPECT_TRUE(base_color->directly_linked_links().is_empty());

  /* A value with coverage is a row that takes part: its channel is wired, with no map behind it. */
  const float value[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Result Base Color", true));
  EXPECT_FALSE(interface_has_socket(*ma->paint_layers_tree, "Result Roughness", true));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 0);
  EXPECT_EQ(layer_tree_count(*bmain), 1);
  base_color = principled_input("Base Color");
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  bNodeSocket *roughness = principled_input("Roughness");
  ASSERT_NE(roughness, nullptr);
  EXPECT_TRUE(roughness->directly_linked_links().is_empty());
}

TEST_F(PaintLayersGenerateTest, row_values_are_inputs_of_the_row_group)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  const float value[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  add_constant_channel(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.4f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  bNode *instance = group_instance_find(*ma->paint_layers_tree, *group);
  ASSERT_NE(instance, nullptr);

  const bNodeTreeInterfaceSocket *opacity_iface = interface_input_find(
      *group, "Paint Base Color Opacity");
  const bNodeTreeInterfaceSocket *fill_iface = interface_input_find(*group, "Paint Base Color");
  ASSERT_NE(opacity_iface, nullptr);
  ASSERT_NE(fill_iface, nullptr);
  bNodeSocket *opacity = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(opacity_iface->identifier));
  bNodeSocket *fill = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(fill_iface->identifier));
  ASSERT_NE(opacity, nullptr);
  ASSERT_NE(fill, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 0.4f);
  const float *color = static_cast<bNodeSocketValueRGBA *>(fill->default_value)->value;
  EXPECT_FLOAT_EQ(color[0], 0.2f);
  EXPECT_FLOAT_EQ(color[1], 0.4f);
  EXPECT_FLOAT_EQ(color[2], 0.6f);
  EXPECT_FLOAT_EQ(color[3], 1.0f);

  /* A switched-off row stays in the graph with no opacity. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  instance = group_instance_find(*ma->paint_layers_tree, *group);
  ASSERT_NE(instance, nullptr);
  opacity_iface = interface_input_find(*group, "Paint Base Color Opacity");
  ASSERT_NE(opacity_iface, nullptr);
  opacity = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(opacity_iface->identifier));
  ASSERT_NE(opacity, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 0.0f);
}

TEST_F(PaintLayersGenerateTest, mask_constant_and_opacity_are_inputs_of_the_row_group)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, layer, 0.4f);
  ASSERT_NE(mask, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  const bNodeTreeInterfaceSocket *value_iface = interface_input_find(*group, "Paint Mask");
  const bNodeTreeInterfaceSocket *opacity_iface = interface_input_find(*group,
                                                                       "Paint Mask Opacity");
  ASSERT_NE(value_iface, nullptr);
  ASSERT_NE(opacity_iface, nullptr);
  /* The map of the row, the Mix of its blend and its two alpha-model Mix nodes, plus one Mix
   * for the mask and no second map. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 1);
  EXPECT_EQ(count_type(*group, SH_NODE_MIX), 4);

  bNode *instance = group_instance_find(*ma->paint_layers_tree, *group);
  ASSERT_NE(instance, nullptr);
  bNodeSocket *value = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(value_iface->identifier));
  ASSERT_NE(value, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(value->default_value)->value, 0.4f);
}

TEST_F(PaintLayersGenerateTest, mask_edits_are_value_only_until_the_item_gets_a_map)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, layer, 0.4f);
  ASSERT_NE(mask, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* The constant and the switch reach the instance without any rebuild. */
  const float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, half));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, mask, false));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  bNode *instance = group_instance_find(*ma->paint_layers_tree, *group);
  ASSERT_NE(instance, nullptr);
  const bNodeTreeInterfaceSocket *value_iface = interface_input_find(*group, "Paint Mask");
  const bNodeTreeInterfaceSocket *opacity_iface = interface_input_find(*group,
                                                                       "Paint Mask Opacity");
  ASSERT_NE(value_iface, nullptr);
  ASSERT_NE(opacity_iface, nullptr);
  bNodeSocket *value = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(value_iface->identifier));
  bNodeSocket *opacity = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(opacity_iface->identifier));
  ASSERT_NE(value, nullptr);
  ASSERT_NE(opacity, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(value->default_value)->value, 0.5f);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(opacity->default_value)->value, 0.0f);

  /* A map is nodes: the constant input goes and an Image Texture takes its place. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("MaskMap")));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 2);
  EXPECT_EQ(interface_input_find(*group, "Paint Mask"), nullptr);
  EXPECT_NE(interface_input_find(*group, "Paint Mask Opacity"), nullptr);
}

TEST_F(PaintLayersGenerateTest, removing_the_mask_drops_its_inputs)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 0.4f), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  ASSERT_NE(interface_input_find(*group, "Paint Mask"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_mask_remove(*ma, layer));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(interface_input_find(*group, "Paint Mask"), nullptr);
  EXPECT_EQ(interface_input_find(*group, "Paint Mask Opacity"), nullptr);
  /* The row is back to its one map, its blend and its two alpha-model Mix nodes. */
  EXPECT_EQ(count_type(*group, SH_NODE_MIX), 3);
}

TEST_F(PaintLayersGenerateTest, leaf_group_has_the_layer_contract)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom, nullptr);
  ASSERT_NE(top, nullptr);

  for (bNodeTree *group : {bottom, top}) {
    EXPECT_TRUE(interface_has_socket(*group, "Below Base Color", false));
    EXPECT_TRUE(interface_has_socket(*group, "Result Base Color", true));
    /* The tree is marked as that layer's own group. */
    const IDProperty *props = IDP_GetProperties(&group->id);
    ASSERT_NE(props, nullptr);
    EXPECT_NE(IDP_GetPropertyTypeFromGroup(props, "pbr_paint_layers_layer_tree", IDP_STRING),
              nullptr);
  }

  /* One instance per layer, and nothing else that is a group, in the root tree. */
  int instances = 0;
  for (bNode &node : ma->paint_layers_tree->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        (node.id == &bottom->id || node.id == &top->id))
    {
      instances++;
    }
  }
  EXPECT_EQ(instances, 2);
}

TEST_F(PaintLayersGenerateTest, regenerate_reuses_layer_trees)
{
  Image *bottom_image = add_image("Bottom");
  Image *top_image = add_image("Top");
  add_paint_layer("Bottom", bottom_image);
  add_paint_layer("Top", top_image);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom, nullptr);
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(layer_tree_count(*bmain), 2);
  EXPECT_EQ(ID_REAL_USERS(&bottom->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&top->id), 1);
  const int bottom_image_users = ID_REAL_USERS(&bottom_image->id);
  const int top_image_users = ID_REAL_USERS(&top_image->id);
  EXPECT_GT(bottom_image_users, 1);
  EXPECT_GT(top_image_users, 1);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom);
  EXPECT_EQ(layer_tree_find(*bmain, "Top"), top);
  EXPECT_EQ(layer_tree_count(*bmain), 2);
  /* A rebuild takes and drops its own user references; the groups and the maps keep exactly the
   * users they had, or every regeneration would leak one. */
  EXPECT_EQ(ID_REAL_USERS(&bottom->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&top->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&bottom_image->id), bottom_image_users);
  EXPECT_EQ(ID_REAL_USERS(&top_image->id), top_image_users);
}

TEST_F(PaintLayersGenerateTest, removed_layer_drops_its_tree)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(layer_tree_find(*bmain, "Bottom"), nullptr);
  ASSERT_NE(top_tree, nullptr);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, bottom));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), nullptr);
  EXPECT_EQ(layer_tree_count(*bmain), 1);
  EXPECT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
}

TEST_F(PaintLayersGenerateTest, renamed_layer_keeps_its_tree)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);

  ASSERT_TRUE(BKE_paint_layers_rename(*ma, bottom, "Renamed"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Renamed"), bottom_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), nullptr);
}

TEST_F(PaintLayersGenerateTest, foreign_tree_is_replaced_not_overwritten)
{
  bNodeTree *foreign = make_tree("Foreign Tree");
  ASSERT_NE(foreign, nullptr);
  ma->paint_layers_tree = foreign;
  add_paint_layer("Bottom", add_image("Bottom"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(report.replaced_foreign_tree);
  EXPECT_NE(ma->paint_layers_tree, foreign);
  EXPECT_EQ(count_type(*foreign, SH_NODE_TEX_IMAGE), 0);
}

TEST_F(PaintLayersGenerateTest, tagged_regenerate_is_the_single_scheduling_point)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  /* The mutator only marked the description stale; nothing generated it yet. */
  EXPECT_EQ(ma->paint_layers_tree, nullptr);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_regenerate_tagged(*bmain);
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  bNodeTree *tree = ma->paint_layers_tree;

  /* A new edit marks it stale again, and only the scheduling point rebuilds it. */
  add_paint_layer("Top", add_image("Top"));
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 1);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 2);
}

TEST_F(PaintLayersGenerateTest, non_layered_material_is_not_regenerated)
{
  Material *plain = BKE_material_add(bmain, "Plain");
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(plain->paint_layers_tree, nullptr);
  EXPECT_FALSE(BKE_paint_layers_regenerate(*bmain, *plain));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy
 * \{ */

TEST_F(PaintLayersGenerateTest, copy_does_not_share_generated_tree)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->paint_layers_tree, nullptr);
  EXPECT_NE(copy->paint_layers_tree, ma->paint_layers_tree);
  EXPECT_FALSE(BLI_uuid_equal(copy->paint_layers_owner_uid, ma->paint_layers_owner_uid));

  bNode *copy_instance = nullptr;
  for (bNode &node : copy->nodetree->nodes) {
    if (node.id == &copy->paint_layers_tree->id) {
      copy_instance = &node;
      break;
    }
  }
  EXPECT_NE(copy_instance, nullptr);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, no_main_copy_shares_tree_and_uid)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  const int trees_before = BLI_listbase_count(&bmain->nodetrees);
  const int users_before = ma->paint_layers_tree->id.us;

  /* A copy outside Main is a view of its source, not a second owner: it must not put a second
   * generated tree into Main nor take the uid the source's trees are keyed by. */
  Material *copy = id_cast<Material *>(
      BKE_id_copy_ex(bmain, &ma->id, nullptr, LIB_ID_COPY_DEFAULT | LIB_ID_CREATE_NO_MAIN));
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->paint_layers_tree, ma->paint_layers_tree);
  EXPECT_TRUE(BLI_uuid_equal(copy->paint_layers_owner_uid, ma->paint_layers_owner_uid));
  EXPECT_EQ(BLI_listbase_count(&bmain->nodetrees), trees_before);

  BKE_id_free(nullptr, copy);
  EXPECT_EQ(ma->paint_layers_tree->id.us, users_before);
}

TEST_F(PaintLayersGenerateTest, copied_material_owns_its_layer_trees)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  bNodeTree *original_root = ma->paint_layers_tree;
  const int original_images = count_type(*original_root, SH_NODE_TEX_IMAGE);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->paint_layers_tree, nullptr);
  EXPECT_NE(copy->paint_layers_tree, original_root);

  /* The copy's layer tree is its own, marked with the copy's owner. */
  bNodeTree *copy_bottom = nullptr;
  for (bNode &node : copy->paint_layers_tree->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        node.id != &copy->paint_layers_tree->id)
    {
      copy_bottom = id_cast<bNodeTree *>(node.id);
    }
  }
  ASSERT_NE(copy_bottom, nullptr);
  EXPECT_NE(copy_bottom, bottom_tree);
  const IDProperty *props = IDP_GetProperties(&copy_bottom->id);
  ASSERT_NE(props, nullptr);
  const IDProperty *owner = IDP_GetPropertyTypeFromGroup(
      props, "pbr_paint_layers_owner", IDP_STRING);
  ASSERT_NE(owner, nullptr);
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, copy->paint_layers_owner_uid);
  EXPECT_STREQ(IDP_string_get(owner), formatted);

  /* Regenerating the copy leaves the original's tree and groups untouched. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *copy));
  EXPECT_EQ(ma->paint_layers_tree, original_root);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_EQ(count_type(*original_root, SH_NODE_TEX_IMAGE), original_images);

  BKE_id_free(bmain, copy);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Undo
 *
 * A memfile undo decodes the data-blocks whose stored state changed and keeps the others as they
 * were; the generated trees are rewritten after an undo push, so the decode can leave a tree that
 * names a map or a group the target state does not have. No node may keep an ID that is not in
 * #Main, or the next regeneration decrements the users of freed memory.
 * \{ */

TEST_F(PaintLayersGenerateTest, undo_validate_clears_dangling_node_ids_and_regenerates)
{
  Image *image = add_image("Bottom");
  add_paint_layer("Bottom", image);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);

  bNode *map = nullptr;
  for (bNode &node : group->nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      map = &node;
    }
  }
  ASSERT_NE(map, nullptr);
  ASSERT_EQ(map->id, &image->id);

  /* An image the target state does not have: it is a real data-block, but not one of #Main's. */
  Image *stray = BKE_id_new_nomain<Image>("Stray");
  ASSERT_NE(stray, nullptr);
  map->id = &stray->id;

  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  BKE_paint_layers_undo_validate(*bmain);
  EXPECT_EQ(map->id, nullptr);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* A pointer that is still in #Main is left alone. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), group);
  map = nullptr;
  for (bNode &node : group->nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      map = &node;
    }
  }
  ASSERT_NE(map, nullptr);
  EXPECT_EQ(map->id, &image->id);
  BKE_paint_layers_undo_validate(*bmain);
  EXPECT_EQ(map->id, &image->id);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_id_free(nullptr, stray);
}

TEST_F(PaintLayersGenerateTest, undo_prepare_detaches_generated_data_and_the_next_pass_restores)
{
  Image *image = add_image("Bottom");
  add_paint_layer("Bottom", image);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  ASSERT_EQ(instance->id, &ma->paint_layers_tree->id);

  BKE_paint_layers_undo_prepare(*bmain);
  EXPECT_EQ(instance->id, nullptr);
  for (bNode &node : group->nodes) {
    EXPECT_EQ(node.id, nullptr);
  }
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* The scheduling point puts the derived data back, into the groups the material already owns. */
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(layer_tree_count(*bmain), 1);
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), group);
  instance = instance_find();
  ASSERT_NE(instance, nullptr);
  EXPECT_EQ(instance->id, &ma->paint_layers_tree->id);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  for (bNode &node : group->nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      EXPECT_EQ(node.id, &image->id);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Participation
 * \{ */

TEST_F(PaintLayersGenerateTest, participation_cpu_matches_generator)
{
  /* One predicate decides whether a row takes part, for the generated graph and for the CPU stack
   * alike: every state of a record has to give the same answer on both sides. */
  const float alphas[] = {0.0f, 0.5f};
  for (const bool enabled : {true, false}) {
    for (const bool with_map : {true, false}) {
      for (const float alpha : alphas) {
        MaterialPaintLayer *layer = BKE_paint_layers_add(
            *ma, "L", nullptr, PaintLayerPlace::Above);
        ASSERT_NE(layer, nullptr);
        ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                  nullptr);
        BKE_paint_layers_channel_set_enabled(
            *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, enabled);
        const float value[4] = {1.0f, 0.0f, 0.0f, alpha};
        BKE_paint_layers_channel_set_value(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value);
        if (with_map) {
          BKE_paint_layers_channel_set_image(
              *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Map"));
        }

        Vector<PaintMaterialCompositeImageLayer> layers;
        const bool cpu = BKE_paint_layers_composite_image_layers(
            *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers);
        EXPECT_EQ(cpu,
                  paint_layers::leaf_participates(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
            << "enabled=" << enabled << " map=" << with_map << " alpha=" << alpha;
        BKE_paint_layers_remove(*ma, layer);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Values and topology
 * \{ */

/** A node a test drops into a generated tree to tell whether a later pass rebuilt it. */
static bool tree_sentinel_set(bNodeTree &tree)
{
  return bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE) != nullptr;
}

static bool tree_sentinel_get(const bNodeTree &tree)
{
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_VALUE) {
      return true;
    }
  }
  return false;
}

TEST_F(PaintLayersGenerateTest, value_edit_does_not_rebuild_any_group)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *fill = BKE_paint_layers_add(*ma, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  add_constant_channel(*fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, grey);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(fill_tree, nullptr);
  ASSERT_TRUE(tree_sentinel_set(*bottom_tree));
  ASSERT_TRUE(tree_sentinel_set(*fill_tree));

  /* Opacity and a Fill constant are group inputs: they change values, not topology, and never mark
   * the stack for a rebuild. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, bottom, 0.42f));
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, green));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Fill"), fill_tree);
  EXPECT_TRUE(tree_sentinel_get(*bottom_tree));
  EXPECT_TRUE(tree_sentinel_get(*fill_tree));
}

TEST_F(PaintLayersGenerateTest, paint_opacity_does_not_rebuild_the_group)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(tree_sentinel_set(*group));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.3f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Paint"), group);
  EXPECT_TRUE(tree_sentinel_get(*group));
}

TEST_F(PaintLayersGenerateTest, reorder_rebuilds_the_root)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  ASSERT_TRUE(tree_sentinel_set(*root));
  const int maps_before = count_type(*root, SH_NODE_TEX_IMAGE);

  /* Row order is structure: a reorder moves a row's own group in the root's chain, so the root is
   * rebuilt (the groups themselves may be kept). */
  MaterialPaintLayer *bottom = static_cast<MaterialPaintLayer *>(ma->paint_layers.first);
  MaterialPaintLayer *top = bottom->next;
  ASSERT_NE(top, nullptr);
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, top, 0));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_FALSE(tree_sentinel_get(*root));
  EXPECT_EQ(count_type(*root, SH_NODE_TEX_IMAGE), maps_before);
}

TEST_F(PaintLayersGenerateTest, reorder_inside_a_folder_rebuilds_the_root)
{
  MaterialPaintLayer *first = add_paint_layer("First", add_image("First"));
  MaterialPaintLayer *second = add_paint_layer("Second", add_image("Second"));
  MaterialPaintLayer *third = add_paint_layer("Third", add_image("Third"));
  MaterialPaintLayer *members[3] = {first, second, third};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 3)), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  ASSERT_TRUE(tree_sentinel_set(*root));

  /* Same rows, same parent, another order: the root hash is where that has to show, or the chain is
   * kept as it was and the generated graph disagrees with the description. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, third, 0));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_FALSE(tree_sentinel_get(*root));

  ASSERT_TRUE(tree_sentinel_set(*root));
  ASSERT_TRUE(BKE_paint_layers_move(*ma, third, second, PaintLayerPlace::Above));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(tree_sentinel_get(*root));
}

TEST_F(PaintLayersGenerateTest, root_hash_depends_on_row_order)
{
  MaterialPaintLayer *first = add_paint_layer("First", add_image("First"));
  MaterialPaintLayer *second = add_paint_layer("Second", add_image("Second"));
  MaterialPaintLayer *third = add_paint_layer("Third", add_image("Third"));
  const Vector<int> wired = paint_layers::paint_layers_wired_channels(*ma);
  const uint64_t flat = paint_layers::paint_layers_root_topology_hash(*ma, wired);

  /* A reorder at the root. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, third, 0));
  const uint64_t reordered = paint_layers::paint_layers_root_topology_hash(*ma, wired);
  EXPECT_NE(reordered, flat);
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, third, 2));
  EXPECT_EQ(paint_layers::paint_layers_root_topology_hash(*ma, wired), flat);

  /* A reorder inside a folder, with the folder's parent and members unchanged. */
  MaterialPaintLayer *members[3] = {first, second, third};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 3)), nullptr);
  const uint64_t grouped = paint_layers::paint_layers_root_topology_hash(*ma, wired);
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, first, 2));
  const uint64_t inner_reordered = paint_layers::paint_layers_root_topology_hash(*ma, wired);
  EXPECT_NE(inner_reordered, grouped);
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, first, 0));
  EXPECT_EQ(paint_layers::paint_layers_root_topology_hash(*ma, wired), grouped);
}

TEST_F(PaintLayersGenerateTest, set_source_rebuilds_the_row)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(tree_sentinel_set(*group));

  /* Source is structure (it decides the row's nodes and whether it carries a map at all). */
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Paint"), group);
  EXPECT_FALSE(tree_sentinel_get(*group));
}

TEST_F(PaintLayersGenerateTest, rna_opacity_is_value_only)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));
  /* No rebuild is pending, so the tagged scheduler leaves the group exactly as it is. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(layer_tree_find(*bmain, "Paint"), group);
}

TEST_F(PaintLayersGenerateTest, repeated_regenerate_does_not_grow_the_group)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  add_constant_channel(*layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, grey);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  const int inputs_before = int(group->interface_inputs().size());

  /* A value edit and a regenerate with nothing else changed keep the root, so the row's interface
   * cannot gain a duplicate input on any pass. */
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.3f + 0.1f * float(i)));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    group = layer_tree_find(*bmain, "Paint");
    ASSERT_NE(group, nullptr);
    group->ensure_interface_cache();
    EXPECT_EQ(int(group->interface_inputs().size()), inputs_before);
  }
}

TEST_F(PaintLayersGenerateTest, height_regenerate_does_not_link_bump_to_itself)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Paint"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT, add_image("Height"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The second pass finds the Principled Normal already fed by our Bump: it must not be relinked. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *bump = nullptr;
  for (bNode &node : ma->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      bump = &node;
    }
  }
  ASSERT_NE(bump, nullptr);
  bNodeSocket *bump_normal = bke::node_find_socket(*bump, SOCK_IN, "Normal");
  ASSERT_NE(bump_normal, nullptr);
  ma->nodetree->ensure_topology_cache();
  for (bNodeLink *link : bump_normal->directly_linked_links()) {
    EXPECT_NE(link->fromnode, bump);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout grid
 * \{ */

TEST(PaintLayersLayoutGrid, grids_are_independent)
{
  paint_layers::layout::LayoutGrid a;
  paint_layers::layout::LayoutGrid b;
  a.at(paint_layers::layout::Column::Source, 5);
  /* A second grid starts from row zero, whatever the first one has claimed. */
  const paint_layers::layout::Cursor fresh = b.at(paint_layers::layout::Column::Source, 0);
  EXPECT_FLOAT_EQ(fresh.y, 0.0f);
  EXPECT_FLOAT_EQ(fresh.x, paint_layers::layout::kColumnWidth);
}

TEST(PaintLayersLayoutGrid, lane_starts_past_the_previous_lane)
{
  paint_layers::layout::LayoutGrid grid;
  {
    paint_layers::layout::LayoutGrid::LaneScope lane0(grid, 0);
    grid.at(paint_layers::layout::Column::Source, 3);
  }
  paint_layers::layout::LayoutGrid::LaneScope lane1(grid, 1);
  const paint_layers::layout::Cursor cursor = grid.at(paint_layers::layout::Column::Source, 0);
  /* Base of lane 1: the tallest row of lane 0 (3) plus the two-row gap. */
  EXPECT_FLOAT_EQ(cursor.y, -5.0f * paint_layers::layout::kRowPitch);
}

TEST(PaintLayersLayoutGrid, lane_scope_restores_the_previous_lane)
{
  paint_layers::layout::LayoutGrid grid;
  paint_layers::layout::LayoutGrid::LaneScope outer(grid, 0);
  grid.at(paint_layers::layout::Column::Source, 2);
  {
    paint_layers::layout::LayoutGrid::LaneScope inner(grid, 1);
  }
  /* Back in lane 0: row 0 is the lane's own first row again. */
  EXPECT_FLOAT_EQ(grid.at(paint_layers::layout::Column::Source, 0).y, 0.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Global channel set
 * \{ */

TEST_F(PaintLayersGenerateTest, channel_out_of_the_set_is_not_wired)
{
  MaterialPaintLayer *layer = add_paint_layer(
      "Paint", add_image("Map"), PAINT_MATERIAL_CHANNEL_METALLIC);

  EXPECT_TRUE(paint_layers::leaf_participates(*ma, *layer, PAINT_MATERIAL_CHANNEL_METALLIC));
  Vector<int> wired = paint_layers_wired_channels(*ma);
  EXPECT_TRUE(std::find(wired.begin(), wired.end(), PAINT_MATERIAL_CHANNEL_METALLIC) !=
              wired.end());

  /* Dropping the channel from the set filters the row out without touching its record or map. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, false));
  EXPECT_FALSE(paint_layers::leaf_participates(*ma, *layer, PAINT_MATERIAL_CHANNEL_METALLIC));
  wired = paint_layers_wired_channels(*ma);
  EXPECT_TRUE(std::find(wired.begin(), wired.end(), PAINT_MATERIAL_CHANNEL_METALLIC) ==
              wired.end());
  ASSERT_EQ(layer->channels_num, 1);
  EXPECT_NE(layer->channels[0].image, nullptr);

  /* Adding it back restores participation. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, true));
  EXPECT_TRUE(paint_layers::leaf_participates(*ma, *layer, PAINT_MATERIAL_CHANNEL_METALLIC));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV mapping
 * \{ */

/** A Fill row whose Base Color is backed by a map. */
static MaterialPaintLayer *mapping_fill_add(PaintLayersGenerateTest &test, const char *name)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(*test.ma, name, nullptr, PaintLayerPlace::Above);
  EXPECT_TRUE(BKE_paint_layers_set_source(*test.ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  test.add_channel(*fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, test.add_image(name));
  return fill;
}

/** The nodes of \a tree that feed the `Vector` input of an Image Texture, by type. */
static Vector<int> image_vector_sources(const bNodeTree &tree)
{
  Vector<int> types;
  for (const bNodeLink &link : tree.links) {
    if (link.tonode->type_legacy == SH_NODE_TEX_IMAGE && STREQ(link.tosock->identifier, "Vector")) {
      types.append(link.fromnode->type_legacy);
    }
  }
  return types;
}

TEST_F(PaintLayersGenerateTest, mapping_builds_one_node_fed_by_one_coordinate_source)
{
  MaterialPaintLayer *fill = mapping_fill_add(*this, "Fill");
  /* A second map in another channel of the same row shares the one Mapping. */
  add_channel(*fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("FillRough"));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_COORD), 1);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 2);
  const Vector<int> sources = image_vector_sources(*group);
  ASSERT_EQ(sources.size(), 2);
  EXPECT_EQ(sources[0], SH_NODE_MAPPING);
  EXPECT_EQ(sources[1], SH_NODE_MAPPING);
  EXPECT_TRUE(interface_has_socket(*group, "Fill Mapping Offset", false));
  EXPECT_TRUE(interface_has_socket(*group, "Fill Mapping Scale", false));
  EXPECT_TRUE(interface_has_socket(*group, "Fill Mapping Rotation", false));
}

TEST_F(PaintLayersGenerateTest, an_unmapped_row_builds_no_mapping_and_no_coordinate)
{
  mapping_fill_add(*this, "Fill");
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_COORD), 0);
  EXPECT_TRUE(image_vector_sources(*group).is_empty()) << "the map keeps the implicit UV";
  EXPECT_FALSE(interface_has_socket(*group, "Fill Mapping Offset", false));

  /* Paint rows never map: strokes address the raw UV. */
  MaterialPaintLayer *painted = add_paint_layer("Painted", add_image("Painted"));
  EXPECT_FALSE(BKE_paint_layers_mapping_set_enabled(*ma, painted, true));
}

TEST_F(PaintLayersGenerateTest, toggling_the_mapping_changes_the_shape_and_dragging_does_not)
{
  MaterialPaintLayer *fill = mapping_fill_add(*this, "Fill");
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(group, nullptr);
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t plain = paint_layers::paint_layers_layer_topology_hash(*ma, *fill, wired, forms);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  const uint64_t mapped = paint_layers::paint_layers_layer_topology_hash(*ma, *fill, wired, forms);
  EXPECT_NE(mapped, plain);

  /* Offset, scale and rotation are inputs: a drag moves no hash and keeps the nodes. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(group, nullptr);
  const int mapping_nodes = count_type(*group, SH_NODE_MAPPING);
  EXPECT_EQ(mapping_nodes, 1);
  const float offset[2] = {0.25f, 0.5f};
  const float scale[2] = {2.0f, 3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, fill, offset));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(*ma, fill, false));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, fill, 0.5f));
  EXPECT_EQ(paint_layers::paint_layers_layer_topology_hash(*ma, *fill, wired, forms), mapped);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "a value edit syncs the inputs and leaves the nodes";
  EXPECT_EQ(count_type(*layer_tree_find(*bmain, "Fill"), SH_NODE_MAPPING), mapping_nodes);

  /* The inputs carry the new values. */
  group = layer_tree_find(*bmain, "Fill");
  const bNodeTreeInterfaceSocket *offset_socket = interface_input_find(*group,
                                                                       "Fill Mapping Offset");
  ASSERT_NE(offset_socket, nullptr);
  bNode *instance = group_instance_find(*ma->paint_layers_tree, *group);
  ASSERT_NE(instance, nullptr);
  const bNodeSocket *instance_offset = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(offset_socket->identifier));
  ASSERT_NE(instance_offset, nullptr);
  const float *value = static_cast<const bNodeSocketValueVector *>(
                           instance_offset->default_value)->value;
  EXPECT_FLOAT_EQ(value[0], 0.25f);
  EXPECT_FLOAT_EQ(value[1], 0.5f);

  /* Switching it off again takes the nodes and the inputs away. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, false));
  EXPECT_EQ(paint_layers::paint_layers_layer_topology_hash(*ma, *fill, wired, forms), plain);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
  EXPECT_FALSE(interface_has_socket(*group, "Fill Mapping Offset", false));
}

TEST_F(PaintLayersGenerateTest, a_channel_blend_is_a_node_property_not_a_new_shape)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_ROUGHNESS};
  const uint64_t plain = paint_layers::paint_layers_layer_topology_hash(*ma, *row, wired, forms);

  /* A plain mode change is the blend_type of the existing Mix nodes: the shape does not move. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_EQ(paint_layers::paint_layers_layer_topology_hash(*ma, *row, wired, forms), plain);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);

  /* Replace swaps the Normal node for a Mix: that is structure. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE));
  EXPECT_NE(paint_layers::paint_layers_layer_topology_hash(*ma, *row, wired, forms), plain);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersGenerateTest, a_channel_blend_edit_keeps_the_row_group)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  bNode *blend = nullptr;
  for (bNode &node : group->nodes) {
    if (paint_layers::prop_string_get(node.system_properties, paint_layers::BLEND_NODE_KEY_PROP)) {
      blend = &node;
      break;
    }
  }
  ASSERT_NE(blend, nullptr);
  ASSERT_NE(blend->storage, nullptr);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  /* The edit only asks for the sync: no group is rebuilt. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Row"), group);
  EXPECT_EQ(static_cast<NodeShaderMix *>(blend->storage)->blend_type, MA_RAMP_MULT);
}

TEST_F(PaintLayersGenerateTest, a_mapped_correction_and_mask_item_build_their_own_mapping)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, "Corr", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("CorrMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Item", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ItemMap")));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  /* Two rows map, so two Mapping nodes; they read the group's one coordinate source. */
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 2);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_COORD), 1);
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 3);
  int mapped = 0;
  for (const int source : image_vector_sources(*group)) {
    mapped += source == SH_NODE_MAPPING ? 1 : 0;
  }
  EXPECT_EQ(mapped, 2) << "the row's own Paint map keeps the implicit UV";
}

/** The Math nodes of \a operation in \a tree. */
static int math_node_count(const bNodeTree &tree, const int operation)
{
  int count = 0;
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_MATH && node.custom1 == operation) {
      count++;
    }
  }
  return count;
}

TEST_F(PaintLayersGenerateTest, a_clipping_wrap_builds_one_factor_per_clipped_axis_and_repeat_none)
{
  MaterialPaintLayer *fill = mapping_fill_add(*this, "Fill");
  /* A second map in another channel shares the row's one factor. */
  add_channel(*fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("FillRough"));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));

  struct WrapCase {
    int16_t wrap;
    int axes;
  };
  const WrapCase cases[] = {{MA_PAINT_LAYER_MAPPING_WRAP_REPEAT, 0},
                            {MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X, 1},
                            {MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y, 1},
                            {MA_PAINT_LAYER_MAPPING_WRAP_CLIP, 2}};
  for (const WrapCase &test : cases) {
    SCOPED_TRACE(::testing::Message() << "wrap=" << test.wrap);
    ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, test.wrap));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    bNodeTree *group = layer_tree_find(*bmain, "Fill");
    ASSERT_NE(group, nullptr);
    /* A floor and a compare per clipped axis; a Mapping and its maps exist either way. */
    EXPECT_EQ(math_node_count(*group, NODE_MATH_FLOOR), test.axes);
    EXPECT_EQ(math_node_count(*group, NODE_MATH_COMPARE), test.axes);
    EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
    EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 2);
  }
}

TEST_F(PaintLayersGenerateTest, the_wrap_is_topology_only_while_the_mapping_applies)
{
  MaterialPaintLayer *fill = mapping_fill_add(*this, "Fill");
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const auto row_hash = [&]() {
    return paint_layers::paint_layers_layer_topology_hash(*ma, *fill, wired, forms);
  };

  /* A wrap on a row whose mapping is off builds nothing: the hash is the one of Repeat. */
  const uint64_t plain = row_hash();
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  EXPECT_EQ(row_hash(), plain);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  const uint64_t clipped = row_hash();
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT));
  const uint64_t repeated = row_hash();
  EXPECT_NE(clipped, repeated);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X));
  const uint64_t repeat_x = row_hash();
  EXPECT_NE(repeat_x, clipped);
  EXPECT_NE(repeat_x, repeated);
}

TEST_F(PaintLayersGenerateTest, a_clipped_correction_and_mask_item_each_get_their_own_factor)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("Row"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, "Corr", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("CorrMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Item", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ItemMap")));
  for (MaterialPaintLayer *mapped : {correction, item}) {
    ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, mapped, true));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, mapped, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  }
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  /* Each mapped row owns a Mapping and a factor of two axes. */
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 2);
  EXPECT_EQ(math_node_count(*group, NODE_MATH_FLOOR), 4);
  EXPECT_EQ(math_node_count(*group, NODE_MATH_COMPARE), 4);
}

/** \} */
}  // namespace blender::bke::tests
