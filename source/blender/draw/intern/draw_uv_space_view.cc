/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#include "draw_uv_space_view.hh"

namespace blender::draw {

float4x4 uv_space_projection_get(const rctf &view_rect,
                                  const int2 &tile_offset,
                                  const float2 &jitter_ndc)
{
  /* Named range_min/range_max rather than min/max: MSVC pulls in min/max macros from Windows
   * headers in some translation units. */
  const float2 range_min = float2(view_rect.xmin, view_rect.ymin) + float2(tile_offset);
  const float2 range_max = float2(view_rect.xmax, view_rect.ymax) + float2(tile_offset);

  /* Orthographic projection of the visible UV range onto normalized device coordinates. The
   * depth range is left unused: every fragment writes a constant depth, see the design spec. */
  const float2 scale = 2.0f / (range_max - range_min);
  const float2 offset = -(range_max + range_min) / (range_max - range_min) + jitter_ndc;

  float4x4 mat = float4x4::identity();
  mat[0][0] = scale.x;
  mat[1][1] = scale.y;
  /* Zero, so the constant translation below fully determines the depth of every fragment. This
   * makes the matrix singular by construction: it must never be inverted. Any feature needing a
   * clip-space to UV mapping (picking, gizmos) has to build its own inverse from `scale` and
   * `offset` rather than call #math::invert on the result. */
  mat[2][2] = 0.0f;
  mat[3][0] = offset.x;
  mat[3][1] = offset.y;
  mat[3][2] = 0.5f;

  return mat;
}

}  // namespace blender::draw
