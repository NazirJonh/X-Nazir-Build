/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#pragma once

#include <memory>

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

namespace detail {
struct GlobalGeodesicTopology;
}

/**
 * \param geodesic_topology_cache: Persists the #detail::GlobalGeodesicTopology built for the
 * Geodesic mode across repeated calls with the same object set / topology / bridge (e.g. one call
 * per mouse-move while moving the Expand origin, spec 5.2) -- pass the same instance (owned by
 * the caller, typically alongside #Cache) on every call within one Expand operation so only the
 * first call pays the concatenation + adjacency-map build cost. Ignored for Uniform /
 * UniformDiagonals. Pass a default-constructed (null) instance to always rebuild, matching the
 * previous behavior.
 */
void multi_object_graph_propagate(
    const Depsgraph &depsgraph,
    Span<Object *> objects,
    Span<Array<float3>> world_positions,
    Span<MultiVertRef> seeds,
    const MultiObjectBridge &bridge,
    PropagationMode mode,
    std::unique_ptr<detail::GlobalGeodesicTopology> &geodesic_topology_cache,
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
 * Exposed for unit testing on hand-built topology (no Blender Object required).
 *
 * Rebuilds the concatenated topology and its adjacency maps from scratch every call -- suitable
 * for one-shot / test use. #multi_object_graph_propagate's hot (per-mouse-move) path instead
 * builds a #GlobalGeodesicTopology once via #build_global_geodesic_topology and repropagates from
 * it via #propagate_geodesic_from_topology, since the topology is invariant for the whole Expand
 * operation (see `Architecture_Refactoring_Analysis.md` 5.2). */
void propagate_geodesic(Span<ObjectTopology> objects,
                        const MultiObjectBridge &bridge,
                        Span<MultiVertRef> seeds,
                        MutableSpan<Array<float>> r_vert_falloff_per_object);

/**
 * Concatenated multi-object topology + adjacency maps for the Geodesic core, cacheable across
 * repeated #propagate_geodesic_from_topology calls (e.g. one per mouse-move while moving the
 * Expand origin) as long as the object set / mesh topology / bridge do not change -- true for the
 * whole lifetime of one Expand modal operation, which never mutates geometry.
 *
 * \note #edge_to_face_offsets/#indices and #vert_to_edge_offsets/#indices are stored as the owning
 * #Array pair rather than a #GroupedSpan: #Array relocates its small-size inline buffer on
 * move/copy, which would leave a #GroupedSpan view stored alongside it dangling. Reconstruct the
 * #GroupedSpan / #OffsetIndices views from these owning arrays at each use site instead (cheap --
 * both are lightweight pointer+size wrappers).
 */
struct GlobalGeodesicTopology {
  Array<float3> positions;
  Array<int2> edges;
  /** Backs an #OffsetIndices<int> view over the concatenated faces; see the note above. */
  Array<int> face_offset_data;
  Array<int> corner_verts;
  /** Empty when no object hides any face. */
  Array<bool> hide_poly;
  Array<int> edge_to_face_offsets;
  Array<int> edge_to_face_indices;
  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  /** Size `objects.size() + 1`; object `i`'s vertices occupy `[vert_offset[i], vert_offset[i+1])`
   * in #positions, and its seeds translate to global indices via `vert + vert_offset[i]`. */
  Array<int> vert_offset;
};

GlobalGeodesicTopology build_global_geodesic_topology(Span<ObjectTopology> objects,
                                                      const MultiObjectBridge &bridge);

void propagate_geodesic_from_topology(const GlobalGeodesicTopology &topology,
                                      Span<MultiVertRef> seeds,
                                      MutableSpan<Array<float>> r_vert_falloff_per_object);

}  // namespace detail

}  // namespace blender::ed::sculpt_paint::expand
