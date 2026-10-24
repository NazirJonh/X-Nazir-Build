/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_document.hh"

#include <algorithm>

#include "BLI_index_range.hh"

namespace blender::ed::sculpt_paint {

int curve_patch_index_for_source_curve(const CurvePatchDocument &document,
                                       const int source_curve_index)
{
  for (const int i : document.patches.index_range()) {
    if (document.patches[i].source_curve_index == source_curve_index) {
      return i;
    }
  }
  return -1;
}

void curve_patch_document_active_patch_set(CurvePatchDocument &document, const int index)
{
  if (document.patches.is_empty()) {
    document.active_patch = 0;
    document.active_point = -1;
    return;
  }
  document.active_patch = std::clamp(index, 0, int(document.patches.size()) - 1);
  /* The point belonged to the patch that was active until now. */
  document.active_point = -1;
  document.texture_pick_point = -1;
}

}  // namespace blender::ed::sculpt_paint
