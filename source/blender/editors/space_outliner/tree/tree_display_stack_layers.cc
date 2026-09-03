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
  outliner_stack_rows_ensure(ctx, space_outliner_, *owner);

  SpaceOutliner_Runtime &runtime = *space_outliner_.runtime;
  if (runtime.stack_rows.is_empty()) {
    return tree;
  }

  /* The owner sits above the rows so that the tree still says whose stack this is once the header
   * has scrolled a breadcrumb out of sight, and so a collapsed stack is one click away. */
  TreeElement *base = add_element(&tree, owner, owner, nullptr, TSE_STACK_BASE, 0, false);
  if (base == nullptr) {
    return tree;
  }
  /* A stack that is listed collapsed is a stack the user cannot see, so a row the tree has not met
   * before starts open; one it has met keeps whatever the user set. */
  if (!TREESTORE(base)->used) {
    TREESTORE(base)->flag &= ~TSE_CLOSED;
  }

  const bool show_sub_rows = (space_outliner_.stack_layers_flag & SO_SL_HIDE_CHANNELS) == 0;
  /* Sources describe a stack bottom to top, because that is the order it composites in. It is
   * listed the other way round, because that is the order every layer manager shows and the one
   * the word "top" means to the person reading it. */
  /* Where each group's children go, by the group's ordinal. A group is listed as one row and the
   * layers it holds hang off it, so the tree has to remember the element it made for it. */
  Map<int, TreeElement *> group_elements;
  for (int64_t index = runtime.stack_rows.size() - 1; index >= 0; index--) {
    StackRow &row = runtime.stack_rows[index];
    TreeElement *parent = base;
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
    TreeElement *layer = add_element(
        &parent->subtree, owner, &row, parent, TSE_STACK_LAYER, row.ordinal, false);
    if (layer == nullptr) {
      continue;
    }
    if (row.is_group) {
      group_elements.add(int(row.ordinal), layer);
      /* A folder whose contents are hidden is a folder the user has to open before they can see
       * what they just made, so a group the tree has not met before starts open. */
      if (!TREESTORE(layer)->used) {
        TREESTORE(layer)->flag &= ~TSE_CLOSED;
      }
    }
    if (!row.supported || !show_sub_rows) {
      continue;
    }
    for (StackSubRow &sub_row : row.sub_rows) {
      const int index = row.ordinal * STACK_ROW_SUB_ROW_STRIDE + sub_row.role;
      add_element(&layer->subtree, owner, &sub_row, layer, TSE_STACK_ITEM, short(index), false);
    }
  }
  return tree;
}

}  // namespace blender::ed::outliner
