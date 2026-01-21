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

  IndexMaskMemory memory;
  const IndexMask visible_mask = bke::curves::hide::get_visible_mask(curves, domain, memory);

  if (const auto meta_data = attributes.lookup_meta_data(".selection")) {
    /* Case 1: Bool → Float conversion. */
    if (meta_data->data_type == bke::AttrType::Bool) {
      const VArray<bool> selection = *attributes.lookup<bool>(".selection");

      float *dst = MEM_new_array_uninitialized<float>(selection.size(), __func__);

      int selected_count = 0;
      for (const int i : selection.index_range()) {
        if (selection[i] && visible_mask.contains(i)) {
          dst[i] = 1.0f;
          selected_count++;
        }
        else {
          dst[i] = 0.0f;
        }
      }

      attributes.remove(".selection");
      attributes.add(
          ".selection", meta_data->domain, bke::AttrType::Float, bke::AttributeInitMoveArray(dst));
    }
    /* Case 2: Float → Float. Reset hidden elements. */
    else if (meta_data->data_type == bke::AttrType::Float) {
      bke::GSpanAttributeWriter selection_writer = attributes.lookup_for_write_span(".selection");
      MutableSpan<float> selection = selection_writer.span.typed<float>();

      for (const int i : selection.index_range()) {
        if (selection[i] > 0.0f && !visible_mask.contains(i)) {
          selection[i] = 0.0f;
        }
      }
      selection_writer.finish();
    }
  }
  /* Case 3: Create new float selection. */
  else {
    const int64_t size = attributes.domain_size(domain);

    float *dst = MEM_new_array_uninitialized<float>(size, __func__);
    for (int i = 0; i < size; i++) {
      dst[i] = visible_mask.contains(i) ? 1.0f : 0.0f;
    }

    attributes.add(".selection", domain, bke::AttrType::Float, bke::AttributeInitMoveArray(dst));
  }

  return curves.attributes_for_write().lookup_for_write_span<float>(".selection");
}

}  // namespace blender::ed::sculpt_paint
