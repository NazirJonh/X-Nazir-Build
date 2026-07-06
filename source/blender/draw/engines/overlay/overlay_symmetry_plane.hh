/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 *
 * Draws the translucent X/Y/Z symmetry planes. Used by sculpt mode (meshes and curves) and curves
 * edit mode, where a surface contour is not meaningful. The planes pass through the object origin
 * and are oriented per axis in the shader; their size follows the object's local bounds.
 */

#pragma once

#include <optional>

#include "BKE_object.hh"
#include "BKE_paint.hh"

#include "BLI_bounds_types.hh"
#include "BLI_math_vector.hh"

#include "overlay_private.hh"

namespace blender::draw::overlay {

class SymmetryPlaneOverlay {
 private:
  bool show_ = false;
  float opacity_ = 0.2f;
  PassSimple pass_;

 public:
  SymmetryPlaneOverlay(const char *pass_name) : pass_(pass_name) {}

  void begin_sync(Resources &res, const State &state, const bool show, const float opacity)
  {
    show_ = show;
    opacity_ = opacity;

    pass_.init();
    if (!show_) {
      return;
    }
    pass_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                    state.clipping_plane_count);
    pass_.shader_set(res.shaders->sculpt_symmetry_plane.get());
    pass_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    pass_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
  }

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   const int symmetry_flags,
                   Resources &res)
  {
    if (!show_ || symmetry_flags == 0) {
      return;
    }

    /* Size the plane to comfortably cover the object's local bounds. */
    float plane_size = 1.0f;
    if (const std::optional<blender::Bounds<float3>> bounds = BKE_object_boundbox_get(
            ob_ref.object))
    {
      const float3 extent = bounds->max - bounds->min;
      plane_size = math::max(math::reduce_max(extent) * 0.6f, 1e-3f);
    }

    const ResourceHandleRange handle = manager.unique_handle(ob_ref);

    auto draw_axis = [&](const int axis, const float3 &rgb) {
      pass_.push_constant("plane_axis", axis);
      pass_.push_constant("plane_size", plane_size);
      pass_.push_constant("plane_color", float4(rgb, opacity_));
      pass_.draw(res.shapes.quad_solid.get(), handle);
    };

    if (symmetry_flags & PAINT_SYMM_X) {
      draw_axis(0, float3(1.0f, 0.25f, 0.25f));
    }
    if (symmetry_flags & PAINT_SYMM_Y) {
      draw_axis(1, float3(0.25f, 1.0f, 0.25f));
    }
    if (symmetry_flags & PAINT_SYMM_Z) {
      draw_axis(2, float3(0.25f, 0.4f, 1.0f));
    }
  }

  void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view)
  {
    if (!show_) {
      return;
    }
    GPU_framebuffer_bind(framebuffer);
    manager.submit(pass_, view);
  }
};

}  // namespace blender::draw::overlay
