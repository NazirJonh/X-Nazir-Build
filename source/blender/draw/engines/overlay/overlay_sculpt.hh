/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "DEG_depsgraph_query.hh"

#include "DRW_render.hh"
#include "bmesh.hh"

#include "draw_cache_impl.hh"
#include "draw_sculpt.hh"

#include "overlay_base.hh"

/* Debug throttling */
static int overlay_debug_counter = 0;
#define OVERLAY_DEBUG_INTERVAL 60
#define OVERLAY_DEBUG_PRINTF(...) \
  do { \
    overlay_debug_counter++; \
    if (overlay_debug_counter % OVERLAY_DEBUG_INTERVAL == 1) { \
      printf(__VA_ARGS__); \
    } \
  } while (0)

namespace blender::draw::overlay {

/**
 * Display sculpt modes overlays.
 * Covers face sets and mask for meshes.
 * Draw curve cages (curve guides) for curve sculpting.
 */
class Sculpts : Overlay {

 private:
  PassSimple sculpt_mask_ = {"SculptMaskAndFaceSet"};
  PassSimple::Sub *mesh_ps_ = nullptr;
  PassSimple::Sub *curves_ps_ = nullptr;
  PassSimple::Sub *curves_brush_highlight_ps_ = nullptr;  /* NEW: sub pass for brush highlight */

  PassSimple sculpt_curve_cage_ = {"SculptCage"};

  PassSimple sculpt_curves_points_ = {"SculptCurvesPoints"};

  bool show_curves_cage_ = false;
  bool show_face_set_ = false;
  bool show_mask_ = false;
  bool show_brush_highlight_ = false;
  bool show_curves_points_ = false;

  public:
  void begin_sync(Resources &res, const State &state) final
  {
    show_curves_cage_ = state.show_sculpt_curves_cage();
    show_face_set_ = state.show_sculpt_face_sets();
    show_mask_ = state.show_sculpt_mask();
    show_brush_highlight_ = state.object_mode == OB_MODE_SCULPT_CURVES;

    enabled_ = state.is_space_v3d() && !state.is_wire() && !res.is_selection() &&
               !state.is_depth_only_drawing &&
               ELEM(state.object_mode, OB_MODE_SCULPT_CURVES, OB_MODE_SCULPT) &&
               (show_curves_cage_ || show_face_set_ || show_mask_ || show_brush_highlight_ || show_curves_points_);

    /* One-liner debug to avoid throttling sync issues */
    OVERLAY_DEBUG_PRINTF("[BrushHighlight] begin_sync: obj_mode=%d cage=%d mask=%d highlight=%d => enabled=%d\n",
                         state.object_mode, show_curves_cage_, show_mask_, show_brush_highlight_, enabled_);

    if (!enabled_) {
      /* Not used. But release the data. */
      sculpt_mask_.init();
      sculpt_curve_cage_.init();
      sculpt_curves_points_.init();
      return;
    }

    float curve_cage_opacity = show_curves_cage_ ? state.overlay.sculpt_curves_cage_opacity : 0.0f;
    float face_set_opacity = show_face_set_ ? state.overlay.sculpt_mode_face_sets_opacity : 0.0f;
    float mask_opacity = show_mask_ ? state.overlay.sculpt_mode_mask_opacity : 0.0f;
    float brush_highlight_opacity = show_brush_highlight_ ? state.overlay.sculpt_curves_brush_highlight_opacity : 0.0f;

    {
      sculpt_mask_.init();
      sculpt_mask_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      sculpt_mask_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      {
        auto &sub = sculpt_mask_.sub("Mesh");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_MUL,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->sculpt_mesh.get());
        sub.push_constant("mask_opacity", mask_opacity);
        sub.push_constant("face_sets_opacity", face_set_opacity);
        mesh_ps_ = &sub;
      }
      {
        auto &sub = sculpt_mask_.sub("Curves");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->sculpt_curves.get());
        sub.push_constant("selection_opacity", mask_opacity);
        curves_ps_ = &sub;
      }
      /* NEW: Brush highlight sub pass - renders on top of everything */
      {
        auto &sub = sculpt_mask_.sub("BrushHighlight");
        /* Use DEPTH_ALWAYS to render on top of other geometry */
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_ALWAYS | DRW_STATE_BLEND_ALPHA,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->sculpt_curves_brush_highlight.get());
        sub.push_constant("brush_highlight_opacity", brush_highlight_opacity);
        curves_brush_highlight_ps_ = &sub;
      }
    }
    {
      auto &pass = sculpt_curve_cage_;
      pass.init();
      pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                     state.clipping_plane_count);
      pass.shader_set(res.shaders->sculpt_curves_cage.get());
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      pass.push_constant("opacity", curve_cage_opacity);
    }
    {
      auto &pass = sculpt_curves_points_;
      pass.init();
      pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_GREATER | DRW_STATE_BLEND_ALPHA,
                     state.clipping_plane_count);
      pass.shader_set(res.shaders->sculpt_curves_points.get());
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      pass.push_constant("brush_highlight_opacity", 0.5f);
      pass.push_constant("brush_highlight_color", float3(1.0f, 0.5f, 0.0f));
    }
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final
  {
    if (!enabled_) {
      return;
    }

    switch (ob_ref.object->type) {
      case OB_MESH:
        mesh_sync(manager, ob_ref, state);
        break;
      case OB_CURVES:
        curves_sync(manager, ob_ref, state);
        break;
    }
  }

  void curves_sync(Manager &manager, const ObjectRef &ob_ref, const State &state)
  {
    /* Always print to debug - no throttling */
    printf("[BrushHighlight] curves_sync: obj_type=%d show_brush_highlight_=%d\n",
           ob_ref.object->type, show_brush_highlight_);

    blender::Curves &curves = DRW_object_get_data_for_drawing<blender::Curves>(*ob_ref.object);

    /* As an optimization, draw nothing if everything is selected. */
    if (show_mask_ && !everything_selected(curves)) {
      /* Retrieve the location of the texture. */
      bool is_point_domain;
      bool is_valid;
      gpu::VertBufPtr &select_attr_buf = DRW_curves_texture_for_evaluated_attribute(
          &curves, ".selection", is_point_domain, is_valid);
      if (is_valid) {
        /* Evaluate curves and their attributes if necessary. */
        const char *error = nullptr;
        /* The error string will always have been printed by the engine already.
         * No need to display it twice. */
        gpu::Batch *geometry = curves_sub_pass_setup(
            *curves_ps_, state.scene, ob_ref.object, error);
        if (select_attr_buf.get()) {
          ResourceHandleRange handle = manager.unique_handle(ob_ref);

          curves_ps_->push_constant("is_point_domain", is_point_domain);
          curves_ps_->bind_texture("selection_tx", select_attr_buf);
          curves_ps_->draw(geometry, handle);
        }
      }
    }

    if (show_curves_cage_) {
      ResourceHandleRange handle = manager.unique_handle(ob_ref);

      gpu::Batch *geometry = DRW_curves_batch_cache_get_sculpt_curves_cage(&curves);
      sculpt_curve_cage_.draw(geometry, handle);
    }

    if (show_brush_highlight_ && curves_brush_highlight_ps_) {
      static int highlight_debug_counter = 0;
      highlight_debug_counter++;
      if (highlight_debug_counter % 60 == 1) {
        printf("[BrushHighlight] BRUSH_HIGHLIGHT BLOCK: using curves_sub_pass_setup\n");
      }
      gpu::VertBuf *brush_highlight_buf = DRW_curves_batch_cache_get_brush_highlight(&curves);
      if (brush_highlight_buf) {
        const char *error = nullptr;
        gpu::Batch *geometry = curves_sub_pass_setup(
            *curves_brush_highlight_ps_, state.scene, ob_ref.object, error);
        if (geometry) {
          ResourceHandleRange handle = manager.unique_handle(ob_ref);
          curves_brush_highlight_ps_->bind_texture("brush_highlight_tx", brush_highlight_buf);
          curves_brush_highlight_ps_->draw(geometry, handle);
          if (highlight_debug_counter % 60 == 1) {
            printf("[BrushHighlight]   curves geometry=%p, drawing!\n", (void*)geometry);
          }
        }
      }
    }
  }

  void mesh_sync(Manager &manager, const ObjectRef &ob_ref, const State &state)
  {
    if (!show_face_set_ && !show_mask_) {
      /* Nothing to display. */
      return;
    }

    const SculptSession *sculpt_session = ob_ref.object->runtime->sculpt_session;
    if (sculpt_session == nullptr) {
      return;
    }

    bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob_ref.object);
    if (!pbvh) {
      /* It is possible to have SculptSession without pbvh::Tree. This happens, for example, when
       * toggling object mode to sculpt then to edit mode. */
      return;
    }

    /* Using the original object/geometry is necessary because we skip depsgraph updates in sculpt
     * mode to improve performance. This means the evaluated mesh doesn't have the latest face set,
     * visibility, and mask data. */
    Object *object_orig = DEG_get_original(ob_ref.object);
    if (!object_orig) {
      BLI_assert_unreachable();
      return;
    }

    switch (pbvh->type()) {
      case bke::pbvh::Type::Mesh: {
        const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*object_orig);
        if (!mesh.attributes().contains(".sculpt_face_set") &&
            !mesh.attributes().contains(".sculpt_mask"))
        {
          return;
        }
        break;
      }
      case bke::pbvh::Type::Grids: {
        const SubdivCCG &subdiv_ccg = *sculpt_session->subdiv_ccg;
        const Mesh &base_mesh = DRW_object_get_data_for_drawing<Mesh>(*object_orig);
        if (subdiv_ccg.masks.is_empty() && !base_mesh.attributes().contains(".sculpt_face_set")) {
          return;
        }
        break;
      }
      case bke::pbvh::Type::BMesh: {
        const BMesh &bm = *sculpt_session->bm;
        if (!CustomData_has_layer_named(&bm.pdata, CD_PROP_FLOAT, ".sculpt_face_set") &&
            !CustomData_has_layer_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask"))
        {
          return;
        }
        break;
      }
    }

    const bool use_pbvh = BKE_sculptsession_use_pbvh_draw(ob_ref.object, state.rv3d);
    if (use_pbvh) {
      ResourceHandleRange handle = manager.unique_handle_for_sculpt(ob_ref);

      SculptBatchFeature sculpt_batch_features_ = (show_face_set_ ? SCULPT_BATCH_FACE_SET :
                                                                    SCULPT_BATCH_DEFAULT) |
                                                  (show_mask_ ? SCULPT_BATCH_MASK :
                                                                SCULPT_BATCH_DEFAULT);

      for (SculptBatch &batch : sculpt_batches_get(ob_ref.object, sculpt_batch_features_)) {
        mesh_ps_->draw(batch.batch, handle);
      }
    }
    else {
      ResourceHandleRange handle = manager.unique_handle(ob_ref);

      Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_ref.object);
      gpu::Batch *sculpt_overlays = DRW_mesh_batch_cache_get_sculpt_overlays(mesh);
      mesh_ps_->draw(sculpt_overlays, handle);
    }
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(sculpt_curve_cage_, view);
  }

  void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(sculpt_mask_, view);
  }

 private:
  bool everything_selected(const blender::Curves &curves_id)
  {
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
        ".selection", bke::AttrDomain::Point, true);
    return selection.is_single() && selection.get_internal_single();
  }
};

}  // namespace blender::draw::overlay
