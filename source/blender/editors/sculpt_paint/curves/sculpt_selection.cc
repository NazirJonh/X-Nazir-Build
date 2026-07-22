/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_curves_hide.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

bke::SpanAttributeWriter<float> float_selection_ensure(Curves &curves_id)
{
  /* TODO: Use a generic attribute conversion utility instead of this function. */
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  const bke::AttrDomain domain = bke::AttrDomain(curves_id.selection_domain);

  if (const auto meta_data = attributes.lookup_meta_data(".selection")) {
    if (meta_data->data_type == bke::AttrType::Bool) {
      const VArray<float> selection = *attributes.lookup<float>(".selection");
      float *dst = MEM_new_array_uninitialized<float>(selection.size(), __func__);
      selection.materialize({dst, selection.size()});

      attributes.remove(".selection");
      attributes.add(
          ".selection", meta_data->domain, bke::AttrType::Float, bke::AttributeInitMoveArray(dst));
    }
  }
  else {
    attributes.add(".selection", domain, bke::AttrType::Float, bke::AttributeInitValue(1.0f));
  }

  bke::SpanAttributeWriter<float> selection =
      curves.attributes_for_write().lookup_for_write_span<float>(".selection");

  /* Brushes only act on the selection, so hidden elements have to be deselected here as well as
   * when they are hidden: they may have been selected before the geometry became editable. */
  IndexMaskMemory memory;
  const IndexMask visible = bke::curves::visible_mask(curves, domain, memory);
  if (visible.size() != selection.span.size()) {
    const IndexMask hidden = IndexMask::from_difference(
        IndexRange(selection.span.size()), visible, memory);
    index_mask::masked_fill(selection.span, 0.0f, hidden);
  }

  return selection;
}

}  // namespace blender::ed::sculpt_paint
