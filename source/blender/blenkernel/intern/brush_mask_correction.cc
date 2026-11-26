/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_brush_mask_correction.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_interp.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_colortools.hh"
#include "BKE_image.hh"

#include "DNA_brush_types.h"
#include "DNA_color_types.h"
#include "DNA_image_types.h"
#include "DNA_texture_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "BLO_read_write.hh"

#include "paint_layers/paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Lifetime
 * \{ */

BrushMaskCorrection *BKE_brush_mask_correction_ensure(Brush &brush)
{
  if (brush.mask_correction == nullptr) {
    brush.mask_correction = MEM_new<BrushMaskCorrection>(__func__);
    /* The curve is part of the block and is expected to exist as soon as the block does: the UI
     * draws its widget unconditionally and the sampler reads it the moment #CURVE is switched on.
     * An identity curve is the neutral value. */
    brush.mask_correction->curve = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  }
  return brush.mask_correction;
}

void BKE_brush_mask_correction_free(BrushMaskCorrection *correction)
{
  if (correction == nullptr) {
    return;
  }
  BKE_curvemapping_free(correction->curve);
  MEM_delete(correction);
}

void BKE_brush_mask_correction_delete(Brush &brush)
{
  BKE_brush_mask_correction_free(brush.mask_correction);
  brush.mask_correction = nullptr;
}

BrushMaskCorrection *BKE_brush_mask_correction_copy(const BrushMaskCorrection *src)
{
  if (src == nullptr) {
    return nullptr;
  }
  BrushMaskCorrection *dst = MEM_new<BrushMaskCorrection>(__func__, dna::shallow_copy(*src));
  dst->curve = BKE_curvemapping_copy(src->curve);
  return dst;
}

const BrushMaskCorrection *BKE_brush_mask_correction_get(const Brush &brush)
{
  if (brush.mask_correction == nullptr) {
    return nullptr;
  }
  if ((brush.mask_correction->flag & BRUSH_MASK_CORRECTION_ENABLED) == 0) {
    return nullptr;
  }
  return brush.mask_correction;
}

bool BKE_brush_mask_correction_active(const Brush &brush)
{
  return BKE_brush_mask_correction_get(brush) != nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Application
 * \{ */

/**
 * Curve tables are built lazily. A stroke evaluates the mask from several worker threads, so the
 * one-time build is serialized here; after that the table is read-only for the lifetime of the
 * stroke and `BKE_curvemapping_evaluateF` is safe to call concurrently.
 */
static std::mutex mask_correction_curve_mutex;

static const CurveMapping *mask_correction_curve_prepare(const CurveMapping *curve)
{
  if (curve == nullptr) {
    return nullptr;
  }
  if (curve->cm[0].table == nullptr) {
    std::lock_guard lock(mask_correction_curve_mutex);
    if (curve->cm[0].table == nullptr) {
      BKE_curvemapping_init(const_cast<CurveMapping *>(curve));
    }
  }
  return curve;
}

/** A unit-range parameter from a possibly corrupted file: no NaN/Inf, no out-of-range value. */
static float mask_correction_sanitize_unit(const float value, const float fallback)
{
  return std::isfinite(value) ? math::clamp(value, 0.0f, 1.0f) : fallback;
}

/**
 * Quantize a stage radius the way the filter will use it, so `use_spatial()` and the map builder
 * cannot disagree. A positive radius below half a texel becomes one texel instead of a silent
 * `lround() == 0` no-op; a non-finite or non-positive radius disables the stage; a huge value from
 * a damaged file is capped instead of allocating an unbounded kernel.
 */
static int mask_correction_stage_radius(const float radius, const int max_radius)
{
  if (!std::isfinite(radius) || radius <= 0.0f) {
    return 0;
  }
  return math::clamp(int(std::lround(std::min(radius, float(max_radius)))), 1, max_radius);
}

BrushMaskCorrectionPrepared BKE_brush_mask_correction_prepare(
    const BrushMaskCorrection &correction)
{
  BrushMaskCorrectionPrepared prepared;
  prepared.enabled = (correction.flag & BRUSH_MASK_CORRECTION_ENABLED) != 0;
  prepared.use_levels = (correction.flag & BRUSH_MASK_CORRECTION_LEVELS) != 0;
  prepared.use_smoothstep = (correction.flag & BRUSH_MASK_CORRECTION_SMOOTHSTEP) != 0;
  prepared.use_curve = (correction.flag & BRUSH_MASK_CORRECTION_CURVE) != 0;
  prepared.use_threshold = (correction.flag & BRUSH_MASK_CORRECTION_THRESHOLD) != 0;
  prepared.use_posterize = (correction.flag & BRUSH_MASK_CORRECTION_POSTERIZE) != 0;
  prepared.use_clamp = (correction.flag & BRUSH_MASK_CORRECTION_CLAMP) != 0;

  /* Radius quantization decides whether a spatial stage is active at all, matching the DNA
   * contract that a zero radius disables its stage. Sharpen additionally needs a positive amount. */
  prepared.blur_radius = mask_correction_stage_radius(correction.blur_radius, 256);
  prepared.sharpen_radius = mask_correction_stage_radius(correction.sharpen_radius, 256);
  prepared.sharpen_amount = std::isfinite(correction.sharpen_amount) ?
                                math::clamp(correction.sharpen_amount, 0.0f, 4.0f) :
                                0.0f;
  prepared.morph_radius = mask_correction_stage_radius(correction.morph_radius, 256);
  prepared.median_radius = mask_correction_stage_radius(correction.median_radius, 4);
  prepared.use_blur = (correction.flag & BRUSH_MASK_CORRECTION_BLUR) != 0 &&
                      prepared.blur_radius > 0;
  prepared.use_sharpen = (correction.flag & BRUSH_MASK_CORRECTION_SHARPEN) != 0 &&
                         prepared.sharpen_radius > 0 && prepared.sharpen_amount > 0.0f;
  prepared.use_morph = (correction.flag & BRUSH_MASK_CORRECTION_MORPH) != 0 &&
                       prepared.morph_radius > 0;
  prepared.use_median = (correction.flag & BRUSH_MASK_CORRECTION_MEDIAN) != 0 &&
                        prepared.median_radius > 0;

  /* Sort the pairs before deriving the reciprocal span: a corrupted file can carry min > max, and
   * `std::clamp`/`1 / (white - black)` assume the opposite order. */
  float levels_black = mask_correction_sanitize_unit(correction.levels_in_black, 0.0f);
  float levels_white = mask_correction_sanitize_unit(correction.levels_in_white, 1.0f);
  if (levels_black > levels_white) {
    std::swap(levels_black, levels_white);
  }
  const float levels_span = levels_white - levels_black;
  /* A zero span (black == white) collapses to `out_black`: there is no range left to remap. */
  prepared.levels_inv_span = levels_span > 0.0f ? 1.0f / levels_span : 0.0f;
  prepared.levels_in_black = levels_black;
  prepared.levels_inv_gamma = std::isfinite(correction.levels_gamma) &&
                                      correction.levels_gamma > 0.0f ?
                                  1.0f / correction.levels_gamma :
                                  1.0f;
  prepared.levels_out_black = mask_correction_sanitize_unit(correction.levels_out_black, 0.0f);
  prepared.levels_out_white = mask_correction_sanitize_unit(correction.levels_out_white, 1.0f);

  float smoothstep_min = mask_correction_sanitize_unit(correction.smoothstep_min, 0.0f);
  float smoothstep_max = mask_correction_sanitize_unit(correction.smoothstep_max, 1.0f);
  if (smoothstep_min > smoothstep_max) {
    std::swap(smoothstep_min, smoothstep_max);
  }
  const float smoothstep_span = smoothstep_max - smoothstep_min;
  prepared.smoothstep_min = smoothstep_min;
  prepared.smoothstep_inv_span = smoothstep_span > 0.0f ? 1.0f / smoothstep_span : 0.0f;
  prepared.smoothstep_softness = mask_correction_sanitize_unit(correction.smoothstep_softness,
                                                               1.0f);

  prepared.threshold = mask_correction_sanitize_unit(correction.threshold, 0.5f);
  prepared.posterize_steps = math::clamp(correction.posterize_steps, 1, 64);
  float clamp_min = mask_correction_sanitize_unit(correction.clamp_min, 0.0f);
  float clamp_max = mask_correction_sanitize_unit(correction.clamp_max, 1.0f);
  if (clamp_min > clamp_max) {
    std::swap(clamp_min, clamp_max);
  }
  prepared.clamp_min = clamp_min;
  prepared.clamp_max = clamp_max;

  if (prepared.use_curve) {
    prepared.curve = mask_correction_curve_prepare(correction.curve);
    prepared.use_curve = prepared.curve != nullptr;
  }
  return prepared;
}

float BKE_brush_mask_correction_apply_prepared_value(
    const BrushMaskCorrectionPrepared &prepared, float value)
{
  if (!prepared.enabled) {
    return value;
  }
  /* Fixed order: Levels, Smoothstep, Curve, Threshold, Posterize, Clamp. The pointwise
   * stack is applied as one chain so a spatial sample and a procedural one cannot drift apart. */
  if (prepared.use_levels) {
    const float t = (value - prepared.levels_in_black) * prepared.levels_inv_span;
    value = paint_layer_levels_stage_apply(
        t, prepared.levels_inv_gamma, prepared.levels_out_black, prepared.levels_out_white);
  }
  if (prepared.use_smoothstep) {
    float t = (value - prepared.smoothstep_min) * prepared.smoothstep_inv_span;
    t = math::clamp(t, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    value = math::interpolate(t, smooth, prepared.smoothstep_softness);
  }
  if (prepared.use_curve) {
    value = BKE_curvemapping_evaluateF(prepared.curve, 0, value);
  }
  if (prepared.use_threshold) {
    value = value >= prepared.threshold ? 1.0f : 0.0f;
  }
  if (prepared.use_posterize) {
    /* N levels: 0, 1/(N-1), ..., 1. A single level reads as a threshold at 1. */
    const int steps = std::max(prepared.posterize_steps, 1);
    const float denominator = float(std::max(steps - 1, 1));
    value = math::clamp(std::floor(value * float(steps)) / denominator, 0.0f, 1.0f);
  }
  if (prepared.use_clamp) {
    value = math::clamp(value, prepared.clamp_min, prepared.clamp_max);
  }
  /* The coverage contract is [0, 1]. Curve and a mis-set Clamp can leave the range, and every
   * caller multiplies a height or weight by this scalar, so clamp the result here once. */
  return math::clamp(value, 0.0f, 1.0f);
}

float BKE_brush_mask_correction_apply_value(const BrushMaskCorrection &correction, float value)
{
  return BKE_brush_mask_correction_apply_prepared_value(
      BKE_brush_mask_correction_prepare(correction), value);
}

float BKE_brush_mask_correction_apply(const Brush &brush, float value)
{
  const BrushMaskCorrection *correction = BKE_brush_mask_correction_get(brush);
  return correction != nullptr ? BKE_brush_mask_correction_apply_value(*correction, value) :
                                 value;
}

/** The one channel switch: used by the public helpers and the cache builder. */
static float mask_correction_pick_channel(const int channel,
                                          const float intensity,
                                          const float rgba[4])
{
  switch (channel) {
    case BRUSH_MASK_CORRECTION_CHANNEL_RED:
      return rgba[0];
    case BRUSH_MASK_CORRECTION_CHANNEL_GREEN:
      return rgba[1];
    case BRUSH_MASK_CORRECTION_CHANNEL_BLUE:
      return rgba[2];
    case BRUSH_MASK_CORRECTION_CHANNEL_ALPHA:
      return rgba[3];
    case BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE:
    default:
      return intensity;
  }
}

float BKE_brush_mask_correction_channel_value(const BrushMaskCorrection &correction,
                                              const float intensity,
                                              const float rgba[4])
{
  /* Clamp before the pointwise stack so a texture whose channel is outside [0, 1] (an HDR image,
   * a decoded byte buffer) enters the stack on the same contract as a coverage value. */
  return math::clamp(mask_correction_pick_channel(correction.channel, intensity, rgba),
                     0.0f,
                     1.0f);
}

float BKE_brush_mask_correction_channel_value_for_brush(const Brush &brush,
                                                       float intensity,
                                                       const float rgba[4])
{
  const BrushMaskCorrection *correction = BKE_brush_mask_correction_get(brush);
  if (correction == nullptr) {
    /* No correction: the channel selection is part of the correction, so the plain luminance the
     * sampler has always used is kept verbatim. */
    return intensity;
  }
  return BKE_brush_mask_correction_channel_value(*correction, intensity, rgba);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hash
 * \{ */

uint64_t BKE_brush_mask_correction_hash(const BrushMaskCorrection &correction)
{
  /* An off correction changes nothing, so it has no identity either. */
  if ((correction.flag & BRUSH_MASK_CORRECTION_ENABLED) == 0) {
    return 0;
  }
  /* Inactive stages still enter the hash: their parameters are what the user will see the moment
   * the stage is switched on, and the re-stamp chain has no cheaper way to notice an edit made
   * while the stage was off. */
  uint64_t hash = get_default_hash(uint64_t(uint16_t(correction.flag)),
                                   uint64_t(uint8_t(correction.channel)),
                                   uint64_t(uint8_t(correction.blur_shape)),
                                   uint64_t(uint8_t(correction.morph_mode)));
  const auto mix = [&hash](const auto value) { hash = get_default_hash(hash, value); };
  mix(correction.levels_in_black);
  mix(correction.levels_in_white);
  mix(correction.levels_gamma);
  mix(correction.levels_out_black);
  mix(correction.levels_out_white);
  mix(correction.smoothstep_min);
  mix(correction.smoothstep_max);
  mix(correction.smoothstep_softness);
  mix(correction.threshold);
  mix(uint64_t(uint32_t(correction.posterize_steps)));
  mix(correction.clamp_min);
  mix(correction.clamp_max);
  mix(correction.blur_radius);
  mix(correction.sharpen_radius);
  mix(correction.sharpen_amount);
  mix(correction.morph_radius);
  mix(correction.median_radius);
  /* The pointer identifies the curve; its edit timestamp makes a moved point invalidate the
   * spatial cache even though the pointer is unchanged. */
  mix(uint64_t(uintptr_t(correction.curve)));
  mix(uint64_t(uint32_t(correction.curve != nullptr ? correction.curve->changed_timestamp : 0)));
  return hash;
}

uint64_t BKE_brush_mask_correction_hash_for_brush(const Brush &brush)
{
  const BrushMaskCorrection *correction = BKE_brush_mask_correction_get(brush);
  return correction != nullptr ? BKE_brush_mask_correction_hash(*correction) : 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spatial filters
 * \{ */

namespace bke::mask_correction {

/**
 * One separable axis pass. \a src is read, \a dst written, both `width * height`. The radius is
 * clamped to whichever axis it runs along: a kernel wider than the image would only repeat the
 * clamped edge.
 */
static void blur_axis(Span<float> src,
                      MutableSpan<float> dst,
                      const int width,
                      const int height,
                      const int radius,
                      const bool horizontal,
                      Span<float> kernel)
{
  if (radius <= 0) {
    dst.copy_from(src);
    return;
  }
  const int line_count = horizontal ? height : width;
  const int line_length = horizontal ? width : height;
  const int taps = radius * 2 + 1;

  threading::parallel_for(IndexRange(line_count), 16, [&](const IndexRange lines) {
    for (const int line : lines) {
      const float *src_line = horizontal ? src.data() + int64_t(line) * width :
                                           src.data() + line;
      float *dst_line = horizontal ? dst.data() + int64_t(line) * width : dst.data() + line;
      const int step = horizontal ? 1 : width;
      for (int i = 0; i < line_length; i++) {
        float sum = 0.0f;
        for (int k = 0; k < taps; k++) {
          const int sample = math::clamp(i + k - radius, 0, line_length - 1);
          sum += src_line[int64_t(sample) * step] * kernel[k];
        }
        dst_line[int64_t(i) * step] = sum;
      }
    }
  });
}

void blur_apply(MutableSpan<float> values,
                const int width,
                const int height,
                const int radius,
                const bool gaussian)
{
  if (radius <= 0 || width <= 0 || height <= 0) {
    return;
  }
  Array<float> kernel(int64_t(radius) * 2 + 1);
  if (gaussian) {
    /* Radius is the visible support, i.e. about two standard deviations. */
    const float sigma = std::max(float(radius) * 0.5f, 0.5f);
    const float scale = -0.5f / (sigma * sigma);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; i++) {
      const float weight = std::exp(float(i * i) * scale);
      kernel[int64_t(i + radius)] = weight;
      sum += weight;
    }
    for (float &weight : kernel) {
      weight /= sum;
    }
  }
  else {
    const float weight = 1.0f / float(2 * radius + 1);
    for (float &value : kernel) {
      value = weight;
    }
  }

  Array<float> temp(values.size());
  blur_axis(values, temp, width, height, radius, true, kernel);
  blur_axis(temp, values, width, height, radius, false, kernel);
}

void sharpen_apply(MutableSpan<float> values,
                   const int width,
                   const int height,
                   const int radius,
                   const float amount)
{
  if (radius <= 0 || amount <= 0.0f || width <= 0 || height <= 0) {
    return;
  }
  Array<float> blurred(values.as_span());
  blur_apply(blurred, width, height, radius, true);
  threading::parallel_for(IndexRange(height), 16, [&](const IndexRange rows) {
    for (const int y : rows) {
      const int64_t row = int64_t(y) * width;
      for (int x = 0; x < width; x++) {
        const float base = values[row + x];
        values[row + x] = math::clamp(base + amount * (base - blurred[row + x]), 0.0f, 1.0f);
      }
    }
  });
}

/** One separable min/max axis pass. */
static void morph_axis(Span<float> src,
                       MutableSpan<float> dst,
                       const int width,
                       const int height,
                       const int radius,
                       const bool horizontal,
                       const bool dilate)
{
  if (radius <= 0) {
    dst.copy_from(src);
    return;
  }
  const int line_count = horizontal ? height : width;
  const int line_length = horizontal ? width : height;
  threading::parallel_for(IndexRange(line_count), 16, [&](const IndexRange lines) {
    for (const int line : lines) {
      const float *src_line = horizontal ? src.data() + int64_t(line) * width :
                                           src.data() + line;
      float *dst_line = horizontal ? dst.data() + int64_t(line) * width : dst.data() + line;
      const int step = horizontal ? 1 : width;
      for (int i = 0; i < line_length; i++) {
        float result = src_line[int64_t(i) * step];
        const int begin = std::max(i - radius, 0);
        const int end = std::min(i + radius, line_length - 1);
        for (int k = begin; k <= end; k++) {
          const float sample = src_line[int64_t(k) * step];
          result = dilate ? std::max(result, sample) : std::min(result, sample);
        }
        dst_line[int64_t(i) * step] = result;
      }
    }
  });
}

void morph_apply(MutableSpan<float> values,
                 const int width,
                 const int height,
                 const int radius,
                 const bool dilate)
{
  if (radius <= 0 || width <= 0 || height <= 0) {
    return;
  }
  Array<float> temp(values.size());
  morph_axis(values, temp, width, height, radius, true, dilate);
  morph_axis(temp, values, width, height, radius, false, dilate);
}

void median_apply(MutableSpan<float> values, const int width, const int height, const int radius)
{
  const int clamped_radius = math::clamp(radius, 1, 4);
  if (width <= 0 || height <= 0) {
    return;
  }
  constexpr int max_taps = 9 * 9;
  Array<float> temp(values.size());

  threading::parallel_for(IndexRange(height), 8, [&](const IndexRange rows) {
    float taps[max_taps];
    for (const int y : rows) {
      for (int x = 0; x < width; x++) {
        int count = 0;
        for (int dy = -clamped_radius; dy <= clamped_radius; dy++) {
          const int sy = math::clamp(y + dy, 0, height - 1);
          for (int dx = -clamped_radius; dx <= clamped_radius; dx++) {
            const int sx = math::clamp(x + dx, 0, width - 1);
            taps[count++] = values[int64_t(sy) * width + sx];
          }
        }
        /* Kernel sizes are tiny (at most 81): an insertion sort beats a heap here. */
        for (int i = 1; i < count; i++) {
          const float key = taps[i];
          int j = i - 1;
          while (j >= 0 && taps[j] > key) {
            taps[j + 1] = taps[j];
            j--;
          }
          taps[j + 1] = key;
        }
        temp[int64_t(y) * width + x] = taps[count / 2];
      }
    }
  });
  values.copy_from(temp);
}

}  // namespace bke::mask_correction

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spatial image cache
 *
 * The cache owns one single-channel buffer per (image buffer, correction) pair, already
 * channel-selected and spatially filtered. It holds no user on the #ImBuf: the key records the
 * pixel buffer pointer and #ImageRuntime::pixel_version, and an entry is rebuilt when either
 * changes. The budget and LRU are the same shape as the Mask Source registry, so a large image
 * cannot pin memory indefinitely.
 *
 * Only exactly representable cases are cached. The engine filters RGBA and de-premultiplies
 * afterwards; a single channel reproduces that for nearest sampling and, with bilinear sampling,
 * only while the de-premultiply scale is constant. `TEX_CALCALPHA`, a non-default `filtersize` and
 * a varying alpha on a non-alpha channel therefore resolve without a map and take the engine +
 * pointwise path. The key carries every input that decides the representation, so a change
 * invalidates the entry instead of serving a stale map.
 * \{ */

/** One channel-selected, spatially filtered mask image. Declared in the header as opaque. */
struct MaskCorrectionMap {
  Array<float> buffer;
  int width = 0;
  int height = 0;
};

namespace {

struct MaskCorrectionCacheKey {
  const Image *image = nullptr;
  const void *ibuf = nullptr;
  const void *pixels = nullptr;
  uint64_t pixel_version = 0;
  int width = 0;
  int height = 0;
  int channel = 0;
  bool use_alpha = false;
  uint64_t correction_hash = 0;
  int framenr = 0;
  int pass = 0;
  int tile = 0;
  int layer = 0;
  int view = 0;
  int multi_index = 0;
  int multiview_eye = 0;
  /** Identity that survives an `Image`/`ImBuf` being freed and reused at the same address. */
  uint64_t session_uid = 0;
  int alpha_mode = 0;
  int channels = 0;
  bool is_float = false;
  int tex_flag = 0;
  int tex_imaflag = 0;
  int tex_source = 0;
  int projx = 0;
  int projy = 0;
  int filtersize = 0;

  uint64_t hash() const
  {
    uint64_t hash = get_default_hash(uint64_t(uintptr_t(image)),
                                     uint64_t(uintptr_t(ibuf)),
                                     uint64_t(uintptr_t(pixels)),
                                     pixel_version,
                                     uint64_t(uint32_t(width)),
                                     uint64_t(uint32_t(height)));
    const auto mix = [&hash](const auto value) { hash = get_default_hash(hash, value); };
    mix(uint64_t(uint8_t(channel)));
    mix(uint64_t(use_alpha));
    mix(correction_hash);
    mix(uint64_t(uint32_t(framenr)));
    mix(uint64_t(uint32_t(pass)));
    mix(uint64_t(uint32_t(tile)));
    mix(uint64_t(uint32_t(layer)));
    mix(uint64_t(uint32_t(view)));
    mix(uint64_t(uint32_t(multi_index)));
    mix(uint64_t(uint8_t(multiview_eye)));
    mix(session_uid);
    mix(uint64_t(uint32_t(alpha_mode)));
    mix(uint64_t(uint32_t(channels)));
    mix(uint64_t(is_float));
    mix(uint64_t(uint32_t(tex_flag)));
    mix(uint64_t(uint32_t(tex_imaflag)));
    mix(uint64_t(uint32_t(tex_source)));
    mix(uint64_t(uint32_t(projx)));
    mix(uint64_t(uint32_t(projy)));
    mix(uint64_t(uint32_t(filtersize)));
    return hash;
  }

  friend bool operator==(const MaskCorrectionCacheKey &a, const MaskCorrectionCacheKey &b)
  {
    return a.image == b.image && a.ibuf == b.ibuf && a.pixels == b.pixels &&
           a.pixel_version == b.pixel_version && a.width == b.width && a.height == b.height &&
           a.channel == b.channel && a.use_alpha == b.use_alpha &&
           a.correction_hash == b.correction_hash &&
           a.framenr == b.framenr && a.pass == b.pass && a.tile == b.tile && a.layer == b.layer &&
           a.view == b.view && a.multi_index == b.multi_index &&
           a.multiview_eye == b.multiview_eye && a.session_uid == b.session_uid &&
           a.alpha_mode == b.alpha_mode && a.channels == b.channels &&
           a.is_float == b.is_float && a.tex_flag == b.tex_flag &&
           a.tex_imaflag == b.tex_imaflag && a.tex_source == b.tex_source &&
           a.projx == b.projx && a.projy == b.projy && a.filtersize == b.filtersize;
  }
};

struct MaskCorrectionCacheEntry {
  MaskCorrectionCacheKey key;
  std::shared_ptr<const MaskCorrectionMap> map;
  int64_t bytes = 0;
  uint64_t last_use = 0;
};

/* One 4K float map fits (4096 * 4096 * sizeof(float) == 64 MiB); the entry cap and the budget are
 * independent limits, so pin their relationship down rather than trusting two magic numbers. */
constexpr int64_t mask_correction_cache_budget_bytes = int64_t(64) * 1024 * 1024;
constexpr int mask_correction_cache_max_entries = 8;
constexpr int64_t mask_correction_cache_max_pixels = int64_t(4096) * 4096;
static_assert(mask_correction_cache_budget_bytes >=
                  sizeof(float) * mask_correction_cache_max_pixels,
              "The budget must hold at least one map at the pixel cap, or a full-size mask can "
              "never be cached.");
static_assert(mask_correction_cache_max_entries >= 1, "At least one cache entry must fit.");

std::mutex mask_correction_cache_mutex;
Vector<MaskCorrectionCacheEntry> mask_correction_cache;
uint64_t mask_correction_cache_use_counter = 0;
int64_t mask_correction_cache_bytes_used = 0;
BrushMaskCorrectionCacheStats mask_correction_cache_stats_data;

void mask_correction_cache_evict_oldest()
{
  int oldest = 0;
  for (const int i : mask_correction_cache.index_range()) {
    if (mask_correction_cache[i].last_use < mask_correction_cache[oldest].last_use) {
      oldest = i;
    }
  }
  mask_correction_cache_bytes_used -= mask_correction_cache[oldest].bytes;
  mask_correction_cache.remove(oldest);
  mask_correction_cache_stats_data.evictions++;
}

std::shared_ptr<const MaskCorrectionMap> mask_correction_cache_lookup(
    const MaskCorrectionCacheKey &key)
{
  std::lock_guard lock(mask_correction_cache_mutex);
  const uint64_t stamp = ++mask_correction_cache_use_counter;
  for (MaskCorrectionCacheEntry &entry : mask_correction_cache) {
    if (entry.key == key) {
      entry.last_use = stamp;
      mask_correction_cache_stats_data.hits++;
      return entry.map;
    }
  }
  return nullptr;
}

void mask_correction_cache_insert(MaskCorrectionCacheKey key,
                                  std::shared_ptr<const MaskCorrectionMap> map)
{
  const int64_t new_bytes = int64_t(map->width) * map->height * sizeof(float);
  std::lock_guard lock(mask_correction_cache_mutex);
  const uint64_t stamp = ++mask_correction_cache_use_counter;
  for (MaskCorrectionCacheEntry &entry : mask_correction_cache) {
    if (entry.key == key) {
      mask_correction_cache_bytes_used += new_bytes - entry.bytes;
      entry.map = std::move(map);
      entry.bytes = new_bytes;
      entry.last_use = stamp;
      return;
    }
  }
  while ((mask_correction_cache.size() >= mask_correction_cache_max_entries ||
          mask_correction_cache_bytes_used + new_bytes > mask_correction_cache_budget_bytes) &&
         !mask_correction_cache.is_empty())
  {
    mask_correction_cache_evict_oldest();
  }
  if (mask_correction_cache.size() >= mask_correction_cache_max_entries ||
      mask_correction_cache_bytes_used + new_bytes > mask_correction_cache_budget_bytes)
  {
    return;
  }
  mask_correction_cache_bytes_used += new_bytes;
  mask_correction_cache.append(MaskCorrectionCacheEntry{
      std::move(key), std::move(map), new_bytes, stamp});
}

}  // namespace

/** The engine's `ibuf_get_color()`: byte buffers are pre-multiplied, float buffers are straight. */
static void mask_correction_ibuf_get_color(const ImBuf &ibuf, const int64_t ofs, float r_col[4])
{
  if (const float *fp = ibuf.float_data()) {
    if (ibuf.channels == 4) {
      copy_v4_v4(r_col, fp + 4 * ofs);
    }
    else if (ibuf.channels == 3) {
      copy_v3_v3(r_col, fp + 3 * ofs);
      r_col[3] = 1.0f;
    }
    else {
      r_col[0] = r_col[1] = r_col[2] = r_col[3] = fp[ofs];
    }
    return;
  }
  const uchar *rect = ibuf.byte_data();
  if (rect == nullptr) {
    r_col[0] = r_col[1] = r_col[2] = r_col[3] = 0.0f;
    return;
  }
  const uchar *p = rect + 4 * ofs;
  r_col[0] = float(p[0]) * (1.0f / 255.0f);
  r_col[1] = float(p[1]) * (1.0f / 255.0f);
  r_col[2] = float(p[2]) * (1.0f / 255.0f);
  r_col[3] = float(p[3]) * (1.0f / 255.0f);
  /* Bytes are stored straight, but the render pipeline expects pre-multiplied. */
  r_col[0] *= r_col[3];
  r_col[1] *= r_col[3];
  r_col[2] *= r_col[3];
}

/**
 * Fill \a r_buffer with the chosen channel of \a ibuf, one float per texel, applying the same
 * alpha pipeline `imagewrap()` applies after `ibuf_get_color()`: pre-multiplied bytes, the
 * `TEX_USEALPHA`/`TEX_CALCALPHA` alpha rule, `TEX_NEGALPHA`, and the matching de-premultiply.
 *
 * Byte buffers are not colorspace-decoded, which matches `RE_texture_evaluate()` for a Mask slot.
 *
 * The pipeline runs per texel, so the bilinear fetch filters already-converted values. For a
 * non-alpha channel that is exact only while the de-premultiply scale is constant: the engine
 * filters RGBA first and de-premultiplies after filtering. \a track_alpha_range reports whether
 * the effective alpha varies, which is what lets the builder refuse a case it cannot reproduce
 * instead of quietly approximating it.
 */
static void mask_correction_fill_channel(const ImBuf &ibuf,
                                         const int channel,
                                         const bool use_alpha,
                                         const bool calc_alpha,
                                         const bool neg_alpha,
                                         const bool track_alpha_range,
                                         MutableSpan<float> r_buffer,
                                         bool &r_alpha_variable)
{
  const int width = ibuf.x;
  const int height = ibuf.y;
  /* `imagewrap()` only honours the stored alpha when the texture uses it and the image does not
   * ignore it, and `TEX_CALCALPHA` takes over from there. */
  const bool take_alpha = use_alpha && !calc_alpha;

  const auto convert = [&](const int64_t ofs, float *r_alpha) -> float {
    float rgba[4];
    mask_correction_ibuf_get_color(ibuf, ofs, rgba);
    float alpha;
    if (take_alpha) {
      alpha = rgba[3];
    }
    else if (calc_alpha) {
      alpha = std::max({rgba[0], rgba[1], rgba[2]});
    }
    else {
      alpha = 1.0f;
    }
    if (neg_alpha) {
      alpha = 1.0f - alpha;
    }
    if (alpha != 1.0f && alpha > 1e-4f && !calc_alpha) {
      const float inv = 1.0f / alpha;
      rgba[0] *= inv;
      rgba[1] *= inv;
      rgba[2] *= inv;
    }
    rgba[3] = alpha;
    if (r_alpha != nullptr) {
      *r_alpha = alpha;
    }
    return mask_correction_pick_channel(
        channel, IMB_colormanagement_get_luminance(rgba), rgba);
  };

  if (ibuf.float_data() != nullptr || ibuf.byte_data() != nullptr) {
    Array<float> row_min;
    Array<float> row_max;
    if (track_alpha_range) {
      row_min = Array<float>(height, 1.0f);
      row_max = Array<float>(height, 0.0f);
    }
    threading::parallel_for(IndexRange(height), 64, [&](const IndexRange rows) {
      for (const int y : rows) {
        float *out = r_buffer.data() + int64_t(y) * width;
        float alpha_min = 1.0f;
        float alpha_max = 0.0f;
        for (const int x : IndexRange(width)) {
          float alpha = 1.0f;
          out[x] = math::clamp(
              convert(int64_t(y) * width + x, track_alpha_range ? &alpha : nullptr),
              0.0f,
              1.0f);
          if (track_alpha_range) {
            alpha_min = std::min(alpha_min, alpha);
            alpha_max = std::max(alpha_max, alpha);
          }
        }
        if (track_alpha_range) {
          row_min[y] = alpha_min;
          row_max[y] = alpha_max;
        }
      }
    });
    if (track_alpha_range) {
      float alpha_min = 1.0f;
      float alpha_max = 0.0f;
      for (const int y : IndexRange(height)) {
        alpha_min = std::min(alpha_min, row_min[y]);
        alpha_max = std::max(alpha_max, row_max[y]);
      }
      r_alpha_variable = alpha_max - alpha_min > 1e-6f;
    }
  }
}

/** Wrap one texel index for the texture's repeat extension mode. Mirroring is already folded
 * into the `u` / `v` coordinates (see #mask_correction_sample_map), so it must not run here. */
static int mask_correction_resolve_index(int index, const int size)
{
  if (size <= 1) {
    return 0;
  }
  int wrapped = index % size;
  if (wrapped < 0) {
    wrapped += size;
  }
  return wrapped;
}

/**
 * Bilinear fetch of the corrected map, honoring the texture's extension mode. The 90-degree image
 * rotation and the repeat/mirror/crop placement are applied here exactly as `do_2d_mapping()`
 * applies them before the engine samples the image.
 *
 * \note This is a copy of the placement rules in
 * `render/intern/texture_procedural.cc` (`do_2d_mapping`) and the texel/extension handling in
 * `render/intern/texture_image.cc` (`imagewrap`). It cannot call those helpers (render-internal,
 * and BKE must not depend on the render module), so every change to the originals has to be
 * mirrored here. The parity tests `spatial_mapping_parity_*` in
 * `paint_curve_patch_mask_test.cc` compare the two paths on the same #MTex and are the
 * synchronization point.
 */
static float mask_correction_sample_map(const MaskCorrectionMap &map,
                                        const BrushMaskCorrectionTexPlacement &tex,
                                        const float in_u,
                                        const float in_v)
{
  const int width = map.width;
  const int height = map.height;
  if (width <= 0 || height <= 0) {
    return 0.0f;
  }

  /* Reproduce the placement `do_2d_mapping()` applies to an image texture before the image sampler
   * sees it: Tiling and crop are part of the texture's own mapping, so a filtered mask has to
   * repeat and crop the same way the engine sample does. */
  float u = in_u;
  float v = in_v;
  if (tex.extend == TEX_REPEAT) {
    if (tex.xrepeat > 1) {
      const float orig = u * float(tex.xrepeat);
      u = orig;
      if (u > 1.0f) {
        u -= int(u);
      }
      else if (u < 0.0f) {
        u += 1 - int(u);
      }
      if ((tex.flag & TEX_REPEAT_XMIR) && (int(floorf(orig)) & 1)) {
        u = 1.0f - u;
      }
    }
    if (tex.yrepeat > 1) {
      const float orig = v * float(tex.yrepeat);
      v = orig;
      if (v > 1.0f) {
        v -= int(v);
      }
      else if (v < 0.0f) {
        v += 1 - int(v);
      }
      if ((tex.flag & TEX_REPEAT_YMIR) && (int(floorf(orig)) & 1)) {
        v = 1.0f - v;
      }
    }
  }
  if (tex.cropxmin != 0.0f || tex.cropxmax != 1.0f) {
    u = tex.cropxmin + u * (tex.cropxmax - tex.cropxmin);
  }
  if (tex.cropymin != 0.0f || tex.cropymax != 1.0f) {
    v = tex.cropymin + v * (tex.cropymax - tex.cropymin);
  }

  /* The engine applies the 90-degree image rotation inside `imagewrap()`, i.e. after
   * `do_2d_mapping()` has already repeated and cropped the coordinates. */
  if (tex.imaflag & TEX_IMAROT) {
    std::swap(u, v);
  }

  /* The engine works in integer texel coordinates after the rotation and folds the extension mode
   * into them, so the clip test and the nearest fetch below use the same indices. */
  const int ix = int(std::floor(u * float(width)));
  const int iy = int(std::floor(v * float(height)));
  if (ELEM(tex.extend, TEX_CLIP, TEX_CLIPCUBE) &&
      (ix < 0 || iy < 0 || ix >= width || iy >= height))
  {
    return 0.0f;
  }

  const bool repeat = tex.extend == TEX_REPEAT;
  const bool clamp_mode = !repeat;

  const auto wrap_x = [&](int index) -> int {
    return clamp_mode ? math::clamp(index, 0, width - 1) : mask_correction_resolve_index(index, width);
  };
  const auto wrap_y = [&](int index) -> int {
    return clamp_mode ? math::clamp(index, 0, height - 1) :
                        mask_correction_resolve_index(index, height);
  };

  /* No filtering in the texture: nearest texel, matching `imagewrap()`'s `ibuf_get_color()` path. */
  if ((tex.imaflag & TEX_INTERPOL) == 0) {
    return map.buffer[int64_t(wrap_y(iy)) * width + wrap_x(ix)];
  }

  /* Bilinear with texel centers on half-integers, matching the engine's `boxsample()` at the
   * default `filtersize`. A wider `filtersize` (or a bilinear clip) never reaches this function:
   * the sampler declines to build a map for it, see #BrushMaskCorrectionSampler::sample_image. */
  const float px = u * float(width) - 0.5f;
  const float py = v * float(height) - 0.5f;
  const int x0 = int(std::floor(px));
  const int y0 = int(std::floor(py));
  const float fx = px - float(x0);
  const float fy = py - float(y0);
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const int rx0 = wrap_x(x0);
  const int rx1 = wrap_x(x1);
  const int ry0 = wrap_y(y0);
  const int ry1 = wrap_y(y1);

  const int64_t row0 = int64_t(ry0) * width;
  const int64_t row1 = int64_t(ry1) * width;
  const float bottom = math::interpolate(map.buffer[row0 + rx0], map.buffer[row0 + rx1], fx);
  const float top = math::interpolate(map.buffer[row1 + rx0], map.buffer[row1 + rx1], fx);
  return math::interpolate(bottom, top, fy);
}

/** Build the corrected map for \a ibuf, or null when it cannot be represented exactly. */
static std::shared_ptr<const MaskCorrectionMap> mask_correction_map_build(
    const ImBuf &ibuf,
    const BrushMaskCorrection &correction,
    const BrushMaskCorrectionPrepared &prepared,
    const bool use_alpha,
    const bool calc_alpha,
    const bool neg_alpha,
    const bool interpolate)
{
  const int width = ibuf.x;
  const int height = ibuf.y;
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  if (int64_t(width) * int64_t(height) > mask_correction_cache_max_pixels) {
    return nullptr;
  }
  if (ibuf.float_data() == nullptr && ibuf.byte_data() == nullptr) {
    return nullptr;
  }

  auto map = std::make_shared<MaskCorrectionMap>();
  map->width = width;
  map->height = height;
  map->buffer = Array<float>(int64_t(width) * int64_t(height));
  /* The engine filters RGBA and de-premultiplies after filtering. That commutes with a
   * single-channel reduction only while the de-premultiply scale is constant, so a varying alpha
   * on a non-alpha channel is refused instead of approximated. Nearest sampling has a single
   * texel, where the order cannot matter. */
  const bool track_alpha_range = interpolate && use_alpha && !calc_alpha &&
                                 correction.channel != BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  bool alpha_variable = false;
  mask_correction_fill_channel(ibuf,
                               correction.channel,
                               use_alpha,
                               calc_alpha,
                               neg_alpha,
                               track_alpha_range,
                               map->buffer,
                               alpha_variable);
  if (track_alpha_range && alpha_variable) {
    return nullptr;
  }

  /* A kernel wider than the image would only repeat the clamped edge, and an absurd radius from a
   * damaged file must not allocate an unbounded kernel. */
  const int max_radius = std::max(std::min(width, height) - 1, 1);

  /* Spatial order is fixed: Blur, Sharpen, Erode/Dilate, Median. The radii were quantized once in
   * #BKE_brush_mask_correction_prepare so activity and execution cannot disagree. */
  if (prepared.use_blur) {
    bke::mask_correction::blur_apply(map->buffer,
                                width,
                                height,
                                std::min(prepared.blur_radius, max_radius),
                                correction.blur_shape ==
                                    BRUSH_MASK_CORRECTION_BLUR_GAUSSIAN);
  }
  if (prepared.use_sharpen) {
    bke::mask_correction::sharpen_apply(map->buffer,
                                   width,
                                   height,
                                   std::min(prepared.sharpen_radius, max_radius),
                                   prepared.sharpen_amount);
  }
  if (prepared.use_morph) {
    bke::mask_correction::morph_apply(map->buffer,
                                 width,
                                 height,
                                 std::min(prepared.morph_radius, max_radius),
                                 correction.morph_mode == BRUSH_MASK_CORRECTION_MORPH_DILATE);
  }
  if (prepared.use_median) {
    bke::mask_correction::median_apply(
        map->buffer, width, height, std::min(prepared.median_radius, max_radius));
  }
  return map;
}

BrushMaskCorrectionSampler BrushMaskCorrectionSampler::resolve(const Brush &brush,
                                                               const MTex &mtex,
                                                               ImagePool *pool)
{
  BrushMaskCorrectionSampler sampler;
  const BrushMaskCorrection *correction = BKE_brush_mask_correction_get(brush);
  if (correction == nullptr) {
    return sampler;
  }
  sampler.prepared_ = BKE_brush_mask_correction_prepare(*correction);
  sampler.hash_ = BKE_brush_mask_correction_hash(*correction);
  /* Snapshot the curve table: the RNA editor rebuilds it in place, and the worker threads of a
   * stroke must not read a table that another thread can free and reallocate. The copy is one
   * deep copy per resolve, never per sample. */
  if (sampler.prepared_.use_curve) {
    sampler.curve_snapshot_ = std::shared_ptr<CurveMapping>(
        BKE_curvemapping_copy(sampler.prepared_.curve), BKE_curvemapping_free);
    sampler.prepared_.curve = sampler.curve_snapshot_.get();
  }
  if (!sampler.prepared_.use_spatial()) {
    return sampler;
  }
  /* Procedural textures and node trees have no pixel buffer to filter; 3D mapping samples object
   * space, where a 2D filter has no meaning. Both keep the engine + pointwise path. */
  const Tex *tex = mtex.tex;
  if (tex == nullptr || tex->type != TEX_IMAGE || tex->ima == nullptr ||
      tex->nodetree != nullptr || mtex.brush_map_mode == MTEX_MAP_MODE_3D ||
      mtex.mapping != MTEX_FLAT)
  {
    /* The mapping condition keeps the cache on the flat image projection it can reproduce. Tube,
     * sphere and cube mappings are rare for a Mask and would need their own sampler; the engine
     * path still runs the pointwise stack. */
    return sampler;
  }
  /* The engine picks the vector components from `projx`/`projy`; this sampler assumes X/Y, so any
   * other projection must fall back rather than read the wrong axes. */
  if (mtex.projx != 1 || mtex.projy != 2) {
    return sampler;
  }
  /* The engine applies #BRICONTRGB to every image sample after `imagewrap()`. This map stores the
   * channel before that tone transform, so a non-neutral Brightness / Contrast / Saturation /
   * R/G/B factor would silently diverge from the engine. Neutral values keep the spatial path;
   * anything else falls back to the engine plus pointwise. */
  if (tex->bright != 1.0f || tex->contrast != 1.0f || tex->saturation != 1.0f ||
      tex->rfac != 1.0f || tex->gfac != 1.0f || tex->bfac != 1.0f)
  {
    return sampler;
  }
  /* Tiled (UDIM) sources resolve their tile from the sampled position inside the engine; this
   * cache is built for one tile, so the two cannot agree without duplicating that lookup. Checker
   * is a procedural pattern over the tile, not an image filter. Both stay on the engine path. */
  if (tex->ima->source == IMA_SRC_TILED || tex->extend == TEX_CHECKER) {
    return sampler;
  }
  const bool interpolate = (tex->imaflag & TEX_INTERPOL) != 0;
  /* The engine's box filter is as wide as `filtersize`; the single-channel map cannot run it
   * before the channel reduction, so only the default width is representable. */
  if (interpolate && tex->filtersize != 1.0f) {
    return sampler;
  }
  /* A clipped box that straddles the border is scaled by the engine after filtering, which the
   * single-channel map does not reproduce near the edge. Nearest sampling clips on the integer
   * texel and stays exact. */
  if (interpolate && ELEM(tex->extend, TEX_CLIP, TEX_CLIPCUBE)) {
    return sampler;
  }
  Image *image = tex->ima;
  ImBuf *ibuf = BKE_image_pool_acquire_ibuf(
      image, const_cast<ImageUser *>(&tex->iuser), pool);
  if (ibuf == nullptr) {
    return sampler;
  }

  const bool calc_alpha = (tex->imaflag & TEX_CALCALPHA) != 0;
  const bool use_alpha = (tex->imaflag & TEX_USEALPHA) &&
                         image->alpha_mode != IMA_ALPHA_IGNORE;
  const bool neg_alpha = (tex->flag & TEX_NEGALPHA) != 0;
  const bool is_float = ibuf->float_data() != nullptr;

  /* `TEX_CALCALPHA` derives alpha from the filtered RGB, which one channel cannot reproduce. */
  if (calc_alpha) {
    BKE_image_pool_release_ibuf(image, ibuf, pool);
    return sampler;
  }

  MaskCorrectionCacheKey key;
  key.image = image;
  key.ibuf = ibuf;
  key.pixels = is_float ? reinterpret_cast<const void *>(ibuf->float_data()) :
                          reinterpret_cast<const void *>(ibuf->byte_data());
  key.pixel_version = image->runtime != nullptr ?
                          image->runtime->pixel_version.load(std::memory_order_relaxed) :
                          0;
  key.width = ibuf->x;
  key.height = ibuf->y;
  key.channel = correction->channel;
  key.use_alpha = use_alpha;
  key.correction_hash = sampler.hash_;
  key.framenr = tex->iuser.framenr;
  key.pass = tex->iuser.pass;
  key.tile = tex->iuser.tile;
  key.layer = tex->iuser.layer;
  key.view = tex->iuser.view;
  key.multi_index = tex->iuser.multi_index;
  key.multiview_eye = tex->iuser.multiview_eye;
  key.session_uid = image->id.session_uid;
  key.alpha_mode = image->alpha_mode;
  key.channels = ibuf->channels;
  key.is_float = is_float;
  key.tex_flag = tex->flag;
  key.tex_imaflag = tex->imaflag;
  key.tex_source = image->source;
  key.projx = mtex.projx;
  key.projy = mtex.projy;
  key.filtersize = tex->filtersize;

  std::shared_ptr<const MaskCorrectionMap> map = mask_correction_cache_lookup(key);
  if (map == nullptr) {
    map = mask_correction_map_build(
        *ibuf, *correction, sampler.prepared_, use_alpha, calc_alpha, neg_alpha, interpolate);
    if (map != nullptr) {
      mask_correction_cache_insert(key, map);
      std::lock_guard lock(mask_correction_cache_mutex);
      mask_correction_cache_stats_data.builds++;
    }
  }
  BKE_image_pool_release_ibuf(image, ibuf, pool);
  if (map == nullptr) {
    return sampler;
  }
  sampler.placement_.extend = tex->extend;
  sampler.placement_.flag = tex->flag;
  sampler.placement_.imaflag = tex->imaflag;
  sampler.placement_.xrepeat = tex->xrepeat;
  sampler.placement_.yrepeat = tex->yrepeat;
  sampler.placement_.cropxmin = tex->cropxmin;
  sampler.placement_.cropxmax = tex->cropxmax;
  sampler.placement_.cropymin = tex->cropymin;
  sampler.placement_.cropymax = tex->cropymax;
  sampler.size_[0] = mtex.size[0];
  sampler.size_[1] = mtex.size[1];
  sampler.ofs_[0] = mtex.ofs[0];
  sampler.ofs_[1] = mtex.ofs[1];
  sampler.map_ = map;
  return sampler;
}

std::optional<float> BrushMaskCorrectionSampler::sample_image(const float x, const float y) const
{
  if (map_ == nullptr) {
    return std::nullopt;
  }
  /* The texture engine applies size/offset to the coordinate before sampling; mirror it here so
   * the cached map and the raw path see the same texel. */
  const float tex_x = size_[0] * (x + ofs_[0]);
  const float tex_y = size_[1] * (y + ofs_[1]);
  const float u = (tex_x + 1.0f) * 0.5f;
  const float v = (tex_y + 1.0f) * 0.5f;
  return mask_correction_sample_map(*map_, placement_, u, v);
}

float BrushMaskCorrectionSampler::apply_pointwise(const float value) const
{
  return BKE_brush_mask_correction_apply_prepared_value(prepared_, value);
}

std::optional<float> BKE_brush_mask_correction_sample_image(const Brush &brush,
                                                            const MTex &mtex,
                                                            const float x,
                                                            const float y,
                                                            ImagePool *pool)
{
  return BrushMaskCorrectionSampler::resolve(brush, mtex, pool).sample_image(x, y);
}

BrushMaskCorrectionCacheStats BKE_brush_mask_correction_cache_stats()
{
  std::lock_guard lock(mask_correction_cache_mutex);
  BrushMaskCorrectionCacheStats stats = mask_correction_cache_stats_data;
  stats.entries = uint64_t(mask_correction_cache.size());
  stats.bytes = uint64_t(std::max(mask_correction_cache_bytes_used, int64_t(0)));
  return stats;
}

void BKE_brush_mask_correction_cache_clear()
{
  std::lock_guard lock(mask_correction_cache_mutex);
  /* `clear` alone would leave the heap buffer for the static teardown, reported as a leak by the
   * guarded allocator. Shrink while the exit path still runs. */
  mask_correction_cache.clear_and_shrink();
  mask_correction_cache_bytes_used = 0;
  mask_correction_cache_stats_data.builds = 0;
  mask_correction_cache_stats_data.hits = 0;
  mask_correction_cache_stats_data.evictions = 0;
  mask_correction_cache_stats_data.entries = 0;
  mask_correction_cache_stats_data.bytes = 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend I/O
 * \{ */

void BKE_brush_mask_correction_blend_write(BlendWriter *writer, const Brush &brush)
{
  if (brush.mask_correction == nullptr) {
    return;
  }
  writer->write_struct(brush.mask_correction);
  if (brush.mask_correction->curve != nullptr) {
    BKE_curvemapping_blend_write(writer, brush.mask_correction->curve);
  }
}

void BKE_brush_mask_correction_blend_read(BlendDataReader *reader, Brush &brush)
{
  BLO_read_struct(reader, BrushMaskCorrection, &brush.mask_correction);
  if (brush.mask_correction == nullptr) {
    return;
  }
  BLO_read_struct(reader, CurveMapping, &brush.mask_correction->curve);
  if (brush.mask_correction->curve != nullptr) {
    BKE_curvemapping_blend_read(reader, brush.mask_correction->curve);
  }
  else {
    /* A file written before the curve existed, or one whose curve was never built: the UI expects
     * a mapping to draw and #CURVE reads it, so give it the identity. */
    brush.mask_correction->curve = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  }
}

/** \} */

}  // namespace blender
