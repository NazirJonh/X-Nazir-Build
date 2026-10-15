/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <algorithm>
#include <climits>
#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_space_types.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"

#include "BLI_span.hh"
#include "BLI_uuid.h"

#include "ED_screen.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_drag.hh"
#include "outliner_stack_source.hh"

namespace blender {

namespace ed::outliner {

/** \name Stack Layer Drop Operator
 *
 * Reordering a row of the Stack Layers display mode.
 *
 * A path of its own rather than an extension of the data-stack drop in `outliner_dragdrop.cc`:
 * that one models ownership by an #Object or a #bPoseChannel and moves a #ListBase entry, and a
 * paint layer is neither -- it is a position in a node graph, and which data-block owns it is the
 * business of the mode's #StackSource. The two share the drop-indicator helpers and nothing else.
 * \{ */

/** Cap on walking up a row's parents, so a corrupt parent chain cannot loop forever. */
constexpr int STACK_ROW_PARENT_WALK_MAX = 64;

void stack_layer_drop_data_init(SpaceOutliner &space_outliner,
                                wmDrag *drag,
                                const TreeStoreElem &tselem)
{
  /* #MEM_new, as the Vector member requires non-trivial construction. */
  StackLayerDragData *drop_data = MEM_new<StackLayerDragData>("stack layer drop data");

  /* Fill from selection: all selected rows move together. The row being dragged must be included
   * even if it is not selected (standard Blender drag behavior). Every selected row is carried, so
   * the selection is whole again after the move; the rows actually *moved* are the roots of that
   * set -- a folder already brings its contents -- and the drop prunes the list itself. */
  Vector<int> selected_ordinals;
  stack_selected_ordinals_get(space_outliner, selected_ordinals);
  const int dragged_ordinal = int(tselem.nr);
  if (!selected_ordinals.contains(dragged_ordinal)) {
    selected_ordinals.append(dragged_ordinal);
  }

  for (const int ordinal : selected_ordinals) {
    const StackItemIdentity identity = outliner_stack_identity_of(space_outliner, ordinal);
    if (identity.is_valid()) {
      drop_data->drag_rows.append(identity);
    }
  }

  /* Sort by the position the rows had to preserve their relative order during the move. */
  std::sort(drop_data->drag_rows.begin(),
            drop_data->drag_rows.end(),
            [](const StackItemIdentity &a, const StackItemIdentity &b) {
              return a.ordinal_hint < b.ordinal_hint;
            });

  drag->poin = drop_data;
  drag->poin_free_fn = stack_layer_drag_payload_free;
  drag->flags |= WM_DRAG_FREE_DATA;
}

/** The stack row under the cursor and where in it the cursor is. */
struct StackDropZone {
  TreeElement *te = nullptr;
  TreeElementInsertType insert_type = TE_INSERT_BEFORE;
  /** How far up the row the cursor is: 0 at its bottom edge, 1 at its top edge. */
  float offset_fraction = 0.0f;
};

/**
 * Find the stack row under the cursor and which of its three zones the cursor is in.
 *
 * A path of its own rather than #outliner_drop_insert_find, which answers a different question: it
 * redirects a drop near the bottom edge of an open element to that element's first child, so the
 * area under an expanded group would mean "into the group" when the user is aiming below it, and
 * it scales the "into" zone to half the row, which on a two-unit row leaves almost nowhere to aim
 * above. Here every row is split in three even parts: above, into, below.
 */
static bool stack_layer_drop_zone_find(bContext *C, const int xy[2], StackDropZone &r_zone)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  if (space_outliner->runtime->tree.is_empty()) {
    return false;
  }

  float view_mval[2];
  ui::view2d_region_to_view(&region->v2d,
                            xy[0] - region->winrct.xmin,
                            xy[1] - region->winrct.ymin,
                            &view_mval[0],
                            &view_mval[1]);
  TreeElement *te = outliner_find_item_at_y(
      space_outliner, &space_outliner->runtime->tree, view_mval[1]);
  if (te == nullptr) {
    /* Past the ends of the list: above the first row, or below the last one. */
    TreeElement *first = static_cast<TreeElement *>(space_outliner->runtime->tree.first);
    TreeElement *last = static_cast<TreeElement *>(space_outliner->runtime->tree.last);
    const bool below_last = view_mval[1] < last->ys;
    r_zone.te = below_last ? last : first;
    r_zone.insert_type = below_last ? TE_INSERT_AFTER : TE_INSERT_BEFORE;
    r_zone.offset_fraction = below_last ? 0.0f : 1.0f;
    return true;
  }

  const float height = float(outliner_tree_element_height(*space_outliner, *te));
  r_zone.te = te;
  r_zone.offset_fraction = (view_mval[1] - float(te->ys)) / height;
  if (r_zone.offset_fraction > 2.0f / 3) {
    r_zone.insert_type = TE_INSERT_BEFORE;
  }
  else if (r_zone.offset_fraction < 1.0f / 3) {
    r_zone.insert_type = TE_INSERT_AFTER;
  }
  else {
    r_zone.insert_type = TE_INSERT_INTO;
  }
  return true;
}

/** Whether \a ordinal is \a group_ordinal itself or a row that group holds, however deeply. */
static bool stack_row_is_within(const SpaceOutliner &space_outliner,
                                const int ordinal,
                                const int group_ordinal)
{
  int walk = ordinal;
  for (int step = 0; walk >= 0 && step < STACK_ROW_PARENT_WALK_MAX; step++) {
    if (walk == group_ordinal) {
      return true;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, walk);
    if (row == nullptr) {
      break;
    }
    walk = row->parent_ordinal;
  }
  return false;
}

/**
 * Whether the target is inside the dragged block.
 *
 * The target being one of the block's rows is the obvious case -- a row cannot be dropped onto
 * itself -- but aiming between two of the block's own rows is the same mistake: wherever the
 * block goes while moving, the position the drop names is one the block itself vacates and
 * refills, and honoring it scrambles the rows rather than moving them.
 */
static bool stack_drop_target_in_block(const StackReadContext &ctx,
                                       SpaceOutliner &space_outliner,
                                       const Span<StackItemIdentity> drag_rows,
                                       const int target_ordinal)
{
  int block_low = INT_MAX;
  int block_high = INT_MIN;
  for (const StackItemIdentity &drag_row : drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, space_outliner, drag_row);
    if (drag_ordinal < 0) {
      /* A row that no longer resolves is not part of the block: -1 in the range would stretch it
       * over the whole stack and reject drops that belong inside it. */
      continue;
    }
    if (drag_ordinal == target_ordinal) {
      return true;
    }
    /* The block is the contiguous run of top-level rows being moved. A row inside a folder is not
     * a position in that run -- its group-child ordinal is not comparable to a top-level one, and
     * it only moves because its folder does. Aiming inside a dragged folder is caught by
     * #stack_row_is_within. */
    const StackRow *drag_stack_row = outliner_stack_row_find(space_outliner, drag_ordinal);
    if (drag_stack_row != nullptr && drag_stack_row->parent_ordinal >= 0) {
      continue;
    }
    block_low = std::min(block_low, drag_ordinal);
    block_high = std::max(block_high, drag_ordinal);
  }
  return block_low <= block_high && target_ordinal > block_low && target_ordinal < block_high;
}

/**
 * Apply the rules for rows attached to a parent's content section, which may refine
 * \a io_insert_type. Returns false when the drop has to be refused.
 *
 * A row attached to a parent's content section only ever lands among its own kind: beside a
 * sibling from the same section, and never inside anything -- "into" would read as joining the
 * target's content, which is not a move between rows at all. A plain row, in turn, never lands
 * beside an attached one: the run between a parent's attached rows is that section's own
 * ordering, not a place among the plain rows.
 */
static bool stack_drop_attached_rules_apply(const StackReadContext &ctx,
                                            SpaceOutliner &space_outliner,
                                            const Span<StackItemIdentity> drag_rows,
                                            const StackRow &target_row,
                                            const float offset_fraction,
                                            TreeElementInsertType &io_insert_type)
{
  for (const StackItemIdentity &drag_row : drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, space_outliner, drag_row);
    const StackRow *drag_stack_row = (drag_ordinal >= 0) ?
                                         outliner_stack_row_find(space_outliner, drag_ordinal) :
                                         nullptr;
    if (drag_stack_row == nullptr) {
      continue;
    }
    if (drag_stack_row->parent_section_id.empty()) {
      if (!target_row.parent_section_id.empty()) {
        return false;
      }
      continue;
    }
    if (!stack_rows_are_siblings(*drag_stack_row, target_row)) {
      return false;
    }
    if (io_insert_type == TE_INSERT_INTO) {
      /* An attached row has no inside to land in, and its rows are only one line high, so the
       * middle third of one is a dead zone. It reads as "put it here": above or below the row by
       * the half of it the pointer is in. */
      io_insert_type = (offset_fraction > 0.5f) ? TE_INSERT_BEFORE : TE_INSERT_AFTER;
    }
  }
  return true;
}

/** Whether any dragged group would end up inside itself when dropped on \a target_row. */
static bool stack_drop_group_into_itself(const StackReadContext &ctx,
                                         SpaceOutliner &space_outliner,
                                         const Span<StackItemIdentity> drag_rows,
                                         const StackRow &target_row)
{
  for (const StackItemIdentity &drag_row : drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, space_outliner, drag_row);
    if (drag_ordinal < 0) {
      continue;
    }
    const StackRow *drag_stack_row = outliner_stack_row_find(space_outliner, drag_ordinal);
    if (drag_stack_row != nullptr && drag_stack_row->can_hold_children &&
        stack_row_is_within(space_outliner, target_row.ordinal, drag_stack_row->ordinal))
    {
      return true;
    }
  }
  return false;
}

/** Write the drop target and the indicator for the zone the cursor is in, or refuse the drop. */
static bool stack_layer_drop_init(bContext *C, const wmEvent *event, StackLayerDragData *drop_data)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  if (!outliner_stack_can_reorder(*C, *space_outliner)) {
    return false;
  }

  /* Check that at least one dragged row resolves in this space. Rows may have been dragged from a
   * different Outliner showing a different stack (different owner or source). Resolving against
   * *this* space's stack refuses a drop between two spaces on different stacks. */
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const Span<StackItemIdentity> drag_rows = drop_data->drag_rows;
  const bool any_resolved = std::any_of(
      drag_rows.begin(), drag_rows.end(), [&](const StackItemIdentity &drag_row) {
        return outliner_stack_identity_resolve(ctx, *space_outliner, drag_row) >= 0;
      });
  if (!any_resolved) {
    return false;
  }

  StackDropZone zone;
  if (!stack_layer_drop_zone_find(C, event->xy, zone) ||
      TREESTORE(zone.te)->type != TSE_STACK_LAYER)
  {
    return false;
  }
  const int target_ordinal = int(TREESTORE(zone.te)->nr);
  if (stack_drop_target_in_block(ctx, *space_outliner, drag_rows, target_ordinal)) {
    return false;
  }
  const StackRow *target_row = outliner_stack_row_find(*space_outliner, target_ordinal);
  if (target_row == nullptr) {
    return false;
  }

  TreeElementInsertType insert_type = zone.insert_type;
  if (!stack_drop_attached_rules_apply(
          ctx, *space_outliner, drag_rows, *target_row, zone.offset_fraction, insert_type))
  {
    return false;
  }
  if (stack_drop_group_into_itself(ctx, *space_outliner, drag_rows, *target_row)) {
    return false;
  }
  if (insert_type == TE_INSERT_INTO && !target_row->can_hold_children) {
    /* Only a row that can hold children has an inside. Aiming at the middle of a plain row is not
     * a mistake worth refusing, though -- it reads as "put it here", above the row it points
     * at. */
    insert_type = TE_INSERT_BEFORE;
  }

  /* The row aimed at is the anchor in every case, a group included: "into that folder" is a place
   * of its own, so an empty folder -- which has no row inside to aim at -- is a destination like
   * any other. The list is drawn top of stack first, so "before" on screen is "above". */
  StackMovePlace place = StackMovePlace::Above;
  if (insert_type == TE_INSERT_INTO) {
    place = StackMovePlace::Into;
  }
  else if (insert_type == TE_INSERT_AFTER) {
    place = StackMovePlace::Below;
  }

  drop_data->target = outliner_stack_identity_of(*space_outliner, target_row->ordinal);
  drop_data->target_place = place;
  /* The indicator points at *this* space's tree: it lives here, not on the drag, which may have
   * come from a different window and outlives the tree it was born over. */
  space_outliner->runtime->stack_drop_indicator.ordinal = target_ordinal;
  space_outliner->runtime->stack_drop_indicator.insert_type = insert_type;
  return true;
}

bool stack_layer_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (drag->type != WM_DRAG_STACK_LAYER) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  const bool changed = outliner_flag_set(
      *space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);

  /* Like the other drop polls of the Outliner, this one writes the state the drop and the drawing
   * read: the target on the payload and the indicator on the space. */
  StackLayerDragData *drop_data = static_cast<StackLayerDragData *>(drag->poin);
  if (drop_data == nullptr || !stack_layer_drop_init(C, event, drop_data)) {
    /* Nowhere to drop here: the line goes away with the refusal. */
    space_outliner->runtime->stack_drop_indicator.ordinal = -1;
    if (changed) {
      ED_region_tag_redraw_no_rebuild(region);
    }
    return false;
  }

  /* What is drawn comes from the runtime indicator #stack_layer_drop_init just wrote, not from the
   * tree store's drag flags: every drop poll in the Outliner clears those before deciding, so a
   * flag set here survives only until the next poll of a box that refuses. */
  ED_region_tag_redraw_no_rebuild(region);
  return true;
}

std::string stack_layer_drop_tooltip(bContext *C,
                                     wmDrag * /*drag*/,
                                     const int /*xy*/[2],
                                     wmDropBox * /*drop*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr &&
      space_outliner->runtime->stack_drop_indicator.insert_type == TE_INSERT_INTO)
  {
    return TIP_("Move layer into group");
  }
  return TIP_("Reorder layer");
}

/**
 * The rows actually moved are the roots of the dragged set: a dragged folder already carries its
 * dragged descendants, and moving those again would pull them back out of it. Pruned once, by the
 * ordinals the rows have now, before the first move renumbers anything.
 */
static Vector<StackItemIdentity> stack_drop_rows_to_move(const StackReadContext &ctx,
                                                         SpaceOutliner &space_outliner,
                                                         const Span<StackItemIdentity> drag_rows)
{
  Vector<int> root_ordinals;
  for (const StackItemIdentity &drag_row : drag_rows) {
    const int ordinal = outliner_stack_identity_resolve(ctx, space_outliner, drag_row);
    if (ordinal >= 0) {
      root_ordinals.append(ordinal);
    }
  }
  stack_ordinals_drop_covered_descendants(space_outliner, root_ordinals);

  Vector<StackItemIdentity> rows_to_move;
  for (const int ordinal : root_ordinals) {
    rows_to_move.append(outliner_stack_identity_of(space_outliner, ordinal));
  }
  return rows_to_move;
}

/**
 * Put the selection and the active row back after a move.
 *
 * Each move activates the row it moved, including in the source data. The source's active row goes
 * back, then the selection of every dragged identity: a drag of a block moves rows, it must not
 * turn that block into a single selected row after it lands below itself.
 */
static void stack_drop_ui_state_restore(bContext *C,
                                        const StackReadContext &ctx,
                                        SpaceOutliner &space_outliner,
                                        const Span<StackItemIdentity> drag_rows,
                                        const StackItemIdentity &active_before)
{
  SpaceOutliner_Runtime &runtime = *space_outliner.runtime;
  const int active_ordinal_after = outliner_stack_identity_resolve(
      ctx, space_outliner, active_before);
  if (active_ordinal_after >= 0) {
    outliner_stack_row_activate(C, space_outliner, active_ordinal_after);
  }
  for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
    state.selected = false;
    state.active = false;
  }
  for (const StackItemIdentity &drag_row : drag_rows) {
    /* A row the source gives no identity for cannot be addressed by this map; keying it at nil
     * would fold every such row into one shared state. */
    if (outliner_stack_identity_resolve(ctx, space_outliner, drag_row) < 0 ||
        BLI_uuid_is_nil(drag_row.row_id))
    {
      continue;
    }
    runtime.stack_row_ui_state.lookup_or_add_default(drag_row.row_id).selected = true;
  }
  if (active_ordinal_after >= 0 && !BLI_uuid_is_nil(active_before.row_id)) {
    StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(active_before.row_id);
    state.active = true;
    state.selected = true;
  }
}

static wmOperatorStatus stack_layer_drop_invoke(bContext *C,
                                                wmOperator * /*op*/,
                                                const wmEvent *event)
{
  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }
  ListBaseT<wmDrag> *lb = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);
  StackLayerDragData *drop_data = static_cast<StackLayerDragData *>(drag->poin);
  if (drop_data == nullptr || !drop_data->target.is_valid() || drop_data->drag_rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const StackItemIdentity anchor_identity = drop_data->target;
  const StackMovePlace place = drop_data->target_place;

  /* The row that was active keeps the role through the move. The mutator hands the selection to
   * whichever row it moved last, which reads right for a single-row drop and wrong for a block:
   * a drop moves rows, it does not change which row the user is painting with. */
  const int active_ordinal_before = outliner_stack_active_ordinal_get(ctx, *space_outliner);
  const StackItemIdentity active_before = (active_ordinal_before >= 0) ?
                                              outliner_stack_identity_of(*space_outliner,
                                                                         active_ordinal_before) :
                                              StackItemIdentity();

  Vector<StackItemIdentity> rows_to_move = stack_drop_rows_to_move(
      ctx, *space_outliner, drop_data->drag_rows);
  /* Ordinals count from the bottom of the stack, and each row after the first is put below the one
   * before it. The block is therefore walked from its top row down, or it lands reversed. */
  std::stable_sort(rows_to_move.begin(),
                   rows_to_move.end(),
                   [&](const StackItemIdentity &a, const StackItemIdentity &b) {
    return outliner_stack_identity_resolve(ctx, *space_outliner, a) >
           outliner_stack_identity_resolve(ctx, *space_outliner, b);
  });

  /* Move all dragged rows, preserving their relative order. The first row goes where the drop
   * asked, and every row after it goes right below the one moved before it: moving each against
   * the original anchor would stack the block in reverse. Into a group reads the same way -- the
   * first row into the folder, the rest lined up under it inside.
   *
   * Identities are resolved before each move: #StackEditor::row_move renumbers the stack, so one
   * read at the top of the loop is stale by the time the loop reaches the second row. This is the
   * core protection #StackItemIdentity exists for. */
  bool any_moved = false;
  StackItemIdentity last_moved = anchor_identity;
  StackMovePlace place_now = place;
  for (const StackItemIdentity &drag_row : rows_to_move) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    const int anchor_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, last_moved);
    if (drag_ordinal < 0 || anchor_ordinal < 0) {
      /* This row no longer resolves (stack changed mid-drag or row was deleted). Skip it rather
       * than canceling the whole operation: partial success is better than abandoning the rows
       * that still exist. */
      continue;
    }
    int new_ordinal = -1;
    if (!outliner_stack_row_move(
            C, *space_outliner, drag_ordinal, anchor_ordinal, place_now, &new_ordinal))
    {
      continue;
    }
    any_moved = true;
    if (new_ordinal >= 0) {
      last_moved = outliner_stack_identity_of(*space_outliner, new_ordinal);
      place_now = StackMovePlace::Below;
    }
    else {
      /* The editor did not report where the row landed; the best the next row can do is the same
       * spot this one was aimed at. */
      last_moved = anchor_identity;
      place_now = place;
    }
  }

  if (!any_moved) {
    return OPERATOR_CANCELLED;
  }
  stack_drop_ui_state_restore(C, ctx, *space_outliner, drop_data->drag_rows, active_before);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_drop(wmOperatorType *ot)
{
  ot->name = "Stack Layer Drop";
  ot->description = "Move a layer to another position in the stack";
  ot->idname = "OUTLINER_OT_stack_layer_drop";

  ot->invoke = stack_layer_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

}  // namespace ed::outliner
}  // namespace blender
