/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <cstdio>

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

#define DEBUG_COLOR_GLYPH_PRESETS 1

namespace blender::ui {

/* Minimum width required to show all preset buttons inline (in UI units) */
#define MIN_WIDTH_FOR_INLINE_BUTTONS 22.0f

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
#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] color_glyph_preset_popup_callback called!\n");
#endif

  using namespace blender::ui;

  /* Get style for layout */
  const uiStyle *style = ui::style_get_dpi();

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] Creating popup block...\n");
#endif

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

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] Layout created successfully\n");
#endif

  /* Get theme colors */
  bTheme *btheme = static_cast<bTheme *>(U.themes.first);

  /* Get RNA data from static storage */
  if (!color_glyph_popup_data.is_set) {
#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_POPUP] ERROR: Static data not set!\n");
#endif
    layout.label("Error: Color data not available", ICON_ERROR);
    return block;
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] Static data is set, getting RNA data...\n");
#endif

  PointerRNA *ptr = &color_glyph_popup_data.ptr;
  PropertyRNA *prop = color_glyph_popup_data.prop;
  const char *propname = color_glyph_popup_data.propname;

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] ptr=%p, prop=%p, propname='%s'\n", ptr, prop, propname);
#endif

  /* Get operator type for color preset */
  wmOperatorType *ot_preset = WM_operatortype_find("WM_OT_tag_color_preset", false);
  if (!ot_preset) {
    printf("[COLOR_GLYPH_POPUP] ERROR: WM_OT_tag_color_preset not found!\n");
    return block;
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] Creating menu items...\n");
#endif

  /* Get tag name */
  PropertyRNA *name_prop = RNA_struct_find_property(ptr, "name");
  std::string tag_name_str;
  if (name_prop) {
    tag_name_str = RNA_property_string_get(ptr, name_prop);
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] Tag name: '%s'\n", tag_name_str.c_str());
#endif

  /* Create menu items for each color preset */
  for (int i = 0; i < 9; i++) {
    const int preset = i - 1;  /* -1 to 7 */

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_POPUP] Creating item %d, preset=%d\n", i, preset);
#endif

    /* Create row for this preset */
    Layout &row = layout.row(true);
    row.alignment_set(LayoutAlign::Left);

    /* Create operator button */
    const char *button_text = (i == 0) ? IFACE_("None") : "  ";
    Button *but = uiDefButO_ptr(
        block,
        ButtonType::But,
        ot_preset,
        wm::OpCallContext::ExecDefault,
        button_text,
        0,
        0,
        UI_UNIT_X * 8,
        UI_UNIT_Y,
        std::nullopt);

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_POPUP] Button created at %p\n", but);
#endif

    if (i == 0) {
      /* NONE button - show X icon */
      def_but_icon(but, ICON_X, UI_HAS_ICON);
    }
    else {
      /* Color preset button - show colored box */
      const ThemeCollectionColor *category_tab_color = &btheme->collection_color[preset];
      button_color_set(but, category_tab_color->color);
    }

    /* Set operator properties */
    PointerRNA *op_ptr = button_operator_ptr_ensure(but);
    RNA_string_set(op_ptr, "tag_name", tag_name_str.c_str());
    RNA_string_set(op_ptr, "propname", propname);
    RNA_int_set(op_ptr, "preset", preset);

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_POPUP] Set op props: tag_name='%s', propname='%s', preset=%d\n",
           tag_name_str.c_str(), propname, preset);
#endif

    /* Store RNA data for operator to access directly */
    but->rnapoin = *ptr;
    but->rnaprop = prop;
    but->rnaindex = 0;

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_POPUP] Set button RNA: rnapoin.data=%p, rnaprop=%p\n",
           ptr->data, prop);
#endif
  }

  layout.separator();

  /* Custom color picker at the bottom */
  Layout &picker_row = layout.row(true);
  picker_row.prop(ptr, propname, ITEM_R_ICON_ONLY, std::nullopt, ICON_COLOR);

  /* Set popup bounds and direction after all content is created */
  block_bounds_set_normal(block, 0.3f * U.widget_unit);
  block_direction_set(block, UI_DIR_DOWN);

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_POPUP] Popup complete, returning block=%p\n", block);
#endif

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
#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] uiTemplateColorGlyphPresets called\n");
  if (ptr->type) {
    printf("[COLOR_GLYPH_PRESETS] ptr type identifier: %s\n", RNA_struct_identifier(ptr->type));
  }
  else {
    printf("[COLOR_GLYPH_PRESETS] ptr type: NULL\n");
  }
#endif

  /* Get the color property */
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname);
  if (!prop) {
    printf("[COLOR_GLYPH_PRESETS] ERROR: Property '%s' not found!\n", propname);
    return;
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] Property '%s' found, type: %d\n", propname, RNA_property_type(prop));
#endif

  /* Verify property type (must be float color array) */
  if (RNA_property_type(prop) != PROP_FLOAT) {
    printf("[COLOR_GLYPH_PRESETS] ERROR: Property type is not FLOAT!\n");
    return;
  }

  const int array_length = RNA_property_array_length(ptr, prop);
  if (array_length != 3) {
    printf("[COLOR_GLYPH_PRESETS] ERROR: Array length is %d, expected 3!\n", array_length);
    return;
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] Property validation passed (float[3])\n");
#endif

  /* Get theme colors */
  bTheme *btheme = static_cast<bTheme *>(U.themes.first);

  /* Determine if we have enough space for inline buttons
   * Check region width if available */
  bool use_inline_buttons = true;
  float available_width = 0.0f;

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] === Checking available space ===\n");
#endif

  if (C) {
#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Context is available\n");
#endif
    ARegion *region = CTX_wm_region(C);
    if (region) {
      /* Calculate available width in UI units
       * region->winx is in pixels, UI_SCALE_FAC is DPI scale, U.widget_unit is pixels per UI unit */
      available_width = region->winx / UI_SCALE_FAC / U.widget_unit;

      /* Minimum width needed:
       * - 9 buttons * 1.5 units each = 13.5 units
       * - 1 separator = 0.5 units
       * - 1 color picker = 1.0 units
       * Total = 15 units minimum, but we use 14 as threshold
       */
      use_inline_buttons = (available_width >= MIN_WIDTH_FOR_INLINE_BUTTONS);

#if DEBUG_COLOR_GLYPH_PRESETS
      printf("[COLOR_GLYPH_PRESETS] Region winx: %d pixels\n", region->winx);
      printf("[COLOR_GLYPH_PRESETS] UI_SCALE_FAC: %.2f\n", UI_SCALE_FAC);
      printf("[COLOR_GLYPH_PRESETS] U.widget_unit: %.1f pixels/UI_unit\n", U.widget_unit);
      printf("[COLOR_GLYPH_PRESETS] Available width: %.1f UI units (%.0f / %.2f / %.1f)\n",
             available_width, float(region->winx), UI_SCALE_FAC, U.widget_unit);
      printf("[COLOR_GLYPH_PRESETS] Threshold: %.1f UI units\n", MIN_WIDTH_FOR_INLINE_BUTTONS);
      printf("[COLOR_GLYPH_PRESETS] Comparison: %.1f >= %.1f = %s\n",
             available_width, MIN_WIDTH_FOR_INLINE_BUTTONS,
             use_inline_buttons ? "true" : "false");
      printf("[COLOR_GLYPH_PRESETS] Result: Using %s\n",
             use_inline_buttons ? "inline buttons" : "dropdown menu");
#endif
    }
    else {
#if DEBUG_COLOR_GLYPH_PRESETS
      printf("[COLOR_GLYPH_PRESETS] WARNING: No region available, defaulting to inline\n");
#endif
    }
  }
  else {
#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] WARNING: No context available, defaulting to inline\n");
#endif
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] === Space check complete ===\n");
#endif

  /* Register the popup menu if needed */
  /* Note: No registration needed since we use direct popup callback */

  /* Get tag name from the RNA pointer (CategoryTagDef has a 'name' property) */
  PropertyRNA *name_prop = RNA_struct_find_property(ptr, "name");
  std::string tag_name_str;
  if (name_prop) {
    tag_name_str = RNA_property_string_get(ptr, name_prop);
  }
#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] Tag name from ptr: '%s'\n", tag_name_str.c_str());
#endif

  if (use_inline_buttons) {
    /* INLINE MODE: Show all preset buttons in a row */

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] === INLINE MODE ACTIVATED ===\n");
#endif

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
      printf("[COLOR_GLYPH_PRESETS] ERROR: WM_OT_tag_color_preset operator not found!\n");
      return;
    }

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Creating inline preset buttons...\n");
    printf("[COLOR_GLYPH_PRESETS] Block pointer: %p\n", block);
#endif

    /* Create button for each color preset (9 buttons: NONE + 8 colors) */
    for (int i = 0; i < 9; i++) {
      const int preset = i - 1;  /* -1 to 7 */

#if DEBUG_COLOR_GLYPH_PRESETS
      printf("[COLOR_GLYPH_PRESETS] Creating button %d with preset %d\n", i, preset);
#endif

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

#if DEBUG_COLOR_GLYPH_PRESETS
      printf("[COLOR_GLYPH_PRESETS] Button %d created at %p\n", i, but);
#endif

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
        const ThemeCollectionColor *category_tab_color = &btheme->collection_color[preset];
        button_color_set(but, category_tab_color->color);
        but->drawflag |= BUT_TEXT_USE_COL;
      }

      /* Set operator properties */
      PointerRNA *op_ptr = button_operator_ptr_ensure(but);

#if DEBUG_COLOR_GLYPH_PRESETS
      printf("[COLOR_GLYPH_PRESETS] Setting operator props: tag_name='%s', propname='%s', preset=%d\n",
             tag_name_str.c_str(), propname, preset);
      printf("[COLOR_GLYPH_PRESETS] op_ptr=%p\n", op_ptr);
#endif

      RNA_string_set(op_ptr, "tag_name", tag_name_str.c_str());
      RNA_string_set(op_ptr, "propname", propname);
      RNA_int_set(op_ptr, "preset", preset);

      /* Store RNA data in button */
      but->rnapoin = *ptr;
      but->rnaprop = prop;
      but->rnaindex = 0;

#if DEBUG_COLOR_GLYPH_PRESETS
      printf("[COLOR_GLYPH_PRESETS] Set button RNA: rnapoin.data=%p, rnaprop=%p\n",
             ptr->data, prop);
#endif
    }

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Adding color picker...\n");
#endif

    /* Add separator */
    presets_row.separator();

    /* Add custom color picker for colors not in presets */
    Layout &picker_col = presets_row.column(false);
    picker_col.ui_units_x_set(1.0f);
    picker_col.prop(ptr, propname, ITEM_R_ICON_ONLY, "", ICON_NONE);

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Inline mode complete\n");
    printf("[COLOR_GLYPH_PRESETS] === END INLINE MODE ===\n");
#endif
  }
  else {
    /* DROPDOWN MODE: Show a "..." button that opens a popup with all presets */

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] === DROPDOWN MODE ACTIVATED ===\n");
#endif

    /* Create a row with the dropdown button and color picker */
    Layout &main_row = layout->row(true);
    main_row.alignment_set(LayoutAlign::Center);

    /* Store RNA data in static storage for popup callback to access */
    color_glyph_popup_data.is_set = true;
    color_glyph_popup_data.ptr = *ptr;
    color_glyph_popup_data.prop = prop;
    BLI_strncpy(color_glyph_popup_data.propname, propname, sizeof(color_glyph_popup_data.propname));

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Stored static data: ptr=%p, prop=%p, propname='%s'\n",
           ptr, prop, propname);
    printf("[COLOR_GLYPH_PRESETS] Creating popup button...\n");
#endif

    /* Create "..." button that opens popup */
    Block *block = main_row.block();
    block_layout_set_current(block, &main_row);

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Block pointer: %p\n", block);
#endif

    Button *popup_but = uiDefBlockButN(
        block,
        (BlockCreateFunc)color_glyph_preset_popup_callback,
        nullptr,
        IFACE_("Color Presets..."),
        0,
        0,
        int(UI_UNIT_X * 6.0f),
        UI_UNIT_Y,
        std::nullopt,
        nullptr,
        nullptr);

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Popup button created at %p\n", popup_but);
#endif

    /* Store RNA data in the button for future use */
    popup_but->rnapoin = *ptr;
    popup_but->rnaprop = prop;
    popup_but->rnaindex = 0;

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Set button RNA: rnapoin.data=%p, rnaprop=%p\n",
           ptr->data, prop);
#endif

    /* Add color picker next to dropdown */
    Layout &picker_col = main_row.column(false);
    picker_col.ui_units_x_set(1.0f);
    picker_col.prop(ptr, propname, ITEM_R_ICON_ONLY, "", ICON_NONE);

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Dropdown mode setup complete\n");
    printf("[COLOR_GLYPH_PRESETS] === END DROPDOWN MODE ===\n");
#endif
  }
}

}  // namespace blender::ui
