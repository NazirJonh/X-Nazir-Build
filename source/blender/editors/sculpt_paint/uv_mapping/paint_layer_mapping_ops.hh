/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The Mapping panel's operators: the scale lock toggle of the shown row, the reset of its
 * mapping values and the Mirror center placement. The gizmo reaches the same edits through
 * #PaintLayerRowMappingTarget, which resolves the same row.
 */

#include "DNA_uuid_types.h"

namespace blender {
struct bContext;
struct wmOperator;
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::uv_mapping {

/** Toggle the scale lock of the row the Mapping panel and the gizmo edit. */
void PAINT_OT_layer_mapping_scale_lock_toggle(wmOperatorType *ot);

/** Put the shown row's UV mapping values back to their defaults, all at once or one part at a
 * time. */
void PAINT_OT_layer_mapping_reset(wmOperatorType *ot);

/** Switch a layer row's Mirror fold on or off across both axes (the Outliner row button). */
void PAINT_OT_layer_mapping_mirror_toggle(wmOperatorType *ot);

/** Place the shown row's Mirror fold center by clicking the Image Editor, or at a given UV. */
void PAINT_OT_layer_mapping_mirror_set_point(wmOperatorType *ot);

/** Set the shown row's Mirror fold center from the sculpt 3D cursor, projected to UV. */
void PAINT_OT_layer_mapping_mirror_from_cursor(wmOperatorType *ot);

/** Place the shown row's Mirror fold center on the mesh surface in the 3D Viewport. */
void PAINT_OT_layer_mapping_mirror_pick_3d(wmOperatorType *ot);

/**
 * The row a Mirror placement operator edits: the marker a panel button passes, or a nil marker for
 * the row the context shows. Shared with #layer_mapping_mirror_marker_define, which declares the
 * property, so every such operator reads the same name off its own instance.
 */
bUUID layer_mapping_mirror_marker_read(wmOperator *op);
void layer_mapping_mirror_marker_define(wmOperatorType *ot);

/**
 * Poll shared by the Mirror placement operators: a mapped row the context shows, and with
 * \a need_image_editor only while the Image Editor shows that material's image.
 */
bool layer_mapping_mirror_target_poll(bContext *C, bool need_image_editor);

}  // namespace blender::ed::sculpt_paint::uv_mapping
