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

  /* Use the same Z-offset as the retopology overlay so both passes stay coincident. */
  gl_Position.z += get_homogenous_z_offset(
      drw_view().winmat, view_pos.z, gl_Position.w, retopology_offset);

  /* The color is computed on the CPU (see #extract_edit_face_set). The alpha channel is a flag:
   * zero marks the default face set, which is left transparent unless retopology is enabled, where
   * it reuses the retopology theme color to keep parity with the sculpt retopology view. */
  bool is_default = (face_set_color_in.a == 0.0f);
  float3 rgb = is_default ?
                   (retopology_enabled ? theme.colors.face_retopology.rgb : float3(0.0f)) :
                   face_set_color_in.rgb;
  float alpha = is_default ? (retopology_enabled ? face_sets_opacity : 0.0f) : face_sets_opacity;

  /* When retopology is enabled the base mesh is hidden, so the render target holds the background
   * (black). BLEND_MUL would multiply that to black, so the pass uses BLEND_ALPHA and outputs the
   * color directly with alpha.
   *
   * In normal mode (BLEND_MUL) the color is mixed from white towards the face set color, matching
   * Sculpt Mode. This yields the correct multiplicative result: `dst.rgb = src.rgb * dst.rgb`. */
  face_set_color = retopology_enabled ? float4(rgb, alpha) :
                                        float4(mix(float3(1.0f), rgb, alpha), 1.0f);

  view_clipping_distances(world_pos);
}
