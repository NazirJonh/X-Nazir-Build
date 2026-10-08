/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: pass-through folders and content alpha (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


/* -------------------------------------------------------------------- */
/** \name Pass Through folders
 * \{ */

/**
 * A Pass Through folder is pixel-identical to the same rows with the folder taken away, on the CPU
 * and in the generated chain, for the Base Color and Roughness channels. The rows are real-shaped:
 * a Paint base, a partially covered Paint child and a Fill child, plus a partially covered row on
 * top, so both the factors and the coverage are non-trivial.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_equals_the_ungrouped_stack)
{
  const int size = 4;
  const eMaterialPaintChannel channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS};

  auto add_child = [&](MaterialPaintLayer *anchor,
                       const char *name,
                       const eMaterialPaintLayerSource source,
                       Image *image,
                       const eMaterialPaintChannel channel) -> MaterialPaintLayer * {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     source,
                                                     name,
                                                     anchor,
                                                     anchor != nullptr ? PaintLayerPlace::Into :
                                                                         PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    EXPECT_NE(record, nullptr);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    if (source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
      const float fill[4] = {0.1f, 0.6f, 0.2f, 1.0f};
      BKE_paint_layers_set_fill_color(*ma, layer, fill);
      record->image = nullptr;
    }
    else {
      record->image = image;
    }
    return layer;
  };

  auto build = [&](const bool folded, const char *material_name, RGBA r_out[2]) {
    ma = BKE_material_add(bmain, material_name);
    add_child(nullptr,
              "Bottom",
              MA_PAINT_LAYER_SOURCE_IMAGE,
              add_solid_image("Bottom", size, 220, 40, 40, 255),
              channels[0]);
    MaterialPaintLayer *folder = nullptr;
    if (folded) {
      folder = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
      ASSERT_NE(folder, nullptr);
      ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
    }
    MaterialPaintLayer *child = add_child(folder,
                                          "Child",
                                          MA_PAINT_LAYER_SOURCE_IMAGE,
                                          add_solid_image("Child", size, 0, 0, 255, 140),
                                          channels[0]);
    BKE_paint_layers_set_opacity(*ma, child, 0.5f);
    add_child(folder, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT, nullptr, channels[1]);
    add_child(folder,
              "Top",
              MA_PAINT_LAYER_SOURCE_IMAGE,
              add_solid_image("Top", size, 10, 10, 10, 110),
              channels[1]);

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    for (const int i : IndexRange(2)) {
      const RGBA graph = eval_channel_result(interpreter, channels[i]);
      const RGBA cpu = cpu_pixel(channels[i]);
      EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
      EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
      EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
      r_out[i] = cpu;
    }
    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  RGBA folded[2];
  RGBA plain[2];
  build(true, "PassThroughFolded", folded);
  build(false, "PassThroughPlain", plain);
  for (const int i : IndexRange(2)) {
    EXPECT_NEAR(folded[i].r, plain[i].r, 1e-4f);
    EXPECT_NEAR(folded[i].g, plain[i].g, 1e-4f);
    EXPECT_NEAR(folded[i].b, plain[i].b, 1e-4f);
  }
}

/**
 * A Material row read live through the Hybrid path (constant Principled inputs, no bake yet) inside
 * a Pass Through folder still matches the CPU for every channel it takes part in, next to a Paint
 * child with partial coverage.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_with_a_material_child_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS};

  ma = BKE_material_add(bmain, "PassThroughMaterial");
  MaterialPaintLayer *bottom = add_layer("Bottom",
                                         MA_PAINT_LAYER_SOURCE_IMAGE,
                                         add_solid_image("Bottom", size, 200, 30, 30, 255),
                                         channels[0]);
  /* A Roughness map on the bottom row gives the Roughness channel a size; without one the CPU has
   * no buffer to composite the constant-only channel into. */
  MaterialPaintLayerChannel *bottom_rough = BKE_paint_layers_channel_add(*ma, bottom, channels[1]);
  ASSERT_NE(bottom_rough, nullptr);
  bottom_rough->image = add_solid_image("BottomRough", size, 90, 90, 90, 255);
  bottom_rough->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Material *source = BKE_material_add(bmain, "PassThroughSource");
  bNodeTree &source_tree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, source_tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, source_tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(source_tree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *principled_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(principled_color, nullptr);
  const float color[4] = {0.2f, 0.5f, 0.9f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(principled_color->default_value)->value, color);
  bNodeSocket *principled_rough = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(principled_rough, nullptr);
  static_cast<bNodeSocketValueFloat *>(principled_rough->default_value)->value = 0.3f;
  BKE_ntree_update_tag_all(&source_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, source_tree);

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayer *mat_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", folder, PaintLayerPlace::Into);
  ASSERT_NE(mat_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat_row, source));
  for (const eMaterialPaintChannel channel : channels) {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, mat_row, channel), nullptr);
  }

  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", folder, PaintLayerPlace::Into);
  ASSERT_NE(paint, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, paint, channels[0]);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Paint", size, 0, 0, 255, 160);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  BKE_paint_layers_set_opacity(*ma, paint, 0.5f);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *mat_row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  for (const eMaterialPaintChannel channel : channels) {
    const RGBA graph = eval_channel_result(interpreter, channel);
    const RGBA cpu = cpu_pixel(channel);
    const char *ui_name = BKE_paint_material_channel_info(channel).ui_name;
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << ui_name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << ui_name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << ui_name;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Hiding a Pass Through folder is a value edit: its children's factor goes to zero and the result
 * equals the same stack without those children, on both the CPU and the generated chain.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_visibility_matches_the_missing_children)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  /* The reference: only the bottom row. */
  ma = BKE_material_add(bmain, "PassVisibleBottom");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 200, 30, 30, 255), bc);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter plain;
  plain.instance = find_instance();
  plain.tree = ma->paint_layers_tree;
  plain.x = 1;
  plain.y = 1;
  const RGBA bottom_graph = eval_channel_result(plain, bc);
  const RGBA bottom_cpu = cpu_pixel(bc);
  EXPECT_NEAR(bottom_graph.r, bottom_cpu.r, 1e-4f);
  BKE_id_free(bmain, ma);
  ma = nullptr;

  /* The folder hidden must equal that. */
  ma = BKE_material_add(bmain, "PassVisibleFolder");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 200, 30, 30, 255), bc);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, child, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Child", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  BKE_paint_layers_values_sync(*ma);

  GraphInterpreter folded;
  folded.instance = find_instance();
  folded.tree = ma->paint_layers_tree;
  folded.x = 1;
  folded.y = 1;
  const RGBA graph = eval_channel_result(folded, bc);
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  EXPECT_NEAR(graph.r, bottom_graph.r, 1e-4f);
  EXPECT_NEAR(graph.g, bottom_graph.g, 1e-4f);
  EXPECT_NEAR(graph.b, bottom_graph.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Giving a Pass Through folder a mask turns it isolating in one rebuild; both modes stay correct on
 * the CPU and the generated chain, and the mask actually changes the pixels.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_mode_switch_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "PassThroughSwitch");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 200, 30, 30, 255), bc);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, child, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Child", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  auto evaluate = [&](const char *tag) -> RGBA {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel(bc);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << tag;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << tag;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << tag;
    return cpu;
  };

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  const RGBA pass = evaluate("pass through");

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, folder, 0.5f);
  ASSERT_NE(item, nullptr);
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA isolated = evaluate("isolating");

  /* The 0.5 mask must actually dim the blue child over the red bottom. */
  EXPECT_LT(isolated.b, pass.b + 1e-3f);
  EXPECT_GT(isolated.r, pass.r - 1e-3f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Correction opacity on a Material row
 *
 * A content correction mixes its map into the row's colour with factor `alpha(map) * opacity`.
 * The opacity lives on the correction's per (row, channel) settings, written through the BKE
 * setter (9.2); these tests drive that write and check the generated result against the formula,
 * so a path that silently falls back to `alpha` alone cannot pass. The RNA slider reaching the
 * same field is covered by the RNA suite (paint_layers_description_test.cc).
 * \{ */

namespace {

/* -------------------------------------------------------------------- */
/** \name F2-C5: a content correction raises the tracked content alpha
 * \{ */

/** The over pair the CPU folds a content correction in with: `a + fac * (1 - a)`. */
static float content_alpha_over(const float a, const float fac)
{
  return a + fac * (1.0f - a);
}

/** The `.PL Layer <name>` group in \a bmain, or null. */
static bNodeTree *nested_layer_tree_find(Main &bmain, const char *layer_name)
{
  char full[96];
  BLI_snprintf(full, sizeof(full), ".PL Layer %s", layer_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

/** Read \a row_name's tracked `Content Alpha <channel>` output, or -1 when it carries none. */
static float row_content_alpha_eval(const GraphInterpreter &interpreter,
                                    Main &bmain,
                                    bNodeTree &root,
                                    const char *row_name,
                                    const char *channel_name)
{
  bNodeTree *row_tree = nested_layer_tree_find(bmain, row_name);
  if (row_tree == nullptr) {
    return -1.0f;
  }
  bNode *row_instance = nested_group_instance_find(root, *row_tree);
  if (row_instance == nullptr) {
    return -1.0f;
  }
  char socket_name[96];
  BLI_snprintf(socket_name, sizeof(socket_name), "Content Alpha %s", channel_name);
  return nested_folder_named_output_eval(interpreter, *row_tree, *row_instance, socket_name);
}

/**
 * F2-C5: an opaque content correction must raise a part-covered row's alpha to one, the over pair
 * `a = a + fac * (1 - a)` the CPU applies while it blends the correction in. Before the fix the
 * generated chain kept the map's own alpha, so the graph's fourth component lagged the CPU while
 * the colour stayed equal.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_opaque_raises_the_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float a0 = 128.0f / 255.0f;
  ma = BKE_material_add(bmain, "CorrAlphaOpaque");
  MaterialPaintLayer *row = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 200, 200, 200, 128), bc);
  Image *corr_map = add_solid_image("CorrOpaque", size, 0, 0, 255, 255);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 1.0f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 0;
  ASSERT_NE(interpreter.instance, nullptr);

  const float expected_content_a = content_alpha_over(a0, 1.0f);
  EXPECT_NEAR(row_content_alpha_eval(
                  interpreter, *bmain, *ma->paint_layers_tree, "Top", "Base Color"),
              expected_content_a,
              1e-4f);

  const RGBA graph = eval_channel_result(interpreter, bc);
  const RGBA cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  EXPECT_NEAR(graph.a, cpu.a, 1e-4f);
  EXPECT_NEAR(graph.a, 1.0f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5: the same over with a soft correction edge and a partial correction opacity. The tracked
 * content alpha is `a0 + op * A * (1 - a0)` at every column, and graph and CPU agree on all four
 * components.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_soft_edge_raises_the_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float a0 = 128.0f / 255.0f;
  const float op = 0.5f;
  ma = BKE_material_add(bmain, "CorrAlphaSoft");
  MaterialPaintLayer *row = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 200, 200, 200, 128), bc);
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *corr_map = BKE_image_add_generated(
      bmain, size, size, "CorrSoft", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(corr_map, nullptr);
  fill_straight_soft_edge(corr_map, size, 0.3f);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, op));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const float expected_content_a = content_alpha_over(a0, op * soft_edge_alpha_q(x));
    EXPECT_NEAR(row_content_alpha_eval(
                    interpreter, *bmain, *ma->paint_layers_tree, "Top", "Base Color"),
                expected_content_a,
                1e-4f)
        << "x=" << x;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel_at(bc, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, cpu.a, 1e-4f) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5: the corrected row sits inside an isolating folder, so its raised content alpha is
 * straightened by the folder's divide and composed back at the Result. Graph and CPU still agree on
 * all four components, and the folder's own `Content Alpha` output carries the raised value.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_inside_a_folder_raises_the_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float a0 = 128.0f / 255.0f;
  const float op = 0.5f;
  ma = BKE_material_add(bmain, "CorrAlphaFolder");

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  /* Opacity below one keeps the folder isolating: a full-weight single-child folder is
   * auto-detected as Pass Through and gets no group. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Top", folder, PaintLayerPlace::Into);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, row, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Top", size, 200, 200, 200, 128);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *corr_map = BKE_image_add_generated(
      bmain, size, size, "CorrSoftIso", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(corr_map, nullptr);
  fill_straight_soft_edge(corr_map, size, 0.3f);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, op));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "Iso");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const float expected_content_a = content_alpha_over(a0, op * soft_edge_alpha_q(x));
    EXPECT_NEAR(nested_folder_named_output_eval(
                    interpreter, *folder_tree, *folder_instance, "Content Alpha Base Color"),
                expected_content_a,
                1e-4f)
        << "x=" << x;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel_at(bc, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, cpu.a, 1e-4f) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5: the correction map is read as colour data (Roughness), so the chain straightens it with
 * the Vector Math divide. The content alpha still rises by the over, graph and CPU still agree.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_data_map_raises_the_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  const float a0 = 128.0f / 255.0f;
  ma = BKE_material_add(bmain, "CorrAlphaData");
  MaterialPaintLayer *row = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 128, 128, 128, 128), channel);
  Image *corr_map = add_solid_image("CorrData", size, 3, 3, 3, 128);
  make_image_data(corr_map);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  record->image = corr_map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 1.0f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 0;
  ASSERT_NE(interpreter.instance, nullptr);

  const float expected_content_a = content_alpha_over(a0, 128.0f / 255.0f);
  EXPECT_NEAR(row_content_alpha_eval(
                  interpreter, *bmain, *ma->paint_layers_tree, "Top", "Roughness"),
              expected_content_a,
              1e-4f);
  const RGBA graph = eval_channel_result(interpreter, channel);
  const RGBA cpu = cpu_pixel_at(channel, 1, 0);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  EXPECT_NEAR(graph.a, cpu.a, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** F2-C5 guard: a mask item in the mask stack leaves the tracked content alpha untouched. */
TEST_F(PaintLayersGraphEvalTest, mask_correction_leaves_the_content_alpha_alone)
{
  const int size = 4;
  const float a0 = 128.0f / 255.0f;
  ma = BKE_material_add(bmain, "MaskCorrAlpha");
  MaterialPaintLayer *row = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 200, 200, 200, 128));
  set_mask_image(*row, add_solid_image("MaskItem", size, 255, 255, 255, 255));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 0;
  ASSERT_NE(interpreter.instance, nullptr);
  EXPECT_NEAR(row_content_alpha_eval(
                  interpreter, *bmain, *ma->paint_layers_tree, "Top", "Base Color"),
              a0,
              1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** F2-C5 guard: a Material row with a content correction tracks no content alpha, so its group
 * carries no `Content Alpha` output. */
TEST_F(PaintLayersGraphEvalTest, material_content_correction_builds_no_content_alpha)
{
  const int size = 4;
  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  ma = BKE_material_add(bmain, "MatCorrNoAlpha");
  Material *source = make_constant_principled_source(*bmain, "MatNoAlphaSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  Image *corr_map = add_solid_image("CorrMap", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *row_tree = nested_layer_tree_find(*bmain, "Mat");
  ASSERT_NE(row_tree, nullptr);
  EXPECT_FALSE(folder_interface_has_socket(*row_tree, "Content Alpha Base Color"));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name F2-C5d: the explicit content-alpha flag on the CPU stack
 * \{ */

/** Whether any top-level CPU layer of \a channel tracks a content alpha. */
static bool cpu_stack_tracks_content_alpha(Material &ma, const int channel)
{
  Vector<PaintMaterialCompositeImageLayer> layers;
  if (!BKE_paint_layers_composite_image_layers(ma, channel, layers)) {
    return false;
  }
  for (const PaintMaterialCompositeImageLayer &layer : layers) {
    if (layer.tracks_content_alpha) {
      return true;
    }
  }
  return false;
}

/**
 * F2-C5d: a content correction on a constant Fill row lays over the row's own content alpha c_a.
 * The CPU used to fold the correction into its coverage base and overwrite the fourth component
 * with that factor, so a Fill at c_a = 0.4 turned opaque; now both sides agree on
 * `a0 + op * A * (1 - a0)`.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_constant_row_keeps_its_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float c_a = 0.4f;
  const float op = 0.5f;
  ma = BKE_material_add(bmain, "CorrConstAlpha");
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, bc), nullptr);
  const float fill_color[4] = {0.2f, 0.3f, 0.8f, c_a};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, fill_color));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *corr_map = BKE_image_add_generated(bmain,
                                            size,
                                            size,
                                            "CorrConstAlphaMap",
                                            32,
                                            false,
                                            IMA_GENTYPE_BLANK,
                                            black,
                                            false,
                                            false,
                                            false);
  ASSERT_NE(corr_map, nullptr);
  fill_straight_soft_edge(corr_map, size, 0.6f);
  MaterialPaintLayer *correction = add_content_correction(*ma, *fill, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, op));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(cpu_stack_tracks_content_alpha(*ma, bc));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const float expected = content_alpha_over(c_a, op * soft_edge_alpha_q(x));
    EXPECT_NEAR(row_content_alpha_eval(
                    interpreter, *bmain, *ma->paint_layers_tree, "Fill", "Base Color"),
                expected,
                1e-4f)
        << "x=" << x;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel_at(bc, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, cpu.a, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, expected, 1e-4f) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5d: a Paint row with no map paints through a constant whose alpha is its content alpha, so a
 * content correction raises it the same way a Fill's is raised.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_paint_constant_row_keeps_its_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float c_a = 0.4f;
  const float op = 0.5f;
  ma = BKE_material_add(bmain, "CorrPaintConstAlpha");
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "PaintC", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, paint, bc), nullptr);
  const float value[4] = {0.2f, 0.3f, 0.8f, c_a};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(*ma, paint, bc, value));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *corr_map = BKE_image_add_generated(bmain,
                                            size,
                                            size,
                                            "CorrPaintConstMap",
                                            32,
                                            false,
                                            IMA_GENTYPE_BLANK,
                                            black,
                                            false,
                                            false,
                                            false);
  ASSERT_NE(corr_map, nullptr);
  fill_straight_soft_edge(corr_map, size, 0.6f);
  MaterialPaintLayer *correction = add_content_correction(*ma, *paint, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, op));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(cpu_stack_tracks_content_alpha(*ma, bc));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const float expected = content_alpha_over(c_a, op * soft_edge_alpha_q(x));
    EXPECT_NEAR(row_content_alpha_eval(
                    interpreter, *bmain, *ma->paint_layers_tree, "PaintC", "Base Color"),
                expected,
                1e-4f)
        << "x=" << x;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel_at(bc, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, cpu.a, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, expected, 1e-4f) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5d: an isolating folder's own content correction lays over the folder's content alpha, not
 * over its coverage. The CPU used the coverage override as the base, so a folder over a
 * half-transparent Fill turned opaque under a correction.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_folder_row_keeps_its_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float c_a = 0.4f;
  const float op = 0.5f;
  ma = BKE_material_add(bmain, "CorrFolderAlpha");
  /* An opaque bottom map gives the CPU stack its pixel dimensions; the folder covers it fully. */
  add_layer("Bottom",
            MA_PAINT_LAYER_SOURCE_IMAGE,
            add_solid_image("CorrFolderBottom", size, 255, 0, 0, 255),
            bc);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Fold", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, bc), nullptr);
  const float child_color[4] = {0.2f, 0.3f, 0.8f, c_a};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, child, child_color));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *corr_map = BKE_image_add_generated(bmain,
                                            size,
                                            size,
                                            "CorrFolderMap",
                                            32,
                                            false,
                                            IMA_GENTYPE_BLANK,
                                            black,
                                            false,
                                            false,
                                            false);
  ASSERT_NE(corr_map, nullptr);
  fill_straight_soft_edge(corr_map, size, 0.6f);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, folder, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, bc);
  ASSERT_NE(record, nullptr);
  record->image = corr_map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, op));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(cpu_stack_tracks_content_alpha(*ma, bc));

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "Fold");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const float expected = content_alpha_over(c_a, op * soft_edge_alpha_q(x));
    EXPECT_NEAR(nested_folder_named_output_eval(
                    interpreter, *folder_tree, *folder_instance, "Content Alpha Base Color"),
                expected,
                1e-4f)
        << "x=" << x;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel_at(bc, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, cpu.a, 1e-4f) << "x=" << x;
    EXPECT_NEAR(graph.a, expected, 1e-4f) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** F2-C5d guard: a Fill without a correction keeps its own content alpha, unchanged by the flag. */
TEST_F(PaintLayersGraphEvalTest, guarded_fill_without_correction_keeps_its_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float c_a = 0.4f;
  ma = BKE_material_add(bmain, "GuardFillAlpha");
  add_layer(
      "Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("GuardBottom", size, 255, 0, 0, 255), bc);
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, bc), nullptr);
  const float fill_color[4] = {0.2f, 0.3f, 0.8f, c_a};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, fill_color));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 0;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = eval_channel_result(interpreter, bc);
  const RGBA cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_NEAR(graph.a, cpu.a, 1e-4f);
  EXPECT_NEAR(graph.a, c_a, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5d guard: a Material row's transparency is its Alpha input, never the channel map, so the new
 * flag must not be set and its fourth component must stay what the pre-flag CPU produced. That was
 * opaque here (the row's coverage is one, no mask); graph.a != cpu.a on the alpha channel is a
 * known backlog divergence and is not asserted.
 */
TEST_F(PaintLayersGraphEvalTest, guarded_material_row_content_correction_alpha_unchanged)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  ma = BKE_material_add(bmain, "GuardMatAlpha");
  Material *source = make_constant_principled_source(*bmain, "GuardMatSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  Image *corr_map = add_solid_image("GuardMatCorr", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(cpu_stack_tracks_content_alpha(*ma, bc));

  const RGBA cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_NEAR(cpu.a, 1.0f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C5d guard: the Normal channel never tracks a content alpha, so the flag stays off and the CPU
 * fourth component is unchanged. The row's blend is the normal combine, which leaves alpha at the
 * bottom's one.
 */
TEST_F(PaintLayersGraphEvalTest, guarded_normal_row_content_correction_alpha_unchanged)
{
  const int size = 4;
  const eMaterialPaintChannel ch = PAINT_MATERIAL_CHANNEL_NORMAL;
  ma = BKE_material_add(bmain, "GuardNormalAlpha");
  MaterialPaintLayer *row = add_layer(
      "NormalRow", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("GuardNormalMap", size, 128, 128, 255, 128), ch);
  Image *corr_map = add_solid_image("GuardNormalCorr", size, 200, 200, 255, 128);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, ch);
  ASSERT_NE(record, nullptr);
  record->image = corr_map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(cpu_stack_tracks_content_alpha(*ma, ch));

  const RGBA cpu = cpu_pixel_at(ch, 1, 0);
  EXPECT_NEAR(cpu.a, 1.0f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name F2-C6: the bake keeps the row's content alpha in the colour map's alpha
 * \{ */

/** Bake \a row synchronously through the real planner, at \a size, and require it valid. */
static void c6_bake_row_now(Main &bmain, Material &ma, MaterialPaintLayer &row, const int size)
{
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(ma, row, MA_PAINT_LAYER_BAKE_ALWAYS));
  ASSERT_TRUE(BKE_paint_layers_bake_size_set(ma, row, size));
  bool changed = false;
  BKE_paint_layers_bake_plan_run(bmain, ma, &changed);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(ma, row));
}

/** Every component of \a baked near \a live, within the two 8-bit maps' quantization (~2/255). */
static void c6_expect_baked_matches_live(const RGBA &live, const RGBA &baked, const char *tag)
{
  const float tolerance = 0.01f;
  EXPECT_NEAR(baked.r, live.r, tolerance) << tag;
  EXPECT_NEAR(baked.g, live.g, tolerance) << tag;
  EXPECT_NEAR(baked.b, live.b, tolerance) << tag;
  EXPECT_NEAR(baked.a, live.a, tolerance) << tag;
}

/** One top-level Paint row at (1,0), live then baked, always with the bake at map size. */
TEST_F(PaintLayersGraphEvalTest, baked_paint_row_keeps_its_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "C6PaintRow");
  MaterialPaintLayer *row = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("C6PaintTop", size, 200, 200, 200, 128), bc);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);
  const RGBA live_cpu = cpu_pixel_at(bc, 1, 0);
  /* a0 = 128/255; the row covers a0 and reports content alpha a0 over it: 1 - a0 + a0^2. */
  EXPECT_NEAR(live_graph.a, 0.7500f, 0.01f);
  EXPECT_NEAR(live_graph.a, live_cpu.a, 1e-4f);

  c6_bake_row_now(*bmain, *ma, *row, size);
  /* The colour map itself carries the content alpha, quantized to 128. */
  ASSERT_NE(row->bake->images[bc], nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(row->bake->images[bc], nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    const uchar *pixels = ibuf->byte_buffer.data;
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(pixels[1 * 4 + 3], 128);
    BKE_image_release_ibuf(row->bake->images[bc], ibuf, lock);
  }

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  const RGBA baked_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(baked_graph.a, 0.99f);
  EXPECT_NEAR(baked_graph.a, baked_cpu.a, 1e-4f);
  c6_expect_baked_matches_live(live_graph, baked_graph, "paint row");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Paint leaf inside an isolating folder, baked while still live in the folder. */
TEST_F(PaintLayersGraphEvalTest, baked_paint_leaf_in_folder_keeps_its_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "C6PaintFolder");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Fold", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, child, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("C6FolderChild", size, 200, 200, 200, 128);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);
  const RGBA live_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(live_graph.a, 0.99f);

  c6_bake_row_now(*bmain, *ma, *child, size);
  ASSERT_NE(child->bake->images[bc], nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  const RGBA baked_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(baked_graph.a, 0.99f);
  EXPECT_NEAR(baked_graph.a, baked_cpu.a, 1e-4f);
  c6_expect_baked_matches_live(live_graph, baked_graph, "paint leaf in folder");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Fill leaf with c_a = 0.4: it has no map of its own, so a content correction gives the bake a
 * source rectangle and the content alpha (0.4 over the correction) has a place to land.
 */
TEST_F(PaintLayersGraphEvalTest, baked_fill_row_keeps_its_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float c_a = 0.4f;
  ma = BKE_material_add(bmain, "C6FillRow");
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, bc), nullptr);
  const float fill_color[4] = {0.2f, 0.3f, 0.8f, c_a};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, fill_color));
  Image *corr_map = add_solid_image("C6FillCorr", size, 60, 60, 60, 255);
  MaterialPaintLayer *correction = add_content_correction(*ma, *fill, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);
  const RGBA live_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(live_graph.a, 0.99f);

  c6_bake_row_now(*bmain, *ma, *fill, size);
  ASSERT_NE(fill->bake->images[bc], nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  const RGBA baked_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(baked_graph.a, 0.99f);
  EXPECT_NEAR(baked_graph.a, baked_cpu.a, 1e-4f);
  c6_expect_baked_matches_live(live_graph, baked_graph, "fill row");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** An isolating folder baked whole, over a partially transparent child. */
TEST_F(PaintLayersGraphEvalTest, baked_folder_keeps_its_content_alpha)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "C6FolderBake");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Fold", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, child, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("C6FolderBakeChild", size, 200, 200, 200, 128);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);
  const RGBA live_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(live_graph.a, 0.99f);

  c6_bake_row_now(*bmain, *ma, *folder, size);
  ASSERT_NE(folder->bake->images[bc], nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  const RGBA baked_cpu = cpu_pixel_at(bc, 1, 0);
  EXPECT_LT(baked_graph.a, 0.99f);
  EXPECT_NEAR(baked_graph.a, baked_cpu.a, 1e-4f);
  c6_expect_baked_matches_live(live_graph, baked_graph, "folder");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** F2-C6 guard: a Material row tracks no content alpha, so its bake colour map stays opaque. */
TEST_F(PaintLayersGraphEvalTest, baked_material_row_color_alpha_stays_one)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  ma = BKE_material_add(bmain, "C6MaterialRow");
  Material *source = make_constant_principled_source(*bmain, "C6MaterialSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  Image *corr_map = add_solid_image("C6MaterialCorr", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_FALSE(cpu_stack_tracks_content_alpha(*ma, bc));

  Vector<float> color(int64_t(size) * size * 4, -1.0f);
  Vector<float> coverage(int64_t(size) * size, -1.0f);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *row, int(bc), size, color.data(), coverage.data()));
  EXPECT_NEAR(color[0 * 4 + 3], 1.0f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C6 guard: the Normal channel tracks no content alpha in the stack, but the bake colour map's
 * alpha still carries the row's content (variant C, #keep_normal_alpha in
 * paint_material_composite.cc). It does not affect the normal vector. 0.502 map alpha over the 0.502
 * correction: 0.502 + 0.502 * 0.498 = 0.752 (128/255 quantization).
 */
TEST_F(PaintLayersGraphEvalTest, baked_normal_row_color_alpha_carries_content)
{
  const int size = 4;
  const eMaterialPaintChannel ch = PAINT_MATERIAL_CHANNEL_NORMAL;
  ma = BKE_material_add(bmain, "C6NormalRow");
  MaterialPaintLayer *row = add_layer(
      "NormalRow", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("C6NormalMap", size, 128, 128, 255, 128), ch);
  Image *corr_map = add_solid_image("C6NormalCorr", size, 200, 200, 255, 128);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, ch);
  ASSERT_NE(record, nullptr);
  record->image = corr_map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_FALSE(cpu_stack_tracks_content_alpha(*ma, ch));

  Vector<float> color(int64_t(size) * size * 4, -1.0f);
  Vector<float> coverage(int64_t(size) * size, -1.0f);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *row, int(ch), size, color.data(), coverage.data()));
  EXPECT_NEAR(color[0 * 4 + 3], 0.752f, 1e-2f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-D/step 0: "Use Row Result" exports a Paint row's content alpha into the destination map
 * (straight), and leaves a Material row's map opaque.
 */
TEST_F(PaintLayersGraphEvalTest, bake_row_to_image_keeps_paint_alpha_and_is_opaque_for_material)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ma = BKE_material_add(bmain, "C6ExportAlpha");
  MaterialPaintLayer *paint = add_layer(
      "Paint", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("ExportPaint", size, 0, 200, 0, 128), bc);

  Image *dst = BKE_image_add_generated(
      bmain, size, size, "ExportDst", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(dst, nullptr);
  dst->alpha_mode = IMA_ALPHA_STRAIGHT;
  ASSERT_TRUE(BKE_paint_layers_bake_row_to_image(*ma, *paint, int(bc), size, *dst));
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    const uchar *pixels = ibuf->byte_data();
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(pixels[1 * 4 + 3], 128) << "the Paint row's content alpha travels straight";
    BKE_image_release_ibuf(dst, ibuf, lock);
  }

  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "ExportMatSource", row_color, 0.4f);
  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));
  ASSERT_NE(add_content_correction(*ma, *mat, add_solid_image("ExportMatCorr", size, 240, 20, 20, 96)),
            nullptr);
  Image *dst_mat = BKE_image_add_generated(
      bmain, size, size, "ExportDstMat", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(dst_mat, nullptr);
  dst_mat->alpha_mode = IMA_ALPHA_STRAIGHT;
  ASSERT_TRUE(BKE_paint_layers_bake_row_to_image(*ma, *mat, int(bc), size, *dst_mat));
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(dst_mat, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    const uchar *pixels = ibuf->byte_data();
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(pixels[1 * 4 + 3], 255) << "a Material row's map stays opaque";
    BKE_image_release_ibuf(dst_mat, ibuf, lock);
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-D: a heavy isolating folder with no bake yet is auto-baked by the heavy job, and the result
 * matches the live folder on all four components.
 */
TEST_F(PaintLayersGraphEvalTest, auto_baked_folder_matches_live)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "C6AutoFolder");
  MaterialPaintLayer *child = add_layer(
      "Child", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("AutoFolderChild", size, 200, 200, 200, 128), bc);
  MaterialPaintLayer *members[1] = {child};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  /* AUTO bakes only folders nested in another folder (level 2), so wrap it. */
  MaterialPaintLayer *outer_members[1] = {folder};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(outer_members, 1)), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *folder));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_EQ(folder->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);
  EXPECT_LT(live_graph.a, 0.99f);

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  EXPECT_LT(baked_graph.a, 0.99f);
  c6_expect_baked_matches_live(live_graph, baked_graph, "auto folder");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * 4B.4: a Stack (content) correction folder is gated into the AUTO bake cycle exactly like a Layer
 * folder (#BKE_paint_layers_bake_plan_run now walks #BKE_paint_layers_flatten_all), and its bake
 * substitutes into the generated graph the same way (#row_is_substituted is role-agnostic): the
 * graph read before the bake (live) and after the bake job commits (baked, through the generator's
 * substitution) must agree, mirroring #auto_baked_folder_matches_live for a Layer folder.
 */
TEST_F(PaintLayersGraphEvalTest, stack_effect_correction_folder_auto_baked_matches_live)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "C6AutoStackFX");
  MaterialPaintLayer *owner = add_layer(
      "Owner", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("StackFXOwner", size, 255, 0, 0, 255), bc);
  /* AUTO bakes only folders nested in another folder (level 2), so the owner sits in one. */
  MaterialPaintLayer *owner_members[1] = {owner};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(owner_members, 1)), nullptr);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayer *child = add_layer_into(
      correction,
      "Child",
      MA_PAINT_LAYER_SOURCE_IMAGE,
      add_solid_image("AutoStackFXChild", size, 200, 200, 200, 128),
      bc);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *correction));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_EQ(correction->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *correction));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  c6_expect_baked_matches_live(live_graph, baked_graph, "stack effect correction folder");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The Mask Item counterpart: a Stack mask folder is gated into the AUTO bake cycle the same way,
 * and its bake substitutes into the mask evaluation identically to the content case above.
 */
TEST_F(PaintLayersGraphEvalTest, stack_mask_item_folder_auto_baked_matches_live)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "C6AutoStackMask");
  MaterialPaintLayer *owner = add_layer(
      "Owner", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("StackMaskOwner", size, 255, 0, 0, 255), bc);
  /* AUTO bakes only folders nested in another folder (level 2), so the owner sits in one. */
  MaterialPaintLayer *owner_members[1] = {owner};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(owner_members, 1)), nullptr);
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = bc;
  MaterialPaintLayer *child = add_layer_into(
      mask,
      "Child",
      MA_PAINT_LAYER_SOURCE_IMAGE,
      add_solid_image("AutoStackMaskChild", size, 200, 200, 200, 128),
      bc);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, mask, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *mask));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_EQ(mask->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter live;
  live.instance = find_instance();
  live.tree = ma->paint_layers_tree;
  live.x = 1;
  live.y = 0;
  ASSERT_NE(live.instance, nullptr);
  const RGBA live_graph = eval_channel_result(live, bc);

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *mask));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter baked;
  baked.instance = find_instance();
  baked.tree = ma->paint_layers_tree;
  baked.x = 1;
  baked.y = 0;
  ASSERT_NE(baked.instance, nullptr);
  const RGBA baked_graph = eval_channel_result(baked, bc);
  c6_expect_baked_matches_live(live_graph, baked_graph, "stack mask item folder");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** \} */

/**
 * The expected colour: the correction mixed into the row's own colour `S` with factor
 * `alpha(map) * opacity`, then the row laid over the bottom. The row covers fully here (its source
 * alpha is one), so the bottom is not reached and the result is the corrected colour.
 */
}  // namespace

}  // namespace blender::bke::tests
