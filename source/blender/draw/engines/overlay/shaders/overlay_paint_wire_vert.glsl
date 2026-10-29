/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_paint_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_paint_wire)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  bool is_select = (paint_overlay_flag > 0) && use_select;
  bool is_hidden = (paint_overlay_flag < 0) && use_select;

  float3 world_pos = drw_point_object_to_world(pos);
  float3 view_pos = drw_point_world_to_view(world_pos);
  gl_Position = drw_point_view_to_homogenous(view_pos);
  /* Offset in view-space units, positive values move the wire towards the camera. A constant
   * view-space offset does not grow with the distance. A constant NDC offset does: with
   * perspective `ndc_z = A + B / view_z`, so it corresponds to a view-space shift of
   * `bias * z^2 / B`, which eventually exceeds the mesh thickness and lets the back-side edges
   * pass the depth test. The Fill wire gets a larger offset than the plain paint wire: it is an
   * independent overlay that must stay visible even when the viewport's own Wireframe overlay is
   * disabled. */
  constexpr float view_offset = 5e-4f;
  constexpr float select_view_offset = 1e-3f;
  float vs_offset = (is_select || use_wire_override) ? select_view_offset : view_offset;
  gl_Position.z += get_homogenous_z_offset(
      drw_view().winmat, view_pos.z, gl_Position.w, vs_offset);

  if (is_hidden) {
    gl_Position = float4(-2.0f, -2.0f, -2.0f, 1.0f);
  }

  constexpr float4 colSel = float4(1.0f);

  final_color = (is_select) ? colSel : theme.colors.wire;

  /* Weight paint needs a light color to contrasts with dark weights. */
  if (!use_select) {
    final_color = float4(1.0f, 1.0f, 1.0f, 0.3f);
  }

  /* The Fill brush opts out of the theme colors: its wire is a fixed alert color, so it reads
   * against any paint below. This branch must stay last: the fill wire always requests
   * `use_select == false` from the pass, so the white fallback above would otherwise win. */
  if (use_wire_override) {
    final_color = wire_override_color;
  }

  view_clipping_distances(world_pos);
}
