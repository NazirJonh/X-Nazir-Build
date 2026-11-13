/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Transform Selection in the 3D Viewport: drag interaction (move / rotate / scale handles).
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"

#include "ED_image.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_transform_view3d_intern.hh"
#include "paint_image_select_view3d.hh"
#include "../mesh/sculpt_intern.hh"

#include "BKE_mesh.hh"
#include "BKE_bvhutils.hh"
#include "BKE_paint_layers.hh"
#include "BKE_screen.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Drag interaction
 * \{ */

Transform3DSnapshot transform3d_snapshot(const ImageSelectTransform3DState &state)
{
  Transform3DSnapshot snap;
  snap.linear = state.uv_linear;
  snap.pivot = state.uv_pivot;
  snap.translation = state.uv_translation;
  /* The decal frame rides along; the UV fields of the snapshot are the synced affine, the decal
   * fields are the state it was synced from. */
  snap.decal_center_uv = state.decal.center_uv;
  snap.decal_roll = state.decal.roll;
  snap.decal_scale = state.decal.scale;
  return snap;
}

void transform3d_restore(ImageSelectTransform3DState &state,
                         const Transform3DSnapshot &snap)
{
  state.uv_linear = snap.linear;
  state.uv_pivot = snap.pivot;
  state.uv_translation = snap.translation;
  state.display_frame_valid = false;
  state.prev_surface_valid = false;
  if (state.mode == image_select_v3d::Transform3DContentMode::Decal) {
    /* The decal frame is authoritative in decal mode: put it back and let the next draw (the one
     * caller that always has a context) re-sync the affine and the preview rows. */
    state.decal.center_uv = snap.decal_center_uv;
    state.decal.roll = snap.decal_roll;
    state.decal.scale = snap.decal_scale;
    state.decal_frame_dirty = true;
  }
}

/**
 * Resolve the cursor onto the surface: the same view raycast the paint cursor and the anchor
 * creation use (the sculpt BVH, plus a face hint for the UV lookup), so crossing a face or a UV
 * seam costs one raycast rather than a full mesh search. Off the mesh the local plane's own
 * mapping is kept, so a drag that leaves the silhouette still moves the fragment.
 */
bool image_select_view3d_surface_point_at(bContext *C,
                                          Object &ob,
                                          const ImageSelectView3DAnchor &anchor,
                                          const ARegion &region,
                                          const int2 &mval,
                                          float2 &r_uv,
                                          float3 &r_co)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Scene *scene = CTX_data_scene(C);
  const Sculpt *sculpt = (scene != nullptr && scene->toolsettings != nullptr) ?
                             scene->toolsettings->sculpt :
                             nullptr;
  const Mesh *mesh = (ob.type == OB_MESH) ? id_cast<const Mesh *>(ob.data) : nullptr;
  if (mesh == nullptr || depsgraph == nullptr || sculpt == nullptr) {
    return false;
  }

  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(
      *depsgraph, sculpt->paint, sculpt, vc, CTX_data_active_base(C), float2(mval), false, false);
  if (hit) {
    const SculptSession *ss = (ob.runtime != nullptr) ? ob.runtime->sculpt_session : nullptr;
    const int face_hint = (ss != nullptr) ? ss->active_face_index.value_or(-1) : -1;
    shape::SurfaceAnchor probe = anchor.anchor;
    probe.co = hit->location;
    probe.tri = -1;
    probe.tri_corners = int3(-1);
    if (shape::surface_anchor_cache_fill(ob, *depsgraph, face_hint, probe) &&
        probe.has_surface_uv)
    {
      r_uv = probe.surface_uv;
      r_co = hit->location;
      return true;
    }
  }

  float2 plane_px;
  if (!image_select_view3d_mval_to_plane_px(anchor, region, mval, plane_px)) {
    return false;
  }
  r_uv = anchor.jacobian_valid ? anchor.anchor_uv + anchor.plane_uv_jacobian * plane_px :
                                 anchor.anchor_uv;
  r_co = anchor.plane_px_to_object(plane_px);
  return true;
}

bool transform3d_surface_point_at(bContext *C,
                                  ImageSelectTransform3DState &state,
                                  const ImageSelectView3DAnchor &anchor,
                                  const ARegion &region,
                                  const int2 &mval,
                                  float2 &r_uv,
                                  float3 &r_co)
{
  if (state.anchor.object == nullptr) {
    return false;
  }
  return image_select_view3d_surface_point_at(
      C, *state.anchor.object, anchor, region, mval, r_uv, r_co);
}

/** Capture the frame and the cursor's surface point a press starts from. False when the view ray
 * is (near-)parallel to the frame plane, i.e. the drag cannot be mapped. */
bool transform3d_begin_drag(bContext *C,
                            ImageSelectTransform3DState &state,
                            const int2 &mval)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return false;
  }
  transform3d_refresh_display_anchor(C, state, true);
  state.gesture_anchor = state.display_frame_valid ? state.display_anchor : state.anchor;
  state.drag_start = transform3d_snapshot(state);
  state.press_xy = mval;
  state.prev_surface_valid = false;
  if (!image_select_view3d_mval_to_plane_px(
          state.gesture_anchor, *region, mval, state.press_plane))
  {
    state.press_plane = float2(0.0f);
    return false;
  }
  float2 uv;
  float3 co;
  if (transform3d_surface_point_at(C, state, state.gesture_anchor, *region, mval, uv, co)) {
    state.prev_surface_uv = uv;
    state.prev_surface_co = co;
    state.prev_surface_valid = true;
  }
  return true;
}

/**
 * Move: follow the surface instead of the frozen plane. The cursor's surface point is re-resolved
 * on every step; its object-space delta is projected onto the *current* local tangent frame and
 * pushed through that frame's plane->UV Jacobian. Using the object-space delta (rather than a UV
 * difference) is what keeps the fragment from jumping when the cursor crosses a UV seam, and
 * re-sampling the frame is what lets it cross the cube's edge onto the side face.
 */
static void transform3d_apply_move(bContext *C,
                            ImageSelectTransform3DState &state,
                            const ARegion &region,
                            const int2 &mval)
{
  const ImageSelectView3DAnchor &frame = state.display_frame_valid ? state.display_anchor :
                                                                    state.anchor;
  if (!frame.jacobian_valid) {
    return;
  }
  float2 uv_now;
  float3 co_now;
  if (!transform3d_surface_point_at(C, state, frame, region, mval, uv_now, co_now)) {
    return;
  }
  float2 d_px;
  if (state.prev_surface_valid) {
    const float3 d3 = co_now - state.prev_surface_co;
    d_px = float2(math::dot(d3, frame.frame.tangent), math::dot(d3, frame.frame.bitangent)) *
           frame.anchor.px_per_unit;
  }
  else {
    /* The press did not land on the surface: fall back to the gesture plane's own mapping. */
    float2 plane_now, plane_prev;
    if (!image_select_view3d_mval_to_plane_px(state.gesture_anchor, region, mval, plane_now) ||
        !image_select_view3d_mval_to_plane_px(
            state.gesture_anchor, region, state.press_xy, plane_prev))
    {
      return;
    }
    d_px = plane_now - plane_prev;
    state.uv_translation = state.uv_translation +
                           state.gesture_anchor.plane_uv_jacobian * d_px;
    transform3d_refresh_display_anchor(C, state, false);
    return;
  }
  state.uv_translation = state.uv_translation + frame.plane_uv_jacobian * d_px;
  state.prev_surface_uv = uv_now;
  state.prev_surface_co = co_now;
  state.prev_surface_valid = true;
  transform3d_refresh_display_anchor(C, state, false);
}

/** The destination pivot in the gesture frame's plane px. */
static bool transform3d_gesture_pivot_plane(const ImageSelectTransform3DState &state,
                                     const ImageSelectView3DAnchor &frame,
                                     float2 &r_pivot_plane)
{
  bool ok = false;
  const float2x2 jacobian_inv = math::invert(frame.plane_uv_jacobian, ok);
  if (!ok) {
    return false;
  }
  r_pivot_plane = jacobian_inv *
                  ((state.uv_pivot + state.drag_start.translation) - frame.anchor_uv);
  return true;
}

void transform3d_apply_handle(bContext *C,
                              ImageSelectTransform3DState &state,
                              const ARegion &region,
                              const int2 &mval,
                              const bool ctrl)
{
  if (state.mode == image_select_v3d::Transform3DContentMode::Decal) {
    /* The decal frame is the authority: the gestures move its center, roll and scale, and the
     * frame sync keeps the affine and the preview rows in step. */
    transform3d_decal_apply_handle(C, state, region, mval, ctrl);
    return;
  }
  switch (state.active_handle) {
    case Transform3DHandle::Move:
      transform3d_apply_move(C, state, region, mval);
      return;
    case Transform3DHandle::Rotate: {
      const ImageSelectView3DAnchor &frame = state.gesture_anchor;
      if (!frame.jacobian_valid) {
        return;
      }
      float2 plane_now;
      if (!image_select_view3d_mval_to_plane_px(frame, region, mval, plane_now)) {
        return;
      }
      float2 pivot_plane;
      if (!transform3d_gesture_pivot_plane(state, frame, pivot_plane)) {
        return;
      }
      const float2 a = plane_now - pivot_plane;
      const float2 b = state.press_plane - pivot_plane;
      if (math::length(a) <= 1e-6f || math::length(b) <= 1e-6f) {
        return;
      }
      float angle = std::atan2(a.y, a.x) - std::atan2(b.y, b.x);
      /* Ctrl snaps to 15 degree steps. */
      if (ctrl) {
        angle = std::floor(angle / (M_PI / 12.0f) + 0.5f) * (M_PI / 12.0f);
      }
      bool invert_ok = false;
      const float2x2 jacobian_inv = math::invert(frame.plane_uv_jacobian, invert_ok);
      if (!invert_ok) {
        return;
      }
      /* `L = J * R * J^-1 * L0`: a rotation about the pivot as it lies on the current face. */
      state.uv_linear = frame.plane_uv_jacobian *
                        image_select_v3d::plane_rotation(angle) * jacobian_inv *
                        state.drag_start.linear;
      return;
    }
    case Transform3DHandle::Pivot: {
      /* The handle rides at the transformed pivot (uv_pivot + uv_translation). Dragging it
       * re-anchors the pivot under the mouse while the already-applied transform stays visually in
       * place:
       *   P' + L * (p - P') + T' == P + L * (p - P) + T   for every p
       *   =>  T' = T + (I - L) * (P - P')
       * which lands the handle exactly on the cursor's surface UV through
       *   P' = P + L^-1 * (q_uv - (P + T)). */
      const ImageSelectView3DAnchor &frame = state.display_frame_valid ? state.display_anchor :
                                                                        state.anchor;
      float2 q_uv;
      float3 q_co;
      if (!transform3d_surface_point_at(C, state, frame, region, mval, q_uv, q_co)) {
        return;
      }
      UNUSED_VARS(q_co);
      bool invert_ok = false;
      const float2x2 linear_inv = math::invert(state.uv_linear, invert_ok);
      if (!invert_ok) {
        return;
      }
      const float2 pivot_old = state.uv_pivot;
      const float2 pivot_new = pivot_old +
                               linear_inv * (q_uv - (pivot_old + state.uv_translation));
      const float2 pivot_delta = pivot_old - pivot_new;
      state.uv_translation = state.uv_translation + pivot_delta -
                             state.uv_linear * pivot_delta;
      state.uv_pivot = pivot_new;
      transform3d_refresh_display_anchor(C, state, true);
      return;
    }
    default: {
      /* Per-axis scale along the cage's own axes, measured on the gesture frame's plane. */
      const ImageSelectView3DAnchor &frame = state.gesture_anchor;
      if (!frame.jacobian_valid || !state.anchor.jacobian_valid) {
        return;
      }
      float2 plane_now;
      if (!image_select_view3d_mval_to_plane_px(frame, region, mval, plane_now)) {
        return;
      }
      float2 pivot_plane;
      if (!transform3d_gesture_pivot_plane(state, frame, pivot_plane)) {
        return;
      }
      bool invert_ok = false;
      const float2x2 jacobian_inv = math::invert(frame.plane_uv_jacobian, invert_ok);
      if (!invert_ok) {
        return;
      }
      /* The cage's X/Y axes are the source frame's axes carried through the drag-start linear and
       * back onto the gesture plane, so an edge handle scales across the edge it sits on. */
      const float2x2 source_frame = image_select_v3d::plane_rotation(state.frame_angle);
      float2 ex0 = jacobian_inv *
                   (state.drag_start.linear *
                    (state.anchor.plane_uv_jacobian * (source_frame * float2(1.0f, 0.0f))));
      float2 ey0 = jacobian_inv *
                   (state.drag_start.linear *
                    (state.anchor.plane_uv_jacobian * (source_frame * float2(0.0f, 1.0f))));
      if (math::length_squared(ex0) < 1e-20f) {
        ex0 = float2(1.0f, 0.0f);
      }
      else {
        ex0 = math::normalize(ex0);
      }
      if (math::length_squared(ey0) < 1e-20f) {
        ey0 = float2(0.0f, 1.0f);
      }
      else {
        ey0 = math::normalize(ey0);
      }
      const float2 q0 = state.press_plane - pivot_plane;
      const float2 q1 = plane_now - pivot_plane;
      const float denom_x = math::dot(q0, ex0);
      const float denom_y = math::dot(q0, ey0);
      /* Corners scale both axes, an edge only the axis across it (the cage's own frame defines the
       * directions): the left / right edges scale X, the bottom / top edges scale Y. */
      const bool is_edge = ELEM(state.active_handle,
                                Transform3DHandle::Edge0,
                                Transform3DHandle::Edge1,
                                Transform3DHandle::Edge2,
                                Transform3DHandle::Edge3);
      const bool scale_x = !is_edge || ELEM(state.active_handle,
                                            Transform3DHandle::Edge1,
                                            Transform3DHandle::Edge3);
      const bool scale_y = !is_edge || ELEM(state.active_handle,
                                            Transform3DHandle::Edge0,
                                            Transform3DHandle::Edge2);
      if ((scale_x && std::abs(denom_x) < 1e-6f) || (scale_y && std::abs(denom_y) < 1e-6f)) {
        return;
      }
      float sx = scale_x ? math::dot(q1, ex0) / denom_x : 1.0f;
      float sy = scale_y ? math::dot(q1, ey0) / denom_y : 1.0f;
      sx = std::clamp(sx, -1000.0f, 1000.0f);
      sy = std::clamp(sy, -1000.0f, 1000.0f);
      if (std::abs(sx) < 0.001f) {
        sx = 0.001f * (sx < 0.0f ? -1.0f : 1.0f);
      }
      if (std::abs(sy) < 0.001f) {
        sy = 0.001f * (sy < 0.0f ? -1.0f : 1.0f);
      }
      float2x2 axes;
      axes[0] = ex0;
      axes[1] = ey0;
      bool axes_ok = false;
      const float2x2 axes_inv = math::invert(axes, axes_ok);
      if (!axes_ok) {
        return;
      }
      const float2x2 scale_matrix({sx, 0.0f}, {0.0f, sy});
      /* `L = J * E * S * E^-1 * J^-1 * L0`: scale about the pivot along the cage's on-surface
       * axes. */
      state.uv_linear = frame.plane_uv_jacobian * axes * scale_matrix * axes_inv *
                        jacobian_inv * state.drag_start.linear;
      return;
    }
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint
