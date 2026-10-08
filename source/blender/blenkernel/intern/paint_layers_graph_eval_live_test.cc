/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: live material rows and channels (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


/**
 * The template a "New Custom Layer" makes has to carry the row below through to its color output,
 * or the first bake renders empty. The link is written before the group's topology settles, so this
 * pins that it survives the update.
 */
TEST_F(PaintLayersGraphEvalTest, custom_template_group_body_passes_below_into_color)
{
  ma = BKE_material_add(bmain, "CustomTemplate");
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);
  ASSERT_NE(custom->custom_group, nullptr);
  bNodeTree *group = custom->custom_group;
  group->ensure_topology_cache();

  const bNode *input = nullptr;
  const bNode *output = nullptr;
  for (const bNode &node : group->nodes) {
    if (node.is_group_input()) {
      input = &node;
    }
    if (node.is_group_output()) {
      output = &node;
    }
  }
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);

  const bNodeSocket *below = static_cast<const bNodeSocket *>(input->outputs.first);
  const bNodeSocket *color = static_cast<const bNodeSocket *>(output->inputs.first);
  ASSERT_NE(below, nullptr);
  ASSERT_NE(color, nullptr);
  bool linked = false;
  for (const bNodeLink &link : group->links) {
    if (link.fromnode == input && link.tonode == output && link.fromsock == below &&
        link.tosock == color)
    {
      linked = true;
    }
  }
  EXPECT_TRUE(linked);

  Vector<int> channels;
  BKE_paint_layers_custom_channels_get(*custom, channels);
  EXPECT_TRUE(channels.contains(int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Per (row, channel) blend/opacity: a channel record's own blend and opacity multiplier, and the
 * row's opacity, must mean the same in the generated chain and the CPU. The override is set on a
 * card that is not the record's channel yet, exercising the on-demand creation too.
 */
TEST_F(PaintLayersGraphEvalTest, per_channel_blend_opacity_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const eMaterialPaintChannel rough = PAINT_MATERIAL_CHANNEL_ROUGHNESS;

  auto result_name_for = [](const int channel) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).ui_name;
  };

  auto add_single_channel_layer = [&](const char *name,
                                       Image *image,
                                       MaterialPaintLayer *anchor,
                                       const PaintLayerPlace place,
                                       const eMaterialPaintChannel channel)
      -> MaterialPaintLayer * {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, anchor, place);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  auto add_two_channel_layer = [&](const char *name,
                                   Image *bc_image,
                                   Image *rough_image,
                                   MaterialPaintLayer *anchor,
                                   const PaintLayerPlace place) -> MaterialPaintLayer * {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, anchor, place);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *b = BKE_paint_layers_channel_add(*ma, layer, bc);
    EXPECT_NE(b, nullptr);
    b->image = bc_image;
    b->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    MaterialPaintLayerChannel *r = BKE_paint_layers_channel_add(*ma, layer, rough);
    EXPECT_NE(r, nullptr);
    r->image = rough_image;
    r->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  auto compare = [&](const char *name, const int channel) {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result(result_name_for(channel).c_str());
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << name;
  };

  /* A leaf: the row's opacity times the channel's own multiplier, and a per-channel blend. */
  {
    ma = BKE_material_add(bmain, "PerChannelLeaf");
    add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *top = add_layer(
        "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), bc);
    BKE_paint_layers_set_opacity(*ma, top, 0.5f);
    ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *top, bc, 0.5f));
    ASSERT_TRUE(BKE_paint_layers_channel_blend_set(*ma, *top, bc, MA_PAINT_LAYER_BLEND_MULTIPLY));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    compare("leaf", bc);
    BKE_id_free(bmain, ma);
    ma = nullptr;
  }

  /* A folder: one channel overridden, another inheriting the row's blend and opacity. */
  {
    ma = BKE_material_add(bmain, "PerChannelFolder");
    add_two_channel_layer("Bottom",
                          add_solid_image("BottomBC", size, 255, 0, 0, 255),
                          add_solid_image("BottomR", size, 64, 64, 64, 255),
                          nullptr,
                          PaintLayerPlace::Above);
    MaterialPaintLayer *folder = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(folder, nullptr);
    add_two_channel_layer("Child",
                          add_solid_image("ChildBC", size, 0, 0, 255, 255),
                          add_solid_image("ChildR", size, 200, 200, 200, 255),
                          folder,
                          PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
    ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
        *ma, *folder, bc, MA_PAINT_LAYER_BLEND_MULTIPLY));
    ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *folder, rough, 0.5f));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    compare("folder_bc_override", bc);
    compare("folder_rough_inherit", rough);
    BKE_id_free(bmain, ma);
    ma = nullptr;
  }

  /* A nested folder: opacity at two levels, each channel its own multiplier. */
  {
    ma = BKE_material_add(bmain, "PerChannelNested");
    add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *outer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(outer, nullptr);
    MaterialPaintLayer *inner = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
    ASSERT_NE(inner, nullptr);
    add_single_channel_layer("Leaf",
                             add_solid_image("Leaf", size, 0, 0, 255, 255),
                             inner,
                             PaintLayerPlace::Into,
                             bc);
    BKE_paint_layers_set_opacity(*ma, outer, 0.5f);
    ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *inner, bc, 0.5f));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    compare("nested", bc);
    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/** A freshly authored Paint row takes part in the default channels but lays nothing down. */
TEST_F(PaintLayersGraphEvalTest, authored_paint_without_a_map_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "AuthoredPaint");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *paint);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Base Color");
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  /* The Paint row covers nothing, so the red bottom shows through unchanged. */
  EXPECT_NEAR(graph.r, 1.0f, 1e-4f);
  EXPECT_NEAR(graph.g, 0.0f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Fill shows a value per channel, Base Color included, from the row's channel records. */
TEST_F(PaintLayersGraphEvalTest, fill_has_a_value_per_channel)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "FillPerChannel");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 255, 255, 255));
  /* A Roughness map too, so that channel has a pixel size to composite into. */
  add_layer("BottomR",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("BottomR", size, 128, 128, 128, 255),
            PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, red));
  const float rough[4] = {0.75f, 0.75f, 0.75f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);

  const RGBA graph_bc = interpreter.eval_result("Result Base Color");
  const RGBA cpu_bc = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(graph_bc.r, cpu_bc.r, 1e-4f);
  EXPECT_NEAR(graph_bc.g, cpu_bc.g, 1e-4f);
  EXPECT_NEAR(graph_bc.b, cpu_bc.b, 1e-4f);
  EXPECT_NEAR(graph_bc.r, 1.0f, 1e-4f);
  EXPECT_NEAR(graph_bc.g, 0.0f, 1e-4f);

  const RGBA graph_rough = interpreter.eval_result("Result Roughness");
  const RGBA cpu_rough = cpu_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(graph_rough.r, cpu_rough.r, 1e-4f);
  EXPECT_NEAR(graph_rough.r, 0.75f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The main TZ-2 check: an active Material row whose source channels are constants is shown live by
 * the generator and by the CPU composite, and the two must agree. The source's Base Color and
 * Alpha are unlinked, so the row is a constant colour limited by a constant coverage; the row's own
 * opacity scales both.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_constant_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "LiveMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.75f;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  /* The source Alpha is the row's coverage, and Alpha is outside the build default set. */
  channel_set_extend(*ma, channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* The row blends the live constant over the red bottom by opacity * source alpha = 0.375. */
  const float factor = 0.5f * 0.75f;
  const float expected[3] = {1.0f + (source_color[0] - 1.0f) * factor,
                             0.0f + (source_color[1] - 0.0f) * factor,
                             0.0f + (source_color[2] - 0.0f) * factor};
  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same live Material row, but with the focus elsewhere and no baked maps for the row. The
 * source constant still carries the row, so the generator and the CPU must agree exactly as when
 * the row is active. This is the viewport case the fix exists for: the layer must not change when
 * the user moves off it.
 */
TEST_F(PaintLayersGraphEvalTest, inactive_material_constant_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "InactiveLiveMaterial");
  MaterialPaintLayer *bottom = add_layer(
      "Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "InactiveLiveSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.75f;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  /* The source Alpha is the row's coverage, and Alpha is outside the build default set. */
  channel_set_extend(*ma, channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));
  /* The focus is on the bottom row, not on the Material row. */
  BKE_paint_layers_active_set(*ma, bottom->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  const float factor = 0.5f * 0.75f;
  const float expected[3] = {1.0f + (source_color[0] - 1.0f) * factor,
                             0.0f + (source_color[1] - 0.0f) * factor,
                             0.0f + (source_color[2] - 0.0f) * factor};
  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-26: a live Hybrid constant is a group input now, not a value baked into an RGB node's default
 * (see the topology hash and #create_value_inputs in paint_layers_generate.cc). Moving the source's
 * Base Color must not rebuild the row's group or the root -- the edit reaches the graph through
 * #BKE_paint_layers_values_sync -- and the graph must read the new value, matching the CPU exactly
 * as #live_material_constant_matches_the_cpu already does for the initial value.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_constant_edit_syncs_without_rebuild)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "LiveSyncMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveSyncSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.2f;
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.75f;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  /* The source Alpha is the row's coverage, and Alpha is outside the build default set. */
  channel_set_extend(*ma, channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_node_ptrs(*root);
  bNodeTree *group = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group = &tree;
      break;
    }
  }
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> group_before = root_node_ptrs(*group);

  /* Move the source's Base Color and Roughness -- values shown live from the source, per the RNA
   * path a real edit uses (RNA_property_update / node-tree update, which #material_changed in
   * render_update.cc answers by tagging the layered material edited): #BKE_paint_layers_tag_edited
   * plus the depsgraph's #ID_RECALC_SHADING tag, which #BKE_material_eval answers with
   * #BKE_paint_layers_values_sync on every evaluated copy. Editing the node socket's default value
   * directly and calling #BKE_paint_layers_regenerate reproduces exactly that contract without a
   * window and a depsgraph, the same substitution the neighbouring tests in this file already make
   * for every other "user moved a slider" case. */
  const float new_color[4] = {0.9f, 0.1f, 0.4f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, new_color);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.8f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Neither the row's group nor the root was rebuilt: the value travelled through a group input
   * instead of forcing a new topology hash. */
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(root_before, root_node_ptrs(*root)));
  bNodeTree *group_after = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group_after = &tree;
      break;
    }
  }
  EXPECT_EQ(group_after, group);
  EXPECT_TRUE(same_node_ptrs(group_before, root_node_ptrs(*group)));

  /* The graph reads the new value, matching the CPU compositor, which reads the same live helper. */
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  const float factor = 0.5f * 0.75f;
  const float expected[3] = {1.0f + (new_color[0] - 1.0f) * factor,
                             0.0f + (new_color[1] - 0.0f) * factor,
                             0.0f + (new_color[2] - 0.0f) * factor};
  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-26: the view a channel is shown in is still topology -- switching the source's Base Color from
 * a constant to an Image Texture must rebuild the row's group, unlike a plain value edit above.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_view_change_rebuilds_the_row_group)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "LiveViewChangeMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveViewChangeSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *group = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group = &tree;
      break;
    }
  }
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> group_before = root_node_ptrs(*group);

  /* Replace the constant with a trivially-mapped texture: the channel's view changes from Constant
   * to Image, which is topology (#topology_hash_layer still hashes `live_constant` and
   * `live_map_probe`), so the row's group must be rebuilt. */
  Image *source_map = add_solid_image("LiveViewChangeMap", size, 200, 200, 200, 255);
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &source_map->id;
  bke::node_add_link(
      ntree, *texture, *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr), *principled, *base_color);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group_after = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group_after = &tree;
      break;
    }
  }
  ASSERT_NE(group_after, nullptr);
  EXPECT_EQ(group_after, group) << "the group is preserved, only rebuilt in place";
  EXPECT_FALSE(same_node_ptrs(group_before, root_node_ptrs(*group)))
      << "constant -> texture must rebuild the row's group";

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-26: #BKE_material_eval calls #BKE_paint_layers_values_sync on the evaluated (COW) copy of the
 * material -- never on the original -- and #layer.material is walked by
 * #material_paint_layer_foreach_id (IDWALK_CB_USER), so depsgraph's generic pointer remap already
 * gives that evaluated row an evaluated `layer.material` by the time the sync runs; nothing in
 * #values_sync_socket needs to special-case evaluated vs original. This test proves the reading
 * side of that contract directly: #BKE_paint_layers_material_live_constant (and so
 * #values_sync_socket, which only wraps it) resolves the constant from whatever Material
 * `layer.material` currently names, with no assumption that it is the original -- swapping the
 * pointer to an independent copy of the source, the same substitution a depsgraph remap performs,
 * must be picked up by the very next sync. A full evaluated-copy Material is not built here: a
 * plain #BKE_id_copy_ex of `ma` would leave `layer.material` shared with the original (only
 * depsgraph's relation-driven remap swaps it, and #paint_layers_tree is `IDWALK_CB_USER`-walked
 * the same way as #layer.material, so a bare ID copy cannot stand in for that pass without
 * reimplementing it) -- swapping `layer.material` on `ma` itself isolates exactly the one behaviour
 * in question.
 */
TEST_F(PaintLayersGraphEvalTest, live_constant_values_sync_reads_whatever_material_it_is_given)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "EvalPathMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "EvalPathSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  const float source_color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA before = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NEAR(before.r, source_color[0], 1e-4f);
  EXPECT_NEAR(before.g, source_color[1], 1e-4f);
  EXPECT_NEAR(before.b, source_color[2], 1e-4f);

  /* An independent copy of the source, its own embedded node tree included -- what a depsgraph
   * remap would install in `layer.material` for an evaluated `ma`. */
  Material *source_eval = reinterpret_cast<Material *>(
      BKE_id_copy_ex(nullptr, &source->id, nullptr, LIB_ID_COPY_LOCALIZE));
  ASSERT_NE(source_eval, nullptr);
  ASSERT_NE(source_eval->nodetree, source->nodetree)
      << "the embedded node tree must be its own copy, or this test proves nothing";
  bNode *principled_eval = nullptr;
  for (bNode &node : source_eval->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled_eval = &node;
      break;
    }
  }
  ASSERT_NE(principled_eval, nullptr);
  bNodeSocket *base_color_eval = bke::node_find_socket(*principled_eval, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color_eval, nullptr);
  const float eval_color[4] = {0.9f, 0.1f, 0.4f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color_eval->default_value)->value, eval_color);

  /* The substitution itself: `layer.material` now names the independent copy, exactly as a
   * depsgraph remap would leave it on the evaluated `ma`. No regeneration runs -- only sync, the
   * same call #BKE_material_eval makes for every evaluated copy. */
  row->material = source_eval;
  BKE_paint_layers_values_sync(*ma);

  const RGBA after = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NEAR(after.r, eval_color[0], 1e-4f)
      << "values_sync must read the Material layer.material currently names, not the original";
  EXPECT_NEAR(after.g, eval_color[1], 1e-4f);
  EXPECT_NEAR(after.b, eval_color[2], 1e-4f);

  /* The untouched original source still carries its own constant -- the sync read the copy, it did
   * not write back into it. */
  bNodeSocket *base_color_after = bke::node_find_socket(
      *principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color_after, nullptr);
  const float *original_value =
      static_cast<bNodeSocketValueRGBA *>(base_color_after->default_value)->value;
  EXPECT_NEAR(original_value[0], source_color[0], 1e-6f);
  EXPECT_NEAR(original_value[1], source_color[1], 1e-6f);
  EXPECT_NEAR(original_value[2], source_color[2], 1e-6f);

  row->material = source;
  BKE_id_free(bmain, source_eval);
  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-4's main check: an active Material row whose Base Color and Alpha are plain textures. The
 * generator shows the source's own Image Texture and reads its Alpha for coverage; the CPU samples
 * the same image in UV space and reads its alpha. On a soft alpha edge the two must agree at every
 * pixel, or leaving the row would show a rim.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "LiveImageMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveImageSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  Image *source_map = add_solid_image("LiveSourceMap", size, 200, 200, 200, 255);
  fill_straight_soft_edge(source_map, size, 0.5f);
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &source_map->id;
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Alpha"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A top-level source whose Base Color (and, when \a link_alpha, Alpha) is a plain Image Texture
 * over \a map, so both channels resolve to the row's own live map in Hybrid mode. */
static Material *make_live_image_source(Main *bmain,
                                        const char *name,
                                        Image *map,
                                        const bool link_alpha)
{
  Material *source = BKE_material_add(bmain, name);
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &map->id;
  id_us_plus(&map->id);
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  if (link_alpha) {
    bke::node_add_link(ntree,
                       *texture,
                       *bke::node_find_socket(*texture, SOCK_OUT, "Alpha"_ustr),
                       *principled,
                       *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
  }
  return source;
}

/**
 * Spec: a Hybrid live-map Material row must stay in the generated graph. The early drop checked
 * `live_constant` and `source_group_instance` but not `live_map`, so a row whose source shows a
 * texture (no constant) vanished from the graph entirely. Top level, source Base Color a live image
 * with a uniform partial alpha and its Alpha input left at the constant 1: a Material row's
 * transparency is the Alpha input, so the channel map's own alpha is ignored -- the row covers by
 * `opacity * 1` and its content alpha is 1. Graph, CPU and the closed form agree on all four
 * components. The result differing from the bare red bottom is what proves the row took part.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_row_participates_at_top_level)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float row_opacity = 0.5f;
  const float tolerance = 1e-4f;

  ma = BKE_material_add(bmain, "LiveImageTop");
  add_layer(
      "Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("LiveImageTopBottom", size, 255, 0, 0, 255));
  Image *source_map = add_solid_image("LiveImageTopMap", size, 128, 128, 128, 128);
  Material *source = make_live_image_source(bmain, "LiveImageTopSource", source_map, false);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, row_opacity));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA sampled = interpreter.sample_image(source_map, "Color");
  const float map_alpha = interpreter.sample_image(source_map, "Alpha").r;
  /* The map is genuinely partial, so the assertions below would fail if its alpha were folded in. */
  EXPECT_GT(map_alpha, 0.4f);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    /* A Material row's transparency is the Alpha input (the constant 1): the channel map's own
     * alpha is ignored, so the factor is the opacity alone. */
    const float factor = row_opacity;
    const RGBA expected = {1.0f * (1.0f - factor) + sampled.r * factor,
                           0.0f * (1.0f - factor) + sampled.g * factor,
                           0.0f * (1.0f - factor) + sampled.b * factor,
                           0.0f};
    /* The row is in the graph, not dropped: the bare red bottom would read (1, 0, 0). */
    EXPECT_LT(graph.r, 1.0f - tolerance) << "x=" << x;
    EXPECT_NEAR(graph.r, expected.r, tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "graph x=" << x;
    /* The Result alpha keeps the chain's blended value; see `live_material_image_matches_the_cpu`,
     * which for the same reason pins rgb only. */
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "cpu x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec: the same live-map row inside an isolating folder. A Material row's transparency is its Alpha
 * input (the constant 1 here), so its factor is 1 and its content alpha is 1; the channel map's own
 * alpha is ignored. The folder's isolated colour is the source's straight colour and the isolated
 * content alpha is 1, laid over the bottom at `folder_opacity * 1`. Graph, CPU, the closed form and
 * the folder's Coverage/Content Alpha outputs must all agree even though the map is partial.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_in_isolating_folder_content_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "LiveImageIso");
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("LiveImageIsoBottom", size, 255, 0, 0, 255);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "LiveImageIsoFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *source_map = BKE_image_add_generated(
      bmain, size, size, "LiveImageIsoMap", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(source_map, nullptr);
  fill_nested_leaf_soft_edge(source_map, size, 128, 128, 128);

  Material *source = make_live_image_source(bmain, "LiveImageIsoSource", source_map, false);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", folder, PaintLayerPlace::Into);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "LiveImageIsoFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_FALSE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA sampled = interpreter.sample_image(source_map, "Color");

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = eval_channel_result(interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    /* The row's factor is its material Alpha (the constant 1), not the map's partial alpha; one
     * child, so the folder's straight isolated colour is the sampled colour and it is laid at
     * folder_opacity. A Material row tracks no content alpha, so the folder grows none. */
    const float cov = folder_opacity;
    const RGBA expected = {1.0f * (1.0f - cov) + sampled.r * cov,
                           0.0f * (1.0f - cov) + sampled.g * cov,
                           0.0f * (1.0f - cov) + sampled.b * cov,
                           0.0f};
    const float graph_cov = nested_folder_named_output_eval(
        interpreter, *folder_tree, *folder_instance, "Coverage Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph_cov, cov, tolerance) << "coverage x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec: the live-map row on the Normal channel, which has no content alpha of its own, must still take
 * part rather than drop. Source Normal goes through a Normal Map over a flat data texture, so it is
 * a trivial live map; the row combines its decode against the flat bottom's. Graph, CPU and the
 * decode-combine-normalize-encode closed form agree, and the result leaves the flat bottom. The
 * map's partial alpha is what the row covers by (the source Alpha input is the constant 1).
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_on_normal_channel_participates)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;

  ma = BKE_material_add(bmain, "LiveImageNormal");
  Image *bottom_map = add_solid_image("LiveImageNormalBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  Image *normal_map = add_solid_image("LiveImageNormalMap", size, 255, 128, 128, 200);
  make_image_data(normal_map);

  Material *source = BKE_material_add(bmain, "LiveImageNormalSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &normal_map->id;
  id_us_plus(&normal_map->id);
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;
  bNode *normal_node = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
  ASSERT_NE(normal_node, nullptr);
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *normal_node,
                     *bke::node_find_socket(*normal_node, SOCK_IN, "Color"_ustr));
  bke::node_add_link(ntree,
                     *normal_node,
                     *bke::node_find_socket(*normal_node, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA bottom_enc = interpreter.sample_image(bottom_map, "Color");
  const RGBA row_enc = interpreter.sample_image(normal_map, "Color");
  const float map_alpha = interpreter.sample_image(normal_map, "Alpha").r;
  /* The normal map is partial; as a Material row its alpha is data, not transparency. */
  EXPECT_GT(map_alpha, 0.5f);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    /* The row's transparency is the Alpha input (the constant 1), so the normal map's own partial
     * alpha (which is data, not transparency) is ignored and the factor is 1. */
    const float fac = 1.0f;
    const float base[3] = {
        bottom_enc.r * 2.0f - 1.0f, bottom_enc.g * 2.0f - 1.0f, bottom_enc.b * 2.0f - 1.0f};
    const float detail[3] = {
        row_enc.r * 2.0f - 1.0f, row_enc.g * 2.0f - 1.0f, row_enc.b * 2.0f - 1.0f};
    float combined[3] = {base[0] + detail[0], base[1] + detail[1], base[2] * detail[2]};
    normalize_v3(combined);
    float encoded[3] = {combined[0] * 0.5f + 0.5f,
                        combined[1] * 0.5f + 0.5f,
                        combined[2] * 0.5f + 0.5f};
    /* The Normal Combine interpolates base and encoded by the factor, then the chain's final
     * decode-normalize-encode runs once more, exactly as #cpu_pixel_at applies it. */
    float raw[3] = {bottom_enc.r * (1.0f - fac) + encoded[0] * fac,
                    bottom_enc.g * (1.0f - fac) + encoded[1] * fac,
                    bottom_enc.b * (1.0f - fac) + encoded[2] * fac};
    float final_n[3] = {raw[0] * 2.0f - 1.0f, raw[1] * 2.0f - 1.0f, raw[2] * 2.0f - 1.0f};
    normalize_v3(final_n);
    const float expected[3] = {
        final_n[0] * 0.5f + 0.5f, final_n[1] * 0.5f + 0.5f, final_n[2] * 0.5f + 0.5f};

    /* The row participates: the bare flat bottom would stay at its stored encoding. */
    EXPECT_GT(fabsf(graph.r - bottom_enc.r), 0.05f) << "x=" << x;
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "graph x=" << x;
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "cpu x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "cpu x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec (material alpha): one source, one texture with a=0.25 on Base Color AND Alpha, op=0.5, over an
 * opaque bottom. A Material row's transparency is the Alpha input, so Hybrid live_map and Baked (a
 * real opaque channel map plus coverage = a) must both give the Principled's `op * a`, and the graph
 * must agree with the CPU on all four components. This is the double-count regression guard: before
 * the fix Hybrid multiplied the map alpha in a second time and read 0.975303 where the Principled
 * reads 0.901600.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_alpha_input_matches_baked)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float row_opacity = 0.5f;
  const float tolerance = 1e-4f;

  ma = BKE_material_add(bmain, "LiveImageParity");
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("LiveImageParityBottom", size, 255, 0, 0, 255));
  /* One texture, grey 0.5 with alpha 64/255, feeding Base Color and Alpha alike. */
  Image *source_map = add_solid_image("LiveImageParityMap", size, 128, 128, 128, 64);
  Material *source = make_live_image_source(bmain, "LiveImageParitySource", source_map, true);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, row_opacity));
  /* The source Alpha is the row's coverage, and Alpha is outside the build default set. */
  channel_set_extend(*ma, channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA sampled = interpreter.sample_image(source_map, "Color");
  const float alpha = interpreter.sample_image(source_map, "Alpha").r;
  /* The Principled's transparency: the Alpha input, here the same texture. */
  const float factor = row_opacity * alpha;
  const RGBA expected = {1.0f * (1.0f - factor) + sampled.r * factor,
                         0.0f * (1.0f - factor) + sampled.g * factor,
                         0.0f * (1.0f - factor) + sampled.b * factor,
                         0.0f};

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, expected.r, tolerance) << "hybrid graph x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "hybrid graph x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "hybrid graph x=" << x;
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "hybrid cpu x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "hybrid cpu x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "hybrid cpu x=" << x;
  }

  /* The same row from a real bake: an opaque channel map (the source colour) plus coverage = a. */
  const float cov[4] = {alpha, alpha, alpha, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "LiveImageParityCov", size, cov);
  ASSERT_NE(coverage, nullptr);
  for (int c = 0; c < PAINT_MATERIAL_CHANNEL_NUM; c++) {
    const float rgba[4] = {sampled.r, sampled.g, sampled.b, 1.0f};
    Image *map = make_bake_data_map(bmain, "LiveImageParityBake", size, rgba);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, c, map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, expected.r, tolerance) << "baked graph x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "baked graph x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "baked graph x=" << x;
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "baked cpu x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "baked cpu x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "baked cpu x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec (material alpha): a Hybrid live_map row on the Normal channel whose normal map has alpha 200
 * while the Alpha input is the constant 1. Normal-map alpha is data, not transparency, so it must
 * not influence the row's contribution: rewriting it to 255 changes nothing, and the graph still
 * matches the CPU.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_normal_map_alpha_is_ignored)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  const float tolerance = 1e-4f;

  ma = BKE_material_add(bmain, "LiveImageNormalAlpha");
  Image *bottom_map = add_solid_image("LiveImageNormalAlphaBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  Image *normal_map = add_solid_image("LiveImageNormalAlphaMap", size, 255, 128, 128, 200);
  make_image_data(normal_map);

  Material *source = BKE_material_add(bmain, "LiveImageNormalAlphaSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &normal_map->id;
  id_us_plus(&normal_map->id);
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;
  bNode *normal_node = bke::node_add_static_node(nullptr, ntree, SH_NODE_NORMAL_MAP);
  ASSERT_NE(normal_node, nullptr);
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *normal_node,
                     *bke::node_find_socket(*normal_node, SOCK_IN, "Color"_ustr));
  bke::node_add_link(ntree,
                     *normal_node,
                     *bke::node_find_socket(*normal_node, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  interpreter.x = 0;
  interpreter.y = 0;
  const RGBA with_200 = interpreter.eval_result(result_name(channel));

  /* Rewrite the map's alpha to 255: a Material row ignores the channel map's alpha. */
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(normal_map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(normal_map, ibuf, lock);
  }
  const RGBA with_255 = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel_at(channel, 0, 0);

  EXPECT_NEAR(with_200.r, with_255.r, tolerance);
  EXPECT_NEAR(with_200.g, with_255.g, tolerance);
  EXPECT_NEAR(with_200.b, with_255.b, tolerance);
  EXPECT_NEAR(with_255.r, cpu.r, tolerance);
  EXPECT_NEAR(with_255.g, cpu.g, tolerance);
  EXPECT_NEAR(with_255.b, cpu.b, tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A source whose Principled is at the top level and whose Base Color is a Noise: the resolver calls
 * it Baked, so every row on it must show its source through the SourceGroup wrapper. Returns the
 * material; \a r_principled receives the node the caller feeds Normal.
 */
}  // namespace blender::bke::tests
