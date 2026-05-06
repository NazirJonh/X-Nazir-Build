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

#include "DNA_scene_enums.h"

#include "DRW_render.hh"
#include "bmesh.hh"

#include "draw_cache_impl.hh"
#include "draw_sculpt.hh"

#include "overlay_base.hh"
#include "overlay_symmetry_contour.hh"

/* DEBUG: Symmetry plane debugging */
#include <iostream>
#define DEBUG_SYMMETRY_PLANE 1
#if DEBUG_SYMMETRY_PLANE
#define DEBUG_PRINT_SCULPT(msg) std::cout << "[SCULPT_DEBUG] " << msg << std::endl
#else
#define DEBUG_PRINT_SCULPT(msg)
#endif

namespace blender::draw::overlay {

/**
 * Display sculpt modes overlays.
 * Covers face sets and mask for meshes.
 * Draw curve cages (curve guides) for curve sculpting.
 */
class Sculpts : Overlay {

 public:
  Sculpts(SelectionType selection_type)
      : symmetry_contour_(selection_type, "SculptSymmetryContour")
  {
  }

 private:
  PassSimple sculpt_mask_ = {"SculptMaskAndFaceSet"};
  PassSimple::Sub *mesh_ps_ = nullptr;
  PassSimple::Sub *curves_ps_ = nullptr;

  PassSimple sculpt_curve_cage_ = {"SculptCage"};
  PassSimple sculpt_symmetry_plane_ = {"SculptSymmetryPlane"};
  SymmetryContourOverlay symmetry_contour_;

  bool show_curves_cage_ = false;
  bool show_face_set_ = false;
  bool show_mask_ = false;
  bool show_symmetry_plane_ = false;
  bool show_symmetry_contour_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    show_curves_cage_ = state.show_sculpt_curves_cage();
    show_face_set_ = state.show_sculpt_face_sets();
    show_mask_ = state.show_sculpt_mask();
    show_symmetry_plane_ = state.overlay.show_sculpt_symmetry_plane;
    show_symmetry_contour_ = state.overlay.show_sculpt_symmetry_contour;

    // DEBUG: Log symmetry plane state
    DEBUG_PRINT_SCULPT("show_symmetry_plane_ = " << int(show_symmetry_plane_));
    DEBUG_PRINT_SCULPT("state.overlay.show_sculpt_symmetry_plane = " << int(state.overlay.show_sculpt_symmetry_plane));
    DEBUG_PRINT_SCULPT("show_symmetry_contour_ = " << int(show_symmetry_contour_));
    DEBUG_PRINT_SCULPT("state.overlay.show_sculpt_symmetry_contour = "
                       << int(state.overlay.show_sculpt_symmetry_contour));
    DEBUG_PRINT_SCULPT("state.object_mode = " << state.object_mode);

    // ИСПРАВЛЕНИЕ: Не проверяем глобальный object_mode, так как он может не отражать 
    // режим конкретных объектов. Активируем overlay если есть что показывать в 3D виде.
    enabled_ = state.is_space_v3d() && !state.is_wire() && !res.is_selection() &&
               !state.is_depth_only_drawing &&
               (show_curves_cage_ || show_face_set_ || show_mask_ || show_symmetry_plane_ ||
                show_symmetry_contour_);

    // DEBUG: Log enabled state
    DEBUG_PRINT_SCULPT("Sculpts enabled_ = " << enabled_);

    if (!enabled_) {
      /* Not used. But release the data. */
      DEBUG_PRINT_SCULPT("Early return: Sculpts not enabled");
      sculpt_mask_.init();
      sculpt_curve_cage_.init();
      sculpt_symmetry_plane_.init();
      return;
    }

    float curve_cage_opacity = show_curves_cage_ ? state.overlay.sculpt_curves_cage_opacity : 0.0f;
    float face_set_opacity = show_face_set_ ? state.overlay.sculpt_mode_face_sets_opacity : 0.0f;
    float mask_opacity = show_mask_ ? state.overlay.sculpt_mode_mask_opacity : 0.0f;
    float symmetry_plane_opacity = show_symmetry_plane_ ? state.overlay.sculpt_symmetry_plane_opacity : 0.0f;

    // DEBUG: Log opacity values
    DEBUG_PRINT_SCULPT("symmetry_plane_opacity = " << symmetry_plane_opacity);
    DEBUG_PRINT_SCULPT("state.overlay.sculpt_symmetry_plane_opacity = " << state.overlay.sculpt_symmetry_plane_opacity);

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
      auto &pass = sculpt_symmetry_plane_;
      pass.init();
      pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA_PREMUL,
                     state.clipping_plane_count);
      pass.shader_set(res.shaders->sculpt_symmetry_plane.get());
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      pass.push_constant("opacity", symmetry_plane_opacity);
    }

    symmetry_contour_.begin_sync(res, state, state.overlay.show_sculpt_symmetry_contour);
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
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
      default:
        break;
    }

    /* Draw symmetry plane for sculpt mode - проверяем режим конкретного объекта */
    if (show_symmetry_plane_ && ob_ref.object->mode == OB_MODE_SCULPT) {
      symmetry_plane_sync(manager, ob_ref, state, res);
    }

    /* Update contours for sculpt mode - проверяем режим конкретного объекта */
    DEBUG_PRINT_SCULPT("object_sync: show_symmetry_contour_=" << int(show_symmetry_contour_)
                       << " object_mode=" << int(ob_ref.object->mode));
    if (show_symmetry_contour_ && ob_ref.object->mode == OB_MODE_SCULPT) {
      const Sculpt *sd = state.scene->toolsettings->sculpt;
      if (sd) {
        symmetry_contour_.object_sync(ob_ref.object, sd->paint.symmetry_flags, state);
      }
    }
  }

  void end_sync(Resources &res, const State &state) final
  {
    (void)res; // Suppress unused parameter warning
    
    if (!enabled_) {
      return;
    }

    symmetry_contour_.end_sync();
  }

  void curves_sync(Manager &manager, const ObjectRef &ob_ref, const State &state)
  {
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

    /* Рисуем контур симметрии в line framebuffer, чтобы заполнить line_tx для post-AA. */
    symmetry_contour_.draw_line(framebuffer, manager, view);
  }

  virtual void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view, const State &state) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(sculpt_mask_, view);
    
    /* Draw symmetry plane with transparency */
    if (show_symmetry_plane_) {
      manager.submit(sculpt_symmetry_plane_, view);
    }
  }

 private:
  bool everything_selected(const blender::Curves &curves_id)
  {
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
        ".selection", bke::AttrDomain::Point, true);
    return selection.is_single() && selection.get_internal_single();
  }

  void symmetry_plane_sync(Manager &manager, const ObjectRef &ob_ref, const State &state, Resources &res)
  {
    DEBUG_PRINT_SCULPT("symmetry_plane_sync called");

    const SculptSession *sculpt_session = ob_ref.object->runtime->sculpt_session;
    if (!sculpt_session) {
      DEBUG_PRINT_SCULPT("No sculpt session found");
      return;
    }

    /* Get sculpt symmetry settings */
    const Sculpt *sd = state.scene->toolsettings->sculpt;
    if (!sd) {
      DEBUG_PRINT_SCULPT("No sculpt data found");
      return;
    }

    /* Create symmetry plane geometry based on sculpt symmetry flags */
    const int symmetry_flags = sd->paint.symmetry_flags;
    DEBUG_PRINT_SCULPT("symmetry_flags = " << symmetry_flags);
    
    if (symmetry_flags & PAINT_SYMM_X) {
      DEBUG_PRINT_SCULPT("Creating X symmetry plane");
      create_symmetry_plane_geometry(manager, ob_ref, 0, res); /* X plane */
    }
    if (symmetry_flags & PAINT_SYMM_Y) {
      DEBUG_PRINT_SCULPT("Creating Y symmetry plane");
      create_symmetry_plane_geometry(manager, ob_ref, 1, res); /* Y plane */
    }
    if (symmetry_flags & PAINT_SYMM_Z) {
      DEBUG_PRINT_SCULPT("Creating Z symmetry plane");
      create_symmetry_plane_geometry(manager, ob_ref, 2, res); /* Z plane */
    }
  }

  void create_symmetry_plane_geometry(Manager &manager, const ObjectRef &ob_ref, int axis, Resources &res)
  {
    ResourceHandleRange handle = manager.unique_handle(ob_ref);
    
    /* Use existing quad_solid geometry for the symmetry plane */
    sculpt_symmetry_plane_.draw(res.shapes.quad_solid.get(), handle);
  }

  gpu::Batch *create_plane_batch(int axis)
  {
    /* This method is no longer needed as we use the existing quad_solid */
    return nullptr;
  }
};

}  // namespace blender::draw::overlay
