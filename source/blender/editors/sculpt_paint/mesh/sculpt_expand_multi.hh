/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#pragma once

#include <array>
#include <memory>
#include <utility>

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
}  // namespace blender

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
  UniformDiagonals, /* +1 per shared-face-corner / bridge hop (TopologyNormals = Topology
                       Diagonals). */
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

/**
 * Union-find canonicalization of `object`'s CCG "duplicate" seam vertices (see
 * #detail::canonicalize_duplicates) -- a Grids object stores one flat vertex slot per grid, so a
 * point on a shared boundary between two grids of the same Multires cage is stored TWICE, at two
 * different flat indices, and #BKE_subdiv_ccg_neighbor_coords_get reports each as the other's
 * "duplicate". `result.size() == BKE_sculpt_get_grid_num_verts(object)`; `result[raw]` is the
 * smallest flat index among every raw index that is the same physical point as `raw`. Must be
 * called on a Grids-backed `object` (`bke::object::pbvh_get(object)->type() == Type::Grids`).
 */
Array<int> grids_canonical_map_create(const Object &object);

/**
 * Real (non-duplicate) CCG edge adjacency for `object`, over CANONICAL vertex ids (see
 * #grids_canonical_map_create) -- ready to feed #detail::propagate_uniform directly.
 * `r_offsets`/`r_data` back the returned view and must outlive it.
 */
GroupedSpan<int> grids_edge_neighbors_create(const Object &object,
                                             Span<int> canonical_map,
                                             Array<int> &r_offsets,
                                             Array<int> &r_data);

/**
 * Same-quad ("diagonal") adjacency for `object`, over CANONICAL vertex ids, for the
 * `PropagationMode::UniformDiagonals` arm. Includes both edge- and diagonal-adjacent same-quad
 * partners for INTERIOR grid coordinates (exact); a coordinate on a grid boundary or corner keeps
 * only its edge (#grids_edge_neighbors_create) neighbors here -- boundary/corner grid vertices
 * intentionally do not gain extra diagonal edges this round. `r_offsets`/`r_data` back the
 * returned view and must outlive it.
 */
GroupedSpan<int> grids_diagonal_neighbors_create(const Object &object,
                                                 Span<int> canonical_map,
                                                 Array<int> &r_offsets,
                                                 Array<int> &r_data);

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
 * \param grids_canonical_maps: index-aligned with `objects`; for a Grids object,
 * `grids_canonical_maps[i]` maps its raw flat CCG index to a canonical representative (see
 * #grids_canonical_map_create) -- an empty inner array means "this object is not Grids, use raw
 * indices directly, no remap". Ignored for `PropagationMode::Geodesic` (Grids objects never reach
 * the Geodesic arm -- routed through Sphere instead).
 */
void multi_object_graph_propagate(
    const Depsgraph &depsgraph,
    Span<Object *> objects,
    Span<Array<float3>> world_positions,
    Span<Array<int>> grids_canonical_maps,
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
 * threshold is `bridge_factor * min(mean_world_edge_length[a], mean_world_edge_length[b])`. Keeps
 * a stitch only if it is mutually nearest, capped at one edge per vertex per side, spatially
 * deduped. */
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

/* World-space per-object mesh topology, sufficient to run the geodesic core. Positions are in
 * WORLD space (spec §6.0). `hide_poly` may be empty (⇒ no hidden faces). */
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

/**
 * Union-find over `vert_count` flat vertex indices, merging every pair `(v, d)` where `d` is
 * reported as a duplicate of `v` by `duplicates_of`. Returns, for every vertex, the SMALLEST index
 * in its merged equivalence class (deterministic representative). A vertex reported as its own
 * duplicate, or never reported at all, canonicalizes to itself.
 *
 * Exposed for unit testing on a hand-built `duplicates_of` functor -- no #SubdivCCG required. The
 * production caller (#grids_canonical_map_create) wraps
 * #BKE_subdiv_ccg_neighbor_coords_get(..., include_duplicates=true, ...)'s `.duplicates()`.
 */
Array<int> canonicalize_duplicates(
    int vert_count, FunctionRef<void(int vert, Vector<int> &r_duplicates)> duplicates_of);

/**
 * Builds a symmetric adjacency list over CANONICAL vertex ids (a dense 0-based compaction of the
 * distinct values in `canonical_of_raw`, NOT the raw representative value itself -- callers get a
 * graph ready to feed straight into #propagate_uniform, matching its "one array entry per graph
 * vertex" convention). For every raw vertex, `real_neighbors_of` supplies its REAL (non-duplicate)
 * neighbors as raw indices; each is remapped through `canonical_of_raw` and every raw vertex
 * sharing a canonical id contributes its neighbors to that one merged row (deduplicated). A vertex
 * is never its own neighbor even if some raw neighbor remaps to the same canonical id as the
 * source (e.g. a duplicate reported as a "real" neighbor by mistake upstream -- defensively
 * excluded here, not assumed impossible).
 *
 * Exposed for unit testing on hand-built functors -- no #SubdivCCG required. `r_offsets`/`r_data`
 * back the returned #GroupedSpan and must outlive it (same convention as
 * #build_vert_to_vert_map).
 */
GroupedSpan<int> build_canonical_neighbors(
    int raw_vert_count,
    Span<int> canonical_of_raw,
    FunctionRef<void(int raw_vert, Vector<int> &r_real_neighbors)> real_neighbors_of,
    Array<int> &r_offsets,
    Array<int> &r_data);

/**
 * The 4 diagonal `(dx, dy)` offsets from an interior CCG grid coordinate (`dx` along `x`, `dy`
 * along `y`) to its 4 diagonal same-quad neighbors. Only valid for a coordinate whose `x±1`/`y±1`
 * all stay within `[0, grid_size)` of the SAME grid -- the production caller
 * (#grids_diagonal_neighbors_create, non-`detail`) checks that bound before applying these.
 * Factored out as a named function (not inlined at each call site) so it has exactly one
 * definition shared by production code and its unit test.
 */
std::array<std::pair<int, int>, 4> interior_diagonal_offsets();

}  // namespace detail

}  // namespace blender::ed::sculpt_paint::expand
