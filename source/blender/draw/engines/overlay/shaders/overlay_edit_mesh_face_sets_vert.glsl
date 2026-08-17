/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_mesh_face_sets)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  float3 world_pos = drw_point_object_to_world(pos);
  float3 view_pos = drw_point_world_to_view(world_pos);
  gl_Position = drw_point_view_to_homogenous(view_pos);
  gl_Position.z += get_homogenous_z_offset(
      drw_view().winmat, view_pos.z, gl_Position.w, retopology_offset);

  const bool is_default = (face_set_color_in.a == 0.0f);
  const float3 rgb = is_default ?
                         (retopology_enabled ? theme.colors.face_retopology.rgb : float3(0.0f)) :
                         face_set_color_in.rgb;
  const float alpha = is_default ? (retopology_enabled ? face_sets_opacity : 0.0f) :
                                   face_sets_opacity;
  face_set_color = retopology_enabled ? float4(rgb, alpha) :
                                        float4(mix(float3(1.0f), rgb, alpha), 1.0f);
  view_clipping_distances(world_pos);
}
