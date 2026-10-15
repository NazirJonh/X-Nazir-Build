/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Content corrections (Image source): a map laid over its row's colour before the row's own blend.
 * The generated graph and the CPU composite must agree on every case, and the closed-form cases pin
 * the formula both implement:
 *
 *   c_eff = lerp(c, blend(S, c), a)
 *   a'    = a + f * (1 - a)
 *   S'    = lerp(S, c_eff, f / a')
 *
 * which differs from a plain `lerp(S, c, f)` exactly where the row covers less than fully.
 */

#include <cmath>

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

namespace {

constexpr eMaterialPaintChannel kColor = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

}  // namespace

class PaintLayersCorrectionTest : public PaintLayersGraphEvalTest {
 public:
  /** A correction on \a row with \a image in \a channel. */
  MaterialPaintLayer *add_correction(MaterialPaintLayer &row,
                                     Image *image,
                                     const eMaterialPaintChannel channel = kColor,
                                     const char *name = "Correction")
  {
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, &row, name);
    EXPECT_NE(effect, nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, effect, channel, image));
    return effect;
  }

  /** A Fill correction on \a row laying \a color (scene linear) in Base Color. */
  MaterialPaintLayer *add_fill_correction(MaterialPaintLayer &row,
                                          const float color[4],
                                          const char *name = "Fill")
  {
    MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
        *ma, &row, name, MA_PAINT_LAYER_SOURCE_CONSTANT);
    EXPECT_NE(effect, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, color));
    return effect;
  }

  /** A Paint row with no map in \a channel and nothing painted: only corrections give it content. */
  MaterialPaintLayer *add_empty_row(const char *name, const eMaterialPaintChannel channel = kColor)
  {
    MaterialPaintLayer *row = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(row, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, row, channel), nullptr);
    return row;
  }
};

TEST_F(PaintLayersCorrectionTest, correction_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *effect = add_correction(*row, add_solid_image("Corr", 240, 20, 90, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.6f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, translucent_correction_over_a_translucent_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 128));
  add_correction(*row, add_solid_image("Corr", 240, 20, 90, 100));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.8f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, correction_on_an_unpainted_row_is_a_plain_over)
{
  /* A black bottom and a white half-covering correction: the layer is the correction's colour at
   * its own coverage, `f * 1`, not the `f * f` a blend with the row's empty black would give. */
  add_layer("Bottom", add_solid_image("Bottom", 0, 0, 0, 255));
  MaterialPaintLayer *row = add_empty_row("Empty");
  add_correction(*row, add_solid_image("Corr", 255, 255, 255, 128));

  expect_graph_matches_cpu(kColor);
  const RGBA cpu = cpu_pixel(kColor);
  const float f = 128.0f / 255.0f;
  EXPECT_NEAR(cpu.r, f, 2e-3f);
  EXPECT_NEAR(cpu.g, f, 2e-3f);
  EXPECT_NEAR(cpu.b, f, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, opaque_correction_on_an_unpainted_row_is_its_own_colour)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_empty_row("Empty");
  add_correction(*row, add_solid_image("Corr", 0, 0, 255, 255));

  expect_graph_matches_cpu(kColor);
  const RGBA cpu = cpu_pixel(kColor);
  EXPECT_NEAR(cpu.r, 0.0f, 2e-3f);
  EXPECT_NEAR(cpu.g, 0.0f, 2e-3f);
  EXPECT_NEAR(cpu.b, 1.0f, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, correction_on_a_fill_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  const float value[4] = {0.9f, 0.1f, 0.2f, 1.0f};
  MaterialPaintLayer *fill = add_constant_layer("Fill", value);
  add_correction(*fill, add_solid_image("Corr", 30, 160, 220, 180));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, two_corrections_lay_in_the_order_they_were_added)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 200));
  add_correction(*row, add_solid_image("First", 240, 20, 90, 150), kColor, "First");
  add_correction(*row, add_solid_image("Second", 20, 240, 90, 150), kColor, "Second");

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, a_hidden_correction_changes_nothing)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA plain = graph_pixel(kColor);

  MaterialPaintLayer *effect = add_correction(*row, add_solid_image("Corr", 0, 0, 255, 255));
  expect_graph_matches_cpu(kColor);
  const RGBA with = graph_pixel(kColor);
  EXPECT_GT(std::abs(with.b - plain.b) + std::abs(with.r - plain.r), 0.1f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, effect, false));
  expect_graph_matches_cpu(kColor);
  const RGBA hidden = graph_pixel(kColor);
  EXPECT_NEAR(hidden.r, plain.r, 2e-3f);
  EXPECT_NEAR(hidden.g, plain.g, 2e-3f);
  EXPECT_NEAR(hidden.b, plain.b, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, correction_opacity_is_a_value_edit)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *effect = add_correction(*row, add_solid_image("Corr", 240, 20, 90, 255));
  expect_graph_matches_cpu(kColor);
  const RGBA before = graph_pixel(kColor);

  /* Nothing is rebuilt: the correction's opacity is an input of its row's group. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.25f));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  expect_graph_matches_cpu(kColor);
  const RGBA after = graph_pixel(kColor);
  EXPECT_GT(std::abs(before.r - after.r) + std::abs(before.g - after.g), 0.05f);
}

TEST_F(PaintLayersCorrectionTest, correction_in_a_data_channel_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 64, 64, 64, 255);
  Image *top = add_solid_image("Top", 200, 200, 200, 200);
  Image *corr = add_solid_image("Corr", 20, 20, 20, 160);
  make_image_data(bottom);
  make_image_data(top);
  make_image_data(corr);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *row = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  add_correction(*row, corr, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersCorrectionTest, correction_in_the_normal_channel_combines_like_the_cpu)
{
  Image *base = add_solid_image("Base", 160, 128, 230, 255);
  Image *detail = add_solid_image("Detail", 100, 150, 220, 255);
  Image *corr = add_solid_image("Corr", 130, 100, 240, 200);
  make_image_data(base);
  make_image_data(detail);
  make_image_data(corr);
  add_layer("Base", base, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *row = add_layer("Detail", detail, PAINT_MATERIAL_CHANNEL_NORMAL);
  add_correction(*row, corr, PAINT_MATERIAL_CHANNEL_NORMAL);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
}

TEST_F(PaintLayersCorrectionTest, correction_on_a_row_of_an_isolating_folder_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 180));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 120));
  add_correction(*second, add_solid_image("Corr", 20, 240, 90, 140));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.7f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, correction_on_a_row_of_a_pass_through_folder_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 200));
  add_correction(*row, add_solid_image("Corr", 20, 240, 90, 140));
  MaterialPaintLayer *members[1] = {row};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  expect_graph_matches_cpu(kColor);

  /* Hiding the folder scales the correction's opacity too. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_over_a_row_is_a_plain_blend_by_its_opacity)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA plain = graph_pixel(kColor);

  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, red);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.4f));
  expect_graph_matches_cpu(kColor);

  /* The row covers fully, so the correction is `lerp(row, colour, opacity)`. */
  const RGBA with = graph_pixel(kColor);
  EXPECT_NEAR(with.r, plain.r + (1.0f - plain.r) * 0.4f, 2e-3f);
  EXPECT_NEAR(with.g, plain.g * 0.6f, 2e-3f);
  EXPECT_NEAR(with.b, plain.b * 0.6f, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_over_a_translucent_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 128));
  const float color[4] = {0.1f, 0.7f, 0.3f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, color);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.7f));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.8f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_on_an_unpainted_row_is_its_own_colour)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_empty_row("Empty");
  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  add_fill_correction(*row, blue);

  expect_graph_matches_cpu(kColor);
  const RGBA cpu = cpu_pixel(kColor);
  EXPECT_NEAR(cpu.r, 0.0f, 2e-3f);
  EXPECT_NEAR(cpu.g, 0.0f, 2e-3f);
  EXPECT_NEAR(cpu.b, 1.0f, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_on_a_fill_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  const float value[4] = {0.9f, 0.1f, 0.2f, 1.0f};
  MaterialPaintLayer *fill = add_constant_layer("Fill", value);
  const float color[4] = {0.1f, 0.2f, 0.9f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*fill, color);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.5f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_colour_is_a_value_edit)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const float first[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, first);
  expect_graph_matches_cpu(kColor);
  const RGBA before = graph_pixel(kColor);

  /* Nothing is rebuilt: the colour is an input of its row's group. */
  const float second[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, effect, second));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  expect_graph_matches_cpu(kColor);
  const RGBA after = graph_pixel(kColor);
  EXPECT_GT(std::abs(before.r - after.r) + std::abs(before.g - after.g), 0.3f);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_covers_by_its_opacity_whatever_its_alpha)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const float opaque[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, opaque);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.5f));
  const RGBA full = graph_pixel(kColor);

  const float faint[4] = {1.0f, 0.0f, 0.0f, 0.1f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(*ma, effect, kColor, faint));
  expect_graph_matches_cpu(kColor);
  const RGBA with_alpha = graph_pixel(kColor);
  EXPECT_NEAR(with_alpha.r, full.r, 1e-4f);
  EXPECT_NEAR(with_alpha.g, full.g, 1e-4f);
  EXPECT_NEAR(with_alpha.b, full.b, 1e-4f);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_reads_the_map_of_a_channel_that_holds_one)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, color);
  const RGBA flat = graph_pixel(kColor);

  /* The map covers by its own alpha where the colour covered fully, so a half-transparent one
   * lays half as much, and the colour is no longer read in that channel. */
  Image *map = add_solid_image("Map", 0, 0, 255, 128);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, effect, kColor, map));
  expect_graph_matches_cpu(kColor);
  const RGBA mapped = graph_pixel(kColor);
  EXPECT_GT(std::abs(mapped.b - flat.b), 0.1f);

  /* Taking the map away brings the colour back. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, effect, kColor, nullptr));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(graph_pixel(kColor).r, flat.r, 2e-3f);
  EXPECT_NEAR(graph_pixel(kColor).b, flat.b, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, a_hidden_fill_correction_changes_nothing)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA plain = graph_pixel(kColor);

  const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, blue);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, effect, false));
  expect_graph_matches_cpu(kColor);
  const RGBA hidden = graph_pixel(kColor);
  EXPECT_NEAR(hidden.r, plain.r, 2e-3f);
  EXPECT_NEAR(hidden.g, plain.g, 2e-3f);
  EXPECT_NEAR(hidden.b, plain.b, 2e-3f);
}

TEST_F(PaintLayersCorrectionTest, fill_and_paint_corrections_lay_in_the_order_they_were_added)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 200));
  const float color[4] = {0.8f, 0.3f, 0.1f, 1.0f};
  MaterialPaintLayer *fill = add_fill_correction(*row, color, "Fill");
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, fill, 0.5f));
  add_correction(*row, add_solid_image("Paint", 20, 240, 90, 150), kColor, "Paint");

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_in_a_data_channel_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 64, 64, 64, 255);
  Image *top = add_solid_image("Top", 200, 200, 200, 200);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *row = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  const float color[4] = {0.9f, 0.1f, 0.1f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, color);
  const float grey[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS, grey));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.6f));

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_is_left_out_of_the_normal_channel)
{
  Image *base = add_solid_image("Base", 160, 128, 230, 255);
  Image *detail = add_solid_image("Detail", 100, 150, 220, 255);
  make_image_data(base);
  make_image_data(detail);
  add_layer("Base", base, PAINT_MATERIAL_CHANNEL_NORMAL);
  MaterialPaintLayer *row = add_layer("Detail", detail, PAINT_MATERIAL_CHANNEL_NORMAL);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
  const RGBA plain = graph_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);

  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*row, red);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);

  /* A flat red is no normal: the generator builds nothing for it and the CPU carries nothing. */
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
  const RGBA with = graph_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_NEAR(with.r, plain.r, 1e-4f);
  EXPECT_NEAR(with.g, plain.g, 1e-4f);
  EXPECT_NEAR(with.b, plain.b, 1e-4f);
}

TEST_F(PaintLayersCorrectionTest, fill_correction_on_a_row_of_an_isolating_folder_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 180));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 120));
  const float color[4] = {0.1f, 0.9f, 0.4f, 1.0f};
  MaterialPaintLayer *effect = add_fill_correction(*second, color);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.6f));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.7f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCorrectionTest, a_map_less_correction_lays_nothing)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA plain = graph_pixel(kColor);

  /* Added, not painted: its records are transparent and have no map. */
  ASSERT_NE(BKE_paint_layers_correction_add(*ma, row, "Empty"), nullptr);
  expect_graph_matches_cpu(kColor);
  const RGBA after = graph_pixel(kColor);
  EXPECT_NEAR(after.r, plain.r, 1e-4f);
  EXPECT_NEAR(after.g, plain.g, 1e-4f);
  EXPECT_NEAR(after.b, plain.b, 1e-4f);
}

}  // namespace blender::bke::tests
