/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Texture drop functionality for UI elements.
 * Handles drag and drop operations for textures and images onto UI buttons.
 */

#pragma once

#include <string>

#include "DNA_space_enums.h"

namespace blender {

struct ARegion;
struct Brush;
struct Image;
struct ImBuf;
struct Main;
struct Object;
struct PointerRNA;
struct Tex;
struct bContext;
namespace ui { struct Button; }
struct wmDrag;
struct wmDropBox;
struct wmEvent;
struct wmTimer;

/**
 * Supported editor types for texture drop operations.
 */
enum class TextureDropEditorType {
  VIEW3D = 0,
  PROPERTIES = 1,
  IMAGE_EDITOR = 2,
  NODE_EDITOR = 3,
  UNSUPPORTED = 4
};

/**
 * Paint modes that support texture drops.
 */
enum class TexturePaintMode {
  NONE = 0,
  TEXTURE_PAINT = 1,
  SCULPT = 2,
  VERTEX_PAINT = 3,
  WEIGHT_PAINT = 4
};

/**
 * Types of texture slots that can receive drops.
 */
enum class TextureSlotType {
  MAIN_TEXTURE = 0,
  MASK_TEXTURE = 1
};

/**
 * Context structure for texture drop operations.
 * Contains all necessary information for validating and executing drops.
 */
struct TextureDropContext {
  bContext *C;
  TextureDropEditorType editor_type;
  TexturePaintMode paint_mode;
  ARegion *region;
  ui::Button *target_button;
  TextureSlotType slot_type;
  Brush *active_brush;
  Object *active_object;
  
  TextureDropContext(bContext *context);
};

/**
 * State information for active texture drag operations.
 * Tracks the current state of drag operations for texture slots.
 */
struct TextureDragState {
  /** Flag indicating if drag operation is currently active */
  bool is_active{false};
  /** Pointer to active brush during drag operation */
  Brush *active_brush{nullptr};
  /** Alpha value for highlight animation during drag */
  float highlight_alpha{0.0f};
  /** Flag indicating if cursor is over a valid drop target */
  bool over_valid_target{false};
  /** Timer for highlight animation */
  struct wmTimer *highlight_timer{nullptr};
  /** Flag indicating if drag target is mask slot */
  bool is_mask_target{false};
  
  /** Initialize drag state */
  void init();
  /** Clear drag state */
  void clear();
  /** Check if drag state is active */
  bool is_drag_active() const;
};

/**
 * Data about the dragged texture and operation context.
 * Contains information about what is being dragged and operation parameters.
 */
struct TextureDragData {
  /** Mouse coordinates during drag operation */
  int mval[2]{};
  /** Active brush during drag operation */
  Brush *active_brush{nullptr};
  /** Editor type where drag started */
  int space_type{-1};
  /** Properties editor context */
  int properties_context{-1};
  /** Flag indicating drag operation is in progress */
  bool is_dragging{false};
  /** Alpha value for highlight animation */
  float highlight_alpha{0.0f};
  /** Flag indicating cursor is over valid target */
  bool is_over_valid_target{false};
  /** File path if dragging file */
  char filepath[FILE_MAX]{};
  /** Flag indicating drag started from template ID */
  bool from_template_id{false};
  
  /** Set drag data from context and event */
  void set_from_context(bContext *C, const wmEvent *event);
  /** Clear drag data */
  void clear();
};

/**
 * Loads and scales an image preview for drag operations.
 * Creates a thumbnail-sized preview image for drag feedback.
 * 
 * @param filepath Path to the image file
 * @param max_size Maximum size for the preview (default 128px)
 * @return ImBuf containing the scaled preview, or nullptr if failed
 */
ImBuf *DROP_IMAGE_load_and_scale_preview(const char *filepath, int max_size = 128);

/**
 * Loads and scales an image preview from Blender ID object for drag operations.
 * Creates a thumbnail-sized preview image for drag feedback from existing Image ID.
 * 
 * @param image Pointer to Image ID object
 * @param max_size Maximum size for the preview (default 128px)
 * @return ImBuf containing the scaled preview, or nullptr if failed
 */
ImBuf *DROP_IMAGE_load_and_scale_preview_from_id(struct Image *image, int max_size = 128);

/**
 * Universal function for loading image preview for drag operations.
 * Handles both file paths and Blender ID objects.
 * 
 * @param drag Pointer to wmDrag structure
 * @param max_size Maximum size for the preview (default 128px)
 * @return ImBuf containing the scaled preview, or nullptr if failed
 */
ImBuf *DROP_IMAGE_load_preview_for_drag(wmDrag *drag, int max_size = 128);

/**
 * Sets image preview for drag operation.
 * Loads and scales image preview, then attaches it to drag operation.
 * 
 * @param drag Pointer to wmDrag structure
 * @param max_size Maximum size for the preview (default 128px)
 * @return true if preview was successfully set, false otherwise
 */
bool DROP_IMAGE_set_preview_for_drag(wmDrag *drag, int max_size = 128);

/* Forward declarations for preview functions - implemented in interface_drop_image_preview.cc */
void DROP_IMAGE_update_texture_preview(bContext *C, Main *bmain, Tex *tex, bool force_update = false);
void DROP_IMAGE_update_texture_paint_preview(bContext *C, Main *bmain, Tex *tex, Brush *brush = nullptr);
void DROP_IMAGE_update_texture_preview_smart(bContext *C, Main *bmain, Tex *tex, bool force_update = false);

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
std::string DROP_IMAGE_drop_tooltip(bContext *C, wmDrag *drag, const int xy[2], wmDropBox *drop);

/* Internal tooltip generation functions */
std::string DROP_IMAGE_tooltip_node_editor(bContext *C, const char *filename);
std::string DROP_IMAGE_tooltip_view3d(bContext *C, const char *filename, const char *op_idname);
std::string DROP_IMAGE_tooltip_properties(bContext *C, const char *filename, const char *op_idname);
std::string DROP_IMAGE_tooltip_image_editor(bContext *C, const char *filename, const char *op_idname);


/**
 * Enhanced function for determining texture slot type based on RNA structure.
 * Analyzes the slot itself, not just the button.
 * 
 * @param but UI button being analyzed
 * @param brush Brush containing the texture slots
 * @param r_use_mask_slot Output parameter: true if mask slot should be used
 * @param C Optional context for additional checks
 * @return true if slot type was successfully determined
 */
bool determine_texture_slot_type(const ui::Button *but, const Brush *brush, bool *r_use_mask_slot, bContext *C = nullptr);

/* -------------------------------------------------------------------- */
/** \name Texture Drag State Management
 * \{ */

/**
 * Global texture drag state instance.
 * Used to track active drag operations across the interface.
 */
extern TextureDragState g_texture_drag_state;

/**
 * Initialize texture drag state.
 * Resets all state variables to default values.
 */
void DROP_IMAGE_drag_state_init();

/**
 * Clear texture drag state.
 * Cleans up any active timers and resets state.
 */
void DROP_IMAGE_drag_state_clear();

/**
 * Check if texture drag operation is currently active.
 * @return true if drag operation is in progress
 */
bool DROP_IMAGE_drag_state_is_active();

/**
 * Set texture drag state as active.
 * @param brush Active brush for the drag operation
 * @param is_mask_target Whether target is mask slot
 */
void DROP_IMAGE_drag_state_set_active(Brush *brush, bool is_mask_target = false);

/* -------------------------------------------------------------------- */
/** \name Texture Drag Data Management
 * \{ */

/**
 * Create texture drag data from context and event.
 * @param C Blender context
 * @param event Mouse event
 * @return New TextureDragData instance
 */
TextureDragData *DROP_IMAGE_drag_data_create(bContext *C, const wmEvent *event);

/**
 * Free texture drag data.
 * @param data Drag data to free
 */
void DROP_IMAGE_drag_data_free(TextureDragData *data);

/**
 * Set drag data from context and event.
 * @param data Drag data to populate
 * @param C Blender context
 * @param event Mouse event
 */
void DROP_IMAGE_drag_data_set(TextureDragData *data, bContext *C, const wmEvent *event);

/* -------------------------------------------------------------------- */
/** \name Texture Drop Validation
 * \{ */

/**
 * Check if UI button is a valid texture slot for brush.
 * @param but UI button to check
 * @return true if button is valid texture slot
 */
bool DROP_IMAGE_is_valid_brush_texture_property(const ui::Button *but, bContext *C);

/**
 * Check if area is suitable for texture drop operations.
 * @param C Blender context
 * @param event Mouse event
 * @return true if area supports texture drops
 */
bool DROP_IMAGE_is_texture_drop_area(bContext *C, const wmEvent *event);

/**
 * Check if texture can be dropped on specific button.
 * @param but UI button to check
 * @param context Texture drop context
 * @return true if drop is allowed
 */
bool DROP_IMAGE_can_drop_on_button(const ui::Button *but, const TextureDropContext *context);

/* -------------------------------------------------------------------- */
/** \name Texture Drop Operations
 * \{ */

/**
 * Handle texture drop operation.
 * Main function for processing texture drops onto brush slots.
 * @param C Blender context
 * @param drag Drag operation data
 * @param drop Drop box configuration
 * @return true if drop was successful
 */
bool DROP_IMAGE_handle_texture_drop(bContext *C, wmDrag *drag, wmDropBox *drop);

/**
 * Apply texture to brush slot.
 * @param C Blender context
 * @param brush Target brush
 * @param image Image to apply
 * @param slot_type Type of slot (main or mask)
 * @return true if application was successful
 */
bool DROP_IMAGE_apply_texture_to_brush(bContext *C, Brush *brush, Image *image, TextureSlotType slot_type);

/**
 * Clear texture slot.
 * @param C Blender context
 * @param brush Target brush
 * @param slot_type Type of slot to clear
 */
void DROP_IMAGE_clear_texture_slot(bContext *C, Brush *brush, TextureSlotType slot_type);

/* -------------------------------------------------------------------- */
/** \name Texture Drop Registration
 * \{ */

/**
 * Register texture drop boxes for all supported editors.
 * Sets up drop handlers for View3D, Properties, Image Editor, and Node Editor.
 */
void DROP_IMAGE_register_dropboxes();

/**
 * Universal image/texture drop validation for template_id_preview.
 * Works across all editor types and contexts.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param event Mouse event
 * @return true if drop is allowed on texture slot
 */
bool DROP_IMAGE_texture_slot_poll(bContext *C, wmDrag *drag, const wmEvent *event);

/**
 * Universal copy function for image/texture drops on template_id_preview.
 * Handles texture assignment to brush slots and other texture properties.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param drop Drop box data
 */
void DROP_IMAGE_texture_slot_copy(bContext *C, wmDrag *drag, wmDropBox *drop);

/**
 * Universal tooltip generator for image/texture drops on template_id_preview.
 * Provides context-aware tooltips based on editor type and target slot.
 * 
 * @param C Blender context
 * @param drag Drag operation data
 * @param xy Mouse coordinates
 * @param drop Drop box data
 * @return Generated tooltip string
 */
std::string DROP_IMAGE_texture_slot_tooltip(bContext *C, wmDrag *drag, const int xy[2], wmDropBox *drop);

/* -------------------------------------------------------------------- */
/** \name Texture Drop Validation Functions
 * \{ */

/**
 * Check if UI button is a valid texture slot for brush.
 * Validates that the button represents a texture slot that can receive drops.
 * 
 * @param but UI button to check
 * @param C Blender context
 * @return true if button is valid texture slot
 */
bool DROP_IMAGE_is_valid_brush_texture_property(const ui::Button *but, bContext *C);

/**
 * Check if area is suitable for texture drop operations.
 * Validates that the current area supports texture drop operations.
 * 
 * @param C Blender context
 * @param event Mouse event
 * @return true if area supports texture drops
 */
bool DROP_IMAGE_is_texture_drop_area(bContext *C, const wmEvent *event);

/**
 * Check if texture can be dropped on specific button.
 * Comprehensive validation for texture drop operations.
 * 
 * @param but UI button to check
 * @param context Texture drop context
 * @return true if drop is allowed
 */
bool DROP_IMAGE_can_drop_on_button(const ui::Button *but, const TextureDropContext *context);

/**
 * Validate drag operation for texture drop.
 * Checks if the drag operation contains valid texture data.
 * 
 * @param drag Drag operation data
 * @return true if drag contains valid texture data
 */
bool DROP_IMAGE_validate_drag_data(const wmDrag *drag);

/**
 * Validate drop context for texture operations.
 * Ensures all necessary context is available for texture drop.
 * 
 * @param context Texture drop context
 * @return true if context is valid for texture operations
 */
bool DROP_IMAGE_validate_drop_context(const TextureDropContext *context);

/**
 * Determine texture slot type based on RNA structure.
 * Analyzes the slot itself, not just the button.
 * 
 * @param but UI button being analyzed
 * @param brush Brush containing the texture slots
 * @param r_use_mask_slot Output parameter: true if mask slot should be used
 * @param C Optional context for additional checks
 * @return true if slot type was successfully determined
 */
bool determine_texture_slot_type(const ui::Button *but, const Brush *brush, bool *r_use_mask_slot, bContext *C);

/**
 * Universal search for active texture slot through context.
 * Based on the approach from code_3.cc example.
 * 
 * @param C Blender context
 * @return PointerRNA to active texture slot or empty if not found
 */
PointerRNA find_active_texture_slot_from_context(bContext *C);

/** \} */
}