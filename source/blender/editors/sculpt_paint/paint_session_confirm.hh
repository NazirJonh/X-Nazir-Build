/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared "Apply / Discard / Continue Editing" dialog for interrupted live sessions (a Paint Shape
 * session, a Curve Patch edit session). An interruption a session cannot survive on its own (a
 * sculpt-mode exit, a workspace change, an active-object change) is refused and handed to the
 * dialog instead of deciding for the user: Apply / Discard resolve the session's work and
 * re-issue the interrupted change through the resume helpers, Continue keeps editing and drops
 * it. Esc or clicking away answers Continue -- the only non-destructive default.
 *
 * The dialog is raised from the session's own internal action operator (its `invoke`): the
 * per-session data travels in that prompt operator's RNA properties, and the answer callback
 * copies them into a second call of the same operator's `exec`, which performs the commit or the
 * restore. Going through a real operator keeps #OPTYPE_UNDO's transaction on a real operator.
 */

#pragma once

#include "DNA_windowmanager_enums.h"

namespace blender {
struct bContext;
/* DNA window-manager types (including #wmOperator) live in `namespace blender`. */
struct wmOperator;
}  // namespace blender

namespace blender::ed::sculpt_paint {

/** The three answers of the interrupted-session dialog (the RNA "action" enum of every session's
 * confirm operator uses these values in this order). */
enum PaintSessionConfirmAction {
  PAINT_SESSION_CONFIRM_APPLY = 0,
  PAINT_SESSION_CONFIRM_DISCARD = 1,
  PAINT_SESSION_CONFIRM_CONTINUE = 2,
};

/** Runs one \a action through the session's action operator, carrying the per-session fields
 * (session uids, deferred-work flags) from \a prompt_op's RNA properties. The dialog keeps the
 * prompt operator alive until it closes, so the callback may read from it. */
using PaintSessionConfirmAnswerFn = void (*)(bContext &C, int action, wmOperator &prompt_op);

/** Open the shared dialog for \a prompt_op's session. \a title and \a message are static strings
 * (marked with #N_ at the call site, translated by the dialog itself). Returns #OPERATOR_RUNNING_MODAL. */
wmOperatorStatus paint_session_confirm_popup_open(bContext *C,
                                                  wmOperator &prompt_op,
                                                  const char *title,
                                                  const char *message,
                                                  PaintSessionConfirmAnswerFn answer);

/** Re-issue the sculpt-mode exit that deferred itself to the dialog (see `sculpt_mode_toggle_exec`
 * in `mesh/sculpt_ops.cc`). No-op when \a resume_mode_toggle is false. */
void paint_session_confirm_resume_mode_toggle(bContext *C, bool resume_mode_toggle);

/** Re-issue the workspace change the session refused. Posted as a `NC_SCREEN | ND_WORKSPACE_SET`
 * notifier rather than run inline: this runs from the dialog's popup handler, and a workspace
 * change swaps the screen out from under it (same deferral as `rna_Window_workspace_update`). */
void paint_session_confirm_resume_workspace_change(bContext *C, int workspace_session_uid);

/** Re-issue the active-object change the session refused: select the target, then activate it
 * through #object::base_activate_with_mode_exit_if_needed, the full user activation. */
void paint_session_confirm_resume_object_change(bContext *C, int object_session_uid);

}  // namespace blender::ed::sculpt_paint
