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
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
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
#include "BKE_image_format.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_texture.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_image.hh"
#include "ED_node.hh"
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

#include "../io/io_utils.hh"
#include "interface_drop_image.hh"
#include "interface_intern.hh"
#include "../space_node/node_intern.hh"

namespace blender {

/** -------------------------------------------------------------------- */
/** \name Preview Scaling Utility Functions
 * \{ */

/* Debug macro for consistent logging - only prints when g_drop_image_debug_enabled is true */
#define DROP_IMAGE_DEBUG_PRINT(fmt, ...) \
  do { \
    if (g_drop_image_debug_enabled) { \
      printf("[DEBUG] DROP_IMAGE: " fmt "\n", ##__VA_ARGS__); \
    } \
  } while (0)

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
    DROP_IMAGE_DEBUG_PRINT("scale_image_buffer: Image does not require scaling");
    return ibuf;
  }

  /* Calculate new dimensions with aspect ratio preservation */
  int new_width, new_height;
  DROP_IMAGE_calculate_scaled_dimensions(ibuf->x, ibuf->y, max_size, &new_width, &new_height);

  /* Create new scaled buffer */
  ImBuf *scaled_ibuf = IMB_scale_into_new(ibuf, new_width, new_height, IMBScaleFilter::Bilinear, false);
  if (scaled_ibuf) {
    DROP_IMAGE_DEBUG_PRINT("scale_image_buffer: Image scaled to %dx%d (max_size=%d)", 
                           new_width, new_height, max_size);
    return scaled_ibuf;
  }
  else {
    DROP_IMAGE_DEBUG_PRINT("scale_image_buffer: Image scaling error");
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
    DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview: Unsupported file extension: %s", filepath);
    return nullptr;
  }

  /* Load image */
  ImBuf *ibuf = IMB_load_image_from_filepath(filepath, IB_byte_data | IB_metadata, nullptr);
  if (!ibuf) {
    DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview: Failed to load image: %s", filepath);
    return nullptr;
  }

  DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview: Loaded image %s (size: %dx%d)", 
                         filepath, ibuf->x, ibuf->y);

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

  DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview_from_id: Loading preview for Image ID: %s", image->id.name);

  /* Get ImBuf from image */
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, nullptr);
  if (!ibuf) {
    DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview_from_id: Failed to get image buffer");
    return nullptr;
  }

  /* Create copy for preview */
  ImBuf *preview_copy = IMB_dupImBuf(ibuf);
  BKE_image_release_ibuf(image, ibuf, nullptr);
  
  if (!preview_copy) {
    DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview_from_id: Failed to create buffer copy");
    return nullptr;
  }

  DROP_IMAGE_DEBUG_PRINT("load_and_scale_preview_from_id: Got image buffer (size: %dx%d)", 
                         preview_copy->x, preview_copy->y);

  /* Scale if needed */
  ImBuf *result = DROP_IMAGE_scale_image_buffer(preview_copy, max_size);
  if (result != preview_copy) {
    IMB_freeImBuf(preview_copy); // Free original if scaled
  }
  
  return result;
}

/** \} */

/** -------------------------------------------------------------------- */
/** \name Tooltip Generation Functions
 * \{ */

/**
 * Generates tooltip for Node Editor texture drop operations.
 * Checks if cursor is over existing Image Texture Node and provides appropriate tooltip.
 * 
 * @param C Blender context
 * @param filename Name of the image file being dropped
 * @return Tooltip string for Node Editor
 */
std::string DROP_IMAGE_tooltip_node_editor(bContext *C, const char *filename)
{
  static char tooltip[256] = "";
  
  /* Check if cursor is over existing Image Texture Node */
  SpaceNode *snode = CTX_wm_space_node(C);
  bool is_over_image_node = false;
  
  if (snode && snode->edittree) {
    float2 cursor_pos = snode->runtime->cursor * UI_SCALE_FAC;
    for (bNode &node : ListBaseT<bNode>(snode->edittree->nodes)) {
      if (BLI_rctf_isect_pt(&node.runtime->draw_bounds, cursor_pos[0], cursor_pos[1])) {
        /* Check if node is Image Texture Node */
        if ((snode->edittree->type == NTREE_SHADER && node.type_legacy == SH_NODE_TEX_IMAGE) ||
            (snode->edittree->type == NTREE_GEOMETRY && node.type_legacy == GEO_NODE_IMAGE_TEXTURE) ||
            (snode->edittree->type == NTREE_TEXTURE && node.type_legacy == TEX_NODE_IMAGE) ||
            (snode->edittree->type == NTREE_COMPOSIT && node.type_legacy == CMP_NODE_IMAGE)) {
          is_over_image_node = true;
          break;
        }
      }
    }
  }
  
  if (is_over_image_node) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Replace image in Image Texture Node with '%s'", filename);
  } else {
    BLI_snprintf(tooltip, sizeof(tooltip), "Create Image Texture Node with image '%s'", filename);
  }
  
  return std::string(tooltip);
}

/**
 * Generates tooltip for 3D Viewport texture drop operations.
 * Handles brush texture assignment and material operations.
 * ENHANCED: Now integrates with TextureDragState for better feedback.
 * 
 * @param C Blender context
 * @param filename Name of the image file being dropped
 * @param op_idname Operator ID name for the drop operation
 * @return Tooltip string for 3D Viewport
 */
std::string DROP_IMAGE_tooltip_view3d(bContext *C, const char *filename, const char *op_idname)
{
  static char tooltip[512] = "";
  
  /* Check paint mode for more accurate tooltip */
  TextureDropContext context(C);
  
  /* ENHANCED: Check drag state for additional context */
  bool is_drag_active = DROP_IMAGE_drag_state_is_active();
  const char *drag_status = is_drag_active ? " (drag active)" : "";
  
  /* Determine slot information for tooltip */
  const char *slot_info = "";
  bool use_mask_slot = false;
  
  /* Get active brush to determine slot */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint && paint->brush) {
    Brush *brush = paint->brush;
    
    /* ENHANCED: Use drag state information if available */
    if (is_drag_active && g_texture_drag_state.active_brush == brush) {
      use_mask_slot = g_texture_drag_state.is_mask_target;
      slot_info = use_mask_slot ? " (mask slot - drag target)" : " (main slot - drag target)";
      DROP_IMAGE_DEBUG_PRINT("Using drag state info - mask_target=%s", use_mask_slot ? "true" : "false");
    }
    else {
      /* Check if mask slot is used */
      if (context.paint_mode == TexturePaintMode::SCULPT) {
        /* In Sculpt mode always use main slot */
        slot_info = " (main slot)";
        DROP_IMAGE_DEBUG_PRINT("Sculpt mode - using main texture slot");
      }
      else if (context.paint_mode == TexturePaintMode::TEXTURE_PAINT) {
        /* In Texture Paint mode check settings */
        /* CRITICAL: Validate texture pointers before access to prevent crash after undo */
        bool mask_tex_valid = brush->mask_mtex.tex && BKE_id_is_in_global_main(&brush->mask_mtex.tex->id);
        bool main_tex_valid = brush->mtex.tex && BKE_id_is_in_global_main(&brush->mtex.tex->id);
        if (mask_tex_valid && (!main_tex_valid || brush->mask_mtex.tex != brush->mtex.tex)) {
          use_mask_slot = true;
          slot_info = " (mask slot)";
          DROP_IMAGE_DEBUG_PRINT("Texture Paint mode - using mask slot");
        }
        else {
          slot_info = " (main slot)";
          DROP_IMAGE_DEBUG_PRINT("Texture Paint mode - using main slot");
        }
      }
      else {
        slot_info = " (main slot)";
        DROP_IMAGE_DEBUG_PRINT("Other paint mode - using main slot");
      }
    }
  }
  
  if (context.paint_mode == TexturePaintMode::TEXTURE_PAINT) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' as texture for Texture Paint%s%s", filename, slot_info, drag_status);
  }
  else if (context.paint_mode == TexturePaintMode::SCULPT) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' as texture for Sculpt%s%s", filename, slot_info, drag_status);
  }
  else if (context.paint_mode == TexturePaintMode::VERTEX_PAINT) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' as texture for Vertex Paint%s%s", filename, slot_info, drag_status);
  }
  else if (context.paint_mode == TexturePaintMode::WEIGHT_PAINT) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' as texture for Weight Paint%s%s", filename, slot_info, drag_status);
  }
  else {
    /* General tooltip for 3D Viewport */
    if (STREQ(op_idname, "PAINT_OT_brush_texture_set") || 
        STREQ(op_idname, "TEXTURE_OT_new_with_image") || 
        STREQ(op_idname, "TEXTURE_OT_assign_image")) {
      BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' as brush texture%s%s", filename, slot_info, drag_status);
    }
    else {
      BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' in 3D Viewport%s", filename, drag_status);
    }
  }
  
  return std::string(tooltip);
}

/**
 * Generates tooltip for Properties Editor texture drop operations.
 * Handles material and texture property assignments.
 * 
 * @param C Blender context
 * @param filename Name of the image file being dropped
 * @param op_idname Operator ID name for the drop operation
 * @return Tooltip string for Properties Editor
 */
std::string DROP_IMAGE_tooltip_properties(bContext *C, const char *filename, const char *op_idname)
{
  static char tooltip[256] = "";
  
  if (STREQ(op_idname, "TEXTURE_OT_new_with_image") || 
      STREQ(op_idname, "TEXTURE_OT_assign_image")) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Create new texture with image '%s'", filename);
  }
  else if (STREQ(op_idname, "MATERIAL_OT_new_with_image")) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Create new material with image '%s'", filename);
  }
  else {
    BLI_snprintf(tooltip, sizeof(tooltip), "Set image '%s' in Properties", filename);
  }
  
  return std::string(tooltip);
}

/**
 * Generates tooltip for Image Editor texture drop operations.
 * Handles image loading and editing operations.
 * 
 * @param C Blender context
 * @param filename Name of the image file being dropped
 * @param op_idname Operator ID name for the drop operation
 * @return Tooltip string for Image Editor
 */
std::string DROP_IMAGE_tooltip_image_editor(bContext *C, const char *filename, const char *op_idname)
{
  static char tooltip[256] = "";
  
  if (STREQ(op_idname, "IMAGE_OT_open")) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Open image '%s' in Image Editor", filename);
  }
  else if (STREQ(op_idname, "IMAGE_OT_replace")) {
    BLI_snprintf(tooltip, sizeof(tooltip), "Replace current image with '%s'", filename);
  }
  else {
    BLI_snprintf(tooltip, sizeof(tooltip), "Load image '%s' in Image Editor", filename);
  }
  
  return std::string(tooltip);
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

  if (!BKE_id_is_in_global_main(&tex->id)) {
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Texture is not in Main anymore: %s",
                           tex->id.name + 2);
    return;
  }

  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Starting preview update for texture: %s", 
         tex->id.name + 2);

  /* 1. CRITICAL: Stop all competing preview jobs to prevent conflicts */
  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Cancelled existing preview jobs");

  /* 2. Force preview regeneration with enhanced system */
  BKE_previewimg_id_free(&tex->id);
  BKE_previewimg_id_ensure(&tex->id);
  BKE_icon_changed(BKE_icon_id_ensure(&tex->id));
  ED_previews_tag_dirty_by_id(*bmain, tex->id);
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Updated preview system");

  /* 3. Render preview synchronously to avoid dangling job access during immediate Undo. */
  ui::icon_render_id(C, nullptr, &tex->id, ICON_SIZE_PREVIEW, false);
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Rendered preview synchronously");

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
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Updated all relevant areas");

  /* 5. Update dependency graph with enhanced flags */
  DEG_id_tag_update(&tex->id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL); //ID_RECALC_SYNC_TO_EVAL проверрить как влияет на производительность
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

  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview: Preview update completed");
}

/**
 * Enhanced preview update function specifically for Texture Paint mode.
 * Combines Properties Editor and 3D Viewport updates to ensure immediate texture display.
 * This function addresses the issue where texture previews don't update in Texture Paint mode.
 * ENHANCED: Now integrates with TextureDragState for better state management.
 * 
 * @param C Blender context
 * @param bmain Main database
 * @param tex Texture to update preview for
 * @param brush Active brush (optional, for brush-specific updates)
 */
void DROP_IMAGE_update_texture_paint_preview(bContext *C, Main *bmain, Tex *tex, Brush *brush)
{
  if (!tex) {
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: No texture provided");
    return;
  }

  if (!BKE_id_is_in_global_main(&tex->id)) {
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Texture is not in Main anymore: %s",
                           tex->id.name + 2);
    return;
  }
  
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Starting enhanced preview update for texture: %s", 
         tex->id.name + 2);
  
  /* ENHANCED: Check drag state for additional context */
  bool is_drag_active = DROP_IMAGE_drag_state_is_active();
  if (is_drag_active) {
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Drag operation active, using enhanced update");
    
    /* Update drag state with current texture */
    if (g_texture_drag_state.active_brush && brush == g_texture_drag_state.active_brush) {
      g_texture_drag_state.over_valid_target = true;
      g_texture_drag_state.highlight_alpha = 1.0f; // Full highlight during update
      DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Updated drag state for active brush");
    }
  }
  
  /* 1. CRITICAL: Cancel all competing preview jobs to prevent conflicts */
  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Cancelled existing preview jobs");
  
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
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Updated Properties Editor preview");
  
  /* 3. ENHANCEMENT: Add 3D Viewport-specific refresh for Texture Paint mode */
  for (bScreen &screen : ListBaseT<bScreen>(bmain->screens)) {
    for (ScrArea &area : ListBaseT<ScrArea>(screen.areabase)) {
      if (area.spacetype == SPACE_VIEW3D) {
        ED_region_tag_redraw((ARegion *)area.regionbase.first);
      }
    }
  }
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Updated 3D Viewport");
  
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
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Updated brush dependencies");
  }
  
  /* ENHANCED: Reset drag state highlight after update */
  if (is_drag_active && g_texture_drag_state.active_brush && brush == g_texture_drag_state.active_brush) {
    g_texture_drag_state.highlight_alpha = 0.8f; // Reduce highlight after update
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Reset drag state highlight");
  }
  
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_paint_preview: Enhanced preview update completed");
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
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview_smart: Invalid parameters - C:%p bmain:%p tex:%p", 
           (void*)C, (void*)bmain, (void*)tex);
    return;
  }

  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview_smart: Starting smart preview update for texture: %s", 
         tex->id.name + 2);

  /* Determine the best update method based on context */
  TextureDropContext context(C);
  
  /* Check if we're in Texture Paint mode */
  if (context.paint_mode == TexturePaintMode::TEXTURE_PAINT) {
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview_smart: Using Texture Paint specific update");
    DROP_IMAGE_update_texture_paint_preview(C, bmain, tex, context.active_brush);
  }
  else {
    DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview_smart: Using universal update");
    DROP_IMAGE_update_texture_preview(C, bmain, tex, force_update);
  }
  
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_update_texture_preview_smart: Smart preview update completed");
}

/** \} */

/** -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

/**
 * Universal tooltip generator for texture drop operations.
 * Determines editor type and generates appropriate tooltip based on context.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param xy Mouse coordinates
 * @param drop Drop box configuration
 * @return Generated tooltip string
 */
std::string DROP_IMAGE_drop_tooltip(bContext *C, wmDrag *drag, const int xy[2], wmDropBox *drop)
{
  DROP_IMAGE_DEBUG_PRINT("DROP_IMAGE_drop_tooltip: ENTRY - Function called from interface_drop_image_feedback.cc");
  
  /* Determine operation type by dropbox */
  const char *op_idname = drop->ot ? drop->ot->idname : "";
  
  if (drag->type != WM_DRAG_PATH) {
    return std::string("Unsupported drag type");
  }
  
  const char *path = WM_drag_get_single_path(drag);
  if (!path) {
    return std::string("Failed to get file path");
  }
  
  /* Extract filename from path */
  const char *filename = strrchr(path, '/');
  if (!filename) {
    filename = strrchr(path, '\\');
  }
  filename = filename ? filename + 1 : path;
  
  /* Determine editor type and generate appropriate tooltip */
  TextureDropContext context(C);
  
  switch (context.editor_type) {
    case TextureDropEditorType::NODE_EDITOR:
      return DROP_IMAGE_tooltip_node_editor(C, filename);
      
    case TextureDropEditorType::VIEW3D:
      return DROP_IMAGE_tooltip_view3d(C, filename, op_idname);
      
    case TextureDropEditorType::PROPERTIES:
      return DROP_IMAGE_tooltip_properties(C, filename, op_idname);
      
    case TextureDropEditorType::IMAGE_EDITOR:
      return DROP_IMAGE_tooltip_image_editor(C, filename, op_idname);
      
    case TextureDropEditorType::UNSUPPORTED:
    default:
      return std::string("Unsupported editor for image drag and drop");
  }
}

/** \} */

}  // namespace blender
