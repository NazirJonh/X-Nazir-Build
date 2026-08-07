/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * One-handle gradient value slider for material-paint scalar channels.
 */

#include "BKE_library.hh"

#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "interface_intern.hh"
#include "interface_templates_intern.hh"

namespace blender::ui {

void template_material_paint_value_slider(Layout *layout,
                                          PointerRNA *ptr,
                                          const StringRefNull propname,
                                          const int index)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop || RNA_property_type(prop) != PROP_FLOAT) {
    return;
  }

  if (RNA_property_array_check(prop)) {
    const int length = RNA_property_array_length(ptr, prop);
    if (index < 0 || index >= length) {
      return;
    }
  }
  else if (index != 0) {
    return;
  }

  Layout &row = layout->row(true);
  Block *block = row.block();

  ID *id = ptr->owner_id;
  block_lock_set(block, (id && !ID_IS_EDITABLE(id)), ERROR_LIBDATA_MESSAGE);

  /* Width is filled by the layout; height matches a normal slider row. */
  uiDefButR_prop(block,
                 ButtonType::MaterialPaintValue,
                 "",
                 0,
                 0,
                 UI_UNIT_X * 10,
                 UI_UNIT_Y,
                 ptr,
                 prop,
                 index,
                 0.0f,
                 0.0f,
                 TIP_("Paint value"));

  block_lock_clear(block);
}

}  // namespace blender::ui
