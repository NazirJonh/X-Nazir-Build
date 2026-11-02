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

#include "BKE_appdir.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_paint_layers_edit.hh"

#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_fileops.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_color_types.h"
#include "DNA_colorband_types.h"
#include "DNA_curve_types.h"

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
  /* The green and blue bases are positive, so their channels keep the honest logarithm: the
   * layer's sub-1 linear values log to a negative number, which is a defined result, not the
   * node's refusal. Only the non-positive base folds its channel to zero. */
  EXPECT_LT(after.g, 0.0f);
  EXPECT_LT(after.b, 0.0f);
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

class PaintLayersClampTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_clamp(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_CLAMP);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }
};

TEST_F(PaintLayersClampTest, clamp_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *clamp = add_clamp(*row, 0.7f);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MIN, 0.2f));
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MAX, 0.8f));

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersClampTest, a_full_strength_clamp_keeps_the_colour_inside_the_range)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *clamp = add_clamp(*row);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MIN, 0.2f));
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MAX, 0.8f));

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_GE(after.r, 0.2f - 2e-3f);
  EXPECT_GE(after.g, 0.2f - 2e-3f);
  EXPECT_GE(after.b, 0.2f - 2e-3f);
  EXPECT_LE(after.r, 0.8f + 2e-3f);
  EXPECT_LE(after.g, 0.8f + 2e-3f);
  EXPECT_LE(after.b, 0.8f + 2e-3f);
}

TEST_F(PaintLayersClampTest, the_bounds_are_clamped_to_their_ranges)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *clamp = add_clamp(*row);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MIN, 5.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*clamp, PAINT_LAYER_CLAMP_MIN), 1.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MAX, -5.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*clamp, PAINT_LAYER_CLAMP_MAX), 0.0f);
}

TEST_F(PaintLayersClampTest, a_clamp_with_min_above_max_reads_the_max)
{
  /* The node order `min(max(c, min), max)` keeps a well-defined result when the bounds are
   * crossed: every value folds onto the upper bound itself. */
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *clamp = add_clamp(*row);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MIN, 0.8f));
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MAX, 0.2f));

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, 0.2f, 2e-3f);
  EXPECT_NEAR(after.g, 0.2f, 2e-3f);
  EXPECT_NEAR(after.b, 0.2f, 2e-3f);
}

class PaintLayersCurvesTest : public PaintLayersFilterTest {
 public:
  MaterialPaintLayer *add_curves(MaterialPaintLayer &row, const float opacity = 1.0f)
  {
    MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
        *ma, &row, MA_PAINT_LAYER_FILTER_CURVES);
    EXPECT_NE(filter, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, filter, opacity));
    return filter;
  }

  /**
   * Channel curves that lift the value by a quarter (`v + 0.25` over the range, extrapolated past
   * it), with the combined curve left at its identity, so a lifted value is one lift only.
   */
  static void set_lift(MaterialPaintLayer &filter)
  {
    MaterialPaintLayerCurves *curves = BKE_paint_layers_curves_ensure(filter);
    ASSERT_NE(curves, nullptr);
    CurveMapping &cumap = curves->curve_mapping;
    for (const int i : IndexRange(3)) {
      CurveMap &curve = cumap.cm[i];
      ASSERT_EQ(curve.totpoint, 2);
      curve.curve[0].x = 0.0f;
      curve.curve[0].y = 0.25f;
      curve.curve[1].x = 1.0f;
      curve.curve[1].y = 1.25f;
    }
    BKE_curvemapping_init(&cumap);
  }
};

TEST_F(PaintLayersCurvesTest, curves_over_a_row_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *curves = add_curves(*row, 0.7f);
  set_lift(*curves);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCurvesTest, the_default_curves_leave_the_colour_alone)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  add_curves(*row);

  expect_graph_matches_cpu(kColor);
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r, 2e-3f);
  EXPECT_NEAR(after.g, before.g, 2e-3f);
  EXPECT_NEAR(after.b, before.b, 2e-3f);
}

TEST_F(PaintLayersCurvesTest, a_full_strength_lift_raises_every_channel)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA before = cpu_pixel(kColor);
  MaterialPaintLayer *curves = add_curves(*row);
  set_lift(*curves);

  expect_graph_matches_cpu(kColor);
  /* The lift is linear over the range: every channel reads exactly a quarter up. */
  const RGBA after = cpu_pixel(kColor);
  EXPECT_NEAR(after.r, before.r + 0.25f, 2e-3f);
  EXPECT_NEAR(after.g, before.g + 0.25f, 2e-3f);
  EXPECT_NEAR(after.b, before.b + 0.25f, 2e-3f);
}

namespace {

/** The `.PL Layer <name>` group the generator made for the row of that name, or null. */
bNodeTree *curves_test_layer_tree(Main &bmain, const char *layer_name)
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

/** The mapping of the keyed curves node in \a group, or null. */
const CurveMapping *curves_node_mapping_find(bNodeTree &group)
{
  for (bNode &node : group.nodes) {
    if (paint_layers::prop_string_get(node.system_properties,
                                      paint_layers::CURVES_NODE_KEY_PROP) == nullptr)
    {
      continue;
    }
    if (node.type_legacy == SH_NODE_CURVE_RGB && node.storage != nullptr) {
      return static_cast<const CurveMapping *>(node.storage);
    }
    return nullptr;
  }
  return nullptr;
}

}  // namespace

TEST_F(PaintLayersCurvesTest, editing_the_mapping_syncs_the_node_in_place)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row);
  MaterialPaintLayerCurves *curves = BKE_paint_layers_curves_ensure(*filter);
  ASSERT_NE(curves, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The curves are a value of the keyed node, not topology: the hash has to stay where it was,
   * the node's storage takes the new shape in place, and no rebuild is asked for. */
  const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
  const Vector<int> wired = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t before = paint_layers::paint_layers_layer_topology_hash(*ma, *row, wired, forms);

  set_lift(*filter);
  EXPECT_TRUE(BKE_paint_layers_curves_edited(*bmain, *ma, &curves->curve_mapping));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  const uint64_t after = paint_layers::paint_layers_layer_topology_hash(*ma, *row, wired, forms);
  EXPECT_EQ(after, before);

  /* The node in the row's group carries the edited shape already, hash against hash. */
  bNodeTree *group = curves_test_layer_tree(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  const CurveMapping *node_mapping = curves_node_mapping_find(*group);
  ASSERT_NE(node_mapping, nullptr);
  EXPECT_EQ(paint_layer_curves_hash(*node_mapping),
            paint_layer_curves_hash(curves->curve_mapping));

  /* A mapping no row owns edits nothing. */
  EXPECT_FALSE(BKE_paint_layers_curves_edited(*bmain, *ma, &row->mapping));
}

TEST_F(PaintLayersCurvesTest, live_edit_reaches_the_graph_without_regeneration)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row, 0.7f);
  set_lift(*filter);
  /* The tree the drag edits below. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  MaterialPaintLayerCurves *curves = BKE_paint_layers_curves_ensure(*filter);
  ASSERT_NE(curves, nullptr);
  curves->curve_mapping.cm[0].curve[0].y = 0.5f;
  BKE_curvemapping_init(&curves->curve_mapping);

  /* A drag: the edit lands inside the live scope, the interpreter reads `node.storage` as it is
   * afterwards — no regeneration in between. */
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  BKE_paint_layers_curves_live_begin();
  EXPECT_TRUE(BKE_paint_layers_curves_edited(*bmain, *ma, &curves->curve_mapping));
  BKE_paint_layers_curves_live_end();
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  GraphInterpreter interpreter;
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  const RGBA graph = interpreter.eval_result(result_name(kColor));
  const RGBA cpu = cpu_pixel(kColor);
  EXPECT_NEAR(graph.r, cpu.r, 2e-3f);
  EXPECT_NEAR(graph.g, cpu.g, 2e-3f);
  EXPECT_NEAR(graph.b, cpu.b, 2e-3f);
}

TEST_F(PaintLayersCurvesTest, missing_curves_node_falls_back_to_rebuild)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row);
  set_lift(*filter);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* A hand edit took the keyed node out: the sync cannot carry the group any more. */
  bNodeTree *group = curves_test_layer_tree(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  bNode *curves_node = nullptr;
  for (bNode &node : group->nodes) {
    if (paint_layers::prop_string_get(node.system_properties,
                                      paint_layers::CURVES_NODE_KEY_PROP) != nullptr)
    {
      curves_node = &node;
      break;
    }
  }
  ASSERT_NE(curves_node, nullptr);
  bke::node_remove_node(bmain, *group, *curves_node, true, false);

  MaterialPaintLayerCurves *curves = BKE_paint_layers_curves_ensure(*filter);
  ASSERT_NE(curves, nullptr);
  EXPECT_TRUE(BKE_paint_layers_curves_edited(*bmain, *ma, &curves->curve_mapping));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* And the rebuild the tag asks for heals the group: the count is there again. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = curves_test_layer_tree(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_GE(paint_layers::prop_int_get(
                group->id.properties, paint_layers::CURVES_NODE_COUNT_PROP, -1),
            0);
}

TEST_F(PaintLayersCurvesTest, loaded_group_without_the_count_rebuilds_once)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row);
  set_lift(*filter);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* A file written before the curves nodes were keyed: no count on the group. */
  bNodeTree *group = curves_test_layer_tree(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  paint_layers::prop_int_set(
      group->id.properties, paint_layers::CURVES_NODE_COUNT_PROP, -1);

  MaterialPaintLayerCurves *curves = BKE_paint_layers_curves_ensure(*filter);
  ASSERT_NE(curves, nullptr);
  EXPECT_TRUE(BKE_paint_layers_curves_edited(*bmain, *ma, &curves->curve_mapping));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* The rebuild the tag asks for commits the count, so the next edit syncs in place. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = curves_test_layer_tree(*bmain, "Row");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(paint_layers::prop_int_get(
                group->id.properties, paint_layers::CURVES_NODE_COUNT_PROP, -1),
            1);

  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  curves->curve_mapping.cm[0].curve[0].y = 0.75f;
  BKE_curvemapping_init(&curves->curve_mapping);
  EXPECT_TRUE(BKE_paint_layers_curves_edited(*bmain, *ma, &curves->curve_mapping));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersCurvesTest, curves_adjustment_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Adjustment;
  params.filter = MA_PAINT_LAYER_FILTER_CURVES;
  MaterialPaintLayer *curves = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(curves, nullptr);
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, curves, 0.8f));
  set_lift(*curves);

  expect_graph_matches_cpu(kColor);
}

TEST_F(PaintLayersCurvesTest, curves_in_the_mask_lift_the_coverage)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* The combined curve is the identity, so the coverage lifts once: 0.25 + 0.25 = 0.5. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskFilter;
  params.anchor = row;
  params.filter = MA_PAINT_LAYER_FILTER_CURVES;
  MaterialPaintLayer *curves = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(curves, nullptr);
  set_lift(*curves);

  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.5f, 3e-3f);
}

TEST_F(PaintLayersCurvesTest, a_duplicate_of_the_row_owns_curves_of_its_own)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row);
  set_lift(*filter);

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, filter);
  ASSERT_NE(copy, nullptr);
  MaterialPaintLayerCurves *orig_curves = BKE_paint_layers_curves_ensure(*filter);
  MaterialPaintLayerCurves *copy_curves = BKE_paint_layers_curves_ensure(*copy);
  ASSERT_NE(orig_curves, nullptr);
  ASSERT_NE(copy_curves, nullptr);
  /* The copy is deep: its own points and tables, equal to the original's at the moment of the
   * copy. */
  EXPECT_NE(orig_curves, copy_curves);
  EXPECT_NE(orig_curves->curve_mapping.cm[0].curve, copy_curves->curve_mapping.cm[0].curve);
  ASSERT_EQ(copy_curves->curve_mapping.cm[0].totpoint, orig_curves->curve_mapping.cm[0].totpoint);
  EXPECT_NEAR(copy_curves->curve_mapping.cm[0].curve[0].y, 0.25f, 1e-6f);

  /* An edit of the original's curve leaves the copy where it was. */
  orig_curves->curve_mapping.cm[0].curve[0].y = 0.6f;
  BKE_curvemapping_init(&orig_curves->curve_mapping);
  EXPECT_NEAR(copy_curves->curve_mapping.cm[0].curve[0].y, 0.25f, 1e-6f);
}

TEST_F(PaintLayersCurvesTest, a_copied_material_owns_curves_of_its_own)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row);
  set_lift(*filter);

  Material *ma_dup = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(ma_dup, nullptr);
  MaterialPaintLayer *dup_row = static_cast<MaterialPaintLayer *>(ma_dup->paint_layers.first);
  ASSERT_NE(dup_row, nullptr);
  MaterialPaintLayer *dup_filter = static_cast<MaterialPaintLayer *>(dup_row->effects.first);
  ASSERT_NE(dup_filter, nullptr);
  const CurveMapping *saved_curves = BKE_paint_layers_curves_get(*filter);
  const CurveMapping *dup_curves = BKE_paint_layers_curves_get(*dup_filter);
  ASSERT_NE(saved_curves, nullptr);
  ASSERT_NE(dup_curves, nullptr);
  /* The material copy is deep too: own points, same shape at the moment of the copy. */
  EXPECT_NE(saved_curves->cm[0].curve, dup_curves->cm[0].curve);
  ASSERT_EQ(dup_curves->cm[0].totpoint, saved_curves->cm[0].totpoint);
  EXPECT_NEAR(dup_curves->cm[0].curve[0].y, 0.25f, 1e-6f);

  /* Freeing the copy must not touch the original's points (each owns its own). */
  BKE_id_free(bmain, ma_dup);
  EXPECT_NE(saved_curves->cm[0].curve, nullptr);
  EXPECT_NEAR(saved_curves->cm[0].curve[0].y, 0.25f, 1e-6f);
}

TEST_F(PaintLayersCurvesTest, blend_round_trip_rebuilds_and_keeps_the_curves)
{
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *filter = add_curves(*row);
  set_lift(*filter);
  const bUUID filter_marker = filter->marker;
  id_us_plus(&ma->id);

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_curves.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);
  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "Layered")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_filter = BKE_paint_layers_find(*reloaded, filter_marker);
  ASSERT_NE(reloaded_filter, nullptr);
  const CurveMapping *reloaded_curves = BKE_paint_layers_curves_get(*reloaded_filter);
  ASSERT_NE(reloaded_curves, nullptr);
  /* The load rebuilt the tables the file does not keep. */
  for (const int i : IndexRange(4)) {
    EXPECT_NE(reloaded_curves->cm[i].table, nullptr);
    EXPECT_NE(reloaded_curves->cm[i].curve, nullptr);
  }
  /* And the shape works: a lift of the same colour reads the same both sides. */
  const CurveMapping *saved_curves = BKE_paint_layers_curves_get(*filter);
  ASSERT_NE(saved_curves, nullptr);
  const float sample_in[3] = {0.3f, 0.6f, 0.9f};
  float saved_out[3];
  float reloaded_out[3];
  copy_v3_v3(saved_out, sample_in);
  copy_v3_v3(reloaded_out, sample_in);
  paint_layer_curves_apply(*saved_curves, saved_out);
  paint_layer_curves_apply(*reloaded_curves, reloaded_out);
  EXPECT_NEAR(reloaded_out[0], saved_out[0], 1e-6f);
  EXPECT_NEAR(reloaded_out[1], saved_out[1], 1e-6f);
  EXPECT_NEAR(reloaded_out[2], saved_out[2], 1e-6f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
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

TEST_F(PaintLayersAdjustmentTest, clamp_adjustment_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  MaterialPaintLayer *clamp = add_adjustment(MA_PAINT_LAYER_FILTER_CLAMP, 0.7f);
  ASSERT_NE(clamp, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MIN, 0.1f));
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MAX, 0.9f));

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

TEST_F(PaintLayersMaskFilterTest, clamp_in_the_mask_raises_the_coverage_to_its_min)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *row = add_layer("Row", add_solid_image("Row", 30, 160, 220, 255));
  const RGBA row_alone_weight_one = cpu_pixel(kColor);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const RGBA bottom = cpu_pixel(kColor);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.25f), nullptr);

  /* `min(max(0.25, 0.4), 0.9) = 0.4`. */
  MaterialPaintLayer *clamp = add_mask_filter(*row, MA_PAINT_LAYER_FILTER_CLAMP);
  ASSERT_NE(clamp, nullptr);
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MIN, 0.4f));
  EXPECT_TRUE(paint_layer_kind_param_set(*clamp, PAINT_LAYER_CLAMP_MAX, 0.9f));
  expect_graph_matches_cpu(kColor);
  EXPECT_NEAR(row_weight(*row, bottom, row_alone_weight_one), 0.4f, 3e-3f);
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

/* -------------------------------------------------------------------- */
/** \name Filter LUTs
 *
 * The Combined preview reads a Color Ramp and an RGB Curves from a table instead of uniforms. The
 * shader's lookup has a CPU twin, and these hold it to the functions the CPU preview applies: if the
 * two disagree here, the GPU preview disagrees with the bake and the 3D viewport.
 * \{ */

namespace {

/** Compare the LUT's lookup with `paint_layer_curves_apply` over the range and past both ends. */
void expect_curves_lut_matches_apply(CurveMapping &cumap, const char *label)
{
  BKE_curvemapping_init(&cumap);

  Vector<float4> texels;
  int width = 0;
  int height = 0;
  paint_layer_curves_lut_build(cumap, texels, width, height);
  ASSERT_EQ(height, 2) << label;
  ASSERT_GE(width, CM_TOT) << label;

  for (const int i : IndexRange(512)) {
    const float value = -0.5f + 2.0f * (float(i) / 511.0f);
    const float input[3] = {value, value * 0.5f, 1.0f - value};
    float expected[3];
    copy_v3_v3(expected, input);
    paint_layer_curves_apply(cumap, expected);

    float actual[3];
    copy_v3_v3(actual, input);
    paint_layer_curves_lut_eval(texels, width, actual);

    for (const int channel : IndexRange(3)) {
      EXPECT_NEAR(actual[channel], expected[channel], 2e-3f)
          << label << " value " << value << " channel " << channel;
    }
  }
}

}  // namespace

TEST(PaintLayersCurvesLut, lookup_matches_the_filter_for_flat_lift_s_curve_and_extrapolation)
{
  {
    /* Flat: the identity mapping. */
    CurveMapping cumap = {};
    BKE_curvemapping_set_defaults(&cumap, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    expect_curves_lut_matches_apply(cumap, "identity");
    BKE_curvemapping_free_data(&cumap);
  }
  {
    /* Different offsets on each channel: R lifts, G drops, B compresses. */
    CurveMapping cumap = {};
    BKE_curvemapping_set_defaults(&cumap, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    cumap.cm[0].curve[0].x = 0.0f;
    cumap.cm[0].curve[0].y = 0.25f;
    cumap.cm[0].curve[1].x = 1.0f;
    cumap.cm[0].curve[1].y = 1.25f;
    cumap.cm[1].curve[0].x = 0.0f;
    cumap.cm[1].curve[0].y = -0.1f;
    cumap.cm[1].curve[1].x = 1.0f;
    cumap.cm[1].curve[1].y = 0.9f;
    cumap.cm[2].curve[0].x = 0.0f;
    cumap.cm[2].curve[0].y = 0.0f;
    cumap.cm[2].curve[1].x = 1.0f;
    cumap.cm[2].curve[1].y = 0.5f;
    expect_curves_lut_matches_apply(cumap, "per-channel");
    BKE_curvemapping_free_data(&cumap);
  }
  {
    /* An S: the combined curve gets a middle point above the diagonal. */
    CurveMapping cumap = {};
    BKE_curvemapping_set_defaults(&cumap, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    BKE_curvemap_insert(&cumap.cm[3], 0.5f, 0.8f);
    expect_curves_lut_matches_apply(cumap, "s-curve");
    BKE_curvemapping_free_data(&cumap);
  }
  {
    /* Extrapolation past [0, 1] with non-horizontal slopes. */
    CurveMapping cumap = {};
    BKE_curvemapping_set_defaults(&cumap, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    cumap.flag |= CUMA_EXTEND_EXTRAPOLATE;
    for (const int i : IndexRange(CM_TOT)) {
      CurveMap &curve = cumap.cm[i];
      curve.ext_in[0] = 1.0f;
      curve.ext_in[1] = 0.5f;
      curve.ext_out[0] = 1.0f;
      curve.ext_out[1] = 0.5f;
    }
    expect_curves_lut_matches_apply(cumap, "extrapolate");
    BKE_curvemapping_free_data(&cumap);
  }
  {
    /* Extrapolation off: beyond the table the edge value stays flat, zero slopes. */
    CurveMapping cumap = {};
    BKE_curvemapping_set_defaults(&cumap, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    cumap.flag &= ~CUMA_EXTEND_EXTRAPOLATE;
    expect_curves_lut_matches_apply(cumap, "flat-extrapolate");
    BKE_curvemapping_free_data(&cumap);
  }
  {
    /* The clip flag changes what the UI clamps, not what the RGB Curves filter evaluates. */
    CurveMapping cumap = {};
    BKE_curvemapping_set_defaults(&cumap, 4, 0.0f, 0.0f, 1.0f, 1.0f, HD_AUTO);
    cumap.flag |= CUMA_DO_CLIP;
    cumap.clipr.ymin = 0.1f;
    cumap.clipr.ymax = 0.9f;
    expect_curves_lut_matches_apply(cumap, "clip");
    BKE_curvemapping_free_data(&cumap);
  }
}

TEST(PaintLayersColorRampLut, lookup_matches_the_filter_for_every_interpolation_and_colour_mode)
{
  const eColorBand_Interp interpolations[] = {COLBAND_INTERP_LINEAR,
                                              COLBAND_INTERP_EASE,
                                              COLBAND_INTERP_CARDINAL,
                                              COLBAND_INTERP_B_SPLINE,
                                              COLBAND_INTERP_CONSTANT};
  const eColorBand_ColorMode colour_modes[] = {COLBAND_BLEND_RGB,
                                               COLBAND_BLEND_HSV,
                                               COLBAND_BLEND_HSL};

  for (const eColorBand_Interp ipotype : interpolations) {
    for (const eColorBand_ColorMode colour_mode : colour_modes) {
      ColorBand band = {};
      BKE_colorband_init(&band, true);
      band.ipotype = ipotype;
      band.color_mode = colour_mode;
      /* Three stops, so every interpolation has a segment on both sides. */
      band.tot = 3;
      band.data[0].pos = 0.0f;
      band.data[0].r = 0.05f;
      band.data[0].g = 0.1f;
      band.data[0].b = 0.5f;
      band.data[0].a = 0.3f;
      band.data[1].pos = 0.3f;
      band.data[1].r = 0.8f;
      band.data[1].g = 0.2f;
      band.data[1].b = 0.1f;
      band.data[1].a = 0.7f;
      band.data[2].pos = 1.0f;
      band.data[2].r = 0.95f;
      band.data[2].g = 0.9f;
      band.data[2].b = 0.1f;
      band.data[2].a = 1.0f;

      Vector<float4> texels;
      int width = 0;
      paint_layer_color_ramp_lut_build(band, texels, width);
      ASSERT_EQ(width, 2048);

      /* A CONSTANT band is read at its nearest sample; every other mode interpolates. The CPU
       * forces a LINEAR interpolation for the HSV and HSL colour modes. */
      const bool constant = colour_mode == COLBAND_BLEND_RGB &&
                            ipotype == COLBAND_INTERP_CONSTANT;
      for (const bool use_alpha : {false, true}) {
        for (const int i : IndexRange(512)) {
          const float value = float(i) / 511.0f;
          const float input[3] = {value, value, value};
          float expected[3];
          copy_v3_v3(expected, input);
          const float expected_strength = paint_layer_color_ramp_apply(band, use_alpha, expected);

          float actual[3];
          copy_v3_v3(actual, input);
          const float actual_strength = paint_layer_color_ramp_lut_eval(
              texels, width, constant, use_alpha, actual);

          for (const int channel : IndexRange(3)) {
            EXPECT_NEAR(actual[channel], expected[channel], 2e-3f)
                << "ipotype " << ipotype << " mode " << colour_mode << " value " << value
                << " channel " << channel;
          }
          EXPECT_NEAR(actual_strength, expected_strength, 2e-3f)
              << "ipotype " << ipotype << " mode " << colour_mode << " value " << value
              << " strength";
        }
      }
    }
  }
}

/** \} */

}  // namespace blender::bke::tests
