/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved. */
#pragma once

/** \file
 * \ingroup edinterface
 */

#include "BLI_vector.hh"
#include "DNA_vec_types.h" /* for rcti */

namespace blender {
struct ARegion;
struct ARegionType;
struct bContext;
struct CategoryTagDef;
struct wmEvent;
struct wmRegionListenerParams;
struct wmRegionMessageSubscribeParams;
struct wmWindowManager;
struct SpaceProperties;
struct View3D;
}

namespace blender::ui {
struct Layout;
struct Block;
}

namespace blender::ui {

using blender::ARegion;
using blender::bContext;
using blender::wmEvent;
using blender::wmWindowManager;
using blender::View3D;
using blender::wmRegionListenerParams;
using blender::wmRegionMessageSubscribeParams;

/**
 * Data for a single tag button in the tag bar.
 */
struct TagButton {
  char tag_name[64];    /**< Name of the tag */
  char glyph[8];        /**< Glyph character */
  float color[3];       /**< RGB color of the tag */
  int category_count;   /**< Number of categories with this tag */
  rcti rect;            /**< Button rectangle for hover detection */
  bool is_visible;      /**< Whether the button is visible in UI */
  bool is_hovered;      /**< Whether cursor is over the button */
  bool is_active;       /**< Whether the filter is active */
  char _pad[1];         /**< Padding for alignment */
};

/**
 * Runtime data for the tag bar.
 */
struct TagBarRuntimeData {
  blender::Vector<TagButton> buttons;
  int total_width = 0;       /**< Total width of all buttons */
  int visible_buttons = 0;   /**< Number of visible buttons */
  int scroll_offset = 0;     /**< Current scroll offset */
  int max_scroll = 0;        /**< Maximum scroll value */
  bool needs_update = true;  /**< Force update on first access */

  char saved_tags[256] = ""; /**< Temporary storage for tags when filter is disabled */
};

/**
 * Get or create tag bar runtime data from global cache.
 * Creates the data if it doesn't exist.
 * \param C: Context (for accessing SpaceProperties)
 * \return Tag bar runtime data for this window manager
 */
TagBarRuntimeData *get_tag_bar_data_global(const bContext *C);

/**
 * Mark all cached tag bar data as needing update.
 * Called from listeners when glyphs or tags change.
 */
void tag_bar_mark_all_dirty();

/**
 * Update tag bar buttons based on tags from window manager.
 * \param C: Context for getting current mode
 * \param wm: Window manager containing tag definitions
 * \param v3d: View3D containing active_tag_filter_mask (can be nullptr)
 * \param data: Runtime data to update
 */
void tag_bar_buttons_update(const bContext *C,
                            const blender::wmWindowManager *wm,
                            blender::View3D *v3d,
                            TagBarRuntimeData *data);

/**
 * Check if a tag exists in a comma-separated tag string.
 * \param tags_string: Comma or semicolon separated tag list
 * \param tag_name: Tag name to search for
 * \return True if tag is found in the string
 */
bool has_tag_in_string(const char *tags_string, const char *tag_name);

/**
 * Check if any active tag matches the given tag string (OR logic).
 * \param wm: Window manager containing tag definitions
 * \param tags_string: Comma or semicolon separated tag list
 * \param active_mask: Bitmask of active tag filters
 * \return True if any active tag matches, or if filter is disabled (mask=0)
 */
bool has_any_tag_active(const blender::wmWindowManager *wm,
                        const char *tags_string,
                        int64_t active_mask);

/**
 * Check if ALL active tags match the given tag string (AND logic).
 * When multiple tags are active, the category must have ALL of them to be visible.
 * \param wm: Window manager containing tag definitions
 * \param tags_string: Comma or semicolon separated tag list
 * \param active_mask: Bitmask of active tag filters
 * \return True if all active tags are present in the category, or if filter is disabled (mask=0)
 */
bool has_all_tags_active(const blender::wmWindowManager *wm,
                         const char *tags_string,
                         int64_t active_mask);

/**
 * Callback for tag button click.
 * Toggles the tag filter and updates the UI.
 * \param C: Context
 * \param arg1: Window manager
 * \param arg2: Mode flags as int pointer
 */
void tag_button_click_by_mode(bContext *C, void *arg1, void *arg2);

/**
 * Event handler for tag bar mouse wheel navigation.
 * Handles Ctrl + Mouse Wheel to cycle through tags when exactly one tag is active.
 * \param C: Context
 * \param event: Window event
 * \param userdata: User data (unused)
 * \return WM_UI_HANDLER_BREAK if event was handled, WM_UI_HANDLER_CONTINUE otherwise
 */
int tag_bar_region_handler(bContext *C, const wmEvent *event, void *userdata);

/** \name Region Callbacks */
/** \{ */

/**
 * Initialize the tag bar region.
 * \param wm: Window manager
 * \param region: Region to initialize
 */
void buttons_tag_bar_region_init(blender::wmWindowManager *wm, ARegion *region);

/**
 * Clean up tag bar region data.
 * \param wm: Window manager
 * \param region: Region being exited
 */
void buttons_tag_bar_region_exit(blender::wmWindowManager *wm, ARegion *region);

/**
 * Draw the tag bar region with horizontal tag buttons.
 * \param C: Context
 * \param region: Region to draw
 */
void buttons_tag_bar_region_draw(const bContext *C, ARegion *region);

/**
 * Draw tag bar inside an existing region (e.g., footer region).
 * This is called from view3d_tag_bar_footer_region_draw to display tags.
 * \param C: Context
 * \param region: Region to draw in
 */
void buttons_tag_bar_draw_in_header(const bContext *C, ARegion *region);

/**
 * Draw tag buttons into a given UI block.
 * This is designed to be called from region draw callbacks.
 * \param C: Context
 * \param block: UI block to add buttons to
 * \param region: Region for size calculations
 * \param start_x: X position to start drawing from (after external buttons)
 */
void tag_bar_draw_in_layout(const bContext *C, Block *block, ARegion *region, int start_x = 0);

/**
 * Handle notifier events for the tag bar region.
 * \param params: Listener parameters containing notifier and region
 */
void buttons_tag_bar_region_listener(const wmRegionListenerParams *params);

/**
 * Subscribe to message bus notifications for the tag bar region.
 * \param params: Message subscribe parameters
 */
void buttons_tag_bar_region_message_subscribe(const wmRegionMessageSubscribeParams *params);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Bar Filter Popover Panel
 * \{ */

/**
 * Register the tag bar filter popover panel for a given region type.
 * This function should be called from each spacetype that uses the TAG_BAR region.
 * The panel is added to the global panel type list so it can be used from anywhere.
 *
 * \param art: The region type to register the panel with (typically TAG_BAR region)
 */
void tag_bar_filter_popover_panel_register(ARegionType *art);

/** \} */

}  // namespace blender::ui
