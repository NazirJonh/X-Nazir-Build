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
 * - Tangent frame: Z = surface normal at the dab center. X/Y from a stable object-space
 *   reference axis projected into the tangent plane (object X, or object Z near the singularity
 *   |Z·X| >= 0.9). Not UV stroke direction, not the UV Jacobian axes. #MTex.rot then rotates in
 *   that tangent plane. The |Z·X| threshold can flip the frame; scale and projection stay valid.
 */

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
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
};

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

/**
 * Half-open UV coverage: strict interior, or a boundary texel whose supporting edges are
 * top-left after the triangle is oriented counter-clockwise in UV. Shared edges go to exactly
 * one of the two triangles; overlapping interiors can both return true.
 */
bool area_plane_uv_pixel_inside_triangle(const float2 uv[3], const float2 &p);

}  // namespace blender::ed::sculpt_paint
