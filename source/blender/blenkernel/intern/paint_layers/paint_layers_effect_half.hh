/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The FP16 resting form of a CPU effect result (stage 2.3a), and the float form every caller sees.
 *
 * A cached effect result is read by the composite, the bake and the GPU gather as a float RGBA
 * #ImBuf, but most kinds do not need float precision while the result just sits in the cache: the
 * kernels already ran, and the next read can tolerate the half quantization (relative error about
 * 1e-3, the tolerance the effect tests name). Kind selection is not made here -- the cache decides
 * which kinds may rest as half (#paint_layer_effect_half_allowed) -- this header only owns the
 * storage and the half<->float boundary, so no kernel ever sees a half buffer.
 *
 * #PaintLayerEffectBuffer is the entry's value: exactly one of the two storages is set, never both.
 * Whole-map kinds (a flood, a distance field, a reduce) keep #floats, the kinds the CPU effect cache
 * patches regionally may rest as #halfs. #paint_layer_effect_buffer_read materializes a fresh float
 * buffer for a half entry; it is never stored beside the half one, so the resting cost stays half.
 * The trade is deliberate: every read of a half entry pays one full-canvas alloc and conversion
 * (O(canvas) per read, where the float entry is O(1)), and a pinned preview reads once a frame.
 * A float LRU or a cached materialization beside the half would need its own lifetime and budget;
 * neither is implemented, so the read cost is the accepted price of the halved resting footprint.
 */

#pragma once

#include <cstdint>
#include <memory>

#include "BLI_array.hh"
#include "BLI_math_half.hh"
#include "BLI_utildefines.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/** One effect result kept as RGBA FP16: four `uint16_t` per texel, row-major, scene linear. */
struct PaintLayerHalfBuffer {
  int width = 0;
  int height = 0;
  Array<uint16_t> pixels;
};

/**
 * The resting value of one cached effect result. Exactly one storage is set:
 * - #floats is the full-precision #ImBuf -- the whole-map kinds, whose result the regional patch and
 *   the bake read exactly, and the fallback when a pack allocation fails;
 * - #halfs is the FP16 form, #paint_layer_effect_buffer_read turns it into a float buffer on use.
 *
 * The FP16 form keeps all four components (colour and content alpha): the effect boundary is an
 * RGBA kernel, so a half storage must not drop alpha the way the scalar mask cache may.
 */
struct PaintLayerEffectBuffer {
  std::shared_ptr<ImBuf> floats;
  std::shared_ptr<PaintLayerHalfBuffer> halfs;

  bool is_half() const
  {
    return halfs != nullptr;
  }
};

/** A float RGBA buffer of \a width x \a height, owned by the returned shared pointer. The effect
 * result form: four channels, zero-initialized unless \a initialize_pixels is false -- the caller
 * then writes every texel before reading it. */
inline std::shared_ptr<ImBuf> paint_layer_effect_float_buffer_new(
    const int width, const int height, const bool initialize_pixels = true)
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

/** The float form a caller reads: the stored float buffer itself, or a fresh float copy of the half
 * one. A half entry never keeps the materialized copy, so the resting cost stays half. The copy
 * writes all four components of every texel, hence no zero-initialization (a full-canvas memset
 * would be paid again on every read). */
inline std::shared_ptr<ImBuf> paint_layer_effect_buffer_read(const std::shared_ptr<ImBuf> &stored)
{
  return stored;
}

inline std::shared_ptr<ImBuf> paint_layer_effect_buffer_read(const PaintLayerEffectBuffer &stored)
{
  if (stored.halfs == nullptr) {
    return stored.floats;
  }
  const PaintLayerHalfBuffer &half = *stored.halfs;
  std::shared_ptr<ImBuf> floats = paint_layer_effect_float_buffer_new(
      half.width, half.height, /*initialize_pixels=*/false);
  if (floats == nullptr) {
    return {};
  }
  const int64_t count = int64_t(half.width) * half.height * 4;
  math::half_to_float_array(half.pixels.data(), floats->float_data_for_write(), size_t(count));
  return floats;
}

inline void paint_layer_effect_buffer_dimensions(const std::shared_ptr<ImBuf> &stored,
                                                 int &r_width,
                                                 int &r_height)
{
  r_width = stored != nullptr ? stored->x : 0;
  r_height = stored != nullptr ? stored->y : 0;
}

inline void paint_layer_effect_buffer_dimensions(const PaintLayerEffectBuffer &stored,
                                                 int &r_width,
                                                 int &r_height)
{
  if (stored.halfs != nullptr) {
    r_width = stored.halfs->width;
    r_height = stored.halfs->height;
    return;
  }
  r_width = stored.floats != nullptr ? stored.floats->x : 0;
  r_height = stored.floats != nullptr ? stored.floats->y : 0;
}

/**
 * Pack the float RGBA \a rgba into the resting form: FP16 when \a use_half, else the float buffer
 * itself. The pack runs outside the cache mutex, on the computing thread, and the source stays the
 * kernel's float buffer -- no kernel reads or writes half pixels.
 *
 * A one-channel or non-RGBA buffer is refused by the assert: the effect boundary is RGBA, and a
 * half entry that dropped a component would change the answer.
 */
inline PaintLayerEffectBuffer paint_layer_effect_buffer_pack(std::shared_ptr<ImBuf> rgba,
                                                             const bool use_half)
{
  PaintLayerEffectBuffer result;
  if (!use_half || rgba == nullptr) {
    result.floats = std::move(rgba);
    return result;
  }
  /* Only an RGBA source can rest as half; a zero channel count is the ImBuf default that the
   * readers treat as four. Anything else keeps its float buffer in release too, where the assert
   * below is compiled out. */
  if (rgba->channels != 0 && rgba->channels != 4) {
    result.floats = std::move(rgba);
    return result;
  }
  BLI_assert(rgba->channels == 0 || rgba->channels == 4);
  const int64_t count = int64_t(rgba->x) * rgba->y * 4;
  auto half = std::make_shared<PaintLayerHalfBuffer>();
  half->width = rgba->x;
  half->height = rgba->y;
  half->pixels = Array<uint16_t>(count);
  /* Saturate to the finite FP16 range: a value above 65504 would otherwise rest as an infinity
   * and spread through the composite stack. NaN rests as zero. */
  math::float_to_half_make_finite_array(rgba->float_data(), half->pixels.data(), size_t(count));
  result.halfs = std::move(half);
  return result;
}

}  // namespace blender
