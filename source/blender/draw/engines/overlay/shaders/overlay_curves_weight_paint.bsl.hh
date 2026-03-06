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

struct CurvesWeightPaintResources {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_modelmat;
  [[legacy_info]] ShaderCreateInfo draw_globals;

  [[sampler(0)]] const sampler1D colorramp;

  [[push_constant]] const float opacity;
  [[push_constant]] const bool draw_contours;
  [[push_constant]] const float3 light_dir;

  [[compilation_constant]] const bool use_fake_shading;
  [[compilation_constant]] const bool use_clipping;
};

struct CurvesWeightPaintVertOut {
  [[smooth]] float2 weight_interp;
  [[smooth]] float color_fac;
};

struct CurvesWeightPaintVertIn {
  [[attribute(0)]] float weight;
  [[attribute(1)]] float3 pos;
  [[attribute(2)]] float3 tangent;
};

[[vertex]] void curves_weight_paint_vert(
    [[resource_table]] CurvesWeightPaintResources &srt,
    [[in]] const CurvesWeightPaintVertIn &v_in,
    [[out]] CurvesWeightPaintVertOut &v_out,
    [[position]] float4 &position,
    [[point_size]] float &point_size)
{
  position = drw_point_object_to_homogenous(v_in.pos);
  float3 world_pos = drw_point_object_to_world(v_in.pos);

  /* Set point size for GPU_PRIM_POINTS primitive */
  point_size = 4.0f;

  /* Separate actual weight and alerts for independent interpolation */
  v_out.weight_interp = max(float2(v_in.weight, -v_in.weight), 0.0f);

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

float contours(float value, float steps, float width_px, float max_rel_width, float gradient)
{
  /* Minimum visible and minimum full strength line width in screen space for fade out. */
  constexpr float min_width_px = 1.3f, fade_width_px = 2.3f;
  /* Line is thinner towards the increase in the weight gradient by this factor. */
  constexpr float hi_bias = 2.0f;

  /* Don't draw lines at 0 or 1. */
  float rel_value = value * steps;

  if (rel_value < 0.5f || rel_value > steps - 0.5f) {
    return 0.0f;
  }

  /* Check if completely invisible due to fade out. */
  float rel_gradient = gradient * steps;
  float rel_min_width = min_width_px * rel_gradient;

  if (max_rel_width <= rel_min_width) {
    return 0.0f;
  }

  /* Main shape of the line, accounting for width bias and maximum weight space width. */
  float rel_width = width_px * rel_gradient;

  float offset = fract(rel_value + 0.5f) - 0.5f;

  float base_alpha = 1.0f - max(offset * hi_bias, -offset) / min(max_rel_width, rel_width);

  /* Line fade-out when too thin in screen-space. */
  float rel_fade_width = fade_width_px * rel_gradient;

  float fade_alpha = (max_rel_width - rel_min_width) / (rel_fade_width - rel_min_width);

  return clamp(base_alpha, 0.0f, 1.0f) * clamp(fade_alpha, 0.0f, 1.0f);
}

float4 contour_grid(float weight, float weight_gradient)
{
  /* Fade away when the gradient is too low to avoid big fills and noise. */
  float flt_eps = max(1e-8f, 1e-6f * weight);

  if (weight_gradient <= flt_eps) {
    return float4(0.0f);
  }

  /* Three levels of grid lines */
  float grid10 = contours(weight, 10.0f, 5.0f, 0.3f, weight_gradient);
  float grid100 = contours(weight, 100.0f, 3.5f, 0.35f, weight_gradient) * 0.6f;
  float grid1000 = contours(weight, 1000.0f, 2.5f, 0.4f, weight_gradient) * 0.25f;

  /* White lines for 0.1 and 0.01, and black for 0.001. */
  float4 grid = float4(1.0f) * max(grid10, grid100);

  grid.a = max(grid.a, grid1000);

  return grid * clamp((weight_gradient - flt_eps) / flt_eps, 0.0f, 1.0f);
}

float4 apply_color_fac(float4 color_in, float color_fac)
{
  float4 color = color_in;
  color.rgb = max(float3(0.005f), color_in.rgb) * color_fac;
  return color;
}

struct CurvesWeightPaintFragOut {
  [[frag_color(0)]] float4 frag_color;
  [[frag_color(1)]] float4 line_output;
};

[[fragment]] void curves_weight_paint_frag([[resource_table]] CurvesWeightPaintResources &srt,
                                          [[in]] const CurvesWeightPaintVertOut &v_in,
                                          [[out]] CurvesWeightPaintFragOut &frag_out)
{
  float alert = v_in.weight_interp.y;
  float4 color;

  /* Missing vertex group alert color. Uniform in practice. */
  if (alert > 1.1f) {
    color = apply_color_fac(theme.colors.vert_missing_data, v_in.color_fac);
  }
  /* Weights are available */
  else {
    float weight = v_in.weight_interp.x;
    float4 weight_color = texture(srt.colorramp, weight);
    weight_color = apply_color_fac(weight_color, v_in.color_fac);

    /* Contour display */
    if (srt.draw_contours) {
      /* This must be executed uniformly for all fragments */
      float weight_gradient = length(float2(gpu_dfdx(weight), gpu_dfdy(weight)));

      float4 grid = contour_grid(weight, weight_gradient);

      weight_color = grid + weight_color * (1.0f - grid.a);
    }

    /* Zero weight alert color. Nonlinear blend to reduce impact. */
    float4 color_unreferenced = apply_color_fac(theme.colors.vert_unreferenced, v_in.color_fac);
    color = mix(weight_color, color_unreferenced, alert * alert);
  }

  frag_out.frag_color = float4(color.rgb, srt.opacity);
  frag_out.line_output = float4(0.0f);
}

/* clang-format off */
PipelineGraphic curves_weight_paint(
    curves_weight_paint_vert, curves_weight_paint_frag, CurvesWeightPaintResources{.use_fake_shading = false, .use_clipping = false});
PipelineGraphic curves_weight_paint_clipped(
    curves_weight_paint_vert, curves_weight_paint_frag, CurvesWeightPaintResources{.use_fake_shading = false, .use_clipping = true});
PipelineGraphic curves_weight_paint_fake_shading(
    curves_weight_paint_vert, curves_weight_paint_frag, CurvesWeightPaintResources{.use_fake_shading = true, .use_clipping = false});
PipelineGraphic curves_weight_paint_fake_shading_clipped(
    curves_weight_paint_vert, curves_weight_paint_frag, CurvesWeightPaintResources{.use_fake_shading = true, .use_clipping = true});
/* clang-format on */

}  // namespace overlay
