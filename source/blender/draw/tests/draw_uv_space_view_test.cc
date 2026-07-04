/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "draw_uv_space_view.hh"

namespace blender::draw::tests {

/* A View2D rect covering exactly the 0..1 UV range. */
static rctf unit_rect()
{
  rctf rect;
  rect.xmin = 0.0f;
  rect.xmax = 1.0f;
  rect.ymin = 0.0f;
  rect.ymax = 1.0f;
  return rect;
}

static float2 project(const float4x4 &mat, const float2 &uv)
{
  const float4 clip = mat * float4(uv.x, uv.y, 0.0f, 1.0f);
  return float2(clip.x / clip.w, clip.y / clip.w);
}

/* Note: EXPECT_V2_NEAR expands its arguments twice, so every projected value goes into a local
 * first. */

TEST(draw_uv_space_view, unit_rect_maps_uv_to_ndc_corners)
{
  const float4x4 mat = uv_space_projection_get(unit_rect(), int2(0, 0), float2(0.0f));

  const float2 bottom_left = project(mat, float2(0.0f, 0.0f));
  const float2 top_right = project(mat, float2(1.0f, 1.0f));
  const float2 center = project(mat, float2(0.5f, 0.5f));

  EXPECT_V2_NEAR(bottom_left, float2(-1.0f, -1.0f), 1e-5f);
  EXPECT_V2_NEAR(top_right, float2(1.0f, 1.0f), 1e-5f);
  EXPECT_V2_NEAR(center, float2(0.0f, 0.0f), 1e-5f);
}

TEST(draw_uv_space_view, tile_offset_shifts_by_exactly_one)
{
  const float4x4 base = uv_space_projection_get(unit_rect(), int2(0, 0), float2(0.0f));
  const float4x4 tile = uv_space_projection_get(unit_rect(), int2(1, 0), float2(0.0f));

  /* UV (1.5, 0.5) sits in the middle of tile 1, exactly where UV (0.5, 0.5) sits in tile 0. */
  const float2 in_tile = project(tile, float2(1.5f, 0.5f));
  const float2 in_base = project(base, float2(0.5f, 0.5f));

  EXPECT_V2_NEAR(in_tile, in_base, 1e-5f);
}

TEST(draw_uv_space_view, zoom_and_pan_compose)
{
  /* Rect zoomed 2x and panned: covers UV 0.25..0.75 horizontally. */
  rctf rect;
  rect.xmin = 0.25f;
  rect.xmax = 0.75f;
  rect.ymin = 0.0f;
  rect.ymax = 1.0f;

  const float4x4 mat = uv_space_projection_get(rect, int2(0, 0), float2(0.0f));

  const float2 bottom_left = project(mat, float2(0.25f, 0.0f));
  const float2 top_right = project(mat, float2(0.75f, 1.0f));

  EXPECT_V2_NEAR(bottom_left, float2(-1.0f, -1.0f), 1e-5f);
  EXPECT_V2_NEAR(top_right, float2(1.0f, 1.0f), 1e-5f);
}

TEST(draw_uv_space_view, jitter_offsets_result_and_is_neutral_when_zero)
{
  const float4x4 no_jitter = uv_space_projection_get(unit_rect(), int2(0, 0), float2(0.0f));
  const float4x4 jittered = uv_space_projection_get(
      unit_rect(), int2(0, 0), float2(0.125f, -0.25f));

  const float2 a = project(no_jitter, float2(0.5f, 0.5f));
  const float2 b = project(jittered, float2(0.5f, 0.5f));
  const float2 delta = b - a;

  EXPECT_V2_NEAR(delta, float2(0.125f, -0.25f), 1e-5f);
  EXPECT_V2_NEAR(a, float2(0.0f, 0.0f), 1e-5f);
}

}  // namespace blender::draw::tests
