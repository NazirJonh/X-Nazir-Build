/* SPDX-FileCopyrightText: 2026 Blender Authors. All rights reserved. */
/*
 * Horizontal Tag Bar for Category Filtering
 */

#include <map>
#include <optional>
#include <algorithm>

#include "interface_tag_bar.hh"
#include "interface_intern.hh"

#include "DNA_userdef_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "BLF_api.hh"

namespace blender::ui {

using blender::wmWindowManager;
using blender::bContext;
using blender::View3D;
using blender::SpaceProperties;
using blender::ScrArea;
using blender::CategoryTagDef;
using blender::ARegion;
using blender::wmNotifier;
using blender::wmEvent;
using blender::wmRegionListenerParams;
using blender::wmRegionMessageSubscribeParams;
using blender::Vector;
using blender::Span;

/* Global cache for tag bar data, keyed by wmWindowManager pointer */
static std::map<const wmWindowManager *, TagBarRuntimeData *> g_tag_bar_cache;

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

/**
 * Mark all cached tag bar data as needing update.
 * Called from listeners when glyphs or tags change.
 */
void tag_bar_mark_all_dirty()
{
  for (auto &entry : g_tag_bar_cache) {
    if (entry.second) {
      entry.second->needs_update = true;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Functions
 * \{ */

/**
 * Get or create tag bar runtime data from global cache.
 * Creates the data if it doesn't exist.
 * Updates data if needs_update flag is set.
 */
TagBarRuntimeData *get_tag_bar_data_global(const bContext *C)
{
  if (!C) {
    return nullptr;
  }

  wmWindowManager *wm = CTX_wm_manager(C);

  if (!wm) {
    return nullptr;
  }

  TagBarRuntimeData *data = g_tag_bar_cache[wm];

  if (!data) {
    data = MEM_new<TagBarRuntimeData>(__func__);
    g_tag_bar_cache[wm] = data;
  }

  /* Get View3D for filter state */
  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = nullptr;
  if (area && area->spacetype == SPACE_VIEW3D) {
    v3d = static_cast<View3D *>(area->spacedata.first);
  }

  /* Update data if needs_update flag is set */
  if (data->needs_update) {
    tag_bar_buttons_update(wm, v3d, data);
    data->needs_update = false;
  }

  return data;
}

/**
 * Check if a tag exists in a comma-separated tag string.
 */
bool has_tag_in_string(const char *tags_string, const char *tag_name)
{
  if (!tags_string || !tags_string[0] || !tag_name || !tag_name[0]) {
    return false;
  }

  const char *cursor = tags_string;
  char tag[64];

  while (*cursor) {
    /* Skip whitespace */
    while (*cursor == ' ') {
      cursor++;
    }

    if (!*cursor) {
      break;
    }

    int i = 0;
    while (*cursor && *cursor != ',' && *cursor != ';' && i < 63) {
      tag[i++] = *cursor++;
    }

    while (i > 0 && tag[i - 1] == ' ') {
      i--;
    }
    tag[i] = '\0';

    if (*cursor == ',' || *cursor == ';') {
      cursor++;
    }

    if (tag[0] == '\0') {
      continue;
    }

    if (STREQ(tag, tag_name)) {
      return true;
    }
  }

  return false;
}

/**
 * Check if any active tag matches the given tag string (OR logic).
 */
bool has_any_tag_active(const wmWindowManager *wm,
                        const char *tags_string,
                        int64_t active_mask)
{
  if (active_mask == 0) {
    return true;  /* Filter not active - show all */
  }

  if (!tags_string || !tags_string[0]) {
    return false;  /* No tags - hide */
  }

  char tags_copy[256];
  STRNCPY(tags_copy, tags_string);

  char *tag = strtok(tags_copy, ",;");
  while (tag != nullptr) {
    while (*tag == ' ') {
      tag++;
    }

    /* Find tag_def for this tag */
    if (wm && category_tag_list_is_valid(&wm->category_tags)) {
      for (const CategoryTagDef *tag_def =
               static_cast<const CategoryTagDef *>(wm->category_tags.first);
           tag_def;
           tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
      {
        if (STREQ(tag, tag_def->name)) {
          if (active_mask & tag_def->mode_flags) {
            return true;  /* This tag is active */
          }
          break;
        }
      }
    }

    tag = strtok(nullptr, ",;");
  }

  return false;
}

/**
 * Check if ALL active tags match the given tag string (AND logic).
 * When multiple tags are active, the category must have ALL of them to be visible.
 */
bool has_all_tags_active(const wmWindowManager *wm,
                         const char *tags_string,
                         int64_t active_mask)
{
  if (active_mask == 0) {
    return true;  /* Filter not active - show all */
  }

  if (!tags_string || !tags_string[0]) {
    return false;  /* No tags - hide */
  }

  /* First, collect all tag bits that the category has */
  int64_t category_tag_bits = 0;

  char tags_copy[256];
  STRNCPY(tags_copy, tags_string);

  char *tag = strtok(tags_copy, ",;");
  while (tag != nullptr) {
    while (*tag == ' ') {
      tag++;
    }

    /* Find tag_def for this tag and add its bit to category_tag_bits */
    if (wm && category_tag_list_is_valid(&wm->category_tags)) {
      for (const CategoryTagDef *tag_def =
               static_cast<const CategoryTagDef *>(wm->category_tags.first);
           tag_def;
           tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
      {
        if (STREQ(tag, tag_def->name)) {
          category_tag_bits |= tag_def->mode_flags;
          break;
        }
      }
    }

    tag = strtok(nullptr, ",;");
  }

  /* Check if ALL active tag bits are present in category's tags */
  /* (active_mask & category_tag_bits) must equal active_mask for AND logic */
  return (active_mask & category_tag_bits) == active_mask;
}

/**
 * Update tag bar buttons based on tags from window manager.
 */
void tag_bar_buttons_update(const wmWindowManager *wm,
                            View3D *v3d,
                            TagBarRuntimeData *data)
{
  if (!data) {
    return;
  }

  data->buttons.clear();
  data->total_width = 0;

  /* Get active filter tags from View3D */
  char active_tags[256] = "";
  if (v3d) {
    STRNCPY(active_tags, v3d->active_tag_filter_tags);
  }

  /* Iterate through all tags from wm */
  if (wm && category_tag_list_is_valid(&wm->category_tags)) {
    for (const CategoryTagDef *tag_def =
             static_cast<const CategoryTagDef *>(wm->category_tags.first);
         tag_def;
         tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
    {
      TagButton btn;
      STRNCPY(btn.tag_name, tag_def->name);
      STRNCPY(btn.glyph, tag_def->glyph);
      copy_v3_v3(btn.color, tag_def->color);
      btn.is_visible = true;
      btn.is_hovered = false;
      /* Check if this tag is in the active tags list */
      btn.is_active = has_tag_in_string(active_tags, tag_def->name);

      /* Count categories with this tag */
      btn.category_count = 0;
      if (category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
        for (const CategoryGlyphItem *item =
                 static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
             item;
             item = static_cast<const CategoryGlyphItem *>(item->next))
        {
          if (has_tag_in_string(item->tags, tag_def->name)) {
            btn.category_count++;
          }
        }
      }

      data->buttons.append(btn);
    }
  }

  /* Sort by category count (highest first) */
  std::sort(data->buttons.begin(), data->buttons.end(),
            [](const TagButton &a, const TagButton &b) {
              if (a.category_count != b.category_count) {
                return a.category_count > b.category_count;
              }
              return strcmp(a.tag_name, b.tag_name) < 0;
            });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Region Callbacks
 * \{ */

/* Callback for tag button click */
void tag_button_click_by_mode(bContext *C, void *arg1, void *arg2)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(arg1);
  const int mode_flags = POINTER_AS_INT(arg2);

  /* Get View3D to update the filter mask */
  ScrArea *area = CTX_wm_area(C);
  View3D *v3d = nullptr;
  if (area && area->spacetype == SPACE_VIEW3D) {
    v3d = static_cast<View3D *>(area->spacedata.first);
  }

  /* NOTE: This function is deprecated. Tag toggling is now handled by the operator in view3d_ops.cc.
   * The operator directly modifies the active_tag_filter_tags string. */

  /* Update cache and redraw */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (data) {
    tag_bar_buttons_update(wm, v3d, data);
    data->needs_update = false;  /* Just updated, no need to update again */
  }

  WM_main_add_notifier(NC_WM | ND_CATEGORY_GLYPHS, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
}

void buttons_tag_bar_region_init(wmWindowManager *wm, ARegion *region)
{
  /* Initialize region for UI interaction */
  ED_region_header_init(region);

  /* Add UI handlers for button interaction (mouse click, hover, etc) */
  region_handlers_add(&region->runtime->handlers);

  UNUSED_VARS(wm);
}

void buttons_tag_bar_region_exit(wmWindowManager * /*wm*/, ARegion * /*region*/)
{
  /* Cache cleanup is handled by SpaceProperties lifecycle */
}

void buttons_tag_bar_region_draw(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }

  SpaceProperties *sbuts = static_cast<SpaceProperties *>(area->spacedata.first);
  if (!sbuts) {
    return;
  }

  wmWindowManager *wm = CTX_wm_manager(C);

  /* Get or create data */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data || data->buttons.is_empty()) {
    return;
  }

  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;

  /* No background drawing - overlay region is transparent */

  /* Create UI block for buttons */
  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);
  Layout &layout = block_layout(block,
                                LayoutDirection::Horizontal,
                                LayoutType::Header,
                                0, 0,
                                region->winx, region->winy,
                                0,
                                style);

  /* Calculate scroll offset */
  int scroll_offset = sbuts->tag_bar_scroll_offset;
  if (scroll_offset < 0) {
    data->max_scroll = std::max(0, data->total_width - region->winx + 20);
    scroll_offset = -sbuts->tag_bar_scroll_offset;
  }

  /* Draw buttons via UI API */
  int xco = UI_UNIT_X / 2;
  for (TagButton &btn : data->buttons) {
    /* Calculate button width */
    const int fontid = style->widget.uifont_id;
    BLF_size(fontid, UI_UNIT_Y * 0.7f * dpi_fac);

    /* Convert glyph from hex to UTF-8 for display */
    char glyph_utf8[8] = "";
    const char *display_glyph = "";
    if (btn.glyph[0] != '\0') {
      if (tag_glyph_hex_to_utf8(btn.glyph, glyph_utf8)) {
        display_glyph = glyph_utf8;
      }
      else {
        display_glyph = btn.glyph;
      }
    }

    const int text_width = BLF_width(fontid, btn.tag_name, strlen(btn.tag_name));
    const int glyph_width = (display_glyph[0]) ? BLF_width(fontid, display_glyph, strlen(display_glyph)) : 0;
    const int btn_width = glyph_width + text_width + UI_UNIT_X / 2;

    /* Create button label with glyph */
    char button_label[72];
    if (display_glyph[0]) {
      SNPRINTF(button_label, "%s %s", display_glyph, btn.tag_name);
    }
    else {
      STRNCPY(button_label, btn.tag_name);
    }

    Button *but = uiDefBut(block,
                                ButtonType::ButToggle,
                               button_label,
                               xco,
                               0,
                               btn_width,
                               UI_UNIT_Y,
                               nullptr,
                               0.0f,
                               0.0f,
                               TIP_("Toggle category filter"));

    /* Set color for active button */
    if (but && btn.is_active) {
      but->col[0] = btn.color[0];
      but->col[1] = btn.color[1];
      but->col[2] = btn.color[2];
    }

    if (but) {
      but->func = tag_button_click_by_mode;
      but->func_arg1 = sbuts;
      /* Find the mode_flags for this tag */
      if (wm && category_tag_list_is_valid(&wm->category_tags)) {
        for (const CategoryTagDef *tag_def =
                 static_cast<const CategoryTagDef *>(wm->category_tags.first);
             tag_def;
             tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
        {
          if (STREQ(tag_def->name, btn.tag_name)) {
            but->func_arg2 = POINTER_FROM_INT(tag_def->mode_flags);
            break;
          }
        }
      }
    }

    xco += btn_width + UI_UNIT_X / 4;
  }
  data->total_width = xco;

  /* Scroll button (if needed) */
  if (data->total_width > region->winx) {
    [[maybe_unused]] Layout &row = layout.row(false);
    uiDefBut(block, ButtonType::Scroll, "",
             0, 0, UI_UNIT_X, UI_UNIT_Y,
             &sbuts->tag_bar_scroll_offset, 0.0f, float(data->max_scroll),
             "");
  }

  block_end(C, block);
  block_draw(C, block);
}

/**
 * Draw tag bar inside an existing region (e.g., header region).
 * This is called from view3d_header_region_draw to display tags inline.
 */
void buttons_tag_bar_draw_in_header(const bContext *C, ARegion *region)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }

  /* Only draw in View3D */
  if (area->spacetype != SPACE_VIEW3D) {
    return;
  }

  wmWindowManager *wm = CTX_wm_manager(C);

  /* Get or create data from global cache */
  TagBarRuntimeData *data = get_tag_bar_data_global(C);
  if (!data || data->buttons.is_empty()) {
    return;
  }

  /* Draw a simple horizontal bar with tag buttons */
  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;

  /* Create UI block for tag buttons - use default positioning */
  Block *block = block_begin(C, region, __func__, EmbossType::Emboss);
  Layout &layout = block_layout(block,
                                LayoutDirection::Horizontal,
                                LayoutType::Header,
                                0, 0,
                                region->winx, region->winy,
                                0,
                                style);

  /* Create row for buttons with alignment */
  [[maybe_unused]] Layout &row = layout.row(true);  /* true = align items */

  /* Draw tag buttons */
  int xco = 0;
  for (TagButton &btn : data->buttons) {
    /* Calculate button width */
    const int fontid = style->widget.uifont_id;
    BLF_size(fontid, UI_UNIT_Y * 0.5f * dpi_fac);

    /* Convert glyph from hex to UTF-8 for display */
    char glyph_utf8[8] = "";
    const char *display_glyph = "";
    if (btn.glyph[0] != '\0') {
      if (tag_glyph_hex_to_utf8(btn.glyph, glyph_utf8)) {
        display_glyph = glyph_utf8;
      }
      else {
        display_glyph = btn.glyph;
      }
    }

    const int text_width = BLF_width(fontid, btn.tag_name, strlen(btn.tag_name));
    const int glyph_width = (display_glyph[0]) ? BLF_width(fontid, display_glyph, strlen(display_glyph)) : 0;
    const int btn_width = glyph_width + text_width + 20;

    /* Create button label with glyph */
    char button_label[72];
    if (display_glyph[0]) {
      SNPRINTF(button_label, "%s %s", display_glyph, btn.tag_name);
    }
    else {
      STRNCPY(button_label, btn.tag_name);
    }

    Button *but = uiDefBut(block,
                                ButtonType::ButToggle,
                                button_label,
                                xco,
                                0,
                                btn_width,
                                UI_UNIT_Y - 4,
                                nullptr,
                                0.0f,
                                0.0f,
                                TIP_("Toggle category filter"));

    if (but) {
      /* Setup button colors and glyphs based on state */
      if (btn.is_active) {
        /* Active button: background uses tag color */
        rgb_float_to_uchar(but->col, btn.color);
        but->col[3] = 255;
      }
      else if (btn.glyph[0]) {
        /* Inactive but with glyph: we want the text (glyph + label) to be colored.
         * We MUST set BUT_TEXT_USE_COL flag for but->col to affect text drawing. */
        rgb_float_to_uchar(but->col, btn.color);
        but->col[3] = 255;
        but->drawflag |= BUT_TEXT_USE_COL;
      }

      but->func = tag_button_click_by_mode;
      but->func_arg1 = wm; /* Pass wm instead of sbuts */
      /* Find the mode_flags for this tag */
      if (wm && category_tag_list_is_valid(&wm->category_tags)) {
        for (const CategoryTagDef *tag_def =
                 static_cast<const CategoryTagDef *>(wm->category_tags.first);
             tag_def;
             tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
        {
          if (STREQ(tag_def->name, btn.tag_name)) {
            but->func_arg2 = POINTER_FROM_INT(tag_def->mode_flags);
            break;
          }
        }
      }
    }

    xco += btn_width + 4;
  }

  block_end(C, block);
  block_draw(C, block);
}

/**
 * Draw tag buttons into a given UI block.
 * This is designed to be called from region draw callbacks.
 * \param C: Context
 * \param block: UI block to add buttons to
 * \param region: Region for size calculations
 * \param start_x: X position to start drawing from (after external buttons)
 */
void tag_bar_draw_in_layout(const bContext *C, Block *block, ARegion *region, int start_x)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  TagBarRuntimeData *data = get_tag_bar_data_global(C);

  if (!data) {
    return;
  }

  const uiStyle *style = style_get_dpi();
  const float dpi_fac = UI_SCALE_FAC;

  /* Calculate total buttons width for positioning */
  int total_buttons_width = 0;
  if (!data->buttons.is_empty()) {
    for (const TagButton &btn : data->buttons) {
      const int fontid = style->widget.uifont_id;
      BLF_size(fontid, UI_UNIT_Y * 0.7f * dpi_fac);

      /* Convert glyph from hex to UTF-8 for display */
      char glyph_utf8[8] = "";
      const char *display_glyph = "";
      if (btn.glyph[0] != '\0') {
        if (tag_glyph_hex_to_utf8(btn.glyph, glyph_utf8)) {
          display_glyph = glyph_utf8;
        }
        else {
          display_glyph = btn.glyph;
        }
      }

      const int text_width = BLF_width(fontid, btn.tag_name, strlen(btn.tag_name));
      const int glyph_width = (display_glyph[0]) ? BLF_width(fontid, display_glyph, strlen(display_glyph)) : 0;
      total_buttons_width += glyph_width + text_width + UI_UNIT_X + UI_UNIT_X / 4;
    }
  }

  /* Start from the given position (after external buttons) with some padding.
   * For TAG_BAR region, use y=0 (top-aligned region) with proper height. */
  int xco = start_x + UI_UNIT_X / 2;
  const int yco = 0;  /* TAG_BAR is top-aligned, y=0 is the top edge */

  /* Draw tag buttons (if any exist) */
  if (!data->buttons.is_empty()) {
    for (TagButton &btn : data->buttons) {
    const int fontid = style->widget.uifont_id;
    BLF_size(fontid, UI_UNIT_Y * 0.7f * dpi_fac);

    /* Convert glyph from hex to UTF-8 for display */
    char glyph_utf8[8] = "";
    const char *display_glyph = "";
    if (btn.glyph[0] != '\0') {
      if (tag_glyph_hex_to_utf8(btn.glyph, glyph_utf8)) {
        display_glyph = glyph_utf8;
      }
      else {
        display_glyph = btn.glyph;
      }
    }

    const int text_width = BLF_width(fontid, btn.tag_name, strlen(btn.tag_name));
    const int glyph_width = (display_glyph[0]) ? BLF_width(fontid, display_glyph, strlen(display_glyph)) : 0;
    const int btn_width = glyph_width + text_width + UI_UNIT_X;

    /* Create button label with glyph */
    char button_label[72];
    if (display_glyph[0]) {
      SNPRINTF(button_label, "%s %s", display_glyph, btn.tag_name);
    }
    else {
      STRNCPY(button_label, btn.tag_name);
    }

    Button *but = uiDefBut(block,
                                ButtonType::ButToggle,
                                button_label,
                                xco,
                                yco,
                                btn_width,
                                UI_UNIT_Y - 4,  /* Match header button height */
                                nullptr,
                                0.0f,
                                0.0f,
                                TIP_("Toggle category filter"));

    if (but) {
      /* Setup button colors and glyphs based on state */
      if (btn.is_active) {
        /* Active button: background uses tag color.
         * For ButToggle, but->col sets the "checked" background color. */
        rgb_float_to_uchar(but->col, btn.color);
        but->col[3] = 255;
      }
      else if (btn.glyph[0]) {
        /* Inactive but with glyph: we want the text (glyph + label) to be colored.
         * We MUST set BUT_TEXT_USE_COL flag for but->col to affect text drawing. */
        rgb_float_to_uchar(but->col, btn.color);
        but->col[3] = 255;
        but->drawflag |= BUT_TEXT_USE_COL;
      }

      /* Set callback for button click */
      but->func = tag_button_click_by_mode;
      but->func_arg1 = wm;

      /* Find the mode_flags for this tag */
      if (wm && category_tag_list_is_valid(&wm->category_tags)) {
        for (const CategoryTagDef *tag_def =
                 static_cast<const CategoryTagDef *>(wm->category_tags.first);
             tag_def;
             tag_def = static_cast<const CategoryTagDef *>(tag_def->next))
        {
          if (STREQ(tag_def->name, btn.tag_name)) {
            but->func_arg2 = POINTER_FROM_INT(tag_def->mode_flags);
            break;
          }
        }
      }
    }

    xco += btn_width + UI_UNIT_X / 4;
  }
  }

  /* Store total width for View2D scrolling */
  data->total_width = xco;
}

void buttons_tag_bar_region_listener(const wmRegionListenerParams *params)
{
  const wmNotifier *wmn = params->notifier;
  ARegion *region = params->region;

  switch (wmn->category) {
    case NC_WM:
      if (wmn->data == ND_CATEGORY_GLYPHS) {
        /* Mark all tag bar data as dirty - will be updated on next draw */
        tag_bar_mark_all_dirty();
        ED_region_tag_redraw(region);
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_PROPERTIES) {
        ED_region_tag_redraw(region);
      }
      break;
  }
}

void buttons_tag_bar_region_message_subscribe(const wmRegionMessageSubscribeParams *params)
{
  wmMsgBus *mbus = params->message_bus;
  ARegion *region = params->region;

  wmMsgSubscribeValue msg_sub_value_region_tag_redraw{};
  msg_sub_value_region_tag_redraw.owner = region;
  msg_sub_value_region_tag_redraw.user_data = region;
  msg_sub_value_region_tag_redraw.notify = ED_region_do_msg_notify_tag_redraw;

  WM_msg_subscribe_rna_anon_prop(mbus, Window, view_layer, &msg_sub_value_region_tag_redraw);
}

/** \} */

}  // namespace blender::ui
