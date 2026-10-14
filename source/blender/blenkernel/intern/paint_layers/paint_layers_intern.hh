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

#include "BKE_paint_layers.hh"

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

/**
 * Whether \a record lays something down: it is live and carries a map, or a flat value with some
 * alpha. A map covers by its own alpha; a flat value only takes part when it has any, so an
 * unpainted row stays out of the generated graph exactly as it stays out of the CPU stack.
 *
 * The one place the rule is written: the generator's participation test and the CPU adapter both
 * call it, so a row cannot take part on one side and drop out on the other.
 */
inline bool paint_layer_channel_contributes(const MaterialPaintLayerChannel *record)
{
  return paint_layer_channel_live(record) && (record->image != nullptr || record->value[3] > 0.0f);
}

/**
 * Whether \a channel is filtered out by \a ma's global paint channel set.
 *
 * A channel absent from #Material::paint_layers_channels takes part nowhere; the record and its map
 * stay on the row, so re-adding the channel brings it back untouched. The participation helpers
 * below and the CPU adapter share this one predicate, so a row cannot take part on one side and
 * drop out on the other.
 */
inline bool paint_layer_channel_filtered(const Material &ma, const int channel)
{
  return !BKE_paint_layers_channel_in_set(ma, eMaterialPaintChannel(channel));
}

/**
 * Whether \a layer takes part in \a channel, from the row rather than the record alone.
 *
 * A Fill lays its constant whatever its channels' alpha, so a live record always takes part; every
 * other row follows #paint_layer_channel_contributes (a map, or a flat value with some alpha). A
 * channel the material's global set leaves out takes part nowhere, whatever the record says. The
 * generator's participation test and the CPU adapter both call this, so a Fill cannot take part on
 * one side and drop out on the other.
 */
inline bool paint_layer_channel_contributes(const Material &ma,
                                            const MaterialPaintLayer &layer,
                                            const MaterialPaintLayerChannel *record)
{
  if (record == nullptr || paint_layer_channel_filtered(ma, record->channel)) {
    return false;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    return paint_layer_channel_live(record);
  }
  return paint_layer_channel_contributes(record);
}

/**
 * The `MA_RAMP_*` blend code a description blend stands for.
 *
 * The one table the generator's Mix nodes and the CPU's `ramp_blend` both read, so a blend cannot
 * mean two different things. #MA_PAINT_LAYER_BLEND_NORMAL_COMBINE is not a Mix mode and has no ramp
 * code; it answers #MA_RAMP_BLEND, and the Normal channel never reaches this with it.
 */
int BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend blend);

}  // namespace blender
