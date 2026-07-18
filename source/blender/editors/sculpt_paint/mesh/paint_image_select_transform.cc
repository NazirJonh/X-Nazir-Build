/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_blender.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"
#include "BKE_undo_system.hh"

#include "DEG_depsgraph.hh"

#include "BLT_translation.hh"

#include "ED_gizmo_library.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "BIF_glutil.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_intern.hh"
#include "paint_image_select_intern.hh"

namespace blender {

struct ImageSelectTransformState : public PaintSelectFloatingSession {
  /* Unlike move/warp, transform owns gizmos, so it also pins the concrete area/region the cage
   * was built for (hit-testing and gizmo refresh both need them). */
  ScrArea *owner_area = nullptr;
  ARegion *owner_region = nullptr;

  blender::Vector<SelectionTileFragment> fragments;
  blender::Vector<gpu::Texture *> fragment_textures;
  blender::Vector<gpu::Texture *> fragment_feather_textures;

  float2 uv_translation = {0.0f, 0.0f};
  float rotation = 0.0f;
  float2 scale = {1.0f, 1.0f};
  float2 uv_anchor = {0.0f, 0.0f};
  /**
   * Pivot baked into preview, cage, and commit. May differ from #uv_anchor while the user
   * repositions the anchor gizmo; synced to #uv_anchor when scale/rotate starts.
   */
  float2 uv_pivot = {0.0f, 0.0f};
  bool use_proportional_scale = true;

  /* Snapping state */
  /* True if the anchor point is magnetically snapped. */
  bool is_snapped = false;
  /* Screen coordinates of the snapped keypoint, for visual feedback. */
  float2 snap_indicator_screen = {0.0f, 0.0f};

  /* Modal interaction flags */
  enum HandleType {
    HANDLE_NONE = 0,
    HANDLE_MOVE,                 /* Translating the entire fragment */
    HANDLE_ROTATE,               /* Rotating the fragment */
    HANDLE_ANCHOR,               /* Repositioning the pivot/anchor point */
    /* Scale handles */
    HANDLE_C0, HANDLE_C1, HANDLE_C2, HANDLE_C3,       /* Corners: BL, BR, TR, TL */
    HANDLE_M_BOTTOM, HANDLE_M_RIGHT, HANDLE_M_TOP, HANDLE_M_LEFT /* Edge Midpoints */
  } active_handle = HANDLE_NONE;

  /* Screen space cache for hit detection (8 scale + 1 rotate + 1 anchor) */
  float2 screen_corners[4] = {{0.0f, 0.0f}};
  float2 screen_midpoints[4] = {{0.0f, 0.0f}};
  float2 screen_center = {0.0f, 0.0f};
  float2 screen_rotate_handle = {0.0f, 0.0f};
  float2 screen_anchor = {0.0f, 0.0f};

  /* Mouse tracking variables */
  float2 mouse_start_pos = {0.0f, 0.0f};
  float2 mouse_curr_pos = {0.0f, 0.0f};
  float2 drag_start_translation = {0.0f, 0.0f};
  float drag_start_rotation = 0.0f;
  float2 drag_start_scale = {1.0f, 1.0f};
  float2 drag_start_anchor = {0.0f, 0.0f};
  float2 drag_start_pivot = {0.0f, 0.0f};
  bool drag_start_anchor_inside_bounds = true;
  int2 prev_mouse_xy = {0, 0};
};

static int image_select_transform_ref_tile(const ImageSelectTransformState *state)
{
  return state->fragments.is_empty() ? 1001 : state->fragments[0].geom.tile_number;
}

/** View2d (global UV) -> tile-local pixel coords on the reference tile. */
static float2 image_select_view_to_tile_px(const ImageSelectTransformState *state,
                                           const float2 &view_co,
                                           const float canvas_w,
                                           const float canvas_h)
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(image_select_transform_ref_tile(state));
  return float2((view_co.x - tile_uv.x) * canvas_w, (view_co.y - tile_uv.y) * canvas_h);
}

/** Tile-local pixel coords on the reference tile -> view2d (global UV). */
static float2 image_select_tile_px_to_view(const ImageSelectTransformState *state,
                                           const float2 &tile_px,
                                           const float canvas_w,
                                           const float canvas_h)
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(image_select_transform_ref_tile(state));
  return float2(tile_uv.x + tile_px.x / canvas_w, tile_uv.y + tile_px.y / canvas_h);
}

/** UV AABB of all fragments at their current translated position (before rotation/scale). */
static bool image_select_transform_source_uv_bounds(const ImageSelectTransformState *state,
                                                    float &r_uv_x_min,
                                                    float &r_uv_y_min,
                                                    float &r_uv_x_max,
                                                    float &r_uv_y_max)
{
  r_uv_x_min = std::numeric_limits<float>::max();
  r_uv_y_min = std::numeric_limits<float>::max();
  r_uv_x_max = std::numeric_limits<float>::lowest();
  r_uv_y_max = std::numeric_limits<float>::lowest();

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }
    const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float fw = float(frag.geom.tile_size_px.x);
    const float fh = float(frag.geom.tile_size_px.y);
    const int2 ui_origin = image_select_fragment_ui_origin(frag);
    const int2 ui_size = image_select_fragment_ui_size(frag);
    const float x0 = tile_uv.x + float(ui_origin.x) / fw + state->uv_translation.x;
    const float y0 = tile_uv.y + float(ui_origin.y) / fh + state->uv_translation.y;
    const float x1 = x0 + float(ui_size.x) / fw;
    const float y1 = y0 + float(ui_size.y) / fh;
    r_uv_x_min = std::min(r_uv_x_min, x0);
    r_uv_y_min = std::min(r_uv_y_min, y0);
    r_uv_x_max = std::max(r_uv_x_max, x1);
    r_uv_y_max = std::max(r_uv_y_max, y1);
  }
  return r_uv_x_min < r_uv_x_max && r_uv_y_min < r_uv_y_max;
}

/** Scale \a uv around #ImageSelectTransformState::uv_pivot (no rotation). */
static float2 image_select_transform_uv_scaled(const ImageSelectTransformState *state,
                                               const float2 &uv)
{
  const float2 centered = uv - state->uv_pivot;
  return state->uv_pivot + centered * state->scale;
}

/** Map global UV to tile-local pixels on the tile that contains \a uv. */
static bool image_select_transform_uv_to_containing_tile_px(
    const float2 &uv,
    const blender::Span<SelectionTileFragment> &fragments,
    float2 &r_tile_px,
    float &r_canvas_w,
    float &r_canvas_h)
{
  const int col = int(floorf(uv.x));
  const int row = int(floorf(uv.y));
  if (col < 0 || col > 9 || row < 0) {
    return false;
  }
  const int tile_num = 1001 + col + row * 10;
  for (const SelectionTileFragment &frag : fragments) {
    if (frag.geom.tile_number != tile_num) {
      continue;
    }
    r_canvas_w = float(frag.geom.tile_size_px.x);
    r_canvas_h = float(frag.geom.tile_size_px.y);
    r_tile_px = float2((uv.x - float(col)) * r_canvas_w, (uv.y - float(row)) * r_canvas_h);
    return true;
  }
  return false;
}

/**
 * Forward preview/cage path: scale in UV around #uv_pivot, view2d to screen, rotate in screen
 * space around the display pivot (not #uv_anchor).
 */
static float2 image_select_transform_pivot_to_screen(const ImageSelectTransformState *state,
                                                     const ARegion *region)
{
  const float2 pivot_uv = image_select_transform_uv_scaled(state, state->uv_pivot);
  float sx, sy;
  ui::view2d_view_to_region_fl(&region->v2d, pivot_uv.x, pivot_uv.y, &sx, &sy);
  return float2(sx, sy);
}

/** Column-major 2D rotation: columns are the rotated X and Y axes. */
static float2x2 image_select_rotation_matrix_2d(const float angle_rad)
{
  const float c = cosf(angle_rad);
  const float s = sinf(angle_rad);
  return float2x2(float2(c, s), float2(-s, c));
}

static float2 image_select_transform_uv_to_screen(const ImageSelectTransformState *state,
                                                  const ARegion *region,
                                                  const float2 &uv)
{
  const float2 uv_scaled = image_select_transform_uv_scaled(state, uv);
  float sx, sy;
  ui::view2d_view_to_region_fl(&region->v2d, uv_scaled.x, uv_scaled.y, &sx, &sy);
  const float2 scr(sx, sy);
  const float2 rot_center = image_select_transform_pivot_to_screen(state, region);
  const float2 v = scr - rot_center;
  return image_select_rotation_matrix_2d(state->rotation) * v + rot_center;
}

static float2 image_select_transform_anchor_to_screen(const ImageSelectTransformState *state,
                                                    const ARegion *region)
{
  float sx, sy;
  ui::view2d_view_to_region_fl(&region->v2d, state->uv_anchor.x, state->uv_anchor.y, &sx, &sy);
  return float2(sx, sy);
}

/**
 * UV translation delta that keeps #image_select_transform_uv_scaled output unchanged when the
 * pivot moves.
 */
static float2 image_select_transform_pivot_translation_compensation(const float2 &old_pivot,
                                                                    const float2 &new_pivot,
                                                                    const float2 &scale)
{
  float2 compensation(0.0f, 0.0f);
  if (fabsf(scale.x) > 1e-6f) {
    compensation.x = (new_pivot.x - old_pivot.x) * (1.0f - 1.0f / scale.x);
  }
  if (fabsf(scale.y) > 1e-6f) {
    compensation.y = (new_pivot.y - old_pivot.y) * (1.0f - 1.0f / scale.y);
  }
  return compensation;
}

/**
 * Move the baked transform pivot while keeping the selection fixed on screen.
 * Used when syncing #uv_pivot to #uv_anchor before scale/rotate.
 */
static void image_select_transform_set_pivot_preserve_visual(ImageSelectTransformState *state,
                                                             const ARegion *region,
                                                             const float2 &new_pivot_uv)
{
  const float2 old_pivot = state->uv_pivot;
  if (math::distance_squared(old_pivot, new_pivot_uv) < 1e-12f) {
    return;
  }

  float uv_x_min, uv_y_min, uv_x_max, uv_y_max;
  if (!image_select_transform_source_uv_bounds(state, uv_x_min, uv_y_min, uv_x_max, uv_y_max)) {
    state->uv_pivot = new_pivot_uv;
    return;
  }

  const float2 uv_center_before((uv_x_min + uv_x_max) * 0.5f, (uv_y_min + uv_y_max) * 0.5f);
  const float2 screen_center_target = image_select_transform_uv_to_screen(
      state, region, uv_center_before);

  state->uv_translation += image_select_transform_pivot_translation_compensation(
      old_pivot, new_pivot_uv, state->scale);
  state->uv_pivot = new_pivot_uv;

  if (fabsf(state->rotation) < 1e-6f) {
    return;
  }

  constexpr float uv_eps = 1e-4f;
  constexpr int max_iterations = 6;
  for (int iter = 0; iter < max_iterations; iter++) {
    if (!image_select_transform_source_uv_bounds(state, uv_x_min, uv_y_min, uv_x_max, uv_y_max)) {
      break;
    }
    const float2 uv_center((uv_x_min + uv_x_max) * 0.5f, (uv_y_min + uv_y_max) * 0.5f);
    const float2 screen_center = image_select_transform_uv_to_screen(state, region, uv_center);
    const float2 err = screen_center_target - screen_center;
    if (math::length_squared(err) < 0.25f) {
      break;
    }

    const float2 dscr_dtx = (image_select_transform_uv_to_screen(
                                 state, region, uv_center + float2(uv_eps, 0.0f)) -
                             screen_center) /
                            uv_eps;
    const float2 dscr_dty = (image_select_transform_uv_to_screen(
                                 state, region, uv_center + float2(0.0f, uv_eps)) -
                             screen_center) /
                            uv_eps;

    bool ok;
    const float2x2 jacobian(dscr_dtx, dscr_dty);
    const float2 uv_delta = math::invert(jacobian, ok) * err;
    if (!ok) {
      break;
    }
    state->uv_translation += uv_delta;
  }
}

/** Bake #uv_anchor into the transform pivot before scale/rotate (keeps preview fixed). */
static void image_select_transform_sync_pivot_to_anchor(ImageSelectTransformState *state,
                                                      const ARegion *region)
{
  image_select_transform_set_pivot_preserve_visual(state, region, state->uv_anchor);
}

/** Tile-local pixel coords -> region (screen) coordinates. */
static float2 image_select_tile_px_to_screen(const ImageSelectTransformState *state,
                                             const ARegion *region,
                                             const float2 &tile_px,
                                             const float canvas_w,
                                             const float canvas_h)
{
  const float2 view_co = image_select_tile_px_to_view(state, tile_px, canvas_w, canvas_h);
  float sx, sy;
  ui::view2d_view_to_region_fl(&region->v2d, view_co.x, view_co.y, &sx, &sy);
  return float2(sx, sy);
}

/** Jacobian of tile-pixel -> screen (columns = screen delta per +1 px in X / Y). */
static float2x2 image_select_pixel_to_screen_jacobian(const ImageSelectTransformState *state,
                                                      const ARegion *region,
                                                      const float2 &tile_px,
                                                      const float canvas_w,
                                                      const float canvas_h)
{
  const float2 screen_origin = image_select_tile_px_to_screen(
      state, region, tile_px, canvas_w, canvas_h);
  const float2 screen_dx = image_select_tile_px_to_screen(
                               state, region, tile_px + float2(1.0f, 0.0f), canvas_w, canvas_h) -
                           screen_origin;
  const float2 screen_dy = image_select_tile_px_to_screen(
                               state, region, tile_px + float2(0.0f, 1.0f), canvas_w, canvas_h) -
                           screen_origin;
  return float2x2(screen_dx, screen_dy);
}

/** Pixel-space rotation matching a screen-space rotation (view2d X/Y scale may differ). */
static float2x2 image_select_rotation_pixel_from_screen(const float2x2 &pixel_to_screen_jacobian,
                                                        const float rotation_rad,
                                                        bool *r_jacobian_ok)
{
  const float2x2 rot_screen = image_select_rotation_matrix_2d(rotation_rad);
  bool ok;
  const float2x2 jacobian_inv = math::invert(pixel_to_screen_jacobian, ok);
  if (!ok) {
    *r_jacobian_ok = false;
    return rot_screen;
  }
  *r_jacobian_ok = true;
  return jacobian_inv * rot_screen * pixel_to_screen_jacobian;
}

/**
 * Same path as texture preview: scale/translate in tile px, map to screen, rotate in screen space
 * around the anchor pivot.
 */
static bool image_select_transform_screen_corners(const ImageSelectTransformState *state,
                                                  const ARegion *region,
                                                  const float /*canvas_w*/,
                                                  const float /*canvas_h*/,
                                                  float2 r_scr_corners[4],
                                                  float2 *r_scr_pivot)
{
  if (!state || !region || state->fragments.is_empty()) {
    return false;
  }

  float uv_x_min, uv_y_min, uv_x_max, uv_y_max;
  if (!image_select_transform_source_uv_bounds(state, uv_x_min, uv_y_min, uv_x_max, uv_y_max)) {
    return false;
  }

  const float2 uv_corners[4] = {
      {uv_x_min, uv_y_min},
      {uv_x_max, uv_y_min},
      {uv_x_max, uv_y_max},
      {uv_x_min, uv_y_max},
  };

  const float2 scr_pivot = image_select_transform_pivot_to_screen(state, region);

  const float2x2 rot = image_select_rotation_matrix_2d(state->rotation);
  for (int i = 0; i < 4; i++) {
    const float2 uv_scaled = image_select_transform_uv_scaled(state, uv_corners[i]);
    float sx, sy;
    ui::view2d_view_to_region_fl(&region->v2d, uv_scaled.x, uv_scaled.y, &sx, &sy);
    const float2 v = float2(sx, sy) - scr_pivot;
    r_scr_corners[i] = rot * v + scr_pivot;
  }

  if (r_scr_pivot) {
    *r_scr_pivot = scr_pivot;
  }
  return true;
}

static void image_select_transform_cache_screen_coords(ImageSelectTransformState *state,
                                                       ARegion *region)
{
  if (!state || state->fragments.is_empty() || !region) {
    return;
  }
  /* Draw callback is registered on #ARegionType; only update hit-test coords for the invoking
   * region. */
  if (state->owner_region && region != state->owner_region) {
    return;
  }

  SpaceImage *sima = state->owner_sima;
  if (!sima || !sima->image) {
    return;
  }
  void *canvas_lock = nullptr;
  ImBuf *canvas_ibuf = BKE_image_acquire_ibuf(sima->image, &state->iuser, &canvas_lock);
  if (!canvas_ibuf) {
    return;
  }
  const float canvas_w = float(canvas_ibuf->x);
  const float canvas_h = float(canvas_ibuf->y);
  BKE_image_release_ibuf(sima->image, canvas_ibuf, canvas_lock);

  float2 scr_coords[4];
  float2 scr_pivot;
  if (!image_select_transform_screen_corners(
          state, region, canvas_w, canvas_h, scr_coords, &scr_pivot))
  {
    return;
  }

  for (int i = 0; i < 4; i++) {
    state->screen_corners[i] = scr_coords[i];
  }

  state->screen_midpoints[0] = 0.5f * (scr_coords[0] + scr_coords[1]);
  state->screen_midpoints[1] = 0.5f * (scr_coords[1] + scr_coords[2]);
  state->screen_midpoints[2] = 0.5f * (scr_coords[2] + scr_coords[3]);
  state->screen_midpoints[3] = 0.5f * (scr_coords[3] + scr_coords[0]);
  state->screen_center = 0.25f * (scr_coords[0] + scr_coords[1] + scr_coords[2] + scr_coords[3]);

  const float2 top_normal = math::normalize(scr_coords[2] - scr_coords[1]);
  state->screen_rotate_handle = state->screen_midpoints[2] + top_normal * 35.0f;
  state->screen_anchor = image_select_transform_anchor_to_screen(state, region);
}

static void image_select_transform_fragment_uv_corners(const SelectionTileFragment &frag,
                                                       const ImageSelectTransformState *state,
                                                       float2 r_uv_corners[4])
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  const float tile_w = float(frag.geom.tile_size_px.x);
  const float tile_h = float(frag.geom.tile_size_px.y);

  const float uv_x0 = tile_uv.x + float(frag.geom.origin_px.x) / tile_w + state->uv_translation.x;
  const float uv_y0 = tile_uv.y + float(frag.geom.origin_px.y) / tile_h + state->uv_translation.y;
  const float uv_x1 = uv_x0 + float(frag.geom.size_px.x) / tile_w;
  const float uv_y1 = uv_y0 + float(frag.geom.size_px.y) / tile_h;

  r_uv_corners[0] = float2(uv_x0, uv_y0);
  r_uv_corners[1] = float2(uv_x1, uv_y0);
  r_uv_corners[2] = float2(uv_x1, uv_y1);
  r_uv_corners[3] = float2(uv_x0, uv_y1);
}

static void draw_select_transform_textured_quad(const float2 scr_coords[4], gpu::Texture *tex)
{
  if (!tex) {
    return;
  }

  GPUVertFormat *fmt_tex = immVertexFormat();
  const uint pos_tex = GPU_vertformat_attr_add(fmt_tex, "pos", gpu::VertAttrType::SFLOAT_32_32);
  const uint texco = GPU_vertformat_attr_add(fmt_tex, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  GPU_texture_bind(tex, 0);
  immBegin(GPU_PRIM_TRIS, 6);
  immAttr2f(texco, 0.0f, 0.0f);
  immVertex2f(pos_tex, scr_coords[0].x, scr_coords[0].y);
  immAttr2f(texco, 1.0f, 0.0f);
  immVertex2f(pos_tex, scr_coords[1].x, scr_coords[1].y);
  immAttr2f(texco, 1.0f, 1.0f);
  immVertex2f(pos_tex, scr_coords[2].x, scr_coords[2].y);
  immAttr2f(texco, 0.0f, 0.0f);
  immVertex2f(pos_tex, scr_coords[0].x, scr_coords[0].y);
  immAttr2f(texco, 1.0f, 1.0f);
  immVertex2f(pos_tex, scr_coords[2].x, scr_coords[2].y);
  immAttr2f(texco, 0.0f, 1.0f);
  immVertex2f(pos_tex, scr_coords[3].x, scr_coords[3].y);
  immEnd();
  GPU_texture_unbind(tex);
  immUnbindProgram();
}

static void draw_select_transform_texture(const bContext * /*C*/,
                                          ARegion *region,
                                          void *clientdata)
{
  ImageSelectTransformState *state = static_cast<ImageSelectTransformState *>(clientdata);
  if (!state || state->fragments.is_empty()) {
    return;
  }
  if (state->owner_region && region != state->owner_region) {
    return;
  }

  image_select_transform_cache_screen_coords(state, region);

  for (const int frag_index : state->fragments.index_range()) {
    const SelectionTileFragment &frag = state->fragments[frag_index];
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }

    float2 uv_corners[4];
    image_select_transform_fragment_uv_corners(frag, state, uv_corners);

    float2 scr_coords[4];
    for (int i = 0; i < 4; i++) {
      scr_coords[i] = image_select_transform_uv_to_screen(state, region, uv_corners[i]);
    }

    ImBuf *interior_ibuf = frag.preview.fragment_display_ibuf ?
                               frag.preview.fragment_display_ibuf :
                               frag.pixels.fragment_ibuf;

    gpu::Texture *&frag_tex = state->fragment_textures[frag_index];
    if (!frag_tex) {
      frag_tex = IMB_create_gpu_texture("TransformFragment", interior_ibuf, true, false, false);
      if (frag_tex) {
        GPU_texture_filter_mode(frag_tex, false);
      }
    }

    if (frag_tex) {
      GPU_blend(GPU_BLEND_ALPHA);
      draw_select_transform_textured_quad(scr_coords, frag_tex);
    }

    gpu::Texture *&feather_tex = state->fragment_feather_textures[frag_index];
    if (frag.preview.fragment_feather_display_ibuf && frag.edge_policy.use_outward_feather) {
      if (!feather_tex) {
        feather_tex = IMB_create_gpu_texture("TransformFragmentFeather",
                                             frag.preview.fragment_feather_display_ibuf,
                                             true,
                                             false,
                                             false);
        if (feather_tex) {
          GPU_texture_filter_mode(feather_tex, true);
        }
      }
      if (feather_tex) {
        GPU_blend(GPU_BLEND_ALPHA_PREMULT);
        draw_select_transform_textured_quad(scr_coords, feather_tex);
      }
    }
  }

  GPU_blend(GPU_BLEND_NONE);
}

static void image_select_transform_lift_source(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    return;
  }
  image_select_fragment_lift_source(
      C, sima->image, state->iuser, state->fragments, "Transform Selection", state->undo_begun);
}

static void image_select_transform_restore_source(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  Image *ima = sima ? sima->image : nullptr;
  image_select_fragment_restore_source(
      C, ima, state->iuser, state->fragments, state->undo_begun);
}


void image_select_transform_state_free(ImageSelectTransformState *state)
{
  if (!state) {
    return;
  }
  if (state->owner_region && state->owner_region->runtime->gizmo_map) {
    WM_gizmomap_tag_refresh(state->owner_region->runtime->gizmo_map);
  }
  image_select_floating_draw_handle_clear(*state);
  for (gpu::Texture *tex : state->fragment_textures) {
    if (tex) {
      GPU_texture_free(tex);
    }
  }
  state->fragment_textures.clear();
  for (gpu::Texture *tex : state->fragment_feather_textures) {
    if (tex) {
      GPU_texture_free(tex);
    }
  }
  state->fragment_feather_textures.clear();
  selection_tile_fragments_free(state->fragments);
  MEM_delete(state);
}

bool image_select_transform_is_floating(bContext *C)
{
  return image_select_transform_is_floating_in_space(CTX_wm_space_image(C));
}

static bool image_select_transform_floating_poll(bContext *C)
{
  return image_select_transform_is_floating(C);
}

static bool image_select_transform_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return false;
  }
  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;

  if (image_select_floating_state_owns(state_ref, sima)) {
    return true;
  }
  /* Switch from floating move to transform (keymap / menu) without a canvas mask. */
  if (image_select_move_is_floating_in_space(sima)) {
    return true;
  }
  if (image_select_warp_is_floating(C)) {
    return false;
  }
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  if (!sima->image) {
    return false;
  }
  if (!BKE_image_paint_selection_mask_has_any(sima->image)) {
    CTX_wm_operator_poll_msg_set(C, "No active selection mask");
    return false;
  }
  return true;
}

static void image_select_transform_cancel(bContext *C, wmOperator *op)
{
  /* Use the session state directly: with the long-lived modal, op->customdata may already
   * be stale if confirm/cancel exec ran before the modal received its final event. */
  image_select_floating_drag_end(C, nullptr);
  image_select_floating_status_clear(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->runtime) {
    ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
    if (state_ref) {
      if (state_ref->owner_area) {
        ED_area_status_text(state_ref->owner_area, nullptr);
      }
      image_select_transform_restore_source(C, state_ref);
      image_select_transform_state_free(state_ref);
      state_ref = nullptr;
    }
  }
  op->customdata = nullptr;
}

static void image_select_transform_commit(bContext *C, ImageSelectTransformState *state);

void image_select_transform_session_end_for_takeover(bContext *C, SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return;
  }
  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
  if (!state_ref) {
    return;
  }
  ImageSelectTransformState *state = state_ref;
  /* Cleared before the commit, not after: #image_select_transform_commit notifies and tags the
   * image, and anything that looks the session up again in response must not find the state being
   * torn down here. */
  state_ref = nullptr;
  if (state->owner_area) {
    ED_area_status_text(state->owner_area, nullptr);
  }
  /* A handle drag may still be in progress when another tool takes over (the long-lived modal is
   * left registered and exits on its next event, seeing a null state). Give the window its normal
   * cursor and the status bar its normal text back here. */
  image_select_floating_drag_end(C, state);
  image_select_floating_status_clear(C);
  image_select_transform_commit(C, state);
  image_select_transform_state_free(state);
  if (ARegion *region = CTX_wm_region(C)) {
    if (region->runtime->gizmo_map) {
      WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
    }
  }
}

static wmOperatorStatus image_select_transform_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = state_ref;
  ED_area_status_text(state->owner_area, nullptr);
  image_select_floating_status_clear(C);

  PointerRNA props_ptr = WM_operator_properties_create_ptr(
      WM_operatortype_find("PAINT_OT_image_select_transform", false));
  float translation[2] = {state->uv_translation.x, state->uv_translation.y};
  float scale[2] = {state->scale.x, state->scale.y};
  float uv_anchor[2] = {state->uv_anchor.x, state->uv_anchor.y};
  RNA_float_set_array(&props_ptr, "translation", translation);
  RNA_float_set(&props_ptr, "rotation", state->rotation);
  RNA_float_set_array(&props_ptr, "scale", scale);
  RNA_float_set_array(&props_ptr, "uv_anchor", uv_anchor);

  state_ref = nullptr;
  image_select_transform_restore_source(C, state);
  image_select_transform_state_free(state);
  if (ARegion *region = CTX_wm_region(C)) {
    if (region->runtime->gizmo_map) {
      WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
    }
  }

  WM_operator_name_call(
      C, "PAINT_OT_image_select_transform", wm::OpCallContext::ExecDefault, &props_ptr, nullptr);
  WM_operator_properties_free(&props_ptr);

  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_transform_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = state_ref;
  ED_area_status_text(state->owner_area, nullptr);
  image_select_floating_status_clear(C);
  state_ref = nullptr;
  image_select_transform_restore_source(C, state);
  image_select_transform_state_free(state);
  if (ARegion *region = CTX_wm_region(C)) {
    if (region->runtime->gizmo_map) {
      WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
    }
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

/** Screen-space hit radius for transform handles (cage2d-style, scales with on-screen bounds). */
static float image_select_transform_handle_hit_px(const ImageSelectTransformState *state)
{
  const float dx = math::distance(state->screen_corners[0], state->screen_corners[1]);
  const float dy = math::distance(state->screen_corners[0], state->screen_corners[3]);
  const float screen_extent = math::max(dx, dy);
  return math::max(12.0f * UI_SCALE_FAC, screen_extent * 0.12f);
}

static bool image_select_transform_point_in_handle_rect(const float2 &mouse_co,
                                                        const float2 &center,
                                                        const float half_extent)
{
  return (math::abs(mouse_co.x - center.x) <= half_extent &&
          math::abs(mouse_co.y - center.y) <= half_extent);
}

static ImageSelectTransformState::HandleType get_active_handle_hit(
    const ImageSelectTransformState *state, const float2 &mouse_co)
{
  const float hit_half = image_select_transform_handle_hit_px(state) * 0.5f;

  /* Rotation handle sits outside the quad -- test before corners/move (see cage2d_gizmo). */
  if (image_select_transform_point_in_handle_rect(
          mouse_co, state->screen_rotate_handle, hit_half))
  {
    return ImageSelectTransformState::HANDLE_ROTATE;
  }

  for (int i = 0; i < 4; i++) {
    if (image_select_transform_point_in_handle_rect(
            mouse_co, state->screen_corners[i], hit_half))
    {
      return static_cast<ImageSelectTransformState::HandleType>(
          ImageSelectTransformState::HANDLE_C0 + i);
    }
  }

  for (int i = 0; i < 4; i++) {
    if (image_select_transform_point_in_handle_rect(
            mouse_co, state->screen_midpoints[i], hit_half))
    {
      return static_cast<ImageSelectTransformState::HandleType>(
          ImageSelectTransformState::HANDLE_M_BOTTOM + i);
    }
  }

  if (image_select_transform_point_in_handle_rect(mouse_co, state->screen_anchor, hit_half)) {
    return ImageSelectTransformState::HANDLE_ANCHOR;
  }

  /* Move body: inside transformed quad only when not on a dedicated handle. */
  float c0[2] = {state->screen_corners[0].x, state->screen_corners[0].y};
  float c1[2] = {state->screen_corners[1].x, state->screen_corners[1].y};
  float c2[2] = {state->screen_corners[2].x, state->screen_corners[2].y};
  float c3[2] = {state->screen_corners[3].x, state->screen_corners[3].y};
  float m_co[2] = {mouse_co.x, mouse_co.y};
  if (isect_point_quad_v2(m_co, c0, c1, c2, c3)) {
    return ImageSelectTransformState::HANDLE_MOVE;
  }

  return ImageSelectTransformState::HANDLE_NONE;
}

static void image_select_transform_begin_handle_drag(ImageSelectTransformState *state,
                                                     const wmEvent *event,
                                                     ImageSelectTransformState::HandleType hit,
                                                     ARegion *region)
{
  if (!region) {
    region = state->owner_region;
  }
  if (region && hit != ImageSelectTransformState::HANDLE_ANCHOR &&
      hit != ImageSelectTransformState::HANDLE_MOVE &&
      hit != ImageSelectTransformState::HANDLE_NONE)
  {
    image_select_transform_sync_pivot_to_anchor(state, region);
  }

  const float2 mouse_co(event->mval[0], event->mval[1]);
  state->active_handle = hit;
  state->mouse_start_pos = mouse_co;
  state->mouse_curr_pos = mouse_co;
  state->drag_start_translation = state->uv_translation;
  state->drag_start_rotation = state->rotation;
  state->drag_start_scale = state->scale;
  state->drag_start_anchor = state->uv_anchor;
  state->drag_start_pivot = state->uv_pivot;
  float uv_x_min, uv_y_min, uv_x_max, uv_y_max;
  if (image_select_transform_source_uv_bounds(state, uv_x_min, uv_y_min, uv_x_max, uv_y_max)) {
    state->drag_start_anchor_inside_bounds =
        (state->drag_start_anchor.x >= uv_x_min - 0.001f &&
         state->drag_start_anchor.x <= uv_x_max + 0.001f &&
         state->drag_start_anchor.y >= uv_y_min - 0.001f &&
         state->drag_start_anchor.y <= uv_y_max + 0.001f);
  }
  else {
    state->drag_start_anchor_inside_bounds = true;
  }
  state->is_snapped = false;
}

static ImageSelectTransformState::HandleType image_select_transform_handle_to_internal(
    const ImageSelectTransformHandleType handle)
{
  return static_cast<ImageSelectTransformState::HandleType>(int(handle));
}

bool image_select_transform_is_floating_in_space(const SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return false;
  }
  return image_select_floating_state_owns(sima->runtime->paint_select.transform, sima);
}

ImageSelectTransformState *image_select_transform_state_get(SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return nullptr;
  }
  return sima->runtime->paint_select.transform;
}

bool ED_image_paint_select_is_transforming(SpaceImage *sima)
{
  return image_select_transform_state_get(sima) != nullptr;
}

void ED_image_paint_select_translation_get(SpaceImage *sima, float r_translation[2])
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (state) {
    copy_v2_v2(r_translation, state->uv_translation);
  }
  else {
    zero_v2(r_translation);
  }
}

void ED_image_paint_select_translation_set(SpaceImage *sima, const float translation[2])
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (state) {
    /* Move the anchor by the same delta to keep its relative position within the
     * selection rectangle, matching the behaviour of the HANDLE_MOVE mouse drag path. */
    const float2 delta = float2(translation[0] - state->uv_translation.x,
                                translation[1] - state->uv_translation.y);
    state->uv_anchor += delta;
    state->uv_pivot += delta;
    copy_v2_v2(state->uv_translation, translation);
    WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
  }
}

float ED_image_paint_select_rotation_get(SpaceImage *sima)
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  return state ? state->rotation : 0.0f;
}

void ED_image_paint_select_rotation_set(SpaceImage *sima, float rotation)
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (state) {
    state->rotation = rotation;
    WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
  }
}

void ED_image_paint_select_scale_get(SpaceImage *sima, float r_scale[2])
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (state) {
    copy_v2_v2(r_scale, state->scale);
  }
  else {
    r_scale[0] = 1.0f;
    r_scale[1] = 1.0f;
  }
}

void ED_image_paint_select_scale_set(SpaceImage *sima, const float scale[2])
{
  ImageSelectTransformState *state = image_select_transform_state_get(sima);
  if (state) {
    copy_v2_v2(state->scale, scale);
    WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
  }
}

ImageSelectTransformHandleType image_select_transform_cage_part_to_handle_type(const int cage_part)
{
  switch (cage_part) {
    case ED_GIZMO_CAGE2D_PART_TRANSLATE:
      return ImageSelectTransformHandleType::Move;
    case ED_GIZMO_CAGE2D_PART_ROTATE:
      return ImageSelectTransformHandleType::Rotate;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MIN_Y:
      return ImageSelectTransformHandleType::C0;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MIN_Y:
      return ImageSelectTransformHandleType::C1;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MAX_Y:
      return ImageSelectTransformHandleType::C2;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MAX_Y:
      return ImageSelectTransformHandleType::C3;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X:
      return ImageSelectTransformHandleType::MLeft;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X:
      return ImageSelectTransformHandleType::MRight;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_Y:
      return ImageSelectTransformHandleType::MBottom;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_Y:
      return ImageSelectTransformHandleType::MTop;
    default:
      return ImageSelectTransformHandleType::None;
  }
}

void image_select_transform_begin_drag(ImageSelectTransformState *state,
                                       const wmEvent *event,
                                       const ImageSelectTransformHandleType handle)
{
  if (!state || handle == ImageSelectTransformHandleType::None) {
    return;
  }
  image_select_transform_begin_handle_drag(
      state, event, image_select_transform_handle_to_internal(handle), nullptr);
}

void image_select_transform_end_drag(ImageSelectTransformState *state)
{
  if (!state) {
    return;
  }
  if (state->owner_area) {
    ED_area_status_text(state->owner_area, nullptr);
  }
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;
  state->is_snapped = false;
}

bool image_select_transform_has_active_handle(const ImageSelectTransformState *state)
{
  return state && state->active_handle != ImageSelectTransformState::HANDLE_NONE;
}

void image_select_transform_gizmo_refresh_tweak(const bContext *C,
                                                wmGizmo *gz_cage,
                                                wmGizmo *gz_anchor,
                                                bool *r_was_modal_tweak)
{
  if (!C || !gz_cage || !gz_anchor || !r_was_modal_tweak) {
    return;
  }

  const SpaceImage *sima = CTX_wm_space_image(C);
  ImageSelectTransformState *state = image_select_transform_state_get(
      const_cast<SpaceImage *>(sima));

  ARegion *region = state ? state->owner_region : CTX_wm_region(C);
  wmGizmoMap *gzmap = (region && region->runtime) ? region->runtime->gizmo_map : nullptr;
  wmGizmo *modal_gz = gzmap ? WM_gizmomap_get_modal(gzmap) : nullptr;
  const bool is_our_modal = modal_gz && (modal_gz == gz_cage || modal_gz == gz_anchor);

  /* GIZMOGROUP_OT_gizmo_tweak finishes on button release before custom_modal runs, so release
   * handlers in paint_select_transform_*_modal never clear header status text. */
  if (*r_was_modal_tweak && !is_our_modal && state &&
      image_select_transform_has_active_handle(state))
  {
    image_select_transform_end_drag(state);
  }
  *r_was_modal_tweak = is_our_modal;
}

void image_select_transform_apply_handle(bContext *C,
                                         ImageSelectTransformState *state,
                                         const wmEvent *event,
                                         const ImageSelectTransformHandleType handle,
                                         ARegion *region)
{
  if (!state || !event || !region || handle == ImageSelectTransformHandleType::None) {
    return;
  }

  const ImageSelectTransformState::HandleType active =
      image_select_transform_handle_to_internal(handle);
  state->active_handle = active;

  image_select_transform_cache_screen_coords(state, region);

  const float2 mouse_co(event->mval[0], event->mval[1]);
  state->mouse_curr_pos = mouse_co;

  float2 mouse_uv;
  ui::view2d_region_to_view(&region->v2d, mouse_co.x, mouse_co.y, &mouse_uv.x, &mouse_uv.y);

  float2 mouse_start_uv;
  ui::view2d_region_to_view(&region->v2d,
                            state->mouse_start_pos.x,
                            state->mouse_start_pos.y,
                            &mouse_start_uv.x,
                            &mouse_start_uv.y);

  float uv_x_min, uv_y_min, uv_x_max, uv_y_max;
  if (!image_select_transform_source_uv_bounds(state, uv_x_min, uv_y_min, uv_x_max, uv_y_max)) {
    return;
  }
  const float2 uv_size(uv_x_max - uv_x_min, uv_y_max - uv_y_min);

  if (active == ImageSelectTransformState::HANDLE_MOVE) {
    /* Translate the fragment by the mouse delta in UV space.
     * The anchor (pivot) moves with the fragment so its relative position within the
     * selection rectangle is preserved across moves.  This also ensures that the
     * anchor stays inside the bounds for subsequent drags, preventing the degenerate
     * "pivot outside bounds" path from corrupting uv_anchor / uv_translation. */
    const float2 uv_delta = mouse_uv - mouse_start_uv;
    state->uv_translation = state->drag_start_translation + uv_delta;
    state->uv_anchor = state->drag_start_anchor + uv_delta;
    state->uv_pivot = state->drag_start_pivot + uv_delta;
  }
  else if (active == ImageSelectTransformState::HANDLE_ROTATE) {
    const float2 center_scr = image_select_transform_anchor_to_screen(state, region);
    const float2 start_vec = state->mouse_start_pos - center_scr;
    const float2 curr_vec = mouse_co - center_scr;
    const float angle_start = atan2f(start_vec.y, start_vec.x);
    const float angle_curr = atan2f(curr_vec.y, curr_vec.x);
    const float delta_angle = angle_curr - angle_start;
    const float raw_angle = state->drag_start_rotation + delta_angle;
    if (event->modifier & KM_CTRL) {
      const float snap_interval = 15.0f * (float(M_PI) / 180.0f);
      state->rotation = roundf(raw_angle / snap_interval) * snap_interval;
    }
    else {
      state->rotation = raw_angle;
    }
    /* Kept ASCII in source (no literal degree sign) so the file stays 7-bit; translations are free
     * to use the real symbol. */
    char status_str[128];
    SNPRINTF_UTF8(
        status_str, IFACE_("Rotation: %.2f deg"), state->rotation * 180.0f / float(M_PI));
    ED_area_status_text(state->owner_area ? state->owner_area : CTX_wm_area(C), status_str);
  }
  else if (active == ImageSelectTransformState::HANDLE_ANCHOR) {
    float target_scr_x = mouse_co.x;
    float target_scr_y = mouse_co.y;

    const float2 snap_points_scr[9] = {state->screen_corners[0],
                                       state->screen_corners[1],
                                       state->screen_corners[2],
                                       state->screen_corners[3],
                                       state->screen_midpoints[0],
                                       state->screen_midpoints[1],
                                       state->screen_midpoints[2],
                                       state->screen_midpoints[3],
                                       state->screen_center};

    state->is_snapped = false;
    const float snap_threshold_scr = 12.0f;
    float min_dist = snap_threshold_scr;
    int snap_idx = -1;

    for (int i = 0; i < 9; i++) {
      const float dist = math::distance(snap_points_scr[i], mouse_co);
      if (dist < min_dist) {
        min_dist = dist;
        snap_idx = i;
      }
    }

    if (snap_idx >= 0) {
      target_scr_x = snap_points_scr[snap_idx].x;
      target_scr_y = snap_points_scr[snap_idx].y;
      state->is_snapped = true;
      state->snap_indicator_screen = snap_points_scr[snap_idx];
    }

    /* Reposition anchor gizmo only; preview/cage use #uv_pivot and stay fixed. */
    float2 target_anchor_uv;
    ui::view2d_region_to_view(
        &region->v2d, target_scr_x, target_scr_y, &target_anchor_uv.x, &target_anchor_uv.y);
    state->uv_anchor = target_anchor_uv;
  }
  else if (active >= ImageSelectTransformState::HANDLE_C0 &&
           active <= ImageSelectTransformState::HANDLE_M_LEFT)
  {
    float2 uv_local(0.0f, 0.0f);
    switch (active) {
      case ImageSelectTransformState::HANDLE_C0:
        uv_local = float2(0.0f, 0.0f);
        break;
      case ImageSelectTransformState::HANDLE_C1:
        uv_local = float2(uv_size.x, 0.0f);
        break;
      case ImageSelectTransformState::HANDLE_C2:
        uv_local = float2(uv_size.x, uv_size.y);
        break;
      case ImageSelectTransformState::HANDLE_C3:
        uv_local = float2(0.0f, uv_size.y);
        break;
      case ImageSelectTransformState::HANDLE_M_BOTTOM:
        uv_local = float2(uv_size.x * 0.5f, 0.0f);
        break;
      case ImageSelectTransformState::HANDLE_M_RIGHT:
        uv_local = float2(uv_size.x, uv_size.y * 0.5f);
        break;
      case ImageSelectTransformState::HANDLE_M_TOP:
        uv_local = float2(uv_size.x * 0.5f, uv_size.y);
        break;
      case ImageSelectTransformState::HANDLE_M_LEFT:
        uv_local = float2(0.0f, uv_size.y * 0.5f);
        break;
      default:
        break;
    }

    const float2 pivot_start = state->drag_start_anchor;
    const float2x2 rot_start = image_select_rotation_matrix_2d(state->drag_start_rotation);
    auto apply_forward_start_uv = [&](const float2 uv_pt) -> float2 {
      float2 centered = uv_pt - pivot_start;
      centered *= state->drag_start_scale;
      return rot_start * centered + pivot_start;
    };
    const float2 handle_start_uv = apply_forward_start_uv(float2(uv_x_min, uv_y_min) + uv_local);

    const float2 ref_anchor = state->drag_start_anchor;
    const float2 vec_curr = mouse_uv - ref_anchor;
    const float2 vec_start = handle_start_uv - ref_anchor;

    /* Columns of the start rotation are the local axes the drag is projected onto. */
    const float2 axis_x = rot_start[0];
    const float2 axis_y = rot_start[1];

    const float proj_start_x = math::dot(vec_start, axis_x);
    const float proj_start_y = math::dot(vec_start, axis_y);
    const float proj_curr_x = math::dot(vec_curr, axis_x);
    const float proj_curr_y = math::dot(vec_curr, axis_y);

    float sx = state->drag_start_scale.x;
    float sy = state->drag_start_scale.y;

    if (fabsf(proj_start_x) > 0.001f) {
      sx *= (proj_curr_x / proj_start_x);
    }
    if (fabsf(proj_start_y) > 0.001f) {
      sy *= (proj_curr_y / proj_start_y);
    }

    if (active == ImageSelectTransformState::HANDLE_M_LEFT ||
        active == ImageSelectTransformState::HANDLE_M_RIGHT)
    {
      sy = state->drag_start_scale.y;
    }
    if (active == ImageSelectTransformState::HANDLE_M_TOP ||
        active == ImageSelectTransformState::HANDLE_M_BOTTOM)
    {
      sx = state->drag_start_scale.x;
    }

    bool proportional = state->use_proportional_scale;
    if (event->modifier & KM_SHIFT) {
      proportional = !proportional;
    }
    if (proportional && (active >= ImageSelectTransformState::HANDLE_C0 &&
                         active <= ImageSelectTransformState::HANDLE_C3))
    {
      const float aspect_ratio = state->drag_start_scale.x / state->drag_start_scale.y;
      const float scale_ratio_x = sx / state->drag_start_scale.x;
      const float scale_ratio_y = sy / state->drag_start_scale.y;
      const float max_ratio = (fabsf(scale_ratio_x - 1.0f) > fabsf(scale_ratio_y - 1.0f)) ?
                                  scale_ratio_x :
                                  scale_ratio_y;
      sx = state->drag_start_scale.x * max_ratio;
      sy = sx / aspect_ratio;
    }

    if (fabsf(sx) < 0.001f) {
      sx = sx < 0.0f ? -0.001f : 0.001f;
    }
    if (fabsf(sy) < 0.001f) {
      sy = sy < 0.0f ? -0.001f : 0.001f;
    }

    state->scale = float2(sx, sy);
  }

  ED_region_tag_redraw(region);
}

bool image_select_transform_calc_gizmo_matrices(const bContext *C,
                                                const ImageSelectTransformState *state,
                                                ImageSelectTransformGizmoMatrices *r_mats)
{
  if (!state || state->fragments.is_empty() || !r_mats) {
    return false;
  }

  ARegion *region = state->owner_region ? state->owner_region : CTX_wm_region(C);
  if (!region) {
    return false;
  }

  const SpaceImage *sima = state->owner_sima;
  if (!sima || !sima->image) {
    return false;
  }

  float canvas_w = 1.0f;
  float canvas_h = 1.0f;
  void *lock = nullptr;
  ImageUser iuser = state->iuser;
  ImBuf *canvas_ibuf = BKE_image_acquire_ibuf(sima->image, &iuser, &lock);
  if (canvas_ibuf) {
    canvas_w = float(canvas_ibuf->x);
    canvas_h = float(canvas_ibuf->y);
    BKE_image_release_ibuf(sima->image, canvas_ibuf, lock);
  }

  float2 scr_corners[4];
  float2 scr_pivot;
  if (!image_select_transform_screen_corners(
          state, region, canvas_w, canvas_h, scr_corners, &scr_pivot))
  {
    return false;
  }

  /* Work entirely in region/screen pixel space.
   *
   * The preview quad is produced by a screen-space rotation, so the four corners always
   * form a true rectangle in region pixels.  Encoding the cage in the same space avoids
   * the non-uniform-zoom problem that arises when converting to view UV first:
   *   - screen rotation angle != UV rotation angle when view2d x/y scales differ,
   *   - the four view-UV corners form a parallelogram, not a rectangle, so the old
   *     R + diag(w,h) fit in UV was only exact for one corner (BL), leaving the other
   *     three displaced -- the visible "collapse" when rotating.
   *
   * With matrix_space = identity, the cage2d final matrix is:
   *   matrix_final = I * I * matrix_offset = matrix_offset
   *
   * matrix_offset is constructed so that for cage2d local coords [-0.5, 0.5]^2:
   *   col 0  (local +X)  = e0 = scr_corners[1] - scr_corners[0]  (BL->BR screen edge)
   *   col 1  (local +Y)  = e1 = scr_corners[3] - scr_corners[0]  (BL->TL screen edge)
   *   col 3  (translate) = center of the screen quad
   *
   * Proof (BL corner, local = (-0.5, -0.5)):
   *   center = BL + 0.5*e0 + 0.5*e1
   *   M * (-0.5,-0.5) + center = -0.5*e0 - 0.5*e1 + BL + 0.5*e0 + 0.5*e1 = BL  OK
   *
   * Because the corners come from a screen-space rotation, e0 is always guaranteed perpendicular
   * to e1, so the cage stays a proper rectangle for any rotation angle and any view2d aspect ratio
   * (including non-square UDIM tiles and asymmetric zoom). */
  unit_m4(r_mats->matrix_space);
  unit_m4(r_mats->matrix_basis);

  const float2 e0 = scr_corners[1] - scr_corners[0]; /* BL -> BR */
  const float2 e1 = scr_corners[3] - scr_corners[0]; /* BL -> TL */

  /* Guard against degenerate (zero-area) cage. */
  if (math::length_squared(e0) < 1e-8f || math::length_squared(e1) < 1e-8f) {
    return false;
  }

  const float2 center = 0.25f *
                        (scr_corners[0] + scr_corners[1] + scr_corners[2] + scr_corners[3]);

  /* Build matrix_offset in Blender column-major float4x4 convention (m[col][row]). */
  unit_m4(r_mats->matrix_offset);
  /* Column 0: cage +X axis maps to BL->BR screen edge. */
  r_mats->matrix_offset[0][0] = e0.x;
  r_mats->matrix_offset[0][1] = e0.y;
  /* Column 1: cage +Y axis maps to BL->TL screen edge. */
  r_mats->matrix_offset[1][0] = e1.x;
  r_mats->matrix_offset[1][1] = e1.y;
  /* Column 3: translation to the geometric centre of the quad. */
  r_mats->matrix_offset[3][0] = center.x;
  r_mats->matrix_offset[3][1] = center.y;

  /* cage_center_uv repurposed as cage_center_screen (field name kept to avoid API churn). */
  r_mats->cage_center_uv[0] = center.x;
  r_mats->cage_center_uv[1] = center.y;
  r_mats->cage_center_uv[2] = 0.0f;

  const float2 anchor_scr = image_select_transform_anchor_to_screen(state, region);
  r_mats->anchor_screen[0] = anchor_scr.x;
  r_mats->anchor_screen[1] = anchor_scr.y;
  r_mats->anchor_screen[2] = 0.0f;

  return true;
}

static void image_select_transform_commit(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima;
  if (!sima || !sima->image) {
    return;
  }
  Image *ima = sima->image;
  ImageUser iuser = state->iuser;

  if (state->fragments.is_empty()) {
    image_select_fragment_undo_push_end_if_open(state->undo_begun);
    return;
  }

  ImageUndoStep *us_open = image_select_undo_session_step_get();

  blender::Set<int> snapshotted_tiles;
  for (const SelectionTileFragment &frag_init : state->fragments) {
    snapshotted_tiles.add(frag_init.geom.tile_number);
  }

  void *lock = nullptr;

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }

    const int canvas_w = frag.geom.tile_size_px.x;
    const int canvas_h = frag.geom.tile_size_px.y;
    const bool is_float = frag.pixels.fragment_ibuf->float_buffer.data != nullptr;
    const int2 src_col_row = image_select_udim_tile_col_row(frag.geom.tile_number);
    const int src_col = src_col_row.x;
    const int src_row = src_col_row.y;

    const float2 pixel_translation = float2(
        state->uv_translation.x * float(canvas_w),
        state->uv_translation.y * float(canvas_h));
    const float2 pixel_anchor = float2(
        (state->uv_pivot.x - float(src_col)) * float(canvas_w),
        (state->uv_pivot.y - float(src_row)) * float(canvas_h));
    /* Build the forward transform matrix: fragment_local -> source_tile_pixel_space. */
    const float2 pivot_commit = pixel_anchor;

    float3x3 forward_matrix;
    bool jac_used = false;
    {
      float2x2 rot_linear = image_select_rotation_matrix_2d(state->rotation);
      ARegion *region = state->owner_region ? state->owner_region : CTX_wm_region(C);
      if (region) {
        bool jac_ok;
        const float2x2 jacobian = image_select_pixel_to_screen_jacobian(
            state, region, pivot_commit, float(canvas_w), float(canvas_h));
        rot_linear = image_select_rotation_pixel_from_screen(jacobian, state->rotation, &jac_ok);
        jac_used = jac_ok;
      }

      const float2x2 mat_scale = math::from_scale<float2x2>(state->scale);
      const float2x2 mat_linear = rot_linear * mat_scale;
      /* Match preview/gizmo order: translate in UV/pixel space, then scale+rotate around pivot.
       * p' = pivot + mat_linear * ((origin + p_local + translation) - pivot). */
      const float2 origin_translated = float2(float(frag.geom.origin_px.x),
                                              float(frag.geom.origin_px.y)) +
                                       pixel_translation;
      const float2 linear_offset = origin_translated - pivot_commit;
      const float2 trans_vec = pivot_commit + mat_linear * linear_offset;
      forward_matrix[0] = float3(mat_linear[0], 0.0f);
      forward_matrix[1] = float3(mat_linear[1], 0.0f);
      forward_matrix[2] = float3(trans_vec, 1.0f);
    }
    /* IMB_transform expects a backward matrix: dest_pixel -> fragment_local. */
    const float3x3 backward_matrix_src = math::invert(forward_matrix);

    const float2 uv_origin_src = float2(float(src_col), float(src_row));

    /* Compute the UV-space AABB of the transformed fragment by projecting all four corners
     * through the forward matrix into source-tile pixel space, then into UV space. */
    const float2 corners_local[4] = {
        {0.0f, 0.0f},
        {float(frag.geom.size_px.x), 0.0f},
        {float(frag.geom.size_px.x), float(frag.geom.size_px.y)},
        {0.0f, float(frag.geom.size_px.y)}};
    float2 uv_min = float2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    float2 uv_max = float2(std::numeric_limits<float>::lowest(),
                           std::numeric_limits<float>::lowest());
    for (const float2 &corner : corners_local) {
      const float3 src_px = math::transform_point(forward_matrix,
                                                  float3(corner.x, corner.y, 1.0f));
      const float2 uv = float2(src_px.x / float(canvas_w) + uv_origin_src.x,
                               src_px.y / float(canvas_h) + uv_origin_src.y);
      uv_min = math::min(uv_min, uv);
      uv_max = math::max(uv_max, uv);
    }

    const rctf src_crop{0.0f, float(frag.geom.size_px.x), 0.0f, float(frag.geom.size_px.y)};

    for (ImageTile *dest_tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
      const int dest_tile_num = dest_tile->tile_number;
      const int2 dst_col_row = image_select_udim_tile_col_row(dest_tile_num);
      const int dst_col = dst_col_row.x;
      const int dst_row = dst_col_row.y;

      if (uv_max.x <= float(dst_col) || uv_min.x >= float(dst_col + 1) ||
          uv_max.y <= float(dst_row) || uv_min.y >= float(dst_row + 1)) {
        continue;
      }

      iuser.tile = dest_tile_num;
      ImBuf *dest_ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
      if (!dest_ibuf) {
        continue;
      }

      /* Register the pre-state of every destination tile that differs from the source tile.
       * The source tile was already snapshotted (with its original pixels) inside
       * lift_source, so re-snapshotting it here would overwrite that with the cleared
       * (hole) state and break undo for the source tile as well. */
      if (us_open && !snapshotted_tiles.contains(dest_tile_num)) {
        ED_image_undo_push(ima, dest_ibuf, &iuser, us_open);
        ED_image_undo_capture_selection_mask(ima, dest_tile_num);
        snapshotted_tiles.add(dest_tile_num);
      }

      const int dest_w = dest_ibuf->x;
      const int dest_h = dest_ibuf->y;

      /* Build the mapping from destination-tile pixel -> source-tile pixel.
       *
       * A destination pixel (px, py) has UV: (px/dest_w + dst_col, py/dest_h + dst_row).
       * The same UV in source-tile pixel space is:
       *   src_x = px * (canvas_w / dest_w) + (dst_col - src_col) * canvas_w
       *   src_y = py * (canvas_h / dest_h) + (dst_row - src_row) * canvas_h
       *
       * This is a scale + translate (not a pure translate) when resolutions differ.
       * In column-major homogeneous 3x3: M[col][row].
       */
      const float sx = float(canvas_w) / float(dest_w);
      const float sy = float(canvas_h) / float(dest_h);
      const float tx = float(dst_col - src_col) * float(canvas_w);
      const float ty = float(dst_row - src_row) * float(canvas_h);

      float3x3 mat_dest_to_src = float3x3::identity();
      mat_dest_to_src[0] = float3(sx, 0.0f, 0.0f);
      mat_dest_to_src[1] = float3(0.0f, sy, 0.0f);
      mat_dest_to_src[2] = float3(tx, ty, 1.0f);

      /* backward_dest maps: dest_tile_px -> src_tile_px -> fragment_local. */
      const float3x3 backward_dest = backward_matrix_src * mat_dest_to_src;

      /* Work on the sub-rectangle of the destination tile the fragment can actually reach instead
       * of three tile-sized buffers per fragment per tile. `uv_min` / `uv_max` already bound the
       * transformed fragment in UV space, so converting them into this tile's pixel space gives
       * the footprint directly.
       *
       * The margin covers two things: the outward feather, which #compute_blend_mask_to_4ch
       * spreads up to `blend_radius_px + 1` pixels past the binary mask, and one pixel of slack
       * for the truncation inside IMB_transform's edge anti-aliasing. Because the region contains
       * every pixel the transformed mask can set plus that margin, the feather computed over the
       * smaller buffer is identical to the full-tile one: pixels beyond the margin have zero
       * weight anyway, so the buffer border being treated as "nothing selected" is the correct
       * answer there. */
      const int region_pad = std::max(1, frag.edge_policy.blend_radius_px + 2);
      const int reg_x0 = std::max(
          0, int(floorf((uv_min.x - float(dst_col)) * float(dest_w))) - region_pad);
      const int reg_y0 = std::max(
          0, int(floorf((uv_min.y - float(dst_row)) * float(dest_h))) - region_pad);
      const int reg_x1 = std::min(
          dest_w, int(ceilf((uv_max.x - float(dst_col)) * float(dest_w))) + region_pad);
      const int reg_y1 = std::min(
          dest_h, int(ceilf((uv_max.y - float(dst_row)) * float(dest_h))) + region_pad);
      if (reg_x1 <= reg_x0 || reg_y1 <= reg_y0) {
        BKE_image_release_ibuf(ima, dest_ibuf, lock);
        continue;
      }
      const int reg_w = reg_x1 - reg_x0;
      const int reg_h = reg_y1 - reg_y0;
      const int region_origin[2] = {reg_x0, reg_y0};

      /* IMB_transform indexes destination pixels from zero with no origin concept, so shifting the
       * destination is a pure re-indexing: folding a translation by the region origin (in
       * destination space, hence on the right) into the backward matrix reproduces the full-tile
       * result exactly within the region. Edge anti-aliasing follows along because it derives its
       * geometry from `src_crop` through the inverse of this same matrix. Integer offsets only --
       * a fractional one would change the truncation the AA rasterizer performs. */
      float3x3 mat_region_offset = float3x3::identity();
      mat_region_offset[2] = float3(float(reg_x0), float(reg_y0), 1.0f);
      const float3x3 backward_region = backward_dest * mat_region_offset;

      /* temp_pixels must match the destination buffer type -- IMB_transform does not support
       * mixed src/dst types. */
      ImBuf *temp_pixels = IMB_allocImBuf(
          reg_w, reg_h, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
      ImBuf *temp_mask = IMB_allocImBuf(reg_w, reg_h, ImBufFlags::FloatData);
      ImBuf *temp_blend_mask = IMB_allocImBuf(reg_w, reg_h, ImBufFlags::FloatData);
      if (!temp_pixels || !temp_mask || !temp_blend_mask) {
        /* Out of memory: free any partial allocations and skip this tile. */
        IMB_freeImBuf(temp_pixels);
        IMB_freeImBuf(temp_mask);
        IMB_freeImBuf(temp_blend_mask);
        BKE_image_release_ibuf(ima, dest_ibuf, lock);
        continue;
      }
      temp_pixels->channels = 4;
      IMB_rectfill_alpha(temp_pixels, 0.0f);

      /* Fill temp_mask red channel (index 0) with either transform result or full opacity. */
      float *fill_data = temp_mask->float_data_for_write();
      const int fill_count = reg_w * reg_h;
      for (int i = 0; i < fill_count; i++) {
        fill_data[i * 4 + 0] = 0.0f;
      }

      IMB_transform(frag.pixels.fragment_ibuf, temp_pixels, IMB_TRANSFORM_MODE_CROP_SRC,
                    IMB_FILTER_BILINEAR, backward_region, &src_crop);

      /* Transform the fragment selection mask into temp_mask using the same matrix and crop as the
       * pixel transform above.  The mask and pixel transforms must stay in sync: any dest pixel
       * that is NOT written to the canvas (because the pixel transform discarded it) must also
       * receive a zero mask value so global_mask does not mark it as selectable for future brush
       * strokes.
       *
       * IMB_FILTER_NEAREST is intentional here (not BILINEAR like the pixel transform).
       * IMB_transform with BILINEAR + CROP_SRC calls edge_aa() after rasterisation, which
       * multiplies boundary pixels by a sub-pixel coverage factor (typically 0.1..0.9).  That
       * turns the binary 0/1 selection mask into soft values at the boundary: pixels just inside
       * the rotated/translated region get mask ~= 0.3..0.5, falling below the 0.5 visual threshold
       * while remaining > 0.  The selection overlay shows those pixels as unselected, but the
       * paint brush still reaches them -- the user sees strokes bleeding outside the visible
       * selection boundary.  NEAREST skips edge_aa entirely, keeping global_mask strictly 0 or 1
       * so the brush constraint matches the visual selection boundary exactly. */
      if (frag.pixels.fragment_mask_ibuf) {
        IMB_transform(frag.pixels.fragment_mask_ibuf, temp_mask, IMB_TRANSFORM_MODE_CROP_SRC,
                      IMB_FILTER_NEAREST, backward_region, &src_crop);
      }
      else {
        float *mask_fill = temp_mask->float_data_for_write();
        for (int i = 0; i < fill_count; i++) {
          mask_fill[i * 4 + 0] = 1.0f;
        }
      }

      /* Outward feather from the destination-grid binary mask (extends past the selection
       * edge). */
      float *blend_fill = temp_blend_mask->float_data_for_write();
      BKE_image_paint_selection_compute_blend_mask_to_4ch(temp_mask->float_buffer.data,
                                                          reg_w,
                                                          reg_h,
                                                          blend_fill,
                                                          4,
                                                          frag.edge_policy);

      /* Alpha-blend the transformed fragment onto the destination canvas.
       * blend = mask * fragment_alpha so partially-transparent pixels are respected. */
      image_select_blend_buffer_into_canvas_at(
          dest_ibuf, temp_pixels, temp_blend_mask, region_origin);

      /* Merge the transformed selection mask into the destination tile's runtime mask. */
      ImBuf *global_mask = BKE_image_paint_selection_mask_lookup(ima, dest_tile_num);
      if (!global_mask) {
        /* If the original selection didn't touch this tile, there may be no runtime mask yet.
         * Still create one so the merged result can be inspected and subsequent ops see it. */
        global_mask = BKE_image_paint_selection_mask_get(
            ima, dest_tile_num, dest_ibuf->x, dest_ibuf->y);
      }

      /* The merge indexes the runtime mask with destination-tile strides, so it may only run when
       * the mask really has that resolution. A mask left over from a different resolution would be
       * written past its allocation; skip the merge (pixels stay committed, the mask stays stale)
       * rather than corrupting the heap. Only the region is merged: outside it the transformed
       * mask is zero, so those pixels would be unchanged anyway. */
      if (global_mask && image_select_mask_matches(global_mask, dest_ibuf)) {
        float *global_mask_data = global_mask->float_data_for_write();
        const float *add_data = temp_mask->float_buffer.data;
        for (int y = 0; y < reg_h; y++) {
          for (int x = 0; x < reg_w; x++) {
            const float add = add_data[(int64_t(y) * reg_w + x) * 4 + 0];
            float &dst = global_mask_data[int64_t(y + reg_y0) * dest_w + (x + reg_x0)];
            dst = std::clamp(dst + add, 0.0f, 1.0f);
          }
        }
        global_mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
      }

      IMB_freeImBuf(temp_pixels);
      IMB_freeImBuf(temp_mask);
      IMB_freeImBuf(temp_blend_mask);

      dest_ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(ima, dest_ibuf);
      BKE_image_release_ibuf(ima, dest_ibuf, lock);
    }
  }

  BKE_image_partial_update_mark_full_update(ima);
  BKE_image_free_gputextures(ima);

  image_select_fragment_undo_push_end_if_open(state->undo_begun);

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

static wmOperatorStatus image_select_transform_start_drag_gesture(
    bContext *C,
    wmOperator *op,
    ImageSelectTransformState *state,
    const wmEvent *event,
    ImageSelectTransformState::HandleType hit)
{
  image_select_transform_begin_drag(
      state, event, static_cast<ImageSelectTransformHandleType>(int(hit)));
  state->is_dragging = true;
  op->customdata = state;
  WM_event_add_modal_handler(C, op);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_RUNNING_MODAL;
}

/** LMB re-drag while transform is floating (short-lived modal, same as move selection). */
static wmOperatorStatus image_select_transform_drag_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *const stored_state = sima->runtime->paint_select.transform;
  if (!stored_state || stored_state->owner_sima != sima) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = stored_state;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    image_select_transform_cache_screen_coords(state, region);
  }

  const float2 mouse_co(event->mval[0], event->mval[1]);
  const ImageSelectTransformState::HandleType hit = get_active_handle_hit(state, mouse_co);
  if (hit == ImageSelectTransformState::HANDLE_NONE) {
    return OPERATOR_PASS_THROUGH;
  }

  return image_select_transform_start_drag_gesture(C, op, state, event, hit);
}

static wmOperatorStatus image_select_transform_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
  if (state_ref) {
    image_select_transform_restore_source(C, state_ref);
    image_select_transform_state_free(state_ref);
    state_ref = nullptr;
  }

  /* This exec path lifts and commits in one go, opening an undo step in between, so every other
   * tool's session has to be ended first. See #image_select_floating_sessions_end. */
  image_select_floating_sessions_end(C,
                                     sima,
                                     IMAGE_SELECT_FLOATING_TOOL_MOVE |
                                         IMAGE_SELECT_FLOATING_TOOL_GRADIENT |
                                         IMAGE_SELECT_FLOATING_TOOL_WARP);

  Image *ima = sima->image;
  ImageUser iuser = sima->iuser;
  if (ima->source == IMA_SRC_TILED) {
    /* `tiles` can legitimately be empty; mirror the null handling in
     * #image_paint_selection_resolve_tile rather than dereferencing `tiles.first` blindly. */
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&ima->tiles, ima->active_tile_index));
    if (!active) {
      active = static_cast<const ImageTile *>(ima->tiles.first);
    }
    if (active) {
      iuser.tile = active->tile_number;
    }
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(ima, &iuser);
  int tile_number = active_tile ? active_tile->tile_number : 1001;
  tile_number = image_paint_selection_resolve_tile(ima, sima, tile_number);
  iuser.tile = tile_number;

  blender::Vector<SelectionTileFragment> fragments;
  if (!image_select_extract_per_tile(op, ima, iuser, &fragments)) {
    return OPERATOR_CANCELLED;
  }

  auto *state = MEM_new<ImageSelectTransformState>(__func__);
  state->owner_sima = sima;
  state->iuser = iuser;
  state->fragments = std::move(fragments);
  state->fragment_textures.resize(state->fragments.size(), nullptr);
  state->fragment_feather_textures.resize(state->fragments.size(), nullptr);

  float uv_translation[2];
  float scale[2];
  float uv_anchor[2];
  RNA_float_get_array(op->ptr, "translation", uv_translation);
  RNA_float_get_array(op->ptr, "scale", scale);
  RNA_float_get_array(op->ptr, "uv_anchor", uv_anchor);

  state->uv_translation = float2(uv_translation[0], uv_translation[1]);
  state->scale = float2(scale[0], scale[1]);
  state->uv_anchor = float2(uv_anchor[0], uv_anchor[1]);
  state->uv_pivot = state->uv_anchor;
  state->rotation = RNA_float_get(op->ptr, "rotation");
  state->use_proportional_scale = RNA_boolean_get(op->ptr, "proportional");

  image_select_transform_lift_source(C, state);
  image_select_transform_commit(C, state);
  image_select_transform_state_free(state);

  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_transform_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;

  if (state_ref && state_ref->owner_sima == sima) {
    ImageSelectTransformState *state = state_ref;
    ARegion *region = CTX_wm_region(C);
    if (region) {
      image_select_transform_cache_screen_coords(state, region);
    }

    const float2 mouse_co(event->mval[0], event->mval[1]);
    const ImageSelectTransformState::HandleType hit = get_active_handle_hit(state, mouse_co);

    /* Re-drag via keyboard re-invoke: only start drag on handle/quad hit. */
    if (hit == ImageSelectTransformState::HANDLE_NONE) {
      return OPERATOR_PASS_THROUGH;
    }

    return image_select_transform_start_drag_gesture(C, op, state, event, hit);
  }

  /* If a transform is already active in a different space, restore it. */
  if (state_ref) {
    image_select_transform_restore_source(C, state_ref);
    image_select_transform_state_free(state_ref);
    state_ref = nullptr;
  }

  /* Gradient and warp sessions are ended here, before the move hand-over below: that path passes
   * the move's *open* undo step straight to the new transform session, so it must not be preceded
   * by anything that opens or closes a step. Move itself is therefore deliberately absent from
   * this mask and stays handled by the two branches that follow. */
  image_select_floating_sessions_end(
      C, sima, IMAGE_SELECT_FLOATING_TOOL_GRADIENT | IMAGE_SELECT_FLOATING_TOOL_WARP);

  /* Switch from floating move to transform without baking the move to the canvas. */
  ImageSelectMoveState *&move_state_ref = sima->runtime->paint_select.move;
  if (image_select_move_is_floating_in_space(sima)) {
    return image_select_move_convert_to_transform(C, op, event);
  }
  if (move_state_ref) {
    image_select_move_commit(C, move_state_ref);
    image_select_move_state_free(move_state_ref);
    move_state_ref = nullptr;
  }

  Image *ima = sima->image;

  /* For UDIM images, sima->iuser.tile is reset to 0 after each temporary acquire.
   * The true active tile is tracked by ima->active_tile_index. */
  ImageUser iuser = sima->iuser;
  if (ima->source == IMA_SRC_TILED) {
    /* `tiles` can legitimately be empty; mirror the null handling in
     * #image_paint_selection_resolve_tile rather than dereferencing `tiles.first` blindly. */
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&ima->tiles, ima->active_tile_index));
    if (!active) {
      active = static_cast<const ImageTile *>(ima->tiles.first);
    }
    if (active) {
      iuser.tile = active->tile_number;
    }
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(ima, &iuser);
  int tile_number = active_tile ? active_tile->tile_number : 1001;
  tile_number = image_paint_selection_resolve_tile(ima, sima, tile_number);
  iuser.tile = tile_number;

  /* Resolved before anything is lifted: the cage gizmos and the preview draw callback both need a
   * region, so without one the fragment would be lifted into a session the user cannot see or
   * dismiss (reachable when the operator is called from Python or a menu). */
  ARegion *region = CTX_wm_region(C);
  if (!region || !region->runtime->type) {
    return OPERATOR_CANCELLED;
  }

  auto *state = MEM_new<ImageSelectTransformState>(__func__);
  state->owner_sima = sima;
  state->iuser = iuser;
  state->prev_mouse_xy = int2{event->mval[0], event->mval[1]};
  state->mouse_start_pos = float2(event->mval[0], event->mval[1]);
  state->mouse_curr_pos = float2(event->mval[0], event->mval[1]);
  state->uv_translation = float2(0.0f, 0.0f);
  state->rotation = 0.0f;
  state->scale = float2(1.0f, 1.0f);
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;
  state->is_snapped = false;
  state->use_proportional_scale = RNA_boolean_get(op->ptr, "proportional");

  if (!image_select_extract_per_tile(op, ima, iuser, &state->fragments)) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }
  state->fragment_textures.resize(state->fragments.size(), nullptr);
  state->fragment_feather_textures.resize(state->fragments.size(), nullptr);
  state->iuser.tile = state->fragments[0].geom.tile_number;

  {
    float uv_x_min, uv_y_min, uv_x_max, uv_y_max;
    if (image_select_transform_source_uv_bounds(state, uv_x_min, uv_y_min, uv_x_max, uv_y_max)) {
      state->uv_anchor = float2((uv_x_min + uv_x_max) * 0.5f, (uv_y_min + uv_y_max) * 0.5f);
      state->uv_pivot = state->uv_anchor;
    }
    else {
      const SelectionTileFragment &frag0 = state->fragments[0];
      const float2 t0_uv = image_select_udim_tile_uv_origin(frag0.geom.tile_number);
      const float fw = float(frag0.geom.tile_size_px.x);
      const float fh = float(frag0.geom.tile_size_px.y);
      const int2 ui_origin = image_select_fragment_ui_origin(frag0);
      const int2 ui_size = image_select_fragment_ui_size(frag0);
      state->uv_anchor = float2(t0_uv.x + (float(ui_origin.x) + float(ui_size.x) * 0.5f) / fw,
                                t0_uv.y + (float(ui_origin.y) + float(ui_size.y) * 0.5f) / fh);
    }
    state->uv_pivot = state->uv_anchor;
  }

  image_select_transform_lift_source(C, state);

  state->owner_area = CTX_wm_area(C);
  state->owner_region = region;
  state->owner_region_type = region->runtime->type;

  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_transform_texture, state, REGION_DRAW_POST_VIEW);

  state_ref = state;
  op->customdata = nullptr;
  state->is_dragging = false;
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;

  if (region->runtime->gizmo_map) {
    WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
  }

  /* Long-lived modal for Enter/Esc; handle drags are owned by IMAGE_GGT_paint_select_transform. */
  ED_region_tag_redraw(region);
  WM_event_add_modal_handler(C, op);
  image_select_floating_status_set(C, op->type, true);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus image_select_transform_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
  ImageSelectTransformState *state = state_ref;
  if (!state) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ARegion *region = CTX_wm_region(C);

  if (!state->is_dragging) {
    /* Floating state: handles are owned by IMAGE_GGT_paint_select_transform gizmos.
     * Enter/Esc are handled here; LMB and other events pass through. */
    if (event->type == LEFTMOUSE) {
      return OPERATOR_PASS_THROUGH;
    }

    if (event->type == EVT_MODAL_MAP) {
      /* Confirm commits; both Cancel and Undo Step discard the floating transform. The latter is
       * deliberately not passed through to the WM: the image undo step opened at lift time is
       * discarded inside #image_select_transform_restore_source so the undo stack stays clean. */
      switch (event->val) {
        case IMAGE_SELECT_FLOATING_MODAL_CONFIRM: {
          float translation[2] = {state->uv_translation.x, state->uv_translation.y};
          float scale[2] = {state->scale.x, state->scale.y};
          float uv_anchor[2] = {state->uv_anchor.x, state->uv_anchor.y};
          RNA_float_set_array(op->ptr, "translation", translation);
          RNA_float_set(op->ptr, "rotation", state->rotation);
          RNA_float_set_array(op->ptr, "scale", scale);
          RNA_float_set_array(op->ptr, "uv_anchor", uv_anchor);

          ED_area_status_text(state->owner_area, nullptr);
          image_select_floating_status_clear(C);
          state_ref = nullptr;
          image_select_transform_commit(C, state);
          image_select_transform_state_free(state);
          op->customdata = nullptr;
          WM_event_add_notifier(C, NC_WINDOW, nullptr);
          return OPERATOR_FINISHED;
        }
        case IMAGE_SELECT_FLOATING_MODAL_CANCEL:
        case IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP: {
          ED_area_status_text(state->owner_area, nullptr);
          image_select_floating_status_clear(C);
          state_ref = nullptr;
          image_select_transform_restore_source(C, state);
          image_select_transform_state_free(state);
          op->customdata = nullptr;
          WM_event_add_notifier(C, NC_WINDOW, nullptr);
          return OPERATOR_FINISHED;
        }
        default:
          break;
      }
    }

    /* All other events (pan/zoom wheels, MOUSEMOVE, etc.).
     * Must be plain PASS_THROUGH -- combining with RUNNING_MODAL sets WM_HANDLER_BREAK
     * which prevents area/region keymap handlers (zoom, pan) from receiving the event. */
    return OPERATOR_PASS_THROUGH;
  }

  if (!region) {
    /* Lost region temporarily during drag; suspend drag, stay floating. */
    ED_area_status_text(state->owner_area, nullptr);
    state->active_handle = ImageSelectTransformState::HANDLE_NONE;
    state->is_snapped = false;
    state->is_dragging = false;
    op->customdata = nullptr;
    return OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case LEFTMOUSE: {
      if (event->val == KM_PRESS) {
        image_select_transform_cache_screen_coords(state, region);
        const float2 mouse_co(event->mval[0], event->mval[1]);
        const ImageSelectTransformState::HandleType hit = get_active_handle_hit(state, mouse_co);
        if (hit != ImageSelectTransformState::HANDLE_NONE) {
          image_select_transform_begin_drag(
              state, event, static_cast<ImageSelectTransformHandleType>(int(hit)));
          ED_region_tag_redraw(region);
        }
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->val == KM_RELEASE) {
        image_select_transform_end_drag(state);
        state->is_dragging = false;
        op->customdata = nullptr;
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    case MOUSEMOVE: {
      if (state->active_handle != ImageSelectTransformState::HANDLE_NONE) {
        image_select_transform_apply_handle(
            C,
            state,
            event,
            static_cast<ImageSelectTransformHandleType>(int(state->active_handle)),
            region);
      }
      return OPERATOR_RUNNING_MODAL;
    }

    default:
      /* During drag, unhandled events (wheel, middle-mouse pan, etc.) must not carry
       * RUNNING_MODAL -- that would set WM_HANDLER_BREAK and prevent area/region keymap
       * handlers (zoom, pan) from receiving the event. */
      return OPERATOR_PASS_THROUGH;
  }

  return OPERATOR_PASS_THROUGH;
}

wmOperatorStatus image_select_transform_adopt_move_state(
    bContext *C,
    wmOperator *op,
    const wmEvent *event,
    SpaceImage *sima,
    blender::Vector<SelectionTileFragment> &&fragments,
    const float2 uv_drag_offset,
    const int ref_tile_number,
    const ImageUser &iuser,
    const bool undo_begun,
    const bool proportional)
{
  if (!sima || !sima->runtime || fragments.is_empty()) {
    selection_tile_fragments_free(fragments);
    return OPERATOR_CANCELLED;
  }

  auto *state = MEM_new<ImageSelectTransformState>(__func__);
  state->owner_sima = sima;
  state->fragments = std::move(fragments);
  state->fragment_textures.resize(state->fragments.size(), nullptr);
  state->fragment_feather_textures.resize(state->fragments.size(), nullptr);
  state->iuser = iuser;
  state->iuser.tile = ref_tile_number;
  state->undo_begun = undo_begun;
  state->uv_translation = uv_drag_offset;
  float uv_x_min = std::numeric_limits<float>::max();
  float uv_y_min = std::numeric_limits<float>::max();
  float uv_x_max = std::numeric_limits<float>::lowest();
  float uv_y_max = std::numeric_limits<float>::lowest();
  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }
    const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float tile_w = float(frag.geom.tile_size_px.x);
    const float tile_h = float(frag.geom.tile_size_px.y);
    const int2 ui_origin = image_select_fragment_ui_origin(frag);
    const int2 ui_size = image_select_fragment_ui_size(frag);
    const float uv_x0 = tile_uv.x + float(ui_origin.x) / tile_w;
    const float uv_y0 = tile_uv.y + float(ui_origin.y) / tile_h;
    const float uv_x1 = uv_x0 + float(ui_size.x) / tile_w;
    const float uv_y1 = uv_y0 + float(ui_size.y) / tile_h;
    uv_x_min = std::min(uv_x_min, uv_x0);
    uv_y_min = std::min(uv_y_min, uv_y0);
    uv_x_max = std::max(uv_x_max, uv_x1);
    uv_y_max = std::max(uv_y_max, uv_y1);
  }
  state->uv_anchor = float2((uv_x_min + uv_x_max) * 0.5f + uv_drag_offset.x,
                            (uv_y_min + uv_y_max) * 0.5f + uv_drag_offset.y);
  state->uv_pivot = state->uv_anchor;
  state->prev_mouse_xy = int2{event->mval[0], event->mval[1]};
  state->mouse_start_pos = float2(event->mval[0], event->mval[1]);
  state->mouse_curr_pos = float2(event->mval[0], event->mval[1]);
  state->rotation = 0.0f;
  state->scale = float2(1.0f, 1.0f);
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;
  state->is_snapped = false;
  state->use_proportional_scale = proportional;

  ARegion *region = CTX_wm_region(C);
  if (!region || !region->runtime->type) {
    image_select_transform_state_free(state);
    return OPERATOR_CANCELLED;
  }

  state->owner_area = CTX_wm_area(C);
  state->owner_region = region;
  state->owner_region_type = region->runtime->type;

  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_transform_texture, state, REGION_DRAW_POST_VIEW);

  ImageSelectTransformState *&state_ref = sima->runtime->paint_select.transform;
  state_ref = state;
  op->customdata = nullptr;
  state->is_dragging = false;
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;

  if (region->runtime->gizmo_map) {
    WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
  }

  ED_region_tag_redraw(region);
  WM_event_add_modal_handler(C, op);
  image_select_floating_status_set(C, op->type, true);
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_transform(wmOperatorType *ot)
{
  ot->name = "Transform Selection";
  ot->idname = "PAINT_OT_image_select_transform";
  ot->description = "Free transform the selected region (Scale, Rotate, Move)";

  ot->invoke = image_select_transform_invoke;
  ot->exec = image_select_transform_exec;
  ot->modal = image_select_transform_modal;
  ot->cancel = image_select_transform_cancel;
  ot->poll = image_select_transform_poll;

  /* No OPTYPE_UNDO, see #PAINT_OT_image_select_move. The modal keeps running while the fragment
   * floats and returns FINISHED from paths that do not own an undo step (see the early exits in
   * #image_select_transform_modal, taken once confirm/cancel or a switch to another floating tool
   * has already destroyed the session). With OPTYPE_UNDO the WM would then push whatever step
   * happens to be open at that moment -- a step opened, and possibly already freed, by a different
   * tool -- and encode it from stale bookkeeping. */
  ot->flag = OPTYPE_REGISTER;

  RNA_def_boolean(ot->srna,
                  "proportional",
                  true,
                  "Constrain Proportions",
                  "Maintain uniform aspect ratio when scaling");
  RNA_def_float_vector(ot->srna,
                       "translation",
                       2,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Translation",
                       "Translation of the selection",
                       -1e4f,
                       1e4f);
  RNA_def_float(ot->srna,
                "rotation",
                0.0f,
                -FLT_MAX,
                FLT_MAX,
                "Rotation",
                "Rotation of the selection",
                -1e4f,
                1e4f);

  static const float default_scale[2] = {1.0f, 1.0f};
  RNA_def_float_vector(ot->srna,
                       "scale",
                       2,
                       default_scale,
                       -FLT_MAX,
                       FLT_MAX,
                       "Scale",
                       "Scale of the selection",
                       -1e4f,
                       1e4f);
  RNA_def_float_vector(ot->srna,
                       "uv_anchor",
                       2,
                       nullptr,
                       -FLT_MAX,
                       FLT_MAX,
                       "Anchor",
                       "Pivot point for rotation and scaling",
                       -1e4f,
                       1e4f);
}

void PAINT_OT_image_select_transform_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Selection Transform";
  ot->idname = "PAINT_OT_image_select_transform_confirm";
  ot->description = "Apply the transform to the canvas";

  ot->exec = image_select_transform_confirm_exec;
  ot->poll = image_select_transform_floating_poll;

  /* No OPTYPE_UNDO: commit closes the image undo step opened while the fragment was lifted.
   * See the note in #PAINT_OT_image_select_move. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_transform_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Selection Transform";
  ot->idname = "PAINT_OT_image_select_transform_cancel";
  ot->description = "Restore the fragment to its original state";

  ot->exec = image_select_transform_cancel_exec;
  ot->poll = image_select_transform_floating_poll;

  /* No OPTYPE_UNDO: restore_source closes the image undo step opened while the fragment was
   * lifted. See the note in #PAINT_OT_image_select_move. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_transform_drag(wmOperatorType *ot)
{
  ot->name = "Transform Selection Drag";
  ot->idname = "PAINT_OT_image_select_transform_drag";
  ot->description = "Drag transform handles while the selection transform is active";

  ot->invoke = image_select_transform_drag_invoke;
  ot->modal = image_select_transform_modal;
  ot->poll = image_select_transform_floating_poll;

  ot->flag = OPTYPE_REGISTER;
}

}  /* namespace blender */
