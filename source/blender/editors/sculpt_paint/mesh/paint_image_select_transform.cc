/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_blender.hh"
#include "BKE_image.hh"
#include "BKE_library.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "ED_gizmo_library.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "BKE_undo_system.hh"

#include "BIF_glutil.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_geom.h"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "../../space_image/image_runtime.hh"
#include "paint_image_select_intern.hh"


namespace blender {

struct ImageSelectTransformState {
  /* Context & target details */
  SpaceImage *owner_sima = nullptr;
  ScrArea *owner_area = nullptr;
  ARegion *owner_region = nullptr;
  ARegionType *owner_region_type = nullptr;
  ImageUser iuser = {};
  int tile_number = 1001;
  bool undo_begun = false;

  /* Image buffers */
  ImBuf *fragment_ibuf = nullptr;          /* Extracted source pixel data */
  ImBuf *fragment_mask_ibuf = nullptr;     /* Extracted float selection mask (0.0 to 1.0) */
  /* RGBA float preview with mask baked into alpha (lasso/circle); null for full-rect masks. */
  ImBuf *fragment_display_ibuf = nullptr;
  gpu::Texture *fragment_tex = nullptr;    /* GPU texture uploaded for live preview */

  /* Bounding box dimensions at lift time (in tile pixels) */
  int2 origin_px = {0, 0};
  int2 size_px = {0, 0};

  /* Transform parameters */
  float2 translation = {0.0f, 0.0f};       /* Offset in tile pixels, initially (0, 0) */
  float rotation = 0.0f;                   /* Rotation in radians, initially 0.0f */
  float2 scale = {1.0f, 1.0f};             /* Scale factors, initially (1.0f, 1.0f) */
  float2 anchor = {0.0f, 0.0f};            /* Global pivot coordinates in tile pixels */
  bool use_proportional_scale = true;      /* Proportional X/Y scale toggle */

  /* Snapping state */
  bool is_snapped = false;                 /* True if the anchor point is magnetically snapped */
  float2 snap_indicator_screen = {0.0f, 0.0f};  /* Screen coordinates of snapped keypoint for visual feedback */

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
  int2 prev_mouse_xy = {0, 0};
  bool is_dragging = false;

  /* Handle returned by ED_region_draw_cb_activate, removed on exit. */
  void *draw_handle = nullptr;
};

/** Tile UV origin for UDIM (tile 1001 -> (0, 0), tile 1012 -> (1, 1), etc.). */
static float2 image_select_tile_uv_origin(const int tile_number)
{
  return float2(float((tile_number - 1001) % 10), float((tile_number - 1001) / 10));
}

/** View2d (global UV) -> tile-local pixel coords; same convention as #image_select_move. */
static float2 image_select_view_to_tile_px(const ImageSelectTransformState *state,
                                           const float2 &view_co,
                                           const float canvas_w,
                                           const float canvas_h)
{
  const float2 tile_uv = image_select_tile_uv_origin(state->tile_number);
  return float2((view_co.x - tile_uv.x) * canvas_w, (view_co.y - tile_uv.y) * canvas_h);
}

/** Tile-local pixel coords -> view2d (global UV). */
static float2 image_select_tile_px_to_view(const ImageSelectTransformState *state,
                                           const float2 &tile_px,
                                           const float canvas_w,
                                           const float canvas_h)
{
  const float2 tile_uv = image_select_tile_uv_origin(state->tile_number);
  return float2(tile_uv.x + tile_px.x / canvas_w, tile_uv.y + tile_px.y / canvas_h);
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

static float2x2 image_select_rotation_matrix_2d(const float angle_rad)
{
  const float c = cosf(angle_rad);
  const float s = sinf(angle_rad);
  return float2x2(float2(c, s), float2(-s, c));
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
  const float2x2 R_screen = image_select_rotation_matrix_2d(rotation_rad);
  bool ok;
  const float2x2 J_inv = math::invert(pixel_to_screen_jacobian, ok);
  if (!ok) {
    *r_jacobian_ok = false;
    return R_screen;
  }
  *r_jacobian_ok = true;
  return J_inv * R_screen * pixel_to_screen_jacobian;
}

/**
 * Same path as texture preview: scale/translate in tile px, map to screen, rotate in screen space
 * around the anchor pivot.
 */
static bool image_select_transform_screen_corners(const ImageSelectTransformState *state,
                                                  const ARegion *region,
                                                  const float canvas_w,
                                                  const float canvas_h,
                                                  float2 r_scr_corners[4],
                                                  float2 *r_scr_pivot)
{
  if (!state || !region) {
    return false;
  }

  const float2 pivot = state->anchor;

  auto apply_forward_no_rotation = [&](const float2 local) -> float2 {
    const float2 global = float2(state->origin_px) + local;
    const float2 centered = global - pivot;
    const float2 scaled = centered * state->scale;
    return scaled + pivot + state->translation;
  };

  const float2 local_corners[4] = {
      float2(0.0f, 0.0f),
      float2(state->size_px.x, 0.0f),
      float2(state->size_px.x, state->size_px.y),
      float2(0.0f, state->size_px.y),
  };

  float2 scr_base[4];
  for (int i = 0; i < 4; i++) {
    const float2 tile_co = apply_forward_no_rotation(local_corners[i]);
    scr_base[i] = image_select_tile_px_to_screen(state, region, tile_co, canvas_w, canvas_h);
  }

  const float2 local_anchor = state->anchor - float2(state->origin_px);
  const float2 scr_pivot = image_select_tile_px_to_screen(
      state, region, apply_forward_no_rotation(local_anchor), canvas_w, canvas_h);

  const float cos_r = cosf(state->rotation);
  const float sin_r = sinf(state->rotation);
  for (int i = 0; i < 4; i++) {
    const float2 v = scr_base[i] - scr_pivot;
    r_scr_corners[i] = float2(v.x * cos_r - v.y * sin_r, v.x * sin_r + v.y * cos_r) + scr_pivot;
  }

  if (r_scr_pivot) {
    *r_scr_pivot = scr_pivot;
  }
  return true;
}

static void image_select_transform_cache_screen_coords(ImageSelectTransformState *state, ARegion *region)
{
  if (!state || !state->fragment_ibuf || !region) {
    return;
  }
  /* Draw callback is registered on #ARegionType; only update hit-test coords for the invoking region. */
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
  state->screen_anchor = scr_pivot;
}

static void draw_select_transform_texture(const bContext * /*C*/, ARegion *region, void *clientdata)
{
  ImageSelectTransformState *state = static_cast<ImageSelectTransformState *>(clientdata);
  if (!state || !state->fragment_ibuf) {
    return;
  }
  if (state->owner_region && region != state->owner_region) {
    return;
  }

  image_select_transform_cache_screen_coords(state, region);

  float2 scr_coords[4];
  for (int i = 0; i < 4; i++) {
    scr_coords[i] = state->screen_corners[i];
  }

  GPU_blend(GPU_BLEND_ALPHA);

  /* --- 1. Draw Textured Fragment Preview --- */
  ImBuf *frag_for_gpu = state->fragment_display_ibuf ? state->fragment_display_ibuf :
                                                       state->fragment_ibuf;
  if (!state->fragment_tex) {
    state->fragment_tex = IMB_create_gpu_texture(
        "TransformFragment", frag_for_gpu, true, true, false);
  }

  if (state->fragment_tex) {
    GPUVertFormat *fmt_tex = immVertexFormat();
    const uint pos_tex = GPU_vertformat_attr_add(fmt_tex, "pos", gpu::VertAttrType::SFLOAT_32_32);
    const uint texco = GPU_vertformat_attr_add(fmt_tex, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
    GPU_texture_bind(state->fragment_tex, 0);

    /* TRI_FAN as two triangles: BL-BR-TR and BL-TR-TL */
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

    GPU_texture_unbind(state->fragment_tex);
    immUnbindProgram();
  }

  GPU_blend(GPU_BLEND_NONE);
}

static void image_select_transform_lift_source(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima) {
    return;
  }
  Image *ima = sima->image;
  ImageUser iuser = state->iuser;

  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
    if (ibuf) {
      BKE_image_release_ibuf(ima, ibuf, lock);
    }
    return;
  }

  /* Capture pre-lift state for undo before any modification. */
  ED_imapaint_clear_partial_redraw();
  ED_image_undo_push_begin_with_image("Transform Selection", ima, ibuf, &iuser);
  /* Also snapshot the selection mask so it is restored together with the pixels on undo. */
  ED_image_undo_capture_selection_mask(ima, state->tile_number);
  state->undo_begun = true;

  /* Clear source pixels -- respect the actual selection mask shape (e.g. lasso). */
  const float *fmask = state->fragment_mask_ibuf ? state->fragment_mask_ibuf->float_buffer.data :
                                                    nullptr;
  if (ibuf->float_data()) {
    const int channels = ibuf->channels ? ibuf->channels : 4;
    float *data = ibuf->float_data_for_write();
    for (int ly = 0; ly < state->size_px.y; ly++) {
      const int py = state->origin_px.y + ly;
      if (py < 0 || py >= ibuf->y) {
        continue;
      }
      for (int lx = 0; lx < state->size_px.x; lx++) {
        if (fmask && fmask[ly * state->size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
          continue;
        }
        const int px = state->origin_px.x + lx;
        if (px < 0 || px >= ibuf->x) {
          continue;
        }
        memset(data + (py * ibuf->x + px) * channels, 0, sizeof(float) * channels);
      }
    }
  }
  else if (ibuf->byte_data()) {
    uint8_t *data = ibuf->byte_data_for_write();
    for (int ly = 0; ly < state->size_px.y; ly++) {
      const int py = state->origin_px.y + ly;
      if (py < 0 || py >= ibuf->y) {
        continue;
      }
      for (int lx = 0; lx < state->size_px.x; lx++) {
        if (fmask && fmask[ly * state->size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
          continue;
        }
        const int px = state->origin_px.x + lx;
        if (px < 0 || px >= ibuf->x) {
          continue;
        }
        memset(data + (py * ibuf->x + px) * 4, 0, 4);
      }
    }
  }

  /* Invalidate the display buffer after the pixel data has been modified. */
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

  /* Clear source region in the selection mask (only pixels that belong to the selection). */
  ImBuf *mask = BKE_image_paint_selection_mask_lookup(ima, state->tile_number);
  if (mask) {
    float *mdata = mask->float_data_for_write();
    if (mdata) {
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = state->origin_px.y + ly;
        if (py < 0 || py >= mask->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          if (fmask && fmask[ly * state->size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }
          const int px = state->origin_px.x + lx;
          if (px >= 0 && px < mask->x) {
            mdata[py * mask->x + px] = 0.0f;
          }
        }
      }
    }
  }

  BKE_image_mark_dirty(ima, ibuf);

  /* Mark the source region dirty so the partial-update system and GPU texture cache both
   * reflect the cleared pixels before the first draw callback fires. */
  rcti dirty;
  BLI_rcti_init(&dirty,
                state->origin_px.x,
                state->origin_px.x + state->size_px.x,
                state->origin_px.y,
                state->origin_px.y + state->size_px.y);
  const ImageTile *tile = BKE_image_get_tile(ima, state->tile_number);
  BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);

  BKE_image_release_ibuf(ima, ibuf, lock);

  /* Free cached GPU textures so the Image Editor reloads from the modified ibuf. */
  BKE_image_free_gputextures(ima);

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

static void image_select_transform_restore_source(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima || !sima->image || !state->fragment_ibuf) {
    if (state->undo_begun) {
      ED_image_undo_push_end();
    }
    return;
  }

  Image *ima = sima->image;
  ImageUser iuser = state->iuser;

  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  const float *fmask = state->fragment_mask_ibuf ? state->fragment_mask_ibuf->float_buffer.data :
                                                    nullptr;
  if (ibuf) {
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

    /* Paste fragment back at source origin -- only selected pixels (mask-aware). */
    if (fmask) {
      const bool is_float = ibuf->float_data() != nullptr;
      if (is_float) {
        const int channels = ibuf->channels ? ibuf->channels : 4;
        float *dst = ibuf->float_data_for_write();
        const float *src = state->fragment_ibuf->float_data();
        for (int ly = 0; ly < state->size_px.y; ly++) {
          const int py = state->origin_px.y + ly;
          if (py < 0 || py >= ibuf->y) {
            continue;
          }
          for (int lx = 0; lx < state->size_px.x; lx++) {
            if (fmask[ly * state->size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
              continue;
            }
            const int px = state->origin_px.x + lx;
            if (px < 0 || px >= ibuf->x) {
              continue;
            }
            memcpy(dst + (py * ibuf->x + px) * channels,
                   src + (ly * state->size_px.x + lx) * channels,
                   channels * sizeof(float));
          }
        }
      }
      else if (ibuf->byte_data()) {
        uint8_t *dst = ibuf->byte_data_for_write();
        const uint8_t *src = state->fragment_ibuf->byte_data();
        for (int ly = 0; ly < state->size_px.y; ly++) {
          const int py = state->origin_px.y + ly;
          if (py < 0 || py >= ibuf->y) {
            continue;
          }
          for (int lx = 0; lx < state->size_px.x; lx++) {
            if (fmask[ly * state->size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
              continue;
            }
            const int px = state->origin_px.x + lx;
            if (px < 0 || px >= ibuf->x) {
              continue;
            }
            memcpy(dst + (py * ibuf->x + px) * 4, src + (ly * state->size_px.x + lx) * 4, 4);
          }
        }
      }
    }
    else {
      /* Fallback: no mask, restore the full bounding box. */
      IMB_copy_rect(ibuf, state->fragment_ibuf, int2{0, 0}, state->origin_px, state->size_px);
    }
    BKE_image_mark_dirty(ima, ibuf);
  }

  /* Restore selection mask at source region using the actual lasso shape. */
  ImBuf *mask = BKE_image_paint_selection_mask_lookup(ima, state->tile_number);
  if (mask) {
    float *mdata = mask->float_data_for_write();
    if (mdata) {
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = state->origin_px.y + ly;
        if (py < 0 || py >= mask->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          const int px = state->origin_px.x + lx;
          if (px < 0 || px >= mask->x) {
            continue;
          }
          mdata[py * mask->x + px] = fmask ? fmask[ly * state->size_px.x + lx] : 1.0f;
        }
      }
    }
  }

  if (ibuf) {
    rcti dirty;
    BLI_rcti_init(&dirty,
                  state->origin_px.x,
                  state->origin_px.x + state->size_px.x,
                  state->origin_px.y,
                  state->origin_px.y + state->size_px.y);
    const ImageTile *tile = BKE_image_get_tile(ima, state->tile_number);
    BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);
    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  if (state->undo_begun) {
    /* Close the undo step opened in lift_source (pre/post are identical since we restored),
     * then immediately discard the no-op step so Ctrl+Z is not wasted on it. */
    ED_image_undo_push_end();
    UndoStack *undo_stack = ED_undo_stack_get();
    if (undo_stack) {
      BKE_undosys_stack_clear_active(undo_stack);
    }
  }

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

void image_select_transform_state_free(ImageSelectTransformState *state)
{
  if (!state) {
    return;
  }
  if (state->owner_region && state->owner_region->runtime->gizmo_map) {
    WM_gizmomap_tag_refresh(state->owner_region->runtime->gizmo_map);
  }
  if (state->draw_handle && state->owner_region_type) {
    ED_region_draw_cb_exit(state->owner_region_type, state->draw_handle);
    state->draw_handle = nullptr;
  }
  if (state->fragment_tex) {
    GPU_texture_free(state->fragment_tex);
    state->fragment_tex = nullptr;
  }
  if (state->fragment_ibuf) {
    IMB_freeImBuf(state->fragment_ibuf);
    state->fragment_ibuf = nullptr;
  }
  if (state->fragment_mask_ibuf) {
    IMB_freeImBuf(state->fragment_mask_ibuf);
    state->fragment_mask_ibuf = nullptr;
  }
  if (state->fragment_display_ibuf) {
    IMB_freeImBuf(state->fragment_display_ibuf);
    state->fragment_display_ibuf = nullptr;
  }
  MEM_delete(state);
}

bool image_select_transform_is_floating(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return sima && sima->runtime && sima->runtime->paint_select.transform != nullptr;
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
  ImageSelectTransformState *&g_transform_state = sima->runtime->paint_select.transform;

  if (g_transform_state && sima == g_transform_state->owner_sima) {
    return true;
  }
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  if (!sima->image) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  if (!scene->toolsettings->imapaint.use_selection_mask) {
    CTX_wm_operator_poll_msg_set(C, "No active selection mask");
    return false;
  }
  return true;
}

static void image_select_transform_cancel(bContext *C, wmOperator *op)
{
  /* Use the session state directly: with the long-lived modal, op->customdata may already
   * be stale if confirm/cancel exec ran before the modal received its final event. */
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->runtime) {
    ImageSelectTransformState *&g_transform_state = sima->runtime->paint_select.transform;
    if (g_transform_state) {
      image_select_transform_restore_source(C, g_transform_state);
      image_select_transform_state_free(g_transform_state);
      g_transform_state = nullptr;
    }
  }
  op->customdata = nullptr;
}

static void image_select_transform_commit(bContext *C, ImageSelectTransformState *state);

static wmOperatorStatus image_select_transform_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *&g_transform_state = sima->runtime->paint_select.transform;
  if (!g_transform_state) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = g_transform_state;
  g_transform_state = nullptr;
  image_select_transform_commit(C, state);
  image_select_transform_state_free(state);
  if (ARegion *region = CTX_wm_region(C)) {
    if (region->runtime->gizmo_map) {
      WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
    }
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_transform_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *&g_transform_state = sima->runtime->paint_select.transform;
  if (!g_transform_state) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = g_transform_state;
  g_transform_state = nullptr;
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
                                                     ImageSelectTransformState::HandleType hit)
{
  const float2 mouse_co(event->mval[0], event->mval[1]);
  state->active_handle = hit;
  state->mouse_start_pos = mouse_co;
  state->mouse_curr_pos = mouse_co;
  state->drag_start_translation = state->translation;
  state->drag_start_rotation = state->rotation;
  state->drag_start_scale = state->scale;
  state->drag_start_anchor = state->anchor;
  state->is_snapped = false;
}

static ImageSelectTransformState::HandleType image_select_transform_handle_to_internal(
    const ImageSelectTransformHandleType handle)
{
  return static_cast<ImageSelectTransformState::HandleType>(int(handle));
}

bool image_select_transform_is_floating_in_space(const SpaceImage *sima)
{
  return sima && sima->runtime && sima->runtime->paint_select.transform != nullptr;
}

ImageSelectTransformState *image_select_transform_state_get(SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return nullptr;
  }
  return sima->runtime->paint_select.transform;
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
      state, event, image_select_transform_handle_to_internal(handle));
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
  ui::view2d_region_to_view(
      &region->v2d, state->mouse_start_pos.x, state->mouse_start_pos.y, &mouse_start_uv.x, &mouse_start_uv.y);

  SpaceImage *sima_modal = state->owner_sima;
  float canvas_w_modal = float(state->fragment_ibuf->x);
  float canvas_h_modal = float(state->fragment_ibuf->y);
  if (sima_modal && sima_modal->image) {
    void *modal_lock = nullptr;
    ImBuf *modal_canvas = BKE_image_acquire_ibuf(sima_modal->image, &state->iuser, &modal_lock);
    if (modal_canvas) {
      canvas_w_modal = float(modal_canvas->x);
      canvas_h_modal = float(modal_canvas->y);
      BKE_image_release_ibuf(sima_modal->image, modal_canvas, modal_lock);
    }
  }

  const float2 mouse_px = image_select_view_to_tile_px(
      state, mouse_uv, canvas_w_modal, canvas_h_modal);
  const float2 start_px = image_select_view_to_tile_px(
      state, mouse_start_uv, canvas_w_modal, canvas_h_modal);

  const float2 pivot_modal = state->anchor;
  const float cos_r_modal = cosf(state->rotation);
  const float sin_r_modal = sinf(state->rotation);
  auto apply_forward_modal = [&](const float2 local) -> float2 {
    const float2 global = float2(state->origin_px) + local;
    const float2 centered = global - pivot_modal;
    const float2 scaled = centered * state->scale;
    const float2 rotated = float2(scaled.x * cos_r_modal - scaled.y * sin_r_modal,
                                  scaled.x * sin_r_modal + scaled.y * cos_r_modal);
    return rotated + pivot_modal + state->translation;
  };

  if (active == ImageSelectTransformState::HANDLE_MOVE) {
    const float2 delta_scr = mouse_co - state->mouse_start_pos;
    const float2 delta_px = mouse_px - start_px;

    const bool anchor_inside = (state->drag_start_anchor.x >= state->origin_px.x - 0.1f &&
                                state->drag_start_anchor.x <= state->origin_px.x + state->size_px.x + 0.1f &&
                                state->drag_start_anchor.y >= state->origin_px.y - 0.1f &&
                                state->drag_start_anchor.y <= state->origin_px.y + state->size_px.y + 0.1f);

    if (anchor_inside) {
      /* Preview moves in screen space (then rotates around pivot). Map screen drag delta to
       * tile-pixel translation so the point under the cursor does not jump on press. */
      bool jac_ok;
      const float2x2 J = image_select_pixel_to_screen_jacobian(state,
                                                               region,
                                                               state->drag_start_anchor,
                                                               canvas_w_modal,
                                                               canvas_h_modal);
      const float2x2 J_inv = math::invert(J, jac_ok);
      const float2 delta_translation = jac_ok ? J_inv * delta_scr : delta_px;
      state->translation = state->drag_start_translation + delta_translation;
      state->anchor = state->drag_start_anchor;
    }
    else {
      const float inv_cos = cosf(-state->rotation);
      const float inv_sin = sinf(-state->rotation);
      const float2 inv_rotated = float2(delta_px.x * inv_cos - delta_px.y * inv_sin,
                                        delta_px.x * inv_sin + delta_px.y * inv_cos);
      const float2 delta_pivot = float2(state->scale.x != 0.0f ? inv_rotated.x / state->scale.x :
                                                                 0.0f,
                                        state->scale.y != 0.0f ? inv_rotated.y / state->scale.y :
                                                                 0.0f);

      state->anchor = state->drag_start_anchor - delta_pivot;
      state->translation = state->drag_start_translation + delta_pivot;
    }
  }
  else if (active == ImageSelectTransformState::HANDLE_ROTATE) {
    const float2 center_scr = state->screen_anchor;
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
    char status_str[128];
    snprintf(status_str,
             sizeof(status_str),
             "Rotation: %.2f°",
             state->rotation * 180.0f / float(M_PI));
    ED_area_status_text(state->owner_area ? state->owner_area : CTX_wm_area(C), status_str);
  }
  else if (active == ImageSelectTransformState::HANDLE_ANCHOR) {
    float2 target_anchor_px = mouse_px;

    const float2 snap_points_scr[9] = {state->screen_corners[0],
                                        state->screen_corners[1],
                                        state->screen_corners[2],
                                        state->screen_corners[3],
                                        state->screen_midpoints[0],
                                        state->screen_midpoints[1],
                                        state->screen_midpoints[2],
                                        state->screen_midpoints[3],
                                        state->screen_center};

    const float2 snap_points_px[9] = {
        apply_forward_modal(float2(0, 0)),
        apply_forward_modal(float2(state->size_px.x, 0)),
        apply_forward_modal(float2(state->size_px.x, state->size_px.y)),
        apply_forward_modal(float2(0, state->size_px.y)),
        apply_forward_modal(float2(state->size_px.x * 0.5f, 0.0f)),
        apply_forward_modal(float2(state->size_px.x, state->size_px.y * 0.5f)),
        apply_forward_modal(float2(state->size_px.x * 0.5f, state->size_px.y)),
        apply_forward_modal(float2(0.0f, state->size_px.y * 0.5f)),
        apply_forward_modal(float2(state->size_px.x * 0.5f, state->size_px.y * 0.5f))};

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
      target_anchor_px = snap_points_px[snap_idx];
      state->is_snapped = true;
      state->snap_indicator_screen = snap_points_scr[snap_idx];
    }

    const float2 offset = target_anchor_px - state->anchor - state->translation;
    const float inv_cos = cosf(-state->rotation);
    const float inv_sin = sinf(-state->rotation);
    const float2 inv_rotated = float2(offset.x * inv_cos - offset.y * inv_sin,
                                      offset.x * inv_sin + offset.y * inv_cos);
    const float2 inv_scaled = float2(state->scale.x != 0.0f ? inv_rotated.x / state->scale.x :
                                                              0.0f,
                                      state->scale.y != 0.0f ? inv_rotated.y / state->scale.y :
                                                              0.0f);

    const float2 target_anchor_untransformed = state->anchor + inv_scaled;

    const float2 delta_anchor = target_anchor_untransformed - state->anchor;
    const float2 scaled_delta = delta_anchor * state->scale;
    const float2 rotated_scaled_delta = float2(scaled_delta.x * cos_r_modal - scaled_delta.y * sin_r_modal,
                                               scaled_delta.x * sin_r_modal + scaled_delta.y * cos_r_modal);
    state->translation += (rotated_scaled_delta - delta_anchor);
    state->anchor = target_anchor_untransformed;
  }
  else if (active >= ImageSelectTransformState::HANDLE_C0 &&
           active <= ImageSelectTransformState::HANDLE_M_LEFT)
  {
    float2 local_offset(0.0f, 0.0f);
    switch (active) {
      case ImageSelectTransformState::HANDLE_C0:
        local_offset = float2(0.0f, 0.0f);
        break;
      case ImageSelectTransformState::HANDLE_C1:
        local_offset = float2(state->size_px.x, 0.0f);
        break;
      case ImageSelectTransformState::HANDLE_C2:
        local_offset = float2(state->size_px.x, state->size_px.y);
        break;
      case ImageSelectTransformState::HANDLE_C3:
        local_offset = float2(0.0f, state->size_px.y);
        break;
      case ImageSelectTransformState::HANDLE_M_BOTTOM:
        local_offset = float2(state->size_px.x * 0.5f, 0.0f);
        break;
      case ImageSelectTransformState::HANDLE_M_RIGHT:
        local_offset = float2(state->size_px.x, state->size_px.y * 0.5f);
        break;
      case ImageSelectTransformState::HANDLE_M_TOP:
        local_offset = float2(state->size_px.x * 0.5f, state->size_px.y);
        break;
      case ImageSelectTransformState::HANDLE_M_LEFT:
        local_offset = float2(0.0f, state->size_px.y * 0.5f);
        break;
      default:
        break;
    }

    const float2 pivot_start = state->drag_start_anchor;
    const float cos_r_start = cosf(state->drag_start_rotation);
    const float sin_r_start = sinf(state->drag_start_rotation);
    auto apply_forward_start = [&](const float2 local) -> float2 {
      const float2 global = float2(state->origin_px) + local;
      const float2 centered = global - pivot_start;
      const float2 scaled = centered * state->drag_start_scale;
      const float2 rotated = float2(scaled.x * cos_r_start - scaled.y * sin_r_start,
                                      scaled.x * sin_r_start + scaled.y * cos_r_start);
      return rotated + pivot_start + state->drag_start_translation;
    };
    const float2 H_start = apply_forward_start(local_offset);

    const float2 ref_anchor = state->drag_start_anchor + state->drag_start_translation;
    const float2 V_curr = mouse_px - ref_anchor;
    const float2 V_start = H_start - ref_anchor;

    const float rad = state->drag_start_rotation;
    const float2 Ux(cosf(rad), sinf(rad));
    const float2 Uy(-sinf(rad), cosf(rad));

    const float proj_start_x = math::dot(V_start, Ux);
    const float proj_start_y = math::dot(V_start, Uy);
    const float proj_curr_x = math::dot(V_curr, Ux);
    const float proj_curr_y = math::dot(V_curr, Uy);

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
  if (!state || !state->fragment_ibuf || !r_mats) {
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
   *   M * (-0.5,-0.5) + center = -0.5*e0 - 0.5*e1 + BL + 0.5*e0 + 0.5*e1 = BL  ✓
   *
   * Because the corners come from a screen-space rotation, e0 ⊥ e1 is always guaranteed,
   * so the cage stays a proper rectangle for any rotation angle and any view2d aspect ratio
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

  r_mats->anchor_screen[0] = scr_pivot.x;
  r_mats->anchor_screen[1] = scr_pivot.y;
  r_mats->anchor_screen[2] = 0.0f;

  return true;
}

static void image_select_transform_commit(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima;
  if (!sima || !sima->image || !state->fragment_ibuf) {
    return;
  }
  Image *ima = sima->image;
  ImageUser iuser = state->iuser;

  /* Acquire source tile to determine canvas dimensions and pixel format. */
  void *lock = nullptr;
  iuser.tile = state->tile_number;
  ImBuf *src_ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  if (!src_ibuf) {
    return;
  }
  const bool is_float = src_ibuf->float_buffer.data != nullptr;
  const int canvas_w = src_ibuf->x;
  const int canvas_h = src_ibuf->y;
  BKE_image_release_ibuf(ima, src_ibuf, lock);

  /* Build the forward transform matrix: fragment_local -> source_tile_pixel_space.
   * Forward: X_dst = R*S*(origin_px + X_local - anchor) + anchor + translation
   * column-major float3x3: m[col][row].
   * Rotation matches screen-space cage (state->rotation is measured on screen). */
  const float2 pivot_commit = state->anchor;

  float3x3 forward_matrix;
  {
    float2x2 R_linear = image_select_rotation_matrix_2d(state->rotation);
    ARegion *region = state->owner_region ? state->owner_region : CTX_wm_region(C);
    if (region) {
      bool jac_ok;
      const float2x2 J = image_select_pixel_to_screen_jacobian(
          state, region, pivot_commit, float(canvas_w), float(canvas_h));
      R_linear = image_select_rotation_pixel_from_screen(J, state->rotation, &jac_ok);
    }

    const float2x2 S = math::from_scale<float2x2>(state->scale);
    const float2x2 L = R_linear * S;
    const float a = L[0][0];
    const float b = L[1][0];
    const float c = L[0][1];
    const float d = L[1][1];
    const float2 rs_pivot = float2(
        a * (pivot_commit.x - float(state->origin_px.x)) +
            b * (pivot_commit.y - float(state->origin_px.y)),
        c * (pivot_commit.x - float(state->origin_px.x)) +
            d * (pivot_commit.y - float(state->origin_px.y)));
    const float tx = pivot_commit.x + state->translation.x - rs_pivot.x;
    const float ty = pivot_commit.y + state->translation.y - rs_pivot.y;
    forward_matrix[0] = float3(a, c, 0.0f);
    forward_matrix[1] = float3(b, d, 0.0f);
    forward_matrix[2] = float3(tx, ty, 1.0f);
  }
  /* IMB_transform expects a backward matrix: dest_pixel -> fragment_local. */
  const float3x3 backward_matrix_src = math::invert(forward_matrix);

  /* Compute source tile UV origin. */
  const int src_col = (state->tile_number - 1001) % 10;
  const int src_row = (state->tile_number - 1001) / 10;
  const float2 uv_origin_src = float2(float(src_col), float(src_row));

  /* Compute the UV-space AABB of the transformed fragment by projecting all four corners
   * through the forward matrix into source-tile pixel space, then into UV space. */
  const float2 corners_local[4] = {
      {0.0f, 0.0f},
      {float(state->size_px.x), 0.0f},
      {float(state->size_px.x), float(state->size_px.y)},
      {0.0f, float(state->size_px.y)}};
  float2 uv_min = float2(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  float2 uv_max = float2(std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest());
  for (const float2 &corner : corners_local) {
    const float3 src_px = math::transform_point(forward_matrix, float3(corner.x, corner.y, 1.0f));
    const float2 uv = float2(src_px.x / float(canvas_w) + uv_origin_src.x,
                             src_px.y / float(canvas_h) + uv_origin_src.y);
    uv_min = math::min(uv_min, uv);
    uv_max = math::max(uv_max, uv);
  }

  /* Fragment crop rect in fragment-local space -- constant across all destination tiles. */
  const rctf src_crop{0.0f, float(state->size_px.x), 0.0f, float(state->size_px.y)};

  /* Obtain the currently-open image undo step (lives in step_init until push_end).
   * Destination tiles other than the source tile were not registered at lift time, so their
   * pre-state must be snapshotted here -- before any pixel data is overwritten -- to make
   * Ctrl+Z restore them correctly. */
  UndoStack *ustack = ED_undo_stack_get();
  ImageUndoStep *us_open = (ustack && ustack->step_init &&
                            ustack->step_init->type == BKE_UNDOSYS_TYPE_IMAGE) ?
                               reinterpret_cast<ImageUndoStep *>(ustack->step_init) :
                               nullptr;

  /* Iterate over every tile in the image and apply the fragment to those that intersect
   * the UV AABB. When dest == source the pixel offset is zero and the math is identical
   * to the previous single-tile implementation. */
  for (ImageTile *dest_tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    const int dest_tile_num = dest_tile->tile_number;
    const int dst_col = (dest_tile_num - 1001) % 10;
    const int dst_row = (dest_tile_num - 1001) / 10;

    /* Skip tiles whose UV range [dst_col, dst_col+1) x [dst_row, dst_row+1) does not
     * intersect the transformed fragment's AABB. */
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
    if (us_open && dest_tile_num != state->tile_number) {
      ED_image_undo_push(ima, dest_ibuf, &iuser, us_open);
      /* Also capture the selection mask pre-state for this tile so it is restored
       * together with the pixel data on undo. */
      ED_image_undo_capture_selection_mask(ima, dest_tile_num);
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

    float3x3 M_dest_to_src = float3x3::identity();
    M_dest_to_src[0] = float3(sx, 0.0f, 0.0f);
    M_dest_to_src[1] = float3(0.0f, sy, 0.0f);
    M_dest_to_src[2] = float3(tx, ty, 1.0f);

    /* backward_dest maps: dest_tile_px -> src_tile_px -> fragment_local. */
    const float3x3 backward_dest = backward_matrix_src * M_dest_to_src;

    /* temp_pixels must match the destination buffer type -- IMB_transform does not support
     * mixed src/dst types. */
    ImBuf *temp_pixels = IMB_allocImBuf(
        dest_ibuf->x, dest_ibuf->y, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
    ImBuf *temp_mask = IMB_allocImBuf(dest_ibuf->x, dest_ibuf->y, ImBufFlags::FloatData);
    temp_pixels->channels = 4;
    IMB_rectfill_alpha(temp_pixels, 0.0f);

    /* Fill temp_mask red channel (index 0) with either transform result or full opacity. */
    float *fill_data = temp_mask->float_data_for_write();
    const int fill_count = dest_ibuf->x * dest_ibuf->y;
    for (int i = 0; i < fill_count; i++) {
      fill_data[i * 4 + 0] = 0.0f;
    }

    IMB_transform(state->fragment_ibuf, temp_pixels, IMB_TRANSFORM_MODE_CROP_SRC,
                  IMB_FILTER_BILINEAR, backward_dest, &src_crop);
    if (state->fragment_mask_ibuf) {
      IMB_transform(state->fragment_mask_ibuf, temp_mask, IMB_TRANSFORM_MODE_CROP_SRC,
                    IMB_FILTER_BILINEAR, backward_dest, &src_crop);
    }
    else {
      float *mask_fill = temp_mask->float_data_for_write();
      for (int i = 0; i < fill_count; i++) {
        mask_fill[i * 4 + 0] = 1.0f;
      }
    }

    const float *mask_data = temp_mask->float_buffer.data;
    const int total_pixels = dest_ibuf->x * dest_ibuf->y;

    /* Alpha-blend the transformed fragment onto the destination canvas.
     * blend = mask * fragment_alpha so partially-transparent pixels are respected. */
    if (dest_ibuf->float_data() && temp_pixels->float_buffer.data) {
      const float *frag_data = temp_pixels->float_buffer.data;
      float *canvas = dest_ibuf->float_data_for_write();
      const int ch = dest_ibuf->channels ? dest_ibuf->channels : 4;
      for (int i = 0; i < total_pixels; i++) {
        const float m = mask_data[i * 4 + 0];
        if (m <= 0.001f) {
          continue;
        }
        const float frag_alpha = (ch >= 4) ? frag_data[i * 4 + 3] : 1.0f;
        const float blend = m * frag_alpha;
        if (blend <= 0.001f) {
          continue;
        }
        canvas[i * ch + 0] = (1.0f - blend) * canvas[i * ch + 0] + blend * frag_data[i * 4 + 0];
        canvas[i * ch + 1] = (1.0f - blend) * canvas[i * ch + 1] + blend * frag_data[i * 4 + 1];
        canvas[i * ch + 2] = (1.0f - blend) * canvas[i * ch + 2] + blend * frag_data[i * 4 + 2];
        if (ch >= 4) {
          canvas[i * ch + 3] = (1.0f - blend) * canvas[i * ch + 3] +
                               blend * frag_data[i * 4 + 3];
        }
      }
    }
    else if (dest_ibuf->byte_data() && temp_pixels->byte_buffer.data) {
      const uint8_t *frag_data = temp_pixels->byte_buffer.data;
      uint8_t *canvas = dest_ibuf->byte_data_for_write();
      for (int i = 0; i < total_pixels; i++) {
        const float m = mask_data[i * 4 + 0];
        if (m <= 0.001f) {
          continue;
        }
        const float frag_alpha = frag_data[i * 4 + 3] / 255.0f;
        const float blend = m * frag_alpha;
        if (blend <= 0.001f) {
          continue;
        }
        canvas[i * 4 + 0] = uint8_t(std::clamp(
            (1.0f - blend) * float(canvas[i * 4 + 0]) + blend * float(frag_data[i * 4 + 0]),
            0.0f,
            255.0f));
        canvas[i * 4 + 1] = uint8_t(std::clamp(
            (1.0f - blend) * float(canvas[i * 4 + 1]) + blend * float(frag_data[i * 4 + 1]),
            0.0f,
            255.0f));
        canvas[i * 4 + 2] = uint8_t(std::clamp(
            (1.0f - blend) * float(canvas[i * 4 + 2]) + blend * float(frag_data[i * 4 + 2]),
            0.0f,
            255.0f));
        canvas[i * 4 + 3] = uint8_t(std::clamp(
            (1.0f - blend) * float(canvas[i * 4 + 3]) + blend * float(frag_data[i * 4 + 3]),
            0.0f,
            255.0f));
      }
    }

    /* Merge the transformed selection mask into the destination tile's runtime mask. */
    ImBuf *global_mask = BKE_image_paint_selection_mask_lookup(ima, dest_tile_num);
    if (global_mask) {
      float *g_mask = global_mask->float_data_for_write();
      for (int i = 0; i < total_pixels; i++) {
        g_mask[i] = std::clamp(g_mask[i] + mask_data[i * 4 + 0], 0.0f, 1.0f);
      }
      global_mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
    }

    IMB_freeImBuf(temp_pixels);
    IMB_freeImBuf(temp_mask);

    dest_ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(ima, dest_ibuf);
    BKE_image_release_ibuf(ima, dest_ibuf, lock);
  }

  BKE_image_partial_update_mark_full_update(ima);
  BKE_image_free_gputextures(ima);

  if (state->undo_begun) {
    ED_image_undo_push_end();
    state->undo_begun = false;
  }

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

static wmOperatorStatus image_select_transform_start_drag_gesture(bContext *C,
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
  ImageSelectTransformState *const g_transform_state = sima->runtime->paint_select.transform;
  if (!g_transform_state || g_transform_state->owner_sima != sima) {
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = g_transform_state;
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

static wmOperatorStatus image_select_transform_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectTransformState *&g_transform_state = sima->runtime->paint_select.transform;

  if (g_transform_state && g_transform_state->owner_sima == sima) {
    ImageSelectTransformState *state = g_transform_state;
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
  if (g_transform_state) {
    image_select_transform_restore_source(C, g_transform_state);
    image_select_transform_state_free(g_transform_state);
    g_transform_state = nullptr;
  }

  /* Commit any floating move-selection before starting transform. */
  ImageSelectMoveState *&g_floating_move = sima->runtime->paint_select.move;
  if (g_floating_move) {
    image_select_move_commit(C, g_floating_move);
    image_select_move_state_free(g_floating_move);
    g_floating_move = nullptr;
  }

  Image *ima = sima->image;

  /* For UDIM images, sima->iuser.tile is reset to 0 after each temporary acquire.
   * The true active tile is tracked by ima->active_tile_index. */
  ImageUser iuser = sima->iuser;
  if (ima->source == IMA_SRC_TILED) {
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&ima->tiles, ima->active_tile_index));
    iuser.tile = active ? active->tile_number :
                          static_cast<const ImageTile *>(ima->tiles.first)->tile_number;
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(ima, &iuser);
  int tile_number = active_tile ? active_tile->tile_number : 1001;
  tile_number = image_paint_selection_resolve_tile(ima, sima, tile_number);
  iuser.tile = tile_number;

  auto *state = MEM_new<ImageSelectTransformState>(__func__);
  state->owner_sima = sima;
  state->tile_number = tile_number;
  state->iuser = iuser;
  state->prev_mouse_xy = int2{event->mval[0], event->mval[1]};
  state->mouse_start_pos = float2(event->mval[0], event->mval[1]);
  state->mouse_curr_pos = float2(event->mval[0], event->mval[1]);
  state->translation = float2(0.0f, 0.0f);
  state->rotation = 0.0f;
  state->scale = float2(1.0f, 1.0f);
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;
  state->fragment_tex = nullptr;
  state->is_snapped = false;
  state->use_proportional_scale = RNA_boolean_get(op->ptr, "proportional");

  state->fragment_ibuf = image_select_move_extract(op,
                                                   ima,
                                                   &iuser,
                                                   sima,
                                                   tile_number,
                                                   &state->origin_px,
                                                   &state->size_px,
                                                   &state->fragment_mask_ibuf);
  if (!state->fragment_ibuf) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }

  if (state->fragment_mask_ibuf) {
    state->fragment_display_ibuf = image_select_make_display_ibuf(state->fragment_ibuf,
                                                                state->fragment_mask_ibuf);
  }

  state->anchor = float2(state->origin_px.x + state->size_px.x * 0.5f,
                         state->origin_px.y + state->size_px.y * 0.5f);

  image_select_transform_lift_source(C, state);

  ARegion *region = CTX_wm_region(C);
  state->owner_area = CTX_wm_area(C);
  state->owner_region = region;
  state->owner_region_type = region->runtime->type;

  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_transform_texture, state, REGION_DRAW_POST_VIEW);

  g_transform_state = state;
  op->customdata = nullptr;
  state->is_dragging = false;
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;

  if (region->runtime->gizmo_map) {
    WM_gizmomap_tag_refresh(region->runtime->gizmo_map);
  }

  /* Long-lived modal for Enter/Esc; handle drags are owned by IMAGE_GGT_paint_select_transform. */
  ED_region_tag_redraw(region);
  WM_event_add_modal_handler(C, op);
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
  ImageSelectTransformState *&g_transform_state = sima->runtime->paint_select.transform;
  ImageSelectTransformState *state = g_transform_state;
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

    /* Enter -- commit the transform. */
    if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS) {
      ED_area_status_text(state->owner_area, nullptr);
      g_transform_state = nullptr;
      image_select_transform_commit(C, state);
      image_select_transform_state_free(state);
      op->customdata = nullptr;
      WM_event_add_notifier(C, NC_WINDOW, nullptr);
      return OPERATOR_FINISHED;
    }

    /* Esc -- cancel the transform. */
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      ED_area_status_text(state->owner_area, nullptr);
      g_transform_state = nullptr;
      image_select_transform_restore_source(C, state);
      image_select_transform_state_free(state);
      op->customdata = nullptr;
      WM_event_add_notifier(C, NC_WINDOW, nullptr);
      return OPERATOR_FINISHED;
    }

    /* Ctrl+Z: cancel the floating transform rather than passing the undo event through to
     * the WM. The open image undo step (opened at lift time) is discarded inside
     * image_select_transform_restore_source so the undo stack stays clean. */
    if (event->type == EVT_ZKEY && event->val == KM_PRESS &&
        (event->modifier & KM_CTRL) && !(event->modifier & KM_SHIFT))
    {
      ED_area_status_text(state->owner_area, nullptr);
      g_transform_state = nullptr;
      image_select_transform_restore_source(C, state);
      image_select_transform_state_free(state);
      op->customdata = nullptr;
      WM_event_add_notifier(C, NC_WINDOW, nullptr);
      return OPERATOR_FINISHED;
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

void PAINT_OT_image_select_transform(wmOperatorType *ot)
{
  ot->name = "Transform Selection";
  ot->idname = "PAINT_OT_image_select_transform";
  ot->description = "Free transform the selected region (Scale, Rotate, Move)";

  ot->invoke = image_select_transform_invoke;
  ot->modal = image_select_transform_modal;
  ot->cancel = image_select_transform_cancel;
  ot->poll = image_select_transform_poll;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_boolean(ot->srna, "proportional", true, "Constrain Proportions", "Maintain uniform aspect ratio when scaling");
}

void PAINT_OT_image_select_transform_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Selection Transform";
  ot->idname = "PAINT_OT_image_select_transform_confirm";
  ot->description = "Apply the transform to the canvas";

  ot->exec = image_select_transform_confirm_exec;
  ot->poll = image_select_transform_floating_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void PAINT_OT_image_select_transform_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Selection Transform";
  ot->idname = "PAINT_OT_image_select_transform_cancel";
  ot->description = "Restore the fragment to its original state";

  ot->exec = image_select_transform_cancel_exec;
  ot->poll = image_select_transform_floating_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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

/** \} */
}  /* namespace blender */
