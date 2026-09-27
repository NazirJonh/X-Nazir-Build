/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the UV projection of a live 3D shape; see #paint_shape_uv_trace.hh.
 */

#include "paint_shape_uv_trace.hh"

#include <algorithm>
#include <cmath>
#include <cfloat>

#include "BLI_index_mask.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"
#include "BLI_virtual_array.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_paint_bvh.hh"

#include "paint_shape_space.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Pure helpers
 * \{ */

/** The seam threshold of a run: never below #SHAPE_UV_SEAM_JUMP_MIN, else a multiple of the
 * polyline's lower-quartile sample step -- a seam crosses the outline once or twice, so the
 * quartile stays a legit step even when the jumps are counted in. */
static float uv_seam_threshold(const Span<float2> uvs, const bool cyclic)
{
  const int n = uvs.size();
  if (n < 2) {
    return SHAPE_UV_SEAM_JUMP_MIN;
  }
  Vector<float> steps;
  const int step_num = cyclic ? n : n - 1;
  steps.reserve(step_num);
  for (const int i : IndexRange(step_num)) {
    steps.append(math::distance(uvs[i], uvs[(i + 1) % n]));
  }
  const int quartile_i = steps.size() / 4;
  std::nth_element(steps.begin(), steps.begin() + quartile_i, steps.end());
  return std::max(SHAPE_UV_SEAM_JUMP_MIN, SHAPE_UV_SEAM_JUMP_FACTOR * steps[quartile_i]);
}

Vector<ShapeUVTraceRun> shape_uv_trace_split_runs(const Span<float2> uvs, const bool cyclic)
{
  Vector<ShapeUVTraceRun> runs;
  const int n = uvs.size();
  if (n == 0) {
    return runs;
  }
  if (n == 1) {
    ShapeUVTraceRun run;
    run.indices.append(0);
    runs.append(std::move(run));
    return runs;
  }

  const float threshold = uv_seam_threshold(uvs, cyclic);
  const auto is_jump = [&](const int i) {
    return math::distance(uvs[i], uvs[(i + 1) % n]) > threshold;
  };

  /* A cyclic polyline with a continuous wrap is one loop. Otherwise rotate it open at the largest
   * jump, so a seam never cuts a run's middle, and split at the remaining jumps. */
  if (cyclic && !is_jump(n - 1)) {
    ShapeUVTraceRun run;
    for (const int i : IndexRange(n)) {
      run.indices.append(i);
    }
    run.cyclic = true;
    runs.append(std::move(run));
    return runs;
  }
  int first = 0;
  if (cyclic) {
    int max_jump = 0;
    float max_jump_len = -1.0f;
    for (const int i : IndexRange(n)) {
      if (const float len = math::distance(uvs[i], uvs[(i + 1) % n]); len > max_jump_len) {
        max_jump_len = len;
        max_jump = i;
      }
    }
    first = (max_jump + 1) % n;
  }

  Vector<int> current;
  for (const int off : IndexRange(n)) {
    const int i = (first + off) % n;
    current.append(i);
    const bool last = (off == n - 1);
    if (!last && is_jump(i)) {
      ShapeUVTraceRun run;
      run.indices = std::move(current);
      runs.append(std::move(run));
      current = Vector<int>();
    }
  }
  if (!current.is_empty()) {
    ShapeUVTraceRun run;
    run.indices = std::move(current);
    runs.append(std::move(run));
  }
  return runs;
}

float2x2 shape_uv_trace_jacobian(const float2 &p0,
                                 const float2 &p1,
                                 const float2 &p2,
                                 const float2 &uv0,
                                 const float2 &uv1,
                                 const float2 &uv2,
                                 bool &r_ok)
{
  r_ok = false;
  /* Column-major: each matrix holds its two edge vectors as columns, so the Jacobian maps a
   * plane-px delta to the UV delta (`uv = uv0 + J * (p - p0)` inside the triangle). */
  float2x2 edges;
  edges[0] = p1 - p0;
  edges[1] = p2 - p0;
  float2x2 uv_edges;
  uv_edges[0] = uv1 - uv0;
  uv_edges[1] = uv2 - uv0;
  bool success = false;
  const float2x2 inverse = math::invert(edges, success);
  if (!success) {
    return float2x2::identity();
  }
  r_ok = true;
  return uv_edges * inverse;
}

bool shape_uv_trace_cage_map(const ShapeUVTrace &trace, ShapeUVCageMap &r_map)
{
  if (!trace.valid || trace.cage_part < 0 || trace.cage_part >= trace.parts.size()) {
    return false;
  }
  const ShapeUVTracePart &part = trace.parts[trace.cage_part];
  if (part.ref_sample < 0 || !part.jacobian_valid || part.uvs.is_empty()) {
    return false;
  }
  r_map = ShapeUVCageMap{};
  /* The anchor's own mapping when its triangle was sampled on the cage part: exact at the plane
   * origin, where the shape's origin and the cage interior sit. */
  const bool anchor_on_part = trace.anchor_jacobian_valid &&
                              part.tris.contains(trace.anchor_tri);
  if (anchor_on_part) {
    r_map.ref_px = float2(0.0f);
    r_map.ref_uv = trace.anchor_uv;
    r_map.jacobian = trace.anchor_jacobian;
    r_map.at_anchor = true;
    return true;
  }
  r_map.ref_px = part.ref_px;
  r_map.ref_uv = part.uvs[part.ref_sample];
  r_map.jacobian = part.jacobian;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Trace build
 * \{ */

/** The contour samples are the same flattened polylines the overlays draw. */
static constexpr float SHAPE_UV_TRACE_FLATTEN_ERROR_PX = 2.0f;

ShapeUVTraceCache::~ShapeUVTraceCache() = default;

/** The cheap deformation fingerprint: summed positions of the trace triangles' vertices plus the
 * anchor position. Sculpt strokes and Mesh Filters move the evaluated positions in place, where a
 * Span identity check sees nothing; a changed fingerprint means the BVH bounds and the trace are
 * stale. */
static float3 trace_positions_fingerprint(const Mesh &mesh,
                                          const Span<float3> positions,
                                          const SurfaceAnchor &anchor,
                                          const ShapeUVTrace &trace)
{
  float3 sum = anchor.co;
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  for (const ShapeUVTracePart &part : trace.parts) {
    for (const int tri_i : part.tris) {
      if (tri_i < 0 || tri_i >= corner_tris.size()) {
        continue;
      }
      const int3 tri = corner_tris[tri_i];
      sum += positions[corner_verts[tri[0]]];
      sum += positions[corner_verts[tri[1]]];
      sum += positions[corner_verts[tri[2]]];
    }
  }
  return sum;
}

bool ShapeUVTraceCache::ensure_bvh(const Depsgraph &depsgraph, const Object &ob)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  if (bvh_valid_ && bvh_positions_.data() == positions.data() &&
      bvh_positions_.size() == positions.size())
  {
    return true;
  }
  const IndexMask all_faces(IndexRange(mesh.faces_num));
  bvh_ = bke::bvhtree_from_mesh_corner_tris_ex(
      positions, mesh.faces(), mesh.corner_verts(), mesh.corner_tris(), all_faces);
  bvh_valid_ = bvh_.tree != nullptr;
  bvh_positions_ = positions;
  return bvh_valid_;
}

/** The exact plane-px -> UV Jacobian of \a tri (corner UVs from \a uv_map), for the cage map. */
static float2x2 tri_jacobian(const Span<float3> positions,
                             const Span<int> corner_verts,
                             const int3 &tri,
                             const VArraySpan<float2> &uv_map,
                             const SurfaceAnchorFrame &frame,
                             const float px_per_unit,
                             const float3 &anchor_co,
                             bool &r_ok)
{
  const auto plane_px_of = [&](const float3 &co) {
    const float3 d = co - anchor_co;
    return float2(math::dot(d, frame.tangent), math::dot(d, frame.bitangent)) * px_per_unit;
  };
  return shape_uv_trace_jacobian(plane_px_of(positions[corner_verts[tri[0]]]),
                                 plane_px_of(positions[corner_verts[tri[1]]]),
                                 plane_px_of(positions[corner_verts[tri[2]]]),
                                 uv_map[tri[0]],
                                 uv_map[tri[1]],
                                 uv_map[tri[2]],
                                 r_ok);
}

bool ShapeUVTraceCache::rebuild(const Depsgraph &depsgraph,
                                const Object &ob,
                                const ShapeSpaceDesc &space_desc,
                                const Span<VectorItem> items)
{
  trace_ = ShapeUVTrace{};
  trace_.valid = false;

  if (ob.type != OB_MESH || ob.data == nullptr) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  /* A fresh restore of the anchor drives the whole projection, so the trace follows the current
   * (possibly sculpted) surface exactly like the commit's re-anchor does. */
  SurfaceAnchor anchor = space_desc.anchor;
  if (!surface_anchor_restore(ob, depsgraph, anchor)) {
    return false;
  }
  const StringRefNull uv_name = (anchor.surface_uv_map[0] != '\0') ?
                                    StringRefNull(anchor.surface_uv_map) :
                                    mesh.active_or_default_uv_map_name();
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return false;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  if (!this->ensure_bvh(depsgraph, ob)) {
    return false;
  }

  const SurfaceAnchorFrame frame = surface_anchor_frame(anchor.normal, anchor.tangent);
  const float px_per_unit = std::max(anchor.px_per_unit, 1e-6f);
  const auto plane_px_of = [&](const float3 &co) {
    const float3 d = co - anchor.co;
    return float2(math::dot(d, frame.tangent), math::dot(d, frame.bitangent)) * px_per_unit;
  };

  trace_.anchor_co = anchor.co;
  trace_.anchor_normal = anchor.normal;
  trace_.anchor_tri = anchor.tri;
  trace_.uv_map_name = uv_name;
  if (anchor.tri >= 0 && anchor.tri < corner_tris.size()) {
    const int3 tri = corner_tris[anchor.tri];
    trace_.anchor_uv = bke::mesh_surface_sample::sample_corner_attribute_with_bary_coords(
        anchor.bary, tri, uv_map);
    trace_.anchor_jacobian = tri_jacobian(
        positions, corner_verts, tri, uv_map, frame, px_per_unit, anchor.co,
        trace_.anchor_jacobian_valid);
  }

  /* Sample every shape's flattened outline onto the surface. A sample is invalid when the plane
   * point has no usable surface under it (no hit, beyond the anchor's depth, back-facing); the
   * runs break there, exactly like they break at a seam. */
  for (const VectorItem &item : items) {
    const float2 origin_px = shape_effective_origin(item.shape);
    for (const ShapePolyline &poly : shape_flatten(item.shape, SHAPE_UV_TRACE_FLATTEN_ERROR_PX)) {
      const int sample_num = poly.points.size();
      if (sample_num < 2) {
        continue;
      }
      Vector<float2> uvs_all(sample_num);
      Vector<int> tris_all(sample_num);
      Vector<float2> px_all(sample_num);
      Vector<bool> valid_all(sample_num);
      bool all_valid = true;
      for (const int i : IndexRange(sample_num)) {
        const float2 &p = poly.points[i];
        px_all[i] = p;
        const float3 co_object = anchor.co + frame.tangent * (p.x / px_per_unit) +
                                 frame.bitangent * (p.y / px_per_unit);
        const float co_object_v[3] = {co_object.x, co_object.y, co_object.z};
        /* Project the plane point onto the surface with a ray along the anchor normal, both
         * directions; of the two hits, take the nearest one that passes the acceptance rules
         * (the same surface point a texel at this plane position bakes through). A hit rejected
         * on one side must not hide an accepted hit on the other. */
        BVHTreeRayHit hit{};
        hit.index = -1;
        hit.dist = FLT_MAX;
        for (const float dir_sign : {1.0f, -1.0f}) {
          const float3 dir = frame.normal * dir_sign;
          const float dir_v[3] = {dir.x, dir.y, dir.z};
          BVHTreeRayHit casted{};
          casted.index = -1;
          casted.dist = FLT_MAX;
          BLI_bvhtree_ray_cast(
              bvh_.tree, co_object_v, dir_v, 0.0f, &casted, bvh_.raycast_callback, &bvh_);
          if (casted.index < 0) {
            continue;
          }
          /* Exactly the write backend's acceptance rules (see #surface_anchor_hit_accepts): the
           * contour in UV must match what is actually painted. */
          const int3 tri = corner_tris[casted.index];
          const float3 v0 = positions[corner_verts[tri[0]]];
          const float3 v1 = positions[corner_verts[tri[1]]];
          const float3 v2 = positions[corner_verts[tri[2]]];
          const float3 tri_normal = math::normalize(math::cross(v1 - v0, v2 - v0));
          if (!surface_anchor_hit_accepts(
                  anchor.co, anchor.normal, float3(casted.co), tri_normal, anchor.max_depth))
          {
            continue;
          }
          if (casted.dist < hit.dist) {
            hit = casted;
          }
        }
        const bool valid = hit.index >= 0;
        valid_all[i] = valid;
        all_valid = all_valid && valid;
        if (valid) {
          const int3 tri = corner_tris[hit.index];
          const float3 bary = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
              positions, corner_verts, tri, float3(hit.co));
          uvs_all[i] = bke::mesh_surface_sample::sample_corner_attribute_with_bary_coords(
              bary, tri, uv_map);
          tris_all[i] = hit.index;
        }
      }

      /* Split into UV-continuous runs: seam-split when everything sampled, otherwise cut at the
       * invalid samples first. */
      Vector<ShapeUVTraceRun> runs;
      if (all_valid) {
        runs = shape_uv_trace_split_runs(uvs_all, poly.cyclic);
      }
      else {
        int start = -1;
        for (const int i : IndexRange(sample_num + 1)) {
          const bool valid = i < sample_num && valid_all[i];
          if (valid && start < 0) {
            start = i;
          }
          if (!valid && start >= 0) {
            const Span<float2> slice = uvs_all.as_span().slice(start, i - start);
            for (ShapeUVTraceRun &run : shape_uv_trace_split_runs(slice, false)) {
              ShapeUVTraceRun offset = std::move(run);
              for (int &index : offset.indices) {
                index += start;
              }
              runs.append(std::move(offset));
            }
            start = -1;
          }
        }
      }

      for (ShapeUVTraceRun &run : runs) {
        if (run.indices.size() < 2) {
          continue;
        }
        ShapeUVTracePart part;
        part.cyclic = run.cyclic;
        /* The reference sample: the run's sample closest to the shape's origin, with the exact
         * Jacobian of its triangle (the cage maps linearly through it). */
        float best_dist = FLT_MAX;
        for (const int i : run.indices) {
          part.uvs.append(uvs_all[i]);
          part.tris.append(tris_all[i]);
          if (const float dist = math::distance(px_all[i], origin_px); dist < best_dist) {
            best_dist = dist;
            part.ref_sample = part.uvs.size() - 1;
            part.ref_px = px_all[i];
          }
        }
        part.jacobian = tri_jacobian(positions,
                                     corner_verts,
                                     corner_tris[part.tris[part.ref_sample]],
                                     uv_map,
                                     frame,
                                     px_per_unit,
                                     anchor.co,
                                     part.jacobian_valid);
        trace_.parts.append(std::move(part));
      }
    }
  }

  trace_.valid = !trace_.parts.is_empty();

  /* Cage part: the largest one (enclosed area for a loop, length otherwise). */
  trace_.cage_part = -1;
  float best_measure = -1.0f;
  for (const int part_i : trace_.parts.index_range()) {
    const ShapeUVTracePart &part = trace_.parts[part_i];
    if (part.ref_sample < 0 || !part.jacobian_valid) {
      continue;
    }
    float measure = 0.0f;
    const int n = part.uvs.size();
    const int seg_num = part.cyclic ? n : n - 1;
    for (const int i : IndexRange(seg_num)) {
      const float2 &a = part.uvs[i];
      const float2 &b = part.uvs[(i + 1) % n];
      measure += part.cyclic ? (a.x * b.y - b.x * a.y) : math::distance(a, b);
    }
    if (part.cyclic) {
      measure = std::abs(measure) * 0.5f;
    }
    if (measure > best_measure) {
      best_measure = measure;
      trace_.cage_part = part_i;
    }
  }

  shape_dirty_ = false;
  last_check_ = BLI_time_now_seconds();
  positions_fingerprint_ = trace_positions_fingerprint(mesh, positions, anchor, trace_);
  return trace_.valid;
}

const ShapeUVTrace *ShapeUVTraceCache::update(const Depsgraph *depsgraph,
                                              const Object &ob,
                                              const ShapeSpaceDesc &space_desc,
                                              const Span<VectorItem> items,
                                              const double now)
{
  if (depsgraph == nullptr) {
    return trace_.valid ? &trace_ : nullptr;
  }
  if (items.is_empty()) {
    trace_ = ShapeUVTrace{};
    shape_dirty_ = true;
    return nullptr;
  }

  /* Shape edits rebuild immediately; otherwise only a moved surface triggers a rebuild, checked
   * with a cheap anchor restore (O(1) on the saved triangle) and the positions fingerprint of the
   * sampled triangles, at a fixed pace. */
  constexpr double DEFORM_CHECK_INTERVAL = 0.2;
  bool surface_moved = false;
  if (!shape_dirty_ && now - last_check_ >= DEFORM_CHECK_INTERVAL) {
    last_check_ = now;
    SurfaceAnchor anchor = space_desc.anchor;
    if (surface_anchor_restore(ob, *depsgraph, anchor)) {
      surface_moved = math::distance(anchor.co, trace_.anchor_co) > 1e-5f ||
                      math::distance(anchor.normal, trace_.anchor_normal) > 1e-5f;
      if (!surface_moved && trace_.valid && ob.type == OB_MESH && ob.data != nullptr) {
        const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
        surface_moved = trace_positions_fingerprint(
                            mesh, bke::pbvh::vert_positions_eval(*depsgraph, ob), anchor, trace_) !=
                        positions_fingerprint_;
      }
    }
    else if (trace_.valid) {
      /* The surface became unrestorable: nothing valid to show anymore. */
      trace_ = ShapeUVTrace{};
      return nullptr;
    }
  }
  if (surface_moved) {
    /* The positions were edited in place: the BVH bounds are stale even though the positions Span
     * kept its identity. */
    bvh_valid_ = false;
  }
  if (!shape_dirty_ && !surface_moved) {
    return trace_.valid ? &trace_ : nullptr;
  }
  this->rebuild(*depsgraph, ob, space_desc, items);
  return trace_.valid ? &trace_ : nullptr;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
