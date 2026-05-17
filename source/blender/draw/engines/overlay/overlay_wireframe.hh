/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <memory>

#include "BKE_modifier.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"
#include "BLI_bounds_types.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"
#include "DNA_modifier_types.h"
#include "DNA_view3d_types.h"
#include "DNA_volume_types.h"

#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"
#include "draw_common.hh"
#include "draw_sculpt.hh"

#include "overlay_base.hh"
#include "overlay_mesh.hh"

namespace blender::draw::overlay {

/**
 * Draw wireframe of objects.
 *
 * The object wireframe can be drawn because of:
 * - display option (Object > Viewport Display > Wireframe)
 * - overlay option (Viewport Overlays > Geometry > Wireframe)
 * - display as (Object > Viewport Display > Wire)
 * - wireframe shading mode
 */
class Wireframe : Overlay {
 private:
  PassMain wireframe_ps_ = {"Wireframe"};
  struct ColoringPass {
    PassMain::Sub *curves_ps_ = nullptr;
    PassMain::Sub *mesh_ps_ = nullptr;
    /* Variant for meshes that force drawing all edges. */
    PassMain::Sub *mesh_all_edges_ps_ = nullptr;
    /* Adaptive wireframe for Multires objects. */
    PassMain::Sub *mesh_multires_ps_ = nullptr;
    PassMain::Sub *points_ps_ = nullptr;
    PassMain::Sub *pointcloud_ps_ = nullptr;
  } colored, non_colored;

  /* Copy of the depth buffer to be able to read it during wireframe rendering. */
  TextureFromPool tmp_depth_tx_ = {"tmp_depth_tx"};
  bool do_depth_copy_workaround_ = false;

  /* Force display of wireframe on surface objects, regardless of the object display settings. */
  bool show_wire_ = false;

  /* Per-object Multires wireframe data. One UBO per object — sharing a single
   * UBO across all multires objects in the scene caused later-processed
   * objects to overwrite earlier ones, so every draw command (which is
   * deferred) ended up reading the last object's parameters instead of its
   * own. The pool is rebuilt every frame in `begin_sync`. `unique_ptr` keeps
   * the underlying `UniformBuffer` address stable even when the Vector
   * reallocates, so previously bound UBO pointers stay valid. */
  using MultiresWireUBO = draw::UniformBuffer<OVERLAY_MultiresWireData>;
  Vector<std::unique_ptr<MultiresWireUBO>> multires_wire_ubo_pool_;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = state.is_space_v3d() && (state.is_wireframe_mode || !state.hide_overlays);
    if (!enabled_) {
      return;
    }

    /* Reset per-frame state. Releasing the pool here ensures that previously
     * recorded `bind_ubo` pointers from the prior frame are not reused — the
     * pass commands are also rebuilt every frame, so the lifetime matches. */
    multires_wire_ubo_pool_.clear();

    show_wire_ = state.is_wireframe_mode || state.show_wireframes();

    const bool is_selection = res.is_selection();
    const bool do_smooth_lines = (U.gpu_flag & USER_GPU_FLAG_OVERLAY_SMOOTH_WIRE) != 0;
    const bool is_transform = (G.moving & G_TRANSFORM_OBJ) != 0;
    const float wire_threshold = wire_discard_threshold_get(state.overlay.wireframe_threshold);

    gpu::Texture **depth_tex = (state.xray_enabled) ? &res.depth_tx : &tmp_depth_tx_;
    if (is_selection) {
      depth_tex = &res.dummy_depth_tx;
    }

    /* Note: Depth buffer has different format when doing selection. Avoid copy in this case. */
    do_depth_copy_workaround_ = !is_selection && (depth_tex == &tmp_depth_tx_);

    {
      auto &pass = wireframe_ps_;
      pass.init();
      pass.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
      pass.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
      pass.state_set(DRW_STATE_FIRST_VERTEX_CONVENTION | DRW_STATE_WRITE_COLOR |
                         DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                     state.clipping_plane_count);
      res.select_bind(pass);

      auto shader_pass =
          [&](gpu::Shader *shader,
              const char *name,
              bool use_coloring,
              float wire_threshold,
              bool use_multires = false) {
            auto &sub = pass.sub(name);
            if (res.shaders->wireframe_mesh.get() == shader) {
              sub.specialize_constant(shader, "use_custom_depth_bias", do_smooth_lines);
              sub.specialize_constant(shader, "use_multires_wireframe", use_multires);
            }
            sub.shader_set(shader);
            sub.bind_texture("depth_tx", depth_tex);
            sub.push_constant("wire_opacity", state.overlay.wireframe_opacity);
            sub.push_constant("is_transform", is_transform);
            sub.push_constant("color_type", state.v3d->shading.wire_color_type);
            sub.push_constant("use_coloring", use_coloring);
            sub.push_constant("wire_step_param", wire_threshold);
            sub.push_constant("ndc_offset_factor", &state.ndc_offset_factor);
            sub.push_constant("is_hair", false);
            return &sub;
          };

      auto coloring_pass = [&](ColoringPass &ps, bool use_color) {
        overlay::ShaderModule &sh = *res.shaders;
        ps.mesh_ps_ = shader_pass(sh.wireframe_mesh.get(), "Mesh", use_color, wire_threshold);
        ps.mesh_all_edges_ps_ = shader_pass(sh.wireframe_mesh.get(), "Wire", use_color, 1.0f);
        ps.mesh_multires_ps_ = shader_pass(
            sh.wireframe_mesh.get(), "MeshMultires", use_color, wire_threshold, true);
        ps.points_ps_ = shader_pass(sh.wireframe_points.get(), "Points", use_color, 1.0f);
        ps.pointcloud_ps_ = shader_pass(
            sh.wireframe_points_with_radius.get(), "PtCloud", use_color, 1.0f);
        ps.curves_ps_ = shader_pass(sh.wireframe_curve.get(), "Curve", use_color, 1.0f);
      };

      coloring_pass(non_colored, false);
      coloring_pass(colored, true);
    }
  }

  void object_sync_ex(Manager &manager,
                      const ObjectRef &ob_ref,
                      Resources &res,
                      const State &state,
                      const bool in_edit_paint_mode,
                      const bool in_edit_mode)
  {
    if (!enabled_) {
      return;
    }

    if (ob_ref.object->dt < OB_WIRE) {
      return;
    }

    const bool all_edges = (ob_ref.object->dtx & OB_DRAW_ALL_EDGES) != 0;
    const bool show_surface_wire = show_wire_ || (ob_ref.object->dtx & OB_DRAWWIRE) ||
                                   (ob_ref.object->dt == OB_WIRE);

    ColoringPass &coloring = in_edit_paint_mode ? non_colored : colored;
    switch (ob_ref.object->type) {
      case OB_CURVES_LEGACY: {
        gpu::Batch *geom = DRW_cache_curve_edge_wire_get(ob_ref.object);
        coloring.curves_ps_->draw(
            geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        break;
      }
      case OB_FONT: {
        gpu::Batch *geom = DRW_cache_text_edge_wire_get(ob_ref.object);
        coloring.curves_ps_->draw(
            geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        break;
      }
      case OB_SURF: {
        gpu::Batch *geom = DRW_cache_surf_edge_wire_get(ob_ref.object);
        coloring.curves_ps_->draw(
            geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        break;
      }
      case OB_CURVES:
        /* TODO(fclem): Not yet implemented. */
        break;
      case OB_GREASE_PENCIL: {
        if (show_surface_wire) {
          gpu::Batch *geom = DRW_cache_grease_pencil_face_wireframe_get(state.scene,
                                                                        ob_ref.object);
          coloring.curves_ps_->draw(
              geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        }
        break;
      }
      case OB_MESH: {
        /* Force display in edit mode when overlay is off in wireframe mode (see #78484). */
        const bool wireframe_no_overlay = state.hide_overlays && state.is_wireframe_mode;

        /* In some cases the edit mode wireframe overlay is already drawn for the same edges.
         * We want to avoid this redundant work and avoid Z-fighting, but detecting this case is
         * relatively complicated. Whether edit mode draws edges on the evaluated mesh depends on
         * whether there is a separate cage and whether there is a valid mapping between the
         * evaluated and original edit mesh. */
        const bool edit_wires_overlap_all = mesh_edit_wires_overlap(ob_ref, in_edit_mode);

        const bool bypass_mode_check = wireframe_no_overlay || !edit_wires_overlap_all;

        if (show_surface_wire) {
          /* Check for Multires modifier to enable adaptive wireframe. */
          ModifierData *mmd_md = BKE_modifiers_findby_type(ob_ref.object,
                                                           eModifierType_Multires);
          const MultiresModifierData *mmd = reinterpret_cast<const MultiresModifierData *>(
              mmd_md);
          const bool is_pbvh = BKE_sculptsession_use_pbvh_draw(ob_ref.object, state.rv3d);
          const bool use_multires_wire = mmd != nullptr &&
                                        BKE_modifier_is_enabled(
                                            state.scene, mmd_md, eModifierMode_Realtime);

          /* Reused below to estimate the visible subdivision level for the PBVH draw. */
          float object_diameter = 1.0f;
          /* Per-object UBO pointer (null for non-multires objects). Bound to the
           * draw pass instead of a shared class-level UBO so each object renders
           * with its own parameters even though the draw commands are deferred. */
          MultiresWireUBO *object_ubo = nullptr;
          /* Effective max subdivision level present in the data being drawn.
           * In Sculpt Mode the PBVH is evaluated at `mmd->sculptlvl`, so the SubdivCCG
           * (and therefore the `subdiv_level` VBO) only contains values up to that level,
           * even when `mmd->totlvl` is higher. Using `totlvl` here would make the shader
           * expect levels that physically do not exist in the buffer. */
          /* For the non-PBVH (Object Mode) path the evaluated mesh is tagged with
           * `BKE_multires_tag_edge_levels(result, mmd->lvl)`, so the VBO data is bounded
           * by the viewport level, not `totlvl`. Using `totlvl` would make the shader
           * normalise on the wrong denominator when `lvl < totlvl`. The PBVH path
           * overrides this below with the actual CCG level. */
          int effective_max_level = mmd ? mmd->lvl : 0;
          /* Minimum subdiv_level stored in the VBO (non-zero in Sculpt Mode when the PBVH
           * grid depth is smaller than the total SubdivCCG level). Matches `level_offset` in
           * `fill_subdivision_levels_grids` in draw_pbvh.cc so the shader can normalise raw
           * VBO values to a 0-based range where 0 = coarsest visible edge. */
          int effective_min_level = 0;
          if (use_multires_wire) {
            if (is_pbvh) {
              const SculptSession *ss = ob_ref.object->runtime->sculpt_session;
              if (ss && ss->subdiv_ccg) {
                effective_max_level = ss->subdiv_ccg->level;
                const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss->subdiv_ccg);
                int grid_depth = 0;
                while ((1 << grid_depth) < (key.grid_size - 1)) {
                  grid_depth++;
                }
                effective_min_level = std::max(0, effective_max_level - grid_depth);
              }
            }

            /* Use the evaluated mesh bounds for the object diameter. `ob_ref.object` is the
             * evaluated object from the depsgraph, so `->data` is the evaluated mesh
             * (after Multires). Multires smoothing may slightly alter the bounds compared
             * to the original base mesh, but the difference is negligible for the adaptive
             * wireframe LOD computation. */
            const Mesh *base_mesh = id_cast<const Mesh *>(ob_ref.object->data);
            std::optional<blender::Bounds<float3>> bounds;

            if (base_mesh) {
              bounds = base_mesh->bounds_min_max();
            }
            else {
              /* Fallback to evaluated mesh if base mesh is not available. */
              const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_ref.object);
              bounds = mesh.bounds_min_max();
            }

            /* Convert local-space bounds diagonal to world space. The shader formula
             * `object_screen_px = diameter * pixels_per_world` treats `diameter` as a
             * world-space length, so without applying the object's scale two cubes
             * with identical local bounds but different object scales would produce
             * the same `wire_level` and thus the same subdivision density — leaving
             * the larger cube visibly under-subdivided on screen relative to the
             * smaller one. Transforming the diagonal as a direction folds in rotation
             * and (non-)uniform scaling without introducing translation error. */
            if (bounds) {
              const float3 local_diag = bounds->max - bounds->min;
              const float3 world_diag = math::transform_direction(
                  ob_ref.object->object_to_world(), local_diag);
              object_diameter = math::length(world_diag);
            }
            else {
              object_diameter = 1.0f;
            }

            /* Allocate a fresh UBO from the per-frame pool for this object. The
             * heap-allocated `UniformBuffer` keeps a stable address even if the
             * pool Vector reallocates, so the pointer captured by `bind_ubo`
             * below remains valid until the next `begin_sync`. */
            multires_wire_ubo_pool_.append(std::make_unique<MultiresWireUBO>());
            object_ubo = multires_wire_ubo_pool_.last().get();
            (*object_ubo).object_diameter = object_diameter;
            (*object_ubo).wire_level_max = float(effective_max_level); /* std140: stored as float */
            (*object_ubo).wire_level_min = float(effective_min_level);
            object_ubo->push_update();
          }

          if (is_pbvh) {
            ResourceHandleRange handle = manager.unique_handle(ob_ref);
            PassMain::Sub *mesh_pass = use_multires_wire ? coloring.mesh_multires_ps_ :
                                                           coloring.mesh_all_edges_ps_;
            if (use_multires_wire) {
              /* `&(*object_ubo)` invokes the `operator&` overload on
               * `draw::UniformBuffer<T>` which returns `gpu::UniformBuf**`,
               * matching the deferred-binding `bind_ubo` overload. Passing
               * `object_ubo` directly would not match any overload because the
               * conversion to `gpu::UniformBuf*` only fires on glvalues. */
              mesh_pass->bind_ubo("multires_wire_buf", &(*object_ubo));
            }
            for (SculptBatch &batch : sculpt_batches_get(
                     ob_ref.object, SCULPT_BATCH_WIREFRAME, {}))
            {
              mesh_pass->draw(batch.batch, handle);
            }
          }
          else if (!in_edit_mode || bypass_mode_check) {
            /* Only draw the wireframe in edit mode if object has edit cage.
             * Otherwise the wireframe will conflict with the edit cage drawing and produce
             * unpleasant aliasing. */
            gpu::Batch *geom = DRW_cache_mesh_face_wireframe_get(ob_ref.object);

            PassMain::Sub *mesh_pass;
            if (use_multires_wire) {
              if (all_edges) {
                /* Force-all-edges mode: use the standard pass (shader does not read the UBO). */
                mesh_pass = coloring.mesh_all_edges_ps_;
              }
              else {
                mesh_pass = coloring.mesh_multires_ps_;
                /* See note in the PBVH branch for why `&(*object_ubo)` is required here. */
                mesh_pass->bind_ubo("multires_wire_buf", &(*object_ubo));
              }
            }
            else {
              mesh_pass = all_edges ? coloring.mesh_all_edges_ps_ : coloring.mesh_ps_;
            }
            mesh_pass->draw(geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
        }

        /* Draw loose geometry. */
        if (!in_edit_paint_mode || bypass_mode_check) {
          const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_ref.object);
          gpu::Batch *geom;
          if ((mesh.edges_num == 0) && (mesh.verts_num > 0)) {
            geom = DRW_cache_mesh_all_verts_get(ob_ref.object);
            coloring.points_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
          else if ((geom = DRW_cache_mesh_loose_edges_get(ob_ref.object))) {
            coloring.mesh_all_edges_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
        }
        break;
      }
      case OB_POINTCLOUD: {
        if (show_surface_wire) {
          gpu::Batch *geom = DRW_pointcloud_batch_cache_get_dots(ob_ref.object);
          coloring.pointcloud_ps_->draw(
              geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
        }
        break;
      }
      case OB_VOLUME: {
        if (show_surface_wire) {
          gpu::Batch *geom = DRW_cache_volume_face_wireframe_get(ob_ref.object);
          if (geom == nullptr) {
            break;
          }
          if (DRW_object_get_data_for_drawing<Volume>(*ob_ref.object).display.wireframe_type ==
              VOLUME_WIREFRAME_POINTS)
          {
            coloring.points_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
          else {
            coloring.mesh_ps_->draw(
                geom, manager.unique_handle(ob_ref), res.select_id(ob_ref).get());
          }
        }
        break;
      }
      default:
        /* Would be good to have. */
        // BLI_assert_unreachable();
        break;
    }
  }

  void pre_draw(Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    manager.generate_commands(wireframe_ps_, view);
  }

  void copy_depth(TextureRef &depth_tx)
  {
    if (!enabled_ || !do_depth_copy_workaround_) {
      return;
    }

    eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
    int2 render_size = int2(depth_tx.size());
    tmp_depth_tx_.acquire_2d(render_size, gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, usage);

    /* WORKAROUND: Nasty framebuffer copy.
     * We should find a way to have nice wireframe without this. */
    GPU_texture_copy(tmp_depth_tx_, depth_tx);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit_only(wireframe_ps_, view);

    tmp_depth_tx_.release();
  }

 private:
  float wire_discard_threshold_get(float threshold)
  {
    /* Use `sqrt` since the value stored in the edge is a variation of the cosine, so its square
     * becomes more proportional with a variation of angle. */
    threshold = sqrt(abs(threshold));
    /* The maximum value (255 in the VBO) is used to force hide the edge. */
    return math::interpolate(0.0f, 1.0f - (1.0f / 255.0f), threshold);
  }

  static bool mesh_edit_wires_overlap(const ObjectRef &ob_ref, const bool in_edit_mode)
  {
    if (!in_edit_mode) {
      return false;
    }
    const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob_ref.object);
    const Mesh *orig_edit_mesh = BKE_object_get_pre_modified_mesh(ob_ref.object);
    const bool edit_mapping_valid = BKE_editmesh_eval_orig_map_available(mesh, orig_edit_mesh);
    if (!edit_mapping_valid) {
      /* The mesh edit mode overlay doesn't include wireframe for the evaluated mesh when it
       * doesn't correspond with the original edit mesh. So the main wireframe overlay should draw
       * wires for the evaluated mesh instead. */
      return false;
    }
    if (Meshes::mesh_has_edit_cage(ob_ref.object)) {
      /* If a cage exists, the edit overlay might not display every edge. */
      return false;
    }
    /* The edit mode overlay displays all of the edges of the evaluated mesh; drawing the edges
     * again would be redundant. */
    return true;
  }
};

}  // namespace blender::draw::overlay
