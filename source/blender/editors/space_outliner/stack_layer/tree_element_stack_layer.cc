/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include "DNA_outliner_types.h"

#include "BLI_string.h"

#include "BKE_icons.hh"

#include "UI_interface_icons.hh"

#include "../outliner_intern.hh"
#include "tree_element_stack_layer.hh"

namespace blender::ed::outliner {

static BIFIconID stack_glyph_icon_get(const uint32_t glyph, const int fallback)
{
  if (glyph != 0) {
    if (const int icon_id = BKE_icon_glyph_ensure(glyph)) {
      return BIFIconID(icon_id);
    }
  }
  return BIFIconID(fallback);
}

TreeElementStackLayer::TreeElementStackLayer(TreeElement &legacy_te, const StackRow &row)
    : AbstractTreeElement(legacy_te), icon_(row.icon), glyph_(row.glyph)
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
  return stack_glyph_icon_get(glyph_, icon_);
}

TreeElementStackItem::TreeElementStackItem(TreeElement &legacy_te, const StackSubRow &sub_row)
    : AbstractTreeElement(legacy_te), icon_(sub_row.icon), glyph_(sub_row.glyph)
{
  BLI_assert(legacy_te_.store_elem->type == TSE_STACK_ITEM);
  legacy_te_.name = BLI_strdup(sub_row.name.c_str());
  legacy_te_.flag |= TE_FREE_NAME;
  /* #TreeElement.directdata carries the sub-row's data-block itself, not the #StackSubRow it
   * came from: readers must not cast it to anything but #ID. */
  legacy_te_.directdata = sub_row.id;
}

std::optional<BIFIconID> TreeElementStackItem::get_icon() const
{
  return stack_glyph_icon_get(glyph_, icon_);
}

}  // namespace blender::ed::outliner
