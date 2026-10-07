/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../shapes/paint_shape_render.hh"

#include <memory>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "../mesh/paint_material_blend.hh"
#include "../shapes/paint_shape_texture.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

static ShapeStyle flat_style()
{
  ShapeStyle style;
  style.stroke_profile_table.fill(1.0f);
  style.fill_profile_table.fill(1.0f);
  style.stroke_ramp_table.fill(float4(1.0f, 1.0f, 1.0f, 1.0f));
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

/* -------------------------------------------------------------------- */
/** \name Texture sourcing
 * \{ */

namespace {

/** A style whose fill carries a texture with one constant channel. */
ShapeStyle texture_style(const eMaterialPaintChannel channel, const float4 value)
{
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  auto texture = std::make_shared<ShapeTexture>();
  texture->native_size = int2(64, 64);
  texture->channels[channel].kind = ShapeTextureChannel::Kind::Constant;
  texture->channels[channel].constant = value;
  style.fill_texture = texture;
  style.fill_source = ShapeFillSource::Texture;
  style.fill_channels[channel].use = true;
  return style;
}

/** A 2x1 texture: the left texel black (a = 0), the right one white (a = 1). Registered as the
 * Base Color and the Alpha channel source (an IMAGE-style resolve). */
std::shared_ptr<ShapeTexture> split_texture()
{
  auto texture = std::make_shared<ShapeTexture>();
  texture->native_size = int2(2, 1);
  auto *pixels = new float[8]{0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  ShapeTextureChannel channel;
  channel.kind = ShapeTextureChannel::Kind::Buffer;
  channel.size = int2(2, 1);
  channel.float_px = pixels;
  channel.is_linear = true;
  texture->channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = channel;
  texture->channels[PAINT_MATERIAL_CHANNEL_ALPHA] = channel;
  /* The buffer dies with the texture. */
  std::shared_ptr<void> guard(pixels, [](void *data) { delete[] static_cast<float *>(data); });
  texture->keeper = std::move(guard);
  return texture;
}

}  // namespace

TEST(ShapeShade, TextureChannelColorAndFallback)
{
  /* A constant texture channel mixes with the solid entry by the mapping opacity. */
  ShapeStyle style = texture_style(PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   float4(0.0f, 1.0f, 0.0f, 1.0f));
  ShapeSample sample{.fill = 1.0f, .stroke = 0.0f};
  sample.shape_uv = float2(0.5f, 0.5f);

  const ChannelWrite write = shade_channel(style, sample, ShapePart::Fill,
                                           PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(write.value.y, 1.0f, 1e-6f);
  EXPECT_NEAR(write.alpha, 1.0f, 1e-6f);

  /* No texture: the solid entry stands (the pre-texture behavior). */
  ShapeStyle plain = flat_style();
  plain.flag = PAINT_SHAPE_USE_FILL;
  plain.fill_source = ShapeFillSource::Texture;
  plain.fill_channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR].use = true;
  const ChannelWrite fallback = shade_channel(
      plain, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(fallback.value.x, 1.0f, 1e-6f); /* the flat fill color is red */
}

TEST(ShapeShade, TextureStampOutsideWritesNothing)
{
  /* A non-tiling Mask mapping: a point outside the texture writes zero alpha. */
  ShapeStyle style = texture_style(PAINT_MATERIAL_CHANNEL_ROUGHNESS, float4(1.0f, 1.0f, 1.0f, 1.0f));
  style.fill_tex_map.mapping = PAINT_SHAPE_TEX_MAP_MASK;
  style.fill_tex_map.tile_x = false;
  style.fill_tex_map.tile_y = false;
  style.fill_tex_map.ref_tex_size_px = float2(10.0f, 10.0f);

  ShapeSample sample{.fill = 1.0f, .stroke = 0.0f};
  /* The texture frame spans p in [0, 10]; (15, 5) is outside. */
  const ChannelWrite outside = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, float2(15.0f, 5.0f));
  EXPECT_NEAR(outside.alpha, 0.0f, 1e-6f);
  const ChannelWrite inside = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, float2(5.0f, 5.0f));
  EXPECT_NEAR(inside.alpha, 1.0f, 1e-6f);
}

TEST(ShapeShade, TextureScalarInvertAndOpacity)
{
  ShapeStyle style = texture_style(PAINT_MATERIAL_CHANNEL_ROUGHNESS, float4(0.8f, 0.8f, 0.8f, 1.0f));
  style.fill_tex_map.invert = true;
  style.fill_tex_map.opacity = 0.5f;
  const ShapeSample sample{.fill = 1.0f, .stroke = 0.0f};
  sample.shape_uv = float2(0.5f, 0.5f);

  const ChannelWrite write = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  /* The constant 0.8 reads as a white buffer value (constant feeds rgba directly), inverted to
   * 0.2, then mixed with the solid entry (0) at 0.5 opacity -> 0.1. */
  EXPECT_NEAR(write.value.x, 0.1f, 1e-4f);
}

TEST(ShapeShade, TextureAlphaMasksCoverage)
{
  /* The Alpha channel multiplies its write by the texture's alpha: the black (a=0) half of the
   * split texture erases the coverage. */
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.fill_texture = split_texture();
  style.fill_source = ShapeFillSource::Texture;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = true;

  ShapeSample transparent_half{.fill = 1.0f, .stroke = 0.0f};
  transparent_half.shape_uv = float2(0.25f, 0.5f);
  ShapeSample opaque_half{.fill = 1.0f, .stroke = 0.0f};
  opaque_half.shape_uv = float2(0.75f, 0.5f);

  const ChannelWrite erased = shade_channel(
      style, transparent_half, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ALPHA);
  EXPECT_NEAR(erased.alpha, 0.0f, 1e-4f);
  const ChannelWrite kept = shade_channel(
      style, opaque_half, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ALPHA);
  EXPECT_NEAR(kept.alpha, 1.0f, 1e-4f);
}

TEST(ShapeShade, TextureNormalReplacesProfileSlope)
{
  /* With a usable normal texture the write carries the sampled (unpacked) normal, not the
   * profile slope. */
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  auto texture = std::make_shared<ShapeTexture>();
  texture->native_size = int2(1, 1);
  auto *pixels = new float[4]{0.75f, 0.5f, 1.0f, 1.0f}; /* encoded +X */
  ShapeTextureChannel &channel = texture->channels[PAINT_MATERIAL_CHANNEL_NORMAL];
  channel.kind = ShapeTextureChannel::Kind::Buffer;
  channel.size = int2(1, 1);
  channel.float_px = pixels;
  channel.is_linear = true;
  std::shared_ptr<void> guard(pixels, [](void *data) { delete[] static_cast<float *>(data); });
  texture->keeper = std::move(guard);
  style.fill_texture = texture;
  style.fill_source = ShapeFillSource::Texture;
  style.fill_channels[PAINT_MATERIAL_CHANNEL_NORMAL].use = true;

  ShapeSample sample{.fill = 1.0f, .stroke = 0.0f};
  sample.shape_uv = float2(0.5f, 0.5f);
  sample.fill_d = 4.0f;
  sample.fill_dir = float2(1.0f, 0.0f);
  const ChannelWrite write = shade_channel(
      style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_NORMAL);
  EXPECT_NEAR(write.value.x, 0.5f, 1e-5f);
  EXPECT_NEAR(write.value.y, 0.0f, 1e-5f);
  EXPECT_NEAR(write.value.z, 1.0f, 1e-5f);
}

TEST(ShapeShade, TextureCanvasColorAndTint)
{
  /* The canvas consumer reads the texture's canvas color (or the Base Color source) and tints
   * it with the part color. */
  ShapeStyle style = flat_style();
  style.flag = PAINT_SHAPE_USE_FILL;
  style.fill_texture = split_texture();
  style.fill_source = ShapeFillSource::Texture;
  style.fill_tex_map.use_tint = true;
  style.fill_color = float4(1.0f, 0.0f, 0.0f, 1.0f);

  ShapeSample opaque_half{.fill = 1.0f, .stroke = 0.0f};
  opaque_half.shape_uv = float2(0.75f, 0.5f);
  const float4 canvas = shade_canvas(style, opaque_half, ShapePart::Fill, float2(0.0f));
  /* White texture * red tint = red, with the texture's alpha in w. */
  EXPECT_NEAR(canvas.x, 1.0f, 1e-5f);
  EXPECT_NEAR(canvas.y, 0.0f, 1e-5f);
  EXPECT_NEAR(canvas.z, 0.0f, 1e-5f);
  EXPECT_NEAR(canvas.w, 1.0f, 1e-5f);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
