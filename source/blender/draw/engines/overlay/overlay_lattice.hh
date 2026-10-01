/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "draw_cache.hh"
#include "draw_cache_impl.hh"
#include "draw_common_c.hh"
#include "overlay_base.hh"
#include "overlay_symmetry_contour.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_lattice_types.h"

namespace blender::draw::overlay {

/**
 * Draw lattice objects in object and edit mode.
 */
class Lattices : Overlay {
 private:
  PassMain ps_ = {"Lattice"};

  PassMain::Sub *lattice_ps_;
  PassMain::Sub *edit_lattice_wire_ps_;
  PassMain::Sub *edit_lattice_point_ps_;

  bool show_symmetry_contour_ = false;
  SymmetryContourOverlay symmetry_contour_;

 public:
  Lattices(bool in_front)
      : symmetry_contour_(SelectionType::DISABLED, "EditLatticeSymmetryContour", in_front)
  {
  }

  void begin_sync(Resources &res, const State &state) final
  {
    enabled_ = state.is_space_v3d();
    if (!enabled_) {
      show_symmetry_contour_ = false;
      symmetry_contour_.begin_sync(res, state, false);
      return;
    }

    show_symmetry_contour_ = state.ctx_mode == CTX_MODE_EDIT_LATTICE &&
                             state.show_lattice_symmetry_contour();
    symmetry_contour_.begin_sync(res, state, show_symmetry_contour_);

    auto create_sub_pass = [&](const char *name, gpu::Shader *shader, bool add_weight_tex) {
      PassMain::Sub &sub_pass = ps_.sub(name);
      sub_pass.shader_set(shader);
      if (add_weight_tex) {
        sub_pass.bind_texture("weight_tx", &res.weight_ramp_tx);
      }
      return &sub_pass;
    };

    ps_.init();
    ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL,
                  state.clipping_plane_count);
    ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    res.select_bind(ps_);
    edit_lattice_wire_ps_ = create_sub_pass(
        "edit_lattice_wire", res.shaders->lattice_wire.get(), true);
    edit_lattice_point_ps_ = create_sub_pass(
        "edit_lattice_points", res.shaders->lattice_points.get(), false);
    lattice_ps_ = create_sub_pass("lattice", res.shaders->extra_wire_object.get(), false);
  }

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State &state) final
  {
    if (!enabled_) {
      return;
    }

    ResourceHandleRange res_handle = manager.unique_handle(ob_ref);
    {
      gpu::Batch *geom = DRW_cache_lattice_wire_get(ob_ref.object, true);
      edit_lattice_wire_ps_->draw(geom, res_handle, res.select_id(ob_ref).get());
    }
    {
      gpu::Batch *geom = DRW_cache_lattice_vert_overlay_get(ob_ref.object);
      edit_lattice_point_ps_->draw(geom, res_handle, res.select_id(ob_ref).get());
    }

    if (show_symmetry_contour_) {
      /* Read symmetry from the original object, not the evaluated copy, so the toggle always
       * takes effect regardless of when the evaluated copy catches up (see the same approach in
       * overlay_mesh.hh for the Edit Mesh overlay). */
      const Object *ob_orig = DEG_get_original(ob_ref.object);
      const Lattice &latt_orig = *id_cast<const Lattice *>(ob_orig->data);
      const int symmetry_flags = symmetry_flags_from_lattice_symmetry(latt_orig.symmetry);
      if (symmetry_flags != 0) {
        symmetry_contour_.object_sync(ob_ref.object, symmetry_flags, state);
      }
    }
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final
  {
    if (!enabled_) {
      return;
    }

    if (!state.show_extras() || (ob_ref.object->dt == OB_BOUNDBOX)) {
      return;
    }

    gpu::Batch *geom = DRW_cache_lattice_wire_get(ob_ref.object, false);
    if (geom) {
      const float4 &color = res.object_wire_color(ob_ref, state);
      float4x4 draw_mat(ob_ref.object->object_to_world().ptr());
      for (int i : IndexRange(3)) {
        draw_mat[i][3] = color[i];
      }
      draw_mat[3][3] = 0.0f /* No stipples. */;
      ResourceHandleRange res_handle = manager.resource_handle(
          ob_ref, &draw_mat, nullptr, nullptr);
      lattice_ps_->draw(geom, res_handle, res.select_id(ob_ref).get());
    }
  }

  void end_sync(Resources & /*res*/, const State & /*state*/) final
  {
    if (!enabled_) {
      return;
    }
    symmetry_contour_.end_sync();
  }

  void pre_draw(Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    manager.generate_commands(ps_, view);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit_only(ps_, view);
  }

  /**
   * The symmetry contour is drawn into the depth-less line frame-buffer so it feeds post-AA, and
   * is therefore not depth tested against the passes that follow. It is kept out of #draw_line and
   * submitted by #Instance::draw_v3d after the grid and the other line overlays, which share the
   * same `line_tx` and would otherwise draw over it (same pattern as #Sculpts::draw_symmetry_contour).
   */
  void draw_symmetry_contour(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    if (!enabled_) {
      return;
    }
    symmetry_contour_.draw_line(framebuffer, manager, view);
  }
};
}  // namespace blender::draw::overlay
