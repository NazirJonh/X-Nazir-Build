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
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"

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

/** The rows laid over \a layer: its corrections and its mask items. */
template<typename LayerT, typename Fn> void gradient_laid_row_foreach(LayerT &layer, const Fn &fn)
{
  for (LayerT &item : layer.mask_stack) {
    fn(item);
  }
  for (LayerT &effect : layer.effects) {
    fn(effect);
  }
}

/** The row, its corrections and its mask items; the caller's BKE_paint_layers_foreach already walks
 * folders' children. */
template<typename LayerT, typename Fn> void gradient_row_foreach(LayerT &layer, const Fn &fn)
{
  fn(layer);
  gradient_laid_row_foreach(layer, fn);
}

}  // namespace

bool BKE_paint_layers_gradient_supported(const MaterialPaintLayer &layer)
{
  /* The subtype is asked for by name, not through the generator table: this is the Gradient
   * module's own gate. A second generator gets a predicate of its own, and the generic dispatch
   * keeps reading the table. */
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
  /* A new Gradient works in object space: that is where a 3D line is aimed at the surface. UV and
   * Generated remain one click away in the panel. */
  gradient.space = MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT;
  gradient.type = MA_PAINT_LAYER_GRADIENT_LINEAR;
  gradient.repeat = MA_PAINT_LAYER_GRADIENT_REPEAT_NONE;
  /* A new row opens with its handles shown: they are how the line is edited, and the panel's
   * button hides them. Base Color is the channel the handles read until the user opens another. */
  gradient.gizmo_shown = 1;
  gradient.gizmo_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
}

ColorBand *BKE_paint_layers_gradient_channel_ramp_ensure(MaterialPaintLayerChannel &record)
{
  /* The value ramp belongs to the channel record, not to the Gradient: every generator reads the
   * same one, so #BKE_paint_layers_generator_channel_ramp_ensure is its only implementation. */
  return BKE_paint_layers_generator_channel_ramp_ensure(record);
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
  /* A scalar channel reads its own value ramp; one written before it existed gets it here, seeded
   * from the two ends that record kept. */
  if (layer.channels != nullptr) {
    for (const int i : IndexRange(layer.channels_num)) {
      BKE_paint_layers_generator_channel_ramp_ensure(layer.channels[i]);
    }
  }
  return layer.gradient;
}

void BKE_paint_layers_gradient_free(MaterialPaintLayer &layer)
{
  if (layer.channels != nullptr) {
    for (const int i : IndexRange(layer.channels_num)) {
      MEM_SAFE_DELETE(layer.channels[i].gradient_ramp);
    }
  }
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
  return BKE_paint_layers_generator_add(ma, MA_PAINT_LAYER_GENERATOR_GRADIENT, name, anchor, place);
}

float BKE_paint_layers_gradient_ramp_position(const MaterialPaintLayerGradient &gradient,
                                              const float t)
{
  return math::gradient_remap_midpoint(
      gradient_repeat_apply(eMaterialPaintLayerGradientRepeat(gradient.repeat), t),
      BKE_paint_layers_gradient_midpoint_effective(gradient));
}

float BKE_paint_layers_gradient_line_z(const MaterialPaintLayerGradient &gradient, const float z)
{
  return gradient.space == MA_PAINT_LAYER_GRADIENT_SPACE_UV ? 0.0f : z;
}

float BKE_paint_layers_gradient_factor(const MaterialPaintLayerGradient &gradient,
                                       const float3 &coordinate)
{
  /* A UV line lives in 2D: Z of the coordinate and of both ends is ignored, so a UV row reads the
   * same factor however the payload's Z was left by a switch from another space. An unknown space
   * is treated as UV: it is the safe default and never drops the row. */
  const bool is_uv = gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT &&
                     gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED;
  float3 start(gradient.start);
  float3 end(gradient.end);
  float3 point = coordinate;
  if (is_uv) {
    start.z = end.z = point.z = 0.0f;
  }

  const float3 delta = end - start;
  const float len_sq = math::dot(delta, delta);
  float t = 0.0f;
  if (len_sq > 1e-12f) {
    const float3 offset = point - start;
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

uint64_t BKE_paint_layers_gradient_ramp_shape_hash(const ColorBand &ramp)
{
  /* Mirrors gpu_shader_valtorgb's choice of shader function. A one-stop band is a uniform; a
   * two-stop RGB band with linear/constant/ease interpolation uses the optimised uniform functions;
   * everything else samples the band as a texture (nearest for constant interpolation). */
  if (ramp.tot <= 1) {
    return 1;
  }
  if (ramp.tot == 2 && ramp.color_mode == COLBAND_BLEND_RGB &&
      (ramp.ipotype == COLBAND_INTERP_LINEAR || ramp.ipotype == COLBAND_INTERP_CONSTANT ||
       ramp.ipotype == COLBAND_INTERP_EASE))
  {
    return 2 + uint64_t(ramp.ipotype);
  }
  return 10 + (ramp.ipotype == COLBAND_INTERP_CONSTANT ? 1 : 0);
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
            return;
          }
          /* A scalar channel's own value ramp: its stops are stops of the same row. */
          for (const int i : IndexRange(row.channels != nullptr ? row.channels_num : 0)) {
            const ColorBand *channel_ramp = row.channels[i].gradient_ramp;
            if (channel_ramp == nullptr) {
              continue;
            }
            const uintptr_t channel_begin = uintptr_t(channel_ramp);
            const uintptr_t channel_end = channel_begin + sizeof(*channel_ramp);
            if (address >= channel_begin && address < channel_end) {
              found = &row;
              return;
            }
          }
        });
        return found == nullptr;
      });
  return found;
}

bool BKE_paint_layers_gradient_ramp_edited(Material &ma, const void *data)
{
  /* A Gradient's colour band is its payload's, but its scalar channels keep the value ramps every
   * generator keeps, so the shared scan is the one implementation. */
  return BKE_paint_layers_generator_ramp_edited(ma, data);
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

float BKE_paint_layers_gradient_midpoint_effective(const MaterialPaintLayerGradient &gradient)
{
  return gradient.distribution == MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_RAMP ? 0.5f :
                                                                               gradient.midpoint;
}

bool BKE_paint_layers_gradient_set_distribution(
    Material &ma,
    MaterialPaintLayer &layer,
    const eMaterialPaintLayerGradientDistribution distribution)
{
  if (!gradient_owned(ma, layer) ||
      !ELEM(distribution,
            MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_MIDPOINT,
            MA_PAINT_LAYER_GRADIENT_DISTRIBUTION_RAMP))
  {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  if (gradient->distribution != distribution) {
    gradient->distribution = distribution;
    BKE_paint_layers_tag_value_edited(ma);
  }
  return true;
}

namespace {

/** A Gradient row's colour band or value ramp, through the accessors every generator shares. */
template<typename Layer, typename Ramp> Ramp *gradient_channel_ramp_find(Layer &layer, int channel)
{
  return BKE_paint_layers_gradient_supported(layer) ?
             static_cast<Ramp *>(BKE_paint_layers_generator_channel_ramp_get(layer, channel)) :
             nullptr;
}

}  // namespace

const ColorBand *BKE_paint_layers_gradient_channel_ramp_get(const MaterialPaintLayer &layer,
                                                           const int channel)
{
  return gradient_channel_ramp_find<const MaterialPaintLayer, const ColorBand>(layer, channel);
}

ColorBand *BKE_paint_layers_gradient_channel_ramp_get(MaterialPaintLayer &layer, const int channel)
{
  return gradient_channel_ramp_find<MaterialPaintLayer, ColorBand>(layer, channel);
}

void BKE_paint_layers_gradient_channel_ramp_ends_set(ColorBand &ramp,
                                                     const eMaterialPaintChannel channel,
                                                     const float start,
                                                     const float end)
{
  BKE_paint_layers_generator_channel_ramp_ends_set(ramp, channel, start, end);
}

float BKE_paint_layers_gradient_channel_ramp_end_get(const ColorBand &ramp,
                                                     const eMaterialPaintChannel channel,
                                                     const bool end)
{
  return BKE_paint_layers_generator_channel_ramp_end_get(ramp, channel, end);
}

bool BKE_paint_layers_gradient_set_gizmo_channel(Material &ma,
                                                 MaterialPaintLayer &layer,
                                                 const eMaterialPaintChannel channel)
{
  if (!gradient_owned(ma, layer) || channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  if (BKE_paint_layers_gradient_channel_ramp_get(layer, channel) == nullptr) {
    return false;
  }
  /* A viewport aid like #BKE_paint_layers_gradient_set_gizmo_display: no graph rebuild. */
  gradient->gizmo_channel = int8_t(channel);
  return true;
}

bool BKE_paint_layers_gradient_set_gizmo_display(
    Material &ma,
    MaterialPaintLayer &layer,
    const eMaterialPaintLayerGradientGizmoDisplay display)
{
  if (!gradient_owned(ma, layer) ||
      !ELEM(display,
            MA_PAINT_LAYER_GRADIENT_GIZMO_PAINT_ONLY,
            MA_PAINT_LAYER_GRADIENT_GIZMO_ALWAYS))
  {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  /* Like the visibility of the gizmo: a viewport aid the graph and the composite never read. */
  gradient->gizmo_display = display;
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

bool BKE_paint_layers_gradient_set_space(Material &ma,
                                         MaterialPaintLayer &layer,
                                         const eMaterialPaintLayerGradientSpace space)
{
  if (!gradient_owned(ma, layer) ||
      !ELEM(space,
            MA_PAINT_LAYER_GRADIENT_SPACE_UV,
            MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT,
            MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED))
  {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  if (gradient->space == space) {
    /* Idempotent: a second call also fills a cache that was released earlier. */
    paint_layers_bake_precreate_texture(ma, layer);
    return true;
  }
  /* The numbers stay as they are and are read in the new space: the source of the coordinate is
   * graph structure, not a value. */
  gradient->space = space;
  /* The maps belong to the old space: a UV bake is one plain image, an Object or Generated one is
   * tiled. Recreate them in the new shape now; a per-object row gets its first tile and the bake
   * adds the rest, a Never or hidden row gets no record at all. A correction's maps go to its owner
   * row, never to the correction itself. */
  paint_layers_bake_precreate_texture(ma, layer);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

namespace {

/** A gradient-space point as an object-space one. */
bool gradient_point_to_object(const MaterialPaintLayerGradient &gradient,
                              const Object *object,
                              const Mesh &mesh,
                              const float3 &point,
                              float3 &r_object)
{
  switch (eMaterialPaintLayerGradientSpace(gradient.space)) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      r_object = point;
      return true;
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      return BKE_paint_layers_gradient_generated_to_object(object, mesh, point, r_object);
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
    default:
      return BKE_paint_layers_gradient_uv_to_object(mesh, float2(point.x, point.y), r_object);
  }
}

/** An object-space point as one in \a space. */
bool gradient_point_from_object(const eMaterialPaintLayerGradientSpace space,
                                const Object *object,
                                const Mesh &mesh,
                                const float3 &object_point,
                                float3 &r_point)
{
  switch (space) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      r_point = object_point;
      return true;
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      return BKE_paint_layers_gradient_generated_from_object(object, mesh, object_point, r_point);
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
    default: {
      float2 uv;
      if (!BKE_paint_layers_gradient_object_to_uv(mesh, object_point, uv)) {
        return false;
      }
      r_point = float3(uv.x, uv.y, 0.0f);
      return true;
    }
  }
}

}  // namespace

bool BKE_paint_layers_gradient_convert_space(Material &ma,
                                             MaterialPaintLayer &layer,
                                             const Object *object,
                                             const Mesh *mesh,
                                             const eMaterialPaintLayerGradientSpace space,
                                             bool &r_converted)
{
  r_converted = false;
  if (!gradient_owned(ma, layer) ||
      !ELEM(space,
            MA_PAINT_LAYER_GRADIENT_SPACE_UV,
            MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT,
            MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED))
  {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  if (gradient->space == space) {
    r_converted = true;
    return true;
  }
  /* Without an evaluated mesh there is nothing to measure the line against: the numbers stay as
   * they are and only the space changes, which the caller reports. */
  if (object == nullptr || mesh == nullptr) {
    return BKE_paint_layers_gradient_set_space(ma, layer, space);
  }

  float3 start_object;
  float3 end_object;
  if (!gradient_point_to_object(*gradient, object, *mesh, float3(gradient->start), start_object) ||
      !gradient_point_to_object(*gradient, object, *mesh, float3(gradient->end), end_object))
  {
    return BKE_paint_layers_gradient_set_space(ma, layer, space);
  }

  float3 start;
  float3 end;
  if (!gradient_point_from_object(space, object, *mesh, start_object, start) ||
      !gradient_point_from_object(space, object, *mesh, end_object, end))
  {
    return BKE_paint_layers_gradient_set_space(ma, layer, space);
  }

  const float start_array[3] = {start.x, start.y, start.z};
  const float end_array[3] = {end.x, end.y, end.z};
  if (!BKE_paint_layers_gradient_set_line(ma, layer, start_array, end_array)) {
    return BKE_paint_layers_gradient_set_space(ma, layer, space);
  }
  if (!BKE_paint_layers_gradient_set_space(ma, layer, space)) {
    return false;
  }
  r_converted = true;
  return true;
}

bool BKE_paint_layers_gradient_set_gizmo_shown(Material &ma,
                                               MaterialPaintLayer &layer,
                                               const bool shown)
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  /* A viewport aid, not a value the generated graph or the CPU composite reads: deliberately no
   * #BKE_paint_layers_tag_value_edited, because rebuilding the tree for a UI toggle would also
   * invalidate the maps of every row. The RNA update redraws the editors instead. */
  gradient->gizmo_shown = shown ? 1 : 0;
  return true;
}

bool BKE_paint_layers_gradient_set_start(Material &ma,
                                         MaterialPaintLayer &layer,
                                         const float start[3])
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  for (const int i : IndexRange(3)) {
    if (!std::isfinite(start[i])) {
      return false;
    }
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  copy_v3_v3(gradient->start, start);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_end(Material &ma,
                                       MaterialPaintLayer &layer,
                                       const float end[3])
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  for (const int i : IndexRange(3)) {
    if (!std::isfinite(end[i])) {
      return false;
    }
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  copy_v3_v3(gradient->end, end);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

namespace {

bool gradient_ramp_set_stop_position(Material &ma,
                                     ColorBand &ramp,
                                     const int index,
                                     const float position)
{
  if (!std::isfinite(position) || index < 0 || index >= ramp.tot) {
    return false;
  }
  /* The stops are kept sorted. Clamping to both neighbours keeps the order and still lets two
   * stops share a position, as a ColorRamp allows. A position that clamps to the one the stop
   * already holds is accepted as it is: false is for a refused call, not for a no-op. */
  const float lower = (index > 0) ? ramp.data[index - 1].pos : 0.0f;
  const float upper = (index + 1 < ramp.tot) ? ramp.data[index + 1].pos : 1.0f;
  const float clamped = clamp_f(position, lower, upper);
  ramp.cur = index;
  if (ramp.data[index].pos == clamped) {
    return true;
  }
  ramp.data[index].pos = clamped;
  /* The ramp lives in the generated ColorRamp node's storage: this is a topology edit. */
  BKE_paint_layers_gradient_ramp_edited(ma, &ramp);
  return true;
}

}  // namespace

bool BKE_paint_layers_gradient_set_stop_position(Material &ma,
                                                 MaterialPaintLayer &layer,
                                                 const int index,
                                                 const float position)
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  MaterialPaintLayerGradient *gradient = BKE_paint_layers_gradient_ensure(layer);
  if (gradient == nullptr) {
    return false;
  }
  return gradient_ramp_set_stop_position(ma, gradient->ramp, index, position);
}

bool BKE_paint_layers_gradient_set_channel_stop_position(Material &ma,
                                                         MaterialPaintLayer &layer,
                                                         const eMaterialPaintChannel channel,
                                                         const int index,
                                                         const float position)
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  ColorBand *ramp = BKE_paint_layers_gradient_channel_ramp_get(layer, channel);
  if (ramp == nullptr) {
    return false;
  }
  return gradient_ramp_set_stop_position(ma, *ramp, index, position);
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
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_gradient_set_channel_scalars(Material &ma,
                                                   MaterialPaintLayer &layer,
                                                   const eMaterialPaintChannel channel,
                                                   const float start,
                                                   const float end)
{
  if (!gradient_owned(ma, layer)) {
    return false;
  }
  if (BKE_paint_layers_gradient_ensure(layer) == nullptr) {
    return false;
  }
  return BKE_paint_layers_generator_set_channel_scalars(ma, layer, channel, start, end);
}

bool BKE_paint_layers_gradient_sync_channel_ramp(Material &ma,
                                                 MaterialPaintLayer &layer,
                                                 const eMaterialPaintChannel channel)
{
  if (!gradient_owned(ma, layer) || !BKE_paint_layers_generator_supported(layer)) {
    return false;
  }
  MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (record == nullptr) {
    return false;
  }
  ColorBand *ramp = BKE_paint_layers_gradient_channel_ramp_ensure(*record);
  if (ramp == nullptr) {
    return false;
  }
  /* The band the sync copies from is the row's Base Color ramp, whichever kind the row is: the
   * accessors every generator shares answer for the Gradient payload and the schema kinds alike. */
  const ColorBand *base_ptr = BKE_paint_layers_generator_channel_ramp_get(
      layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  if (base_ptr == nullptr || base_ptr->tot <= 0) {
    return false;
  }
  const ColorBand &base = *base_ptr;

  /* Sample the channel's current values at the Base Color positions, so a re-sync keeps a tune the
   * user already made. The old ramp is read before it is replaced. */
  ColorBand synced;
  BKE_colorband_init(&synced, true);
  synced.color_mode = base.color_mode;
  synced.ipotype = base.ipotype;
  synced.ipotype_hue = base.ipotype_hue;
  synced.cur = std::min<int>(ramp->cur, base.tot - 1);
  for (const int i : IndexRange(base.tot)) {
    const float position = base.data[i].pos;
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    BKE_colorband_evaluate(ramp, position, rgba);
    CBData &stop = synced.data[i];
    stop.pos = position;
    stop.r = stop.g = stop.b = rgba[0];
    stop.a = 1.0f;
  }
  synced.tot = base.tot;
  *ramp = synced;
  BKE_paint_layers_gradient_ramp_edited(ma, ramp);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

}  // namespace blender
