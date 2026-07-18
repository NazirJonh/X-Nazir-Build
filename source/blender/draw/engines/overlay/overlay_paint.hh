/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "draw_cache.hh"
#include "draw_cache_impl.hh"

#include "overlay_base.hh"
#include "overlay_symmetry_contour.hh"

namespace blender::draw::overlay {

/**
 * Display paint mode overlays.
 * Covers weight paint, vertex paint and texture paint, plus the symmetry contour.
 */
class Paints : Overlay {

 private:
  /* Draw selection state on top of the mesh to communicate which areas can be painted on. */
  PassSimple paint_region_ps_ = {"paint_region_ps_"};
  PassSimple::Sub *paint_region_edge_ps_ = nullptr;
  PassSimple::Sub *paint_region_face_ps_ = nullptr;
  PassSimple::Sub *paint_region_vert_ps_ = nullptr;

  PassSimple weight_ps_ = {"weight_ps_"};
  /* Used when there's not a valid pre-pass (depth <=). */
  PassSimple::Sub *weight_opaque_ps_ = nullptr;
  /* Used when there's a valid pre-pass (depth ==). */
  PassSimple::Sub *weight_masked_transparency_ps_ = nullptr;
  /* Black and white mask overlayed on top of mesh to preview painting influence. */
  PassSimple paint_mask_ps_ = {"paint_mask_ps_"};

  bool show_wires_ = false;
  bool show_paint_mask_ = false;
  bool masked_transparency_support_ = false;
  bool show_symmetry_contour_ = false;
  /* Effective paint context mode, falling back to the object mode when the global context mode
   * doesn't match (e.g. during certain mode transitions). */
  int paint_ctx_mode_ = -1;
  SymmetryContourOverlay symmetry_contour_;

 public:
  Paints(bool in_front)
      : symmetry_contour_(SelectionType::DISABLED, "PaintSymmetryContour", in_front)
  {
  }

  void begin_sync(Resources &res, const State &state) final
  {
    paint_ctx_mode_ = state.ctx_mode;
    const int ob_mode = state.object_active ? state.object_active->mode : 0;
    const bool is_paint_ctx = ELEM(
        state.ctx_mode, CTX_MODE_PAINT_WEIGHT, CTX_MODE_PAINT_VERTEX, CTX_MODE_PAINT_TEXTURE);
    if (!is_paint_ctx &&
        (ob_mode & (OB_MODE_WEIGHT_PAINT | OB_MODE_VERTEX_PAINT | OB_MODE_TEXTURE_PAINT)))
    {
      if (ob_mode & OB_MODE_WEIGHT_PAINT) {
        paint_ctx_mode_ = CTX_MODE_PAINT_WEIGHT;
      }
      else if (ob_mode & OB_MODE_VERTEX_PAINT) {
        paint_ctx_mode_ = CTX_MODE_PAINT_VERTEX;
      }
      else if (ob_mode & OB_MODE_TEXTURE_PAINT) {
        paint_ctx_mode_ = CTX_MODE_PAINT_TEXTURE;
      }
    }

    enabled_ = state.is_space_v3d() && !res.is_selection() &&
               ELEM(paint_ctx_mode_,
                    CTX_MODE_PAINT_WEIGHT,
                    CTX_MODE_PAINT_VERTEX,
                    CTX_MODE_PAINT_TEXTURE);

    /* Init in any case to release the data. */
    paint_region_ps_.init();
    weight_ps_.init();
    paint_mask_ps_.init();

    if (!enabled_) {
      show_symmetry_contour_ = false;
      symmetry_contour_.begin_sync(res, state, false);
      return;
    }

    show_wires_ = state.overlay.paint_flag & V3D_OVERLAY_PAINT_WIRE;
    show_symmetry_contour_ = !state.is_wire() && !state.is_depth_only_drawing &&
                             ((paint_ctx_mode_ == CTX_MODE_PAINT_WEIGHT &&
                               state.overlay.show_weight_paint_symmetry_contour) ||
                              (paint_ctx_mode_ == CTX_MODE_PAINT_VERTEX &&
                               state.overlay.show_vertex_paint_symmetry_contour) ||
                              (paint_ctx_mode_ == CTX_MODE_PAINT_TEXTURE &&
                               state.overlay.show_texture_paint_symmetry_contour));
    symmetry_contour_.begin_sync(res, state, show_symmetry_contour_);

    {
      auto &pass = paint_region_ps_;
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      {
        auto &sub = pass.sub("Face");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                          DRW_STATE_BLEND_ALPHA,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->paint_region_face.get());
        sub.push_constant("ucolor", float4(1.0, 1.0, 1.0, 0.2));
        paint_region_face_ps_ = &sub;
      }
      {
        auto &sub = pass.sub("Edge");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
                          DRW_STATE_BLEND_ALPHA,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->paint_region_edge.get());
        paint_region_edge_ps_ = &sub;
      }
      {
        auto &sub = pass.sub("Vert");
        sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                      state.clipping_plane_count);
        sub.shader_set(res.shaders->paint_region_vert.get());
        paint_region_vert_ps_ = &sub;
      }
    }

    if (paint_ctx_mode_ == CTX_MODE_PAINT_WEIGHT) {
      /* Support masked transparency in Workbench.
       * EEVEE can't be supported since depth won't match. */
      const eDrawType shading_type = eDrawType(state.v3d->shading.type);
      masked_transparency_support_ = ((shading_type == OB_SOLID) ||
                                      (shading_type >= OB_SOLID &&
                                       BKE_scene_uses_blender_workbench(state.scene))) &&
                                     !state.xray_enabled;
      const bool shadeless = shading_type == OB_WIRE;
      const bool draw_contours = state.overlay.wpaint_flag & V3D_OVERLAY_WPAINT_CONTOURS;

      auto &pass = weight_ps_;
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      auto weight_subpass = [&](const char *name, DRWState drw_state) {
        auto &sub = pass.sub(name);
        sub.state_set(drw_state, state.clipping_plane_count);
        sub.shader_set(shadeless ? res.shaders->paint_weight.get() :
                                   res.shaders->paint_weight_fake_shading.get());
        sub.bind_texture("colorramp", &res.weight_ramp_tx);
        sub.push_constant("draw_contours", draw_contours);
        sub.push_constant("opacity", state.overlay.weight_paint_mode_opacity);
        if (!shadeless) {
          /* Arbitrary light to give a hint of the geometry behind the weights. */
          sub.push_constant("light_dir", math::normalize(float3(0.0f, 0.5f, 0.86602f)));
        }
        return &sub;
      };
      weight_opaque_ps_ = weight_subpass(
          "Opaque", DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_WRITE_DEPTH);
      weight_masked_transparency_ps_ = weight_subpass(
          "Masked Transparency",
          DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL | DRW_STATE_BLEND_ALPHA);
    }

    if (paint_ctx_mode_ == CTX_MODE_PAINT_TEXTURE) {
      const ImagePaintSettings &paint_settings = state.scene->toolsettings->imapaint;
      show_paint_mask_ = paint_settings.stencil &&
                         (paint_settings.flag & IMAGEPAINT_PROJECT_LAYER_STENCIL);

      if (show_paint_mask_) {
        const bool mask_premult = (paint_settings.stencil->alpha_mode == IMA_ALPHA_PREMUL);
        const bool mask_inverted = (paint_settings.flag & IMAGEPAINT_PROJECT_LAYER_STENCIL_INV);
        gpu::Texture *mask_texture = BKE_image_get_gpu_texture(paint_settings.stencil, nullptr);

        auto &pass = paint_mask_ps_;
        pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_EQUAL | DRW_STATE_BLEND_ALPHA,
                       state.clipping_plane_count);
        pass.shader_set(res.shaders->paint_texture.get());
        pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
        pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
        pass.bind_texture("mask_image", mask_texture);
        pass.push_constant("maskPremult", mask_premult);
        pass.push_constant("mask_invert_stencil", mask_inverted);
        pass.push_constant("mask_color", float3(paint_settings.stencil_col));
        pass.push_constant("opacity", state.overlay.texture_paint_mode_opacity);
      }
    }
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final
  {
    if (!enabled_ || ob_ref.object->type != OB_MESH) {
      /* Only meshes are supported. */
      return;
    }

    /* Read symmetry from the original object, not the evaluated copy: `use_mesh_mirror_*`
     * (see rna_object.cc) only sends a redraw notifier and never tags the mesh for depsgraph
     * re-evaluation, so the evaluated copy can go stale until something else forces a geometry
     * recalc (Vertex/Weight Paint strokes do; Texture Paint never touches geometry, so it never
     * refreshes and stays stuck with the symmetry flags from the last real recalc). Sculpt
     * overlay already does the same for the same reason, see overlay_sculpt.hh. */
    const Object *ob_orig = DEG_get_original(ob_ref.object);
    const Mesh &mesh_orig = DRW_object_get_data_for_drawing<Mesh>(*ob_orig);
    /* 3D paint symmetry follows the mesh symmetry flags (see paint_image_proj.cc). */
    const int symmetry_flags = symmetry_flags_from_mesh_symmetry(mesh_orig.symmetry);

    switch (paint_ctx_mode_) {
      case CTX_MODE_PAINT_WEIGHT:
        if (ob_ref.object->mode != OB_MODE_WEIGHT_PAINT) {
          return;
        }
        break;
      case CTX_MODE_PAINT_VERTEX:
        if (ob_ref.object->mode != OB_MODE_VERTEX_PAINT) {
          return;
        }
        break;
      case CTX_MODE_PAINT_TEXTURE:
        if (ob_ref.object->mode != OB_MODE_TEXTURE_PAINT) {
          return;
        }
        break;
      default:
        return;
    }

    if (show_symmetry_contour_) {
      symmetry_contour_.object_sync(ob_ref.object, symmetry_flags, state);
    }

    switch (paint_ctx_mode_) {
      case CTX_MODE_PAINT_WEIGHT: {
        gpu::Batch *geom = DRW_cache_mesh_surface_weights_get(ob_ref.object);
        if (masked_transparency_support_ && ob_ref.object->dt >= OB_SOLID) {
          weight_masked_transparency_ps_->draw(geom, manager.unique_handle(ob_ref));
        }
        else {
          weight_opaque_ps_->draw(geom, manager.unique_handle(ob_ref));
        }
        break;
      }
      case CTX_MODE_PAINT_VERTEX: {
        /* Drawing of vertex paint color is done by the render engine (i.e. workbench). */
        break;
      }
      case CTX_MODE_PAINT_TEXTURE: {
        if (show_paint_mask_) {
          gpu::Batch *geom = DRW_cache_mesh_surface_texpaint_single_get(ob_ref.object);
          paint_mask_ps_.draw(geom, manager.unique_handle(ob_ref));
        }
        break;
      }
      default:
        BLI_assert_unreachable();
        return;
    }

    /* Selection Display. */
    {
      /* NOTE(fclem): Why do we need original mesh here, only to get the flag? */
      const Mesh &mesh_orig = DRW_object_get_data_for_drawing<Mesh>(
          *DEG_get_original(ob_ref.object));
      const bool use_face_selection = (mesh_orig.editflag & ME_EDIT_PAINT_FACE_SEL);
      const bool use_vert_selection = (mesh_orig.editflag & ME_EDIT_PAINT_VERT_SEL);
      /* Texture paint mode only draws the face selection without wires or vertices as we don't
       * draw on the geometry data directly. */
      const bool in_texture_paint_mode = paint_ctx_mode_ == CTX_MODE_PAINT_TEXTURE;

      if ((use_face_selection || show_wires_) && !in_texture_paint_mode) {
        gpu::Batch *geom = DRW_cache_mesh_paint_overlay_edges_get(ob_ref.object);
        paint_region_edge_ps_->push_constant("use_select", use_face_selection);
        paint_region_edge_ps_->draw(geom, manager.unique_handle(ob_ref));
      }
      if (use_face_selection) {
        gpu::Batch *geom = DRW_cache_mesh_paint_overlay_surface_get(ob_ref.object);
        paint_region_face_ps_->draw(geom, manager.unique_handle(ob_ref));
      }
      if (use_vert_selection && !in_texture_paint_mode) {
        gpu::Batch *geom = DRW_cache_mesh_paint_overlay_verts_get(ob_ref.object);
        paint_region_vert_ps_->draw(geom, manager.unique_handle(ob_ref));
      }
    }
  }

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(weight_ps_, view);
    manager.submit(paint_mask_ps_, view);
    /* TODO(fclem): Draw this onto the line frame-buffer to get wide-line and anti-aliasing.
     * Just need to make sure the shaders output line data. */
    manager.submit(paint_region_ps_, view);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }
    symmetry_contour_.draw_line(framebuffer, manager, view);
  }

  void end_sync(Resources & /*res*/, const State & /*state*/) final
  {
    if (!enabled_) {
      return;
    }
    symmetry_contour_.end_sync();
  }
};

}  // namespace blender::draw::overlay
