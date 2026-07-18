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
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
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

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_intern.hh"
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
struct ImageSelectMoveState : public PaintSelectFloatingSession {
  /* One fragment per selected UDIM tile. */
  blender::Vector<SelectionTileFragment> fragments;
  /* Accumulated drag in UV space (tile 1001 has UV [0,1]x[0,1]). */
  float2 uv_drag_offset = {0.0f, 0.0f};
  /* First fragment's tile number (used by convert_to_transform). */
  int ref_tile_number = 1001;
  /* Previous cursor position of the active drag gesture, in REGION coordinates
   * (`wmEvent::mval`) -- the space #ui::view2d_region_to_view expects. */
  int2 prev_mouse_xy = {0, 0};
  /* Each append = snapshot of uv_drag_offset before a drag gesture. */
  blender::Vector<float2> drag_position_history;
};

/**
 * Clipboard state for copy/paste of selection mask fragments.
 * Persists across operations; freed on app shutdown.
 */
struct ImageClipboardState {
  blender::Vector<SelectionTileFragment> fragments;
  bool has_mask = false;
};

/* Global clipboard buffer. Cleared on app exit via WM_exit_handler.
 * Main-thread only: accessed exclusively from operator callbacks, so it needs no locking. */
static ImageClipboardState *g_clipboard_state = nullptr;
static bool g_clipboard_atexit_registered = false;
static void image_clipboard_state_free();

static void image_clipboard_atexit(void * /*user_data*/)
{
  image_clipboard_state_free();
}

/** \} */

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
  selection_tile_fragments_free(g_clipboard_state->fragments);
  MEM_delete(g_clipboard_state);
  g_clipboard_state = nullptr;
}

static void image_clipboard_state_set(blender::Vector<SelectionTileFragment> &&fragments,
                                      bool has_mask)
{
  image_clipboard_state_free();
  g_clipboard_state = MEM_new<ImageClipboardState>(__func__);
  g_clipboard_state->fragments = std::move(fragments);
  g_clipboard_state->has_mask = has_mask;
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

/* -------------------------------------------------------------------- */
/** \name Fragment Preview, Extraction and Commit
 * \{ */

static void draw_select_move_preview_ibuf(const ImBuf *draw_frag,
                                          const float sx0,
                                          const float sy0,
                                          const float zoom_x,
                                          const float zoom_y,
                                          const bool use_filter,
                                          const GPUBlend blend_mode)
{
  if (!draw_frag) {
    return;
  }

  GPU_blend(blend_mode);

  PixelBitmapDrawer drawer(GPU_SHADER_3D_IMAGE_COLOR);
  if (draw_frag->float_buffer.data) {
    const int ch = draw_frag->channels ? draw_frag->channels : 4;
    const gpu::TextureFormat fmt = (ch >= 4) ? gpu::TextureFormat::SFLOAT_16_16_16_16 :
                                   (ch == 3) ? gpu::TextureFormat::SFLOAT_16_16_16 :
                                               gpu::TextureFormat::SFLOAT_16;
    drawer.draw(sx0, sy0, draw_frag->x, draw_frag->y, fmt, use_filter,
                draw_frag->float_buffer.data, zoom_x, zoom_y, nullptr);
  }
  else if (draw_frag->byte_buffer.data) {
    drawer.draw(sx0, sy0, draw_frag->x, draw_frag->y, gpu::TextureFormat::UNORM_8_8_8_8,
                use_filter, draw_frag->byte_buffer.data, zoom_x, zoom_y, nullptr);
  }
}

/**
 * Global-UV rect of \a frag's user-visible outline (the tight selection box when known, otherwise
 * the whole capture rect) shifted by \a uv_drag_offset. Shared by the preview draw callback and
 * the cursor hit-test so the box the user sees is exactly the box they can grab.
 */
static void image_select_move_fragment_ui_uv_rect(const SelectionTileFragment &frag,
                                                  const float2 &uv_drag_offset,
                                                  float2 *r_min,
                                                  float2 *r_max)
{
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  const float tile_w = float(frag.geom.tile_size_px.x);
  const float tile_h = float(frag.geom.tile_size_px.y);
  const int2 ui_origin = image_select_fragment_ui_origin(frag);
  const int2 ui_size = image_select_fragment_ui_size(frag);

  *r_min = tile_uv + uv_drag_offset +
           float2(float(ui_origin.x) / tile_w, float(ui_origin.y) / tile_h);
  *r_max = *r_min + float2(float(ui_size.x) / tile_w, float(ui_size.y) / tile_h);
}

/**
 * Region post-pixel draw callback: renders the fragment pixel data at the destination position
 * with a yellow outline, giving a live preview of the move result.
 * The source region is already transparent on the canvas (cleared in lift_source).
 * Two GPU passes: sharp binary interior (nearest), then outward feather rim (linear).
 * Registered in invoke, removed on exit.
 */
static void draw_select_move_preview(const bContext * /*C*/, ARegion *region, void *arg)
{
  const ImageSelectMoveState *state = static_cast<const ImageSelectMoveState *>(arg);
  if (state->fragments.is_empty()) {
    return;
  }

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }

    const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float tile_w = float(frag.geom.tile_size_px.x);
    const float tile_h = float(frag.geom.tile_size_px.y);

    const float dst_uv_x0 = tile_uv.x + float(frag.geom.origin_px.x) / tile_w +
                            state->uv_drag_offset.x;
    const float dst_uv_y0 = tile_uv.y + float(frag.geom.origin_px.y) / tile_h +
                            state->uv_drag_offset.y;
    const float dst_uv_x1 = dst_uv_x0 + float(frag.geom.size_px.x) / tile_w;
    const float dst_uv_y1 = dst_uv_y0 + float(frag.geom.size_px.y) / tile_h;

    float2 outline_uv_min, outline_uv_max;
    image_select_move_fragment_ui_uv_rect(
        frag, state->uv_drag_offset, &outline_uv_min, &outline_uv_max);
    const float outline_uv_x0 = outline_uv_min.x;
    const float outline_uv_y0 = outline_uv_min.y;
    const float outline_uv_x1 = outline_uv_max.x;
    const float outline_uv_y1 = outline_uv_max.y;

    float sx0, sy0, sx1, sy1;
    ui::view2d_view_to_region_fl(&region->v2d, dst_uv_x0, dst_uv_y0, &sx0, &sy0);
    ui::view2d_view_to_region_fl(&region->v2d, dst_uv_x1, dst_uv_y1, &sx1, &sy1);

    float outline_sx0, outline_sy0, outline_sx1, outline_sy1;
    ui::view2d_view_to_region_fl(
        &region->v2d, outline_uv_x0, outline_uv_y0, &outline_sx0, &outline_sy0);
    ui::view2d_view_to_region_fl(
        &region->v2d, outline_uv_x1, outline_uv_y1, &outline_sx1, &outline_sy1);

    const float zoom_x = (sx1 - sx0) / float(frag.geom.size_px.x);
    const float zoom_y = (sy1 - sy0) / float(frag.geom.size_px.y);

    if (frag.preview.fragment_display_ibuf) {
      draw_select_move_preview_ibuf(frag.preview.fragment_display_ibuf,
                                    sx0,
                                    sy0,
                                    zoom_x,
                                    zoom_y,
                                    false,
                                    GPU_BLEND_ALPHA);
    }
    else {
      draw_select_move_preview_ibuf(frag.pixels.fragment_ibuf,
                                    sx0,
                                    sy0,
                                    zoom_x,
                                    zoom_y,
                                    false,
                                    GPU_BLEND_ALPHA);
    }

    if (frag.preview.fragment_feather_display_ibuf && frag.edge_policy.use_outward_feather) {
      draw_select_move_preview_ibuf(frag.preview.fragment_feather_display_ibuf,
                                    sx0,
                                    sy0,
                                    zoom_x,
                                    zoom_y,
                                    true,
                                    GPU_BLEND_ALPHA_PREMULT);
    }

    GPU_blend(GPU_BLEND_NONE);

    GPU_line_smooth(true);
    const uint pos = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4f(1.0f, 0.85f, 0.0f, 0.9f);
    immBegin(GPU_PRIM_LINE_LOOP, 4);
    immVertex2f(pos, outline_sx0, outline_sy0);
    immVertex2f(pos, outline_sx1, outline_sy0);
    immVertex2f(pos, outline_sx1, outline_sy1);
    immVertex2f(pos, outline_sx0, outline_sy1);
    immEnd();
    immUnbindProgram();
    GPU_line_smooth(false);
  }

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
  /* Presence check only: the `const Image *` overload avoids advancing the mask revision. */
  if (ima && BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(ima), preferred_tile))
  {
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

  /* Presence check only: the `const Image *` overload avoids advancing the mask revision. */
  if (!BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(ima), tile_number)) {
    BKE_report(op->reports, RPT_ERROR, "No selection on active tile");
    return nullptr;
  }

  int tight_min[2], tight_max[2];
  if (!BKE_image_paint_selection_mask_bounds(ima, tile_number, tight_min, tight_max)) {
    BKE_report(op->reports, RPT_ERROR, "Selection is empty");
    return nullptr;
  }

  int r_min[2] = {tight_min[0], tight_min[1]};
  int r_max[2] = {tight_max[0], tight_max[1]};

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

  BKE_image_paint_selection_bounds_expand_for_blend(
      r_min, r_max, ibuf->x, ibuf->y, BKE_image_paint_selection_edge_policy_get(ima));

  const int x_min = r_min[0];
  const int y_min = r_min[1];
  const int x_max = r_max[0];
  const int y_max = r_max[1];
  const int w = x_max - x_min;
  const int h = y_max - y_min;

  /* Kept past the release below: the mask sub-region extraction has to verify that the runtime
   * mask really was allocated at this tile's resolution before indexing it with these bounds. */
  const int tile_w = ibuf->x;
  const int tile_h = ibuf->y;

  /* Clamp bbox to ibuf bounds. */
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
  ImBuf *fragment = IMB_allocImBuf(
      safe_w, safe_h, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
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
    /* The mask is only sampled below, so take the `const Image *` overload. */
    const ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(ima),
                                                                   tile_number);
    /* `safe_*` is clamped against the tile buffer, not against the mask, so the mask may only be
     * read with those bounds when both have the same resolution. Falling back to an empty mask
     * keeps the fragment well-formed instead of half-reading a smaller allocation. */
    if (!image_select_mask_matches(tile_mask, tile_w, tile_h)) {
      tile_mask = nullptr;
    }
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

bool image_select_extract_per_tile(wmOperator *op,
                                   Image *ima,
                                   const ImageUser &base_iuser,
                                   blender::Vector<SelectionTileFragment> *r_fragments)
{
  bool any = false;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    int tight_min[2], tight_max[2];
    if (!BKE_image_paint_selection_mask_bounds(ima, tile->tile_number, tight_min, tight_max)) {
      continue;
    }

    int r_min[2] = {tight_min[0], tight_min[1]};
    int r_max[2] = {tight_max[0], tight_max[1]};

    ImageUser tile_iuser = base_iuser;
    tile_iuser.tile = tile->tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
    if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
      if (ibuf) {
        BKE_image_release_ibuf(ima, ibuf, lock);
      }
      continue;
    }

    const int tile_w = ibuf->x;
    const int tile_h = ibuf->y;

    const PaintSelectionEdgePolicy edge_policy = BKE_image_paint_selection_edge_policy_get(ima);
    BKE_image_paint_selection_bounds_expand_for_blend(r_min, r_max, tile_w, tile_h, edge_policy);
    const int frag_w = r_max[0] - r_min[0];
    const int frag_h = r_max[1] - r_min[1];
    if (frag_w <= 0 || frag_h <= 0) {
      BKE_image_release_ibuf(ima, ibuf, lock);
      continue;
    }

    const bool is_float = ibuf->float_buffer.data != nullptr;

    ImBuf *frag_ibuf = IMB_allocImBuf(
        frag_w, frag_h, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
    if (!frag_ibuf) {
      BKE_image_release_ibuf(ima, ibuf, lock);
      BKE_report(op->reports, RPT_ERROR, "Cannot allocate fragment buffer");
      continue;
    }
    if (is_float) {
      frag_ibuf->channels = ibuf->channels ? ibuf->channels : 4;
    }

    IMB_copy_rect(frag_ibuf, ibuf, int2{r_min[0], r_min[1]}, int2{0, 0}, int2{frag_w, frag_h});

    if (is_float) {
      const ColorSpace *cs = ibuf->float_buffer.colorspace;
      if (cs) {
        IMB_colormanagement_assign_float_colorspace(
            frag_ibuf, IMB_colormanagement_colorspace_get_name(cs));
      }
    }
    else {
      const ColorSpace *cs = ibuf->byte_buffer.colorspace;
      if (cs) {
        IMB_colormanagement_assign_byte_colorspace(
            frag_ibuf, IMB_colormanagement_colorspace_get_name(cs));
      }
    }

    BKE_image_release_ibuf(ima, ibuf, lock);

    /* The mask is only sampled below, so take the `const Image *` overload. */
    const ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(ima),
                                                                   tile->tile_number);
    /* `r_min` is clamped against the tile buffer; indexing the mask with it is only valid when the
     * mask was allocated at that same resolution. */
    if (!image_select_mask_matches(tile_mask, tile_w, tile_h)) {
      tile_mask = nullptr;
    }
    ImBuf *frag_mask = nullptr;
    if (tile_mask && tile_mask->float_buffer.data) {
      frag_mask = IMB_allocImBuf(frag_w, frag_h, ImBufFlags::Zero);
      IMB_alloc_float_pixels(frag_mask, 1);
      const float *src_mask = tile_mask->float_data();
      float *dst_mask = frag_mask->float_data_for_write();
      for (int ly = 0; ly < frag_h; ly++) {
        for (int lx = 0; lx < frag_w; lx++) {
          dst_mask[ly * frag_w + lx] = src_mask[(r_min[1] + ly) * tile_mask->x + (r_min[0] + lx)];
        }
      }
    }

    SelectionTileFragment frag;
    frag.pixels.fragment_ibuf = frag_ibuf;
    frag.pixels.fragment_mask_ibuf = frag_mask;
    frag.preview.fragment_blend_mask_ibuf = nullptr;
    frag.preview.fragment_display_ibuf = nullptr;
    frag.edge_policy = edge_policy;
    if (frag_mask) {
      image_select_fragment_update_preview_buffers(frag);
    }
    frag.geom.origin_px = int2{r_min[0], r_min[1]};
    frag.geom.size_px = int2{frag_w, frag_h};
    frag.geom.selection_origin_px = int2{tight_min[0], tight_min[1]};
    frag.geom.selection_size_px = int2{tight_max[0] - tight_min[0], tight_max[1] - tight_min[1]};
    frag.geom.tile_size_px = int2{tile_w, tile_h};
    frag.geom.tile_number = tile->tile_number;
    r_fragments->append(std::move(frag));
    any = true;
  }

  if (!any) {
    BKE_report(op->reports, RPT_WARNING, "Selection is empty");
  }
  return any;
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
  if (!sima || !sima->image) {
    return;
  }
  image_select_fragment_lift_source(
      C, sima->image, state->iuser, state->fragments, "Move Selection", state->undo_begun);
}

static void image_select_move_restore_source(bContext *C, ImageSelectMoveState *state)
{
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  Image *ima = sima ? sima->image : nullptr;
  image_select_fragment_restore_source(
      C, ima, state->iuser, state->fragments, state->undo_begun);
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
  SpaceImage *sima = state->owner_sima ? state->owner_sima : CTX_wm_space_image(C);
  if (!sima || !sima->image) {
    image_select_fragment_undo_push_end_if_open(state->undo_begun);
    return;
  }
  Image *ima = sima->image;

  if (state->fragments.is_empty()) {
    image_select_fragment_undo_push_end_if_open(state->undo_begun);
    return;
  }

  ImageUndoStep *us_open = image_select_undo_session_step_get();

  blender::Set<int> snapshotted_tiles;
  for (const SelectionTileFragment &frag : state->fragments) {
    snapshotted_tiles.add(frag.geom.tile_number);
  }

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }

    const float2 src_tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float tile_w = float(frag.geom.tile_size_px.x);
    const float tile_h = float(frag.geom.tile_size_px.y);

    const float frag_uv_x0 = src_tile_uv.x + float(frag.geom.origin_px.x) / tile_w +
                             state->uv_drag_offset.x;
    const float frag_uv_y0 = src_tile_uv.y + float(frag.geom.origin_px.y) / tile_h +
                             state->uv_drag_offset.y;
    const float frag_uv_x1 = frag_uv_x0 + float(frag.geom.size_px.x) / tile_w;
    const float frag_uv_y1 = frag_uv_y0 + float(frag.geom.size_px.y) / tile_h;

    const float *fmask = frag.pixels.fragment_mask_ibuf ?
                             frag.pixels.fragment_mask_ibuf->float_buffer.data :
                             nullptr;
    const ImBuf *blend_mask = frag.edge_policy.use_outward_feather ?
                                  frag.preview.fragment_blend_mask_ibuf :
                                  nullptr;

    for (ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
      const float2 dst_tile_uv = image_select_udim_tile_uv_origin(tile->tile_number);
      const float t_uv_x0 = dst_tile_uv.x;
      const float t_uv_y0 = dst_tile_uv.y;

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
      if (us_open && !snapshotted_tiles.contains(tile->tile_number)) {
        ED_image_undo_push(ima, ibuf, &tile_iuser, us_open);
        ED_image_undo_capture_selection_mask(ima, tile->tile_number);
        snapshotted_tiles.add(tile->tile_number);
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

      /* Clamping can invert a sliver-thin intersection. #IndexRange requires a non-negative size,
       * where the plain `for` loops this replaced simply did not execute; the rest of the tile
       * handling below is left to run exactly as before. */
      const int64_t dp_rows = std::max(0, dp_y1 - dp_y0);

      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

      const bool is_float = ibuf->float_data() != nullptr;

      if (is_float && frag.pixels.fragment_ibuf->float_data()) {
        const int channels = ibuf->channels ? ibuf->channels : 4;
        float *dst_data = ibuf->float_data_for_write();
        const float *src_data = frag.pixels.fragment_ibuf->float_data();

        /* Rows are independent: each iteration writes only its own row of `dst_data`. The writable
         * pointer is resolved above on this thread -- ImBuf::float_data_for_write() lazily
         * materializes through ImplicitSharingPtr and must never be called from a worker (see the
         * commentary in image_select_warp_commit()). */
        threading::parallel_for(IndexRange(dp_y0, dp_rows), 16, [&](const IndexRange range) {
          for (const int64_t py : range) {
            const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * tile_h;
            if (fy < 0.0f || fy >= float(frag.geom.size_px.y)) {
              continue;
            }

            const int mask_ly = int(fy);

            const float bly = std::max(
                0.0f, std::min(float(frag.geom.size_px.y) - 1.0001f, fy - 0.5f));
            const int by0 = int(bly);
            const int by1 = std::min(frag.geom.size_px.y - 1, by0 + 1);
            const float wy1 = bly - float(by0);
            const float wy0 = 1.0f - wy1;

            for (int px = dp_x0; px < dp_x1; px++) {
              const float fx = (t_uv_x0 + (float(px) + 0.5f) / dst_w - frag_uv_x0) * tile_w;
              if (fx < 0.0f || fx >= float(frag.geom.size_px.x)) {
                continue;
              }

              float m = 1.0f;
              if (blend_mask) {
                m = image_select_sample_mask_bilinear(blend_mask, fx, fy);
              }
              else if (fmask) {
                const int mask_lx = int(fx);
                if (fmask[mask_ly * frag.geom.size_px.x + mask_lx] <= SELECTION_MASK_THRESHOLD) {
                  continue;
                }
              }
              if (m <= 0.001f) {
                continue;
              }

              const float blx = std::max(
                  0.0f, std::min(float(frag.geom.size_px.x) - 1.0001f, fx - 0.5f));
              const int bx0 = int(blx);
              const int bx1 = std::min(frag.geom.size_px.x - 1, bx0 + 1);
              const float wx1 = blx - float(bx0);
              const float wx0 = 1.0f - wx1;

              float frag_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
              for (int c = 0; c < channels; c++) {
                frag_color[c] =
                    src_data[(by0 * frag.geom.size_px.x + bx0) * channels + c] * wx0 * wy0 +
                    src_data[(by0 * frag.geom.size_px.x + bx1) * channels + c] * wx1 * wy0 +
                    src_data[(by1 * frag.geom.size_px.x + bx0) * channels + c] * wx0 * wy1 +
                    src_data[(by1 * frag.geom.size_px.x + bx1) * channels + c] * wx1 * wy1;
              }

              const float frag_alpha = (channels >= 4) ? frag_color[3] : 1.0f;
              const float blend = m * frag_alpha;
              if (blend <= 0.001f) {
                continue;
              }

              float *dst_px = dst_data + (py * int(dst_w) + px) * channels;
              for (int c = 0; c < channels; c++) {
                dst_px[c] = (1.0f - blend) * dst_px[c] + blend * frag_color[c];
              }
            }
          }
        });
      }
      else if (!is_float && ibuf->byte_data() && frag.pixels.fragment_ibuf->byte_data()) {
        uint8_t *dst_data = ibuf->byte_data_for_write();
        const uint8_t *src_data = frag.pixels.fragment_ibuf->byte_data();

        /* Same row-independence and same pointer-resolution discipline as the float path above. */
        threading::parallel_for(IndexRange(dp_y0, dp_rows), 16, [&](const IndexRange range) {
          for (const int64_t py : range) {
            const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * tile_h;
            if (fy < 0.0f || fy >= float(frag.geom.size_px.y)) {
              continue;
            }

            const float bly = std::max(
                0.0f, std::min(float(frag.geom.size_px.y) - 1.0001f, fy - 0.5f));
            const int by0 = int(bly);
            const int by1 = std::min(frag.geom.size_px.y - 1, by0 + 1);
            const float wy1 = bly - float(by0);
            const float wy0 = 1.0f - wy1;

            for (int px = dp_x0; px < dp_x1; px++) {
              const float fx = (t_uv_x0 + (float(px) + 0.5f) / dst_w - frag_uv_x0) * tile_w;
              if (fx < 0.0f || fx >= float(frag.geom.size_px.x)) {
                continue;
              }

              float m = 1.0f;
              if (blend_mask) {
                m = image_select_sample_mask_bilinear(blend_mask, fx, fy);
              }
              else if (fmask) {
                const int lx = int(fx);
                const int ly = int(fy);
                if (fmask[ly * frag.geom.size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
                  continue;
                }
              }
              if (m <= 0.001f) {
                continue;
              }

              const float blx = std::max(
                  0.0f, std::min(float(frag.geom.size_px.x) - 1.0001f, fx - 0.5f));
              const int bx0 = int(blx);
              const int bx1 = std::min(frag.geom.size_px.x - 1, bx0 + 1);
              const float wx1 = blx - float(bx0);
              const float wx0 = 1.0f - wx1;

              float frag_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
              for (int c = 0; c < 4; c++) {
                frag_color[c] =
                    float(src_data[(by0 * frag.geom.size_px.x + bx0) * 4 + c]) * wx0 * wy0 +
                    float(src_data[(by0 * frag.geom.size_px.x + bx1) * 4 + c]) * wx1 * wy0 +
                    float(src_data[(by1 * frag.geom.size_px.x + bx0) * 4 + c]) * wx0 * wy1 +
                    float(src_data[(by1 * frag.geom.size_px.x + bx1) * 4 + c]) * wx1 * wy1;
              }

              const float frag_alpha = frag_color[3] / 255.0f;
              const float blend = m * frag_alpha;
              if (blend <= 0.001f) {
                continue;
              }

              uint8_t *dst_px = dst_data + (py * int(dst_w) + px) * 4;
              for (int c = 0; c < 4; c++) {
                /* Round rather than truncate. Truncating biases every blended pixel down by up
                 * to one level, which darkens the feathered rim and compounds across repeated
                 * moves of the same selection. Matches the byte path of
                 * #BKE_image_paint_selection_write_region. */
                dst_px[c] = uint8_t(std::clamp(
                    (1.0f - blend) * float(dst_px[c]) + blend * frag_color[c] + 0.5f,
                    0.0f,
                    255.0f));
              }
            }
          }
        });
      }

      BKE_image_mark_dirty(ima, ibuf);

      /* Update the destination tile's selection mask (nearest-neighbor -- mask is binary). */
      ImBuf *mask = BKE_image_paint_selection_mask_lookup(ima, tile->tile_number);
      if (mask) {
        float *mdata = mask->float_data_for_write();
        if (mdata) {
          for (int py = dp_y0; py < dp_y1; py++) {
            const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * tile_h;
            if (fy < 0.0f || fy >= float(frag.geom.size_px.y)) {
              continue;
            }
            const int ly = int(fy);
            for (int px = dp_x0; px < dp_x1; px++) {
              if (py < 0 || py >= mask->y || px < 0 || px >= mask->x) {
                continue;
              }
              const float fx = (t_uv_x0 + (float(px) + 0.5f) / dst_w - frag_uv_x0) * tile_w;
              if (fx < 0.0f || fx >= float(frag.geom.size_px.x)) {
                continue;
              }
              const int lx = int(fx);
              mdata[py * mask->x + px] = fmask ? fmask[ly * frag.geom.size_px.x + lx] : 1.0f;
            }
          }
        }
      }

      rcti dirty;
      BLI_rcti_init(&dirty, dp_x0, dp_x1, dp_y0, dp_y1);
      BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);

      BKE_image_release_ibuf(ima, ibuf, lock);
    }
  }

  image_select_fragment_undo_push_end_if_open(state->undo_begun);

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
  image_select_floating_draw_handle_clear(*state);
  selection_tile_fragments_free(state->fragments);
  MEM_delete(state);
}

void image_select_move_session_end_for_takeover(bContext *C, SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return;
  }
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;
  if (!state_ref) {
    return;
  }
  ImageSelectMoveState *state = state_ref;
  /* Cleared before the commit, not after: #image_select_move_commit notifies and tags the image,
   * and anything that looks the session up again in response must not find the state being torn
   * down here. */
  state_ref = nullptr;
  /* The session may still be mid-drag when another tool takes over (its modal handler is left
   * registered and exits on the next event, seeing a null state). Give the window its normal
   * cursor and the status bar its normal text back here. */
  image_select_floating_drag_end(C, state);
  image_select_floating_status_clear(C);
  image_select_move_commit(C, state);
  image_select_move_state_free(state);
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Image Select Move Operator
 * \{ */

bool image_select_move_is_floating(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  return image_select_move_is_floating_in_space(sima);
}

bool image_select_move_is_floating_in_space(const SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return false;
  }
  return image_select_floating_state_owns(sima->runtime->paint_select.move, sima);
}

/* Returns true when a selection fragment is currently floating in the active Image Editor. */
static bool image_select_move_floating_poll(bContext *C)
{
  return image_select_move_is_floating(C);
}

static bool image_select_move_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return false;
  }
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;

  if (image_select_transform_is_floating(C)) {
    return false;
  }
  if (image_select_warp_is_floating(C)) {
    return false;
  }
  /* Re-drag an already-floating fragment, no selection needed. */
  if (state_ref && sima == state_ref->owner_sima) {
    return true;
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Initial drag modal (runs only while LMB is held after invoke)
 * \{ */

static void image_select_move_update_drag_offset(bContext * /*C*/,
                                                 ImageSelectMoveState *state,
                                                 const wmEvent *event,
                                                 ARegion *region)
{
  /* #ui::view2d_region_to_view expects region-local coordinates, so both the stored previous
   * position and the current one come from `event->mval`, never `event->xy` (window space). */
  float prev_view_x, prev_view_y, cur_view_x, cur_view_y;
  ui::view2d_region_to_view(
      &region->v2d, state->prev_mouse_xy.x, state->prev_mouse_xy.y, &prev_view_x, &prev_view_y);
  ui::view2d_region_to_view(
      &region->v2d, event->mval[0], event->mval[1], &cur_view_x, &cur_view_y);

  state->uv_drag_offset.x += cur_view_x - prev_view_x;
  state->uv_drag_offset.y += cur_view_y - prev_view_y;
  state->prev_mouse_xy = int2{event->mval[0], event->mval[1]};
}

static wmOperatorStatus image_select_move_modal(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    image_select_floating_drag_end(C, nullptr);
    image_select_floating_status_clear(C);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;
  ImageSelectMoveState *state = state_ref;
  /* `state` is null when the session was ended elsewhere; `is_dragging` is cleared by
   * #image_select_move_undo_step_exec to abort the active drag. Both exits go through
   * #image_select_floating_drag_end so the drag cursor is always given back -- returning here
   * without restoring it used to leave #WM_CURSOR_NSEW_SCROLL applied indefinitely. */
  if (!state || !state->is_dragging) {
    image_select_floating_drag_end(C, state);
    image_select_floating_status_clear(C);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  /* Can be null when the area is closed or split mid-drag. */
  ARegion *region = CTX_wm_region(C);
  if (!region) {
    image_select_floating_drag_end(C, state);
    image_select_floating_status_clear(C);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }

  if (event->type == EVT_MODAL_MAP) {
    if (event->val == IMAGE_SELECT_FLOATING_MODAL_CANCEL) {
      image_select_floating_drag_end(C, state);
      image_select_floating_status_clear(C);
      image_select_move_restore_source(C, state);
      image_select_move_state_free(state);
      state_ref = nullptr;
      op->customdata = nullptr;
      ED_region_tag_redraw(region);
      return OPERATOR_CANCELLED;
    }
    /* Confirm / undo-step are not drag-scoped; the keymap-bound
     * #PAINT_OT_image_select_move_confirm / `_undo_step` operators own them. */
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case MOUSEMOVE:
      image_select_move_update_drag_offset(C, state, event, region);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;

    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        /* Drag finished -- fragment stays floating, modal ends.
         * Navigation (pan/zoom) is fully available between drag gestures. */
        image_select_floating_drag_end(C, state);
        image_select_floating_status_clear(C);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;

    default:
      /* Let keymap invoke #PAINT_OT_image_select_transform (and other bindings). */
      return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }
}

static void image_select_move_cancel(bContext *C, wmOperator *op)
{
  /* Called by WM when the modal is forcibly removed (e.g. area close, mode change). */
  image_select_floating_drag_end(C, nullptr);
  image_select_floating_status_clear(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->runtime) {
    ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;
    if (state_ref) {
      image_select_move_restore_source(C, state_ref);
      image_select_move_state_free(state_ref);
      state_ref = nullptr;
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
                                          Image * /*ima*/)
{
  float uv_x, uv_y;
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv_x, &uv_y);

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }
    float2 uv_min, uv_max;
    image_select_move_fragment_ui_uv_rect(frag, state->uv_drag_offset, &uv_min, &uv_max);

    /* Aggregate init rather than #BLI_rctf_init: the latter sanitizes inverted rects, which would
     * make a degenerate fragment bounds test succeed where the explicit comparison failed. */
    const rctf uv_rect{uv_min.x, uv_max.x, uv_min.y, uv_max.y};
    const float mouse_uv[2] = {uv_x, uv_y};
    if (BLI_rctf_isect_pt_v(&uv_rect, mouse_uv)) {
      return true;
    }
  }
  return false;
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
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;

  /* Re-drag path: a fragment is already floating in this space -- start another drag gesture,
   * but only when LMB was pressed inside the fragment's current bounding box.
   * If the press is outside, pass through so selection/commit operators handle it. */
  if (state_ref && state_ref->owner_sima == sima) {
    ARegion *region_for_hit = CTX_wm_region(C);
    if (region_for_hit &&
        !image_select_move_cursor_in_fragment(state_ref, region_for_hit, event, sima->image))
    {
      return OPERATOR_PASS_THROUGH;
    }
    /* Push the current position so Ctrl+Z can step back to it. */
    state_ref->drag_position_history.append(state_ref->uv_drag_offset);
    state_ref->is_dragging = true;
    state_ref->prev_mouse_xy = int2{event->mval[0], event->mval[1]};
    op->customdata = state_ref;
    WM_event_add_modal_handler(C, op);
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    image_select_floating_status_set(C, op->type, false);
    return OPERATOR_RUNNING_MODAL;
  }

  /* Another tool may still have a session floating in this space. It has to be ended before
   * #image_select_move_lift_source below opens an undo step, which would otherwise free the step
   * that session still believes it owns. See #image_select_floating_sessions_end. */
  image_select_floating_sessions_end(C,
                                     sima,
                                     IMAGE_SELECT_FLOATING_TOOL_TRANSFORM |
                                         IMAGE_SELECT_FLOATING_TOOL_GRADIENT |
                                         IMAGE_SELECT_FLOATING_TOOL_WARP);

  /* If a floating fragment from a different space is still alive, commit it. */
  if (state_ref) {
    image_select_move_commit(C, state_ref);
    image_select_move_state_free(state_ref);
    state_ref = nullptr;
  }
  Image *ima = sima->image;

  /* Resolved before anything is lifted: without a region there is nowhere to register the preview
   * draw callback, so the fragment would be lifted into an invisible session the user cannot see
   * or dismiss (reachable when the operator is called from Python or a menu). */
  ARegion *region = CTX_wm_region(C);
  if (!region || !region->runtime->type) {
    return OPERATOR_CANCELLED;
  }

  /* For UDIM images, sima->iuser.tile is reset to 0 after each temporary acquire.
   * The true active tile is tracked by ima->active_tile_index. */
  auto *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->iuser = sima->iuser;
  state->prev_mouse_xy = int2{event->mval[0], event->mval[1]};

  if (!image_select_extract_per_tile(op, ima, state->iuser, &state->fragments)) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }
  state->ref_tile_number = state->fragments[0].geom.tile_number;
  state->iuser.tile = state->ref_tile_number;

  image_select_move_lift_source(C, state);

  state->owner_region_type = region->runtime->type;
  state->draw_handle = ED_region_draw_cb_activate(
      state->owner_region_type, draw_select_move_preview, state, REGION_DRAW_POST_PIXEL);

  state_ref = state;

  /* Only start dragging immediately when the operator was invoked by an LMB click.
   * When invoked via keyboard shortcut (e.g. a key bound to Move Selection), the cursor
   * may be far from the fragment, so auto-starting the drag would jump the fragment to
   * an unexpected position.  In that case, enter floating mode without dragging -- the
   * first LMB press inside the fragment (delegated from a selection-tool invoke via
   * image_select_move_delegate_to_move_operator) will trigger the re-drag path above. */
  if (event->type == LEFTMOUSE) {
    /* The initial LMB invoke is the first drag gesture: snapshot offset so Ctrl+Z works. */
    state->drag_position_history.append(state->uv_drag_offset);
    state->is_dragging = true;
    op->customdata = state;
    WM_event_add_modal_handler(C, op);
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    image_select_floating_status_set(C, op->type, false);
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
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *state = state_ref;
  state_ref = nullptr;
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
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;
  if (!state_ref) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *state = state_ref;
  state_ref = nullptr;
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
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;
  ImageSelectMoveState *state = state_ref;
  if (!state) {
    return OPERATOR_CANCELLED;
  }

  ARegion *region = CTX_wm_region(C);

  if (state->is_dragging) {
    /* Abort the active drag and revert to the position saved before it started. The drag modal
     * will notice the cleared flag and finish, so the drag cursor has to be restored here -- the
     * modal's early-out cannot tell an aborted drag from a normal one. */
    state->uv_drag_offset = state->drag_position_history.pop_last();
    image_select_floating_drag_end(C, state);
    image_select_floating_status_clear(C);
    if (region) {
      ED_region_tag_redraw(region);
    }
    return OPERATOR_FINISHED;
  }

  if (!state->drag_position_history.is_empty()) {
    /* Step back one completed gesture. */
    state->uv_drag_offset = state->drag_position_history.pop_last();
    if (region) {
      ED_region_tag_redraw(region);
    }
    return OPERATOR_FINISHED;
  }

  /* History exhausted -- restore source pixels and end the floating operation. */
  state_ref = nullptr;
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

  return BKE_image_paint_selection_mask_has_any(sima->image);
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
  ImageUser iuser = sima->iuser;
  blender::Vector<SelectionTileFragment> fragments;
  if (!image_select_extract_per_tile(op, ima, iuser, &fragments)) {
    return OPERATOR_CANCELLED;
  }

  if (!fragments.is_empty() && fragments[0].pixels.fragment_ibuf) {
    if (!WM_clipboard_image_set_byte_buffer(fragments[0].pixels.fragment_ibuf)) {
      selection_tile_fragments_free(fragments);
      BKE_report(op->reports, RPT_WARNING, "Failed to set system clipboard");
      return OPERATOR_CANCELLED;
    }
  }

  image_clipboard_state_set(std::move(fragments), true);

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

  /* Have system clipboard. Only probe for availability here: polls run on every redraw and
   * #WM_clipboard_image_get decodes the whole image. This matches how #IMAGE_OT_clipboard_paste
   * polls. */
  return WM_clipboard_image_available();
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
  ImageSelectMoveState *&state_ref = sima->runtime->paint_select.move;

  Image *ima = sima->image;

  const ImageClipboardState *cb = clipboard_get();
  ImageUser iuser = sima->iuser;
  blender::Vector<SelectionTileFragment> paste_frags;

  if (cb && cb->has_mask) {
    paste_frags.reserve(cb->fragments.size());
    for (const SelectionTileFragment &src_frag : cb->fragments) {
      SelectionTileFragment dst_frag;
      dst_frag.pixels.fragment_ibuf = src_frag.pixels.fragment_ibuf ?
                                          IMB_dupImBuf(src_frag.pixels.fragment_ibuf) :
                                          nullptr;
      dst_frag.pixels.fragment_mask_ibuf = src_frag.pixels.fragment_mask_ibuf ?
                                               IMB_dupImBuf(src_frag.pixels.fragment_mask_ibuf) :
                                               nullptr;
      dst_frag.preview.fragment_blend_mask_ibuf = nullptr;
      dst_frag.preview.fragment_display_ibuf = nullptr;
      if (dst_frag.pixels.fragment_mask_ibuf) {
        image_select_fragment_update_preview_buffers(dst_frag);
      }
      dst_frag.geom.origin_px = src_frag.geom.origin_px;
      dst_frag.geom.size_px = src_frag.geom.size_px;
      dst_frag.geom.selection_origin_px = src_frag.geom.selection_origin_px;
      dst_frag.geom.selection_size_px = src_frag.geom.selection_size_px;
      dst_frag.edge_policy = src_frag.edge_policy;
      dst_frag.geom.tile_size_px = src_frag.geom.tile_size_px;
      dst_frag.geom.tile_number = src_frag.geom.tile_number;
      paste_frags.append(std::move(dst_frag));
    }
    if (!paste_frags.is_empty()) {
      iuser.tile = paste_frags[0].geom.tile_number;
    }
  }
  else {
    ImBuf *sys_ibuf = WM_clipboard_image_get();
    if (!sys_ibuf) {
      BKE_report(op->reports, RPT_ERROR, "No image in clipboard");
      return OPERATOR_CANCELLED;
    }
    ImBuf *paste_ibuf = IMB_dupImBuf(sys_ibuf);
    IMB_freeImBuf(sys_ibuf);
    if (!paste_ibuf) {
      BKE_report(op->reports, RPT_ERROR, "Failed to allocate paste buffer");
      return OPERATOR_CANCELLED;
    }

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

    int2 paste_origin = {0, 0};
    int2 tile_size = {paste_ibuf->x, paste_ibuf->y};
    void *lock = nullptr;
    ImBuf *canvas_ibuf = BKE_image_acquire_ibuf(ima, &iuser, &lock);
    if (canvas_ibuf) {
      paste_origin.x = (canvas_ibuf->x - paste_ibuf->x) / 2;
      paste_origin.y = (canvas_ibuf->y - paste_ibuf->y) / 2;
      tile_size = int2{canvas_ibuf->x, canvas_ibuf->y};
      BKE_image_release_ibuf(ima, canvas_ibuf, lock);
    }

    SelectionTileFragment frag;
    frag.pixels.fragment_ibuf = paste_ibuf;
    frag.geom.origin_px = paste_origin;
    frag.geom.size_px = int2{paste_ibuf->x, paste_ibuf->y};
    frag.geom.selection_origin_px = paste_origin;
    frag.geom.selection_size_px = int2{paste_ibuf->x, paste_ibuf->y};
    frag.edge_policy = BKE_image_paint_selection_edge_policy_feathered();
    frag.geom.tile_size_px = tile_size;
    const ImageTile *paste_tile = BKE_image_get_tile_from_iuser(ima, &iuser);
    frag.geom.tile_number = paste_tile ? paste_tile->tile_number : 1001;
    paste_frags.append(std::move(frag));
  }

  if (paste_frags.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Paste opens an undo step of its own below, so any other tool's floating session has to be
   * ended first or that step would be freed from under it. See #image_select_floating_sessions_end
   * for why the session slots are not mutually exclusive to begin with. */
  image_select_floating_sessions_end(C,
                                     sima,
                                     IMAGE_SELECT_FLOATING_TOOL_TRANSFORM |
                                         IMAGE_SELECT_FLOATING_TOOL_GRADIENT |
                                         IMAGE_SELECT_FLOATING_TOOL_WARP);

  /* A fragment may already be floating (pasting twice without confirming). Commit and free it
   * first, exactly like #image_select_move_invoke does: overwriting `state_ref` below would
   * leak the old state *and* leave its #ED_region_draw_cb_activate handle registered, drawing a
   * preview backed by freed memory. */
  if (state_ref) {
    image_select_move_commit(C, state_ref);
    image_select_move_state_free(state_ref);
    state_ref = nullptr;
  }

  ImageSelectMoveState *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->fragments = std::move(paste_frags);
  state->uv_drag_offset = {0.0f, 0.0f};
  state->ref_tile_number = state->fragments[0].geom.tile_number;
  state->iuser = iuser;
  state->iuser.tile = state->ref_tile_number;

  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->type) {
    state->owner_region_type = region->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, draw_select_move_preview, state, REGION_DRAW_POST_PIXEL);
  }

  const int first_paste_tile = state->fragments[0].geom.tile_number;
  {
    ImageUser undo_iuser = iuser;
    undo_iuser.tile = first_paste_tile;
    void *undo_lock = nullptr;
    ImBuf *undo_ibuf = BKE_image_acquire_ibuf(ima, &undo_iuser, &undo_lock);
    if (undo_ibuf) {
      ED_image_undo_push_begin_with_image("Paste Selection", ima, undo_ibuf, &undo_iuser);
      BKE_image_release_ibuf(ima, undo_ibuf, undo_lock);
    }
    else {
      ED_image_undo_push_begin("Paste Selection", PaintMode::Texture2D);
    }
  }
  ImageUndoStep *us_paste = image_select_undo_session_step_get();
  for (const SelectionTileFragment &frag : state->fragments) {
    ED_image_undo_capture_selection_mask(ima, frag.geom.tile_number);
    if (us_paste && frag.geom.tile_number != first_paste_tile) {
      ImageUser reg_iuser = iuser;
      reg_iuser.tile = frag.geom.tile_number;
      void *reg_lock = nullptr;
      ImBuf *reg_ibuf = BKE_image_acquire_ibuf(ima, &reg_iuser, &reg_lock);
      if (reg_ibuf) {
        ED_image_undo_push(ima, reg_ibuf, &reg_iuser, us_paste);
        BKE_image_release_ibuf(ima, reg_ibuf, reg_lock);
      }
    }
  }
  state->undo_begun = true;

  state_ref = state;

  /* Clear the canvas selection mask so the lasso/box/circle outline at the copy position
   * disappears while the pasted fragment is floating.  The fragment rectangle preview
   * (draw callback) is now the sole visual indicator of the active selection.
   * Without this, the old selection mask outline remains painted on the canvas and
   * appears to "follow" the copy source rather than the moving fragment. */
  BKE_image_paint_selection_mask_free(ima);
  DEG_id_tag_update(&ima->id, 0);

  WM_toolsystem_ref_set_by_id(C, "builtin.select_move");
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

/** \} */

wmOperatorStatus image_select_move_convert_to_transform(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&move_state_ref = sima->runtime->paint_select.move;
  ImageSelectMoveState *move_state = move_state_ref;
  if (!move_state || move_state->owner_sima != sima) {
    return OPERATOR_CANCELLED;
  }

  if (move_state->is_dragging) {
    image_select_floating_drag_end(C, move_state);
    image_select_floating_status_clear(C);
  }

  /* The transform session registers its own preview callback; drop this one before the move state
   * is freed so the region is never left with two previews (or a stale one). */
  image_select_floating_draw_handle_clear(*move_state);

  blender::Vector<SelectionTileFragment> fragments = std::move(move_state->fragments);
  const float2 uv_drag_offset = move_state->uv_drag_offset;
  const int ref_tile_number = move_state->ref_tile_number;
  const ImageUser iuser = move_state->iuser;
  const bool undo_begun = move_state->undo_begun;
  const bool proportional = RNA_boolean_get(op->ptr, "proportional");

  move_state->undo_begun = false;
  move_state_ref = nullptr;
  image_select_move_state_free(move_state);

  const wmOperatorStatus result = image_select_transform_adopt_move_state(C,
                                                 op,
                                                 event,
                                                 sima,
                                                 std::move(fragments),
                                                 uv_drag_offset,
                                                 ref_tile_number,
                                                 iuser,
                                                 undo_begun,
                                                 proportional);
  if (result == OPERATOR_CANCELLED && undo_begun) {
    image_select_undo_session_end();
  }
  return result;
}

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
   * BKE_undosys_type_from_context(nullptr) and crash.
   *
   * The same reasoning applies to the `_confirm` and `_cancel` operators below: they run
   * commit / restore_source, which close the very step opened here, so they must not let WM
   * finalize it first. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_move_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Selection Move";
  ot->idname = "PAINT_OT_image_select_move_confirm";
  ot->description = "Apply the moved fragment to the canvas";

  ot->exec = image_select_move_confirm_exec;
  ot->poll = image_select_move_floating_poll;

  /* No OPTYPE_UNDO, see #PAINT_OT_image_select_move. */
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_move_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Selection Move";
  ot->idname = "PAINT_OT_image_select_move_cancel";
  ot->description = "Restore the fragment to its original position and discard the move";

  ot->exec = image_select_move_cancel_exec;
  ot->poll = image_select_move_floating_poll;

  /* No OPTYPE_UNDO, see #PAINT_OT_image_select_move. */
  ot->flag = OPTYPE_REGISTER;
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

/* -------------------------------------------------------------------- */
/** \name Public C++ API
 * \{ */

bool ED_image_paint_select_is_moving(SpaceImage *sima)
{
  return sima && sima->runtime && sima->runtime->paint_select.move != nullptr;
}

void ED_image_paint_select_move_offset_get(SpaceImage *sima, float r_offset[2])
{
  const ImageSelectMoveState *state = (sima && sima->runtime) ?
                                          sima->runtime->paint_select.move :
                                          nullptr;
  if (state) {
    copy_v2_v2(r_offset, state->uv_drag_offset);
  }
  else {
    zero_v2(r_offset);
  }
}

void ED_image_paint_select_move_offset_set(SpaceImage *sima, const float offset[2])
{
  ImageSelectMoveState *state = (sima && sima->runtime) ? sima->runtime->paint_select.move :
                                                          nullptr;
  if (state) {
    copy_v2_v2(state->uv_drag_offset, offset);
    WM_main_add_notifier(NC_SPACE | ND_SPACE_IMAGE, nullptr);
  }
}

/** \} */

}  /* namespace blender */
