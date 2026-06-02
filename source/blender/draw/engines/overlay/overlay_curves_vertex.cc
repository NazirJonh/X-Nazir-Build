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

void CurvesVertexPaint::begin_sync(Resources &res, const State &state)
{
  enabled_ = false;
  use_fake_shading_ = false;

  if (!state.is_space_v3d()) {
    return;
  }

  if (state.object_mode != OB_MODE_VERTEX_CURVES) {
    return;
  }

  const Object *active_object = state.object_active;
  if (!active_object || active_object->type != OB_CURVES) {
    return;
  }

  enabled_ = is_curves_vertex_paint_mode(active_object, state);
  if (!enabled_) {
    return;
  }

  use_fake_shading_ = state.v3d && (state.v3d->shading.flag & V3D_SHADING_OBJECT_OUTLINE) != 0;

  const float opacity = state.v3d ? state.v3d->overlay.vertex_paint_mode_opacity : 1.0f;

  curves_vertex_ps_.init();
  curves_vertex_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
  curves_vertex_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

  {
    auto &sub = curves_vertex_ps_.sub("VertexColor");
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
    color_ps_ = &sub;
  }

  if (use_fake_shading_) {
    auto &sub = curves_vertex_ps_.sub("VertexColorFakeShading");
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
      color_fake_shading_ps_ = &sub;
    }
  }
}

void CurvesVertexPaint::object_sync(Manager &manager,
                                    const ObjectRef &ob_ref,
                                    Resources & /*res*/,
                                    const State &state)
{
  if (!enabled_) {
    return;
  }

  if (ob_ref.object->type != OB_CURVES) {
    return;
  }

  if (!is_curves_vertex_paint_mode(ob_ref.object, state)) {
    return;
  }

  curves_vertex_sync(manager, ob_ref, state);
}

void CurvesVertexPaint::draw(Framebuffer &fb, Manager &manager, View &view)
{
  if (!enabled_) {
    return;
  }

  GPU_framebuffer_bind(fb);
  manager.submit(curves_vertex_ps_, view);
}

bool CurvesVertexPaint::is_curves_vertex_paint_mode(const Object *object, const State &state)
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

void CurvesVertexPaint::curves_vertex_sync(Manager &manager,
                                          const ObjectRef &ob_ref,
                                          const State & /*state*/)
{
  gpu::Batch *geometry = DRW_cache_curves_vertex_paint_lines_get(ob_ref.object);
  if (geometry == nullptr) {
    geometry = DRW_cache_curves_vertex_paint_points_get(ob_ref.object);
  }

  if (geometry == nullptr) {
    return;
  }

  const ResourceHandleRange handle = manager.unique_handle(ob_ref);
  color_ps_->draw(geometry, handle);

  if (use_fake_shading_ && color_fake_shading_ps_) {
    color_fake_shading_ps_->draw(geometry, handle);
  }
}

void CurvesVertexPaint::draw_on_render(gpu::FrameBuffer *fb, Manager &manager, View &view)
{
  if (!enabled_ || fb == nullptr) {
    return;
  }

  GPU_framebuffer_bind(fb);
  manager.submit(curves_vertex_ps_, view);
}

}  // namespace blender::draw::overlay
