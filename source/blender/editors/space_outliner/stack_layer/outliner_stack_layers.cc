/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
#include <set>
#include <string>

#include "DNA_ID.h"
#include "DNA_defs.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_color.h"
#include "BLI_mempool.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "ED_object.hh"
#include "ED_outliner_stack.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_iterator.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_layers_intern.hh"


namespace blender::ed::outliner {

/** Defined below: the shared resolver of the operators that address one row, marker first. */
int stack_operator_ordinal_get(bContext &C, SpaceOutliner &space_outliner, wmOperator &op);
const ed::outliner::StackRow *outliner_stack_row_resolve(const SpaceOutliner &space_outliner,
                                                         const bUUID &row_id,
                                                         int ordinal_hint);

/**
 * Whether the operator runs in the Stack Layers display. The View Layer and other display modes
 * share the Outliner's keys (X, Shift+D), so a stack operator must refuse outside it or it takes
 * those keys from the data-block operators.
 */
bool stack_mode_poll(bContext *C)
{
  if (!ED_operator_outliner_active(C)) {
    return false;
  }
  const SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return space_outliner != nullptr && space_outliner->runtime != nullptr &&
         space_outliner->outlinevis == SO_STACK_LAYERS;
}

bool stack_row_activate_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->is_editable(*owner);
}

wmOperatorStatus stack_row_activate_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  return outliner_stack_row_activate(C, *space_outliner, ordinal) ? OPERATOR_FINISHED :
                                                                    OPERATOR_CANCELLED;
}

/**
 * The ordinal of the row the operator's \a marker names, -1 when it was given none, or
 * #STACK_MARKER_UNRESOLVED when it names a row that is gone: falling back to the selection then
 * would act on a row the caller never asked for.
 *
 * A marker is the row's identity -- #StackRow::stable_id, as an earlier call of the mode handed it
 * out. A script that re-addresses a row by its marker gets the same row after an edit moved it,
 * where a remembered ordinal is the position-trap the seam's own code avoids through that same
 * marker; the position properties stay for UI buttons, which speak for the row on screen at the
 * moment of the call.
 */

int stack_operator_marker_ordinal_get(bContext &C,
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
    return STACK_MARKER_UNRESOLVED;
  }
  const StackReadContext ctx = outliner_stack_read_context(C);
  ID *owner = outliner_stack_owner_get(ctx, space_outliner);
  if (owner == nullptr) {
    return STACK_MARKER_UNRESOLVED;
  }
  outliner_stack_rows_ensure(ctx, space_outliner, *owner);
  const StackRow *row = outliner_stack_row_resolve(space_outliner, marker, -1);
  return (row != nullptr) ? row->ordinal : STACK_MARKER_UNRESOLVED;
}

/**
 * The ordinal \a row shifts to for a one-step move up or down the listed order, or -1 when no
 * sibling sits in that direction.
 *
 * Rows attached to a parent's content section share the run of ordinals with the plain rows around
 * them but only reorder among themselves, so the step walks past everything the sibling predicate
 * refuses -- an attached row never lands among plain rows, and a plain row never lands between
 * another row's attached rows.
 */
static int stack_row_sibling_ordinal_find(const SpaceOutliner &space_outliner,
                                          const StackRow &row,
                                          const bool move_up)
{
  const int step = move_up ? 1 : -1;
  for (int candidate = row.ordinal + step; candidate >= 0 && candidate <= STACK_ROW_ORDINAL_MAX;
       candidate += step)
  {
    const StackRow *candidate_row = outliner_stack_row_find(space_outliner, candidate);
    if (candidate_row != nullptr && stack_rows_are_siblings(row, *candidate_row)) {
      return candidate;
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
  if (ordinal == STACK_MARKER_UNRESOLVED) {
    BKE_report(op->reports, RPT_ERROR, "The marker names no row in this stack");
    return OPERATOR_CANCELLED;
  }
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

  /* Move each row one sibling step in the direction, resolving its ordinal fresh: every move
   * renumbers the stack, so an ordinal read before a move can name a different row after it. The
   * step lands on the nearest sibling rather than the next ordinal -- rows attached to a parent's
   * section and rows under another parent share the run of ordinals but reorder among themselves
   * only, so they are passed over rather than landed between. */
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
    const StackRow *current_row = outliner_stack_row_find(*space_outliner, current_ordinal);
    if (current_row == nullptr) {
      continue;
    }
    const int target_ordinal = stack_row_sibling_ordinal_find(
        *space_outliner, *current_row, move_up);
    if (target_ordinal < 0) {
      continue;
    }
    if (outliner_stack_row_reorder(C, *space_outliner, current_ordinal, target_ordinal)) {
      any_moved = true;
    }
  }

  return any_moved ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
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

wmOperatorStatus stack_row_mask_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  const bool add = RNA_boolean_get(op->ptr, "add");
  /* The seam takes a fill colour, so a third kind of mask is a new item on this menu, not a new
   * parameter carried through every layer between here and the description. */
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
               grouping->row_aux_set(*C, focus, owner, ordinal, add, initial_color);
      });
  if (ok && add) {
    /* A mask the user just asked for is the one they mean to work on: the layer it belongs to
     * becomes the selected and active row, even when the Add was aimed at another one. */
    outliner_stack_row_activate(C, *space_outliner, ordinal);
    /* The row now has a mask section; show it, as a click on the mask thumbnail would. The
     * mutation rebuilt the rows, so the row is looked up again. */
    const StringRefNull aux_section = stack_aux_section_id(*space_outliner);
    if (const StackRow *rebuilt = outliner_stack_row_find(*space_outliner, ordinal)) {
      if (!aux_section.is_empty()) {
        outliner_stack_row_active_section_set(*space_outliner, *rebuilt, aux_section);
      }
    }
  }
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

bool stack_row_mask_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

bool stack_row_mask_section_poll(bContext *C)
{
  if (!stack_row_mask_poll(C)) {
    return false;
  }
  /* The row a header button or the context menu would act on: the selected one, else the active
   * one, as #stack_operator_ordinal_get answers for an operator with no ordinal of its own. */
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StringRefNull aux_section = stack_aux_section_id(*space_outliner);
  if (aux_section.is_empty()) {
    return false;
  }
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ?
                          selected.first() :
                          outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                            *space_outliner);
  const StackRow *row = outliner_stack_row_find(*space_outliner, ordinal);
  if (row == nullptr) {
    return false;
  }
  /* A row hung on its parent's section (a mask correction) has a mask slot of its own, but no mask
   * to copy: the layer's whole mask is the parent's. Left to the layer copy, which carries the
   * correction itself and lets it be pasted onto the mask of another layer. */
  if (!row->parent_section_id.empty()) {
    return false;
  }
  /* Only a mask slot that is the one showing: a row with no mask section has nothing to act on. */
  bool has_section = false;
  for (const StackContentSection &section : row->content_sections) {
    has_section |= section.identifier == aux_section;
  }
  return has_section && outliner_stack_row_active_section_get(*space_outliner, *row) == aux_section;
}

wmOperatorStatus stack_row_mask_toggle_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const int ordinal = stack_operator_ordinal_get(*C, *space_outliner, *op);
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
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
        return grouping != nullptr && grouping->row_aux_toggle(*C, focus, owner, ordinal);
      });
  return ok ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

bool stack_row_visibility_toggle_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && stack_source_for_space(*space_outliner)->can_edit(*owner);
}

bool stack_row_reorder_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  return outliner_stack_can_reorder(*C, *space_outliner);
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
  if (marker_ordinal == STACK_MARKER_UNRESOLVED) {
    /* Not the selection: the caller named a row, and it is gone. */
    return -1;
  }
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
   * a position, breaks any code that treats the set as a contiguous run. A row attached to a
   * parent's content section needs no separate case: its #StackRow::parent_ordinal is the row it
   * hangs under, so selecting that row drops it from the set with everything else the walk lifts
   * out. */
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
  return stack_mutate(*C,
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

bool outliner_stack_row_set_enabled(bContext *C,
                                    SpaceOutliner &space_outliner,
                                    const int ordinal,
                                    const bool enable)
{
  if (ordinal < 0) {
    return false;
  }
  return stack_mutate(*C,
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
  if (ordinal < 0) {
    return OPERATOR_CANCELLED;
  }
  /* The pair reads as one folder row from then on, and that row is what the user works with. */
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
  /* The copy sits directly above the original and becomes the row the user works on next. */
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
          int & /*r_select_ordinal*/) {
        return editor.row_rename(*C, focus, owner, ordinal, name);
      });
}

}  // namespace blender::ed::outliner
