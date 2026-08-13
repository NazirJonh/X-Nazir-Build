/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Session-local undo for a live Curve Patch edit. See `CurvePatchEditState::undo_steps` for
 * why this cannot go through Blender's own undo systems.
 */

#include <algorithm>

#include "DNA_object_types.h"

#include "BKE_context.hh"

#include "paint_curve_patch_edit_intern.hh"
#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {

/* Deep enough for any realistic editing session; the snapshots are a handful of control points
 * each, so the cap exists to bound a pathological session rather than to save meaningful memory.
 */
static constexpr int CURVE_PATCH_UNDO_STEPS_MAX = 64;

/* Record the CURRENT state as a new step. Called after an action completes -- once per action, not
 * once per event, so a drag is a single step. Declared in `paint_curve_patch_session.hh` because
 * `paint_curve_patch_session.cc`'s #ED_curve_patch_session_undo_push also needs it, for the
 * Transform system's G/R/S handle drags. */
void curve_patch_undo_push(CurvePatchSession &patch)
{
  /* Anything above the cursor is a redo branch the new edit invalidates. */
  patch.edit.undo_steps.resize(patch.edit.undo_step_current + 1);

  CurvePatchEditStep step;
  step.items.reserve(patch.patches.size());
  for (const CurvePatchItem &item : patch.patches) {
    step.items.append({item.control_curve, item.params});
  }
  step.active_patch = patch.active_patch;
  patch.edit.undo_steps.append(std::move(step));

  if (patch.edit.undo_steps.size() > CURVE_PATCH_UNDO_STEPS_MAX) {
    patch.edit.undo_steps.remove(0);
    /* Step 0 is no longer the anchor stroke's state -- #curve_patch_undo_step_back must not treat
     * "back at index 0" as "back at the start of the session" any more, or Ctrl+Z past a
     * long-running edit's trimmed history would cancel the whole patch instead of simply having
     * nothing left to undo. */
    patch.edit.undo_anchor_trimmed = true;
  }
  patch.edit.undo_step_current = int(patch.edit.undo_steps.size()) - 1;
}

static void curve_patch_undo_restore(bContext &C, Object &ob, CurvePatchSession &patch)
{
  const CurvePatchEditStep &step = patch.edit.undo_steps[patch.edit.undo_step_current];
  /* Resized rather than rebuilt: only the three snapshotted fields are restored, so each patch
   * keeps its surface snapshot and its derived geometry, which the next re-stamp overwrites
   * anyway. A step can hold fewer patches than the session currently has. */
  patch.patches.resize(step.items.size());
  for (const int i : step.items.index_range()) {
    patch.patches[i].control_curve = step.items[i].curve;
    patch.patches[i].params = step.items[i].params;
  }
  patch.active_patch = std::min(step.active_patch, int(patch.patches.size()) - 1);
  /* The restored curve may hold fewer points than the one just replaced. */
  patch.edit.active_point = -1;

  curve_patch_edit_status_set(&C, patch);
  curve_patch_restore_and_restamp(C, ob, patch);
  curve_patch_tag_viewports_redraw_after_edit(C, ob, patch);
}

/* Returns false when there is nothing left to undo, i.e. the session is back at the state the
 * anchor stroke produced. The caller then cancels the patch outright.
 *
 * When the history was trimmed (#CurvePatchEditState::undo_anchor_trimmed), index 0 is some
 * later state instead, so "already at 0" no longer means "back at the start" -- it means the
 * session simply cannot recall anything earlier, and the caller must NOT cancel the patch over
 * that. The step is a no-op in that case; there is nothing further back to restore. */
bool curve_patch_undo_step_back(bContext &C, Object &ob, CurvePatchSession &patch)
{
  if (patch.edit.undo_step_current <= 0) {
    return patch.edit.undo_anchor_trimmed;
  }
  patch.edit.undo_step_current--;
  curve_patch_undo_restore(C, ob, patch);
  return true;
}

void curve_patch_undo_step_forward(bContext &C, Object &ob, CurvePatchSession &patch)
{
  if (patch.edit.undo_step_current + 1 >= int(patch.edit.undo_steps.size())) {
    return;
  }
  patch.edit.undo_step_current++;
  curve_patch_undo_restore(C, ob, patch);
}

}  // namespace blender::ed::sculpt_paint
