/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Texture drop visual feedback functionality for UI elements.
 * Handles tooltip generation and preview updates during drag and drop operations.
 * Provides real-time visual feedback to users about the state of their drag operations.
 */

#include <cstdio>
#include <cstdlib>

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_fileops.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_paint.hh"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_texture.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_paint.hh"
#include "ED_render.hh"
#include "ED_screen.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_colormanagement.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm_window.hh"

#include <fmt/format.h>

#include "interface_drop_image.hh"
#include "interface_intern.hh"

namespace blender {

/** -------------------------------------------------------------------- */
/** \name Preview Scaling Utility Functions
 * \{ */

/**
 * Calculates new dimensions for scaling while preserving aspect ratio.
 * 
 * @param original_width Original image width
 * @param original_height Original image height  
 * @param max_size Maximum size constraint
 * @param new_width Output parameter for new width
 * @param new_height Output parameter for new height
 */
static void DROP_IMAGE_calculate_scaled_dimensions(int original_width, int original_height, 
                                                   int max_size, int *new_width, int *new_height)
{
  if (original_width > original_height) {
    *new_width = max_size;
    *new_height = int(original_height * (float(*new_width) / float(original_width)));
  }
  else if (original_height > original_width) {
    *new_height = max_size;
    *new_width = int(original_width * (float(*new_height) / float(original_height)));
  }
  else {
    *new_width = *new_height = max_size;
  }
}

/**
 * Scales an image buffer to fit within maximum size while preserving aspect ratio.
 * 
 * @param ibuf Input image buffer to scale
 * @param max_size Maximum size for the preview
 * @return New scaled ImBuf, or nullptr if scaling failed
 */
static ImBuf *DROP_IMAGE_scale_image_buffer(ImBuf *ibuf, int max_size)
{
  if (!ibuf) {
    return nullptr;
  }

  /* Check if scaling is needed */
  if (ibuf->x <= max_size && ibuf->y <= max_size) {
    return ibuf;
  }

  /* Calculate new dimensions with aspect ratio preservation */
  int new_width, new_height;
  DROP_IMAGE_calculate_scaled_dimensions(ibuf->x, ibuf->y, max_size, &new_width, &new_height);

  /* Create new scaled buffer */
  ImBuf *scaled_ibuf = IMB_scale_into_new(ibuf, new_width, new_height, IMBScaleFilter::Bilinear, false);
  if (scaled_ibuf) {
    return scaled_ibuf;
  }
  else {
    return nullptr;
  }
}

/**
 * Loads and scales an image preview for drag operations.
 * Creates a thumbnail-sized preview image for drag feedback.
 * 
 * @param filepath Path to the image file
 * @param max_size Maximum size for the preview (default 128px)
 * @return ImBuf containing the scaled preview, or nullptr if failed
 */
ImBuf *DROP_IMAGE_load_and_scale_preview(const char *filepath, int max_size)
{
  if (!filepath) {
    return nullptr;
  }

  /* Check file extension */
  if (!BLI_path_extension_check_array(filepath, imb_ext_image)) {
    return nullptr;
  }

  /* Load image */
  ImBuf *ibuf = IMB_load_image_from_filepath(
      filepath, ImBufFlags::ByteData | ImBufFlags::Metadata, nullptr);
  if (!ibuf) {
    return nullptr;
  }

  /* Scale if needed */
  ImBuf *result = DROP_IMAGE_scale_image_buffer(ibuf, max_size);
  if (result != ibuf) {
    IMB_freeImBuf(ibuf); // Free original if scaled
  }
  
  return result;
}

/**
 * Loads and scales an image preview from Blender ID object for drag operations.
 * Creates a thumbnail-sized preview image for drag feedback from existing Image ID.
 * 
 * @param image Pointer to Image ID object
 * @param max_size Maximum size for the preview (default 128px)
 * @return ImBuf containing the scaled preview, or nullptr if failed
 */
ImBuf *DROP_IMAGE_load_and_scale_preview_from_id(Image *image, int max_size)
{
  if (!image) {
    return nullptr;
  }

  /* Get ImBuf from image */
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, nullptr);
  if (!ibuf) {
    return nullptr;
  }

  /* Create copy for preview */
  ImBuf *preview_copy = IMB_dupImBuf(ibuf);
  BKE_image_release_ibuf(image, ibuf, nullptr);
  
  if (!preview_copy) {
    return nullptr;
  }

  /* Scale if needed */
  ImBuf *result = DROP_IMAGE_scale_image_buffer(preview_copy, max_size);
  if (result != preview_copy) {
    IMB_freeImBuf(preview_copy); // Free original if scaled
  }
  
  return result;
}

/** \} */

/** -------------------------------------------------------------------- */
/** \name Preview Update Functions
 * \{ */

/**
 * Updates texture preview in the UI system.
 * Forces regeneration of texture previews and updates UI elements.
 * Enhanced version that works for any editor type and ensures immediate visual feedback.
 * 
 * @param C Blender context
 * @param bmain Main database
 * @param tex Texture to update preview for
 * @param force_update Whether to force update even if not needed
 */
void DROP_IMAGE_update_texture_preview(bContext *C, Main *bmain, Tex *tex, bool force_update)
{
  if (!C || !bmain || !tex) {
    return;
  }

  /* 1. CRITICAL: Stop all competing preview jobs to prevent conflicts */
  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);

  /* 2. Force preview regeneration with enhanced system */
  BKE_previewimg_id_free(&tex->id);
  BKE_previewimg_id_ensure(&tex->id);
  BKE_icon_changed(BKE_icon_id_ensure(&tex->id));
  ED_previews_tag_dirty_by_id(*bmain, tex->id);

  /* 3. Start asynchronous rendering for better performance */
  ui::icon_render_id(C, nullptr, &tex->id, ICON_SIZE_PREVIEW, true);

  /* 4. Universal area refresh - update all relevant areas, not just Properties */
  for (bScreen &screen : ListBaseT<bScreen>(bmain->screens)) {
    for (ScrArea &area : ListBaseT<ScrArea>(screen.areabase)) {
      switch (area.spacetype) {
        case SPACE_PROPERTIES: {
          SpaceProperties *sbuts = (SpaceProperties *)area.spacedata.first;
          if (sbuts) {
            sbuts->preview = 1;
          }
          break;
        }
        case SPACE_VIEW3D: {
          /* Force 3D Viewport refresh for immediate texture display */
          ED_region_tag_redraw((ARegion *)area.regionbase.first);
          break;
        }
        case SPACE_NODE: {
          /* Force Node Editor refresh for texture nodes */
          ED_region_tag_redraw((ARegion *)area.regionbase.first);
          break;
        }
        case SPACE_IMAGE: {
          /* Force Image Editor refresh */
          ED_region_tag_redraw((ARegion *)area.regionbase.first);
          break;
        }
        default:
          break;
      }
    }
  }

  /* 5. Update dependency graph with enhanced flags */
  DEG_id_tag_update(&tex->id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  DEG_relations_tag_update(bmain);

  /* 6. Send comprehensive notifications for all relevant systems */
  WM_event_add_notifier(C, NC_TEXTURE | ND_SHADING_PREVIEW, &tex->id);
  WM_event_add_notifier(C, NC_TEXTURE | ND_SHADING_DRAW, &tex->id);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_NODE, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (force_update) {
    WM_event_add_notifier(C, NC_TEXTURE | ND_SHADING_DRAW, nullptr);
  }

}

/**
 * Enhanced preview update function specifically for Texture Paint mode.
 * Combines Properties Editor and 3D Viewport updates to ensure immediate texture display.
 * This function addresses the issue where texture previews don't update in Texture Paint mode.
 *
 * @param C Blender context
 * @param bmain Main database
 * @param tex Texture to update preview for
 * @param brush Active brush (optional, for brush-specific updates)
 */
void DROP_IMAGE_update_texture_paint_preview(bContext *C, Main *bmain, Tex *tex, Brush *brush)
{
  if (!tex) {
    return;
  }
  
  /* 1. CRITICAL: Cancel all competing preview jobs to prevent conflicts */
  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);
  
  /* 2. Force Properties Editor preview update (same as standard function) */
  BKE_previewimg_id_free(&tex->id);
  BKE_previewimg_id_ensure(&tex->id);
  BKE_icon_changed(BKE_icon_id_ensure(&tex->id));
  ED_previews_tag_dirty_by_id(*bmain, tex->id);
  
  /* Force Properties Editor preview refresh */
  for (bScreen &screen : ListBaseT<bScreen>(bmain->screens)) {
    for (ScrArea &area : ListBaseT<ScrArea>(screen.areabase)) {
      if (area.spacetype == SPACE_PROPERTIES) {
        SpaceProperties *sbuts = (SpaceProperties *)area.spacedata.first;
        if (sbuts) {
          sbuts->preview = 1;
        }
      }
    }
  }
  
  /* 3. ENHANCEMENT: Add 3D Viewport-specific refresh for Texture Paint mode */
  for (bScreen &screen : ListBaseT<bScreen>(bmain->screens)) {
    for (ScrArea &area : ListBaseT<ScrArea>(screen.areabase)) {
      if (area.spacetype == SPACE_VIEW3D) {
        ED_region_tag_redraw((ARegion *)area.regionbase.first);
      }
    }
  }
  
  /* 4. Send comprehensive notifications for all relevant systems */
  WM_event_add_notifier(C, NC_TEXTURE | ND_SHADING_PREVIEW, &tex->id);
  WM_event_add_notifier(C, NC_TEXTURE | ND_SHADING_DRAW, &tex->id);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  
  /* 5. Update dependency graph for immediate visual feedback */
  DEG_id_tag_update(&tex->id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  
  /* 6. If brush is provided, update brush-specific dependencies */
  if (brush) {
    DEG_id_tag_update(&brush->id, ID_RECALC_PARAMETERS);
    WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  }
  
}

/**
 * Universal texture preview update function that automatically selects the best method
 * based on the current context and editor type.
 * 
 * @param C Blender context
 * @param bmain Main database
 * @param tex Texture to update preview for
 * @param force_update Whether to force update even if not needed
 */
void DROP_IMAGE_update_texture_preview_smart(bContext *C, Main *bmain, Tex *tex, bool force_update)
{
  if (!C || !bmain || !tex) {
    return;
  }

  /* Texture Paint mode needs the 3D viewport refreshed as well as the previews. */
  const Object *ob = CTX_data_active_object(C);
  if (ob && ob->mode == OB_MODE_TEXTURE_PAINT) {
    Brush *brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));
    DROP_IMAGE_update_texture_paint_preview(C, bmain, tex, brush);
  }
  else {
    DROP_IMAGE_update_texture_preview(C, bmain, tex, force_update);
  }
}

/** \} */

}  // namespace blender