/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Transform Selection in the 3D Viewport: move / rotate / scale the floating fragments with a
 * handle overlay drawn on the surface. The transform is authored on the anchor plane (plane px,
 * like the 3D paint-shape cage) and applied in UV space through the anchor's plane->UV Jacobian,
 * so the committed result is the correctly projected transform: `dest_uv = pivot + J * R * S *
 * J^-1 * (src - pivot) + J * translation`.
 *
 * The commit resampling is the shared affine fragment writer; only the input mapping, the handle
 * interaction and the preview differ from the Image Editor's transform tool.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
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
#include "paint_image_select_view3d.hh"
#include "../mesh/sculpt_intern.hh"

#include "BKE_mesh.hh"
#include "BKE_screen.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

namespace image_select_v3d {

enum class Transform3DHandle {
  None,
  Move,
  Rotate,
  Pivot,
  Corner0,
  Corner1,
  Corner2,
  Corner3,
  /** Edge midpoints of the cage (bottom, right, top, left): scale along one axis only. */
  Edge0,
  Edge1,
  Edge2,
  Edge3,
};

/** One finished gesture's transform snapshot, for step-back. */
struct Transform3DSnapshot {
  float2 plane_translation;
  float rotation;
  float2 scale;
  float2 plane_pivot;
};

/** Map a UV point through the forward affine transform. */
inline float2 transform_uv(const float2x2 &linear,
                           const float2 &pivot_uv,
                           const float2 &translation_uv,
                           const float2 &uv)
{
  return pivot_uv + linear * (uv - pivot_uv) + translation_uv;
}

/** The plane-space linear map (rotate then scale) of the current transform. */
inline float2x2 transform_plane_linear(const float rotation, const float2 &scale)
{
  const float s = std::sin(rotation);
  const float c = std::cos(rotation);
  /* R * S: scale along the plane axes, then rotate. */
  return float2x2({c * scale.x, s * scale.x}, {-s * scale.y, c * scale.y});
}

}  // namespace image_select_v3d

struct ImageSelectTransform3DState : PaintSelectView3DFloatingSession {
  ImageSelectView3DAnchor anchor;
  Vector<ImagePaintSelectionTarget> targets;
  Vector<Vector<SelectionTileFragment>> fragments;
  /** Transform authored on the anchor plane. */
  float2 plane_translation = float2(0.0f);
  float rotation = 0.0f;
  float2 scale = float2(1.0f);
  float2 plane_pivot = float2(0.0f);
  Vector<image_select_v3d::Transform3DSnapshot> drag_history;
  ImageSelectView3DSurfaceMap surface_map;
  /** Fragment outlines in source UV space (segment pairs). */
  Vector<Vector<float2>> outline_uv;
  /** GPU copies of the fragments' display buffers for the on-surface preview, created lazily by
   * the texture draw callback (a GPU context is current there), parallel to #fragments. */
  Vector<Vector<gpu::Texture *>> preview_textures;
  /** Which target the on-surface preview shows; -1 = auto (the base-color map, else the first),
   * cycled by #preview_channel_cycle (a Material canvas lifts one fragment set per PBR map, and
   * stacking roughness / normal maps on top of each other reads as nothing). */
  int preview_target = -1;
  /** POST_VIEW draw handle of the texture preview. Two callbacks because the handles / outline
   * draw in region px (#draw_handle, POST_PIXEL) while the fragment pixels draw through the
   * view-projection matrix (this one, POST_VIEW). */
  void *draw_handle_texture = nullptr;
  /**
   * Bounds of the fragment in source plane px: the cage the handles sit on. Authored on the
   * anchor plane (like the Shapes cage) so it stays a rectangle under any UV orientation, and
   * so a handle lies exactly where the drag mapping (#image_select_view3d_mval_to_plane_px)
   * puts the cursor.
   */
  float2 plane_min = float2(0.0f);
  float2 plane_max = float2(0.0f);
  bool plane_bounds_valid = false;
  /** Screen-space handle positions, refreshed by the draw callback. */
  float2 screen_corners[4] = {float2(0.0f), float2(0.0f), float2(0.0f), float2(0.0f)};
  float2 screen_edges[4] = {float2(0.0f), float2(0.0f), float2(0.0f), float2(0.0f)};
  float2 screen_rotate = float2(0.0f);
  float2 screen_pivot = float2(0.0f);
  bool screen_valid = false;
  /** Handle under the cursor while idle, for the hover highlight. */
  image_select_v3d::Transform3DHandle hover_handle = image_select_v3d::Transform3DHandle::None;
  /** Live drag. */
  image_select_v3d::Transform3DHandle active_handle = image_select_v3d::Transform3DHandle::None;
  image_select_v3d::Transform3DSnapshot drag_start;
  int2 press_xy = int2(0);
  /** Press position on the anchor plane (plane px), captured at drag start. */
  float2 press_plane = float2(0.0f);

  explicit ImageSelectTransform3DState()
      : PaintSelectView3DFloatingSession(PaintSelectView3DTool::Transform)
  {
  }

  /** Resolve #preview_target: an explicit index while it stays valid, else the base-color map,
   * else the first target. */
  int preview_target_resolved() const
  {
    if (this->preview_target >= 0 && this->preview_target < this->targets.size()) {
      return this->preview_target;
    }
    for (const int target_i : this->targets.index_range()) {
      if (this->targets[target_i].is_color_channel) {
        return target_i;
      }
    }
    return 0;
  }

  void preview_channel_cycle() override
  {
    if (this->targets.is_empty()) {
      return;
    }
    const int current = std::max(this->preview_target_resolved(), 0);
    this->preview_target = (current + 1) % int(this->targets.size());
  }

  ~ImageSelectTransform3DState() override
  {
    image_select_view3d_preview_textures_free(this->preview_textures);
    for (Vector<SelectionTileFragment> &per_target : this->fragments) {
      selection_tile_fragments_free(per_target);
    }
    this->fragments.clear();
  }

  void commit(bContext *C) override;
  void cancel(bContext *C) override;
  bool undo_step() override;
  bool cursor_over_fragment(const bContext *C, const int2 &mval) const override;

  /* Current transform in UV space. */
  float2x2 linear_uv() const;
  float2 pivot_uv() const;
  float2 translation_uv() const;

  ImageSelectView3DSurfaceMap &surface_map_ref()
  {
    return this->surface_map;
  }
};

using image_select_v3d::Transform3DHandle;
using image_select_v3d::Transform3DSnapshot;

float2x2 ImageSelectTransform3DState::linear_uv() const
{
  if (!this->anchor.jacobian_valid) {
    return float2x2::identity();
  }
  const float2x2 plane_linear = image_select_v3d::transform_plane_linear(this->rotation,
                                                                         this->scale);
  bool invert_ok = false;
  const float2x2 jacobian_inv = math::invert(this->anchor.plane_uv_jacobian, invert_ok);
  if (!invert_ok) {
    return float2x2::identity();
  }
  return this->anchor.plane_uv_jacobian * plane_linear * jacobian_inv;
}

float2 ImageSelectTransform3DState::pivot_uv() const
{
  if (!this->anchor.jacobian_valid) {
    return float2(0.0f);
  }
  return this->anchor.anchor_uv +
         this->anchor.plane_uv_jacobian * this->plane_pivot;
}

float2 ImageSelectTransform3DState::translation_uv() const
{
  if (!this->anchor.jacobian_valid) {
    return float2(0.0f);
  }
  return this->anchor.plane_uv_jacobian * this->plane_translation;
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
  /* The cage is the fragment's bounds on the anchor plane, taken from the outline so it hugs the
   * selection however the UV island is oriented on screen (the four UV corners would map to a
   * parallelogram). The pivot starts at its center. */
  state.plane_bounds_valid = false;
  bool invert_ok = false;
  const float2x2 jacobian_inv = math::invert(state.anchor.plane_uv_jacobian, invert_ok);
  if (state.anchor.jacobian_valid && invert_ok) {
    float2 plane_lo(FLT_MAX, FLT_MAX);
    float2 plane_hi(-FLT_MAX, -FLT_MAX);
    const auto include_uv = [&](const float2 &uv) {
      const float2 p = jacobian_inv * (uv - state.anchor.anchor_uv);
      plane_lo = math::min(plane_lo, p);
      plane_hi = math::max(plane_hi, p);
    };
    for (const Vector<float2> &segments : state.outline_uv) {
      for (const float2 &uv : segments) {
        include_uv(uv);
      }
    }
    if (plane_lo.x > plane_hi.x) {
      const Bounds<float2> bounds = transform3d_source_bounds(state.fragments);
      if (bounds.min.x <= bounds.max.x) {
        include_uv(bounds.min);
        include_uv(bounds.max);
        include_uv(float2(bounds.min.x, bounds.max.y));
        include_uv(float2(bounds.max.x, bounds.min.y));
      }
    }
    if (plane_lo.x <= plane_hi.x) {
      state.plane_min = plane_lo;
      state.plane_max = plane_hi;
      state.plane_bounds_valid = true;
      state.plane_pivot = (plane_lo + plane_hi) * 0.5f;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen mapping and handles
 * \{ */

static bool transform3d_uv_to_screen(ImageSelectTransform3DState &state,
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
    /* Fallback: the anchor plane. The cage corners / rotate handle can sit outside the UV layout
     * (a fragment next to an UV island border has no triangle there), and the handles must stay
     * visible and grabbable -- #screen_valid gates the whole handle set, one missed projection
     * used to hide them all and made rotate / scale unreachable. The move tool maps its drags on
     * the same plane, so the handle position stays consistent with the drag math. */
    if (!state.anchor.jacobian_valid) {
      return false;
    }
    bool ok = false;
    const float2x2 jacobian_inv = math::invert(state.anchor.plane_uv_jacobian, ok);
    if (!ok) {
      return false;
    }
    co = state.anchor.plane_px_to_object(jacobian_inv * (uv - state.anchor.anchor_uv));
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

/** The forward plane transform of the current rotation / scale / translation about the pivot. */
static float2 transform3d_plane_forward(const ImageSelectTransform3DState &state, const float2 &p)
{
  const float2x2 linear = image_select_v3d::transform_plane_linear(state.rotation, state.scale);
  return state.plane_pivot + linear * (p - state.plane_pivot) + state.plane_translation;
}

/** Region position of a plane-px point: the exact inverse of #image_select_view3d_mval_to_plane_px,
 * so a handle always sits under the cursor that drags it. */
static bool transform3d_plane_to_screen(const ImageSelectTransform3DState &state,
                                        const ARegion &region,
                                        const float2 &plane_px,
                                        float2 &r_screen)
{
  const Object *ob = state.anchor.object;
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (ob == nullptr || rv3d == nullptr) {
    return false;
  }
  const float3 world = math::transform_point(float4x4(ob->object_to_world()),
                                             state.anchor.plane_px_to_object(plane_px));
  const float4 proj = float4x4(rv3d->persmat) * float4(world.x, world.y, world.z, 1.0f);
  if (proj.w <= 0.0f) {
    return false;
  }
  r_screen = float2((proj.x / proj.w * 0.5f + 0.5f) * float(region.winx),
                    (proj.y / proj.w * 0.5f + 0.5f) * float(region.winy));
  return true;
}

/** Distance of the rotate handle from the top edge of the cage, in region px. */
static float transform3d_rotate_offset_px()
{
  return 28.0f * UI_SCALE_FAC;
}

/** The cage (corners, edge midpoints, rotate handle, pivot) of the *transformed* fragment,
 * projected to the region. */
static void transform3d_refresh_screen(ImageSelectTransform3DState &state, const ARegion &region)
{
  state.screen_valid = false;
  if (!state.plane_bounds_valid || state.fragments.is_empty()) {
    return;
  }
  const float2 lo = state.plane_min;
  const float2 hi = state.plane_max;
  const float2 corners_src[4] = {lo, float2(hi.x, lo.y), hi, float2(lo.x, hi.y)};
  bool all = true;
  for (const int i : IndexRange(4)) {
    all = all && transform3d_plane_to_screen(
                     state, region, transform3d_plane_forward(state, corners_src[i]),
                     state.screen_corners[i]);
    const float2 edge_src = (corners_src[i] + corners_src[(i + 1) % 4]) * 0.5f;
    all = all && transform3d_plane_to_screen(
                     state, region, transform3d_plane_forward(state, edge_src),
                     state.screen_edges[i]);
  }
  /* The rotate handle stands off the top edge along the screen-space direction from the cage
   * center, so it keeps a constant distance whatever the rotation, scale or view perspective. */
  float2 center_screen;
  all = all && transform3d_plane_to_screen(
                   state, region, transform3d_plane_forward(state, (lo + hi) * 0.5f),
                   center_screen);
  if (all) {
    float2 dir = state.screen_edges[2] - center_screen;
    dir = math::length(dir) > 1e-6f ? math::normalize(dir) : float2(0.0f, 1.0f);
    state.screen_rotate = state.screen_edges[2] + dir * transform3d_rotate_offset_px();
  }
  /* Origin handle: the *transformed* pivot (rotation / scale fix the pivot, the translation
   * carries it), so the handle rides with the fragment during a move like the Image Editor's
   * anchor gizmo (#uv_anchor moves with the content there) and the Shapes origin ring. */
  all = all && transform3d_plane_to_screen(
                   state, region, state.plane_pivot + state.plane_translation,
                   state.screen_pivot);
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
/** \name Draw (POST_VIEW: fragment pixels on the surface)
 * \{ */

/**
 * The on-surface fragment preview: every fragment's display buffer drawn where the commit will
 * write it, through the current affine transform. The move preview is the identity-linear special
 * case of this; here the texture coordinates map each mesh triangle's destination UV back to the
 * fragment's source space through the inverse transform.
 */
static void transform3d_draw_texture(const bContext * /*C*/, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectTransform3DState *>(arg);
  if (state == nullptr || state->anchor.object == nullptr || !state->surface_map_ref().sampler) {
    return;
  }
  if (state->preview_textures.size() != state->fragments.size()) {
    state->preview_textures.resize(state->fragments.size());
  }
  /* A Material canvas lifts one fragment set per PBR map: drawing them all would stack the maps.
   * Show the user-cycled target, else the base-color map (what the surface looks like). */
  const int target_i = state->preview_target_resolved();
  if (target_i < 0 || target_i >= state->fragments.size()) {
    return;
  }
  /* The preview triangles lie exactly on the mesh surface: without a depth offset the shared
   * pixels z-fight and the texture shows through only in patches. */
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 1.0f);
  }
  const float2x2 linear = state->linear_uv();
  const float2 pivot = state->pivot_uv();
  const float2 translation = state->translation_uv();
  bool invert_ok = false;
  const float2x2 linear_inv = math::invert(linear, invert_ok);
  if (!invert_ok) {
    return;
  }
  const auto src_uv_of = [&](const float2 &dest_uv) {
    return pivot + linear_inv * (dest_uv - pivot - translation);
  };
  const Vector<SelectionTileFragment> &per_target = state->fragments[target_i];
  Vector<gpu::Texture *> &textures = state->preview_textures[target_i];
  if (textures.size() != per_target.size()) {
    textures.resize(per_target.size(), nullptr);
  }
  for (const int frag_i : per_target.index_range()) {
    const SelectionTileFragment &frag = per_target[frag_i];
    if (frag.preview.fragment_display_ibuf == nullptr) {
      continue;
    }
    if (textures[frag_i] == nullptr) {
      textures[frag_i] = IMB_create_gpu_texture(
          "Transform3DFragment", frag.preview.fragment_display_ibuf, true, false, false);
      if (textures[frag_i] != nullptr) {
        /* Linear, not nearest: see #move3d_draw (heavy minification on the surface). */
        GPU_texture_filter_mode(textures[frag_i], true);
      }
    }
    const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float2 tile_size = float2(frag.geom.tile_size_px);
    const float2 src_origin_uv = tile_uv + float2(frag.geom.origin_px) / tile_size;
    const float2 src_size_uv = float2(frag.geom.size_px) / tile_size;
    /* Destination rect of this fragment: its four source corners through the forward transform. */
    float2 dest_min(FLT_MAX, FLT_MAX);
    float2 dest_max(-FLT_MAX, -FLT_MAX);
    for (const int i : IndexRange(4)) {
      const float2 corner_src = src_origin_uv +
                                float2((i & 1) ? src_size_uv.x : 0.0f,
                                       (i & 2) ? src_size_uv.y : 0.0f);
      const float2 corner_dest = image_select_v3d::transform_uv(linear,
                                                                pivot,
                                                                translation,
                                                                corner_src);
      dest_min = math::min(dest_min, corner_dest);
      dest_max = math::max(dest_max, corner_dest);
    }
    image_select_view3d_draw_uv_texture_ext(*state->anchor.object,
                                            state->surface_map_ref(),
                                            textures[frag_i],
                                            dest_min,
                                            dest_max,
                                            [&](const float2 &dest_uv) {
                                              return (src_uv_of(dest_uv) - src_origin_uv) /
                                                     src_size_uv;
                                            });
  }
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 0.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw (POST_PIXEL: outline + handles)
 * \{ */

static void transform3d_draw(const bContext * /*C*/, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectTransform3DState *>(arg);
  if (state == nullptr || region == nullptr || state->anchor.object == nullptr) {
    return;
  }
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  if (rv3d == nullptr) {
    return;
  }
  transform3d_refresh_screen(*state, *region);

  /* Fragment outline (transformed), drawn in region px with the dashed shader. */
  Vector<Vector<float2>> screen_polylines;
  if (state->surface_map_ref().sampler) {
    const float2x2 linear = state->linear_uv();
    const float2 pivot = state->pivot_uv();
    const float2 translation = state->translation_uv();
    for (const Vector<float2> &outline : state->outline_uv) {
      Vector<float2> screen;
      screen.resize(outline.size());
      int drawn = 0;
      for (const int i : outline.index_range()) {
        const float2 uv = image_select_v3d::transform_uv(linear, pivot, translation, outline[i]);
        if (transform3d_uv_to_screen(*state, *region, uv, screen[i])) {
          drawn++;
        }
      }
      if (drawn >= 2) {
        screen_polylines.append(std::move(screen));
      }
    }
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_NONE);
  const uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                           gpu::VertAttrType::SFLOAT_32_32_32);

  if (!screen_polylines.is_empty()) {
    immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
    float viewport_size[4];
    GPU_viewport_size_get_f(viewport_size);
    immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
    immUniform1i("colors_len", 2);
    immUniform4f("color", 1.0f, 0.85f, 0.0f, 0.9f);
    immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
    immUniform1f("dash_width", 8.0f);
    immUniform1f("udash_factor", 0.5f);
    /* The inputs are segment pairs (#image_select_mask_boundary_segments), not connected
     * polylines: chaining consecutive points joins unrelated boundary segments with long lines
     * across the fragment (the move tool's outlines go through the same contract). */
    int64_t total = 0;
    for (const Vector<float2> &poly : screen_polylines) {
      total += int64_t(poly.size());
    }
    immBegin(GPU_PRIM_LINES, uint(total));
    for (const Vector<float2> &poly : screen_polylines) {
      for (int64_t i = 0; i + 1 < poly.size(); i += 2) {
        immVertex3f(pos, poly[i].x, poly[i].y, 0.0f);
        immVertex3f(pos, poly[i + 1].x, poly[i + 1].y, 0.0f);
      }
    }
    immEnd();
    immUnbindProgram();
  }

  /* Handles: the cage, corner / edge squares, the rotate handle on a stem and the pivot ring. */
  if (state->screen_valid) {
    const float s = 5.0f * UI_SCALE_FAC;
    const Transform3DHandle emphasized = state->is_dragging ? state->active_handle :
                                                              state->hover_handle;
    const float4 fill_color(1.0f, 1.0f, 1.0f, 0.95f);
    const float4 fill_color_hot(1.0f, 0.65f, 0.1f, 1.0f);
    const float4 edge_color(0.05f, 0.05f, 0.05f, 0.9f);

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    /* Cage outline, a dark underlay one pixel off keeps it readable on any surface color. */
    const auto draw_cage = [&](const float2 &offset) {
      immBegin(GPU_PRIM_LINE_LOOP, 4);
      for (const int i : IndexRange(4)) {
        immVertex3f(pos,
                    state->screen_corners[i].x + offset.x,
                    state->screen_corners[i].y + offset.y,
                    0.0f);
      }
      immEnd();
    };
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.45f);
    draw_cage(float2(1.0f, -1.0f));
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    draw_cage(float2(0.0f));

    /* Stem between the top edge and the rotate handle. */
    immBegin(GPU_PRIM_LINES, 2);
    immVertex3f(pos, state->screen_edges[2].x, state->screen_edges[2].y, 0.0f);
    immVertex3f(pos, state->screen_rotate.x, state->screen_rotate.y, 0.0f);
    immEnd();

    const auto draw_square = [&](const float2 &p, const float half, const bool hot) {
      immUniformColor4fv(hot ? fill_color_hot : fill_color);
      immBegin(GPU_PRIM_TRI_FAN, 4);
      immVertex3f(pos, p.x - half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y + half, 0.0f);
      immVertex3f(pos, p.x - half, p.y + half, 0.0f);
      immEnd();
      immUniformColor4fv(edge_color);
      immBegin(GPU_PRIM_LINE_LOOP, 4);
      immVertex3f(pos, p.x - half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y + half, 0.0f);
      immVertex3f(pos, p.x - half, p.y + half, 0.0f);
      immEnd();
    };
    constexpr int CIRCLE_SEGMENTS = 20;
    const auto draw_circle = [&](const float2 &p,
                                 const float radius,
                                 const bool filled,
                                 const float4 &color) {
      if (filled) {
        immUniformColor4fv(color);
        immBegin(GPU_PRIM_TRI_FAN, CIRCLE_SEGMENTS + 2);
        immVertex3f(pos, p.x, p.y, 0.0f);
        for (const int i : IndexRange(CIRCLE_SEGMENTS + 1)) {
          const float angle = float(i) / float(CIRCLE_SEGMENTS) * float(2.0 * M_PI);
          immVertex3f(pos, p.x + std::cos(angle) * radius, p.y + std::sin(angle) * radius, 0.0f);
        }
        immEnd();
      }
      immUniformColor4fv(filled ? edge_color : color);
      immBegin(GPU_PRIM_LINE_LOOP, CIRCLE_SEGMENTS);
      for (const int i : IndexRange(CIRCLE_SEGMENTS)) {
        const float angle = float(i) / float(CIRCLE_SEGMENTS) * float(2.0 * M_PI);
        immVertex3f(pos, p.x + std::cos(angle) * radius, p.y + std::sin(angle) * radius, 0.0f);
      }
      immEnd();
    };

    for (const int i : IndexRange(4)) {
      draw_square(state->screen_edges[i],
                  s * 0.8f,
                  emphasized == Transform3DHandle(int(Transform3DHandle::Edge0) + i));
    }
    for (const int i : IndexRange(4)) {
      draw_square(state->screen_corners[i],
                  s,
                  emphasized == Transform3DHandle(int(Transform3DHandle::Corner0) + i));
    }
    draw_circle(state->screen_rotate,
                s * 1.2f,
                true,
                emphasized == Transform3DHandle::Rotate ? fill_color_hot : fill_color);

    /* Pivot: a ring with a cross, blue so it never reads as a scale handle. */
    const float4 pivot_color = emphasized == Transform3DHandle::Pivot ?
                                   fill_color_hot :
                                   float4(0.2f, 0.6f, 1.0f, 1.0f);
    draw_circle(state->screen_pivot, s * 1.2f, false, pivot_color);
    immUniformColor4fv(pivot_color);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex3f(pos, state->screen_pivot.x - s * 0.7f, state->screen_pivot.y, 0.0f);
    immVertex3f(pos, state->screen_pivot.x + s * 0.7f, state->screen_pivot.y, 0.0f);
    immVertex3f(pos, state->screen_pivot.x, state->screen_pivot.y - s * 0.7f, 0.0f);
    immVertex3f(pos, state->screen_pivot.x, state->screen_pivot.y + s * 0.7f, 0.0f);
    immEnd();
    immUnbindProgram();
  }

  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag interaction
 * \{ */

static Transform3DSnapshot transform3d_snapshot(const ImageSelectTransform3DState &state)
{
  return Transform3DSnapshot{state.plane_translation, state.rotation, state.scale,
                             state.plane_pivot};
}

static void transform3d_restore(ImageSelectTransform3DState &state,
                                const Transform3DSnapshot &snap)
{
  state.plane_translation = snap.plane_translation;
  state.rotation = snap.rotation;
  state.scale = snap.scale;
  state.plane_pivot = snap.plane_pivot;
}

static void transform3d_apply_handle(ImageSelectTransform3DState &state,
                                     const ARegion &region,
                                     const int2 &mval,
                                     const bool ctrl)
{
  float2 plane_now;
  if (!image_select_view3d_mval_to_plane_px(state.anchor, region, mval, plane_now)) {
    return;
  }
  switch (state.active_handle) {
    case Transform3DHandle::Move:
      state.plane_translation = state.drag_start.plane_translation + (plane_now - state.press_plane);
      break;
    case Transform3DHandle::Rotate: {
      const float2 a = plane_now - state.plane_pivot;
      const float2 b = state.press_plane - state.plane_pivot;
      if (math::length(a) > 1e-6f && math::length(b) > 1e-6f) {
        float angle = std::atan2(a.y, a.x) - std::atan2(b.y, b.x);
        /* Ctrl snaps to 15 degree steps. */
        if (ctrl) {
          angle = std::floor(angle / (M_PI / 12.0f) + 0.5f) * (M_PI / 12.0f);
        }
        state.rotation = state.drag_start.rotation + angle;
      }
      break;
    }
    case Transform3DHandle::Pivot: {
      /* The handle rides at the transformed pivot (plane_pivot + plane_translation, see
       * #transform3d_refresh_screen); dragging it re-anchors the pivot under the mouse while the
       * already-applied transform stays visually in place:
       *   P' + L * (p - P') + T' == P + L * (p - P) + T   for every p
       *   =>  T' = T + (I - L) * (P - P')
       * which lands the handle exactly on \a plane_now through
       *   P' = P + L^-1 * (plane_now - (P + T)).
       * (The Image Editor reaches the same end state by moving its independent anchor marker and
       * syncing the baked pivot via #image_select_transform_set_pivot_preserve_visual.) */
      const float2x2 plane_linear = image_select_v3d::transform_plane_linear(state.rotation,
                                                                            state.scale);
      bool invert_ok = false;
      const float2x2 plane_linear_inv = math::invert(plane_linear, invert_ok);
      if (!invert_ok) {
        break;
      }
      const float2 pivot_old = state.plane_pivot;
      const float2 pivot_new = pivot_old + plane_linear_inv *
                                                   (plane_now -
                                                    (pivot_old + state.plane_translation));
      const float2 pivot_delta = pivot_old - pivot_new;
      state.plane_translation = state.plane_translation + pivot_delta -
                                plane_linear * pivot_delta;
      state.plane_pivot = pivot_new;
      break;
    }
    default: {
      /* Per-axis scale along the *start* rotation's axes, in plane space. */
      const float s0 = std::sin(state.drag_start.rotation);
      const float c0 = std::cos(state.drag_start.rotation);
      const float2 ex0(c0, s0);
      const float2 ey0(-s0, c0);
      const float2 q0 = state.press_plane - state.plane_pivot;
      const float2 q1 = plane_now - state.plane_pivot;
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
        break;
      }
      float2 new_scale = state.drag_start.scale;
      if (scale_x) {
        new_scale.x = state.drag_start.scale.x * (math::dot(q1, ex0) / denom_x);
      }
      if (scale_y) {
        new_scale.y = state.drag_start.scale.y * (math::dot(q1, ey0) / denom_y);
      }
      new_scale.x = std::clamp(new_scale.x, -1000.0f, 1000.0f);
      new_scale.y = std::clamp(new_scale.y, -1000.0f, 1000.0f);
      if (std::abs(new_scale.x) < 0.001f) {
        new_scale.x = 0.001f * (new_scale.x < 0.0f ? -1.0f : 1.0f);
      }
      if (std::abs(new_scale.y) < 0.001f) {
        new_scale.y = 0.001f * (new_scale.y < 0.0f ? -1.0f : 1.0f);
      }
      state.scale = new_scale;
      break;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifecycle
 * \{ */

void ImageSelectTransform3DState::commit(bContext *C)
{
  transform3d_free_draw_handle(*this);
  WM_cursor_modal_restore(CTX_wm_window(C));
  const float2x2 linear = this->linear_uv();
  const float2 pivot = this->pivot_uv();
  const float2 translation = this->translation_uv();

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

static wmOperatorStatus transform3d_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_PASS_THROUGH;
  }
  /* Re-drag path: pick a handle of a live transform session of this Viewport. */
  if (ImageSelectTransform3DState *floating = transform3d_state_get()) {
    if (floating->owner_v3d != CTX_wm_view3d(C) || floating->owner_region != CTX_wm_region(C)) {
      return OPERATOR_PASS_THROUGH;
    }
    floating->active_handle = transform3d_hit_test(*floating, int2(event->mval[0], event->mval[1]));
    floating->drag_start = transform3d_snapshot(*floating);
    floating->press_xy = int2(event->mval[0], event->mval[1]);
    float2 plane_press;
    if (image_select_view3d_mval_to_plane_px(floating->anchor,
                                             *CTX_wm_region(C),
                                             floating->press_xy,
                                             plane_press))
    {
      floating->press_plane = plane_press;
    }
    floating->is_dragging = true;
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
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
  transform3d_lift_fragments(C, *state);
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
  state->drag_start = transform3d_snapshot(*state);
  state->press_xy = int2(event->mval[0], event->mval[1]);
  if (image_select_view3d_mval_to_plane_px(
          state->anchor, *CTX_wm_region(C), state->press_xy, state->press_plane))
  {
    state->is_dragging = true;
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
      transform3d_apply_handle(*state,
                               *region,
                               int2(event->mval[0], event->mval[1]),
                               (event->modifier & KM_CTRL) != 0);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      state->is_dragging = false;
      state->active_handle = Transform3DHandle::None;
      state->drag_history.append(transform3d_snapshot(*state));
      WM_cursor_modal_restore(CTX_wm_window(C));
      ED_region_tag_redraw(region);
      return OPERATOR_FINISHED;
    }
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      transform3d_restore(*state, state->drag_start);
      state->is_dragging = false;
      state->active_handle = Transform3DHandle::None;
      WM_cursor_modal_restore(CTX_wm_window(C));
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
    state->drag_start = transform3d_snapshot(*state);
    state->press_xy = int2(event->mval[0], event->mval[1]);
    if (image_select_view3d_mval_to_plane_px(
            state->anchor, *region, state->press_xy, state->press_plane))
    {
      state->is_dragging = true;
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
