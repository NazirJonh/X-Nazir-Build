/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Runtime, per-UDIM-tile selection masks for 2D image paint. See
 * #BKE_image_paint_selection.hh for the module overview.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "DNA_image_types.h"

#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_image_partial_update.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Mask Storage
 * \{ */

static void paint_selection_blend_mask_tile_invalidate(Image *image, int tile_number);

/** Drop every memoized selection bounding box. Entries are plain values, so this frees them. */
static void paint_selection_bounds_cache_clear(Image *image)
{
  if (image && image->runtime) {
    image->runtime->paint_selection_bounds_cache.clear();
  }
}

/**
 * Mark the selection masks as changed. Called from every path that can alter mask contents,
 * including the one that hands a writable #ImBuf to callers outside this module.
 */
static void paint_selection_revision_bump(Image *image)
{
  if (image && image->runtime) {
    image->runtime->paint_selection_revision++;
    /* The memoized bounding boxes are derived from the mask contents, so they die with it. The
     * revision comparison below would catch this on its own; clearing here also releases the
     * memory instead of leaving stale entries around until the next query. */
    paint_selection_bounds_cache_clear(image);
  }
}

uint64_t BKE_image_paint_selection_mask_revision_get(const Image *image)
{
  if (!image || !image->runtime) {
    return 0;
  }
  return image->runtime->paint_selection_revision;
}

ImBuf *BKE_image_paint_selection_mask_get(Image *image, int tile_number, int width, int height)
{
  bke::ImageRuntime *runtime = image->runtime;
  ImBuf **mask_ptr = runtime->paint_selection_masks.lookup_ptr(tile_number);
  if (mask_ptr) {
    ImBuf *mask = *mask_ptr;
    if (mask->x == width && mask->y == height) {
      /* Same contract as the mutable #BKE_image_paint_selection_mask_lookup overload: a writable
       * buffer is handed out and callers write into it directly, so assume the contents change. */
      paint_selection_revision_bump(image);
      return mask;
    }
    /* Size changed, free old mask. */
    IMB_freeImBuf(mask);
    runtime->paint_selection_masks.remove(tile_number);
    paint_selection_blend_mask_tile_invalidate(image, tile_number);
  }

  ImBuf *mask = IMB_allocImBuf(width, height, ImBufFlags::Zero);
  IMB_alloc_float_pixels(mask, 1);
  memset(mask->float_data_for_write(), 0, size_t(width) * height * sizeof(float));
  runtime->paint_selection_masks.add_new(tile_number, mask);
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
  paint_selection_revision_bump(image);
  return mask;
}

static void paint_selection_blend_mask_tile_invalidate(Image *image, int tile_number)
{
  if (!image || !image->runtime) {
    return;
  }
  bke::ImageRuntime *runtime = image->runtime;
  ImBuf **blend_ptr = runtime->paint_selection_blend_masks.lookup_ptr(tile_number);
  if (blend_ptr) {
    if (*blend_ptr) {
      IMB_freeImBuf(*blend_ptr);
    }
    runtime->paint_selection_blend_masks.remove(tile_number);
  }
}

void BKE_image_paint_selection_blend_mask_invalidate_tile(Image *image, int tile_number)
{
  /* Only the derived blend mask is dropped. The binary mask, and therefore the outline batch and
   * the bounding-box cache that are built from it, are untouched, so the revision must not advance
   * here. Whoever edits the binary mask bumps it (see #paint_selection_revision_bump), which is the
   * same rule #BKE_image_paint_selection_edge_policy_set relies on. */
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
}

void BKE_image_paint_selection_blend_mask_invalidate(Image *image)
{
  if (!image || !image->runtime) {
    return;
  }
  bke::ImageRuntime *runtime = image->runtime;
  for (ImBuf *blend : runtime->paint_selection_blend_masks.values()) {
    IMB_freeImBuf(blend);
  }
  runtime->paint_selection_blend_masks.clear();
}

void BKE_image_paint_selection_mask_free(Image *image)
{
  bke::ImageRuntime *runtime = image->runtime;
  for (ImBuf *mask : runtime->paint_selection_masks.values()) {
    IMB_freeImBuf(mask);
  }
  runtime->paint_selection_masks.clear();

  BKE_image_paint_selection_blend_mask_invalidate(image);
  image->runtime->paint_selection_edge_policy = BKE_image_paint_selection_edge_policy_feathered();
  paint_selection_revision_bump(image);
}

void BKE_image_paint_selection_mask_tile_free(Image *image, int tile_number)
{
  bke::ImageRuntime *runtime = image->runtime;
  ImBuf **mask_ptr = runtime->paint_selection_masks.lookup_ptr(tile_number);
  if (mask_ptr) {
    IMB_freeImBuf(*mask_ptr);
    runtime->paint_selection_masks.remove(tile_number);
  }
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
  paint_selection_revision_bump(image);
}

ImBuf *BKE_image_paint_selection_mask_lookup(Image *image, int tile_number)
{
  if (!image || !image->runtime) {
    return nullptr;
  }
  ImBuf **mask_ptr = image->runtime->paint_selection_masks.lookup_ptr(tile_number);
  if (!mask_ptr) {
    return nullptr;
  }
  /* A writable buffer is being handed out. Most callers of this overload edit the mask in place
   * without notifying this module afterwards, so assume the contents change. Consumers that only
   * read must use the `const Image *` overload. */
  paint_selection_revision_bump(image);
  return *mask_ptr;
}

const ImBuf *BKE_image_paint_selection_mask_lookup(const Image *image, int tile_number)
{
  if (!image || !image->runtime) {
    return nullptr;
  }
  ImBuf *const *mask_ptr = image->runtime->paint_selection_masks.lookup_ptr(tile_number);
  return mask_ptr ? *mask_ptr : nullptr;
}

static bool image_paint_selection_mask_imbuf_bounds(const ImBuf *mask, int r_min[2], int r_max[2])
{
  if (!mask) {
    return false;
  }
  const float *pixels = mask->float_data();
  if (!pixels) {
    return false;
  }

  const int w = mask->x;
  const int h = mask->y;

  /* Rows are scanned independently and their partial boxes merged, which makes this a plain
   * reduction over an immutable buffer. */
  const rcti empty_box = {w, 0, h, 0};
  const rcti box = threading::parallel_reduce(
      IndexRange(h),
      64,
      empty_box,
      [&](const IndexRange range, const rcti &init) {
        rcti local = init;
        for (const int64_t y : range) {
          const float *row = pixels + size_t(y) * w;
          for (int x = 0; x < w; x++) {
            if (row[x] > IMAGE_PAINT_SELECTION_MASK_THRESHOLD) {
              local.xmin = std::min(local.xmin, x);
              local.xmax = std::max(local.xmax, x + 1);
              local.ymin = std::min(local.ymin, int(y));
              local.ymax = std::max(local.ymax, int(y) + 1);
            }
          }
        }
        return local;
      },
      [](const rcti &a, const rcti &b) {
        rcti r;
        r.xmin = std::min(a.xmin, b.xmin);
        r.xmax = std::max(a.xmax, b.xmax);
        r.ymin = std::min(a.ymin, b.ymin);
        r.ymax = std::max(a.ymax, b.ymax);
        return r;
      });

  r_min[0] = box.xmin;
  r_min[1] = box.ymin;
  r_max[0] = box.xmax;
  r_max[1] = box.ymax;

  return r_max[0] > r_min[0] && r_max[1] > r_min[1];
}

/**
 * Memoized #image_paint_selection_mask_imbuf_bounds.
 *
 * The scan is O(width * height) and is issued once per tile by callers that walk a whole UDIM set,
 * so without this a multi-tile image rescans every tile on every query. Entries live only as long
 * as #bke::ImageRuntime::paint_selection_revision is unchanged; every path that can alter a mask
 * bumps that counter (see #paint_selection_revision_bump), which drops the whole cache.
 *
 * \param mask: The tile's mask, already looked up by the caller through the `const` overload so
 * this query does not itself advance the revision.
 */
static bool paint_selection_mask_bounds_cached(const Image *image,
                                               const int tile_number,
                                               const ImBuf *mask,
                                               int r_min[2],
                                               int r_max[2])
{
  if (!image || !image->runtime) {
    return image_paint_selection_mask_imbuf_bounds(mask, r_min, r_max);
  }
  /* `runtime` is a non-const pointee even on a `const Image *`; the cache is derived data behind a
   * logically const query. */
  bke::ImageRuntime &runtime = *image->runtime;

  if (runtime.paint_selection_bounds_revision != runtime.paint_selection_revision) {
    runtime.paint_selection_bounds_cache.clear();
    runtime.paint_selection_bounds_revision = runtime.paint_selection_revision;
  }

  const PaintSelectionBounds *cached = runtime.paint_selection_bounds_cache.lookup_ptr(
      tile_number);
  if (!cached) {
    PaintSelectionBounds bounds;
    /* Leaves `min` / `max` at their zeroed defaults when it returns false, so the cached entry is
     * never populated from uninitialized memory. */
    bounds.has_selection = image_paint_selection_mask_imbuf_bounds(mask, bounds.min, bounds.max);
    cached = &runtime.paint_selection_bounds_cache.lookup_or_add(tile_number, bounds);
  }

  r_min[0] = cached->min[0];
  r_min[1] = cached->min[1];
  r_max[0] = cached->max[0];
  r_max[1] = cached->max[1];
  return cached->has_selection;
}

bool BKE_image_paint_selection_mask_bounds(const Image *image,
                                           int tile_number,
                                           int r_min[2],
                                           int r_max[2])
{
  const ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile_number);
  return paint_selection_mask_bounds_cached(image, tile_number, mask, r_min, r_max);
}

float BKE_image_paint_selection_mask_sample(const Image *image, int tile_number, int x, int y)
{
  const ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile_number);
  if (!mask) {
    return 0.0f;
  }
  if (x < 0 || x >= mask->x || y < 0 || y >= mask->y) {
    return 0.0f;
  }
  const float *pixels = mask->float_buffer.data;
  if (!pixels) {
    return 0.0f;
  }
  return pixels[size_t(y) * mask->x + x];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Weights
 * \{ */

static bool image_paint_selection_mask_pixel_is_inside(
    const float *src, const int w, const int h, const int x, const int y, const int src_stride)
{
  if (x < 0 || x >= w || y < 0 || y >= h) {
    return false;
  }
  return src[(y * w + x) * src_stride] > IMAGE_PAINT_SELECTION_MASK_THRESHOLD;
}

static float image_paint_selection_smoothstep(const float edge0, const float edge1, const float x)
{
  if (edge1 <= edge0) {
    return x >= edge0 ? 1.0f : 0.0f;
  }
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static float image_paint_selection_min_dist_to_opposite(const float *src,
                                                        const int w,
                                                        const int h,
                                                        const int x,
                                                        const int y,
                                                        const int src_stride,
                                                        const int blend_radius_px)
{
  const bool center_inside = image_paint_selection_mask_pixel_is_inside(
      src, w, h, x, y, src_stride);
  const int search = blend_radius_px + 3;
  float min_dist_sq = std::numeric_limits<float>::max();

  for (int dy = -search; dy <= search; dy++) {
    const int sy = y + dy;
    if (sy < 0 || sy >= h) {
      continue;
    }
    for (int dx = -search; dx <= search; dx++) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      const int sx = x + dx;
      if (sx < 0 || sx >= w) {
        continue;
      }
      const bool neighbor_inside = image_paint_selection_mask_pixel_is_inside(
          src, w, h, sx, sy, src_stride);
      if (neighbor_inside == center_inside) {
        continue;
      }
      const float dist_sq = float(dx * dx + dy * dy);
      min_dist_sq = std::min(min_dist_sq, dist_sq);
    }
  }

  if (min_dist_sq == std::numeric_limits<float>::max()) {
    return float(search + 1);
  }
  return std::sqrt(min_dist_sq);
}

static float image_paint_selection_smooth_blend_weight(const float *src,
                                                       const int w,
                                                       const int h,
                                                       const int x,
                                                       const int y,
                                                       const int src_stride,
                                                       const int blend_radius_px,
                                                       const float edge_gamma)
{
  const bool inside = image_paint_selection_mask_pixel_is_inside(src, w, h, x, y, src_stride);

  /* Preserve full interior content; feather only outside the binary mask. */
  if (inside) {
    return 1.0f;
  }

  const float radius = float(blend_radius_px);
  const float dist_to_inside = image_paint_selection_min_dist_to_opposite(
      src, w, h, x, y, src_stride, blend_radius_px);

  if (dist_to_inside > radius + 1.5f) {
    return 0.0f;
  }

  const float falloff_extent = radius + 0.5f;
  const float dist_outside = dist_to_inside - 0.5f;
  float weight = 1.0f - image_paint_selection_smoothstep(0.0f, falloff_extent, dist_outside);

  /* Push outward feather weights toward zero for a more transparent rim. */
  weight = std::pow(weight, edge_gamma);
  return weight;
}

static float image_paint_selection_binary_blend_weight(
    const float *src, const int w, const int h, const int x, const int y, const int src_stride)
{
  return image_paint_selection_mask_pixel_is_inside(src, w, h, x, y, src_stride) ? 1.0f : 0.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Policy
 * \{ */

const PaintSelectionEdgePolicy &BKE_image_paint_selection_edge_policy_get(const Image *image)
{
  static const PaintSelectionEdgePolicy default_policy =
      BKE_image_paint_selection_edge_policy_feathered();
  if (!image || !image->runtime) {
    return default_policy;
  }
  return image->runtime->paint_selection_edge_policy;
}

void BKE_image_paint_selection_edge_policy_set(Image *image,
                                               const PaintSelectionEdgePolicy &edge_policy)
{
  if (!image || !image->runtime) {
    return;
  }
  if (image->runtime->paint_selection_edge_policy.use_outward_feather ==
          edge_policy.use_outward_feather &&
      image->runtime->paint_selection_edge_policy.blend_radius_px == edge_policy.blend_radius_px &&
      image->runtime->paint_selection_edge_policy.edge_gamma == edge_policy.edge_gamma)
  {
    return;
  }
  image->runtime->paint_selection_edge_policy = edge_policy;
  BKE_image_paint_selection_blend_mask_invalidate(image);
}

bool BKE_image_paint_selection_use_feather(const Image *image)
{
  return BKE_image_paint_selection_edge_policy_get(image).use_outward_feather;
}

void BKE_image_paint_selection_use_feather_set(Image *image, const bool use_feather)
{
  BKE_image_paint_selection_edge_policy_set(
      image,
      use_feather ? BKE_image_paint_selection_edge_policy_feathered() :
                    BKE_image_paint_selection_edge_policy_hard());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Masks
 * \{ */

static ImBuf *paint_selection_blend_mask_ensure(Image *image, int tile_number)
{
  if (!image || !image->runtime) {
    return nullptr;
  }
  /* Read-only use: take the const overload so deriving blend weights does not advance the
   * selection revision (this runs on every blend sample). */
  const ImBuf *binary = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(image),
                                                              tile_number);
  if (!binary || !binary->float_buffer.data) {
    paint_selection_blend_mask_tile_invalidate(image, tile_number);
    return nullptr;
  }

  bke::ImageRuntime *runtime = image->runtime;
  ImBuf **blend_ptr = runtime->paint_selection_blend_masks.lookup_ptr(tile_number);
  if (blend_ptr && *blend_ptr && (*blend_ptr)->x == binary->x && (*blend_ptr)->y == binary->y) {
    return *blend_ptr;
  }

  paint_selection_blend_mask_tile_invalidate(image, tile_number);
  const PaintSelectionEdgePolicy &edge_policy = BKE_image_paint_selection_edge_policy_get(image);
  ImBuf *blend = BKE_image_paint_selection_compute_blend_mask(binary, edge_policy);
  if (blend) {
    runtime->paint_selection_blend_masks.add_new(tile_number, blend);
  }
  return blend;
}

static float paint_selection_blend_mask_sample_imbuf(const ImBuf *blend, const int x, const int y)
{
  if (!blend || !blend->float_buffer.data) {
    return 1.0f;
  }
  if (x < 0 || x >= blend->x || y < 0 || y >= blend->y) {
    return 0.0f;
  }
  return blend->float_data()[size_t(y) * blend->x + x];
}

static float paint_selection_blend_mask_sample_imbuf_bilinear(const ImBuf *blend,
                                                              const float fx,
                                                              const float fy)
{
  if (!blend || !blend->float_buffer.data) {
    return 1.0f;
  }
  const int w = blend->x;
  const int h = blend->y;
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

  const float *m = blend->float_data();
  const float v00 = m[size_t(y0) * w + x0];
  const float v10 = m[size_t(y0) * w + x1];
  const float v01 = m[size_t(y1) * w + x0];
  const float v11 = m[size_t(y1) * w + x1];
  return (1.0f - wx) * (1.0f - wy) * v00 + wx * (1.0f - wy) * v10 + (1.0f - wx) * wy * v01 +
         wx * wy * v11;
}

float BKE_image_paint_selection_blend_sample(const Image *image, int tile_number, int x, int y)
{
  const PaintSelectionEdgePolicy &edge_policy = BKE_image_paint_selection_edge_policy_get(image);
  if (!edge_policy.use_outward_feather) {
    return BKE_image_paint_selection_mask_sample(image, tile_number, x, y) >
                   IMAGE_PAINT_SELECTION_MASK_THRESHOLD ?
               1.0f :
               0.0f;
  }

  ImBuf *blend = paint_selection_blend_mask_ensure(const_cast<Image *>(image), tile_number);
  if (!blend) {
    return 1.0f;
  }
  return paint_selection_blend_mask_sample_imbuf(blend, x, y);
}

float BKE_image_paint_selection_blend_sample_bilinear(const Image *image,
                                                      const int tile_number,
                                                      const float fx,
                                                      const float fy)
{
  const PaintSelectionEdgePolicy &edge_policy = BKE_image_paint_selection_edge_policy_get(image);
  if (!edge_policy.use_outward_feather) {
    const int x = int(floorf(fx));
    const int y = int(floorf(fy));
    return BKE_image_paint_selection_mask_sample(image, tile_number, x, y) >
                   IMAGE_PAINT_SELECTION_MASK_THRESHOLD ?
               1.0f :
               0.0f;
  }

  ImBuf *blend = paint_selection_blend_mask_ensure(const_cast<Image *>(image), tile_number);
  if (!blend) {
    return 1.0f;
  }
  return paint_selection_blend_mask_sample_imbuf_bilinear(blend, fx, fy);
}

ImBuf *BKE_image_paint_selection_compute_blend_mask(const ImBuf *binary_mask,
                                                    const PaintSelectionEdgePolicy &edge_policy)
{
  BLI_assert(binary_mask && binary_mask->float_buffer.data);

  const int w = binary_mask->x;
  const int h = binary_mask->y;
  ImBuf *blend = IMB_allocImBuf(w, h, ImBufFlags::Zero);
  IMB_alloc_float_pixels(blend, 1);
  float *dst = blend->float_data_for_write();
  const float *src = binary_mask->float_data();

  /* Each destination pixel is an independent read-only neighborhood search over `src`, and the
   * writable pointer above is resolved on this thread before dispatching -- ImBuf's lazy
   * materialization is not safe to trigger from workers. */
  threading::parallel_for(IndexRange(h), 16, [&](const IndexRange range) {
    for (const int64_t y : range) {
      for (int x = 0; x < w; x++) {
        dst[y * w + x] = edge_policy.use_outward_feather ?
                             image_paint_selection_smooth_blend_weight(src,
                                                                       w,
                                                                       h,
                                                                       x,
                                                                       int(y),
                                                                       1,
                                                                       edge_policy.blend_radius_px,
                                                                       edge_policy.edge_gamma) :
                             image_paint_selection_binary_blend_weight(src, w, h, x, int(y), 1);
      }
    }
  });

  return blend;
}

void BKE_image_paint_selection_bounds_expand_for_blend(
    int r_min[2],
    int r_max[2],
    int tile_width,
    int tile_height,
    const PaintSelectionEdgePolicy &edge_policy)
{
  if (!edge_policy.use_outward_feather) {
    return;
  }
  const int pad = edge_policy.blend_radius_px + 1;
  r_min[0] = std::max(0, r_min[0] - pad);
  r_min[1] = std::max(0, r_min[1] - pad);
  r_max[0] = std::min(tile_width, r_max[0] + pad);
  r_max[1] = std::min(tile_height, r_max[1] + pad);
}

void BKE_image_paint_selection_compute_blend_mask_to_4ch(
    const float *binary,
    const int width,
    const int height,
    float *blend_4ch,
    const int binary_stride_channels,
    const PaintSelectionEdgePolicy &edge_policy)
{
  /* Per-pixel independent; `blend_4ch` is caller-owned so there is no lazy buffer to materialize
   * from a worker thread. This is the dominant cost of a full-tile transform commit. */
  threading::parallel_for(IndexRange(height), 16, [&](const IndexRange range) {
    for (const int64_t y : range) {
      for (int x = 0; x < width; x++) {
        const int64_t i = y * width + x;
        blend_4ch[i * 4 + 0] = edge_policy.use_outward_feather ?
                                   image_paint_selection_smooth_blend_weight(
                                       binary,
                                       width,
                                       height,
                                       x,
                                       int(y),
                                       binary_stride_channels,
                                       edge_policy.blend_radius_px,
                                       edge_policy.edge_gamma) :
                                   image_paint_selection_binary_blend_weight(
                                       binary, width, height, x, int(y), binary_stride_channels);
      }
    }
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask Edits
 * \{ */

void BKE_image_paint_selection_mask_fill(Image *image, int tile_number, float value)
{
  ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile_number);
  if (!mask) {
    return;
  }
  float *data = mask->float_data_for_write();
  if (!data) {
    return;
  }
  std::fill_n(data, size_t(mask->x) * mask->y, value);
  mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
}

void BKE_image_paint_selection_mask_invert(Image *image, int tile_number)
{
  ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile_number);
  if (!mask) {
    return;
  }
  float *data = mask->float_data_for_write();
  if (!data) {
    return;
  }
  /* `data` is resolved above on this thread; the loop itself is elementwise. */
  const int64_t total = int64_t(mask->x) * mask->y;
  threading::parallel_for(IndexRange(total), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      data[i] = 1.0f - data[i];
    }
  });
  mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
}

void BKE_image_paint_selection_mask_replace(Image *image,
                                            int tile_number,
                                            const ImBuf *fragment_mask,
                                            const int origin[2])
{
  ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile_number);
  if (!mask || !fragment_mask) {
    return;
  }
  float *mdata = mask->float_data_for_write();
  const float *fmask = fragment_mask->float_buffer.data;
  if (!mdata || !fmask) {
    return;
  }

  const int ox = origin[0];
  const int oy = origin[1];
  for (int ly = 0; ly < fragment_mask->y; ly++) {
    const int py = oy + ly;
    if (py < 0 || py >= mask->y) {
      continue;
    }
    for (int lx = 0; lx < fragment_mask->x; lx++) {
      const int px = ox + lx;
      if (px < 0 || px >= mask->x) {
        continue;
      }
      mdata[py * mask->x + px] = fmask[ly * fragment_mask->x + lx];
    }
  }
  mask->userflags |= IB_DISPLAY_BUFFER_INVALID;
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
}

bool BKE_image_paint_selection_mask_has_any(const Image *image)
{
  if (!image || !image->runtime) {
    return false;
  }
  return !image->runtime->paint_selection_masks.is_empty();
}

int BKE_image_paint_selection_mask_first_tile_with_selection(const Image *image)
{
  if (!image || !image->runtime) {
    return 0;
  }
  /* Goes through the memoized bounds: this walks every tile, and each scan is O(width * height).
   * Populating #paint_selection_bounds_cache does not touch #paint_selection_masks, so mutating it
   * while iterating here is safe. */
  for (const auto &item : image->runtime->paint_selection_masks.items()) {
    int r_min[2], r_max[2];
    if (paint_selection_mask_bounds_cached(image, item.key, item.value, r_min, r_max)) {
      return item.key;
    }
  }
  return 0;
}

ImBuf *BKE_image_paint_selection_mask_dup_tile(Image *image, int tile_number)
{
  /* Read-only use: the const overload keeps undo snapshots from advancing the revision. */
  const ImBuf *mask = BKE_image_paint_selection_mask_lookup(const_cast<const Image *>(image),
                                                            tile_number);
  return mask ? IMB_dupImBuf(mask) : nullptr;
}

void BKE_image_paint_selection_mask_restore_tile(Image *image,
                                                 int tile_number,
                                                 const ImBuf *src_mask)
{
  if (!image || !image->runtime) {
    return;
  }
  bke::ImageRuntime *runtime = image->runtime;
  ImBuf **mask_ptr = runtime->paint_selection_masks.lookup_ptr(tile_number);

  if (src_mask) {
    if (mask_ptr && *mask_ptr) {
      ImBuf *cur = *mask_ptr;
      if (cur->float_data() && src_mask->float_data()) {
        if (cur->x == src_mask->x && cur->y == src_mask->y) {
          memcpy(cur->float_data_for_write(),
                 src_mask->float_data(),
                 sizeof(float) * cur->x * cur->y);
          cur->userflags |= IB_DISPLAY_BUFFER_INVALID;
        }
        else {
          IMB_freeImBuf(cur);
          *mask_ptr = IMB_dupImBuf(src_mask);
        }
      }
    }
    else {
      runtime->paint_selection_masks.add(tile_number, IMB_dupImBuf(src_mask));
    }
  }
  else {
    if (mask_ptr && *mask_ptr) {
      IMB_freeImBuf(*mask_ptr);
      runtime->paint_selection_masks.remove(tile_number);
    }
  }
  paint_selection_blend_mask_tile_invalidate(image, tile_number);
  paint_selection_revision_bump(image);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pixel Transfer
 * \{ */

ImBuf *BKE_image_paint_selection_extract_pixels(Image *image,
                                                int tile_number,
                                                ImageUser *iuser,
                                                int r_origin[2],
                                                int r_size[2],
                                                ImBuf **r_mask_out)
{
  if (!image || !image->runtime) {
    return nullptr;
  }

  int r_min[2], r_max[2];
  if (!BKE_image_paint_selection_mask_bounds(image, tile_number, r_min, r_max)) {
    return nullptr;
  }

  /* Use a local iuser if none provided; overwrite tile in either case. */
  ImageUser local_iuser;
  if (!iuser) {
    BKE_imageuser_default(&local_iuser);
    iuser = &local_iuser;
  }
  iuser->tile = tile_number;

  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, iuser, &lock);
  if (!ibuf) {
    return nullptr;
  }
  if (!ibuf->float_buffer.data && !ibuf->byte_buffer.data) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return nullptr;
  }

  BKE_image_paint_selection_bounds_expand_for_blend(
      r_min, r_max, ibuf->x, ibuf->y, BKE_image_paint_selection_edge_policy_get(image));

  const int x_min = r_min[0];
  const int y_min = r_min[1];
  const int w = r_max[0] - x_min;
  const int h = r_max[1] - y_min;

  const bool is_float = ibuf->float_buffer.data != nullptr;

  /* Clamp bbox to ibuf dimensions for safety (mask and ibuf should agree, but guard anyway). */
  const int safe_x = std::max(x_min, 0);
  const int safe_y = std::max(y_min, 0);
  const int safe_w = std::min(w, ibuf->x - safe_x);
  const int safe_h = std::min(h, ibuf->y - safe_y);

  if (safe_w <= 0 || safe_h <= 0) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return nullptr;
  }

  ImBuf *fragment = IMB_allocImBuf(
      safe_w, safe_h, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  if (!fragment) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return nullptr;
  }
  if (is_float) {
    /* Match the source channel count so IMB_copy_rect uses the correct stride. */
    fragment->channels = ibuf->channels ? ibuf->channels : 4;
  }

  IMB_copy_rect(fragment, ibuf, int2{safe_x, safe_y}, int2{0, 0}, int2{safe_w, safe_h});

  /* Preserve colorspace so downstream tools see the correct color encoding. */
  if (is_float) {
    const ColorSpace *cs = ibuf->float_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_float_colorspace(
          fragment, IMB_colormanagement_colorspace_get_name(cs));
    }
  }
  else {
    const ColorSpace *cs = ibuf->byte_buffer.colorspace;
    if (cs) {
      IMB_colormanagement_assign_byte_colorspace(
          fragment, IMB_colormanagement_colorspace_get_name(cs));
    }
  }

  BKE_image_release_ibuf(image, ibuf, lock);

  r_origin[0] = safe_x;
  r_origin[1] = safe_y;
  r_size[0] = safe_w;
  r_size[1] = safe_h;

  /* Optionally extract the corresponding mask sub-region. */
  if (r_mask_out) {
    /* Read-only use: the const overload avoids advancing the revision. */
    const ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(
        const_cast<const Image *>(image), tile_number);
    ImBuf *fmask = IMB_allocImBuf(safe_w, safe_h, ImBufFlags::Zero);
    IMB_alloc_float_pixels(fmask, 1);
    float *dst_mask = fmask->float_data_for_write();
    if (tile_mask && tile_mask->float_buffer.data) {
      const float *src_mask = tile_mask->float_data();
      const int full_w = tile_mask->x;
      for (int row = 0; row < safe_h; row++) {
        memcpy(dst_mask + row * safe_w,
               src_mask + (safe_y + row) * full_w + safe_x,
               size_t(safe_w) * sizeof(float));
      }
    }
    else {
      memset(dst_mask, 0, size_t(safe_w) * safe_h * sizeof(float));
    }
    *r_mask_out = fmask;
  }

  return fragment;
}

bool BKE_image_paint_selection_write_region(Image *image,
                                            int tile_number,
                                            ImageUser *iuser,
                                            const float *pixels,
                                            int channels,
                                            int x,
                                            int y,
                                            int width,
                                            int height,
                                            const float *mask)
{
  if (!image || !image->runtime || !pixels || width <= 0 || height <= 0 || channels <= 0) {
    return false;
  }

  ImageUser local_iuser;
  if (!iuser) {
    BKE_imageuser_default(&local_iuser);
    iuser = &local_iuser;
  }
  iuser->tile = tile_number;

  void *lock;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, iuser, &lock);
  if (!ibuf) {
    return false;
  }
  if (!ibuf->float_buffer.data && !ibuf->byte_buffer.data) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return false;
  }

  /* Require exact channel match to avoid silent data corruption. */
  const int ibuf_channels = ibuf->channels ? ibuf->channels : 4;
  if (channels != ibuf_channels) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return false;
  }

  const bool is_float = ibuf->float_buffer.data != nullptr;

  /* Clamp region to image bounds; allow partial writes at image edges. */
  const int rx = std::max(0, x);
  const int ry = std::max(0, y);
  const int rx2 = std::min(x + width, ibuf->x);
  const int ry2 = std::min(y + height, ibuf->y);
  const int rw = rx2 - rx;
  const int rh = ry2 - ry;

  if (rw <= 0 || rh <= 0) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return false;
  }

  /* Offsets into the src pixels buffer when the region extends beyond the image boundary. */
  const int src_x_off = rx - x; /* >= 0 */
  const int src_y_off = ry - y; /* >= 0 */

  if (is_float) {
    float *dst = ibuf->float_data_for_write();
    for (int row = 0; row < rh; row++) {
      const int src_row = src_y_off + row;
      for (int col = 0; col < rw; col++) {
        const int src_col = src_x_off + col;
        const int dst_idx = ((ry + row) * ibuf->x + (rx + col)) * channels;
        const int src_idx = (src_row * width + src_col) * channels;
        if (mask) {
          const float m = mask[src_row * width + src_col];
          for (int c = 0; c < channels; c++) {
            dst[dst_idx + c] = dst[dst_idx + c] * (1.0f - m) + pixels[src_idx + c] * m;
          }
        }
        else {
          memcpy(dst + dst_idx, pixels + src_idx, size_t(channels) * sizeof(float));
        }
      }
    }
  }
  else {
    uint8_t *dst = ibuf->byte_data_for_write();
    for (int row = 0; row < rh; row++) {
      const int src_row = src_y_off + row;
      for (int col = 0; col < rw; col++) {
        const int src_col = src_x_off + col;
        const int dst_idx = ((ry + row) * ibuf->x + (rx + col)) * channels;
        const int src_idx = (src_row * width + src_col) * channels;
        const float m = mask ? mask[src_row * width + src_col] : 1.0f;
        for (int c = 0; c < channels; c++) {
          float src_f = pixels[src_idx + c];
          if (mask) {
            const float dst_f = float(dst[dst_idx + c]) * (1.0f / 255.0f);
            src_f = dst_f * (1.0f - m) + src_f * m;
          }
          dst[dst_idx + c] = static_cast<uint8_t>(
              std::max(0.0f, std::min(1.0f, src_f)) * 255.0f + 0.5f);
        }
      }
    }
  }

  /* Mark the affected region dirty so the viewport and GPU texture update. */
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(image, ibuf);

  rcti dirty_rect;
  dirty_rect.xmin = rx;
  dirty_rect.xmax = rx2;
  dirty_rect.ymin = ry;
  dirty_rect.ymax = ry2;
  const ImageTile *tile = BKE_image_get_tile(image, tile_number);
  if (tile) {
    BKE_image_partial_update_mark_region(image, tile, ibuf, &dirty_rect);
  }
  else {
    BKE_image_partial_update_mark_full_update(image);
  }

  BKE_image_release_ibuf(image, ibuf, lock);
  return true;
}

/** \} */

}  // namespace blender
