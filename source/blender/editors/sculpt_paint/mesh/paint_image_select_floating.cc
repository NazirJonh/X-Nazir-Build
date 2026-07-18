/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BLI_utildefines.h"

#include "BKE_context.hh"

#include "BLT_translation.hh"

/* #SpaceImage must be complete: #image_select_floating_sessions_end reaches through
 * `sima->runtime`. */
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "IMB_imbuf_types.hh"

#include "RNA_types.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

/* For the per-tool session-end entry points called by #image_select_floating_sessions_end. This
 * header re-includes paint_image_select_floating.hh, which is `#pragma once`. */
#include "paint_image_select_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Shared floating session
 * \{ */

bool image_select_floating_state_owns(const PaintSelectFloatingSession *state,
                                      const SpaceImage *sima)
{
  return state != nullptr && state->owner_sima == sima;
}

void image_select_floating_draw_handle_clear(PaintSelectFloatingSession &session)
{
  if (session.draw_handle && session.owner_region_type) {
    ED_region_draw_cb_exit(session.owner_region_type, session.draw_handle);
  }
  session.draw_handle = nullptr;
}

void image_select_floating_drag_end(bContext *C, PaintSelectFloatingSession *session)
{
  if (session) {
    session->is_dragging = false;
  }
  /* Restored unconditionally: some exit paths are reached after another operator already cleared
   * #is_dragging (see #PAINT_OT_image_select_move_undo_step), and those must still hand the
   * window its normal cursor back. */
  if (wmWindow *win = CTX_wm_window(C)) {
    WM_cursor_modal_restore(win);
  }
}

bool image_select_mask_matches(const ImBuf *mask, const int width, const int height)
{
  if (!mask) {
    return false;
  }
  const bool matches = (mask->x == width && mask->y == height);
  /* A mismatch means the image changed resolution behind the runtime mask's back; the mask should
   * have been rebuilt. Assert in debug builds, skip the tile in release builds. */
  BLI_assert(matches);
  return matches;
}

bool image_select_mask_matches(const ImBuf *mask, const ImBuf *ibuf)
{
  if (!ibuf) {
    return false;
  }
  return image_select_mask_matches(mask, ibuf->x, ibuf->y);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cross-tool session teardown
 * \{ */

void image_select_floating_sessions_end_all(bContext *C, SpaceImage *sima)
{
  const PaintSelectFloatingSession *session = image_select_session_active(sima);
  if (!session) {
    return;
  }
  /* One slot, so at most one tool can need ending and the old "un-paint the gradient before baking
   * the lifted fragments" ordering has nothing left to order. Each entry point clears the slot
   * itself, which is what makes ending a session twice impossible. */
  switch (session->tool) {
    case PaintSelectTool::Move:
      image_select_move_session_end_for_takeover(C, sima);
      break;
    case PaintSelectTool::Transform:
      image_select_transform_session_end_for_takeover(C, sima);
      break;
    case PaintSelectTool::Gradient:
      image_select_gradient_session_end_for_takeover(C, sima);
      break;
    case PaintSelectTool::Warp:
      image_select_warp_session_end_for_takeover(C, sima);
      break;
  }
}

void image_select_floating_sessions_end(bContext *C,
                                        SpaceImage *sima,
                                        const PaintSelectTool taking_over)
{
  const PaintSelectFloatingSession *session = image_select_session_active(sima);
  if (!session || session->tool == taking_over) {
    return;
  }
  image_select_floating_sessions_end_all(C, sima);
}

void image_select_floating_session_free(PaintSelectFloatingSession *session)
{
  if (!session) {
    return;
  }
  /* Dispatched rather than deleted through the base: the base is deliberately non-polymorphic (see
   * the file header), so only the tool that defines the concrete state can destroy it. */
  switch (session->tool) {
    case PaintSelectTool::Move:
      image_select_move_session_free(session);
      break;
    case PaintSelectTool::Transform:
      image_select_transform_session_free(session);
      break;
    case PaintSelectTool::Gradient:
      image_select_gradient_session_free(session);
      break;
    case PaintSelectTool::Warp:
      image_select_warp_session_free(session);
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared modal keymap and status bar
 * \{ */

wmKeyMap *image_select_floating_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {IMAGE_SELECT_FLOATING_MODAL_CONFIRM,
       "CONFIRM",
       0,
       "Confirm",
       "Apply the floating fragment to the canvas"},
      {IMAGE_SELECT_FLOATING_MODAL_CANCEL,
       "CANCEL",
       0,
       "Cancel",
       "Discard the edit and restore the original fragment"},
      {IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP,
       "UNDO_STEP",
       0,
       "Undo Step",
       "Step back one gesture without leaving the floating selection"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Image Paint Selection Floating Modal";

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);

  /* Called once per space-type; the map and its items only need to be built the first time. */
  if (keymap && keymap->modal_items) {
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);

  {
    KeyMapItem_Params params{};
    params.type = EVT_RETKEY;
    params.value = KM_PRESS;
    params.modifier = KM_ANY;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, IMAGE_SELECT_FLOATING_MODAL_CONFIRM);
  }
  {
    KeyMapItem_Params params{};
    params.type = EVT_PADENTER;
    params.value = KM_PRESS;
    params.modifier = KM_ANY;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, IMAGE_SELECT_FLOATING_MODAL_CONFIRM);
  }
  {
    KeyMapItem_Params params{};
    params.type = EVT_ESCKEY;
    params.value = KM_PRESS;
    params.modifier = KM_ANY;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, IMAGE_SELECT_FLOATING_MODAL_CANCEL);
  }
  {
    KeyMapItem_Params params{};
    params.type = RIGHTMOUSE;
    params.value = KM_PRESS;
    params.modifier = KM_ANY;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, IMAGE_SELECT_FLOATING_MODAL_CANCEL);
  }
  {
    /* Replaces the raw Ctrl+Z check the tools used to do, so the binding is user-configurable and
     * no longer bypasses the keymap. */
    KeyMapItem_Params params{};
    params.type = EVT_ZKEY;
    params.value = KM_PRESS;
    params.modifier = KM_CTRL;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP);
  }

  WM_modalkeymap_assign(keymap, "PAINT_OT_image_select_move");
  WM_modalkeymap_assign(keymap, "PAINT_OT_image_select_transform");
  WM_modalkeymap_assign(keymap, "PAINT_OT_image_select_transform_drag");
  WM_modalkeymap_assign(keymap, "PAINT_OT_image_select_warp");
  /* Gradient only uses CONFIRM / CANCEL; it ignores UNDO_STEP. */
  WM_modalkeymap_assign(keymap, "PAINT_OT_image_select_gradient");

  return keymap;
}

void image_select_floating_status_set(bContext *C,
                                      const wmOperatorType *ot,
                                      const bool has_undo_step)
{
  if (!ot) {
    return;
  }
  WorkspaceStatus status(C);
  status.opmodal(IFACE_("Confirm"), ot, IMAGE_SELECT_FLOATING_MODAL_CONFIRM);
  status.opmodal(IFACE_("Cancel"), ot, IMAGE_SELECT_FLOATING_MODAL_CANCEL);
  if (has_undo_step) {
    status.opmodal(IFACE_("Undo Step"), ot, IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP);
  }
}

void image_select_floating_status_clear(bContext *C)
{
  ED_workspace_status_text(C, nullptr);
}

/** \} */

}  /* namespace blender */
