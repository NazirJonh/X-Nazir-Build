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
  EXPECT_NEAR(cpu.r, 0.5f * 0.75f + 0.8f * 0.25f, 0.02f);
}

/** \} */

}  // namespace blender::bke::tests
