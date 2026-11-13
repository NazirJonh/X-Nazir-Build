/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Surface unfold: lays a patch of adjacent mesh triangles flat into the plane of a seed triangle.
 * The Transform Selection tool's decal mode uses it to carry a stamp over cube edges and curved
 * surfaces without cutting it off where a planar projection turns edge-on: every triangle is
 * rotated about the edge it shares with an already placed neighbor, so the stamp wraps the surface
 * like a sticker.
 *
 * Pure geometry (spans in, a patch out): no GPU, no Image, no Depsgraph.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::ed::sculpt_paint {

/**
 * Corner-triangle adjacency across edges, built once per topology (the unfold itself runs on every
 * drag step). `neighbors[tri][i]` is the triangle across the edge from corner `i` to corner
 * `(i + 1) % 3`, or -1 on a mesh boundary and on a non-manifold edge (shared by more than two
 * triangles): the unfold never crosses those.
 */
struct SurfaceUnfoldAdjacency {
  Array<int3> neighbors;
};

SurfaceUnfoldAdjacency surface_unfold_adjacency_build(Span<int3> corner_tris,
                                                      Span<int> corner_verts);

/** One triangle of the patch: its corners in the order of the mesh's corner triangle. */
struct SurfaceUnfoldTri {
  int tri_index = -1;
  /** Object-space corner positions. */
  float3 co[3] = {float3(0.0f), float3(0.0f), float3(0.0f)};
  /** Corner positions flattened into the stamp plane, in object units, origin at the seed point. */
  float2 plane[3] = {float2(0.0f), float2(0.0f), float2(0.0f)};
};

struct SurfaceUnfoldPatch {
  /** The seed triangle first, then breadth-first by edge distance. */
  Vector<SurfaceUnfoldTri> tris;
  float2 bounds_min = float2(0.0f);
  float2 bounds_max = float2(0.0f);
  /** The radius the patch was grown to (object units). */
  float radius = 0.0f;
  /** True when #max_tris cut the growth short: the patch does not reach the full radius. */
  bool truncated = false;
};

/**
 * Grow a patch around \a seed_tri and flatten it into the seed's plane.
 *
 * The plane has its origin at \a seed_point, its X axis along \a tangent projected into the seed
 * triangle's plane, its Z along the seed triangle's normal (corner-order winding, as
 * #ImageSelectView3DSurfaceMap::sample reports it) and Y = Z x X. A neighbor across a shared edge
 * is placed by its edge lengths on the side away from the triangle it was reached from (a rotation
 * about the edge), so the layout is isometric per triangle.
 *
 * Growth stops at triangles whose flattened bounds miss the square `[-radius, radius]^2` and at
 * \a max_tris. Around a vertex whose angles sum to more than 360 degrees (a saddle) or less (a
 * cone tip) the flattened triangles can overlap or leave a gap; each triangle is placed once, from
 * the first neighbor that reaches it, so the first placement wins and the overlap is accepted.
 *
 * \param corner_tris / \a corner_verts / \a vert_positions: the mesh's corner triangles (corner
 * indices), corner-to-vertex map and the (evaluated) vertex positions.
 * \return false when \a seed_tri is invalid or degenerate (the patch is left empty).
 */
bool surface_unfold_build(Span<int3> corner_tris,
                          Span<int> corner_verts,
                          Span<float3> vert_positions,
                          const SurfaceUnfoldAdjacency &adjacency,
                          int seed_tri,
                          const float3 &seed_point,
                          const float3 &tangent,
                          float radius,
                          int max_tris,
                          SurfaceUnfoldPatch &r_patch);

}  // namespace blender::ed::sculpt_paint
