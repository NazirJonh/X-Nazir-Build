/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_layers_edit.hh"

#include <algorithm>

#include "BLI_listbase.h"

#include "DNA_material_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Default names
 * \{ */

/** Highest N among the rows named `"<base> N"` in \a list. */
static int paint_layer_name_number_max(const ListBaseT<MaterialPaintLayer> &list,
                                       const StringRef base)
{
  int highest = 0;
  for (const MaterialPaintLayer &layer : list) {
    const StringRef name = layer.name;
    if (name.startswith(base) && name.size() > base.size() + 1 && name[base.size()] == ' ') {
      const StringRef digits = name.drop_prefix(base.size() + 1);
      int number = 0;
      bool all_digits = true;
      for (const char c : digits) {
        all_digits &= (c >= '0' && c <= '9');
        number = (number < 100000) ? number * 10 + (c - '0') : number;
      }
      if (all_digits) {
        highest = std::max(highest, number);
      }
    }
  }
  return highest;
}

std::string BKE_paint_layers_unique_default_name(const Material &ma, const StringRef base)
{
  const int next = paint_layer_name_number_max(ma.paint_layers, base) + 1;
  return std::string(base) + " " + std::to_string(next);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add
 * \{ */

MaterialPaintLayer *BKE_paint_layers_add_with_policy(Material &ma,
                                                     const PaintLayerAddParams &params)
{
  const char *name_base = (params.kind == PaintLayerAddKind::Fill) ? "Fill" : "Layer";
  const std::string name = (params.name != nullptr) ?
                               std::string(params.name) :
                               BKE_paint_layers_unique_default_name(ma, name_base);
  MaterialPaintLayer *created = BKE_paint_layers_add(
      ma, name.c_str(), params.anchor, params.place);
  if (created == nullptr) {
    return nullptr;
  }
  /* A Fill is a Constant row. Set before the channels, so a fresh Fill's default Base Color starts
   * transparent, the way a Fill expects until #BKE_paint_layers_set_fill_color gives it a colour. */
  if (params.kind == PaintLayerAddKind::Fill) {
    created->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  }
  /* The default channel set is a policy of its own (see the BKE helper); the Add only places
   * the row. */
  BKE_paint_layers_default_channels_apply(ma, *created);
  if (params.kind == PaintLayerAddKind::Fill && params.fill_color != nullptr) {
    BKE_paint_layers_set_fill_color(ma, created, params.fill_color);
  }
  return created;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edits
 * \{ */

bool BKE_paint_layers_edit_reorder(Material &ma,
                                   MaterialPaintLayer &from,
                                   MaterialPaintLayer &to)
{
  if (&from == &to) {
    return false;
  }
  const int index = BLI_findindex(&ma.paint_layers, &to);
  if (index < 0 || BLI_findindex(&ma.paint_layers, &from) < 0) {
    return false;
  }
  return BKE_paint_layers_reorder(ma, &from, index);
}

/** \} */

}  // namespace blender
