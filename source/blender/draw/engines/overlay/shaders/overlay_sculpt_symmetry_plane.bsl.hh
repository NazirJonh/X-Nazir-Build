/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Sculpt symmetry plane overlay.
 *
 * Draws a translucent symmetry plane. The input quad is placed by an origin and two in-plane
 * directions supplied by the caller, and sized to the object bounds. Ordinarily that is one of the
 * object's own X/Y/Z planes through its origin, but a multi-object sculpt shares one plane across
 * every mesh, so it can sit anywhere. See `overlay_symmetry_plane.hh` for the CPU side.
 */

#pragma once

#include "draw_view_infos.hh" /* IWYU pragma: export */

#include "draw_model.bsl.hh"
#include "draw_view.bsl.hh"
#include "draw_view_clipping_lib.glsl"
#include "gpu_shader_compat.hh"

namespace overlay::sculpt {

struct SymmetryPlaneVertIn {
  [[attribute(0)]] float3 pos;
};

struct SymmetryPlane {
  /** Point the plane passes through, in object space. */
  [[push_constant]] const float3 plane_origin;
  /** Half-size of the plane quad in object space. */
  [[push_constant]] const float plane_size;
  /** Unit in-plane directions the quad's local X and Y map to, in object space. */
  [[push_constant]] const float3 plane_tangent;
  [[push_constant]] const float3 plane_bitangent;
  /** RGB color and alpha (opacity) of the plane. */
  [[push_constant]] const float4 plane_color;
};

struct Clipping {
  [[legacy_info]] ShaderCreateInfo drw_clipped;

  /** WORKAROUND: This exact compilation constant is checked in the Metal backend to enable clip
   * distances. */
  [[compilation_constant]] const bool use_clipping;
};

struct SymmetryPlaneVertOut {
  /** Constant per draw call; passed through so the fragment stage needs no resource table. */
  [[flat]] float4 color;
};

[[vertex]] void vert([[resource_table]] const SymmetryPlane &plane,
                     [[resource_table]] const Clipping &clipping,
                     [[resource_table]] const draw::View &views,
                     [[resource_table]] const draw::Model &models,
                     [[resource_table]] const draw::Resource &res_id,
                     [[instance_index]] const int inst_index,
                     [[in]] const SymmetryPlaneVertIn &v_in,
                     [[out]] SymmetryPlaneVertOut &v_out,
                     [[position]] float4 &out_position)
{
  const draw::ID id = res_id.get(inst_index);
  const ViewMatrices view = views.get(id.view_id<1>());
  const ObjectMatrices obj = models.get(id.resource_id<1>());

  /* The input quad lies in the XY plane (z = 0). Scale it and map its two axes onto the plane's
   * in-plane directions, centered on the plane's origin. */
  const float2 p = v_in.pos.xy * plane.plane_size;
  const float3 plane_pos = plane.plane_origin + plane.plane_tangent * p.x +
                           plane.plane_bitangent * p.y;

  const float3 world_pos = obj.point_object_to_world(plane_pos);
  out_position = view.point_world_to_homogenous(world_pos);

  v_out.color = plane.plane_color;

  if (clipping.use_clipping) [[static_branch]] {
    view_clipping_distances(world_pos);
  }
}

struct SymmetryPlaneFragOut {
  [[frag_color(0)]] float4 frag_color;
};

[[fragment]] void frag([[in]] const SymmetryPlaneVertOut &v_in,
                       [[out]] SymmetryPlaneFragOut &frag_out)
{
  /* Straight-alpha translucent plane. */
  frag_out.frag_color = v_in.color;
}

/* clang-format off */
PipelineGraphic symmetry_plane(        vert, frag, Clipping{.use_clipping = false});
PipelineGraphic symmetry_plane_clipped(vert, frag, Clipping{.use_clipping = true });
/* clang-format on */

}  // namespace overlay::sculpt
