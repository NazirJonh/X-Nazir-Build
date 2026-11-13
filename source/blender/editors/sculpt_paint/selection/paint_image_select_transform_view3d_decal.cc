/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Transform Selection in the 3D Viewport, decal mode: the floating fragments follow the surface
 * instead of the UV affine, so a fragment moved over a cube edge or a curved surface wraps around
 * it like a sticker rather than being cut off where a planar projection turns edge-on.
 *
 * The session builds one stamp per canvas target (the lifted fragments re-sampled from UV into the
 * cage's own frame, the soft selection coverage as alpha, Normal vectors rewritten into the frame
 * basis) and a surface patch: the mesh triangles around the frame center unfolded into the plane
 * of the center's triangle (#surface_unfold_build). The frame is authored on the surface -- a
 * center UV, a roll relative to the sampled anchor's tangent, scale on the cage axes -- and maps
 * the flattened patch onto the stamp, so the patch is rebuilt only when the center moves or the
 * stamp outgrows it; a rotation or a scale is a 2D re-mapping of the same patch.
 *
 * Nothing in the material is touched while the session runs: the live preview is a draw overlay
 * (POST_VIEW), and the commit rasterizes every patch triangle's UV footprint into the canvas
 * targets (one image undo step). The overlay keeps reading the UV-equivalent affine the state syncs
 * from the frame, so the cage, the handles and the hit tests are the shared ones.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../decal/decal_target.hh"
#include "../projection/projection_target.hh"
#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_decal_log.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_surface_decal.hh"
#include "BKE_surface_unfold.hh"
#include "paint_image_select_transform_view3d_intern.hh"
#include "paint_image_select_view3d.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Shared core adapters
 *
 * The stamp build, the surface patch, the preview and the bake live in
 * `paint_image_select_surface_decal.cc`, shared with the Image Editor's Transform / Move sessions;
 * these adapt the 3D session's state to them.
 * \{ */

void transform3d_decal_release(ImageSelectTransform3DDecal &decal)
{
  surface_decal_release(decal);
}

/** The cage of the lifted fragments in the source anchor's plane, as the stamp build reads it. */
static ImBuf *transform3d_decal_stamp_build(ImageSelectTransform3DState &state, const int target_i)
{
  const ImageSelectView3DAnchor &anchor = state.anchor;
  if (!anchor.jacobian_valid) {
    return nullptr;
  }
  SurfaceDecalCage cage;
  cage.anchor_uv = anchor.anchor_uv;
  cage.plane_uv_jacobian = anchor.plane_uv_jacobian;
  cage.plane_min = state.plane_min;
  cage.plane_max = state.plane_max;
  cage.frame_angle = state.frame_angle;
  return surface_decal_stamp_build(
      cage, state.targets[target_i], Span<SelectionTileFragment>(state.fragments[target_i]));
}

/** Patch seeded at the current display frame (the source anchor until the first re-sample). */
static bool transform3d_decal_patch_ensure(const bContext *C, ImageSelectTransform3DState &state)
{
  const ImageSelectView3DAnchor &frame = state.display_frame_valid ? state.display_anchor :
                                                                    state.anchor;
  return surface_decal_patch_ensure(C,
                                    state.decal,
                                    state.anchor.object,
                                    state.surface_map,
                                    frame.anchor.tri,
                                    frame.anchor.co,
                                    frame.frame.tangent);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mode resolve and session begin
 * \{ */

void transform3d_mode_resolve(bContext *C, ImageSelectTransform3DState &state, wmOperator * /*op*/)
{
  state.mode = image_select_v3d::Transform3DContentMode::UV;
  state.decal = ImageSelectTransform3DDecal();
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr ||
      scene->toolsettings->imapaint.selection_transform_mode !=
          IMAGE_PAINT_SELECT_TRANSFORM_DECAL)
  {
    return;
  }
  /* The surface patch needs only the mesh and its UVs, no material. */
  if (state.anchor.object == nullptr || state.anchor.object->type != OB_MESH) {
    return;
  }
  state.mode = image_select_v3d::Transform3DContentMode::Decal;
}

bool transform3d_decal_begin(bContext *C, ImageSelectTransform3DState &state, wmOperator *op)
{
  if (!state.plane_bounds_valid || state.surface_map.sampler == nullptr) {
    return false;
  }

  /* The frame starts where the lift put the cage: the center on the source anchor's surface
   * point, the roll on the cage's own angle, one scale. The extents are the cage's in object
   * units, so the patch reproduces the fragment at its source size. */
  transform3d_decal_release(state.decal);
  state.decal = ImageSelectTransform3DDecal();
  state.decal.center_uv = state.uv_pivot;
  state.decal.roll = state.frame_angle;
  state.decal.scale = float2(1.0f);
  const float source_px_per_unit = std::max(state.anchor.anchor.px_per_unit, 1e-6f);
  state.decal.base_size = (state.plane_max - state.plane_min) / source_px_per_unit;

  for (const int target_i : state.targets.index_range()) {
    state.decal.stamps.append(transform3d_decal_stamp_build(state, target_i));
  }
  if (!std::any_of(state.decal.stamps.begin(),
                   state.decal.stamps.end(),
                   [](const ImBuf *stamp) { return stamp != nullptr; }))
  {
    transform3d_decal_release(state.decal);
    return false;
  }

  if (!transform3d_decal_patch_ensure(C, state) || state.decal.patch.tris.is_empty()) {
    transform3d_decal_release(state.decal);
    if (op != nullptr) {
      BKE_report(op->reports, RPT_WARNING, "Decal: no surface patch; transforming in UV mode");
    }
    return false;
  }
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                         "session begin: stamps %d, patch %d tris, radius %.5f%s\n",
                         int(state.decal.stamps.size()),
                         int(state.decal.patch.tris.size()),
                         state.decal.patch.radius,
                         state.decal.patch.truncated ? " (truncated)" : "");
  return true;
}

/**
 * Decompose the current UV affine into the decal frame (center, roll, scale), the inverse of
 * #transform3d_decal_sync_affine, so a session switched from UV to Decal starts where the cage and
 * the fragment are. With \a dest the frame sampled at the destination of the cage center:
 *   B = J_dst^-1 · L · J_src = R(roll) · diag(kx, ky) · R(-angle)
 * so `B · R(angle)` has the rotated, scaled axes as columns: the roll is the first column's
 * direction, kx its length and ky the second column's component across it (signed, a mirrored UV
 * keeps its sign). A shear in L (a rotation after a non-uniform scale) has no place in the frame and
 * is dropped. False when the affine or a Jacobian is degenerate.
 */
static bool transform3d_decal_from_affine(const ImageSelectTransform3DState &state,
                                          const ImageSelectView3DAnchor &dest,
                                          float &r_roll,
                                          float2 &r_scale)
{
  if (!dest.jacobian_valid || !state.anchor.jacobian_valid) {
    return false;
  }
  bool invert_ok = false;
  const float2x2 jacobian_dst_inv = math::invert(dest.plane_uv_jacobian, invert_ok);
  if (!invert_ok) {
    return false;
  }
  const float2x2 frame_b = jacobian_dst_inv * (state.uv_linear * state.anchor.plane_uv_jacobian);
  const float2x2 axes = frame_b * image_select_v3d::plane_rotation(state.frame_angle);
  const float2 axis_x = axes[0];
  const float2 axis_y = axes[1];
  const float kx = math::length(axis_x);
  if (kx < 1e-6f) {
    return false;
  }
  const float roll = std::atan2(axis_x.y, axis_x.x);
  const float ky = math::dot(axis_y, float2(-std::sin(roll), std::cos(roll)));
  if (std::abs(ky) < 1e-6f) {
    return false;
  }
  /* The inverse of the `k = base_size * scale * dest_px_per_unit / cage_px` of the sync, with
   * `base_size = cage_px / source_px_per_unit`. */
  const float to_scale = std::max(state.anchor.anchor.px_per_unit, 1e-6f) /
                         std::max(dest.anchor.px_per_unit, 1e-6f);
  r_roll = roll;
  r_scale = float2(kx * to_scale, ky * to_scale);
  return true;
}

bool transform3d_mode_switch(bContext *C, ImageSelectTransform3DState &state, const bool want_decal)
{
  using image_select_v3d::Transform3DContentMode;
  const bool is_decal = state.mode == Transform3DContentMode::Decal;
  if (is_decal == want_decal) {
    /* The setting went back to the session's mode: a change kept during the gesture is void. */
    state.pending_decal = -1;
    return false;
  }
  if (state.is_dragging) {
    /* The gesture keeps the mapping it started with; the change is applied when it ends (see
     * #image_select_view3d_pending_mode_apply). */
    state.pending_decal = want_decal ? 1 : 0;
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                           "mode change deferred until the drag ends\n");
    return false;
  }
  state.pending_decal = -1;
  /* The preview table holds stamp copies in Decal mode and fragment copies in UV mode, in the same
   * slots: drop it so neither mode draws the other's textures. The free enters the GPU context
   * itself. */
  image_select_view3d_preview_textures_free(state.preview_textures);

  if (is_decal) {
    /* Decal -> UV loses nothing: the affine is synced from the frame (bring it up to date when a
     * step-back left it stale), and the patch and stamps are just released. The drag history stays:
     * its snapshots carry the synced affine too. */
    if (state.decal_frame_dirty) {
      transform3d_decal_frame_sync(C, state);
    }
    transform3d_decal_release(state.decal);
    state.decal = ImageSelectTransform3DDecal();
    state.decal_frame_dirty = false;
    state.mode = Transform3DContentMode::UV;
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D "live switch Decal -> UV\n");
    return true;
  }

  if (state.anchor.object == nullptr || state.anchor.object->type != OB_MESH ||
      !state.source_corners_valid || !state.plane_bounds_valid)
  {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                           "live switch to Decal refused: no mesh object or no source cage\n");
    return false;
  }

  /* The decal's frame center is the stamp's center, which is the cage center, and the synced affine
   * pivots on #uv_pivot, so the pivot is moved to the cage center keeping the affine (the same
   * `T' = T + (I - L) * (P - P')` as the Pivot handle). */
  const float2 old_pivot = state.uv_pivot;
  const float2 old_translation = state.uv_translation;
  const float2 cage_center_uv = (state.source_corner_uv[0] + state.source_corner_uv[2]) * 0.5f;
  const float2 pivot_delta = old_pivot - cage_center_uv;
  state.uv_translation = old_translation + pivot_delta - state.uv_linear * pivot_delta;
  state.uv_pivot = cage_center_uv;
  const float2 center_uv = state.uv_pivot + state.uv_translation;
  /* The frame under the destination of the center: its Jacobian and scale carry the decomposition.
   * A center in a UV gap keeps the previous frame. */
  transform3d_refresh_display_anchor(C, state, true);
  const ImageSelectView3DAnchor &dest = state.display_frame_valid ? state.display_anchor :
                                                                    state.anchor;
  float roll = 0.0f;
  float2 scale = float2(1.0f);
  const bool affine_ok = transform3d_decal_from_affine(state, dest, roll, scale);
  if (!affine_ok || !transform3d_decal_begin(C, state, nullptr)) {
    state.uv_pivot = old_pivot;
    state.uv_translation = old_translation;
    state.display_frame_valid = false;
    transform3d_refresh_display_anchor(C, state, true);
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D "live switch to Decal refused: %s\n",
                           affine_ok ? "no stamp or surface patch" : "degenerate affine");
    return false;
  }
  state.decal.center_uv = center_uv;
  state.decal.roll = roll;
  state.decal.scale = scale;
  state.mode = Transform3DContentMode::Decal;
  state.prev_surface_valid = false;
  transform3d_decal_frame_sync(C, state);
  /* The earlier snapshots hold no decal frame: stepping back into one would put a zero center in.
   * The history restarts at the switched state. */
  state.drag_history.clear();
  state.drag_history.append(transform3d_snapshot(state));
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                         "live switch UV -> Decal: center uv (%.4f, %.4f) roll %.4f scale "
                         "(%.4f, %.4f)\n",
                         center_uv.x,
                         center_uv.y,
                         roll,
                         scale.x,
                         scale.y);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame and affine
 * \{ */

/**
 * The UV-equivalent affine of the decal frame, written into the state's #uv_linear /
 * #uv_translation so the overlay and the hit tests read the same fields in both modes:
 *   dest_uv = center_uv + J_dst · R(roll) · diag(kx, ky) · R(-angle) · J_src_inv · (src - pivot)
 * The cage point c lands at the frame's own placement (the extents scaled onto the display
 * anchor's plane), which is what the unfolded patch puts the content at.
 */
static void transform3d_decal_sync_affine(ImageSelectTransform3DState &state)
{
  const ImageSelectView3DAnchor &dest = state.display_frame_valid ? state.display_anchor :
                                                                    state.anchor;
  if (!dest.jacobian_valid || !state.anchor.jacobian_valid || !state.plane_bounds_valid) {
    return;
  }
  bool invert_ok = false;
  const float2x2 jacobian_src_inv = math::invert(state.anchor.plane_uv_jacobian, invert_ok);
  if (!invert_ok) {
    return;
  }
  const float2 cage_size_px = state.plane_max - state.plane_min;
  const float dest_px_per_unit = std::max(dest.anchor.px_per_unit, 1e-6f);
  /* Cage px -> object units (the base extents) over the display anchor's px-per-unit: the
   * dimensionless scale each cage axis carries onto the display plane. */
  const float kx = state.decal.base_size.x * state.decal.scale.x * dest_px_per_unit /
                   std::max(cage_size_px.x, 1e-6f);
  const float ky = state.decal.base_size.y * state.decal.scale.y * dest_px_per_unit /
                   std::max(cage_size_px.y, 1e-6f);
  const float2x2 scale({kx, 0.0f}, {0.0f, ky});
  state.uv_linear = dest.plane_uv_jacobian *
                    (image_select_v3d::plane_rotation(state.decal.roll) *
                     (scale * math::transpose(image_select_v3d::plane_rotation(state.frame_angle)) *
                      jacobian_src_inv));
  state.uv_translation = state.decal.center_uv - state.uv_pivot;
}

bool transform3d_decal_frame_get(const ImageSelectTransform3DState &state,
                                 projection::ProjectionFrame &r_frame)
{
  const ImageSelectView3DAnchor &dest = state.display_frame_valid ? state.display_anchor :
                                                                    state.anchor;
  r_frame.center[0] = dest.anchor.co.x;
  r_frame.center[1] = dest.anchor.co.y;
  r_frame.center[2] = dest.anchor.co.z;
  r_frame.size[0] = std::max(state.decal.base_size.x * state.decal.scale.x, 1e-6f);
  r_frame.size[1] = std::max(state.decal.base_size.y * state.decal.scale.y, 1e-6f);
  /* The depth axis stays neutral: a surface patch has no thickness. */
  r_frame.size[2] = 1.0f;

  /* The frame's X is the anchor tangent rolled by the decal roll, Y follows, Z is the surface
   * normal: the same basis construction #transform3d_projection_frame builds from the affine. */
  const float cos_roll = std::cos(state.decal.roll);
  const float sin_roll = std::sin(state.decal.roll);
  float3 axis_x = dest.frame.tangent * cos_roll + dest.frame.bitangent * sin_roll;
  float3 axis_y = dest.frame.tangent * (-sin_roll) + dest.frame.bitangent * cos_roll;
  const float3 axis_z = dest.frame.normal;
  if (math::dot(math::cross(axis_x, axis_y), axis_z) < 0.0f) {
    axis_y = -axis_y;
  }
  /* Blender's matrix helpers read a row-major matrix; the frame stores columns. */
  const float3 axes[3] = {axis_x, axis_y, axis_z};
  float rot[3][3];
  for (const int c : IndexRange(3)) {
    for (const int r : IndexRange(3)) {
      rot[r][c] = axes[c][r];
    }
  }
  BKE_paint_layers_projection_frame_from_matrix(rot, r_frame.normal, &r_frame.roll);
  return true;
}

void transform3d_decal_frame_sync(const bContext *C, ImageSelectTransform3DState &state)
{
  /* The affine first: #transform3d_refresh_display_anchor samples it at the pivot's destination,
   * which is the decal center; the sync runs again after, with the anchor the resample built. */
  transform3d_decal_sync_affine(state);
  transform3d_refresh_display_anchor(C, state, false);
  transform3d_decal_sync_affine(state);
  /* Keeps the previous patch when the center has no surface under it (a UV gap). */
  transform3d_decal_patch_ensure(C, state);
  state.decal_frame_dirty = false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gestures
 * \{ */

/**
 * True when \a center_uv samples onto the surface within \a tolerance (object units) of
 * \a expected_co. The Move gesture advances the center through the plane -> UV Jacobian, which is
 * only right inside one UV island: past a seam the result is a gap, or another island's point far
 * from where the cursor actually is.
 */
static bool transform3d_decal_center_near(const bContext *C,
                                          const ImageSelectTransform3DState &state,
                                          const float2 &center_uv,
                                          const float3 &expected_co,
                                          const float tolerance)
{
  Object *ob = state.anchor.object;
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (ob == nullptr || depsgraph == nullptr || ob->type != OB_MESH || ob->data == nullptr) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  float3 co, no;
  if (!state.surface_map.sample(mesh.corner_verts(),
                                bke::pbvh::vert_positions_eval(*depsgraph, *ob),
                                center_uv,
                                co,
                                no))
  {
    return false;
  }
  return math::distance(co, expected_co) <= tolerance;
}

void transform3d_decal_apply_handle(bContext *C,
                                    ImageSelectTransform3DState &state,
                                    const ARegion &region,
                                    const int2 &mval,
                                    const bool ctrl)
{
  switch (state.active_handle) {
    case Transform3DHandle::Move: {
      /* The center follows the surface point under the cursor: its object-space delta is
       * projected onto the current display frame and pushed through that frame's plane->UV
       * Jacobian, so crossing an edge inside an island keeps the fragment glued to the surface
       * (the same discipline as the UV mode's move). */
      const ImageSelectView3DAnchor &frame = state.display_frame_valid ? state.display_anchor :
                                                                        state.anchor;
      float2 uv_now;
      float3 co_now;
      if (!transform3d_surface_point_at(C, state, frame, region, mval, uv_now, co_now)) {
        return;
      }
      if (state.prev_surface_valid) {
        const float3 d3 = co_now - state.prev_surface_co;
        const float2 d_px = float2(math::dot(d3, frame.frame.tangent),
                                   math::dot(d3, frame.frame.bitangent)) *
                           frame.anchor.px_per_unit;
        float2 center = state.decal.center_uv + frame.plane_uv_jacobian * d_px;
        if (!transform3d_decal_center_near(
                C, state, center, frame.anchor.co + d3, 0.6f * math::length(d3) + 1e-5f))
        {
          /* A seam (or a gap) in the way: the patch itself does not care about the UV layout, so
           * the center is re-seated from the cursor's own surface point, shifted back by the grab
           * offset when that lands on the surface too. */
          center = uv_now;
          ImageSelectView3DAnchor now;
          if (image_select_view3d_anchor_from_uv(
                  C, state.surface_map, uv_now, frame.frame.tangent, now) &&
              now.jacobian_valid)
          {
            const float2 shifted = uv_now - now.plane_uv_jacobian * state.press_plane;
            const float grab = math::length(state.press_plane) /
                               std::max(frame.anchor.px_per_unit, 1e-6f);
            if (transform3d_decal_center_near(C, state, shifted, co_now, grab * 1.5f + 1e-5f)) {
              center = shifted;
            }
          }
        }
        state.decal.center_uv = center;
      }
      else {
        /* The press did not land on the surface: the drag delta of the gesture plane moves the
         * center through the gesture anchor's own Jacobian. */
        float2 plane_now;
        if (!image_select_view3d_mval_to_plane_px(state.gesture_anchor, region, mval, plane_now)) {
          return;
        }
        state.decal.center_uv = state.decal.center_uv +
                                state.gesture_anchor.plane_uv_jacobian *
                                    (plane_now - state.press_plane);
      }
      state.prev_surface_uv = uv_now;
      state.prev_surface_co = co_now;
      state.prev_surface_valid = true;
      transform3d_decal_frame_sync(C, state);
      return;
    }
    case Transform3DHandle::Rotate: {
      /* The roll turns with the cursor around the frame center, which is the gesture anchor's
       * origin (the display anchor is anchored at the center). */
      const ImageSelectView3DAnchor &frame = state.gesture_anchor;
      if (!frame.jacobian_valid) {
        return;
      }
      float2 plane_now;
      if (!image_select_view3d_mval_to_plane_px(frame, region, mval, plane_now)) {
        return;
      }
      if (math::length(plane_now) <= 1e-6f || math::length(state.press_plane) <= 1e-6f) {
        return;
      }
      float angle = std::atan2(plane_now.y, plane_now.x) -
                    std::atan2(state.press_plane.y, state.press_plane.x);
      /* Ctrl snaps to 15 degree steps, as the UV rotate does. */
      if (ctrl) {
        angle = std::floor(angle / (M_PI / 12.0f) + 0.5f) * (M_PI / 12.0f);
      }
      state.decal.roll = state.drag_start.decal_roll + angle;
      transform3d_decal_frame_sync(C, state);
      return;
    }
    case Transform3DHandle::Pivot: {
      /* Re-anchor the frame center under the cursor's surface point. */
      const ImageSelectView3DAnchor &frame = state.display_frame_valid ? state.display_anchor :
                                                                        state.anchor;
      float2 q_uv;
      float3 q_co;
      if (!transform3d_surface_point_at(C, state, frame, region, mval, q_uv, q_co)) {
        return;
      }
      UNUSED_VARS(q_co);
      state.decal.center_uv = q_uv;
      state.prev_surface_valid = false;
      transform3d_decal_frame_sync(C, state);
      return;
    }
    default: {
      /* Scale on the cage axes, measured on the gesture plane against the frame's own axes (the
       * pivot is the gesture anchor's origin, the frame center). */
      const ImageSelectView3DAnchor &frame = state.gesture_anchor;
      if (!frame.jacobian_valid) {
        return;
      }
      float2 plane_now;
      if (!image_select_view3d_mval_to_plane_px(frame, region, mval, plane_now)) {
        return;
      }
      const float2x2 roll_press = image_select_v3d::plane_rotation(state.drag_start.decal_roll);
      const float2 ex0 = roll_press * float2(1.0f, 0.0f);
      const float2 ey0 = roll_press * float2(0.0f, 1.0f);
      const float2 q0 = state.press_plane;
      const float2 q1 = plane_now;
      const float denom_x = math::dot(q0, ex0);
      const float denom_y = math::dot(q0, ey0);
      /* Corners scale both axes, an edge only the axis across it, the UV cage's rule. */
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
      state.decal.scale = float2(state.drag_start.decal_scale.x * sx,
                                 state.drag_start.decal_scale.y * sy);
      transform3d_decal_frame_sync(C, state);
      return;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preview (POST_VIEW overlay)
 * \{ */

void transform3d_decal_draw_preview(const ARegion *region, ImageSelectTransform3DState &state)
{
  if (state.anchor.object == nullptr) {
    return;
  }
  surface_decal_draw_preview(region,
                             state.decal,
                             *state.anchor.object,
                             state.surface_map,
                             state.targets,
                             state.preview_textures,
                             state.pbr_default_textures,
                             state.preview_target_resolved());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Commit (bake)
 * \{ */

void transform3d_decal_commit(bContext *C, ImageSelectTransform3DState &state)
{
  /* The patch of the final frame (a step-back or a restore may have left it stale). */
  transform3d_decal_patch_ensure(C, state);
  if (state.decal.patch.tris.is_empty()) {
    /* Nothing to write the stamps through: end like a cancel, which puts the lifted source region
     * back, rather than losing the content. */
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D "commit: no surface patch, cancelling\n");
    state.cancel(C);
    return;
  }

  /* The stamps rasterize into the real canvas targets in ONE image undo step. */
  surface_decal_commit(
      C,
      "Transform Selection (Decal)",
      state.decal,
      state.surface_map,
      state.targets,
      state.fragments,
      [&](const Image *ima) { return image_select_view3d_image_is_alive(state, ima); });
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                         "commit bake: %d target(s) baked through %d patch tris\n",
                         int(state.targets.size()),
                         int(state.decal.patch.tris.size()));
  transform3d_decal_release(state.decal);
}

/** \} */

}  // namespace blender::ed::sculpt_paint