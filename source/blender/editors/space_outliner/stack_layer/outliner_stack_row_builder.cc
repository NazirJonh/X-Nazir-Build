/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The C half of the OutlinerStackRowBuilder RNA type: what a Python #rows_build call drives.
 * Each function fills the plain #StackRow model the generic draw reads -- no translation happens
 * past these entry points.
 */

#include <utility>

#include "BLI_vector.hh"

#include "DNA_space_types.h"

#include "ED_outliner_stack.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "outliner_stack_source.hh"
#include "outliner_stack_source_python.hh"

namespace blender {

/* The generated code declares and calls these unqualified from this namespace, the one every
 * RNA callback lives in. */
using namespace ed::outliner;

int outliner_stack_row_builder_add_row(StackRowBuilderData *builder,
                                       const char *name,
                                       const char *icon_name,
                                       int glyph,
                                       int depth,
                                       int parent_ordinal,
                                       bool can_hold_children,
                                       bool has_children,
                                       bool enabled,
                                       bool locked,
                                       bool aux_enabled,
                                       int color_tag,
                                       bool compact,
                                       const char *unsupported_reason,
                                       PointerRNA *value,
                                       const char *value_prop,
                                       PointerRNA *mode,
                                       const char *mode_prop,
                                       bool value_inherited,
                                       bool mode_inherited)
{
  if (builder == nullptr || builder->rows_vector == nullptr) {
    /* Not a row-building call: there is no vector of rows to append to. */
    return -1;
  }
  auto *source = static_cast<StackSourcePython *>(builder->source);
  if (source == nullptr) {
    return -1;
  }
  auto &rows = *static_cast<Vector<StackRow> *>(builder->rows_vector);
  if (rows.size() > STACK_ROW_ORDINAL_MAX) {
    return -1;
  }

  StackRow row;
  row.ordinal = int16_t(rows.size());
  row.depth = depth > 0 ? depth : 0;
  row.can_hold_children = can_hold_children;
  row.has_children = has_children && can_hold_children;
  row.enabled = enabled;
  row.locked = locked;
  row.aux_enabled = aux_enabled;
  row.color_tag = int8_t((color_tag >= 0 && color_tag <= STACK_ROW_COLOR_NUM) ? color_tag : 0);
  row.compact = compact;
  row.name = (name != nullptr) ? name : "";
  row.icon = 0;
  if (icon_name != nullptr && icon_name[0] != '\0') {
    int icon = 0;
    if (RNA_enum_value_from_id(rna_enum_icon_items, icon_name, &icon)) {
      row.icon = icon;
    }
  }
  row.glyph = uint32_t((glyph > 0) ? glyph : 0);
  if (unsupported_reason != nullptr && unsupported_reason[0] != '\0') {
    row.supported = false;
    row.unsupported_reason = source->string_intern(unsupported_reason);
  }

  /* A parent may still be ahead of this row: rows list top first, so a folder follows the rows it
   * holds in the vector. The Python source validates the forward reference once the whole build is
   * in and clears a dangling one there; a parent already in the vector keeps the old rule that it
   * has to be able to hold children. */
  row.parent_ordinal = -1;
  if (parent_ordinal >= 0 && parent_ordinal <= STACK_ROW_ORDINAL_MAX &&
      parent_ordinal != row.ordinal)
  {
    if (parent_ordinal >= rows.size()) {
      row.parent_ordinal = int16_t(parent_ordinal);
    }
    else if (rows[parent_ordinal].can_hold_children) {
      row.parent_ordinal = int16_t(parent_ordinal);
      rows[parent_ordinal].has_children = true;
    }
  }

  if (value != nullptr && value_prop != nullptr && value_prop[0] != '\0' &&
      value->data != nullptr)
  {
    row.value_ptr = *value;
    row.value_prop = source->string_intern(value_prop);
  }
  if (mode != nullptr && mode_prop != nullptr && mode_prop[0] != '\0' && mode->data != nullptr) {
    row.mode_ptr = *mode;
    row.mode_prop = source->string_intern(mode_prop);
  }
  row.value_inherited = value_inherited;
  row.mode_inherited = mode_inherited;

  rows.append(std::move(row));
  builder->current_row = &rows.last();
  return rows.last().ordinal;
}

}  // namespace blender
