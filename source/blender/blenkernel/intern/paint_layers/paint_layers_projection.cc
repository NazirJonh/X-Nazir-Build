/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The projection payload of a Paint Layers row: allocating, reading and changing the frame
 * (space, axis, center, size, normal+roll, clip) and its controller object. The orientation is
 * always stored as a unit normal and a roll angle; nothing here reads or writes a node tree;
 * every mutation only marks the material stale.
 */

#include <cmath>

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLI_index_range.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"

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
    /* Space, axis, controller display, blend, cube layout, samples, volume, cage style. */
    {MA_PAINT_LAYER_MAPPING_SPACE_UV,
     false,
     OB_PLAINAXES,
     false,
     false,
     1,
     false,
     PROJECTION_CAGE_BOX},
    {MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
     true,
     OB_SINGLE_ARROW,
     false,
     false,
     1,
     false,
     PROJECTION_CAGE_BOX},
    {MA_PAINT_LAYER_MAPPING_SPACE_BOX, false, OB_CUBE, false, false, 3, true, PROJECTION_CAGE_BOX},
    {MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR,
     false,
     OB_CUBE,
     true,
     false,
     3,
     true,
     PROJECTION_CAGE_BOX},
    {MA_PAINT_LAYER_MAPPING_SPACE_CUBE, false, OB_CUBE, false, true, 1, true, PROJECTION_CAGE_BOX},
    {MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER,
     true,
     OB_CIRCLE,
     false,
     false,
     1,
     true,
     PROJECTION_CAGE_CYLINDER},
    {MA_PAINT_LAYER_MAPPING_SPACE_SPHERE,
     false,
     OB_EMPTY_SPHERE,
     false,
     false,
     1,
     true,
     PROJECTION_CAGE_SPHERE},
    /* Decal is a flat frame with a pickable axis like Planar; only its map coordinate differs. */
    {MA_PAINT_LAYER_MAPPING_SPACE_DECAL,
     true,
     OB_SINGLE_ARROW,
     false,
     false,
     1,
     false,
     PROJECTION_CAGE_BOX},
};

/* Indexed by #eMaterialPaintLayerProjectionCubeLayout. The origins are the lower-left corner of
 * each face's cell, in the order +X, -X, +Y, -Y, +Z, -Z. */
static const ProjectionCubeLayoutInfo
    projection_cube_layout_table[MA_PAINT_LAYER_PROJECTION_CUBE_LAYOUT_NUM] = {
        /* Cross 4x3: +Y above, -Y below, -X +Z +X -Z across the middle. */
        {{0.25f, 1.0f / 3.0f},
         {{0.5f, 1.0f / 3.0f},
          {0.0f, 1.0f / 3.0f},
          {0.25f, 2.0f / 3.0f},
          {0.25f, 0.0f},
          {0.25f, 1.0f / 3.0f},
          {0.75f, 1.0f / 3.0f}}},
};

static bool paint_layer_projection_cube_layout_valid(const int8_t layout)
{
  return layout >= 0 && layout < MA_PAINT_LAYER_PROJECTION_CUBE_LAYOUT_NUM;
}

const ProjectionCubeLayoutInfo &BKE_paint_layers_projection_cube_layout_info(const int8_t layout)
{
  return projection_cube_layout_table[paint_layer_projection_cube_layout_valid(layout) ?
                                          layout :
                                          MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3];
}

/** The stored roll wrapped to (-pi, pi], so equal turns store equal values. */
static float paint_layer_projection_roll_wrap(float roll)
{
  roll = fmodf(roll, 2.0f * M_PI);
  if (roll <= -M_PI) {
    roll += 2.0f * M_PI;
  }
  else if (roll > M_PI) {
    roll -= 2.0f * M_PI;
  }
  return roll;
}

/** \a normal made unit; a zero vector reads as +Z. */
static void paint_layer_projection_normal_unit(const float normal[3], float r_normal[3])
{
  copy_v3_v3(r_normal, normal);
  if (!normalize_v3(r_normal)) {
    r_normal[0] = 0.0f;
    r_normal[1] = 0.0f;
    r_normal[2] = 1.0f;
  }
}

void BKE_paint_layers_projection_normal_get(const MaterialPaintLayer &layer, float r_normal[3])
{
  const float default_normal[3] = {0.0f, 0.0f, 1.0f};
  paint_layer_projection_normal_unit(
      (layer.projection != nullptr) ? layer.projection->normal : default_normal, r_normal);
}

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
  if (BKE_paint_layers_gradient_supported(layer)) {
    /* A Gradient owns no map: the projection replaces the UV its line reads, which is only
     * meaningful for a UV line (an Object or Generated line has coordinates of its own) and only
     * for a row -- a mask item follows the row its stack hangs on, it holds no frame. */
    return layer.role == MA_PAINT_LAYER_ROLE_LAYER && layer.gradient != nullptr &&
           layer.gradient->space == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
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
  /* A transform effect owns no map: its Decal frame moves where its owner's map is read, and the
   * first iteration supports no other space on it. */
  if (paint_layer_kind_transforms_input(layer)) {
    return space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL &&
           BKE_paint_layers_effect_space_supported(layer, space);
  }
  /* A Gradient owns no map to unfold over the surface: its Decal would read as a Planar frame,
   * which is not what the mode promises. */
  if (space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL && BKE_paint_layers_gradient_supported(layer)) {
    return false;
  }
  return paint_layer_projection_space_valid(space) &&
         space != MA_PAINT_LAYER_MAPPING_SPACE_UV &&
         paint_layer_projection_payload_supported(ma, layer);
}

bool BKE_paint_layers_projection_supported(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A transform effect's Decal frame is derived from its Mirror center / pivot, never placed: the
   * Projection Frame tool and the frame fields have nothing to edit on it. */
  if (paint_layer_kind_transforms_input(layer)) {
    return false;
  }
  return paint_layer_projection_payload_supported(ma, layer);
}

bool BKE_paint_layers_projection_applies(const Material &ma, const MaterialPaintLayer &layer)
{
  if (paint_layer_kind_transforms_input(layer)) {
    /* An effect in Decal has no projection frame of the user's: its frame is derived from its
     * Mirror center (#BKE_paint_layers_effect_decal_applies), so the frame tools, the Decal gizmo
     * and the projection nodes never see it. */
    return false;
  }
  return layer.projection != nullptr &&
         paint_layer_projection_space_valid(layer.mapping.space) &&
         layer.mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_UV &&
         paint_layer_projection_payload_supported(ma, layer);
}

bool BKE_paint_layers_effect_space_supported(const MaterialPaintLayer &effect,
                                             const int16_t space)
{
  if (!paint_layer_kind_transforms_input(effect)) {
    return false;
  }
  /* A mask item moves the grey the mask has built up on the canvas. The Decal frame walks the
   * painted object's surface, and the mask's grey is computed without one, so the mask form acts
   * in UV only. */
  if (effect.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return space == MA_PAINT_LAYER_MAPPING_SPACE_UV;
  }
  return ELEM(space, MA_PAINT_LAYER_MAPPING_SPACE_UV, MA_PAINT_LAYER_MAPPING_SPACE_DECAL);
}

bool BKE_paint_layers_effect_decal_applies(const Material &ma,
                                           const MaterialPaintLayer &owner_row,
                                           const MaterialPaintLayer &effect)
{
  /* A projected owner reads its map at the projection's map coordinate, where an effect's step
   * already acts; a Decal frame on top would be a second surface walk the first iteration leaves
   * out, so the effect keeps its old meaning there. */
  return effect.mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL &&
         BKE_paint_layers_effect_space_supported(effect, effect.mapping.space) &&
         !BKE_paint_layers_projection_applies(ma, owner_row);
}

bool BKE_paint_layers_effect_decal_frame_active(const MaterialPaintLayer &effect)
{
  return paint_layer_effect_acts_in_decal_frame(effect);
}

blender::Vector<const MaterialPaintLayer *, 4> BKE_paint_layers_active_uv_transform_effects(
    const Material &ma, const MaterialPaintLayer &row)
{
  blender::Vector<const MaterialPaintLayer *, 4> effects;
  for (const MaterialPaintLayer *effect : BKE_paint_layers_active_transform_effects(ma, row)) {
    if (!BKE_paint_layers_effect_decal_applies(ma, row, *effect)) {
      effects.append(effect);
    }
  }
  return effects;
}

bool BKE_paint_layers_effect_decal_frame_init(Material &ma,
                                              MaterialPaintLayer &effect,
                                              const float center[3],
                                              const float normal[3],
                                              const float size)
{
  if (!paint_layer_kind_transforms_input(effect)) {
    return false;
  }
  MaterialPaintLayerProjection *projection = BKE_paint_layers_projection_ensure(effect);
  /* The row defaults are a projected decal's (clip, depth slab, aspect fit, facing fade); on the
   * surface only the clip means anything: the effect acts inside the patch and reads transparent
   * past its edge, the boundary the unfolded surface gives instead of the UV islands. */
  projection->flag = MA_PAINT_LAYER_PROJECTION_CLIP;
  projection->axis = MA_PAINT_LAYER_PROJECTION_AXIS_Z;
  projection->roll = 0.0f;
  projection->blend = 0.0f;
  projection->angle = PAINT_LAYER_PROJECTION_ANGLE_DEFAULT;
  if (center != nullptr && std::isfinite(center[0]) && std::isfinite(center[1]) &&
      std::isfinite(center[2]))
  {
    copy_v3_v3(projection->center, center);
  }
  else {
    copy_v3_fl(projection->center, 0.0f);
  }
  const float default_normal[3] = {0.0f, 0.0f, 1.0f};
  const bool normal_valid = normal != nullptr && std::isfinite(normal[0]) &&
                            std::isfinite(normal[1]) && std::isfinite(normal[2]);
  paint_layer_projection_normal_unit(normal_valid ? normal : default_normal, projection->normal);
  const float extent = (std::isfinite(size) && size > 0.0f) ?
                           BKE_paint_layers_projection_size_normalize(size) :
                           1.0f;
  copy_v3_fl(projection->size, extent);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

MaterialPaintLayerProjection *BKE_paint_layers_projection_ensure(MaterialPaintLayer &layer)
{
  if (layer.projection != nullptr) {
    return layer.projection;
  }
  MaterialPaintLayerProjection *projection = MEM_new<MaterialPaintLayerProjection>(__func__);
  copy_v3_fl(projection->center, 0.0f);
  copy_v3_fl(projection->size, 1.0f);
  projection->normal[0] = 0.0f;
  projection->normal[1] = 0.0f;
  projection->normal[2] = 1.0f;
  projection->roll = 0.0f;
  projection->blend = 0.0f;
  projection->axis = MA_PAINT_LAYER_PROJECTION_AXIS_Z;
  projection->cube_layout = MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3;
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
  if (paint_layer_kind_transforms_input(*layer)) {
    /* An effect's spaces are UV and a Decal frame derived from its Mirror center, and the frame
     * never acts on a projected owner (the effect keeps its old meaning in the projection's map
     * coordinate). */
    if (!BKE_paint_layers_effect_space_supported(*layer, space)) {
      return false;
    }
    if (space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL) {
      const MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(ma, *layer);
      if (owner == nullptr || BKE_paint_layers_projection_applies(ma, *owner)) {
        return false;
      }
    }
    /* The frame is derived from the Mirror center on every read, so the effect keeps no payload of
     * its own (a file saved with the placed-frame version leaves one behind; it is dropped here
     * and never read). */
    MEM_SAFE_DELETE(layer->projection);
    layer->mapping.space = space;
    BKE_paint_layers_tag_edited(ma);
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

/* -------------------------------------------------------------------- */
/** \name Frame orientation helpers
 * \{ */

/**
 * Builds the frame basis: local +Z = `normal`, +X from the reference up and rolled by `roll`.
 *
 * The reference up is +Z of the parent space. When `normal` is nearly parallel to +Z the cross
 * product degenerates, so the reference falls back to +Y. This choice keeps "top of the image
 * points up" for a vertical wall projected along +X or -X.
 */
void BKE_paint_layers_projection_frame_matrix(const float normal_in[3],
                                              const float roll,
                                              float r_mat[3][3])
{
  float z_axis[3];
  copy_v3_v3(z_axis, normal_in);
  if (!normalize_v3(z_axis)) {
    zero_v3(z_axis);
    z_axis[2] = 1.0f;
  }
  /* Reference up: +Z of parent, or +Y when nearly parallel to the normal. */
  float up[3] = {0.0f, 0.0f, 1.0f};
  if (fabsf(dot_v3v3(z_axis, up)) > 0.999f) {
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
  }
  /* Un-rolled x and y: right-handed with z. */
  float x_axis[3], y_axis[3];
  cross_v3_v3v3(x_axis, up, z_axis);
  normalize_v3(x_axis);
  cross_v3_v3v3(y_axis, z_axis, x_axis);
  /* Rotate x and y around z by `roll`. */
  const float cos_r = cosf(roll);
  const float sin_r = sinf(roll);
  float xr[3], yr[3];
  for (const int i : IndexRange(3)) {
    xr[i] = x_axis[i] * cos_r + y_axis[i] * sin_r;
    yr[i] = -x_axis[i] * sin_r + y_axis[i] * cos_r;
  }
  /* Blender stores a 3x3 as `mat[axis][component]`: `r_mat[j]` is the j-th frame axis in parent
   * space, the layout `eul_to_mat3`, `mul_m3_v3` and `copy_m4_m3` expect. */
  copy_v3_v3(r_mat[0], xr);
  copy_v3_v3(r_mat[1], yr);
  copy_v3_v3(r_mat[2], z_axis);
}

/** XYZ Euler of the same basis, for graph and CPU consumers that still take Euler. */
void BKE_paint_layers_projection_frame_euler(const float normal[3],
                                             const float roll,
                                             float r_euler[3])
{
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(normal, roll, mat);
  mat3_to_eul(r_euler, mat);
}

/**
 * Inverse: extracts the normal (axis 2) and the roll from an orthonormal basis. The roll is
 * the signed angle from the un-rolled reference x-axis to axis 0 of `mat`, measured around the
 * normal using the right-hand rule.
 */
void BKE_paint_layers_projection_frame_from_matrix(const float mat[3][3],
                                                   float r_normal[3],
                                                   float *r_roll)
{
  /* `mat[2]` = the frame's Z axis in parent space = the normal. */
  copy_v3_v3(r_normal, mat[2]);
  if (!normalize_v3(r_normal)) {
    zero_v3(r_normal);
    r_normal[2] = 1.0f;
  }
  float up[3] = {0.0f, 0.0f, 1.0f};
  if (fabsf(dot_v3v3(r_normal, up)) > 0.999f) {
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
  }
  float x_ref[3], y_ref[3];
  cross_v3_v3v3(x_ref, up, r_normal);
  normalize_v3(x_ref);
  cross_v3_v3v3(y_ref, r_normal, x_ref);
  /* `mat[0]` = the rolled x axis in parent space. */
  *r_roll = atan2f(dot_v3v3(mat[0], y_ref), dot_v3v3(mat[0], x_ref));
}

void BKE_paint_layers_projection_normal_frame_to_tangent(const float frame_axes[3][3],
                                                         const float tangent[3],
                                                         const float bitangent[3],
                                                         const float surface_normal[3],
                                                         const float n_frame[3],
                                                         float r_n_tangent[3])
{
  /* The frame carries the vector into the surface's space: `frame_axes[c]` is the c-th axis (a
   * column of the rotation, as `eul_to_mat3` stores it), so the result is the sum of the axes
   * weighted by the vector's components, `R * n_frame`. Reading `frame_axes[i][c]` as row `i`
   * would apply the inverse rotation. */
  float n_world[3];
  mul_v3_v3fl(n_world, frame_axes[0], n_frame[0]);
  madd_v3_v3fl(n_world, frame_axes[1], n_frame[1]);
  madd_v3_v3fl(n_world, frame_axes[2], n_frame[2]);
  /* Encoded in the target tangent basis; the components renormalized, since a surface tangent
   * basis is only approximately orthonormal and the decoded vector came out of an interpolation. */
  float encoded[3] = {dot_v3v3(n_world, tangent),
                      dot_v3v3(n_world, bitangent),
                      dot_v3v3(n_world, surface_normal)};
  if (!normalize_v3(encoded)) {
    /* A degenerate input (a zero vector, a degenerate basis) passes through unchanged rather than
     * encoding a NaN. */
    copy_v3_v3(r_n_tangent, n_frame);
    return;
  }
  copy_v3_v3(r_n_tangent, encoded);
}

/**
 * Frame orientation for a surface hit.
 *
 * The frame's +Z looks outward (toward the observer). Without FACING_INVERT the facing weight
 * peaks at dot(world_normal, frame_Z) = +1, so the face directly at the projector is kept. The
 * image is not mirrored because `cross(up, normal)` puts frame X to the observer's right for a
 * vertical wall. The caller passes `surface_normal` already in the frame's space.
 */
void BKE_paint_layers_projection_orientation_from_surface(const float surface_normal[3],
                                                         float r_normal[3],
                                                         float *r_roll)
{
  copy_v3_v3(r_normal, surface_normal);
  if (!normalize_v3(r_normal)) {
    r_normal[0] = 0.0f;
    r_normal[1] = 0.0f;
    r_normal[2] = 1.0f;
  }
  *r_roll = 0.0f;
}

/** \} */

bool BKE_paint_layers_projection_set_orientation(Material &ma,
                                                 MaterialPaintLayer *layer,
                                                 const float normal[3],
                                                 float roll)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(normal[i])) {
      return false;
    }
  }
  if (!std::isfinite(roll)) {
    return false;
  }
  paint_layer_projection_normal_unit(normal, layer->projection->normal);
  layer->projection->roll = paint_layer_projection_roll_wrap(roll);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_roll(Material &ma,
                                          MaterialPaintLayer *layer,
                                          const float roll)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  if (!std::isfinite(roll)) {
    return false;
  }
  layer->projection->roll = paint_layer_projection_roll_wrap(roll);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

bool BKE_paint_layers_projection_set_values_euler(Material &ma,
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
  /* Euler from a script or a legacy caller: decompose it into normal+roll before storing. */
  float mat[3][3];
  eul_to_mat3(mat, rotation);
  float normal[3];
  float roll;
  BKE_paint_layers_projection_frame_from_matrix(mat, normal, &roll);
  return BKE_paint_layers_projection_set_values(ma, layer, center, size, normal, roll);
}

bool BKE_paint_layers_projection_set_values(Material &ma,
                                            MaterialPaintLayer *layer,
                                            const float center[3],
                                            const float size[3],
                                            const float normal_in[3],
                                            const float roll)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(center[i]) || !std::isfinite(size[i]) || !std::isfinite(normal_in[i])) {
      return false;
    }
  }
  if (!std::isfinite(roll)) {
    return false;
  }
  copy_v3_v3(layer->projection->center, center);
  for (int i = 0; i < 3; i++) {
    layer->projection->size[i] = BKE_paint_layers_projection_size_normalize(size[i]);
  }
  paint_layer_projection_normal_unit(normal_in, layer->projection->normal);
  layer->projection->roll = paint_layer_projection_roll_wrap(roll);
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
  /* Euler wrapper: decompose into normal+roll via the frame basis matrix. */
  float mat[3][3];
  eul_to_mat3(mat, rotation);
  float normal[3];
  float roll;
  BKE_paint_layers_projection_frame_from_matrix(mat, normal, &roll);
  copy_v3_v3(layer->projection->normal, normal);
  layer->projection->roll = roll;
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
  /* The weight peaks on the faces whose normal looks toward the frame's +Z axis (the projection
   * direction): component = dot(world_normal, frame_Z) runs through scale and offset, and the
   * positive scale for the default convention maps component from cos_min (the angular limit)
   * to cos_max (full weight) onto t from 0 to 1. Flip negates the scale, so the weight peaks
   * instead on the faces looking away from +Z. Both normal paths read this one pair. */
  r_scale = invert ? -scale : scale;
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

float BKE_paint_layers_projection_blend_get(const MaterialPaintLayer &layer)
{
  if (layer.projection != nullptr) {
    return clamp_f(layer.projection->blend, 0.0f, 1.0f);
  }
  return 0.0f;
}

bool BKE_paint_layers_projection_set_blend(Material &ma,
                                           MaterialPaintLayer *layer,
                                           const float blend)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr || !std::isfinite(blend)) {
    return false;
  }
  layer->projection->blend = clamp_f(blend, 0.0f, 1.0f);
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

int8_t BKE_paint_layers_projection_cube_layout_get(const MaterialPaintLayer &layer)
{
  /* An unknown stored value (a damaged payload) reads as the first layout, like the table lookup. */
  if (layer.projection != nullptr &&
      BKE_paint_layers_projection_mode_info(layer.mapping.space).uses_cube_layout &&
      paint_layer_projection_cube_layout_valid(layer.projection->cube_layout))
  {
    return layer.projection->cube_layout;
  }
  return MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3;
}

bool BKE_paint_layers_projection_set_cube_layout(Material &ma,
                                                 MaterialPaintLayer *layer,
                                                 const int8_t layout)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr ||
      !paint_layer_projection_cube_layout_valid(layout))
  {
    return false;
  }
  if (layer->projection->cube_layout == layout) {
    return true;
  }
  layer->projection->cube_layout = layout;
  BKE_paint_layers_tag_edited(ma);
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

ePaintLayerProjectionPreset BKE_paint_layers_projection_preset_get(
    const MaterialPaintLayer &layer)
{
  if (layer.projection == nullptr) {
    return PAINT_LAYER_PROJ_PRESET_CUSTOM;
  }
  const int16_t f = layer.projection->flag;
  /* Decal: CLIP | DEPTH | FIT_ASPECT | FACING, no FACING_INVERT. */
  const int16_t decal_on = MA_PAINT_LAYER_PROJECTION_CLIP | MA_PAINT_LAYER_PROJECTION_DEPTH |
                           MA_PAINT_LAYER_PROJECTION_FIT_ASPECT |
                           MA_PAINT_LAYER_PROJECTION_FACING;
  const int16_t decal_off = MA_PAINT_LAYER_PROJECTION_FACING_INVERT;
  if ((f & decal_on) == decal_on && (f & decal_off) == 0) {
    return PAINT_LAYER_PROJ_PRESET_DECAL;
  }
  /* Flat: CLIP | DEPTH | FACING | FIT_ASPECT all off. */
  const int16_t flat_flags = MA_PAINT_LAYER_PROJECTION_CLIP | MA_PAINT_LAYER_PROJECTION_DEPTH |
                             MA_PAINT_LAYER_PROJECTION_FIT_ASPECT |
                             MA_PAINT_LAYER_PROJECTION_FACING;
  if ((f & flat_flags) == 0) {
    return PAINT_LAYER_PROJ_PRESET_FLAT;
  }
  return PAINT_LAYER_PROJ_PRESET_CUSTOM;
}

bool BKE_paint_layers_projection_apply_preset(Material &ma,
                                              MaterialPaintLayer *layer,
                                              const ePaintLayerProjectionPreset preset)
{
  if (!paint_layer_owned(ma, layer) || layer->projection == nullptr) {
    return false;
  }
  constexpr int16_t relevant = MA_PAINT_LAYER_PROJECTION_CLIP | MA_PAINT_LAYER_PROJECTION_DEPTH |
                               MA_PAINT_LAYER_PROJECTION_FIT_ASPECT |
                               MA_PAINT_LAYER_PROJECTION_FACING |
                               MA_PAINT_LAYER_PROJECTION_FACING_INVERT;
  const int16_t other = layer->projection->flag & ~relevant;
  int16_t new_flag = other;
  switch (preset) {
    case PAINT_LAYER_PROJ_PRESET_DECAL: {
      new_flag |= MA_PAINT_LAYER_PROJECTION_CLIP | MA_PAINT_LAYER_PROJECTION_DEPTH |
                  MA_PAINT_LAYER_PROJECTION_FIT_ASPECT | MA_PAINT_LAYER_PROJECTION_FACING;
      layer->projection->angle = PAINT_LAYER_PROJECTION_ANGLE_DEFAULT;
      break;
    }
    case PAINT_LAYER_PROJ_PRESET_FLAT: {
      /* CLIP, DEPTH, FIT_ASPECT, FACING all off. */
      break;
    }
    case PAINT_LAYER_PROJ_PRESET_CUSTOM: {
      return true;
    }
    default: {
      /* A value from outside the enum (a script) must not read as Flat. */
      return false;
    }
  }
  if (new_flag == layer->projection->flag) {
    return true;
  }
  layer->projection->flag = new_flag;
  BKE_paint_layers_tag_value_edited(ma);
  return true;
}

/** \} */


}  // namespace blender

