/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_shape_shade.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "mesh/paint_material_blend.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

static ShapeStyle flat_style()
{
  ShapeStyle style;
  style.stroke_profile_table.fill(1.0f);
  style.fill_profile_table.fill(1.0f);
  style.stroke_ramp_table.fill(float4(1.0f, 1.0f, 1.0f, 1.0f));
  style.fill_gradient_table.fill(float4(1.0f, 1.0f, 1.0f, 1.0f));
  style.feather = 1.0f;
  style.fill_color = float4(1.0f, 0.0f, 0.0f, 1.0f);
  style.stroke_color = float4(0.0f, 0.0f, 1.0f, 1.0f);
  return style;
}

TEST(ShapeShade, CanvasSolidColors)
{
  const ShapeStyle style = flat_style();
  const ShapeSample sample{.fill = 1.0f, .stroke = 1.0f};
  const float4 fill = shade_canvas(style, sample, ShapePart::Fill, float2(0.0f));
  EXPECT_FLOAT_EQ(fill.x, 1.0f);
  EXPECT_FLOAT_EQ(fill.y, 0.0f);
  const float4 stroke = shade_canvas(style, sample, ShapePart::Stroke, float2(0.0f));
  EXPECT_FLOAT_EQ(stroke.z, 1.0f);
}

TEST(ShapeShade, ChannelScalarAndColor)
{
  ShapeStyle style = flat_style();
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = true;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].value = 0.25f;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = true;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[0] = 0.5f;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[1] = 0.25f;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].color[2] = 0.0f;

  const ShapeSample sample{.fill = 0.5f, .stroke = 1.0f};
  const ChannelWrite rough = shade_channel(
      style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_FLOAT_EQ(rough.value.x, 0.25f);
  EXPECT_FLOAT_EQ(rough.alpha, 1.0f);
  const ChannelWrite base = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_FLOAT_EQ(base.value.x, 0.5f);
  EXPECT_FLOAT_EQ(base.alpha, 0.5f);

  /* Disabled channels write nothing. */
  const ChannelWrite metallic = shade_channel(
      style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_METALLIC);
  EXPECT_FLOAT_EQ(metallic.alpha, 0.0f);
}

TEST(ShapeShade, HeightFollowsProfile)
{
  ShapeStyle style = flat_style();
  style.flag |= PAINT_SHAPE_USE_PROFILE;
  style.profile_mode = PAINT_SHAPE_PROFILE_HEIGHT;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use = true;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].value = 0.2f;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].strength = 0.5f;
  style.height_depth = 2.0f;
  /* Flat profile: h(t) == 1 everywhere, so the relief is strength * depth. */
  const ShapeSample sample{.stroke = 1.0f, .stroke_t = 0.3f};
  const ChannelWrite height = shade_channel(
      style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(height.value.x, 0.5f * 2.0f, 1e-5);
  EXPECT_FLOAT_EQ(height.alpha, 1.0f);
}

TEST(ShapeShade, NormalFlatIsUp)
{
  ShapeStyle style = flat_style();
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = true;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_NORMAL].strength = 1.0f;
  /* Flat profile has zero slope: the normal stays up no matter the gradient direction. */
  const ShapeSample sample{.stroke = 1.0f, .stroke_t = 0.5f, .stroke_dir = float2(1.0f, 0.0f)};
  const ChannelWrite normal = shade_channel(
      style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_NEAR(normal.value.x, 0.0f, 1e-5);
  EXPECT_NEAR(normal.value.y, 0.0f, 1e-5);
  EXPECT_NEAR(normal.value.z, 1.0f, 1e-5);
}

TEST(ShapeShade, FillGradientLinear)
{
  ShapeStyle style = flat_style();
  /* Red-to-blue ramp over the bbox: edges sample the stops, the middle mixes. */
  style.fill_gradient_table.fill(float4(1.0f, 0.0f, 0.0f, 1.0f));
  style.fill_gradient_table[ShapeStyle::PROFILE_TABLE_SIZE - 1] = float4(0.0f, 0.0f, 1.0f, 1.0f);
  const float2 lo(10.0f, 10.0f);
  const float2 hi(110.0f, 60.0f);
  EXPECT_FLOAT_EQ(fill_gradient_param(float2(10.0f, 30.0f), lo, hi), 0.0f);
  EXPECT_FLOAT_EQ(fill_gradient_param(float2(110.0f, 30.0f), lo, hi), 1.0f);
  EXPECT_FLOAT_EQ(fill_gradient_param(float2(60.0f, 30.0f), lo, hi), 0.5f);
  const float4 edge = shade_fill_gradient(style, float2(10.0f, 30.0f), lo, hi);
  EXPECT_FLOAT_EQ(edge.x, 1.0f);
  EXPECT_FLOAT_EQ(edge.z, 0.0f);
  const float4 far = shade_fill_gradient(style, float2(110.0f, 30.0f), lo, hi);
  EXPECT_FLOAT_EQ(far.x, 0.0f);
  EXPECT_FLOAT_EQ(far.z, 1.0f);
}

TEST(ShapeShade, RnmBlend)
{
  const float3 base(0.0f, 0.0f, 1.0f);
  const float3 detail(0.0f, 0.0f, 1.0f);
  /* No detail and flat detail both keep the base. */
  const float3 no_detail = material::blend_normal_rnm(base, float3(0.5f, 0.0f, 0.5f), 0.0f);
  EXPECT_FLOAT_EQ(no_detail.x, base.x);
  EXPECT_FLOAT_EQ(no_detail.y, base.y);
  EXPECT_FLOAT_EQ(no_detail.z, base.z);
  const float3 flat = material::blend_normal_rnm(base, detail, 1.0f);
  EXPECT_NEAR(flat.x, 0.0f, 1e-5);
  EXPECT_NEAR(flat.y, 0.0f, 1e-5);
  EXPECT_NEAR(flat.z, 1.0f, 1e-5);
  /* A tilted detail tilts the result and stays normalized. */
  const float3 tilted = material::blend_normal_rnm(base, math::normalize(float3(0.5f, 0.0f, 1.0f)), 1.0f);
  EXPECT_GT(tilted.x, 0.1f);
  EXPECT_NEAR(math::length(tilted), 1.0f, 1e-5);
}
TEST(ShapeShade, NormalSlopeDirectionAndFlip)
{
  /* A stroke profile falling from the centerline to the edge tilts the normal toward the
   * stroke direction (away from the centerline); DirectX (flip Y) mirrors the green channel. */
  ShapeStyle style = flat_style();
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = true;
  for (const int i : IndexRange(ShapeStyle::PROFILE_TABLE_SIZE)) {
    style.stroke_profile_table[i] = 1.0f - float(i) / float(ShapeStyle::PROFILE_TABLE_SIZE - 1);
  }

  ShapeSample sample{.fill = 0.0f, .stroke = 1.0f};
  sample.stroke_t = 0.5f;
  sample.stroke_dir = float2(0.0f, 1.0f);

  const ChannelWrite gl = shade_channel(
      style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_FLOAT_EQ(gl.value.x, 0.0f);
  EXPECT_GT(gl.value.y, 0.0f);
  EXPECT_LT(gl.value.z, 1.0f);
  EXPECT_NEAR(math::length(float3(gl.value.x, gl.value.y, gl.value.z)), 1.0f, 1e-5);

  ShapeStyle flipped = style;
  flipped.flag |= PAINT_SHAPE_NORMAL_FLIP_Y;
  const ChannelWrite dx = shade_channel(
      flipped, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_FLOAT_EQ(dx.value.x, gl.value.y);
  EXPECT_FLOAT_EQ(dx.value.y, gl.value.x);
}

TEST(ShapeShade, FillNormalFollowsFillProfile)
{
  /* A fill profile rising inward (0 at the edge, 1 at the falloff depth) tilts the fill normal
   * toward the edge - the downhill direction opposite #ShapeSample::fill_dir. */
  ShapeStyle style = flat_style();
  style.fill_channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = true;
  for (const int i : IndexRange(ShapeStyle::PROFILE_TABLE_SIZE)) {
    style.fill_profile_table[i] = float(i) / float(ShapeStyle::PROFILE_TABLE_SIZE - 1);
  }

  ShapeSample sample{.fill = 1.0f, .stroke = 0.0f};
  sample.fill_d = 4.0f;
  sample.fill_dir = float2(1.0f, 0.0f);

  const ChannelWrite fill = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_LT(fill.value.x, 0.0f);
  EXPECT_NEAR(fill.value.y, 0.0f, 1e-5);
  EXPECT_LT(fill.value.z, 1.0f);
}

TEST(ShapeShade, HeightNormalLink)
{
  /* With the height-to-normal link, a profile that does not drive height (coverage mode)
   * leaves the normal flat; switching the profile to height tilts it again. */
  ShapeStyle style = flat_style();
  style.flag |= PAINT_SHAPE_HEIGHT_NORMAL_LINK;
  style.profile_mode = PAINT_SHAPE_PROFILE_COVERAGE;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = true;
  for (const int i : IndexRange(ShapeStyle::PROFILE_TABLE_SIZE)) {
    style.fill_profile_table[i] = float(i) / float(ShapeStyle::PROFILE_TABLE_SIZE - 1);
  }

  ShapeSample sample{.fill = 1.0f, .stroke = 0.0f};
  sample.fill_d = 4.0f;
  sample.fill_dir = float2(1.0f, 0.0f);

  const ChannelWrite linked = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_FLOAT_EQ(linked.value.x, 0.0f);
  EXPECT_FLOAT_EQ(linked.value.y, 0.0f);
  EXPECT_FLOAT_EQ(linked.value.z, 1.0f);

  style.profile_mode = PAINT_SHAPE_PROFILE_HEIGHT;
  const ChannelWrite height_driven = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_LT(height_driven.value.x, 0.0f);
}

}  // namespace blender::ed::sculpt_paint::shape
