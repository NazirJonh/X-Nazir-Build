/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared interrupted-session dialog and the resume helpers; see
 * `paint_session_confirm.hh`. Modeled on the file-close dialog: an alert box with three buttons
 * sharing one row, "Continue Editing" the default button, Esc / click-away = Continue.
 */

#include "paint_session_confirm.hh"

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "BLT_translation.hh"

#include "ED_object.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name The dialog
 * \{ */

/** Everything the shared dialog needs. The per-session data lives in \a op's RNA properties; the
 * answer callback knows which fields to carry. */
struct PaintSessionConfirmPopup {
  wmOperator *op = nullptr;
  /** Static strings, translated by the block builder. */
  const char *title = nullptr;
  const char *message = nullptr;
  PaintSessionConfirmAnswerFn answer = nullptr;
  /** Set once an answer ran, so the Esc / click-away path does not answer twice. */
  bool answered = false;
};

static void paint_session_confirm_popup_free(PaintSessionConfirmPopup *popup)
{
  if (popup->op) {
    WM_operator_free(popup->op);
  }
  MEM_delete(popup);
}

/** Called when the popup closes with OK: a button already ran the answer, so just release. */
static void paint_session_confirm_popup_ok(bContext * /*C*/, void *arg, int /*retval*/)
{
  paint_session_confirm_popup_free(static_cast<PaintSessionConfirmPopup *>(arg));
}

/** Esc / click-away means Continue Editing, the non-destructive answer. */
static void paint_session_confirm_popup_cancel(bContext *C, void *arg)
{
  PaintSessionConfirmPopup *popup = static_cast<PaintSessionConfirmPopup *>(arg);
  if (!popup->answered) {
    popup->answer(*C, PAINT_SESSION_CONFIRM_CONTINUE, *popup->op);
  }
  paint_session_confirm_popup_free(popup);
}

static void paint_session_confirm_add_button(ui::Block *block,
                                             PaintSessionConfirmPopup *popup,
                                             const char *text,
                                             const int action,
                                             const bool active_default)
{
  ui::Button *but = uiDefIconTextBut(
      block, ui::ButtonType::But, ICON_NONE, text, 0, 0, 0, UI_UNIT_Y, nullptr, "");
  button_drawflag_disable(but, ui::BUT_TEXT_LEFT);
  if (active_default) {
    button_flag_enable(but, ui::BUT_ACTIVE_DEFAULT);
  }
  button_func_set(but, [block, popup, action](bContext &C) {
    popup->answered = true;
    /* Close before acting: an answer swaps the screen or the active object out from under the
     * popup handler (the file-close dialog orders it the same way). */
    wmWindow *win = CTX_wm_window(&C);
    popup_block_close(&C, win, block);
    popup->answer(C, action, *popup->op);
    paint_session_confirm_popup_free(popup);
  });
}

static ui::Block *paint_session_confirm_block_create(bContext *C, ARegion *region, void *arg)
{
  PaintSessionConfirmPopup *popup = static_cast<PaintSessionConfirmPopup *>(arg);

  ui::Block *block = block_begin(C, region, __func__, ui::EmbossType::Emboss);
  block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);
  block_flag_enable(
      block, ui::BLOCK_KEEP_OPEN | ui::BLOCK_LOOP | ui::BLOCK_NO_WIN_CLIP | ui::BLOCK_NUMSELECT);
  if (popup->op) {
    popup_dummy_panel_set(region, block, popup->op->idname);
  }

  ui::Layout &layout = *uiItemsAlertBox(block, 34, ui::AlertIcon::Question);

  uiItemL_ex(&layout, IFACE_(popup->title), ICON_NONE, true, false);
  layout.separator(0.5f);
  layout.label(IFACE_(popup->message), ICON_NONE);
  layout.separator(2.0f);

  /* Split so the three buttons share the row, exactly like the file-close dialog. */
  ui::Layout &split = layout.split(0.0f, true);
  split.scale_y_set(1.2f);
  split.column(false);
  paint_session_confirm_add_button(
      block, popup, IFACE_("Apply"), PAINT_SESSION_CONFIRM_APPLY, false);
  split.column(false);
  paint_session_confirm_add_button(
      block, popup, IFACE_("Discard"), PAINT_SESSION_CONFIRM_DISCARD, false);
  split.column(false);
  paint_session_confirm_add_button(
      block, popup, IFACE_("Continue Editing"), PAINT_SESSION_CONFIRM_CONTINUE, true);

  block_bounds_set_centered(block, int(14 * UI_SCALE_FAC));
  return block;
}

wmOperatorStatus paint_session_confirm_popup_open(bContext *C,
                                                  wmOperator &prompt_op,
                                                  const char *title,
                                                  const char *message,
                                                  PaintSessionConfirmAnswerFn answer)
{
  auto *popup = MEM_new<PaintSessionConfirmPopup>(__func__);
  popup->op = &prompt_op;
  popup->title = title;
  popup->message = message;
  popup->answer = answer;

  popup_block_ex(C,
                 paint_session_confirm_block_create,
                 paint_session_confirm_popup_ok,
                 paint_session_confirm_popup_cancel,
                 popup,
                 &prompt_op);
  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resume helpers
 * \{ */

void paint_session_confirm_resume_mode_toggle(bContext *C, const bool resume_mode_toggle)
{
  if (!resume_mode_toggle) {
    return;
  }
  WM_operator_name_call(
      C, "SCULPT_OT_sculptmode_toggle", wm::OpCallContext::ExecDefault, nullptr, nullptr);
}

void paint_session_confirm_resume_workspace_change(bContext *C, const int workspace_session_uid)
{
  if (workspace_session_uid == 0) {
    return;
  }
  WorkSpace *workspace = id_cast<WorkSpace *>(
      BKE_libblock_find_session_uid(CTX_data_main(C), ID_WS, uint32_t(workspace_session_uid)));
  wmWindow *win = CTX_wm_window(C);
  if (workspace == nullptr || win == nullptr) {
    return;
  }
  /* Posted as a notifier rather than run through #WM_window_set_active_workspace directly: this
   * runs from the dialog's popup handler, and a workspace change swaps the screen out from under
   * whatever handler is executing. The window manager performs it from #wm_event_do_notifiers
   * instead, at the same safe point the workspace tabs themselves go through (see
   * `rna_Window_workspace_update()`, which defers for exactly this reason). */
  WM_event_add_notifier_ex(CTX_wm_manager(C), win, NC_SCREEN | ND_WORKSPACE_SET, workspace);
}

void paint_session_confirm_resume_object_change(bContext *C, const int object_session_uid)
{
  if (object_session_uid == 0) {
    return;
  }
  Object *target = id_cast<Object *>(
      BKE_libblock_find_session_uid(CTX_data_main(C), ID_OB, uint32_t(object_session_uid)));
  if (target == nullptr) {
    return;
  }
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, CTX_data_scene(C), view_layer);
  Base *base = BKE_view_layer_base_find(view_layer, target);
  if (base == nullptr) {
    return;
  }
  /* The user's activation is replayed in full -- selection, then the mode-exit /
   * sculpt-mode-flash activation every user activation goes through -- rather than the bare
   * #base_activate primitive, so the multi-object-sculpt switch lands exactly where the user
   * asked. The session is gone by now (Apply / Discard), so re-activation cannot re-trigger the
   * refusal. */
  object::base_select(base, object::BA_SELECT);
  object::base_activate_with_mode_exit_if_needed(C, base);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
