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
