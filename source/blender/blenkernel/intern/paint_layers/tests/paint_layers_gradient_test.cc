/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_gradient.hh"

#include "DNA_material_types.h"

#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

class PaintLayersGradient : public PaintLayersTestBase {};

TEST_F(PaintLayersGradient, add_creates_a_generator_row_with_base_color)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(BKE_paint_layers_gradient_supported(*layer));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_GENERATOR);
  EXPECT_EQ(layer->kind_subtype, MA_PAINT_LAYER_GENERATOR_GRADIENT);
  ASSERT_NE(layer->gradient, nullptr);
  ASSERT_EQ(layer->channels_num, 1);
  EXPECT_EQ(layer->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_FALSE(BKE_paint_layers_row_is_paintable(*layer));
}

TEST_F(PaintLayersGradient, the_line_decides_the_ramp_position)
{
  MaterialPaintLayerGradient gradient;
  BKE_paint_layers_gradient_init(gradient);

  /* The defaults run from (0, 0.5) to (1, 0.5), so u is the ramp position. */
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.0f, 0.5f, 0.0f)), 0.0f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.5f, 0.5f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(1.0f, 0.5f, 0.0f)), 1.0f, 1e-6f);
  /* Outside the line the no-repeat default clamps to the nearest end. */
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(-1.0f, 0.5f, 0.0f)), 0.0f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(2.0f, 0.5f, 0.0f)), 1.0f, 1e-6f);
}

TEST_F(PaintLayersGradient, repeat_and_midpoint_fold_the_parameter)
{
  MaterialPaintLayerGradient gradient;
  BKE_paint_layers_gradient_init(gradient);

  gradient.repeat = MA_PAINT_LAYER_GRADIENT_REPEAT_REPEAT;
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(1.25f, 0.5f, 0.0f)), 0.25f, 1e-5f);

  gradient.repeat = MA_PAINT_LAYER_GRADIENT_REPEAT_REFLECT;
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(1.25f, 0.5f, 0.0f)), 0.75f, 1e-5f);

  /* A midpoint of 0.75 puts the ramp's 0.5 at t = 0.75. */
  gradient.repeat = MA_PAINT_LAYER_GRADIENT_REPEAT_NONE;
  gradient.midpoint = 0.75f;
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.75f, 0.5f, 0.0f)), 0.5f, 1e-5f);
}

TEST_F(PaintLayersGradient, a_degenerate_line_reads_the_start_of_the_ramp)
{
  MaterialPaintLayerGradient gradient;
  BKE_paint_layers_gradient_init(gradient);
  gradient.end[0] = gradient.start[0];
  gradient.end[1] = gradient.start[1];

  EXPECT_FLOAT_EQ(BKE_paint_layers_gradient_factor(gradient, float3(0.7f, 0.2f, 0.0f)), 0.0f);
}

}  // namespace blender::bke::tests
