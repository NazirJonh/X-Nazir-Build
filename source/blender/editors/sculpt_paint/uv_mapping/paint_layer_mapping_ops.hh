/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The Mapping panel's operators: the scale lock toggle of the shown row, and the reset of its
 * mapping values. The gizmo reaches the same edits through #PaintLayerRowMappingTarget, which
 * resolves the same row.
 */

namespace blender {
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::uv_mapping {

/** Toggle the scale lock of the row the Mapping panel and the gizmo edit. */
void PAINT_OT_layer_mapping_scale_lock_toggle(wmOperatorType *ot);

/** Put the shown row's UV mapping values back to their defaults, all at once or one part at a
 * time. */
void PAINT_OT_layer_mapping_reset(wmOperatorType *ot);

}  // namespace blender::ed::sculpt_paint::uv_mapping
