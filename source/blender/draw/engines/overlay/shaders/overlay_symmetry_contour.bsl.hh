/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Symmetry contour overlay.
 *
 * Draws a contour line on the surface of the mesh where the symmetry plane intersects it.
 * Procedural drawing using storage buffer.
 */

#pragma once

#include "draw_view_infos.hh" /* IWYU pragma: export */

#include "draw_model.bsl.hh"
#include "draw_view.bsl.hh"
#include "draw_view_clipping_lib.glsl"
#include "gpu_shader_compat.hh"
#include "infos/overlay_common_infos.hh"

#include "overlay_shader_shared.hh"

SHADER_LIBRARY_CREATE_INFO(draw_globals)

namespace overlay {

struct Resources {
  [[legacy_info]] ShaderCreateInfo draw_globals;

  [[sampler(0)]] sampler2DDepth scene_depth_tx;

  [[push_constant]] const float contour_width;
  [[push_constant]] const int colorid;

  [[storage(0, read)]] const VertexData (&data_buf)[];
};

struct Clipping {
  [[legacy_info]] ShaderCreateInfo drw_clipped;

  [[compilation_constant]] const bool use_clipping;
};

struct VertOut {
  /* Screen-space start/current position of the line, used to recover its direction for the
   * post-process anti-aliasing (see #pack_line_data). */
  [[no_perspective]] float2 edge_pos;
  [[flat]] float2 edge_start;
  [[flat]] float4 final_color;
  /* World-space occlusion tolerance, carried per-vertex in the unused `w` of #VertexData.pos_ so
   * that objects of different sizes can share one accumulated buffer and one draw. */
  [[flat]] float depth_bias;
};

struct FragOut {
  [[frag_color(0)]] float4 frag_color;
  [[frag_color(1)]] float4 line_output;
};

/* Helper for screen position. */
float2 screen_position(float4 p)
{
  return ((p.xy / p.w) * 0.5f + 0.5f) * uniform_buf.size_viewport;
}

/* Helper for packing line data (from overlay_common_lib.glsl). */
float4 pack_line_data(float2 frag_co, float2 edge_start, float2 edge_pos)
{
  float2 edge = edge_start - edge_pos;
  float len = length(edge);
  if (len > 0.0f) {
    edge /= len;
    float2 perp = float2(-edge.y, edge.x);
    float dist = dot(perp, frag_co - edge_start);
    /* Add 0.1f to differentiate with cleared pixels. */
    return float4(perp * 0.5f + 0.5f, dist * 0.25f + 0.5f + 0.1f, 1.0f);
  }
  else {
    /* Default line if the origin is perfectly aligned with a pixel. */
    return float4(1.0f, 0.0f, 0.5f + 0.1f, 1.0f);
  }
}

[[vertex]] void vert([[resource_table]] const Resources &res,
                     [[resource_table]] const Clipping &clipping,
                     [[resource_table]] const draw::View &views,
                     [[resource_table]] const draw::Model &models,
                     [[resource_table]] const draw::Resource &res_id,
                     [[instance_index]] const int inst_index,
                     [[vertex_id]] const int v_id,
                     [[out]] VertOut &v_out,
                     [[position]] float4 &out_position)
{
  const draw::ID id = res_id.get(inst_index);
  const ViewMatrices view = views.get(id.view_id<1>());
  const ObjectMatrices obj = models.get(id.resource_id<1>());

  const VertexData v_data = res.data_buf[v_id];
  const float3 world_pos = obj.point_object_to_world(v_data.pos_.xyz);
  out_position = view.point_world_to_homogenous(world_pos);

  v_out.edge_pos = v_out.edge_start = screen_position(out_position);
  v_out.depth_bias = v_data.pos_.w;

  if (res.colorid != 0) {
    /* TH_CAMERA_PATH is the only color code at the moment. */
    v_out.final_color = uniform_buf.colors.camera_path;
  }
  else {
    /* The theme alpha is kept: unlike the dashed extra-wire overlays this shader derives from, the
     * symmetry contour is a solid line whose opacity is user configurable. */
    v_out.final_color = v_data.color_;
  }

  if (clipping.use_clipping) [[static_branch]] {
    view_clipping_distances(world_pos);
  }
}

[[fragment]] void frag([[resource_table]] const Resources &res,
                       [[resource_table]] const draw::View &views,
                       [[in]] const VertOut &v_in,
                       [[frag_coord]] const float4 frag_co,
                       [[out]] FragOut &frag_out)
{
  /* Occlusion test in view-space. */
  const ViewMatrices view = views.get(0);
  float scene_depth = texelFetch(res.scene_depth_tx, int2(frag_co.xy), 0).r;
  float scene_z = view.depth_screen_to_view(scene_depth);
  float frag_z = view.depth_screen_to_view(frag_co.z);

  /* Discard if the fragment is further than the surface depth + bias.
   * Using view-space Z (negative). */
  if (frag_z < scene_z - v_in.depth_bias) {
    gpu_discard_fragment();
    /* Metal requires `discard` to be followed by a return. */
    return;
  }

  frag_out.frag_color = v_in.final_color;
  frag_out.line_output = pack_line_data(frag_co.xy, v_in.edge_start, v_in.edge_pos);
  /* The alpha channel carries the line width for the post-process anti-aliasing, which decodes it
   * as `w * 255` (see #Line::decode in overlay_antialiasing.bsl.hh). */
  frag_out.line_output.w = res.contour_width / 255.0f;
}

PipelineGraphic extra_wire_contour(vert, frag, Clipping{.use_clipping = false});
PipelineGraphic extra_wire_contour_clipped(vert, frag, Clipping{.use_clipping = true});

}  // namespace overlay
