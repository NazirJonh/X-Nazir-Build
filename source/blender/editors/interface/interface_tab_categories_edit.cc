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

#include <cctype>
#include <cstdlib>
#include <cstring>

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
#include "RNA_prototypes.hh"

#include "BLF_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "interface_intern.hh"
#include "regions/interface_regions_intern.hh"

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
ui::Block *category_tab_popup_block = nullptr;

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
  ui::Layout &layout = *menu->layout;
  wmWindowManager *wm = CTX_wm_manager(C);

  layout.label(IFACE_("Filter Tags"), ICON_NONE);
  layout.separator();

  PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
  layout.prop(
      &wm_ptr, "category_tag_filter_show_all_modes", UI_ITEM_NONE, IFACE_("All Modes"), ICON_NONE);
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
/** \name Popup Block Creation
 * \{ */

ui::Block *category_tab_edit_block_create(bContext *C, ARegion *region, void *user_data)
{
  wmOperator *op = static_cast<wmOperator *>(user_data);
  const uiStyle *style = ui::style_get_dpi();

  /* Calculate dialog width - increased for better visibility and tag grid */
  const int dialog_width = 450 * UI_SCALE_FAC;

  ui::Block *block = block_begin(C, region, __func__, ui::EmbossType::Emboss);
  block_flag_disable(block, ui::BLOCK_LOOP);
  block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);
  popup_dummy_panel_set(region, block, op->idname);

  /* Keep popup open while editing - important for live preview */
  block_flag_enable(block, ui::BLOCK_KEEP_OPEN | ui::BLOCK_NUMSELECT);

  /* Store block pointer for Save button to close popup */
  category_tab_popup_block = block;

  /* Set up live update callback - this is the key for instant preview */
  block_func_handle_set(block, category_tab_edit_live_update_cb, op);

  /* Create layout */
  ui::Layout &layout = ui::block_layout(block,
                                         ui::LayoutDirection::Vertical,
                                         ui::LayoutType::Panel,
                                         0,
                                         0,
                                         dialog_width,
                                         0,
                                         0,
                                         style);

  /* Title */
  uiItemL_ex(&layout, IFACE_("Edit Category Tab"), ICON_NONE, true, false);
  layout.separator(0.2f, ui::LayoutSeparatorType::Line);
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
  ui::Layout &col_props = layout.column(false);
  col_props.use_property_split_set(true);

  /* Category Name - check if reserved for read-only */
  if (is_reserved) {
    /* Reserved categories: show field as read-only (disabled) */
    ui::Layout &row_name = col_props.row(false);
    row_name.enabled_set(false);
    row_name.prop(op->ptr, "display_name", UI_ITEM_NONE, IFACE_("Category Name"), ICON_NONE);
  }
  else {
    col_props.prop(op->ptr, "display_name", UI_ITEM_NONE, IFACE_("Category Name"), ICON_NONE);
  }

  layout.separator();

  /* Change Icon panel */
  ui::PanelLayout icon_panel = layout.panel(C, "change_icon", false);

  if (icon_panel.header) {
    icon_panel.header->label(IFACE_("Change the display"), ICON_NONE);
  }

  if (icon_panel.body) {
    /* Properties for glyph search and code */
    ui::Layout &col_glyph = icon_panel.body->column(false);

    /* Row with Search Glyph on left and Code with Paste on right */
    ui::Layout &row_search_glyph = col_glyph.row(false);
    ui::Layout &split_search_glyph = row_search_glyph.split(0.7f, false);

    /* Left side: Search Glyph field - right aligned */
    ui::Layout &col_search = split_search_glyph.column(false);

    /* Search field - use split to control width, align right side */
    ui::Layout &split_search = col_search.split(0.89f, false);
    ui::Layout &search_row = split_search.row(false);
    search_row.alignment_set(ui::LayoutAlign::Right);
    search_row.prop(op->ptr, "glyph_search", UI_ITEM_NONE, IFACE_("Glyph"), ICON_VIEWZOOM);

    /* Right side: Glyph button, Code with Paste button - aligned right */
    ui::Layout &col_glyph_code = split_search_glyph.column(false);
    col_glyph_code.alignment_set(ui::LayoutAlign::Right);
    ui::Layout &row_code = col_glyph_code.row(true);
    row_code.fixed_size_set(true);

    /* Glyph button f02f - before Code */
    char glyph_btn[8] = "";
    process_glyph_input("f02f", glyph_btn, sizeof(glyph_btn));
    ui::Block *search_block = row_code.block();
    ui::block_layout_set_current(search_block, &row_code);
    ui::Button *glyph_but = uiDefBut(search_block,
                                      ui::ButtonType::But,
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
    glyph_but->tip_quick_func = [](const ui::Button *) { return "More glyphs"; };
    (void)glyph_but;

    /* Code field with Paste button */
    ui::Layout &row_glyph = row_code.row(true);
    row_glyph.prop(op->ptr, "glyph", UI_ITEM_NONE, IFACE_("Code"), ICON_NONE);
    ui::Layout &row_glyph_btn = row_glyph.row(true);
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
     * If glyph property is set, use the processed glyph.
     * Otherwise, use panel_category_glyph_lookup to get the default/mapped glyph.
     */
    const char *preview_glyph = nullptr;

    if (glyph[0] != '\0') {
      /* User has set a custom glyph - use the processed glyph */
      preview_glyph = glyph;
    }
    else {
      /* No custom glyph - lookup the default glyph for this category */
      preview_glyph = panel_category_glyph_lookup(wm, category, nullptr, nullptr, nullptr);
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
    ui::Layout &preview_row = icon_panel.body->row(false);
    preview_row.alignment_set(ui::LayoutAlign::Center);

    /* Get block and create preview button */
    ui::Block *preview_block = preview_row.block();
    const int preview_size = int(style->widget.points * UI_SCALE_FAC * 3.0f);

    /* Create custom button with draw callback */
    ui::Button *preview_but = uiDefBut(preview_block,
                                       ui::ButtonType::Extra,
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
  ui::PanelLayout color_panel = layout.panel(C, "glyph_color", false);

  if (color_panel.header) {
    color_panel.header->label(IFACE_("Color"), ICON_NONE);
  }

  if (color_panel.body) {
    ui::Layout &col_color = color_panel.body->column(false);

    /* Centered row for preset buttons and custom color picker */
    ui::Layout &presets_row = col_color.row(true);
    presets_row.alignment_set(ui::LayoutAlign::Center);
    presets_row.emboss_set(ui::EmbossType::Pulldown);

    /* Get block for creating buttons */
    ui::Block *block = presets_row.block();
    ui::block_layout_set_current(block, &presets_row);

    /* Get operator type */
    wmOperatorType *ot = WM_operatortype_find("SCREEN_OT_category_tab_color_preset", false);
    bTheme *btheme = theme::theme_get();

    /* Create button for each color preset (9 buttons: NONE + 8 colors) */
    for (int i = 0; i < 9; i++) {
      const int preset = i - 1;  /* -1 to 7 */

      ui::Button *but = uiDefButO_ptr(block,
                                      ui::ButtonType::But,
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
    ui::Layout &picker_col = presets_row.column(false);
    picker_col.ui_units_x_set(1.0f);
    picker_col.prop(op->ptr, "color", ui::ITEM_R_ICON_ONLY, "", ICON_NONE);
  }

  layout.separator();

  /* Tags section in a sub-panel - only show for non-reserved categories */
  /* Get filter settings from window manager */
  const bool filter_show_all_modes = wm->category_tag_filter_show_all_modes;
  const bool filter_current_mode = wm->category_tag_filter_current_mode;
  const uint32_t current_mode_flag = get_current_tag_mode_flag(C);

  const std::string tags_data = get_tags_for_category_ui(
      wm, category, filter_show_all_modes, filter_current_mode, current_mode_flag);

  /* Don't show tags panel for reserved categories */
  if (!is_reserved) {
    ui::PanelLayout tags_panel = layout.panel(C, "tags_list", false);

    /* Add label, active tag glyphs, and "New Tag" button to panel header */
    if (tags_panel.header) {
      tags_panel.header->label(IFACE_("Tags list"), ICON_NONE);

      /* Show active tags as colored glyph buttons in header */
      if (!tags_data.empty()) {
        ui::Layout &glyphs_row = tags_panel.header->row(true);
        glyphs_row.alignment_set(ui::LayoutAlign::Center);

        const char *cursor = tags_data.c_str();
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
            ui::Block *block = glyphs_row.block();
            ui::block_layout_set_current(block, &glyphs_row);
            ui::Button *glyph_but = uiDefBut(block,
                                              ui::ButtonType::Label,
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
            glyph_but->tip_quick_func = [tag_name_copy](const ui::Button *) { return tag_name_copy; };

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

      ui::Layout &header_row = tags_panel.header->row(true);
      header_row.alignment_set(ui::LayoutAlign::Right);
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
      ui::Layout &tags_body = *tags_panel.body;
      if (!tags_data.empty()) {
        /* Create centered container for the grid */
        ui::Layout &centered_row = tags_body.row(false);
        centered_row.alignment_set(ui::LayoutAlign::Center);

        /* Use grid_flow for automatic column wrapping (max 3 columns, row-major) */
        ui::Layout &tags_grid = centered_row.grid_flow(true, 3, true, false, false);

        const char *cursor = tags_data.c_str();
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
            /* Create a box container for this tag to visually unify elements */
            ui::Layout &tag_box = tags_grid.box();
            ui::Layout &tag_item = tag_box.row(true);
            tag_item.alignment_set(ui::LayoutAlign::Left);

            /* Parse color for the glyph label */
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

            ui::Block *block = tag_item.block();
            ui::block_layout_set_current(block, &tag_item);
            wmOperatorType *ot = WM_operatortype_find("wm.category_tag_toggle", false);

            /* 1. Small checkbox toggle (no text) */
            ui::Button *toggle_but = uiDefButO_ptr(block,
                                                    ui::ButtonType::Checkbox,
                                                    ot,
                                                    wm::OpCallContext::ExecDefault,
                                                    "",
                                                    0,
                                                    0,
                                                    UI_UNIT_X,
                                                    UI_UNIT_Y,
                                                    std::nullopt);

            /* Set operator properties */
            PointerRNA *op_ptr = button_operator_ptr_ensure(toggle_but);
            RNA_string_set(op_ptr, "category", category);
            RNA_string_set(op_ptr, "tag_name", tag_name);

            /* Set active state for toggle */
            if (is_active) {
              toggle_but->flag |= ui::UI_SELECT_DRAW;
            }

            /* 2. Colored glyph label (if glyph exists) */
            if (tag_glyph[0] != '\0') {
              ui::Button *glyph_but = uiDefBut(block,
                                               ui::ButtonType::Label,
                                               tag_glyph,
                                               0,
                                               0,
                                               UI_UNIT_X,
                                               UI_UNIT_Y,
                                               nullptr,
                                               0,
                                               0,
                                               "");

              if (has_custom_color) {
                uchar color_uchar[4];
                color_uchar[0] = uchar(color_rgb[0] * 255.0f);
                color_uchar[1] = uchar(color_rgb[1] * 255.0f);
                color_uchar[2] = uchar(color_rgb[2] * 255.0f);
                color_uchar[3] = 255;
                button_color_set(glyph_but, color_uchar);
              }
            }

            /* 3. Tag name label */
            uiDefBut(block,
                     ui::ButtonType::Label,
                     IFACE_(tag_name),
                     0,
                     0,
                     UI_UNIT_X * 4,
                     UI_UNIT_Y,
                     nullptr,
                     0,
                     0,
                     "");
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
  ui::Layout &split = layout.split(0.4f, false);
  ui::Layout &row_left = split.row(true);

  /* Reset button (left aligned) */
  PointerRNA reset_ptr = row_left.op("SCREEN_OT_category_tab_reset", IFACE_("Reset"), ICON_LOOP_BACK);
  RNA_string_set(&reset_ptr, "category", category);

  /* Spacer and right-aligned buttons */
  ui::Layout &row_right = split.row(true);
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
  ui::popup_block_ex(C,
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
