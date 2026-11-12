/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "projection_target.hh"

#include <cmath>

#include "BLI_math_vector.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"

#include "../uv_mapping/uv_mapping_target.hh"

namespace blender::ed::sculpt_paint::projection {

MaterialPaintLayer *PaintLayerRowProjectionTarget::row() const
{
  if (material_ == nullptr || BLI_uuid_is_nil(marker_)) {
    return nullptr;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(*material_, marker_);
  if (layer == nullptr || !BKE_paint_layers_projection_applies(*material_, *layer)) {
    return nullptr;
  }
  return layer;
}

bool PaintLayerRowProjectionTarget::resolve(const bContext &C)
{
  material_ = nullptr;
  object_ = nullptr;
  marker_ = {};

  Object *ob = CTX_data_active_object(&C);
  /* The frame is edited in Sculpt Mode only: the handles must not stay behind in Object Mode or
   * show up in Texture Paint, and the Place, Move and Flip operators share this poll. */
  if (ob == nullptr || (ob->mode & OB_MODE_SCULPT) == 0) {
    return false;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr) {
    return false;
  }
  /* The one shown-row rule the Mapping panel, its Python mirror and the mapping gizmo share. */
  MaterialPaintLayer *layer = uv_mapping::mapping_shown_row_get(C, *ma);
  if (layer == nullptr || !BKE_paint_layers_projection_applies(*ma, *layer)) {
    return false;
  }

  material_ = ma;
  object_ = ob;
  marker_ = layer->marker;
  return true;
}

bool PaintLayerRowProjectionTarget::read(ProjectionFrame &r_frame) const
{
  const MaterialPaintLayer *layer = row();
  if (layer == nullptr || layer->projection == nullptr) {
    return false;
  }
  copy_v3_v3(r_frame.center, layer->projection->center);
  BKE_paint_layers_projection_size_effective(*layer, r_frame.size);
  BKE_paint_layers_projection_normal_get(*layer, r_frame.normal);
  r_frame.roll = layer->projection->roll;
  return true;
}

bool PaintLayerRowProjectionTarget::write(const ProjectionFrame &frame)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  ProjectionFrame written = frame;
  if (BKE_paint_layers_projection_size_from_controller_get(*layer) &&
      layer->projection != nullptr)
  {
    /* Read() reports one while the option is on; writing that one back would erase the extents
     * the user kept for turning the option off. Keep the stored size, which is ignored anyway. */
    copy_v3_v3(written.size, layer->projection->size);
  }
  return BKE_paint_layers_projection_set_values(
      *material_, layer, written.center, written.size, written.normal, written.roll);
}

bool PaintLayerRowProjectionTarget::write_roll(const float roll)
{
  if (!std::isfinite(roll)) {
    return false;
  }
  ProjectionFrame frame;
  if (!read(frame)) {
    return false;
  }
  frame.roll = roll;
  return write(frame);
}

Object *PaintLayerRowProjectionTarget::space_object() const
{
  const MaterialPaintLayer *layer = row();
  if (layer != nullptr && layer->projection_object != nullptr) {
    return layer->projection_object;
  }
  return object_;
}

}  // namespace blender::ed::sculpt_paint::projection
