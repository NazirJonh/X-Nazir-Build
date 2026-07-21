/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Reading a built Curve Patch back out as ordinary geometry: the ribbon as a mesh with UVs, the
 * stamp layout as a point cloud.
 *
 * Neither path stamps anything, touches a sculpt session or reads a brush -- they turn what
 * #curve_patch_geometry_build already produced into data-blocks a script can consume.
 */

#include <algorithm>

#include "DNA_mesh_types.h"
#include "DNA_pointcloud_types.h"

#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_curve_patch.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

namespace blender::bke {

/* -------------------------------------------------------------------- */
/** \name Ribbon to Mesh
 * \{ */

/* The across-curve direction at each polyline sample, from the SMOOTHED normal field -- the same
 * derivation `curve_patch_frames_build()` feeds the rasterizer. A discontinuous field would break
 * `u` wherever the curve crosses an edge at an angle, and a mesh built from a different frame than
 * the patch samples in would not be the strip the patch stamps along. */
static Vector<float3> ribbon_binormals_from_spline(const CurvePatchSpline &spline)
{
  const int point_num = int(spline.poly_3d.size());
  const bool have_smooth = spline.normals_smooth_3d.size() == point_num;
  if (!have_smooth) {
    /* No surface snapshot: the rasterizer derives `cross(tangent, plane_normal)` itself from an
     * empty span, and so must this. */
    return {};
  }

  Vector<float3> binormals(point_num);
  for (const int i : IndexRange(point_num)) {
    const float3 b = math::cross(spline.tangents_3d[i], spline.normals_smooth_3d[i]);
    const float length = math::length(b);
    binormals[i] = length > 1e-7f ?
                       b / length :
                       math::normalize(math::cross(spline.tangents_3d[i], spline.plane_normal));
  }
  return binormals;
}

Mesh *curve_patch_geometry_to_mesh(const CurvePatchGeometry &geometry,
                                   const CurvePatchParams &params,
                                   const bool caps_enabled,
                                   const float world_cap_start,
                                   const float world_cap_end)
{
  if (geometry.spline.is_empty()) {
    return nullptr;
  }

  const Vector<float3> binormals = ribbon_binormals_from_spline(geometry.spline);
  CurvePatchRibbonGrid grid;
  if (!curve_patch_ribbon_grid_build(geometry.spline,
                                     geometry.ribbon_radius,
                                     binormals.as_span(),
                                     geometry.ribbon_end_margin,
                                     geometry.ribbon_end_margin,
                                     grid))
  {
    return nullptr;
  }
  if (grid.rows < 2 || grid.cols < 2) {
    return nullptr;
  }

  const int verts_num = grid.rows * grid.cols;
  const int faces_num = (grid.rows - 1) * (grid.cols - 1);
  Mesh *mesh = BKE_mesh_new_nomain(verts_num, 0, faces_num, faces_num * 4);
  mesh_smooth_set(*mesh, true);

  mesh->vert_positions_for_write().copy_from(grid.positions.as_span());

  offset_indices::fill_constant_group_size(4, 0, mesh->face_offsets_for_write());
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int r : IndexRange(grid.rows - 1)) {
    for (const int c : IndexRange(grid.cols - 1)) {
      const int face = r * (grid.cols - 1) + c;
      const int v00 = r * grid.cols + c;
      corner_verts[face * 4 + 0] = v00;
      corner_verts[face * 4 + 1] = v00 + 1;
      corner_verts[face * 4 + 2] = v00 + grid.cols + 1;
      corner_verts[face * 4 + 3] = v00 + grid.cols;
    }
  }

  /* Per-vertex texture coordinates, resolved exactly the way the relief resolves them
   * (`paint_curve_patch_sampler.cc`): the zone and along-curve coordinate come from
   * #curve_patch_texture_zone_at, then the two axes are swapped if the patch asks for it. The
   * texture slot's own size/offset are deliberately NOT applied -- those are mapping settings, and
   * the mesh carries the patch's coordinates for the caller to map as they please. */
  const float total_length = geometry.spline.total_length();
  Array<float2> vert_uv(verts_num);
  Array<bool> vert_uv_valid(verts_num);
  for (const int i : IndexRange(verts_num)) {
    const float u = grid.uv[i].x;
    const float s = grid.uv[i].y;
    const float falloff_radius_at_s = geometry.spline.radius_at(s) * params.radius;
    const CurvePatchTextureZoneSample zone = curve_patch_texture_zone_at(s,
                                                                        total_length,
                                                                        falloff_radius_at_s,
                                                                        caps_enabled,
                                                                        world_cap_start,
                                                                        world_cap_end,
                                                                        params.length_mode,
                                                                        params.length_repeat,
                                                                        geometry.spline.cyclic);
    vert_uv_valid[i] = zone.valid;
    float2 uv(u, zone.v);
    if (params.swap_axis) {
      std::swap(uv.x, uv.y);
    }
    vert_uv[i] = uv;
  }

  MutableAttributeAccessor attributes = mesh->attributes_for_write();
  SpanAttributeWriter<float2> uv_map = attributes.lookup_or_add_for_write_only_span<float2>(
      "UVMap", AttrDomain::Corner);
  if (uv_map) {
    for (const int corner : corner_verts.index_range()) {
      const int vert = corner_verts[corner];
      /* A degenerate zone (two oversized caps squeezing the middle to nothing) has no coordinate to
       * report; the relief leaves such a stretch untouched, and here the corner falls back to the
       * across-curve coordinate alone rather than to an uninitialized value. */
      uv_map.span[corner] = vert_uv_valid[vert] ? vert_uv[vert] : float2(grid.uv[vert].x, 0.0f);
    }
    uv_map.finish();
  }

  mesh_calc_edges(*mesh, false, false);
  return mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stamps to Points
 * \{ */

PointCloud *curve_patch_geometry_to_stamp_points(const CurvePatchGeometry &geometry)
{
  if (geometry.stamps.is_empty()) {
    return nullptr;
  }

  const int stamp_num = int(geometry.stamps.size());
  PointCloud *points = BKE_pointcloud_new_nomain(stamp_num);
  MutableAttributeAccessor attributes = points->attributes_for_write();

  MutableSpan<float3> positions = points->positions_for_write();
  SpanAttributeWriter<float> radii = attributes.lookup_or_add_for_write_only_span<float>(
      "radius", AttrDomain::Point);
  SpanAttributeWriter<float> rotation = attributes.lookup_or_add_for_write_only_span<float>(
      "rotation", AttrDomain::Point);
  SpanAttributeWriter<float> strength = attributes.lookup_or_add_for_write_only_span<float>(
      "strength", AttrDomain::Point);
  SpanAttributeWriter<int> texture_index = attributes.lookup_or_add_for_write_only_span<int>(
      "texture_index", AttrDomain::Point);
  SpanAttributeWriter<float3> axis_v = attributes.lookup_or_add_for_write_only_span<float3>(
      "axis_v", AttrDomain::Point);
  SpanAttributeWriter<float3> axis_u = attributes.lookup_or_add_for_write_only_span<float3>(
      "axis_u", AttrDomain::Point);

  for (const int i : IndexRange(stamp_num)) {
    const CurvePatchStamp &stamp = geometry.stamps[i];
    positions[i] = stamp.origin;
    if (radii) {
      radii.span[i] = stamp.half_extent;
    }
    if (rotation) {
      rotation.span[i] = stamp.angle;
    }
    if (strength) {
      strength.span[i] = stamp.strength;
    }
    if (texture_index) {
      texture_index.span[i] = stamp.tex_index;
    }
    if (axis_v) {
      axis_v.span[i] = stamp.axis_v;
    }
    if (axis_u) {
      axis_u.span[i] = stamp.axis_u;
    }
  }

  radii.finish();
  rotation.finish();
  strength.finish();
  texture_index.finish();
  axis_v.finish();
  axis_u.finish();
  return points;
}

/** \} */

}  // namespace blender::bke
