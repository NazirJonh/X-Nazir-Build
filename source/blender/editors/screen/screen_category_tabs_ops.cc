/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
#include "BKE_global.hh"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#ifdef WITH_PYTHON
#  include "BPY_extern.hh"
#  include "BPY_extern_run.hh"
#endif

#include "ED_screen.hh"

/* Debug flag for category tab operations - set to 0 to disable debug output.
 * Keep this value in sync with CATEGORY_TAB_DEBUG_ENABLED in interface_tab_categories_edit.cc;
 * a shared header is the proper single source of truth (follow-up). */
#ifndef CATEGORY_TAB_DEBUG_ENABLED
#  define CATEGORY_TAB_DEBUG_ENABLED 1
#endif

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
#include "../interface/interface_category_py_bridge.hh"

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
using ui::category_tab_try_auto_detect_extension_icon;
using ui::category_is_reserved;

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

struct CategoryTabResetDefaults {
  const char *glyph = nullptr;
  const char *display_name = nullptr;
  float color[3] = {0.0f, 0.0f, 0.0f};
  /* Default icon info from mapping (nullptr/empty = no default icon). */
  const char *icon_path = nullptr;
  const char *icon_key = nullptr;
  const char *icon_provider = nullptr;
  int icon_source = ui::CATEGORY_TAB_ICON_SOURCE_AUTO; /* AUTO = 0 */
  bool has_default_icon = false;
};

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
                                                   const CategoryTabResetDefaults &defaults,
                                                   const bool reset_name,
                                                   const bool reset_glyph,
                                                   const bool reset_color,
                                                   const bool reset_icon,
                                                   const wmWindowManager *wm)
{
  if (target_op == nullptr) {
    return;
  }

  if (reset_name) {
    if (defaults.display_name != nullptr && defaults.display_name[0] != '\0') {
      RNA_string_set(target_op->ptr, "display_name", defaults.display_name);
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
    /* Reset glyph character (hex code). */
    if (defaults.glyph != nullptr) {
      char glyph_hex[16];
      utf8_to_hex_codepoint(defaults.glyph, glyph_hex, sizeof(glyph_hex));
#if CATEGORY_TAB_DEBUG_ENABLED
      printf("[RESET APPLY] category='%s', default_glyph='%s', glyph_hex='%s'\n",
             category, defaults.glyph, glyph_hex);
#endif
      RNA_string_set(target_op->ptr, "glyph", glyph_hex);
    }
    else {
#if CATEGORY_TAB_DEBUG_ENABLED
      printf("[RESET APPLY] category='%s', default_glyph=nullptr, setting glyph=''\n", category);
#endif
      RNA_string_set(target_op->ptr, "glyph", "");
    }

    if (reset_icon) {
      /* Clear icon fields first - user's manually-assigned custom icon is
       * an override, not a default, and must be removed on reset.
       * Then restore the mapping-level default icon only if it is an extension_auto
       * icon (i.e. a built-in icon provided automatically by an extension). */
      RNA_string_set(target_op->ptr, "icon_path", "");
      RNA_string_set(target_op->ptr, "icon_key", "");
      RNA_string_set(target_op->ptr, "icon_provider", "");
      RNA_enum_set(target_op->ptr, "icon_source", ui::CATEGORY_TAB_ICON_SOURCE_AUTO);

      PropertyRNA *display_mode_prop = RNA_struct_find_property(target_op->ptr,
                                                                "display_mode_ui");
      if (display_mode_prop != nullptr) {
        if (defaults.has_default_icon) {
          /* Restore built-in extension icon and show Icon Custom panel. */
          RNA_string_set(target_op->ptr,
                         "icon_path",
                         defaults.icon_path ? defaults.icon_path : "");
          RNA_string_set(target_op->ptr,
                         "icon_provider",
                         defaults.icon_provider ? defaults.icon_provider : "");
          RNA_string_set(target_op->ptr,
                         "icon_key",
                         defaults.icon_key ? defaults.icon_key : "");
          RNA_enum_set(target_op->ptr, "icon_source", defaults.icon_source);
          RNA_enum_set(target_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_CUSTOM_ICON);
          RNA_enum_set(target_op->ptr, "custom_icon_mode_ui", CATEGORY_TAB_CUSTOM_ICON_MODE_CUSTOM);
        }
        else {
          /* No built-in default icon.
           * For text categories, reset must keep first-letter behavior after Save,
           * otherwise save path may persist glyph_mode=auto and UI can fall back to
           * tag glyph.
           * 
           * EXCEPTION: For reserved categories (from DEFAULT_CATEGORY_GLYPHS in Python),
           * always use Glyph mode (AUTO) instead of First Letter, because reserved
           * categories always have a standard glyph assigned and should not show
           * first-letter behavior. */
          /* Reserved categories (from DEFAULT_CATEGORY_GLYPHS) always have a standard
           * glyph assigned. On reset, show the Glyph tab so the user sees the glyph,
           * not the First Letter text mode. */
          if (is_single_glyph_str(category) ||
              (category_is_reserved(wm, category) && defaults.glyph != nullptr))
          {
            RNA_enum_set(target_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_GLYPH);
          }
          else {
            RNA_enum_set(target_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_TEXT);
            PropertyRNA *glyph_mode_prop = RNA_struct_find_property(target_op->ptr,
                                                                     "glyph_mode");
            if (glyph_mode_prop != nullptr) {
              RNA_enum_set(target_op->ptr,
                           "glyph_mode",
                           ui::CATEGORY_TAB_GLYPH_MODE_FIRST_LETTER);
            }
          }
        }
      }
    }
  }

  if (reset_color) {
    RNA_float_set_array(target_op->ptr, "color", defaults.color);
  }

  RNA_string_set(target_op->ptr, "glyph_search", "");
}

static void category_tab_reset_tag_redraw(bContext *C, wmWindowManager *wm, ScrArea *area)
{
  /* Force redraw of the popup to update glyph preview/tag UI.
   * NOTE: Only use redraw_no_rebuild here. ED_region_tag_refresh_ui would
   * destroy and recreate the popup block, which triggers the cancel callback
   * and restores the original snapshot data — undoing the reset. */
  if (category_tab_popup_block) {
    ARegion *region = CTX_wm_region(C);
    if (region) {
      ED_region_tag_redraw_no_rebuild(region);
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


/**
 * Result of locating the glyph-mapping entries relevant to a reset.
 *
 * `apply_item` is the entry the reset mutates (space-specific if present, otherwise the first
 * match, otherwise the global entry); `global_item` is the space_type == -1 entry if any.
 */
struct CategoryMappingMatch {
  CategoryGlyphItem *apply_item = nullptr;
  CategoryGlyphItem *global_item = nullptr;
};

/** Locate the mapping entries used by #compute_reset_defaults. */
static CategoryMappingMatch find_reset_mapping_items(wmWindowManager *wm,
                                                     const char *category,
                                                     const int space_type)
{
  CategoryMappingMatch match;
  CategoryGlyphItem *target_item = nullptr;
  CategoryGlyphItem *first_match = nullptr;

  for (CategoryGlyphItem *item = static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
       item;
       item = static_cast<CategoryGlyphItem *>(item->next))
  {
    if (!STREQ(item->category, category)) {
      continue;
    }

    if (item->space_type == -1) {
      match.global_item = item;
    }
    if (space_type == item->space_type) {
      target_item = item;
    }
    if (!first_match) {
      first_match = item;
    }
    if (target_item && match.global_item) {
      break;
    }
  }

  /* Choose the entry we will mutate (apply reset to). */
  match.apply_item = target_item ? target_item : (first_match ? first_match : match.global_item);
  return match;
}

/**
 * Resolve and apply the default glyph/color during a reset.
 *
 * Mutates `apply_item` (and clears its color) and records the resulting default glyph in
 * `defaults`. Extracted from #compute_reset_defaults. The `default_source` is `global_item` when
 * it carries a glyph (or for single-glyph categories), otherwise `apply_item`.
 */
static void resolve_reset_glyph(CategoryGlyphItem *apply_item,
                                CategoryGlyphItem *global_item,
                                const char *category,
                                const bool reset_glyph,
                                CategoryTabResetDefaults &defaults)
{
  const bool is_glyph_only_category = is_single_glyph_str(category);
  const bool global_has_glyph = (global_item != nullptr) &&
                                (global_item->glyph[0] != '\0' || global_item->default_glyph[0] != '\0');
  CategoryGlyphItem *default_source = apply_item;
  if (global_has_glyph || (is_glyph_only_category && global_item)) {
    default_source = global_item;
  }

  if (reset_glyph) {
    /* Reset should always clear explicit first-letter mode in persisted mapping. */
    apply_item->glyph_mode = ui::CATEGORY_TAB_GLYPH_MODE_AUTO;
  }

  if (is_glyph_only_category) {
    if (default_source->default_glyph[0] != '\0') {
      defaults.glyph = default_source->default_glyph;
    }
    else if (default_source->glyph[0] != '\0') {
      defaults.glyph = default_source->glyph;
    }

    if (defaults.glyph) {
      STRNCPY(apply_item->glyph, defaults.glyph);
      if (apply_item->default_glyph[0] == '\0') {
        STRNCPY(apply_item->default_glyph, defaults.glyph);
      }
    }
    else {
      apply_item->glyph[0] = '\0';
    }
  }
  else {
    bool has_valid_default_glyph = false;
    if (default_source->default_glyph[0] != '\0') {
      has_valid_default_glyph = !ui::category_tab_glyph_is_fallback_letter(
          default_source->default_glyph, category);
    }

    if (has_valid_default_glyph) {
      defaults.glyph = default_source->default_glyph;
      if (apply_item->glyph[0] != '\0' && !STREQ(apply_item->glyph, default_source->default_glyph)) {
        STRNCPY(apply_item->glyph, default_source->default_glyph);
      }
      if (!is_zero_v3(apply_item->color)) {
        zero_v3(apply_item->color);
      }
    }
    else {
      defaults.glyph = nullptr;
      if (apply_item->glyph[0] != '\0') {
        apply_item->glyph[0] = '\0';
      }
      if (!is_zero_v3(apply_item->color)) {
        zero_v3(apply_item->color);
      }
    }
  }
}

static CategoryTabResetDefaults compute_reset_defaults(wmWindowManager *wm,
                                                       const char *category,
                                                       const bool reset_glyph,
                                                       const int space_type)
{
  CategoryTabResetDefaults defaults;

  if (!(wm && wm->category_glyph_mappings.first)) {
    return defaults;
  }

  const CategoryMappingMatch match = find_reset_mapping_items(wm, category, space_type);
  CategoryGlyphItem *apply_item = match.apply_item;
  CategoryGlyphItem *global_item = match.global_item;
  if (!apply_item) {
    return defaults;
  }

  const bool is_glyph_only_category = is_single_glyph_str(category);

  resolve_reset_glyph(apply_item, global_item, category, reset_glyph, defaults);

  if (apply_item->default_display_name[0] != '\0') {
    defaults.display_name = apply_item->default_display_name;
  }
  else if (apply_item->display_name[0] != '\0') {
    defaults.display_name = apply_item->display_name;
  }

  /* Collect default icon info from the global mapping item.
   * Only treat extension_auto icons as "defaults" that survive reset.
   * Manually-assigned custom icons (empty/missing provider) are user overrides
   * and must be cleared on reset. */
  if (global_item != nullptr) {
    const bool is_extension_auto_icon =
        (global_item->icon_provider[0] != '\0' &&
         STRPREFIX(global_item->icon_provider, "extension_auto"));
    if (is_extension_auto_icon && global_item->icon_path[0] != '\0') {
      defaults.icon_path = global_item->icon_path;
      defaults.icon_provider = global_item->icon_provider;
      defaults.icon_key = global_item->icon_key;
      defaults.icon_source = global_item->icon_source;
      defaults.has_default_icon = true;
    }
  }

  /* When resetting the glyph, also reset icon fields in the mapping item.
   * If no extension_auto default icon exists, clear the icon fields so that
   * the user's manually-assigned custom icon is removed from the mapping and,
   * consequently, from any subsequent JSON save triggered by "Save" after "Reset". */
  if (reset_glyph && apply_item != nullptr) {
    if (defaults.has_default_icon) {
      /* Keep the extension-auto default; update mapping to reflect it. */
      STRNCPY(apply_item->icon_path, defaults.icon_path ? defaults.icon_path : "");
      STRNCPY(apply_item->icon_provider, defaults.icon_provider ? defaults.icon_provider : "");
      STRNCPY(apply_item->icon_key, defaults.icon_key ? defaults.icon_key : "");
      apply_item->icon_source = defaults.icon_source;
    }
    else {
      /* No built-in default icon - clear icon fields from the mapping item. */
      apply_item->icon_path[0] = '\0';
      apply_item->icon_key[0] = '\0';
      apply_item->icon_provider[0] = '\0';
      apply_item->icon_source = ui::CATEGORY_TAB_ICON_SOURCE_AUTO;
    }
  }

  /* CRITICAL FIX: For text_only/glyph_text categories, also clear GLOBAL mappings
   * to prevent stale glyph data from appearing in "Categories using this tag" panel.
   * The issue: get_category_glyph_data uses GLOBAL fallback which finds old glyph
   * in GLOBAL mappings even after resetting space-specific entry. */
  if (reset_glyph && !is_glyph_only_category && global_item != nullptr) {
    /* Reset glyph to empty (will use first_letter fallback) */
    global_item->glyph[0] = '\0';
    /* Keep glyph_mode=AUTO so Cancel restores original icon/glyph correctly.
     * The first_letter display will be used at runtime when glyph is empty,
     * but we must not persist FIRST_LETTER mode or Cancel will restore wrong state. */
    global_item->glyph_mode = ui::CATEGORY_TAB_GLYPH_MODE_AUTO;
    /* Clear color */
    zero_v3(global_item->color);
    /* Clear icon data */
    global_item->icon_path[0] = '\0';
    global_item->icon_key[0] = '\0';
    global_item->icon_provider[0] = '\0';
    global_item->icon_source = ui::CATEGORY_TAB_ICON_SOURCE_OFF;
  }

  return defaults;
}

static CategoryGlyphItem *category_tab_reset_override_ensure(wmWindowManager *wm,
                                                              const char *category,
                                                              const int space_type)
{
  for (CategoryGlyphItem *it = static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first); it;
       it = static_cast<CategoryGlyphItem *>(it->next))
  {
    if (it->space_type == space_type && STREQ(it->category, category)) {
      return it;
    }
  }

  CategoryGlyphItem *item = MEM_new<CategoryGlyphItem>(__func__);
  STRNCPY(item->category, category);
  item->space_type = space_type;
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
#if CATEGORY_TAB_DEBUG_ENABLED
  printf("[RESET EXEC] === category_tab_reset_exec START ===\n");
#endif
  
  char category[64];
  RNA_string_get(op->ptr, "category", category);
#if CATEGORY_TAB_DEBUG_ENABLED
  printf("[RESET EXEC] category='%s'\n", category);
#endif

  /* Read checkbox flags for selective reset */
  const bool reset_name = RNA_boolean_get(op->ptr, "reset_name");
  const bool reset_glyph = RNA_boolean_get(op->ptr, "reset_glyph");
  const bool reset_color = RNA_boolean_get(op->ptr, "reset_color");
  const bool reset_tag = RNA_boolean_get(op->ptr, "reset_tag");
  const bool reset_icon = RNA_boolean_get(op->ptr, "reset_icon");
#if CATEGORY_TAB_DEBUG_ENABLED
  printf("[RESET EXEC] reset_name=%d, reset_glyph=%d, reset_color=%d, reset_tag=%d, reset_icon=%d\n",
         reset_name, reset_glyph, reset_color, reset_tag, reset_icon);
#endif

  wmWindowManager *wm = CTX_wm_manager(C);
  wmOperator *const dialog_op = category_tab_current_dialog_op;
  ScrArea *area = CTX_wm_area(C);
  int space_type = area ? area->spacetype : -1;
  if (space_type == -1 && dialog_op != nullptr && dialog_op->ptr != nullptr) {
    PropertyRNA *prop_space_type = RNA_struct_find_property(dialog_op->ptr, "original_space_type");
    if (prop_space_type != nullptr) {
      space_type = RNA_int_get(dialog_op->ptr, "original_space_type");
    }
  }
#if CATEGORY_TAB_DEBUG_ENABLED
  printf("[RESET EXEC] space_type=%d\n", space_type);
#endif
  
#if CATEGORY_TAB_DEBUG_ENABLED
  printf("[RESET EXEC] Calling compute_reset_defaults...\n");
#endif
  const CategoryTabResetDefaults defaults =
      compute_reset_defaults(wm, category, reset_glyph, space_type);
#if CATEGORY_TAB_DEBUG_ENABLED
  printf("[RESET EXEC] compute_reset_defaults returned\n");
#endif

  category_tab_reset_apply_to_operator(C,
                                       dialog_op,
                                       category,
                                       defaults,
                                       reset_name,
                                       reset_glyph,
                                       reset_color,
                                       reset_icon,
                                       wm);

  /* Keep reset popup operator props in sync with the dialog operator props. */
  if (op != dialog_op) {
    category_tab_reset_apply_to_operator(C,
                                         op,
                                         category,
                                         defaults,
                                         reset_name,
                                         reset_glyph,
                                         reset_color,
                                         reset_icon,
                                         wm);
  }

  /* When resetting the glyph/icon, also clear icon fields from the WM override so that
   * the live_update callback (called below) does not re-read stale icon data and
   * propagate it back into the override. Without this, the custom icon assigned by the
   * user keeps appearing in Preview Image and the tab even after clicking Reset.
   * Skip this entire block when reset_icon is false - user wants to preserve their icon. */
  if (reset_glyph && reset_icon) {
    CategoryGlyphItem *override_item = category_tab_reset_override_ensure(wm, category, space_type);
    if (defaults.has_default_icon) {
      /* Restore default extension icon in the WM override. */
      STRNCPY(override_item->icon_path, defaults.icon_path ? defaults.icon_path : "");
      STRNCPY(override_item->icon_provider, defaults.icon_provider ? defaults.icon_provider : "");
      STRNCPY(override_item->icon_key, defaults.icon_key ? defaults.icon_key : "");
      override_item->icon_source = defaults.icon_source;
    }
    else {
      /* No built-in default icon - clear all icon fields in the override. */
      override_item->icon_path[0] = '\0';
      override_item->icon_key[0] = '\0';
      override_item->icon_provider[0] = '\0';
      override_item->icon_source = ui::CATEGORY_TAB_ICON_SOURCE_AUTO;
    }
  }

  /* Edge case fallback: If no default extension icon was found in mappings,
   * try auto-detecting icon.png from the extension directory. This handles the
   * scenario where:
   * 1) icon.png was auto-detected at dialog invoke time but not yet saved to
   *    mappings (so compute_reset_defaults can't find it)
   * 2) User had manually overridden the extension icon with their own custom
   *    icon, and Reset should restore the original extension icon.png */
  if (reset_glyph && reset_icon && !defaults.has_default_icon && dialog_op != nullptr) {
    char detected_icon_path[1024] = "";
    char detected_icon_provider[128] = "";

    if (category_tab_try_auto_detect_extension_icon(
            C, category, detected_icon_path, detected_icon_provider))
    {
      /* Apply detected extension icon to dialog operator. */
      RNA_string_set(dialog_op->ptr, "icon_path", detected_icon_path);
      RNA_string_set(dialog_op->ptr, "icon_provider", detected_icon_provider);
      RNA_string_set(dialog_op->ptr, "icon_key", "");
      RNA_enum_set(dialog_op->ptr, "icon_source", 0); /* AUTO */
      PropertyRNA *dm_prop = RNA_struct_find_property(dialog_op->ptr, "display_mode_ui");
      if (dm_prop) {
        RNA_enum_set(dialog_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_CUSTOM_ICON);
        RNA_enum_set(dialog_op->ptr, "custom_icon_mode_ui", CATEGORY_TAB_CUSTOM_ICON_MODE_CUSTOM);
      }

      /* Also update WM override with detected icon. */
      CategoryGlyphItem *override_item = category_tab_reset_override_ensure(wm, category, -1);
      STRNCPY(override_item->icon_path, detected_icon_path);
      STRNCPY(override_item->icon_provider, detected_icon_provider);
      override_item->icon_key[0] = '\0';
      override_item->icon_source = 0; /* AUTO */
    }
  }

  /* Trigger live update to refresh preview and override. */
  if (dialog_op && (reset_name || reset_glyph || reset_color)) {
    category_tab_edit_live_update_cb(C, dialog_op, 0);
  }

#ifdef WITH_PYTHON
  /* CRITICAL FIX: Sync Python _glyph_cache with WM changes after Reset glyph.
   * The issue: C++ clears global_item->glyph in wm->category_glyph_mappings,
   * but Python _sync_wm_to_glyph_cache_impl skips empty glyphs (line 5939 condition:
   * "if item_glyph and ..."). This causes stale GLOBAL glyph to persist in
   * _glyph_cache and JSON, appearing in "Categories using this tag" panel.
   *
   * Solution: Call Python reset_category_to_defaults() which directly clears
   * GLOBAL entry in _glyph_cache, bypassing the sync issue.
   * Run when reset_icon is true - user wants to reset icon along with glyph,
   * and clearing Python cache removes stale icon data so auto-detected extension icon takes effect. */
  if (reset_glyph && reset_icon) {
    PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
    RNA_string_set(&wm_ptr, "category_tab_save_category", category);

    /* Reset glyph/icon to defaults via the centralized Python bridge. */
    ui::category_py_reset_to_defaults(C, space_type);
  }
#endif

  /* Reset tags: set empty tags in WM override.
   * This updates the UI immediately. If user clicks Cancel, original tags will be restored.
   *
   * Global-First: Always use GLOBAL space_type (-1) for override to prevent C++ lookup
   * from finding stale space-specific overrides first. */
  if (reset_tag) {
    const int global_space_type = -1;  // Always GLOBAL for consistency

    /* Remove all space-specific overrides for this category first */
    for (CategoryGlyphItem *old_item = static_cast<CategoryGlyphItem *>(wm->category_glyph_overrides.first);
         old_item != nullptr;)
    {
      CategoryGlyphItem *next_item = static_cast<CategoryGlyphItem *>(old_item->next);
      if (STREQ(old_item->category, category) && old_item->space_type != global_space_type) {
        BLI_remlink(&wm->category_glyph_overrides, old_item);
        MEM_delete(old_item);
      }
      old_item = next_item;
    }

    CategoryGlyphItem *reset_item = category_tab_reset_override_ensure(wm, category, global_space_type);
    /* Clear tags in WM override - this updates UI to show no tags selected */
    reset_item->tags[0] = '\0';

    /* Also clear tags in category_glyph_mappings so C++ lookup finds empty tags */
    for (CategoryGlyphItem *map_item = static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         map_item != nullptr; map_item = static_cast<CategoryGlyphItem *>(map_item->next))
    {
      if (STREQ(map_item->category, category)) {
        map_item->tags[0] = '\0';
        break;
      }
    }

#ifdef WITH_PYTHON
    /* Also clear tags in Python _glyph_cache to ensure consistency */
    /* When user clicks a tag button after Reset, toggle_category_tag_no_save
     * reads from _glyph_cache, so it must be in sync with WM override */
    PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
    RNA_string_set(&wm_ptr, "category_tab_save_category", category);

    /* Reset tags to empty via the centralized Python bridge. */
    ui::category_py_reset_tags(C, global_space_type);
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
  RNA_def_boolean(ot->srna, "reset_icon", true, "Icon", "Reset custom icon to default");

  /* Icon data properties for sync (hidden, not user-facing). */
  prop = RNA_def_string(ot->srna, "icon_path", nullptr, 1024, "Icon Path", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(ot->srna, "icon_key", nullptr, 128, "Icon Key", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(ot->srna, "icon_provider", nullptr, 128, "Icon Provider", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
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

/**
 * Resolve which operator the custom-icon file operators should write into.
 *
 * A non-empty `target_operator_ptr` is the tag dialogs' way of saying "write into me". The address
 * itself is only a marker and is never dereferenced: the live operator is looked up by
 * #ui::category_tag_dialog_operator_find, which walks the popup's UI block. An address cannot be
 * validated against `wm->runtime->operators` because an open dialog has not been registered there
 * yet, so that lookup would always miss.
 */
static wmOperator *category_tab_custom_icon_target_op_get(bContext *C, wmOperator *op)
{
  char target_op_ptr_str[64] = "";
  if (PropertyRNA *prop = RNA_struct_find_property(op->ptr, "target_operator_ptr")) {
    RNA_property_string_get(op->ptr, prop, target_op_ptr_str);
  }

  if (target_op_ptr_str[0] != '\0') {
    return ui::category_tag_dialog_operator_find(C);
  }

  return category_tab_current_dialog_op;
}

/** True when the resolved target is a tag dialog rather than the category tab dialog. */
static bool category_tab_custom_icon_target_is_tag(const wmOperator *target_op)
{
  return target_op != nullptr && target_op != category_tab_current_dialog_op;
}

/**
 * Resolve the tag named by the `target_tag` property, if any.
 *
 * This is the Tag Management panel's route: that panel is plain UI with no operator behind it, so
 * it names the tag it is editing instead of pointing at a dialog.
 */
static CategoryTagDef *category_tab_custom_icon_target_tag_get(bContext *C, wmOperator *op)
{
  char tag_name[64] = "";
  if (PropertyRNA *prop = RNA_struct_find_property(op->ptr, "target_tag")) {
    RNA_property_string_get(op->ptr, prop, tag_name);
  }
  if (tag_name[0] == '\0') {
    return nullptr;
  }

  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return nullptr;
  }
  for (CategoryTagDef *tag = static_cast<CategoryTagDef *>(wm->category_tags.first); tag;
       tag = tag->next)
  {
    if (STREQ(tag->name, tag_name)) {
      return tag;
    }
  }
  return nullptr;
}

/**
 * Write the custom icon fields into a tag and persist them.
 *
 * The write goes through RNA rather than straight into the struct so that
 * #rna_CategoryTagDef_update runs, which is what syncs the change back into the Python cache and
 * on to disk.
 */
static void category_tab_custom_icon_apply_to_tag(bContext *C,
                                                  CategoryTagDef *tag,
                                                  const char *filepath)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  PointerRNA tag_ptr = RNA_pointer_create_discrete(&wm->id, RNA_CategoryTagDef, tag);

  if (filepath != nullptr) {
    PropertyRNA *path_prop = RNA_struct_find_property(&tag_ptr, "icon_path");
    RNA_property_string_set(&tag_ptr, path_prop, filepath);

    PropertyRNA *key_prop = RNA_struct_find_property(&tag_ptr, "icon_key");
    RNA_property_string_set(&tag_ptr, key_prop, "");
  }

  PropertyRNA *source_prop = RNA_struct_find_property(&tag_ptr, "icon_source");
  RNA_property_int_set(&tag_ptr, source_prop, CATEGORY_TAG_ICON_SOURCE_CUSTOM_FILE);

  /* One update call is enough: the callback re-syncs the whole tag, not a single property. Its
   * NC_WM|ND_CATEGORY_GLYPHS notifier is also what invalidates the tag bar's icon cache, so no
   * explicit dirty call is needed here. */
  RNA_property_update(C, &tag_ptr, source_prop);
}

static bool category_tab_pick_custom_icon_poll(bContext *C)
{
  /* Deliberately not checking #category_tab_current_dialog_op: the tag dialogs are Python
   * operators that pass their own target instead, so the C++ dialog globals are empty for them.
   * The real target is resolved and validated in invoke/exec. */
  return CTX_wm_manager(C) != nullptr;
}

static bool category_tab_icon_filepath_is_supported_image(const char *filepath)
{
  return (filepath && filepath[0] != '\0' &&
          BLI_path_extension_check_n(filepath,
                                     ".png",
                                     ".jpg",
                                     ".jpeg",
                                     ".webp",
                                     ".bmp",
                                     ".tif",
                                     ".tiff",
                                     nullptr));
}

static wmOperatorStatus category_tab_pick_custom_icon_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent * /*event*/)
{
  const CategoryTagDef *target_tag = category_tab_custom_icon_target_tag_get(C, op);
  wmOperator *target_op = target_tag ? nullptr : category_tab_custom_icon_target_op_get(C, op);
  if (target_tag == nullptr && target_op == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No icon editor is active");
    return OPERATOR_CANCELLED;
  }

  if (U.category_tabs_custom_icon_dir[0] != '\0') {
    RNA_string_set(op->ptr, "directory", U.category_tabs_custom_icon_dir);
  }
  else {
    char current_icon_path[1024] = "";
    if (target_tag != nullptr) {
      STRNCPY(current_icon_path, target_tag->icon_path);
    }
    else if (PropertyRNA *prop = RNA_struct_find_property(target_op->ptr, "icon_path")) {
      RNA_property_string_get(target_op->ptr, prop, current_icon_path);
    }
    if (current_icon_path[0] != '\0') {
      RNA_string_set(op->ptr, "filepath", current_icon_path);
    }
  }

  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus category_tab_pick_custom_icon_exec(bContext *C, wmOperator *op)
{
  CategoryTagDef *target_tag = category_tab_custom_icon_target_tag_get(C, op);
  wmOperator *target_op = target_tag ? nullptr : category_tab_custom_icon_target_op_get(C, op);
  if (target_tag == nullptr && target_op == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No icon editor is active");
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

  /* Force refresh preview cache for the selected path to avoid stale thumbnail when file changed. */
  BKE_previewimg_cached_release(filepath);

  if (target_tag != nullptr) {
    category_tab_custom_icon_apply_to_tag(C, target_tag, filepath);
    WM_main_add_notifier(NC_WINDOW, nullptr);
    BKE_report(op->reports, RPT_INFO, "Custom icon file selected");
    return OPERATOR_FINISHED;
  }

  const bool is_tag = category_tab_custom_icon_target_is_tag(target_op);

  RNA_enum_set(target_op->ptr, "display_mode_ui", CATEGORY_TAB_EDIT_MODE_CUSTOM_ICON);
  /* Looked up rather than set blindly: not every dialog that owns an `icon_path` also offers the
   * Blender/Custom sub-mode toggle. */
  if (RNA_struct_find_property(target_op->ptr, "custom_icon_mode_ui") != nullptr) {
    RNA_enum_set(target_op->ptr, "custom_icon_mode_ui", CATEGORY_TAB_CUSTOM_ICON_MODE_CUSTOM);
  }
  RNA_string_set(target_op->ptr, "icon_path", filepath);
  RNA_string_set(target_op->ptr, "icon_key", "");
  if (is_tag) {
    /* Tags have no icon provider; their icon_source enum carries the custom-file value. */
    RNA_enum_set(target_op->ptr, "icon_source", CATEGORY_TAG_ICON_SOURCE_CUSTOM_FILE);
  }
  else {
    RNA_string_set(target_op->ptr, "icon_provider", "");
  }

  if (is_tag) {
    ui::tag_icon_live_update_cb(C, target_op, 0);
  }
  else {
    category_tab_edit_live_update_cb(C, target_op, 0);
  }
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
                                 WM_FILESEL_FILEPATH | WM_FILESEL_DIRECTORY,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "filter_glob",
                                     "*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.tif;*.tiff",
                                     0,
                                     "Extension Filter",
                                     "");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  PropertyRNA *target_prop = RNA_def_string(ot->srna,
                                            "target_operator_ptr",
                                            nullptr,
                                            64,
                                            "Target Operator Pointer",
                                            "Internal: address of the dialog operator to write into");
  RNA_def_property_flag(target_prop, PROP_HIDDEN);

  PropertyRNA *target_tag_prop = RNA_def_string(
      ot->srna,
      "target_tag",
      nullptr,
      64,
      "Target Tag",
      "Internal: name of the tag to write into, used when no dialog is open");
  RNA_def_property_flag(target_tag_prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tab Reload Custom Icon Operator
 * \{ */

static bool category_tab_reload_custom_icon_poll(bContext *C)
{
  /* See #category_tab_pick_custom_icon_poll for why the dialog globals are not checked here. */
  return CTX_wm_manager(C) != nullptr;
}

static wmOperatorStatus category_tab_reload_custom_icon_exec(bContext *C, wmOperator *op)
{
  CategoryTagDef *target_tag = category_tab_custom_icon_target_tag_get(C, op);
  wmOperator *target_op = target_tag ? nullptr : category_tab_custom_icon_target_op_get(C, op);
  if (target_tag == nullptr && target_op == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No icon editor is active");
    return OPERATOR_CANCELLED;
  }

  char icon_path[1024] = "";
  if (target_tag != nullptr) {
    STRNCPY(icon_path, target_tag->icon_path);
  }
  else if (PropertyRNA *prop = RNA_struct_find_property(target_op->ptr, "icon_path")) {
    RNA_property_string_get(target_op->ptr, prop, icon_path);
  }
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

  if (target_tag != nullptr) {
    /* Path is unchanged, so only the cache had to be dropped above; the update call re-resolves
     * it and repaints everything showing this tag. */
    category_tab_custom_icon_apply_to_tag(C, target_tag, nullptr);
    WM_main_add_notifier(NC_WINDOW, nullptr);
    BKE_report(op->reports, RPT_INFO, "Custom icon reloaded");
    return OPERATOR_FINISHED;
  }

  if (category_tab_custom_icon_target_is_tag(target_op)) {
    ui::tag_icon_live_update_cb(C, target_op, 0);
  }
  else {
    category_tab_edit_live_update_cb(C, target_op, 0);
  }
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

  PropertyRNA *target_prop = RNA_def_string(ot->srna,
                                            "target_operator_ptr",
                                            nullptr,
                                            64,
                                            "Target Operator Pointer",
                                            "Internal: address of the dialog operator to write into");
  RNA_def_property_flag(target_prop, PROP_HIDDEN);

  PropertyRNA *target_tag_prop = RNA_def_string(
      ot->srna,
      "target_tag",
      nullptr,
      64,
      "Target Tag",
      "Internal: name of the tag to write into, used when no dialog is open");
  RNA_def_property_flag(target_tag_prop, PROP_HIDDEN);
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

  /* Read category before closing popup. */
  char category[64] = "";
  if (category_tab_current_dialog_op) {
    RNA_string_get(category_tab_current_dialog_op->ptr, "category", category);
  }

  if (category[0] == '\0') {
    return OPERATOR_CANCELLED;
  }

  /* Commit the current dialog state to the runtime cache before writing JSON.
   * This preserves the values the user just edited in the popup. */
  if (category_tab_current_dialog_op) {
    category_tab_edit_dialog_exec(C, category_tab_current_dialog_op);
  }

#ifdef WITH_PYTHON
  /* Store category in WM property (UTF-8 safe via RNA) */
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm) {
    PointerRNA wm_ptr = RNA_pointer_create_discrete(&wm->id, RNA_WindowManager, wm);
    RNA_string_set(&wm_ptr, "category_tab_save_category", category);
  }

  ScrArea *area = CTX_wm_area(C);

  /* Directly save to JSON without triggering WM sync - live update already applied changes.
   * This avoids the 2-4ms UI block caused by _sync_glyph_mappings_to_wm_impl() processing
   * all 37 categories in the main thread after Save.
   * 
   * The _save_glyph_mappings_to_file with force_discovery_skip=False will:
   * 1. Build data from _glyph_cache (already updated by live preview via toggle_category_tag_no_save)
   * 2. Save to JSON file in background thread
   * 3. NOT trigger any WM sync operations */
  /* Save glyph mappings to JSON (background, no WM sync) via the centralized Python bridge. */
  ui::category_py_save_glyph_mappings_to_file(C);

  category_tab_reset_tag_redraw(C, wm, area);
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
  /* For other operators (for example Create Tag) the update happens automatically through RNA. */

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
  RNA_def_string(ot->srna, "source_extension", nullptr, 256, "Add-on", "Add-on identifier for this category");

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
  RNA_def_int(ot->srna,
              "original_space_type",
              -1,
              -1,
              4096,
              "Original Space Type",
              "Space type captured at dialog open for cancel-restore semantics",
              -1,
              4096);
  RNA_def_int(ot->srna,
              "original_override_space_type",
              -1,
              -1,
              4096,
              "Original Override Space Type",
              "Space type of the override source captured at dialog open",
              -1,
              4096);
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
  /* Popup-local tag filter mode operator */
  WM_operatortype_append(ui::SCREEN_OT_category_tab_popup_filter_set);
}

/** \} */

}  // namespace blender
