/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * PBR passepartout of the Image Editor.
 *
 * A solid cell of the image plane (one UDIM tile), drawn at the depth the UV faces are written at,
 * so a depth test that rejects equal depth leaves the UV faces uncovered. See `overlay_mesh.hh` for
 * the CPU side.
 */

#pragma once

#include "draw_view_infos.hh" /* IWYU pragma: export */

#include "draw_view.bsl.hh"
#include "gpu_shader_compat.hh"

namespace overlay::edit_uv {

struct PassepartoutVertIn {
  /** A quad in -1..1, the shared `quad_solid` shape. */
  [[attribute(0)]] float3 pos;
};

struct Passepartout {
  [[push_constant]] const float4 ucolor;
  /** Lower-left corner of the cell, in image plane units. */
  [[push_constant]] const float3 tile_pos;
};

[[vertex]] void passepartout_vert([[resource_table]] const Passepartout &srt,
                                  [[resource_table]] const draw::View &views,
                                  [[in]] const PassepartoutVertIn &v_in,
                                  [[position]] float4 &out_position)
{
  const ViewMatrices view = views.get(0);
  /* Z is zero, exactly the depth the UV face overlay writes. */
  const float2 cell = v_in.pos.xy * 0.5f + 0.5f;
  out_position = view.point_world_to_homogenous(float3(cell + srt.tile_pos.xy, 0.0f));
}

struct PassepartoutFragOut {
  [[frag_color(0)]] float4 frag_color;
};

[[fragment]] void passepartout_frag([[resource_table]] const Passepartout &srt,
                                    [[out]] PassepartoutFragOut &frag_out)
{
  frag_out.frag_color = srt.ucolor;
}

#ifndef GLSL_CPP_STUBS
/* The shader is named after its namespace: `overlay_edit_uv_passepartout`. */
PipelineGraphic passepartout(passepartout_vert, passepartout_frag);
#endif

}  // namespace overlay::edit_uv
