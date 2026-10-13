/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Helpers shared by the Paint Layers description (`paint_layers.cc`) and the code that reads it.
 * They live here rather than in the public header because they are tiny, pure and only meant for
 * the Paint Layers implementation and its tests.
 */

#pragma once

#include "DNA_material_types.h"

namespace blender {

/**
 * The record of \a layer's channels array that stands for \a channel, or null when it has none.
 *
 * The participation rules built on this are shared by every reader of the description, so a row
 * cannot take part in a channel on one side and not on the other.
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

/** Mutable counterpart of #paint_layer_channel_find. */
inline MaterialPaintLayerChannel *paint_layer_channel_find(MaterialPaintLayer &layer,
                                                           const int channel)
{
  return const_cast<MaterialPaintLayerChannel *>(
      paint_layer_channel_find(const_cast<const MaterialPaintLayer &>(layer), channel));
}

/**
 * Whether \a record is a live channel: not null and #MA_PAINT_LAYER_CHANNEL_ENABLED.
 *
 * #MA_PAINT_LAYER_CHANNEL_DISABLED keeps the record and its map but unlinks the channel, so the row
 * takes part nowhere while the map stays in the file; #MA_PAINT_LAYER_CHANNEL_ABSENT has no map at
 * all. Every reader of the description shares this one predicate, so a row cannot take part in a
 * channel on one side and drop out on the other.
 */
inline bool paint_layer_channel_live(const MaterialPaintLayerChannel *record)
{
  return record != nullptr && record->state == MA_PAINT_LAYER_CHANNEL_ENABLED;
}

}  // namespace blender
