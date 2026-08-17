/* SPDX-FileCopyrightText: 2018-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

#include "workbench_defines.hh"
#include "workbench_shader_shared.hh"

#define EPSILON 0.00001f

#define CAVITY_BUFFER_RANGE 4.0f

/**
 * Poly Paint: neutral value of the per-vertex Specular channel, i.e. what every object that is not
 * painting it writes.
 *
 * #get_world_lighting maps Specular to dielectric F0 as `0.08 * specular`, the same way Principled
 * BSDF's "IOR Level" does. 0.625f is the value for which that yields exactly the fixed 0.05f
 * reflectance Workbench used before per-vertex Specular existed, so an unpainted scene shades
 * bit-for-bit as it did before. Do not "round" this to 0.5f: that is the Principled default, not
 * Workbench's, and would darken the specular of every existing scene.
 */
namespace workbench {

struct World {
  [[uniform(WB_WORLD_SLOT)]] const WorldData &world_data;
};

/* From http://aras-p.info/texts/CompactNormalStorage.html
 * Using Method #4: Sphere-map Transform */
float3 normal_decode(float4 enc)
{
  float2 fenc = enc.xy * 4.0f - 2.0f;
  float f = dot(fenc, fenc);
  float g = sqrt(1.0f - f / 4.0f);
  float3 n;
  n.xy = fenc * g;
  n.z = 1 - f / 2;
  return n;
}

/* From http://aras-p.info/texts/CompactNormalStorage.html
 * Using Method #4: Sphere-map Transform */
float2 normal_encode(bool front_face, float3 n)
{
  n = normalize(front_face ? n : -n);
  float p = sqrt(n.z * 8.0f + 8.0f);
  n.xy = clamp(n.xy / p + 0.5f, 0.0f, 1.0f);
  return n.xy;
}

/* Encoding into the alpha of a RGBA16F texture. (10bit mantissa)
 *
 * NOTE: Poly Paint deliberately does NOT change this packing. Squeezing a third value in here
 * would have to come out of Roughness's or Metallic's bits, and 2 bits cannot even represent the
 * unpainted Specular default of 0.5f - every ordinary Workbench scene would shift its dielectric
 * reflectance as a side effect. Painted objects get full float precision from #material_ext_tx
 * instead (see #WORKBENCH_MATERIAL_EXT_DEFAULT_SPECULAR), which is exactly what that texture is
 * for, so the packed path stays byte-for-byte what it was before Poly Paint. */
#define TARGET_BITCOUNT 8u
#define METALLIC_BITS 3u /* Metallic channel is less important. */
#define ROUGHNESS_BITS (TARGET_BITCOUNT - METALLIC_BITS)

/* Encode 2 float into 1 with the desired precision. */
float float_pair_encode(float v1, float v2)
{
  // constexpr uint v1_mask = ~(0xFFFFFFFFu << ROUGHNESS_BITS);
  // constexpr uint v2_mask = ~(0xFFFFFFFFu << METALLIC_BITS);
  /* Same as above because some compiler are very dumb and think we use medium int. */
  constexpr int v1_mask = 0x1F;
  constexpr int v2_mask = 0x7;
  int iv1 = int(v1 * float(v1_mask));
  int iv2 = int(v2 * float(v2_mask)) << int(ROUGHNESS_BITS);
  return float(iv1 | iv2);
}

void float_pair_decode(float data, float &v1, float &v2)
{
  // constexpr uint v1_mask = ~(0xFFFFFFFFu << ROUGHNESS_BITS);
  // constexpr uint v2_mask = ~(0xFFFFFFFFu << METALLIC_BITS);
  /* Same as above because some compiler are very dumb and think we use medium int. */
  constexpr int v1_mask = 0x1F;
  constexpr int v2_mask = 0x7;
  int idata = int(data);
  v1 = float(idata & v1_mask) * (1.0f / float(v1_mask));
  v2 = float(idata >> int(ROUGHNESS_BITS)) * (1.0f / float(v2_mask));
}

}  // namespace workbench
