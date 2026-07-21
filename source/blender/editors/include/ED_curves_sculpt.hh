/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

struct bContext;

namespace blender {

void ED_operatortypes_sculpt_curves();

/**
 * Restart the mode-transfer flash on every Curves object the current multi-object edit scope
 * covers, so that changing the scope reads as "this is what you are editing now".
 */
void ED_curves_sculpt_flash_edit_scope(bContext *C);

}  // namespace blender
