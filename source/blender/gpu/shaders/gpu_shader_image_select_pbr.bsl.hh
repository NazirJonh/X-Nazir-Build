/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Preview-grade PBR shading of the Warp Selection patch in the 3D Viewport: composites the
 * Base Color / Roughness / Metallic / Normal maps of the lifted fragment on the deformed lattice.
 * Each map has its own texture-coordinate scale and offset (one pair per channel), because the
 * maps may live at different tile resolutions.
 */

#pragma once

#include "gpu_shader_compat.hh"

namespace builtin::image_select_pbr {

struct VertIn {
  [[attribute(0)]] float3 pos;
  [[attribute(1)]] float2 src_uv;
  [[attribute(2)]] float3 nor;
};

struct VertOut {
  [[smooth]] float2 uv_interp;
  [[smooth]] float3 normal_interp;
  [[smooth]] float3 world_position_interp;
};

struct FragOut {
  [[frag_color(0)]] float4 fragColor;
};

struct Resources {
  [[push_constant]] const float4x4 ModelViewProjectionMatrix;
  [[sampler(0)]] const sampler2D base_color_tex;
  [[sampler(1)]] const sampler2D roughness_tex;
  [[sampler(2)]] const sampler2D metallic_tex;
  [[sampler(3)]] const sampler2D normal_tex;
  [[push_constant]] const float3 light_direction;
  [[push_constant]] const float3 view_direction;
  [[push_constant]] const float2 texco_scale_base_color;
  [[push_constant]] const float2 texco_offset_base_color;
  [[push_constant]] const float2 texco_scale_roughness;
  [[push_constant]] const float2 texco_offset_roughness;
  [[push_constant]] const float2 texco_scale_metallic;
  [[push_constant]] const float2 texco_offset_metallic;
  [[push_constant]] const float2 texco_scale_normal;
  [[push_constant]] const float2 texco_offset_normal;
};

/**
 * Tangent-frame normal perturbation from screen-space derivatives (Lengyel), so the mesh's own UV
 * layout drives the tangent space without a tangent attribute. Falls back to the geometric normal
 * where the derivatives degenerate (a seam, or a triangle of constant UV).
 */
float3 perturb_normal(const float3 world_position,
                      const float2 uv,
                      const float3 n,
                      const float3 map_normal)
{
  float3 sigma_x = gpu_dfdx(world_position);
  float3 sigma_y = gpu_dfdy(world_position);
  float2 uv_x = gpu_dfdx(uv);
  float2 uv_y = gpu_dfdy(uv);
  float3 dp2perp = cross(sigma_y, n);
  float3 dp1perp = cross(n, sigma_x);
  float3 tangent = dp2perp * uv_x.x + dp1perp * uv_y.x;
  float3 bitangent = dp2perp * uv_x.y + dp1perp * uv_y.y;
  float t_len2 = dot(tangent, tangent);
  float b_len2 = dot(bitangent, bitangent);
  if (max(t_len2, b_len2) <= 0.0f) {
    return n;
  }
  float invmax = inversesqrt(max(t_len2, b_len2));
  tangent *= invmax;
  bitangent *= invmax;
  return normalize(tangent * map_normal.x + bitangent * map_normal.y + n * map_normal.z);
}

[[vertex]] void vert([[resource_table]] const Resources &srt,
                     [[in]] const VertIn &v_in,
                     [[position]] float4 &gl_Position,
                     [[out]] VertOut &interp)
{
  interp.uv_interp = v_in.src_uv;
  interp.normal_interp = v_in.nor;
  interp.world_position_interp = v_in.pos;
  gl_Position = srt.ModelViewProjectionMatrix * float4(v_in.pos, 1.0f);
}

[[fragment]] void frag([[resource_table]] const Resources &srt,
                       [[in]] const VertOut &interp,
                       [[out]] FragOut &frag_out)
{
  float4 base_color = texture(
      srt.base_color_tex, interp.uv_interp * srt.texco_scale_base_color + srt.texco_offset_base_color);
  if (base_color.a <= 0.002f) {
    gpu_discard_fragment();
  }
  float roughness = texture(srt.roughness_tex,
                            interp.uv_interp * srt.texco_scale_roughness +
                                srt.texco_offset_roughness)
                        .r;
  float metallic = texture(srt.metallic_tex,
                           interp.uv_interp * srt.texco_scale_metallic +
                               srt.texco_offset_metallic)
                       .r;
  float3 map_normal = texture(
                          srt.normal_tex,
                          interp.uv_interp * srt.texco_scale_normal + srt.texco_offset_normal)
                          .rgb;

  roughness = clamp(roughness, 0.04f, 1.0f);
  metallic = clamp(metallic, 0.0f, 1.0f);

  float3 n = normalize(interp.normal_interp);
  n = perturb_normal(interp.world_position_interp,
                     interp.uv_interp,
                     n,
                     normalize(map_normal * 2.0f - 1.0f));

  float3 l = normalize(srt.light_direction);
  float3 v = normalize(srt.view_direction);
  float3 h = normalize(l + v);
  float ndl = max(dot(n, l), 0.0f);
  float ndh = max(dot(n, h), 0.0f);

  /* Preview-grade PBR response (diffuse + Blinn-Phong specular), not the render engine's shading:
   * enough for the floating patch to read as the material next to the viewport's own lighting.
   * Metals drop the diffuse and tint the reflection with the base color. */
  float shininess = exp2(11.0f * (1.0f - roughness));
  float3 f0 = mix(float3(0.04f), base_color.rgb, metallic);
  float3 diffuse = base_color.rgb * (0.35f + 0.65f * ndl) * (1.0f - metallic);
  float spec_strength = pow(ndh, shininess) * (1.0f - roughness) * ndl;
  float3 rgb = diffuse + f0 * spec_strength;

  frag_out.fragColor = float4(rgb, base_color.a);
}

}  // namespace builtin::image_select_pbr

PipelineGraphic gpu_shader_image_select_pbr(builtin::image_select_pbr::vert,
                                            builtin::image_select_pbr::frag);
