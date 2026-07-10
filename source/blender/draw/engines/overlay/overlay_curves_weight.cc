/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "DEG_depsgraph_query.hh"

#include "draw_cache_impl_curves_weight.hh"
#include "overlay_curves_weight.hh"
#include "overlay_private.hh"

namespace blender::draw::overlay {

bool CurvesWeightPaint::is_paint_mode(const Object *object, const State &state)
{
  if (!object || object->type != OB_CURVES) {
    return false;
  }

  if (state.object_mode != OB_MODE_WEIGHT_CURVES) {
    return false;
  }

  if (!BKE_object_supports_vertex_groups(object)) {
    return false;
  }

  /* Vertex groups and deform verts live on the original object data. */
  const Object *original_object = DEG_get_original(const_cast<Object *>(object));
  if (!original_object) {
    return false;
  }

  const Curves *curves_id = id_cast<const Curves *>(original_object->data);
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  const Span<MDeformVert> dverts = curves.deform_verts();
  if (dverts.is_empty()) {
    return false;
  }

  const ListBase *defbase = BKE_object_defgroup_list(original_object);
  if (BLI_listbase_is_empty(defbase)) {
    return false;
  }

  return true;
}

void CurvesWeightPaint::setup_passes(Resources &res, const State &state)
{
  {
    auto &sub = main_ps_.sub("Weight");
    /* Depth test + depth write to keep proper occlusion ordering. */
    sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA |
                      DRW_STATE_WRITE_DEPTH,
                  state.clipping_plane_count);
    gpu::Shader *shader = res.shaders->curves_weight_paint.get();
    if (shader == nullptr) {
      enabled_ = false;
      return;
    }
    sub.shader_set(shader);
    sub.bind_texture("colorramp", &res.weight_ramp_tx);
    sub.push_constant("opacity", 1.0f);
    sub.push_constant("draw_contours", false);
    main_sub_ = &sub;
  }

  if (use_fake_shading_) {
    auto &sub = main_ps_.sub("WeightFakeShading");
    sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA |
                      DRW_STATE_WRITE_DEPTH,
                  state.clipping_plane_count);
    gpu::Shader *shader_fs = res.shaders->curves_weight_paint_fake_shading.get();
    if (shader_fs == nullptr) {
      use_fake_shading_ = false;
    }
    else {
      sub.shader_set(shader_fs);
      sub.bind_texture("colorramp", &res.weight_ramp_tx);
      sub.push_constant("opacity", 1.0f);
      sub.push_constant("draw_contours", false);
      sub.push_constant("light_dir", float3(0.0f, 0.0f, 1.0f));
      fake_shading_sub_ = &sub;
    }
  }
}

gpu::Batch *CurvesWeightPaint::geometry_batch_get(Object *object)
{
  gpu::Batch *geometry = DRW_cache_curves_weight_lines_get(object);
  if (geometry == nullptr) {
    geometry = DRW_cache_curves_weight_points_get(object);
  }
  return geometry;
}

}  // namespace blender::draw::overlay
