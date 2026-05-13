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

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"

namespace blender {

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
 * Does not require an active brush — selection tools are independent of the brush.
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
 * Stored in g_floating_state — survives the initial drag modal so the user
 * can navigate freely between drag gestures while the fragment is floating.
 */
struct ImageSelectMoveState {
  /* Space and region type that own this state — used for draw callback lifetime. */
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

  ImBuf *disp = IMB_allocImBuf(src->x, src->y, 0);
  IMB_alloc_float_pixels(disp, 4);
  disp->channels = 4;

  const int pixel_count = src->x * src->y;
  float *dst = disp->float_buffer.data;
  const float *fmask = mask->float_buffer.data;

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
   * so calling ED_draw_imbuf (which applies CM inline) would cause double conversion — the
   * root cause of the yellow tint seen in the preview. */
  GPU_blend(GPU_BLEND_ALPHA);

  {
    IMMDrawPixelsTexState tex_state = immDrawPixelsTexSetup(GPU_SHADER_3D_IMAGE_COLOR);
    /* Use the display buffer (mask baked into alpha) when available so the GPU
     * alpha-blends only the lasso-selected pixels, not the full bbox rectangle. */
    const ImBuf *frag = state->fragment_display_ibuf ? state->fragment_display_ibuf :
                                                       state->fragment_ibuf;
    if (frag->float_buffer.data) {
      const int ch = frag->channels ? frag->channels : 4;
      const gpu::TextureFormat fmt = (ch >= 4) ? gpu::TextureFormat::SFLOAT_16_16_16_16 :
                                     (ch == 3) ? gpu::TextureFormat::SFLOAT_16_16_16 :
                                                 gpu::TextureFormat::SFLOAT_16;
      immDrawPixelsTexTiled_scaling(
          &tex_state, sx0, sy0, frag->x, frag->y, fmt, true,
          frag->float_buffer.data, 1.0f, 1.0f, zoom_x, zoom_y, nullptr);
    }
    else if (frag->byte_buffer.data) {
      immDrawPixelsTexTiled_scaling(
          &tex_state, sx0, sy0, frag->x, frag->y, gpu::TextureFormat::UNORM_8_8_8_8, true,
          frag->byte_buffer.data, 1.0f, 1.0f, zoom_x, zoom_y, nullptr);
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
 * Does not modify the source ibuf — pixels are cleared only at commit time.
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
  ImBuf *fragment = IMB_allocImBuf(safe_w, safe_h, is_float ? IB_float_data : IB_byte_data);
  if (!fragment) {
    BKE_image_release_ibuf(ima, ibuf, lock);
    return nullptr;
  }
  if (is_float) {
    /* Ensure channel count matches the source so IMB_copy_rect uses the correct stride. */
    fragment->channels = ibuf->channels ? ibuf->channels : 4;
  }

  /* Copy source rectangle → fragment origin (0, 0). */
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
    ImBuf *fmask = IMB_allocImBuf(safe_w, safe_h, 0);
    IMB_alloc_float_pixels(fmask, 1);
    const float *src_mask = (*mask_ptr)->float_buffer.data;
    float *dst_mask = fmask->float_buffer.data;
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
 * visual feedback that the fragment has been lifted. Pushes the undo begin marker — the
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

  /* Clear source pixels — respect the actual selection mask shape (e.g. lasso). */
  const float *fmask = state->fragment_mask_ibuf ? state->fragment_mask_ibuf->float_buffer.data :
                                                    nullptr;
  if (ibuf->float_buffer.data) {
    const int channels = ibuf->channels ? ibuf->channels : 4;
    float *data = ibuf->float_buffer.data;
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
  else if (ibuf->byte_buffer.data) {
    uint8_t *data = ibuf->byte_buffer.data;
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
    float *mdata = mask->float_buffer.data;
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
 * Closes the undo step opened in lift_source — the step is a no-op since the restored state
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

    /* Paste fragment back at source origin — only selected pixels (mask-aware). */
    if (fmask) {
      const bool is_float = ibuf->float_buffer.data != nullptr;
      if (is_float) {
        const int channels = ibuf->channels ? ibuf->channels : 4;
        float *dst = ibuf->float_buffer.data;
        const float *src = state->fragment_ibuf->float_buffer.data;
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
      else if (ibuf->byte_buffer.data) {
        uint8_t *dst = ibuf->byte_buffer.data;
        const uint8_t *src = state->fragment_ibuf->byte_buffer.data;
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
    float *mdata = mask->float_buffer.data;
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
 * Closes the undo step opened there — undoing this step restores both source and destination.
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

  /* Raw destination — no clamping. Per-pixel bounds checks below handle clipping. */
  const int2 dst = {
      state->origin_px.x + state->drag_offset.x,
      state->origin_px.y + state->drag_offset.y,
  };

  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

  /* Blit only the selected pixels (mask-aware) to the destination. */
  const float *fmask = state->fragment_mask_ibuf ? state->fragment_mask_ibuf->float_buffer.data :
                                                    nullptr;
  if (fmask) {
    const bool is_float = ibuf->float_buffer.data != nullptr;
    if (is_float) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *dst_data = ibuf->float_buffer.data;
      const float *src_data = state->fragment_ibuf->float_buffer.data;
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
    else if (ibuf->byte_buffer.data) {
      uint8_t *dst_data = ibuf->byte_buffer.data;
      const uint8_t *src_data = state->fragment_ibuf->byte_buffer.data;
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
    float *mdata = mask->float_buffer.data;
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
  float *data = mask->float_buffer.data;
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
    float *data = mask->float_buffer.data;
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
      float *data = mask->float_buffer.data;

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
      float *row = ibuf->float_buffer.data + y * width;
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
        ibuf->float_buffer.data[y * width + x] = color;
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
  /* Re-drag an already-floating fragment — no selection needed. */
  if (g_floating_state) {
    SpaceImage *sima = CTX_wm_space_image(C);
    if (sima && sima == g_floating_state->owner_sima) {
      return true;
    }
  }
  if (!image_paint_selection_poll(C)) {
    return false;
  }
  SpaceImage *sima = CTX_wm_space_image(C);
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
        /* Drag finished — fragment stays floating, modal ends.
         * Navigation (pan/zoom) is fully available between drag gestures. */
        state->is_dragging = false;
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;

    case RIGHTMOUSE:
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
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
/** \name Invoke — extract fragment, start initial drag
 * \{ */

static wmOperatorStatus image_select_move_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);

  /* Re-drag path: a fragment is already floating in this space — start another drag gesture.
   * Push the current position before the new drag so Ctrl+Z can step back to it. */
  if (g_floating_state && g_floating_state->owner_sima == sima) {
    g_floating_state->drag_position_history.append(g_floating_state->drag_offset);
    g_floating_state->is_dragging = true;
    g_floating_state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};
    op->customdata = g_floating_state;
    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  /* If a floating fragment from a different space is still alive, commit it. */
  if (g_floating_state) {
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

  /* History exhausted — restore source pixels and end the floating operation. */
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
  ImBuf *frag_ibuf = IMB_allocImBuf(frag_width, frag_height, is_float ? IB_float_data : IB_byte_data);
  if (!frag_ibuf) {
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot allocate fragment buffer");
    return OPERATOR_CANCELLED;
  }

  if (is_float) {
    frag_ibuf->channels = canvas_ibuf->channels ? canvas_ibuf->channels : 4;
  }

  /* Copy source rectangle → fragment origin (0, 0). */
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
  ImBuf *frag_mask_ibuf = IMB_allocImBuf(frag_width, frag_height, 0);
  IMB_alloc_float_pixels(frag_mask_ibuf, 1);
  float *dst_mask = frag_mask_ibuf->float_buffer.data;
  const float *src_mask = mask_ibuf->float_buffer.data;

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
   * (step_init → nullptr). The subsequent ED_image_undo_push_end in commit would then call
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

/** \} */

}  /* namespace blender */
