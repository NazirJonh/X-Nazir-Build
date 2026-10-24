/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_gradient.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "BKE_colorband.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_gradient.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

namespace {

bool gradient_owned(Material &ma, const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_find(ma, layer.marker) == &layer;
}

float gradient_repeat_apply(const eMaterialPaintLayerGradientRepeat repeat, const float t)
{
  switch (repeat) {
    case MA_PAINT_LAYER_GRADIENT_REPEAT_REPEAT:
      return math::gradient_repeat(t);
    case MA_PAINT_LAYER_GRADIENT_REPEAT_REFLECT:
      return math::gradient_reflect(t);
    case MA_PAINT_LAYER_GRADIENT_REPEAT_NONE:
      break;
  }
  return clamp_f(t, 0.0f, 1.0f);
}

/** The row, its corrections and its mask items; the caller's BKE_paint_layers_foreach already walks
 * folders' children. */
template<typename LayerT, typename Fn> void gradient_row_foreach(LayerT &layer, const Fn &fn)
{
  fn(layer);
  for (LayerT &item : layer.mask_stack) {
    fn(item);
  }
  for (LayerT &effect : layer.effects) {
    fn(effect);
  }
}

}  // namespace

bool BKE_paint_layers_gradient_supported(const MaterialPaintLayer &layer)
{
  return layer.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
         layer.kind_subtype == MA_PAINT_LAYER_GENERATOR_GRADIENT;
}

void BKE_paint_layers_gradient_init(MaterialPaintLayerGradient &gradient)
{
  /* Black at the line start to white at its end: a full, visible ramp rather than the tool
   * gradient's half-black mask default. */
  BKE_colorband_init(&gradient.ramp, true);
  copy_v3_fl3(gradient.start, 0.0f, 0.5f, 0.0f);
  copy_v3_fl3(gradient.end, 1.0f, 0.5f, 0.0f);
  gradient.midpoint = 0.5f;
  gradient.space = MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  gradient.type = MA_PAINT_LAYER_GRADIENT_LINEAR;
  gradient.repeat = MA_PAINT_LAYER_GRADIENT_REPEAT_NONE;
  gradient.gizmo_shown = 0;
}

MaterialPaintLayerGradient *BKE_paint_layers_gradient_ensure(MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_gradient_supported(layer)) {
    return nullptr;
  }
  if (layer.gradient == nullptr) {
    layer.gradient = MEM_new<MaterialPaintLayerGradient>(__func__);
    BKE_paint_layers_gradient_init(*layer.gradient);
  }
  else if (layer.gradient->ramp.tot == 0) {
    /* A file written before the ramp existed, or a zeroed payload: the rest of the parameters keep
     * their values, the missing ramp gets the default stops. */
    BKE_colorband_init(&layer.gradient->ramp, true);
  }
  return layer.gradient;
}

void BKE_paint_layers_gradient_free(MaterialPaintLayer &layer)
{
  MEM_SAFE_DELETE(layer.gradient);
}

void BKE_paint_layers_gradient_copy(MaterialPaintLayer &dst, const MaterialPaintLayer &src)
{
  dst.gradient = (src.gradient != nullptr) ?
                     static_cast<MaterialPaintLayerGradient *>(MEM_dupalloc(src.gradient)) :
                     nullptr;
}

MaterialPaintLayer *BKE_paint_layers_gradient_add(Material &ma,
                                                  const char *name,
                                                  MaterialPaintLayer *anchor,
                                                  const PaintLayerPlace place)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_GENERATOR,
                                                         MA_PAINT_LAYER_GENERATOR_GRADIENT);
  if (kind == nullptr ||
      (kind->allowed_roles & paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER)) == 0)
  {
    return nullptr;
  }
  const std::string default_name = BKE_paint_layers_unique_default_name(ma, "Gradient");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      ma, name != nullptr ? name : default_name.c_str(), anchor, place);
  if (layer == nullptr) {
    return nullptr;
  }
  layer->source = MA_PAINT_LAYER_SOURCE_GENERATOR;
  layer->kind_subtype = MA_PAINT_LAYER_GENERATOR_GRADIENT;
  BKE_paint_layers_gradient_ensure(*layer);
  /* It starts in the first channel of the material's set, which is Base Color unless the user
   * removed it. Another channel is switched on like a Fill's. */
  const uint16_t set = BKE_paint_layers_channel_set_mask_get(ma);
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (channel != PAINT_MATERIAL_CHANNEL_NORMAL && (set & (uint16_t(1) << channel)) != 0) {
      BKE_paint_layers_channel_add(ma, layer, eMaterialPaintChannel(channel));
      break;
    }
  }
  /* The source and the record change what the graph builds: the Add's own tag was too early. */
  BKE_paint_layers_tag_edited(ma);
  return layer;
}

float BKE_paint_layers_gradient_ramp_position(const MaterialPaintLayerGradient &gradient,
                                              const float t)
{
  return math::gradient_remap_midpoint(
      gradient_repeat_apply(eMaterialPaintLayerGradientRepeat(gradient.repeat), t),
      gradient.midpoint);
}

float BKE_paint_layers_gradient_factor(const MaterialPaintLayerGradient &gradient,
                                       const float3 &coordinate)
{
  /* Z of the line points only matters in the 3D spaces; in UV it is zeroed on write and read
   * alike, so one 3D formula serves every space. */
  const float3 delta = float3(gradient.end) - float3(gradient.start);
  const float len_sq = math::dot(delta, delta);
  float t = 0.0f;
  if (len_sq > 1e-12f) {
    const float3 offset = coordinate - float3(gradient.start);
    if (gradient.type == MA_PAINT_LAYER_GRADIENT_RADIAL) {
      t = math::length(offset) * (1.0f / sqrtf(len_sq));
    }
    else {
      t = math::dot(offset, delta) / len_sq;
    }
  }
  return BKE_paint_layers_gradient_ramp_position(gradient, t);
}

void BKE_paint_layers_gradient_color(const MaterialPaintLayerGradient &gradient,
                                     const float t,
                                     float r_rgba[4])
{
  if (gradient.ramp.tot == 0) {
    r_rgba[0] = r_rgba[1] = r_rgba[2] = r_rgba[3] = 1.0f;
    return;
  }
  BKE_colorband_evaluate(&gradient.ramp, t, r_rgba);
}

uint64_t BKE_paint_layers_gradient_ramp_hash(const ColorBand &ramp)
{
  const auto mix = [](const uint64_t hash, const uint64_t value) -> uint64_t {
    return hash * 1000003u ^ value;
  };
  const auto mix_float = [&](const uint64_t hash, const float value) -> uint64_t {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return mix(hash, bits);
  };
  uint64_t hash = mix(0, uint16_t(ramp.tot));
  hash = mix(hash, uint8_t(ramp.ipotype));
  hash = mix(hash, uint8_t(ramp.ipotype_hue));
  hash = mix(hash, uint8_t(ramp.color_mode));
  for (const int i : IndexRange(std::min<int>(ramp.tot, MAXCOLORBAND))) {
    const CBData &stop = ramp.data[i];
    hash = mix_float(hash, stop.r);
    hash = mix_float(hash, stop.g);
    hash = mix_float(hash, stop.b);
    hash = mix_float(hash, stop.a);
    hash = mix_float(hash, stop.pos);
  }
  return hash;
}

bool BKE_paint_layers_gradient_any(const Material &ma)
{
  bool found = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        gradient_row_foreach(layer, [&](const MaterialPaintLayer &row) {
          found = found || BKE_paint_layers_gradient_supported(row);
        });
        return !found;
      });
  return found;
}

MaterialPaintLayer *BKE_paint_layers_gradient_ramp_row_from_data(Material &ma, const void *data)
{
  if (data == nullptr) {
    return nullptr;
  }
  const uintptr_t address = uintptr_t(data);
  MaterialPaintLayer *found = nullptr;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        gradient_row_foreach(layer, [&](MaterialPaintLayer &row) {
          if (row.gradient == nullptr) {
            return;
          }
          const ColorBand &ramp = row.gradient->ramp;
          const uintptr_t begin = uintptr_t(&ramp);
          const uintptr_t end = uintptr_t(&ramp) + sizeof(ramp);
          if (address >= begin && address < end) {
            found = &row;
          }
        });
        return found == nullptr;
      });
  return found;
}

bool BKE_paint_layers_gradient_ramp_edited(Material &ma, const void *data)
{
  MaterialPaintLayer *row = BKE_paint_layers_gradient_ramp_row_from_data(ma, data);
  if (row == nullptr) {
    return false;
  }
  /* The ramp lives in the ColorRamp node's storage; a stop or an interpolation change is a new
   * graph, so regeneration is the safe path (and the one that updates the CPU hash). A baked row or
   * folder that contains one also has to be rendered again. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_type(Material &ma,
                                        MaterialPaintLayer &layer,
                                        const eMaterialPaintLayerGradientType type)
{
  if (!gradient_owned(ma, layer) ||
      !ELEM(type, MA_PAINT_LAYER_GRADIENT_LINEAR, MA_PAINT_LAYER_GRADIENT_RADIAL))
  {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  gradient->type = type;
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_repeat(Material &ma,
                                          MaterialPaintLayer &layer,
                                          const eMaterialPaintLayerGradientRepeat repeat)
{
  if (!gradient_owned(ma, layer) ||
      !ELEM(repeat,
            MA_PAINT_LAYER_GRADIENT_REPEAT_NONE,
            MA_PAINT_LAYER_GRADIENT_REPEAT_REPEAT,
            MA_PAINT_LAYER_GRADIENT_REPEAT_REFLECT))
  {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  gradient->repeat = repeat;
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_midpoint(Material &ma,
                                            MaterialPaintLayer &layer,
                                            const float midpoint)
{
  if (!gradient_owned(ma, layer) || !std::isfinite(midpoint)) {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  gradient->midpoint = clamp_f(midpoint, math::gradient_midpoint_min, math::gradient_midpoint_max);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_line(Material &ma,
                                        MaterialPaintLayer &layer,
                                        const float start[3],
                                        const float end[3])
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  for (const int i : IndexRange(3)) {
    if (!std::isfinite(start[i]) || !std::isfinite(end[i])) {
      return false;
    }
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  copy_v3_v3(gradient->start, start);
  copy_v3_v3(gradient->end, end);
  if (gradient->space == MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
    /* The UV space has no third axis: whatever wrote the line keeps it flat. */
    gradient->start[2] = 0.0f;
    gradient->end[2] = 0.0f;
  }
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_channel_scalars(Material &ma,
                                                   MaterialPaintLayer &layer,
                                                   const eMaterialPaintChannel channel,
                                                   const float start,
                                                   const float end)
{
  if (!gradient_owned(ma, layer) || !std::isfinite(start) || !std::isfinite(end)) {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  if (info.is_color || channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (record == nullptr) {
    return false;
  }
  record->value[0] = clamp_f(start, info.value_min, info.value_max);
  record->value[1] = clamp_f(end, info.value_min, info.value_max);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

}  // namespace blender
