/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: core build, channels, folders, corrections, masks, regen and values (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_generate_test_fixture.hh` /
 * `paint_layers_generate_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_generate_test_fixture.hh"
#include "intern/paint_layers_generate_test_shared.hh"

namespace blender::bke::tests {


TEST_F(PaintLayersGenerateTest, color_chain_builds_one_mix_over_two_maps)
{
  Image *bottom = add_image("Bottom");
  Image *top = add_image("Top");
  add_paint_layer("Bottom", bottom);
  add_paint_layer("Top", top);

  bNodeTree *tree = make_tree("PBR Layers A");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  /* One Color channel is wired here (Base Color). The chain starts from a transparent constant, so
   * the two maps produce two Mix nodes. The Group Output and its wiring are the #Main-side
   * regenerate step's job, not this pure build. */
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 2);
  EXPECT_EQ(count_type(*tree, SH_NODE_MIX), 2);
}

TEST_F(PaintLayersGenerateTest, build_is_deterministic)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));

  bNodeTree *first = make_tree("PBR Layers First");
  bNodeTree *second = make_tree("PBR Layers Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *first, ctx);
  paint_layers_tree_build(*ma, *second, ctx);

  EXPECT_EQ(count_type(*first, SH_NODE_TEX_IMAGE), count_type(*second, SH_NODE_TEX_IMAGE));
  EXPECT_EQ(count_type(*first, SH_NODE_MIX),
            count_type(*second, SH_NODE_MIX));
}

TEST_F(PaintLayersGenerateTest, regenerate_builds_group_instance_and_principled)
{
  add_paint_layer("Bottom", add_image("Bottom"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_TRUE(report.created_principled);

  bNode *instance = instance_find();
  ASSERT_NE(instance, nullptr);
  ASSERT_NE(interface_output_find("Result Base Color"), nullptr);

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  EXPECT_FALSE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN);
}

TEST_F(PaintLayersGenerateTest, authored_fill_wires_the_default_channels)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  /* The raw constructor adds no channels; the authored policy is what gives them. */
  EXPECT_EQ(fill->channels_num, 0);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  EXPECT_EQ(fill->channels_num, 5);
  /* Idempotent: a second call adds nothing. */
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  EXPECT_EQ(fill->channels_num, 5);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  for (const char *socket_name : {"Base Color", "Metallic", "Roughness"}) {
    bNodeSocket *socket = bke::node_find_socket(
        const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(socket_name));
    ASSERT_NE(socket, nullptr) << socket_name;
    EXPECT_FALSE(socket->directly_linked_links().is_empty()) << socket_name;
  }
  /* The channels that would change the whole material's transparency or glow are not default. */
  for (const char *socket_name : {"Alpha", "Emission Color"}) {
    bNodeSocket *socket = bke::node_find_socket(
        const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(socket_name));
    if (socket != nullptr) {
      EXPECT_TRUE(socket->directly_linked_links().is_empty()) << socket_name;
    }
  }
}

TEST_F(PaintLayersGenerateTest, channel_outside_set_is_not_generated)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *metallic = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Metallic"_ustr);
  ASSERT_NE(metallic, nullptr);
  EXPECT_FALSE(metallic->directly_linked_links().is_empty());

  /* Metallic leaves the set: the row no longer participates, so the channel is not generated. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  metallic = bke::node_find_socket(const_cast<bNode &>(*principled), SOCK_IN, "Metallic"_ustr);
  ASSERT_NE(metallic, nullptr);
  EXPECT_TRUE(metallic->directly_linked_links().is_empty());

  /* Base Color is always in the set, so it stays wired. */
  bNodeSocket *base_color = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
}

TEST_F(PaintLayersGenerateTest, disabled_channel_drops_its_map_from_the_tree)
{
  MaterialPaintLayer *layer = add_paint_layer("Paint", add_image("Base"));
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *base = nullptr;
  for (int i = 0; i < layer->channels_num; i++) {
    if (layer->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      base = &layer->channels[i];
    }
  }
  ASSERT_NE(base, nullptr);
  ASSERT_NE(base->image, nullptr);
  Image *base_image = base->image;
  /* Adding a channel reallocates the row's array, so re-resolve the record after this. */
  ASSERT_NE(add_channel(*layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Rough")), nullptr);
  base = nullptr;
  for (int i = 0; i < layer->channels_num; i++) {
    if (layer->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      base = &layer->channels[i];
    }
  }
  ASSERT_NE(base, nullptr);

  /* Whether the generated tree, or any nested group, samples \a image. */
  auto tree_samples = [](auto &&self, const bNodeTree &tree, const Image &image) -> bool {
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image.id) {
        return true;
      }
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        if (self(self, *reinterpret_cast<const bNodeTree *>(node.id), image)) {
          return true;
        }
      }
    }
    return false;
  };

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_TRUE(tree_samples(tree_samples, *ma->paint_layers_tree, *base_image));

  /* Disabling Base Color rebuilds the tree without its map; the record and its image stay. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_FALSE(tree_samples(tree_samples, *ma->paint_layers_tree, *base_image));
  EXPECT_EQ(base->state, MA_PAINT_LAYER_CHANNEL_DISABLED);
  EXPECT_EQ(base->image, base_image);

  /* Enabling it back brings the map's node back. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_TRUE(tree_samples(tree_samples, *ma->paint_layers_tree, *base_image));
  EXPECT_EQ(base->image, base_image);
}

TEST_F(PaintLayersGenerateTest, authored_paint_participates_but_covers_nothing)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *paint);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The channel is wired (the row participates), but the row has no constant to lay down, so the
   * chain has no map or Mix of its own for it. */
  ASSERT_NE(interface_output_find("Result Base Color"), nullptr);
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  /* Only the bottom map and no fill input for the paint row. */
  /* +warm: Bottom adds Warm Mask and Warm Effect maps. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 3);
}

TEST_F(PaintLayersGenerateTest, uv_map_name_wires_every_image_texture)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNode *uv = find_type(*ma->paint_layers_tree, SH_NODE_UVMAP);
  ASSERT_NE(uv, nullptr);
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(uv->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_STREQ(storage->uv_map, "UVMap");
  EXPECT_TRUE(every_tex_image_uv_wired(*ma->paint_layers_tree));
}

TEST_F(PaintLayersGenerateTest, uv_map_no_name_keeps_graph_unchanged)
{
  add_paint_layer("Bottom", add_image("Bottom"));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_UVMAP), 0);
}

TEST_F(PaintLayersGenerateTest, uv_map_name_change_rebuilds_same_name_keeps_group)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_io_sentinel_set(*group, 0.5f));

  /* The same name: the group's topology hash is unchanged, so its nodes are preserved. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group_io_sentinel_get(*group, 0.5f));

  /* A new name is topology: the group is rebuilt and its UV Map node carries the new name. */
  BLI_strncpy(ma->paint_layers_uv_map, "Other", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNode *uv = find_type(*ma->paint_layers_tree, SH_NODE_UVMAP);
  ASSERT_NE(uv, nullptr);
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(uv->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_STREQ(storage->uv_map, "Other");
}

TEST_F(PaintLayersGenerateTest, uv_map_node_adds_no_sampler)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int with_name = BKE_paint_layers_sampler_count(*ma);

  BLI_strncpy(ma->paint_layers_uv_map, "", sizeof(ma->paint_layers_uv_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int without_name = BKE_paint_layers_sampler_count(*ma);

  EXPECT_EQ(with_name, without_name);
}

TEST_F(PaintLayersGenerateTest, regenerate_is_idempotent)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *tree = ma->paint_layers_tree;
  const int images = count_type(*tree, SH_NODE_TEX_IMAGE);
  const int mixes = count_type(*tree, SH_NODE_MIX);
  const int instances = node_count(*ma->nodetree);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), images);
  EXPECT_EQ(count_type(*tree, SH_NODE_MIX), mixes);
  EXPECT_EQ(node_count(*ma->nodetree), instances);
}

TEST_F(PaintLayersGenerateTest, folder_builds_an_isolated_subchain)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The child's map is emitted, and the folder builds its isolated accumulation and overlay on top
   * of it (design §5): the sub-chain's Mix nodes, the P/a divide, the coverage chain. */
  /* +warm: the root folder adds a Warm Mask map. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 2);
  EXPECT_GE(count_type(*ma->paint_layers_tree, SH_NODE_MIX), 2);
  EXPECT_GE(count_type(*ma->paint_layers_tree, SH_NODE_MATH), 1);
  EXPECT_NE(interface_output_find("Result Base Color"), nullptr);
}

TEST_F(PaintLayersGenerateTest, fill_constant_has_no_map)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  const float color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, layer, color));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* +warm: the root Fill row adds a Warm Mask map per wired channel record (Base Color and
   * Roughness here): the mask builder looks the spare's image up through Base Color whatever
   * channel is being built, so the same shared image lands in one node per channel. The Fill
   * constant itself still builds no map of its own. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 2);
  /* The Fill constant lives on the layer's own group input, with a mirror on the root for the write
   * during evaluation (A1). */
  bNodeTree *fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(fill_tree, nullptr);
  ASSERT_NE(group_input_find(*fill_tree, "Fill Roughness"), nullptr);
  ASSERT_NE(interface_output_find("Result Roughness"), nullptr);

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  EXPECT_FALSE(roughness->directly_linked_links().is_empty());
}

TEST_F(PaintLayersGenerateTest, fill_without_base_color_builds_like_any_channel)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  const float color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, color));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *base_color = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());

  /* A switched-off Base Color unwires only Base Color: the row rebuilds like a row that never
   * opted into the channel, and its other channels keep their constants. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  base_color = bke::node_find_socket(const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_TRUE(base_color->directly_linked_links().is_empty());
  bNodeSocket *roughness = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  EXPECT_FALSE(roughness->directly_linked_links().is_empty());
  bNodeTree *fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(fill_tree, nullptr);
  EXPECT_EQ(group_input_find(*fill_tree, "Fill Base Color"), nullptr);
  EXPECT_NE(group_input_find(*fill_tree, "Fill Roughness"), nullptr);

  /* Enabling rewires the same record's constant; the stored colour never left the record. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  base_color = bke::node_find_socket(const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_FALSE(base_color->directly_linked_links().is_empty());
  fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(fill_tree, nullptr);
  EXPECT_NE(group_input_find(*fill_tree, "Fill Base Color"), nullptr);

  /* Removing the record takes the same path: Base Color unwired, Roughness untouched. */
  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  base_color = bke::node_find_socket(const_cast<bNode &>(*principled), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  EXPECT_TRUE(base_color->directly_linked_links().is_empty());
  roughness = bke::node_find_socket(const_cast<bNode &>(*principled), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  EXPECT_FALSE(roughness->directly_linked_links().is_empty());
  fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(fill_tree, nullptr);
  EXPECT_EQ(group_input_find(*fill_tree, "Fill Base Color"), nullptr);
  EXPECT_NE(group_input_find(*fill_tree, "Fill Roughness"), nullptr);
}

TEST_F(PaintLayersGenerateTest, fill_correction_per_channel_value_is_a_value_input)
{
  MaterialPaintLayer *owner = add_paint_layer("Paint", add_image("Bottom"));
  /* The owner must itself take part in Roughness, or the row has no chain in that channel and the
   * correction has nowhere to apply (the CPU skips it for the same reason). */
  add_channel(*owner, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Rough"));
  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillCorr");
  ASSERT_NE(corr, nullptr);
  const float fill[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  copy_v4_v4(corr->fill_color, fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* No records: the legacy single fallback socket, and no per-channel socket. */
  bNodeTree *layer_tree = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(layer_tree, nullptr);
  EXPECT_NE(group_input_find(*layer_tree, "Paint FillCorr Fill"), nullptr);
  EXPECT_EQ(group_input_find(*layer_tree, "Paint FillCorr Roughness"), nullptr);

  /* A live record adds its own constant input; the fallback socket stays for the other channels. */
  MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  rough->value[0] = rough->value[1] = rough->value[2] = 0.75f;
  rough->value[3] = 1.0f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  layer_tree = layer_tree_find(*bmain, "Paint");
  ASSERT_NE(layer_tree, nullptr);
  EXPECT_NE(group_input_find(*layer_tree, "Paint FillCorr Roughness"), nullptr);
  EXPECT_NE(group_input_find(*layer_tree, "Paint FillCorr Fill"), nullptr);

  /* A value-only edit does not move the topology, so the generated tree is not invalidated
   * (#MA_PAINT_LAYERS_REGEN stays clear; the bake-stale signal is a different, expected one). */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  const float value[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  EXPECT_TRUE(BKE_paint_layers_channel_set_value(*ma, corr, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersGenerateTest, values_sync_writes_opacity_and_enabled)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  BKE_paint_layers_set_opacity(*ma, layer, 0.37f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The value is mirrored onto the root instance in the material's embedded tree; the layer group's
   * own socket is fed from it by a link (A1). */
  bNodeSocket *socket = root_instance_input("Bottom Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.37f);

  BKE_paint_layers_set_enabled(*ma, layer, false);
  BKE_paint_layers_values_sync(*ma);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);
}

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

TEST_F(PaintLayersGenerateTest, foreign_tree_is_replaced_not_overwritten)
{
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
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

  /* Nothing is pending: a second pass is a no-op and does not rebuild the tree. */
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);

  /* A new edit marks it stale again, and only the scheduling point rebuilds it. */
  add_paint_layer("Top", add_image("Top"));
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  /* +warm: 2 root layers, each map + Warm Mask + Warm Effect. */
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), 6);
}

TEST_F(PaintLayersGenerateTest, mask_item_map_builds_its_chain)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &l) { return layer_tree_for(l); };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;

  /* The same layer built without a mask first, so the item's own nodes are measured as a delta. */
  bNodeTree *without_mask = make_tree("Without Mask");
  ASSERT_NE(without_mask, nullptr);
  paint_layers_tree_build(*ma, *without_mask, ctx);
  const int math_without_mask = count_type(*without_mask, SH_NODE_MATH);
  const int mix_without_mask = count_type(*without_mask, SH_NODE_MIX);

  MaterialPaintLayer *item_const = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item_const, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_add(*ma, item_const, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item_const, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Mask")));
  /* A map is read only while the item is a Paint one. */
  ASSERT_TRUE(
      BKE_paint_layers_correction_source_set(*ma, item_const, MA_PAINT_LAYER_SOURCE_IMAGE));
  bNodeTree *with_mask = make_tree("With Mask Map");
  ASSERT_NE(with_mask, nullptr);
  paint_layers_tree_build(*ma, *with_mask, ctx);

  /* The layer's map and the mask item's map. */
  EXPECT_EQ(count_type(*with_mask, SH_NODE_TEX_IMAGE), 2);
  /* The item adds its grey chain and a Mix. */
  EXPECT_GT(count_type(*with_mask, SH_NODE_MATH), math_without_mask);
  EXPECT_GT(count_type(*with_mask, SH_NODE_MIX), mix_without_mask);
}

TEST_F(PaintLayersGenerateTest, constant_mask_item_adds_no_map)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &l) { return layer_tree_for(l); };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;

  bNodeTree *without_mask = make_tree("Without Mask");
  ASSERT_NE(without_mask, nullptr);
  paint_layers_tree_build(*ma, *without_mask, ctx);
  const int mix_without_mask = count_type(*without_mask, SH_NODE_MIX);

  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 0.5f), nullptr);
  bNodeTree *with_mask = make_tree("With Constant Mask");
  ASSERT_NE(with_mask, nullptr);
  paint_layers_tree_build(*ma, *with_mask, ctx);

  /* A constant item adds no map, only its Mix. */
  EXPECT_EQ(count_type(*with_mask, SH_NODE_TEX_IMAGE), 1);
  EXPECT_GT(count_type(*with_mask, SH_NODE_MIX), mix_without_mask);
}

TEST_F(PaintLayersGenerateTest, content_correction_adds_a_mix_over_the_map)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("Correction");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Two layer maps and the correction map, each layer and the correction with one Mix. */
  /* +warm: a fresh Warm Mask and Warm Effect stay beside the real correction. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 7);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_MIX), 9); /* +warm: as above, plus a base. */
}


/**
 * F2-C5: a content correction whose map is read as colour data reaches the chain pre-multiplied, so
 * the generator straightens it with a CombineXYZ plus a Vector Math divide. A colour-space map is
 * un-premultiplied by the Image Texture node itself, so no such nodes are built.
 */
TEST_F(PaintLayersGenerateTest, content_correction_data_map_builds_a_straighten_divide)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionData");
  make_generate_image_data(*record->image);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_COMBXYZ), 1);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_VECTOR_MATH), 1);
}

TEST_F(PaintLayersGenerateTest, content_correction_color_map_builds_no_straighten_divide)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionColor");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_COMBXYZ), 0);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_VECTOR_MATH), 0);
}

TEST_F(PaintLayersGenerateTest, fill_effect_correction_builds_a_constant_mix)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, correction, green));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The layer's map and the channel's bottom constant; the Fill correction's colour comes from a
   * group input, not a node, and each row contributes a Mix. */
  /* +warm: Warm Mask and Warm Effect maps. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 3);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_RGB), 1);
  /* +warm: Warm Mask, Warm Effect and mask base Mix. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_MIX), 5);
}

/** Stage 1 mapping: values ride group inputs, so they never move the topology hash, while the
 * Mapping node itself (toggled by `enabled` with a map present) does. */
TEST_F(PaintLayersGenerateTest, mapping_values_do_not_move_topology_hash_but_enabled_does)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_plain = paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1));

  /* Enabling the mapping adds the row's Mapping node: the hash moves once. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  const uint64_t hash_mapped = paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1));
  EXPECT_NE(hash_mapped, hash_plain);

  /* Sliding offset/scale/rotation syncs through group inputs instead: the hash stands. */
  const float offset[2] = {0.25f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, correction, offset));
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1)), hash_mapped);
  const float scale[2] = {2.0f, 2.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, correction, scale));
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1)), hash_mapped);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, correction, 0.5f));
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1)), hash_mapped);
}

/** Stage 1 mapping: one Mapping node per row between the one coordinate source and the row's map,
 * with its Location/Rotation/Scale driven by the row group's inputs. */
TEST_F(PaintLayersGenerateTest, mapping_builds_one_node_on_one_coordinate_source)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  /* A correction only reaches channels its owner takes part in. */
  add_channel(*bottom, PAINT_MATERIAL_CHANNEL_METALLIC, add_image("BottomMetal"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  /* Adding a channel may reallocate the records, so the images are kept by pointer. */
  Image *const base_map = record->image;
  /* A second mapped channel on the same row: it must reuse the row's one Mapping node. */
  MaterialPaintLayerChannel *metal_record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_METALLIC);
  ASSERT_NE(metal_record, nullptr);
  metal_record->image = add_image("CorrectionMetalMap");
  metal_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  Image *const metal_map = metal_record->image;
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));

  bNodeTree *tree = make_tree("PBR Layers Mapping");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  /* The tree was built without a topology update; the link queries below read that cache. */
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(*bmain, *group);
  /* One Mapping node for the correction, fed by the tree's one coordinate source. It is shared by
   * every channel and map of the row, so a second mapped channel adds no node. */
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  EXPECT_EQ(count_type(*group, SH_NODE_UVMAP) + count_type(*group, SH_NODE_TEX_COORD), 1);
  bNode *mapping = find_type(*group, SH_NODE_MAPPING);
  ASSERT_NE(mapping, nullptr);
  bNodeSocket *mapping_in = bke::node_find_socket(*mapping, SOCK_IN, "Vector"_ustr);
  ASSERT_NE(mapping_in, nullptr);
  ASSERT_FALSE(mapping_in->directly_linked_links().is_empty());
  const bNode *source = mapping_in->directly_linked_links()[0]->fromnode;
  ASSERT_NE(source, nullptr);
  EXPECT_TRUE(source->type_legacy == SH_NODE_UVMAP || source->type_legacy == SH_NODE_TEX_COORD);
  /* The correction's maps read through the Mapping node, which repeats the texture. Both the
   * Base Color and the Metallic map share the row's one node. */
  int maps_through_mapping = 0;
  bool map_repeat = false;
  for (bNode &node : group->nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE ||
        (node.id != &base_map->id && node.id != &metal_map->id))
    {
      continue;
    }
    bNodeSocket *vector = bke::node_find_socket(node, SOCK_IN, "Vector"_ustr);
    ASSERT_NE(vector, nullptr);
    ASSERT_FALSE(vector->directly_linked_links().is_empty());
    if (vector->directly_linked_links()[0]->fromnode == mapping) {
      maps_through_mapping++;
    }
    if (const NodeTexImage *storage = static_cast<const NodeTexImage *>(node.storage)) {
      map_repeat = map_repeat || storage->extension == SHD_IMAGE_EXTENSION_REPEAT;
    }
  }
  EXPECT_EQ(maps_through_mapping, 2);
  EXPECT_TRUE(map_repeat);
  /* The row group's interface carries the three mapping value inputs. */
  EXPECT_TRUE(interface_has_value_for(
      *group, correction->marker, bke::paint_layers::ROLE_MAPPING_OFFSET));
  EXPECT_TRUE(interface_has_value_for(
      *group, correction->marker, bke::paint_layers::ROLE_MAPPING_SCALE));
  EXPECT_TRUE(interface_has_value_for(
      *group, correction->marker, bke::paint_layers::ROLE_MAPPING_ROTATION));
}

/** Stage 1 mapping: a Fill mask that carries a map reads it through its own Mapping node (with
 * the grey chain reducing it after), instead of staying the constant. */
TEST_F(PaintLayersGenerateTest, mapping_applies_to_fill_mask_map)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *mask_item = BKE_paint_layers_mask_add(*ma, bottom, 1.0f);
  ASSERT_NE(mask_item, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, mask_item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("MaskMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_mapping_supported(*ma, *mask_item));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, mask_item, true));

  bNodeTree *tree = make_tree("PBR Layers Mask Mapping");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  /* The tree was built without a topology update; the link queries below read that cache. */
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(*bmain, *group);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
  bNode *mapping = find_type(*group, SH_NODE_MAPPING);
  ASSERT_NE(mapping, nullptr);
  bool map_through_mapping = false;
  bool map_repeat = false;
  /* Why nested: the mask stack packs into its own `.PL Mask` group, so the mapped image lives
   * there instead of flat in the row group. */
  Vector<bNodeTree *> mask_groups;
  collect_group_instances(*group, mask_groups);
  auto check_tree = [&](bNodeTree &tree) {
    for (bNode &node : tree.nodes) {
      if (node.type_legacy != SH_NODE_TEX_IMAGE || node.id != &record->image->id) {
        continue;
      }
      bNodeSocket *vector = bke::node_find_socket(node, SOCK_IN, "Vector"_ustr);
      ASSERT_NE(vector, nullptr);
      ASSERT_FALSE(vector->directly_linked_links().is_empty());
      if (vector->directly_linked_links()[0]->fromnode == mapping) {
        map_through_mapping = true;
      }
      if (const NodeTexImage *storage = static_cast<const NodeTexImage *>(node.storage)) {
        map_repeat = map_repeat || storage->extension == SHD_IMAGE_EXTENSION_REPEAT;
      }
    }
  };
  check_tree(*group);
  for (bNodeTree *sub : mask_groups) {
    if (sub != nullptr) {
      /* Why: the nested group was built without a topology update either, and the link queries in
       * #check_tree read that cache. */
      BKE_ntree_update_tag_all(sub);
      BKE_ntree_update_after_single_tree_change(*bmain, *sub);
      check_tree(*sub);
    }
  }
  EXPECT_TRUE(map_through_mapping);
  EXPECT_TRUE(map_repeat);
}

/** Stage 1 mapping: a bare enabled flag without a map builds no Mapping node and moves no hash. */
TEST_F(PaintLayersGenerateTest, mapping_without_a_map_builds_no_node)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);

  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_before = paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1));
  /* The BKE setter refuses this; write the flag directly to model a stale file. */
  correction->mapping.flag |= MA_PAINT_LAYER_MAPPING_ENABLED;
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *bottom, Span<int>(wired, 1)), hash_before);

  bNodeTree *tree = make_tree("PBR Layers Mapping Bare");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 0);
}

/** Stage 1 mapping: a second regeneration without edits duplicates neither Mapping nodes nor the
 * row group's mapping inputs. */
TEST_F(PaintLayersGenerateTest, second_regenerate_keeps_mapping_nodes_and_inputs)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("CorrectionMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int mappings = count_type(*ma->paint_layers_tree, SH_NODE_MAPPING);
  EXPECT_EQ(mappings, 1);
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  const int64_t inputs = group->interface_inputs().size();

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_MAPPING), mappings);
  group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  EXPECT_EQ(group->interface_inputs().size(), inputs);
}

TEST_F(PaintLayersGenerateTest, scene_graph_update_runs_the_scheduling_point)
{
  /* The paths without an event loop -- a script's depsgraph.update(), a background render -- reach
   * the scheduling point through BKE_scene_graph_update_tagged, not through the WM. */
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ASSERT_NE(scene, nullptr);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  ASSERT_NE(view_layer, nullptr);
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
  ASSERT_NE(depsgraph, nullptr);

  add_paint_layer("Bottom", add_image("Bottom"));
  EXPECT_EQ(ma->paint_layers_tree, nullptr);

  BKE_scene_graph_update_tagged(depsgraph, bmain);
  EXPECT_NE(ma->paint_layers_tree, nullptr);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersGenerateTest, regenerate_tagged_is_main_thread_only)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  EXPECT_EQ(ma->paint_layers_tree, nullptr);

  std::thread worker([this]() { BKE_paint_layers_regenerate_tagged(*bmain); });
  worker.join();

  /* Rule K-1: a worker thread must not create IDs or write node trees into Main. */
  EXPECT_EQ(ma->paint_layers_tree, nullptr);
}

TEST_F(PaintLayersGenerateTest, localized_copy_is_not_regenerated)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *tree = ma->paint_layers_tree;
  ASSERT_NE(tree, nullptr);
  const int images = count_type(*tree, SH_NODE_TEX_IMAGE);

  Material *copy = id_cast<Material *>(BKE_id_copy_ex(bmain,
                                                      &ma->id,
                                                      nullptr,
                                                      LIB_ID_COPY_DEFAULT | LIB_ID_CREATE_LOCAL |
                                                          LIB_ID_CREATE_NO_USER_REFCOUNT));
  ASSERT_NE(copy, nullptr);
  /* A localized copy shares the tree; regenerating it would rewrite the original. */
  ASSERT_EQ(copy->paint_layers_tree, tree);
  copy->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;

  BKE_paint_layers_regenerate_tagged(*bmain);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  EXPECT_EQ(count_type(*tree, SH_NODE_TEX_IMAGE), images);
  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, set_enabled_is_a_value_edit)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_values_sync(*ma);
  /* The disabled row's factor is written to the root instance mirror (A1). */
  bNodeSocket *socket = root_instance_input("Bottom Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.0f);
}

TEST_F(PaintLayersGenerateTest, enabled_toggle_stabilizes_the_root_in_one_pass)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);

  /* Disabling a row is a value edit while the graph is kept; the rebuild that does happen (here the
   * first, which creates the tree) records it. The stored root hash must describe the graph that
   * rebuild produced, so the next unchanged pass keeps the root instead of rebuilding a second
   * time -- that second rebuild is the extra EEVEE compile D1 removes. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, off, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes_before = root_nodes(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(nodes_before, root_nodes(*root)));

  /* Enabling the recorded row moves the topology hash back: exactly one rebuild restores it, and
   * the pass after that must keep the root again (both directions). */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, off, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *rebuilt = ma->paint_layers_tree;
  ASSERT_NE(rebuilt, nullptr);
  const Vector<bNode *> rebuilt_nodes = root_nodes(*rebuilt);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, rebuilt);
  EXPECT_TRUE(same_nodes(rebuilt_nodes, root_nodes(*rebuilt)));
}

TEST_F(PaintLayersGenerateTest, disabled_row_survives_a_runtime_state_reload_in_one_pass)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, off, false));

  /* A rebuild records the disabled row and drops it from the graph. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  /* Loading the file drops the runtime removed set (#removed_rows_state is not saved), while the
   * stored root hash still describes the graph that omitted the row -- exactly the mismatch a load
   * produces. The recovery must take at most one rebuild. */
  BKE_paint_layers_generate_runtime_free(*ma);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *settled = ma->paint_layers_tree;
  ASSERT_NE(settled, nullptr);
  const Vector<bNode *> settled_nodes = root_nodes(*settled);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, settled) << "a load must not rebuild the root forever";
  EXPECT_TRUE(same_nodes(settled_nodes, root_nodes(*settled)));
}

TEST_F(PaintLayersGenerateTest, disabled_row_survives_an_undo_of_the_stored_root_in_one_pass)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);

  /* Build with the row present and remember the hash that describes that graph. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  uint64_t row_present_hash = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*ma->paint_layers_tree, row_present_hash));

  /* Disable it; the rebuild drops the row and stores the hash of the graph without it. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, off, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* A memfile undo restores the stored root hash from its snapshot but not the runtime removed set,
   * so write back the pre-drop hash while the row stays recorded. Recovery must take at most one
   * rebuild. */
  bke::paint_layers::tree_root_hash_set(*ma->paint_layers_tree, row_present_hash);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *settled = ma->paint_layers_tree;
  ASSERT_NE(settled, nullptr);
  const Vector<bNode *> settled_nodes = root_nodes(*settled);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, settled) << "an undone root hash must not rebuild forever";
  EXPECT_TRUE(same_nodes(settled_nodes, root_nodes(*settled)));
}

TEST_F(PaintLayersGenerateTest, constant_mask_value_is_a_value_edit)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.5f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  const float quarter[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, item, quarter));
  /* A constant item's colour is a group input: value-only, no rebuild. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0);
}

TEST_F(PaintLayersGenerateTest, correction_opacity_is_synced_without_regen)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, correction, green));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  /* Changing the value does not mark the topology stale. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.25f));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_values_sync(*ma);
  bNodeSocket *socket = root_instance_input("Bottom C Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.25f);
}

TEST_F(PaintLayersGenerateTest, mask_correction_builds_a_factor_chain)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
  ASSERT_NE(correction, nullptr);
  /* A mask correction lays its coverage over the factor by its own blend (default Mix here). */
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("MaskCorrection");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The chain builds and updates without a malformed graph, and the channel is still wired. */
  EXPECT_NE(interface_output_find("Result Base Color"), nullptr);
  /* +warm: a fresh Warm Mask and Warm Effect stay beside the real mask. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 7);
}

TEST_F(PaintLayersGenerateTest, pure_build_instantiates_normal_combine_with_links)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  /* A content Paint correction on the Normal channel is a second Combine instance. */
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Correction"));

  bNodeTree *tree = make_tree("PBR Layers Normal");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(*bmain);
  ASSERT_NE(ctx.normal_combine_group, nullptr);
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  /* No #Main reachable from the build itself: the group sockets come from the node declaration. */
  paint_layers_tree_build(*ma, *tree, ctx);

  /* The combines live inside the layer groups now, so walk the tree recursively. */
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
  /* One Combine for the row itself, one for its content correction. */
  EXPECT_EQ(combines, 2);
}

TEST_F(PaintLayersGenerateTest, pure_build_matches_regenerate_topology)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*bottom, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_image("Top"));
  add_channel(*top, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("TopNormal"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("MaskCorr"));

  PaintLayersBuildContext ctx;
  ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(*bmain);
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  bNodeTree *pure = make_tree("PBR Layers Pure");
  ASSERT_NE(pure, nullptr);
  paint_layers_tree_build(*ma, *pure, ctx);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  auto idname_multiset = [](const bNodeTree &tree) {
    Vector<std::string> names;
    for (const bNode &node : tree.nodes) {
      names.append(std::string(node.idname));
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  const Vector<std::string> pure_names = idname_multiset(*pure);
  const Vector<std::string> regen_names = idname_multiset(*ma->paint_layers_tree);
  EXPECT_EQ(pure_names, regen_names);
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
    EXPECT_TRUE(interface_has_socket(*group, "Color Base Color", true));
    EXPECT_TRUE(interface_has_socket(*group, "Coverage Base Color", true));
    EXPECT_TRUE(interface_has_socket(*group, "Blend Base Color", true));
    EXPECT_TRUE(interface_has_socket(*group, "Result Base Color", true));
    /* The tree is marked as that layer's own group. */
    const IDProperty *props = IDP_GetProperties(&group->id);
    ASSERT_NE(props, nullptr);
    EXPECT_NE(IDP_GetPropertyTypeFromGroup(props, "pbr_paint_layers_layer_tree", IDP_STRING),
              nullptr);
  }

  /* One instance per layer, and nothing else, in the root tree. */
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

  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom, nullptr);
  ASSERT_NE(top, nullptr);
  EXPECT_EQ(layer_tree_count(*bmain), 2);
  EXPECT_EQ(ID_REAL_USERS(&bottom->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&top->id), 1);
  /* The layer groups read their maps, so each image is referenced by the generated graph. */
  const int bottom_image_users = ID_REAL_USERS(&bottom_image->id);
  const int top_image_users = ID_REAL_USERS(&top_image->id);
  EXPECT_GT(bottom_image_users, 0);
  EXPECT_GT(top_image_users, 0);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom);
  EXPECT_EQ(layer_tree_find(*bmain, "Top"), top);
  EXPECT_EQ(layer_tree_count(*bmain), 2);
  /* The scratch build takes and must drop its own user references; the real groups and the maps
   * keep exactly the users they had, or a kept root would leak one per regeneration. */
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

TEST_F(PaintLayersGenerateTest, folder_group_holds_its_children)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "ChildA", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *child_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "ChildB", folder, PaintLayerPlace::Into);
  ASSERT_NE(child_a, nullptr);
  ASSERT_NE(child_b, nullptr);
  add_channel(*child_a, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ChildA"));
  add_channel(*child_b, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("ChildB"));
  /* Opacity below one keeps the folder isolating; a default folder would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  bNodeTree *child_a_tree = layer_tree_find(*bmain, "ChildA");
  bNodeTree *child_b_tree = layer_tree_find(*bmain, "ChildB");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_NE(child_a_tree, nullptr);
  ASSERT_NE(child_b_tree, nullptr);

  /* The folder exposes the same contract as a leaf. */
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Below Base Color", false));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Color Base Color", true));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Coverage Base Color", true));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Blend Base Color", true));
  EXPECT_TRUE(interface_has_socket(*folder_tree, "Result Base Color", true));

  /* Children instantiated inside the folder, the folder in the root, and no child in the root. */
  EXPECT_NE(group_instance_find(*folder_tree, *child_a_tree), nullptr);
  EXPECT_NE(group_instance_find(*folder_tree, *child_b_tree), nullptr);
  EXPECT_NE(group_instance_find(*ma->paint_layers_tree, *folder_tree), nullptr);
  EXPECT_EQ(group_instance_find(*ma->paint_layers_tree, *child_a_tree), nullptr);
  EXPECT_EQ(group_instance_find(*ma->paint_layers_tree, *child_b_tree), nullptr);
  EXPECT_EQ(layer_group_tree_count(*bmain), 3);
}

TEST_F(PaintLayersGenerateTest, regenerate_reuses_folder_trees)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating; a default folder would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  bNodeTree *child_tree = layer_tree_find(*bmain, "Child");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_NE(child_tree, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), folder_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Child"), child_tree);
  EXPECT_EQ(layer_group_tree_count(*bmain), 2);
  EXPECT_EQ(ID_REAL_USERS(&folder_tree->id), 1);
  EXPECT_EQ(ID_REAL_USERS(&child_tree->id), 1);
}

TEST_F(PaintLayersGenerateTest, removed_folder_drops_its_subtree)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating; a default folder would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(folder_tree_find(*bmain, "Folder"), nullptr);
  ASSERT_NE(layer_tree_find(*bmain, "Child"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_EQ(layer_tree_find(*bmain, "Child"), nullptr);
  EXPECT_EQ(layer_group_tree_count(*bmain), 0);
}

TEST_F(PaintLayersGenerateTest, copied_material_owns_nested_trees)
{
  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Leaf", inner, PaintLayerPlace::Into);
  add_channel(*leaf, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Leaf"));
  /* Opacity below one keeps both folders isolating; default folders would pass through. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *outer_tree = folder_tree_find(*bmain, "Outer");
  bNodeTree *inner_tree = folder_tree_find(*bmain, "Inner");
  bNodeTree *leaf_tree = layer_tree_find(*bmain, "Leaf");
  ASSERT_NE(outer_tree, nullptr);
  ASSERT_NE(inner_tree, nullptr);
  ASSERT_NE(leaf_tree, nullptr);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);

  Vector<bNodeTree *> level1;
  collect_chain_group_instances(*copy->paint_layers_tree, level1);
  ASSERT_EQ(level1.size(), 1);
  bNodeTree *copy_outer = level1[0];
  Vector<bNodeTree *> level2;
  collect_chain_group_instances(*copy_outer, level2);
  ASSERT_EQ(level2.size(), 1);
  bNodeTree *copy_inner = level2[0];
  Vector<bNodeTree *> level3;
  collect_chain_group_instances(*copy_inner, level3);
  ASSERT_EQ(level3.size(), 1);
  bNodeTree *copy_leaf = level3[0];

  EXPECT_NE(copy_outer, outer_tree);
  EXPECT_NE(copy_inner, inner_tree);
  EXPECT_NE(copy_leaf, leaf_tree);
  const IDProperty *props = IDP_GetProperties(&copy_leaf->id);
  ASSERT_NE(props, nullptr);
  EXPECT_NE(IDP_GetPropertyTypeFromGroup(props, "pbr_paint_layers_layer_tree", IDP_STRING),
            nullptr);

  /* Regenerating the copy leaves the original's nested trees alone. */
  const int original_images = count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *copy));
  EXPECT_EQ(folder_tree_find(*bmain, "Outer"), outer_tree);
  EXPECT_EQ(folder_tree_find(*bmain, "Inner"), inner_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Leaf"), leaf_tree);
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), original_images);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersGenerateTest, baked_row_is_its_own_group)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*layer);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("BakedColor");
  bake->coverage = add_image("BakedCoverage");
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  Image *substituted = nullptr;
  ASSERT_TRUE(BKE_paint_layers_bake_substitute(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &substituted));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(interface_has_socket(*group, "Below Base Color", false));
  EXPECT_TRUE(interface_has_socket(*group, "Result Base Color", true));
  EXPECT_NE(group_instance_find(*ma->paint_layers_tree, *group), nullptr);
  /* The bake's colour and coverage stand in: two Image Texture nodes, not the live map. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 3); /* +warm: Warm Mask map. */
}

TEST_F(PaintLayersGenerateTest, root_holds_only_instances_and_chain_ends)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Base Color only: the root keeps the group I/O, the channel's bottom constant and the group
   * instances. A Normal channel would add its decode-normalize-encode Vector Math at the end.
   * F2-B adds the Base Color content-alpha chain (Value/Math) and its final Compose Color Alpha
   * before the Result output. */
  for (bNode &node : ma->paint_layers_tree->nodes) {
    const bool allowed = node.is_group_input() || node.is_group_output() || node.is_group() ||
                         node.type_legacy == SH_NODE_RGB || node.type_legacy == SH_NODE_VALUE ||
                         node.type_legacy == SH_NODE_MATH ||
                         node.type_legacy == SH_NODE_COMPOSE_COLOR_ALPHA;
    EXPECT_TRUE(allowed) << node.idname;
  }
}

TEST_F(PaintLayersGenerateTest, unchanged_layer_tree_is_not_rebuilt)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom, nullptr);
  const int nodes_before = node_count(*bottom);
  Vector<bNode *> pointers_before;
  for (bNode &node : bottom->nodes) {
    pointers_before.append(&node);
  }
  ASSERT_TRUE(group_mix_sentinel_set(*bottom, 0.125f));

  /* A regenerate without any edit must leave the layer group's nodes alone. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_after = layer_tree_find(*bmain, "Bottom");
  ASSERT_EQ(bottom_after, bottom);
  EXPECT_EQ(node_count(*bottom_after), nodes_before);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_after, 0.125f));
  Vector<bNode *> pointers_after;
  for (bNode &node : bottom_after->nodes) {
    pointers_after.append(&node);
  }
  ASSERT_EQ(pointers_after.size(), pointers_before.size());
  for (const int i : pointers_before.index_range()) {
    EXPECT_EQ(pointers_after[i], pointers_before[i]);
  }
}

TEST_F(PaintLayersGenerateTest, value_edit_does_not_rebuild_any_group)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *fill_tree = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(fill_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*fill_tree, 0.5f));

  /* Opacity and a Fill constant are group inputs: they change values, not topology. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, bottom, 0.42f));
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, green));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  EXPECT_EQ(layer_tree_find(*bmain, "Fill"), fill_tree);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*fill_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, map_change_rebuilds_only_its_layer)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("BottomReplaced")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  ASSERT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
  /* The map change rebuilds Bottom's group (the stamp is gone) but not Top's. */
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, correction_add_rebuilds_only_its_layer)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Correction")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  ASSERT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
  /* The new effect changes Bottom's topology only. */
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, folder_mask_rebuilds_the_root_and_syncs)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating, so adding the mask only grows its interface. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *child_tree = layer_tree_find(*bmain, "Child");
  ASSERT_NE(child_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*child_tree, 0.25f));

  /* A mask on the folder adds a value input to the folder's group; the root now carries a mirror for
   * every value, so its interface and links change and it rebuilds (A1). */
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, folder, 0.5f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  /* +warm: the mask takes the spare's slot and its mirrors are keyed by the slot, so the root keeps
   * its nodes. */
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  /* The mask's value lives on the folder group, the child group is untouched, and the value is
   * mirrored onto the root instance. */
  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_tree, nullptr);
  /* +warm: the base takes the spare's slot, so its value lives in the spare's socket. */
  bNodeTreeInterfaceSocket *iface = group_input_find(
      *folder_tree, "Folder Warm Base Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  EXPECT_NE(root_instance_input("Folder Warm Base Base Color Opacity"), nullptr);
  EXPECT_TRUE(group_mix_sentinel_get(*child_tree, 0.25f));
}

TEST_F(PaintLayersGenerateTest, root_interface_carries_value_mirrors)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  /* The root carries a mirror for every value in the stack; values are written there and linked
   * down to the layer groups (A1). */
  bNodeTreeInterfaceSocket *iface = interface_input_find("Bottom Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  const IDProperty *mirror = IDP_GetPropertyTypeFromGroup(
      iface->properties, "pbr_paint_layers_mirror", IDP_INT);
  ASSERT_NE(mirror, nullptr);
  EXPECT_EQ(IDP_int_get(mirror), 1);
  EXPECT_TRUE(interface_has_socket(*ma->paint_layers_tree, "Result Base Color", true));
  EXPECT_FALSE(interface_has_socket(*ma->paint_layers_tree, "Below Base Color", false));
}

TEST_F(PaintLayersGenerateTest, layer_group_owns_its_value_inputs)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  BKE_paint_layers_set_opacity(*ma, layer, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bNodeTreeInterfaceSocket *opacity = group_input_find(*group, "Bottom Base Color Opacity");
  ASSERT_NE(opacity, nullptr);
  ASSERT_NE(opacity->properties, nullptr);
  const IDProperty *role = IDP_GetPropertyTypeFromGroup(
      opacity->properties, "pbr_paint_layers_role", IDP_STRING);
  ASSERT_NE(role, nullptr);
  EXPECT_STREQ(IDP_string_get(role), "opacity");
  const IDProperty *channel = IDP_GetPropertyTypeFromGroup(
      opacity->properties, "pbr_paint_layers_channel", IDP_INT);
  ASSERT_NE(channel, nullptr);
  EXPECT_EQ(IDP_int_get(channel), PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NE(IDP_GetPropertyTypeFromGroup(
                opacity->properties, "pbr_paint_layers_layer", IDP_STRING),
            nullptr);
}

TEST_F(PaintLayersGenerateTest, values_sync_writes_to_the_instance)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.42f));
  BKE_paint_layers_values_sync(*ma);

  /* The value is written to the root instance mirror (A1). */
  bNodeSocket *socket = root_instance_input("Bottom Base Color Opacity");
  ASSERT_NE(socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(socket->default_value)->value, 0.42f);
}

TEST_F(PaintLayersGenerateTest, values_sync_writes_to_a_nested_instance)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating, so the child instance lives in its tree. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, child, 0.33f));
  BKE_paint_layers_values_sync(*ma);

  /* The child's value is written to the root mirror and relayed down through the folder interface
   * into the child instance (A1). */
  bNodeSocket *root_socket = root_instance_input("Child Base Color Opacity");
  ASSERT_NE(root_socket, nullptr);
  EXPECT_FLOAT_EQ(static_cast<bNodeSocketValueFloat *>(root_socket->default_value)->value, 0.33f);

  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  bNodeTree *child_tree = layer_tree_find(*bmain, "Child");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_NE(child_tree, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*child_tree, "Child Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  bNodeSocket *child_instance_socket = group_instance_input(*folder_tree, *child_tree, *iface);
  ASSERT_NE(child_instance_socket, nullptr);
  EXPECT_NE(child_instance_socket->link, nullptr);
}

TEST_F(PaintLayersGenerateTest, deleting_a_nested_effect_drops_its_mirrors)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Isolating: the child's group and its mirrors live inside the folder. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, child, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  const bUUID effect_marker = effect->marker;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_TRUE(interface_has_value_for(*folder_tree, effect_marker, "correction_opacity"));
  ASSERT_TRUE(
      interface_has_value_for(*ma->paint_layers_tree, effect_marker, "correction_opacity"));

  /* Removing the effect must prune its relay on the folder's interface and the root's. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, effect));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  folder_tree = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_tree, nullptr);
  EXPECT_FALSE(interface_has_value_for(*folder_tree, effect_marker, "correction_opacity"));
  EXPECT_FALSE(
      interface_has_value_for(*ma->paint_layers_tree, effect_marker, "correction_opacity"));
}

TEST_F(PaintLayersGenerateTest, deleting_a_folder_child_drops_its_mirrors)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *drop = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Drop", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *keep = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Keep", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(drop, nullptr);
  ASSERT_NE(keep, nullptr);
  add_channel(*drop, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Drop"));
  add_channel(*keep, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Keep"));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  const bUUID drop_marker = drop->marker;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *folder_tree = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_tree, nullptr);
  ASSERT_TRUE(interface_has_value_for(*folder_tree, drop_marker, "opacity"));
  ASSERT_TRUE(interface_has_value_for(*ma->paint_layers_tree, drop_marker, "opacity"));

  /* The folder stays (Keep remains); Drop's relay must be gone from both interfaces. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, drop));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  folder_tree = folder_tree_find(*bmain, "Folder");
  ASSERT_NE(folder_tree, nullptr);
  EXPECT_FALSE(interface_has_value_for(*folder_tree, drop_marker, "opacity"));
  EXPECT_FALSE(interface_has_value_for(*ma->paint_layers_tree, drop_marker, "opacity"));
}

TEST_F(PaintLayersGenerateTest, value_mirrors_are_stable_across_regenerations)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(
      BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  root->ensure_interface_cache();
  const int inputs_before = root->interface_inputs().size();
  ASSERT_GT(inputs_before, 0);

  /* An unchanged material must keep the root and its mirror set exactly. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  root->ensure_interface_cache();
  EXPECT_EQ(root->interface_inputs().size(), inputs_before);
}

TEST_F(PaintLayersGenerateTest, reorder_rebuilds_no_group)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  /* Swapping the rows changes only the root's links, never a layer group's topology. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, bottom, 1));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_EQ(layer_tree_find(*bmain, "Bottom"), bottom_tree);
  ASSERT_EQ(layer_tree_find(*bmain, "Top"), top_tree);
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, map_change_keeps_the_root)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));

  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, bottom, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("BottomReplaced")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  /* The changed group's nodes are new; the root was left alone. */
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
}

TEST_F(PaintLayersGenerateTest, correction_add_rebuilds_the_root)
{
  /* Adding an effect adds a value input to the owner's group; the root mirrors every value, so its
   * interface and links change and it rebuilds (A1). */
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  /* +warm: a Paint effect would take the spare's slot (or wait for it without a map) and leave the
   * root alone, so a Fill effect, which does not fit the slot, stands for a new correction. */
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(
      BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The root rebuilds; among the rows only the owner's group was rebuilt. */
  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
  /* The correction's value lives on the owner's group and is mirrored onto the root. */
  bNode *instance = group_instance_find(*root, *bottom_tree);
  ASSERT_NE(instance, nullptr);
  bNodeTreeInterfaceSocket *iface = group_input_find(*bottom_tree, "Bottom C Base Color Opacity");
  ASSERT_NE(iface, nullptr);
  EXPECT_NE(bke::node_find_socket(
                *instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier)),
            nullptr);
  EXPECT_NE(root_instance_input("Bottom C Base Color Opacity"), nullptr);
}

TEST_F(PaintLayersGenerateTest, reorder_rebuilds_the_root)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, bottom, 1));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  /* Only the root's links changed; the groups' contents are untouched. */
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_TRUE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, layer_add_rebuilds_the_root)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));

  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  /* The neighbour's group is untouched. */
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
}

TEST_F(PaintLayersGenerateTest, channel_add_rebuilds_the_root)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  bNodeTree *top_tree = layer_tree_find(*bmain, "Top");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_NE(top_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));
  ASSERT_TRUE(group_mix_sentinel_set(*top_tree, 0.5f));

  /* A new channel changes every layer's hash and the root's interface. */
  ASSERT_NE(add_channel(*bottom, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("BottomNormal")),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_FALSE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_FALSE(group_mix_sentinel_get(*bottom_tree, 0.25f));
  EXPECT_FALSE(group_mix_sentinel_get(*top_tree, 0.5f));
}

TEST_F(PaintLayersGenerateTest, value_edit_rebuilds_nothing)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *bottom_tree = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(bottom_tree, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*bottom_tree, 0.25f));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, bottom, 0.37f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_TRUE(group_mix_sentinel_get(*bottom_tree, 0.25f));
}

/**
 * TZ-26 note: this test used to assert the live constant was built as an #SH_NODE_RGB node with
 * the value baked into its default (`EXPECT_GE(count_type(*group, SH_NODE_RGB), 1)`). The value is
 * now a group input instead (#create_value_inputs, role #ROLE_LIVE_CONSTANT / "live_constant"), so
 * it travels through #BKE_paint_layers_values_sync like opacity and Fill rather than being baked
 * into the row's topology -- this is the change TZ-26 asks for, so the assertion is rewritten to
 * check for the new mechanism (the interface socket and its value) instead of the old one.
 */
TEST_F(PaintLayersGenerateTest, live_material_constant_builds_a_group_input)
{
  Material *source = add_principled_source("LiveSource", 0.42f);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* The source carries Specular, Alpha and Emission beyond the build default set. */
  extend_channel_set(channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));
  /* A baked map for the row, so the non-live path has something to show. */
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("SourceBake")));
  BKE_paint_layers_bake_finalize(*ma, *row);
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* Live: the source's constant is a group input, no RGB node baked into the topology, and the
   * row's baked map is not read. */
  /* +warm: a Warm Mask map per channel of the live row. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 6);
  bNodeTreeInterfaceSocket *live_input = group_input_find(*group, "Source Roughness Source");
  ASSERT_NE(live_input, nullptr);
  ASSERT_NE(live_input->properties, nullptr);
  const IDProperty *role = IDP_GetPropertyTypeFromGroup(
      live_input->properties, "pbr_paint_layers_role", IDP_STRING);
  ASSERT_NE(role, nullptr);
  EXPECT_STREQ(IDP_string_get(role), "live_constant");
  ASSERT_NE(live_input->socket_data, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(live_input->socket_data)->value;
  EXPECT_NEAR(value[0], 0.42f, 1e-4f);

  /* Leaving the row rebuilds its group on the baked map. */
  BKE_paint_layers_active_set(*ma, bottom->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  /* +warm: baked map plus a Warm Mask map per channel. */
  EXPECT_EQ(count_type(*group, SH_NODE_TEX_IMAGE), 7);
}

/**
 * TZ-26: this test used to be named `live_material_constant_is_topology_but_keeps_the_root` and
 * asserted the opposite of what it now checks -- moving the source's constant used to rebuild the
 * row's own group (the stamp was gone) while leaving the root alone. The live constant is a value
 * now (#ROLE_LIVE_CONSTANT group input, synced by #values_sync_socket), not topology, so the fix
 * this TZ asks for is exactly that the row's group must survive the edit too; the name and the body
 * are rewritten together to match.
 */
TEST_F(PaintLayersGenerateTest, live_material_constant_edit_syncs_without_rebuild)
{
  Material *source = add_principled_source("LiveSource", 0.1f);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.125f));

  /* Move the source's constant: it is now a value the row's group reads through a group input, not
   * topology, so it must sync in place. */
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Neither the row's group nor the root was rebuilt: the sentinel and the node lists survive. */
  ASSERT_EQ(layer_tree_find(*bmain, "Source"), group);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));

  /* The new value reached the root instance mirror through the sync at the end of regeneration; the
   * row's group input is fed from it by a link (A1). */
  bNodeSocket *socket = root_instance_input("Source Roughness Source");
  ASSERT_NE(socket, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value;
  EXPECT_NEAR(value[0], 0.9f, 1e-4f);
}

TEST_F(PaintLayersGenerateTest, material_row_participation_is_stable_across_focus)
{
  Material *source = add_principled_source("StableSource", 0.3f);
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.25f));

  auto output_names = [](bNodeTree &tree) {
    tree.ensure_interface_cache();
    Vector<std::string> names;
    for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
      if (socket->name != nullptr) {
        names.append(std::string(socket->name));
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  const Vector<std::string> outputs_active = output_names(*root);

  /* Leave the row: it has no baked maps, so the source constant still carries it. The channel set,
   * the root hash and the row's own topology must all be the same as while it was active. */
  BKE_paint_layers_active_set(*ma, bottom->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));
  EXPECT_EQ(output_names(*root), outputs_active);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.25f));
}

TEST_F(PaintLayersGenerateTest, live_material_image_uses_the_source_texture)
{
  Material *source = add_principled_source("LiveImageSource", 0.3f);
  bNode *principled = principled_of(*source);
  ASSERT_NE(principled, nullptr);
  Image *source_map = add_image("LiveSourceTexture");
  bNode *texture = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  texture->id = &source_map->id;
  bke::node_add_link(*source->nodetree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  NodeTexImage *source_storage = static_cast<NodeTexImage *>(texture->storage);
  ASSERT_NE(source_storage, nullptr);
  source_storage->projection = SHD_PROJ_FLAT;
  source_storage->interpolation = SHD_INTERP_CLOSEST;
  source_storage->extension = SHD_IMAGE_EXTENSION_CLIP;

  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* A baked map for Base Color, so the non-live path would show that instead. */
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("LiveSourceBaked")));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Source");
  ASSERT_NE(group, nullptr);
  bNode *map = find_type(*group, SH_NODE_TEX_IMAGE);
  ASSERT_NE(map, nullptr);
  /* The source's own texture, not the baked map, and its sampling settings travelled with it. */
  EXPECT_EQ(map->id, &source_map->id);
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(map->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->interpolation, SHD_INTERP_CLOSEST);
  EXPECT_EQ(storage->extension, SHD_IMAGE_EXTENSION_CLIP);
}


TEST_F(PaintLayersGenerateTest, source_group_uv_wires_unlinked_textures_only)
{
  Material *source = add_principled_source("UvWrapSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNodeTree &src_tree = *source->nodetree;

  /* A texture with no Vector link must sample the named layer; one already wired to its own UV Map
   * node is left alone. */
  bNode *tex_open = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *tex_linked = bke::node_add_static_node(nullptr, src_tree, SH_NODE_TEX_IMAGE);
  bNode *src_uv = bke::node_add_static_node(nullptr, src_tree, SH_NODE_UVMAP);
  ASSERT_NE(tex_open, nullptr);
  ASSERT_NE(tex_linked, nullptr);
  ASSERT_NE(src_uv, nullptr);
  BLI_strncpy(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map,
              "SourceUV",
              sizeof(static_cast<NodeShaderUVMap *>(src_uv->storage)->uv_map));
  bke::node_add_link(src_tree,
                     *src_uv,
                     *bke::node_find_socket(*src_uv, SOCK_OUT, "UV"_ustr),
                     *tex_linked,
                     *bke::node_find_socket(*tex_linked, SOCK_IN, "Vector"_ustr));
  BKE_ntree_update_tag_all(&src_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, src_tree);

  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
  /* The links the wrapper build added are only visible after its runtime cache is rebuilt. */
  group->ensure_topology_cache();

  bNode *copied_open = node_by_name(*group, tex_open->name);
  bNode *copied_linked = node_by_name(*group, tex_linked->name);
  ASSERT_NE(copied_open, nullptr);
  ASSERT_NE(copied_linked, nullptr);
  bNode *open_source = vector_source(*copied_open);
  bNode *linked_source = vector_source(*copied_linked);
  ASSERT_NE(open_source, nullptr);
  ASSERT_NE(linked_source, nullptr);
  EXPECT_EQ(open_source->type_legacy, SH_NODE_UVMAP);
  EXPECT_STREQ(uv_node_layer(*open_source), "UVMap");
  /* The already-wired texture keeps its own UV Map node untouched. */
  EXPECT_EQ(linked_source->type_legacy, SH_NODE_UVMAP);
  EXPECT_STREQ(uv_node_layer(*linked_source), "SourceUV");

  /* The source material itself is never written to: its open texture stays open. */
  EXPECT_EQ(vector_source(*tex_open), nullptr);
}

TEST_F(PaintLayersGenerateTest, source_group_uv_name_change_rebuilds_the_wrapper)
{
  Material *source = add_principled_source("UvRebuildSource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNode *tex = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);

  BLI_strncpy(ma->paint_layers_uv_map, "First", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  bNode *first_uv = vector_source(*node_by_name(*group, tex->name));
  ASSERT_NE(first_uv, nullptr);
  EXPECT_STREQ(uv_node_layer(*first_uv), "First");

  /* The same name keeps the wrapper and its nodes. */
  bNodeTree *again = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(again, group);
  group->ensure_topology_cache();
  EXPECT_EQ(vector_source(*node_by_name(*group, tex->name)), first_uv);

  /* A new name is topology: the wrapper rebuilds and the UV Map node names the new layer. */
  BLI_strncpy(ma->paint_layers_uv_map, "Second", sizeof(ma->paint_layers_uv_map));
  bNodeTree *rebuilt = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  EXPECT_EQ(rebuilt, group);
  group->ensure_topology_cache();
  bNode *second_uv = vector_source(*node_by_name(*group, tex->name));
  ASSERT_NE(second_uv, nullptr);
  EXPECT_STREQ(uv_node_layer(*second_uv), "Second");
}

/* GUARD: an empty UV name adds no node and leaves textures open. */
TEST_F(PaintLayersGenerateTest, source_group_uv_empty_name_adds_no_node)
{
  Material *source = add_principled_source("UvEmptySource", 0.3f);
  source_set_noise_base_color(*bmain, *source);
  bNode *tex = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_TEX_IMAGE);
  ASSERT_NE(tex, nullptr);

  BLI_strncpy(ma->paint_layers_uv_map, "", sizeof(ma->paint_layers_uv_map));
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  bNodeTree *group = BKE_paint_layers_source_group_ensure(*bmain, *ma, *source, refusal);
  ASSERT_NE(group, nullptr);
  group->ensure_topology_cache();
  EXPECT_EQ(vector_source(*node_by_name(*group, tex->name)), nullptr);
}

/* -------------------------------------------------------------------- */

}  // namespace blender::bke::tests
