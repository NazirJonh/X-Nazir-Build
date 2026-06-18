/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 * \brief UI integration for brush texture preview system.
 */

#include "BLI_math_vector_types.hh"

struct bContext;
struct uiLayout;
struct PointerRNA;

namespace blender::ed::interface {

/* -------------------------------------------------------------------- */
/** \name UI Template Functions
 * \{ */

/**
 * Create a brush texture preview UI template.
 * 
 * \param layout: UI layout to add the template to
 * \param C: Blender context
 * \param ptr: RNA pointer to the object containing the brush property
 * \param propname: Name of the brush property
 * \param preview_size: Size of the preview widget in pixels
 */
void uiTemplateBrushTexturePreview(uiLayout *layout,
                                  bContext *C,
                                  PointerRNA *ptr,
                                  const char *propname,
                                  int preview_size);

/**
 * Create a compact brush texture preview (64x64 pixels, no controls).
 * 
 * \param layout: UI layout to add the template to
 * \param C: Blender context
 * \param ptr: RNA pointer to the object containing the brush property
 * \param propname: Name of the brush property
 */
void uiTemplateBrushTexturePreviewCompact(uiLayout *layout,
                                         bContext *C,
                                         PointerRNA *ptr,
                                         const char *propname);

/**
 * Create a large brush texture preview (256x256 pixels, full controls).
 * 
 * \param layout: UI layout to add the template to
 * \param C: Blender context
 * \param ptr: RNA pointer to the object containing the brush property
 * \param propname: Name of the brush property
 */
void uiTemplateBrushTexturePreviewLarge(uiLayout *layout,
                                       bContext *C,
                                       PointerRNA *ptr,
                                       const char *propname);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registration Functions
 * \{ */

/**
 * Register brush texture preview UI templates with Blender's UI system.
 * Called during editor initialization.
 */
void ED_brush_texture_preview_ui_register();

/**
 * Unregister brush texture preview UI templates.
 * Called during editor cleanup.
 */
void ED_brush_texture_preview_ui_unregister();

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Template Properties
 * \{ */

/**
 * RNA property definitions for brush texture preview UI templates.
 * These properties control the appearance and behavior of the preview.
 */
struct BrushTexturePreviewUIProperties {
  /** Show pattern overlay highlighting texture elements */
  bool show_pattern_overlay;
  
  /** Show mask overlay for masked areas */
  bool show_mask_overlay;
  
  /** Enable interactive mode (zoom, pan, click sampling) */
  bool interactive_mode;
  
  /** Number of pattern elements to generate */
  int pattern_element_count;
  
  /** Pattern density factor (0.0 - 1.0) */
  float pattern_density;
  
  /** Pattern distribution type */
  int pattern_distribution_type;
  
  /** Random seed for pattern generation */
  int pattern_random_seed;
  
  /** Render blend mode */
  int render_blend_mode;
  
  /** Render opacity (0.0 - 1.0) */
  float render_opacity;
  
  /** Render contrast adjustment (-1.0 - 1.0) */
  float render_contrast;
  
  /** Render brightness adjustment (-1.0 - 1.0) */
  float render_brightness;
};

/**
 * Default values for UI properties.
 */
static const BrushTexturePreviewUIProperties BRUSH_TEXTURE_PREVIEW_UI_DEFAULTS = {
  /* show_pattern_overlay */ true,
  /* show_mask_overlay */ false,
  /* interactive_mode */ false,
  /* pattern_element_count */ 100,
  /* pattern_density */ 0.5f,
  /* pattern_distribution_type */ 0, /* RANDOM */
  /* pattern_random_seed */ 12345,
  /* render_blend_mode */ 0, /* NORMAL */
  /* render_opacity */ 1.0f,
  /* render_contrast */ 0.0f,
  /* render_brightness */ 0.0f,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name UI Event Handling
 * \{ */

/**
 * UI event types for brush texture preview interaction.
 */
enum eBrushTexturePreviewUIEvent {
  /** Mouse movement for panning */
  BRUSH_TEXTURE_UI_EVENT_PAN = 0,
  
  /** Mouse wheel for zooming */
  BRUSH_TEXTURE_UI_EVENT_ZOOM,
  
  /** Mouse click for sampling */
  BRUSH_TEXTURE_UI_EVENT_SAMPLE,
  
  /** Keyboard shortcut for reset view */
  BRUSH_TEXTURE_UI_EVENT_RESET,
  
  /** Toggle overlay visibility */
  BRUSH_TEXTURE_UI_EVENT_TOGGLE_OVERLAY,
  
  /** Refresh preview */
  BRUSH_TEXTURE_UI_EVENT_REFRESH,
};

/**
 * UI interaction state for brush texture preview.
 */
struct BrushTexturePreviewUIState {
  /** Current zoom factor */
  float zoom_factor;
  
  /** Pan offset in UV coordinates */
  float2 pan_offset;
  
  /** Last mouse position for drag operations */
  float2 last_mouse_pos;
  
  /** Whether currently dragging */
  bool is_dragging;
  
  /** Active interaction mode */
  eBrushTexturePreviewUIEvent active_event;
  
  /** Whether preview needs update */
  bool needs_update;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utility Functions
 * \{ */

/**
 * Convert screen coordinates to UV coordinates within preview widget.
 * 
 * \param screen_pos: Screen position in pixels
 * \param widget_rect: Widget rectangle in screen space
 * \param ui_state: Current UI interaction state
 * \return UV coordinates (0.0 - 1.0)
 */
float2 brush_texture_preview_screen_to_uv(const int2 &screen_pos,
                                         const struct rcti *widget_rect,
                                         const BrushTexturePreviewUIState *ui_state);

/**
 * Convert UV coordinates to screen coordinates within preview widget.
 * 
 * \param uv_pos: UV position (0.0 - 1.0)
 * \param widget_rect: Widget rectangle in screen space
 * \param ui_state: Current UI interaction state
 * \return Screen coordinates in pixels
 */
int2 brush_texture_preview_uv_to_screen(const float2 &uv_pos,
                                       const struct rcti *widget_rect,
                                       const BrushTexturePreviewUIState *ui_state);

/**
 * Get optimal preview size based on available UI space.
 * 
 * \param available_width: Available width in pixels
 * \param available_height: Available height in pixels
 * \param aspect_ratio: Desired aspect ratio (width/height)
 * \return Optimal preview size
 */
int2 brush_texture_preview_get_optimal_size(int available_width,
                                           int available_height,
                                           float aspect_ratio = 1.0f);

/**
 * Check if brush texture preview should be updated.
 * 
 * \param brush: Brush to check
 * \param last_update_time: Time of last update
 * \return True if update is needed
 */
bool brush_texture_preview_needs_update(const struct Brush *brush,
                                       double last_update_time);

/** \} */

} // namespace blender::ed::interface