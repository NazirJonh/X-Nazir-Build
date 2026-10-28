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
  return BKE_paint_layers_mapping_enabled_get(layer) &&
         BKE_paint_layers_mapping_available(ma, layer);
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

bool PaintLayerRowMappingTarget::read(UvMappingXform &r_xform) const
{
  const MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  r_xform.offset = float2(layer->mapping.offset[0], layer->mapping.offset[1]);
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

const char *PaintLayerRowMappingTarget::undo_name() const
{
  return "Edit Layer Mapping";
}

}  // namespace blender::ed::sculpt_paint::uv_mapping
