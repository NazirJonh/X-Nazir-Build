/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The generated graph of Paint rows against the CPU composite of the same description: for each
 * case the graph is evaluated at a pixel by the interpreter of
 * `paint_layers_graph_eval_test_fixture.hh` and compared with
 * #BKE_paint_layers_composite_image_layers evaluated at the same pixel.
 */

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

TEST_F(PaintLayersGraphEvalTest, two_map_rows_match_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.6f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, map_alpha_is_the_row_coverage_like_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  /* A texel the map leaves half transparent shows what is below, like on the CPU. */
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 128));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.8f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, constant_row_over_a_map_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  const float value[4] = {0.9f, 0.1f, 0.2f, 1.0f};
  MaterialPaintLayer *top = add_constant_layer("Top", value);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.5f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, disabled_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, top, false));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, scalar_channel_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 64, 64, 64, 255);
  Image *top = add_solid_image("Top", 200, 200, 200, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top_layer, 0.35f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersGraphEvalTest, normal_rows_combine_like_the_cpu)
{
  Image *base = add_solid_image("Base", 160, 128, 230, 255);
  Image *detail = add_solid_image("Detail", 100, 150, 220, 255);
  make_image_data(base);
  make_image_data(detail);
  add_layer("Base", base, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *detail_layer = add_layer("Detail", detail, PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, detail_layer, 0.5f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersGraphEvalTest, rows_of_different_channels_do_not_leak_into_each_other)
{
  Image *color = add_solid_image("Color", 200, 100, 50, 255);
  Image *rough = add_solid_image("Rough", 200, 200, 200, 255);
  make_image_data(rough);
  add_layer("Color", color, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  add_layer("Rough", rough, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersGraphEvalTest, every_blend_mode_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.7f));

  /* Every user-facing Mix mode: the generated node's blend and the CPU's `ramp_blend` read the one
   * table, so each must land on the same pixel. Normal Combine is the Normal channel's own doing
   * and is covered by `normal_rows_combine_like_the_cpu`. */
  const eMaterialPaintLayerBlend blends[] = {
      MA_PAINT_LAYER_BLEND_MIX,        MA_PAINT_LAYER_BLEND_MULTIPLY,
      MA_PAINT_LAYER_BLEND_OVERLAY,    MA_PAINT_LAYER_BLEND_ADD,
      MA_PAINT_LAYER_BLEND_SUBTRACT,   MA_PAINT_LAYER_BLEND_SCREEN,
      MA_PAINT_LAYER_BLEND_DIFFERENCE, MA_PAINT_LAYER_BLEND_DARKEN,
      MA_PAINT_LAYER_BLEND_LIGHTEN,    MA_PAINT_LAYER_BLEND_BURN,
      MA_PAINT_LAYER_BLEND_DODGE,      MA_PAINT_LAYER_BLEND_EXCLUSION,
      MA_PAINT_LAYER_BLEND_SOFT_LIGHT, MA_PAINT_LAYER_BLEND_LINEAR_LIGHT,
      MA_PAINT_LAYER_BLEND_HUE,        MA_PAINT_LAYER_BLEND_SATURATION,
      MA_PAINT_LAYER_BLEND_COLOR,      MA_PAINT_LAYER_BLEND_VALUE,
  };
  for (const eMaterialPaintLayerBlend blend : blends) {
    ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, top, blend));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersGraphEvalTest, constant_mask_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 0.25f), nullptr);

  /* The mask hides three quarters of the row: the pixel is mostly what is below. */
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, painted_mask_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, top, 1.0f);
  ASSERT_NE(mask, nullptr);
  /* A mask map that is grey and half covered: the mask only bites where it was painted. */
  Image *map = add_solid_image("Mask", 60, 60, 60, 128);
  make_image_data(map);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, mask_on_a_translucent_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 128));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.8f));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 0.5f), nullptr);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, disabled_mask_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, top, 0.0f);
  ASSERT_NE(mask, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, mask, false));

  /* An off mask is not a mask that hides everything: the row is wholly visible. */
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, mask_reaches_every_channel_of_its_row)
{
  Image *color = add_solid_image("Color", 200, 100, 50, 255);
  Image *rough = add_solid_image("Rough", 200, 200, 200, 255);
  make_image_data(rough);
  add_layer("Bottom", add_solid_image("Bottom", 30, 30, 30, 255));
  MaterialPaintLayer *top = add_layer("Top", color);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, top, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, top, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 0.3f), nullptr);

  /* One mask per row, not per channel: both chains read the same factor. */
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersGraphEvalTest, fill_layer_lays_its_colour)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  /* Alpha 0: a Fill still lays its constant, unlike a Paint row, whose alpha 0 lays nothing. */
  const float color[4] = {0.2f, 0.4f, 0.6f, 0.0f};
  MaterialPaintLayer *fill = add_constant_layer("Fill", color);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, color));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

}  // namespace blender::bke::tests
