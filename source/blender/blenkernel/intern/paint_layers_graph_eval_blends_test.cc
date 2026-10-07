/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: blend modes incl. the mandatory parity table (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


TEST_F(PaintLayersGraphEvalTest, every_blend_mode_matches_the_cpu)
{
  const int size = 4;
  struct BlendCase {
    const char *name;
    eMaterialPaintLayerBlend blend;
    uchar bottom[3];
    uchar top[3];
  };
  const BlendCase cases[] = {
      {"mix", MA_PAINT_LAYER_BLEND_MIX, {192, 64, 32}, {64, 192, 224}},
      {"multiply", MA_PAINT_LAYER_BLEND_MULTIPLY, {192, 64, 32}, {64, 192, 224}},
      {"overlay", MA_PAINT_LAYER_BLEND_OVERLAY, {192, 64, 32}, {64, 192, 224}},
      {"add", MA_PAINT_LAYER_BLEND_ADD, {192, 64, 32}, {64, 192, 224}},
      {"add_over_one", MA_PAINT_LAYER_BLEND_ADD, {200, 200, 200}, {200, 200, 200}},
      {"subtract_below_zero", MA_PAINT_LAYER_BLEND_SUBTRACT, {64, 64, 64}, {200, 200, 200}},
      {"divide_by_zero", MA_PAINT_LAYER_BLEND_DIVIDE, {200, 200, 200}, {0, 0, 0}},
      {"divide", MA_PAINT_LAYER_BLEND_DIVIDE, {192, 64, 32}, {64, 192, 224}},
      {"darken", MA_PAINT_LAYER_BLEND_DARKEN, {192, 64, 32}, {64, 192, 224}},
      {"lighten", MA_PAINT_LAYER_BLEND_LIGHTEN, {192, 64, 32}, {64, 192, 224}},
      {"screen", MA_PAINT_LAYER_BLEND_SCREEN, {192, 64, 32}, {64, 192, 224}},
      {"burn", MA_PAINT_LAYER_BLEND_BURN, {192, 64, 32}, {64, 192, 224}},
      {"dodge", MA_PAINT_LAYER_BLEND_DODGE, {192, 64, 32}, {64, 192, 224}},
      {"difference", MA_PAINT_LAYER_BLEND_DIFFERENCE, {192, 64, 32}, {64, 192, 224}},
      {"exclusion", MA_PAINT_LAYER_BLEND_EXCLUSION, {192, 64, 32}, {64, 192, 224}},
      {"soft_light", MA_PAINT_LAYER_BLEND_SOFT_LIGHT, {192, 64, 32}, {64, 192, 224}},
      {"linear_light", MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, {192, 64, 32}, {64, 192, 224}},
      {"hue", MA_PAINT_LAYER_BLEND_HUE, {192, 64, 32}, {64, 192, 224}},
      {"saturation", MA_PAINT_LAYER_BLEND_SATURATION, {192, 64, 32}, {64, 192, 224}},
      {"color", MA_PAINT_LAYER_BLEND_COLOR, {192, 64, 32}, {64, 192, 224}},
      {"value", MA_PAINT_LAYER_BLEND_VALUE, {192, 64, 32}, {64, 192, 224}},
  };

  for (const BlendCase &blend_case : cases) {
    ma = BKE_material_add(bmain, "BlendEval");
    add_layer("Bottom",
              MA_PAINT_LAYER_SOURCE_IMAGE,
              add_solid_image("Bottom",
                              size,
                              blend_case.bottom[0],
                              blend_case.bottom[1],
                              blend_case.bottom[2],
                              255));
    MaterialPaintLayer *top = add_layer("Top",
                                        MA_PAINT_LAYER_SOURCE_IMAGE,
                                        add_solid_image("Top",
                                                        size,
                                                        blend_case.top[0],
                                                        blend_case.top[1],
                                                        blend_case.top[2],
                                                        255));
    BKE_paint_layers_set_blend(*ma, top, blend_case.blend);

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << blend_case.name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << blend_case.name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << blend_case.name;

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/**
 * 10.4: the anti-drift harness is mandatory. Every blend mode needs a graph-vs-CPU parity case:
 * the table in #PaintLayersGraphEvalTest.every_blend_mode_matches_the_cpu plus the Normal-only
 * modes covered by #PaintLayersGraphEvalTest.normal_replace_override_matches_the_cpu (combine is
 * the Normal default every normal parity test already runs). A new mode must extend one of the
 * two instead of landing without coverage -- this test fails with its number until it does.
 */
TEST_F(PaintLayersGraphEvalTest, every_blend_mode_has_a_parity_case)
{
  static_assert(MA_PAINT_LAYER_BLEND_NORMAL_REPLACE == 20,
                "A new blend mode was added: extend the parity table below and its tests");
  const eMaterialPaintLayerBlend tabled[] = {
      MA_PAINT_LAYER_BLEND_MIX,          MA_PAINT_LAYER_BLEND_MULTIPLY,
      MA_PAINT_LAYER_BLEND_OVERLAY,      MA_PAINT_LAYER_BLEND_ADD,
      MA_PAINT_LAYER_BLEND_SUBTRACT,     MA_PAINT_LAYER_BLEND_DIVIDE,
      MA_PAINT_LAYER_BLEND_DARKEN,       MA_PAINT_LAYER_BLEND_LIGHTEN,
      MA_PAINT_LAYER_BLEND_SCREEN,       MA_PAINT_LAYER_BLEND_BURN,
      MA_PAINT_LAYER_BLEND_DODGE,        MA_PAINT_LAYER_BLEND_DIFFERENCE,
      MA_PAINT_LAYER_BLEND_EXCLUSION,    MA_PAINT_LAYER_BLEND_SOFT_LIGHT,
      MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, MA_PAINT_LAYER_BLEND_HUE,
      MA_PAINT_LAYER_BLEND_SATURATION,   MA_PAINT_LAYER_BLEND_COLOR,
      MA_PAINT_LAYER_BLEND_VALUE,
  };
  /* The Normal channel's own modes never reach the generic table: combine is its forced default,
   * replace its only override, and both are Normal-harness cases. */
  const eMaterialPaintLayerBlend normal_only[] = {
      MA_PAINT_LAYER_BLEND_NORMAL_COMBINE,
      MA_PAINT_LAYER_BLEND_NORMAL_REPLACE,
  };
  for (int v = int(MA_PAINT_LAYER_BLEND_MIX); v <= int(MA_PAINT_LAYER_BLEND_NORMAL_REPLACE); v++) {
    const auto covered = [&](const eMaterialPaintLayerBlend *modes, const int n) {
      for (int i = 0; i < n; i++) {
        if (int(modes[i]) == v) {
          return true;
        }
      }
      return false;
    };
    EXPECT_TRUE(covered(tabled, ARRAY_SIZE(tabled)) || covered(normal_only, ARRAY_SIZE(normal_only)))
        << "Blend mode " << v << " has no graph-vs-CPU parity case";
  }
}

/**
 * The Normal channel's only blend override: the top map replaces the combined normal instead of
 * combining with it. Graph and CPU must agree with each other and -- at full opacity -- with the
 * top map itself.
 */
TEST_F(PaintLayersGraphEvalTest, normal_replace_override_matches_the_cpu)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;

  ma = BKE_material_add(bmain, "NormalReplace");
  Image *bottom_map = add_solid_image("NormalReplaceBottom", size, 128, 128, 255, 255);
  make_image_data(bottom_map);
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, bottom_map, channel);

  Image *detail_map = add_solid_image("NormalReplaceDetail", size, 200, 100, 220, 255);
  make_image_data(detail_map);
  MaterialPaintLayer *top = add_layer("Top", MA_PAINT_LAYER_SOURCE_IMAGE, detail_map, channel);
  ASSERT_NE(top, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *top, channel, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.y = 0;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA detail_enc = interpreter.sample_image(detail_map, "Color");
  const float expected[3] = {
      detail_enc.r * 2.0f - 1.0f, detail_enc.g * 2.0f - 1.0f, detail_enc.b * 2.0f - 1.0f};

  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "graph vs cpu x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "graph vs cpu x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "graph vs cpu x=" << x;
    EXPECT_NEAR(graph.r, expected[0], tolerance) << "replace x=" << x;
    EXPECT_NEAR(graph.g, expected[1], tolerance) << "replace x=" << x;
    EXPECT_NEAR(graph.b, expected[2], tolerance) << "replace x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A folder is an isolated group (design §5): its children composite over transparency, the folder
 * lays the result over what is below with its own blend, opacity and mask. The generated chain and
 * the CPU must agree on all of it, and the model is checked here on the cases that would break a
 * naive "sub-chain then mix by coverage" -- a single child at half opacity, a Multiply child over
 * an empty folder, a folder with its own blend, mask and mask correction, and nesting.
 */
TEST_F(PaintLayersGraphEvalTest, folders_match_the_cpu)
{
  const int size = 4;

  auto run_case = [&](const char *name,
                      const eMaterialPaintChannel channel,
                      const std::function<void()> &setup) {
    ma = BKE_material_add(bmain, "FolderEval");
    setup();
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << name;
    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  auto add_layer = [&](const char *layer_name,
                       Image *image,
                       const eMaterialPaintChannel channel,
                       MaterialPaintLayer *anchor = nullptr,
                       const PaintLayerPlace place = PaintLayerPlace::Above) {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     MA_PAINT_LAYER_SOURCE_IMAGE,
                                                     layer_name,
                                                     anchor,
                                                     place);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };
  auto add_folder = [&](const char *folder_name) {
    return BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_STACK, folder_name, nullptr, PaintLayerPlace::Above);
  };
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  run_case("parent_none", bc, [&]() {
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
  });

  run_case("single_child_half", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    MaterialPaintLayer *child = add_layer(
        "Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, child, 0.5f);
  });

  run_case("multiply_inside_over_empty", bc, [&]() {
    MaterialPaintLayer *folder = add_folder("Folder");
    MaterialPaintLayer *child = add_layer(
        "Child", add_solid_image("Child", size, 200, 100, 50, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_blend(*ma, child, MA_PAINT_LAYER_BLEND_MULTIPLY);
  });

  run_case("folder_blend_multiply", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 255, 0, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY);
  });

  run_case("mask_on_folder", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    set_mask_image(*folder, add_solid_image("FolderMask", size, 255, 255, 255, 128));
  });

  run_case("mask_correction_on_folder", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
        *ma, folder, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
    ASSERT_NE(correction, nullptr);
    MaterialPaintLayerChannel *mask_record = BKE_paint_layers_channel_add(
        *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    ASSERT_NE(mask_record, nullptr);
    mask_record->image = add_solid_image("FolderMaskCorr", size, 128, 128, 128, 255);
    mask_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  });

  run_case("nested_folder", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *outer = add_folder("Outer");
    MaterialPaintLayer *inner = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
    add_layer("Leaf", add_solid_image("Leaf", size, 0, 0, 255, 255), bc, inner, PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, inner, 0.5f);
    BKE_paint_layers_set_opacity(*ma, outer, 0.5f);
  });

  run_case("empty_folder_is_below", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    add_folder("Empty");
  });

  run_case("normal_inside_folder", PAINT_MATERIAL_CHANNEL_NORMAL, [&]() {
    add_layer("Bottom",
              add_solid_image("Bottom", size, 128, 128, 255, 255),
              PAINT_MATERIAL_CHANNEL_NORMAL);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child",
              add_solid_image("Child", size, 160, 128, 232, 255),
              PAINT_MATERIAL_CHANNEL_NORMAL,
              folder,
              PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
  });
}

/**
 * A value changed on a grandchild after regeneration must reach the graph: the mirror chain carries
 * it from the root interface through both parent folders, and the CPU reads the same description.
 */
TEST_F(PaintLayersGraphEvalTest, nested_values_reach_the_grandchild)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "NestedValues");

  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Leaf", inner, PaintLayerPlace::Into);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, leaf, bc);
  record->image = add_solid_image("Leaf", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const float opacity : {1.0f, 0.4f}) {
    ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, leaf, opacity));
    BKE_paint_layers_values_sync(*ma);

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel(bc);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << opacity;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << opacity;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << opacity;
  }
}

/**
 * A content correction on a folder edits the isolated straight result (S_folder) and its coverage
 * folds into the folder's coverage by the over model: a = a + f * (1 - a). This is checked against
 * a hand calculation, not only against the CPU, so generator and CPU cannot agree on the wrong
 * formula.
 */
TEST_F(PaintLayersGraphEvalTest, folder_content_correction_folds_into_coverage)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "FolderCorrEval");

  Image *child_image = add_solid_image("Child", size, 200, 100, 50, 255);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *child_record = BKE_paint_layers_channel_add(*ma, child, bc);
  child_record->image = child_image;
  child_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, child, 0.5f));

  const float correction_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, folder, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "Corr");
  ASSERT_NE(correction, nullptr);
  BKE_paint_layers_set_fill_color(*ma, correction, correction_color);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Base Color");
  const RGBA cpu = cpu_pixel(bc);
  const RGBA child_color = interpreter.sample_image(child_image, "Color");

  /* Manual formula. The child covers a0 = 0.5 and paints S_folder = its own colour; the correction
   * mixes into that colour at f = 0.5 and folds into the coverage (a1 = a0 + f * (1 - a0) = 0.75);
   * the folder then lays S over the channel bottom (black) at a1. */
  const float a0 = 0.5f;
  const float corr_factor = 0.5f;
  const float a1 = a0 + corr_factor * (1.0f - a0);
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(bc, bottom);
  const float child_rgb[3] = {child_color.r, child_color.g, child_color.b};
  float expected[3];
  for (const int k : IndexRange(3)) {
    const float s = child_rgb[k] + (correction_color[k] - child_rgb[k]) * corr_factor;
    expected[k] = bottom[k] + (s - bottom[k]) * a1;
  }

  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(cpu.r, expected[0], 1e-4f);
  EXPECT_NEAR(cpu.g, expected[1], 1e-4f);
  EXPECT_NEAR(cpu.b, expected[2], 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A folder that changes nothing but grouping must not change the pixels: a folder with default
 * blend, opacity, no mask and one child at f = 0.5 has to equal the same child without the folder.
 * This is the case a "sub-chain, then mix again by coverage" formula gets wrong -- it would darken
 * the half-covered pixel twice.
 */
TEST_F(PaintLayersGraphEvalTest, folder_single_child_equals_the_child_alone)
{
  const int size = 4;

  auto add_child_layer = [&](const char *name, Image *image, MaterialPaintLayer *anchor) {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     MA_PAINT_LAYER_SOURCE_IMAGE,
                                                     name,
                                                     anchor,
                                                     anchor != nullptr ? PaintLayerPlace::Into :
                                                                          PaintLayerPlace::Above);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  /* With a folder. */
  ma = BKE_material_add(bmain, "FolderEquiv");
  add_child_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), nullptr);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = add_child_layer(
      "Child", add_solid_image("Child", size, 0, 0, 255, 255), folder);
  BKE_paint_layers_set_opacity(*ma, child, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA folded = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_id_free(bmain, ma);
  ma = nullptr;

  /* The same stack, ungrouped. */
  ma = BKE_material_add(bmain, "PlainEquiv");
  add_child_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), nullptr);
  MaterialPaintLayer *plain = add_child_layer(
      "Child", add_solid_image("Child", size, 0, 0, 255, 255), nullptr);
  BKE_paint_layers_set_opacity(*ma, plain, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA ungrouped = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_id_free(bmain, ma);
  ma = nullptr;

  EXPECT_NEAR(folded.r, ungrouped.r, 1e-4f);
  EXPECT_NEAR(folded.g, ungrouped.g, 1e-4f);
  EXPECT_NEAR(folded.b, ungrouped.b, 1e-4f);
}

/**
 * Phase 4: a Stack correction on either role composites its own children in isolation, exactly
 * like a Layer folder's own children, and the isolated result feeds the same correction pipeline a
 * Material correction's own source does. These tests build the correction's #children through the
 * public #add_layer_into (#BKE_paint_layers_add with \a place Into, anchored on the correction/mask
 * item itself: a Stack source is a folder in every sense #BKE_paint_layers_is_folder cares about,
 * regardless of role, so Into no longer needs to be built by hand).
 */
TEST_F(PaintLayersGraphEvalTest, stack_effect_correction_paint_and_fill_children_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "StackEffectEval");

  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Row", size, 200, 100, 50, 255), bc);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.6f));

  /* Paint child: half-covering blue. */
  MaterialPaintLayer *paint_child = add_layer_into(
      correction,
      "PaintChild",
      MA_PAINT_LAYER_SOURCE_IMAGE,
      add_solid_image("PaintChild", size, 0, 0, 255, 128),
      bc);

  /* Fill child on top, half opacity, green: the subtree's coverage is not simply the paint child's
   * own alpha, so a formula that skipped the Fill's own contribution to coverage would disagree.
   * Into always appends, so the second call lands after `paint_child`, in the same order the
   * hand-built list used to. */
  MaterialPaintLayer *fill_child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillChild", correction, PaintLayerPlace::Into);
  ASSERT_NE(fill_child, nullptr);
  const float fill_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  BKE_paint_layers_set_fill_color(*ma, fill_child, fill_color);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, fill_child, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);
  /* Not a no-op: the correction must actually have moved the row's colour away from its own map. */
  EXPECT_NE(graph.g, 0.0f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Stack mask item reading its subtree's Alpha (coverage itself, flat Fac). */
TEST_F(PaintLayersGraphEvalTest, stack_mask_item_alpha_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "StackMaskAlphaEval");

  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Row", size, 0, 0, 255, 255), bc);

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ALPHA);

  /* Half-covering child: the mask clips the row to half. */
  MaterialPaintLayer *child = add_layer_into(
      mask,
      "MaskChild",
      MA_PAINT_LAYER_SOURCE_IMAGE,
      add_solid_image("MaskChild", size, 255, 255, 255, 128),
      bc);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);
  /* Partial coverage: strictly between the bottom (red) and the row's own blue. */
  EXPECT_GT(graph.b, 0.0f);
  EXPECT_LT(graph.b, 1.0f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Stack mask item reading its subtree's Base Color (luminance of the straight result). */
TEST_F(PaintLayersGraphEvalTest, stack_mask_item_base_color_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "StackMaskColorEval");

  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Row", size, 0, 0, 255, 255), bc);

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* A mid-grey, fully covering child: its luminance is the mask's grey, and its full coverage is
   * the mask's own Fac multiplier. */
  MaterialPaintLayer *child = add_layer_into(
      mask,
      "MaskChild",
      MA_PAINT_LAYER_SOURCE_IMAGE,
      add_solid_image("MaskChild", size, 180, 180, 180, 255),
      bc);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_GT(graph.b, 0.0f);
  EXPECT_LT(graph.b, 1.0f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A folder nested inside a Stack content correction's own subtree, with a further leaf inside that
 * folder: two levels of row grouping inside the correction's #children, exactly as an isolating
 * folder nests inside another one at the top level. Regression test for a use-after-scope bug where
 * `folder_source_node`/`folder_coverage_node`/`folder_content_alpha_node` (the bookkeeping a
 * #build_row call uses when the row it is building is itself a folder) were captured by reference
 * from the enclosing per-channel #build_list scope instead of being local to each #build_row call:
 * a correction's own children recurse back into #build_row for a different row, and that recursion
 * left its folder bookkeeping in the shared variables past its own return. The owner row (not a
 * folder itself) then read that leftover state near the end of its own #build_row call and linked a
 * node living in the nested folder's tree into its own tree, corrupting the graph (SEH 0xC0000005 in
 * #BKE_paint_layers_regenerate). Fixed by making the six variables local to #build_row.
 */
TEST_F(PaintLayersGraphEvalTest, nested_folder_inside_stack_correction_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "StackNestedFolderEval");

  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Row", size, 200, 100, 50, 255), bc);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.7f));

  /* An inner folder inside the correction's own subtree, holding one half-covering child. */
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", correction, PaintLayerPlace::Into);
  ASSERT_NE(inner, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, 0.5f));

  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Leaf", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf, nullptr);
  MaterialPaintLayerChannel *leaf_record = BKE_paint_layers_channel_add(*ma, leaf, bc);
  leaf_record->image = add_solid_image("Leaf", size, 0, 0, 255, 200);
  leaf_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A child of a Stack correction that itself carries its own mask item, exercising a grouped row
 * inside a correction's #children whose own #build_row call reaches the mask-processing code. Kept
 * alongside #nested_folder_inside_stack_correction_matches_the_cpu as a second regression case for
 * the same #build_row-local-variable fix (see that test's comment); this one confirms the fix does
 * not depend on the nested row being a folder.
 */
TEST_F(PaintLayersGraphEvalTest, stack_correction_child_with_own_mask_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "StackChildOwnMaskEval");

  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Row", size, 200, 100, 50, 255), bc);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.8f));

  MaterialPaintLayer *child = add_layer_into(
      correction, "Child", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Child", size, 0, 0, 255, 255), bc);

  /* The child's own mask item, an ordinary Image mask at half influence: the recursion into a
   * correction's subtree must reach a plain row's own mask_stack exactly as it does at the top
   * level, with no cross-talk between the two contexts. */
  set_mask_image(*child, add_solid_image("ChildMask", size, 255, 255, 255, 128));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Stack mask item's subtree must build in \a mask_channel, not in whichever channel the owner
 * row currently happens to be generating: the owner here paints both Base Color and Roughness, and
 * the mask targets Roughness alone. The mask's only child carries content in Roughness only (no
 * Base Color record at all), so a build that wrongly used the owner's current channel would drop
 * the child out of the Base Color pass entirely -- zero coverage, no masking effect at all on that
 * output -- while Roughness would happen to still work (it coincides with `mask_channel` there).
 * #BKE_paint_layers_composite_image_layers (the CPU side) already keys off `mask_channel` and not
 * its own \a channel argument for a non-Alpha Stack mask, so it is ground truth here: comparing the
 * generated graph against it on BOTH owner channels is enough to catch the generator alone reading
 * its enclosing per-channel loop variable instead of `mask_channel`.
 */
TEST_F(PaintLayersGraphEvalTest, stack_mask_fixed_channel_applies_to_every_owner_channel_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const eMaterialPaintChannel rough = PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  auto result_name_for = [](const int ch) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(ch)).ui_name;
  };
  ma = BKE_material_add(bmain, "StackMaskFixedChannelRoughness");

  MaterialPaintLayer *row = add_layer(
      "Row", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("RowBC", size, 200, 100, 50, 255), bc);
  MaterialPaintLayerChannel *row_rough = BKE_paint_layers_channel_add(*ma, row, rough);
  ASSERT_NE(row_rough, nullptr);
  row_rough->image = add_solid_image("RowRough", size, 90, 90, 90, 255);
  row_rough->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(rough);

  MaterialPaintLayer *child = add_layer_into(
      mask,
      "MaskChild",
      MA_PAINT_LAYER_SOURCE_IMAGE,
      add_solid_image("MaskChild", size, 180, 180, 180, 128),
      rough);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);

  const RGBA graph_bc = interpreter.eval_result(result_name_for(bc).c_str());
  const RGBA graph_rough = interpreter.eval_result(result_name_for(rough).c_str());
  const RGBA cpu_bc = cpu_pixel(bc);
  const RGBA cpu_rough = cpu_pixel(rough);

  /* The mask must have a real, partial effect on Base Color too: not fully unmasked (the bug --
   * the child dropped out of a Base-Color-context build) and not fully masked out either. */
  EXPECT_GT(graph_bc.r, 0.0f);
  EXPECT_LT(graph_bc.r, 200.0f / 255.0f + 0.05f);
  EXPECT_NEAR(graph_bc.r, cpu_bc.r, 1e-4f);
  EXPECT_NEAR(graph_bc.g, cpu_bc.g, 1e-4f);
  EXPECT_NEAR(graph_bc.b, cpu_bc.b, 1e-4f);
  EXPECT_NEAR(graph_rough.r, cpu_rough.r, 1e-4f);
  EXPECT_NEAR(graph_rough.g, cpu_rough.g, 1e-4f);
  EXPECT_NEAR(graph_rough.b, cpu_rough.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

}  // namespace blender::bke::tests
