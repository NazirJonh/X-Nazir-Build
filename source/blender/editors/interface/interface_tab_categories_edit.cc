/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved.
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
#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
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
#include "regions/interface_regions_intern.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Static Variables for Preview and State
 * \{ */

/* Static buffers for preview callback - updated by live update callback */
static char category_tab_preview_glyph[8] = "";
static float category_tab_preview_color[3] = {0.0f, 0.0f, 0.0f};

/* Static pointer to current dialog operator - needed for Reset/Save buttons */
wmOperator *category_tab_current_dialog_op = nullptr;

/* Static pointer to popup block - needed for Save button to close popup */
Block *category_tab_popup_block = nullptr;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hex/UTF-8 Conversion Utilities
 * \{ */

bool hex_codepoint_to_utf8(const char *input, char *utf8_out, size_t utf8_max)
{
  if (!input || !input[0]) {
    return false;
  }

  const char *hex_start = input;

  /* Skip optional "0x" or "0X" prefix */
  if (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
    hex_start = input + 2;
    if (!hex_start[0]) {
      return false;
    }
  }

  /* Check if remaining string is a valid hex number (1-6 hex digits for Unicode) */
  size_t hex_len = strlen(hex_start);
  if (hex_len == 0 || hex_len > 6) {
    return false;
  }

  /* Verify all characters are hex digits */
  for (size_t i = 0; i < hex_len; i++) {
    if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
      return false;
    }
  }

  /* Parse hex to unsigned int */
  uint val = strtoul(hex_start, nullptr, 16);

  /* Validate Unicode codepoint range */
  if (val < 32 || val > 0x10FFFF) {
    return false;
  }

  /* Convert to UTF-8 using Blender's built-in function */
  const int utf8_len = BLI_str_utf8_from_unicode(val, utf8_out, utf8_max);

  /* BLI_str_utf8_from_unicode does NOT null-terminate, so we must do it */
  if (utf8_len > 0 && size_t(utf8_len) < utf8_max) {
    utf8_out[utf8_len] = '\0';
  }

  return utf8_len > 0;
}

bool process_glyph_input(const char *input, char *output, size_t output_max)
{
  if (!input || !input[0]) {
    output[0] = '\0';
    return false;
  }

  /* Try to convert as hex codepoint first */
  if (hex_codepoint_to_utf8(input, output, output_max)) {
    return true;
  }

  /* Invalid input - return empty string */
  output[0] = '\0';
  return false;
}

void utf8_to_hex_codepoint(const char *input, char *output, size_t output_max)
{
  if (!input || !input[0]) {
    output[0] = '\0';
    return;
  }

  /* Convert UTF-8 to codepoint using Blender's built-in function */
  unsigned int codepoint = BLI_str_utf8_as_unicode_safe(input);

  if (codepoint == BLI_UTF8_ERR || codepoint == 0) {
    output[0] = '\0';
    return;
  }

  /* Format as lowercase hex without "0x" prefix */
  BLI_snprintf(output, output_max, "%x", codepoint);
}

bool is_display_glyph_codepoint(unsigned int codepoint)
{
  /* Private Use Areas (font icons like Material Symbols) */
  if ((codepoint >= 0xE000 && codepoint <= 0xF8FF) ||
      (codepoint >= 0xF0000 && codepoint <= 0xFFFFD) ||
      (codepoint >= 0x100000 && codepoint <= 0x10FFFD))
  {
    return true;
  }
  /* Common symbol ranges */
  if ((codepoint >= 0x2600 && codepoint <= 0x27BF) ||
      (codepoint >= 0x1F300 && codepoint <= 0x1FAFF))
  {
    return true;
  }
  return false;
}

bool is_single_glyph_str(const char *str)
{
  if (!str || !str[0]) {
    return false;
  }
  const int utf8_char_size = BLI_str_utf8_size_safe(str);
  const size_t len = BLI_strnlen(str, 64);
  return (len == 1) || (utf8_char_size > 0 && size_t(utf8_char_size) == len);
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

  /* Get original values saved when dialog was opened */
  char original_display_name[32] = "";
  char original_glyph_hex[16] = "";
  float original_color[3] = {0.0f, 0.0f, 0.0f};
  char original_tags[256] = "";
  bool original_has_override = false;

  RNA_string_get(op->ptr, "original_display_name", original_display_name);
  RNA_string_get(op->ptr, "original_glyph", original_glyph_hex);
  RNA_float_get_array(op->ptr, "original_color", original_color);
  RNA_string_get(op->ptr, "original_tags", original_tags);
  original_has_override = RNA_boolean_get(op->ptr, "original_has_override");

  /* Convert hex glyph back to UTF-8 for restoration */
  char original_glyph_utf8[8] = "";
  process_glyph_input(original_glyph_hex, original_glyph_utf8, sizeof(original_glyph_utf8));

  wmWindowManager *wm = CTX_wm_manager(C);

  /* Find current override (may have been created by live preview) */
  CategoryGlyphItem *item = nullptr;
  for (CategoryGlyphItem *it =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       it;
       it = static_cast<CategoryGlyphItem *>(it->next))
  {
    if (STREQ(it->category, category)) {
      item = it;
      break;
    }
  }

  if (original_has_override) {
    /* There was an override before - restore original values */
    if (!item) {
      /* Override was deleted by live preview - recreate it */
      item = MEM_new<CategoryGlyphItem>(__func__);
      STRNCPY(item->category, category);
      BLI_addtail(&wm->category_glyph_overrides, item);
    }
    STRNCPY(item->display_name, original_display_name);
    STRNCPY(item->glyph, original_glyph_utf8);
    copy_v3_v3(item->color, original_color);
    STRNCPY(item->tags, original_tags);
  }
  else {
    /* There was no override before - remove any created by live preview */
    if (item) {
      /* Check if override has any user changes besides tags/glyph/color/display_name */
      bool has_changes = (item->display_name[0] != '\0' || item->glyph[0] != '\0' ||
                          !is_zero_v3(item->color) ||
                          (item->tags[0] != '\0' && original_tags[0] == '\0'));

      if (!has_changes) {
        BLI_remlink(&wm->category_glyph_overrides, item);
        MEM_delete(item);
      }
      else {
        /* Keep override but restore original values */
        STRNCPY(item->display_name, original_display_name);
        STRNCPY(item->glyph, original_glyph_utf8);
        copy_v3_v3(item->color, original_color);
        STRNCPY(item->tags, original_tags);
      }
    }
  }

  /* Clear dialog operator pointer and popup block */
  category_tab_current_dialog_op = nullptr;
  category_tab_popup_block = nullptr;

#ifdef WITH_PYTHON
  /* Restore tags in Python _glyph_cache to revert live preview changes. */
  if (category[0] != '\0') {
    wmWindowManager *wm_ptr = CTX_wm_manager(C);
    if (wm_ptr) {
      PointerRNA wm_ptr_rna = RNA_pointer_create_discrete(&wm_ptr->id, RNA_WindowManager, wm_ptr);
      RNA_string_set(&wm_ptr_rna, "category_tab_save_category", category);

      const char *imports[] = {"bpy", nullptr};
      char restore_cmd[512];
      BLI_snprintf(restore_cmd,
                   sizeof(restore_cmd),
                   "from bl_ui.space_userpref import restore_category_tags_from_string\n"
                   "import bpy\n"
                   "wm = bpy.context.window_manager\n"
                   "category = wm.category_tab_save_category\n"
                   "wm.category_tab_save_category = ''\n"
                   "if category:\n"
                   "    restore_category_tags_from_string(category, r'''%s''')\n",
                   original_tags);

      BPY_run_string_exec(C, imports, restore_cmd);
    }
  }
#endif

  /* Show info message that changes were discarded */
  WM_global_report(RPT_INFO, "Category tab changes discarded");

  WM_main_add_notifier(NC_WINDOW, nullptr);
}

void category_tab_edit_popup_ok_cb(bContext * /*C*/, void * /*user_data*/, int /*retval*/)
{
  /* Clear dialog operator pointer and popup block */
  category_tab_current_dialog_op = nullptr;
  category_tab_popup_block = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tag Filter Menu
 * \{ */

static MenuType *category_tag_filter_menu_type = nullptr;

static void category_tag_filter_menu_draw(const bContext *C, Menu *menu)
{
  Layout &layout = *menu->layout;
  wmWindowManager *wm = CTX_wm_manager(C);

  layout.label(IFACE_("Filter Tags"), ICON_NONE);
  layout.separator();

  PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
  layout.prop(&wm_ptr,
              "category_tag_filter_current_mode",
              UI_ITEM_NONE,
              IFACE_("Current Mode"),
              ICON_NONE);
}

static bool category_tag_filter_menu_poll(const bContext *C, MenuType * /*mt*/)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  return wm != nullptr;
}

void category_tag_filter_menu_register()
{
  if (category_tag_filter_menu_type != nullptr) {
    return; /* Already registered */
  }

  MenuType *mt = MEM_new_zeroed<MenuType>(__func__);
  STRNCPY_UTF8(mt->idname, "SCREEN_MT_category_tag_filter");
  STRNCPY_UTF8(mt->label, N_("Filter Tags"));
  STRNCPY_UTF8(mt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  mt->description = N_("Filter tags by mode in category tab popup");
  mt->poll = category_tag_filter_menu_poll;
  mt->draw = category_tag_filter_menu_draw;

  WM_menutype_add(mt);
  category_tag_filter_menu_type = mt;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Live Update Callback
 * \{ */

void category_tab_edit_live_update_cb(bContext *C, void *arg_op, int /*event*/)
{
  wmOperator *op = static_cast<wmOperator *>(arg_op);

  /* Guard: If the dialog is closing/cancelled, the global pointer will be null.
   * Stop processing to avoid resurrecting deleted overrides.
   */
  if (category_tab_current_dialog_op != op) {
    return;
  }

  char category[64];
  RNA_string_get(op->ptr, "category", category);

  float color[3];
  RNA_float_get_array(op->ptr, "color", color);

  char glyph_raw[16];
  RNA_string_get(op->ptr, "glyph", glyph_raw);

  /* Validate glyph input: must be empty or valid hex code (1-6 hex digits) */
  bool glyph_valid = true;
  if (glyph_raw[0] != '\0') {
    /* Check total string length (including optional 0x prefix) */
    size_t total_len = strlen(glyph_raw);
    if (total_len > 8) { /* Max: "0x" + 6 hex digits */
      glyph_valid = false;
    }
    else {
      const char *hex_start = glyph_raw;

      /* Skip optional "0x" or "0X" prefix */
      if (glyph_raw[0] == '0' && (glyph_raw[1] == 'x' || glyph_raw[1] == 'X')) {
        hex_start = glyph_raw + 2;
      }

      /* Check if remaining string is a valid hex number (1-6 hex digits for Unicode) */
      size_t hex_len = strlen(hex_start);

      if (hex_len == 0 || hex_len > 6) {
        glyph_valid = false;
      }
      else {
        /* Verify all characters are hex digits */
        for (size_t i = 0; i < hex_len; i++) {
          if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
            glyph_valid = false;
            break;
          }
        }
      }

      if (glyph_valid) {
        /* Validate Unicode codepoint range */
        uint val = strtoul(hex_start, nullptr, 16);
        if (val < 32 || val > 0x10FFFF) {
          glyph_valid = false;
        }
      }
    }
  }

  /* Process glyph input: convert hex code (e.g., "e5d2") to UTF-8 character.
   * Only process if input is valid hex. */
  char glyph[8];
  glyph[0] = '\0';

  if (glyph_valid && glyph_raw[0] != '\0') {
    process_glyph_input(glyph_raw, glyph, sizeof(glyph));
  }

  /* Update the override immediately for live preview */
  wmWindowManager *wm = CTX_wm_manager(C);

  /* Look up default glyph for preview fallback (when user input is empty or invalid) */
  bool is_fallback = false;
  const char *default_glyph = panel_category_glyph_lookup(
      wm, category, nullptr, &is_fallback, nullptr);

  /* Update preview buffers for popup preview.
   * Use the processed glyph from valid input, or fall back to default lookup.
   * Invalid input shows the default glyph (not the invalid text).
   * Fallback letter is also shown in preview.
   */
  copy_v3_v3(category_tab_preview_color, color);
  if (glyph[0] != '\0') {
    /* Valid custom glyph - show it */
    STRNCPY(category_tab_preview_glyph, glyph);
  }
  else if (default_glyph) {
    /* Empty or invalid input - show default glyph (including fallback letter) */
    STRNCPY(category_tab_preview_glyph, default_glyph);
  }
  else if (is_fallback) {
    /* default_glyph is nullptr but is_fallback is true - use first char of category */
    const int first_char_size = BLI_str_utf8_size_safe(category);
    if (first_char_size > 0) {
      memcpy(category_tab_preview_glyph, category, first_char_size);
      category_tab_preview_glyph[first_char_size] = '\0';
    }
    else {
      category_tab_preview_glyph[0] = '\0';
    }
  }
  else {
    category_tab_preview_glyph[0] = '\0';
  }

  CategoryGlyphItem *item = nullptr;

  /* Find existing override */
  for (CategoryGlyphItem *it =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       it;
       it = static_cast<CategoryGlyphItem *>(it->next))
  {
    if (STREQ(it->category, category)) {
      item = it;
      break;
    }
  }

  /* Create if not found */
  if (!item) {
    item = MEM_new<CategoryGlyphItem>(__func__);
    STRNCPY(item->category, category);
    item->display_name[0] = '\0';
    item->glyph[0] = '\0';
    zero_v3(item->color);
    item->tags[0] = '\0';  /* Initialize tags as empty */

    /* Preserve existing tags from mappings when creating new override.
     * This prevents losing tags when user modifies display_name/glyph/color. */
    const char *existing_tags = nullptr;

    /* First check mappings for existing tags */
    for (CategoryGlyphItem *map_item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         map_item;
         map_item = static_cast<CategoryGlyphItem *>(map_item->next))
    {
      if (STREQ(map_item->category, category) && map_item->tags[0] != '\0') {
        existing_tags = map_item->tags;
        break;
      }
    }

    /* If no tags in mappings, try original_tags from op->ptr */
    if (!existing_tags || existing_tags[0] == '\0') {
      char original_tags[256];
      RNA_string_get(op->ptr, "original_tags", original_tags);
      if (original_tags[0] != '\0') {
        existing_tags = original_tags;
      }
    }

    /* Copy tags to new override if found */
    if (existing_tags && existing_tags[0] != '\0') {
      STRNCPY(item->tags, existing_tags);
    }

    BLI_addtail(&wm->category_glyph_overrides, item);
  }

  /* Update display_name for live preview */
  char display_name[32];
  RNA_string_get(op->ptr, "display_name", display_name);
  if (display_name[0] != '\0') {
    STRNCPY(item->display_name, display_name);
  }

  /* Update color for live preview */
  copy_v3_v3(item->color, color);

  /* Update glyph in override only if valid.
   * Save the processed glyph if user has entered something.
   * Note: We always save the glyph because panel_category_glyph_lookup returns
   * the override glyph if it exists, which would cause a false "match" condition.
   */
  if (glyph_valid && glyph[0] != '\0') {
    /* User has entered a valid glyph - save it to override */
    STRNCPY(item->glyph, glyph);
  }
  else if (!glyph_valid) {
    /* Invalid glyph - don't update override, keep previous value */
  }
  else {
    /* Empty glyph - clear override glyph to use defaults */
    item->glyph[0] = '\0';
  }

  /* Trigger redraw to show the updated color */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Search Callback
 * \{ */

void category_tab_edit_glyph_search_cb(bContext * /*C*/, void *arg_op, int /*event*/)
{
  wmOperator *op = static_cast<wmOperator *>(arg_op);

  /* Guard: If the dialog is closing/cancelled, the global pointer will be null */
  if (category_tab_current_dialog_op != op) {
    return;
  }

  char glyph_search_query[64];
  RNA_string_get(op->ptr, "glyph_search", glyph_search_query);

  /* TODO: Implement glyph search results display
   * This callback will be triggered when the glyph_search field changes.
   * It should:
   * 1. Call Python API search_glyphs(query, category, max_results)
   * 2. Store results in window_manager.glyph_search_results
   * 3. Trigger redraw to update UI with search results
   */

  /* Trigger redraw to show search results */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glyph Search Helper Functions
 * \{ */

/**
 * Call Python API to search glyphs.
 * Returns a list of glyph dictionaries with 'unicode' and 'name' keys.
 * Uses BPY_run_string_as_string to execute Python code and parse JSON result.
 */
/**
 * Helper function to safely print a string that might contain Unicode characters.
 * Prints the hex codepoint representation for Unicode glyphs to avoid console encoding issues.
 */
static void safe_print_string(const char *label, const char *str)
{
  if (!str || !str[0]) {
    printf("[%s] (empty)\n", label);
    return;
  }

  /* Check if this is a single Unicode glyph character */
  if (is_single_glyph_str(str)) {
    char hex_cp[32] = "";
    utf8_to_hex_codepoint(str, hex_cp, sizeof(hex_cp));
    printf("[%s] U+%s\n", label, hex_cp);
  }
  else {
    printf("[%s] %s\n", label, str);
  }
}

/**
 * Decode JSON Unicode escape sequences (like \uXXXX) to UTF-8.
 * For example, "\ue5d4" becomes the actual UTF-8 bytes for U+E5D4.
 *
 * \param str: Input string with possible \uXXXX escapes
 * \return Decoded UTF-8 string
 */
static std::string decode_json_unicode(const char *str)
{
  if (!str) {
    return "";
  }

  std::string result;
  size_t len = strlen(str);
  size_t i = 0;

  while (i < len) {
    /* Check for Unicode escape sequence \uXXXX */
    if (i + 5 < len && str[i] == '\\' && str[i + 1] == 'u') {
      /* Parse 4 hex digits */
      char hex_str[5] = {str[i + 2], str[i + 3], str[i + 4], str[i + 5], 0};

      /* Convert hex string to integer */
      uint32_t codepoint = 0;
      for (int j = 0; j < 4; j++) {
        char c = hex_str[j];
        codepoint <<= 4;
        if (c >= '0' && c <= '9') {
          codepoint |= (c - '0');
        }
        else if (c >= 'a' && c <= 'f') {
          codepoint |= (c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F') {
          codepoint |= (c - 'A' + 10);
        }
      }

      /* Convert codepoint to UTF-8 */
      char utf8_buf[5];
      int utf8_len = 0;

      if (codepoint <= 0x7F) {
        /* 1 byte: 0xxxxxxx */
        utf8_buf[0] = (char)codepoint;
        utf8_len = 1;
      }
      else if (codepoint <= 0x7FF) {
        /* 2 bytes: 110xxxxx 10xxxxxx */
        utf8_buf[0] = (char)(0xC0 | (codepoint >> 6));
        utf8_buf[1] = (char)(0x80 | (codepoint & 0x3F));
        utf8_len = 2;
      }
      else if (codepoint <= 0xFFFF) {
        /* 3 bytes: 1110xxxx 10xxxxxx 10xxxxxx */
        utf8_buf[0] = (char)(0xE0 | (codepoint >> 12));
        utf8_buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8_buf[2] = (char)(0x80 | (codepoint & 0x3F));
        utf8_len = 3;
      }
      else {
        /* 4 bytes: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
        utf8_buf[0] = (char)(0xF0 | (codepoint >> 18));
        utf8_buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        utf8_buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        utf8_buf[3] = (char)(0x80 | (codepoint & 0x3F));
        utf8_len = 4;
      }

      result.append(utf8_buf, utf8_len);
      i += 6; /* Skip \uXXXX */
    }
    else {
      /* Copy character as-is */
      result += str[i];
      i++;
    }
  }

  return result;
}

static blender::Vector<std::pair<std::string, std::string>> glyph_search_call_python(
    bContext *C, const char *query, const char *category, int max_results)
{
  blender::Vector<std::pair<std::string, std::string>> results;

#ifdef WITH_PYTHON
  /* Debug: Print input parameters safely */
  printf("[GLYPH SEARCH] Query: '%s', ", query ? query : "");
  safe_print_string("Category", category);
  printf("[GLYPH SEARCH] Max: %d\n", max_results);

  /* Build Python script to call glyph search and return JSON result */
  /* Note: We don't pass category to Python due to encoding issues with Unicode glyph characters.
   * Category filtering will be done in C++ after getting results. */

  /* Escape query for Python string */
  std::string escaped_query = query ? query : "";
  size_t pos = 0;
  while ((pos = escaped_query.find("\"", pos)) != std::string::npos) {
    escaped_query.replace(pos, 1, "\\\"");
    pos += 2;
  }
  /* Escape backslashes */
  pos = 0;
  while ((pos = escaped_query.find("\\", pos)) != std::string::npos) {
    escaped_query.replace(pos, 1, "\\\\");
    pos += 2;
  }

  /* Prepare Python imports array - only json needs to be imported */
  const char *imports[] = {
      "json",
      nullptr
  };

  /* Create Python expression using __import__ to access the registry module */
  /* This avoids the issue of imported modules not being in eval() scope */
  char python_expr[2048];

  snprintf(
      python_expr,
      sizeof(python_expr),
      "json.dumps([{'unicode': g['unicode'], 'name': g['name']} "
      "for g in __import__('bl_ui.glyph_library.registry', fromlist=['']).search_glyphs('%s', '', %d)])",
      escaped_query.c_str(),
      max_results);

  printf("[GLYPH SEARCH] Python expr length: %d\n", int(strlen(python_expr)));

  /* Print the Python expression for debugging */
  printf("[GLYPH SEARCH] === Python expr ===\n");
  printf("%s\n", python_expr);
  printf("[GLYPH SEARCH] === End expr ===\n");

  /* Execute Python expression and capture output */
  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};

  /* Use BPY_run_string_as_string with imports array */
  bool success = BPY_run_string_as_string(C, imports, python_expr, &err_info, &result_str);
  printf("[GLYPH SEARCH] BPY_run_string_as_string success: %d\n", success);

  /* Print error details if failed */
  if (!success) {
    printf("[GLYPH SEARCH] Execution failed!\n");
    if (err_msg) {
      printf("[GLYPH SEARCH] Error string: %s\n", err_msg);
      MEM_delete(err_msg);
    }
  }

  if (success && result_str) {
    /* Safe print of result (may contain Unicode) */
    size_t result_len = strlen(result_str);
    if (result_len > 200) {
      printf("[GLYPH SEARCH] Result length: %zu bytes (first 150 chars)\n", result_len);
      /* Print ascii-safe version */
      for (size_t i = 0; i < 150 && i < result_len; i++) {
        unsigned char c = (unsigned char)result_str[i];
        if (c >= 32 && c <= 126) {
          putchar(c);
        }
        else {
          putchar('?');
        }
      }
      printf("\n");
    }
    else {
      printf("[GLYPH SEARCH] Result: %s\n", result_str);
    }

    /* Check if result is an error message */
    if (strncmp(result_str, "{\"error\":", 9) == 0) {
      printf("[GLYPH SEARCH] Python error: %s\n", result_str);
      MEM_delete(result_str);
      return results;
    }

    /* Check if result is empty array */
    if (strcmp(result_str, "[]") == 0) {
      printf("[GLYPH SEARCH] Empty result (no glyphs found)\n");
      MEM_delete(result_str);
      return results;
    }

    /* Parse JSON result */
    const char *p = result_str;

    /* Skip to array start */
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    else {
      printf("[GLYPH SEARCH] Result is not a JSON array: %s\n", result_str);
      MEM_delete(result_str);
      return results;
    }

    int parsed_count = 0;

    /* Parse array elements */
    while (*p && *p != ']') {
      /* Skip to object start */
      while (*p && *p != '{') p++;
      if (*p == '{') p++;

      std::string unicode;
      std::string name;

      /* Parse object fields */
      while (*p && *p != '}') {
        /* Parse unicode field */
        if (strncmp(p, "\"unicode\":", 10) == 0) {
          p += 10;
          while (*p && *p != '"') p++;
          if (*p == '"') p++;
          const char *start = p;
          while (*p && *p != '"') {
            p++;
          }
          std::string raw_unicode = std::string(start, p - start);
          /* Decode JSON Unicode escape sequences like \ue5d4 to actual UTF-8 */
          unicode = decode_json_unicode(raw_unicode.c_str());
          printf("[GLYPH SEARCH] Decoded unicode: '%s' -> ", raw_unicode.c_str());
          safe_print_string("result", unicode.c_str());
        }
        /* Parse name field */
        else if (strncmp(p, "\"name\":", 7) == 0) {
          p += 7;
          while (*p && *p != '"') p++;
          if (*p == '"') p++;
          const char *start = p;
          while (*p && *p != '"') {
            p++;
          }
          name = std::string(start, p - start);
        }
        else {
          p++;
        }
      }

      if (!unicode.empty() && !name.empty()) {
        printf("[GLYPH SEARCH] Found glyph %d: name='%s', ", ++parsed_count, name.c_str());
        safe_print_string("unicode", unicode.c_str());
        results.append({unicode, name});

        if (results.size() >= max_results) {
          break;
        }
      }

      /* Move to next object or end of array */
      while (*p && *p != ',' && *p != ']') p++;
      if (*p == ',' || *p == '}') p++;
    }

    MEM_delete(result_str);
  }
  else {
    printf("[GLYPH SEARCH] Failed or no result\n");
  }

  printf("[GLYPH SEARCH] Total glyphs found: %d\n", int(results.size()));
#else
  (void)query;
  (void)category;
  (void)max_results;
#endif

  return results;
}

/* -------------------------------------------------------------------- */
/** \name Glyph Cache
 * \{ */

/* Static cache for glyphs - persists until explicitly cleared or Blender exits */
static blender::Vector<std::pair<std::string, std::string>> g_glyph_cache;
static bool g_glyph_cache_valid = false;

/**
 * Clear the glyph cache. Call this when glyphs need to be reloaded.
 */
static void glyph_cache_clear()
{
  g_glyph_cache.clear();
  g_glyph_cache_valid = false;
}

/**
 * Get glyphs from cache, or load them if cache is invalid.
 * Returns a reference to the cached glyph vector.
 */
static const blender::Vector<std::pair<std::string, std::string>>& glyph_cache_get(bContext *C)
{
  if (!g_glyph_cache_valid) {
    printf("[GLYPH CACHE] Cache miss, loading glyphs from Python...\n");
    g_glyph_cache = glyph_search_call_python(C, "", "", 1000);
    g_glyph_cache_valid = true;
    printf("[GLYPH CACHE] Cached %d glyphs\n", int(g_glyph_cache.size()));
  }
  else {
    printf("[GLYPH CACHE] Cache hit, using %d cached glyphs\n", int(g_glyph_cache.size()));
  }
  return g_glyph_cache;
}

/** \} */

/**
 * Callback for when a search result button is clicked.
 * Sets the glyph code and updates the preview.
 */
static void glyph_search_result_button_cb(bContext *C, void *arg1, void *arg2)
{
  wmOperator *op = static_cast<wmOperator *>(arg1);
  const char *glyph_unicode = static_cast<const char *>(arg2);

  if (!op || !glyph_unicode) {
    return;
  }

  /* Convert unicode to hex codepoint */
  char hex_code[16] = "";
  utf8_to_hex_codepoint(glyph_unicode, hex_code, sizeof(hex_code));

  /* Set the glyph property */
  RNA_string_set(op->ptr, "glyph", hex_code);

  /* Clear the search field */
  RNA_string_set(op->ptr, "glyph_search", "");

  /* Trigger live update to refresh preview */
  category_tab_edit_live_update_cb(C, op, 0);

  /* Trigger redraw */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Glyph Grid Popup
 * \{ */

/**
 * Data structure for glyph grid popup.
 * Stores the operator, glyphs, categories and UI state.
 */
struct GlyphGridPopupData {
  wmOperator *op;
  Vector<std::pair<std::string, std::string>> glyphs; /* (unicode, name) pairs */
  std::string current_category; /* Currently selected category filter */
  Vector<std::string> categories; /* All available categories */
  std::string search_string; /* Search filter */
  PopupBlockHandle *popup_handle;

  GlyphGridPopupData(wmOperator *op_,
                     Vector<std::pair<std::string, std::string>> glyphs_,
                     std::string category_,
                     Vector<std::string> categories_)
      : op(op_),
        glyphs(std::move(glyphs_)),
        current_category(std::move(category_)),
        categories(std::move(categories_)),
        search_string(""),
        popup_handle(nullptr)
  {
  }
};

/* -------------------------------------------------------------------- */
/** \name Glyph Category Tree View
 * \{ */

/**
 * Tree view for displaying and selecting glyph categories.
 * Similar to Asset Shelf's catalog tree.
 */
class GlyphCategoryTreeView : public AbstractTreeView {
  /** Reference to the popup data to update selected category */
  GlyphGridPopupData *popup_data_;

 public:
  GlyphCategoryTreeView(GlyphGridPopupData *popup_data) : popup_data_(popup_data) {}

  void build_tree() override
  {
    /* "ALL" item - shows all glyphs regardless of category (default) */
    BasicTreeViewItem &all_item = this->add_tree_item<BasicTreeViewItem>(IFACE_("ALL"));
    all_item.set_on_activate_fn([this](bContext &C, BasicTreeViewItem &) {
      popup_data_->current_category = "ALL";
      send_redraw_notifier(C);
    });
    all_item.set_is_active_fn([this]() -> bool {
      return popup_data_->current_category == "ALL" || popup_data_->current_category.empty();
    });
    all_item.uncollapse_by_default();

    /* Add each category as a tree item */
    for (const std::string &category : popup_data_->categories) {
      if (category == "ALL") {
        continue; /* Skip ALL, already added above */
      }

      BasicTreeViewItem &category_item = this->add_tree_item<BasicTreeViewItem>(category);
      category_item.set_on_activate_fn([this, category](bContext &C, BasicTreeViewItem &) {
        popup_data_->current_category = category;
        send_redraw_notifier(C);
      });
      category_item.set_is_active_fn([this, category]() -> bool {
        return popup_data_->current_category == category;
      });
    }

    /* Keep the popup open when clicking a category */
    this->set_popup_keep_open();
  }

 private:
  void send_redraw_notifier(const bContext &C)
  {
    WM_main_add_notifier(NC_WINDOW, nullptr);
  }
};

/** \} */

constexpr int GLYPH_POPUP_LEFT_COL_WIDTH = 12;
constexpr int GLYPH_POPUP_RIGHT_COL_WIDTH = 45;

/**
 * Block creation function for glyph grid popup.
 * Creates a popup with category tree view (left), search (top right), and grid view (right).
 * Similar layout to Asset Shelf popup.
 */
static Block *glyph_grid_popup_block_create(bContext *C, ARegion *region, void *arg)
{
  GlyphGridPopupData *popup_data = static_cast<GlyphGridPopupData *>(arg);

  /* Sync search string from RNA property */
  char search_buf[256] = "";
  RNA_string_get(popup_data->op->ptr, "glyph_search", search_buf);
  popup_data->search_string = search_buf;

  /* Create block */
  Block *block = block_begin(C, region, "glyph_grid_popup", EmbossType::Emboss);
  block_flag_enable(block, BLOCK_LOOP | BLOCK_MOVEMOUSE_QUIT);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);

  /* Set popup size - similar to Asset Shelf, but with larger height */
  const int popup_width = (GLYPH_POPUP_LEFT_COL_WIDTH + GLYPH_POPUP_RIGHT_COL_WIDTH) * UI_UNIT_X;
  const int popup_height = 175 * UI_UNIT_Y; /* 5x larger height (35 * 5 = 175) with scroll */
  /* Center the popup on screen */
  block_bounds_set_centered(block, 6 * UI_SCALE_FAC);

  /* Create main layout */
  Layout &layout = block_layout(block,
                                LayoutDirection::Vertical,
                                LayoutType::Panel,
                                0,
                                0,
                                popup_width,
                                popup_height,
                                0,
                                style_get());

  /* Create two-column layout like Asset Shelf */
  Layout &row = layout.row(false);

  /* Left column: Category tree view */
  Layout &left_col = row.column(false);
  left_col.ui_units_x_set(GLYPH_POPUP_LEFT_COL_WIDTH);
  left_col.ui_units_y_set(150); /* Match height with right column */
  left_col.fixed_size_set(true);

  /* Add category tree view to left column */
  std::unique_ptr<GlyphCategoryTreeView> category_tree_ptr =
      std::make_unique<GlyphCategoryTreeView>(popup_data);
  AbstractTreeView *category_tree =
      block_add_view(*block, "glyph_category_tree", std::move(category_tree_ptr));
  TreeViewBuilder::build_tree_view(*C, *category_tree, left_col);

  /* Right column: Search + Grid view */
  Layout &right_col = row.column(false);
  right_col.ui_units_y_set(150); /* Set minimum height for right column */
  right_col.fixed_size_set(true);

  /* Add search field at top of right column */
  Layout &search_row = right_col.row(false);

  /* Create RNA pointer for search filter property - use op->ptr directly */
  PointerRNA *op_ptr = popup_data->op->ptr;

  /* Use prop() to create search field, similar to Asset Shelf */
  search_row.prop(op_ptr,
                  "glyph_search",
                  /* Force the button to be active in a semi-modal state. */
                  ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE,
                  "",
                  ICON_VIEWZOOM);

  /* Grid view below search - with scroll support */
  Layout &grid_col = right_col.column(false);
  /* Enable vertical scroll for the grid */
  grid_col.ui_units_x_set(GLYPH_POPUP_RIGHT_COL_WIDTH);
  grid_col.fixed_size_set(true);

  /* Create scrollable container for grid */
  Layout &scroll_col = grid_col.column(false);

  /* Create Grid View */
  std::unique_ptr<GlyphGridView> grid_view_ptr = std::make_unique<GlyphGridView>();
  GlyphGridView *grid_view = grid_view_ptr.get();

  grid_view->set_glyphs(popup_data->glyphs);
  /* Don't set category filter for now - categories will be handled by Python API */
  grid_view->set_search_filter(popup_data->search_string);

  /* Set tile size - larger for better glyph visibility */
  grid_view->set_tile_size(UI_UNIT_X * 2, UI_UNIT_Y * 2);

  /* Set the selection callback */
  grid_view->set_on_glyph_select_fn(
      [popup_data](bContext &C, const std::string &unicode) {
        /* Convert unicode to hex codepoint */
        char hex_code[16] = "";
        utf8_to_hex_codepoint(unicode.c_str(), hex_code, sizeof(hex_code));
        printf("[DEBUG C++] glyph_selected_callback: unicode='%s', hex_code='%s'\n", unicode.c_str(), hex_code);

        /* Set the glyph property of the operator itself */
        RNA_string_set(popup_data->op->ptr, "glyph", hex_code);
        printf("[DEBUG C++] Set picker's own glyph property to '%s'\n", hex_code);

        /* Set the target property if specified (RNA path) */
        char target_prop[256] = "";
        RNA_string_get(popup_data->op->ptr, "target_property", target_prop);
        printf("[DEBUG C++] glyph_selected_callback: target_property='%s'\n", target_prop);
        if (target_prop[0] != '\0') {
          const char *target_prop_path = target_prop;
          if (STRPREFIX(target_prop_path, "window_manager.")) {
            target_prop_path += strlen("window_manager.");
          }

          PointerRNA target_ptr;
          PropertyRNA *prop;
          int index;

          wmOperator *target_op = nullptr;
          char target_op_ptr_str[64] = "";
          RNA_string_get(popup_data->op->ptr, "target_operator_ptr", target_op_ptr_str);
          printf("[DEBUG C++] target_operator_ptr='%s'\n", target_op_ptr_str);
          if (target_op_ptr_str[0] != '\0') {
            const uintptr_t target_op_ptr = uintptr_t(strtoull(target_op_ptr_str, nullptr, 10));
            printf("[DEBUG C++] Converted target_op_ptr=%llu\n", (unsigned long long)target_op_ptr);
            if (target_op_ptr != 0) {
              wmWindowManager *wm = CTX_wm_manager(&C);
              printf("[DEBUG C++] Searching for operator in wm->runtime->operators...\n");
              int op_count = 0;
              for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
                   op_iter;
                   op_iter = op_iter->prev)
              {
                op_count++;
                printf("[DEBUG C++]   Checking op_iter=%llu (count=%d)\n",
                       (unsigned long long)(uintptr_t)op_iter, op_count);
                if ((uintptr_t)op_iter == target_op_ptr) {
                  target_op = op_iter;
                  printf("[DEBUG C++] FOUND target operator at %llu!\n", (unsigned long long)(uintptr_t)target_op);
                  break;
                }
              }
              printf("[DEBUG C++] Total operators scanned: %d\n", op_count);
            }
          } else {
            printf("[DEBUG C++] target_operator_ptr is EMPTY!\n");
          }

          bool resolved = false;
          const bool is_active_operator_path = STRPREFIX(target_prop_path, "active_operator.");
          const char *target_prop_path_for_operator = is_active_operator_path ?
                                                          (target_prop_path + strlen("active_operator.")) :
                                                          target_prop_path;

          printf("[DEBUG C++] target_op=%p, target_op->ptr=%p\n", (void*)target_op, target_op ? (void*)target_op->ptr : nullptr);
          printf("[DEBUG C++] target_prop_path_for_operator='%s'\n", target_prop_path_for_operator);
          if (target_op && target_op->ptr) {
            printf("[DEBUG C++] Attempting to resolve property from target_op...\n");
            if (RNA_path_resolve_full(
                    target_op->ptr, target_prop_path_for_operator, &target_ptr, &prop, &index))
            {
              printf("[DEBUG C++] SUCCESS: Property resolved, setting to '%s'\n", hex_code);
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              printf("[DEBUG C++] Called RNA_property_update after setting property!\n");
              resolved = true;
            } else {
              printf("[DEBUG C++] FAILED: RNA_path_resolve_full returned false\n");
            }
          }

          if (STRPREFIX(target_prop_path, "active_operator.")) {
            const char *active_op_path = target_prop_path + strlen("active_operator.");
            printf("[DEBUG C++] Path starts with 'active_operator.', active_op_path='%s'\n", active_op_path);
            if (!resolved) {
              wmOperator *active_op = context_active_operator_get(&C);
              printf("[DEBUG C++] active_op=%p, active_op->ptr=%p\n", (void*)active_op, active_op ? (void*)active_op->ptr : nullptr);
              if (active_op && active_op->ptr) {
                if (RNA_path_resolve_full(active_op->ptr, active_op_path, &target_ptr, &prop, &index))
                {
                  printf("[DEBUG C++] SUCCESS: Resolved via active_op, setting to '%s'\n", hex_code);
                  RNA_property_string_set(&target_ptr, prop, hex_code);
                  RNA_property_update(&C, &target_ptr, prop);
                  printf("[DEBUG C++] Called RNA_property_update (active_op)!\n");
                  resolved = true;
                } else {
                  printf("[DEBUG C++] FAILED: RNA_path_resolve_full on active_op returned false\n");
                }
              }
            }
          }
          if (!resolved) {
            printf("[DEBUG C++] Not resolved yet, trying window manager root...\n");
            /* Try resolving from window manager as root. */
            wmWindowManager *wm = CTX_wm_manager(&C);
            PointerRNA root_ptr = RNA_id_pointer_create(&wm->id);
            if (RNA_path_resolve_full(&root_ptr, target_prop_path, &target_ptr, &prop, &index)) {
              printf("[DEBUG C++] SUCCESS: Resolved via window manager, setting to '%s'\n", hex_code);
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              printf("[DEBUG C++] Called RNA_property_update (window manager)!\n");
              resolved = true;
            } else {
              printf("[DEBUG C++] FAILED: RNA_path_resolve_full on window manager returned false\n");
            }
          }

          if (!resolved) {
            /* Fallback: try resolving from the picker operator itself (relative path). */
            printf("[DEBUG C++] Trying fallback: picker operator itself...\n");
            if (RNA_path_resolve_full(popup_data->op->ptr, target_prop_path, &target_ptr, &prop, &index)) {
              printf("[DEBUG C++] Fallback SUCCESS: Setting picker's own property to '%s'\n", hex_code);
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              printf("[DEBUG C++] Called RNA_property_update (fallback)!\n");
            } else {
              printf("[DEBUG C++] Fallback FAILED: Could not resolve property\n");
            }
          } else {
            printf("[DEBUG C++] Property was successfully resolved (resolved=%d)\n", resolved);
          }
        }

        /* Clear the search field */
        RNA_string_set(popup_data->op->ptr, "glyph_search", "");

        /* Trigger live update to refresh preview - only for category tab edit dialog */
        if (STREQ(popup_data->op->idname, "SCREEN_OT_category_tab_edit_dialog")) {
          category_tab_edit_live_update_cb(&C, popup_data->op, 0);
        }

        /* Close the popup */
        if (popup_data->popup_handle) {
          popup_data->popup_handle->menuretval = RETURN_OK;
        }

        /* Trigger redraw */
        WM_main_add_notifier(NC_WINDOW, nullptr);
      });

  /* Add grid view to block and build it */
  AbstractGridView *grid = block_add_view(*block, "glyph_grid", std::move(grid_view_ptr));

  /* Build the grid view */
  GridViewBuilder builder(*block);
  builder.build_grid_view(*C, *grid, scroll_col);
  
  /* Add a large spacer to ensure minimum height */
  scroll_col.separator_spacer();
  
  return block;
}

/**
 * Free function for glyph grid popup data.
 */
static void glyph_grid_popup_free(void *arg)
{
  GlyphGridPopupData *popup_data = static_cast<GlyphGridPopupData *>(arg);
  delete popup_data;
}

/**
 * Callback for the "More glyphs" button.
 * Opens the Grid View popup for selecting glyphs with category tree and search.
 */
static void glyph_more_glyphs_button_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  wmOperator *op = static_cast<wmOperator *>(arg1);

  if (!op) {
    return;
  }

  /* Get the current category from op->ptr */
  char current_category[64] = "";
  RNA_string_get(op->ptr, "category", current_category);

  /* Predefined glyph categories (will be expanded in the future) */
  Vector<std::string> all_categories;
  all_categories.append("ALL"); /* Default category - all glyphs */
  /* Future categories will be added here:
   * all_categories.append("Actions");
   * all_categories.append("Activities");
   * all_categories.append("Android");
   * all_categories.append("Audio & Video");
   * all_categories.append("Business");
   * all_categories.append("Communicate");
   * all_categories.append("Hardware");
   * all_categories.append("Home");
   * all_categories.append("Household");
   * all_categories.append("Images");
   * all_categories.append("Maps");
   * all_categories.append("Others");
   * all_categories.append("Privacy");
   * all_categories.append("Social");
   * all_categories.append("Text");
   * all_categories.append("Transit");
   * all_categories.append("Travel");
   * all_categories.append("UI actions");
   */

  /* Get glyphs from cache (loads from Python on first call) */
  const auto &cached_glyphs = glyph_cache_get(C);

  if (cached_glyphs.is_empty()) {
    /* No glyphs found, show a message */
    WM_global_report(RPT_WARNING, "No glyphs found");
    return;
  }

  /* Create a copy of glyphs for popup data (popup needs ownership) */
  blender::Vector<std::pair<std::string, std::string>> glyphs = cached_glyphs;

  /* Create popup data with current category and all categories */
  GlyphGridPopupData *popup_data = new GlyphGridPopupData(
      op, std::move(glyphs), current_category, all_categories);

  /* Create and show popup */
  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, glyph_grid_popup_block_create, nullptr, popup_data, glyph_grid_popup_free, false);

  /* Store handle for closing popup from callback */
  popup_data->popup_handle = handle;

  /* Make it a popup */
  handle->popup = true;

  /* Add handlers */
  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);
}

/**
 * Operator to open the glyph picker grid from Python or other places.
 */
static wmOperatorStatus glyph_picker_grid_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  /* Predefined glyph categories */
  Vector<std::string> all_categories;
  all_categories.append("ALL");

  /* Get glyphs from cache */
  const auto &cached_glyphs = glyph_cache_get(C);
  if (cached_glyphs.is_empty()) {
    WM_global_report(RPT_WARNING, "No glyphs found");
    return OPERATOR_CANCELLED;
  }

  /* Create a copy of glyphs for popup data */
  blender::Vector<std::pair<std::string, std::string>> glyphs = cached_glyphs;

  /* Get initial category and search string from operator properties if provided */
  char initial_category[64] = "";
  RNA_string_get(op->ptr, "category", initial_category);

  wmOperator *active_op = context_active_operator_get(C);
  printf("[DEBUG C++] glyph_picker_grid_invoke: active_op=%p, op=%p\n", (void*)active_op, (void*)op);
  if (active_op && active_op != op && active_op->properties) {
    char target_op_ptr_str[64];
    BLI_snprintf(target_op_ptr_str,
                 sizeof(target_op_ptr_str),
                 "%llu",
                 (unsigned long long)(uintptr_t)active_op);
    printf("[DEBUG C++] Setting target_operator_ptr='%s' (active_op=%p)\n", target_op_ptr_str, (void*)active_op);
    RNA_string_set(op->ptr, "target_operator_ptr", target_op_ptr_str);
  } else {
    printf("[DEBUG C++] active_op is null or same as op or no properties\n");
  }

  /* Debug: Check if target_operator_ptr was already set from Python */
  char existing_target_ptr[64] = "";
  RNA_string_get(op->ptr, "target_operator_ptr", existing_target_ptr);
  printf("[DEBUG C++] Existing target_operator_ptr in op->ptr: '%s'\n", existing_target_ptr);

  /* Create popup data */
  GlyphGridPopupData *popup_data = new GlyphGridPopupData(
      op, std::move(glyphs), initial_category, all_categories);

  /* Create and show popup */
  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, glyph_grid_popup_block_create, nullptr, popup_data, glyph_grid_popup_free, false);

  popup_data->popup_handle = handle;
  handle->popup = true;

  /* Add handlers */
  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);

  return OPERATOR_RUNNING_MODAL;
}

void WM_OT_glyph_picker_grid(wmOperatorType *ot)
{
  ot->name = "Glyph Picker";
  ot->idname = "WM_OT_glyph_picker_grid";
  ot->description = "Open a grid-based glyph picker popup";

  ot->invoke = glyph_picker_grid_invoke;
  ot->poll = ED_operator_regionactive;

  /* Properties */
   RNA_def_string(ot->srna, "category", nullptr, 64, "Category", "Initial category to show");
   RNA_def_string(ot->srna, "glyph", nullptr, 16, "Glyph", "Selected glyph hex code (output)");
   RNA_def_string(ot->srna, "glyph_search", nullptr, 64, "Search", "Search string");
   RNA_def_string(ot->srna, "target_property", nullptr, 256, "Target Property", "RNA path to property that will receive the glyph hex code");
   RNA_def_string(ot->srna, "target_operator_ptr", nullptr, 64, "Target Operator Pointer", "Internal: pointer to target operator properties");
 }

/** \} */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Popup Block Creation
 * \{ */

Block *category_tab_edit_block_create(bContext *C, ARegion *region, void *user_data)
{
  wmOperator *op = static_cast<wmOperator *>(user_data);
  const uiStyle *style = style_get_dpi();

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

  /* Category name field - TEXT ONLY, no glyph */
  char display_name[32] = "";
  RNA_string_get(op->ptr, "display_name", display_name);

  /* If display_name is empty, use category as default */
  if (display_name[0] == '\0') {
    /* Extract any leading glyph from category name (shouldn't happen, but be safe) */
    char extracted_glyph[16];
    char clean_name[64];
    if (extract_leading_glyph(
            category, extracted_glyph, sizeof(extracted_glyph), clean_name, sizeof(clean_name)))
    {
      /* If clean_name is empty after extraction, find panel label */
      if (clean_name[0] == '\0') {
        ARegion *ctx_region = CTX_wm_region(C);
        const char *panel_label = find_panel_label_for_category(ctx_region, category);
        if (panel_label) {
          STRNCPY(clean_name, panel_label);
        }
      }
      RNA_string_set(op->ptr, "display_name", clean_name);
      /* Also set the extracted glyph if glyph field is empty */
      char current_glyph[16];
      RNA_string_get(op->ptr, "glyph", current_glyph);
      if (current_glyph[0] == '\0') {
        RNA_string_set(op->ptr, "glyph", extracted_glyph);
      }
    }
    else {
      /* Check if category is a single glyph - find panel label */
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

  /* Change Icon panel */
  PanelLayout icon_panel = layout.panel(C, "change_icon", false);

  if (icon_panel.header) {
    icon_panel.header->label(IFACE_("Change the display"), ICON_NONE);
  }

  if (icon_panel.body) {
    /* Properties for glyph search and code */
    Layout &col_glyph = icon_panel.body->column(false);

    /* Row with Search Glyph on left and Code with Paste on right */
    Layout &row_search_glyph = col_glyph.row(false);
    Layout &split_search_glyph = row_search_glyph.split(0.7f, false);

    /* Left side: Search Glyph field - right aligned */
    Layout &col_search = split_search_glyph.column(false);

    /* Search field - use split to control width, align right side */
    Layout &split_search = col_search.split(0.89f, false);
    Layout &search_row = split_search.row(false);
    search_row.alignment_set(LayoutAlign::Right);
    search_row.prop(op->ptr, "glyph_search", UI_ITEM_NONE, IFACE_("Glyph"), ICON_VIEWZOOM);

    /* Display search results if glyph_search field is not empty */
    char glyph_search_query[64] = "";
    RNA_string_get(op->ptr, "glyph_search", glyph_search_query);

    if (glyph_search_query[0] != '\0') {
      /* Create a row for search results */
      Layout &results_row = col_glyph.row(false);
      results_row.alignment_set(LayoutAlign::Center);

      /* Create grid for search result buttons (5 columns) */
      Layout &results_grid = results_row.grid_flow(true, 5, true, false, false);

      /* Call Python API to search glyphs */
      char category[64];
      RNA_string_get(op->ptr, "category", category);
      auto search_results = glyph_search_call_python(C, glyph_search_query, category, 50);

      /* Get category color for tinting glyph buttons */
      float category_color[3];
      RNA_float_get_array(op->ptr, "color", category_color);

      /* Convert float RGB (0.0-1.0) to uchar RGB (0-255) for button_color_set */
      uchar category_color_uchar[4];
      category_color_uchar[0] = uchar(category_color[0] * 255.0f);
      category_color_uchar[1] = uchar(category_color[1] * 255.0f);
      category_color_uchar[2] = uchar(category_color[2] * 255.0f);
      category_color_uchar[3] = 255; /* Alpha */

      if (search_results.is_empty()) {
        /* No results found */
        results_grid.label(IFACE_("No glyphs found"), ICON_NONE);
      }
      else {
        /* Create buttons for each search result */
        for (const auto &result : search_results) {
          const std::string &glyph_unicode = result.first;
          const std::string &glyph_name = result.second;

          /* Create button with glyph */
          Block *result_block = results_grid.block();
          block_layout_set_current(result_block, &results_grid);

          Button *result_but = uiDefBut(result_block,
                                             ButtonType::But,
                                             glyph_unicode.c_str(),
                                             0,
                                             0,
                                             UI_UNIT_X * 1.5f,
                                             UI_UNIT_Y,
                                             nullptr,
                                             0,
                                             0,
                                             std::nullopt);

          /* Apply category color to the button */
          button_color_set(result_but, category_color_uchar);
          result_but->drawflag |= BUT_TEXT_USE_COL;

          /* Set tooltip with glyph name */
          result_but->tip_quick_func = [glyph_name](const Button *) { return glyph_name; };

          /* Add callback to set the glyph when clicked */
          char *glyph_unicode_copy = MEM_new_array<char>(glyph_unicode.length() + 1, __func__);
          strcpy(glyph_unicode_copy, glyph_unicode.c_str());
          button_func_set(result_but, glyph_search_result_button_cb, op, glyph_unicode_copy);
        }
      }
    }

    /* Right side: Glyph button, Code with Paste button - aligned right */
    Layout &col_glyph_code = split_search_glyph.column(false);
    col_glyph_code.alignment_set(LayoutAlign::Right);
    Layout &row_code = col_glyph_code.row(true);
    row_code.fixed_size_set(true);

    /* Glyph button f02f - before Code */
    char glyph_btn[8] = "";
    process_glyph_input("f02f", glyph_btn, sizeof(glyph_btn));
    Block *search_block = row_code.block();
    block_layout_set_current(search_block, &row_code);
    Button *glyph_but = uiDefBut(search_block,
                                      ButtonType::But,
                                      glyph_btn,
                                      0,
                                      0,
                                      UI_UNIT_X * 1.5f,
                                      UI_UNIT_Y,
                                      nullptr,
                                      0,
                                      0,
                                      std::nullopt);
    /* Set tooltip for glyph button */
    glyph_but->tip_quick_func = [](const Button *) { return "More glyphs"; };
    
    /* Add callback to open Grid View when clicked */
    button_func_set(glyph_but, glyph_more_glyphs_button_cb, op, nullptr);
    (void)glyph_but;

    /* Code field with Paste button */
    Layout &row_glyph = row_code.row(true);
    row_glyph.prop(op->ptr, "glyph", UI_ITEM_NONE, IFACE_("Code"), ICON_NONE);
    Layout &row_glyph_btn = row_glyph.row(true);
    /* Paste button - allows pasting hex code from clipboard (Ctrl+V) */
    PointerRNA paste_ptr = row_glyph_btn.op("SCREEN_OT_category_tab_paste_glyph", "", ICON_PASTEDOWN);
    RNA_string_set(&paste_ptr, "category", category);

    /* Validate glyph input and show warning if invalid */
    char glyph_raw_check[16] = "";
    RNA_string_get(op->ptr, "glyph", glyph_raw_check);

    bool glyph_valid = true;
    if (glyph_raw_check[0] != '\0') {
      /* Check total string length */
      size_t total_len = strlen(glyph_raw_check);
      if (total_len > 8) {
        glyph_valid = false;
      }
      else {
        const char *hex_start = glyph_raw_check;

        /* Skip optional "0x" prefix */
        if (glyph_raw_check[0] == '0' &&
            (glyph_raw_check[1] == 'x' || glyph_raw_check[1] == 'X'))
        {
          hex_start = glyph_raw_check + 2;
        }

        size_t hex_len = strlen(hex_start);

        if (hex_len == 0 || hex_len > 6) {
          glyph_valid = false;
        }
        else {
          for (size_t i = 0; i < hex_len; i++) {
            if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
              glyph_valid = false;
              break;
            }
          }
        }

        if (glyph_valid) {
          uint val = strtoul(hex_start, nullptr, 16);
          if (val < 32 || val > 0x10FFFF) {
            glyph_valid = false;
          }
        }
      }
    }

    /* Show warning if glyph is invalid */
    if (!glyph_valid && glyph_raw_check[0] != '\0') {
      col_glyph.label("Invalid hex code (use 1-6 hex digits, e.g., e5d2)", ICON_ERROR);
    }

    icon_panel.body->separator();

    /* Glyph Preview - show current glyph centered with custom color */
    char glyph_raw[16] = "";
    RNA_string_get(op->ptr, "glyph", glyph_raw);

    /* Process glyph input: convert hex code (e.g., "e5d2") to UTF-8 character.
     * Only process if input is valid hex. */
    char glyph[8] = "";
    if (glyph_valid && glyph_raw[0] != '\0') {
      process_glyph_input(glyph_raw, glyph, sizeof(glyph));
    }

    /* Get custom color */
    float color_preview[3];
    RNA_float_get_array(op->ptr, "color", color_preview);

    /* Get the proper glyph for preview.
     * Priority:
     * 1. If glyph property is set, use the processed glyph.
     * 2. If search has results, use the first found glyph.
     * 3. Otherwise, use panel_category_glyph_lookup to get the default/mapped glyph.
     */
    const char *preview_glyph = nullptr;

    if (glyph[0] != '\0') {
      /* User has set a custom glyph - use the processed glyph */
      preview_glyph = glyph;
    }
    else {
      /* Check if there's an active search with results */
      char glyph_search_query[64] = "";
      RNA_string_get(op->ptr, "glyph_search", glyph_search_query);

      if (glyph_search_query[0] != '\0') {
        /* Search is active - get first result for preview */
        auto search_results = glyph_search_call_python(C, glyph_search_query, category, 1);
        if (!search_results.is_empty()) {
          /* Use first search result for preview */
          const std::string &first_glyph_unicode = search_results[0].first;
          if (!first_glyph_unicode.empty() && first_glyph_unicode.length() < sizeof(category_tab_preview_glyph)) {
            strcpy(category_tab_preview_glyph, first_glyph_unicode.c_str());
            preview_glyph = category_tab_preview_glyph;
            printf("[GLYPH PREVIEW] Using search result: %s\n", first_glyph_unicode.c_str());
          }
        }
      }

      /* If no glyph from search, lookup the default glyph for this category */
      if (preview_glyph == nullptr) {
        bool is_fallback_letter = false;
        preview_glyph = panel_category_glyph_lookup(wm, category, nullptr, &is_fallback_letter, nullptr);

        /* If lookup returns nullptr (glyph was explicitly cleared), use fallback letter */
        if (preview_glyph == nullptr && is_fallback_letter) {
          const int first_char_size = BLI_str_utf8_size_safe(category);
          if (first_char_size > 0 && first_char_size < sizeof(category_tab_preview_glyph)) {
            memcpy(category_tab_preview_glyph, category, first_char_size);
            category_tab_preview_glyph[first_char_size] = '\0';
            preview_glyph = category_tab_preview_glyph;  /* Mark as handled */
          }
        }
      }
    }

    /* Initialize preview buffers (will be updated by live update callback) */
    if (preview_glyph) {
      STRNCPY(category_tab_preview_glyph, preview_glyph);
    }
    else {
      category_tab_preview_glyph[0] = '\0';
    }
    copy_v3_v3(category_tab_preview_color, color_preview);

    /* Create centered row for preview */
    Layout &preview_row = icon_panel.body->row(false);
    preview_row.alignment_set(LayoutAlign::Center);

    /* Get block and create preview button */
    Block *preview_block = preview_row.block();
    const int preview_size = int(style->widget.points * UI_SCALE_FAC * 3.0f);

    /* Create custom button with draw callback */
    Button *preview_but = uiDefBut(preview_block,
                                       ButtonType::Extra,
                                       "",
                                       0,
                                       0,
                                       preview_size,
                                       preview_size,
                                       nullptr,
                                       0.0f,
                                       0.0f,
                                       std::nullopt);

    /* Set draw callback to render glyph with color */
    button_func_drawextra_set(preview_block, [style](const bContext * /*C*/, rcti *rect) {
      /* Get font - 2x the tab size for preview */
      const int fontid = BLF_default();
      const float font_size = style->widget.points * UI_SCALE_FAC * 2.0f;
      BLF_size(fontid, font_size);

      /* Set custom color from file-scope static buffer (updated by live update callback) */
      BLF_color3fv_alpha(fontid, category_tab_preview_color, 1.0f);

      /* Calculate center position */
      const float glyph_width = BLF_width(fontid, category_tab_preview_glyph, BLF_DRAW_STR_DUMMY_MAX);
      const float glyph_height = BLF_height(fontid, category_tab_preview_glyph, BLF_DRAW_STR_DUMMY_MAX);

      const float rect_width = BLI_rcti_size_x(rect);
      const float rect_height = BLI_rcti_size_y(rect);
      const float x = rect->xmin + (rect_width - glyph_width) / 2.0f;
      const float y = rect->ymin + (rect_height - glyph_height) / 2.0f;

      /* Draw glyph */
      BLF_position(fontid, x, y, 0.0f);
      BLF_draw(fontid, category_tab_preview_glyph, BLF_DRAW_STR_DUMMY_MAX);
    });

    /* Use the button to prevent unused variable warning */
    (void)preview_but;
  }

  /* Color panel */
  PanelLayout color_panel = layout.panel(C, "glyph_color", false);

  if (color_panel.header) {
    color_panel.header->label(IFACE_("Color"), ICON_NONE);
  }

  if (color_panel.body) {
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
        const ThemeCollectionColor *category_tab_color = &btheme->collection_color[preset];
        button_color_set(but, category_tab_color->color);
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

  layout.separator();

  /* Tags section in a sub-panel - only show for non-reserved categories */
  /* Get filter settings from window manager */
  const bool filter_show_all_modes = wm->category_tag_filter_show_all_modes;
  const bool filter_current_mode = wm->category_tag_filter_current_mode;
  const uint32_t current_mode_flag = get_current_tag_mode_flag(C);

  /* Get all active tags for the header (unfiltered) */
  const std::string tags_data_header = get_tags_for_category_ui(wm, category, true, false, 0);

  /* Get filtered tags for the body list */
  const std::string tags_data_body = get_tags_for_category_ui(
      wm, category, filter_show_all_modes, filter_current_mode, current_mode_flag);

  /* Don't show tags panel for reserved categories */
  if (!is_reserved) {
    PanelLayout tags_panel = layout.panel(C, "tags_list", false);

    /* Add label, active tag glyphs, and "New Tag" button to panel header */
    if (tags_panel.header) {
      tags_panel.header->label(IFACE_("Tags list"), ICON_NONE);

      /* Show active tags as colored glyph buttons in header */
      if (!tags_data_header.empty()) {
        Layout &glyphs_row = tags_panel.header->row(true);
        glyphs_row.alignment_set(LayoutAlign::Center);

        const char *cursor = tags_data_header.c_str();
        char tag_name[64];
        char tag_glyph[16];
        char tag_color[32];
        int is_active;

        while (*cursor != '\0') {
          /* Parse tag name */
          int i = 0;
          while (*cursor != '|' && *cursor != '\0' && i < 63) {
            tag_name[i++] = *cursor++;
          }
          tag_name[i] = '\0';
          if (*cursor == '|') {
            cursor++;
          }

          /* Parse glyph */
          i = 0;
          while (*cursor != '|' && *cursor != '\0' && i < 15) {
            tag_glyph[i++] = *cursor++;
          }
          tag_glyph[i] = '\0';
          if (*cursor == '|') {
            cursor++;
          }

          /* Parse is_active */
          is_active = 0;
          while (*cursor >= '0' && *cursor <= '9') {
            is_active = is_active * 10 + (*cursor - '0');
            cursor++;
          }

          /* Parse color */
          if (*cursor == '|') {
            cursor++;
            i = 0;
            while (*cursor != ';' && *cursor != '\0' && i < 31) {
              tag_color[i++] = *cursor++;
            }
            tag_color[i] = '\0';
          }
          else {
            tag_color[0] = '\0';
          }

          /* Only show colored glyph for active tags */
          if (tag_name[0] != '\0' && is_active && tag_glyph[0] != '\0') {
            float color_rgb[3] = {0.0f, 0.0f, 0.0f};
            bool has_color = false;
            if (tag_color[0] != '\0') {
              if (sscanf(tag_color, "%f,%f,%f", &color_rgb[0], &color_rgb[1], &color_rgb[2]) == 3)
              {
                if (color_rgb[0] > 0.001f || color_rgb[1] > 0.001f || color_rgb[2] > 0.001f) {
                  has_color = true;
                }
              }
            }

            /* Create colored glyph label */
            Block *block = glyphs_row.block();
            block_layout_set_current(block, &glyphs_row);
            Button *glyph_but = uiDefBut(block,
                                              ButtonType::Label,
                                              tag_glyph,
                                              0,
                                              0,
                                              UI_UNIT_X,
                                              UI_UNIT_Y,
                                              nullptr,
                                              0,
                                              0,
                                              std::nullopt);

            /* Set tooltip with tag name (copy string to avoid dangling pointer) */
            std::string tag_name_copy = tag_name;
            glyph_but->tip_quick_func = [tag_name_copy](const Button *) { return tag_name_copy; };

            /* Set color if available */
            if (has_color) {
              uchar color_uchar[4];
              color_uchar[0] = uchar(color_rgb[0] * 255.0f);
              color_uchar[1] = uchar(color_rgb[1] * 255.0f);
              color_uchar[2] = uchar(color_rgb[2] * 255.0f);
              color_uchar[3] = 255;
              button_color_set(glyph_but, color_uchar);
            }
          }

          if (*cursor == ';') {
            cursor++;
          }
        }
      }

      Layout &header_row = tags_panel.header->row(true);
      header_row.alignment_set(LayoutAlign::Right);
      header_row.scale_x_set(1.0f);
      PointerRNA new_tag_ptr = header_row.op("wm.category_tag_create", IFACE_("New tag"), ICON_ADD);
      RNA_string_set(&new_tag_ptr, "name", "");
      RNA_string_set(&new_tag_ptr, "category", category);

      header_row.separator();

      /* Filter menu - toggle visibility of tags by mode */
      header_row.menu("SCREEN_MT_category_tag_filter", "", ICON_FILTER);

      header_row.separator();

      /* Button to open Preferences in Tags section */
      PointerRNA prefs_ptr = header_row.op("SCREEN_OT_userpref_show", "", ICON_PREFERENCES);
      RNA_enum_set(&prefs_ptr, "section", USER_SECTION_TAGS);
    }

    if (tags_panel.body) {
      Layout &tags_body = *tags_panel.body;
      if (!tags_data_body.empty()) {
        /* Create centered container for the grid */
        Layout &centered_row = tags_body.row(false);
        centered_row.alignment_set(LayoutAlign::Center);

        /* Use grid_flow for automatic column wrapping (max 3 columns, row-major) */
        Layout &tags_grid = centered_row.grid_flow(true, 3, true, false, false);

        const char *cursor = tags_data_body.c_str();
        char tag_name[64];
        char tag_glyph[16];
        char tag_color[32];
        int is_active;

        while (*cursor != '\0') {
          int i = 0;
          while (*cursor != '|' && *cursor != '\0' && i < 63) {
            tag_name[i++] = *cursor++;
          }
          tag_name[i] = '\0';

          if (*cursor == '|') {
            cursor++;
          }

          i = 0;
          while (*cursor != '|' && *cursor != '\0' && i < 15) {
            tag_glyph[i++] = *cursor++;
          }
          tag_glyph[i] = '\0';

          if (*cursor == '|') {
            cursor++;
          }

          is_active = 0;
          while (*cursor >= '0' && *cursor <= '9') {
            is_active = is_active * 10 + (*cursor - '0');
            cursor++;
          }

          /* Parse color (format: r,g,b where r,g,b are 0.0-1.0) */
          if (*cursor == '|') {
            cursor++;
            i = 0;
            while (*cursor != ';' && *cursor != '\0' && i < 31) {
              tag_color[i++] = *cursor++;
            }
            tag_color[i] = '\0';
          }
          else {
            tag_color[0] = '\0';
          }

          if (tag_name[0] != '\0') {
            /* Create row layout for the tag button */
            Layout &tag_item = tags_grid.row(true);
            tag_item.alignment_set(LayoutAlign::Left);

            Block *block = tag_item.block();
            block_layout_set_current(block, &tag_item);

            /* Parse color for the glyph */
            float color_rgb[3] = {0.0f, 0.0f, 0.0f};
            bool has_custom_color = false;
            if (tag_color[0] != '\0') {
              if (sscanf(tag_color, "%f,%f,%f", &color_rgb[0], &color_rgb[1], &color_rgb[2]) == 3)
              {
                if (color_rgb[0] > 0.001f || color_rgb[1] > 0.001f || color_rgb[2] > 0.001f) {
                  has_custom_color = true;
                }
              }
            }

            /* Create unified Tag button with box container effect */
            Button *tag_but = uiDefButTag(block,
                                              IFACE_(tag_name),
                                              tag_glyph,
                                              has_custom_color ? color_rgb : nullptr,
                                              is_active,
                                              false,  /* is_pref_mode - toggle button with checkbox */
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
                RNA_string_set(op_ptr, "tag_name", tag_name);
              }
            }
          }

          if (*cursor == ';') {
            cursor++;
          }
        }
      }
      else {
        tags_body.label(IFACE_("No tags. Click 'New' to create."), ICON_INFO);
      }
    }
  } /* End of !is_reserved check for tags panel */

  layout.separator();

  /* Buttons row: Reset | Cancel | Save */
  Layout &split = layout.split(0.4f, false);
  Layout &row_left = split.row(true);

  /* Reset button (left aligned) */
  PointerRNA reset_ptr = row_left.op("SCREEN_OT_category_tab_reset", IFACE_("Reset"), ICON_LOOP_BACK);
  RNA_string_set(&reset_ptr, "category", category);

  /* Spacer and right-aligned buttons */
  Layout &row_right = split.row(true);
  row_right.separator_spacer();
  row_right.op("SCREEN_OT_category_tab_edit_dialog_cancel", IFACE_("Cancel"), ICON_NONE);
  row_right.op("SCREEN_OT_category_tab_edit_dialog_save", IFACE_("Save"), ICON_CHECKMARK);

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
  if (U.category_tabs_allow_edit) {
    return false;
  }
  return ED_operator_regionactive(C);
}

wmOperatorStatus category_tab_edit_dialog_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  /* Get category from mouse position */
  ARegion *region = CTX_wm_region(C);
  if (!region || !panel_category_tabs_is_visible(region)) {
    return OPERATOR_CANCELLED;
  }

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

  /* Store category name in operator properties */
  RNA_string_set(op->ptr, "category", category);

  /* Check for existing override and populate properties */
  wmWindowManager *wm = CTX_wm_manager(C);
  bool has_override = false;

  /* First check category_glyph_overrides (user changes in current session) */
  for (CategoryGlyphItem *item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category)) {
      /* Extract any leading glyph from display_name */
      char extracted_glyph[16];
      char clean_display_name[32];
      extract_leading_glyph(
          item->display_name, extracted_glyph, sizeof(extracted_glyph), clean_display_name, sizeof(clean_display_name));

      /* If display_name is empty after glyph extraction, find panel label */
      if (clean_display_name[0] == '\0') {
        const char *panel_label = find_panel_label_for_category(region, category);
        if (panel_label) {
          STRNCPY(clean_display_name, panel_label);
        }
      }

      RNA_string_set(op->ptr, "display_name", clean_display_name);

      /* Use extracted glyph if item->glyph is empty, otherwise use item->glyph */
      char hex_code[16];
      if (item->glyph[0] != '\0') {
        utf8_to_hex_codepoint(item->glyph, hex_code, sizeof(hex_code));
      }
      else if (extracted_glyph[0] != '\0') {
        STRNCPY(hex_code, extracted_glyph);
      }
      else {
        hex_code[0] = '\0';
      }
      RNA_string_set(op->ptr, "glyph", hex_code);
      RNA_float_set_array(op->ptr, "color", item->color);
      has_override = true;
      break;
    }
  }

  /* If no override, check category_glyph_mappings (saved settings from JSON) */
  if (!has_override) {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        /* Load display_name from mappings */
        if (item->display_name[0] != '\0') {
          RNA_string_set(op->ptr, "display_name", item->display_name);
        }

        /* Load glyph */
        if (item->glyph[0] != '\0') {
          char hex_code[16];
          utf8_to_hex_codepoint(item->glyph, hex_code, sizeof(hex_code));
          RNA_string_set(op->ptr, "glyph", hex_code);
        }

        /* Load color if not default black */
        if (!is_zero_v3(item->color)) {
          RNA_float_set_array(op->ptr, "color", item->color);
        }
        break;
      }
    }
  }

  /* If display_name is still empty, use panel label or category */
  char current_display_name[32] = "";
  RNA_string_get(op->ptr, "display_name", current_display_name);
  if (current_display_name[0] == '\0') {
    /* Set display_name: if category is a single glyph, find panel label */
    if (is_single_glyph_str(category)) {
      const char *panel_label = find_panel_label_for_category(region, category);
      if (panel_label) {
        RNA_string_set(op->ptr, "display_name", panel_label);
      }
    }
    else {
      RNA_string_set(op->ptr, "display_name", category);
    }
  }

  /* If glyph is still empty, use panel_category_glyph_lookup */
  char current_glyph[16] = "";
  RNA_string_get(op->ptr, "glyph", current_glyph);
  if (current_glyph[0] == '\0') {
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    bool is_fallback = false;

    const char *default_glyph = panel_category_glyph_lookup(wm, category, nullptr, &is_fallback, glyph_color);

    if (default_glyph) {
      char hex_code[16];
      utf8_to_hex_codepoint(default_glyph, hex_code, sizeof(hex_code));
      RNA_string_set(op->ptr, "glyph", hex_code);
    }
  }

  /* If color is still default, check from lookup */
  float current_color[3] = {0.0f, 0.0f, 0.0f};
  RNA_float_get_array(op->ptr, "color", current_color);
  if (is_zero_v3(current_color)) {
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    bool is_fallback = false;
    panel_category_glyph_lookup(wm, category, nullptr, &is_fallback, glyph_color);
    if (!is_zero_v3(glyph_color)) {
      RNA_float_set_array(op->ptr, "color", glyph_color);
    }
  }

  /* Save original values for cancel functionality */
  RNA_string_get(op->ptr, "display_name", current_display_name);
  RNA_string_get(op->ptr, "glyph", current_glyph);
  RNA_float_get_array(op->ptr, "color", current_color);
  RNA_string_set(op->ptr, "original_display_name", current_display_name);
  RNA_string_set(op->ptr, "original_glyph", current_glyph);
  RNA_float_set_array(op->ptr, "original_color", current_color);
  RNA_boolean_set(op->ptr, "original_has_override", has_override);

  /* Save original tags for cancel functionality - read from WM mappings/overrides */
  char original_tags[256] = "";
  const char *tags_str = nullptr;

  /* First check overrides */
  for (CategoryGlyphItem *item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category)) {
      tags_str = item->tags;
      break;
    }
  }

  /* If no override, check mappings */
  if (!tags_str || tags_str[0] == '\0') {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        tags_str = item->tags;
        break;
      }
    }
  }

  if (tags_str) {
    STRNCPY(original_tags, tags_str);
  }
  RNA_string_set(op->ptr, "original_tags", original_tags);

  /* Store pointer to dialog operator for Reset/Save button access */
  category_tab_current_dialog_op = op;

  /* Open custom popup with live preview support using public API */
  popup_block_ex(C,
                     category_tab_edit_block_create,
                     category_tab_edit_popup_ok_cb,
                     category_tab_edit_popup_cancel_cb,
                     op,
                     op);

  return OPERATOR_RUNNING_MODAL;
}

wmOperatorStatus category_tab_edit_dialog_exec(bContext *C, wmOperator *op)
{
  char category[64];
  RNA_string_get(op->ptr, "category", category);

  char display_name[32];
  RNA_string_get(op->ptr, "display_name", display_name);

  char glyph_raw[16];
  RNA_string_get(op->ptr, "glyph", glyph_raw);

  /* Validate glyph input: must be empty or valid hex code (1-6 hex digits) */
  if (glyph_raw[0] != '\0') {
    /* Check total string length (including optional 0x prefix) */
    size_t total_len = strlen(glyph_raw);
    if (total_len > 8) { /* Max: "0x" + 6 hex digits */
      BKE_report(op->reports, RPT_ERROR, "Glyph Code is too long (max 6 hex digits)");
      return OPERATOR_CANCELLED;
    }

    const char *hex_start = glyph_raw;

    /* Skip optional "0x" or "0X" prefix */
    if (glyph_raw[0] == '0' && (glyph_raw[1] == 'x' || glyph_raw[1] == 'X')) {
      hex_start = glyph_raw + 2;
    }

    /* Check if remaining string is a valid hex number (1-6 hex digits for Unicode) */
    size_t hex_len = strlen(hex_start);
    bool valid = true;

    if (hex_len == 0 || hex_len > 6) {
      valid = false;
    }
    else {
      /* Verify all characters are hex digits */
      for (size_t i = 0; i < hex_len; i++) {
        if (!isxdigit(static_cast<unsigned char>(hex_start[i]))) {
          valid = false;
          break;
        }
      }
    }

    if (!valid) {
      BKE_report(op->reports, RPT_ERROR, "Glyph Code must be 1-6 hex digits (e.g., e5d2)");
      return OPERATOR_CANCELLED;
    }

    /* Validate Unicode codepoint range */
    uint val = strtoul(hex_start, nullptr, 16);
    if (val < 32 || val > 0x10FFFF) {
      BKE_report(op->reports, RPT_ERROR, "Glyph Code must be a valid Unicode codepoint (0020-10FFFF)");
      return OPERATOR_CANCELLED;
    }
  }

  /* Process glyph input: convert hex code (e.g., "e5d2") to UTF-8 character */
  char glyph[8];
  glyph[0] = '\0';
  process_glyph_input(glyph_raw, glyph, sizeof(glyph));

  float color[3];
  RNA_float_get_array(op->ptr, "color", color);

  /* Get or create override */
  wmWindowManager *wm = CTX_wm_manager(C);
  CategoryGlyphItem *item = nullptr;

  for (CategoryGlyphItem *it =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       it;
       it = static_cast<CategoryGlyphItem *>(it->next))
  {
    if (STREQ(it->category, category)) {
      item = it;
      break;
    }
  }

  if (!item) {
    item = MEM_new<CategoryGlyphItem>(__func__);
    STRNCPY(item->category, category);
    BLI_addtail(&wm->category_glyph_overrides, item);
  }

  /* Look up what the default glyph would be for this category.
   * This tells us if the current glyph is a fallback letter.
   */
  bool is_fallback = false;
  const char *default_glyph = panel_category_glyph_lookup(wm, category, nullptr, &is_fallback, nullptr);

  /* Update values */
  STRNCPY(item->display_name, display_name);
  copy_v3_v3(item->color, color);

  /* Only save glyph to override if user has changed it from the default.
   * If glyph matches the default (especially fallback letters), leave it empty
   * so that the lookup function will return the correct is_fallback status.
   */
  if (glyph[0] != '\0' && default_glyph && !STREQ(glyph, default_glyph)) {
    /* User has set a custom glyph different from default */
    STRNCPY(item->glyph, glyph);
  }
  else {
    /* Glyph is same as default or empty - clear override glyph to preserve
     * fallback letter detection in the drawing code. */
    item->glyph[0] = '\0';
  }

  /* Clear dialog operator pointer */
  category_tab_current_dialog_op = nullptr;

  /* Redraw */
  WM_main_add_notifier(NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace blender::ui
