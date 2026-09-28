/* SPDX-FileCopyrightText: 2017-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_curve_point)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  /* Reuse the FREESTYLE flag to determine is GPencil. */
  bool is_gpencil = ((data & EDGE_FREESTYLE) != 0u);
  if ((data & VERT_SELECTED) != 0u) {
    if ((data & VERT_ACTIVE) != 0u) {
      final_color = theme.colors.edit_mesh_active;
    }
    else {
      final_color = (!is_gpencil) ? theme.colors.vert_select : theme.colors.gpencil_vertex_select;
    }
  }
  else {
    final_color = (!is_gpencil) ? theme.colors.vert : theme.colors.gpencil_vertex;
  }

  /* Proportional editing influence highlight (see #transform_proportional_viz.cc). Blends the
   * point color towards the heat-map, fading in from the falloff radius, so the point itself
   * (its alpha) is never affected. */
  if (prop_edit_viz_enabled && falloff > 0.0f) {
    const float blend = smoothstep(0.0f, 0.25f, falloff) * prop_edit_viz_opacity;
    final_color.rgb = mix(final_color.rgb,
                            prop_falloff_heat_color(falloff,
                                                    theme.colors.prop_falloff_low,
                                                    theme.colors.prop_falloff_mid,
                                                    theme.colors.prop_falloff_high),
                            blend);
  }

  float3 world_pos = drw_point_object_to_world(pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);
  gl_PointSize = (!is_gpencil) ? theme.sizes.vert * 2.0f : theme.sizes.vertex_gpencil * 2.0f;
  view_clipping_distances(world_pos);

  bool show_handle = show_curve_handles;
  if ((uint(curve_handle_display) == CURVE_HANDLE_SELECTED) &&
      ((data & VERT_SELECTED_BEZT_HANDLE) == 0u))
  {
    show_handle = false;
  }

  if (!show_handle && ((data & BEZIER_HANDLE) != 0u)) {
    /* We set the vertex at the camera origin to generate 0 fragments. */
    gl_Position = float4(0.0f, 0.0f, -3e36f, 0.0f);
  }
}
