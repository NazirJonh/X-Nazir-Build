/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Internal shared declarations for the Category Tabs implementation, split across:
 *  - interface_tab_categories.cc      (core: lookup / data / order / deferred / extension)
 *  - interface_tab_categories_draw.cc (drawing + drawing-adjacent operators)
 *
 * Include AFTER interface_intern.hh (relies on its UI/DNA types and enums).
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "interface_intern.hh"

/* -------------------------------------------------------------------- */
/** \name Shared Constants & Macros
 *
 * Tab drawing/layout constants shared between interface_tab_categories.cc (core) and
 * interface_tab_categories_draw.cc (drawing). Were file-static `#define`s in the monolith.
 * \{ */

#define TABS_PADDING_BETWEEN_FACTOR 4.0f
#define TABS_PADDING_TEXT_FACTOR 6.0f
#define TABS_GLYPH_TEXT_GAP_FACTOR 6.0f
#define TABS_SETTINGS_ICON "\ue5d3" /* Material Symbols: settings/gear */
/* Glyph darkening factor for inactive tabs (0.0 = no change, 1.0 = black). */
#define TABS_GLYPH_DARKEN_BASE 0.15f
/* Built-in icon scale in tab content draw (70% of glyph/text-derived base size). */
#define TABS_BUILTIN_ICON_SCALE 1.0f

/* Enable experimental visual outline for active tabs when visual effect is active. */
#define CATEGORY_TAB_VISUAL_ACTIVE_OUTLINE_ENABLE 1

/* Tab background brightening factors for inactive tabs (0.0 = no change, 1.0 = white). */
#define TABS_BG_BRIGHTEN_BASE 0.0f
#define TABS_BG_BRIGHTEN_HOVER 0.05f

/* Extension drop preview ghost/offset height (in pixels). */
#define EXTENSION_DROP_GHOST_HEIGHT 20

/** \} */

namespace blender::ui {

/* Shared compile-time debug switch (was a file-static constexpr). */
constexpr bool CATEGORY_TAB_DEBUG_ENABLED = true;

/* Shared data structures (moved here so both translation units can use them). */
struct PendingCategoryInsert {
  std::string tag_key;
  std::string target_category;
  std::string anchor_before;
  std::string anchor_after;
  bool insert_above = true;
  bool valid = false;
  double timestamp = 0.0;
  Set<std::string> existing_categories;
  Set<std::string> all_existing_categories; /* ALL categories from region (not filtered by tag) */
  Vector<std::string> pre_order;
  std::string source_extension_id; /* ID of the extension being dropped (for drag & drop) */
};

struct DeferredCategoryActivation {
  std::string category_id;
  std::string tag_key;
  bool valid = false;
  double timestamp = 0.0;
  double activate_time = 0.0; /* Absolute time when activation becomes safe. */
  int frame_delay = 0;
  bool wait_for_extension_signal = false;
  bool extension_signal_received = false;
  bool discover_new_category = false; /* When true, find and activate new category after extension install */
  int discover_retry_count = 0; /* Retry counter for discover mode */
  /* Extension integration fields */
  std::string source_extension_id; /* ID of the extension that introduced this category */
  int activation_space_type = 0;   /* Space type where the extension was activated */
  uint32_t activation_mode_flag = 0; /* Mode flags where the extension was activated */
  bool tag_already_assigned = false; /* True if a tag was already assigned via drag & drop */
  std::string tag_name_to_assign;   /* Tag name to assign when category appears (for deferred tag assignment) */
  /* Pending insert position - copied from g_pending_category_insert before it's cleared */
  bool pending_insert_valid = false;
  std::string pending_insert_tag_key;      /* Full key like "VIEW3D:AAA" for JSON order */
  std::string pending_insert_anchor_before;
  std::string pending_insert_anchor_after;
  std::string pending_insert_target_category;
  bool pending_insert_insert_above = true;
};

struct CategoryTabIconResolved {
  int source = CATEGORY_TAB_ICON_SOURCE_AUTO;
  const char *key = nullptr;
  const char *path = nullptr;
  const char *provider = nullptr;
};

/* Shared mutable state. Defined in interface_tab_categories.cc. */
extern PendingCategoryInsert g_pending_category_insert;
extern DeferredCategoryActivation g_deferred_category_activation;
extern Set<std::string> g_known_categories_before_extension_drop;

/* Defined in interface_tab_categories.cc, used by interface_tab_categories_draw.cc. */
void apply_category_order(bContext *C, ARegion *region, CategoryDragState *state);
void apply_glyph_darkening(const int fontid, uchar color[3], const float darken_factor);
void brighten_color_4fv(float color[4], const float factor);
void calculate_drag_insert_bounds(const wmWindowManager *wm, const Vector<PanelCategoryDyn *> &ordered_categories, const char *drag_category_id, const bool drag_is_reserved, int *r_min_insert_index, int *r_max_insert_index);
int calculate_insert_index(const bContext *C, ARegion *region, CategoryDragState *state);
const char *category_first_letter_source_name_get(const ARegion *region, const wmWindowManager *wm, const char *category_id, const char *category_id_draw, int space_type);
bool category_is_reserved_for_reorder(const wmWindowManager *wm, const char *category_id);
int category_tab_icon_id_resolve(const CategoryTabIconResolved &icon_resolved);
void category_tabs_tag_refresh_active_area_ui(const bContext *C);
void darken_color_3ub(uchar color[3], const float factor);
void draw_category_tab_builtin_icon(const rcti *rct, const int icon_id, const float icon_center_y, const float icon_size_px, const float custom_color[3], const bool /*is_active*/, const float darken_factor, const uchar /*theme_col_text*/[3], const uchar /*theme_col_text_sel*/[3]);
Vector<std::string> load_category_order_from_json(const bContext *C, const char *tag_key);
void panel_category_color_lookup(const wmWindowManager *wm, const char *category, float r_color[3]);
const char *panel_category_display_name_lookup(const wmWindowManager *wm, const char *category, int space_type);
bool panel_category_icon_data_lookup(const wmWindowManager *wm, const char *category, CategoryTabIconResolved *r_icon, int space_type);
void register_new_extension_category(const bContext *C, const char *category_id, const char *extension_id, int space_type, uint32_t mode_flag, bool tag_already_assigned);
void save_category_order_to_json(const bContext *C, const char *tag_key, const Vector<std::string> &order);
bool set_glyph_color(const int fontid, const float custom_color[3], const bool is_active, const unsigned char theme_col_text[3], const unsigned char theme_col_text_sel[3], unsigned char r_color[3]);
void update_insert_zone(const bContext *C, const wmWindowManager * /*wm*/, ARegion *region, CategoryDragState *state);

/* Defined in interface_tab_categories_draw.cc, used by interface_tab_categories.cc. */
bool category_tab_should_expand_name(const ARegion *region, const char *category_id, const eUserPref_CategoryTabsDisplayMode display_mode, const bool is_active, const bool use_minimized_gate, const bool is_panel_minimized);
void deferred_category_activation_execute(const bContext *C, ARegion *region);

}  // namespace blender::ui
