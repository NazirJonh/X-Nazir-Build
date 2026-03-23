/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Texture drop functionality for UI elements.
 * Handles drag and drop operations for textures and images onto UI buttons.
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
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

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

/* g_drop_image_debug_enabled declared in interface_intern.hh - default false */

/** -------------------------------------------------------------------- */
/** \name Type Definitions and Enums
 * \{ */

/**
 * Implementation of TextureDropContext constructor.
 */
TextureDropContext::TextureDropContext(bContext *context) : C(context) {
  editor_type = TextureDropEditorType::UNSUPPORTED;
  paint_mode = TexturePaintMode::NONE;
  region = nullptr;
  target_button = nullptr;
  slot_type = TextureSlotType::MAIN_TEXTURE;
  active_brush = nullptr;
  active_object = nullptr;
  
  if (C) {
    /* Determine editor type */
    SpaceLink *sl = CTX_wm_space_data(C);
    if (sl) {
      switch (sl->spacetype) {
        case SPACE_VIEW3D:
          editor_type = TextureDropEditorType::VIEW3D;
          break;
        case SPACE_PROPERTIES:
          editor_type = TextureDropEditorType::PROPERTIES;
          break;
        case SPACE_IMAGE:
          editor_type = TextureDropEditorType::IMAGE_EDITOR;
          break;
        case SPACE_NODE:
          editor_type = TextureDropEditorType::NODE_EDITOR;
          break;
        default:
          editor_type = TextureDropEditorType::UNSUPPORTED;
          break;
      }
    }
    
    /* Get region */
    region = CTX_wm_region(C);
    
    /* Get active objects and brushes */
    active_object = CTX_data_active_object(C);
    active_brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));
    
    /* Determine paint mode */
    if (active_brush) {
      Scene *scene = CTX_data_scene(C);
      if (scene && scene->toolsettings) {
        ToolSettings *ts = scene->toolsettings;
        if (ts->sculpt && ts->sculpt->paint.brush == active_brush) {
          paint_mode = TexturePaintMode::SCULPT;
        }
        else if (ts->imapaint.paint.brush == active_brush) {
          paint_mode = TexturePaintMode::TEXTURE_PAINT;
        }
        else if (ts->vpaint && ts->vpaint->paint.brush == active_brush) {
          paint_mode = TexturePaintMode::VERTEX_PAINT;
        }
        else if (ts->wpaint && ts->wpaint->paint.brush == active_brush) {
          paint_mode = TexturePaintMode::WEIGHT_PAINT;
        }
      }
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Texture Drag State Implementation
 * \{ */

/**
 * Global texture drag state instance.
 */
TextureDragState g_texture_drag_state;

void TextureDragState::init()
{
  is_active = false;
  active_brush = nullptr;
  highlight_alpha = 0.0f;
  over_valid_target = false;
  highlight_timer = nullptr;
  is_mask_target = false;
}

void TextureDragState::clear()
{
  is_active = false;
  active_brush = nullptr;
  highlight_alpha = 0.0f;
  over_valid_target = false;
  if (highlight_timer) {
    // Timer cleanup would be handled by the caller with proper context
    highlight_timer = nullptr;
  }
  is_mask_target = false;
}

bool TextureDragState::is_drag_active() const
{
  return is_active;
}

/* -------------------------------------------------------------------- */
/** \name Texture Drag Data Implementation
 * \{ */

void TextureDragData::set_from_context(bContext *C, const wmEvent *event)
{
  if (!C || !event) {
    return;
  }
  
  /* Store mouse coordinates */
  mval[0] = event->mval[0];
  mval[1] = event->mval[1];
  
  /* Determine editor type */
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    space_type = area->spacetype;
    
    /* If Properties editor, remember context */
    if (area->spacetype == SPACE_PROPERTIES) {
      SpaceProperties *sbuts = CTX_wm_space_properties(C);
      if (sbuts) {
        properties_context = sbuts->mainb;
      }
    }
  }
  
  /* Get active brush */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint) {
    active_brush = BKE_paint_brush(paint);
  }
  
  /* Set drag flags */
  is_dragging = true;
  highlight_alpha = 0.0f;
  is_over_valid_target = false;
  from_template_id = false;
}

void TextureDragData::clear()
{
  mval[0] = mval[1] = 0;
  active_brush = nullptr;
  space_type = -1;
  properties_context = -1;
  is_dragging = false;
  highlight_alpha = 0.0f;
  is_over_valid_target = false;
  filepath[0] = '\0';
  from_template_id = false;
}

/* -------------------------------------------------------------------- */
/** \name Texture Drag State Management Functions
 * \{ */

void DROP_IMAGE_drag_state_init()
{
  g_texture_drag_state.init();
}

void DROP_IMAGE_drag_state_clear()
{
  g_texture_drag_state.clear();
}

bool DROP_IMAGE_drag_state_is_active()
{
  return g_texture_drag_state.is_drag_active();
}

void DROP_IMAGE_drag_state_set_active(Brush *brush, bool is_mask_target)
{
  g_texture_drag_state.is_active = true;
  g_texture_drag_state.active_brush = brush;
  g_texture_drag_state.is_mask_target = is_mask_target;
  g_texture_drag_state.highlight_alpha = 0.0f;
  g_texture_drag_state.over_valid_target = false;
}

/* -------------------------------------------------------------------- */
/** \name Texture Drag Data Management Functions
 * \{ */

TextureDragData *DROP_IMAGE_drag_data_create(bContext *C, const wmEvent *event)
{
  TextureDragData *data = MEM_new<TextureDragData>(__func__);
  data->set_from_context(C, event);
  return data;
}

void DROP_IMAGE_drag_data_free(TextureDragData *data)
{
  if (data) {
    MEM_delete(data);
  }
}

void DROP_IMAGE_drag_data_set(TextureDragData *data, bContext *C, const wmEvent *event)
{
  if (data) {
    data->set_from_context(C, event);
  }
}

/* -------------------------------------------------------------------- */
/** \name Texture Preview Update Functions
 * \{ */

/**
 * Viewport refresh utility specifically for Texture Paint mode.
 * Handles 3D Viewport updates that are missing in standard preview updates.
 * This ensures texture changes are immediately visible in the 3D Viewport.
 * 
 * @param C Blender context
 * @param brush Active brush (optional, for brush-specific viewport updates)
 */
static void refresh_texture_paint_viewport(bContext *C, Brush *brush)
{
  printf("[DEBUG] refresh_texture_paint_viewport: Starting viewport refresh\n");
  
  if (!C) {
    printf("[DEBUG] refresh_texture_paint_viewport: No context provided\n");
    return;
  }
  
  // Get the active object for material preview updates
  Object *ob = CTX_data_active_object(C);
  if (ob) {
    // Tag object for redraw to update material preview in viewport
    DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
    printf("[DEBUG] refresh_texture_paint_viewport: Tagged active object for shading update\n");
  }
  
  // Force 3D Viewport redraw for immediate visual feedback
  for (bScreen &screen : ListBaseT<bScreen>(CTX_data_main(C)->screens)) {
    for (ScrArea &area : ListBaseT<ScrArea>(screen.areabase)) {
      if (area.spacetype == SPACE_VIEW3D) {
        for (ARegion &region : ListBaseT<ARegion>(area.regionbase)) {
          if (region.regiontype == RGN_TYPE_WINDOW) {
            // Tag region for redraw to update texture display
            ED_region_tag_redraw(&region);
            printf("[DEBUG] refresh_texture_paint_viewport: Tagged 3D Viewport region for redraw\n");
          }
        }
      }
    }
  }
  
  // Invalidate brush texture cache if brush is provided
  if (brush) {
    // Clear any cached brush texture data to force reload
    if (brush->mtex.tex) {
      BKE_icon_changed(BKE_icon_id_ensure(&brush->mtex.tex->id));
      printf("[DEBUG] refresh_texture_paint_viewport: Invalidated main brush texture cache\n");
    }
    if (brush->mask_mtex.tex) {
      BKE_icon_changed(BKE_icon_id_ensure(&brush->mask_mtex.tex->id));
      printf("[DEBUG] refresh_texture_paint_viewport: Invalidated mask brush texture cache\n");
    }
  }
  
  // Send viewport-specific notifications
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  
  printf("[DEBUG] refresh_texture_paint_viewport: Viewport refresh completed\n");
}

/* -------------------------------------------------------------------- */
/** \name Texture Drop Validation Functions
 * \{ */

/**
 * IMPROVED: Enhanced validation for brush texture property.
 * Based on the approach from code_3.cc example with strict validation.
 * 
 * @param but UI button to check
 * @return true if button is valid texture slot
 */
bool DROP_IMAGE_is_valid_brush_texture_property(const ui::Button *but, bContext *C)
{
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Checking button %p\n", but);
  }
  
  if (!but) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Button is NULL\n");
    }
    return false;
  }
  
  /* Get current brush for intelligent slot detection */
  Brush *brush = nullptr;
  if (C) {
    Paint *paint = BKE_paint_get_active_from_context(C);
    if (paint) {
      brush = BKE_paint_brush(paint);
    }
  }
  
  if (!but->rnaprop) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Button has no RNA property (button type: %d)\n", int(but->type));
      if (but->block && !but->block->name.empty()) {
        printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Button block name: %s\n", but->block->name.c_str());
      }
    }
    
  /* Check if this is a texture slot button without RNA property but in correct context */
  if (but->block && !but->block->name.empty()) {
    const char *block_name = but->block->name.c_str();
    
    /* CRITICAL: Explicitly exclude Asset Shelf contexts */
    if (strstr(block_name, "asset") || 
        strstr(block_name, "Asset") ||
        strstr(block_name, "shelf") ||
        strstr(block_name, "Shelf") ||
        strstr(block_name, "ASSET_SHELF") ||
        strstr(block_name, "AssetShelf")) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Asset Shelf context detected - DENIED\n");
      }
      return false;
    }
    
    /* Use intelligent slot detection to determine if this is a valid drop target */
    if (brush) {
      bool use_mask_slot = false;
      if (determine_texture_slot_type(but, brush, &use_mask_slot, C)) {
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Intelligent detection - valid %s slot\n", 
                 use_mask_slot ? "mask" : "main");
        }
        return true;
      }
    }
    
    /* ENHANCED: Allow texture slot buttons in brush texture panels even without RNA properties */
    if (strstr(block_name, "brush_texture") || 
        strstr(block_name, "brush_mask") ||
        strstr(block_name, "texture_slot") ||
        strstr(block_name, "VIEW3D_PT_tools_brush_texture") ||
        strstr(block_name, "IMAGE_PT") ||
        strstr(block_name, "PROPERTIES_PT") ||
        strstr(block_name, "texture") ||
        strstr(block_name, "Texture")) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Allowing button in texture context without RNA property\n");
      }
      return true;
    }
  }
    
    return false;
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Starting validation for button with RNA property\n");
  }

  /* Use intelligent slot detection for buttons with RNA properties */
  if (brush) {
    bool use_mask_slot = false;
    if (determine_texture_slot_type(but, brush, &use_mask_slot, C)) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Intelligent detection - valid %s slot with RNA\n", 
               use_mask_slot ? "mask" : "main");
      }
      
      /* Additional validation: ensure this is actually a texture-related property */
      const char *prop_id = RNA_property_identifier(but->rnaprop);
      if (prop_id && (STREQ(prop_id, "texture") || 
                     STREQ(prop_id, "mask_texture") ||
                     STREQ(prop_id, "brush_texture") ||
                     STREQ(prop_id, "brush_mask_texture") ||
                     STREQ(prop_id, "image") ||
                     strstr(prop_id, "texture") != nullptr ||
                     strstr(prop_id, "mask") != nullptr)) {
        return true;
      }
      
      /* Check RNA property type - must be PROP_POINTER for texture slots */
      if (RNA_property_type(but->rnaprop) == PROP_POINTER) {
        return true;
      }
    }
  }

  /* FALLBACK: Original validation logic for cases where intelligent detection fails */
  bool is_texture_slot = false;
  
  /* Check RNA property - must be texture property */
  const char *prop_id = RNA_property_identifier(but->rnaprop);
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Property ID: %s\n", prop_id ? prop_id : "NULL");
  }
  
  /* CRITICAL: Check RNA property type - must be PROP_POINTER for texture slots */
  if (RNA_property_type(but->rnaprop) != PROP_POINTER) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Not a pointer property (type=%d) - DENIED\n", 
             RNA_property_type(but->rnaprop));
    }
    return false;
  }

  /* PRIORITY 1: Check property name directly - most reliable */
  if (prop_id && (STREQ(prop_id, "texture") || 
                 STREQ(prop_id, "mask_texture") ||
                 STREQ(prop_id, "brush_texture") ||
                 STREQ(prop_id, "brush_mask_texture") ||
                 STREQ(prop_id, "image") ||
                 strstr(prop_id, "texture") != nullptr ||
                 strstr(prop_id, "mask") != nullptr)) {
    is_texture_slot = true;
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Valid texture slot by property name: %s\n", prop_id);
    }
  } else if (prop_id) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Property is NOT a texture slot: %s - DENIED\n", prop_id);
    }
  }
  
  /* PRIORITY 2: Check RNA pointer type */
  if (!is_texture_slot && but->rnapoin.type && but->rnapoin.data) {
    const char *type_id = RNA_struct_identifier(but->rnapoin.type);
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: RNA type: %s\n", type_id ? type_id : "NULL");
    }
    
    /* Allow if pointer to texture or BrushTextureSlot */
    if (type_id && (STREQ(type_id, "Texture") || STREQ(type_id, "BrushTextureSlot"))) {
      /* ADDITIONAL CHECK: Verify this is actually a texture slot, not settings */
      if (prop_id && (STREQ(prop_id, "texture") || 
                     STREQ(prop_id, "mask_texture") ||
                     STREQ(prop_id, "brush_texture") ||
                     STREQ(prop_id, "brush_mask_texture") ||
                     STREQ(prop_id, "image") ||
                     strstr(prop_id, "texture") != nullptr ||
                     strstr(prop_id, "mask") != nullptr)) {
        is_texture_slot = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Valid texture slot by RNA type: %s\n", prop_id);
        }
      } else {
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Pointer to texture, but property is NOT a slot: %s - DENIED\n", prop_id);
        }
      }
    }
  }
  
  /* PRIORITY 3: Check by panel name */
  if (!is_texture_slot && but->block && !but->block->name.empty()) {
    const char *block_name = but->block->name.c_str();
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Block name: %s\n", block_name);
    }
    
    /* Allow ONLY blocks that are clearly related to brush texture */
    if (strstr(block_name, "brush_texture") || 
        strstr(block_name, "brush_mask") ||
        strstr(block_name, "texture_slot")) {
      
      /* ADDITIONAL CHECK: Even if button is in correct block, 
         verify this is actually a texture slot, not settings */
      if (prop_id && (STREQ(prop_id, "texture") || 
                     STREQ(prop_id, "mask_texture") ||
                     STREQ(prop_id, "brush_texture") ||
                     STREQ(prop_id, "brush_mask_texture") ||
                     STREQ(prop_id, "image") ||
                     strstr(prop_id, "texture") != nullptr ||
                     strstr(prop_id, "mask") != nullptr)) {
        is_texture_slot = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Valid texture slot by block name: %s\n", prop_id);
        }
      } else {
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Button in texture block, but property is NOT a slot: %s - DENIED\n", prop_id);
        }
      }
    }
  }
  
  /* FALLBACK: If couldn't determine by property, but we're in texture context */
  if (!is_texture_slot && but->block && !but->block->name.empty()) {
    const char *block_name = but->block->name.c_str();
    if (strstr(block_name, "brush") != nullptr || 
        strstr(block_name, "texture") != nullptr ||
        strstr(block_name, "paint") != nullptr) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: FALLBACK - allowing by block context: %s\n", block_name);
      }
      is_texture_slot = true;
    }
  }
  
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_is_valid_brush_texture_property: Final result: %s\n", is_texture_slot ? "ALLOWED" : "DENIED");
  }
  return is_texture_slot;
}

/**
 * Check if area is suitable for texture drop operations.
 * Validates that the current area supports texture drop operations.
 * ALLOWS: Properties Editor, Image Editor Paint, View3D brush panels.
 * DISALLOWS: Asset Shelf regions.
 * 
 * @param C Blender context
 * @param event Mouse event
 * @return true if area supports texture drops
 */
bool DROP_IMAGE_is_texture_drop_area(bContext *C, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return false;
  }
  
  /* CRITICAL: Always disallow Asset Shelf regions */
  if (region->regiontype == RGN_TYPE_ASSET_SHELF) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Asset Shelf region - DISALLOWING\n");
    }
    return false;
  }
  
  /* ALLOW: Properties Editor */
  ScrArea *area = CTX_wm_area(C);
  if (area && area->spacetype == SPACE_PROPERTIES) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Properties Editor - ALLOWING\n");
    }
    return true;
  }
  
  /* ALLOW: Image Editor Paint mode */
  if (area && area->spacetype == SPACE_IMAGE) {
    SpaceImage *sima = CTX_wm_space_image(C);
    if (sima && sima->mode == SI_MODE_PAINT) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Image Editor Paint mode - ALLOWING\n");
      }
      return true;
    }
  }
  
  /* ALLOW: View3D brush texture panels */
  if (region->regiontype == RGN_TYPE_UI && area && area->spacetype == SPACE_VIEW3D) {
    /* Additional check - find button under cursor */
    ui::Button *but = ui::but_find_mouse_over(region, event);
    if (but) {
      /* Check if button is in brush texture context */
      if (DROP_IMAGE_is_valid_brush_texture_property(but, C)) {
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Valid brush texture button found in View3D\n");
        }
        return true;
      }
      
      /* Additional check by block name to ensure we're in brush texture panel */
      if (but->block && !but->block->name.empty()) {
        const char *block_name = but->block->name.c_str();
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Block name: %s\n", block_name);
        }
        
        /* Only allow in specific brush texture panels */
        if (strstr(block_name, "brush_texture") || 
            strstr(block_name, "brush_mask") ||
            strstr(block_name, "texture_slot") ||
            strstr(block_name, "VIEW3D_PT_tools_brush_texture")) {
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Allowed in brush texture panel\n");
          }
          return true;
        }
        
        /* DISALLOW in Asset Shelf contexts */
        if (strstr(block_name, "asset") || 
            strstr(block_name, "Asset") ||
            strstr(block_name, "shelf") ||
            strstr(block_name, "Shelf")) {
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Asset Shelf context detected - DISALLOWING\n");
          }
          return false;
        }
      }
    }
  }
  
  /* ALLOW: Node Editor */
  if (area && area->spacetype == SPACE_NODE) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Node Editor - ALLOWING\n");
    }
    return true;
  }
  
  /* DISALLOW: Main 3D View area (general area) */
  if (region->regiontype == RGN_TYPE_WINDOW) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Main 3D View area - DISALLOWING\n");
    }
    return false;
  }
  
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_is_texture_drop_area: Unknown area - DISALLOWING\n");
  }
  return false;
}

/**
 * Check if texture can be dropped on specific button.
 * Comprehensive validation for texture drop operations.
 * 
 * @param but UI button to check
 * @param context Texture drop context
 * @return true if drop is allowed
 */
bool DROP_IMAGE_can_drop_on_button(const ui::Button *but, const TextureDropContext *context)
{
  if (!but || !context) {
    return false;
  }
  
  /* Check if button is valid texture property */
  if (!DROP_IMAGE_is_valid_brush_texture_property(but, context->C)) {
    return false;
  }
  
  /* Check if we have active brush */
  if (!context->active_brush) {
    return false;
  }
  
  /* Check if brush is editable */
  if (ID_IS_LINKED(&context->active_brush->id)) {
    /* Allow drops on linked brushes if they're assets */
    if (!ID_IS_ASSET(&context->active_brush->id)) {
      return false;
    }
  }
  
  /* Check if we're in appropriate paint mode */
  if (context->paint_mode == TexturePaintMode::NONE) {
    return false;
  }
  
  /* Additional validation based on editor type */
  switch (context->editor_type) {
    case TextureDropEditorType::VIEW3D:
      /* Allow drops in 3D View */
      return true;
      
    case TextureDropEditorType::PROPERTIES:
      /* Allow drops in Properties editor */
      return true;
      
    case TextureDropEditorType::IMAGE_EDITOR:
      /* Allow drops in Image Editor */
      return true;
      
    case TextureDropEditorType::NODE_EDITOR:
      /* Allow drops in Node Editor */
      return true;
      
    case TextureDropEditorType::UNSUPPORTED:
    default:
      return false;
  }
}

/**
 * Validate drag operation for texture drop.
 * Checks if the drag operation contains valid texture data.
 * 
 * @param drag Drag operation data
 * @return true if drag contains valid texture data
 */
bool DROP_IMAGE_validate_drag_data(const wmDrag *drag)
{
  if (!drag) {
    return false;
  }
  
  /* Check drag type */
  if (drag->type == WM_DRAG_PATH) {
    /* Check if it's an image file */
    const char *path = WM_drag_get_single_path(drag);
    if (path && BLI_path_extension_check_array(path, imb_ext_image)) {
      return true;
    }
  }
  else if (drag->type == WM_DRAG_ID) {
    /* Check if it's an image ID */
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      return true;
    }
  }
  
  return false;
}

/**
 * Validate drop context for texture operations.
 * Ensures all necessary context is available for texture drop.
 * 
 * @param context Texture drop context
 * @return true if context is valid for texture operations
 */
bool DROP_IMAGE_validate_drop_context(const TextureDropContext *context)
{
  if (!context) {
    return false;
  }
  
  /* Check basic context */
  if (!context->C) {
    return false;
  }
  
  /* Check editor type */
  if (context->editor_type == TextureDropEditorType::UNSUPPORTED) {
    return false;
  }
  
  /* Check paint mode */
  if (context->paint_mode == TexturePaintMode::NONE) {
    return false;
  }
  
  /* Check active brush */
  if (!context->active_brush) {
    return false;
  }
  
  /* Check region */
  if (!context->region) {
    return false;
  }
  
  return true;
}

/**
 * IMPROVED: Enhanced texture slot type determination based on comprehensive analysis.
 * Analyzes the slot itself, not just the button, with multi-level validation.
 * Based on the approach from code_3.cc example.
 * 
 * @param but UI button being analyzed
 * @param brush Brush containing the texture slots
 * @param r_use_mask_slot Output parameter: true if mask slot should be used
 * @param C Optional context for additional checks
 * @return true if slot type was successfully determined
 */
bool determine_texture_slot_type(const ui::Button *but, const Brush *brush, bool *r_use_mask_slot, bContext *C)
{
  if (!but || !brush || !r_use_mask_slot) {
    return false;
  }
  
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] determine_texture_slot_type: Starting analysis\n");
  }
  *r_use_mask_slot = false;
  
  /* 0. SPECIAL CHECK: If we're in Sculpt Mode, mask slot doesn't exist */
  if (C) {
    const enum eContextObjectMode context_mode = CTX_data_mode_enum(C);
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] determine_texture_slot_type: Context mode: %d\n", context_mode);
    }
    
    /* In Sculpt Mode, brushes don't have separate mask slot */
    if (context_mode == CTX_MODE_SCULPT) {
      *r_use_mask_slot = false;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: SCULPT MODE: Mask slot doesn't exist, using main slot\n");
      }
      return true;
    }
  }
  
  /* 1. PRIORITY: Check by panel name (most important criterion) */
  if (but->block && !but->block->name.empty()) {
    const char *block_name = but->block->name.c_str();
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] determine_texture_slot_type: Panel name: %s\n", block_name);
    }
    
    /* CRITICAL: Explicitly exclude Asset Shelf contexts */
    if (strstr(block_name, "asset") || 
        strstr(block_name, "Asset") ||
        strstr(block_name, "shelf") ||
        strstr(block_name, "Shelf") ||
        strstr(block_name, "ASSET_SHELF") ||
        strstr(block_name, "AssetShelf")) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: Asset Shelf context detected - DENIED\n");
      }
      return false;
    }
    
    /* If panel contains "mask", this is definitely mask slot */
    if (strstr(block_name, "mask") || strstr(block_name, "Mask")) {
      *r_use_mask_slot = true;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: PRIORITY: Mask slot determined by panel name\n");
      }
      return true;
    }
    
    /* If panel contains "brush_texture" (but not mask), this is main slot */
    if (strstr(block_name, "brush_texture") && !strstr(block_name, "mask")) {
      *r_use_mask_slot = false;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: PRIORITY: Main slot determined by panel name\n");
      }
      return true;
    }
    
    /* IMPROVED: Support for Image Editor panels */
    if (strstr(block_name, "IMAGE_PT") || strstr(block_name, "IMAGE_MT")) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: Image Editor panel detected\n");
      }
      
      /* In Image Editor, check context more carefully */
      if (strstr(block_name, "mask") || strstr(block_name, "Mask")) {
        *r_use_mask_slot = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR: Mask slot determined by panel name\n");
        }
        return true;
      }
      
      if (strstr(block_name, "texture") || strstr(block_name, "Texture") ||
          strstr(block_name, "brush") || strstr(block_name, "Brush")) {
        *r_use_mask_slot = false;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR: Main slot determined by panel name\n");
        }
        return true;
      }
      
      /* ENHANCED: For any Image Editor panel, use intelligent slot detection */
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR: Using intelligent slot detection\n");
      }
      
      /* Check if we're in Paint mode and use intelligent slot detection */
      if (C) {
        SpaceImage *sima = CTX_wm_space_image(C);
        if (sima && sima->spacetype == SPACE_IMAGE && sima->mode == SI_MODE_PAINT) {
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR PAINT MODE - using intelligent detection\n");
            printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR: Free assignment to any slot\n");
          }
          return true; // Let the intelligent detection handle it
        }
      }
      
      return true; // Let the intelligent detection handle it
    }
    
    /* ENHANCED: Support for Properties Editor panels */
    if (strstr(block_name, "PROPERTIES_PT") || strstr(block_name, "PROPERTIES_MT")) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: Properties Editor panel detected\n");
      }
      
      /* In Properties Editor, check context more carefully */
      if (strstr(block_name, "mask") || strstr(block_name, "Mask")) {
        *r_use_mask_slot = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: PROPERTIES EDITOR: Mask slot determined by panel name\n");
        }
        return true;
      }
      
      if (strstr(block_name, "texture") || strstr(block_name, "Texture") ||
          strstr(block_name, "brush") || strstr(block_name, "Brush")) {
        *r_use_mask_slot = false;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: PROPERTIES EDITOR: Main slot determined by panel name\n");
        }
        return true;
      }
      
      /* ENHANCED: For any Properties Editor panel, use intelligent slot detection */
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: PROPERTIES EDITOR: Using intelligent slot detection\n");
      }
      return true; // Let the intelligent detection handle it
    }
  }
  
  /* 2. Analyze RNA structure of the slot itself */
  if (but->rnapoin.type && but->rnapoin.data) {
    const char *type_identifier = RNA_struct_identifier(but->rnapoin.type);
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] determine_texture_slot_type: RNA type: %s\n", type_identifier);
    }
    
    /* Check if this is mask slot */
    if (STREQ(type_identifier, "BrushMaskTextureSlot") || 
        STREQ(type_identifier, "MaskTextureSlot") ||
        STREQ(type_identifier, "BrushMaskTexture") ||
        STREQ(type_identifier, "MaskTexture")) {
      *r_use_mask_slot = true;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: Mask slot determined by RNA slot type\n");
      }
      return true;
    }
    
    /* Check if this is main texture slot */
    if (STREQ(type_identifier, "BrushTextureSlot") || 
        STREQ(type_identifier, "MainTextureSlot") ||
        STREQ(type_identifier, "BrushTexture") ||
        STREQ(type_identifier, "MainTexture")) {
      *r_use_mask_slot = false;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: Main slot determined by RNA slot type\n");
      }
      return true;
    }
  }
  
  /* 3. Analyze RNA path to determine slot type */
  if (but->rnaprop) {
    const char *prop_identifier = RNA_property_identifier(but->rnaprop);
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] determine_texture_slot_type: Property: %s\n", prop_identifier);
    }
    
    /* Check property path - if contains "mask", this is mask slot */
    if (prop_identifier) {
      if (strstr(prop_identifier, "mask") || strstr(prop_identifier, "Mask")) {
        *r_use_mask_slot = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: Mask slot determined by property path\n");
        }
        return true;
      }
      
      if (strstr(prop_identifier, "texture") || strstr(prop_identifier, "Texture") ||
          strstr(prop_identifier, "image") || strstr(prop_identifier, "Image")) {
        *r_use_mask_slot = false;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: Main slot determined by property path\n");
        }
        return true;
      }
    }
  }
  
  /* 4. IMPROVED: Enhanced logic by slot occupancy for Image Editor */
  if (C) {
    SpaceImage *sima = CTX_wm_space_image(C);
    if (sima && sima->spacetype == SPACE_IMAGE && sima->mode == SI_MODE_PAINT) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR PAINT MODE - using occupancy logic\n");
      }
      
      /* In Image Editor Paint check which slot is already occupied */
      if (brush->mtex.tex && brush->mtex.tex->ima && !brush->mask_mtex.tex) {
        *r_use_mask_slot = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR: Main slot occupied, suggesting mask\n");
        }
        return true;
      }
      
      if (!brush->mtex.tex && brush->mask_mtex.tex && brush->mask_mtex.tex->ima) {
        *r_use_mask_slot = false;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] determine_texture_slot_type: IMAGE EDITOR: Mask slot occupied, suggesting main\n");
        }
        return true;
      }
    }
  }
  
  /* 5. Smart logic by slot occupancy (fallback) */
  if (brush->mtex.tex && brush->mtex.tex->ima && !brush->mask_mtex.tex) {
    *r_use_mask_slot = true;
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] determine_texture_slot_type: Suggesting mask slot (main already occupied)\n");
    }
    return true;
  }
  
  if (!brush->mtex.tex && brush->mask_mtex.tex && brush->mask_mtex.tex->ima) {
    *r_use_mask_slot = false;
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] determine_texture_slot_type: Suggesting main slot (mask already occupied)\n");
    }
    return true;
  }
  
  /* 6. Fallback: use main slot by default */
  *r_use_mask_slot = false;
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] determine_texture_slot_type: Using main slot by default\n");
  }
  return true;
}

/**
 * IMPROVED: Universal search for active texture slot through context.
 * Based on the approach from code_3.cc example.
 * 
 * @param C Blender context
 * @return PointerRNA to active texture slot or empty if not found
 */
static PointerRNA find_active_texture_slot_from_context(bContext *C)
{
  printf("[DEBUG] find_active_texture_slot_from_context: ENTRY - Function called\n");
  printf("[DEBUG] find_active_texture_slot_from_context: C: %s\n", C ? "NOT NULL" : "NULL");
  
  /* First check if we're in Image Editor */
  printf("[DEBUG] find_active_texture_slot_from_context: About to call CTX_wm_space_image\n");
  SpaceImage *sima = CTX_wm_space_image(C);
  printf("[DEBUG] find_active_texture_slot_from_context: sima: %s\n", sima ? "NOT NULL" : "NULL");
  if (sima && sima->spacetype == SPACE_IMAGE && sima->mode == SI_MODE_PAINT) {
    printf("[DEBUG] find_active_texture_slot_from_context: Image Editor Paint mode detected\n");
    
    /* In Image Editor Paint mode, find active brush through ToolSettings */
    Scene *scene = CTX_data_scene(C);
    if (scene && scene->toolsettings) {
      ToolSettings *ts = scene->toolsettings;
      if (ts->imapaint.paint.brush) {
        Brush *brush = ts->imapaint.paint.brush;
        printf("[DEBUG] find_active_texture_slot_from_context: Found Image Paint brush: %s\n", brush->id.name);
        
        /* Create PointerRNA for brush */
        PointerRNA ptr = RNA_id_pointer_create(&brush->id);
        return ptr;
      } else {
        printf("[DEBUG] find_active_texture_slot_from_context: No Image Paint brush found in ToolSettings\n");
      }
    }
  }
  
  /* Fallback: use standard method through Paint context */
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint) {
    Brush *brush = BKE_paint_brush(paint);
    if (brush && brush->mtex.tex) {
      printf("[DEBUG] find_active_texture_slot_from_context: Found Brush->mtex.tex: %p\n", (void *)brush->mtex.tex);
      PointerRNA ptr = RNA_id_pointer_create(&brush->id);
      return ptr;
    }
  }
  
  /* Can add other search types if needed */
  printf("[DEBUG] No valid texture slot found through context\n");
  PointerRNA null_ptr = {};
  return null_ptr;
}

/**
 * Find active texture slot for brush.
 * IMPROVED: Enhanced logic for determining active slot for different modes.
 * 
 * @param C Blender context
 * @param is_mask_slot Output parameter: true if found slot is mask slot
 * @return Pointer to MTex or nullptr if slot not found
 */
static MTex *find_active_brush_texture_slot(bContext *C, bool *is_mask_slot)
{
  if (is_mask_slot) {
    *is_mask_slot = false;
  }
  
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (!brush) {
    return nullptr;
  }
  
  Object *ob = CTX_data_active_object(C);
  eObjectMode ob_mode = ob ? eObjectMode(ob->mode) : OB_MODE_OBJECT;
  
  /* IMPROVED: Enhanced logic for different modes */
  if (ob_mode == OB_MODE_SCULPT) {
    /* In Sculpt mode, brushes don't have separate mask slot */
    if (brush->mtex.tex) {
      return &brush->mtex;
    }
    /* If main slot is empty, create it */
    return &brush->mtex;
    
  } else if (ob_mode == OB_MODE_TEXTURE_PAINT) {
    /* In Texture Paint mode, check both slots */
    
    /* IMPROVED: Check if there are free slots */
    bool main_slot_occupied = (brush->mtex.tex && brush->mtex.tex->ima);
    bool mask_slot_occupied = (brush->mask_mtex.tex && brush->mask_mtex.tex->ima);
    
    /* If main slot is free, use it */
    if (!main_slot_occupied) {
      return &brush->mtex;
    }
    
    /* If mask slot is free, use it */
    if (!mask_slot_occupied) {
      if (is_mask_slot) *is_mask_slot = true;
      return &brush->mask_mtex;
    }
    
    /* If both slots are occupied, return nullptr for forced selection */
    return nullptr;
    
  } else {
    /* For other modes (Paint, Vertex Paint), use standard logic */
    
    /* SPECIAL CASE: Check if we're in Image Editor Paint mode */
    if (C) {
      SpaceImage *sima = CTX_wm_space_image(C);
      if (sima && sima->spacetype == SPACE_IMAGE && sima->mode == SI_MODE_PAINT) {
        printf("[DEBUG] find_active_brush_texture_slot: IMAGE EDITOR PAINT MODE - special logic\n");
        
        /* In Image Editor Paint mode, always allow free assignment to any slot */
        /* Return nullptr to allow free choice */
        printf("[DEBUG] find_active_brush_texture_slot: IMAGE EDITOR: Free assignment to any slot\n");
        return nullptr;
      }
    }
    
    /* IMPROVED: Check free slots */
    bool main_slot_occupied = (brush->mtex.tex && brush->mtex.tex->ima);
    bool mask_slot_occupied = (brush->mask_mtex.tex && brush->mask_mtex.tex->ima);
    
    /* If main slot is free, use it */
    if (!main_slot_occupied) {
      return &brush->mtex;
    }
    
    /* If mask slot is free, use it */
    if (!mask_slot_occupied) {
      if (is_mask_slot) *is_mask_slot = true;
      return &brush->mask_mtex;
    }
    
    /* If both slots are occupied, return nullptr for forced selection */
    return nullptr;
  }
  
  return nullptr;
}

/**
 * Determine active paint mode type.
 * Returns the type of paint mode currently active.
 * 
 * @param C Blender context
 * @return TexturePaintMode enum value
 */
static TexturePaintMode get_active_paint_mode_type(bContext *C)
{
  if (!C) {
    return TexturePaintMode::NONE;
  }
  
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return TexturePaintMode::NONE;
  }
  
  eObjectMode ob_mode = eObjectMode(ob->mode);
  
  switch (ob_mode) {
    case OB_MODE_TEXTURE_PAINT:
      return TexturePaintMode::TEXTURE_PAINT;
      
    case OB_MODE_SCULPT:
      return TexturePaintMode::SCULPT;
      
    case OB_MODE_VERTEX_PAINT:
      return TexturePaintMode::VERTEX_PAINT;
      
    case OB_MODE_WEIGHT_PAINT:
      return TexturePaintMode::WEIGHT_PAINT;
      
    default:
      return TexturePaintMode::NONE;
  }
}

/**
 * Check if brush is valid for texture operations.
 * Validates that the brush can accept texture assignments.
 * 
 * @param brush Brush to check
 * @param C Blender context
 * @return true if brush is valid for texture operations
 */
static bool is_brush_valid_for_texture(Brush *brush, bContext *C)
{
  if (!brush) {
    return false;
  }
  
  /* Check if brush is linked and not editable */
  if (ID_IS_LINKED(&brush->id) && !ID_IS_ASSET(&brush->id)) {
    return false;
  }
  
  /* Check brush type compatibility */
  TexturePaintMode paint_mode = get_active_paint_mode_type(C);
  
  switch (paint_mode) {
    case TexturePaintMode::TEXTURE_PAINT:
      /* Texture Paint mode - most brushes support textures */
      return true;
      
    case TexturePaintMode::SCULPT:
      /* Sculpt mode - most brushes support textures */
      return true;
      
    case TexturePaintMode::VERTEX_PAINT:
    case TexturePaintMode::WEIGHT_PAINT:
      /* Other modes - check if brush supports textures */
      return (brush->ob_mode & OB_MODE_TEXTURE_PAINT) ||
             (brush->ob_mode & OB_MODE_SCULPT) ||
             (brush->ob_mode & OB_MODE_VERTEX_PAINT) ||
             (brush->ob_mode & OB_MODE_WEIGHT_PAINT);
      
    case TexturePaintMode::NONE:
    default:
      return false;
  }
}

/**
 * Check if texture can be assigned to brush slot.
 * Validates texture compatibility with brush and slot.
 * 
 * @param texture Texture to assign
 * @param brush Brush to assign to
 * @param is_mask_slot Whether this is mask slot
 * @param C Blender context
 * @return true if texture can be assigned
 */
static bool can_assign_texture_to_brush(Tex *texture, Brush *brush, bool is_mask_slot, bContext *C)
{
  if (!texture || !brush) {
    return false;
  }
  
  /* Check if brush is valid for texture operations */
  if (!is_brush_valid_for_texture(brush, C)) {
    return false;
  }
  
  /* Check texture type compatibility */
  if (texture->type == TEX_IMAGE) {
    /* Image textures are always compatible */
    return true;
  }
  
  /* Check if texture has valid image */
  if (texture->ima && GS(texture->ima->id.name) == ID_IM) {
    return true;
  }
  
  /* For mask slots, check if texture is suitable for masking */
  if (is_mask_slot) {
    /* Mask textures should be grayscale or have alpha */
    if (texture->ima) {
      /* Check if image has alpha channel - for now always accept */
      return true;
    }
  }
  
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Universal Texture Drop Functions for template_id_preview
 * \{ */

/**
 * Universal image/texture drop validation for template_id_preview.
 * Works across all editor types and contexts.
 * STRICT: Only allows drops in valid brush texture contexts, NOT in Asset Shelf.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param event Mouse event
 * @return true if drop is allowed on texture slot
 */
bool DROP_IMAGE_texture_slot_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!C || !drag || !event) {
    return false;
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Starting validation\n");
  }

  /* ENHANCED: Initialize drag state if not already active */
  if (!DROP_IMAGE_drag_state_is_active()) {
    /* Find active brush and determine target slot */
    Paint *paint = BKE_paint_get_active_from_context(C);
    if (paint && paint->brush) {
      ARegion *region = CTX_wm_region(C);
      if (region) {
        ui::Button *but = ui::but_find_mouse_over(region, event);
        if (but) {
          bool use_mask_slot = false;
          if (determine_texture_slot_type(but, paint->brush, &use_mask_slot, C)) {
            DROP_IMAGE_drag_state_set_active(paint->brush, use_mask_slot);
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Initialized drag state for brush: %s, mask_slot: %s\n", 
                     paint->brush->id.name + 2, use_mask_slot ? "true" : "false");
            }
          }
        }
      }
    }
  }

  /* CRITICAL: First check if we're in a valid texture drop area */
  if (!DROP_IMAGE_is_texture_drop_area(C, event)) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Not in valid texture drop area (possibly Asset Shelf)\n");
    }
    return false;
  }

  /* Check if drag contains valid image/texture data */
  if (!DROP_IMAGE_validate_drag_data(drag)) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Invalid drag data\n");
    }
    return false;
  }

  /* Find button under cursor */
  ARegion *region = CTX_wm_region(C);
  if (!region) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_poll: No region found\n");
    }
    return false;
  }

  ui::Button *but = ui::but_find_mouse_over(region, event);
  if (!but) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_poll: No button under cursor\n");
    }
    return false;
  }

  /* Check if button is valid texture property */
  if (!DROP_IMAGE_is_valid_brush_texture_property(but, C)) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Button is not a valid texture property\n");
    }
    return false;
  }

  /* Create drop context for validation */
  TextureDropContext context(C);
  if (!DROP_IMAGE_validate_drop_context(&context)) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Invalid drop context\n");
    }
    return false;
  }

  /* Check if drop is allowed on this button */
  bool can_drop = DROP_IMAGE_can_drop_on_button(but, &context);
  
  /* ENHANCED: Update drag state with validation result */
  if (DROP_IMAGE_drag_state_is_active()) {
    g_texture_drag_state.over_valid_target = can_drop;
    if (can_drop) {
      g_texture_drag_state.highlight_alpha = 1.0f; // Full highlight for valid target
    } else {
      g_texture_drag_state.highlight_alpha = 0.3f; // Dim highlight for invalid target
    }
  }
  
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_texture_slot_poll: Can drop = %s\n", can_drop ? "true" : "false");
  }

  return can_drop;
}

/**
 * Check if texture slot is occupied and determine the best action.
 * Enhanced version that also checks if the same image is already assigned.
 * 
 * @param brush Brush to check
 * @param use_mask_slot Whether to check mask slot or main slot
 * @param drag_image Image being dragged (optional, for duplicate detection)
 * @param drag_filepath File path being dragged (optional, for duplicate detection)
 * @param r_should_replace Output: true if should replace existing texture
 * @param r_is_same_image Output: true if same image is already assigned
 * @return true if slot has existing texture
 */
static bool check_texture_slot_occupancy(Brush *brush, bool use_mask_slot, Image *drag_image, 
                                        const char *drag_filepath, bool *r_should_replace, bool *r_is_same_image)
{
  if (!brush || !r_should_replace) {
    return false;
  }
  
  *r_should_replace = false;
  if (r_is_same_image) {
    *r_is_same_image = false;
  }
  
  MTex *mtex = use_mask_slot ? &brush->mask_mtex : &brush->mtex;
  
  /* Check if slot is occupied */
  bool slot_occupied = (mtex->tex && mtex->tex->ima);
  
  if (slot_occupied) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] check_texture_slot_occupancy: %s slot is occupied with texture: %s\n",
             use_mask_slot ? "Mask" : "Main", mtex->tex->id.name);
    }
    
    /* Check if the same image is already assigned */
    bool is_same = false;
    
    if (drag_image && mtex->tex->ima == drag_image) {
      /* Direct pointer comparison for ID-based drags */
      is_same = true;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] check_texture_slot_occupancy: Same image already assigned (pointer match)\n");
      }
    } else if (drag_filepath && mtex->tex->ima && mtex->tex->ima->filepath) {
      /* File path comparison for file-based drags */
      if (BLI_path_cmp_normalized(drag_filepath, mtex->tex->ima->filepath) == 0) {
        is_same = true;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] check_texture_slot_occupancy: Same image already assigned (filepath match)\n");
        }
      }
    }
    
    if (is_same) {
      if (r_is_same_image) {
        *r_is_same_image = true;
      }
      *r_should_replace = false;
    } else {
      /* STRATEGY: For occupied slots with different images, prefer to replace existing texture
       * rather than create new one (less clutter in texture list) */
      *r_should_replace = true;
    }
  } else {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] check_texture_slot_occupancy: %s slot is empty\n",
             use_mask_slot ? "Mask" : "Main");
    }
  }
  
  return slot_occupied;
}

/**
 * Universal copy function for image/texture drops on template_id_preview.
 * Intelligently handles texture assignment vs creation based on slot occupancy.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param drop Drop box data
 */
void DROP_IMAGE_texture_slot_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  if (!C || !drag || !drop) {
    return;
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Starting copy operation\n");
  }

  /* ENHANCED: Update drag state during copy operation */
  bool drag_was_active = DROP_IMAGE_drag_state_is_active();
  if (drag_was_active) {
    g_texture_drag_state.highlight_alpha = 1.0f; // Full highlight during operation
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Using active drag state\n");
    }
  }

  /* Get image from drag data */
  ID *id = nullptr;
  if (drag->type == WM_DRAG_PATH) {
    /* For file paths, we'll need to load the image */
    const char *filepath = WM_drag_get_single_path(drag);
    if (filepath) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Loading image from path: %s\n", filepath);
      }
      RNA_string_set(drop->ptr, "filepath", filepath);
    }
  }
  else if (drag->type == WM_DRAG_ID) {
    /* For existing Image IDs */
    id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Using existing Image ID: %s\n", id->name);
      }
      RNA_int_set(drop->ptr, "session_uid", int(id->session_uid));
    }
  }

  /* Store drop context for operator */
  TextureDropContext context(C);
  
  /* Store target button context and determine slot type */
  bool use_mask_slot = false;
  ARegion *region = CTX_wm_region(C);
  const wmEvent *event = CTX_wm_window(C)->runtime->eventstate;
  if (region && event) {
    ui::Button *but = ui::but_find_mouse_over(region, event);
    if (but) {
      /* Try to get property name if available */
      if (but->rnaprop) {
        const char *prop_id = RNA_property_identifier(but->rnaprop);
        if (prop_id) {
          RNA_string_set(drop->ptr, "property_name", prop_id);
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Target property: %s\n", prop_id);
          }
        }
      }
      
      /* ENHANCED: Use drag state information if available */
      if (drag_was_active && g_texture_drag_state.active_brush == context.active_brush) {
        use_mask_slot = g_texture_drag_state.is_mask_target;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Using drag state slot info - mask_slot: %s\n", 
                 use_mask_slot ? "true" : "false");
        }
      }
      /* Fallback: Determine if this is mask slot - works for all button types */
      else if (context.active_brush) {
        if (determine_texture_slot_type(but, context.active_brush, &use_mask_slot, C)) {
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Determined slot type - mask_slot: %s\n", 
                   use_mask_slot ? "true" : "false");
          }
        }
      }
      
      RNA_boolean_set(drop->ptr, "use_mask_slot", use_mask_slot);
    }
  }

  /* SMART OPERATOR SELECTION: Choose the best operator based on slot occupancy */
  if (context.active_brush) {
    bool should_replace = false;
    bool is_same_image = false;
    
    /* Get the image being dragged for duplicate detection */
    Image *drag_image = nullptr;
    const char *drag_filepath = nullptr;
    
    if (drag->type == WM_DRAG_ID) {
      ID *id = WM_drag_get_local_ID(drag, ID_IM);
      if (id) {
        drag_image = (Image *)id;
      }
    } else if (drag->type == WM_DRAG_PATH) {
      drag_filepath = WM_drag_get_single_path(drag);
    }
    
    bool slot_occupied = check_texture_slot_occupancy(context.active_brush, use_mask_slot, 
                                                     drag_image, drag_filepath, &should_replace, &is_same_image);
    
    if (is_same_image) {
      /* Same image already assigned - no action needed */
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Same image already assigned, skipping operation\n");
      }
      
      /* ENHANCED: Clear drag state since operation is complete */
      if (drag_was_active) {
        DROP_IMAGE_drag_state_clear();
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Cleared drag state (same image)\n");
        }
      }
      return;
      
    } else if (slot_occupied && should_replace) {
      /* Slot is occupied with different image - use assign operator to replace existing texture */
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Using TEXTURE_OT_assign_image (replace existing)\n");
      }
      /* Drop->ptr already points to TEXTURE_OT_assign_image from registration */
      
      /* Add special flag to indicate replacement */
      RNA_boolean_set(drop->ptr, "replace_existing", true);
      
    } else {
      /* Slot is empty or user prefers new texture - we could switch to new operator
       * But for simplicity, TEXTURE_OT_assign_image handles both cases */
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Using TEXTURE_OT_assign_image (assign to empty or existing)\n");
      }
      
      /* Add flag to indicate this is not a replacement */
      RNA_boolean_set(drop->ptr, "replace_existing", false);
    }
  }

  /* Note: editor_type and paint_mode are not needed for TEXTURE_OT_assign_image */
  
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_texture_slot_copy: Copy operation completed\n");
  }
}

/**
 * Universal tooltip generator for image/texture drops on template_id_preview.
 * Provides context-aware tooltips based on editor type, target slot, and occupancy.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param xy Mouse coordinates
 * @param drop Drop box data
 * @return Generated tooltip string
 */
std::string DROP_IMAGE_texture_slot_tooltip(bContext *C, wmDrag *drag, const int xy[2], wmDropBox *drop)
{
  UNUSED_VARS(xy, drop);
  if (!C || !drag) {
    return std::string("Invalid drop data");
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Generating smart tooltip\n");
  }

  /* Create drop context for analysis */
  TextureDropContext context(C);
  
  /* Get target slot information */
  bool use_mask_slot = false;
  bool slot_occupied = false;
  std::string slot_name = "texture";
  
  ARegion *region = CTX_wm_region(C);
  const wmEvent *event = CTX_wm_window(C)->runtime->eventstate;
  if (region && event) {
    ui::Button *but = ui::but_find_mouse_over(region, event);
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Found button: %p, has rnaprop: %s, active_brush: %p\n",
             but, (but && but->rnaprop) ? "yes" : "no", context.active_brush);
    }
    
    if (but && context.active_brush) {
      /* ENHANCED: Always determine current slot type in real-time */
      bool current_slot_is_mask = false;
      bool slot_type_determined = determine_texture_slot_type(but, context.active_brush, &current_slot_is_mask, C);
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Real-time slot detection - mask_slot: %s, determined: %s\n", 
               current_slot_is_mask ? "yes" : "no", slot_type_determined ? "yes" : "no");
      }
      
      /* Use drag state information if available, but update it with current slot */
      bool drag_is_active = DROP_IMAGE_drag_state_is_active();
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Drag state active: %s\n", drag_is_active ? "yes" : "no");
      }
      
      if (drag_is_active && g_texture_drag_state.active_brush == context.active_brush && slot_type_determined) {
        /* UPDATE: Use current slot type instead of cached drag state */
        use_mask_slot = current_slot_is_mask;
        
        /* Update drag state with current slot information */
        if (g_texture_drag_state.is_mask_target != current_slot_is_mask) {
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Updating drag state - old mask_slot: %s, new mask_slot: %s\n", 
                   g_texture_drag_state.is_mask_target ? "yes" : "no", current_slot_is_mask ? "yes" : "no");
          }
          g_texture_drag_state.is_mask_target = current_slot_is_mask;
        }
        
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using updated drag state, mask_slot: %s\n", use_mask_slot ? "yes" : "no");
        }
        
        /* Get proper slot name from RNA */
        if (use_mask_slot) {
          PointerRNA brush_ptr = RNA_pointer_create_discrete(nullptr, RNA_Brush, context.active_brush);
          PropertyRNA *mask_tex_prop = RNA_struct_find_property(&brush_ptr, "mask_texture");
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Looking for mask_texture property: %p\n", (void*)mask_tex_prop);
          }
          if (mask_tex_prop) {
            const char *ui_name = RNA_property_ui_name(mask_tex_prop);
            slot_name = ui_name;
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Got RNA UI name for mask texture: '%s'\n", ui_name);
            }
          } else {
            slot_name = "mask texture";
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using fallback name for mask texture\n");
            }
          }
        } else {
          PointerRNA brush_ptr = RNA_pointer_create_discrete(nullptr, RNA_Brush, context.active_brush);
          PropertyRNA *tex_prop = RNA_struct_find_property(&brush_ptr, "texture");
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Looking for texture property: %p\n", (void*)tex_prop);
          }
          if (tex_prop) {
            const char *ui_name = RNA_property_ui_name(tex_prop);
            slot_name = ui_name;
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Got RNA UI name for main texture: '%s'\n", ui_name);
            }
          } else {
            slot_name = "main texture";
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using fallback name for main texture\n");
            }
          }
        }
        
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using updated drag state slot info\n");
        }
      }
      /* Fallback: Use determined slot type */
      else if (slot_type_determined) {
        use_mask_slot = current_slot_is_mask;
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using fallback slot type, mask_slot: %s\n", use_mask_slot ? "yes" : "no");
        }
        
        /* Get proper slot name from RNA */
        if (use_mask_slot) {
          PointerRNA brush_ptr = RNA_pointer_create_discrete(nullptr, RNA_Brush, context.active_brush);
          PropertyRNA *mask_tex_prop = RNA_struct_find_property(&brush_ptr, "mask_texture");
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Looking for mask_texture property: %p\n", (void*)mask_tex_prop);
          }
          if (mask_tex_prop) {
            const char *ui_name = RNA_property_ui_name(mask_tex_prop);
            slot_name = ui_name;
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Got RNA UI name for mask texture: '%s'\n", ui_name);
            }
          } else {
            slot_name = "mask texture";
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using fallback name for mask texture\n");
            }
          }
        } else {
          PointerRNA brush_ptr = RNA_pointer_create_discrete(nullptr, RNA_Brush, context.active_brush);
          PropertyRNA *tex_prop = RNA_struct_find_property(&brush_ptr, "texture");
          if (g_drop_image_debug_enabled) {
            printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Looking for texture property: %p\n", (void*)tex_prop);
          }
          if (tex_prop) {
            const char *ui_name = RNA_property_ui_name(tex_prop);
            slot_name = ui_name;
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Got RNA UI name for main texture: '%s'\n", ui_name);
            }
          } else {
            slot_name = "main texture";
            if (g_drop_image_debug_enabled) {
              printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Using fallback name for main texture\n");
            }
          }
        }
        
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Determined slot type\n");
        }
      } else {
        if (g_drop_image_debug_enabled) {
          printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Could not determine slot type\n");
        }
      }
        
      /* Note: Slot occupancy will be checked later with actual drag data */
    } else {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Missing button or active brush\n");
      }
    }
  } else {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Missing region or event\n");
    }
  }
  
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: Final slot_name: '%s'\n", slot_name.c_str());
  }
  
  /* Get drag image information for occupancy check */
  Image *drag_image = nullptr;
  const char *drag_filepath = nullptr;
  std::string image_name = "image";
  
  if (drag->type == WM_DRAG_ID) {
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      drag_image = (Image *)id;
      image_name = std::string(id->name + 2); // Skip ID prefix
    }
  } else if (drag->type == WM_DRAG_PATH) {
    drag_filepath = WM_drag_get_single_path(drag);
    if (drag_filepath) {
      const char *filename = BLI_path_basename(drag_filepath);
      if (filename) {
        image_name = std::string(filename);
      }
    }
  }
  
  /* Check if slot is occupied with correct drag data */
  if (context.active_brush) {
    bool should_replace = false;
    bool is_same_image = false;
    slot_occupied = check_texture_slot_occupancy(context.active_brush, use_mask_slot, 
                                                drag_image, drag_filepath, &should_replace, &is_same_image);
    
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_texture_slot_tooltip: slot_occupied=%s, should_replace=%s, is_same_image=%s\n",
             slot_occupied ? "true" : "false", should_replace ? "true" : "false", is_same_image ? "true" : "false");
    }
  }
  
  /* ENHANCED: Add drag state information to tooltip */
  std::string drag_status = "";
  if (DROP_IMAGE_drag_state_is_active()) {
    if (g_texture_drag_state.over_valid_target) {
      drag_status = "  ✓";
    } else {
      drag_status = " (invalid target)";
    }
  }
  
  /* Generate context-aware tooltip */
  if (slot_occupied) {
    return fmt::format("Replace {} slot with {}{}", slot_name, image_name, drag_status);
  } else {
    return fmt::format("Assign {} to {} slot{}", image_name, slot_name, drag_status);
  }
}

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */


/**
 * Universal function for loading image preview for drag operations.
 * Handles both file paths and Blender ID objects.
 * 
 * @param drag Pointer to wmDrag structure
 * @param max_size Maximum size for the preview (default 128px)
 * @return ImBuf containing the scaled preview, or nullptr if failed
 */


/**
 * Sets image preview for drag operation.
 * Loads and scales image preview, then attaches it to drag operation.
 * 
 * @param drag Pointer to wmDrag structure
 * @param max_size Maximum size for the preview (default 128px)
 * @return true if preview was successfully set, false otherwise
 */
static bool DROP_IMAGE_set_preview_for_drag(wmDrag *drag, int max_size)
{
  if (!drag) {
    return false;
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: Setting preview for drag operation\n");
  }

  /* Load image preview using functions from interface_drop_image_feedback.cc */
  ImBuf *preview_imb = nullptr;
  
  if (drag->type == WM_DRAG_PATH) {
    /* For image files */
    const char *filepath = WM_drag_get_single_path(drag);
    if (filepath) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: Processing file: %s\n", filepath);
      }
      preview_imb = DROP_IMAGE_load_and_scale_preview(filepath, max_size);
    }
  }
  else if (drag->type == WM_DRAG_ID) {
    /* For ID images */
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      Image *image = (Image *)id;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: Processing Image ID: %s\n", id->name);
      }
      preview_imb = DROP_IMAGE_load_and_scale_preview_from_id(image, max_size);
    }
  }
  
  if (!preview_imb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: Failed to load preview\n");
    }
    return false;
  }

  /* Set preview for drag operation */
  WM_event_drag_image(drag, preview_imb, 1.0f);
  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: Preview successfully set (size: %dx%d)\n", 
           preview_imb->x, preview_imb->y);
    
    /* Check that preview is actually set */
    printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: Verification - drag->imb: %p, drag->imbuf_scale: %.2f\n", 
           drag->imb, drag->imbuf_scale);
    
    /* Additional check - ensure preview won't be overwritten */
    if (drag->imb != preview_imb) {
      printf("[DEBUG] DROP_IMAGE_set_preview_for_drag: ERROR! Preview was overwritten!\n");
    }
  }
  
  /* Принудительно обновляем отображение */
  /* Note: We can't access context here, so we'll skip the refresh for now */

  return true;
}

/**
 * Callback function for drag start event.
 * Sets up image preview for drag operations when drag starts.
 * This is called by the dropbox system when drag operation begins.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 */
static void DROP_IMAGE_drag_start_callback(bContext *C, wmDrag *drag)
{
  if (!C || !drag) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_drag_start_callback: Invalid parameters\n");
    }
    return;
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_drag_start_callback: Drag start callback called for type: %d\n", drag->type);
    printf("[DEBUG] DROP_IMAGE_drag_start_callback: Current drag->imb: %p\n", drag->imb);
  }

  /* Only set preview if it's not already set */
  if (drag->imb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_drag_start_callback: Preview already set, skipping\n");
    }
    return;
  }

  /* Check if this is an image/texture drag operation */
  if (drag->type == WM_DRAG_PATH) {
    const char *path = WM_drag_get_single_path(drag);
    if (path && BLI_path_extension_check_array(path, imb_ext_image)) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_drag_start_callback: Setting preview for image file: %s\n", path);
      }
      DROP_IMAGE_set_preview_for_drag(drag, 128);
    }
  }
  else if (drag->type == WM_DRAG_ID) {
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_drag_start_callback: Setting preview for Image ID: %s\n", id->name);
      }
      DROP_IMAGE_set_preview_for_drag(drag, 128);
    }
  }
  else {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_drag_start_callback: Unsupported drag type: %d\n", drag->type);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Drop Registration
 * \{ */

/**
 * Register texture drop boxes for all supported editors.
 * Sets up drop handlers for View3D, Properties, Image Editor, and Node Editor.
 * STRICT: Excludes Asset Shelf regions to prevent invalid texture drops.
 */
void DROP_IMAGE_register_dropboxes()
{
  /* Only register if experimental flag is enabled */
  if (!USE_DND_TEXTURE()) {
    return;
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_register_dropboxes: Registering universal texture dropboxes (excluding Asset Shelf)\n");
  }

  /* Register for "User Interface" dropbox map - this covers all UI elements */
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find("User Interface", SPACE_EMPTY, RGN_TYPE_WINDOW);
  
  if (lb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_register_dropboxes: Adding texture slot dropbox to User Interface\n");
    }
    WM_dropbox_add(lb,
                   "TEXTURE_OT_assign_image", // Use created operator for texture assignment
                   DROP_IMAGE_texture_slot_poll,
                   DROP_IMAGE_texture_slot_copy,
                   WM_drag_free_imported_drag_ID,
                   DROP_IMAGE_texture_slot_tooltip);
    
    /* Set drag start callback for preview */
    wmDropBox *drop = static_cast<wmDropBox *>(lb->last);
    if (drop) {
      drop->on_drag_start = DROP_IMAGE_drag_start_callback;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_register_dropboxes: Set drag start callback for User Interface\n");
      }
    }
  }

  /* Additional registrations for specific editors for better integration */
  
  /* 1. View3D Editor */
  lb = WM_dropboxmap_find("View3D", SPACE_VIEW3D, RGN_TYPE_UI);
  if (lb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_register_dropboxes: Adding texture slot dropbox to View3D UI region\n");
    }
    WM_dropbox_add(lb,
                   "TEXTURE_OT_assign_image",
                   DROP_IMAGE_texture_slot_poll,
                   DROP_IMAGE_texture_slot_copy,
                   WM_drag_free_imported_drag_ID,
                   DROP_IMAGE_texture_slot_tooltip);
    
    /* Set drag start callback for preview */
    wmDropBox *drop = static_cast<wmDropBox *>(lb->last);
    if (drop) {
      drop->on_drag_start = DROP_IMAGE_drag_start_callback;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_register_dropboxes: Set drag start callback for View3D\n");
      }
    }
  }

  /* 2. Properties Editor */
  lb = WM_dropboxmap_find("Properties", SPACE_PROPERTIES, RGN_TYPE_WINDOW);
  if (lb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_register_dropboxes: Adding texture slot dropbox to Properties\n");
    }
    WM_dropbox_add(lb,
                   "TEXTURE_OT_assign_image",
                   DROP_IMAGE_texture_slot_poll,
                   DROP_IMAGE_texture_slot_copy,
                   WM_drag_free_imported_drag_ID,
                   DROP_IMAGE_texture_slot_tooltip);
    
    /* Set drag start callback for preview */
    wmDropBox *drop = static_cast<wmDropBox *>(lb->last);
    if (drop) {
      drop->on_drag_start = DROP_IMAGE_drag_start_callback;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_register_dropboxes: Set drag start callback for Properties\n");
      }
    }
  }

  /* 3. Image Editor */
  lb = WM_dropboxmap_find("Image", SPACE_IMAGE, RGN_TYPE_UI);
  if (lb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_register_dropboxes: Adding texture slot dropbox to Image Editor UI\n");
    }
    WM_dropbox_add(lb,
                   "TEXTURE_OT_assign_image",
                   DROP_IMAGE_texture_slot_poll,
                   DROP_IMAGE_texture_slot_copy,
                   WM_drag_free_imported_drag_ID,
                   DROP_IMAGE_texture_slot_tooltip);
    
    /* Set drag start callback for preview */
    wmDropBox *drop = static_cast<wmDropBox *>(lb->last);
    if (drop) {
      drop->on_drag_start = DROP_IMAGE_drag_start_callback;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_register_dropboxes: Set drag start callback for Image Editor\n");
      }
    }
  }

  /* 4. Node Editor */
  lb = WM_dropboxmap_find("Node Editor", SPACE_NODE, RGN_TYPE_UI);
  if (lb) {
    if (g_drop_image_debug_enabled) {
      printf("[DEBUG] DROP_IMAGE_register_dropboxes: Adding texture slot dropbox to Node Editor UI\n");
    }
    WM_dropbox_add(lb,
                   "TEXTURE_OT_assign_image",
                   DROP_IMAGE_texture_slot_poll,
                   DROP_IMAGE_texture_slot_copy,
                   WM_drag_free_imported_drag_ID,
                   DROP_IMAGE_texture_slot_tooltip);
    
    /* Set drag start callback for preview */
    wmDropBox *drop = static_cast<wmDropBox *>(lb->last);
    if (drop) {
      drop->on_drag_start = DROP_IMAGE_drag_start_callback;
      if (g_drop_image_debug_enabled) {
        printf("[DEBUG] DROP_IMAGE_register_dropboxes: Set drag start callback for Node Editor\n");
      }
    }
  }

  if (g_drop_image_debug_enabled) {
    printf("[DEBUG] DROP_IMAGE_register_dropboxes: Universal texture dropbox registration completed\n");
  }
}

}