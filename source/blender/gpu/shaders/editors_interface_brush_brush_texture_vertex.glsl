/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/brush_texture_shader_infos.hh"

VERTEX_SHADER_CREATE_INFO(brush_texture_element)

void main()
{
  /* Unit quad corners (-1..1) scaled to element size, rotated, then translated. */
  float2 local_pos = pos * u_element_scale * 0.5f;
  float cos_rot = cos(u_element_rotation);
  float sin_rot = sin(u_element_rotation);
  float2 rotated_pos = float2(local_pos.x * cos_rot - local_pos.y * sin_rot,
                              local_pos.x * sin_rot + local_pos.y * cos_rot);
  float2 world_pos = rotated_pos + u_element_center;

  gl_Position = ModelViewProjectionMatrix * float4(world_pos, 0.0f, 1.0f);

  /* Forward the (optionally transformed) texture coordinates to the fragment stage. */
  float4 tex_coord_transformed = u_texture_transform * float4(texcoord, 0.0f, 1.0f);
  texCoord_interp = tex_coord_transformed.xy;
}
