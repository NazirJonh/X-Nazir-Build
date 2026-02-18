/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Internal shared declarations for the Category Tab edit popup implementation,
 * split across:
 *  - interface_tab_categories_edit.cc        (dialog: helpers, snapshot, block create, invoke/exec)
 *  - interface_tab_categories_edit_live.cc   (tag filter / tooltip / live-update / glyph-search)
 *  - interface_tab_categories_edit_popups.cc (glyph & icon grid popups and pickers)
 *
 * Include AFTER interface_intern.hh (relies on its UI/DNA types and enums).
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_string_ref.hh"

#include "interface_intern.hh"

struct PointerRNA;
struct bContext;
struct wmWindowManager;

namespace blender::ui {

/* Shared compile-time constants (were file-static constexpr in the monolith). */
constexpr bool CATEGORY_TAB_DEBUG_ENABLED = true;
constexpr int GLYPH_SEARCH_MAX_RESULTS = 1000;

/* Local tag filter mode for the edit popup (0 = all tags, 1+ = current mode).
 * Defined in interface_tab_categories_edit.cc, read by the live-update callbacks. */
extern char category_tab_popup_local_filter_mode;

/* Shared data structures (moved here so the edit translation units can share them). */
struct CategoryTabIconState {
  char key[128] = "";
  char path[1024] = "";
  char provider[128] = "";
};

enum class CategoryTabIconSourceResolveMode {
  Preview,
  Commit,
};

/**
 * Structure to pass tag data to tooltip function.
 */
struct TagTooltipData {
  std::string tag_name;
  uint32_t mode_flags;
};

/* --- Dialog helpers (defined in interface_tab_categories_edit.cc) --- */
CategoryGlyphItem *category_glyph_item_find(ListBase &items,
                                            const char *category,
                                            const int space_type);
CategoryGlyphItem *category_glyph_item_ensure(ListBase &items,
                                              const char *category,
                                              const int space_type);
void category_tab_remove_stale_space_specific_overrides(wmWindowManager *wm,
                                                        const char *category,
                                                        const char *debug_label);
void category_tab_icon_state_read(PointerRNA *ptr, CategoryTabIconState &r_state);
void category_tab_set_string_if_supported(PointerRNA *ptr,
                                          const char *identifier,
                                          const char *value,
                                          bContext *C);
void category_tab_set_int_or_enum_if_supported(PointerRNA *ptr,
                                               const char *identifier,
                                               const int value,
                                               bContext *C);
void category_tab_icon_state_apply(CategoryGlyphItem &item, const CategoryTabIconState &state);
const char *category_tab_lookup_runtime_default_glyph(wmWindowManager *wm,
                                                      const char *category,
                                                      const int space_type,
                                                      CategoryGlyphItem *override_item,
                                                      const bool clear_override_glyph,
                                                      bool *r_is_fallback,
                                                      float *r_color);
void category_tab_compute_preview_glyph(char r_preview_glyph[8],
                                        const int display_mode_ui,
                                        const char *custom_glyph,
                                        const char *default_glyph,
                                        const bool is_default_fallback,
                                        const char *fallback_letter);
int category_tab_resolve_icon_source(const int display_mode_ui,
                                     const int custom_icon_mode_ui,
                                     const int current_icon_source,
                                     const CategoryTabIconSourceResolveMode mode,
                                     bool *r_clear_blender_icon_key);
bool validate_glyph_hex_input(const char *glyph_raw);

/* --- Tag filter / tooltip helpers (defined in interface_tab_categories_edit_live.cc) --- */
char get_current_object_mode_filter_value(const bContext *C);
uint32_t category_tag_get_mode_flags(const wmWindowManager *wm, const char *tag_name);
std::string tag_glyph_tooltip_func(bContext *C, void *argN, StringRef tip);
void tag_tooltip_data_free(void *arg);

/* --- Popup button callbacks (defined in interface_tab_categories_edit_popups.cc) --- */
void glyph_search_result_button_cb(bContext *C, void *arg1, void *arg2);
void glyph_more_glyphs_button_cb(bContext *C, void *arg1, void *arg2);
void icon_more_icons_button_cb(bContext *C, void *arg1, void *arg2);

}  // namespace blender::ui
