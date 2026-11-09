/* SPDX-FileCopyrightText: 2026 Modifications by Nazir
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Private interface between `sculpt_undo.cc`, which owns #StepData, and `sculpt_undo_history.cc`,
 * which drives the per-object sculpt history from the undo pushes. #StepData stays private to its
 * file (it pulls in half of the sculpt undo storage), so the history code reads and writes only
 * the fields it owns through these accessors.
 *
 * # Contract: Undo Stack and Sculpt History Synchronization
 *
 * The sculpt history (#SculptHistory) is a per-object timeline of position/mask changes, separate
 * from but synchronized with the global undo stack. They track the same edits but serve different
 * purposes:
 * - **Undo stack**: restores the full sculpt state (positions, masks, face sets, layers, session)
 * - **Sculpt history**: scrubbing timeline for positions and masks only, with efficient deltas
 *
 * ## Ownership and Authority
 *
 * - The **undo stack is authoritative** for what edits exist and their order.
 * - The **history follows** the undo stack: every history step maps to an undo step.
 * - When undo/redo moves, the history cursor moves with it (#UndoTypeHistoryHooks).
 * - When a history step is deleted (#truncate_after_current), matching undo steps are removed
 *   (#history_steps_deleted).
 *
 * ## Synchronization Points
 *
 * 1. **Recording**: #history_push_end_object is called after an edit, before the undo step is
 *    pushed. It records the history step, and if editing from the middle, truncates later history
 *    steps and matching undo steps.
 *
 * 2. **Undo/Redo**: The undo system calls the #UndoTypeHistoryHooks (#history_hooks) registered
 *    in #register_type:
 *    - `step_poll`: checks if undo/redo should apply a history step instead
 *    - `step`: moves the history cursor before/after based on undo direction
 *    - `sync`: syncs the history cursor when undo jumped without history steps
 *
 * 3. **History Jump**: When the user clicks a history step (#SCULPT_OT_sculpt_history_goto), a
 *    special undo step (#Type::HistoryJump) is pushed to make the move undoable. Recording is
 *    paused (#ScopedPause) so the jump itself doesn't create a new history step.
 *
 * ## Invariants
 *
 * - `history.current` always points to a valid step index or -1 (empty history)
 * - #is_at_current is true when the mesh matches `history.steps[history.current]` bit-exactly
 * - After undo/redo, #is_at_current becomes true once #history_step runs
 * - Each undo step stores `cursor_before` and `cursor_after` (#StepHistoryData)
 * - Deleting future history steps also removes corresponding undo steps from the stack
 *
 * ## Edge Cases
 *
 * - **Whole-scene undo step**: can reload the object without moving the undo stack's sculpt steps.
 *   #is_at_current becomes false; the next history operation calls #apply_step to sync.
 * - **History disabled**: undo still works; history hooks do nothing when `ob.sculpt_history` is
 *   null or recording is off.
 * - **Multi-object**: each object has its own history; undo steps store per-object cursors.
 */

#include <string>

#include "BLI_vector.hh"

namespace blender {
struct Object;
struct UndoStep;
struct UndoTypeHistoryHooks;

namespace ed::sculpt_paint::undo {

/** What a sculpt undo step stores for the history of one object (see #StepData::history). */
struct StepHistoryData {
  /**
   * The object's history cursor (#bke::sculpt_history::cursor_get) before and after the step, so
   * undo and redo move the history timeline along (0: none).
   */
  int cursor_before = 0;
  int cursor_after = 0;
  /** What the history step is named after (the undo push's name, see #history_begin_ex). */
  std::string name;
  /**
   * The points the history may compare for this step (the touched ones, sorted and unique,
   * gathered before the position storage consumes the nodes). Empty: unknown, every point is
   * compared. Only needed until #history_push_end_object, which releases it.
   */
  Vector<int> touched_points;
  /** The step is a jump on the history timeline (#Type::HistoryJump), holding no mesh data. */
  bool jump = false;
};

/**
 * The history data of the undo step being built for \a ob (null when none is being built).
 */
StepHistoryData *step_history_data(const Object &ob);
/**
 * The history data the committed \a us stored for \a ob, or null when the step carries no data of
 * \a ob (matched by stored ID name, like the restore resolves them).
 */
StepHistoryData *step_history_data(UndoStep &us, const Object &ob);
const StepHistoryData *step_history_data(const UndoStep &us, const Object &ob);
/** Mark the step being built as a jump on the history timeline (#Type::HistoryJump). */
void step_history_jump_mark(const Object &ob);

/**
 * The #UndoType history hooks (see #UndoTypeHistoryHooks), assigned by #register_type and
 * dispatched by `ed_undo.cc` over every type that defines them.
 */
extern const UndoTypeHistoryHooks history_hooks;

}  // namespace ed::sculpt_paint::undo
}  // namespace blender
