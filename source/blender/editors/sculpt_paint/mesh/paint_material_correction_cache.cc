/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_material_correction_cache.hh"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

#include "BKE_brush_material_correction.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_mutex.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_material_source.hh"

namespace blender::ed::sculpt_paint::material {

namespace {

struct CacheEntry {
  uint32_t image_uid = 0;
  uint64_t key_hash = 0;
  int channel = 0;
  const ocio::ColorSpace *colorspace = nullptr;
  /** Which frame/layer/tile of the image the buffer holds. */
  CorrectionSourceKey source_key;
  int width = 0;
  int height = 0;
  /** #ImageRuntime::pixel_version the pixels were taken at. */
  uint64_t pixel_version = 0;
  /** Recency for the LRU order; larger is newer. */
  uint64_t last_used = 0;
  std::shared_ptr<const ImBuf> buffer;
};

Vector<std::unique_ptr<CacheEntry>> &cache_entries()
{
  static Vector<std::unique_ptr<CacheEntry>> entries;
  return entries;
}

std::atomic<uint64_t> &cache_clock()
{
  static std::atomic<uint64_t> clock = 0;
  return clock;
}

uint64_t cache_clock_next()
{
  return cache_clock().fetch_add(1) + 1;
}

Mutex &cache_mutex()
{
  static Mutex mutex;
  return mutex;
}

std::atomic<uint64_t> &cache_hits()
{
  static std::atomic<uint64_t> hits = 0;
  return hits;
}

std::atomic<uint64_t> &cache_builds()
{
  static std::atomic<uint64_t> builds = 0;
  return builds;
}

std::atomic<uint64_t> &cache_region_updates()
{
  static std::atomic<uint64_t> region_updates = 0;
  return region_updates;
}

int64_t entry_bytes(const CacheEntry &entry)
{
  return int64_t(entry.width) * entry.height * sizeof(float[4]);
}

int64_t cache_bytes()
{
  int64_t bytes = 0;
  for (const std::unique_ptr<CacheEntry> &entry : cache_entries()) {
    bytes += entry_bytes(*entry);
  }
  return bytes;
}

/** #IMB_allocImBuf behind a `shared_ptr`, so a stroke and the cache can share one buffer. */
std::shared_ptr<ImBuf> owned_imbuf(ImBuf *ibuf)
{
  return std::shared_ptr<ImBuf>(ibuf, [](ImBuf *owned) { IMB_freeImBuf(owned); });
}

/**
 * Corrects the rectangle [xmin, xmax) x [ymin, ymax) of \a pixels in place.
 *
 * Pointwise, so a rectangle can be re-corrected on its own when the image was painted over: a
 * bilinear sample never reads outside the texels it interpolates, so no halo is needed.
 */
void correct_rect(const BrushMaterialPaint &brush_paint,
                  const BrushMaterialPaintCorrection &correction,
                  const eMaterialPaintChannel channel,
                  const float2 value_range,
                  const bool for_mask,
                  float *pixels,
                  const int width,
                  const int xmin,
                  const int xmax,
                  const int ymin,
                  const int ymax)
{
  const bool is_color = BKE_paint_material_channel_info(channel).is_color;
  /* Resolved once for the whole rectangle: the loop below used to ask whether the channel has a
   * correction again for every texel it touched. */
  const BrushMaterialPaintCorrectionPrepared prepared =
      BKE_brush_material_paint_correction_prepare(correction);

  threading::parallel_for(IndexRange(ymin, ymax - ymin), 8, [&](const IndexRange y_range) {
    for (const int64_t y : y_range) {
      for (int x = xmin; x < xmax; x++) {
        float *texel = pixels + (y * width + x) * 4;
        if (is_color) {
          float rgb[3] = {texel[0], texel[1], texel[2]};
          BKE_brush_material_paint_correction_apply_prepared_rgb(prepared, rgb);
          texel[0] = rgb[0];
          texel[1] = rgb[1];
          texel[2] = rgb[2];
          continue;
        }
        const float luminance = IMB_colormanagement_get_luminance(texel);
        float value;
        if (for_mask) {
          /* A Mask Source read: the grey holds the raw coverage, clamped and corrected, with no
           * channel #source_invert folded in - a Mask coverage read mirrors only through the Mask
           * Source's own invert, applied where the coverage is consumed. Same raw-coverage rule
           * the samplers use (#mask_coverage_correct_raw). */
          value = mask_coverage_correct_raw(&prepared, luminance);
        }
        else {
          /* A scalar channel reads the luminance of its source, so the finished value is stored as
           * grey. Alpha keeps the source's own, which the Mask paths read raw. */
          value = scalar_source_finish_prepared(brush_paint, channel, value_range, luminance, &prepared);
        }
        texel[0] = value;
        texel[1] = value;
        texel[2] = value;
      }
    }
  });
}

/**
 * Copies the rectangle [xmin, xmax) x [ymin, ymax) of the request's source pixels into \a dst and
 * decodes them to scene-linear when the source is a byte buffer.
 *
 * Used both to fill a fresh buffer and to refresh the rectangle a stroke edited: the cache holds
 * corrected texels, so an edited rectangle has to be refilled from the source *before* it is
 * corrected again - correcting it in place would apply the stack to its own result.
 */
void copy_rect_from_source(const CorrectionCacheRequest &request,
                           ImBuf &dst,
                           const int xmin,
                           const int xmax,
                           const int ymin,
                           const int ymax)
{
  const ImBuf &source = request.source;
  const int src_width = source.x;
  const int dst_width = dst.x;
  const int width = xmax - xmin;
  const float *float_pixels = source.float_data();
  const uchar *byte_pixels = source.byte_data();
  float *dst_pixels = dst.float_data_for_write();
  BLI_assert(src_width == dst_width);

  threading::parallel_for(IndexRange(ymin, ymax - ymin), 8, [&](const IndexRange y_range) {
    for (const int64_t y : y_range) {
      const int64_t src_row = y * src_width + xmin;
      float *dst_row = dst_pixels + (y * dst_width + xmin) * 4;
      if (float_pixels != nullptr) {
        memcpy(dst_row, float_pixels + src_row * 4, sizeof(float[4]) * size_t(width));
        continue;
      }
      for (int x = 0; x < width; x++) {
        rgba_uchar_to_float(dst_row + x * 4, byte_pixels + (src_row + x) * 4);
      }
      if (request.decode_colorspace != nullptr) {
        IMB_colormanagement_colorspace_to_scene_linear(
            dst_row, width, 1, 4, request.decode_colorspace, false);
      }
    }
  });
}

/** Allocates the corrected buffer for the request's source image and fills it. */
std::shared_ptr<ImBuf> copy_source_pixels(const CorrectionCacheRequest &request)
{
  const int width = request.source.x;
  const int height = request.source.y;

  ImBuf *dst = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::Zero);
  if (dst == nullptr) {
    return nullptr;
  }
  if (!IMB_alloc_float_pixels(dst, 4, /*initialize_pixels=*/false)) {
    IMB_freeImBuf(dst);
    return nullptr;
  }
  /* The direct samplers only accept a four-channel float buffer (see
   * #channel_source_image_direct_ok), and #IMB_alloc_float_pixels above is what sets this. */
  dst->channels = 4;

  copy_rect_from_source(request, *dst, 0, width, 0, height);
  return owned_imbuf(dst);
}

std::shared_ptr<const ImBuf> build_entry(const CorrectionCacheRequest &request)
{
  /* A buffer larger than the whole budget would be built only to be evicted immediately. */
  const int64_t bytes = int64_t(request.source.x) * request.source.y * sizeof(float[4]);
  if (bytes > correction_cache_max_bytes) {
    return nullptr;
  }

  std::shared_ptr<ImBuf> buffer = copy_source_pixels(request);
  if (!buffer) {
    return nullptr;
  }
  correct_rect(request.brush_paint,
               request.correction,
               request.channel,
               request.value_range,
               request.for_mask,
               buffer->float_data_for_write(),
               buffer->x,
               0,
               buffer->x,
               0,
               buffer->y);
  cache_builds().fetch_add(1);
  return buffer;
}

/** Drops the least recently used entry that is not \a keep. */
bool evict_one(const CacheEntry *keep)
{
  Vector<std::unique_ptr<CacheEntry>> &entries = cache_entries();
  int64_t oldest = -1;
  for (const int i : entries.index_range()) {
    if (entries[i].get() == keep) {
      continue;
    }
    if (oldest == -1 || entries[i]->last_used < entries[oldest]->last_used) {
      oldest = i;
    }
  }
  if (oldest == -1) {
    return false;
  }
  entries.remove(int(oldest));
  return true;
}

void enforce_budget(const CacheEntry *keep)
{
  while (cache_bytes() > correction_cache_max_bytes && cache_entries().size() > 1) {
    if (!evict_one(keep)) {
      return;
    }
  }
}

/** The entry built from exactly these pixels, or null. */
CacheEntry *find_entry(const uint32_t image_uid,
                       const uint64_t key_hash,
                       const int channel,
                       const CorrectionCacheRequest &request)
{
  for (const std::unique_ptr<CacheEntry> &entry : cache_entries()) {
    if (entry->image_uid == image_uid && entry->key_hash == key_hash &&
        entry->channel == channel && entry->colorspace == request.decode_colorspace &&
        entry->source_key == request.source_key && entry->width == request.source.x &&
        entry->height == request.source.y)
    {
      return entry.get();
    }
  }
  return nullptr;
}

}  // namespace

std::shared_ptr<const ImBuf> correction_cache_get(const CorrectionCacheRequest &request)
{
  const uint32_t image_uid = request.image.id.session_uid;
  /* The correction's own parameters are not the whole story: a scalar channel's finished value is
   * defined over the channel range and folds in the texture's Source Invert, and the buffer holds
   * that finished value. A Mask-mode build never folds that invert, so it keys apart from a
   * Maps-mode build of the same channel. */
  const uint64_t key_hash = get_default_hash(
      BKE_brush_material_paint_correction_hash(request.correction),
      request.value_range.x,
      request.value_range.y,
      request.for_mask ?
          0 :
          int(request.brush_paint.channels[int(request.channel)].source_invert));
  const int channel = int(request.channel);
  const int width = request.source.x;
  const int height = request.source.y;
  /* No runtime means the image was never initialized for painting, so nothing can have been
   * marked; a fixed version keeps the key stable in that case. */
  const uint64_t pixel_version = request.image.runtime != nullptr ?
                                     request.image.runtime->pixel_version.load(
                                         std::memory_order_relaxed) :
                                     0;

  {
    std::lock_guard lock{cache_mutex()};

    CacheEntry *matching = find_entry(image_uid, key_hash, channel, request);
    if (matching != nullptr) {
      if (matching->pixel_version == pixel_version) {
        matching->last_used = cache_clock_next();
        cache_hits().fetch_add(1);
        return matching->buffer;
      }

      /* The source image was edited since. Only the edited rectangle has to be refreshed, and only
       * while nothing else reads the buffer: a running stroke holds its own reference and must not
       * see its texels change under it. */
      rcti region;
      const ePixelChange change = BKE_image_pixel_changes_since(
          request.image, matching->pixel_version, region);
      if (change == ePixelChange::Region && matching->buffer.use_count() == 1) {
        ImBuf *buffer = const_cast<ImBuf *>(matching->buffer.get());
        /* The journal's rectangle includes its far edges (it is grown with
         * `BLI_rcti_do_minmax_v`); the correction loop works with a half-open one. */
        const int xmin = math::clamp(region.xmin, 0, width);
        const int xmax = math::clamp(region.xmax + 1, 0, width);
        const int ymin = math::clamp(region.ymin, 0, height);
        const int ymax = math::clamp(region.ymax + 1, 0, height);
        if (xmin < xmax && ymin < ymax) {
          /* Refill from the source first: the buffer already holds corrected texels, so correcting
           * them again would apply the stack to its own result. */
          copy_rect_from_source(request, *buffer, xmin, xmax, ymin, ymax);
          correct_rect(request.brush_paint,
                       request.correction,
                       request.channel,
                       request.value_range,
                       request.for_mask,
                       buffer->float_data_for_write(),
                       width,
                       xmin,
                       xmax,
                       ymin,
                       ymax);
          matching->pixel_version = pixel_version;
          matching->last_used = cache_clock_next();
          cache_region_updates().fetch_add(1);
          cache_hits().fetch_add(1);
          return matching->buffer;
        }
      }
    }
  }

  /* Building a 4K buffer takes long enough that the cursor preview and the other channels must not
   * wait on the cache lock: build it here, then publish it below. */
  std::shared_ptr<const ImBuf> built = build_entry(request);
  if (!built) {
    return nullptr;
  }

  std::lock_guard lock{cache_mutex()};

  Vector<std::unique_ptr<CacheEntry>> &entries = cache_entries();
  /* Another stroke may have published the same buffer while this one was building. */
  CacheEntry *matching = find_entry(image_uid, key_hash, channel, request);
  if (matching != nullptr && matching->pixel_version == pixel_version) {
    return matching->buffer;
  }

  if (matching != nullptr) {
    matching->buffer = std::move(built);
    matching->pixel_version = pixel_version;
    matching->last_used = cache_clock_next();
    enforce_budget(matching);
    return matching->buffer;
  }

  /* One entry per (image, channel) is enough: dragging a correction slider or changing the range
   * is one stroke after another with dead parameters in between, and keeping each of those would
   * fill the budget with the history of a drag. */
  entries.remove_if([&](const std::unique_ptr<CacheEntry> &entry) {
    return entry->image_uid == image_uid && entry->channel == channel;
  });

  std::unique_ptr<CacheEntry> entry = std::make_unique<CacheEntry>();
  entry->image_uid = image_uid;
  entry->key_hash = key_hash;
  entry->channel = channel;
  entry->colorspace = request.decode_colorspace;
  entry->source_key = request.source_key;
  entry->width = width;
  entry->height = height;
  entry->pixel_version = pixel_version;
  entry->last_used = cache_clock_next();
  entry->buffer = std::move(built);
  CacheEntry *added = entry.get();
  entries.append(std::move(entry));
  enforce_budget(added);
  return added->buffer;
}

CorrectionCacheStats correction_cache_stats()
{
  std::lock_guard lock{cache_mutex()};
  CorrectionCacheStats stats;
  stats.hits = cache_hits().load(std::memory_order_relaxed);
  stats.builds = cache_builds().load(std::memory_order_relaxed);
  stats.region_updates = cache_region_updates().load(std::memory_order_relaxed);
  stats.bytes = size_t(cache_bytes());
  stats.entries = size_t(cache_entries().size());
  return stats;
}

void correction_cache_clear()
{
  std::lock_guard lock{cache_mutex()};
  cache_entries().clear();
}

bool channel_source_bake_correction(const BrushMaterialPaint &brush_paint,
                                    const PaintModeSettings &settings,
                                    const Image &image,
                                    const ImageUser &iuser,
                                    const ImBuf &source_ibuf,
                                    ChannelSourceSet::ChannelSource &source,
                                    const eMaterialPaintChannel channel,
                                    std::shared_ptr<const ImBuf> &r_entry,
                                    const bool for_mask)
{
  const BrushMaterialPaintCorrection *correction =
      BKE_brush_material_paint_correction_for_channel(brush_paint, channel);
  if (correction == nullptr) {
    return false;
  }
  if (source.alpha_weighted) {
    /* Alpha-weighted sampling corrects the alpha-weighted average of the texels, which a corrected
     * texel buffer cannot reproduce. Such sources are bakes, which never carry a correction; the
     * guard is here so a future alpha-weighted kind does not silently paint the wrong colors. */
    return false;
  }
  /* A scalar channel reads an intensity, and neither the stroke nor the 2D engine ever
   * colorspace-decodes that: only color channels are decoded. Baking a decode into the cache would
   * make a scalar with an sRGB tag read different numbers than the on-the-fly path. */
  const bool is_color = BKE_paint_material_channel_info(channel).is_color;
  const CorrectionSourceKey source_key{iuser.framenr,
                                       int(iuser.layer),
                                       int(iuser.pass),
                                       int(iuser.view),
                                       iuser.tile};
  const CorrectionCacheRequest request{
      brush_paint,
      *correction,
      channel,
      image,
      source_ibuf,
      is_color ? source.colorspace : static_cast<const ocio::ColorSpace *>(nullptr),
      source_key,
      /* Storage range, like every other #scalar_source_finish caller: the cached texels must hold
       * the same answer the on-the-fly path would give (Height mirrors around its stored neutral).
       * A Mask-mode build (\a for_mask) stores the raw coverage instead - see
       * #CorrectionCacheRequest::for_mask. */
      stored_scalar_range(settings, channel),
      for_mask};
  std::shared_ptr<const ImBuf> buffer = correction_cache_get(request);
  if (!buffer) {
    return false;
  }
  r_entry = std::move(buffer);
  source.ibuf = const_cast<ImBuf *>(r_entry.get());
  /* The cached buffer is already scene-linear and already corrected. */
  source.do_linear_conversion = false;
  source.colorspace = nullptr;
  source.correction_baked = true;
  return true;
}

}  // namespace blender::ed::sculpt_paint::material
