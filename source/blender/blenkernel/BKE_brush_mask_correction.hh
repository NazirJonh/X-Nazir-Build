/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Extended correction of the Mask slot's scalar coverage ("Correction Image"): the pointwise
 * Levels / Smoothstep / Curve / Threshold / Posterize / Clamp stack, the image-only
 * spatial Blur / Sharpen / Erode-Dilate / Median filters, the identity hash every cache keys on,
 * and the ownership helpers that keep one correction per brush.
 *
 * Kept out of `BKE_brush.hh` for the same reason as `BKE_brush_material_correction.hh`: only the
 * mask samplers, the brush copy/free/blend paths and the RNA/UI need it, and the spatial cache
 * drags in the image module.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "BLI_span.hh"

#include "DNA_brush_types.h"

struct BlendDataReader;
struct BlendWriter;

namespace blender {

struct Brush;
struct ImagePool;

/* -------------------------------------------------------------------- */
/** \name Lifetime
 * \{ */

/**
 * Allocates the brush's correction with neutral values and a default identity curve when it has
 * none. The master #BRUSH_MASK_CORRECTION_ENABLED bit is left to the caller, so a plainly ensured
 * block can be filled before it starts affecting strokes.
 */
BrushMaskCorrection *BKE_brush_mask_correction_ensure(Brush &brush);

/** Releases the brush's correction, switching the feature off. */
void BKE_brush_mask_correction_delete(Brush &brush);

/** Deep-copies \a src, including its curve; null in, null out. */
BrushMaskCorrection *BKE_brush_mask_correction_copy(const BrushMaskCorrection *src);

/** Releases a correction and its curve without touching a brush. */
void BKE_brush_mask_correction_free(BrushMaskCorrection *correction);

/** The brush's correction while it is switched on, otherwise null. */
const BrushMaskCorrection *BKE_brush_mask_correction_get(const Brush &brush);

/** Whether the brush carries an enabled Mask correction. */
bool BKE_brush_mask_correction_active(const Brush &brush);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Application
 * \{ */

/**
 * The per-texel constants of one enabled correction, derived once instead of per sample.
 *
 * Plain data, never saved: it mirrors #BrushMaskCorrection with the divisions already done and
 * the stage switches cached. A single sample builds it on the spot; the spatial cache builder and
 * tests keep one for a whole buffer.
 *
 * \note `curve` is borrowed from the correction. #BrushMaskCorrectionSampler replaces it with its
 * own snapshot before publishing the prepared data to worker threads.
 */
struct BrushMaskCorrectionPrepared {
  float levels_inv_span = 0.0f;
  float levels_inv_gamma = 0.0f;
  float levels_in_black = 0.0f;
  float levels_out_black = 0.0f;
  float levels_out_white = 1.0f;

  float smoothstep_min = 0.0f;
  float smoothstep_inv_span = 0.0f;
  float smoothstep_softness = 1.0f;

  float threshold = 0.5f;
  float clamp_min = 0.0f;
  float clamp_max = 1.0f;
  int posterize_steps = 1;

  bool enabled = false;
  bool use_levels = false;
  bool use_smoothstep = false;
  bool use_curve = false;
  bool use_threshold = false;
  bool use_posterize = false;
  bool use_clamp = false;

  bool use_blur = false;
  bool use_sharpen = false;
  bool use_morph = false;
  bool use_median = false;

  /** Quantized texel radii, derived once so the builder and the activity test cannot disagree.
   * Zero means the stage is off, even when its flag is set. */
  int blur_radius = 0;
  int sharpen_radius = 0;
  int morph_radius = 0;
  int median_radius = 0;
  float sharpen_amount = 0.0f;

  const CurveMapping *curve = nullptr;

  /** Whether any image-only spatial stage is switched on. */
  bool use_spatial() const
  {
    return this->use_blur || this->use_sharpen || this->use_morph || this->use_median;
  }
};

BrushMaskCorrectionPrepared BKE_brush_mask_correction_prepare(
    const BrushMaskCorrection &correction);

/**
 * Applies the pointwise stack to one scalar. Fixed order: Levels, Smoothstep, Curve, Threshold,
 * Posterize, Clamp; the result is clamped to [0, 1]. No-op while the correction is disabled.
 */
float BKE_brush_mask_correction_apply_prepared_value(
    const BrushMaskCorrectionPrepared &prepared, float value);

/**
 * The one-shot form of #BKE_brush_mask_correction_apply_prepared_value.
 *
 * Serial use only: it prepares the stack on every call and borrows the correction's live
 * #CurveMapping instead of taking a snapshot. A stroke worker must use a
 * #BrushMaskCorrectionSampler resolved once for the stroke; these one-shot helpers are for the
 * UI/serial call sites and tests.
 */
float BKE_brush_mask_correction_apply_value(const BrushMaskCorrection &correction, float value);

/** The brush's pointwise Mask correction, or returns \a value unchanged when it is off.
 *
 * Same lifetime/threading contract as #BKE_brush_mask_correction_apply_value: serial only. */
float BKE_brush_mask_correction_apply(const Brush &brush, float value);

/**
 * Reduces the sampled RGBA to the channel the correction reads.
 *
 * \a intensity is the texture engine's own scalar (luminance of the RGBA for RGB textures), used
 * for #BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE so the uncorrected path stays byte-for-byte the
 * value the sampler already had. The other channels read \a rgba directly.
 */
float BKE_brush_mask_correction_channel_value(const BrushMaskCorrection &correction,
                                              float intensity,
                                              const float rgba[4]);

/** The brush-aware form of #BKE_brush_mask_correction_channel_value; returns \a intensity when
 * the brush has no enabled correction, so callers can stay on the old path exactly. */
float BKE_brush_mask_correction_channel_value_for_brush(const Brush &brush,
                                                       float intensity,
                                                       const float rgba[4]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spatial image cache
 * \{ */

/**
 * The spatially filtered channel of an image-texture Mask slot at texture coordinates \a x / \a y,
 * or null when spatial correction cannot be used (no correction, no spatial stage, procedural
 * texture, 3D mapping, missing pixel buffer, or a case the single-channel map cannot represent
 * exactly - see #BrushMaskCorrectionSampler::sample_image).
 *
 * The returned value has already been channel-selected and spatially filtered, but **not**
 * pointwise-corrected: the samplers apply the pointwise stack after the mask slot's own invert,
 * which keeps the whole pipeline's order identical for spatial and procedural sources. Callers
 * that get a value must skip the texture engine for this sample; callers that get null keep the
 * ordinary engine sample and the pointwise stack.
 *
 * This one-shot form resolves the sampler again on every call. Hot paths (a stroke, a Curve Patch
 * chunk) must keep a #BrushMaskCorrectionSampler instead: `prepare()`, the identity hash, the
 * buffer acquire and the cache lookup are then paid once, not per sample.
 *
 * \a x and \a y are the same coordinates the caller would pass to `RE_texture_evaluate`; the
 * placement (#MTex size/offset) is applied here exactly as the texture engine does.
 */
std::optional<float> BKE_brush_mask_correction_sample_image(const Brush &brush,
                                                            const MTex &mtex,
                                                            float x,
                                                            float y,
                                                            ImagePool *pool);

/** The corrected map, opaque outside this module. */
struct MaskCorrectionMap;

/**
 * Plain copy of the #Tex placement fields a resolved sampler still needs. Keeping them by value
 * means the sampler stays valid after the #Tex / #Brush is freed or replaced mid-stroke.
 */
struct BrushMaskCorrectionTexPlacement {
  eTex_Extend extend = TEX_EXTEND;
  eTex_Flag flag = {};
  eTex_ImaFlag imaflag = {};
  short xrepeat = 1;
  short yrepeat = 1;
  float cropxmin = 0.0f;
  float cropxmax = 1.0f;
  float cropymin = 0.0f;
  float cropymax = 1.0f;
};

/**
 * A spatial Mask sampler resolved once for a stroke, sampler or chunk.
 *
 * Construct it with #resolve before the hot loop and call #sample_image for every sample. The
 * pointwise stack is kept here too, so even a fallback (procedural texture, non-representable
 * alpha/interpolation, missing buffer) never calls #BKE_brush_mask_correction_prepare again.
 *
 * Immutable after #resolve and therefore safe to share between the worker threads of a stroke;
 * the map is held through a `shared_ptr`, so a cache eviction cannot pull it from under a
 * running sampler, and the correction curve is deep-copied so an edit cannot change the stack
 * mid-stroke. The object is cheap to copy (the prepared data is plain, the payloads are shared).
 */
class BrushMaskCorrectionSampler {
 public:
  BrushMaskCorrectionSampler() = default;

  /**
   * Resolve the brush's correction, its hash, the image buffer identity and the cached corrected
   * map for \a mtex. Prefer the correction's channel-augmented cache key; the map is built only
   * once per image and correction, and reused from the shared cache on later strokes.
   *
   * When spatial sampling cannot represent the slot exactly (see #sample_image) the sampler
   * still carries #prepared and #hash: the caller keeps the engine path and only the pointwise
   * stack uses this object.
   */
  static BrushMaskCorrectionSampler resolve(const Brush &brush,
                                            const MTex &mtex,
                                            ImagePool *pool);

  /** Whether the brush carries an enabled correction at all. */
  bool enabled() const
  {
    return prepared_.enabled;
  }

  /** Whether #sample_image can answer without the texture engine. */
  bool has_spatial_map() const
  {
    return map_ != nullptr;
  }

  /** The correction identity the cache was keyed on, 0 when it is off. */
  uint64_t hash() const
  {
    return hash_;
  }

  const BrushMaskCorrectionPrepared &prepared() const
  {
    return prepared_;
  }

  /**
   * Sample the resolved map at the texture coordinates \a x / \a y (the same coordinates the
   * caller passes to `RE_texture_evaluate`). Null when no map was resolved, in which case the
   * caller has to evaluate the texture engine instead.
   *
   * Exactness contract: the map reproduces the engine for nearest sampling, and for bilinear
   * sampling at `TEX_INTERPOL` with the default `filtersize == 1`. Cases whose per-channel
   * filtering followed by the engine's de-premultiply cannot be represented in one channel
   * (variable alpha with a non-alpha channel, `TEX_CALCALPHA`) resolve without a map and take
   * the engine + pointwise fallback. `filtersize != 1` and bilinear `TEX_CLIP` are likewise a
   * fallback because the box filter/edge scaling would have to run before the channel is reduced.
   */
  std::optional<float> sample_image(float x, float y) const;

  /** Apply the resolved pointwise stack; a no-op when the correction is disabled. */
  float apply_pointwise(float value) const;

 private:
  BrushMaskCorrectionPrepared prepared_;
  uint64_t hash_ = 0;
  /** Placement of the slot, copied so the hot loop does not read a live #Tex or #MTex. */
  BrushMaskCorrectionTexPlacement placement_;
  /** Deep copy of the correction curve while #CURVE is on: the stroke reads this stable table,
   * not the one the RNA editor may rebuild underneath it. */
  std::shared_ptr<CurveMapping> curve_snapshot_;
  float size_[2] = {1.0f, 1.0f};
  float ofs_[2] = {0.0f, 0.0f};
  std::shared_ptr<const MaskCorrectionMap> map_;
};

/** Cache counters, for the tests and diagnostics. */
struct BrushMaskCorrectionCacheStats {
  uint64_t builds = 0;
  uint64_t hits = 0;
  uint64_t evictions = 0;
  uint64_t entries = 0;
  uint64_t bytes = 0;
};

BrushMaskCorrectionCacheStats BKE_brush_mask_correction_cache_stats();

/** Drops every cached corrected map. Called by #BKE_brush_system_exit and by the tests. */
void BKE_brush_mask_correction_cache_clear();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Identity
 * \{ */

/**
 * A hash over the correction's parameters, including the channel and the curve's edit timestamp;
 * zero while the correction is disabled, so an off correction invalidates nothing.
 */
uint64_t BKE_brush_mask_correction_hash(const BrushMaskCorrection &correction);

/** Hash of the brush's correction, or 0 when it carries none / it is switched off. */
uint64_t BKE_brush_mask_correction_hash_for_brush(const Brush &brush);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend I/O
 * \{ */

/**
 * Serializes the brush's correction and its curve. Must be called next to every brush blend
 * write/read, after the parent struct: the correction is a non-ID heap payload, so blending the
 * brush alone would leave a stale pointer behind on load.
 */
void BKE_brush_mask_correction_blend_write(BlendWriter *writer, const Brush &brush);
void BKE_brush_mask_correction_blend_read(BlendDataReader *reader, Brush &brush);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Testable spatial filters
 * \{ */

namespace bke::mask_correction {

/**
 * Separable Gaussian or box blur of \a values in place, radius in pixels. Edges clamp, so a mask
 * that reaches the image border is not darkened by the outside.
 */
void blur_apply(MutableSpan<float> values,
                int width,
                int height,
                int radius,
                bool gaussian);

/** Unsharp mask: `v + amount * (v - blur(v))`, clamped to [0, 1]. */
void sharpen_apply(MutableSpan<float> values,
                   int width,
                   int height,
                   int radius,
                   float amount);

/** Separable square-kernel minimum (erode) / maximum (dilate). */
void morph_apply(MutableSpan<float> values,
                 int width,
                 int height,
                 int radius,
                 bool dilate);

/** Square-kernel median, radius clamped to the 1..4 the stage allows. */
void median_apply(MutableSpan<float> values, int width, int height, int radius);

}  // namespace bke::mask_correction

/** \} */

}  // namespace blender
