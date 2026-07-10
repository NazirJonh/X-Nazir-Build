/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Operators for texture assignment to brushes via drag and drop.
 */

#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_utildefines.hh"

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
 * Operator to assign an image to an existing brush texture.
 * Used for drag and drop functionality.
 */
static wmOperatorStatus paint_assign_image_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  /* Get the image to assign. `image_existed` tracks whether the image already existed in the file
   * (an existing datablock, or matched by filepath) rather than being freshly loaded by this
   * operator. It decides whether a local image may be moved into a linked texture's library or must
   * be copied into it - see the assignment below. */
  Image *image = nullptr;
  bool image_existed = false;

  /* Try to get image by filepath */
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  if (filepath[0] != '\0') {
    image = BKE_image_load_exists(bmain, filepath, &image_existed);
    if (!image) {
      BKE_reportf(op->reports, RPT_ERROR, "Cannot load image: %s", filepath);
      return OPERATOR_CANCELLED;
    }
  }
  else {
    /* Try to get image by ID lookup */
    image = (Image *)WM_operator_properties_id_lookup_from_name_or_session_uid(
        bmain, op->ptr, ID_IM);
    if (!image) {
      BKE_report(op->reports, RPT_ERROR, "No image specified for assignment");
      return OPERATOR_CANCELLED;
    }
    /* A drag of an existing image datablock always references pre-existing data. */
    image_existed = true;
  }

  /* Find active brush */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (!paint) {
    BKE_report(op->reports, RPT_ERROR, "No active paint mode");
    return OPERATOR_CANCELLED;
  }

  Brush *brush = BKE_paint_brush(paint);
  if (!brush) {
    BKE_report(op->reports, RPT_ERROR, "No active brush");
    return OPERATOR_CANCELLED;
  }

  bool use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  /* Sculpt brushes only have the primary texture slot. */
  if (CTX_data_mode_enum(C) == CTX_MODE_SCULPT) {
    use_mask_slot = false;
  }
  MTex &mtex = use_mask_slot ? brush->mask_mtex : brush->mtex;

  /* Nothing to do if that exact image is already assigned. */
  if (mtex.tex && mtex.tex->ima == image) {
    return OPERATOR_FINISHED;
  }

  Tex *tex = mtex.tex;
  const bool created = (tex == nullptr);
  if (created) {
    /* Create the wrapping texture and assign it to the brush slot the same way #TEXTURE_OT_new
     * does, so reference counting, library membership and undo stay consistent - a brush may be a
     * linked asset, and linked data must not reference a local texture. */
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
  ot->exec = paint_assign_image_exec;
  ot->poll = paint_assign_image_poll;

  /* Flags. NOTE: no #OPTYPE_UNDO - undo is pushed manually (and skipped for linked brushes),
   * the same way #TEXTURE_OT_new does. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;

  /* Properties */
  RNA_def_string_file_path(ot->srna, "filepath", nullptr, FILE_MAX, "File Path", "Path to image file");
  RNA_def_boolean(ot->srna, "use_mask_slot", false, "Use Mask Slot", "Assign to mask texture slot instead of main texture slot");
  RNA_def_boolean(ot->srna, "replace_existing", false, "Replace Existing", "Replace existing texture in slot instead of creating new one");
  RNA_def_boolean(ot->srna, "skip_reference_count", false, "Skip Reference Count", "Skip reference count changes (for same image detection)");
  
  /* ID lookup properties for drag and drop */
  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

}