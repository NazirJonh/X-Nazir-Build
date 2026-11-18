/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The domain-neutral pointwise formulas and parameter layouts the paint-layer stacks share: the
 * Levels, Invert, Exposure, Brightness/Contrast, Posterize, Math, Threshold, Clamp, HSL, Noise and
 * Channel Mixer filters, plus the folded layouts the CPU composites, the generated graphs and the
 * GPU push constants read.
 *
 * The material stack (`paint_layers_kind.hh` and its composite) calls these functions, and a second
 * paint stack may reuse them, so two CPU statements of one filter cannot drift apart. Nothing here
 * names a stack's row type or a material DNA enum: the parameter positions are plain ints, and the
 * one formula that reads a stack's payload (`paint_layer_channel_mixer_fold`) is a template over
 * the mixer's shape.
 *
 * The GPU side mirrors these functions in `image_engine_combined.bsl.hh`; the parity tests hold
 * the two to the same answers.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "BLI_index_range.hh"
#include "BLI_math_base_safe.h"
#include "BLI_math_color.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Parameter layouts
 *
 * The positions of every shared filter's parameters in its schema. A stack's registry pins its row
 * schemas to these positions, so the formulas below read the same slot whichever stack folded the
 * array.
 * \{ */

/** The positions of the Levels filter's parameters in its schema. */
enum PaintLayerLevelsParam : int {
  PAINT_LAYER_LEVELS_IN_BLACK = 0,
  PAINT_LAYER_LEVELS_IN_WHITE = 1,
  PAINT_LAYER_LEVELS_GAMMA = 2,
  PAINT_LAYER_LEVELS_OUT_BLACK = 3,
  PAINT_LAYER_LEVELS_OUT_WHITE = 4,
  PAINT_LAYER_LEVELS_PARAM_NUM = 5,
};

/** The positions of the HSL filter's parameters in its schema. */
enum PaintLayerHslParam : int {
  PAINT_LAYER_HSL_HUE = 0,
  PAINT_LAYER_HSL_SATURATION = 1,
  PAINT_LAYER_HSL_VALUE = 2,
  /** The lift the muted colours get, in the addon's -100..100 scaled to -1..1: zero is no-op. */
  PAINT_LAYER_HSL_VIBRANCE = 3,
  PAINT_LAYER_HSL_PARAM_NUM = 4,
};

/** What the HSL Perspective adjusts by, the value of its #PAINT_LAYER_HSL_PERSPECTIVE_MODE
 * parameter. */
enum PaintLayerHslPerspectiveMode : int {
  /** The internal value channel shifts: the max channel of the colour moves, what the colour is
   * stored as. */
  PAINT_LAYER_HSL_PERSPECTIVE_REGULAR = 0,
  /** The colour lifts toward white or black, so what it looks like moves: the perceptual shift
   * the headroom of the colour eases. */
  PAINT_LAYER_HSL_PERSPECTIVE_PERCEPTIVE = 1,
};

/** The positions of the HSL Perspective filter's parameters in its schema. */
enum PaintLayerHslPerspectiveParam : int {
  PAINT_LAYER_HSL_PERSPECTIVE_MODE = 0,
  /** Bipolar around a half: below it the hue shifts one way, above it the other. */
  PAINT_LAYER_HSL_PERSPECTIVE_HUE = 1,
  /** Bipolar around a half: below it the saturation falls, above it the saturation rises. */
  PAINT_LAYER_HSL_PERSPECTIVE_SATURATION = 2,
  /** Bipolar around a half: below it the lightness falls, above it the lightness rises. */
  PAINT_LAYER_HSL_PERSPECTIVE_LIGHTNESS = 3,
  PAINT_LAYER_HSL_PERSPECTIVE_PARAM_NUM = 4,
};

/** The positions of the Brightness/Contrast filter's parameters in its schema. */
enum PaintLayerBrightnessContrastParam : int {
  PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS = 0,
  PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST = 1,
  PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM = 2,
};

enum PaintLayerInvertParam : int {
  /** How far the red channel turns to its complement: one is the whole way, zero none of it. */
  PAINT_LAYER_INVERT_AMOUNT_R = 0,
  /** The same for green. */
  PAINT_LAYER_INVERT_AMOUNT_G = 1,
  /** The same for blue. */
  PAINT_LAYER_INVERT_AMOUNT_B = 2,
  PAINT_LAYER_INVERT_PARAM_NUM = 3,
};

enum PaintLayerExposureParam : int {
  /** The power of two the colour is scaled by: one stop per unit. */
  PAINT_LAYER_EXPOSURE_EXPOSURE = 0,
  /** What the scaled colour rises or falls by, in scene linear. */
  PAINT_LAYER_EXPOSURE_OFFSET = 1,
  /** The power every positive value is raised to, read as one over it. */
  PAINT_LAYER_EXPOSURE_GAMMA = 2,
  PAINT_LAYER_EXPOSURE_PARAM_NUM = 3,
};

/** The positions of the Posterize filter's parameters in its schema. */
enum PaintLayerPosterizeParam : int {
  PAINT_LAYER_POSTERIZE_STEPS = 0,
  PAINT_LAYER_POSTERIZE_PARAM_NUM = 1,
};

/** The operation of the Math filter, the value of its #PAINT_LAYER_MATH_OPERATION parameter. */
enum PaintLayerMathOperation : int {
  PAINT_LAYER_MATH_OP_ADD = 0,
  PAINT_LAYER_MATH_OP_SUBTRACT = 1,
  PAINT_LAYER_MATH_OP_MULTIPLY = 2,
  PAINT_LAYER_MATH_OP_DIVIDE = 3,
  PAINT_LAYER_MATH_OP_MINIMUM = 4,
  PAINT_LAYER_MATH_OP_MAXIMUM = 5,
  PAINT_LAYER_MATH_OP_POWER = 6,
  PAINT_LAYER_MATH_OP_LOGARITHM = 7,
};

/** The positions of the Math filter's parameters in its schema. */
enum PaintLayerMathParam : int {
  PAINT_LAYER_MATH_OPERATION = 0,
  /** The node's own Use Clamp: the result folds into [0, 1]. */
  PAINT_LAYER_MATH_CLAMP = 1,
  /** One operand per colour channel: the value the channel is combined with. */
  PAINT_LAYER_MATH_OPERAND_R = 2,
  PAINT_LAYER_MATH_OPERAND_G = 3,
  PAINT_LAYER_MATH_OPERAND_B = 4,
  PAINT_LAYER_MATH_PARAM_NUM = 5,
};

/** The positions of the Threshold filter's parameters in its schema. */
enum PaintLayerThresholdParam : int {
  PAINT_LAYER_THRESHOLD_THRESHOLD = 0,
  PAINT_LAYER_THRESHOLD_PARAM_NUM = 1,
};

/** The positions of the Clamp filter's parameters in its schema. */
enum PaintLayerClampParam : int {
  PAINT_LAYER_CLAMP_MIN = 0,
  PAINT_LAYER_CLAMP_MAX = 1,
  PAINT_LAYER_CLAMP_PARAM_NUM = 2,
};

/** The positions of the Histogram Scan filter's parameters in its schema. */
enum PaintLayerHistogramScanParam : int {
  /** The crossover point of the remap, the brightness the ramp is centered on. */
  PAINT_LAYER_HISTOGRAM_SCAN_POSITION = 0,
  /** The half-width of the soft transition around the position; zero reads as a hard edge. */
  PAINT_LAYER_HISTOGRAM_SCAN_CONTRAST = 1,
  /** On: the two states flip, `1 - out`, the way an inverted mask item does. */
  PAINT_LAYER_HISTOGRAM_SCAN_INVERT = 2,
  PAINT_LAYER_HISTOGRAM_SCAN_PARAM_NUM = 3,
};

/** The positions of the Histogram Shift filter's parameters in its schema. */
enum PaintLayerHistogramShiftParam : int {
  /** The fraction of the tonal range the value rolls by; one is the value again. */
  PAINT_LAYER_HISTOGRAM_SHIFT_POSITION = 0,
  PAINT_LAYER_HISTOGRAM_SHIFT_PARAM_NUM = 1,
};

/** The positions of the Channel Mixer filter's parameters in its schema. */
enum PaintLayerChannelMixerParam : int {
  /** The payload: the stack's own mixer matrix, behind the row's own pointer. */
  PAINT_LAYER_CHANNEL_MIXER_MIXER = 0,
  /** On: a grey from the red row and the constants' mean. */
  PAINT_LAYER_CHANNEL_MIXER_MONOCHROME = 1,
  PAINT_LAYER_CHANNEL_MIXER_PARAM_NUM = 2,
};

/**
 * The layout of the Channel Mixer's folded 12 floats, shared by the CPU composites, the generated
 * graphs and the GPU push constants: the three rows in the `.xyz` of the three float4s (positions
 * 0, 4, 8) and the three constants in their `.w` (3, 7, 11), the output channel k reading row k.
 */
enum PaintLayerChannelMixerFolded : int {
  PAINT_LAYER_CHANNEL_MIXER_ROW_R = 0,
  PAINT_LAYER_CHANNEL_MIXER_ROW_G = 4,
  PAINT_LAYER_CHANNEL_MIXER_ROW_B = 8,
  /** The position of the first constant: the `.w` of the red row's float4. */
  PAINT_LAYER_CHANNEL_MIXER_CONSTANTS = 3,
  /** The length the folded array needs; the constant of row k sits at `3 + 4 * k`. */
  PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM = 12,
};

/** One vector a Channel Mixer input carries: a row, or the three constants. */
struct PaintLayerChannelMixerVector {
  const char *suffix;
  int position;
};
inline constexpr int PAINT_LAYER_CHANNEL_MIXER_VECTOR_NUM = 4;
inline constexpr PaintLayerChannelMixerVector
    PAINT_LAYER_CHANNEL_MIXER_VECTORS[PAINT_LAYER_CHANNEL_MIXER_VECTOR_NUM] = {
        {"Mixer R", PAINT_LAYER_CHANNEL_MIXER_ROW_R},
        {"Mixer G", PAINT_LAYER_CHANNEL_MIXER_ROW_G},
        {"Mixer B", PAINT_LAYER_CHANNEL_MIXER_ROW_B},
        {"Mixer Constants", PAINT_LAYER_CHANNEL_MIXER_CONSTANTS},
};

/** The Noise filter's families. */
enum PaintLayerNoiseType : int {
  /** A sum of three uniforms, scaled so the spread reads as the intensity. */
  PAINT_LAYER_NOISE_GAUSSIAN = 0,
  /** One uniform, signed and scaled. */
  PAINT_LAYER_NOISE_UNIFORM = 1,
  /** The gaussian noise as a factor of the colour itself. */
  PAINT_LAYER_NOISE_SPECKLE = 2,
  /** The channels the uniform puts under the intensity's half read as black, the ones over the
   * other end as white, and the rest stay as they are. */
  PAINT_LAYER_NOISE_SALT_PEPPER = 3,
};

/** The positions of the Noise filter's parameters in its schema. */
enum PaintLayerNoiseParam : int {
  PAINT_LAYER_NOISE_TYPE = 0,
  /** The spread of the noise, in colour: the sigma of the gaussian family, the reach of the
   * rest. */
  PAINT_LAYER_NOISE_INTENSITY = 1,
  /** On: the noise moves the scene-linear luminance and the colour follows it. */
  PAINT_LAYER_NOISE_MONOCHROME = 2,
  PAINT_LAYER_NOISE_SEED = 3,
  PAINT_LAYER_NOISE_PARAM_NUM = 4,
};

/** The shape of the Blur filter's kernel, the value of its #PAINT_LAYER_BLUR_SHAPE parameter. */
enum PaintLayerBlurShape : int {
  PAINT_LAYER_BLUR_BOX = 0,
  PAINT_LAYER_BLUR_GAUSSIAN = 1,
};

/** The positions of the Blur filter's parameters in its schema. */
enum PaintLayerBlurParam : int {
  PAINT_LAYER_BLUR_SHAPE = 0,
  PAINT_LAYER_BLUR_RADIUS = 1,
  PAINT_LAYER_BLUR_PARAM_NUM = 2,
};

/** The positions of the Blur Directional filter's parameters in its schema. */
enum PaintLayerBlurDirectionalParam : int {
  /** The angle the line of samples runs at, in degrees, counter-clockwise off the +X of the map. */
  PAINT_LAYER_BLUR_DIRECTIONAL_ANGLE = 0,
  /** Whole texels: the line's half reach. */
  PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS = 1,
  /** How much of the average the result takes: zero is the colour as it is, one is all average. */
  PAINT_LAYER_BLUR_DIRECTIONAL_MIX = 2,
  PAINT_LAYER_BLUR_DIRECTIONAL_PARAM_NUM = 3,
};

/** The field the Normal to Mask kernel rebuilds from the normal, the value of its
 * #PAINT_LAYER_NORMAL_TO_MASK_MODE parameter. */
enum PaintLayerNormalToMaskMode : int {
  /** The height the tangent normal's gradients integrate back into (a Poisson solve over the
   * divergence of the normal's XY): the raised elements stand out as bright or dark regions. */
  PAINT_LAYER_NORMAL_TO_MASK_HEIGHT = 0,
  /** The magnitude of the normal's gradient (a Sobel over each channel summed): the borders of
   * the elements light up, their insides stay dark. */
  PAINT_LAYER_NORMAL_TO_MASK_EDGES = 1,
  /** The slope the normal's blue channel carries, `1 - z`: flat areas read dark, tilted ones
   * bright. */
  PAINT_LAYER_NORMAL_TO_MASK_SLOPE = 2,
};

/** The positions of the Normal to Mask filter's parameters in its schema. */
enum PaintLayerNormalToMaskParam : int {
  /** The field the normal rebuilds into: #PaintLayerNormalToMaskMode. */
  PAINT_LAYER_NORMAL_TO_MASK_MODE = 0,
  /** The position the smoothstep centers on, in the field's normalized [0, 1]. */
  PAINT_LAYER_NORMAL_TO_MASK_THRESHOLD = 1,
  /** The ramp's width around the position: zero is a hard cut, one the full range. */
  PAINT_LAYER_NORMAL_TO_MASK_SOFTNESS = 2,
  PAINT_LAYER_NORMAL_TO_MASK_PARAM_NUM = 3,
};

/** The positions of the Sharpen filter's parameters in its schema. */
enum PaintLayerSharpenParam : int {
  PAINT_LAYER_SHARPEN_RADIUS = 0,
  PAINT_LAYER_SHARPEN_AMOUNT = 1,
  PAINT_LAYER_SHARPEN_PARAM_NUM = 2,
};

/** The value of the High Pass filter's #PAINT_LAYER_HIGH_PASS_OUTPUT. */
enum PaintLayerHighPassOutput : int {
  /** The detail of each colour channel. */
  PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR = 0,
  /** The detail of the luminance, the same grey in all three channels. */
  PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE = 1,
};

/** The positions of the High Pass filter's parameters in its schema. */
enum PaintLayerHighPassParam : int {
  PAINT_LAYER_HIGH_PASS_RADIUS = 0,
  PAINT_LAYER_HIGH_PASS_CONTRAST = 1,
  PAINT_LAYER_HIGH_PASS_OUTPUT = 2,
  PAINT_LAYER_HIGH_PASS_PARAM_NUM = 3,
};

/** The direction of the Erode/Dilate kernel, the value of its #PAINT_LAYER_ERODE_DILATE_MODE. */
enum PaintLayerErodeDilateMode : int {
  /** The minimum of the neighbourhood: the bright regions shrink. */
  PAINT_LAYER_ERODE_DILATE_ERODE = 0,
  /** The maximum of the neighbourhood: the bright regions grow. */
  PAINT_LAYER_ERODE_DILATE_DILATE = 1,
};

/** The positions of the Erode/Dilate filter's parameters in its schema. */
enum PaintLayerErodeDilateParam : int {
  PAINT_LAYER_ERODE_DILATE_MODE = 0,
  PAINT_LAYER_ERODE_DILATE_RADIUS = 1,
  PAINT_LAYER_ERODE_DILATE_PARAM_NUM = 2,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pointwise formulas
 * \{ */

/**
 * The Levels stage every consumer runs at one texel: \a t clamped into [0, 1], gamma applied to
 * the positive part, then the linear map from \a out_black to \a out_white. The one formula every
 * CPU consumer runs. The stack's Levels filter scales from its parameter
 * array per texel; the brush's "Correction Image" calls it with the constants its prepare step
 * resolved, so the two cannot drift apart on the math they paint.
 */
inline float paint_layer_levels_stage_apply(const float t,
                                            const float inv_gamma,
                                            const float out_black,
                                            const float out_white)
{
  float result = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
  /* The Gamma node leaves a non-positive value as it is. */
  if (result > 0.0f) {
    result = powf(result, inv_gamma);
  }
  return result * (out_white - out_black) + out_black;
}

/** The Levels of a filter value: the same function every CPU consumer applies. */
inline float paint_layer_levels_apply(const float value,
                                      const float params[PAINT_LAYER_LEVELS_PARAM_NUM])
{
  /* A zero span or gamma reads as zero, which is what the Math Divide node gives the graph. */
  const float span = params[PAINT_LAYER_LEVELS_IN_WHITE] - params[PAINT_LAYER_LEVELS_IN_BLACK];
  const float k = (span != 0.0f) ? 1.0f / span : 0.0f;
  const float m = -(params[PAINT_LAYER_LEVELS_IN_BLACK] * k);
  const float inv_gamma = (params[PAINT_LAYER_LEVELS_GAMMA] != 0.0f) ?
                              1.0f / params[PAINT_LAYER_LEVELS_GAMMA] :
                              0.0f;
  return paint_layer_levels_stage_apply(value * k + m,
                                        inv_gamma,
                                        params[PAINT_LAYER_LEVELS_OUT_BLACK],
                                        params[PAINT_LAYER_LEVELS_OUT_WHITE]);
}

/**
 * The Invert stage's per-channel target: `|c - 1|`, the complement on [0, 1], the same distance
 * past it - what the Difference mix against white and the CPU composite both read. Shared by the
 * stack's Invert filter (which moves each channel part of the way, by its own amount) and the
 * brush correction's full invert.
 */
inline float paint_layer_invert_target(const float c)
{
  return std::fabs(c - 1.0f);
}

/**
 * The Invert filter applied to \a rgb in place, each channel by its own strength: the channel moves
 * from its colour to #paint_layer_invert_target of itself. The default of every amount is one, so a
 * row without stored parameters inverts the whole way as the unparameterised filter always did.
 * Alpha is not part of \a rgb and so stays untouched.
 */
inline void paint_layer_invert_apply(float rgb[3],
                                     const float params[PAINT_LAYER_INVERT_PARAM_NUM])
{
  for (int k = 0; k < 3; k++) {
    rgb[k] += (paint_layer_invert_target(rgb[k]) - rgb[k]) * params[k];
  }
}

/**
 * The Exposure filter applied to \a rgb in place: the colour scaled by `2^exposure`, shifted by the
 * offset, then raised to one over the gamma. The power is the Gamma node's own function -- a
 * non-positive value is left as it is -- so a default gamma leaves a colour above one above one:
 * the filter brightens an HDR map instead of clipping what it already holds. Alpha is not part of
 * \a rgb and so stays untouched.
 */
inline void paint_layer_exposure_apply(
    float rgb[3], const float params[PAINT_LAYER_EXPOSURE_PARAM_NUM])
{
  const float scale = powf(2.0f, params[PAINT_LAYER_EXPOSURE_EXPOSURE]);
  const float exponent = 1.0f / params[PAINT_LAYER_EXPOSURE_GAMMA];
  for (int i = 0; i < 3; i++) {
    const float scaled = rgb[i] * scale + params[PAINT_LAYER_EXPOSURE_OFFSET];
    rgb[i] = (scaled > 0.0f) ? powf(scaled, exponent) : scaled;
  }
}

/**
 * The Brightness/Contrast filter applied to \a rgb in place, at full strength: the
 * Brightness/Contrast shader node's function, `out = max(a * in + b, 0)` with `a = 1 + contrast`
 * and `b = brightness - contrast * 0.5`, so the CPU composite and the generated node agree. Alpha
 * is not part of \a rgb and so stays untouched.
 */
inline void paint_layer_brightness_contrast_apply(
    float rgb[3], const float params[PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM])
{
  const float contrast = params[PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST];
  const float a = 1.0f + contrast;
  const float b = params[PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS] - contrast * 0.5f;
  for (int i = 0; i < 3; i++) {
    rgb[i] = std::max(a * rgb[i] + b, 0.0f);
  }
}

/**
 * The Posterize filter applied to one channel: the value quantized to \a steps evenly spaced levels
 * over [0, 1], `floor(clamp(value, 0, 1) * (steps - 1) + 0.5) / (steps - 1)`, so both endpoints are
 * preserved and a value at a level's midpoint rounds to the nearer one. A step count below two
 * reads as two, the smallest count that shows a step.
 */
inline float paint_layer_posterize_apply(const float value, const float steps)
{
  const float k = std::max(steps - 1.0f, 1.0f);
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return std::floor(clamped * k + 0.5f) / k;
}

/**
 * One channel of the Math filter: \a value combined with the constant \a operand by \a operation
 * (a #PaintLayerMathOperation). POWER and LOGARITHM read the Math node's safe rules through
 * #safe_powf and #safe_logf, so a negative base under a fractional exponent and a non-positive
 * logarithm base or argument stay defined; a division by zero reads zero. With \a clamp the result
 * folds into [0, 1], the node's own Use Clamp, so the CPU composite and the generated node agree.
 */
inline float paint_layer_math_apply(const float value,
                                    const int operation,
                                    const float operand,
                                    const bool clamp)
{
  float result = value;
  switch (operation) {
    case PAINT_LAYER_MATH_OP_ADD:
      result = value + operand;
      break;
    case PAINT_LAYER_MATH_OP_SUBTRACT:
      result = value - operand;
      break;
    case PAINT_LAYER_MATH_OP_MULTIPLY:
      result = value * operand;
      break;
    case PAINT_LAYER_MATH_OP_DIVIDE:
      result = (operand != 0.0f) ? value / operand : 0.0f;
      break;
    case PAINT_LAYER_MATH_OP_MINIMUM:
      result = std::min(value, operand);
      break;
    case PAINT_LAYER_MATH_OP_MAXIMUM:
      result = std::max(value, operand);
      break;
    case PAINT_LAYER_MATH_OP_POWER:
      result = safe_powf(value, operand);
      break;
    case PAINT_LAYER_MATH_OP_LOGARITHM:
      result = safe_logf(value, operand);
      break;
    default:
      break;
  }
  return clamp ? std::clamp(result, 0.0f, 1.0f) : result;
}

/**
 * One channel of the Threshold filter: the two states of the Math node's Greater Than, `1.0` when
 * the value is strictly above \a threshold and `0.0` otherwise, so the CPU composite and the
 * generated node agree on the comparison's edge.
 */
inline float paint_layer_threshold_apply(const float value, const float threshold)
{
  return (value > threshold) ? 1.0f : 0.0f;
}

/**
 * One channel of the Histogram Scan filter: the smoothstep the Math node's own Smooth Step
 * evaluates, centered on \a position and opened by \a contrast into a ramp of the same width each
 * side, so `contrast = 0` reads as the Threshold the position alone makes. An inverted position
 * flips the two states, `1 - out`, the way an inverted mask item does.
 */
inline float paint_layer_histogram_scan_apply(const float value,
                                              const float position,
                                              const float contrast,
                                              const bool invert_position)
{
  /* The transition spans the same width each side of the position, so a full contrast is the
   * ramp the value walks from zero at the position's low edge to one at its high one. */
  const float edge = contrast * 0.5f;
  const float lo = position - edge;
  const float span = std::max(edge * 2.0f, 1e-6f);
  const float t = std::clamp((value - lo) / span, 0.0f, 1.0f);
  const float result = t * t * (3.0f - 2.0f * t);
  return invert_position ? 1.0f - result : result;
}

/**
 * One channel of the Histogram Shift filter: the value plus the position, wrapped back through
 * zero when it passes one, the Math node's own Wrap. The shift is cyclic: a position of one is
 * the value again.
 */
inline float paint_layer_histogram_shift_apply(const float value, const float position)
{
  const float shifted = value + position;
  return shifted - std::floor(shifted);
}

/**
 * The cut of the Normal to Mask kernel: the smoothstep ramp the Histogram Scan's own formula
 * lays, centered on \a threshold and opened by \a softness the same way, so a zero softness is
 * the hard cut the Threshold makes and the ramp's width grows with the softness. The one formula
 * the CPU kernel and the preview session run, so the two cannot drift on where the mask cuts.
 */
inline float paint_layer_normal_to_mask_ramp_apply(const float value,
                                                   const float threshold,
                                                   const float softness)
{
  const float edge = std::clamp(softness, 0.0f, 1.0f) * 0.5f;
  const float lo = threshold - edge;
  const float span = std::max(edge * 2.0f, 1e-6f);
  const float t = std::clamp((value - lo) / span, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/**
 * One channel of the Smoothstep filter: the Hermite interpolation between the range's edges, the
 * Map Range node's own Smoothstep. An empty range reads as the low edge's state: the extraction
 * of nothing leaves nothing.
 */
inline float paint_layer_smoothstep_apply(const float value, const float minimum, const float maximum)
{
  const float span = maximum - minimum;
  const float t = (std::abs(span) < 1e-6f) ? 0.0f : std::clamp((value - minimum) / span, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/**
 * One channel of the Contrast Luminosity filter: the value's distance from the middle scaled by
 * the contrast, then shifted by the luminosity. A negative contrast folds the distance in, a
 * positive one spreads it; the shift rides after, so a full-black value still moves.
 */
inline float paint_layer_contrast_luminosity_apply(const float value,
                                                   const float contrast,
                                                   const float luminosity)
{
  const float scaled = (value - 0.5f) * (1.0f + contrast) + 0.5f;
  return scaled + luminosity;
}

/**
 * One channel of the Height Adjust filter: the invert, then the offset, then the multiply -- the
 * order the filter's own note pins, where a zero offset result annihilates the multiply. The
 * result is left open above one: a height pushed past the top stays there, and the height delta
 * machinery reads the excess as it reads any HDR value.
 */
inline float paint_layer_height_adjust_apply(
    const float value, const bool invert, const float offset, const float multiply)
{
  const float flipped = invert ? 1.0f - value : value;
  return (flipped + offset) * multiply;
}

/**
 * One channel of the Clamp filter: `min(max(value, min), max)`, the Math node's own Clamp and the
 * order that keeps a well-defined result when \a min is above \a max.
 */
inline float paint_layer_clamp_apply(const float value, const float min, const float max)
{
  return std::min(std::max(value, min), max);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Channel Mixer
 * \{ */

/**
 * Fold \a mixer's matrix into the 12-float layout #PaintLayerChannelMixerFolded names, so the CPU
 * composite, the generated graph and the GPU push constants read one shape. A template over the
 * mixer's shape, because each stack owns its own DNA matrix of the same fields (the material's is
 * #MaterialPaintLayerChannelMixer) and this header may not include a stack's DNA.
 */
template<typename MixerT>
inline void paint_layer_channel_mixer_fold(const MixerT &mixer,
                                           const bool monochrome,
                                           float r_params[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM])
{
  const float *rows[3] = {mixer.red_row, mixer.green_row, mixer.blue_row};
  /* The addon's monochrome reads one grey from the red row and the mean of the constants; folding
   * it here keeps the CPU, the graph and the GPU on one layout with no branch of their own. */
  const float mean_constant = (mixer.constants[0] + mixer.constants[1] + mixer.constants[2]) /
                              3.0f;
  for (const int k : IndexRange(3)) {
    const float *row = monochrome ? rows[0] : rows[k];
    const int base = PAINT_LAYER_CHANNEL_MIXER_ROW_R + 4 * k;
    r_params[base + 0] = row[0];
    r_params[base + 1] = row[1];
    r_params[base + 2] = row[2];
    r_params[base + 3] = monochrome ? mean_constant : mixer.constants[k];
  }
}

/**
 * One vector a Channel Mixer input carries, out of the folded array: the row at \a index, or the
 * three constants read with a stride of four.
 */
void paint_layer_channel_mixer_vector_get(
    const float folded[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM], int index, float r_vec[3]);

/**
 * The Channel Mixer filter applied to \a rgb in place from the folded matrix: the output channel k
 * reads row k and its constant, clamped into [0, 1] like the addon's result; the fold does not
 * clamp, so every consumer does it itself.
 */
void paint_layer_channel_mixer_apply(const float params[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM],
                                     float rgb[3]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name HSL and noise
 * \{ */

/**
 * The Hue/Saturation/Value stage itself: hue shifted by \a hue_turns turns, saturation clamped to
 * one, value scaled, result not below zero. The one formula every CPU consumer runs - the stack's
 * HSL filter passes its own parameters (and then applies its vibrance); the brush's "Correction
 * Image" passes the constants its prepare step resolved, so the two cannot drift apart.
 */
void paint_layer_hsv_stage_apply(float rgb[3], float hue_turns, float saturation, float value);

/**
 * The HSL filter applied to \a rgb in place, at full strength: the Hue/Saturation/Value node's
 * function (hue shifted by `hue - 0.5` turns, saturation clamped to one, value scaled, result not
 * below zero), so the CPU composite and the generated node agree.
 */
void paint_layer_hsl_apply(float rgb[3], const float params[PAINT_LAYER_HSL_PARAM_NUM]);

/**
 * The HSL Perspective applied to one texel \a rgb in place: the hue and saturation shift the way
 * the Hue/Saturation node's own math does, the lightness moves the value channel in the Regular
 * mode and lifts the colour toward white or black in the Perceptive one.
 */
void paint_layer_hsl_perspective_apply(float rgb[3],
                                       const float params[PAINT_LAYER_HSL_PERSPECTIVE_PARAM_NUM]);

/**
 * One uniform in [0, 1) of a texel hash family: \a salt separates the passes and the channels, the
 * seed moves the pattern, and the texel's coordinates are where the noise lives. The one generator
 * every texel-noise family reads, so their sequences cannot drift apart.
 */
float paint_layer_texel_hash_uniform(int x, int y, int seed, uint32_t salt);

/**
 * One uniform in [0, 1) of the Noise filter, from the same hash family the Deband pair reads: the
 * salt keeps the three passes and the channels apart, the seed moves the pattern, and the texel is
 * where the noise lives.
 */
float paint_layer_noise_uniform(int x, int y, int seed, int channel, int pass);

/**
 * The gaussian noise of one texel channel: three uniforms of #paint_layer_noise_uniform summed,
 * centred and scaled, so the spread of the answer reads as \a intensity. The one function the
 * effect kernel and the tests both call, so they cannot disagree.
 */
float paint_layer_noise_gaussian(int x, int y, int seed, int channel, float intensity);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Filter dispatch and kernel formulas
 *
 * The pointwise filter dispatch the CPU composites read, and the formula bodies the neighborhood
 * kernels share. The traversals (the material's ImBuf rows) stay in each stack; only the arithmetic
 * of one element lives here.
 * \{ */

/**
 * The filter codes #paint_layer_filter_apply knows. A stack's own filter enum carries the same
 * value for each of these, which that stack's translation unit pins with a `static_assert`, so it
 * passes its code straight through and an unmapped code is simply not handled.
 */
enum PaintLayerFilterCode : int {
  PAINT_LAYER_FILTER_CODE_INVERT = 1,
  PAINT_LAYER_FILTER_CODE_LEVELS = 2,
  PAINT_LAYER_FILTER_CODE_HSL = 3,
  PAINT_LAYER_FILTER_CODE_BRIGHTNESS_CONTRAST = 6,
  PAINT_LAYER_FILTER_CODE_POSTERIZE = 8,
  PAINT_LAYER_FILTER_CODE_MATH = 9,
  PAINT_LAYER_FILTER_CODE_THRESHOLD = 10,
  PAINT_LAYER_FILTER_CODE_CLAMP = 11,
  PAINT_LAYER_FILTER_CODE_EXPOSURE = 23,
  PAINT_LAYER_FILTER_CODE_CHANNEL_MIXER = 27,
};

/**
 * The result of the pointwise filter \a code on \a rgb at full strength, written back into it,
 * for the filters that need no stack-specific input (Invert, Levels, HSL, Brightness/Contrast,
 * Posterize, Math, Threshold, Clamp, Exposure, Channel Mixer), reading \a params in the schemas'
 * own positions.
 *
 * \return false when \a code is not one of them and \a rgb was left alone, so a stack can fall
 * through to the filters only it has.
 */
bool paint_layer_filter_apply(int code, const float *params, float rgb[3]);

/** The scene-linear luminance of \a rgb, the colour management's own weights. */
float paint_layer_luminance_get(const float rgb[3]);

/** The radius of each of the three box passes a gaussian blur of \a radius is made of. */
inline int paint_layer_gaussian_pass_radius(const int radius)
{
  return std::max(radius / 2, 1);
}

/**
 * The Sharpen kernel's channel: the detail the blur removes (\a orig - \a blurred) comes back on
 * top of the colour, scaled by \a amount.
 */
inline float paint_layer_sharpen_apply(const float orig, const float blurred, const float amount)
{
  return orig + (orig - blurred) * amount;
}

/**
 * The High Pass kernel's channel: the detail against the blurred copy over the mid grey. The
 * difference is taken in the display encoding, as image editors do: in scene linear the dark
 * tones' detail is squeezed to nothing and the result reads as a flat grey. The grey is the
 * encoded 0.5, decoded back to the linear value the rest of the stack expects.
 */
inline float paint_layer_high_pass_apply(const float orig,
                                         const float blurred,
                                         const float contrast)
{
  const float detail = linearrgb_to_srgb(orig) - linearrgb_to_srgb(blurred);
  return srgb_to_linearrgb(0.5f + detail * contrast);
}

/**
 * The frame of every unsharp-style kernel's channel: \a op against the blurred copy, clipped
 * below -- a negative linear colour would break the blends and the encoding after it -- while
 * above the map may be HDR.
 */
template<typename Op>
inline float paint_layer_unsharp_channel_apply(Op op, const float orig, const float blurred)
{
  return std::max(op(orig, blurred), 0.0f);
}

/**
 * The mask stack's filter rule: a filter works on the factor so far as a grey,
 * `M = lerp(M, f(M), opacity)`. \a filter_grey receives the three-float grey and rewrites it with
 * the filter's result (the strength multiplier of a filter that has one is ignored: a grey
 * coefficient has no stop alpha to apply). Not part of the rule, and so left to each sampler: the
 * final clamp into 0..1 and everything that reads an item's own coverage.
 */
template<typename FilterGreyFn>
inline float paint_layer_mask_filter_apply(const float factor,
                                           const float opacity,
                                           FilterGreyFn filter_grey)
{
  float grey[3] = {factor, factor, factor};
  filter_grey(grey);
  return factor + (grey[0] - factor) * opacity;
}

/**
 * The Noise kernel's grain laid on \a rgb in place at the element (\a x, \a y): the \a type's
 * family at \a intensity from \a seed, per channel or (with \a monochrome) through the
 * scene-linear luminance. A function of the element and the seed alone, so a bake and a preview
 * compute the same grain and elements can run in parallel. The salt-and-pepper family ignores
 * \a monochrome. Alpha is not an input.
 */
void paint_layer_noise_texel_apply(
    int type, float intensity, bool monochrome, int seed, int x, int y, float rgb[3]);

/** \} */

}  // namespace blender
