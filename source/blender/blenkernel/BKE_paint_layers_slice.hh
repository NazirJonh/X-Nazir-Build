/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The resting form of an Anchor Point slice's colour (stage 2.3b): FP16 RGBA in the cache, and the
 * float form every kernel-facing caller reads.
 *
 * A slice is rendered into a float RGBA buffer, but the composite only needs it while it just sits
 * in the cache: the half quantization (relative error about 1e-3) is the tolerance the slice tests
 * name. The coverage stays one float per texel -- it is a scalar plane, not a colour.
 *
 * #PaintLayerSliceColor is exactly one storage: #floats is the full-precision #ImBuf (the value a
 * caller that just rendered receives, a CPU mask effect's scalar grey, and the fallback when a pack
 * is refused), #halfs is the FP16 form. #paint_layer_slice_color_read_float materializes a fresh
 * float buffer for a half slice; it is never stored beside the half one, so the resting footprint
 * stays half. Half never reaches a kernel: every consumer decodes the tile it reads.
 */

#include <cstdint>
#include <memory>

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_math_half.hh"
#include "BLI_rect.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/** One Anchor Point slice colour kept as RGBA FP16: four `uint16_t` per texel, row-major, scene
 * linear. */
struct PaintLayerSliceHalf {
  int width = 0;
  int height = 0;
  Array<uint16_t> pixels;
};

/**
 * The resting colour of one slice. Exactly one storage is set:
 * - #floats is the full-precision #ImBuf -- the buffer a full render hands its caller, a CPU mask
 *   effect's one-channel float grey, and the fallback when a pack allocation is refused;
 * - #halfs is the FP16 form, #paint_layer_slice_color_read_float turns it into a float buffer on
 *   use.
 */
struct PaintLayerSliceColor {
  std::shared_ptr<ImBuf> floats;
  std::shared_ptr<PaintLayerSliceHalf> halfs;
  bool is_half() const
  {
    return halfs != nullptr;
  }
  /** Exactly one storage is set: #floats xor #halfs. */
  bool valid() const
  {
    return (floats != nullptr) != (halfs != nullptr);
  }
};

inline bool paint_layer_slice_color_is_half(const PaintLayerSliceColor &color)
{
  return color.is_half();
}

/** A float RGBA buffer of \a width x \a height, owned by the returned shared pointer. */
inline std::shared_ptr<ImBuf> paint_layer_slice_float_buffer_new(const int width,
                                                                 const int height,
                                                                 const bool initialize_pixels)
{
  ImBuf *ibuf = IMB_allocImBuf(width, height, ImBufFlags::Zero);
  if (ibuf == nullptr) {
    return {};
  }
  if (!IMB_alloc_float_pixels(ibuf, 4, initialize_pixels)) {
    IMB_freeImBuf(ibuf);
    return {};
  }
  return std::shared_ptr<ImBuf>(ibuf, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

inline void paint_layer_slice_color_dimensions(const PaintLayerSliceColor &color,
                                               int &r_width,
                                               int &r_height)
{
  if (color.halfs != nullptr) {
    r_width = color.halfs->width;
    r_height = color.halfs->height;
    return;
  }
  r_width = color.floats != nullptr ? color.floats->x : 0;
  r_height = color.floats != nullptr ? color.floats->y : 0;
}

/** The bytes \a color rests at: four FP16 per texel when half, else the actual bytes of the
 * stored #ImBuf buffers (a byte-only buffer is four bytes per RGBA texel, not four floats). */
inline size_t paint_layer_slice_color_bytes(const PaintLayerSliceColor &color)
{
  int width = 0;
  int height = 0;
  paint_layer_slice_color_dimensions(color, width, height);
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const size_t texels = size_t(width) * size_t(height);
  if (color.is_half()) {
    return texels * 4 * sizeof(uint16_t);
  }
  const ImBuf *ibuf = color.floats.get();
  if (ibuf == nullptr) {
    return 0;
  }
  const size_t channels = ibuf->channels != 0 ? size_t(ibuf->channels) : 4;
  size_t per_texel = 0;
  if (ibuf->float_buffer.data != nullptr) {
    per_texel += channels * sizeof(float);
  }
  if (ibuf->byte_buffer.data != nullptr) {
    per_texel += channels * sizeof(uint8_t);
  }
  return texels * per_texel;
}

/** The float form a caller reads: the stored float buffer itself, or a fresh float copy of the
 * half one. A half entry never keeps the materialized copy, so the resting cost stays half. */
inline std::shared_ptr<ImBuf> paint_layer_slice_color_read_float(const PaintLayerSliceColor &color)
{
  if (color.halfs == nullptr) {
    return color.floats;
  }
  const PaintLayerSliceHalf &half = *color.halfs;
  std::shared_ptr<ImBuf> floats = paint_layer_slice_float_buffer_new(
      half.width, half.height, /*initialize_pixels=*/false);
  if (floats == nullptr) {
    return {};
  }
  const int64_t count = int64_t(half.width) * half.height * 4;
  math::half_to_float_array(half.pixels.data(), floats->float_data_for_write(), size_t(count));
  return floats;
}

/**
 * Pack the float RGBA \a rgba into the resting form: FP16 when \a use_half, else the float buffer
 * itself.
 *
 * A one-channel or non-RGBA buffer is kept as float: a slice colour is RGBA, but a CPU mask
 * effect's grey rides the same type as one scalar channel, and a half form that dropped a
 * component would change the answer.
 */
inline PaintLayerSliceColor paint_layer_slice_color_pack(std::shared_ptr<ImBuf> rgba,
                                                         const bool use_half)
{
  PaintLayerSliceColor result;
  if (!use_half || rgba == nullptr) {
    result.floats = std::move(rgba);
    return result;
  }
  if (rgba->channels != 0 && rgba->channels != 4) {
    result.floats = std::move(rgba);
    BLI_assert(result.valid());
    return result;
  }
  if (rgba->float_buffer.data == nullptr) {
    /* A byte-only RGBA buffer has no float pixels to pack: keep it as it is rather than read
     * through a null float pointer. */
    result.floats = std::move(rgba);
    BLI_assert(result.valid());
    return result;
  }
  const int64_t count = int64_t(rgba->x) * rgba->y * 4;
  auto half = std::make_shared<PaintLayerSliceHalf>();
  half->width = rgba->x;
  half->height = rgba->y;
  half->pixels = Array<uint16_t>(count);
  /* Saturate to the finite FP16 range: a value above 65504 would otherwise rest as an infinity
   * and spread through the composite stack. NaN rests as zero. */
  math::float_to_half_make_finite_array(rgba->float_data(), half->pixels.data(), size_t(count));
  result.halfs = std::move(half);
  BLI_assert(result.valid());
  return result;
}

/** A copy that shares nothing with \a color: the regional patch writes one without touching the
 * buffers a live description still reads. Returns an empty colour if the copy cannot be allocated
 * (out of memory); a caller must fall back to a full render rather than assume validity. */
inline PaintLayerSliceColor paint_layer_slice_color_clone(const PaintLayerSliceColor &color)
{
  PaintLayerSliceColor result;
  if (color.halfs != nullptr) {
    auto half = std::make_shared<PaintLayerSliceHalf>();
    half->width = color.halfs->width;
    half->height = color.halfs->height;
    half->pixels = color.halfs->pixels;
    result.halfs = std::move(half);
    BLI_assert(result.valid());
    return result;
  }
  if (color.floats != nullptr) {
    ImBuf *copy = IMB_dupImBuf(color.floats.get());
    if (copy == nullptr) {
      return {};
    }
    result.floats = std::shared_ptr<ImBuf>(copy, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
  }
  BLI_assert(result.valid());
  return result;
}

/** One texel of \a color as stored RGBA floats: \a texel is the row-major index. A one-channel
 * float buffer reads as a grey, the way the tile decode reads it; any channel count other than
 * four aliases the missing blue/alpha rather than reading past the texel. The values are
 * premultiplied exactly as the slice rests; #paint_layer_slice_color_decode_tile is what
 * straightens them. */
inline void paint_layer_slice_color_sample(const PaintLayerSliceColor &color,
                                           const int64_t texel,
                                           float out[4])
{
  if (color.halfs != nullptr) {
    const uint16_t *pixel = color.halfs->pixels.data() + texel * 4;
    out[0] = math::half_to_float(pixel[0]);
    out[1] = math::half_to_float(pixel[1]);
    out[2] = math::half_to_float(pixel[2]);
    out[3] = math::half_to_float(pixel[3]);
    return;
  }
  const ImBuf &ibuf = *color.floats;
  if (ibuf.byte_buffer.data != nullptr) {
    const uchar *pixel = ibuf.byte_data() + texel * 4;
    out[0] = float(pixel[0]) / 255.0f;
    out[1] = float(pixel[1]) / 255.0f;
    out[2] = float(pixel[2]) / 255.0f;
    out[3] = float(pixel[3]) / 255.0f;
    return;
  }
  const int channels = ibuf.channels == 0 ? 4 : ibuf.channels;
  const float *pixel = ibuf.float_data() + texel * channels;
  out[0] = pixel[0];
  out[1] = channels >= 2 ? pixel[1] : pixel[0];
  out[2] = channels >= 3 ? pixel[2] : pixel[0];
  out[3] = channels == 4 ? pixel[3] : 1.0f;
}

/**
 * Decode the \a tile of \a color into \a dst as straight scene-linear RGBA: `tw * th * 4` floats.
 *
 * A slice has no colorspace of its own, so the only decode is the premultiplied straighten every
 * float map takes before the composite reads it; a half tile is converted and straightened the
 * same way, which is what makes the half and float paths agree texel for texel. A byte tile is
 * straight already and is left as it is. A float tile of any channel count is read without
 * stepping past its texel: a missing blue aliases red, a missing alpha reads one.
 */
inline void paint_layer_slice_color_decode_tile(const PaintLayerSliceColor &color,
                                                const rcti &tile,
                                                float *dst)
{
  const int tw = BLI_rcti_size_x(&tile);
  const int th = BLI_rcti_size_y(&tile);
  const ImBuf *float_source = color.floats.get();
  /* A byte buffer is straight already; only a float one is premultiplied. */
  const bool is_float = float_source != nullptr &&
                        float_source->byte_buffer.data == nullptr &&
                        float_source->float_buffer.data != nullptr;
  if (color.halfs != nullptr) {
    const PaintLayerSliceHalf &half = *color.halfs;
    for (int ty = 0; ty < th; ty++) {
      const int y = tile.ymin + ty;
      for (int tx = 0; tx < tw; tx++) {
        const int x = tile.xmin + tx;
        const uint16_t *pixel = half.pixels.data() + (int64_t(y) * half.width + x) * 4;
        float *out = dst + (int64_t(ty) * tw + tx) * 4;
        out[0] = math::half_to_float(pixel[0]);
        out[1] = math::half_to_float(pixel[1]);
        out[2] = math::half_to_float(pixel[2]);
        out[3] = math::half_to_float(pixel[3]);
      }
    }
  }
  else {
    const ImBuf &ibuf = *color.floats;
    const int channels = ibuf.channels == 0 ? 4 : ibuf.channels;
    const bool four = channels == 4;
    for (int ty = 0; ty < th; ty++) {
      const int y = tile.ymin + ty;
      for (int tx = 0; tx < tw; tx++) {
        const int x = tile.xmin + tx;
        float *out = dst + (int64_t(ty) * tw + tx) * 4;
        if (!is_float) {
          const uchar *pixel = ibuf.byte_data() + (int64_t(y) * ibuf.x + x) * 4;
          out[0] = float(pixel[0]) / 255.0f;
          out[1] = float(pixel[1]) / 255.0f;
          out[2] = float(pixel[2]) / 255.0f;
          out[3] = float(pixel[3]) / 255.0f;
        }
        else {
          const float *pixel = ibuf.float_buffer.data + (int64_t(y) * ibuf.x + x) * channels;
          out[0] = pixel[0];
          out[1] = channels >= 2 ? pixel[1] : pixel[0];
          out[2] = channels >= 3 ? pixel[2] : pixel[0];
          out[3] = four ? pixel[3] : 1.0f;
        }
      }
    }
  }
  /* Only a float or half slice is premultiplied: a byte buffer is straight already (the rule
   * #composite_decode_tile follows), so straighten just those. */
  if (color.is_half() || is_float) {
    for (int64_t i = 0; i < int64_t(tw) * th; i++) {
      const float alpha = dst[i * 4 + 3];
      if (alpha > 0.0f) {
        const float inv = 1.0f / alpha;
        dst[i * 4 + 0] *= inv;
        dst[i * 4 + 1] *= inv;
        dst[i * 4 + 2] *= inv;
      }
    }
  }
}

}  // namespace blender
