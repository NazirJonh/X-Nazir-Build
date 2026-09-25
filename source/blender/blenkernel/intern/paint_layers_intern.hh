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
#include "BKE_image.hh"
#include "BKE_mesh_maps.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender {

struct Material;

/** The IDProperty key a Custom group's interface socket carries its role under. */
constexpr const char *PAINT_LAYERS_CUSTOM_ROLE_PROP = "pbr_custom_role";

/**
 * Whether a #eMaterialMeshMapType is scalar -- its R is the whole value -- rather than an RGB map.
 *
 * A scalar atlas (AO, Curvature, Edge) contributes its R spread across the channel's RGB; an RGB
 * atlas (Normal, the two IDs) contributes its stored RGB as-is. The one classification the generator
 * and the CPU composite share, so a scalar map cannot render grey on one side and coloured on the
 * other.
 */
inline bool paint_layer_mesh_map_is_scalar(const int8_t type)
{
  return ELEM(type, MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE);
}

/**
 * The atlas a MESH_MAP \a layer reads, when one is assigned, or null.
 *
 * A MESH_MAP row reads the material's atlas slot for its #MaterialPaintLayer::mesh_map_type; every
 * channel of the row reads the same Image. The one definition the generator and the CPU composite
 * both use, so the two cannot disagree about whether the row contributes at all. Null -- no slot or
 * no image -- means the row contributes nothing on either side (spec M2 item 4).
 *
 * The answer is structural: it says what the description (DNA) names, and nothing about runtime
 * state. Whether the image's buffer happens to be loaded must not decide whether the row builds a
 * TEX_IMAGE node, or what the topology hash and the sampler counter see -- an atlas that is merely
 * not loaded yet (a freshly opened file, say) would otherwise stay invisible until something else
 * loads its buffer, and the next regenerate would flicker the tree. The CPU path therefore never
 * consults this about buffers: it acquires and releases the buffer through
 * #composite_image_acquire, like every painted map, and an atlas whose buffer cannot be acquired
 * fails the stack exactly like a painted map without one. When the slot has no atlas the row
 * contributes nothing on both sides, as before.
 */
inline Image *paint_layer_mesh_map_image(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return nullptr;
  }
  const MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, layer.mesh_map_type);
  if (slot == nullptr || slot->image == nullptr) {
    return nullptr;
  }
  return slot->image;
}

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
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer.bake == nullptr || channel < 0 ||
      channel >= int(ARRAY_SIZE(layer.bake->images)))
  {
    return nullptr;
  }
  /* Alpha has no channel slot of its own: #BKE_paint_layers_material_bake_apply and the bake
   * planner (`render_material_bake.cc`) both write and read the source's transparency as the
   * row's #MaterialPaintLayerBake::coverage, not `images[PAINT_MATERIAL_CHANNEL_ALPHA]` -- nothing
   * ever fills that slot. Reading `images[]` here left every Material row seeing Alpha as
   * permanently unmapped, so a row could never settle on Baked. */
  if (channel == PAINT_MATERIAL_CHANNEL_ALPHA) {
    return layer.bake->coverage;
  }
  return layer.bake->images[channel];
}

/** Whether \a layer takes part in \a channel at all: a record that is not absent. */
inline bool paint_layer_channel_present(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const int channel)
{
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* A MESH_MAP row paints the atlas in every channel its records name, whatever the map type. */
    return paint_layer_mesh_map_image(ma, layer) != nullptr &&
           paint_layer_channel_find(layer, channel) != nullptr &&
           paint_layer_channel_find(layer, channel)->state != MA_PAINT_LAYER_CHANNEL_ABSENT;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return paint_layer_material_source_map(layer, channel) != nullptr;
  }
  const MaterialPaintLayerChannel *entry = paint_layer_channel_find(layer, channel);
  return entry != nullptr && entry->state != MA_PAINT_LAYER_CHANNEL_ABSENT;
}

/**
 * The image \a layer shows in \a channel, or null when its source is a constant.
 *
 * A MESH_MAP row's map is the material's shared atlas for its type, the same Image in every
 * channel; it is never the channel record's own `image`, which stays unused. One resolver for the
 * generator and the CPU composite, so the two cannot disagree about whether a row contributes.
 */
inline Image *paint_layer_channel_image(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const int channel)
{
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return paint_layer_channel_present(ma, layer, channel) ?
               paint_layer_mesh_map_image(ma, layer) :
               nullptr;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
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
 * record; Base Color is only a placeholder index. A Mask-mode stroke on the item writes there. A
 * MESH_MAP mask item reads the atlas through the same record, so a stroke can still give it a map.
 */
inline Image *paint_layer_mask_correction_image(const Material &ma,
                                                const MaterialPaintLayer &correction,
                                                const int /*channel*/)
{
  if (correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return paint_layer_mesh_map_image(ma, correction);
  }
  return paint_layer_channel_image(ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/**
 * The flat colour \a layer contributes to \a channel when it has no map: a Fill's `fill_color` for
 * Base Color and its own record value for the other channels, or a Paint's record value (empty for
 * a fresh row). One definition for the generator and the CPU, so a constant cannot mean two things.
 */
void paint_layer_channel_constant(const MaterialPaintLayer &layer, int channel, float r_color[4]);

}  // namespace blender
