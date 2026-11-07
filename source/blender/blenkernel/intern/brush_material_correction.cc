/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_brush_material_correction.hh"

#include "BKE_paint.hh"

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BLO_read_write.hh"

#include "paint_layers/paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Lifetime
 * \{ */

BrushMaterialPaintCorrection *BKE_brush_material_paint_correction_ensure(
    BrushMaterialPaintChannel &channel)
{
  if (channel.correction == nullptr) {
    channel.correction = MEM_new<BrushMaterialPaintCorrection>(__func__);
  }
  return channel.correction;
}

void BKE_brush_material_paint_correction_delete(BrushMaterialPaintChannel &channel)
{
  MEM_SAFE_DELETE(channel.correction);
}

bool BKE_brush_material_paint_correction_active(const BrushMaterialPaintChannel &channel)
{
  return channel.correction != nullptr &&
         (channel.correction->flag & BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED) != 0;
}

const BrushMaterialPaintCorrection *BKE_brush_material_paint_correction_for_channel(
    const BrushMaterialPaint &brush_paint, const eMaterialPaintChannel channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return nullptr;
  }
  /* Material/Layer sources are baked or read from the node tree, so there is no per-channel image
   * for the correction to prepare. */
  if (brush_paint.source_mode != BRUSH_MATERIAL_PAINT_SOURCE_MAPS) {
    return nullptr;
  }
  /* The tangent-space Normal map must stay unit length, which no pointwise color stack promises;
   * its DirectX green flip is the dedicated correction. */
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return nullptr;
  }
  const BrushMaterialPaintChannel &paint_channel = brush_paint.channels[channel];
  if (paint_channel.correction == nullptr ||
      (paint_channel.correction->flag & BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED) == 0)
  {
    return nullptr;
  }
  return paint_channel.correction;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Application
 * \{ */

BrushMaterialPaintCorrectionPrepared BKE_brush_material_paint_correction_prepare(
    const BrushMaterialPaintCorrection &correction)
{
  BrushMaterialPaintCorrectionPrepared prepared;
  prepared.enabled = (correction.flag & BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED) != 0;
  /* One bit per stage, so a stage can be added without another field in the correction. */
  prepared.use_levels = (correction.flag & BRUSH_MATERIAL_PAINT_CORRECTION_LEVELS) != 0;
  prepared.use_hsv = (correction.flag & BRUSH_MATERIAL_PAINT_CORRECTION_HSV) != 0;
  prepared.invert = (correction.flag & BRUSH_MATERIAL_PAINT_CORRECTION_INVERT) != 0;

  const float span = correction.levels_in_white - correction.levels_in_black;
  prepared.levels_inv_span = span != 0.0f ? 1.0f / span : 0.0f;
  prepared.levels_in_black = correction.levels_in_black;
  /* Same degenerate-gamma rule as the stack filter: a zero gamma reads as zero, not a huge
   * exponent. */
  prepared.levels_inv_gamma = correction.levels_gamma != 0.0f ? 1.0f / correction.levels_gamma :
                                                                0.0f;
  prepared.levels_out_black = correction.levels_out_black;
  prepared.levels_out_white = correction.levels_out_white;

  prepared.hsv_hue_shift = correction.hsv_hue + 0.5f;
  prepared.hsv_saturation = correction.hsv_saturation;
  prepared.hsv_value = correction.hsv_value;
  return prepared;
}

/** The stack layer Levels filter's stage (see paint_layers_kind.hh), on the t in [0, 1] input. */
static float correction_levels_stage(const BrushMaterialPaintCorrectionPrepared &prepared,
                                     const float t)
{
  return paint_layer_levels_stage_apply(
      t, prepared.levels_inv_gamma, prepared.levels_out_black, prepared.levels_out_white);
}

/** The stack's Hue/Saturation/Value stage (see paint_layers_kind.hh), on the prepared constants. */
static void correction_hsv_stage(const BrushMaterialPaintCorrectionPrepared &prepared, float rgb[3])
{
  paint_layer_hsv_stage_apply(
      rgb, prepared.hsv_hue_shift, prepared.hsv_saturation, prepared.hsv_value);
}

static void correction_invert_stage(float rgb[3])
{
  for (int i = 0; i < 3; i++) {
    rgb[i] = paint_layer_invert_target(rgb[i]);
  }
}

void BKE_brush_material_paint_correction_apply_prepared_rgb(
    const BrushMaterialPaintCorrectionPrepared &prepared, float rgb[3])
{
  if (!prepared.enabled) {
    return;
  }
  /* The stage order is the stack's own filter chain's (see paint_layers_kind.hh): Levels first so
   * the other two work on the remapped range, Hue/Saturation/Value next because Invert is defined
   * on the result the user sees, Invert last. Reordering would make a brush correction and the
   * equivalent layer stack paint different texels. */
  if (prepared.use_levels) {
    for (int i = 0; i < 3; i++) {
      const float t = (rgb[i] - prepared.levels_in_black) * prepared.levels_inv_span;
      rgb[i] = correction_levels_stage(prepared, t);
    }
  }
  if (prepared.use_hsv) {
    correction_hsv_stage(prepared, rgb);
  }
  if (prepared.invert) {
    correction_invert_stage(rgb);
  }
}

void BKE_brush_material_paint_correction_apply_rgb(const BrushMaterialPaintCorrection &correction,
                                                   float rgb[3])
{
  BKE_brush_material_paint_correction_apply_prepared_rgb(
      BKE_brush_material_paint_correction_prepare(correction), rgb);
}

void BKE_brush_material_paint_correction_apply_rgb_batch(
    const BrushMaterialPaintCorrection &correction, MutableSpan<float3> colors)
{
  BKE_brush_material_paint_correction_apply_prepared_rgb_batch(
      BKE_brush_material_paint_correction_prepare(correction), colors);
}

void BKE_brush_material_paint_correction_apply_prepared_rgb_batch(
    const BrushMaterialPaintCorrectionPrepared &prepared, MutableSpan<float3> colors)
{
  if (!prepared.enabled) {
    return;
  }
  for (float3 &color : colors) {
    float rgb[3] = {color.x, color.y, color.z};
    BKE_brush_material_paint_correction_apply_prepared_rgb(prepared, rgb);
    color = float3(rgb[0], rgb[1], rgb[2]);
  }
}

float BKE_brush_material_paint_correction_apply_prepared_value(
    const BrushMaterialPaintCorrectionPrepared &prepared,
    const float value_min,
    const float value_max,
    float value)
{
  if (!prepared.enabled) {
    return value;
  }
  if (prepared.use_levels) {
    const float t = (BKE_paint_material_t_from_value(value_min, value_max, value) -
                     prepared.levels_in_black) *
                    prepared.levels_inv_span;
    value = BKE_paint_material_value_from_t(
        value_min, value_max, correction_levels_stage(prepared, t));
  }
  if (prepared.invert) {
    value = BKE_paint_material_value_invert(value_min, value_max, value);
  }
  return value;
}

float BKE_brush_material_paint_correction_apply_value(
    const BrushMaterialPaintCorrection &correction,
    const float value_min,
    const float value_max,
    float value)
{
  return BKE_brush_material_paint_correction_apply_prepared_value(
      BKE_brush_material_paint_correction_prepare(correction), value_min, value_max, value);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Identity
 * \{ */

/** FNV-1a, the one copy of its constants every correction hash draws from. */
static uint64_t correction_hash_mix(uint64_t hash, const uint64_t bits)
{
  hash ^= bits;
  return hash * 1099511628211ull;
}

uint64_t BKE_brush_material_paint_correction_hash(const BrushMaterialPaintCorrection &correction)
{
  /* An off correction changes nothing, so it also has no identity: reporting a hash for it would
   * make dragging the parameters of a switched-off correction throw away every cache and repaint
   * the cursor overlay for nothing. */
  if ((correction.flag & BRUSH_MATERIAL_PAINT_CORRECTION_ENABLED) == 0) {
    return 0;
  }
  /* Over the fields, not over the bytes: padding must never enter the hash (it is not
   * guaranteed zero for a struct built in memory), and a new parameter then only has to be
   * listed here instead of breaking a size assert. #get_default_hash takes at most five
   * arguments, so the fields fold into an inner hash first (the one way the codebase chains
   * them, see #LightSet::hash). */
  return get_default_hash(get_default_hash(correction.flag,
                                           correction.levels_in_black,
                                           correction.levels_in_white,
                                           correction.levels_gamma,
                                           correction.levels_out_black),
                          correction.levels_out_white,
                          correction.hsv_hue,
                          correction.hsv_saturation,
                          correction.hsv_value);
}

uint64_t BKE_brush_material_paint_corrections_hash(const BrushMaterialPaint &brush_paint)
{
  uint64_t hash = 14695981039346656037ull;
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const BrushMaterialPaintCorrection *correction =
        BKE_brush_material_paint_correction_for_channel(brush_paint, eMaterialPaintChannel(i));
    if (correction == nullptr) {
      continue;
    }
    /* The index takes part as well: moving the same correction to another channel changes what a
     * stroke paints, so it must change the identity too. */
    hash = correction_hash_mix(hash, uint64_t(i + 1));
    hash = correction_hash_mix(hash, BKE_brush_material_paint_correction_hash(*correction));
  }
  return hash;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend I/O
 * \{ */

void BKE_brush_material_paint_corrections_blend_write(BlendWriter *writer,
                                                      const BrushMaterialPaint &material_paint)
{
  /* Corrections are heap payloads behind each channel. The brush and every scene preset own
   * separate copies after #BKE_brush_material_paint_copy(), so each call serializes its own. */
  for (const BrushMaterialPaintChannel &channel : material_paint.channels) {
    if (channel.correction != nullptr) {
      writer->write_struct(channel.correction);
    }
  }
}

void BKE_brush_material_paint_corrections_blend_read(BlendDataReader *reader,
                                                     BrushMaterialPaint &material_paint)
{
  for (BrushMaterialPaintChannel &channel : material_paint.channels) {
    BLO_read_struct(reader, BrushMaterialPaintCorrection, &channel.correction);
  }
}

/** \} */

}  // namespace blender
