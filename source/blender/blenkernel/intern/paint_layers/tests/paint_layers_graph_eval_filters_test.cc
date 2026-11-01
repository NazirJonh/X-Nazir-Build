/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Filter corrections: a change to the colour a row has so far, not a map laid over it. The
 * generated graph and the CPU composite must agree on every case, and a filter must leave the row's
 * coverage alone.
 */

#include <cmath>

#include "paint_layers_graph_eval_test_fixture.hh"

#include "BKE_paint_layers_edit.hh"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

namespace {

constexpr eMaterialPaintChannel kColor = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

}  // namespace

class PaintLayersFilterTest : public PaintLayersGraphEvalTest {
 public:
  MaterialPaintLayer *add_invert(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_INVERT);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }

  /**
   * The weight the row's mask gives the row, read from the stack: the result is linear in it,
   * `bottom + (row - bottom) * M`, so two readings of the same stack with the row hidden and shown
   * give `M` relative to a reading at a known one.
   */
  float row_weight(MaterialPaintLayer &row, const RGBA &bottom, const RGBA &row_alone)
  {
    EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, &row, true));
    const RGBA shown = cpu_pixel(kColor);
    return (shown.r - bottom.r) / (row_alone.r - bottom.r);
  }
};

class PaintLayersLevelsTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_levels(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_LEVELS);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }

  static void params_set(MaterialPaintLayer &filter,
                         const float in_black,
                         const float in_white,
                         const float gamma,
                         const float out_black,
                         const float out_white)
  {
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_LEVELS_IN_BLACK, in_black));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_LEVELS_IN_WHITE, in_white));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_LEVELS_GAMMA, gamma));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_LEVELS_OUT_BLACK, out_black));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_LEVELS_OUT_WHITE, out_white));
  }
};

TEST_F(PaintLayersLevelsTest, levels_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *levels = add_levels(*row, 0.7f);
  params_set(*levels, 0.2f, 0.8f, 1.6f, 0.1f, 0.9f);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersLevelsTest, default_parameters_leave_the_colour_alone)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  add_levels(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r, 2e-3f);
  EXPECT_NEAR(after.g, before.g, 2e-3f);
  EXPECT_NEAR(after.b, before.b, 2e-3f);
}

TEST_F(PaintLayersLevelsTest, editing_a_parameter_is_a_value_edit_both_sides_follow)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *levels = add_levels(*row);
  params_set(*levels, 0.1f, 0.9f, 1.0f, 0.0f, 1.0f);
  expect_graph_matches_cpu(kColor);
  const RGBA before = graph_pixel(kColor);
  bNodeTree *tree = ma->paint_layers_tree;

  /* The graph keeps its shape and takes the new value; the CPU composite reads the same one. */
  EXPECT_TRUE(paint_layer_kind_param_set(*levels, PAINT_LAYER_LEVELS_GAMMA, 2.5f));
  expect_graph_matches_cpu(kColor);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  const RGBA after = graph_pixel(kColor);
  EXPECT_GT(std::abs(after.g - before.g) + std::abs(after.b - before.b), 1e-3f);
}

TEST_F(PaintLayersLevelsTest, a_parameter_is_clamped_to_its_range)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *levels = add_levels(*row);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*levels, PAINT_LAYER_LEVELS_GAMMA), 1.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*levels, PAINT_LAYER_LEVELS_GAMMA, 0.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*levels, PAINT_LAYER_LEVELS_GAMMA), 0.01f);
  EXPECT_FALSE(paint_layer_kind_param_set(*levels, 99, 1.0f));
}

class PaintLayersBrightnessContrastTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_brightness_contrast(MaterialPaintLayer &row,
                                              const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }

  static void params_set(MaterialPaintLayer &filter,
                         const float brightness,
                         const float contrast)
  {
    EXPECT_TRUE(paint_layer_kind_param_set(
        filter, PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS, brightness));
    EXPECT_TRUE(
        paint_layer_kind_param_set(filter, PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST, contrast));
  }
};

TEST_F(PaintLayersBrightnessContrastTest, brightness_contrast_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *bc = add_brightness_contrast(*row, 0.7f);
  /* The dark channels go below zero, so both sides must clamp them there. */
  params_set(*bc, -0.5f, 0.5f);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersBrightnessContrastTest, default_parameters_leave_the_colour_alone)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  add_brightness_contrast(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r, 2e-3f);
  EXPECT_NEAR(after.g, before.g, 2e-3f);
  EXPECT_NEAR(after.b, before.b, 2e-3f);
}

TEST_F(PaintLayersBrightnessContrastTest, a_parameter_is_clamped_to_its_range)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *bc = add_brightness_contrast(*row);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS), 0.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(
      *bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS, 2.0f));
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS), 1.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(
      *bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST, -2.0f));
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST), -1.0f);
}

class PaintLayersColorRampTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_color_ramp(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_COLOR_RAMP);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }

  /** A red-to-blue band, so a grey input comes out coloured and a wrong index is obvious. */
  static void set_red_to_blue(MaterialPaintLayer &filter)
  {
    MaterialPaintLayerRamp *ramp = BKE_paint_layers_ramp_ensure(filter);
    ASSERT_NE(ramp, nullptr);
    ColorBand &band = ramp->band;
    band.tot = 2;
    band.ipotype = COLBAND_INTERP_LINEAR;
    band.data[0] = {};
    band.data[0].pos = 0.0f;
    band.data[0].r = 1.0f;
    band.data[0].a = 1.0f;
    band.data[1] = {};
    band.data[1].pos = 1.0f;
    band.data[1].b = 1.0f;
    band.data[1].a = 1.0f;
  }
};

TEST_F(PaintLayersColorRampTest, colour_ramp_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *ramp = add_color_ramp(*row, 0.8f);
  set_red_to_blue(*ramp);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersColorRampTest, affect_alpha_multiplies_the_strength_by_the_stop_alpha)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *ramp = add_color_ramp(*row, 0.8f);
  set_red_to_blue(*ramp);
  expect_graph_matches_cpu(kColor);
  const RGBA ignored = cpu_pixel(kColor);

  /* Stops of different alpha, so the found colour's alpha is neither 0 nor 1. */
  ramp->ramp->band.data[0].a = 0.2f;
  ramp->ramp->band.data[1].a = 0.9f;
  expect_graph_matches_cpu(kColor);
  const RGBA unused_alpha = cpu_pixel(kColor);
  EXPECT_FLOAT_EQ(unused_alpha.r, ignored.r);
  EXPECT_FLOAT_EQ(unused_alpha.g, ignored.g);
  EXPECT_FLOAT_EQ(unused_alpha.b, ignored.b);

  ASSERT_TRUE(paint_layer_kind_param_int_set(*ramp, PAINT_LAYER_COLOR_RAMP_AFFECT_ALPHA, 1));
  expect_graph_matches_cpu(kColor);
  const RGBA used_alpha = cpu_pixel(kColor);
  EXPECT_GT(std::abs(used_alpha.r - ignored.r) + std::abs(used_alpha.b - ignored.b), 1e-3f);

  ASSERT_TRUE(paint_layer_kind_param_int_set(*ramp, PAINT_LAYER_COLOR_RAMP_AFFECT_ALPHA, 0));
  expect_graph_matches_cpu(kColor);
  const RGBA back = cpu_pixel(kColor);
  EXPECT_FLOAT_EQ(back.r, ignored.r);
  EXPECT_FLOAT_EQ(back.b, ignored.b);
}

TEST_F(PaintLayersColorRampTest, affect_alpha_adjustment_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Adjustment;
  params.filter = MA_PAINT_LAYER_FILTER_COLOR_RAMP;
  MaterialPaintLayer *adjustment = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(adjustment, nullptr);
  set_red_to_blue(*adjustment);
  adjustment->ramp->band.data[0].a = 0.2f;
  adjustment->ramp->band.data[1].a = 0.9f;
  ASSERT_TRUE(
      paint_layer_kind_param_int_set(*adjustment, PAINT_LAYER_COLOR_RAMP_AFFECT_ALPHA, 1));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersColorRampTest, editing_the_band_or_a_stop_tags_the_material)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_color_ramp(*row);
  MaterialPaintLayerRamp *ramp = BKE_paint_layers_ramp_ensure(*filter);
  ASSERT_NE(ramp, nullptr);

  EXPECT_TRUE(BKE_paint_layers_ramp_edited(*ma, &ramp->band));
  EXPECT_TRUE(BKE_paint_layers_ramp_edited(*ma, &ramp->band.data[0]));
  EXPECT_FALSE(BKE_paint_layers_ramp_edited(*ma, &row->mapping));
}

class PaintLayersPosterizeTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_posterize(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_POSTERIZE);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }
};

TEST_F(PaintLayersPosterizeTest, posterize_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *posterize = add_posterize(*row, 0.7f);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 5));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersPosterizeTest, two_steps_round_every_channel_to_an_endpoint)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *posterize = add_posterize(*row);
  /* `k = steps - 1 = 1`: every channel rounds to the nearer of 0 and 1. */
  EXPECT_TRUE(paint_layer_kind_param_int_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 2));

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  const auto an_endpoint = [](const float value) {
    return std::abs(value) < 2e-3f || std::abs(value - 1.0f) < 2e-3f;
  };
  EXPECT_TRUE(an_endpoint(after.r));
  EXPECT_TRUE(an_endpoint(after.g));
  EXPECT_TRUE(an_endpoint(after.b));
}

TEST_F(PaintLayersPosterizeTest, white_and_black_survive_a_full_strength_posterize)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 255, 255, 255, 255));
  add_posterize(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA white = cpu_pixel(kColor);
  EXPECT_NEAR(white.r, 1.0f, 2e-3f);
  EXPECT_NEAR(white.g, 1.0f, 2e-3f);
  EXPECT_NEAR(white.b, 1.0f, 2e-3f);

  MaterialPaintLayer *black = add_layer("Black", add_solid_image("Black", 0, 0, 0, 255));
  add_posterize(*black);

  expect_graph_matches_cpu(kColor);
  const RGBA dark = cpu_pixel(kColor);
  EXPECT_NEAR(dark.r, 0.0f, 2e-3f);
  EXPECT_NEAR(dark.g, 0.0f, 2e-3f);
  EXPECT_NEAR(dark.b, 0.0f, 2e-3f);
}

TEST_F(PaintLayersPosterizeTest, the_step_count_is_an_integer_clamped_to_its_range)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *posterize = add_posterize(*row);
  EXPECT_EQ(paint_layer_kind_param_type(*posterize, PAINT_LAYER_POSTERIZE_STEPS),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_int_get(*posterize, PAINT_LAYER_POSTERIZE_STEPS), 4);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 0));
  EXPECT_EQ(paint_layer_kind_param_int_get(*posterize, PAINT_LAYER_POSTERIZE_STEPS), 2);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 100));
  EXPECT_EQ(paint_layer_kind_param_int_get(*posterize, PAINT_LAYER_POSTERIZE_STEPS), 64);
  /* A float write to an integer parameter is refused. */
  EXPECT_FALSE(paint_layer_kind_param_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 8.0f));
}

class PaintLayersMathTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_math(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_MATH);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }

  /** One operand for every channel: what the tests that do not care about channels mean. */
  static void params_set(MaterialPaintLayer &filter, const int operation, const float operand)
  {
    params_set_channels(filter, operation, operand, operand, operand);
  }

  static void params_set_channels(MaterialPaintLayer &filter,
                                  const int operation,
                                  const float r,
                                  const float g,
                                  const float b)
  {
    EXPECT_TRUE(paint_layer_kind_param_int_set(filter, PAINT_LAYER_MATH_OPERATION, operation));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_MATH_OPERAND_R, r));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_MATH_OPERAND_G, g));
    EXPECT_TRUE(paint_layer_kind_param_set(filter, PAINT_LAYER_MATH_OPERAND_B, b));
  }

  static void clamp_set(MaterialPaintLayer &filter, const bool on)
  {
    EXPECT_TRUE(paint_layer_kind_param_bool_set(filter, PAINT_LAYER_MATH_CLAMP, on));
  }
};

TEST_F(PaintLayersMathTest, every_operation_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row, 0.7f);
  /* One operand per channel, so a per-channel mix-up cannot read as an agreement. */
  for (const int operation : IndexRange(int(PAINT_LAYER_MATH_OP_LOGARITHM) + 1)) {
    params_set_channels(*math, operation, 0.5f, 2.0f, 0.25f);
    expect_graph_matches_cpu(kColor);
  }
}

TEST_F(PaintLayersMathTest, clamping_folds_the_result_into_the_unit_range)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 220, 230, 250, 255));
  MaterialPaintLayer *math = add_math(*row);
  /* A bright row plus a ninth reads above one everywhere: unclamped both sides read the overshoot,
   * clamped the node's Use Clamp folds them onto one. */
  params_set(*math, PAINT_LAYER_MATH_OP_ADD, 0.9f);
  expect_graph_matches_cpu(kColor);
  const RGBA unclamped = cpu_pixel(kColor);
  EXPECT_GT(unclamped.r, 1.0f);
  EXPECT_GT(unclamped.g, 1.0f);
  EXPECT_GT(unclamped.b, 1.0f);

  clamp_set(*math, true);
  expect_graph_matches_cpu(kColor);
  const RGBA clamped = cpu_pixel(kColor);
  EXPECT_NEAR(clamped.r, 1.0f, 2e-3f);
  EXPECT_NEAR(clamped.g, 1.0f, 2e-3f);
  EXPECT_NEAR(clamped.b, 1.0f, 2e-3f);
}

TEST_F(PaintLayersMathTest, a_division_by_a_zero_operand_reads_zero_in_that_channel)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row);
  /* The red operand is the zero one, so only the red channel folds to zero. */
  params_set_channels(*math, PAINT_LAYER_MATH_OP_DIVIDE, 0.0f, 2.0f, 0.5f);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 0.0f, 2e-3f);
  EXPECT_GT(after.g, 0.0f);
  EXPECT_GT(after.b, 0.0f);
}

TEST_F(PaintLayersMathTest, a_power_of_a_negative_base_under_a_fraction_reads_zero)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *down = add_math(*row);
  /* The subtract digs every channel below zero; the fractional power of a negative base is the
   * node's own refusal and reads zero, on both sides. */
  params_set(*down, PAINT_LAYER_MATH_OP_SUBTRACT, 5.0f);
  MaterialPaintLayer *power = add_math(*row);
  params_set(*power, PAINT_LAYER_MATH_OP_POWER, 0.5f);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 0.0f, 2e-3f);
  EXPECT_NEAR(after.g, 0.0f, 2e-3f);
  EXPECT_NEAR(after.b, 0.0f, 2e-3f);
}

TEST_F(PaintLayersMathTest, a_logarithm_with_a_non_positive_base_reads_zero_in_that_channel)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row);
  /* The red base is the non-positive one, so only the red channel folds to zero. */
  params_set_channels(*math, PAINT_LAYER_MATH_OP_LOGARITHM, -2.0f, 2.0f, 2.0f);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 0.0f, 2e-3f);
  EXPECT_GT(after.g, 0.0f);
  EXPECT_GT(after.b, 0.0f);
}

TEST_F(PaintLayersMathTest, a_logarithm_of_a_non_positive_argument_reads_zero)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *down = add_math(*row);
  /* The subtract digs every channel below zero: a logarithm of it is the node's own refusal. */
  params_set(*down, PAINT_LAYER_MATH_OP_SUBTRACT, 5.0f);
  MaterialPaintLayer *log = add_math(*row);
  params_set(*log, PAINT_LAYER_MATH_OP_LOGARITHM, 2.0f);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 0.0f, 2e-3f);
  EXPECT_NEAR(after.g, 0.0f, 2e-3f);
  EXPECT_NEAR(after.b, 0.0f, 2e-3f);
}

TEST_F(PaintLayersMathTest, editing_the_operand_is_a_value_edit_both_sides_follow)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row);
  params_set(*math, PAINT_LAYER_MATH_OP_MULTIPLY, 0.5f);
  expect_graph_matches_cpu(kColor);
  const RGBA before = graph_pixel(kColor);
  bNodeTree *tree = ma->paint_layers_tree;

  /* The graph keeps its shape and takes the new value; the CPU composite reads the same one. */
  params_set(*math, PAINT_LAYER_MATH_OP_MULTIPLY, 0.8f);
  expect_graph_matches_cpu(kColor);
  EXPECT_EQ(ma->paint_layers_tree, tree);
  const RGBA after = graph_pixel(kColor);
  EXPECT_GT(std::abs(after.g - before.g) + std::abs(after.b - before.b), 1e-3f);
}

TEST_F(PaintLayersMathTest, the_parameters_are_clamped_to_their_ranges)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*math, PAINT_LAYER_MATH_OPERAND_R), 0.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_R, 20.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*math, PAINT_LAYER_MATH_OPERAND_R), 10.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*math, PAINT_LAYER_MATH_OPERAND_G), 0.0f);
  EXPECT_EQ(paint_layer_kind_param_type(*math, PAINT_LAYER_MATH_OPERATION),
            PaintLayerKindParamType::Enum);
  EXPECT_EQ(paint_layer_kind_param_type(*math, PAINT_LAYER_MATH_CLAMP),
            PaintLayerKindParamType::Bool);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*math, PAINT_LAYER_MATH_OPERATION, 99));
  EXPECT_EQ(paint_layer_kind_param_int_get(*math, PAINT_LAYER_MATH_OPERATION),
            PAINT_LAYER_MATH_OP_LOGARITHM);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*math, PAINT_LAYER_MATH_OPERATION, -3));
  EXPECT_EQ(paint_layer_kind_param_int_get(*math, PAINT_LAYER_MATH_OPERATION),
            PAINT_LAYER_MATH_OP_ADD);
  /* The clamp is a Boolean now: a write lands on one of its two ends, and the float value the
   * composite and the graph read folds it. The integer API is not its writer. */
  EXPECT_FALSE(paint_layer_kind_param_bool_get(*math, PAINT_LAYER_MATH_CLAMP));
  EXPECT_TRUE(paint_layer_kind_param_bool_set(*math, PAINT_LAYER_MATH_CLAMP, true));
  EXPECT_TRUE(paint_layer_kind_param_bool_get(*math, PAINT_LAYER_MATH_CLAMP));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*math, PAINT_LAYER_MATH_CLAMP), 1.0f);
  EXPECT_FALSE(paint_layer_kind_param_int_set(*math, PAINT_LAYER_MATH_CLAMP, 7));
  EXPECT_TRUE(paint_layer_kind_param_bool_get(*math, PAINT_LAYER_MATH_CLAMP));
  EXPECT_TRUE(paint_layer_kind_param_bool_set(*math, PAINT_LAYER_MATH_CLAMP, false));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*math, PAINT_LAYER_MATH_CLAMP), 0.0f);
}

/** The `.PL Layer <name>` group of the generated tree in \a bmain, or null. */
static bNodeTree *math_layer_tree_find(Main &bmain, const char *layer_name)
{
  char full[96];
  SNPRINTF(full, ".PL Layer %s", layer_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

/** How many of \a group's Math nodes run \a operation with the clamp flag \a clamp. */
static int math_group_count_op(bNodeTree &group, const int operation, const int clamp)
{
  int count = 0;
  for (bNode &node : group.nodes) {
    if (node.type_legacy == SH_NODE_MATH && node.custom1 == operation &&
        (node.custom2 & SHD_MATH_CLAMP) == clamp)
    {
      count++;
    }
  }
  return count;
}

TEST_F(PaintLayersMathTest, changing_the_operation_moves_the_hash_and_rebuilds_the_group)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row);
  params_set(*math, PAINT_LAYER_MATH_OP_ADD, 0.5f);
  expect_graph_matches_cpu(kColor);

  /* The operation decides which Math node each channel's chain is built from, so it is in the
   * topology hash like the Color Ramp's band: the edit moves the hash and the rebuilt group really
   * carries the new nodes, which is what keeps a stale shape from shading. */
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t as_add = paint_layers::paint_layers_layer_topology_hash(
      *ma, *row, wired, forms);
  bNodeTree *group = math_layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(math_group_count_op(*group, NODE_MATH_ADD, 0), 3);

  ASSERT_TRUE(paint_layer_kind_param_int_set(
      *math, PAINT_LAYER_MATH_OPERATION, PAINT_LAYER_MATH_OP_MULTIPLY));
  const uint64_t as_multiply = paint_layers::paint_layers_layer_topology_hash(
      *ma, *row, wired, forms);
  EXPECT_NE(as_multiply, as_add);
  expect_graph_matches_cpu(kColor);
  group = math_layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(math_group_count_op(*group, NODE_MATH_MULTIPLY, 0), 3);
  EXPECT_EQ(math_group_count_op(*group, NODE_MATH_ADD, 0), 0);

  /* The operands are inputs: the hash holds and the graph keeps its shape, the values sync. */
  params_set(*math, PAINT_LAYER_MATH_OP_MULTIPLY, 0.8f);
  EXPECT_EQ(paint_layers::paint_layers_layer_topology_hash(*ma, *row, wired, forms), as_multiply);
  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersMathTest, turning_the_clamp_on_moves_the_hash_and_sets_the_nodes_flag)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *math = add_math(*row);
  params_set(*math, PAINT_LAYER_MATH_OP_ADD, 0.5f);
  expect_graph_matches_cpu(kColor);

  /* The clamp decides whether the scalar Math nodes carry their Use Clamp flag, so it is in the
   * topology hash with the operation: an edit of it rebuilds the group. */
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t unclamped = paint_layers::paint_layers_layer_topology_hash(
      *ma, *row, wired, forms);
  bNodeTree *group = math_layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(math_group_count_op(*group, NODE_MATH_ADD, 0), 3);
  ASSERT_EQ(math_group_count_op(*group, NODE_MATH_ADD, SHD_MATH_CLAMP), 0);

  clamp_set(*math, true);
  const uint64_t clamped = paint_layers::paint_layers_layer_topology_hash(
      *ma, *row, wired, forms);
  EXPECT_NE(clamped, unclamped);
  expect_graph_matches_cpu(kColor);
  group = math_layer_tree_find(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(math_group_count_op(*group, NODE_MATH_ADD, SHD_MATH_CLAMP), 3);
  EXPECT_EQ(math_group_count_op(*group, NODE_MATH_ADD, 0), 0);
}

class PaintLayersThresholdTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_threshold(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_THRESHOLD);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }
};

TEST_F(PaintLayersThresholdTest, threshold_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *threshold = add_threshold(*row, 0.7f);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 0.4f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersThresholdTest, a_dark_row_thresholds_to_black_and_white_stays_white)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 50, 50, 50, 255));
  MaterialPaintLayer *threshold = add_threshold(*row);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 0.5f));

  expect_graph_matches_cpu(kColor);
  /* A dark grey is far below the threshold: every channel is off. */
  const RGBA dark = cpu_pixel(kColor);
  EXPECT_NEAR(dark.r, 0.0f, 2e-3f);
  EXPECT_NEAR(dark.g, 0.0f, 2e-3f);
  EXPECT_NEAR(dark.b, 0.0f, 2e-3f);

  MaterialPaintLayer *white = add_layer("White", add_solid_image("White", 255, 255, 255, 255));
  add_threshold(*white);

  expect_graph_matches_cpu(kColor);
  const RGBA bright = cpu_pixel(kColor);
  EXPECT_NEAR(bright.r, 1.0f, 2e-3f);
  EXPECT_NEAR(bright.g, 1.0f, 2e-3f);
  EXPECT_NEAR(bright.b, 1.0f, 2e-3f);
}

TEST_F(PaintLayersThresholdTest, the_threshold_is_clamped_to_its_range)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *threshold = add_threshold(*row);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD), 0.5f);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 5.0f));
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD), 1.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, -1.0f));
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD), 0.0f);
}

class PaintLayersHslTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_hsl(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(*ma, &row, MA_PAINT_LAYER_FILTER_HSL);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }
};

TEST_F(PaintLayersHslTest, hsl_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *hsl = add_hsl(*row, 0.8f);
  EXPECT_TRUE(paint_layer_kind_param_set(*hsl, PAINT_LAYER_HSL_HUE, 0.65f));
  EXPECT_TRUE(paint_layer_kind_param_set(*hsl, PAINT_LAYER_HSL_SATURATION, 1.4f));
  EXPECT_TRUE(paint_layer_kind_param_set(*hsl, PAINT_LAYER_HSL_VALUE, 0.7f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersHslTest, default_parameters_leave_the_colour_alone)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  add_hsl(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r, 2e-3f);
  EXPECT_NEAR(after.g, before.g, 2e-3f);
  EXPECT_NEAR(after.b, before.b, 2e-3f);
}

TEST_F(PaintLayersHslTest, zero_saturation_makes_the_colour_grey_and_a_value_edit_follows)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *hsl = add_hsl(*row);
  expect_graph_matches_cpu(kColor);

  EXPECT_TRUE(paint_layer_kind_param_set(*hsl, PAINT_LAYER_HSL_SATURATION, 0.0f));
  expect_graph_matches_cpu(kColor);
  const RGBA grey = cpu_pixel(kColor);
  EXPECT_NEAR(grey.r, grey.g, 2e-3f);
  EXPECT_NEAR(grey.g, grey.b, 2e-3f);
}

class PaintLayersAdjustmentTest : public PaintLayersFilterTest {
 public:
  /** An adjustment of \a filter on top of the stack (or into \a anchor), or null when refused. */
  MaterialPaintLayer *add_adjustment(const eMaterialPaintLayerFilter filter,
                                     const float opacity = 1.0f,
                                     MaterialPaintLayer *anchor = nullptr)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Adjustment;
    params.filter = filter;
    params.anchor = anchor;
    MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
    if (row != nullptr) {
      EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, row, opacity));
    }
    return row;
  }
};

TEST_F(PaintLayersAdjustmentTest, invert_adjustment_over_a_stack_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 200));
  ASSERT_NE(add_adjustment(MA_PAINT_LAYER_FILTER_INVERT, 0.6f), nullptr);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersAdjustmentTest, full_invert_adjustment_is_the_complement_of_the_stack)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  ASSERT_NE(add_adjustment(MA_PAINT_LAYER_FILTER_INVERT), nullptr);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 1.0f - before.r, 2e-3f);
  EXPECT_NEAR(after.g, 1.0f - before.g, 2e-3f);
  EXPECT_NEAR(after.b, 1.0f - before.b, 2e-3f);
}

TEST_F(PaintLayersAdjustmentTest, an_adjustment_only_changes_what_is_below_it)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *adjustment = add_adjustment(MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(adjustment, nullptr);
  add_layer("Top", add_solid_image("Top", 30, 160, 220, 255));
  /* An opaque layer above covers the adjusted stack completely. */
  const RGBA with_adjustment = cpu_pixel(kColor);

  expect_graph_matches_cpu(kColor);
  /* Hidden, the adjustment leaves the same top layer showing. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, adjustment, false));
  const RGBA without = cpu_pixel(kColor);
  EXPECT_NEAR(with_adjustment.r, without.r, 2e-3f);
  EXPECT_NEAR(with_adjustment.g, without.g, 2e-3f);
  EXPECT_NEAR(with_adjustment.b, without.b, 2e-3f);
}

TEST_F(PaintLayersAdjustmentTest, levels_and_hsl_adjustments_match_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *levels = add_adjustment(MA_PAINT_LAYER_FILTER_LEVELS, 0.8f);
  ASSERT_NE(levels, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*levels, PAINT_LAYER_LEVELS_GAMMA, 1.8f));
  EXPECT_TRUE(paint_layer_kind_param_set(*levels, PAINT_LAYER_LEVELS_IN_BLACK, 0.15f));
  MaterialPaintLayer *hsl = add_adjustment(MA_PAINT_LAYER_FILTER_HSL, 0.7f);
  ASSERT_NE(hsl, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*hsl, PAINT_LAYER_HSL_SATURATION, 1.5f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersAdjustmentTest, brightness_contrast_adjustment_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *bc = add_adjustment(MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST, 0.8f);
  ASSERT_NE(bc, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(
      *bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS, 0.2f));
  EXPECT_TRUE(paint_layer_kind_param_set(
      *bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST, 0.4f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersAdjustmentTest, posterize_and_math_adjustments_match_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *posterize = add_adjustment(MA_PAINT_LAYER_FILTER_POSTERIZE, 0.8f);
  ASSERT_NE(posterize, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 6));
  MaterialPaintLayer *math = add_adjustment(MA_PAINT_LAYER_FILTER_MATH, 0.7f);
  ASSERT_NE(math, nullptr);
  EXPECT_TRUE(
      paint_layer_kind_param_int_set(*math, PAINT_LAYER_MATH_OPERATION, PAINT_LAYER_MATH_OP_SUBTRACT));
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_R, 0.2f));
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_G, 0.2f));
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_B, 0.2f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersAdjustmentTest, threshold_adjustment_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *threshold = add_adjustment(MA_PAINT_LAYER_FILTER_THRESHOLD, 0.7f);
  ASSERT_NE(threshold, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 0.4f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersAdjustmentTest, an_adjustment_works_only_in_the_channels_it_is_switched_on_for)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Base", 30, 160, 220, 255));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_solid_image("Rough", 60, 60, 60, 255)));
  const RGBA rough_before = cpu_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  const RGBA color_before = cpu_pixel(kColor);

  /* Base Color is on to start with; Roughness is not. */
  MaterialPaintLayer *adjustment = add_adjustment(MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(adjustment, nullptr);
  ASSERT_NE(paint_layer_channel_find(*adjustment, kColor), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*adjustment, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  expect_graph_matches_cpu(kColor);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(cpu_pixel(kColor).r, 1.0f - color_before.r, 2e-3f);
  EXPECT_NEAR(cpu_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS).r, rough_before.r, 2e-3f);

  /* Switched on, it changes Roughness too; switched off again, it leaves the channel alone. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, adjustment, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            nullptr);
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(cpu_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS).r, 1.0f - rough_before.r, 2e-3f);
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, adjustment, PAINT_MATERIAL_CHANNEL_ROUGHNESS, false));
  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(cpu_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS).r, rough_before.r, 2e-3f);
}

TEST_F(PaintLayersAdjustmentTest, an_adjustment_obeys_its_mask)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  MaterialPaintLayer *adjustment = add_adjustment(MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(adjustment, nullptr);
  /* A mask of 0.4 lets 40 % of the inversion through, over the stack it adjusts. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, adjustment, 0.4f), nullptr);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r + (1.0f - 2.0f * before.r) * 0.4f, 3e-3f);
  EXPECT_NEAR(after.g, before.g + (1.0f - 2.0f * before.g) * 0.4f, 3e-3f);
  EXPECT_NEAR(after.b, before.b + (1.0f - 2.0f * before.b) * 0.4f, 3e-3f);
}

TEST_F(PaintLayersAdjustmentTest, an_adjustment_does_not_touch_the_normal_channel)
{
  const float flat[4] = {0.5f, 0.5f, 1.0f, 1.0f};
  Image &normal = *BKE_image_add_generated(
      bmain, 4, 4, "Normal", 32, false, IMA_GENTYPE_BLANK, flat, false, false, false);
  add_layer("Row", &normal, PAINT_MATERIAL_CHANNEL_NORMAL);
  const RGBA before = cpu_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_NE(add_adjustment(MA_PAINT_LAYER_FILTER_INVERT), nullptr);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_NORMAL);
  const RGBA after = cpu_pixel(PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_NEAR(after.r, before.r, 2e-3f);
  EXPECT_NEAR(after.g, before.g, 2e-3f);
  EXPECT_NEAR(after.b, before.b, 2e-3f);
}

TEST_F(PaintLayersAdjustmentTest, a_pass_through_folder_takes_one_and_an_isolating_one_refuses)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  PaintLayerAddParams folder_params;
  folder_params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(*ma, folder_params);
  ASSERT_NE(folder, nullptr);

  /* A plain folder is Pass Through: the adjustment lays over the stack below it. */
  ASSERT_NE(add_adjustment(MA_PAINT_LAYER_FILTER_INVERT, 0.5f, folder), nullptr);
  expect_graph_matches_cpu(kColor);

  /* A mask on the folder makes it isolating, which a filter of the colour cannot read. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, folder, 1.0f), nullptr);
  EXPECT_EQ(add_adjustment(MA_PAINT_LAYER_FILTER_HSL, 1.0f, folder), nullptr);
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  bool reported = false;
  for (const PaintLayersIssue &issue : issues) {
    reported |= issue.code == PaintLayersIssueCode::KindNotAllowed;
  }
  /* The adjustment that was there before the mask is now inside an isolating folder. */
  EXPECT_TRUE(reported);
  expect_graph_matches_cpu(kColor);
}

class PaintLayersMaskFilterTest : public PaintLayersFilterTest {
 public:
  /** A filter of \a filter in the mask of \a row, or null when refused. */
  MaterialPaintLayer *add_mask_filter(MaterialPaintLayer &row,
                                      const eMaterialPaintLayerFilter filter,
                                      const float opacity = 1.0f)
  {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::MaskFilter;
    params.anchor = &row;
    params.filter = filter;
    MaterialPaintLayer *item = BKE_paint_layers_add_with_policy(*ma, params);
    if (item != nullptr) {
      EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, item, opacity));
    }
    return item;
  }
};

TEST_F(PaintLayersMaskFilterTest, invert_in_the_mask_turns_the_coverage_around)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.3f), nullptr);

  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.3f, 3e-3f);
  ASSERT_NE(add_mask_filter(*row, MA_PAINT_LAYER_FILTER_INVERT), nullptr);

  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.7f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, levels_and_hsl_in_the_mask_work_on_its_coverage)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* A gamma of 2 raises the coverage to the power of a half: 0.25 becomes 0.5. */
  MaterialPaintLayer *levels = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_LEVELS);
  ASSERT_NE(levels, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*levels, PAINT_LAYER_LEVELS_GAMMA, 2.0f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.5f, 3e-3f);

  /* Half the value halves it again. */
  MaterialPaintLayer *hsl = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_HSL);
  ASSERT_NE(hsl, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*hsl, PAINT_LAYER_HSL_VALUE, 0.5f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.25f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, brightness_contrast_in_the_mask_works_on_its_coverage)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* `max(1.2 * 0.25 + (0.2 - 0.1), 0) = 0.4`. */
  MaterialPaintLayer *bc = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST);
  ASSERT_NE(bc, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(
      *bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS, 0.2f));
  EXPECT_TRUE(paint_layer_kind_param_set(
      *bc, PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST, 0.2f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.4f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, color_ramp_in_the_mask_works_on_its_coverage)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  MaterialPaintLayer *ramp = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_COLOR_RAMP);
  ASSERT_NE(ramp, nullptr);
  MaterialPaintLayerRamp *band = BKE_paint_layers_ramp_ensure(*ramp);
  ASSERT_NE(band, nullptr);
  band->band.tot = 2;
  band->band.ipotype = COLBAND_INTERP_LINEAR;
  band->band.data[0] = {};
  band->band.data[0].a = 1.0f;
  band->band.data[1] = {};
  band->band.data[1].pos = 1.0f;
  band->band.data[1].a = 1.0f;

  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.25f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, posterize_in_the_mask_works_on_its_coverage)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* `floor(0.25 * 3 + 0.5) / 3 = 1 / 3`: the coverage rounds to the second of four steps. */
  MaterialPaintLayer *posterize = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_POSTERIZE);
  ASSERT_NE(posterize, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*posterize, PAINT_LAYER_POSTERIZE_STEPS, 4));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 1.0f / 3.0f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, math_in_the_mask_works_on_its_coverage)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* `0.25 * 2 = 0.5`. */
  MaterialPaintLayer *math = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_MATH);
  ASSERT_NE(math, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_int_set(
      *math, PAINT_LAYER_MATH_OPERATION, PAINT_LAYER_MATH_OP_MULTIPLY));
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_R, 2.0f));
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_G, 2.0f));
  EXPECT_TRUE(paint_layer_kind_param_set(*math, PAINT_LAYER_MATH_OPERAND_B, 2.0f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.5f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, threshold_in_the_mask_turns_the_coverage_into_two_states)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* `0.25 > 0.2`: the whole coverage passes. */
  MaterialPaintLayer *threshold = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_THRESHOLD);
  ASSERT_NE(threshold, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 0.2f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 1.0f, 3e-3f);

  /* `0.25 > 0.3` is false: none of it passes. */
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 0.3f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.0f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, a_threshold_equal_to_the_coverage_passes_nothing)
{
  /* The strict `>` at its own edge. The coverage travels as a plain float, with no image decode in
   * between, so both sides compare the very same value; a byte colour decodes through each side's
   * own sRGB step and is not reproducible bit for bit. */
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  MaterialPaintLayer *threshold = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_THRESHOLD);
  ASSERT_NE(threshold, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*threshold, PAINT_LAYER_THRESHOLD_THRESHOLD, 0.25f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.0f, 3e-3f);
}

TEST_F(PaintLayersMaskFilterTest, a_mask_filter_needs_no_channel_and_makes_the_base_mask_itself)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  EXPECT_EQ(BKE_paint_layers_mask_base(*row), nullptr);

  MaterialPaintLayer *item = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_INVERT, 0.5f);
  ASSERT_NE(item, nullptr);
  /* White to start with, so inverted by half it covers by half, and it carries no channel. */
  EXPECT_NE(BKE_paint_layers_mask_base(*row), nullptr);
  EXPECT_EQ(item->channels_num, 0);
  EXPECT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersFilterTest, invert_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  add_invert(*row, 0.6f);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersFilterTest, full_invert_is_the_complement_of_the_unfiltered_colour)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  add_invert(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 1.0f - before.r, 2e-3f);
  EXPECT_NEAR(after.g, 1.0f - before.g, 2e-3f);
  EXPECT_NEAR(after.b, 1.0f - before.b, 2e-3f);
}

TEST_F(PaintLayersFilterTest, invert_keeps_the_coverage_of_a_translucent_row)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 128));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.8f));
  add_invert(*row, 0.7f);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersFilterTest, a_filter_after_a_correction_works_on_the_corrected_colour)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(*ma, row, "Correction");
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, correction, kColor, add_solid_image("Corr", 240, 20, 90, 200)));
  add_invert(*row, 0.5f);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersFilterTest, a_filter_alone_does_not_make_a_row_take_part)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Empty", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, kColor), nullptr);
  const RGBA before = cpu_pixel(kColor);
  add_invert(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r, 2e-3f);
  EXPECT_NEAR(after.g, before.g, 2e-3f);
  EXPECT_NEAR(after.b, before.b, 2e-3f);
}

}  // namespace blender::bke::tests
