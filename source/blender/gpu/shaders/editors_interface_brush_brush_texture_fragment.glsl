/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/brush_texture_shader_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(brush_texture_element)

void main()
{
  /* Apply the texture scale and offset. */
  float2 uv = texCoord_interp * u_texture_scale + u_texture_offset;

  /* Rotate the texture coordinates around their center. */
  if (u_texture_rotation != 0.0f) {
    float2 center = float2(0.5f, 0.5f);
    float2 centered = uv - center;
    float cos_rot = cos(u_texture_rotation);
    float sin_rot = sin(u_texture_rotation);
    uv = float2(centered.x * cos_rot - centered.y * sin_rot,
                centered.x * sin_rot + centered.y * cos_rot) +
         center;
  }

  float4 tex_color = texture(u_texture, uv);
  float4 mask_color = texture(u_mask_texture, uv);

  float4 final_color = tex_color;
  final_color.rgb *= u_color_tint.rgb;
  final_color.a *= u_opacity;

  /* Modulate alpha by the mask (a 1x1 white texture is bound when there is no mask). */
  if (mask_color.a > 0.0f) {
    final_color.a *= mask_color.r;
  }

  /* Basic blend-mode approximation for the preview. */
  if (u_blend_mode == 1) {
    /* Multiply. */
    final_color.rgb *= 0.5f;
  }
  else if (u_blend_mode == 2) {
    /* Screen. */
    final_color.rgb = 1.0f - (1.0f - final_color.rgb) * 0.5f;
  }

  fragColor = final_color;
}
