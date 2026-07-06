/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#pragma once

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {
struct Depsgraph;
struct Object;
}

namespace blender::ed::sculpt_paint::expand {

struct MultiVertRef {
  int object_index = -1;
  int vert = -1;
};

struct BridgeEdge {
  MultiVertRef a;
  MultiVertRef b;
  float world_distance = 0.0f;
};

struct MultiObjectBridge {
  Vector<BridgeEdge> edges;
};

enum class PropagationMode {
  Uniform,          /* +1 per mesh-edge / bridge hop (Topology, BoundaryTopology). */
  UniformDiagonals, /* +1 per shared-face-corner / bridge hop (TopologyNormals = Topology Diagonals). */
  Geodesic,
};

Array<Array<float3>> world_positions_create(const Depsgraph &depsgraph, Span<Object *> objects);

/* A local-space search radius guaranteed to cover a world-space sphere of `world_radius` under
 * `world_to_object`. bke::pbvh nearest-vert queries take max_distance in OBJECT-LOCAL units, so a
 * raw world radius under-searches when the object is scaled down / non-uniform. Conservative
 * (over-inclusive) — the exact world gap is compared afterward. */
float world_radius_to_local_search_radius(const float4x4 &world_to_object, float world_radius);

MultiObjectBridge build_multi_object_bridge(const Depsgraph &depsgraph,
                                            Span<Object *> objects,
                                            Span<Array<float3>> world_positions,
                                            float bridge_factor);

void multi_object_graph_propagate(const Depsgraph &depsgraph,
                                  Span<Object *> objects,
                                  Span<Array<float3>> world_positions,
                                  Span<MultiVertRef> seeds,
                                  const MultiObjectBridge &bridge,
                                  PropagationMode mode,
                                  MutableSpan<Array<float>> r_vert_falloff_per_object);

namespace detail {

/* Core proximity-bridge construction, independent of Blender Object so it is unit-testable.
 * `object_world_positions[i]` = world-space verts of object i. `mean_world_edge_length[i]` = that
 * object's average world edge length. `nearest(obj, world_p, max_world_dist)` returns the nearest
 * vertex index of object `obj` to world point `world_p` within `max_world_dist`, or -1. The pair
 * threshold is `bridge_factor * min(mean_world_edge_length[a], mean_world_edge_length[b])`. Keeps a
 * stitch only if it is mutually nearest, capped at one edge per vertex per side, spatially deduped. */
MultiObjectBridge build_bridge_impl(
    Span<Span<float3>> object_world_positions,
    Span<float> mean_world_edge_length,
    float bridge_factor,
    FunctionRef<int(int obj, const float3 &world_p, float max_world_dist)> nearest);

/* Multi-source Uniform (unit-weight) BFS over a graph = each object's vertex adjacency
 * (`object_neighbors[i]` maps a vertex to its neighbor vertices in object i) PLUS the cross-object
 * `bridge` edges (both directions). Distances start at 0 for `seeds`, FLT_MAX elsewhere; every hop
 * (intra-object edge or bridge edge) adds 1. Writes one distance Array per object into
 * `r_vert_falloff_per_object` (index-aligned to `object_neighbors`). Exposed for unit testing on
 * hand-built topology (no Blender Object required). */
void propagate_uniform(Span<GroupedSpan<int>> object_neighbors,
                       const MultiObjectBridge &bridge,
                       Span<MultiVertRef> seeds,
                       MutableSpan<Array<float>> r_vert_falloff_per_object);

/* World-space per-object mesh topology, sufficient to run the geodesic core. Positions are in WORLD
 * space (spec §6.0). `hide_poly` may be empty (⇒ no hidden faces). */
struct ObjectTopology {
  Span<float3> positions;
  Span<int2> edges;
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  Span<int> corner_edges;
  Span<bool> hide_poly;
};

/* Geodesic (triangle-unfold) propagation over all objects, treating each `bridge` edge as a
 * face-less edge (Euclidean world length). Concatenates into one global topology and defers to
 * geodesic::distances_create, so single-object / no-bridge output is elementwise identical to it.
 * Exposed for unit testing on hand-built topology (no Blender Object required). */
void propagate_geodesic(Span<ObjectTopology> objects,
                        const MultiObjectBridge &bridge,
                        Span<MultiVertRef> seeds,
                        MutableSpan<Array<float>> r_vert_falloff_per_object);

}  // namespace detail

}  // namespace blender::ed::sculpt_paint::expand
