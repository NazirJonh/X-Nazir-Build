/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_constants.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_colorband.hh"
#include "BKE_idprop.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"

#include "DNA_material_types.h"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

float3 paint_layer_generator_seed_offset(const int seed)
{
  const float s = float(seed);
  float x = s * 0.6180339887498948482f;
  x -= floorf(x);
  float y = s * 0.7548776662466927361f;
  y -= floorf(y);
  return {x, y, 0.0f};
}

float paint_layer_generator_scale_y_ratio(const float scale_x, const float scale_y, const bool locked)
{
  return (locked || scale_x <= 0.0f) ? 1.0f : scale_y / scale_x;
}

float3 paint_layer_generator_coordinate(float3 coordinate,
                                        const float scale_y,
                                        const float rotation_degrees,
                                        const float offset_x,
                                        const float offset_y,
                                        const int seed)
{
  const float radians = DEG2RADF(rotation_degrees);
  const float cos_r = cosf(radians);
  const float sin_r = sinf(radians);
  /* The turn the generated Vector Rotate node makes, the same one the row mapping's chain uses. */
  coordinate = {cos_r * coordinate.x - sin_r * coordinate.y,
                sin_r * coordinate.x + cos_r * coordinate.y,
                coordinate.z};
  /* The Mapping node of that same chain applies its Scale after the rotation, component by
   * component; the generator's is `(1, scale_y, 1)`, so the depth of a 3D pattern never turns. */
  coordinate *= float3(1.0f, scale_y, 1.0f);
  return coordinate + float3(offset_x, offset_y, 0.0f) + paint_layer_generator_seed_offset(seed);
}

bool paint_layer_generator_placement_is_identity(const float scale_y,
                                                 const float rotation_degrees,
                                                 const float offset_x,
                                                 const float offset_y,
                                                 const int seed)
{
  constexpr float epsilon = 1e-6f;
  return fabsf(scale_y - 1.0f) <= epsilon && fabsf(rotation_degrees) <= epsilon &&
         fabsf(offset_x) <= epsilon && fabsf(offset_y) <= epsilon && seed == 0;
}

namespace {

/** The rows laid over \a layer: its corrections and its mask items. */
template<typename LayerT, typename Fn> void generator_laid_row_foreach(LayerT &layer, const Fn &fn)
{
  for (LayerT &item : layer.mask_stack) {
    fn(item);
  }
  for (LayerT &effect : layer.effects) {
    fn(effect);
  }
}

/**
 * Whether \a row takes part in what is laid over it by evaluating itself per texel: an enabled
 * generator, which owns no map a cache could stand in for.
 */
bool generator_row_is_live(const MaterialPaintLayer &row)
{
  return (row.flag & MA_PAINT_LAYER_ENABLED) != 0 &&
         paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_GENERATES);
}

/** Whether \a row belongs to \a ma's description: the guard every generator edit verb needs. */
bool generator_row_owned(Material &ma, const MaterialPaintLayer &row)
{
  return BKE_paint_layers_find(ma, row.marker) == &row;
}

/** Whether \a data is \a band itself or one of its stops. */
bool ramp_owns_data(const ColorBand &band, const void *data)
{
  if (data == &band) {
    return true;
  }
  for (const int i : IndexRange(band.tot)) {
    if (data == &band.data[i]) {
      return true;
    }
  }
  return false;
}

/**
 * Give \a row the storage its kind reads: the Gradient its payload, whose defaults include every
 * scalar channel's value ramp, and the schema kinds their parameter defaults, whose Ramp slot
 * allocates the row's own band.
 */
void generator_row_defaults_init(MaterialPaintLayer &row)
{
  if (row.kind_subtype == MA_PAINT_LAYER_GENERATOR_GRADIENT) {
    BKE_paint_layers_gradient_ensure(row);
    return;
  }
  paint_layer_kind_params_init(row);
  if (row.channels != nullptr) {
    for (const int i : IndexRange(row.channels_num)) {
      BKE_paint_layers_generator_channel_ramp_ensure(row.channels[i]);
    }
  }
}

/**
 * Drop the parameters of the kind \a row was: the schema of the kind it is now is the only one the
 * panel draws, and a parameter outside it would draw as one that edits nothing. The placement
 * every schema shares keeps the values it was given, and the smart-asset marks beside the
 * parameters are not parameters, so both stay.
 */
static void generator_kind_params_prune(MaterialPaintLayer &row)
{
  IDProperty *group = row.system_properties;
  const PaintLayerKindInfo *kind = paint_layer_kind_of(row);
  if (group == nullptr || group->type != IDP_GROUP || kind == nullptr) {
    return;
  }
  IDProperty *prop = static_cast<IDProperty *>(group->data.group.first);
  while (prop != nullptr) {
    IDProperty *next = prop->next;
    if (prop->type != IDP_GROUP) {
      bool in_schema = false;
      for (const int i : IndexRange(kind->params_num)) {
        if (STREQ(kind->params[i].name, prop->name)) {
          in_schema = true;
          break;
        }
      }
      if (!in_schema) {
        IDP_RemoveFromGroup(group, prop);
        IDP_FreeProperty(prop);
      }
    }
    prop = next;
  }
}

/** Shared by the const and mutable getters; \a Row and \a Ramp carry the constness. */
template<typename Row, typename Ramp> Ramp *generator_channel_ramp_find(Row &row, int channel)
{
  if (!BKE_paint_layers_generator_supported(row) || channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.is_color) {
    /* A Gradient's band is in its payload; the schema kinds keep theirs in the row's ramp. */
    if (row.kind_subtype == MA_PAINT_LAYER_GENERATOR_GRADIENT) {
      return (row.gradient != nullptr) ? &row.gradient->ramp : nullptr;
    }
    return (row.ramp != nullptr) ? &row.ramp->band : nullptr;
  }
  auto *record = paint_layer_channel_find(row, channel);
  return (record != nullptr) ? record->gradient_ramp : nullptr;
}

}  // namespace

bool BKE_paint_layers_generator_supported(const MaterialPaintLayer &layer)
{
  return paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES);
}

int BKE_paint_layers_generator_space_of(const MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_generator_supported(layer)) {
    return MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  }
  return paint_layer_generator_space_of(layer);
}

bool BKE_paint_layers_generator_is_bakeable(const MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_generator_supported(layer)) {
    return true;
  }
  /* A Gradient's line, ramp and space are all in its payload; a row written before the payload
   * existed, or by a failed allocation, lays nothing the plan could evaluate. The other generators
   * need nothing but their row: their parameters are properties and their ramp a payload, both of
   * which the loader brings to defaults. */
  if (layer.kind_subtype == MA_PAINT_LAYER_GENERATOR_GRADIENT) {
    return layer.gradient != nullptr;
  }
  return true;
}

bool BKE_paint_layers_generator_is_shared_bakeable(const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_generator_is_bakeable(layer) &&
         BKE_paint_layers_generator_space_of(layer) == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
}

bool BKE_paint_layers_generator_subtree_is_bakeable(const MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_generator_is_bakeable(layer)) {
    return false;
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (!BKE_paint_layers_generator_subtree_is_bakeable(child)) {
      return false;
    }
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (!BKE_paint_layers_generator_subtree_is_bakeable(effect)) {
      return false;
    }
  }
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if (!BKE_paint_layers_generator_subtree_is_bakeable(item)) {
      return false;
    }
  }
  return true;
}

bool BKE_paint_layers_generator_subtree_is_shared_bakeable(const MaterialPaintLayer &layer)
{
  /* A row that is not a generator asks nothing of the space it reads, and a subtree of such rows
   * is one map for every object of the material: only a generator reading the surface breaks that.
   * A generator the bake cannot evaluate at all is refused here too, so a caller that pairs this
   * with the tile readiness never substitutes a cache it could not have made. */
  if (BKE_paint_layers_generator_supported(layer) &&
      !BKE_paint_layers_generator_is_shared_bakeable(layer))
  {
    return false;
  }
  /* A transform effect in its Decal frame walks the surface of the object it is shaded on: its map
   * is per object, like an Object generator's, and needs the object's surface to be rendered. */
  if (paint_layer_effect_acts_in_decal_frame(layer)) {
    return false;
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (!BKE_paint_layers_generator_subtree_is_shared_bakeable(child)) {
      return false;
    }
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (!BKE_paint_layers_generator_subtree_is_shared_bakeable(effect)) {
      return false;
    }
  }
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if (!BKE_paint_layers_generator_subtree_is_shared_bakeable(item)) {
      return false;
    }
  }
  return true;
}

bool BKE_paint_layers_generator_subtree_holds_laid_generator(const MaterialPaintLayer &layer)
{
  bool holds = false;
  generator_laid_row_foreach(
      layer, [&](const MaterialPaintLayer &row) { holds = holds || generator_row_is_live(row); });
  /* A folder lays its children, and each of them its own corrections and mask items: a generator
   * anywhere below is one the folder's bake would fold in. */
  if (holds || !BKE_paint_layers_is_folder(layer)) {
    return holds;
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (BKE_paint_layers_generator_subtree_holds_laid_generator(child)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_generator_subtree_holds_generated(const MaterialPaintLayer &layer)
{
  bool holds = false;
  generator_laid_row_foreach(layer, [&](const MaterialPaintLayer &row) {
    holds = holds || (BKE_paint_layers_generator_supported(row) &&
                      BKE_paint_layers_generator_space_of(row) ==
                          MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED);
  });
  /* A folder lays its children, and each of them its own corrections and mask items: a Generated
   * pattern anywhere below is one the folder's bake would fold in. */
  if (holds || !BKE_paint_layers_is_folder(layer)) {
    return holds;
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (BKE_paint_layers_generator_subtree_holds_generated(child)) {
      return true;
    }
  }
  return false;
}

MaterialPaintLayer *BKE_paint_layers_generator_add(Material &ma,
                                                   const eMaterialPaintLayerGenerator generator,
                                                   const char *name,
                                                   MaterialPaintLayer *anchor,
                                                   const PaintLayerPlace place)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_GENERATOR,
                                                         generator);
  if (kind == nullptr ||
      (kind->allowed_roles & paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER)) == 0)
  {
    return nullptr;
  }
  const std::string default_name = BKE_paint_layers_unique_default_name(ma, kind->name);
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      ma, name != nullptr ? name : default_name.c_str(), anchor, place);
  if (layer == nullptr) {
    return nullptr;
  }
  layer->source = MA_PAINT_LAYER_SOURCE_GENERATOR;
  layer->kind_subtype = generator;
  generator_row_defaults_init(*layer);
  /* It starts in the first channel of the material's set, which is Base Color unless the user
   * removed it. Another channel is switched on like a Fill's. */
  const uint32_t set = BKE_paint_layers_channel_set_mask_get(ma);
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (channel != PAINT_MATERIAL_CHANNEL_NORMAL && (set & (uint32_t(1) << channel)) != 0) {
      MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
          ma, layer, eMaterialPaintChannel(channel));
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      if (record != nullptr && !info.is_color) {
        /* A scalar channel's value ramp spans the channel's whole range; a fresh record is all
         * zero, which would make the row a flat zero. */
        BKE_paint_layers_generator_set_channel_scalars(
            ma, *layer, eMaterialPaintChannel(channel), info.value_min, info.value_max);
      }
      break;
    }
  }
  /* The source, the kind and the record change what the graph builds: the Add's own tag was too
   * early. The space the pattern reads decides the shape of the cache, so the row's first map is
   * created now and the first bake only fills pixels. */
  paint_layers_bake_precreate_texture(ma, *layer);
  BKE_paint_layers_tag_edited(ma);
  return layer;
}

bool BKE_paint_layers_generator_kind_set(Material &ma,
                                         MaterialPaintLayer &row,
                                         const eMaterialPaintLayerGenerator generator)
{
  if (!generator_row_owned(ma, row) || !BKE_paint_layers_generator_supported(row)) {
    return false;
  }
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_GENERATOR,
                                                         generator);
  if (kind == nullptr || (kind->allowed_roles & paint_layer_role_bit(row.role)) == 0) {
    return false;
  }
  if (row.kind_subtype == generator) {
    /* Idempotent: a second call also fills a cache that was released earlier. */
    paint_layers_bake_precreate_texture(ma, row);
    return true;
  }
  row.kind_subtype = generator;
  /* The channel records are the row's participation in the material's channels and stay as they
   * are; only the storages the new kind reads are made. A Gradient keeps the payload it was given
   * -- nothing reads it while the row is another kind, and switching back restores the line. */
  /* The parameters the new kind's schema does not hold are dropped, so the panel draws the new
   * schema and nothing else. A Gradient reads none of the group, so switching to it keeps the
   * parameters for the switch back, like the payload does. */
  if (generator != MA_PAINT_LAYER_GENERATOR_GRADIENT) {
    generator_kind_params_prune(row);
  }
  generator_row_defaults_init(row);
  /* Two kinds can read the same space, and two can differ: asking the bake for the shape this kind
   * uses is cheap when it already has it, and a map of the wrong shape is replaced by the next bake
   * whatever this call leaves. */
  paint_layers_bake_precreate_texture(ma, row);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

const ColorBand *BKE_paint_layers_generator_channel_ramp_get(const MaterialPaintLayer &row,
                                                             const int channel)
{
  return generator_channel_ramp_find<const MaterialPaintLayer, const ColorBand>(row, channel);
}

ColorBand *BKE_paint_layers_generator_channel_ramp_get(MaterialPaintLayer &row, const int channel)
{
  return generator_channel_ramp_find<MaterialPaintLayer, ColorBand>(row, channel);
}

ColorBand *BKE_paint_layers_generator_channel_ramp_ensure(MaterialPaintLayerChannel &record)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(record.channel));
  if (info.is_color || record.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return nullptr;
  }
  if (record.gradient_ramp == nullptr) {
    record.gradient_ramp = MEM_new<ColorBand>(__func__);
    BKE_colorband_init(record.gradient_ramp, true);
    /* Existing files had two ends and no ramp: seed the ramp with them instead of resetting the
     * channel to black-to-white. */
    BKE_paint_layers_generator_channel_ramp_ends_set(*record.gradient_ramp,
                                                     eMaterialPaintChannel(record.channel),
                                                     record.value[0],
                                                     record.value[1]);
  }
  return record.gradient_ramp;
}

void BKE_paint_layers_generator_channel_ramp_ends_set(ColorBand &ramp,
                                                      const eMaterialPaintChannel channel,
                                                      const float start,
                                                      const float end)
{
  if (ramp.tot <= 0) {
    return;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  const float first = BKE_paint_material_t_from_value(info.value_min, info.value_max, start);
  const float last = BKE_paint_material_t_from_value(info.value_min, info.value_max, end);
  const auto set_grey = [](CBData &stop, const float value) {
    stop.r = stop.g = stop.b = value;
    stop.a = 1.0f;
  };
  set_grey(ramp.data[0], first);
  set_grey(ramp.data[ramp.tot - 1], last);
}

float BKE_paint_layers_generator_channel_ramp_end_get(const ColorBand &ramp,
                                                      const eMaterialPaintChannel channel,
                                                      const bool end)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  const CBData &stop = ramp.data[end ? std::max<int>(ramp.tot - 1, 0) : 0];
  return BKE_paint_material_value_from_t(info.value_min, info.value_max, stop.r);
}

bool BKE_paint_layers_generator_set_channel_scalars(Material &ma,
                                                    MaterialPaintLayer &row,
                                                    const eMaterialPaintChannel channel,
                                                    const float start,
                                                    const float end)
{
  if (!std::isfinite(start) || !std::isfinite(end) ||
      !BKE_paint_layers_generator_supported(row))
  {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  if (info.is_color || channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
  if (record == nullptr) {
    return false;
  }
  record->value[0] = clamp_f(start, info.value_min, info.value_max);
  record->value[1] = clamp_f(end, info.value_min, info.value_max);
  /* Keep the ramp's outer stops equal to the two ends the RNA properties expose. Its inner stops
   * stay as the user left them. */
  if (ColorBand *ramp = BKE_paint_layers_generator_channel_ramp_ensure(*record)) {
    BKE_paint_layers_generator_channel_ramp_ends_set(
        *ramp, channel, record->value[0], record->value[1]);
    BKE_paint_layers_generator_ramp_edited(ma, ramp);
  }
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_generator_ramp_edited(Material &ma, const void *data)
{
  if (data == nullptr) {
    return false;
  }
  bool found = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        const auto test_row = [&](const MaterialPaintLayer &ramp_row) {
          if (ramp_row.ramp != nullptr && ramp_owns_data(ramp_row.ramp->band, data)) {
            found = true;
            return;
          }
          if (ramp_row.gradient != nullptr && ramp_owns_data(ramp_row.gradient->ramp, data)) {
            found = true;
            return;
          }
          for (const int i :
               IndexRange(ramp_row.channels != nullptr ? ramp_row.channels_num : 0))
          {
            const ColorBand *channel_ramp = ramp_row.channels[i].gradient_ramp;
            if (channel_ramp != nullptr && ramp_owns_data(*channel_ramp, data)) {
              found = true;
              return;
            }
          }
        };
        test_row(row);
        generator_laid_row_foreach(row, test_row);
        return !found;
      });
  if (!found) {
    return false;
  }
  /* The ramp reads as uniforms or a band texture in the shader, so a stop or a color edit is data,
   * not a different graph: the kept regeneration copies it into the node storage
   * (#paint_layers_gradient_ramp_sync). Only a change of the shader shape rebuilds. A baked row or
   * folder that contains one still has to be rendered again: the bake reads the ramp's pixels. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  BKE_paint_layers_tag_edited(ma);
  /* After the tag, which clears it: any other edit that follows clears it again, so the kept
   * regeneration only trusts a ramp edit that was the sole reason for it. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_RAMP_ONLY;
  return true;
}

}  // namespace blender
