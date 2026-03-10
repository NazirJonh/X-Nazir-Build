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

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

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
#include "regions/interface_regions_intern.hh"

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

/* Static buffers for preview callback - updated by live update callback */
static char category_tab_preview_glyph[8] = "";
static float category_tab_preview_color[3] = {0.0f, 0.0f, 0.0f};

/* Static pointer to preview button - updated when popup opens, used for live updates */
static Button *category_tab_preview_button = nullptr;

/* Static pointer to current dialog operator - needed for Reset/Save buttons */
wmOperator *category_tab_current_dialog_op = nullptr;

/* Static pointer to popup block - needed for Save button to close popup */
Block *category_tab_popup_block = nullptr;

/* Track last closed popup time and category to prevent immediate reopen */
double category_tab_popup_close_time = 0.0;
char category_tab_last_closed_category[64] = "";

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

static bool validate_glyph_hex_input(const char *glyph_raw)
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

  /* Get original values saved when dialog was opened */
  char original_display_name[32] = "";
  char original_glyph_hex[16] = "";
  float original_color[3] = {0.0f, 0.0f, 0.0f};
  char original_tags[256] = "";
  char original_icon_key[128] = "";
  char original_icon_path[1024] = "";
  char original_icon_provider[128] = "";
  int original_icon_source = 0;
  int original_glyph_mode = 0;
  bool original_has_override = false;

  RNA_string_get(op->ptr, "original_display_name", original_display_name);
  RNA_string_get(op->ptr, "original_glyph", original_glyph_hex);
  RNA_float_get_array(op->ptr, "original_color", original_color);
  RNA_string_get(op->ptr, "original_tags", original_tags);
  RNA_string_get(op->ptr, "original_icon_key", original_icon_key);
  RNA_string_get(op->ptr, "original_icon_path", original_icon_path);
  RNA_string_get(op->ptr, "original_icon_provider", original_icon_provider);
  original_icon_source = RNA_enum_get(op->ptr, "original_icon_source");
  original_glyph_mode = RNA_enum_get(op->ptr, "original_glyph_mode");
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
      item->glyph[0] = '\0';
      item->display_name[0] = '\0';
      zero_v3(item->color);
      item->tags[0] = '\0';
      item->icon_key[0] = '\0';
      item->icon_path[0] = '\0';
      item->icon_provider[0] = '\0';
      item->icon_source = 0;
      item->glyph_mode = 0;
      BLI_addtail(&wm->category_glyph_overrides, item);
    }
    STRNCPY(item->display_name, original_display_name);
    STRNCPY(item->glyph, original_glyph_utf8);
    copy_v3_v3(item->color, original_color);
    STRNCPY(item->tags, original_tags);
    STRNCPY(item->icon_key, original_icon_key);
    STRNCPY(item->icon_path, original_icon_path);
    STRNCPY(item->icon_provider, original_icon_provider);
    item->icon_source = original_icon_source;
    item->glyph_mode = original_glyph_mode;
  }
  else {
    /* There was no override before - remove any created by live preview */
  if (item) {
    /* Check if override has any user changes besides tags/glyph/color/display_name.
     * Note: color is considered changed if it's not the original color.
     */
    bool color_changed = !is_zero_v3(item->color) && !compare_v3v3(item->color, original_color, 0.001f);
    bool has_changes = (item->display_name[0] != '\0' || item->glyph[0] != '\0' ||
                        color_changed ||
                        (item->tags[0] != '\0' && original_tags[0] == '\0'));

    if (!has_changes || !original_has_override) {
      BLI_remlink(&wm->category_glyph_overrides, item);
      MEM_delete(item);
    }
    else {
      /* Keep override but restore original values */
      STRNCPY(item->display_name, original_display_name);
      STRNCPY(item->glyph, original_glyph_utf8);
      copy_v3_v3(item->color, original_color);
      STRNCPY(item->tags, original_tags);
      STRNCPY(item->icon_key, original_icon_key);
      STRNCPY(item->icon_path, original_icon_path);
      STRNCPY(item->icon_provider, original_icon_provider);
      item->icon_source = original_icon_source;
      item->glyph_mode = original_glyph_mode;
    }
  }

  /* Reset operator can temporarily modify category_glyph_mappings (for example when resetting
   * a fallback-letter category to an empty glyph). If user cancels the dialog and there was no
   * original override, restore mapping glyph/color from dialog-open snapshot as well, otherwise
   * UI may keep fallback state until restart even though changes were discarded. */
  if (!original_has_override) {
    for (CategoryGlyphItem *map_item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         map_item;
         map_item = static_cast<CategoryGlyphItem *>(map_item->next))
    {
      if (!STREQ(map_item->category, category)) {
        continue;
      }
      STRNCPY(map_item->glyph, original_glyph_utf8);
      copy_v3_v3(map_item->color, original_color);
      map_item->glyph_mode = original_glyph_mode;
      break;
    }
  }
  }

  /* Clear dialog operator pointer and popup block */
  category_tab_current_dialog_op = nullptr;
  category_tab_popup_block = nullptr;
  category_tab_preview_button = nullptr;

  /* Record popup close time and category to prevent immediate reopen */
  category_tab_popup_close_time = BLI_time_now_seconds();
  STRNCPY(category_tab_last_closed_category, category);

#ifdef WITH_PYTHON
  /* Restore tags in Python _glyph_cache to revert live preview changes. */
  if (category[0] != '\0') {
    wmWindowManager *wm_ptr = CTX_wm_manager(C);
    if (wm_ptr) {
      /* Get space_type from context for space-specific tag restoration */
      ScrArea *area = CTX_wm_area(C);
      const int space_type = area ? area->spacetype : -1;

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
                   "    restore_category_tags_from_string(category, r'''%s''', space_type=%d)\n",
                   original_tags, space_type);

      BPY_run_string_exec(C, imports, restore_cmd);
    }
  }
#endif

  /* Show info message that changes were discarded */
  WM_global_report(RPT_INFO, "Category tab changes discarded");

  WM_main_add_notifier(NC_WINDOW, nullptr);
}

void category_tab_edit_popup_ok_cb(bContext * /*C*/, void *user_data, int /*retval*/)
{
  /* Clear dialog operator pointer and popup block */
  category_tab_current_dialog_op = nullptr;
  category_tab_popup_block = nullptr;
  category_tab_preview_button = nullptr;

  /* Record popup close time and category */
  if (user_data) {
    wmOperator *op = static_cast<wmOperator *>(user_data);
    char category[64];
    RNA_string_get(op->ptr, "category", category);
    category_tab_popup_close_time = BLI_time_now_seconds();
    STRNCPY(category_tab_last_closed_category, category);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tag Filter Mode Helpers
 * \{ */

/**
 * Convert current object mode to category_tag_filter_mode value.
 * Returns 1-10 for specific modes, 1 (OBJECT_MODE) as default.
 *
 * Mapping (matches RNA enum in rna_wm.cc):
 *   1 = OBJECT_MODE, 2 = EDIT_MODE, 3 = SCULPT_MODE,
 *   4 = VERTEX_PAINT, 5 = WEIGHT_PAINT, 6 = TEXTURE_PAINT,
 *   7 = UV_EDIT, 8 = POSE_MODE, 9 = GEOMETRY_NODES, 10 = SHADER_EDITOR
 */
static char get_current_object_mode_filter_value(const bContext *C)
{
  /* Check for Node Editor first */
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    if (area->spacetype == SPACE_NODE) {
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      if (snode) {
        if (STREQ(snode->tree_idname, "GeometryNodeTree")) {
          return 9; /* GEOMETRY_NODES */
        }
        if (STREQ(snode->tree_idname, "ShaderNodeTree")) {
          return 10; /* SHADER_EDITOR */
        }
      }
    }
    else if (area->spacetype == SPACE_IMAGE) {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      if (sima) {
        if (sima->mode == SI_MODE_PAINT) {
          return 6; /* TEXTURE_PAINT */
        }
        if (sima->mode == SI_MODE_UV) {
          return 7; /* UV_EDIT */
        }
      }
    }
  }

  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return 1; /* Default to Object Mode */
  }

  /* Map object mode flags to filter mode values */
  switch (ob->mode) {
    case OB_MODE_EDIT:
      return 2; /* EDIT_MODE */
    case OB_MODE_SCULPT:
      return 3; /* SCULPT_MODE */
    case OB_MODE_VERTEX_PAINT:
      return 4; /* VERTEX_PAINT */
    case OB_MODE_WEIGHT_PAINT:
      return 5; /* WEIGHT_PAINT */
    case OB_MODE_TEXTURE_PAINT:
      return 6; /* TEXTURE_PAINT */
    case OB_MODE_EDIT_GPENCIL_LEGACY:
    case OB_MODE_PAINT_GREASE_PENCIL:
      return 7; /* UV_EDIT (closest match for 2D editing modes) */
    case OB_MODE_POSE:
      return 8; /* POSE_MODE */
    default:
      return 1; /* OBJECT_MODE (default) */
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tag Filter Toggle Menu
 * \{ */

static MenuType *category_tag_filter_toggle_menu_type = nullptr;

static void category_tag_filter_toggle_menu_draw(const bContext *C, Menu *menu)
{
  Layout &layout = *menu->layout;
  wmWindowManager *wm = CTX_wm_manager(C);

  /* Get current filter state */
  bool use_current_mode = (wm->category_tag_filter_mode != 0);

  /* "Current Mode" button - activates filtering by current object mode */
  /* Use radiobutton icons to indicate active state */
  int current_mode_icon = use_current_mode ? ICON_RADIOBUT_ON : ICON_RADIOBUT_OFF;
  PointerRNA current_mode_ptr = layout.op(
      "wm.category_tag_filter_set_mode", IFACE_("Current Mode"), current_mode_icon);
  RNA_boolean_set(&current_mode_ptr, "use_current_mode", true);

  /* "All Tags" button - shows all tags regardless of mode */
  int all_tags_icon = use_current_mode ? ICON_RADIOBUT_OFF : ICON_RADIOBUT_ON;
  PointerRNA all_tags_ptr = layout.op(
      "wm.category_tag_filter_set_mode", IFACE_("All Tags"), all_tags_icon);
  RNA_boolean_set(&all_tags_ptr, "use_current_mode", false);
}

static bool category_tag_filter_toggle_menu_poll(const bContext *C, MenuType * /*mt*/)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  return wm != nullptr;
}

void category_tag_filter_toggle_menu_register()
{
  if (category_tag_filter_toggle_menu_type != nullptr) {
    return; /* Already registered */
  }

  MenuType *mt = MEM_new_zeroed<MenuType>(__func__);
  STRNCPY_UTF8(mt->idname, "SCREEN_MT_category_tag_filter_toggle");
  STRNCPY_UTF8(mt->label, N_("Filter Tags"));
  STRNCPY_UTF8(mt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  mt->description = N_("Toggle tag filter mode: Current Mode or All Tags");
  mt->poll = category_tag_filter_toggle_menu_poll;
  mt->draw = category_tag_filter_toggle_menu_draw;

  WM_menutype_add(mt);
  category_tag_filter_toggle_menu_type = mt;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Live Update Callback
 * \{ */

static void category_tab_live_update_notify_redraw(bContext *C)
{
  if (!C) {
    return;
  }

  /* Notify category glyph/icon subscribers (tag bar + category tabs). */
  WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_CATEGORY_GLYPHS, nullptr);
  WM_event_add_notifier(C, NC_SPACE | ND_DRAW, nullptr);

  /* Immediate redraw for currently active UI context (popup/region). */
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_tag_redraw(area);
  }
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }

  /* Keep legacy window notifier for broad UI refresh. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

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

  /* Validate glyph input: must be empty or valid hex code (1-6 hex digits). */
  const bool glyph_valid = validate_glyph_hex_input(glyph_raw);

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
  const int display_mode_ui = RNA_enum_get(op->ptr, "display_mode_ui");

  /* Update preview buffers for popup preview.
   * Use the processed glyph from valid input, or fall back to default lookup.
   * Invalid input shows the default glyph (not the invalid text).
   * Fallback letter is also shown in preview.
   */
  copy_v3_v3(category_tab_preview_color, color);
  if (display_mode_ui == 2) {
    char preview_name[32] = "";
    RNA_string_get(op->ptr, "display_name", preview_name);
    const char *first_letter_source = (preview_name[0] != '\0') ? preview_name : category;
    if (!category_tab_first_utf8_char_copy(
            first_letter_source, category_tab_preview_glyph, sizeof(category_tab_preview_glyph)))
    {
      category_tab_preview_glyph[0] = '\0';
    }
  }
  else if (glyph[0] != '\0') {
    /* Valid custom glyph - show it */
    STRNCPY(category_tab_preview_glyph, glyph);
  }
  else if (default_glyph) {
    /* Empty or invalid input - show default glyph (including fallback letter) */
    STRNCPY(category_tab_preview_glyph, default_glyph);
  }
  else if (is_fallback) {
    /* default_glyph is nullptr but is_fallback is true - use first char of category */
    if (!category_tab_first_utf8_char_copy(
            category, category_tab_preview_glyph, sizeof(category_tab_preview_glyph)))
    {
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
    item->icon_key[0] = '\0';
    item->icon_path[0] = '\0';
    item->icon_provider[0] = '\0';
    item->icon_source = 0;
    item->glyph_mode = 0;

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

  /* Update icon fields for live preview/state carry-over. */
  char icon_key[128] = "";
  char icon_path[1024] = "";
  char icon_provider[128] = "";
  RNA_string_get(op->ptr, "icon_key", icon_key);
  RNA_string_get(op->ptr, "icon_path", icon_path);
  RNA_string_get(op->ptr, "icon_provider", icon_provider);
  STRNCPY(item->icon_key, icon_key);
  STRNCPY(item->icon_path, icon_path);
  STRNCPY(item->icon_provider, icon_provider);

  const int custom_icon_mode_ui = RNA_enum_get(op->ptr, "custom_icon_mode_ui");

  int resolved_icon_source = RNA_enum_get(op->ptr, "icon_source");
  if (display_mode_ui == 0 || display_mode_ui == 2) {
    resolved_icon_source = 2; /* OFF */
  }
  else if (custom_icon_mode_ui == 0) {
    resolved_icon_source = 1; /* MANUAL: Blender Icon */
  }
  else {
    resolved_icon_source = 0; /* AUTO: path/provider chain (external icon) */
    /* Clear Blender icon key when switching to Custom icon mode */
    RNA_string_set(op->ptr, "icon_key", "");
    item->icon_key[0] = '\0';
  }
  RNA_enum_set(op->ptr, "icon_source", resolved_icon_source);
  item->icon_source = resolved_icon_source;
  item->glyph_mode = (display_mode_ui == 2) ? 1 : 0;

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

  /* Trigger live redraw so icon/glyph updates appear instantly without restart. */
  category_tab_live_update_notify_redraw(C);

  /* Note: Preview button uses custom draw callback that reads directly from
   * category_tab_preview_glyph and category_tab_preview_color static buffers,
   * which are already updated above. No button-specific update needed. */
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
static void glyph_search_python_expr_build(
    const char *query, const int max_results, char r_python_expr[2048])
{
  const std::string escaped_query = category_tab_escape_for_python_literal(query);
  snprintf(r_python_expr,
           2048,
           "json.dumps([{'unicode': g['unicode'], 'name': g['name']} "
           "for g in __import__('bl_ui.glyph_library.registry', fromlist=['']).search_glyphs('%s', '', "
           "%d)])",
           escaped_query.c_str(),
           max_results);
}

static void glyph_search_parse_object_array(const char *json,
                                            const int max_results,
                                            blender::Vector<std::pair<std::string, std::string>> &r_results)
{
  const char *p = json;

  while (*p && *p != '[') {
    p++;
  }
  if (*p != '[') {
    return;
  }
  p++;

  int parsed_count = 0;
  while (*p && *p != ']') {
    while (*p && *p != '{') {
      p++;
    }
    if (*p == '{') {
      p++;
    }

    std::string unicode;
    std::string name;

    while (*p && *p != '}') {
      if (strncmp(p, "\"unicode\":", 10) == 0) {
        p += 10;
        while (*p && *p != '"') {
          p++;
        }
        if (*p == '"') {
          p++;
        }
        const char *start = p;
        while (*p && *p != '"') {
          p++;
        }
        const std::string raw_unicode(start, p - start);
        unicode = category_tab_decode_json_unicode(raw_unicode.c_str());
      }
      else if (strncmp(p, "\"name\":", 7) == 0) {
        p += 7;
        while (*p && *p != '"') {
          p++;
        }
        if (*p == '"') {
          p++;
        }
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
      parsed_count++;
      r_results.append({unicode, name});
      if (r_results.size() >= max_results) {
        break;
      }
    }

    while (*p && *p != ',' && *p != ']') {
      p++;
    }
    if (*p == ',' || *p == '}') {
      p++;
    }
  }
}

blender::Vector<std::pair<std::string, std::string>> glyph_search_call_python(
    bContext *C, const char *query, const char *category, int max_results)
{
  blender::Vector<std::pair<std::string, std::string>> results;

#ifdef WITH_PYTHON
  /* Build Python expression and execute it. */
  char python_expr[2048];
  glyph_search_python_expr_build(query, max_results, python_expr);

  const char *imports[] = {"json", nullptr};

  /* Execute Python expression and capture output */
  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};

  /* Use BPY_run_string_as_string with imports array */
  bool success = BPY_run_string_as_string(C, imports, python_expr, &err_info, &result_str);
  if (!success) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
  }

  if (success && result_str) {
    /* Check if result is an error message */
    if (strncmp(result_str, "{\"error\":", 9) == 0) {
      MEM_delete(result_str);
      return results;
    }

    if (strcmp(result_str, "[]") == 0) {
      MEM_delete(result_str);
      return results;
    }

    glyph_search_parse_object_array(result_str, max_results, results);

    if (results.is_empty()) {
      blender::Vector<std::string> string_results;
      if (category_tab_parse_json_string_array_minimal(result_str, string_results)) {
        for (const std::string &value : string_results) {
          const std::string unicode = category_tab_decode_json_unicode(value.c_str());
          if (!unicode.empty()) {
            results.append({unicode, value});
            if (results.size() >= max_results) {
              break;
            }
          }
        }
      }
    }

    MEM_delete(result_str);
  }
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
    g_glyph_cache = glyph_search_call_python(C, "", "", 1000);
    g_glyph_cache_valid = true;
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

  /* Free the glyph unicode copy that was allocated with MEM_new_array */
  MEM_delete_void(static_cast<void *>(const_cast<char *>(glyph_unicode)));
}

/* -------------------------------------------------------------------- */
/** \name Glyph Grid Popup
 * \{ */

/**
 * Data structure for glyph grid popup.
 * Stores the operator, glyphs, categories and UI state.
 */
struct GlyphGridPopupData {
  wmOperator *op; /* The picker operator (WM_OT_glyph_picker_grid) */
  wmOperator *target_op; /* The target operator whose property should be updated (e.g., wm.category_tag_create) */
  IDProperty *target_op_properties;
  Vector<std::pair<std::string, std::string>> glyphs; /* (unicode, name) pairs */
  std::string current_category; /* Currently selected category filter */
  Vector<std::string> categories; /* All available categories */
  std::string search_string; /* Search filter */
  PopupBlockHandle *popup_handle;

  GlyphGridPopupData(wmOperator *op_,
                     wmOperator *target_op_,
                     IDProperty *target_op_properties_,
                     Vector<std::pair<std::string, std::string>> glyphs_,
                     std::string category_,
                     Vector<std::string> categories_)
      : op(op_),
        target_op(target_op_),
        target_op_properties(target_op_properties_),
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
    UNUSED_VARS(C);
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

  /* Sync search string from picker operator if available */
  if (popup_data->op) {
    char search_buf[256] = "";
    RNA_string_get(popup_data->op->ptr, "glyph_search", search_buf);
    popup_data->search_string = search_buf;
  }

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

  /* Add search field at top of right column - only if picker operator available */
  Layout *search_row_ptr = nullptr;
  if (popup_data->op) {
    search_row_ptr = &right_col.row(false);

    /* Use prop() to create search field, similar to Asset Shelf */
    search_row_ptr->prop(popup_data->op->ptr,
                    "glyph_search",
                    /* Force the button to be active in a semi-modal state. */
                    ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE,
                    "",
                    ICON_VIEWZOOM);
  }

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

        /* Set the glyph property of the picker operator if available */
        if (popup_data->op) {
          RNA_string_set(popup_data->op->ptr, "glyph", hex_code);
        }

        /* Get target property - from picker operator or use default "glyph" */
        char target_prop[256] = "glyph";  /* Default to "glyph" property */
        if (popup_data->op) {
          RNA_string_get(popup_data->op->ptr, "target_property", target_prop);
        }

        if (target_prop[0] != '\0') {
          const char *target_prop_path = target_prop;
          if (STRPREFIX(target_prop_path, "window_manager.")) {
            target_prop_path += strlen("window_manager.");
          }

          PointerRNA target_ptr;
          PropertyRNA *prop;
          int index;

          /* Use the directly stored target operator from popup data */
          wmOperator *target_op = popup_data->target_op;

          if (!target_op && popup_data->target_op_properties) {
            wmWindowManager *wm = CTX_wm_manager(&C);
            if (wm) {
              for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
                   op_iter;
                   op_iter = op_iter->prev)
              {
                if (op_iter && op_iter->properties == popup_data->target_op_properties) {
                  target_op = op_iter;
                  break;
                }
              }
            }
          }

          /* Fallback: if no target_op stored, try to find it through context */
          if (!target_op) {
            if (!popup_data->target_op_properties) {
              target_op = context_active_operator_get(&C);
            }
          }

          bool resolved = false;
          const bool is_active_operator_path = STRPREFIX(target_prop_path, "active_operator.");
          const char *target_prop_path_for_operator = is_active_operator_path ?
                                                          (target_prop_path + strlen("active_operator.")) :
                                                          target_prop_path;

          /* Primary path for invoke_props_dialog operators: write directly to the captured
           * operator IDProperty group. This avoids routing through unrelated active operators. */
          if (!resolved && popup_data->target_op_properties) {
            if (strchr(target_prop_path_for_operator, '.') == nullptr &&
                strchr(target_prop_path_for_operator, '[') == nullptr)
            {
              IDProperty *idprop = IDP_GetPropertyFromGroup(popup_data->target_op_properties,
                                                            target_prop_path_for_operator);
              if (!idprop) {
                idprop = IDP_NewString(hex_code, target_prop_path_for_operator);
                IDP_AddToGroup(popup_data->target_op_properties, idprop);
                resolved = true;
              }
              else if (idprop->type == IDP_STRING) {
                IDP_AssignString(idprop, hex_code);
                resolved = true;
              }
            }
          }

          if (!resolved && target_op && target_op->ptr) {
            if (RNA_path_resolve_full(
                    target_op->ptr, target_prop_path_for_operator, &target_ptr, &prop, &index))
            {
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              resolved = true;
            }
          }

          if (STRPREFIX(target_prop_path, "active_operator.") && !resolved) {
            const char *active_op_path = target_prop_path + strlen("active_operator.");
            wmOperator *active_op = context_active_operator_get(&C);
            if (active_op && active_op->ptr) {
              if (RNA_path_resolve_full(active_op->ptr, active_op_path, &target_ptr, &prop, &index))
              {
                RNA_property_string_set(&target_ptr, prop, hex_code);
                RNA_property_update(&C, &target_ptr, prop);
                resolved = true;
              }
            }
          }
          if (!resolved) {
            /* Try resolving from window manager as root. */
            wmWindowManager *wm = CTX_wm_manager(&C);
            PointerRNA root_ptr = RNA_id_pointer_create(&wm->id);
            if (RNA_path_resolve_full(&root_ptr, target_prop_path, &target_ptr, &prop, &index)) {
              RNA_property_string_set(&target_ptr, prop, hex_code);
              RNA_property_update(&C, &target_ptr, prop);
              resolved = true;
            }
          }

          if (!resolved) {
            /* Fallback: try resolving from the picker operator itself (relative path). */
            if (popup_data->op) {
              if (RNA_path_resolve_full(popup_data->op->ptr, target_prop_path, &target_ptr, &prop, &index)) {
                RNA_property_string_set(&target_ptr, prop, hex_code);
                RNA_property_update(&C, &target_ptr, prop);
                resolved = true;
              }
            }
          }
        }

        /* Clear the search field if picker operator available */
        if (popup_data->op) {
          RNA_string_set(popup_data->op->ptr, "glyph_search", "");

          /* Trigger live update to refresh preview - only for category tab edit dialog */
          if (popup_data->op->idname && STREQ(popup_data->op->idname, "SCREEN_OT_category_tab_edit_dialog")) {
            category_tab_edit_live_update_cb(&C, popup_data->op, 0);
          }
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
 *
 * Note: arg1 is the target operator (e.g., SCREEN_OT_category_tab_edit_dialog or wm.category_tag_create)
 * whose property should be updated when a glyph is selected.
 */
static void glyph_more_glyphs_button_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  wmOperator *target_op = static_cast<wmOperator *>(arg1);

  if (!target_op) {
    return;
  }

  /* Get the current category from target_op->ptr */
  char current_category[64] = "";
  RNA_string_get(target_op->ptr, "category", current_category);

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

  /* Direct popup path (without WM_OT_glyph_picker_grid):
   * use the dialog operator as `op` so glyph_search/glyph updates and live preview
   * callbacks are applied immediately in the open edit dialog. */
  GlyphGridPopupData *popup_data = new GlyphGridPopupData(
      target_op,
      target_op,
      target_op ? target_op->properties : nullptr,
      std::move(glyphs),
      current_category,
      all_categories);

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

  /* Try to get the target operator from target_operator_properties_ptr property first.
   * This is set by glyph_more_glyphs_default_cb before invoking this operator. */
  wmOperator *target_op = nullptr;
  IDProperty *target_op_properties = nullptr;
  bool has_explicit_target = false;
  char target_op_props_ptr_str[64] = "";
  RNA_string_get(op->ptr, "target_operator_properties_ptr", target_op_props_ptr_str);

  if (target_op_props_ptr_str[0] != '\0') {
    has_explicit_target = true;
    const uintptr_t target_props_ptr = uintptr_t(strtoull(target_op_props_ptr_str, nullptr, 10));

    if (target_props_ptr != 0) {
      wmWindowManager *wm = CTX_wm_manager(C);
      target_op_properties = reinterpret_cast<IDProperty *>(target_props_ptr);
      for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
           op_iter;
           op_iter = op_iter->prev)
      {
        if (op_iter && op_iter->properties == target_op_properties) {
          target_op = op_iter;
          break;
        }
      }
    }
  }

  /* Try to get the target operator from target_operator_ptr property next.
   * This is set by glyph_more_glyphs_default_cb before invoking this operator. */
  char target_op_ptr_str[64] = "";
  RNA_string_get(op->ptr, "target_operator_ptr", target_op_ptr_str);

  if (!target_op && target_op_ptr_str[0] != '\0') {
    has_explicit_target = true;
    /* Try to find the operator by pointer in wm->runtime->operators */
    const uintptr_t target_op_ptr = uintptr_t(strtoull(target_op_ptr_str, nullptr, 10));

    if (target_op_ptr != 0) {
      wmWindowManager *wm = CTX_wm_manager(C);
      for (wmOperator *op_iter = static_cast<wmOperator *>(wm->runtime->operators.last);
           op_iter;
           op_iter = op_iter->prev)
      {
        if ((uintptr_t)op_iter == target_op_ptr) {
          target_op = op_iter;
          break;
        }
      }
    }
  }

  /* Fallback: try context_active_operator_get only when no explicit target was provided.
   * If explicit target data was provided but couldn't be resolved to a live operator,
   * keep target_op null and rely on target_op_properties in the select callback. */
  if (!target_op) {
    if (!has_explicit_target) {
      wmOperator *active_op = context_active_operator_get(C);
      if (active_op && active_op != op) {
        target_op = active_op;
      }
    }
  }

  /* Create popup data with both picker op and target op */
  GlyphGridPopupData *popup_data = new GlyphGridPopupData(
      op, target_op, target_op_properties, std::move(glyphs), initial_category, all_categories);

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
   RNA_def_string(ot->srna, "target_operator_properties_ptr", nullptr, 64, "Target Operator Properties Pointer", "Internal: pointer to target operator properties data");
 }

/** \} */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Icon Grid Popup (Builtin Icons MVP)
 * \{ */

struct IconGridPopupItem {
  std::string identifier;
  std::string name;
  int icon;
};

struct IconGridPopupData {
  wmOperator *op; /* Dialog operator (SCREEN_OT_category_tab_edit_dialog). */
  wmOperator *target_op;
  IDProperty *target_op_properties;
  Vector<IconGridPopupItem> icons;
  std::string search_string;
  PopupBlockHandle *popup_handle;

  IconGridPopupData(wmOperator *op_,
                    wmOperator *target_op_,
                    IDProperty *target_op_properties_,
                    Vector<IconGridPopupItem> icons_)
      : op(op_),
        target_op(target_op_),
        target_op_properties(target_op_properties_),
        icons(std::move(icons_)),
        search_string(""),
        popup_handle(nullptr)
  {
  }
};

class IconGridView : public AbstractGridView {
 public:
  using OnIconSelectFn = std::function<void(bContext &C, const IconGridPopupItem &item)>;

 private:
  Vector<IconGridPopupItem> icons_;
  OnIconSelectFn on_icon_select_fn_;
  std::string search_filter_;

 public:
  void set_icons(const Vector<IconGridPopupItem> &icons)
  {
    icons_ = icons;
  }

  void set_search_filter(StringRef search_filter)
  {
    search_filter_ = search_filter;
  }

  void set_on_icon_select_fn(OnIconSelectFn fn)
  {
    on_icon_select_fn_ = fn;
  }

 protected:
  void build_items() override
  {
    std::string search_lower = search_filter_;
    if (!search_lower.empty()) {
      std::transform(search_lower.begin(), search_lower.end(), search_lower.begin(), ::tolower);
    }

    for (int64_t i = 0; i < icons_.size(); i++) {
      const IconGridPopupItem &icon_item = icons_[i];

      if (!search_lower.empty()) {
        std::string identifier_lower = icon_item.identifier;
        std::transform(
            identifier_lower.begin(), identifier_lower.end(), identifier_lower.begin(), ::tolower);

        std::string name_lower = icon_item.name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        if (identifier_lower.find(search_lower) == std::string::npos &&
            name_lower.find(search_lower) == std::string::npos)
        {
          continue;
        }
      }

      std::string view_id = "icon_" + std::to_string(i);
      PreviewGridItem &item = this->add_item<PreviewGridItem>(view_id,
                                                              icon_item.identifier,
                                                              BIFIconID(icon_item.icon));
      item.hide_label();

      if (on_icon_select_fn_) {
        item.set_on_activate_fn([this, i](bContext &C, PreviewGridItem & /*new_active*/) {
          on_icon_select_fn_(C, icons_[i]);
        });
      }
    }
  }
};

static Vector<IconGridPopupItem> icon_grid_builtin_icons_collect()
{
  Vector<IconGridPopupItem> items;

  for (const EnumPropertyItem *item = rna_enum_icon_items; item->identifier != nullptr; item++) {
    if (item->identifier[0] == '\0') {
      continue; /* Separator row. */
    }

    IconGridPopupItem icon_item;
    icon_item.identifier = item->identifier;
    icon_item.name = item->name ? item->name : item->identifier;
    icon_item.icon = item->value;
    items.append(std::move(icon_item));
  }

  return items;
}

static bool icon_grid_writeback_icon_key(bContext &C,
                                         IconGridPopupData *popup_data,
                                         const char *icon_key)
{
  if (!popup_data || !icon_key || icon_key[0] == '\0') {
    return false;
  }

  bool resolved = false;

  if (popup_data->target_op_properties) {
    IDProperty *idprop = IDP_GetPropertyFromGroup(popup_data->target_op_properties, "icon_key");
    if (!idprop) {
      idprop = IDP_NewString(icon_key, "icon_key");
      IDP_AddToGroup(popup_data->target_op_properties, idprop);
      resolved = true;
    }
    else if (idprop->type == IDP_STRING) {
      IDP_AssignString(idprop, icon_key);
      resolved = true;
    }
  }

  if (!resolved && popup_data->target_op && popup_data->target_op->ptr) {
    PointerRNA target_ptr;
    PropertyRNA *target_prop;
    int index;
    if (RNA_path_resolve_full(popup_data->target_op->ptr, "icon_key", &target_ptr, &target_prop, &index)) {
      RNA_property_string_set(&target_ptr, target_prop, icon_key);
      RNA_property_update(&C, &target_ptr, target_prop);
      resolved = true;
    }
  }

  if (!resolved && popup_data->op && popup_data->op->ptr) {
    RNA_string_set(popup_data->op->ptr, "icon_key", icon_key);
    resolved = true;
  }

  return resolved;
}

static Block *icon_grid_popup_block_create(bContext *C, ARegion *region, void *arg)
{
  IconGridPopupData *popup_data = static_cast<IconGridPopupData *>(arg);

  if (popup_data->op) {
    char search_buf[256] = "";
    RNA_string_get(popup_data->op->ptr, "icon_search", search_buf);
    popup_data->search_string = search_buf;
  }

  Block *block = block_begin(C, region, "icon_grid_popup", EmbossType::Emboss);
  block_flag_enable(block, BLOCK_LOOP | BLOCK_MOVEMOUSE_QUIT);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);
  block_bounds_set_centered(block, 6 * UI_SCALE_FAC);

  const int popup_width = 46 * UI_UNIT_X;
  const int popup_height = 60 * UI_UNIT_Y;

  Layout &layout = block_layout(block,
                                LayoutDirection::Vertical,
                                LayoutType::Panel,
                                0,
                                0,
                                popup_width,
                                popup_height,
                                0,
                                style_get());

  if (popup_data->op) {
    Layout &search_row = layout.row(false);
    search_row.prop(popup_data->op->ptr,
                    "icon_search",
                    ITEM_R_TEXT_BUT_FORCE_SEMI_MODAL_ACTIVE,
                    "",
                    ICON_VIEWZOOM);
  }

  Layout &grid_col = layout.column(false);
  grid_col.ui_units_x_set(45);
  grid_col.fixed_size_set(true);

  std::unique_ptr<IconGridView> grid_view_ptr = std::make_unique<IconGridView>();
  IconGridView *grid_view = grid_view_ptr.get();
  grid_view->set_icons(popup_data->icons);
  grid_view->set_search_filter(popup_data->search_string);
  grid_view->set_tile_size(UI_UNIT_X * 1.8f, UI_UNIT_Y * 1.8f);

  grid_view->set_on_icon_select_fn([popup_data](bContext &C, const IconGridPopupItem &item) {
    if (popup_data->op) {
      RNA_string_set(popup_data->op->ptr, "icon_key", item.identifier.c_str());
      RNA_enum_set(popup_data->op->ptr, "icon_source", 1);
      RNA_enum_set(popup_data->op->ptr, "display_mode_ui", 1);
      RNA_enum_set(popup_data->op->ptr, "custom_icon_mode_ui", 0);
      RNA_string_set(popup_data->op->ptr, "icon_search", "");
    }

    icon_grid_writeback_icon_key(C, popup_data, item.identifier.c_str());

    if (popup_data->op && popup_data->op->idname &&
        STREQ(popup_data->op->idname, "SCREEN_OT_category_tab_edit_dialog"))
    {
      category_tab_edit_live_update_cb(&C, popup_data->op, 0);
    }

    if (popup_data->popup_handle) {
      popup_data->popup_handle->menuretval = RETURN_OK;
    }

    WM_main_add_notifier(NC_WINDOW, nullptr);
  });

  AbstractGridView *grid = block_add_view(*block, "icon_grid", std::move(grid_view_ptr));
  GridViewBuilder builder(*block);
  builder.build_grid_view(*C, *grid, grid_col);

  return block;
}

static void icon_grid_popup_free(void *arg)
{
  IconGridPopupData *popup_data = static_cast<IconGridPopupData *>(arg);
  delete popup_data;
}

static void icon_more_icons_button_cb(bContext *C, void *arg1, void * /*arg2*/)
{
  wmOperator *target_op = static_cast<wmOperator *>(arg1);
  if (!target_op) {
    return;
  }

  Vector<IconGridPopupItem> icons = icon_grid_builtin_icons_collect();
  if (icons.is_empty()) {
    WM_global_report(RPT_WARNING, "No built-in icons found");
    return;
  }

  IconGridPopupData *popup_data = new IconGridPopupData(
      target_op, target_op, target_op->properties, std::move(icons));

  PopupBlockHandle *handle = popup_block_create(
      C, nullptr, nullptr, icon_grid_popup_block_create, nullptr, popup_data, icon_grid_popup_free, false);

  popup_data->popup_handle = handle;
  handle->popup = true;

  wmWindow *window = CTX_wm_window(C);
  popup_handlers_add(C, &window->runtime->modalhandlers, handle, 0);
  WM_event_add_mousemove(window);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Popup Block Creation
 * \{ */

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

  layout.separator();

  /* Tags section in a sub-panel - only show for non-reserved categories */
  /* Get filter mode from window manager (0 = all tags). */
  uint32_t filter_mode_flag = 0;
  switch (wm->category_tag_filter_mode) {
    case 1:
      filter_mode_flag = uint32_t(CategoryTagMode::OBJECT_MODE);
      break;
    case 2:
      filter_mode_flag = uint32_t(CategoryTagMode::EDIT_MODE);
      break;
    case 3:
      filter_mode_flag = uint32_t(CategoryTagMode::SCULPT_MODE);
      break;
    case 4:
      filter_mode_flag = uint32_t(CategoryTagMode::VERTEX_PAINT);
      break;
    case 5:
      filter_mode_flag = uint32_t(CategoryTagMode::WEIGHT_PAINT);
      break;
    case 6:
      filter_mode_flag = uint32_t(CategoryTagMode::TEXTURE_PAINT);
      break;
    case 7:
      filter_mode_flag = uint32_t(CategoryTagMode::UV_EDIT);
      break;
    case 8:
      filter_mode_flag = uint32_t(CategoryTagMode::POSE_MODE);
      break;
    case 9:
      filter_mode_flag = uint32_t(CategoryTagMode::GEOMETRY_NODES);
      break;
    case 10:
      filter_mode_flag = uint32_t(CategoryTagMode::SHADER_EDITOR);
      break;
    default:
      filter_mode_flag = 0;
      break;
  }

  /* Get all active tags for the header (unfiltered) */
  const std::string tags_data_header = get_tags_for_category_ui(wm, category, 0, space_type);

  /* Get filtered tags for the body list */
  const std::string tags_data_body = get_tags_for_category_ui(wm, category, filter_mode_flag, space_type);

  /* Don't show tags panel for reserved categories */
  if (!is_reserved) {
    /* Create unique panel idname per category so each category has its own collapse state */
    char tags_panel_idname[128];
    SNPRINTF(tags_panel_idname, "tags_list_%s", category);

    /* Check if category has any active tags assigned */
    const char *category_tags_string = category_tags_string_lookup(wm, category, space_type);
    const bool category_has_tags = (category_tags_string && category_tags_string[0] != '\0');
    /* Panel should be closed by default if category has tags, open if no tags */
    const bool panel_default_closed = category_has_tags; /* true = closed, false = open */

    PanelLayout tags_panel = layout.panel(C, tags_panel_idname, panel_default_closed);

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

      /* Filter menu button - opens popup with Current Mode / All Tags toggle buttons */
      header_row.menu("SCREEN_MT_category_tag_filter_toggle", "", ICON_FILTER);

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
                                              false,  /* center_glyph - left align for category buttons */
                                              0, "",  /* icon_id, icon_path - no icon for this button */
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
                RNA_int_set(op_ptr, "space_type", space_type);
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
  if (U.category_tabs_allow_edit) {
    return false;
  }
  return ED_operator_regionactive(C);
}

static bool category_tab_try_auto_detect_extension_icon(bContext *C,
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

  const std::string escaped_category = category_tab_escape_for_python_literal(category);

  char python_expr[2048];
  SNPRINTF(python_expr,
           "(lambda _r: json.dumps([_r[0].replace('\\\\', '/'), _r[1]]))"
           "(__import__('bl_ui.space_userpref', fromlist=[''])"
           "._auto_detect_extension_icon_path('%s'))",
           escaped_category.c_str());

  const char *imports[] = {"json", nullptr};

  char *result_str = nullptr;
  char *err_msg = nullptr;
  BPy_RunErrInfo err_info = {false, nullptr, "", &err_msg};
  const bool success = BPY_run_string_as_string(C, imports, python_expr, &err_info, &result_str);

  if (!success || !result_str) {
    if (err_msg) {
      MEM_delete(err_msg);
    }
    return false;
  }

  blender::Vector<std::string> parts;
  const bool parsed = category_tab_parse_json_string_array_minimal(result_str, parts);
  bool detected = false;

  if (parsed && parts.size() >= 2) {
    const std::string icon_path = category_tab_decode_json_unicode(parts[0].c_str());
    const std::string icon_provider = category_tab_decode_json_unicode(parts[1].c_str());

    if (!icon_path.empty()) {
      BLI_strncpy(r_icon_path, icon_path.c_str(), 1024);
      BLI_strncpy(r_icon_provider, icon_provider.c_str(), 128);
      detected = true;
    }
  }

  MEM_delete(result_str);
  if (err_msg) {
    MEM_delete(err_msg);
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

  /* Set filter mode to current object mode when opening the dialog */
  wm->category_tag_filter_mode = get_current_object_mode_filter_value(C);

  bool has_override = false;
  bool override_is_empty = false;
  bool override_icon_needs_mapping = false;
  bool user_glyph_override_assigned = false;
  bool explicit_icon_mode_assigned = false;

  /* First check category_glyph_overrides (user changes in current session) */
  for (CategoryGlyphItem *item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category)) {
      const bool has_icon_payload = (item->icon_key[0] != '\0') || (item->icon_path[0] != '\0') ||
                                    (item->icon_provider[0] != '\0');
      const bool has_explicit_icon_mode = ELEM(item->icon_source, 1, 2); /* MANUAL/OFF */
      if (has_explicit_icon_mode) {
        explicit_icon_mode_assigned = true;
      }

      /* Check if override is empty (created by tag restore but has no actual data) */
      if (item->display_name[0] == '\0' && item->glyph[0] == '\0' && is_zero_v3(item->color) &&
          !has_icon_payload && !has_explicit_icon_mode)
      {
        override_is_empty = true;
        has_override = true; /* Mark as found but empty, so we skip checking mappings again */
        break;
      }

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
        user_glyph_override_assigned = true;
      }
      else if (extracted_glyph[0] != '\0') {
        STRNCPY(hex_code, extracted_glyph);
        user_glyph_override_assigned = true;
      }
      else {
        hex_code[0] = '\0';
      }
      RNA_string_set(op->ptr, "glyph", hex_code);
      RNA_float_set_array(op->ptr, "color", item->color);
      RNA_enum_set(op->ptr, "icon_source", item->icon_source);
      RNA_string_set(op->ptr, "icon_key", item->icon_key);
      RNA_string_set(op->ptr, "icon_path", item->icon_path);
      RNA_string_set(op->ptr, "icon_provider", item->icon_provider);
      override_icon_needs_mapping = (!has_icon_payload && !has_explicit_icon_mode);
      has_override = true;
      break;
    }
  }

  /* If override is used for text/glyph/color only and doesn't carry explicit icon data,
   * keep icon fields from persisted mappings (JSON source of truth). */
  if (has_override && !override_is_empty && override_icon_needs_mapping) {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        RNA_enum_set(op->ptr, "icon_source", item->icon_source);
        RNA_string_set(op->ptr, "icon_key", item->icon_key);
        RNA_string_set(op->ptr, "icon_path", item->icon_path);
        RNA_string_set(op->ptr, "icon_provider", item->icon_provider);
        if (ELEM(item->icon_source, 1, 2)) {
          explicit_icon_mode_assigned = true;
        }
        break;
      }
    }
  }

  /* If no override OR override is empty, check category_glyph_mappings (saved settings from JSON) */
  if (!has_override || override_is_empty) {
    bool found_in_mappings = false;
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        found_in_mappings = true;
        /* Load display_name from mappings */
        if (item->display_name[0] != '\0') {
          RNA_string_set(op->ptr, "display_name", item->display_name);
        }

        /* Load glyph.
         * For glyph-only categories, `category` itself is the canonical id/default glyph.
         * Do not seed the editable glyph field with that intrinsic id value, otherwise live
         * preview/save can treat it like a user-entered override. */
        if (item->glyph[0] != '\0') {
          const bool is_glyph_only_category = is_single_glyph_str(category);
          const bool is_intrinsic_category_glyph = is_glyph_only_category && STREQ(item->glyph, category);
          if (!is_intrinsic_category_glyph) {
            char hex_code[16];
            utf8_to_hex_codepoint(item->glyph, hex_code, sizeof(hex_code));
            RNA_string_set(op->ptr, "glyph", hex_code);
          }
        }

        /* Load color if not default black */
        if (!is_zero_v3(item->color)) {
          RNA_float_set_array(op->ptr, "color", item->color);
        }
        RNA_enum_set(op->ptr, "icon_source", item->icon_source);
        RNA_string_set(op->ptr, "icon_key", item->icon_key);
        RNA_string_set(op->ptr, "icon_path", item->icon_path);
        RNA_string_set(op->ptr, "icon_provider", item->icon_provider);
        /* When using mappings, mark as no override since data comes from JSON */
        has_override = false;
        break;
      }
    }
  }

  /* If no explicit icon mode/payload is set, try to detect extension root icon
   * (icon.png/icon.webp/...) for this category.
   * Only do this when there is no user glyph override; fallback glyphs are ignored. */
  if (!user_glyph_override_assigned && !explicit_icon_mode_assigned) {

    char current_icon_key[128] = "";
    char current_icon_path[1024] = "";
    char current_icon_provider[128] = "";
    RNA_string_get(op->ptr, "icon_key", current_icon_key);
    RNA_string_get(op->ptr, "icon_path", current_icon_path);
    RNA_string_get(op->ptr, "icon_provider", current_icon_provider);

    const bool has_icon_payload =
        (current_icon_key[0] != '\0') || (current_icon_path[0] != '\0') || (current_icon_provider[0] != '\0');

    if (!has_icon_payload) {
      char detected_icon_path[1024] = "";
      char detected_icon_provider[128] = "";
      if (category_tab_try_auto_detect_extension_icon(
              C, category, detected_icon_path, detected_icon_provider))
      {
        RNA_enum_set(op->ptr, "icon_source", 0); /* AUTO */
        RNA_string_set(op->ptr, "icon_path", detected_icon_path);
        if (detected_icon_provider[0] != '\0') {
          RNA_string_set(op->ptr, "icon_provider", detected_icon_provider);
        }
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

  /* If glyph is still empty, use panel_category_glyph_lookup.
   * For glyph-only categories, avoid seeding with the intrinsic category-id glyph
   * (same value as `category`) for the same reason as above. */
  char current_glyph[16] = "";
  RNA_string_get(op->ptr, "glyph", current_glyph);
  if (current_glyph[0] == '\0') {
    float glyph_color[3] = {0.0f, 0.0f, 0.0f};
    bool is_fallback = false;

    const char *default_glyph = panel_category_glyph_lookup(wm, category, nullptr, &is_fallback, glyph_color);

    if (default_glyph) {
      const bool is_glyph_only_category = is_single_glyph_str(category);
      const bool is_intrinsic_category_glyph = is_glyph_only_category && STREQ(default_glyph, category);
      if (!is_intrinsic_category_glyph) {
      char hex_code[16];
      utf8_to_hex_codepoint(default_glyph, hex_code, sizeof(hex_code));
      RNA_string_set(op->ptr, "glyph", hex_code);
      }
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
  const int current_icon_source = RNA_enum_get(op->ptr, "icon_source");
  char current_icon_key[128] = "";
  char current_icon_path[1024] = "";
  char current_icon_provider[128] = "";
  RNA_string_get(op->ptr, "icon_key", current_icon_key);
  RNA_string_get(op->ptr, "icon_path", current_icon_path);
  RNA_string_get(op->ptr, "icon_provider", current_icon_provider);

  const bool has_icon_key = (current_icon_key[0] != '\0');
  const bool has_icon_path = (current_icon_path[0] != '\0');
  const bool has_icon_payload = has_icon_key || has_icon_path;
  const bool icon_mode_enabled = (current_icon_source != 2); /* OFF */
  int current_glyph_mode = 0;
  for (CategoryGlyphItem *item = static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (STREQ(item->category, category)) {
      current_glyph_mode = item->glyph_mode;
      break;
    }
  }
  if (current_glyph_mode == 0) {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (STREQ(item->category, category)) {
        current_glyph_mode = item->glyph_mode;
        break;
      }
    }
  }

  int display_mode_ui = 0; /* GLYPH */
  if (current_glyph_mode == 1) {
    display_mode_ui = 2; /* TEXT */
  }
  else if (icon_mode_enabled && has_icon_payload) {
    display_mode_ui = 1; /* CUSTOM_ICON */
  }
  RNA_enum_set(op->ptr, "display_mode_ui", display_mode_ui);
  RNA_enum_set(op->ptr, "custom_icon_mode_ui", has_icon_path ? 1 : 0);

  RNA_enum_set(op->ptr, "original_icon_source", current_icon_source);
  RNA_enum_set(op->ptr, "original_glyph_mode", current_glyph_mode);
  RNA_string_set(op->ptr, "original_icon_key", current_icon_key);
  RNA_string_set(op->ptr, "original_icon_path", current_icon_path);
  RNA_string_set(op->ptr, "original_icon_provider", current_icon_provider);
  RNA_float_set_array(op->ptr, "original_color", current_color);
  RNA_boolean_set(op->ptr, "original_has_override", has_override);

  /* Save original tags for cancel functionality - read from WM mappings/overrides
   * IMPORTANT: Filter by space_type to get per-space tags */
  char original_tags[256] = "";
  const char *tags_str = nullptr;

  /* First check overrides with matching space_type */
  for (CategoryGlyphItem *item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (item->space_type == space_type && STREQ(item->category, category)) {
      tags_str = item->tags;
      break;
    }
  }

  /* If no override with matching space_type, check global overrides (space_type = -1) */
  if (!tags_str || tags_str[0] == '\0') {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (item->space_type == -1 && STREQ(item->category, category)) {
        tags_str = item->tags;
        break;
      }
    }
  }

  /* If no override, check mappings with matching space_type */
  if (!tags_str || tags_str[0] == '\0') {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (item->space_type == space_type && STREQ(item->category, category)) {
        tags_str = item->tags;
        break;
      }
    }
  }

  /* If no mapping with matching space_type, check global mappings (space_type = -1) */
  if (!tags_str || tags_str[0] == '\0') {
    for (CategoryGlyphItem *item =
             static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<CategoryGlyphItem *>(item->next))
    {
      if (item->space_type == -1 && STREQ(item->category, category)) {
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

  /* Get space_type from current context for proper per-space tag storage */
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;

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

  /* Get or create override with matching space_type */
  wmWindowManager *wm = CTX_wm_manager(C);
  CategoryGlyphItem *item = nullptr;

  for (CategoryGlyphItem *it =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
       it;
       it = static_cast<CategoryGlyphItem *>(it->next))
  {
    if (it->space_type == space_type && STREQ(it->category, category)) {
      item = it;
      break;
    }
  }

  if (!item) {
    item = MEM_new<CategoryGlyphItem>(__func__);
    STRNCPY(item->category, category);
    item->space_type = space_type;
    item->glyph[0] = '\0';
    item->display_name[0] = '\0';
    zero_v3(item->color);
    item->tags[0] = '\0';
    item->icon_key[0] = '\0';
    item->icon_path[0] = '\0';
    item->icon_provider[0] = '\0';
    item->icon_source = 0;
    item->glyph_mode = 0;
    BLI_addtail(&wm->category_glyph_overrides, item);
  }

  /* Resolve base/default glyph from stable mappings first (canonical category key),
   * and only then use lookup as a fallback.
   *
   * Rationale:
   * - `panel_category_glyph_lookup()` is runtime-oriented and can be influenced by live override state.
   * - Save logic must compare against a stable baseline (mapping default), otherwise custom glyphs can be
   *   incorrectly treated as default and cleared.
   */
  char default_glyph[8] = "";
  bool default_glyph_found = false;

  for (CategoryGlyphItem *map_item =
           static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
       map_item;
       map_item = static_cast<CategoryGlyphItem *>(map_item->next))
  {
    if (!STREQ(map_item->category, category)) {
      continue;
    }

    if (map_item->default_glyph[0] != '\0') {
      STRNCPY(default_glyph, map_item->default_glyph);
      default_glyph_found = true;
    }
    else if (is_single_glyph_str(category) && map_item->glyph[0] != '\0') {
      /* Glyph-only category: reset/default is its glyph value. */
      STRNCPY(default_glyph, map_item->glyph);
      default_glyph_found = true;
    }
    /* text_only categories intentionally keep default_glyph empty so reset falls back to first letter. */
    break;
  }

  if (!default_glyph_found) {
    /* Fallback for categories not present in mappings yet.
     * Temporarily clear live override glyph to avoid self-matching. */
    char previous_override_glyph[8] = "";
    if (item->glyph[0] != '\0') {
      STRNCPY(previous_override_glyph, item->glyph);
      item->glyph[0] = '\0';
    }

    bool is_fallback = false;
    const char *default_glyph_lookup = panel_category_glyph_lookup(
        wm, category, nullptr, &is_fallback, nullptr);
    if (default_glyph_lookup) {
      STRNCPY(default_glyph, default_glyph_lookup);
    }

    if (previous_override_glyph[0] != '\0') {
      STRNCPY(item->glyph, previous_override_glyph);
    }
  }

  /* Update values */
  STRNCPY(item->display_name, display_name);
  copy_v3_v3(item->color, color);
  const int display_mode_ui_exec = RNA_enum_get(op->ptr, "display_mode_ui");
  const int custom_icon_mode_ui_exec = RNA_enum_get(op->ptr, "custom_icon_mode_ui");
  int resolved_icon_source_exec = RNA_enum_get(op->ptr, "icon_source");
  if (display_mode_ui_exec == 0 || display_mode_ui_exec == 2) {
    resolved_icon_source_exec = 2; /* OFF */
  }
  else if (custom_icon_mode_ui_exec == 0) {
    resolved_icon_source_exec = 1; /* MANUAL: Blender Icon */
  }
  else {
    resolved_icon_source_exec = 0; /* AUTO: path/provider chain (external icon) */
  }
  RNA_enum_set(op->ptr, "icon_source", resolved_icon_source_exec);
  item->icon_source = resolved_icon_source_exec;
  item->glyph_mode = (display_mode_ui_exec == 2) ? 1 : 0;
  RNA_string_get(op->ptr, "icon_key", item->icon_key);
  RNA_string_get(op->ptr, "icon_path", item->icon_path);
  RNA_string_get(op->ptr, "icon_provider", item->icon_provider);

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
  category_tab_current_dialog_op = nullptr;
  category_tab_preview_button = nullptr;

  /* Redraw */
  WM_main_add_notifier(NC_WINDOW, nullptr);

  /* Save updated data to JSON (including tags which might have been modified) */
  {
    char python_cmd[8192];
    /* Convert color to hex for Python set_category_data */
    char color_hex[8];
    SNPRINTF(color_hex, "%02x%02x%02x",
             int(item->color[0] * 255.0f),
             int(item->color[1] * 255.0f),
             int(item->color[2] * 255.0f));

    /* Convert UTF-8 glyph back to hex codepoint for storage */
    char glyph_hex[16] = "";
    if (item->glyph[0] != '\0') {
      utf8_to_hex_codepoint(item->glyph, glyph_hex, sizeof(glyph_hex));
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

    /* Escape string parameters for Python (backslashes in paths, quotes, etc.) */
    char category_esc[128];
    char display_name_esc[64];
    char icon_key_esc[256];
    char icon_path_esc[2048];
    char icon_provider_esc[256];
    BLI_str_escape(category_esc, category, sizeof(category_esc));
    BLI_str_escape(display_name_esc, item->display_name, sizeof(display_name_esc));
    BLI_str_escape(icon_key_esc, item->icon_key, sizeof(icon_key_esc));
    BLI_str_escape(icon_path_esc, item->icon_path, sizeof(icon_path_esc));
    BLI_str_escape(icon_provider_esc, item->icon_provider, sizeof(icon_provider_esc));

    /* DEBUG: Log before calling Python set_category_data */
    printf("[C++ SAVE] Calling set_category_data: category='%s', space_type=%d\n", category, space_type);
    printf("[C++ SAVE] item->tags='%s' (NOT passed to Python - tags managed by Python)\n", item->tags);

    SNPRINTF(python_cmd,
             "from bl_ui.space_userpref import set_category_data\n"
             "set_category_data('%s', display_name='%s', glyph='%s', color='%s', "
             "icon_source='%s', icon_key='%s', icon_path='%s', icon_provider='%s', glyph_mode='%s', space_type=%d)\n",
             category_esc,
             display_name_esc,
             glyph_hex,
             color_hex,
             icon_source_py,
             icon_key_esc,
             icon_path_esc,
             icon_provider_esc,
             (item->glyph_mode == 1) ? "first_letter" : "auto",
             space_type);
    const char *imports_none[] = {nullptr};
    BPY_run_string_exec(C, imports_none, python_cmd);

    /* DEBUG: Confirm call completed */
    printf("[C++ SAVE] set_category_data call completed for '%s'\n", category);
  }

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
  }

  /* Create operator for the popup dialog */
  wmOperator *target_op = MEM_new<wmOperator>(__func__);
  target_op->type = ot;
  target_op->ptr = props_ptr;
  target_op->properties = properties;
  target_op->reports = op->reports;
  target_op->flag = OP_IS_INVOKE;

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
}

void centered_popup_operator_register()
{
  WM_operatortype_append(CENTERED_OT_popup_operator_wrapper);
}

/** \} */

}  // namespace blender::ui
