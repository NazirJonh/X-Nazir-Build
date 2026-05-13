/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
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

#include "BLF_api.hh"
#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"

namespace blender {

/* Set to 0 to disable console trace (Windows: Visual Studio / cmd stdout). */
#ifndef IMAGE_SELECT_DEBUG
#  define IMAGE_SELECT_DEBUG 1
#endif

#if IMAGE_SELECT_DEBUG
#  define IMG_SEL_DBG(fmt, ...) \
    printf("[image_select] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)

static const char *image_select_debug_op_status(const wmOperatorStatus status)
{
  if (status & OPERATOR_RUNNING_MODAL) {
    return "RUNNING_MODAL";
  }
  if (status & OPERATOR_FINISHED) {
    return "FINISHED";
  }
  if (status & OPERATOR_CANCELLED) {
    return "CANCELLED";
  }
  if (status & OPERATOR_PASS_THROUGH) {
    return "PASS_THROUGH";
  }
  return "OTHER";
}

static const char *image_select_debug_event_type(const short type)
{
  if (type == LEFTMOUSE) {
    return "LEFTMOUSE";
  }
  if (type == MOUSEMOVE) {
    return "MOUSEMOVE";
  }
  if (type == EVT_ESCKEY) {
    return "ESC";
  }
  if (type == EVT_RETKEY) {
    return "RET";
  }
  return "other";
}

static void image_select_debug_log_event(const char *label, const wmEvent *event)
{
  if (!event) {
    IMG_SEL_DBG("%s: event=null", label);
    return;
  }
  IMG_SEL_DBG("%s: type=%s val=%d mval=(%d,%d) xy=(%d,%d)",
              label,
              image_select_debug_event_type(event->type),
              int(event->val),
              event->mval[0],
              event->mval[1],
              event->xy[0],
              event->xy[1]);
}
#else
#  define IMG_SEL_DBG(fmt, ...) ((void)0)
#endif

/* -------------------------------------------------------------------- */
/** \name Internal helpers
 * \{ */

static void image_paint_selection_reset(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->image) {
    BKE_image_paint_selection_mask_free(sima->image);
  }

  Scene *scene = CTX_data_scene(C);
  scene->toolsettings->imapaint.use_selection_mask = 0;

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (sima && sima->image) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, sima->image);
  }
}

/**
 * Poll for all image paint selection operators.
 * Does not require an active brush тАФ selection tools are independent of the brush.
 */
static bool image_paint_selection_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima) {
    return false;
  }
  if (sima->mode != SI_MODE_PAINT) {
    return false;
  }
  if (sima->image != nullptr &&
      (!ID_IS_EDITABLE(sima->image) || ID_IS_OVERRIDE_LIBRARY(sima->image)))
  {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move Selection Internal Helpers
 * \{ */

/**
 * Compute the bounding box of selected pixels (value > 0.5) in a 1-channel float mask.
 * Returns false when the mask is null or contains no selected pixels.
 * Coordinates are in mask-pixel space (0..mask->x, 0..mask->y);
 * since the mask is always created with ibuf->x * ibuf->y dimensions
 * (#BKE_image_paint_selection_mask_get), these are also tile-pixel coordinates.
 */
static bool image_selection_bbox(
    const ImBuf *mask, int *r_x_min, int *r_y_min, int *r_x_max, int *r_y_max)
{
  if (!mask || !mask->float_buffer.data) {
    printf("[DEBUG] image_selection_bbox: No float buffer data. mask=%p\n", (void*)mask);
    if (mask) printf("[DEBUG] image_selection_bbox: mask->float_buffer.data=%p\n", (void*)mask->float_buffer.data);
    return false;
  }
  const float *pixels = mask->float_buffer.data;
  *r_x_min = mask->x;
  *r_y_min = mask->y;
  *r_x_max = 0;
  *r_y_max = 0;

  int pixel_count = 0;
  float min_val = 1.0f, max_val = 0.0f;

  for (int y = 0; y < mask->y; y++) {
    for (int x = 0; x < mask->x; x++) {
      float val = pixels[y * mask->x + x];
      min_val = min_ff(min_val, val);
      max_val = max_ff(max_val, val);
      if (val > 0.5f) {
        pixel_count++;
        *r_x_min = min_ii(*r_x_min, x);
        *r_y_min = min_ii(*r_y_min, y);
        *r_x_max = max_ii(*r_x_max, x + 1);
        *r_y_max = max_ii(*r_y_max, y + 1);
      }
    }
  }

  printf("[DEBUG] image_selection_bbox: Mask size: %dx%d, Value range: [%.4f, %.4f], Pixels > 0.5: %d\n",
         mask->x, mask->y, min_val, max_val, pixel_count);

  bool result = *r_x_max > *r_x_min && *r_y_max > *r_y_min;
  if (result) {
    printf("[DEBUG] image_selection_bbox: Found bbox (%d,%d)-(%d,%d)\n", *r_x_min, *r_y_min, *r_x_max, *r_y_max);
  } else {
    printf("[DEBUG] image_selection_bbox: No bbox found\n");
  }
  return result;
}

/**
 * Runtime state for the image select move operation.
 * Lifetime: allocated in invoke, freed in confirm or cancel.
 * Stored in g_floating_state тАФ survives the initial drag modal so the user
 * can navigate freely between drag gestures while the fragment is floating.
 */
struct ImageSelectMoveState {
  /* Space and region type that own this state тАФ used for draw callback lifetime. */
  SpaceImage *owner_sima = nullptr;
  ARegionType *owner_region_type = nullptr;
  /* Fragment pixels extracted from the source region. */
  ImBuf *fragment_ibuf = nullptr;
  /* Cropped 1-channel float mask (same dims as fragment_ibuf).
   * Pixels > 0.5 belong to the selection; respects arbitrary lasso shapes. */
  ImBuf *fragment_mask_ibuf = nullptr;
  /* 4-channel RGBA float copy of fragment_ibuf with mask baked into alpha.
   * Used only for the GPU draw preview; never written back to the canvas.
   * Null when there is no lasso mask (rectangular selection uses fragment_ibuf directly). */
  ImBuf *fragment_display_ibuf = nullptr;
  /* Source bbox bottom-left in tile-pixel coordinates. */
  int2 origin_px = {0, 0};
  /* Fragment size in tile-pixel coordinates. */
  int2 size_px = {0, 0};
  /* Accumulated drag offset in tile-pixel coordinates. */
  int2 drag_offset = {0, 0};
  /* Tile that owns the fragment. */
  int tile_number = 1001;
  /* Copy of ImageUser for BKE_image_acquire_ibuf calls. */
  ImageUser iuser = {};
  /* Mouse position at the previous drag event (for incremental delta). */
  int2 prev_mouse_xy = {0, 0};
  /* Handle returned by ED_region_draw_cb_activate, removed on exit. */
  void *draw_handle = nullptr;
  /* True if lift_source successfully called ED_image_undo_push_begin. */
  bool undo_begun = false;
  /* Snapshot of drag_offset taken before each drag gesture starts.
   * Ctrl+Z while floating pops this stack to step back one gesture at a time.
   * When the stack is empty, Ctrl+Z restores the source and ends the operation. */
  blender::Vector<int2> drag_position_history;
  /* True while the user holds LMB during an active drag gesture. */
  bool is_dragging = false;
};

/* Alive between invoke (fragment lift) and confirm/cancel. Null when no move is in progress.
 * A single global is safe: only one Image Editor can be active at a time. */
static ImageSelectMoveState *g_floating_state = nullptr;

struct ImageSelectTransformState {
  /* Context & target details */
  SpaceImage *owner_sima = nullptr;
  ARegionType *owner_region_type = nullptr;
  ImageUser iuser = {};
  int tile_number = 1001;
  bool undo_begun = false;

  /* Image buffers */
  ImBuf *fragment_ibuf = nullptr;          /* Extracted source pixel data */
  ImBuf *fragment_mask_ibuf = nullptr;     /* Extracted float selection mask (0.0 to 1.0) */
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

static ImageSelectTransformState *g_transform_state = nullptr;

bool image_select_transform_is_floating(bContext *C);

#if IMAGE_SELECT_DEBUG
static const char *image_select_debug_handle_name(
    const ImageSelectTransformState::HandleType handle)
{
  switch (handle) {
    case ImageSelectTransformState::HANDLE_NONE:
      return "NONE";
    case ImageSelectTransformState::HANDLE_MOVE:
      return "MOVE";
    case ImageSelectTransformState::HANDLE_ROTATE:
      return "ROTATE";
    case ImageSelectTransformState::HANDLE_ANCHOR:
      return "ANCHOR";
    case ImageSelectTransformState::HANDLE_C0:
      return "C0";
    case ImageSelectTransformState::HANDLE_C1:
      return "C1";
    case ImageSelectTransformState::HANDLE_C2:
      return "C2";
    case ImageSelectTransformState::HANDLE_C3:
      return "C3";
    case ImageSelectTransformState::HANDLE_M_BOTTOM:
      return "M_BOTTOM";
    case ImageSelectTransformState::HANDLE_M_RIGHT:
      return "M_RIGHT";
    case ImageSelectTransformState::HANDLE_M_TOP:
      return "M_TOP";
    case ImageSelectTransformState::HANDLE_M_LEFT:
      return "M_LEFT";
  }
  return "?";
}
#endif

/**
 * Clipboard state for copy/paste of selection mask fragments.
 * Persists across operations; freed on app shutdown.
 */
struct ImageClipboardState {
  /* Fragment pixel data from the copied region. */
  ImBuf *fragment_ibuf = nullptr;
  /* 1-channel float mask from the copied selection (0..1). */
  ImBuf *fragment_mask_ibuf = nullptr;
  /* 4-channel RGBA float copy of fragment_ibuf with mask baked into alpha.
   * Used only for GPU preview drawing; never written back to the canvas.
   * Null when there is no lasso mask (rectangular selection uses fragment_ibuf directly). */
  ImBuf *fragment_display_ibuf = nullptr;
  /* Position in tile-pixel coordinates where the fragment was copied from. */
  int2 origin_px = {0, 0};
  /* Tile number from which the fragment was copied. */
  int tile_number = 1001;
  /* True if this clipboard contains a mask (copied from image paint selection).
   * False if clipboard was filled from WM system clipboard. */
  bool has_mask = false;
};

/* Global clipboard buffer. Cleared on app exit via WM_exit_handler.
 * A single global is safe: only one Image Editor can be active at a time. */
static ImageClipboardState *g_clipboard_state = nullptr;
static bool g_clipboard_atexit_registered = false;
static void image_clipboard_state_free();

static void image_clipboard_atexit(void * /*user_data*/)
{
  image_clipboard_state_free();
}

/* -------------------------------------------------------------------- */
/** \name Clipboard State Management
 * \{ */

/**
 * Free all buffers in the clipboard state and deallocate it.
 * Safe to call even if g_clipboard_state is null.
 */
static void image_clipboard_state_free()
{
  if (!g_clipboard_state) {
    return;
  }

  if (g_clipboard_state->fragment_ibuf) {
    IMB_freeImBuf(g_clipboard_state->fragment_ibuf);
  }
  if (g_clipboard_state->fragment_mask_ibuf) {
    IMB_freeImBuf(g_clipboard_state->fragment_mask_ibuf);
  }
  if (g_clipboard_state->fragment_display_ibuf) {
    IMB_freeImBuf(g_clipboard_state->fragment_display_ibuf);
  }

  MEM_delete(g_clipboard_state);
  g_clipboard_state = nullptr;
}

/**
 * Allocate a new clipboard state and assign to global.
 * Frees any existing clipboard state first.
 * Fragment buffers must be non-null; always succeeds (MEM_new asserts on failure).
 */
static bool image_clipboard_state_set(ImBuf *fragment_ibuf,
                                       ImBuf *fragment_mask_ibuf,
                                       const int2 &origin_px,
                                       int tile_number,
                                       bool has_mask)
{
  BLI_assert(fragment_ibuf != nullptr);

  image_clipboard_state_free();

  g_clipboard_state = MEM_new<ImageClipboardState>(__func__);
  g_clipboard_state->fragment_ibuf = fragment_ibuf;
  g_clipboard_state->fragment_mask_ibuf = fragment_mask_ibuf;
  g_clipboard_state->origin_px = origin_px;
  g_clipboard_state->tile_number = tile_number;
  g_clipboard_state->has_mask = has_mask;

  return true;
}

void image_paint_clipboard_ensure_atexit_handler()
{
  if (g_clipboard_atexit_registered) {
    return;
  }

  BKE_blender_atexit_register(image_clipboard_atexit, nullptr);
  g_clipboard_atexit_registered = true;
}

/** \} */

/**
 * Build a 4-channel RGBA float ImBuf suitable for GPU preview drawing.
 * RGB channels come from `src`, alpha channel from the 1-channel `mask` (0..1).
 * This lets the GPU alpha-blend only the pixels inside the lasso shape, so the
 * preview respects the actual selection outline rather than showing the full bbox.
 */
static ImBuf *image_select_move_make_display_ibuf(const ImBuf *src, const ImBuf *mask)
{
  BLI_assert(src && mask);
  BLI_assert(mask->float_buffer.data);

  ImBuf *disp = IMB_allocImBuf(src->x, src->y, ImBufFlags::Zero);
  IMB_alloc_float_pixels(disp, 4);
  disp->channels = 4;

  const int pixel_count = src->x * src->y;
  float *dst = disp->float_data_for_write();
  const float *fmask = mask->float_data();

  if (src->float_buffer.data) {
    const int ch = src->channels ? src->channels : 4;
    const float *src_f = src->float_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      dst[i * 4 + 0] = (ch > 0) ? src_f[i * ch + 0] : 0.0f;
      dst[i * 4 + 1] = (ch > 1) ? src_f[i * ch + 1] : 0.0f;
      dst[i * 4 + 2] = (ch > 2) ? src_f[i * ch + 2] : 0.0f;
      dst[i * 4 + 3] = fmask[i];
    }
  }
  else if (src->byte_buffer.data) {
    /* Byte images are always 4 bytes per pixel (RGBA) in Blender's ImBuf. */
    const uint8_t *src_b = src->byte_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      dst[i * 4 + 0] = src_b[i * 4 + 0] / 255.0f;
      dst[i * 4 + 1] = src_b[i * 4 + 1] / 255.0f;
      dst[i * 4 + 2] = src_b[i * 4 + 2] / 255.0f;
      dst[i * 4 + 3] = fmask[i];
    }
  }

  return disp;
}

/**
 * Region post-pixel draw callback: renders the fragment pixel data at the destination position
 * with a yellow outline, giving a live preview of the move result.
 * The source region is already transparent on the canvas (cleared in lift_source).
 * Registered in invoke, removed on exit.
 */
static void draw_select_move_preview(const bContext *C, ARegion *region, void *arg)
{
  const ImageSelectMoveState *state = static_cast<const ImageSelectMoveState *>(arg);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image || !state->fragment_ibuf) {
    return;
  }

  ImageUser iuser = state->iuser;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(sima->image, &iuser, &lock);
  if (!ibuf) {
    return;
  }

  /* Destination origin in tile-pixel coordinates. */
  const int dst_x = state->origin_px.x + state->drag_offset.x;
  const int dst_y = state->origin_px.y + state->drag_offset.y;

  /* Convert destination bbox to normalised UV (0..1 for single tile). */
  const float v_x0 = float(dst_x) / float(ibuf->x);
  const float v_y0 = float(dst_y) / float(ibuf->y);
  const float v_x1 = float(dst_x + state->size_px.x) / float(ibuf->x);
  const float v_y1 = float(dst_y + state->size_px.y) / float(ibuf->y);

  BKE_image_release_ibuf(sima->image, ibuf, lock);

  /* Convert UV to region (screen) coordinates. */
  float sx0, sy0, sx1, sy1;
  ui::view2d_view_to_region_fl(&region->v2d, v_x0, v_y0, &sx0, &sy0);
  ui::view2d_view_to_region_fl(&region->v2d, v_x1, v_y1, &sx1, &sy1);

  /* Pixel scale: how many screen pixels correspond to one fragment pixel. */
  const float zoom_x = (sx1 - sx0) / float(state->size_px.x);
  const float zoom_y = (sy1 - sy0) / float(state->size_px.y);

  /* Draw fragment pixel data directly to the linear viewport framebuffer, without any inline
   * color management. The viewport CM is applied as a post-process during wm_draw_region_blit,
   * so calling ED_draw_imbuf (which applies CM inline) would cause double conversion тАФ the
   * root cause of the yellow tint seen in the preview. */
  GPU_blend(GPU_BLEND_ALPHA);

  {
    PixelBitmapDrawer drawer(GPU_SHADER_3D_IMAGE_COLOR);
    /* Use the display buffer (mask baked into alpha) when available so the GPU
     * alpha-blends only the lasso-selected pixels, not the full bbox rectangle. */
    const ImBuf *frag = state->fragment_display_ibuf ? state->fragment_display_ibuf :
                                                       state->fragment_ibuf;
    if (frag->float_buffer.data) {
      const int ch = frag->channels ? frag->channels : 4;
      const gpu::TextureFormat fmt = (ch >= 4) ? gpu::TextureFormat::SFLOAT_16_16_16_16 :
                                     (ch == 3) ? gpu::TextureFormat::SFLOAT_16_16_16 :
                                                 gpu::TextureFormat::SFLOAT_16;
      drawer.draw(sx0, sy0, frag->x, frag->y, fmt, true,
                  frag->float_buffer.data, zoom_x, zoom_y, nullptr);
    }
    else if (frag->byte_buffer.data) {
      drawer.draw(sx0, sy0, frag->x, frag->y, gpu::TextureFormat::UNORM_8_8_8_8, true,
                  frag->byte_buffer.data, zoom_x, zoom_y, nullptr);
    }
  }

  /* Draw destination bbox outline on top of the pixel preview. */
  GPU_line_smooth(true);

  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4f(1.0f, 0.85f, 0.0f, 0.9f);

  immBegin(GPU_PRIM_LINE_LOOP, 4);
  immVertex2f(pos, sx0, sy0);
  immVertex2f(pos, sx1, sy0);
  immVertex2f(pos, sx1, sy1);
  immVertex2f(pos, sx0, sy1);
  immEnd();

  immUnbindProgram();
  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
}

/**
 * Extract the bounding-box region of the selection into a new ImBuf.
 * Does not modify the source ibuf тАФ pixels are cleared only at commit time.
 * Returns nullptr on failure (no selection, no pixel data, zero-size bbox).
 * Supports both float and byte image buffers.
 *
 * On success writes tile-pixel bbox into r_origin_px and r_size_px.
 */
static ImBuf *image_select_move_extract(wmOperator *op,
                                        Image *ima,
                                        ImageUser *iuser,
                                        int tile_number,
                                        int2 *r_origin_px,
                                        int2 *r_size_px,
                                        ImBuf **r_fragment_mask)
{
  ImBuf **mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(tile_number);
  if (!mask_ptr || !*mask_ptr) {
    BKE_report(op->reports, RPT_ERROR, "No selection on active tile");
    return nullptr;
  }

  int x_min, y_min, x_max, y_max;
  if (!image_selection_bbox(*mask_ptr, &x_min, &y_min, &x_max, &y_max)) {
    BKE_report(op->reports, RPT_ERROR, "Selection is empty");
    return nullptr;
  }

  const int w = x_max - x_min;
  const int h = y_max - y_min;

  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, &lock);
  if (!ibuf) {
    BKE_report(op->reports, RPT_ERROR, "Could not acquire image buffer");
    return nullptr;
  }

  const bool is_float = ibuf->float_buffer.data != nullptr;
  if (!is_float && !ibuf->byte_buffer.data) {
    BKE_report(op->reports, RPT_ERROR, "Image has no pixel data");
    BKE_image_release_ibuf(ima, ibuf, lock);
    return nullptr;
  }

  /* Clamp bbox to ibuf bounds (safety: mask and ibuf dimensions should match). */
  const int safe_x = std::max(x_min, 0);
  const int safe_y = std::max(y_min, 0);
  const int safe_w = std::min(w, ibuf->x - safe_x);
  const int safe_h = std::min(h, ibuf->y - safe_y);

  if (safe_w <= 0 || safe_h <= 0) {
    BKE_report(op->reports, RPT_ERROR, "Selection bounding box is out of image bounds");
    BKE_image_release_ibuf(ima, ibuf, lock);
    return nullptr;
  }

  /* Allocate fragment matching the source buffer type so IMB_copy_rect works correctly. */
  ImBuf *fragment = IMB_allocImBuf(safe_w, safe_h, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  if (!fragment) {
    BKE_image_release_ibuf(ima, ibuf, lock);
    return nullptr;
  }
  if (is_float) {
    /* Ensure channel count matches the source so IMB_copy_rect uses the correct stride. */
    fragment->channels = ibuf->channels ? ibuf->channels : 4;
  }

  /* Copy source rectangle тЖТ fragment origin (0, 0). */
  IMB_copy_rect(fragment, ibuf, int2{safe_x, safe_y}, int2{0, 0}, int2{safe_w, safe_h});

  /* Keep the fragment's colorspace consistent with the source image.
   * IMB_allocImBuf assigns default spaces which may differ from the source image. */
  if (is_float) {
    const ColorSpace *cs = ibuf->float_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_float_colorspace(fragment,
                                                  IMB_colormanagement_colorspace_get_name(cs));
    }
  }
  else {
    const ColorSpace *cs = ibuf->byte_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_byte_colorspace(fragment,
                                                 IMB_colormanagement_colorspace_get_name(cs));
    }
  }

  BKE_image_release_ibuf(ima, ibuf, lock);

  *r_origin_px = int2{safe_x, safe_y};
  *r_size_px = int2{safe_w, safe_h};

  /* Extract the mask sub-region so pixel operations respect the actual lasso shape. */
  if (r_fragment_mask) {
    ImBuf *fmask = IMB_allocImBuf(safe_w, safe_h, ImBufFlags::Zero);
    IMB_alloc_float_pixels(fmask, 1);
    const float *src_mask = (*mask_ptr)->float_data();
    float *dst_mask = fmask->float_data_for_write();
    const int full_w = (*mask_ptr)->x;
    for (int y = 0; y < safe_h; y++) {
      for (int x = 0; x < safe_w; x++) {
        dst_mask[y * safe_w + x] = src_mask[(safe_y + y) * full_w + (safe_x + x)];
      }
    }
    *r_fragment_mask = fmask;
  }

  return fragment;
}

/**
 * Immediately clear the source region from the canvas and selection mask, giving the user
 * visual feedback that the fragment has been lifted. Pushes the undo begin marker тАФ the
 * caller must ensure ED_image_undo_push_end() is called exactly once on commit or cancel.
 * Sets state->undo_begun on success.
 */
static void image_select_move_lift_source(bContext *C, ImageSelectMoveState *state)
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
  ED_image_undo_push_begin_with_image("Move Selection", ima, ibuf, &iuser);
  /* Also snapshot the selection mask so it is restored together with the pixels on undo. */
  ED_image_undo_capture_selection_mask(ima, state->tile_number);
  state->undo_begun = true;

  /* Clear source pixels тАФ respect the actual selection mask shape (e.g. lasso). */
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
        if (fmask && fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
        if (fmask && fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
  ImBuf **mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(state->tile_number);
  if (mask_ptr && *mask_ptr) {
    ImBuf *mask = *mask_ptr;
    float *mdata = mask->float_data_for_write();
    if (mdata) {
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = state->origin_px.y + ly;
        if (py < 0 || py >= mask->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          if (fmask && fmask[ly * state->size_px.x + lx] <= 0.5f) {
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

/**
 * Restore the source pixels and mask from the extracted fragment (used on cancel).
 * Closes the undo step opened in lift_source тАФ the step is a no-op since the restored state
 * matches the pre-lift state captured at undo push-begin.
 */
static void image_select_move_restore_source(bContext *C, ImageSelectMoveState *state)
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

    /* Paste fragment back at source origin тАФ only selected pixels (mask-aware). */
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
            if (fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
            if (fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
  ImBuf **mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(state->tile_number);
  if (mask_ptr && *mask_ptr) {
    ImBuf *mask = *mask_ptr;
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
    BKE_undosys_stack_clear_active(ED_undo_stack_get());
  }

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

/**
 * Commit the move: blit the floating fragment to the destination and update the selection mask.
 * Source pixels were already cleared by lift_source on invoke.
 * Closes the undo step opened there тАФ undoing this step restores both source and destination.
 * Pixels that fall outside the canvas boundary are clipped, not shifted back.
 */
static void image_select_move_commit(bContext *C, ImageSelectMoveState *state)
{
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    if (state->undo_begun) {
      ED_image_undo_push_end();
    }
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
    if (state->undo_begun) {
      ED_image_undo_push_end();
    }
    return;
  }

  /* Raw destination тАФ no clamping. Per-pixel bounds checks below handle clipping. */
  const int2 dst = {
      state->origin_px.x + state->drag_offset.x,
      state->origin_px.y + state->drag_offset.y,
  };

  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

  /* Blit only the selected pixels (mask-aware) to the destination. */
  const float *fmask = state->fragment_mask_ibuf ? state->fragment_mask_ibuf->float_buffer.data :
                                                    nullptr;
  if (fmask) {
    const bool is_float = ibuf->float_data() != nullptr;
    if (is_float) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *dst_data = ibuf->float_data_for_write();
      const float *src_data = state->fragment_ibuf->float_data();
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = dst.y + ly;
        if (py < 0 || py >= ibuf->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          if (fmask[ly * state->size_px.x + lx] <= 0.5f) {
            continue;
          }
          const int px = dst.x + lx;
          if (px < 0 || px >= ibuf->x) {
            continue;
          }
          memcpy(dst_data + (py * ibuf->x + px) * channels,
                 src_data + (ly * state->size_px.x + lx) * channels,
                 channels * sizeof(float));
        }
      }
    }
    else if (ibuf->byte_data()) {
      uint8_t *dst_data = ibuf->byte_data_for_write();
      const uint8_t *src_data = state->fragment_ibuf->byte_data();
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = dst.y + ly;
        if (py < 0 || py >= ibuf->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          if (fmask[ly * state->size_px.x + lx] <= 0.5f) {
            continue;
          }
          const int px = dst.x + lx;
          if (px < 0 || px >= ibuf->x) {
            continue;
          }
          memcpy(dst_data + (py * ibuf->x + px) * 4, src_data + (ly * state->size_px.x + lx) * 4, 4);
        }
      }
    }
  }
  else {
    /* Fallback: no mask. Clip the fragment to canvas bounds before blit so IMB_copy_rect
     * never receives out-of-range coordinates. */
    const int clip_x0 = std::max(0, dst.x);
    const int clip_y0 = std::max(0, dst.y);
    const int clip_x1 = std::min(ibuf->x, dst.x + state->size_px.x);
    const int clip_y1 = std::min(ibuf->y, dst.y + state->size_px.y);
    if (clip_x0 < clip_x1 && clip_y0 < clip_y1) {
      const int2 src_offset = {clip_x0 - dst.x, clip_y0 - dst.y};
      const int2 dst_offset = {clip_x0, clip_y0};
      const int2 clip_size = {clip_x1 - clip_x0, clip_y1 - clip_y0};
      IMB_copy_rect(ibuf, state->fragment_ibuf, src_offset, dst_offset, clip_size);
    }
  }
  BKE_image_mark_dirty(ima, ibuf);

  /* Update selection mask at destination using the actual lasso shape. */
  ImBuf **mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(state->tile_number);
  if (mask_ptr && *mask_ptr) {
    ImBuf *mask = *mask_ptr;
    float *mdata = mask->float_data_for_write();
    if (mdata) {
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = dst.y + ly;
        if (py < 0 || py >= mask->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          const int px = dst.x + lx;
          if (px < 0 || px >= mask->x) {
            continue;
          }
          mdata[py * mask->x + px] = fmask ? fmask[ly * state->size_px.x + lx] : 1.0f;
        }
      }
    }
  }

  /* Mark the destination region dirty, clipped to canvas bounds. */
  rcti dirty;
  BLI_rcti_init(&dirty,
                std::max(0, dst.x),
                std::min(ibuf->x, dst.x + state->size_px.x),
                std::max(0, dst.y),
                std::min(ibuf->y, dst.y + state->size_px.y));
  const ImageTile *tile = BKE_image_get_tile(ima, state->tile_number);
  BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);

  BKE_image_release_ibuf(ima, ibuf, lock);

  if (state->undo_begun) {
    ED_image_undo_push_end();
  }

  /* Free cached GPU textures so the Image Editor reloads from the modified ibuf. */
  BKE_image_free_gputextures(ima);

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

/**
 * Free all operator state and remove the preview draw callback.
 * Safe to call with a partially initialised state (checks for nullptr).
 */
static void image_select_move_state_free(ImageSelectMoveState *state)
{
  if (!state) {
    return;
  }
  if (state->draw_handle && state->owner_region_type) {
    ED_region_draw_cb_exit(state->owner_region_type, state->draw_handle);
    state->draw_handle = nullptr;
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select All
 * \{ */

static wmOperatorStatus image_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Select All" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);

  /* Only select the active tile, not all tiles. */
  ImageUser iuser = sima->iuser;
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(image, &iuser);
  if (!active_tile) {
    return OPERATOR_CANCELLED;
  }
  iuser.tile = active_tile->tile_number;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  if (!ibuf) {
    return OPERATOR_CANCELLED;
  }

  ED_image_undo_push_begin_selection("Select All", image);

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  ImBuf *mask = BKE_image_paint_selection_mask_get(
      image, active_tile->tile_number, ibuf->x, ibuf->y);
  float *data = mask->float_data_for_write();
  const int total = mask->x * mask->y;
  for (int i = 0; i < total; i++) {
    data[i] = 1.0f;
  }

  BKE_image_release_ibuf(image, ibuf, nullptr);

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_all(wmOperatorType *ot)
{
  ot->name = "Select All";
  ot->idname = "PAINT_OT_image_select_all";
  ot->description = "Select the entire image as a paint mask";
  ot->exec = image_select_all_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  /* RNA properties allow hotkey display in menus and operator properties. */
  RNA_def_boolean(ot->srna,
                  "action",
                  true,
                  "Action",
                  "Action to perform (always selects all for this operator)");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select None
 * \{ */

static wmOperatorStatus image_select_none_exec(bContext *C, wmOperator * /*op*/)
{
  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Select None" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  Image *image = sima ? sima->image : nullptr;

  if (image) {
    ED_image_undo_push_begin_selection("Select None", image);
  }

  if (image) {
    BKE_image_paint_selection_mask_free(image);
  }

  Scene *scene = CTX_data_scene(C);
  scene->toolsettings->imapaint.use_selection_mask = 0;

  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (image) {
    ED_image_undo_push_end();
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_none(wmOperatorType *ot)
{
  ot->name = "Select None";
  ot->idname = "PAINT_OT_image_select_none";
  ot->description = "Deselect the entire image (remove paint mask)";
  ot->exec = image_select_none_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  /* RNA properties allow hotkey display in menus and operator properties. */
  RNA_def_boolean(ot->srna,
                  "action",
                  true,
                  "Action",
                  "Action to perform (always deselects all for this operator)");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invert Selection
 * \{ */

static wmOperatorStatus image_select_invert_exec(bContext *C, wmOperator * /*op*/)
{
  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Invert Selection" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  SpaceImage *sima = CTX_wm_space_image(C);
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;

  ED_image_undo_push_begin_selection("Invert Selection", image);

  imapaint->use_selection_mask = 1;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
    float *data = mask->float_data_for_write();
    const int total = mask->x * mask->y;
    for (int i = 0; i < total; i++) {
      data[i] = 1.0f - data[i];
    }

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_invert(wmOperatorType *ot)
{
  ot->name = "Invert Selection";
  ot->idname = "PAINT_OT_image_select_invert";
  ot->description = "Invert the current paint selection mask";
  ot->exec = image_select_invert_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  /* RNA properties allow hotkey display in menus and operator properties. */
  RNA_def_boolean(ot->srna,
                  "action",
                  true,
                  "Action",
                  "Action to perform (always inverts selection for this operator)");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Box
 * \{ */

static wmOperatorStatus image_select_box_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  rctf rectf;
  WM_operator_properties_border_to_rctf(op, &rectf);

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  if (is_simple_click ||
      (BLI_rctf_size_x(&rectf) == 0.0f && BLI_rctf_size_y(&rectf) == 0.0f))
  {
    if (g_floating_state) {
      image_select_move_commit(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
    ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
    if (imapaint->use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  ui::view2d_region_to_view_rctf(&region->v2d, &rectf, &rectf);

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  ED_image_undo_push_begin_selection("Box Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  printf("[DEBUG] box_select_exec: Processing tiles. rect=[%.2f,%.2f]-[%.2f,%.2f]\n",
         rectf.xmin, rectf.ymin, rectf.xmax, rectf.ymax);

  int tiles_processed = 0;
  int pixels_filled = 0;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      printf("[DEBUG] box_select_exec: Tile %d - no ibuf\n", tile->tile_number);
      continue;
    }

    /* UDIM tile UV origin. */
    const float uv_origin[2] = {float((tile->tile_number - 1001) % 10),
                                float((tile->tile_number - 1001) / 10)};

    /* Check if tile UV rect [origin, origin+1] intersects with box. */
    rctf tile_rect;
    tile_rect.xmin = uv_origin[0];
    tile_rect.xmax = uv_origin[0] + 1.0f;
    tile_rect.ymin = uv_origin[1];
    tile_rect.ymax = uv_origin[1] + 1.0f;

    printf("[DEBUG] box_select_exec: Checking tile %d, uv_origin=[%.2f,%.2f], tile_rect=[%.2f,%.2f]-[%.2f,%.2f]\n",
           tile->tile_number, uv_origin[0], uv_origin[1],
           tile_rect.xmin, tile_rect.ymin, tile_rect.xmax, tile_rect.ymax);

    if (BLI_rctf_isect(&tile_rect, &rectf, &tile_rect)) {
      printf("[DEBUG] box_select_exec: INTERSECT! Tile %d, intersected rect=[%.2f,%.2f]-[%.2f,%.2f]\n",
             tile->tile_number, tile_rect.xmin, tile_rect.ymin, tile_rect.xmax, tile_rect.ymax);

      printf("[DEBUG] box_select_exec: Getting mask for tile %d. Canvas size: %dx%d\n",
             tile->tile_number, ibuf->x, ibuf->y);
      ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      float *data = mask->float_data_for_write();

      const int x1 = int(roundf((tile_rect.xmin - uv_origin[0]) * mask->x));
      const int y1 = int(roundf((tile_rect.ymin - uv_origin[1]) * mask->y));
      const int x2 = int(roundf((tile_rect.xmax - uv_origin[0]) * mask->x));
      const int y2 = int(roundf((tile_rect.ymax - uv_origin[1]) * mask->y));
      const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;

      printf("[DEBUG] box_select_exec: Fill coords (%d,%d)-(%d,%d), fill_value=%.1f\n",
             x1, y1, x2, y2, fill_value);

      int tile_pixels = 0;
      for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
          if (x >= 0 && x < mask->x && y >= 0 && y < mask->y) {
            data[y * mask->x + x] = fill_value;
            tile_pixels++;
          }
        }
      }
      printf("[DEBUG] box_select_exec: Filled %d pixels in tile %d\n", tile_pixels, tile->tile_number);
      pixels_filled += tile_pixels;
      tiles_processed++;
    } else {
      printf("[DEBUG] box_select_exec: NO intersect for tile %d\n", tile->tile_number);
    }

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  printf("[DEBUG] box_select_exec: SUMMARY - Processed %d tiles, filled %d total pixels\n",
         tiles_processed, pixels_filled);

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  ED_region_tag_redraw(region);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_box_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus image_select_box_modal(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (event->type == MOUSEMOVE && gesture) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > 3 || abs(event->xy[1] - start_y) > 3) {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
  return WM_gesture_box_modal(C, op, event);
}

void PAINT_OT_image_select_box(wmOperatorType *ot)
{
  ot->name = "Select Box";
  ot->idname = "PAINT_OT_image_select_box";
  ot->description = "Select a rectangular region as a paint mask";

  ot->invoke = image_select_box_invoke;
  ot->modal = image_select_box_modal;
  ot->exec = image_select_box_exec;
  ot->cancel = WM_gesture_box_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_box(ot);
  WM_operator_properties_select_operation_simple(ot);

  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Lasso
 * \{ */

/**
 * Scanline polygon fill for a 1-channel float buffer.
 * Fills the interior of \a points with \a color.
 * Requires at least 3 points; silently does nothing otherwise.
 */
static void fill_polygon_float(ImBuf *ibuf, Span<int2> points, float color)
{
  if (points.size() < 3) {
    return;
  }

  const int width = ibuf->x;
  const int height = ibuf->y;

  int min_y = points[0].y;
  int max_y = points[0].y;
  for (const int2 &p : points) {
    min_y = min_ii(min_y, p.y);
    max_y = max_ii(max_y, p.y);
  }
  min_y = max_ii(min_y, 0);
  max_y = min_ii(max_y, height - 1);

  Vector<int> intersections;
  for (int y = min_y; y <= max_y; y++) {
    intersections.clear();
    const int n = points.size();
    for (int i = 0; i < n; i++) {
      const int next = (i + 1) % n;
      const int y1 = points[i].y;
      const int y2 = points[next].y;
      if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
        const float x = float(y - y1) * float(points[next].x - points[i].x) /
                            float(y2 - y1) +
                        float(points[i].x);
        intersections.append(int(x));
      }
    }
    std::sort(intersections.begin(), intersections.end());

    for (int i = 0; i + 1 < int(intersections.size()); i += 2) {
      const int x1 = max_ii(intersections[i], 0);
      const int x2 = min_ii(intersections[i + 1], width - 1);
      float *row = ibuf->float_data_for_write() + y * width;
      for (int x = x1; x <= x2; x++) {
        row[x] = color;
      }
    }
  }
}

static wmOperatorStatus image_select_lasso_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return WM_gesture_lasso_invoke(C, op, event);
}

static wmOperatorStatus image_select_lasso_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (event->type == MOUSEMOVE && gesture) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > 3 || abs(event->xy[1] - start_y) > 3) {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
  return WM_gesture_lasso_modal(C, op, event);
}

static wmOperatorStatus image_select_lasso_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  if (is_simple_click) {
    if (g_floating_state) {
      image_select_move_commit(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
    if (scene->toolsettings->imapaint.use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Array<int2> mcoords = WM_gesture_lasso_path_to_array(C, op);
  if (mcoords.is_empty() || int(mcoords.size()) < 3) {
    if (scene->toolsettings->imapaint.use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  ED_image_undo_push_begin_selection("Lasso Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  /* Convert lasso points to UV space once. */
  Vector<float2> uv_points;
  uv_points.reserve(mcoords.size());
  for (const int2 &p : mcoords) {
    float co[2] = {float(p.x), float(p.y)};
    ui::view2d_region_to_view(&region->v2d, co[0], co[1], &co[0], &co[1]);
    uv_points.append(float2(co[0], co[1]));
  }

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    /* UDIM tile UV origin. */
    const float uv_origin[2] = {float((tile->tile_number - 1001) % 10),
                                float((tile->tile_number - 1001) / 10)};

    ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);

    /* Convert UV points to tile pixel space. */
    Vector<int2> tile_points;
    tile_points.reserve(uv_points.size());
    for (const float2 &uv : uv_points) {
      tile_points.append(
          int2(int(roundf((uv.x - uv_origin[0]) * mask->x)), int(roundf((uv.y - uv_origin[1]) * mask->y))));
    }

    const float color = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
    fill_polygon_float(mask, tile_points, color);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_lasso(wmOperatorType *ot)
{
  ot->name = "Select Lasso";
  ot->idname = "PAINT_OT_image_select_lasso";
  ot->description = "Select a freehand region as a paint mask";

  ot->invoke = image_select_lasso_invoke;
  ot->modal = image_select_lasso_modal;
  ot->exec = image_select_lasso_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_lasso(ot);
  WM_operator_properties_select_operation_simple(ot);

  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Circle
 * \{ */

/**
 * Rasterize a filled circle into a 1-channel float buffer.
 */
static void fill_circle_float(ImBuf *ibuf, int cx, int cy, int radius, float color)
{
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int x1 = max_ii(cx - radius, 0);
  const int x2 = min_ii(cx + radius, width - 1);
  const int y1 = max_ii(cy - radius, 0);
  const int y2 = min_ii(cy + radius, height - 1);
  const float r2 = float(radius) * float(radius);

  for (int y = y1; y <= y2; y++) {
    const float dy = float(y - cy);
    const float dy2 = dy * dy;
    for (int x = x1; x <= x2; x++) {
      const float dx = float(x - cx);
      if (dx * dx + dy2 <= r2) {
        ibuf->float_data_for_write()[y * width + x] = color;
      }
    }
  }
}

static wmOperatorStatus image_select_circle_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return WM_gesture_circle_invoke(C, op, event);
}

static wmOperatorStatus image_select_circle_modal(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (event->type == MOUSEMOVE && gesture) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > 3 || abs(event->xy[1] - start_y) > 3) {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
  return WM_gesture_circle_modal(C, op, event);
}

static wmOperatorStatus image_select_circle_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  const int mradius = RNA_int_get(op->ptr, "radius");

  if (is_simple_click || mradius <= 0) {
    if (g_floating_state) {
      image_select_move_commit(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
    if (scene->toolsettings->imapaint.use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  ED_image_undo_push_begin_selection("Circle Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  const int mx = RNA_int_get(op->ptr, "x");
  const int my = RNA_int_get(op->ptr, "y");

  /* Convert center to UV space. */
  float co_center[2] = {float(mx), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_center[0], co_center[1], &co_center[0], &co_center[1]);

  /* Convert radius to UV space: measure a point one radius to the right. */
  float co_edge[2] = {float(mx + mradius), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_edge[0], co_edge[1], &co_edge[0], &co_edge[1]);
  const float uv_radius = co_edge[0] - co_center[0];

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    /* UDIM tile UV origin. */
    const float uv_origin[2] = {float((tile->tile_number - 1001) % 10),
                                float((tile->tile_number - 1001) / 10)};

    ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);

    const int cx = int(roundf((co_center[0] - uv_origin[0]) * mask->x));
    const int cy = int(roundf((co_center[1] - uv_origin[1]) * mask->y));
    int rx = int(roundf(uv_radius * mask->x));
    if (rx <= 0) {
      rx = 1;
    }

    const float color = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
    fill_circle_float(mask, cx, cy, rx, color);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_circle(wmOperatorType *ot)
{
  ot->name = "Select Circle";
  ot->idname = "PAINT_OT_image_select_circle";
  ot->description = "Select a circular region as a paint mask";

  ot->invoke = image_select_circle_invoke;
  ot->modal = image_select_circle_modal;
  ot->exec = image_select_circle_exec;
  ot->cancel = WM_gesture_circle_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_circle(ot);
  WM_operator_properties_select_operation_simple(ot);

  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Select Move Operator
 * \{ */

/* Returns true when a selection fragment is currently floating (lifted but not yet committed). */
static bool image_select_move_floating_poll(bContext * /*C*/)
{
  return g_floating_state != nullptr;
}

static bool image_select_move_poll(bContext *C)
{
  const wmWindow *win = CTX_wm_window(C);
  const wmEvent *ev = (win && win->runtime) ? win->runtime->eventstate : nullptr;
  const bool log_lmb = ev && ev->type == LEFTMOUSE && ev->val == KM_PRESS;

  if (image_select_transform_is_floating(C)) {
    if (log_lmb) {
      IMG_SEL_DBG("move_poll: FAIL (transform floating, blocks move)");
    }
    return false;
  }
  /* Re-drag an already-floating fragment тАФ no selection needed. */
  if (g_floating_state) {
    SpaceImage *sima = CTX_wm_space_image(C);
    if (sima && sima == g_floating_state->owner_sima) {
      if (log_lmb) {
        IMG_SEL_DBG("move_poll: OK (re-drag floating move)");
      }
      return true;
    }
  }
  if (!image_paint_selection_poll(C)) {
    if (log_lmb) {
      IMG_SEL_DBG("move_poll: FAIL (image_paint_selection_poll)");
    }
    return false;
  }
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima->image) {
    if (log_lmb) {
      IMG_SEL_DBG("move_poll: FAIL (no image)");
    }
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  if (!scene->toolsettings->imapaint.use_selection_mask) {
    CTX_wm_operator_poll_msg_set(C, "No active selection mask");
    if (log_lmb) {
      IMG_SEL_DBG("move_poll: FAIL (no selection mask)");
    }
    return false;
  }
  if (log_lmb) {
    IMG_SEL_DBG("move_poll: OK (new move)");
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Initial drag modal (runs only while LMB is held after invoke)
 * \{ */

static void image_select_move_update_drag_offset(bContext * /*C*/,
                                                 ImageSelectMoveState *state,
                                                 const wmEvent *event,
                                                 ARegion *region)
{
  SpaceImage *sima = state->owner_sima;
  if (!sima || !sima->image) {
    state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};
    return;
  }
  Image *ima = sima->image;
  ImageUser iuser = state->iuser;
  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  if (ibuf) {
    float prev_view_x, prev_view_y, cur_view_x, cur_view_y;
    ui::view2d_region_to_view(
        &region->v2d, state->prev_mouse_xy.x, state->prev_mouse_xy.y, &prev_view_x, &prev_view_y);
    ui::view2d_region_to_view(
        &region->v2d, event->xy[0], event->xy[1], &cur_view_x, &cur_view_y);
    state->drag_offset.x += int((cur_view_x - prev_view_x) * float(ibuf->x));
    state->drag_offset.y += int((cur_view_y - prev_view_y) * float(ibuf->y));
    BKE_image_release_ibuf(ima, ibuf, lock);
  }
  state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};
}

static wmOperatorStatus image_select_move_modal(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  ImageSelectMoveState *state = g_floating_state;
  if (!state) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  /* image_select_move_undo_step_exec sets is_dragging=false to abort the active drag. */
  if (!state->is_dragging) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ARegion *region = CTX_wm_region(C);

  switch (event->type) {
    case MOUSEMOVE:
      image_select_move_update_drag_offset(C, state, event, region);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;

    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        /* Drag finished тАФ fragment stays floating, modal ends.
         * Navigation (pan/zoom) is fully available between drag gestures. */
        state->is_dragging = false;
        op->customdata = nullptr;
        IMG_SEL_DBG("move_modal: LMB release -> FINISHED (still floating)");
        return OPERATOR_FINISHED;
      }
      IMG_SEL_DBG("move_modal: LMB press (dragging=%d)", int(state->is_dragging));
      return OPERATOR_RUNNING_MODAL;

    case RIGHTMOUSE:
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        IMG_SEL_DBG("move_modal: cancel");
        image_select_move_restore_source(C, state);
        image_select_move_state_free(state);
        g_floating_state = nullptr;
        op->customdata = nullptr;
        ED_region_tag_redraw(region);
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;

    default:
      return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }
}

static void image_select_move_cancel(bContext *C, wmOperator *op)
{
  /* Called by WM when the modal is forcibly removed (e.g. area close, mode change). */
  if (g_floating_state) {
    image_select_move_restore_source(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }
  op->customdata = nullptr;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invoke тАФ extract fragment, start initial drag
 * \{ */

static wmOperatorStatus image_select_move_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  image_select_debug_log_event("move_invoke", event);
  IMG_SEL_DBG("move_invoke: g_floating=%p g_transform=%p transform_floating=%d",
              (void *)g_floating_state,
              (void *)g_transform_state,
              int(image_select_transform_is_floating(C)));

  /* Re-drag path: a fragment is already floating in this space тАФ start another drag gesture.
   * Push the current position before the new drag so Ctrl+Z can step back to it. */
  if (g_floating_state && g_floating_state->owner_sima == sima) {
    g_floating_state->drag_position_history.append(g_floating_state->drag_offset);
    g_floating_state->is_dragging = true;
    g_floating_state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};
    op->customdata = g_floating_state;
    WM_event_add_modal_handler(C, op);
    IMG_SEL_DBG("move_invoke -> %s (re-drag)", image_select_debug_op_status(OPERATOR_RUNNING_MODAL));
    return OPERATOR_RUNNING_MODAL;
  }

  /* If a floating fragment from a different space is still alive, commit it. */
  if (g_floating_state) {
    IMG_SEL_DBG("move_invoke: committing stale floating move from other space");
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }
  Image *ima = sima->image;

  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(ima, &sima->iuser);
  const int tile_number = active_tile ? active_tile->tile_number : 1001;

  ImageUser iuser = sima->iuser;
  iuser.tile = tile_number;

  auto *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->tile_number = tile_number;
  state->iuser = iuser;
  state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};

  state->fragment_ibuf = image_select_move_extract(
      op, ima, &iuser, tile_number, &state->origin_px, &state->size_px, &state->fragment_mask_ibuf);
  if (!state->fragment_ibuf) {
    IMG_SEL_DBG("move_invoke -> CANCELLED (extract failed)");
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }

  if (state->fragment_mask_ibuf) {
    state->fragment_display_ibuf = image_select_move_make_display_ibuf(state->fragment_ibuf,
                                                                       state->fragment_mask_ibuf);
  }

  image_select_move_lift_source(C, state);

  /* The initial invoke is itself the first drag gesture: snapshot the pre-drag position
   * (origin, i.e. {0,0} offset) so the first Ctrl+Z can undo back to it. */
  state->drag_position_history.append(state->drag_offset);
  state->is_dragging = true;

  ARegion *region = CTX_wm_region(C);
  state->owner_region_type = region->runtime->type;
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_move_preview, state, REGION_DRAW_POST_PIXEL);

  g_floating_state = state;
  op->customdata = state;
  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(region);
  IMG_SEL_DBG("move_invoke -> %s (new floating, size_px=%d,%d)",
              image_select_debug_op_status(OPERATOR_RUNNING_MODAL),
              state->size_px.x,
              state->size_px.y);
  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Confirm / Cancel operators (non-modal, keymap-driven)
 * \{ */

static wmOperatorStatus image_select_move_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  if (!g_floating_state) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *state = g_floating_state;
  g_floating_state = nullptr;
  image_select_move_commit(C, state);
  image_select_move_state_free(state);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_move_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  if (!g_floating_state) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *state = g_floating_state;
  g_floating_state = nullptr;
  image_select_move_restore_source(C, state);
  image_select_move_state_free(state);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Undo step within floating selection (Ctrl+Z, keymap-driven)
 * \{ */

/**
 * Step back one drag gesture while the selection is floating.
 * Bound to Ctrl+Z in the Image Editor keymap; the poll gates it so normal
 * WM undo fires when no fragment is floating.
 * When a drag is currently active the operator also signals the drag modal
 * to abort (via is_dragging = false) so the modal exits on its next event.
 */
static wmOperatorStatus image_select_move_undo_step_exec(bContext *C, wmOperator * /*op*/)
{
  ImageSelectMoveState *state = g_floating_state;
  if (!state) {
    return OPERATOR_CANCELLED;
  }

  ARegion *region = CTX_wm_region(C);

  if (state->is_dragging) {
    /* Abort the active drag and revert to the position saved before it started. */
    state->drag_offset = state->drag_position_history.pop_last();
    state->is_dragging = false;
    if (region) {
      ED_region_tag_redraw(region);
    }
    return OPERATOR_FINISHED;
  }

  if (!state->drag_position_history.is_empty()) {
    /* Step back one completed gesture. */
    state->drag_offset = state->drag_position_history.pop_last();
    if (region) {
      ED_region_tag_redraw(region);
    }
    return OPERATOR_FINISHED;
  }

  /* History exhausted тАФ restore source pixels and end the floating operation. */
  g_floating_state = nullptr;
  image_select_move_restore_source(C, state);
  image_select_move_state_free(state);
  if (region) {
    ED_region_tag_redraw(region);
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy Selection Operator (Ctrl+C)
 * \{ */

/**
 * Poll: copy is available when there is an active selection mask.
 */
static bool image_select_copy_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    printf("[DEBUG] copy_poll: No image in context\n");
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  bool has_mask = scene->toolsettings->imapaint.use_selection_mask != 0;
  printf("[DEBUG] copy_poll: has_mask=%d\n", has_mask);
  return has_mask;
}

/**
 * Extract the selected region's pixels and mask, store in local clipboard,
 * and copy pixels to system clipboard.
 */
static wmOperatorStatus image_select_copy_exec(bContext *C, wmOperator *op)
{
  printf("[DEBUG] image_select_copy_exec CALLED\n");
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    printf("[DEBUG] image_select_copy_exec: No image in context\n");
    BKE_report(op->reports, RPT_ERROR, "No image in context");
    return OPERATOR_CANCELLED;
  }
  printf("[DEBUG] image_select_copy_exec: Image found: %s\n", sima->image->id.name);

  Image *ima = sima->image;
  ImBuf *canvas_ibuf = nullptr;
  void *canvas_lock = nullptr;

  /* Acquire the image buffer for the current tile. */
  canvas_ibuf = BKE_image_acquire_ibuf(ima, &sima->iuser, &canvas_lock);
  if (!canvas_ibuf) {
    printf("[DEBUG] copy_exec: Cannot acquire image buffer\n");
    BKE_report(op->reports, RPT_ERROR, "Cannot acquire image buffer");
    return OPERATOR_CANCELLED;
  }
  printf("[DEBUG] copy_exec: Canvas buffer acquired. Size: %dx%d\n", canvas_ibuf->x, canvas_ibuf->y);

  /* Get the selection mask for this tile. */
  ImageTile *tile = BKE_image_get_tile_from_iuser(ima, &sima->iuser);
  if (!tile) {
    printf("[DEBUG] copy_exec: Cannot get tile from ImageUser\n");
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot determine image tile");
    return OPERATOR_CANCELLED;
  }

  int tile_number = tile->tile_number;
  printf("[DEBUG] copy_exec: Getting mask for tile %d. Canvas size: %dx%d\n",
         tile_number, canvas_ibuf->x, canvas_ibuf->y);
  ImBuf *mask_ibuf = BKE_image_paint_selection_mask_get(
      ima, tile_number, canvas_ibuf->x, canvas_ibuf->y);
  if (!mask_ibuf) {
    printf("[DEBUG] copy_exec: Cannot acquire selection mask. Tile: %d\n", tile_number);
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot acquire selection mask");
    return OPERATOR_CANCELLED;
  }
  printf("[DEBUG] copy_exec: Mask buffer acquired. Size: %dx%d\n", mask_ibuf->x, mask_ibuf->y);

  /* Find the bounding box of the selection. */
  int x_min, y_min, x_max, y_max;
  if (!image_selection_bbox(mask_ibuf, &x_min, &y_min, &x_max, &y_max)) {
    printf("[DEBUG] copy_exec: Selection bbox is empty or not found\n");
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_WARNING, "Selection is empty");
    return OPERATOR_CANCELLED;
  }
  printf("[DEBUG] copy_exec: Selection bbox found: (%d,%d) - (%d,%d)\n", x_min, y_min, x_max, y_max);

  /* Extract fragment from canvas at selection bounds. */
  int frag_width = x_max - x_min;
  int frag_height = y_max - y_min;

  const bool is_float = canvas_ibuf->float_buffer.data != nullptr;
  ImBuf *frag_ibuf = IMB_allocImBuf(frag_width, frag_height, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  if (!frag_ibuf) {
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot allocate fragment buffer");
    return OPERATOR_CANCELLED;
  }

  if (is_float) {
    frag_ibuf->channels = canvas_ibuf->channels ? canvas_ibuf->channels : 4;
  }

  /* Copy source rectangle тЖТ fragment origin (0, 0). */
  IMB_copy_rect(frag_ibuf, canvas_ibuf, int2{x_min, y_min}, int2{0, 0}, int2{frag_width, frag_height});

  /* Keep the fragment's colorspace consistent with the source image. */
  if (is_float) {
    const ColorSpace *cs = canvas_ibuf->float_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_float_colorspace(frag_ibuf,
                                                  IMB_colormanagement_colorspace_get_name(cs));
    }
  }
  else {
    const ColorSpace *cs = canvas_ibuf->byte_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_byte_colorspace(frag_ibuf,
                                                 IMB_colormanagement_colorspace_get_name(cs));
    }
  }

  /* Extract fragment mask. */
  ImBuf *frag_mask_ibuf = IMB_allocImBuf(frag_width, frag_height, ImBufFlags::Zero);
  IMB_alloc_float_pixels(frag_mask_ibuf, 1);
  float *dst_mask = frag_mask_ibuf->float_data_for_write();
  const float *src_mask = mask_ibuf->float_data();

  for (int y = 0; y < frag_height; y++) {
    for (int x = 0; x < frag_width; x++) {
      dst_mask[y * frag_width + x] = src_mask[(y_min + y) * canvas_ibuf->x + (x_min + x)];
    }
  }

  BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);

  printf("[DEBUG] copy_exec: Extracted fragment %dx%d, releasing canvas\n", frag_width, frag_height);

  /* Copy fragment to system clipboard. */
  printf("[DEBUG] copy_exec: Calling WM_clipboard_image_set_byte_buffer\n");
  if (!WM_clipboard_image_set_byte_buffer(frag_ibuf)) {
    printf("[DEBUG] copy_exec: WM_clipboard_image_set_byte_buffer FAILED\n");
    IMB_freeImBuf(frag_ibuf);
    IMB_freeImBuf(frag_mask_ibuf);
    BKE_report(op->reports, RPT_WARNING, "Failed to set system clipboard");
    return OPERATOR_CANCELLED;
  }
  printf("[DEBUG] copy_exec: WM_clipboard_image_set_byte_buffer SUCCESS\n");

  /* Store in local clipboard with mask. */
  printf("[DEBUG] copy_exec: Calling image_clipboard_state_set with tile_number=%d\n", tile_number);
  image_clipboard_state_set(frag_ibuf, frag_mask_ibuf, int2{x_min, y_min},
                            tile_number, true);
  printf("[DEBUG] copy_exec: Local clipboard state set\n");

  printf("[DEBUG] image_select_copy_exec: Selection copied successfully. Size: %dx%d\n",
         frag_width, frag_height);
  BKE_report(op->reports, RPT_INFO, "Selection copied");
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paste Selection Operator (Ctrl+V)
 * \{ */

/**
 * Poll: paste is available if there is local clipboard data or system clipboard data.
 */
static bool image_select_paste_poll(bContext *C)
{
  if (!CTX_wm_space_image(C)) {
    printf("[DEBUG] paste_poll: No SpaceImage in context\n");
    return false;
  }

  /* Have local clipboard with mask. */
  if (g_clipboard_state && g_clipboard_state->has_mask) {
    printf("[DEBUG] paste_poll: Using LOCAL clipboard (has mask)\n");
    return true;
  }

  /* Have system clipboard. */
  printf("[DEBUG] paste_poll: Checking system clipboard...\n");
  ImBuf *test_ibuf = WM_clipboard_image_get();
  if (test_ibuf) {
    printf("[DEBUG] paste_poll: System clipboard HAS IMAGE. Size: %dx%d\n", test_ibuf->x, test_ibuf->y);
    IMB_freeImBuf(test_ibuf);
    return true;
  }

  printf("[DEBUG] paste_poll: System clipboard EMPTY\n");
  return false;
}

/**
 * Paste from local clipboard (with mask) or system clipboard.
 * Initializes a floating selection that can be moved and confirmed/cancelled.
 */
static wmOperatorStatus image_select_paste_exec(bContext *C, wmOperator *op)
{
  printf("[DEBUG] image_select_paste_exec CALLED\n");
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    printf("[DEBUG] image_select_paste_exec: No image in context\n");
    return OPERATOR_CANCELLED;
  }
  printf("[DEBUG] image_select_paste_exec: Image found: %s\n", sima->image->id.name);

  ImBuf *paste_ibuf = nullptr;
  ImBuf *paste_mask_ibuf = nullptr;
  int2 paste_origin = {0, 0};
  bool has_mask = false;
  bool owns_buffers = false;

  /* Check local clipboard first (has mask). */
  printf("[DEBUG] paste_exec: Checking local clipboard state. g_clipboard_state=%p\n", (void*)g_clipboard_state);
  if (g_clipboard_state && g_clipboard_state->has_mask) {
    printf("[DEBUG] paste_exec: Using LOCAL clipboard buffer\n");
    /* Copy buffers from clipboard state to avoid lifetime issues. */
    paste_ibuf = IMB_dupImBuf(g_clipboard_state->fragment_ibuf);
    printf("[DEBUG] paste_exec: Duplicated fragment buffer. paste_ibuf=%p\n", (void*)paste_ibuf);
    if (g_clipboard_state->fragment_mask_ibuf) {
      paste_mask_ibuf = IMB_dupImBuf(g_clipboard_state->fragment_mask_ibuf);
      printf("[DEBUG] paste_exec: Duplicated mask buffer. paste_mask_ibuf=%p\n", (void*)paste_mask_ibuf);
    }
    paste_origin = g_clipboard_state->origin_px;
    has_mask = true;
    owns_buffers = true;
  }
  else {
    printf("[DEBUG] paste_exec: Local clipboard NOT available, using SYSTEM clipboard\n");
    /* Fall back to system clipboard (no mask, center on canvas). */
    ImBuf *sys_ibuf = WM_clipboard_image_get();
    if (!sys_ibuf) {
      printf("[DEBUG] paste_exec: System clipboard is EMPTY\n");
      BKE_report(op->reports, RPT_ERROR, "No image in clipboard");
      return OPERATOR_CANCELLED;
    }
    printf("[DEBUG] paste_exec: Got system clipboard buffer. Size: %dx%d\n", sys_ibuf->x, sys_ibuf->y);
    /* Copy system clipboard buffer to prevent lifetime issues. */
    paste_ibuf = IMB_dupImBuf(sys_ibuf);
    IMB_freeImBuf(sys_ibuf);
    if (!paste_ibuf) {
      BKE_report(op->reports, RPT_ERROR, "Failed to allocate paste buffer");
      return OPERATOR_CANCELLED;
    }
    /* Acquire canvas to center the paste. */
    void *lock = nullptr;
    ImBuf *canvas_ibuf = BKE_image_acquire_ibuf(sima->image, &sima->iuser, &lock);
    if (canvas_ibuf) {
      paste_origin.x = (canvas_ibuf->x - paste_ibuf->x) / 2;
      paste_origin.y = (canvas_ibuf->y - paste_ibuf->y) / 2;
      BKE_image_release_ibuf(sima->image, canvas_ibuf, lock);
    }
    has_mask = false;
    owns_buffers = true;
  }

  if (!paste_ibuf) {
    return OPERATOR_CANCELLED;
  }

  /* Get the correct tile number for this paste operation. */
  ImageTile *paste_tile = BKE_image_get_tile_from_iuser(sima->image, &sima->iuser);
  if (!paste_tile) {
    printf("[DEBUG] paste_exec: Cannot get tile from ImageUser\n");
    IMB_freeImBuf(paste_ibuf);
    if (paste_mask_ibuf) {
      IMB_freeImBuf(paste_mask_ibuf);
    }
    BKE_report(op->reports, RPT_ERROR, "Cannot determine image tile");
    return OPERATOR_CANCELLED;
  }
  int paste_tile_number = paste_tile->tile_number;
  printf("[DEBUG] paste_exec: paste_tile_number=%d\n", paste_tile_number);

  /* Create floating selection state from clipboard. */
  ImageSelectMoveState *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->fragment_ibuf = paste_ibuf;
  state->fragment_mask_ibuf = paste_mask_ibuf;
  state->origin_px = paste_origin;
  state->size_px = {paste_ibuf->x, paste_ibuf->y};
  state->drag_offset = {0, 0};
  state->tile_number = paste_tile_number;
  state->iuser = sima->iuser;

  /* If clipboard has a mask, create display buffer for lasso preview. */
  if (has_mask && paste_mask_ibuf) {
    state->fragment_display_ibuf = image_select_move_make_display_ibuf(paste_ibuf, paste_mask_ibuf);
  }

  /* Register draw callback for preview. */
  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->type) {
    state->owner_region_type = region->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, draw_select_move_preview, state, REGION_DRAW_POST_PIXEL);
  }

  /* Begin undo step for the floating selection. */
  ED_image_undo_push_begin("Paste Selection", PaintMode::Texture2D);
  state->undo_begun = true;

  g_floating_state = state;

  printf("[DEBUG] image_select_paste_exec: Floating state created. Size: %dx%d, Origin: (%d, %d)\n",
         paste_ibuf->x, paste_ibuf->y, paste_origin.x, paste_origin.y);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  /* Activate Move Selection tool for convenient positioning. */
  printf("[DEBUG] image_select_paste_exec: Attempting to activate Move Selection tool\n");
  WM_operator_name_call(C, "PAINT_OT_image_select_move", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
  printf("[DEBUG] image_select_paste_exec: Move Selection tool called\n");

  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator type registrations
 * \{ */

void PAINT_OT_image_select_move(wmOperatorType *ot)
{
  ot->name = "Move Selection";
  ot->idname = "PAINT_OT_image_select_move";
  ot->description = "Lift the selected region and start moving it";

  ot->invoke = image_select_move_invoke;
  ot->modal = image_select_move_modal;
  ot->cancel = image_select_move_cancel;
  ot->poll = image_select_move_poll;

  /* No OPTYPE_BLOCKING: modal only runs during the first LMB drag.
   * No OPTYPE_UNDO: undo is managed by ED_image_undo_push_begin/end inside lift_source and
   * commit. Adding OPTYPE_UNDO here would cause WM to call BKE_undosys_step_push when the
   * modal finishes (LMB release), which prematurely finalizes the open image undo step
   * (step_init тЖТ nullptr). The subsequent ED_image_undo_push_end in commit would then call
   * BKE_undosys_type_from_context(nullptr) and crash. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_move_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Selection Move";
  ot->idname = "PAINT_OT_image_select_move_confirm";
  ot->description = "Apply the moved fragment to the canvas";

  ot->exec = image_select_move_confirm_exec;
  ot->poll = image_select_move_floating_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void PAINT_OT_image_select_move_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Selection Move";
  ot->idname = "PAINT_OT_image_select_move_cancel";
  ot->description = "Restore the fragment to its original position and discard the move";

  ot->exec = image_select_move_cancel_exec;
  ot->poll = image_select_move_floating_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void PAINT_OT_image_select_move_undo_step(wmOperatorType *ot)
{
  ot->name = "Undo Selection Move Step";
  ot->idname = "PAINT_OT_image_select_move_undo_step";
  ot->description = "Step back one drag gesture; when history is empty, restores the source";

  ot->exec = image_select_move_undo_step_exec;
  ot->poll = image_select_move_floating_poll;

  /* No OPTYPE_UNDO: the image undo step remains open across all gestures and is managed
   * internally; WM must not push an extra step here. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_copy(wmOperatorType *ot)
{
  ot->name = "Copy Selection";
  ot->idname = "PAINT_OT_image_select_copy";
  ot->description = "Copy the selected region to clipboard";

  ot->exec = image_select_copy_exec;
  ot->poll = image_select_copy_poll;

  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_paste(wmOperatorType *ot)
{
  ot->name = "Paste Selection";
  ot->idname = "PAINT_OT_image_select_paste";
  ot->description = "Paste from clipboard; creates a floating selection that can be moved";

  ot->exec = image_select_paste_exec;
  ot->poll = image_select_paste_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** Update screen-space handle positions for hit-testing (also called from draw preview). */
static void image_select_transform_cache_screen_coords(ImageSelectTransformState *state, ARegion *region)
{
  if (!state || !state->fragment_ibuf || !region) {
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

  const float2 pivot = state->anchor;
  const float cos_r = cosf(state->rotation);
  const float sin_r = sinf(state->rotation);

  auto apply_forward = [&](float2 local) -> float2 {
    float2 global = float2(state->origin_px) + local;
    float2 centered = global - pivot;
    float2 scaled = centered * state->scale;
    float2 rotated = float2(scaled.x * cos_r - scaled.y * sin_r,
                            scaled.x * sin_r + scaled.y * cos_r);
    return rotated + pivot + state->translation;
  };

  float2 local_corners[4] = {
      float2(0.0f, 0.0f),
      float2(state->size_px.x, 0.0f),
      float2(state->size_px.x, state->size_px.y),
      float2(0.0f, state->size_px.y),
  };

  float2 scr_coords[4];
  for (int i = 0; i < 4; i++) {
    float2 tile_co = apply_forward(local_corners[i]);
    float uv_x = tile_co.x / canvas_w;
    float uv_y = tile_co.y / canvas_h;
    int rx, ry;
    ui::view2d_view_to_region(&region->v2d, uv_x, uv_y, &rx, &ry);
    scr_coords[i] = float2(rx, ry);
    state->screen_corners[i] = scr_coords[i];
  }

  state->screen_midpoints[0] = 0.5f * (scr_coords[0] + scr_coords[1]);
  state->screen_midpoints[1] = 0.5f * (scr_coords[1] + scr_coords[2]);
  state->screen_midpoints[2] = 0.5f * (scr_coords[2] + scr_coords[3]);
  state->screen_midpoints[3] = 0.5f * (scr_coords[3] + scr_coords[0]);
  state->screen_center = 0.25f * (scr_coords[0] + scr_coords[1] + scr_coords[2] + scr_coords[3]);

  float2 top_normal = math::normalize(scr_coords[2] - scr_coords[1]);
  state->screen_rotate_handle = state->screen_midpoints[2] + top_normal * 35.0f;

  int arx, ary;
  ui::view2d_view_to_region(
      &region->v2d, (state->anchor.x + state->translation.x) / canvas_w, (state->anchor.y + state->translation.y) / canvas_h, &arx, &ary);
  state->screen_anchor = float2(arx, ary);
}

static void draw_select_transform_preview(const bContext * /*C*/, ARegion *region, void *clientdata)
{
  ImageSelectTransformState *state = static_cast<ImageSelectTransformState *>(clientdata);
  if (!state || !state->fragment_ibuf) {
    return;
  }

  image_select_transform_cache_screen_coords(state, region);

  float2 scr_coords[4];
  for (int i = 0; i < 4; i++) {
    scr_coords[i] = state->screen_corners[i];
  }

  GPU_blend(GPU_BLEND_ALPHA);

  /* --- 1. Draw Textured Fragment Preview --- */
  if (!state->fragment_tex) {
    state->fragment_tex = IMB_create_gpu_texture(
        "TransformFragment", state->fragment_ibuf, true, true, false);
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

  /* All remaining drawing uses GPU_SHADER_3D_UNIFORM_COLOR with a pos-only format. */
  {
    GPUVertFormat *fmt = immVertexFormat();
    const uint pos = GPU_vertformat_attr_add(fmt, "pos", gpu::VertAttrType::SFLOAT_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    /* --- 2. Draw Bounding Box Outline --- */
    GPU_line_width(1.0f);
    immUniformColor4f(1.0f, 0.85f, 0.0f, 0.9f); /* Yellow outline */

    immBegin(GPU_PRIM_LINE_LOOP, 4);
    for (int i = 0; i < 4; i++) {
      immVertex2f(pos, scr_coords[i].x, scr_coords[i].y);
    }
    immEnd();

    /* Line from top-midpoint to rotation handle */
    immBegin(GPU_PRIM_LINES, 2);
    immVertex2f(pos, state->screen_midpoints[2].x, state->screen_midpoints[2].y);
    immVertex2f(pos, state->screen_rotate_handle.x, state->screen_rotate_handle.y);
    immEnd();

    /* --- 3. Draw Handle Squares --- */
    /* Helper: draw a filled square as two triangles. */
    auto draw_handle_square = [&](float2 center, float size) {
      const float r = size * 0.5f;
      const float x0 = center.x - r, x1 = center.x + r;
      const float y0 = center.y - r, y1 = center.y + r;
      immBegin(GPU_PRIM_TRIS, 6);
      immVertex2f(pos, x0, y0);
      immVertex2f(pos, x1, y0);
      immVertex2f(pos, x1, y1);
      immVertex2f(pos, x0, y0);
      immVertex2f(pos, x1, y1);
      immVertex2f(pos, x0, y1);
      immEnd();
    };

    immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f); /* White handles */
    for (int i = 0; i < 4; i++) {
      draw_handle_square(state->screen_corners[i], 8.0f);
    }
    for (int i = 0; i < 4; i++) {
      draw_handle_square(state->screen_midpoints[i], 8.0f);
    }
    draw_handle_square(state->screen_rotate_handle, 8.0f);

    /* --- 4. Snap Feedback --- */
    if (state->is_snapped) {
      immUniformColor4f(0.0f, 1.0f, 0.2f, 0.7f);
      draw_handle_square(state->snap_indicator_screen, 12.0f);
    }

    /* --- 5. Anchor Crosshair --- */
    immUniformColor4f(0.1f, 0.6f, 1.0f, 1.0f); /* Cyan pivot */
    draw_handle_square(state->screen_anchor, 6.0f);

    immBegin(GPU_PRIM_LINES, 4);
    immVertex2f(pos, state->screen_anchor.x - 12.0f, state->screen_anchor.y);
    immVertex2f(pos, state->screen_anchor.x + 12.0f, state->screen_anchor.y);
    immVertex2f(pos, state->screen_anchor.x, state->screen_anchor.y - 12.0f);
    immVertex2f(pos, state->screen_anchor.x, state->screen_anchor.y + 12.0f);
    immEnd();

    /* --- 6. Tooltip Background (when dragging anchor) --- */
    if (state->active_handle == ImageSelectTransformState::HANDLE_ANCHOR) {
      const float tx = state->screen_anchor.x + 15.0f;
      const float ty = state->screen_anchor.y - 25.0f;
      const float tw = 160.0f;
      const float th = 20.0f;

      immUniformColor4f(0.0f, 0.0f, 0.0f, 0.65f);
      immBegin(GPU_PRIM_TRIS, 6);
      immVertex2f(pos, tx, ty);
      immVertex2f(pos, tx + tw, ty);
      immVertex2f(pos, tx + tw, ty + th);
      immVertex2f(pos, tx, ty);
      immVertex2f(pos, tx + tw, ty + th);
      immVertex2f(pos, tx, ty + th);
      immEnd();
    }

    immUnbindProgram();

    /* --- 6b. Tooltip Text --- */
    if (state->active_handle == ImageSelectTransformState::HANDLE_ANCHOR) {
      char tooltip[64];
      snprintf(tooltip, sizeof(tooltip), "Pivot: X: %.1f px, Y: %.1f px",
               state->anchor.x, state->anchor.y);
      const float tx = state->screen_anchor.x + 15.0f;
      const float ty = state->screen_anchor.y - 25.0f;
      BLF_size(blf_mono_font, 11.0f * UI_SCALE_FAC);
      BLF_position(blf_mono_font, tx + 8.0f, ty + 5.0f, 0.0f);
      BLF_color4f(blf_mono_font, 1.0f, 1.0f, 1.0f, 1.0f);
      BLF_draw(blf_mono_font, tooltip, strlen(tooltip));
    }
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

  /* Clear source pixels — respect the actual selection mask shape (e.g. lasso). */
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
        if (fmask && fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
        if (fmask && fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
  ImBuf **mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(state->tile_number);
  if (mask_ptr && *mask_ptr) {
    ImBuf *mask = *mask_ptr;
    float *mdata = mask->float_data_for_write();
    if (mdata) {
      for (int ly = 0; ly < state->size_px.y; ly++) {
        const int py = state->origin_px.y + ly;
        if (py < 0 || py >= mask->y) {
          continue;
        }
        for (int lx = 0; lx < state->size_px.x; lx++) {
          if (fmask && fmask[ly * state->size_px.x + lx] <= 0.5f) {
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

    /* Paste fragment back at source origin — only selected pixels (mask-aware). */
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
            if (fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
            if (fmask[ly * state->size_px.x + lx] <= 0.5f) {
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
  ImBuf **mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(state->tile_number);
  if (mask_ptr && *mask_ptr) {
    ImBuf *mask = *mask_ptr;
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
    BKE_undosys_stack_clear_active(ED_undo_stack_get());
  }

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

static void image_select_transform_state_free(ImageSelectTransformState *state)
{
  if (!state) {
    return;
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
  MEM_delete(state);
}

bool image_select_transform_is_floating(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return g_transform_state != nullptr && sima && g_transform_state->owner_sima == sima;
}

static bool image_select_transform_floating_poll(bContext *C)
{
  const bool ok = image_select_transform_is_floating(C);
  const wmWindow *win = CTX_wm_window(C);
  const wmEvent *ev = (win && win->runtime) ? win->runtime->eventstate : nullptr;
  if (ev && ev->type == LEFTMOUSE && ELEM(ev->val, KM_PRESS, KM_PRESS_DRAG)) {
    IMG_SEL_DBG("transform_drag_poll: %s (g_transform=%p)", ok ? "OK" : "FAIL", (void *)g_transform_state);
  }
  return ok;
}

static bool image_select_transform_poll(bContext *C)
{
  const wmWindow *win = CTX_wm_window(C);
  const wmEvent *ev = (win && win->runtime) ? win->runtime->eventstate : nullptr;
  const bool log_ev = ev && ELEM(ev->type, LEFTMOUSE, EVT_TKEY) && ev->val == KM_PRESS;

  if (g_transform_state) {
    SpaceImage *sima = CTX_wm_space_image(C);
    if (sima && sima == g_transform_state->owner_sima) {
      if (log_ev) {
        IMG_SEL_DBG("transform_poll: OK (already floating in this space)");
      }
      return true;
    }
  }
  if (!image_paint_selection_poll(C)) {
    if (log_ev) {
      IMG_SEL_DBG("transform_poll: FAIL (image_paint_selection_poll)");
    }
    return false;
  }
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima->image) {
    if (log_ev) {
      IMG_SEL_DBG("transform_poll: FAIL (no image)");
    }
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  if (!scene->toolsettings->imapaint.use_selection_mask) {
    CTX_wm_operator_poll_msg_set(C, "No active selection mask");
    if (log_ev) {
      IMG_SEL_DBG("transform_poll: FAIL (no selection mask)");
    }
    return false;
  }
  if (log_ev) {
    IMG_SEL_DBG("transform_poll: OK (can start transform)");
  }
  return true;
}

static void image_select_transform_cancel(bContext *C, wmOperator *op)
{
  /* Use g_transform_state directly: with the long-lived modal, op->customdata may already
   * be stale if confirm/cancel exec ran before the modal received its final event. */
  if (g_transform_state) {
    image_select_transform_restore_source(C, g_transform_state);
    image_select_transform_state_free(g_transform_state);
    g_transform_state = nullptr;
  }
  op->customdata = nullptr;
}

static void image_select_transform_commit(bContext *C, ImageSelectTransformState *state);

static wmOperatorStatus image_select_transform_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  if (!g_transform_state) {
    IMG_SEL_DBG("transform_confirm: CANCELLED (no state)");
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = g_transform_state;
  g_transform_state = nullptr;
  image_select_transform_commit(C, state);
  image_select_transform_state_free(state);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  IMG_SEL_DBG("transform_confirm: FINISHED");
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_transform_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  if (!g_transform_state) {
    IMG_SEL_DBG("transform_cancel: CANCELLED (no state)");
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = g_transform_state;
  g_transform_state = nullptr;
  image_select_transform_restore_source(C, state);
  image_select_transform_state_free(state);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  IMG_SEL_DBG("transform_cancel: FINISHED");
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

  /* Rotation handle sits outside the quad — test before corners/move (see cage2d_gizmo). */
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

static void image_select_debug_log_handle_hit(const char *caller,
                                              const ImageSelectTransformState *state,
                                              const float2 &mouse_co,
                                              const ImageSelectTransformState::HandleType hit)
{
  const float hit_half = image_select_transform_handle_hit_px(state) * 0.5f;
  IMG_SEL_DBG(
      "%s: hit=%s mval=(%.1f,%.1f) hit_half=%.1f corner0=(%.1f,%.1f) rotate=(%.1f,%.1f)",
      caller,
      image_select_debug_handle_name(hit),
      mouse_co.x,
      mouse_co.y,
      hit_half,
      state->screen_corners[0].x,
      state->screen_corners[0].y,
      state->screen_rotate_handle.x,
      state->screen_rotate_handle.y);
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

static void image_select_transform_commit(bContext *C, ImageSelectTransformState *state)
{
  SpaceImage *sima = state->owner_sima;
  if (!sima || !sima->image || !state->fragment_ibuf) {
    return;
  }
  Image *ima = sima->image;
  ImageUser iuser = state->iuser;

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  if (!ibuf) {
    return;
  }

  const bool is_float = ibuf->float_buffer.data != nullptr;
  /* temp_pixels must match the fragment buffer type so IMB_transform works (it does not
   * support mixed src/dst types, e.g. byte source → float destination). */
  ImBuf *temp_pixels = IMB_allocImBuf(
      ibuf->x, ibuf->y, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  ImBuf *temp_mask = IMB_allocImBuf(ibuf->x, ibuf->y, ImBufFlags::FloatData);
  temp_pixels->channels = 4;
  /* temp_mask keeps channels = 4 (default) because IMB_transform requires dst->channels == 4. */
  IMB_rectfill_alpha(temp_pixels, 0.0f);
  IMB_rectfill_alpha(temp_mask, 0.0f);

  /* Compute inverse matrix mapping global tile dst to global tile src.
   * Forward transform: fragment_local → canvas_global
   *   X_dst = R·S·(origin_px + X_local - anchor) + anchor + translation
   * IMB_transform ожидает backward матрицу: canvas_global → fragment_local
   *
   * Строим forward 3x3 матрицу явно в однородных координатах:
   *   M = T(anchor + translation) · R(θ) · S · T(-anchor) · T(origin_px)
   * Затем берём инверсию.
   */
  const float2 pivot_commit = state->anchor;
  const float cos_r_commit = cosf(state->rotation);
  const float sin_r_commit = sinf(state->rotation);

  /* Строим forward матрицу 3x3 (2D affine в однородных координатах):
   * Колонки: [cos*sx, sin*sx, tx], [-sin*sy, cos*sy, ty], [0, 0, 1]
   * где tx, ty — полное смещение с учётом pivot и translation */
  float3x3 forward_matrix;
  {
    /* R·S часть */
    const float a = cos_r_commit * state->scale.x;
    const float b = -sin_r_commit * state->scale.y;
    const float c = sin_r_commit * state->scale.x;
    const float d = cos_r_commit * state->scale.y;

    /* Полное смещение: pivot + translation - R·S·(pivot - origin_px) */
    const float2 rs_pivot = float2(
        a * (pivot_commit.x - float(state->origin_px.x)) + b * (pivot_commit.y - float(state->origin_px.y)),
        c * (pivot_commit.x - float(state->origin_px.x)) + d * (pivot_commit.y - float(state->origin_px.y)));
    const float tx = pivot_commit.x + state->translation.x - rs_pivot.x;
    const float ty = pivot_commit.y + state->translation.y - rs_pivot.y;

    /* Матрица в column-major (Blender float3x3): forward_matrix[col][row] */
    forward_matrix[0] = float3(a, c, 0.0f);   /* col 0 */
    forward_matrix[1] = float3(b, d, 0.0f);   /* col 1 */
    forward_matrix[2] = float3(tx, ty, 1.0f); /* col 2 */
  }
  const float3x3 backward_matrix = math::invert(forward_matrix);

  /* Transform fragment pixels and mask into temporary tile buffer */
  rctf src_crop{0.0f, float(state->size_px.x), 0.0f, float(state->size_px.y)};
  IMB_transform(state->fragment_ibuf, temp_pixels, IMB_TRANSFORM_MODE_CROP_SRC,
                IMB_FILTER_BILINEAR, backward_matrix, &src_crop);

  IMB_transform(state->fragment_mask_ibuf, temp_mask, IMB_TRANSFORM_MODE_CROP_SRC,
                IMB_FILTER_BILINEAR, backward_matrix, &src_crop);

  /* Alpha-blend transformed fragment onto canvas.
   * Blend factor = mask * fragment_alpha so partially-transparent pixels are respected. */
  const float *mask_data = temp_mask->float_buffer.data;
  const int total_pixels = ibuf->x * ibuf->y;

  if (ibuf->float_data() && temp_pixels->float_buffer.data) {
    const float *frag_data = temp_pixels->float_buffer.data;
    float *canvas = ibuf->float_data_for_write();
    const int ch = ibuf->channels ? ibuf->channels : 4;
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
        canvas[i * ch + 3] = (1.0f - blend) * canvas[i * ch + 3] + blend * frag_data[i * 4 + 3];
      }
    }
  }
  else if (ibuf->byte_data() && temp_pixels->byte_buffer.data) {
    const uint8_t *frag_data = temp_pixels->byte_buffer.data;
    uint8_t *canvas = ibuf->byte_data_for_write();
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
      canvas[i * 4 + 0] = uint8_t(
          std::clamp((1.0f - blend) * canvas[i * 4 + 0] + blend * frag_data[i * 4 + 0], 0.0f, 255.0f));
      canvas[i * 4 + 1] = uint8_t(
          std::clamp((1.0f - blend) * canvas[i * 4 + 1] + blend * frag_data[i * 4 + 1], 0.0f, 255.0f));
      canvas[i * 4 + 2] = uint8_t(
          std::clamp((1.0f - blend) * canvas[i * 4 + 2] + blend * frag_data[i * 4 + 2], 0.0f, 255.0f));
      canvas[i * 4 + 3] = uint8_t(
          std::clamp((1.0f - blend) * canvas[i * 4 + 3] + blend * frag_data[i * 4 + 3], 0.0f, 255.0f));
    }
  }

  /* Merge selection mask back into runtime mask */
  ImBuf **global_mask_ptr = ima->runtime->paint_selection_masks.lookup_ptr(state->tile_number);
  if (global_mask_ptr && *global_mask_ptr) {
    float *g_mask = (*global_mask_ptr)->float_data_for_write();
    for (int i = 0; i < total_pixels; i++) {
      g_mask[i] = std::clamp(g_mask[i] + mask_data[i * 4 + 0], 0.0f, 1.0f);
    }
    (*global_mask_ptr)->userflags |= IB_DISPLAY_BUFFER_INVALID;
  }

  IMB_freeImBuf(temp_pixels);
  IMB_freeImBuf(temp_mask);

  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(ima, ibuf);
  
  BKE_image_partial_update_mark_full_update(ima);
  BKE_image_release_ibuf(ima, ibuf, lock);
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
  image_select_transform_begin_handle_drag(state, event, hit);
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
  image_select_debug_log_event("transform_drag_invoke", event);
  if (!g_transform_state || g_transform_state->owner_sima != sima) {
    IMG_SEL_DBG("transform_drag_invoke -> CANCELLED (no floating state for this space)");
    return OPERATOR_CANCELLED;
  }

  ImageSelectTransformState *state = g_transform_state;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    image_select_transform_cache_screen_coords(state, region);
  }
  else {
    IMG_SEL_DBG("transform_drag_invoke: WARNING region=null, screen coords stale");
  }

  const float2 mouse_co(event->mval[0], event->mval[1]);
  const ImageSelectTransformState::HandleType hit = get_active_handle_hit(state, mouse_co);
  image_select_debug_log_handle_hit("transform_drag_invoke", state, mouse_co, hit);
  if (hit == ImageSelectTransformState::HANDLE_NONE) {
    IMG_SEL_DBG("transform_drag_invoke -> PASS_THROUGH (miss)");
    return OPERATOR_PASS_THROUGH;
  }

  const wmOperatorStatus ret = image_select_transform_start_drag_gesture(C, op, state, event, hit);
  IMG_SEL_DBG("transform_drag_invoke -> %s handle=%s",
              image_select_debug_op_status(ret),
              image_select_debug_handle_name(hit));
  return ret;
}

static wmOperatorStatus image_select_transform_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  image_select_debug_log_event("transform_invoke", event);
  IMG_SEL_DBG("transform_invoke: g_transform=%p g_floating_move=%p",
              (void *)g_transform_state,
              (void *)g_floating_state);

  if (g_transform_state && g_transform_state->owner_sima == sima) {
    ImageSelectTransformState *state = g_transform_state;
    ARegion *region = CTX_wm_region(C);
    if (region) {
      image_select_transform_cache_screen_coords(state, region);
    }

    const float2 mouse_co(event->mval[0], event->mval[1]);
    const ImageSelectTransformState::HandleType hit = get_active_handle_hit(state, mouse_co);
    image_select_debug_log_handle_hit("transform_invoke re-float", state, mouse_co, hit);

    /* Re-drag via keyboard re-invoke: only start drag on handle/quad hit. */
    if (hit == ImageSelectTransformState::HANDLE_NONE) {
      IMG_SEL_DBG("transform_invoke -> PASS_THROUGH (re-float, no hit)");
      return OPERATOR_PASS_THROUGH;
    }

    const wmOperatorStatus ret = image_select_transform_start_drag_gesture(C, op, state, event, hit);
    IMG_SEL_DBG("transform_invoke -> %s (re-float drag)", image_select_debug_op_status(ret));
    return ret;
  }

  /* If a transform is already active in a different space, commit it first. */
  if (g_transform_state) {
    IMG_SEL_DBG("transform_invoke: commit stale transform in other space");
    image_select_transform_restore_source(C, g_transform_state);
    image_select_transform_state_free(g_transform_state);
    g_transform_state = nullptr;
  }

  Image *ima = sima->image;
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(ima, &sima->iuser);
  const int tile_number = active_tile ? active_tile->tile_number : 1001;

  ImageUser iuser = sima->iuser;
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

  state->fragment_ibuf = image_select_move_extract(
      op, ima, &iuser, tile_number, &state->origin_px, &state->size_px, &state->fragment_mask_ibuf);
  if (!state->fragment_ibuf) {
    IMG_SEL_DBG("transform_invoke -> CANCELLED (extract failed)");
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }

  state->anchor = float2(state->origin_px.x + state->size_px.x * 0.5f,
                         state->origin_px.y + state->size_px.y * 0.5f);

  image_select_transform_lift_source(C, state);

  ARegion *region = CTX_wm_region(C);
  state->owner_region_type = region->runtime->type;

  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_transform_preview, state, REGION_DRAW_POST_PIXEL);

  g_transform_state = state;
  op->customdata = nullptr;
  state->is_dragging = false;
  state->active_handle = ImageSelectTransformState::HANDLE_NONE;

  /* Enter floating mode as a long-lived modal so LMB handle hits have the highest
   * event priority (modal > tool keymaps).  Miss events pass through to box-select etc. */
  ED_region_tag_redraw(region);
  IMG_SEL_DBG("transform_invoke -> RUNNING_MODAL (floating, size_px=%d,%d)",
              state->size_px.x,
              state->size_px.y);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus image_select_transform_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  ImageSelectTransformState *state = g_transform_state;
  if (!state) {
    IMG_SEL_DBG("transform_modal: no state -> FINISHED");
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ARegion *region = CTX_wm_region(C);

  if (!state->is_dragging) {
    /* Floating state: intercept LMB, Enter, Esc directly; pass everything else through so
     * pan/zoom, box-select and other operators still work normally. */
    if (event->type == LEFTMOUSE && ELEM(event->val, KM_PRESS, KM_PRESS_DRAG)) {
      if (region) {
        image_select_transform_cache_screen_coords(state, region);
      }
      const float2 mouse_co(event->mval[0], event->mval[1]);
      const ImageSelectTransformState::HandleType hit = get_active_handle_hit(state, mouse_co);
      image_select_debug_log_handle_hit("transform_modal float LMB", state, mouse_co, hit);
      if (hit != ImageSelectTransformState::HANDLE_NONE) {
        image_select_transform_begin_handle_drag(state, event, hit);
        state->is_dragging = true;
        op->customdata = state;
        if (region) {
          ED_region_tag_redraw(region);
        }
        IMG_SEL_DBG("transform_modal float: drag start handle=%s",
                    image_select_debug_handle_name(hit));
        return OPERATOR_RUNNING_MODAL;
      }
      IMG_SEL_DBG("transform_modal float: LMB miss -> PASS_THROUGH");
      return OPERATOR_PASS_THROUGH;
    }

    /* Enter — commit the transform. */
    if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS) {
      IMG_SEL_DBG("transform_modal float: ENTER -> commit");
      ED_area_status_text(CTX_wm_area(C), nullptr);
      g_transform_state = nullptr;
      image_select_transform_commit(C, state);
      image_select_transform_state_free(state);
      op->customdata = nullptr;
      WM_event_add_notifier(C, NC_WINDOW, nullptr);
      return OPERATOR_FINISHED;
    }

    /* Esc — cancel the transform. */
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      IMG_SEL_DBG("transform_modal float: ESC -> cancel");
      ED_area_status_text(CTX_wm_area(C), nullptr);
      g_transform_state = nullptr;
      image_select_transform_restore_source(C, state);
      image_select_transform_state_free(state);
      op->customdata = nullptr;
      WM_event_add_notifier(C, NC_WINDOW, nullptr);
      return OPERATOR_FINISHED;
    }

    /* All other events (pan/zoom wheels, MOUSEMOVE, etc.).
     * Must be plain PASS_THROUGH — combining with RUNNING_MODAL sets WM_HANDLER_BREAK
     * which prevents area/region keymap handlers (zoom, pan) from receiving the event. */
    return OPERATOR_PASS_THROUGH;
  }

  if (!region) {
    /* Lost region temporarily during drag; suspend drag, stay floating. */
    IMG_SEL_DBG("transform_modal: no region, suspend drag -> floating");
    ED_area_status_text(CTX_wm_area(C), nullptr);
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
        image_select_debug_log_handle_hit("transform_modal LMB press", state, mouse_co, hit);
        if (hit != ImageSelectTransformState::HANDLE_NONE) {
          image_select_transform_begin_handle_drag(state, event, hit);
          ED_region_tag_redraw(region);
        }
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->val == KM_RELEASE) {
        ED_area_status_text(CTX_wm_area(C), nullptr);
        state->active_handle = ImageSelectTransformState::HANDLE_NONE;
        state->is_snapped = false;
        state->is_dragging = false;
        op->customdata = nullptr;
        ED_region_tag_redraw(region);
        IMG_SEL_DBG("transform_modal: LMB release -> floating (RUNNING_MODAL)");
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    case MOUSEMOVE: {
      float2 mouse_co(event->mval[0], event->mval[1]);
      state->mouse_curr_pos = mouse_co;

      if (state->active_handle != ImageSelectTransformState::HANDLE_NONE) {
        /* Handle active drag transformations */
        float2 mouse_uv;
        ui::view2d_region_to_view(&region->v2d, mouse_co.x, mouse_co.y, &mouse_uv.x, &mouse_uv.y);

        float2 mouse_start_uv;
        ui::view2d_region_to_view(&region->v2d, state->mouse_start_pos.x, state->mouse_start_pos.y, &mouse_start_uv.x, &mouse_start_uv.y);

        /* Получаем размер canvas для правильного перевода UV → tile-pixel */
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

        /* UV → tile-pixel: умножаем на размер CANVAS, а не фрагмента */
        float2 mouse_px = mouse_uv * float2(canvas_w_modal, canvas_h_modal);
        float2 start_px = mouse_start_uv * float2(canvas_w_modal, canvas_h_modal);

        /* Вспомогательная лямбда forward transform для snap points */
        const float2 pivot_modal = state->anchor;
        const float cos_r_modal = cosf(state->rotation);
        const float sin_r_modal = sinf(state->rotation);
        auto apply_forward_modal = [&](float2 local) -> float2 {
          float2 global = float2(state->origin_px) + local;
          float2 centered = global - pivot_modal;
          float2 scaled = centered * state->scale;
          float2 rotated = float2(scaled.x * cos_r_modal - scaled.y * sin_r_modal,
                                  scaled.x * sin_r_modal + scaled.y * cos_r_modal);
          return rotated + pivot_modal + state->translation;
        };

        if (state->active_handle == ImageSelectTransformState::HANDLE_MOVE) {
          float2 delta = mouse_px - start_px;
          
          bool anchor_inside = (state->drag_start_anchor.x >= state->origin_px.x - 0.1f &&
                                state->drag_start_anchor.x <= state->origin_px.x + state->size_px.x + 0.1f &&
                                state->drag_start_anchor.y >= state->origin_px.y - 0.1f &&
                                state->drag_start_anchor.y <= state->origin_px.y + state->size_px.y + 0.1f);
          
          if (anchor_inside) {
            state->translation = state->drag_start_translation + delta;
            state->anchor = state->drag_start_anchor;
          }
          else {
            float inv_cos = cosf(-state->rotation);
            float inv_sin = sinf(-state->rotation);
            float2 inv_rotated = float2(
                delta.x * inv_cos - delta.y * inv_sin,
                delta.x * inv_sin + delta.y * inv_cos);
            float2 delta_pivot = float2(
                state->scale.x != 0.0f ? inv_rotated.x / state->scale.x : 0.0f,
                state->scale.y != 0.0f ? inv_rotated.y / state->scale.y : 0.0f);
                
            state->anchor = state->drag_start_anchor - delta_pivot;
            state->translation = state->drag_start_translation + delta_pivot;
          }
          
          IMG_SEL_DBG("transform_modal MOVE: delta=(%.1f,%.1f) translation=(%.1f,%.1f) "
                      "anchor=(%.1f,%.1f) [untransformed space]",
                      delta.x, delta.y,
                      state->translation.x, state->translation.y,
                      state->anchor.x, state->anchor.y);
        }
        else if (state->active_handle == ImageSelectTransformState::HANDLE_ROTATE) {
          float2 center_scr = state->screen_anchor;
          float2 start_vec = state->mouse_start_pos - center_scr;
          float2 curr_vec = mouse_co - center_scr;
          float angle_start = atan2f(start_vec.y, start_vec.x);
          float angle_curr = atan2f(curr_vec.y, curr_vec.x);
          float delta_angle = angle_curr - angle_start;
          float raw_angle = state->drag_start_rotation + delta_angle;
          if (event->modifier & KM_CTRL) {
            const float snap_interval = 15.0f * (float(M_PI) / 180.0f);
            state->rotation = roundf(raw_angle / snap_interval) * snap_interval;
          }
          else {
            state->rotation = raw_angle;
          }
          char status_str[128];
          snprintf(status_str, sizeof(status_str), "Rotation: %.2f°", state->rotation * 180.0f / float(M_PI));
          ED_area_status_text(CTX_wm_area(C), status_str);
          IMG_SEL_DBG("transform_modal ROTATE: anchor_scr=(%.1f,%.1f) center_scr=(%.1f,%.1f) anchor_px=(%.1f,%.1f) "
                      "start_vec=(%.1f,%.1f) delta_deg=%.2f rot_deg=%.2f",
                      center_scr.x, center_scr.y,
                      state->screen_center.x, state->screen_center.y,
                      state->anchor.x, state->anchor.y,
                      start_vec.x, start_vec.y,
                      float(delta_angle * 180.0 / M_PI),
                      float(state->rotation * 180.0 / M_PI));
        }
        else if (state->active_handle == ImageSelectTransformState::HANDLE_ANCHOR) {
          float2 target_anchor_px = mouse_px;
          
          /* Snap points в tile-pixel пространстве через явный forward transform */
          float2 snap_points_scr[9] = {
              state->screen_corners[0], state->screen_corners[1],
              state->screen_corners[2], state->screen_corners[3],
              state->screen_midpoints[0], state->screen_midpoints[1],
              state->screen_midpoints[2], state->screen_midpoints[3],
              state->screen_center
          };

          float2 snap_points_px[9] = {
              apply_forward_modal(float2(0, 0)),
              apply_forward_modal(float2(state->size_px.x, 0)),
              apply_forward_modal(float2(state->size_px.x, state->size_px.y)),
              apply_forward_modal(float2(0, state->size_px.y)),
              apply_forward_modal(float2(state->size_px.x * 0.5f, 0.0f)),
              apply_forward_modal(float2(state->size_px.x, state->size_px.y * 0.5f)),
              apply_forward_modal(float2(state->size_px.x * 0.5f, state->size_px.y)),
              apply_forward_modal(float2(0.0f, state->size_px.y * 0.5f)),
              apply_forward_modal(float2(state->size_px.x * 0.5f, state->size_px.y * 0.5f))
          };

          state->is_snapped = false;
          const float snap_threshold_scr = 12.0f;
          float min_dist = snap_threshold_scr;
          int snap_idx = -1;

          for (int i = 0; i < 9; i++) {
            float dist = math::distance(snap_points_scr[i], mouse_co);
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

          /* Вычисляем untransformed позицию нового якоря (обратное преобразование) */
          float2 offset = target_anchor_px - state->anchor - state->translation;
          float inv_cos = cosf(-state->rotation);
          float inv_sin = sinf(-state->rotation);
          float2 inv_rotated = float2(
              offset.x * inv_cos - offset.y * inv_sin,
              offset.x * inv_sin + offset.y * inv_cos);
          float2 inv_scaled = float2(
              state->scale.x != 0.0f ? inv_rotated.x / state->scale.x : 0.0f,
              state->scale.y != 0.0f ? inv_rotated.y / state->scale.y : 0.0f);
          
          float2 target_anchor_untransformed = state->anchor + inv_scaled;

          /* Anti-shift correction: перемещаем pivot без визуального сдвига фрагмента.
           * T_new = T + (R·S - I) · (P_new - P_old) */
          float2 delta_anchor = target_anchor_untransformed - state->anchor;
          /* Применяем R·S к delta (без translation) */
          float2 scaled_delta = delta_anchor * state->scale;
          float2 rotated_scaled_delta = float2(
              scaled_delta.x * cos_r_modal - scaled_delta.y * sin_r_modal,
              scaled_delta.x * sin_r_modal + scaled_delta.y * cos_r_modal);
          state->translation += (rotated_scaled_delta - delta_anchor);
          state->anchor = target_anchor_untransformed;
        }
        else if (state->active_handle >= ImageSelectTransformState::HANDLE_C0 &&
                 state->active_handle <= ImageSelectTransformState::HANDLE_M_LEFT) {
          /* Edge or corner scaling */
          float2 local_offset(0.0f, 0.0f);
          switch (state->active_handle) {
            case ImageSelectTransformState::HANDLE_C0: local_offset = float2(0.0f, 0.0f); break;
            case ImageSelectTransformState::HANDLE_C1: local_offset = float2(state->size_px.x, 0.0f); break;
            case ImageSelectTransformState::HANDLE_C2: local_offset = float2(state->size_px.x, state->size_px.y); break;
            case ImageSelectTransformState::HANDLE_C3: local_offset = float2(0.0f, state->size_px.y); break;
            case ImageSelectTransformState::HANDLE_M_BOTTOM: local_offset = float2(state->size_px.x * 0.5f, 0.0f); break;
            case ImageSelectTransformState::HANDLE_M_RIGHT: local_offset = float2(state->size_px.x, state->size_px.y * 0.5f); break;
            case ImageSelectTransformState::HANDLE_M_TOP: local_offset = float2(state->size_px.x * 0.5f, state->size_px.y); break;
            case ImageSelectTransformState::HANDLE_M_LEFT: local_offset = float2(0.0f, state->size_px.y * 0.5f); break;
            default: break;
          }

          /* Вычисляем начальное положение handle через явный forward transform */
          const float2 pivot_start = state->drag_start_anchor;
          const float cos_r_start = cosf(state->drag_start_rotation);
          const float sin_r_start = sinf(state->drag_start_rotation);
          auto apply_forward_start = [&](float2 local) -> float2 {
            float2 global = float2(state->origin_px) + local;
            float2 centered = global - pivot_start;
            float2 scaled = centered * state->drag_start_scale;
            float2 rotated = float2(scaled.x * cos_r_start - scaled.y * sin_r_start,
                                    scaled.x * sin_r_start + scaled.y * cos_r_start);
            return rotated + pivot_start + state->drag_start_translation;
          };
          float2 H_start = apply_forward_start(local_offset);

          float2 V_curr = mouse_px - state->drag_start_anchor;
          float2 V_start = H_start - state->drag_start_anchor;

          float rad = state->drag_start_rotation;
          float2 Ux(cosf(rad), sinf(rad));
          float2 Uy(-sinf(rad), cosf(rad));

          float proj_start_x = math::dot(V_start, Ux);
          float proj_start_y = math::dot(V_start, Uy);
          float proj_curr_x = math::dot(V_curr, Ux);
          float proj_curr_y = math::dot(V_curr, Uy);

          float sx = state->drag_start_scale.x;
          float sy = state->drag_start_scale.y;

          if (fabsf(proj_start_x) > 0.001f) {
            sx *= (proj_curr_x / proj_start_x);
          }
          if (fabsf(proj_start_y) > 0.001f) {
            sy *= (proj_curr_y / proj_start_y);
          }

          /* Constrain scale flags for edge drags */
          if (state->active_handle == ImageSelectTransformState::HANDLE_M_LEFT ||
              state->active_handle == ImageSelectTransformState::HANDLE_M_RIGHT) {
            sy = state->drag_start_scale.y;
          }
          if (state->active_handle == ImageSelectTransformState::HANDLE_M_TOP ||
              state->active_handle == ImageSelectTransformState::HANDLE_M_BOTTOM) {
            sx = state->drag_start_scale.x;
          }

          /* Check proportional constraint (with shift toggle) */
          bool proportional = state->use_proportional_scale;
          if (event->modifier & KM_SHIFT) {
            proportional = !proportional;
          }
          if (proportional && (state->active_handle >= ImageSelectTransformState::HANDLE_C0 &&
                               state->active_handle <= ImageSelectTransformState::HANDLE_C3)) {
            float aspect_ratio = state->drag_start_scale.x / state->drag_start_scale.y;
            float scale_ratio_x = sx / state->drag_start_scale.x;
            float scale_ratio_y = sy / state->drag_start_scale.y;
            float max_ratio = (fabsf(scale_ratio_x - 1.0f) > fabsf(scale_ratio_y - 1.0f)) ? scale_ratio_x : scale_ratio_y;
            sx = state->drag_start_scale.x * max_ratio;
            sy = sx / aspect_ratio;
          }

          /* Clamp scales to avoid division by zero or negative flip bugs if undesirable */
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
      return OPERATOR_RUNNING_MODAL;
    }

    default:
      /* During drag, unhandled events (wheel, middle-mouse pan, etc.) must not carry
       * RUNNING_MODAL — that would set WM_HANDLER_BREAK and prevent area/region keymap
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
