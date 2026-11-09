/* SPDX-FileCopyrightText: 2026 Modifications by Nazir
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Hooks the sculpt undo pushes into the per-object sculpt history (see `BKE_sculpt_history.hh`).
 * A step of the history is what an undo step does to positions and masks, so the record points
 * sit exactly where undo steps close. The undo step data (#StepData) stays private to
 * `sculpt_undo.cc`; it is reached through `sculpt_undo_intern.hh`.
 */

#include <algorithm>

#include "BLI_vector.hh"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_sculpt_history.hh"
#include "BKE_undo_system.hh"

#include "ED_undo.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "sculpt_undo.hh"
#include "sculpt_undo_intern.hh"

namespace blender::ed::sculpt_paint::undo {

/** Dyntopo edits a BMesh, which has no stable point indices: the history can't follow those. */
static bool history_supports_object(const Object &ob)
{
  if (const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob)) {
    return pbvh->type() != bke::pbvh::Type::BMesh;
  }
  const SculptSession *ss = ob.runtime->sculpt_session;
  return ss == nullptr || ss->bm == nullptr;
}

static bool history_applies(const Object &ob)
{
  return bke::sculpt_history::is_recording(ob) && history_supports_object(ob);
}

void history_begin_ex(Object &ob, const char *name)
{
  StepHistoryData *step = step_history_data(ob);
  if (step) {
    step->name = name;
  }
  if (history_applies(ob)) {
    bke::sculpt_history::begin_edit(ob, name);
  }
  if (step) {
    step->cursor_before = bke::sculpt_history::cursor_get(ob);
  }
}

void history_push_end_object(Object &ob, const bool topology_changed, const bool record)
{
  StepHistoryData *step = step_history_data(ob);
  if (record && history_applies(ob)) {
    const SculptHistory *history = ob.sculpt_history;
    const bool had_later = history && history->current < history->steps_num - 1;
    const int before = bke::sculpt_history::cursor_get(ob);
    /* A topology change compares nothing: the whole shape becomes a keyframe. An empty span (no
     * step data, or the touched points are unknown) compares every point. */
    Span<int> touched;
    if (!topology_changed && step != nullptr) {
      touched = step->touched_points.as_span();
    }
    bke::sculpt_history::record(
        ob, step ? StringRef(step->name) : StringRef(), topology_changed, touched);
    if (had_later && bke::sculpt_history::cursor_get(ob) != before) {
      /* An edit on an older step deleted the steps after it: undo must not lead into them. */
      history_steps_deleted(ob, before);
    }
  }
  if (step) {
    /* Also after going to a history step (recording paused): undoing it goes back. */
    step->cursor_after = bke::sculpt_history::cursor_get(ob);
    /* The undo step lives on while the stack does; the points were only needed for the record. */
    step->touched_points.clear_and_shrink();
  }
}

void history_jump_begin(const Scene &scene, Object &ob, const wmOperator *op)
{
  push_begin(scene, ob, op);
  step_history_jump_mark(ob);
}

void history_jump_end(Object &ob)
{
  /* The undo step is for the change of step, not a new step of the history. */
  bke::sculpt_history::ScopedPause pause(ob);
  push_end(ob);
}

void history_steps_deleted(const Object &ob, const int current)
{
  UndoStack *ustack = ED_undo_stack_get();
  if (ustack == nullptr || ustack->step_active == nullptr) {
    return;
  }
  /* Redo leads into the deleted steps. */
  BKE_undosys_stack_clear_after(ustack, ustack->step_active);

  /* After jumps back on the timeline, undo steps lead through the deleted steps. The undo steps
   * go from one history step to the next, so a run of them that starts and ends at the same step
   * leaves the mesh in the same shape and can go. Cut out such runs around the steps leading
   * through deleted ones, latest first, until none are left. Other steps (like masking, or edits
   * of another object) stay; a step that does not touch this object ends the run. */
  struct Entry {
    UndoStep *step;
    StepHistoryData *history;
  };
  Vector<Entry> steps;
  for (UndoStep *us = ustack->step_active; us; us = us->prev) {
    StepHistoryData *history = step_history_data(*us, ob);
    if (!history) {
      break;
    }
    steps.append({us, history});
  }
  std::reverse(steps.begin(), steps.end());
  const auto before = [&](const int i) { return steps[i].history->cursor_before; };
  const auto after = [&](const int i) { return steps[i].history->cursor_after; };
  while (true) {
    int last = -1;
    for (int i = int(steps.size()) - 1; i >= 0; i--) {
      if (before(i) > current || after(i) > current) {
        last = i;
        break;
      }
    }
    if (last == -1 || after(last) > current) {
      break;
    }
    int first = -1;
    for (int i = last; i >= 0; i--) {
      if (before(i) == after(last)) {
        first = i;
        break;
      }
    }
    if (first != -1) {
      BKE_undosys_stack_remove_range(ustack, steps[first].step, steps[last].step);
      steps.remove(first, last - first + 1);
      continue;
    }
    /* No run back to the same step: the timeline went into the deleted steps and jumped out to
     * another one. A jump only holds the history steps it goes between, so it can start where the
     * way into the deleted steps did, and the steps from there go. */
    int entry = last;
    while (entry > 0 && before(entry) > current) {
      entry--;
    }
    if (entry == last || before(entry) > current || !steps[last].history->jump) {
      break;
    }
    steps[last].history->cursor_before = before(entry);
    BKE_undosys_stack_remove_range(ustack, steps[entry].step, steps[last - 1].step);
    steps.remove(entry, last - entry);
  }
}

/* -------------------------------------------------------------------- */
/** \name Undo-type history hooks
 * \{ */

/* The cheap checks come first: this runs from the undo buttons' polls on every redraw. */
static bool history_object_usable(const Object *ob)
{
  return ob != nullptr && ob->type == OB_MESH && (ob->mode & OB_MODE_SCULPT) &&
         ob->sculpt_history != nullptr && ob->sculpt_history->steps_num > 0 &&
         history_supports_object(*ob);
}

/** The history step undo (or redo) should go to directly, or -1 when the undo stack does it. */
static int history_step_target(const bContext *C, const bool redo)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->sculpt_history == nullptr) {
    return -1;
  }
  /* A step to go to must exist before anything heavier is looked at. */
  const SculptHistory &history = *ob->sculpt_history;
  const int target = history.current + (redo ? 1 : -1);
  if (target < 0 || target >= history.steps_num || !history_object_usable(ob)) {
    return -1;
  }
  /* The undo step that would run: when it is this object's and starts (for redo) or ends (for
   * undo) at the current history step, undo and the history agree and the undo stack goes on. */
  UndoStack *ustack = ED_undo_stack_get();
  if (ustack == nullptr) {
    return -1;
  }
  const UndoStep *us = ustack->step_active;
  if (redo && us) {
    us = us->next;
  }
  /* (Undo can't undo the first step of the stack.) */
  const bool undo_can_run = us && (redo || us->prev != nullptr);
  if (undo_can_run && us->type != BKE_UNDOSYS_TYPE_SCULPT) {
    /* Another kind of step (an Extract, a new object...): the undo stack undoes that. */
    return -1;
  }
  if (undo_can_run) {
    const StepHistoryData *step = step_history_data(*us, *ob);
    if (step && (redo ? step->cursor_before : step->cursor_after) ==
                    bke::sculpt_history::cursor_get(*ob))
    {
      return -1;
    }
  }
  return target;
}

static bool history_step_instead(const bContext *C, const bool redo)
{
  return history_step_target(C, redo) != -1;
}

static bool history_step(bContext *C, const bool redo)
{
  const int target = history_step_target(C, redo);
  if (target == -1) {
    return false;
  }
  Object &ob = *CTX_data_active_object(C);
  const int cursor = bke::sculpt_history::step_cursor(ob.sculpt_history->steps[target]);
  /* No stack transition happens here (like going to a step from the panel, which is an operator
   * of its own): the app handlers `undo_pre/post` exist for the reload of data-blocks, and this
   * only moves the mesh's positions and masks, which #history_apply_cursor already tags for the
   * depsgraph and redraws. Only the undo-related notifier is added, so the UI that lists the undo
   * history follows. */
  history_apply_cursor(C, ob, cursor);
  WM_event_add_notifier(C, NC_WM | ND_UNDO, nullptr);
  return true;
}

static void history_sync(bContext *C, const bool redo)
{
  /* The step the undo stack just executed: its decode already moved the history cursors of the
   * objects it stores (see #restore_list), leaving them in the step's shape. */
  const UndoStack *ustack = ED_undo_stack_get();
  const UndoStep *executed = nullptr;
  if (ustack && ustack->step_active) {
    executed = redo ? ustack->step_active : ustack->step_active->next;
  }

  /* Every object in Sculpt Mode keeps its own history: undo can leave any of them out of step,
   * not only the active one. */
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const ObjectsInModeParams params{OB_MODE_SCULPT, true, nullptr, nullptr};
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_mode_params(
      *bmain, scene, view_layer, CTX_wm_view3d(C), &params);
  for (Object *ob : objects) {
    if (!history_object_usable(ob)) {
      continue;
    }
    /* Comparing the whole shape is slow on dense meshes: skip it when the executed step is this
     * object's own sculpt step and the cursor agrees with it. Whole-scene steps (and anything
     * else that reloads the object) are still compared. */
    if (executed) {
      const StepHistoryData *step = step_history_data(*executed, *ob);
      if (step && (redo ? step->cursor_after : step->cursor_before) ==
                      bke::sculpt_history::cursor_get(*ob))
      {
        continue;
      }
    }
    if (!bke::sculpt_history::is_at_current(*ob)) {
      history_apply_cursor(C, *ob, bke::sculpt_history::cursor_get(*ob));
    }
  }
}

const UndoTypeHistoryHooks history_hooks = {history_step_instead, history_step, history_sync};

/** \} */

}  // namespace blender::ed::sculpt_paint::undo
