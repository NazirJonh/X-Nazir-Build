/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tab Edit Popup - UI and utility functions for editing category tabs.
 *
 * This module provides functionality for:
 * - Hex/UTF-8 glyph conversion utilities
 * - Category tab edit popup UI
 * - Live preview of glyph and color changes
 * - Callbacks for popup cancel/ok
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "BLF_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_glyph_grid_view.hh"
#include "UI_tree_view.hh"

#include "interface_intern.hh"
#include "interface_category_py_bridge.hh"
#include "interface_tag_bar.hh"
#include "regions/interface_regions_intern.hh"
#include "interface_tab_categories_edit_intern.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

/* Import internal template functions with callback support */
using internal::uiTemplateGlyphInputRowWithCallback;
using internal::uiTemplateGlyphSearchResultsWithCallback;
using internal::uiTemplateGlyphSelectorWithCallback;

/* -------------------------------------------------------------------- */
/** \name Static Variables for Preview and State
 * \{ */

/* NOTE: The category edit dialog uses process-global state (preview buffers,
 * `category_tab_current_dialog_op`, `category_tab_popup_block` and the glyph cache below).
 * This intentionally supports only a single open edit dialog per Blender process: opening a
 * second dialog (for example in another window) overwrites these globals, so the first dialog's
 * live preview and glyph search stop updating until it is closed. The runtime guards
 * (`category_tab_current_dialog_op != op`) keep this safe (no dangling access), but multi-window
 * concurrent editing is not supported. */

/* Static buffers for preview callback - updated by live update callback */
/* Declared extern in UI_interface_c.hh for access from interface_tab_categories.cc */
char category_tab_preview_glyph[8] = "";
char category_tab_preview_first_letter[8] = "";
float category_tab_preview_color[3] = {0.0f, 0.0f, 0.0f};

/* Static pointer to preview button - updated when popup opens, used for live updates */
static Button *category_tab_preview_button = nullptr;

/* Static pointer to current dialog operator - needed for Reset/Save buttons */
wmOperator *category_tab_current_dialog_op = nullptr;

/* Static pointer to popup block - needed for Save button to close popup */
Block *category_tab_popup_block = nullptr;

/* Track last closed popup time and category to prevent immediate reopen */
double category_tab_popup_close_time = 0.0;
char category_tab_last_closed_category[64] = "";

/* Local tag filter mode for the edit popup (0 = all tags, 1+ = current mode).
 * This is separate from wm->category_tag_filter_mode to prevent cross-UI interference.
 * When the popup opens, it initializes from the global setting, but changes only affect the popup. */
char category_tab_popup_local_filter_mode = 0;



/* [POPULAR ADDONS DB] - Temporary: fallback icon lookup from Popular Addons Database.
 * When extensions start bundling their own icons, this functionality will no longer be needed.
 * Set to false to disable all Popular Addons Database integration code. */
static constexpr bool WITH_POPULAR_ADDONS_DATABASE = true;

static void category_tab_edit_dialog_clear_runtime_state(const bool clear_popup_block)
{
  category_tab_current_dialog_op = nullptr;
  category_tab_preview_button = nullptr;
  if (clear_popup_block) {
    category_tab_popup_block = nullptr;
  }
}

static void category_tab_edit_dialog_mark_closed(const char *category)
{
  category_tab_popup_close_time = BLI_time_now_seconds();
  STRNCPY(category_tab_last_closed_category, category);
}

static void category_glyph_item_init(
    CategoryGlyphItem &item, const char *category, const int space_type)
{
  STRNCPY(item.category, category);
  item.space_type = space_type;
  item.first_letter[0] = category[0] ? category[0] : '?';
  item.first_letter[1] = '\0';
  item.glyph[0] = '\0';
  item.display_name[0] = '\0';
  zero_v3(item.color);
  item.tags[0] = '\0';
  item.icon_key[0] = '\0';
  item.icon_path[0] = '\0';
  item.icon_provider[0] = '\0';
  item.icon_source = 0;
  item.glyph_mode = 0;
}

CategoryGlyphItem *category_glyph_item_find(
    ListBase &items, const char *category, const int space_type)
{
  for (CategoryGlyphItem *item = static_cast<CategoryGlyphItem *>(items.first); item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (item->space_type == space_type && STREQ(item->category, category)) {
      return item;
    }
  }
  return nullptr;
}

static const CategoryGlyphItem *category_glyph_item_find_const(
    const ListBase &items, const char *category, const int space_type)
{
  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(items.first); item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (item->space_type == space_type && STREQ(item->category, category)) {
      return item;
    }
  }
  return nullptr;
}

static CategoryGlyphItem *category_glyph_item_find_with_global_fallback(
    ListBase &items, const char *category, const int space_type)
{
  if (CategoryGlyphItem *item = category_glyph_item_find(items, category, space_type)) {
    return item;
  }
  if (space_type != -1) {
    return category_glyph_item_find(items, category, -1);
  }
  return nullptr;
}

const CategoryGlyphItem *category_glyph_item_find_with_global_fallback_const(
    const ListBase &items, const char *category, const int space_type)
{
  if (const CategoryGlyphItem *item =
          category_glyph_item_find_const(items, category, space_type))
  {
    return item;
  }
  if (space_type != -1) {
    return category_glyph_item_find_const(items, category, -1);
  }
  return nullptr;
}

CategoryGlyphItem *category_glyph_item_ensure(
    ListBase &items, const char *category, const int space_type)
{
  if (CategoryGlyphItem *item = category_glyph_item_find(items, category, space_type)) {
    return item;
  }

  CategoryGlyphItem *item = MEM_new<CategoryGlyphItem>(__func__);
  category_glyph_item_init(*item, category, space_type);
  BLI_addtail(&items, item);
  return item;
}

/**
 * Global-First migration: remove all stale space-specific overrides (space_type != -1) for a
 * category, keeping only the GLOBAL (space_type == -1) entry. The whole system is Global-First,
 * so any leftover per-space overrides would shadow the GLOBAL entry in lookup. This is
 * destructive: the removed per-space data is not preserved, so callers that must be able to undo
 * (such as cancel) MUST NOT call this. \a debug_label is only used for debug logging.
 */
void category_tab_remove_stale_space_specific_overrides(wmWindowManager *wm,
                                                               const char *category,
                                                               const char *debug_label)
{
  for (CategoryGlyphItem *old_item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       old_item != nullptr;)
  {
    CategoryGlyphItem *next_item = static_cast<CategoryGlyphItem *>(old_item->next);
    if (STREQ(old_item->category, category) && old_item->space_type != -1) {
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[%s] Removed stale space-specific override for '%s' (space_type=%d)\n",
               debug_label,
               category,
               old_item->space_type);
      }
      BLI_remlink(&wm->category_glyph_overrides, old_item);
      MEM_delete(old_item);
    }
    old_item = next_item;
  }
  UNUSED_VARS(debug_label);
}


void category_tab_icon_state_read(PointerRNA *ptr, CategoryTabIconState &r_state)
{
  RNA_string_get(ptr, "icon_key", r_state.key);
  RNA_string_get(ptr, "icon_path", r_state.path);
  RNA_string_get(ptr, "icon_provider", r_state.provider);
}

static void category_tab_icon_state_write(PointerRNA *ptr, const CategoryTabIconState &state)
{
  RNA_string_set(ptr, "icon_key", state.key);
  RNA_string_set(ptr, "icon_path", state.path);
  RNA_string_set(ptr, "icon_provider", state.provider);
}

void category_tab_set_string_if_supported(PointerRNA *ptr,
                                                 const char *identifier,
                                                 const char *value,
                                                 bContext *C = nullptr)
{
  PropertyRNA *property = RNA_struct_find_property(ptr, identifier);
  if (property && RNA_property_type(property) == PROP_STRING) {
    RNA_string_set(ptr, identifier, value);
    /* Trigger Python update callback and UI redraw */
    if (C) {
      RNA_property_update(C, ptr, property);
    }
  }
}

void category_tab_set_int_or_enum_if_supported(PointerRNA *ptr,
                                                      const char *identifier,
                                                      const int value,
                                                      bContext *C = nullptr)
{
  PropertyRNA *property = RNA_struct_find_property(ptr, identifier);
  if (!property) {
    return;
  }

  switch (RNA_property_type(property)) {
    case PROP_INT:
      RNA_int_set(ptr, identifier, value);
      break;
    case PROP_ENUM:
      RNA_enum_set(ptr, identifier, value);
      break;
    default:
      break;
  }
  
  /* Trigger Python update callback and UI redraw */
  if (C) {
    RNA_property_update(C, ptr, property);
  }
}

static void category_tab_icon_state_from_item(const CategoryGlyphItem &item,
                                              CategoryTabIconState &r_state)
{
  STRNCPY(r_state.key, item.icon_key);
  STRNCPY(r_state.path, item.icon_path);
  STRNCPY(r_state.provider, item.icon_provider);
}

static void category_tab_icon_state_apply_item_to_operator(PointerRNA *ptr,
                                                           const CategoryGlyphItem &item)
{
  RNA_enum_set(ptr, "icon_source", item.icon_source);
  CategoryTabIconState icon_state;
  category_tab_icon_state_from_item(item, icon_state);
  category_tab_icon_state_write(ptr, icon_state);
}

void category_tab_icon_state_apply(CategoryGlyphItem &item,
                                          const CategoryTabIconState &state)
{
  STRNCPY(item.icon_key, state.key);
  STRNCPY(item.icon_path, state.path);
  STRNCPY(item.icon_provider, state.provider);
}

struct CategoryTabDialogSnapshot {
  char display_name[32] = "";
  char glyph_hex[16] = "";
  float color[3] = {0.0f, 0.0f, 0.0f};
  char tags[256] = "";
  CategoryTabIconState icon;
  int icon_source = 0;
  int glyph_mode = 0;
  bool has_override = false;
  int space_type = -1;
  int override_space_type = -1;
};

static void category_tab_dialog_snapshot_read(PointerRNA *ptr,
                                              CategoryTabDialogSnapshot &r_snapshot)
{
  RNA_string_get(ptr, "original_display_name", r_snapshot.display_name);
  RNA_string_get(ptr, "original_glyph", r_snapshot.glyph_hex);
  RNA_float_get_array(ptr, "original_color", r_snapshot.color);
  RNA_string_get(ptr, "original_tags", r_snapshot.tags);
  RNA_string_get(ptr, "original_icon_key", r_snapshot.icon.key);
  RNA_string_get(ptr, "original_icon_path", r_snapshot.icon.path);
  RNA_string_get(ptr, "original_icon_provider", r_snapshot.icon.provider);
  r_snapshot.icon_source = RNA_enum_get(ptr, "original_icon_source");
  r_snapshot.glyph_mode = RNA_enum_get(ptr, "original_glyph_mode");
  r_snapshot.has_override = RNA_boolean_get(ptr, "original_has_override");
  r_snapshot.space_type = RNA_int_get(ptr, "original_space_type");
  r_snapshot.override_space_type = RNA_int_get(ptr, "original_override_space_type");
}

static void category_tab_dialog_snapshot_write(PointerRNA *ptr,
                                               const CategoryTabDialogSnapshot &snapshot)
{
  RNA_string_set(ptr, "original_display_name", snapshot.display_name);
  RNA_string_set(ptr, "original_glyph", snapshot.glyph_hex);
  RNA_float_set_array(ptr, "original_color", snapshot.color);
  RNA_string_set(ptr, "original_tags", snapshot.tags);
  RNA_string_set(ptr, "original_icon_key", snapshot.icon.key);
  RNA_string_set(ptr, "original_icon_path", snapshot.icon.path);
  RNA_string_set(ptr, "original_icon_provider", snapshot.icon.provider);
  RNA_enum_set(ptr, "original_icon_source", snapshot.icon_source);
  RNA_enum_set(ptr, "original_glyph_mode", snapshot.glyph_mode);
  RNA_boolean_set(ptr, "original_has_override", snapshot.has_override);
  RNA_int_set(ptr, "original_space_type", snapshot.space_type);
  RNA_int_set(ptr, "original_override_space_type", snapshot.override_space_type);
}

static void category_tab_dialog_snapshot_apply_to_item(CategoryGlyphItem &item,
                                                       const CategoryTabDialogSnapshot &snapshot)
{
  char glyph_utf8[8] = "";
  process_glyph_input(snapshot.glyph_hex, glyph_utf8, sizeof(glyph_utf8));

  STRNCPY(item.display_name, snapshot.display_name);
  STRNCPY(item.glyph, glyph_utf8);
  copy_v3_v3(item.color, snapshot.color);
  STRNCPY(item.tags, snapshot.tags);
  category_tab_icon_state_apply(item, snapshot.icon);
  item.icon_source = snapshot.icon_source;
  item.glyph_mode = snapshot.glyph_mode;
}

static void category_tab_dialog_snapshot_apply_runtime_visuals_to_item(
    CategoryGlyphItem &item, const CategoryTabDialogSnapshot &snapshot)
{
  char glyph_utf8[8] = "";
  process_glyph_input(snapshot.glyph_hex, glyph_utf8, sizeof(glyph_utf8));

  STRNCPY(item.glyph, glyph_utf8);
  copy_v3_v3(item.color, snapshot.color);
  category_tab_icon_state_apply(item, snapshot.icon);
  item.icon_source = snapshot.icon_source;
  item.glyph_mode = snapshot.glyph_mode;
}

const char *category_tab_lookup_runtime_default_glyph(wmWindowManager *wm,
                                                             const char *category,
                                                             const int space_type,
                                                             CategoryGlyphItem *override_item,
                                                             const bool clear_override_glyph,
                                                             bool *r_is_fallback,
                                                             float *r_color)
{
  bool is_fallback_dummy = false;
  bool *is_fallback = r_is_fallback ? r_is_fallback : &is_fallback_dummy;

  char previous_override_glyph[8] = "";
  const bool temporarily_clear_override_for_lookup =
      clear_override_glyph && override_item && override_item->glyph[0] != '\0';

  if (temporarily_clear_override_for_lookup) {
    STRNCPY(previous_override_glyph, override_item->glyph);
    override_item->glyph[0] = '\0';
  }

  const char *default_glyph =
      panel_category_glyph_lookup(wm, category, nullptr, is_fallback, r_color, space_type);

  if (temporarily_clear_override_for_lookup) {
    STRNCPY(override_item->glyph, previous_override_glyph);
  }

  return default_glyph;
}

void category_tab_compute_preview_glyph(char r_preview_glyph[8],
                                               const int display_mode_ui,
                                               const char *custom_glyph,
                                               const char *default_glyph,
                                               const bool is_default_fallback,
                                               const char *fallback_letter)
{
  if (display_mode_ui == 2) {
    BLI_strncpy(r_preview_glyph, fallback_letter ? fallback_letter : "", 8);
  }
  else if (custom_glyph && custom_glyph[0] != '\0') {
    BLI_strncpy(r_preview_glyph, custom_glyph, 8);
  }
  else if (default_glyph && !is_default_fallback) {
    BLI_strncpy(r_preview_glyph, default_glyph, 8);
  }
  else {
    BLI_strncpy(r_preview_glyph, fallback_letter ? fallback_letter : "", 8);
  }
}

static int category_tab_resolve_glyph_mode_with_fallback(const wmWindowManager *wm,
                                                         const char *category,
                                                         const int space_type)
{
  const CategoryGlyphItem *override_exact = category_glyph_item_find_const(
      wm->category_glyph_overrides, category, space_type);
  if (override_exact) {
    return override_exact->glyph_mode;
  }

  if (space_type != -1) {
    const CategoryGlyphItem *override_global = category_glyph_item_find_const(
        wm->category_glyph_overrides, category, -1);
    if (override_global && override_global->glyph_mode != 0) {
      return override_global->glyph_mode;
    }
  }

  const CategoryGlyphItem *mapping_exact = category_glyph_item_find_const(
      wm->category_glyph_mappings, category, space_type);
  if (mapping_exact) {
    return mapping_exact->glyph_mode;
  }

  if (space_type != -1) {
    const CategoryGlyphItem *mapping_global = category_glyph_item_find_const(
        wm->category_glyph_mappings, category, -1);
    if (mapping_global && mapping_global->glyph_mode != 0) {
      return mapping_global->glyph_mode;
    }
  }

  return 0;
}

static const char *category_tab_lookup_tags_with_fallback(const wmWindowManager *wm,
                                                          const char *category,
                                                          const int space_type)
{
  const CategoryGlyphItem *override_exact = category_glyph_item_find_const(
      wm->category_glyph_overrides, category, space_type);
  if (override_exact && override_exact->tags[0] != '\0') {
    return override_exact->tags;
  }

  if (space_type != -1) {
    const CategoryGlyphItem *override_global = category_glyph_item_find_const(
        wm->category_glyph_overrides, category, -1);
    if (override_global && override_global->tags[0] != '\0') {
      return override_global->tags;
    }
  }

  const CategoryGlyphItem *mapping_exact = category_glyph_item_find_const(
      wm->category_glyph_mappings, category, space_type);
  if (mapping_exact && mapping_exact->tags[0] != '\0') {
    return mapping_exact->tags;
  }

  if (space_type != -1) {
    const CategoryGlyphItem *mapping_global = category_glyph_item_find_const(
        wm->category_glyph_mappings, category, -1);
    if (mapping_global && mapping_global->tags[0] != '\0') {
      return mapping_global->tags;
    }
  }

  return nullptr;
}

static bool category_tab_resolve_default_glyph_from_mappings(const wmWindowManager *wm,
                                                             const char *category,
                                                             const int space_type,
                                                             char r_default_glyph[8])
{
  r_default_glyph[0] = '\0';

  const CategoryGlyphItem *map_item_resolved = category_glyph_item_find_with_global_fallback_const(
      wm->category_glyph_mappings, category, space_type);
  if (!map_item_resolved) {
    return false;
  }

  if (map_item_resolved->default_glyph[0] != '\0') {
    BLI_strncpy(r_default_glyph, map_item_resolved->default_glyph, 8);
    return true;
  }

  if (is_single_glyph_str(category) && map_item_resolved->glyph[0] != '\0') {
    /* Glyph-only category: reset/default is its glyph value. */
    BLI_strncpy(r_default_glyph, map_item_resolved->glyph, 8);
    return true;
  }

  /* text_only categories intentionally keep default_glyph empty so reset falls back to first letter. */
  return false;
}

struct CategoryTabInvokeLoadResult {
  CategoryGlyphItem *override_item = nullptr;
  bool has_override = false;
  bool override_is_empty = false;
  bool override_icon_needs_mapping = false;
  bool user_glyph_override_assigned = false;
  bool explicit_icon_mode_assigned = false;
};

static CategoryTabInvokeLoadResult category_tab_invoke_load_operator_state_from_items(
    PointerRNA *op_ptr,
    wmWindowManager *wm,
    ARegion *region,
    const char *category,
    const int space_type)
{
  CategoryTabInvokeLoadResult result;

  /* First check category_glyph_overrides (user changes in current session).
   * Priority: exact editor space first, then GLOBAL fallback. */
  result.override_item = category_glyph_item_find_with_global_fallback(
      wm->category_glyph_overrides, category, space_type);

  if (result.override_item) {
    const bool has_icon_payload = (result.override_item->icon_key[0] != '\0') ||
                                  (result.override_item->icon_path[0] != '\0') ||
                                  (result.override_item->icon_provider[0] != '\0');
    const bool has_explicit_icon_mode = ELEM(result.override_item->icon_source, 1, 2);
    if (has_explicit_icon_mode) {
      result.explicit_icon_mode_assigned = true;
    }

    if (result.override_item->display_name[0] == '\0' && result.override_item->glyph[0] == '\0' &&
        is_zero_v3(result.override_item->color) && !has_icon_payload && !has_explicit_icon_mode)
    {
      result.override_is_empty = true;
      result.has_override = true;
    }
    else {
      char extracted_glyph[16];
      char clean_display_name[32];
      extract_leading_glyph(result.override_item->display_name,
                            extracted_glyph,
                            sizeof(extracted_glyph),
                            clean_display_name,
                            sizeof(clean_display_name));

      if (clean_display_name[0] == '\0') {
        // Only use panel label for glyph_only categories
        // For text_only categories, use the category name itself
        if (is_single_glyph_str(category)) {
          const char *panel_label = find_panel_label_for_category(region, category);
          if (panel_label) {
            STRNCPY(clean_display_name, panel_label);
          }
        }
        else {
          // For text_only categories: use category name, not panel label
          STRNCPY(clean_display_name, category);
        }
      }

      RNA_string_set(op_ptr, "display_name", clean_display_name);

      char hex_code[16];
      if (result.override_item->glyph[0] != '\0') {
        utf8_to_hex_codepoint(result.override_item->glyph, hex_code, sizeof(hex_code));
        result.user_glyph_override_assigned = true;
      }
      else if (extracted_glyph[0] != '\0') {
        STRNCPY(hex_code, extracted_glyph);
        result.user_glyph_override_assigned = true;
      }
      else {
        hex_code[0] = '\0';
      }

      RNA_string_set(op_ptr, "glyph", hex_code);
      RNA_float_set_array(op_ptr, "color", result.override_item->color);
      category_tab_icon_state_apply_item_to_operator(op_ptr, *result.override_item);

      result.override_icon_needs_mapping = (!has_icon_payload && !has_explicit_icon_mode);
      result.has_override = true;
    }
  }

  /* If override is used for text/glyph/color only and doesn't carry explicit icon data,
   * keep icon fields from persisted mappings (JSON source of truth). */
  if (result.has_override && !result.override_is_empty && result.override_icon_needs_mapping) {
    const CategoryGlyphItem *mapping_item = category_glyph_item_find_with_global_fallback_const(
        wm->category_glyph_mappings, category, space_type);

    if (mapping_item) {
      category_tab_icon_state_apply_item_to_operator(op_ptr, *mapping_item);
      if (ELEM(mapping_item->icon_source, 1, 2)) {
        result.explicit_icon_mode_assigned = true;
      }
      if (mapping_item->source_extension[0] != '\0') {
        RNA_string_set(op_ptr, "source_extension", mapping_item->source_extension);
      }
    }
  }

  /* If no override OR override is empty, check category_glyph_mappings (saved settings from JSON). */
  if (!result.has_override || result.override_is_empty) {
    const CategoryGlyphItem *mapping_item = category_glyph_item_find_with_global_fallback_const(
        wm->category_glyph_mappings, category, space_type);

    if (mapping_item) {
      if (mapping_item->display_name[0] != '\0') {
        RNA_string_set(op_ptr, "display_name", mapping_item->display_name);
      }

      if (mapping_item->glyph[0] != '\0') {
        const bool is_glyph_only_category = is_single_glyph_str(category);
        const bool is_intrinsic_category_glyph = is_glyph_only_category &&
                                                 STREQ(mapping_item->glyph, category);
        if (!is_intrinsic_category_glyph) {
          char hex_code[16];
          utf8_to_hex_codepoint(mapping_item->glyph, hex_code, sizeof(hex_code));
          RNA_string_set(op_ptr, "glyph", hex_code);
        }
      }

      if (!is_zero_v3(mapping_item->color)) {
        RNA_float_set_array(op_ptr, "color", mapping_item->color);
      }

      category_tab_icon_state_apply_item_to_operator(op_ptr, *mapping_item);
      result.has_override = false;
    }
  }

  return result;
}

bool category_tab_try_auto_detect_extension_icon(bContext *C,
                                                   const char *category,
                                                   char r_icon_path[1024],
                                                   char r_icon_provider[128]);

/**
 * [POPULAR ADDONS DB] Query Popular Addons Database for icon information.
 * Returns true if icon found and paths set.
 * Can be removed when extensions bundle their own icons.
 */
static bool category_tab_query_popular_addons_database(
    bContext *C,
    const char *addon_id,
    char r_icon_path[1024],
    char r_icon_provider[128])
{
#ifdef WITH_PYTHON
    if (!C || !addon_id || !addon_id[0]) {
        return false;
    }
    
    // Clear output buffers
    r_icon_path[0] = '\0';
    r_icon_provider[0] = '\0';
    
    // Check if Popular Addons Database is available
    const char *check_script =
        "try:\n"
        "    from bl_ext.user_default.popular_addons_database import api as _pad_api\n"
        "    available = _pad_api.check_popular_addons_database_available()\n"
        "    print(f'POPULAR_ADDONS_AVAILABLE:{available}')\n"
        "except Exception:\n"
        "    print('POPULAR_ADDONS_AVAILABLE:False')";
    
    // Execute availability check via BPY
    const char *imports[] = {"bpy", nullptr};
    char *result = nullptr;
    char *err_msg = nullptr;
    BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};
    const bool check_success = BPY_run_string_as_string(C, imports, check_script, &err_info, &result);
    
    if (!check_success || !result) {
        if (err_msg) {
            MEM_delete(err_msg);
        }
        return false;
    }
    
    // Parse availability result
    bool available = (strstr(result, "POPULAR_ADDONS_AVAILABLE:True") != nullptr);
    MEM_delete(result);
    if (err_msg) {
        MEM_delete(err_msg);
    }
    
    if (!available) {
        return false;
    }
    
    // Query for addon icon. Escape the addon id before interpolating it into the Python
    // literal (it is an arbitrary category name; unescaped ' \ or newline would raise
    // SyntaxError or allow injection), matching the centralized bridge's convention.
    const std::string escaped_addon_id = category_tab_escape_for_python_literal(addon_id);
    char query_script[2048];
    SNPRINTF(query_script,
        "try:\n"
        "    from bl_ext.user_default.popular_addons_database import api as _pad_api\n"
        "    result = _pad_api.query_popular_addon_icon('%s')\n"
        "    if result:\n"
        "        icon_path = result.get('icon_path', '')\n"
        "        icon_provider = result.get('icon_provider', 'popular_addons_database')\n"
        "        print(f'ADDON_ICON_FOUND:{icon_path}:{icon_provider}')\n"
        "    else:\n"
        "        print('ADDON_ICON_NOT_FOUND')\n"
        "except Exception as e:\n"
        "    print(f'ADDON_ICON_ERROR:{e}')",
        escaped_addon_id.c_str());
    
    char *query_result = nullptr;
    char *query_err_msg = nullptr;
    BPy_RunErrInfo query_err_info = {false, nullptr, "", &query_err_msg};
    const bool query_success = BPY_run_string_as_string(C, imports, query_script, &query_err_info, &query_result);
    
    if (!query_success || !query_result) {
        if (query_err_msg) {
            MEM_delete(query_err_msg);
        }
        return false;
    }
    
    // Parse query result
    bool icon_found = false;
    if (const char *found_line = strstr(query_result, "ADDON_ICON_FOUND:")) {
        const char *data_start = found_line + strlen("ADDON_ICON_FOUND:");
        
        // Find first colon (separator between path and provider)
        const char *colon_pos = strchr(data_start, ':');
        if (colon_pos) {
            // Extract icon path
            size_t path_len = colon_pos - data_start;
            if (path_len > 0 && path_len < 1023) {
                BLI_strncpy_ensure_pad(r_icon_path, data_start, path_len + 1, '\0');
                
                // Extract provider
                const char *provider_start = colon_pos + 1;
                const char *newline_pos = strchr(provider_start, '\n');
                size_t provider_len = newline_pos ? (newline_pos - provider_start) : strlen(provider_start);
                
                if (provider_len < 127) {
                    BLI_strncpy_ensure_pad(r_icon_provider, provider_start, provider_len + 1, '\0');
                    
                    icon_found = true;
                    
                    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
                        printf("[POPULAR_ADDONS] Found icon for '%s': path='%s', provider='%s'\n",
                               addon_id, r_icon_path, r_icon_provider);
                    }
                }
            }
        }
    }
    
    MEM_delete(query_result);
    if (query_err_msg) {
        MEM_delete(query_err_msg);
    }
    return icon_found;
    
#else
    UNUSED_VARS(C, addon_id, r_icon_path, r_icon_provider);
    return false;
#endif
}

static void category_tab_invoke_apply_post_load_defaults(bContext *C,
                                                         PointerRNA *op_ptr,
                                                         wmWindowManager *wm,
                                                         ARegion *region,
                                                         const char *category,
                                                         const int space_type,
                                                         const bool user_glyph_override_assigned,
                                                         const bool explicit_icon_mode_assigned)
{
  /* If no explicit icon mode/payload is set, try to detect extension root icon
   * (icon.png/icon.webp/...) for this category.
   * Only do this when there is no user glyph override; fallback glyphs are ignored. */
  if (!user_glyph_override_assigned && !explicit_icon_mode_assigned) {
    CategoryTabIconState current_icon_state;
    category_tab_icon_state_read(op_ptr, current_icon_state);

    const bool has_icon_payload = (current_icon_state.key[0] != '\0') ||
                                  (current_icon_state.path[0] != '\0') ||
                                  (current_icon_state.provider[0] != '\0');

    if (!has_icon_payload) {
      char detected_icon_path[1024] = "";
      char detected_icon_provider[128] = "";
      if (category_tab_try_auto_detect_extension_icon(
              C, category, detected_icon_path, detected_icon_provider))
      {
        RNA_enum_set(op_ptr, "icon_source", 0); /* AUTO */
        RNA_string_set(op_ptr, "icon_path", detected_icon_path);
        if (detected_icon_provider[0] != '\0') {
          RNA_string_set(op_ptr, "icon_provider", detected_icon_provider);
        }
      }
    }
  }

  /* If display_name is still empty, use panel label or category. */
  char current_display_name[32] = "";
  RNA_string_get(op_ptr, "display_name", current_display_name);
  if (current_display_name[0] == '\0') {
    if (is_single_glyph_str(category)) {
      const char *panel_label = find_panel_label_for_category(region, category);
      if (panel_label) {
        RNA_string_set(op_ptr, "display_name", panel_label);
      }
    }
    else {
      RNA_string_set(op_ptr, "display_name", category);
    }
  }

  /* If glyph/color are still default, fill from runtime glyph lookup. */
  char runtime_default_glyph[8] = "";
  float runtime_default_color[3] = {0.0f, 0.0f, 0.0f};
  bool runtime_default_is_fallback = false;
  bool runtime_default_lookup_done = false;

  auto ensure_runtime_default_lookup = [&]() {
    if (runtime_default_lookup_done) {
      return;
    }

    const char *default_glyph = category_tab_lookup_runtime_default_glyph(
        wm,
        category,
        space_type,
        nullptr,
        false,
        &runtime_default_is_fallback,
        runtime_default_color);
    if (default_glyph) {
      STRNCPY(runtime_default_glyph, default_glyph);
    }
    runtime_default_lookup_done = true;
  };

  char current_glyph[16] = "";
  RNA_string_get(op_ptr, "glyph", current_glyph);
  if (current_glyph[0] == '\0') {
    ensure_runtime_default_lookup();

    if (runtime_default_glyph[0] != '\0') {
      const bool is_glyph_only_category = is_single_glyph_str(category);
      const bool is_intrinsic_category_glyph =
          is_glyph_only_category && STREQ(runtime_default_glyph, category);
      if (!is_intrinsic_category_glyph && !runtime_default_is_fallback) {
        char hex_code[16];
        utf8_to_hex_codepoint(runtime_default_glyph, hex_code, sizeof(hex_code));
        RNA_string_set(op_ptr, "glyph", hex_code);
      }

      const CategoryGlyphItem *mapping_item = category_glyph_item_find_with_global_fallback_const(
          wm->category_glyph_mappings, category, space_type);
      if (mapping_item && mapping_item->source_extension[0] != '\0') {
        RNA_string_set(op_ptr, "source_extension", mapping_item->source_extension);
      }
    }
  }

  float current_color[3] = {0.0f, 0.0f, 0.0f};
  RNA_float_get_array(op_ptr, "color", current_color);
  if (is_zero_v3(current_color)) {
    ensure_runtime_default_lookup();
    if (!is_zero_v3(runtime_default_color)) {
      RNA_float_set_array(op_ptr, "color", runtime_default_color);
    }
  }
}

struct CategoryTabDisplayModeState {
  int display_mode_ui = 0;
  int custom_icon_mode_ui = 0;
};


static CategoryTabDisplayModeState category_tab_resolve_display_mode_state(
    const int glyph_mode, const int icon_source, const bool has_icon_key, const bool has_icon_path)
{
  CategoryTabDisplayModeState state;

  const bool has_icon_payload = has_icon_key || has_icon_path;
  const bool icon_mode_enabled = (icon_source != 2); /* OFF */

  if (glyph_mode == 1) {
    state.display_mode_ui = 2; /* TEXT */
    return state;
  }

  if (!(icon_mode_enabled && has_icon_payload)) {
    return state; /* GLYPH/BLENDER by default. */
  }

  state.display_mode_ui = 1; /* CUSTOM_ICON */

  if (icon_source == 1 && has_icon_key) {
    state.custom_icon_mode_ui = 0; /* BLENDER */
  }
  else if (icon_source == 0 && has_icon_path) {
    state.custom_icon_mode_ui = 1; /* CUSTOM */
  }
  else if (has_icon_path) {
    state.custom_icon_mode_ui = 1; /* CUSTOM fallback */
  }
  else if (has_icon_key) {
    state.custom_icon_mode_ui = 0; /* BLENDER fallback */
  }

  return state;
}

int category_tab_resolve_icon_source(const int display_mode_ui,
                                            const int custom_icon_mode_ui,
                                            const int current_icon_source,
                                            const CategoryTabIconSourceResolveMode mode,
                                            bool *r_clear_blender_icon_key)
{
  if (r_clear_blender_icon_key) {
    *r_clear_blender_icon_key = false;
  }

  if (display_mode_ui == 0 || display_mode_ui == 2) {
    return 2; /* OFF */
  }

  if (custom_icon_mode_ui == 0) {
    return 1; /* MANUAL: Blender Icon */
  }

  if (mode == CategoryTabIconSourceResolveMode::Preview) {
    if (r_clear_blender_icon_key) {
      *r_clear_blender_icon_key = true;
    }
    return 0; /* AUTO: path/provider chain (external icon) */
  }

  UNUSED_VARS(current_icon_source);
  return 1; /* MANUAL: Custom image icon - explicit user choice on commit */
}

static void category_tab_invoke_build_and_store_snapshot(PointerRNA *op_ptr,
                                                         const wmWindowManager *wm,
                                                         const char *category,
                                                         const int space_type,
                                                         const bool has_override,
                                                         const CategoryGlyphItem *override_item)
{
  char current_display_name[32] = "";
  char current_glyph[16] = "";
  float current_color[3] = {0.0f, 0.0f, 0.0f};
  RNA_string_get(op_ptr, "display_name", current_display_name);
  RNA_string_get(op_ptr, "glyph", current_glyph);
  RNA_float_get_array(op_ptr, "color", current_color);

  const int current_icon_source = RNA_enum_get(op_ptr, "icon_source");
  CategoryTabIconState current_icon_state;
  category_tab_icon_state_read(op_ptr, current_icon_state);

  const bool has_icon_key = (current_icon_state.key[0] != '\0');
  const bool has_icon_path = (current_icon_state.path[0] != '\0');
  const int current_glyph_mode = category_tab_resolve_glyph_mode_with_fallback(
      wm, category, space_type);

  CategoryTabDialogSnapshot snapshot;
  STRNCPY(snapshot.display_name, current_display_name);
  STRNCPY(snapshot.glyph_hex, current_glyph);
  copy_v3_v3(snapshot.color, current_color);
  snapshot.icon = current_icon_state;
  snapshot.icon_source = current_icon_source;
  snapshot.glyph_mode = current_glyph_mode;
  snapshot.has_override = has_override;
  snapshot.space_type = space_type;
  snapshot.override_space_type = (has_override && override_item) ? override_item->space_type :
                                                                    space_type;

  const CategoryTabDisplayModeState display_mode_state = category_tab_resolve_display_mode_state(
      current_glyph_mode, current_icon_source, has_icon_key, has_icon_path);
  RNA_enum_set(op_ptr, "display_mode_ui", display_mode_state.display_mode_ui);
  RNA_enum_set(op_ptr, "custom_icon_mode_ui", display_mode_state.custom_icon_mode_ui);

  const char *tags_str = category_tab_lookup_tags_with_fallback(wm, category, space_type);
  if (tags_str) {
    STRNCPY(snapshot.tags, tags_str);
  }
  category_tab_dialog_snapshot_write(op_ptr, snapshot);
}

bool category_tab_edit_dialog_is_open_for_category(const char *category)
{
  if (!category || category[0] == '\0' || category_tab_current_dialog_op == nullptr) {
    return false;
  }

  char edited_category[64] = "";
  RNA_string_get(category_tab_current_dialog_op->ptr, "category", edited_category);
  return STREQ(edited_category, category);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hex/UTF-8 Conversion Utilities
 * \{ */

bool validate_glyph_hex_input(const char *glyph_raw)
{
  if (!glyph_raw || glyph_raw[0] == '\0') {
    return true;
  }

  /* Check total string length (including optional 0x prefix). */
  const size_t total_len = strlen(glyph_raw);
  if (total_len > 8) { /* Max: "0x" + 6 hex digits. */
    return false;
  }

  const char *hex_start = glyph_raw;

  /* Skip optional "0x" or "0X" prefix. */
  if (glyph_raw[0] == '0' && (glyph_raw[1] == 'x' || glyph_raw[1] == 'X')) {
    hex_start = glyph_raw + 2;
  }

  /* Check if remaining string is a valid hex number (1-6 hex digits for Unicode). */
  const size_t hex_len = strlen(hex_start);
  if (hex_len == 0 || hex_len > 6) {
    return false;
  }

  /* Verify all characters are hex digits. */
  for (size_t i = 0; i < hex_len; i++) {
    if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
      return false;
    }
  }

  /* Validate Unicode codepoint range. */
  const unsigned int val = strtoul(hex_start, nullptr, 16);
  if (val < 32 || val > 0x10FFFF) {
    return false;
  }

  /* Reject invisible/format codepoints (zero-width, BOM, tags, unassigned, ...) so a tab cannot
   * become visually nameless. */
  if (!is_display_glyph_codepoint(val)) {
    return false;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Lookup Utilities
 * \{ */

const char *find_panel_label_for_category(ARegion *region, const char *category)
{
  if (!region || !region->runtime || !region->runtime->type || !category) {
    return nullptr;
  }

  for (const PanelType &pt : region->runtime->type->paneltypes) {
    if (pt.category && STREQ(pt.category, category)) {
      const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
      if (panel_label && panel_label[0]) {
        return panel_label;
      }
    }
  }
  return nullptr;
}

bool extract_leading_glyph(const char *input,
                           char *glyph_hex_out,
                           size_t glyph_hex_max,
                           char *text_out,
                           size_t text_max)
{
  if (!input || !input[0]) {
    glyph_hex_out[0] = '\0';
    text_out[0] = '\0';
    return false;
  }

  /* Get first UTF-8 character */
  const int first_char_len = BLI_str_utf8_size_safe(input);
  if (first_char_len <= 0) {
    glyph_hex_out[0] = '\0';
    BLI_strncpy(text_out, input, text_max);
    return false;
  }

  /* Extract first character as codepoint */
  unsigned int codepoint = BLI_str_utf8_as_unicode_safe(input);

  /* Check if it's a display glyph */
  if (!is_display_glyph_codepoint(codepoint)) {
    glyph_hex_out[0] = '\0';
    BLI_strncpy(text_out, input, text_max);
    return false;
  }

  /* Convert glyph to hex */
  BLI_snprintf(glyph_hex_out, glyph_hex_max, "%x", codepoint);

  /* Skip the glyph character and any following space */
  const char *rest = input + first_char_len;
  while (*rest == ' ') {
    rest++;
  }

  BLI_strncpy(text_out, rest, text_max);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Popup Callbacks
 * \{ */

void category_tab_edit_popup_cancel_cb(bContext *C, void *user_data)
{
  wmOperator *op = static_cast<wmOperator *>(user_data);

  char category[64];
  RNA_string_get(op->ptr, "category", category);

  CategoryTabDialogSnapshot snapshot;
  category_tab_dialog_snapshot_read(op->ptr, snapshot);

  if (snapshot.space_type == -1) {
    ScrArea *area = CTX_wm_area(C);
    snapshot.space_type = area ? area->spacetype : -1;
  }

  /* Global-First: Always use GLOBAL space_type (-1) for override.
   * The restore_override_space_type from snapshot is used only for lookup fallback. */
  const int restore_override_space_type = -1;  // Always GLOBAL

  wmWindowManager *wm = CTX_wm_manager(C);

  /* Cancel must restore, not destroy: deliberately do NOT remove space-specific overrides here.
   * The dialog-open snapshot only captures GLOBAL data, so deleting per-space overrides on cancel
   * would lose user data that cancel is supposed to keep intact. (Commit paths use
   * category_tab_remove_stale_space_specific_overrides() for the Global-First migration.) */

  /* Find current override (may have been created by live preview) */
  CategoryGlyphItem *item = category_glyph_item_find(
      wm->category_glyph_overrides, category, restore_override_space_type);

  if (snapshot.has_override) {
    /* There was an override before - restore original values */
    if (!item) {
      /* Override was deleted by live preview - recreate it with GLOBAL space_type */
      item = category_glyph_item_ensure(
          wm->category_glyph_overrides, category, restore_override_space_type);
    }
    category_tab_dialog_snapshot_apply_to_item(*item, snapshot);
  }
  else {
    /* There was no override before - remove any created by live preview */
    if (item) {
      BLI_remlink(&wm->category_glyph_overrides, item);
      MEM_delete(item);
    }

  /* Reset operator can temporarily modify category_glyph_mappings (for example when resetting
   * a fallback-letter category to an empty glyph or toggling icon mode). On cancel, restore
   * runtime visuals in mappings from dialog-open snapshot for both space-specific and GLOBAL
   * entries, otherwise next dialog reopen may pick stale first-letter mode from fallback chain. */
  CategoryGlyphItem *map_item_exact = category_glyph_item_find(
      wm->category_glyph_mappings, category, snapshot.space_type);
  if (map_item_exact) {
    category_tab_dialog_snapshot_apply_runtime_visuals_to_item(*map_item_exact, snapshot);
  }

  if (snapshot.space_type != -1) {
    CategoryGlyphItem *map_item_global = category_glyph_item_find(
        wm->category_glyph_mappings, category, -1);
    if (map_item_global && map_item_global != map_item_exact) {
      category_tab_dialog_snapshot_apply_runtime_visuals_to_item(*map_item_global, snapshot);
    }
  }
  }

  /* Clear dialog operator pointer and popup block */
  category_tab_edit_dialog_clear_runtime_state(true);

  /* Record popup close time and category to prevent immediate reopen */
  category_tab_edit_dialog_mark_closed(category);

#ifdef WITH_PYTHON
  /* Restore tags in Python _glyph_cache to revert live preview changes. */
  if (category[0] != '\0') {
    wmWindowManager *wm_ptr = CTX_wm_manager(C);
    if (wm_ptr) {
      const int space_type = snapshot.space_type;

      PointerRNA wm_ptr_rna = RNA_pointer_create_discrete(&wm_ptr->id, RNA_WindowManager, wm_ptr);
      RNA_string_set(&wm_ptr_rna, "category_tab_save_category", category);

      /* Restore tags + glyph snapshot via the centralized Python bridge (escaping handled there). */
      category_py_restore_on_cancel(C,
                                    snapshot.tags,
                                    snapshot.glyph_hex,
                                    snapshot.glyph_mode,
                                    snapshot.color,
                                    space_type,
                                    snapshot.icon_source,
                                    snapshot.icon.key,
                                    snapshot.icon.path,
                                    snapshot.icon.provider);
    }
  }
#endif

  /* Show info message that changes were discarded */
  WM_global_report(RPT_INFO, "Category tab changes discarded");

  WM_main_add_notifier(NC_WINDOW, nullptr);
}

void category_tab_edit_popup_ok_cb(bContext *C, void *user_data, int /*retval*/)
{
  /* Save data to Python cache before clearing runtime state */
  if (user_data) {
    wmOperator *op = static_cast<wmOperator *>(user_data);
    char category[64];
    RNA_string_get(op->ptr, "category", category);
    
    /* Get icon state for saving */
    CategoryTabIconState icon_state;
    category_tab_icon_state_read(op->ptr, icon_state);
    
    int icon_source = RNA_enum_get(op->ptr, "icon_source");
    
#ifdef WITH_PYTHON
    /* Save to Python cache via the centralized bridge (update_tag). */
    if (category[0] != '\0') {
      category_py_update_tag_icon(C, category, icon_state.key, icon_source);
    }
#endif
    
    category_tab_edit_dialog_mark_closed(category);
  }
  
  /* Clear dialog operator pointer and popup block */
  category_tab_edit_dialog_clear_runtime_state(true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Popup Block Creation
 * \{ */

/** Map the popup-local tag filter mode (0 = all tags) to a #CategoryTagMode bit. */
static uint32_t category_tab_popup_local_filter_mode_flag()
{
  switch (category_tab_popup_local_filter_mode) {
    case 1:
      return uint32_t(CategoryTagMode::OBJECT_MODE);
    case 2:
      return uint32_t(CategoryTagMode::EDIT_MODE);
    case 3:
      return uint32_t(CategoryTagMode::SCULPT_MODE);
    case 4:
      return uint32_t(CategoryTagMode::VERTEX_PAINT);
    case 5:
      return uint32_t(CategoryTagMode::WEIGHT_PAINT);
    case 6:
      return uint32_t(CategoryTagMode::TEXTURE_PAINT);
    case 7:
      return uint32_t(CategoryTagMode::UV_EDIT);
    case 8:
      return uint32_t(CategoryTagMode::POSE_MODE);
    case 9:
      return uint32_t(CategoryTagMode::GEOMETRY_NODES);
    case 10:
      return uint32_t(CategoryTagMode::SHADER_EDITOR);
    default:
      return 0;
  }
}

/**
 * Populate the dialog's "display_name" (and possibly "glyph") operator properties with sensible
 * defaults when the name is empty. Extracted from #category_tab_edit_block_create.
 */
static void category_tab_edit_resolve_default_display_name(bContext *C,
                                                           wmOperator *op,
                                                           const char *category)
{
  char display_name[32] = "";
  RNA_string_get(op->ptr, "display_name", display_name);
  if (display_name[0] != '\0') {
    return;
  }

  /* Extract any leading glyph from category name (shouldn't happen, but be safe). */
  char extracted_glyph[16];
  char clean_name[64];
  if (extract_leading_glyph(
          category, extracted_glyph, sizeof(extracted_glyph), clean_name, sizeof(clean_name)))
  {
    /* If clean_name is empty after extraction, find panel label. */
    if (clean_name[0] == '\0') {
      ARegion *ctx_region = CTX_wm_region(C);
      const char *panel_label = find_panel_label_for_category(ctx_region, category);
      if (panel_label) {
        STRNCPY(clean_name, panel_label);
      }
    }
    RNA_string_set(op->ptr, "display_name", clean_name);
    /* Also set the extracted glyph if glyph field is empty. */
    char current_glyph[16];
    RNA_string_get(op->ptr, "glyph", current_glyph);
    if (current_glyph[0] == '\0') {
      RNA_string_set(op->ptr, "glyph", extracted_glyph);
    }
  }
  else {
    /* Check if category is a single glyph - find panel label. */
    if (is_single_glyph_str(category)) {
      ARegion *ctx_region = CTX_wm_region(C);
      const char *panel_label = find_panel_label_for_category(ctx_region, category);
      if (panel_label) {
        RNA_string_set(op->ptr, "display_name", panel_label);
      }
      else {
        RNA_string_set(op->ptr, "display_name", category);
      }
    }
    else {
      RNA_string_set(op->ptr, "display_name", category);
    }
  }
}

/**
 * Draw the "Change the display" panel (glyph / custom icon / text mode selector and inputs).
 * Extracted from #category_tab_edit_block_create.
 */
static void category_tab_edit_draw_icon_panel(bContext *C,
                                              Layout &layout,
                                              wmOperator *op,
                                              const char *category)
{
  PanelLayout icon_panel = layout.panel(C, "change_icon", false);

  if (icon_panel.header) {
    icon_panel.header->label(IFACE_("Change the display"), ICON_NONE);
  }

  if (!icon_panel.body) {
    return;
  }

  const int display_mode_ui = RNA_enum_get(op->ptr, "display_mode_ui");
  const bool show_glyph_inputs = (display_mode_ui == 0); /* GLYPH */
  const bool show_custom_icon_options = (display_mode_ui == 1); /* CUSTOM_ICON */
  const bool show_text_mode_hint = (display_mode_ui == 2); /* TEXT */

  Layout &mode_row = icon_panel.body->row(false);
  mode_row.prop(op->ptr, "display_mode_ui", ITEM_R_EXPAND, std::nullopt, ICON_NONE);

  icon_panel.body->separator();

  /* Preview is always visible; search/code are visible only in Glyph mode. */
  Layout &col_glyph = icon_panel.body->column(false);
  uiTemplateGlyphSelectorWithCallback(&col_glyph,
                                      C,
                                      op->ptr,
                                      "glyph",
                                      "glyph_search",
                                      "color",
                                      category,
                                      true,
                                      show_glyph_inputs,
                                      show_glyph_inputs,
                                      glyph_more_glyphs_button_cb,
                                      op,
                                      glyph_search_result_button_cb);

  if (show_glyph_inputs) {
    /* Validate glyph input and show warning if invalid. */
    char glyph_raw_check[16] = "";
    RNA_string_get(op->ptr, "glyph", glyph_raw_check);
    const bool glyph_valid = validate_glyph_hex_input(glyph_raw_check);
    if (!glyph_valid && glyph_raw_check[0] != '\0') {
      col_glyph.label("Invalid hex code (use 1-6 hex digits, e.g., e5d2)", ICON_ERROR);
    }
  }

  col_glyph.separator();

  if (show_custom_icon_options) {
    const int custom_icon_mode_ui = RNA_enum_get(op->ptr, "custom_icon_mode_ui");
    const bool use_blender_icon = (custom_icon_mode_ui == 0);

    Layout &icon_mode_row = col_glyph.row(false);
    icon_mode_row.prop(op->ptr, "custom_icon_mode_ui", ITEM_R_EXPAND, std::nullopt, ICON_NONE);
    col_glyph.separator();

    if (use_blender_icon) {
      Layout &icon_select_row = col_glyph.row(true);
      icon_select_row.alignment_set(LayoutAlign::Center);

      Layout &icon_key_col = icon_select_row.column(false);
      icon_key_col.ui_units_x_set(18.0f);
      Layout &icon_key_readonly = icon_key_col.row(false);
      icon_key_readonly.enabled_set(false);
      icon_key_readonly.prop(op->ptr, "icon_key", UI_ITEM_NONE, IFACE_("Blender"), ICON_NONE);

      /* More icons button */
      Block *icon_actions_block = icon_select_row.block();
      block_layout_set_current(icon_actions_block, &icon_select_row);
      char icon_btn_glyph[8] = "";
      process_glyph_input("f02f", icon_btn_glyph, sizeof(icon_btn_glyph));
      Button *more_icons_but = uiDefBut(icon_actions_block,
                                        ButtonType::But,
                                        icon_btn_glyph,
                                        0,
                                        0,
                                        UI_UNIT_X * 1.5f,
                                        UI_UNIT_Y,
                                        nullptr,
                                        0,
                                        0,
                                        std::nullopt);
      more_icons_but->tip_quick_func = [](const Button *) { return "More icons"; };
      button_func_set(more_icons_but, icon_more_icons_button_cb, op, nullptr);
    }
    else {
      char custom_icon_path[1024] = "";
      RNA_string_get(op->ptr, "icon_path", custom_icon_path);
      const char *custom_icon_display = (custom_icon_path[0] != '\0') ? custom_icon_path : "None";

      Layout &custom_icon_row = col_glyph.row(true);
      custom_icon_row.alignment_set(LayoutAlign::Center);

      Layout &custom_path_col = custom_icon_row.column(false);
      custom_path_col.ui_units_x_set(18.0f);
      Layout &readonly_path_row = custom_path_col.row(false);
      readonly_path_row.enabled_set(false);
      readonly_path_row.label(IFACE_("Custom"), ICON_NONE);

      Block *readonly_path_block = readonly_path_row.block();
      block_layout_set_current(readonly_path_block, &readonly_path_row);
      uiDefBut(readonly_path_block,
               ButtonType::But,
               custom_icon_display,
               0,
               0,
               UI_UNIT_X * 18.0f,
               UI_UNIT_Y,
               nullptr,
               0,
               0,
               std::nullopt);

      custom_icon_row.op("SCREEN_OT_category_tab_pick_custom_icon", "", ICON_FILE_FOLDER);
      custom_icon_row.op("SCREEN_OT_category_tab_reload_custom_icon", "", ICON_FILE_REFRESH);
    }
  }
  else if (show_text_mode_hint) {
    col_glyph.label(IFACE_("Text mode: icon override disabled"), ICON_INFO);
  }
}

/**
 * Draw the "Color" panel (theme color presets + custom color picker).
 * Extracted from #category_tab_edit_block_create.
 */
static void category_tab_edit_draw_color_panel(bContext *C, Layout &layout, wmOperator *op)
{
  PanelLayout color_panel = layout.panel(C, "glyph_color", false);

  if (color_panel.header) {
    color_panel.header->label(IFACE_("Color"), ICON_NONE);
  }

  if (!color_panel.body) {
    return;
  }

  Layout &col_color = color_panel.body->column(false);

  /* Centered row for preset buttons and custom color picker */
  Layout &presets_row = col_color.row(true);
  presets_row.alignment_set(LayoutAlign::Center);
  presets_row.emboss_set(EmbossType::Pulldown);

  /* Get block for creating buttons */
  Block *block = presets_row.block();
  block_layout_set_current(block, &presets_row);

  /* Get operator type */
  wmOperatorType *ot = WM_operatortype_find("SCREEN_OT_category_tab_color_preset", false);
  bTheme *btheme = theme::theme_get();

  /* Create button for each color preset (9 buttons: NONE + 8 colors) */
  for (int i = 0; i < 9; i++) {
    const int preset = i - 1;  /* -1 to 7 */

    Button *but = uiDefButO_ptr(block,
                                    ButtonType::But,
                                    ot,
                                    wm::OpCallContext::ExecDefault,
                                    (i == 0) ? "" : "\xEE\xA6\x97",
                                    0,
                                    0,
                                    UI_UNIT_X * 1.5f,
                                    UI_UNIT_Y,
                                    std::nullopt);

    if (i == 0) {
      def_but_icon(but, ICON_X, UI_HAS_ICON);
      but->col[0] = 0;
      but->col[1] = 0;
      but->col[2] = 0;
      but->col[3] = 0;
      but->drawflag &= ~BUT_TEXT_USE_COL;
    }
    else {
      const ThemeGlyphColor *glyph_color = &btheme->glyph_color[preset];
      button_color_set(but, glyph_color->color);
      but->drawflag |= BUT_TEXT_USE_COL;
    }

    /* Set operator properties */
    PointerRNA *op_ptr = button_operator_ptr_ensure(but);
    RNA_int_set(op_ptr, "preset", preset);
  }

  /* Custom color picker - minimal size, immediately after presets */
  presets_row.separator();
  Layout &picker_col = presets_row.column(false);
  picker_col.ui_units_x_set(1.0f);
  picker_col.prop(op->ptr, "color", ITEM_R_ICON_ONLY, "", ICON_NONE);
}

Block *category_tab_edit_block_create(bContext *C, ARegion *region, void *user_data)
{
  wmOperator *op = static_cast<wmOperator *>(user_data);
  const uiStyle *style = style_get_dpi();

  /* Get space_type from context for category lookup */
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;

  /* Calculate dialog width - increased for better visibility and tag grid */
  const int dialog_width = 450 * UI_SCALE_FAC;

  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);
  block_flag_disable(block, BLOCK_LOOP);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);
  popup_dummy_panel_set(region, block, op->idname);

  /* Keep popup open while editing - important for live preview */
  block_flag_enable(block, BLOCK_KEEP_OPEN | BLOCK_NUMSELECT);

  /* Store block pointer for Save button to close popup */
  category_tab_popup_block = block;

  /* Set up live update callback - this is the key for instant preview */
  block_func_handle_set(block, category_tab_edit_live_update_cb, op);

  /* Set active operator for the block - enables color preset buttons to find this operator */
  block_set_active_operator(block, op, false);

  /* Create layout */
  Layout &layout = block_layout(block,
                                         LayoutDirection::Vertical,
                                         LayoutType::Panel,
                                         0,
                                         0,
                                         dialog_width,
                                         0,
                                         0,
                                         style);

  /* Title */
  uiItemL_ex(&layout, IFACE_("Edit Category Tab"), ICON_NONE, true, false);
  layout.separator(0.2f, LayoutSeparatorType::Line);
  layout.separator(0.5f);

  /* Get category for button wiring */
  char category[64];
  RNA_string_get(op->ptr, "category", category);

  /* Get window manager for checking reserved categories */
  wmWindowManager *wm = CTX_wm_manager(C);

  /* Category name field - resolve a sensible default display name when empty. */
  category_tab_edit_resolve_default_display_name(C, op, category);

  /* Check if category is reserved (from DEFAULT_CATEGORY_GLYPHS).
   * Reserved categories cannot have their display name changed. */
  bool is_reserved = category_is_reserved(wm, category);

  /* Properties layout with split for consistent alignment */
  Layout &col_props = layout.column(false);
  col_props.use_property_split_set(true);

  /* Category Name - check if reserved for read-only */
  if (is_reserved) {
    /* Reserved categories: show field as read-only (disabled) */
    Layout &row_name = col_props.row(false);
    row_name.enabled_set(false);
    row_name.prop(op->ptr, "display_name", UI_ITEM_NONE, IFACE_("Category Name"), ICON_NONE);
  }
  else {
    col_props.prop(op->ptr, "display_name", UI_ITEM_NONE, IFACE_("Category Name"), ICON_NONE);
  }
 
  layout.separator();

  /* Change Icon panel (glyph / custom icon / text mode). */
  category_tab_edit_draw_icon_panel(C, layout, op, category);

  /* Color panel (presets + custom picker). */
  category_tab_edit_draw_color_panel(C, layout, op);

  layout.separator();

  /* Tags section in a sub-panel - only show for non-reserved categories.
   * Filter mode comes from the popup's local state (0 = all tags), separate from
   * wm->category_tag_filter_mode to prevent cross-UI interference. */
  const uint32_t filter_mode_flag = category_tab_popup_local_filter_mode_flag();

  /* Get all active tags for the header (unfiltered) */
  const Vector<CategoryTagUIRecord> tags_data_header = get_tags_for_category_ui(
      wm, category, 0, space_type);

  /* Get filtered tags for the body list */
  const Vector<CategoryTagUIRecord> tags_data_body = get_tags_for_category_ui(
      wm, category, filter_mode_flag, space_type);

  /* Don't show tags panel for reserved categories */
  if (!is_reserved) {
    /* Create unique panel idname per category so each category has its own collapse state */
    char tags_panel_idname[128];
    SNPRINTF(tags_panel_idname, "tags_list_%s", category);

    /* Check if category has any active tags assigned */
    const char *category_tags_string = category_tags_string_lookup(wm, category, space_type);
    const bool category_has_tags = (category_tags_string && category_tags_string[0] != '\0');

    /* Persistent state for Tags list panel across popup sessions.
     * Once the user expands the panel in any popup, it stays expanded
     * for all future popup openings in the current session. */
    static bool g_tags_list_panel_expanded = false;

    /* Use stored state if user previously expanded; otherwise default:
     * closed if category has tags, open if no tags. */
    const bool panel_default_closed = g_tags_list_panel_expanded ? false : category_has_tags;

    PanelLayout tags_panel = layout.panel(C, tags_panel_idname, panel_default_closed);

    /* Detect user expansion: if panel was supposed to be closed by default
     * but body exists, the user expanded it — remember for future popups. */
    if (tags_panel.body && panel_default_closed) {
      g_tags_list_panel_expanded = true;
    }

    /* Add label, active tag glyphs, and "New Tag" button to panel header */
    if (tags_panel.header) {
      tags_panel.header->label(IFACE_("Tags list"), ICON_NONE);

      /* Create split layout: left side for glyphs, right side for buttons */
      Layout &header_split = tags_panel.header->split(0.48f, false); /* 65% for glyphs, 35% for buttons */
      Layout &glyphs_section = header_split.column(false);
      Layout &buttons_section = header_split.column(false);
      buttons_section.alignment_set(LayoutAlign::Right);

      /* Show active tags as colored glyph buttons in header */
      if (!tags_data_header.is_empty()) {
        Layout &glyphs_row = glyphs_section.row(true);
        glyphs_row.alignment_set(LayoutAlign::Left); /* Left align to use full available width */

        for (const CategoryTagUIRecord &tag : tags_data_header) {
          /* Only show colored glyph for active tags */
          if (tag.name[0] != '\0' && tag.is_active && tag.glyph[0] != '\0') {
            /* Determine what to display: icon takes priority over glyph if icon_source is ICON (1).
             * This ensures that when a tag is switched to 'Icon' mode, it renders correctly
             * in the Tags List header (using standard Label icon rendering). */
            int draw_icon_id = ICON_NONE;
            const char *draw_text = tag.glyph;
            if (tag.icon_source == 1 && tag.icon_id > 0) {
              draw_icon_id = tag.icon_id;
              draw_text = ""; /* Don't show glyph if icon is set. */
            }

            /* Create colored glyph label or icon button. */
            Block *block = glyphs_row.block();
            block_layout_set_current(block, &glyphs_row);
            Button *glyph_but = uiDefBut(block,
                                              ButtonType::Label,
                                              draw_text,
                                              0,
                                              0,
                                              UI_UNIT_X,
                                              UI_UNIT_Y,
                                              nullptr,
                                              0,
                                              0,
                                              std::nullopt);

            /* Set icon if available (Labels support icons via def_but_icon). */
            if (draw_icon_id > 0) {
              def_but_icon(glyph_but, draw_icon_id, UI_HAS_ICON);
            }

            /* Set up full tooltip for tag glyph (shows tag name + filter modes) */
            TagTooltipData *tooltip_data = new TagTooltipData();
            tooltip_data->tag_name = tag.name;
            tooltip_data->mode_flags = category_tag_get_mode_flags(wm, tag.name);
            button_func_tooltip_set(
                glyph_but, tag_glyph_tooltip_func, tooltip_data, tag_tooltip_data_free);

            /* Set color if available */
            if (tag.has_color) {
              uchar color_uchar[4];
              color_uchar[0] = uchar(tag.color[0] * 255.0f);
              color_uchar[1] = uchar(tag.color[1] * 255.0f);
              color_uchar[2] = uchar(tag.color[2] * 255.0f);
              color_uchar[3] = 255;
              button_color_set(glyph_but, color_uchar);
            }
          }
        }
      }

      /* Add buttons to the right section */
      Layout &buttons_row = buttons_section.row(true);
      PointerRNA new_tag_ptr = buttons_row.op("wm.category_tag_create", IFACE_("New tag"), ICON_ADD);
      RNA_string_set(&new_tag_ptr, "category", category);

      buttons_row.separator();

      /* Filter menu button - opens popup with Current Mode / All Tags toggle buttons */
      buttons_row.menu("SCREEN_MT_category_tag_filter_toggle", "", ICON_FILTER);

      buttons_row.separator();

      /* Button to open Preferences in Tags section */
      PointerRNA prefs_ptr = buttons_row.op("SCREEN_OT_userpref_show", "", ICON_PREFERENCES);
      RNA_enum_set(&prefs_ptr, "section", USER_SECTION_TAGS);
    }

    if (tags_panel.body) {
      Layout &tags_body = *tags_panel.body;

      /* Show "Without Tag" only for categories that are truly unassigned in current context,
       * exactly like "New Add-ons!" filter logic. */
      const uint32_t current_mode_flag = get_current_tag_mode_flag(C);
      const CategoryGlyphItem *mapping_item = category_glyph_item_find_with_global_fallback_const(
          wm->category_glyph_mappings, category, space_type);
      const bool show_without_tag = category_is_unassigned_for_context(
          wm, mapping_item, space_type, current_mode_flag);

      if (!tags_data_body.is_empty() || show_without_tag) {
        /* Create centered container for the grid */
        Layout &centered_row = tags_body.row(false);
        centered_row.alignment_set(LayoutAlign::Center);

        /* Use grid_flow for automatic column wrapping (max 3 columns, row-major) */
        Layout &tags_grid = centered_row.grid_flow(true, 3, true, false, false);

        /* Add "Without Tag" button as the FIRST item in the grid if pending */
        if (show_without_tag) {
          bool without_tag_is_active = false;
          /* Use GLOBAL fallback to find override - Python always creates overrides with space_type=-1 */
          CategoryGlyphItem *override_item = category_glyph_item_find_with_global_fallback(
              wm->category_glyph_overrides, category, space_type);
          if (override_item) {
            /* Check without_tag_preview flag which is set when user selects "Without Tag"
             * in preview mode. We keep pending_tag_assignment=1 for "New Add-ons!" visibility,
             * so we can't use pending==0 as the check anymore. */
            without_tag_is_active = (override_item->tags[0] == '\0') &&
                                    (override_item->without_tag_preview == 1);
          }

          Layout &without_tag_item = tags_grid.row(true);
          without_tag_item.alignment_set(LayoutAlign::Left);

          Block *block = without_tag_item.block();
          block_layout_set_current(block, &without_tag_item);

          /* Create "Without Tag" button with same style as regular tag buttons */
          Button *without_tag_but = uiDefButTag(block,
                                                IFACE_("Without Tag"),
                                                "✕",  /* Simple X glyph for "no tag" */
                                                nullptr,  /* no custom color */
                                                without_tag_is_active,
                                                false,  /* is_pref_mode */
                                                false,  /* center_glyph */
                                                0, "",  /* icon_id, icon_path */
                                               0, 0,
                                               UI_UNIT_X * 8,
                                               UI_UNIT_Y * 1.5f,
                                               nullptr);

          /* Set operator for clearing tags */
          wmOperatorType *ot_clear = WM_operatortype_find("wm.category_clear_tags", false);
          if (ot_clear) {
            button_operator_set(without_tag_but, ot_clear, wm::OpCallContext::ExecDefault);
            PointerRNA *op_ptr = button_operator_ptr_ensure(without_tag_but);
            if (op_ptr) {
              RNA_string_set(op_ptr, "category", category);
              RNA_int_set(op_ptr, "space_type", space_type);
            }
          }
        }

        if (!tags_data_body.is_empty()) {
          for (const CategoryTagUIRecord &tag : tags_data_body) {
            if (tag.name[0] == '\0') {
              continue;
            }

            /* Create row layout for the tag button */
            Layout &tag_item = tags_grid.row(true);
            tag_item.alignment_set(LayoutAlign::Left);

            Block *block = tag_item.block();
            block_layout_set_current(block, &tag_item);

            /* Create unified Tag button with box container effect */
            Button *tag_but = uiDefButTag(block,
                                               IFACE_(tag.name),
                                               tag.glyph,
                                               tag.has_color ? tag.color : nullptr,
                                               tag.is_active,
                                               false,  /* is_pref_mode - toggle button with checkbox */
                                               false,  /* center_glyph - left align for category buttons */
                                               tag.icon_id, "",  /* icon_id from tag data, no custom path */
                                               0, 0,
                                               UI_UNIT_X * 8,
                                               UI_UNIT_Y * 1.5f,
                                               nullptr);

            /* Set operator properties */
            wmOperatorType *ot = WM_operatortype_find("wm.category_tag_toggle", false);
            if (ot) {
              button_operator_set(tag_but, ot, wm::OpCallContext::ExecDefault);
              PointerRNA *op_ptr = button_operator_ptr_ensure(tag_but);
              if (op_ptr) {
                RNA_string_set(op_ptr, "category", category);
                RNA_string_set(op_ptr, "tag_name", tag.name);
                RNA_int_set(op_ptr, "space_type", space_type);
              }
            }
          }
        } /* End of if (!tags_data_body.empty()) block */
      }
      else {
        tags_body.label(IFACE_("No tags. Click 'New tag' to create."), ICON_INFO);
      }
    }
  } /* End of !is_reserved check for tags panel */

  layout.separator();

  /* Buttons row: Reset | Save | Cancel */
  Layout &split = layout.split(0.15f, false);
  Layout &row_left = split.row(true);

  /* Reset button (left aligned, minimal width) */
  PointerRNA reset_ptr = row_left.op("SCREEN_OT_category_tab_reset", IFACE_("Reset"), ICON_LOOP_BACK);
  RNA_string_set(&reset_ptr, "category", category);

  /* Spacer and right-aligned buttons */
  Layout &row_right = split.row(true);
  row_right.separator_spacer();
  /* Save button (active/default, no icon) */
  row_right.active_default_set(true);
  row_right.op("SCREEN_OT_category_tab_edit_dialog_save", IFACE_("Save"), ICON_NONE);
  row_right.active_default_set(false);
  row_right.op("SCREEN_OT_category_tab_edit_dialog_cancel", IFACE_("Cancel"), ICON_NONE);

  /* Set block bounds - centered like the original dialog */
  block_bounds_set_centered(block, 6 * UI_SCALE_FAC);

  return block;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dialog Invoke and Exec
 * \{ */

bool category_tab_edit_poll(bContext *C)
{
  if (U.category_tabs_lock_edit) {
    return false;
  }
  return ED_operator_regionactive(C);
}

bool category_tab_try_auto_detect_extension_icon(bContext *C,
                                                   const char *category,
                                                   char r_icon_path[1024],
                                                   char r_icon_provider[128])
{
  r_icon_path[0] = '\0';
  r_icon_provider[0] = '\0';

#ifdef WITH_PYTHON
  if (!C || !category || category[0] == '\0') {
    return false;
  }

  // [POPULAR ADDONS DB] BEGIN - First try Popular Addons Database
  // Can be removed when extensions bundle their own icons.
  if constexpr (WITH_POPULAR_ADDONS_DATABASE) {
    if (category_tab_query_popular_addons_database(C, category, r_icon_path, r_icon_provider)) {
      return true;
    }
  }
  // [POPULAR ADDONS DB] END

  // Fallback to existing logic (original extension icon detection).
  // Query the JSON result from the centralized Python bridge and parse it here.
  const std::string json = category_py_auto_detect_extension_icon_json(C, category);
  if (json.empty()) {
    return false;
  }

  blender::Vector<std::string> parts;
  const bool parsed = category_tab_parse_json_string_array_minimal(json.c_str(), parts);
  bool detected = false;

  if (parsed && parts.size() >= 2) {
    /* The serialization layer already fully decoded these strings. */
    const std::string &icon_path = parts[0];
    const std::string &icon_provider = parts[1];

    if (!icon_path.empty()) {
      BLI_strncpy(r_icon_path, icon_path.c_str(), 1024);
      BLI_strncpy(r_icon_provider, icon_provider.c_str(), 128);
      detected = true;
    }
  }

  return detected;
#else
  UNUSED_VARS(C, category);
  return false;
#endif
}

wmOperatorStatus category_tab_edit_dialog_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  /* Check if dialog was just closed for the same category to prevent immediate reopen
   * caused by the same click that closed the popup. */
  if (category_tab_last_closed_category[0] != '\0') {
    double time_since_close = BLI_time_now_seconds() - category_tab_popup_close_time;
    if (time_since_close < 0.1) {
      /* Get category from mouse position to check if it matches */
      ARegion *region = CTX_wm_region(C);
      if (region && panel_category_tabs_is_visible(region)) {
        for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
          if (BLI_rcti_isect_pt(&pc_dyn.rect, event->mval[0], event->mval[1])) {
            if (STREQ(category_tab_last_closed_category, pc_dyn.idname)) {
              return OPERATOR_CANCELLED;
            }
            break;
          }
        }
      }
    }
  }

  /* Check if dialog is already open for the same category to prevent data corruption */
  if (category_tab_current_dialog_op) {
    char existing_category[64];
    RNA_string_get(category_tab_current_dialog_op->ptr, "category", existing_category);

    /* Get category from mouse position */
    ARegion *region = CTX_wm_region(C);
    if (region && panel_category_tabs_is_visible(region)) {
      const int mx = event->mval[0];
      const int my = event->mval[1];

      const char *clicked_category = nullptr;
      for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (BLI_rcti_isect_pt(&pc_dyn.rect, mx, my)) {
          clicked_category = pc_dyn.idname;
          break;
        }
      }

      /* If clicking on the same category that's already being edited, ignore */
      if (clicked_category && STREQ(existing_category, clicked_category)) {
        return OPERATOR_CANCELLED;
      }
    }
  }

  /* Get category from mouse position */
  ARegion *region = CTX_wm_region(C);
  if (!region || !panel_category_tabs_is_visible(region)) {
    return OPERATOR_CANCELLED;
  }

  /* Get space_type for per-space tag storage */
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;

  /* Find which tab the mouse is over */
  const int mx = event->mval[0];
  const int my = event->mval[1];

  const char *category = nullptr;
  for (const PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (BLI_rcti_isect_pt(&pc_dyn.rect, mx, my)) {
      category = pc_dyn.idname;
      break;
    }
  }

  if (!category) {
    return OPERATOR_CANCELLED;
  }

  /* Store category name and space_type in operator properties */
  RNA_string_set(op->ptr, "category", category);

  /* Check for existing override and populate properties */
  wmWindowManager *wm = CTX_wm_manager(C);

  /* NOTE: Do NOT reset wm->category_tag_filter_mode here.
   * This is a global preference used by the Preferences "Tags" UI-list to filter visible tags.
   * Previously, opening this popup would reset the filter to the current context mode,
   * causing confusing cross-UI synchronization where opening the edit popup would change
   * which tags are visible in Preferences.
   * The popup has its own "Filter Tags" menu (Current Mode / All Tags) that users can use
   * to control tag filtering within the popup without affecting the global preference.
   */

  /* Initialize popup's local filter mode to Current Mode by default.
   * This ensures the edit dialog always starts with mode-specific tag filtering enabled,
   * showing only tags relevant to the current object/editor mode.
   * Subsequent changes in the popup won't affect Preferences. */
  category_tab_popup_local_filter_mode = get_current_object_mode_filter_value(C);

  const CategoryTabInvokeLoadResult invoke_load = category_tab_invoke_load_operator_state_from_items(
      op->ptr, wm, region, category, space_type);
  const bool has_override = invoke_load.has_override;
  const bool user_glyph_override_assigned = invoke_load.user_glyph_override_assigned;
  const bool explicit_icon_mode_assigned = invoke_load.explicit_icon_mode_assigned;
  CategoryGlyphItem *override_item = invoke_load.override_item;

  category_tab_invoke_apply_post_load_defaults(C,
                                               op->ptr,
                                               wm,
                                               region,
                                               category,
                                               space_type,
                                               user_glyph_override_assigned,
                                               explicit_icon_mode_assigned);

  category_tab_invoke_build_and_store_snapshot(
      op->ptr, wm, category, space_type, has_override, override_item);

  /* Store pointer to dialog operator for Reset/Save button access */
  category_tab_current_dialog_op = op;

  /* Enable preview mode in Python to prevent mappings sync until Save.
   * This ensures tag button changes only affect overrides, not mappings.
   * Critical for categories that are already assigned (not in "New Add-ons!"). */
  category_py_set_preview_mode(C, true);

  /* Open custom popup with live preview support using public API */
  popup_block_ex(C,
                     category_tab_edit_block_create,
                     category_tab_edit_popup_ok_cb,
                     category_tab_edit_popup_cancel_cb,
                     op,
                     op);

  return OPERATOR_RUNNING_MODAL;
}

/**
 * Persist the edited category to the JSON store via the Python helper.
 *
 * NOTE: this is the Python bridge boundary for the save path; phase H4 of the refactor will move
 * the command construction into the centralized C++/Python bridge translation unit. The behavior
 * (set_category_data + finalize_category_tag_changes) is unchanged.
 */
static void category_tab_edit_save_to_json(bContext *C,
                                           wmOperator *op,
                                           const CategoryGlyphItem *item,
                                           const int space_type)
{
  /* Convert color to hex for Python set_category_data. */
  char color_hex[8];
  SNPRINTF(color_hex, "%02x%02x%02x",
           int(item->color[0] * 255.0f),
           int(item->color[1] * 255.0f),
           int(item->color[2] * 255.0f));

  /* Read glyph hex from operator properties (UI input), not from the override, so the user's
   * input is saved even if it matches the default. */
  char glyph_hex[16] = "";
  char glyph_raw[16] = "";
  RNA_string_get(op->ptr, "glyph", glyph_raw);
  if (glyph_raw[0] != '\0') {
    STRNCPY(glyph_hex, glyph_raw);
  }

  const char *icon_source_py = "auto";
  switch (item->icon_source) {
    case 0:
      icon_source_py = "auto";
      break;
    case 1:
      icon_source_py = "manual";
      break;
    case 2:
      icon_source_py = "off";
      break;
    default:
      icon_source_py = "auto";
      break;
  }

  char category[64];
  RNA_string_get(op->ptr, "category", category);

  /* Command construction + escaping live in the centralized Python bridge. */
  category_py_save_category_data(C,
                                 category,
                                 item->display_name,
                                 category_tab_preview_first_letter,
                                 glyph_hex,
                                 color_hex,
                                 icon_source_py,
                                 item->icon_key,
                                 item->icon_path,
                                 item->icon_provider,
                                 (item->glyph_mode == 1) ? "first_letter" : "auto",
                                 space_type);
}

/**
 * After a save, if there are no more unassigned categories and the "New Add-ons!" filter is
 * active, deactivate it and restore filtering from the active category (or disable tag filtering
 * for "Without Tag"). Extracted from #category_tab_edit_dialog_exec.
 */
static void category_tab_deactivate_new_addon_filter_if_done(bContext *C,
                                                             wmWindowManager *wm,
                                                             ScrArea *area,
                                                             const char *category,
                                                             const int space_type)
{
  const uint32_t current_mode_flag = get_current_tag_mode_flag(C);
  const bool has_unassigned = should_show_new_addon_tag(wm, space_type, current_mode_flag);

  if (has_unassigned || !is_new_addon_filter_active(area)) {
    return;
  }

  /* No more unassigned categories - deactivate "New Add-ons!" filter. */
  set_new_addon_filter_active(area, false);
  set_saved_tag_filter_tags(area, "");

  /* Also reset tag_bar_manually_hidden so future auto-show can work */
  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    if (area && area->spacetype == SPACE_NODE) {
      SpaceNode *snode = CTX_wm_space_node(C);
      if (snode && snode->tag_bar_manually_hidden) {
        snode->tag_bar_manually_hidden = false;
        printf("[CATEGORY_TAB_EDIT] Reset tag_bar_manually_hidden - no more unassigned\n");
      }
    }
  }

  ARegion *region_ui = area ? BKE_area_find_region_type(area, RGN_TYPE_UI) : nullptr;
  const char *active_category = region_ui ? panel_category_active_get(region_ui, false) : nullptr;
  if (!active_category || active_category[0] == '\0') {
    active_category = category;
  }

  const char *active_category_tags = category_tags_string_lookup(wm, active_category, space_type);

  TagFilterStateRef state{};
  if (tag_filter_state_from_area(area, &state) && state.active_tags && state.filter_enabled) {
    Vector<std::string> active_tag_list;
    if (active_category_tags && active_category_tags[0] != '\0') {
      category_tab_split_tags(active_category_tags, active_tag_list, ",;");
    }

    if (!active_tag_list.is_empty() && !active_tag_list[0].empty()) {
      BLI_strncpy(state.active_tags, active_tag_list[0].c_str(), 256);
      *state.filter_enabled = 1;
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY_TAB_EDIT] No more unassigned categories, switched to tag '%s'\n",
               active_tag_list[0].c_str());
      }
    }
    else {
      /* "Without Tag": disable tag filtering and keep active tab unchanged. */
      state.active_tags[0] = '\0';
      *state.filter_enabled = 0;
      if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
        printf("[CATEGORY_TAB_EDIT] No more unassigned categories, disabled tag filtering\n");
      }
    }
  }

  if (region_ui && active_category && active_category[0] != '\0') {
    panel_category_active_set_safe(C, region_ui, active_category, false);
  }
}

/**
 * After a save, switch the active tag to the category's first assigned tag, but ONLY when the
 * category was truly unassigned before the save (it was in "New Add-ons!") and just received its
 * first tag. For already-assigned categories the active tag must stay unchanged.
 * Extracted from #category_tab_edit_dialog_exec.
 */
static void category_tab_switch_to_assigned_tag_after_save(bContext *C,
                                                           wmWindowManager *wm,
                                                           ScrArea *area,
                                                           const char *category,
                                                           const int space_type,
                                                           const bool was_pending_before_save)
{
  const char *saved_category_tags = category_tags_string_lookup(wm, category, space_type);
  if (!(was_pending_before_save && saved_category_tags && saved_category_tags[0] != '\0')) {
    return;
  }

  /* Category has tags assigned - switch to the first assigned tag */
  Vector<std::string> assigned_tag_list;
  category_tab_split_tags(saved_category_tags, assigned_tag_list, ",;");

  if (assigned_tag_list.is_empty() || assigned_tag_list[0].empty()) {
    return;
  }

  TagFilterStateRef state{};
  if (tag_filter_state_from_area(area, &state) && state.active_tags && state.filter_enabled) {
    /* Switch to the first assigned tag */
    BLI_strncpy(state.active_tags, assigned_tag_list[0].c_str(), 256);
    *state.filter_enabled = 1;

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[CATEGORY_TAB_EDIT] Category '%s' saved with tag, switched to tag '%s'\n",
             category, assigned_tag_list[0].c_str());
    }

    /* Ensure the category tab is visible and active */
    ARegion *region_ui = area ? BKE_area_find_region_type(area, RGN_TYPE_UI) : nullptr;
    if (region_ui) {
      panel_category_active_set_safe(C, region_ui, category, false);
    }
  }
}

wmOperatorStatus category_tab_edit_dialog_exec(bContext *C, wmOperator *op)
{
  char category[64];
  RNA_string_get(op->ptr, "category", category);

  /* Global-First: Always use GLOBAL space_type (-1) for override.
   * space_type from context is still used for lookup defaults. */
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;
  const int override_space_type = -1;  // Always GLOBAL for override

  char display_name[32];
  RNA_string_get(op->ptr, "display_name", display_name);

  char glyph_raw[16];
  RNA_string_get(op->ptr, "glyph", glyph_raw);

  /* Validate glyph input: must be empty or valid hex code (1-6 hex digits). */
  if (!validate_glyph_hex_input(glyph_raw)) {
    BKE_report(op->reports, RPT_ERROR, "Glyph Code must be 1-6 hex digits (e.g., e5d2)");
    return OPERATOR_CANCELLED;
  }

  /* Process glyph input: convert hex code (e.g., "e5d2") to UTF-8 character */
  char glyph[8];
  glyph[0] = '\0';
  process_glyph_input(glyph_raw, glyph, sizeof(glyph));

  float color[3];
  RNA_float_get_array(op->ptr, "color", color);

  /* Get or create override with GLOBAL space_type */
  wmWindowManager *wm = CTX_wm_manager(C);

  category_tab_remove_stale_space_specific_overrides(wm, category, "EXEC SAVE");

  CategoryGlyphItem *item = category_glyph_item_ensure(
      wm->category_glyph_overrides, category, override_space_type);

  /* Resolve base/default glyph from stable mappings first (canonical category key),
   * and only then use lookup as a fallback.
   *
   * Rationale:
   * - `panel_category_glyph_lookup()` is runtime-oriented and can be influenced by live override state.
   * - Save logic must compare against a stable baseline (mapping default), otherwise custom glyphs can be
   *   incorrectly treated as default and cleared.
   */
  char default_glyph[8] = "";
  const bool default_glyph_found = category_tab_resolve_default_glyph_from_mappings(
      wm, category, space_type, default_glyph);

  if (!default_glyph_found) {
    /* Fallback for categories not present in mappings yet.
     * Temporarily clear live override glyph to avoid self-matching. */
    bool is_fallback = false;
    const char *default_glyph_lookup = category_tab_lookup_runtime_default_glyph(
        wm, category, space_type, item, true, &is_fallback, nullptr);
    if (default_glyph_lookup) {
      STRNCPY(default_glyph, default_glyph_lookup);
    }
  }

  /* Update values */
  STRNCPY(item->display_name, display_name);
  copy_v3_v3(item->color, color);
  const int display_mode_ui_exec = RNA_enum_get(op->ptr, "display_mode_ui");
  const int custom_icon_mode_ui_exec = RNA_enum_get(op->ptr, "custom_icon_mode_ui");
  char icon_key_before[128] = "";
  RNA_string_get(op->ptr, "icon_key", icon_key_before);
  const int resolved_icon_source_exec = category_tab_resolve_icon_source(
      display_mode_ui_exec,
      custom_icon_mode_ui_exec,
      RNA_enum_get(op->ptr, "icon_source"),
      CategoryTabIconSourceResolveMode::Commit,
      nullptr);
  CategoryTabIconState icon_state_exec;
  category_tab_icon_state_read(op->ptr, icon_state_exec);
  category_tab_icon_state_apply(*item, icon_state_exec);
  if (g_tag_filter_debug_enabled) {
    printf("[ICON SAVE DEBUG] display_mode_ui=%d, custom_icon_mode_ui=%d, icon_source_before=%d, icon_key='%s'\n",
           display_mode_ui_exec, custom_icon_mode_ui_exec, RNA_enum_get(op->ptr, "icon_source"), icon_key_before);
    printf("[ICON SAVE DEBUG] resolved_icon_source=%d\n", resolved_icon_source_exec);
    printf("[ICON SAVE DEBUG] icon_state_exec.key='%s', path='%s', provider='%s'\n",
           icon_state_exec.key, icon_state_exec.path, icon_state_exec.provider);
    printf("[ICON SAVE DEBUG] After apply: item->icon_key='%s', icon_source=%d\n",
           item->icon_key, item->icon_source);
  }
  RNA_enum_set(op->ptr, "icon_source", resolved_icon_source_exec);
  item->icon_source = resolved_icon_source_exec;
  item->glyph_mode = (display_mode_ui_exec == 2) ? 1 : 0;

  /* Only save glyph to override if user has changed it from the default.
   * If glyph matches the default (especially fallback letters), leave it empty
   * so that the lookup function will return the correct is_fallback status.
   */
  if (glyph[0] != '\0' && default_glyph[0] != '\0' && !STREQ(glyph, default_glyph)) {
    /* User has set a custom glyph different from default */
    STRNCPY(item->glyph, glyph);
  }
  else if (glyph[0] != '\0' && default_glyph[0] == '\0') {
    /* No resolved default glyph (rare edge-case) - keep explicit user glyph. */
    STRNCPY(item->glyph, glyph);
  }
  else {
    /* Glyph is same as default or empty - clear override glyph to preserve
     * fallback letter detection in the drawing code. */
    item->glyph[0] = '\0';
  }

  /* Keep persisted mapping in WM synchronized for immediate runtime consistency.
   * Rationale: draw code can fall back to mappings in several paths; if mapping keeps
   * stale glyph_mode=AUTO while override state is transient/filtered, reserved tabs can
   * still render the old glyph instead of first letter until a full Python resync. */
  for (CategoryGlyphItem *map_item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
       map_item;
       map_item = static_cast<CategoryGlyphItem *>(map_item->next))
  {
    if (map_item->space_type != space_type || !STREQ(map_item->category, category)) {
      continue;
    }
    map_item->glyph_mode = item->glyph_mode;
    map_item->icon_source = item->icon_source;
    break;
  }

  /* Clear dialog operator pointer and preview button */
  category_tab_edit_dialog_clear_runtime_state(false);

  /* Capture pending state BEFORE Python save modifies it.
   * finalize_category_tag_changes sets pending_tag_assignment=0 in mappings,
   * so we must read it now to know if the category was in "New Add-ons!". */
  const CategoryGlyphItem *mapping_pre_save =
      category_glyph_item_find_with_global_fallback_const(
          wm->category_glyph_mappings, category, space_type);
  const bool was_pending_before_save = (mapping_pre_save != nullptr &&
                                         mapping_pre_save->pending_tag_assignment != 0);

  /* Save updated data to JSON (including tags which might have been modified).
   * Note: Python side uses timers to defer heavy operations (json.dumps off main thread),
   * so this call returns quickly without blocking the UI. */
  category_tab_edit_save_to_json(C, op, item, space_type);

  /* After saving, check if there are still unassigned categories.
   * If not and "New Add-ons!" filter is active, deactivate it and restore
   * filtering from the currently active category (or disable tag filtering
   * for "Without Tag"). */
  category_tab_deactivate_new_addon_filter_if_done(C, wm, area, category, space_type);

   /* After saving, switch to the assigned tag ONLY when the category was truly
    * unassigned (pending_tag_assignment=true in mappings, i.e. in "New Add-ons!")
    * and just received its first tag. For already-assigned categories
    * (pending_tag_assignment=false), the active tag must remain unchanged —
    * switching it would break the normal editing workflow.
    *
    * Uses the pending state captured BEFORE the Python save (was_pending_before_save):
    * finalize_category_tag_changes has already set pending_tag_assignment=0 in the mapping,
    * so reading it now would always return false. */
  category_tab_switch_to_assigned_tag_after_save(
      C, wm, area, category, space_type, was_pending_before_save);

  /* Redraw after Python save to update all UI elements */
  WM_main_add_notifier(NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Centered Popup Operator
 *
 * Wrapper operator that opens another operator's properties in a centered popup.
 * This solves the issue where invoke_props_dialog in Python always positions
 * the popup at mouse location instead of center.
 * \{ */

/**
 * Open a centered popup dialog for the given operator.
 * This is a wrapper around WM_operator_props_dialog_popup that ensures
 * the popup is always centered regardless of message parameter.
 */
static wmOperatorStatus centered_popup_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  char op_idname[64];
  RNA_string_get(op->ptr, "operator_idname", op_idname);

  /* Get the target operator type */
  wmOperatorType *ot = WM_operatortype_find(op_idname, true);
  if (!ot) {
    return OPERATOR_CANCELLED;
  }

  /* Get popup width from wrapper properties */
  int width = RNA_int_get(op->ptr, "width");

  /* Create a temporary operator instance with default properties.
   * This will show in the dialog for user to fill in. */
  PointerRNA *props_ptr = nullptr;
  IDProperty *properties = nullptr;
  WM_operator_properties_alloc(&props_ptr, &properties, ot->idname);
  WM_operator_properties_sanitize(props_ptr, false);

  /* Preserve invoke-time defaults for operators opened through this wrapper.
   *
   * `WM_operator_props_dialog_popup` shows operator properties directly and does not run the
   * target operator's `invoke()`. `wm.category_tag_create` initializes a default glyph in its
   * Python `invoke()` (`DEFAULT_TAG_GLYPH_HEX = "e866"`), so when opened via this wrapper the
   * glyph/preview starts empty unless we seed the property here.
   *
   * Keep this targeted to avoid changing behavior of unrelated operators. */
  if (STREQ(op_idname, "wm.category_tag_create")) {
    PropertyRNA *glyph_prop = RNA_struct_find_property(props_ptr, "glyph");
    if (glyph_prop && RNA_property_type(glyph_prop) == PROP_STRING) {
      char glyph_value[16] = "";
      RNA_property_string_get(props_ptr, glyph_prop, glyph_value);
      if (glyph_value[0] == '\0') {
        RNA_property_string_set(props_ptr, glyph_prop, "e866");
      }
    }

    PropertyRNA *glyph_search_prop = RNA_struct_find_property(props_ptr, "glyph_search");
    if (glyph_search_prop && RNA_property_type(glyph_search_prop) == PROP_STRING) {
      RNA_property_string_set(props_ptr, glyph_search_prop, "");
    }
    
    /* Copy category property from wrapper operator if provided */
    PropertyRNA *category_prop = RNA_struct_find_property(props_ptr, "category");
    if (category_prop && RNA_property_type(category_prop) == PROP_STRING) {
      PropertyRNA *src_category_prop = RNA_struct_find_property(op->ptr, "category");
      if (src_category_prop && RNA_property_type(src_category_prop) == PROP_STRING) {
        char category_value[64] = "";
        RNA_property_string_get(op->ptr, src_category_prop, category_value);
        if (category_value[0] != '\0') {
          RNA_property_string_set(props_ptr, category_prop, category_value);
        }
      }
    }
  }

  /* Create operator for the popup dialog */
  wmOperator *target_op = MEM_new<wmOperator>(__func__);
  target_op->type = ot;
  target_op->ptr = props_ptr;
  target_op->properties = properties;
  target_op->reports = op->reports;
  target_op->flag = OP_IS_INVOKE;

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[CENTERED_POPUP DEBUG] Creating popup for operator: %s\n", ot->idname);
    printf("[CENTERED_POPUP DEBUG]   target_op=%p, idname='%s'\n", (void*)target_op, target_op->idname);
  }

  /* Call WM_operator_props_dialog_popup with a dummy message to force centering.
   * The function centers the popup when message is non-empty (line 1881 in wm_operators.cc):
   * data->position = (message) ? WM_POPUP_POSITION_CENTER : WM_POPUP_POSITION_MOUSE;
   *
   * Note: The popup system takes ownership of target_op on success (OPERATOR_RUNNING_MODAL),
   * so we don't need to clean up in that case.
   */
  wmOperatorStatus ret = WM_operator_props_dialog_popup(
      C,
      target_op,
      width,
      std::nullopt,  // title - use operator's default
      std::nullopt,  // confirm_text
      false,         // cancel_default
      std::string(" ")  // dummy message to force centering (single space)
  );

  /* Clean up only on failure - popup system takes ownership on success */
  if (ret != OPERATOR_RUNNING_MODAL) {
    MEM_delete(target_op);
    /* Clean up properties on failure since popup system won't take them */
    if (properties) {
      IDP_FreeProperty(properties);
    }
    /* Note: props_ptr is allocated by WM_operator_properties_alloc using MEM_new<PointerRNA>,
     * so we need to use MEM_delete to free it. */
    if (props_ptr) {
      MEM_delete(props_ptr);
    }
  }

  return ret;
}

static void CENTERED_OT_popup_operator_wrapper(wmOperatorType *ot)
{
  ot->name = "Centered Popup Operator";
  ot->idname = "WM_OT_centered_popup_operator_wrapper";
  ot->description = "Wrapper that opens an operator in a centered popup dialog";

  ot->invoke = centered_popup_invoke;

  RNA_def_string(
      ot->srna,
      "operator_idname",
      nullptr,
      64,
      "Operator ID Name",
      "The idname of the operator to open in popup (e.g., 'wm.category_tag_create')"
  );

  RNA_def_int(
      ot->srna,
      "width",
      350,
      100,
      1000,
      "Width",
      "Popup width in pixels",
      100,
      1000
  );
  
  /* Category property for wm.category_tag_create */
  RNA_def_string(
      ot->srna,
      "category",
      nullptr,
      64,
      "Category",
      "Category to assign the tag to (passed to wrapped operator)"
  );
}

void centered_popup_operator_register()
{
  WM_operatortype_append(CENTERED_OT_popup_operator_wrapper);
}

/** \} */

}  // namespace blender::ui
