/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix_types.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_blender.hh"
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

  /* The display alpha is the *total* alpha: the selection mask's binary coverage times the
   * source pixel's own alpha. A layer map can be partially transparent (the layers below show
   * through it), and a preview that ignores that paints those regions solid. */
  if (src->float_buffer.data) {
    const int ch = src->channels ? src->channels : 4;
    const float *src_f = src->float_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float src_alpha = (ch >= 4) ? src_f[i * ch + 3] : 1.0f;
      const float alpha = (fmask[i] > SELECTION_MASK_THRESHOLD ? 1.0f : 0.0f) * src_alpha;
      dst[i * 4 + 0] = (ch > 0) ? src_f[i * ch + 0] : 0.0f;
      dst[i * 4 + 1] = (ch > 1) ? src_f[i * ch + 1] : 0.0f;
      dst[i * 4 + 2] = (ch > 2) ? src_f[i * ch + 2] : 0.0f;
      dst[i * 4 + 3] = alpha;
    }
  }
  else if (src->byte_buffer.data) {
    const uint8_t *src_b = src->byte_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float src_alpha = src_b[i * 4 + 3] / 255.0f;
      const float alpha = (fmask[i] > SELECTION_MASK_THRESHOLD ? 1.0f : 0.0f) * src_alpha;
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

  /* Premultiplied output, the total alpha = feather weight times the source pixel's own alpha
   * (see #image_select_make_display_ibuf_binary). */
  if (src->float_buffer.data) {
    const int ch = src->channels ? src->channels : 4;
    const float *src_f = src->float_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float src_alpha = (ch >= 4) ? src_f[i * ch + 3] : 1.0f;
      const float alpha =
          (binary[i] > SELECTION_MASK_THRESHOLD ? 0.0f : blend[i]) * src_alpha;
      dst[i * 4 + 0] = ((ch > 0) ? src_f[i * ch + 0] : 0.0f) * alpha;
      dst[i * 4 + 1] = ((ch > 1) ? src_f[i * ch + 1] : 0.0f) * alpha;
      dst[i * 4 + 2] = ((ch > 2) ? src_f[i * ch + 2] : 0.0f) * alpha;
      dst[i * 4 + 3] = alpha;
    }
  }
  else if (src->byte_buffer.data) {
    const uint8_t *src_b = src->byte_buffer.data;
    for (int i = 0; i < pixel_count; i++) {
      const float src_alpha = src_b[i * 4 + 3] / 255.0f;
      const float alpha =
          (binary[i] > SELECTION_MASK_THRESHOLD ? 0.0f : blend[i]) * src_alpha;
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
  return BKE_image_paint_selection_sample_mask_imbuf_bilinear(mask, fx, fy);
}

bool image_select_fragment_sample_bilinear(const SelectionTileFragment &frag,
                                           const float2 &f,
                                           const int channels,
                                           float4 &r_color)
{
  if (frag.pixels.fragment_ibuf == nullptr) {
    return false;
  }
  const int fw = frag.geom.size_px.x;
  const int fh = frag.geom.size_px.y;
  if (fw <= 0 || fh <= 0 || channels <= 0) {
    return false;
  }
  const float blx = std::max(0.0f, std::min(float(fw) - 1.0001f, f.x - 0.5f));
  const float bly = std::max(0.0f, std::min(float(fh) - 1.0001f, f.y - 0.5f));
  const int bx0 = int(blx);
  const int bx1 = std::min(fw - 1, bx0 + 1);
  const int by0 = int(bly);
  const int by1 = std::min(fh - 1, by0 + 1);
  const float wx1 = blx - float(bx0);
  const float wx0 = 1.0f - wx1;
  const float wy1 = bly - float(by0);
  const float wy0 = 1.0f - wy1;

  const int64_t texel_offset[4] = {int64_t(by0) * fw + bx0,
                                   int64_t(by0) * fw + bx1,
                                   int64_t(by1) * fw + bx0,
                                   int64_t(by1) * fw + bx1};
  const float weights[4] = {wx0 * wy0, wx1 * wy0, wx0 * wy1, wx1 * wy1};

  float4 texels[4];
  bool has_alpha = true;
  if (frag.pixels.fragment_ibuf->float_data() != nullptr) {
    const float *src = frag.pixels.fragment_ibuf->float_data();
    for (const int i : IndexRange(4)) {
      for (const int c : IndexRange(4)) {
        texels[i][c] = src[texel_offset[i] * channels + std::min(c, channels - 1)];
      }
    }
    has_alpha = channels >= 4;
  }
  else if (frag.pixels.fragment_ibuf->byte_data() != nullptr) {
    const uint8_t *src = frag.pixels.fragment_ibuf->byte_data();
    for (const int i : IndexRange(4)) {
      for (const int c : IndexRange(4)) {
        texels[i][c] = float(src[texel_offset[i] * 4 + c]) / 255.0f;
      }
    }
  }
  else {
    return false;
  }

  if (!has_alpha) {
    for (const int c : IndexRange(4)) {
      r_color[c] = 0.0f;
      for (const int i : IndexRange(4)) {
        r_color[c] += texels[i][c] * weights[i];
      }
    }
    return true;
  }

  /* Alpha-weighted, like the Clone sampler (#sample_channel_at_uv_cpu): the RGB of a transparent
   * texel (usually black) must not leak into the opaque neighbors it is mixed with, or the edge of
   * a layer's painted area lands as a dark fringe. */
  float3 rgb_weighted(0.0f);
  float alpha = 0.0f;
  for (const int i : IndexRange(4)) {
    rgb_weighted += float3(texels[i].x, texels[i].y, texels[i].z) * (texels[i].w * weights[i]);
    alpha += texels[i].w * weights[i];
  }
  const float3 rgb = alpha > 1e-6f ? rgb_weighted / alpha : float3(0.0f);
  r_color = float4(rgb.x, rgb.y, rgb.z, alpha);
  return true;
}

Array<float> image_select_fragment_content_mask(const SelectionTileFragment &frag, bool &r_any)
{
  r_any = false;
  const int w = frag.geom.size_px.x;
  const int h = frag.geom.size_px.y;
  if (w <= 0 || h <= 0) {
    return {};
  }
  Array<float> content(int64_t(w) * h, 0.0f);
  const float *mask = frag.pixels.fragment_mask_ibuf ? frag.pixels.fragment_mask_ibuf->float_data() :
                                                       nullptr;
  const ImBuf *pixels = frag.pixels.fragment_ibuf;
  const float *pixels_f = pixels ? pixels->float_data() : nullptr;
  const uint8_t *pixels_b = pixels ? pixels->byte_data() : nullptr;
  const int channels = (pixels && pixels->channels) ? pixels->channels : 4;
  /* No alpha channel: every selected pixel is data. */
  const bool has_alpha = pixels_b != nullptr || (pixels_f != nullptr && channels >= 4);
  constexpr float alpha_epsilon = 1e-4f;

  for (const int64_t i : content.index_range()) {
    if (mask != nullptr && mask[i] <= SELECTION_MASK_THRESHOLD) {
      continue;
    }
    if (has_alpha) {
      const float alpha = pixels_f ? pixels_f[i * channels + 3] : float(pixels_b[i * 4 + 3]) / 255.0f;
      if (alpha <= alpha_epsilon) {
        continue;
      }
    }
    content[i] = 1.0f;
    r_any = true;
  }
  return content;
}

bool image_select_fragments_have_content(const Span<Vector<SelectionTileFragment>> fragments)
{
  for (const Vector<SelectionTileFragment> &per_target : fragments) {
    for (const SelectionTileFragment &frag : per_target) {
      bool any = false;
      image_select_fragment_content_mask(frag, any);
      if (any) {
        return true;
      }
    }
  }
  return false;
}

float4 image_select_composite_over(const float4 &dst, const float4 &src, const float mask_weight)
{
  const float coverage = std::clamp(mask_weight, 0.0f, 1.0f) * std::clamp(src.w, 0.0f, 1.0f);
  if (coverage <= 0.0f) {
    return dst;
  }
  const float dst_alpha = std::clamp(dst.w, 0.0f, 1.0f);
  const float alpha_out = dst_alpha + coverage * (1.0f - dst_alpha);
  /* Straight-alpha "over" (the weight #clone_blend_texel gives the Clone stamp): a texel written
   * onto an empty target is copied as is, whatever RGB the target stored there. */
  const float weight = coverage / alpha_out;
  return float4(dst.x + (src.x - dst.x) * weight,
                dst.y + (src.y - dst.y) * weight,
                dst.z + (src.z - dst.z) * weight,
                alpha_out);
}

float2x2 image_select_normal_rotation(const float2x2 &linear)
{
  const float det = linear[0].x * linear[1].y - linear[0].y * linear[1].x;
  /* A reflection mirrors the second column so what is left is a pure rotation; the mirror is put
   * back on the result. */
  const float flip = det < 0.0f ? -1.0f : 1.0f;
  const float cos_sum = linear[0].x + linear[1].y * flip;
  const float sin_diff = linear[0].y - linear[1].x * flip;
  const float length = std::hypot(cos_sum, sin_diff);
  if (length < 1e-8f) {
    return float2x2::identity();
  }
  const float c = cos_sum / length;
  const float s = sin_diff / length;
  return float2x2({c, s}, {-s * flip, c * flip});
}

bool image_select_normal_rotation_is_identity(const float2x2 &rotation)
{
  constexpr float eps = 1e-5f;
  return std::abs(rotation[0].x - 1.0f) < eps && std::abs(rotation[0].y) < eps &&
         std::abs(rotation[1].x) < eps && std::abs(rotation[1].y - 1.0f) < eps;
}

void image_select_normal_color_rotate(float4 &color, const float2x2 &rotation)
{
  const float2 xy = rotation * float2(color[0] * 2.0f - 1.0f, color[1] * 2.0f - 1.0f);
  color[0] = std::clamp(xy.x * 0.5f + 0.5f, 0.0f, 1.0f);
  color[1] = std::clamp(xy.y * 0.5f + 0.5f, 0.0f, 1.0f);
}

void image_select_normal_buffer_rotate(ImBuf &ibuf, const float2x2 &rotation)
{
  const int64_t pixel_num = int64_t(ibuf.x) * int64_t(ibuf.y);
  if (float *data = ibuf.float_data_for_write()) {
    const int channels = ibuf.channels >= 4 ? ibuf.channels : 4;
    threading::parallel_for(IndexRange(pixel_num), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        float *px = data + i * channels;
        if (px[3] <= 0.0f) {
          continue;
        }
        float4 color(px[0], px[1], px[2], px[3]);
        image_select_normal_color_rotate(color, rotation);
        px[0] = color[0];
        px[1] = color[1];
      }
    });
  }
  else if (uint8_t *data = ibuf.byte_data_for_write()) {
    threading::parallel_for(IndexRange(pixel_num), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        uint8_t *px = data + i * 4;
        if (px[3] == 0) {
          continue;
        }
        float4 color(float(px[0]) / 255.0f, float(px[1]) / 255.0f, 0.0f, 1.0f);
        image_select_normal_color_rotate(color, rotation);
        px[0] = uint8_t(color[0] * 255.0f + 0.5f);
        px[1] = uint8_t(color[1] * 255.0f + 0.5f);
      }
    });
  }
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
          canvas[d + c] = uint8_t(std::clamp((1.0f - blend) * float(canvas[d + c]) +
                                                 blend * float(frag_data[s * 4 + c]),
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
 * See the architectural note in `paint_image_select_fragment.hh` for the model this implements:
 * a step is only ever opened and closed within a single call to
 * #image_select_fragment_commit_with_undo, so it can never be interrupted by another operator.
 * \{ */

ImageUndoStep *image_select_undo_session_step_get()
{
  if (!ED_image_undo_is_step_active()) {
    return nullptr;
  }
  UndoStack *ustack = ED_undo_stack_get();
  return reinterpret_cast<ImageUndoStep *>(ustack->step_init);
}

/** \} */

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
                                       Span<SelectionTileFragment> fragments)
{
  if (fragments.is_empty() || !ima) {
    return;
  }

  for (const SelectionTileFragment &frag : fragments) {
    if (frag.is_pasted) {
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
  if (C) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
  }
}

void image_select_fragment_restore_source(bContext *C,
                                          Image *ima,
                                          const ImageUser &base_iuser,
                                          Span<SelectionTileFragment> fragments)
{
  if (!ima) {
    return;
  }

  for (const SelectionTileFragment &frag : fragments) {
    if (frag.is_pasted || !frag.pixels.fragment_ibuf) {
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

  DEG_id_tag_update(&ima->id, 0);
  if (C) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
  }
}

void image_select_fragment_commit_with_undo(bContext * /*C*/,
                                            Image *ima,
                                            const ImageUser &base_iuser,
                                            Span<SelectionTileFragment> fragments,
                                            const char *undo_label,
                                            FunctionRef<void()> write_final)
{
  if (fragments.is_empty() || !ima) {
    return;
  }

  /* Put the true originals back on the canvas first, so the "before" snapshot #push_begin
   * captures below is the pre-edit state rather than the lifted/holed one. Pasted fragments are
   * skipped by #image_select_fragment_restore_source itself: there is nothing on the canvas to
   * restore for them. */
  image_select_fragment_restore_source(nullptr, ima, base_iuser, fragments);

  /* Anchor the step on the first non-pasted fragment's tile, so its "before" snapshot is the
   * restored original above. If every fragment is pasted (a pure Paste Selection commit) there is
   * nothing to disturb, and the untouched canvas at the first fragment's tile is itself the
   * correct "before". */
  int first_tile = fragments[0].geom.tile_number;
  for (const SelectionTileFragment &frag : fragments) {
    if (!frag.is_pasted) {
      first_tile = frag.geom.tile_number;
      break;
    }
  }

  ED_imapaint_clear_partial_redraw();
  {
    ImageUser undo_iuser = base_iuser;
    undo_iuser.tile = first_tile;
    void *undo_lock = nullptr;
    ImBuf *undo_ibuf = BKE_image_acquire_ibuf(ima, &undo_iuser, &undo_lock);
    if (undo_ibuf) {
      ED_image_undo_push_begin_with_image(undo_label, ima, undo_ibuf, &undo_iuser);
      BKE_image_release_ibuf(ima, undo_ibuf, undo_lock);
    }
    else {
      ED_image_undo_push_begin(undo_label, PaintMode::Texture2D);
    }
    ED_image_undo_capture_selection_mask(ima, first_tile);
  }

  /* Register every other fragment tile's "before" state into the same step. #write_final may
   * register further tiles beyond these (destination tiles a move/transform commit reaches that
   * were not part of the original lift) through #image_select_undo_session_step_get -- exactly
   * the same call used here. */
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
  }

  write_final();

  /* The step was opened just above in this same call, so it is always still open here: no other
   * operator can have run in between. */
  ED_image_undo_push_end();
}

/* -------------------------------------------------------------------- */
/** \name Shared selection clipboard
 * \{ */

static ImageSelectClipboard *g_clipboard = nullptr;

static void image_select_clipboard_atexit(void * /*user_data*/)
{
  image_select_clipboard_clear();
  /* Clearing only drops the contents; the struct itself was allocated by #image_select_clipboard_get
   * and is reported as a leak on exit if it is not freed here. */
  MEM_delete(g_clipboard);
  g_clipboard = nullptr;
}

ImageSelectClipboard *image_select_clipboard_get()
{
  if (g_clipboard == nullptr) {
    g_clipboard = MEM_new<ImageSelectClipboard>(__func__);
    BKE_blender_atexit_register(image_select_clipboard_atexit, nullptr);
  }
  return g_clipboard;
}

void image_select_clipboard_clear()
{
  if (g_clipboard == nullptr) {
    return;
  }
  for (ImageSelectClipboardGroup &group : g_clipboard->groups) {
    selection_tile_fragments_free(group.fragments);
  }
  g_clipboard->groups.clear();
  g_clipboard->has_mask = false;
}

/** \} */

} /* namespace blender */
