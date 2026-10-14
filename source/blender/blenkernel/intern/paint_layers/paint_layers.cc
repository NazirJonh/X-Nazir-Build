/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers.hh: the description of a layered material -- adding, removing and looking
 * rows up by marker. Nothing here reads or writes a node tree; every mutation only marks the
 * material stale.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"

#include <cstring>

#include "paint_layers_intern.hh"

#include "MEM_guardedalloc.h"

#include "BKE_lib_id.hh"
#include "BKE_material.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "DEG_depsgraph.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

namespace blender {

void BKE_paint_layers_tag_edited(Material &ma)
{
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
}

void BKE_paint_layers_tag_value_edited(Material &ma)
{
  /* A value edit changes no socket set: every row's inputs already exist, so the generated tree
   * keeps its nodes and only its values are re-synced. Never MA_PAINT_LAYERS_REGEN; SYNC_TO_EVAL so
   * the evaluated copy the shader reads is refreshed. If the tree does not match the description's
   * values (it was hand-edited), fall back to a full regeneration rather than leaving a stale one. */
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  if (BKE_paint_layers_values_sync(ma)) {
    BKE_paint_layers_values_tag_trees(ma);
  }
  else {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
}

bool BKE_paint_layers_is_layered(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERED) != 0;
}

/** Whether \a layer is a row of \a ma's stack. */
static bool paint_layer_in_stack(const Material &ma, const MaterialPaintLayer *layer)
{
  return layer != nullptr && BLI_findindex(&ma.paint_layers, layer) != -1;
}

/**
 * The record of \a channel when \a layer is a row of \a ma's stack and carries one, otherwise null.
 * Every channel edit starts with this, so a foreign row and a missing record are refused alike.
 */
static MaterialPaintLayerChannel *paint_layer_channel_in_stack(const Material &ma,
                                                               MaterialPaintLayer *layer,
                                                               const eMaterialPaintChannel channel)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return nullptr;
  }
  return paint_layer_channel_find(*layer, channel);
}

/**
 * Link the unlinked or detached \a layer into \a ma's stack: appended on top for a null \a anchor,
 * otherwise next to \a anchor by \a place. \a anchor must already be validated.
 */
static void paint_layer_link(Material &ma,
                             MaterialPaintLayer *layer,
                             MaterialPaintLayer *anchor,
                             const PaintLayerPlace place)
{
  if (anchor == nullptr) {
    BLI_addtail(&ma.paint_layers, layer);
  }
  else if (place == PaintLayerPlace::Above) {
    BLI_insertlinkafter(&ma.paint_layers, anchor, layer);
  }
  else {
    BLI_insertlinkbefore(&ma.paint_layers, anchor, layer);
  }
}

/** A fresh marker no row of \a ma's stack carries yet. */
static bUUID paint_layer_unique_marker(const Material &ma)
{
  bUUID marker;
  bool in_use;
  do {
    marker = BLI_uuid_generate_random();
    in_use = false;
    for (const MaterialPaintLayer &layer : ma.paint_layers) {
      if (BLI_uuid_equal(layer.marker, marker)) {
        in_use = true;
        break;
      }
    }
  } while (in_use);
  return marker;
}

/**
 * A detached row with the usual defaults and a marker unique within \a ma. It is not linked yet:
 * #BKE_paint_layers_add links it into the stack.
 */
static MaterialPaintLayer *paint_layer_alloc(const Material &ma, const char *name)
{
  MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
  layer->flag = MA_PAINT_LAYER_ENABLED;
  layer->opacity = 1.0f;
  STRNCPY(layer->name, name != nullptr ? name : "");
  layer->marker = paint_layer_unique_marker(ma);
  return layer;
}

MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         const PaintLayerPlace place)
{
  /* The material's channel set is derived while the field is zero; freeze it before the new row
   * is linked, so the per-channel filter reads the stored mask instead of recomputing the union.
   * A no-op once anything has materialized it. */
  BKE_paint_layers_channels_materialize(ma);

  /* Every place validates its anchor: a row may never be linked under a foreign material. */
  if (anchor != nullptr && !paint_layer_in_stack(ma, anchor)) {
    return nullptr;
  }

  MaterialPaintLayer *layer = paint_layer_alloc(ma, name);
  paint_layer_link(ma, layer, anchor, place);

  ma.paint_layers_flag |= MA_PAINT_LAYERED;
  BKE_paint_layers_tag_edited(ma);
  return layer;
}

MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return nullptr;
  }

  MaterialPaintLayer *copy = paint_layer_alloc(ma, layer->name);
  copy->flag = layer->flag;
  copy->opacity = layer->opacity;
  if (layer->channels_num > 0 && layer->channels != nullptr) {
    copy->channels = MEM_new_array<MaterialPaintLayerChannel>(layer->channels_num, __func__);
    copy->channels_num = layer->channels_num;
    /* The record is plain data and DNA types delete their copy assignment, so it is copied as one
     * block, the way #BKE_paint_layers_channel_add grows the array. */
    memcpy(copy->channels, layer->channels, sizeof(*copy->channels) * layer->channels_num);
    for (const int i : IndexRange(layer->channels_num)) {
      /* A map is painted in place, so a copy sharing its source's image would turn every stroke on
       * one into a stroke on the other: the copy gets its own. The fresh image carries the one user
       * that this record is; no separate id_us_plus is taken (as #BKE_id_copy returns it). */
      if (copy->channels[i].image != nullptr) {
        copy->channels[i].image = id_cast<Image *>(
            BKE_id_copy(&bmain, &copy->channels[i].image->id));
      }
    }
  }
  BLI_insertlinkafter(&ma.paint_layers, layer, copy);

  BKE_paint_layers_tag_edited(ma);
  return copy;
}

void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer)
{
  /* Every channel the material's set names gets a record. A set that was never authored derives
   * from the layer records and the build default, so this is also how the default reaches a fresh
   * stack: one source of truth instead of a second hard-coded list here. */
  const uint16_t set = BKE_paint_layers_channel_set_mask_get(ma);
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if ((set & (uint16_t(1) << channel)) == 0) {
      continue;
    }
    const eMaterialPaintChannel paint_channel = eMaterialPaintChannel(channel);
    if (paint_layer_channel_find(layer, paint_channel) != nullptr) {
      continue;
    }
    BKE_paint_layers_channel_add(ma, &layer, paint_channel);
  }
}

namespace {

/** The channels a layered material offers before any channel set is authored. Base Color included. */
constexpr uint16_t PAINT_LAYERS_DEFAULT_CHANNEL_SET =
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_NORMAL) |
    (uint16_t(1) << PAINT_MATERIAL_CHANNEL_AO);

}  // namespace

uint16_t BKE_paint_layers_default_channel_set()
{
  return PAINT_LAYERS_DEFAULT_CHANNEL_SET;
}

uint16_t BKE_paint_layers_channel_set_mask_get(const Material &ma)
{
  uint16_t mask = ma.paint_layers_channels;
  if (mask == 0) {
    /* Not authored: the union of the build default and every record any row already carries, so a
     * stack's own channels are never dropped from the set. */
    mask = PAINT_LAYERS_DEFAULT_CHANNEL_SET;
    for (const MaterialPaintLayer &layer : ma.paint_layers) {
      for (const int i : IndexRange(layer.channels_num)) {
        mask |= uint16_t(uint16_t(1) << int(layer.channels[i].channel));
      }
    }
  }
  /* Base Color is the material's constant and can never leave the set. */
  mask |= uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  return mask;
}

void BKE_paint_layers_channels_materialize(Material &ma)
{
  if (ma.paint_layers_channels != 0) {
    return;
  }
  /* Read the derived view while the field is still zero, so it carries the union of the rows and
   * the build default; the write then freezes it. The getter forces Base Color in, so the stored
   * mask is never zero and every later read takes the cheap branch. */
  ma.paint_layers_channels = BKE_paint_layers_channel_set_mask_get(ma);
}

bool BKE_paint_layers_channel_in_set(const Material &ma, const eMaterialPaintChannel channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  return (BKE_paint_layers_channel_set_mask_get(ma) & (uint16_t(1) << int(channel))) != 0;
}

bool BKE_paint_layers_channel_set_enable(Material &ma,
                                         const eMaterialPaintChannel channel,
                                         const bool enabled)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  /* Base Color is the material's constant, so switching it off would leave a set with no colour. */
  if (!enabled && channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    return false;
  }
  uint16_t mask = BKE_paint_layers_channel_set_mask_get(ma);
  if (enabled) {
    mask |= uint16_t(1) << int(channel);
  }
  else {
    mask &= uint16_t(~(uint16_t(1) << int(channel)));
  }
  BKE_paint_layers_channel_set_mask_set(ma, mask);
  return true;
}

void BKE_paint_layers_channel_set_mask_set(Material &ma, uint16_t mask)
{
  /* Base Color is the material's constant and can never leave the set. */
  mask |= uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  if (ma.paint_layers_channels == mask) {
    return;
  }
  ma.paint_layers_channels = mask;
  /* A material setting, not a row: the layers keep every record and map they had. */
  BKE_paint_layers_tag_edited(ma);
}

/**
 * Let go of every data-block \a layer holds a user on. The row's free routine never does, so
 * without this a removed row's map keeps a user and is written into the file as if it were still in
 * use. The data-blocks themselves stay in `Main`: an undo step restores the row, and a map's
 * unsaved pixels live only in memory, so freeing it here would bring the row back with the painting
 * gone. A map left with no user is an orphan, dropped on save or purge like any other.
 */
static void paint_layer_release_users(MaterialPaintLayer &layer)
{
  for (const int i : IndexRange(layer.channels_num)) {
    if (layer.channels[i].image != nullptr) {
      id_us_min(&layer.channels[i].image->id);
    }
  }
}

bool BKE_paint_layers_remove(Material &ma, MaterialPaintLayer *layer)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }

  if (!BLI_uuid_is_nil(ma.active_layer_marker) &&
      BLI_uuid_equal(layer->marker, ma.active_layer_marker))
  {
    ma.active_layer_marker = {};
  }

  paint_layer_release_users(*layer);
  BLI_remlink(&ma.paint_layers, layer);
  BKE_material_paint_layer_free(layer);

  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayer *BKE_paint_layers_find(Material &ma, const bUUID &marker)
{
  for (MaterialPaintLayer &layer : ma.paint_layers) {
    if (BLI_uuid_equal(layer.marker, marker)) {
      return &layer;
    }
  }
  return nullptr;
}

void BKE_paint_layers_active_set(Material &ma, const bUUID &marker)
{
  ma.active_layer_marker = marker;
}

bUUID BKE_paint_layers_active_get(const Material &ma)
{
  return ma.active_layer_marker;
}

MaterialPaintLayer *BKE_paint_layers_active_layer_get(Material &ma)
{
  const bUUID marker = BKE_paint_layers_active_get(ma);
  if (BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  return BKE_paint_layers_find(ma, marker);
}

bool BKE_paint_layers_move(Material &ma,
                           MaterialPaintLayer *layer,
                           MaterialPaintLayer *anchor,
                           const PaintLayerPlace place)
{
  if (!paint_layer_in_stack(ma, layer) || anchor == layer) {
    return false;
  }
  if (anchor != nullptr && !paint_layer_in_stack(ma, anchor)) {
    return false;
  }

  BLI_remlink(&ma.paint_layers, layer);
  paint_layer_link(ma, layer, anchor, place);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_reorder(Material &ma, MaterialPaintLayer *layer, int index)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  const int count = BLI_listbase_count(&ma.paint_layers);
  index = clamp_i(index, 0, count - 1);

  BLI_remlink(&ma.paint_layers, layer);
  /* The stack is bottom-to-top, so index 0 is the first link of the list. */
  if (index == count - 1) {
    BLI_addtail(&ma.paint_layers, layer);
  }
  else if (index == 0) {
    BLI_addhead(&ma.paint_layers, layer);
  }
  else {
    BLI_insertlinkbefore(&ma.paint_layers, BLI_findlink(&ma.paint_layers, index), layer);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_set_value(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const eMaterialPaintChannel channel,
                                        const float value[4])
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  copy_v4_v4(record->value, value);
  /* A Fill row lays its Base-Color (and every other map-less channel's) constant through a group
   * input, so changing a value there is value-only. A Paint row's value alpha decides whether the
   * channel takes part at all, which is topology: it stays a regeneration. */
  if (layer != nullptr && layer->source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    BKE_paint_layers_tag_value_edited(ma);
  }
  else {
    BKE_paint_layers_tag_edited(ma);
  }
  return true;
}

bool BKE_paint_layers_channel_set_image(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const eMaterialPaintChannel channel,
                                        Image *image)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  if (record->image != nullptr) {
    id_us_min(&record->image->id);
  }
  record->image = image;
  if (record->image != nullptr) {
    id_us_plus(&record->image->id);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayerChannel *BKE_paint_layers_channel_add(Material &ma,
                                                        MaterialPaintLayer *layer,
                                                        const eMaterialPaintChannel channel)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return nullptr;
  }
  if (MaterialPaintLayerChannel *existing = paint_layer_channel_find(*layer, channel)) {
    return existing;
  }
  MaterialPaintLayerChannel *channels = MEM_new_array<MaterialPaintLayerChannel>(
      layer->channels_num + 1, __func__);
  if (layer->channels_num > 0) {
    memcpy(channels, layer->channels, sizeof(*channels) * layer->channels_num);
  }
  /* A fresh record is transparent: it takes part but lays nothing down until painted. */
  MaterialPaintLayerChannel &record = channels[layer->channels_num];
  record.channel = channel;
  record.state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  record.image = nullptr;
  zero_v4(record.value);

  MEM_delete(layer->channels);
  layer->channels = channels;
  layer->channels_num++;
  /* A new record is an explicit participation. Once the set is materialized a channel a record
   * names must enter it, otherwise the stored mask would disagree with the derived union a zero
   * field computes. A zero field stays derived, so this is skipped until something materializes. */
  if (ma.paint_layers_channels != 0 && channel >= 0 && channel < PAINT_MATERIAL_CHANNEL_NUM) {
    ma.paint_layers_channels |= uint16_t(uint16_t(1) << int(channel));
  }
  BKE_paint_layers_tag_edited(ma);
  return &layer->channels[layer->channels_num - 1];
}

bool BKE_paint_layers_channel_remove(Material &ma,
                                     MaterialPaintLayer *layer,
                                     const eMaterialPaintChannel channel)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  /* Same as removing a row: the record was one of the users keeping its map alive. */
  if (record->image != nullptr) {
    id_us_min(&record->image->id);
  }
  const int index = int(record - layer->channels);
  if (layer->channels_num > 1) {
    memmove(layer->channels + index,
            layer->channels + index + 1,
            sizeof(*layer->channels) * (layer->channels_num - index - 1));
    layer->channels_num--;
  }
  else {
    MEM_delete(layer->channels);
    layer->channels = nullptr;
    layer->channels_num = 0;
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_channel_set_enabled(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const eMaterialPaintChannel channel,
                                          const bool enabled)
{
  MaterialPaintLayerChannel *record = paint_layer_channel_in_stack(ma, layer, channel);
  if (record == nullptr) {
    return false;
  }
  /* A disabled record keeps its value and map, so re-enabling restores the channel untouched. */
  record->state = enabled ? MA_PAINT_LAYER_CHANNEL_ENABLED : MA_PAINT_LAYER_CHANNEL_DISABLED;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_set_opacity(Material &ma, MaterialPaintLayer *layer, const float opacity)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  layer->opacity = clamp_f(opacity, 0.0f, 1.0f);
  /* Opacity is an input of the row's group, so this is value-only. */
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, const bool enabled)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  SET_FLAG_FROM_TEST(layer->flag, enabled, MA_PAINT_LAYER_ENABLED);
  /* The enabled flag is folded into the row's opacity input (see BKE_paint_layers_effective_opacity),
   * so switching a row off is a value, not a different shape of graph. */
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_rename(Material &ma, MaterialPaintLayer *layer, const char *name)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  STRNCPY(layer->name, name != nullptr ? name : "");
  BKE_paint_layers_tag_edited(ma);
  return true;
}

int BKE_paint_layers_blend_to_ramp(const eMaterialPaintLayerBlend blend)
{
  switch (blend) {
    case MA_PAINT_LAYER_BLEND_MIX:
      return MA_RAMP_BLEND;
    case MA_PAINT_LAYER_BLEND_MULTIPLY:
      return MA_RAMP_MULT;
    case MA_PAINT_LAYER_BLEND_OVERLAY:
      return MA_RAMP_OVERLAY;
    case MA_PAINT_LAYER_BLEND_ADD:
      return MA_RAMP_ADD;
    case MA_PAINT_LAYER_BLEND_DARKEN:
      return MA_RAMP_DARK;
    case MA_PAINT_LAYER_BLEND_BURN:
      return MA_RAMP_BURN;
    case MA_PAINT_LAYER_BLEND_LIGHTEN:
      return MA_RAMP_LIGHT;
    case MA_PAINT_LAYER_BLEND_SCREEN:
      return MA_RAMP_SCREEN;
    case MA_PAINT_LAYER_BLEND_DODGE:
      return MA_RAMP_DODGE;
    case MA_PAINT_LAYER_BLEND_SUBTRACT:
      return MA_RAMP_SUB;
    case MA_PAINT_LAYER_BLEND_DIVIDE:
      return MA_RAMP_DIV;
    case MA_PAINT_LAYER_BLEND_DIFFERENCE:
      return MA_RAMP_DIFF;
    case MA_PAINT_LAYER_BLEND_EXCLUSION:
      return MA_RAMP_EXCLUSION;
    case MA_PAINT_LAYER_BLEND_SOFT_LIGHT:
      return MA_RAMP_SOFT;
    case MA_PAINT_LAYER_BLEND_LINEAR_LIGHT:
      return MA_RAMP_LINEAR;
    case MA_PAINT_LAYER_BLEND_HUE:
      return MA_RAMP_HUE;
    case MA_PAINT_LAYER_BLEND_SATURATION:
      return MA_RAMP_SAT;
    case MA_PAINT_LAYER_BLEND_COLOR:
      return MA_RAMP_COLOR;
    case MA_PAINT_LAYER_BLEND_VALUE:
      return MA_RAMP_VAL;
    case MA_PAINT_LAYER_BLEND_NORMAL_COMBINE:
      return MA_RAMP_BLEND;
  }
  return MA_RAMP_BLEND;
}

bool BKE_paint_layers_set_blend(Material &ma,
                                MaterialPaintLayer *layer,
                                const eMaterialPaintLayerBlend blend)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  /* The Normal channel forces the combine operation by itself, and no other channel can express
   * it; a stored value would only ever be a switch the user cannot make effective. */
  if (blend == MA_PAINT_LAYER_BLEND_NORMAL_COMBINE) {
    return false;
  }
  layer->blend = blend;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_set_source(Material &ma,
                                 MaterialPaintLayer *layer,
                                 const eMaterialPaintLayerSource source)
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  if (!ELEM(source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
    return false;
  }
  if (layer->source == source) {
    return true;
  }
  MaterialPaintLayerChannel *base = paint_layer_channel_find(*layer,
                                                             PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (source == MA_PAINT_LAYER_SOURCE_CONSTANT) {
    /* A painted map is not a constant, so the maps go; the records stay, because a per-channel
     * setting is not a pixel. Base Color already lives in its record, so it survives the flip; a
     * transparent one becomes an opaque black Fill so the row lays a clean constant. The users are
     * let go first, as a removed row does, so the orphaned maps are not saved as if still used. */
    paint_layer_release_users(*layer);
    for (const int i : IndexRange(layer->channels_num)) {
      layer->channels[i].image = nullptr;
    }
    if (base != nullptr && paint_layer_channel_live(base) && base->value[3] == 0.0f) {
      base->value[0] = base->value[1] = base->value[2] = 0.0f;
      base->value[3] = 1.0f;
    }
  }
  else {
    /* A Paint row lays nothing until it is painted, so the Fill's colour must not stay in the
     * record: an opaque value there would cover everything below. The record goes back to the
     * transparent state a fresh one has, and the first stroke's map starts blank. */
    if (base != nullptr && paint_layer_channel_live(base)) {
      zero_v4(base->value);
    }
  }
  layer->source = source;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

void BKE_paint_layers_base_color_get(const MaterialPaintLayer &layer, float r_color[4])
{
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (record != nullptr) {
    copy_v4_v4(r_color, record->value);
    return;
  }
  zero_v4(r_color);
}

bool BKE_paint_layers_base_color_active(const MaterialPaintLayer &layer)
{
  return paint_layer_channel_live(
      paint_layer_channel_find(layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

bool BKE_paint_layers_set_fill_color(Material &ma, MaterialPaintLayer *layer, const float color[4])
{
  if (!paint_layer_in_stack(ma, layer)) {
    return false;
  }
  /* A Fill's colour lives in its Base-Color record like every other channel; the record is created
   * on demand so a script cannot write into a field nothing reads. */
  if (paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
    if (BKE_paint_layers_channel_add(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr) {
      return false;
    }
  }
  return BKE_paint_layers_channel_set_value(
      ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
}

float BKE_paint_layers_effective_opacity(const MaterialPaintLayer &layer)
{
  if ((layer.flag & MA_PAINT_LAYER_ENABLED) == 0) {
    return 0.0f;
  }
  return layer.opacity;
}

void BKE_paint_layers_channel_bottom_color(const eMaterialPaintChannel channel, float r_color[4])
{
  zero_v4(r_color);
  r_color[3] = 1.0f;
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
    case PAINT_MATERIAL_CHANNEL_HEIGHT:
      /* A neutral mid value, so a partially covered scalar row fades towards the Principled
       * default rather than towards zero. */
      r_color[0] = r_color[1] = r_color[2] = 0.5f;
      break;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      /* Flat tangent-space: straight out of the surface. */
      r_color[0] = 0.5f;
      r_color[1] = 0.5f;
      r_color[2] = 1.0f;
      break;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
    case PAINT_MATERIAL_CHANNEL_AO:
      r_color[0] = r_color[1] = r_color[2] = 1.0f;
      break;
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
    case PAINT_MATERIAL_CHANNEL_METALLIC:
    case PAINT_MATERIAL_CHANNEL_CUSTOM:
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      break;
  }
}

}  // namespace blender
