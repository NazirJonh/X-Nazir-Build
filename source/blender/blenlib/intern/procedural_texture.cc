/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup BLI
 */

#include <algorithm>
#include <cmath>

#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_noise.hh"
#include "BLI_procedural_texture.hh"

namespace blender {

float procedural_checker_factor(float3 vector, float scale)
{
  /* Avoid precision issues on unit coordinates. */
  const float3 p = (vector * scale + 0.000001f) * 0.999999f;

  const int xi = std::abs(int(floorf(p.x)));
  const int yi = std::abs(int(floorf(p.y)));
  const int zi = std::abs(int(floorf(p.z)));

  return ((xi % 2 == yi % 2) == (zi % 2)) ? 1.0f : 0.0f;
}

float procedural_brick_noise(uint n)
{
  n = (n + 1013) & 0x7fffffff;
  n = (n >> 13) ^ n;
  const uint nn = (n * (n * n * 60493 + 19990303) + 1376312589) & 0x7fffffff;
  return 0.5f * (float(nn) / 1073741824.0f);
}

static float brick_smoothstepf(const float f)
{
  const float ff = f * f;
  return (3.0f * ff - 2.0f * ff * f);
}

float2 procedural_brick(float3 p,
                        float mortar_size,
                        float mortar_smooth,
                        float bias,
                        float brick_width,
                        float row_height,
                        float offset_amount,
                        int offset_frequency,
                        float squash_amount,
                        int squash_frequency)
{
  float offset = 0.0f;

  const int rownum = int(floorf(p.y / row_height));

  if (offset_frequency && squash_frequency) {
    brick_width *= (rownum % squash_frequency) ? 1.0f : squash_amount;
    offset = (rownum % offset_frequency) ? 0.0f : (brick_width * offset_amount);
  }

  const int bricknum = int(floorf((p.x + offset) / brick_width));

  const float x = (p.x + offset) - brick_width * bricknum;
  const float y = p.y - row_height * rownum;

  const float tint = clamp_f(procedural_brick_noise((rownum << 16) + (bricknum & 0xFFFF)) + bias,
                             0.0f,
                             1.0f);
  float min_dist = std::min({x, y, brick_width - x, row_height - y});

  float mortar;
  if (min_dist >= mortar_size) {
    mortar = 0.0f;
  }
  else if (mortar_smooth == 0.0f) {
    mortar = 1.0f;
  }
  else {
    min_dist = 1.0f - min_dist / mortar_size;
    mortar = (min_dist < mortar_smooth) ? brick_smoothstepf(min_dist / mortar_smooth) : 1.0f;
  }

  return float2(tint, mortar);
}

float procedural_brick_factor(float3 vector,
                              float scale,
                              float mortar_size,
                              float mortar_smooth,
                              float bias,
                              float brick_width,
                              float row_height,
                              float offset_amount,
                              int offset_frequency,
                              float squash_amount,
                              int squash_frequency)
{
  const float2 tint_and_mortar = procedural_brick(vector * scale,
                                                  mortar_size,
                                                  mortar_smooth,
                                                  bias,
                                                  brick_width,
                                                  row_height,
                                                  offset_amount,
                                                  offset_frequency,
                                                  squash_amount,
                                                  squash_frequency);
  /* The node's Color socket with Color1 white and Color2 and Mortar black: the two mix steps it
   * does reduce to these two products, written in its own order so the texels agree. */
  return (1.0f - tint_and_mortar.x) * (1.0f - tint_and_mortar.y);
}

float procedural_wave_factor(float3 vector,
                             float scale,
                             float distortion,
                             float detail,
                             float detail_scale,
                             float detail_roughness,
                             float phase_offset,
                             eProceduralWaveType wave_type,
                             int bands_direction,
                             int rings_direction,
                             eProceduralWaveProfile wave_profile)
{
  float3 p = vector * scale;
  /* Prevent precision issues on unit coordinates. */
  p = (p + 0.000001f) * 0.999999f;

  float n = 0.0f;

  switch (wave_type) {
    case PROC_WAVE_BANDS:
      switch (bands_direction) {
        case PROC_WAVE_BANDS_DIRECTION_X:
          n = p.x * 20.0f;
          break;
        case PROC_WAVE_BANDS_DIRECTION_Y:
          n = p.y * 20.0f;
          break;
        case PROC_WAVE_BANDS_DIRECTION_Z:
          n = p.z * 20.0f;
          break;
        case PROC_WAVE_BANDS_DIRECTION_DIAGONAL:
          n = (p.x + p.y + p.z) * 10.0f;
          break;
      }
      break;
    case PROC_WAVE_RINGS:
      float3 rp = p;
      switch (rings_direction) {
        case PROC_WAVE_RINGS_DIRECTION_X:
          rp *= float3(0.0f, 1.0f, 1.0f);
          break;
        case PROC_WAVE_RINGS_DIRECTION_Y:
          rp *= float3(1.0f, 0.0f, 1.0f);
          break;
        case PROC_WAVE_RINGS_DIRECTION_Z:
          rp *= float3(1.0f, 1.0f, 0.0f);
          break;
        case PROC_WAVE_RINGS_DIRECTION_SPHERICAL:
          /* Ignore. */
          break;
      }
      n = len_v3(rp) * 20.0f;
      break;
  }

  n += phase_offset;

  if (distortion != 0.0f) {
    n += distortion *
         (noise::perlin_fbm<float3>(p * detail_scale, detail, detail_roughness, 2.0f, true) * 2.0f -
          1.0f);
  }

  switch (wave_profile) {
    case PROC_WAVE_PROFILE_SIN:
      return 0.5f + 0.5f * sinf(n - M_PI_2);
    case PROC_WAVE_PROFILE_SAW:
      n /= M_PI * 2.0f;
      return n - floorf(n);
    case PROC_WAVE_PROFILE_TRI:
      n /= M_PI * 2.0f;
      return fabsf(n - floorf(n + 0.5f)) * 2.0f;
  }
  return 0.0f;
}

}  // namespace blender
