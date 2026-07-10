/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Operators for texture assignment to brushes via drag and drop.
 */

#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"
#include "DNA_userdef_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_texture.h"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_intern.hh"

/* For #DROP_IMAGE_update_texture_preview_smart(). */
#include "../interface/interface_drop_image.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Assign Image to Texture Operator
 * \{ */

/**
 * Resolve the image referenced by the drop, either by an explicit filepath or by looking up an
 * existing data-block. `r_existed` reports whether the image already existed in the file (an
 * existing data-block, or a filepath match) rather than being freshly loaded here; this governs the
 * move-vs-copy decision when pulling the image into a linked texture's library. Errors are reported
 * only when `reports` is non-null (the invoke pre-check passes null and lets #exec report).
 */
static Image *assign_image_get(Main *bmain, wmOperator *op, bool *r_existed, ReportList *reports)
{
  *r_existed = false;

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  if (filepath[0] != '\0') {
    Image *image = BKE_image_load_exists(bmain, filepath, r_existed);
    if (!image && reports) {
      BKE_reportf(reports, RPT_ERROR, "Cannot load image: %s", filepath);
    }
    return image;
  }

  Image *image = reinterpret_cast<Image *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_IM));
  if (!image) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "No image specified for assignment");
    }
    return nullptr;
  }
  /* A drag of an existing image data-block always references pre-existing data. */
  *r_existed = true;
  return image;
}

/** Resolve the active brush and the targeted texture slot (primary or mask). */
static bool assign_image_target_get(bContext *C,
                                    wmOperator *op,
                                    Brush **r_brush,
                                    bool *r_use_mask_slot,
                                    ReportList *reports)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (!paint) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "No active paint mode");
    }
    return false;
  }

  Brush *brush = BKE_paint_brush(paint);
  if (!brush) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "No active brush");
    }
    return false;
  }

  bool use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  /* Sculpt brushes only have the primary texture slot. */
  if (CTX_data_mode_enum(C) == CTX_MODE_SCULPT) {
    use_mask_slot = false;
  }

  *r_brush = brush;
  *r_use_mask_slot = use_mask_slot;
  return true;
}

/**
 * True when both images refer to the same on-disk source file. Used to detect a re-drop of the
 * image already shown in the slot, so its pixels can simply be reloaded from disk instead of
 * repointing or duplicating any data-block.
 */
static bool image_same_source(const Image *a, const Image *b)
{
  if (a == b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  if (!BKE_image_has_filepath(a) || !BKE_image_has_filepath(b)) {
    return false;
  }
  return BLI_path_cmp(a->filepath, b->filepath) == 0;
}

/** Copy the drop's image/target properties onto a menu item's operator pointer. */
static void assign_image_props_copy(PointerRNA *dst, wmOperator *op)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  RNA_string_set(dst, "filepath", filepath);
  RNA_boolean_set(dst, "use_mask_slot", RNA_boolean_get(op->ptr, "use_mask_slot"));
  if (RNA_struct_property_is_set(op->ptr, "session_uid")) {
    RNA_int_set(dst, "session_uid", RNA_int_get(op->ptr, "session_uid"));
  }
}

/** Shared tail: refresh evaluation, notify, update the preview icon and push undo. */
static wmOperatorStatus assign_image_finish(
    bContext *C, Main *bmain, Brush *brush, Tex *tex, const bool created)
{
  tex->type = TEX_IMAGE;
  DEG_id_tag_update(&tex->id, ID_RECALC_SHADING);

  WM_event_add_notifier(C, NC_TEXTURE | (created ? NA_ADDED : NA_EDITED), tex);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);

  /* Refresh the texture preview/icon for immediate visual feedback. */
  DROP_IMAGE_update_texture_preview_smart(C, bmain, tex, true);

  /* Linked data cannot participate in global (memfile) undo. */
  if (!ID_IS_LINKED(&brush->id) && !ID_IS_LINKED(&tex->id)) {
    ED_undo_push(C, "Assign Image to Brush");
  }

  return OPERATOR_FINISHED;
}

/**
 * Operator to assign an image to a brush texture slot. Used for drag and drop.
 *
 * On an already-occupied slot the existing texture is reused rather than accumulating copies:
 * re-dropping the same on-disk file merely reloads it, while a different image repoints the slot.
 * A packed dropped image is ambiguous (its data has no external file) and is routed through the
 * invoke popup, which offers updating the existing texture or creating a new one via `create_new`.
 */
static wmOperatorStatus paint_assign_image_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  bool image_existed = false;
  Image *image = assign_image_get(bmain, op, &image_existed, op->reports);
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Brush *brush = nullptr;
  bool use_mask_slot = false;
  if (!assign_image_target_get(C, op, &brush, &use_mask_slot, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  MTex &mtex = use_mask_slot ? brush->mask_mtex : brush->mtex;

  const bool create_new = RNA_boolean_get(op->ptr, "create_new");
  Tex *tex = mtex.tex;

  /* In-place reload: dropping the same on-disk (unpacked) file that the slot already shows just
   * refreshes its pixels, without creating or repointing any data-block. */
  if (tex && tex->ima && !create_new && !BKE_image_has_packedfile(image) &&
      image_same_source(tex->ima, image))
  {
    BKE_image_signal(bmain, tex->ima, nullptr, IMA_SIGNAL_RELOAD);
    return assign_image_finish(C, bmain, brush, tex, false);
  }

  /* Create a wrapping texture when the slot is empty, or when the user explicitly chose to create a
   * new one for a packed image (see #paint_assign_image_invoke). This mirrors #TEXTURE_OT_new so
   * reference counting, library membership and undo stay consistent - a brush may be a linked
   * asset, and linked data must not reference a local texture. */
  const bool created = (tex == nullptr) || create_new;
  if (created) {
    tex = BKE_texture_add(bmain,
                          use_mask_slot ? DATA_("Brush Mask Texture") : DATA_("Brush Texture"));
    tex->type = TEX_IMAGE;
    BKE_id_move_to_same_lib(*bmain, tex->id, brush->id);

    /* #BKE_texture_add already left one user; the RNA assignment below adds one, so compensate. */
    id_us_min(&tex->id);
    PointerRNA brush_ptr = RNA_id_pointer_create(&brush->id);
    PropertyRNA *slot_prop = RNA_struct_find_property(&brush_ptr,
                                                      use_mask_slot ? "mask_texture" : "texture");
    const PointerRNA tex_ptr = RNA_id_pointer_create(&tex->id);
    RNA_property_pointer_set(&brush_ptr, slot_prop, tex_ptr, nullptr);
    RNA_property_update(C, &brush_ptr, slot_prop);
  }

  /* Resolve the image that will actually be stored in the texture slot.
   *
   * A linked (asset) texture must never reference a local image: besides being an illegal
   * linked -> local reference, moving a *pre-existing* local image into the texture's (never-undo)
   * library would leave its session_uid present both locally (referenced by earlier undo steps) and
   * linked, and crash on the next undo (#BKE_main_idmap_insert_id hits a duplicate session_uid).
   *
   * So, only when the texture is linked and the image is local:
   *   - a freshly loaded image (absent from every earlier undo step) is simply moved into the
   *     library, exactly like the newly created texture above;
   *   - a pre-existing local image is copied into the library instead, leaving the original local
   *     image (and its session_uid) untouched.
   * For a fully local texture nothing is moved and the image is assigned as-is. */
  Image *slot_image = image;
  if (ID_IS_LINKED(&tex->id) && !ID_IS_LINKED(&image->id)) {
    if (image_existed) {
      slot_image = id_cast<Image *>(BKE_id_copy(bmain, &image->id));
      /* #BKE_id_copy leaves one user; the slot assignment below adds one, so compensate. */
      id_us_min(&slot_image->id);
    }
    BKE_id_move_to_same_lib(*bmain, slot_image->id, tex->id);
  }

  if (tex->ima != slot_image) {
    if (tex->ima) {
      id_us_min(&tex->ima->id);
    }
    tex->ima = slot_image;
    id_us_plus(&slot_image->id);
  }

  return assign_image_finish(C, bmain, brush, tex, created);
}

static wmOperatorStatus paint_assign_image_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  /* The choice between updating the existing texture and creating a new one is only offered for a
   * packed dropped image on an already-occupied slot: packed data has no external file, so silently
   * overwriting it could discard pixels that exist nowhere else. Everything else runs directly.
   *
   * Only an image dragged as an existing data-block (by `session_uid`) can be packed; an image
   * loaded from a dropped file path never is. Skip the pre-check for file drops so #exec resolves
   * the image exactly once - resolving it here as well would load it early and make #exec see it as
   * pre-existing, needlessly copying (instead of moving) it into a linked texture's library. */
  if (!RNA_boolean_get(op->ptr, "create_new") &&
      RNA_struct_property_is_set(op->ptr, "session_uid"))
  {
    Main *bmain = CTX_data_main(C);
    bool image_existed = false;
    Image *image = assign_image_get(bmain, op, &image_existed, nullptr);
    Brush *brush = nullptr;
    bool use_mask_slot = false;
    if (image && BKE_image_has_packedfile(image) &&
        assign_image_target_get(C, op, &brush, &use_mask_slot, nullptr))
    {
      const MTex &mtex = use_mask_slot ? brush->mask_mtex : brush->mtex;
      if (mtex.tex) {
        ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Assign Packed Image"), ICON_NONE);
        ui::Layout &layout = *popup_menu_layout(pup);

        PointerRNA update_ptr = layout.op(op->type,
                                          IFACE_("Update Existing Texture"),
                                          ICON_NONE,
                                          wm::OpCallContext::ExecDefault,
                                          UI_ITEM_NONE);
        assign_image_props_copy(&update_ptr, op);
        RNA_boolean_set(&update_ptr, "create_new", false);

        PointerRNA new_ptr = layout.op(op->type,
                                       IFACE_("Create New Texture"),
                                       ICON_NONE,
                                       wm::OpCallContext::ExecDefault,
                                       UI_ITEM_NONE);
        assign_image_props_copy(&new_ptr, op);
        RNA_boolean_set(&new_ptr, "create_new", true);

        popup_menu_end(C, pup);
        return OPERATOR_INTERFACE;
      }
    }
  }

  return op->type->exec(C, op);
}

static bool paint_assign_image_poll(bContext *C)
{
  /* Check if we have an active paint mode and brush */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (!paint) {
    return false;
  }

  Brush *brush = BKE_paint_brush(paint);
  return (brush != nullptr);
}

void BRUSH_OT_texture_slot_assign_image(wmOperatorType *ot)
{
  /* Identifiers */
  ot->name = "Assign Image to Brush Texture";
  ot->idname = "BRUSH_OT_texture_slot_assign_image";
  ot->description = "Assign image to active brush texture slot";

  /* API callbacks */
  ot->invoke = paint_assign_image_invoke;
  ot->exec = paint_assign_image_exec;
  ot->poll = paint_assign_image_poll;

  /* Flags. NOTE: no #OPTYPE_UNDO - undo is pushed manually (and skipped for linked brushes),
   * the same way #TEXTURE_OT_new does. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* Properties */
  RNA_def_string_file_path(
      ot->srna, "filepath", nullptr, FILE_MAX, "File Path", "Path to image file");
  RNA_def_boolean(ot->srna,
                  "use_mask_slot",
                  false,
                  "Use Mask Slot",
                  "Assign to mask texture slot instead of main texture slot");
  RNA_def_boolean(ot->srna,
                  "replace_existing",
                  false,
                  "Replace Existing",
                  "Replace existing texture in slot instead of creating new one");
  RNA_def_boolean(ot->srna,
                  "skip_reference_count",
                  false,
                  "Skip Reference Count",
                  "Skip reference count changes (for same image detection)");

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna,
      "create_new",
      false,
      "Create New Texture",
      "Create a new texture for the dropped image instead of updating the existing one");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  /* ID lookup properties for drag and drop */
  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

}  // namespace blender
