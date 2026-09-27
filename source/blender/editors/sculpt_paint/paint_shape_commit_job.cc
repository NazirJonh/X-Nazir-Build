/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * See #paint_shape_commit_job.hh. The hooks are intentionally empty in this branch; they exist so
 * the Stack Layers merge has a single place to bracket a bake.
 */

#include "paint_shape_commit_job.hh"

namespace blender::ed::sculpt_paint::shape {

void shape_commit_job_begin(Material * /*ma*/) {}
void shape_commit_job_end(Material * /*ma*/) {}

}  // namespace blender::ed::sculpt_paint::shape
