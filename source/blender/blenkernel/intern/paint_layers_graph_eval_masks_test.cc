/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


/** \file
 * Paint Layers graph-vs-CPU tests: masks, corrections and topology (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_graph_eval_test_fixture.hh` /
 * `paint_layers_graph_eval_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"
#include "intern/paint_layers_graph_eval_test_shared.hh"

namespace blender::bke::tests {


TEST_F(PaintLayersGraphEvalTest, generated_tree_topology_matches_cpu_formulas)
{
  const int size = 4;

  struct Variant {
    const char *name;
    eMaterialPaintLayerSource source;
    eMaterialPaintLayerBlend blend;
    float opacity;
    /* 0 none, 1 mask const, 2 mask image, 3 content image, 4 content fill, 5 mask corr. */
    int extra;
    eMaterialPaintChannel channel;
  };
  const Variant variants[] = {
      {"mix", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mul", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MULTIPLY, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"overlay", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_OVERLAY, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"add", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_ADD, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"fill", MA_PAINT_LAYER_SOURCE_CONSTANT, MA_PAINT_LAYER_BLEND_MIX, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_const", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 1,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_image", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 2,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"content_image", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 3,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"content_fill", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 4,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_corr", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 5,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_map_two_corr", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 6,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"disabled_layer", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 7,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"disabled_content_corr", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 8,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"disabled_mask_corr", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 9,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"normal_mask_corr", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 5,
       PAINT_MATERIAL_CHANNEL_NORMAL},
      {"normal_content_image", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 3,
       PAINT_MATERIAL_CHANNEL_NORMAL},
      {"normal_content_fill_ignored", MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 4,
       PAINT_MATERIAL_CHANNEL_NORMAL},
  };

  for (const Variant &variant : variants) {
    ma = BKE_material_add(bmain, "GraphEval");
    add_layer("Bottom",
              MA_PAINT_LAYER_SOURCE_IMAGE,
              add_solid_image("Bottom", size, 255, 0, 0, 255),
              variant.channel);
    if (variant.extra == 7) {
      /* A disabled row stays in the topology with factor zero; it must not change the result. */
      MaterialPaintLayer *middle = add_layer(
          "Middle",
          MA_PAINT_LAYER_SOURCE_IMAGE,
          add_solid_image("Middle", size, 255, 255, 0, 255),
          variant.channel);
      BKE_paint_layers_set_enabled(*ma, middle, false);
    }
    MaterialPaintLayer *top = add_layer(
        "Top", variant.source, add_solid_image("Top", size, 0, 0, 255, 255), variant.channel);
    BKE_paint_layers_set_blend(*ma, top, variant.blend);
    BKE_paint_layers_set_opacity(*ma, top, variant.opacity);
    if (variant.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
      const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      BKE_paint_layers_set_fill_color(*ma, top, blue);
      /* A Fill has no map in the channel; the record is only for presence. */
      top->channels[0].image = nullptr;
    }
    switch (variant.extra) {
      case 1:
        BKE_paint_layers_mask_add(*ma, top, 0.5f);
        break;
      case 2: {
        set_mask_image(*top, add_solid_image("Mask", size, 128, 128, 128, 255));
        break;
      }
      case 3: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("Correction", size, 0, 255, 0, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        break;
      }
      case 4: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        BKE_paint_layers_set_fill_color(*ma, correction, green);
        break;
      }
      case 5: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("MaskCorr", size, 128, 128, 128, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        break;
      }
      case 8: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("C", size, 0, 255, 0, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_enabled(*ma, correction, false);
        break;
      }
      case 9: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("M", size, 128, 128, 128, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_enabled(*ma, correction, false);
        break;
      }
      case 6: {
        /* A mask map below two content corrections with different opacities is where the order of
         * the "over" accumulation shows: a wrong order still passes with a single correction. */
        set_mask_image(*top, add_solid_image("Mask", size, 128, 128, 128, 255));
        MaterialPaintLayer *first = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C1");
        MaterialPaintLayerChannel *first_record = BKE_paint_layers_channel_add(
            *ma, first, variant.channel);
        first_record->image = add_solid_image("C1", size, 0, 255, 0, 255);
        first_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_opacity(*ma, first, 0.3f);
        MaterialPaintLayer *second = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C2");
        MaterialPaintLayerChannel *second_record = BKE_paint_layers_channel_add(
            *ma, second, variant.channel);
        second_record->image = add_solid_image("C2", size, 255, 255, 0, 255);
        second_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_opacity(*ma, second, 0.7f);
        break;
      }
      default:
        break;
    }

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result(result_name(variant.channel));
    const RGBA cpu = cpu_pixel(variant.channel);

    /* Both sides decode their maps to scene linear and mix in float, so nothing quantizes between
     * rows any more: what is left is float rounding. */
    const float tolerance = 1e-4f;
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << variant.name;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << variant.name;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << variant.name;

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/**
 * Every blend mode the description offers on Base Color, with inputs chosen to reach the unusual
 * branches: values below and above half (Overlay, Soft Light, Linear Light, Burn, Dodge), a
 * saturated pair (Hue/Saturation/Color/Value), a result above one (Add) and a top that is zero
 * (Divide) or a bottom below the top (Subtract).
 *
 * The two sides decode the maps and mix through the same `ramp_blend`; an accidental result clamp
 * in the generated Mix would show up on `add_over_one`, and a wrong ramp code on any other row.
 */
TEST_F(PaintLayersGraphEvalTest, mask_item_multiply_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "MaskItemMultiply");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255));
  Image *mask_image = add_solid_image("Mask", size, 204, 204, 204, 255);
  make_image_data(mask_image);
  set_mask_image(*top, mask_image);

  /* A Paint item with a semi-transparent map, MULTIPLY. */
  MaterialPaintLayer *paint_item = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "P");
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, paint_item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  Image *item_image = add_solid_image("ItemMap", size, 64, 64, 64, 128);
  make_image_data(item_image);
  item_image->alpha_mode = IMA_ALPHA_STRAIGHT;
  item_image->flag |= IMA_GPU_LINEAR_PREMUL;
  record->image = item_image;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  BKE_paint_layers_set_blend(*ma, paint_item, MA_PAINT_LAYER_BLEND_MULTIPLY);

  /* A Fill item with a 0.5 constant, MULTIPLY. */
  MaterialPaintLayer *fill_item = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "F");
  const float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  BKE_paint_layers_set_fill_color(*ma, fill_item, half);
  BKE_paint_layers_set_blend(*ma, fill_item, MA_PAINT_LAYER_BLEND_MULTIPLY);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float tolerance = 1e-4f;
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A black Fill mask item over an unpainted (map-less) Image base mask must hide the row: the top
 * Image layer is blue, the bottom is red, so the result is red and both sides agree.
 */
TEST_F(PaintLayersGraphEvalTest, fill_mask_black_over_unpainted_image_mask_hides_the_row)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "FillMaskBlackHidesRow");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255));
  /* Base mask without a map: white coverage, so the row shows until the Fill zeroes it. */
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, top, 1.0f);
  ASSERT_NE(base, nullptr);
  /* Why Image without a map: the log case, where the builder skips a map-less base mask. */
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, base, MA_PAINT_LAYER_SOURCE_IMAGE));
  /* Black Fill mask item, default MIX: the row coverage becomes zero. */
  MaterialPaintLayer *fill = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill");
  ASSERT_NE(fill, nullptr);
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, black));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float tolerance = 1e-4f;
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);
  /* The blue top is hidden, the red bottom shows through. */
  EXPECT_NEAR(graph.r, 1.0f, tolerance);
  EXPECT_NEAR(graph.g, 0.0f, tolerance);
  EXPECT_NEAR(graph.b, 0.0f, tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same black-Fill-hides-the-row case over a map-less Image base mask, but the hidden row reads
 * a live Material source (a blue Principled constant) instead of an image: the masked row drops
 * out, the red bottom shows, and the graph agrees with the CPU.
 */
TEST_F(PaintLayersGraphEvalTest, fill_mask_black_hides_a_material_row)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "FillMaskBlackHidesMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "FillMaskBlueSource");
  ASSERT_NE(source, nullptr);
  ASSERT_NE(source->nodetree, nullptr);
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  ASSERT_NE(principled, nullptr);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  ASSERT_NE(output, nullptr);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);

  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Top", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(top, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, top, source));
  /* Base mask without a map: white coverage, so the row shows until the Fill zeroes it. */
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(*ma, top, 1.0f);
  ASSERT_NE(base, nullptr);
  /* Why Image without a map: the log case, where the builder skips a map-less base mask. */
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, base, MA_PAINT_LAYER_SOURCE_IMAGE));
  /* Black Fill mask item, default MIX: the row coverage becomes zero. */
  MaterialPaintLayer *fill = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill");
  ASSERT_NE(fill, nullptr);
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, black));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float tolerance = 1e-4f;
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);
  /* The blue Material row is hidden, the red bottom shows through. */
  EXPECT_NEAR(graph.r, 1.0f, tolerance);
  EXPECT_NEAR(graph.g, 0.0f, tolerance);
  EXPECT_NEAR(graph.b, 0.0f, tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A mask item whose map carries a soft edge, stored the one way every paint-layer map is: straight
 * byte RGB (grey 0.6) with alpha 0.25 / 0.5 / 0.75 / 1.0 across the row, and #IMA_GPU_LINEAR_PREMUL
 * set. The texture upload pre-multiplies the straight bytes, the chain's Divide undoes that and the
 * CPU reads straight; the chain and the CPU must agree at every pixel. A mismatch is the visible
 * rim along a soft mask stroke (10b).
 */
TEST_F(PaintLayersGraphEvalTest, mask_item_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "MaskPartialAlpha");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255));

  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *mask = BKE_image_add_generated(
      bmain, size, size, "MaskStraight", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(mask, nullptr);
  fill_straight_soft_edge(mask, size, 0.6f);
  set_mask_image(*top, mask);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel_at(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same soft edge, but on a map tagged with a colour space (not data). The Image Texture node
 * itself un-premultiplies a non-data texture whenever the Alpha output is used
 * (node_shader_tex_image.cc:162-167), so the generated chain must NOT build its own Divide for
 * such a map -- otherwise alpha is removed twice and the soft edge shows `C / A` (the H1 rim).
 * With the Divide built only for data maps the two sides agree at every pixel.
 */
TEST_F(PaintLayersGraphEvalTest, mask_item_color_space_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "MaskColorSpace");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255));

  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *mask = BKE_image_add_generated(
      bmain, size, size, "MaskColorSpace", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(mask, nullptr);
  fill_straight_soft_edge(mask, size, 0.6f);
  set_mask_image(*top, mask);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel_at(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Fill mask on a row that takes part in four channels must apply to each of them: the graph and
 * the CPU composite have to agree per channel. That only holds if every channel's row chain feeds
 * its own factor into the one shared `.PL Mask` tree, the multi-channel counterpart of the packed
 * single-channel mask tests.
 */
TEST_F(PaintLayersGraphEvalTest, mask_row_in_four_channels_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channels[] = {
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
      PAINT_MATERIAL_CHANNEL_SPECULAR,
  };
  auto result_name_for = [](const int ch) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(ch)).ui_name;
  };
  ma = BKE_material_add(bmain, "MaskFourChannels");

  MaterialPaintLayer *row = add_layer("Row",
                                      MA_PAINT_LAYER_SOURCE_IMAGE,
                                      add_solid_image("RowBC", size, 200, 100, 50, 255),
                                      channels[0]);
  ASSERT_NE(row, nullptr);
  const uchar extra_pixels[3][3] = {{60, 60, 60}, {90, 90, 90}, {128, 128, 128}};
  for (int i = 1; i < 4; i++) {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, row, channels[i]);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("RowChannel",
                                    size,
                                    extra_pixels[i - 1][0],
                                    extra_pixels[i - 1][1],
                                    extra_pixels[i - 1][2],
                                    255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  /* The base Fill mask: one grey constant the shared subgroup reduces on every channel. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.5f), nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (const eMaterialPaintChannel channel : channels) {
    const RGBA graph = interpreter.eval_result(result_name_for(channel).c_str());
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << result_name_for(channel);
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << result_name_for(channel);
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << result_name_for(channel);
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A content Paint correction whose map is read as colour data (Roughness, Normal, Metallic, ...):
 * the Image Texture node leaves the upload's pre-multiplied colour as `C * A`, so the chain builds
 * a Divide to recover `C`; the CPU reads the straight bytes. On a soft edge the two must agree, or
 * the correction's coverage is applied twice (a rim at the edge).
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_data_channel_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  auto result_name_for = [](const int ch) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(ch)).ui_name;
  };

  ma = BKE_material_add(bmain, "ContentData");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 128, 128, 128, 255), channel);
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 200, 200, 200, 255), channel);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *map = BKE_image_add_generated(
      bmain, size, size, "RoughCorr", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(map, nullptr);
  fill_straight_soft_edge(map, size, 0.3f);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const std::string out = result_name_for(channel);
  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(out.c_str());
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same correction on Base Color: the map is not data, so the Image Texture node itself
 * un-premultiplies and the chain must NOT build its own Divide.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_color_channel_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  auto result_name_for = [](const int ch) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(ch)).ui_name;
  };

  ma = BKE_material_add(bmain, "ContentColor");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255), channel);
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *map = BKE_image_add_generated(
      bmain, size, size, "ColorCorr", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(map, nullptr);
  fill_straight_soft_edge(map, size, 0.4f);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const std::string out = result_name_for(channel);
  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(out.c_str());
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * An Effect correction with source Material in Hybrid mode (a live constant): the generator's
 * group input and the CPU's #composite_material_live must agree exactly, like a Layer row of that
 * source (#live_material_constant_matches_the_cpu).
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_material_constant_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CorrMatConstant");
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "CorrMatConstantSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.6f, 0.2f, 0.1f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same Effect correction, but in Baked mode (the source has no Principled): both sides read
 * the correction's own external bake through #paint_layer_channel_image.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_material_baked_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CorrMatBaked");
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "CorrMatBakedSource");
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  Image *bake_map = add_solid_image("CorrMatBakedMap", size, 10, 200, 90, 255);
  fill_straight_soft_edge(bake_map, size, 0.4f);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, channel, bake_map));
  BKE_paint_layers_bake_finalize(*ma, *correction);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Baked);

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

/**
 * A Material correction in Hybrid mode with a live source Alpha < 1: its coverage is that Alpha
 * constant, exactly like a Layer row of that source (`layer_factor`) -- never the flat opacity a
 * live colour constant would otherwise give it (that was the bug: before this fix, a live-constant
 * correction ignored its source's Alpha entirely and covered fully).
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_material_constant_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CorrMatConstantAlpha");
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "CorrMatConstantAlphaSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.6f, 0.2f, 0.1f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.4f;

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Hybrid);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  /* The correction's blend factor is opacity(1.0, default) * source Alpha(0.4), so it must land
   * strictly between the bottom colour and the correction's own colour -- proof the Alpha actually
   * scaled it, not a full-strength blend. */
  EXPECT_GT(graph.b, 0.0f);
  EXPECT_LT(graph.b, 1.0f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Material correction in Baked mode whose own bake coverage is a soft, non-uniform edge (not a
 * fresh row's flat 1.0): the coverage varies per pixel exactly like a Layer row's own
 * `layer->bake->coverage` fallback (`grey_of_map`), never the content bake map's own alpha.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_material_baked_partial_coverage_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CorrMatBakedCoverage");
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "CorrMatBakedCoverageSource");
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "C");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, correction, source));
  /* An opaque colour map: only the separate bake coverage should vary the result across x. A baked
   * coverage is read as its grey (the mean of RGB), never its alpha -- the mask-correction
   * convention #paint_material_composite.cc documents -- so the map's grey is what must vary here,
   * with alpha left opaque throughout. */
  Image *bake_map = add_solid_image("CorrMatBakedCoverageMap", size, 10, 200, 90, 255);
  Image *bake_coverage = add_solid_image("CorrMatBakedCoverageAlpha", size, 255, 255, 255, 255);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(bake_coverage, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (int x = 0; x < size; x++) {
      const uchar grey = uchar(64 * (x + 1));
      pixels[x * 4 + 0] = grey;
      pixels[x * 4 + 1] = grey;
      pixels[x * 4 + 2] = grey;
      pixels[x * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(bake_coverage, ibuf, lock);
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, channel, bake_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, -1, bake_coverage));
  BKE_paint_layers_bake_finalize(*ma, *correction);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *correction), PaintLayerMaterialMode::Baked);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const float tolerance = 1e-4f;
  Vector<float> graph_reds;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
    graph_reds.append(graph.r);
  }
  /* The soft-edge coverage must actually vary the result across x, or this test could pass with
   * the coverage silently ignored on both sides. */
  EXPECT_NE(graph_reds[0], graph_reds[size - 1]);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * An Effect correction with source Node Group: Baked-only, like a Layer row of that source. With a
 * valid bake, both sides read the same external bake image.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_node_group_baked_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CorrNodeGroupBaked");
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "NGC");
  ASSERT_NE(correction, nullptr);
  Image *bake_map = add_solid_image("CorrNgBakedMap", size, 220, 30, 140, 255);
  fill_straight_soft_edge(bake_map, size, 0.6f);
  Image *coverage = add_solid_image("CorrNgBakedCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, channel, bake_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *correction);

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

/**
 * Without a valid bake, a Node Group correction contributes nothing at all -- on both sides -- so
 * the result equals the row with no correction. #BKE_paint_layers_composite_image_layers's own
 * content_corrections list must come back empty for it too.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_node_group_without_bake_matches_the_uncorrected_row)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CorrNodeGroupNoBake");
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA cpu_before = cpu_pixel(channel);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "NGC");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
  for (const PaintMaterialCompositeImageLayer &l : layers) {
    if (BLI_uuid_equal(l.marker, top->marker)) {
      EXPECT_TRUE(l.content_corrections.is_empty());
    }
  }

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu_after = cpu_pixel(channel);
  EXPECT_NEAR(cpu_after.r, cpu_before.r, 1e-6f);
  EXPECT_NEAR(cpu_after.g, cpu_before.g, 1e-6f);
  EXPECT_NEAR(cpu_after.b, cpu_before.b, 1e-6f);
  EXPECT_NEAR(graph.r, cpu_after.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu_after.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu_after.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Mask Item with source Material reading its own Alpha channel (`mask_channel` = Alpha), in
 * Hybrid mode (a live constant): the mask's grey is the coverage itself, and its own factor is
 * flat (opacity alone) -- graph and CPU must still agree exactly.
 */
TEST_F(PaintLayersGraphEvalTest, mask_material_alpha_hybrid_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatAlphaHybrid");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "MaskMatAlphaHybridSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.35f;

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ALPHA);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  /* Partial coverage: the result must sit strictly between the bottom (red) and the row's own
   * blue, proving the mask actually clipped the row rather than leaving it fully on or off. */
  EXPECT_GT(graph.b, 0.0f);
  EXPECT_LT(graph.b, 1.0f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same Alpha-channel mask, but Baked (no Principled): both sides read the correction's own
 * bake coverage, which varies across x. */
TEST_F(PaintLayersGraphEvalTest, mask_material_alpha_baked_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatAlphaBaked");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "MaskMatAlphaBakedSource");
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ALPHA);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  Image *coverage = add_solid_image("MaskMatAlphaBakedCoverage", size, 255, 255, 255, 255);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(coverage, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (int x = 0; x < size; x++) {
      const uchar grey = uchar(50 * (x + 1));
      pixels[x * 4 + 0] = grey;
      pixels[x * 4 + 1] = grey;
      pixels[x * 4 + 2] = grey;
      pixels[x * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(coverage, ibuf, lock);
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *mask, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *mask);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const float tolerance = 1e-4f;
  Vector<float> reds;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
    reds.append(graph.r);
  }
  EXPECT_NE(reds[0], reds[size - 1]);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Mask Item reading a scalar channel (Roughness), Hybrid mode: the mask's grey is the live
 * constant itself (R=G=B by construction), and its own factor multiplies by the source's separate
 * Alpha coverage. */
TEST_F(PaintLayersGraphEvalTest, mask_material_roughness_hybrid_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatRoughHybrid");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "MaskMatRoughHybridSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.7f;
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.5f;

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same Roughness mask, Baked: both sides read the correction's own bake map, which varies
 * across x, as its grey (mean of RGB, R=G=B by the bake/AOV convention). */
TEST_F(PaintLayersGraphEvalTest, mask_material_roughness_baked_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatRoughBaked");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "MaskMatRoughBakedSource");
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  Image *rough_map = add_solid_image("MaskMatRoughBakedMap", size, 255, 255, 255, 255);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(rough_map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (int x = 0; x < size; x++) {
      const uchar grey = uchar(50 * (x + 1));
      pixels[x * 4 + 0] = grey;
      pixels[x * 4 + 1] = grey;
      pixels[x * 4 + 2] = grey;
      pixels[x * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(rough_map, ibuf, lock);
  }
  ASSERT_TRUE(
      BKE_paint_layers_bake_set_map(*ma, *mask, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_map));
  BKE_paint_layers_bake_finalize(*ma, *mask);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const float tolerance = 1e-4f;
  Vector<float> reds;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
    reds.append(graph.r);
  }
  EXPECT_NE(reds[0], reds[size - 1]);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Mask Item reading a colour channel (Base Color), Hybrid mode: the mask's grey is the live
 * constant's luminance. This is the critical check the coordinator asked for: the generator's
 * #SH_NODE_RGBTOBW and the CPU's #IMB_colormanagement_get_luminance must agree on a distinctly
 * non-grey colour, or graph and CPU would disagree here specifically -- no tolerance was widened
 * to make this pass.
 */
TEST_F(PaintLayersGraphEvalTest, mask_material_base_color_hybrid_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatColorHybrid");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "MaskMatColorHybridSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float base_color[4] = {0.6f, 0.2f, 0.1f, 1.0f};
  bNodeSocket *base_color_socket = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color_socket, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color_socket->default_value)->value,
            base_color);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.6f;

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  /* A mean of (0.6, 0.2, 0.1) would be 0.3; its luminance is noticeably different. Proves the
   * mask actually used luminance and not the mean the Image/Mesh Map path still uses. */
  const float mean = (base_color[0] + base_color[1] + base_color[2]) / 3.0f;
  float luminance[3] = {base_color[0], base_color[1], base_color[2]};
  const float luminance_value = IMB_colormanagement_get_luminance(luminance);
  EXPECT_GT(std::abs(luminance_value - mean), 0.02f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same Base Color mask, Baked: both sides reduce the correction's own bake map to luminance,
 * per pixel, across x. */
TEST_F(PaintLayersGraphEvalTest, mask_material_base_color_baked_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatColorBaked");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  Material *source = BKE_material_add(bmain, "MaskMatColorBakedSource");
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  Image *color_map = add_solid_image("MaskMatColorBakedMap", size, 150, 60, 20, 255);
  ASSERT_TRUE(
      BKE_paint_layers_bake_set_map(*ma, *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color_map));
  BKE_paint_layers_bake_finalize(*ma, *mask);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Mask Item with source Node Group: Baked-only, like its content-correction counterpart. With a
 * valid bake (a scalar channel and its own coverage), both sides agree. */
TEST_F(PaintLayersGraphEvalTest, mask_node_group_baked_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskNodeGroupBaked");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "N");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  Image *rough_map = add_solid_image("MaskNgRoughMap", size, 255, 255, 255, 255);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(rough_map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (int x = 0; x < size; x++) {
      const uchar grey = uchar(50 * (x + 1));
      pixels[x * 4 + 0] = grey;
      pixels[x * 4 + 1] = grey;
      pixels[x * 4 + 2] = grey;
      pixels[x * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(rough_map, ibuf, lock);
  }
  Image *coverage = add_solid_image("MaskNgCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(
      BKE_paint_layers_bake_set_map(*ma, *mask, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *mask, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *mask);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const float tolerance = 1e-4f;
  Vector<float> reds;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
    reds.append(graph.r);
  }
  EXPECT_NE(reds[0], reds[size - 1]);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Material mask with no bake at all yet (Baked mode, no Principled) leaves the row's coverage
 * exactly as it was without the mask -- on both sides. */
TEST_F(PaintLayersGraphEvalTest, mask_material_without_bake_does_not_change_coverage)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatNoBake");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA cpu_before = cpu_pixel(channel);

  Material *source = BKE_material_add(bmain, "MaskMatNoBakeSource");
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu_after = cpu_pixel(channel);
  EXPECT_NEAR(cpu_after.r, cpu_before.r, 1e-6f);
  EXPECT_NEAR(cpu_after.g, cpu_before.g, 1e-6f);
  EXPECT_NEAR(cpu_after.b, cpu_before.b, 1e-6f);
  EXPECT_NEAR(graph.r, cpu_after.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu_after.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu_after.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Normal mask channel is quietly skipped: the row's coverage is unaffected, on both sides. */
TEST_F(PaintLayersGraphEvalTest, mask_material_normal_channel_is_skipped)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaskMatNormalSkip");
  add_layer("Bottom", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_SOURCE_IMAGE, add_solid_image("Top", size, 0, 0, 255, 255), channel);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA cpu_before = cpu_pixel(channel);

  Material *source = BKE_material_add(bmain, "MaskMatNormalSkipSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mask, source));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu_after = cpu_pixel(channel);
  EXPECT_NEAR(cpu_after.r, cpu_before.r, 1e-6f);
  EXPECT_NEAR(cpu_after.g, cpu_before.g, 1e-6f);
  EXPECT_NEAR(cpu_after.b, cpu_before.b, 1e-6f);
  EXPECT_NEAR(graph.r, cpu_after.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu_after.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu_after.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

}  // namespace blender::bke::tests
