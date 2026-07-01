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

int grid_max_scroll_px(const int item_count,
                       const int cols,
                       const int tile_h,
                       const int viewport_px)
{
  const int safe_cols = max_ii(1, cols);
  const int safe_tile_h = max_ii(1, tile_h);
  const int content_rows = (item_count > 0) ?
                               int(divide_ceil_u(uint(item_count), uint(safe_cols))) :
                               0;
  /* Pixel-exact: the whole content minus the raw pixel viewport, so scrolling stops exactly at
   * the content end (revealing a partial bottom row fully) with no over- or under-scroll. */
  return max_ii(0, content_rows * safe_tile_h - viewport_px);
}

int grid_clamp_scroll_px(const int scroll_px, const int max_scroll_px)
{
  return clamp_i(scroll_px, 0, max_ii(0, max_scroll_px));
}

int grid_rows_to_build(const int viewport_px, const int tile_h, const int offset_px)
{
  const int safe_tile_h = max_ii(1, tile_h);
  /* Rows the clip window intersects: `ceil((viewport + offset) / tile)`. A sub-row offset shifts
   * content up, so the window intersects one more row at the bottom; without building it that
   * partial row would vanish instead of being drawn clipped. */
  return max_ii(1, (viewport_px + max_ii(0, offset_px) + safe_tile_h - 1) / safe_tile_h);
}

} /* namespace blender::ui */
