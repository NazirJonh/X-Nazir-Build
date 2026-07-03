/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_image_select_warp_intern.hh"

#include "testing/testing.h"

namespace blender::tests {

TEST(WarpBilinear, EvalIdentitySquareReturnsUV)
{
  const float2 p00(0.0f, 0.0f), p10(1.0f, 0.0f), p01(0.0f, 1.0f), p11(1.0f, 1.0f);
  for (const float2 uv : {float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
                          float2(1.0f, 1.0f), float2(0.3f, 0.7f)})
  {
    const float2 p = warp_bilinear_eval(p00, p10, p01, p11, uv.x, uv.y);
    EXPECT_NEAR(p.x, uv.x, 1e-5f);
    EXPECT_NEAR(p.y, uv.y, 1e-5f);
  }
}

TEST(WarpBilinear, CellCornersMatchGridIndexing)
{
  float2 pts[IMAGE_SELECT_WARP_POINT_COUNT];
  for (int y = 0; y < IMAGE_SELECT_WARP_GRID_SIZE; y++) {
    for (int x = 0; x < IMAGE_SELECT_WARP_GRID_SIZE; x++) {
      pts[y * IMAGE_SELECT_WARP_GRID_SIZE + x] = float2(float(x), float(y));
    }
  }
  float2 p00, p10, p01, p11;
  warp_grid_cell_corners(pts, IMAGE_SELECT_WARP_GRID_SIZE, 1, 1, &p00, &p10, &p01, &p11);
  EXPECT_EQ(p00, float2(1.0f, 1.0f));
  EXPECT_EQ(p10, float2(2.0f, 1.0f));
  EXPECT_EQ(p01, float2(1.0f, 2.0f));
  EXPECT_EQ(p11, float2(2.0f, 2.0f));
}

TEST(WarpBilinear, CellCornersMatchGridIndexingForNonDefaultGridSize)
{
  /* Grid Size is configurable via ImagePaintSettings::warp_grid_size (2-10); verify the indexing
   * math holds for a size other than the compiled-in default of 4. */
  constexpr int grid_size = 5;
  float2 pts[grid_size * grid_size];
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      pts[y * grid_size + x] = float2(float(x), float(y));
    }
  }
  float2 p00, p10, p01, p11;
  warp_grid_cell_corners(pts, grid_size, 3, 2, &p00, &p10, &p01, &p11);
  EXPECT_EQ(p00, float2(3.0f, 2.0f));
  EXPECT_EQ(p10, float2(4.0f, 2.0f));
  EXPECT_EQ(p01, float2(3.0f, 3.0f));
  EXPECT_EQ(p11, float2(4.0f, 3.0f));
}

TEST(WarpSmooth, PassesThroughControlPoints)
{
  /* Interpolating spline: evaluated at integer grid coordinates it must return the control
   * points exactly (the property that keeps a dragged handle under the cursor). */
  constexpr int grid_size = 4;
  float2 pts[grid_size * grid_size];
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      pts[y * grid_size + x] = float2(float(x) + 0.1f * float(y),
                                      float(y) - 0.2f * float(x) * float(x));
    }
  }
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      const float2 p = warp_grid_eval_smooth(pts, grid_size, float(x), float(y));
      EXPECT_NEAR(p.x, pts[y * grid_size + x].x, 1e-4f);
      EXPECT_NEAR(p.y, pts[y * grid_size + x].y, 1e-4f);
    }
  }
}

TEST(WarpSmooth, ReproducesLinearGridExactly)
{
  /* Catmull-Rom reproduces affine (degree-1) fields exactly, including at the borders -- this is
   * why the border phantom points are linearly extrapolated rather than edge-clamped. A clamped
   * implementation would fail the near-border samples below, breaking the init/resample invariant
   * (undeformed grid must stay evenly spaced). */
  constexpr int grid_size = 4;
  auto f = [](float x, float y) {
    return float2(2.0f * x - 0.5f * y + 1.0f, 0.3f * x + 1.5f * y - 2.0f);
  };
  float2 pts[grid_size * grid_size];
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      pts[y * grid_size + x] = f(float(x), float(y));
    }
  }
  for (const float2 g : {float2(0.5f, 0.5f), float2(0.1f, 2.7f), float2(2.9f, 0.05f),
                         float2(1.5f, 1.5f)})
  {
    const float2 p = warp_grid_eval_smooth(pts, grid_size, g.x, g.y);
    const float2 e = f(g.x, g.y);
    EXPECT_NEAR(p.x, e.x, 1e-4f) << "g=" << g.x << "," << g.y;
    EXPECT_NEAR(p.y, e.y, 1e-4f) << "g=" << g.x << "," << g.y;
  }
}

TEST(WarpSmooth, IsC1ContinuousAcrossCellSeam)
{
  /* Bilinear kinks (derivative jumps) at a cell seam; Catmull-Rom does not. Bump one interior
   * column and compare the x-derivative approaching the seam gx=2 from both sides. */
  constexpr int grid_size = 4;
  float2 pts[grid_size * grid_size];
  for (int y = 0; y < grid_size; y++) {
    for (int x = 0; x < grid_size; x++) {
      pts[y * grid_size + x] = float2(float(x), float(y) + (x == 2 ? 0.6f : 0.0f));
    }
  }
  const float h = 1e-3f;
  const float gy = 0.5f;
  const float2 left = (warp_grid_eval_smooth(pts, grid_size, 2.0f, gy) -
                       warp_grid_eval_smooth(pts, grid_size, 2.0f - h, gy)) / h;
  const float2 right = (warp_grid_eval_smooth(pts, grid_size, 2.0f + h, gy) -
                        warp_grid_eval_smooth(pts, grid_size, 2.0f, gy)) / h;
  EXPECT_NEAR(left.x, right.x, 1e-2f);
  EXPECT_NEAR(left.y, right.y, 1e-2f);
}

}  // namespace blender::tests
