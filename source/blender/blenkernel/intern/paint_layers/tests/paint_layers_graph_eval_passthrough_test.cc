/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Pass Through folders: a folder with no mask, the Mix blend and full opacity is laid in the stack
 * as if it were not there. The generated graph and the CPU composite must agree on that, equal the
 * same rows without the folder, and keep agreeing as the folder leaves and re-enters the mode.
 */

#include <cmath>

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

namespace {

constexpr eMaterialPaintChannel kColor = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

}  // namespace

TEST_F(PaintLayersGraphEvalTest, pass_through_predicate_follows_mask_blend_and_opacity)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *child = add_layer("Child", add_solid_image("Child", 30, 160, 220, 255));
  MaterialPaintLayer *members[1] = {child};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*child));

  /* Visibility is not part of the mode: hiding is a value edit. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, true));

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 1.0f));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MIX));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  ASSERT_NE(BKE_paint_layers_mask_add(*ma, folder, 0.5f), nullptr);
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
}

TEST_F(PaintLayersGraphEvalTest, pass_through_folder_equals_the_ungrouped_stack)
{
  /* Translucent maps and a partial opacity, so both the factors and the coverage are non-trivial. */
  const auto build = [&](const bool folded, const char *name) {
    ma = BKE_material_add(bmain, name);
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
    MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 30, 160, 220, 140));
    ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, first, 0.5f));
    MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 110));
    if (folded) {
      MaterialPaintLayer *members[2] = {first, second};
      MaterialPaintLayer *folder = BKE_paint_layers_group(
          *ma, Span<MaterialPaintLayer *>(members, 2));
      ASSERT_NE(folder, nullptr);
      ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
    }
  };

  build(true, "Folded");
  expect_graph_matches_cpu(kColor);
  const RGBA folded = cpu_pixel(kColor);
  const RGBA folded_graph = graph_pixel(kColor);

  build(false, "Plain");
  expect_graph_matches_cpu(kColor);
  const RGBA plain = cpu_pixel(kColor);
  const RGBA plain_graph = graph_pixel(kColor);

  EXPECT_NEAR(folded.r, plain.r, 1e-4f);
  EXPECT_NEAR(folded.g, plain.g, 1e-4f);
  EXPECT_NEAR(folded.b, plain.b, 1e-4f);
  EXPECT_NEAR(folded_graph.r, plain_graph.r, 1e-4f);
  EXPECT_NEAR(folded_graph.g, plain_graph.g, 1e-4f);
  EXPECT_NEAR(folded_graph.b, plain_graph.b, 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, pass_through_folder_in_another_channel_matches_the_cpu)
{
  Image *bottom = add_solid_image("Bottom", 64, 64, 64, 255);
  Image *top = add_solid_image("Top", 200, 200, 200, 255);
  make_image_data(bottom);
  make_image_data(top);
  add_layer("Bottom", bottom, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *top_layer = add_layer("Top", top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, top_layer, 0.35f));
  MaterialPaintLayer *members[1] = {top_layer};
  ASSERT_NE(BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1)), nullptr);

  expect_graph_matches_cpu(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersGraphEvalTest, hiding_a_pass_through_folder_shows_what_is_below)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  const RGBA bottom = graph_pixel(kColor);

  MaterialPaintLayer *child = add_layer("Child", add_solid_image("Child", 0, 0, 255, 255));
  MaterialPaintLayer *members[1] = {child};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  expect_graph_matches_cpu(kColor);
  const RGBA shown = graph_pixel(kColor);
  EXPECT_GT(std::abs(shown.b - bottom.b), 0.1f);

  /* The folder has no row of its own, so its visibility reaches the child through the scale. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  expect_graph_matches_cpu(kColor);
  const RGBA hidden = graph_pixel(kColor);
  EXPECT_NEAR(hidden.r, bottom.r, 2e-3f);
  EXPECT_NEAR(hidden.g, bottom.g, 2e-3f);
  EXPECT_NEAR(hidden.b, bottom.b, 2e-3f);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, true));
  const RGBA again = graph_pixel(kColor);
  EXPECT_NEAR(again.b, shown.b, 2e-3f);
}

TEST_F(PaintLayersGraphEvalTest, leaving_and_entering_pass_through_keeps_graph_and_cpu_equal)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *first = add_layer("First", add_solid_image("First", 0, 0, 255, 255));
  MaterialPaintLayer *second = add_layer("Second", add_solid_image("Second", 240, 20, 90, 128));
  MaterialPaintLayer *members[2] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);

  expect_graph_matches_cpu(kColor);
  const RGBA pass = graph_pixel(kColor);

  /* Half opacity isolates the folder: a new graph shape, and a visibly different result. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  expect_graph_matches_cpu(kColor);
  const RGBA isolated = graph_pixel(kColor);
  EXPECT_GT(std::abs(isolated.b - pass.b) + std::abs(isolated.r - pass.r), 0.05f);

  /* Back to full opacity: the pass-through graph again, the original result. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 1.0f));
  expect_graph_matches_cpu(kColor);
  const RGBA back = graph_pixel(kColor);
  EXPECT_NEAR(back.r, pass.r, 1e-4f);
  EXPECT_NEAR(back.g, pass.g, 1e-4f);
  EXPECT_NEAR(back.b, pass.b, 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, pass_through_folder_inside_an_isolating_folder_matches_the_cpu)
{
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *inner_row = add_layer("Inner", add_solid_image("Inner", 30, 160, 220, 160));
  MaterialPaintLayer *other = add_layer("Other", add_solid_image("Other", 240, 20, 90, 200));
  MaterialPaintLayer *inner_members[1] = {inner_row};
  MaterialPaintLayer *inner = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(inner_members, 1));
  ASSERT_NE(inner, nullptr);
  MaterialPaintLayer *outer_members[2] = {inner, other};
  MaterialPaintLayer *outer = BKE_paint_layers_group(
      *ma, Span<MaterialPaintLayer *>(outer_members, 2));
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, 0.6f));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*inner));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*outer));

  /* The inner folder inlines into the outer one's isolated chain, whose rows keep the isolated
   * form. */
  expect_graph_matches_cpu(kColor);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, inner, false));
  expect_graph_matches_cpu(kColor);
}

}  // namespace blender::bke::tests