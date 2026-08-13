/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Image Editor Area Plane (#MTEX_MAP_MODE_AREA) walks from the mesh surface to UV, the same
 * direction as Sculpt PBVH pixel nodes. It does not look up a triangle from a UV pixel
 * (no nearest-UV, no extrapolation into empty UV).
 *
 * Invariants
 * ----------
 * - Dab center: the triangle *containing* the cursor UV (barycentric >= 0). If several overlap,
 *   the smallest #AreaPlaneTriangle::index supplies the tangent frame only. If none contain the
 *   UV, the dab is a no-op — never a nearest miss with extrapolated coordinates.
 * - Coverage: triangles whose object-space closest point lies inside the dab sphere (full 3D
 *   length after #area_plane_local_mat, not an XY disk). Each accepted triangle is rasterized
 *   independently into the tiles its UV bbox overlaps.
 * - Shared UV edge (same island): a texel belongs to exactly one triangle via the half-open
 *   winding test (#area_plane_uv_pixel_inside_triangle). No hole, no double blend, no tri_index
 *   jump from a second nearest query.
 * - UV seam (shared 3D edge, different UV): islands do not overlap in UV; both write their own
 *   texels. The same object-space point samples the same Area Plane coordinate.
 * - Overlapping UV islands: interior texels of every accepted triangle may write. Order is
 *   #AreaPlaneTriangle::index.
 * - Tangent frame: 3D falloff uses a sphere at the dab center. Source sample uses one Area
 *   Plane whose X/Y are the image-texture axes of the dab triangle (object-space ∂P/∂u, ∂P/∂v
 *   from that triangle's UV, then #MTex.rot in that plane). Not object X/Z — those axes ignore
 *   how the image sits on the island and turn ±90° at a cube fold.
 * - A neighbor is unfolded into the dab plane along a path of UV-sewn edges first (the image
 *   net), then 3D-only seams. Direct 3D hinges that are not sewn in UV (front–top at a cube
 *   corner while the net goes front–right–top) rotate the stamp 90° on the third island.
 *   Coplanar steps are identity. Falloff still uses the original object-space point.
 */

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct Object;

}  // namespace blender

namespace blender::ed::sculpt_paint {

struct AreaPlaneTriangle {
  /** Index into the evaluated mesh's corner triangles. */
  int index = -1;
  /** Evaluated-mesh vertex indices of the three corners (for shared-edge unfold). */
  int vert[3] = {-1, -1, -1};
  float2 uv[3] = {};
  /** Object space. */
  float3 position[3] = {};
  /** Object space, not necessarily unit length (interpolated at the dab center). */
  float3 normal[3] = {};
};

struct AreaPlaneHit {
  float3 position = float3(0.0f);
  /** Object space, normalized. */
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  int tri_index = -1;
};

/**
 * Stroke-local copy of UV triangles and object-space positions/normals.
 *
 * Built once per stroke from the evaluated mesh. Immutable afterwards. Has no UV BVH and no
 * per-texel UV→surface query.
 */
class AreaPlaneMesh {
  Vector<AreaPlaneTriangle> triangles_;

 public:
  AreaPlaneMesh(const Depsgraph &depsgraph, const Object &ob, StringRef uv_map_name);

  bool is_valid() const;

  /**
   * True when \a uv lies inside at least one triangle (inclusive barycentric, no extrapolation).
   * On UV overlap the smallest triangle index is reported, and is only used to build the dab's
   * tangent frame.
   */
  bool hit_at_uv(const float2 &uv, AreaPlaneHit &r_hit) const;

  /**
   * Object-space radius matching a UV-space dab of \a radius_uv at \a hit, via the UV Jacobian
   * of that triangle. Zero when the triangle is degenerate in UV.
   */
  float radius_object(const AreaPlaneHit &hit, float radius_uv) const;

  /**
   * Triangle indices whose closest point to the origin, after \a object_to_brush, lies inside
   * the unit sphere (plus a small tolerance). \a object_to_brush must be built with rotation 0
   * so texture rotation does not change which triangles the dab covers.
   */
  Vector<int> triangles_in_sphere(const float4x4 &object_to_brush) const;

  AreaPlaneTriangle triangle(int index) const;

  Span<AreaPlaneTriangle> triangles() const;
};

/**
 * Unscaled object-space Area Plane frame: origin at the dab center, orthonormal X/Y/Z, and the
 * object-space radius that maps to local length 1.
 */
struct AreaPlaneFrame {
  float3 origin = float3(0.0f);
  float3 axis_x = float3(1.0f, 0.0f, 0.0f);
  float3 axis_y = float3(0.0f, 1.0f, 0.0f);
  float3 axis_z = float3(0.0f, 0.0f, 1.0f);
  float radius = 1.0f;
};

AreaPlaneFrame area_plane_frame(const float3 &position,
                                const float3 &normal,
                                float radius_object,
                                float rotation);

/**
 * Area Plane whose X/Y follow the image texture on \a tri (∂P/∂u, ∂P/∂v), with \a rotation
 * (#MTex.rot) in that plane. Falls back to #area_plane_frame when the UV Jacobian is degenerate.
 */
AreaPlaneFrame area_plane_frame_from_triangle(const AreaPlaneTriangle &tri,
                                              const float3 &position,
                                              float radius_object,
                                              float rotation);

/**
 * Object space → brush-local for \a frame, matching #calc_brush_local_mat's scale convention:
 * a point #frame.radius from the origin along the tangent plane maps to local length 1.
 */
float4x4 area_plane_object_to_local(const AreaPlaneFrame &frame);

/**
 * One object-space rigid transform per \a accepted triangle that maps that triangle's plane
 * onto the dab triangle's plane along the UV net (then 3D seams). Identity for the dab itself
 * and for coplanar faces.
 */
Vector<float4x4> area_plane_unfold_matrices(Span<AreaPlaneTriangle> triangles,
                                            int dab_index,
                                            Span<int> accepted);

/**
 * Object space → brush-local, matching #calc_brush_local_mat's scale convention: a point
 * #radius_object from \a position along the tangent plane maps to local length 1.
 *
 * \param rotation: #MTex.rot in radians.
 */
float4x4 area_plane_local_mat(const float3 &position,
                              const float3 &normal,
                              float radius_object,
                              float rotation);

/** Geometric face normal of \a tri (object space), or zero if the triangle is degenerate. */
float3 area_plane_triangle_face_normal(const AreaPlaneTriangle &tri);

/**
 * Half-open UV coverage: strict interior, or a boundary texel whose supporting edges are
 * top-left after the triangle is oriented counter-clockwise in UV. Shared edges go to exactly
 * one of the two triangles; overlapping interiors can both return true.
 */
bool area_plane_uv_pixel_inside_triangle(const float2 uv[3], const float2 &p);

}  // namespace blender::ed::sculpt_paint
