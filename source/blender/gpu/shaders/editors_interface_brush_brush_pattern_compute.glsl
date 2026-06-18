/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/brush_texture_shader_infos.hh"

COMPUTE_SHADER_CREATE_INFO(brush_texture_compute)

/* Pseudo-random value in [0, 1) from a 2D coordinate. */
float random(float2 st)
{
  return fract(sin(dot(st.xy, float2(12.9898f, 78.233f))) * 43758.5453123f);
}

/* Value noise. */
float noise(float2 st)
{
  float2 i = floor(st);
  float2 f = fract(st);

  float a = random(i);
  float b = random(i + float2(1.0f, 0.0f));
  float c = random(i + float2(0.0f, 1.0f));
  float d = random(i + float2(1.0f, 1.0f));

  float2 u = f * f * (3.0f - 2.0f * f);

  return mix(a, b, u.x) + (c - a) * u.y * (1.0f - u.x) + (d - b) * u.x * u.y;
}

float4 generate_sinusoidal_pattern(float2 uv, float time)
{
  float frequency = pattern_params.x * 0.1f;
  float amplitude = pattern_params.y;

  float wave = sin(uv.x * frequency * 6.28318f + time);
  float pattern = smoothstep(0.3f, 0.7f, wave * amplitude);

  return float4(pattern, pattern, pattern, pattern);
}

float4 generate_random_pattern(float2 uv, float seed)
{
  float scale = pattern_params.x * 0.01f;
  float density = pattern_params.y;

  float2 st = uv * scale;
  float noise_val = noise(st + seed);

  float pattern = step(1.0f - density, noise_val);
  return float4(pattern, pattern, pattern, pattern);
}

float4 generate_grid_pattern(float2 uv, float density)
{
  float scale = pattern_params.x * 0.1f;
  float2 grid = abs(fract(uv * scale) - 0.5f) / fwidth(uv * scale);
  float line = min(grid.x, grid.y);

  float pattern = (1.0f - smoothstep(0.0f, 1.0f, line)) * density;

  return float4(pattern, pattern, pattern, pattern);
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 size = imageSize(u_output_image);

  if (texel.x >= size.x || texel.y >= size.y) {
    return;
  }

  float2 uv = (float2(texel) + 0.5f) / float2(size);

  float4 input_color = texture(u_input_texture, uv);
  float4 pattern_color = float4(0.0f);

  float pattern_type = mod(pattern_params.z, 3.0f);
  if (pattern_type < 1.0f) {
    pattern_color = generate_sinusoidal_pattern(uv, pattern_params.w);
  }
  else if (pattern_type < 2.0f) {
    pattern_color = generate_random_pattern(uv, pattern_params.z);
  }
  else {
    pattern_color = generate_grid_pattern(uv, pattern_params.y);
  }

  float4 result = mix(input_color, pattern_color, pattern_color.a);
  imageStore(u_output_image, texel, result);
}
