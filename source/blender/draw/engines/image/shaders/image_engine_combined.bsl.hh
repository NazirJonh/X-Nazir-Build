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

float3 apply_filter(int filter_kind, float3 rgb, float4 p0, float4 p1)
{
  float3 result = rgb;
  switch (filter_kind) {
    case COMBINED_FILTER_INVERT:
      result = abs(rgb - 1.0f);
      break;
    case COMBINED_FILTER_LEVELS:
      result = float3(levels_channel(rgb.x, p0, p1),
                      levels_channel(rgb.y, p0, p1),
                      levels_channel(rgb.z, p0, p1));
      break;
    case COMBINED_FILTER_HSL: {
      float3 hsv = rgb_to_hsv3(rgb);
      float turned = hsv.x + p0.x + 0.5f;
      hsv.x = turned - floor(turned);
      hsv.y = clamp(hsv.y * p0.y, 0.0f, 1.0f);
      hsv.z = hsv.z * p0.z;
      result = max(hsv_to_rgb3(hsv), float3(0.0f));
      break;
    }
    default:
      break;
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Map Sampling
 * \{ */

/**
 * One straight, scene-linear texel of a map at the canvas \a uv, read through the row's UV mapping.
 *
 * `uv' = R * (uv * scale) + offset` is the transform of the Mapping node and of the CPU resample.
 * An axis that repeats wraps; one that clips reads transparent outside [0, 1). A tangent-space
 * Normal map has its vectors follow the mapping, as in `composite_resample_mapped`: rotation undone,
 * then flipped by the sign of the scale.
 */
float4 sample_map(sampler2D tx,
                  float2 uv,
                  int flags,
                  float2 mapping_offset,
                  float2 mapping_scale,
                  float2 mapping_rotation,
                  float lod)
{
  float2 sample_uv = uv;
  bool clipped = false;
  if ((flags & COMBINED_LAYER_MAPPED) != 0) {
    float2 scaled = uv * mapping_scale;
    sample_uv = float2(scaled.x * mapping_rotation.x - scaled.y * mapping_rotation.y,
                       scaled.x * mapping_rotation.y + scaled.y * mapping_rotation.x) +
                mapping_offset;
    bool repeat_u = (flags & COMBINED_LAYER_REPEAT_U) != 0;
    bool repeat_v = (flags & COMBINED_LAYER_REPEAT_V) != 0;
    if (repeat_u) {
      sample_uv.x -= floor(sample_uv.x);
    }
    if (repeat_v) {
      sample_uv.y -= floor(sample_uv.y);
    }
    clipped = (!repeat_u && (sample_uv.x < 0.0f || sample_uv.x >= 1.0f)) ||
              (!repeat_v && (sample_uv.y < 0.0f || sample_uv.y >= 1.0f));
  }
  float4 color = clipped ? float4(0.0f) : textureLod(tx, sample_uv, lod);
  if ((flags & COMBINED_LAYER_PREMULTIPLIED) != 0 && color.w > 0.0f) {
    color.xyz = color.xyz / color.w;
  }
  if ((flags & COMBINED_LAYER_TANGENT_NORMAL) != 0 && color.w > 0.0f) {
    float3 n = color.xyz * 2.0f - 1.0f;
    float rx = (mapping_scale.x < 0.0f) ? -1.0f : 1.0f;
    float ry = (mapping_scale.y < 0.0f) ? -1.0f : 1.0f;
    float c = mapping_rotation.x;
    float s = mapping_rotation.y;
    color.xyz = float3(0.5f * rx * (c * n.x + s * n.y) + 0.5f,
                       0.5f * ry * (-s * n.x + c * n.y) + 0.5f,
                       0.5f * n.z + 0.5f);
  }
  return color;
}

/** \} */

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

#define COMBINED_MASK_HAS_MAP 2
#define COMBINED_MASK_PREMULTIPLIED 8
#define COMBINED_MASK_MAPPED 32
#define COMBINED_MASK_REPEAT_U 64
#define COMBINED_MASK_REPEAT_V 128
#define COMBINED_MASK_INVERT 512
#define COMBINED_MASK_FROM_ALPHA 1024

struct MaskResources {
  [[sampler(0)]] sampler2D mask_tx;
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D mask_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  [[push_constant]] const float2 mapping_offset;
  [[push_constant]] const float2 mapping_scale;
  [[push_constant]] const float2 mapping_rotation;
  /** The filter's parameters in its schema order. */
  [[push_constant]] const float4 filter_params_0;
  [[push_constant]] const float4 filter_params_1;
  /** The grey of an item that has no map. */
  [[push_constant]] const float constant_gray;
  [[push_constant]] const float opacity;
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

  float2 uv = (srt.texel_to_uv * float4(float2(texel) + 0.5f, 0.0f, 1.0f)).xy;
  float m = imageLoad(srt.mask_img, texel).x;
  if (any(lessThan(uv, float2(0.0f))) || any(greaterThanEqual(uv, float2(1.0f)))) {
    /* Outside the canvas the layer pass writes nothing, so the factor is not read there. */
    imageStore(srt.mask_img, texel, float4(m));
    return;
  }

  if (srt.mode == COMBINED_MASK_ITEM) {
    float opacity = clamp(srt.opacity, 0.0f, 1.0f);
    if (srt.filter_kind != COMBINED_FILTER_NONE) {
      /* A filter works on the factor so far as a grey. */
      float grey = apply_filter(
                       srt.filter_kind, float3(m), srt.filter_params_0, srt.filter_params_1)
                       .x;
      m += (grey - m) * opacity;
    }
    else {
      float gray = clamp(srt.constant_gray, 0.0f, 1.0f);
      float weight = opacity;
      if ((srt.flags & COMBINED_MASK_HAS_MAP) != 0) {
        float4 c = sample_map(srt.mask_tx,
                              uv,
                              srt.flags,
                              srt.mapping_offset,
                              srt.mapping_scale,
                              srt.mapping_rotation,
                              srt.lod);
        gray = clamp((c.x + c.y + c.z) / 3.0f, 0.0f, 1.0f);
        weight = clamp(c.w, 0.0f, 1.0f) * opacity;
      }
      /* The grey only: a transparent texel is still one the item does not cover. */
      if ((srt.flags & COMBINED_MASK_INVERT) != 0) {
        gray = 1.0f - gray;
      }
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
    float4 c = sample_map(srt.mask_tx,
                          uv,
                          srt.flags,
                          srt.mapping_offset,
                          srt.mapping_scale,
                          srt.mapping_rotation,
                          srt.lod);
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
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D accum_img;
  /** The row's colour and coverage while corrections are laid over it. */
  [[image(1, read_write, SFLOAT_16_16_16_16)]] image2D row_color_img;
  [[image(2, read_write, SFLOAT_16_16_16_16)]] image2D row_factor_img;
  /** Output texel centre to canvas UV. An affine map, carried as a mat4 for the push constant. */
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  /** The row's flat colour while it has no map. */
  [[push_constant]] const float4 layer_color;
  /** The value the accumulator is seeded with by the pass that carries #COMBINED_LAYER_INIT. */
  [[push_constant]] const float4 init_color;
  [[push_constant]] const float2 mapping_offset;
  [[push_constant]] const float2 mapping_scale;
  /** Cosine and sine of the mapping rotation. */
  [[push_constant]] const float2 mapping_rotation;
  /** The adjustment's filter and its parameters in the schema order. */
  [[push_constant]] const float4 filter_params_0;
  [[push_constant]] const float4 filter_params_1;
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
      /* The stack so far, filtered; the row then lays it by its own blend, opacity and mask. */
      top = dst;
      top.xyz = apply_filter(srt.filter_kind, dst.xyz, srt.filter_params_0, srt.filter_params_1);
    }
    else if ((srt.flags & COMBINED_LAYER_HAS_MAP) != 0) {
      top = sample_map(srt.layer_tx,
                       uv,
                       srt.flags,
                       srt.mapping_offset,
                       srt.mapping_scale,
                       srt.mapping_rotation,
                       srt.lod);
    }
    if ((srt.flags & COMBINED_LAYER_ALPHA_COVERAGE) != 0) {
      coverage = clamp(top.w, 0.0f, 1.0f);
    }
    if ((srt.flags & COMBINED_LAYER_EMPTY_BASE) != 0) {
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
  [[image(0, read_write, SFLOAT_16_16_16_16)]] image2D row_color_img;
  [[image(1, read_write, SFLOAT_16_16_16_16)]] image2D row_factor_img;
  [[push_constant]] const float4x4 texel_to_uv;
  [[push_constant]] const int2 texture_size;
  /** A Fill correction's flat colour while it has no map. */
  [[push_constant]] const float4 constant_color;
  [[push_constant]] const float2 mapping_offset;
  [[push_constant]] const float2 mapping_scale;
  [[push_constant]] const float2 mapping_rotation;
  [[push_constant]] const float4 filter_params_0;
  [[push_constant]] const float4 filter_params_1;
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
  if (srt.filter_kind != COMBINED_FILTER_NONE) {
    float weight = clamp(srt.opacity, 0.0f, 1.0f);
    float3 filtered = apply_filter(srt.filter_kind, s.xyz, srt.filter_params_0, srt.filter_params_1);
    s.xyz = s.xyz + (filtered - s.xyz) * weight;
    imageStore(srt.row_color_img, texel, s);
    return;
  }

  float a = clamp(imageLoad(srt.row_factor_img, texel).x, 0.0f, 1.0f);
  float4 c = srt.constant_color;
  bool has_map = (srt.flags & COMBINED_LAYER_HAS_MAP) != 0;
  if (has_map) {
    c = sample_map(srt.correction_tx,
                   uv,
                   srt.flags,
                   srt.mapping_offset,
                   srt.mapping_scale,
                   srt.mapping_rotation,
                   srt.lod);
  }
  /* A flat colour covers by its opacity alone: its alpha is content alpha, no part of the factor. */
  float f = clamp(srt.opacity * (has_map ? c.w : 1.0f), 0.0f, 1.0f);

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

  /* The CPU shades the byte composite of each channel, so a stack that overshoots (Add, Subtract)
   * is already clamped to [0, 1] there; the accumulators here are not. */
  float4 base = clamp(texelFetch(srt.base_tx, texel, 0), float4(0.0f), float4(1.0f));
  float metallic = clamp(texelFetch(srt.metallic_tx, texel, 0).x, 0.0f, 1.0f);
  float roughness = clamp(texelFetch(srt.roughness_tx, texel, 0).x, 0.0f, 1.0f);
  float specular = clamp(texelFetch(srt.specular_tx, texel, 0).x, 0.0f, 1.0f);
  float ao_map = clamp(texelFetch(srt.ao_tx, texel, 0).x, 0.0f, 1.0f);
  float alpha = clamp(texelFetch(srt.alpha_tx, texel, 0).x, 0.0f, 1.0f);
  float4 emission = clamp(texelFetch(srt.emission_tx, texel, 0), float4(0.0f), float4(1.0f));

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
PipelineCompute image_engine_combined_folder(image_engine::combined::folder_comp);
PipelineCompute image_engine_combined_display(image_engine::combined::display_comp);
PipelineCompute image_engine_combined_shade(image_engine::combined::shade_comp);
