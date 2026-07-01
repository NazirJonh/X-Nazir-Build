/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "interface_grid_view.hh"
#include "testing/testing.h"

namespace blender::ui::tests {

TEST(grid_core_math, window_size_caps_to_visible_plus_buffer)
{
  /* 3 visible rows, 4 cols => (3+1)*4 = 16 visible slots, capped by max. */
  EXPECT_EQ(grid_build_window_size(/*grip_px*/ 300, /*tile_h*/ 100, /*cols*/ 4, /*max*/ 512), 16);
}

TEST(grid_core_math, window_size_clamps_rows_to_16)
{
  EXPECT_EQ(grid_build_window_size(/*grip_px*/ 100000, /*tile_h*/ 1, /*cols*/ 1, /*max*/ 512), 17);
}

TEST(grid_core_math, total_rows_ceil)
{
  EXPECT_EQ(grid_total_rows(/*item_count*/ 9, /*cols*/ 4), 3);
  EXPECT_EQ(grid_total_rows(/*item_count*/ 0, /*cols*/ 4, /*fallback_rows*/ 2), 2);
}

TEST(grid_core_math, max_scroll_px)
{
  /* 10 items in 2 cols = 5 content rows; tile 100, viewport 250 -> 500 - 250. */
  EXPECT_EQ(grid_max_scroll_px(/*item_count*/ 10, /*cols*/ 2, /*tile_h*/ 100, /*viewport*/ 250),
            250);
  /* Everything fits. */
  EXPECT_EQ(grid_max_scroll_px(4, 2, 100, 250), 0);
  /* Empty grid. */
  EXPECT_EQ(grid_max_scroll_px(0, 2, 100, 250), 0);
  /* Viewport an exact row multiple: whole-row scroll range, no sub-row remainder. */
  EXPECT_EQ(grid_max_scroll_px(10, 2, 100, 200), 300);
  /* Degenerate guards behave like the 1-clamped inputs. */
  EXPECT_EQ(grid_max_scroll_px(10, 0, 100, 250), grid_max_scroll_px(10, 1, 100, 250));
  EXPECT_EQ(grid_max_scroll_px(10, 2, 0, 250), grid_max_scroll_px(10, 2, 1, 250));
}

TEST(grid_core_math, clamp_scroll_px)
{
  EXPECT_EQ(grid_clamp_scroll_px(/*scroll_px*/ -5, /*max*/ 250), 0);
  EXPECT_EQ(grid_clamp_scroll_px(50, 250), 50);
  EXPECT_EQ(grid_clamp_scroll_px(300, 250), 250);
  EXPECT_EQ(grid_clamp_scroll_px(10, 0), 0);
  /* A negative max behaves like 0 (no scrollable range). */
  EXPECT_EQ(grid_clamp_scroll_px(10, -4), 0);
}

TEST(grid_core_math, rows_to_build)
{
  /* Exact multiple, no offset: exactly the visible rows, no buffer row. */
  EXPECT_EQ(grid_rows_to_build(/*viewport_px*/ 300, /*tile_h*/ 100, /*offset_px*/ 0), 3);
  /* A sub-row offset makes the window intersect one more row at the bottom. */
  EXPECT_EQ(grid_rows_to_build(300, 100, 40), 4);
  /* Non-multiple viewport: the partial bottom row is always built. */
  EXPECT_EQ(grid_rows_to_build(250, 100, 0), 3);
  EXPECT_EQ(grid_rows_to_build(250, 100, 40), 3); /* ceil(290 / 100). */
  EXPECT_EQ(grid_rows_to_build(250, 100, 60), 4); /* ceil(310 / 100). */
  /* Guards. */
  EXPECT_EQ(grid_rows_to_build(0, 100, 0), 1);
  EXPECT_EQ(grid_rows_to_build(250, 0, 0), grid_rows_to_build(250, 1, 0));
}

} /* namespace blender::ui::tests */
