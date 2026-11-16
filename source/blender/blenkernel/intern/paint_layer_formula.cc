/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The implementations of the shared paint-layer formulas whose bodies are not inline: the HSV
 * stage, the texel hash and the noise families it feeds, and the Channel Mixer's vector read-out.
 * The parameter layouts and the pointwise formulas live in the header.
 */

#include "paint_layer_formula.hh"

#include <algorithm>

#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_utildefines.h"

#include "IMB_colormanagement.hh"

namespace blender {

void paint_layer_hsv_stage_apply(float rgb[3],
                                 const float hue_turns,
                                 const float saturation,
                                 const float value)
{
  float hsv[3];
  rgb_to_hsv_v(rgb, hsv);
  const float turned = hsv[0] + hue_turns;
  hsv[0] = turned - floorf(turned);
  hsv[1] = clamp_f(hsv[1] * saturation, 0.0f, 1.0f);
  hsv[2] *= value;
  hsv_to_rgb_v(hsv, rgb);
  for (int i = 0; i < 3; i++) {
    rgb[i] = std::max(rgb[i], 0.0f);
  }
}

void paint_layer_hsl_apply(float rgb[3], const float params[PAINT_LAYER_HSL_PARAM_NUM])
{
  paint_layer_hsv_stage_apply(rgb,
                              params[PAINT_LAYER_HSL_HUE] + 0.5f,
                              params[PAINT_LAYER_HSL_SATURATION],
                              params[PAINT_LAYER_HSL_VALUE]);
  /* The vibrance lifts the channels away from their Rec.709 grey, most where the colour is least
   * saturated, and clamps below zero like the HSV branch above. The same coefficients the RGB to BW
   * node reads, so the graph and this agree. */
  const float vibrance = params[PAINT_LAYER_HSL_VIBRANCE];
  if (vibrance != 0.0f) {
    const float max_c = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    const float min_c = std::min(rgb[0], std::min(rgb[1], rgb[2]));
    const float sat = (max_c > 1e-6f) ? (max_c - min_c) / max_c : 0.0f;
    const float boost = vibrance * (1.0f - sat);
    const float gray = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    for (int i = 0; i < 3; i++) {
      rgb[i] = std::max(rgb[i] + (rgb[i] - gray) * boost, 0.0f);
    }
  }
}

void paint_layer_hsl_perspective_apply(float rgb[3],
                                       const float params[PAINT_LAYER_HSL_PERSPECTIVE_PARAM_NUM])
{
  const float hue = params[PAINT_LAYER_HSL_PERSPECTIVE_HUE];
  /* The bipolar sliders read as factors: zero is no change, the full sweep doubles or drains. */
  const float saturation = 1.0f + params[PAINT_LAYER_HSL_PERSPECTIVE_SATURATION] * 2.0f;
  const float lightness = params[PAINT_LAYER_HSL_PERSPECTIVE_LIGHTNESS] * 2.0f;
  if (int(params[PAINT_LAYER_HSL_PERSPECTIVE_MODE] + 0.5f) ==
      PAINT_LAYER_HSL_PERSPECTIVE_PERCEPTIVE)
  {
    /* The Perceptive shift: the hue and the saturation move the same way the Regular's do, but the
     * lightness lifts the colour toward white or scales it toward black -- what the colour looks
     * like moves, where the Regular's value multiply moves what the max channel is stored as. The
     * lift eases by the headroom the channel has, so a lit channel lifts least. */
    paint_layer_hsv_stage_apply(rgb, hue, saturation, 1.0f);
    for (int i = 0; i < 3; i++) {
      rgb[i] += std::max(lightness, 0.0f) * (1.0f - rgb[i]) +
                std::min(lightness, 0.0f) * rgb[i];
    }
    return;
  }
  paint_layer_hsv_stage_apply(rgb, hue, saturation, 1.0f + lightness);
}

float paint_layer_texel_hash_uniform(const int x,
                                     const int y,
                                     const int seed,
                                     const uint32_t salt)
{
  /* A BLI 2D hash over the texel: the salt sits in the first argument's multiples, and the seed's
   * own stride never meets it below the largest salt, so two seeds never share a pattern. The
   * result is a half-open uniform in [0, 1). The one generator every texel-noise family reads, so
   * their sequences cannot drift apart. */
  const uint32_t noise = BLI_hash_int_2d(uint32_t(x) + salt * 7919u + uint32_t(seed) * 6151u,
                                         uint32_t(y));
  return float(noise) / 4294967296.0f;
}

float paint_layer_noise_uniform(int x, int y, int seed, int channel, int pass)
{
  /* The same family the Deband pair reads, with three passes to a channel instead of two. */
  return paint_layer_texel_hash_uniform(x, y, seed, uint32_t(channel * 3 + pass));
}

float paint_layer_noise_gaussian(int x, int y, int seed, int channel, float intensity)
{
  /* Three uniforms sum to a bell whose spread the scale sets: the one fixed function the kernel
   * and the tests both call, so they cannot disagree. */
  const float u1 = paint_layer_noise_uniform(x, y, seed, channel, 1);
  const float u2 = paint_layer_noise_uniform(x, y, seed, channel, 2);
  const float u3 = paint_layer_noise_uniform(x, y, seed, channel, 3);
  return (u1 + u2 + u3 - 1.5f) * intensity * 2.0f;
}

void paint_layer_channel_mixer_vector_get(
    const float folded[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM], const int index, float r_vec[3])
{
  BLI_assert(index >= 0 && index < PAINT_LAYER_CHANNEL_MIXER_VECTOR_NUM);
  const PaintLayerChannelMixerVector &vector = PAINT_LAYER_CHANNEL_MIXER_VECTORS[index];
  if (vector.position == PAINT_LAYER_CHANNEL_MIXER_CONSTANTS) {
    /* The constants sit in the `.w` of the three rows, a stride of four apart. */
    r_vec[0] = folded[PAINT_LAYER_CHANNEL_MIXER_CONSTANTS];
    r_vec[1] = folded[PAINT_LAYER_CHANNEL_MIXER_CONSTANTS + 4];
    r_vec[2] = folded[PAINT_LAYER_CHANNEL_MIXER_CONSTANTS + 8];
    return;
  }
  r_vec[0] = folded[vector.position];
  r_vec[1] = folded[vector.position + 1];
  r_vec[2] = folded[vector.position + 2];
}

void paint_layer_channel_mixer_apply(const float params[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM],
                                     float rgb[3])
{
  const float in[3] = {rgb[0], rgb[1], rgb[2]};
  for (const int k : IndexRange(3)) {
    float row[3];
    paint_layer_channel_mixer_vector_get(params, k, row);
    const float constant = params[PAINT_LAYER_CHANNEL_MIXER_CONSTANTS + 4 * k];
    /* Clamped into [0, 1] like the addon's result; the fold does not clamp, so every consumer does
     * it itself. */
    rgb[k] = std::clamp(
        row[0] * in[0] + row[1] * in[1] + row[2] * in[2] + constant, 0.0f, 1.0f);
  }
}

bool paint_layer_filter_apply(const int code, const float *params, float rgb[3])
{
  switch (code) {
    case PAINT_LAYER_FILTER_CODE_INVERT:
      paint_layer_invert_apply(rgb, params);
      return true;
    case PAINT_LAYER_FILTER_CODE_HSL:
      paint_layer_hsl_apply(rgb, params);
      return true;
    case PAINT_LAYER_FILTER_CODE_LEVELS:
      for (const int k : IndexRange(3)) {
        rgb[k] = paint_layer_levels_apply(rgb[k], params);
      }
      return true;
    case PAINT_LAYER_FILTER_CODE_BRIGHTNESS_CONTRAST:
      paint_layer_brightness_contrast_apply(rgb, params);
      return true;
    case PAINT_LAYER_FILTER_CODE_POSTERIZE:
      for (const int k : IndexRange(3)) {
        rgb[k] = paint_layer_posterize_apply(rgb[k], params[PAINT_LAYER_POSTERIZE_STEPS]);
      }
      return true;
    case PAINT_LAYER_FILTER_CODE_EXPOSURE:
      paint_layer_exposure_apply(rgb, params);
      return true;
    case PAINT_LAYER_FILTER_CODE_CHANNEL_MIXER:
      /* The folded matrix (#paint_layer_channel_mixer_fold, monochrome included), clamped into
       * [0, 1] like the addon's result. No payload pointer: the 12 numbers are the whole input. */
      paint_layer_channel_mixer_apply(params, rgb);
      return true;
    case PAINT_LAYER_FILTER_CODE_MATH:
      for (const int k : IndexRange(3)) {
        rgb[k] = paint_layer_math_apply(rgb[k],
                                        int(params[PAINT_LAYER_MATH_OPERATION] + 0.5f),
                                        params[PAINT_LAYER_MATH_OPERAND_R + k],
                                        params[PAINT_LAYER_MATH_CLAMP] > 0.5f);
      }
      return true;
    case PAINT_LAYER_FILTER_CODE_THRESHOLD:
      for (const int k : IndexRange(3)) {
        rgb[k] = paint_layer_threshold_apply(rgb[k], params[PAINT_LAYER_THRESHOLD_THRESHOLD]);
      }
      return true;
    case PAINT_LAYER_FILTER_CODE_CLAMP:
      for (const int k : IndexRange(3)) {
        rgb[k] = paint_layer_clamp_apply(
            rgb[k], params[PAINT_LAYER_CLAMP_MIN], params[PAINT_LAYER_CLAMP_MAX]);
      }
      return true;
    default:
      return false;
  }
}

float paint_layer_luminance_get(const float rgb[3])
{
  return IMB_colormanagement_get_luminance(rgb);
}

void paint_layer_noise_texel_apply(const int type,
                                   const float intensity,
                                   const bool monochrome,
                                   const int seed,
                                   const int x,
                                   const int y,
                                   float rgb[3])
{
  const auto offset = [&](const int channel) {
    return (type == PAINT_LAYER_NOISE_UNIFORM) ?
               (paint_layer_noise_uniform(x, y, seed, channel, 1) * 2.0f - 1.0f) * intensity :
               paint_layer_noise_gaussian(x, y, seed, channel, intensity);
  };

  if (type == PAINT_LAYER_NOISE_SALT_PEPPER) {
    /* A salted channel reads black, a peppered one white, and the rest stay as they are: the
     * amount is the intensity, half to each side. */
    const float reach = intensity * 0.5f;
    for (const int k : IndexRange(3)) {
      const float u = paint_layer_noise_uniform(x, y, seed, k, 1);
      rgb[k] = (u < reach) ? 0.0f : ((u > 1.0f - reach) ? 1.0f : rgb[k]);
    }
    return;
  }
  if (!monochrome) {
    for (const int k : IndexRange(3)) {
      const float n = offset(k);
      rgb[k] = (type == PAINT_LAYER_NOISE_SPECKLE) ? clamp_f(rgb[k] * (1.0f + n), 0.0f, 1.0f) :
                                                     clamp_f(rgb[k] + n, 0.0f, 1.0f);
    }
    return;
  }
  /* One noise per element, laid through the luminance: the colour follows it, so the grain is the
   * same grey the eye reads. */
  const float lum = paint_layer_luminance_get(rgb);
  const float n = offset(0);
  const float moved = (type == PAINT_LAYER_NOISE_SPECKLE) ? clamp_f(lum * (1.0f + n), 0.0f, 1.0f) :
                                                            clamp_f(lum + n, 0.0f, 1.0f);
  const float delta = moved - lum;
  rgb[0] += delta;
  rgb[1] += delta;
  rgb[2] += delta;
}

}  // namespace blender
