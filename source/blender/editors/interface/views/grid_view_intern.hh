/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 *
 * Shared helpers for the grid-view translation units (view / input / layout).
 * Not a public API — callers outside `views/` should use #UI_grid_view.hh.
 */

#include <optional>

#include "BLI_assert.h"

#include "UI_grid_view.hh"

namespace blender::ui {

inline std::optional<int> grid_view_find_filtered_item_index(const AbstractGridViewItem &item)
{
  BLI_assert(item.is_filtered_visible());

  const AbstractGridView &view = item.get_view();
  std::optional<int> index;

  int i = 0;
  view.foreach_filtered_item([&](AbstractGridViewItem &iter_item) {
    if (&item == &iter_item) {
      index = i;
    }
    i++;
  });

  return index;
}

}  // namespace blender::ui
