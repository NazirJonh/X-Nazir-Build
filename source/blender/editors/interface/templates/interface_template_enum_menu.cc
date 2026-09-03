/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <algorithm>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "interface_intern.hh"

namespace blender::ui {

/* RNA enum stepping wraps around by default. Keep this selector on its first/last item instead.
 * Domain-specific step functions with their own lists (image slots/layers/passes,
 * #grid_library_selector_menu_step) are untouched; this is the generic clamped one. */
static int enum_menu_step_clamped(bContext *C, const int direction, Button *but)
{
  PointerRNA ptr = but->rnapoin;
  PropertyRNA *prop = but->rnaprop;
  const int current_value = RNA_property_enum_get(&ptr, prop);

  const EnumPropertyItem *items = nullptr;
  bool free = false;
  RNA_property_enum_items_gettexted(C, &ptr, prop, &items, nullptr, &free);
  if (!items) {
    return current_value;
  }

  Vector<int> values;
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (item->identifier[0] != '\0') {
      values.append(item->value);
    }
  }
  if (free) {
    MEM_delete(items);
  }

  if (values.is_empty()) {
    return current_value;
  }

  const int current_index = values.first_index_of_try(current_value);
  if (current_index < 0) {
    return current_value;
  }

  const int next_index = std::clamp(current_index + direction, 0, int(values.size()) - 1);
  return values[next_index];
}

void template_enum_menu(Layout *layout,
                        PointerRNA *ptr,
                        const StringRefNull propname,
                        const float width_units,
                        const bool wrap)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop) {
    return;
  }

  Block *block = layout->block();
  block_layout_set_current(block, layout);
  /* Keep the selector width independent of the current item's name and count. */
  Button *but = uiDefButR_prop(block,
                               ButtonType::Menu,
                               std::nullopt,
                               0,
                               0,
                               UI_UNIT_X * ((width_units > 0.0f) ? width_units : 6.0f),
                               UI_UNIT_Y,
                               ptr,
                               prop,
                               -1,
                               0,
                               0,
                               std::nullopt);
  if (!wrap) {
    button_func_menu_step_set(but, enum_menu_step_clamped);
  }
}

}  // namespace blender::ui
