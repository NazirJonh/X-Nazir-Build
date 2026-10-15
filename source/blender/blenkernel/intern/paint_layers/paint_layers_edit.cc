/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_layers_edit.hh"

#include <algorithm>

#include "BLI_listbase.h"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Default names
 * \{ */

/** Highest N among the rows named `"<base> N"` in \a list and every folder under it. */
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
    highest = std::max(highest, paint_layer_name_number_max(layer.children, base));
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
  const char *name_base = (params.kind == PaintLayerAddKind::Fill)   ? "Fill" :
                          (params.kind == PaintLayerAddKind::Folder) ? "Folder" :
                                                                       "Layer";
  const std::string name = (params.name != nullptr) ?
                               std::string(params.name) :
                               BKE_paint_layers_unique_default_name(ma, name_base);
  PaintLayerPlace place = params.place;
  if (params.anchor != nullptr && BKE_paint_layers_is_folder(*params.anchor) &&
      place == PaintLayerPlace::Above)
  {
    place = PaintLayerPlace::Into;
  }
  MaterialPaintLayer *created = BKE_paint_layers_add(ma, name.c_str(), params.anchor, place);
  if (created == nullptr) {
    return nullptr;
  }
  if (params.kind == PaintLayerAddKind::Folder) {
    /* A folder owns no channel records: it takes part through its children. */
    created->source = MA_PAINT_LAYER_SOURCE_STACK;
    return created;
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

bool BKE_paint_layers_edit_move(Material &ma,
                                MaterialPaintLayer &from,
                                MaterialPaintLayer *anchor,
                                const PaintLayerPlace place)
{
  if (place == PaintLayerPlace::Into && anchor != nullptr &&
      !BKE_paint_layers_is_folder(*anchor))
  {
    /* Dropping onto a plain row means "put these together": nothing here turns a row into a folder
     * in place, since its maps would then be ignored. */
    if (anchor == &from) {
      return false;
    }
    Vector<MaterialPaintLayer *> to_group{anchor, &from};
    return BKE_paint_layers_group(ma, to_group) != nullptr;
  }
  return BKE_paint_layers_move(ma, &from, anchor, place);
}

bool BKE_paint_layers_edit_reorder(Material &ma,
                                   MaterialPaintLayer &from,
                                   MaterialPaintLayer &to)
{
  if (&from == &to) {
    return false;
  }
  ListBaseT<MaterialPaintLayer> *owner = BKE_paint_layers_owner_list(ma, from);
  if (owner == nullptr || BKE_paint_layers_owner_list(ma, to) != owner) {
    return false;
  }
  return BKE_paint_layers_reorder(ma, &from, BLI_findindex(owner, &to));
}

MaterialPaintLayer *BKE_paint_layers_edit_merge_down(Material &ma,
                                                     MaterialPaintLayer &upper,
                                                     MaterialPaintLayer &lower)
{
  ListBaseT<MaterialPaintLayer> *owner = BKE_paint_layers_owner_list(ma, upper);
  if (owner == nullptr || BKE_paint_layers_owner_list(ma, lower) != owner || &upper == &lower) {
    return nullptr;
  }
  Vector<MaterialPaintLayer *> to_group{&lower, &upper};
  return BKE_paint_layers_group(ma, to_group);
}

MaterialPaintLayer *BKE_paint_layers_edit_group_range(Material &ma,
                                                      const Span<MaterialPaintLayer *> rows)
{
  return BKE_paint_layers_group(ma, rows);
}

MaterialPaintLayer *BKE_paint_layers_edit_group_add(Material &ma, MaterialPaintLayer *anchor)
{
  /* Beside the anchor even when it is a folder: this is the "new group" verb, and the redirect an
   * Add applies to a folder anchor would nest the new one inside it instead. */
  const std::string name = BKE_paint_layers_unique_default_name(ma, "Folder");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      ma, name.c_str(), anchor, PaintLayerPlace::Above);
  if (folder != nullptr) {
    folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  }
  return folder;
}

/** \} */

}  // namespace blender
