/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Projection used to rasterize geometry in UV space inside the Image Editor.
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_vec_types.h" /* for rctf */

namespace blender::draw {

/**
 * Build the matrix mapping UV coordinates to clip space for the UV-space shaded display.
 *
 * Only the rasterization position goes through this matrix. World position, normals and
 * attributes keep using the regular 3D view matrices, which is what makes the shading match
 * the 3D viewport.
 *
 * \param view_rect: the visible UV range, taken from #View2D::cur of the Image Editor region.
 * \param tile_offset: integer UDIM tile offset, so that tile `n` occupies UV range `n..n+1`.
 * \param jitter_ndc: temporal anti-aliasing jitter, already expressed in NDC units. The
 * regular pipeline applies this to the window matrix, which this matrix replaces, so it has
 * to be reapplied here or sample accumulation would keep adding identical samples.
 * \return the UV to clip space matrix.
 */
float4x4 uv_space_projection_get(const rctf &view_rect,
                                  const int2 &tile_offset,
                                  const float2 &jitter_ndc);

}  // namespace blender::draw
