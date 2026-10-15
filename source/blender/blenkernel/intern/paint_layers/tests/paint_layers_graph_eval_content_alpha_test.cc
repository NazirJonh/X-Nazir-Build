/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The content alpha of the generated graph against the CPU composite.
 *
 * The fourth component of a channel's result is the alpha of what the rows lay: a map's alpha, a
 * constant's, raised by each correction by the over model, and laid by the row's factor like the
 * colour. The shader never reads it; a bake of the graph into a map and the CPU composite do, and
 * they have to agree, which these tests keep honest. The colour channels are compared by the other
 * graph-eval files.
 */

#include <cmath>

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

namespace {

constexpr eMaterialPaintChannel kColor = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

}  // namespace

class PaintLayersContentAlphaTest : public PaintLayersGraphEvalTest {
 public:
  /** The graph's and the CPU's alpha agree at the pixel, and so does the colour. */
  void expect_alpha_matches_cpu(const eMaterialPaintChannel channel, const float tolerance = 2e-3f)
  {
    const RGBA graph = graph_pixel(channel);
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.a, cpu.a, tolerance);
    EXPECT_NEAR(graph.r, cpu.r, tolerance);
    EXPECT_NEAR(graph.g, cpu.g, tolerance);
    EXPECT_NEAR(graph.b, cpu.b, tolerance);
  }

  MaterialPaintLayer *add_correction(MaterialPaintLayer &row, Image *image)
  {
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, &row, "Correction");
    EXPECT_NE(effect, nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, effect, kColor, image));
    return effect;
  }
};

TEST_F(PaintLayersContentAlphaTest, a_translucent_map_lays_its_alpha)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 128));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.8f));

  expect_alpha_matches_cpu(kColor);
  /* Not the colour chain's own alpha: the row lays its map's alpha at its factor. */
  const float factor = 0.8f * (128.0f / 255.0f);
  const float expected = 1.0f + (128.0f / 255.0f - 1.0f) * factor;
  EXPECT_NEAR(graph_pixel(kColor).a, expected, 2e-3f);
}

TEST_F(PaintLayersContentAlphaTest, a_constant_lays_the_alpha_of_its_value)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  const float value[4] = {0.9f, 0.1f, 0.2f, 0.5f};
  MaterialPaintLayer *top = add_constant_layer("Top", value);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top, 0.6f));

  expect_alpha_matches_cpu(kColor);

  /* The alpha is an input of the row's group, so editing it is a value edit. */
  const float other[4] = {0.9f, 0.1f, 0.2f, 0.2f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(*ma, top, kColor, other));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  expect_alpha_matches_cpu(kColor);
}

TEST_F(PaintLayersContentAlphaTest, a_blend_other_than_mix_keeps_the_alpha_below)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", add_solid_image("Top", 30, 160, 220, 100));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, top, MA_PAINT_LAYER_BLEND_MULTIPLY));

  expect_alpha_matches_cpu(kColor);
  EXPECT_NEAR(graph_pixel(kColor).a, 1.0f, 2e-3f);
}

TEST_F(PaintLayersContentAlphaTest, a_correction_raises_the_alpha_by_the_over_model)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 100));
  add_correction(*row, add_solid_image("Corr", 240, 20, 90, 150));

  expect_alpha_matches_cpu(kColor);
}

TEST_F(PaintLayersContentAlphaTest, a_fill_correction_raises_the_alpha_by_its_opacity)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 100));
  const RGBA plain = graph_pixel(kColor);

  /* The colour's own alpha is no part of it: a Fill covers by its opacity alone. */
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);
  const float color[4] = {0.9f, 0.2f, 0.1f, 0.25f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, color));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.5f));

  expect_alpha_matches_cpu(kColor);
  EXPECT_GT(graph_pixel(kColor).a, plain.a);
}

TEST_F(PaintLayersContentAlphaTest, a_correction_on_an_unpainted_row_brings_its_alpha_in)
{
  add_layer("Bottom", add_solid_image("Bottom", 0, 0, 0, 255));
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Empty", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, kColor), nullptr);
  add_correction(*row, add_solid_image("Corr", 255, 255, 255, 128));

  expect_alpha_matches_cpu(kColor);
}

TEST_F(PaintLayersContentAlphaTest, rows_of_an_isolating_folder_accumulate_their_alpha)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 140));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 110));
  add_correction(*second, add_solid_image("Corr", 20, 240, 90, 90));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.7f));

  expect_alpha_matches_cpu(kColor);
}

TEST_F(PaintLayersContentAlphaTest, a_non_mix_row_in_an_isolating_folder_reads_the_straight_alpha)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 140));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 110));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, second, MA_PAINT_LAYER_BLEND_MULTIPLY));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.6f));

  expect_alpha_matches_cpu(kColor);
}

TEST_F(PaintLayersContentAlphaTest, a_pass_through_folder_changes_no_alpha)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 140));
  add_correction(*row, add_solid_image("Corr", 20, 240, 90, 90));
  MaterialPaintLayer *members[1] = {row};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  expect_alpha_matches_cpu(kColor);
  const RGBA with_folder = graph_pixel(kColor);

  ASSERT_TRUE(BKE_paint_layers_ungroup(*ma, folder));
  expect_alpha_matches_cpu(kColor);
  EXPECT_NEAR(graph_pixel(kColor).a, with_folder.a, 1e-4f);
}

TEST_F(PaintLayersContentAlphaTest, a_scalar_channel_tracks_its_alpha_too)
{
  Image *bottom = add_solid_image("Bottom", 64, 64, 64, 255);
  Image *top = add_solid_image("Top", 200, 200, 200, 128);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  add_layer("Top", top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  expect_alpha_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

}  // namespace blender::bke::tests
