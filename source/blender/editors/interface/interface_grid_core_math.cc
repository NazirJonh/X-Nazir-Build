/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "interface_grid_view.hh"

#include "BLI_math_base.h"

namespace blender::ui {

int grid_build_window_size(const int grip_px,
                           const int tile_h,
                           const int cols,
                           const int max_items)
{
  const int safe_cols = max_ii(1, cols);
  const int safe_tile_h = max_ii(1, tile_h);
  const int effective_rows = clamp_i(
      int(divide_ceil_u(uint(max_ii(grip_px, 0)), uint(safe_tile_h))), 1, 16);
  const int visible_slots = max_ii(1, (effective_rows + 1) * safe_cols);
  return min_ii(visible_slots, max_items);
}

int grid_total_rows(const int item_count, const int cols, const int fallback_rows)
{
  const int safe_cols = max_ii(1, cols);
  if (item_count <= 0) {
    return max_ii(1, fallback_rows);
  }
  return int(divide_ceil_u(uint(item_count), uint(safe_cols)));
}

int grid_clamp_scroll_row(const int scroll_row, const int max_scroll_row)
{
  return clamp_i(scroll_row, 0, max_ii(0, max_scroll_row));
}

} /* namespace blender::ui */
