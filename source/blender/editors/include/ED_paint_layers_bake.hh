/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * The wmJob half of the paint-layer bake planner: a heavy row (see
 * #BKE_paint_layers_bake_is_heavy) is computed on a worker from a localized description and
 * written back on the main thread. The synchronous half stays in `BKE_paint_layers_bake_plan_run`,
 * run at the regeneration point; this only drains what that point leaves pending, from the editor
 * side of the depsgraph update.
 */

namespace blender {
struct Main;
struct wmWindow;
struct wmWindowManager;
}  // namespace blender

namespace blender::ed::material_bake {

/**
 * Start a job for every layered material with a heavy, not-yet-valid baked row and no job of its
 * own already in flight, and the render of every Material row that is due
 * (#material_bake_layered_rows_ensure). Called on the main thread after the depsgraph update; a
 * material with nothing pending is skipped without touching the job system.
 */
void paint_layers_bake_jobs_ensure(wmWindowManager &wm, wmWindow *win, Main &bmain);

}  // namespace blender::ed::material_bake
