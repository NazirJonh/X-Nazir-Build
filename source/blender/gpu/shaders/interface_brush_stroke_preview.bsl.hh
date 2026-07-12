/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Brush stroke preview: renders a single brush-texture stamp as a transformed quad.
 * The UI widget instances this shader for every stamp placed along the previewed stroke.
 */

#pragma once

#include "gpu_shader_compat.hh"

#include "GPU_shader_shared.hh"

namespace interface::brush_stroke_preview {

struct VertIn {
  [[attribute(0)]] float2 pos;
  [[attribute(1)]] float2 texcoord;
};

struct VertOut {
  [[smooth]] float2 texCoord_interp;
};

struct FragOut {
  [[frag_color(0)]] float4 fragColor;
};

struct Resources {
  [[push_constant]] const float4x4 ModelViewProjectionMatrix;
  [[push_constant]] const float4x4 u_texture_transform;
  [[push_constant]] const float2 u_element_center;
  [[push_constant]] const float2 u_element_scale;
  [[push_constant]] const float u_element_rotation;
  [[push_constant]] const float4 u_color_tint;
  [[push_constant]] const float u_opacity;
  [[push_constant]] const int u_blend_mode;
  [[push_constant]] const float2 u_texture_scale;
  [[push_constant]] const float2 u_texture_offset;
  [[push_constant]] const float u_texture_rotation;
  [[sampler(0)]] const sampler2D u_texture;
  [[sampler(1)]] const sampler2D u_mask_texture;
};

[[vertex]] void vert([[resource_table]] const Resources &srt,
                     [[in]] const VertIn &v_in,
                     [[position]] float4 &gl_Position,
                     [[out]] VertOut &v_out)
{
  /* Unit quad corners (-1..1) scaled to element size, rotated, then translated. */
  float2 local_pos = v_in.pos * srt.u_element_scale * 0.5f;
  float cos_rot = cos(srt.u_element_rotation);
  float sin_rot = sin(srt.u_element_rotation);
  float2 rotated_pos = float2(local_pos.x * cos_rot - local_pos.y * sin_rot,
                              local_pos.x * sin_rot + local_pos.y * cos_rot);
  float2 world_pos = rotated_pos + srt.u_element_center;

  gl_Position = srt.ModelViewProjectionMatrix * float4(world_pos, 0.0f, 1.0f);

  /* Forward the (optionally transformed) texture coordinates to the fragment stage. */
  float4 tex_coord_transformed = srt.u_texture_transform * float4(v_in.texcoord, 0.0f, 1.0f);
  v_out.texCoord_interp = tex_coord_transformed.xy;
}

[[fragment]] void frag([[resource_table]] const Resources &srt,
                       [[in]] const VertOut &v_out,
                       [[out]] FragOut &frag_out)
{
  /* Apply the texture scale and offset. */
  float2 uv = v_out.texCoord_interp * srt.u_texture_scale + srt.u_texture_offset;

  /* Rotate the texture coordinates around their center. */
  if (srt.u_texture_rotation != 0.0f) {
    float2 center = float2(0.5f, 0.5f);
    float2 centered = uv - center;
    float cos_rot = cos(srt.u_texture_rotation);
    float sin_rot = sin(srt.u_texture_rotation);
    uv = float2(centered.x * cos_rot - centered.y * sin_rot,
                centered.x * sin_rot + centered.y * cos_rot) +
         center;
  }

  float4 tex_color = texture(srt.u_texture, uv);
  float4 mask_color = texture(srt.u_mask_texture, uv);

  float4 final_color = tex_color;
  final_color.rgb *= srt.u_color_tint.rgb;
  final_color.a *= srt.u_opacity;

  /* Modulate alpha by the mask (a 1x1 white texture is bound when there is no mask). */
  if (mask_color.a > 0.0f) {
    final_color.a *= mask_color.r;
  }

  /* Basic blend-mode approximation for the preview. */
  if (srt.u_blend_mode == 1) {
    /* Multiply. */
    final_color.rgb *= 0.5f;
  }
  else if (srt.u_blend_mode == 2) {
    /* Screen. */
    final_color.rgb = 1.0f - (1.0f - final_color.rgb) * 0.5f;
  }

  frag_out.fragColor = final_color;
}

}  // namespace interface::brush_stroke_preview

PipelineGraphic interface_brush_stroke_preview(interface::brush_stroke_preview::vert,
                                               interface::brush_stroke_preview::frag);
