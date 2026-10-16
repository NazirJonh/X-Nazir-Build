/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "paint_layers_kind.hh"

namespace blender {

static constexpr uint8_t all_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
                                     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                                     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM);

/* New kinds are added here. A kind that needs the row bake is registered with a `TODO:` naming
 * it, so what is deferred shows up in a search of this file. */
static const PaintLayerKindInfo kinds[] = {
    {MA_PAINT_LAYER_SOURCE_IMAGE, 0, "Image", PaintLayerKindCategory::Source, all_roles},
    {MA_PAINT_LAYER_SOURCE_CONSTANT, 0, "Constant", PaintLayerKindCategory::Source, all_roles},
    /* A folder owns children, which corrections and mask items never have. */
    {MA_PAINT_LAYER_SOURCE_STACK,
     0,
     "Stack",
     PaintLayerKindCategory::Container,
     paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER)},
};

const PaintLayerKindInfo *paint_layer_kind_find(const int8_t source, const int16_t subtype)
{
  for (const PaintLayerKindInfo &info : kinds) {
    if (info.source == source && info.subtype == subtype) {
      return &info;
    }
  }
  return nullptr;
}

bool paint_layer_kind_role_allowed(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  return info != nullptr && (info->allowed_roles & paint_layer_role_bit(row.role)) != 0;
}

}  // namespace blender
