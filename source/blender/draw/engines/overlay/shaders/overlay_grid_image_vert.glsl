/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_grid_image)

#ifdef GLSL_CPP_STUBS
/* The values are provided by the shader create-info during GPU compilation. */
#  define tile_pos float3(0.0f)
#  define tile_scale float3(0.0f)
#endif

#include "draw_view_lib.glsl"

void main()
{
  /* `pos` contains the coordinates of a quad (-1..1), but the tile coordinates are (0..1). */
  float3 image_pos = pos * 0.5f + 0.5f;
  float3 line_start;
  float3 line_end;
  if (gl_VertexID < 2) {
    line_start = float3(-1.0f, -1.0f, 0.0f);
    line_end = float3(-1.0f, 1.0f, 0.0f);
  }
  else if (gl_VertexID < 4) {
    line_start = float3(-1.0f, 1.0f, 0.0f);
    line_end = float3(1.0f, 1.0f, 0.0f);
  }
  else if (gl_VertexID < 6) {
    line_start = float3(1.0f, 1.0f, 0.0f);
    line_end = float3(1.0f, -1.0f, 0.0f);
  }
  else {
    line_start = float3(1.0f, -1.0f, 0.0f);
    line_end = float3(-1.0f, -1.0f, 0.0f);
  }

  float4 line_start_clip = drw_point_world_to_homogenous(tile_scale *
                                                           (line_start * 0.5f + 0.5f) + tile_pos);
  float4 line_end_clip = drw_point_world_to_homogenous(tile_scale *
                                                         (line_end * 0.5f + 0.5f) + tile_pos);
  gl_Position = drw_point_world_to_homogenous(tile_scale * image_pos + tile_pos);

  /* Stage the line endpoints for the viewport anti-aliasing pass. */
  edge_start = ((line_start_clip.xy / line_start_clip.w) * 0.5f + 0.5f) *
               uniform_buf.size_viewport;
  edge_pos = ((line_end_clip.xy / line_end_clip.w) * 0.5f + 0.5f) * uniform_buf.size_viewport;
}
