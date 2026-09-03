/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "../outliner_stack_source.hh"

#include "tree_element.hh"

namespace blender::ed::outliner {

/** One layer of the stack. Holds no data of its own: rows live in the space runtime. */
class TreeElementStackLayer final : public AbstractTreeElement {
  int icon_;

 public:
  TreeElementStackLayer(TreeElement &legacy_te, const StackRow &row);

  std::optional<BIFIconID> get_icon() const override;
};

/**
 * One of the data-blocks a layer is made of, such as a channel's map.
 *
 * The element keeps the sub-row's data-block (#StackSubRow::id) in #TreeElement.directdata: the
 * ID itself, never the #StackSubRow -- the sub-row lives in the rebuilt rows and is gone by the
 * time a drag or an icon read reaches this element. Readers must not cast #directdata to anything
 * but #ID; see #outliner_stack_item_drag_id_get for the drag side of this contract.
 */
class TreeElementStackItem final : public AbstractTreeElement {
  int icon_;

 public:
  TreeElementStackItem(TreeElement &legacy_te, const StackSubRow &sub_row);

  std::optional<BIFIconID> get_icon() const override;
};

}  // namespace blender::ed::outliner
