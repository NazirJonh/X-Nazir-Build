/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Image selection mask boundary overlay: samples the selection mask as a 2D array texture (one
 * layer per UDIM tile) through the mesh's UV geometry and draws the mask boundary as a dashed
 * line via screen-space derivatives, so no CPU-side boundary extraction is needed.
 */

#pragma once

#include "gpu_shader_compat.hh"

namespace builtin::image_select_mask {

struct VertIn {
  [[attribute(0)]] float3 pos;
  [[attribute(1)]] float2 uv;
  [[attribute(2)]] float layer_in;
};

struct VertOut {
  [[smooth]] float2 uv_interp;
  [[flat]] float tile_layer;
};

struct FragOut {
  [[frag_color(0)]] float4 fragColor;
};

struct Resources {
  [[push_constant]] const float4x4 ModelViewProjectionMatrix;
  [[sampler(0)]] const sampler2DArray mask_tex;
  [[push_constant]] const float4 line_color;
  [[push_constant]] const float4 line_color2;
  [[push_constant]] const float dash_width;
  [[push_constant]] const float fill_alpha;
};

[[vertex]] void vert([[resource_table]] const Resources &srt,
                     [[in]] const VertIn &v_in,
                     [[position]] float4 &gl_Position,
                     [[out]] VertOut &interp)
{
  interp.uv_interp = v_in.uv;
  interp.tile_layer = v_in.layer_in;
  gl_Position = srt.ModelViewProjectionMatrix * float4(v_in.pos, 1.0f);
}

[[fragment]] void frag([[resource_table]] const Resources &srt,
                       [[frag_coord]] const float4 &frag_coord,
                       [[in]] const VertOut &interp,
                       [[out]] FragOut &frag_out)
{
  /* Tile-local sampling: the per-vertex layer is the tile the triangle belongs to, so only the
   * fractional part of the global UV addresses the mask. */
  float mask = texture(srt.mask_tex, float3(fract(interp.uv_interp), interp.tile_layer)).r;

  /* The mask boundary is where the sampled mask crosses its 0.5 threshold; the screen-space
   * derivative gives the local edge width so the line stays ~1 pixel at any zoom. The floor keeps
   * `smoothstep` defined: its edges must differ, and the derivative is exactly zero wherever the
   * mask is constant (undefined result, NaN alpha on some drivers). */
  float width = max(gpu_fwidth(mask), 1e-4f);
  float edge = smoothstep(0.5f - width, 0.5f + width, mask);
  float border = clamp(edge * (1.0f - edge) * 4.0f, 0.0f, 1.0f);

  float alpha = max(border, srt.fill_alpha * edge);
  if (alpha <= 0.002f) {
    gpu_discard_fragment();
  }

  /* Two-tone dash along the screen diagonal, matching the Image Editor's marching ants. */
  float dash = fract((frag_coord.x + frag_coord.y) / srt.dash_width);
  float4 col = mix(srt.line_color, srt.line_color2, step(0.5f, dash));
  frag_out.fragColor = float4(col.rgb, col.a * alpha);
}

}  // namespace builtin::image_select_mask

PipelineGraphic gpu_shader_image_select_mask(builtin::image_select_mask::vert,
                                             builtin::image_select_mask::frag);
