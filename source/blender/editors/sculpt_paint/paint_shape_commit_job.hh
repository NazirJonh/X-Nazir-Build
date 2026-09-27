/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * S6 begin/end hooks of a Paint Shape document commit / bake job. No-ops now; when Stack Layers
 * merge they set and clear `MA_PAINT_LAYERS_BAKE_SCHEDULED` on \a ma so the material is not
 * reported stale while a bake is still writing (Stack_Layers_Integration.md D8).
 */

#pragma once

namespace blender {
struct Material;
}

namespace blender::ed::sculpt_paint::shape {

/** Called on the main thread before a commit / bake job starts writing. */
void shape_commit_job_begin(Material *ma);
/** Called on the main thread when the job finished (success or failure). */
void shape_commit_job_end(Material *ma);

}  // namespace blender::ed::sculpt_paint::shape
