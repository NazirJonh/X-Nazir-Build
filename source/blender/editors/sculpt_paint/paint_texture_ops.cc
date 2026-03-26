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
#include "BKE_main_invariants.hh"
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

/* Include drag state structures */
#include "../interface/interface_drop_image.hh"

/* Debug macro for consistent logging - only prints when g_drop_image_debug_enabled is true */
#define PAINT_DEBUG_PRINT(fmt, ...) \
  do { \
    if (g_drop_image_debug_enabled) { \
      printf("[DEBUG] PAINT: " fmt "\n", ##__VA_ARGS__); \
    } \
  } while (0)

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

  PAINT_DEBUG_PRINT("paint_assign_image_exec: Starting execution");
  PAINT_DEBUG_PRINT("paint_assign_image_exec: replace_existing=%s, skip_reference_count=%s", 
         RNA_boolean_get(op->ptr, "replace_existing") ? "true" : "false", 
         RNA_boolean_get(op->ptr, "skip_reference_count") ? "true" : "false");

  /* INTEGRATION: Initialize drag state if not already active */
  if (!g_texture_drag_state.is_active) {
    DROP_IMAGE_drag_state_init();
    PAINT_DEBUG_PRINT("paint_assign_image_exec: Initialized drag state");
  }

  /* Get the image to assign */
  Image *image = nullptr;
  
  /* Try to get image by filepath */
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  
  if (filepath[0] != '\0') {
    PAINT_DEBUG_PRINT("paint_assign_image_exec: Loading image from filepath: %s", filepath);
    image = BKE_image_load_exists(bmain, filepath);
    if (!image) {
      BKE_reportf(op->reports, RPT_ERROR, "Cannot load image: %s", filepath);
      return OPERATOR_CANCELLED;
    }

    /* Ensure the image is properly part of Main.
     *
     * In undo decode/refcount recompute, we must not end up with ID pointers that are not owned
     * by the current Main. When loading by filepath, the image should already be in Main, but
     * this assert helps catch unexpected cases early (instead of later crashing on undo).
     */
    BLI_assert_msg(BKE_main_is_empty(bmain) || BLI_findindex(&bmain->images, image) != -1,
                   "Loaded image is not in Main (unexpected for BKE_image_load_exists)");
  }
  else {
    /* Try to get image by ID lookup */
    image = (Image *)WM_operator_properties_id_lookup_from_name_or_session_uid(
        bmain, op->ptr, ID_IM);
    if (!image) {
      BKE_report(op->reports, RPT_ERROR, "No image specified for assignment");
      return OPERATOR_CANCELLED;
    }
    PAINT_DEBUG_PRINT("paint_assign_image_exec: Using existing image: %s", image->id.name);
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

  PAINT_DEBUG_PRINT("paint_assign_image_exec: Found active brush: %s", brush->id.name);

  /* IMPROVED: Enhanced slot determination logic */
  bool use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  bool replace_existing = RNA_boolean_get(op->ptr, "replace_existing");
  /* NOTE: keep the RNA property for backwards compatibility with existing drop-box setups,
   * but do not use it to alter user counts (see note below). */
  const bool skip_reference_count = RNA_boolean_get(op->ptr, "skip_reference_count");
  
  /* ENHANCED: Check context mode for Sculpt mode special handling */
  const enum eContextObjectMode context_mode = CTX_data_mode_enum(C);
  if (context_mode == CTX_MODE_SCULPT) {
    /* In Sculpt mode, force main texture slot (mask slot doesn't exist) */
    use_mask_slot = false;
    PAINT_DEBUG_PRINT("paint_assign_image_exec: SCULPT MODE: Forcing main texture slot");
  } else if (context_mode == CTX_MODE_PAINT_TEXTURE) {
    /* In Texture Paint mode, respect the use_mask_slot parameter from dropbox */
    PAINT_DEBUG_PRINT("paint_assign_image_exec: TEXTURE PAINT MODE: Using slot from dropbox (use_mask_slot=%s)", 
           use_mask_slot ? "true" : "false");
  }
  
  Tex **tex_slot_ptr = use_mask_slot ? &brush->mask_mtex.tex : &brush->mtex.tex;
  Tex *tex_slot = *tex_slot_ptr;

  /* CRITICAL: After undo, tex_slot may be a dangling pointer (deleted from Main
   * but brush->mtex.tex still holds the old address). Must validate before access. */
  if (tex_slot && !BKE_id_is_in_global_main(&tex_slot->id)) {
    PAINT_DEBUG_PRINT("paint_assign_image_exec: tex_slot is dangling pointer, treating as null");
    tex_slot = nullptr;
    *tex_slot_ptr = nullptr;  /* Clear the dangling pointer to prevent future issues */
  }

  /* Also check if tex_slot->ima is a dangling pointer after undo */
  if (tex_slot && tex_slot->ima && !BKE_id_is_in_global_main(&tex_slot->ima->id)) {
    PAINT_DEBUG_PRINT("paint_assign_image_exec: tex_slot->ima is dangling pointer, clearing");
    tex_slot->ima = nullptr;  /* Clear the dangling pointer */
  }

  PAINT_DEBUG_PRINT("paint_assign_image_exec: Using %s slot (replace_existing=%s)",
         use_mask_slot ? "mask" : "main", replace_existing ? "true" : "false");

  /* NOTE: Do not manipulate Image user counts here.
   *
   * This operator can load (or reuse) external-file backed images via BKE_image_load_exists.
   * When executed with OPTYPE_UNDO, the undo system will decode memfile states and then
   * recompute ID reference counts from ID links.
   *
   * If we also change Image user counts manually (id_us_plus/id_us_min), we can create temporary
   * states where an Image has users while not being part of Main during undo decode, triggering
   * asserts in id_us_plus_no_lib.
   *
   * So: only set pointer relationships (Tex->ima) and let the undo system/Main refcount
   * recomputation handle user counting.
   */
  (void)skip_reference_count;

  if (tex_slot && tex_slot->ima) {
    /* Slot has existing texture with image */
    if (replace_existing) {
      PAINT_DEBUG_PRINT("paint_assign_image_exec: Replacing existing texture image");
      
      /* Release old image if different */
      bool is_same_image = (tex_slot->ima == image);
      
      /* Additional check for file-based images - compare filepaths */
      if (!is_same_image && filepath[0] != '\0' && tex_slot->ima && tex_slot->ima->filepath) {
        if (BLI_path_cmp_normalized(filepath, tex_slot->ima->filepath) == 0) {
          is_same_image = true;
          PAINT_DEBUG_PRINT("paint_assign_image_exec: Same image file already assigned (filepath match)");
        }
      }
      
      if (!is_same_image) {
        /* Assign new image to existing texture */
        tex_slot->ima = image;
        tex_slot->type = TEX_IMAGE;
        
        PAINT_DEBUG_PRINT("paint_assign_image_exec: Assigned new image '%s' to existing texture '%s'", 
               image->id.name, tex_slot->id.name);
      } else {
        PAINT_DEBUG_PRINT("paint_assign_image_exec: Same image already assigned, no reference count change needed");
        /* Same image already assigned - no need to change reference count */
      }
    } else {
      PAINT_DEBUG_PRINT("paint_assign_image_exec: Slot occupied, but replace_existing=false, updating anyway");
      
      /* Still update if explicitly requested, but only if different image */
      bool is_same_image = (tex_slot->ima == image);
      
      /* Additional check for file-based images - compare filepaths */
      if (!is_same_image && filepath[0] != '\0' && tex_slot->ima && tex_slot->ima->filepath) {
        if (BLI_path_cmp_normalized(filepath, tex_slot->ima->filepath) == 0) {
          is_same_image = true;
          PAINT_DEBUG_PRINT("paint_assign_image_exec: Same image file already assigned (filepath match)");
        }
      }
      
      if (!is_same_image) {
        tex_slot->ima = image;
        tex_slot->type = TEX_IMAGE;
        
        PAINT_DEBUG_PRINT("paint_assign_image_exec: Updated to different image '%s'", image->id.name);
      } else {
        PAINT_DEBUG_PRINT("paint_assign_image_exec: Same image already assigned, no reference count change needed");
        /* Same image already assigned - no need to change reference count */
      }
    }
    
    /* Mark texture as modified */
    DEG_id_tag_update(&tex_slot->id, ID_RECALC_SHADING);
  }
  else if (tex_slot && !tex_slot->ima) {
    /* Texture exists but has no image */
    PAINT_DEBUG_PRINT("paint_assign_image_exec: Assigning image to existing texture without image");
    
    tex_slot->ima = image;
    tex_slot->type = TEX_IMAGE;
    
    /* Mark texture as modified */
    DEG_id_tag_update(&tex_slot->id, ID_RECALC_SHADING);
  }
  else {
    /* No texture in slot - create new one */
    PAINT_DEBUG_PRINT("paint_assign_image_exec: Creating new texture for empty slot");

    const char *tex_name = use_mask_slot ? "BrushMaskTexture" : "BrushTexture";
    tex_slot = BKE_texture_add(bmain, tex_name);

    /* Setup texture image before assigning to brush (to avoid unnecessary refcount changes) */
    tex_slot->type = TEX_IMAGE;
    tex_slot->ima = image;

    /* Assign texture to brush using proper refcount management.
     * This is critical for undo/redo to work correctly - the undo system needs
     * consistent refcount to properly restore state. */
    if (use_mask_slot) {
      set_current_brush_mask_texture(brush, tex_slot);
    }
    else {
      set_current_brush_texture(brush, tex_slot);
    }

    PAINT_DEBUG_PRINT("paint_assign_image_exec: Created new texture '%s' with image '%s'",
           tex_slot->id.name, image->id.name);

    /* Mark texture as modified */
    DEG_id_tag_update(&tex_slot->id, ID_RECALC_SHADING);
  }

  /* Update brush */
  DEG_id_tag_update(&brush->id, 0);
  DEG_id_tag_update(&image->id, 0);

  /* Send notifications */
  WM_event_add_notifier(C, NC_TEXTURE | NA_EDITED, tex_slot);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
  
  /* CRITICAL: Force texture preview update */
  DROP_IMAGE_update_texture_preview_smart(C, bmain, tex_slot, true);
  
  /* ENHANCED: Force UI refresh for better visual feedback */
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
  
  /* ENHANCED: Force viewport refresh for Texture Paint mode */
  if (context_mode == CTX_MODE_PAINT_TEXTURE) {
    /* Additional viewport refresh for immediate texture display */
    Object *ob = CTX_data_active_object(C);
    if (ob) {
      DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
      PAINT_DEBUG_PRINT("paint_assign_image_exec: Tagged object for shading update in Texture Paint mode");
    }
  }

  /* Undo is handled by the operator system (OPTYPE_UNDO).
   * Pushing an extra undo step here can create a state where external-file backed images created
   * during drag & drop are not consistently available on undo/redo, causing invalid ID pointers
   * and crashes during refcount recomputation.
   * See .MyDoc_2026/LOGS/log_A1.md.
   */

  /* INTEGRATION: Clear drag state after successful completion */
  if (g_texture_drag_state.is_active) {
    DROP_IMAGE_drag_state_clear();
    PAINT_DEBUG_PRINT("paint_assign_image_exec: Cleared drag state after successful completion");
  }

  PAINT_DEBUG_PRINT("paint_assign_image_exec: Successfully assigned image to brush");
  return OPERATOR_FINISHED;
}

static bool paint_assign_image_poll(bContext *C)
{
  /* Only available when experimental flag is enabled */
  if (!USE_DND_TEXTURE()) {
    return false;
  }

  /* Check if we have an active paint mode and brush */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (!paint) {
    return false;
  }
  
  Brush *brush = BKE_paint_brush(paint);
  return (brush != nullptr);
}

void TEXTURE_OT_assign_image(wmOperatorType *ot)
{
  /* Identifiers */
  ot->name = "Assign Image to Brush Texture";
  ot->idname = "TEXTURE_OT_assign_image";
  ot->description = "Assign image to active brush texture slot";

  /* API callbacks */
  ot->exec = paint_assign_image_exec;
  ot->poll = paint_assign_image_poll;

  /* Flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  /* Properties */
  RNA_def_string_file_path(ot->srna, "filepath", nullptr, FILE_MAX, "File Path", "Path to image file");
  RNA_def_boolean(ot->srna, "use_mask_slot", false, "Use Mask Slot", "Assign to mask texture slot instead of main texture slot");
  RNA_def_boolean(ot->srna, "replace_existing", false, "Replace Existing", "Replace existing texture in slot instead of creating new one");
  RNA_def_boolean(ot->srna, "skip_reference_count", false, "Skip Reference Count", "Skip reference count changes (for same image detection)");
  
  /* ID lookup properties for drag and drop */
  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Texture with Image Operator
 * \{ */

/**
 * Operator to create a new texture with an image for brush.
 * Used for drag and drop functionality when no texture exists.
 */
static wmOperatorStatus paint_new_texture_with_image_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Starting execution");

  /* INTEGRATION: Initialize drag state if not already active */
  if (!g_texture_drag_state.is_active) {
    DROP_IMAGE_drag_state_init();
    PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Initialized drag state");
  }

  /* Get the image */
  Image *image = nullptr;
  
  /* Try to get image by filepath */
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  
  if (filepath[0] != '\0') {
    PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Loading image from filepath: %s", filepath);
    image = BKE_image_load_exists(bmain, filepath);
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
      BKE_report(op->reports, RPT_ERROR, "No image specified for texture creation");
      return OPERATOR_CANCELLED;
    }
    PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Using existing image: %s", image->id.name);
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

  PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Found active brush: %s", brush->id.name);

  /* IMPROVED: Enhanced slot determination logic */
  bool use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  
  /* ENHANCED: Check context mode for Sculpt mode special handling */
  const enum eContextObjectMode context_mode = CTX_data_mode_enum(C);
  if (context_mode == CTX_MODE_SCULPT) {
    /* In Sculpt mode, force main texture slot (mask slot doesn't exist) */
    use_mask_slot = false;
    PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: SCULPT MODE: Forcing main texture slot");
  } else if (context_mode == CTX_MODE_PAINT_TEXTURE) {
    /* In Texture Paint mode, respect the use_mask_slot parameter from dropbox */
    PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: TEXTURE PAINT MODE: Using slot from dropbox (use_mask_slot=%s)", 
           use_mask_slot ? "true" : "false");
  }

  PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Using %s slot",
         use_mask_slot ? "mask" : "main");

  /* Create new texture (always create new, even if slot is occupied) */
  const char *tex_name = use_mask_slot ? "BrushMaskTexture" : "BrushTexture";
  Tex *new_texture = BKE_texture_add(bmain, tex_name);

  PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Created new texture: %s", new_texture->id.name);

  /* Setup texture */
  new_texture->type = TEX_IMAGE;
  new_texture->ima = image;

  /* Assign texture to brush using proper refcount management.
   * This is critical for undo/redo to work correctly - the undo system needs
   * consistent refcount to properly restore state.
   */
  if (use_mask_slot) {
    set_current_brush_mask_texture(brush, new_texture);
  }
  else {
    set_current_brush_texture(brush, new_texture);
  }

  /* Update dependencies */
  DEG_id_tag_update(&new_texture->id, ID_RECALC_SHADING);
  DEG_id_tag_update(&brush->id, 0);
  DEG_id_tag_update(&image->id, 0);

  /* Send notifications */
  WM_event_add_notifier(C, NC_TEXTURE | NA_ADDED, new_texture);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
  
  /* CRITICAL: Force texture preview update */
  DROP_IMAGE_update_texture_preview_smart(C, bmain, new_texture, true);
  
  /* ENHANCED: Force UI refresh for better visual feedback */
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
  
  /* ENHANCED: Force viewport refresh for Texture Paint mode */
  if (context_mode == CTX_MODE_PAINT_TEXTURE) {
    /* Additional viewport refresh for immediate texture display */
    Object *ob = CTX_data_active_object(C);
    if (ob) {
      DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
      PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Tagged object for shading update in Texture Paint mode");
    }
  }

  /* Undo is handled by OPTYPE_UNDO. */

  /* INTEGRATION: Clear drag state after successful completion */
  if (g_texture_drag_state.is_active) {
    DROP_IMAGE_drag_state_clear();
    PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Cleared drag state after successful completion");
  }

  PAINT_DEBUG_PRINT("paint_new_texture_with_image_exec: Successfully created texture with image");
  return OPERATOR_FINISHED;
}

static bool paint_new_texture_with_image_poll(bContext *C)
{
  /* Only available when experimental flag is enabled */
  if (!USE_DND_TEXTURE()) {
    return false;
  }

  /* Check if we have an active paint mode and brush */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (!paint) {
    return false;
  }
  
  Brush *brush = BKE_paint_brush(paint);
  return (brush != nullptr);
}

void TEXTURE_OT_new_with_image(wmOperatorType *ot)
{
  /* Identifiers */
  ot->name = "New Texture with Image";
  ot->idname = "TEXTURE_OT_new_with_image";
  ot->description = "Create new texture with image for active brush";

  /* API callbacks */
  ot->exec = paint_new_texture_with_image_exec;
  ot->poll = paint_new_texture_with_image_poll;

  /* Flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  /* Properties */
  RNA_def_string_file_path(ot->srna, "filepath", nullptr, FILE_MAX, "File Path", "Path to image file");
  RNA_def_boolean(ot->srna, "use_mask_slot", false, "Use Mask Slot", "Create texture in mask slot instead of main texture slot");
  
  /* ID lookup properties for drag and drop */
  WM_operator_properties_id_lookup(ot, true);
}

/** \} */

}
