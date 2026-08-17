/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Blending rules shared by the two material paint engines: the raster one that writes image maps
 * (sculpt_paint_image.cc, and the Image Editor's 2D path) and the vertex one that writes mesh
 * attributes (sculpt_paint_material.cc).
 *
 * \note What is deliberately *not* shared is how each engine accumulates a stroke.
 *
 * Both engines composite a per-stroke coverage accumulator against a frozen pre-stroke value,
 * because re-blending an already-blended value makes overlapping dabs of one stroke compound -
 * banding where their falloffs meet, and runaway saturation for modes like Add. They differ only
 * in where those two things come from: the vertex engine reads the pre-stroke value from the
 * sculpt undo step and keys the accumulator by vertex (#StrokeCache::material_mix_scalars), while
 * the raster engine captures it per texel on first touch and keys the accumulator by texel block
 * (#paint::image::MaterialStrokeAccum).
 *
 * The raster engine keeps one shortcut: Mix converges when applied dab after dab, so it skips the
 * accumulator and blends straight into the live image, where the image undo system preserves the
 * pre-stroke state. That shortcut is only valid for Mix.
 *
 * Only the per-element math below is common, and it lives here so the two engines cannot drift
 * apart on what a channel value *means*.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"

namespace blender::ed::sculpt_paint::material {

/**
 * The scalar-channel convention: a scalar is the gray color with that value in every component.
 *
 * Both engines rely on this. The raster engine writes the gray into an image map; the vertex
 * engine runs it through #IMB_blend_color_float so that every brush blend mode works on a scalar
 * channel exactly as it would on the equivalent gray in Base Color, instead of the scalar
 * channels silently falling back to Mix for the modes nobody reimplemented.
 */
inline float3 scalar_as_color(const float value)
{
  return float3(value);
}

/**
 * Whether \a channel's write is scaled by the Alpha channel this stroke.
 *
 * Alpha masks every *other* channel but never its own write - otherwise painting Alpha would
 * multiply by itself. Both engines need this rule and both used to spell it out inline; keep it
 * here so they cannot disagree.
 *
 * \param alpha_masking_active: whether the stroke uses Alpha as a mask at all (the brush setting
 * plus a usable Alpha source).
 */
inline bool channel_uses_alpha_mask(const bool alpha_masking_active,
                                    const eMaterialPaintChannel channel)
{
  return alpha_masking_active && channel != PAINT_MATERIAL_CHANNEL_ALPHA;
}

/**
 * Composites \a target (scaled by \a factor) onto a scalar coverage accumulator, always with Mix
 * regardless of the channel's configured blend mode - mirroring how #blend_color_mix_float is used
 * for #StrokeCache::paint_brush.mix_colors.
 *
 * \a mix and the result are pre-multiplied (the value is already scaled by its alpha), matching
 * the "pre-multiplied alpha float blending modes" convention documented in
 * BLI_math_color_blend.h / math_color_blend_inline.cc.
 *
 * This is #blend_color_mix_float specialized to one component plus alpha: a scalar accumulator
 * carries no color, so the RGBA path would compute the identical result three times over. It must
 * stay in lockstep with that function - if the shared Mix formula ever changes, change this too.
 */
inline float2 accumulate_scalar_coverage(const float2 &mix, const float target, const float factor)
{
  /* dst = `mix`, src = pre-multiplied (target * factor, factor). `fac` is the source alpha, and
   * both components are a plain lerp because the source is already pre-multiplied. */
  if (factor <= 0.0f) {
    return mix;
  }
  const float mfac = 1.0f - factor;
  return float2(mfac * mix.x + target * factor, mfac * mix.y + factor);
}

/**
 * Whether \a blend_mode reads its source color pre-multiplied by that color's own alpha.
 *
 * #IMB_blend_color_float covers two families that disagree on this, and nothing in its signature
 * says which one a mode belongs to. The modes listed here fold the alpha into their algebra -
 * #blend_color_mul_float's "un-pre-multiplied > multiply > pre-multiplied, simplified", or
 * #blend_color_lighten_float's `map_alpha` division. Every other mode (Screen, Overlay, Soft
 * Light, ...) instead reads `src2` straight and uses its alpha purely as a mix factor, so handing
 * one of those a pre-multiplied color silently scales it down by the brush falloff.
 *
 * Listing the pre-multiplied family rather than the other one keeps this short and fails safe: an
 * unrecognized mode is treated as straight, which is what the majority are.
 */
inline bool blend_mode_source_is_premultiplied(const IMB_BlendMode blend_mode)
{
  return ELEM(blend_mode,
              IMB_BLEND_MIX,
              IMB_BLEND_NORMAL_MIX,
              IMB_BLEND_ADD,
              IMB_BLEND_SUB,
              IMB_BLEND_MUL,
              IMB_BLEND_LIGHTEN,
              IMB_BLEND_DARKEN,
              IMB_BLEND_ERASE_ALPHA,
              IMB_BLEND_ADD_ALPHA);
}

/**
 * Composites a stroke's coverage accumulator \a mix onto \a current with \a blend_mode.
 *
 * Both engines build \a mix pre-multiplied, so this converts it for the modes that want a straight
 * color and normalizes the result's alpha. It is the single place either engine should apply a
 * channel's blend mode - going straight to #IMB_blend_color_float skips both corrections.
 */
inline float4 composite_coverage(const float4 &current,
                                 const float4 &mix,
                                 const IMB_BlendMode blend_mode)
{
  const bool premultiplied = blend_mode_source_is_premultiplied(blend_mode);
  float4 source = mix;
  if (!premultiplied && mix.w > 0.0f) {
    source = float4(mix.x / mix.w, mix.y / mix.w, mix.z / mix.w, mix.w);
  }

  float4 result;
  IMB_blend_color_float(result, current, source, blend_mode);

  if (!premultiplied) {
    /* The straight-source modes write only RGB, leaving the destination alpha as whatever was in
     * it. The pre-multiplied ones all set it to the destination's own alpha; match that instead
     * of letting the coverage leak out as the result's alpha. */
    result.w = current.w;
  }
  return result;
}

/**
 * Applies \a blend_mode to a single scalar, blending \a current (as an opaque gray) against the
 * accumulated coverage \a target_mix (already pre-multiplied by its own alpha, as built up by
 * #accumulate_scalar_coverage).
 */
inline float apply_scalar_blend(const float current,
                                const float2 &target_mix,
                                const IMB_BlendMode blend_mode)
{
  const float4 current_color(scalar_as_color(current), 1.0f);
  const float4 mix_color(scalar_as_color(target_mix.x), target_mix.y);
  float4 result;
  IMB_blend_color_float(result, current_color, mix_color, blend_mode);
  return result.x;
}

}  // namespace blender::ed::sculpt_paint::material
