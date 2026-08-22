/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "paint_image_curve_patch_raster.hh"

#include "BLI_math_vector.hh"

namespace blender::ed::sculpt_paint::tests {

TEST(image_curve_patch_raster, uv_ref_px_round_trip_square_tile)
{
  const float2 origin(0.0f, 0.0f);
  const int2 resolution(1024, 1024);
  const float2 uv(0.3f, 0.71f);

  const float2 px = image_curve_patch_uv_to_ref_px(uv, origin, resolution);
  const float2 back = image_curve_patch_ref_px_to_uv(px, origin, resolution);

  EXPECT_NEAR(back.x, uv.x, 1e-5f);
  EXPECT_NEAR(back.y, uv.y, 1e-5f);
}

TEST(image_curve_patch_raster, uv_ref_px_round_trip_non_square_tile)
{
  const float2 origin(2.0f, 0.0f); /* UDIM tile 1003: column 2, row 0. */
  const int2 resolution(2048, 512);
  const float2 uv(2.6f, 0.15f);

  const float2 px = image_curve_patch_uv_to_ref_px(uv, origin, resolution);
  const float2 back = image_curve_patch_ref_px_to_uv(px, origin, resolution);

  EXPECT_NEAR(back.x, uv.x, 1e-5f);
  EXPECT_NEAR(back.y, uv.y, 1e-5f);
}

TEST(image_curve_patch_raster, uv_to_ref_px_matches_expected_scale)
{
  /* A UV point at the tile's exact center must land at the pixel-space center, for a tile whose
   * UV origin is (0,0) -- the common case (tile 1001). */
  const float2 origin(0.0f, 0.0f);
  const int2 resolution(1000, 500);
  const float2 px = image_curve_patch_uv_to_ref_px(float2(0.5f, 0.5f), origin, resolution);

  EXPECT_NEAR(px.x, 500.0f, 1e-4f);
  EXPECT_NEAR(px.y, 250.0f, 1e-4f);
}

TEST(image_curve_patch_raster, blend_src_float_is_premultiplied)
{
  const float3 color(1.0f, 0.5f, 0.0f);
  const float4 src = image_curve_patch_blend_src_float(color, 0.4f);

  /* Premultiplied: rgb scaled by alpha, alpha carried through unchanged. */
  EXPECT_NEAR(src.x, 0.4f, 1e-6f);
  EXPECT_NEAR(src.y, 0.2f, 1e-6f);
  EXPECT_NEAR(src.z, 0.0f, 1e-6f);
  EXPECT_NEAR(src.w, 0.4f, 1e-6f);
}

TEST(image_curve_patch_raster, blend_src_float_opaque_is_the_plain_color)
{
  const float3 color(0.2f, 0.6f, 0.9f);
  const float4 src = image_curve_patch_blend_src_float(color, 1.0f);

  EXPECT_NEAR(src.x, color.x, 1e-6f);
  EXPECT_NEAR(src.y, color.y, 1e-6f);
  EXPECT_NEAR(src.z, color.z, 1e-6f);
  EXPECT_NEAR(src.w, 1.0f, 1e-6f);
}

TEST(image_curve_patch_raster, blend_src_byte_is_straight_not_premultiplied)
{
  const float3 color(1.0f, 1.0f, 1.0f);
  uchar src[4];
  /* Null colorspace: no conversion, so this isolates the premultiply question from color
   * management. */
  image_curve_patch_blend_src_byte(color, 0.5f, nullptr, src);

  /* Straight alpha: RGB stays at full white regardless of alpha -- premultiplying would have
   * halved it to ~128, which is the exact bug this test guards against. */
  EXPECT_EQ(src[0], 255);
  EXPECT_EQ(src[1], 255);
  EXPECT_EQ(src[2], 255);
  EXPECT_NEAR(int(src[3]), 128, 2); /* 0.5 * 255, uchar rounding tolerance. */
}

}  // namespace blender::ed::sculpt_paint::tests
