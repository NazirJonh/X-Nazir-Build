/* SPDX-FileCopyrightText: 2018-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

#include "workbench_defines.hh"
#include "workbench_shader_shared.hh"

#define EPSILON 0.00001f

#define CAVITY_BUFFER_RANGE 4.0f

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

/* Encoding into the alpha of a RGBA16F texture. The 11 packed bits produce integer codes in
 * [0..2047], and an FP16 value represents every integer up to 2048 exactly, so the round-trip
 * through the material buffer stays bit-exact.
 *
 * Poly Paint makes Metallic a genuinely continuous per-vertex value (not just a single
 * per-material constant), so it needs more than the 3 bits (8 levels) it had - a gradient across
 * those 8 levels is visible as a hard-edged staircase instead of a smooth falloff. Specular gives
 * up 1 bit to fund it, keeping the same 11-bit total (no G-buffer format change): Roughness keeps
 * its full 5-bit precision, Metallic doubles from 3 to 4 bits, Specular halves from 3 to 2. */
#define TARGET_BITCOUNT 11u
#define SPECULAR_BITS 2u /* Specular channel is less important. */
#define METALLIC_BITS 4u
#define ROUGHNESS_BITS (TARGET_BITCOUNT - METALLIC_BITS - SPECULAR_BITS)

/* Encode 3 float into 1 with the desired precision. */
float float_triplet_encode(float v1, float v2, float v3)
{
  // constexpr uint v1_mask = ~(0xFFFFFFFFu << ROUGHNESS_BITS);
  // constexpr uint v2_mask = ~(0xFFFFFFFFu << METALLIC_BITS);
  // constexpr uint v3_mask = ~(0xFFFFFFFFu << SPECULAR_BITS);
  /* Same as above because some compiler are very dumb and think we use medium int. */
  constexpr int v1_mask = 0x1F;
  constexpr int v2_mask = 0xF;
  constexpr int v3_mask = 0x3;
  int iv1 = int(v1 * float(v1_mask));
  int iv2 = int(v2 * float(v2_mask)) << int(ROUGHNESS_BITS);
  int iv3 = int(v3 * float(v3_mask)) << int(ROUGHNESS_BITS + METALLIC_BITS);
  return float(iv1 | iv2 | iv3);
}

void float_triplet_decode(float data, float &v1, float &v2, float &v3)
{
  constexpr int v1_mask = 0x1F;
  constexpr int v2_mask = 0xF;
  constexpr int v3_mask = 0x3;
  int idata = int(data);
  v1 = float(idata & v1_mask) * (1.0f / float(v1_mask));
  v2 = float((idata >> int(ROUGHNESS_BITS)) & v2_mask) * (1.0f / float(v2_mask));
  v3 = float(idata >> int(ROUGHNESS_BITS + METALLIC_BITS)) * (1.0f / float(v3_mask));
}

}  // namespace workbench
