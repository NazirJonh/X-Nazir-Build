/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Helpers shared by `paint_layers.cc` (the description and its edits) and `paint_layers_bake.cc`
 * (the bake side). They live here rather than in one of the two so neither owns the other, and are
 * inline because both are tiny and pure.
 */

#pragma once

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BKE_idprop.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender {

/** The IDProperty key a Custom group's interface socket carries its role under. */
constexpr const char *PAINT_LAYERS_CUSTOM_ROLE_PROP = "pbr_custom_role";

/** The list whose direct members include \a target, or null when it is not in \a list. */
inline ListBase *paint_layer_owner_list(ListBase *list, const MaterialPaintLayer *target)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(list)) {
    if (&layer == target) {
      return list;
    }
    if (ListBase *found = paint_layer_owner_list(&layer.children, target)) {
      return found;
    }
    if (ListBase *found = paint_layer_owner_list(&layer.effects, target)) {
      return found;
    }
    if (ListBase *found = paint_layer_owner_list(&layer.mask_stack, target)) {
      return found;
    }
  }
  return nullptr;
}

/** The `pbr_custom_role` string on \a properties, or null when there is none. */
inline const char *custom_role_get(const IDProperty *properties)
{
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      properties, PAINT_LAYERS_CUSTOM_ROLE_PROP, IDP_STRING);
  return prop != nullptr ? IDP_string_get(prop) : nullptr;
}

/**
 * The record of \a layer's channels array that stands for \a channel, or null when it has none.
 *
 * The participation rules built on this -- present, image -- are shared by the generator, the CPU
 * compositor and the bake, so a row cannot take part in a channel on one side and not the other.
 */
inline const MaterialPaintLayerChannel *paint_layer_channel_find(const MaterialPaintLayer &layer,
                                                                 const int channel)
{
  for (int i = 0; i < layer.channels_num; i++) {
    if (layer.channels[i].channel == channel) {
      return &layer.channels[i];
    }
  }
  return nullptr;
}

/**
 * The map a Material layer shows in \a channel: what its source material was baked into, or null.
 *
 * Those maps are the layer's *content*, like a Paint layer's own maps: its mask, corrections,
 * opacity and blend apply to them live, so editing any of those never re-bakes the source. The
 * maps are used whether or not the source changed since: a re-bake refreshes them in place, and
 * the layer keeps showing the previous result meanwhile rather than dropping out.
 */
inline Image *paint_layer_material_source_map(const MaterialPaintLayer &layer, const int channel)
{
  if (layer.kind != MA_PAINT_LAYER_KIND_MATERIAL || layer.bake == nullptr || channel < 0 ||
      channel >= int(ARRAY_SIZE(layer.bake->images)))
  {
    return nullptr;
  }
  return layer.bake->images[channel];
}

/** Whether \a layer takes part in \a channel at all: a record that is not absent. */
inline bool paint_layer_channel_present(const MaterialPaintLayer &layer, const int channel)
{
  if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL) {
    return paint_layer_material_source_map(layer, channel) != nullptr;
  }
  const MaterialPaintLayerChannel *entry = paint_layer_channel_find(layer, channel);
  return entry != nullptr && entry->state != MA_PAINT_LAYER_CHANNEL_ABSENT;
}

/** The image \a layer shows in \a channel, or null when its source is a constant. */
inline Image *paint_layer_channel_image(const MaterialPaintLayer &layer, const int channel)
{
  if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL) {
    return paint_layer_material_source_map(layer, channel);
  }
  const MaterialPaintLayerChannel *entry = paint_layer_channel_find(layer, channel);
  if (entry == nullptr || entry->state == MA_PAINT_LAYER_CHANNEL_ABSENT) {
    return nullptr;
  }
  return entry->image;
}

/**
 * The map a mask item carries in its channel record, or null when the item is a constant.
 *
 * A mask is scalar and one for every channel, so its map and constant live in a single channel
 * record; Base Color is only a placeholder index. A Mask-mode stroke on the item writes there.
 */
inline Image *paint_layer_mask_correction_image(const MaterialPaintLayer &correction,
                                                const int /*channel*/)
{
  return paint_layer_channel_image(correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/**
 * The flat colour \a layer contributes to \a channel when it has no map: a Fill's `fill_color` for
 * Base Color and its own record value for the other channels, or a Paint's record value (empty for
 * a fresh row). One definition for the generator and the CPU, so a constant cannot mean two things.
 */
void paint_layer_channel_constant(const MaterialPaintLayer &layer, int channel, float r_color[4]);

}  // namespace blender
