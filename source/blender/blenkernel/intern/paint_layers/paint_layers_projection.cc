/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The projection payload of a Paint Layers row: allocating, reading and changing the frame
 * (space, axis, center, size, rotation, clip) and its controller object. Nothing here reads or
 * writes a node tree; every mutation only marks the material stale.
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BLI_index_range.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint_layers.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Row projection
 * \{ */

float BKE_paint_layers_projection_size_normalize(const float size)
{
  /* A new row and an old file both read zero; a collapsed frame would divide the coordinates to
   * infinity, so it means one, as a zero UV scale does. */
  if (size == 0.0f) {
    return 1.0f;
  }
  if (fabsf(size) < PAINT_LAYER_PROJECTION_SIZE_MIN) {
    return copysignf(PAINT_LAYER_PROJECTION_SIZE_MIN, size);
  }
  return size;
}

void BKE_paint_layers_projection_size_effective(const MaterialPaintLayer &layer, float r_size[3])
{
  for (int i = 0; i < 3; i++) {
    /* The controller's own transform already scales the frame; reading one here keeps the inline
     * size from multiplying that scale a second time. The stored size is left untouched so the
     * option can be turned off again. */
    r_size[i] = (layer.projection != nullptr &&
                 (layer.projection->flag & MA_PAINT_LAYER_PROJECTION_SIZE_FROM_CONTROLLER) != 0) ?
                    1.0f :
                    BKE_paint_layers_projection_size_normalize(
                        (layer.projection != nullptr) ? layer.projection->size[i] : 1.0f);
  }
}

float BKE_paint_layers_projection_angle_normalize(const float angle)
{
  /* The stored limit is kept in `(0, 180]` by the setter and the RNA range. A value outside it
   * can only come from a damaged payload, and a zero limit would fade the whole projection out,
   * so it reads as the default. */
  if (angle <= 0.0f || angle > 180.0f) {
    return PAINT_LAYER_PROJECTION_ANGLE_DEFAULT;
  }
  return angle;
}

/* Indexed by #eMaterialPaintLayerMappingSpace. UV has no frame, so its controller display is the
 * plain default a user Empty starts with. */
static const ProjectionModeInfo projection_mode_table[MA_PAINT_LAYER_MAPPING_SPACE_NUM] = {
    {MA_PAINT_LAYER_MAPPING_SPACE_UV, false, OB_PLAINAXES},
    {MA_PAINT_LAYER_MAPPING_SPACE_PLANAR, true, OB_SINGLE_ARROW},
};

static bool paint_layer_projection_space_valid(const int16_t space)
{
  return space >= MA_PAINT_LAYER_MAPPING_SPACE_UV && space < MA_PAINT_LAYER_MAPPING_SPACE_NUM;
}

const ProjectionModeInfo &BKE_paint_layers_projection_mode_info(const int16_t space)
{
  return projection_mode_table[paint_layer_projection_space_valid(space) ?
                                   space :
                                   MA_PAINT_LAYER_MAPPING_SPACE_UV];
}

/**
 * Whether \a layer's kind holds a map a projection can read. The projection wraps a map's lookup
 * exactly as the UV mapping does, so the support rules are the same, except that a Material row's
 * source graph is not wrapped for projection yet.
 */
static bool paint_layer_projection_payload_supported(const Material &ma,
                                                     const MaterialPaintLayer &layer)
{
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* The source wrapper rewrites every UV reader in a copied source tree; projecting that tree
     * needs the same rewrite once per face, which is a later step. */
    return false;
  }
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_FLAT_VALUE)) {
    /* A painted map is written at the raw UV; a filter owns no map, and a kind that generates its
     * own UVs keeps them as its parameter. */
    return false;
  }
  if (layer.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    return paint_layer_channel_live(record) && record->image != nullptr;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (BKE_paint_layers_fill_reads_map(ma, layer, channel)) {
      return true;
    }
  }
  return false;
}

static bool paint_layer_projection_space_supported(const Material &ma,
                                                   const MaterialPaintLayer &layer,
                                                   const int16_t space)
{
  return paint_layer_projection_space_valid(space) &&
         space != MA_PAINT_LAYER_MAPPING_SPACE_UV &&
         paint_layer_projection_payload_supported(ma, layer);
}

bool BKE_paint_layers_projection_supported(const Material &ma, const MaterialPaintLayer &layer)
{
  return paint_layer_projection_payload_supported(ma, layer);
}

bool BKE_paint_layers_projection_applies(const Material &ma, const MaterialPaintLayer &layer)
{
  return layer.projection != nullptr && paint_layer_projection_space_valid(layer.mapping.space) &&
         layer.mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_UV &&
         paint_layer_projection_payload_supported(ma, layer);
}

MaterialPaintLayerProjection *BKE_paint_layers_projection_ensure(MaterialPaintLayer &layer)
{
  if (layer.projection != nullptr) {
    return layer.projection;
  }
  MaterialPaintLayerProjection *projection = MEM_new<MaterialPaintLayerProjection>(__func__);
  copy_v3_fl(projection->center, 0.0f);
  copy_v3_fl(projection->size, 1.0f);
  copy_v3_fl(projection->rotation, 0.0f);
  projection->axis = MA_PAINT_LAYER_PROJECTION_AXIS_Z;
  projection->angle = PAINT_LAYER_PROJECTION_ANGLE_DEFAULT;
  /* A frame starts as the decal it is meant to be: clipped to the frame, limited in depth, fitted
   * to the source image and faded out past the facing limit. */
  projection->flag = MA_PAINT_LAYER_PROJECTION_CLIP | MA_PAINT_LAYER_PROJECTION_DEPTH |
                     MA_PAINT_LAYER_PROJECTION_FIT_ASPECT | MA_PAINT_LAYER_PROJECTION_FACING;
  layer.projection = projection;
  return projection;
}

/** Give the controller slot one user and take it from the object it leaves. */
static void paint_layer_projection_object_assign(MaterialPaintLayer &layer, Object *object)
{
  if (layer.projection_object == object) {
    return;
  }
  if (layer.projection_object != nullptr) {
    id_us_min(&layer.projection_object->id);
  }
  layer.projection_object = object;
  if (object != nullptr) {
    id_us_plus(&object->id);
  }
}

bool BKE_paint_layers_projection_set_space(Material &ma,
                                           MaterialPaintLayer *layer,
                                           const int16_t space)
{
  if (!paint_layer_owned(ma, layer) || !paint_layer_projection_space_valid(space)) {
    return false;
  }
  if (layer->mapping.space == space) {
    return true;
  }
  if (space != MA_PAINT_LAYER_MAPPING_SPACE_UV) {
    if (!paint_layer_projection_space_supported(ma, *layer, space)) {
      return false;
    }
    BKE_paint_layers_projection_ensure(*layer);
  }
  else {
    /* The frame is on-demand and the controller is only meaningful while a projection exists. */
    MEM_SAFE_DELETE(layer->projection);
    paint_layer_projection_object_assign(*layer, nullptr);
  }
  layer->mapping.space = space;
  /* The mode picks different nodes (or none at all), which is topology. */
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_values(Material &ma,
                                            MaterialPaintLayer *layer,
                                            const float center[3],
                                            const float size[3],
                                            const float rotation[3])
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(center[i]) || !std::isfinite(size[i]) || !std::isfinite(rotation[i])) {
      return false;
    }
  }
  copy_v3_v3(layer->projection->center, center);
  for (int i = 0; i < 3; i++) {
    layer->projection->size[i] = BKE_paint_layers_projection_size_normalize(size[i]);
  }
  copy_v3_v3(layer->projection->rotation, rotation);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_center(Material &ma,
                                            MaterialPaintLayer *layer,
                                            const float center[3])
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(center[i])) {
      return false;
    }
  }
  copy_v3_v3(layer->projection->center, center);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_size(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const float size[3])
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(size[i])) {
      return false;
    }
    layer->projection->size[i] = BKE_paint_layers_projection_size_normalize(size[i]);
  }
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_rotation(Material &ma,
                                              MaterialPaintLayer *layer,
                                              const float rotation[3])
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(rotation[i])) {
      return false;
    }
  }
  copy_v3_v3(layer->projection->rotation, rotation);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

/** Read one flag bit of the row's frame; a row with no payload reports false. */
static bool paint_layer_projection_flag_get(const MaterialPaintLayer &layer,
                                            const int16_t flag_bit)
{
  return layer.projection != nullptr && (layer.projection->flag & flag_bit) != 0;
}

/**
 * Set one flag bit of the row's frame. Every flag is a value input: the factor's nodes are always
 * built and the toggle mixes them in place, so only a real change marks the material stale.
 */
static bool paint_layer_projection_flag_set(Material &ma,
                                            MaterialPaintLayer *layer,
                                            const int16_t flag_bit,
                                            const bool value)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  if (((layer->projection->flag & flag_bit) != 0) == value) {
    return true;
  }
  SET_FLAG_FROM_TEST(layer->projection->flag, value, flag_bit);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_clip_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_CLIP);
}

bool BKE_paint_layers_projection_set_clip(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const bool clip)
{
  return paint_layer_projection_flag_set(ma, layer, MA_PAINT_LAYER_PROJECTION_CLIP, clip);
}

int8_t BKE_paint_layers_projection_axis_get(const MaterialPaintLayer &layer)
{
  if (layer.projection != nullptr && layer.projection->axis >= MA_PAINT_LAYER_PROJECTION_AXIS_X &&
      layer.projection->axis <= MA_PAINT_LAYER_PROJECTION_AXIS_Z)
  {
    return layer.projection->axis;
  }
  return MA_PAINT_LAYER_PROJECTION_AXIS_Z;
}

bool BKE_paint_layers_projection_depth_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_DEPTH);
}

bool BKE_paint_layers_projection_set_depth(Material &ma, MaterialPaintLayer *layer, const bool depth)
{
  return paint_layer_projection_flag_set(
      ma, layer, MA_PAINT_LAYER_PROJECTION_DEPTH, depth);
}

bool BKE_paint_layers_projection_facing_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_FACING);
}

bool BKE_paint_layers_projection_set_facing(Material &ma,
                                            MaterialPaintLayer *layer,
                                            const bool facing)
{
  return paint_layer_projection_flag_set(
      ma, layer, MA_PAINT_LAYER_PROJECTION_FACING, facing);
}

bool BKE_paint_layers_projection_facing_invert_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_FACING_INVERT);
}

bool BKE_paint_layers_projection_set_facing_invert(Material &ma,
                                                   MaterialPaintLayer *layer,
                                                   const bool invert)
{
  return paint_layer_projection_flag_set(
      ma, layer, MA_PAINT_LAYER_PROJECTION_FACING_INVERT, invert);
}

bool BKE_paint_layers_projection_snap_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_SNAP);
}

bool BKE_paint_layers_projection_set_snap(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const bool snap)
{
  return paint_layer_projection_flag_set(ma, layer, MA_PAINT_LAYER_PROJECTION_SNAP, snap);
}

bool BKE_paint_layers_projection_size_from_controller_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_SIZE_FROM_CONTROLLER);
}

bool BKE_paint_layers_projection_set_size_from_controller(Material &ma,
                                                          MaterialPaintLayer *layer,
                                                          const bool from_controller)
{
  return paint_layer_projection_flag_set(
      ma, layer, MA_PAINT_LAYER_PROJECTION_SIZE_FROM_CONTROLLER, from_controller);
}

void BKE_paint_layers_projection_facing_weights(const float angle,
                                                const bool invert,
                                                float &r_scale,
                                                float &r_offset)
{
  const float limit = BKE_paint_layers_projection_angle_normalize(angle);
  /* `DEG2RADF` is function-like: it has to wrap the angle, not multiply the angle by it. */
  const float cos_min = cosf(DEG2RADF(limit));
  const float cos_max = cosf(DEG2RADF(max_ff(limit - PAINT_LAYER_PROJECTION_ANGLE_BAND, 0.0f)));
  const float scale = 1.0f / max_ff(cos_max - cos_min, 1e-6f);
  /* The weight peaks on the faces whose normal points back at the projector -- Ucupaint's decal,
   * whose arrow enters the surface -- so the sign is baked here rather than into the component:
   * both normal paths read this one scale, and the flip therefore reaches both. The offset keeps
   * the unflipped magnitude, so the limit still lands where the angle says. */
  r_scale = invert ? scale : -scale;
  r_offset = -cos_min * scale;
}

bool BKE_paint_layers_projection_fit_aspect_get(const MaterialPaintLayer &layer)
{
  return paint_layer_projection_flag_get(layer, MA_PAINT_LAYER_PROJECTION_FIT_ASPECT);
}

bool BKE_paint_layers_projection_set_fit_aspect(Material &ma,
                                                MaterialPaintLayer *layer,
                                                const bool fit_aspect)
{
  return paint_layer_projection_flag_set(
      ma, layer, MA_PAINT_LAYER_PROJECTION_FIT_ASPECT, fit_aspect);
}

float BKE_paint_layers_projection_angle_get(const MaterialPaintLayer &layer)
{
  if (layer.projection != nullptr) {
    return BKE_paint_layers_projection_angle_normalize(layer.projection->angle);
  }
  return PAINT_LAYER_PROJECTION_ANGLE_DEFAULT;
}

bool BKE_paint_layers_projection_set_angle(Material &ma, MaterialPaintLayer *layer, float angle)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  if (!std::isfinite(angle)) {
    return false;
  }
  /* A zero limit would fade the whole projection out, so the smallest stored limit is one degree. */
  angle = clamp_f(angle, 1.0f, 180.0f);
  const float current = BKE_paint_layers_projection_angle_get(*layer);
  if (current == angle) {
    return true;
  }
  layer->projection->angle = angle;
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_axis(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const int8_t axis)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr ||
      axis < MA_PAINT_LAYER_PROJECTION_AXIS_X || axis > MA_PAINT_LAYER_PROJECTION_AXIS_Z)
  {
    return false;
  }
  if (layer->projection->axis == axis) {
    return true;
  }
  layer->projection->axis = axis;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_object(Material &ma,
                                            MaterialPaintLayer *layer,
                                            Object *object)
{
  if (!paint_layer_owned(ma, layer)) {
    return false;
  }
  if (object != nullptr && layer->mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_UV) {
    /* A controller only has a space to control while the row projects. */
    return false;
  }
  if (layer->projection_object == object) {
    return true;
  }
  paint_layer_projection_object_assign(*layer, object);
  /* The generated Texture Coordinate node names the object, which is topology. */
  BKE_paint_layers_tag_edited(ma);
  return true;
}

const Image *BKE_paint_layers_projection_aspect_image(const Material &ma,
                                                      const MaterialPaintLayer &layer)
{
  /* The same image predicate the payload support reads: a mask item projects its Base Color record,
   * a fill projects the first channel map it holds. */
  if (layer.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    return (paint_layer_channel_live(record) && record->image != nullptr) ? record->image : nullptr;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (BKE_paint_layers_fill_reads_map(ma, layer, channel)) {
      return paint_layer_channel_find(layer, channel)->image;
    }
  }
  return nullptr;
}

void BKE_paint_layers_projection_aspect_factors(const Image *image, float r_factors[2])
{
  r_factors[0] = 1.0f;
  r_factors[1] = 1.0f;
  if (image == nullptr) {
    return;
  }
  /* The footprint axis of a wider image must be longer, which the factor before the 0.5 offset
   * makes by shrinking the coordinate: exactly Ucupaint's decal Scale. */
  int width = 0;
  int height = 0;
  BKE_image_get_size(const_cast<Image *>(image), nullptr, &width, &height);
  if (width <= 0 || height <= 0) {
    return;
  }
  if (width > height) {
    r_factors[0] = float(height) / float(width);
  }
  else {
    r_factors[1] = float(width) / float(height);
  }
}

/** \} */

}  // namespace blender

