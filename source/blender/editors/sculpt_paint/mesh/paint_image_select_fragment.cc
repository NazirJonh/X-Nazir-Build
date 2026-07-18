/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_paint_types.hh"
#include "BKE_undo_system.hh"

#include "DEG_depsgraph.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_undo.hh"

#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_image_select_intern.hh"

namespace blender {

static ImBuf *image_select_alloc_display_ibuf(const ImBuf *src)
{
  ImBuf *disp = IMB_allocImBuf(src->x, src->y, ImBufFlags::Zero);
  IMB_alloc_float_pixels(disp, 4);
  disp->channels = 4;
  return disp;
}

void image_select_fragment_free(SelectionTileFragment &frag)
{
  if (frag.pixels.fragment_ibuf) {
    IMB_freeImBuf(frag.pixels.fragment_ibuf);
    frag.pixels.fragment_ibuf = nullptr;
  }
  if (frag.pixels.fragment_mask_ibuf) {
    IMB_freeImBuf(frag.pixels.fragment_mask_ibuf);
    frag.pixels.fragment_mask_ibuf = nullptr;
  }
  if (frag.preview.fragment_blend_mask_ibuf) {
    IMB_freeImBuf(frag.preview.fragment_blend_mask_ibuf);
    frag.preview.fragment_blend_mask_ibuf = nullptr;
  }
  if (frag.preview.fragment_display_ibuf) {
    IMB_freeImBuf(frag.preview.fragment_display_ibuf);
    frag.preview.fragment_display_ibuf = nullptr;
  }
  if (frag.preview.fragment_feather_display_ibuf) {
    IMB_freeImBuf(frag.preview.fragment_feather_display_ibuf);
    frag.preview.fragment_feather_display_ibuf = nullptr;
  }
}

ImBuf *image_select_make_display_ibuf_binary(const ImBuf *src, const ImBuf *binary_mask)
{
  BLI_assert(src && binary_mask);
  BLI_assert(binary_mask->float_buffer.data);

  ImBuf *disp = image_select_alloc_display_ibuf(src);
  const int pixel_count = src->x * src->y;
  float *dst = disp->float_data_for_write();
  const float *fmask = binary_mask->float_data();

  if (src->float_buffer.data) {
    const int ch = src->channels ? src->channels : 4;
    const float *src_f = src->float_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float alpha = fmask[i] > SELECTION_MASK_THRESHOLD ? 1.0f : 0.0f;
      dst[i * 4 + 0] = (ch > 0) ? src_f[i * ch + 0] : 0.0f;
      dst[i * 4 + 1] = (ch > 1) ? src_f[i * ch + 1] : 0.0f;
      dst[i * 4 + 2] = (ch > 2) ? src_f[i * ch + 2] : 0.0f;
      dst[i * 4 + 3] = alpha;
    }
  }
  else if (src->byte_buffer.data) {
    const uint8_t *src_b = src->byte_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float alpha = fmask[i] > SELECTION_MASK_THRESHOLD ? 1.0f : 0.0f;
      dst[i * 4 + 0] = src_b[i * 4 + 0] / 255.0f;
      dst[i * 4 + 1] = src_b[i * 4 + 1] / 255.0f;
      dst[i * 4 + 2] = src_b[i * 4 + 2] / 255.0f;
      dst[i * 4 + 3] = alpha;
    }
  }

  return disp;
}

ImBuf *image_select_make_display_ibuf_feather(const ImBuf *src,
                                              const ImBuf *binary_mask,
                                              const ImBuf *blend_mask)
{
  BLI_assert(src && binary_mask && blend_mask);
  BLI_assert(binary_mask->float_buffer.data && blend_mask->float_buffer.data);

  ImBuf *disp = image_select_alloc_display_ibuf(src);
  const int pixel_count = src->x * src->y;
  float *dst = disp->float_data_for_write();
  const float *binary = binary_mask->float_data();
  const float *blend = blend_mask->float_data();

  if (src->float_buffer.data) {
    const int ch = src->channels ? src->channels : 4;
    const float *src_f = src->float_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float alpha = binary[i] > SELECTION_MASK_THRESHOLD ? 0.0f : blend[i];
      dst[i * 4 + 0] = ((ch > 0) ? src_f[i * ch + 0] : 0.0f) * alpha;
      dst[i * 4 + 1] = ((ch > 1) ? src_f[i * ch + 1] : 0.0f) * alpha;
      dst[i * 4 + 2] = ((ch > 2) ? src_f[i * ch + 2] : 0.0f) * alpha;
      dst[i * 4 + 3] = alpha;
    }
  }
  else if (src->byte_buffer.data) {
    const uint8_t *src_b = src->byte_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float alpha = binary[i] > SELECTION_MASK_THRESHOLD ? 0.0f : blend[i];
      dst[i * 4 + 0] = (src_b[i * 4 + 0] / 255.0f) * alpha;
      dst[i * 4 + 1] = (src_b[i * 4 + 1] / 255.0f) * alpha;
      dst[i * 4 + 2] = (src_b[i * 4 + 2] / 255.0f) * alpha;
      dst[i * 4 + 3] = alpha;
    }
  }

  return disp;
}

float image_select_sample_mask_bilinear(const ImBuf *mask, float fx, float fy)
{
  if (!mask || !mask->float_buffer.data) {
    return 1.0f;
  }

  const int w = mask->x;
  const int h = mask->y;
  if (w <= 0 || h <= 0) {
    return 0.0f;
  }

  const float px = std::clamp(fx - 0.5f, 0.0f, float(w) - 1.0001f);
  const float py = std::clamp(fy - 0.5f, 0.0f, float(h) - 1.0001f);
  const int x0 = int(px);
  const int y0 = int(py);
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const float wx = px - float(x0);
  const float wy = py - float(y0);

  const float *m = mask->float_data();
  const float v00 = m[y0 * w + x0];
  const float v10 = m[y0 * w + x1];
  const float v01 = m[y1 * w + x0];
  const float v11 = m[y1 * w + x1];
  return (1.0f - wx) * (1.0f - wy) * v00 + wx * (1.0f - wy) * v10 +
         (1.0f - wx) * wy * v01 + wx * wy * v11;
}

void image_select_blend_buffer_into_canvas_at(ImBuf *dst_canvas,
                                              const ImBuf *src_fragment,
                                              const ImBuf *blend_mask,
                                              const int origin[2])
{
  if (!dst_canvas || !src_fragment || !blend_mask || !blend_mask->float_buffer.data) {
    return;
  }

  /* The fragment and the blend mask share one coordinate system; the canvas is offset by
   * `origin` within it, which is what lets callers work on a sub-rectangle of a tile. */
  const int src_w = src_fragment->x;
  const int src_h = src_fragment->y;
  const int ox = origin[0];
  const int oy = origin[1];
  const float *mask_data = blend_mask->float_buffer.data;

  /* Clip the region to the canvas rather than trusting the caller's arithmetic. */
  const int x_begin = std::max(0, -ox);
  const int y_begin = std::max(0, -oy);
  const int x_end = std::min(src_w, dst_canvas->x - ox);
  const int y_end = std::min(src_h, dst_canvas->y - oy);
  if (x_begin >= x_end || y_begin >= y_end) {
    return;
  }

  if (dst_canvas->float_data() && src_fragment->float_buffer.data) {
    const float *frag_data = src_fragment->float_buffer.data;
    float *canvas = dst_canvas->float_data_for_write();
    const int ch = dst_canvas->channels ? dst_canvas->channels : 4;
    for (int y = y_begin; y < y_end; y++) {
      for (int x = x_begin; x < x_end; x++) {
        const int64_t s = int64_t(y) * src_w + x;
        const float m = mask_data[s * 4 + 0];
        if (m <= 0.001f) {
          continue;
        }
        const float frag_alpha = (ch >= 4) ? frag_data[s * 4 + 3] : 1.0f;
        const float blend = m * frag_alpha;
        if (blend <= 0.001f) {
          continue;
        }
        const int64_t d = (int64_t(y + oy) * dst_canvas->x + (x + ox)) * ch;
        canvas[d + 0] = (1.0f - blend) * canvas[d + 0] + blend * frag_data[s * 4 + 0];
        canvas[d + 1] = (1.0f - blend) * canvas[d + 1] + blend * frag_data[s * 4 + 1];
        canvas[d + 2] = (1.0f - blend) * canvas[d + 2] + blend * frag_data[s * 4 + 2];
        if (ch >= 4) {
          canvas[d + 3] = (1.0f - blend) * canvas[d + 3] + blend * frag_data[s * 4 + 3];
        }
      }
    }
  }
  else if (dst_canvas->byte_data() && src_fragment->byte_buffer.data) {
    const uint8_t *frag_data = src_fragment->byte_buffer.data;
    uint8_t *canvas = dst_canvas->byte_data_for_write();
    for (int y = y_begin; y < y_end; y++) {
      for (int x = x_begin; x < x_end; x++) {
        const int64_t s = int64_t(y) * src_w + x;
        const float m = mask_data[s * 4 + 0];
        if (m <= 0.001f) {
          continue;
        }
        const float frag_alpha = frag_data[s * 4 + 3] / 255.0f;
        const float blend = m * frag_alpha;
        if (blend <= 0.001f) {
          continue;
        }
        const int64_t d = (int64_t(y + oy) * dst_canvas->x + (x + ox)) * 4;
        for (int c = 0; c < 4; c++) {
          canvas[d + c] = uint8_t(std::clamp(
              (1.0f - blend) * float(canvas[d + c]) + blend * float(frag_data[s * 4 + c]),
              0.0f,
              255.0f));
        }
      }
    }
  }
}


/* -------------------------------------------------------------------- */
/** \name Image undo session
 *
 * The only place in the selection tools that reads or reasons about #UndoStack::step_init.
 * See the architectural note in `paint_image_select_fragment.hh` for why an open step can go
 * missing under the floating-selection tools and what the real fix would be.
 * \{ */

bool image_select_undo_session_is_open()
{
  const UndoStack *ustack = ED_undo_stack_get();
  return ustack && ustack->step_init != nullptr;
}

ImageUndoStep *image_select_undo_session_step_get()
{
  UndoStack *ustack = ED_undo_stack_get();
  if (!ustack || !ustack->step_init || ustack->step_init->type != BKE_UNDOSYS_TYPE_IMAGE) {
    return nullptr;
  }
  return reinterpret_cast<ImageUndoStep *>(ustack->step_init);
}

void image_select_undo_session_end()
{
  /* ED_image_undo_push_end() does not tolerate a already-finalized step, so only close a step
   * that is still open. */
  if (!image_select_undo_session_is_open()) {
    return;
  }
  ED_image_undo_push_end();
}

/** \} */

void image_select_fragment_undo_push_end_if_open(bool &r_undo_begun)
{
  if (!r_undo_begun) {
    return;
  }
  image_select_undo_session_end();
  r_undo_begun = false;
}

void image_select_fragment_update_preview_buffers(SelectionTileFragment &frag)
{
  if (frag.preview.fragment_blend_mask_ibuf) {
    IMB_freeImBuf(frag.preview.fragment_blend_mask_ibuf);
    frag.preview.fragment_blend_mask_ibuf = nullptr;
  }
  if (frag.preview.fragment_display_ibuf) {
    IMB_freeImBuf(frag.preview.fragment_display_ibuf);
    frag.preview.fragment_display_ibuf = nullptr;
  }
  if (frag.preview.fragment_feather_display_ibuf) {
    IMB_freeImBuf(frag.preview.fragment_feather_display_ibuf);
    frag.preview.fragment_feather_display_ibuf = nullptr;
  }

  if (!frag.pixels.fragment_mask_ibuf) {
    return;
  }

  frag.preview.fragment_blend_mask_ibuf = BKE_image_paint_selection_compute_blend_mask(
      frag.pixels.fragment_mask_ibuf, frag.edge_policy);
  if (frag.pixels.fragment_ibuf && frag.preview.fragment_blend_mask_ibuf) {
    frag.preview.fragment_display_ibuf = image_select_make_display_ibuf_binary(
        frag.pixels.fragment_ibuf, frag.pixels.fragment_mask_ibuf);
    if (frag.edge_policy.use_outward_feather) {
      frag.preview.fragment_feather_display_ibuf = image_select_make_display_ibuf_feather(
          frag.pixels.fragment_ibuf,
          frag.pixels.fragment_mask_ibuf,
          frag.preview.fragment_blend_mask_ibuf);
    }
  }
}

void image_select_fragment_lift_source(bContext *C,
                                       Image *ima,
                                       const ImageUser &base_iuser,
                                       const Vector<SelectionTileFragment> &fragments,
                                       const char *undo_label,
                                       bool &r_undo_begun)
{
  if (fragments.is_empty() || !ima) {
    return;
  }

  const int first_tile = fragments[0].geom.tile_number;
  {
    ImageUser undo_iuser = base_iuser;
    undo_iuser.tile = first_tile;
    void *undo_lock = nullptr;
    ImBuf *undo_ibuf = BKE_image_acquire_ibuf(ima, &undo_iuser, &undo_lock);
    if (undo_ibuf) {
      ED_imapaint_clear_partial_redraw();
      ED_image_undo_push_begin_with_image(undo_label, ima, undo_ibuf, &undo_iuser);
      BKE_image_release_ibuf(ima, undo_ibuf, undo_lock);
    }
    else {
      ED_image_undo_push_begin(undo_label, PaintMode::Texture2D);
    }
    ED_image_undo_capture_selection_mask(ima, first_tile);
    r_undo_begun = true;
  }

  ImageUndoStep *us_open = image_select_undo_session_step_get();

  for (const SelectionTileFragment &frag : fragments) {
    if (us_open && frag.geom.tile_number != first_tile) {
      ImageUser tile_iuser = base_iuser;
      tile_iuser.tile = frag.geom.tile_number;
      void *reg_lock = nullptr;
      ImBuf *reg_ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &reg_lock);
      if (reg_ibuf) {
        ED_image_undo_push(ima, reg_ibuf, &tile_iuser, us_open);
        BKE_image_release_ibuf(ima, reg_ibuf, reg_lock);
      }
      ED_image_undo_capture_selection_mask(ima, frag.geom.tile_number);
    }

    ImageUser tile_iuser = base_iuser;
    tile_iuser.tile = frag.geom.tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
    if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
      if (ibuf) {
        BKE_image_release_ibuf(ima, ibuf, lock);
      }
      continue;
    }

    const float *fmask = frag.pixels.fragment_mask_ibuf ?
                             frag.pixels.fragment_mask_ibuf->float_buffer.data :
                             nullptr;
    const int ox = frag.geom.origin_px.x;
    const int oy = frag.geom.origin_px.y;

    if (ibuf->float_data()) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *data = ibuf->float_data_for_write();
      for (int ly = 0; ly < frag.geom.size_px.y; ly++) {
        const int py = oy + ly;
        if (py < 0 || py >= ibuf->y) {
          continue;
        }
        for (int lx = 0; lx < frag.geom.size_px.x; lx++) {
          if (fmask && fmask[ly * frag.geom.size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }
          const int px = ox + lx;
          if (px < 0 || px >= ibuf->x) {
            continue;
          }
          memset(data + (py * ibuf->x + px) * channels, 0, sizeof(float) * channels);
        }
      }
    }
    else if (ibuf->byte_data()) {
      uint8_t *data = ibuf->byte_data_for_write();
      for (int ly = 0; ly < frag.geom.size_px.y; ly++) {
        const int py = oy + ly;
        if (py < 0 || py >= ibuf->y) {
          continue;
        }
        for (int lx = 0; lx < frag.geom.size_px.x; lx++) {
          if (fmask && fmask[ly * frag.geom.size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }
          const int px = ox + lx;
          if (px < 0 || px >= ibuf->x) {
            continue;
          }
          memset(data + (py * ibuf->x + px) * 4, 0, 4);
        }
      }
    }

    ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(ima, frag.geom.tile_number);
    if (tile_mask) {
      float *mdata = tile_mask->float_data_for_write();
      if (mdata) {
        for (int ly = 0; ly < frag.geom.size_px.y; ly++) {
          const int py = oy + ly;
          if (py < 0 || py >= tile_mask->y) {
            continue;
          }
          for (int lx = 0; lx < frag.geom.size_px.x; lx++) {
            if (fmask && fmask[ly * frag.geom.size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
              continue;
            }
            const int px = ox + lx;
            if (px < 0 || px >= tile_mask->x) {
              continue;
            }
            mdata[py * tile_mask->x + px] = 0.0f;
          }
        }
        tile_mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
      }
    }

    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(ima, ibuf);

    rcti dirty;
    BLI_rcti_init(&dirty, ox, ox + frag.geom.size_px.x, oy, oy + frag.geom.size_px.y);
    const ImageTile *itile = BKE_image_get_tile(ima, frag.geom.tile_number);
    BKE_image_partial_update_mark_region(ima, itile, ibuf, &dirty);

    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  BKE_image_free_gputextures(ima);
  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

void image_select_fragment_restore_source(bContext *C,
                                          Image *ima,
                                          const ImageUser &base_iuser,
                                          const Vector<SelectionTileFragment> &fragments,
                                          const bool undo_begun)
{
  if (!ima) {
    if (undo_begun) {
      image_select_undo_session_end();
    }
    return;
  }

  for (const SelectionTileFragment &frag : fragments) {
    if (!frag.pixels.fragment_ibuf) {
      continue;
    }

    ImageUser tile_iuser = base_iuser;
    tile_iuser.tile = frag.geom.tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
    if (!ibuf || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
      if (ibuf) {
        BKE_image_release_ibuf(ima, ibuf, lock);
      }
      continue;
    }

    const float *fmask = frag.pixels.fragment_mask_ibuf ?
                             frag.pixels.fragment_mask_ibuf->float_buffer.data :
                             nullptr;
    const int ox = frag.geom.origin_px.x;
    const int oy = frag.geom.origin_px.y;

    if (ibuf->float_data() && frag.pixels.fragment_ibuf->float_data()) {
      const int channels = ibuf->channels ? ibuf->channels : 4;
      float *dst = ibuf->float_data_for_write();
      const float *src = frag.pixels.fragment_ibuf->float_data();
      for (int ly = 0; ly < frag.geom.size_px.y; ly++) {
        const int py = oy + ly;
        if (py < 0 || py >= ibuf->y) {
          continue;
        }
        for (int lx = 0; lx < frag.geom.size_px.x; lx++) {
          if (fmask && fmask[ly * frag.geom.size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }
          const int px = ox + lx;
          if (px < 0 || px >= ibuf->x) {
            continue;
          }
          memcpy(dst + (py * ibuf->x + px) * channels,
                 src + (ly * frag.geom.size_px.x + lx) * channels,
                 channels * sizeof(float));
        }
      }
    }
    else if (ibuf->byte_data() && frag.pixels.fragment_ibuf->byte_data()) {
      uint8_t *dst = ibuf->byte_data_for_write();
      const uint8_t *src = frag.pixels.fragment_ibuf->byte_data();
      for (int ly = 0; ly < frag.geom.size_px.y; ly++) {
        const int py = oy + ly;
        if (py < 0 || py >= ibuf->y) {
          continue;
        }
        for (int lx = 0; lx < frag.geom.size_px.x; lx++) {
          if (fmask && fmask[ly * frag.geom.size_px.x + lx] <= SELECTION_MASK_THRESHOLD) {
            continue;
          }
          const int px = ox + lx;
          if (px < 0 || px >= ibuf->x) {
            continue;
          }
          memcpy(dst + (py * ibuf->x + px) * 4, src + (ly * frag.geom.size_px.x + lx) * 4, 4);
        }
      }
    }

    ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(ima, frag.geom.tile_number);
    if (tile_mask && frag.pixels.fragment_mask_ibuf &&
        frag.pixels.fragment_mask_ibuf->float_buffer.data)
    {
      float *mdata = tile_mask->float_data_for_write();
      const float *fmask_restore = frag.pixels.fragment_mask_ibuf->float_data();
      if (mdata && fmask_restore) {
        for (int ly = 0; ly < frag.geom.size_px.y; ly++) {
          const int py = oy + ly;
          if (py < 0 || py >= tile_mask->y) {
            continue;
          }
          for (int lx = 0; lx < frag.geom.size_px.x; lx++) {
            const int px = ox + lx;
            if (px < 0 || px >= tile_mask->x) {
              continue;
            }
            mdata[py * tile_mask->x + px] = fmask_restore[ly * frag.geom.size_px.x + lx];
          }
        }
        tile_mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
      }
    }

    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(ima, ibuf);

    rcti dirty;
    BLI_rcti_init(&dirty, ox, ox + frag.geom.size_px.x, oy, oy + frag.geom.size_px.y);
    const ImageTile *itile = BKE_image_get_tile(ima, frag.geom.tile_number);
    BKE_image_partial_update_mark_region(ima, itile, ibuf, &dirty);
    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  BKE_image_free_gputextures(ima);

  if (undo_begun) {
    image_select_undo_session_end();
    UndoStack *undo_stack = ED_undo_stack_get();
    if (undo_stack) {
      BKE_undosys_stack_clear_active(undo_stack);
    }
  }

  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

} /* namespace blender */
