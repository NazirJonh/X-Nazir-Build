/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_extra_infos.hh"

/* Use only one for C++ linting to avoid ambiguity of 'pos', 'color', etc.
 * The dependency scanner will still see both. */
#ifdef __cplusplus
VERTEX_SHADER_CREATE_INFO(overlay_extra_wire_object_base)
#else
VERTEX_SHADER_CREATE_INFO(overlay_extra_wire_base)
VERTEX_SHADER_CREATE_INFO(overlay_extra_wire_object_base)
#endif
VERTEX_SHADER_CREATE_INFO(draw_modelmat)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "select_lib.glsl"

float2 screen_position(float4 p)
{
  return ((p.xy / p.w) * 0.5f + 0.5f) * uniform_buf.size_viewport;
}

void main()
{
#ifdef OBJECT_WIRE
  select_id_set(drw_custom_id());
#else
  select_id_set(in_select_buf[gl_InstanceID]);
#endif

#ifdef OBJECT_WIRE
  float3 world_pos = drw_point_object_to_world(pos);
#else
  /* 'pos' and 'color' are macros defined in 'overlay_extra_wire_base' info.
   * We need to use them via local variables to avoid C++ linting errors
   * when multiple info blocks are present. */
  float3 v_pos = pos;
  float4 v_color = color;
  float3 world_pos = drw_point_object_to_world(v_pos);
#endif
  gl_Position = drw_point_world_to_homogenous(world_pos);

#if defined(SELECT_ENABLE)
  /* HACK: to avoid losing sub-pixel object in selections, we add a bit of randomness to the
   * wire to at least create one fragment that will pass the occlusion query. */
  /* TODO(fclem): Limit this workaround to selection. It's not very noticeable but still... */
  gl_Position.xy += uniform_buf.size_viewport_inv * gl_Position.w *
                    ((gl_VertexID % 2 == 0) ? -1.0f : 1.0f);
#endif

  stipple_coord = stipple_start = screen_position(gl_Position);

#ifdef OBJECT_WIRE
  /* Extract data packed inside the unused float4x4 members. */
  final_color = float4(
      drw_modelmat()[0][3], drw_modelmat()[1][3], drw_modelmat()[2][3], drw_modelmat()[3][3]);
#else

  if (color_id_attr != 0) {
    /* TH_CAMERA_PATH is the only color code at the moment.
     * Checking `color_id_attr != 0` to avoid having to sync its value with the GLSL code. */
    final_color = theme.colors.camera_path;
    final_color.a = 0.0f; /* No Stipple */
  }
  else {
    final_color = v_color;
    final_color.a = 1.0f; /* Stipple */
  }
#endif

#if defined(SELECT_ENABLE)
  final_color.a = 0.0f; /* No Stipple */
#endif

  view_clipping_distances(world_pos);
}
