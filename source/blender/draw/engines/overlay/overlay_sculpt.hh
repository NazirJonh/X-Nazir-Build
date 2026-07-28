/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include <optional>

#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "DEG_depsgraph_query.hh"

#include "BLI_math_matrix.hh"
#include "BLI_vector.hh"

#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

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
        symmetry_plane_("SculptSymmetryPlane", in_front)
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
  bool show_layer_mask_ = false;
  bool show_layer_preview_ = false;
  bool show_symmetry_plane_ = false;
  bool show_curves_symmetry_plane_ = false;
  bool show_symmetry_contour_ = false;

  /**
   * More than one object is in Sculpt Mode and strokes act on all of them, so they all mirror
   * across the single shared plane resolved by #shared_symmetry_space_to_object rather than each
   * across its own local axes. Mirrors `sculpt_mode_objects().size() > 1` in
   * editors/sculpt_paint/mesh, which is what the stroke itself gates its shared symmetry on.
   */
  bool multi_object_sculpt_ = false;
  /**
   * The object supplying the shared symmetry plane and axis flags, i.e. the stroke's
   * `mode_objects[0]`. Not simply #State.object_active: the view-layer iterator skips the active
   * base when it is hidden or otherwise not in the mode, and the stroke then references a
   * different mesh than the overlay would.
   */
  const Object *symmetry_reference_ob_ = nullptr;

  /** Resolved once in #begin_sync so #object_sync does not re-query the view layer per object. */
  void update_multi_object_sculpt(const State &state)
  {
    multi_object_sculpt_ = false;
    symmetry_reference_ob_ = nullptr;
    if (!show_symmetry_contour_ && !show_symmetry_plane_) {
      return;
    }
    const Object *active = state.object_active;
    if (active == nullptr || active->mode != OB_MODE_SCULPT || state.scene == nullptr) {
      return;
    }
    const Sculpt *sculpt = state.scene->toolsettings->sculpt;
    if (sculpt == nullptr || sculpt->multi_object_edit_scope != SCULPT_MULTI_OBJECT_EDIT_ALL) {
      /* "Active Only" makes every stroke single-object, whatever else is in the mode. */
      return;
    }
    const DRWContext *ctx = DRW_context_get();
    if (ctx == nullptr || ctx->depsgraph == nullptr) {
      return;
    }
    /* Same parameters as #sculpt_mode_objects, so the overlay agrees with the stroke on both how
     * many objects are in the mode and which one comes first. In particular linked duplicates must
     * NOT be collapsed: the stroke counts them individually. */
    const ObjectsInModeParams params = {OB_MODE_SCULPT, /*no_dup_data*/ false, nullptr, nullptr};
    const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_mode_params(
        *DEG_get_bmain(ctx->depsgraph), state.scene, ctx->view_layer, state.v3d, &params);
    if (objects.size() > 1) {
      multi_object_sculpt_ = true;
      symmetry_reference_ob_ = objects[0];
    }
  }

  /**
   * Maps the space the symmetry planes live in into `ob`'s local space, or nullopt when the plane
   * is `ob`'s own local plane (single object in ACTIVE_OBJECT space). Note the difference from an
   * identity matrix, which still means "a non-local frame that happens to coincide with this
   * object's own axes" - the plane overlay draws a shared plane once for the whole mode, so it
   * must be able to tell the two apart.
   *
   * This reproduces `StrokeCache.symm_cur_from_ref` as built by
   * #propagate_shared_sampling_and_symmetry_state (editors/sculpt_paint/mesh/
   * sculpt_multi_object.cc): World and Cursor spaces engage for ANY object count (matching the
   * stroke's #shared_symmetry_active, which is true for a single object outside ACTIVE_OBJECT
   * space), while ACTIVE_OBJECT only shares a plane across a genuine multi-object session. It is
   * duplicated rather than shared because the draw module must not depend on editors/, and because
   * that value only exists inside a live #StrokeCache, whereas the overlay has to draw the plane
   * while merely hovering.
   */
  std::optional<float4x4> shared_symmetry_space_to_object(const Object *ob,
                                                          const State &state) const
  {
    if (state.scene == nullptr) {
      return std::nullopt;
    }
    const Sculpt *sculpt = state.scene->toolsettings->sculpt;
    if (sculpt == nullptr) {
      return std::nullopt;
    }
    switch (ePaintSymmetrySpace(sculpt->paint.symmetry_space)) {
      case PAINT_SYMM_SPACE_GLOBAL_WORLD:
        return ob->world_to_object();
      case PAINT_SYMM_SPACE_GLOBAL_CURSOR:
        /* `symm_cur_from_ref` = `ob->world_to_object() * S_inv`, and for the cursor frame
         * `S = invert(cursor_to_world)`, so `S_inv` is the cursor matrix itself. Using its full
         * transform (location AND orientation) makes the overlay follow a rotated 3D cursor,
         * exactly as the stroke does (see #symmetry_space_frame). */
        return ob->world_to_object() * state.scene->cursor.matrix<float4x4>();
      case PAINT_SYMM_SPACE_ACTIVE_OBJECT:
        /* The reference object's local axes. Only meaningful across more than one object; a single
         * object mirrors in its own local space, so the plane is drawn in local axes (nullopt). */
        if (!multi_object_sculpt_) {
          return std::nullopt;
        }
        if (ob == symmetry_reference_ob_) {
          return float4x4::identity();
        }
        return ob->world_to_object() * symmetry_reference_ob_->object_to_world();
    }
    BLI_assert_unreachable();
    return std::nullopt;
  }

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    show_curves_cage_ = state.show_sculpt_curves_cage();
    show_face_set_ = state.show_sculpt_face_sets();
    show_mask_ = state.show_sculpt_mask();
    show_layer_mask_ = state.show_sculpt_layer_mask();
    show_layer_preview_ = state.show_sculpt_layer_preview();
    show_symmetry_plane_ = state.show_sculpt_symmetry_plane();
    show_curves_symmetry_plane_ = state.show_curves_symmetry_plane();
    show_symmetry_contour_ = state.show_sculpt_symmetry_contour();

    /* Deliberately not gated on #State.object_mode. The symmetry plane and contour are drawn for
     * objects that are in a sculpt mode while the ACTIVE object is not, which the symmetry
     * overlays rely on and which such a gate would hide. The per-object work stays gated in
     * #object_sync / #mesh_sync, which return early without a sculpt session. */
    enabled_ = state.is_space_v3d() && !state.is_wire() && !res.is_selection() &&
               !state.is_depth_only_drawing &&
               (show_curves_cage_ || show_face_set_ || show_mask_ || show_layer_mask_ ||
                show_layer_preview_ || show_symmetry_plane_ || show_curves_symmetry_plane_ ||
                show_symmetry_contour_);

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
    /* Only the sculpt-mesh path knows how to source a layer mask (the PBVH attribute filler does);
     * the curves sub-pass keeps its own selection shader and never references the third member, so
     * the constant is always pushed and only the opacity gates it. The tint is the same value the
     * old #mask_tint used: it is the same indicator, relocated onto the area it should have
     * described all along. */
    const float layer_mask_opacity = show_layer_mask_ ?
                                         state.overlay.sculpt_mode_layer_mask_opacity :
                                         0.0f;
    const float3 layer_mask_tint = float3(0.12f, 0.45f, 0.95f);
    /* Amber, chosen for maximum separation from the layer mask's blue under the multiplicative
     * blend: the two overlays answer different questions (where the layer is attenuated, versus
     * where it holds displacement) and are meant to be readable at the same time. */
    const float layer_preview_opacity =
        show_layer_preview_ ? state.scene->toolsettings->sculpt->sculpt_layer_preview_opacity :
                              0.0f;
    const float layer_preview_threshold =
        show_layer_preview_ ? state.scene->toolsettings->sculpt->sculpt_layer_preview_threshold :
                              1.0f;
    const float3 layer_preview_tint = float3(0.95f, 0.55f, 0.15f);

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
        sub.push_constant("layer_mask_opacity", layer_mask_opacity);
        sub.push_constant("layer_mask_tint", layer_mask_tint);
        sub.push_constant("layer_preview_threshold", layer_preview_threshold);
        sub.push_constant("layer_preview_opacity", layer_preview_opacity);
        sub.push_constant("layer_preview_tint", layer_preview_tint);
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

    update_multi_object_sculpt(state);
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
           * that disagrees with the real stroke mirroring. In a multi-object session those flags
           * come from the ACTIVE object for every mesh (see #StrokeCache.symm_reference_object),
           * since the header X/Y/Z toggles only ever set them on the active mesh. Read from the
           * original object, not the evaluated copy: like `use_mirror_x` toggling, a
           * #Mesh.symmetry change only sends a redraw notifier and never tags the mesh for
           * depsgraph re-evaluation (see the identical pattern in #edit_object_sync in
           * overlay_mesh.hh). */
          const Object *symmetry_ob = multi_object_sculpt_ ? symmetry_reference_ob_ :
                                                             ob_ref.object;
          const Object *ob_orig = DEG_get_original(symmetry_ob);
          const Mesh &mesh_orig = DRW_object_get_data_for_drawing<Mesh>(*ob_orig);
          const int symmetry_flags = symmetry_flags_from_mesh_symmetry(mesh_orig.symmetry);
          const std::optional<float4x4> to_object = shared_symmetry_space_to_object(ob_ref.object,
                                                                                    state);
          const float4x4 *to_object_ptr = to_object ? &*to_object : nullptr;
          if (show_symmetry_contour_) {
            symmetry_contour_.object_sync(
                ob_ref.object, symmetry_flags, state, nullptr, to_object_ptr);
          }
          if (show_symmetry_plane_) {
            symmetry_plane_.object_sync(manager, ob_ref, symmetry_flags, res, to_object_ptr);
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

  void end_sync(Resources &res, const State & /*state*/) final
  {
    if (!enabled_) {
      return;
    }
    symmetry_contour_.end_sync();
    symmetry_plane_.end_sync(res);
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
    if (!show_face_set_ && !show_mask_ && !show_layer_mask_ && !show_layer_preview_) {
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

    /* The layer mask lives in DNA (#SculptLayerTreeNode::mask), not in the .sculpt_face_set /
     * .sculpt_mask attributes this switch checks. A mesh with layers but no ordinary mask would be
     * silently dropped by the early returns below, so the whole check is skipped when the layer
     * overlay is on. That includes the BMesh branch, which has no layers to show at all (dyntopo
     * carries none): its filler writes the neutral weight, which costs one pass over the buffer
     * and keeps the branch structure uniform. */
    if (!show_layer_mask_ && !show_layer_preview_) {
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
          if (subdiv_ccg.masks.is_empty() &&
              !base_mesh.attributes().contains(".sculpt_face_set")) {
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
    }

    const bool use_pbvh = BKE_sculptsession_use_pbvh_draw_for_display(ob_ref.object, state.rv3d);
    if (use_pbvh) {
      ResourceHandleRange handle = manager.unique_handle_for_sculpt(ob_ref);

      SculptBatchFeature sculpt_batch_features_ =
          (show_face_set_ ? SCULPT_BATCH_FACE_SET : SCULPT_BATCH_DEFAULT) |
          (show_mask_ ? SCULPT_BATCH_MASK : SCULPT_BATCH_DEFAULT) |
          (show_layer_mask_ ? SCULPT_BATCH_LAYER_MASK : SCULPT_BATCH_DEFAULT) |
          (show_layer_preview_ ? SCULPT_BATCH_LAYER_PREVIEW : SCULPT_BATCH_DEFAULT);

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

  void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view) final
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
