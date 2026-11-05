/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The image kernels of the CPU-computed effects: a Blur's box passes, an Erode / Dilate's
 * sliding-window extremum, the 3x3 Sobel and Emboss convolutions, a Glow's blurred luminance, a
 * Bevel's distance transform and a Color Match's grade. They read and write a whole
 * #ImBuf and know nothing about the composite, the description or the cache; the CPU composite and
 * the bake both reach them through #paint_layer_effect_kernel_apply.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "BKE_paint_perf_debug.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

std::shared_ptr<ImBuf> paint_layer_anchor_slice_buffer_clone(const ImBuf &source)
{
  ImBuf *copy = IMB_dupImBuf(&source);
  if (copy == nullptr) {
    return {};
  }
  return std::shared_ptr<ImBuf>(copy, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

/**
 * One horizontal box pass over the RGB of \a buffer, edges clamped, alpha untouched. Rows run in
 * parallel; each keeps a scratch copy of itself, since the sliding window reads behind the texel
 * the pass has already written.
 */
static void paint_layer_effect_blur_pass_h(ImBuf &buffer, const int radius)
{
  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    Vector<float> row(int64_t(width) * 4);
    for (const int y : range) {
      float *dst = pixels + int64_t(y) * width * 4;
      memcpy(row.data(), dst, size_t(width) * sizeof(float[4]));
      float sum[3] = {0.0f, 0.0f, 0.0f};
      int lo = 0;
      int hi = -1;
      for (const int x : IndexRange(width)) {
        const int new_hi = std::min(x + radius, width - 1);
        for (int k = hi + 1; k <= new_hi; k++) {
          sum[0] += row[int64_t(k) * 4 + 0];
          sum[1] += row[int64_t(k) * 4 + 1];
          sum[2] += row[int64_t(k) * 4 + 2];
        }
        hi = new_hi;
        const int new_lo = std::max(x - radius, 0);
        for (int k = lo; k < new_lo; k++) {
          sum[0] -= row[int64_t(k) * 4 + 0];
          sum[1] -= row[int64_t(k) * 4 + 1];
          sum[2] -= row[int64_t(k) * 4 + 2];
        }
        lo = new_lo;
        const float inv = 1.0f / float(hi - lo + 1);
        dst[int64_t(x) * 4 + 0] = sum[0] * inv;
        dst[int64_t(x) * 4 + 1] = sum[1] * inv;
        dst[int64_t(x) * 4 + 2] = sum[2] * inv;
      }
    }
  });
}

/**
 * One vertical box pass over the RGB of \a buffer, edges clamped, alpha untouched.
 *
 * The pass walks the buffer row by row with one sliding sum per column, instead of walking it
 * column by column: a column walk of a 4K float buffer strided by a row is a cache miss per texel,
 * where the row walk reads and writes each line once. The window adds rows below the one being
 * written but subtracts rows above it, which the pass has already overwritten, so a small ring
 * keeps the originals of those. The sum is sequential in y, so the pass stays single-threaded; the
 * horizontal pass above carries the parallelism.
 */
static void paint_layer_effect_blur_pass_v(ImBuf &buffer, const int radius)
{
  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  Vector<float> sums(int64_t(width) * 3, 0.0f);
  /* The window drops row `k` once `y - radius > k`, but row `k` was overwritten when the pass wrote
   * it. The originals of the rows still to be dropped are kept in a ring: a row is saved just before
   * it is written and read back `radius + 1` rows later, so `radius + 2` slots never collide. */
  const int ring_num = radius + 2;
  Vector<float> ring(int64_t(ring_num) * width * 3);
  int lo = 0;
  int hi = -1;
  for (const int y : IndexRange(height)) {
    const int new_hi = std::min(y + radius, height - 1);
    for (int k = hi + 1; k <= new_hi; k++) {
      const float *src = pixels + int64_t(k) * width * 4;
      for (const int x : IndexRange(width)) {
        float *s = sums.data() + int64_t(x) * 3;
        s[0] += src[int64_t(x) * 4 + 0];
        s[1] += src[int64_t(x) * 4 + 1];
        s[2] += src[int64_t(x) * 4 + 2];
      }
    }
    hi = new_hi;
    const int new_lo = std::max(y - radius, 0);
    for (int k = lo; k < new_lo; k++) {
      const float *original = ring.data() + int64_t(k % ring_num) * width * 3;
      for (const int x : IndexRange(width)) {
        float *s = sums.data() + int64_t(x) * 3;
        s[0] -= original[int64_t(x) * 3 + 0];
        s[1] -= original[int64_t(x) * 3 + 1];
        s[2] -= original[int64_t(x) * 3 + 2];
      }
    }
    lo = new_lo;
    const float inv = 1.0f / float(hi - lo + 1);
    float *dst = pixels + int64_t(y) * width * 4;
    float *saved = ring.data() + int64_t(y % ring_num) * width * 3;
    for (const int x : IndexRange(width)) {
      saved[int64_t(x) * 3 + 0] = dst[int64_t(x) * 4 + 0];
      saved[int64_t(x) * 3 + 1] = dst[int64_t(x) * 4 + 1];
      saved[int64_t(x) * 3 + 2] = dst[int64_t(x) * 4 + 2];
    }
    for (const int x : IndexRange(width)) {
      const float *s = sums.data() + int64_t(x) * 3;
      dst[int64_t(x) * 4 + 0] = s[0] * inv;
      dst[int64_t(x) * 4 + 1] = s[1] * inv;
      dst[int64_t(x) * 4 + 2] = s[2] * inv;
    }
  }
}

/** A box blur of \a radius over the RGB of \a buffer, one pass per axis. */
static void paint_layer_effect_blur_box(ImBuf &buffer, const int radius)
{
  const int r = std::clamp(radius, 1, std::max(buffer.x, buffer.y));
  paint_layer_effect_blur_pass_h(buffer, r);
  paint_layer_effect_blur_pass_v(buffer, r);
}

/** A private blurred copy of \a source: the detail the unsharp-style kernels read against. */
static bool paint_layer_effect_blur_copy(const ImBuf &source,
                                         const int radius,
                                         std::shared_ptr<ImBuf> &r_blurred)
{
  r_blurred = paint_layer_anchor_slice_buffer_clone(source);
  if (r_blurred == nullptr) {
    return false;
  }
  paint_layer_effect_blur_box(*r_blurred, radius);
  return true;
}

/**
 * Apply \a op against a blurred copy of \a buffer: the shape every unsharp-style kernel shares.
 * \a op reads the original and the blurred texel and returns the new value. A negative result is
 * clipped below -- a negative linear colour would break the blends and the encoding after it --
 * while above the map may be HDR.
 */
template<typename Op>
static void paint_layer_effect_unsharp_apply(ImBuf &buffer, const int radius, Op op)
{
  std::shared_ptr<ImBuf> blurred;
  if (!paint_layer_effect_blur_copy(buffer, radius, blurred)) {
    return;
  }
  const float *detail = blurred->float_buffer.data;
  float *pixels = buffer.float_data_for_write();
  const int width = buffer.x;
  const int height = buffer.y;
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      float *dst = pixels + int64_t(y) * width * 4;
      const float *src = detail + int64_t(y) * width * 4;
      for (const int x : IndexRange(width)) {
        for (const int c : IndexRange(3)) {
          const int64_t i = int64_t(x) * 4 + c;
          dst[i] = std::max(op(dst[i], src[i]), 0.0f);
        }
      }
    }
  });
}

/**
 * Run \a op over the RGB of \a buffer, one texel at a time, in place: rows run in parallel, and
 * \a op receives the texel's column, row and pointer and writes its three colour channels. Alpha is
 * untouched. The one frame the per-texel effects write their result through.
 */
template<typename Op> static void paint_layer_effect_map_rgb(ImBuf &buffer, Op op)
{
  const int width = buffer.x;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(buffer.y), 64, [&](const IndexRange range) {
    for (const int y : range) {
      float *row = pixels + int64_t(y) * width * 4;
      for (const int x : IndexRange(width)) {
        op(x, y, row + int64_t(x) * 4);
      }
    }
  });
}

/** The scene-linear luminance of each texel of \a buffer written to all three colour channels. */
static void paint_layer_effect_to_luminance(ImBuf &buffer)
{
  paint_layer_effect_map_rgb(buffer, [](const int /*x*/, const int /*y*/, float *texel) {
    const float luminance = IMB_colormanagement_get_luminance(texel);
    texel[0] = luminance;
    texel[1] = luminance;
    texel[2] = luminance;
  });
}

/**
 * One channel's sliding-window extremum along an axis, the min/max counterpart of the blur's
 * running sum: indices enter in order, a stronger one drops the weaker off the back, and the front
 * holds the window's answer once the left edge has passed it. Each texel is pushed and dropped at
 * most once, so a pass costs one step per texel.
 */
class PaintLayerExtremumWindow {
 public:
  PaintLayerExtremumWindow(const bool maximum) : maximum_(maximum) {}

  void push(const int index, const float value)
  {
    while (entries_.size() > head_ &&
           (maximum_ ? entries_.last().value <= value : entries_.last().value >= value))
    {
      entries_.pop_last();
    }
    entries_.append({index, value});
  }

  /**
   * The window's left edge moved past some texels: the front goes with it. A pass shifts the window
   * by one per texel, so one entry leaves per call; the loop states the invariant without relying
   * on the caller's step size.
   */
  void drop_before(const int index)
  {
    while (head_ < entries_.size() && entries_[head_].index < index) {
      head_++;
    }
  }

  float front() const
  {
    BLI_assert(head_ < entries_.size());
    return entries_[head_].value;
  }

  /** Emptied for a new row or column, keeping the entries' storage. */
  void clear()
  {
    entries_.clear();
    head_ = 0;
  }

 private:
  struct Entry {
    int index;
    float value;
  };
  bool maximum_;
  /** Entries below #head_ are already dropped; the front never moves backwards. */
  Vector<Entry> entries_;
  int head_ = 0;
};

/** The three per-channel windows of one pass, built once and cleared between rows or columns. */
class PaintLayerExtremumWindows {
 public:
  explicit PaintLayerExtremumWindows(const bool maximum)
      : windows_{PaintLayerExtremumWindow(maximum),
                 PaintLayerExtremumWindow(maximum),
                 PaintLayerExtremumWindow(maximum)}
  {
  }

  void clear()
  {
    for (PaintLayerExtremumWindow &window : windows_) {
      window.clear();
    }
  }

  PaintLayerExtremumWindow &operator[](const int channel)
  {
    return windows_[channel];
  }

 private:
  PaintLayerExtremumWindow windows_[3];
};

/**
 * One horizontal pass of the extreme of a (2*radius+1) window over the RGB of \a buffer, edges
 * clamped, alpha untouched. Rows run in parallel; each keeps a scratch copy of itself, since the
 * sliding window reads behind the texel the pass has already written.
 */
static void paint_layer_effect_extremum_pass_h(ImBuf &buffer, const int radius, const bool maximum)
{
  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    Vector<float> row(int64_t(width) * 4);
    PaintLayerExtremumWindows windows(maximum);
    for (const int y : range) {
      float *dst = pixels + int64_t(y) * width * 4;
      memcpy(row.data(), dst, size_t(width) * sizeof(float[4]));
      windows.clear();
      int pushed = -1;
      for (const int x : IndexRange(width)) {
        const int new_hi = std::min(x + radius, width - 1);
        for (int k = pushed + 1; k <= new_hi; k++) {
          for (const int c : IndexRange(3)) {
            windows[c].push(k, row[int64_t(k) * 4 + c]);
          }
        }
        pushed = new_hi;
        const int lo = x - radius;
        for (const int c : IndexRange(3)) {
          windows[c].drop_before(lo);
          dst[int64_t(x) * 4 + c] = windows[c].front();
        }
      }
    }
  });
}

/**
 * One vertical pass of the same window over the RGB of \a buffer.
 *
 * The pass walks the buffer column by column, and a column walk of a 4K float buffer strided by a
 * row is a cache miss per texel, so the columns go in blocks: a block copies its own pixels out one
 * row at a time (contiguous, like the horizontal pass's scratch), works over the copy and copies
 * back the same way. Blocks run in parallel; the windows live for the whole block and are emptied
 * between its columns, so no row or column allocates.
 */
static void paint_layer_effect_extremum_pass_v(ImBuf &buffer, const int radius, const bool maximum)
{
  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(width), 16, [&](const IndexRange block) {
    const int block_width = block.size();
    Vector<float> copy(int64_t(block_width) * height * 4);
    PaintLayerExtremumWindows windows(maximum);
    for (const int y : IndexRange(height)) {
      memcpy(copy.data() + int64_t(y) * block_width * 4,
             pixels + (int64_t(y) * width + block.start()) * 4,
             size_t(block_width) * sizeof(float[4]));
    }
    for (const int x : IndexRange(block_width)) {
      windows.clear();
      int pushed = -1;
      for (const int y : IndexRange(height)) {
        const int new_hi = std::min(y + radius, height - 1);
        for (int k = pushed + 1; k <= new_hi; k++) {
          for (const int c : IndexRange(3)) {
            windows[c].push(k, copy[(int64_t(k) * block_width + x) * 4 + c]);
          }
        }
        pushed = new_hi;
        const int lo = y - radius;
        for (const int c : IndexRange(3)) {
          windows[c].drop_before(lo);
          copy[(int64_t(y) * block_width + x) * 4 + c] = windows[c].front();
        }
      }
    }
    for (const int y : IndexRange(height)) {
      memcpy(pixels + (int64_t(y) * width + block.start()) * 4,
             copy.data() + int64_t(y) * block_width * 4,
             size_t(block_width) * sizeof(float[4]));
    }
  });
}

/** The Erode/Dilate kernel: the extreme of a square (2*radius+1) window per channel, one pass per
 * axis (min and max are separable), edges clamped, alpha untouched. */
static void paint_layer_effect_erode_dilate(ImBuf &buffer, const bool erode, const int radius)
{
  const int r = std::clamp(radius, 1, std::max(buffer.x, buffer.y));
  paint_layer_effect_extremum_pass_h(buffer, r, !erode);
  paint_layer_effect_extremum_pass_v(buffer, r, !erode);
}

/**
 * Run \a op over the RGB of \a buffer against a 3x3 neighbourhood of a private source clone. The
 * nine neighbours are read once into \a taps, laid out `[dy][dx]` with `dx/dy = 0` for `x - 1` /
 * `y - 1`; the row and column indices are clamped once per tap, not twice per read, and an interior
 * texel reads its rows without any clamp. The source is a clone, so the rows write the buffer while
 * reading past it without a race. Alpha is untouched.
 *
 * A new 3x3 effect is one formula over \a taps, with no copy of this frame around it.
 */
template<typename Op>
static void paint_layer_effect_convolve3x3(ImBuf &buffer, Op op)
{
  std::shared_ptr<ImBuf> source = paint_layer_anchor_slice_buffer_clone(buffer);
  if (source == nullptr) {
    return;
  }
  const int width = buffer.x;
  const int height = buffer.y;
  const float *src = source->float_buffer.data;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      const int64_t row_prev = int64_t(std::max(y - 1, 0)) * width * 4;
      const int64_t row_cur = int64_t(y) * width * 4;
      const int64_t row_next = int64_t(std::min(y + 1, height - 1)) * width * 4;
      float *dst = pixels + row_cur;
      for (const int x : IndexRange(width)) {
        const int64_t col_prev = int64_t(std::max(x - 1, 0)) * 4;
        const int64_t col_cur = int64_t(x) * 4;
        const int64_t col_next = int64_t(std::min(x + 1, width - 1)) * 4;
        for (const int c : IndexRange(3)) {
          const float taps[3][3] = {
              {src[row_prev + col_prev + c],
               src[row_prev + col_cur + c],
               src[row_prev + col_next + c]},
              {src[row_cur + col_prev + c],
               src[row_cur + col_cur + c],
               src[row_cur + col_next + c]},
              {src[row_next + col_prev + c],
               src[row_next + col_cur + c],
               src[row_next + col_next + c]},
          };
          dst[col_cur + c] = op(taps);
        }
      }
    }
  });
}

/**
 * The Sobel gradient magnitude of \a buffer, per channel: the 3x3 gradients along both axes,
 * normalized so a full-range step reads as one and clamped there. A flat area is zero. Alpha is
 * untouched.
 */
static void paint_layer_effect_edge_detect(ImBuf &buffer)
{
  paint_layer_effect_convolve3x3(buffer, [](const float t[3][3]) {
    const float gx = (t[0][2] + 2.0f * t[1][2] + t[2][2]) -
                     (t[0][0] + 2.0f * t[1][0] + t[2][0]);
    const float gy = (t[0][0] + 2.0f * t[0][1] + t[0][2]) -
                     (t[2][0] + 2.0f * t[2][1] + t[2][2]);
    /* The kernels sum their side to four: a full-range step reaches exactly one, and the response of
     * a softer edge scales with it. */
    return std::clamp(std::sqrt(gx * gx + gy * gy) * 0.25f, 0.0f, 1.0f);
  });
}

/**
 * The Emboss kernel of \a buffer, per channel: the diagonal 3x3 lighting kernel
 * `[-2 -1 0 / -1 1 1 / 0 1 2]`, whose weights sum to one, so a flat colour is its own result and
 * only edges take the light. The response is clamped to [0, 1]. Alpha is untouched.
 */
static void paint_layer_effect_emboss(ImBuf &buffer)
{
  static const float kernel[3][3] = {{-2.0f, -1.0f, 0.0f},
                                     {-1.0f, 1.0f, 1.0f},
                                     {0.0f, 1.0f, 2.0f}};
  paint_layer_effect_convolve3x3(buffer, [](const float t[3][3]) {
    float sum = 0.0f;
    for (const int dy : IndexRange(3)) {
      for (const int dx : IndexRange(3)) {
        sum += kernel[dy][dx] * t[dy][dx];
      }
    }
    return std::clamp(sum, 0.0f, 1.0f);
  });
}

/**
 * The Noise kernel over \a buffer in place: a grain of the type's family, at the intensity, from
 * the seed, per channel or through the colour management's scene-linear luminance. It is a function
 * of the texel and the seed alone, so the bake and the preview compute the same grain and the rows
 * can run in parallel. Alpha stays as it is: the contract of a noisy alpha is not settled, so it is
 * not offered.
 *
 * The luminance is `IMB_colormanagement_get_luminance`, the same one the High Pass reads, so the
 * two agree in a non-Rec.709 working space; the combined preview's shader keeps its own Rec.709
 * weights, which is an existing divergence of that path and not of the CPU kernel.
 */
static void paint_layer_effect_noise_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  const int type = paint_layer_kind_param_int_get(effect, PAINT_LAYER_NOISE_TYPE);
  const float intensity = paint_layer_kind_param_get(effect, PAINT_LAYER_NOISE_INTENSITY);
  const bool monochrome = paint_layer_kind_param_bool_get(effect, PAINT_LAYER_NOISE_MONOCHROME);
  const int seed = paint_layer_kind_param_int_get(effect, PAINT_LAYER_NOISE_SEED);

  const auto offset = [&](const int channel, const int x, const int y) {
    return (type == PAINT_LAYER_NOISE_UNIFORM) ?
               (paint_layer_noise_uniform(x, y, seed, channel, 1) * 2.0f - 1.0f) * intensity :
               paint_layer_noise_gaussian(x, y, seed, channel, intensity);
  };

  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        float *p = pixels + (int64_t(y) * width + x) * 4;
        if (type == PAINT_LAYER_NOISE_SALT_PEPPER) {
          /* A salted channel reads black, a peppered one white, and the rest stay as they are: the
           * amount is the intensity, half to each side. */
          const float reach = intensity * 0.5f;
          for (const int k : IndexRange(3)) {
            const float u = paint_layer_noise_uniform(x, y, seed, k, 1);
            p[k] = (u < reach) ? 0.0f : ((u > 1.0f - reach) ? 1.0f : p[k]);
          }
          continue;
        }
        if (!monochrome) {
          for (const int k : IndexRange(3)) {
            const float n = offset(k, x, y);
            p[k] = (type == PAINT_LAYER_NOISE_SPECKLE) ? clamp_f(p[k] * (1.0f + n), 0.0f, 1.0f) :
                                                         clamp_f(p[k] + n, 0.0f, 1.0f);
          }
          continue;
        }
        /* One noise per texel, laid through the luminance: the colour follows it, so the grain is
         * the same grey the eye reads. */
        const float lum = IMB_colormanagement_get_luminance(p);
        const float n = offset(0, x, y);
        const float moved = (type == PAINT_LAYER_NOISE_SPECKLE) ?
                                clamp_f(lum * (1.0f + n), 0.0f, 1.0f) :
                                clamp_f(lum + n, 0.0f, 1.0f);
        const float delta = moved - lum;
        p[0] += delta;
        p[1] += delta;
        p[2] += delta;
      }
    }
  });
}

/** Three box passes, a gaussian approximation; the radius reads as the per-pass half reach. */
static void paint_layer_effect_blur_soft(ImBuf &buffer, const int radius)
{
  const int pass = std::max(radius / 2, 1);
  paint_layer_effect_blur_box(buffer, pass);
  paint_layer_effect_blur_box(buffer, pass);
  paint_layer_effect_blur_box(buffer, pass);
}

/**
 * The Glow kernel over \a buffer in place: the content's luminance, choked by the spread, blurred
 * by the width and screened under the colour at the opacity. The glow bleeds off the content's own
 * light, so the bright areas of a row spill over its dark ones; the source is the colour
 * management's scene-linear luminance, the one the High Pass and the Noise read. Alpha is untouched.
 */
static void paint_layer_effect_glow_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  const int mode = paint_layer_kind_param_int_get(effect, PAINT_LAYER_GLOW_MODE);
  const int width = paint_layer_kind_param_int_get(effect, PAINT_LAYER_GLOW_WIDTH);
  const float spread = paint_layer_kind_param_get(effect, PAINT_LAYER_GLOW_SPREAD);
  const float opacity = paint_layer_kind_param_get(effect, PAINT_LAYER_GLOW_OPACITY);
  float color[4];
  paint_layer_kind_param_color_get(effect, PAINT_LAYER_GLOW_COLOR, color);
  if (opacity <= 0.0f) {
    return;
  }

  std::shared_ptr<ImBuf> glow = paint_layer_anchor_slice_buffer_clone(buffer);
  if (glow == nullptr) {
    return;
  }
  const int tile_width = buffer.x;
  const float choke_den = std::max(1.0f - spread, 1e-6f);
  paint_layer_effect_map_rgb(*glow, [&](const int /*x*/, const int /*y*/, float *texel) {
    const float luma = clamp_f(IMB_colormanagement_get_luminance(texel), 0.0f, 1.0f);
    const float m = clamp_f((luma - spread) / choke_den, 0.0f, 1.0f);
    texel[0] = m;
    texel[1] = m;
    texel[2] = m;
  });

  /* One box pass is precise; three soften into a gaussian falloff. */
  if (mode == PAINT_LAYER_GLOW_PRECISE) {
    paint_layer_effect_blur_box(*glow, width);
  }
  else {
    paint_layer_effect_blur_soft(*glow, width);
  }

  /* Screened under the colour, `out = c + glow * (1 - c)`: a black texel under the glow lands
   * exactly on the colour, a lit one keeps its light, and the result stays bounded where the colour
   * is. */
  const float *glow_data = glow->float_buffer.data;
  paint_layer_effect_map_rgb(buffer, [&](const int x, const int y, float *texel) {
    const float m = glow_data[(int64_t(y) * tile_width + x) * 4] * opacity;
    for (const int c : IndexRange(3)) {
      texel[c] += color[c] * m * (1.0f - texel[c]);
    }
  });
}

/**
 * The Bevel kernel over \a buffer in place: the distance of each texel from the content's border,
 * normalized by the distance parameter and clamped to [0, 1], written to all three colour channels,
 * then softened by a box blur of the smooth radius. The source parameter says what a border is: the
 * content's alpha below one half, or its luminance below one half -- the same scene-linear
 * luminance the Glow and the Noise read, so an opaque row bevels by the dark side of its paint. A
 * row with no border is one everywhere. Alpha is untouched.
 *
 * The distance is a two-pass chamfer transform. The round corner type walks sides at 3 and
 * diagonals at 4 (an approximation of the Euclidean distance, hence the 1/3 normalization); the
 * angular one is the chessboard walk, which keeps corners square. An interior texel reads its
 * neighbours through the row pointers alone; only the edge rows and columns pay for the clamped
 * read, which was the pass's whole cost on a 4K map.
 */
static void paint_layer_effect_bevel_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  const int distance = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BEVEL_DISTANCE);
  const bool luminance = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BEVEL_SOURCE) ==
                         PAINT_LAYER_BEVEL_SOURCE_LUMINANCE;
  const bool angular = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BEVEL_CORNER) ==
                       PAINT_LAYER_BEVEL_ANGULAR;
  const int smooth = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BEVEL_SMOOTH);
  const float side = angular ? 1.0f : 3.0f;
  const float diagonal = angular ? 1.0f : 4.0f;
  const float normalize = angular ? 1.0f : 1.0f / 3.0f;
  constexpr float UNSEEN = 1e12f;

  const int width = buffer.x;
  const int height = buffer.y;
  const int64_t count = int64_t(width) * height;
  const float *source = buffer.float_data();
  Vector<float> dist(count);
  for (const int64_t i : IndexRange(count)) {
    const float *texel = source + i * 4;
    const float interior = luminance ? IMB_colormanagement_get_luminance(texel) : texel[3];
    dist[i] = (interior >= 0.5f) ? UNSEEN : 0.0f;
  }
  /* The buffer's edge is not a border: a neighbour past it is read as the edge texel itself. */
  const auto at = [&](const int x, const int y) {
    return dist[int64_t(std::clamp(y, 0, height - 1)) * width + std::clamp(x, 0, width - 1)];
  };
  for (const int y : IndexRange(height)) {
    float *row = dist.data() + int64_t(y) * width;
    const float *above = (y > 0) ? row - width : row;
    for (const int x : IndexRange(width)) {
      if (y == 0 || x == 0 || x == width - 1) {
        row[x] = std::min({row[x],
                           at(x - 1, y) + side,
                           at(x, y - 1) + side,
                           at(x - 1, y - 1) + diagonal,
                           at(x + 1, y - 1) + diagonal});
      }
      else {
        row[x] = std::min({row[x],
                           row[x - 1] + side,
                           above[x] + side,
                           above[x - 1] + diagonal,
                           above[x + 1] + diagonal});
      }
    }
  }
  for (int y = height - 1; y >= 0; y--) {
    float *row = dist.data() + int64_t(y) * width;
    const float *below = (y < height - 1) ? row + width : row;
    for (int x = width - 1; x >= 0; x--) {
      if (y == height - 1 || x == 0 || x == width - 1) {
        row[x] = std::min({row[x],
                           at(x + 1, y) + side,
                           at(x, y + 1) + side,
                           at(x + 1, y + 1) + diagonal,
                           at(x - 1, y + 1) + diagonal});
      }
      else {
        row[x] = std::min({row[x],
                           row[x + 1] + side,
                           below[x] + side,
                           below[x + 1] + diagonal,
                           below[x - 1] + diagonal});
      }
    }
  }

  const float reach = float(std::max(distance, 1));
  paint_layer_effect_map_rgb(buffer, [&](const int x, const int y, float *texel) {
    const float value = clamp_f(dist[int64_t(y) * width + x] * normalize / reach, 0.0f, 1.0f);
    texel[0] = value;
    texel[1] = value;
    texel[2] = value;
  });
  if (smooth > 0) {
    paint_layer_effect_blur_box(buffer, smooth);
  }
}

/** One thread's running total of the average: the alpha-weighted sums and the weight they divide
 * by. Doubles, so a 4K map's accumulation stays far past the float precision of its pixels. */
struct PaintLayerAverageRun {
  double sum[3] = {0.0, 0.0, 0.0};
  double weight = 0.0;
};

/** The alpha-weighted average RGB of \a buffer; black when it holds no content. Rows accumulate in
 * parallel and the totals reduce in the order the threads finish in, which moves the sum by a
 * fraction of its last double digit -- far under anything a colour shows. */
static void paint_layer_effect_color_average(const ImBuf &buffer, float r_average[3])
{
  const float *pixels = buffer.float_data();
  const int width = buffer.x;
  const PaintLayerAverageRun total = threading::parallel_reduce(
      IndexRange(buffer.y),
      64,
      PaintLayerAverageRun{},
      [&](const IndexRange range, const PaintLayerAverageRun &identity) {
        PaintLayerAverageRun run = identity;
        for (const int y : range) {
          const float *row = pixels + int64_t(y) * width * 4;
          for (const int x : IndexRange(width)) {
            const float *texel = row + int64_t(x) * 4;
            if (texel[3] > 0.0f) {
              for (const int c : IndexRange(3)) {
                run.sum[c] += double(texel[c]) * texel[3];
              }
              run.weight += texel[3];
            }
          }
        }
        return run;
      },
      [](const PaintLayerAverageRun &a, const PaintLayerAverageRun &b) {
        PaintLayerAverageRun out;
        for (const int c : IndexRange(3)) {
          out.sum[c] = a.sum[c] + b.sum[c];
        }
        out.weight = a.weight + b.weight;
        return out;
      });
  for (const int c : IndexRange(3)) {
    r_average[c] = (total.weight > 0.0) ? float(total.sum[c] / total.weight) : 0.0f;
  }
}

/**
 * The Color Match kernel over \a buffer in place: the source colour (the input's own average, or
 * the picked one) graded to the target colour, with the optional variation on top. Alpha is
 * untouched.
 */
static void paint_layer_effect_color_match_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  float source[4];
  float target[4];
  paint_layer_kind_param_color_get(effect, PAINT_LAYER_COLOR_MATCH_SOURCE_COLOR, source);
  paint_layer_kind_param_color_get(effect, PAINT_LAYER_COLOR_MATCH_TARGET_COLOR, target);
  if (!paint_layer_kind_param_bool_get(effect, PAINT_LAYER_COLOR_MATCH_SOURCE_PICKED)) {
    paint_layer_effect_color_average(buffer, source);
  }
  const bool variation_on = paint_layer_kind_param_bool_get(effect,
                                                            PAINT_LAYER_COLOR_MATCH_VARIATION);
  PaintLayerColorMatchVariation variation;
  if (variation_on) {
    variation.hue = paint_layer_kind_param_get(effect, PAINT_LAYER_COLOR_MATCH_VARIATION_HUE);
    variation.chroma = paint_layer_kind_param_get(effect,
                                                  PAINT_LAYER_COLOR_MATCH_VARIATION_CHROMA);
    variation.luma = paint_layer_kind_param_get(effect, PAINT_LAYER_COLOR_MATCH_VARIATION_LUMA);
  }

  paint_layer_effect_map_rgb(buffer, [&](const int x, const int y, float *texel) {
    paint_layer_color_match_apply(source, target, variation, x, y, texel);
  });
}

void paint_layer_effect_kernel_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  /* Timed apart from the input it is handed: this is the half an effect pass computes instead, so
   * the line stopping to print in `BLENDER_PBR_PERF` is what a covered kind bought. The region is
   * always the whole buffer -- a neighbourhood reads past an edited rectangle, and the cache has no
   * halo to re-render one with yet. */
  bke::paint_perf::ScopedTimer perf_kernel("effect.cpu.kernel");
  perf_kernel.note(
      "kind=%d size=%dx%d region=full", int(effect.kind_subtype), buffer.x, buffer.y);
  switch (eMaterialPaintLayerFilter(effect.kind_subtype)) {
    case MA_PAINT_LAYER_FILTER_BLUR: {
      const int radius = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BLUR_RADIUS);
      if (paint_layer_kind_param_int_get(effect, PAINT_LAYER_BLUR_SHAPE) ==
          PAINT_LAYER_BLUR_BOX)
      {
        paint_layer_effect_blur_box(buffer, radius);
      }
      else {
        paint_layer_effect_blur_soft(buffer, radius);
      }
      break;
    }
    case MA_PAINT_LAYER_FILTER_SHARPEN: {
      /* Unsharp masking: the detail the blur removes comes back on top of the colour. */
      const int radius = paint_layer_kind_param_int_get(effect, PAINT_LAYER_SHARPEN_RADIUS);
      const float amount = paint_layer_kind_param_get(effect, PAINT_LAYER_SHARPEN_AMOUNT);
      paint_layer_effect_unsharp_apply(
          buffer, radius, [amount](const float orig, const float blurred) {
            return orig + (orig - blurred) * amount;
          });
      break;
    }
    case MA_PAINT_LAYER_FILTER_HIGH_PASS: {
      /* The detail against a blurred copy, over the mid grey; a flat colour lands on it exactly.
       * The difference is taken in the display encoding, as image editors do: in scene linear the
       * dark tones' detail is squeezed to nothing and the result reads as a flat grey. The grey is
       * the encoded 0.5, decoded back to the linear value the rest of the stack expects. */
      const int radius = paint_layer_kind_param_int_get(effect, PAINT_LAYER_HIGH_PASS_RADIUS);
      const float contrast = paint_layer_kind_param_get(effect, PAINT_LAYER_HIGH_PASS_CONTRAST);
      if (paint_layer_kind_param_int_get(effect, PAINT_LAYER_HIGH_PASS_OUTPUT) ==
          PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE)
      {
        /* The luminance goes in before the filter, so the blur and the detail are the luminance's
         * own rather than a grey made of three separate details. */
        paint_layer_effect_to_luminance(buffer);
      }
      paint_layer_effect_unsharp_apply(
          buffer, radius, [contrast](const float orig, const float blurred) {
            const float detail = linearrgb_to_srgb(orig) - linearrgb_to_srgb(blurred);
            return srgb_to_linearrgb(0.5f + detail * contrast);
          });
      break;
    }
    case MA_PAINT_LAYER_FILTER_EDGE_DETECT:
      paint_layer_effect_edge_detect(buffer);
      break;
    case MA_PAINT_LAYER_FILTER_EMBOSS:
      paint_layer_effect_emboss(buffer);
      break;
    case MA_PAINT_LAYER_FILTER_ERODE_DILATE: {
      const int radius = paint_layer_kind_param_int_get(effect, PAINT_LAYER_ERODE_DILATE_RADIUS);
      const bool erode = paint_layer_kind_param_int_get(effect, PAINT_LAYER_ERODE_DILATE_MODE) ==
                         PAINT_LAYER_ERODE_DILATE_ERODE;
      paint_layer_effect_erode_dilate(buffer, erode, radius);
      break;
    }
    case MA_PAINT_LAYER_FILTER_NOISE:
      paint_layer_effect_noise_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_GLOW:
      paint_layer_effect_glow_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_BEVEL:
      paint_layer_effect_bevel_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_COLOR_MATCH:
      paint_layer_effect_color_match_apply(buffer, effect);
      break;
    default:
      break;
  }
}

}  // namespace blender
