/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

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
#include "overlay_symmetry_plane.hh"

namespace blender::draw::overlay {

/**
 * Display sculpt mode overlays.
 * Covers face sets and mask for meshes, curve cages for curve sculpting, and the symmetry
 * plane / contour overlays.
 */
class Sculpts : Overlay {

 public:
  Sculpts(SelectionType selection_type, bool in_front)
      : symmetry_contour_(selection_type, "SculptSymmetryContour", in_front),
        symmetry_plane_("SculptSymmetryPlane")
  {
  }

 private:
  PassSimple sculpt_mask_ = {"SculptMaskAndFaceSet"};
  PassSimple::Sub *mesh_ps_ = nullptr;
  PassSimple::Sub *curves_ps_ = nullptr;

  PassSimple sculpt_curve_cage_ = {"SculptCage"};
  SymmetryContourOverlay symmetry_contour_;
  SymmetryPlaneOverlay symmetry_plane_;

  bool show_curves_cage_ = false;
  bool show_face_set_ = false;
  bool show_mask_ = false;
  bool show_symmetry_plane_ = false;
  bool show_curves_symmetry_plane_ = false;
  bool show_symmetry_contour_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    show_curves_cage_ = state.show_sculpt_curves_cage();
    show_face_set_ = state.show_sculpt_face_sets();
    show_mask_ = state.show_sculpt_mask();
    show_symmetry_plane_ = state.overlay.show_sculpt_symmetry_plane;
    show_curves_symmetry_plane_ = state.overlay.show_curves_symmetry_plane;
    show_symmetry_contour_ = state.overlay.show_sculpt_symmetry_contour;

    enabled_ = state.is_space_v3d() && !state.is_wire() && !res.is_selection() &&
               !state.is_depth_only_drawing &&
               (show_curves_cage_ || show_face_set_ || show_mask_ || show_symmetry_plane_ ||
                show_curves_symmetry_plane_ || show_symmetry_contour_);

    if (!enabled_) {
      /* Not used, but release the data. */
      sculpt_mask_.init();
      sculpt_curve_cage_.init();
      symmetry_contour_.begin_sync(res, state, false);
      symmetry_plane_.begin_sync(res, state, false, 0.0f);
      return;
    }

    const float curve_cage_opacity = show_curves_cage_ ? state.overlay.sculpt_curves_cage_opacity :
                                                         0.0f;
    const float face_set_opacity = show_face_set_ ? state.overlay.sculpt_mode_face_sets_opacity :
                                                    0.0f;
    const float mask_opacity = show_mask_ ? state.overlay.sculpt_mode_mask_opacity : 0.0f;

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

    symmetry_contour_.begin_sync(res, state, show_symmetry_contour_);
    symmetry_plane_.begin_sync(res,
                               state,
                               show_symmetry_plane_ || show_curves_symmetry_plane_,
                               state.overlay.sculpt_symmetry_plane_opacity);
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
      case OB_MESH: {
        mesh_sync(manager, ob_ref, state);
        if (ob_ref.object->mode == OB_MODE_SCULPT &&
            (show_symmetry_contour_ || show_symmetry_plane_))
        {
          /* The contour/plane must track the same axis the brush actually mirrors strokes
           * across, which is #Mesh.symmetry (Object Data Properties > Symmetry) -
           * #do_symmetrical_brush_actions never reads the Sculpt tool settings' own
           * symmetry_flags to choose mirrored dabs, so reading it here would show an overlay
           * that disagrees with the real stroke mirroring. Read from the original object, not
           * the evaluated copy: like `use_mirror_x` toggling, a #Mesh.symmetry change only sends
           * a redraw notifier and never tags the mesh for depsgraph re-evaluation (see the
           * identical pattern in #edit_object_sync in overlay_mesh.hh). */
          const Object *ob_orig = DEG_get_original(ob_ref.object);
          const Mesh &mesh_orig = DRW_object_get_data_for_drawing<Mesh>(*ob_orig);
          const int symmetry_flags = symmetry_flags_from_mesh_symmetry(mesh_orig.symmetry);
          if (show_symmetry_contour_) {
            symmetry_contour_.object_sync(ob_ref.object, symmetry_flags, state);
          }
          if (show_symmetry_plane_) {
            symmetry_plane_.object_sync(manager, ob_ref, symmetry_flags, res);
          }
        }
        break;
      }
      case OB_CURVES: {
        curves_sync(manager, ob_ref, state);
        if (show_curves_symmetry_plane_) {
          const blender::Curves &curves_id = DRW_object_get_data_for_drawing<blender::Curves>(
              *ob_ref.object);
          symmetry_plane_.object_sync(
              manager, ob_ref, symmetry_flags_from_curves_symmetry(curves_id.symmetry), res);
        }
        break;
      }
      default:
        break;
    }
  }

  void end_sync(Resources & /*res*/, const State & /*state*/) final
  {
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
  }

  /**
   * The symmetry contour is drawn into the depth-less line frame-buffer so it feeds post-AA, and
   * is therefore not depth tested against the passes that follow. It is kept out of #draw_line and
   * submitted by #Instance::draw_v3d after the grid and the mesh line overlays, which share the
   * same `line_tx` and would otherwise draw over it.
   */
  void draw_symmetry_contour(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    if (!enabled_) {
      return;
    }
    symmetry_contour_.draw_line(framebuffer, manager, view);
  }

  virtual void draw_on_render(gpu::FrameBuffer *framebuffer,
                              Manager &manager,
                              View &view,
                              const State & /*state*/) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(sculpt_mask_, view);

    /* Translucent symmetry plane, blended over the rendered surface. */
    symmetry_plane_.draw_on_render(framebuffer, manager, view);
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
