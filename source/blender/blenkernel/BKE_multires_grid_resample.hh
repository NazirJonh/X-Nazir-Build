/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Resample a per-grid `float3` field between multires subdivision levels. CCG grids are nested: a
 * grid point `(x, y)` at level `L` is the point `(x<<k, y<<k)` at level `L + k`. Downsampling is
 * therefore an exact index stride; upsampling is per-grid bilinear interpolation. Used by the sculpt
 * layer system to store grid layers canonically at the max level while combining them onto the live
 * CCG (which sits at the current sculpt level).
 */

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_sys_types.h"

namespace blender::bke {

/**
 * Element count of a per-grid `float3` field at \a level: `grids_num * grid_size(level)^2`.
 *
 * 64-bit: the product crosses 2^31 at resolutions multires already allows (a 200k-corner base mesh
 * at level 7 reaches 3.3e9), and truncating it here fed a wrapped length to the layer allocator
 * while the callers kept indexing with the untruncated one.
 */
int64_t grid_totelem(int grids_num, int level);

/**
 * Downsample \a src (a per-grid field at \a src_level) to \a dst_level (<= src_level) by exact
 * index stride. \a src must hold `grid_totelem(grids_num, src_level)` elements; the result holds
 * `grid_totelem(grids_num, dst_level)`.
 */
Array<float3> grid_subsample(Span<float3> src, int src_level, int dst_level, int grids_num);

/**
 * Upsample \a src (a per-grid field at \a src_level) to \a dst_level (>= src_level) by per-grid
 * bilinear interpolation. Exact at the coarse sample points (so subsample(upsample(x)) == x) and
 * watertight for CCG delta fields, whose shared grid-boundary samples are equal.
 */
Array<float3> grid_upsample(Span<float3> src, int src_level, int dst_level, int grids_num);

}  // namespace blender::bke
