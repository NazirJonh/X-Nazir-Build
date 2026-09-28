/* SPDX-FileCopyrightText: 2019-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_curves_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_curves_cage)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);
  /* Small bias to always be on top of the geom. */
  gl_Position.z -= 1e-3f;

  /* The hit curves are drawn solid by the influence pass; cull their cage segments so the two do
   * not fight in the AA resolve (all points of a hit curve carry curve_hit, and the line-strip
   * IBO breaks between curves, so exactly those segments are removed). */
  if (hide_hit_curves && curve_hit > 0.0f) {
    gl_Position = float4(0.0f, 0.0f, -3e36f, 0.0f);
    return;
  }

  final_color = float4(selection);
  final_color.a *= opacity;

  /* Convert to screen position [0..sizeVp]. */
  edge_pos = edge_start = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;
}
