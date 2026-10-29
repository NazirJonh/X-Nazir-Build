/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <limits>

#include "BLI_index_range.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"

#include "BKE_colorband.hh"
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

  /* The defaults run from (0, 0.5, 0) to (1, 0.5, 0), so u is the ramp position. */
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
  copy_v3_v3(gradient.end, gradient.start);

  EXPECT_FLOAT_EQ(BKE_paint_layers_gradient_factor(gradient, float3(0.7f, 0.2f, 0.4f)), 0.0f);
}

TEST_F(PaintLayersGradient, space_rejects_an_unknown_value_and_keeps_the_line)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(layer->gradient, nullptr);
  float start[3];
  float end[3];
  copy_v3_v3(start, layer->gradient->start);
  copy_v3_v3(end, layer->gradient->end);

  EXPECT_FALSE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, eMaterialPaintLayerGradientSpace(42)));
  EXPECT_EQ(layer->gradient->space, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT);

  EXPECT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_EQ(layer->gradient->space, MA_PAINT_LAYER_GRADIENT_SPACE_UV);
  /* Switching the space reinterprets the numbers, it never converts them. */
  for (const int i : IndexRange(3)) {
    EXPECT_FLOAT_EQ(layer->gradient->start[i], start[i]);
    EXPECT_FLOAT_EQ(layer->gradient->end[i], end[i]);
  }
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  EXPECT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *layer, end, start));
  EXPECT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *layer, MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED));
  for (const int i : IndexRange(3)) {
    EXPECT_FLOAT_EQ(layer->gradient->start[i], end[i]);
    EXPECT_FLOAT_EQ(layer->gradient->end[i], start[i]);
  }
}

TEST_F(PaintLayersGradient, ramp_stop_position_is_clamped_to_its_neighbours)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(layer->gradient, nullptr);
  ColorBand &ramp = layer->gradient->ramp;
  ASSERT_EQ(ramp.tot, 2);
  ASSERT_TRUE(BKE_colorband_element_add(&ramp, 0.5f));
  ASSERT_EQ(ramp.tot, 3);

  /* The middle stop may move between its neighbours, never past them. */
  EXPECT_TRUE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, 1, 0.9f));
  EXPECT_FLOAT_EQ(ramp.data[1].pos, 0.9f);
  EXPECT_TRUE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, 1, -2.0f));
  EXPECT_FLOAT_EQ(ramp.data[1].pos, ramp.data[0].pos);
  EXPECT_TRUE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, 1, 2.0f));
  EXPECT_FLOAT_EQ(ramp.data[1].pos, ramp.data[2].pos);

  /* The ends stay inside 0..1. */
  EXPECT_TRUE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, 0, -1.0f));
  EXPECT_FLOAT_EQ(ramp.data[0].pos, 0.0f);
  EXPECT_TRUE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, 2, 3.0f));
  EXPECT_FLOAT_EQ(ramp.data[2].pos, 1.0f);

  EXPECT_FALSE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, -1, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_gradient_set_stop_position(*ma, *layer, 3, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_gradient_set_stop_position(
      *ma, *layer, 1, std::numeric_limits<float>::quiet_NaN()));
  /* The order is still the ramp's. */
  EXPECT_LE(ramp.data[0].pos, ramp.data[1].pos);
  EXPECT_LE(ramp.data[1].pos, ramp.data[2].pos);
}

TEST_F(PaintLayersGradient, a_scalar_channel_gets_a_black_to_white_value_ramp)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *base = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(base, nullptr);
  /* A colour channel reads the row's colour ramp, not a value ramp. */
  EXPECT_EQ(base->gradient_ramp, nullptr);

  MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  ASSERT_NE(rough->gradient_ramp, nullptr);
  EXPECT_EQ(rough->gradient_ramp->tot, 2);
  EXPECT_FLOAT_EQ(rough->gradient_ramp->data[0].r, 0.0f);
  EXPECT_FLOAT_EQ(rough->gradient_ramp->data[1].r, 1.0f);
  EXPECT_FLOAT_EQ(rough->gradient_ramp->data[0].a, 1.0f);
  EXPECT_FLOAT_EQ(rough->gradient_ramp->data[1].a, 1.0f);
}

TEST_F(PaintLayersGradient, a_height_channel_value_ramp_spans_the_signed_relief)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *height = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_NE(height, nullptr);
  /* The generator authors relief, not storage: a fresh Height ramp spans -1..1 (the ramp's stops
   * hold the normalized 0..1 parameter of that range), and the composite encodes the sampled
   * relief to the map's 0..1 around the neutral (see the CPU/graph parity test). */
  EXPECT_FLOAT_EQ(height->value[0], -1.0f);
  EXPECT_FLOAT_EQ(height->value[1], 1.0f);
  ASSERT_NE(height->gradient_ramp, nullptr);
  EXPECT_FLOAT_EQ(height->gradient_ramp->data[0].r, 0.0f);
  EXPECT_FLOAT_EQ(height->gradient_ramp->data[1].r, 1.0f);
}

TEST_F(PaintLayersGradient, sync_copies_the_base_color_stop_layout)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  ASSERT_NE(layer->gradient, nullptr);

  ColorBand &base = layer->gradient->ramp;
  ASSERT_TRUE(BKE_colorband_element_add(&base, 0.4f));
  ASSERT_EQ(base.tot, 3);
  /* Give the middle colour stop a value the channel should keep at the new position. */
  base.data[1].pos = 0.4f;

  ASSERT_TRUE(BKE_paint_layers_gradient_sync_channel_ramp(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  ASSERT_NE(rough->gradient_ramp, nullptr);
  ASSERT_EQ(rough->gradient_ramp->tot, base.tot);
  for (const int i : IndexRange(base.tot)) {
    EXPECT_FLOAT_EQ(rough->gradient_ramp->data[i].pos, base.data[i].pos);
    /* Every stop is grey: the value ramp stores its value in all three components. */
    EXPECT_FLOAT_EQ(rough->gradient_ramp->data[i].r, rough->gradient_ramp->data[i].g);
    EXPECT_FLOAT_EQ(rough->gradient_ramp->data[i].g, rough->gradient_ramp->data[i].b);
  }
}

TEST_F(PaintLayersGradient, the_gizmo_channel_selects_a_colour_or_value_ramp)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);

  EXPECT_EQ(BKE_paint_layers_gradient_channel_ramp_get(
                *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            &layer->gradient->ramp);
  EXPECT_EQ(BKE_paint_layers_gradient_channel_ramp_get(
                *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            rough->gradient_ramp);
  EXPECT_EQ(BKE_paint_layers_gradient_channel_ramp_get(
                *layer, PAINT_MATERIAL_CHANNEL_NORMAL),
            nullptr);

  EXPECT_TRUE(BKE_paint_layers_gradient_set_gizmo_channel(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_EQ(layer->gradient->gizmo_channel, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_FALSE(BKE_paint_layers_gradient_set_gizmo_channel(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_EQ(layer->gradient->gizmo_channel, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
}

TEST_F(PaintLayersGradient, a_channel_stop_moves_on_its_own_value_ramp)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  ASSERT_NE(rough->gradient_ramp, nullptr);
  ASSERT_TRUE(BKE_colorband_element_add(rough->gradient_ramp, 0.5f));

  EXPECT_TRUE(BKE_paint_layers_gradient_set_channel_stop_position(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 1, 0.75f));
  EXPECT_FLOAT_EQ(rough->gradient_ramp->data[1].pos, 0.75f);
  /* The colour ramp is untouched. */
  EXPECT_FLOAT_EQ(layer->gradient->ramp.data[1].pos, 1.0f);

  EXPECT_TRUE(BKE_paint_layers_gradient_set_channel_stop_position(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 1, -1.0f));
  EXPECT_FLOAT_EQ(rough->gradient_ramp->data[1].pos, rough->gradient_ramp->data[0].pos);
}

TEST_F(PaintLayersGradient, setters_refuse_non_finite_values)
{
  MaterialPaintLayer *layer = BKE_paint_layers_gradient_add(
      *ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  float values[3] = {0.0f, 0.0f, 0.0f};
  values[1] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(BKE_paint_layers_gradient_set_start(*ma, *layer, values));
  EXPECT_FALSE(BKE_paint_layers_gradient_set_end(*ma, *layer, values));
  EXPECT_FALSE(BKE_paint_layers_gradient_set_line(*ma, *layer, values, values));
  EXPECT_FLOAT_EQ(layer->gradient->start[1], 0.5f);
  EXPECT_FLOAT_EQ(layer->gradient->end[1], 0.5f);
}

TEST_F(PaintLayersGradient, object_space_factor_reads_the_3d_line)
{
  MaterialPaintLayerGradient gradient;
  BKE_paint_layers_gradient_init(gradient);
  gradient.space = MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT;
  const float start[3] = {0.0f, 0.0f, 0.0f};
  const float end[3] = {0.0f, 2.0f, 0.0f};
  copy_v3_v3(gradient.start, start);
  copy_v3_v3(gradient.end, end);

  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.0f, 0.5f, 0.0f)), 0.25f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.0f, 1.0f, 3.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.0f, 1.5f, 0.0f)), 0.75f, 1e-6f);
}

TEST_F(PaintLayersGradient, generated_space_factor_reads_normalized_coordinates)
{
  MaterialPaintLayerGradient gradient;
  BKE_paint_layers_gradient_init(gradient);
  gradient.space = MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED;
  const float start[3] = {0.0f, 0.0f, 0.0f};
  const float end[3] = {1.0f, 0.0f, 0.0f};
  copy_v3_v3(gradient.start, start);
  copy_v3_v3(gradient.end, end);

  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.25f, 0.5f, 0.9f)), 0.25f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.75f, 0.0f, 0.1f)), 0.75f, 1e-6f);
}

TEST_F(PaintLayersGradient, uv_factor_ignores_z_and_3d_spaces_do_not)
{
  MaterialPaintLayerGradient zero_z;
  BKE_paint_layers_gradient_init(zero_z);
  MaterialPaintLayerGradient with_z;
  BKE_paint_layers_gradient_init(with_z);
  zero_z.space = MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  with_z.space = MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  with_z.start[2] = 9.0f;
  with_z.end[2] = -4.0f;

  /* Linear UV: Z of both ends and of the coordinate is ignored. */
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(with_z, float3(0.25f, 0.5f, 100.0f)),
              BKE_paint_layers_gradient_factor(zero_z, float3(0.25f, 0.5f, 0.0f)),
              1e-6f);
  /* Radial UV: the radius is 2D as well. */
  zero_z.type = MA_PAINT_LAYER_GRADIENT_RADIAL;
  with_z.type = MA_PAINT_LAYER_GRADIENT_RADIAL;
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(with_z, float3(0.5f, 0.75f, -20.0f)),
              BKE_paint_layers_gradient_factor(zero_z, float3(0.5f, 0.75f, 0.0f)),
              1e-6f);

  /* Object space really is 3D: a line along Z reads its coordinate. */
  MaterialPaintLayerGradient object;
  BKE_paint_layers_gradient_init(object);
  object.space = MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT;
  const float start[3] = {0.0f, 0.0f, 0.0f};
  const float end[3] = {0.0f, 0.0f, 2.0f};
  copy_v3_v3(object.start, start);
  copy_v3_v3(object.end, end);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(object, float3(0.0f, 0.0f, 1.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(object, float3(5.0f, 5.0f, 0.0f)), 0.0f, 1e-6f);

  /* Generated space uses the same 3D coordinate, only normalized differently. */
  MaterialPaintLayerGradient generated;
  BKE_paint_layers_gradient_init(generated);
  generated.space = MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED;
  copy_v3_v3(generated.start, start);
  copy_v3_v3(generated.end, end);
  EXPECT_NEAR(
      BKE_paint_layers_gradient_factor(generated, float3(0.0f, 0.0f, 0.5f)), 0.25f, 1e-6f);
}

TEST_F(PaintLayersGradient, radial_factor_uses_the_3d_distance)
{
  MaterialPaintLayerGradient gradient;
  BKE_paint_layers_gradient_init(gradient);
  gradient.space = MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT;
  gradient.type = MA_PAINT_LAYER_GRADIENT_RADIAL;
  const float start[3] = {0.0f, 0.0f, 0.0f};
  const float end[3] = {0.0f, 0.0f, 2.0f};
  copy_v3_v3(gradient.start, start);
  copy_v3_v3(gradient.end, end);

  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.0f, 0.0f, 1.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(1.0f, 0.0f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(BKE_paint_layers_gradient_factor(gradient, float3(0.0f, 0.0f, 2.0f)), 1.0f, 1e-6f);
}

}  // namespace blender::bke::tests
