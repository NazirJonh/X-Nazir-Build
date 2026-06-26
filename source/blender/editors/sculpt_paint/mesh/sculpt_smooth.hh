/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_generic_span.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

namespace blender {

struct BMVert;
struct Object;
struct SubdivCCG;
struct SubdivCCGCoord;
namespace bke {
enum class AttrDomain : int8_t;
}

namespace ed::sculpt_paint::smooth {

/**
 * For bmesh: Average surrounding verts based on an orthogonality measure.
 * Naturally converges to a quad-like structure.
 */
void bmesh_four_neighbor_average(float avg[3], const float3 &direction, const BMVert *v);

void neighbor_color_average(OffsetIndices<int> faces,
                            Span<int> corner_verts,
                            GroupedSpan<int> vert_to_face_map,
                            GSpan color_attribute,
                            bke::AttrDomain color_domain,
                            GroupedSpan<int> vert_neighbors,
                            MutableSpan<float4> smooth_colors);

void neighbor_position_average_interior_grids(OffsetIndices<int> faces,
                                              Span<int> corner_verts,
                                              BitSpan boundary_verts,
                                              const Set<OrderedEdge> &boundary_edges,
                                              const SubdivCCG &subdiv_ccg,
                                              Span<int> grids,
                                              Span<float> factors,
                                              MutableSpan<float3> new_positions);
void neighbor_position_average_interior_grids(OffsetIndices<int> faces,
                                              Span<int> corner_verts,
                                              BitSpan boundary_verts,
                                              const Set<OrderedEdge> &boundary_edges,
                                              const SubdivCCG &subdiv_ccg,
                                              Span<int> grids,
                                              MutableSpan<float3> new_positions);

void neighbor_position_average_bmesh(const Set<BMVert *, 0> &verts,
                                     MutableSpan<float3> new_positions);
void neighbor_position_average_interior_bmesh(const Set<BMVert *, 0> &verts,
                                              Span<float> factors,
                                              MutableSpan<float3> new_positions);
void neighbor_position_average_interior_bmesh(const Set<BMVert *, 0> &verts,
                                              MutableSpan<float3> new_positions);

template<typename T>
void neighbor_data_average_mesh(Span<T> src, GroupedSpan<int> vert_neighbors, MutableSpan<T> dst);

template<typename T>
void neighbor_data_average_mesh_check_loose(Span<T> src,
                                            Span<int> verts,
                                            GroupedSpan<int> vert_neighbors,
                                            MutableSpan<T> dst);

template<typename T>
void average_data_grids(const SubdivCCG &subdiv_ccg,
                        Span<T> src,
                        Span<int> grids,
                        MutableSpan<T> dst);

template<typename T>
void average_data_bmesh(Span<T> src, const Set<BMVert *, 0> &verts, MutableSpan<T> dst);

/* Average the data in the argument span across vertex neighbors. */
void blur_geometry_data_array(const Object &object, int iterations, MutableSpan<float> data);

/* Surface Smooth Brush. */

void surface_smooth_laplacian_step(Span<float3> positions,
                                   Span<float3> orig_positions,
                                   Span<float3> average_positions,
                                   float alpha,
                                   MutableSpan<float3> laplacian_disp,
                                   MutableSpan<float3> translations);
void surface_smooth_displace_step(Span<float3> laplacian_disp,
                                  Span<float3> average_laplacian_disp,
                                  float beta,
                                  MutableSpan<float3> translations);

void calc_relaxed_translations_faces(Span<float3> vert_positions,
                                     Span<float3> vert_normals,
                                     OffsetIndices<int> faces,
                                     Span<int> corner_verts,
                                     GroupedSpan<int> vert_to_face_map,
                                     BitSpan boundary_verts,
                                     const Set<OrderedEdge> &boundary_edges,
                                     Span<int> face_sets,
                                     Span<bool> hide_poly,
                                     bool filter_boundary_face_sets,
                                     Span<int> verts,
                                     Span<float> factors,
                                     MutableSpan<float3> translations);
void calc_relaxed_translations_grids(const SubdivCCG &subdiv_ccg,
                                     OffsetIndices<int> faces,
                                     Span<int> corner_verts,
                                     Span<int> face_sets,
                                     GroupedSpan<int> vert_to_face_map,
                                     BitSpan boundary_verts,
                                     const Set<OrderedEdge> &boundary_edges,
                                     Span<int> grids,
                                     bool filter_boundary_face_sets,
                                     Span<float> factors,
                                     MutableSpan<float3> translations);
void calc_relaxed_translations_bmesh(const Set<BMVert *, 0> &verts,
                                     Span<float3> positions,
                                     const int face_set_offset,
                                     bool filter_boundary_face_sets,
                                     Span<float> factors,
                                     MutableSpan<float3> translations);

/**
 * Resolution-independent "Spatial Taubin" shape smoothing for dense meshes.
 *
 * Instead of averaging each vertex over its topological 1-ring (whose world-space reach shrinks
 * as resolution grows), this gathers a spatial region around the brush once, builds a uniform
 * hash-grid over it, and runs a volume-preserving Taubin (lambda/mu) pass over a local working
 * copy of the region positions. The averaging radius is fixed in world space, so a single brush
 * stroke smooths large shapes on hi-poly meshes as strongly as on low-poly ones, while the mu
 * inflate step keeps the overall volume from collapsing.
 *
 * The region is collected from PBVH nodes whose bounds intersect the brush sphere, so the cost
 * scales with the brush footprint rather than the whole mesh (the previous per-vertex tree
 * search did the latter). Results are returned per region vertex; the caller maps them back to
 * the vertices it is responsible for via \a r_vert_to_region.
 *
 * \param r_region_verts: global vertex indices contained in the gathered region.
 * \param r_region_positions: smoothed position for each entry in \a r_region_verts (parallel).
 * \param r_vert_to_region: sized to the mesh vertex count; maps a global vertex index to its
 *   index in \a r_region_verts, or -1 when the vertex is outside the region.
 */
void radius_based_smooth_mesh_aggressive(const bke::pbvh::Tree &pbvh,
                                         const Span<float3> vert_positions,
                                         const Span<float3> vert_normals,
                                         const BitSpan boundary_verts,
                                         const float3 &brush_center,
                                         const float brush_radius,
                                         const float search_radius_factor,
                                         const float distance_exponent,
                                         Vector<int> &r_region_verts,
                                         Array<float3> &r_region_positions,
                                         Array<int> &r_vert_to_region);

}  // namespace ed::sculpt_paint::smooth

}  // namespace blender
