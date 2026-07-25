/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_bit_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender::ed::sculpt_paint::smooth {

struct VoxelSmoothParams {
  /** World-space smoothing scale, used as the sigma of the Gaussian kernel. */
  float kernel_radius = 0.0f;
  /** Use the volume-preserving Taubin operator instead of a plain Gaussian blur. */
  bool preserve_volume = false;
};

/**
 * Smooth positions on a coarse voxel field instead of on the mesh itself.
 *
 * Averaging over the topological one-ring shrinks in world space as the mesh gets denser, so a
 * stroke on a dense mesh only removes fine detail. Here the smoothing scale is set by
 * \a params.kernel_radius in world space and is therefore independent of mesh density: the region
 * is rasterized into a voxel field, the field is smoothed, and the result is sampled back.
 *
 * All spans use region-local indexing and must have the same length.
 *
 * \param field_mask: elements that contribute to the field. Hidden geometry must be excluded, or
 *   it would drag the field toward parts of the surface the user cannot see.
 * \param anchor_mask: elements that keep their original position, such as mesh boundary vertices.
 *   These still contribute to the field when \a field_mask allows it.
 * \param r_targets: the smoothed target position of every element. Elements outside
 *   \a field_mask and elements in \a anchor_mask receive their original position.
 */
void voxel_smooth_positions(Span<float3> positions,
                            Span<float3> normals,
                            BitSpan field_mask,
                            BitSpan anchor_mask,
                            const VoxelSmoothParams &params,
                            MutableSpan<float3> r_targets);

}  // namespace blender::ed::sculpt_paint::smooth
