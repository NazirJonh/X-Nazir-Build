/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "interface_intern.hh"

#include "WM_api.hh"

#include "BLT_translation.hh"

namespace blender::ui {

/* Minimum width required to show all preset buttons inline (in UI units) */
#define MIN_WIDTH_FOR_INLINE_BUTTONS 16.0f

/* -------------------------------------------------------------------- */
/** \name Color Glyph Preset Popup Data
 *
 * Static storage for RNA pointer data to be used by popup menu.
 * \{ */

/* Static storage for passing RNA data to popup menu.
 * This is set when the "..." button is clicked and read by the popup draw function.
 */
static struct {
  bool is_set;
  PointerRNA ptr;
  PropertyRNA *prop;
  char propname[64];
} color_glyph_popup_data = {false};

/* -------------------------------------------------------------------- */
/** \name Color Glyph Preset Popup Menu
 *
 * Dropdown menu shown when there's not enough space for inline buttons.
 * \{ */

/* Callback function for popup block creation */
static Block *color_glyph_preset_popup_callback(bContext *C, ARegion *region, void * /*arg*/)
{
  using namespace blender::ui;

  /* Get style for layout */
  const uiStyle *style = ui::style_get_dpi();

  /* Create popup block using C++ API */
  Block *block = block_begin(C, region, "color_glyph_preset_popup", EmbossType::Pulldown);
  block_flag_enable(block, BLOCK_LOOP | BLOCK_MOVEMOUSE_QUIT);
  block_theme_style_set(block, BLOCK_THEME_STYLE_POPUP);

  /* Create layout in the block */
  Layout &layout = block_layout(
      block,
      LayoutDirection::Vertical,
      LayoutType::Menu,
      0,
      0,
      UI_UNIT_X * 10,  /* Width */
      0,
      0,
      style);

  /* Get theme colors */
  bTheme *btheme = static_cast<bTheme *>(U.themes.first);

  /* Get RNA data from static storage */
  if (!color_glyph_popup_data.is_set) {
    layout.label("Error: Color data not available", ICON_ERROR);
    return block;
  }

  PointerRNA *ptr = &color_glyph_popup_data.ptr;
  PropertyRNA *prop = color_glyph_popup_data.prop;
  const char *propname = color_glyph_popup_data.propname;

  /* Get operator type for color preset */
  wmOperatorType *ot_preset = WM_operatortype_find("WM_OT_tag_color_preset", false);
  if (!ot_preset) {
    return block;
  }

  /* Get tag name */
  PropertyRNA *name_prop = RNA_struct_find_property(ptr, "name");
  std::string tag_name_str;
  if (name_prop) {
    tag_name_str = RNA_property_string_get(ptr, name_prop);
  }

  /* Create menu items for each color preset */
  for (int i = 0; i < 9; i++) {
    const int preset = i - 1;  /* -1 to 7 */

    /* Create row for this preset */
    Layout &row = layout.row(true);
    row.alignment_set(LayoutAlign::Left);

    /* Create button text with glyph and label */
    char button_text[64];
    if (i == 0) {
      STRNCPY(button_text, IFACE_("None"));
    }
    else {
      /* Format: "⦗ Set Color Tag 1" etc */
      SNPRINTF(button_text, "\xEE\xA6\x97 %s %d", IFACE_("Set Color"), preset + 1);
    }

    Button *but = uiDefButO_ptr(
        block,
        ButtonType::But,
        ot_preset,
        wm::OpCallContext::ExecDefault,
        button_text,
        0,
        0,
        UI_UNIT_X * 5.5f,
        UI_UNIT_Y,
        std::nullopt);

    if (i == 0) {
      /* NONE button - show X icon */
      def_but_icon(but, ICON_X, UI_HAS_ICON);
      but->col[0] = 0;
      but->col[1] = 0;
      but->col[2] = 0;
      but->col[3] = 0;
      but->drawflag &= ~BUT_TEXT_USE_COL;
    }
    else {
      /* Color preset button - show colored glyph */
      const ThemeGlyphColor *glyph_color = &btheme->glyph_color[preset];
      button_color_set(but, glyph_color->color);
      but->drawflag |= BUT_TEXT_USE_COL;
    }

    /* Set operator properties */
    PointerRNA *op_ptr = button_operator_ptr_ensure(but);
    RNA_string_set(op_ptr, "tag_name", tag_name_str.c_str());
    RNA_string_set(op_ptr, "propname", propname);
    RNA_int_set(op_ptr, "preset", preset);

    /* Store RNA data for operator to access directly */
    but->rnapoin = *ptr;
    but->rnaprop = prop;
    but->rnaindex = 0;
  }

  /* Set popup bounds and direction after all content is created */
  block_bounds_set_normal(block, 0.3f * U.widget_unit);
  block_direction_set(block, UI_DIR_DOWN);

  return block;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Glyph Presets Template
 * \{ */

void uiTemplateColorGlyphPresets(Layout *layout,
                                  bContext *C,
                                  PointerRNA *ptr,
                                  const char *propname)
{
  /* Get the color property */
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    return;
  }

  /* Verify property type (must be float color array) */
  if (RNA_property_type(prop) != PROP_FLOAT) {
    return;
  }

  const int array_length = RNA_property_array_length(ptr, prop);
  if (array_length != 3) {
    return;
  }

  /* Get theme colors */
  bTheme *btheme = static_cast<bTheme *>(U.themes.first);

  /* Determine if we have enough space for inline buttons
   * Check region width if available */
  bool use_inline_buttons = true;
  float available_width = 0.0f;

  if (C) {
    ARegion *region = CTX_wm_region(C);
    if (region) {
      /* Calculate available width in UI units
       * region->winx is in pixels, UI_SCALE_FAC is DPI scale, U.widget_unit is pixels per UI unit */
      available_width = region->winx / UI_SCALE_FAC / U.widget_unit;

      /* Minimum width needed:
       * - 9 buttons * 1.5 units each = 13.5 units
       * - 1 separator = 0.5 units
       * - 1 color picker = 1.0 units
       * Total = 15 units minimum, but we use 16 as threshold
       */
      use_inline_buttons = (available_width >= MIN_WIDTH_FOR_INLINE_BUTTONS);
    }
  }

  /* Get tag name from the RNA pointer (CategoryTagDef has a 'name' property) */
  PropertyRNA *name_prop = RNA_struct_find_property(ptr, "name");
  std::string tag_name_str;
  if (name_prop) {
    tag_name_str = RNA_property_string_get(ptr, name_prop);
  }

  if (use_inline_buttons) {
    /* INLINE MODE: Show all preset buttons in a row */

    /* Create centered row for preset buttons */
    Layout &presets_row = layout->row(true);
    presets_row.alignment_set(LayoutAlign::Center);
    presets_row.emboss_set(EmbossType::Pulldown);

    /* Get block for creating buttons */
    Block *block = presets_row.block();
    block_layout_set_current(block, &presets_row);

    /* Get operator type */
    wmOperatorType *ot = WM_operatortype_find("WM_OT_tag_color_preset", false);
    if (!ot) {
      return;
    }

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
        /* NONE button - show X icon, no color */
        def_but_icon(but, ICON_X, UI_HAS_ICON);
        but->col[0] = 0;
        but->col[1] = 0;
        but->col[2] = 0;
        but->col[3] = 0;
        but->drawflag &= ~BUT_TEXT_USE_COL;
      }
      else {
        /* Color preset button - show glyph with theme color */
        const ThemeGlyphColor *glyph_color = &btheme->glyph_color[preset];
        button_color_set(but, glyph_color->color);
        but->drawflag |= BUT_TEXT_USE_COL;
      }

      /* Set operator properties */
      PointerRNA *op_ptr = button_operator_ptr_ensure(but);

      RNA_string_set(op_ptr, "tag_name", tag_name_str.c_str());
      RNA_string_set(op_ptr, "propname", propname);
      RNA_int_set(op_ptr, "preset", preset);

      /* Store RNA data in button */
      but->rnapoin = *ptr;
      but->rnaprop = prop;
      but->rnaindex = 0;
    }

    /* Add separator */
    presets_row.separator();

    /* Add custom color picker for colors not in presets */
    Layout &picker_col = presets_row.column(false);
    picker_col.ui_units_x_set(1.0f);
    picker_col.prop(ptr, propname, ITEM_R_ICON_ONLY, "", ICON_NONE);
  }
  else {
    /* DROPDOWN MODE: Show a "..." button that opens a popup with all presets */

    /* Create a row with the dropdown button and color picker */
    Layout &main_row = layout->row(true);
    main_row.alignment_set(LayoutAlign::Center);

    /* Store RNA data in static storage for popup callback to access */
    color_glyph_popup_data.is_set = true;
    color_glyph_popup_data.ptr = *ptr;
    color_glyph_popup_data.prop = prop;
    BLI_strncpy(color_glyph_popup_data.propname, propname, sizeof(color_glyph_popup_data.propname));

    /* Create "..." button that opens popup */
    Block *block = main_row.block();
    block_layout_set_current(block, &main_row);

    Button *popup_but = uiDefBlockButN(
        block,
        (BlockCreateFunc)color_glyph_preset_popup_callback,
        nullptr,
        IFACE_("Color Presets"),
        0,
        0,
        int(UI_UNIT_X * 6.0f),
        UI_UNIT_Y,
        std::nullopt,
        nullptr,
        nullptr);

    /* Store RNA data in the button for future use */
    popup_but->rnapoin = *ptr;
    popup_but->rnaprop = prop;
    popup_but->rnaindex = 0;

    /* Add color picker next to dropdown */
    Layout &picker_col = main_row.column(false);
    picker_col.ui_units_x_set(1.0f);
    picker_col.prop(ptr, propname, ITEM_R_ICON_ONLY, "", ICON_NONE);
  }
}

}  // namespace blender::ui
