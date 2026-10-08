/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: multi-source rows and isolating folders (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


/**
 * A single Material row per source, read live. The generated chain must equal the
 * hand-computed composite of the source's clean channel over the channel bottom, with coverage =
 * the source alpha. The mode the contract picks (SourceGroup for the grouped graph, Hybrid for the
 * constant/trivial-map one) is asserted too.
 */
TEST_F(PaintLayersGraphEvalTest, multi_source_rows_match_the_reference)
{
  struct Case {
    const char *name;
    Material *source;
    PaintLayerMaterialMode expected_mode;
    float coverage;
  };

  const int points[3][2] = {{0, 0}, {1, 0}, {1, 1}};
  const float tolerance = 1e-4f;

  auto run_case = [&](const Case &test_case,
                      const bool grouped,
                      const GroupSourceSpec &group_spec,
                      const HybridSourceSpec &hybrid_spec) {
    ma = BKE_material_add(bmain, test_case.name);
    BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, test_case.source));
    /* The source carries Specular, Alpha and Emission, none of them in the build default set. */
    channel_set_extend(*ma,
                       channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                           channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA) |
                           channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));
    BKE_paint_layers_active_set(*ma, row->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    ASSERT_NE(ma->paint_layers_tree, nullptr);
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), test_case.expected_mode);

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      RGBA raw;
      const bool has_source = grouped ? group_source_expected(group_spec, channel, 0, 0, raw) :
                                        hybrid_source_expected(hybrid_spec, channel, raw);
      if (!has_source) {
        EXPECT_FALSE(result_output_exists(*ma->paint_layers_tree, channel))
            << info.ui_name << ": no source value, so the channel must not be wired";
        continue;
      }
      ASSERT_TRUE(result_output_exists(*ma->paint_layers_tree, channel)) << info.ui_name;
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        const int x = points[i][0];
        const int y = points[i][1];
        if (grouped) {
          ASSERT_TRUE(group_source_expected(group_spec, channel, x, y, raw));
        }
        const RGBA expected = composite_over(channel, raw, test_case.coverage);
        interpreter.x = x;
        interpreter.y = y;
        const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
        EXPECT_NEAR(graph.r, expected.r, tolerance)
            << info.ui_name << " point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.g, expected.g, tolerance)
            << info.ui_name << " point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.b, expected.b, tolerance)
            << info.ui_name << " point(" << x << "," << y << ")";
      }
    }

    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  Material *source_a = build_group_source(
      *bmain, "MultiSourceA", "MultiGroupA", "MultiMapA", "MultiSharedA", source_spec_a);
  Material *source_b = build_hybrid_source(
      *bmain, "MultiSourceB", "MultiNormalB", source_spec_b);
  Material *source_c = build_group_source(
      *bmain, "MultiSourceC", "MultiGroupC", "MultiMapC", "MultiSharedC", source_spec_c);
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  ASSERT_NE(source_c, nullptr);

  run_case({"MultiRowA", source_a, PaintLayerMaterialMode::SourceGroup, 1.0f},
           true,
           source_spec_a,
           {});
  run_case({"MultiRowB", source_b, PaintLayerMaterialMode::Hybrid, source_spec_b.alpha},
           false,
           source_spec_a,
           source_spec_b);
  run_case({"MultiRowC", source_c, PaintLayerMaterialMode::SourceGroup, 1.0f},
           true,
           source_spec_c,
           {});
}

/**
 * The same sources, each row shown from deterministic bake maps. Every resolvable
 * channel gets a 2x2 float data map holding the reference's clean value at each pixel, plus a grey
 * coverage map holding the source alpha. The row is not deferred, so the contract picks Baked; the
 * generated graph and the CPU composite must both equal the reference composite. The maps are made
 * the way the existing bake-substitution tests do: #BKE_paint_layers_bake_set_map for each channel
 * and `-1` (coverage), then #BKE_paint_layers_bake_finalize, which makes
 * #BKE_paint_layers_bake_is_valid hold.
 */
TEST_F(PaintLayersGraphEvalTest, multi_source_baked_rows_match_the_reference_and_the_cpu)
{
  const int size = 2;
  const int points[3][2] = {{0, 0}, {1, 0}, {1, 1}};
  const float tolerance = 1e-4f;

  auto run_case = [&](const char *name,
                      Material *source,
                      const bool grouped,
                      const GroupSourceSpec &group_spec,
                      const HybridSourceSpec &hybrid_spec,
                      const float coverage) {
    ma = BKE_material_add(bmain, name);
    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
    /* The baked source carries Specular, Alpha and Emission beyond the build default set. */
    channel_set_extend(*ma,
                       channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                           channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA) |
                           channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));

    auto raw_at = [&](const int channel, const int x, const int y, RGBA &r_out) -> bool {
      return grouped ? group_source_expected(group_spec, channel, x, y, r_out) :
                       hybrid_source_expected(hybrid_spec, channel, r_out);
    };

    /* A bake map per resolvable bakeable channel, the reference's clean value per pixel. */
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      RGBA probe;
      if (!raw_at(int(channel), 0, 0, probe)) {
        continue;
      }
      float pixels[4][3];
      for (const int y : IndexRange(size)) {
        for (const int x : IndexRange(size)) {
          RGBA raw;
          EXPECT_TRUE(raw_at(int(channel), x, y, raw));
          const int64_t texel = int64_t(y) * size + x;
          pixels[texel][0] = raw.r;
          pixels[texel][1] = raw.g;
          pixels[texel][2] = raw.b;
        }
      }
      char map_name[64];
      BLI_snprintf(map_name,
                   sizeof(map_name),
                   "%s %s",
                   name,
                   BKE_paint_material_channel_info(channel).ui_name);
      Image *map = make_data_pattern_map(bmain, map_name, size, pixels);
      ASSERT_NE(map, nullptr);
      ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, int(channel), map));
    }
    const float coverage_rgba[4] = {coverage, coverage, coverage, 1.0f};
    char coverage_name[64];
    BLI_snprintf(coverage_name, sizeof(coverage_name), "%s Coverage", name);
    Image *coverage_map = make_bake_data_map(bmain, coverage_name, size, coverage_rgba);
    ASSERT_NE(coverage_map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage_map));
    BKE_paint_layers_bake_finalize(*ma, *row);

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    ASSERT_NE(ma->paint_layers_tree, nullptr);
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
    EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      RGBA probe;
      if (!raw_at(channel, 0, 0, probe)) {
        continue;
      }
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        const int x = points[i][0];
        const int y = points[i][1];
        RGBA raw;
        ASSERT_TRUE(raw_at(channel, x, y, raw));
        const RGBA expected = composite_over(channel, raw, coverage);
        interpreter.x = x;
        interpreter.y = y;
        const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
        const RGBA cpu = cpu_pixel_at(channel, x, y);
        EXPECT_NEAR(graph.r, expected.r, tolerance)
            << info.ui_name << " graph point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.g, expected.g, tolerance)
            << info.ui_name << " graph point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.b, expected.b, tolerance)
            << info.ui_name << " graph point(" << x << "," << y << ")";
        EXPECT_NEAR(cpu.r, expected.r, tolerance)
            << info.ui_name << " cpu point(" << x << "," << y << ")";
        EXPECT_NEAR(cpu.g, expected.g, tolerance)
            << info.ui_name << " cpu point(" << x << "," << y << ")";
        EXPECT_NEAR(cpu.b, expected.b, tolerance)
            << info.ui_name << " cpu point(" << x << "," << y << ")";
      }
    }

    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  Material *source_a = build_group_source(
      *bmain, "MultiBakedSourceA", "MultiBakedGroupA", "MultiBakedMapA", "MultiBakedSharedA",
      source_spec_a);
  Material *source_b = build_hybrid_source(
      *bmain, "MultiBakedSourceB", "MultiBakedNormalB", source_spec_b);
  Material *source_c = build_group_source(
      *bmain, "MultiBakedSourceC", "MultiBakedGroupC", "MultiBakedMapC", "MultiBakedSharedC",
      source_spec_c);
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  ASSERT_NE(source_c, nullptr);

  run_case("MultiBakedA", source_a, true, source_spec_a, {}, 1.0f);
  run_case("MultiBakedB", source_b, false, source_spec_a, source_spec_b, source_spec_b.alpha);
  run_case("MultiBakedC", source_c, true, source_spec_c, {}, 1.0f);
}

/** Two Paint children of the isolating folder: distinct uniform data maps (alpha stored as one). */
static const float kIsolatingPaintB[4] = {0.20f, 0.80f, 0.40f, 1.0f};

/**
 * The reference for an isolating folder (opacity 0.5) holding two Paint children of opacity 0.6
 * and coverage one, laid over the channel bottom: the premultiplied accumulation of
 * `composite_folder_accumulate` (paint_material_composite.cc:702-775) and the
 * `opacity * coverage` overlay of `composite_apply_layer_linear` (`:777-804`). With two children
 * the second one evaluates the over's `(1 - A * op)` against non-zero premultiplied colour, which
 * is exactly what the `(1 - A)` mutation changes.
 */
static RGBA isolating_folder_expected(const int channel)
{
  const bool normal_channel = (channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  RGBA state = channel_bottom(channel);

  float premul[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float coverage = 0.0f;
  auto accumulate = [&](const RGBA &color) {
    const float f = 0.6f;
    const float a = coverage;
    float straight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (a > 0.0f) {
      for (const int k : IndexRange(4)) {
        straight[k] = premul[k] / a;
      }
    }
    RGBA blended = {straight[0], straight[1], straight[2], straight[3]};
    blend_over_rgba(blended, color, MA_RAMP_BLEND, 1.0f, normal_channel);
    const float child[4] = {color.r, color.g, color.b, color.a};
    const float blended_rgba[4] = {blended.r, blended.g, blended.b, blended.a};
    float c_eff[4];
    for (const int k : IndexRange(4)) {
      c_eff[k] = child[k] + (blended_rgba[k] - child[k]) * a;
    }
    for (const int k : IndexRange(4)) {
      premul[k] = premul[k] * (1.0f - f) + c_eff[k] * f;
    }
    coverage = a + f * (1.0f - a);
  };

  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    accumulate({kIsolatingPaintA[0], kIsolatingPaintA[1], kIsolatingPaintA[2], 1.0f});
    accumulate({kIsolatingPaintB[0], kIsolatingPaintB[1], kIsolatingPaintB[2], 1.0f});
  }

  RGBA folder_src = {0.0f, 0.0f, 0.0f, 0.0f};
  if (coverage > 0.0f) {
    folder_src = {premul[0] / coverage,
                  premul[1] / coverage,
                  premul[2] / coverage,
                  premul[3] / coverage};
  }
  blend_over_rgba(state, folder_src, MA_RAMP_BLEND, 0.5f * coverage, normal_channel);
  return state;
}

/**
 * An isolating folder with two Paint children of opacity below one: the generated chain, the CPU
 * composite and the reference must agree. This is the narrow case the premultiplied over's
 * `(1 - A * op)` needs; the full stack that would also cover it is blocked by the generator crash
 * the report describes.
 */
TEST_F(PaintLayersGraphEvalTest, isolating_folder_partial_coverage_matches_the_cpu)
{
  const int size = 2;
  const int points[3][2] = {{1, 1}, {1, 0}, {0, 1}};
  const char *point_names[3] = {"center", "edge", "corner"};
  const float tolerance = 1e-4f;

  ma = BKE_material_add(bmain, "IsolatingFolder");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  auto add_paint_child = [&](const char *name, const float color[4]) -> MaterialPaintLayer * {
    MaterialPaintLayer *child = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, folder, PaintLayerPlace::Into);
    EXPECT_NE(child, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    Image *map = make_bake_data_map(bmain, name, size, color);
    EXPECT_NE(map, nullptr);
    record->image = map;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    BKE_paint_layers_set_opacity(*ma, child, 0.6f);
    return child;
  };
  MaterialPaintLayer *child_a = add_paint_child("IsolatingPaintA", kIsolatingPaintA);
  MaterialPaintLayer *child_b = add_paint_child("IsolatingPaintB", kIsolatingPaintB);
  ASSERT_NE(child_a, nullptr);
  ASSERT_NE(child_b, nullptr);

  struct ActiveCase {
    const char *label;
    const bUUID marker;
  };
  const ActiveCase active_cases[] = {
      {"none", BLI_uuid_nil()},
      {"folder", folder->marker},
      {"paint-a", child_a->marker},
      {"paint-b", child_b->marker},
  };

  for (const ActiveCase &active_case : active_cases) {
    BKE_paint_layers_active_set(*ma, active_case.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    ASSERT_NE(ma->paint_layers_tree, nullptr);

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      if (!result_output_exists(*ma->paint_layers_tree, channel)) {
        continue;
      }
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        const int x = points[i][0];
        const int y = points[i][1];
        const RGBA expected = isolating_folder_expected(channel);
        interpreter.x = x;
        interpreter.y = y;
        const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
        const RGBA cpu = cpu_pixel_at(channel, x, y);
        EXPECT_NEAR(graph.r, expected.r, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " graph";
        EXPECT_NEAR(graph.g, expected.g, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " graph";
        EXPECT_NEAR(graph.b, expected.b, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " graph";
        EXPECT_NEAR(cpu.r, expected.r, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " cpu";
        EXPECT_NEAR(cpu.g, expected.g, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " cpu";
        EXPECT_NEAR(cpu.b, expected.b, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " cpu";
      }
    }
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Diagnostic parity for two nested isolating folders with partial alpha (design §11).
 *
 * Semantics verified in `paint_material_composite.cc` before writing the formula: a byte
 * paint map is stored straight, the GPU upload pre-multiplies it (`IMA_GPU_LINEAR_PREMUL`)
 * and the Image Texture node un-premultiplies a non-data map because the chain also reads
 * Alpha, while the CPU reads the straight bytes; both sides therefore see the straight color
 * `C` and coverage `A = byte_alpha / 255`. A folder accumulates its children premultiplied
 * (`composite_folder_accumulate`, `:702-775`): `P = 0`, `a = 0`, per child
 * `f = opacity * factor`, `S = P / a` (`0` when `a` is `0`), `blended = blend(S, c)` at full
 * factor, `c_eff = c + (blended - c) * a`, `P = P * (1 - f) + c_eff * f`,
 * `a = a + f * (1 - a)`; the straight result is `S_folder = P / a` with coverage `a`. The
 * generator builds the same chain (`paint_layers_generate.cc`, isolated accumulation with
 * `S = P / a`, `c_eff = lerp(c, blend, a)`, `P = lerp(P, c_eff, f)`, `a = a + f * (1 - a)`).
 * The folder's own row then lays `S_folder` over what is below with `opacity * coverage`
 * (`composite_apply_layer_linear`, `:777-804`). Opacity below one isolates
 * (`paint_layers.cc:356-370`), so both folders here are structurally isolating and no
 * mask/effect is used: the test isolates exactly folder accumulation and coverage.
 *
 * The reference below is computed only from the quantized input alphas, the pure straight
 * colors (whose sRGB to linear is identity) and the two folder opacities; it never reads
 * the graph or CPU outputs.
 */
TEST_F(PaintLayersGraphEvalTest, nested_isolating_folders_partial_alpha_matches_cpu_and_formula)
{
  const int size = 4;
  const float outer_opacity = 0.75f;
  const float inner_opacity = 0.5f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "NestedIsoAlpha");
  ASSERT_NE(ma, nullptr);

  /* Opaque contrasting bottom: red. */
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("NestedBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  /* Outer isolating folder. */
  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, outer_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *outer));

  /* Inner isolating folder inside the outer one. */
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
  ASSERT_NE(inner, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, inner_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *inner));

  /* Two leaves with straight-alpha soft edges: green below, blue on top. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *leaf_a_image = BKE_image_add_generated(
      bmain, size, size, "NestedLeafA", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_a_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_a_image, size, 0, 255, 0);
  MaterialPaintLayer *leaf_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "LeafA", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf_a, nullptr);
  MaterialPaintLayerChannel *record_a = BKE_paint_layers_channel_add(*ma, leaf_a, channel);
  ASSERT_NE(record_a, nullptr);
  record_a->image = leaf_a_image;
  record_a->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Image *leaf_b_image = BKE_image_add_generated(
      bmain, size, size, "NestedLeafB", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_b_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_b_image, size, 0, 0, 255);
  MaterialPaintLayer *leaf_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "LeafB", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf_b, nullptr);
  MaterialPaintLayerChannel *record_b = BKE_paint_layers_channel_add(*ma, leaf_b, channel);
  ASSERT_NE(record_b, nullptr);
  record_b->image = leaf_b_image;
  record_b->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  /* Folder groups and their instances, to read the Coverage sockets directly. */
  bNodeTree *outer_tree = nested_folder_tree_find(*bmain, "Outer");
  bNodeTree *inner_tree = nested_folder_tree_find(*bmain, "Inner");
  ASSERT_NE(outer_tree, nullptr);
  ASSERT_NE(inner_tree, nullptr);
  bNode *outer_instance = nested_group_instance_find(*ma->paint_layers_tree, *outer_tree);
  ASSERT_NE(outer_instance, nullptr);
  bNode *inner_instance = nested_group_instance_find(*outer_tree, *inner_tree);
  ASSERT_NE(inner_instance, nullptr);

  /* CPU coverage of each folder's own row, the same values the bake writes. */
  Vector<float> outer_color(int64_t(size) * size * 4);
  Vector<float> outer_coverage(int64_t(size) * size);
  Vector<float> inner_color(int64_t(size) * size * 4);
  Vector<float> inner_coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *outer, channel, size, outer_color.data(), outer_coverage.data()));
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *inner, channel, size, inner_color.data(), inner_coverage.data()));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  outer_tree->ensure_topology_cache();
  inner_tree->ensure_topology_cache();

  for (int x = 0; x < size; x++) {
    const int y = 0;
    /* Independent inputs: the quantized stored alphas and the pure straight colors. */
    const float alpha_ideal = 0.25f * float(x + 1);
    const float alpha_q = float(uchar(clamp_f(alpha_ideal, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
    const float leaf_a_alpha = alpha_q;
    const float leaf_b_alpha = alpha_q;
    const float leaf_a_color[3] = {0.0f, 1.0f, 0.0f};
    const float leaf_b_color[3] = {0.0f, 0.0f, 1.0f};
    const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
    const float f1 = leaf_a_alpha;
    const float f2 = leaf_b_alpha;
    const float a1 = f1;
    const float premul1[3] = {leaf_a_color[0] * f1, leaf_a_color[1] * f1, leaf_a_color[2] * f1};
    const float premul1_a = leaf_a_alpha * f1;
    const float a2 = a1 + f2 * (1.0f - a1);
    float straight[3] = {0.0f, 0.0f, 0.0f};
    float straight_a = 0.0f;
    if (a2 > 0.0f) {
      const float premul2[3] = {premul1[0] * (1.0f - f2) + leaf_b_color[0] * f2,
                                premul1[1] * (1.0f - f2) + leaf_b_color[1] * f2,
                                premul1[2] * (1.0f - f2) + leaf_b_color[2] * f2};
      const float premul2_a = premul1_a * (1.0f - f2) + leaf_b_alpha * f2;
      straight[0] = premul2[0] / a2;
      straight[1] = premul2[1] / a2;
      straight[2] = premul2[2] / a2;
      straight_a = premul2_a / a2;
    }
    const float inner_cov_expected = inner_opacity * a2;
    const float outer_cov_expected = outer_opacity * inner_opacity * a2;
    const float factor = outer_cov_expected;
    RGBA expected;
    expected.r = bottom_color[0] * (1.0f - factor) + straight[0] * factor;
    expected.g = bottom_color[1] * (1.0f - factor) + straight[1] * factor;
    expected.b = bottom_color[2] * (1.0f - factor) + straight[2] * factor;
    expected.a = 1.0f * (1.0f - factor) + straight_a * factor;

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);

    GraphInterpreter outer_interpreter;
    outer_interpreter.instance = outer_instance;
    outer_interpreter.tree = outer_tree;
    outer_interpreter.x = x;
    outer_interpreter.y = y;
    outer_interpreter.parent = &root_interpreter;
    const float graph_outer_cov = nested_folder_coverage_eval(
        root_interpreter, *outer_tree, *outer_instance);
    const float graph_inner_cov = nested_folder_coverage_eval(
        outer_interpreter, *inner_tree, *inner_instance);
    const float cpu_outer_cov = outer_coverage[int64_t(y) * size + x];
    const float cpu_inner_cov = inner_coverage[int64_t(y) * size + x];

    EXPECT_NEAR(graph.r, expected.r, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph.g, expected.g, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph.b, expected.b, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph.a, expected.a, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.r, expected.r, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.g, expected.g, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.b, expected.b, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.a, expected.a, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph_outer_cov, outer_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " outer coverage formula=" << outer_cov_expected
        << " graph=" << graph_outer_cov << " cpu=" << cpu_outer_cov;
    EXPECT_NEAR(cpu_outer_cov, outer_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " outer coverage formula=" << outer_cov_expected
        << " graph=" << graph_outer_cov << " cpu=" << cpu_outer_cov;
    EXPECT_NEAR(graph_inner_cov, inner_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " inner coverage formula=" << inner_cov_expected
        << " graph=" << graph_inner_cov << " cpu=" << cpu_inner_cov;
    EXPECT_NEAR(cpu_inner_cov, inner_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " inner coverage formula=" << inner_cov_expected
        << " graph=" << graph_inner_cov << " cpu=" << cpu_inner_cov;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C1: one isolating folder over an opaque bottom, holding two soft-edge Paint maps with partial
 * alpha (green below, blue above). The map's content alpha must reach the Result on \a channel's
 * machine, not only Base Color's. Checks graph, CPU and the closed-form isolated accumulation on
 * every component, plus the folder's Coverage and its `Content Alpha <channel>` output. Frees `ma`.
 */
void PaintLayersGraphEvalTest::check_iso_folder_two_map_channel(
    const eMaterialPaintChannel channel,
    const char *material_name,
    const char *folder_name,
    const uchar bottom_r,
    const uchar bottom_g,
    const uchar bottom_b,
    const float folder_opacity)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);

  ma = BKE_material_add(bmain, material_name);
  ASSERT_NE(ma, nullptr) << info.ui_name;

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("IsoBottom", size, bottom_r, bottom_g, bottom_b, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, folder_name, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  auto add_soft_edge_map = [&](const char *name, const uchar r, const uchar g, const uchar b) {
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
    EXPECT_NE(image, nullptr);
    fill_nested_leaf_soft_edge(image, size, r, g, b);
    MaterialPaintLayer *leaf = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_IMAGE, name, folder, PaintLayerPlace::Into);
    EXPECT_NE(leaf, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, leaf, channel);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  };
  add_soft_edge_map("IsoLeafA", 0, 255, 0);
  add_soft_edge_map("IsoLeafB", 0, 0, 255);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, folder_name);
  ASSERT_NE(folder_tree, nullptr) << info.ui_name;
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr) << info.ui_name;

  char content_alpha_name[64];
  char coverage_name[64];
  BLI_snprintf(content_alpha_name, sizeof(content_alpha_name), "Content Alpha %s", info.ui_name);
  BLI_snprintf(coverage_name, sizeof(coverage_name), "Coverage %s", info.ui_name);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, content_alpha_name)) << info.ui_name;

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float leaf_a_color[3] = {0.0f, 1.0f, 0.0f};
  const float leaf_b_color[3] = {0.0f, 0.0f, 1.0f};
  const float bottom_color[3] = {float(bottom_r) / 255.0f,
                                 float(bottom_g) / 255.0f,
                                 float(bottom_b) / 255.0f};

  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float f1 = soft_edge_alpha_q(x);
    const float f2 = soft_edge_alpha_q(x);
    const float a1 = f1;
    const float premul1[3] = {leaf_a_color[0] * f1, leaf_a_color[1] * f1, leaf_a_color[2] * f1};
    const float premul1_a = f1 * f1;
    const float a2 = a1 + f2 * (1.0f - a1);
    float straight[3] = {0.0f, 0.0f, 0.0f};
    float straight_a = 0.0f;
    if (a2 > 0.0f) {
      const float premul2[3] = {premul1[0] * (1.0f - f2) + leaf_b_color[0] * f2,
                                premul1[1] * (1.0f - f2) + leaf_b_color[1] * f2,
                                premul1[2] * (1.0f - f2) + leaf_b_color[2] * f2};
      const float premul2_a = premul1_a * (1.0f - f2) + f2 * f2;
      straight[0] = premul2[0] / a2;
      straight[1] = premul2[1] / a2;
      straight[2] = premul2[2] / a2;
      straight_a = premul2_a / a2;
    }
    const float factor = folder_opacity * a2;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + straight[0] * factor,
                           bottom_color[1] * (1.0f - factor) + straight[1] * factor,
                           bottom_color[2] * (1.0f - factor) + straight[2] * factor,
                           1.0f * (1.0f - factor) + straight_a * factor};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, coverage_name);
    const float graph_content_a = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, content_alpha_name);

    EXPECT_NEAR(graph.r, expected.r, tolerance) << info.ui_name << " x=" << x << " graph";
    EXPECT_NEAR(graph.g, expected.g, tolerance) << info.ui_name << " x=" << x << " graph";
    EXPECT_NEAR(graph.b, expected.b, tolerance) << info.ui_name << " x=" << x << " graph";
    EXPECT_NEAR(graph.a, expected.a, tolerance) << info.ui_name << " x=" << x << " graph";
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << info.ui_name << " x=" << x << " cpu";
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << info.ui_name << " x=" << x << " cpu";
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << info.ui_name << " x=" << x << " cpu";
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << info.ui_name << " x=" << x << " cpu";
    EXPECT_NEAR(graph_cov, folder_opacity * a2, tolerance) << info.ui_name << " x=" << x << " cov";
    EXPECT_NEAR(graph_content_a, straight_a, tolerance)
        << info.ui_name << " x=" << x << " content alpha";
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C1: content alpha is channel-agnostic. The same isolate-and-accumulate chain as the F2-B Base
 * Color case must hold for a scalar channel (Specular), where the bug showed identically.
 */
TEST_F(PaintLayersGraphEvalTest, single_isolating_folder_partial_alpha_matches_cpu_specular)
{
  check_iso_folder_two_map_channel(PAINT_MATERIAL_CHANNEL_SPECULAR,
                                   "IsoSpecular",
                                   "IsoSpec",
                                   255,
                                   255,
                                   255,
                                   0.5f);
}

/**
 * F2-C1: the color Emission path gets the same content alpha chain as Base Color, with its own
 * bottom constant and blend.
 */
TEST_F(PaintLayersGraphEvalTest, single_isolating_folder_partial_alpha_matches_cpu_emission)
{
  check_iso_folder_two_map_channel(PAINT_MATERIAL_CHANNEL_EMISSION,
                                   "IsoEmission",
                                   "IsoEmis",
                                   255,
                                   0,
                                   0,
                                   0.5f);
}

/**
 * F2-C1: Alpha, the channel whose own value is opacity, still tracks its content alpha without
 * doubling the stack coverage: the folder's `Content Alpha Alpha` output is the isolated straight
 * alpha, exactly as the CPU accumulates it.
 */
TEST_F(PaintLayersGraphEvalTest, single_isolating_folder_partial_alpha_matches_cpu_alpha_channel)
{
  check_iso_folder_two_map_channel(PAINT_MATERIAL_CHANNEL_ALPHA,
                                   "IsoAlphaChannel",
                                   "IsoAlphaCh",
                                   255,
                                   255,
                                   255,
                                   0.5f);
}

/**
 * F2-B: one isolating folder over opaque red, holding a soft-edge Paint map below and a Paint
 * constant (blue, alpha 0.6) above. The map exercises the Image Texture Alpha leaf path, the
 * constant the Value leaf path; graph, CPU and the closed-form reference must agree on all four
 * components. The constant covers fully, so the folder ends fully covered with its alpha.
 */
TEST_F(PaintLayersGraphEvalTest, single_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const float const_alpha = 0.6f;

  ma = BKE_material_add(bmain, "SingleIsoAlpha");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("SingleBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Single", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *leaf_image = BKE_image_add_generated(
      bmain, size, size, "SingleLeaf", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_image, size, 0, 255, 0);
  MaterialPaintLayer *leaf_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "LeafA", folder, PaintLayerPlace::Into);
  ASSERT_NE(leaf_a, nullptr);
  MaterialPaintLayerChannel *record_a = BKE_paint_layers_channel_add(*ma, leaf_a, channel);
  ASSERT_NE(record_a, nullptr);
  record_a->image = leaf_image;
  record_a->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *leaf_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "LeafB", folder, PaintLayerPlace::Into);
  ASSERT_NE(leaf_b, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, leaf_b, channel), nullptr);
  const float const_blue[4] = {0.0f, 0.0f, 1.0f, const_alpha};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(*ma, leaf_b, channel, const_blue));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "Single");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  Vector<float> folder_color(int64_t(size) * size * 4);
  Vector<float> folder_coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *folder, channel, size, folder_color.data(), folder_coverage.data()));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float alpha_ideal = 0.25f * float(x + 1);
    const float alpha_q = float(uchar(clamp_f(alpha_ideal, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
    const float f1 = alpha_q;
    const float f2 = 1.0f;
    const float a1 = f1;
    const float a2 = a1 + f2 * (1.0f - a1);
    const float premul1_a = alpha_q * f1;
    const float straight_a = (premul1_a * (1.0f - f2) + const_alpha * f2) / a2;
    const float factor = folder_opacity * a2;
    RGBA expected;
    expected.r = 1.0f * (1.0f - factor) + 0.0f * factor;
    expected.g = 0.0f * (1.0f - factor) + 0.0f * factor;
    expected.b = 0.0f * (1.0f - factor) + 1.0f * factor;
    expected.a = 1.0f * (1.0f - factor) + straight_a * factor;

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_coverage_eval(
        root_interpreter, *folder_tree, *folder_instance);
    const float graph_content_a = nested_folder_content_alpha_eval(
        root_interpreter, *folder_tree, *folder_instance);
    const float cpu_cov = folder_coverage[int64_t(y) * size + x];

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, folder_opacity * a2, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu_cov, folder_opacity * a2, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_content_a, straight_a, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-B: three nested isolating folders with the partial alpha on the innermost leaves. A single
 * child passes its straight result through unchanged, so every level shares the same straight
 * colour and alpha while the coverages multiply; the fix must hold at every depth, not just one.
 */
TEST_F(PaintLayersGraphEvalTest, three_nested_isolating_folders_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float op1 = 0.6f;
  const float op2 = 0.7f;
  const float op3 = 0.8f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "TripleIsoAlpha");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("TripleBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, op1));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *outer));
  MaterialPaintLayer *middle = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Middle", outer, PaintLayerPlace::Into);
  ASSERT_NE(middle, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, middle, op2));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *middle));
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", middle, PaintLayerPlace::Into);
  ASSERT_NE(inner, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, op3));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *inner));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *leaf_a_image = BKE_image_add_generated(
      bmain, size, size, "TripleLeafA", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_a_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_a_image, size, 0, 255, 0);
  MaterialPaintLayer *leaf_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "LeafA", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf_a, nullptr);
  MaterialPaintLayerChannel *record_a = BKE_paint_layers_channel_add(*ma, leaf_a, channel);
  ASSERT_NE(record_a, nullptr);
  record_a->image = leaf_a_image;
  record_a->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Image *leaf_b_image = BKE_image_add_generated(
      bmain, size, size, "TripleLeafB", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_b_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_b_image, size, 0, 0, 255);
  MaterialPaintLayer *leaf_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "LeafB", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf_b, nullptr);
  MaterialPaintLayerChannel *record_b = BKE_paint_layers_channel_add(*ma, leaf_b, channel);
  ASSERT_NE(record_b, nullptr);
  record_b->image = leaf_b_image;
  record_b->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *outer_tree = nested_folder_tree_find(*bmain, "Outer");
  bNodeTree *middle_tree = nested_folder_tree_find(*bmain, "Middle");
  bNodeTree *inner_tree = nested_folder_tree_find(*bmain, "Inner");
  ASSERT_NE(outer_tree, nullptr);
  ASSERT_NE(middle_tree, nullptr);
  ASSERT_NE(inner_tree, nullptr);
  bNode *outer_instance = nested_group_instance_find(*ma->paint_layers_tree, *outer_tree);
  bNode *middle_instance = nested_group_instance_find(*outer_tree, *middle_tree);
  bNode *inner_instance = nested_group_instance_find(*middle_tree, *inner_tree);
  ASSERT_NE(outer_instance, nullptr);
  ASSERT_NE(middle_instance, nullptr);
  ASSERT_NE(inner_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*inner_tree, "Content Alpha Base Color"));

  Vector<float> inner_color(int64_t(size) * size * 4);
  Vector<float> inner_coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *inner, channel, size, inner_color.data(), inner_coverage.data()));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  outer_tree->ensure_topology_cache();
  middle_tree->ensure_topology_cache();
  inner_tree->ensure_topology_cache();

  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float alpha_ideal = 0.25f * float(x + 1);
    const float alpha_q = float(uchar(clamp_f(alpha_ideal, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
    const float leaf_a_alpha = alpha_q;
    const float leaf_b_alpha = alpha_q;
    const float leaf_a_color[3] = {0.0f, 1.0f, 0.0f};
    const float leaf_b_color[3] = {0.0f, 0.0f, 1.0f};
    const float f1 = leaf_a_alpha;
    const float f2 = leaf_b_alpha;
    const float a1 = f1;
    const float premul1_a = leaf_a_alpha * f1;
    const float a_inner = a1 + f2 * (1.0f - a1);
    float straight[3] = {0.0f, 0.0f, 0.0f};
    float straight_a = 0.0f;
    if (a_inner > 0.0f) {
      const float premul2[3] = {leaf_a_color[0] * f1 * (1.0f - f2) + leaf_b_color[0] * f2,
                                leaf_a_color[1] * f1 * (1.0f - f2) + leaf_b_color[1] * f2,
                                leaf_a_color[2] * f1 * (1.0f - f2) + leaf_b_color[2] * f2};
      const float premul2_a = premul1_a * (1.0f - f2) + leaf_b_alpha * f2;
      straight[0] = premul2[0] / a_inner;
      straight[1] = premul2[1] / a_inner;
      straight[2] = premul2[2] / a_inner;
      straight_a = premul2_a / a_inner;
    }
    const float cov_inner = op3 * a_inner;
    const float cov_middle = op2 * cov_inner;
    const float cov_outer = op1 * cov_middle;
    const float factor = cov_outer;
    RGBA expected;
    expected.r = 1.0f * (1.0f - factor) + straight[0] * factor;
    expected.g = 0.0f * (1.0f - factor) + straight[1] * factor;
    expected.b = 0.0f * (1.0f - factor) + straight[2] * factor;
    expected.a = 1.0f * (1.0f - factor) + straight_a * factor;

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);

    GraphInterpreter outer_interpreter;
    outer_interpreter.instance = outer_instance;
    outer_interpreter.tree = outer_tree;
    outer_interpreter.x = x;
    outer_interpreter.y = y;
    outer_interpreter.parent = &root_interpreter;
    GraphInterpreter middle_interpreter;
    middle_interpreter.instance = middle_instance;
    middle_interpreter.tree = middle_tree;
    middle_interpreter.x = x;
    middle_interpreter.y = y;
    middle_interpreter.parent = &outer_interpreter;
    const float graph_outer_cov = nested_folder_coverage_eval(
        root_interpreter, *outer_tree, *outer_instance);
    const float graph_middle_cov = nested_folder_coverage_eval(
        outer_interpreter, *middle_tree, *middle_instance);
    const float graph_inner_cov = nested_folder_coverage_eval(
        middle_interpreter, *inner_tree, *inner_instance);
    const float graph_inner_a = nested_folder_content_alpha_eval(
        middle_interpreter, *inner_tree, *inner_instance);
    const float cpu_inner_cov = inner_coverage[int64_t(y) * size + x];

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_outer_cov, cov_outer, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_middle_cov, cov_middle, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_inner_cov, cov_inner, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu_inner_cov, cov_inner, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_inner_a, straight_a, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-B: an isolating folder whose leaf is fully transparent (zero coverage) next to an empty
 * isolating folder. Both divides see a zero divisor, so the whole path -- including the new
 * content-alpha Math divide -- must stay finite, and the opaque bottom must show through
 * unchanged on both sides.
 */
TEST_F(PaintLayersGraphEvalTest, zero_coverage_isolating_folder_stays_finite)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "ZeroCoverage");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("ZeroBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *zero = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Zero", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(zero, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, zero, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *zero));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *leaf_image = BKE_image_add_generated(
      bmain, size, size, "ZeroLeaf", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_image, nullptr);
  leaf_image->alpha_mode = IMA_ALPHA_STRAIGHT;
  leaf_image->flag |= IMA_GPU_LINEAR_PREMUL;
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Leaf", zero, PaintLayerPlace::Into);
  ASSERT_NE(leaf, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, leaf, channel);
  ASSERT_NE(record, nullptr);
  record->image = leaf_image;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *empty = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Empty", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(empty, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, empty, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *zero_tree = nested_folder_tree_find(*bmain, "Zero");
  ASSERT_NE(zero_tree, nullptr);
  bNode *zero_instance = nested_group_instance_find(*ma->paint_layers_tree, *zero_tree);
  ASSERT_NE(zero_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*zero_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  zero_tree->ensure_topology_cache();

  for (int x = 0; x < size; x++) {
    const int y = 0;
    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_coverage_eval(
        root_interpreter, *zero_tree, *zero_instance);
    const float graph_content_a = nested_folder_content_alpha_eval(
        root_interpreter, *zero_tree, *zero_instance);

    EXPECT_TRUE(std::isfinite(graph.r));
    EXPECT_TRUE(std::isfinite(graph.g));
    EXPECT_TRUE(std::isfinite(graph.b));
    EXPECT_TRUE(std::isfinite(graph.a));
    EXPECT_TRUE(std::isfinite(graph_cov));
    EXPECT_TRUE(std::isfinite(graph_content_a));
    EXPECT_NEAR(graph.r, 1.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, 0.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, 0.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, 1.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, 1.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, 0.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, 0.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, 1.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, 0.0f, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_content_a, 0.0f, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C1: the isolating folder on Roughness was the known gap in F2-B -- `graph.a` diverged from
 * `cpu.a` because the folder's Vector Math divide dropped the colour's fourth component. Content
 * alpha now tracks this scalar channel like any map channel, so graph, CPU and the closed-form
 * isolated accumulation agree on all four components.
 */
TEST_F(PaintLayersGraphEvalTest, roughness_isolating_folder_partial_alpha_matches_cpu)
{
  check_iso_folder_two_map_channel(PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                   "RoughIso",
                                   "RoughFolder",
                                   255,
                                   255,
                                   255,
                                   0.5f);
}

/**
 * F2-C1: a Fill leaf supplies its content alpha the same way a Paint constant does -- the constant's
 * `.a` is frozen into a Value before the isolating divide, instead of the Vector Math divide cutting
 * the Result's fourth component to zero. The Fill fully covers, so the folder's Coverage is
 * its opacity and its Content Alpha is the fill alpha.
 */
TEST_F(PaintLayersGraphEvalTest, fill_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float fill_alpha = 0.6f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "FillIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("FillBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "FillFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, channel), nullptr);
  const float fill_color[4] = {0.0f, 0.0f, 1.0f, fill_alpha};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, fill_color));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "FillFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float factor = folder_opacity;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + fill_color[0] * factor,
                           bottom_color[1] * (1.0f - factor) + fill_color[1] * factor,
                           bottom_color[2] * (1.0f - factor) + fill_color[2] * factor,
                           1.0f * (1.0f - factor) + fill_alpha * factor};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Coverage Base Color");
    const float graph_content_a = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Content Alpha Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, folder_opacity, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_content_a, fill_alpha, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Fill whose Base Color is switched off -- or whose Base Color record is removed entirely -- takes
 * part nowhere in that channel: the graph and the CPU composite both show the stack below the Fill
 * unchanged, and the Fill's remaining channels keep taking part (Roughness stays wired).
 */
TEST_F(PaintLayersGraphEvalTest, fill_without_base_color_matches_cpu)
{
  const int size = 4;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "FillNoBase");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("FillNoBaseBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  const float fill_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, fill_color));

  const RGBA expected = {1.0f, 0.0f, 0.0f, 1.0f};
  auto graph_and_cpu_show_the_bottom = [&](const char *what) {
    GraphInterpreter root_interpreter;
    root_interpreter.instance = find_instance();
    root_interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(root_interpreter.instance, nullptr) << what;
    root_interpreter.tree->ensure_topology_cache();
    for (int x = 0; x < size; x++) {
      root_interpreter.x = x;
      root_interpreter.y = 0;
      const RGBA graph = eval_channel_result(root_interpreter, channel);
      const RGBA cpu = cpu_pixel_at(channel, x, 0);
      EXPECT_NEAR(graph.r, expected.r, tolerance) << what << " x=" << x;
      EXPECT_NEAR(graph.g, expected.g, tolerance) << what << " x=" << x;
      EXPECT_NEAR(graph.b, expected.b, tolerance) << what << " x=" << x;
      EXPECT_NEAR(graph.a, expected.a, tolerance) << what << " x=" << x;
      EXPECT_NEAR(cpu.r, expected.r, tolerance) << what << " x=" << x;
      EXPECT_NEAR(cpu.g, expected.g, tolerance) << what << " x=" << x;
      EXPECT_NEAR(cpu.b, expected.b, tolerance) << what << " x=" << x;
      EXPECT_NEAR(cpu.a, expected.a, tolerance) << what << " x=" << x;
    }
  };

  /* The switched-off Base Color: the Fill paints nothing there, while its Roughness record keeps
   * taking part, so Roughness stays wired. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
  ASSERT_NE(principled, nullptr);
  bNodeSocket *roughness = bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  EXPECT_FALSE(roughness->directly_linked_links().is_empty());
  graph_and_cpu_show_the_bottom("disabled");

  /* The removed Base Color: the same answer, through both evaluators. */
  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  graph_and_cpu_show_the_bottom("removed");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C1: a Paint map and a Fill constant inside the same isolating folder. The map's content alpha
 * starts the chain and the Fill's partial opacity lays the constant over the accumulated alpha, so
 * graph, CPU and the closed-form accumulation agree on every component, not just RGB.
 */
TEST_F(PaintLayersGraphEvalTest, mixed_paint_and_fill_isolating_folder_partial_alpha_matches_cpu)
{
  const int size = 4;
  const float folder_opacity = 0.5f;
  const float fill_opacity = 0.5f;
  const float fill_alpha = 0.6f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "MixedIso");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("MixedBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "MixedFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, folder_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *leaf_image = BKE_image_add_generated(
      bmain, size, size, "MixedLeaf", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_image, size, 0, 255, 0);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "MixedLeaf", folder, PaintLayerPlace::Into);
  ASSERT_NE(leaf, nullptr);
  MaterialPaintLayerChannel *leaf_record = BKE_paint_layers_channel_add(*ma, leaf, channel);
  ASSERT_NE(leaf_record, nullptr);
  leaf_record->image = leaf_image;
  leaf_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "MixedFill", folder, PaintLayerPlace::Into);
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, channel), nullptr);
  const float fill_color[4] = {0.0f, 0.0f, 1.0f, fill_alpha};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, fill_color));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, fill, fill_opacity));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "MixedFolder");
  ASSERT_NE(folder_tree, nullptr);
  bNode *folder_instance = nested_group_instance_find(*ma->paint_layers_tree, *folder_tree);
  ASSERT_NE(folder_instance, nullptr);
  EXPECT_TRUE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  folder_tree->ensure_topology_cache();

  const float leaf_color[3] = {0.0f, 1.0f, 0.0f};
  const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
  for (int x = 0; x < size; x++) {
    const int y = 0;
    const float f1 = soft_edge_alpha_q(x);
    const float coverage = f1 + fill_opacity * (1.0f - f1);
    float straight[3] = {0.0f, 0.0f, 0.0f};
    float straight_a = 0.0f;
    if (coverage > 0.0f) {
      for (const int k : IndexRange(3)) {
        straight[k] = (leaf_color[k] * f1 * (1.0f - fill_opacity) + fill_color[k] * fill_opacity) /
                      coverage;
      }
      straight_a = (f1 * f1 * (1.0f - fill_opacity) + fill_alpha * fill_opacity) / coverage;
    }
    const float factor = folder_opacity * coverage;
    const RGBA expected = {bottom_color[0] * (1.0f - factor) + straight[0] * factor,
                           bottom_color[1] * (1.0f - factor) + straight[1] * factor,
                           bottom_color[2] * (1.0f - factor) + straight[2] * factor,
                           1.0f * (1.0f - factor) + straight_a * factor};

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);
    const float graph_cov = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Coverage Base Color");
    const float graph_content_a = nested_folder_named_output_eval(
        root_interpreter, *folder_tree, *folder_instance, "Content Alpha Base Color");

    EXPECT_NEAR(graph.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.r, expected.r, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.g, expected.g, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.b, expected.b, tolerance) << "x=" << x;
    EXPECT_NEAR(cpu.a, expected.a, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_cov, folder_opacity * coverage, tolerance) << "x=" << x;
    EXPECT_NEAR(graph_content_a, straight_a, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * F2-C1/F2-C4a regression guard: a Material leaf whose row reads through the SourceGroup wrapper
 * (its channels form a real graph, not a plain constant or a trivial map -- see the "A and C"
 * comment on #build_group_source) is outside the content-alpha scope even after F2-C4a: the
 * `source_group_instance` branch in the generator hands the row's colour and coverage to the
 * wrapper's own COLOR/COVERAGE outputs and never assigns `current.content_alpha` at all, so the
 * folder still carries no Content Alpha socket and the row is treated as opaque there. This is
 * deliberately not a Hybrid row: F2-C4a gave Hybrid's own live_constant/live_map rows a content
 * alpha (see the `material_hybrid_live_*` tests), so a Hybrid source would no longer prove the
 * gap this guard exists for.
 */
TEST_F(PaintLayersGraphEvalTest, untracked_material_child_inside_folder_adds_no_content_alpha)
{
  Material *source = build_group_source(
      *bmain, "NoContentSource", "NoContentGroup", "NoContentMap", "NoContentShared", source_spec_a);
  ASSERT_NE(source, nullptr);
  ma = BKE_material_add(bmain, "NoContentStack");
  ASSERT_NE(ma, nullptr);

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "NoContentFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayer *mat = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "NoContentMat", folder, PaintLayerPlace::Into);
  ASSERT_NE(mat, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *mat), PaintLayerMaterialMode::SourceGroup);

  bNodeTree *folder_tree = nested_folder_tree_find(*bmain, "NoContentFolder");
  ASSERT_NE(folder_tree, nullptr);
  EXPECT_FALSE(folder_interface_has_socket(*folder_tree, "Content Alpha Base Color"));
  EXPECT_FALSE(folder_interface_has_socket(*folder_tree, "Content Alpha Metallic"));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The generated root's node pointers, for the "a second rebuild changed nothing" check. */
/**
 * Stage 4: a Pass Through folder with a Material child must be byte-identical to moving the child
 * to the parent, hiding it must be a value edit (same result, same root nodes), and a second
 * regeneration after an active-row change must rebuild nothing.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_material_child_and_hiding_match_the_flat_stack)
{
  const int size = 2;
  const int points[3][2] = {{1, 1}, {1, 0}, {0, 1}};
  const float tolerance = 1e-4f;
  const float base_color[4] = {0.10f, 0.60f, 0.20f, 1.0f};

  Material *source_c = build_hybrid_source(
      *bmain, "PassThroughSourceC", "PassThroughNormalC", source_spec_b);
  ASSERT_NE(source_c, nullptr);

  /* \a folded puts Material C in a Pass Through folder; without it C sits on the parent. */
  auto build_material = [&](const char *suffix, const bool folded, MaterialPaintLayer **r_c) -> Material * {
    Material *material = BKE_material_add(bmain, suffix);
    MaterialPaintLayer *base = BKE_paint_layers_add(
        *material, MA_PAINT_LAYER_SOURCE_IMAGE, "Base", nullptr, PaintLayerPlace::Above);
    if (base == nullptr) {
      return nullptr;
    }
    MaterialPaintLayerChannel *base_record = BKE_paint_layers_channel_add(
        *material, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (base_record == nullptr) {
      return nullptr;
    }
    char name[96];
    BLI_snprintf(name, sizeof(name), "PassThroughBase%s", suffix);
    Image *base_map = make_bake_data_map(bmain, name, size, base_color);
    if (base_map == nullptr) {
      return nullptr;
    }
    base_record->image = base_map;
    base_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

    MaterialPaintLayer *anchor = nullptr;
    if (folded) {
      anchor = BKE_paint_layers_add(
          *material, MA_PAINT_LAYER_SOURCE_STACK, "Pass", nullptr, PaintLayerPlace::Above);
      if (anchor == nullptr || !BKE_paint_layers_folder_is_pass_through(*material, *anchor)) {
        return nullptr;
      }
    }
    MaterialPaintLayer *c = BKE_paint_layers_add(*material,
                                                 MA_PAINT_LAYER_SOURCE_MATERIAL,
                                                 "MatC",
                                                 anchor,
                                                 folded ? PaintLayerPlace::Into :
                                                          PaintLayerPlace::Above);
    if (c == nullptr || !BKE_paint_layers_set_material(*material, c, source_c)) {
      return nullptr;
    }
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      RGBA raw;
      if (!hybrid_source_expected(source_spec_b, int(channel), raw)) {
        continue;
      }
      const float rgba[4] = {raw.r, raw.g, raw.b, 1.0f};
      BLI_snprintf(name, sizeof(name), "PassThroughBake%s", suffix);
      Image *map = make_bake_data_map(bmain, name, size, rgba);
      if (map == nullptr || !BKE_paint_layers_bake_set_map(*material, *c, int(channel), map)) {
        return nullptr;
      }
    }
    const float coverage_rgba[4] = {
        source_spec_b.alpha, source_spec_b.alpha, source_spec_b.alpha, 1.0f};
    BLI_snprintf(name, sizeof(name), "PassThroughCoverage%s", suffix);
    Image *coverage_map = make_bake_data_map(bmain, name, size, coverage_rgba);
    if (coverage_map == nullptr ||
        !BKE_paint_layers_bake_set_map(*material, *c, -1, coverage_map))
    {
      return nullptr;
    }
    BKE_paint_layers_bake_finalize(*material, *c);
    *r_c = c;
    return material;
  };

  MaterialPaintLayer *folded_c = nullptr;
  MaterialPaintLayer *plain_c = nullptr;
  Material *folded = build_material("Folded", true, &folded_c);
  Material *plain = build_material("Plain", false, &plain_c);
  ASSERT_NE(folded, nullptr);
  ASSERT_NE(plain, nullptr);

  /* The generated channels of both stacks must hold the same values at every point. */
  auto evaluate = [&](Material *material) -> Vector<RGBA> {
    ma = material;
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
    EXPECT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    EXPECT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();
    Vector<RGBA> values;
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      if (!result_output_exists(*ma->paint_layers_tree, channel)) {
        continue;
      }
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        interpreter.x = points[i][0];
        interpreter.y = points[i][1];
        values.append(eval_channel_result(interpreter, eMaterialPaintChannel(channel)));
      }
    }
    return values;
  };
  const Vector<RGBA> folded_values = evaluate(folded);
  const Vector<RGBA> plain_values = evaluate(plain);
  ASSERT_EQ(folded_values.size(), plain_values.size());
  for (const int i : folded_values.index_range()) {
    EXPECT_NEAR(folded_values[i].r, plain_values[i].r, tolerance) << "index " << i;
    EXPECT_NEAR(folded_values[i].g, plain_values[i].g, tolerance) << "index " << i;
    EXPECT_NEAR(folded_values[i].b, plain_values[i].b, tolerance) << "index " << i;
  }

  /* An active-row change on the folded stack: the next regen rebuilds, the one after does not. */
  ma = folded;
  BKE_paint_layers_active_set(*ma, folded_c->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root)));

  /* Hiding C is a value edit: the result equals the base alone and the root keeps its nodes. */
  ma = plain;
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *plain_root = ma->paint_layers_tree;
  const Vector<bNode *> plain_nodes = root_node_ptrs(*plain_root);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, plain_c, false));
  BKE_paint_layers_values_sync(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, plain_root);
  EXPECT_TRUE(same_node_ptrs(plain_nodes, root_node_ptrs(*plain_root)));
  GraphInterpreter hidden_interpreter;
  hidden_interpreter.instance = find_instance();
  hidden_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(hidden_interpreter.instance, nullptr);
  hidden_interpreter.tree->ensure_topology_cache();
  hidden_interpreter.x = 1;
  hidden_interpreter.y = 1;
  const RGBA hidden = eval_channel_result(
      hidden_interpreter, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* The base map is fully covering, so hiding C leaves exactly the base colour. */
  EXPECT_NEAR(hidden.r, base_color[0], tolerance);
  EXPECT_NEAR(hidden.g, base_color[1], tolerance);
  EXPECT_NEAR(hidden.b, base_color[2], tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

}  // namespace blender::bke::tests
