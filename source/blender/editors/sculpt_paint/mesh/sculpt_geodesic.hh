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

/** Distance and curve-parameter field over the mesh surface, for the Curve gradient's geodesic
 * distance mode. For every vertex: the geodesic distance from the seeded curve neighborhood and a
 * smoothly blended arc-length position propagated from nearby seeds. */
struct GeodesicCurveField {
  Array<float> dist;
  Array<float> param;
};

/**
 * Propagate distances and arc-length positions from seeds near the curve with a min-heap Dijkstra
 * (same straight-edge + triangle-unfold updates as #distances_create_priority_queue). Competing
 * seed parameters are blended across triangle updates to avoid abrupt Voronoi boundaries.
 *
 * \param seed_verts mesh vertex indices, one per curve seed.
 * \param seed_params arc-length position of the matching curve seed, same size as \a seed_verts.
 * \param seed_distances initial Euclidean distance from each seed vertex to the curve.
 */
GeodesicCurveField curve_geodesic_field_create(Span<float3> vert_positions,
                                               Span<int2> edges,
                                               OffsetIndices<int> faces,
                                               Span<int> corner_verts,
                                               GroupedSpan<int> vert_to_edge_map,
                                               GroupedSpan<int> edge_to_face_map,
                                               Span<bool> hide_poly,
                                               Span<int> seed_verts,
                                               Span<float> seed_params,
                                               Span<float> seed_distances);

}  // namespace blender::ed::sculpt_paint::geodesic
