/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tabs - Drawing and event handling for category tab panels.
 *
 * This module provides functionality for:
 * - Drawing category tabs with glyphs, text, and zoom support
 * - Handling mouse events (hover, click, drag)
 * - Drag & drop reordering of tabs
 * - Tooltips for category tabs
 * - Settings button for tab preferences
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
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
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "RNA_access.hh"
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

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

/* Forward declarations */
static bool category_name_is_glyph(const char *category_id);

/* -------------------------------------------------------------------- */
/** \name Constants & Macros
 * \{ */

#define TABS_PADDING_BETWEEN_FACTOR 4.0f
#define TABS_PADDING_TEXT_FACTOR 6.0f
#define TABS_GLYPH_TEXT_GAP_FACTOR 6.0f
#define TABS_SETTINGS_ICON "\ue5d3" /* Material Symbols: settings/gear */

/* Glyph darkening factor for inactive tabs (0.0 = no change, 1.0 = black). */
#define TABS_GLYPH_DARKEN_BASE 0.15f

/* Tab background brightening factors for inactive tabs (0.0 = no change, 1.0 = white). */
#define TABS_BG_BRIGHTEN_BASE 0.0f
#define TABS_BG_BRIGHTEN_HOVER 0.05f

/** \} */

/* -------------------------------------------------------------------- */
/** \name Enums
 * \{ */

enum eCategoryGlyphBaseSource {
  CATEGORY_GLYPH_BASE_SOURCE_MAPPING,
  CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE,
  CATEGORY_GLYPH_BASE_SOURCE_DEFAULT,
  CATEGORY_GLYPH_BASE_SOURCE_FALLBACK,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Default Glyph Mappings
 * \{ */

/* Default glyph mappings - Material Symbols font */
static const struct {
  const char *category;
  const char *glyph;
} default_glyph_mappings[] = {
    {"Item", "\ue8f4"},        /* visibility */
    {"View", "\ue417"},        /* visibility */
    {"Edit", "\ue3c9"},        /* edit */
    {"Tool", "\ue166"},        /* construction */
    {"Asset", "\ue2c7"},       /* folder */
    {"Options", "\ue8b8"},     /* settings */
    {"Animation", "\ue71b"},   /* motion_photos_on */
    {"Physics", "\ue3d4"},     /* science */
    {"World", "\ue88e"},       /* public */
    {"Material", "\ue429"},    /* palette */
    {"Modifiers", "\ue429"},   /* palette */
    {"Texture", "\ue40a"},     /* texture */
    {"Particles", "\ue3d4"},   /* science */
    {"Curve", "\ue148"},       /* timeline */
    {"Mesh", "\ue204"},        /* category */
    {"Object", "\ue8d4"},      /* select_all */
    {"Scene", "\ue8f9"},       /* dashboard */
    {"Render", "\ue439"},      /* photo_camera */
    {"Script", "\ue86f"},      /* terminal */
    {"Sound", "\ue3a1"},       /* speaker */
    {"Surface", "\ue76c"},     /* waves */
    {"Volume", "\ue2c8"},      /* folder_open */
    {"Constraints", "\ue8d2"}, /* rule */
    {"Data", "\ue23e"},        /* database */
    {"Node", "\ue1b8"},        /* account_tree */
    {nullptr, nullptr},
};

static const char *lookup_default_glyph(const char *category)
{
  for (int i = 0; default_glyph_mappings[i].category != nullptr; i++) {
    if (STREQ(default_glyph_mappings[i].category, category)) {
      return default_glyph_mappings[i].glyph;
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name List Validation Utilities
 * \{ */

/**
 * Check if a ListBase containing CategoryGlyphItem appears to be valid.
 * After file read, the list may contain garbage pointers from the old file's memory space.
 */
bool category_glyph_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true; /* Empty list is valid. */
  }

  const CategoryGlyphItem *first = static_cast<const CategoryGlyphItem *>(list->first);

  /* First item should have prev == nullptr. If not, list is corrupted. */
  if (first->prev != nullptr) {
    return false;
  }

  /* Check for obviously invalid next pointer (like -1 which is 0xFFFFFFFF...). */
  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }

  return true;
}

static bool workspace_category_order_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true; /* Empty list is valid. */
  }

  const WorkspaceCategoryOrder *first = static_cast<const WorkspaceCategoryOrder *>(list->first);

  if (first->prev != nullptr) {
    return false;
  }

  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }

  return true;
}

bool category_tag_list_is_valid(const ListBase *list)
{
  if (list == nullptr || list->first == nullptr) {
    return true;
  }

  const CategoryTagDef *first = static_cast<const CategoryTagDef *>(list->first);
  if (first->prev != nullptr) {
    return false;
  }
  if (first->next == reinterpret_cast<const void *>(static_cast<intptr_t>(-1))) {
    return false;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Utilities
 * \{ */

const char *category_tags_string_lookup(const wmWindowManager *wm, const char *category)
{
  if (wm == nullptr || category == nullptr) {
    return "";
  }

  if (category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        return item->tags;
      }
    }
  }

  if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        return item->tags;
      }
    }
  }
  return "";
}

bool category_has_tag(const char *tags_string, const char *tag_name)
{
  if (tags_string == nullptr || tags_string[0] == '\0') {
    return false;
  }
  if (tag_name == nullptr || tag_name[0] == '\0') {
    return false;
  }

  const size_t tag_name_len = strlen(tag_name);
  const char *p = tags_string;
  while (*p != '\0') {
    const char *start = p;
    while (*p != '\0' && *p != ';') {
      p++;
    }
    const size_t len = size_t(p - start);
    if (len == tag_name_len && STREQLEN(start, tag_name, len)) {
      return true;
    }
    if (*p == ';') {
      p++;
    }
  }
  return false;
}

bool tag_glyph_hex_to_utf8(const char *input, char r_utf8[8])
{
  r_utf8[0] = '\0';

  if (input == nullptr || input[0] == '\0') {
    return false;
  }

  const char *hex_str = input;
  if (hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
    hex_str += 2;
  }

  char *end = nullptr;
  const unsigned long code_point = strtoul(hex_str, &end, 16);
  if (end == hex_str || *end != '\0' || code_point > 0x10FFFFul) {
    return false;
  }

  const size_t len = BLI_str_utf8_from_unicode(uint(code_point), r_utf8, 8);
  return len != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Lookup Functions
 * \{ */

/* Forward declaration for glyph source lookup. */
static const char *panel_category_base_source_lookup(const wmWindowManager *wm,
                                                     const char *category,
                                                     const PanelType *panel_type,
                                                     bool *r_is_reserved,
                                                     eCategoryGlyphBaseSource *r_source_type);

const char *panel_category_glyph_lookup(const wmWindowManager *wm,
                                        const char *category,
                                        const PanelType *panel_type,
                                        bool *r_is_fallback_letter,
                                        float r_color[3])
{
  /* Initialize outputs. */
  if (r_is_fallback_letter) {
    *r_is_fallback_letter = false;
  }
  /* Initialize color to black (use theme). */
  if (r_color) {
    zero_v3(r_color);
  }

  /* 1. Check user overrides in wm->category_glyph_overrides. */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (item->glyph[0] != '\0') {
          /* Check if this is actually a fallback letter (first char of category).
           * A real glyph should be different from the category name or longer. */
          const int glyph_len = strlen(item->glyph);
          const int category_len = strlen(category);

          /* Check if glyph is a single Unicode character that matches the first char of category.
           * This indicates a fallback letter, not a custom glyph. */
          bool is_fallback_letter = false;
          if (glyph_len < category_len) {
            /* Glyph is shorter than category - might be a fallback letter */
            const uint glyph_code = BLI_str_utf8_as_unicode_safe(item->glyph);
            const uint category_code = BLI_str_utf8_as_unicode_safe(category);
            if (glyph_code == category_code && glyph_code != BLI_UTF8_ERR) {
              is_fallback_letter = true;
            }
          }

          if (is_fallback_letter) {
            /* Glyph is the first character of category - treat as fallback letter. */
            if (r_color && !is_zero_v3(item->color)) {
              copy_v3_v3(r_color, item->color);
            }
            /* If we found a fallback letter but NO color and NO display name override,
             * it might be an "empty" override created for tags. Continue searching
             * in mappings to find the real default color/glyph. */
            if (is_zero_v3(item->color) && item->display_name[0] == '\0') {
              continue;
            }
            if (r_is_fallback_letter) {
              *r_is_fallback_letter = true;
            }
            return nullptr;
          }
          if (r_color) {
            copy_v3_v3(r_color, item->color);
          }
          return item->glyph;
        }
        /* Override has no glyph - this means glyph was explicitly cleared OR
         * this is an "empty" override created just for tags. */
        if (item->glyph[0] == '\0') {
          if (r_color && !is_zero_v3(item->color)) {
            copy_v3_v3(r_color, item->color);
          }
          if (is_zero_v3(item->color) && item->display_name[0] == '\0') {
            /* No glyph, no color, no name - continue searching in mappings. */
            continue;
          }
          /* If we have an override with color/name but no glyph, we should CONTINUE
           * searching in mappings and default mappings to find a real glyph before
           * falling back to the first character. */
          continue;
        }
      }
    }
  }

  /* 2. Check global mappings in wm->category_glyph_mappings. */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (r_color && is_zero_v3(r_color) && !is_zero_v3(item->color)) {
          copy_v3_v3(r_color, item->color);
        }
        if (item->glyph[0] != '\0') {
          /* Check if this is actually a fallback letter. */
          const int category_first_char_size = BLI_str_utf8_size_safe(category);
          if (category_first_char_size > 0 &&
              STREQLEN(item->glyph, category, category_first_char_size) &&
              item->glyph[category_first_char_size] == '\0')
          {
            /* Glyph is the first character of category - treat as fallback letter.
             * Continue searching in mappings/defaults to find a real icon if possible. */
            continue;
          }
          return item->glyph;
        }
        /* No glyph in mapping - continue searching for default mappings or fallback. */
        continue;
      }
    }
  }

  /* 3. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_color && is_zero_v3(r_color)) {
      if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
        for (const CategoryGlyphItem *item =
                 static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
             item;
             item = static_cast<const CategoryGlyphItem *>(item->next))
        {
          if (STREQ(item->category, category) && !is_zero_v3(item->color)) {
            copy_v3_v3(r_color, item->color);
            break;
          }
        }
      }
      if (is_zero_v3(r_color) && wm &&
          category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
        for (const CategoryGlyphItem *item =
                 static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
             item;
             item = static_cast<const CategoryGlyphItem *>(item->next))
        {
          if (STREQ(item->category, category) && !is_zero_v3(item->color)) {
            copy_v3_v3(r_color, item->color);
            break;
          }
        }
      }
    }
    return panel_type->icon_glyph;
  }

  /* 4. Check static default mappings. */
  const char *glyph = lookup_default_glyph(category);
  if (glyph) {
    return glyph;
  }

  /* 5. Fallback: return first character of category. */
  if (r_is_fallback_letter) {
    /* If the category name itself is a glyph (high Unicode), don't treat it as a fallback letter.
     * This avoids adding extra_shift to these icons. */
    *r_is_fallback_letter = !category_name_is_glyph(category);
  }
  static char first_char_buf[8];
  const int char_size = BLI_str_utf8_size_safe(category);
  if (char_size > 0 && char_size < int(sizeof(first_char_buf))) {
    memcpy(first_char_buf, category, char_size);
    first_char_buf[char_size] = '\0';
    return first_char_buf;
  }
  return category;
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

  /* 1. Check global mappings (registered by Python DEFAULT_CATEGORY_GLYPHS) */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (r_is_reserved) {
          *r_is_reserved = true;
        }
        if (r_source_type) {
          *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_MAPPING;
        }
        return item->glyph;
      }
    }
  }

  /* 2. Check static default mappings */
  const char *glyph = lookup_default_glyph(category);
  if (glyph) {
    if (r_is_reserved) {
      *r_is_reserved = true;
    }
    if (r_source_type) {
      *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_DEFAULT;
    }
    return glyph;
  }

  /* 3. Check PanelType.icon_glyph. */
  if (panel_type && panel_type->icon_glyph && panel_type->icon_glyph[0]) {
    if (r_source_type) {
      *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_PANEL_TYPE;
    }
    return panel_type->icon_glyph;
  }

  /* 4. Fallback: return first character of category. */
  if (r_source_type) {
    *r_source_type = CATEGORY_GLYPH_BASE_SOURCE_FALLBACK;
  }
  static char first_char_buf[8];
  const int char_size = BLI_str_utf8_size_safe(category);
  if (char_size > 0 && char_size < int(sizeof(first_char_buf))) {
    memcpy(first_char_buf, category, char_size);
    first_char_buf[char_size] = '\0';
    return first_char_buf;
  }
  return category;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Display Name Lookup
 * \{ */

static const char *panel_category_display_name_lookup(const wmWindowManager *wm, const char *category)
{
  /* 1. Check user overrides first */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        break;
      }
    }
  }

  /* 2. Check global mappings */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        break;
      }
    }
  }

  return category;
}

const char *panel_category_tooltip_name_get(const ARegion *region,
                                             const wmWindowManager *wm,
                                             const char *category_idname)
{
  /* 1. Check user overrides first. If category is in overrides and has display_name, use it. */
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_overrides)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category_idname)) {
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        /* Category is in overrides but display_name is empty - use category name. */
        return category_idname;
      }
    }
  }

  /* 2. Check global mappings. If category is in mappings and has display_name, use it. */
  bool found_in_mappings = false;
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category_idname)) {
        found_in_mappings = true;
        if (item->display_name[0] != '\0') {
          return item->display_name;
        }
        /* Category is in mappings but display_name is empty - use category name. */
        return category_idname;
      }
    }
  }

  /* 3. For reserved categories (built-in Blender categories), use category name. */
  if (category_is_reserved(wm, category_idname)) {
    return category_idname;
  }

  /* 4. For categories NOT in mappings (not explicitly configured), look up panel label.
   * This handles special cases like "Script 3" where category name contains only a glyph. */
  if (!found_in_mappings) {
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

/* -------------------------------------------------------------------- */
/** \name String Utilities
 * \{ */

static bool is_single_glyph_str(const char *str)
{
  if (!str || !str[0]) {
    return false;
  }

  const int utf8_char_size = BLI_str_utf8_size_safe(str);
  const size_t len = BLI_strnlen(str, 64);

  /* Single ASCII character or single UTF-8 character. */
  return (len == 1) || (utf8_char_size > 0 && size_t(utf8_char_size) == len);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Utilities
 * \{ */

static bool set_glyph_color(const int fontid,
                            const float custom_color[3],
                            const bool is_active,
                            const unsigned char theme_col_text[3],
                            const unsigned char theme_col_text_sel[3],
                            unsigned char r_color[3])
{
  if (!is_zero_v3(custom_color)) {
    BLF_color3fv_alpha(fontid, custom_color, 1.0f);
    r_color[0] = uchar(custom_color[0] * 255);
    r_color[1] = uchar(custom_color[1] * 255);
    r_color[2] = uchar(custom_color[2] * 255);
    return true;
  }
  const unsigned char *col = is_active ? theme_col_text_sel : theme_col_text;
  BLF_color3ubv(fontid, col);
  r_color[0] = col[0];
  r_color[1] = col[1];
  r_color[2] = col[2];
  return false;
}

static void brighten_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] + (255 - color[i]) * factor);
  }
}

static void darken_color_3ub(uchar color[3], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = uchar(color[i] * (1.0f - factor));
  }
}

static void brighten_color_4fv(float color[4], const float factor)
{
  BLI_assert(factor >= 0.0f && factor <= 1.0f);

  for (int i = 0; i < 3; i++) {
    color[i] = color[i] + (1.0f - color[i]) * factor;
  }
  /* Alpha remains unchanged. */
}

static void apply_glyph_darkening(const int fontid, uchar color[3], const float darken_factor)
{
  if (darken_factor <= 0.0f) {
    return;
  }

  darken_color_3ub(color, darken_factor);
  BLF_color3ubv(fontid, color);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reserved Categories
 * \{ */

static bool category_name_is_glyph(const char *category_id)
{
  if (category_id == nullptr || category_id[0] == '\0') {
    return false;
  }

  /* Check if the category name is a single high Unicode character (glyph). */
  const size_t len = strlen(category_id);

  /* Single UTF-8 character that's a high Unicode glyph */
  if (len <= 4) {
    unsigned int codepoint = 0;
    if ((category_id[0] & 0x80) == 0) {
      /* ASCII - not a glyph */
      return false;
    }
    else if ((category_id[0] & 0xE0) == 0xC0) {
      /* 2-byte UTF-8 */
      codepoint = (category_id[0] & 0x1F) << 6;
      if (category_id[1]) {
        codepoint |= (category_id[1] & 0x3F);
      }
    }
    else if ((category_id[0] & 0xF0) == 0xE0) {
      /* 3-byte UTF-8 */
      codepoint = (category_id[0] & 0x0F) << 12;
      if (category_id[1]) {
        codepoint |= (category_id[1] & 0x3F) << 6;
      }
      if (category_id[2]) {
        codepoint |= (category_id[2] & 0x3F);
      }
    }
    else if ((category_id[0] & 0xF8) == 0xF0) {
      /* 4-byte UTF-8 */
      codepoint = (category_id[0] & 0x07) << 18;
      if (category_id[1]) {
        codepoint |= (category_id[1] & 0x3F) << 12;
      }
      if (category_id[2]) {
        codepoint |= (category_id[2] & 0x3F) << 6;
      }
      if (category_id[3]) {
        codepoint |= (category_id[3] & 0x3F);
      }
    }

    /* Check if codepoint is in Private Use Area */
    if (codepoint >= 0xE000 && codepoint <= 0xF8FF) {
      return true;
    }
  }

  return false;
}

bool category_is_reserved(const wmWindowManager * /*wm*/, const char *category_id)
{
  /* Categories with glyph names (high Unicode) are from addons and NOT reserved */
  if (category_name_is_glyph(category_id)) {
    return false;
  }

  /* Only check static default_glyph_mappings (built-in Blender categories). */
  if (lookup_default_glyph(category_id)) {
    return true;
  }

  return false;
}

static bool category_is_reserved_for_reorder(const wmWindowManager *wm, const char *category_id)
{
  return category_is_reserved(wm, category_id);
}

static int count_reserved_tabs_at_start(const wmWindowManager *wm, ARegion *region)
{
  int reserved_count = 0;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (category_is_reserved_for_reorder(wm, pc_dyn.idname)) {
      reserved_count++;
    }
    else {
      break;
    }
  }
  return reserved_count;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tag Mode Functions
 * \{ */

/**
 * Get the current object mode as a CategoryTagMode bitmask.
 */
uint32_t get_current_tag_mode_flag(const bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
  }

  switch (ob->mode) {
    case OB_MODE_OBJECT:
      return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
    case OB_MODE_EDIT:
      return static_cast<uint32_t>(CategoryTagMode::EDIT_MODE);
    case OB_MODE_SCULPT:
      return static_cast<uint32_t>(CategoryTagMode::SCULPT_MODE);
    case OB_MODE_VERTEX_PAINT:
      return static_cast<uint32_t>(CategoryTagMode::VERTEX_PAINT);
    case OB_MODE_WEIGHT_PAINT:
      return static_cast<uint32_t>(CategoryTagMode::WEIGHT_PAINT);
    case OB_MODE_TEXTURE_PAINT:
      return static_cast<uint32_t>(CategoryTagMode::TEXTURE_PAINT);
    case OB_MODE_POSE:
      return static_cast<uint32_t>(CategoryTagMode::POSE_MODE);
    default:
      return static_cast<uint32_t>(CategoryTagMode::OBJECT_MODE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Visibility by Tags
 * \{ */

/**
 * Check if the category passes the tag filter.
 * When multiple tags are active, category must have AT LEAST ONE of them (OR logic).
 */
static bool category_passes_tag_filter(const bContext *C, const char *category_idname)
{
  if (!C) {
    return true;
  }

  const wmWindowManager *wm = CTX_wm_manager(C);
  ScrArea *area = CTX_wm_area(C);

  /* Get active filter tags string from current space type */
  char active_tags[256] = "";

  if (area) {
    if (area->spacetype == SPACE_VIEW3D) {
      /* Tag bar is in View3D */
      View3D *v3d = static_cast<View3D *>(area->spacedata.first);
      if (v3d) {
        STRNCPY(active_tags, v3d->active_tag_filter_tags);
      }
    }
    else if (area->spacetype == SPACE_PROPERTIES) {
      /* Tag bar might also be in Properties (for future use) */
      SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
      if (sbuts) {
        STRNCPY(active_tags, sbuts->active_tag_filter_tags);
      }
    }
  }

  /* If filter is not active (empty string) - show all */
  if (active_tags[0] == '\0') {
    return true;
  }

  /* Get category tags */
  const char *category_tags = category_tags_string_lookup(wm, category_idname);

  /* If category has no tags - hide it (since filter is active) */
  if (!category_tags || category_tags[0] == '\0') {
    return false;
  }

  /* Check if ANY active tag is present in the category (OR logic) */
  /* Parse active_tags (comma-separated) and verify at least one tag exists in category_tags */

  const char *cursor = active_tags;
  char active_tag[64];

  while (*cursor) {
    /* Skip leading spaces */
    while (*cursor == ' ') {
      cursor++;
    }

    if (!*cursor) {
      break;
    }

    /* Extract tag name */
    int i = 0;
    while (*cursor && *cursor != ',' && *cursor != ';' && i < 63) {
      active_tag[i++] = *cursor++;
    }

    /* Process trailing spaces */
    while (i > 0 && active_tag[i - 1] == ' ') {
      i--;
    }
    active_tag[i] = '\0';

    if (*cursor == ',' || *cursor == ';') {
      cursor++;
    }

    if (active_tag[0] == '\0') {
      continue;
    }

    /* Check if this active_tag exists in category_tags */
    if (has_tag_in_string(category_tags, active_tag)) {
      return true;  // Found at least one matching tag
    }
  }

  return false;  // No matching tags found in category
}

bool panel_category_is_visible_by_tags(const bContext *C,
                                       const wmWindowManager *wm,
                                       const char *category)
{
  /* Reserved categories are always visible */
  if (category_is_reserved_for_reorder(wm, category)) {
    return true;
  }

  /* Tag filtering - check horizontal tag bar filter */
  if (!category_passes_tag_filter(C, category)) {
    return false;
  }

  /* Get tags assigned to this category */
  const char *tags_string = category_tags_string_lookup(wm, category);
  if (tags_string == nullptr || tags_string[0] == '\0') {
    return true; /* No tags = always visible */
  }

  /* Get current mode */
  uint32_t current_mode_flag = get_current_tag_mode_flag(C);

  /* Parse semicolon-separated tags and check if any is active in current mode */
  char tag_name[64];
  const char *cursor = tags_string;

  while (*cursor) {
    /* Extract tag name */
    int i = 0;
    while (*cursor && *cursor != ';' && i < 63) {
      tag_name[i++] = *cursor++;
    }
    tag_name[i] = '\0';
    if (*cursor == ';') {
      cursor++;
    }

    /* Skip empty tags */
    if (tag_name[0] == '\0') {
      continue;
    }

    /* Find tag definition and check mode */
    for (const CategoryTagDef *tag = static_cast<const CategoryTagDef *>(
             wm->category_tags.first);
         tag;
         tag = static_cast<const CategoryTagDef *>(tag->next))
    {
      if (STREQ(tag->name, tag_name)) {
        /* mode_flags == 0 means all modes active */
        if (tag->mode_flags == 0 || (tag->mode_flags & current_mode_flag)) {
          return true;
        }
      }
    }
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Order Functions
 * \{ */

static int get_category_order_index(const bContext *C, ARegion *region, const char *category_id)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;

  /* Look up in workspace order */
  int user_index = 0;
  if (workspace_category_order_list_is_valid(&workspace->category_order)) {
    for (WorkspaceCategoryOrder *order =
             static_cast<WorkspaceCategoryOrder *>(workspace->category_order.first);
         order;
         order = order->next)
    {
      if (order->space_type == space_type && order->region_type == region_type) {
        if (STREQ(order->category_id, category_id)) {
          return user_index;
        }
        user_index++;
      }
    }
  }

  /* Not found in order - return default position */
  int default_index = 0;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (STREQ(pc_dyn.idname, category_id)) {
      return default_index;
    }
    default_index++;
  }
  return 0;
}

/**
 * Generate a unique key for the current active tag combination.
 * Tags are sorted alphabetically to ensure consistent keys.
 * Returns empty string for no filter, "Tag1;Tag2" for multiple tags.
 */
static std::string get_tag_combination_key(const wmWindowManager *wm, View3D *v3d)
{
  UNUSED_VARS(wm);  /* Reserved for future use */
  
  if (!v3d || v3d->active_tag_filter_tags[0] == '\0') {
    return "";  /* No filter active */
  }

  /* Parse and collect tag names */
  Vector<std::string> active_tags;
  char tags_copy[256];
  STRNCPY(tags_copy, v3d->active_tag_filter_tags);

  char *tag = strtok(tags_copy, ",;");
  while (tag != nullptr) {
    while (*tag == ' ') {
      tag++;
    }
    if (tag[0] != '\0') {
      active_tags.append(std::string(tag));
    }
    tag = strtok(nullptr, ",;");
  }

  if (active_tags.is_empty()) {
    return "";
  }

  /* Sort alphabetically for consistent keys */
  std::sort(active_tags.begin(), active_tags.end());

  /* Join with semicolons */
  std::string key;
  for (int i = 0; i < active_tags.size(); i++) {
    if (i > 0) {
      key += ";";
    }
    key += active_tags[i];
  }

  return key;
}

/**
 * Load category order from JSON for a specific tag combination.
 * Calls Python function get_category_order().
 */
static Vector<std::string> load_category_order_from_json(const bContext *C, const char *tag_key)
{
#ifdef WITH_PYTHON
  Vector<std::string> result;

  if (!C) {
    return result;
  }

  /* Escape tag_key for Python string literal */
  char escaped_key[256];
  int j = 0;
  for (int i = 0; tag_key[i] != '\0' && j < (int)sizeof(escaped_key) - 1; i++) {
    char c = tag_key[i];
    if (c == '\\' || c == '\'') {
      if (j + 1 < (int)sizeof(escaped_key) - 1) {
        escaped_key[j++] = '\\';
        escaped_key[j++] = c;
      }
    }
    else {
      escaped_key[j++] = c;
    }
  }
  escaped_key[j] = '\0';

  /* Use json.dumps to convert Python list to JSON string for C++ parsing */
  /* ensure_ascii=False to preserve Unicode characters (not escape them as \uXXXX) */
  char python_expr[512];
  SNPRINTF(python_expr,
           "json.dumps(__import__('bl_ui.space_userpref', fromlist=['']).get_category_order('%s') or [], ensure_ascii=False)",
           escaped_key);

  /* Execute Python expression and capture output */
  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};

  /* BPY_run_string_as_string requires non-const context */
  const char *imports_json[] = {"json", nullptr};
  bool success = BPY_run_string_as_string(
      const_cast<bContext *>(C),
      imports_json,
      python_expr,
      &err_info,
      &result_str);

  if (!success) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
    return result;
  }

  if (!result_str) {
    return result;
  }

  /* Parse JSON array: ["Item", "Tool", ...] */
  const char *p = result_str;

  /* Skip to array start */
  while (*p && *p != '[') p++;
  if (*p == '[') p++;
  else {
    MEM_delete(result_str);
    return result;
  }

  /* Parse array elements */
  while (*p && *p != ']') {
    /* Skip whitespace and commas */
    while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
    if (*p == ']') break;

    /* Skip to string start (JSON uses double quotes) */
    if (*p != '"') {
      p++;
      continue;
    }
    p++; /* Skip opening quote */

    const char *start = p;
    /* Find end of string, handling escape sequences */
    while (*p && *p != '"') {
      if (*p == '\\' && *(p+1)) p += 2;
      else p++;
    }

    if (*p == '"') {
      std::string cat_id = std::string(start, p - start);
      p++; /* Skip closing quote */
      if (!cat_id.empty()) {
        result.append(cat_id);
      }
    }
  }

  MEM_delete(result_str);
  return result;
#else
  UNUSED_VARS(C, tag_key);
  return Vector<std::string>();
#endif
}

/**
 * Save category order to JSON for a specific tag combination.
 * Calls Python function set_category_order().
 */
static void save_category_order_to_json(const bContext *C,
                                        const char *tag_key,
                                        const Vector<std::string> &order)
{
#ifdef WITH_PYTHON
  if (!C) {
    return;
  }

  /* Build Python list of category IDs */
  std::string python_list = "[";
  for (int i = 0; i < order.size(); i++) {
    if (i > 0) {
      python_list += ", ";
    }
    python_list += "'";
    /* Escape backslashes and quotes for Python string literal */
    for (char c : order[i]) {
      if (c == '\\') {
        python_list += "\\\\";
      }
      else if (c == '\'') {
        python_list += "\\'";
      }
      else {
        python_list += c;
      }
    }
    python_list += "'";
  }
  python_list += "]";

  /* Escape tag_key for Python string literal */
  char escaped_key[256];
  int j = 0;
  for (int i = 0; tag_key[i] != '\0' && j < (int)sizeof(escaped_key) - 1; i++) {
    char c = tag_key[i];
    if (c == '\\' || c == '\'') {
      if (j + 1 < (int)sizeof(escaped_key) - 1) {
        escaped_key[j++] = '\\';
        escaped_key[j++] = c;
      }
    }
    else {
      escaped_key[j++] = c;
    }
  }
  escaped_key[j] = '\0';

  char python_cmd[8192];
  SNPRINTF(python_cmd,
           "from bl_ui.space_userpref import set_category_order\n"
           "set_category_order('%s', %s)\n",
           escaped_key,
           python_list.c_str());

  /* BPY_run_string_exec requires non-const context */
  const char *imports_none[] = {nullptr};
  BPY_run_string_exec(const_cast<bContext *>(C), imports_none, python_cmd);
#else
  UNUSED_VARS(C, tag_key, order);
#endif
}

/* Forward declaration */
Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region);

static int calculate_insert_index(const bContext *C,
                                  const wmWindowManager *wm,
                                  ARegion *region,
                                  CategoryDragState *state)
{
  const int min_insert_index = count_reserved_tabs_at_start(wm, region);
  int index = 0;

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    if (STREQ(pc_dyn.idname, state->drag_category_id)) {
      continue;
    }

    const int tab_height = BLI_rcti_size_y(&pc_dyn.rect);

    int y_shift = 0;
    if (!state->is_reserved) {
      int curr_disp = index;
      if (index >= state->original_index) {
        curr_disp++;
      }

      if (state->current_insert_index > state->original_index) {
        if (curr_disp > state->original_index && curr_disp <= state->current_insert_index) {
          y_shift = state->drag_tab_height + state->tab_v_pad;
        }
      }
      else if (state->current_insert_index < state->original_index) {
        if (curr_disp >= state->current_insert_index && curr_disp < state->original_index) {
          y_shift = -state->drag_tab_height - state->tab_v_pad;
        }
      }
    }

    const int tab_center_y = (pc_dyn.rect.ymax + y_shift) - tab_height / 2;

    int edge_offset;
    if (state->drag_offset_y > state->prev_drag_offset_y) {
      edge_offset = state->drag_top_edge_offset;
    }
    else {
      edge_offset = state->drag_bottom_edge_offset;
    }
    const int effective_y = state->drag_start_y + edge_offset + int(state->drag_offset_y);

    if (effective_y > tab_center_y) {
      return max_ii(index, min_insert_index);
    }

    index++;
  }

  return index;
}

static void update_insert_zone(const bContext *C,
                               const wmWindowManager * /*wm*/,
                               ARegion *region,
                               CategoryDragState *state)
{
  int current_index = 0;
  int y_accumulated = 0;

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    const int tab_height = BLI_rcti_size_y(&pc_dyn.rect);

    if (current_index == state->current_insert_index) {
      state->insert_y_start = y_accumulated;
      state->insert_y_end = y_accumulated + tab_height + state->tab_v_pad;
      return;
    }

    if (!STREQ(pc_dyn.idname, state->drag_category_id)) {
      y_accumulated += tab_height + state->tab_v_pad;
    }
    current_index++;
  }

  state->insert_y_start = y_accumulated;
  state->insert_y_end = y_accumulated + state->drag_tab_height + state->tab_v_pad;
}

static void workspace_category_order_clear(WorkSpace *workspace, int space_type, int region_type)
{
  if (!workspace_category_order_list_is_valid(&workspace->category_order)) {
    return;
  }

  WorkspaceCategoryOrder *order = static_cast<WorkspaceCategoryOrder *>(
      workspace->category_order.first);
  WorkspaceCategoryOrder *order_next;

  while (order) {
    order_next = order->next;
    if (order->space_type == space_type && order->region_type == region_type) {
      BLI_remlink(&workspace->category_order, order);
      MEM_delete(order);
    }
    order = order_next;
  }
}

static void apply_category_order(bContext *C, ARegion *region, CategoryDragState *state)
{
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : 0;
  const int region_type = region->regiontype;
  const wmWindowManager *wm = CTX_wm_manager(C);
  View3D *v3d = nullptr;

  if (area && area->spacetype == SPACE_VIEW3D) {
    v3d = static_cast<View3D *>(area->spacedata.first);
  }

  /* Get tag combination key for current filter state */
  std::string tag_key = get_tag_combination_key(wm, v3d);

  Vector<std::string> final_order;
  int insert_idx = 0;

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    if (insert_idx == state->current_insert_index) {
      final_order.append(state->drag_category_id);
    }

    if (!STREQ(pc_dyn.idname, state->drag_category_id)) {
      final_order.append(pc_dyn.idname);
      insert_idx++;
    }
  }

  if (insert_idx <= state->current_insert_index &&
      !final_order.contains(state->drag_category_id))
  {
    final_order.append(state->drag_category_id);
  }

  /* Save to JSON for this tag combination */
  save_category_order_to_json(C, tag_key.c_str(), final_order);

  /* Also update WorkspaceCategoryOrder for backward compatibility */
  workspace_category_order_clear(workspace, space_type, region_type);

  for (int i = 0; i < final_order.size(); i++) {
    WorkspaceCategoryOrder *item = MEM_new<WorkspaceCategoryOrder>(__func__);
    item->space_type = space_type;
    item->region_type = region_type;
    STRNCPY(item->category_id, final_order[i].c_str());
    item->order_index = i;
    BLI_addtail(&workspace->category_order, item);
  }

  /* Trigger full redraw to ensure new order is used */
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);
}

void panel_category_tabs_ensure_active_visible(const bContext *C, ARegion *region)
{
  if (!panel_category_tabs_is_visible(region)) {
    return;
  }

  const wmWindowManager *wm = CTX_wm_manager(C);
  const char *current_active = panel_category_active_get(region, false);

  if (current_active && panel_category_is_visible_by_tags(C, wm, current_active)) {
    return;
  }

  Vector<PanelCategoryDyn *> visible_categories = get_ordered_categories(C, region);
  if (!visible_categories.is_empty()) {
    panel_category_active_set(region, visible_categories[0]->idname);
  }
}

Vector<PanelCategoryDyn *> get_ordered_categories(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  const wmWindowManager *wm = CTX_wm_manager(C);
  View3D *v3d = nullptr;

  if (area && area->spacetype == SPACE_VIEW3D) {
    v3d = static_cast<View3D *>(area->spacedata.first);
  }

  /* Get tag combination key for current filter state */
  std::string tag_key = get_tag_combination_key(wm, v3d);

  /* Load order from JSON for this tag combination */
  Vector<std::string> json_order = load_category_order_from_json(C, tag_key.c_str());

  /* Map of existing categories for quick lookup */
  Map<std::string, PanelCategoryDyn *> existing;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    /* Only include categories that are visible by tag filtering */
    if (panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      existing.add(std::string(pc_dyn.idname), &pc_dyn);
    }
  }

  /* Build result list following JSON order */
  Vector<PanelCategoryDyn *> result;
  Set<std::string> added;

  /* First: categories in JSON order (skip missing - disabled addons) */
  for (const std::string &cat_id : json_order) {
    PanelCategoryDyn **pc = existing.lookup_ptr(cat_id);
    if (pc && !added.contains(cat_id)) {
      result.append(*pc);
      added.add(cat_id);
    }
    /* Else: category in JSON but not registered - skip silently (disabled addon) */
  }

  /* Then: remaining categories (new ones or not in JSON order) */
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    std::string id(pc_dyn.idname);
    if (!added.contains(id) && panel_category_is_visible_by_tags(C, wm, pc_dyn.idname)) {
      result.append(&pc_dyn);
      added.add(id);
    }
  }

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Drawing Functions
 * \{ */

void panel_category_tabs_draw_settings_button(const bContext *C,
                                               ARegion *region,
                                               float zoom,
                                               const unsigned char theme_col_tab_text[3])
{
  const uiStyle *style = style_get();
  const int fontid = style->widget.uifont_id;

  const rcti *rct = &region->runtime->category_tabs_settings_rect;

  bool is_hover = region->runtime->category_tabs_settings_hover;
  wmWindow *win = CTX_wm_window(C);

  const double current_time = BLI_time_now_seconds();
  const double time_since_hover = current_time - region->runtime->category_tabs_settings_hover_time;

  const bool drag_active = (region->runtime->category_tabs_drag_state != nullptr);
  const bool drag_pending = (region->runtime->category_tabs_drag_pending_id[0] != '\0');

  const bool hover_timeout_expired = time_since_hover > 2.0;

  if (win && win->runtime->eventstate) {
    const int mx = win->runtime->eventstate->xy[0] - region->winrct.xmin;
    const int my = win->runtime->eventstate->xy[1] - region->winrct.ymin;
    const bool mouse_in_region = BLI_rcti_isect_pt(&region->winrct,
                                                   win->runtime->eventstate->xy[0],
                                                   win->runtime->eventstate->xy[1]);
    const bool actually_over = mouse_in_region && BLI_rcti_isect_pt(rct, mx, my);

    if (drag_active || drag_pending) {
      is_hover = false;
    }
    else if (actually_over) {
      if (!is_hover) {
        region->runtime->category_tabs_settings_hover_time = current_time;
      }
      is_hover = true;
    }
    else if (hover_timeout_expired) {
      is_hover = false;
    }

    region->runtime->category_tabs_settings_hover = is_hover;
  }

  bTheme *btheme = theme::theme_get();
  const float tab_curve_radius = btheme->tui.wcol_tab.roundness * U.widget_unit * zoom;
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  const int roundboxtype = region->overlap ? CNR_ALL :
                                             (is_left ? (CNR_TOP_LEFT | CNR_BOTTOM_LEFT) :
                                                        (CNR_TOP_RIGHT | CNR_BOTTOM_RIGHT));

  float theme_col_tab_bg[4];
  float theme_col_tab_outline[4];
  if (is_hover) {
    theme::get_color_4fv(TH_TAB_ACTIVE, theme_col_tab_bg);
  }
  else {
    theme::get_color_4fv(TH_TAB_INACTIVE, theme_col_tab_bg);
  }
  theme::get_color_4fv(TH_TAB_OUTLINE, theme_col_tab_outline);

  GPU_blend(GPU_BLEND_ALPHA);

  rctf box_rect;
  box_rect.xmin = float(rct->xmin);
  box_rect.xmax = float(rct->xmax);
  box_rect.ymin = float(rct->ymin);
  box_rect.ymax = float(rct->ymax);

  draw_roundbox_corner_set(roundboxtype);
  draw_roundbox_4fv(&box_rect, true, tab_curve_radius, theme_col_tab_bg);
  draw_roundbox_4fv(&box_rect, false, tab_curve_radius, theme_col_tab_outline);

  const float glyph_width = BLF_width(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX);
  const int ascender_i = BLF_ascender(fontid);
  const int descender_i = BLF_descender(fontid);
  const float ascender = float(ascender_i);
  const float descender = float(descender_i);
  const float glyph_height = ascender - descender;

  const float tab_center_x = float(rct->xmin + rct->xmax) * 0.5f;
  const float tab_center_y = float(rct->ymin + rct->ymax) * 0.5f;

  float pos_x = tab_center_x - glyph_width * 0.5f;
  float pos_y = tab_center_y - glyph_height * 0.5f - descender;

  uchar theme_col_text_hi[3];
  theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_text_hi);

  BLF_disable(fontid, BLF_ROTATION);
  BLF_position(fontid, pos_x, pos_y, 0.0f);
  BLF_color3ubv(fontid, is_hover ? theme_col_text_hi : theme_col_tab_text);
  BLF_draw(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX);

  GPU_blend(GPU_BLEND_NONE);
}

static void ui_panel_category_draw_content(
    const ARegion *region,
    const wmWindowManager *wm,
    const char *category_id,
    const char *category_id_draw,
    const rcti *rct,
    const int rct_xmin,
    const int rct_xmax,
    const bool is_active,
    const bool is_left,
    const eUserPref_CategoryTabsDisplayMode display_mode,
    const int fontid,
    const uiFontStyle *fstyle,
    const float fstyle_points,
    const float zoom,
    const float category_tabs_zoom,
    const int tab_v_pad_text,
    const float darken_factor,
    const uchar theme_col_tab_text[3],
    const uchar theme_col_tab_text_sel[3])
{
  bool is_fallback_letter = false;
  float glyph_color[3] = {0.0f, 0.0f, 0.0f};
  const char *glyph = panel_category_glyph_lookup(
      wm, category_id, nullptr, &is_fallback_letter, glyph_color);

  /* Handle nullptr glyph (explicitly cleared) - use fallback letter from category */
  char fallback_glyph_buf[8];
  if (glyph == nullptr && is_fallback_letter) {
    /* Get first character of category as fallback letter */
    const int first_char_size = BLI_str_utf8_size_safe(category_id);
    if (first_char_size > 0) {
      memcpy(fallback_glyph_buf, category_id, first_char_size);
      fallback_glyph_buf[first_char_size] = '\0';
      glyph = fallback_glyph_buf;
    }
    else {
      /* Fallback to category_id if we can't extract first char */
      glyph = category_id;
    }
  }

  const bool has_glyph = is_single_glyph_str(glyph) && !is_fallback_letter;

  bool draw_dual = false;
  const char *text_for_name = category_id_draw;

  if (display_mode == USER_CATEGORY_TABS_GLYPHS_TEXT && (has_glyph || is_fallback_letter)) {
    draw_dual = true;
    if (is_fallback_letter && is_single_glyph_str(category_id_draw)) {
      for (const PanelType &pt : region->runtime->type->paneltypes) {
        if (pt.category && STREQ(pt.category, category_id)) {
          const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
          if (panel_label && panel_label[0]) {
            text_for_name = panel_label;
            break;
          }
        }
      }
    }
  }
  else if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
           U.category_tabs_show_active_name && is_active)
  {
    if (has_glyph || is_fallback_letter) {
      draw_dual = true;
      if (is_single_glyph_str(category_id_draw)) {
        for (const PanelType &pt : region->runtime->type->paneltypes) {
          if (pt.category && STREQ(pt.category, category_id)) {
            const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
            if (panel_label && panel_label[0]) {
              text_for_name = panel_label;
              break;
            }
          }
        }
      }
    }
  }

  auto shadow_enable = [&]() {
    if (fstyle->shadow) {
      BLF_enable(fontid, BLF_SHADOW);
      const float shadow_color[4] = {
          fstyle->shadowcolor, fstyle->shadowcolor, fstyle->shadowcolor, fstyle->shadowalpha};
      BLF_shadow(fontid, FontShadowType(fstyle->shadow), shadow_color);
      BLF_shadow_offset(fontid, fstyle->shadx, fstyle->shady);
    }
  };
  auto shadow_disable = [&]() {
    if (fstyle->shadow) {
      BLF_disable(fontid, BLF_SHADOW);
    }
  };

  if (draw_dual) {
    BLF_disable(fontid, BLF_ROTATION);

    const float glyph_width_val = BLF_width(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX);
    const float ascender = float(BLF_ascender(fontid));
    const float descender = float(BLF_descender(fontid));
    const float glyph_height = ascender - descender;

    const float tab_center_x = float(rct->xmin + rct->xmax) * 0.5f;
    const float extra_shift = is_fallback_letter ? (4.0f * UI_SCALE_FAC) : 0.0f;
    const float glyph_pos_y = float(rct->ymax) - glyph_height - (tab_v_pad_text - extra_shift);

    BLF_position(fontid, tab_center_x - glyph_width_val * 0.5f, glyph_pos_y - descender, 0.0f);
    uchar glyph_color_out[3];
    set_glyph_color(
        fontid, glyph_color, is_active, theme_col_tab_text, theme_col_tab_text_sel, glyph_color_out);
    if (!is_active && darken_factor > 0.0f) {
      apply_glyph_darkening(fontid, glyph_color_out, darken_factor);
    }

    shadow_enable();
    BLF_draw(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX);
    shadow_disable();

    BLF_enable(fontid, BLF_ROTATION);
    BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);

    const int text_v_ofs = round_fl_to_int(float(rct_xmax - rct_xmin) * 0.5f);
    const int text_size_offset = round_fl_to_int(fstyle_points * UI_SCALE_FAC *
                                                  category_tabs_zoom * 0.35f);
    const float text_pos_x = is_left ? rct->xmax - text_v_ofs + text_size_offset :
                                        rct->xmin + text_v_ofs - text_size_offset;

    const int glyph_h = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
    const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
    const float text_pos_y = is_left ? rct->ymin + tab_v_pad_text + glyph_text_gap :
                                        rct->ymax - tab_v_pad_text - glyph_h - glyph_text_gap;

    BLF_position(fontid, text_pos_x, text_pos_y, 0.0f);

    if (!is_active && darken_factor > 0.0f) {
      uchar text_color[3] = {theme_col_tab_text[0], theme_col_tab_text[1], theme_col_tab_text[2]};
      darken_color_3ub(text_color, darken_factor);
      BLF_color3ubv(fontid, text_color);
    }
    else {
      BLF_color3ubv(fontid, is_active ? theme_col_tab_text_sel : theme_col_tab_text);
    }

    shadow_enable();
    BLF_draw(fontid, text_for_name, BLF_DRAW_STR_DUMMY_MAX);
    shadow_disable();

    BLF_disable(fontid, BLF_ROTATION);
    return;
  }

  const char *draw_str;
  bool draw_as_glyph;
  bool should_rotate = false;

  switch (display_mode) {
    case USER_CATEGORY_TABS_GLYPHS_ONLY:
      draw_str = glyph;
      draw_as_glyph = !is_fallback_letter;
      should_rotate = false;
      break;
    case USER_CATEGORY_TABS_GLYPHS_TEXT:
      draw_str = category_id_draw;
      draw_as_glyph = is_single_glyph_str(category_id_draw);
      should_rotate = !draw_as_glyph;
      break;
    case USER_CATEGORY_TABS_TEXT_ONLY:
    default:
      if (is_single_glyph_str(category_id_draw)) {
        const char *panel_label = nullptr;
        for (const PanelType &pt : region->runtime->type->paneltypes) {
          if (pt.category && STREQ(pt.category, category_id)) {
            panel_label = CTX_IFACE_(pt.translation_context, pt.label);
            if (panel_label && panel_label[0]) {
              break;
            }
          }
        }
        draw_str = panel_label ? panel_label : category_id_draw;
      }
      else {
        draw_str = category_id_draw;
      }
      draw_as_glyph = false;
      should_rotate = true;
      break;
  }

  if (!should_rotate) {
    BLF_disable(fontid, BLF_ROTATION);
    const float gw = BLF_width(fontid, draw_str, BLF_DRAW_STR_DUMMY_MAX);
    const float asc = float(BLF_ascender(fontid));
    const float desc = float(BLF_descender(fontid));
    const float gh = asc - desc;
    const float cx = float(rct->xmin + rct->xmax) * 0.5f;
    const float cy = float(rct->ymin + rct->ymax) * 0.5f;
    const float draw_pos_y = cy - gh * 0.5f - desc;
    BLF_position(fontid, cx - gw * 0.5f, draw_pos_y, 0.0f);

  }
  else {
    BLF_enable(fontid, BLF_ROTATION);
    BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
    const int text_v_ofs = round_fl_to_int(float(rct_xmax - rct_xmin) * 0.5f);
    const int text_size_offset = round_fl_to_int(fstyle_points * UI_SCALE_FAC *
                                                  category_tabs_zoom * 0.35f);
    const float px = is_left ? rct->xmax - text_v_ofs + text_size_offset :
                               rct->xmin + text_v_ofs - text_size_offset;
    const float py = is_left ? rct->ymin + tab_v_pad_text : rct->ymax - tab_v_pad_text;
    BLF_position(fontid, px, py, 0.0f);
  }

  if (draw_as_glyph) {
    uchar glyph_color_out[3];
    set_glyph_color(
        fontid, glyph_color, is_active, theme_col_tab_text, theme_col_tab_text_sel, glyph_color_out);
    apply_glyph_darkening(fontid, glyph_color_out, darken_factor);
  }
  else {
    uchar text_color[3];
    /* Use custom glyph color even for fallback letters or when in icon-only mode.
     * Respect the 'Show Colored Text' preference in Text mode. */
    bool use_custom_color = !is_zero_v3(glyph_color);
    if (display_mode == USER_CATEGORY_TABS_TEXT_ONLY && !U.category_tabs_text_mode_show_colored_text) {
      use_custom_color = false;
    }

    if (use_custom_color) {
      text_color[0] = uchar(glyph_color[0] * 255);
      text_color[1] = uchar(glyph_color[1] * 255);
      text_color[2] = uchar(glyph_color[2] * 255);
    }
    else {
      text_color[0] = is_active ? theme_col_tab_text_sel[0] : theme_col_tab_text[0];
      text_color[1] = is_active ? theme_col_tab_text_sel[1] : theme_col_tab_text[1];
      text_color[2] = is_active ? theme_col_tab_text_sel[2] : theme_col_tab_text[2];
    }
    if (darken_factor > 0.0f) {
      darken_color_3ub(text_color, darken_factor);
    }
    BLF_color3ubv(fontid, text_color);
  }

  shadow_enable();
  BLF_draw(fontid, draw_str, BLF_DRAW_STR_DUMMY_MAX);
  shadow_disable();

  BLF_disable(fontid, BLF_ROTATION);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Indicator Drawing
 * \{ */

/**
 * Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color.
 * This is used both for regular tabs and during drag & drop.
 */
static void draw_category_tab_color_indicator(const rcti *rct,
                                                const float glyph_color[3],
                                                const bool is_left,
                                                const eUserPref_CategoryTabsDisplayMode display_mode,
                                                const bool show_color_indicator)
{
  /* Only draw indicator in TEXT_ONLY mode when setting is enabled. */
  if (display_mode != USER_CATEGORY_TABS_TEXT_ONLY || !show_color_indicator) {
    return;
  }

  /* Check if custom glyph color is assigned (non-zero means custom color set). */
  if (glyph_color[0] == 0.0f && glyph_color[1] == 0.0f && glyph_color[2] == 0.0f) {
    return;
  }

  const int indicator_thickness = round_fl_to_int(1.0f * UI_SCALE_FAC);

  /* For left-aligned tabs: indicator on left edge (outer edge). */
  /* For right-aligned tabs: indicator on right edge (outer edge). */
  const int indicator_xmin = is_left ? rct->xmin : rct->xmax - indicator_thickness;
  const int indicator_xmax = indicator_xmin + indicator_thickness;

  /* Setup GPU for immediate mode rectangle drawing. */
  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  float indicator_color[4] = {glyph_color[0], glyph_color[1], glyph_color[2], 0.8f};
  immUniformColor4fv(indicator_color);
  immRectf(pos, float(indicator_xmin), float(rct->ymin), float(indicator_xmax), float(rct->ymax));

  immUnbindProgram();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Drawing Function
 * \{ */

void panel_category_tabs_draw_all(const bContext *C, ARegion *region, const char *category_id_active)
{
  const bool is_left = RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT;
  View2D *v2d = &region->v2d;
  const uiStyle *style = style_get();
  const uiFontStyle *fstyle = &style->widget;
  fontstyle_set(fstyle);
  const int fontid = fstyle->uifont_id;
  float fstyle_points = fstyle->points;
  const float aspect = BLI_listbase_is_empty(&region->runtime->uiblocks) ?
                           1.0f :
                           (static_cast<Block *>(region->runtime->uiblocks.first))->aspect;

  CategoryDragState *drag_state = static_cast<CategoryDragState *>(
      region->runtime->category_tabs_drag_state);
  const bool is_dragging = (drag_state != nullptr && drag_state->is_dragging);
  const char *drag_category_id = is_dragging ? drag_state->drag_category_id : "";

  const eUserPref_CategoryTabsDisplayMode display_mode =
      static_cast<eUserPref_CategoryTabsDisplayMode>(U.category_tabs_display_mode);

  float category_tabs_zoom;
  switch (display_mode) {
    case USER_CATEGORY_TABS_GLYPHS_ONLY:
      category_tabs_zoom = U.category_tabs_zoom_icon;
      break;
    case USER_CATEGORY_TABS_GLYPHS_TEXT:
      category_tabs_zoom = U.category_tabs_zoom_mixed;
      break;
    case USER_CATEGORY_TABS_TEXT_ONLY:
    default:
      category_tabs_zoom = U.category_tabs_zoom_text;
      break;
  }
  const float zoom = (1.0f / aspect) * category_tabs_zoom;

  const wmWindowManager *wm = CTX_wm_manager(C);

  const int px = U.pixelsize;
  const int category_tabs_width = round_fl_to_int(UI_PANEL_CATEGORY_MARGIN_WIDTH * zoom);
  const float dpi_fac = UI_SCALE_FAC;
  const int tab_v_pad_text = round_fl_to_int(TABS_PADDING_TEXT_FACTOR * dpi_fac * zoom) + 2 * px;
  const int tab_v_pad = round_fl_to_int(TABS_PADDING_BETWEEN_FACTOR * dpi_fac * zoom);
  bTheme *btheme = theme::theme_get();
  const float tab_curve_radius = btheme->tui.wcol_tab.roundness * U.widget_unit * zoom;
  const int roundboxtype = region->overlap ? CNR_ALL :
                                             (is_left ? (CNR_TOP_LEFT | CNR_BOTTOM_LEFT) :
                                                        (CNR_TOP_RIGHT | CNR_BOTTOM_RIGHT));
  bool is_alpha;

  const int rct_xmin = is_left ? v2d->mask.xmin + 3 : (v2d->mask.xmax - category_tabs_width);
  const int rct_xmax = is_left ? v2d->mask.xmin + category_tabs_width : (v2d->mask.xmax - 3);

  int y_ofs = tab_v_pad;

  uchar theme_col_back[4];
  uchar theme_col_tab_bg[4];
  uchar theme_col_tab_text[3];
  uchar theme_col_tab_text_sel[3];
  float theme_col_tab_active[4];
  float theme_col_tab_inactive[4];
  float theme_col_tab_outline[4];
  float theme_col_tab_outline_sel[4];

  theme::get_color_4ubv(TH_BACK, theme_col_back);
  theme::get_color_3ubv(TH_TAB_TEXT, theme_col_tab_text);
  theme::get_color_3ubv(TH_TAB_TEXT_HI, theme_col_tab_text_sel);
  theme::get_color_4ubv(TH_TAB_BACK, theme_col_tab_bg);
  theme::get_color_4fv(TH_TAB_ACTIVE, theme_col_tab_active);
  theme::get_color_4fv(TH_TAB_INACTIVE, theme_col_tab_inactive);
  theme::get_color_4fv(TH_TAB_OUTLINE, theme_col_tab_outline);
  theme::get_color_4fv(TH_TAB_OUTLINE_ACTIVE, theme_col_tab_outline_sel);

  is_alpha = (region->overlap && (theme_col_back[3] != 255));

  fontscale(&fstyle_points, aspect);
  BLF_size(fontid, fstyle_points * UI_SCALE_FAC * category_tabs_zoom);

  if (BKE_regiontype_uses_category_tabs(region->runtime->type)) {
    BLI_assert(panel_category_is_visible(region));
  }

  wmWindow *win = CTX_wm_window(C);
  int mouse_x = 0, mouse_y = 0;
  bool mouse_in_region = false;
  if (win && win->runtime->eventstate) {
    mouse_x = win->runtime->eventstate->xy[0] - region->winrct.xmin;
    mouse_y = win->runtime->eventstate->xy[1] - region->winrct.ymin;
    mouse_in_region = BLI_rcti_isect_pt(&region->winrct,
                                        win->runtime->eventstate->xy[0],
                                        win->runtime->eventstate->xy[1]);
  }

  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;
    rcti *rct = &pc_dyn.rect;
    const char *category_id = pc_dyn.idname;
    const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id));

    bool is_fallback_letter = false;
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    const char *glyph = panel_category_glyph_lookup(wm, category_id, nullptr, &is_fallback_letter, glyph_color);

    /* Handle nullptr glyph (explicitly cleared) - use fallback letter from category */
    char fallback_glyph_buf[8];
    if (glyph == nullptr && is_fallback_letter) {
      /* Get first character of category as fallback letter */
      const int first_char_size = BLI_str_utf8_size_safe(category_id);
      if (first_char_size > 0) {
        memcpy(fallback_glyph_buf, category_id, first_char_size);
        fallback_glyph_buf[first_char_size] = '\0';
        glyph = fallback_glyph_buf;
      }
      else {
        /* Fallback to category_id if we can't extract first char */
        glyph = category_id;
      }
    }

    const bool has_glyph = is_single_glyph_str(glyph) && !is_fallback_letter;

    int category_width;
    switch (display_mode) {
      case USER_CATEGORY_TABS_GLYPHS_ONLY: {
        /* Use glyph height (without rotation) for consistent sizing with GLYPHS_TEXT mode. */
        const int glyph_h = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
        category_width = glyph_h;

        if (U.category_tabs_show_active_name && STREQ(category_id, category_id_active)) {
          const char *text_for_name = category_id_draw;
          if (is_single_glyph_str(category_id_draw)) {
            for (const PanelType &pt : region->runtime->type->paneltypes) {
              if (pt.category && STREQ(pt.category, category_id)) {
                const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
                if (panel_label && panel_label[0]) {
                  text_for_name = panel_label;
                  break;
                }
              }
            }
          }
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          const int text_w = round_fl_to_int(BLF_width(fontid, text_for_name, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
          category_width += text_w + glyph_text_gap;
        }
        break;
      }

      case USER_CATEGORY_TABS_GLYPHS_TEXT: {
        const char *text_for_width = category_id_draw;
        if (has_glyph || is_fallback_letter) {
          if (is_fallback_letter && is_single_glyph_str(category_id_draw)) {
            for (const PanelType &pt : region->runtime->type->paneltypes) {
              if (pt.category && STREQ(pt.category, category_id)) {
                const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
                if (panel_label && panel_label[0]) {
                  text_for_width = panel_label;
                  break;
                }
              }
            }
          }
          const int glyph_h = round_fl_to_int(BLF_height(fontid, glyph, BLF_DRAW_STR_DUMMY_MAX));
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          const int text_w = round_fl_to_int(BLF_width(fontid, text_for_width, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
          const int glyph_text_gap = round_fl_to_int(TABS_GLYPH_TEXT_GAP_FACTOR * UI_SCALE_FAC * zoom);
          category_width = glyph_h + text_w + glyph_text_gap;
        }
        else {
          BLF_enable(fontid, BLF_ROTATION);
          BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
          category_width = round_fl_to_int(BLF_width(fontid, category_id_draw, BLF_DRAW_STR_DUMMY_MAX));
          BLF_disable(fontid, BLF_ROTATION);
        }
        break;
      }

      case USER_CATEGORY_TABS_TEXT_ONLY:
      default: {
        const char *text_for_size = category_id_draw;
        if (is_single_glyph_str(category_id_draw)) {
          for (const PanelType &pt : region->runtime->type->paneltypes) {
            if (pt.category && STREQ(pt.category, category_id)) {
              const char *panel_label = CTX_IFACE_(pt.translation_context, pt.label);
              if (panel_label && panel_label[0]) {
                text_for_size = panel_label;
                break;
              }
            }
          }
        }
        BLF_enable(fontid, BLF_ROTATION);
        BLF_rotation(fontid, is_left ? M_PI_2 : -M_PI_2);
        category_width = round_fl_to_int(BLF_width(fontid, text_for_size, BLF_DRAW_STR_DUMMY_MAX));
        BLF_disable(fontid, BLF_ROTATION);
        break;
      }
    }

    rct->xmin = rct_xmin;
    rct->xmax = rct_xmax;
    rct->ymin = v2d->mask.ymax - (y_ofs + category_width + (tab_v_pad_text * 2));
    rct->ymax = v2d->mask.ymax - (y_ofs);
    y_ofs += category_width + tab_v_pad + (tab_v_pad_text * 2);
  }

  const int settings_icon_height = round_fl_to_int(BLF_height(fontid, TABS_SETTINGS_ICON, BLF_DRAW_STR_DUMMY_MAX));
  const int settings_button_height = settings_icon_height + (tab_v_pad_text * 2);
  rcti *settings_rct = &region->runtime->category_tabs_settings_rect;
  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    settings_rct->xmin = rct_xmin;
    settings_rct->xmax = rct_xmax;
    settings_rct->ymin = v2d->mask.ymax - (y_ofs + settings_button_height);
    settings_rct->ymax = v2d->mask.ymax - y_ofs;
  }

  const int total_content_height = y_ofs + settings_button_height + tab_v_pad;
  const int max_scroll = max_ii(total_content_height - BLI_rcti_size_y(&v2d->mask), 0);
  const int scroll = clamp_i(region->category_scroll, 0, max_scroll);
  region->category_scroll = scroll;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    rcti *rct = &pc_dyn.rect;
    rct->ymin += scroll;
    rct->ymax += scroll;
  }

  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    settings_rct->ymin += scroll;
    settings_rct->ymax += scroll;
  }

  GPU_line_smooth(true);

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (is_alpha) {
    GPU_blend(GPU_BLEND_ALPHA);
    immUniformColor4ubv(theme_col_tab_bg);
  }
  else {
    immUniformColor3ubv(theme_col_tab_bg);
  }

  if (is_left) {
    immRectf(pos, v2d->mask.xmin, v2d->mask.ymin, v2d->mask.xmin + category_tabs_width, v2d->mask.ymax);
  }
  else {
    immRectf(pos, v2d->mask.xmax - category_tabs_width, v2d->mask.ymin, v2d->mask.xmax + 1, v2d->mask.ymax);
  }

  if (is_alpha) {
    GPU_blend(GPU_BLEND_NONE);
  }

  immUnbindProgram();

  const bool too_narrow = BLI_rcti_size_x(&region->winrct) <=
                          int(UI_PANEL_CATEGORY_MIN_WIDTH * UI_SCALE_FAC / aspect);

  int current_display_index = 0;

  for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
    PanelCategoryDyn &pc_dyn = *pc_dyn_ptr;

    if (is_dragging && !drag_state->is_reserved && STREQ(pc_dyn.idname, drag_category_id)) {
      current_display_index++;
      continue;
    }

    int y_shift = 0;
    if (is_dragging && !drag_state->is_reserved) {
      const int insert_idx = drag_state->current_insert_index;
      const int original_idx = drag_state->original_index;

      if (insert_idx > original_idx) {
        if (current_display_index > original_idx && current_display_index <= insert_idx) {
          y_shift = drag_state->drag_tab_height + tab_v_pad;
        }
      }
      else if (insert_idx < original_idx) {
        if (current_display_index >= insert_idx && current_display_index < original_idx) {
          y_shift = -drag_state->drag_tab_height - tab_v_pad;
        }
      }
    }

    rcti shifted_rect = pc_dyn.rect;
    shifted_rect.ymin += y_shift;
    shifted_rect.ymax += y_shift;

    const rcti *rct = &shifted_rect;

    if (rct->ymin > v2d->mask.ymax) {
      current_display_index++;
      continue;
    }
    if (rct->ymax < v2d->mask.ymin) {
      break;
    }
    const char *category_id = pc_dyn.idname;
    const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id));
    const bool is_active = !too_narrow && STREQ(category_id, category_id_active);

    /* Get glyph color for color indicator in TEXT_ONLY mode. */
    bool is_fallback_letter = false;
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    panel_category_glyph_lookup(wm, category_id, nullptr, &is_fallback_letter, glyph_color);

    current_display_index++;

    const bool is_hover = BLI_rcti_isect_pt(rct, mouse_x, mouse_y);

    float darken_factor = 0.0f;
    if (!is_active && !is_hover) {
      darken_factor = TABS_GLYPH_DARKEN_BASE;
    }

    float bg_brighten_factor = 0.0f;
    if (!is_active) {
      bg_brighten_factor = is_hover ? TABS_BG_BRIGHTEN_HOVER : TABS_BG_BRIGHTEN_BASE;
    }

    GPU_blend(GPU_BLEND_ALPHA);

    {
      draw_roundbox_corner_set(roundboxtype);
      rctf box_rect;
      box_rect.xmin = rct->xmin;
      box_rect.xmax = rct->xmax;
      box_rect.ymin = rct->ymin;
      box_rect.ymax = rct->ymax;

      float tab_bg_color[4];
      if (is_active) {
        copy_v4_v4(tab_bg_color, theme_col_tab_active);
      }
      else {
        copy_v4_v4(tab_bg_color, theme_col_tab_inactive);
        brighten_color_4fv(tab_bg_color, bg_brighten_factor);
      }

      draw_roundbox_4fv(&box_rect, true, tab_curve_radius, tab_bg_color);
      draw_roundbox_4fv(&box_rect, false, tab_curve_radius,
                        is_active ? theme_col_tab_outline_sel : theme_col_tab_outline);

      /* Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color. */
      draw_category_tab_color_indicator(
          rct, glyph_color, is_left, display_mode, U.category_tabs_text_mode_show_color_indicator);

      if (!region->overlap) {
        pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
        immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

        immUniformColor4fv(tab_bg_color);
        immRectf(pos,
                 is_left ? rct->xmax - px : rct->xmin,
                 rct->ymin + px,
                 is_left ? rct->xmax : rct->xmin + px,
                 rct->ymax - px);
        immUnbindProgram();
      }
    }

    ui_panel_category_draw_content(region,
                                   wm,
                                   category_id,
                                   category_id_draw,
                                   rct,
                                   rct_xmin,
                                   rct_xmax,
                                   is_active,
                                   is_left,
                                   display_mode,
                                   fontid,
                                   fstyle,
                                   fstyle_points,
                                   zoom,
                                   category_tabs_zoom,
                                   tab_v_pad_text,
                                   darken_factor,
                                   theme_col_tab_text,
                                   theme_col_tab_text_sel);

    if (is_left) {
      pc_dyn.rect.xmin = v2d->mask.xmin;
    }
    else {
      pc_dyn.rect.xmax = v2d->mask.xmax;
    }
  }

  if (!BLI_listbase_is_empty(&region->runtime->panels_category)) {
    const rcti *settings_rct_ptr = &region->runtime->category_tabs_settings_rect;
    if (settings_rct_ptr->ymin <= v2d->mask.ymax && settings_rct_ptr->ymax >= v2d->mask.ymin) {
      panel_category_tabs_draw_settings_button(C, region, zoom, theme_col_tab_text);
    }
  }

  /* Draw the dragged tab at cursor position and ghost tab at insert position */
  if (is_dragging && !drag_state->is_reserved) {
    PanelCategoryDyn *drag_tab = nullptr;
    for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
      if (STREQ(pc_dyn.idname, drag_category_id)) {
        drag_tab = &pc_dyn;
        break;
      }
    }

    if (drag_tab) {
      const int insert_idx = drag_state->current_insert_index;
      const int original_idx = drag_state->original_index;
      const int tab_h = drag_state->drag_tab_height;

      /* Find the tab at insert position to determine ghost location */
      PanelCategoryDyn *insert_position_tab = nullptr;
      int current_idx = 0;
      for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
        if (STREQ(pc_dyn_ptr->idname, drag_category_id)) {
          continue;
        }
        if (current_idx == insert_idx) {
          insert_position_tab = pc_dyn_ptr;
          break;
        }
        current_idx++;
      }

      /* Draw ghost tab at insert position */
      if (insert_position_tab || insert_idx >= int(ordered_categories.size()) - 1) {
        rcti ghost_rect = drag_tab->rect;

        /* Find the tab to position relative to.
         * If inserting before a tab, position above it.
         * If appending (insert_position_tab is NULL), position below the last visible tab. */
        PanelCategoryDyn *target_tab = insert_position_tab;
        bool position_above = true;
        int target_orig_idx = -1;

        if (target_tab) {
          /* Need to find the original index of the target tab for shift calculation */
          int loop_idx = 0;
          for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
            if (pc_dyn_ptr == target_tab) {
              target_orig_idx = loop_idx;
              break;
            }
            loop_idx++;
          }
        }
        else {
          /* Append case: use last visible tab */
          int loop_idx = 0;
          for (PanelCategoryDyn *pc_dyn_ptr : ordered_categories) {
            if (STREQ(pc_dyn_ptr->idname, drag_category_id)) {
              loop_idx++;
              continue;
            }
            target_tab = pc_dyn_ptr;
            target_orig_idx = loop_idx;
            loop_idx++;
          }
          position_above = false;
        }

        if (target_tab && target_orig_idx != -1) {
          /* Calculate the visual shift that was applied to the target tab. */
          int target_shift_y = 0;
          if (insert_idx > original_idx) {
            if (target_orig_idx > original_idx && target_orig_idx <= insert_idx) {
              target_shift_y = tab_h + tab_v_pad;
            }
          }
          else if (insert_idx < original_idx) {
            if (target_orig_idx >= insert_idx && target_orig_idx < original_idx) {
              target_shift_y = -tab_h - tab_v_pad;
            }
          }

          /* Apply shift to target rect before calculating ghost position */
          rcti target_rect = target_tab->rect;
          target_rect.ymin += target_shift_y;
          target_rect.ymax += target_shift_y;

          if (position_above) {
            /* Ghost appears above the target tab */
            ghost_rect.ymin = target_rect.ymax + tab_v_pad;
            ghost_rect.ymax = target_rect.ymax + tab_h + tab_v_pad;
          }
          else {
            /* Ghost appears below the target tab */
            ghost_rect.ymin = target_rect.ymin - tab_h - tab_v_pad;
            ghost_rect.ymax = target_rect.ymin - tab_v_pad;
          }
        }

        /* Draw ghost tab (semi-transparent placeholder at insert position) */
        {
          rctf ghost_box_rect;
          ghost_box_rect.xmin = float(ghost_rect.xmin);
          ghost_box_rect.xmax = float(ghost_rect.xmax);
          ghost_box_rect.ymin = float(ghost_rect.ymin);
          ghost_box_rect.ymax = float(ghost_rect.ymax);

          /* Very transparent background for ghost */
          float ghost_bg_color[4];
          copy_v4_v4(ghost_bg_color, theme_col_tab_active);
          ghost_bg_color[3] = 0.3f;  /* More transparent */

          GPU_blend(GPU_BLEND_ALPHA);
          draw_roundbox_corner_set(roundboxtype);
          draw_roundbox_4fv(&ghost_box_rect, true, tab_curve_radius, ghost_bg_color);

          /* Dashed outline for ghost */
          float ghost_outline[4];
          copy_v3_v3(ghost_outline, theme_col_tab_outline_sel);
          ghost_outline[3] = 0.5f;
          draw_roundbox_4fv(&ghost_box_rect, false, tab_curve_radius, ghost_outline);

          GPU_blend(GPU_BLEND_NONE);
        }
      }

      /* Calculate dragged tab position (follows cursor) */
      rcti drag_rect = drag_tab->rect;
      const int scroll_diff = region->category_scroll - drag_state->initial_scroll;
      drag_rect.ymin -= scroll_diff;
      drag_rect.ymax -= scroll_diff;

      const int offset_y = int(drag_state->drag_offset_y);
      drag_rect.ymin += offset_y;
      drag_rect.ymax += offset_y;

      rctf box_rect;
      box_rect.xmin = float(drag_rect.xmin);
      box_rect.xmax = float(drag_rect.xmax);
      box_rect.ymin = float(drag_rect.ymin);
      box_rect.ymax = float(drag_rect.ymax);

      float drag_bg_color[4];
      copy_v4_v4(drag_bg_color, theme_col_tab_active);
      drag_bg_color[3] = 0.7f;

      GPU_blend(GPU_BLEND_ALPHA);
      draw_roundbox_corner_set(roundboxtype);
      draw_roundbox_4fv(&box_rect, true, tab_curve_radius, drag_bg_color);
      draw_roundbox_4fv(&box_rect, false, tab_curve_radius, theme_col_tab_outline_sel);

      /* Get glyph color for color indicator in TEXT_ONLY mode. */
      bool is_fallback_letter = false;
      float glyph_color[3] = {0.0f, 0.0f, 0.0f};
      const char *category_id = drag_tab->idname;
      panel_category_glyph_lookup(wm, category_id, nullptr, &is_fallback_letter, glyph_color);

      /* Draw color indicator bar for TEXT_ONLY mode to show assigned glyph color. */
      draw_category_tab_color_indicator(
          &drag_rect, glyph_color, is_left, display_mode, U.category_tabs_text_mode_show_color_indicator);

      const char *category_id_draw = IFACE_(panel_category_display_name_lookup(wm, category_id));
      const rcti *rct = &drag_rect;

      ui_panel_category_draw_content(region,
                                     wm,
                                     category_id,
                                     category_id_draw,
                                     rct,
                                     rct->xmin,
                                     rct->xmax,
                                     STREQ(category_id, category_id_active),
                                     is_left,
                                     display_mode,
                                     fontid,
                                     fstyle,
                                     fstyle_points,
                                     zoom,
                                     category_tabs_zoom,
                                     tab_v_pad_text,
                                     0.0f,
                                     theme_col_tab_text,
                                     theme_col_tab_text_sel);
    }
  }

  GPU_line_smooth(false);
  BLF_disable(fontid, BLF_ROTATION);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hover Handler
 * \{ */

static int screen_category_tabs_hover_handler(bContext *C, const wmEvent *event, void * /*userdata*/)
{
  if (!ISMOUSE_MOTION(event->type)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  bScreen *screen = CTX_wm_screen(C);
  if (!screen) {
    return WM_UI_HANDLER_CONTINUE;
  }

  wmWindow *win = CTX_wm_window(C);
  if (!win || !win->runtime->eventstate) {
    return WM_UI_HANDLER_CONTINUE;
  }

  const int mouse_x = win->runtime->eventstate->xy[0];
  const int mouse_y = win->runtime->eventstate->xy[1];
  const double current_time = BLI_time_now_seconds();

  for (ScrArea &area_check : screen->areabase) {
    for (ARegion &region_check : area_check.regionbase) {
      if (!panel_category_tabs_is_visible(&region_check)) {
        continue;
      }

      const bool mouse_in_this_region = BLI_rcti_isect_pt(&region_check.winrct, mouse_x, mouse_y);

      if (region_check.runtime->category_tabs_settings_hover) {
        if (!mouse_in_this_region) {
          const double time_since_click = current_time -
                                          region_check.runtime->category_tabs_settings_click_time;
          if (time_since_click > 0.5) {
            region_check.runtime->category_tabs_settings_hover = false;
            ED_region_tag_redraw(&region_check);
          }
        }
        else {
          const int mx_local = mouse_x - region_check.winrct.xmin;
          const int my_local = mouse_y - region_check.winrct.ymin;
          const bool mouse_over_button = BLI_rcti_isect_pt(
              &region_check.runtime->category_tabs_settings_rect, mx_local, my_local);

          if (!mouse_over_button) {
            const double time_since_click = current_time -
                                            region_check.runtime->category_tabs_settings_click_time;
            if (time_since_click > 0.5) {
              region_check.runtime->category_tabs_settings_hover = false;
              ED_region_tag_redraw(&region_check);
            }
          }
        }
      }

      if (mouse_in_this_region) {
        const int mx_local = mouse_x - region_check.winrct.xmin;
        const int my_local = mouse_y - region_check.winrct.ymin;

        bool is_over_any_tab = false;
        for (const PanelCategoryDyn &pc_dyn : region_check.runtime->panels_category) {
          if (BLI_rcti_isect_pt(&pc_dyn.rect, mx_local, my_local)) {
            is_over_any_tab = true;
            break;
          }
        }

        if (!is_over_any_tab) {
          ED_region_tag_redraw(&region_check);
        }
      }
      else {
        ED_region_tag_redraw(&region_check);
      }
    }
  }

  return WM_UI_HANDLER_CONTINUE;
}

static void screen_category_tabs_hover_handler_remove(bContext * /*C*/, void * /*userdata*/)
{
  /* Nothing to clean up */
}

void screen_category_tabs_hover_handler_add(ListBaseT<wmEventHandler> *handlers)
{
  WM_event_remove_ui_handler(handlers,
                             screen_category_tabs_hover_handler,
                             screen_category_tabs_hover_handler_remove,
                             nullptr,
                             false);
  WM_event_add_ui_handler(
      nullptr,
      handlers,
      screen_category_tabs_hover_handler,
      screen_category_tabs_hover_handler_remove,
      nullptr,
      eWM_EventHandlerFlag(0));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Show Active Tab
 * \{ */

int ui_panel_category_show_active_tab(ARegion *region, const int mval[2])
{
  if (!ED_region_panel_category_gutter_isect_xy(region, mval)) {
    return WM_UI_HANDLER_CONTINUE;
  }

  BLI_assert(BKE_regiontype_uses_category_tabs(region->runtime->type));

  const View2D *v2d = &region->v2d;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    const bool is_active = STREQ(pc_dyn.idname, region->runtime->category);
    if (!is_active) {
      continue;
    }
    const rcti *rct = &pc_dyn.rect;
    region->category_scroll = v2d->mask.ymax - (rct->ymax - region->category_scroll);

    if (pc_dyn.next) {
      const PanelCategoryDyn *pc_dyn_next = static_cast<PanelCategoryDyn *>(pc_dyn.next);
      const int tab_v_pad = rct->ymin - pc_dyn_next->rect.ymax;
      region->category_scroll -= tab_v_pad;
    }
    break;
  }
  ED_region_tag_redraw(region);
  return WM_UI_HANDLER_BREAK;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag Operator
 * \{ */

/* Forward declaration */
static void category_tab_drag_cancel(bContext *C, wmOperator *op);

static bool category_tab_drag_poll(bContext *C)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return false;
  }
  if (!panel_category_tabs_is_visible(region)) {
    return false;
  }
  /* Drag & drop is always allowed, independent of Allow Edit Category Data setting. */
  return true;
}

static wmOperatorStatus category_tab_drag_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Find clicked tab */
  PanelCategoryDyn *clicked_pc = nullptr;
  for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
    if (BLI_rcti_isect_pt(&pc_dyn.rect, event->mval[0], event->mval[1])) {
      clicked_pc = &pc_dyn;
      break;
    }
  }

  if (clicked_pc == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Check if reserved (cannot be reordered) */
  const wmWindowManager *wm = CTX_wm_manager(C);
  bool is_reserved_glyph = category_is_reserved(wm, clicked_pc->idname);

  /* Initialize drag state (allow all categories for now) */
  CategoryDragState *state = MEM_new<CategoryDragState>(__func__);
  state->is_dragging = true;
  state->is_reserved = is_reserved_glyph;

  if (is_reserved_glyph) {
    /* Create persistent tooltip with proper alignment to avoid overlapping tabs. */
    char msg[128];
    SNPRINTF(msg, "%s (Cannot Reorder)", IFACE_(clicked_pc->idname));

    /* Use the same positioning logic as hover tooltips. */
    const bool is_left = (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT);

    /* Position tooltip to avoid overlapping the tab.
     * Convert tab rect from region-local to screen coordinates. */
    rcti tab_rect_screen;
    tab_rect_screen.xmin = region->winrct.xmin + clicked_pc->rect.xmin;
    tab_rect_screen.xmax = region->winrct.xmin + clicked_pc->rect.xmax;
    tab_rect_screen.ymin = event->xy[1] - UI_UNIT_Y / 2;
    tab_rect_screen.ymax = event->xy[1] + UI_UNIT_Y / 2;

    int position[2];
    if (is_left) {
      position[0] = tab_rect_screen.xmax + UI_POPUP_MARGIN;
    }
    else {
      position[0] = tab_rect_screen.xmin - UI_POPUP_MARGIN;
    }
    position[1] = event->xy[1];

    const bool prefer_left = !is_left;
    state->tooltip_region = tooltip_create_from_text(
        C, msg, position, &tab_rect_screen, prefer_left);
    /* Store initial X position to keep tooltip fixed horizontally during drag. */
    state->tooltip_initial_x = state->tooltip_region->winrct.xmin;
  }
  else {
    /* Check if we need to show category name tooltip for Icon mode.
     * Show tooltip in Icon mode when:
     * - Show Drag Tooltips is enabled
     * - AND (Show Active Tab Name is OFF, OR category is not the active one)
     * This helps users identify tabs when only icons are visible. */
    const eUserPref_CategoryTabsDisplayMode display_mode =
        static_cast<eUserPref_CategoryTabsDisplayMode>(U.category_tabs_display_mode);
    const bool is_active = STREQ(clicked_pc->idname, region->runtime->category);

    if (display_mode == USER_CATEGORY_TABS_GLYPHS_ONLY &&
        U.category_tabs_show_drag_tooltips &&
        !(U.category_tabs_show_active_name && is_active))
    {
      /* Get category display name using the same method as hover tooltips. */
      const char *category_display_name = panel_category_tooltip_name_get(region, wm, clicked_pc->idname);
      if (category_display_name && category_display_name[0]) {
        /* Create tooltip with proper alignment to avoid overlapping tabs.
         * Uses the same positioning logic as hover tooltips. */
        const bool is_left = (RGN_ALIGN_ENUM_FROM_MASK(region->alignment) != RGN_ALIGN_RIGHT);

        /* Position tooltip to avoid overlapping the tab.
         * Convert tab rect from region-local to screen coordinates.
         * Use mouse Y position to keep tooltip aligned with cursor vertically. */
        rcti tab_rect_screen;
        tab_rect_screen.xmin = region->winrct.xmin + clicked_pc->rect.xmin;
        tab_rect_screen.xmax = region->winrct.xmin + clicked_pc->rect.xmax;
        /* Use mouse Y position to keep tooltip vertically aligned with cursor. */
        tab_rect_screen.ymin = event->xy[1] - UI_UNIT_Y / 2;
        tab_rect_screen.ymax = event->xy[1] + UI_UNIT_Y / 2;

        int position[2];
        if (is_left) {
          /* Tabs on left side: position tooltip to the right of tabs. */
          position[0] = tab_rect_screen.xmax + UI_POPUP_MARGIN;
        }
        else {
          /* Tabs on right side: position tooltip to the left of tabs. */
          position[0] = tab_rect_screen.xmin - UI_POPUP_MARGIN;
        }
        position[1] = event->xy[1];

        /* For tabs on right side, prefer left side positioning first. */
        const bool prefer_left = !is_left;
        state->tooltip_region = tooltip_create_from_text(
            C, IFACE_(category_display_name), position, &tab_rect_screen, prefer_left);
        /* Store initial X position to keep tooltip fixed horizontally during drag. */
        state->tooltip_initial_x = state->tooltip_region->winrct.xmin;
      }
    }
  }
  state->current_mouse_x = event->mval[0];
  state->current_mouse_y = event->mval[1];
  STRNCPY(state->drag_category_id, clicked_pc->idname);
  state->drag_start_y = event->mval[1];
  state->drag_tab_height = BLI_rcti_size_y(&clicked_pc->rect);

  /* Calculate offsets from click point to tab edges.
   * When moving up, we use top edge; when moving down, we use bottom edge.
   * This provides intuitive insert position feedback regardless of click position. */
  state->drag_top_edge_offset = clicked_pc->rect.ymax - event->mval[1];
  state->drag_bottom_edge_offset = clicked_pc->rect.ymin - event->mval[1];

  /* Calculate original index based on visual order, not workspace order value */
  int visual_index = 0;
  Vector<PanelCategoryDyn *> ordered_categories = get_ordered_categories(C, region);
  for (PanelCategoryDyn *pc_dyn : ordered_categories) {
    if (STREQ(pc_dyn->idname, clicked_pc->idname)) {
      break;
    }
    visual_index++;
  }
  state->original_index = visual_index;
  state->current_insert_index = state->original_index;
  state->drag_offset_y = 0.0f;
  state->prev_drag_offset_y = 0.0f;
  state->initial_scroll = region->category_scroll;
  state->tab_v_pad = 0;  /* Will be calculated during draw */
  state->insert_y_start = 0;
  state->insert_y_end = 0;

  op->customdata = state;

  /* Store initial state in region runtime */
  region->runtime->category_tabs_drag_state = state;
  region->runtime->category_tabs_drag_pending_id[0] = '\0';  /* Clear pending */

  /* Start auto-scroll timer */
  state->scroll_timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.02f);

  WM_event_add_modal_handler(C, op);

  /* Set grab cursor during drag */
  WM_cursor_modal_set(CTX_wm_window(C), state->is_reserved ? WM_CURSOR_HAND : WM_CURSOR_HAND_CLOSED);

  ED_region_tag_redraw(region);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus category_tab_drag_modal(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  CategoryDragState *state = static_cast<CategoryDragState *>(op->customdata);

  if (region == nullptr || state == nullptr) {
    if (state && state->scroll_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
      state->scroll_timer = nullptr;
    }
    WM_cursor_modal_restore(CTX_wm_window(C));
    return OPERATOR_CANCELLED;
  }

  const bool is_timer = (event->type == TIMER && event->customdata == state->scroll_timer);

  /* Update tooltip position on mouse move (for all categories with tooltip).
   * Keep tooltip fixed horizontally (X), only move vertically (Y) with cursor. */
  if (event->type == MOUSEMOVE && state->tooltip_region) {
    int dy = event->mval[1] - state->current_mouse_y;
    /* Move tooltip only vertically, keep horizontal position fixed. */
    state->tooltip_region->winrct.ymin += dy;
    state->tooltip_region->winrct.ymax += dy;
    /* Ensure X position stays at initial value. */
    int width = BLI_rcti_size_x(&state->tooltip_region->winrct);
    state->tooltip_region->winrct.xmin = state->tooltip_initial_x;
    state->tooltip_region->winrct.xmax = state->tooltip_initial_x + width;
    state->current_mouse_x = event->mval[0];
    state->current_mouse_y = event->mval[1];
    ED_region_tag_redraw(region);
  }

  /* Special handling for reserved tabs (tooltip only) */
  if (state->is_reserved) {
    /* Finish on mouse release or leaving region */
    bool finish = (event->type == LEFTMOUSE && event->val == KM_RELEASE);
    if (!finish && (event->mval[0] < 0 || event->mval[0] > region->winx ||
                    event->mval[1] < 0 || event->mval[1] > region->winy))
    {
      finish = true;
    }

    if (finish) {
      if (state->tooltip_region) {
        tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
        state->tooltip_region = nullptr;
      }
      if (state->scroll_timer) {
        WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
        state->scroll_timer = nullptr;
      }
      region->runtime->category_tabs_drag_state = nullptr;
      MEM_delete(state);
      op->customdata = nullptr;
      WM_cursor_modal_restore(CTX_wm_window(C));
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }

    return OPERATOR_RUNNING_MODAL;
  }

  if (is_timer || event->type == MOUSEMOVE || event->type == WHEELUPMOUSE ||
      event->type == WHEELDOWNMOUSE)
  {
    if (event->type == MOUSEMOVE) {
      /* Save previous offset for direction detection, then update */
      state->prev_drag_offset_y = state->drag_offset_y;
      state->drag_offset_y = float(event->mval[1] - state->drag_start_y);
    }

    bool scrolled = false;
    float scroll_amount = 0.0f;

    if (event->type == WHEELUPMOUSE) {
      scroll_amount = -20.0f * U.pixelsize;
    }
    else if (event->type == WHEELDOWNMOUSE) {
      scroll_amount = 20.0f * U.pixelsize;
    }
    else {
      /* Auto-scroll */
      const float edge_margin = 30.0f * U.pixelsize;
      const float auto_scroll_speed = 10.0f * U.pixelsize;
      /* Use calculated mouse Y because TIMER event might not have valid mval */
      int current_mouse_y = state->drag_start_y + int(state->drag_offset_y);

      if (current_mouse_y > region->winrct.ymax - region->winrct.ymin - edge_margin) {
        scroll_amount = -auto_scroll_speed;
      }
      else if (current_mouse_y < edge_margin) {
        scroll_amount = auto_scroll_speed;
      }
    }

    if (scroll_amount != 0.0f) {
      const int old_scroll = region->category_scroll;

      /* Apply scroll */
      region->category_scroll += int(scroll_amount);

      /* Note: Clamping happens in panel_category_tabs_draw_all during redraw.
       * We rely on that to keep category_scroll within valid bounds. */

      if (old_scroll != region->category_scroll) {
        scrolled = true;
        ED_region_tag_redraw(region);
      }
    }

    if (scrolled || event->type == MOUSEMOVE) {
      /* Calculate new insert index */
      const wmWindowManager *wm = CTX_wm_manager(C);
      state->current_insert_index = calculate_insert_index(C, wm, region, state);
      update_insert_zone(C, wm, region, state);

      /* Check if cursor is over a reserved tab and update cursor accordingly */
      bool over_reserved = false;
      for (PanelCategoryDyn &pc_dyn : region->runtime->panels_category) {
        if (BLI_rcti_isect_pt(&pc_dyn.rect, event->mval[0], event->mval[1])) {
          if (category_is_reserved_for_reorder(wm, pc_dyn.idname)) {
            over_reserved = true;
          }
          break;
        }
      }

      /* Set cursor: STOP if over reserved tab, otherwise closed hand for dragging */
      WM_cursor_modal_set(CTX_wm_window(C),
                          over_reserved ? WM_CURSOR_STOP : WM_CURSOR_HAND_CLOSED);

      ED_region_tag_redraw(region);
    }

    if (is_timer) {
      return OPERATOR_RUNNING_MODAL;
    }
    /* Consume mouse move and wheel events */
    return OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        /* Apply the new order (only for non-reserved tabs) */
        if (!state->is_reserved) {
          apply_category_order(C, region, state);
        }

        /* Cleanup */
        if (state->tooltip_region) {
          tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
          state->tooltip_region = nullptr;
        }
        if (state->scroll_timer) {
          WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
          state->scroll_timer = nullptr;
        }
        region->runtime->category_tabs_drag_state = nullptr;
        MEM_delete(state);
        op->customdata = nullptr;

        WM_cursor_modal_restore(CTX_wm_window(C));

        ED_region_tag_redraw(region);
        return OPERATOR_FINISHED;
      }
      break;

    case EVT_ESCKEY:
    case RIGHTMOUSE:
      /* Cancel drag */
      if (state->tooltip_region) {
        tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
        state->tooltip_region = nullptr;
      }
      if (state->scroll_timer) {
        WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
        state->scroll_timer = nullptr;
      }
      region->runtime->category_tabs_drag_state = nullptr;
      MEM_delete(state);
      op->customdata = nullptr;

      WM_cursor_modal_restore(CTX_wm_window(C));

      ED_region_tag_redraw(region);
      return OPERATOR_CANCELLED;

    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void category_tab_drag_cancel(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->category_tabs_drag_state) {
    CategoryDragState *state = static_cast<CategoryDragState *>(op->customdata);

    if (state && state->tooltip_region) {
      tooltip_free(C, CTX_wm_screen(C), state->tooltip_region);
      state->tooltip_region = nullptr;
    }

    if (state && state->scroll_timer) {
      WM_event_timer_remove(CTX_wm_manager(C), CTX_wm_window(C), (wmTimer *)state->scroll_timer);
      state->scroll_timer = nullptr;
    }

    MEM_delete(state);
    region->runtime->category_tabs_drag_state = nullptr;
    op->customdata = nullptr;

    WM_cursor_modal_restore(CTX_wm_window(C));

    ED_region_tag_redraw(region);
  }
}

void UI_OT_category_tab_drag(wmOperatorType *ot)
{
  ot->name = "Category Tab Drag";
  ot->idname = "UI_OT_category_tab_drag";
  ot->description = "Drag to reorder category tabs";

  ot->invoke = category_tab_drag_invoke;
  ot->modal = category_tab_drag_modal;
  ot->cancel = category_tab_drag_cancel;
  ot->poll = category_tab_drag_poll;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender::ui
