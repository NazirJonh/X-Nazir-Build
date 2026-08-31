/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Global debug flags for controlling verbose logging in interface module.
 * All flags default to false (disabled) to prevent performance issues
 * caused by excessive printf calls during normal operation.
 * 
 * To enable debug logging, set the corresponding flag to true in this file
 * before building Blender.
 */

#include "interface_intern.hh"

namespace blender {

/* ===================================================================== */
/* Global Debug Flag Definitions                                         */
/* ===================================================================== */

/**
 * Enable/disable drag-and-drop texture debug logging.
 * Controls printf output in interface_drop_image.cc:
 * - DROP_IMAGE_register_dropboxes() registration messages
 * 
 * Default: false (disabled)
 */
bool g_drop_image_debug_enabled = false;

/**
 * Enable/disable UI button tag debug logging.
 * Controls printf output in interface.cc:
 * - uiDefButTag() glyph and icon logging
 * 
 * Default: false (disabled)
 */
bool g_ui_button_tag_debug_enabled = false;

/**
 * Enable/disable UI button function application debug logging.
 * Controls printf output in interface_handlers.cc:
 * - ui_apply_but_func() operator and callback logging
 * 
 * Default: false (disabled)
 */
bool g_ui_apply_but_func_debug_enabled = false;

/**
 * Enable/disable tag filter debug logging.
 * Controls printf output in interface_tab_categories.cc:
 * - category_filter_match_tags() rejection messages
 * 
 * Default: false (disabled)
 */
bool g_tag_filter_debug_enabled = false;

/**
 * Enable/disable unassigned category function debug logging.
 * Controls printf output in interface_panel.cc:
 * - category_is_unassigned_for_context() decision path logging
 *
 * Default: false (disabled)
 */
bool g_unassigned_func_debug_enabled = false;

/**
 * Enable/disable tag bar debug logging.
 * Controls printf output in interface_tag_bar.cc:
 * - tag bar data caching, button update and region draw logging
 *
 * Default: false (disabled)
 */
bool g_tag_bar_debug_enabled = false;

}  // namespace blender
