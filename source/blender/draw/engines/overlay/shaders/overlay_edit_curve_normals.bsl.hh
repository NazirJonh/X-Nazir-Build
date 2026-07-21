/* SPDX-FileCopyrightText: 2018-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Curve normals overlay.
 *
 * Draws a short "V" shaped line at each evaluated curve point along the normal and tilt
 * direction. Shared by legacy curves (`draw_cache_impl_curve.cc`) and the new curves object
 * type (`draw_cache_impl_curves.cc`) in edit mode.
 *
 * The vertex shader emulates a geometry shader: each output line is expanded from a single
 * indexed point loaded from the position/radius/normal/tangent storage buffers.
 */

#pragma once

#include "gpu_shader_compat.hh"

#include "draw_view_infos.hh"
#include "gpu_index_load_infos.hh"
#include "infos/overlay_common_infos.hh"
#include "overlay_shader_shared.hh"

SHADER_LIBRARY_CREATE_INFO(gpu_index_buffer_load)
SHADER_LIBRARY_CREATE_INFO(draw_view)
SHADER_LIBRARY_CREATE_INFO(draw_modelmat)
SHADER_LIBRARY_CREATE_INFO(draw_globals)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"
#include "gpu_shader_attribute_load_lib.glsl"
#include "gpu_shader_index_load_lib.glsl"

namespace overlay::edit_curve {

struct VertOut {
  [[flat]] float4 final_color;
};

struct Resources {
  [[legacy_info]] ShaderCreateInfo gpu_index_buffer_load;
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat;
  [[legacy_info]] ShaderCreateInfo draw_globals;
  [[legacy_info]] ShaderCreateInfo drw_clipped;

  [[storage(0, read), frequency(GEOMETRY)]] const float (&pos)[];
  [[storage(1, read), frequency(GEOMETRY)]] const float (&rad)[];
  [[storage(2, read), frequency(GEOMETRY)]] const uint (&nor)[];
  [[storage(3, read), frequency(GEOMETRY)]] const uint (&tangent)[];

  [[push_constant]] const int2 gpu_attr_0;
  [[push_constant]] const int2 gpu_attr_1;
  [[push_constant]] const int2 gpu_attr_2;
  [[push_constant]] const int2 gpu_attr_3;
  [[push_constant]] const float normal_size;
  [[push_constant]] const bool use_hq_normals;

  /** WORKAROUND: This exact compilation constant is checked in Metal backend to enable clip
   * distances. */
  [[compilation_constant]] const bool use_clipping;
};

[[vertex]] void vert_main([[resource_table]] const Resources &srt,
                          [[vertex_id]] const int vert_id,
                          [[out]] VertOut &v_out,
                          [[position]] float4 &out_position)
{
  /* Line list primitive. */
  constexpr uint input_primitive_vertex_count = 2u;
  /* Line list primitive. */
  constexpr uint output_primitive_vertex_count = 2u;
  constexpr uint output_primitive_count = 2u;
  constexpr uint output_invocation_count = 1u;
  constexpr uint output_vertex_count_per_invocation = output_primitive_count *
                                                      output_primitive_vertex_count;
  constexpr uint output_vertex_count_per_input_primitive = output_vertex_count_per_invocation *
                                                           output_invocation_count;

  uint in_primitive_id = uint(vert_id) / output_vertex_count_per_input_primitive;
  uint in_primitive_first_vertex = in_primitive_id * input_primitive_vertex_count;

  uint v_i = gpu_index_load(in_primitive_first_vertex);
  float3 ls_P = gpu_attr_load_float3(srt.pos, srt.gpu_attr_0, v_i);
  float radius = srt.rad[gpu_attr_load_index(v_i, srt.gpu_attr_1)];
  float3 ls_N = srt.use_hq_normals ?
                    gpu_attr_load_short4_snorm(srt.nor, srt.gpu_attr_2, v_i).xyz :
                    gpu_attr_load_uint_1010102_snorm(srt.nor, srt.gpu_attr_2, v_i).xyz;
  float3 ls_T = srt.use_hq_normals ?
                    gpu_attr_load_short4_snorm(srt.tangent, srt.gpu_attr_3, v_i).xyz :
                    gpu_attr_load_uint_1010102_snorm(srt.tangent, srt.gpu_attr_3, v_i).xyz;

  if ((vert_id & 1) == 0) {
    float flip = ((vert_id & 2) == 0) ? -1.0f : 1.0f;
    ls_P += srt.normal_size * radius * (flip * ls_N - ls_T);
  }

  float3 world_pos = drw_point_object_to_world(ls_P);
  out_position = drw_point_world_to_homogenous(world_pos);

  v_out.final_color = theme.colors.wire_edit;

  if (srt.use_clipping) [[static_branch]] {
    view_clipping_distances(world_pos);
  }
}

struct FragOut {
  [[frag_color(0)]] float4 frag_color;
  [[frag_color(1)]] float4 line_output;
};

[[fragment]] void frag_main([[in]] const VertOut &v_out, [[out]] FragOut &frag_out)
{
  frag_out.frag_color = v_out.final_color;
  frag_out.line_output = float4(0.0f);
}

#ifndef GLSL_CPP_STUBS
/* clang-format off */
PipelineGraphic normals(        vert_main, frag_main, Resources{.use_clipping = false});
PipelineGraphic normals_clipped(vert_main, frag_main, Resources{.use_clipping = true });
/* clang-format on */
#endif

}  // namespace overlay::edit_curve
