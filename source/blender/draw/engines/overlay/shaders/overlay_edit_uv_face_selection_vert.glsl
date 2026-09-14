/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_uv_face_selection)

#include "draw_view_lib.glsl"

void main()
{
  /* The vertices hold the UVs of the active UV map; they are drawn through the Image Editor's view
   * matrix like the other UV overlays. */
  gl_Position = drw_point_world_to_homogenous(float3(au, 0.0f));

  /* Same rule as the 3D paint overlay (see `overlay_paint_face_vert.glsl`): the face selection is
   * drawn as a veil over the faces a masked stroke leaves out. Faces the flag marks as selected
   * (1) and hidden faces (-1) stay untouched. */
  float alpha = (paint_overlay_flag == 0) ? ucolor.a : 0.0f;
  final_color = float4(ucolor.rgb, alpha);
}
