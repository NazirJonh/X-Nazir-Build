/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "BKE_curves.hh"
#include "BKE_lib_id.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "draw_cache_impl_curves_vertex.hh"
#include "overlay_curves_vertex.hh"
#include "overlay_private.hh"

namespace blender::draw::overlay {

bool CurvesVertexPaint::is_paint_mode(const Object *object, const State &state)
{
  if (!object || object->type != OB_CURVES) {
    return false;
  }

  if (state.object_mode != OB_MODE_VERTEX_CURVES) {
    return false;
  }

  const Curves *curves_id = id_cast<const Curves *>(object->data);
  if (curves_id == nullptr) {
    return false;
  }

  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  return !curves.is_empty();
}

void CurvesVertexPaint::setup_passes(Resources &res, const State &state)
{
  const float opacity = state.v3d ? state.v3d->overlay.vertex_paint_mode_opacity : 1.0f;

  {
    auto &sub = main_ps_.sub("VertexColor");
    sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA |
                      DRW_STATE_WRITE_DEPTH,
                  state.clipping_plane_count);
    gpu::Shader *shader = res.shaders->curves_vertex_paint.get();
    if (shader == nullptr) {
      enabled_ = false;
      return;
    }
    sub.shader_set(shader);
    sub.push_constant("opacity", opacity);
    sub.push_constant("light_dir", float3(0.0f, 0.0f, 1.0f));
    main_sub_ = &sub;
  }

  if (use_fake_shading_) {
    auto &sub = main_ps_.sub("VertexColorFakeShading");
    sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA |
                      DRW_STATE_WRITE_DEPTH,
                  state.clipping_plane_count);
    gpu::Shader *shader_fs = res.shaders->curves_vertex_paint_fake_shading.get();
    if (shader_fs == nullptr) {
      use_fake_shading_ = false;
    }
    else {
      sub.shader_set(shader_fs);
      sub.push_constant("opacity", opacity);
      sub.push_constant("light_dir", float3(0.0f, 0.0f, 1.0f));
      fake_shading_sub_ = &sub;
    }
  }
}

gpu::Batch *CurvesVertexPaint::geometry_batch_get(Object *object)
{
  gpu::Batch *geometry = DRW_cache_curves_vertex_paint_lines_get(object);
  if (geometry == nullptr) {
    geometry = DRW_cache_curves_vertex_paint_points_get(object);
  }
  return geometry;
}

}  // namespace blender::draw::overlay
