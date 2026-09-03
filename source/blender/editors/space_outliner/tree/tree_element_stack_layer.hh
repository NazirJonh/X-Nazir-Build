/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#pragma once

#include "../outliner_stack_source.hh"

#include "tree_element.hh"

struct ID;

namespace blender::ed::outliner {

/** The data-block the stack belongs to, shown above its rows. */
class TreeElementStackBase final : public AbstractTreeElement {
  const ID &owner_;

 public:
  TreeElementStackBase(TreeElement &legacy_te, const ID &owner);

  std::optional<BIFIconID> get_icon() const override;
};

/** One layer of the stack. Holds no data of its own: rows live in the space runtime. */
class TreeElementStackLayer final : public AbstractTreeElement {
  int icon_;

 public:
  TreeElementStackLayer(TreeElement &legacy_te, const StackRow &row);

  std::optional<BIFIconID> get_icon() const override;
};

/** One of the data-blocks a layer is made of, such as a channel's map. */
class TreeElementStackItem final : public AbstractTreeElement {
  int icon_;

 public:
  TreeElementStackItem(TreeElement &legacy_te, const StackSubRow &sub_row);

  std::optional<BIFIconID> get_icon() const override;
};

}  // namespace blender::ed::outliner
