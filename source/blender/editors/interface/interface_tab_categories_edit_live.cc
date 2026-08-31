/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tab Edit Popup - tag filter mode, tooltip, live preview update and
 * glyph-search callbacks driven by the edit dialog.
 *
 * Split from interface_tab_categories_edit.cc; see
 * interface_tab_categories_edit_intern.hh for the shared internal contract.
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
#include "interface_tag_bar.hh"
#include "regions/interface_regions_intern.hh"
#include "interface_tab_categories_edit_intern.hh"

namespace blender::ui {

/* Import internal template functions with callback support */
using internal::uiTemplateGlyphInputRowWithCallback;
using internal::uiTemplateGlyphSearchResultsWithCallback;
using internal::uiTemplateGlyphSelectorWithCallback;

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
char get_current_object_mode_filter_value(const bContext *C)
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
/** \name Category Tag Tooltip Helpers
 * \{ */


/**
 * Get mode_flags for a tag by name from the window manager's category_tags list.
 * Returns 0 if tag not found or has no mode restrictions (works in all modes).
 */
uint32_t category_tag_get_mode_flags(const wmWindowManager *wm, const char *tag_name)
{
  if (wm == nullptr || tag_name == nullptr || tag_name[0] == '\0') {
    return 0;
  }

  for (const CategoryTagDef *tag = static_cast<const CategoryTagDef *>(wm->category_tags.first);
       tag;
       tag = static_cast<const CategoryTagDef *>(tag->next))
  {
    if (STREQ(tag->name, tag_name)) {
      return tag->mode_flags;
    }
  }
  return 0;
}

/**
 * Get translated name for a CategoryTagMode flag.
 */
static const char *category_tag_mode_get_name(CategoryTagMode mode)
{
  switch (mode) {
    case CategoryTagMode::OBJECT_MODE:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Object Mode");
    case CategoryTagMode::EDIT_MODE:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Edit Mode");
    case CategoryTagMode::SCULPT_MODE:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Sculpt Mode");
    case CategoryTagMode::VERTEX_PAINT:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Vertex Paint");
    case CategoryTagMode::WEIGHT_PAINT:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Weight Paint");
    case CategoryTagMode::TEXTURE_PAINT:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Texture Paint");
    case CategoryTagMode::UV_EDIT:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "UV Edit");
    case CategoryTagMode::POSE_MODE:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Pose Mode");
    case CategoryTagMode::GEOMETRY_NODES:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Geometry Nodes");
    case CategoryTagMode::SHADER_EDITOR:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Shader Editor");
    case CategoryTagMode::IMAGE_PAINT:
      return CTX_TIP_(BLT_I18NCONTEXT_ID_WINDOWMANAGER, "Image Paint");
    default:
      return "";
  }
}

/**
 * Build a string listing all modes for a tag's mode_flags.
 * Format: "Filter Mode:\n• Object Mode\n• Edit Mode\n..."
 */
static std::string category_tag_build_modes_string(uint32_t mode_flags)
{
  if (mode_flags == 0) {
    return "";
  }

  std::string result = TIP_("Filter Mode:");

  /* Check each mode flag */
  const CategoryTagMode modes[] = {
      CategoryTagMode::OBJECT_MODE,
      CategoryTagMode::EDIT_MODE,
      CategoryTagMode::SCULPT_MODE,
      CategoryTagMode::VERTEX_PAINT,
      CategoryTagMode::WEIGHT_PAINT,
      CategoryTagMode::TEXTURE_PAINT,
      CategoryTagMode::UV_EDIT,
      CategoryTagMode::POSE_MODE,
      CategoryTagMode::GEOMETRY_NODES,
      CategoryTagMode::SHADER_EDITOR,
      CategoryTagMode::IMAGE_PAINT,
  };

  for (const CategoryTagMode mode : modes) {
    if (mode_flags & uint32_t(mode)) {
      const char *mode_name = category_tag_mode_get_name(mode);
      if (mode_name && mode_name[0] != '\0') {
        result += "\n• ";
        result += mode_name;
      }
    }
  }

  return result;
}

/**
 * Tooltip function for tag glyphs that shows tag name and mode information.
 */
std::string tag_glyph_tooltip_func(bContext * /*C*/, void *argN, StringRef /*tip*/)
{
  TagTooltipData *data = static_cast<TagTooltipData *>(argN);
  if (data == nullptr) {
    return "";
  }

  /* Build full tooltip: tag name + modes */
  std::string result = data->tag_name;

  const std::string modes_str = category_tag_build_modes_string(data->mode_flags);
  if (!modes_str.empty()) {
    result += "\n\n";
    result += modes_str;
  }

  return result;
}

/**
 * Free function for tooltip data.
 */
void tag_tooltip_data_free(void *arg)
{
  TagTooltipData *data = static_cast<TagTooltipData *>(arg);
  delete data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Category Tag Filter Toggle Menu
 * \{ */

static MenuType *category_tag_filter_toggle_menu_type = nullptr;

/**
 * Operator to set the popup's local tag filter mode.
 * This only affects the edit popup, not the global wm->category_tag_filter_mode.
 */
static wmOperatorStatus category_tab_popup_filter_set_exec(bContext *C, wmOperator *op)
{
  const bool use_current_mode = RNA_boolean_get(op->ptr, "use_current_mode");
  
  /* Set the popup's local filter mode */
  if (use_current_mode) {
    /* Get current object mode and convert to filter mode enum value */
    category_tab_popup_local_filter_mode = get_current_object_mode_filter_value(C);
  }
  else {
    /* Show all tags */
    category_tab_popup_local_filter_mode = 0;
  }
  
  /* Trigger redraw */
  WM_main_add_notifier(NC_WINDOW, nullptr);
  
  return OPERATOR_FINISHED;
}

void SCREEN_OT_category_tab_popup_filter_set(wmOperatorType *ot)
{
  ot->name = "Set Popup Tag Filter Mode";
  ot->idname = "SCREEN_OT_category_tab_popup_filter_set";
  ot->description = "Set tag filter mode for the edit popup (Current Mode or All Tags)";
  
  ot->exec = category_tab_popup_filter_set_exec;
  
  RNA_def_boolean(
      ot->srna,
      "use_current_mode",
      true,
      "Use Current Mode",
      "Filter tags by current object mode (True) or show all tags (False)"
  );
}

static void category_tag_filter_toggle_menu_draw(const bContext * /*C*/, Menu *menu)
{
  Layout &layout = *menu->layout;

  /* Use the popup's local filter mode, not the global wm->category_tag_filter_mode.
   * This prevents changes in the popup from affecting the Preferences UI list filtering. */
  bool use_current_mode = (category_tab_popup_local_filter_mode != 0);

  /* "Current Mode" button - activates filtering by current object mode */
  /* Use radiobutton icons to indicate active state */
  int current_mode_icon = use_current_mode ? ICON_RADIOBUT_ON : ICON_RADIOBUT_OFF;
  PointerRNA current_mode_ptr = layout.op(
      "SCREEN_OT_category_tab_popup_filter_set", IFACE_("Current Mode"), current_mode_icon);
  RNA_boolean_set(&current_mode_ptr, "use_current_mode", true);

  /* "All Tags" button - shows all tags regardless of mode */
  int all_tags_icon = use_current_mode ? ICON_RADIOBUT_OFF : ICON_RADIOBUT_ON;
  PointerRNA all_tags_ptr = layout.op(
      "SCREEN_OT_category_tab_popup_filter_set", IFACE_("All Tags"), all_tags_icon);
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

  /* Global-First: Always use GLOBAL space_type (-1) for override.
   * This ensures consistency with Python code and prevents C++ from finding
   * stale space-specific overrides first (which have priority in lookup). */
  ScrArea *area = CTX_wm_area(C);
  const int space_type = area ? area->spacetype : -1;
  const int override_space_type = -1;  // Always GLOBAL for override

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

  category_tab_remove_stale_space_specific_overrides(wm, category, "LIVE UPDATE");

  CategoryGlyphItem *item = category_glyph_item_find(
      wm->category_glyph_overrides, category, override_space_type);

  /* Look up default glyph for preview fallback (when user input is empty or invalid) */
  bool is_fallback = false;
  const char *default_glyph = category_tab_lookup_runtime_default_glyph(
      wm, category, space_type, item, glyph_raw[0] == '\0', &is_fallback, nullptr);

  const int display_mode_ui = RNA_enum_get(op->ptr, "display_mode_ui");

  char preview_name[32] = "";
  RNA_string_get(op->ptr, "display_name", preview_name);
  const char *first_letter_source = (preview_name[0] != '\0') ? preview_name : category;
  char fallback_letter[8] = "";
  if (!category_tab_first_utf8_char_copy(first_letter_source, fallback_letter, sizeof(fallback_letter))) {
    fallback_letter[0] = '\0';
  }
  STRNCPY(category_tab_preview_first_letter, fallback_letter);

  /* Update preview buffers for popup preview.
   * Use the processed glyph from valid input, or fall back to default lookup.
   * Invalid input shows the default glyph (not the invalid text).
   * Fallback letter is sourced from the display name/category. */
  copy_v3_v3(category_tab_preview_color, color);
  category_tab_compute_preview_glyph(
      category_tab_preview_glyph, display_mode_ui, glyph, default_glyph, is_fallback, fallback_letter);

  /* Create if not found - always use GLOBAL space_type */
  if (!item) {
    item = category_glyph_item_ensure(wm->category_glyph_overrides, category, override_space_type);

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
  }

  /* Update display_name for live preview */
  char display_name[32];
  RNA_string_get(op->ptr, "display_name", display_name);
  if (display_name[0] != '\0') {
    STRNCPY(item->display_name, display_name);
    category_tab_first_utf8_char_copy(display_name, item->first_letter, sizeof(item->first_letter));
  }

  /* Update color for live preview */
  copy_v3_v3(item->color, color);

  /* Update icon fields for live preview/state carry-over. */
  CategoryTabIconState icon_state;
  category_tab_icon_state_read(op->ptr, icon_state);
  category_tab_icon_state_apply(*item, icon_state);

  const int custom_icon_mode_ui = RNA_enum_get(op->ptr, "custom_icon_mode_ui");

  bool clear_blender_icon_key = false;
  const int resolved_icon_source = category_tab_resolve_icon_source(
      display_mode_ui,
      custom_icon_mode_ui,
      RNA_enum_get(op->ptr, "icon_source"),
      CategoryTabIconSourceResolveMode::Preview,
      &clear_blender_icon_key);
  if (clear_blender_icon_key) {
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
    /* Empty glyph - check if this is a glyph_only category */
    const bool is_glyph_only_category = is_single_glyph_str(category);
    if (is_glyph_only_category) {
      /* For glyph_only categories, get glyph from mappings (the original glyph) */
      for (CategoryGlyphItem *map_item =
               static_cast<CategoryGlyphItem *>(wm->category_glyph_mappings.first);
           map_item;
           map_item = static_cast<CategoryGlyphItem *>(map_item->next))
      {
        if (STREQ(map_item->category, category)) {
          if (map_item->glyph[0] != '\0') {
            STRNCPY(item->glyph, map_item->glyph);
          }
          else if (map_item->default_glyph[0] != '\0') {
            STRNCPY(item->glyph, map_item->default_glyph);
          }
          else {
            item->glyph[0] = '\0';
          }
          break;
        }
      }
    }
    else {
      /* Not a glyph_only category - clear override glyph to use defaults */
      item->glyph[0] = '\0';
    }
  }

  /* Trigger live redraw so icon/glyph updates appear instantly without restart. */
  category_tab_live_update_notify_redraw(C);

  /* Note: Preview button uses custom draw callback that reads directly from
   * category_tab_preview_glyph and category_tab_preview_color static buffers,
   * which are already updated above. No button-specific update needed. */
}

/**
 * Live update callback for tag icon picker.
 * Updates wm.category_tags collection when icon is selected in the picker.
 * This is the Tags equivalent of category_tab_edit_live_update_cb.
 */
void tag_icon_live_update_cb(bContext *C, void *arg_op, int /*event*/)
{
  wmOperator *op = static_cast<wmOperator *>(arg_op);

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[TAG_ICON_LIVE_UPDATE] Called with op=%p, idname='%s'\n", (void*)op, op ? op->idname : "null");
  }

  /* Get tag name from operator */
  char tag_name[64];
  RNA_string_get(op->ptr, "name", tag_name);

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[TAG_ICON_LIVE_UPDATE] tag_name='%s'\n", tag_name);
  }

  if (tag_name[0] == '\0') {
    /* No tag name yet, skip update */
    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[TAG_ICON_LIVE_UPDATE] No tag name, skipping\n");
    }
    return;
  }

  /* Get icon properties from operator */
  char icon_key[128];
  RNA_string_get(op->ptr, "icon_key", icon_key);
  int icon_source = RNA_enum_get(op->ptr, "icon_source");
  const int display_mode_ui = RNA_enum_get(op->ptr, "display_mode_ui");

  /* `icon_path` only exists on dialogs that support custom icon files; read defensively so this
   * callback keeps working for any caller that lacks the property. */
  char icon_path[1024] = "";
  if (PropertyRNA *icon_path_prop = RNA_struct_find_property(op->ptr, "icon_path")) {
    RNA_property_string_get(op->ptr, icon_path_prop, icon_path);
  }

  if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
    printf("[TAG_ICON_LIVE_UPDATE] icon_key='%s', icon_path='%s', icon_source=%d, display_mode_ui=%d\n",
           icon_key, icon_path, icon_source, display_mode_ui);
  }

  /* Get color */
  float color[3];
  RNA_float_get_array(op->ptr, "color", color);

  /* Update operator properties first - this is critical for template_icon_preview to update */
  RNA_string_set(op->ptr, "icon_key", icon_key);
  if (PropertyRNA *icon_path_prop = RNA_struct_find_property(op->ptr, "icon_path")) {
    RNA_property_string_set(op->ptr, icon_path_prop, icon_path);
  }
  RNA_enum_set(op->ptr, "icon_source", icon_source);
  RNA_enum_set(op->ptr, "display_mode_ui", display_mode_ui);
  RNA_float_set_array(op->ptr, "color", color);

  /* Update WM category_tags collection */
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm && category_tag_list_is_valid(&wm->category_tags)) {
    /* Find existing tag to update */
    CategoryTagDef *tag_item = nullptr;
    for (CategoryTagDef *iter = static_cast<CategoryTagDef *>(wm->category_tags.first);
         iter != nullptr;
         iter = iter->next)
    {
      if (STREQ(iter->name, tag_name)) {
        tag_item = iter;
        break;
      }
    }

    /* If tag doesn't exist in WM yet, we can't add it here - will be added by execute */
    if (!tag_item) {
      /* Tag not created yet - skip WM update, will be done by create_tag */
      return;
    }

    /* Update existing tag */
    STRNCPY(tag_item->icon_key, icon_key);
    STRNCPY(tag_item->icon_path, icon_path);
    tag_item->icon_source = icon_source;
    copy_v3_v3(tag_item->color, color);

    if constexpr (CATEGORY_TAB_DEBUG_ENABLED) {
      printf("[TAG ICON LIVE UPDATE] Updated tag '%s': icon_key='%s', icon_path='%s', icon_source=%d, display_mode_ui=%d\n",
             tag_name, icon_key, icon_path, icon_source, display_mode_ui);
    }
  }

  /* The tag bar caches resolved icon ids per button; without this the custom icon would only
   * appear after some unrelated event invalidated the cache. */
  tag_bar_mark_all_dirty();

  /* Trigger redraw for live preview - notify area to update template_icon_preview */
  WM_main_add_notifier(NC_WINDOW, nullptr);
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_tag_redraw(area);
  }
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

}  // namespace blender::ui
