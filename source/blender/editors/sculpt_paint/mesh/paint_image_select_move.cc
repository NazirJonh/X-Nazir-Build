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
#include "../../space_image/image_runtime.hh"
#include "paint_image_select_intern.hh"


namespace blender {

/* -------------------------------------------------------------------- */
/** \name Move Selection Internal Helpers
 * \{ */

/**
 * Runtime state for the image select move operation.
 * Lifetime: allocated in invoke, freed in confirm or cancel.
 * Owned by SpaceImage_Runtime::paint_select.move; survives the initial drag
 * modal so the user can navigate freely between drag gestures while floating.
 */
struct ImageSelectMoveState {
  /* Space and region type that own this state -- used for draw callback lifetime. */
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

/* Global clipboard buffer. Cleared on app exit via WM_exit_handler. */
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

static const ImageClipboardState *clipboard_get()
{
  return g_clipboard_state;
}

/** \} */

/**
 * Build a 4-channel RGBA float ImBuf suitable for GPU preview drawing.
 * RGB channels come from `src`, alpha channel from the 1-channel `mask` (0..1).
 * This lets the GPU alpha-blend only the pixels inside the lasso shape, so the
 * preview respects the actual selection outline rather than showing the full bbox.
 */
ImBuf *image_select_make_display_ibuf(const ImBuf *src, const ImBuf *mask)
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

  /* UDIM tile UV origin: tile 1001 -> (0,0), tile 1012 -> (1,1), etc.
   * view2d in the Image Editor uses this global UV convention, so we must add
   * the tile origin before converting to screen space. */
  const float tile_uv_x = float((state->tile_number - 1001) % 10);
  const float tile_uv_y = float((state->tile_number - 1001) / 10);

  /* Convert destination bbox to global UV (tile origin + tile-local [0..1]). */
  const float v_x0 = tile_uv_x + float(dst_x) / float(ibuf->x);
  const float v_y0 = tile_uv_y + float(dst_y) / float(ibuf->y);
  const float v_x1 = tile_uv_x + float(dst_x + state->size_px.x) / float(ibuf->x);
  const float v_y1 = tile_uv_y + float(dst_y + state->size_px.y) / float(ibuf->y);

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
   * so calling ED_draw_imbuf (which applies CM inline) would cause double conversion -- the
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
 * Does not modify the source ibuf -- pixels are cleared only at commit time.
 * Returns nullptr on failure (no selection, no pixel data, zero-size bbox).
 * Supports both float and byte image buffers.
 *
 * On success writes tile-pixel bbox into r_origin_px and r_size_px.
 */
int image_paint_selection_resolve_tile(Image *ima, const SpaceImage *sima, int preferred_tile)
{
  if (ima && BKE_image_paint_selection_mask_lookup(ima, preferred_tile)) {
    int r_min[2], r_max[2];
    if (BKE_image_paint_selection_mask_bounds(ima, preferred_tile, r_min, r_max)) {
      return preferred_tile;
    }
  }
  if (ima) {
    const int found_tile = BKE_image_paint_selection_mask_first_tile_with_selection(ima);
    if (found_tile != 0) {
      return found_tile;
    }
  }
  if (sima && ima && ima->source == IMA_SRC_TILED) {
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&ima->tiles, ima->active_tile_index));
    if (active) {
      return active->tile_number;
    }
    const ImageTile *first = static_cast<const ImageTile *>(ima->tiles.first);
    if (first) {
      return first->tile_number;
    }
  }
  return preferred_tile;
}

ImBuf *image_select_move_extract(wmOperator *op,
                                        Image *ima,
                                        ImageUser *iuser,
                                        const SpaceImage *sima,
                                        int tile_number,
                                        int2 *r_origin_px,
                                        int2 *r_size_px,
                                        ImBuf **r_fragment_mask)
{
  tile_number = image_paint_selection_resolve_tile(ima, sima, tile_number);
  if (iuser) {
    iuser->tile = tile_number;
  }

  if (!BKE_image_paint_selection_mask_lookup(ima, tile_number)) {
    BKE_report(op->reports, RPT_ERROR, "No selection on active tile");
    return nullptr;
  }

  int r_min[2], r_max[2];
  if (!BKE_image_paint_selection_mask_bounds(ima, tile_number, r_min, r_max)) {
    BKE_report(op->reports, RPT_ERROR, "Selection is empty");
    return nullptr;
  }

  const int x_min = r_min[0];
  const int y_min = r_min[1];
  const int x_max = r_max[0];
  const int y_max = r_max[1];
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

  /* Copy source rectangle to fragment origin (0, 0). */
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
    ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(ima, tile_number);
    ImBuf *fmask = IMB_allocImBuf(safe_w, safe_h, ImBufFlags::Zero);
    IMB_alloc_float_pixels(fmask, 1);
    const float *src_mask = tile_mask ? tile_mask->float_data() : nullptr;
    float *dst_mask = fmask->float_data_for_write();
    const int full_w = tile_mask ? tile_mask->x : 0;
    if (!src_mask) {
      memset(dst_mask, 0, sizeof(float) * safe_w * safe_h);
    }
    else {
      for (int y = 0; y < safe_h; y++) {
        for (int x = 0; x < safe_w; x++) {
          dst_mask[y * safe_w + x] = src_mask[(safe_y + y) * full_w + (safe_x + x)];
        }
      }
    }
    *r_fragment_mask = fmask;
  }

  return fragment;
}

/**
 * Immediately clear the source region from the canvas and selection mask, giving the user
 * visual feedback that the fragment has been lifted. Pushes the undo begin marker -- the
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

/**
 * Restore the source pixels and mask from the extracted fragment (used on cancel).
 * Closes the undo step opened in lift_source -- the step is a no-op since the restored state
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
     * then immediately discard the no-op step so Ctrl+Z is not wasted on it.
     * Guard against calling push_end when step_init was stolen by another operator
     * (tool switch / intervening undo-bearing operator) -- see image_select_move_commit
     * for a full explanation of the crash path. */
    UndoStack *undo_stack = ED_undo_stack_get();
    if (undo_stack && undo_stack->step_init != nullptr) {
      ED_image_undo_push_end();
      BKE_undosys_stack_clear_active(undo_stack);
    }
  }

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

/**
 * Commit the move: blit the floating fragment to the destination and update the selection mask.
 * Source pixels were already cleared by lift_source on invoke.
 * Closes the undo step opened there -- undoing this step restores both source and destination.
 *
 * Position and scale are preserved in UV space so that UDIM tiles with different pixel
 * resolutions display the fragment exactly as seen in the Image Editor: the same UV region
 * is covered, resampled to the target tile's pixel density when necessary.
 * Float images use bilinear interpolation; byte images use nearest-neighbor.
 */
void image_select_move_commit(bContext *C, ImageSelectMoveState *state)
{
  /* Retrieve the undo stack once; all push_end guard checks in this function use this
   * pointer to verify that step_init is still open before calling ED_image_undo_push_end.
   * See the comment above the final guard at the end of this function for a full explanation
   * of why step_init can be nullptr at commit time and why the guard is necessary. */
  UndoStack *ustack = ED_undo_stack_get();

  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    if (state->undo_begun && ustack && ustack->step_init != nullptr) {
      ED_image_undo_push_end();
    }
    return;
  }
  Image *ima = sima->image;

  /* Acquire the source tile ibuf to read its pixel dimensions.
   * drag_offset was accumulated using these dimensions, so they are needed to convert
   * the offset back to UV units. */
  ImageUser iuser_src = state->iuser;
  void *lock_src;
  ImBuf *ibuf_src = BKE_image_acquire_ibuf(ima, &iuser_src, &lock_src);
  if (!ibuf_src || (!ibuf_src->float_buffer.data && !ibuf_src->byte_buffer.data)) {
    if (ibuf_src) {
      BKE_image_release_ibuf(ima, ibuf_src, lock_src);
    }
    if (state->undo_begun && ustack && ustack->step_init != nullptr) {
      ED_image_undo_push_end();
    }
    return;
  }
  const float src_w = float(ibuf_src->x);
  const float src_h = float(ibuf_src->y);
  BKE_image_release_ibuf(ima, ibuf_src, lock_src);

  /* Source tile grid position.
   * Tile 1001 -> (col=0, row=0), tile 1011 -> (col=0, row=1), tile 1012 -> (col=1, row=1), etc. */
  const int src_col = (state->tile_number - 1001) % 10;
  const int src_row = (state->tile_number - 1001) / 10;

  /* Fragment bounds in UV space.
   * drag_offset is in source-tile pixels; dividing by source dimensions converts to UV units.
   * UV (0,0)-(1,1) is tile 1001; UV (0,1)-(1,2) is tile 1011, etc. */
  const float frag_uv_x0 = float(src_col) +
                            float(state->origin_px.x + state->drag_offset.x) / src_w;
  const float frag_uv_y0 = float(src_row) +
                            float(state->origin_px.y + state->drag_offset.y) / src_h;
  const float frag_uv_x1 = frag_uv_x0 + float(state->size_px.x) / src_w;
  const float frag_uv_y1 = frag_uv_y0 + float(state->size_px.y) / src_h;

  const float *fmask = state->fragment_mask_ibuf ?
                           state->fragment_mask_ibuf->float_buffer.data :
                           nullptr;

  /* Obtain the currently-open image undo step (lives in step_init until push_end).
   * Destination tiles that differ from the source/paste tile were not registered when the
   * undo step was opened, so their pre-state must be snapshotted here -- before any pixel
   * data is overwritten -- to make Ctrl+Z restore them correctly.
   * This mirrors the identical pattern applied to image_select_transform_commit.
   * `ustack` was already retrieved at the top of this function. */
  ImageUndoStep *us_open = (ustack && ustack->step_init &&
                            ustack->step_init->type == BKE_UNDOSYS_TYPE_IMAGE) ?
                               reinterpret_cast<ImageUndoStep *>(ustack->step_init) :
                               nullptr;

  /* Iterate all tiles and write pixels to those the fragment overlaps. */
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    const int t_col = (tile->tile_number - 1001) % 10;
    const int t_row = (tile->tile_number - 1001) / 10;
    const float t_uv_x0 = float(t_col);
    const float t_uv_y0 = float(t_row);

    /* Fragment UV bounds clipped to this tile's UV space. */
    const float iu_x0 = std::max(frag_uv_x0, t_uv_x0);
    const float iu_y0 = std::max(frag_uv_y0, t_uv_y0);
    const float iu_x1 = std::min(frag_uv_x1, t_uv_x0 + 1.0f);
    const float iu_y1 = std::min(frag_uv_y1, t_uv_y0 + 1.0f);
    if (iu_x0 >= iu_x1 || iu_y0 >= iu_y1) {
      continue;
    }

    ImageUser tile_iuser = state->iuser;
    tile_iuser.tile = tile->tile_number;
    void *lock;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
    if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
      if (ibuf) {
        BKE_image_release_ibuf(ima, ibuf, lock);
      }
      continue;
    }

    /* Register the pre-state of every destination tile that differs from the source/paste
     * tile.  That tile was already snapshotted at lift/paste time, so re-snapshotting it
     * here would overwrite the original (pre-lift) pixels with the cleared-hole state and
     * break undo for the primary tile. */
    if (us_open && tile->tile_number != state->tile_number) {
      ED_image_undo_push(ima, ibuf, &tile_iuser, us_open);
      ED_image_undo_capture_selection_mask(ima, tile->tile_number);
    }

    /* Destination tile's actual pixel dimensions (may differ from source tile). */
    const float dst_w = float(ibuf->x);
    const float dst_h = float(ibuf->y);

    /* Convert the UV intersection to destination-tile pixel bounds.
     * floor/ceil ensures every pixel that partially overlaps the fragment is included. */
    const int dp_x0 = std::max(0, int(std::floor((iu_x0 - t_uv_x0) * dst_w)));
    const int dp_y0 = std::max(0, int(std::floor((iu_y0 - t_uv_y0) * dst_h)));
    const int dp_x1 = std::min(int(dst_w), int(std::ceil((iu_x1 - t_uv_x0) * dst_w)));
    const int dp_y1 = std::min(int(dst_h), int(std::ceil((iu_y1 - t_uv_y0) * dst_h)));

    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

    const bool is_float = ibuf->float_data() != nullptr;

    if (is_float && state->fragment_ibuf->float_data()) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *dst_data = ibuf->float_data_for_write();
      const float *src_data = state->fragment_ibuf->float_data();

      for (int py = dp_y0; py < dp_y1; py++) {
        /* UV at the center of this destination pixel -> fragment-local float coordinate.
         * fy = 0.5 is the center of fragment pixel 0, fy = 1.5 is the center of pixel 1, etc.
         * For same-resolution tiles the value is always ly + 0.5 (no bilinear blending). */
        const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * src_h;
        if (fy < 0.0f || fy >= float(state->size_px.y)) {
          continue;
        }

        /* Nearest-neighbor y index for mask lookup. */
        const int mask_ly = int(fy);

        /* Bilinear y weights: shift to pixel-corner convention and clamp to fragment bounds. */
        const float bly = std::max(0.0f,
                                   std::min(float(state->size_px.y) - 1.0001f, fy - 0.5f));
        const int by0 = int(bly);
        const int by1 = std::min(state->size_px.y - 1, by0 + 1);
        const float wy1 = bly - float(by0);
        const float wy0 = 1.0f - wy1;

        for (int px = dp_x0; px < dp_x1; px++) {
          const float fx = (t_uv_x0 + (float(px) + 0.5f) / dst_w - frag_uv_x0) * src_w;
          if (fx < 0.0f || fx >= float(state->size_px.x)) {
            continue;
          }
          const int mask_lx = int(fx);

          /* Skip pixels that fall outside the lasso selection. */
          if (fmask && fmask[mask_ly * state->size_px.x + mask_lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }

          /* Bilinear x weights. */
          const float blx = std::max(0.0f,
                                     std::min(float(state->size_px.x) - 1.0001f, fx - 0.5f));
          const int bx0 = int(blx);
          const int bx1 = std::min(state->size_px.x - 1, bx0 + 1);
          const float wx1 = blx - float(bx0);
          const float wx0 = 1.0f - wx1;

          for (int c = 0; c < channels; c++) {
            dst_data[(py * int(dst_w) + px) * channels + c] =
                src_data[(by0 * state->size_px.x + bx0) * channels + c] * wx0 * wy0 +
                src_data[(by0 * state->size_px.x + bx1) * channels + c] * wx1 * wy0 +
                src_data[(by1 * state->size_px.x + bx0) * channels + c] * wx0 * wy1 +
                src_data[(by1 * state->size_px.x + bx1) * channels + c] * wx1 * wy1;
          }
        }
      }
    }
    else if (!is_float && ibuf->byte_data() && state->fragment_ibuf->byte_data()) {
      /* Byte images: nearest-neighbor sampling. */
      uint8_t *dst_data = ibuf->byte_data_for_write();
      const uint8_t *src_data = state->fragment_ibuf->byte_data();

      for (int py = dp_y0; py < dp_y1; py++) {
        const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * src_h;
        if (fy < 0.0f || fy >= float(state->size_px.y)) {
          continue;
        }
        const int ly = int(fy);

        for (int px = dp_x0; px < dp_x1; px++) {
          const float fx = (t_uv_x0 + (float(px) + 0.5f) / dst_w - frag_uv_x0) * src_w;
          if (fx < 0.0f || fx >= float(state->size_px.x)) {
            continue;
          }
          const int lx = int(fx);

          if (fmask && fmask[ly * state->size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }
          memcpy(dst_data + (py * int(dst_w) + px) * 4,
                 src_data + (ly * state->size_px.x + lx) * 4,
                 4);
        }
      }
    }

    BKE_image_mark_dirty(ima, ibuf);

    /* Update the destination tile's selection mask (nearest-neighbor -- mask is binary). */
    ImBuf *mask = BKE_image_paint_selection_mask_lookup(ima, tile->tile_number);
    if (mask) {
      float *mdata = mask->float_data_for_write();
      if (mdata) {
        for (int py = dp_y0; py < dp_y1; py++) {
          const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * src_h;
          if (fy < 0.0f || fy >= float(state->size_px.y)) {
            continue;
          }
          const int ly = int(fy);
          for (int px = dp_x0; px < dp_x1; px++) {
            if (py < 0 || py >= mask->y || px < 0 || px >= mask->x) {
              continue;
            }
            const float fx = (t_uv_x0 + (float(px) + 0.5f) / dst_w - frag_uv_x0) * src_w;
            if (fx < 0.0f || fx >= float(state->size_px.x)) {
              continue;
            }
            const int lx = int(fx);
            mdata[py * mask->x + px] = fmask ? fmask[ly * state->size_px.x + lx] : 1.0f;
          }
        }
      }
    }

    /* Mark the modified region dirty (tile-local pixel coordinates). */
    rcti dirty;
    BLI_rcti_init(&dirty, dp_x0, dp_x1, dp_y0, dp_y1);
    BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);

    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  /* Only close the undo step if it is still open.
   * step_init can be prematurely set to nullptr in two ways:
   *   1. A selection operator (box/lasso/circle) that was invoked while this
   *      floating fragment was live called push_begin_selection, which in turn
   *      calls BKE_undosys_step_push_init_with_type.  That function frees and
   *      unlinks any existing step_init before creating its own.
   *   2. Any operator with OPTYPE_UNDO that completed after lift_source called
   *      BKE_undosys_step_push, which finalises step_init and sets it to nullptr.
   * In either case, calling ED_image_undo_push_end() would reach the branch
   *   ut = BKE_undosys_type_from_context(nullptr)
   * because step_init is nullptr and push_end always passes nullptr for C.
   * BKE_undosys_type_from_context then iterates all undo-type poll functions
   * with a null context; armature_undosys_poll dereferences it and crashes.
   * Skipping push_end is safe: the pixels have already been written above, so
   * the commit is applied to the canvas.  The undo record is unfortunately lost
   * (the earlier step_init was destroyed by the intervening operator), but that
   * is preferable to crashing. */
  if (state->undo_begun && ustack && ustack->step_init != nullptr) {
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
void image_select_move_state_free(ImageSelectMoveState *state)
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
/** \name Image Select Move Operator
 * \{ */

/* Returns true when a selection fragment is currently floating in the active Image Editor. */
static bool image_select_move_floating_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return sima && sima->runtime && sima->runtime->paint_select.move != nullptr;
}

static bool image_select_move_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return false;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  if (image_select_transform_is_floating(C)) {
    return false;
  }
  /* Re-drag an already-floating fragment, no selection needed. */
  if (g_floating_state && sima == g_floating_state->owner_sima) {
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
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
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
        /* Drag finished -- fragment stays floating, modal ends.
         * Navigation (pan/zoom) is fully available between drag gestures. */
        state->is_dragging = false;
        op->customdata = nullptr;
        WM_cursor_modal_restore(CTX_wm_window(C));
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;

    case RIGHTMOUSE:
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        WM_cursor_modal_restore(CTX_wm_window(C));
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
  wmWindow *win = CTX_wm_window(C);
  if (win) {
    WM_cursor_modal_restore(win);
  }
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->runtime) {
    ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
    if (g_floating_state) {
      image_select_move_restore_source(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
  }
  op->customdata = nullptr;
  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invoke -- extract fragment, start initial drag
 * \{ */

/**
 * Return true when the cursor (from `event->mval`) is inside the floating fragment's
 * current bounding box in tile-pixel space.  Used to decide whether a new LMB press
 * should start a drag or be passed through to a selection/commit operator.
 */
bool image_select_move_cursor_in_fragment(const ImageSelectMoveState *state,
                                                 const ARegion *region,
                                                 const wmEvent *event,
                                                 Image *ima)
{
  ImageUser iuser = state->iuser;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
  if (!ibuf) {
    return true;
  }
  const float ibuf_w = float(ibuf->x);
  const float ibuf_h = float(ibuf->y);
  BKE_image_release_ibuf(ima, ibuf, lock);

  /* Convert region-relative cursor to view2d (global UV) space. */
  float uv_x, uv_y;
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv_x, &uv_y);

  /* Remove tile UV origin to get tile-local UV, then convert to pixel coords. */
  const float tile_uv_x = float((state->tile_number - 1001) % 10);
  const float tile_uv_y = float((state->tile_number - 1001) / 10);
  const float px_x = (uv_x - tile_uv_x) * ibuf_w;
  const float px_y = (uv_y - tile_uv_y) * ibuf_h;

  const int dst_x = state->origin_px.x + state->drag_offset.x;
  const int dst_y = state->origin_px.y + state->drag_offset.y;

  return (px_x >= float(dst_x) && px_x <= float(dst_x + state->size_px.x) &&
          px_y >= float(dst_y) && px_y <= float(dst_y + state->size_px.y));
}

bool image_select_move_delegate_to_move_operator(bContext *C, const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return false;
  }
  ImageSelectMoveState *state = sima->runtime->paint_select.move;
  if (!state || state->owner_sima != sima || !sima->image) {
    return false;
  }
  ARegion *region = CTX_wm_region(C);
  if (!region || !image_select_move_cursor_in_fragment(state, region, event, sima->image)) {
    return false;
  }
  WM_operator_name_call(
      C, "PAINT_OT_image_select_move", wm::OpCallContext::InvokeDefault, nullptr, event);
  return true;
}

static wmOperatorStatus image_select_move_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Re-drag path: a fragment is already floating in this space -- start another drag gesture,
   * but only when LMB was pressed inside the fragment's current bounding box.
   * If the press is outside, pass through so selection/commit operators handle it. */
  if (g_floating_state && g_floating_state->owner_sima == sima) {
    ARegion *region_for_hit = CTX_wm_region(C);
    if (region_for_hit &&
        !image_select_move_cursor_in_fragment(g_floating_state, region_for_hit, event, sima->image))
    {
      return OPERATOR_PASS_THROUGH;
    }
    /* Push the current position so Ctrl+Z can step back to it. */
    g_floating_state->drag_position_history.append(g_floating_state->drag_offset);
    g_floating_state->is_dragging = true;
    g_floating_state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};
    op->customdata = g_floating_state;
    WM_event_add_modal_handler(C, op);
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    return OPERATOR_RUNNING_MODAL;
  }

  /* If a floating fragment from a different space is still alive, commit it. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
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

  auto *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->tile_number = tile_number;
  state->iuser = iuser;
  state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};

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

  image_select_move_lift_source(C, state);

  ARegion *region = CTX_wm_region(C);
  state->owner_region_type = region->runtime->type;
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_move_preview, state, REGION_DRAW_POST_PIXEL);

  g_floating_state = state;

  /* Only start dragging immediately when the operator was invoked by an LMB click.
   * When invoked via keyboard shortcut (e.g. a key bound to Move Selection), the cursor
   * may be far from the fragment, so auto-starting the drag would jump the fragment to
   * an unexpected position.  In that case, enter floating mode without dragging -- the
   * first LMB press inside the fragment (delegated from a selection-tool invoke via
   * image_select_move_delegate_to_move_operator) will trigger the re-drag path above. */
  if (event->type == LEFTMOUSE) {
    /* The initial LMB invoke is the first drag gesture: snapshot offset so Ctrl+Z works. */
    state->drag_position_history.append(state->drag_offset);
    state->is_dragging = true;
    op->customdata = state;
    WM_event_add_modal_handler(C, op);
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    ED_region_tag_redraw(region);
    return OPERATOR_RUNNING_MODAL;
  }

  /* Keyboard invoke: fragment is floating, waiting for LMB press inside it to start drag. */
  ED_region_tag_redraw(region);
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Confirm / Cancel operators (non-modal, keymap-driven)
 * \{ */

static wmOperatorStatus image_select_move_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
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
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
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
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
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

  /* History exhausted -- restore source pixels and end the floating operation. */
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
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  return scene->toolsettings->imapaint.use_selection_mask != 0;
}

/**
 * Extract the selected region's pixels and mask, store in local clipboard,
 * and copy pixels to system clipboard.
 */
static wmOperatorStatus image_select_copy_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    BKE_report(op->reports, RPT_ERROR, "No image in context");
    return OPERATOR_CANCELLED;
  }

  Image *ima = sima->image;
  ImBuf *canvas_ibuf = nullptr;
  void *canvas_lock = nullptr;

  /* Resolve the source tile. For UDIM images, scan all tiles that have a selection mask and
   * use the first one with a non-empty selection. This allows copy to work without requiring
   * the user to manually activate the tile first. Falls back to active_tile_index when
   * no selection exists anywhere. */
  ImageUser iuser = sima->iuser;
  if (ima->source == IMA_SRC_TILED) {
    int source_tile = BKE_image_paint_selection_mask_first_tile_with_selection(ima);
    if (source_tile == 0) {
      const ImageTile *active = static_cast<const ImageTile *>(
          BLI_findlink(&ima->tiles, ima->active_tile_index));
      source_tile = active ? active->tile_number :
                             static_cast<const ImageTile *>(ima->tiles.first)->tile_number;
    }
    iuser.tile = source_tile;
  }

  /* Acquire the image buffer for the active tile. */
  canvas_ibuf = BKE_image_acquire_ibuf(ima, &iuser, &canvas_lock);
  if (!canvas_ibuf) {
    BKE_report(op->reports, RPT_ERROR, "Cannot acquire image buffer");
    return OPERATOR_CANCELLED;
  }

  /* Get the selection mask for this tile. */
  ImageTile *tile = BKE_image_get_tile_from_iuser(ima, &iuser);
  if (!tile) {
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot determine image tile");
    return OPERATOR_CANCELLED;
  }

  int tile_number = tile->tile_number;
  ImBuf *mask_ibuf = BKE_image_paint_selection_mask_get(
      ima, tile_number, canvas_ibuf->x, canvas_ibuf->y);
  if (!mask_ibuf) {
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_ERROR, "Cannot acquire selection mask");
    return OPERATOR_CANCELLED;
  }

  /* Find the bounding box of the selection. */
  int r_min[2], r_max[2];
  if (!BKE_image_paint_selection_mask_bounds(ima, tile_number, r_min, r_max)) {
    BKE_image_release_ibuf(ima, canvas_ibuf, canvas_lock);
    BKE_report(op->reports, RPT_WARNING, "Selection is empty");
    return OPERATOR_CANCELLED;
  }

  /* Extract fragment from canvas at selection bounds. */
  const int x_min = r_min[0];
  const int y_min = r_min[1];
  const int x_max = r_max[0];
  const int y_max = r_max[1];
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

  /* Copy source rectangle to fragment origin (0, 0). */
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

  /* Copy fragment to system clipboard. */
  if (!WM_clipboard_image_set_byte_buffer(frag_ibuf)) {
    IMB_freeImBuf(frag_ibuf);
    IMB_freeImBuf(frag_mask_ibuf);
    BKE_report(op->reports, RPT_WARNING, "Failed to set system clipboard");
    return OPERATOR_CANCELLED;
  }

  /* Store in local clipboard with mask. */
  image_clipboard_state_set(frag_ibuf, frag_mask_ibuf, int2{x_min, y_min},
                            tile_number, true);

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
    return false;
  }

  /* Have local clipboard with mask. */
  const ImageClipboardState *cb = clipboard_get();
  if (cb && cb->has_mask) {
    return true;
  }

  /* Have system clipboard. */
  ImBuf *test_ibuf = WM_clipboard_image_get();
  if (test_ibuf) {
    IMB_freeImBuf(test_ibuf);
    return true;
  }

  return false;
}

/**
 * Paste from local clipboard (with mask) or system clipboard.
 * Initializes a floating selection that can be moved and confirmed/cancelled.
 */
static wmOperatorStatus image_select_paste_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->image || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  Image *ima = sima->image;

  /* For UDIM images, resolve the paste target tile. When pasting from the local clipboard
   * (which stores the source tile number), paste back to that same tile so the result
   * always lands where the copy was made. For system clipboard fall back to active tile. */
  const ImageClipboardState *cb = clipboard_get();
  ImageUser iuser = sima->iuser;
  if (ima->source == IMA_SRC_TILED) {
    int target_tile = 0;
    if (cb && cb->has_mask) {
      target_tile = cb->tile_number;
    }
    if (target_tile == 0) {
      const ImageTile *active = static_cast<const ImageTile *>(
          BLI_findlink(&ima->tiles, ima->active_tile_index));
      target_tile = active ? active->tile_number :
                             static_cast<const ImageTile *>(ima->tiles.first)->tile_number;
    }
    iuser.tile = target_tile;
  }

  ImBuf *paste_ibuf = nullptr;
  ImBuf *paste_mask_ibuf = nullptr;
  int2 paste_origin = {0, 0};
  bool has_mask = false;

  /* Check local clipboard first (has mask). */
  if (cb && cb->has_mask) {
    /* Copy buffers from clipboard state to avoid lifetime issues. */
    paste_ibuf = IMB_dupImBuf(cb->fragment_ibuf);
    if (cb->fragment_mask_ibuf) {
      paste_mask_ibuf = IMB_dupImBuf(cb->fragment_mask_ibuf);
    }
    paste_origin = cb->origin_px;
    has_mask = true;
  }
  else {
    /* Fall back to system clipboard (no mask, center on canvas). */
    ImBuf *sys_ibuf = WM_clipboard_image_get();
    if (!sys_ibuf) {
      BKE_report(op->reports, RPT_ERROR, "No image in clipboard");
      return OPERATOR_CANCELLED;
    }
    /* Copy system clipboard buffer to prevent lifetime issues. */
    paste_ibuf = IMB_dupImBuf(sys_ibuf);
    IMB_freeImBuf(sys_ibuf);
    if (!paste_ibuf) {
      BKE_report(op->reports, RPT_ERROR, "Failed to allocate paste buffer");
      return OPERATOR_CANCELLED;
    }
    /* Acquire canvas to center the paste. */
    void *lock = nullptr;
    ImBuf *canvas_ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
    if (canvas_ibuf) {
      paste_origin.x = (canvas_ibuf->x - paste_ibuf->x) / 2;
      paste_origin.y = (canvas_ibuf->y - paste_ibuf->y) / 2;
      BKE_image_release_ibuf(sima->image, canvas_ibuf, lock);
    }
    has_mask = false;
  }

  if (!paste_ibuf) {
    return OPERATOR_CANCELLED;
  }

  /* Get the correct tile number for this paste operation. */
  ImageTile *paste_tile = BKE_image_get_tile_from_iuser(ima, &iuser);
  if (!paste_tile) {
    IMB_freeImBuf(paste_ibuf);
    if (paste_mask_ibuf) {
      IMB_freeImBuf(paste_mask_ibuf);
    }
    BKE_report(op->reports, RPT_ERROR, "Cannot determine image tile");
    return OPERATOR_CANCELLED;
  }
  int paste_tile_number = paste_tile->tile_number;

  /* Create floating selection state from clipboard. */
  ImageSelectMoveState *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->fragment_ibuf = paste_ibuf;
  state->fragment_mask_ibuf = paste_mask_ibuf;
  state->origin_px = paste_origin;
  state->size_px = {paste_ibuf->x, paste_ibuf->y};
  state->drag_offset = {0, 0};
  state->tile_number = paste_tile_number;
  state->iuser = iuser;

  /* If clipboard has a mask, create display buffer for lasso preview. */
  if (has_mask && paste_mask_ibuf) {
    state->fragment_display_ibuf = image_select_make_display_ibuf(paste_ibuf, paste_mask_ibuf);
  }

  /* Register draw callback for preview. */
  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->type) {
    state->owner_region_type = region->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, draw_select_move_preview, state, REGION_DRAW_POST_PIXEL);
  }

  /* Begin undo step for the floating selection.
   * Capture the destination tile's pixel pre-state immediately so that Ctrl+Z can restore
   * the canvas after the paste is confirmed.  Using push_begin_with_image (rather than
   * plain push_begin) mirrors what lift_source does for Move Selection and ensures the
   * undo step is non-empty even before commit writes any pixels.
   * If the ibuf cannot be acquired (uncommon), fall back to the empty-step variant so
   * undo_begun is still set and the step is closed cleanly in image_select_move_commit. */
  {
    ImageUser undo_iuser = iuser;
    undo_iuser.tile = paste_tile_number;
    void *undo_lock = nullptr;
    ImBuf *undo_ibuf = BKE_image_acquire_ibuf(ima, &undo_iuser, &undo_lock);
    if (undo_ibuf) {
      ED_image_undo_push_begin_with_image("Paste Selection", ima, undo_ibuf, &undo_iuser);
      BKE_image_release_ibuf(ima, undo_ibuf, undo_lock);
    }
    else {
      ED_image_undo_push_begin("Paste Selection", PaintMode::Texture2D);
    }
    /* Capture the copy-source tile's selection mask before it is freed below so that
     * Ctrl+Z also restores the original selection outline. */
    if (has_mask && cb) {
      ED_image_undo_capture_selection_mask(ima, cb->tile_number);
    }
  }
  state->undo_begun = true;

  g_floating_state = state;

  /* Clear the canvas selection mask so the lasso/box/circle outline at the copy position
   * disappears while the pasted fragment is floating.  The fragment rectangle preview
   * (draw callback) is now the sole visual indicator of the active selection.
   * Without this, the old selection mask outline remains painted on the canvas and
   * appears to "follow" the copy source rather than the moving fragment. */
  BKE_image_paint_selection_mask_free(ima);
  {
    Scene *paste_scene = CTX_data_scene(C);
    if (paste_scene) {
      paste_scene->toolsettings->imapaint.use_selection_mask = 0;
    }
  }
  DEG_id_tag_update(&ima->id, 0);

  WM_event_add_notifier(C, NC_WINDOW, nullptr);

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
   * (step_init -> nullptr). The subsequent ED_image_undo_push_end in commit would then call
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
}  /* namespace blender */
