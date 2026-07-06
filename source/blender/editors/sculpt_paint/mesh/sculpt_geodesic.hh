/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_set.hh"

namespace blender::ed::sculpt_paint::geodesic {

/**
 * Returns an array indexed by vertex index containing the geodesic distance to the closest vertex
 * in the initial vertex set.
 */
Array<float> distances_create(Span<float3> vert_positions,
                              Span<int2> edges,
                              OffsetIndices<int> faces,
                              Span<int> corner_verts,
                              GroupedSpan<int> vert_to_edge_map,
                              GroupedSpan<int> edge_to_face_map,
                              Span<bool> hide_poly,
                              const Set<int> &initial_verts,
                              float limit_radius);

/**
 * Same contract and per-triangle update formula as #distances_create, but propagates in true
 * increasing-distance order via a min-heap (the standard Fast Marching Method for triangulated
 * surfaces) instead of #distances_create's round-based BFS. Prefer this variant whenever the
 * topology may contain long-range "shortcut" edges not backed by mesh faces (e.g. a cross-object
 * proximity bridge) -- #distances_create's round order can then diverge badly from true distance
 * order and re-relax large parts of the mesh many times over; this variant finalizes each vertex
 * exactly once regardless of topology.
 */
Array<float> distances_create_priority_queue(Span<float3> vert_positions,
                                             Span<int2> edges,
                                             OffsetIndices<int> faces,
                                             Span<int> corner_verts,
                                             GroupedSpan<int> vert_to_edge_map,
                                             GroupedSpan<int> edge_to_face_map,
                                             Span<bool> hide_poly,
                                             const Set<int> &initial_verts,
                                             float limit_radius);

}  // namespace blender::ed::sculpt_paint::geodesic
