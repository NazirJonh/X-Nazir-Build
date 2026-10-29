/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../shapes/paint_shape_render.hh"

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
  EXPECT_NEAR(dst.x, 1.0f, 1e-5f);
  EXPECT_NEAR(dst.y, 1.0f, 1e-5f);
  EXPECT_NEAR(dst.z, 1.0f, 1e-5f);
}

TEST(ShapeBlend, HeightOnAFreshTexelRaisesCoverageFromTheNeutral)
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

  /* A fresh float Height texel is premultiplied-transparent. The shape has to raise its coverage or
   * the composite would display nothing, and add its relief to the neutral mid rather than to
   * zero. */
  float4 dst(0.0f, 0.0f, 0.0f, 0.0f);
  shape_blend_pixel(dst, ctx, ShapeSample{.stroke = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(dst.w, 1.0f, 1e-5f);
  /* The relief delta 2.0 is 1.0 in storage (scale 0.5); neutral 0.5 plus it = 1.5, premultiplied (coverage 1). */
  EXPECT_NEAR(dst.x, 1.5f, 1e-5f);
  EXPECT_NEAR(dst.y, 1.5f, 1e-5f);
  EXPECT_NEAR(dst.z, 1.5f, 1e-5f);
}

TEST(ShapeBlend, HeightMaxAndReplaceTargetTheNeutralPlusTheRelief)
{
  for (const ePaintShapeHeightBlend mode :
       {PAINT_SHAPE_HEIGHT_MAX, PAINT_SHAPE_HEIGHT_REPLACE})
  {
    ShapeStyle style = flat_style();
    style.flag = PAINT_SHAPE_USE_STROKE;
    style.height_blend = mode;
    style.height_depth = 2.0f;
    style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use = true;
    style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].strength = 1.0f;

    ShapeBlendContext ctx;
    ctx.style = &style;
    ctx.channel = int(PAINT_MATERIAL_CHANNEL_HEIGHT);

    /* Max and Replace converge to the stored neutral plus the stored relief, not to the relief around zero. */
    float4 dst(0.0f, 0.0f, 0.0f, 0.0f);
    shape_blend_pixel(dst, ctx, ShapeSample{.stroke = 1.0f}, 1.0f, float2(0.0f));
    EXPECT_NEAR(dst.w, 1.0f, 1e-5f) << "mode=" << int(mode);
    EXPECT_NEAR(dst.x, 1.5f, 1e-5f) << "mode=" << int(mode);
  }
}

TEST(ShapeBlend, HeightNegativeDepthDepressesBelowTheNeutral)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_STROKE;
  style.height_blend = PAINT_SHAPE_HEIGHT_ADD;
  style.height_depth = -0.5f;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].use = true;
  style.stroke_channels[PAINT_MATERIAL_CHANNEL_HEIGHT].strength = 1.0f;

  ShapeBlendContext ctx;
  ctx.style = &style;
  ctx.channel = int(PAINT_MATERIAL_CHANNEL_HEIGHT);

  /* A flat opaque texel at the neutral: a relief of -0.5 is -0.25 in storage, so the shape digs
   * the map below the neutral instead of only ever raising it. */
  float4 dst(0.5f, 0.5f, 0.5f, 1.0f);
  shape_blend_pixel(dst, ctx, ShapeSample{.stroke = 1.0f}, 1.0f, float2(0.0f));
  EXPECT_NEAR(dst.x, 0.25f, 1e-5f);
  EXPECT_NEAR(dst.w, 1.0f, 1e-5f);
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
