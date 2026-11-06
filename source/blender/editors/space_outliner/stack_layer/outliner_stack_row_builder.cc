/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The C half of the OutlinerStackRowBuilder RNA type: what a Python #rows_build or
 * #decorate_row call drives. Each function fills the plain #StackRow model the generic draw
 * reads -- no translation happens past these entry points.
 */

#include <algorithm>
#include <utility>

#include "BLI_vector.hh"

#include "DNA_space_types.h"

#include "ED_outliner_stack.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "outliner_stack_source.hh"
#include "outliner_stack_source_python.hh"

namespace blender::ed::outliner {

namespace {

/** The row \a ordinal names in the vector a build is filling, or the row a decorator is editing;
 * null when there is no such row. -1 names the row just added, which is how a build addresses
 * the row #add_row handed back without remembering the number. */
StackRow *stack_row_builder_row_get(StackRowBuilderData &builder, const int row_ordinal)
{
  if (builder.rows_vector == nullptr) {
    /* Decorator mode: the builder stands for exactly one row. */
    return static_cast<StackRow *>(builder.current_row);
  }
  auto &rows = *static_cast<Vector<StackRow> *>(builder.rows_vector);
  if (row_ordinal < 0) {
    return static_cast<StackRow *>(builder.current_row);
  }
  /* Rows are appended in ordinal order, so a backwards walk finds a recent ordinal fast and
   * rejects a future one without touching the whole vector. */
  if (row_ordinal >= rows.size()) {
    return nullptr;
  }
  return &rows[row_ordinal];
}

/** The operator properties every slot's operator carries: the row it was clicked on, in the same
 * terms the built-in row operators speak -- the ordinal, the row's hashed identity, and, when it
 * has one, the readable stable key the source can resolve the row by without any per-space state.
 *
 * The group's own name is not a key anywhere (it never sits inside a parent group), so it only
 * labels the group in debugging output; it reads as the namespace the operator properties it is
 * merged into live under. */
OwnedIDProperty stack_row_button_properties_create(const StackRow &row)
{
  IDPropertyTemplate val{};
  OwnedIDProperty group(IDP_New(IDP_GROUP, &val, "properties"));
  if (group == nullptr) {
    return nullptr;
  }

  IDP_AddToGroup(group.get(), IDP_NewInt(row.ordinal, "ordinal"));

  if (!BLI_uuid_is_nil(row.stable_id)) {
    char marker[UUID_STRING_SIZE];
    BLI_uuid_format(marker, row.stable_id);
    IDP_AddToGroup(group.get(), IDP_NewString(marker, "marker"));
  }
  if (row.stable_key != nullptr && row.stable_key[0] != '\0') {
    IDP_AddToGroup(group.get(), IDP_NewString(row.stable_key, "stable_key"));
  }
  return group;
}

}  // namespace

}  // namespace blender::ed::outliner

namespace blender {

/* The generated code declares and calls these unqualified from this namespace, the one every
 * RNA callback lives in; the module's own vocabulary stays in blender::ed::outliner above. */
using namespace ed::outliner;

int outliner_stack_row_builder_add_row(StackRowBuilderData *builder, const StackRowAddArgs &args)
{
  if (builder == nullptr || builder->rows_vector == nullptr) {
    /* Decorator mode: a decorator adds buttons, never rows. */
    return -1;
  }
  auto *source = static_cast<StackSourcePython *>(builder->source);
  if (source == nullptr) {
    return -1;
  }
  const char *name = args.name;
  const char *icon_name = args.icon_name;
  const int depth = args.depth;
  const char *stable_key = args.stable_key;
  const char *unsupported_reason = args.unsupported_reason;
  auto &rows = *static_cast<Vector<StackRow> *>(builder->rows_vector);
  if (rows.size() > STACK_ROW_ORDINAL_MAX) {
    return -1;
  }

  StackRow row;
  row.ordinal = int16_t(rows.size());
  row.depth = depth > 0 ? depth : 0;
  row.can_hold_children = args.can_hold_children;
  row.has_children = args.has_children && args.can_hold_children;
  row.enabled = args.enabled;
  row.locked = args.locked;
  row.aux_enabled = args.aux_enabled;
  row.color_tag = int8_t((args.color_tag >= 0 && args.color_tag <= STACK_ROW_COLOR_NUM) ?
                             args.color_tag :
                             0);
  row.compact = args.compact;
  row.name = (name != nullptr) ? name : "";
  row.icon = 0;
  if (icon_name != nullptr && icon_name[0] != '\0') {
    int icon = 0;
    if (RNA_enum_value_from_id(rna_enum_icon_items, icon_name, &icon)) {
      row.icon = icon;
    }
  }
  row.glyph = uint32_t((args.glyph > 0) ? args.glyph : 0);
  if (unsupported_reason != nullptr && unsupported_reason[0] != '\0') {
    row.supported = false;
    row.unsupported_reason = source->string_intern(unsupported_reason);
  }

  /* Only a keyed row has an identity to carry across rebuilds and storage for its own name. */
  if (stable_key != nullptr && stable_key[0] != '\0') {
    row.stable_id = StackSourcePython::stable_id_from_key(stable_key);
    row.stable_key = source->string_intern(stable_key);
    row.name_buffer = source->name_buffer_ensure(stable_key, row.name);
  }

  /* A parent may still be ahead of this row: rows list top first, so a folder follows the rows it
   * holds in the vector. The Python source validates the forward reference once the whole build is
   * in and clears a dangling one there; a parent already in the vector keeps the old rule that it
   * has to be able to hold children. */
  row.parent_ordinal = -1;
  if (args.parent_ordinal >= 0 && args.parent_ordinal <= STACK_ROW_ORDINAL_MAX &&
      args.parent_ordinal != row.ordinal)
  {
    if (args.parent_ordinal >= rows.size()) {
      row.parent_ordinal = int16_t(args.parent_ordinal);
    }
    else if (rows[args.parent_ordinal].can_hold_children) {
      row.parent_ordinal = int16_t(args.parent_ordinal);
      rows[args.parent_ordinal].has_children = true;
    }
  }

  if (args.value != nullptr && args.value_prop != nullptr && args.value_prop[0] != '\0' &&
      args.value->data != nullptr)
  {
    row.value_ptr = *args.value;
    row.value_prop = source->string_intern(args.value_prop);
  }
  if (args.mode != nullptr && args.mode_prop != nullptr && args.mode_prop[0] != '\0' &&
      args.mode->data != nullptr)
  {
    row.mode_ptr = *args.mode;
    row.mode_prop = source->string_intern(args.mode_prop);
  }
  row.value_inherited = args.value_inherited;
  row.mode_inherited = args.mode_inherited;

  rows.append(std::move(row));
  builder->current_row = &rows.last();
  return rows.last().ordinal;
}

bool outliner_stack_row_builder_button_add(StackRowBuilderData *builder,
                                           int row_ordinal,
                                           const char *text,
                                           const char *tooltip,
                                           const char *icon_name,
                                           int glyph,
                                           const char *operator_idname,
                                           const char *menu_idname,
                                           bool pressed,
                                           bool enabled,
                                           bool before_value,
                                           bool hover_only,
                                           const char *int_prop,
                                           int int_value)
{
  if (builder == nullptr) {
    return false;
  }
  StackRow *row = stack_row_builder_row_get(*builder, row_ordinal);
  if (row == nullptr) {
    return false;
  }
  const bool has_operator = (operator_idname != nullptr && operator_idname[0] != '\0');
  const bool has_menu = (menu_idname != nullptr && menu_idname[0] != '\0');
  if (!has_operator && !has_menu) {
    return false;
  }

  StackRowButton button;
  if (icon_name != nullptr && icon_name[0] != '\0') {
    int icon = 0;
    if (RNA_enum_value_from_id(rna_enum_icon_items, icon_name, &icon)) {
      button.icon = icon;
    }
  }
  button.glyph = uint32_t((glyph > 0) ? glyph : 0);
  button.text = (text != nullptr) ? text : "";
  button.tooltip = (tooltip != nullptr) ? tooltip : "";
  button.operator_idname = has_operator ? operator_idname : "";
  button.menu_idname = has_menu ? menu_idname : "";
  button.pressed = pressed;
  button.enabled = enabled;
  button.before_value = before_value;
  button.hover_only = hover_only;
  /* The row identity the operator resolves the row by is not stored on the button: the draw reads
   * it off the row the button hangs on, where it is current. Only the one extra integer the
   * source named travels with the button. */
  button.int_prop = (int_prop != nullptr) ? int_prop : "";
  button.int_value = int_value;

  row->buttons.append(std::move(button));
  return true;
}

bool outliner_stack_row_builder_preview_slot_add(StackRowBuilderData *builder,
                                                 int row_ordinal,
                                                 const char *icon_name,
                                                 int glyph,
                                                 const char *label,
                                                 const char *tooltip,
                                                 const char *operator_idname,
                                                 const char *section_id,
                                                 bool enabled,
                                                 bool keeps_row_icon)
{
  if (builder == nullptr) {
    return false;
  }
  StackRow *row = stack_row_builder_row_get(*builder, row_ordinal);
  if (row == nullptr) {
    return false;
  }

  StackRowPreview slot;
  if (icon_name != nullptr && icon_name[0] != '\0') {
    int icon = 0;
    if (RNA_enum_value_from_id(rna_enum_icon_items, icon_name, &icon)) {
      slot.icon = icon;
    }
  }
  slot.glyph = uint32_t((glyph > 0) ? glyph : 0);
  slot.label = (label != nullptr) ? label : "";
  slot.tooltip = (tooltip != nullptr) ? tooltip : "";
  slot.enabled = enabled;
  slot.keeps_row_icon = keeps_row_icon;
  if (operator_idname != nullptr && operator_idname[0] != '\0') {
    slot.operator_idname = operator_idname;
    slot.properties = stack_row_button_properties_create(*row);
  }

  if (section_id != nullptr && section_id[0] != '\0') {
    /* The section is what frames the slot while it is the one showing; a Python source has no
     * content to put in it, so the section is made empty here and the first slot's is the row's
     * default. */
    slot.section_id = section_id;
    const bool section_exists = std::any_of(
        row->content_sections.begin(),
        row->content_sections.end(),
        [&](const StackContentSection &section) { return section.identifier == section_id; });
    if (!section_exists) {
      StackContentSection section;
      section.identifier = section_id;
      section.name = slot.label;
      row->content_sections.append(std::move(section));
    }
  }

  row->preview_slots.append(std::move(slot));
  return true;
}

}  // namespace blender
