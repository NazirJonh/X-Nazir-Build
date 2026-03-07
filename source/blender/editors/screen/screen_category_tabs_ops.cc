/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edscreen
 *
 * Category Tab Operators - Operator definitions for category tab editing.
 *
 * This module provides:
 * - Reset operator (reset to defaults)
 * - Paste Glyph operator (paste from clipboard)
 * - Cancel operator (cancel dialog)
 * - Save operator (save changes)
 * - Main dialog operator
 */

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_fileops.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "screen_intern.hh"

/* For category tab functions from interface module */
#include "interface_intern.hh"
#include "../interface/interface_intern.hh" /* for UI_SELECT_DRAW and Button */

namespace blender {

/* Import functions from interface module into this namespace for convenience */
using ui::category_tab_current_dialog_op;
using ui::category_tab_popup_block;
using ui::category_tab_edit_popup_cancel_cb;
using ui::category_tab_edit_live_update_cb;
using ui::category_tab_edit_poll;
using ui::category_tab_edit_dialog_invoke;
using ui::category_tab_edit_dialog_exec;
using ui::find_panel_label_for_category;
using ui::is_single_glyph_str;
using ui::utf8_to_hex_codepoint;
using ui::context_active_but_get_respect_popup;

/* -------------------------------------------------------------------- */
/** \name Category Tab Icon Source Enum (Stage 1 wiring)
 * \{ */

enum eCategoryTabEditDisplayMode {
  CATEGORY_TAB_EDIT_MODE_GLYPH = 0,
  CATEGORY_TAB_EDIT_MODE_CUSTOM_ICON = 1,
  CATEGORY_TAB_EDIT_MODE_TEXT = 2,
};

enum eCategoryTabCustomIconMode {
  CATEGORY_TAB_CUSTOM_ICON_MODE_BLENDER = 0,
  CATEGORY_TAB_CUSTOM_ICON_MODE_CUSTOM = 1,
};

static const EnumPropertyItem rna_enum_category_tab_icon_source_items[] = {
    {ui::CATEGORY_TAB_ICON_SOURCE_AUTO,
     "AUTO",
     ICON_NONE,
     "Auto",
     "Use automatic icon resolve chain (provider/fallback)"},
    {ui::CATEGORY_TAB_ICON_SOURCE_MANUAL,
     "MANUAL",
     ICON_NONE,
     "Manual",
     "Use manual icon key/path override for this category"},
    {ui::CATEGORY_TAB_ICON_SOURCE_OFF,
     "OFF",
     ICON_NONE,
     "Off",
     "Disable icon resolve and use fallback glyph/text"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_category_tab_glyph_mode_items[] = {
    {ui::CATEGORY_TAB_GLYPH_MODE_AUTO,
     "AUTO",
     ICON_NONE,
     "Auto",
     "Use configured/default glyph behavior"},
    {ui::CATEGORY_TAB_GLYPH_MODE_FIRST_LETTER,
     "FIRST_LETTER",
     ICON_NONE,
     "First Letter",
     "Force first letter of category"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_category_tab_edit_display_mode_items[] = {
    {CATEGORY_TAB_EDIT_MODE_GLYPH,
     "GLYPH",
     ICON_NONE,
     "Glyph",
     "Show and edit glyph for this category"},
    {CATEGORY_TAB_EDIT_MODE_CUSTOM_ICON,
     "CUSTOM_ICON",
     ICON_NONE,
     "Icon",
     "Show and edit custom icon for this category"},
    {CATEGORY_TAB_EDIT_MODE_TEXT,
     "TEXT",
     ICON_NONE,
     "First Letter",
     "Use text/fallback behavior without custom icon override"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem rna_enum_category_tab_custom_icon_mode_items[] = {
    {CATEGORY_TAB_CUSTOM_ICON_MODE_BLENDER,
     "BLENDER",
     ICON_NONE,
     "Blender",
     "Use one of Blender built-in icons"},
    {CATEGORY_TAB_CUSTOM_ICON_MODE_CUSTOM,
     "CUSTOM",
     ICON_NONE,
     "Custom",
     "Use external icon path (read-only source path)"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Presets Enum
 * \{ */

/** Category tab color preset values. */
enum CategoryTabColorPreset {
  CATEGORY_TAB_COLOR_NONE = -1,
  CATEGORY_TAB_COLOR_01 = 0,
  CATEGORY_TAB_COLOR_02 = 1,
  CATEGORY_TAB_COLOR_03 = 2,
  CATEGORY_TAB_COLOR_04 = 3,
  CATEGORY_TAB_COLOR_05 = 4,
  CATEGORY_TAB_COLOR_06 = 5,
  CATEGORY_TAB_COLOR_07 = 6,
  CATEGORY_TAB_COLOR_08 = 7,
};

/** Get RGB color from preset value (reads from theme) */
void category_tab_color_preset_to_rgb(const int preset, float r_color[3])
{
  if (preset == CATEGORY_TAB_COLOR_NONE) {
    zero_v3(r_color);
    return;
  }

  bTheme *btheme = ui::theme::theme_get();
  /* Theme glyph colors are stored as 0-7, matching our preset values. */
  if (preset >= 0 && preset < GLYPH_COLOR_TOT) {
    const uchar *color_uchar = btheme->glyph_color[preset].color;
    r_color[0] = color_uchar[0] / 255.0f;
    r_color[1] = color_uchar[1] / 255.0f;
    r_color[2] = color_uchar[2] / 255.0f;
  }
  else {
    zero_v3(r_color);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Reset Operator
 * \{ */

static wmOperatorStatus category_tab_reset_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  return WM_operator_props_popup_confirm_ex(
      C, op, event,
      IFACE_("Reset Category Data"),
      CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Confirm"));
}

static void category_tab_reset_apply_to_operator(bContext *C,
                                                  wmOperator *target_op,
                                                  const char *category,
                                                  const char *default_display_name,
                                                  const char *default_glyph,
                                                  const float default_color[3],
                                                  const bool reset_name,
                                                  const bool reset_glyph,
                                                  const bool reset_color)
{
  if (target_op == nullptr) {
    return;
  }

  if (reset_name) {
    if (default_display_name != nullptr && default_display_name[0] != '\0') {
      RNA_string_set(target_op->ptr, "display_name", default_display_name);
    }
    else if (is_single_glyph_str(category)) {
      ARegion *region = CTX_wm_region(C);
      const char *panel_label = find_panel_label_for_category(region, category);
      RNA_string_set(target_op->ptr, "display_name", panel_label ? panel_label : "");
    }
    else {
      RNA_string_set(target_op->ptr, "display_name", category);
    }
  }

  if (reset_glyph) {
    if (default_glyph != nullptr) {
      char glyph_hex[16];
      utf8_to_hex_codepoint(default_glyph, glyph_hex, sizeof(glyph_hex));
      RNA_string_set(target_op->ptr, "glyph", glyph_hex);
    }
    else {
      RNA_string_set(target_op->ptr, "glyph", "");
    }

    /* Resetting glyph should also leave explicit First Letter mode.
     * Otherwise live-update/save keeps `glyph_mode=FIRST_LETTER` from previous state,
     * and Reset appears unsaved after closing the dialog. */
    PropertyRNA *display_mode_prop = RNA_struct_find_property(target_op->ptr, "display_mode_ui");
    if (display_mode_prop != nullptr) {
      RNA_enum_set(target_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_GLYPH);
    }
  }

  if (reset_color) {
    RNA_float_set_array(target_op->ptr, "color", default_color);
  }

  RNA_string_set(target_op->ptr, "glyph_search", "");
}

static void category_tab_reset_tag_redraw(bContext *C, wmWindowManager *wm, ScrArea *area)
{
  /* Force redraw of the popup to update glyph preview/tag UI. */
  if (category_tab_popup_block) {
    ARegion *region = CTX_wm_region(C);
    if (region) {
      ED_region_tag_redraw_no_rebuild(region);
      ED_region_tag_refresh_ui(region);
    }
  }

  /* Force redraw of all header regions in current area to update category tabs. */
  if (area) {
    for (ARegion *region = static_cast<ARegion *>(area->regionbase.first); region;
         region = static_cast<ARegion *>(region->next))
    {
      ED_region_tag_redraw(region);
    }
  }
  else {
    /* If area is null (popup context), iterate through all windows and screens. */
    wmWindow *win = static_cast<wmWindow *>(wm->windows.first);
    while (win) {
      bScreen *screen = WM_window_get_active_screen(win);
      if (screen) {
        for (ScrArea *area_iter = static_cast<ScrArea *>(screen->areabase.first); area_iter;
             area_iter = static_cast<ScrArea *>(area_iter->next))
        {
          for (ARegion *region = static_cast<ARegion *>(area_iter->regionbase.first); region;
               region = static_cast<ARegion *>(region->next))
          {
            ED_region_tag_redraw(region);
          }
        }
      }
      win = static_cast<wmWindow *>(win->next);
    }
  }

  /* Force redraw of the screen to update category tabs in all areas. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
  WM_main_add_notifier(NC_SCREEN | NA_EDITED, nullptr);
}

struct CategoryTabResetDefaults {
  const char *glyph = nullptr;
  const char *display_name = nullptr;
  float color[3] = {0.0f, 0.0f, 0.0f};
};

static CategoryTabResetDefaults compute_reset_defaults(wmWindowManager *wm,
                                                       const char *category,
                                                       const bool reset_glyph)
{
  CategoryTabResetDefaults defaults;

  if (!(wm && wm->category_glyph_mappings.first)) {
    return defaults;
  }

  for (CategoryGlyphItem *item = static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (!STREQ(item->category, category)) {
      continue;
    }

    const bool is_glyph_only_category = is_single_glyph_str(category);

    if (reset_glyph) {
      /* Reset should always clear explicit first-letter mode in persisted mapping. */
      item->glyph_mode = ui::CATEGORY_TAB_GLYPH_MODE_AUTO;
    }

    if (is_glyph_only_category) {
      if (item->default_glyph[0] != '\0') {
        defaults.glyph = item->default_glyph;
      }
      else if (item->glyph[0] != '\0') {
        defaults.glyph = item->glyph;
      }
    }
    else {
      bool has_valid_default_glyph = false;
      if (item->default_glyph[0] != '\0') {
        has_valid_default_glyph = !ui::category_tab_glyph_is_fallback_letter(item->default_glyph,
                                                                              category);
      }

      if (has_valid_default_glyph) {
        defaults.glyph = item->default_glyph;
        if (item->glyph[0] != '\0' && !STREQ(item->glyph, item->default_glyph)) {
          STRNCPY(item->glyph, item->default_glyph);
        }
        if (!is_zero_v3(item->color)) {
          zero_v3(item->color);
        }
      }
      else {
        defaults.glyph = nullptr;
        if (item->glyph[0] != '\0') {
          item->glyph[0] = '\0';
        }
        if (!is_zero_v3(item->color)) {
          zero_v3(item->color);
        }
      }
    }

    if (item->default_display_name[0] != '\0') {
      defaults.display_name = item->default_display_name;
    }
    else if (item->display_name[0] != '\0') {
      defaults.display_name = item->display_name;
    }

    break;
  }

  return defaults;
}

static CategoryGlyphItem *category_tab_reset_override_ensure(wmWindowManager *wm, const char *category)
{
  for (CategoryGlyphItem *it = static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first); it;
       it = static_cast<CategoryGlyphItem *>(it->next))
  {
    if (STREQ(it->category, category)) {
      return it;
    }
  }

  CategoryGlyphItem *item = MEM_new<CategoryGlyphItem>(__func__);
  STRNCPY(item->category, category);
  item->glyph[0] = '\0';
  item->display_name[0] = '\0';
  zero_v3(item->color);
  item->tags[0] = '\0';
  item->icon_key[0] = '\0';
  item->icon_path[0] = '\0';
  item->icon_provider[0] = '\0';
  item->icon_source = ui::CATEGORY_TAB_ICON_SOURCE_AUTO;
  item->glyph_mode = ui::CATEGORY_TAB_GLYPH_MODE_AUTO;
  BLI_addtail(&wm->category_glyph_overrides, item);
  return item;
}

static wmOperatorStatus category_tab_reset_exec(bContext *C, wmOperator *op)
{
  char category[64];
  RNA_string_get(op->ptr, "category", category);

  /* Read checkbox flags for selective reset */
  const bool reset_name = RNA_boolean_get(op->ptr, "reset_name");
  const bool reset_glyph = RNA_boolean_get(op->ptr, "reset_glyph");
  const bool reset_color = RNA_boolean_get(op->ptr, "reset_color");
  const bool reset_tag = RNA_boolean_get(op->ptr, "reset_tag");

  wmWindowManager *wm = CTX_wm_manager(C);
  ScrArea *area = CTX_wm_area(C);
  const CategoryTabResetDefaults defaults = compute_reset_defaults(wm, category, reset_glyph);

  wmOperator *const dialog_op = category_tab_current_dialog_op;
  category_tab_reset_apply_to_operator(C,
                                       dialog_op,
                                       category,
                                       defaults.display_name,
                                       defaults.glyph,
                                       defaults.color,
                                       reset_name,
                                       reset_glyph,
                                       reset_color);

  /* Keep reset popup operator props in sync with the dialog operator props. */
  if (op != dialog_op) {
    category_tab_reset_apply_to_operator(C,
                                         op,
                                         category,
                                         defaults.display_name,
                                         defaults.glyph,
                                         defaults.color,
                                         reset_name,
                                         reset_glyph,
                                         reset_color);
  }

  /* Trigger live update to refresh preview and override. */
  if (dialog_op && (reset_name || reset_glyph || reset_color)) {
    category_tab_edit_live_update_cb(C, dialog_op, 0);
  }

  /* Reset tags: set empty tags in WM override.
   * This updates the UI immediately. If user clicks Cancel, original tags will be restored. */
  if (reset_tag) {
    CategoryGlyphItem *reset_item = category_tab_reset_override_ensure(wm, category);
    /* Clear tags in WM override - this updates UI to show no tags selected */
    reset_item->tags[0] = '\0';

#ifdef WITH_PYTHON
    /* Also clear tags in Python _glyph_cache to ensure consistency */
    /* When user clicks a tag button after Reset, toggle_category_tag_no_save
     * reads from _glyph_cache, so it must be in sync with WM override */
    PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
    RNA_string_set(&wm_ptr, "category_tab_save_category", category);

    const char *imports[] = {"bpy", nullptr};
    char reset_tags_cmd[512];
    BLI_snprintf(reset_tags_cmd,
                 sizeof(reset_tags_cmd),
                 "from bl_ui.space_userpref import set_category_tags\n"
                 "import bpy\n"
                 "wm = bpy.context.window_manager\n"
                 "category = wm.category_tab_save_category\n"
                 "wm.category_tab_save_category = ''\n"
                 "if category:\n"
                 "    set_category_tags(category, [], auto_save=False)\n",
                 category);

    BPY_run_string_exec(C, imports, reset_tags_cmd);
#endif
  }

  category_tab_reset_tag_redraw(C, wm, area);

  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_reset(wmOperatorType *ot)
{
  ot->name = "Reset Category Tab";
  ot->idname = "SCREEN_OT_category_tab_reset";
  ot->description = "Reset category tab to default";

  ot->invoke = category_tab_reset_invoke;
  ot->exec = category_tab_reset_exec;
  ot->poll = category_tab_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Hidden properties for data passing (not shown in popup UI) */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "category", nullptr, 64, "Category", "Category identifier");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(ot->srna, "display_name", nullptr, 32, "Display Name", "Reset display name");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(ot->srna, "glyph", nullptr, 8, "Glyph", "Reset glyph");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(ot->srna, "glyph_search", nullptr, 64, "Search", "Reset search");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float_color(ot->srna, "color", 3, nullptr, 0.0f, 1.0f, "Color", "Reset color", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);

  /* Checkbox options for selective reset (all default to true) - these are shown in popup */
  RNA_def_boolean(ot->srna, "reset_name", true, "Name", "Reset category name");
  RNA_def_boolean(ot->srna, "reset_glyph", true, "Glyph", "Reset glyph");
  RNA_def_boolean(ot->srna, "reset_color", true, "Color", "Reset color");
  RNA_def_boolean(ot->srna, "reset_tag", true, "Tag", "Reset tags");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Paste Glyph Operator
 * \{ */

static wmOperatorStatus category_tab_paste_glyph_exec(bContext *C, wmOperator *op)
{
  /* Get text from clipboard */
  int clipboard_len;
  char *clipboard_text = WM_clipboard_text_get(false, true, &clipboard_len);

  if (!clipboard_text || !clipboard_text[0]) {
    if (clipboard_text) {
      MEM_delete(clipboard_text);
    }
    BKE_report(op->reports, RPT_WARNING, "Clipboard is empty");
    return OPERATOR_CANCELLED;
  }

  /* Validate clipboard content as hex code */
  const char *hex_start = clipboard_text;

  /* Skip optional "0x" or "0X" prefix */
  if (clipboard_text[0] == '0' && (clipboard_text[1] == 'x' || clipboard_text[1] == 'X')) {
    hex_start = clipboard_text + 2;
  }

  /* Trim trailing whitespace/newlines */
  size_t hex_len = strlen(hex_start);
  while (hex_len > 0 && (hex_start[hex_len - 1] == ' ' || hex_start[hex_len - 1] == '\t' ||
                         hex_start[hex_len - 1] == '\r' || hex_start[hex_len - 1] == '\n'))
  {
    hex_len--;
  }

  bool valid = true;

  /* Check length (exactly 4 hex digits) */
  if (hex_len != 4) {
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
    MEM_delete(clipboard_text);
    BKE_report(op->reports, RPT_ERROR, "Clipboard must contain exactly 4 hex digits (e.g., f1c8)");
    return OPERATOR_CANCELLED;
  }

  /* Copy exactly 4 hex digits to clean buffer */
  char hex_clean[5];
  memcpy(hex_clean, hex_start, 4);
  hex_clean[4] = '\0';

  /* Validate Unicode codepoint range */
  uint val = strtoul(hex_clean, nullptr, 16);

  if (val < 32 || val > 0x10FFFF) {
    MEM_delete(clipboard_text);
    BKE_report(op->reports, RPT_ERROR, "Invalid Unicode codepoint (must be 0020-10FFFF)");
    return OPERATOR_CANCELLED;
  }

  /* Get category from operator properties */
  char category[64];
  RNA_string_get(op->ptr, "category", category);

  /* Set glyph in operator properties */
  RNA_string_set(op->ptr, "glyph", hex_clean);

  /* Update the dialog operator if it exists */
  if (category_tab_current_dialog_op) {
    RNA_string_set(category_tab_current_dialog_op->ptr, "glyph", hex_clean);
  }

  MEM_delete(clipboard_text);

  /* Trigger live update */
  if (category_tab_current_dialog_op) {
    wmOperator *dialog_op = category_tab_current_dialog_op;

    /* Call live update callback */
    category_tab_edit_live_update_cb(C, dialog_op, 0);
  }

  WM_main_add_notifier(NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_paste_glyph(wmOperatorType *ot)
{
  ot->name = "Paste Glyph Code";
  ot->idname = "SCREEN_OT_category_tab_paste_glyph";
  ot->description = "Paste glyph hex code from clipboard (Ctrl+V)";

  ot->exec = category_tab_paste_glyph_exec;
  ot->poll = category_tab_edit_poll;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "category", nullptr, 64, "Category", "Category identifier");
  RNA_def_string(ot->srna, "glyph", nullptr, 16, "Glyph", "Hex codepoint");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Pick Custom Icon Operator
 * \{ */

static bool category_tab_pick_custom_icon_poll(bContext *C)
{
  if (!category_tab_current_dialog_op || !category_tab_popup_block) {
    return false;
  }
  return category_tab_edit_poll(C);
}

static bool category_tab_icon_filepath_is_supported_image(const char *filepath)
{
  return (filepath && filepath[0] != '\0' &&
          BLI_path_extension_check_n(filepath,
                                     ".png",
                                     ".jpg",
                                     ".jpeg",
                                     ".bmp",
                                     ".tif",
                                     ".tiff",
                                     nullptr));
}

static wmOperatorStatus category_tab_pick_custom_icon_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent * /*event*/)
{
  if (category_tab_current_dialog_op == nullptr) {
    return OPERATOR_CANCELLED;
  }

  char current_icon_path[1024] = "";
  RNA_string_get(category_tab_current_dialog_op->ptr, "icon_path", current_icon_path);
  if (current_icon_path[0] != '\0') {
    RNA_string_set(op->ptr, "filepath", current_icon_path);
  }

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus category_tab_pick_custom_icon_exec(bContext *C, wmOperator *op)
{
  if (category_tab_current_dialog_op == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Edit Category Tab dialog is not active");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX] = "";
  RNA_string_get(op->ptr, "filepath", filepath);

  if (filepath[0] == '\0') {
    BKE_report(op->reports, RPT_WARNING, "No file selected");
    return OPERATOR_CANCELLED;
  }

  if (!category_tab_icon_filepath_is_supported_image(filepath)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Only static image files are supported: png, jpg, jpeg, webp, bmp, tif, tiff");
    return OPERATOR_CANCELLED;
  }

  wmOperator *dialog_op = category_tab_current_dialog_op;
  RNA_enum_set(dialog_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_CUSTOM_ICON);
  RNA_enum_set(dialog_op->ptr, "custom_icon_mode_ui", CATEGORY_TAB_CUSTOM_ICON_MODE_CUSTOM);
  RNA_string_set(dialog_op->ptr, "icon_path", filepath);
  RNA_string_set(dialog_op->ptr, "icon_key", "");
  RNA_string_set(dialog_op->ptr, "icon_provider", "");

  /* Force refresh preview cache for the selected path to avoid stale thumbnail when file changed. */
  BKE_previewimg_cached_release(filepath);

  category_tab_edit_live_update_cb(C, dialog_op, 0);
  WM_main_add_notifier(NC_WINDOW, nullptr);

  BKE_report(op->reports, RPT_INFO, "Custom icon file selected");
  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_pick_custom_icon(wmOperatorType *ot)
{
  ot->name = "Pick Custom Icon File";
  ot->idname = "SCREEN_OT_category_tab_pick_custom_icon";
  ot->description = "Choose a custom icon image file for category tab";

  ot->invoke = category_tab_pick_custom_icon_invoke;
  ot->exec = category_tab_pick_custom_icon_exec;
  ot->poll = category_tab_pick_custom_icon_poll;

  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_IMAGE,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "filter_glob",
                                     "*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.tif;*.tiff",
                                     0,
                                     "Extension Filter",
                                     "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Reload Custom Icon Operator
 * \{ */

static bool category_tab_reload_custom_icon_poll(bContext *C)
{
  if (!category_tab_current_dialog_op || !category_tab_popup_block) {
    return false;
  }
  return category_tab_edit_poll(C);
}

static wmOperatorStatus category_tab_reload_custom_icon_exec(bContext *C, wmOperator *op)
{
  if (category_tab_current_dialog_op == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Edit Category Tab dialog is not active");
    return OPERATOR_CANCELLED;
  }

  char icon_path[1024] = "";
  RNA_string_get(category_tab_current_dialog_op->ptr, "icon_path", icon_path);
  if (icon_path[0] == '\0') {
    BKE_report(op->reports, RPT_WARNING, "No custom icon path set");
    return OPERATOR_CANCELLED;
  }
  if (!BLI_exists(icon_path)) {
    BKE_report(op->reports, RPT_ERROR, "Custom icon file not found");
    return OPERATOR_CANCELLED;
  }
  if (!category_tab_icon_filepath_is_supported_image(icon_path)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Only static image files are supported: png, jpg, jpeg, webp, bmp, tif, tiff");
    return OPERATOR_CANCELLED;
  }

  BKE_previewimg_cached_release(icon_path);

  wmOperator *dialog_op = category_tab_current_dialog_op;
  category_tab_edit_live_update_cb(C, dialog_op, 0);
  WM_main_add_notifier(NC_WINDOW, nullptr);

  BKE_report(op->reports, RPT_INFO, "Custom icon reloaded");
  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_reload_custom_icon(wmOperatorType *ot)
{
  ot->name = "Reload Custom Icon File";
  ot->idname = "SCREEN_OT_category_tab_reload_custom_icon";
  ot->description = "Reload custom icon from file and refresh preview";

  ot->exec = category_tab_reload_custom_icon_exec;
  ot->poll = category_tab_reload_custom_icon_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Edit Dialog Cancel Operator
 * \{ */

static wmOperatorStatus category_tab_edit_dialog_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  if (!category_tab_popup_block || !category_tab_current_dialog_op) {
    return OPERATOR_CANCELLED;
  }

  wmOperator *dialog_op = category_tab_current_dialog_op;
  ui::Block *block = category_tab_popup_block;
  wmWindow *win = CTX_wm_window(C);

  /* Trigger cancel callback to restore values (also shows report) */
  category_tab_edit_popup_cancel_cb(C, dialog_op);

  /* Close popup */
  ui::popup_menu_retval_set(block, ui::RETURN_CANCEL, true);
  ui::popup_block_close(C, win, block);

  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_edit_dialog_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Category Tab Edit";
  ot->idname = "SCREEN_OT_category_tab_edit_dialog_cancel";
  ot->description = "Cancel editing category tab";

  ot->exec = category_tab_edit_dialog_cancel_exec;
  ot->poll = category_tab_edit_poll;

  ot->flag = OPTYPE_REGISTER; /* Show reports in status bar */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Edit Dialog Save Operator
 * \{ */

static wmOperatorStatus category_tab_edit_dialog_save_exec(bContext *C, wmOperator *op)
{
  /* Live preview already updated the override. We just need to close the popup. */
  if (!category_tab_popup_block) {
    return OPERATOR_CANCELLED;
  }

  /* Read category before exec, because exec clears category_tab_current_dialog_op. */
  char category[64] = "";
  if (category_tab_current_dialog_op) {
    RNA_string_get(category_tab_current_dialog_op->ptr, "category", category);
  }

  /* Persist current dialog values (including icon fields) to JSON using the same
   * code-path as the edit dialog exec callback. */
  if (category_tab_current_dialog_op) {
    category_tab_edit_dialog_exec(C, category_tab_current_dialog_op);
  }

  if (category[0] == '\0') {
    return OPERATOR_CANCELLED;
  }

#ifdef WITH_PYTHON
  /* Store category in WM property (UTF-8 safe via RNA) */
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm) {
    PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
    RNA_string_set(&wm_ptr, "category_tab_save_category", category);
  }

  const char *imports[] = {"bpy", nullptr};
  const char *save_cmd =
      "from bl_ui.space_userpref import update_category_tags_in_wm, get_category_tags, sync_wm_to_glyph_cache\n"
      "import bpy\n"
      "wm = bpy.context.window_manager\n"
      "category = wm.category_tab_save_category\n"
      "wm.category_tab_save_category = ''\n"
      "print(f'[GLYPH SAVE PY] Category from WM property: {category}')\n"
      "if category:\n"
      "    tags = get_category_tags(category)\n"
      "    print(f'[GLYPH SAVE PY] Tags in _glyph_cache for {category}: {tags}')\n"
      "    update_category_tags_in_wm(category)\n"
      "    print(f'[GLYPH SAVE PY] Tags synced to WM override')\n"
      "    result = sync_wm_to_glyph_cache()\n"
      "    print(f'[GLYPH SAVE PY] sync_wm_to_glyph_cache returned: {result}')\n"
      "else:\n"
      "    print('[GLYPH SAVE PY] ERROR: No category found in WM property')\n";

  BPY_run_string_exec(C, imports, save_cmd);
#else
  (void)C; /* Unused when Python is disabled */
#endif

  ui::popup_menu_retval_set(category_tab_popup_block, ui::RETURN_OK, true);

  wmWindow *win = CTX_wm_window(C);
  ui::popup_block_close(C, win, category_tab_popup_block);

  BKE_report(op->reports, RPT_INFO, "Category tab settings saved");

  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_edit_dialog_save(wmOperatorType *ot)
{
  ot->name = "Save Category Tab Edit";
  ot->idname = "SCREEN_OT_category_tab_edit_dialog_save";
  ot->description = "Save category tab changes";

  ot->exec = category_tab_edit_dialog_save_exec;
  ot->poll = category_tab_edit_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Color Preset Operator
 * \{ */

static wmOperatorStatus category_tab_color_preset_exec(bContext *C, wmOperator *op)
{
  /* Get preset value from operator property */
  const int preset = RNA_int_get(op->ptr, "preset");

  /* Get RGB color from preset */
  float color[3];
  category_tab_color_preset_to_rgb(preset, color);

  /* Find target operator - the active operator in the popup block */
  wmOperator *target_op = ui::context_active_operator_get(C);

  if (!target_op) {
    return OPERATOR_CANCELLED;
  }

  /* Set the color in the target operator */
  RNA_float_set_array(target_op->ptr, "color", color);

  /* Trigger live update to refresh preview */
  if (STREQ(target_op->idname, "SCREEN_OT_category_tab_edit_dialog")) {
    category_tab_edit_live_update_cb(C, target_op, 0);
  }
  /* Для других операторов (например, Create Tag) обновление произойдет автоматически через RNA */

  return OPERATOR_FINISHED;
}

static void SCREEN_OT_category_tab_color_preset(wmOperatorType *ot)
{
  ot->name = "Set Category Tab Color Preset";
  ot->idname = "SCREEN_OT_category_tab_color_preset";
  ot->description = "Set category tab color from preset";

  ot->exec = category_tab_color_preset_exec;
  /* Works when there's an active operator (category edit or tag create) */
  ot->poll = [](bContext *C) {
    return ui::context_active_operator_get(C) != nullptr;
  };

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_int(ot->srna, "preset", -1, -1, 7, "Preset", "Color preset value (-1 for NONE, 0-7 for colors)", -1, 7);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tag Color Preset Operator
 * \{ */

static wmOperatorStatus tag_color_preset_exec(bContext *C, wmOperator *op)
{
  /* Get preset value and convert to RGB */
  const int preset = RNA_int_get(op->ptr, "preset");
  float color[3];
  category_tab_color_preset_to_rgb(preset, color);

  /* Get the active button - it contains RNA data set by uiTemplateColorGlyphPresets */
  ui::Button *active_but = context_active_but_get_respect_popup(C);

  PointerRNA ptr;
  PropertyRNA *prop = nullptr;

  /* PRIMARY METHOD: Use RNA data from the button (set by uiTemplateColorGlyphPresets)
   * This works for ANY RNA pointer - existing tags in WM, operator properties, etc.
   */
  if (active_but && active_but->rnaprop != nullptr) {
    ptr = active_but->rnapoin;
    prop = active_but->rnaprop;
  }
  else {
    /* FALLBACK: Try to find the tag in category_tags collection by name (for backward compatibility) */

    char tag_name[64];
    char propname[64];
    RNA_string_get(op->ptr, "tag_name", tag_name);
    RNA_string_get(op->ptr, "propname", propname);

    /* Get Window Manager */
    wmWindowManager *wm = CTX_wm_manager(C);

    /* Try to find the tag in category_tags collection (for existing tags) */
    CategoryTagDef *tag = nullptr;
    if (tag_name[0] != '\0') {
      for (CategoryTagDef *item = static_cast<CategoryTagDef *>(wm->category_tags.first);
           item;
           item = static_cast<CategoryTagDef *>(item->next))
      {
        if (STREQ(item->name, tag_name)) {
          tag = item;
          break;
        }
      }
    }

    if (tag) {
      /* Existing tag - create PointerRNA to the tag in WM */
      ptr = RNA_pointer_create_discrete(&wm->id, RNA_CategoryTagDef, tag);
      prop = RNA_struct_find_property(&ptr, propname);
    }
    else {
      /* No button RNA data and tag not found in WM */
      return OPERATOR_CANCELLED;
    }
  }

  if (!prop) {
    return OPERATOR_CANCELLED;
  }

  /* Verify property type (must be float color array) */
  if (RNA_property_type(prop) != PROP_FLOAT) {
    return OPERATOR_CANCELLED;
  }

  /* Set the color property */
  RNA_property_float_set_array(&ptr, prop, color);
  RNA_property_update(C, &ptr, prop);
  return OPERATOR_FINISHED;
}

static void WM_OT_tag_color_preset(wmOperatorType *ot)
{
  ot->name = "Set Tag Color Preset";
  ot->idname = "WM_OT_tag_color_preset";
  ot->description = "Set tag color from preset";

  ot->exec = tag_color_preset_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_string(ot->srna, "tag_name", nullptr, 64, "Tag Name", "Name of the tag to set color for");
  RNA_def_string(ot->srna, "propname", nullptr, 64, "Property Name", "Name of the color property");
  RNA_def_int(ot->srna, "preset", -1, -1, 7, "Preset", "Color preset value (-1 for NONE, 0-7 for colors)", -1, 7);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Edit Dialog Operator
 * \{ */

static void SCREEN_OT_category_tab_edit_dialog(wmOperatorType *ot)
{
  ot->name = "Edit Category Tab";
  ot->idname = "SCREEN_OT_category_tab_edit_dialog";
  ot->description = "Edit category tab name, glyph and color";

  ot->invoke = category_tab_edit_dialog_invoke;
  ot->exec = category_tab_edit_dialog_exec;
  ot->poll = category_tab_edit_poll;
  ot->ui = nullptr;  /* Layout is handled in block_create */

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna, "category", nullptr, 64, "Category", "Category identifier");
  RNA_def_string(ot->srna, "display_name", nullptr, 32, "Display Name", "Custom display name");
  RNA_def_string(ot->srna, "glyph", nullptr, 16, "Glyph Code", "Hex codepoint (e.g., e5d2)");
  RNA_def_string(ot->srna, "glyph_search", nullptr, 64, "Search", "Search glyphs");
  RNA_def_string(ot->srna, "icon_search", nullptr, 64, "Icon Search", "Search built-in icons");

  RNA_def_enum(ot->srna,
               "display_mode_ui",
               rna_enum_category_tab_edit_display_mode_items,
               CATEGORY_TAB_EDIT_MODE_GLYPH,
               "Display Mode",
               "UI mode for category display editor");
  RNA_def_enum(ot->srna,
               "custom_icon_mode_ui",
               rna_enum_category_tab_custom_icon_mode_items,
               CATEGORY_TAB_CUSTOM_ICON_MODE_BLENDER,
               "Custom Icon Mode",
               "Choose between Blender icon picker and external custom icon path");

  RNA_def_enum(ot->srna,
               "icon_source",
               rna_enum_category_tab_icon_source_items,
               ui::CATEGORY_TAB_ICON_SOURCE_AUTO,
               "Icon Source",
               "Icon source mode for category tab content (Stage 1 wiring)");
  RNA_def_string(ot->srna,
                 "icon_key",
                 nullptr,
                 128,
                 "Icon Key",
                 "Stable icon key for built-in/manual icon resolver");
  RNA_def_string(ot->srna,
                 "icon_path",
                 nullptr,
                 1024,
                 "Icon Path",
                 "Optional manual icon path (future resolver input)");
  RNA_def_string(ot->srna,
                 "icon_provider",
                 nullptr,
                 128,
                 "Icon Provider",
                 "Stable icon provider key/id");

  PropertyRNA *prop = RNA_def_float_color(
      ot->srna, "color", 3, nullptr, 0.0f, 1.0f, "Color", "Glyph color", 0.0f, 1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR_GAMMA);

  /* Original values for cancel functionality */
  RNA_def_string(
      ot->srna, "original_display_name", nullptr, 32, "Original Display Name", "");
  RNA_def_string(ot->srna, "original_glyph", nullptr, 16, "Original Glyph", "");
  RNA_def_enum(ot->srna,
               "original_icon_source",
               rna_enum_category_tab_icon_source_items,
               ui::CATEGORY_TAB_ICON_SOURCE_AUTO,
               "Original Icon Source",
               "Original icon source value for cancel semantics");
  RNA_def_enum(ot->srna,
               "original_glyph_mode",
               rna_enum_category_tab_glyph_mode_items,
               ui::CATEGORY_TAB_GLYPH_MODE_AUTO,
               "Original Glyph Mode",
               "Original glyph mode value for cancel semantics");
  RNA_def_string(ot->srna, "original_icon_key", nullptr, 128, "Original Icon Key", "");
  RNA_def_string(ot->srna, "original_icon_path", nullptr, 1024, "Original Icon Path", "");
  RNA_def_string(ot->srna, "original_icon_provider", nullptr, 128, "Original Icon Provider", "");
  prop = RNA_def_float_color(
      ot->srna, "original_color", 3, nullptr, 0.0f, 1.0f, "Original Color", "", 0.0f, 1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR_GAMMA);
  RNA_def_boolean(ot->srna, "original_has_override", false, "Original Has Override", "");
  RNA_def_string(
      ot->srna, "original_tags", nullptr, 256, "Original Tags", "Semicolon-separated tag names");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Registration
 * \{ */

void ED_operatortypes_screen_category_tabs()
{
  WM_operatortype_append(SCREEN_OT_category_tab_edit_dialog);
  WM_operatortype_append(SCREEN_OT_category_tab_pick_custom_icon);
  WM_operatortype_append(SCREEN_OT_category_tab_reload_custom_icon);
  WM_operatortype_append(SCREEN_OT_category_tab_edit_dialog_cancel);
  WM_operatortype_append(SCREEN_OT_category_tab_edit_dialog_save);
  WM_operatortype_append(SCREEN_OT_category_tab_color_preset);
  WM_operatortype_append(WM_OT_tag_color_preset);
  WM_operatortype_append(SCREEN_OT_category_tab_reset);
  WM_operatortype_append(SCREEN_OT_category_tab_paste_glyph);
  WM_operatortype_append(ui::WM_OT_glyph_picker_grid);
}

/** \} */

}  // namespace blender
