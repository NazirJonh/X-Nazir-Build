/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

struct wmOperatorType;

namespace blender {
namespace ed::sculpt_paint::image::curve_patch::edit {

/**
 * Stage 7 modal editor for `BRUSH_STROKE_CURVE_PATCH` in the Image Editor. The operator has
 * no properties -- it pulls the live session state through
 * #image_curve_patch_session_active_get() and routes user input through
 * #image_curve_patch_session_commit / #image_curve_patch_session_cancel /
 * #image_curve_patch_session_restore_and_restamp.
 *
 * The first pass implements the minimum viable edit cycle (point drag, Enter=commit,
 * Esc=cancel, pass-through for other events). Insert/remove/radius-handle are documented
 * but not yet wired -- they bottom out in the same `restore_and_restamp()` call so the
 * pattern is already there.
 */
void PAINT_OT_image_curve_patch_edit(wmOperatorType *ot);

/**
 * Context-menu operators for the live 2D session. A popup can only invoke operators, never call
 * back into the running modal, so each entry of the right-click menu needs one of these. They
 * resolve their data through #image_curve_patch_session_active_get(), not through an #Object, so
 * none of the `SCULPT_OT_curve_patch_*` equivalents can be reused.
 */
void PAINT_OT_image_curve_patch_handle_type_set(wmOperatorType *ot);
void PAINT_OT_image_curve_patch_delete_point(wmOperatorType *ot);
void PAINT_OT_image_curve_patch_toggle_cyclic(wmOperatorType *ot);
void PAINT_OT_image_curve_patch_switch_direction(wmOperatorType *ot);

}  // namespace ed::sculpt_paint::image::curve_patch::edit
}  // namespace blender
