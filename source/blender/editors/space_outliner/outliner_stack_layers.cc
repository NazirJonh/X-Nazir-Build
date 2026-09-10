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
#include <functional>

#include "DNA_defs.h"
#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_outliner_stack_automation.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_define.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"
#include "../interface/interface_intern.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "tree/tree_iterator.hh"
#include "outliner_stack_source.hh"

namespace blender::ui {

/* RNA enum stepping wraps around by default. Keep this selector on its first/last item instead. */
static int stack_focus_sub_index_menu_step(bContext *C, const int direction, Button *but)
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

void template_stack_focus_sub_index(Layout *layout,
                                    PointerRNA *ptr,
                                    const StringRefNull propname)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  if (!prop) {
    return;
  }

  Block *block = layout->block();
  block_layout_set_current(block, layout);
  /* Keep the selector width independent of the current material name and item count. */
  Button *but = uiDefButR_prop(block,
                               ButtonType::Menu,
                               std::nullopt,
                               0,
                               0,
                               UI_UNIT_X * 6,
                               UI_UNIT_Y,
                               ptr,
                               prop,
                               -1,
                               0,
                               0,
                               std::nullopt);
  button_func_menu_step_set(but, stack_focus_sub_index_menu_step);
}

}  // namespace blender::ui

namespace blender::ed::outliner {

/** Defined below: the shared resolver of the operators that address one row, marker first. */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op);

namespace {

/* The Stack Layers clipboard holds row identities rather than raw source data. A paste asks the
 * source to duplicate those rows, so every source keeps ownership of its data and copy semantics. */
Vector<StackItemIdentity> stack_layer_clipboard;

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

/** The same question every mutating stack operator asks: is there a stack here to remove from,
 * and does its source take edits at all. */
bool stack_row_remove_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  const StackSource *source = stack_source_for_space(*space_outliner);
  return owner != nullptr && source->can_edit(*owner);
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
  const bool enter_edit_mode = RNA_boolean_get(op->ptr, "enter_edit_mode");
  return outliner_stack_focus_set(C, *space_outliner, *object, sub_index, enter_edit_mode) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

wmOperatorStatus stack_back_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (RNA_boolean_get(op->ptr, "keep_view")) {
    /* Staying in the Stack Layers mode only steps back to the objects list. */
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
  }
  else {
    /* A plain invocation leaves the Stack Layers mode for the regular View Layer browsing. */
    space_outliner->outlinevis = SO_VIEW_LAYER;
    space_outliner->stack_layers_view = SO_SL_VIEW_OBJECTS;
    /* Leaving the mode through an operator skips #rna_SpaceOutliner_display_mode_update, so the
     * tool header that carries only the Stack Layers controls is synced here through the same
     * helper the RNA update calls. */
    ScrArea *area = CTX_wm_area(C);
    if (area != nullptr) {
      outliner_tool_header_visibility_sync(area, *space_outliner);
      ED_area_tag_redraw(area);
    }
  }
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus stack_back_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  /* The modifier is read once, at invoke: an exec reached from a script or from Repeat Last has
   * no event to read, and an operator whose behavior is not in its properties is not
   * reproducible. */
  RNA_boolean_set(op->ptr, "keep_view", (event->modifier & KM_ALT) != 0);
  return stack_back_exec(C, op);
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
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  return outliner_stack_row_activate(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
}

/**
 * Remember every row's UI state by identity, before the rows go away.
 *
 * Clicks and collapses live in the tree store, keyed by owner and ordinal: state a row keeps only
 * while no edit renumbers it. Read into #SpaceOutliner_Runtime::stack_row_ui_state here, while the
 * tree and the rows still agree on what each ordinal means, this is what re-attaches collapsed and
 * selected to the same rows after the edit -- whichever ordinals they land on.
 *
 * Called from #outliner_stack_rows_invalidate, which every path that re-reads the rows goes
 * through, so nothing that renumbers has to remember to ask.
 */
void stack_row_ui_state_capture(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* Once an edit has re-read the rows, the tree store still addresses the previous ordinals.
   * Capturing it again would overwrite identity state with the row now sitting at each old ordinal.
   */
  if (runtime.stack_owner_uid == 0 || runtime.stack_rows_rebuilt_since_build) {
    return;
  }
  tree_iterator::all(space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type != TSE_STACK_LAYER) {
      return;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, int(tselem->nr));
    if (row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
      return;
    }
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row->stable_id);
    state.closed = (tselem->flag & TSE_CLOSED) != 0;
    state.selected = (tselem->flag & TSE_SELECTED) != 0;
    state.active = (tselem->flag & TSE_ACTIVE) != 0;
  });
}

/**
 * The selection an edit just left behind: the row it names reads as selected and active once the
 * tree rebuilds, and every other row reads as not -- an edit renumbers the stack, and without
 * this the tree store would hand the old ordinal's selection to whichever row moved into it.
 *
 * The edit has already run, so the rows re-read here are the renumbered ones and \a select_ordinal
 * addresses them. -1 names nothing -- an edit that took a row away, such as a remove or an
 * ungroup -- and only clears. The map is what carries the selection across the rebuild; a row
 * without a #StackRow::stable_id cannot be addressed by it and keeps the tree store's state.
 */
void stack_row_ui_state_edit_select(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    ID &owner,
                                    const int select_ordinal)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  outliner_stack_rows_ensure(ctx, space_outliner, owner);
  for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
    state.selected = false;
    state.active = false;
  }
  if (select_ordinal < 0) {
    return;
  }
  const StackItemIdentity identity = outliner_stack_identity_of(space_outliner, select_ordinal);
  if (BLI_uuid_is_nil(identity.row_id)) {
    return;
  }
  StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(identity.row_id);
  state.selected = true;
  state.active = true;
}

/**
 * The prologue and epilogue every mutating wrapper below shares: resolve the owner, get the
 * source's editor, run \a fn, invalidate the cached rows and notify on success.
 *
 * \param fn: `bool(const StackSource &source, const StackEditor &editor, const StackFocus &focus,
 * ID &owner, int &r_select_ordinal)` -- the one call that differs between callers, including
 * whichever guard it needs beyond an editor existing at all: #StackEditor::can_reorder for a
 * reorder, #StackSource::is_editable for the rest of what used to be separate `can_*` predicates,
 * or nothing at all for an edit that never had one. Returning false leaves the stack exactly as
 * this found it. \a r_select_ordinal starts at -1; a caller whose edit renumbers the stack sets it
 * to whichever row should read as selected once the tree rebuilds.
 * \param needs_renumber: whether the edit changes which row sits at which ordinal at all. A
 * rename or a visibility toggle does not, and leaves every row under the tree-store key it
 * already had -- no selection to re-target. \a r_ordinal: when given, receives \a
 * select_ordinal, for a caller that needs the row's new position for something of its own, such
 * as activating it.
 */
template<typename Fn>
bool stack_mutate(bContext &C,
                  SpaceOutliner &space_outliner,
                  const bool needs_renumber,
                  Fn &&fn,
                  int *r_ordinal = nullptr)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(space_outliner);
  const StackEditor *editor = source.editor();
  /* The one editability check of the middle layer: the polls grey the UI out, the source's own
   * methods guard the seam, and a check in every lambda here would only be a third copy of the
   * same question. */
  if (editor == nullptr || !source.is_editable(*owner)) {
    return false;
  }
  int select_ordinal = -1;
  if (!fn(source, *editor, space_outliner.runtime->stack_focus, *owner, select_ordinal)) {
    return false;
  }
  if (r_ordinal != nullptr) {
    *r_ordinal = select_ordinal;
  }
  /* Invalidating captures the rows' UI state into the identity-keyed map before the rows go
   * away; a renumbering edit then re-targets the selection on the map, which is what carries it
   * across the rebuild. */
  outliner_stack_rows_invalidate(space_outliner);
  if (needs_renumber) {
    stack_row_ui_state_edit_select(ctx, space_outliner, *owner, select_ordinal);
  }
  WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);
  return true;
}

/**
 * The ordinal of the row the operator's \a marker names, or -1.
 *
 * A marker is the row's identity -- #StackRow::stable_id, as an earlier call of the mode handed it
 * out. A script that re-addresses a row by its marker gets the same row after an edit moved it,
 * where a remembered ordinal is the position-trap the seam's own code avoids through that same
 * marker; the position properties stay for UI buttons, which speak for the row on screen at the
 * moment of the call.
 */
static int stack_operator_marker_ordinal_get(bContext &C,
                                             SpaceOutliner &space_outliner,
                                             wmOperator &op)
{
  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op.ptr, "marker", marker_str);
  if (marker_str[0] == '\0') {
    return -1;
  }
  bUUID marker;
  if (!BLI_uuid_parse_string(&marker, marker_str)) {
    return -1;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return -1;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  for (const StackRow &row : space_outliner.runtime->stack_rows) {
    if (BLI_uuid_equal(row.stable_id, marker)) {
      return row.ordinal;
    }
  }
  return -1;
}

wmOperatorStatus stack_row_move_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

  /* A marker or position the operator was given names one row; without one, the whole selection
   * moves together, and with nothing selected the active row -- the same vocabulary every other
   * operator reads. #stack_operator_ordinal_get cannot be shared here, because it answers a
   * selection with its first row, and a move of one row is not a move of the selection. */
  Vector<int> ordinals_to_move;
  int ordinal = stack_operator_marker_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    ordinal = RNA_int_get(op->ptr, "ordinal");
  }
  if (ordinal >= 0) {
    ordinals_to_move.append(ordinal);
  }
  else {
    stack_selected_ordinals_get(*space_outliner, ordinals_to_move);
  }
  if (ordinals_to_move.is_empty()) {
    /* Nothing picked: the row the user was on, which is what a bare Move has always meant. */
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                 *space_outliner);
    if (active_ordinal < 0) {
      return OPERATOR_CANCELLED;
    }
    ordinals_to_move.append(active_ordinal);
  }

  /* Up is toward the top of the screen, and the screen lists a stack top first: row 0 is the
   * bottom, so up means a larger ordinal. The block walks like a line of people, its leading row
   * first -- the row closest to where the block is going -- which is the highest ordinal moving
   * up and the lowest moving down: any other order makes the block step over itself. */
  const bool move_up = RNA_enum_get(op->ptr, "direction") == 0;
  if (move_up) {
    std::sort(ordinals_to_move.begin(), ordinals_to_move.end(), std::greater<int>());
  }
  else {
    std::sort(ordinals_to_move.begin(), ordinals_to_move.end());
  }

  /* Move each row one step in the direction, resolving its ordinal fresh: every move renumbers
   * the stack, so an ordinal read before a move can name a different row after it. */
  bool any_moved = false;
  for (const int original_ordinal : ordinals_to_move) {
    const StackItemIdentity identity = outliner_stack_identity_of(*space_outliner,
                                                                  original_ordinal);
    if (!identity.is_valid()) {
      continue; /* Row disappeared mid-operation. */
    }
    const StackReadContext ctx = outliner_stack_read_context(*C);
    const int current_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, identity);
    if (current_ordinal < 0) {
      continue;
    }
    const int target_ordinal = current_ordinal + (move_up ? 1 : -1);
    if (outliner_stack_row_reorder(C, *space_outliner, current_ordinal, target_ordinal)) {
      any_moved = true;
    }
  }

  return any_moved ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_copy_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  if (selected.is_empty()) {
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                  *space_outliner);
    if (active_ordinal >= 0) {
      selected.append(active_ordinal);
    }
  }

  /* A selected folder already carries every row below it. Keeping selected descendants as separate
   * clipboard items would try to paste them once more after their containing folder. */
  stack_ordinals_drop_covered_descendants(*space_outliner, selected);

  stack_layer_clipboard.clear();
  for (const int ordinal : selected) {
    const StackItemIdentity identity = outliner_stack_identity_of(*space_outliner, ordinal);
    if (identity.is_valid()) {
      stack_layer_clipboard.append(identity);
    }
  }
  if (stack_layer_clipboard.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stack layers to copy");
    return OPERATOR_CANCELLED;
  }
  BKE_reportf(
      op->reports, RPT_INFO, "Copied %d stack layer(s)", int(stack_layer_clipboard.size()));
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_paste_exec(bContext *C, wmOperator *op)
{
  if (stack_layer_clipboard.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stack layers to paste");
    return OPERATOR_CANCELLED;
  }

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const int active_ordinal = outliner_stack_active_ordinal_get(ctx, *space_outliner);
  if (active_ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  StackItemIdentity anchor = outliner_stack_identity_of(*space_outliner, active_ordinal);
  if (!anchor.is_valid()) {
    return OPERATOR_CANCELLED;
  }

  Vector<StackItemIdentity> pasted;
  for (const StackItemIdentity &source_identity : stack_layer_clipboard) {
    const int source_ordinal = outliner_stack_identity_resolve(
        ctx, *space_outliner, source_identity);
    const int anchor_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, anchor);
    if (source_ordinal < 0 || anchor_ordinal < 0) {
      continue;
    }

    int copied_ordinal = -1;
    const bool copied = stack_mutate(
        *C,
        *space_outliner,
        true,
        [&](const StackSource & /*source*/,
            const StackEditor &editor,
            const StackFocus &focus,
            ID &owner,
            int &r_select_ordinal) {
          r_select_ordinal = editor.row_duplicate(*C, focus, owner, source_ordinal);
          return r_select_ordinal >= 0;
        },
        &copied_ordinal);
    if (!copied || copied_ordinal < 0) {
      continue;
    }

    const StackItemIdentity copied_identity = outliner_stack_identity_of(*space_outliner,
                                                                           copied_ordinal);
    if (!copied_identity.is_valid()) {
      continue;
    }
    const int copied_ordinal_now = outliner_stack_identity_resolve(
        ctx, *space_outliner, copied_identity);
    const int anchor_ordinal_now = outliner_stack_identity_resolve(ctx, *space_outliner, anchor);
    if (copied_ordinal_now < 0 || anchor_ordinal_now < 0 ||
        !outliner_stack_row_move(
            C, *space_outliner, copied_ordinal_now, anchor_ordinal_now, StackMovePlace::Above))
    {
      continue;
    }
    pasted.append(copied_identity);
    anchor = copied_identity;
  }

  if (pasted.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "Could not paste the copied stack layers here");
    return OPERATOR_CANCELLED;
  }

  SpaceOutliner_Runtime &runtime = *space_outliner->runtime;
  for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
    state.selected = false;
    state.active = false;
  }
  for (const StackItemIdentity &identity : pasted) {
    /* A row the source gives no identity for cannot be addressed by this map; keying it at nil
     * would fold every such row into one shared state. */
    if (BLI_uuid_is_nil(identity.row_id)) {
      continue;
    }
    runtime.stack_row_ui_state.lookup_or_add_default(identity.row_id).selected = true;
  }
  const int last_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, pasted.last());
  if (last_ordinal >= 0) {
    outliner_stack_row_activate(C, *space_outliner, last_ordinal);
    if (!BLI_uuid_is_nil(pasted.last().row_id)) {
      StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(
          pasted.last().row_id);
      state.active = true;
      state.selected = true;
    }
  }
  BKE_reportf(op->reports, RPT_INFO, "Pasted %d stack layer(s)", int(pasted.size()));
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_remove_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* A marker names the row by identity and wins over the position; without one, the position the
   * operator was given addresses the single row, and -1 falls through to the selection. */
  int ordinal = stack_operator_marker_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    ordinal = RNA_int_get(op->ptr, "ordinal");
  }
  if (ordinal >= 0) {
    return outliner_stack_row_remove(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
  }
  /* The whole selection goes at once, the highest ordinal first: a removal renumbers the rows
   * above it, so walking down the stack keeps every remaining ordinal what it was when read. */
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  if (selected.is_empty()) {
    /* Nothing picked: the row the user was on, which is what a bare Remove has always meant. */
    const int active_ordinal = outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                                 *space_outliner);
    if (active_ordinal < 0) {
      return OPERATOR_CANCELLED;
    }
    selected.append(active_ordinal);
  }
  bool removed_any = false;
  for (int64_t index = selected.size() - 1; index >= 0; index--) {
    removed_any |= outliner_stack_row_remove(C, *space_outliner, selected[index]);
  }
  return removed_any ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

/** The Add kind \a kind as the space's source declares it; false when it declares no such kind. */
static bool stack_add_kind_info_get(const bContext &C, const int kind, StackAddKindInfo &r_info)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackEditor *editor = stack_source_for_space(*space_outliner)->editor();
  if (editor == nullptr) {
    return false;
  }
  Vector<StackAddKindInfo> kinds;
  editor->add_kinds(kinds);
  if (!kinds.index_range().contains(kind)) {
    return false;
  }
  r_info = std::move(kinds[kind]);
  return true;
}

wmOperatorStatus stack_row_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int kind = RNA_enum_get(op->ptr, "type");
  StackAddKindInfo kind_info;
  if (!stack_add_kind_info_get(*C, kind, kind_info)) {
    return OPERATOR_CANCELLED;
  }

  StackAddArgs args;
  /* A kind made from an existing data-block is handed it by name rather than by a pointer the UI
   * held on to, so the call stays what the info log shows: a repeat or a script finds the same
   * data-block again. */
  char source_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "source", source_name);
  if (kind_info.source_id_type != 0) {
    args.source = (source_name[0] != '\0') ?
                      BKE_libblock_find_name(
                          CTX_data_main(C), kind_info.source_id_type, source_name) :
                      nullptr;
    if (args.source == nullptr) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  RPT_("No data-block named \"%s\" to make the layer from"),
                  source_name);
      return OPERATOR_CANCELLED;
    }
  }

  /* The row the Add is anchored to: a marker or the explicit "ordinal" property when a script gave
   * one, otherwise the selection and then the active row. -1 names no row and puts the new layer
   * on top of the stack. The source decides what "anchored to" means -- directly above the row,
   * or, when the row is a folder, inside it -- so a row inside a folder keeps the new layer in
   * that folder. */
  const int anchor_ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  float fill_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  RNA_float_get_array(op->ptr, "fill_color", fill_color);
  if (kind_info.takes_color) {
    args.color = fill_color;
  }
  return outliner_stack_row_add(C, *space_outliner, kind, anchor_ordinal, args) >= 0 ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

/** Whether the Add's current kind declared #StackAddKindInfo::takes_color. */
static bool stack_row_add_takes_color(const bContext &C, wmOperator &op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return false;
  }
  const StackEditor *editor = stack_source_for_space(*space_outliner)->editor();
  if (editor == nullptr) {
    return false;
  }
  Vector<StackAddKindInfo> kinds;
  editor->add_kinds(kinds);
  const int kind = RNA_enum_get(op.ptr, "type");
  return kinds.index_range().contains(kind) && kinds[kind].takes_color;
}

/**
 * The Add's invoke: a kind whose creation takes a colour opens the color picker first, the rest
 * run straight through. The picker is what "add a fill layer" means -- the colour is the layer's
 * content, not an option to be changed afterwards.
 *
 * A confirm dialog rather than a redo popup: every change in a redo popup re-runs the whole add,
 * and dragging a colour would create and throw away a set of full-size maps per mouse move.
 */
static wmOperatorStatus stack_row_add_invoke(bContext *C,
                                             wmOperator *op,
                                             const wmEvent * /*event*/)
{
  if (!stack_row_add_takes_color(*C, *op)) {
    return stack_row_add_exec(C, op);
  }
  return WM_operator_props_dialog_popup(C, op, 220, std::nullopt, IFACE_("Add"));
}

/**
 * Only the colour is a choice the user makes here; the anchor, the kind and the marker are what the
 * button that placed the call already decided.
 */
static void stack_row_add_ui(bContext *C, wmOperator *op)
{
  if (!stack_row_add_takes_color(*C, *op)) {
    return;
  }
  op->layout->prop(op->ptr, "fill_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
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
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

bool stack_row_clipboard_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  const SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->outlinevis == SO_STACK_LAYERS &&
         space_outliner->stack_layers_view == SO_SL_VIEW_STACK;
}

/**
 * The Add operator's own poll, on top of #stack_row_add_poll: a source that declares no kinds of
 * rows gets no Add at all, rather than an Add that would refuse whatever it was asked for.
 */
bool stack_add_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<StackAddKindInfo> kinds;
  if (const StackEditor *editor = stack_source_for_space(*space_outliner)->editor()) {
    editor->add_kinds(kinds);
  }
  return !kinds.is_empty();
}

wmOperatorStatus stack_row_visibility_toggle_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  if (ID *owner = outliner_stack_owner_get(ctx, *space_outliner)) {
    outliner_stack_rows_ensure(ctx, *space_outliner, *owner);
  }
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
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

/**
 * The sub-selections of the focused object, as enum items.
 *
 * Built from the source's own sub-selections, so the Outliner offers "which material slot" without
 * knowing that a slot is what a paint stack's sub-index means; the item's preview icon comes from
 * the data-block the source named, when it named one.
 */
const EnumPropertyItem *stack_sub_index_itemf_impl(bContext *C,
                                                    SpaceOutliner &space_outliner,
                                                    bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  if (C != nullptr && space_outliner.runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    Vector<StackSubSelection> selections;
    stack_source_for_space(space_outliner)
        ->sub_selections_get(ctx, space_outliner.runtime->stack_focus, selections);
    for (const int index : selections.index_range()) {
      /* The identifier has to survive the item array, and a slot number is stable while the name
       * is not; the name is what the user reads. */
      char identifier[16];
      SNPRINTF_UTF8(identifier, "%d", index);
      EnumPropertyItem item = {};
      item.value = index;
      item.identifier = BLI_strdup(identifier);
      item.name = BLI_strdup(selections[index].name.empty() ? IFACE_("Empty Slot") :
                                                              selections[index].name.c_str());
      item.description = "";
      if (selections[index].preview_id != nullptr) {
        item.icon = ui::icon_id_preview_get(C, selections[index].preview_id);
      }
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

const EnumPropertyItem *stack_sub_index_itemf(bContext *C,
                                              PointerRNA * /*ptr*/,
                                              PropertyRNA * /*prop*/,
                                              bool *r_free)
{
  SpaceOutliner *space_outliner = (C != nullptr) ? CTX_wm_space_outliner(C) : nullptr;
  if (space_outliner == nullptr) {
    *r_free = true;
    return nullptr;
  }
  return stack_sub_index_itemf_impl(C, *space_outliner, r_free);
}

/**
 * The kinds of rows the source can add, as enum items.
 *
 * Built from the editor's own list, so the Add UI offers the domain's vocabulary without the
 * Outliner knowing it: the item's value is the kind's place in that list, which is what
 * #StackEditor::row_add reads back, and the identifier is the stable name scripts see.
 */
const EnumPropertyItem *stack_add_type_itemf(bContext *C,
                                             PointerRNA * /*ptr*/,
                                             PropertyRNA * /*prop*/,
                                             bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  SpaceOutliner *space_outliner = (C == nullptr) ? nullptr : CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
    const StackSource &source = *stack_source_for_space(*space_outliner);
    Vector<StackAddKindInfo> kinds;
    if (owner != nullptr && source.can_edit(*owner)) {
      if (const StackEditor *editor = source.editor()) {
        editor->add_kinds(kinds);
      }
    }
    for (const int64_t index : kinds.index_range()) {
      EnumPropertyItem item = {};
      item.value = int(index);
      item.identifier = BLI_strdup(kinds[index].identifier.c_str());
      /* The source's own strings are untranslated vocabulary; the display is where they meet the
       * user's language, names in the interface domain and descriptions in the tooltip one. */
      item.name = BLI_strdup(IFACE_(kinds[index].name.c_str()));
      item.description = BLI_strdup(TIP_(kinds[index].description.c_str()));
      item.icon = kinds[index].icon;
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
  outliner_stack_focus_sub_index_set(*space_outliner, RNA_enum_get(op->ptr, "sub_index"));
  return outliner_stack_focus_sub_index_apply(*C, *space_outliner) ?
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
         outliner_stack_focus_object_get(outliner_stack_read_context(*C),
                                         space_outliner->runtime->stack_focus) != nullptr;
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
  /* Wrapping a run of rows inserts the folder row among them, so everything above it is
   * renumbered; the folder itself is what the user is left working with. */
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr) {
          return false;
        }
        r_select_ordinal = grouping->rows_group(*C, focus, owner, from_ordinal, to_ordinal);
        return r_select_ordinal >= 0;
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_group_add_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  /* -1 means the top of the stack, which is where a folder goes when nothing is pointed at. */
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  /* An empty folder is inserted into the stack, so every row above it has a new ordinal. */
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr) {
          return false;
        }
        r_select_ordinal = grouping->group_add(*C, focus, owner, ordinal);
        return r_select_ordinal >= 0;
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_ungroup_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  /* The folder row is gone and its contents took its place, so there is no row left to hand the
   * selection to -- but the rows that moved up still have to be stripped of the state the tree
   * store holds under their new ordinals. */
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        return grouping != nullptr && grouping->row_ungroup(*C, focus, owner, ordinal) >= 0;
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

wmOperatorStatus stack_row_merge_down_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 1) {
    /* Nothing below the bottom of the stack to merge into. */
    return OPERATOR_CANCELLED;
  }
  /* The pair reads as one group row from then on, and that row is what the user works with. */
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr) {
          return false;
        }
        r_select_ordinal = grouping->row_merge_down(*C, focus, owner, ordinal);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_row_activate(C, *space_outliner, new_ordinal);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_duplicate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        r_select_ordinal = editor.row_duplicate(*C, focus, owner, ordinal);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return OPERATOR_CANCELLED;
  }
  /* The copy sits directly above the original; that is the seam's own promise for
   * #StackEditor::row_duplicate, not something the Outliner papers over here -- a source that
   * lands its copy elsewhere is a source bug, and a move on top of it would turn one wrong
   * ordinal into two edits and two undo steps. */
  outliner_stack_row_activate(C, *space_outliner, new_ordinal);
  return OPERATOR_FINISHED;
}

wmOperatorStatus stack_row_rename_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "A layer needs a name");
    return OPERATOR_CANCELLED;
  }
  return outliner_stack_row_rename(C, *space_outliner, ordinal, name) ? OPERATOR_FINISHED :
                                                                       OPERATOR_CANCELLED;
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
  const bool add = RNA_boolean_get(op->ptr, "add");
  /* The seam takes a fill color, so a third kind of mask is a new item on this menu, not a new
   * parameter carried through every layer between here and the generated image. */
  const bool black = RNA_enum_get(op->ptr, "initial_color") == 1;
  const float initial_color[4] = {black ? 0.0f : 1.0f,
                                  black ? 0.0f : 1.0f,
                                  black ? 0.0f : 1.0f,
                                  1.0f};
  const bool ok = stack_mutate(
      *C,
      *space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
           int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        return grouping != nullptr &&
               grouping->row_mask_set(*C, focus, owner, ordinal, add, initial_color);
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
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
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
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

const EnumPropertyItem *outliner_stack_focus_sub_index_itemf(bContext *C,
                                                              SpaceOutliner &space_outliner,
                                                              bool *r_free)
{
  return stack_sub_index_itemf_impl(C, space_outliner, r_free);
}

StackReadContext outliner_stack_read_context(const bContext &C)
{
  bContext &context = const_cast<bContext &>(C);
  StackReadContext ctx;
  ctx.bmain = CTX_data_main(&context);
  ctx.scene = CTX_data_scene(&context);
  ctx.view_layer = CTX_data_view_layer(&context);
  return ctx;
}

Object *outliner_stack_focus_object_get(const StackReadContext &ctx, const StackFocus &focus)
{
  if (focus.object_uid == 0) {
    /* Resolved at use rather than stored: the active object can be freed or replaced under a
     * stored pointer by an undo step, a file load or a delete. */
    if (ctx.bmain == nullptr || ctx.view_layer == nullptr) {
      return nullptr;
    }
    BKE_view_layer_synced_ensure(*ctx.bmain, ctx.scene, ctx.view_layer);
    return BKE_view_layer_active_object_get(ctx.view_layer);
  }
  if (ctx.bmain == nullptr) {
    return nullptr;
  }
  return id_cast<Object *>(BKE_libblock_find_session_uid(ctx.bmain, ID_OB, focus.object_uid));
}

Object *outliner_stack_focus_object_resolve(const StackReadContext &ctx,
                                            SpaceOutliner &space_outliner)
{
  Object *object = outliner_stack_focus_object_get(ctx, space_outliner.runtime->stack_focus);
  if (object == nullptr && space_outliner.runtime->stack_focus.object_uid != 0) {
    /* A focus that names a gone object pins nothing: drop it and the rows cached under it, and
     * fall back to the object the view layer has active now. */
    space_outliner.runtime->stack_focus = {};
    outliner_stack_rows_invalidate(space_outliner);
    object = outliner_stack_focus_object_get(ctx, space_outliner.runtime->stack_focus);
  }
  return object;
}

const char *outliner_stack_focus_name_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ?
             space_outliner.runtime->stack_focus_name.c_str() :
             "";
}

int outliner_stack_focus_sub_index_get(const SpaceOutliner &space_outliner)
{
  return (space_outliner.runtime != nullptr) ? space_outliner.runtime->stack_focus.sub_index : -1;
}

void outliner_stack_focus_sub_index_set(SpaceOutliner &space_outliner, const int sub_index)
{
  if (space_outliner.runtime != nullptr) {
    space_outliner.runtime->stack_focus.sub_index = sub_index;
  }
}

bool outliner_stack_focus_sub_index_apply(bContext &C, SpaceOutliner &space_outliner)
{
  if (space_outliner.runtime == nullptr) {
    return false;
  }
  Object *object = outliner_stack_focus_object_resolve(outliner_stack_read_context(C),
                                                        space_outliner);
  if (object == nullptr) {
    return false;
  }
  const int sub_index = outliner_stack_focus_sub_index_get(space_outliner);
  /* The sub-selection's own side effects in the data -- moving the material slot pointer along,
   * for one -- are the source's business; the generic path only passes through. */
  stack_source_for_space(space_outliner)->sub_selection_apply(
      C, space_outliner.runtime->stack_focus, *object);
  return outliner_stack_focus_set(&C, space_outliner, *object, sub_index, false);
}

bool outliner_stack_row_is_selected(const SpaceOutliner &space_outliner, const int ordinal)
{
  bool selected = false;
  tree_iterator::all(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      selected = (tselem->flag & TSE_SELECTED) != 0;
    }
  });
  return selected;
}

bool outliner_stack_row_is_open(const SpaceOutliner &space_outliner, const int ordinal)
{
  bool open = false;
  tree_iterator::all(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      open = (tselem->flag & TSE_CLOSED) == 0;
    }
  });
  return open;
}

void outliner_stack_row_select(bContext &C,
                               SpaceOutliner &space_outliner,
                               const int ordinal,
                               const bool select,
                               const bool extend)
{
  tree_iterator::all(space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      outliner_item_select(&C,
                           &space_outliner,
                           te,
                           (select ? OL_ITEM_SELECT : 0) | (extend ? OL_ITEM_EXTEND : 0));
    }
  });
}

void outliner_stack_row_closed_set(SpaceOutliner &space_outliner,
                                   const int ordinal,
                                   const bool closed)
{
  tree_iterator::all(space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      outliner_item_openclose(te, !closed, false);
    }
  });
}

uint32_t outliner_stack_row_preview_uid(const SpaceOutliner &space_outliner, const int ordinal)
{
  uint32_t uid = 0;
  tree_iterator::all(space_outliner, [&](const TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
      if (row != nullptr) {
        /* The row's main preview is its first slot that names a data-block. */
        for (const StackRowPreview &slot : row->preview_slots) {
          if (slot.id_uid != 0) {
            uid = slot.id_uid;
            break;
          }
        }
      }
    }
  });
  return uid;
}

void outliner_stack_rows_invalidate(SpaceOutliner &space_outliner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* The rows and the tree still agree on what each ordinal means; whatever the user did with
   * them is only about to be renumbered, so this is the last chance to keep it by identity. */
  stack_row_ui_state_capture(space_outliner);
  runtime.stack_rows.clear();
  runtime.stack_row_index.clear();
  runtime.stack_owner_uid = 0;
  runtime.stack_state_hash = 0;
  runtime.stack_rows_valid = false;
  /* The rows are about to go away, and the drop indicator names one of them.
   *
   * What a drop was aimed at is deliberately *not* cleared with them: it names its row by
   * identity, not by address, and is resolved against whatever rows exist when the drop lands.
   * Clearing it here loses the aim of a drop that is already under way -- importing a dragged
   * asset remaps data-blocks, which reaches this function through #outliner_id_remap, between the
   * poll that recorded the aim and the operator that reads it back. */
  runtime.stack_drop_indicator.ordinal = -1;
}

void outliner_stack_row_ui_state_sync(SpaceOutliner &space_outliner, const ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  /* The tree store speaks the ordinals of the tree it was filled by, so it can only be read
   * against the rows that tree was built from: the same owner, and not one an edit has re-read
   * since -- #outliner_stack_rows_ensure marks those, and the build that follows clears the
   * mark. */
  if (space_outliner.treestore == nullptr || runtime.stack_owner_uid != owner.session_uid ||
      runtime.stack_rows_rebuilt_since_build)
  {
    return;
  }
  BLI_mempool_iter iter;
  BLI_mempool_iternew(space_outliner.treestore, &iter);
  while (const TreeStoreElem *tselem = static_cast<const TreeStoreElem *>(
             BLI_mempool_iterstep(&iter)))
  {
    if (tselem->type != TSE_STACK_LAYER || tselem->id != &owner) {
      continue;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, int(tselem->nr));
    if (row == nullptr || BLI_uuid_is_nil(row->stable_id)) {
      continue;
    }
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row->stable_id);
    state.closed = (tselem->flag & TSE_CLOSED) != 0;
    state.selected = (tselem->flag & TSE_SELECTED) != 0;
    state.active = (tselem->flag & TSE_ACTIVE) != 0;
  }
}

void outliner_stack_row_ui_state_prune(SpaceOutliner_Runtime &runtime, Set<UUID> &&seen)
{
  /* An entry unseen at this build and the previous one names a row the stack no longer has. */
  Vector<UUID> forgotten;
  for (const UUID &key : runtime.stack_row_ui_state.keys()) {
    if (!seen.contains(key) && !runtime.stack_row_ui_state_seen.contains(key)) {
      forgotten.append(key);
    }
  }
  for (const UUID &key : forgotten) {
    runtime.stack_row_ui_state.remove(key);
  }
  runtime.stack_row_ui_state_seen = std::move(seen);
}

StringRef outliner_stack_row_active_section_get(const SpaceOutliner &space_outliner,
                                                const StackRow &row)
{
  /* A row without sections has nothing to switch between; its content stays collapsed. */
  if (row.content_sections.is_empty()) {
    return {};
  }

  const SpaceOutliner_Runtime &runtime = *space_outliner.runtime;

  /* No stable_id means no persistent UI state: always use default section. */
  if (BLI_uuid_is_nil(row.stable_id)) {
    return row.content_sections[0].identifier;
  }

  const StackRowUiState *state = runtime.stack_row_ui_state.lookup_ptr(row.stable_id);
  if (state == nullptr || state->active_content_section.empty()) {
    /* No stored state: default to first section. */
    return row.content_sections[0].identifier;
  }

  /* Validate that the stored section still exists in the row's current data. */
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier == state->active_content_section) {
      return state->active_content_section;
    }
  }

  /* Stored section no longer exists (e.g., mask was deleted): fall back to first section. */
  return row.content_sections[0].identifier;
}

bool outliner_stack_row_active_section_set(SpaceOutliner &space_outliner,
                                           const StackRow &row,
                                           const StringRefNull section_id)
{
  /* Rows without content sections don't support section switching. */
  if (row.content_sections.is_empty()) {
    return false;
  }

  /* Validate that the section exists in the row. */
  bool section_exists = false;
  for (const StackContentSection &section : row.content_sections) {
    if (section.identifier == section_id) {
      section_exists = true;
      break;
    }
  }

  if (!section_exists) {
    return false;
  }

  /* No stable_id means no persistent UI state: cannot store selection. */
  if (BLI_uuid_is_nil(row.stable_id)) {
    return false;
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(row.stable_id);
  state.active_content_section = section_id;

  return true;
}

void outliner_stack_rows_ensure(const StackReadContext &ctx,
                                SpaceOutliner &space_outliner,
                                ID &owner)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  const StackSource &source = *stack_source_for_space(space_outliner);
  const uint64_t state_hash = source.state_hash(ctx, owner);
  if (runtime.stack_rows_valid && runtime.stack_owner_uid == owner.session_uid &&
      runtime.stack_state_hash == state_hash)
  {
    return;
  }
  runtime.stack_rows.clear();
  source.rows_build(ctx, runtime.stack_focus, owner, runtime.stack_rows);
  /* Rows are addressed by ordinal all over the mode; one map spares every read a walk. */
  runtime.stack_row_index.clear();
  for (const int64_t index : runtime.stack_rows.index_range()) {
    runtime.stack_row_index.add(int(runtime.stack_rows[index].ordinal), int(index));
  }
  runtime.stack_owner_uid = owner.session_uid;
  runtime.stack_state_hash = state_hash;
  runtime.stack_rows_valid = true;
  /* Whatever tree the tree store was last filled by speaks the previous rows' ordinals. */
  runtime.stack_rows_rebuilt_since_build = true;
  /* Headers and scripts ask for the owner's name through RNA, where there is no context of their
   * own to resolve the focus with. */
  runtime.stack_focus_name = owner.name + 2;
}

const StackRow *outliner_stack_row_find(const SpaceOutliner &space_outliner, const int ordinal)
{
  const int *index = space_outliner.runtime->stack_row_index.lookup_ptr(ordinal);
  return (index != nullptr) ? &space_outliner.runtime->stack_rows[*index] : nullptr;
}

ID *outliner_stack_owner_get(const StackReadContext &ctx, SpaceOutliner &space_outliner)
{
  /* Resolving here, so that a focus left pointing at a deleted object drops itself, and the rows
   * cached under it with it, before anything gets to read them. */
  if (outliner_stack_focus_object_resolve(ctx, space_outliner) == nullptr) {
    return nullptr;
  }
  return stack_source_for_space(space_outliner)->owner_get(
      ctx, space_outliner.runtime->stack_focus);
}

int outliner_stack_identity_resolve(const StackReadContext &ctx,
                                    SpaceOutliner &space_outliner,
                                    const StackItemIdentity &identity)
{
  if (!identity.is_valid()) {
    return -1;
  }
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr || owner->session_uid != identity.owner_uid ||
      space_outliner.stack_source != identity.source_type)
  {
    return -1;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  if (!BLI_uuid_is_nil(identity.row_id)) {
    for (const StackRow &row : space_outliner.runtime->stack_rows) {
      if (BLI_uuid_equal(row.stable_id, identity.row_id)) {
        return row.ordinal;
      }
    }
    return -1;
  }
  if (identity.ordinal_hint >= 0 &&
      outliner_stack_row_find(space_outliner, identity.ordinal_hint) != nullptr)
  {
    return identity.ordinal_hint;
  }
  return -1;
}

StackItemIdentity outliner_stack_identity_of(const SpaceOutliner &space_outliner, const int ordinal)
{
  StackItemIdentity identity;
  const SpaceOutliner_Runtime *runtime = space_outliner.runtime;
  if (runtime == nullptr || runtime->stack_owner_uid == 0) {
    return identity;
  }
  identity.owner_uid = runtime->stack_owner_uid;
  identity.source_type = space_outliner.stack_source;
  identity.ordinal_hint = int16_t(ordinal);
  const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
  if (row != nullptr) {
    identity.row_id = row->stable_id;
  }
  return identity;
}

/**
 * The ordinal the operator was given, or the row the user is pointing at when it was left at -1.
 *
 * A marker naming the row by identity comes first, when the operator has one. Then selection:
 * clicking a row is how a layer manager says "this one", and it is what the context menu and the
 * header buttons act on. The activated row -- the one the paint tools write into -- is the
 * fallback, since a stack can be worked on without anything being selected.
 */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op)
{
  const int marker_ordinal = stack_operator_marker_ordinal_get(C, space_outliner, op);
  if (marker_ordinal >= 0) {
    return marker_ordinal;
  }
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

void stack_ordinals_drop_covered_descendants(const SpaceOutliner &space_outliner,
                                             Vector<int> &r_ordinals)
{
  /* A folder in the set already carries every row it holds; a descendant kept alongside it is a
   * second copy of a row that is going to move anyway -- and its group-child ordinal, which is not
   * a position, breaks any code that treats the set as a contiguous run. */
  Set<int> ordinal_set;
  ordinal_set.add_multiple(r_ordinals);
  Vector<int> roots;
  for (const int ordinal : r_ordinals) {
    const StackRow *row = outliner_stack_row_find(space_outliner, ordinal);
    bool has_selected_ancestor = false;
    while (row != nullptr && row->parent_ordinal >= 0) {
      if (ordinal_set.contains(row->parent_ordinal)) {
        has_selected_ancestor = true;
        break;
      }
      row = outliner_stack_row_find(space_outliner, row->parent_ordinal);
    }
    if (!has_selected_ancestor) {
      roots.append(ordinal);
    }
  }
  r_ordinals = std::move(roots);
}

bool outliner_stack_layer_debug_drop(bContext &C,
                                     const SpaceOutliner &source_space,
                                     const int source_ordinal,
                                     SpaceOutliner &space_outliner,
                                     const int target_ordinal)
{
  const StackItemIdentity source_identity = outliner_stack_identity_of(source_space,
                                                                        source_ordinal);
  const StackItemIdentity target_identity = outliner_stack_identity_of(space_outliner,
                                                                        target_ordinal);
  const StackReadContext ctx = outliner_stack_read_context(C);
  const int drag_ordinal = outliner_stack_identity_resolve(ctx, space_outliner, source_identity);
  const int anchor_ordinal = outliner_stack_identity_resolve(
      ctx, space_outliner, target_identity);
  if (drag_ordinal < 0 || anchor_ordinal < 0) {
    return false;
  }
  return outliner_stack_row_move(
      &C, space_outliner, drag_ordinal, anchor_ordinal, StackMovePlace::Above);
}

bool outliner_stack_layer_debug_drop_id(bContext &C,
                                        SpaceOutliner &space_outliner,
                                        const ID &dropped,
                                        const int target_ordinal)
{
  if (space_outliner.runtime == nullptr) {
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackDropHandler *handler = stack_source_for_space(space_outliner)->drop_handler();
  if (handler == nullptr) {
    return false;
  }

  StackDropPayload payload;
  payload.id_uid = dropped.session_uid;
  payload.id_type = GS(dropped.name);

  StackDropTarget target;
  if (target_ordinal >= 0) {
    target.anchor = outliner_stack_identity_of(space_outliner, target_ordinal);
  }

  const char *unused_hint = nullptr;
  if (!handler->can_accept(ctx, *owner, payload, target, &unused_hint)) {
    return false;
  }
  return handler->execute(
      C, space_outliner.runtime->stack_focus, *owner, payload, target, nullptr, nullptr);
}

bool outliner_stack_focus_set(bContext *C,
                              SpaceOutliner &space_outliner,
                              Object &object,
                              const int sub_index,
                              const bool enter_edit_mode)
{
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Base *base = BKE_view_layer_base_find(view_layer, &object);
  if (base == nullptr) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Object is not in the active view layer");
    return false;
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const StackSource &source = *stack_source_for_space(space_outliner);
  if (sub_index < -1) {
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
    return false;
  }
  /* What a sub-index selects among is the source's own business; an empty list means the source
   * has no notion of one at all, and only the default (-1) is valid then. */
  StackFocus probe_focus;
  probe_focus.object_uid = object.id.session_uid;
  Vector<StackSubSelection> sub_selections;
  source.sub_selections_get(ctx, probe_focus, sub_selections);
  /* The object list opens a paint stack at its active material. Store that slot explicitly so the
   * header enum has a concrete current item and can cycle from it with Ctrl+Wheel. */
  const int resolved_sub_index = (sub_index < 0 && !sub_selections.is_empty()) ?
                                     int(object.actcol) - 1 :
                                     sub_index;
  if (resolved_sub_index >= 0) {
    if (resolved_sub_index >= sub_selections.size()) {
      BKE_report(CTX_wm_reports(C), RPT_ERROR, "Stack index is out of range");
      return false;
    }
  }

  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  runtime.stack_focus.object_uid = object.id.session_uid;
  runtime.stack_focus.sub_index = resolved_sub_index;
  outliner_stack_rows_invalidate(space_outliner);
  space_outliner.outlinevis = SO_STACK_LAYERS;
  space_outliner.stack_layers_view = SO_SL_VIEW_STACK;
  ScrArea *area = CTX_wm_area(C);
  if (area != nullptr) {
    for (ARegion &region : area->regionbase) {
      if (region.regiontype != RGN_TYPE_TOOL_HEADER) {
        continue;
      }
      const bool hidden = region.flag & RGN_FLAG_HIDDEN_BY_USER;
      if (bool(region.flag & RGN_FLAG_HIDDEN) != hidden) {
        SET_FLAG_FROM_TEST(region.flag, hidden, RGN_FLAG_HIDDEN);
        ED_area_tag_region_size_update(area, &region);
      }
      ED_region_tag_redraw(&region);
    }
    ED_area_tag_redraw(area);
  }

  /* A stack whose model partly lives in its own data needs a one-time write before its rows can
   * be read -- identities handed out, a legacy shape brought into line. That is an explicit,
   * undoable step tied to opening the stack here, not a side effect of reading it every redraw
   * -- see #StackSource::focus_will_open. */
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner != nullptr && source.focus_will_open(*C, *owner)) {
    ED_undo_push(C, "Open Layer Stack");
  }

  /* A source with no object mode of its own -- editing a shape key is not a mode -- leaves
   * nothing to enter, and asking for one anyway is not an error. */
  const std::optional<eObjectMode> mode = enter_edit_mode ? source.focus_object_mode() :
                                                            std::nullopt;
  if (mode.has_value()) {
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    if (!object::mode_set(C, *mode)) {
      BKE_report(CTX_wm_reports(C), RPT_WARNING, "Could not enter the stack's edit mode");
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
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(space_outliner, nr / STACK_ROW_SUB_ROW_STRIDE);
  if (row == nullptr) {
    return false;
  }
  const int role = nr % STACK_ROW_SUB_ROW_STRIDE;
  Span<StackSubRow> sub_rows;
  const StringRef active_section = outliner_stack_row_active_section_get(space_outliner, *row);
  for (const StackContentSection &section : row->content_sections) {
    if (section.identifier == active_section) {
      sub_rows = section.sub_rows;
      break;
    }
  }
  for (const StackSubRow &sub_row : sub_rows) {
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
                                  SpaceOutliner &space_outliner,
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

bool outliner_stack_can_reorder(const bContext &C, SpaceOutliner &space_outliner)
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  const ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  const StackEditor *editor = stack_source_for_space(space_outliner)->editor();
  return owner != nullptr && editor != nullptr && editor->can_reorder(*owner);
}

bool outliner_stack_row_reorder(bContext *C,
                                SpaceOutliner &space_outliner,
                                const int from_ordinal,
                                const int to_ordinal)
{
  if (from_ordinal == to_ordinal || from_ordinal < 0 || to_ordinal < 0) {
    return false;
  }
  /* Ordinals are positions, so every row past the move has a new one. The moved row keeps the
   * selection -- it is the one the user just acted on -- and the groups they had open have to be
   * asked for by identity rather than by a number that just changed under them. */
  return stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        if (!editor.can_reorder(owner)) {
          return false;
        }
        r_select_ordinal = to_ordinal;
        return editor.row_reorder(*C, focus, owner, from_ordinal, to_ordinal);
      });
}

bool outliner_stack_row_move(bContext *C,
                             SpaceOutliner &space_outliner,
                             const int from_ordinal,
                             const int anchor_ordinal,
                             const StackMovePlace place,
                             int *r_ordinal)
{
  if (from_ordinal < 0 || anchor_ordinal < 0) {
    return false;
  }
  /* Ordinals are positions, so every row past the move has a new one. The moved row itself is the
   * one the user was just working with, so it stays the one selected and active rather than the
   * drop reading as if it landed on nothing. */
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        if (!editor.can_reorder(owner)) {
          return false;
        }
        return editor.row_move(
            *C, focus, owner, from_ordinal, anchor_ordinal, place, &r_select_ordinal);
      },
      &new_ordinal);
  if (!ok) {
    return false;
  }
  if (r_ordinal != nullptr) {
    *r_ordinal = new_ordinal;
  }
  if (new_ordinal >= 0) {
    outliner_stack_row_activate(C, space_outliner, new_ordinal);
  }
  return true;
}

int outliner_stack_row_add(bContext *C,
                           SpaceOutliner &space_outliner,
                           const int kind,
                           const int ordinal,
                           const StackAddArgs &args)
{
  /* Inserting a row renumbers everything above it. */
  int new_ordinal = -1;
  const bool ok = stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int &r_select_ordinal) {
        r_select_ordinal = editor.row_add(*C, focus, owner, kind, ordinal, args);
        return r_select_ordinal >= 0;
      },
      &new_ordinal);
  if (!ok) {
    return -1;
  }
  /* A layer the user just asked for is the one they mean to work on next. */
  outliner_stack_row_activate(C, space_outliner, new_ordinal);
  return new_ordinal;
}

bool outliner_stack_row_fill_color_set(bContext *C,
                                       SpaceOutliner &space_outliner,
                                       const int ordinal,
                                       const float color[4])
{
  /* A re-fill changes pixels and a marker, not the row order, so needs_renumber = false. */
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr) {
          return false;
        }
        return grouping->row_fill_color_set(*C, focus, owner, ordinal, color);
      });
}

bool outliner_stack_row_color_tag_set(bContext *C,
                                      SpaceOutliner &space_outliner,
                                      const int ordinal,
                                      const int color_tag)
{
  /* Color tag is not a stack mutation (it does not renumber), so needs_renumber = false. */
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        const StackGroupingEditor *grouping = editor.grouping();
        if (grouping == nullptr) {
          return false;
        }
        return grouping->row_color_tag_set(*C, focus, owner, ordinal, color_tag);
      });
}

bool outliner_stack_row_set_enabled(bContext *C,
                                    SpaceOutliner &space_outliner,
                                    const int ordinal,
                                    const bool enable)
{
  if (ordinal < 0) {
    return false;
  }
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        return editor.row_set_enabled(*C, focus, owner, ordinal, enable);
      });
}

bool outliner_stack_row_remove(bContext *C, SpaceOutliner &space_outliner, const int ordinal)
{
  if (ordinal < 0) {
    return false;
  }
  /* Taking a row out renumbers everything that was above it, and the row that inherits the removed
   * one's ordinal must not inherit its selection with it. */
  return stack_mutate(
      *C,
      space_outliner,
      true,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) {
        return editor.row_remove(*C, focus, owner, ordinal);
      });
}

char *outliner_stack_row_name_buffer(const SpaceOutliner &space_outliner, const int ordinal)
{
  const StackRow *row = (ordinal < 0) ? nullptr : outliner_stack_row_find(space_outliner, ordinal);
  return (row != nullptr) ? row->name_buffer : nullptr;
}

bool outliner_stack_row_rename(bContext *C,
                               SpaceOutliner &space_outliner,
                               const int ordinal,
                               const StringRefNull name)
{
  if (ordinal < 0 || name.is_empty()) {
    return false;
  }
  /* A rename leaves every ordinal where it was, so the rows come back under the same tree store
   * keys and there is no pending state to carry across. */
  return stack_mutate(
      *C,
      space_outliner,
      false,
      [&](const StackSource & /*source*/,
          const StackEditor &editor,
          const StackFocus &focus,
          ID &owner,
          int & /*r_select_ordinal*/) { return editor.row_rename(*C, focus, owner, ordinal, name); });
}

void outliner_stack_sources_undo_reset()
{
  for (const StackSource *source : stack_sources_get()) {
    source->undo_reset();
  }
}

/**
 * The identity property every operator that addresses an existing row shares: the row's marker,
 * which survives the renumbering an edit puts the position through. Read through
 * #stack_operator_ordinal_get, so a caller names a row either way.
 */
static void rna_def_stack_row_marker(wmOperatorType *ot)
{
  RNA_def_string(ot->srna,
                 "marker",
                 nullptr,
                 0,
                 "Marker",
                 "The row's identity marker, as this mode issued it; takes precedence over the "
                 "position, which is only the row's at the time of the call");
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
      ot->srna, "enter_edit_mode", true, "Enter Edit Mode", "Enter the edit mode the source works in");
}

void OUTLINER_OT_stack_layers_back(wmOperatorType *ot)
{
  ot->name = "Back to Stack Objects";
  ot->idname = "OUTLINER_OT_stack_layers_back";
  ot->description =
      "Return to the View Layer; hold Alt to stay and show the objects that have a layer stack";
  ot->invoke = stack_back_invoke;
  ot->exec = stack_back_exec;
  ot->poll = ED_operator_outliner_active;
  ot->flag = OPTYPE_REGISTER;
  RNA_def_boolean(ot->srna,
                  "keep_view",
                  false,
                  "Keep Mode",
                  "Stay in Stack Layers and show the objects that have a layer stack, rather "
                  "than leaving the mode");
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
  rna_def_stack_row_marker(ot);
}

/* -------------------------------------------------------------------- */
/** \name Stack Layer Preview Section Activate Operator
 * \{ */

static wmOperatorStatus stack_preview_section_activate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int ordinal = RNA_int_get(op->ptr, "ordinal");
  char section_id[64];
  RNA_string_get(op->ptr, "section_id", section_id);

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  outliner_stack_rows_ensure(ctx, *space_outliner, *owner);
  const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* A preview click is also a layer click: bind its texture maps as the paint target and make its
   * row the Outliner's active selection before showing the requested content section. */
  if (!outliner_stack_row_activate(C, *space_outliner, ordinal)) {
    return OPERATOR_CANCELLED;
  }
  tree_iterator::all(*space_outliner, [&](TreeElement *te) {
    TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_LAYER && int(tselem->nr) == ordinal) {
      outliner_item_select(C, space_outliner, te, OL_ITEM_SELECT | OL_ITEM_ACTIVATE);
    }
  });

  /* A layer without a mask still has a CHANNELS preview, but uses legacy flat content rather than
   * a switchable content section. Its preview click must activate the layer without failing. */
  if (!row->content_sections.is_empty() &&
      !outliner_stack_row_active_section_set(*space_outliner, *row, section_id))
  {
    return OPERATOR_CANCELLED;
  }

  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_OUTLINER, nullptr);

  return OPERATOR_FINISHED;
}

static bool stack_preview_section_activate_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  const SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->outlinevis == SO_STACK_LAYERS;
}

void OUTLINER_OT_stack_preview_section_activate(wmOperatorType *ot)
{
  ot->name = "Activate Preview Section";
  ot->idname = "OUTLINER_OT_stack_preview_section_activate";
  ot->description = "Switch the active content section";
  ot->exec = stack_preview_section_activate_exec;
  ot->poll = stack_preview_section_activate_poll;
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_int(ot->srna,
              "ordinal",
              0,
              0,
              SHRT_MAX,
              "Ordinal",
              "Position of the layer in the stack",
              0,
              SHRT_MAX);
  RNA_def_string(ot->srna,
                 "section_id",
                 nullptr,
                 64,
                 "Section ID",
                 "Identifier of the content section to activate (e.g., 'CHANNELS' or 'MASK')");
  rna_def_stack_row_marker(ot);
}

/** \} */

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
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_copy(wmOperatorType *ot)
{
  ot->name = "Copy Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_copy";
  ot->description = "Copy the selected stack layers to the Stack Layers clipboard";
  ot->exec = stack_row_copy_exec;
  ot->poll = stack_row_clipboard_poll;
}

void OUTLINER_OT_stack_layer_paste(wmOperatorType *ot)
{
  ot->name = "Paste Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_paste";
  ot->description = "Paste copied stack layers above the active layer";
  ot->exec = stack_row_paste_exec;
  ot->poll = stack_row_clipboard_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void OUTLINER_OT_stack_layer_add(wmOperatorType *ot)
{
  ot->name = "Add Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_add";
  ot->description = "Add a layer to the stack";
  ot->exec = stack_row_add_exec;
  ot->invoke = stack_row_add_invoke;
  ot->ui = stack_row_add_ui;
  ot->poll = stack_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* What the kinds are is the source's own vocabulary, built into items as the menu asks for
   * them; a source that declares none is polled out entirely. */
  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "type", rna_enum_dummy_NULL_items, 0, "Type", "What the new layer holds");
  RNA_def_enum_funcs(prop, stack_add_type_itemf);
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Row to anchor the new layer to; it lands above that row, or inside it when the row "
              "is a folder. -1 uses the selected or active row, or the top of the stack",
              -1,
              SHRT_MAX);
  /* Read only by the kinds that asked for a colour; the others leave it at its white default. */
  PropertyRNA *fill_prop = RNA_def_float_color(ot->srna,
                                               "fill_color",
                                               4,
                                               nullptr,
                                               0.0f,
                                               FLT_MAX,
                                               "Fill Color",
                                               "Color a fill layer starts out filled with",
                                               0.0f,
                                               1.0f);
  RNA_def_property_subtype(fill_prop, PROP_COLOR_GAMMA);
  /* #layout.tag_button writes its tag name into every operator it attaches; the Add never reads
   * it, but the property keeps that write from warning on every redraw. */
  PropertyRNA *tag_prop = RNA_def_string(
      ot->srna, "tag_name", "", 0, "Tag Name", "Tag the button that placed the call belongs to");
  RNA_def_property_flag(tag_prop, PROP_HIDDEN);
  /* The data-block a kind with #StackAddKindInfo::source_id_type is made from, by name: what a UI
   * picked, and what a repeat or a script names again. Bounded, since the exec reads it into an ID
   * name buffer. */
  PropertyRNA *source_prop = RNA_def_string(ot->srna,
                                            "source",
                                            "",
                                            MAX_ID_NAME - 2,
                                            "Source",
                                            "Name of the data-block the new layer is made from, "
                                            "for a kind that is made from one");
  RNA_def_property_flag(source_prop, PROP_HIDDEN);
  rna_def_stack_row_marker(ot);
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
  rna_def_stack_row_marker(ot);
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
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row an ungroup would land on is a group.
 *
 * Only a group folder can be unwrapped, and the context menu leans on this poll to show the entry
 * only where it applies. The poll cannot read the operator's marker, so it answers for what the
 * operator will do without one: the selection if there is one, the active row otherwise. The exec
 * still takes a marker, and the grouping editor refuses an ungroup for anything that is not a
 * group.
 */
bool stack_row_ungroup_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ? selected.first() :
                                             outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
  const StackRow *row = (ordinal < 0) ? nullptr : outliner_stack_row_find(*space_outliner, ordinal);
  return row != nullptr && row->can_hold_children;
}

void OUTLINER_OT_stack_layer_ungroup(wmOperatorType *ot)
{
  ot->name = "Ungroup Stack Layers";
  ot->idname = "OUTLINER_OT_stack_layer_ungroup";
  ot->description = "Put the layers of a group back into the stack around it";
  ot->exec = stack_row_ungroup_exec;
  ot->poll = stack_row_ungroup_poll;
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
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row a color tag would land on is a group.
 *
 * Color tags belong to group folders, and the UI that offers them -- the context menu's color
 * swatches -- leans on this poll to appear only where they apply. The poll cannot read the
 * operator's marker, so it answers for what the operator will do without one: the selection if
 * there is one, the active row otherwise. The exec still takes a marker, and the source refuses
 * a tag for anything that is not a group.
 */
bool stack_row_color_tag_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ? selected.first() :
                                             outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
  const StackRow *row = (ordinal < 0) ? nullptr : outliner_stack_row_find(*space_outliner, ordinal);
  return row != nullptr && row->can_hold_children;
}

static wmOperatorStatus stack_row_color_tag_set_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const int color_tag = RNA_enum_get(op->ptr, "color");
  return outliner_stack_row_color_tag_set(C, *space_outliner, ordinal, color_tag) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

void OUTLINER_OT_stack_layer_color_tag_set(wmOperatorType *ot)
{
  /* Match collection_color_tag_set's color enum: 8 colors, NONE at index 0 (becomes -1). */
  static const EnumPropertyItem color_items[] = {
      {-1, "NONE", ICON_X, "None", ""},
      {0, "COLOR_01", ICON_COLLECTION_COLOR_01, "Color 01", ""},
      {1, "COLOR_02", ICON_COLLECTION_COLOR_02, "Color 02", ""},
      {2, "COLOR_03", ICON_COLLECTION_COLOR_03, "Color 03", ""},
      {3, "COLOR_04", ICON_COLLECTION_COLOR_04, "Color 04", ""},
      {4, "COLOR_05", ICON_COLLECTION_COLOR_05, "Color 05", ""},
      {5, "COLOR_06", ICON_COLLECTION_COLOR_06, "Color 06", ""},
      {6, "COLOR_07", ICON_COLLECTION_COLOR_07, "Color 07", ""},
      {7, "COLOR_08", ICON_COLLECTION_COLOR_08, "Color 08", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Set Layer Group Color";
  ot->idname = "OUTLINER_OT_stack_layer_color_tag_set";
  ot->description = "Set the color tag of a layer group folder";
  ot->exec = stack_row_color_tag_set_exec;
  ot->poll = stack_row_color_tag_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "color", color_items, -1, "Color", "Color tag for the group");
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Group to set color for; -1 uses the active row",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

/**
 * Whether the row a fill color would land on is a fill row.
 *
 * A re-fill is refused for any row that does not stand for a colour, and the context menu leans on
 * this poll to show the entry only where it applies. The poll cannot read the operator's marker,
 * so it answers for what the operator will do without one: the selection if there is one, the
 * active row otherwise. The row's colour slot is what says it is a fill -- the source drew it
 * there for exactly this question.
 */
bool stack_row_fill_color_poll(bContext *C)
{
  if (!stack_row_add_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ? selected.first() :
                                             outliner_stack_active_ordinal_get(
                                                 outliner_stack_read_context(*C), *space_outliner);
  const StackRow *row = (ordinal < 0) ? nullptr : outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  for (const StackRowPreview &slot : row->preview_slots) {
    if (slot.is_color_swatch) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus stack_row_fill_color_set_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  float color[4];
  RNA_float_get_array(op->ptr, "color", color);
  return outliner_stack_row_fill_color_set(C, *space_outliner, ordinal, color) ?
             OPERATOR_FINISHED :
             OPERATOR_CANCELLED;
}

static wmOperatorStatus stack_row_fill_color_set_invoke(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent * /*event*/)
{
  /* The dialog starts at the colour the row stands for now, which the source already put in the
   * row's swatch -- a picker that opens at white is a picker that loses the current colour. */
  PropertyRNA *color_prop = RNA_struct_find_property(op->ptr, "color");
  if (!RNA_property_is_set(op->ptr, color_prop)) {
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
    const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
    if (const StackRow *row = (ordinal < 0) ?
                                  nullptr :
                                  outliner_stack_row_find(*space_outliner, ordinal))
    {
      for (const StackRowPreview &slot : row->preview_slots) {
        if (slot.is_color_swatch) {
          RNA_property_float_set_array(op->ptr, color_prop, slot.color);
          break;
        }
      }
    }
  }
  return WM_operator_props_dialog_popup(C, op, 220, IFACE_("Fill Color"), IFACE_("Fill"));
}

/** The colour is the only choice here; the row was decided by where the call came from. */
static void stack_row_fill_color_set_ui(bContext * /*C*/, wmOperator *op)
{
  op->layout->prop(op->ptr, "color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void OUTLINER_OT_stack_layer_fill_color_set(wmOperatorType *ot)
{
  ot->name = "Set Fill Color";
  ot->idname = "OUTLINER_OT_stack_layer_fill_color_set";
  ot->description = "Re-fill a fill layer with a new color, replacing what was drawn on it";
  ot->exec = stack_row_fill_color_set_exec;
  ot->invoke = stack_row_fill_color_set_invoke;
  ot->ui = stack_row_fill_color_set_ui;
  ot->poll = stack_row_fill_color_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_float_color(ot->srna,
                                          "color",
                                          4,
                                          nullptr,
                                          0.0f,
                                          FLT_MAX,
                                          "Color",
                                          "Color the fill layer stands for",
                                          0.0f,
                                          1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR_GAMMA);
  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Fill layer to re-fill; -1 uses the active row",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_merge_down(wmOperatorType *ot)
{
  ot->name = "Merge Layer Down";
  ot->idname = "OUTLINER_OT_stack_layer_merge_down";
  ot->description =
      "Collapse a layer and the one below it into one group row that composites just the same";
  ot->exec = stack_row_merge_down_exec;
  ot->poll = stack_row_add_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "ordinal",
              -1,
              -1,
              SHRT_MAX,
              "Ordinal",
              "Layer to merge into the one below it; -1 uses the active one",
              -1,
              SHRT_MAX);
  rna_def_stack_row_marker(ot);
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
  rna_def_stack_row_marker(ot);
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
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_mask(wmOperatorType *ot)
{
  static const EnumPropertyItem initial_color_items[] = {
      {0, "WHITE", 0, "Add White Mask", "Add a mask filled with white"},
      {1, "BLACK", 0, "Add Black Mask", "Add a mask filled with black"},
      {0, nullptr, 0, nullptr, nullptr},
  };

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
  RNA_def_enum(ot->srna,
               "initial_color",
               initial_color_items,
               0,
               "Initial Color",
               "Initial fill color for a newly added mask");
  rna_def_stack_row_marker(ot);
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
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_remove(wmOperatorType *ot)
{
  ot->name = "Remove Stack Layer";
  ot->idname = "OUTLINER_OT_stack_layer_remove";
  ot->description = "Delete a layer from the stack";
  ot->exec = stack_row_remove_exec;
  ot->poll = stack_row_remove_poll;
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
  rna_def_stack_row_marker(ot);
}

}  // namespace blender::ed::outliner
