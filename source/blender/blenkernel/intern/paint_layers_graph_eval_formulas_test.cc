/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: correction opacity formulas and transitions (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


/**
 * The reported stack: a Material row active (the correction selected in the Outliner defers the
 * row), so it reads its source live through the wrapper. The correction's Base Color opacity slider
 * must change the pixels; before the fix the RNA setter could not resolve the correction and the
 * result stayed at the full-map mix.
 */
TEST_F(PaintLayersGraphEvalTest, material_source_group_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatCorrSourceGroup");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  Material *source = make_specular_source(*bmain, "MatCorrWiredSource", false);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  Image *corr_map = add_solid_image("CorrMap", size, 0, 0, 255, 128);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");
  const float source_rgb[3] = {0.2f, 0.5f, 0.9f};

  expect_correction_mix(
      sample, source_rgb, 1.0f, eval_channel_result(interpreter, bc), "op=1");

  bke_set_channel_opacity(*ma, *correction, bc, 0.35f);
  expect_correction_mix(
      sample, source_rgb, 0.35f, eval_channel_result(interpreter, bc), "op=0.35");

  /* Hiding the correction is the same factor at zero. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, false));
  BKE_paint_layers_values_sync(*ma);
  expect_correction_mix(
      sample, source_rgb, 0.0f, eval_channel_result(interpreter, bc), "hidden");

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, true));
  BKE_paint_layers_values_sync(*ma);
  expect_correction_mix(
      sample, source_rgb, 0.35f, eval_channel_result(interpreter, bc), "re-enabled");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same on a Material row read through the Hybrid path (its channel has no baked map yet). */
TEST_F(PaintLayersGraphEvalTest, material_hybrid_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatCorrHybrid");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "MatCorrHybridSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  Image *corr_map = add_solid_image("CorrMap", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");

  expect_correction_mix(sample, row_color, 1.0f, eval_channel_result(interpreter, bc), "op=1");
  bke_set_channel_opacity(*ma, *correction, bc, 0.5f);
  expect_correction_mix(sample, row_color, 0.5f, eval_channel_result(interpreter, bc), "op=0.5");

  /* The CPU reproduces the Hybrid result, so both sides read the same opacity. */
  const RGBA graph = eval_channel_result(interpreter, bc);
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same on a Material row read from its baked maps (Baked): the map is the row's content. */
TEST_F(PaintLayersGraphEvalTest, material_baked_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatCorrBaked");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "MatCorrBakedSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* A baked map in every channel makes the row ineligible to read its source live. */
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "MatCorrCoverage", size, white);
  ASSERT_NE(coverage, nullptr);
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    const float rgba[4] = {row_color[0], row_color[1], row_color[2], 1.0f};
    Image *map = make_bake_data_map(bmain, "MatCorrBake", size, rgba);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, channel, map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);

  Image *corr_map = add_solid_image("CorrMap", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");

  expect_correction_mix(sample, row_color, 1.0f, eval_channel_result(interpreter, bc), "op=1");
  bke_set_channel_opacity(*ma, *correction, bc, 0.25f);
  expect_correction_mix(sample, row_color, 0.25f, eval_channel_result(interpreter, bc), "op=0.25");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec-F2-C4a / material-alpha semantics: a Material row read from its baked maps (Baked mode), on a
 * non-ALPHA tracked channel, inside an isolating folder. A Material row's transparency is its Alpha
 * input, and a real bake map is opaque; the channel map's own alpha (kept partial here on purpose,
 * in a data-space map) is therefore ignored -- neither its `content_cov` nor its content alpha. The
 * row's factor is its coverage bake (1.0), its content alpha is 1, and the folder lays it at
 * `folder_opacity * coverage`.
 */
TEST_F(PaintLayersGraphEvalTest, material_baked_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float content_alpha = 0.4f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MatBakedIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("MatBakedIsoBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "MatBakedFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  const float row_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "MatBakedIsoSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", folder, PaintLayerPlace::Into);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* A baked map in every channel makes the row ineligible to read its source live (Baked mode). */
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "MatBakedIsoCoverage", size, white);
  ASSERT_NE(coverage, nullptr);
  for (int c = 0; c < PAINT_MATERIAL_CHANNEL_NUM; c++) {
    Image *map = nullptr;
    if (c == channel) {
      /* Base Color deliberately carries a partial alpha, in a data-space map so both sides read
       * the bytes as-is (a real bake is opaque; this proves the alpha is ignored rather than
       * relied on). #make_bake_data_map always writes alpha 1.0, so the byte helper is used. */
      map = add_solid_image("MatBakedIsoBake",
                            size,
                            uchar(row_color[0] * 255.0f + 0.5f),
                            uchar(row_color[1] * 255.0f + 0.5f),
                            uchar(row_color[2] * 255.0f + 0.5f),
                            uchar(content_alpha * 255.0f + 0.5f));
    }
    else {
      /* Every other channel is opaque so it cannot influence the row's own coverage. */
      const float rgba[4] = {row_color[0], row_color[1], row_color[2], 1.0f};
      map = make_bake_data_map(bmain, "MatBakedIsoBake", size, rgba);
    }
    ASSERT_NE(map, nullptr);
    make_image_data(map);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, c, map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "MatBakedFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_FALSE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
  for (int x = 0; x < size; x++) {
    const int y = 0;
    /* A Material row's factor is its coverage bake (white, 1.0) times the Alpha input (the constant
     * 1): the channel map's partial alpha is ignored, so the row's own factor is 1 and the folder's
     * is `folder_opacity`. With one row and no accumulation below it, the straight colour the
     * divide-by-coverage produces is the row's own colour, and no content-alpha socket is grown. */
    const float row_factor = 1.0f;
    const float factor = folder_opacity * row_factor;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + row_color[0] * factor,
                           bottom_color[1] * (1.0f - factor) + row_color[1] * factor,
                           bottom_color[2] * (1.0f - factor) + row_color[2] * factor,
                           0.0f};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Coverage Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, factor, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Spec-F2-C4a guard: a Material row read from its baked maps, on the ALPHA channel itself. Alpha has
 * no bake slot of its own (#paint_layer_material_source_map answers it from
 * #MaterialPaintLayerBake::coverage), and that same image already feeds the row's own coverage
 * factor, so folding it in again as this channel's content alpha would double-count it. The
 * isolating folder must therefore grow no "Content Alpha Alpha" socket at all.
 */
TEST_F(PaintLayersGraphEvalTest, material_baked_alpha_channel_isolating_folder_adds_no_content_alpha)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_ALPHA;

  ma = BKE_material_add(bmain, "MatBakedAlphaIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("MatBakedAlphaIsoBottom", size, 255, 255, 255, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "MatBakedAlphaFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  const float row_color[4] = {0.2f, 0.6f, 0.8f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "MatBakedAlphaIsoSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", folder, PaintLayerPlace::Into);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "MatBakedAlphaIsoCoverage", size, white);
  ASSERT_NE(coverage, nullptr);
  for (int c = 0; c < PAINT_MATERIAL_CHANNEL_NUM; c++) {
    Image *map = make_bake_data_map(bmain, "MatBakedAlphaIsoBake", size, white);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, c, map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "MatBakedAlphaFolder");
  ASSERT_NE(folder_tree, nullptr);
  EXPECT_FALSE(folder_interface_has_socket(*folder_tree, "Content Alpha Alpha"));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Material row read live (Hybrid mode) whose Base Color resolves to a constant (unlinked RGBA
 * socket, #ChannelResolution::Constant). A Material row's transparency is the Principled Alpha
 * input, not the Base Color socket's own `.a`; its content alpha is a constant 1 (opaque), so the
 * non-opaque Base Color alpha deliberately set below must change nothing.
 */
TEST_F(PaintLayersGraphEvalTest, material_hybrid_live_constant_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float content_alpha = 0.4f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MatHybridConstIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("MatHybridConstIsoBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "MatHybridConstFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  /* The Base Color RGBA socket's own `.a` (0.4) is deliberately non-opaque: a Material row's
   * transparency is the Principled Alpha input (here the default 1.0), so this alpha must be
   * ignored -- it neither scales the factor nor becomes the row's content alpha. */
  const float row_color[4] = {0.0f, 1.0f, 0.0f, content_alpha};
  Material *source = make_constant_principled_source(*bmain, "MatHybridConstIsoSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", folder, PaintLayerPlace::Into);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "MatHybridConstFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_FALSE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
  const float leaf_color[3] = {row_color[0], row_color[1], row_color[2]};
  for (int x = 0; x < size; x++) {
    const int y = 0;
    /* The row's factor is its Alpha input (the constant 1); the Base Color socket's own `.a` (0.4)
     * is ignored, and a Material row tracks no content alpha, so the folder grows none. */
    const float factor = folder_opacity;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + leaf_color[0] * factor,
                           bottom_color[1] * (1.0f - factor) + leaf_color[1] * factor,
                           bottom_color[2] * (1.0f - factor) + leaf_color[2] * factor,
                           0.0f};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Coverage Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, folder_opacity, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same on a plain Paint row, so the correction path stays green for it too. */
TEST_F(PaintLayersGraphEvalTest, paint_row_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "PaintCorr");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
  Image *row_map = add_solid_image("Row", size, 30, 200, 60, 255);
  MaterialPaintLayer *row = add_layer("Row", MA_PAINT_LAYER_SOURCE_IMAGE, row_map, bc);

  Image *corr_map = add_solid_image("CorrMap", size, 10, 10, 240, 160);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");
  const RGBA row_sample = interpreter.sample_image(row_map, "Color");
  const float source_rgb[3] = {row_sample.r, row_sample.g, row_sample.b};

  expect_correction_mix(sample, source_rgb, 1.0f, eval_channel_result(interpreter, bc), "op=1");
  bke_set_channel_opacity(*ma, *correction, bc, 0.4f);
  expect_correction_mix(sample, source_rgb, 0.4f, eval_channel_result(interpreter, bc), "op=0.4");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Fill content correction covers fully, so its factor is the opacity alone; the same per (row,
 * channel) slider drives it. This is the other kind whose RNA setter had to resolve a correction.
 */
TEST_F(PaintLayersGraphEvalTest, material_correction_fill_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatFillCorr");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  Material *source = make_specular_source(*bmain, "MatFillCorrSource", false);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  const float fill[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  BKE_paint_layers_set_fill_color(*ma, correction, fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = {fill[0], fill[1], fill[2], 1.0f};
  const float source_rgb[3] = {0.2f, 0.5f, 0.9f};

  bke_set_channel_opacity(*ma, *correction, bc, 0.4f);
  expect_correction_mix(sample, source_rgb, 0.4f, eval_channel_result(interpreter, bc), "fill op");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** \} */

TEST_F(PaintLayersGraphEvalTest, source_group_material_in_pass_through_and_isolating_folder)
{
  Material *source_c = build_group_source(
      *bmain, "ReproSourceC", "ReproGroupC", "ReproMapC", "ReproSharedC", source_spec_c);
  Material *source_b = build_hybrid_source(
      *bmain, "ReproSourceB", "ReproNormalB", source_spec_b);
  ASSERT_NE(source_c, nullptr);
  ASSERT_NE(source_b, nullptr);
  ma = BKE_material_add(bmain, "ReproStack");

  MaterialPaintLayer *iso = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(iso, nullptr);
  BKE_paint_layers_set_opacity(*ma, iso, 0.5f);
  MaterialPaintLayer *b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatB", iso, PaintLayerPlace::Into);
  ASSERT_NE(b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, b, source_b));

  MaterialPaintLayer *pass = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Pass", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(pass, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *pass));
  MaterialPaintLayer *c = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatC", pass, PaintLayerPlace::Into);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, c, source_c));

  auto find_wrapper = [&]() -> bNodeTree * {
    for (bNodeTree &tree : bmain->nodetrees) {
      if (STREQ(tree.id.name + 2, ".PL Source ReproSourceC")) {
        return &tree;
      }
    }
    return nullptr;
  };

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper = find_wrapper();
  ASSERT_NE(wrapper, nullptr);
  const int users_after_first = wrapper->id.us;
  EXPECT_GT(users_after_first, 0);

  /* A second regeneration must reuse the same wrapper instance without another user. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper_again = find_wrapper();
  ASSERT_NE(wrapper_again, nullptr);
  EXPECT_EQ(wrapper_again->id.us, users_after_first);

  /* Removing the row releases the wrapper (or the wrapper is pruned): never a leftover user. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, c));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper_after = find_wrapper();
  if (wrapper_after != nullptr) {
    EXPECT_LT(wrapper_after->id.us, users_after_first);
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The Baked->SourceGroup rebuild of a Material row at the root, next to an isolating folder and a
 * Pass Through folder whose child is a SourceGroup row. The active change moves row A from Baked to
 * SourceGroup, which rebuilds A's layer group; the non-supplied Normal channel must not make the
 * generator link A's wrapper instance into the root tree.
 */
TEST_F(PaintLayersGraphEvalTest, material_source_group_transition_next_to_both_folder_kinds)
{
  const int size = 2;
  Material *source_a = build_group_source(
      *bmain, "ReproTransSourceA", "ReproTransGroupA", "ReproTransMapA", "ReproTransSharedA",
      source_spec_a);
  Material *source_b = build_hybrid_source(
      *bmain, "ReproTransSourceB", "ReproTransNormalB", source_spec_b);
  Material *source_c = build_group_source(
      *bmain, "ReproTransSourceC", "ReproTransGroupC", "ReproTransMapC", "ReproTransSharedC",
      source_spec_c);
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  ASSERT_NE(source_c, nullptr);
  ma = BKE_material_add(bmain, "ReproTransStack");

  MaterialPaintLayer *a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatA", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, a, source_a));
  for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
    RGBA raw;
    if (!group_source_expected(source_spec_a, int(channel), 0, 0, raw)) {
      continue;
    }
    const float rgba[4] = {raw.r, raw.g, raw.b, 1.0f};
    Image *map = make_bake_data_map(bmain, "ReproTransBakeA", size, rgba);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *a, int(channel), map));
  }
  const float coverage_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage_map = make_bake_data_map(bmain, "ReproTransCoverageA", size, coverage_rgba);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *a, -1, coverage_map));
  BKE_paint_layers_bake_finalize(*ma, *a);

  MaterialPaintLayer *iso = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(iso, nullptr);
  BKE_paint_layers_set_opacity(*ma, iso, 0.5f);
  MaterialPaintLayer *b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatB", iso, PaintLayerPlace::Into);
  ASSERT_NE(b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, b, source_b));

  MaterialPaintLayer *pass = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Pass", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(pass, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *pass));
  MaterialPaintLayer *c = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatC", pass, PaintLayerPlace::Into);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, c, source_c));

  /* First build: A is not deferred, so its valid bakes make it Baked. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *a), PaintLayerMaterialMode::Baked);

  /* Move the active row to A: it becomes SourceGroup and its group is rebuilt. */
  BKE_paint_layers_active_set(*ma, a->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *a), PaintLayerMaterialMode::SourceGroup);

  /* The next regeneration must rebuild nothing. */
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root)));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name Full parity stack
 *
 * One stack with every construct that mattered so far, bottom to top: a Fill, Material A
 * (SourceGroup) with a mask and a Paint correction, an isolating folder (Material B, Hybrid, and a
 * Paint row), a Pass Through folder holding Material C (SourceGroup) and a Multiply Paint row on
 * top. The reference below composites the whole stack in one pass; the generated graph and the CPU
 * must both equal it for every active row, channel and point.
 * \{ */

static const int kFsSize = 2;
/** Mask map grey per texel (row-major 2x2): closed, partial, mostly open, open. Alpha is one. */
static const uchar kFsMaskBytes[4] = {0, 102, 204, 255};
/** A's Base Color correction map per texel: straight RGB bytes and a shared alpha of 0.6. */
static const uchar kFsCorrBytes[4][4] = {
    {230, 40, 40, 153}, {40, 230, 40, 153}, {40, 40, 230, 153}, {200, 200, 50, 153}};
static const float kFsMaskOpacity = 0.6f;
static const float kFsCorrOpacity = 0.5f;
static const float kFsIsoOpacity = 0.5f;
static const float kFsInnerPaintOpacity = 0.6f;
static const float kFsTopOpacity = 0.7f;
/** Below one, or C would cover everything A's mask and correction do. */

}  // namespace blender::bke::tests
