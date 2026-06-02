/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"
#include "draw_view_infos.hh"
#include "infos/overlay_common_infos.hh"
#include "overlay_shader_shared.hh"

SHADER_LIBRARY_CREATE_INFO(draw_modelmat_common)
SHADER_LIBRARY_CREATE_INFO(draw_resource_id)
SHADER_LIBRARY_CREATE_INFO(draw_globals)
SHADER_LIBRARY_CREATE_INFO(draw_view)

#include "draw_model_lib.glsl"
#include "draw_view_clipping_lib.glsl"
#include "draw_view_lib.glsl"

namespace overlay {

struct CurvesVertexPaintResources {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat;
  [[legacy_info]] ShaderCreateInfo draw_globals;

  [[push_constant]] const float opacity;
  [[push_constant]] const float3 light_dir;

  [[compilation_constant]] const bool use_fake_shading;
  [[compilation_constant]] const bool use_clipping;
};

struct CurvesVertexPaintVertOut {
  [[smooth]] float4 color;
  [[smooth]] float color_fac;
};

struct CurvesVertexPaintVertIn {
  [[attribute(0)]] float4 vert_color;
  [[attribute(1)]] float3 pos;
  [[attribute(2)]] float3 tangent;
};

[[vertex]] void curves_vertex_paint_vert(
    [[resource_table]] CurvesVertexPaintResources &srt,
    [[in]] const CurvesVertexPaintVertIn &v_in,
    [[out]] CurvesVertexPaintVertOut &v_out,
    [[position]] float4 &position,
    [[point_size]] float &point_size)
{
  position = drw_point_object_to_homogenous(v_in.pos);
  float3 world_pos = drw_point_object_to_world(v_in.pos);

  point_size = 4.0f;
  v_out.color = v_in.vert_color;

  if (srt.use_fake_shading) [[static_branch]] {
    float3 view_tangent = normalize(drw_normal_object_to_view(v_in.tangent));
    v_out.color_fac = abs(dot(view_tangent, srt.light_dir));
    v_out.color_fac = v_out.color_fac * 0.9f + 0.1f;
  }
  else {
    v_out.color_fac = 1.0f;
  }

  if (srt.use_clipping) [[static_branch]] {
    view_clipping_distances(world_pos);
  }
}

float4 curves_vertex_paint_apply_color_fac(float4 color_in, float color_fac)
{
  float4 color = color_in;
  color.rgb = max(float3(0.005f), color_in.rgb) * color_fac;
  return color;
}

struct CurvesVertexPaintFragOut {
  [[frag_color(0)]] float4 frag_color;
  [[frag_color(1)]] float4 line_output;
};

[[fragment]] void curves_vertex_paint_frag([[resource_table]] CurvesVertexPaintResources &srt,
                                           [[in]] const CurvesVertexPaintVertOut &v_in,
                                           [[out]] CurvesVertexPaintFragOut &frag_out)
{
  float4 color = curves_vertex_paint_apply_color_fac(v_in.color, v_in.color_fac);
  frag_out.frag_color = float4(color.rgb, srt.opacity * color.a);
  frag_out.line_output = float4(0.0f);
}

/* clang-format off */
PipelineGraphic curves_vertex_paint(
    curves_vertex_paint_vert, curves_vertex_paint_frag, CurvesVertexPaintResources{.use_fake_shading = false, .use_clipping = false});
PipelineGraphic curves_vertex_paint_clipped(
    curves_vertex_paint_vert, curves_vertex_paint_frag, CurvesVertexPaintResources{.use_fake_shading = false, .use_clipping = true});
PipelineGraphic curves_vertex_paint_fake_shading(
    curves_vertex_paint_vert, curves_vertex_paint_frag, CurvesVertexPaintResources{.use_fake_shading = true, .use_clipping = false});
PipelineGraphic curves_vertex_paint_fake_shading_clipped(
    curves_vertex_paint_vert, curves_vertex_paint_frag, CurvesVertexPaintResources{.use_fake_shading = true, .use_clipping = true});
/* clang-format on */

}  // namespace overlay
