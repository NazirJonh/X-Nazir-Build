/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Paint slot assignments: which slot a button draws, the session clipboard that carries an
 * assignment from one slot to another ("Copy Image" / "Copy Material" in a slot's context menu)
 * and the operators behind those menu entries.
 */

#include <optional>
#include <string>

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_assert.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"
#include "BKE_wm_runtime.hh"

#include "ED_paint_slot_clipboard.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"
#include "interface_paint_slot_clipboard.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Paint Slot Targets
 * \{ */

bool paint_slot_owner_editable(const ID *id)
{
  /* The same test the drag targets apply: linked data can only be edited where its library allows
   * it, and an override's assigned pointer is not the override's to rewrite. */
  return id != nullptr && ID_IS_EDITABLE(id) && !ID_IS_OVERRIDE_LIBRARY(id);
}

std::optional<PaintSlotOwner> paint_slot_material_owner(const PointerRNA &ptr,
                                                        const StringRefNull prop_name)
{
  if (ptr.type == nullptr) {
    return std::nullopt;
  }
  if (RNA_struct_is_a(ptr.type, RNA_BrushMaterialPaint)) {
    /* #BrushMaterialPaint.source_layer_owner is the Layer kind's owner and is picked in the panel:
     * an assignment only ever targets the brush's Default source. */
    if (prop_name != "source_material") {
      return std::nullopt;
    }
    return PaintSlotOwner::Brush;
  }
  if (RNA_struct_is_a(ptr.type, RNA_PaintShapeTexture)) {
    /* A Shape part keeps its own source, independent of the brush: a plain material, a layered
     * one, or one of its rows. */
    if (prop_name != "material" && prop_name != "layer_owner") {
      return std::nullopt;
    }
    return PaintSlotOwner::Shape;
  }
  return std::nullopt;
}

bool paint_slot_is_layer_owner(const PaintSlotTarget &target)
{
  return target.owner == PaintSlotOwner::Shape && target.prop != nullptr &&
         STREQ(RNA_property_identifier(target.prop), "layer_owner");
}

bool paint_slot_material_acceptable(const PaintSlotTarget &target,
                                    const Material &material,
                                    const char **r_hint)
{
  /* A Shape Layer owner must be a layered material: a plain material has no stack or row to
   * bake. */
  if (paint_slot_is_layer_owner(target) && !BKE_paint_layers_is_layered(material)) {
    if (r_hint != nullptr) {
      *r_hint = TIP_("Layer Material must be a layered material");
    }
    return false;
  }
  return true;
}

/** The ID type \a ptr/\a prop assigns, when it is one a paint slot clipboard can carry. */
static short paint_slot_target_idcode(PointerRNA &ptr, PropertyRNA *prop)
{
  if (prop == nullptr || RNA_property_type(prop) != PROP_POINTER) {
    return 0;
  }
  const StructRNA *prop_type = RNA_property_pointer_type(&ptr, prop);
  const short idcode = prop_type ? short(RNA_type_to_ID_code(prop_type)) : 0;
  return ELEM(idcode, ID_IM, ID_MA) ? idcode : 0;
}

/** The paint slot \a ptr/\a prop_name names, or false when it is not one the clipboard carries. */
static bool paint_slot_target_init(PointerRNA ptr,
                                   const StringRefNull prop_name,
                                   PaintSlotTarget *r_target)
{
  PropertyRNA *prop = RNA_struct_find_property(&ptr, prop_name.c_str());
  const short idcode = paint_slot_target_idcode(ptr, prop);
  if (idcode == 0) {
    return false;
  }
  /* A material slot's owner decides how an assignment has to be applied (see #PaintSlotOwner);
   * an image slot has no such split. */
  std::optional<PaintSlotOwner> owner;
  if (idcode == ID_MA) {
    owner = paint_slot_material_owner(ptr, prop_name);
    if (!owner) {
      return false;
    }
  }
  r_target->ptr = ptr;
  r_target->prop = prop;
  r_target->idcode = idcode;
  r_target->owner = owner;
  return true;
}

/**
 * The context keys a slot button publishes its property under, tried in this order: `id_browser_*`
 * for a specialized browser's own controls, `template_id_*` for an assigned slot's name button (the
 * standard template-ID context). The button's own store and the operator's context are read with
 * the same list, so a key added for one is answered by both.
 */
static constexpr const char *paint_slot_context_keys[][2] = {
    {"id_browser_ptr", "id_browser_prop"},
    {"template_id_ptr", "template_id_prop"},
};

/**
 * The RNA property \a but publishes as its slot, under either context key pair the widgets use.
 */
static bool paint_slot_button_target_prop_get(const ui::Button &but,
                                              PointerRNA *r_ptr,
                                              std::string *r_prop_name)
{
  if (but.context == nullptr) {
    return false;
  }
  for (const auto &keys : paint_slot_context_keys) {
    const PointerRNA *slot_ptr = CTX_store_ptr_lookup(but.context, keys[0]);
    std::optional<StringRefNull> prop_name = CTX_store_string_lookup(but.context, keys[1]);
    if (slot_ptr == nullptr || slot_ptr->data == nullptr || slot_ptr->type == nullptr ||
        !prop_name)
    {
      continue;
    }
    *r_ptr = *slot_ptr;
    *r_prop_name = prop_name->c_str();
    return true;
  }
  return false;
}

bool paint_slot_button_target_get(const ui::Button &but, PaintSlotTarget *r_target)
{
  PointerRNA ptr = {};
  std::string prop_name;
  if (!paint_slot_button_target_prop_get(but, &ptr, &prop_name)) {
    return false;
  }
  return paint_slot_target_init(ptr, prop_name, r_target);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Assigning a Slot
 * \{ */

bool paint_slot_assign_id_to_prop(bContext &C,
                                  PointerRNA &owner_ptr,
                                  PropertyRNA *owner_prop,
                                  ID &id,
                                  const char *undo_name)
{
  BLI_assert(owner_prop != nullptr);
  if (owner_prop == nullptr) {
    return false;
  }
  RNA_property_pointer_set(&owner_ptr, owner_prop, RNA_id_pointer_create(&id), nullptr);
  RNA_property_update(&C, &owner_ptr, owner_prop);
  /* Linked data cannot participate in global (memfile) undo. */
  if (!owner_ptr.owner_id || (!ID_IS_LINKED(owner_ptr.owner_id) && !ID_IS_LINKED(&id))) {
    ED_undo_memfile_push(&C, undo_name);
  }
  return true;
}

bool paint_slot_assign_material_to_brush(bContext &C, const Material &ma)
{
  /* The operator reports its own refusals (a linked material, a linked brush), so the false return
   * only means the assignment did not happen. */
  PointerRNA props = WM_operator_properties_create("PAINT_OT_material_paint_source_material_set");
  /* A session UID is a `uint32_t` carried through an `int` property: the flip above #INT32_MAX is
   * intended, the operator casts it back to `uint32_t` before the lookup. */
  RNA_int_set(&props, "session_uid", int(ma.id.session_uid));
  RNA_string_set(&props, "name", ma.id.name + 2);
  const wmOperatorStatus status = WM_operator_name_call(
      &C, "PAINT_OT_material_paint_source_material_set", wm::OpCallContext::ExecDefault, &props,
      nullptr);
  WM_operator_properties_free(&props);
  return (status & OPERATOR_FINISHED) != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Slot Clipboard
 * \{ */

/** The window manager's paint-slot clipboard, or null without one (background mode). */
static bke::PaintSlotClipboard *paint_slot_clipboard_get(const bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr || wm->runtime == nullptr) {
    return nullptr;
  }
  return &wm->runtime->paint_slot_clipboard;
}

void ED_paint_slot_clipboard_store(const bContext *C, const ID &id)
{
  if (bke::PaintSlotClipboard *clipboard = paint_slot_clipboard_get(C)) {
    clipboard->store(id);
  }
}

ID *ED_paint_slot_clipboard_resolve(const bContext *C, const short expected_idcode)
{
  const bke::PaintSlotClipboard *clipboard = paint_slot_clipboard_get(C);
  if (clipboard == nullptr || clipboard->is_empty() || clipboard->idcode != expected_idcode) {
    return nullptr;
  }
  Main *bmain = CTX_data_main(C);
  if (bmain == nullptr) {
    return nullptr;
  }
  /* The session UID alone: it belongs to the data-block and survives undo, so the exact copy is
   * found after one too. A name fallback would answer with the first same-name block of the file
   * -- a linked one, say -- which is not what the user copied. */
  return BKE_libblock_find_session_uid(bmain, expected_idcode, clipboard->uid);
}

void ED_paint_slot_clipboard_copy_report(const bContext *C, const ID &id, ReportList *reports)
{
  ED_paint_slot_clipboard_store(C, id);
  BLI_assert(ELEM(GS(id.name), ID_IM, ID_MA));
  if (GS(id.name) == ID_IM) {
    BKE_reportf(reports, RPT_INFO, TIP_("Copied image \"%s\""), id.name + 2);
  }
  else if (GS(id.name) == ID_MA) {
    BKE_reportf(reports, RPT_INFO, TIP_("Copied material \"%s\""), id.name + 2);
  }
}

/**
 * The slot the copy/paste operators act on: the one the context store names. That is the hovered
 * button's own store while a menu is built (#popup_context_menu_for_button publishes it), and the
 * menu item's inherited one when the operator runs.
 */
static bool paint_slot_target_from_context(const bContext *C, PaintSlotTarget *r_target)
{
  for (const auto &keys : paint_slot_context_keys) {
    PointerRNA ptr = CTX_data_pointer_get(C, keys[0]);
    std::optional<StringRefNull> prop_name = CTX_data_string_get(C, keys[1]);
    if (ptr.data == nullptr || !prop_name) {
      continue;
    }
    return paint_slot_target_init(ptr, *prop_name, r_target);
  }
  return false;
}

/**
 * True when \a target's owner takes an assignment in place. A brush source is the exception: the
 * operator that assigns it makes the brush local itself and reports why it could not, which is also
 * what a drop onto that slot does.
 */
static bool paint_slot_target_owner_editable(const PaintSlotTarget &target)
{
  if (target.idcode == ID_MA && target.owner == PaintSlotOwner::Brush) {
    return true;
  }
  return target.ptr.owner_id == nullptr || paint_slot_owner_editable(target.ptr.owner_id);
}

/** True when \a target takes the copied assignment: an editable owner, and something to paste. */
static bool paint_slot_paste_possible(const bContext *C, const PaintSlotTarget &target)
{
  if (!paint_slot_target_owner_editable(target)) {
    return false;
  }
  ID *id = ED_paint_slot_clipboard_resolve(C, target.idcode);
  if (id == nullptr) {
    return false;
  }
  if (target.idcode == ID_MA &&
      !paint_slot_material_acceptable(target, *id_cast<const Material *>(id), nullptr))
  {
    return false;
  }
  return true;
}

static bool paint_slot_copy_poll(bContext *C)
{
  PaintSlotTarget target;
  return paint_slot_target_from_context(C, &target);
}

static wmOperatorStatus paint_slot_copy_exec(bContext *C, wmOperator *op)
{
  PaintSlotTarget target;
  if (!paint_slot_target_from_context(C, &target)) {
    return OPERATOR_CANCELLED;
  }
  const ID *id = static_cast<const ID *>(RNA_property_pointer_get(&target.ptr, target.prop).data);
  if (id == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The slot has nothing to copy");
    return OPERATOR_CANCELLED;
  }
  ED_paint_slot_clipboard_copy_report(C, *id, op->reports);
  return OPERATOR_FINISHED;
}

static bool paint_slot_paste_poll(bContext *C)
{
  PaintSlotTarget target;
  if (!paint_slot_target_from_context(C, &target)) {
    return false;
  }
  return paint_slot_paste_possible(C, target);
}

static wmOperatorStatus paint_slot_paste_exec(bContext *C, wmOperator *op)
{
  PaintSlotTarget target;
  if (!paint_slot_target_from_context(C, &target)) {
    return OPERATOR_CANCELLED;
  }
  ID *id = ED_paint_slot_clipboard_resolve(C, target.idcode);
  if (id == nullptr) {
    /* Nothing answers the copied session UID: the block was deleted since the copy. Forget it, so
     * the clipboard's own answer stays truthful. */
    if (bke::PaintSlotClipboard *clipboard = paint_slot_clipboard_get(C);
        clipboard != nullptr && clipboard->idcode == target.idcode)
    {
      clipboard->clear();
    }
    BKE_report(op->reports, RPT_WARNING, "The copied assignment is no longer available");
    return OPERATOR_CANCELLED;
  }
  if (target.idcode == ID_MA) {
    const char *hint = nullptr;
    if (!paint_slot_material_acceptable(target, *id_cast<const Material *>(id), &hint)) {
      BKE_report(op->reports, RPT_ERROR, hint);
      return OPERATOR_CANCELLED;
    }
  }
  if (!paint_slot_target_owner_editable(target)) {
    BKE_report(op->reports, RPT_ERROR, "The slot's owner is linked or overridden");
    return OPERATOR_CANCELLED;
  }

  if (target.idcode == ID_MA && target.owner == PaintSlotOwner::Brush) {
    const bool assigned = paint_slot_assign_material_to_brush(*C, *id_cast<Material *>(id));
    return assigned ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }

  /* A paint channel's record keeps a "use" switch beside its map: pasting while it is off would
   * read as a paste that did nothing. Switched on before the assignment, so the undo step the
   * assignment pushes contains it too. */
  if (target.ptr.type != nullptr &&
      (RNA_struct_is_a(target.ptr.type, RNA_BrushMaterialPaintChannel) ||
       RNA_struct_is_a(target.ptr.type, RNA_MaterialPaintLayerChannel)))
  {
    PropertyRNA *use_prop = RNA_struct_find_property(&target.ptr, "use");
    if (use_prop != nullptr && RNA_property_type(use_prop) == PROP_BOOLEAN) {
      RNA_boolean_set(&target.ptr, "use", true);
      RNA_property_update(C, &target.ptr, use_prop);
    }
  }
  if (!paint_slot_assign_id_to_prop(*C, target.ptr, target.prop, *id, "Paste Slot Assignment")) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Slot Context Menu
 * \{ */

bool paint_slot_context_menu_add(bContext *C, const ui::Button &but, ui::Layout &layout)
{
  PaintSlotTarget target;
  if (!paint_slot_button_target_get(but, &target)) {
    return false;
  }

  const char *copy_label = (target.idcode == ID_IM) ? IFACE_("Copy Image") :
                                                      IFACE_("Copy Material");
  const char *paste_label = (target.idcode == ID_IM) ? IFACE_("Paste Image") :
                                                       IFACE_("Paste Material");

  ui::Layout &copy_row = layout.column(true);
  copy_row.enabled_set(RNA_property_pointer_get(&target.ptr, target.prop).data != nullptr);
  copy_row.op("UI_OT_paint_slot_copy", copy_label, ICON_COPYDOWN);

  /* The paste entry only shows while something compatible is on the clipboard, the way the
   * driver's own does: an always-greyed paste on every slot is noise where nothing was copied.
   * Asked of the button's own target, not of the context: the menu must not depend on the caller
   * having published the button's store before this call. */
  if (paint_slot_paste_possible(C, target)) {
    layout.op("UI_OT_paint_slot_paste", paste_label, ICON_PASTEDOWN);
  }
  return true;
}

namespace ui {

void UI_OT_paint_slot_copy(wmOperatorType *ot)
{
  ot->name = "Copy Paint Slot";
  ot->idname = "UI_OT_paint_slot_copy";
  ot->description = "Copy the image or material assigned to this slot, to paste into another one";

  ot->poll = paint_slot_copy_poll;
  ot->exec = paint_slot_copy_exec;

  ot->flag = OPTYPE_INTERNAL;
}

void UI_OT_paint_slot_paste(wmOperatorType *ot)
{
  ot->name = "Paste Paint Slot";
  ot->idname = "UI_OT_paint_slot_paste";
  ot->description = "Assign the copied image or material to this slot";

  ot->poll = paint_slot_paste_poll;
  ot->exec = paint_slot_paste_exec;

  ot->flag = OPTYPE_INTERNAL;
}

}  // namespace ui

/** \} */

}  // namespace blender
