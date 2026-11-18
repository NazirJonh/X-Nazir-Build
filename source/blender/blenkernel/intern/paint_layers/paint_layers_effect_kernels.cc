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
#include <limits>
#include <memory>

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_constants.h"
#include "BLI_math_interp.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"

#include "BKE_paint.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"
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
 * A line average at \a angle_deg over the RGB of \a buffer, mixed over the colour by \a mix: the
 * samples walk the line the angle names, one texel at a time, `\a radius` steps each way of the
 * centre, each read clamped to the canvas edge like a box pass's window. The samples average
 * plainly -- the streak the filter lays is the direction, not a weighted tail. Alpha is untouched.
 */
static void paint_layer_effect_blur_directional(ImBuf &buffer,
                                                const float angle_deg,
                                                const int radius,
                                                const float mix)
{
  if (mix <= 0.0f) {
    return;
  }
  std::shared_ptr<ImBuf> source = paint_layer_anchor_slice_buffer_clone(buffer);
  if (source == nullptr) {
    return;
  }
  const int width = buffer.x;
  const int height = buffer.y;
  /* The angle names the direction the samples walk, counter-clockwise off the map's +X: the same
   * reading the angle of a gradient or a mapping has. The step is whole texels, so a diagonal
   * angle's reach is the radius along the line, not along either axis alone. */
  const float angle_rad = angle_deg * float(M_PI) / 180.0f;
  const float2 direction = {std::cos(angle_rad), std::sin(angle_rad)};
  const int reach = std::clamp(radius, 1, std::max(width, height));
  const float weight = std::clamp(mix, 0.0f, 1.0f);
  const float *src = source->float_buffer.data;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      float *dst = pixels + int64_t(y) * width * 4;
      for (const int x : IndexRange(width)) {
        float sum[3] = {0.0f, 0.0f, 0.0f};
        for (const int k : IndexRange(-reach, 2 * reach + 1)) {
          /* One texel along the line per step, clamped to the canvas the window is. */
          const int sx = std::clamp(x + int(std::floor(direction.x * float(k) + 0.5f)), 0, width - 1);
          const int sy = std::clamp(y + int(std::floor(direction.y * float(k) + 0.5f)), 0, height - 1);
          const float *tap = src + (int64_t(sy) * width + sx) * 4;
          sum[0] += tap[0];
          sum[1] += tap[1];
          sum[2] += tap[2];
        }
        const float inv = 1.0f / float(2 * reach + 1);
        for (const int c : IndexRange(3)) {
          const float averaged = sum[c] * inv;
          dst[int64_t(x) * 4 + c] += (averaged - dst[int64_t(x) * 4 + c]) * weight;
        }
      }
    }
  });
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
          dst[i] = paint_layer_unsharp_channel_apply(op, dst[i], src[i]);
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

/** The scene-linear luminance of each texel of \a buffer written to all three colour channels. The
 * mask effects whose kernels can leave the grey colored bring their result back through it. */
void paint_layer_effect_to_luminance(ImBuf &buffer)
{
  paint_layer_effect_map_rgb(buffer, [](const int /*x*/, const int /*y*/, float *texel) {
    const float luminance = paint_layer_luminance_get(texel);
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

  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        float *p = pixels + (int64_t(y) * width + x) * 4;
        paint_layer_noise_texel_apply(type, intensity, monochrome, seed, x, y, p);
      }
    }
  });
}

/** Three box passes, a gaussian approximation; the radius reads as the per-pass half reach. */
static void paint_layer_effect_blur_soft(ImBuf &buffer, const int radius)
{
  const int pass = paint_layer_gaussian_pass_radius(radius);
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
 * The Drop Shadow over \a buffer in place: the silhouette its source names, spread (a negative
 * spread erodes it), blurred by the size and offset by the distance along the angle, tinted by
 * the colour and laid over the content by the opacity. An alpha source lays the shadow where the
 * content is not -- the shadow a stroke casts on the empty map around it -- and a luminance
 * source lays it wherever the offset silhouette covers, the way a mask's grey is darkened by the
 * shape the mask itself casts.
 *
 * \a as_layer picks what the buffer holds afterwards. A content effect leaves the shadow as a layer
 * of its own (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA): the colour of the shadow, premultiplied like
 * the effect cache's other buffers, and the alpha it covers by, `shadow x opacity x under` with the
 * "under the content" folded into it, so the composite lays it over a row that has no content
 * there. Without it (a mask item) the shadow is mixed into the grey and alpha is untouched; a
 * mask's buffer has no alpha to cast from, so the shape is always the luminance there.
 */
static void paint_layer_effect_drop_shadow_apply(ImBuf &buffer,
                                                 const MaterialPaintLayer &effect,
                                                 const bool as_layer)
{
  const int source = as_layer ?
                         paint_layer_kind_param_int_get(effect, PAINT_LAYER_DROP_SHADOW_SOURCE) :
                         int(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE);
  const float angle_deg = paint_layer_kind_param_get(effect, PAINT_LAYER_DROP_SHADOW_ANGLE);
  const int distance = paint_layer_kind_param_int_get(effect, PAINT_LAYER_DROP_SHADOW_DISTANCE);
  const int size = paint_layer_kind_param_int_get(effect, PAINT_LAYER_DROP_SHADOW_SIZE);
  const int spread = paint_layer_kind_param_int_get(effect, PAINT_LAYER_DROP_SHADOW_SPREAD);
  const float opacity = paint_layer_kind_param_get(effect, PAINT_LAYER_DROP_SHADOW_OPACITY);
  float color[4];
  paint_layer_kind_param_color_get(effect, PAINT_LAYER_DROP_SHADOW_COLOR, color);
  /* A layer that casts nothing is empty, not the content it was handed: the buffer would otherwise
   * be read as a shadow of the row's own colour. */
  const auto layer_clear = [&]() {
    if (as_layer) {
      std::fill_n(buffer.float_data_for_write(), int64_t(buffer.x) * buffer.y * 4, 0.0f);
    }
  };
  if (opacity <= 0.0f) {
    layer_clear();
    return;
  }

  std::shared_ptr<ImBuf> silhouette = paint_layer_anchor_slice_buffer_clone(buffer);
  if (silhouette == nullptr) {
    layer_clear();
    return;
  }
  paint_layer_effect_map_rgb(*silhouette, [&](const int /*x*/, const int /*y*/, float *texel) {
    const float shape = (source == PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE) ?
                            clamp_f(IMB_colormanagement_get_luminance(texel), 0.0f, 1.0f) :
                            texel[3];
    texel[0] = shape;
    texel[1] = shape;
    texel[2] = shape;
  });

  /* The spread grows (a positive one dilates) or shrinks (a negative one erodes) the silhouette
   * before the blur softens its edge; zero of either leaves it as it is. */
  if (spread > 0) {
    paint_layer_effect_erode_dilate(*silhouette, false, spread);
  }
  else if (spread < 0) {
    paint_layer_effect_erode_dilate(*silhouette, true, -spread);
  }
  if (size > 0) {
    paint_layer_effect_blur_box(*silhouette, size);
  }

  /* The shadow is cast toward the angle: every texel reads the silhouette the offset lands on, so
   * the shadow sits displaced along the direction the angle names. */
  const float angle_rad = angle_deg * float(M_PI) / 180.0f;
  const float2 direction = {std::cos(angle_rad), std::sin(angle_rad)};
  /* Rounded half up, the project rule, not the half away from zero of `std::round`. */
  const int offset_x = int(std::floor(direction.x * float(distance) + 0.5f));
  const int offset_y = int(std::floor(direction.y * float(distance) + 0.5f));
  const int width = buffer.x;
  const int height = buffer.y;
  const float *shadow_data = silhouette->float_buffer.data;
  paint_layer_effect_map_rgb(buffer, [&](const int x, const int y, float *texel) {
    const int sx = x - offset_x;
    const int sy = y - offset_y;
    /* A texel the offset reads from past the map's edge has no silhouette to cast: clamping would
     * smear the edge row of the shape across the whole shadow band. */
    const bool inside = sx >= 0 && sx < width && sy >= 0 && sy < height;
    const float shadow = inside ? shadow_data[(int64_t(sy) * width + sx) * 4] : 0.0f;
    /* An alpha source keeps the shadow where the content is not: the stroke casts on the empty
     * map around it, and the stroke itself keeps its colour. A luminance source darkens wherever
     * the offset silhouette covers, the way a mask's grey is laid over the mask. */
    const float under = (source == PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA) ?
                            1.0f - clamp_f(texel[3], 0.0f, 1.0f) :
                            1.0f;
    const float weight = shadow * opacity * under;
    if (as_layer) {
      /* The alpha is the weight and the colour is the shadow's own, premultiplied by it. */
      const float alpha = clamp_f(weight, 0.0f, 1.0f);
      texel[0] = color[0] * alpha;
      texel[1] = color[1] * alpha;
      texel[2] = color[2] * alpha;
      texel[3] = alpha;
      return;
    }
    for (const int c : IndexRange(3)) {
      texel[c] += (color[c] - texel[c]) * weight;
    }
  });
}

/**
 * A two-pass chamfer walk over \a dist, seeded with the zeroes the caller laid (every other texel
 * holds a huge value): every texel walks to its nearest seed, \a side per orthogonal step and
 * \a diagonal per diagonal one (3 and 4 approximate the Euclidean distance, hence the 1/3 the
 * caller normalizes by; 1 and 1 is the chessboard walk). A texel's neighbour past the buffer's edge
 * is read as the texel itself, so the edge is no border of its own. A texel no seed reaches (the
 * buffer holds none) keeps its huge value, which reads as infinitely far.
 *
 * An interior texel reads its neighbours through the row pointers alone; only the edge rows and
 * columns pay for the clamped read, which was the pass's whole cost on a 4K map.
 */
static void paint_layer_effect_chamfer_walk(
    Vector<float> &dist, const int width, const int height, const float side, const float diagonal)
{
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
}

/**
 * The Mask Outline over \a buffer in place: a band along the border the threshold crosses in the
 * grey, lying outside it, inside it, or on both sides, falling from one at the border to zero a
 * width away, bent by the curve shape from a line to the smoothstep, and softened by the blur.
 *
 * \a as_layer picks what the buffer holds afterwards. A content effect reads its shape from the
 * alpha the source names (or the luminance) and leaves the band as a layer of its own
 * (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA): the colour premultiplied by the band, and the band as the
 * alpha, so the ring shows on the empty map around the islands and the islands themselves are
 * untouched. Without it (a mask item) the shape is the luminance of the mask's grey, the band is
 * written as a grey into the colour and alpha is untouched. The kernel reads one ImBuf, one UDIM
 * tile: a shape that runs off the tile's border is outlined against that border, not followed
 * into its neighbour.
 */
static void paint_layer_effect_mask_outline_apply(ImBuf &buffer,
                                                  const MaterialPaintLayer &effect,
                                                  const bool as_layer)
{
  const bool alpha_source = as_layer &&
                            paint_layer_kind_param_int_get(
                                effect, PAINT_LAYER_MASK_OUTLINE_SOURCE) ==
                                PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA;
  float color[4];
  paint_layer_kind_param_color_get(effect, PAINT_LAYER_MASK_OUTLINE_COLOR, color);
  if (alpha_source) {
    /* The panel hides the colour while the shape is read from the alpha, so a value left from an
     * earlier luminance source must not tint the ring. */
    std::fill(std::begin(color), std::end(color), 1.0f);
  }
  const int position = paint_layer_kind_param_int_get(effect, PAINT_LAYER_MASK_OUTLINE_POSITION);
  const bool start_black = paint_layer_kind_param_bool_get(effect,
                                                           PAINT_LAYER_MASK_OUTLINE_START_BLACK);
  const float threshold = paint_layer_kind_param_get(effect, PAINT_LAYER_MASK_OUTLINE_THRESHOLD);
  const int width_param = paint_layer_kind_param_int_get(effect, PAINT_LAYER_MASK_OUTLINE_WIDTH);
  const int blur = paint_layer_kind_param_int_get(effect, PAINT_LAYER_MASK_OUTLINE_BLUR);
  const float shape = paint_layer_kind_param_get(effect, PAINT_LAYER_MASK_OUTLINE_SHAPE);

  const int width = buffer.x;
  const int height = buffer.y;
  const int64_t count = int64_t(width) * height;
  const float *source = buffer.float_data();

  /* The inside the threshold holds: what crosses it (or, from black, what falls short of it). */
  const auto is_inside = [&](const float *texel) {
    const float grey = alpha_source ?
                           clamp_f(texel[3], 0.0f, 1.0f) :
                           clamp_f(IMB_colormanagement_get_luminance(texel), 0.0f, 1.0f);
    return start_black ? (grey <= threshold) : (grey >= threshold);
  };
  /* The inside is read once: the walks, the merge and the final write all ask it per texel. */
  Vector<bool> inside(count);
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    const int64_t first = range.start() * int64_t(width);
    for (const int64_t i : IndexRange(first, range.size() * int64_t(width))) {
      inside[i] = is_inside(source + i * 4);
    }
  });

  constexpr float UNSEEN = 1e12f;
  const auto seeded = [&](const bool seed_on_inside) {
    Vector<float> dist(count);
    threading::parallel_for(IndexRange(count), 65536, [&](const IndexRange range) {
      for (const int64_t i : range) {
        dist[i] = (inside[i] == seed_on_inside) ? 0.0f : UNSEEN;
      }
    });
    paint_layer_effect_chamfer_walk(dist, width, height, 3.0f, 4.0f);
    return dist;
  };

  /* A band lies on one side of the border and is empty on the other: outside lays the ring around
   * the shape (the shape itself stays black), inside the ring within it, double both. Each side
   * measures from the texels of the other, so only the walks the position needs run; a buffer with
   * no border on that side leaves the walk unseen, which reads as infinitely far (no band), not as
   * the border itself. */
  const auto position_kind = PaintLayerMaskOutlinePosition(position);
  Vector<float> dist = seeded(position_kind != PAINT_LAYER_MASK_OUTLINE_INSIDE);
  if (position_kind == PAINT_LAYER_MASK_OUTLINE_DOUBLE) {
    /* The outward walk holds the outside texels' distance; the inward one, laid first, holds the
     * inside ones'. The one pass merges them, and the second field is freed with the scope. */
    const Vector<float> inward = seeded(false);
    threading::parallel_for(IndexRange(count), 65536, [&](const IndexRange range) {
      for (const int64_t i : range) {
        if (inside[i]) {
          dist[i] = inward[i];
        }
      }
    });
  }

  const float reach = float(std::max(width_param, 1));
  const float normalize = 1.0f / 3.0f;
  paint_layer_effect_map_rgb(buffer, [&](const int x, const int y, float *texel) {
    const int64_t i = int64_t(y) * width + x;
    /* The side the position leaves empty is no band at all. */
    const bool on_band_side = (position_kind == PAINT_LAYER_MASK_OUTLINE_DOUBLE) ||
                              (inside[i] == (position_kind == PAINT_LAYER_MASK_OUTLINE_INSIDE));
    float band = on_band_side ? clamp_f(1.0f - dist[i] * normalize / reach, 0.0f, 1.0f) : 0.0f;
    /* The falloff's bend: the line the distance lays, walked toward the smoothstep's own curve by
     * the shape. */
    const float smooth = band * band * (3.0f - 2.0f * band);
    band += (smooth - band) * clamp_f(shape, 0.0f, 1.0f);
    texel[0] = band;
    texel[1] = band;
    texel[2] = band;
  });
  if (blur > 0) {
    paint_layer_effect_blur_box(buffer, blur);
  }
  if (as_layer) {
    /* The band is built (and blurred) as a grey first, so the layer's colour and alpha follow the
     * softened band. */
    paint_layer_effect_map_rgb(buffer, [&](const int /*x*/, const int /*y*/, float *texel) {
      const float alpha = clamp_f(texel[0], 0.0f, 1.0f);
      texel[0] = color[0] * alpha;
      texel[1] = color[1] * alpha;
      texel[2] = color[2] * alpha;
      texel[3] = alpha;
    });
  }
}

/**
 * The Fill Area over \a buffer in place: the regions the border of the content closes off take
 * the colour the border carries, diffused inward by the iterations, or the solid colour the mode
 * names. The border is the alpha (or the luminance, as the Bevel reads) at or past the threshold;
 * a region is the connected run of texels short of it, the background the one an open border
 * leaves reachable from the map's edge (or, when the border closes what runs off the map, the
 * largest one). The kernel reads one ImBuf, one UDIM tile: a region open at the tile's border is
 * filled against that border, never through into its neighbour.
 *
 * \a as_layer picks what the buffer holds afterwards. A content effect leaves a layer of its own
 * (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA): the enclosed texels carry the fill, premultiplied, with
 * an alpha of one, everything else is empty -- a hole between strokes has no coverage of its own,
 * so a colour-only result would stay invisible on it. Without it (a mask item) the border is
 * always the luminance, the fill is written into the colour and alpha is untouched.
 */
static void paint_layer_effect_fill_area_apply(ImBuf &buffer,
                                               const MaterialPaintLayer &effect,
                                               const bool as_layer)
{
  /* A mask's grey carries no alpha of its own: its border is always the luminance. */
  const bool by_alpha = as_layer &&
                        paint_layer_kind_param_int_get(effect, PAINT_LAYER_FILL_AREA_SOURCE) ==
                            PAINT_LAYER_FILL_AREA_SOURCE_ALPHA;
  const float threshold = paint_layer_kind_param_get(effect, PAINT_LAYER_FILL_AREA_THRESHOLD);
  const bool debug = paint_layer_kind_param_bool_get(effect, PAINT_LAYER_FILL_AREA_DEBUG);
  const int uv_border = paint_layer_kind_param_int_get(effect, PAINT_LAYER_FILL_AREA_UV_BORDER);
  const int color_mode = paint_layer_kind_param_int_get(effect, PAINT_LAYER_FILL_AREA_COLOR_MODE);
  const int blur = paint_layer_kind_param_int_get(effect, PAINT_LAYER_FILL_AREA_BLUR);
  const int iterations = paint_layer_kind_param_int_get(effect, PAINT_LAYER_FILL_AREA_ITERATIONS);
  float fill_color[4];
  paint_layer_kind_param_color_get(effect, PAINT_LAYER_FILL_AREA_COLOR, fill_color);

  const int width = buffer.x;
  const int height = buffer.y;
  const int64_t count = int64_t(width) * height;
  float *pixels = buffer.float_data_for_write();
  const auto layer_clear = [&]() {
    if (as_layer) {
      std::fill_n(pixels, count * 4, 0.0f);
    }
  };
  /* The texels of a row range, as the flat indices the per-texel passes walk. */
  const auto texels_of = [&](const IndexRange rows) {
    return IndexRange(rows.start() * int64_t(width), rows.size() * int64_t(width));
  };

  /* The border is read once: the flood and the diffusion both ask it per texel. */
  Vector<bool> border(count);
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange rows) {
    for (const int64_t i : texels_of(rows)) {
      const float *texel = pixels + i * 4;
      const float shape = by_alpha ? texel[3] :
                                     clamp_f(IMB_colormanagement_get_luminance(texel), 0.0f, 1.0f);
      border[i] = shape >= threshold;
    }
  });

  /* The region id of every texel short of the border: zero is no region, the rest count the
   * connected runs the flood walks. The flood is the one serial pass (a region grows from one
   * texel), iterative on an explicit queue so a 4K map cannot overflow the stack. */
  Vector<int32_t> region(count, int32_t(0));
  int32_t regions = 0;
  {
    /* Texel indices only, as 32-bit: the image size limit keeps a map's texel count below 2^31.
     * The queue is freed with the scope, before the diffusion's buffer is allocated. */
    Vector<int32_t> queue;
    queue.reserve(count);
    for (const int64_t start : IndexRange(count)) {
      if (border[start] || region[start] != 0) {
        continue;
      }
      regions++;
      region[start] = regions;
      queue.clear();
      queue.append(int32_t(start));
      for (int64_t head = 0; head < queue.size(); head++) {
        const int32_t i = queue[head];
        const int x = i % width;
        const int y = i / width;
        const int32_t neighbours[4] = {
            (x > 0) ? i - 1 : -1,
            (x < width - 1) ? i + 1 : -1,
            (y > 0) ? i - width : -1,
            (y < height - 1) ? i + width : -1,
        };
        for (const int32_t n : neighbours) {
          if (n >= 0 && region[n] == 0 && !border[n]) {
            region[n] = regions;
            queue.append(n);
          }
        }
      }
    }
  }
  if (regions == 0) {
    /* The border covers the whole map: nothing is enclosed. */
    layer_clear();
    return;
  }

  /* The regions the fill leaves alone: the ones an open border leaves reachable from the map's
   * edge, or -- when the border closes what runs off it -- the largest one, the background the
   * map itself holds. The slot of "no region" counts as one, so a border texel is never enclosed. */
  Vector<bool> is_background(int64_t(regions) + 1, false);
  is_background[0] = true;
  if (uv_border == PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE) {
    for (const int x : IndexRange(width)) {
      is_background[region[x]] = true;
      is_background[region[int64_t(height - 1) * width + x]] = true;
    }
    for (const int y : IndexRange(height)) {
      is_background[region[int64_t(y) * width]] = true;
      is_background[region[int64_t(y) * width + width - 1]] = true;
    }
  }
  else {
    /* The largest run wins: a map holds one background, however much the strokes cut into it. */
    Vector<int64_t> area(int64_t(regions) + 1, 0);
    for (const int64_t i : IndexRange(count)) {
      area[region[i]]++;
    }
    int32_t largest_region = 1;
    for (const int32_t r : IndexRange(1, regions)) {
      if (area[r] > area[largest_region]) {
        largest_region = r;
      }
    }
    is_background[largest_region] = true;
  }
  const auto enclosed = [&](const int64_t i) { return !is_background[region[i]]; };

  /* The result written row-parallel: \a color_of gives an enclosed texel's colour. The layer is
   * empty everywhere else; a mask item keeps what the texel held. */
  const auto write_result = [&](const auto &color_of) {
    threading::parallel_for(IndexRange(height), 64, [&](const IndexRange rows) {
      for (const int64_t i : texels_of(rows)) {
        float *texel = pixels + i * 4;
        if (enclosed(i)) {
          color_of(i, texel);
          if (as_layer) {
            texel[3] = 1.0f;
          }
        }
        else if (as_layer) {
          texel[0] = texel[1] = texel[2] = texel[3] = 0.0f;
        }
      }
    });
  };

  /* The debug lays the regions themselves: a grey ramp of their number (the background ones
   * too, the border at zero), the check that finds the strokes whose border is not closed. */
  if (debug) {
    const float scale = 1.0f / float(regions);
    threading::parallel_for(IndexRange(height), 64, [&](const IndexRange rows) {
      for (const int64_t i : texels_of(rows)) {
        float *texel = pixels + i * 4;
        const float value = float(region[i]) * scale;
        texel[0] = texel[1] = texel[2] = value;
        if (as_layer) {
          /* Opaque everywhere, the border as the ramp's zero: the debug is the whole picture. */
          texel[3] = 1.0f;
        }
      }
    });
    return;
  }

  if (color_mode == PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID) {
    /* The solid mode skips the diffusion: the enclosed region takes the colour as it is. */
    write_result([&](const int64_t /*i*/, float *texel) {
      texel[0] = fill_color[0];
      texel[1] = fill_color[1];
      texel[2] = fill_color[2];
    });
  }
  else if (iterations > 0) {
    /* The diffusion: the border texels hold their own colour, the enclosed ones walk toward the
     * average of their neighbours, one pass at a time. A texel's neighbour is a border texel or
     * one of its own region (regions are the connected runs), so the passes ping-pong between the
     * buffer's own colour and one extra RGB field -- the only full-size float buffer beyond it. */
    Vector<float> pong(count * 3, 0.0f);
    threading::parallel_for(IndexRange(height), 64, [&](const IndexRange rows) {
      for (const int64_t i : texels_of(rows)) {
        float *texel = pixels + i * 4;
        if (border[i]) {
          if (as_layer && texel[3] >= 0.01f && texel[3] < 1.0f) {
            /* The input is premultiplied: the fill carries the border's own colour. */
            const float inverse = 1.0f / texel[3];
            texel[0] *= inverse;
            texel[1] *= inverse;
            texel[2] *= inverse;
          }
          pong[i * 3 + 0] = texel[0];
          pong[i * 3 + 1] = texel[1];
          pong[i * 3 + 2] = texel[2];
        }
        else if (enclosed(i)) {
          texel[0] = texel[1] = texel[2] = 0.0f;
        }
      }
    });
    for (const int pass : IndexRange(iterations)) {
      const bool read_pixels = (pass % 2 == 0);
      const float *read = read_pixels ? pixels : pong.data();
      float *write = read_pixels ? pong.data() : pixels;
      const int64_t read_stride = read_pixels ? 4 : 3;
      const int64_t write_stride = read_pixels ? 3 : 4;
      threading::parallel_for(IndexRange(height), 64, [&](const IndexRange rows) {
        for (const int y : rows) {
          for (const int x : IndexRange(width)) {
            const int64_t i = int64_t(y) * width + x;
            if (!enclosed(i)) {
              continue;
            }
            /* The map's edge is no neighbour: a region left open is no wall there. */
            const int64_t neighbours[4] = {
                (x > 0) ? i - 1 : -1,
                (x < width - 1) ? i + 1 : -1,
                (y > 0) ? i - width : -1,
                (y < height - 1) ? i + width : -1,
            };
            float sum[3] = {0.0f, 0.0f, 0.0f};
            int samples = 0;
            for (const int64_t n : neighbours) {
              if (n < 0) {
                continue;
              }
              sum[0] += read[n * read_stride + 0];
              sum[1] += read[n * read_stride + 1];
              sum[2] += read[n * read_stride + 2];
              samples++;
            }
            if (samples == 0) {
              continue;
            }
            const float inverse = 1.0f / float(samples);
            write[i * write_stride + 0] = sum[0] * inverse;
            write[i * write_stride + 1] = sum[1] * inverse;
            write[i * write_stride + 2] = sum[2] * inverse;
          }
        }
      });
    }
    /* The last pass wrote into the buffer when the count is even, into the extra field otherwise. */
    const bool done_in_pixels = (iterations % 2 == 0);
    const float *done = done_in_pixels ? pixels : pong.data();
    const int64_t done_stride = done_in_pixels ? 4 : 3;
    write_result([&](const int64_t i, float *texel) {
      texel[0] = done[i * done_stride + 0];
      texel[1] = done[i * done_stride + 1];
      texel[2] = done[i * done_stride + 2];
    });
  }
  else {
    /* No iterations: the fill has nothing to reach in with, the regions stay as they are. */
    layer_clear();
    return;
  }

  if (blur > 0) {
    if (as_layer) {
      /* The box blur reads the colour only: the coverage is softened as a grey of its own, so the
       * fill's edge fades out instead of staying a hard step under a soft colour. */
      std::shared_ptr<ImBuf> coverage = paint_layer_anchor_slice_buffer_clone(buffer);
      if (coverage == nullptr) {
        return;
      }
      paint_layer_effect_map_rgb(*coverage, [](const int /*x*/, const int /*y*/, float *texel) {
        texel[0] = texel[1] = texel[2] = texel[3];
      });
      paint_layer_effect_blur_box(*coverage, blur);
      paint_layer_effect_blur_box(buffer, blur);
      const float *soft = coverage->float_data();
      float *out = buffer.float_data_for_write();
      threading::parallel_for(IndexRange(count), 65536, [&](const IndexRange range) {
        for (const int64_t i : range) {
          out[i * 4 + 3] = clamp_f(soft[i * 4], 0.0f, 1.0f);
        }
      });
    }
    else {
      paint_layer_effect_blur_box(buffer, blur);
    }
  }
}

/**
 * The Height To Normal kernel over \a buffer in place. The buffer holds the row's Height (the input
 * describe lays the Height content for this kind), read from the red; it is replaced by the
 * tangent-space normal of that height, encoded `0.5 + 0.5 n` in the OpenGL convention the Normal
 * Map node reads (green up along +V, which is the row direction of an image), or with the green
 * flipped for the DirectX one. A flat height is the neutral normal (0.5, 0.5, 1). Alpha is
 * untouched.
 *
 * The gradients are the Sobel pair of the Edge Detect (a full-range step reads as one), edges
 * clamped to the canvas. The intensity multiplies them; with the world units the multiplier is
 * instead the physical slope: the height delta's centimetres against the texel's own, so the same
 * surface laid at another resolution keeps its normal.
 */
static void paint_layer_effect_height_to_normal_apply(ImBuf &buffer,
                                                      const MaterialPaintLayer &effect)
{
  const float strength = paint_layer_kind_param_get(effect, PAINT_LAYER_HEIGHT_TO_NORMAL_STRENGTH);
  const bool world_units = paint_layer_kind_param_bool_get(
      effect, PAINT_LAYER_HEIGHT_TO_NORMAL_WORLD_UNITS);
  const float surface_size = paint_layer_kind_param_get(effect,
                                                        PAINT_LAYER_HEIGHT_TO_NORMAL_SURFACE_SIZE);
  const float height_depth = paint_layer_kind_param_get(effect,
                                                        PAINT_LAYER_HEIGHT_TO_NORMAL_HEIGHT_DEPTH);
  const bool flip_green = paint_layer_kind_param_bool_get(effect,
                                                          PAINT_LAYER_HEIGHT_TO_NORMAL_FLIP_GREEN);

  const int width = buffer.x;
  const int height = buffer.y;
  if (width <= 0 || height <= 0) {
    return;
  }
  std::shared_ptr<ImBuf> source = paint_layer_anchor_slice_buffer_clone(buffer);
  if (source == nullptr) {
    return;
  }
  const float *src = source->float_buffer.data;
  float *pixels = buffer.float_data_for_write();
  /* The kernel sums its side to four, so the gradient is twice the true slope per texel: the
   * half turns the world units' slope into that scale. Without a surface or a depth the world
   * units name no slope, and the normal stays flat. */
  float scale = strength;
  if (world_units) {
    const float texel_cm = surface_size / float(std::max(width, height));
    scale = (texel_cm > 0.0f) ? 0.5f * height_depth / texel_cm : 0.0f;
  }
  const float green_sign = flip_green ? 1.0f : -1.0f;
  /* Where the Height row has no coverage the map holds no relief, which reads as the neutral
   * height: its straight colour there is not a height, so the alpha weighs it in. Without it a
   * stroke's edge would read as a cliff. */
  const auto height_at = [src](const int64_t texel) {
    const float *p = src + texel * 4;
    return PAINT_HEIGHT_NEUTRAL + (p[0] - PAINT_HEIGHT_NEUTRAL) * p[3];
  };
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      const int64_t row_lo = int64_t(std::max(y - 1, 0)) * width;
      const int64_t row_mid = int64_t(y) * width;
      const int64_t row_hi = int64_t(std::min(y + 1, height - 1)) * width;
      for (const int x : IndexRange(width)) {
        const int col_lf = std::max(x - 1, 0);
        const int col_rt = std::min(x + 1, width - 1);
        const float h00 = height_at(row_lo + col_lf);
        const float h01 = height_at(row_lo + x);
        const float h02 = height_at(row_lo + col_rt);
        const float h10 = height_at(row_mid + col_lf);
        const float h12 = height_at(row_mid + col_rt);
        const float h20 = height_at(row_hi + col_lf);
        const float h21 = height_at(row_hi + x);
        const float h22 = height_at(row_hi + col_rt);
        /* Row y + 1 is the higher V, so the slopes are the height rising along +U and +V. */
        const float dh_du = ((h02 + 2.0f * h12 + h22) - (h00 + 2.0f * h10 + h20)) * 0.25f;
        const float dh_dv = ((h20 + 2.0f * h21 + h22) - (h00 + 2.0f * h01 + h02)) * 0.25f;
        /* The normal leans against the slope; the DirectX encoding flips the green. */
        const float nx = -dh_du * scale;
        const float ny = green_sign * dh_dv * scale;
        const float inv_len = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
        float *dst = pixels + (row_mid + x) * 4;
        dst[0] = nx * inv_len * 0.5f + 0.5f;
        dst[1] = ny * inv_len * 0.5f + 0.5f;
        dst[2] = inv_len * 0.5f + 0.5f;
      }
    }
  });
}
/**
 * The Pixelate over \a buffer in place: the map cut into a grid of \a Pixels Amount blocks per
 * side, every block taking one colour -- the whole block read from a private clone, so the average
 * is the one the block had, not the one its neighbour left. The colour is weighted by the alpha, so
 * the black a transparent texel keeps at an island's edge never leaks into an opaque one; a block
 * with no coverage keeps the plain average. Alpha is untouched (a mask grey has alpha one, so it
 * averages plainly). A block is at least one texel, and the last one of a side may be shorter.
 */
static void paint_layer_effect_pixelate_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  const int amount = paint_layer_kind_param_int_get(effect, PAINT_LAYER_PIXELATE_AMOUNT);
  const int pixels = std::max(amount, 1);
  const int width = buffer.x;
  const int height = buffer.y;
  if (width <= 0 || height <= 0) {
    return;
  }
  const int block_w = std::max(width / pixels, 1);
  const int block_h = std::max(height / pixels, 1);
  if (block_w <= 1 && block_h <= 1) {
    return;
  }

  std::shared_ptr<ImBuf> source = paint_layer_anchor_slice_buffer_clone(buffer);
  if (source == nullptr) {
    return;
  }
  const float *src = source->float_buffer.data;
  float *dst_pixels = buffer.float_data_for_write();
  /* Each block is summed once and its colour written to all of its texels: summing the block again
   * for every texel would cost the block's area per texel, hours on a 4K map with big blocks. The
   * blocks are independent (they read the clone and write their own texels), so one block is one
   * unit of work. */
  const int blocks_x = (width + block_w - 1) / block_w;
  const int blocks_y = (height + block_h - 1) / block_h;
  threading::parallel_for(IndexRange(int64_t(blocks_x) * blocks_y), 4, [&](const IndexRange range) {
    for (const int64_t block : range) {
      const int bx = int(block % blocks_x) * block_w;
      const int by = int(block / blocks_x) * block_h;
      const int bx_end = std::min(bx + block_w, width);
      const int by_end = std::min(by + block_h, height);
      float weighted[3] = {0.0f, 0.0f, 0.0f};
      float plain[3] = {0.0f, 0.0f, 0.0f};
      float alpha_sum = 0.0f;
      for (const int sy : IndexRange(by, by_end - by)) {
        const float *row = src + int64_t(sy) * width * 4;
        for (const int sx : IndexRange(bx, bx_end - bx)) {
          const float *p = row + int64_t(sx) * 4;
          for (const int c : IndexRange(3)) {
            weighted[c] += p[c] * p[3];
            plain[c] += p[c];
          }
          alpha_sum += p[3];
        }
      }
      const float count = float((bx_end - bx) * (by_end - by));
      float color[3];
      for (const int c : IndexRange(3)) {
        color[c] = (alpha_sum > 1e-6f) ? weighted[c] / alpha_sum : plain[c] / count;
      }
      for (const int sy : IndexRange(by, by_end - by)) {
        float *dst = dst_pixels + (int64_t(sy) * width + bx) * 4;
        for (const int sx : IndexRange(bx_end - bx)) {
          dst[int64_t(sx) * 4 + 0] = color[0];
          dst[int64_t(sx) * 4 + 1] = color[1];
          dst[int64_t(sx) * 4 + 2] = color[2];
        }
      }
    }
  });
}

/**
 * The FXAA over \a buffer in place: the single-pass luminance-driven edge blend. The nine lumas
 * name an edge, the gradient of the luminance across the ring names the direction the edge runs
 * at, and the two ring texels that direction walks to blend with the centre -- the jag smoothed
 * along its own direction, not across it. A ring under the contrast threshold reads as it is.
 * The colour is weighted by each texel's alpha, so the black colour under a transparent island
 * never bleeds into its border as a dark fringe. Alpha is untouched.
 */
static void paint_layer_effect_fxaa_apply(ImBuf &buffer, const MaterialPaintLayer & /*effect*/)
{
  std::shared_ptr<ImBuf> source = paint_layer_anchor_slice_buffer_clone(buffer);
  if (source == nullptr) {
    return;
  }
  const int width = buffer.x;
  const int height = buffer.y;
  const float *src = source->float_buffer.data;
  float *pixels = buffer.float_data_for_write();
  /* The edge threshold the console variant reads: under it the ring is no edge, whatever the
   * direction its gradient names. */
  constexpr float EDGE_THRESHOLD = 0.0618f;
  const auto texel_at = [&](const int x, const int y) {
    return src + (int64_t(std::clamp(y, 0, height - 1)) * width + std::clamp(x, 0, width - 1)) * 4;
  };
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        const float *center = texel_at(x, y);
        if (center[3] <= 0.0f) {
          continue;
        }
        const float luma_nw = IMB_colormanagement_get_luminance(texel_at(x - 1, y - 1));
        const float luma_n = IMB_colormanagement_get_luminance(texel_at(x, y - 1));
        const float luma_ne = IMB_colormanagement_get_luminance(texel_at(x + 1, y - 1));
        const float luma_w = IMB_colormanagement_get_luminance(texel_at(x - 1, y));
        const float luma_m = IMB_colormanagement_get_luminance(center);
        const float luma_e = IMB_colormanagement_get_luminance(texel_at(x + 1, y));
        const float luma_sw = IMB_colormanagement_get_luminance(texel_at(x - 1, y + 1));
        const float luma_s = IMB_colormanagement_get_luminance(texel_at(x, y + 1));
        const float luma_se = IMB_colormanagement_get_luminance(texel_at(x + 1, y + 1));
        const float highest = std::max(
            std::max(std::max(luma_nw, luma_n), std::max(luma_ne, luma_w)),
            std::max(std::max(luma_m, luma_e), std::max(std::max(luma_sw, luma_s), luma_se)));
        const float lowest = std::min(
            std::min(std::min(luma_nw, luma_n), std::min(luma_ne, luma_w)),
            std::min(std::min(luma_m, luma_e), std::min(std::min(luma_sw, luma_s), luma_se)));
        if (highest - lowest < EDGE_THRESHOLD) {
          continue;
        }
        /* The luminance gradient across the ring, turned a quarter to run along the edge. */
        const float dir_x = -((luma_nw + luma_n + luma_ne) - (luma_sw + luma_s + luma_se));
        const float dir_y = ((luma_nw + luma_w + luma_sw) - (luma_ne + luma_e + luma_se));
        const float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
        if (len < 1e-6f) {
          continue;
        }
        /* The ring texel the unit direction points at, rounded half up, the project rule. */
        const int step_x = int(std::floor(dir_x / len + 0.5f));
        const int step_y = int(std::floor(dir_y / len + 0.5f));
        const float *along = texel_at(x + step_x, y + step_y);
        const float *against = texel_at(x - step_x, y - step_y);
        const float weight_center = 2.0f * center[3];
        const float weight_along = along[3];
        const float weight_against = against[3];
        const float weight_sum = weight_center + weight_along + weight_against;
        float *dst = pixels + (int64_t(y) * width + x) * 4;
        for (const int c : IndexRange(3)) {
          dst[c] = (center[c] * weight_center + along[c] * weight_along +
                    against[c] * weight_against) /
                   weight_sum;
        }
      }
    }
  });
}
/**
 * The smooth value noise the Blur Slope reads its driver from: the texel hash's lattice,
 * interpolated bilinearly about the smoothstep, so what it slopes is a slope a gradient can read.
 * The seed rides the hash, so a different seed is a different pattern of the same kind.
 */
static float paint_layer_effect_value_noise(const float x, const float y, const int seed)
{
  const int ix = int(std::floor(x));
  const int iy = int(std::floor(y));
  const float fx = x - float(ix);
  const float fy = y - float(iy);
  const float sx = fx * fx * (3.0f - 2.0f * fx);
  const float sy = fy * fy * (3.0f - 2.0f * fy);
  const float a = paint_layer_texel_hash_uniform(ix, iy, seed, 48739u);
  const float b = paint_layer_texel_hash_uniform(ix + 1, iy, seed, 48739u);
  const float c = paint_layer_texel_hash_uniform(ix, iy + 1, seed, 48739u);
  const float d = paint_layer_texel_hash_uniform(ix + 1, iy + 1, seed, 48739u);
  return (a * (1.0f - sx) + b * sx) * (1.0f - sy) + (c * (1.0f - sx) + d * sx) * sy;
}

/**
 * The driver the Blur Slope reads: the source mode's noise or the input's own luminance, blurred
 * and re-graded -- one float per texel, smooth enough that its gradient is the slope the walk
 * follows. The position shifts the midpoint, the contrast scales it about the half; the blur rides
 * only on the input, whose own detail is what a blur has anything to soften. There is no separate
 * input: the "previous input" is the content the row is laid over, its luminance gradient.
 */
static bool paint_layer_effect_slope_source_build(const ImBuf &buffer,
                                                  const int source_mode,
                                                  const int seed,
                                                  const int source_blur,
                                                  const float source_position,
                                                  const float source_contrast,
                                                  const float source_tilling,
                                                  Vector<float> &r_source)
{
  const int width = buffer.x;
  const int height = buffer.y;
  r_source.reinitialize(int64_t(width) * height);
  if (source_mode == PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT) {
    std::shared_ptr<ImBuf> clone = paint_layer_anchor_slice_buffer_clone(buffer);
    if (clone == nullptr) {
      return false;
    }
    if (source_blur > 0) {
      paint_layer_effect_blur_box(*clone, source_blur);
    }
    const float *data = clone->float_buffer.data;
    threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
      for (const int y : range) {
        for (const int x : IndexRange(width)) {
          const int64_t i = int64_t(y) * width + x;
          const float value = IMB_colormanagement_get_luminance(data + i * 4) + source_position;
          r_source[i] = (value - 0.5f) * source_contrast + 0.5f;
        }
      }
    });
    return true;
  }
  /* The noise: the tiles the tilling names across the map, eight lattice cells to a tile, so the
   * pattern the seed names has a slope worth following at any size. */
  const float tiles = std::max(source_tilling, 0.25f);
  threading::parallel_for(IndexRange(height), 16, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        const float nx = (float(x) / float(width) * tiles + float(seed % 7) * 0.113f) * 8.0f;
        const float ny = (float(y) / float(height) * tiles + float(seed % 13) * 0.179f) * 8.0f;
        const float value = paint_layer_effect_value_noise(nx, ny, seed) + source_position;
        r_source[int64_t(y) * width + x] = (value - 0.5f) * source_contrast + 0.5f;
      }
    }
  });
  return true;
}

/**
 * The Blur Slope over \a buffer in place: the samples the quality names walk the gradient's
 * direction of the driver, spanning the intensity (in texels of the map), and the blending mode
 * says what the walk accumulates -- the average, the minimum that eats the edges away, or the
 * maximum that smears the white out. A texel whose slope is flat reads as it is. The taps read a
 * private clone, nearest and clamped, and are weighted by their alpha so the black colour under a
 * transparent island never darkens a border; alpha is untouched. The cost is the quality's taps
 * per texel (at most 32), one gradient read per texel; the rows run in parallel, each texel
 * written from the clone alone, so the result does not depend on the thread order.
 */
static void paint_layer_effect_blur_slope_apply(ImBuf &buffer, const MaterialPaintLayer &effect)
{
  const int seed = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BLUR_SLOPE_SEED);
  const float intensity = paint_layer_kind_param_get(effect, PAINT_LAYER_BLUR_SLOPE_INTENSITY);
  const int quality = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BLUR_SLOPE_QUALITY);
  const int blending = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BLUR_SLOPE_BLENDING);
  const int source_mode = paint_layer_kind_param_int_get(effect,
                                                         PAINT_LAYER_BLUR_SLOPE_SOURCE_MODE);
  const int source_blur = paint_layer_kind_param_int_get(effect,
                                                         PAINT_LAYER_BLUR_SLOPE_SOURCE_BLUR);
  const float source_position = paint_layer_kind_param_get(
      effect, PAINT_LAYER_BLUR_SLOPE_SOURCE_POSITION);
  const float source_contrast = paint_layer_kind_param_get(
      effect, PAINT_LAYER_BLUR_SLOPE_SOURCE_CONTRAST);
  const float source_tilling = paint_layer_kind_param_get(effect,
                                                          PAINT_LAYER_BLUR_SLOPE_SOURCE_TILLING);
  const int samples = std::clamp(quality, 1, 32);
  if (intensity <= 0.0f || samples <= 1) {
    /* One sample reads the centre: the walk has no span to cover. */
    return;
  }

  Vector<float> source;
  if (!paint_layer_effect_slope_source_build(buffer,
                                             source_mode,
                                             seed,
                                             source_blur,
                                             source_position,
                                             source_contrast,
                                             source_tilling,
                                             source))
  {
    return;
  }
  std::shared_ptr<ImBuf> clone = paint_layer_anchor_slice_buffer_clone(buffer);
  if (clone == nullptr) {
    return;
  }
  const int width = buffer.x;
  const int height = buffer.y;
  const float *src = clone->float_buffer.data;
  const bool take_min = (blending == PAINT_LAYER_BLUR_SLOPE_BLENDING_MIN);
  const bool take_max = (blending == PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX);
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 32, [&](const IndexRange range) {
    for (const int y : range) {
      const int yu = std::max(y - 1, 0);
      const int yd = std::min(y + 1, height - 1);
      for (const int x : IndexRange(width)) {
        const int xl = std::max(x - 1, 0);
        const int xr = std::min(x + 1, width - 1);
        const float gx = (source[int64_t(y) * width + xr] - source[int64_t(y) * width + xl]) * 0.5f;
        const float gy = (source[int64_t(yd) * width + x] - source[int64_t(yu) * width + x]) * 0.5f;
        const float slope = std::sqrt(gx * gx + gy * gy);
        if (slope < 1e-4f) {
          continue;
        }
        /* The walk: the slope's direction, the intensity's span, the samples from one end of it
         * to the other. */
        const float dir_x = gx / slope;
        const float dir_y = gy / slope;
        float result[3] = {0.0f, 0.0f, 0.0f};
        float weight_sum = 0.0f;
        bool any = false;
        for (const int k : IndexRange(samples)) {
          const float t = (float(k) / float(samples - 1) - 0.5f) * intensity;
          const int sx = std::clamp(int(std::floor(float(x) + dir_x * t + 0.5f)), 0, width - 1);
          const int sy = std::clamp(int(std::floor(float(y) + dir_y * t + 0.5f)), 0, height - 1);
          const float *texel = src + (int64_t(sy) * width + sx) * 4;
          const float alpha = texel[3];
          if (alpha <= 0.0f) {
            continue;
          }
          if (take_min || take_max) {
            for (const int c : IndexRange(3)) {
              result[c] = !any ? texel[c] :
                          take_min ? std::min(result[c], texel[c]) :
                                     std::max(result[c], texel[c]);
            }
            any = true;
          }
          else {
            result[0] += texel[0] * alpha;
            result[1] += texel[1] * alpha;
            result[2] += texel[2] * alpha;
            weight_sum += alpha;
          }
        }
        float *dst = pixels + (int64_t(y) * width + x) * 4;
        if (take_min || take_max) {
          if (any) {
            dst[0] = result[0];
            dst[1] = result[1];
            dst[2] = result[2];
          }
        }
        else if (weight_sum > 0.0f) {
          const float inv = 1.0f / weight_sum;
          dst[0] = result[0] * inv;
          dst[1] = result[1] * inv;
          dst[2] = result[2] * inv;
        }
      }
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
 * angular one is the chessboard walk, which keeps corners square. Both are the shared
 * #paint_layer_effect_chamfer_walk.
 */
static void paint_layer_effect_bevel_apply(ImBuf &buffer,
                                              const MaterialPaintLayer &effect,
                                              const bool for_mask)
{
  const int distance = paint_layer_kind_param_int_get(effect, PAINT_LAYER_BEVEL_DISTANCE);
  /* A mask's grey carries no alpha (it is one everywhere), so its border is always the luminance;
   * the alpha source would bevel nothing and lay a solid one. */
  const bool luminance = for_mask || paint_layer_kind_param_int_get(
                                         effect, PAINT_LAYER_BEVEL_SOURCE) ==
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
  /* The buffer's edge is not a border: the walk reads a neighbour past it as the edge texel. */
  paint_layer_effect_chamfer_walk(dist, width, height, side, diagonal);

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

/**
 * The Channel Split: one channel of the row's slot image (an ORM map, say) laid as a grey in the
 * RGB of every texel. A content effect lays a layer of its own (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA):
 * the map covers fully, so the grey shows on a row that has no paint yet; a mask item keeps the
 * input alpha, its grey carries none of its own. The image is sampled at the texel's own UV, so
 * it stretches over the map whatever its resolution, the way a mask item's map is read. An empty
 * slot, or one with no readable pixels, leaves the buffer as it is: the effect is the identity
 * while no image is assigned. The value is written as it is, the way the pointwise filters write
 * the channels they correct, and the kind table needs no luminance pass back from a mask form:
 * the extraction is one value already.
 */
static void paint_layer_effect_channel_split_apply(ImBuf &buffer,
                                                   const MaterialPaintLayer &effect,
                                                   const int tile_number,
                                                   const bool lays_layer)
{
  Image *slot = paint_layer_effect_slot_image(effect);
  if (slot == nullptr) {
    return;
  }
  /* The same tile the buffer stands for: an UDIM slot is sampled on the tile the map was computed
   * on, and a plain image answers whatever tile is asked. The acquire decodes into a float RGBA
   * buffer of the image's own size, so the resample below is the only scale the split applies. It
   * reads the slot as data, without retagging the image or its other users as Non-Color. */
  bke::paint_perf::ScopedTimer perf("effect.cpu.channel_split");
  perf.note("kind=%d size=%dx%d", int(effect.kind_subtype), buffer.x, buffer.y);
  ImBuf *map = BKE_paint_material_mask_map_acquire_data(slot, tile_number, nullptr, 0, 0);
  if (map == nullptr || map->float_data() == nullptr || map->x <= 0 || map->y <= 0) {
    if (map != nullptr) {
      IMB_freeImBuf(map);
    }
    return;
  }
  const int component = std::clamp(
      paint_layer_kind_param_int_get(effect, PAINT_LAYER_CHANNEL_SPLIT_CHANNEL),
      int(PAINT_LAYER_CHANNEL_SPLIT_RED),
      int(PAINT_LAYER_CHANNEL_SPLIT_ALPHA));
  const float *data = map->float_data();
  const int width = buffer.x;
  const int height = buffer.y;
  float *pixels = buffer.float_data_for_write();
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      const float sv = ((float(y) + 0.5f) / float(height)) * float(map->y) - 0.5f;
      float *dst = pixels + int64_t(y) * width * 4;
      for (const int x : IndexRange(width)) {
        const float su = ((float(x) + 0.5f) / float(width)) * float(map->x) - 0.5f;
        const float4 sample = math::interpolate_bilinear_fl(data, map->x, map->y, su, sv);
        const float value = sample[component];
        dst[int64_t(x) * 4 + 0] = value;
        dst[int64_t(x) * 4 + 1] = value;
        dst[int64_t(x) * 4 + 2] = value;
        if (lays_layer) {
          dst[int64_t(x) * 4 + 3] = 1.0f;
        }
      }
    }
  });
  IMB_freeImBuf(map);
}

/* -------------------------------------------------------------------- */
/** \name Normal to Mask
 * \{ */

/** The sweeps the coarsest level of the Height solve runs: at most a 64x64 canvas, so the whole
 * low-frequency half of the field costs less than one fine sweep of a 4K map. */
constexpr int PAINT_LAYER_NORMAL_TO_MASK_COARSE_SWEEPS = 400;
/** The sweeps every finer level of the Height solve runs over the stretched coarse solution. */
constexpr int PAINT_LAYER_NORMAL_TO_MASK_LEVEL_SWEEPS = 24;
/** The size the Height solve's pyramid stops coarsening at: both dimensions of the coarsest
 * canvas stay at or under it. */
constexpr int PAINT_LAYER_NORMAL_TO_MASK_COARSE_SIZE = 64;

/** The decoded tangent normal at \a x, \a y: the encoding the Normal channel carries, read as
 * numbers the way a Normal Convert reads them, each component clamped into [-1, 1] so an
 * unnormalized or HDR-encoded map cannot push the field out of the range its cut expects. The
 * read is clamped to the canvas edge, so a border texel repeats its edge neighbour the way a
 * blur pass's window does. */
static float3 paint_layer_effect_normal_at(const float *src,
                                           const int width,
                                           const int height,
                                           const int x,
                                           const int y)
{
  const float *texel = src + (int64_t(std::clamp(y, 0, height - 1)) * width +
                              std::clamp(x, 0, width - 1)) *
                                 4;
  /* `std::clamp` hands a NaN through, and in the Height mode one broken texel would spread it over
   * the whole Poisson solve. A component that is not a finite number reads as the flat normal's,
   * so a broken texel cuts as no signal instead. */
  const auto decode = [](const float value, const float flat) {
    return std::isfinite(value) ? std::clamp(value * 2.0f - 1.0f, -1.0f, 1.0f) : flat;
  };
  return float3(decode(texel[0], 0.0f), decode(texel[1], 0.0f), decode(texel[2], 1.0f));
}

/** The divergence of the normal's XY, the right side of the Poisson equation the Height mode
 * solves: how much the tangent normal's tilt spreads out of each texel, `d(nx)/dx + d(ny)/dy`
 * by central differences clamped to the canvas edge.
 *
 * The sweeps reflect at the border (a zero slope), which alone would throw away the tilt the
 * normal carries at the edge: the integration would bend the height into a bowl and a ramp would
 * come out lower where it climbs. The border cells therefore also take the flux the edge normal
 * pushes through the border -- the slope there is the normal, not zero -- which makes the system
 * compatible (the divergence sums to nothing) and lets a ramp integrate as a ramp. */
static void paint_layer_effect_normal_divergence(const float *src,
                                                 const int width,
                                                 const int height,
                                                 const MutableSpan<float> r_div)
{
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        const float3 left = paint_layer_effect_normal_at(src, width, height, x - 1, y);
        const float3 right = paint_layer_effect_normal_at(src, width, height, x + 1, y);
        const float3 up = paint_layer_effect_normal_at(src, width, height, x, y - 1);
        const float3 down = paint_layer_effect_normal_at(src, width, height, x, y + 1);
        float divergence = (right.x - left.x) * 0.5f + (down.y - up.y) * 0.5f;
        const float3 center = paint_layer_effect_normal_at(src, width, height, x, y);
        if (x == 0) {
          divergence += center.x;
        }
        if (x == width - 1) {
          divergence -= center.x;
        }
        if (y == 0) {
          divergence += center.y;
        }
        if (y == height - 1) {
          divergence -= center.y;
        }
        r_div[int64_t(y) * width + x] = divergence;
      }
    }
  });
}

/** One level of the Height solve's pyramid: the divergence averaged over 2x2 blocks, the halved
 * canvas carrying the field's low frequencies the fine sweeps crawl towards. An odd tail row or
 * column averages the texels it has, the clamped reads keeping every level the same shape its
 * half-resolution canvas names. */
static void paint_layer_effect_divergence_coarsen(const Span<float> fine,
                                                  const int width,
                                                  const int height,
                                                  Vector<float> &r_coarse,
                                                  int &r_coarse_width,
                                                  int &r_coarse_height)
{
  r_coarse_width = std::max(width / 2, 1);
  r_coarse_height = std::max(height / 2, 1);
  r_coarse.reinitialize(int64_t(r_coarse_width) * r_coarse_height);
  threading::parallel_for(IndexRange(r_coarse_height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(r_coarse_width)) {
        const int x0 = x * 2;
        const int y0 = y * 2;
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        r_coarse[int64_t(y) * r_coarse_width + x] = 0.25f *
                                                    (fine[int64_t(y0) * width + x0] +
                                                     fine[int64_t(y0) * width + x1] +
                                                     fine[int64_t(y1) * width + x0] +
                                                     fine[int64_t(y1) * width + x1]);
      }
    }
  });
}

/** One Jacobi relaxation sweep of the Height solve: every texel becomes the mean of its four
 * neighbours minus a quarter of the divergence it carries. The reads run on \a current and the
 * writes on \a next, so a sweep is a pure step the threads cannot race; the edge reads clamp,
 * which makes the canvas a reflecting border and leaves the solve with no artificial dark frame
 * a zero boundary would hang around the content. */
static void paint_layer_effect_poisson_sweep(const float *current,
                                             float *next,
                                             const float *divergence,
                                             const int width,
                                             const int height)
{
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      const float *row = current + int64_t(y) * width;
      float *dst = next + int64_t(y) * width;
      const float *above = current + int64_t(std::max(y - 1, 0)) * width;
      const float *below = current + int64_t(std::min(y + 1, height - 1)) * width;
      for (const int x : IndexRange(width)) {
        const float l = row[std::max(x - 1, 0)];
        const float r = row[std::min(x + 1, width - 1)];
        dst[x] = (l + r + above[x] + below[x] - divergence[int64_t(y) * width + x]) * 0.25f;
      }
    }
  });
}

/** The coarse solution bilinearly stretched over the finer level: the warm start the level's own
 * sweeps refine. The fine texel's center maps to the coarse canvas at half rate, the reads
 * clamped into it. */
static void paint_layer_effect_poisson_prolong(const Span<float> coarse,
                                               const int coarse_width,
                                               const int coarse_height,
                                               const MutableSpan<float> fine,
                                               const int width,
                                               const int height)
{
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      const float fy = (float(y) + 0.5f) * 0.5f - 0.5f;
      const int y0 = int(std::floor(fy));
      const float ty = fy - float(y0);
      const int cy0 = std::clamp(y0, 0, coarse_height - 1);
      const int cy1 = std::clamp(y0 + 1, 0, coarse_height - 1);
      for (const int x : IndexRange(width)) {
        const float fx = (float(x) + 0.5f) * 0.5f - 0.5f;
        const int x0 = int(std::floor(fx));
        const float tx = fx - float(x0);
        const int cx0 = std::clamp(x0, 0, coarse_width - 1);
        const int cx1 = std::clamp(x0 + 1, 0, coarse_width - 1);
        const float v00 = coarse[int64_t(cy0) * coarse_width + cx0];
        const float v10 = coarse[int64_t(cy0) * coarse_width + cx1];
        const float v01 = coarse[int64_t(cy1) * coarse_width + cx0];
        const float v11 = coarse[int64_t(cy1) * coarse_width + cx1];
        const float top = v00 + (v10 - v00) * tx;
        const float bottom = v01 + (v11 - v01) * tx;
        fine[int64_t(y) * width + x] = top + (bottom - top) * ty;
      }
    }
  });
}

/** The [0, 1] window the field maps into before its cut: the 1st and the 99th percentile of the
 * field's values, found over a histogram whose bins are exact integer counts, so neither the
 * parallel extents above nor the scan order can move them. Non-finite values (a broken normal's
 * NaN read through the differences) stand out of every window: they are skipped, and a field of
 * nothing but them answers the degenerate window, so the cut reads black instead of converting
 * the garbage into a bin the histogram cannot hold. A constant field (a flat normal carries no
 * divergence to integrate) reports a degenerate window, which the kernel maps as zero; a peaked
 * one where the percentiles collapse onto one bin falls back to the full extent, so a handful of
 * features the percentiles under-read still spans the range the mask reads. */
static void paint_layer_effect_field_window(const Span<float> field, float &r_lo, float &r_hi)
{
  constexpr int BINS = 4096;
  struct FieldExtent {
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
  };
  const FieldExtent extent = threading::parallel_reduce(
      IndexRange(field.size()),
      4096,
      FieldExtent{},
      [&](const IndexRange range, const FieldExtent &running) {
        /* The running extent folds in: the reducer hands a chunk what the ones before it found,
         * and an extent seeded with zero would pull a positive field's minimum down to it. */
        FieldExtent run = running;
        for (const int64_t i : range) {
          if (!std::isfinite(field[i])) {
            continue;
          }
          run.lo = std::min(run.lo, field[i]);
          run.hi = std::max(run.hi, field[i]);
        }
        return run;
      },
      [](const FieldExtent &a, const FieldExtent &b) {
        return FieldExtent{std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
      });
  if (!(extent.hi >= extent.lo) || !std::isfinite(extent.lo)) {
    /* No finite value at all (an empty field, or one of nothing but broken normals): the
     * degenerate window, whatever the field holds, cuts as black. */
    r_lo = 0.0f;
    r_hi = 0.0f;
    return;
  }
  if (!(extent.hi > extent.lo)) {
    r_lo = extent.lo;
    r_hi = extent.lo;
    return;
  }
  Vector<int64_t> bins(BINS, 0);
  const float scale = float(BINS) / (extent.hi - extent.lo);
  for (const int64_t i : field.index_range()) {
    if (!std::isfinite(field[i])) {
      continue;
    }
    bins[std::clamp(int((field[i] - extent.lo) * scale), 0, BINS - 1)]++;
  }
  const int64_t total = field.size();
  /* One percent of the field off either end stands out of the window: the two scans below both
   * walk until they have counted past that share. */
  const int64_t tail_target = total / 100;
  int low_bin = 0;
  int high_bin = BINS - 1;
  int64_t seen = 0;
  for (; low_bin < BINS - 1; low_bin++) {
    seen += bins[low_bin];
    if (seen > tail_target) {
      break;
    }
  }
  seen = 0;
  for (; high_bin > 0; high_bin--) {
    seen += bins[high_bin];
    if (seen > tail_target) {
      break;
    }
  }
  r_lo = extent.lo + float(low_bin) / scale;
  r_hi = extent.lo + float(high_bin) / scale;
  if (!(r_hi > r_lo)) {
    r_lo = extent.lo;
    r_hi = extent.hi;
  }
}

/** The Height field: the divergence of the normal's XY integrated back into a height. The solve
 * is the mode's whole cost, which is why a preview caches the field instead of the kernel's
 * result: the cut is cheap, the pyramid is not. */
static void paint_layer_normal_to_mask_field_height(const float *src,
                                                    const int width,
                                                    const int height,
                                                    Vector<float> &r_field)
{
  /* The divergence pyramid, coarsest solved first. Each finer level stretches the solution
   * above it and refines it, so the whole hierarchy costs a handful of sweeps per level of a
   * few thousand texels each and the fine canvas only answers for the detail. */
  Vector<Vector<float>> pyramid;
  Vector<int> widths;
  Vector<int> heights;
  pyramid.append(Vector<float>(int64_t(width) * height, 0.0f));
  widths.append(width);
  heights.append(height);
  paint_layer_effect_normal_divergence(src, width, height, pyramid[0]);
  while ((widths.last() > PAINT_LAYER_NORMAL_TO_MASK_COARSE_SIZE ||
          heights.last() > PAINT_LAYER_NORMAL_TO_MASK_COARSE_SIZE) &&
         widths.last() > 1 && heights.last() > 1)
  {
    Vector<float> coarse;
    int coarse_width = 0;
    int coarse_height = 0;
    paint_layer_effect_divergence_coarsen(
        pyramid.last(), widths.last(), heights.last(), coarse, coarse_width, coarse_height);
    pyramid.append(std::move(coarse));
    widths.append(coarse_width);
    heights.append(coarse_height);
  }
  /* Two whole-canvas work buffers, sized once: the sweeps ping-pong between them and every level
   * works on the span its own size names. A fresh `fine` and a scratch beside it per level would
   * reallocate the canvas the pyramid walks down, and the peak of a 4K solve would hold the
   * divergence pyramid plus two buffers per step instead of two buffers. */
  Vector<float> solution(int64_t(width) * height, 0.0f);
  Vector<float> scratch(int64_t(width) * height, 0.0f);
  const Span<float> top_divergence(pyramid.last());
  for (int sweep = 0; sweep < PAINT_LAYER_NORMAL_TO_MASK_COARSE_SWEEPS; sweep++) {
    paint_layer_effect_poisson_sweep(solution.data(),
                                     scratch.data(),
                                     top_divergence.data(),
                                     widths.last(),
                                     heights.last());
    std::swap(solution, scratch);
  }
  for (int level = pyramid.size() - 2; level >= 0; level--) {
    const int level_width = widths[level];
    const int level_height = heights[level];
    /* The coarse solution stretches into the other buffer: the prolongation reads the texels a
     * write into the same canvas would race with, so the two hold different levels of the solve. */
    paint_layer_effect_poisson_prolong(
        Span<float>(solution).take_front(int64_t(widths[level + 1]) * heights[level + 1]),
        widths[level + 1],
        heights[level + 1],
        MutableSpan<float>(scratch).take_front(int64_t(level_width) * level_height),
        level_width,
        level_height);
    std::swap(solution, scratch);
    const Span<float> level_divergence(pyramid[level]);
    for (int sweep = 0; sweep < PAINT_LAYER_NORMAL_TO_MASK_LEVEL_SWEEPS; sweep++) {
      paint_layer_effect_poisson_sweep(solution.data(),
                                       scratch.data(),
                                       level_divergence.data(),
                                       level_width,
                                       level_height);
      std::swap(solution, scratch);
    }
  }
  /* The last sweep's swap leaves the fine result in `solution`, whose whole canvas is the level 0
   * span the field is. */
  r_field = std::move(solution);
}

/** The Edges field: the Sobel magnitude of the normal per channel, summed. The borders of the
 * elements light up, their insides stay dark. */
static void paint_layer_normal_to_mask_field_edges(const float *src,
                                                   const int width,
                                                   const int height,
                                                   Vector<float> &r_field)
{
  r_field.reinitialize(int64_t(width) * height);
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        float3 taps[3][3];
        for (const int dy : IndexRange(-1, 3)) {
          for (const int dx : IndexRange(-1, 3)) {
            taps[dy + 1][dx + 1] = paint_layer_effect_normal_at(
                src, width, height, x + dx, y + dy);
          }
        }
        float sum = 0.0f;
        for (const int c : IndexRange(3)) {
          const float gx = (taps[2][0][c] + 2.0f * taps[2][1][c] + taps[2][2][c]) -
                           (taps[0][0][c] + 2.0f * taps[0][1][c] + taps[0][2][c]);
          const float gy = (taps[0][2][c] + 2.0f * taps[1][2][c] + taps[2][2][c]) -
                           (taps[0][0][c] + 2.0f * taps[1][0][c] + taps[2][0][c]);
          sum += std::sqrt(gx * gx + gy * gy);
        }
        r_field[int64_t(y) * width + x] = sum;
      }
    }
  });
}

/** The Slope field: the blue channel's tilt, `1 - z`. Flat areas read dark, tilted ones bright. */
static void paint_layer_normal_to_mask_field_slope(const float *src,
                                                   const int width,
                                                   const int height,
                                                   Vector<float> &r_field)
{
  r_field.reinitialize(int64_t(width) * height);
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange range) {
    for (const int y : range) {
      for (const int x : IndexRange(width)) {
        const float3 normal = paint_layer_effect_normal_at(src, width, height, x, y);
        r_field[int64_t(y) * width + x] = std::clamp(1.0f - normal.z, 0.0f, 1.0f);
      }
    }
  });
}

/** The compute of one field mode: the encoded normal in, the texel field out. */
using PaintLayerNormalToMaskFieldFn = void (*)(const float *src,
                                               const int width,
                                               const int height,
                                               Vector<float> &r_field);

struct PaintLayerNormalToMaskFieldMode {
  PaintLayerNormalToMaskMode mode;
  PaintLayerNormalToMaskFieldFn compute;
};

/** The modes the field dispatches through: a fourth field is one function above and one row
 * here, where a switch sat inside the kernel for every one of its consumers to read around. */
constexpr PaintLayerNormalToMaskFieldMode PAINT_LAYER_NORMAL_TO_MASK_FIELD_MODES[] = {
    {PAINT_LAYER_NORMAL_TO_MASK_HEIGHT, paint_layer_normal_to_mask_field_height},
    {PAINT_LAYER_NORMAL_TO_MASK_EDGES, paint_layer_normal_to_mask_field_edges},
    {PAINT_LAYER_NORMAL_TO_MASK_SLOPE, paint_layer_normal_to_mask_field_slope},
};

/**
 * The field \a mode names, rebuilt from the encoded tangent normal in \a src: one value per
 * texel of the \a width x \a height canvas into \a r_field. The mode a file may name anything is
 * dispatched through the table, an unknown one standing in as the Height, the same answer the
 * parameter read gives.
 */
static void paint_layer_normal_to_mask_field_compute(const float *src,
                                                     const int width,
                                                     const int height,
                                                     const PaintLayerNormalToMaskMode mode,
                                                     Vector<float> &r_field)
{
  r_field.reinitialize(int64_t(width) * height);
  for (const PaintLayerNormalToMaskFieldMode &entry : PAINT_LAYER_NORMAL_TO_MASK_FIELD_MODES) {
    if (entry.mode == mode) {
      entry.compute(src, width, height, r_field);
      return;
    }
  }
  PAINT_LAYER_NORMAL_TO_MASK_FIELD_MODES[0].compute(src, width, height, r_field);
}

/**
 * The Normal to Mask field of \a slice: the encoded tangent normal it carries rebuilt into the
 * field \a params name, one value per texel into \a r_field, with the robust [0, 1] window the
 * field maps into before its cut. The preview session calls this once per mode change and the
 * cut on every repaint, so a threshold drag never reruns the solve.
 */
void BKE_paint_layers_normal_to_mask_field_compute(const ImBuf &slice,
                                                   const PaintLayerNormalToMaskParams &params,
                                                   Vector<float> &r_field,
                                                   float &r_lo,
                                                   float &r_hi)
{
  const float *src = slice.float_buffer.data;
  if (src == nullptr) {
    r_field.clear();
    r_lo = 0.0f;
    r_hi = 1.0f;
    return;
  }
  paint_layer_normal_to_mask_field_compute(
      src, slice.x, slice.y, PaintLayerNormalToMaskMode(params.mode), r_field);
  paint_layer_effect_field_window(r_field, r_lo, r_hi);
}

/**
 * The cut of the Normal to Mask: the field -- normalized by the window \a lo, \a hi its solve
 * reported -- through the ramp \a threshold and \a softness lay, over the RGB of \a buffer in
 * place. Alpha is untouched. A texel the field answers with a non-finite number (a broken
 * normal's NaN read through the differences) cuts as no signal, black, instead of writing the
 * NaN into the mask.
 */
static void paint_layer_normal_to_mask_cut(ImBuf &buffer,
                                           const Span<float> field,
                                           const float lo,
                                           const float hi,
                                           const float threshold,
                                           const float softness,
                                           const bool lays_layer = false)
{
  const int width = buffer.x;
  /* The window never narrows below a floor: an 8-bit "flat" normal is not exactly flat (128 reads
   * 0.0039), and its sliver of tilt would otherwise be stretched to the full range and cut as if
   * it were a feature. A field whose whole extent sits under the floor cuts dark. */
  constexpr float MIN_WINDOW_SPAN = 0.25f;
  const float span = std::max(hi - lo, MIN_WINDOW_SPAN);
  paint_layer_effect_map_rgb(buffer, [&](const int x, const int y, float *texel) {
    const float raw = field[int64_t(y) * width + x];
    const float value = std::isfinite(raw) ? std::clamp((raw - lo) / span, 0.0f, 1.0f) : 0.0f;
    const float grey = paint_layer_normal_to_mask_ramp_apply(value, threshold, softness);
    texel[0] = grey;
    texel[1] = grey;
    texel[2] = grey;
    if (lays_layer) {
      texel[3] = 1.0f;
    }
  });
}

/**
 * The Normal to Mask cut over \a buffer, exposed for the preview session: the field computed
 * over \a buffer's own size, cut with the parameters \a params name. The one formula the CPU
 * kernel and the preview run (#paint_layer_normal_to_mask_ramp_apply), so a preview cannot
 * drift from what the bake lays.
 */
void BKE_paint_layers_normal_to_mask_cut_apply(ImBuf &buffer,
                                               const Span<float> field,
                                               const float lo,
                                               const float hi,
                                               const PaintLayerNormalToMaskParams &params)
{
  paint_layer_normal_to_mask_cut(buffer, field, lo, hi, params.threshold, params.softness);
}

/**
 * The Normal to Mask kernel over \a buffer in place: the tangent normal the RGB carries is read
 * as numbers, rebuilt into the field \a params name, and cut into the grey the ramp lays. Alpha
 * is untouched.
 *
 * Every field is normalized by its robust range (the 1st and the 99th percentiles), so outliers
 * cannot stretch the range the cut reads, and a flat normal collapses onto a black mask in every
 * mode. The polarity follows the normal's tilt, and the mask item's own invert flips it.
 */
static void paint_layer_effect_normal_to_mask(ImBuf &buffer,
                                              const PaintLayerNormalToMaskParams &params,
                                              const bool lays_layer)
{
  const int width = buffer.x;
  const int height = buffer.y;
  const float *src = buffer.float_buffer.data;
  if (src == nullptr) {
    return;
  }
  bke::paint_perf::ScopedTimer perf("effect.cpu.normal_to_mask");
  perf.note("kind=%d mode=%d size=%dx%d",
            int(MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK),
            params.mode,
            width,
            height);
  Vector<float> field;
  paint_layer_normal_to_mask_field_compute(
      src, width, height, PaintLayerNormalToMaskMode(params.mode), field);
  float lo = 0.0f;
  float hi = 1.0f;
  paint_layer_effect_field_window(field, lo, hi);
  paint_layer_normal_to_mask_cut(
      buffer, field, lo, hi, params.threshold, params.softness, lays_layer);
}

/** \} */

void paint_layer_effect_kernel_apply(ImBuf &buffer,
                                     const MaterialPaintLayer &effect,
                                     const bool for_mask,
                                     const int tile_number)
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
    case MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL: {
      /* The line average at the angle, softened over the colour by the intensity. */
      paint_layer_effect_blur_directional(
          buffer,
          paint_layer_kind_param_get(effect, PAINT_LAYER_BLUR_DIRECTIONAL_ANGLE),
          paint_layer_kind_param_int_get(effect, PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS),
          paint_layer_kind_param_get(effect, PAINT_LAYER_BLUR_DIRECTIONAL_MIX));
      break;
    }
    case MA_PAINT_LAYER_FILTER_SHARPEN: {
      /* Unsharp masking: the detail the blur removes comes back on top of the colour. */
      const int radius = paint_layer_kind_param_int_get(effect, PAINT_LAYER_SHARPEN_RADIUS);
      const float amount = paint_layer_kind_param_get(effect, PAINT_LAYER_SHARPEN_AMOUNT);
      paint_layer_effect_unsharp_apply(
          buffer, radius, [amount](const float orig, const float blurred) {
            return paint_layer_sharpen_apply(orig, blurred, amount);
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
            return paint_layer_high_pass_apply(orig, blurred, contrast);
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
    case MA_PAINT_LAYER_FILTER_DROP_SHADOW:
      paint_layer_effect_drop_shadow_apply(
          buffer, effect, !for_mask && paint_layer_kind_result_has_alpha(effect));
      break;
    case MA_PAINT_LAYER_FILTER_MASK_OUTLINE:
      paint_layer_effect_mask_outline_apply(
          buffer, effect, !for_mask && paint_layer_kind_result_has_alpha(effect));
      break;
    case MA_PAINT_LAYER_FILTER_FILL_AREA:
      paint_layer_effect_fill_area_apply(
          buffer, effect, !for_mask && paint_layer_kind_result_has_alpha(effect));
      break;
    case MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT:
      paint_layer_effect_channel_split_apply(
          buffer, effect, tile_number, !for_mask && paint_layer_kind_result_has_alpha(effect));
      break;
    case MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL:
      paint_layer_effect_height_to_normal_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_PIXELATE:
      paint_layer_effect_pixelate_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_FXAA:
      paint_layer_effect_fxaa_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_BLUR_SLOPE:
      paint_layer_effect_blur_slope_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_BEVEL:
      paint_layer_effect_bevel_apply(buffer, effect, for_mask);
      break;
    case MA_PAINT_LAYER_FILTER_COLOR_MATCH:
      paint_layer_effect_color_match_apply(buffer, effect);
      break;
    case MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK: {
      /* The normal rebuilt into the field the mode names and cut by the threshold's ramp: the
       * typed read validates the mode into the range the kernel answers for, so a file that
       * carries anything stands in as the Height. */
      PaintLayerNormalToMaskParams params;
      if (BKE_paint_layers_normal_to_mask_params_get(effect, params)) {
        /* As a correction the grey is a layer of its own: it covers where the row laid nothing, so
         * a channel the row has no content in still gets its result. A mask item carries no alpha
         * of its own. */
        paint_layer_effect_normal_to_mask(
            buffer, params, !for_mask && paint_layer_kind_result_has_alpha(effect));
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace blender
