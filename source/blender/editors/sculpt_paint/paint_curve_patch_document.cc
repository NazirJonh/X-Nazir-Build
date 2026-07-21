/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_document.hh"

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

}  // namespace blender::ed::sculpt_paint
