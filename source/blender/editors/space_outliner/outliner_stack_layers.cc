/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Navigation and activation for the Stack Layers display mode.
 *
 * Everything here is phrased in rows and ordinals. What a row *is* -- a paint layer, a shape key,
 * whatever comes next -- is the business of the #StackSource the space is set to, so the operators
 * below never mention a material, an image or a node.
 */

#include <algorithm>
#include <climits>

#include "DNA_defs.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "ED_object.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender::ed::outliner {

namespace {

bool stack_row_activate_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->is_editable(*owner);
}

wmOperatorStatus stack_focus_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Main *bmain = CTX_data_main(C);
  char object_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "object", object_name);
  Object *object = id_cast<Object *>(BKE_libblock_find_name(bmain, ID_OB, object_name));
  if (object == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Object not found");
    return OPERATOR_CANCELLED;
  }
  const int sub_index = RNA_int_get(op->ptr, "sub_index");
  const bool enter_paint_mode = RNA_boolean_get(op->ptr, "enter_paint_mode");
  return outliner_stack_focus_set(C, *space_outliner, *object, sub_index, enter_paint_mode) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

wmOperatorStatus stack_back_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_pin_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  space_outliner->stack_layers_flag ^= SO_SL_PINNED;
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_activate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = RNA_int_get(op->ptr, "ordinal");
  return outliner_stack_row_activate(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
}

/** Ordinals of the stack rows the user has selected, bottom-up. Empty when none are. */
void stack_selected_ordinals_get(SpaceOutliner &space_outliner, Vector<int> &r_ordinals)
{
  r_ordinals.clear();
  tree_iterator::all(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && (tselem->flag & TSE_SELECTED)) {
      r_ordinals.append_non_duplicates(int(tselem->nr));
    }
  });
  std::sort(r_ordinals.begin(), r_ordinals.end());
}

/**
 * The ordinal the operator was given, or the row the user is pointing at when it was left at -1.
 *
 * Selection comes first: clicking a row is how a layer manager says "this one", and it is what the
 * context menu and the header buttons act on. The activated row -- the one the paint tools write
 * into -- is the fallback, since a stack can be worked on without anything being selected.
 */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op)
{
  const int ordinal = RNA_int_get(op.ptr, "ordinal");
  if (ordinal >= 0) {
    return ordinal;
  }
  Vector<int> selected;
  stack_selected_ordinals_get(space_outliner, selected);
  if (!selected.is_empty()) {
    return selected.first();
  }
  return outliner_stack_active_ordinal_get(outliner_stack_read_context(C), space_outliner);
}

wmOperatorStatus stack_row_move_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int from_ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (from_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  int to_ordinal = RNA_int_get(op->ptr, "to_ordinal");
  if (to_ordinal < 0) {
    /* "Up" is towards the top of the stack, which is the end of the array: the Outliner lists a
     * stack the way a layer manager does, top row first, and the operator speaks that language. */
    to_ordinal = from_ordinal + (RNA_enum_get(op->ptr, "direction") == 0 ? 1 : -1);
  }
  return outliner_stack_row_reorder(C, *space_outliner, from_ordinal, to_ordinal) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_remove_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  return (ordinal >= 0 && outliner_stack_row_remove(C, *space_outliner, ordinal)) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackAddKind kind = StackAddKind(RNA_enum_get(op->ptr, "type"));
  const int ordinal = RNA_int_get(op->ptr, "ordinal");
  return outliner_stack_row_add(C, *space_outliner, kind, ordinal) >= 0 ? OPERATOR_FINISHED :
                                                                         OPERATOR_CANCELLED;
}

bool stack_row_add_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_add(*owner);
}

wmOperatorStatus stack_row_visibility_toggle_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackRow *row = (ordinal < 0) ? nullptr :
                                        outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }
  return outliner_stack_row_set_enabled(C, *space_outliner, ordinal, !row->enabled) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

bool stack_row_visibility_toggle_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_set_enabled(*owner);
}

/**
 * The sub-selections of the focused object, as enum items.
 *
 * Built from the source's names, so the Outliner offers "which material slot" without knowing that
 * a slot is what a paint stack's sub-index means.
 */
const EnumPropertyItem *stack_sub_index_itemf(bContext *C,
                                              PointerRNA * /*ptr*/,
                                              PropertyRNA * /*prop*/,
                                              bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  SpaceOutliner *space_outliner = (C == nullptr) ? nullptr : CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    Vector<std::string> names;
    stack_source_for_space(*space_outliner)
        ->sub_selection_names(ctx, space_outliner->runtime->stack_focus, names);
    for (const int index : names.index_range()) {
      /* The identifier has to survive the item array, and a slot number is stable while the name
       * is not; the name is what the user reads. */
      char identifier[16];
      SNPRINTF_UTF8(identifier, "%d", index);
      EnumPropertyItem item = {};
      item.value = index;
      item.identifier = BLI_strdup(identifier);
      item.name = BLI_strdup(names[index].empty() ? IFACE_("Empty Slot") : names[index].c_str());
      item.description = "";
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

wmOperatorStatus stack_sub_index_set_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Object *object = space_outliner->runtime->stack_focus.object;
  if (object == nullptr) {
    return OPERATOR_CANCELLED;
  }
  return outliner_stack_focus_set(
             C, *space_outliner, *object, RNA_enum_get(op->ptr, "sub_index"), false) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

bool stack_sub_index_set_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->runtime != nullptr &&
         space_outliner->runtime->stack_focus.object != nullptr;
}

wmOperatorStatus stack_rows_group_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  int from_ordinal = ordinal;
  int to_ordinal = RNA_int_get(op->ptr, "to_ordinal");
  if (to_ordinal < 0) {
    /* The selection is the run to wrap. One row on its own is a group of one, which is what "put
     * this in a folder" means when nothing else is picked. */
    Vector<int> selected;
    stack_selected_ordinals_get(*space_outliner, selected);
    if (selected.size() > 1) {
      from_ordinal = selected.first();
      to_ordinal = selected.last();
      if (to_ordinal - from_ordinal + 1 != selected.size()) {
        BKE_report(op->reports, RPT_ERROR, "Only layers next to each other can be grouped");
        return OPERATOR_CANCELLED;
      }
    }
    else {
      to_ordinal = from_ordinal;
    }
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int group_ordinal = stack_source_for_space(*space_outliner)
                                ->rows_group(*C,
                                             space_outliner->runtime->stack_focus,
                                             *owner,
                                             from_ordinal,
                                             to_ordinal);
  if (group_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_invalidate(*space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_group_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* -1 means the top of the stack, which is where a folder goes when nothing is pointed at. */
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int group_ordinal = stack_source_for_space(*space_outliner)
                                ->group_add(
                                    *C, space_outliner->runtime->stack_focus, *owner, ordinal);
  if (group_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_invalidate(*space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_ungroup_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (stack_source_for_space(*space_outliner)
          ->row_ungroup(*C, space_outliner->runtime->stack_focus, *owner, ordinal) < 0)
  {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_invalidate(*space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_duplicate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackSource &source = *stack_source_for_space(*space_outliner);
  const int new_ordinal = source.row_duplicate(
      *C, space_outliner->runtime->stack_focus, *owner, ordinal);
  if (new_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_invalidate(*space_outliner);
  outliner_stack_row_activate(C, *space_outliner, new_ordinal);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_rename_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "A layer needs a name");
    return OPERATOR_CANCELLED;
  }
  if (!stack_source_for_space(*space_outliner)
           ->row_rename(*C, space_outliner->runtime->stack_focus, *owner, ordinal, name))
  {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_invalidate(*space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_rename_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackRow *row = (ordinal < 0) ? nullptr :
                                        outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* Start from the current name: a rename dialog that opens empty is a delete-and-retype. */
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "name");
  if (!RNA_property_is_set(op->ptr, prop)) {
    RNA_property_string_set(op->ptr, prop, row->name.c_str());
  }
  return WM_operator_props_popup_confirm(C, op, nullptr);
}

wmOperatorStatus stack_row_mask_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackSource &source = *stack_source_for_space(*space_outliner);
  if (!source.can_mask(*owner) ||
      !source.row_mask_set(*C,
                           space_outliner->runtime->stack_focus,
                           *owner,
                           ordinal,
                           RNA_boolean_get(op->ptr, "add")))
  {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_invalidate(*space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

bool stack_row_mask_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_mask(*owner);
}

wmOperatorStatus stack_row_show_map_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const int role = RNA_int_get(op->ptr, "role");
  return outliner_stack_sub_row_activate(
             C, *space_outliner, ordinal * STACK_ROW_SUB_ROW_STRIDE + role) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

bool stack_row_reorder_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->runtime != nullptr &&
         outliner_stack_can_reorder(*C, *space_outliner);
}

wmOperatorStatus stack_target_clear_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return stack_source_for_space(*space_outliner)->target_clear(*C) ? OPERATOR_FINISHED :
                                                                     OPERATOR_CANCELLED;
}

}  // namespace

StackReadContext outliner_stack_read_context(const bContext &C)
{
  bContext &context = const_cast<bContext &>(C);
  StackReadContext ctx;
  ctx.bmain = CTX_data_main(&context);
  ctx.scene = CTX_data_scene(&context);
  ctx.view_layer = CTX_data_view_layer(&context);
  return ctx;
}

void outliner_stack_rows_invalidate(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  runtime.stack_rows.clear();
  runtime.stack_owner = nullptr;
  runtime.stack_state_hash = 0;
  runtime.stack_rows_valid = false;
}

void outliner_stack_rows_ensure(const StackReadContext &ctx,
                                SpaceOutliner &space_outliner,
                                ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  const StackSource &source = *stack_source_for_space(space_outliner);
  const uint64_t state_hash = source.state_hash(ctx, owner);
  if (runtime.stack_rows_valid && runtime.stack_owner == &owner &&
      runtime.stack_state_hash == state_hash)
  {
    return;
  }
  runtime.stack_rows.clear();
  source.rows_build(ctx, runtime.stack_focus, owner, runtime.stack_rows);
  runtime.stack_owner = &owner;
  runtime.stack_state_hash = state_hash;
  runtime.stack_rows_valid = true;
}

const StackRow *outliner_stack_row_find(const SpaceOutliner &space_outliner, const int ordinal)
{
  for (const StackRow &row : space_outliner.runtime->stack_rows) {
    if (row.ordinal == ordinal) {
      return &row;
    }
  }
  return nullptr;
}

ID *outliner_stack_owner_get(const StackReadContext &ctx, const SpaceOutliner &space_outliner)
{
  StackFocus focus = space_outliner.runtime->stack_focus;
  if (focus.object == nullptr) {
    /* Resolved at use rather than stored: the active object can be freed under a stored pointer by
     * an undo step, a file load or a delete. */
    if (ctx.bmain == nullptr || ctx.view_layer == nullptr) {
      return nullptr;
    }
    BKE_view_layer_synced_ensure(*ctx.bmain, ctx.scene, ctx.view_layer);
    focus.object = BKE_view_layer_active_object_get(ctx.view_layer);
  }
  if (focus.object == nullptr) {
    return nullptr;
  }
  return stack_source_for_space(space_outliner)->owner_get(ctx, focus);
}

bool outliner_stack_focus_set(bContext *C,
                              SpaceOutliner &space_outliner,
                              Object &object,
                              const int sub_index,
                              const bool enter_paint_mode)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Base *base = BKE_view_layer_base_find(view_layer, &object);
  if (base == nullptr) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Object is not in the active view layer");
    return false;
  }
  if (sub_index < -1 || sub_index >= object.totcol) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
    return false;
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  runtime.stack_focus.object = &object;
  runtime.stack_focus.sub_index = sub_index;
  outliner_stack_rows_invalidate(space_outliner);
  space_outliner.outlinevis = SO_STACK_LAYERS;
  space_outliner.stack_layers_view = SO_SL_VIEW_STACK;

  if (enter_paint_mode) {
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    if (!object::mode_set(C, OB_MODE_TEXTURE_PAINT)) {
      BKE_report(CTX_wm_reports(C), RPT_WARNING, "Could not enter Texture Paint mode");
    }
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  ED_area_tag_refresh(CTX_wm_area(C));
  return true;
}

bool outliner_stack_row_activate(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  return stack_source_for_space(space_outliner)
      ->row_activate(*C, space_outliner.runtime->stack_focus, *owner, ordinal, *row);
}

bool outliner_stack_sub_row_activate(bContext *C, SpaceOutliner &space_outliner, const int nr)
{
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackRow *row = outliner_stack_row_find(space_outliner, nr / STACK_ROW_SUB_ROW_STRIDE);
  if (row == nullptr) {
    return false;
  }
  const int role = nr % STACK_ROW_SUB_ROW_STRIDE;
  for (const StackSubRow &sub_row : row->sub_rows) {
    if (sub_row.role == role) {
      return stack_source_for_space(space_outliner)
          ->sub_row_activate(*C, space_outliner.runtime->stack_focus, *owner, *row, sub_row);
    }
  }
  return false;
}

int outliner_stack_active_ordinal_get(const StackReadContext &ctx,
                                      SpaceOutliner &space_outliner)
{
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  /* The rows may never have been built in this space: a header button is reachable before the
   * tree has been drawn once. */
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackSource &source = *stack_source_for_space(space_outliner);
  for (const StackRow &row : space_outliner.runtime->stack_rows) {
    if (source.row_is_active(ctx, space_outliner.runtime->stack_focus, *owner, row)) {
      return row.ordinal;
    }
  }
  return -1;
}

bool outliner_stack_row_is_active(const StackReadContext &ctx,
                                  const SpaceOutliner &space_outliner,
                                  const int ordinal)
{
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (row == nullptr || owner == nullptr) {
    return false;
  }
  return stack_source_for_space(space_outliner)
      ->row_is_active(ctx, space_outliner.runtime->stack_focus, *owner, *row);
}

bool outliner_stack_can_reorder(const bContext &C, const SpaceOutliner &space_outliner)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  const ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  return owner != nullptr && stack_source_for_space(space_outliner)->can_reorder(*owner);
}

bool outliner_stack_row_reorder(bContext *C,
                                SpaceOutliner &space_outliner,
                                const int from_ordinal,
                                const int to_ordinal)
{
  if (from_ordinal == to_ordinal || from_ordinal < 0 || to_ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (!source.can_reorder(*owner)) {
    return false;
  }
  if (!source.row_reorder(
          *C, space_outliner.runtime->stack_focus, *owner, from_ordinal, to_ordinal))
  {
    return false;
  }
  /* Ordinals are positions, so every row past the move has a new one. */
  outliner_stack_rows_invalidate(space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return true;
}

bool outliner_stack_row_move(bContext *C,
                             SpaceOutliner &space_outliner,
                             const int from_ordinal,
                             const int anchor_ordinal,
                             const StackMovePlace place)
{
  if (from_ordinal < 0 || anchor_ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (!source.can_reorder(*owner)) {
    return false;
  }
  if (!source.row_move(*C,
                       space_outliner.runtime->stack_focus,
                       *owner,
                       from_ordinal,
                       anchor_ordinal,
                       place))
  {
    return false;
  }
  /* Ordinals are positions, so every row past the move has a new one. */
  outliner_stack_rows_invalidate(space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return true;
}

int outliner_stack_row_add(bContext *C,
                           SpaceOutliner &space_outliner,
                           const StackAddKind kind,
                           const int ordinal)
{
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (!source.can_add(*owner)) {
    return -1;
  }
  const int new_ordinal = source.row_add(
      *C, space_outliner.runtime->stack_focus, *owner, kind, ordinal);
  if (new_ordinal < 0) {
    return -1;
  }
  outliner_stack_rows_invalidate(space_outliner);
  /* A layer the user just asked for is the one they mean to work on next. */
  outliner_stack_row_activate(C, space_outliner, new_ordinal);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return new_ordinal;
}

bool outliner_stack_row_set_enabled(bContext *C,
                                    SpaceOutliner &space_outliner,
                                    const int ordinal,
                                    const bool enable)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (!source.can_set_enabled(*owner) ||
      !source.row_set_enabled(*C, space_outliner.runtime->stack_focus, *owner, ordinal, enable))
  {
    return false;
  }
  outliner_stack_rows_invalidate(space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return true;
}

bool outliner_stack_row_remove(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (!source.can_remove(*owner) ||
      !source.row_remove(*C, space_outliner.runtime->stack_focus, *owner, ordinal))
  {
    return false;
  }
  outliner_stack_rows_invalidate(space_outliner);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return true;
}

void outliner_stack_sources_undo_reset()
{
  for (const StackSource *source : stack_sources_get()) {
    source->undo_reset();
  }
}

void OUTLINER_OT_stack_layer_focus(wmOperatorType *ot)
{
  ot->name = "Focus Stack";
  ot->idname = "OUTLINER_OT_stack_layer_focus";
  ot->description = "Show the layer stack of an object in the Outliner";
  ot->exec = stack_focus_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "object", nullptr, MAX_ID_NAME - 2, "Object", "Object to focus");
  RNA_def_int(ot->srna,
              "sub_index",
              -1,
              -1,
              SHRT_MAX,
              "Index",
              "Stack index within the object, such as a material slot; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_boolean(
      ot->srna, "enter_paint_mode", true, "Enter Paint Mode", "Enter Texture Paint mode");
}

void OUTLINER_OT_stack_layers_back(wmOperatorType *ot)
{
  ot->name = "Back to Stack Objects";
  ot->idname = "OUTLINER_OT_stack_layers_back";
  ot->description = "Show the objects that have a layer stack";
  ot->exec = stack_back_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;
}

void OUTLINER_OT_stack_layer_pin_toggle(wmOperatorType *ot)
{
  ot->name = "Pin Stack";
  ot->idname = "OUTLINER_OT_stack_layer_pin_toggle";
  ot->description = "Keep the focused stack when the active object changes";
  ot->exec = stack_pin_toggle_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;
}

void OUTLINER_OT_stack_layer_activate(wmOperatorType *ot)
{
  ot->name = "Activate Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_activate";
  ot->description = "Make a stack layer the one the rest of Blender acts on";
  ot->exec = stack_row_activate_exec;
  ot->poll = stack_row_activate_poll;
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "ordinal",
              0,
              0,
              SHRT_MAX,
              "Ordinal",
              "Position of the layer in the stack",
              0,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_clear_target(wmOperatorType *ot)
{
  ot->name = "Clear Stack Target";
  ot->idname = "OUTLINER_OT_stack_layer_clear_target";
  ot->description = "Stop directing edits at the activated stack layer";
  ot->exec = stack_target_clear_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;
}

void OUTLINER_OT_stack_layer_move(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {0, "UP", 0, "Up", "Move the layer towards the top of the stack"},
      {1, "DOWN", 0, "Down", "Move the layer towards the bottom of the stack"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_move";
  ot->description = "Change the position of a layer in the stack";
  ot->exec = stack_row_move_exec;
  ot->poll = stack_row_reorder_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to move; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_enum(ot->srna, "direction", direction_items, 0, "Direction", "Which way to move it");
  RNA_def_int(ot->srna,
              "to_ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Target",
              "Position to move the layer to; -1 uses the direction instead",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_add(wmOperatorType *ot)
{
  static const EnumPropertyItem type_items[] = {
      {int(StackAddKind::Empty),
       "EMPTY",
       0,
       "Empty Layer",
       "A layer that shows nothing until it is painted on"},
      {int(StackAddKind::Fill),
       "FILL",
       0,
       "Fill Layer",
       "A layer that covers what is below it from the start"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Add Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_add";
  ot->description = "Add a layer to the stack";
  ot->exec = stack_row_add_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(
      ot->srna, "type", type_items, int(StackAddKind::Empty), "Type", "What the new layer holds");
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Position for the new layer; -1 puts it on top of the stack",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_focus_sub_index(wmOperatorType *ot)
{
  ot->name = "Set Stack";
  ot->idname = "OUTLINER_OT_stack_focus_sub_index";
  ot->description = "Choose which of the object's stacks to show, such as a material slot";
  ot->exec = stack_sub_index_set_exec;
  ot->invoke = WM_enum_search_invoke;
  ot->poll = stack_sub_index_set_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "sub_index", rna_enum_dummy_NULL_items, 0, "Stack", "");
  RNA_def_enum_funcs(prop, stack_sub_index_itemf);
  RNA_def_property_flag(prop, PROP_ENUM_NO_TRANSLATE);
  ot->prop = prop;
}

void OUTLINER_OT_stack_layer_group(wmOperatorType *ot)
{
  ot->name = "Group Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_group";
  ot->description = "Put a run of layers into a group, composited on its own and laid over the "
                    "rest as one layer";
  ot->exec = stack_rows_group_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Bottom layer of the run; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_int(ot->srna,
              "to_ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Top",
              "Top layer of the run; -1 groups the bottom layer on its own",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_group_add(wmOperatorType *ot)
{
  ot->name = "Add Layer Group";
  ot->idname = "OUTLINER_OT_stack_layer_group_add";
  ot->description = "Add an empty group above the active layer, to put layers into";
  ot->exec = stack_group_add_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to sit above; -1 uses the active row",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_ungroup(wmOperatorType *ot)
{
  ot->name = "Ungroup Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_ungroup";
  ot->description = "Put the layers of a group back into the stack around it";
  ot->exec = stack_row_ungroup_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Group to unwrap; -1 uses the active row",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_duplicate(wmOperatorType *ot)
{
  ot->name = "Duplicate Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_duplicate";
  ot->description = "Copy a layer, maps and all, and put the copy above it";
  ot->exec = stack_row_duplicate_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to copy; -1 uses the active one",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_rename(wmOperatorType *ot)
{
  ot->name = "Rename Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_rename";
  ot->description = "Give a layer of the stack a new name";
  ot->exec = stack_row_rename_exec;
  ot->invoke = stack_row_rename_invoke;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to rename; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "New name for the layer");
}

void OUTLINER_OT_stack_layer_mask(wmOperatorType *ot)
{
  ot->name = "Stack Layer Mask";
  ot->idname = "OUTLINER_OT_stack_layer_mask";
  ot->description = "Add or remove the mask that modulates a layer";
  ot->exec = stack_row_mask_exec;
  ot->poll = stack_row_mask_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to mask; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_boolean(
      ot->srna, "add", true, "Add", "Add a mask, rather than removing the one already there");
}

void OUTLINER_OT_stack_layer_show_map(wmOperatorType *ot)
{
  ot->name = "Show Stack Layer Map";
  ot->idname = "OUTLINER_OT_stack_layer_show_map";
  ot->description = "Show one of a layer's maps in an Image Editor";
  ot->exec = stack_row_show_map_exec;
  ot->poll = ED_operator_outliner_active;
  /* A view change, not an edit: nothing here belongs in an undo step. */
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer whose map to show; -1 uses the active one",
              -1,
              SHRT_MAX);
  RNA_def_int(ot->srna,
              "role",
              0,
              0,
              STACK_ROW_SUB_ROW_STRIDE - 1,
              "Role",
              "Which of the layer's maps to show, as the source numbers them",
              0,
              STACK_ROW_SUB_ROW_STRIDE - 1);
}

void OUTLINER_OT_stack_layer_visibility_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Stack Layer Visibility";
  ot->idname = "OUTLINER_OT_stack_layer_visibility_toggle";
  /* Phrased as an edit rather than a view setting on purpose: for a paint stack this mutes the
   * layer's nodes, so it changes what renders. */
  ot->description = "Turn a layer of the stack on or off";
  ot->exec = stack_row_visibility_toggle_exec;
  ot->poll = stack_row_visibility_toggle_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to toggle; -1 uses the active one",
              -1,
              SHRT_MAX);
}

void OUTLINER_OT_stack_layer_remove(wmOperatorType *ot)
{
  ot->name = "Remove Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_remove";
  ot->description = "Delete a layer from the stack";
  ot->exec = stack_row_remove_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to remove; -1 uses the active one",
              -1,
              SHRT_MAX);
}

}  // namespace blender::ed::outliner
