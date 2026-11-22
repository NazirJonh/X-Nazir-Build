/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 *
 * The Combined preview of a layered material, evaluated on the GPU straight into the Image Editor's
 * display texture. Two compute passes:
 *
 * - `layer_comp` lays one row of a channel stack over the channel's accumulator, mirroring
 *   `composite_layer_render` and `blend_row_linear` of `paint_material_composite.cc` for the rows it
 *   handles (a map or a flat colour, opacity, UV mapping, Mix or Normal Combine);
 * - `shade_comp` shades the eight accumulators with the studio light rig, mirroring
 *   `BKE_paint_material_combined_eval`.
 *
 * The flags of `layer_comp` are the `gpu_layer_*` values of `paint_material_combined_gather.cc`.
 */

#pragma once

#include "gpu_shader_common_color_utils.glsl"
#include "gpu_shader_compat.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_constants_lib.glsl"

#define COMBINED_LAYER_INIT 1
#define COMBINED_LAYER_HAS_MAP 2
#define COMBINED_LAYER_ALPHA_COVERAGE 4
#define COMBINED_LAYER_PREMULTIPLIED 8
#define COMBINED_LAYER_MAPPED 32
#define COMBINED_LAYER_REPEAT_U 64
#define COMBINED_LAYER_REPEAT_V 128
#define COMBINED_LAYER_TANGENT_NORMAL 256
#define COMBINED_LAYER_NORMAL_DIRECTX 262144
/* The row's Mirror fold: the map is read through `|uv - center| + center` on each set axis, and a
 * tangent-space Normal's component along that axis is flipped per texel by the side of the center
 * it reads from -- the per-texel counterpart of the negative-scale flip. */
#define COMBINED_LAYER_MIRROR_U 1048576
#define COMBINED_LAYER_MIRROR_V 2097152
/* The fold's direction on a set axis: with the bit the map is read through
 * `center - |uv - center|` (the smaller side shows on both) and the Normal's side sign flips. */
#define COMBINED_LAYER_MIRROR_U_NEG 134217728
#define COMBINED_LAYER_MIRROR_V_NEG 268435456
/* What the row's map stores (`#eMaterialPaintLayerMapEncoding`), read into the channel's value
 * before the coverage: a glossiness map is `1 - rgb`, the spec-gloss packing reads `1 - alpha`
 * with the map's alpha as data (no `COMBINED_LAYER_ALPHA_COVERAGE` alongside), an F0 colour reads
 * as the specular level `clamp(dot(rgb, map_luma.rgb) * map_luma.a, 0, 1)`. */
#define COMBINED_LAYER_MAP_GLOSSINESS 4194304
#define COMBINED_LAYER_MAP_GLOSSINESS_ALPHA 8388608
#define COMBINED_LAYER_MAP_F0 16777216
/* The spec-gloss packing on the Specular record: the F0 level from the colour, the alpha glossiness
 * data and so on, as the CPU composite reads it. */
#define COMBINED_LAYER_MAP_F0_GLOSSINESS_ALPHA 33554432
/* The F0 a level of one is (`0.08`, #SPECULAR_LEVEL_F0_FULL on the CPU) rides in `map_luma.w` as
 * its reciprocal, so the pass multiplies by one value. */
/* The row's map is read through its projection frame instead of the canvas UV: the position and
 * normal of the painted surface at the texel come from the surface map (`proj_pos_tx`,
 * `proj_nrm_tx`), the formulas are those of `composite_resample_projected`. */
#define COMBINED_LAYER_PROJECTED 67108864
#define COMBINED_LAYER_HAS_MASK 2048
/* The row stage: the layer pass stores the row's colour and coverage instead of blending them, and a
 * later pass blends what the corrections left. */
#define COMBINED_LAYER_ROW_STORE 4096
#define COMBINED_LAYER_ROW_USE 8192
/* An unpainted row kept for its corrections: it covers nothing until they bring coverage in. */
#define COMBINED_LAYER_EMPTY_BASE 16384
/* The fourth component of the row's colour is a content alpha the corrections raise. */
#define COMBINED_LAYER_TRACKS_ALPHA 32768
/* An adjustment: the row's colour is the stack so far with a filter applied. */
#define COMBINED_LAYER_ADJUSTMENT 65536
/* The row reads an Anchor Point slice from a screen-space slot: the colour in `layer_tx`, the
 * coverage in `slot_cov_tx`. */
#define COMBINED_LAYER_SLOT 524288
/* The correction is a CPU-computed effect the GPU computed instead: its result is in `effect_tx`, at
 * the input's own resolution, and is mixed into the row's colour with no coverage of its own. */
#define COMBINED_LAYER_EFFECT_RESULT 1048576

/* The transform effects' steps a pass applies ahead of the row's own mapping: at most
 * #COMBINED_INPUT_STEP_MAX of them, each packed in two float4s (offset.xy, scale.xy |
 * rotation.xy, mirror_center.xy) with seven flag bits a step in `input_step_flags`. A CPU composite
 * composes more; past the two the preview refuses the row to the CPU, so a stack is never shaded
 * as if a step were not there. */
/* Mirrors #PAINT_MATERIAL_COMBINED_INPUT_STEP_MAX (BKE_paint_material_composite.hh) and
 * #gpu_input_step_max (paint_material_combined_effects.hh): change them together. */
#define COMBINED_INPUT_STEP_MAX 2
#define COMBINED_INPUT_STEP_MAPPED 1
#define COMBINED_INPUT_STEP_MIRROR_U 2
#define COMBINED_INPUT_STEP_MIRROR_V 4
#define COMBINED_INPUT_STEP_REPEAT_U 8
#define COMBINED_INPUT_STEP_REPEAT_V 16
#define COMBINED_INPUT_STEP_MIRROR_U_NEG 32
#define COMBINED_INPUT_STEP_MIRROR_V_NEG 64
/* Bits a step takes in `input_step_flags`. */
#define COMBINED_INPUT_STEP_BITS 7
#define COMBINED_INPUT_STEP_MASK 127

/* The `CompositeBlend` values, which are the `MA_RAMP_*` codes of the Mix node. */
#define COMBINED_BLEND_MIX 0
#define COMBINED_BLEND_ADD 1
#define COMBINED_BLEND_MULTIPLY 2
#define COMBINED_BLEND_SUBTRACT 3
#define COMBINED_BLEND_SCREEN 4
#define COMBINED_BLEND_DIVIDE 5
#define COMBINED_BLEND_DIFFERENCE 6
#define COMBINED_BLEND_DARKEN 7
#define COMBINED_BLEND_LIGHTEN 8
#define COMBINED_BLEND_OVERLAY 9
#define COMBINED_BLEND_DODGE 10
#define COMBINED_BLEND_BURN 11
#define COMBINED_BLEND_HUE 12
#define COMBINED_BLEND_SATURATION 13
#define COMBINED_BLEND_VALUE 14
#define COMBINED_BLEND_COLOR 15
#define COMBINED_BLEND_SOFT_LIGHT 16
#define COMBINED_BLEND_LINEAR_LIGHT 17
#define COMBINED_BLEND_EXCLUSION 18
#define COMBINED_BLEND_NORMAL_COMBINE 19

namespace image_engine::combined {

/* -------------------------------------------------------------------- */
/** \name Blend Modes
 *
 * A port of `ramp_blend` (material.cc), the function the CPU composite calls for every Mix mode, so
 * the two cannot disagree. Mix itself is handled by the caller because it also mixes alpha.
 * \{ */

float3 rgb_to_hsv3(float3 rgb)
{
  float4 hsv;
  rgb_to_hsv(float4(rgb, 1.0f), hsv);
  return hsv.xyz;
}

float3 hsv_to_rgb3(float3 hsv)
{
  float4 rgb;
  hsv_to_rgb(float4(hsv, 1.0f), rgb);
  return rgb.xyz;
}

float dodge_channel(float base, float col, float fac)
{
  if (base == 0.0f) {
    return base;
  }
  float tmp = 1.0f - fac * col;
  if (tmp <= 0.0f) {
    return 1.0f;
  }
  return min(base / tmp, 1.0f);
}

float burn_channel(float base, float col, float fac)
{
  float tmp = (1.0f - fac) + fac * col;
  if (tmp <= 0.0f) {
    return 0.0f;
  }
  return clamp(1.0f - (1.0f - base) / tmp, 0.0f, 1.0f);
}

float overlay_channel(float base, float col, float fac)
{
  float facm = 1.0f - fac;
  if (base < 0.5f) {
    return base * (facm + 2.0f * fac * col);
  }
  return 1.0f - (facm + 2.0f * fac * (1.0f - col)) * (1.0f - base);
}

float linear_light_channel(float base, float col, float fac)
{
  return (col > 0.5f) ? base + fac * (2.0f * (col - 0.5f)) : base + fac * (2.0f * col - 1.0f);
}

/** The colour of `ramp_blend(type, base, fac, col)` for every mode but Mix. */
float3 blend_rgb(int type, float3 base, float3 col, float fac)
{
  float facm = 1.0f - fac;
  float3 result = base;
  switch (type) {
    case COMBINED_BLEND_ADD:
      result = base + fac * col;
      break;
    case COMBINED_BLEND_MULTIPLY:
      result = base * (facm + fac * col);
      break;
    case COMBINED_BLEND_SUBTRACT:
      result = base - fac * col;
      break;
    case COMBINED_BLEND_SCREEN:
      result = float3(1.0f) - (float3(facm) + fac * (float3(1.0f) - col)) * (float3(1.0f) - base);
      break;
    case COMBINED_BLEND_DIVIDE:
      /* A zero divisor leaves the channel as it was. */
      result.x = (col.x != 0.0f) ? facm * base.x + fac * base.x / col.x : base.x;
      result.y = (col.y != 0.0f) ? facm * base.y + fac * base.y / col.y : base.y;
      result.z = (col.z != 0.0f) ? facm * base.z + fac * base.z / col.z : base.z;
      break;
    case COMBINED_BLEND_DIFFERENCE:
      result = facm * base + fac * abs(base - col);
      break;
    case COMBINED_BLEND_DARKEN:
      result = min(base, col) * fac + base * facm;
      break;
    case COMBINED_BLEND_LIGHTEN:
      result = max(base, col) * fac + base * facm;
      break;
    case COMBINED_BLEND_OVERLAY:
      result = float3(overlay_channel(base.x, col.x, fac),
                      overlay_channel(base.y, col.y, fac),
                      overlay_channel(base.z, col.z, fac));
      break;
    case COMBINED_BLEND_DODGE:
      result = float3(dodge_channel(base.x, col.x, fac),
                      dodge_channel(base.y, col.y, fac),
                      dodge_channel(base.z, col.z, fac));
      break;
    case COMBINED_BLEND_BURN:
      result = float3(burn_channel(base.x, col.x, fac),
                      burn_channel(base.y, col.y, fac),
                      burn_channel(base.z, col.z, fac));
      break;
    case COMBINED_BLEND_HUE: {
      float3 col_hsv = rgb_to_hsv3(col);
      if (col_hsv.y != 0.0f) {
        float3 base_hsv = rgb_to_hsv3(base);
        result = facm * base + fac * hsv_to_rgb3(float3(col_hsv.x, base_hsv.y, base_hsv.z));
      }
      break;
    }
    case COMBINED_BLEND_SATURATION: {
      float3 base_hsv = rgb_to_hsv3(base);
      if (base_hsv.y != 0.0f) {
        float3 col_hsv = rgb_to_hsv3(col);
        result = hsv_to_rgb3(float3(base_hsv.x, facm * base_hsv.y + fac * col_hsv.y, base_hsv.z));
      }
      break;
    }
    case COMBINED_BLEND_VALUE: {
      float3 base_hsv = rgb_to_hsv3(base);
      float3 col_hsv = rgb_to_hsv3(col);
      result = hsv_to_rgb3(float3(base_hsv.x, base_hsv.y, facm * base_hsv.z + fac * col_hsv.z));
      break;
    }
    case COMBINED_BLEND_COLOR: {
      float3 col_hsv = rgb_to_hsv3(col);
      if (col_hsv.y != 0.0f) {
        float3 base_hsv = rgb_to_hsv3(base);
        result = facm * base + fac * hsv_to_rgb3(float3(col_hsv.x, col_hsv.y, base_hsv.z));
      }
      break;
    }
    case COMBINED_BLEND_SOFT_LIGHT: {
      /* The non-factor Screen mix first, as `ramp_blend` does. */
      float3 screen = float3(1.0f) - (float3(1.0f) - col) * (float3(1.0f) - base);
      result = facm * base + fac * ((float3(1.0f) - base) * col * base + base * screen);
      break;
    }
    case COMBINED_BLEND_LINEAR_LIGHT:
      result = float3(linear_light_channel(base.x, col.x, fac),
                      linear_light_channel(base.y, col.y, fac),
                      linear_light_channel(base.z, col.z, fac));
      break;
    case COMBINED_BLEND_EXCLUSION:
      result = max(facm * base + fac * (base + col - 2.0f * base * col), float3(0.0f));
      break;
    default:
      break;
  }
  return result;
}

/** \} */

/** The whiteout blend of two encoded tangent-space normals (lend_normal_combine). */
float3 normal_combine(float3 base_encoded, float3 detail_encoded)
{
  float3 base = base_encoded * 2.0f - 1.0f;
  float3 detail = detail_encoded * 2.0f - 1.0f;
  float3 combined = float3(base.x + detail.x, base.y + detail.y, base.z * detail.z);
  /* A zero vector stays zero, as `math::normalize` has it; GLSL would make it NaN. */
  if (dot(combined, combined) > 0.0f) {
    combined = normalize(combined);
  }
  return combined * 0.5f + 0.5f;
}

/* -------------------------------------------------------------------- */
/** \name Texel Hash
 *
 * The grain every texel-noise family reads, ported from `paint_layer_texel_hash_uniform` and the
 * `BLI_hash_int_2d` it calls: Jenkins' lookup3 `final` over the two texel coordinates. Every step is
 * uint32 arithmetic with the same constants as the C++, so the sequence cannot drift between the CPU
 * kernel, the bake and this pass; the shifts are never by zero, so no backend's undefined shift rule
 * is reached. Deband and a Color Match's variation read the same family, which is why this is a
 * section of its own rather than part of the noise below.
 * \{ */

/** `rot(x, k)` of `BLI_hash.h`. */
uint texel_hash_rot(uint x, uint k)
{
  return (x << k) | (x >> (32u - k));
}

/** `BLI_hash_int_2d`: the `final` mix of the two coordinates and the fixed seed. */
uint texel_hash_int_2d(uint kx, uint ky)
{
  uint a = 0xdeadbeefu + (2u << 2u) + 13u;
  uint b = a;
  uint c = a;
  a += kx;
  b += ky;
  c ^= b;
  c -= texel_hash_rot(b, 14u);
  a ^= c;
  a -= texel_hash_rot(c, 11u);
  b ^= a;
  b -= texel_hash_rot(a, 25u);
  c ^= b;
  c -= texel_hash_rot(b, 16u);
  a ^= c;
  a -= texel_hash_rot(c, 4u);
  b ^= a;
  b -= texel_hash_rot(a, 14u);
  c ^= b;
  c -= texel_hash_rot(b, 24u);
  return c;
}

/**
 * `paint_layer_texel_hash_uniform`: one uniform in [0, 1) of the texel hash family, the salt keeping
 * the passes and the channels apart and the seed moving the pattern. The coordinates are those of the
 * texture being grained, not a normalized position: the grain is a field over the canvas, so a bake
 * at another resolution shows the same pattern resampled rather than a different one.
 */
float texel_hash_uniform(int2 texel, int seed, uint salt)
{
  const uint kx = uint(texel.x) + salt * 7919u + uint(seed) * 6151u;
  const uint ky = uint(texel.y);
  return float(texel_hash_int_2d(kx, ky)) / 4294967296.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Filters
 *
 * The filters of an adjustment, a correction or a mask item (`composite_filter_color`), at full
 * strength. The parameters are the kind's schema order: Levels reads `in_black`, `in_white`,
 * `gamma`, `out_black`, `out_white`; HSL reads `hue`, `saturation`, `value`.
 * \{ */

#define COMBINED_FILTER_NONE 0
#define COMBINED_FILTER_INVERT 1
#define COMBINED_FILTER_LEVELS 2
#define COMBINED_FILTER_HSL 3
#define COMBINED_FILTER_BRIGHTNESS_CONTRAST 6
#define COMBINED_FILTER_COLOR_RAMP 7
#define COMBINED_FILTER_POSTERIZE 8
#define COMBINED_FILTER_MATH 9
#define COMBINED_FILTER_THRESHOLD 10
#define COMBINED_FILTER_CLAMP 11
#define COMBINED_FILTER_CURVES 12
#define COMBINED_FILTER_COLOR_BALANCE 13
#define COMBINED_FILTER_EXPOSURE 23
#define COMBINED_FILTER_DEBAND 24
#define COMBINED_FILTER_NORMAL_CONVERT 26
#define COMBINED_FILTER_CHANNEL_MIXER 27
#define COMBINED_FILTER_HISTOGRAM_SCAN 30
#define COMBINED_FILTER_HISTOGRAM_SHIFT 31
#define COMBINED_FILTER_SMOOTHSTEP 37
#define COMBINED_FILTER_CONTRAST_LUMINOSITY 38
#define COMBINED_FILTER_HEIGHT_ADJUST 39
#define COMBINED_FILTER_HSL_PERSPECTIVE 41

/* The modes the Deband and the Normal Convert read in their first parameter, the values of
 * `PaintLayerDebandMode` and `PaintLayerNormalConvertMode`. */
#define COMBINED_DEBAND_DEBAND 0
#define COMBINED_DEBAND_DITHER 1
#define COMBINED_NORMAL_CONVERT_DX_TO_GL 0
#define COMBINED_NORMAL_CONVERT_GL_TO_DX 1
#define COMBINED_NORMAL_CONVERT_REBUILD_Z 2
#define COMBINED_NORMAL_CONVERT_CLEAR_Z 3
#define COMBINED_NORMAL_CONVERT_LINEAR_TO_SRGB 4
#define COMBINED_NORMAL_CONVERT_SRGB_TO_LINEAR 5

float levels_channel(float value, float4 p0, float4 p1)
{
  /* A zero span or gamma reads as zero, which is what the Math Divide node gives the graph. */
  float span = p0.y - p0.x;
  float k = (span != 0.0f) ? 1.0f / span : 0.0f;
  float m = -(p0.x * k);
  float inv_gamma = (p0.z != 0.0f) ? 1.0f / p0.z : 0.0f;
  float t = clamp(value * k + m, 0.0f, 1.0f);
  /* The Gamma node leaves a non-positive value as it is. */
  if (t > 0.0f) {
    t = pow(t, inv_gamma);
  }
  return t * (p1.x - p0.w) + p0.w;
}

/** One channel of the Math filter (the CPU's `paint_layer_math_apply`, the Math node's own rules:
 * a negative base under a fractional exponent and a non-positive logarithm base or argument read
 * zero, as does a division by zero). */
float math_channel(float a, int operation, float b, bool use_clamp)
{
  float result = a;
  if (operation == 1) {
    result = a - b;
  }
  else if (operation == 2) {
    result = a * b;
  }
  else if (operation == 3) {
    result = (b != 0.0f) ? a / b : 0.0f;
  }
  else if (operation == 4) {
    result = min(a, b);
  }
  else if (operation == 5) {
    result = max(a, b);
  }
  else if (operation == 6) {
    /* The CPU's `safe_powf`: a negative base only carries an integer exponent. GLSL pow refuses
     * negatives, so the sign goes in by the exponent's parity, the way the node's own shader does. */
    if (a < 0.0f) {
      if (b != float(int(b))) {
        result = 0.0f;
      }
      else {
        float raised = pow(-a, b);
        result = (mod(abs(b), 2.0f) < 0.5f) ? raised : -raised;
      }
    }
    else {
      result = pow(a, b);
    }
  }
  else if (operation == 7) {
    /* The CPU's `safe_logf`: a non-positive base or argument reads zero, and so does a base of
     * one, whose logarithm divides by zero. */
    result = (a > 0.0f && b > 0.0f && log2(b) != 0.0f) ? log2(a) / log2(b) : 0.0f;
  }
  return use_clamp ? clamp(result, 0.0f, 1.0f) : result;
}

/* -------------------------------------------------------------------- */
/** \name Filter LUTs
 *
 * A Color Ramp and an RGB Curves are not a handful of uniforms: the gather bakes their payload into
 * a 2D RGBA32F table and reads it with `texelFetch` and explicit interpolation, so the result does
 * not depend on sampler filtering. The layout is the one `paint_layer_*_lut_build` writes.
 * \{ */

/** One texel of a filter LUT, clamped to its bounds so an out-of-range index reads the edge. */
float4 filter_lut_fetch(sampler2D lut_tx, int x, int y)
{
  int2 size = textureSize(lut_tx, 0);
  return texelFetch(lut_tx, int2(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1)), 0);
}

/** Row 0 of an RGB Curves table at \a parameter, linearly interpolated like `CurveMap::range`. */
float4 curves_lut_sample(sampler2D lut_tx, float parameter)
{
  int last = textureSize(lut_tx, 0).x - 1;
  float index = parameter * float(last);
  float low = floor(index);
  float fraction = index - low;
  return mix(filter_lut_fetch(lut_tx, int(low), 0),
             filter_lut_fetch(lut_tx, int(low) + 1, 0),
             fraction);
}

/** `extrapolate_if_needed` of the RGB Curves node: the edge value plus the slope outside [0, 1]. */
float3 curves_lut_extrapolate(float3 parameters,
                              float3 values,
                              float3 start_slopes,
                              float3 end_slopes)
{
  float3 slopes = float3(0.0f);
  slopes = select(slopes, end_slopes, greaterThan(parameters, float3(1.0f)));
  slopes = select(slopes, start_slopes, lessThan(parameters, float3(0.0f)));
  float3 above = select(float3(0.0f), float3(1.0f), greaterThan(parameters, float3(1.0f)));
  return values + (parameters - above) * slopes;
}

/** The RGB Curves node's `curves_combined_rgb`, without white balancing: the CPU does the same. */
float3 apply_curves(sampler2D lut_tx, float3 rgb)
{
  /* Row 1: the start slopes, end slopes, range minimums and range dividers of the four curves. */
  float4 start_slopes = filter_lut_fetch(lut_tx, 0, 1);
  float4 end_slopes = filter_lut_fetch(lut_tx, 1, 1);
  float4 range_minimums = filter_lut_fetch(lut_tx, 2, 1);
  float4 range_dividers = filter_lut_fetch(lut_tx, 3, 1);

  /* The combined curve first, on all three channels. */
  float3 parameters = (rgb - range_minimums.aaa) * range_dividers.aaa;
  float3 combined = float3(curves_lut_sample(lut_tx, parameters.x).a,
                           curves_lut_sample(lut_tx, parameters.y).a,
                           curves_lut_sample(lut_tx, parameters.z).a);
  combined = curves_lut_extrapolate(parameters, combined, start_slopes.aaa, end_slopes.aaa);

  /* Then each channel's own curve. */
  parameters = (combined - range_minimums.rgb) * range_dividers.rgb;
  float3 result = float3(curves_lut_sample(lut_tx, parameters.x).r,
                         curves_lut_sample(lut_tx, parameters.y).g,
                         curves_lut_sample(lut_tx, parameters.z).b);
  return curves_lut_extrapolate(parameters, result, start_slopes.rgb, end_slopes.rgb);
}

/** The Color Ramp's colour at the Rec.709 luminance of \a rgb; `use_nearest` reads the nearest. */
float4 apply_color_ramp(sampler2D lut_tx, float3 rgb, bool use_nearest)
{
  float width = float(textureSize(lut_tx, 0).x);
  /* The RGB to BW node's scene-linear Rec.709 luminance, as `paint_layer_color_ramp_apply`. */
  float value = clamp(0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z, 0.0f, 1.0f);
  float index = value * (width - 1.0f);
  if (use_nearest) {
    return filter_lut_fetch(lut_tx, int(index + 0.5f), 0);
  }
  float low = floor(index);
  return mix(filter_lut_fetch(lut_tx, int(low), 0),
             filter_lut_fetch(lut_tx, int(low) + 1, 0),
             index - low);
}

/** \} */

/**
 * The scalar `linearrgb_to_srgb` / `srgb_to_linearrgb` of `BLI_math_color`, ported verbatim (both
 * branches) with the `effect_` prefix so they cannot collide with the display-encoding helpers of
 * `gpu_shader_common_color_utils.glsl`. A High Pass takes its detail through them, and the Normal
 * Convert's colour-space modes run them in `apply_filter`, so they stand above it: the shader and
 * the CPU kernel must agree on the piecewise curve, including the low-value linear branch.
 */
float effect_linearrgb_to_srgb(float c)
{
  if (c < 0.0031308f) {
    return (c < 0.0f) ? 0.0f : c * 12.92f;
  }
  return 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

float effect_srgb_to_linearrgb(float c)
{
  if (c < 0.04045f) {
    return (c < 0.0f) ? 0.0f : c * (1.0f / 12.92f);
  }
  return pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}

/** One channel of the Deband, the loop body of the CPU's `paint_layer_deband_apply`
 * (`paint_layers_kind.cc`): the Dither mode rounds the value through the quantizer with the raw TPDF
 * half a step in, the Deband mode adds the triangle widened by the CPU's
 * #PAINT_LAYER_DEBAND_NOISE_SCALE (1.5). The clamp is the CPU's own. */
float deband_channel_apply(float value, float u1, float u2, int mode, int levels, float step)
{
  if (mode == COMBINED_DEBAND_DITHER) {
    return clamp(floor(value * float(levels) + (u1 - u2) + 0.5f) / float(levels), 0.0f, 1.0f);
  }
  return clamp(value + (u1 - u2) * step * 1.5f, 0.0f, 1.0f);
}

/**
 * One filter at full strength. `.xyz` is the filtered colour and `.w` the strength multiplier the
 * CPU's `composite_filter_color` returns: a Color Ramp's stop alpha when Affect Alpha is set, one
 * for every other filter and for a ramp that ignores it. The Color Balance reads its exponent
 * triple from `p2`; every other filter leaves it unused. \a texel is the caller's own texel: the
 * grain of a Deband keys on it, as the CPU passes `texel_xy` to the same call.
 */
float4 apply_filter(int filter_kind,
                    int2 texel,
                    float3 rgb,
                    float4 p0,
                    float4 p1,
                    float4 p2,
                    sampler2D lut_tx)
{
  float3 result = rgb;
  float strength = 1.0f;
  switch (filter_kind) {
    case COMBINED_FILTER_INVERT:
      /* `lerp(c, |c - 1|, amount)` per channel; the schema's three amounts ride in `p0.xyz`, each
       * defaulting to one, so a row without stored parameters inverts the whole way as before. */
      result = float3(mix(rgb.x, abs(rgb.x - 1.0f), p0.x),
                      mix(rgb.y, abs(rgb.y - 1.0f), p0.y),
                      mix(rgb.z, abs(rgb.z - 1.0f), p0.z));
      break;
    case COMBINED_FILTER_LEVELS:
      result = float3(levels_channel(rgb.x, p0, p1),
                      levels_channel(rgb.y, p0, p1),
                      levels_channel(rgb.z, p0, p1));
      break;
    case COMBINED_FILTER_HSL: {
      float3 turned_rgb = rgb;
      {
        float3 hsv = rgb_to_hsv3(rgb);
        float turned = hsv.x + p0.x + 0.5f;
        hsv.x = turned - floor(turned);
        hsv.y = clamp(hsv.y * p0.y, 0.0f, 1.0f);
        hsv.z = hsv.z * p0.z;
        turned_rgb = max(hsv_to_rgb3(hsv), float3(0.0f));
      }
      /* The CPU's vibrance on top: the lift away from the Rec.709 grey, most where the colour is
       * least saturated, clamped below zero like the HSV step. `p0.w` carries it. */
      float vibrance = p0.w;
      if (vibrance != 0.0f) {
        float max_c = max(turned_rgb.x, max(turned_rgb.y, turned_rgb.z));
        float min_c = min(turned_rgb.x, min(turned_rgb.y, turned_rgb.z));
        float sat = (max_c > 1e-6f) ? (max_c - min_c) / max_c : 0.0f;
        float boost = vibrance * (1.0f - sat);
        float gray = dot(turned_rgb, float3(0.2126f, 0.7152f, 0.0722f));
        turned_rgb = max(turned_rgb + (turned_rgb - gray) * boost, float3(0.0f));
      }
      result = turned_rgb;
      break;
    }
    case COMBINED_FILTER_COLOR_RAMP: {
      /* `p1.x` says the band is CONSTANT; `p0.x` is Affect Alpha, the schema's first parameter. */
      float4 ramp = apply_color_ramp(lut_tx, rgb, p1.x > 0.5f);
      result = ramp.xyz;
      strength = (p0.x > 0.5f) ? clamp(ramp.w, 0.0f, 1.0f) : 1.0f;
      break;
    }
    case COMBINED_FILTER_BRIGHTNESS_CONTRAST: {
      float a = 1.0f + p0.y;
      float b = p0.x - p0.y * 0.5f;
      result = max(a * rgb + b, float3(0.0f));
      break;
    }
    case COMBINED_FILTER_POSTERIZE: {
      /* The CPU's quantization: steps evenly spaced levels over [0, 1], endpoints preserved. */
      float k = max(p0.x - 1.0f, 1.0f);
      result = floor(clamp(rgb, 0.0f, 1.0f) * k + 0.5f) / k;
      break;
    }
    case COMBINED_FILTER_MATH: {
      /* `op(c, operand_r/g/b)` per channel, the operation and the clamp read from the first
       * parameters; the operand of each channel is its own. The safe rules are the Math node's
       * own, the ones the CPU composite follows. */
      int operation = int(p0.x + 0.5f);
      bool clamped = p0.y > 0.5f;
      result = float3(math_channel(rgb.x, operation, p0.z, clamped),
                      math_channel(rgb.y, operation, p0.w, clamped),
                      math_channel(rgb.z, operation, p1.x, clamped));
      break;
    }
    case COMBINED_FILTER_THRESHOLD: {
      /* The Math node's Greater Than, `1` strictly above the threshold (the CPU's own edge). */
      float threshold = p0.x;
      result = float3(rgb.x > threshold ? 1.0f : 0.0f,
                      rgb.y > threshold ? 1.0f : 0.0f,
                      rgb.z > threshold ? 1.0f : 0.0f);
      break;
    }
    case COMBINED_FILTER_CLAMP: {
      /* `min(max(c, min), max)` in the Math node's order, which stays defined when min > max. */
      /* Per component: a vector against a scalar bound is not an overload every backend has. */
      result = float3(min(max(rgb.x, p0.x), p0.y),
                      min(max(rgb.y, p0.x), p0.y),
                      min(max(rgb.z, p0.x), p0.y));
      break;
    }
    case COMBINED_FILTER_HISTOGRAM_SCAN: {
      /* The CPU's `paint_layer_histogram_scan_apply`: the smoothstep centered on the position and
       * opened by the contrast into a ramp of the same width each side; `contrast = 0` is the
       * Threshold the position alone makes. `p0.x` is the position, `p0.y` the contrast and
       * `p0.z` the invert switch. The step is written out per component, the floor of the span
       * keeping a zero contrast defined (the built-in smoothstep's equal-edge pair is not): what
       * the CPU's own clamp does. An inverted position flips the two states, `1 - out`. */
      float edge = p0.y * 0.5f;
      float lo = p0.x - edge;
      float span = max(edge * 2.0f, 1e-6f);
      float tx = clamp((rgb.x - lo) / span, 0.0f, 1.0f);
      float ty = clamp((rgb.y - lo) / span, 0.0f, 1.0f);
      float tz = clamp((rgb.z - lo) / span, 0.0f, 1.0f);
      result = float3(tx * tx * (3.0f - 2.0f * tx),
                      ty * ty * (3.0f - 2.0f * ty),
                      tz * tz * (3.0f - 2.0f * tz));
      if (p0.z > 0.5f) {
        result = 1.0f - result;
      }
      break;
    }
    case COMBINED_FILTER_HISTOGRAM_SHIFT: {
      /* The CPU's `paint_layer_histogram_shift_apply`: the value plus the position, wrapped back
       * through zero when it passes one -- the Math node's Wrap, cyclic, so a full turn is the
       * value again. `p0.x` is the position. */
      result = fract(rgb + float3(p0.x));
      break;
    }
    case COMBINED_FILTER_SMOOTHSTEP: {
      /* The CPU's paint_layer_smoothstep_apply: the Hermite interpolation between the range's
       * edges, written out per component. An empty range reads as zero (the graph's Map Range
       * gives zero for equal edges too). p0.x is the minimum, p0.y the maximum. */
      float span = p0.y - p0.x;
      float inv_span = (abs(span) < 1e-6f) ? 0.0f : 1.0f / span;
      float3 t = float3(clamp((rgb.x - p0.x) * inv_span, 0.0f, 1.0f),
                        clamp((rgb.y - p0.x) * inv_span, 0.0f, 1.0f),
                        clamp((rgb.z - p0.x) * inv_span, 0.0f, 1.0f));
      result = t * t * (float3(3.0f) - 2.0f * t);
      break;
    }
    case COMBINED_FILTER_CONTRAST_LUMINOSITY: {
      /* The CPU's `paint_layer_contrast_luminosity_apply`: the distance from the middle scaled by
       * the contrast, then shifted by the luminosity. `p0.x` is the contrast, `p0.y` the
       * luminosity. */
      result = (rgb - float3(0.5f)) * (1.0f + p0.x) + float3(0.5f + p0.y);
      break;
    }
    case COMBINED_FILTER_HEIGHT_ADJUST: {
      /* The CPU's `paint_layer_height_adjust_apply`: the invert, then the offset, then the
       * multiply -- the order the filter's own note pins, where a zero offset result annihilates
       * the multiply. The result stays open above one. The params ride in the schema order:
       * `p0.x` is the invert switch, `p0.y` the offset, `p0.z` the multiply. Per component: the
       * invert flips before the offset reads. */
      float3 flipped = (p0.x > 0.5f) ? (1.0f - rgb) : rgb;
      result = (flipped + float3(p0.y)) * p0.z;
      break;
    }
    case COMBINED_FILTER_HSL_PERSPECTIVE: {
      /* The CPU's `paint_layer_hsl_perspective_apply`: the hue and the saturation shift the way the
       * hsv stage does (the hue's turns, the saturation's factor of one plus the double), and the
       * lightness scales the value channel in the Regular mode or lifts toward white or black in
       * the Perceptive one. The params ride in the schema order: `p0.x` is the mode, `p0.y` the hue
       * in turns, `p0.z` the raw saturation slider, `p0.w` the raw lightness slider. */
      float lightness = p0.w * 2.0f;
      bool perceptive = p0.x > 0.5f;
      float3 hsv = rgb_to_hsv3(rgb);
      float turned = hsv.x + p0.y;
      hsv.x = turned - floor(turned);
      hsv.y = clamp(hsv.y * (1.0f + p0.z * 2.0f), 0.0f, 1.0f);
      if (!perceptive) {
        hsv.z = hsv.z * (1.0f + lightness);
      }
      float3 shifted = max(hsv_to_rgb3(hsv), float3(0.0f));
      if (perceptive) {
        shifted = shifted + max(lightness, 0.0f) * (float3(1.0f) - shifted) +
                  min(lightness, 0.0f) * shifted;
      }
      result = shifted;
      break;
    }    case COMBINED_FILTER_COLOR_BALANCE: {
      /* `pow(max(rgb * A + B, 0), E)` per channel, the folded triples the CPU composite reads:
       * `p0.xyz` is A, `p1.xyz` is B and `p2.xyz` is E (each in the `.xyz` of its own float4, not
       * laid end to end). Per component: the vectorized pow is not an overload every backend has.
       */
      float raised_x = pow(max(rgb.x * p0.x + p1.x, 0.0f), p2.x);
      float raised_y = pow(max(rgb.y * p0.y + p1.y, 0.0f), p2.y);
      float raised_z = pow(max(rgb.z * p0.z + p1.z, 0.0f), p2.z);
      result = float3(raised_x, raised_y, raised_z);
      break;
    }
    case COMBINED_FILTER_CURVES:
      result = apply_curves(lut_tx, rgb);
      break;
    case COMBINED_FILTER_EXPOSURE: {
      /* The CPU's `paint_layer_exposure_apply`: the colour scaled by `2^exposure`, then raised to
       * one over the gamma. A non-positive value is left as it is (the Gamma node's own rule), so
       * an HDR texel above one stays above one. `p0.x` is exposure and `p0.y` the gamma; the gamma
       * never reaches zero (the schema clamps it to 0.1 at the least), so the reciprocal is safe.
       * Per component: the vectorized pow is not an overload every backend has. */
      float scale = pow(2.0f, p0.x);
      float exponent = 1.0f / p0.y;
      float scaled_x = rgb.x * scale;
      float scaled_y = rgb.y * scale;
      float scaled_z = rgb.z * scale;
      result = float3(scaled_x > 0.0f ? pow(scaled_x, exponent) : scaled_x,
                      scaled_y > 0.0f ? pow(scaled_y, exponent) : scaled_y,
                      scaled_z > 0.0f ? pow(scaled_z, exponent) : scaled_z);
      break;
    }
    case COMBINED_FILTER_CHANNEL_MIXER: {
      /* `clamp(dot(row_k, rgb) + constant_k, 0, 1)` per output channel, the folded matrix the CPU
       * composite reads: the rows ride in the `.xyz` of `p0`, `p1` and `p2` and the constants in
       * their `.w` (the monochrome switch is already folded into them). The dot products are written
       * out per component and the clamp uses explicit floats: no vector-against-scalar overload is
       * assumed on every backend. */
      float mixed_x = p0.x * rgb.x + p0.y * rgb.y + p0.z * rgb.z + p0.w;
      float mixed_y = p1.x * rgb.x + p1.y * rgb.y + p1.z * rgb.z + p1.w;
      float mixed_z = p2.x * rgb.x + p2.y * rgb.y + p2.z * rgb.z + p2.w;
      result = float3(clamp(mixed_x, 0.0f, 1.0f),
                      clamp(mixed_y, 0.0f, 1.0f),
                      clamp(mixed_z, 0.0f, 1.0f));
      break;
    }
    case COMBINED_FILTER_DEBAND: {
      /* The CPU's `paint_layer_deband_apply` (`paint_layers_kind.cc`): the addon's TPDF, two
       * independent uniforms making a triangular density of the quantization step's own width, one
       * pair per channel and texel. The uniforms come from the texel hash family above, keyed by
       * the texel's own coordinates -- the grain is a field over the canvas, so a bake and this pass
       * read the same one, which is why the texel travels with the call. `p0` is the schema order:
       * `.x` the mode, `.y` the bit depth, `.z` the seed. The salt `channel * 2 + pass` keeps the
       * two passes and the three channels apart, the CPU's `paint_layer_deband_noise`. */
      const int mode = int(p0.x);
      const int levels = (1 << int(p0.y)) - 1;
      const float step = 1.0f / float(levels);
      const int seed = int(p0.z);
      const float u1_x = texel_hash_uniform(texel, seed, 1u);
      const float u2_x = texel_hash_uniform(texel, seed, 2u);
      const float u1_y = texel_hash_uniform(texel, seed, 3u);
      const float u2_y = texel_hash_uniform(texel, seed, 4u);
      const float u1_z = texel_hash_uniform(texel, seed, 5u);
      const float u2_z = texel_hash_uniform(texel, seed, 6u);
      /* Per component: a dynamic subscript of a vector is not an overload every backend has. */
      result = float3(deband_channel_apply(rgb.x, u1_x, u2_x, mode, levels, step),
                      deband_channel_apply(rgb.y, u1_y, u2_y, mode, levels, step),
                      deband_channel_apply(rgb.z, u1_z, u2_z, mode, levels, step));
      break;
    }
    case COMBINED_FILTER_NORMAL_CONVERT: {
      /* The CPU's `paint_layer_normal_convert_apply` (`paint_layers_kind.cc`): the mode rides in
       * `p0.x`, the Normalize switch in `p0.y`. The two encodings flip the green (the same conversion
       * either way), Rebuild Z re-encodes the blue the unit length leaves, Clear Z drops it, and the
       * two colour-space modes run the IEC 61966-2.1 broken line -- the same `BLI_math_color`
       * scalars the High Pass tail reads. */
      const int mode = int(p0.x);
      if (mode == COMBINED_NORMAL_CONVERT_DX_TO_GL || mode == COMBINED_NORMAL_CONVERT_GL_TO_DX) {
        result.y = 1.0f - rgb.y;
      }
      else if (mode == COMBINED_NORMAL_CONVERT_REBUILD_Z) {
        /* The two encoded xy, decoded; the blue the unit length leaves, encoded again. */
        const float vx = rgb.x * 2.0f - 1.0f;
        const float vy = rgb.y * 2.0f - 1.0f;
        const float z = sqrt(max(1.0f - vx * vx - vy * vy, 0.0f));
        result.z = (z + 1.0f) * 0.5f;
      }
      else if (mode == COMBINED_NORMAL_CONVERT_CLEAR_Z) {
        result.z = 0.0f;
      }
      else if (mode == COMBINED_NORMAL_CONVERT_LINEAR_TO_SRGB) {
        result = float3(effect_linearrgb_to_srgb(rgb.x),
                        effect_linearrgb_to_srgb(rgb.y),
                        effect_linearrgb_to_srgb(rgb.z));
      }
      else if (mode == COMBINED_NORMAL_CONVERT_SRGB_TO_LINEAR) {
        result = float3(effect_srgb_to_linearrgb(rgb.x),
                        effect_srgb_to_linearrgb(rgb.y),
                        effect_srgb_to_linearrgb(rgb.z));
      }
      /* The switch's default leaves the colour alone, and the Normalize switch runs whatever the
       * mode wrote: the decoded vector re-laid on the unit sphere, a zero vector keeping its place
       * (the CPU's floor keeps the division finite). */
      if (int(p0.y) != 0) {
        const float vx = result.x * 2.0f - 1.0f;
        const float vy = result.y * 2.0f - 1.0f;
        const float vz = result.z * 2.0f - 1.0f;
        const float len = max(sqrt(vx * vx + vy * vy + vz * vz), 1e-10f);
        result = float3((vx / len) * 0.5f + 0.5f,
                        (vy / len) * 0.5f + 0.5f,
                        (vz / len) * 0.5f + 0.5f);
      }
      break;
    }
    default:
      break;
  }
  return float4(result, strength);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Map Sampling
 * \{ */

/** Read the map at the nearest texel instead of filtering it; set by the ID map of a coverage item. */
#define COMBINED_MAP_NEAREST 131072

/**
 * The Mirror fold of one coordinate around \a center. The reference is
 * `BKE_paint_layers_mapping_mirror_fold` (BKE_paint_layers.hh); \a negative keeps the smaller side.
 */
float combined_mirror_fold(float coordinate, float center, bool negative)
{
  float distance = abs(coordinate - center);
  return negative ? center - distance : center + distance;
}

/**
 * The side sign the fold gives a coordinate, as `BKE_paint_layers_mapping_mirror_side`: +1 at and
 * beyond the center. The negative fold's derivative is the positive one's, negated.
 */
float combined_mirror_side(float coordinate, float center, bool negative)
{
  float side = (coordinate >= center) ? 1.0f : -1.0f;
  return negative ? -side : side;
}

/**
 * One transform effect's step on the map coordinate \a p, as `CompositeMappingTransform::
 * apply_fold_affine`: the Mirror fold about the center in `b.zw`, then the rotation in `b.xy`, the
 * scale in `a.zw` and the offset in `a.xy`. No repeat is applied: the caller wraps the composed
 * lookup. \a r_side receives the fold's side signs, for the tangent-Normal remap.
 * \return false when an axis that clips puts the coordinate outside the unit square.
 */
bool combined_input_step_apply(const int step_flags,
                               const float4 a,
                               const float4 b,
                               float2 &p,
                               float2 &r_side)
{
  float2 folded = p;
  r_side = float2(1.0f);
  /* The direction multiplies both the fold and the side sign, as the CPU's `negative` fold. */
  const bool negative_u = (step_flags & COMBINED_INPUT_STEP_MIRROR_U_NEG) != 0;
  const bool negative_v = (step_flags & COMBINED_INPUT_STEP_MIRROR_V_NEG) != 0;
  if ((step_flags & COMBINED_INPUT_STEP_MIRROR_U) != 0) {
    folded.x = combined_mirror_fold(p.x, b.z, negative_u);
    r_side.x = combined_mirror_side(p.x, b.z, negative_u);
  }
  if ((step_flags & COMBINED_INPUT_STEP_MIRROR_V) != 0) {
    folded.y = combined_mirror_fold(p.y, b.w, negative_v);
    r_side.y = combined_mirror_side(p.y, b.w, negative_v);
  }
  float2 rotated = float2(folded.x * b.x - folded.y * b.y, folded.x * b.y + folded.y * b.x);
  p = rotated * a.zw + a.xy;
  const bool repeat_u = (step_flags & COMBINED_INPUT_STEP_REPEAT_U) != 0;
  const bool repeat_v = (step_flags & COMBINED_INPUT_STEP_REPEAT_V) != 0;
  return !((!repeat_u && (p.x < 0.0f || p.x >= 1.0f)) ||
           (!repeat_v && (p.y < 0.0f || p.y >= 1.0f)));
}

/**
 * One straight, scene-linear texel of a map at the canvas \a uv, read through the row's UV mapping.
 *
 * `uv' = (R * fold(uv)) * scale + offset` is the transform of the generated Mapping node chain and
 * of the CPU resample: the Mirror fold reads the canvas UV first, then the rotate-scale-offset
 * transform moves it. An axis that repeats wraps; one that clips reads transparent outside [0, 1).
 * A tangent-space Normal map has its vectors follow the mapping, as in
 * `composite_resample_mapped`: flipped by the sign of the scale, rotated back, then flipped per axis by the
 * side of the fold center the canvas point reads from.
 */
float4 sample_map(sampler2D tx,
                  float2 uv,
                  int flags,
                  float2 mapping_offset,
                  float2 mapping_scale,
                  float2 mapping_rotation,
                  float2 mapping_mirror_center,
                  const int input_step_count,
                  const float4 input_step_0_a,
                  const float4 input_step_0_b,
                  const float4 input_step_1_a,
                  const float4 input_step_1_b,
                  const int input_step_flags,
                  float lod)
{
  float2 sample_uv = uv;
  float2 side = float2(1.0f);
  bool clipped = false;
  /* The side signs each step's fold produced, for the tangent-Normal remap. The steps run first,
   * in list order: each folds about its center and then rotates, scales and offsets; an axis a
   * step clips makes the texel transparent. No step repeats -- the composed lookup repeats at the
   * end, the way the graph's Image Texture extension repeats the lookup it is fed (a clipping
   * step has already declared the texel transparent, so flooring an in-range coordinate is the
   * identity it reads as). */
  float2 step_side[COMBINED_INPUT_STEP_MAX];
  bool stepped = false;
  for (int s = 0; s < COMBINED_INPUT_STEP_MAX && s < input_step_count; s++) {
    float4 a = (s == 0) ? input_step_0_a : input_step_1_a;
    float4 b = (s == 0) ? input_step_0_b : input_step_1_b;
    int step_flags = (input_step_flags >> (s * COMBINED_INPUT_STEP_BITS)) & COMBINED_INPUT_STEP_MASK;
    if (!combined_input_step_apply(step_flags, a, b, sample_uv, step_side[s])) {
      clipped = true;
    }
    stepped = true;
  }
  /* The coordinate the row's own fold reads: the steps' output, so its side signs and its clip
   * test sit where the CPU's last step does. */
  float2 row_input = sample_uv;
  if ((flags & COMBINED_LAYER_MAPPED) != 0) {
    float2 folded = row_input;
    const bool row_negative_u = (flags & COMBINED_LAYER_MIRROR_U_NEG) != 0;
    const bool row_negative_v = (flags & COMBINED_LAYER_MIRROR_V_NEG) != 0;
    if ((flags & COMBINED_LAYER_MIRROR_U) != 0) {
      folded.x = combined_mirror_fold(row_input.x, mapping_mirror_center.x, row_negative_u);
      side.x = combined_mirror_side(row_input.x, mapping_mirror_center.x, row_negative_u);
    }
    if ((flags & COMBINED_LAYER_MIRROR_V) != 0) {
      folded.y = combined_mirror_fold(row_input.y, mapping_mirror_center.y, row_negative_v);
      side.y = combined_mirror_side(row_input.y, mapping_mirror_center.y, row_negative_v);
    }
    float2 rotated = float2(folded.x * mapping_rotation.x - folded.y * mapping_rotation.y,
                            folded.x * mapping_rotation.y + folded.y * mapping_rotation.x);
    sample_uv = rotated * mapping_scale + mapping_offset;
    bool repeat_u = (flags & COMBINED_LAYER_REPEAT_U) != 0;
    bool repeat_v = (flags & COMBINED_LAYER_REPEAT_V) != 0;
    if (repeat_u) {
      sample_uv.x -= floor(sample_uv.x);
    }
    if (repeat_v) {
      sample_uv.y -= floor(sample_uv.y);
    }
    clipped = clipped || ((!repeat_u && (sample_uv.x < 0.0f || sample_uv.x >= 1.0f)) ||
                          (!repeat_v && (sample_uv.y < 0.0f || sample_uv.y >= 1.0f)));
  }
  else if (stepped) {
    /* Steps without a row mapping: the composed lookup is the read, and it repeats. */
    sample_uv -= floor(sample_uv);
  }
  float4 color;
  if (clipped) {
    color = float4(0.0f);
  }
  else if ((flags & COMBINED_MAP_NEAREST) != 0) {
    /* An ID map is a lookup: the texel under the point, never a blend of two. */
    int2 size = textureSize(tx, 0);
    int2 texel = clamp(int2(floor(sample_uv * float2(size))), int2(0), size - 1);
    color = texelFetch(tx, texel, 0);
  }
  else {
    color = textureLod(tx, sample_uv, lod);
  }
  if ((flags & COMBINED_LAYER_PREMULTIPLIED) != 0 && color.w > 0.0f) {
    color.xyz = color.xyz / color.w;
  }
  if ((flags & COMBINED_LAYER_TANGENT_NORMAL) != 0 && color.w > 0.0f) {
    float3 n = color.xyz * 2.0f - 1.0f;
    float2 v = n.xy;
    /* The row's own chain's inverse acts first: `side * R^T * S^-1` (flipped by the scale's sign,
     * then rotated back), the covariant counterpart of its `S * R * fold`; the magnitude of the
     * scale does not turn a direction. */
    if ((flags & COMBINED_LAYER_MAPPED) != 0) {
      float rx = (mapping_scale.x < 0.0f) ? -1.0f : 1.0f;
      float ry = (mapping_scale.y < 0.0f) ? -1.0f : 1.0f;
      float c = mapping_rotation.x;
      float s = mapping_rotation.y;
      float fx = rx * v.x;
      float fy = ry * v.y;
      /* The fold's side signs act on the canvas axes, after the rotation back. */
      v = float2(side.x * (c * fx + s * fy), side.y * (-s * fx + c * fy));
    }
    /* Then the steps' inverses, in reverse list order, each side sign taken where that step's
     * fold reads -- the composition the CPU resample applies to the same steps. */
    for (int s = input_step_count - 1; s >= 0; s--) {
      float4 a = (s == 0) ? input_step_0_a : input_step_1_a;
      float4 b = (s == 0) ? input_step_0_b : input_step_1_b;
      float rx = (a.z < 0.0f) ? -1.0f : 1.0f;
      float ry = (a.w < 0.0f) ? -1.0f : 1.0f;
      float c = b.x;
      float sv = b.y;
      float fx = rx * v.x;
      float fy = ry * v.y;
      v = float2(step_side[s].x * (c * fx + sv * fy),
                 step_side[s].y * (-sv * fx + c * fy));
    }
    color.xyz = float3(0.5f * v.x + 0.5f, 0.5f * v.y + 0.5f, 0.5f * n.z + 0.5f);
  }
  return color;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection
 *
 * The frame of a projected map, shared by the mask, layer and correction passes: a pass reads the
 * painted surface per texel (`proj_pos_tx`, `proj_nrm_tx`) instead of the canvas UV when
 * #COMBINED_LAYER_PROJECTED is set in its flags.
 * \{ */

/* The projection modes of `proj_mode`, and the bits of `proj_flags`. */
#define COMBINED_PROJ_PLANAR 0
#define COMBINED_PROJ_BOX 1
#define COMBINED_PROJ_CUBE 2
#define COMBINED_PROJ_CYLINDER 3
#define COMBINED_PROJ_SPHERE 4
#define COMBINED_PROJ_CLIP 1
#define COMBINED_PROJ_DEPTH 2
#define COMBINED_PROJ_FACING 4
#define COMBINED_PROJ_UV_ADJUST 8
#define COMBINED_PROJ_WRAP_CLIP 16
/* A Decal frame: `proj_pos_tx` holds `(q.xy, patch triangle id, covered)` instead of the surface
 * position, and `proj_p_to_q` only places `q` on the frame's axes (the host builds both), so
 * #projection_sample_map runs the Planar path unchanged. */
#define COMBINED_PROJ_DECAL 32
/* #PAINT_LAYER_PROJECTION_EPSILON, and the 1 / (2 pi) and 1 / pi of the angle to UV conversions. */
#define COMBINED_PROJ_EPSILON 1e-6f
#define COMBINED_PROJ_INV_TWO_PI 0.15915494309189535f
#define COMBINED_PROJ_INV_PI 0.3183098861837907f

/* A port of the helpers of `paint_material_composite_projection.cc` the projected bake is built
 * from (`composite_projection_face_uv`, `_cube_uv`, `_cylinder_uv`, `_sphere_uv`, `_frame_weight`,
 * `_uv_wrap`), so the CPU bake and this pass cannot disagree. */

float projection_axis_factor(float value, float extent, float soft)
{
  return clamp((extent - abs(value)) / soft, 0.0f, 1.0f);
}

/** The face UV of Box and Triplanar: face 0 is X, 1 is Y, 2 is Z. */
float2 projection_face_uv(int face, float3 q)
{
  float2 uv;
  if (face == 0) {
    uv = float2(0.5f - q.y * 0.5f, 0.5f + q.z * 0.5f);
  }
  else if (face == 1) {
    uv = float2(0.5f + q.x * 0.5f, 0.5f + q.z * 0.5f);
  }
  else {
    uv = float2(0.5f + q.x * 0.5f, 0.5f + q.y * 0.5f);
  }
  return uv;
}

/**
 * The cube-cross atlas UV of the dominant frame axis in `xy` and that face's cell origin in `zw`;
 * the six cell origins ride in three float4.
 */
float4 projection_cube_uv(float3 q, float4 atlas_0, float4 atlas_1, float4 atlas_2, float2 cell)
{
  float ax = abs(q.x);
  float ay = abs(q.y);
  float az = abs(q.z);
  int atlas_face;
  if (ax + COMBINED_PROJ_EPSILON > ay && ax + COMBINED_PROJ_EPSILON > az) {
    atlas_face = (q.x > 0.0f) ? 0 : 1;
  }
  else if (ay + COMBINED_PROJ_EPSILON > az) {
    atlas_face = (q.y > 0.0f) ? 2 : 3;
  }
  else {
    atlas_face = (q.z > 0.0f) ? 4 : 5;
  }
  float2 local_uv = projection_face_uv(atlas_face / 2, q);
  float2 origin;
  if (atlas_face == 0) {
    origin = atlas_0.xy;
  }
  else if (atlas_face == 1) {
    origin = atlas_0.zw;
  }
  else if (atlas_face == 2) {
    origin = atlas_1.xy;
  }
  else if (atlas_face == 3) {
    origin = atlas_1.zw;
  }
  else if (atlas_face == 4) {
    origin = atlas_2.xy;
  }
  else {
    origin = atlas_2.zw;
  }
  return float4(origin + local_uv * cell, origin);
}

/** Cylinder: U winds around the axis, V reads the height along it. */
float2 projection_cylinder_uv(float3 q, int axis)
{
  int a = (axis + 1) % 3;
  int b = (axis + 2) % 3;
  float angle = atan2(q[b], q[a]);
  return float2(angle * COMBINED_PROJ_INV_TWO_PI + 0.5f, 0.5f + q[axis] * 0.5f);
}

/** Sphere: U winds around the axis, V is the polar angle from it. */
float2 projection_sphere_uv(float3 q, int axis)
{
  int a = (axis + 1) % 3;
  int b = (axis + 2) % 3;
  float len = sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  float ratio = clamp(q[axis] / max(len, COMBINED_PROJ_EPSILON), -1.0f, 1.0f);
  float angle = atan2(q[b], q[a]);
  return float2(angle * COMBINED_PROJ_INV_TWO_PI + 0.5f, acos(ratio) * COMBINED_PROJ_INV_PI);
}

/** The frame factor: the footprint with its smooth edges under the clip, then the Planar facing. */
float projection_frame_weight(int mode,
                              int axis,
                              int pflags,
                              float3 q,
                              float3 n_frame,
                              float2 facing,
                              float extent,
                              float soft)
{
  float weight = 1.0f;
  if ((pflags & COMBINED_PROJ_CLIP) != 0) {
    if (mode == COMBINED_PROJ_BOX || mode == COMBINED_PROJ_CUBE) {
      weight = projection_axis_factor(q.x, extent, soft) *
               projection_axis_factor(q.y, extent, soft) *
               projection_axis_factor(q.z, extent, soft);
    }
    else if (mode == COMBINED_PROJ_CYLINDER) {
      int a = (axis + 1) % 3;
      int b = (axis + 2) % 3;
      float radial = q[a] * q[a] + q[b] * q[b];
      weight = (radial < 1.0f) ? 1.0f : 0.0f;
      weight *= projection_axis_factor(q[axis], extent, soft);
    }
    else if (mode == COMBINED_PROJ_SPHERE) {
      float length_sq = q.x * q.x + q.y * q.y + q.z * q.z;
      weight = (length_sq < 1.0f) ? 1.0f : 0.0f;
    }
    else {
      int u_index = (axis == 0) ? 1 : 0;
      int v_index = (axis == 2) ? 1 : 2;
      weight = projection_axis_factor(q[u_index], extent, soft) *
               projection_axis_factor(q[v_index], extent, soft);
      if ((pflags & COMBINED_PROJ_DEPTH) != 0) {
        weight *= projection_axis_factor(q[axis], extent, soft);
      }
    }
  }
  /* The facing is a Planar input: the volume kinds are reached from every side. */
  if (weight > 0.0f && (pflags & COMBINED_PROJ_FACING) != 0 && mode == COMBINED_PROJ_PLANAR) {
    float t = clamp(n_frame[axis] * facing.x + facing.y, 0.0f, 1.0f);
    weight *= t * t * (3.0f - 2.0f * t);
  }
  return weight;
}

/**
 * One tap of the row's map at a projected \a uv: the row's UV adjust and wrap first (the Cube atlas
 * never wraps), then a bilinear read of the base level; a clipped tap reads zero. Straight colour
 * and alpha, like `CompositeProjectionSampler::sample` followed by its straightening.
 */
float4 projection_tap(sampler2D tx,
                      float2 uv,
                      int layer_flags,
                      int pflags,
                      bool is_cube,
                      float2 cube_origin,
                      float2 cube_cell,
                      float2 offset,
                      float2 scale,
                      float2 rotation,
                      const int input_step_count,
                      const float4 input_step_0_a,
                      const float4 input_step_0_b,
                      const float4 input_step_1_a,
                      const float4 input_step_1_b,
                      const int input_step_flags)
{
  float2 p = uv;
  /* The transform effects' steps move the map coordinate (after the tiling, before the UV adjust),
   * as `composite_resample_projected` does. A step that clips leaves the tap transparent outside
   * the unit square of that coordinate. A Cube tap runs them in the local [0, 1] space of its own
   * atlas cell and returns to the atlas before the UV adjust, as the CPU does. The fold's side
   * signs are kept per step for the tangent-Normal covariant remap below. */
  float2 step_side[COMBINED_INPUT_STEP_MAX];
  {
    bool cube_local = is_cube && input_step_count > 0;
    if (cube_local) {
      p = (uv - cube_origin) / cube_cell;
    }
    for (int s = 0; s < COMBINED_INPUT_STEP_MAX && s < input_step_count; s++) {
      float4 a = (s == 0) ? input_step_0_a : input_step_1_a;
      float4 b = (s == 0) ? input_step_0_b : input_step_1_b;
      int step_flags = (input_step_flags >> (s * COMBINED_INPUT_STEP_BITS)) &
                       COMBINED_INPUT_STEP_MASK;
      if (!combined_input_step_apply(step_flags, a, b, p, step_side[s])) {
        return float4(0.0f);
      }
    }
    if (cube_local) {
      p = cube_origin + p * cube_cell;
    }
  }
  if ((pflags & COMBINED_PROJ_UV_ADJUST) != 0) {
    float ru = p.x * rotation.x - p.y * rotation.y;
    float rv = p.x * rotation.y + p.y * rotation.x;
    p = float2(ru, rv) * scale + offset;
  }
  bool repeat = !is_cube && (pflags & COMBINED_PROJ_WRAP_CLIP) == 0;
  if (repeat) {
    p -= floor(p);
  }
  float4 color = float4(0.0f);
  if (repeat || (p.x >= 0.0f && p.x < 1.0f && p.y >= 0.0f && p.y < 1.0f)) {
    color = textureLod(tx, p, 0.0f);
    if ((layer_flags & COMBINED_LAYER_PREMULTIPLIED) != 0 && color.w > 0.0f) {
      color.xyz = color.xyz / color.w;
    }
    if ((layer_flags & COMBINED_LAYER_TANGENT_NORMAL) != 0 && input_step_count > 0 &&
        color.w > 0.0f)
    {
      /* The covariant counterpart of the steps, per tap (Box blends taps whose fold sides differ):
       * the steps' inverses in reverse list order, `side * R^T * S^-1` each, as
       * `CompositeMappingChain::remap_tangent_normal` does. It runs before the frame-to-tangent
       * conversion of #projection_normal_to_tangent, the order the CPU composes them in. */
      float2 v = color.xy * 2.0f - 1.0f;
      for (int s = COMBINED_INPUT_STEP_MAX - 1; s >= 0; s--) {
        if (s >= input_step_count) {
          continue;
        }
        float4 a = (s == 0) ? input_step_0_a : input_step_1_a;
        float4 b = (s == 0) ? input_step_0_b : input_step_1_b;
        float rx = (a.z < 0.0f) ? -1.0f : 1.0f;
        float ry = (a.w < 0.0f) ? -1.0f : 1.0f;
        float fx = rx * v.x;
        float fy = ry * v.y;
        v = float2(step_side[s].x * (b.x * fx + b.y * fy), step_side[s].y * (-b.y * fx + b.x * fy));
      }
      color.xy = v * 0.5f + 0.5f;
    }
  }
  return color;
}

/**
 * The frame-to-tangent conversion of a projected tangent-space Normal row, the port of the tail of
 * #composite_resample_projected's texel callback (#BKE_paint_layers_projection_normal_frame_to_tangent).
 * \a mapped is #projection_sample_map's result (the straight colour weighted by the frame factor,
 * and the alpha sum); the vectors it holds read in the frame's own basis and leave in the tangent
 * basis of the triangle that owns the canvas \a uv. The basis is the triangle's UV gradient, read
 * from the table `proj_tri_tx` by the id in the normal map's `w`, both moved to the controller's
 * space as the CPU moves them. A texel without a basis (a degenerate UV triangle, an id the table
 * does not hold) keeps the unconverted vector, as the CPU keeps it.
 */
float4 projection_normal_to_tangent(float4 mapped,
                                    sampler2D pos_tx,
                                    sampler2D nrm_tx,
                                    sampler2D tri_tx,
                                    sampler2D decal_tx,
                                    int pflags,
                                    float2 uv,
                                    int2 canvas_size,
                                    float4x4 frame_rot,
                                    float4x4 n_to_controller)
{
  if (mapped.w <= 0.0f) {
    return mapped;
  }
  int2 surface_texel = clamp(int2(floor(uv * float2(canvas_size))), int2(0), canvas_size - int2(1));
  float4 surface_n = texelFetch(nrm_tx, surface_texel, 0);
  int tri_id = int(surface_n.w + 0.5f);
  int2 table_size = textureSize(tri_tx, 0);
  int basis_index = tri_id * 2;
  int2 basis_texel = int2(basis_index % max(table_size.x, 1), basis_index / max(table_size.x, 1));
  if (tri_id <= 0 || table_size.x < 2 || basis_texel.y >= table_size.y) {
    return mapped;
  }
  float4 tangent_texel = texelFetch(tri_tx, basis_texel, 0);
  if (tangent_texel.w < 0.5f) {
    return mapped;
  }
  float3 tangent = (n_to_controller * float4(tangent_texel.xyz, 0.0f)).xyz;
  float3 bitangent =
      (n_to_controller * float4(texelFetch(tri_tx, basis_texel + int2(1, 0), 0).xyz, 0.0f)).xyz;
  /* Unit directions, as the CPU normalizes them after the controller's normal matrix. */
  tangent = normalize(tangent);
  bitangent = normalize(bitangent);
  float3 surface_normal = (n_to_controller * float4(surface_n.xyz, 0.0f)).xyz;
  float surface_normal_length = length(surface_normal);
  surface_normal = (surface_normal_length > 0.0f) ? surface_normal / surface_normal_length :
                                                    float3(0.0f);
  /* The accumulated colour is the weighted mean of the encoded taps: decode it, carry it through
   * the frame's basis exactly as the CPU helper indexes it, and encode it in the triangle's basis,
   * renormalized. */
  float3 n_frame = mapped.xyz * (1.0f / mapped.w) * 2.0f - 1.0f;
  float3 n_world = (frame_rot * float4(n_frame, 0.0f)).xyz;
  if ((pflags & COMBINED_PROJ_DECAL) != 0) {
    /* The frame turns with the unfold: the axes of the patch triangle this texel was laid on, the
     * sum of the axes weighted by the vector's components (`decal_frame->axes` on the CPU). The
     * table holds three texels per triangle id, `textureSize` texels a row. */
    int decal_tri = int(texelFetch(pos_tx, surface_texel, 0).z + 0.5f);
    int2 frames_size = textureSize(decal_tx, 0);
    int frame_index = decal_tri * 3;
    int2 frame_texel = int2(frame_index % max(frames_size.x, 1), frame_index / max(frames_size.x, 1));
    if (frame_texel.y >= frames_size.y) {
      return mapped;
    }
    float3 axis_0 = texelFetch(decal_tx, frame_texel, 0).xyz;
    float3 axis_1 = texelFetch(decal_tx, frame_texel + int2(1, 0), 0).xyz;
    float3 axis_2 = texelFetch(decal_tx, frame_texel + int2(2, 0), 0).xyz;
    n_world = axis_0 * n_frame.x + axis_1 * n_frame.y + axis_2 * n_frame.z;
  }
  float3 encoded = float3(dot(n_world, tangent), dot(n_world, bitangent), dot(n_world, surface_normal));
  float encoded_length = length(encoded);
  if (!(encoded_length > 0.0f)) {
    return mapped;
  }
  encoded = encoded / encoded_length;
  return float4((encoded * 0.5f + 0.5f) * mapped.w, mapped.w);
}

/**
 * The row's map as #composite_resample_projected bakes it, at the canvas \a uv: the texel's surface
 * point through the frame, one tap (three for Box and Triplanar) weighted by the frame factor. A
 * texel no triangle owns stays transparent, like the texel the rasterizer leaves untouched. Shared
 * by the mask, layer and correction passes, so the three read a projected map alike. The result is
 * straight colour and alpha, black where nothing is covered.
 */
float4 projection_sample_map(sampler2D tx,
                             sampler2D pos_tx,
                             sampler2D nrm_tx,
                             float2 uv,
                             int2 canvas_size,
                             float4x4 p_to_q,
                             float4x4 n_to_frame,
                             float4 params_0,
                             float4 params_1,
                             float4 atlas_0,
                             float4 atlas_1,
                             float4 atlas_2,
                             float2 atlas_cell,
                             int mode,
                             int axis,
                             int pflags,
                             int layer_flags,
                             float2 offset,
                             float2 scale,
                             float2 rotation,
                             const int input_step_count,
                             const float4 input_step_0_a,
                             const float4 input_step_0_b,
                             const float4 input_step_1_a,
                             const float4 input_step_1_b,
                             const int input_step_flags)
{
  float4 result = float4(0.0f);
  int2 surface_texel = clamp(int2(floor(uv * float2(canvas_size))), int2(0), canvas_size - int2(1));
  float4 surface_p = texelFetch(pos_tx, surface_texel, 0);
  if (surface_p.w <= 0.0f) {
    return result;
  }
  float4 surface_n = texelFetch(nrm_tx, surface_texel, 0);
  float3 q = (p_to_q * float4(surface_p.xyz, 1.0f)).xyz;
  float3 n_frame = (n_to_frame * float4(surface_n.xyz, 0.0f)).xyz;
  float weight = projection_frame_weight(
      mode, axis, pflags, q, n_frame, params_0.zw, params_1.y, params_1.z);
  if (weight <= 0.0f) {
    return result;
  }
  float3 straight = float3(0.0f);
  float alpha = 0.0f;
  if (mode == COMBINED_PROJ_BOX) {
    /* The three face reads blended by the frame normal's powers, normalized. */
    float3 powers = float3(pow(abs(n_frame.x), params_1.x),
                           pow(abs(n_frame.y), params_1.x),
                           pow(abs(n_frame.z), params_1.x));
    float sum = max(powers.x + powers.y + powers.z, COMBINED_PROJ_EPSILON);
    for (int face = 0; face < 3; face++) {
      float2 face_uv = projection_face_uv(face, q);
      float2 tap_uv = float2(0.5f) + (face_uv - float2(0.5f)) * params_0.xy;
      float4 tap = projection_tap(tx,
                                  tap_uv,
                                  layer_flags,
                                  pflags,
                                  false,
                                  float2(0.0f),
                                  float2(1.0f),
                                  offset,
                                  scale,
                                  rotation,
                                  input_step_count,
                                  input_step_0_a,
                                  input_step_0_b,
                                  input_step_1_a,
                                  input_step_1_b,
                                  input_step_flags);
      float tap_weight = weight * powers[face] / sum;
      straight += tap.xyz * tap_weight;
      alpha += tap.w * tap_weight;
    }
  }
  else {
    float2 tap_uv;
    float2 cube_origin = float2(0.0f);
    if (mode == COMBINED_PROJ_CUBE) {
      float4 cube_tap = projection_cube_uv(q, atlas_0, atlas_1, atlas_2, atlas_cell);
      tap_uv = cube_tap.xy;
      cube_origin = cube_tap.zw;
    }
    else if (mode == COMBINED_PROJ_CYLINDER) {
      tap_uv = projection_cylinder_uv(q, axis);
    }
    else if (mode == COMBINED_PROJ_SPHERE) {
      tap_uv = projection_sphere_uv(q, axis);
    }
    else {
      /* Planar: X reads Y/Z, Y reads X/Z and Z reads X/Y. */
      int u_index = (axis == 0) ? 1 : 0;
      int v_index = (axis == 2) ? 1 : 2;
      tap_uv = float2(0.5f + q[u_index] * params_0.x * 0.5f, 0.5f + q[v_index] * params_0.y * 0.5f);
    }
    float4 tap = projection_tap(tx,
                                tap_uv,
                                layer_flags,
                                pflags,
                                mode == COMBINED_PROJ_CUBE,
                                cube_origin,
                                atlas_cell,
                                offset,
                                scale,
                                rotation,
                                input_step_count,
                                input_step_0_a,
                                input_step_0_b,
                                input_step_1_a,
                                input_step_1_b,
                                input_step_flags);
    straight += tap.xyz * weight;
    alpha += tap.w * weight;
  }
  /* The baked buffer is premultiplied and read back straightened: black where nothing is covered. */
  result = float4((alpha > 0.0f) ? straight : float3(0.0f), alpha);
  return result;
}

/** \}*/

/* -------------------------------------------------------------------- */
/** \name Mask Pass
 *
 * A row's mask factor `M` is built in an accumulator of its own, one pass per step, and the layer
 * pass then scales the row's coverage by it (`composite_layer_render`). The steps are: seed `M` with
 * one; lay each mask item over it (`M = blend(M, C, A * opacity)` for a map, `M = lerp(M, f(M),
 * opacity)` for a filter); multiply by the row's own mask map.
 * \{ */

#define COMBINED_MASK_INIT 0
#define COMBINED_MASK_ITEM 1
#define COMBINED_MASK_MAP 2
/* A map-driven coverage item (`BKE_paint_material_coverage_match`) is several passes: the match so
 * far is kept in the `y` of the accumulator, `x` being the factor. BEGIN zeroes it; each Color ID
 * colour (COLOR) or the UV Island scalar (SCALAR) raises it; END inverts, scales and lays it. */
#define COMBINED_MASK_COV_BEGIN 3
#define COMBINED_MASK_COV_COLOR 4
#define COMBINED_MASK_COV_SCALAR 5
#define COMBINED_MASK_COV_END 6
/* Write the factor the mask build just produced into a screen-space slot, grey in rgb and one in
 * alpha, for a mask Anchor Point slice. */
#define COMBINED_MASK_SLOT_STORE 7

#define COMBINED_MASK_HAS_MAP 2
#define COMBINED_MASK_PREMULTIPLIED 8
#define COMBINED_MASK_MAPPED 32
#define COMBINED_MASK_REPEAT_U 64
#define COMBINED_MASK_REPEAT_V 128
#define COMBINED_MASK_INVERT 512
#define COMBINED_MASK_FROM_ALPHA 1024
/* The item reads an Anchor Point mask slice from a screen-space slot. */
#define COMBINED_MASK_SLOT 2048

struct MaskResources {
  [[sampler(0)]] sampler2D mask_tx;
  /** A Color Ramp or RGB Curves table, or the gather's placeholder for other filters. */
  [[sampler(1)]] sampler2D filter_lut_tx;
  /** The painted surface's position and normal per canvas texel, for #COMBINED_LAYER_PROJECTED. */
  [[sampler(2)]] sampler2D proj_pos_tx;
  [[sampler(3)]] sampler2D proj_nrm_tx;
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D mask_img;
  /** The destination of #COMBINED_MASK_SLOT_STORE. */
  [[image(1, write, SFLOAT_16_16_16_16)]] image2D slot_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  [[push_constant]] const float2 mapping_offset;
  [[push_constant]] const float2 mapping_scale;
  [[push_constant]] const float2 mapping_rotation;
  /** The UV point the row's Mirror fold reflects around, on its folded axes. */
  [[push_constant]] const float2 mapping_mirror_center;
  /** The projection frame of #projection_sample_map, read when #COMBINED_LAYER_PROJECTED is set. */
  [[push_constant]] const float4x4 proj_p_to_q;
  [[push_constant]] const float4x4 proj_n_to_frame;
  [[push_constant]] const float4 proj_params_0;
  [[push_constant]] const float4 proj_params_1;
  [[push_constant]] const float4 proj_atlas_0;
  [[push_constant]] const float4 proj_atlas_1;
  [[push_constant]] const float4 proj_atlas_2;
  [[push_constant]] const float2 proj_atlas_cell;
  [[push_constant]] const int2 proj_canvas_size;
  [[push_constant]] const int proj_mode;
  [[push_constant]] const int proj_axis;
  [[push_constant]] const int proj_flags;
  /** The transform effects' steps ahead of the row's own mapping: `a` carries offset.xy and
   * scale.xy, `b` the rotation's cosine and sine and the fold center; seven flag bits a step live
   * in `input_step_flags`, and `input_step_count` says how many run. */
  [[push_constant]] const float4 input_step_0_a;
  [[push_constant]] const float4 input_step_0_b;
  [[push_constant]] const float4 input_step_1_a;
  [[push_constant]] const float4 input_step_1_b;
  [[push_constant]] const int input_step_flags;
  [[push_constant]] const int input_step_count;
  /** The filter's parameters in its schema order. */
  [[push_constant]] const float4 filter_params_0;
  [[push_constant]] const float4 filter_params_1;
  /** A Color Balance's exponent triple; unused by every other filter. */
  [[push_constant]] const float4 filter_params_2;
  /** The grey of an item that has no map. */
  [[push_constant]] const float constant_gray;
  [[push_constant]] const float opacity;
  /** The contrast the item's coverage is reshaped around its half with (one keeps it). */
  [[push_constant]] const float contrast;
  /** How much of the row's own mask map applies; one for an item. */
  [[push_constant]] const float influence;
  [[push_constant]] const float lod;
  [[push_constant]] const int flags;
  /** A `COMBINED_MASK_*` step. */
  [[push_constant]] const int mode;
  /** A `COMBINED_BLEND_*` value: the item's blend. */
  [[push_constant]] const int blend;
  /** A `COMBINED_FILTER_*` value; none for a map. */
  [[push_constant]] const int filter_kind;
};

[[compute, local_size(16, 16)]]
void mask_comp([[resource_table]] MaskResources &srt,
               [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (any(greaterThanEqual(texel, srt.texture_size))) {
    return;
  }
  if (srt.mode == COMBINED_MASK_INIT) {
    imageStore(srt.mask_img, texel, float4(1.0f));
    return;
  }
  if (srt.mode == COMBINED_MASK_SLOT_STORE) {
    /* The factor the build just left, duplicated into rgb with an opaque alpha, so the item that
     * reads the slot sees the same grey a rendered mask slice carries. */
    float m = clamp(imageLoad(srt.mask_img, texel).x, 0.0f, 1.0f);
    imageStore(srt.slot_img, texel, float4(m, m, m, 1.0f));
    return;
  }

  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  float m = imageLoad(srt.mask_img, texel).x;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    /* Outside the canvas the layer pass writes nothing, so the factor is not read there. */
    imageStore(srt.mask_img, texel, float4(m));
    return;
  }

  if (srt.mode >= COMBINED_MASK_COV_BEGIN) {
    float match = imageLoad(srt.mask_img, texel).y;
    if (srt.mode == COMBINED_MASK_COV_BEGIN) {
      match = 0.0f;
    }
    else if (srt.mode == COMBINED_MASK_COV_END) {
      if ((srt.flags & COMBINED_MASK_INVERT) != 0) {
        match = 1.0f - match;
      }
      /* White and Black carry one; Gradient carries the item's standard value. */
      match *= srt.influence;
      /* The item's own contrast, the formula #BKE_paint_layers_mask_contrast_apply implements. */
      match = clamp((match - 0.5f) * srt.contrast + 0.5f, 0.0f, 1.0f);
      m *= 1.0f + (match - 1.0f) * clamp(srt.opacity, 0.0f, 1.0f);
    }
    else {
      float4 c;
      if ((srt.flags & COMBINED_LAYER_PROJECTED) != 0) {
        c = projection_sample_map(srt.mask_tx,
                                srt.proj_pos_tx,
                                srt.proj_nrm_tx,
                                uv,
                                srt.proj_canvas_size,
                                srt.proj_p_to_q,
                                srt.proj_n_to_frame,
                                srt.proj_params_0,
                                srt.proj_params_1,
                                srt.proj_atlas_0,
                                srt.proj_atlas_1,
                                srt.proj_atlas_2,
                                srt.proj_atlas_cell,
                                srt.proj_mode,
                                srt.proj_axis,
                                srt.proj_flags,
                                srt.flags,
                                srt.mapping_offset,
                                srt.mapping_scale,
                                srt.mapping_rotation,
                                srt.input_step_count,
                                srt.input_step_0_a,
                                srt.input_step_0_b,
                                srt.input_step_1_a,
                                srt.input_step_1_b,
                                srt.input_step_flags);
      }
      else {
        c = sample_map(srt.mask_tx,
                       uv,
                       srt.flags,
                       srt.mapping_offset,
                       srt.mapping_scale,
                       srt.mapping_rotation,
                       srt.mapping_mirror_center,
                       srt.input_step_count,
                       srt.input_step_0_a,
                       srt.input_step_0_b,
                       srt.input_step_1_a,
                       srt.input_step_1_b,
                       srt.input_step_flags,
                       srt.lod);
      }
      if (srt.mode == COMBINED_MASK_COV_SCALAR) {
        match = clamp(c.x, 0.0f, 1.0f);
      }
      else {
        /* The colour in `filter_params_0.xyz`, its tolerance in `.w` and the falloff width in
         * `filter_params_1.x`: the max-channel distance, `1 - smoothstep(tol, tol + soft, dist)`
         * with the CPU's own edge cases (a hard edge when the two are equal). */
        float3 delta = abs(c.xyz - srt.filter_params_0.xyz);
        float dist = max(max(delta.x, delta.y), delta.z);
        float edge0 = srt.filter_params_0.w;
        float edge1 = edge0 + srt.filter_params_1.x;
        float s;
        if (dist < edge0) {
          s = 0.0f;
        }
        else if (dist >= edge1) {
          s = 1.0f;
        }
        else {
          float t = (dist - edge0) / (edge1 - edge0);
          s = (3.0f - 2.0f * t) * (t * t);
        }
        match = max(match, 1.0f - s);
      }
    }
    imageStore(srt.mask_img, texel, float4(m, match, 0.0f, 0.0f));
    return;
  }

  if (srt.mode == COMBINED_MASK_ITEM) {
    float opacity = clamp(srt.opacity, 0.0f, 1.0f);
    if (srt.filter_kind != COMBINED_FILTER_NONE) {
      /* A filter works on the factor so far as a grey. The Color Ramp's strength multiplier is
       * ignored, as the CPU's mask path ignores it: a grey coefficient has no stop alpha. */
      float grey = apply_filter(srt.filter_kind,
                                texel,
                                float3(m),
                                srt.filter_params_0,
                                srt.filter_params_1,
                                srt.filter_params_2,
                                srt.filter_lut_tx)
                       .x;
      m += (grey - m) * opacity;
      /* The filter's result is the coverage the item leaves; the contrast reshapes it like any
       * item's. */
      m = clamp((m - 0.5f) * srt.contrast + 0.5f, 0.0f, 1.0f);
    }
    else {
      float gray = clamp(srt.constant_gray, 0.0f, 1.0f);
      float weight = opacity;
      if ((srt.flags & COMBINED_MASK_SLOT) != 0) {
        /* An Anchor Point mask slice is screen-space and already scene linear: its factor is the
         * slot's red channel, opaque as every mask map. */
        gray = clamp(texelFetch(srt.mask_tx, texel, 0).x, 0.0f, 1.0f);
      }
      else if ((srt.flags & COMBINED_MASK_HAS_MAP) != 0) {
        float4 c;
        if ((srt.flags & COMBINED_LAYER_PROJECTED) != 0) {
          c = projection_sample_map(srt.mask_tx,
                                  srt.proj_pos_tx,
                                  srt.proj_nrm_tx,
                                  uv,
                                  srt.proj_canvas_size,
                                  srt.proj_p_to_q,
                                  srt.proj_n_to_frame,
                                  srt.proj_params_0,
                                  srt.proj_params_1,
                                  srt.proj_atlas_0,
                                  srt.proj_atlas_1,
                                  srt.proj_atlas_2,
                                  srt.proj_atlas_cell,
                                  srt.proj_mode,
                                  srt.proj_axis,
                                  srt.proj_flags,
                                  srt.flags,
                                  srt.mapping_offset,
                                  srt.mapping_scale,
                                  srt.mapping_rotation,
                                  srt.input_step_count,
                                  srt.input_step_0_a,
                                  srt.input_step_0_b,
                                  srt.input_step_1_a,
                                  srt.input_step_1_b,
                                  srt.input_step_flags);
        }
        else {
          c = sample_map(srt.mask_tx,
                         uv,
                         srt.flags,
                         srt.mapping_offset,
                         srt.mapping_scale,
                         srt.mapping_rotation,
                         srt.mapping_mirror_center,
                         srt.input_step_count,
                         srt.input_step_0_a,
                         srt.input_step_0_b,
                         srt.input_step_1_a,
                         srt.input_step_1_b,
                         srt.input_step_flags,
                         srt.lod);
        }
        gray = clamp((c.x + c.y + c.z) / 3.0f, 0.0f, 1.0f);
        weight = clamp(c.w, 0.0f, 1.0f) * opacity;
      }
      /* The grey only: a transparent texel is still one the item does not cover. */
      if ((srt.flags & COMBINED_MASK_INVERT) != 0) {
        gray = 1.0f - gray;
      }
      /* The item's own contrast, the formula #BKE_paint_layers_mask_contrast_apply implements. */
      gray = clamp((gray - 0.5f) * srt.contrast + 0.5f, 0.0f, 1.0f);
      float fac = clamp(weight, 0.0f, 1.0f);
      if (srt.blend == COMBINED_BLEND_MIX) {
        m = mix(m, gray, fac);
      }
      else {
        m = blend_rgb(srt.blend, float3(m), float3(gray), fac).x;
      }
    }
  }
  else {
    /* The row's own mask map. */
    float4 c;
    if ((srt.flags & COMBINED_LAYER_PROJECTED) != 0) {
      c = projection_sample_map(srt.mask_tx,
                              srt.proj_pos_tx,
                              srt.proj_nrm_tx,
                              uv,
                              srt.proj_canvas_size,
                              srt.proj_p_to_q,
                              srt.proj_n_to_frame,
                              srt.proj_params_0,
                              srt.proj_params_1,
                              srt.proj_atlas_0,
                              srt.proj_atlas_1,
                              srt.proj_atlas_2,
                              srt.proj_atlas_cell,
                              srt.proj_mode,
                              srt.proj_axis,
                              srt.proj_flags,
                              srt.flags,
                              srt.mapping_offset,
                              srt.mapping_scale,
                              srt.mapping_rotation,
                              srt.input_step_count,
                              srt.input_step_0_a,
                              srt.input_step_0_b,
                              srt.input_step_1_a,
                              srt.input_step_1_b,
                              srt.input_step_flags);
    }
    else {
      c = sample_map(srt.mask_tx,
                     uv,
                     srt.flags,
                     srt.mapping_offset,
                     srt.mapping_scale,
                     srt.mapping_rotation,
                     srt.mapping_mirror_center,
                     srt.input_step_count,
                     srt.input_step_0_a,
                     srt.input_step_0_b,
                     srt.input_step_1_a,
                     srt.input_step_1_b,
                     srt.input_step_flags,
                     srt.lod);
    }
    float value = ((srt.flags & COMBINED_MASK_FROM_ALPHA) != 0) ? c.w : (c.x + c.y + c.z) / 3.0f;
    m *= (1.0f - srt.influence) + srt.influence * clamp(value, 0.0f, 1.0f);
  }
  imageStore(srt.mask_img, texel, float4(m));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layer Pass
 * \{ */

struct LayerResources {
  [[sampler(0)]] sampler2D layer_tx;
  /** The row's mask factor in its `x`, read when #COMBINED_LAYER_HAS_MASK is set. */
  [[sampler(1)]] sampler2D mask_tx;
  /** The coverage of an Anchor Point slice in its `x`, read when #COMBINED_LAYER_SLOT is set. */
  [[sampler(2)]] sampler2D slot_cov_tx;
  /** A Color Ramp or RGB Curves table, or the gather's placeholder for other filters. */
  [[sampler(3)]] sampler2D filter_lut_tx;
  /** The painted surface's object-space position (`w` = 1 where covered) and normal per canvas
   * texel, read with texelFetch when #COMBINED_LAYER_PROJECTED is set. */
  [[sampler(4)]] sampler2D proj_pos_tx;
  [[sampler(5)]] sampler2D proj_nrm_tx;
  /** The tangent basis per triangle id (the `w` of `proj_nrm_tx`), two texels each, read when a
   * projected tangent-space Normal row converts its vectors to the surface's tangent basis. */
  [[sampler(6)]] sampler2D proj_tri_tx;
  /** A Decal frame's axes per patch triangle (three texels each), read when #COMBINED_PROJ_DECAL
   * is set in `proj_flags` and the row is a tangent-space Normal. */
  [[sampler(7)]] sampler2D proj_decal_tx;
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D accum_img;
  /** The row's colour and coverage while corrections are laid over it. */
  [[image(1, read_write, SFLOAT_16_16_16_16)]] image2D row_color_img;
  [[image(2, read_write, SFLOAT_16_16_16_16)]] image2D row_factor_img;
  /** Output texel centre to canvas UV. An affine map, carried as a mat4 for the push constant. */
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  /**
   * The projection frame, for #COMBINED_LAYER_PROJECTED. `proj_p_to_q` takes an object-space point
   * (through the controller when there is one) to the frame-space `q`, the aspect fit included;
   * `proj_n_to_frame` takes the surface normal to the frame's axes.
   */
  [[push_constant]] const float4x4 proj_p_to_q;
  [[push_constant]] const float4x4 proj_n_to_frame;
  /** The frame's rotation alone and the object-to-controller normal matrix, the two factors of
   * `proj_n_to_frame` the tangent-space Normal conversion uses apart. */
  [[push_constant]] const float4x4 proj_frame_rot;
  [[push_constant]] const float4x4 proj_n_to_controller;
  /** The tiling in `xy`, the facing scale and offset in `zw`. */
  [[push_constant]] const float4 proj_params_0;
  /** The Box and Triplanar face exponent, the frame extent and the edge softness. */
  [[push_constant]] const float4 proj_params_1;
  /** The Cube atlas: the origins of +X, -X, +Y, -Y, +Z, -Z in pairs, and the cell size. */
  [[push_constant]] const float4 proj_atlas_0;
  [[push_constant]] const float4 proj_atlas_1;
  [[push_constant]] const float4 proj_atlas_2;
  [[push_constant]] const float2 proj_atlas_cell;
  /** The size of the surface map, in texels. */
  [[push_constant]] const int2 proj_canvas_size;
  /** A `COMBINED_PROJ_*` mode, the projection axis and the `COMBINED_PROJ_*` flag bits. */
  [[push_constant]] const int proj_mode;
  [[push_constant]] const int proj_axis;
  [[push_constant]] const int proj_flags;
  /** The row's flat colour while it has no map. */
  [[push_constant]] const float4 layer_color;
  /** The value the accumulator is seeded with by the pass that carries #COMBINED_LAYER_INIT. */
  [[push_constant]] const float4 init_color;
  [[push_constant]] const float2 mapping_offset;
  [[push_constant]] const float2 mapping_scale;
  /** Cosine and sine of the mapping rotation. */
  [[push_constant]] const float2 mapping_rotation;
  /** The UV point the row's Mirror fold reflects around, on its folded axes. */
  [[push_constant]] const float2 mapping_mirror_center;
  /** The transform effects' steps ahead of the row's own mapping: `a` carries offset.xy and
   * scale.xy, `b` the rotation's cosine and sine and the fold center; seven flag bits a step live
   * in `input_step_flags`, and `input_step_count` says how many run. */
  [[push_constant]] const float4 input_step_0_a;
  [[push_constant]] const float4 input_step_0_b;
  [[push_constant]] const float4 input_step_1_a;
  [[push_constant]] const float4 input_step_1_b;
  [[push_constant]] const int input_step_flags;
  [[push_constant]] const int input_step_count;
  /** The adjustment's filter and its parameters in the schema order. */
  [[push_constant]] const float4 filter_params_0;
  [[push_constant]] const float4 filter_params_1;
  /** A Color Balance's exponent triple; unused by every other filter. */
  [[push_constant]] const float4 filter_params_2;
  /**
   * The scene luminance an F0 map encoding reads a specular colour by: the RGB the scene
   * coefficients (the ones the CPU helper and the RGB to BW nodes use), `w` the F0 a level of one
   * is pre-divided (`1 / 0.08`).
   */
  [[push_constant]] const float4 map_luma;
  [[push_constant]] const float opacity;
  [[push_constant]] const float lod;
  [[push_constant]] const int flags;
  /** A `COMBINED_BLEND_*` value. */
  [[push_constant]] const int blend;
  [[push_constant]] const int filter_kind;
};

[[compute, local_size(16, 16)]]
void layer_comp([[resource_table]] LayerResources &srt,
                [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (any(greaterThanEqual(texel, srt.texture_size))) {
    return;
  }

  /* A texel outside the canvas is transparent, like the texel IMB_transform leaves untouched. */
  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    imageStore(srt.accum_img, texel, float4(0.0f));
    return;
  }

  float4 dst = ((srt.flags & COMBINED_LAYER_INIT) != 0) ? srt.init_color :
                                                          imageLoad(srt.accum_img, texel);
  float4 top = srt.layer_color;
  float coverage = 1.0f;
  if ((srt.flags & COMBINED_LAYER_ROW_USE) != 0) {
    /* The corrections have already worked on the row's colour and coverage. */
    top = imageLoad(srt.row_color_img, texel);
    coverage = imageLoad(srt.row_factor_img, texel).x;
  }
  else {
    if ((srt.flags & COMBINED_LAYER_ADJUSTMENT) != 0) {
      /* The stack so far, filtered; the row then lays it by its own blend, opacity and mask. A
       * Color Ramp's stop alpha lowers the strength the way the CPU's adjustment path does. */
      top = dst;
      float4 filtered = apply_filter(srt.filter_kind,
                                     texel,
                                     dst.xyz,
                                     srt.filter_params_0,
                                     srt.filter_params_1,
                                     srt.filter_params_2,
                                     srt.filter_lut_tx);
      top.xyz = (filtered.w == 1.0f) ? filtered.xyz :
                                      dst.xyz + (filtered.xyz - dst.xyz) * filtered.w;
    }
    else if ((srt.flags & COMBINED_LAYER_SLOT) != 0) {
      /* An Anchor Point slice: screen-space colour and coverage, no mapping, no lod and no
       * premultiply. Its alpha is content alpha, the coverage is separate. */
      top = texelFetch(srt.layer_tx, texel, 0);
      coverage = clamp(texelFetch(srt.slot_cov_tx, texel, 0).x, 0.0f, 1.0f);
    }
    else if ((srt.flags & COMBINED_LAYER_HAS_MAP) != 0) {
      if ((srt.flags & COMBINED_LAYER_PROJECTED) != 0) {
        top = projection_sample_map(srt.layer_tx,
                                    srt.proj_pos_tx,
                                    srt.proj_nrm_tx,
                                    uv,
                                    srt.proj_canvas_size,
                                    srt.proj_p_to_q,
                                    srt.proj_n_to_frame,
                                    srt.proj_params_0,
                                    srt.proj_params_1,
                                    srt.proj_atlas_0,
                                    srt.proj_atlas_1,
                                    srt.proj_atlas_2,
                                    srt.proj_atlas_cell,
                                    srt.proj_mode,
                                    srt.proj_axis,
                                    srt.proj_flags,
                                    srt.flags,
                                    srt.mapping_offset,
                                    srt.mapping_scale,
                                    srt.mapping_rotation,
                                    srt.input_step_count,
                                    srt.input_step_0_a,
                                    srt.input_step_0_b,
                                    srt.input_step_1_a,
                                    srt.input_step_1_b,
                                    srt.input_step_flags);
        if ((srt.flags & COMBINED_LAYER_TANGENT_NORMAL) != 0) {
          /* After the per-tap remap of the steps and before the DirectX flip below, the order the
           * CPU bake and its decode compose them in. */
          top = projection_normal_to_tangent(top,
                                             srt.proj_pos_tx,
                                             srt.proj_nrm_tx,
                                             srt.proj_tri_tx,
                                             srt.proj_decal_tx,
                                             srt.proj_flags,
                                             uv,
                                             srt.proj_canvas_size,
                                             srt.proj_frame_rot,
                                             srt.proj_n_to_controller);
        }
      }
      else {
        top = sample_map(srt.layer_tx,
                         uv,
                         srt.flags,
                         srt.mapping_offset,
                         srt.mapping_scale,
                         srt.mapping_rotation,
                         srt.mapping_mirror_center,
                         srt.input_step_count,
                         srt.input_step_0_a,
                         srt.input_step_0_b,
                         srt.input_step_1_a,
                         srt.input_step_1_b,
                         srt.input_step_flags,
                         srt.lod);
      }
      if ((srt.flags & COMBINED_LAYER_NORMAL_DIRECTX) != 0) {
        top.y = 1.0f - top.y;
      }
      /* What the map stores, read as the channel's value where the texel enters the pass -- the
       * same place the CPU composite converts and the generated graph builds nodes, so the three
       * cannot disagree. The spec-gloss alpha packing reads `1 - a` and leaves the alpha one: it
       * is glossiness data, not the coverage, which stays the pass's `1 x mask x opacity`. */
      if ((srt.flags & COMBINED_LAYER_MAP_GLOSSINESS) != 0) {
        top.xyz = float3(1.0f) - top.xyz;
      }
      else if ((srt.flags & COMBINED_LAYER_MAP_GLOSSINESS_ALPHA) != 0) {
        const float roughness = 1.0f - clamp(top.w, 0.0f, 1.0f);
        top = float4(float3(roughness), 1.0f);
      }
      else if ((srt.flags & COMBINED_LAYER_MAP_F0_GLOSSINESS_ALPHA) != 0) {
        const float level = clamp(dot(top.xyz, srt.map_luma.xyz) * srt.map_luma.w, 0.0f, 1.0f);
        top = float4(float3(level), 1.0f);
      }
      else if ((srt.flags & COMBINED_LAYER_MAP_F0) != 0) {
        const float level = clamp(dot(top.xyz, srt.map_luma.xyz) * srt.map_luma.w, 0.0f, 1.0f);
        top = float4(float3(level), top.w);
      }
    }
    if ((srt.flags & (COMBINED_LAYER_SLOT | COMBINED_LAYER_ALPHA_COVERAGE)) ==
        COMBINED_LAYER_ALPHA_COVERAGE)
    {
      coverage = clamp(top.w, 0.0f, 1.0f);
    }
    if ((srt.flags & COMBINED_LAYER_EMPTY_BASE) != 0 &&
        (srt.flags & COMBINED_LAYER_SLOT) == 0)
    {
      coverage = 0.0f;
    }
    if ((srt.flags & COMBINED_LAYER_ROW_STORE) != 0) {
      imageStore(srt.row_color_img, texel, top);
      imageStore(srt.row_factor_img, texel, float4(coverage));
      return;
    }
  }
  if ((srt.flags & COMBINED_LAYER_HAS_MASK) != 0) {
    /* The row's mask factor, built by the mask passes. */
    coverage *= texelFetch(srt.mask_tx, texel, 0).x;
  }

  float fac = clamp(srt.opacity * coverage, 0.0f, 1.0f);
  if (srt.blend == COMBINED_BLEND_NORMAL_COMBINE) {
    dst.xyz = mix(dst.xyz, normal_combine(dst.xyz, top.xyz), fac);
  }
  else if (srt.blend == COMBINED_BLEND_MIX) {
    dst = mix(dst, top, fac);
  }
  else {
    dst.xyz = blend_rgb(srt.blend, dst.xyz, top.xyz, fac);
  }
  imageStore(srt.accum_img, texel, dst);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Correction Pass
 *
 * One content correction laid over a row's colour and coverage (`composite_layer_render`). With `a`
 * the coverage so far and `f = opacity * alpha(map)`:
 *
 *   c_eff = lerp(c, blend(S, c), a)   (where the row covers nothing, the correction is its colour)
 *   a'    = a + f * (1 - a)
 *   S'    = lerp(S, c_eff, f / a')
 *
 * which is the plain blend by `f` where `a` is one. A filter instead changes the colour by its own
 * opacity and leaves the coverage alone.
 * \{ */

struct CorrectionResources {
  [[sampler(0)]] sampler2D correction_tx;
  /** The coverage of an Anchor Point slice in its `x`, read when #COMBINED_LAYER_SLOT is set. */
  [[sampler(1)]] sampler2D slot_cov_tx;
  /** A Color Ramp or RGB Curves table, or the gather's placeholder for other filters. */
  [[sampler(2)]] sampler2D filter_lut_tx;
  /** The result of a GPU effect pass, at the effect input's resolution. */
  [[sampler(3)]] sampler2D effect_tx;
  /** The painted surface's position and normal per canvas texel, for #COMBINED_LAYER_PROJECTED. */
  [[sampler(4)]] sampler2D proj_pos_tx;
  [[sampler(5)]] sampler2D proj_nrm_tx;
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D row_color_img;
  [[image(1, read_write, SFLOAT_16_16_16_16)]] image2D row_factor_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  /** A Fill correction's flat colour while it has no map. */
  [[push_constant]] const float4 constant_color;
  [[push_constant]] const float2 mapping_offset;
  [[push_constant]] const float2 mapping_scale;
  [[push_constant]] const float2 mapping_rotation;
  /** The UV point the row's Mirror fold reflects around, on its folded axes. */
  [[push_constant]] const float2 mapping_mirror_center;
  /** The projection frame of #projection_sample_map, read when #COMBINED_LAYER_PROJECTED is set. */
  [[push_constant]] const float4x4 proj_p_to_q;
  [[push_constant]] const float4x4 proj_n_to_frame;
  [[push_constant]] const float4 proj_params_0;
  [[push_constant]] const float4 proj_params_1;
  [[push_constant]] const float4 proj_atlas_0;
  [[push_constant]] const float4 proj_atlas_1;
  [[push_constant]] const float4 proj_atlas_2;
  [[push_constant]] const float2 proj_atlas_cell;
  [[push_constant]] const int2 proj_canvas_size;
  [[push_constant]] const int proj_mode;
  [[push_constant]] const int proj_axis;
  [[push_constant]] const int proj_flags;
  /** The transform effects' steps ahead of the row's own mapping, as in #MaskResources. */
  [[push_constant]] const float4 input_step_0_a;
  [[push_constant]] const float4 input_step_0_b;
  [[push_constant]] const float4 input_step_1_a;
  [[push_constant]] const float4 input_step_1_b;
  [[push_constant]] const int input_step_flags;
  [[push_constant]] const int input_step_count;
  [[push_constant]] const float4 filter_params_0;
  [[push_constant]] const float4 filter_params_1;
  /** A Color Balance's exponent triple; unused by every other filter. */
  [[push_constant]] const float4 filter_params_2;
  [[push_constant]] const float opacity;
  [[push_constant]] const float lod;
  [[push_constant]] const int flags;
  [[push_constant]] const int blend;
  [[push_constant]] const int filter_kind;
};

[[compute, local_size(16, 16)]]
void correction_comp([[resource_table]] CorrectionResources &srt,
                     [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (any(greaterThanEqual(texel, srt.texture_size))) {
    return;
  }
  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    return;
  }

  float4 s = imageLoad(srt.row_color_img, texel);
  if ((srt.flags & COMBINED_LAYER_EFFECT_RESULT) != 0) {
    /* A CPU-computed effect the GPU computed: its result is mixed over the row's colour and nothing
     * else moves, exactly as the CPU evaluator applies a correction's `effect_color` -- the coverage
     * and the content alpha stay as they are, since the effect lays no coverage of its own. */
    const float weight = clamp(srt.opacity, 0.0f, 1.0f);
    if (weight > 0.0f) {
      float4 effect = textureLod(srt.effect_tx, uv, 0.0f);
      /* The result carries the content alpha it was computed on in its fourth component, and the CPU
       * reads a float canvas of the composite as premultiplied (`composite_decode_tile` straightens
       * it). The same division here is the same number the CPU mixes with. */
      if (effect.w > 0.0f) {
        effect.xyz = effect.xyz / effect.w;
      }
      s.xyz = s.xyz + (effect.xyz - s.xyz) * weight;
    }
    imageStore(srt.row_color_img, texel, s);
    return;
  }
  if (srt.filter_kind != COMBINED_FILTER_NONE) {
    float weight = clamp(srt.opacity, 0.0f, 1.0f);
    float4 filtered = apply_filter(srt.filter_kind,
                                   texel,
                                   s.xyz,
                                   srt.filter_params_0,
                                   srt.filter_params_1,
                                   srt.filter_params_2,
                                   srt.filter_lut_tx);
    s.xyz = s.xyz + (filtered.xyz - s.xyz) * weight * filtered.w;
    imageStore(srt.row_color_img, texel, s);
    return;
  }

  float a = clamp(imageLoad(srt.row_factor_img, texel).x, 0.0f, 1.0f);
  bool slot = (srt.flags & COMBINED_LAYER_SLOT) != 0;
  bool has_map = (srt.flags & COMBINED_LAYER_HAS_MAP) != 0;
  float4 c = srt.constant_color;
  if (slot) {
    c = texelFetch(srt.correction_tx, texel, 0);
  }
  else if (has_map) {
    if ((srt.flags & COMBINED_LAYER_PROJECTED) != 0) {
      c = projection_sample_map(srt.correction_tx,
                              srt.proj_pos_tx,
                              srt.proj_nrm_tx,
                              uv,
                              srt.proj_canvas_size,
                              srt.proj_p_to_q,
                              srt.proj_n_to_frame,
                              srt.proj_params_0,
                              srt.proj_params_1,
                              srt.proj_atlas_0,
                              srt.proj_atlas_1,
                              srt.proj_atlas_2,
                              srt.proj_atlas_cell,
                              srt.proj_mode,
                              srt.proj_axis,
                              srt.proj_flags,
                              srt.flags,
                              srt.mapping_offset,
                              srt.mapping_scale,
                              srt.mapping_rotation,
                              srt.input_step_count,
                              srt.input_step_0_a,
                              srt.input_step_0_b,
                              srt.input_step_1_a,
                              srt.input_step_1_b,
                              srt.input_step_flags);
    }
    else {
      c = sample_map(srt.correction_tx,
                     uv,
                     srt.flags,
                     srt.mapping_offset,
                     srt.mapping_scale,
                     srt.mapping_rotation,
                     srt.mapping_mirror_center,
                     srt.input_step_count,
                     srt.input_step_0_a,
                     srt.input_step_0_b,
                     srt.input_step_1_a,
                     srt.input_step_1_b,
                     srt.input_step_flags,
                     srt.lod);
    }
  }
  /* A slice correction covers by the coverage its reader rendered with; its colour's alpha is
   * content alpha. A flat colour covers by its opacity alone. */
  float correction_factor = slot ? clamp(texelFetch(srt.slot_cov_tx, texel, 0).x, 0.0f, 1.0f) :
                          (has_map ? c.w : 1.0f);
  float f = clamp(srt.opacity * correction_factor, 0.0f, 1.0f);

  float4 blended = s;
  if (srt.blend == COMBINED_BLEND_NORMAL_COMBINE) {
    /* Where the row has no content its colour is the map's empty black, which decodes to a tangent
     * of `(-1, -1, -1)`: the flat normal stands in by the coverage, as in the graph. */
    blended.x = 0.5f + (s.x - 0.5f) * a;
    blended.y = 0.5f + (s.y - 0.5f) * a;
    blended.z = 1.0f + (s.z - 1.0f) * a;
    blended.xyz = normal_combine(blended.xyz, c.xyz);
  }
  else if (srt.blend == COMBINED_BLEND_MIX) {
    blended = c;
  }
  else {
    blended.xyz = blend_rgb(srt.blend, blended.xyz, c.xyz, 1.0f);
  }

  float a_new = a + f * (1.0f - a);
  float weight = (a_new > 0.0f) ? f / a_new : 0.0f;
  float3 c_eff = c.xyz + (blended.xyz - c.xyz) * a;
  s.xyz = s.xyz + (c_eff - s.xyz) * weight;
  /* The fourth component follows the over model on its own where it is a content alpha. */
  float alpha = clamp(s.w, 0.0f, 1.0f);
  s.w = ((srt.flags & COMBINED_LAYER_TRACKS_ALPHA) != 0) ? alpha + f * (1.0f - alpha) : a_new;
  imageStore(srt.row_color_img, texel, s);
  imageStore(srt.row_factor_img, texel, float4(a_new));
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Effect Pass
 *
 * One CPU-computed effect, computed here instead of in the effect cache: the pass reads the row's
 * content up to the effect (`input_tx`, at the input's own resolution and texel space, which the
 * gather lays with the passes above) and writes the result the correction pass mixes in. The kernels
 * are ports of `paint_layer_effect_kernel_apply` kept in its order of operations, so
 * `BLENDER_PBR_GPU_VERIFY` compares one number.
 *
 * An effect of several passes runs this entry point once per pass of its plan: the host pushes the
 * pass in `pass_kind`, binds the pass's own source in `input_tx` (a ping-pong buffer of its own for
 * the middle passes, the luminance buffer for a High Pass tail, or the original for a Sharpen tail)
 * and the combine tail's blurred copy in `aux_tx`, and clips the dispatch to the rectangle the pass
 * reads. The separable windows are a direct enumeration
 * of their clamped window from `input_tx`, matching the CPU kernel's arithmetic and its edge handling
 * for any radius -- including a High Pass reach of 256, past what a shared-memory tile of one workgroup
 * could hold. A shared-memory tile for the small radii (and a scan for the large ones) is the measured
 * optimization the next phase adds behind a radius threshold; the reduction of a Color Match is
 * another pass kind here.
 * \{ */

/* The families of the Noise, the value of its first parameter (`PaintLayerNoiseType`). */
#define COMBINED_NOISE_GAUSSIAN 0
#define COMBINED_NOISE_UNIFORM 1
#define COMBINED_NOISE_SPECKLE 2
#define COMBINED_NOISE_SALT_PEPPER 3

/** `paint_layer_noise_uniform`: one uniform of the family, three passes to a channel. */
float noise_uniform(int2 texel, int seed, int channel, int pass)
{
  return texel_hash_uniform(texel, seed, uint(channel * 3 + pass));
}

/** `paint_layer_noise_gaussian`: three uniforms sum to a bell whose spread the intensity sets. */
float noise_gaussian(int2 texel, int seed, int channel, float intensity)
{
  const float u1 = noise_uniform(texel, seed, channel, 1);
  const float u2 = noise_uniform(texel, seed, channel, 2);
  const float u3 = noise_uniform(texel, seed, channel, 3);
  return (u1 + u2 + u3 - 1.5f) * intensity * 2.0f;
}

/** The signed offset one channel reads: the gaussian family, or one uniform swung around zero. */
float noise_offset(int2 texel, int seed, int channel, int type, float intensity)
{
  if (type == COMBINED_NOISE_UNIFORM) {
    return (noise_uniform(texel, seed, channel, 1) * 2.0f - 1.0f) * intensity;
  }
  return noise_gaussian(texel, seed, channel, intensity);
}

/**
 * One channel of the grain, the way `paint_layer_effect_noise_apply` writes it: salt and pepper
 * thresholds the uniform, the speckle scales the colour, the rest add to it. The clip at one is the
 * kernel's, not the encode's: a Noise trims an HDR map where a Sharpen does not.
 *
 * The components are written out by name rather than indexed: not every backend overloads a dynamic
 * subscript of a vector the same way.
 */
float noise_channel_apply(float value, int2 texel, int seed, int channel, int type, float intensity)
{
  if (type == COMBINED_NOISE_SALT_PEPPER) {
    /* A salted channel reads black, a peppered one white, and the rest stay as they are: the amount
     * is the intensity, half to each side. */
    const float reach = intensity * 0.5f;
    const float u = noise_uniform(texel, seed, channel, 1);
    return (u < reach) ? 0.0f : ((u > 1.0f - reach) ? 1.0f : value);
  }
  const float n = noise_offset(texel, seed, channel, type, intensity);
  if (type == COMBINED_NOISE_SPECKLE) {
    return clamp(value * (1.0f + n), 0.0f, 1.0f);
  }
  return clamp(value + n, 0.0f, 1.0f);
}

/** The Noise kernel of one texel, in place over its three colour channels. */
void noise_apply(float3 &rgb, int2 texel, float4 params, float3 luma_coefficients)
{
  /* The schema order the gather packs: the family, the intensity, the monochrome switch, the seed.
   * The two integers are stored as floats, so they are read back rounded. */
  const int type = int(params.x + 0.5f);
  const float intensity = params.y;
  const bool monochrome = params.z > 0.5f;
  const int seed = int(params.w + 0.5f);

  if (type == COMBINED_NOISE_SALT_PEPPER || !monochrome) {
    /* Salt and pepper is per channel whatever the switch says: the kernel tests the family first. */
    rgb.x = noise_channel_apply(rgb.x, texel, seed, 0, type, intensity);
    rgb.y = noise_channel_apply(rgb.y, texel, seed, 1, type, intensity);
    rgb.z = noise_channel_apply(rgb.z, texel, seed, 2, type, intensity);
    return;
  }
  /* One noise per texel, laid through the luminance: the colour follows it, so the grain is the same
   * grey the eye reads. The weights are the working space's own, which the CPU kernel reads; the
   * Rec.709 triple the rest of this file uses would grain a different grey in another space. */
  const float lum = dot(rgb, luma_coefficients);
  const float n = noise_offset(texel, seed, 0, type, intensity);
  const float moved = (type == COMBINED_NOISE_SPECKLE) ? clamp(lum * (1.0f + n), 0.0f, 1.0f) :
                                                        clamp(lum + n, 0.0f, 1.0f);
  rgb = rgb + float3(moved - lum);
}

/**
 * The smooth value noise the Blur Slope reads its driver from (`paint_layer_effect_value_noise`):
 * the texel hash's lattice interpolated bilinearly about the smoothstep, so what it slopes is a
 * slope a gradient can read. The seed rides the hash, so a different seed is a different pattern of
 * the same kind; the salt is the same one the CPU's call passes.
 */
float slope_value_noise(const float x, const float y, const int seed)
{
  const int ix = int(floor(x));
  const int iy = int(floor(y));
  const float fx = x - float(ix);
  const float fy = y - float(iy);
  const float sx = fx * fx * (3.0f - 2.0f * fx);
  const float sy = fy * fy * (3.0f - 2.0f * fy);
  const float a = texel_hash_uniform(int2(ix, iy), seed, 48739u);
  const float b = texel_hash_uniform(int2(ix + 1, iy), seed, 48739u);
  const float c = texel_hash_uniform(int2(ix, iy + 1), seed, 48739u);
  const float d = texel_hash_uniform(int2(ix + 1, iy + 1), seed, 48739u);
  return (a * (1.0f - sx) + b * sx) * (1.0f - sy) + (c * (1.0f - sx) + d * sx) * sy;
}

/**
 * The pass kinds of the plan, the values of #PaintLayerEffectPass::kind in
 * `BKE_paint_layers_composite.hh`. The host runs one dispatch per pass and pushes its kind here; a
 * pointwise effect (a Noise) is one pass, a separable one (a Blur) is a pass per axis, a 3x3 kernel
 * (an Edge Detect, an Emboss) is one pass naming its kind, and an unsharp-style one (a Sharpen, a
 * High Pass) is a luminance pre-pass, a blur and a combine tail -- a Sharpen tail and a High Pass
 * tail are two kinds, the latter reading the luminance buffer through the sRGB curve.
 */
#define COMBINED_PASS_NOISE 0
#define COMBINED_PASS_LUMINANCE 1
#define COMBINED_PASS_BLUR_H 2
#define COMBINED_PASS_BLUR_V 3
#define COMBINED_PASS_EXTREMUM_H 4
#define COMBINED_PASS_EXTREMUM_V 5
#define COMBINED_PASS_EDGE_3X3 6
#define COMBINED_PASS_EMBOSS_3X3 7
#define COMBINED_PASS_UNSHARP_TAIL 8
#define COMBINED_PASS_HIGH_PASS_TAIL 9
#define COMBINED_PASS_BLUR_DIRECTIONAL 10
#define COMBINED_PASS_FXAA 11
#define COMBINED_PASS_CHOKE 12
#define COMBINED_PASS_SCREEN_TAIL 13
#define COMBINED_PASS_BLOCK_AVERAGE 14
#define COMBINED_PASS_HEIGHT_TO_NORMAL_3X3 15
#define COMBINED_PASS_SLOT_FETCH 16
#define COMBINED_PASS_JFA_INIT 17
#define COMBINED_PASS_JFA_STEP 18
#define COMBINED_PASS_DIST_EVAL 19
#define COMBINED_PASS_SILHOUETTE 20
#define COMBINED_PASS_OFFSET_TINT_TAIL 21
#define COMBINED_PASS_SLOPE_DRIVER 22
#define COMBINED_PASS_SLOPE_WALK 23
#define COMBINED_PASS_BAND_TINT_TAIL 24
#define COMBINED_PASS_REDUCE_START 25
#define COMBINED_PASS_REDUCE_STEP 26
#define COMBINED_PASS_MATCH_GRADE 27
#define COMBINED_PASS_FLOOD_INIT 28
#define COMBINED_PASS_FLOOD_STEP 29
#define COMBINED_PASS_DIFFUSE_STEP 30
#define COMBINED_PASS_FILL_TAIL 31

/** `rgb_to_hsl` of `BLI_math_color`, ported verbatim, the hue branch order and the exact max/min
 * comparisons included: the Color Match grades in HSL, so the shader and the CPU kernel must
 * convert alike. The lightness is clamped above at one and left as it is below, and an achromatic
 * colour reads zero hue and saturation. */
float3 effect_rgb_to_hsl(float3 rgb)
{
  const float cmax = max(rgb.x, max(rgb.y, rgb.z));
  const float cmin = min(rgb.x, min(rgb.y, rgb.z));
  float hue;
  float sat;
  const float light = min(1.0f, (cmax + cmin) / 2.0f);
  if (cmax == cmin) {
    hue = 0.0f;
    sat = 0.0f;
  }
  else {
    const float d = cmax - cmin;
    sat = (light > 0.5f) ? d / (2.0f - cmax - cmin) : d / (cmax + cmin);
    if (cmax == rgb.x) {
      hue = (rgb.y - rgb.z) / d + ((rgb.y < rgb.z) ? 6.0f : 0.0f);
    }
    else if (cmax == rgb.y) {
      hue = (rgb.z - rgb.x) / d + 2.0f;
    }
    else {
      hue = (rgb.x - rgb.y) / d + 4.0f;
    }
  }
  return float3(hue / 6.0f, sat, light);
}

/** `hsl_to_rgb` of `BLI_math_color`, ported verbatim: the same three triangle ramps the HSV
 * conversion uses, scaled by the chroma the lightness and saturation make and centered on the
 * lightness. */
float3 effect_hsl_to_rgb(float3 hsl)
{
  float nr = abs(hsl.x * 6.0f - 3.0f) - 1.0f;
  float ng = 2.0f - abs(hsl.x * 6.0f - 2.0f);
  float nb = 2.0f - abs(hsl.x * 6.0f - 4.0f);
  nr = clamp(nr, 0.0f, 1.0f);
  ng = clamp(ng, 0.0f, 1.0f);
  nb = clamp(nb, 0.0f, 1.0f);
  const float chroma = (1.0f - abs(2.0f * hsl.z - 1.0f)) * hsl.y;
  return float3((nr - 0.5f) * chroma + hsl.z,
                (ng - 0.5f) * chroma + hsl.z,
                (nb - 0.5f) * chroma + hsl.z);
}

/** One axis of a box pass over the RGB of the fetched line: window `[max(c-r,0), min(c+r,lim-1)]`,
 * divided by its actual texel count (`hi - lo + 1`) -- a clamped edge, not a replicated one, exactly
 * as `paint_layer_effect_blur_pass_h/v`. Alpha is not read here; the caller carries it through. */
float3 box_blur_axis(sampler2D tx, const int2 texel, const int axis, const int r, const int2 size)
{
  const int center = (axis == 0) ? texel.x : texel.y;
  const int line_at = (axis == 0) ? texel.y : texel.x;
  const int lim = (axis == 0) ? size.x : size.y;
  const int lo = max(center - r, 0);
  const int hi = min(center + r, lim - 1);
  float3 sum = float3(0.0f);
  for (int k = lo; k <= hi; k++) {
    const int2 at = (axis == 0) ? int2(k, line_at) : int2(line_at, k);
    sum += texelFetch(tx, at, 0).xyz;
  }
  return sum / float(hi - lo + 1);
}

/** One axis of a sliding-window extremum (an Erode / Dilate): the min or max of the same clamped
 * window, per channel. A min/max of the input is exactly reproducible, so this matches the CPU kernel
 * to the bit, with no running float to accumulate. */
float3 extremum_axis(
    sampler2D tx, const int2 texel, const int axis, const int r, const bool maximum, const int2 size)
{
  const int center = (axis == 0) ? texel.x : texel.y;
  const int line_at = (axis == 0) ? texel.y : texel.x;
  const int lim = (axis == 0) ? size.x : size.y;
  const int lo = max(center - r, 0);
  const int hi = min(center + r, lim - 1);
  const int2 at0 = (axis == 0) ? int2(lo, line_at) : int2(line_at, lo);
  float3 acc = texelFetch(tx, at0, 0).xyz;
  for (int k = lo + 1; k <= hi; k++) {
    const int2 at = (axis == 0) ? int2(k, line_at) : int2(line_at, k);
    const float3 v = texelFetch(tx, at, 0).xyz;
    acc = maximum ? max(acc, v) : min(acc, v);
  }
  return acc;
}

/** A texel of the input clamped to the canvas edge: the 3x3 reads its ring this way, as
 * `paint_layer_effect_kernel_apply` clamps its 3x3 row and column indices once per tap. */
float3 fetch_clamped(sampler2D tx, const int2 p, const int2 size)
{
  const int2 c = int2(clamp(p.x, 0, size.x - 1), clamp(p.y, 0, size.y - 1));
  return texelFetch(tx, c, 0).xyz;
}

/** One texel's height of a Height row, the `height_at` lambda of the Height To Normal kernel: the
 * red weighed by the alpha back to the neutral (#PAINT_HEIGHT_NEUTRAL, 0.5), so an uncovered texel
 * reads as the flat ground and a stroke's edge does not read as a cliff. */
float height_fetch(sampler2D tx, const int2 p, const int2 size)
{
  const int2 c = int2(clamp(p.x, 0, size.x - 1), clamp(p.y, 0, size.y - 1));
  const float4 t = texelFetch(tx, c, 0);
  return 0.5f + (t.x - 0.5f) * t.w;
}

/** A texel of the input read as zero past the canvas instead of clamped to its edge: the Drop
 * Shadow tail's silhouette read (`paint_layer_effect_drop_shadow_apply`), where a clamped edge
 * would smear the shape's border row across the whole shadow band. */
float4 fetch_zero_outside(sampler2D tx, const int2 p, const int2 size)
{
  if (p.x < 0 || p.x >= size.x || p.y < 0 || p.y >= size.y) {
    return float4(0.0f);
  }
  return texelFetch(tx, p, 0);
}

/** The Sobel gradient magnitude of one texel's taps, per channel: the two 3x3 gradients, normalized
 * so a full-range step reads as one and clamped there. The centre tap drops out of both gradients. */
float3 edge_detect(const float3 t00,
                   const float3 t01,
                   const float3 t02,
                   const float3 t10,
                   const float3 t12,
                   const float3 t20,
                   const float3 t21,
                   const float3 t22)
{
  const float3 gx = (t02 + 2.0f * t12 + t22) - (t00 + 2.0f * t10 + t20);
  const float3 gy = (t00 + 2.0f * t01 + t02) - (t20 + 2.0f * t21 + t22);
  return clamp(sqrt(gx * gx + gy * gy) * 0.25f, 0.0f, 1.0f);
}

/** The Emboss diagonal lighting kernel of one texel's taps, per channel: `[-2 -1 0 / -1 1 1 / 0 1 2]`,
 * weights summing to one so a flat colour is its own result; clamped to [0, 1]. */
float3 emboss(const float3 t00,
              const float3 t01,
              const float3 t10,
              const float3 t11,
              const float3 t12,
              const float3 t21,
              const float3 t22)
{
  const float3 sum = -2.0f * t00 - t01 - t10 + t11 + t12 + t21 + 2.0f * t22;
  return clamp(sum, 0.0f, 1.0f);
}

/**
 * The single-pass FXAA: the nine lumas name an edge, the gradient of the luminance across the
 * ring names the direction it runs at, and the two ring texels that direction walks to blend with
 * the centre -- the jag smoothed along its own direction, not across it. The CPU's
 * `paint_layer_effect_fxaa_apply`, the same threshold, the same texel pair and the same alpha
 * weights (the colour under a transparent island never bleeds into its border); the working
 * space's luminance weights ride in `luma_coefficients`, the same ones the CPU's
 * `IMB_colormanagement` grey reads. The caller carries the alpha through.
 */
float3 fxaa(sampler2D tx, const int2 texel, const int2 size, const float3 luma_coefficients)
{
  const float4 center = texelFetch(tx, texel, 0);
  if (center.w <= 0.0f) {
    return center.xyz;
  }
  const float luma_nw = dot(fetch_clamped(tx, texel + int2(-1, -1), size), luma_coefficients);
  const float luma_n = dot(fetch_clamped(tx, texel + int2(0, -1), size), luma_coefficients);
  const float luma_ne = dot(fetch_clamped(tx, texel + int2(1, -1), size), luma_coefficients);
  const float luma_w = dot(fetch_clamped(tx, texel + int2(-1, 0), size), luma_coefficients);
  const float luma_m = dot(center.xyz, luma_coefficients);
  const float luma_e = dot(fetch_clamped(tx, texel + int2(1, 0), size), luma_coefficients);
  const float luma_sw = dot(fetch_clamped(tx, texel + int2(-1, 1), size), luma_coefficients);
  const float luma_s = dot(fetch_clamped(tx, texel + int2(0, 1), size), luma_coefficients);
  const float luma_se = dot(fetch_clamped(tx, texel + int2(1, 1), size), luma_coefficients);
  const float highest = max(max(max(max(luma_nw, luma_n), max(luma_ne, luma_w)),
                                max(max(luma_m, luma_e), max(max(luma_sw, luma_s), luma_se))),
                            luma_m);
  const float lowest = min(min(min(min(luma_nw, luma_n), min(luma_ne, luma_w)),
                               min(min(luma_m, luma_e), min(min(luma_sw, luma_s), luma_se))),
                           luma_m);
  if (highest - lowest < 0.0618f) {
    return center.xyz;
  }
  const float dir_x = -((luma_nw + luma_n + luma_ne) - (luma_sw + luma_s + luma_se));
  const float dir_y = ((luma_nw + luma_w + luma_sw) - (luma_ne + luma_e + luma_se));
  const float len = sqrt(dir_x * dir_x + dir_y * dir_y);
  if (len < 1e-6f) {
    return center.xyz;
  }
  /* The ring texel the unit direction points at, rounded half up, as the CPU rounds it. */
  const int2 step_at = int2(int(floor(dir_x / len + 0.5f)), int(floor(dir_y / len + 0.5f)));
  const int2 along_at = texel + step_at;
  const int2 against_at = texel - step_at;
  const int2 along_clamped = int2(clamp(along_at.x, 0, size.x - 1), clamp(along_at.y, 0, size.y - 1));
  const int2 against_clamped = int2(clamp(against_at.x, 0, size.x - 1),
                                    clamp(against_at.y, 0, size.y - 1));
  const float4 along = texelFetch(tx, along_clamped, 0);
  const float4 against = texelFetch(tx, against_clamped, 0);
  const float weight_center = 2.0f * center.w;
  const float weight_sum = weight_center + along.w + against.w;
  return (center.xyz * weight_center + along.xyz * along.w + against.xyz * against.w) / weight_sum;
}
/**
 * The line average of a Blur Directional: `\a r` texel steps each way along the direction the
 * angle names, every tap clamped to the canvas the CPU's line kernel clamps it, the average mixed
 * over the source by the intensity. The angle rides in `filter_params_0.x` (degrees,
 * counter-clockwise off the map's +X) and the mix in `filter_params_0.z`, as the correction pass
 * is given them; the reach is the pass's own clamped `radius`. Alpha is not read here; the caller
 * carries it through.
 */
float3 blur_directional(sampler2D tx,
                        const int2 texel,
                        const int r,
                        const float angle_deg,
                        const float amount,
                        const int2 size)
{
  const float angle_rad = radians(angle_deg);
  const float2 direction = float2(cos(angle_rad), sin(angle_rad));
  float3 sum = float3(0.0f);
  for (int k = -r; k <= r; k++) {
    const int2 at = int2(clamp(texel.x + int(floor(direction.x * float(k) + 0.5f)), 0, size.x - 1),
                         clamp(texel.y + int(floor(direction.y * float(k) + 0.5f)), 0, size.y - 1));
    sum += texelFetch(tx, at, 0).xyz;
  }
  const float3 averaged = sum / float(2 * r + 1);
  const float3 center = texelFetch(tx, texel, 0).xyz;
  return center + (averaged - center) * clamp(amount, 0.0f, 1.0f);
}

/**
 * One texel of a Channel Split: the slot image resampled to the canvas the way the CPU's
 * `math::interpolate_bilinear_fl` reads it (`math_interp.cc`, the Extend wrap), written through
 * `texelFetch` so no sampler's filtering can drift from the CPU. The taps are the floor of the
 * coordinate and the next one, each clamped to the edge, the weights the fractional part; the four
 * terms are summed in the CPU's own order.
 */
float4 slot_bilinear_fetch(sampler2D tx, const int2 size, const float su, const float sv)
{
  const float fu = floor(su);
  const float fv = floor(sv);
  const int x1 = clamp(int(fu), 0, size.x - 1);
  const int x2 = clamp(int(floor(su + 1.0f)), 0, size.x - 1);
  const int y1 = clamp(int(fv), 0, size.y - 1);
  const int y2 = clamp(int(floor(sv + 1.0f)), 0, size.y - 1);
  const float a = su - fu;
  const float b = sv - fv;
  const float a_b = a * b;
  const float ma_b = (1.0f - a) * b;
  const float a_mb = a * (1.0f - b);
  const float ma_mb = (1.0f - a) * (1.0f - b);
  const float4 t00 = texelFetch(tx, int2(x1, y1), 0);
  const float4 t10 = texelFetch(tx, int2(x2, y1), 0);
  const float4 t01 = texelFetch(tx, int2(x1, y2), 0);
  const float4 t11 = texelFetch(tx, int2(x2, y2), 0);
  return ma_mb * t00 + a_mb * t10 + ma_b * t01 + a_b * t11;
}

struct EffectResources {
  /** This pass's source: the effect input, a ping-pong buffer, the luminance buffer for a High Pass
   * tail, or the original for a Sharpen tail. */
  [[sampler(0)]] sampler2D input_tx;
  /** A tail's second input: the blurred copy of #COMBINED_PASS_UNSHARP_TAIL / _HIGH_PASS_TAIL, the
   * blurred choke grey of #COMBINED_PASS_SCREEN_TAIL, or the slot image #COMBINED_PASS_SLOT_FETCH
   * resamples (that one at its own resolution, not the canvas's). */
  [[sampler(1)]] sampler2D aux_tx;
  /** This pass's destination: a scratch buffer, or the cache texture on the last pass. */
  [[image(0, write, SFLOAT_16_16_16_16)]] image2D result_img;
  /** The float32 destination of the passes whose values a half float would round away: the Color
   * Match's pyramid (the alpha-weighted sums of the whole canvas) and the Fill Area's label field
   * (the labels are texel coordinates, which a half float holds exactly only to 2048). The host
   * binds the pass's own pyramid level or field texture here and binds slot 0 beside it for every
   * dispatch, unused or not; every pass but the four float32 ones writes #result_img. The flooding
   * steps also read it back: their walk is sequential and each texel takes the label the texel
   * before it has just been given, so the field is declared read-write like the accumulator images
   * above. */
  [[image(1, read_write, SFLOAT_32_32_32_32)]] image2D field_img;
  /** The canvas size: the window clamps to it on every pass, whatever the dispatch region is. */
  [[push_constant]] const int2 texture_size;
  /**
   * The first texel of this dispatch. A pass computes only its rectangle (the region for the last
   * pass, the region grown by the halo for the ones before it); the window still clamps to #texture_size
   * so it reads the neighbourhood the previous pass left valid over that grown rectangle.
   */
  [[push_constant]] const int2 dispatch_origin;
  /**
   * One past the last texel of this dispatch's rectangle. The groups are 16x16 and round the
   * rectangle up, and a texel past it reads a scratch buffer that pass never filled, so it must not
   * write the cache.
   */
  [[push_constant]] const int2 dispatch_end;
  /** A `COMBINED_PASS_*` value: which kernel this dispatch runs. */
  [[push_constant]] const int pass_kind;
  /** The window half-reach in texels, already clamped the way the CPU kernel clamps it. */
  [[push_constant]] const int radius;
  /** #COMBINED_PASS_EXTREMUM_H / _V: dilate (maximum) when non-zero, erode (minimum) otherwise. */
  [[push_constant]] const int maximum;
  /** #COMBINED_PASS_UNSHARP_TAIL / _HIGH_PASS_TAIL: the Sharpen amount or the High Pass contrast. */
  [[push_constant]] const float aux0;
  /** Extra per-pass scalars the plan built: the Glow tail's tint colour, the Height To Normal's
   * green sign, whatever a pass kind needs beyond #radius / #maximum / #aux0. */
  [[push_constant]] const float4 pass_params;
  /** The second per-pass float4, for the tails that need a colour and a flag side by side with
   * #pass_params (the Drop Shadow's tint and its under-mode). */
  [[push_constant]] const float4 pass_params2;
  /** Whether this pass writes the alpha its kernel computes instead of carrying the source's (the
   * band tails of the effects whose result is a layer of their own). */
  [[push_constant]] const int alpha_from_kernel;
  /** The Noise parameters in its kind's schema order, as the correction pass is given them. */
  [[push_constant]] const float4 filter_params_0;
  /** The working space's luminance weights, which `IMB_colormanagement_get_luminance` reads. */
  [[push_constant]] const float4 luma_coefficients;
  /** Whether the fourth component is a content alpha the composite keeps: where it is not, the
   * buffer the correction pass reads holds one, as `eval_row_content` writes it. */
  [[push_constant]] const int tracks_alpha;
};

[[compute, local_size(16, 16)]]
void effect_comp([[resource_table]] EffectResources &srt,
                 [[global_invocation_id]] const uint3 global_id)
{
  const int2 texel = int2(global_id.xy) + srt.dispatch_origin;
  if (any(greaterThanEqual(texel, srt.texture_size)) ||
      any(greaterThanEqual(texel, srt.dispatch_end)))
  {
    return;
  }
  const float4 source = texelFetch(srt.input_tx, texel, 0);
  float3 rgb = source.xyz;
  /* The alpha the store keeps: the standard rule, kept or dropped to one. A pass that lays a layer
   * of its own (the Channel Split, which the CPU writes with alpha one) overrides it in its case,
   * and the tails that lay a layer write the alpha their kernel computed into `computed_alpha`,
   * which the store keeps when the plan set #alpha_from_kernel for the pass. */
  float pass_alpha = (srt.tracks_alpha != 0) ? source.w : 1.0f;
  float computed_alpha = pass_alpha;
  switch (srt.pass_kind) {
    case COMBINED_PASS_NOISE:
      noise_apply(rgb, texel, srt.filter_params_0, srt.luma_coefficients.xyz);
      break;
    case COMBINED_PASS_LUMINANCE: {
      const float lum = dot(source.xyz, srt.luma_coefficients.xyz);
      rgb = float3(lum);
      break;
    }
    case COMBINED_PASS_BLUR_H:
      rgb = box_blur_axis(srt.input_tx, texel, 0, srt.radius, srt.texture_size);
      break;
    case COMBINED_PASS_BLUR_V:
      rgb = box_blur_axis(srt.input_tx, texel, 1, srt.radius, srt.texture_size);
      break;
    case COMBINED_PASS_BLUR_DIRECTIONAL:
      rgb = blur_directional(srt.input_tx,
                             texel,
                             srt.radius,
                             srt.filter_params_0.x,
                             srt.filter_params_0.z,
                             srt.texture_size);
      break;
    case COMBINED_PASS_FXAA:
      rgb = fxaa(srt.input_tx, texel, srt.texture_size, srt.luma_coefficients.xyz);
      break;
    case COMBINED_PASS_EXTREMUM_H:
      rgb = extremum_axis(srt.input_tx, texel, 0, srt.radius, srt.maximum != 0, srt.texture_size);
      break;
    case COMBINED_PASS_EXTREMUM_V:
      rgb = extremum_axis(srt.input_tx, texel, 1, srt.radius, srt.maximum != 0, srt.texture_size);
      break;
    case COMBINED_PASS_EDGE_3X3:
    case COMBINED_PASS_EMBOSS_3X3: {
      /* Both 3x3 kernels read the same clamped ring of taps, so one fetch block serves both; the
       * centre tap drops out of the Sobel gradients and the two far corners out of the Emboss. */
      const float3 t00 = fetch_clamped(srt.input_tx, texel + int2(-1, -1), srt.texture_size);
      const float3 t01 = fetch_clamped(srt.input_tx, texel + int2(0, -1), srt.texture_size);
      const float3 t02 = fetch_clamped(srt.input_tx, texel + int2(1, -1), srt.texture_size);
      const float3 t10 = fetch_clamped(srt.input_tx, texel + int2(-1, 0), srt.texture_size);
      const float3 t11 = fetch_clamped(srt.input_tx, texel + int2(0, 0), srt.texture_size);
      const float3 t12 = fetch_clamped(srt.input_tx, texel + int2(1, 0), srt.texture_size);
      const float3 t20 = fetch_clamped(srt.input_tx, texel + int2(-1, 1), srt.texture_size);
      const float3 t21 = fetch_clamped(srt.input_tx, texel + int2(0, 1), srt.texture_size);
      const float3 t22 = fetch_clamped(srt.input_tx, texel + int2(1, 1), srt.texture_size);
      if (srt.pass_kind == COMBINED_PASS_EMBOSS_3X3) {
        rgb = emboss(t00, t01, t10, t11, t12, t21, t22);
      }
      else {
        rgb = edge_detect(t00, t01, t02, t10, t12, t20, t21, t22);
      }
      break;
    }
    case COMBINED_PASS_UNSHARP_TAIL:
    case COMBINED_PASS_HIGH_PASS_TAIL: {
      const float3 blurred = texelFetch(srt.aux_tx, texel, 0).xyz;
      if (srt.pass_kind == COMBINED_PASS_HIGH_PASS_TAIL) {
        /* A High Pass: the detail against the mid grey, taken in the display encoding. A negative
         * linear colour would break the blends, so the result is clipped below zero only. */
        const float dr = effect_linearrgb_to_srgb(source.x) - effect_linearrgb_to_srgb(blurred.x);
        const float dg = effect_linearrgb_to_srgb(source.y) - effect_linearrgb_to_srgb(blurred.y);
        const float db = effect_linearrgb_to_srgb(source.z) - effect_linearrgb_to_srgb(blurred.z);
        rgb = max(float3(effect_srgb_to_linearrgb(0.5f + dr * srt.aux0),
                         effect_srgb_to_linearrgb(0.5f + dg * srt.aux0),
                         effect_srgb_to_linearrgb(0.5f + db * srt.aux0)),
                  float3(0.0f));
      }
      else {
        /* A Sharpen: the detail the blur removed comes back on top of the colour, clipped below. */
        rgb = max(source.xyz + (source.xyz - blurred.xyz) * srt.aux0, float3(0.0f));
      }
      break;
    }
    case COMBINED_PASS_CHOKE: {
      /* The choke step of the Glow, `paint_layer_effect_glow_apply` before its blur: the scene
       * linear luminance clamped into [0, 1] -- the GPU Luminance pass does not clamp, so this pass
       * does, as the CPU's does -- then reopened by the spread, everything below it reading zero and
       * everything above one, the ramp between rescaled to the whole range. The blur and the screen
       * are the passes that follow. */
      const float luma = clamp(dot(source.xyz, srt.luma_coefficients.xyz), 0.0f, 1.0f);
      const float choke_den = max(1.0f - srt.aux0, 1e-6f);
      rgb = float3(clamp((luma - srt.aux0) / choke_den, 0.0f, 1.0f));
      break;
    }
    case COMBINED_PASS_SCREEN_TAIL: {
      /* The screen tail of the Glow, `paint_layer_effect_glow_apply` after its blur: the choked grey
       * the blur built (its red channel, the one grey the CPU reads back) scaled by the opacity in
       * `aux0`, screened under the tint in `pass_params.xyz` -- `c + glow * (1 - c)`, so a black
       * texel lands on the colour and a lit one keeps its light. The source is the original the
       * tail is given. */
      const float m = texelFetch(srt.aux_tx, texel, 0).x * srt.aux0;
      rgb = source.xyz + srt.pass_params.xyz * m * (1.0f - source.xyz);
      break;
    }
    case COMBINED_PASS_BLOCK_AVERAGE: {
      /* The Pixelate, `paint_layer_effect_pixelate_apply`: the block the texel falls into averaged
       * from the clone the input holds, so the average is the one the block had, not the one a
       * neighbour left. The colour is weighted by the alpha, so the black a transparent texel keeps
       * at an island's edge never leaks into an opaque one; a block without coverage keeps the plain
       * average. The block amount rides in `aux0` (blocks per side), the block size the canvas cut
       * into that many, the last one of a side shorter at the canvas edge. Every texel sums its own
       * block -- the CPU sums it once and writes the colour around, the same number either way --
       * walking it row by row in the CPU's own order, so the float adds land the same. */
      const int pixels = max(int(srt.aux0), 1);
      const int block_w = max(srt.texture_size.x / pixels, 1);
      const int block_h = max(srt.texture_size.y / pixels, 1);
      if (block_w > 1 || block_h > 1) {
        const int bx = (texel.x / block_w) * block_w;
        const int by = (texel.y / block_h) * block_h;
        const int bx_end = min(bx + block_w, srt.texture_size.x);
        const int by_end = min(by + block_h, srt.texture_size.y);
        float3 weighted = float3(0.0f);
        float3 plain = float3(0.0f);
        float alpha_sum = 0.0f;
        for (int sy = by; sy < by_end; sy++) {
          for (int sx = bx; sx < bx_end; sx++) {
            const float4 p = texelFetch(srt.input_tx, int2(sx, sy), 0);
            weighted += p.xyz * p.w;
            plain += p.xyz;
            alpha_sum += p.w;
          }
        }
        const float count = float((bx_end - bx) * (by_end - by));
        rgb = (alpha_sum > 1e-6f) ? weighted / alpha_sum : plain / count;
      }
      break;
    }
    case COMBINED_PASS_HEIGHT_TO_NORMAL_3X3: {
      /* The Height To Normal, `paint_layer_effect_height_to_normal_apply`: the Sobel pair of the
       * height the red holds, `dh_du` the slope rising along +U and `dh_dv` along +V (row y + 1 is
       * the higher V), each scaled by `aux0` -- the strength, or the world-units slope the plan
       * folds in. The normal leans against the slope; the DirectX encoding flips the green, and
       * `pass_params.x` names the convention (+1 the OpenGL one, -1 the DirectX one), negated here
       * to the CPU's own `green_sign`. The encode is `0.5 + 0.5 n`. */
      const float h00 = height_fetch(srt.input_tx, texel + int2(-1, -1), srt.texture_size);
      const float h01 = height_fetch(srt.input_tx, texel + int2(0, -1), srt.texture_size);
      const float h02 = height_fetch(srt.input_tx, texel + int2(1, -1), srt.texture_size);
      const float h10 = height_fetch(srt.input_tx, texel + int2(-1, 0), srt.texture_size);
      const float h12 = height_fetch(srt.input_tx, texel + int2(1, 0), srt.texture_size);
      const float h20 = height_fetch(srt.input_tx, texel + int2(-1, 1), srt.texture_size);
      const float h21 = height_fetch(srt.input_tx, texel + int2(0, 1), srt.texture_size);
      const float h22 = height_fetch(srt.input_tx, texel + int2(1, 1), srt.texture_size);
      const float dh_du = ((h02 + 2.0f * h12 + h22) - (h00 + 2.0f * h10 + h20)) * 0.25f;
      const float dh_dv = ((h20 + 2.0f * h21 + h22) - (h00 + 2.0f * h01 + h02)) * 0.25f;
      const float nx = -dh_du * srt.aux0;
      const float ny = -srt.pass_params.x * dh_dv * srt.aux0;
      const float inv_len = 1.0f / sqrt(nx * nx + ny * ny + 1.0f);
      rgb = float3(nx * inv_len * 0.5f + 0.5f, ny * inv_len * 0.5f + 0.5f, inv_len * 0.5f + 0.5f);
      break;
    }
    case COMBINED_PASS_SLOT_FETCH: {
      /* The Channel Split, `paint_layer_effect_channel_split_apply`: one channel of the slot image
       * laid as a grey, resampled to the canvas the way the CPU's bilinear reads it (the slot rides
       * in `aux_tx`, at its own resolution whatever the canvas's is). The channel number rides in
       * `aux0`, 0 red to 3 alpha, clamped like the CPU's. An empty slot is the host's to skip: the
       * CPU leaves the buffer unchanged while no image is assigned, and there is no texture to
       * bind. */
      const int2 map_size = textureSize(srt.aux_tx, 0);
      const float su = ((float(texel.x) + 0.5f) / float(srt.texture_size.x)) * float(map_size.x) -
                       0.5f;
      const float sv = ((float(texel.y) + 0.5f) / float(srt.texture_size.y)) * float(map_size.y) -
                       0.5f;
      const int component = clamp(int(srt.aux0), 0, 3);
      const float4 fetched = slot_bilinear_fetch(srt.aux_tx, map_size, su, sv);
      const float value = (component == 0) ? fetched.x :
                          ((component == 1) ? fetched.y :
                           ((component == 2) ? fetched.z : fetched.w));
      rgb = float3(value);
      /* The split lays a layer of its own: the CPU writes alpha one when it lays a layer, and the
       * GPU pass is only queued as one, so the store keeps one whatever the row carried -- not the
       * standard rule this function's tail applies to the other kinds. */
      pass_alpha = 1.0f;
      break;
    }
    case COMBINED_PASS_JFA_INIT: {
      /* The distance transform's seed field, the seeding of the Bevel
       * (`paint_layer_effect_bevel_apply`, `interior >= 0.5 ? UNSEEN : 0` -- the seeds are the
       * texels short of the border) and of the Mask Outline
       * (`paint_layer_effect_mask_outline_apply`, the `inside` its threshold builds): the texels
       * whose scalar sits on the seed side of the threshold stamp their own coordinates, the rest a
       * far point the JFA steps refine from. `pass_params.x` names the scalar (0 the alpha, 1 the
       * scene-linear luminance), `pass_params.y` is the threshold and `pass_params.z` the polarity
       * (unset the scalars below it seed, set the ones at or over it do). The CPU chamfer-walks its
       * field from here -- the flood is the GPU's own way to the same distances. */
      const float scalar = (srt.pass_params.x == 0.0f) ? source.w :
                                                         dot(source.xyz, srt.luma_coefficients.xyz);
      /* The polarity: the seeds are one side of the threshold, and which side is the plan's to
       * name -- the Bevel seeds the scalars short of the half, the Mask Outline seeds whichever
       * side its position and Start From Black make the inside. `pass_params.z` set seeds the
       * scalars at or over the threshold instead of below it. */
      const bool is_seed = (srt.pass_params.z != 0.0f) ? (scalar >= srt.pass_params.y) :
                                                         (scalar < srt.pass_params.y);
      const float2 field = is_seed ? float2(texel) : float2(1e7f, 1e7f);
      rgb = float3(field, 0.0f);
      break;
    }
    case COMBINED_PASS_JFA_STEP: {
      /* One Jump Flooding step: the field the previous pass left holds each texel's nearest seed so
       * far, and every texel adopts the best candidate its ring of eight neighbours at ±`radius`
       * reports -- its own first, replaced only by a strictly closer one. The radius halves every
       * step the plan queues down to one, so the whole canvas floods in a logarithmic number of
       * passes. A neighbour past the canvas clamps to the edge texel, which only repeats a
       * candidate that texel offers anyway; an unseen candidate's far point never wins the length. */
      float2 best = texelFetch(srt.input_tx, texel, 0).xy;
      float best_dist = length(float2(texel) - best);
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          if (dx == 0 && dy == 0) {
            continue;
          }
          const int2 at = int2(clamp(texel.x + dx * srt.radius, 0, srt.texture_size.x - 1),
                               clamp(texel.y + dy * srt.radius, 0, srt.texture_size.y - 1));
          const float2 candidate = texelFetch(srt.input_tx, at, 0).xy;
          const float candidate_dist = length(float2(texel) - candidate);
          if (candidate_dist < best_dist) {
            best_dist = candidate_dist;
            best = candidate;
          }
        }
      }
      rgb = float3(best, 0.0f);
      break;
    }
    case COMBINED_PASS_DIST_EVAL: {
      /* The distance field's read: the Euclidean distance to the seed the field names -- zero on a
       * seed, the CPU's far value on a texel no seed reached. `pass_params.z` picks the reader, the
       * Bevel's ramp (`paint_layer_effect_bevel_apply`) or the Mask Outline's band
       * (`paint_layer_effect_mask_outline_apply`). The CPU chamfer-walks sides at 3 and diagonals
       * at 4 and divides the distance by 3 toward texels; the flood reads Euclidean, so the reach
       * the plan pushes in `pass_params.x` already folds that normalization in. */
      const float2 seed = texelFetch(srt.input_tx, texel, 0).xy;
      const float dist = (seed.x >= 1e6f) ? 1e6f : length(float2(texel) - seed);
      if (int(srt.pass_params.z) == 0) {
        /* The Bevel's ramp: the distance over the reach, clamped, as a grey. */
        const float value = clamp(dist / srt.pass_params.x, 0.0f, 1.0f);
        rgb = float3(value);
      }
      else {
        /* The Outline's band: one less the distance over the reach, bent toward the smoothstep by
         * `pass_params.y` -- the CPU's `1 - dist / 3 / reach` under the same folded reach. The seed
         * side is the one the position leaves empty -- a texel whose seed is its own coordinates
         * was stamped as a seed, and the CPU sets no band there -- so it reads zero whatever its
         * distance. The band is the layer's own alpha: the blur carries it through and the tint
         * tail writes it. */
        const bool on_seed_side = all(equal(seed, float2(texel)));
        float band = on_seed_side ? 0.0f : clamp(1.0f - dist / srt.pass_params.x, 0.0f, 1.0f);
        /* Not `smooth`: that is a reserved interpolation qualifier in GLSL. */
        const float hermite = band * band * (3.0f - 2.0f * band);
        band += (hermite - band) * clamp(srt.pass_params.y, 0.0f, 1.0f);
        rgb = float3(band);
        computed_alpha = band;
      }
      break;
    }
    case COMBINED_PASS_SILHOUETTE: {
      /* The Drop Shadow's silhouette (`paint_layer_effect_drop_shadow_apply`): the shape its source
       * names, one grey the spread and the blur work on before the tail reads it. `pass_params.x`
       * carries the source, the CPU's own order: 1 the scene-linear luminance clamped into [0, 1]
       * (#PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE), 0 the alpha as it is, unclamped. */
      const float shape = (srt.pass_params.x != 0.0f) ?
                              clamp(dot(source.xyz, srt.luma_coefficients.xyz), 0.0f, 1.0f) :
                              source.w;
      rgb = float3(shape);
      break;
    }
    case COMBINED_PASS_OFFSET_TINT_TAIL: {
      /* The Drop Shadow's combine tail (`paint_layer_effect_drop_shadow_apply` after its blur): the
       * blurred silhouette in `aux_tx` read at the offset the shadow is cast by -- the texel the
       * CPU rounds the angle's direction and the distance to, `floor(dir * dist + 0.5)` half up,
       * from the direction and the distance the plan pushes in `pass_params.xy` / `.z` -- so the
       * read sits at `texel - offset` and the shadow sits displaced along the angle. A texel the
       * offset reads from past the canvas has no silhouette to cast: zero, not the clamped edge,
       * which would smear the border row across the whole band. An alpha source keeps the shadow
       * off the content (`params2.w` set, the under-mode folded), the opacity in `pass_params.w`
       * weighs it, and the tint in `params2.rgb` goes out premultiplied by the weight -- the layer
       * of its own the effect leaves. */
      const int2 offset = int2(floor(srt.pass_params.xy * srt.pass_params.z + 0.5f));
      const float shadow = fetch_zero_outside(srt.aux_tx, texel - offset, srt.texture_size).x;
      const float under = (srt.pass_params2.w != 0.0f) ? 1.0f - clamp(source.w, 0.0f, 1.0f) : 1.0f;
      const float weight = shadow * srt.pass_params.w * under;
      const float alpha = clamp(weight, 0.0f, 1.0f);
      rgb = srt.pass_params2.xyz * alpha;
      computed_alpha = alpha;
      break;
    }
    case COMBINED_PASS_SLOPE_DRIVER: {
      /* The Blur Slope's driver (`paint_layer_effect_slope_source_build`): one grey per texel whose
       * gradient the walk follows. `aux0` carries the source mode, the CPU's own values: 1 the
       * previous input's luminance (#PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT, the plan's blur
       * passes having softened it first), 0 the value noise
       * (#PAINT_LAYER_SLOPE_SOURCE_NOISE). `pass_params` is the schema order: the position, the
       * contrast, the seed and the tilling. */
      float value;
      const int seed = int(srt.pass_params.z + 0.5f);
      if (int(srt.aux0) == 1) {
        value = dot(source.xyz, srt.luma_coefficients.xyz) + srt.pass_params.x;
      }
      else {
        /* The noise: the tiles the tilling names across the map, eight lattice cells to a tile, so
         * the pattern the seed names has a slope worth following at any size. The seed rides the
         * hash and offsets the lattice, exactly as the CPU's build does. */
        const float tiles = max(srt.pass_params.w, 0.25f);
        const float nx = (float(texel.x) / float(srt.texture_size.x) * tiles +
                          float(seed % 7) * 0.113f) *
                         8.0f;
        const float ny = (float(texel.y) / float(srt.texture_size.y) * tiles +
                          float(seed % 13) * 0.179f) *
                         8.0f;
        value = slope_value_noise(nx, ny, seed) + srt.pass_params.x;
      }
      /* The grade both modes take: the position shifted the midpoint, the contrast scaled it about
       * the half. */
      rgb = float3((value - 0.5f) * srt.pass_params.y + 0.5f);
      break;
    }
    case COMBINED_PASS_SLOPE_WALK: {
      /* The Blur Slope's walk (`paint_layer_effect_blur_slope_apply`): the driver in `input_tx`
       * gives the gradient, the central difference of the neighbours clamped to the canvas, and the
       * direction it names is walked for `radius` samples (the CPU's quality, clamped to 1..32)
       * over the span in `pass_params.y`. The mode in `pass_params.x` says what the taps
       * accumulate: the alpha-weighted average, the minimum that eats the edges away or the maximum
       * that smears the white out (`PAINT_LAYER_BLUR_SLOPE_BLENDING_*`). The taps read the original
       * in `aux_tx`, nearest and clamped, and the average weighs them by their alpha so the black
       * under a transparent island never darkens a border. A flat slope, a zero span, a walk
       * without coverage or a single sample keep the texel as it is -- the original's own, not the
       * driver's. */
      const float4 original = texelFetch(srt.aux_tx, texel, 0);
      rgb = original.xyz;
      const float intensity = srt.pass_params.y;
      const int samples = srt.radius;
      if (intensity > 0.0f && samples > 1) {
        const int xl = max(texel.x - 1, 0);
        const int xr = min(texel.x + 1, srt.texture_size.x - 1);
        const int yu = max(texel.y - 1, 0);
        const int yd = min(texel.y + 1, srt.texture_size.y - 1);
        const float gx = (texelFetch(srt.input_tx, int2(xr, texel.y), 0).x -
                          texelFetch(srt.input_tx, int2(xl, texel.y), 0).x) *
                         0.5f;
        const float gy = (texelFetch(srt.input_tx, int2(texel.x, yd), 0).x -
                          texelFetch(srt.input_tx, int2(texel.x, yu), 0).x) *
                         0.5f;
        const float slope = sqrt(gx * gx + gy * gy);
        if (slope >= 1e-4f) {
          const float dir_x = gx / slope;
          const float dir_y = gy / slope;
          const int blending = int(srt.pass_params.x);
          const bool take_min = (blending == 1); /* PAINT_LAYER_BLUR_SLOPE_BLENDING_MIN. */
          const bool take_max = (blending == 2); /* PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX. */
          float3 result = float3(0.0f);
          float weight_sum = 0.0f;
          bool any = false;
          for (int k = 0; k < samples; k++) {
            const float t = (float(k) / float(samples - 1) - 0.5f) * intensity;
            const int sx = clamp(int(floor(float(texel.x) + dir_x * t + 0.5f)),
                                 0,
                                 srt.texture_size.x - 1);
            const int sy = clamp(int(floor(float(texel.y) + dir_y * t + 0.5f)),
                                 0,
                                 srt.texture_size.y - 1);
            const float4 tap = texelFetch(srt.aux_tx, int2(sx, sy), 0);
            const float alpha = tap.w;
            if (alpha <= 0.0f) {
              continue;
            }
            if (take_min || take_max) {
              /* The first covered tap stands as it is, then the extremes narrow or widen it. */
              result = (!any) ? tap.xyz : (take_min ? min(result, tap.xyz) : max(result, tap.xyz));
              any = true;
            }
            else {
              result += tap.xyz * alpha;
              weight_sum += alpha;
            }
          }
          if (take_min || take_max) {
            if (any) {
              rgb = result;
            }
          }
          else if (weight_sum > 0.0f) {
            const float inv = 1.0f / weight_sum;
            rgb = result * inv;
          }
        }
      }
      break;
    }
    case COMBINED_PASS_BAND_TINT_TAIL: {
      /* The Mask Outline's tint tail (`paint_layer_effect_mask_outline_apply` after its blur): the
       * blurred band the previous passes left -- its red, the one grey the CPU reads back -- turned
       * into the premultiplied colour, the band itself as the alpha, so the ring lays over the
       * empty map around the islands and the islands themselves are untouched. The tint rides in
       * `params2.rgb`; the CPU's forced white under an alpha source is the plan's to fold in. */
      const float band = clamp(texelFetch(srt.input_tx, texel, 0).x, 0.0f, 1.0f);
      rgb = srt.pass_params2.xyz * band;
      computed_alpha = band;
      break;
    }
    case COMBINED_PASS_REDUCE_START: {
      /* The Color Match's average (`paint_layer_effect_color_average` of
       * `paint_layers_effect_kernels.cc`) built as a sum pyramid: this level sums every 2x2 block
       * of the input, the colour weighted by the alpha and the alpha summed beside it, so the
       * levels above fold component-wise and the 1x1 texel ends at the totals the CPU's running
       * sums end at. A texel without coverage adds nothing, which is the CPU's own `texel[3] > 0`
       * skip -- a zero weight multiplies nothing and adds none. The sums accumulate in float32
       * registers into a float32 pyramid, the known precision difference against the CPU's doubles:
       * a canvas of unit colours stays far inside the float mantissa, and the grade reads a colour
       * from the totals, not a checksum of them. The block of a texel at the pyramid's edge runs
       * past the canvas and the taps past it add nothing -- each canvas texel is counted once, the
       * way the CPU's loop counts it -- so an odd side folds a shortened block, and the host has to
       * dispatch the level at the size that still covers the canvas, `(size + 1) >> 1` of the level
       * above, not the floor shift that would drop its last texel. `radius` names the level this
       * pass writes -- one here -- for the plan's bookkeeping. */
      const int2 src_size = textureSize(srt.input_tx, 0);
      const int2 block_at = texel * 2;
      float3 sum_rgb = float3(0.0f);
      float sum_alpha = 0.0f;
      for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          const int2 at = block_at + int2(dx, dy);
          if (any(greaterThanEqual(at, src_size))) {
            continue; /* Past the canvas: nothing to add, not the edge texel again. */
          }
          const float4 texel_read = texelFetch(srt.input_tx, at, 0);
          sum_rgb += texel_read.xyz * texel_read.w;
          sum_alpha += texel_read.w;
        }
      }
      imageStore(srt.field_img, texel, float4(sum_rgb, sum_alpha));
      return;
    }
    case COMBINED_PASS_REDUCE_STEP: {
      /* One fold of the Color Match's pyramid: the four texels of the level above that tile this
       * one's block summed component-wise, the colour and the weight together. No weighting happens
       * here -- the first level already carries the alpha-weighted colour in its RGB, so the plain
       * sums are the parent block's totals, the terms the CPU's reduction adds in its own order.
       * The taps past the level's edge add nothing, the first level's own rule: an odd side of the
       * level above folds a shortened block, each of its texels counted once.
       * `radius` names the level this pass writes, for the plan's bookkeeping. */
      const int2 src_size = textureSize(srt.input_tx, 0);
      const int2 block_at = texel * 2;
      float4 sum = float4(0.0f);
      for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          const int2 at = block_at + int2(dx, dy);
          if (any(greaterThanEqual(at, src_size))) {
            continue; /* Past the level: nothing to add, not the edge texel again. */
          }
          sum += texelFetch(srt.input_tx, at, 0);
        }
      }
      imageStore(srt.field_img, texel, sum);
      return;
    }
    case COMBINED_PASS_MATCH_GRADE: {
      /* The Color Match's grade, `paint_layer_color_match_apply`: each HSL component of the texel
       * moves by the target's difference from the source's, so a texel of the source's colour lands
       * exactly on the target; the optional variation jitters the three from the texel hash (the
       * CPU's salts 1, 2 and 3 with the seed zero, swung to [-1, 1]), the hue wrapped again, and
       * the HSL goes back to RGB through the `BLI_math_color` pair this file ports above. The
       * source is the picked colour in `pass_params2.rgb` when `pass_params2.w` is set -- the plan
       * queues no reduction passes then, and this pass never reads the pyramid -- otherwise the
       * alpha-weighted average the 1x1 pyramid texel carries, read at its only texel: the colour
       * sums divided by the weight sum, zero when the weight is zero, the CPU's own test. The
       * target rides in `pass_params.rgb` and the variation's three amounts in `pass_params2.xyz` --
       * the first float4 the correction pass pushes carries the picked flag and the source colour,
       * not the variation. Alpha is untouched. */
      float3 source_color;
      if (srt.pass_params2.w != 0.0f) {
        source_color = srt.pass_params2.rgb;
      }
      else {
        const float4 total = texelFetch(srt.aux_tx, int2(0), 0);
        source_color = (total.w > 0.0f) ? total.xyz / total.w : float3(0.0f);
      }
      const float3 source_hsl = effect_rgb_to_hsl(source_color);
      const float3 target_hsl = effect_rgb_to_hsl(srt.pass_params.rgb);
      const float3 texel_hsl = effect_rgb_to_hsl(rgb);
      float hue = texel_hsl.x + (target_hsl.x - source_hsl.x);
      hue -= floor(hue);
      float chroma = clamp(texel_hsl.y + (target_hsl.y - source_hsl.y), 0.0f, 1.0f);
      float luma = clamp(texel_hsl.z + (target_hsl.z - source_hsl.z), 0.0f, 1.0f);
      const float3 variation = srt.pass_params2.xyz;
      if (any(greaterThan(variation, float3(0.0f)))) {
        /* The hash of the texel keeps the jitter stable between evaluations, the salts keeping the
         * three components independent of each other, exactly the CPU's own lambda. */
        const float jitter_hue = texel_hash_uniform(texel, 0, 1u) * 2.0f - 1.0f;
        const float jitter_chroma = texel_hash_uniform(texel, 0, 2u) * 2.0f - 1.0f;
        const float jitter_luma = texel_hash_uniform(texel, 0, 3u) * 2.0f - 1.0f;
        hue += jitter_hue * variation.x;
        hue -= floor(hue);
        chroma = clamp(chroma + jitter_chroma * variation.y, 0.0f, 1.0f);
        luma = clamp(luma + jitter_luma * variation.z, 0.0f, 1.0f);
      }
      rgb = effect_hsl_to_rgb(float3(hue, chroma, luma));
      break;
    }
    case COMBINED_PASS_FLOOD_INIT: {
      /* The Fill Area's label field, the border and region walk of
       * `paint_layer_effect_fill_area_apply` laid out for the flooding: the scalar the border is
       * read at -- `pass_params.x` names it, 0 the alpha as it is, 1 the scene-linear luminance
       * clamped into [0, 1] (a mask item always reads the luminance, its grey carrying no alpha of
       * its own; the plan folds that in) -- is border at or over the threshold in `pass_params.y`,
       * the CPU's own inclusive edge. A border texel stamps the background label and no region:
       * the label zero with the flag in blue unset, a barrier the labels never cross or offer. A
       * texel short of the border on the canvas edge stamps the background label with the flag
       * set -- the seed the background swallows its region from, the CPU's uv-border IGNORE mode;
       * the FILL mode's largest-region rule and the debug ramp stay on the CPU. Every other texel
       * short of the border stamps its own coordinates, the candidate label its region's flooding
       * narrows to. The labels are texel coordinates, exact in a half float only to 2048 -- the
       * field the host binds here is float32, so a 4K map's labels are exact. The alpha is the
       * store's standard one, carried for the ping-pong. */
      const float scalar = (srt.pass_params.x == 0.0f) ? source.w :
                                                         clamp(dot(source.xyz,
                                                                   srt.luma_coefficients.xyz),
                                                               0.0f,
                                                               1.0f);
      const bool border = scalar >= srt.pass_params.y;
      const bool on_edge = (texel.x == 0 || texel.y == 0 ||
                            texel.x == srt.texture_size.x - 1 ||
                            texel.y == srt.texture_size.y - 1);
      const float2 label = (border || on_edge) ? float2(0.0f) : float2(texel);
      imageStore(srt.field_img, texel, float4(label, border ? 0.0f : 1.0f, pass_alpha));
      return;
    }
    case COMBINED_PASS_FLOOD_STEP: {
      /* The Fill Area's flooding, the GPU's way to the CPU's region walk: every texel short of the
       * border ends at its region's smallest label, and the background label zero swallows exactly
       * the regions the canvas edge reaches -- the CPU's own background test. The step is one
       * raster sweep, the mode in `pass_params.x`: 1 walks the rows, 2 the columns. One thread per
       * line (the host clips the dispatch to a texel-wide region) walks the line down and back,
       * each texel taking the label of the free neighbour it has just passed, and the whole run of
       * free texels a label has reached crosses in one go -- the GPU's substitute for the CPU's
       * sequential region walk, which no window of ring candidates can replace: a jump past a wall
       * thinner than its radius would swallow an enclosed hole the edge never reached, the ring
       * the CPU kernel fills being exactly that hole. The walk adopts whole labels, compared
       * lexicographically, from the four the CPU's regions are connected by -- a diagonal step
       * would merge regions that only touch at a corner, which the CPU's walk keeps separate. The
       * plan queues the two modes alternating a number of times; every pair turns the wavefront
       * once, so a background the edge reaches through a few turns floods early, and the queue
       * length, not this pass, is what bounds how winding it may be.
       *
       * The walk reads the label it has just written back through the field itself
       * (#field_img being read-write), and the line's texels before the sweep through `input_tx`;
       * a border texel keeps the label and the flag it came in with. The flag and the alpha carry
       * through unchanged; the field is float32, so the coordinates are exact. */
      const int2 field_size = textureSize(srt.input_tx, 0);
      if (srt.pass_params.x == 1.0f) {
        /* The rows: down the line, then back up it, the second walk reading the labels the first
         * has written. */
        const int y = texel.y;
        float4 carried = texelFetch(srt.input_tx, int2(0, y), 0);
        imageStore(srt.field_img, int2(0, y), carried);
        for (int x = 1; x < field_size.x; x++) {
          float4 own = texelFetch(srt.input_tx, int2(x, y), 0);
          if (own.z != 0.0f && carried.z != 0.0f &&
              (carried.x < own.x || (carried.x == own.x && carried.y < own.y)))
          {
            own.xy = carried.xy;
          }
          imageStore(srt.field_img, int2(x, y), own);
          carried = own;
        }
        carried = imageLoad(srt.field_img, int2(field_size.x - 1, y));
        for (int x = field_size.x - 2; x >= 0; x--) {
          float4 own = imageLoad(srt.field_img, int2(x, y));
          if (own.z != 0.0f && carried.z != 0.0f &&
              (carried.x < own.x || (carried.x == own.x && carried.y < own.y)))
          {
            own.xy = carried.xy;
          }
          imageStore(srt.field_img, int2(x, y), own);
          carried = own;
        }
        return;
      }
      /* The columns: the same walk across them, down and back up. */
      const int x = texel.x;
      float4 carried = texelFetch(srt.input_tx, int2(x, 0), 0);
      imageStore(srt.field_img, int2(x, 0), carried);
      for (int y = 1; y < field_size.y; y++) {
        float4 own = texelFetch(srt.input_tx, int2(x, y), 0);
        if (own.z != 0.0f && carried.z != 0.0f &&
            (carried.x < own.x || (carried.x == own.x && carried.y < own.y)))
        {
          own.xy = carried.xy;
        }
        imageStore(srt.field_img, int2(x, y), own);
        carried = own;
      }
      carried = imageLoad(srt.field_img, int2(x, field_size.y - 1));
      for (int y = field_size.y - 2; y >= 0; y--) {
        float4 own = imageLoad(srt.field_img, int2(x, y));
        if (own.z != 0.0f && carried.z != 0.0f &&
            (carried.x < own.x || (carried.x == own.x && carried.y < own.y)))
        {
          own.xy = carried.xy;
        }
        imageStore(srt.field_img, int2(x, y), own);
        carried = own;
      }
      return;
    }
    case COMBINED_PASS_DIFFUSE_STEP: {
      /* One pass of the Fill Area's diffusion (`paint_layer_effect_fill_area_apply`, the loop over
       * the iterations): an enclosed texel walks toward the average of its four neighbours inside
       * the map, the map's edge being no neighbour, and everything else stays as it is -- the
       * border texels are the boundary condition the fill takes its colour from, and the
       * background ones are never read, two texels side by side short of the border being one
       * region, so an enclosed texel's neighbours are border or its own region only. The flood
       * field in aux_tx names the three: the flag in blue unset is border, the background label
       * zero with the flag set is background, the rest enclosed. The first step (`radius` one)
       * reads the original, where the CPU's read side holds the border's straightened colours and
       * zeros on the enclosed texels it zeroed at init -- a neighbour short of the border
       * contributes nothing, a border neighbour its straightened colour, premultiplied divided
       * back by an alpha in [0.01, 1) when the effect lays a layer (`pass_params.w`, the plan's
       * as_layer). The later steps read the previous step, where the border passed its colour
       * through and the enclosed hold the running average, so the plain four fetches are the CPU's
       * own read. The average divides by the neighbours that exist, the CPU's own count. A border
       * or a background texel keeps the colour it came in with: the border's is the boundary
       * condition the later steps read, the background's is what a mask item's tail passes
       * through. Alpha is the store's standard one; the coverage lives in the flood field, which
       * the tail reads. */
      const float4 field = texelFetch(srt.aux_tx, texel, 0);
      const bool border = field.z == 0.0f;
      const bool background = (!border && field.x == 0.0f && field.y == 0.0f);
      if (border) {
        if (srt.radius == 1 && srt.pass_params.w != 0.0f && source.w >= 0.01f && source.w < 1.0f) {
          /* The CPU straightens the border's premultiplied colour once, at init; the later steps
           * read the straightened value this step passes through. */
          rgb = source.xyz / source.w;
        }
      }
      else if (!background) {
        const bool first = (srt.radius == 1);
        float3 sum = float3(0.0f);
        int samples = 0;
        for (int dir = 0; dir < 4; dir++) {
          const int2 offset = (dir == 0) ?
                                  int2(1, 0) :
                                  ((dir == 1) ? int2(-1, 0) :
                                                ((dir == 2) ? int2(0, 1) : int2(0, -1)));
          const int2 at = texel + offset;
          if (any(lessThan(at, int2(0))) || any(greaterThanEqual(at, srt.texture_size))) {
            continue; /* The map's edge is no neighbour. */
          }
          if (first) {
            const float4 neighbour_field = texelFetch(srt.aux_tx, at, 0);
            if (neighbour_field.z == 0.0f) {
              /* A border neighbour: the straightened colour the CPU's read side holds. */
              const float4 neighbour = texelFetch(srt.input_tx, at, 0);
              float3 colour = neighbour.xyz;
              if (srt.pass_params.w != 0.0f && neighbour.w >= 0.01f && neighbour.w < 1.0f) {
                colour /= neighbour.w;
              }
              sum += colour;
            }
            /* A neighbour short of the border contributes nothing: the CPU zeroed the enclosed
             * texels of its read side at init. */
          }
          else {
            sum += texelFetch(srt.input_tx, at, 0).xyz;
          }
          samples++;
        }
        rgb = (samples > 0) ? sum / float(samples) : float3(0.0f);
      }
      break;
    }
    case COMBINED_PASS_FILL_TAIL: {
      /* The Fill Area's write tail (`paint_layer_effect_fill_area_apply`'s write_result and the
       * blur that follows it): an enclosed texel takes the fill -- the diffused colour the
       * previous pass left, or the solid one in `pass_params2.rgb` when `pass_params.x` says so --
       * and everything else is empty when the effect lays a layer (#alpha_from_kernel, the plan's
       * as_layer): a hole between strokes has no coverage of its own, so a colour-only result would
       * stay invisible on it. A mask item keeps what the texel held and leaves the alpha alone.
       * The coverage is the enclosed indicator the flood field names, softened by the box blur of
       * `radius` the CPU runs on a grey of its own -- folded here as the window sum of the
       * indicator over the clamped window, which is what the CPU's two axis passes compose to, so
       * the alpha this pass writes survives the blur pair that follows it: the pair carries the
       * centre texel's alpha through and touches the colour only. The colour itself is blurred by
       * that pair, the CPU's own order -- the non-enclosed texels are empty before it, so the
       * fill's edge fades out without the border's own colours smearing in, which is why this tail
       * runs ahead of the pair and not behind it. */
      const float4 field = texelFetch(srt.aux_tx, texel, 0);
      const bool enclosed = (field.z != 0.0f) && !(field.x == 0.0f && field.y == 0.0f);
      if (enclosed) {
        rgb = (srt.pass_params.x != 0.0f) ? srt.pass_params2.rgb : source.rgb;
      }
      else if (srt.alpha_from_kernel != 0) {
        rgb = float3(0.0f);
      }
      if (srt.alpha_from_kernel != 0) {
        const int2 field_size = textureSize(srt.aux_tx, 0);
        const int lo_x = max(texel.x - srt.radius, 0);
        const int hi_x = min(texel.x + srt.radius, field_size.x - 1);
        const int lo_y = max(texel.y - srt.radius, 0);
        const int hi_y = min(texel.y + srt.radius, field_size.y - 1);
        float coverage = 0.0f;
        for (int y = lo_y; y <= hi_y; y++) {
          for (int x = lo_x; x <= hi_x; x++) {
            const float4 neighbour = texelFetch(srt.aux_tx, int2(x, y), 0);
            coverage += ((neighbour.z != 0.0f) && !(neighbour.x == 0.0f && neighbour.y == 0.0f)) ?
                            1.0f :
                            0.0f;
          }
        }
        const float window = float((hi_x - lo_x + 1) * (hi_y - lo_y + 1));
        computed_alpha = clamp(coverage / window, 0.0f, 1.0f);
      }
      break;
    }
    default:
      /* An unknown kind leaves the texel as it came: the host only queues a kind the plan filled. */
      break;
  }
  /* No kernel touches the alpha unless it lays a layer of its own, and the buffer that holds the
   * result carries the content alpha the row went in with -- kept or dropped to one, exactly as the
   * CPU's buffer is written. A pass whose plan set #alpha_from_kernel keeps the alpha its kernel
   * computed instead. */
  const float alpha = (srt.alpha_from_kernel != 0) ? computed_alpha : pass_alpha;
  imageStore(srt.result_img, texel, float4(rgb, alpha));
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Folder Pass
 *
 * A folder composites its children in isolation over nothing (`composite_folder_accumulate`): the
 * pre-multiplied colour `P` and the coverage `a` start at zero, and each child lays itself over
 * them. With `S = P / a` the straight colour so far and `f` the child's factor:
 *
 *   c_eff = lerp(c, blend(S, c), a)   (where nothing is below yet a child is its own colour)
 *   P'    = lerp(P, c_eff, f)
 *   a'    = a + f * (1 - a)
 *
 * The result, `S` and `a`, is what the folder's own row then reads like a map and its alpha.
 * \{ */

#define COMBINED_FOLDER_INIT 0
#define COMBINED_FOLDER_BLEND 1
#define COMBINED_FOLDER_RESOLVE 2

struct FolderResources {
  /** The mask factor of the child being laid, read when #COMBINED_LAYER_HAS_MASK is set. */
  [[sampler(0)]] sampler2D mask_tx;
  /** The colour and coverage of the child, or of the folder's own row after the resolve. */
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D row_color_img;
  [[image(1, read_write, SFLOAT_16_16_16_16)]] image2D row_factor_img;
  /** The folder's pre-multiplied colour and coverage. */
  [[image(2, read_write, SFLOAT_16_16_16_16)]] image2D premul_img;
  [[image(3, read_write, SFLOAT_16_16_16_16)]] image2D cover_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  [[push_constant]] const float opacity;
  [[push_constant]] const int flags;
  [[push_constant]] const int blend;
  /** A `COMBINED_FOLDER_*` step. */
  [[push_constant]] const int mode;
};

[[compute, local_size(16, 16)]]
void folder_comp([[resource_table]] FolderResources &srt,
                 [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (any(greaterThanEqual(texel, srt.texture_size))) {
    return;
  }
  if (srt.mode == COMBINED_FOLDER_INIT) {
    imageStore(srt.premul_img, texel, float4(0.0f));
    imageStore(srt.cover_img, texel, float4(0.0f));
    return;
  }
  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    return;
  }

  float4 premul = imageLoad(srt.premul_img, texel);
  float a = imageLoad(srt.cover_img, texel).x;

  if (srt.mode == COMBINED_FOLDER_RESOLVE) {
    float4 straight = (a > 0.0f) ? premul / a : float4(0.0f);
    imageStore(srt.row_color_img, texel, straight);
    imageStore(srt.row_factor_img, texel, float4(a));
    return;
  }

  /* A child: its colour and the factor it blends by, mask included. */
  float4 c = imageLoad(srt.row_color_img, texel);
  float factor = imageLoad(srt.row_factor_img, texel).x;
  if ((srt.flags & COMBINED_LAYER_HAS_MASK) != 0) {
    factor *= texelFetch(srt.mask_tx, texel, 0).x;
  }
  float f = clamp(srt.opacity * factor, 0.0f, 1.0f);

  float4 straight = (a > 0.0f) ? premul / a : float4(0.0f);
  /* The row's blend at full factor, then the mix towards its own colour by what is there. */
  float4 blended = straight;
  if (srt.blend == COMBINED_BLEND_NORMAL_COMBINE) {
    blended.xyz = normal_combine(straight.xyz, c.xyz);
  }
  else if (srt.blend == COMBINED_BLEND_MIX) {
    blended = c;
  }
  else {
    blended.xyz = blend_rgb(srt.blend, straight.xyz, c.xyz, 1.0f);
  }
  float4 c_eff = c + (blended - c) * a;
  imageStore(srt.premul_img, texel, premul * (1.0f - f) + c_eff * f);
  imageStore(srt.cover_img, texel, float4(a + f * (1.0f - a)));
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Display Pass
 *
 * Shows one channel's accumulator as it is, for the channel passes of the Image Editor.
 * \{ */

struct DisplayResources {
  [[sampler(0)]] sampler2D channel_tx;
  [[image(0, write, SFLOAT_16_16_16_16)]] image2D output_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
};

[[compute, local_size(16, 16)]]
void display_comp([[resource_table]] DisplayResources &srt,
                  [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (any(greaterThanEqual(texel, srt.texture_size))) {
    return;
  }
  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    imageStore(srt.output_img, texel, float4(0.0f));
    return;
  }
  /* Clamped like the byte composite the CPU shows. */
  imageStore(srt.output_img,
             texel,
             clamp(texelFetch(srt.channel_tx, texel, 0), float4(0.0f), float4(1.0f)));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shading Pass
 * \{ */

struct ShadeResources {
  [[sampler(0)]] sampler2D base_tx;
  [[sampler(1)]] sampler2D metallic_tx;
  [[sampler(2)]] sampler2D roughness_tx;
  [[sampler(3)]] sampler2D specular_tx;
  [[sampler(4)]] sampler2D normal_tx;
  [[sampler(5)]] sampler2D ao_tx;
  [[sampler(6)]] sampler2D alpha_tx;
  [[sampler(7)]] sampler2D emission_tx;
  [[image(0, write, SFLOAT_16_16_16_16)]] image2D output_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  /** One light per column: the direction, the diffuse colour, and the specular colour with the
   * wrap in its fourth component. A matrix because a push constant array is not one BSL declares
   * yet. */
  [[push_constant]] const float4x4 light_direction;
  [[push_constant]] const float4x4 light_diffuse;
  [[push_constant]] const float4x4 light_specular;
  [[push_constant]] const float4 ambient_color;
  [[push_constant]] const int lights_num;
  [[push_constant]] const int normal_is_encoded;
  [[push_constant]] const float exposure;
  [[push_constant]] const float ao_influence;
  [[push_constant]] const float emission_strength;
};

/** GGX normal distribution, `alpha` being the squared roughness. */
float ggx_d(float n_dot_h, float alpha)
{
  float alpha2 = alpha * alpha;
  float denom = n_dot_h * n_dot_h * (alpha2 - 1.0f) + 1.0f;
  return alpha2 / max(M_PI * denom * denom, 1e-8f);
}

/** Smith height-correlated visibility, Schlick form, `k` being half the squared roughness. */
float smith_g(float n_dot_v, float n_dot_l, float k)
{
  float gv = n_dot_v / max(n_dot_v * (1.0f - k) + k, 1e-6f);
  float gl = n_dot_l / max(n_dot_l * (1.0f - k) + k, 1e-6f);
  return gv * gl;
}

[[compute, local_size(16, 16)]]
void shade_comp([[resource_table]] ShadeResources &srt,
                [[global_invocation_id]] const uint3 global_id)
{
  int2 texel = int2(global_id.xy);
  if (any(greaterThanEqual(texel, srt.texture_size))) {
    return;
  }

  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    imageStore(srt.output_img, texel, float4(0.0f));
    return;
  }

  /* The CPU shades the color channels from their float composites, which hold the straight
   * scene-linear evaluation unclamped -- so a stack that overshoots (Add, Exposure) keeps its HDR
   * value here too, and the view transform, not this pass, decides what a value above one looks
   * like. The scalar channels are byte composites of a [0, 1] range on both sides, so their clamps
   * mirror the CPU exactly. */
  float4 base = texelFetch(srt.base_tx, texel, 0);
  float metallic = clamp(texelFetch(srt.metallic_tx, texel, 0).x, 0.0f, 1.0f);
  float roughness = clamp(texelFetch(srt.roughness_tx, texel, 0).x, 0.0f, 1.0f);
  float specular = clamp(texelFetch(srt.specular_tx, texel, 0).x, 0.0f, 1.0f);
  float ao_map = clamp(texelFetch(srt.ao_tx, texel, 0).x, 0.0f, 1.0f);
  float alpha = clamp(texelFetch(srt.alpha_tx, texel, 0).x, 0.0f, 1.0f);
  float4 emission = texelFetch(srt.emission_tx, texel, 0);

  float4 encoded = clamp(texelFetch(srt.normal_tx, texel, 0), float4(0.0f), float4(1.0f));
  float3 normal = (srt.normal_is_encoded != 0) ? encoded.xyz * 2.0f - 1.0f : encoded.xyz;
  normal = (dot(normal, normal) > 0.0f) ? normalize(normal) : float3(0.0f, 0.0f, 1.0f);

  float3 base_rgb = base.xyz;
  float3 diffuse_color = base_rgb * (1.0f - metallic);
  float3 f0 = mix(float3(0.08f * specular), base_rgb, metallic);
  float ao = mix(1.0f, ao_map, srt.ao_influence);

  float alpha_ggx = max(roughness * roughness, 1e-4f);
  float smith_k = alpha_ggx * 0.5f;
  float3 view_dir = float3(0.0f, 0.0f, 1.0f);
  float n_dot_v = max(dot(normal, view_dir), 1e-4f);

  float3 lit = float3(0.0f);
  for (int i = 0; i < srt.lights_num; i++) {
    float3 light_dir = srt.light_direction[i].xyz;
    float wrap = srt.light_specular[i].w;
    float n_dot_l_raw = dot(normal, light_dir);

    float n_dot_l_wrapped = max((n_dot_l_raw + wrap) / (1.0f + wrap), 0.0f);
    lit += diffuse_color * srt.light_diffuse[i].xyz * n_dot_l_wrapped;

    float n_dot_l = max(n_dot_l_raw, 0.0f);
    if (n_dot_l <= 0.0f) {
      continue;
    }
    float3 half_vector = normalize(light_dir + view_dir);
    float n_dot_h = max(dot(normal, half_vector), 0.0f);
    float v_dot_h = max(dot(view_dir, half_vector), 0.0f);

    float d = ggx_d(n_dot_h, alpha_ggx);
    /* Schlick's fifth power by multiplication. */
    float t = 1.0f - v_dot_h;
    float t2 = t * t;
    float t5 = t2 * t2 * t;
    float3 fresnel = f0 + (float3(1.0f) - f0) * t5;
    float g = smith_g(n_dot_v, n_dot_l, smith_k);

    lit += srt.light_specular[i].xyz * fresnel * (d * g * n_dot_l);
  }

  lit += (diffuse_color + f0) * srt.ambient_color.xyz * ao;
  lit += emission.xyz * srt.emission_strength;
  lit *= srt.exposure;

  /* The half-float display texture: a specular spike must not become infinity. */
  lit = min(lit, float3(65504.0f));
  imageStore(srt.output_img, texel, float4(lit, alpha));
}

/** \} */

}  // namespace image_engine::combined

PipelineCompute image_engine_combined_layer(image_engine::combined::layer_comp);
PipelineCompute image_engine_combined_mask(image_engine::combined::mask_comp);
PipelineCompute image_engine_combined_correction(image_engine::combined::correction_comp);
PipelineCompute image_engine_combined_effect(image_engine::combined::effect_comp);
PipelineCompute image_engine_combined_folder(image_engine::combined::folder_comp);
PipelineCompute image_engine_combined_display(image_engine::combined::display_comp);
PipelineCompute image_engine_combined_shade(image_engine::combined::shade_comp);
