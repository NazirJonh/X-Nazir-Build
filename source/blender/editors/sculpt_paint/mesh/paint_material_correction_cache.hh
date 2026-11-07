/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Cache of corrected channel source images ("Correction Image").
 *
 * A correction is pointwise, so its result depends only on the source pixels and on the
 * correction parameters - both of which change rarely compared to how often a stroke samples them.
 * Without this cache every texel of every dab of every stroke paid Levels' `powf` and
 * Hue/Saturation/Value's `rgb_to_hsv`/`hsv_to_rgb` again, and a bilinear sample corrected the
 * interpolated result instead of the texels it came from.
 *
 * Entries are immutable once published: a stroke holds its own `shared_ptr`, so the stroke that is
 * running survives the cache replacing or evicting the entry (the same contract as
 * #ed::material_bake::material_source_bake_get).
 */

#pragma once

#include <cstdint>
#include <memory>

#include "BLI_math_vector_types.hh"

namespace blender {

struct BrushMaterialPaint;
struct BrushMaterialPaintCorrection;
struct Image;
struct ImBuf;

enum eMaterialPaintChannel : int8_t;

namespace ocio {
class ColorSpace;
}  // namespace ocio

namespace ed::sculpt_paint::material {

/**
 * How much corrected image data the whole session may hold.
 *
 * Declared beside the Mask Source coverage budget (#mask_coverage_cache_budget_bytes, 256 MiB) so
 * the two derived-image limits stay visible together: a correction buffer is 4x a coverage one
 * (RGBA floats against grey), and a stroke holds at most one per active channel, so the
 * session-wide ceiling for both caches together is this plus the coverage budget (~1 GiB). Raising
 * either constant means re-reading the other.
 */
constexpr int64_t correction_cache_max_bytes = int64_t(768) << 20;

/**
 * The #ImageUser state that selects *which* pixels a buffer holds.
 *
 * An image sequence, a movie or a multi-layer image keeps its #ID.session_uid and its
 * #ImageRuntime::pixel_version while the frame or the layer moves, so these have to be part of the
 * cache key or a stroke would keep correcting the previous frame.
 */
struct CorrectionSourceKey {
  int frame = 0;
  int layer = 0;
  int pass = 0;
  int view = 0;
  int tile = 0;

  friend bool operator==(const CorrectionSourceKey &a, const CorrectionSourceKey &b) = default;
};

/** Everything one corrected copy of a source image is a function of. */
struct CorrectionCacheRequest {
  const BrushMaterialPaint &brush_paint;
  /** Enabled correction of \a channel; the caller asked
   * #BKE_brush_material_paint_correction_for_channel first. */
  const BrushMaterialPaintCorrection &correction;
  eMaterialPaintChannel channel;
  const Image &image;
  /** The source buffer the stroke would otherwise sample. Never kept. */
  const ImBuf &source;
  /**
   * Colorspace the source's byte pixels are stored in, or null when they are already scene-linear
   * (a float buffer) or when the channel reads an intensity the stroke never decodes (a scalar
   * channel). Folded into the cached pixels, hence part of the key.
   */
  const ocio::ColorSpace *decode_colorspace;
  /** Which frame/layer/tile of \a image \a source holds. */
  CorrectionSourceKey source_key;
  /** Channel value range, which a scalar channel's correction is defined over. */
  float2 value_range;
  /**
   * True when the buffer is built for a Mask Source read: those correct the RAW coverage and
   * never fold the channel's own #BrushMaterialPaintChannel::source_invert, while a Maps-mode
   * read of the same channel does. The flag is part of the buffer's identity (it decides what the
   * texels hold and is mixed into the cache key), so the two readers never share one buffer.
   */
  bool for_mask = false;
};

/**
 * The corrected copy of the request's source image, or null when the correction cache cannot serve
 * it (allocation failed, or the source grew past the budget) and the caller has to keep correcting
 * on the fly.
 *
 * The returned buffer holds RGBA floats in scene-linear space with the correction already applied
 * to RGB; alpha is the source's own. A scalar channel's corrected value is stored replicated into
 * RGB, because a scalar channel reads the luminance of its source.
 *
 * Ready to be swapped into #ChannelSourceSet::ChannelSource::ibuf: same size, four float channels,
 * no colorspace left to undo.
 */
std::shared_ptr<const ImBuf> correction_cache_get(const CorrectionCacheRequest &request);

/** Hit, miss and build counters plus the bytes currently held. Exists for tests and debugging. */
struct CorrectionCacheStats {
  /** Requests answered from the cache without touching the pixels. */
  uint64_t hits = 0;
  /** Requests that had to build a buffer from scratch. */
  uint64_t builds = 0;
  /** Requests served by re-correcting only the edited rectangle of an existing buffer. */
  uint64_t region_updates = 0;
  size_t bytes = 0;
  size_t entries = 0;
};
CorrectionCacheStats correction_cache_stats();

/** Drop every entry. Dropping an entry a running stroke holds only releases the cache's share. */
void correction_cache_clear();

}  // namespace ed::sculpt_paint::material
}  // namespace blender
