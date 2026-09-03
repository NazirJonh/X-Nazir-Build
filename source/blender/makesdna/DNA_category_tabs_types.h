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

/* Fixed capacity for the structured last-active-category array. Chosen above realistic use (the
 * legacy packed buffer was capped at ~1 KB anyway). As a POD array embedded by value in
 * #CategoryTabsState it serializes, copies and frees automatically with the owning space — no
 * ListBase / blend-read-write plumbing is needed.
 *
 * NOTE: makesdna cannot resolve #defines in array sizes (it reads digits only), so the array
 * below uses the literal 32. Keep this value and that literal in sync; a static_assert in
 * interface_tag_bar.cc guards it. */
#define CATEGORY_LAST_ACTIVE_MAX 32

/**
 * Last active category for a given tag combination (structured replacement for the
 * ``tag_last_active_categories`` packed string). Plain data (no pointers).
 */
struct CategoryLastActive {
  /** Semicolon-separated, alphabetically sorted tag names. Empty = no active tags. Key. */
  char tags[256];
  /** Category idname to restore for this tag combination. */
  char category[128];
};

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
   * LEGACY packed string for last-active-category-per-tag-combination. Superseded by
   * #last_active_categories; kept only so .blend files written before file subversion 502.47
   * can be migrated on load (see versioning). Cleared after migration and never written at
   * runtime. Format was '\n'-separated "tags:category" records (tags = sorted, ';'-separated).
   */
  char tag_last_active_categories[1024] = "";

  /* Pad so the following DNA-struct array starts on an 8-byte boundary: makesdna requires every
   * struct-typed member to be 8-byte aligned. */
  char _pad2[4] = {0, 0, 0, 0};

  /** Structured last active category per tag combination. Valid entries: [0, last_active_num).
   * Size is the literal value of #CATEGORY_LAST_ACTIVE_MAX (makesdna cannot expand the macro). */
  CategoryLastActive last_active_categories[32] = {};
  int last_active_num = 0;

  /** Last known mode flag for detecting editor mode changes (per-mode filter rebuild).
   * 0 = not initialized yet (first call will set it). Driven by the View3D from the active
   * object's mode; other editors key it on their own editor-specific mode. */
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
  /* Sized so this struct's total size grew from the pre-502.47 layout (2588 bytes) by an exact
   * multiple of 16. Every editor embeds #CategoryTabsState by value, so a 16-multiple size delta
   * keeps all following members in those editors at their original alignment — no per-editor
   * padding changes are needed. */
  char _pad1[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  /**
   * Saved tag filter state when "New Add-on!" filter is activated.
   * Restored when the filter is deactivated.
   */
  char saved_tag_filter_tags[256] = "";
};

}  // namespace blender
