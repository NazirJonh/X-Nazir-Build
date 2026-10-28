/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Paint slot assignments: which slot a button draws, the session clipboard that carries an
 * assignment from one slot to another ("Copy Image" / "Copy Material" in a slot's context menu)
 * and the context-menu entries themselves.
 */

#pragma once

#include <optional>

#include "BLI_string_ref.hh"

#include "RNA_types.hh"

namespace blender {

struct bContext;
struct ID;
struct Material;

namespace ui {
struct Button;
struct Layout;
}  // namespace ui

/* -------------------------------------------------------------------- */
/** \name Paint Slot Targets (interface_paint_slot_clipboard.cc)
 * \{ */

/** Which owner a material slot belongs to: it decides how an assignment reaches the slot. */
enum class PaintSlotOwner {
  /**
   * A #BrushMaterialPaint property: assigned through the brush's own operator, so the brush is
   * made local and its preset snapshot follows the assignment.
   */
  Brush,
  /** A #PaintShapeTexture property: a plain write to the part's own source. */
  Shape,
};

/** The paint slot a button draws: the RNA property it assigns and the owner of that property. */
struct PaintSlotTarget {
  PointerRNA ptr = {};
  PropertyRNA *prop = nullptr;
  /** The ID type the property takes: #ID_IM or #ID_MA. */
  short idcode = 0;
  /** The owner of the slot's property, set for #ID_MA only: an image slot has no such split. */
  std::optional<PaintSlotOwner> owner;
};

/**
 * The owner of the material slot \a ptr/\a prop_name names, or nothing when it is not one of the
 * paint slots' own: a brush's PBR paint source, or a Shape part's own source.
 */
std::optional<PaintSlotOwner> paint_slot_material_owner(const PointerRNA &ptr,
                                                        StringRefNull prop_name);

/** True when \a target is a Shape part's Material-layer owner: its source has to be layered. */
bool paint_slot_is_layer_owner(const PaintSlotTarget &target);

/**
 * True when \a material may be assigned to \a target, and false with \a r_hint naming the reason
 * otherwise. The one answer both the drag targets and the clipboard paste give, so a paste cannot
 * land a material a drop would refuse.
 */
bool paint_slot_material_acceptable(const PaintSlotTarget &target,
                                    const Material &material,
                                    const char **r_hint);

/**
 * The paint slot \a but draws, when it draws one: a specialized image/material browser's control,
 * or an assigned slot's own button. False for every other button, including a brush texture slot
 * (that one carries a texture, not a paint slot assignment).
 */
bool paint_slot_button_target_get(const ui::Button &but, PaintSlotTarget *r_target);

/** True when \a id can be edited in place (a local ID, or an asset-editable linked one). */
bool paint_slot_owner_editable(const ID *id);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Assigning a Slot (interface_paint_slot_clipboard.cc)
 * \{ */

/**
 * Assign \a id to \a owner_prop on \a owner_ptr and push the matching undo step. All ID-slot
 * targets need exactly this, so they share the undo bookkeeping instead of repeating it.
 * Returns false when the property is missing (its name is looked up by string somewhere upstream,
 * so stay runtime safe in release builds too).
 */
bool paint_slot_assign_id_to_prop(bContext &C,
                                  PointerRNA &owner_ptr,
                                  PropertyRNA *owner_prop,
                                  ID &id,
                                  const char *undo_name);

/**
 * Assign \a ma as the active brush's PBR paint source. Goes through
 * #PAINT_OT_material_paint_source_material_set rather than writing the property: the brush is
 * made local and its preset snapshot follows the assignment.
 */
bool paint_slot_assign_material_to_brush(bContext &C, const Material &ma);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Slot Context Menu (interface_paint_slot_clipboard.cc)
 * \{ */

/**
 * The "Copy Image"/"Paste Image" (and their Material twins) entries of a paint slot's button
 * context menu: the assigned data-block travels to another slot of the same kind through the
 * shared paint-slot clipboard. Adds the entries when \a but is a paint-slot button and returns
 * true then; leaves the menu untouched otherwise.
 */
bool paint_slot_context_menu_add(bContext *C, const ui::Button &but, ui::Layout &layout);

/** \} */

}  // namespace blender
