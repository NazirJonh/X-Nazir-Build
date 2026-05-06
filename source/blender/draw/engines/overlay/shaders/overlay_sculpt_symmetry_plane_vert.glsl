/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_sculpt_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_sculpt_symmetry_plane)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  /* Apply transformations for size and rotation */
  float3 transformed_pos = pos;
  
  /* Scale the plane by 2x */
  transformed_pos *= 2.0f;
  
  /* Rotate 90 degrees around Y axis */
  float cos_angle = 0.0f;  /* cos(90°) = 0 */
  float sin_angle = 1.0f;  /* sin(90°) = 1 */
  
  float3 rotated_pos;
  rotated_pos.x = cos_angle * transformed_pos.x - sin_angle * transformed_pos.z;
  rotated_pos.y = transformed_pos.y;
  rotated_pos.z = sin_angle * transformed_pos.x + cos_angle * transformed_pos.z;
  
  float3 world_pos = drw_point_object_to_world(rotated_pos);
  gl_Position = drw_point_world_to_homogenous(world_pos);

  view_clipping_distances(world_pos);
}