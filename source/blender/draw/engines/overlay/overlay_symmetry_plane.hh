/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 *
 * Draws the translucent X/Y/Z symmetry planes. Used by sculpt mode (meshes and curves) and curves
 * edit mode, where a surface contour is not meaningful. Ordinarily the planes pass through the
 * object origin along its own local axes, sized to its local bounds. A multi-object sculpt instead
 * mirrors every mesh across ONE shared plane, which is then drawn once, sized to reach every object
 * taking part (see #object_sync / #end_sync).
 */

#pragma once

#include <optional>

#include "BKE_object.hh"
#include "BKE_paint.hh"

#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "overlay_private.hh"
#include "overlay_symmetry_contour.hh"

namespace blender::draw::overlay {

class SymmetryPlaneOverlay {
 private:
  bool show_ = false;
  /**
   * Whether the pass is already empty, so it is only re-initialized on the on-to-off transition
   * instead of every frame the overlay stays off.
   */
  bool released_ = true;
  /**
   * Set for the overlay layer holding "In Front" objects. A shared plane is emitted by the layer
   * its claiming object belongs to, so that an "In Front" object does not paint over its own
   * plane (the two layers render into different frame-buffers).
   */
  bool in_front_ = false;
  float opacity_ = 0.2f;
  PassSimple pass_;

  /**
   * A plane shared by several objects is one plane in world space, so drawing it per object would
   * stack its translucency where the quads overlap. Instead the first object to sync it claims the
   * draw, every object grows the size needed to reach it, and #end_sync emits the single quad.
   * The state lives in #Resources rather than here because objects are split across the regular and
   * "In Front" overlay layers, each with its own #SymmetryPlaneOverlay.
   */
  using SharedPlane = Resources::SymmetryPlaneShared;

  /** Half-extent of `ob_ref`'s bounds along the plane, in the claiming object's local space. */
  static float shared_plane_size_for_object(const ObjectRef &ob_ref,
                                            const SharedPlane &shared,
                                            const int symmetry_flags)
  {
    const std::optional<blender::Bounds<float3>> bounds = BKE_object_boundbox_get(ob_ref.object);
    if (!bounds) {
      return 0.0f;
    }
    /* The object's own local space straight into the claiming object's local space. */
    const float4x4 to_claimer = shared.world_to_object * ob_ref.object->object_to_world();

    float size = 0.0f;
    for (int axis = 0; axis < 3; axis++) {
      const int axis_flag = (axis == 0) ? PAINT_SYMM_X : (axis == 1) ? PAINT_SYMM_Y : PAINT_SYMM_Z;
      if ((symmetry_flags & axis_flag) == 0) {
        continue;
      }
      const SymmetryPlanePlacement placement = symmetry_plane_placement(
          axis, &shared.symmetry_space_to_object);
      for (int corner = 0; corner < 8; corner++) {
        const float3 local(corner & 1 ? bounds->max.x : bounds->min.x,
                           corner & 2 ? bounds->max.y : bounds->min.y,
                           corner & 4 ? bounds->max.z : bounds->min.z);
        const float3 offset = math::transform_point(to_claimer, local) - placement.origin;
        size = math::max(size, math::abs(math::dot(offset, placement.tangent)));
        size = math::max(size, math::abs(math::dot(offset, placement.bitangent)));
      }
    }
    return size;
  }

  /** Record one quad per enabled axis, all placed in the local space `handle` transforms. */
  void draw_planes(const int symmetry_flags,
                   const float plane_size,
                   const float4x4 *symmetry_space_to_object,
                   const ResourceHandleRange &handle,
                   Resources &res)
  {
    auto draw_axis = [&](const int axis, const float3 &rgb) {
      const SymmetryPlanePlacement placement = symmetry_plane_placement(axis,
                                                                       symmetry_space_to_object);
      pass_.push_constant("plane_origin", placement.origin);
      pass_.push_constant("plane_size", plane_size);
      pass_.push_constant("plane_tangent", placement.tangent);
      pass_.push_constant("plane_bitangent", placement.bitangent);
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

 public:
  SymmetryPlaneOverlay(const char *pass_name, const bool in_front = false)
      : in_front_(in_front), pass_(pass_name)
  {
  }

  void begin_sync(Resources &res, const State &state, const bool show, const float opacity)
  {
    show_ = show;
    opacity_ = opacity;
    /* NOTE: the shared-plane state is NOT reset here - it is shared with the other overlay layer,
     * which may already have claimed it. #Resources::begin_sync clears it once per frame. */

    if (!show_) {
      /* Clear the recorded commands once, when the overlay is switched off. Re-initializing an
       * already empty pass every frame is pure overhead: #draw_on_render never submits it while
       * the overlay is off, so nothing can be recorded in the meantime. */
      if (!released_) {
        pass_.init();
        released_ = true;
      }
      return;
    }
    released_ = false;

    pass_.init();
    pass_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA,
                    state.clipping_plane_count);
    pass_.shader_set(res.shaders->sculpt_symmetry_plane.get());
    pass_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    pass_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
  }

  /**
   * `symmetry_space_to_object` maps the space the planes are defined in into `ob_ref`'s local
   * space. Null means `ob_ref`'s own local axes through its own origin, and the plane is drawn
   * right away. Non-null means the plane is shared with the other objects of a multi-object sculpt
   * stroke, and the actual draw is deferred to #end_sync so it is emitted only once (the matrix
   * is copied, so the caller may pass a temporary).
   */
  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   const int symmetry_flags,
                   Resources &res,
                   const float4x4 *symmetry_space_to_object = nullptr)
  {
    if (!show_ || symmetry_flags == 0) {
      return;
    }

    if (symmetry_space_to_object != nullptr) {
      SharedPlane &shared = res.symmetry_plane_shared;
      if (!shared.valid) {
        shared.valid = true;
        shared.in_front = in_front_;
        shared.handle = manager.unique_handle(ob_ref);
        shared.symmetry_flags = symmetry_flags;
        shared.world_to_object = ob_ref.object->world_to_object();
        shared.symmetry_space_to_object = *symmetry_space_to_object;
      }
      shared.plane_size = math::max(
          shared.plane_size, shared_plane_size_for_object(ob_ref, shared, symmetry_flags));
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

    draw_planes(symmetry_flags, plane_size, nullptr, manager.unique_handle(ob_ref), res);
  }

  /**
   * Emits the deferred shared plane, if any. Harmless when nothing was deferred. The quad is one
   * world-space plane, so exactly one layer records it: the one the claiming object belongs to,
   * since the two layers render into different frame-buffers and an "In Front" object would
   * otherwise paint over a plane drawn into the regular one.
   */
  void end_sync(Resources &res)
  {
    SharedPlane &shared = res.symmetry_plane_shared;
    if (!show_ || !shared.valid || shared.emitted || shared.in_front != in_front_) {
      return;
    }
    shared.emitted = true;
    draw_planes(shared.symmetry_flags,
                shared.plane_size,
                &shared.symmetry_space_to_object,
                shared.handle,
                res);
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
