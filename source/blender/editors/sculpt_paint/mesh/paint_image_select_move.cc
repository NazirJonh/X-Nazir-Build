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
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_set.hh"
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
#include "WM_toolsystem.hh"
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
  SpaceImage *owner_sima = nullptr;
  ARegionType *owner_region_type = nullptr;
  /* One fragment per selected UDIM tile. */
  blender::Vector<SelectionTileFragment> fragments;
  /* Accumulated drag in UV space (tile 1001 has UV [0,1]x[0,1]). */
  float2 uv_drag_offset = {0.0f, 0.0f};
  /* First fragment's tile number (used by convert_to_transform). */
  int ref_tile_number = 1001;
  ImageUser iuser = {};
  int2 prev_mouse_xy = {0, 0};
  void *draw_handle = nullptr;
  bool undo_begun = false;
  /* Each append = snapshot of uv_drag_offset before a drag gesture. */
  blender::Vector<float2> drag_position_history;
  bool is_dragging = false;
};

/**
 * Clipboard state for copy/paste of selection mask fragments.
 * Persists across operations; freed on app shutdown.
 */
struct ImageClipboardState {
  blender::Vector<SelectionTileFragment> fragments;
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

    const int t_col = (frag.geom.tile_number - 1001) % 10;
    const int t_row = (frag.geom.tile_number - 1001) / 10;
    const float tile_w = float(frag.geom.tile_size_px.x);
    const float tile_h = float(frag.geom.tile_size_px.y);

    const int2 ui_origin = image_select_fragment_ui_origin(frag);
    const int2 ui_size = image_select_fragment_ui_size(frag);

    const float dst_uv_x0 = float(t_col) + float(frag.geom.origin_px.x) / tile_w +
                            state->uv_drag_offset.x;
    const float dst_uv_y0 = float(t_row) + float(frag.geom.origin_px.y) / tile_h +
                            state->uv_drag_offset.y;
    const float dst_uv_x1 = dst_uv_x0 + float(frag.geom.size_px.x) / tile_w;
    const float dst_uv_y1 = dst_uv_y0 + float(frag.geom.size_px.y) / tile_h;

    const float outline_uv_x0 = float(t_col) + float(ui_origin.x) / tile_w +
                                state->uv_drag_offset.x;
    const float outline_uv_y0 = float(t_row) + float(ui_origin.y) / tile_h +
                                state->uv_drag_offset.y;
    const float outline_uv_x1 = outline_uv_x0 + float(ui_size.x) / tile_w;
    const float outline_uv_y1 = outline_uv_y0 + float(ui_size.y) / tile_h;

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

    const ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(ima, tile->tile_number);
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

  if (state->fragments.is_empty()) {
    if (state->undo_begun && ustack && ustack->step_init != nullptr) {
      ED_image_undo_push_end();
    }
    return;
  }

  ImageUndoStep *us_open = (ustack && ustack->step_init &&
                            ustack->step_init->type == BKE_UNDOSYS_TYPE_IMAGE) ?
                               reinterpret_cast<ImageUndoStep *>(ustack->step_init) :
                               nullptr;

  blender::Set<int> snapshotted_tiles;
  for (const SelectionTileFragment &frag : state->fragments) {
    snapshotted_tiles.add(frag.geom.tile_number);
  }

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }

    const int src_t_col = (frag.geom.tile_number - 1001) % 10;
    const int src_t_row = (frag.geom.tile_number - 1001) / 10;
    const float tile_w = float(frag.geom.tile_size_px.x);
    const float tile_h = float(frag.geom.tile_size_px.y);

    const float frag_uv_x0 = float(src_t_col) + float(frag.geom.origin_px.x) / tile_w +
                             state->uv_drag_offset.x;
    const float frag_uv_y0 = float(src_t_row) + float(frag.geom.origin_px.y) / tile_h +
                             state->uv_drag_offset.y;
    const float frag_uv_x1 = frag_uv_x0 + float(frag.geom.size_px.x) / tile_w;
    const float frag_uv_y1 = frag_uv_y0 + float(frag.geom.size_px.y) / tile_h;

    const float *fmask = frag.pixels.fragment_mask_ibuf ?
                             frag.pixels.fragment_mask_ibuf->float_buffer.data :
                             nullptr;
    const ImBuf *blend_mask = frag.edge_policy.use_outward_feather ? frag.preview.fragment_blend_mask_ibuf : nullptr;

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

    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

    const bool is_float = ibuf->float_data() != nullptr;

    if (is_float && frag.pixels.fragment_ibuf->float_data()) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *dst_data = ibuf->float_data_for_write();
      const float *src_data = frag.pixels.fragment_ibuf->float_data();

      for (int py = dp_y0; py < dp_y1; py++) {
        const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * tile_h;
        if (fy < 0.0f || fy >= float(frag.geom.size_px.y)) {
          continue;
        }

        const int mask_ly = int(fy);

        const float bly = std::max(0.0f, std::min(float(frag.geom.size_px.y) - 1.0001f, fy - 0.5f));
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

          const float blx = std::max(0.0f, std::min(float(frag.geom.size_px.x) - 1.0001f, fx - 0.5f));
          const int bx0 = int(blx);
          const int bx1 = std::min(frag.geom.size_px.x - 1, bx0 + 1);
          const float wx1 = blx - float(bx0);
          const float wx0 = 1.0f - wx1;

          float frag_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
          for (int c = 0; c < channels; c++) {
            frag_color[c] = src_data[(by0 * frag.geom.size_px.x + bx0) * channels + c] * wx0 * wy0 +
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
    }
    else if (!is_float && ibuf->byte_data() && frag.pixels.fragment_ibuf->byte_data()) {
      uint8_t *dst_data = ibuf->byte_data_for_write();
      const uint8_t *src_data = frag.pixels.fragment_ibuf->byte_data();

      for (int py = dp_y0; py < dp_y1; py++) {
        const float fy = (t_uv_y0 + (float(py) + 0.5f) / dst_h - frag_uv_y0) * tile_h;
        if (fy < 0.0f || fy >= float(frag.geom.size_px.y)) {
          continue;
        }

        const float bly = std::max(0.0f, std::min(float(frag.geom.size_px.y) - 1.0001f, fy - 0.5f));
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

          const float blx = std::max(0.0f, std::min(float(frag.geom.size_px.x) - 1.0001f, fx - 0.5f));
          const int bx0 = int(blx);
          const int bx1 = std::min(frag.geom.size_px.x - 1, bx0 + 1);
          const float wx1 = blx - float(bx0);
          const float wx0 = 1.0f - wx1;

          float frag_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
          for (int c = 0; c < 4; c++) {
            frag_color[c] = float(src_data[(by0 * frag.geom.size_px.x + bx0) * 4 + c]) * wx0 * wy0 +
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
            dst_px[c] = uint8_t(std::clamp(
                (1.0f - blend) * float(dst_px[c]) + blend * frag_color[c], 0.0f, 255.0f));
          }
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
  selection_tile_fragments_free(state->fragments);
  MEM_delete(state);
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
  const ImageSelectMoveState *move_state = sima->runtime->paint_select.move;
  return move_state && move_state->owner_sima == sima;
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
  float prev_view_x, prev_view_y, cur_view_x, cur_view_y;
  ui::view2d_region_to_view(
      &region->v2d, state->prev_mouse_xy.x, state->prev_mouse_xy.y, &prev_view_x, &prev_view_y);
  ui::view2d_region_to_view(
      &region->v2d, event->xy[0], event->xy[1], &cur_view_x, &cur_view_y);

  state->uv_drag_offset.x += cur_view_x - prev_view_x;
  state->uv_drag_offset.y += cur_view_y - prev_view_y;
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
      /* Let keymap invoke #PAINT_OT_image_select_transform (and other bindings). */
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
                                          Image * /*ima*/)
{
  float uv_x, uv_y;
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv_x, &uv_y);

  for (const SelectionTileFragment &frag : state->fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }
    const int t_col = (frag.geom.tile_number - 1001) % 10;
    const int t_row = (frag.geom.tile_number - 1001) / 10;
    const float tile_w = float(frag.geom.tile_size_px.x);
    const float tile_h = float(frag.geom.tile_size_px.y);

    const int2 ui_origin = image_select_fragment_ui_origin(frag);
    const int2 ui_size = image_select_fragment_ui_size(frag);

    const float uv_x0 = float(t_col) + float(ui_origin.x) / tile_w + state->uv_drag_offset.x;
    const float uv_y0 = float(t_row) + float(ui_origin.y) / tile_h + state->uv_drag_offset.y;
    const float uv_x1 = uv_x0 + float(ui_size.x) / tile_w;
    const float uv_y1 = uv_y0 + float(ui_size.y) / tile_h;

    if (uv_x >= uv_x0 && uv_x <= uv_x1 && uv_y >= uv_y0 && uv_y <= uv_y1) {
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
    g_floating_state->drag_position_history.append(g_floating_state->uv_drag_offset);
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
  auto *state = MEM_new<ImageSelectMoveState>(__func__);
  state->owner_sima = sima;
  state->iuser = sima->iuser;
  state->prev_mouse_xy = int2{event->xy[0], event->xy[1]};

  if (!image_select_extract_per_tile(op, ima, state->iuser, &state->fragments)) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }
  state->ref_tile_number = state->fragments[0].geom.tile_number;
  state->iuser.tile = state->ref_tile_number;

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
    state->drag_position_history.append(state->uv_drag_offset);
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
    state->uv_drag_offset = state->drag_position_history.pop_last();
    state->is_dragging = false;
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

  const ImageClipboardState *cb = clipboard_get();
  ImageUser iuser = sima->iuser;
  blender::Vector<SelectionTileFragment> paste_frags;
  bool has_mask = false;

  if (cb && cb->has_mask) {
    paste_frags.reserve(cb->fragments.size());
    for (const SelectionTileFragment &src_frag : cb->fragments) {
      SelectionTileFragment dst_frag;
      dst_frag.pixels.fragment_ibuf = src_frag.pixels.fragment_ibuf ? IMB_dupImBuf(src_frag.pixels.fragment_ibuf) :
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
    has_mask = true;
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
      const ImageTile *active = static_cast<const ImageTile *>(
          BLI_findlink(&ima->tiles, ima->active_tile_index));
      iuser.tile = active ? active->tile_number :
                            static_cast<const ImageTile *>(ima->tiles.first)->tile_number;
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
  UndoStack *ustack_paste = ED_undo_stack_get();
  ImageUndoStep *us_paste = (ustack_paste && ustack_paste->step_init &&
                              ustack_paste->step_init->type == BKE_UNDOSYS_TYPE_IMAGE) ?
                                 reinterpret_cast<ImageUndoStep *>(ustack_paste->step_init) :
                                 nullptr;
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
  UNUSED_VARS(has_mask);

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
  ImageSelectMoveState *&g_floating_move = sima->runtime->paint_select.move;
  ImageSelectMoveState *move_state = g_floating_move;
  if (!move_state || move_state->owner_sima != sima) {
    return OPERATOR_CANCELLED;
  }

  if (move_state->is_dragging) {
    wmWindow *win = CTX_wm_window(C);
    if (win) {
      WM_cursor_modal_restore(win);
    }
    move_state->is_dragging = false;
  }

  if (move_state->draw_handle && move_state->owner_region_type) {
    ED_region_draw_cb_exit(move_state->owner_region_type, move_state->draw_handle);
    move_state->draw_handle = nullptr;
  }

  blender::Vector<SelectionTileFragment> fragments = std::move(move_state->fragments);
  const float2 uv_drag_offset = move_state->uv_drag_offset;
  const int ref_tile_number = move_state->ref_tile_number;
  const ImageUser iuser = move_state->iuser;
  const bool undo_begun = move_state->undo_begun;
  const bool proportional = RNA_boolean_get(op->ptr, "proportional");

  move_state->undo_begun = false;
  g_floating_move = nullptr;
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
    UndoStack *ustack = ED_undo_stack_get();
    if (ustack && ustack->step_init != nullptr) {
      ED_image_undo_push_end();
    }
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
