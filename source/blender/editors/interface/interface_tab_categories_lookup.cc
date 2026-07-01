/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tabs - category lookup: resolve a category's glyph, color, icon,
 * display name and tooltip from `wm.category_glyph_overrides` and
 * `wm.category_glyph_mappings`.
 *
 * Split out of interface_tab_categories.cc: the internal category-lookup
 * helpers plus the public glyph / first-letter / color / icon-data / base-source
 * / display-name / tooltip lookup functions. Behavior is unchanged; the
 * remaining call sites in interface_tab_categories.cc (tag utilities, etc.)
 * resolve these through interface_tab_categories_intern.hh.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "BLF_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "interface_intern.hh"
#include "interface_tag_bar.hh"
#include "regions/interface_regions_intern.hh"

#include "interface_category_py_bridge.hh"
#include "interface_tab_categories_intern.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

/* Forward declaration: normalize_category_key() is defined below (Block B) but used by the
 * lookup helpers in Block A above it. */
static std::string normalize_category_key(const char *category);

/* -------------------------------------------------------------------- */
/** \name Internal Category Lookup Helpers
 * \{ */

/**
 * Internal helper: Find category glyph item with exact match (category + space_type).
 * Returns first matching item or nullptr if not found.
 */
static bool category_item_match_exact(const CategoryGlyphItem *item,
                                      const char *category,
                                      int space_type)
{
  return item && category && STREQ(item->category, category) && item->space_type == space_type;
}

static bool category_item_match_normalized(const CategoryGlyphItem *item,
                                           const std::string &normalized_target,
                                           int space_type)
{
  if (!item || normalized_target.empty()) {
    return false;
  }
  const std::string normalized_item = normalize_category_key(item->category);
  return normalized_item == normalized_target && item->space_type == space_type;
}

static const CategoryGlyphItem *category_item_find_exact_any_space(const ListBase *list,
                                                                   const char *category)
{
  if (!list || !category || !category_glyph_list_is_valid(list)) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category)) {
      return item;
    }
  }

  return nullptr;
}

/* Iterate a CategoryGlyphItem ListBase and return the first item for which `match` returns true,
 * or nullptr. Centralizes the list-validity guard and the node iteration shared by the lookup
 * helpers below; each helper supplies only its match predicate (and any space-type guard). */
template<typename MatchFn>
static const CategoryGlyphItem *category_glyph_list_find(const ListBase *list, MatchFn &&match)
{
  if (!list || !category_glyph_list_is_valid(list)) {
    return nullptr;
  }

  for (const CategoryGlyphItem *item = static_cast<const CategoryGlyphItem *>(list->first);
       item;
       item = static_cast<const CategoryGlyphItem *>(item->next))
  {
    if (match(item)) {
      return item;
    }
  }
  return nullptr;
}

static const CategoryGlyphItem *category_glyph_item_find_exact(const ListBase *list,
                                                               const char *category,
                                                               int space_type)
{
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return category_item_match_exact(item, category, space_type);
  });
}

/**
 * Internal helper: Find category glyph item with global fallback (space_type = -1).
 * Only searches global if space_type != -1.
 */
static const CategoryGlyphItem *category_glyph_item_find_global(const ListBase *list,
                                                                const char *category,
                                                                int space_type)
{
  if (space_type == -1) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return STREQ(item->category, category) && item->space_type == -1;
  });
}

/**
 * Internal helper: Find category glyph item with normalized exact match.
 * Uses normalize_category_key() for canonicalization fallback.
 */
static const CategoryGlyphItem *category_glyph_item_find_normalized_exact(const ListBase *list,
                                                                          const char *category,
                                                                          int space_type)
{
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return category_item_match_normalized(item, normalized_target, space_type);
  });
}

/**
 * Internal helper: Find category glyph item with normalized global fallback.
 * Uses normalize_category_key() for canonicalization with global space_type = -1.
 */
static const CategoryGlyphItem *category_glyph_item_find_normalized_global(const ListBase *list,
                                                                           const char *category,
                                                                           int space_type)
{
  if (space_type == -1) {
    return nullptr;
  }
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *item) {
    return category_item_match_normalized(item, normalized_target, -1);
  });
}

static const CategoryGlyphItem *category_glyph_item_find_global_only(const ListBase *list,
                                                                     const char *category)
{
  if (!category) {
    return nullptr;
  }

  /* First pass: exact global (space_type == -1) match. */
  if (const CategoryGlyphItem *item = category_glyph_list_find(
          list, [&](const CategoryGlyphItem *it) {
            return it->space_type == -1 && STREQ(it->category, category);
          }))
  {
    return item;
  }

  /* Second pass: normalized global match. */
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *it) {
    return it->space_type == -1 && category_item_match_normalized(it, normalized_target, -1);
  });
}

/**
 * Internal helper: Unified category lookup with priority order.
 * Searches in order: exact -> global -> normalized exact -> normalized global.
 * Keep this order stable: edit/live-preview/reset paths rely on the same resolution contract.
 */
static const CategoryGlyphItem *category_glyph_item_find_with_fallback(const ListBase *list,
                                                                        const char *category,
                                                                        int space_type)
{
  if (!list || !category) {
    return nullptr;
  }

  /* First pass: try exact match (category + space_type). */
  if (const CategoryGlyphItem *item = category_glyph_item_find_exact(list, category, space_type)) {
    return item;
  }

  /* Second pass: try global categories (space_type = -1) if not searching for global already. */
  if (const CategoryGlyphItem *item = category_glyph_item_find_global(list, category, space_type)) {
    return item;
  }

  /* Third pass: try canonicalization fallback with space_type match. */
  if (const CategoryGlyphItem *item = category_glyph_item_find_normalized_exact(list, category, space_type)) {
    return item;
  }

  /* Fourth pass: try canonicalization fallback with global categories. */
  if (const CategoryGlyphItem *item = category_glyph_item_find_normalized_global(list, category, space_type)) {
    return item;
  }

  return nullptr;
}

const CategoryGlyphItem *category_item_find_overrides(const wmWindowManager *wm,
                                                      const char *category,
                                                      int space_type)
{
  if (!wm) {
    return nullptr;
  }
  return category_glyph_item_find_with_fallback(&wm->category_glyph_overrides, category, space_type);
}

const CategoryGlyphItem *category_item_find_mappings(const wmWindowManager *wm,
                                                     const char *category,
                                                     int space_type)
{
  if (!wm) {
    return nullptr;
  }
  UNUSED_VARS(space_type);
  return category_glyph_item_find_global_only(&wm->category_glyph_mappings, category);
}

static bool category_item_override_icon_is_effective(const CategoryGlyphItem *item)
{
  if (!item) {
    return false;
  }

  const bool has_icon_payload = (item->icon_key[0] != '\0') || (item->icon_path[0] != '\0') ||
                                (item->icon_provider[0] != '\0');
  const bool has_explicit_icon_mode = ELEM(
      item->icon_source, CATEGORY_TAB_ICON_SOURCE_MANUAL, CATEGORY_TAB_ICON_SOURCE_OFF);

  /* Override entries are often created for glyph/color/tag edits.
   * If an AUTO override has no icon payload, don't mask JSON mapping icon data. */
  return has_icon_payload || has_explicit_icon_mode;
}

/* Two-pass any-space lookup shared by the color/effective-icon helpers below. Pass one matches
 * the exact category name, pass two falls back to the normalized name; both require `pred(item)`.
 * Each caller supplies only the payload predicate that distinguishes it. */
template<typename PredFn>
static const CategoryGlyphItem *category_item_find_any_space_matching(const ListBase *list,
                                                                      const char *category,
                                                                      PredFn &&pred)
{
  if (!category) {
    return nullptr;
  }

  /* First pass: exact category-name match satisfying the predicate, in any space. */
  if (const CategoryGlyphItem *item = category_glyph_list_find(
          list, [&](const CategoryGlyphItem *it) {
            return STREQ(it->category, category) && pred(it);
          }))
  {
    return item;
  }

  /* Second pass: normalized-name fallback satisfying the predicate, in any space. */
  const std::string normalized_target = normalize_category_key(category);
  if (normalized_target.empty()) {
    return nullptr;
  }
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *it) {
    return pred(it) && category_item_match_normalized(it, normalized_target, it->space_type);
  });
}

static const CategoryGlyphItem *category_item_find_with_color_any_space(const ListBase *list,
                                                                        const char *category)
{
  return category_item_find_any_space_matching(
      list, category, [](const CategoryGlyphItem *item) { return !is_zero_v3(item->color); });
}

static const CategoryGlyphItem *category_item_find_with_effective_icon_any_space(
    const ListBase *list, const char *category)
{
  return category_item_find_any_space_matching(list, category, category_item_override_icon_is_effective);
}

static const CategoryGlyphItem *category_item_find_override_with_display_name(
    const ListBase *list, const char *category, int space_type)
{
  if (!category) {
    return nullptr;
  }

  /* First pass: exact space-type match that carries a display name. */
  if (const CategoryGlyphItem *item = category_glyph_list_find(
          list, [&](const CategoryGlyphItem *it) {
            return category_item_match_exact(it, category, space_type) && it->display_name[0] != '\0';
          }))
  {
    return item;
  }

  if (space_type == -1) {
    return nullptr;
  }

  /* Second pass: global (space_type == -1) fallback that carries a display name. */
  return category_glyph_list_find(list, [&](const CategoryGlyphItem *it) {
    return category_item_match_exact(it, category, -1) && it->display_name[0] != '\0';
  });
}

static const char *category_item_display_name_or_default_or_category(const CategoryGlyphItem *item,
                                                                     const char *category)
{
  if (!item) {
    return category;
  }
  if (item->display_name[0] != '\0') {
    return item->display_name;
  }
  if (item->default_display_name[0] != '\0') {
    return item->default_display_name;
  }
  return category;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Key Normalization & Mapping Find
 * \{ */

/**
 * Normalize category name for canonicalization-aware matching.
 * Mirrors Python's _normalize_category_key for ASCII input: removes non-alphanumeric ASCII
 * characters and converts to lowercase. Non-ASCII bytes are dropped, so results may differ from
 * the Python implementation for non-ASCII names.
 */
static std::string normalize_category_key(const char *category)
{
  if (!category) {
    return "";
  }

  std::string result;
  result.reserve(strlen(category));

  for (const char *c = category; *c; c++) {
    if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) {
      result += *c;
    }
    else if (*c >= 'A' && *c <= 'Z') {
      result += (*c + ('a' - 'A')); /* Convert to lowercase. */
    }
    /* Skip all other characters (spaces, punctuation, etc.). */
  }

  return result;
}

/**
 * Find category glyph mapping item with canonicalization fallback.
 * First tries exact match, then tries to match by normalized keys if no exact match found.
 */
static const char *panel_category_glyph_lookup_apply_fallback(const wmWindowManager *wm,
                                                              const char *category,
                                                              bool *r_is_fallback_letter,
                                                              int space_type);

/* Declared in interface_tab_categories_intern.hh (shared with the reserved-category file). */
const CategoryGlyphItem *category_glyph_mapping_find(const wmWindowManager *wm,
                                                     const char *category,
                                                     int space_type)
{
  return category_item_find_mappings(wm, category, space_type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Lookup Functions
 * \{ */

/* Forward declaration for glyph source lookup. */
const char *panel_category_display_name_lookup(const wmWindowManager *wm,
                                                      const char *category,
                                                      int space_type);
static const char *panel_category_base_source_lookup(const wmWindowManager *wm,
                                                     const char *category,
                                                     const PanelType *panel_type,
                                                     bool *r_is_reserved,
                                                     eCategoryGlyphBaseSource *r_source_type);

/**
 * Internal helper: Process a single CategoryGlyphItem from overrides.
 * Handles glyph_mode, fallback letter detection, and color extraction.
 */
static const char *process_override_glyph_item(const CategoryGlyphItem *item,
                                               const char *category,
                                               bool *r_is_fallback_letter,
                                               float r_color[3],
                                               bool *r_handled,
                                               const wmWindowManager *wm,
                                               int space_type)
{
  if (item->glyph_mode == CATEGORY_TAB_GLYPH_MODE_FIRST_LETTER) {
    if (r_color && !is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }
    if (r_is_fallback_letter) {
      *r_is_fallback_letter = true;
    }
    *r_handled = true;
    return nullptr;
  }

  if (item->glyph[0] != '\0') {
    /* For glyph_only categories (category name is a glyph), skip fallback letter check.
     * The glyph equals the category name, which would incorrectly be detected as fallback. */
    const bool is_glyph_only_category = category_name_is_glyph(category);
    const bool is_fallback_letter = is_glyph_only_category ?
                                        false :
                                        category_tab_glyph_is_fallback_letter(item->glyph, category);

    if (is_fallback_letter) {
      if (r_color && !is_zero_v3(item->color)) {
        copy_v3_v3(r_color, item->color);
      }
      /* Empty override used for tags only, keep searching mapping/defaults. */
      if (is_zero_v3(item->color) && item->display_name[0] == '\0') {
        return nullptr; /* Continue searching */
      }
      if (r_is_fallback_letter) {
        *r_is_fallback_letter = true;
      }
      *r_handled = true;
      return nullptr;
    }

    if (r_color && !is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }
    *r_handled = true;
    return item->glyph;
  }

  /* Override has no glyph: may be explicit clear OR tags/color-only carrier.
   * For text_only/glyph_text categories (default_glyph is empty), this means reset to first letter.
   * For glyph_only categories (default_glyph is set), continue to mappings to get default.
   *
   * Important for glyph-id categories (normalized key can be empty): without this,
   * color-only overrides are lost in live preview until full restart/resync.
   */
  if (r_color && !is_zero_v3(item->color)) {
    copy_v3_v3(r_color, item->color);
  }

  /* Check if this is a text_only/glyph_text category by looking at mappings.
   * If default_glyph is empty, reset should return first letter (nullptr), not mapping glyph. */
  if (const CategoryGlyphItem *map_item = category_glyph_mapping_find(wm, category, space_type)) {
    /* Treat as true text-only only when both glyph and default_glyph are empty. */
    if (map_item->default_glyph[0] == '\0' && map_item->glyph[0] == '\0') {
      /* Text-based category with empty override glyph = reset to first letter.
       * Set handled=true to prevent fallback to mappings which would return old glyph.
       * Set is_fallback_letter=true so draw code knows to use first letter. */
      if (r_is_fallback_letter) {
        *r_is_fallback_letter = true;
      }
      *r_handled = true;
      return nullptr;
    }
  }

  return nullptr; /* Continue to mappings */
}

static const char *panel_category_glyph_lookup_override(const wmWindowManager *wm,
                                                         const char *category,
                                                         bool *r_is_fallback_letter,
                                                         float r_color[3],
                                                         bool *r_handled,
                                                         int space_type)
{
  *r_handled = false;

  if (!wm) {
    return nullptr;
  }

  /* First try exact match, then normalized match using unified helper. */
  if (const CategoryGlyphItem *item = category_item_find_overrides(wm, category, space_type))
  {
    const char *result = process_override_glyph_item(
        item, category, r_is_fallback_letter, r_color, r_handled, wm, space_type);
    if (*r_handled || result) {
      return result;
    }
    /* Continue searching if process_override_glyph_item returned nullptr without setting handled */
  }

  return nullptr;
}

static const char *panel_category_glyph_lookup_mapping(const wmWindowManager *wm,
                                                       const char *category,
                                                       bool *r_is_fallback_letter,
                                                       float r_color[3],
                                                       bool *r_handled,
                                                       int space_type)
{
  *r_handled = false;

  const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type);
  if (!item) {
    return nullptr;
  }

  if (r_color && is_zero_v3(r_color) && !is_zero_v3(item->color)) {
    copy_v3_v3(r_color, item->color);
  }

  if (item->glyph_mode == CATEGORY_TAB_GLYPH_MODE_FIRST_LETTER) {
    if (r_is_fallback_letter) {
      *r_is_fallback_letter = true;
    }
    *r_handled = true;
    static char cached_letter[8];
    if (item->first_letter[0] != '\0') {
      STRNCPY(cached_letter, item->first_letter);
      return cached_letter;
    }
    return panel_category_glyph_lookup_apply_fallback(wm, category, r_is_fallback_letter, space_type);
  }

  /* For glyph_only categories (category name is a glyph), skip fallback letter check.
   * The glyph equals the category name, which would incorrectly be detected as fallback. */
  const bool is_glyph_only_category = category_name_is_glyph(category);

  if (item->glyph[0] != '\0') {
    if (is_glyph_only_category || !category_tab_glyph_is_fallback_letter(item->glyph, category)) {
      *r_handled = true;
      return item->glyph;
    }
  }

  if (item->default_glyph[0] != '\0') {
    if (is_glyph_only_category || !category_tab_glyph_is_fallback_letter(item->default_glyph, category)) {
      *r_handled = true;
      return item->default_glyph;
    }
  }

  return nullptr;
}

static const char *panel_category_glyph_lookup_apply_fallback(const wmWindowManager *wm,
                                                              const char *category,
                                                              bool *r_is_fallback_letter,
                                                              int space_type)
{
  if (r_is_fallback_letter) {
    /* If category itself is a glyph, don't treat it as fallback letter. */
    *r_is_fallback_letter = !category_name_is_glyph(category);
  }

  static char first_char_buf[8];
  const char *first_letter_source = panel_category_display_name_lookup(wm, category, space_type);
  if (!first_letter_source || first_letter_source[0] == '\0') {
    first_letter_source = category;
  }

  if (category_tab_first_utf8_char_copy(first_letter_source, first_char_buf, sizeof(first_char_buf))) {
    return first_char_buf;
  }
  return first_letter_source;
}

const char *panel_category_glyph_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        const PanelType *panel_type,
                                        bool *r_is_fallback_letter,
                                        float r_color[3],
                                        int space_type)
{
  /* Initialize outputs. */
  if (r_is_fallback_letter) {
    *r_is_fallback_letter = false;
  }
  /* Initialize color to black (use theme). */
  if (r_color) {
    zero_v3(r_color);
  }

  bool handled = false;
  if (const char *override_glyph = panel_category_glyph_lookup_override(
          wm, category, r_is_fallback_letter, r_color, &handled, space_type))
  {
    return override_glyph;
  }
  if (handled) {
    return nullptr;
  }

  if (const char *mapping_glyph = panel_category_glyph_lookup_mapping(
          wm, category, r_is_fallback_letter, r_color, &handled, space_type))
  {
    return mapping_glyph;
  }
  if (handled) {
    return nullptr;
  }

  /* 3. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_color && is_zero_v3(r_color)) {
      panel_category_color_lookup(wm, category, r_color);
    }
    return panel_type->icon_glyph;
  }

  return panel_category_glyph_lookup_apply_fallback(wm, category, r_is_fallback_letter, space_type);
}

bool panel_category_first_letter_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        int space_type,
                                        char r_letter[8])
{
  if (!wm || !r_letter) {
    return false;
  }

  r_letter[0] = '\0';
  const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type);
  if (!item || item->first_letter[0] == '\0') {
    return false;
  }

  BLI_strncpy(r_letter, item->first_letter, 8);
  return true;
}

void panel_category_color_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        float r_color[3])
{
  if (!r_color) {
    return;
  }

  zero_v3(r_color);

  if (!wm || !category) {
    return;
  }

  if (const CategoryGlyphItem *item = category_item_find_with_color_any_space(
          &wm->category_glyph_overrides, category))
  {
    copy_v3_v3(r_color, item->color);
    return;
  }

  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category)) {
    if (!is_zero_v3(item->color)) {
      copy_v3_v3(r_color, item->color);
    }
  }
}

bool panel_category_icon_data_lookup(const wmWindowManager *wm,
                                            const char *category,
                                            CategoryTabIconResolved *r_icon,
                                            int space_type = -1)
{
  if (!r_icon) {
    return false;
  }

  *r_icon = CategoryTabIconResolved{};

  const auto icon_data_copy_from_item = [&](const CategoryGlyphItem *item) {
    r_icon->source = item->icon_source;
    r_icon->key = item->icon_key;
    r_icon->path = item->icon_path;
    r_icon->provider = item->icon_provider;
  };

  /* 1) User overrides. */
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_with_effective_icon_any_space(
            &wm->category_glyph_overrides, category))
    {
      icon_data_copy_from_item(item);
      return true;
    }
  }

  /* 2) Global mappings from Python cache sync. Use space_type for proper space-specific lookup. */
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category, space_type)) {
    icon_data_copy_from_item(item);
    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Base Source Lookup
 * \{ */

static const char *panel_category_base_source_lookup(const wmWindowManager *wm,
                                                     const char *category,
                                                     const PanelType *panel_type,
                                                     bool *r_is_reserved,
                                                     eCategoryGlyphBaseSource *r_source_type)
{
  if (r_is_reserved) {
    *r_is_reserved = false;
  }
  if (r_source_type) {
    *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;
  }

  /* 1. Check global mappings (synced from Python DEFAULT_CATEGORY_GLYPHS). */
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category)) {
    if (r_is_reserved) {
      *r_is_reserved = (item->is_reserved != 0);
    }
    if (r_source_type) {
      *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_MAPPING;
    }
    if (item->glyph[0] != '\0') {
      return item->glyph;
    }
    if (item->default_glyph[0] != '\0') {
      return item->default_glyph;
    }
  }

  /* 2. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_source_type) {
      *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE;
    }
    return panel_type->icon_glyph;
  }

  /* 3. Fallback: return first character of category. */
  if (r_source_type) {
    *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;
  }
  static char first_char_buf[8];
  if (category_tab_first_utf8_char_copy(category, first_char_buf, sizeof(first_char_buf))) {
    return first_char_buf;
  }
  return category;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Display Name Lookup
 * \{ */

const char *panel_category_display_name_lookup(const wmWindowManager *wm,
                                                      const char *category,
                                                      int space_type)
{
  if (!category) {
    return "";
  }

  /* 1. Check user overrides first (prefer requested space, then GLOBAL fallback). */
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_override_with_display_name(
            &wm->category_glyph_overrides, category, space_type))
    {
      return item->display_name;
    }
  }

  /* 2. Check mappings with per-space lookup semantics. */
  if (wm) {
    if (const CategoryGlyphItem *mapped = category_glyph_mapping_find(wm, category, space_type)) {
      if (mapped->display_name[0] != '\0') {
        return mapped->display_name;
      }
    }
  }

  return category;
}

const char *category_first_letter_source_name_get(const ARegion *region,
                                                         const wmWindowManager *wm,
                                                         const char *category_id,
                                                         const char *category_id_draw,
                                                         int space_type)
{
  if (category_id_draw && category_id_draw[0] != '\0' && !is_single_glyph_str(category_id_draw)) {
    return category_id_draw;
  }

  if (category_id && is_single_glyph_str(category_id) && region && region->runtime &&
      region->runtime->type)
  {
    /* CRITICAL FIX: When multiple extensions use the same tag-category (e.g., "Animation"),
     * prioritize panels from the specific source_extension over generic panels.
     * This prevents MPFB "Add walk cycle" from being used for all Animation extensions. */
    const char *target_source_extension = nullptr;

    /* Try to get source_extension from category mappings for context-aware panel selection */
    if (const CategoryGlyphItem *mapping_item = category_item_find_mappings(wm, category_id, space_type)) {
      if (mapping_item->source_extension[0] != '\0') {
        target_source_extension = mapping_item->source_extension;
      }
    }

    /* If no mapping found, try overrides */
    if (!target_source_extension) {
      if (const CategoryGlyphItem *override_item = category_item_find_overrides(wm, category_id, space_type)) {
        if (override_item && override_item->source_extension[0] != '\0') {
          target_source_extension = override_item->source_extension;
        }
      }
    }

    /* TODO: First pass - find panel from specific extension context
     * Currently disabled until proper source_extension detection is implemented */
    /* if (target_source_extension) {
      for (const PanelType &pt : region->runtime->type->paneltypes) {
        if (pt.category && STREQ(pt.category, category_id)) {
          // TODO: Add proper source_extension matching logic here
          const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
          if (panel_label && panel_label[0] != '\0') {
            return panel_label;
          }
        }
      }
    } */

    /* Second pass: fallback to any panel (original behavior) */
    for (const PanelType &pt : region->runtime->type->paneltypes) {
      if (pt.category && STREQ(pt.category, category_id)) {
        const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
        if (panel_label && panel_label[0] != '\0') {
          return panel_label;
        }
      }
    }
  }

  const char *display_name = panel_category_display_name_lookup(wm, category_id, space_type);
  if (display_name && display_name[0] != '\0' && !is_single_glyph_str(display_name)) {
    return display_name;
  }

  return category_id;
}

const char *panel_category_tooltip_name_get(const ARegion *region,
                                             const wmWindowManager *wm,
                                             const char *category_idname)
{
  /* 1. Check user overrides first. If category is in overrides and has display_name, use it. */
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_exact_any_space(
            &wm->category_glyph_overrides, category_idname))
    {
      return category_item_display_name_or_default_or_category(item, category_idname);
    }
  }

  /* 2. Check global mappings. If category is in mappings and has display_name, use it. */
  bool found_in_mappings = false;
  if (wm) {
    if (const CategoryGlyphItem *item = category_item_find_exact_any_space(
            &wm->category_glyph_mappings, category_idname))
    {
      found_in_mappings = true;
      return category_item_display_name_or_default_or_category(item, category_idname);
    }
  }

  /* 3. For reserved categories (built-in Blender categories), use category name. */
  if (category_is_reserved(wm, category_idname)) {
    return category_idname;
  }

  /* 4. For categories NOT in mappings (not explicitly configured), look up panel label.
   * This handles special cases like "Script 3" where category name contains only a glyph. */
  if (!found_in_mappings) {
    /* CRITICAL FIX: When multiple extensions use the same tag-category (e.g., "Animation"),
     * prioritize panels from the specific source_extension over generic panels.
     * This prevents MPFB "Add walk cycle" from being used for all Animation extensions. */
    const char *target_source_extension = nullptr;

    /* Try to get source_extension from category mappings for context-aware panel selection */
    if (const CategoryGlyphItem *mapping_item = category_item_find_mappings(wm, category_idname, -1)) {
      if (mapping_item->source_extension[0] != '\0') {
        target_source_extension = mapping_item->source_extension;
      }
    }

    /* If no mapping found, try overrides */
    if (!target_source_extension) {
      if (const CategoryGlyphItem *override_item = category_item_find_overrides(wm, category_idname, -1)) {
        if (override_item && override_item->source_extension[0] != '\0') {
          target_source_extension = override_item->source_extension;
        }
      }
    }

    /* TODO: First pass - find panel from specific extension context
     * Currently disabled until proper source_extension detection is implemented */
    /* if (target_source_extension) {
      for (const PanelType &pt : region->runtime->type->paneltypes) {
        if (pt.category && STREQ(pt.category, category_idname)) {
          // TODO: Add proper source_extension matching logic here
          const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
          if (panel_label && panel_label[0]) {
            return panel_label;
          }
        }
      }
    } */

    /* Second pass: fallback to any panel (original behavior) */
    for (const PanelType &pt : region->runtime->type->paneltypes) {
      if (pt.category && STREQ(pt.category, category_idname)) {
        const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
        if (panel_label && panel_label[0]) {
          return panel_label;
        }
      }
    }
  }

  /* 5. Fallback to category name itself */
  return category_idname;
}

/** \} */

}  // namespace blender::ui
