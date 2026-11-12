/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The Projection frame's controller operators: create, select and place the Empty that carries
 * the shown row's projection, the way Ucupaint manages its decal Empty.
 */

namespace blender {
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::projection {

/** Create an Empty at the 3D cursor, parented to the active object, and assign it as the shown
 * row's projection controller. */
void PAINT_OT_layer_projection_add_controller(wmOperatorType *ot);

/** Leave paint mode and select the shown row's projection controller in the viewport. */
void PAINT_OT_layer_projection_select_controller(wmOperatorType *ot);

/** Move the shown row's projection controller to the 3D cursor. */
void PAINT_OT_layer_projection_controller_to_cursor(wmOperatorType *ot);

}  // namespace blender::ed::sculpt_paint::projection
