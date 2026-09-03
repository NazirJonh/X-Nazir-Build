/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared declarations for the Curve Patch live-edit modal split:
 * `paint_curve_patch_edit.cc` (modal + picking + context-menu operators),
 * `paint_curve_patch_edit_keymap.cc`, `paint_curve_patch_edit_undo.cc`,
 * `paint_curve_patch_edit_sync.cc`.
 */

#pragma once

namespace blender {

struct ARegion;
struct bContext;
struct Object;
struct ScrArea;
struct wmKeyConfig;
struct wmKeyMap;

namespace ed::sculpt_paint {

struct CurvePatchSession;

enum {
  CURVE_PATCH_MODAL_CONFIRM = 0,
  CURVE_PATCH_MODAL_CANCEL = 1,
  CURVE_PATCH_MODAL_UNDO = 2,
  CURVE_PATCH_MODAL_REDO = 3,
  CURVE_PATCH_MODAL_TOGGLE_CYCLIC = 4,
  CURVE_PATCH_MODAL_SWAP_AXIS = 5,
  CURVE_PATCH_MODAL_TRANSLATE = 6,
  CURVE_PATCH_MODAL_ROTATE = 7,
  CURVE_PATCH_MODAL_SCALE = 8,
  CURVE_PATCH_MODAL_RADIUS = 9,
  CURVE_PATCH_MODAL_DELETE = 10,
};

/** Modal keymap for #SCULPT_OT_curve_patch_edit. Keyboard actions only; mouse stays in the
 * operator's `switch (event->type)`. Defined in `paint_curve_patch_edit_keymap.cc`. */
wmKeyMap *curve_patch_edit_modal_keymap(wmKeyConfig *keyconf);

void curve_patch_edit_status_set(bContext *C, const CurvePatchSession &patch);

void curve_patch_tag_overlay_redraw_all(bContext *C);
void curve_patch_tag_viewports_redraw_after_edit(bContext &C,
                                                 Object &ob,
                                                 const CurvePatchSession &patch);

void curve_patch_sync_view_context(bContext *C,
                                   ScrArea *area,
                                   ARegion *region,
                                   CurvePatchSession &patch);

bool curve_patch_undo_step_back(bContext &C, Object &ob, CurvePatchSession &patch);
void curve_patch_undo_step_forward(bContext &C, Object &ob, CurvePatchSession &patch);

}  // namespace ed::sculpt_paint
}  // namespace blender
