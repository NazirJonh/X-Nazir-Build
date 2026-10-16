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

#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"

#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

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
 * Whether the content correction \a effect lays anything in \a channel, in a channel the material's
 * set keeps.
 *
 * A live record with a map lays that map, whatever the correction's source. Without one a Paint
 * correction (the Image source) lays nothing however its value reads, and a Fill correction (the
 * Constant source) lays its flat colour -- except in Normal, where a flat value is not a tangent
 * normal and the Normal Combine would read it as a tilt. The one rule for the generator and the CPU
 * adapter.
 */
inline bool paint_layer_effect_lays(const Material &ma,
                                    const MaterialPaintLayer &effect,
                                    const int channel)
{
  if (paint_layer_channel_filtered(ma, channel)) {
    return false;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(effect, channel);
  if (!paint_layer_channel_live(record)) {
    return false;
  }
  if (record->image != nullptr) {
    return true;
  }
  return effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT &&
         channel != PAINT_MATERIAL_CHANNEL_NORMAL;
}

/**
 * Whether the correction \a effect is a filter: it changes the colour the row has so far instead of
 * laying a map or a flat colour over it. A filter lays no coverage, so it is deliberately not part
 * of #paint_layer_effect_lays: it never makes a row take part in a channel or keeps an empty row
 * alive.
 */
inline bool paint_layer_effect_is_filter(const MaterialPaintLayer &effect)
{
  return effect.source == MA_PAINT_LAYER_SOURCE_FILTER;
}

/**
 * Whether the filter \a effect (a correction or an adjustment) changes the colour in \a channel.
 * The channels a filter works in are the live records of its row, the same switches a layer has:
 * the record carries no map, only the choice. A tangent normal is not a colour a per-channel
 * function may touch, so Normal is left alone whatever the record says. The one rule for the
 * generator, the CPU adapter and the topology hash.
 */
inline bool paint_layer_effect_filters(const Material &ma,
                                       const MaterialPaintLayer &effect,
                                       const int channel)
{
  return paint_layer_effect_is_filter(effect) && !paint_layer_channel_filtered(ma, channel) &&
         channel != PAINT_MATERIAL_CHANNEL_NORMAL &&
         paint_layer_channel_live(paint_layer_channel_find(effect, channel));
}

/**
 * Whether \a row is an adjustment: a filter that is a row of the stack and works on the stack below
 * it, with the row's own opacity, blend and mask, instead of on a row's colour.
 */
inline bool paint_layer_row_is_adjustment(const MaterialPaintLayer &row)
{
  return row.source == MA_PAINT_LAYER_SOURCE_FILTER && row.role == MA_PAINT_LAYER_ROLE_LAYER;
}

/** Whether the adjustment \a row changes the stack in \a channel; the rule of a filter. */
inline bool paint_layer_adjustment_acts(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        const int channel)
{
  return paint_layer_row_is_adjustment(row) && paint_layer_effect_filters(ma, row, channel);
}

/** The name a correction goes by in generated sockets when it has none of its own. */
inline const char *paint_layer_effect_label(const MaterialPaintLayer &effect)
{
  if (effect.name[0] != '\0') {
    return effect.name;
  }
  return effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT ? "Correction Fill" : "Correction Paint";
}

/** Whether any content correction of \a layer lays a map in \a channel. */
inline bool paint_layer_effects_lay(const Material &ma,
                                    const MaterialPaintLayer &layer,
                                    const int channel)
{
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (paint_layer_effect_lays(ma, effect, channel)) {
      return true;
    }
  }
  return false;
}

/**
 * Whether a leaf row takes part in \a channel: through its own record, or only through the
 * corrections laid over it (an unpainted Paint row kept alive for them, #paint_layer_row_empty_base).
 * The generator's participation test and the CPU adapter both call this one predicate.
 */
inline bool paint_layer_row_participates(const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         const int channel)
{
  return paint_layer_channel_contributes(ma, layer, paint_layer_channel_find(layer, channel)) ||
         paint_layer_effects_lay(ma, layer, channel) ||
         paint_layer_adjustment_acts(ma, layer, channel);
}

/**
 * Whether a row is only there for its corrections in \a channel: it carries no map and no flat
 * value with alpha (or its record is switched off), so on its own it would cover nothing. Its
 * coverage then starts at zero and the corrections bring it in, instead of the row filling the
 * channel with its empty (black) constant.
 */
inline bool paint_layer_row_empty_base(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const int channel)
{
  return !paint_layer_channel_contributes(ma, layer, paint_layer_channel_find(layer, channel)) &&
         paint_layer_effects_lay(ma, layer, channel);
}

/**
 * Whether the mask item \a item lays coverage over its row's factor.
 *
 * The base mask always does: its Base Color record is a coverage of its own, a constant until a
 * stroke gives it a map. A mask correction over it lays its live record's map, or its flat grey when
 * it is a Fill; a Paint mask correction with no map yet lays nothing, so a fresh one changes no
 * pixel. The generator, the CPU adapter and the topology hash all ask this one rule.
 */
inline bool paint_layer_mask_item_lays(const MaterialPaintLayer &item)
{
  if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
    return true;
  }
  /* A filter item changes the coverage built so far: it has no map or grey of its own and so no
   * record, and what it changes is the opacity's business. */
  if (item.source == MA_PAINT_LAYER_SOURCE_FILTER) {
    return true;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  return paint_layer_channel_live(record) &&
         (record->image != nullptr || item.source == MA_PAINT_LAYER_SOURCE_CONSTANT);
}

/**
 * The constant coverage a mask item's Base Color \a record carries: the mean of its RGB, clamped.
 * The CPU composite and the generator's value input read the one number, so the two cannot
 * disagree about what a constant mask hides.
 */
inline float paint_layer_mask_constant(const MaterialPaintLayerChannel &record)
{
  const float mean = (record.value[0] + record.value[1] + record.value[2]) / 3.0f;
  return clamp_f(mean, 0.0f, 1.0f);
}

/**
 * Whether the mask item \a item reads its coverage as `1 - C`, its map's grey or its flat grey
 * alike, the base mask included. The generator, the CPU adapter and the topology hash all ask this
 * one rule.
 */
inline bool paint_layer_mask_item_inverted(const MaterialPaintLayer &item)
{
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  return record != nullptr && (record->flag & MA_PAINT_LAYER_CHANNEL_INVERT) != 0;
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
