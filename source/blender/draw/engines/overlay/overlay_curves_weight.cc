/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "BKE_curves.hh"
#include "BKE_paint.hh"
#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
#include "BKE_curves_weight_paint.hh"
#include "BKE_object_deform.h"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "DEG_depsgraph_query.hh"

#include "DRW_render.hh"

#include "draw_cache.hh"
#include "draw_cache_impl_curves_weight.hh"
#include "draw_common.hh"
#include "overlay_curves_weight.hh"
#include "overlay_private.hh"

namespace blender::draw::overlay {

void CurvesWeightPaint::begin_sync(Resources &res, const State &state)
{
  enabled_ = false;
  use_fake_shading_ = false;

  if (!state.is_space_v3d()) {
    return;
  }

  /* Check if we're in curves weight paint mode */
  if (state.object_mode != OB_MODE_WEIGHT_CURVES) {
    return;
  }

  const Object *active_object = state.object_active;
  if (!active_object || active_object->type != OB_CURVES) {
    return;
  }

  enabled_ = is_curves_weight_paint_mode(active_object, state);
  if (!enabled_) {
    return;
  }

  /* Check for fake shading preference */
  use_fake_shading_ = state.v3d && (state.v3d->shading.flag & V3D_SHADING_OBJECT_OUTLINE) != 0;

  /* Initialize main pass with sub-passes (like overlay_sculpt.hh) */
  {
    curves_weight_ps_.init();
    curves_weight_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    curves_weight_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    
    {
      auto &sub = curves_weight_ps_.sub("Weight");
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
      sub.bind_texture("colorramp", res.weight_ramp_tx);
      sub.push_constant("opacity", 1.0f);
      sub.push_constant("draw_contours", false);
      weight_ps_ = &sub;
    }
    
    if (use_fake_shading_) {
      auto &sub = curves_weight_ps_.sub("WeightFakeShading");
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
        weight_fake_shading_ps_ = &sub;
      }
    }
  }
}

void CurvesWeightPaint::object_sync(Manager &manager,
                                   const ObjectRef &ob_ref,
                                   Resources &res,
                                   const State &state)
{
  if (!enabled_) {
    return;
  }

  if (ob_ref.object->type != OB_CURVES) {
    return;
  }

  if (!is_curves_weight_paint_mode(ob_ref.object, state)) {
    return;
  }

  curves_weight_sync(manager, ob_ref, res, state);
}

void CurvesWeightPaint::draw(Framebuffer &fb, Manager &manager, View &view)
{
  if (!enabled_) {
    return;
  }

  GPU_framebuffer_bind(fb);
  manager.submit(curves_weight_ps_, view);
}

bool CurvesWeightPaint::is_curves_weight_paint_mode(const Object *object, const State &state)
{
  if (!object || object->type != OB_CURVES) {
    return false;
  }

  if (state.object_mode != OB_MODE_WEIGHT_CURVES) {
    return false;
  }

  /* Check if the object supports vertex groups */
  if (!BKE_object_supports_vertex_groups(object)) {
    return false;
  }

  /* Get original object data (not evaluated) for checking deform verts */
  const Object *original_object = DEG_get_original(const_cast<Object *>(object));
  if (!original_object) {
    return false;
  }

  const Curves *curves_id = id_cast<const Curves *>(original_object->data);
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();

  /* Check for deform verts in original geometry */
  const Span<MDeformVert> dverts = curves.deform_verts();
  if (dverts.is_empty()) {
    return false;
  }

  /* Check for vertex groups */
  const ListBase *defbase = BKE_object_defgroup_list(original_object);
  if (BLI_listbase_is_empty(defbase)) {
    return false;
  }

  /* Allow overlay if vertex groups exist */
  return true;
}

void CurvesWeightPaint::curves_weight_sync(Manager &manager,
                                         const ObjectRef &ob_ref,
                                         Resources & /*res*/,
                                         const State &state)
{
  /* Get weight paint specific geometry batch */
  gpu::Batch *geometry = blender::draw::DRW_cache_curves_weight_lines_get(ob_ref.object);

  if (geometry == nullptr) {
    /* Fallback to points if lines not available */
    geometry = blender::draw::DRW_cache_curves_weight_points_get(ob_ref.object);
  }

  if (geometry == nullptr) {
    return;
  }

  ResourceHandleRange handle = manager.unique_handle(ob_ref);

  /* Draw to main weight pass */
  weight_ps_->draw(geometry, handle);

  /* Also draw to fake shading pass if enabled */
  if (use_fake_shading_ && weight_fake_shading_ps_) {
    weight_fake_shading_ps_->draw(geometry, handle);
  }
}

void CurvesWeightPaint::draw_on_render(gpu::FrameBuffer *fb, Manager &manager, View &view)
{
  if (!enabled_) {
    return;
  }

  /* Guard against nullptr framebuffer (e.g., during selection operations) */
  if (fb == nullptr) {
    return;
  }

  GPU_framebuffer_bind(fb);
  manager.submit(curves_weight_ps_, view);
}

}  // namespace blender::draw::overlay
