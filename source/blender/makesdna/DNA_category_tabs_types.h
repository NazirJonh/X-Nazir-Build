/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Per-editor state for the Category Tabs / Tag Bar system.
 *
 * This state used to be duplicated as a flat set of fields across every editor
 * that hosts a Tag Bar (View3D, SpaceProperties, SpaceImage, SpaceNode). It is
 * collected here into a single embeddable struct so the fields, their RNA
 * registration and versioning live in one place. Editors embed it as
 * ``CategoryTabsState tabs_state``; the RNA layer exposes the members under the
 * historical property names via ``tabs_state.<field>`` sdna paths, so the Python
 * API (``space.tag_filter_enabled`` etc.) is unchanged.
 */

#pragma once

#include "DNA_defs.h"

namespace blender {

struct CategoryTabsState {
  /**
   * Comma-separated list of active tag names for category filtering.
   * Empty string = all categories are visible.
   * Multiple tags are AND-combined (category must have ALL active tags).
   */
  char active_tag_filter_tags[256] = "";

  /**
   * Whether tag filtering is enabled (toggled via filter button).
   * When false: all categories are shown regardless of tags.
   * When true: only categories with matching active tags are shown.
   */
  char tag_filter_enabled = 0;
  /** Category tabs display mode for this editor instance. */
  char category_tabs_display_mode = 1;
  char _pad0[2] = {0, 0};

  /** Per-editor scale factor for category tabs in Icon mode. */
  float category_tabs_zoom_icon = 1.0f;
  /** Per-editor scale factor for category tabs in Mixed mode. */
  float category_tabs_zoom_mixed = 1.0f;
  /** Per-editor scale factor for category tabs in Text mode. */
  float category_tabs_zoom_text = 1.0f;

  /** Horizontal scroll offset in the tag bar (in pixels). */
  int tag_bar_scroll_offset = 0;

  /**
   * Last active category for each tag combination.
   * Format: "tags1:category1;tags2:category2"
   * - tags: semicolon-separated tag names (sorted alphabetically)
   * - category: category idname to restore
   * Example: "Tools:Modify;Create;Modeling:Add Mesh;:General"
   * (empty tags = default category when no tags active)
   */
  char tag_last_active_categories[1024] = "";

  /**
   * Per-mode tag filter state storage.
   * Format: "flag|enabled|tags;flag|enabled|tags;..."
   * - flag: uint32 mode flag value (e.g., 1=OBJECT_MODE, 2048=MESH_EDIT)
   * - enabled: 0 or 1 (tag filter enabled state)
   * - tags: comma-separated tag names
   * Example: "1|1|modeling;2048|1|mesh,animation"
   */
  char tag_filter_state_per_mode[1024] = "";

  /** Last known mode flag for per-mode tag filter save/restore.
   * 0 = not initialized yet (first call will set it).
   * The View3D drives this from the active object's mode; other editors that also carry
   * tag_filter_state_per_mode key their per-mode state on their own editor-specific mode. */
  uint32_t tag_filter_last_mode = 0;

  /**
   * Whether the "New Add-on!" virtual tag filter is active.
   * When true, only pending (unassigned) categories are shown.
   */
  char new_addon_filter_active = 0;
  /**
   * Whether the filter was auto-activated (not by user).
   * Used to distinguish auto-activation (after extension install) from manual activation.
   */
  char new_addon_filter_auto_activated = 0;
  char _pad1[2] = {0, 0};

  /**
   * Saved tag filter state when "New Add-on!" filter is activated.
   * Restored when the filter is deactivated.
   */
  char saved_tag_filter_tags[256] = "";
};

}  // namespace blender
