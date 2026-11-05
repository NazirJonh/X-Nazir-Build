/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_mask_coverage_cache.hh"

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "BLI_index_range.hh"
#include "BLI_math_color.h"
#include "BLI_math_interp.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <utility>

namespace blender::ed::sculpt_paint::material {

/* -------------------------------------------------------------------- */
/** \name Cache key
 *
 * The key type lives in the header (see #MaskCoverageKey); only its construction from a resolved
 * source stays here.
 * \{ */

uint64_t MaskCoverageKey::hash() const
{
  IdentityHasher hasher;
  hasher.mix(int(channel));
  hasher.mix(int(kind));
  hasher.mix(baked_target_uv);
  hasher.mix(source_mode);
  hasher.mix(layer_target_mode);
  hasher.mix(tex);
  hasher.mix(image);
  hasher.mix(ibuf);
  hasher.mix(pixels);
  hasher.mix(sharing);
  hasher.mix(userflags);
  hasher.mix(pixel_version);
  hasher.mix(baked_revision);
  hasher.mix(colorspace);
  hasher.mix(is_float);
  hasher.mix(width);
  hasher.mix(height);
  return hasher.get();
}

/**
 * Build the key for \a source. The identity is deliberately weak - no strong user is taken on the
 * pixels - because #ImBuf::float_data_for_write clones a buffer with more than one user, and a pin
 * would make every write to the source image copy-on-write. #pixel_version keeps an in-place
 * rewrite of the same buffer from matching an entry built before it, and a re-resolve that
 * replaces the buffer changes the pointer fields too. A bake has no #pixel_version, so its own
 * monotonic #baked_revision carries that identity even if the freed buffer's address is reused.
 *
 * The caller does not cache when the buffer carries no sharing info at all: there is no identity
 * to record besides the raw data pointer.
 */
static MaskCoverageKey mask_coverage_key_build(const eMaterialPaintChannel channel,
                                               const ChannelSourceSet::ChannelSource &source,
                                               const BrushMaterialPaint &brush_paint,
                                               const PaintModeSettings &settings)
{
  MaskCoverageKey key;
  key.channel = channel;
  key.kind = source.kind;
  key.baked_target_uv = source.kind == ChannelSourceKind::Baked && source.baked_target_uv;
  key.source_mode = brush_paint.source_mode;
  key.layer_target_mode = settings.layer_target_mode;

  key.tex = source.kind == ChannelSourceKind::Mtex && source.mtex != nullptr ?
                source.mtex->tex :
                nullptr;
  key.image = source.image;
  key.ibuf = source.ibuf;
  if (source.ibuf != nullptr) {
    key.is_float = source.ibuf->float_data() != nullptr;
    key.pixels = key.is_float ? reinterpret_cast<const void *>(source.ibuf->float_data()) :
                                reinterpret_cast<const void *>(source.ibuf->byte_data());
    key.sharing = key.is_float ?
                      reinterpret_cast<const void *>(source.ibuf->float_buffer.sharing_info.get()) :
                      reinterpret_cast<const void *>(source.ibuf->byte_buffer.sharing_info.get());
    key.userflags = source.ibuf->userflags;
    key.pixel_version = source.image != nullptr && source.image->runtime != nullptr ?
                            source.image->runtime->pixel_version.load(
                                std::memory_order_relaxed) :
                            0;
    key.baked_revision = source.baked_revision;
    key.colorspace = source.colorspace;
    key.width = source.ibuf->x;
    key.height = source.ibuf->y;
  }
  return key;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registry
 * \{ */

/**
 * Buffers live here across strokes: a source is rebuilt only when its pixels change, so painting
 * with quick successive strokes never re-pays the extraction. Kept tiny on purpose - one entry
 * per source that is actually in use, evicted least-recently-first when a new source no longer
 * fits the entry count or the byte budget - which is the memory limit the Mask Source needs: one
 * buffer per source, never more.
 */
struct MaskCoverageCacheEntry {
  MaskCoverageKey key;
  std::shared_ptr<const MaskSourceCoverage> coverage;
  /** The registry's own cost of this entry, against #mask_coverage_cache_budget_bytes. */
  int64_t bytes = 0;
  uint64_t last_use = 0;
};

static std::mutex mask_coverage_cache_mutex;
static Vector<MaskCoverageCacheEntry> mask_coverage_cache;
static uint64_t mask_coverage_cache_use_counter = 0;
/** Bytes the registry's live entries hold, against #mask_coverage_cache_budget_bytes. */
static int64_t mask_coverage_budget_bytes_used = 0;

/** The registry's cost of one entry: the coverage buffer it owns. */
static int64_t mask_coverage_entry_bytes(const MaskSourceCoverage &coverage)
{
  return int64_t(coverage.width()) * coverage.height() * sizeof(float);
}

/** Evict the least-recently-used entry, dropping its bytes from the budget. */
static void mask_coverage_cache_evict_oldest()
{
  int oldest = 0;
  for (const int i : mask_coverage_cache.index_range()) {
    if (mask_coverage_cache[i].last_use < mask_coverage_cache[oldest].last_use) {
      oldest = i;
    }
  }
  mask_coverage_budget_bytes_used -= mask_coverage_cache[oldest].bytes;
  mask_coverage_cache.remove(oldest);
}

/**
 * Store \a coverage under \a key, evicting least-recently-used entries until it fits both the
 * entry count and the byte budget. The budget counts the coverage buffers the registry owns, not
 * the source pixels: those belong to the image and are identified weakly by the key.
 *
 * \return false when even an empty registry could not hold it (a single entry above the budget):
 * the caller falls back to the computed-on-the-fly path. The accounting is registry-local: a
 * coverage still referenced by a live stroke is not freed by an eviction, so the resident total
 * can exceed the budget by that borrowed amount until the stroke ends.
 */
bool mask_coverage_cache_insert(MaskCoverageKey &&key,
                                std::shared_ptr<const MaskSourceCoverage> coverage)
{
  const int64_t new_bytes = mask_coverage_entry_bytes(*coverage);
  std::lock_guard lock(mask_coverage_cache_mutex);
  /* Drawn under the lock, like every other access: the counter is shared LRU state and a torn
   * increment from a parallel lookup would hand two entries the same stamp. */
  const uint64_t stamp = ++mask_coverage_cache_use_counter;
  for (MaskCoverageCacheEntry &entry : mask_coverage_cache) {
    if (entry.key == key) {
      mask_coverage_budget_bytes_used += new_bytes - entry.bytes;
      entry.bytes = new_bytes;
      entry.coverage = std::move(coverage);
      entry.last_use = stamp;
      return true;
    }
  }
  while ((mask_coverage_cache.size() >= mask_coverage_cache_max_entries ||
          mask_coverage_budget_bytes_used + new_bytes > mask_coverage_cache_budget_bytes) &&
         !mask_coverage_cache.is_empty())
  {
    mask_coverage_cache_evict_oldest();
  }
  if (mask_coverage_cache.size() >= mask_coverage_cache_max_entries ||
      mask_coverage_budget_bytes_used + new_bytes > mask_coverage_cache_budget_bytes)
  {
    return false;
  }
  mask_coverage_budget_bytes_used += new_bytes;
  mask_coverage_cache.append(
      MaskCoverageCacheEntry{std::move(key), std::move(coverage), new_bytes, stamp});
  return true;
}

std::shared_ptr<const MaskSourceCoverage> mask_coverage_cache_lookup(const MaskCoverageKey &key)
{
  std::lock_guard lock(mask_coverage_cache_mutex);
  /* Under the lock, see #mask_coverage_cache_insert. */
  const uint64_t stamp = ++mask_coverage_cache_use_counter;
  for (MaskCoverageCacheEntry &entry : mask_coverage_cache) {
    if (entry.key == key) {
      entry.last_use = stamp;
      return entry.coverage;
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extraction
 * \{ */

/**
 * The grey one texel reduces to, exactly as the stroke's per-texel path computes it.
 *
 * Alpha follows the direct sampler's intensity (#IMB_colormanagement_get_luminance of the stored
 * color, which is what #sample_layout_image and #ChannelSourceSet::sample_image_direct return);
 * a Baked Target UV source instead reads the bake's first channel, matching
 * #ChannelSourceSet::sample_baked. Base Color takes the scene-linear luminance
 * (#BKE_paint_material_mask_luminance) of the decoded color, which is what the stroke's color
 * path luminance-reduces. Neither reduction is clamped here: an HDR float source can exceed 1 and
 * the read side clamps after its own bias term, so clamping early would change the blend of two
 * out-of-range texels.
 */
static float mask_coverage_texel(const eMaterialPaintChannel channel,
                                 const bool baked_target_uv,
                                 const float4 &rgba)
{
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    return BKE_paint_material_mask_luminance(float3(rgba.x, rgba.y, rgba.z));
  }
  if (baked_target_uv) {
    return rgba.x;
  }
  return IMB_colormanagement_get_luminance(rgba);
}

static void mask_coverage_fill(const eMaterialPaintChannel channel,
                               const ChannelSourceSet::ChannelSource &source,
                               const bool is_float,
                               MutableSpan<float> r_buffer)
{
  const ImBuf *ibuf = source.ibuf;
  const int width = ibuf->x;
  const bool baked_target_uv = source.kind == ChannelSourceKind::Baked && source.baked_target_uv;

  if (is_float) {
    const float *pixels = ibuf->float_data();
    threading::parallel_for(IndexRange(ibuf->y), 64, [&](const IndexRange rows) {
      for (const int y : rows) {
        const float *row = pixels + int64_t(y) * width * 4;
        float *out_row = r_buffer.data() + int64_t(y) * width;
        for (const int x : IndexRange(width)) {
          const float *texel = row + int64_t(x) * 4;
          out_row[x] = mask_coverage_texel(
              channel, baked_target_uv, float4(texel[0], texel[1], texel[2], texel[3]));
        }
      }
    });
    return;
  }

  /* Byte sources: a Base Color grey needs the scene-linear color, decoded in one batched call per
   * the whole buffer (the per-texel API is documented as much slower); an Alpha grey is taken from
   * the stored (encoded) values, which is what the direct sampler's luminance of a byte sample is.
   */
  const uchar *pixels = ibuf->byte_data();
  const int64_t pixel_num = int64_t(width) * int64_t(ibuf->y);
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    Array<float4> decoded(pixel_num);
    for (const int64_t i : IndexRange(pixel_num)) {
      rgba_uchar_to_float(decoded[i], pixels + i * 4);
    }
    IMB_colormanagement_colorspace_to_scene_linear(reinterpret_cast<float *>(decoded.data()),
                                                   int(pixel_num),
                                                   1,
                                                   3,
                                                   source.colorspace,
                                                   false);
    threading::parallel_for(IndexRange(int(pixel_num)), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_buffer[i] = BKE_paint_material_mask_luminance(
            float3(decoded[i].x, decoded[i].y, decoded[i].z));
      }
    });
    return;
  }
  threading::parallel_for(IndexRange(ibuf->y), 64, [&](const IndexRange rows) {
    for (const int y : rows) {
      const uchar *row = pixels + int64_t(y) * width * 4;
      float *out_row = r_buffer.data() + int64_t(y) * width;
      for (const int x : IndexRange(width)) {
        float4 rgba;
        rgba_uchar_to_float(rgba, row + int64_t(x) * 4);
        out_row[x] = mask_coverage_texel(channel, baked_target_uv, rgba);
      }
    }
  });
}

/**
 * Build the coverage for a resolved source, or null when it cannot be represented - the caller
 * keeps its computed-on-the-fly path for those.
 */
std::shared_ptr<const MaskSourceCoverage> mask_source_coverage_build(
    const eMaterialPaintChannel channel, const ChannelSourceSet::ChannelSource &source)
{
  const ImBuf *ibuf = source.ibuf;
  if (ibuf == nullptr || ibuf->x <= 0 || ibuf->y <= 0) {
    return nullptr;
  }
  const bool is_float = ibuf->float_data() != nullptr;
  if (!is_float && ibuf->byte_data() == nullptr) {
    return nullptr;
  }
  if (int64_t(ibuf->x) * int64_t(ibuf->y) > mask_coverage_cache_max_pixels) {
    /* Too large: the per-texel path is the memory limit here, not a slowdown worth avoiding. */
    return nullptr;
  }

  Array<float> buffer(int64_t(ibuf->x) * int64_t(ibuf->y));
  mask_coverage_fill(channel, source, is_float, buffer);
  return std::make_shared<MaskSourceCoverage>(ibuf->x, ibuf->y, std::move(buffer));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling
 * \{ */

float interpolate_bilinear_channel_fl(const float *pixels,
                                      const int width,
                                      const int height,
                                      const float u,
                                      const float v,
                                      const bool wrap)
{
  const math::InterpWrapMode mode = wrap ? math::InterpWrapMode::Repeat :
                                           math::InterpWrapMode::Extend;
  float out;
  math::interpolate_bilinear_wrapmode_fl(pixels, &out, width, height, 1, u, v, mode, mode);
  return out;
}

bool MaskSourceCoverage::matches(const ImBuf &ibuf) const
{
  return this->width_ == ibuf.x && this->height_ == ibuf.y;
}

float MaskSourceCoverage::sample_unit_wrap(const float u, const float v) const
{
  /* Same pixel convention as #ChannelSourceSet::sample_baked: no clamp, the wrap sampler folds
   * out-of-range coordinates back into the image. */
  return interpolate_bilinear_channel_fl(this->buffer_.data(),
                                         this->width_,
                                         this->height_,
                                         u * float(this->width_),
                                         v * float(this->height_),
                                         true);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Fetch
 * \{ */

std::shared_ptr<const MaskSourceCoverage> mask_source_coverage_cache_get(
    const BrushMaterialPaint &brush_paint,
    const ChannelSourceSet &sources,
    const PaintModeSettings &settings)
{
  const std::optional<eMaterialPaintChannel> channel = mask_source_channel_get(brush_paint);
  if (!channel) {
    return nullptr;
  }
  const ChannelSourceSet::ChannelSource &source = sources.source(*channel);
  if (!source.usable || source.kind == ChannelSourceKind::Constant) {
    /* No texture, or the flat constant: nothing to precompute. */
    return nullptr;
  }
  if (source.alpha_weighted) {
    /* A layer bake's bilinear weights each texel's color by its alpha, which does not commute
     * with the per-texel reduction: precomputing would drift from the stroke at transparent
     * edges. The bake is already a prepared buffer, so the on-the-fly path is cheap here. */
    return nullptr;
  }
  if (source.kind == ChannelSourceKind::Mtex && !source.image_direct_sample) {
    /* Procedural or otherwise engine-sampled: the texture engine has no buffer to extract from. */
    return nullptr;
  }

  const MaskCoverageKey key = mask_coverage_key_build(*channel, source, brush_paint, settings);
  if (key.sharing == nullptr) {
    /* No sharing info, hence no identity the registry could use: the entry could not tell a
     * re-resolved buffer from the one it was built for. Stay off the registry. */
    return nullptr;
  }
  if (std::shared_ptr<const MaskSourceCoverage> cached = mask_coverage_cache_lookup(key)) {
    return cached;
  }

  std::shared_ptr<const MaskSourceCoverage> built = mask_source_coverage_build(*channel, source);
  if (built == nullptr) {
    return nullptr;
  }
  if (!mask_coverage_cache_insert(MaskCoverageKey(key), built)) {
    /* Not cached (over budget): the caller takes the on-the-fly path consistently, as it does
     * for a source the registry never accepts. */
    return nullptr;
  }
  return built;
}

void mask_source_coverage_cache_free_all()
{
  std::lock_guard lock(mask_coverage_cache_mutex);
  mask_coverage_cache.clear();
  mask_coverage_budget_bytes_used = 0;
}

void attach_direct_sample_coverage(DirectSampleLayout &layout, const MaskSourceCoverage &coverage)
{
  if (layout.kind == DirectSampleKind::Disabled ||
      (layout.float_pixels == nullptr && layout.byte_pixels == nullptr))
  {
    return;
  }
  if (layout.ibuf_x != coverage.width() || layout.ibuf_y != coverage.height()) {
    /* Built for a different pixel grid: stay on the image buffer. */
    return;
  }
  layout.float_pixels = nullptr;
  layout.byte_pixels = nullptr;
  layout.alpha_weighted = false;
  layout.coverage_pixels = coverage.data().data();
}

/** \} */

}  // namespace blender::ed::sculpt_paint::material
