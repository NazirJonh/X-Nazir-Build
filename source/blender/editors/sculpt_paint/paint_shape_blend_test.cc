/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_shape_blend.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

static ShapeStyle flat_style()
{
  ShapeStyle style;
  style.stroke_profile_table.fill(1.0f);
  style.fill_profile_table.fill(1.0f);
  style.stroke_ramp_table.fill(float4(1.0f));
  style.fill_gradient_table.fill(float4(1.0f));
  style.feather = 1.0f;
  style.fill_color = float4(1.0f, 0.0f, 0.0f, 1.0f);
  style.stroke_color = float4(0.0f, 0.0f, 1.0f, 1.0f);
  return style;
}

TEST(ShapeBlend, PartCoverageAppliesProfileAsCoverage)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL | PAINT_SHAPE_USE_STROKE | PAINT_SHAPE_USE_PROFILE;
  style.profile_mode = PAINT_SHAPE_PROFILE_COVERAGE;
  style.stroke_profile_table.fill(0.5f);
  style.fill_profile_table.fill(0.25f);

  const ShapeSample sample{.fill = 1.0f, .stroke = 1.0f, .fill_d = 0.0f};
  EXPECT_NEAR(shape_part_coverage(style, sample, ShapePart::Stroke), 0.5f, 1e-6f);
  EXPECT_NEAR(shape_part_coverage(style, sample, ShapePart::Fill), 0.25f, 1e-6f);

  /* A profile that does not affect coverage leaves the raw coverage alone. */
  style.profile_mode = PAINT_SHAPE_PROFILE_HEIGHT;
  EXPECT_NEAR(shape_part_coverage(style, sample, ShapePart::Stroke), 1.0f, 1e-6f);
}

TEST(ShapeBlend, CanvasSolidFillReplacesAtFullCoverage)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.fill_blend = IMB_BLEND_MIX;

  ShapeBlendContext ctx;
  ctx.style = &style;
  ctx.channel = -1;

  float4 dst(0.2f, 0.3f, 0.4f, 1.0f);
  shape_blend_pixel(dst, ctx, ShapeSample{.fill = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(dst.x, 1.0f, 1e-5f);
  EXPECT_NEAR(dst.y, 0.0f, 1e-5f);
  EXPECT_NEAR(dst.z, 0.0f, 1e-5f);
  EXPECT_NEAR(dst.w, 1.0f, 1e-5f);
}

TEST(ShapeBlend, CanvasGradientFillSamplesBox)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.fill_blend = IMB_BLEND_MIX;
  /* Red to blue across the whole table, linearly: the sample at t maps to t directly. */
  const int n = ShapeStyle::PROFILE_TABLE_SIZE;
  for (const int i : IndexRange(n)) {
    const float t = float(i) / float(n - 1);
    style.fill_gradient_table[i] = float4(1.0f - t, 0.0f, t, 1.0f);
  }

  const ShapeFillGradient gradient{float2(0.0f, 0.0f), float2(100.0f, 0.0f)};
  ShapeBlendContext ctx;
  ctx.style = &style;
  ctx.channel = -1;
  ctx.fill_gradient = &gradient;

  const auto sample_at_x = [&](const float x) {
    float4 dst(0.0f);
    shape_blend_pixel(dst, ctx, ShapeSample{.fill = 1.0f}, 1.0f, float2(x, 0.0f));
    return dst;
  };

  const float4 left = sample_at_x(0.0f);
  EXPECT_NEAR(left.x, 1.0f, 1e-4f);
  EXPECT_NEAR(left.z, 0.0f, 1e-4f);

  const float4 middle = sample_at_x(50.0f);
  EXPECT_NEAR(middle.x, 0.5f, 1e-4f);
  EXPECT_NEAR(middle.z, 0.5f, 1e-4f);

  const float4 right = sample_at_x(100.0f);
  EXPECT_NEAR(right.x, 0.0f, 1e-4f);
  EXPECT_NEAR(right.z, 1.0f, 1e-4f);
}

TEST(ShapeBlend, ScalarChannelWritesValue)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].use = true;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].value = 0.25f;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_ROUGHNESS].blend = IMB_BLEND_MIX;

  ShapeBlendContext ctx;
  ctx.style = &style;
  ctx.channel = int(PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  float4 dst(0.9f, 0.9f, 0.9f, 1.0f);
  shape_blend_pixel(dst, ctx, ShapeSample{.fill = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(dst.x, 0.25f, 1e-5f);
  EXPECT_NEAR(dst.y, 0.25f, 1e-5f);
  EXPECT_NEAR(dst.z, 0.25f, 1e-5f);
}

TEST(ShapeBlend, HeightAddAccumulatesDelta)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.height_blend = PAINT_SHAPE_HEIGHT_ADD;
  style.height_depth = 2.0f;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use = true;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].strength = 1.0f;

  ShapeBlendContext ctx;
  ctx.style = &style;
  ctx.channel = int(PAINT_MATERIAL_CHANNEL_HEIGHT);

  float4 dst(0.0f, 0.0f, 0.0f, 1.0f);
  shape_blend_pixel(dst, ctx, ShapeSample{.stroke = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(dst.x, 2.0f, 1e-5f);
  EXPECT_NEAR(dst.y, 2.0f, 1e-5f);
  EXPECT_NEAR(dst.z, 2.0f, 1e-5f);
}

TEST(ShapeBlend, FlatNormalKeepsEncodedFlat)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = true;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_NORMAL].strength = 1.0f;

  ShapeBlendContext ctx;
  ctx.style = &style;
  ctx.channel = int(PAINT_MATERIAL_CHANNEL_NORMAL);

  float4 dst(0.5f, 0.5f, 1.0f, 1.0f);
  shape_blend_pixel(dst, ctx, ShapeSample{.fill = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(dst.x, 0.5f, 1e-5f);
  EXPECT_NEAR(dst.y, 0.5f, 1e-5f);
  EXPECT_NEAR(dst.z, 1.0f, 1e-5f);

  /* An explicit identity basis must not change the result. */
  const NormalWriteBasis identity;
  ctx.normal_basis = &identity;
  float4 with_basis(0.5f, 0.5f, 1.0f, 1.0f);
  shape_blend_pixel(with_basis, ctx, ShapeSample{.fill = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(with_basis.x, 0.5f, 1e-5f);
  EXPECT_NEAR(with_basis.y, 0.5f, 1e-5f);
  EXPECT_NEAR(with_basis.z, 1.0f, 1e-5f);
}

}  // namespace blender::ed::sculpt_paint::shape
