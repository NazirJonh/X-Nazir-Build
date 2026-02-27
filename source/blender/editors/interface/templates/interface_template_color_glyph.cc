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
#include "BLI_string_ref.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "interface_intern.hh"

#include "WM_api.hh"

#define DEBUG_COLOR_GLYPH_PRESETS 0

namespace blender::ui {

void uiTemplateColorGlyphPresets(Layout *layout,
                                  bContext * /*C*/,
                                  PointerRNA *ptr,
                                  const char *propname)
{
#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] uiTemplateColorGlyphPresets called\n");
  printf("[COLOR_GLYPH_PRESETS] ptr->type->name: %s\n", ptr->type ? ptr->type->identifier : "NULL");
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
    /* Operator not registered, fall back to standard color picker */
    return;
  }

#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] Creating preset buttons...\n");
#endif

  /* Get tag name from the RNA pointer (CategoryTagDef has a 'name' property) */
  PropertyRNA *name_prop = RNA_struct_find_property(ptr, "name");
  std::string tag_name_str;
  if (name_prop) {
    tag_name_str = RNA_property_string_get(ptr, name_prop);
  }
#if DEBUG_COLOR_GLYPH_PRESETS
  printf("[COLOR_GLYPH_PRESETS] Tag name from ptr: '%s'\n", tag_name_str.c_str());
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
#endif

    RNA_string_set(op_ptr, "tag_name", tag_name_str.c_str());
    RNA_string_set(op_ptr, "propname", propname);
    RNA_int_set(op_ptr, "preset", preset);

    /* CRITICAL: Store RNA data in button so operator can access it directly.
     * This allows the operator to work with ANY RNA pointer (not just existing tags).
     * The operator can use but->rnapoin and but->rnaprop to modify the property directly,
     * without needing to search for the tag in wm->category_tags by name.
     *
     * This is similar to how uiDefButR() works for RNA property buttons.
     */
    but->rnapoin = *ptr;
    but->rnaprop = prop;
    but->rnaindex = 0;

#if DEBUG_COLOR_GLYPH_PRESETS
    printf("[COLOR_GLYPH_PRESETS] Set button RNA data: rnapoin.data=%p, rnaprop=%p\n",
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
}

}  // namespace blender::ui
