/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * The union of the pixels a paint writer actually stored, in the exclusive convention of the
 * partial-update log: #WrittenBounds::as_exclusive_rcti gives `[xmin, xmax) x [ymin, ymax)`,
 * ready for #BKE_image_partial_update_mark_region and for an undo-tile capture. Every pixel writer
 * accumulates into one of these -- per thread where the write is parallel -- and merges it with
 * #WrittenBounds::merge, so no caller converts an inclusive min/max by hand and no mark can be one
 * texel short of what the blend stored.
 */

#include <algorithm>

#include "BLI_rect.h"
#include "BLI_utildefines.h"

namespace blender::ed::sculpt_paint {

/** A union of written texels, empty until the first #add, in the log's exclusive convention. */
struct WrittenBounds {
  bool empty = true;
  /** `[xmin, xmax) x [ymin, ymax)`, meaningful only while #empty is false. */
  int xmin = 0, xmax = 0, ymin = 0, ymax = 0;

  /** Add the single texel `(x, y)` to the union. */
  void add(const int x, const int y)
  {
    if (empty) {
      xmin = x;
      xmax = x + 1;
      ymin = y;
      ymax = y + 1;
      empty = false;
      return;
    }
    xmin = std::min(xmin, x);
    xmax = std::max(xmax, x + 1);
    ymin = std::min(ymin, y);
    ymax = std::max(ymax, y + 1);
  }

  /** Fold another union into this one; an empty \a other changes nothing. */
  void merge(const WrittenBounds &other)
  {
    if (other.empty) {
      return;
    }
    if (empty) {
      *this = other;
      return;
    }
    xmin = std::min(xmin, other.xmin);
    xmax = std::max(xmax, other.xmax);
    ymin = std::min(ymin, other.ymin);
    ymax = std::max(ymax, other.ymax);
  }

  bool is_empty() const
  {
    return empty;
  }

  /** The union as the exclusive rectangle the partial-update log and the undo tiles take. */
  rcti as_exclusive_rcti() const
  {
    BLI_assert(!empty);
    return rcti{xmin, xmax, ymin, ymax};
  }
};

}  // namespace blender::ed::sculpt_paint
