/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_curves_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_curves_brush_highlight)

#include "draw_curves_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

float retrieve_brush_highlight(const curves::Point pt)
{
  return texelFetch(brush_highlight_tx, pt.point_id).r;
}

#if defined(GPU_NVIDIA) && defined(GPU_OPENGL)
/* WORKAROUND: Fix legacy driver compiler issue (see #148472). */
#  define const
#endif

void main()
{
  const curves::Point ls_pt = curves::point_get(uint(gl_VertexID));
  const curves::Point ws_pt = curves::object_to_world(ls_pt, drw_modelmat());
  float3 world_pos = curves::shape_point_get(ws_pt, drw_world_incident_vector(ws_pt.P)).P;

  gl_Position = drw_point_world_to_homogenous(world_pos);

  float highlight_factor = retrieve_brush_highlight(ws_pt);
  highlight_intensity = highlight_factor * brush_highlight_opacity;

  view_clipping_distances(world_pos);
}
