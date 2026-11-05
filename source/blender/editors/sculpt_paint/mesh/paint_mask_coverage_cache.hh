/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Cached coverage of a Mask Source texture.
 *
 * A Mask stroke reduces the selected source (Alpha or Base Color) to one grey per texel, then
 * scales the dab's coverage by it. Without a cache that grey is re-derived - bilinear sample,
 * colorspace decode, luminance - for every texel of every dab, although the source pixels do not
 * change during a stroke. #MaskSourceCoverage holds the grey computed once per source texel, so a
 * dab samples a single-channel buffer instead: the expensive stage is paid once per source, not
 * once per stroke position.
 *
 * The buffer keeps the UN-inverted grey: toggling Invert Source does not rebuild anything, the
 * `1 - c` mirror is applied where the coverage is read (see
 * #ed::sculpt_paint::material::mask_source_invert_get). Sources that cannot be represented this
 * way - procedural textures, alpha-weighted layer bakes, oversized buffers - are simply never
 * cached and keep the computed-on-the-fly path.
 */

#include <memory>
#include <utility>

#include "BLI_array.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"

#include "paint_material_source.hh"

namespace blender::ed::sculpt_paint::material {

/**
 * The grey a Mask Source reduces to, one float per texel of the source image.
 *
 * The extraction reproduces exactly what the stroke's own per-texel path computes for a single
 * texel: the Alpha rule for an Alpha source (the direct sampler's intensity, or the bake's first
 * channel for Target UV placement) and the scene-linear luminance for a Base Color source. Since
 * both are linear in the texel color, a bilinear sample over this buffer equals the per-texel
 * path's bilinear sample followed by the same reduction - the cache cannot drift from the pixels.
 */
class MaskSourceCoverage {
 public:
  /**
   * Build from an already-extracted grey, row-major over a \a width x \a height grid. The public
   * way to fill the buffer, so the extraction code (and the tests that pin its rules) need no
   * access to the storage.
   */
  MaskSourceCoverage(const int width, const int height, Array<float> &&buffer)
      : buffer_(std::move(buffer)), width_(width), height_(height)
  {
    BLI_assert(buffer_.size() == int64_t(width) * int64_t(height));
  }

  int width() const
  {
    return width_;
  }

  int height() const
  {
    return height_;
  }

  Span<float> data() const
  {
    return buffer_;
  }

  /** Whether this coverage was built for \a ibuf's pixel grid (same size). A stroke checks this
   * before sampling, so a source whose buffer was re-resolved mid-stroke never reads a stale
   * grid. */
  bool matches(const ImBuf &ibuf) const;

  /**
   * Bilinear at \a u, \a v over the whole image, wrapping at the edges. The pixel convention and
   * wrap behavior of #ChannelSourceSet::sample_baked, whose stride-4 input this buffer replaces
   * for Baked Target UV sources.
   */
  float sample_unit_wrap(float u, float v) const;

 private:
  Array<float> buffer_;
  int width_ = 0;
  int height_ = 0;
};

/**
 * The coverage of the brush's resolved Mask Source, built once per source and reused by later
 * strokes and painters until the source's pixels change.
 *
 * Returns null when the resolved source has no texture, cannot be sampled directly (a procedural
 * texture keeps the texture-engine path), is an alpha-weighted layer bake (whose weighted bilinear
 * does not commute with the per-texel reduction), carries no sharing info to key the entry on, or
 * is too large for the cache budget.
 */
std::shared_ptr<const MaskSourceCoverage> mask_source_coverage_cache_get(
    const BrushMaterialPaint &brush_paint,
    const ChannelSourceSet &sources,
    const PaintModeSettings &settings);

/* -------------------------------------------------------------------- */
/** \name Registry (module-internal)
 *
 * The registry behind #mask_source_coverage_cache_get, exposed so the module's tests can pin the
 * eviction, budget and pinning rules; strokes and painters only ever go through the fetch above.
 * \{ */

/**
 * Exact-match identity of a cached coverage. The hash only picks candidates; a hit requires every
 * field to compare equal, so a colliding hash can never hand out the wrong buffer. Hashed with the
 * module's shared #IdentityHasher, like the shape tools' source key.
 *
 * The mapping (Size / Angle / map mode) is deliberately absent - the buffer is stored in the
 * source's own pixel space and the placement is applied at sample time - and so is the invert
 * flag: the buffer holds the un-inverted grey, so toggling Invert Source never rebuilds it.
 *
 * The key holds no strong user of the pixels: #ImBuf::float_data_for_write clones a buffer that
 * has more than one user, so a pin would silently turn every write to the source image into a
 * copy-on-write and leave painting in the copy. Identity is weak instead, and
 * #pixel_version makes an in-place rewrite visible: it draws from
 * #bke::image_pixel_version_next on every marked pixel change (paint, reload, undo).
 */
struct MaskCoverageKey {
  /** The channel decides the reduction rule (alpha-rule intensity versus luminance). */
  eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ChannelSourceKind kind = ChannelSourceKind::None;
  /** Baked Target UV placement reads the bake's first channel, not the direct sampler's
   * intensity, so the same bake under the two placements gets its own entry. */
  bool baked_target_uv = false;
  /** #BrushMaterialPaint.source_mode: the same image can back a Maps slot and a material node. */
  char source_mode = 0;
  /** #PaintModeSettings.layer_target_mode. */
  char layer_target_mode = 0;

  /** Where the pixels come from: the #Tex slot for #Mtex, the node's image for #Image. */
  const Tex *tex = nullptr;
  const Image *image = nullptr;
  /** The buffer itself, plus the fields an edit or a re-resolve changes. */
  const ImBuf *ibuf = nullptr;
  const void *pixels = nullptr;
  const void *sharing = nullptr;
  int64_t userflags = 0;
  /** #ImageRuntime::pixel_version of #image, 0 when the source is not backed by an #Image. */
  uint64_t pixel_version = 0;
  /** #ed::material_bake::MaterialSourceBake::revision for a baked source, 0 otherwise. A bake has
   * no #Image and is replaced wholesale, so its own monotonic identity stands in for
   * #pixel_version and keeps a reused buffer address from matching an older entry. */
  uint64_t baked_revision = 0;
  const void *colorspace = nullptr;
  bool is_float = false;
  int width = 0;
  int height = 0;

  uint64_t hash() const;

  friend bool operator==(const MaskCoverageKey &a, const MaskCoverageKey &b)
  {
    return a.channel == b.channel && a.kind == b.kind && a.baked_target_uv == b.baked_target_uv &&
           a.source_mode == b.source_mode && a.layer_target_mode == b.layer_target_mode &&
           a.tex == b.tex && a.image == b.image && a.ibuf == b.ibuf && a.pixels == b.pixels &&
           a.sharing == b.sharing && a.userflags == b.userflags &&
           a.pixel_version == b.pixel_version && a.baked_revision == b.baked_revision &&
           a.colorspace == b.colorspace &&
           a.is_float == b.is_float && a.width == b.width && a.height == b.height;
  }
};

/** Largest source that is still worth caching: 4096x4096, 64 MiB as one float per texel. */
constexpr int64_t mask_coverage_cache_max_pixels = int64_t(1) << 22;
/** Entry count and byte budget of the whole registry. */
constexpr int mask_coverage_cache_max_entries = 4;
constexpr int64_t mask_coverage_cache_budget_bytes = int64_t(256) << 20;

/* The per-source gate above also caps one entry's coverage: even a registry full of the largest
 * accepted sources holds 4 x 64 MiB of grey, exactly the byte budget, so the count limit alone
 * cannot breach it. The budget loop in #mask_coverage_cache_insert still guards the invariant
 * against future limit changes. */
static_assert(mask_coverage_cache_max_entries * mask_coverage_cache_max_pixels * sizeof(float) <=
                  mask_coverage_cache_budget_bytes,
              "Coverage of the largest accepted sources must fit the byte budget.");

/**
 * Store \a coverage under \a key (see the .cc for the eviction and budget rules). The entry holds
 * only the derived grey; the pixels it was built from are identified weakly by \a key.
 *
 * \return false when even an empty registry could not hold it (a single entry above the budget).
 */
bool mask_coverage_cache_insert(MaskCoverageKey &&key,
                                std::shared_ptr<const MaskSourceCoverage> coverage);

/** The cached coverage for \a key, or null. */
std::shared_ptr<const MaskSourceCoverage> mask_coverage_cache_lookup(const MaskCoverageKey &key);

/** \} */

/**
 * Build the coverage for \a source's pinned buffer right now, bypassing the registry. The fetch
 * above is this plus reuse across strokes; exported so the extraction rules can be pinned by
 * tests against a fabricated source. Null when the source has no usable pixel buffer.
 */
std::shared_ptr<const MaskSourceCoverage> mask_source_coverage_build(
    eMaterialPaintChannel channel, const ChannelSourceSet::ChannelSource &source);

/**
 * Swap a direct-sample layout's image buffer for \a coverage, so #sample_direct_layout returns the
 * precomputed grey instead of the source's colors.
 *
 * The layout keeps its own placement (Size / Offset / mapping mode / wrap), which is why the cache
 * key does not include the mapping: the buffer is stored in the source's own pixel space. No-op -
 * the layout stays on its image buffer - when \a coverage was not built for the same pixel grid,
 * which leaves the caller on the computed-on-the-fly path.
 */
void attach_direct_sample_coverage(DirectSampleLayout &layout, const MaskSourceCoverage &coverage);

/** Raw bilinear over a single-channel buffer, the stride-1 counterpart of the stride-4 samplers
 * the direct layout uses. Exposed for the 2D engine's Area Plane path, which reaches its source
 * through #ChannelSourceSet::sample_image_direct rather than through a layout. */
float interpolate_bilinear_channel_fl(const float *pixels,
                                      int width,
                                      int height,
                                      float u,
                                      float v,
                                      bool wrap);

}  // namespace blender::ed::sculpt_paint::material
