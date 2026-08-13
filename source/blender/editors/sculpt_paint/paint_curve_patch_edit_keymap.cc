/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Modal keymap for #SCULPT_OT_curve_patch_edit. Bindings live in `blender_default.py`; this
 * file only ensures the enum and assigns the map to the operator. See the header comment of
 * `paint_curve_patch_edit.cc` for why this is a `WM_modalkeymap` rather than a tool keymap of
 * small operators.
 */

#include "RNA_types.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "paint_curve_patch_edit_intern.hh"

namespace blender::ed::sculpt_paint {

wmKeyMap *curve_patch_edit_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {CURVE_PATCH_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", "Commit the patch"},
      {CURVE_PATCH_MODAL_CANCEL, "CANCEL", 0, "Cancel", "Discard the patch"},
      {CURVE_PATCH_MODAL_UNDO, "UNDO", 0, "Undo", "Undo the last in-session edit"},
      {CURVE_PATCH_MODAL_REDO, "REDO", 0, "Redo", "Redo the last in-session edit"},
      {CURVE_PATCH_MODAL_TOGGLE_CYCLIC, "TOGGLE_CYCLIC", 0, "Toggle Cyclic", "Close or open the curve"},
      {CURVE_PATCH_MODAL_SWAP_AXIS, "SWAP_AXIS", 0, "Swap Texture Axis", "Swap the texture U/V axes"},
      {CURVE_PATCH_MODAL_TRANSLATE, "TRANSLATE", 0, "Move", "Move the active point"},
      {CURVE_PATCH_MODAL_ROTATE, "ROTATE", 0, "Rotate", "Rotate the active point"},
      {CURVE_PATCH_MODAL_SCALE, "SCALE", 0, "Scale", "Scale the active point"},
      {CURVE_PATCH_MODAL_RADIUS, "RADIUS", 0, "Radius", "Drag the active point's radius"},
      {CURVE_PATCH_MODAL_DELETE, "DELETE", 0, "Delete Point", "Delete the active point"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Curve Patch Edit Modal Map";
  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);
  if (keymap && keymap->modal_items) {
    WM_modalkeymap_assign(keymap, "SCULPT_OT_curve_patch_edit");
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  /* Bindings live in `blender_default.py` so key-config merge cannot empty the map (the trap a
   * C-only tool keymap hit) and so the user can rebind them. This function only attaches the enum
   * and assigns it to the operator. Industry Compatible (and any preset that does not list this
   * map) therefore has an empty item list; the modal still swallows Ctrl/Cmd+Z in the raw
   * `EVT_ZKEY` case so global undo cannot leak. */
  WM_modalkeymap_assign(keymap, "SCULPT_OT_curve_patch_edit");
  return keymap;
}

}  // namespace blender::ed::sculpt_paint
