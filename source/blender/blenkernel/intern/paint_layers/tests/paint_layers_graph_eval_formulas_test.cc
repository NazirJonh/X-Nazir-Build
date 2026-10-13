/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The formulas of a Paint row as the generated graph evaluates them, checked against closed-form
 * numbers rather than against the CPU composite: a row lays its map over what is below by
 * `opacity * map_alpha`, a map-less value by `opacity` alone, and the Normal channel always ends on
 * a unit vector.
 *
 * Images are data (Non-Color) so a byte reads back as `byte / 255` with no colorspace in the way.
 */

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

static float lerp_f(const float a, const float b, const float t)
{
  return a + (b - a) * t;
}

TEST_F(PaintLayersGraphEvalTest, opacity_lerps_from_what_is_below_to_the_map)
{
  Image *image = add_solid_image("Map", 51, 102, 204, 255);
  make_image_data(image);
  MaterialPaintLayer *layer = add_layer("Row", image, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.25f));

  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_ROUGHNESS, bottom);
  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(graph.r, lerp_f(bottom[0], 51.0f / 255.0f, 0.25f), 1e-4f);
  EXPECT_NEAR(graph.g, lerp_f(bottom[1], 102.0f / 255.0f, 0.25f), 1e-4f);
  EXPECT_NEAR(graph.b, lerp_f(bottom[2], 204.0f / 255.0f, 0.25f), 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, map_alpha_scales_the_factor)
{
  Image *image = add_solid_image("Map", 255, 255, 255, 102);
  make_image_data(image);
  MaterialPaintLayer *layer = add_layer("Row", image, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));

  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_ROUGHNESS, bottom);
  /* 0.5 opacity times 0.4 alpha. */
  const float factor = 0.5f * (102.0f / 255.0f);
  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(graph.r, lerp_f(bottom[0], 1.0f, factor), 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, constant_value_covers_by_opacity_alone)
{
  /* The value's own alpha only decides that the row takes part; it does not scale the factor. */
  const float value[4] = {0.8f, 0.4f, 0.2f, 0.3f};
  MaterialPaintLayer *layer = add_constant_layer("Row", value, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));

  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(graph.r, lerp_f(bottom[0], 0.8f, 0.5f), 1e-4f);
  EXPECT_NEAR(graph.g, lerp_f(bottom[1], 0.4f, 0.5f), 1e-4f);
  EXPECT_NEAR(graph.b, lerp_f(bottom[2], 0.2f, 0.5f), 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, a_switched_off_row_shows_what_is_below)
{
  Image *image = add_solid_image("Map", 255, 255, 255, 255);
  make_image_data(image);
  MaterialPaintLayer *layer = add_layer("Row", image, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));

  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_ROUGHNESS, bottom);
  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(graph.r, bottom[0], 1e-4f);
  EXPECT_NEAR(graph.g, bottom[1], 1e-4f);
  EXPECT_NEAR(graph.b, bottom[2], 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, normal_channel_ends_on_a_unit_vector)
{
  Image *image = add_solid_image("Normal", 230, 128, 200, 255);
  make_image_data(image);
  MaterialPaintLayer *layer = add_layer("Row", image, PAINT_MATERIAL_CHANNEL_NORMAL);
  /* A partial factor shortens the interpolated vector; the chain restores its length. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.3f));

  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  const float x = graph.r * 2.0f - 1.0f;
  const float y = graph.g * 2.0f - 1.0f;
  const float z = graph.b * 2.0f - 1.0f;
  EXPECT_NEAR(x * x + y * y + z * z, 1.0f, 1e-3f);
}

}  // namespace blender::bke::tests
