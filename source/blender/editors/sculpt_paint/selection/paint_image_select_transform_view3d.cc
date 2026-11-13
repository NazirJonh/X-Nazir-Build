/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Transform Selection in the 3D Viewport: move / rotate / scale the floating fragments with a
 * handle overlay drawn on the surface.
 *
 * The transform is authoritative in UV space: `dest_uv = pivot + L * (src_uv - pivot) +
 * translation`. The *frame* it is authored in is surface-local: a #ImageSelectView3DAnchor is
 * re-sampled at the transformed pivot as the fragment moves, so the cage, the rotate / scale plane
 * and the cursor mapping all follow the face the content is on instead of the plane of the first
 * click. Moving across a cube edge turns the gizmo onto the side face; the cage edges are resolved
 * point by point on the surface, so they bend over the edge with it.
 *
 * #transform3d_mapping_get also carries the destination frame as the projection system's
 * #projection::ProjectionFrame (see #image_select_transform3d_projection_frame_get): a projected content mapping can consume the
 * frame instead of re-deriving it from the affine, replacing only the mapping -- the gizmo, the
 * cage and the gestures stay untouched.
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
#include "BKE_mesh.hh"
#include "BKE_paint_layers.hh"
#include "BKE_screen.hh"

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

#include "../mesh/sculpt_intern.hh"
#include "../paint_intern.hh"
#include "../projection/projection_target.hh"
#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_transform_view3d_intern.hh"
#include "paint_image_select_view3d.hh"

namespace blender::ed::sculpt_paint {

float2x2 ImageSelectTransform3DState::linear_uv() const
{
  return this->uv_linear;
}

float2 ImageSelectTransform3DState::pivot_uv() const
{
  return this->uv_pivot;
}

float2 ImageSelectTransform3DState::translation_uv() const
{
  return this->uv_translation;
}

/**
 * The destination frame as the projection system's inline frame (object-space centre, full
 * extents, normal + roll). The in-plane axes are the cage's own axes, so the values describe the
 * content as the gizmo shows it. The depth extent stays neutral: a surface patch has no thickness,
 * and a projected backend decides the depth from its own settings.
 */
static projection::ProjectionFrame transform3d_projection_frame(
    const ImageSelectTransform3DState &state)
{
  projection::ProjectionFrame frame;
  const ImageSelectView3DAnchor &dest = state.display_frame_valid ? state.display_anchor :
                                                                    state.anchor;
  frame.center[0] = dest.anchor.co.x;
  frame.center[1] = dest.anchor.co.y;
  frame.center[2] = dest.anchor.co.z;
  frame.size[2] = 1.0f;

  bool jacobian_ok = false;
  const float2x2 dest_jacobian_inv = math::invert(dest.plane_uv_jacobian, jacobian_ok);
  const float dest_px_per_unit = std::max(dest.anchor.px_per_unit, 1e-6f);
  if (!state.anchor.jacobian_valid || !jacobian_ok) {
    frame.size[0] = (state.plane_max.x - state.plane_min.x) /
                    std::max(state.anchor.anchor.px_per_unit, 1e-6f);
    frame.size[1] = (state.plane_max.y - state.plane_min.y) /
                    std::max(state.anchor.anchor.px_per_unit, 1e-6f);
    return frame;
  }

  const float2x2 frame_rotation = image_select_v3d::plane_rotation(state.frame_angle);
  const float2 source_axis_x = frame_rotation * float2(1.0f, 0.0f);
  const float2 source_axis_y = frame_rotation * float2(0.0f, 1.0f);
  const float2 dest_axis_x = dest_jacobian_inv *
                             (state.uv_linear *
                              (state.anchor.plane_uv_jacobian * source_axis_x));
  const float2 dest_axis_y = dest_jacobian_inv *
                             (state.uv_linear *
                              (state.anchor.plane_uv_jacobian * source_axis_y));
  const float axis_x_len = math::length(dest_axis_x);
  const float axis_y_len = math::length(dest_axis_y);
  frame.size[0] = (state.plane_max.x - state.plane_min.x) * axis_x_len / dest_px_per_unit;
  frame.size[1] = (state.plane_max.y - state.plane_min.y) * axis_y_len / dest_px_per_unit;

  const float2 ex = (axis_x_len > 1e-6f) ? dest_axis_x / axis_x_len : float2(1.0f, 0.0f);
  const float2 ey = (axis_y_len > 1e-6f) ? dest_axis_y / axis_y_len : float2(0.0f, 1.0f);
  float3 axis_x = dest.frame.tangent * ex.x + dest.frame.bitangent * ex.y;
  float3 axis_y = dest.frame.tangent * ey.x + dest.frame.bitangent * ey.y;
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
  /* The projection frame stores normal + roll: the inverse of the same basis the gizmo builds. */
  BKE_paint_layers_projection_frame_from_matrix(rot, frame.normal, &frame.roll);
  return frame;
}

ImageSelectTransform3DMapping transform3d_mapping_get(
    const ImageSelectTransform3DState &state)
{
  ImageSelectTransform3DMapping mapping;
  mapping.linear = state.uv_linear;
  mapping.pivot = state.uv_pivot;
  mapping.translation = state.uv_translation;
  mapping.frames_valid = state.display_frame_valid;
  if (state.display_frame_valid) {
    mapping.projection = transform3d_projection_frame(state);
  }
  return mapping;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lift / hand-over helpers
 * \{ */

static void transform3d_free_draw_handle(ImageSelectTransform3DState &state)
{
  image_select_view3d_floating_draw_handle_clear(state);
  if (state.draw_handle_texture != nullptr && state.owner_region_type != nullptr) {
    ED_region_draw_cb_exit(state.owner_region_type, state.draw_handle_texture);
    state.draw_handle_texture = nullptr;
  }
}

static Vector<Vector<float2>> transform3d_build_outline_uv(
    const Vector<Vector<SelectionTileFragment>> &fragments)
{
  Vector<Vector<float2>> result;
  for (const int target_i : fragments.index_range()) {
    for (const SelectionTileFragment &frag : fragments[target_i]) {
      if (frag.pixels.fragment_mask_ibuf == nullptr) {
        continue;
      }
      const ImBuf *mask = frag.pixels.fragment_mask_ibuf;
      const float *data = mask->float_data();
      if (data == nullptr) {
        continue;
      }
      const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
      const float2 origin = float2(frag.geom.origin_px);
      const float2 tile_size = float2(frag.geom.tile_size_px);
      const auto uv_cell = [&](const int2 &px) {
        return tile_uv + (origin + float2(px)) / tile_size;
      };
      Vector<float2> segments;
      image_select_mask_boundary_segments(
          data, int2(mask->x, mask->y), int2(0, 0), int2(mask->x, mask->y), uv_cell, 1, segments);
      if (!segments.is_empty()) {
        result.append(std::move(segments));
      }
    }
  }
  return result;
}

/** The union source-UV bounds of all fragments' tight selection rects (for the pivot / handle
 * quad). The capture rect additionally carries the feather pad, which would displace the handles
 * off the selection outline; the Image Editor's transform cage uses the tight rect the same way
 * (#image_select_fragment_ui_origin / #image_select_fragment_ui_size). */
static Bounds<float2> transform3d_source_bounds(
    const Vector<Vector<SelectionTileFragment>> &fragments)
{
  Bounds<float2> bounds;
  bounds.min = float2(FLT_MAX, FLT_MAX);
  bounds.max = float2(-FLT_MAX, -FLT_MAX);
  for (const Vector<SelectionTileFragment> &per_target : fragments) {
    for (const SelectionTileFragment &frag : per_target) {
      const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
      const float2 tile_size = float2(frag.geom.tile_size_px);
      /* #selection_origin_px is absolute tile px for this tool's fragments
       * (#image_select_extract_per_tile); the ui_ helpers fall back to the capture rect when the
       * tight rect is unset. */
      const int2 ui_origin = image_select_fragment_ui_origin(frag);
      const int2 ui_size = image_select_fragment_ui_size(frag);
      const float2 lo = tile_uv + float2(ui_origin) / tile_size;
      const float2 hi = lo + float2(ui_size) / tile_size;
      bounds.min = math::min(bounds.min, lo);
      bounds.max = math::max(bounds.max, hi);
    }
  }
  return bounds;
}

/**
 * The minimal-area oriented rectangle of \a points (rotating calipers over the convex hull): the
 * frame the selection's own edges define. \a r_angle is the frame orientation folded into
 * (-45, 45] degrees (the rectangle is symmetric under quarter turns, so the smallest turn from the
 * plane axes is the one that keeps the cage upright); \a r_lo / \a r_hi are the rectangle bounds in
 * that frame. Near-ties (a square, a circle) keep the frame closest to the plane axes, so the cage
 * does not jitter between equivalent orientations.
 */
static void transform3d_min_area_frame(const Span<float2> points,
                                float &r_angle,
                                float2 &r_lo,
                                float2 &r_hi)
{
  Vector<float2> sorted(points);
  std::sort(sorted.begin(), sorted.end(), [](const float2 &a, const float2 &b) {
    return a.x < b.x || (a.x == b.x && a.y < b.y);
  });
  Vector<float2> unique_points;
  for (const float2 &p : sorted) {
    if (unique_points.is_empty() || unique_points.last() != p) {
      unique_points.append(p);
    }
  }

  const auto bounds_in_frame = [&](const Span<float2> pts, const float phi, float2 &lo, float2 &hi) {
    const float c = std::cos(phi);
    const float s = std::sin(phi);
    lo = float2(FLT_MAX, FLT_MAX);
    hi = float2(-FLT_MAX, -FLT_MAX);
    for (const float2 &p : pts) {
      const float2 q(c * p.x + s * p.y, -s * p.x + c * p.y);
      lo = math::min(lo, q);
      hi = math::max(hi, q);
    }
  };

  r_angle = 0.0f;
  bounds_in_frame(unique_points, 0.0f, r_lo, r_hi);
  if (unique_points.size() < 3) {
    return;
  }

  /* Convex hull (monotone chain). */
  const auto cross = [](const float2 &o, const float2 &a, const float2 &b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };
  Vector<float2> hull;
  for (const float2 &p : unique_points) {
    while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.last(), p) <= 0.0f) {
      hull.remove_last();
    }
    hull.append(p);
  }
  const int64_t lower_size = hull.size() + 1;
  for (int64_t i = unique_points.size() - 2; i >= 0; i--) {
    const float2 &p = unique_points[i];
    while (hull.size() >= lower_size && cross(hull[hull.size() - 2], hull.last(), p) <= 0.0f) {
      hull.remove_last();
    }
    hull.append(p);
  }
  hull.remove_last();
  if (hull.size() < 3) {
    return;
  }

  const float half_pi = float(M_PI_2);
  const float quarter_pi = float(M_PI_4);
  float best_area = (r_hi.x - r_lo.x) * (r_hi.y - r_lo.y);
  for (const int64_t i : hull.index_range()) {
    const float2 edge = hull[(i + 1) % hull.size()] - hull[i];
    if (math::length_squared(edge) < 1e-20f) {
      continue;
    }
    float phi = std::fmod(std::atan2(edge.y, edge.x), half_pi);
    if (phi > quarter_pi) {
      phi -= half_pi;
    }
    else if (phi <= -quarter_pi) {
      phi += half_pi;
    }
    float2 lo, hi;
    bounds_in_frame(hull, phi, lo, hi);
    const float area = (hi.x - lo.x) * (hi.y - lo.y);
    const bool smaller = area < best_area * (1.0f - 1e-4f);
    const bool tie = area <= best_area * (1.0f + 1e-4f) && std::abs(phi) < std::abs(r_angle);
    if (smaller || tie) {
      best_area = std::min(area, best_area);
      r_angle = phi;
      r_lo = lo;
      r_hi = hi;
    }
  }
}

static void transform3d_lift_fragments(bContext *C, ImageSelectTransform3DState &state)
{
  for (const int target_i : state.fragments.index_range()) {
    image_select_fragment_lift_source(C,
                                      state.targets[target_i].image,
                                      state.targets[target_i].iuser,
                                      Span<SelectionTileFragment>(state.fragments[target_i]));
  }
  if (state.anchor.object != nullptr) {
    state.surface_map.ensure(*state.anchor.object, state.anchor.uv_map_name());
  }
  state.outline_uv = transform3d_build_outline_uv(state.fragments);
  /* The cage is the minimal oriented rectangle of the fragment *content* on the anchor plane (see
   * #transform3d_min_area_frame): a box select gives back its own edges, and empty (transparent)
   * pixels of the selection do not stretch it. It is taken on the plane, not in UV, so it stays a
   * rectangle however the UV island is oriented on screen. The pivot starts at its center. */
  state.plane_bounds_valid = false;
  state.frame_angle = 0.0f;
  bool invert_ok = false;
  const float2x2 jacobian_inv = math::invert(state.anchor.plane_uv_jacobian, invert_ok);
  if (!state.anchor.jacobian_valid || !invert_ok) {
    return;
  }
  Vector<float2> plane_points;
  const auto include_uv = [&](const float2 &uv) {
    plane_points.append(jacobian_inv * (uv - state.anchor.anchor_uv));
  };
  for (const Vector<SelectionTileFragment> &per_target : state.fragments) {
    for (const SelectionTileFragment &frag : per_target) {
      bool any = false;
      const Array<float> content = image_select_fragment_content_mask(frag, any);
      if (!any) {
        continue;
      }
      const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
      const float2 origin = float2(frag.geom.origin_px);
      const float2 tile_size = float2(frag.geom.tile_size_px);
      const auto uv_cell = [&](const int2 &px) {
        return tile_uv + (origin + float2(px)) / tile_size;
      };
      Vector<float2> segments;
      image_select_mask_boundary_segments(content.data(),
                                          frag.geom.size_px,
                                          int2(0, 0),
                                          frag.geom.size_px,
                                          uv_cell,
                                          1,
                                          segments);
      for (const float2 &uv : segments) {
        include_uv(uv);
      }
    }
  }
  if (plane_points.is_empty()) {
    /* No content (or no mask): the selection outline, then the tight rect corners. */
    for (const Vector<float2> &segments : state.outline_uv) {
      for (const float2 &uv : segments) {
        include_uv(uv);
      }
    }
  }
  if (plane_points.is_empty()) {
    const Bounds<float2> bounds = transform3d_source_bounds(state.fragments);
    if (bounds.min.x <= bounds.max.x) {
      include_uv(bounds.min);
      include_uv(bounds.max);
      include_uv(float2(bounds.min.x, bounds.max.y));
      include_uv(float2(bounds.max.x, bounds.min.y));
    }
  }
  if (plane_points.is_empty()) {
    return;
  }
  transform3d_min_area_frame(
      plane_points, state.frame_angle, state.plane_min, state.plane_max);
  state.plane_bounds_valid = true;

  /* The cage corners and the pivot in source UV: the affine's origin. Everything the tool does
   * afterwards maps these through #uv_linear / #uv_translation, so the content, the cage and the
   * drag math share one definition. */
  const float2x2 frame = image_select_v3d::plane_rotation(state.frame_angle);
  const float2 plane_corners[4] = {
      frame * state.plane_min,
      frame * float2(state.plane_max.x, state.plane_min.y),
      frame * state.plane_max,
      frame * float2(state.plane_min.x, state.plane_max.y),
  };
  for (const int i : IndexRange(4)) {
    state.source_corner_uv[i] = state.anchor.anchor_uv +
                                state.anchor.plane_uv_jacobian * plane_corners[i];
  }
  state.source_corners_valid = true;
  const float2 plane_pivot = frame * ((state.plane_min + state.plane_max) * 0.5f);
  state.uv_pivot = state.anchor.anchor_uv + state.anchor.plane_uv_jacobian * plane_pivot;
  state.uv_linear = float2x2::identity();
  state.uv_translation = float2(0.0f);
  state.display_frame_valid = false;
  state.prev_surface_valid = false;
  transform3d_refresh_display_anchor(C, state, true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen mapping and handles
 * \{ */

/**
 * Re-sample the surface-local frame at the transformed pivot. This is the frame the cage, the
 * rotate / scale drag plane and the off-surface fallback use: as the fragment moves over an edge,
 * the frame is rebuilt on the new face and the gizmo turns with it instead of staying in the plane
 * it was created in.
 *
 * \param force: rebuild even when the pivot UV has not moved.
 * \return false when the pivot is off the surface (a UV gap); the previous frame is kept.
 */
bool transform3d_refresh_display_anchor(const bContext *C,
                                        ImageSelectTransform3DState &state,
                                        const bool force)
{
  if (C == nullptr || state.anchor.object == nullptr) {
    return false;
  }
  const float2 pivot_dest_uv = state.uv_pivot + state.uv_translation;
  if (!force && state.display_frame_valid &&
      math::distance_squared(pivot_dest_uv, state.display_uv) < 1e-12f)
  {
    return true;
  }
  const float3 prev_tangent = state.display_frame_valid ? state.display_anchor.frame.tangent :
                                                          state.anchor.frame.tangent;
  ImageSelectView3DAnchor anchor;
  if (!image_select_view3d_anchor_from_uv(
          C, state.surface_map, pivot_dest_uv, prev_tangent, anchor))
  {
    return false;
  }
  state.display_anchor = anchor;
  state.display_uv = pivot_dest_uv;
  state.display_frame_valid = true;
  return true;
}

bool transform3d_uv_to_screen(ImageSelectTransform3DState &state,
                              const ARegion &region,
                              const float2 &uv,
                              float2 &r_screen)
{
  const Object *ob = state.anchor.object;
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (ob == nullptr || rv3d == nullptr || !state.surface_map_ref().sampler) {
    return false;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (mesh == nullptr) {
    return false;
  }
  float3 co, no;
  if (!state.surface_map_ref().sample(
          mesh->corner_verts(), mesh->vert_positions(), uv, co, no))
  {
    /* Fallback: the *current* local plane. The cage corners / rotate handle can sit outside the
     * UV layout (a fragment next to an UV island border has no triangle there), and the handles
     * must stay visible and grabbable -- #screen_valid gates the whole handle set, one missed
     * projection used to hide them all and made rotate / scale unreachable. Using the display
     * frame, not the frozen source anchor, keeps the fallback on the face the gizmo now sits on. */
    const ImageSelectView3DAnchor &fallback = state.display_frame_valid ? state.display_anchor :
                                                                          state.anchor;
    if (!fallback.jacobian_valid) {
      return false;
    }
    bool ok = false;
    const float2x2 jacobian_inv = math::invert(fallback.plane_uv_jacobian, ok);
    if (!ok) {
      return false;
    }
    co = fallback.plane_px_to_object(jacobian_inv * (uv - fallback.anchor_uv));
  }
  const float3 world = math::transform_point(float4x4(ob->object_to_world()), co);
  const float4 proj = float4x4(rv3d->persmat) * float4(world.x, world.y, world.z, 1.0f);
  if (proj.w <= 0.0f) {
    return false;
  }
  r_screen = float2((proj.x / proj.w * 0.5f + 0.5f) * float(region.winx),
                    (proj.y / proj.w * 0.5f + 0.5f) * float(region.winy));
  return true;
}

/** The UV point a source point maps to through the current transform. */
static float2 transform3d_dest_uv(const ImageSelectTransform3DState &state, const float2 &src_uv)
{
  return image_select_v3d::transform_uv(
      state.uv_linear, state.uv_pivot, state.uv_translation, src_uv);
}

/**
 * Sample one cage edge on the surface: the edge is a straight line in UV, but on the mesh it bends
 * wherever it crosses a face, so it is resolved as a polyline whose points each lie on the surface.
 * This is what makes the cage visibly follow the cube's side face instead of floating in the plane
 * the transform was created in.
 */
static void transform3d_sample_cage_edge(ImageSelectTransform3DState &state,
                                  const ARegion &region,
                                  const float2 &src_a,
                                  const float2 &src_b,
                                  Vector<float2> &r_screen)
{
  constexpr int SEGMENTS = 16;
  r_screen.clear();
  r_screen.resize(SEGMENTS + 1);
  bool all = true;
  for (const int i : IndexRange(SEGMENTS + 1)) {
    const float t = float(i) / float(SEGMENTS);
    const float2 src = src_a + (src_b - src_a) * t;
    all = all && transform3d_uv_to_screen(
                     state, region, transform3d_dest_uv(state, src), r_screen[i]);
  }
  if (!all) {
    r_screen.clear();
  }
}

/** Distance of the rotate handle from the top edge of the cage, in region px. */
static float transform3d_rotate_offset_px()
{
  return 28.0f * UI_SCALE_FAC;
}

/** The cage (corners, edge midpoints, rotate handle, pivot) of the *transformed* fragment,
 * projected to the region. */
void transform3d_refresh_screen(ImageSelectTransform3DState &state, const ARegion &region)
{
  state.screen_valid = false;
  if (!state.plane_bounds_valid || !state.source_corners_valid || state.fragments.is_empty()) {
    return;
  }
  bool all = true;
  for (const int i : IndexRange(4)) {
    transform3d_sample_cage_edge(state,
                                 region,
                                 state.source_corner_uv[i],
                                 state.source_corner_uv[(i + 1) % 4],
                                 state.screen_cage[i]);
    if (state.screen_cage[i].is_empty()) {
      all = false;
      continue;
    }
    state.screen_corners[i] = state.screen_cage[i][0];
    state.screen_edges[i] = state.screen_cage[i][state.screen_cage[i].size() / 2];
  }
  /* The rotate handle stands off the top edge along the screen-space direction from the cage
   * center, so it keeps a constant distance whatever the rotation, scale or view perspective. */
  float2 center_screen;
  all = all && transform3d_uv_to_screen(
                   state, region, state.uv_pivot + state.uv_translation, center_screen);
  if (all) {
    float2 dir = state.screen_edges[2] - center_screen;
    dir = math::length(dir) > 1e-6f ? math::normalize(dir) : float2(0.0f, 1.0f);
    state.screen_rotate = state.screen_edges[2] + dir * transform3d_rotate_offset_px();
    /* Origin handle: the *transformed* pivot (rotation / scale fix the pivot, the translation
     * carries it), so the handle rides with the fragment during a move like the Image Editor's
     * anchor gizmo (#uv_anchor moves with the content there) and the Shapes origin ring. */
    state.screen_pivot = center_screen;
  }
  state.screen_valid = all;
}

static float transform3d_handle_hit_px()
{
  return 12.0f * UI_SCALE_FAC;
}

/** The handle under \a mval, #Transform3DHandle::None when the cursor is on none of them. */
static Transform3DHandle transform3d_hit_handle(const ImageSelectTransform3DState &state,
                                         const int2 &mval)
{
  if (!state.screen_valid) {
    return Transform3DHandle::None;
  }
  const float2 m(mval);
  const float hit = transform3d_handle_hit_px();
  if (math::distance(m, state.screen_pivot) <= hit) {
    return Transform3DHandle::Pivot;
  }
  if (math::distance(m, state.screen_rotate) <= hit) {
    return Transform3DHandle::Rotate;
  }
  for (const int i : IndexRange(4)) {
    if (math::distance(m, state.screen_corners[i]) <= hit) {
      return Transform3DHandle(int(Transform3DHandle::Corner0) + i);
    }
  }
  for (const int i : IndexRange(4)) {
    if (math::distance(m, state.screen_edges[i]) <= hit) {
      return Transform3DHandle(int(Transform3DHandle::Edge0) + i);
    }
  }
  return Transform3DHandle::None;
}

/** The handle a press at \a mval grabs: a real handle, else a move of the whole fragment. */
static Transform3DHandle transform3d_hit_test(const ImageSelectTransform3DState &state,
                                       const int2 &mval)
{
  const Transform3DHandle handle = transform3d_hit_handle(state, mval);
  return handle == Transform3DHandle::None ? Transform3DHandle::Move : handle;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifecycle
 * \{ */

void ImageSelectTransform3DState::commit(bContext *C)
{
  transform3d_free_draw_handle(*this);
  WM_cursor_modal_restore(CTX_wm_window(C));
  if (this->mode == image_select_v3d::Transform3DContentMode::Decal) {
    /* The decal session owns its finish: the stamps bake into the canvases through the surface
     * patch, then it cleans the session's own state up. */
    transform3d_decal_commit(C, *this);
    return;
  }
  const float2x2 linear = this->uv_linear;
  const float2 pivot = this->uv_pivot;
  const float2 translation = this->uv_translation;

  /* The committable targets, in resolve order: the whole commit lands in ONE image undo step, so
   * a single Ctrl+Z restores every Material canvas together (see
   * #image_select_view3d_commit_targets_with_undo). */
  Vector<ImagePaintSelectionTarget> commit_targets;
  Vector<Span<SelectionTileFragment>> commit_fragments;
  for (const int target_i : this->targets.index_range()) {
    Image *ima = this->targets[target_i].image;
    if (ima == nullptr || this->fragments[target_i].is_empty() ||
        !image_select_view3d_image_is_alive(*this, ima))
    {
      continue;
    }
    commit_targets.append(this->targets[target_i]);
    commit_fragments.append(Span<SelectionTileFragment>(this->fragments[target_i]));
  }
  image_select_view3d_commit_targets_with_undo(
      C,
      "Transform Selection",
      commit_targets,
      commit_fragments,
      [&](Image &ima, const ImageUser &iuser, Span<SelectionTileFragment> fragments) {
        image_select_fragment_lift_source(C, &ima, iuser, fragments);
        /* Tangent-space normals are expressed in the UV frame, so they turn with the rotation. */
        bool is_normal_map = false;
        for (const ImagePaintSelectionTarget &target : this->targets) {
          if (target.image == &ima) {
            is_normal_map = target.channel == PAINT_MATERIAL_CHANNEL_NORMAL;
            break;
          }
        }
        image_select_view3d_fragments_write_final(
            C, &ima, iuser, fragments, linear, pivot, translation, is_normal_map);
      });
}

void ImageSelectTransform3DState::cancel(bContext *C)
{
  transform3d_free_draw_handle(*this);
  if (C != nullptr) {
    WM_cursor_modal_restore(CTX_wm_window(C));
  }
  if (this->mode == image_select_v3d::Transform3DContentMode::Decal) {
    /* The lift cut the source region like the UV mode's does: restore it (the canvas must come
     * out of the cancel exactly as it went in), then the stamps and the patch go away. */
    for (const int target_i : this->targets.index_range()) {
      Image *ima = this->targets[target_i].image;
      if (ima == nullptr || !image_select_view3d_image_is_alive(*this, ima)) {
        continue;
      }
      image_select_fragment_restore_source(
          C, ima, this->targets[target_i].iuser, Span<SelectionTileFragment>(this->fragments[target_i]));
      Vector<ImagePaintSelectionTarget> single = {this->targets[target_i]};
      image_paint_selection_targets_update(C, single);
    }
    transform3d_decal_release(this->decal);
    return;
  }
  for (const int target_i : this->targets.index_range()) {
    Image *ima = this->targets[target_i].image;
    if (ima == nullptr || !image_select_view3d_image_is_alive(*this, ima)) {
      continue;
    }
    image_select_fragment_restore_source(
        C, ima, this->targets[target_i].iuser, Span<SelectionTileFragment>(this->fragments[target_i]));
    Vector<ImagePaintSelectionTarget> single = {this->targets[target_i]};
    image_paint_selection_targets_update(C, single);
  }
}

bool ImageSelectTransform3DState::undo_step()
{
  if (this->drag_history.is_empty()) {
    return false;
  }
  transform3d_restore(*this, this->drag_history.pop_last());
  if (this->owner_region != nullptr) {
    ED_region_tag_redraw(this->owner_region);
  }
  return true;
}

bool ImageSelectTransform3DState::cursor_over_fragment(const bContext *C, const int2 &mval) const
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr || !this->screen_valid) {
    return false;
  }
  /* Approximate: the projected bounds quad (a rotated rectangle in screen space). Point in the
   * two edge spans is enough for delegation hit-testing. */
  const float2 m(mval);
  const float2 &a = this->screen_corners[0];
  const float2 &b = this->screen_corners[1];
  const float2 &d = this->screen_corners[3];
  const float2 ab = b - a;
  const float2 ad = d - a;
  const float denom = ab.x * ad.y - ab.y * ad.x;
  if (std::abs(denom) < 1e-9f) {
    return false;
  }
  const float2 am = m - a;
  const float u = (am.x * ad.y - am.y * ad.x) / denom;
  const float v = (ab.x * am.y - ab.y * am.x) / denom;
  return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static ImageSelectTransform3DState *transform3d_state_get()
{
  return image_select_view3d_session_get<ImageSelectTransform3DState,
                                         PaintSelectView3DTool::Transform>();
}

void image_select_transform3d_mode_changed(bContext *C, const bool want_decal)
{
  ImageSelectTransform3DState *state = transform3d_state_get();
  if (state == nullptr || C == nullptr) {
    return;
  }
  if (transform3d_mode_switch(C, *state, want_decal) && state->owner_region != nullptr) {
    ED_region_tag_redraw(state->owner_region);
  }
}

bool image_select_transform3d_projection_frame_get(const bContext *C,
                                                   projection::ProjectionFrame &r_frame)
{
  ImageSelectTransform3DState *state = transform3d_state_get();
  if (state == nullptr || C == nullptr || state->owner_v3d != CTX_wm_view3d(C)) {
    return false;
  }
  const ImageSelectTransform3DMapping mapping = transform3d_mapping_get(*state);
  if (!mapping.frames_valid) {
    return false;
  }
  r_frame = mapping.projection;
  return true;
}

bool image_select_transform3d_begin_paste(bContext *C,
                                          ReportList *reports,
                                          Vector<ImagePaintSelectionTarget> targets,
                                          Vector<Vector<SelectionTileFragment>> fragments)
{
  BLI_assert(targets.size() == fragments.size());
  ARegion *region = CTX_wm_region(C);
  /* The state owns the fragments from here on: its destructor frees them on every failure path. */
  auto *state = MEM_new<ImageSelectTransform3DState>(__func__);
  state->targets = std::move(targets);
  state->fragments = std::move(fragments);
  if (region == nullptr || CTX_wm_view3d(C) == nullptr || state->fragments.is_empty()) {
    MEM_delete(state);
    return false;
  }
  /* A live session of any tool settles first: the session slot must be empty. */
  image_select_view3d_session_end(C, true);

  /* The paste is an exec (no event): anchor at the view center. */
  if (!image_select_view3d_anchor_from_mval(
          C, int2(region->winx / 2, region->winy / 2), state->anchor) ||
      !state->anchor.jacobian_valid)
  {
    MEM_delete(state);
    BKE_report(reports, RPT_WARNING, "Selection: no surface under the view center");
    return false;
  }
  /* The fragments are #SelectionTileFragment::is_pasted: the lift only builds the outline, the
   * bounds and the pivot, it never clears canvas pixels. */
  transform3d_lift_fragments(C, *state);
  if (!state->plane_bounds_valid) {
    MEM_delete(state);
    BKE_report(reports, RPT_WARNING, "Selection: cannot resolve the paste position");
    return false;
  }
  state->hide_outline = true;
  /* The fragment sits where it was copied from; move its center onto the anchor. */
  state->uv_translation = state->anchor.anchor_uv - state->uv_pivot;
  state->display_frame_valid = false;
  for (const ImagePaintSelectionTarget &target : state->targets) {
    state->borrowed_images.append(target.image);
  }
  state->owner_region_type = region->runtime->type;
  state->draw_handle_texture = ED_region_draw_cb_activate(
      state->owner_region_type, transform3d_draw_texture, state, REGION_DRAW_POST_VIEW);
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, transform3d_draw, state, REGION_DRAW_POST_PIXEL);
  image_select_view3d_session_set(C, CTX_wm_view3d(C), state);
  state->drag_history.append(transform3d_snapshot(*state));
  ED_region_tag_redraw(region);
  return true;
}

static wmOperatorStatus transform3d_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_PASS_THROUGH;
  }
  image_paint_selection_mask_map_ensure(C);
  /* Re-drag path: pick a handle of a live transform session of this Viewport. */
  if (ImageSelectTransform3DState *floating = transform3d_state_get()) {
    if (floating->owner_v3d != CTX_wm_view3d(C) || floating->owner_region != CTX_wm_region(C)) {
      return OPERATOR_PASS_THROUGH;
    }
    floating->active_handle = transform3d_hit_test(*floating, int2(event->mval[0], event->mval[1]));
    floating->is_dragging = transform3d_begin_drag(
        C, *floating, int2(event->mval[0], event->mval[1]));
    if (floating->is_dragging) {
      WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    }
    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  /* Another tool's session may still float in this Viewport: settle it before this tool lifts its
   * own fragments (the session slot must be empty, see #image_select_view3d_session_set). */
  image_select_view3d_session_end(C, true);
  if (!image_paint_selection_view3d_poll(C)) {
    return OPERATOR_PASS_THROUGH;
  }
  auto *state = MEM_new<ImageSelectTransform3DState>(__func__);
  if (!image_select_view3d_anchor_from_mval(C, int2(event->mval[0], event->mval[1]), state->anchor)) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }
  const Vector<ImagePaintSelectionTarget> candidates = image_paint_selection_view3d_targets_get(
      C, *ob);
  bool any = false;
  for (const ImagePaintSelectionTarget &target : candidates) {
    Vector<SelectionTileFragment> fragments;
    if (!image_select_extract_per_tile(op, target.image, target.iuser, &fragments)) {
      continue;
    }
    state->fragments.append(std::move(fragments));
    state->targets.append(target);
    any = true;
  }
  if (!any) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection is empty");
    return OPERATOR_CANCELLED;
  }
  if (!image_select_fragments_have_content(state->fragments)) {
    /* Nothing to lift: the lift would clear no pixels and leave a cage around empty space. */
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection contains only transparent pixels");
    return OPERATOR_CANCELLED;
  }
  transform3d_lift_fragments(C, *state);
  /* The content mapping is resolved now that the lift fixed the cage: Decal needs the stamps and
   * the surface patch, and falls back to the UV affine when they cannot be built. */
  transform3d_mode_resolve(C, *state, op);
  if (state->mode == image_select_v3d::Transform3DContentMode::Decal &&
      !transform3d_decal_begin(C, *state, op))
  {
    state->mode = image_select_v3d::Transform3DContentMode::UV;
  }
  for (const ImagePaintSelectionTarget &target : state->targets) {
    state->borrowed_images.append(target.image);
  }
  if (ARegion *region = CTX_wm_region(C)) {
    state->owner_region_type = region->runtime->type;
    /* POST_VIEW first (the fragment pixels lie under the outline / handles drawn POST_PIXEL). */
    state->draw_handle_texture = ED_region_draw_cb_activate(
        state->owner_region_type, transform3d_draw_texture, state, REGION_DRAW_POST_VIEW);
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, transform3d_draw, state, REGION_DRAW_POST_PIXEL);
  }
  image_select_view3d_session_set(C, CTX_wm_view3d(C), state);
  state->drag_history.append(transform3d_snapshot(*state));
  /* Move-tool parity: the invoke press already has the fragment in hand -- start a move-handle
   * drag right away instead of waiting for a second click on the handles (which are not even
   * drawn until the first redraw). A press that hit a real handle picks that handle instead
   * (#transform3d_hit_test falls back to Move when the handles are not projected yet). */
  state->active_handle = transform3d_hit_test(*state, int2(event->mval[0], event->mval[1]));
  state->is_dragging = transform3d_begin_drag(C, *state, int2(event->mval[0], event->mval[1]));
  if (state->is_dragging) {
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
  }
  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus transform3d_modal(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  ImageSelectTransform3DState *state = transform3d_state_get();
  ARegion *region = CTX_wm_region(C);
  if (state == nullptr || region == nullptr || state->owner_region != region) {
    if (state != nullptr) {
      state->is_dragging = false;
      WM_cursor_modal_restore(CTX_wm_window(C));
    }
    return OPERATOR_FINISHED;
  }

  const bool navigation = ELEM(event->type,
                               MIDDLEMOUSE,
                               WHEELUPMOUSE,
                               WHEELDOWNMOUSE,
                               WHEELINMOUSE,
                               WHEELOUTMOUSE,
                               MOUSEPAN,
                               MOUSEZOOM,
                               MOUSEROTATE,
                               NDOF_MOTION);

  if (state->is_dragging) {
    if (event->type == MOUSEMOVE) {
      transform3d_apply_handle(C,
                               *state,
                               *region,
                               int2(event->mval[0], event->mval[1]),
                               (event->modifier & KM_CTRL) != 0);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      state->is_dragging = false;
      state->active_handle = Transform3DHandle::None;
      /* The pivot may have crossed onto another face during the gesture: settle the frame there
       * so the next drag starts from the surface the content is on. */
      transform3d_refresh_display_anchor(C, *state, true);
      state->drag_history.append(transform3d_snapshot(*state));
      WM_cursor_modal_restore(CTX_wm_window(C));
      /* A mode change requested during the gesture applies now that it is over. */
      image_select_view3d_pending_mode_apply(C);
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      transform3d_restore(*state, state->drag_start);
      state->is_dragging = false;
      state->active_handle = Transform3DHandle::None;
      WM_cursor_modal_restore(CTX_wm_window(C));
      image_select_view3d_pending_mode_apply(C);
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == MOUSEMOVE) {
    const Transform3DHandle hover = transform3d_hit_handle(
        *state, int2(event->mval[0], event->mval[1]));
    if (hover != state->hover_handle) {
      state->hover_handle = hover;
      ED_region_tag_redraw(region);
    }
    return OPERATOR_PASS_THROUGH;
  }
  if (navigation) {
    /* Plain PASS_THROUGH (never combined with RUNNING_MODAL, which sets WM_HANDLER_BREAK and
     * would block navigation) -- the same discipline as the Image Editor's transform modal. */
    return OPERATOR_PASS_THROUGH;
  }
  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* LMB press picks a handle and starts a drag (the op stays modal the whole session, so
     * the tool keymap cannot re-invoke it -- the 2D transform modal does the same). */
    state->active_handle = transform3d_hit_test(*state, int2(event->mval[0], event->mval[1]));
    state->is_dragging = transform3d_begin_drag(C, *state, int2(event->mval[0], event->mval[1]));
    if (state->is_dragging) {
      WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case IMAGE_SELECT_FLOATING_MODAL_CONFIRM:
        image_select_view3d_session_end(C, true);
        return OPERATOR_FINISHED;
      case IMAGE_SELECT_FLOATING_MODAL_CANCEL:
        image_select_view3d_session_end(C, false);
        return OPERATOR_FINISHED;
      case IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP: {
        if (!state->drag_history.is_empty()) {
          transform3d_restore(*state, state->drag_history.pop_last());
          ED_region_tag_redraw(region);
        }
        else {
          image_select_view3d_session_end(C, false);
          return OPERATOR_FINISHED;
        }
        break;
      }
      default:
        break;
    }
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_PASS_THROUGH;
}

void PAINT_OT_image_select_view3d_transform(wmOperatorType *ot)
{
  ot->name = "Transform Selection";
  ot->idname = "PAINT_OT_image_select_view3d_transform";
  ot->description = "Move, rotate or scale the selection on the surface";
  ot->invoke = transform3d_invoke;
  ot->modal = transform3d_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
