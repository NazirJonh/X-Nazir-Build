/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "uv_mapping_target.hh"

#include <cmath>

#include "BLI_uuid.h"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"

#include "ED_image.hh"

namespace blender::ed::sculpt_paint::uv_mapping {

namespace {

/**
 * Whether the row's mapping is edited right now: on, on a row that keeps a mapping. A Fill answers
 * yes before it has a map, so the gizmo is there as soon as the row is made, exactly like the
 * Mapping panel -- the values it writes take effect when a texture is slotted.
 */
bool mapping_is_edited(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A projected row is addressed by its frame and follows only its UV wrap: no island-UV adjust
   * applies, so there is nothing for this gizmo to move. */
  /* A transform effect that is switched off moves nothing, so its handles go with it. */
  if (BKE_paint_layers_is_transform_effect(layer) && (layer.flag & MA_PAINT_LAYER_ENABLED) == 0) {
    return false;
  }
  return BKE_paint_layers_mapping_enabled_get(layer) &&
         BKE_paint_layers_mapping_available(ma, layer) &&
         !BKE_paint_layers_projection_applies(ma, layer);
}

}  // namespace

MaterialPaintLayer *mapping_shown_row_get(const bContext &C, Material &ma)
{
  if (MaterialPaintLayer *item = BKE_paint_layers_active_mask_item_get(ma)) {
    return item;
  }

  MaterialPaintLayer *layer = BKE_paint_layers_active_layer_get(ma);
  if (layer == nullptr) {
    return nullptr;
  }

  const Scene *scene = CTX_data_scene(&C);
  if (scene != nullptr && layer->mask_stack.first != nullptr &&
      scene->toolsettings->paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK)
  {
    if (MaterialPaintLayer *base = BKE_paint_layers_mask_base(*layer)) {
      return base;
    }
  }
  return layer;
}

MaterialPaintLayer *PaintLayerRowMappingTarget::row() const
{
  if (material_ == nullptr || BLI_uuid_is_nil(marker_)) {
    return nullptr;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(*material_, marker_);
  if (layer == nullptr || !mapping_is_edited(*material_, *layer)) {
    return nullptr;
  }
  return layer;
}

bool PaintLayerRowMappingTarget::resolve(const bContext &C)
{
  material_ = nullptr;
  marker_ = {};

  const SpaceImage *sima = CTX_wm_space_image(&C);
  if (sima == nullptr || sima->mode != SI_MODE_PAINT || sima->image == nullptr) {
    return false;
  }

  Object *ob = CTX_data_active_object(&C);
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr) {
    return false;
  }

  /* The panel edits the active object's active material; the gizmo may only edit the material
   * the Image Editor actually shows (a pinned image or a changed object would disagree). */
  if (ED_space_image_composite_material_get(CTX_data_main(&C), sima->image) != ma) {
    return false;
  }

  MaterialPaintLayer *layer = mapping_shown_row_get(C, *ma);
  if (layer == nullptr || !mapping_is_edited(*ma, *layer)) {
    return false;
  }

  material_ = ma;
  marker_ = layer->marker;
  return true;
}

bool PaintLayerRowMappingTarget::resolve_row(const bContext &C, const bUUID marker)
{
  material_ = nullptr;
  marker_ = {};

  Material *ma = BKE_paint_layers_active_material_get(CTX_data_active_object(&C));
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }

  MaterialPaintLayer *layer = BLI_uuid_is_nil(marker) ? mapping_shown_row_get(C, *ma) :
                                                        BKE_paint_layers_find(*ma, marker);
  if (layer == nullptr || !mapping_is_edited(*ma, *layer)) {
    return false;
  }

  material_ = ma;
  marker_ = layer->marker;
  return true;
}


bool PaintLayerRowMappingTarget::read(UvMappingXform &r_xform) const
{
  const MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  /* The offset the canvas shows: a Transform Mapping effect keeps a pivot, which the cage has to
   * follow; set_values takes the same effective offset back. */
  float offset[2];
  BKE_paint_layers_transform_effective_offset(*layer, offset);
  r_xform.offset = float2(offset[0], offset[1]);
  r_xform.scale = float2(BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]),
                         BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[1]));
  r_xform.rotation = layer->mapping.rotation;
  return true;
}

bool PaintLayerRowMappingTarget::write(const UvMappingXform &xform)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }

  /* A non-finite value would poison the stored mapping, and every consumer reads it. */
  if (!std::isfinite(xform.offset.x) || !std::isfinite(xform.offset.y) ||
      !std::isfinite(xform.scale.x) || !std::isfinite(xform.scale.y) ||
      !std::isfinite(xform.rotation))
  {
    return false;
  }

  const float offset[2] = {xform.offset.x, xform.offset.y};
  const float scale[2] = {xform.scale.x, xform.scale.y};
  return BKE_paint_layers_mapping_set_values(*material_, layer, offset, scale, xform.rotation);
}

bool PaintLayerRowMappingTarget::scale_locked() const
{
  const MaterialPaintLayer *layer = row();
  return layer != nullptr && BKE_paint_layers_mapping_scale_lock_get(*layer);
}

bool PaintLayerRowMappingTarget::set_scale_locked(const bool locked)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  /* Locking joins the axes at the X value, which is a scale change: like the panel's toggle, it
   * keeps the tile where the cage draws it. */
  return BKE_paint_layers_mapping_set_scale_lock_holding_tile(*material_, layer, locked);
}

bool PaintLayerRowMappingTarget::mirror_applies() const
{
  const MaterialPaintLayer *layer = row();
  return layer != nullptr && material_ != nullptr &&
         BKE_paint_layers_mapping_mirror_applies(*material_, *layer);
}

bool PaintLayerRowMappingTarget::read_mirror_center(float r_center[2]) const
{
  const MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  BKE_paint_layers_mapping_mirror_center_get(*layer, r_center);
  return true;
}

bool PaintLayerRowMappingTarget::write_mirror_center(const float2 &center)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  const float values[2] = {center.x, center.y};
  return BKE_paint_layers_mapping_set_mirror_center(*material_, layer, values);
}

bool PaintLayerRowMappingTarget::has_pivot() const
{
  const MaterialPaintLayer *layer = row();
  return layer != nullptr && BKE_paint_layers_is_transform_effect(*layer) &&
         layer->kind_subtype == MA_PAINT_LAYER_FILTER_TRANSFORM;
}

bool PaintLayerRowMappingTarget::is_mirror_effect() const
{
  const MaterialPaintLayer *layer = row();
  return layer != nullptr && BKE_paint_layers_is_transform_effect(*layer) &&
         layer->kind_subtype == MA_PAINT_LAYER_FILTER_MIRROR;
}

bool PaintLayerRowMappingTarget::read_pivot(float2 &r_pivot) const
{
  const MaterialPaintLayer *layer = row();
  if (layer == nullptr || !has_pivot()) {
    return false;
  }
  float pivot[2];
  BKE_paint_layers_mapping_mirror_center_get(*layer, pivot);
  r_pivot = float2(pivot[0], pivot[1]);
  return true;
}

bool PaintLayerRowMappingTarget::write_pivot(const float2 &pivot)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr || !has_pivot()) {
    return false;
  }
  const float values[2] = {pivot.x, pivot.y};
  return BKE_paint_layers_set_transform_pivot(*material_, layer, values);
}

bool PaintLayerRowMappingTarget::write_pivoted_scale(const float2 &scale)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr || !has_pivot() || !std::isfinite(scale.x) || !std::isfinite(scale.y)) {
    return false;
  }
  const float values[2] = {scale.x, scale.y};
  return BKE_paint_layers_mapping_set_scale(*material_, layer, values);
}

bool PaintLayerRowMappingTarget::write_pivoted_rotation(const float rotation)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr || !has_pivot() || !std::isfinite(rotation)) {
    return false;
  }
  return BKE_paint_layers_mapping_set_rotation(*material_, layer, rotation);
}

const char *PaintLayerRowMappingTarget::pivot_undo_name() const
{
  return "Move Layer Pivot";
}

const char *PaintLayerRowMappingTarget::mirror_undo_name() const
{
  return "Move Layer Mirror Center";
}

const char *PaintLayerRowMappingTarget::undo_name() const
{
  return "Edit Layer Mapping";
}

}  // namespace blender::ed::sculpt_paint::uv_mapping
