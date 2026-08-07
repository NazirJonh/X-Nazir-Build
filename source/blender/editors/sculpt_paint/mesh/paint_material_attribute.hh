/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

namespace blender {

struct wmOperatorType;

namespace ed::sculpt_paint {
void PAINT_OT_material_attribute_add(wmOperatorType *ot);
void PAINT_OT_material_attribute_remove(wmOperatorType *ot);
}  // namespace ed::sculpt_paint

}  // namespace blender
