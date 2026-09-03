/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_object_types.h"
#include "DNA_outliner_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_layer.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"

#include "tree_element_stack_layer.hh"

#include "../outliner_intern.hh"
#include "../outliner_stack_source.hh"
#include "tree_display.hh"

namespace blender::ed::outliner {

namespace {

StackReadContext stack_read_context_from_source(const TreeSourceData &source_data)
{
  StackReadContext ctx;
  ctx.bmain = source_data.bmain;
  ctx.scene = source_data.scene;
  ctx.view_layer = source_data.view_layer;
  return ctx;
}

}  // namespace

TreeDisplayStackLayersObjects::TreeDisplayStackLayersObjects(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

ListBaseT<TreeElement> TreeDisplayStackLayersObjects::build_tree(const TreeSourceData &source_data)
{
  ListBaseT<TreeElement> tree = {nullptr};
  const StackReadContext ctx = stack_read_context_from_source(source_data);
  const StackSource &source = *stack_source_for_space(space_outliner_);

  BKE_view_layer_synced_ensure(*source_data.bmain, source_data.scene, source_data.view_layer);
  for (Base &base : *BKE_view_layer_object_bases_get(source_data.view_layer)) {
    if (base.object == nullptr || !source.object_has_stack(ctx, *base.object)) {
      continue;
    }
    TreeElement *object_element = add_element(
        &tree, &base.object->id, nullptr, nullptr, TSE_SOME_ID, 0, false);
    if (object_element != nullptr) {
      object_element->directdata = &base;
    }
  }
  return tree;
}

TreeDisplayStackLayersStack::TreeDisplayStackLayersStack(SpaceOutliner &space_outliner)
    : AbstractTreeDisplay(space_outliner)
{
}

ListBaseT<TreeElement> TreeDisplayStackLayersStack::build_tree(const TreeSourceData &source_data)
{
  ListBaseT<TreeElement> tree = {nullptr};
  const StackReadContext ctx = stack_read_context_from_source(source_data);

  ID *owner = outliner_stack_owner_get(ctx, space_outliner_);
  if (owner == nullptr) {
    outliner_stack_rows_invalidate(space_outliner_);
    return tree;
  }
  /* What the user did with the rows the last build listed sits in the tree store, keyed by their
   * old ordinals; read it into the identity-keyed map before the rows are re-read underneath it,
   * so the build below can hand it back however the ordinals moved. */
  outliner_stack_row_ui_state_sync(space_outliner_, *owner);
  outliner_stack_rows_ensure(ctx, space_outliner_, *owner);
  space_outliner_.runtime->stack_rows_rebuilt_since_build = false;

  SpaceOutliner_Runtime &runtime = *space_outliner_.runtime;
  if (runtime.stack_rows.is_empty()) {
    return tree;
  }

  const bool show_sub_rows = (space_outliner_.stack_layers_flag & SO_SL_HIDE_ITEMS) == 0;
  /* Sources describe a stack bottom to top, because that is the order it composites in. It is
   * listed the other way round, because that is the order every layer manager shows and the one
   * the word "top" means to the person reading it. */
  /* Where each group's children go, by the group's ordinal. A group is listed as one row and the
   * layers it holds hang off it, so the tree has to remember the element it made for it. */
  Map<int, TreeElement *> group_elements;
  /* Every row listed here, so the map can drop what the stack no longer holds. */
  Set<UUID> seen;
  for (int64_t index = runtime.stack_rows.size() - 1; index >= 0; index--) {
    StackRow &row = runtime.stack_rows[index];
    TreeElement *parent = nullptr;
    if (row.parent_ordinal >= 0) {
      TreeElement *const *found = group_elements.lookup_ptr(int(row.parent_ordinal));
      if (found == nullptr) {
        /* The enclosing group is not in the tree -- unsupported, or too deeply nested. Listing the
         * child at the top level would claim it belongs to the stack itself, which it does not. */
        continue;
      }
      parent = *found;
    }
    /* Rows are addressed by ordinal rather than by address: the vector is rebuilt whenever the
     * stack changes, and the tree store has to survive that. */
    ListBaseT<TreeElement> *parent_tree = (parent != nullptr) ? &parent->subtree : &tree;
    TreeElement *layer = add_element(
        parent_tree, owner, &row, parent, TSE_STACK_LAYER, row.ordinal, false);
    if (layer == nullptr) {
      continue;
    }
    if (row.can_hold_children) {
      group_elements.add(int(row.ordinal), layer);
      /* A folder whose contents are hidden is a folder the user has to open before they can see
       * what they just made, so a group the tree has not met before starts open. */
      if (!TREESTORE(layer)->used) {
        TREESTORE(layer)->flag &= ~TSE_CLOSED;
      }
    }
    /* The tree store hands this row whatever state the row that used to hold its ordinal left
     * behind; what the user did with *this* row is in the map, keyed by identity, and overrides
     * it. A row the source gives no identity for has nothing to be remembered by and keeps the
     * tree store's state, which is what it has always had. */
    if (!BLI_uuid_is_nil(row.stable_id)) {
      seen.add(row.stable_id);
      if (const StackRowUiState *state = runtime.stack_row_ui_state.lookup_ptr(row.stable_id)) {
        TreeStoreElem *layer_tselem = TREESTORE(layer);
        layer_tselem->flag &= ~(TSE_CLOSED | TSE_SELECTED | TSE_ACTIVE);
        layer_tselem->flag |= (state->closed ? TSE_CLOSED : eTreeStoreElem_Flag{}) |
                              (state->selected ? TSE_SELECTED : eTreeStoreElem_Flag{}) |
                              (state->active ? TSE_ACTIVE : eTreeStoreElem_Flag{});
      }
      else {
        /* A row the state map has never met: added from outside since the last snapshot, or met
         * once and pruned away. The tree store hands it whatever flags the row that used to hold
         * its ordinal left behind, which is a removed row's state, not this row's -- start it the
         * way a first meeting does, open and unselected. */
        TreeStoreElem *layer_tselem = TREESTORE(layer);
        layer_tselem->flag &= ~(TSE_CLOSED | TSE_SELECTED | TSE_ACTIVE);
      }
    }
    if (!row.supported || !show_sub_rows) {
      continue;
    }

    /* Only the active section's sub-rows are shown at once; a row with no sections, or whose
     * active section is gone, simply has nothing below it. */
    Span<StackSubRow> sub_rows_to_show;
    const StringRef active_section_id = outliner_stack_row_active_section_get(space_outliner_,
                                                                              row);
    for (const StackContentSection &section : row.content_sections) {
      if (section.identifier == active_section_id) {
        sub_rows_to_show = section.sub_rows;
        break;
      }
    }

    for (const StackSubRow &sub_row : sub_rows_to_show) {
      /* The role is the sub-row's share of the tree-store key; one out of range collides with
       * the neighboring row's sub-rows. Skip it rather than corrupt that key: in a debug build
       * the assert turns the corruption into a crash on the source that built it. */
      BLI_assert(sub_row.role >= 0 && sub_row.role < STACK_ROW_SUB_ROW_STRIDE);
      BLI_assert(row.ordinal >= 0 && row.ordinal <= STACK_ROW_ORDINAL_MAX);
      if (sub_row.role < 0 || sub_row.role >= STACK_ROW_SUB_ROW_STRIDE ||
          row.ordinal < 0 || row.ordinal > STACK_ROW_ORDINAL_MAX)
      {
        continue;
      }
      const int index = row.ordinal * STACK_ROW_SUB_ROW_STRIDE + sub_row.role;
      /* add_element takes void* for generic data, so the const has to go; the tree never writes
       * through it. */
      add_element(&layer->subtree,
                  owner,
                  const_cast<StackSubRow *>(&sub_row),
                  layer,
                  TSE_STACK_ITEM,
                  short(index),
                  false);
    }
  }
  outliner_stack_row_ui_state_prune(runtime, std::move(seen));
  return tree;
}

}  // namespace blender::ed::outliner
