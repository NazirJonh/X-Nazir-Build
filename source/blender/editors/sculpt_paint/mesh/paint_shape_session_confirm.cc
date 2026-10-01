/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Paint Shape session's interrupted-session dialog (`SCULPT_OT_paint_shape_session_confirm`)
 * and the refusal hooks that raise it (`paint_shape_session_defer_workspace_change` /
 * `paint_shape_session_defer_object_change`); see `paint_shape_vector_3d.hh` for the session
 * lifecycle and `../paint_session_confirm.cc` for the shared dialog itself.
 *
 * The interruptions that cannot leave the session alive are handed to the dialog instead of
 * deciding for the user:
 * - `sculpt_mode_toggle_exec()` (`mesh/sculpt_ops.cc`), before leaving Sculpt Mode;
 * - the workspace change (`workspace_edit.cc`), before the screen swaps out;
 * - the active-object change (`object_select.cc`), before the owner stops being active.
 *
 * All three REFUSE the change they intercepted and let Apply / Discard re-issue it once the shape
 * is resolved (`resume_mode_toggle` / `workspace_session_uid` / `object_new_session_uid`);
 * Continue drops it and keeps editing. The answer has to arrive while the context the session was
 * built against is still intact -- a re-anchor needs the evaluated surface the preview rests on.
 *
 * Unlike the Curve Patch this session has no modal that could police an interruption itself, so
 * the object-change route is an explicit refusal hook on the user-activation entry points
 * (#object::base_activate_user and #object::base_activate_with_mode_exit_if_needed). The plain
 * #object::base_activate stays untouched, so object creation and the other programmatic callers
 * never trip it. A tool change is not an interruption here: it settles the session directly
 * (#paint_shape_session_settle_forced).
 */

#include <climits>

#include "DNA_ID.h"
#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_session_confirm.hh"
#include "paint_shape_vector_3d.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::shape {

/**
 * True between a refused interruption and the dialog answering it. The workspace change and the
 * object activation can reach the hook more than once per user action (every window, several
 * select operators) -- without the flag the same shape would raise one dialog per hit. Cleared by
 * whichever answer runs; the popup's cancel callback always answers (Continue).
 */
static bool g_paint_shape_confirm_pending = false;

/** The object an answer acts on. 0 means the active object; the object-change route passes the
 * real owner so the shape is committed / discarded there. Null once it has been deleted. */
static Object *paint_shape_confirm_target(bContext *C, const int object_session_uid)
{
  if (object_session_uid == 0) {
    return CTX_data_active_object(C);
  }
  return id_cast<Object *>(
      BKE_libblock_find_session_uid(CTX_data_main(C), ID_OB, uint32_t(object_session_uid)));
}

/** Continue Editing: keep the session. The interruptions are refused before they happen, so there
 * is nothing to revert -- only the deferred work to drop. */
static void paint_shape_confirm_continue(bContext * /*C*/)
{
  g_paint_shape_confirm_pending = false;
}

/** The deferred work Apply and Discard owe their caller. At most one of the three is ever armed;
 * Continue deliberately skips all of them -- it means "stay". */
static void paint_shape_confirm_resume_deferred(bContext *C,
                                                const bool resume_mode_toggle,
                                                const int workspace_session_uid,
                                                const int object_session_uid)
{
  g_paint_shape_confirm_pending = false;
  paint_session_confirm_resume_object_change(C, object_session_uid);
  paint_session_confirm_resume_mode_toggle(C, resume_mode_toggle);
  paint_session_confirm_resume_workspace_change(C, workspace_session_uid);
}

static void paint_shape_confirm_answer(bContext &C,
                                       const int action,
                                       wmOperator &prompt_op)
{
  /* Cleared up front: the operator call below can be refused by its poll (the mode may have been
   * left while the dialog was open), and the flag must never outlive the dialog. */
  g_paint_shape_confirm_pending = false;
  PointerRNA props = WM_operator_properties_create("SCULPT_OT_paint_shape_session_confirm");
  RNA_enum_set(&props, "action", action);
  RNA_int_set(&props, "object_session_uid", RNA_int_get(prompt_op.ptr, "object_session_uid"));
  RNA_int_set(&props,
              "workspace_session_uid",
              RNA_int_get(prompt_op.ptr, "workspace_session_uid"));
  RNA_int_set(&props,
              "object_new_session_uid",
              RNA_int_get(prompt_op.ptr, "object_new_session_uid"));
  RNA_boolean_set(&props,
                  "resume_mode_toggle",
                  RNA_boolean_get(prompt_op.ptr, "resume_mode_toggle"));
  WM_operator_name_call(&C,
                        "SCULPT_OT_paint_shape_session_confirm",
                        wm::OpCallContext::ExecDefault,
                        &props,
                        nullptr);
  WM_operator_properties_free(&props);
}

static wmOperatorStatus paint_shape_session_confirm_exec(bContext *C, wmOperator *op)
{
  const int action = RNA_enum_get(op->ptr, "action");
  if (action == PAINT_SESSION_CONFIRM_CONTINUE) {
    paint_shape_confirm_continue(C);
    return OPERATOR_CANCELLED;
  }

  Object *target = paint_shape_confirm_target(C, RNA_int_get(op->ptr, "object_session_uid"));
  const bool live = (target != nullptr) && (paint_shape_session_get(*target) != nullptr);
  if (!live) {
    /* Nothing left to decide, but a deferred mode / workspace / object change must not be dropped
     * with it. */
    paint_shape_confirm_resume_deferred(C,
                                        RNA_boolean_get(op->ptr, "resume_mode_toggle"),
                                        RNA_int_get(op->ptr, "workspace_session_uid"),
                                        RNA_int_get(op->ptr, "object_new_session_uid"));
    return OPERATOR_CANCELLED;
  }

  if (action == PAINT_SESSION_CONFIRM_DISCARD) {
    paint_shape_session_cancel(C, *target);
  }
  else {
    /* Apply with the settle semantics: a refused commit (the anchored surface cannot be restored)
     * reports its own warning and keeps the session alive -- the deferred change then stays
     * cancelled and can be answered again through a fresh interruption. */
    paint_shape_session_commit(C, *target);
    if (paint_shape_session_get(*target) != nullptr) {
      /* Apply refused: the session stays alive, the deferred change is dropped (not resumed), and
       * the pending flag is cleared so the next interruption raises a fresh dialog. */
      g_paint_shape_confirm_pending = false;
      return OPERATOR_CANCELLED;
    }
  }

  paint_shape_confirm_resume_deferred(C,
                                      RNA_boolean_get(op->ptr, "resume_mode_toggle"),
                                      RNA_int_get(op->ptr, "workspace_session_uid"),
                                      RNA_int_get(op->ptr, "object_new_session_uid"));
  return OPERATOR_FINISHED;
}

static wmOperatorStatus paint_shape_session_confirm_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent * /*event*/)
{
  return paint_session_confirm_popup_open(
      C,
      *op,
      N_("Apply Paint Shape?"),
      N_("The Paint Shape session is ending. Apply bakes the shape into the canvas, Discard "
         "drops it, Continue Editing returns to the shape."),
      paint_shape_confirm_answer);
}

static void paint_shape_session_confirm_cancel(bContext *C, wmOperator * /*op*/)
{
  /* A stray cancel is a non-destructive "keep editing" rather than a silent discard. */
  paint_shape_confirm_continue(C);
}

/** Register the operator in the enclosing `ed::sculpt_paint` namespace (see the thin wrapper at
 * the bottom of this file): the SCULPT_OT_* symbols live there, not in `shape`. */
static void paint_shape_ot_session_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Paint Shape";
  ot->idname = "SCULPT_OT_paint_shape_session_confirm";
  ot->description =
      "Apply, discard or continue editing a Paint Shape whose live session was interrupted";

  ot->invoke = paint_shape_session_confirm_invoke;
  ot->exec = paint_shape_session_confirm_exec;
  ot->cancel = paint_shape_session_confirm_cancel;
  /* The dialog outlives the interruption that raised it, so the session can be gone by the time
   * it is answered (object deletion, undo). `exec` re-resolves the target and re-checks its
   * liveness rather than relying on this poll, which only gates that we are in Sculpt Mode at
   * all. The operator is #OPTYPE_INTERNAL (invoked programmatically, never from a menu). No
   * #OPTYPE_UNDO: the shape commit already writes its own image-undo step. */
  ot->poll = sculpt_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  static const EnumPropertyItem action_items[] = {
      {PAINT_SESSION_CONFIRM_APPLY,
       "APPLY",
       0,
       "Apply",
       "Bake the shape into the canvas"},
      {PAINT_SESSION_CONFIRM_DISCARD, "DISCARD", 0, "Discard", "Drop the shape"},
      {PAINT_SESSION_CONFIRM_CONTINUE,
       "CONTINUE",
       0,
       "Continue",
       "Keep the session and return to editing the shape"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "action",
                                   action_items,
                                   PAINT_SESSION_CONFIRM_APPLY,
                                   "Action",
                                   "What to do with the shape");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "resume_mode_toggle",
                         false,
                         "Resume Mode Toggle",
                         "Leave sculpt mode once the shape has been applied or discarded");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_int(ot->srna,
                     "workspace_session_uid",
                     0,
                     INT_MIN,
                     INT_MAX,
                     "Workspace Session UID",
                     "Session UID of the workspace to activate once the shape has been applied or "
                     "discarded",
                     INT_MIN,
                     INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_int(ot->srna,
                     "object_new_session_uid",
                     0,
                     INT_MIN,
                     INT_MAX,
                     "New Object Session UID",
                     "Session UID of the object to activate once the shape has been applied or "
                     "discarded",
                     INT_MIN,
                     INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_int(ot->srna,
                     "object_session_uid",
                     0,
                     INT_MIN,
                     INT_MAX,
                     "Object Session UID",
                     "Session UID of the object that owns the shape; 0 means the active object",
                     INT_MIN,
                     INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender::ed::sculpt_paint::shape

/* -------------------------------------------------------------------- */
/** \name Interruption hooks
 * \{ */

namespace blender::ed::sculpt_paint::shape {

bool paint_shape_session_defer_workspace_change(bContext *C, const int workspace_session_uid)
{
  if (C == nullptr || workspace_session_uid == 0) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || paint_shape_session_get(*ob) == nullptr) {
    /* No live session, so no dialog can still be pending for it. */
    g_paint_shape_confirm_pending = false;
    return false;
  }
  if (g_paint_shape_confirm_pending) {
    return false;
  }
  PointerRNA props = WM_operator_properties_create("SCULPT_OT_paint_shape_session_confirm");
  RNA_int_set(&props, "workspace_session_uid", workspace_session_uid);
  RNA_int_set(&props, "object_session_uid", int(ob->id.session_uid));
  const wmOperatorStatus status = WM_operator_name_call(C,
                                                        "SCULPT_OT_paint_shape_session_confirm",
                                                        wm::OpCallContext::InvokeDefault,
                                                        &props,
                                                        nullptr);
  WM_operator_properties_free(&props);

  /* Only refuse the change once the dialog is actually up to carry it out; otherwise the silent
   * old behavior is still better than swallowing the user's workspace change. */
  g_paint_shape_confirm_pending = (status & OPERATOR_RUNNING_MODAL) != 0;
  return g_paint_shape_confirm_pending;
}

bool paint_shape_session_defer_object_change(bContext *C, const int object_new_session_uid)
{
  if (C == nullptr || object_new_session_uid == 0) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || paint_shape_session_get(*ob) == nullptr) {
    /* No live session, so no dialog can still be pending for it. */
    g_paint_shape_confirm_pending = false;
    return false;
  }
  if (g_paint_shape_confirm_pending) {
    return false;
  }
  if (ob->id.session_uid == uint32_t(object_new_session_uid)) {
    /* Re-activating the session's own object (multi-object mode entry, refresh) is no change. */
    return false;
  }
  PointerRNA props = WM_operator_properties_create("SCULPT_OT_paint_shape_session_confirm");
  RNA_int_set(&props, "object_session_uid", int(ob->id.session_uid));
  RNA_int_set(&props, "object_new_session_uid", object_new_session_uid);
  const wmOperatorStatus status = WM_operator_name_call(C,
                                                        "SCULPT_OT_paint_shape_session_confirm",
                                                        wm::OpCallContext::InvokeDefault,
                                                        &props,
                                                        nullptr);
  WM_operator_properties_free(&props);

  g_paint_shape_confirm_pending = (status & OPERATOR_RUNNING_MODAL) != 0;
  return g_paint_shape_confirm_pending;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape

/* The SCULPT_OT_* operator registrations live in `ed::sculpt_paint` (see `sculpt_intern.hh`),
 * while the implementation above sits in the nested `shape` namespace. */
namespace blender::ed::sculpt_paint {

void SCULPT_OT_paint_shape_session_confirm(wmOperatorType *ot)
{
  shape::paint_shape_ot_session_confirm(ot);
}

}  // namespace blender::ed::sculpt_paint
