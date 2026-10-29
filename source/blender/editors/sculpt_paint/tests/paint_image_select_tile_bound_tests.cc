/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../selection/paint_image_select_intern.hh"

#include "testing/testing.h"

namespace blender::tests {

/** Upper bound of the UDIM tile loop for v: the tile containing the last covered UV row. */
static int bound_floor(const float v)
{
  return image_select_tile_bound_floor(v);
}

TEST(ImageSelectTileBound, InsideTileFloors)
{
  EXPECT_EQ(bound_floor(0.999f), 0);
  EXPECT_EQ(bound_floor(1.001f), 1);
  EXPECT_EQ(bound_floor(1.999f), 1);
  EXPECT_EQ(bound_floor(2.5f), 2);
  EXPECT_EQ(bound_floor(9.999f), 9);
}

TEST(ImageSelectTileBound, ExactEdgeBelongsToTileBelow)
{
  /* A face reaching u == 1.0 covers no area of the next tile (the default 0-1 unwrap ends there):
   * the bound is the tile below, so the empty tile 1002 stays out of the loop. */
  EXPECT_EQ(bound_floor(1.0f), 0);
  EXPECT_EQ(bound_floor(2.0f), 1);
  EXPECT_EQ(bound_floor(10.0f), 9);
}

TEST(ImageSelectTileBound, ClampedAtZero)
{
  /* Zero and negative values clamp to tile 0 instead of an empty loop: a degenerate face resting
   * on the tile border still reaches the tile it touches. */
  EXPECT_EQ(bound_floor(0.0f), 0);
  EXPECT_EQ(bound_floor(-0.5f), 0);
  EXPECT_EQ(bound_floor(-1.0f), 0);
  EXPECT_EQ(bound_floor(-2.5f), 0);
}

/** The tile index range covered by a bounds rect, as the function under test returns it. */
static std::pair<int2, int2> tile_range(const float xmin,
                                        const float ymin,
                                        const float xmax,
                                        const float ymax)
{
  rctf bounds;
  BLI_rctf_init(&bounds, xmin, xmax, ymin, ymax);
  return image_select_udim_tile_range(bounds);
}

TEST(ImageSelectUDIMTileRange, InGridBounds)
{
  /* A face spanning one-and-a-half tiles touches columns 0-1 and row 0 only. */
  const auto [tile_min, tile_max] = tile_range(0.25f, 0.25f, 1.5f, 0.75f);
  EXPECT_EQ(tile_min, int2(0, 0));
  EXPECT_EQ(tile_max, int2(1, 0));
}

TEST(ImageSelectUDIMTileRange, ExactEdgeStaysInTileBelow)
{
  /* The default 0-1 unwrap ends exactly at u == 1.0: the empty tile 1002 must stay out of the
   * range, while the tile the face starts in stays in. */
  const auto [tile_min, tile_max] = tile_range(0.0f, 0.0f, 1.0f, 1.0f);
  EXPECT_EQ(tile_min, int2(0, 0));
  EXPECT_EQ(tile_max, int2(0, 0));
}

TEST(ImageSelectUDIMTileRange, ClampedToGrid)
{
  /* UVs outside the valid grid clamp to the first tile instead of an empty or overflowing loop,
   * so every tile in the range stays below the #IMA_UDIM_MAX tile-number limit. */
  const auto [tile_min, tile_max] = tile_range(-2.5f, -1.0f, 12.0f, 150.0f);
  EXPECT_EQ(tile_min, int2(0, 0));
  EXPECT_EQ(tile_max, int2(9, IMAGE_SELECT_UDIM_TILE_ROW_MAX));
}

}  // namespace blender::tests
