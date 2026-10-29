/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The generated graph of Paint rows against the CPU composite of the same description: for each
 * case the graph is evaluated at a pixel by the interpreter of
 * `paint_layers_graph_eval_test_fixture.hh` and compared with
 * #BKE_paint_layers_composite_image_layers evaluated at the same pixel.
 */

#include <cmath>
#include <cstring>
#include <iterator>

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

TEST_F(PaintLayersGraphEvalTest, channel_blend_overrides_the_rows_own_for_that_channel)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.7f));

  /* The row mixes; the Base Color channel multiplies over that. */
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, top, MA_PAINT_LAYER_BLEND_MIX));
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA multiplied = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* Inherit puts the channel back on the row's Mix, so the pixel has to change: the override is
   * read by the CPU and the graph alike, not silently ignored by both. */
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, -1));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA mixed = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_GT(std::fabs(mixed.r - multiplied.r) + std::fabs(mixed.g - multiplied.g) +
                std::fabs(mixed.b - multiplied.b),
            1e-3f);
}

TEST_F(PaintLayersGraphEvalTest, every_channel_blend_mode_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.7f));

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
    ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
        *ma, *top, PAINT_MATERIAL_CHANNEL_BASE_COLOR, blend));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersGraphEvalTest, normal_replace_covers_the_lower_normal_like_the_cpu)
{
  Image *base = add_solid_image("Base", 160, 128, 230, 255);
  Image *detail = add_solid_image("Detail", 100, 150, 220, 255);
  make_image_data(base);
  make_image_data(detail);
  add_layer("Base", base, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *detail_layer = add_layer("Detail", detail, PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, detail_layer, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *detail_layer, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
  const RGBA replaced = cpu_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *detail_layer, PAINT_MATERIAL_CHANNEL_NORMAL, -1));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
  const RGBA combined = cpu_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_GT(std::fabs(combined.r - replaced.r) + std::fabs(combined.g - replaced.g) +
                std::fabs(combined.b - replaced.b),
            1e-3f);
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

TEST_F(PaintLayersGraphEvalTest, fill_mask_correction_replaces_the_coverage_by_its_opacity)
{
  /* A black bottom, so the pixel is the row's colour at the mask's coverage and nothing else. */
  add_layer("Bottom", add_solid_image("Bottom", 0, 0, 0, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 0.2f), nullptr);
  const RGBA hidden = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* A white Fill at half opacity over a base of 0.2: `lerp(0.2, 1, 0.5) = 0.6`, three times it. */
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, item, 0.5f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA shown = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(shown.r, hidden.r * 3.0f, 3e-3f);
  EXPECT_NEAR(shown.g, hidden.g * 3.0f, 3e-3f);
  EXPECT_NEAR(shown.b, hidden.b * 3.0f, 3e-3f);
}

TEST_F(PaintLayersGraphEvalTest, paint_mask_correction_matches_the_cpu_and_lays_nothing_unpainted)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 0.3f), nullptr);
  const RGBA base_only = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* Added, not painted: it has no map, so there is no node for it and no pixel changes. */
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint");
  ASSERT_NE(item, nullptr);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA unpainted = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(unpainted.r, base_only.r, 1e-4f);
  EXPECT_NEAR(unpainted.g, base_only.g, 1e-4f);
  EXPECT_NEAR(unpainted.b, base_only.b, 1e-4f);

  /* A white, half-covering stroke brings the coverage towards one where it was painted. */
  Image *map = add_solid_image("MaskPaint", 255, 255, 255, 128);
  make_image_data(map);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA painted = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_GT(painted.b, base_only.b);
}

TEST_F(PaintLayersGraphEvalTest, mask_correction_blends_match_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 0.8f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", 0.4f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, item, 0.7f));

  /* The blend of the factor below with the correction's grey: one table for both sides. */
  const eMaterialPaintLayerBlend blends[] = {MA_PAINT_LAYER_BLEND_MIX,
                                             MA_PAINT_LAYER_BLEND_MULTIPLY,
                                             MA_PAINT_LAYER_BLEND_ADD,
                                             MA_PAINT_LAYER_BLEND_SUBTRACT,
                                             MA_PAINT_LAYER_BLEND_SCREEN,
                                             MA_PAINT_LAYER_BLEND_DARKEN,
                                             MA_PAINT_LAYER_BLEND_LIGHTEN,
                                             MA_PAINT_LAYER_BLEND_DIFFERENCE};
  for (const eMaterialPaintLayerBlend blend : blends) {
    ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, item, blend));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersGraphEvalTest, mask_corrections_lay_in_order_and_are_value_edits)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 200));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 1.0f), nullptr);
  MaterialPaintLayer *first = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "First", 0.1f);
  MaterialPaintLayer *second = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_IMAGE, "Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  Image *map = add_solid_image("SecondMap", 220, 220, 220, 150);
  make_image_data(map);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, second, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, first, 0.6f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA before = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* Opacity, visibility and the grey are inputs of the row's group: nothing is rebuilt. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, first, 0.3f));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float grey[4] = {0.9f, 0.9f, 0.9f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, first, PAINT_MATERIAL_CHANNEL_BASE_COLOR, grey));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA after = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_GT(std::abs(before.r - after.r) + std::abs(before.g - after.g) +
                std::abs(before.b - after.b),
            1e-3f);

  /* A hidden correction is one that is not there. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, first, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, second, false));
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

TEST_F(PaintLayersGraphEvalTest, folder_of_two_rows_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 255));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, second, 0.5f));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.7f));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, folder_with_translucent_maps_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 128));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 100));
  MaterialPaintLayer *members[2] = {first, second};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2)), nullptr);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, reorder_inside_a_folder_reaches_the_graph)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 255));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 255));
  MaterialPaintLayer *members[2] = {first, second};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2)), nullptr);

  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  expect_graph_matches_cpu(channel);
  const RGBA before = graph_pixel(channel);

  /* The folder's rows are the same set in another order, so only the order is structure here: a
   * root hash that ignores it keeps the old chain and the graph keeps showing the old result. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, second, 0));
  expect_graph_matches_cpu(channel);
  const RGBA after = graph_pixel(channel);
  EXPECT_GT(std::abs(before.r - after.r) + std::abs(before.g - after.g) +
                std::abs(before.b - after.b),
            0.1f);
}

TEST_F(PaintLayersGraphEvalTest, nested_folder_order_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *base = add_layer("Base", add_solid_image("Base", 10, 200, 40, 255));
  MaterialPaintLayer *inner_low = add_layer("InnerLow",
                                            add_solid_image("InnerLow", 30, 160, 220, 255));
  MaterialPaintLayer *inner_high = add_layer("InnerHigh",
                                             add_solid_image("InnerHigh", 240, 20, 90, 255));
  MaterialPaintLayer *over = add_layer("Over", add_solid_image("Over", 250, 250, 20, 128));

  /* B = [Base, A = [InnerLow, InnerHigh], Over]: A sits below the row that overlaps it. */
  MaterialPaintLayer *inner_members[2] = {inner_low, inner_high};
  MaterialPaintLayer *inner = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(inner_members, 2));
  ASSERT_NE(inner, nullptr);
  MaterialPaintLayer *outer_members[3] = {base, inner, over};
  MaterialPaintLayer *outer = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(outer_members, 3));
  ASSERT_NE(outer, nullptr);

  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  expect_graph_matches_cpu(channel);
  const RGBA initial = graph_pixel(channel);

  /* Inside the inner folder. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, inner_high, 0));
  expect_graph_matches_cpu(channel);

  /* The inner folder moves below Base, still inside the outer one. */
  ASSERT_TRUE(BKE_paint_layers_move(*ma, inner, base, PaintLayerPlace::Below));
  expect_graph_matches_cpu(channel);
  const RGBA moved = graph_pixel(channel);
  EXPECT_GT(std::abs(initial.r - moved.r) + std::abs(initial.g - moved.g) +
                std::abs(initial.b - moved.b),
            0.1f);

  /* Back above Base: the original structure gives the original pixel again. */
  ASSERT_TRUE(BKE_paint_layers_reorder(*ma, inner_low, 0));
  ASSERT_TRUE(BKE_paint_layers_move(*ma, inner, base, PaintLayerPlace::Above));
  const RGBA restored = graph_pixel(channel);
  EXPECT_NEAR(restored.r, initial.r, 2e-3f);
  EXPECT_NEAR(restored.g, initial.g, 2e-3f);
  EXPECT_NEAR(restored.b, initial.b, 2e-3f);
  expect_graph_matches_cpu(channel);
}

/* -------------------------------------------------------------------- */
/** \name UV mapping
 *
 * A Fill's own map read through its Mapping node: the generated graph (a Texture Coordinate, a
 * Mapping fed by the row's inputs, the Image Texture's Vector) against the CPU resample, for a set
 * of offset / scale / rotation values that includes a mirror and a zoom.
 * \{ */

class PaintLayersMappingEvalTest : public PaintLayersGraphEvalTest {
 public:
  struct Case {
    float offset[2];
    float scale[2];
    float rotation;
  };

  /** The values the cases walk through; each is an edit of values only. */
  static Span<const Case> cases()
  {
    static const Case all[] = {
        {{0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f},
        {{0.1f, 0.2f}, {2.0f, 2.0f}, 0.0f},
        {{0.0f, 0.0f}, {1.0f, 1.0f}, 0.7f},
        {{0.3f, -0.15f}, {2.0f, 1.5f}, 1.2f},
        {{0.0f, 0.0f}, {-1.0f, 1.0f}, 0.0f},
        {{0.25f, 0.0f}, {0.5f, -2.0f}, 2.4f},
    };
    return Span<const Case>(all, int64_t(std::size(all)));
  }

  /** A 2x2 byte map of four distinct colours. */
  Image *add_quad_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, 2, 2, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    const uchar texels[4][4] = {
        {220, 40, 30, 255}, {30, 200, 60, 255}, {40, 60, 230, 255}, {200, 170, 90, 255}};
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int i : IndexRange(4)) {
      memcpy(pixels + i * 4, texels[i], 4);
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** A Fill row on top of the stack whose \a channel record is backed by \a image. */
  MaterialPaintLayer *add_fill(const char *name,
                               Image *image,
                               const eMaterialPaintChannel channel =
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, layer, channel, image));
    return layer;
  }

  void case_apply(MaterialPaintLayer &row, const Case &test)
  {
    ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, &row, test.offset));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(*ma, &row, false));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, &row, test.scale));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, &row, test.rotation));
  }
};

TEST_F(PaintLayersMappingEvalTest, a_mapped_fill_layer_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 10, 20, 30, 255));
  MaterialPaintLayer *fill = add_fill("Fill", add_quad_image("Quad"));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  for (const Case &test : cases()) {
    SCOPED_TRACE(::testing::Message() << "rotation=" << test.rotation);
    case_apply(*fill, test);
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersMappingEvalTest, a_mapped_fill_correction_matches_the_cpu)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 90, 120, 30, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, "C", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_quad_image("Quad")));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.7f));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  for (const Case &test : cases()) {
    SCOPED_TRACE(::testing::Message() << "rotation=" << test.rotation);
    case_apply(*correction, test);
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersMappingEvalTest, a_mapped_fill_mask_item_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 40, 40, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 60, 220, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_quad_image("MaskQuad")));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  for (const Case &test : cases()) {
    SCOPED_TRACE(::testing::Message() << "rotation=" << test.rotation);
    case_apply(*item, test);
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersMappingEvalTest, a_mapped_fill_normal_follows_rotation_and_mirror_like_the_cpu)
{
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  Image *flat = add_solid_image("Flat", 128, 128, 255, 255);
  Image *detail = add_solid_image("Detail", 204, 128, 230, 255);
  make_image_data(flat);
  make_image_data(detail);
  add_layer("Bottom", flat, channel);
  MaterialPaintLayer *fill = add_fill("MappedNormal", detail, channel);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  for (const Case &test : cases()) {
    SCOPED_TRACE(::testing::Message() << "rotation=" << test.rotation << " scale=" << test.scale[0]
                                      << "," << test.scale[1]);
    case_apply(*fill, test);
    expect_graph_matches_cpu(channel, 5e-3f);
  }
}

TEST_F(PaintLayersMappingEvalTest, an_unmapped_row_still_reads_the_pixel_directly)
{
  add_layer("Bottom", add_solid_image("Bottom", 10, 20, 30, 255));
  /* Support without the toggle: a Fill is made with its mapping on, so switching it off is what
   * leaves no Mapping, and both sides read the map where it is. */
  MaterialPaintLayer *fill = add_fill("Fill", add_solid_image("Solid", 200, 100, 50, 255));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, false));
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(*ma, *fill));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/* UV wrap: the clip factor the graph builds against the transparent texels the CPU resample
 * writes, in every mode, with values that put the evaluated pixel inside and outside the square. */

class PaintLayersWrapEvalTest : public PaintLayersMappingEvalTest {
 public:
  /** Values for the pixel (1, 1), whose UV is (0.375, 0.375): some read inside the square on both
   * axes, some outside on one, some on both. */
  static Span<const Case> wrap_cases()
  {
    static const Case all[] = {
        {{0.0f, 0.0f}, {1.0f, 1.0f}, 0.0f},
        {{0.8f, 0.0f}, {1.0f, 1.0f}, 0.0f},
        {{0.0f, -0.5f}, {1.0f, 1.0f}, 0.0f},
        {{0.1f, 0.2f}, {2.0f, 2.0f}, 0.4f},
        {{-0.2f, 0.7f}, {0.5f, 0.5f}, 1.0f},
        {{0.9f, 0.9f}, {1.0f, 1.0f}, 0.0f},
    };
    return Span<const Case>(all, int64_t(std::size(all)));
  }

  static Span<const int16_t> wraps()
  {
    static const int16_t all[] = {MA_PAINT_LAYER_MAPPING_WRAP_REPEAT,
                                  MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X,
                                  MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y,
                                  MA_PAINT_LAYER_MAPPING_WRAP_CLIP};
    return Span<const int16_t>(all, int64_t(std::size(all)));
  }
};

TEST_F(PaintLayersWrapEvalTest, a_wrapped_fill_layer_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 10, 20, 30, 255));
  MaterialPaintLayer *fill = add_fill("Fill", add_quad_image("Quad"));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  for (const int16_t wrap : wraps()) {
    ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, wrap));
    for (const Case &test : wrap_cases()) {
      SCOPED_TRACE(::testing::Message() << "wrap=" << wrap << " offset=" << test.offset[0] << ","
                                        << test.offset[1] << " rotation=" << test.rotation);
      case_apply(*fill, test);
      expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
  }
}

TEST_F(PaintLayersWrapEvalTest, a_wrapped_fill_correction_matches_the_cpu)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 90, 120, 30, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, "C", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(correction, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_quad_image("Quad")));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.7f));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, correction, true));
  for (const int16_t wrap : wraps()) {
    ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, correction, wrap));
    for (const Case &test : wrap_cases()) {
      SCOPED_TRACE(::testing::Message() << "wrap=" << wrap << " offset=" << test.offset[0] << ","
                                        << test.offset[1] << " rotation=" << test.rotation);
      case_apply(*correction, test);
      expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
  }
}

TEST_F(PaintLayersWrapEvalTest, a_wrapped_fill_mask_item_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 40, 40, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 60, 220, 255));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_quad_image("MaskQuad")));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, item, true));
  for (const int16_t wrap : wraps()) {
    ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, item, wrap));
    for (const Case &test : wrap_cases()) {
      SCOPED_TRACE(::testing::Message() << "wrap=" << wrap << " offset=" << test.offset[0] << ","
                                        << test.offset[1] << " rotation=" << test.rotation);
      case_apply(*item, test);
      expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
  }
}

TEST_F(PaintLayersWrapEvalTest, a_clipped_fill_normal_matches_the_cpu)
{
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_NORMAL;
  Image *flat = add_solid_image("Flat", 128, 128, 255, 255);
  Image *detail = add_solid_image("Detail", 204, 128, 230, 255);
  make_image_data(flat);
  make_image_data(detail);
  add_layer("Bottom", flat, channel);
  MaterialPaintLayer *fill = add_fill("ClippedNormal", detail, channel);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  for (const Case &test : wrap_cases()) {
    SCOPED_TRACE(::testing::Message() << "offset=" << test.offset[0] << "," << test.offset[1]
                                      << " rotation=" << test.rotation);
    case_apply(*fill, test);
    expect_graph_matches_cpu(channel, 5e-3f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Height blend modes
 * \{ */

/** A Height map with a soft edge over a neutral row: the graph and the CPU must read the same
 * straight value from the premultiplied float storage, or a partly-covered texel would decode to
 * value/alpha on one side and the two would disagree exactly where a stroke is softest. */
TEST_F(PaintLayersGraphEvalTest, a_partial_alpha_height_map_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 128, 128, 128, 255);
  make_image_data(bottom);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  Image *soft = add_solid_float_image("Soft", 0.8f, 0.8f, 0.8f, 0.25f);
  add_layer("Top", soft, PAINT_MATERIAL_CHANNEL_HEIGHT);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT, 5e-3f);
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  /* 0.8 over the neutral 0.5 at 25% coverage. A decode of 0.8/0.25 = 3.2 would read ~1.2. */
  EXPECT_NEAR(cpu.r, PAINT_HEIGHT_NEUTRAL * 0.75f + 0.8f * 0.25f, 0.02f);
}

/** Two height rows whose records sit either side of the neutral level, every mode of the Height
 * channel's own blend. The graph's Separate Color, Math and float Mix chain against the CPU's
 * `blend_row_height`, one mode at a time. */
TEST_F(PaintLayersGraphEvalTest, height_blend_modes_match_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 77, 77, 77, 255);
  Image *top = add_solid_image("Top", 204, 204, 204, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top_layer, 0.6f));

  const int modes[] = {MA_PAINT_LAYER_HEIGHT_BLEND_MIX,
                       MA_PAINT_LAYER_HEIGHT_BLEND_ADD,
                       MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT,
                       MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE};
  for (const int mode : modes) {
    SCOPED_TRACE(::testing::Message() << "mode=" << mode);
    ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
        *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, mode));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  }
}

/** The acceptance bar of the whole height model: two neutral records (grey 128 is the closest a
 * byte map comes to 0.5) must not dig a pit or raise a peak in any mode -- the ordinary colour
 * blends would give Multiply 0.25 and Add 1.0, the delta-space step stays at the neutral level. */
TEST_F(PaintLayersGraphEvalTest, height_neutral_layers_are_no_ops_in_every_mode)
{
  Image *bottom = add_solid_image("Bottom", 128, 128, 128, 255);
  Image *top = add_solid_image("Top", 128, 128, 128, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);

  const int modes[] = {MA_PAINT_LAYER_HEIGHT_BLEND_MIX,
                       MA_PAINT_LAYER_HEIGHT_BLEND_ADD,
                       MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT,
                       MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE};
  for (const int mode : modes) {
    SCOPED_TRACE(::testing::Message() << "mode=" << mode);
    ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
        *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, mode));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
    const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
    EXPECT_NEAR(cpu.r, PAINT_HEIGHT_NEUTRAL, 0.02f);
  }
}

/** The Height record's own scale doubles the distance from the neutral level; the input the chain
 * reads and the CPU's `1 + delta` agree. */
TEST_F(PaintLayersGraphEvalTest, height_scale_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 128, 128, 128, 255);
  Image *top = add_solid_image("Top", 204, 204, 204, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_ADD));
  ASSERT_TRUE(
      BKE_paint_layers_channel_height_scale_set(*ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT,
                                                2.0f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);

  /* And a scale edit reaches the built chain as a value: no rebuild needed, the same pixels. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_height_scale_set(*ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT,
                                                0.25f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
}

/** A row's mask and channel opacity weigh the height step exactly as they weigh its colour: the
 * row's factor chain feeds the height nodes the same sockets. */
TEST_F(PaintLayersGraphEvalTest, height_masked_row_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 77, 77, 77, 255);
  Image *top = add_solid_image("Top", 30, 160, 220, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);
  /* A half-transparent map alpha weighs the record by the row's coverage. */
  Image *half = add_solid_image("Half", 204, 204, 204, 128);
  make_image_data(half);
  BKE_paint_layers_channel_set_image(
      *ma, top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, half);
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *top_layer,
                                                   PAINT_MATERIAL_CHANNEL_HEIGHT, 0.35f));

  const int modes[] = {MA_PAINT_LAYER_HEIGHT_BLEND_ADD, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE};
  for (const int mode : modes) {
    SCOPED_TRACE(::testing::Message() << "mode=" << mode);
    ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
        *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, mode));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  }
}

/** The Compare mask gates the row's other channels: where its height lost, the row's Base Color
 * lays nothing; where it won, it covers by its own factors. The CPU renders the mask from the
 * Height prefix, the graph multiplies the won node into the factor -- the two must land together. */
TEST_F(PaintLayersGraphEvalTest, height_compare_masks_the_rows_other_channels)
{
  Image *base = add_solid_image("Base", 200, 100, 50, 255);
  Image *below_height = add_solid_image("BelowHeight", 230, 230, 230, 255);
  Image *losing = add_solid_image("Losing", 60, 60, 60, 255);
  Image *winning = add_solid_image("Winning", 250, 250, 250, 255);
  Image *row_color = add_solid_image("RowColor", 30, 160, 220, 255);
  for (Image *image : {base, below_height, losing, winning, row_color}) {
    make_image_data(image);
  }
  add_layer("Base", base);
  add_layer("BelowHeight", below_height, PAINT_MATERIAL_CHANNEL_HEIGHT);

  /* A row that loses the comparison: its colour must not show over the base. */
  MaterialPaintLayer *loser = add_layer("Loser", row_color);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, loser, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, loser, PAINT_MATERIAL_CHANNEL_HEIGHT, losing));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *loser, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA hidden = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(hidden.r, 200.0f / 255.0f, 2e-3f);

  /* A row that wins: its colour covers the base by its full factor. */
  BKE_paint_layers_channel_set_image(
      *ma, loser, PAINT_MATERIAL_CHANNEL_HEIGHT, winning);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA shown = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(shown.r, 30.0f / 255.0f, 2e-3f);
}

/** A Compare row inside a folder reads the folder's own straightened accumulation below it (the
 * divide node), which the CPU's won render mirrors through the synthetic folder evaluation. */
TEST_F(PaintLayersGraphEvalTest, height_compare_inside_a_folder_matches_the_cpu)
{
  Image *base = add_solid_image("Base", 200, 100, 50, 255);
  Image *inner_height = add_solid_image("InnerHeight", 90, 90, 90, 255);
  Image *compare_height = add_solid_image("CompareHeight", 120, 120, 120, 255);
  Image *row_color = add_solid_image("RowColor", 30, 160, 220, 255);
  for (Image *image : {base, inner_height, compare_height, row_color}) {
    make_image_data(image);
  }
  add_layer("Base", base);

  MaterialPaintLayer *first = BKE_paint_layers_add(*ma, "Inner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(first, nullptr);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, first, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, first, PAINT_MATERIAL_CHANNEL_HEIGHT, inner_height));
  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, "Comparer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, second, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, second, PAINT_MATERIAL_CHANNEL_BASE_COLOR, row_color));
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, second, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, second, PAINT_MATERIAL_CHANNEL_HEIGHT, compare_height));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *second, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));

  /* A folder over the base row: the comparer's height (120) wins over the inner row's (90) inside
   * the folder, so its colour covers within the folder -- and the folder, covering fully, shows it
   * over the base. Both sides must land on that one answer. */
  MaterialPaintLayer *members[2] = {first, second};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2)), nullptr);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  const RGBA shown = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(shown.r, 30.0f / 255.0f, 2e-3f);
}

/**
 * The first rows of an isolating folder have nothing below them, so the folder's divide node reads
 * `0 / 0` and a Vector Math Divide answers zero for it. The CPU's empty accumulation is that same
 * zero, and the boundary case proves it on both sides: a black record sits exactly on the `-0.5`
 * the comparison is made against and must lose, anything above it must win.
 */
TEST_F(PaintLayersGraphEvalTest, height_compare_first_row_of_a_folder_matches_the_cpu)
{
  Image *base = add_solid_image("Base", 200, 100, 50, 255);
  Image *black_height = add_solid_image("BlackHeight", 0, 0, 0, 255);
  Image *grey_height = add_solid_image("GreyHeight", 120, 120, 120, 255);
  Image *row_color = add_solid_image("RowColor", 30, 160, 220, 255);
  for (Image *image : {base, black_height, grey_height, row_color}) {
    make_image_data(image);
  }
  add_layer("Base", base);

  MaterialPaintLayer *comparer = BKE_paint_layers_add(
      *ma, "Comparer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(comparer, nullptr);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, row_color));
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, black_height));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));

  /* The comparer is the first row of its own folder, so it compares against the empty
   * accumulation rather than against anything the stack holds below it. */
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(&comparer, 1)), nullptr);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA hidden = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(hidden.r, 200.0f / 255.0f, 2e-3f);

  /* Above black the record wins, so its colour covers inside the folder. */
  BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, grey_height);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const RGBA shown = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(shown.r, 30.0f / 255.0f, 2e-3f);
}

/** The three factors the acceptance bar names together: the Compare gate, a row mask and a channel
 * opacity. All three multiply into the one factor the row's other channels are laid by, on the
 * CPU and in the graph alike. */
TEST_F(PaintLayersGraphEvalTest, height_compare_with_mask_and_opacity_matches_the_cpu)
{
  Image *base = add_solid_image("Base", 200, 100, 50, 255);
  Image *below_height = add_solid_image("BelowHeight", 60, 60, 60, 255);
  Image *winning = add_solid_image("Winning", 250, 250, 250, 255);
  Image *row_color = add_solid_image("RowColor", 30, 160, 220, 255);
  for (Image *image : {base, below_height, winning, row_color}) {
    make_image_data(image);
  }
  add_layer("Base", base);
  add_layer("BelowHeight", below_height, PAINT_MATERIAL_CHANNEL_HEIGHT);

  MaterialPaintLayer *comparer = add_layer("Comparer", row_color);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, winning));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, comparer, 0.5f), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_opacity_set(*ma, *comparer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.5f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* The row won the comparison, so what is left of its factor is the mask times the opacity. */
  const float factor = 0.5f * 0.5f;
  const RGBA pixel = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(pixel.r, (200.0f / 255.0f) * (1.0f - factor) + (30.0f / 255.0f) * factor, 2e-3f);
}

/**
 * The Compare mask costs the same on any canvas: its prefix and the row's own content are acquired
 * once for the whole mask, and the strips it is rendered in only evaluate them. The picture here
 * is 192 rows, three strips of #COMPOSITE_HEIGHT_WON_STRIP_ROWS, so an acquisition per strip would
 * show up as six instead of two.
 */
TEST_F(PaintLayersGraphEvalTest, height_compare_mask_acquires_its_stacks_once)
{
  const int size = 192;
  const uchar high[4] = {230, 230, 230, 255};
  const uchar low[4] = {60, 60, 60, 255};
  Image *below_height = add_split_image("BelowHeight", size, high, low);
  const uchar row_grey[4] = {191, 191, 191, 255};
  Image *row_height = add_image("RowHeight", size, row_grey);
  const uchar row_color[4] = {30, 160, 220, 255};
  Image *color = add_image("RowColor", size, row_color);
  for (Image *image : {below_height, row_height, color}) {
    make_image_data(image);
  }
  add_layer("BelowHeight", below_height, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *comparer = add_layer("Comparer", color);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, row_height));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));
  /* A mapped row: the resample of a mapped layer is the other half of what a strip repeats. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, comparer, true));
  const float offset[2] = {0.3f, 0.1f};
  const float scale[2] = {1.7f, 1.7f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_values(*ma, comparer, offset, scale, 0.0f));

  paint_material_composite_acquire_call_count_reset();
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  /* The prefix and the row's content: one acquisition each, whatever the height of the canvas. */
  EXPECT_EQ(paint_material_composite_acquire_call_count(), 2);

  ASSERT_EQ(layers.size(), 1);
  const PaintMaterialCompositeImageLayer &top = layers.first();
  ASSERT_TRUE(top.height_won != nullptr);
  const ImBuf &mask_ref = *top.height_won;
  ASSERT_EQ(mask_ref.x, size);
  ASSERT_EQ(mask_ref.y, size);
  ASSERT_EQ(mask_ref.channels, 1);
  const float *mask = mask_ref.float_buffer.data;
  ASSERT_TRUE(mask != nullptr);

  /* The comparison written out rather than read back from the code under test. The rows below are
   * 230 on the left half and 60 on the right, the row's own record is 191 and its scale is one:
   * `d_below = 230/255 - 0.5 = +0.402` is not below `d_layer = 191/255 - 0.5 = +0.249`, so the row
   * lost there; `60/255 - 0.5 = -0.265` is, so it won there. The row's map is a flat grey, so the
   * mapping it reads through moves no value. */
  for (const int y : {0, 63, 64, 127, 128, 191}) {
    for (const int x : {0, 95, 96, 191}) {
      const float expected = (x < size / 2) ? 0.0f : 1.0f;
      EXPECT_NEAR(mask[int64_t(y) * size + x], expected, 1e-6f) << "x=" << x << " y=" << y;
    }
  }
}

/**
 * A Compare mask is rendered at the size of the Height stack, which the Height maps decide, while
 * the channel it gates is as big as that channel's own bottom map. The mask is one number per
 * texel and reaches the wider grid through a nearest-texel read with an edge clamp, which has to
 * read the one-channel float buffer it was rendered into as a grey: read at a four-channel stride
 * it would walk past every texel it means.
 */
TEST_F(PaintLayersGraphEvalTest, height_compare_mask_smaller_than_the_frame_reaches_it)
{
  const uchar neutral[4] = {128, 128, 128, 255};
  Image *below_height = add_image("BelowHeight", 4, neutral);
  const uchar white[4] = {255, 255, 255, 255};
  const uchar black[4] = {0, 0, 0, 255};
  Image *row_height = add_split_image("RowHeight", 4, white, black);
  const uchar row_rgba[4] = {200, 100, 50, 255};
  Image *color = add_image("RowColor", 8, row_rgba);
  for (Image *image : {below_height, row_height, color}) {
    make_image_data(image);
  }
  add_layer("BelowHeight", below_height, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *comparer = add_layer("Comparer", color);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, row_height));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_TRUE(layers.first().height_won != nullptr);
  /* The Height maps are 4x4 and the colour map is 8x8: the mask has to be resampled. */
  ASSERT_EQ(layers.first().height_won->x, 4);

  int width = 0;
  int height = 0;
  ASSERT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
  ASSERT_EQ(width, 8);
  ASSERT_EQ(height, 8);

  paint_material_composite_height_won_resample_count_reset();
  Vector<float> linear(int64_t(width) * height * 4);
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  ASSERT_TRUE(BKE_paint_material_composite_eval_images_linear(
      layers, PaintMaterialCompositeLinearTarget::frame(linear, width), nullptr, nullptr, bottom));
  EXPECT_EQ(paint_material_composite_height_won_resample_count(), 1);

  /* The four-by-four mask is white on its left half and black on its right, so the row won on the
   * left and lost on the right. Resampled onto eight texels by nearest with an edge clamp, the
   * target column x reads source texel floor(x / 2): columns 0-3 carry the row's own colour, the
   * empty bottom the seam never mixes into them the rest. The colours are the map's bytes over
   * 255, the image being a data map. */
  const float red = 200.0f / 255.0f;
  const float green = 100.0f / 255.0f;
  const float blue = 50.0f / 255.0f;
  const float factor[8] = {1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  for (int y = 0; y < height; y++) {
    for (const int x : IndexRange(width)) {
      const float *p = linear.data() + (int64_t(y) * width + x) * 4;
      EXPECT_NEAR(p[0], red * factor[x], 1e-4f) << "x=" << x << " y=" << y;
      EXPECT_NEAR(p[1], green * factor[x], 1e-4f) << "x=" << x << " y=" << y;
      EXPECT_NEAR(p[2], blue * factor[x], 1e-4f) << "x=" << x << " y=" << y;
    }
  }
  /* No leak through the frame's edges: the first column is wholly the row's half, the last wholly
   * the empty one, with nothing of the opposite edge bled in by a wrap. */
  EXPECT_NEAR(linear[0], red, 1e-6f);
  EXPECT_NEAR(linear[int64_t(height - 1) * width * 4 + (width - 1) * 4], 0.0f, 1e-6f);
}

/** The nearest read has no leak along the vertical seam either: a mask that won on the first rows
 * of its stack keeps whole rows on the wider frame, with no fractional row between the halves. */
TEST_F(PaintLayersGraphEvalTest, height_compare_mask_smaller_than_the_frame_leaks_not_vertically)
{
  const uchar neutral[4] = {128, 128, 128, 255};
  Image *below_height = add_image("BelowHeight", 4, neutral);
  const uchar white[4] = {255, 255, 255, 255};
  const uchar black[4] = {0, 0, 0, 255};
  Image *row_height = add_row_split_image("RowHeight", 4, white, black);
  const uchar row_rgba[4] = {200, 100, 50, 255};
  Image *color = add_image("RowColor", 8, row_rgba);
  for (Image *image : {below_height, row_height, color}) {
    make_image_data(image);
  }
  add_layer("BelowHeight", below_height, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *comparer = add_layer("Comparer", color);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, row_height));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_TRUE(layers.first().height_won != nullptr);
  ASSERT_EQ(layers.first().height_won->x, 4);

  int width = 0;
  int height = 0;
  ASSERT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
  ASSERT_EQ(width, 8);
  ASSERT_EQ(height, 8);

  paint_material_composite_height_won_resample_count_reset();
  Vector<float> linear(int64_t(width) * height * 4);
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  ASSERT_TRUE(BKE_paint_material_composite_eval_images_linear(
      layers, PaintMaterialCompositeLinearTarget::frame(linear, width), nullptr, nullptr, bottom));
  EXPECT_EQ(paint_material_composite_height_won_resample_count(), 1);

  /* The mask won on its first two source rows and lost on the last two. Target row y reads source
   * row floor(y / 2): the first four rows carry the row's colour and the last four the empty
   * bottom, with no fractional row at the seam. The colours are the map's bytes over 255. */
  const float red = 200.0f / 255.0f;
  const float row_factor[8] = {1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  for (int y = 0; y < height; y++) {
    for (const int x : IndexRange(width)) {
      const float *p = linear.data() + (int64_t(y) * width + x) * 4;
      EXPECT_NEAR(p[0], red * row_factor[y], 1e-4f) << "x=" << x << " y=" << y;
      EXPECT_NEAR(p[1], (100.0f / 255.0f) * row_factor[y], 1e-4f) << "x=" << x << " y=" << y;
      EXPECT_NEAR(p[2], (50.0f / 255.0f) * row_factor[y], 1e-4f) << "x=" << x << " y=" << y;
    }
  }
  /* No leak through the frame's edges: the first row is wholly the row's half, the last wholly the
   * empty one, with nothing of the opposite edge bled in by a wrap. */
  EXPECT_NEAR(linear[0], red, 1e-6f);
  EXPECT_NEAR(linear[int64_t(height - 1) * width * 4], 0.0f, 1e-6f);
}

/** A mask of exactly the frame's size reaches the evaluator as it was rendered: no resample. */
TEST_F(PaintLayersGraphEvalTest, height_compare_mask_of_the_frame_size_is_not_resampled)
{
  const uchar neutral[4] = {128, 128, 128, 255};
  Image *below_height = add_image("BelowHeight", 4, neutral);
  const uchar white[4] = {255, 255, 255, 255};
  const uchar black[4] = {0, 0, 0, 255};
  Image *row_height = add_split_image("RowHeight", 4, white, black);
  const uchar row_rgba[4] = {200, 100, 50, 255};
  Image *color = add_image("RowColor", 4, row_rgba);
  for (Image *image : {below_height, row_height, color}) {
    make_image_data(image);
  }
  add_layer("BelowHeight", below_height, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *comparer = add_layer("Comparer", color);
  EXPECT_NE(BKE_paint_layers_channel_add(*ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT), nullptr);
  EXPECT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, row_height));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *comparer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE));

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  ASSERT_TRUE(layers.first().height_won != nullptr);
  ASSERT_EQ(layers.first().height_won->x, 4);

  paint_material_composite_height_won_resample_count_reset();
  Vector<float> linear(4 * 4 * 4);
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  ASSERT_TRUE(BKE_paint_material_composite_eval_images_linear(
      layers, PaintMaterialCompositeLinearTarget::frame(linear, 4), nullptr, nullptr, bottom));
  EXPECT_EQ(paint_material_composite_height_won_resample_count(), 0);

  /* The mask reached the evaluator as rendered: the row's colour on the left half, the empty
   * bottom on the right. */
  for (int y = 0; y < 4; y++) {
    for (const int x : IndexRange(4)) {
      const float expected = (x < 2) ? 200.0f / 255.0f : 0.0f;
      const float *p = linear.data() + (int64_t(y) * 4 + x) * 4;
      EXPECT_NEAR(p[0], expected, 1e-4f) << "x=" << x << " y=" << y;
    }
  }
}

/** Inherit follows the row's ordinary blend through the effective-mode rule: Mix stays the ordinary
 * Mix (the same step and the GPU preview), Add and Subtract keep their meaning in the delta space,
 * and every other colour blend reads as Mix instead of digging a pit. */
TEST_F(PaintLayersGraphEvalTest, height_inherit_follows_the_rows_own_blend)
{
  Image *bottom = add_solid_image("Bottom", 77, 77, 77, 255);
  Image *top = add_solid_image("Top", 204, 204, 204, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top_layer, 0.7f));
  /* Explicit Inherit is the same as the mode never being set. */
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT));

  const float below = 77.0f / 255.0f;
  const float layer = 204.0f / 255.0f;
  /* The row's blend, the effective mode it must report, and the step that mode takes. */
  struct Case {
    int row_blend;
    int effective;
    int step;
  };
  const Case cases[] = {
      {MA_PAINT_LAYER_BLEND_MIX, MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT,
       MA_PAINT_LAYER_HEIGHT_BLEND_MIX},
      {MA_PAINT_LAYER_BLEND_ADD, MA_PAINT_LAYER_HEIGHT_BLEND_ADD,
       MA_PAINT_LAYER_HEIGHT_BLEND_ADD},
      {MA_PAINT_LAYER_BLEND_SUBTRACT, MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT,
       MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT},
      {MA_PAINT_LAYER_BLEND_MULTIPLY, MA_PAINT_LAYER_HEIGHT_BLEND_MIX,
       MA_PAINT_LAYER_HEIGHT_BLEND_MIX},
      {MA_PAINT_LAYER_BLEND_OVERLAY, MA_PAINT_LAYER_HEIGHT_BLEND_MIX,
       MA_PAINT_LAYER_HEIGHT_BLEND_MIX},
  };
  for (const Case &test_case : cases) {
    SCOPED_TRACE(::testing::Message() << "row blend=" << test_case.row_blend);
    ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, top_layer, test_case.row_blend));
    EXPECT_EQ(
        BKE_paint_layers_channel_height_blend_effective(*top_layer,
                                                        PAINT_MATERIAL_CHANNEL_HEIGHT),
        test_case.effective);
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
    const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
    EXPECT_NEAR(cpu.r,
                height_blend_step(test_case.step, below, layer, 1.0f, 0.7f),
                2e-3f);
  }
}

/** Two neutral records must not dig a pit through Inherit either, whatever the row's ordinary
 * blend: the effective mode maps Multiply and Overlay to the delta-space Mix, which is flat at the
 * neutral level. */
TEST_F(PaintLayersGraphEvalTest, height_inherit_neutral_rows_are_no_ops_for_every_row_blend)
{
  Image *bottom = add_solid_image("Bottom", 128, 128, 128, 255);
  Image *top = add_solid_image("Top", 128, 128, 128, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT));

  for (const int row_blend : {MA_PAINT_LAYER_BLEND_MIX,
                              MA_PAINT_LAYER_BLEND_ADD,
                              MA_PAINT_LAYER_BLEND_SUBTRACT,
                              MA_PAINT_LAYER_BLEND_MULTIPLY,
                              MA_PAINT_LAYER_BLEND_OVERLAY})
  {
    SCOPED_TRACE(::testing::Message() << "row blend=" << row_blend);
    ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, top_layer, row_blend));
    expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
    const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
    EXPECT_NEAR(cpu.r, PAINT_HEIGHT_NEUTRAL, 0.02f);
  }
}

/** A record authored at relief -1 is stored at the bottom of the map (0.0). Add lowers the stack by
 * its distance and Subtract raises it by the same: the sign lives in the stored value, the mode
 * only decides the direction. */
TEST_F(PaintLayersGraphEvalTest, height_negative_relief_adds_down_and_subtracts_up)
{
  Image *bottom = add_solid_float_image("Bottom", PAINT_HEIGHT_NEUTRAL, PAINT_HEIGHT_NEUTRAL, PAINT_HEIGHT_NEUTRAL, 1.0f);
  Image *top = add_solid_float_image("Top", 0.0f, 0.0f, 0.0f, 1.0f);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top_layer, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_ADD));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  const RGBA lowered = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(lowered.r, PAINT_HEIGHT_NEUTRAL - 0.5f * 0.5f, 2e-3f);

  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  const RGBA raised = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(raised.r, PAINT_HEIGHT_NEUTRAL + 0.5f * 0.5f, 2e-3f);
}

/** The scale is applied to the distance from the neutral level on both sides: the graph and the CPU
 * raise a record above the neutral and push one below it by the same factor. */
TEST_F(PaintLayersGraphEvalTest, height_scale_applies_to_both_signs)
{
  Image *bottom = add_solid_float_image("Bottom", PAINT_HEIGHT_NEUTRAL, PAINT_HEIGHT_NEUTRAL, PAINT_HEIGHT_NEUTRAL, 1.0f);
  Image *high = add_solid_float_image("High", 0.9f, 0.9f, 0.9f, 1.0f);
  Image *low = add_solid_float_image("Low", 0.1f, 0.1f, 0.1f, 1.0f);
  make_image_data(bottom);
  make_image_data(high);
  make_image_data(low);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top_layer = add_layer("Top", high, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_ADD));
  ASSERT_TRUE(
      BKE_paint_layers_channel_height_scale_set(*ma, *top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT,
                                                2.0f));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  const RGBA up = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(up.r, PAINT_HEIGHT_NEUTRAL + 2.0f * (0.9f - PAINT_HEIGHT_NEUTRAL), 5e-3f);

  BKE_paint_layers_channel_set_image(*ma, top_layer, PAINT_MATERIAL_CHANNEL_HEIGHT, low);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_HEIGHT);
  const RGBA down = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(down.r, PAINT_HEIGHT_NEUTRAL + 2.0f * (0.1f - PAINT_HEIGHT_NEUTRAL), 5e-3f);
}

/** \} */

}  // namespace blender::bke::tests
