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
  
  printf("[DEBUG] CurvesWeightPaint::begin_sync called\n");
  
  if (!state.is_space_v3d()) {
    printf("[DEBUG] begin_sync: not in 3D view\n");
    return;
  }

  /* Check if we're in curves weight paint mode */
  if (state.object_mode != OB_MODE_WEIGHT_CURVES) {
    printf("[DEBUG] begin_sync: not in WEIGHT_CURVES mode (mode=%d)\n", state.object_mode);
    return;
  }

  const Object *active_object = state.object_active;
  if (!active_object || active_object->type != OB_CURVES) {
    printf("[DEBUG] begin_sync: no active object or not CURVES type\n");
    return;
  }

  printf("[DEBUG] begin_sync: checking weight paint mode...\n");
  enabled_ = is_curves_weight_paint_mode(active_object, state);
  
  printf("[DEBUG] begin_sync: enabled_=%d\n", enabled_);
  
  if (!enabled_) {
    printf("[DEBUG] begin_sync: overlay NOT enabled\n");
    return;
  }
  
  printf("[DEBUG] begin_sync: overlay ENABLED, initializing passes...\n");

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
        printf("[DEBUG] curves_weight_paint shader is NULL, skipping\n");
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
        printf("[DEBUG] curves_weight_paint_fake_shading shader is NULL, skipping\n");
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
  printf("[DEBUG] CurvesWeightPaint::object_sync called for object '%s'\n", 
         ob_ref.object->id.name + 2);
  
  if (!enabled_) {
    printf("[DEBUG] object_sync: overlay not enabled, skipping\n");
    return;
  }

  if (ob_ref.object->type != OB_CURVES) {
    printf("[DEBUG] object_sync: object is not CURVES type\n");
    return;
  }

  if (!is_curves_weight_paint_mode(ob_ref.object, state)) {
    printf("[DEBUG] object_sync: object not in weight paint mode\n");
    return;
  }

  printf("[DEBUG] object_sync: syncing curves weight geometry...\n");
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
  printf("[DEBUG] CurvesWeightPaint::is_curves_weight_paint_mode called\n");
  
  if (!object || object->type != OB_CURVES) {
    printf("[DEBUG] Overlay check failed: object is null or not CURVES (type=%d)\n", 
           object ? object->type : -1);
    return false;
  }

  printf("[DEBUG] Object type is CURVES, mode=%d, expected OB_MODE_WEIGHT_CURVES=%d\n", 
         state.object_mode, OB_MODE_WEIGHT_CURVES);

  if (state.object_mode != OB_MODE_WEIGHT_CURVES) {
    printf("[DEBUG] Overlay check failed: not in WEIGHT_CURVES mode\n");
    return false;
  }

  /* Check if the object supports vertex groups */
  if (!BKE_object_supports_vertex_groups(object)) {
    printf("[DEBUG] Overlay check failed: object doesn't support vertex groups\n");
    return false;
  }

  /* Get original object data (not evaluated) for checking deform verts */
  const Object *original_object = DEG_get_original(const_cast<Object *>(object));
  if (!original_object) {
    printf("[DEBUG] Overlay check failed: original_object is null\n");
    return false;
  }

  const Curves *curves_id = id_cast<const Curves *>(original_object->data);
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  
  /* Check for deform verts in original geometry */
  const Span<MDeformVert> dverts = curves.deform_verts();
  printf("[DEBUG] Deform verts check: dverts.size()=%zu, is_empty=%d\n", 
         dverts.size(), dverts.is_empty());
  
  if (dverts.is_empty()) {
    printf("[DEBUG] Overlay check failed: deform verts are empty\n");
    return false;
  }

  /* Check for vertex groups */
  const ListBase *defbase = BKE_object_defgroup_list(original_object);
  const int vgroup_count = BLI_listbase_count(defbase);
  const int active_index_eval = BKE_object_defgroup_active_index_get(object);
  const int active_index_original = BKE_object_defgroup_active_index_get(original_object);
  printf("[DEBUG] Vertex groups check: count=%d, is_empty=%d\n", 
         vgroup_count, BLI_listbase_is_empty(defbase));
  printf("[DEBUG] Vertex groups active index: eval_object=%d original_object=%d\n",
         active_index_eval,
         active_index_original);
  
  if (BLI_listbase_is_empty(defbase)) {
    printf("[DEBUG] Overlay check failed: no vertex groups\n");
    return false;
  }

  /* Allow overlay if vertex groups exist */
  printf("[DEBUG] ✅ Overlay check PASSED - enabling curves weight paint overlay\n");
  return true;
}

void CurvesWeightPaint::curves_weight_sync(Manager &manager,
                                         const ObjectRef &ob_ref,
                                         Resources & /*res*/,
                                         const State &state)
{
  printf("[DEBUG] curves_weight_sync: getting weight paint geometry batch...\n");
  
  /* Get weight paint specific geometry batch */
  gpu::Batch *geometry = blender::draw::DRW_cache_curves_weight_lines_get(ob_ref.object);
  
  if (geometry == nullptr) {
    printf("[DEBUG] curves_weight_sync: weight geometry is NULL, trying points batch\n");
    /* Fallback to points if lines not available */
    geometry = blender::draw::DRW_cache_curves_weight_points_get(ob_ref.object);
  }
  
  if (geometry == nullptr) {
    printf("[DEBUG] curves_weight_sync: no weight paint geometry available\n");
    return;
  }
  
  printf("[DEBUG] curves_weight_sync: got weight geometry batch, creating handle\n");
  ResourceHandleRange handle = manager.unique_handle(ob_ref);
  
  /* Draw to main weight pass */
  printf("[DEBUG] curves_weight_sync: drawing to weight_ps_\n");
  weight_ps_->draw(geometry, handle);
  
  /* Also draw to fake shading pass if enabled */
  if (use_fake_shading_ && weight_fake_shading_ps_) {
    printf("[DEBUG] curves_weight_sync: drawing to fake shading pass\n");
    weight_fake_shading_ps_->draw(geometry, handle);
  }
  
  printf("[DEBUG] curves_weight_sync: COMPLETED\n");
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
