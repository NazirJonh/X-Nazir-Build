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

TEST(grid_core_math, clamp_scroll_row)
{
  EXPECT_EQ(grid_clamp_scroll_row(/*scroll*/ 99, /*max_scroll*/ 5), 5);
  EXPECT_EQ(grid_clamp_scroll_row(/*scroll*/ -3, /*max_scroll*/ 5), 0);
}

} /* namespace blender::ui::tests */
