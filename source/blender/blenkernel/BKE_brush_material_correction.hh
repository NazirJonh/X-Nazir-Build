/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Source-image correction ("Correction Image") of a material paint channel: the pointwise
 * Levels / Hue-Saturation-Value / Invert stack, the answers every consumer needs about it
 * (whether a channel has one, its identity hash), the batch entry points, and the ownership
 * helpers that keep one copy per owner.
 *
 * Kept out of `BKE_paint.hh` because that header sits on the include path of nearly every paint
 * file, and the correction is a self-contained stack that only the stroke, the 2D engine, the
 * curve patch, the cursor preview and the brush's own copy/free paths need.
 */

#pragma once

#include <cstdint>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "DNA_brush_types.h"

struct BlendDataReader;
struct BlendWriter;
struct BrushMaterialPaint;
struct BrushMaterialPaintChannel;
struct BrushMaterialPaintCorrection;

enum eMaterialPaintChannel : int8_t;

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Lifetime
 * \{ */

/** Allocates the channel's correction with neutral values when it has none. */
BrushMaterialPaintCorrection *BKE_brush_material_paint_correction_ensure(
    BrushMaterialPaintChannel &channel);

/** Releases the channel's correction, switching the feature off. */
void BKE_brush_material_paint_correction_delete(BrushMaterialPaintChannel &channel);

/** Whether the channel carries an enabled correction, whatever the source mode says. */
bool BKE_brush_material_paint_correction_active(const BrushMaterialPaintChannel &channel);

/**
 * The correction a stroke path may apply to \a channel, or null when it must not apply.
 *
 * The single answer to "is this channel corrected right now": the correction prepares a channel's
 * own Maps-mode source image, so it never applies to Material/Layer bakes, never to the
 * tangent-space Normal map, and not at all while switched off. Every consumer asks this instead of
 * repeating the four conditions, so the cursor preview, the 2D engine and the Sculpt stroke can
 * not drift apart.
 */
const BrushMaterialPaintCorrection *BKE_brush_material_paint_correction_for_channel(
    const BrushMaterialPaint &brush_paint, eMaterialPaintChannel channel);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Application
 * \{ */

/**
 * The per-texel constants of one enabled correction, derived once instead of per sample.
 *
 * Plain data, never saved: it only mirrors #BrushMaterialPaintCorrection with the divisions
 * already done. Building it is the cheap half of the work, applying it the expensive one, so a
 * batch builds it once and a single sample builds it on the spot.
 */
struct BrushMaterialPaintCorrectionPrepared {
  float levels_inv_span = 0.0f;
  float levels_inv_gamma = 0.0f;
  float levels_in_black = 0.0f;
  float levels_out_black = 0.0f;
  float levels_out_white = 0.0f;
  float hsv_hue_shift = 0.0f;
  float hsv_saturation = 1.0f;
  float hsv_value = 1.0f;
  bool use_levels = false;
  bool use_hsv = false;
  bool invert = false;
  bool enabled = false;
};

BrushMaterialPaintCorrectionPrepared BKE_brush_material_paint_correction_prepare(
    const BrushMaterialPaintCorrection &correction);

/**
 * Applies the correction to an RGB sample in scene-linear space. No-op while the correction is
 * disabled. Fixed order: Levels, then Hue/Saturation/Value, then Invert.
 */
void BKE_brush_material_paint_correction_apply_rgb(const BrushMaterialPaintCorrection &correction,
                                                   float rgb[3]);

/**
 * The batched form of #BKE_brush_material_paint_correction_apply_rgb: the only loop over a span of
 * colors the correction has, so every engine (Sculpt strokes, the Image Editor's dabs, the curve
 * patch) applies exactly the same per-texel math.
 */
void BKE_brush_material_paint_correction_apply_rgb_batch(
    const BrushMaterialPaintCorrection &correction, MutableSpan<float3> colors);

void BKE_brush_material_paint_correction_apply_prepared_rgb(
    const BrushMaterialPaintCorrectionPrepared &prepared, float rgb[3]);

/**
 * The batched form of #BKE_brush_material_paint_correction_apply_prepared_rgb: the prepared
 * constants of #BKE_brush_material_paint_correction_apply_rgb_batch, for a caller that keeps them
 * (a #ChannelSourceSet resolves them once per stroke).
 */
void BKE_brush_material_paint_correction_apply_prepared_rgb_batch(
    const BrushMaterialPaintCorrectionPrepared &prepared, MutableSpan<float3> colors);

/**
 * Applies the correction to a scalar sample in the channel's [value_min, value_max] range:
 * Levels through the range-relative t, then Invert. Hue/Saturation/Value has no meaning for a
 * scalar and is skipped. No-op while the correction is disabled.
 */
float BKE_brush_material_paint_correction_apply_value(
    const BrushMaterialPaintCorrection &correction,
    float value_min,
    float value_max,
    float value);

/** The prepared form of #BKE_brush_material_paint_correction_apply_value. */
float BKE_brush_material_paint_correction_apply_prepared_value(
    const BrushMaterialPaintCorrectionPrepared &prepared,
    float value_min,
    float value_max,
    float value);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Identity
 * \{ */

/**
 * A hash over the correction's parameters; changes whenever any parameter does.
 *
 * Zero while the correction is disabled: an off correction changes nothing, so it must not
 * invalidate anything a consumer cached against this hash.
 */
uint64_t BKE_brush_material_paint_correction_hash(const BrushMaterialPaintCorrection &correction);

/**
 * Identity of every correction the stroke would apply to \a brush_paint, mixed with the channel
 * the correction sits on, so moving a correction to another channel changes the hash.
 *
 * One implementation of the mixing for every consumer (the cursor's overlay snapshot, caches that
 * derive a result from corrected texels): two of them would drift apart and one would stop
 * invalidating.
 */
uint64_t BKE_brush_material_paint_corrections_hash(const BrushMaterialPaint &brush_paint);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend I/O
 * \{ */

/**
 * Serializes the channel corrections owned by \a material_paint. Must be called next to every
 * `BrushMaterialPaint` blend write/read, after the parent struct: the corrections are non-ID heap
 * payloads, so blending the parent struct alone would leave stale pointers behind on load.
 */
void BKE_brush_material_paint_corrections_blend_write(BlendWriter *writer,
                                                      const BrushMaterialPaint &material_paint);
void BKE_brush_material_paint_corrections_blend_read(BlendDataReader *reader,
                                                     BrushMaterialPaint &material_paint);

/** \} */

}  // namespace blender
