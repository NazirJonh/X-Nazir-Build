/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_index_range.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_bvhutils.hh"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_mesh_remesh_voxel.hh" /* own include */
#include "BKE_mesh_sample.hh"
#include "BKE_modifier.hh"
#include "BKE_report.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#ifdef WITH_OPENVDB
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/MeshToVolume.h>
#  include <openvdb/tools/VolumeToMesh.h>
#endif

#ifdef WITH_QUADRIFLOW
#  include "quadriflow_capi.hpp"
#endif

namespace blender {

namespace bke {

/* Period-4 (4-RoSy) complex helpers. A line direction at angle `θ` in a local
 * tangent basis is encoded as `exp(i·4θ)`, which collapses the four symmetric
 * directions of a cross into a single value that can be averaged linearly. */
static float2 rosy4_polar(const float r, const float angle)
{
  return float2(r * std::cos(angle), r * std::sin(angle));
}
static float2 rosy4_mul(const float2 a, const float2 b)
{
  return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void mesh_curvature_guide_field(const Mesh &mesh,
                                const float strength,
                                MutableSpan<float3> r_dirs,
                                MutableSpan<float> r_weights)
{
  const int verts_num = mesh.verts_num;
  r_dirs.fill(float3(0.0f));
  r_weights.fill(0.0f);
  if (verts_num == 0 || mesh.faces_num == 0) {
    return;
  }

  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> vert_normals = mesh.vert_normals();
  const Span<float3> face_normals = mesh.face_normals();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int2> edges = mesh.edges();

  /* Face areas weight the normal-variation tensor so larger faces count more. */
  Array<float> face_area(faces.size());
  threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
    for (const int f : range) {
      face_area[f] = mesh::face_area_calc(positions, corner_verts.slice(faces[f]));
    }
  });

  Array<int> v2f_offsets;
  Array<int> v2f_indices;
  const GroupedSpan<int> vert_to_face = mesh::build_vert_to_face_map(
      faces, corner_verts, verts_num, v2f_offsets, v2f_indices);

  Array<int> v2e_offsets;
  Array<int> v2e_indices;
  const GroupedSpan<int> vert_to_edge = mesh::build_vert_to_edge_map(
      edges, verts_num, v2e_offsets, v2e_indices);

  Array<float3> basis_u(verts_num);
  Array<float3> basis_v(verts_num);
  /* Encoded line field `z = anisotropy · exp(i·4·θ_max)` in the local basis; its
   * magnitude is the curvature anisotropy used as confidence. */
  Array<float2> field(verts_num);

  /* NOTE: For very noisy sculpt input, pre-smoothing the input normals a couple
   * of iterations would further stabilize this estimate. The period-4 diffusion
   * below already smooths the resulting field, so it is left out for now. */
  threading::parallel_for(IndexRange(verts_num), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 n = vert_normals[v];
      float u[3], w[3];
      ortho_basis_v3v3_v3(u, w, n);
      const float3 uf(u);
      const float3 wf(w);
      basis_u[v] = uf;
      basis_v[v] = wf;

      /* Variation of face normals projected to the tangent plane gives a 2x2
       * symmetric tensor `[[a, b], [b, c]]`. */
      float a = 0.0f, b = 0.0f, c = 0.0f, wsum = 0.0f;
      for (const int f : vert_to_face[v]) {
        const float3 dn = face_normals[f] - n;
        const float du = math::dot(dn, uf);
        const float dv = math::dot(dn, wf);
        const float fw = face_area[f];
        a += fw * du * du;
        b += fw * du * dv;
        c += fw * dv * dv;
        wsum += fw;
      }
      if (wsum > 0.0f) {
        const float inv = 1.0f / wsum;
        a *= inv;
        b *= inv;
        c *= inv;
      }
      /* Closed-form eigen-decomposition: the larger eigenvalue corresponds to
       * the maximum-curvature direction at `θ = ½·atan2(2b, a-c)`, and the
       * eigenvalue gap over their sum is the confidence. This *relative*
       * anisotropy `(λ1-λ2)/(λ1+λ2)` is scale-invariant: gently curved regions
       * (limbs, torso) guide just as strongly as sharp creases, instead of
       * being drowned out by whichever crease holds the global maximum. It is
       * ≈1 on cylinder-like surfaces and ≈0 on flat/spherical (umbilic) ones,
       * where the direction is undefined. Truly flat regions, whose tensor is
       * pure numeric noise, are zeroed by the threshold; remaining noise has
       * incoherent directions and collapses in the diffusion below. */
      const float disc = std::sqrt(0.25f * (a - c) * (a - c) + b * b);
      const float theta = 0.5f * std::atan2(2.0f * b, a - c);
      const float total = a + c;
      const float anisotropy = (total > 1e-10f) ?
                                   math::clamp(2.0f * disc / total, 0.0f, 1.0f) :
                                   0.0f;
      field[v] = rosy4_polar(anisotropy, 4.0f * theta);
    }
  });

  /* Smooth the line field across the surface. Raw directions cannot be averaged
   * (4-RoSy neighbors 90° apart would cancel), so a neighbor value is parallel-
   * transported into the current basis (a rotation by `4·φ`) before being added.
   * The magnitude doubles as confidence, so disagreeing neighbors damp the
   * weight where the flow is ambiguous. */
  const int iterations = 10;
  const float lambda = 0.4f;
  Array<float2> tmp(verts_num);
  for (int iteration = 0; iteration < iterations; iteration++) {
    threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
      for (const int v : range) {
        const float3 uv = basis_u[v];
        const float3 vv = basis_v[v];
        const float3 nv = vert_normals[v];
        float2 acc(0.0f, 0.0f);
        float wsum = 0.0f;
        for (const int e : vert_to_edge[v]) {
          const int2 edge = edges[e];
          const int nb = (edge[0] == v) ? edge[1] : edge[0];
          float3 uj = basis_u[nb];
          uj = uj - nv * math::dot(uj, nv);
          const float len = math::length(uj);
          if (len < 1e-8f) {
            continue;
          }
          uj /= len;
          const float phi = std::atan2(math::dot(uj, vv), math::dot(uj, uv));
          acc += rosy4_mul(field[nb], rosy4_polar(1.0f, 4.0f * phi));
          wsum += 1.0f;
        }
        const float2 avg = (wsum > 0.0f) ? acc / wsum : field[v];
        tmp[v] = field[v] * (1.0f - lambda) + avg * lambda;
      }
    });
    std::swap(field, tmp);
  }

  /* Decode the smoothed field to object-space directions and confidence weights. */
  threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
    for (const int v : range) {
      const float2 z = field[v];
      const float mag = math::length(z);
      if (mag < 1e-6f) {
        continue;
      }
      const float theta = 0.25f * std::atan2(z.y, z.x);
      float3 dir = basis_u[v] * std::cos(theta) + basis_v[v] * std::sin(theta);
      const float3 n = vert_normals[v];
      dir = dir - n * math::dot(dir, n);
      const float len = math::length(dir);
      if (len < 1e-8f) {
        continue;
      }
      r_dirs[v] = dir / len;
      /* The orientation solver blends `(1-w)·smoothed + w·guide` per iteration,
       * so mid-confidence weights are easily overpowered by the smoothing term
       * and the field relaxes back to a featureless uniform grid. The square
       * root boosts coherent mid-range confidence enough to actually steer the
       * solver, while low-confidence noise still fades out. */
      r_weights[v] = math::clamp(std::sqrt(mag) * strength, 0.0f, 1.0f);
    }
  });
}

void mesh_guide_strokes_field(const Mesh &mesh,
                              const Span<float3> stroke_points,
                              const OffsetIndices<int> stroke_offsets,
                              const float radius,
                              const float strength,
                              MutableSpan<float3> r_dirs,
                              MutableSpan<float> r_weights)
{
  if (stroke_offsets.size() == 0 || radius <= 0.0f || strength <= 0.0f) {
    return;
  }
  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> vert_normals = mesh.vert_normals();
  const float radius_sq = radius * radius;

  /* Per-stroke bounds expanded by `radius`; vertices outside cannot be in range,
   * so they skip the stroke's segment scan entirely. */
  Array<float3> stroke_min(stroke_offsets.size());
  Array<float3> stroke_max(stroke_offsets.size());
  for (const int s : stroke_offsets.index_range()) {
    float3 lo(FLT_MAX);
    float3 hi(-FLT_MAX);
    for (const int i : stroke_offsets[s]) {
      lo = math::min(lo, stroke_points[i]);
      hi = math::max(hi, stroke_points[i]);
    }
    stroke_min[s] = lo - float3(radius);
    stroke_max[s] = hi + float3(radius);
  }

  /* For each vertex, find the nearest stroke segment within `radius` and use its
   * tangent as a soft orientation constraint. Strokes are few, so a direct
   * per-vertex scan keeps this simple and race-free (each vertex is independent),
   * and combines with any existing guidance (e.g. curvature) by keeping the
   * stronger weight. */
  threading::parallel_for(positions.index_range(), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 p = positions[v];
      float best_dist_sq = radius_sq;
      float3 best_tangent(0.0f);
      bool found = false;
      for (const int s : stroke_offsets.index_range()) {
        const IndexRange points = stroke_offsets[s];
        if (points.size() < 2) {
          continue;
        }
        if (p.x < stroke_min[s].x || p.y < stroke_min[s].y || p.z < stroke_min[s].z ||
            p.x > stroke_max[s].x || p.y > stroke_max[s].y || p.z > stroke_max[s].z)
        {
          continue;
        }
        for (const int i : points.drop_back(1)) {
          const float3 a = stroke_points[i];
          const float3 b = stroke_points[i + 1];
          const float d_sq = dist_squared_to_line_segment_v3(p, a, b);
          if (d_sq < best_dist_sq) {
            best_dist_sq = d_sq;
            best_tangent = b - a;
            found = true;
          }
        }
      }
      if (!found) {
        continue;
      }
      const float3 n = vert_normals[v];
      float3 t = best_tangent - n * math::dot(best_tangent, n);
      if (math::length_squared(t) < 1e-12f) {
        continue;
      }
      t = math::normalize(t);
      /* Smoothstep falloff: full strength on the stroke, zero at `radius`. */
      const float x = math::clamp(1.0f - std::sqrt(best_dist_sq) / radius, 0.0f, 1.0f);
      const float w = strength * (x * x * (3.0f - 2.0f * x));
      if (w > r_weights[v]) {
        r_weights[v] = w;
        r_dirs[v] = t;
      }
    }
  });
}

void mesh_face_set_boundaries_field(const Mesh &mesh,
                                    MutableSpan<float3> r_dirs,
                                    MutableSpan<float> r_weights,
                                    MutableSpan<float> r_pin_weights)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArray<int> face_sets = *attributes.lookup_or_default<int>(
      ".sculpt_face_set", bke::AttrDomain::Face, 0);
  if (!face_sets) {
    return;
  }

  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  const Span<int> corner_edges = mesh.corner_edges();
  const OffsetIndices<int> faces = mesh.faces();

  Array<int> edge_face_1(edges.size(), -1);
  Array<int> edge_face_2(edges.size(), -1);

  for (const int f : faces.index_range()) {
    for (const int corner : faces[f]) {
      const int e = corner_edges[corner];
      if (edge_face_1[e] == -1) {
        edge_face_1[e] = f;
      }
      else {
        edge_face_2[e] = f;
      }
    }
  }

  /* Collect up to two incident boundary edges per vertex. Vertices with
   * exactly two are interior chain vertices; one means a chain end, more than
   * two a junction between face set regions. */
  const int verts_num = mesh.verts_num;
  Array<int2> incident(verts_num, int2(-1, -1));
  Array<int> incident_num(verts_num, 0);
  for (const int e : edges.index_range()) {
    const int f1 = edge_face_1[e];
    const int f2 = edge_face_2[e];
    if (f1 == -1 || f2 == -1 || face_sets[f1] == face_sets[f2]) {
      continue;
    }
    for (const int v : {edges[e][0], edges[e][1]}) {
      if (incident_num[v] < 2) {
        incident[v][incident_num[v]] = e;
      }
      incident_num[v]++;
    }
  }

  /* Per-vertex tangent along the boundary. A single edge direction zigzags on
   * a triangulated boundary; averaging both incident segments (with the second
   * one flipped, so both point "along" the chain) gives the direction of the
   * underlying feature curve. The positional pin makes the output geometry
   * pass through the boundary while still sliding along it, the same mechanism
   * `preserve_boundary` uses for open mesh boundaries. */
  for (const int v : IndexRange(verts_num)) {
    if (incident_num[v] == 0) {
      continue;
    }
    const int2 e0 = edges[incident[v][0]];
    const float3 d0 = positions[(e0[0] == v) ? e0[1] : e0[0]] - positions[v];
    float3 tangent = d0;
    if (incident_num[v] == 2) {
      const int2 e1 = edges[incident[v][1]];
      const float3 d1 = positions[(e1[0] == v) ? e1[1] : e1[0]] - positions[v];
      tangent = d0 - d1;
      if (math::length_squared(tangent) < 1e-12f) {
        /* Degenerate hairpin; fall back to a single segment. */
        tangent = d0;
      }
    }
    if (math::length_squared(tangent) < 1e-12f) {
      continue;
    }
    r_dirs[v] = math::normalize(tangent);
    r_weights[v] = 1.0f;
    if (!r_pin_weights.is_empty()) {
      r_pin_weights[v] = 1.0f;
    }
  }

  /* Smooth the tangents along each chain so the constraint describes the
   * feature curve rather than individual jagged edges. Junctions and chain
   * ends stay as-is to anchor the smoothing. The sign flip aligns neighbors
   * before averaging (the constraint is 4-RoSy, so sign itself is free). */
  Array<float3> smoothed(verts_num);
  for (int iteration = 0; iteration < 3; iteration++) {
    for (const int v : IndexRange(verts_num)) {
      smoothed[v] = r_dirs[v];
      if (incident_num[v] != 2) {
        continue;
      }
      float3 acc = r_dirs[v];
      for (const int i : IndexRange(2)) {
        const int2 e = edges[incident[v][i]];
        const int nb = (e[0] == v) ? e[1] : e[0];
        if (incident_num[nb] == 0) {
          continue;
        }
        const float3 d = r_dirs[nb];
        acc += (math::dot(d, r_dirs[v]) < 0.0f) ? -d : d;
      }
      if (math::length_squared(acc) > 1e-12f) {
        smoothed[v] = math::normalize(acc);
      }
    }
    for (const int v : IndexRange(verts_num)) {
      if (incident_num[v] == 2) {
        r_dirs[v] = smoothed[v];
      }
    }
  }
}

void mesh_curvature_density_field(const Mesh &mesh,
                                  const float adaptivity,
                                  MutableSpan<float> r_scales)
{
  const int verts_num = mesh.verts_num;
  r_scales.fill(1.0f);
  if (verts_num == 0 || mesh.faces_num == 0 || adaptivity <= 0.0f) {
    return;
  }

  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> vert_normals = mesh.vert_normals();
  const Span<float3> face_normals = mesh.face_normals();
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int2> edges = mesh.edges();

  Array<float> face_area(faces.size());
  threading::parallel_for(faces.index_range(), 2048, [&](const IndexRange range) {
    for (const int f : range) {
      face_area[f] = mesh::face_area_calc(positions, corner_verts.slice(faces[f]));
    }
  });

  Array<int> v2f_offsets;
  Array<int> v2f_indices;
  const GroupedSpan<int> vert_to_face = mesh::build_vert_to_face_map(
      faces, corner_verts, verts_num, v2f_offsets, v2f_indices);

  Array<int> v2e_offsets;
  Array<int> v2e_indices;
  const GroupedSpan<int> vert_to_edge = mesh::build_vert_to_edge_map(
      edges, verts_num, v2e_offsets, v2e_indices);

  /* Absolute curvature estimate: the RMS tangential deviation of the one-ring
   * face normals is `≈ curvature · ring radius`, so dividing by the average
   * incident edge length removes the dependence on the input tessellation
   * density. Constant factors cancel in the median normalization below. */
  Array<float> curvature(verts_num, 0.0f);
  threading::parallel_for(IndexRange(verts_num), 1024, [&](const IndexRange range) {
    for (const int v : range) {
      const float3 n = vert_normals[v];
      float total = 0.0f;
      float wsum = 0.0f;
      for (const int f : vert_to_face[v]) {
        const float3 dn = face_normals[f] - n;
        const float normal_dev_sq = math::length_squared(dn);
        const float along_n = math::dot(dn, n);
        const float fw = face_area[f];
        total += fw * math::max(normal_dev_sq - along_n * along_n, 0.0f);
        wsum += fw;
      }
      if (wsum > 0.0f) {
        total /= wsum;
      }
      float ring = 0.0f;
      for (const int e : vert_to_edge[v]) {
        const int2 edge = edges[e];
        ring += math::distance(positions[edge[0]], positions[edge[1]]);
      }
      if (!vert_to_edge[v].is_empty()) {
        ring /= float(vert_to_edge[v].size());
      }
      if (ring > 1e-12f) {
        curvature[v] = std::sqrt(total) / ring;
      }
    }
  });

  /* The median curvature maps to scale 1, so roughly half of the surface gets
   * finer quads and half coarser and the total face count stays close to the
   * target. A mean would be dominated by a few sharp creases. */
  Vector<float> sorted;
  sorted.reserve(verts_num);
  for (const int v : IndexRange(verts_num)) {
    if (curvature[v] > 0.0f) {
      sorted.append(curvature[v]);
    }
  }
  if (sorted.is_empty()) {
    return;
  }
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  const float median = sorted[sorted.size() / 2];
  if (median <= 0.0f) {
    return;
  }

  threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
    for (const int v : range) {
      /* Edge length `∝ 1/sqrt(curvature)`: adaptive but less extreme than the
       * theoretically optimal `1/curvature`, which starves flat regions. The
       * lower bound on the curvature keeps flat regions from blowing past the
       * clamp before `adaptivity` is applied. */
      const float k = math::max(curvature[v], median * 0.0625f);
      const float scale_full = math::clamp(std::sqrt(median / k), 0.5f, 2.0f);
      r_scales[v] = 1.0f + (scale_full - 1.0f) * adaptivity;
    }
  });

  /* Smooth the result so quad size changes gradually; abrupt scale jumps read
   * as distorted quads in the output. */
  Array<float> tmp(verts_num);
  for (int iteration = 0; iteration < 5; iteration++) {
    threading::parallel_for(IndexRange(verts_num), 4096, [&](const IndexRange range) {
      for (const int v : range) {
        float acc = 0.0f;
        int num = 0;
        for (const int e : vert_to_edge[v]) {
          const int2 edge = edges[e];
          acc += r_scales[(edge[0] == v) ? edge[1] : edge[0]];
          num++;
        }
        tmp[v] = (num > 0) ? 0.5f * r_scales[v] + 0.5f * (acc / float(num)) : r_scales[v];
      }
    });
    r_scales.copy_from(tmp);
  }
}

void mesh_relax_reproject(Mesh &mesh,
                          const Mesh &source,
                          const int iterations,
                          const float factor,
                          const float sharp_angle)
{
  if (iterations <= 0 || factor <= 0.0f || mesh.verts_num == 0 || source.faces_num == 0) {
    return;
  }
  const int verts_num = mesh.verts_num;
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  const Span<int2> edges = mesh.edges();
  const Span<int> corner_edges = mesh.corner_edges();
  const OffsetIndices<int> faces = mesh.faces();

  /* Vertices on open boundaries (including the symmetry seam of a bisected
   * mesh) and non-manifold edges stay fixed; smoothing them would pull the
   * boundary inward. Vertices on sharp edges are fixed as well: the Laplacian
   * pulls them across the crease and the closest-point projection then cuts
   * the corner, visibly rounding shapes like cube edges. */
  const Span<float3> face_normals = mesh.face_normals();
  const float cos_sharp = std::cos(sharp_angle);
  Array<int> edge_face_count(edges.size(), 0);
  Array<int2> edge_faces(edges.size(), int2(-1, -1));
  for (const int f : faces.index_range()) {
    for (const int corner : faces[f]) {
      const int e = corner_edges[corner];
      if (edge_face_count[e] < 2) {
        edge_faces[e][edge_face_count[e]] = f;
      }
      edge_face_count[e]++;
    }
  }
  Array<bool> pinned(verts_num, false);
  for (const int e : edges.index_range()) {
    bool pin = edge_face_count[e] != 2;
    if (!pin) {
      pin = math::dot(face_normals[edge_faces[e][0]], face_normals[edge_faces[e][1]]) < cos_sharp;
    }
    if (pin) {
      pinned[edges[e][0]] = true;
      pinned[edges[e][1]] = true;
    }
  }

  Array<int> v2e_offsets;
  Array<int> v2e_indices;
  const GroupedSpan<int> vert_to_edge = mesh::build_vert_to_edge_map(
      edges, verts_num, v2e_offsets, v2e_indices);

  bke::BVHTreeFromMesh bvh = source.bvh_corner_tris();
  Array<float3> next(verts_num);
  for (int iteration = 0; iteration < iterations; iteration++) {
    threading::parallel_for(IndexRange(verts_num), 512, [&](const IndexRange range) {
      for (const int v : range) {
        if (pinned[v] || vert_to_edge[v].is_empty()) {
          next[v] = positions[v];
          continue;
        }
        float3 acc(0.0f);
        for (const int e : vert_to_edge[v]) {
          const int2 edge = edges[e];
          acc += positions[(edge[0] == v) ? edge[1] : edge[0]];
        }
        const float3 smoothed = math::interpolate(
            positions[v], acc / float(vert_to_edge[v].size()), factor);
        /* Keep the relaxed vertex on the original surface, otherwise repeated
         * smoothing shrinks the model. */
        BVHTreeNearest nearest;
        nearest.index = -1;
        nearest.dist_sq = FLT_MAX;
        BLI_bvhtree_find_nearest(bvh.tree, smoothed, &nearest, bvh.nearest_callback, &bvh);
        next[v] = (nearest.index != -1) ? float3(nearest.co) : smoothed;
      }
    });
    positions.copy_from(next);
  }
  mesh.tag_positions_changed();
}

}  // namespace bke

#ifdef WITH_QUADRIFLOW
static Mesh *remesh_quadriflow(const Mesh *input_mesh,
                               int target_faces,
                               int seed,
                               bool preserve_sharp,
                               bool preserve_boundary,
                               bool adaptive_scale,
                               void (*update_cb)(void *, float progress, int *cancel),
                               void *update_cb_data,
                               const float *guide_dirs,
                               const float *guide_weights,
                               const float *guide_pin_weights,
                               const float *guide_scales)
{
  using namespace blender::bke;
  const Span<float3> input_positions = input_mesh->vert_positions();
  const Span<int> input_corner_verts = input_mesh->corner_verts();
  const Span<int3> corner_tris = input_mesh->corner_tris();

  /* Gather the required data for export to the internal quadriflow mesh format. */
  Array<int3> vert_tris(corner_tris.size());
  mesh::vert_tris_from_corner_tris(input_corner_verts, corner_tris, vert_tris);

  /* Fill out the required input data */
  QuadriflowRemeshData qrd;

  qrd.totfaces = corner_tris.size();
  qrd.totverts = input_positions.size();
  qrd.verts = input_positions.cast<float>().data();
  qrd.faces = vert_tris.as_span().cast<int>().data();
  qrd.target_faces = target_faces;

  qrd.preserve_sharp = preserve_sharp;
  qrd.preserve_boundary = preserve_boundary;
  qrd.adaptive_scale = adaptive_scale;
  qrd.minimum_cost_flow = false;
  qrd.aggresive_sat = false;
  qrd.rng_seed = seed;

  /* Optional orientation guidance; null unless a caller supplies a guide field. */
  qrd.guide_dirs = guide_dirs;
  qrd.guide_weights = guide_weights;
  qrd.guide_pin_weights = guide_pin_weights;
  qrd.guide_scales = guide_scales;

  qrd.out_faces = nullptr;

  /* Run the remesher */
  QFLOW_quadriflow_remesh(&qrd, update_cb, update_cb_data);

  if (qrd.out_faces == nullptr) {
    /* The remeshing was canceled */
    return nullptr;
  }

  if (qrd.out_totfaces == 0) {
    /* Meshing failed */
    MEM_delete(qrd.out_faces);
    MEM_delete(qrd.out_verts);
    return nullptr;
  }

  /* Construct the new output mesh */
  Mesh *mesh = BKE_mesh_new_nomain(qrd.out_totverts, 0, qrd.out_totfaces, qrd.out_totfaces * 4);
  BKE_mesh_copy_parameters(mesh, input_mesh);
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();

  offset_indices::fill_constant_group_size(4, 0, face_offsets);

  mesh->vert_positions_for_write().copy_from(
      Span(reinterpret_cast<float3 *>(qrd.out_verts), qrd.out_totverts));

  for (const int i : IndexRange(qrd.out_totfaces)) {
    const int loopstart = i * 4;
    corner_verts[loopstart] = qrd.out_faces[loopstart];
    corner_verts[loopstart + 1] = qrd.out_faces[loopstart + 1];
    corner_verts[loopstart + 2] = qrd.out_faces[loopstart + 2];
    corner_verts[loopstart + 3] = qrd.out_faces[loopstart + 3];
  }

  mesh_calc_edges(*mesh, false, false);

  MEM_delete(qrd.out_faces);
  MEM_delete(qrd.out_verts);

  return mesh;
}
#endif

Mesh *BKE_mesh_remesh_quadriflow(const Mesh *mesh,
                                 int target_faces,
                                 int seed,
                                 bool preserve_sharp,
                                 bool preserve_boundary,
                                 bool adaptive_scale,
                                 void (*update_cb)(void *, float progress, int *cancel),
                                 void *update_cb_data,
                                 const float *guide_dirs,
                                 const float *guide_weights,
                                 const float *guide_pin_weights,
                                 const float *guide_scales)
{
#ifdef WITH_QUADRIFLOW
  if (target_faces <= 0) {
    target_faces = -1;
  }
  return remesh_quadriflow(mesh,
                           target_faces,
                           seed,
                           preserve_sharp,
                           preserve_boundary,
                           adaptive_scale,
                           update_cb,
                           update_cb_data,
                           guide_dirs,
                           guide_weights,
                           guide_pin_weights,
                           guide_scales);
#else
  UNUSED_VARS(mesh,
              target_faces,
              seed,
              preserve_sharp,
              preserve_boundary,
              adaptive_scale,
              update_cb,
              update_cb_data,
              guide_dirs,
              guide_weights,
              guide_pin_weights,
              guide_scales);
  return nullptr;
#endif
}

#ifdef WITH_OPENVDB
static openvdb::FloatGrid::Ptr remesh_voxel_level_set_create(
    const Mesh *mesh, openvdb::math::Transform::Ptr transform)
{
  const Span<float3> positions = mesh->vert_positions();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int3> corner_tris = mesh->corner_tris();

  std::vector<openvdb::Vec3s> points(mesh->verts_num);
  std::vector<openvdb::Vec3I> triangles(corner_tris.size());

  for (const int i : IndexRange(mesh->verts_num)) {
    const float3 &co = positions[i];
    points[i] = openvdb::Vec3s(co.x, co.y, co.z);
  }

  for (const int i : IndexRange(corner_tris.size())) {
    const int3 &tri = corner_tris[i];
    triangles[i] = openvdb::Vec3I(
        corner_verts[tri[0]], corner_verts[tri[1]], corner_verts[tri[2]]);
  }

  openvdb::FloatGrid::Ptr grid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
      *transform, points, triangles, 1.0f);

  return grid;
}

static Mesh *remesh_voxel_volume_to_mesh(const openvdb::FloatGrid::Ptr level_set_grid,
                                         const float isovalue,
                                         const float adaptivity,
                                         const bool relax_disoriented_triangles)
{
  using namespace blender::bke;
  std::vector<openvdb::Vec3s> vertices;
  std::vector<openvdb::Vec4I> quads;
  std::vector<openvdb::Vec3I> tris;
  openvdb::tools::volumeToMesh<openvdb::FloatGrid>(
      *level_set_grid, vertices, tris, quads, isovalue, adaptivity, relax_disoriented_triangles);

  if (vertices.empty() || quads.size() + tris.size() == 0) {
    return nullptr;
  }

  Mesh *mesh = BKE_mesh_new_nomain(
      vertices.size(), 0, quads.size() + tris.size(), quads.size() * 4 + tris.size() * 3);
  MutableSpan<float3> vert_positions = mesh->vert_positions_for_write();
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> mesh_corner_verts = mesh->corner_verts_for_write();

  const int triangle_loop_start = quads.size() * 4;
  if (!face_offsets.is_empty()) {
    offset_indices::fill_constant_group_size(4, 0, face_offsets.take_front(quads.size()));
    offset_indices::fill_constant_group_size(
        3, triangle_loop_start, face_offsets.drop_front(quads.size()));
  }

  for (const int i : vert_positions.index_range()) {
    vert_positions[i] = float3(vertices[i].x(), vertices[i].y(), vertices[i].z());
  }

  for (const int i : IndexRange(quads.size())) {
    const int loopstart = i * 4;
    mesh_corner_verts[loopstart] = quads[i][0];
    mesh_corner_verts[loopstart + 1] = quads[i][3];
    mesh_corner_verts[loopstart + 2] = quads[i][2];
    mesh_corner_verts[loopstart + 3] = quads[i][1];
  }

  for (const int i : IndexRange(tris.size())) {
    const int loopstart = triangle_loop_start + i * 3;
    mesh_corner_verts[loopstart] = tris[i][2];
    mesh_corner_verts[loopstart + 1] = tris[i][1];
    mesh_corner_verts[loopstart + 2] = tris[i][0];
  }

  mesh_calc_edges(*mesh, false, false);

  return mesh;
}
#endif

Mesh *BKE_mesh_remesh_voxel(const Mesh *mesh,
                            const float voxel_size,
                            const float adaptivity,
                            const float isovalue,
                            const Object *object,
                            ModifierData *modifier_data)
{
#ifdef WITH_OPENVDB
  openvdb::math::Transform::Ptr transform;
  try {
    transform = openvdb::math::Transform::createLinearTransform(voxel_size);
  }
  catch (const openvdb::ArithmeticError & /*e*/) {
    /* OpenVDB internally has a limit of 3e-15 for the matrix's determinant and throws
     * ArithmeticError if the provided value is too low.
     * See #136637 for more details. */
    BKE_modifier_set_error(
        object, modifier_data, "Voxel size of %f too small to be solved", voxel_size);
    return nullptr;
  }
  openvdb::FloatGrid::Ptr level_set = remesh_voxel_level_set_create(mesh, transform);
  Mesh *result = remesh_voxel_volume_to_mesh(level_set, isovalue, adaptivity, false);
  if (result != nullptr) {
    BKE_mesh_copy_parameters(result, mesh);
  }
  return result;
#else
  UNUSED_VARS(mesh, voxel_size, adaptivity, isovalue, object, modifier_data);
  return nullptr;
#endif
}

Mesh *BKE_mesh_remesh_voxel(const Mesh *mesh,
                            const float voxel_size,
                            const float adaptivity,
                            const float isovalue,
                            ReportList *reports)
{
#ifdef WITH_OPENVDB
  openvdb::math::Transform::Ptr transform;
  try {
    transform = openvdb::math::Transform::createLinearTransform(voxel_size);
  }
  catch (const openvdb::ArithmeticError & /*e*/) {
    /* OpenVDB internally has a limit of 3e-15 for the matrix's determinant and throws
     * ArithmeticError if the provided value is too low.
     * See #136637 for more details. */
    BKE_reportf(reports, RPT_ERROR, "Voxel size of %f too small to be solved", voxel_size);
    return nullptr;
  }
  openvdb::FloatGrid::Ptr level_set = remesh_voxel_level_set_create(mesh, transform);
  Mesh *result = remesh_voxel_volume_to_mesh(level_set, isovalue, adaptivity, false);
  if (result != nullptr) {
    BKE_mesh_copy_parameters(result, mesh);
  }
  return result;
#else
  UNUSED_VARS(mesh, voxel_size, adaptivity, isovalue, reports);
  return nullptr;
#endif
}

namespace bke {

static void calc_edge_centers(const Span<float3> positions,
                              const Span<int2> edges,
                              MutableSpan<float3> edge_centers)
{
  for (const int i : edges.index_range()) {
    edge_centers[i] = math::midpoint(positions[edges[i][0]], positions[edges[i][1]]);
  }
}

static void calc_face_centers(const Span<float3> positions,
                              const OffsetIndices<int> faces,
                              const Span<int> corner_verts,
                              MutableSpan<float3> face_centers)
{
  for (const int i : faces.index_range()) {
    face_centers[i] = mesh::face_center_calc(positions, corner_verts.slice(faces[i]));
  }
}

static void find_nearest_tris(const Span<float3> positions,
                              BVHTreeFromMesh &bvhtree,
                              MutableSpan<int> tris)
{
  for (const int i : positions.index_range()) {
    BVHTreeNearest nearest;
    nearest.index = -1;
    nearest.dist_sq = FLT_MAX;
    BLI_bvhtree_find_nearest(
        bvhtree.tree, positions[i], &nearest, bvhtree.nearest_callback, &bvhtree);
    tris[i] = nearest.index;
  }
}

static void find_nearest_tris_parallel(const Span<float3> positions,
                                       BVHTreeFromMesh &bvhtree,
                                       MutableSpan<int> tris)
{
  threading::parallel_for(tris.index_range(), 512, [&](const IndexRange range) {
    find_nearest_tris(positions.slice(range), bvhtree, tris.slice(range));
  });
}

static void find_nearest_faces(const Span<int> src_tri_faces,
                               const Span<float3> dst_positions,
                               const OffsetIndices<int> dst_faces,
                               const Span<int> dst_corner_verts,
                               BVHTreeFromMesh &bvhtree,
                               MutableSpan<int> nearest_faces)
{
  struct TLS {
    Vector<float3> face_centers;
    Vector<int> tri_indices;
  };
  threading::EnumerableThreadSpecific<TLS> all_tls;
  threading::parallel_for(dst_faces.index_range(), 512, [&](const IndexRange range) {
    threading::isolate_task([&] {
      TLS &tls = all_tls.local();
      Vector<float3> &face_centers = tls.face_centers;
      face_centers.reinitialize(range.size());
      calc_face_centers(dst_positions, dst_faces.slice(range), dst_corner_verts, face_centers);

      Vector<int> &tri_indices = tls.tri_indices;
      tri_indices.reinitialize(range.size());
      find_nearest_tris(face_centers, bvhtree, tri_indices);

      array_utils::gather(src_tri_faces, tri_indices.as_span(), nearest_faces.slice(range));
    });
  });
}

static void find_nearest_edges(const Span<float3> src_positions,
                               const Span<int2> src_edges,
                               const OffsetIndices<int> src_faces,
                               const Span<int> src_corner_edges,
                               const Span<int> src_tri_faces,
                               const Span<float3> dst_positions,
                               const Span<int2> dst_edges,
                               BVHTreeFromMesh &bvhtree,
                               MutableSpan<int> nearest_edges)
{
  struct TLS {
    Vector<float3> edge_centers;
    Vector<int> tri_indices;
    Vector<int> face_indices;
    Vector<float> distances;
  };
  threading::EnumerableThreadSpecific<TLS> all_tls;
  threading::parallel_for(nearest_edges.index_range(), 512, [&](const IndexRange range) {
    threading::isolate_task([&] {
      TLS &tls = all_tls.local();
      Vector<float3> &edge_centers = tls.edge_centers;
      edge_centers.reinitialize(range.size());
      calc_edge_centers(dst_positions, dst_edges.slice(range), edge_centers);

      Vector<int> &tri_indices = tls.tri_indices;
      tri_indices.reinitialize(range.size());
      find_nearest_tris_parallel(edge_centers, bvhtree, tri_indices);

      Vector<int> &face_indices = tls.face_indices;
      face_indices.reinitialize(range.size());
      array_utils::gather(src_tri_faces, tri_indices.as_span(), face_indices.as_mutable_span());

      /* Find the source edge that's closest to the destination edge in the nearest face. Search
       * through the whole face instead of just the triangle because the triangle has edges that
       * might not be actual mesh edges. */
      Vector<float, 64> distances;
      for (const int i : range.index_range()) {
        const int dst_edge = range[i];
        const float3 &dst_position = edge_centers[i];

        const int src_face = face_indices[i];
        const Span<int> src_face_edges = src_corner_edges.slice(src_faces[src_face]);

        distances.reinitialize(src_face_edges.size());
        for (const int i : src_face_edges.index_range()) {
          const int2 src_edge = src_edges[src_face_edges[i]];
          const float3 src_center = math::midpoint(src_positions[src_edge[0]],
                                                   src_positions[src_edge[1]]);
          distances[i] = math::distance_squared(src_center, dst_position);
        }

        const int min = std::min_element(distances.begin(), distances.end()) - distances.begin();
        nearest_edges[dst_edge] = src_face_edges[min];
      }
    });
  });
}

static void gather_attributes(const Span<StringRef> names,
                              const AttributeAccessor src_attributes,
                              const AttrDomain domain,
                              const Span<int> index_map,
                              MutableAttributeAccessor dst_attributes)
{
  for (const StringRef name : names) {
    const GVArraySpan src = *src_attributes.lookup(name, domain);
    const AttrType type = cpp_type_to_attribute_type(src.type());
    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        name, domain, type);
    attribute_math::gather(src, index_map, dst.span);
    dst.finish();
  }
}

static void sample_vertex_attributes(const Span<StringRef> names,
                                     Span<int> corner_verts,
                                     Span<int3> corner_tris,
                                     Span<int> tri_indices,
                                     Span<float3> bary_coords,
                                     const AttributeAccessor src_attributes,
                                     MutableAttributeAccessor dst_attributes)
{
  for (const StringRef name : names) {
    const GVArray src = *src_attributes.lookup(name, AttrDomain::Point);
    const AttrType type = cpp_type_to_attribute_type(src.type());
    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        name, AttrDomain::Point, type);
    mesh_surface_sample::sample_point_attribute(corner_verts,
                                                corner_tris,
                                                tri_indices,
                                                bary_coords,
                                                src,
                                                IndexMask(dst.span.size()),
                                                dst.span);
    dst.finish();
  }
}

static void sample_corner_attributes(const Span<StringRef> names,
                                     Span<int3> corner_tris,
                                     Span<int> tri_indices,
                                     Span<float3> bary_coords,
                                     const AttributeAccessor src_attributes,
                                     MutableAttributeAccessor dst_attributes)
{
  for (const StringRef name : names) {
    const GVArray src = *src_attributes.lookup(name, AttrDomain::Corner);
    const AttrType type = cpp_type_to_attribute_type(src.type());

    GArray<> dst_point(src.type(), bary_coords.size());
    mesh_surface_sample::sample_corner_attribute(
        corner_tris, tri_indices, bary_coords, src, IndexMask(dst_point.size()), dst_point);

    GVArray dst_corner = dst_attributes.adapt_domain(
        GVArray::from_span(dst_point.as_span()), AttrDomain::Point, AttrDomain::Corner);
    dst_attributes.add(name, AttrDomain::Corner, type, AttributeInitVArray(std::move(dst_corner)));
  }
}

void mesh_remesh_reproject_attributes(const Mesh &src, Mesh &dst)
{
  MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  /* Gather attributes to transfer for each domain. This makes it possible to skip
   * building index maps and even the main BVH tree if there are no attributes. */
  const AttributeAccessor src_attributes = src.attributes();
  Vector<StringRef> point_ids;
  Vector<StringRef> edge_ids;
  Vector<StringRef> face_ids;
  Vector<StringRef> corner_ids;
  src_attributes.foreach_attribute([&](const AttributeIter &iter) {
    if (ELEM(iter.name, "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
      return;
    }
    if (iter.storage_type == bke::AttrStorageType::Single) {
      const GVArray src_attr = *iter.get();
      const CommonVArrayInfo info = src_attr.common_info();
      if (info.type == CommonVArrayInfo::Type::Single) {
        const bke::AttributeInitValue init(GPointer(src_attr.type(), info.data));
        if (dst_attributes.add(iter.name, iter.domain, iter.data_type, init)) {
          return;
        }
      }
    }
    switch (iter.domain) {
      case AttrDomain::Point:
        point_ids.append(iter.name);
        break;
      case AttrDomain::Edge:
        edge_ids.append(iter.name);
        break;
      case AttrDomain::Face:
        face_ids.append(iter.name);
        break;
      case AttrDomain::Corner:
        corner_ids.append(iter.name);
        break;
      default:
        BLI_assert_unreachable();
        break;
    }
  });

  if (point_ids.is_empty() && edge_ids.is_empty() && face_ids.is_empty() && corner_ids.is_empty())
  {
    return;
  }

  const Span<float3> src_positions = src.vert_positions();
  const OffsetIndices src_faces = src.faces();
  const Span<int> src_corner_verts = src.corner_verts();
  const Span<int3> src_corner_tris = src.corner_tris();

  /* The main idea in the following code is to trade some complexity in sampling for the benefit of
   * only using and building a single BVH tree. Since sculpt mode doesn't generally deal with loose
   * vertices and edges, we use the standard "triangles" BVH which won't contain them. Also, only
   * relying on a single BVH should reduce memory usage, and work better if the BVH and #pbvh::Tree
   * are ever merged.
   *
   * One key decision is separating building transfer index maps from actually transferring any
   * attribute data. This is important to keep attribute storage independent from the specifics of
   * the decisions made here, which mainly results in easier refactoring, more generic code, and
   * possibly improved performance from lower cache usage in the "complex" sampling part of the
   * algorithm and the copying itself. */
  BVHTreeFromMesh bvhtree = src.bvh_corner_tris();

  const Span<float3> dst_positions = dst.vert_positions();
  const OffsetIndices dst_faces = dst.faces();
  const Span<int> dst_corner_verts = dst.corner_verts();

  if (!point_ids.is_empty() || !corner_ids.is_empty()) {
    Array<int> vert_nearest_tris(dst_positions.size());
    Array<float3> bary_coords(dst_positions.size());
    find_nearest_tris_parallel(dst_positions, bvhtree, vert_nearest_tris);
    mesh_surface_sample::sample_barycentric_weights(src_positions,
                                                    src_corner_verts,
                                                    src_corner_tris,
                                                    vert_nearest_tris,
                                                    dst_positions,
                                                    IndexMask(dst_positions.size()),
                                                    bary_coords);

    if (!point_ids.is_empty()) {
      /* Copy vertex group names (otherwise `MeshVertexGroupsAttributeProvider` wont find them -
       * and these would show up as regular attributes afterwards). "vertex_group_active_index" is
       * taken care of via #BKE_mesh_copy_parameters(). */
      BKE_defgroup_copy_list(&dst.vertex_group_names, &src.vertex_group_names);
      sample_vertex_attributes(point_ids,
                               src_corner_verts,
                               src_corner_tris,
                               vert_nearest_tris,
                               bary_coords,
                               src_attributes,
                               dst_attributes);
    }

    if (!corner_ids.is_empty()) {
      sample_corner_attributes(corner_ids,
                               src_corner_tris,
                               vert_nearest_tris,
                               bary_coords,
                               src_attributes,
                               dst_attributes);
    }
  }

  if (!edge_ids.is_empty()) {
    const Span<int2> src_edges = src.edges();
    const Span<int> src_corner_edges = src.corner_edges();
    const Span<int> src_tri_faces = src.corner_tri_faces();
    const Span<int2> dst_edges = dst.edges();
    Array<int> map(dst.edges_num);
    find_nearest_edges(src_positions,
                       src_edges,
                       src_faces,
                       src_corner_edges,
                       src_tri_faces,
                       dst_positions,
                       dst_edges,
                       bvhtree,
                       map);
    gather_attributes(edge_ids, src_attributes, AttrDomain::Edge, map, dst_attributes);
  }

  if (!face_ids.is_empty()) {
    const Span<int> src_tri_faces = src.corner_tri_faces();
    Array<int> map(dst.faces_num);
    find_nearest_faces(src_tri_faces, dst_positions, dst_faces, dst_corner_verts, bvhtree, map);
    gather_attributes(face_ids, src_attributes, AttrDomain::Face, map, dst_attributes);
  }

  if (src.active_color_attribute) {
    BKE_id_attributes_active_color_set(&dst.id, src.active_color_attribute);
  }
  if (src.default_color_attribute) {
    BKE_id_attributes_default_color_set(&dst.id, src.default_color_attribute);
  }
  if (!src.active_uv_map_name().is_empty()) {
    dst.uv_maps_active_set(src.active_uv_map_name());
  }
  if (!src.default_uv_map_name().is_empty()) {
    dst.uv_maps_default_set(src.default_uv_map_name());
  }
}

}  // namespace bke

Mesh *BKE_mesh_remesh_voxel_fix_poles(const Mesh *mesh)
{
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);

  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &bmesh_create_params);

  BMeshFromMeshParams bmesh_from_mesh_params{};
  bmesh_from_mesh_params.calc_face_normal = true;
  bmesh_from_mesh_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(bm, mesh, &bmesh_from_mesh_params);

  BMVert *v;
  BMEdge *ed, *ed_next;
  BMFace *f, *f_next;
  BMIter iter_a, iter_b;

  /* Merge 3 edge poles vertices that exist in the same face */
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BM_ITER_MESH_MUTABLE (f, f_next, &iter_a, bm, BM_FACES_OF_MESH) {
    BMVert *v1, *v2;
    v1 = nullptr;
    v2 = nullptr;
    BM_ITER_ELEM (v, &iter_b, f, BM_VERTS_OF_FACE) {
      if (BM_vert_edge_count(v) == 3) {
        if (v1) {
          v2 = v;
        }
        else {
          v1 = v;
        }
      }
    }
    if (v1 && v2 && (v1 != v2) && !BM_edge_exists(v1, v2)) {
      BM_face_kill(bm, f);
      BMEdge *e = BM_edge_create(bm, v1, v2, nullptr, BM_CREATE_NOP);
      BM_elem_flag_set(e, BM_ELEM_TAG, true);
    }
  }

  BM_ITER_MESH_MUTABLE (ed, ed_next, &iter_a, bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(ed, BM_ELEM_TAG)) {
      float co[3];
      mid_v3_v3v3(co, ed->v1->co, ed->v2->co);
      BMVert *vc = BM_edge_collapse(bm, ed, ed->v1, true, true);
      copy_v3_v3(vc->co, co);
    }
  }

  /* Delete faces with a 3 edge pole in all their vertices */
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BM_ITER_MESH (f, &iter_a, bm, BM_FACES_OF_MESH) {
    bool dissolve = true;
    BM_ITER_ELEM (v, &iter_b, f, BM_VERTS_OF_FACE) {
      if (BM_vert_edge_count(v) != 3) {
        dissolve = false;
      }
    }
    if (dissolve) {
      BM_ITER_ELEM (v, &iter_b, f, BM_VERTS_OF_FACE) {
        BM_elem_flag_set(v, BM_ELEM_TAG, true);
      }
    }
  }
  BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_VERTS);

  BM_ITER_MESH (ed, &iter_a, bm, BM_EDGES_OF_MESH) {
    if (BM_edge_face_count(ed) != 2) {
      BM_elem_flag_set(ed, BM_ELEM_TAG, true);
    }
  }
  BM_mesh_edgenet(bm, false, true);

  /* Smooth the result */
  for (int i = 0; i < 4; i++) {
    BM_ITER_MESH (v, &iter_a, bm, BM_VERTS_OF_MESH) {
      float co[3];
      zero_v3(co);
      BM_ITER_ELEM (ed, &iter_b, v, BM_EDGES_OF_VERT) {
        BMVert *vert = BM_edge_other_vert(ed, v);
        add_v3_v3(co, vert->co);
      }
      mul_v3_fl(co, 1.0f / float(BM_vert_edge_count(v)));
      mid_v3_v3v3(v->co, v->co, co);
    }
  }

  BM_mesh_normals_update(bm);

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  BM_mesh_elem_hflag_enable_all(bm, BM_FACE, BM_ELEM_TAG, false);
  BMO_op_callf(bm,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "recalc_face_normals faces=%hf",
               BM_ELEM_TAG);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BMeshToMeshParams bmesh_to_mesh_params{};
  bmesh_to_mesh_params.calc_object_remap = false;
  Mesh *result = BKE_mesh_from_bmesh_nomain(bm, &bmesh_to_mesh_params, mesh);

  BM_mesh_free(bm);
  return result;
}

}  // namespace blender
