/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/brush_texture_shader_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(brush_blend_advanced)

float4 blend_normal(float4 base, float4 blend, float opacity)
{
  return mix(base, blend, opacity);
}

float4 blend_multiply(float4 base, float4 blend, float opacity)
{
  float4 result = base * blend;
  return mix(base, result, opacity);
}

float4 blend_screen(float4 base, float4 blend, float opacity)
{
  float4 result = 1.0f - (1.0f - base) * (1.0f - blend);
  return mix(base, result, opacity);
}

float4 blend_overlay(float4 base, float4 blend, float opacity)
{
  float4 result = mix(2.0f * base * blend,
                      1.0f - 2.0f * (1.0f - base) * (1.0f - blend),
                      step(0.5f, base));
  return mix(base, result, opacity);
}

float4 blend_soft_light(float4 base, float4 blend, float opacity)
{
  float4 result = mix(2.0f * base * blend + base * base * (1.0f - 2.0f * blend),
                      sqrt(base) * (2.0f * blend - 1.0f) + 2.0f * base * (1.0f - blend),
                      step(0.5f, blend));
  return mix(base, result, opacity);
}

void main()
{
  /* `screen_uv` comes from the full-screen vertex stage. */
  float2 uv = screen_uv;

  float4 base_color = texture(u_base_texture, uv);
  float4 blend_color = texture(u_blend_texture, uv);
  float4 mask_color = texture(u_mask_texture, uv);

  /* Select the blend mode (packed in `blend_params.y`). */
  float4 result;
  float mode = blend_params.y;
  float opacity = blend_params.z;

  if (mode < 0.5f) {
    result = blend_normal(base_color, blend_color, opacity);
  }
  else if (mode < 1.5f) {
    result = blend_multiply(base_color, blend_color, opacity);
  }
  else if (mode < 2.5f) {
    result = blend_screen(base_color, blend_color, opacity);
  }
  else if (mode < 3.5f) {
    result = blend_overlay(base_color, blend_color, opacity);
  }
  else {
    result = blend_soft_light(base_color, blend_color, opacity);
  }

  /* Apply the mask and the contrast factor (`blend_params.w`). */
  result.a *= mask_color.a;
  result = mix(float4(0.5f), result, blend_params.w);

  fragColor = result;
}
