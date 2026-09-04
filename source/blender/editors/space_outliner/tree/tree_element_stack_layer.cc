/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_ID.h"
#include "DNA_outliner_types.h"

#include "BLI_string.h"

#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "../outliner_intern.hh"
#include "tree_element_stack_layer.hh"

namespace blender::ed::outliner {

TreeElementStackBase::TreeElementStackBase(TreeElement &legacy_te, const ID &owner)
    : AbstractTreeElement(legacy_te), owner_(owner)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_STACK_BASE);
  legacy_te_.name = owner_.name + 2;
}

std::optional<BIFIconID> TreeElementStackBase::get_icon() const
{
  return BIFIconID(ui::icon_from_idcode(GS(owner_.name)));
}

TreeElementStackLayer::TreeElementStackLayer(TreeElement &legacy_te, const StackRow &row)
    : AbstractTreeElement(legacy_te), icon_(row.icon)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_STACK_LAYER);
  /* The row's own string lives in the space runtime, which is rebuilt whenever the stack changes;
   * the tree element outlives that, so it keeps a copy. Display only -- a rename types into
   * #StackRow::name_buffer instead, which is the source's storage and survives a rebuild. */
  legacy_te_.name = BLI_strdup(row.name.c_str());
  legacy_te_.flag |= TE_FREE_NAME;
}

std::optional<BIFIconID> TreeElementStackLayer::get_icon() const
{
  return BIFIconID(icon_);
}

TreeElementStackItem::TreeElementStackItem(TreeElement &legacy_te, const StackSubRow &sub_row)
    : AbstractTreeElement(legacy_te), icon_(sub_row.icon)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_STACK_ITEM);
  legacy_te_.name = BLI_strdup(sub_row.name.c_str());
  legacy_te_.flag |= TE_FREE_NAME;
  legacy_te_.directdata = sub_row.id;
}

std::optional<BIFIconID> TreeElementStackItem::get_icon() const
{
  return BIFIconID(icon_);
}

}  // namespace blender::ed::outliner
