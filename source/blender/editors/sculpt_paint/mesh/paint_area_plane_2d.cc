/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_area_plane_2d.hh"

#include <algorithm>
#include <cmath>

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_virtual_array.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "DEG_depsgraph_query.hh"

namespace blender::ed::sculpt_paint {

static constexpr float AREA_PLANE_HIT_EPS = 1e-6f;
static constexpr float AREA_PLANE_EDGE_EPS = 1e-5f;
static constexpr float AREA_PLANE_RADIUS_TOLERANCE = 1e-4f;

AreaPlaneMesh::AreaPlaneMesh(const Depsgraph &depsgraph,
                             const Object &ob,
                             const StringRef uv_map_name)
{
  const Object *ob_eval = DEG_get_evaluated(&depsgraph, &ob);
  if (ob_eval == nullptr) {
    return;
  }
  const Mesh *mesh = BKE_object_get_editmesh_eval_final(ob_eval);
  if (mesh == nullptr) {
    mesh = BKE_object_get_evaluated_mesh(ob_eval);
  }
  if (mesh == nullptr) {
    return;
  }

  StringRef uv_name = uv_map_name;
  if (uv_name.is_empty()) {
    uv_name = mesh->active_uv_map_name();
  }
  if (uv_name.is_empty()) {
    return;
  }

  const bke::AttributeAccessor attributes = mesh->attributes();
  if (!attributes.contains(uv_name)) {
    return;
  }
  const bke::AttributeReader<float2> uv_attr = attributes.lookup<float2>(uv_name,
                                                                        bke::AttrDomain::Corner);
  if (!uv_attr || uv_attr.varray.is_empty()) {
    return;
  }
  const VArraySpan<float2> uv_map(uv_attr.varray);

  const Span<int3> corner_tris = mesh->corner_tris();
  if (corner_tris.is_empty()) {
    return;
  }
  const Span<float3> positions = mesh->vert_positions();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> corner_normals = mesh->corner_normals();

  triangles_.reserve(corner_tris.size());
  for (const int i : corner_tris.index_range()) {
    const int3 &tri = corner_tris[i];
    AreaPlaneTriangle out;
    out.index = i;
    for (const int j : IndexRange(3)) {
      const int corner = tri[j];
      out.vert[j] = corner_verts[corner];
      out.uv[j] = uv_map[corner];
      out.position[j] = positions[corner_verts[corner]];
      out.normal[j] = corner_normals[corner];
    }
    triangles_.append(out);
  }
}

bool AreaPlaneMesh::is_valid() const
{
  return !triangles_.is_empty();
}

static bool barycentric_contains(const float w[3], const float eps)
{
  return w[0] >= -eps && w[1] >= -eps && w[2] >= -eps;
}

bool AreaPlaneMesh::hit_at_uv(const float2 &uv, AreaPlaneHit &r_hit) const
{
  r_hit = AreaPlaneHit{};
  int best_index = -1;
  float best_w[3] = {};

  for (const AreaPlaneTriangle &tri : triangles_) {
    float w[3];
    barycentric_weights_v2(tri.uv[0], tri.uv[1], tri.uv[2], uv, w);
    if (!barycentric_contains(w, AREA_PLANE_HIT_EPS)) {
      continue;
    }
    if (best_index < 0 || tri.index < best_index) {
      best_index = tri.index;
      best_w[0] = w[0];
      best_w[1] = w[1];
      best_w[2] = w[2];
    }
  }

  if (best_index < 0) {
    return false;
  }

  const AreaPlaneTriangle &tri = triangles_[best_index];
  r_hit.tri_index = best_index;
  r_hit.position = tri.position[0] * best_w[0] + tri.position[1] * best_w[1] +
                   tri.position[2] * best_w[2];
  float3 normal = tri.normal[0] * best_w[0] + tri.normal[1] * best_w[1] +
                  tri.normal[2] * best_w[2];
  if (math::length_squared(normal) < 1e-12f) {
    normal = math::cross(tri.position[1] - tri.position[0], tri.position[2] - tri.position[0]);
  }
  r_hit.normal = math::normalize(normal);
  if (math::length_squared(r_hit.normal) < 1e-12f) {
    return false;
  }
  return true;
}

static bool area_plane_triangle_uv_jacobian(const AreaPlaneTriangle &tri,
                                            float3 &r_dpdu,
                                            float3 &r_dpdv)
{
  const float2 duv1 = tri.uv[1] - tri.uv[0];
  const float2 duv2 = tri.uv[2] - tri.uv[0];
  const float3 e1 = tri.position[1] - tri.position[0];
  const float3 e2 = tri.position[2] - tri.position[0];
  const float det = duv1.x * duv2.y - duv2.x * duv1.y;
  if (std::abs(det) <= 1e-20f) {
    return false;
  }
  const float inv_det = 1.0f / det;
  r_dpdu = (e1 * duv2.y - e2 * duv1.y) * inv_det;
  r_dpdv = (e2 * duv1.x - e1 * duv2.x) * inv_det;
  return true;
}

static float radius_object_from_triangle(const AreaPlaneTriangle &tri, const float radius_uv)
{
  float3 dpdu;
  float3 dpdv;
  if (!area_plane_triangle_uv_jacobian(tri, dpdu, dpdv)) {
    return 0.0f;
  }
  return radius_uv * std::sqrt(math::length(math::cross(dpdu, dpdv)));
}

float AreaPlaneMesh::radius_object(const AreaPlaneHit &hit, const float radius_uv) const
{
  if (hit.tri_index < 0 || hit.tri_index >= triangles_.size()) {
    return 0.0f;
  }
  return radius_object_from_triangle(triangles_[hit.tri_index], radius_uv);
}

Vector<int> AreaPlaneMesh::triangles_in_sphere(const float4x4 &object_to_brush) const
{
  Vector<int> result;
  const float radius = 1.0f + AREA_PLANE_RADIUS_TOLERANCE;
  const float radius_sq = radius * radius;

  for (const AreaPlaneTriangle &tri : triangles_) {
    float3 local[3];
    for (const int i : IndexRange(3)) {
      local[i] = tri.position[i];
      mul_m4_v3(object_to_brush.ptr(), local[i]);
    }
    float3 nearest;
    closest_on_tri_to_point_v3(nearest, float3(0.0f), local[0], local[1], local[2]);
    if (math::length_squared(nearest) <= radius_sq) {
      result.append(tri.index);
    }
  }
  return result;
}

AreaPlaneTriangle AreaPlaneMesh::triangle(const int index) const
{
  return triangles_[index];
}

float3 area_plane_triangle_face_normal(const AreaPlaneTriangle &tri)
{
  return math::normalize(
      math::cross(tri.position[1] - tri.position[0], tri.position[2] - tri.position[0]));
}

static float4x4 area_plane_object_to_local_from_axes(const float3 &origin,
                                                     const float3 &axis_x,
                                                     const float3 &axis_y,
                                                     const float3 &axis_z,
                                                     const float radius_object)
{
  float4x4 mat = float4x4::identity();
  mat[0] = float4(axis_x, 0.0f);
  mat[1] = float4(axis_y, 0.0f);
  mat[2] = float4(axis_z, 0.0f);
  mat[3] = float4(origin, 1.0f);

  normalize_m4(mat.ptr());
  float4x4 scale = float4x4::identity();
  scale_m4_fl(scale.ptr(), radius_object);
  const float4x4 tmat = mat * scale;
  return math::invert(tmat);
}

AreaPlaneFrame area_plane_frame(const float3 &position,
                                const float3 &normal,
                                const float radius_object,
                                const float rotation)
{
  AreaPlaneFrame frame;
  frame.origin = position;
  frame.radius = radius_object;
  frame.axis_z = math::normalize(normal);

  const float3 reference = std::abs(frame.axis_z.x) < 0.9f ? float3(1.0f, 0.0f, 0.0f) :
                                                             float3(0.0f, 0.0f, 1.0f);
  const float3 axis_y = math::normalize(math::cross(frame.axis_z, reference));
  const float3 axis_x = math::cross(axis_y, frame.axis_z);
  const float cos_rot = std::cos(rotation);
  const float sin_rot = std::sin(rotation);
  frame.axis_x = math::normalize(axis_x * cos_rot + axis_y * sin_rot);
  frame.axis_y = math::normalize(axis_y * cos_rot - axis_x * sin_rot);
  return frame;
}

AreaPlaneFrame area_plane_frame_from_triangle(const AreaPlaneTriangle &tri,
                                              const float3 &position,
                                              const float radius_object,
                                              const float rotation)
{
  float3 dpdu;
  float3 dpdv;
  const float3 face_normal = area_plane_triangle_face_normal(tri);
  if (!area_plane_triangle_uv_jacobian(tri, dpdu, dpdv)) {
    return area_plane_frame(position, face_normal, radius_object, rotation);
  }

  float3 axis_x = dpdu;
  if (math::length_squared(axis_x) < 1e-20f) {
    return area_plane_frame(position, face_normal, radius_object, rotation);
  }
  axis_x = math::normalize(axis_x);

  float3 axis_y = dpdv - axis_x * math::dot(axis_x, dpdv);
  if (math::length_squared(axis_y) < 1e-20f) {
    axis_y = math::cross(face_normal, axis_x);
    if (math::length_squared(axis_y) < 1e-20f) {
      return area_plane_frame(position, face_normal, radius_object, rotation);
    }
  }
  axis_y = math::normalize(axis_y);

  AreaPlaneFrame frame;
  frame.origin = position;
  frame.radius = radius_object;
  frame.axis_z = math::normalize(math::cross(axis_x, axis_y));
  const float cos_rot = std::cos(rotation);
  const float sin_rot = std::sin(rotation);
  frame.axis_x = math::normalize(axis_x * cos_rot + axis_y * sin_rot);
  frame.axis_y = math::normalize(axis_y * cos_rot - axis_x * sin_rot);
  return frame;
}

float4x4 area_plane_object_to_local(const AreaPlaneFrame &frame)
{
  return area_plane_object_to_local_from_axes(
      frame.origin, frame.axis_x, frame.axis_y, frame.axis_z, frame.radius);
}

static bool area_plane_shared_edge(const AreaPlaneTriangle &a,
                                   const AreaPlaneTriangle &b,
                                   float3 &r_p0,
                                   float3 &r_p1)
{
  float3 pts[2];
  int n = 0;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      if (a.vert[i] == b.vert[j] && a.vert[i] >= 0) {
        pts[n++] = a.position[i];
        if (n == 2) {
          r_p0 = pts[0];
          r_p1 = pts[1];
          return true;
        }
      }
    }
  }
  return false;
}

float3 area_plane_unfold_to_dab_plane(const float3 &point,
                                      const AreaPlaneTriangle &face,
                                      const AreaPlaneTriangle &dab,
                                      const float3 &dab_origin)
{
  if (face.index == dab.index) {
    return point;
  }

  const float3 n_d = area_plane_triangle_face_normal(dab);
  const float3 n_f = area_plane_triangle_face_normal(face);
  if (math::length_squared(n_d) < 1e-12f || math::length_squared(n_f) < 1e-12f) {
    return point;
  }

  /* Axis n_f × n_d rotates the face normal onto the dab normal (right-hand). */
  const float3 n_cross = math::cross(n_f, n_d);
  const float sin_a = math::length(n_cross);
  const float cos_a = math::clamp(math::dot(n_f, n_d), -1.0f, 1.0f);
  /* Parallel (or anti-parallel): no unique hinge. */
  if (sin_a < 1e-5f) {
    return point;
  }

  float3 axis = n_cross / sin_a;
  const float theta = std::atan2(sin_a, cos_a);
  if (theta < 1e-5f) {
    return point;
  }

  const float d_d = math::dot(n_d, dab_origin);
  float3 hinge_point;
  float3 edge_p0;
  float3 edge_p1;
  if (area_plane_shared_edge(face, dab, edge_p0, edge_p1)) {
    const float3 edge = edge_p1 - edge_p0;
    if (math::length_squared(edge) > 1e-20f) {
      const float3 edge_dir = math::normalize(edge);
      hinge_point = edge_p0 + edge_dir * math::dot(edge_dir, dab_origin - edge_p0);
    }
    else {
      hinge_point = edge_p0;
    }
  }
  else {
    const float d_f = math::dot(n_f, face.position[0]);
    const float3 n1xn2 = math::cross(n_d, n_f);
    const float3 hinge_any = math::cross(d_d * n_f - d_f * n_d, n1xn2) /
                             math::length_squared(n1xn2);
    hinge_point = hinge_any + axis * math::dot(axis, dab_origin - hinge_any);
  }

  float3 v = point - hinge_point;
  float3 v_rot;
  rotate_normalized_v3_v3v3fl(v_rot, v, axis, theta);
  float3 unfolded = hinge_point + v_rot;
  unfolded += n_d * (d_d - math::dot(n_d, unfolded));
  return unfolded;
}

float4x4 area_plane_local_mat(const float3 &position,
                              const float3 &normal,
                              const float radius_object,
                              const float rotation)
{
  return area_plane_object_to_local(area_plane_frame(position, normal, radius_object, rotation));
}

static bool is_top_left_edge(const float2 &a, const float2 &b)
{
  const float dy = b.y - a.y;
  const float dx = b.x - a.x;
  if (std::abs(dy) <= 1e-12f) {
    return dx < 0.0f;
  }
  return dy > 0.0f;
}

bool area_plane_uv_pixel_inside_triangle(const float2 uv[3], const float2 &p)
{
  float w[3];
  barycentric_weights_v2(uv[0], uv[1], uv[2], p, w);
  if (w[0] < -AREA_PLANE_EDGE_EPS || w[1] < -AREA_PLANE_EDGE_EPS || w[2] < -AREA_PLANE_EDGE_EPS) {
    return false;
  }
  if (w[0] > AREA_PLANE_EDGE_EPS && w[1] > AREA_PLANE_EDGE_EPS && w[2] > AREA_PLANE_EDGE_EPS) {
    return true;
  }

  float2 v0 = uv[0];
  float2 v1 = uv[1];
  float2 v2 = uv[2];
  const float signed_area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
  if (signed_area < 0.0f) {
    std::swap(v1, v2);
    std::swap(w[1], w[2]);
  }

  if (w[0] <= AREA_PLANE_EDGE_EPS && !is_top_left_edge(v1, v2)) {
    return false;
  }
  if (w[1] <= AREA_PLANE_EDGE_EPS && !is_top_left_edge(v2, v0)) {
    return false;
  }
  if (w[2] <= AREA_PLANE_EDGE_EPS && !is_top_left_edge(v0, v1)) {
    return false;
  }
  return true;
}

}  // namespace blender::ed::sculpt_paint
