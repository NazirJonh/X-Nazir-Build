/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../shapes/paint_shape_texture.hh"

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

namespace {

/** A flat mapping with the native texture size filled in (as a resolve would). */
ShapeTextureMapping mapping_with_size(const float2 native_size)
{
  ShapeTextureMapping map;
  map.ref_tex_size_px = native_size;
  return map;
}

ShapeSample sample_with_uv(const float2 shape_uv)
{
  ShapeSample sample;
  sample.fill = 1.0f;
  sample.shape_uv = shape_uv;
  return sample;
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Mask mapping
 * \{ */

TEST(ShapeTextureCoord, MaskIdentity)
{
  /* A 100x50 texture at scale 1: the anchor maps to uv (0, 0), the texture's native size spans
   * exactly one tile (uv (1, 1) tiles back to (0, 0)). */
  const ShapeTextureMapping map = mapping_with_size(float2(100.0f, 50.0f));
  const ShapeTexFrame frame;
  const ShapeTexCoord coord = shape_texture_coord_mask(map, float2(0.0f, 0.0f), frame);
  EXPECT_V2_NEAR(coord.uv, float2(0.0f, 0.0f), 1e-6f);
  EXPECT_TRUE(coord.inside);

  const ShapeTexCoord coord_mid = shape_texture_coord_mask(map, float2(100.0f, 50.0f), frame);
  EXPECT_V2_NEAR(coord_mid.uv, float2(0.0f, 0.0f), 1e-6f);
}

TEST(ShapeTextureCoord, MaskTilingAndMirror)
{
  const ShapeTextureMapping map = mapping_with_size(float2(10.0f, 10.0f));
  const ShapeTexFrame frame;
  /* Ten pixels to the right with a 10 px tile: u == 1.0 tiles back to 0 (the same texel). */
  ShapeTexCoord coord = shape_texture_coord_mask(map, float2(10.0f, 0.0f), frame);
  EXPECT_NEAR(coord.uv.x, 0.0f, 1e-5f);

  /* The mirrored fold: 13 px lands on 1 - 0.3 = 0.7. */
  map.mirror = true;
  coord = shape_texture_coord_mask(map, float2(13.0f, 0.0f), frame);
  EXPECT_NEAR(coord.uv.x, 0.7f, 1e-5f);
}

TEST(ShapeTextureCoord, MaskUntiledStamp)
{
  const ShapeTextureMapping map = mapping_with_size(float2(10.0f, 10.0f));
  map.tile_x = false;
  map.tile_y = false;
  const ShapeTexFrame frame;
  const ShapeTexCoord inside = shape_texture_coord_mask(map, float2(5.0f, 5.0f), frame);
  EXPECT_TRUE(inside.inside);
  const ShapeTexCoord outside = shape_texture_coord_mask(map, float2(15.0f, 5.0f), frame);
  EXPECT_FALSE(outside.inside);
}

TEST(ShapeTextureCoord, MaskRotation)
{
  /* A rotation by 90 degrees sends the +X offset onto +Y. */
  const ShapeTextureMapping map = mapping_with_size(float2(10.0f, 10.0f));
  map.angle = float(M_PI_2);
  const ShapeTexFrame frame;
  const ShapeTexCoord coord = shape_texture_coord_mask(map, float2(10.0f, 0.0f), frame);
  /* R(-90) * (10, 0) = (0, -10) -> uv (0, -1), wrapped to (0, 0). */
  EXPECT_NEAR(coord.uv.x, 0.0f, 1e-5f);
  EXPECT_NEAR(coord.uv.y, 0.0f, 1e-5f);
  EXPECT_NEAR(coord.frame_angle, float(M_PI_2), 1e-5f);
}

TEST(ShapeTextureCoord, MaskZeroScaleGuard)
{
  /* The zero scales of a never-defaulted settings block must not survive to a division. */
  ShapeTextureMapping map = mapping_with_size(float2(10.0f, 10.0f));
  map.scale = float2(0.0f, 0.0f);
  const ShapeTexFrame frame;
  const ShapeTexCoord coord = shape_texture_coord_mask(map, float2(5.0f, 5.0f), frame);
  /* The huge uv is clamped into the tile; the values must stay finite. */
  EXPECT_TRUE(std::isfinite(coord.uv.x) && std::isfinite(coord.uv.y));
}

TEST(ShapeTextureCoord, MaskAnchorShape)
{
  const ShapeTextureMapping map = mapping_with_size(float2(10.0f, 10.0f));
  ShapeTexFrame frame;
  frame.center = float2(100.0f, 100.0f);
  const ShapeTexCoord coord = shape_texture_coord_mask(map, float2(100.0f, 100.0f), frame);
  EXPECT_V2_NEAR(coord.uv, float2(0.0f, 0.0f), 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fit mapping
 * \{ */

TEST(ShapeTextureCoord, FitStretchIdentity)
{
  const ShapeTextureMapping map = mapping_with_size(float2(64.0f, 64.0f));
  const ShapeTexFrame frame;
  const ShapeSample sample = sample_with_uv(float2(0.25f, 0.75f));
  const ShapeTexCoord coord = shape_texture_coord_fit(map, sample, frame);
  EXPECT_V2_NEAR(coord.uv, float2(0.25f, 0.75f), 1e-6f);
}

TEST(ShapeTextureCoord, FitRepeat)
{
  ShapeTextureMapping map = mapping_with_size(float2(64.0f, 64.0f));
  map.repeat = float2(2.0f, 4.0f);
  const ShapeTexFrame frame;
  const ShapeSample sample = sample_with_uv(float2(0.25f, 0.75f));
  const ShapeTexCoord coord = shape_texture_coord_fit(map, sample, frame);
  /* The repeats scale the centered coordinates: 0.5 + (0.25 - 0.5) * 2 = 0.0. */
  EXPECT_NEAR(coord.uv.x, 0.0f, 1e-6f);
  EXPECT_NEAR(coord.uv.y, 0.5f, 1e-6f);
}

TEST(ShapeTextureCoord, FitContainLeavesGaps)
{
  /* A wide frame containing a square texture: the sides beyond [0, 1] are outside. */
  ShapeTextureMapping map = mapping_with_size(float2(64.0f, 64.0f));
  map.fit = PAINT_SHAPE_TEX_FIT_CONTAIN;
  map.tile_x = false;
  map.tile_y = false;
  ShapeTexFrame frame;
  frame.size_px = float2(200.0f, 100.0f);
  const ShapeSample edge = sample_with_uv(float2(0.0f, 0.5f));
  const ShapeTexCoord coord = shape_texture_coord_fit(map, edge, frame);
  /* sx = 200/64, sy = 100/64, s = sy: u spans 0.5 +- 1.5625 -> the left edge is outside. */
  EXPECT_FALSE(coord.inside);
  EXPECT_NEAR(coord.uv.x, 0.0f, 1e-5f); /* clamped */
}

TEST(ShapeTextureCoord, FitCoverCropsTexture)
{
  ShapeTextureMapping map = mapping_with_size(float2(64.0f, 64.0f));
  map.fit = PAINT_SHAPE_TEX_FIT_COVER;
  map.tile_x = false;
  map.tile_y = false;
  ShapeTexFrame frame;
  frame.size_px = float2(200.0f, 100.0f);
  /* The frame is relatively wider, so the texture is cropped horizontally: u stays inside
   * [0.5 - 0.32, 0.5 + 0.32]. */
  const ShapeSample center = sample_with_uv(float2(0.5f, 0.5f));
  const ShapeTexCoord coord = shape_texture_coord_fit(map, center, frame);
  EXPECT_V2_NEAR(coord.uv, float2(0.5f, 0.5f), 1e-6f);
  EXPECT_TRUE(coord.inside);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Along mapping
 * \{ */

TEST(ShapeTextureCoord, AlongUV)
{
  ShapeTextureMapping map = mapping_with_size(float2(100.0f, 20.0f));
  map.tile_length = 50.0f;
  ShapeTexFrame frame;
  ShapeSample sample;
  sample.stroke = 1.0f;
  sample.stroke_s = 25.0f;
  sample.stroke_len = 200.0f;
  sample.stroke_v = 0.5f;
  const ShapeTexCoord coord = shape_texture_coord_along(map, sample, frame);
  /* u = 25 / 50 = 0.5; v = 0.5 + 0.5 * 0.5 = 0.75. */
  EXPECT_NEAR(coord.uv.x, 0.5f, 1e-6f);
  EXPECT_NEAR(coord.uv.y, 0.75f, 1e-6f);
  EXPECT_TRUE(coord.inside);
}

TEST(ShapeTextureCoord, AlongWholeRepeats)
{
  ShapeTextureMapping map = mapping_with_size(float2(100.0f, 20.0f));
  map.tile_length = 30.0f;
  map.whole_repeats = true;
  ShapeTexFrame frame;
  ShapeSample sample;
  sample.stroke = 1.0f;
  sample.stroke_s = 0.0f;
  /* A 200 px closed stroke fits 7 repeats of ~28.57 px (round(200/30) = 7). */
  sample.stroke_len = 200.0f;
  const ShapeTexCoord coord = shape_texture_coord_along(map, sample, frame);
  EXPECT_NEAR(coord.uv.x, 0.0f, 1e-5f);
  /* The tile length follows from the repeat count: 200 / 7. */
  ShapeSample sample_end = sample;
  sample_end.stroke_s = 200.0f / 7.0f;
  const ShapeTexCoord coord_end = shape_texture_coord_along(map, sample_end, frame);
  EXPECT_NEAR(coord_end.uv.x, 1.0f, 1e-5f);
}

TEST(ShapeTextureCoord, AlongFlipAcross)
{
  ShapeTextureMapping map = mapping_with_size(float2(100.0f, 20.0f));
  map.tile_length = 100.0f;
  map.flip_y = true;
  ShapeTexFrame frame;
  ShapeSample sample;
  sample.stroke = 1.0f;
  sample.stroke_s = 0.0f;
  sample.stroke_v = 1.0f;
  const ShapeTexCoord coord = shape_texture_coord_along(map, sample, frame);
  /* v = 1 - (0.5 + 0.5 * 1) = 0. */
  EXPECT_NEAR(coord.uv.y, 0.0f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Wrapping and sampling
 * \{ */

TEST(ShapeTextureWrap, MirrorFold)
{
  bool inside = false;
  EXPECT_NEAR(shape_texture_wrap_axis(-0.25f, true, true, false, inside), 0.25f, 1e-6f);
  EXPECT_NEAR(shape_texture_wrap_axis(1.75f, true, true, false, inside), 0.25f, 1e-6f);
  EXPECT_TRUE(inside);
}

TEST(ShapeTextureWrap, FlipMirrorsContent)
{
  bool inside = false;
  EXPECT_NEAR(shape_texture_wrap_axis(0.25f, true, false, true, inside), 0.75f, 1e-6f);
  EXPECT_NEAR(shape_texture_wrap_axis(0.25f, false, false, true, inside), 0.75f, 1e-6f);
  EXPECT_TRUE(inside);
}

TEST(ShapeTextureSample, BilinearCenter)
{
  /* A 2x1 red/green image sampled at the texel centers. */
  ShapeTextureChannel channel;
  channel.kind = ShapeTextureChannel::Kind::Buffer;
  channel.size = int2(2, 1);
  float pixels[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
  channel.float_px = pixels;

  const float4 red = shape_texture_sample_rgba(channel, float2(0.25f, 0.5f));
  EXPECT_V4_NEAR(red, float4(1.0f, 0.0f, 0.0f, 1.0f), 1e-6f);
  const float4 green = shape_texture_sample_rgba(channel, float2(0.75f, 0.5f));
  EXPECT_V4_NEAR(green, float4(0.0f, 1.0f, 0.0f, 1.0f), 1e-6f);
  /* The midpoint blends 50/50. */
  const float4 mid = shape_texture_sample_rgba(channel, float2(0.5f, 0.5f));
  EXPECT_NEAR(mid.x, 0.5f, 1e-6f);
  EXPECT_NEAR(mid.y, 0.5f, 1e-6f);
}

TEST(ShapeTextureSample, ScalarLuminance)
{
  ShapeTextureChannel channel;
  channel.kind = ShapeTextureChannel::Kind::Buffer;
  channel.size = int2(1, 1);
  float pixels[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  channel.float_px = pixels;
  /* White reads as one. */
  EXPECT_NEAR(shape_texture_sample_scalar(channel, float2(0.5f, 0.5f)), 1.0f, 1e-6f);

  ShapeTextureChannel none_channel;
  EXPECT_NEAR(shape_texture_sample_scalar(none_channel, float2(0.5f, 0.5f)), 0.0f, 1e-6f);
}

TEST(ShapeTextureSample, NormalRotationAndFlip)
{
  ShapeTextureChannel channel;
  channel.kind = ShapeTextureChannel::Kind::Buffer;
  channel.size = int2(1, 1);
  /* Encoded +X normal (0.5 -> rgb (0.5, 0.5, 1)). */
  float pixels[4] = {0.75f, 0.5f, 1.0f, 1.0f};
  channel.float_px = pixels;

  const float3 raw = shape_texture_sample_normal(channel, float2(0.5f, 0.5f), 0.0f);
  EXPECT_NEAR(raw.x, 0.5f, 1e-5f);
  EXPECT_NEAR(raw.y, 0.0f, 1e-5f);
  EXPECT_NEAR(raw.z, 1.0f, 1e-5f);

  /* Rotated by 90 degrees, the +X slope becomes +Y. */
  const float3 rotated = shape_texture_sample_normal(channel, float2(0.5f, 0.5f), float(M_PI_2));
  EXPECT_NEAR(rotated.x, 0.0f, 1e-5f);
  EXPECT_NEAR(rotated.y, 0.5f, 1e-5f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Style helpers
 * \{ */

TEST(ShapeTextureFlags, MappingFlagsRoundTrip)
{
  ShapeTextureMapping map;
  map.tile_x = true;
  map.tile_y = true;
  map.mirror = false;
  map.flip_x = true;
  map.follow_rotation = true;
  map.use_tint = true;
  const short flag = shape_tex_mapping_flags(map);
  EXPECT_NE(flag & PAINT_SHAPE_TEX_TILE_X, 0);
  EXPECT_NE(flag & PAINT_SHAPE_TEX_FLIP_X, 0);
  EXPECT_NE(flag & PAINT_SHAPE_TEX_USE_TINT, 0);
  EXPECT_EQ(flag & PAINT_SHAPE_TEX_MIRROR, 0);
  EXPECT_EQ(flag & PAINT_SHAPE_TEX_ROTATE_90, 0);
}

TEST(ShapeTextureFrame, FrameUnion)
{
  Vector<PaintShape> shapes;
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(10.0f, 20.0f);
  rect.half_size = float2(5.0f, 10.0f);
  shapes.append(rect);
  PaintShape rect2 = rect;
  rect2.center = float2(30.0f, 20.0f);
  shapes.append(rect2);

  const ShapeTexFrame frame = shape_tex_frame_calc_union(shapes);
  EXPECT_V2_NEAR(frame.center, float2(20.0f, 20.0f), 1e-5f);
  EXPECT_NEAR(frame.size_px.x, 30.0f, 1e-5f);
  EXPECT_NEAR(frame.size_px.y, 20.0f, 1e-5f);
}

TEST(ShapeTextureFrame, SingleParametricFrame)
{
  PaintShape rect;
  rect.type = PAINT_SHAPE_RECT;
  rect.center = float2(10.0f, 20.0f);
  rect.half_size = float2(5.0f, 10.0f);
  rect.rotation = 0.0f;
  const ShapeTexFrame frame = shape_tex_frame_calc(rect);
  EXPECT_V2_NEAR(frame.center, float2(10.0f, 20.0f), 1e-5f);
  EXPECT_V2_NEAR(frame.size_px, float2(10.0f, 20.0f), 1e-5f);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
