/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <algorithm>
#include <climits>
#include <optional>
#include <string>

#include "AS_asset_representation.hh"

#include "MEM_guardedalloc.h"

#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_span.hh"
#include "BLI_uuid.h"

#include "ED_screen.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "../tree/tree_iterator.hh"
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

static StackMovePlace stack_drop_place_from_insert_type(TreeElementInsertType insert_type);

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
  const StackMovePlace place = stack_drop_place_from_insert_type(insert_type);

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

/* -------------------------------------------------------------------- */
/** \name Stack Layer ID Drop Operator
 *
 * A data-block dropped on a stack row or in empty space becomes something of the source's own
 * making: a new layer, a channel's map, a Material layer made from a material. Which types are
 * worth catching, how a drag that does not already carry a local data-block resolves into one, and
 * the wording of the tooltip are the source's business, asked through #StackDropHandler; nothing
 * below this point names a domain type.
 * \{ */

/**
 * The stack row the mouse points at: the layer under the cursor, or the layer a sub-row belongs
 * to, however deep the listing nests them. Null is empty space or the stack's owner breadcrumb,
 * either of which reads as "the stack itself, no row" -- what that is worth is the source's call.
 */
static TreeElement *stack_drop_target_row(bContext *C,
                                          const int xy[2],
                                          TreeElementInsertType *r_insert_type = nullptr)
{
  StackDropZone zone;
  if (!stack_layer_drop_zone_find(C, xy, zone)) {
    return nullptr;
  }
  TreeElement *te = zone.te;
  TreeElementInsertType insert_type = zone.insert_type;
  bool climbed = false;
  for (int step = 0; te != nullptr && TREESTORE(te)->type == TSE_STACK_ITEM &&
                      step < STACK_ROW_PARENT_WALK_MAX;
       step++)
  {
    /* A sub-row stands for its layer: the zone within the sub-row says nothing about where a
     * data-block would go, so it reads as the middle of the layer it belongs to. */
    te = te->parent;
    climbed = true;
  }
  if (climbed) {
    insert_type = TE_INSERT_INTO;
  }
  if (te == nullptr || TREESTORE(te)->type != TSE_STACK_LAYER) {
    return nullptr;
  }
  if (!climbed && insert_type == TE_INSERT_INTO) {
    /* A correction or mask item is one line high and has no inside to land in, so its middle third
     * is a dead zone that would swallow the gesture of dropping between two of them: read it as the
     * half of the row the pointer is in, the way a row move does (see
     * #stack_drop_attached_rules_apply). */
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
    const StackRow *row = (space_outliner != nullptr) ?
                              outliner_stack_row_find(*space_outliner,
                                                      int(TREESTORE(te)->nr)) :
                              nullptr;
    if (row != nullptr && !row->parent_section_id.empty()) {
      insert_type = (zone.offset_fraction > 0.5f) ? TE_INSERT_BEFORE : TE_INSERT_AFTER;
    }
  }
  if (r_insert_type != nullptr) {
    *r_insert_type = insert_type;
  }
  return te;
}

/**
 * Where a data-block dropped at \a insert_type would land, as the seam names places. The list is
 * drawn top of stack first, so "before" on screen is the place above the row in the stack. The
 * middle third of a row is #StackMovePlace::Into -- onto the row rather than beside it, which each
 * source reads its own way (or, with no such reading, collapses to a place beside the row).
 */
static StackMovePlace stack_drop_place_from_insert_type(const TreeElementInsertType insert_type)
{
  switch (insert_type) {
    case TE_INSERT_INTO:
      return StackMovePlace::Into;
    case TE_INSERT_AFTER:
      return StackMovePlace::Below;
    case TE_INSERT_BEFORE:
      break;
  }
  return StackMovePlace::Above;
}

/**
 * Record where the drop under way would land, for the operator that commits it to read back.
 * \param target_row: null when the drop names no row, which leaves the anchor invalid.
 */
static void stack_drop_aim_set(SpaceOutliner &space_outliner,
                               TreeElement *target_row,
                               const StackMovePlace place)
{
  SpaceOutliner_Runtime::StackDropAim aim;
  if (target_row != nullptr) {
    aim.anchor = outliner_stack_identity_of(space_outliner, int(TREESTORE(target_row)->nr));
    aim.place = place;
  }
  space_outliner.runtime->stack_drop_aim = aim;
}

/**
 * The row a dropped data-block would land on and how, or null when the drop names no row.
 * \param r_place: #StackMovePlace::Into for the middle of a row, otherwise the side the insertion
 * line is drawn at.
 */
static TreeElement *stack_drop_aim_find(bContext *C, const int xy[2], StackMovePlace *r_place)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return nullptr;
  }
  TreeElementInsertType insert_type = TE_INSERT_INTO;
  TreeElement *target_row = stack_drop_target_row(C, xy, &insert_type);
  if (target_row == nullptr) {
    return nullptr;
  }
  if (outliner_stack_row_find(*space_outliner, int(TREESTORE(target_row)->nr)) == nullptr) {
    /* A row the tree lists but the model no longer holds: the rows were re-read since the tree was
     * built, and there is nothing to aim at until the next build. */
    return nullptr;
  }
  if (r_place != nullptr) {
    *r_place = stack_drop_place_from_insert_type(insert_type);
  }
  return target_row;
}

/**
 * Mark the row a dropped data-block would land on: a line above or below the row for an insertion,
 * the row's own outline for a drop onto it (\a place empty).
 */
static void stack_drop_indicator_set(SpaceOutliner &space_outliner,
                                     TreeElement *target_row,
                                     const std::optional<StackMovePlace> place)
{
  SpaceOutliner_Runtime::StackDropIndicator &indicator =
      space_outliner.runtime->stack_drop_indicator;
  if (target_row == nullptr) {
    indicator.ordinal = -1;
    return;
  }
  indicator.ordinal = int(TREESTORE(target_row)->nr);
  if (!place.has_value()) {
    indicator.insert_type = TE_INSERT_INTO;
  }
  else {
    indicator.insert_type = (*place == StackMovePlace::Below) ? TE_INSERT_AFTER : TE_INSERT_BEFORE;
  }
}

/**
 * The topmost visible row of the stack, or null when the tree lists none: where a drop that named
 * no row lands, since a new layer goes on top. The list is built top of stack first, so the first
 * row met is the top one; a collapsed group's contents are skipped.
 */
static TreeElement *stack_drop_top_row_get(SpaceOutliner &space_outliner)
{
  TreeElement *top_row = nullptr;
  tree_iterator::all_open(space_outliner, [&](TreeElement *te) {
    if (top_row == nullptr && TREESTORE(te)->type == TSE_STACK_LAYER) {
      top_row = te;
    }
  });
  return top_row;
}

/**
 * The one type in \a types the drag already names -- a plain local data-block, a single asset, or
 * (the first match) an item of a multi-asset drag -- or 0 when none does, which leaves the
 * source's #StackDropHandler::drop_external_poll to say whether the drag might still resolve into
 * something else, such as a file dropped from outside Blender.
 */
static short stack_drag_matched_id_type(const wmDrag &drag, const Span<short> types)
{
  if (drag.type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (asset_drags == nullptr) {
      return 0;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ?
                                           GS(item.asset_data.local_id->name) :
                                           ID_Type(0));
      if (types.contains(short(item_idtype))) {
        return short(item_idtype);
      }
    }
    return 0;
  }
  for (const short id_type : types) {
    if (WM_drag_is_ID_type(&drag, id_type)) {
      return id_type;
    }
  }
  return 0;
}

/**
 * Whether \a drag is the selected-assets list the Asset Browser starts next to the drag of the
 * item under the cursor.
 *
 * Both drags are polled against this drop box, and both write the one indicator of the space. The
 * list can name nothing this source takes -- it only carries the selected entries, so a dragged
 * item outside the selection is one way -- and its refusal must not erase the line the presentation
 * drag just placed. A list drag with no such sibling is a gesture of its own and keeps the ordinary
 * refusal behavior.
 */
static bool stack_drag_is_asset_list_with_presentation_drag(const wmDrag &drag,
                                                           const wmWindowManager &wm)
{
  if (drag.type != WM_DRAG_ASSET_LIST) {
    return false;
  }
  for (const wmDrag &other : wm.runtime->drags) {
    if (&other != &drag && ELEM(other.type, WM_DRAG_ID, WM_DRAG_ASSET)) {
      return true;
    }
  }
  return false;
}

bool stack_id_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  ARegion *region = CTX_wm_region(C);
  const bool changed = outliner_flag_set(
      *space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);
  /* A refusal takes the line away with it: the poll is the pass that promises where the drop
   * lands, and a refused one must not leave the previous row marked. */
  const auto clear_indicator = [&]() {
    const bool drawn = space_outliner->runtime->stack_drop_indicator.ordinal >= 0;
    space_outliner->runtime->stack_drop_indicator.ordinal = -1;
    if ((changed || drawn) && region != nullptr) {
      ED_region_tag_redraw_no_rebuild(region);
    }
  };
  const auto refuse = [&]() {
    /* The list half of an asset gesture can name nothing this source takes, while the item drag
     * under the cursor does. The item drag is polled first, so its line has to survive the list's
     * refusal. */
    if (!stack_drag_is_asset_list_with_presentation_drag(*drag, *CTX_wm_manager(C))) {
      clear_indicator();
    }
    return false;
  };
  if (handler == nullptr) {
    return refuse();
  }
  Vector<short> types;
  handler->drop_id_types(types);
  const short matched_type = stack_drag_matched_id_type(*drag, types);
  if (matched_type == 0 && !handler->drop_external_poll(*drag)) {
    return refuse();
  }
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return refuse();
  }

  /* Show where the data-block is going: aimed at the middle of a row, the row is outlined, for a
   * source that reads that as landing on the row itself rather than beside it; aimed between rows,
   * or past either end, the line says where the new row would go. */
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, event->xy, &place);
  if (place == StackMovePlace::Into && !handler->drop_supports_into(matched_type)) {
    place = StackMovePlace::Above;
  }
  StackDropTarget target;
  target.place = place;
  if (target_row != nullptr) {
    target.anchor = outliner_stack_identity_of(*space_outliner, int(TREESTORE(target_row)->nr));
  }

  /* A plain local data-block can be judged before the drop, the way the tooltip already does: a
   * drop the handler would refuse must not be drawn as one it accepts. The box stays active so the
   * tooltip can say why -- it is the one place a refusal is worth a word -- but no line is drawn
   * and no aim is recorded. An asset or an external drag resolves only at drop time, so its
   * refusals still come from the handler then. */
  if (drag->type == WM_DRAG_ID && matched_type != 0) {
    if (const ID *local = WM_drag_get_local_ID(drag, matched_type)) {
      StackDropPayload payload;
      payload.id_uid = local->session_uid;
      payload.id_type = matched_type;
      const char *hint = nullptr;
      if (!handler->can_accept(ctx, *owner, payload, target, &hint)) {
        /* The aim is kept even though nothing is drawn: the drop re-asks the handler against it
         * and is refused again. An emptied aim would read as "no row" there, which accepts, and
         * the drop would land on top of the stack instead of where it was refused. */
        stack_drop_aim_set(*space_outliner, target_row, place);
        clear_indicator();
        return true;
      }
    }
  }

  stack_drop_aim_set(*space_outliner, target_row, place);
  if (target_row != nullptr) {
    stack_drop_indicator_set(*space_outliner,
                             target_row,
                             (place == StackMovePlace::Into) ? std::nullopt :
                                                               std::optional(place));
  }
  else {
    target_row = stack_drop_top_row_get(*space_outliner);
    stack_drop_indicator_set(*space_outliner, target_row, StackMovePlace::Above);
  }
  if ((changed || target_row != nullptr) && region != nullptr) {
    ED_region_tag_redraw_no_rebuild(region);
  }
  return true;
}

std::string stack_id_drop_tooltip(bContext *C, wmDrag *drag, const int xy[2], wmDropBox * /*drop*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return std::string();
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return std::string();
  }
  Vector<short> types;
  handler->drop_id_types(types);
  const short matched_type = stack_drag_matched_id_type(*drag, types);

  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, xy, &place);
  if (place == StackMovePlace::Into && !handler->drop_supports_into(matched_type)) {
    place = StackMovePlace::Above;
  }
  StackDropTarget target;
  target.place = place;
  if (target_row != nullptr) {
    target.anchor = outliner_stack_identity_of(*space_outliner, int(TREESTORE(target_row)->nr));
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  const ID *owner = outliner_stack_owner_get(ctx, *space_outliner);

  /* A plain local data-block can be judged before the drop, and what the source would refuse is
   * worth reading while dragging, not only after -- an asset or an external drag resolves only at
   * drop time, so its refusals come from the handler then. */
  if (drag->type == WM_DRAG_ID && matched_type != 0) {
    if (const ID *local = WM_drag_get_local_ID(drag, matched_type)) {
      StackDropPayload payload;
      payload.id_uid = local->session_uid;
      payload.id_type = matched_type;
      const char *hint = nullptr;
      if (owner != nullptr && !handler->can_accept(ctx, *owner, payload, target, &hint)) {
        /* A refusal with no wording of its own keeps the generic tooltip out: the drop will not
         * land, and saying where it would is worse than saying nothing. */
        return (hint != nullptr) ? std::string(hint) : std::string();
      }
    }
  }

  std::string row_name;
  if (target_row != nullptr) {
    const StackRow *row = outliner_stack_row_find(*space_outliner, int(TREESTORE(target_row)->nr));
    row_name = (row != nullptr) ? row->name : std::string("layer");
  }

  StackDropPayload payload;
  payload.id_type = matched_type;
  if (drag->type == WM_DRAG_PATH) {
    const char *path = WM_drag_get_single_path(drag);
    if (path != nullptr) {
      payload.filepath = path;
    }
  }

  return handler->drop_tooltip(
      ctx, owner, payload, WM_drag_get_item_name(drag), row_name, target);
}

static wmOperatorStatus stack_id_drop_invoke(bContext *C,
                                             wmOperator * /*op*/,
                                             const wmEvent *event)
{
  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }
  ListBaseT<wmDrag> *drags = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(drags->first);

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* Read first, resolve second: resolving an asset imports it, and an import remaps data-blocks,
   * which is exactly the kind of event the editor answers by dropping what it has cached. */
  const SpaceOutliner_Runtime::StackDropAim aim = space_outliner->runtime->stack_drop_aim;

  /* Resolving fills in what the payload names: a data-block, or a file path for the actions that
   * take one. */
  StackDropPayload payload;
  ID *dropped = handler->drop_resolve(*C, *drag, payload);
  if (dropped == nullptr && payload.filepath.empty()) {
    return OPERATOR_CANCELLED;
  }
  if (dropped != nullptr) {
    payload.id_uid = dropped->session_uid;
    payload.id_type = GS(dropped->name);
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* What the poll aimed at a moment ago on this very event -- see #stack_drop_aim_set. */
  StackDropTarget target;
  target.anchor = aim.anchor;
  target.place = aim.place;

  /* #StackDropHandler::execute refuses what #StackDropHandler::can_accept refuses, so the gate is
   * not asked a second time here. The handler leaves the selection and the active row where the
   * shared row helpers put them. */
  if (!handler->execute(*C,
                        space_outliner->runtime->stack_focus,
                        *owner,
                        payload,
                        target,
                        event,
                        nullptr))
  {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_id_drop(wmOperatorType *ot)
{
  ot->name = "Drop Data-Block on Stack Layer";
  ot->description =
      "Assign a dropped data-block to a layer, or add it as a new layer or layer group";
  ot->idname = "OUTLINER_OT_stack_layer_id_drop";

  ot->invoke = stack_id_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  /* No #OPTYPE_UNDO: a data-block that only hands off to another operator (a popup that picks a
   * channel, say) has nothing of its own to push, and one that mutates directly pushes its own
   * step explicitly -- the automatic push would land before a mutation that is still pending in a
   * popup. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

/** \} */

}  // namespace ed::outliner
}  // namespace blender
