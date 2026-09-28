/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_curves_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_curves_influence)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);

  /* The gradient only shows when the matching toggle is on; the flat hit color is independent. */
  const float gradient_factor = show_influence ? smoothstep(0.0f, 0.25f, influence) : 0.0f;
  const float3 gradient = prop_falloff_heat_color(influence,
                                                  theme.colors.prop_falloff_low,
                                                  theme.colors.prop_falloff_mid,
                                                  theme.colors.prop_falloff_high);
  if (show_hover_hit && curve_hit > 0.0f) {
    /* Solid hover-hit color under the gradient: the whole touched curve stays visible while the
     * stroke runs, with the heat-map fading in on the weighted points. */
    final_color = float4(mix(theme.colors.curves_hover_hit.rgb, gradient, gradient_factor),
                         influence_opacity);
  }
  else {
    final_color = float4(gradient, gradient_factor * influence_opacity);
  }

  /* Convert to screen position [0..sizeVp], for the same AA line resolve the cage uses. */
  edge_pos = edge_start = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;

  /* Curves the stroke/hover does not touch (curve_hit == 0) are fully transparent by
   * construction; cull them entirely instead of writing zero-alpha line data that the AA resolve
   * would still thicken. */
  if (curve_hit <= 0.0f) {
    gl_Position = float4(0.0f, 0.0f, -3e36f, 0.0f);
    return;
  }

  gl_PointSize = theme.sizes.vert * 2.0f;
  view_clipping_distances(world_pos);
}
