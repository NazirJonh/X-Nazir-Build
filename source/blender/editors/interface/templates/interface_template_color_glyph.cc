/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BLI_string_ref.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "interface_intern.hh"

#include "WM_api.hh"

namespace blender::ui {

void uiTemplateColorGlyphPresets(Layout *layout,
                                  bContext * /*C*/,
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
    /* Operator not registered, fall back to standard color picker */
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
      const ThemeCollectionColor *category_tab_color = &btheme->collection_color[preset];
      button_color_set(but, category_tab_color->color);
      but->drawflag |= BUT_TEXT_USE_COL;
    }

    /* Set operator properties */
    PointerRNA *op_ptr = button_operator_ptr_ensure(but);

    /* Get tag name from the RNA pointer (CategoryTagDef has a 'name' property) */
    PropertyRNA *name_prop = RNA_struct_find_property(ptr, "name");
    std::string tag_name_str;
    if (name_prop) {
      tag_name_str = RNA_property_string_get(ptr, name_prop);
    }

    RNA_string_set(op_ptr, "tag_name", tag_name_str.c_str());
    RNA_string_set(op_ptr, "propname", propname);
    RNA_int_set(op_ptr, "preset", preset);
  }

  /* Add separator */
  presets_row.separator();

  /* Add custom color picker for colors not in presets */
  Layout &picker_col = presets_row.column(false);
  picker_col.ui_units_x_set(1.0f);
  picker_col.prop(ptr, propname, ITEM_R_ICON_ONLY, "", ICON_NONE);
}

}  // namespace blender::ui
