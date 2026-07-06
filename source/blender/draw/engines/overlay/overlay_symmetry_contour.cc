/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "overlay_symmetry_contour.hh"

#include "BKE_ccg.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph_query.hh"

#include "BLI_bit_vector.hh"
#include "BLI_bounds.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "bmesh.hh"

#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <utility>

namespace blender::draw::overlay {

/* -------------------------------------------------------------------- */
/** \name Geometry helpers (object space)
 * \{ */

namespace {

/** Build the 6 view-frustum planes for coarse PBVH-node culling. Returns the plane count. */
int build_frustum_planes(const RegionView3D *rv3d, float r_planes[6][4])
{
  if (rv3d == nullptr) {
    return 0;
  }
  planes_from_projmat(
      rv3d->persmat, r_planes[0], r_planes[1], r_planes[2], r_planes[3], r_planes[4], r_planes[5]);
  return 6;
}

/** Project a world-space point to screen pixels. Returns false if behind the camera. */
bool project_point_to_screen(const float3 &world_pos,
                             const float4x4 &persmat,
                             const int2 region_size,
                             float2 &r_screen)
{
  const float4 clip = persmat * float4(world_pos, 1.0f);
  if (math::abs(clip.w) < 1e-8f || clip.w < 0.0f) {
    return false;
  }
  const float3 ndc = float3(clip) / clip.w;
  if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z)) {
    return false;
  }
  r_screen.x = (ndc.x * 0.5f + 0.5f) * region_size.x;
  r_screen.y = (ndc.y * 0.5f + 0.5f) * region_size.y;
  return true;
}

/**
 * Drop points from an object-space contour that are closer than `pixel_step` on screen.
 * `persmat` already folds in the object-to-world matrix.
 */
void decimate_contour_screen(ContourLoop &contour,
                             const float4x4 &persmat,
                             const int2 region_size,
                             const float pixel_step)
{
  if (contour.points.size() < 2 || pixel_step <= 0.0f) {
    return;
  }

  Vector<float3> decimated;
  decimated.reserve(contour.points.size());

  float2 prev_screen;
  bool prev_valid = project_point_to_screen(contour.points[0], persmat, region_size, prev_screen);
  decimated.append(contour.points[0]);
  float accum = 0.0f;

  for (int i = 1; i < contour.points.size(); i++) {
    float2 curr_screen;
    const bool valid = project_point_to_screen(
        contour.points[i], persmat, region_size, curr_screen);
    if (!prev_valid && !valid) {
      continue;
    }

    float seg_len = 0.0f;
    if (prev_valid && valid) {
      seg_len = math::length(curr_screen - prev_screen);
    }
    accum += seg_len;

    if (accum >= pixel_step || (!valid && prev_valid)) {
      decimated.append(contour.points[i]);
      accum = 0.0f;
    }

    prev_screen = curr_screen;
    prev_valid = valid;
  }

  if (!decimated.is_empty() && decimated.last() != contour.points.last()) {
    decimated.append(contour.points.last());
  }

  if (decimated.size() >= 2) {
    contour.points = std::move(decimated);
  }
}

/** Intersect a triangle with the plane, producing at most one segment. */
bool intersect_triangle_plane(const float3 &v0,
                              const float3 &v1,
                              const float3 &v2,
                              const PlaneParams &plane,
                              float3 &r_start,
                              float3 &r_end)
{
  const float3 vertices[3] = {v0, v1, v2};
  const float distances[3] = {math::dot(v0 - plane.point, plane.normal),
                              math::dot(v1 - plane.point, plane.normal),
                              math::dot(v2 - plane.point, plane.normal)};

  constexpr float epsilon = 1e-6f;
  const int positive_count = int(distances[0] > epsilon) + int(distances[1] > epsilon) +
                             int(distances[2] > epsilon);
  /* All vertices on the same side: no crossing. */
  if (positive_count == 0 || positive_count == 3) {
    return false;
  }

  float3 points[2];
  int count = 0;
  for (int i = 0; i < 3 && count < 2; i++) {
    const int next = (i + 1) % 3;
    if ((distances[i] > epsilon && distances[next] < -epsilon) ||
        (distances[i] < -epsilon && distances[next] > epsilon))
    {
      const float t = distances[i] / (distances[i] - distances[next]);
      points[count++] = math::interpolate(vertices[i], vertices[next], t);
    }
    else if (math::abs(distances[i]) <= epsilon) {
      points[count++] = vertices[i];
    }
  }

  if (count == 2) {
    r_start = points[0];
    r_end = points[1];
    return true;
  }
  return false;
}

PlaneParams build_plane_params(const int axis, const float diag)
{
  PlaneParams plane;
  plane.axis = axis;
  plane.normal = float3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
  plane.point = float3(0.0f);

  plane.tangent = (axis == 0) ? float3(0.0f, 1.0f, 0.0f) :
                  (axis == 1) ? float3(0.0f, 0.0f, 1.0f) :
                                float3(1.0f, 0.0f, 0.0f);
  plane.bitangent = math::cross(plane.normal, plane.tangent);
  if (math::length_squared(plane.bitangent) < 1e-8f) {
    plane.bitangent = float3(0.0f, 0.0f, 1.0f);
  }
  plane.tangent = math::normalize(plane.tangent);
  plane.bitangent = math::normalize(plane.bitangent);

  /* Tolerances scaled by object size so the result is scale independent. */
  plane.quant_step = 1e-4f * diag;
  plane.min_seg_len = 1e-4f * diag;
  plane.min_loop_len = 1e-3f * diag;
  plane.plane_tolerance = 1e-5f * diag;
  plane.smooth_max_disp = 0.25f * plane.quant_step;
  return plane;
}

bool aabb_intersects_plane(const blender::Bounds<float3> &bounds, const PlaneParams &plane)
{
  const float3 center = (bounds.min + bounds.max) * 0.5f;
  const float3 half = (bounds.max - bounds.min) * 0.5f;
  const float dist = math::dot(plane.normal, center - plane.point);
  const float radius = math::abs(plane.normal.x) * half.x + math::abs(plane.normal.y) * half.y +
                       math::abs(plane.normal.z) * half.z;
  return math::abs(dist) <= radius + plane.plane_tolerance;
}

QuantizedPointKey quantize_point(const float3 &p, const PlaneParams &plane)
{
  const float2 uv(math::dot(p, plane.tangent), math::dot(p, plane.bitangent));
  const float inv_step = 1.0f / plane.quant_step;
  return {int(math::round(uv.x * inv_step)), int(math::round(uv.y * inv_step)), plane.axis};
}

uint64_t segment_hash(const ContourSegment &seg)
{
  uint64_t h0 = QuantizedPointKeyHash{}(seg.key_a);
  uint64_t h1 = QuantizedPointKeyHash{}(seg.key_b);
  if (h1 < h0) {
    std::swap(h0, h1);
  }
  return h0 ^ (h1 + 0x9e3779b97f4a7c15ULL + (h0 << 6) + (h0 >> 2));
}

/** Append a deduplicated, length-filtered segment to `segments`. */
void append_segment(Vector<ContourSegment> &segments,
                    Set<uint64_t> &hashes,
                    const float3 &p0,
                    const float3 &p1,
                    const PlaneParams &plane)
{
  const float len = math::distance(p0, p1);
  if (len < plane.min_seg_len) {
    return;
  }

  ContourSegment seg;
  seg.a = p0;
  seg.b = p1;
  seg.key_a = quantize_point(p0, plane);
  seg.key_b = quantize_point(p1, plane);
  seg.length = len;

  if (!hashes.add(segment_hash(seg))) {
    return; /* Duplicate. */
  }
  segments.append(seg);
}

void process_pbvh_mesh(const Span<float3> positions,
                       const Mesh &mesh,
                       const bke::pbvh::MeshNode &node,
                       const PlaneParams &plane,
                       Vector<ContourSegment> &segments,
                       Set<uint64_t> &hashes)
{
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  for (const int face_i : node.faces()) {
    const IndexRange face = faces[face_i];
    if (face.size() < 3) {
      continue;
    }
    const int v0 = corner_verts[face[0]];
    for (int i = 1; i < face.size() - 1; i++) {
      const int v1 = corner_verts[face[i]];
      const int v2 = corner_verts[face[i + 1]];
      float3 s, e;
      if (intersect_triangle_plane(positions[v0], positions[v1], positions[v2], plane, s, e)) {
        append_segment(segments, hashes, s, e, plane);
      }
    }
  }
}

void process_pbvh_bmesh(const bke::pbvh::BMeshNode &node,
                        const PlaneParams &plane,
                        Vector<ContourSegment> &segments,
                        Set<uint64_t> &hashes)
{
  /* Prefer the cached triangulation when present. */
  if (!node.orig_tris_.is_empty() && !node.orig_positions_.is_empty()) {
    for (const int3 &tri : node.orig_tris_) {
      float3 s, e;
      if (intersect_triangle_plane(node.orig_positions_[tri.x],
                                   node.orig_positions_[tri.y],
                                   node.orig_positions_[tri.z],
                                   plane,
                                   s,
                                   e))
      {
        append_segment(segments, hashes, s, e, plane);
      }
    }
    return;
  }

  for (BMFace *f : node.bm_faces_) {
    if (f->len < 3) {
      continue;
    }
    const float3 v0 = float3(f->l_first->v->co);
    BMLoop *l = f->l_first->next;
    for (int i = 1; i < f->len - 1; i++, l = l->next) {
      float3 s, e;
      if (intersect_triangle_plane(v0, float3(l->v->co), float3(l->next->v->co), plane, s, e)) {
        append_segment(segments, hashes, s, e, plane);
      }
    }
  }
}

void process_pbvh_grids(const SubdivCCG &subdiv_ccg,
                        const CCGKey &key,
                        const bke::pbvh::GridsNode &node,
                        const PlaneParams &plane,
                        Vector<ContourSegment> &segments,
                        Set<uint64_t> &hashes)
{
  const Span<float3> positions = subdiv_ccg.positions;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  const int grid_size = key.grid_size;

  for (const int grid : node.grids()) {
    const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
    for (const short y : IndexRange(grid_size - 1)) {
      for (const short x : IndexRange(grid_size - 1)) {
        if (!grid_hidden.is_empty() &&
            paint_is_grid_face_hidden(grid_hidden[grid], grid_size, x, y))
        {
          continue;
        }
        const float3 &v00 = grid_positions[CCG_grid_xy_to_index(grid_size, x, y)];
        const float3 &v10 = grid_positions[CCG_grid_xy_to_index(grid_size, x + 1, y)];
        const float3 &v11 = grid_positions[CCG_grid_xy_to_index(grid_size, x + 1, y + 1)];
        const float3 &v01 = grid_positions[CCG_grid_xy_to_index(grid_size, x, y + 1)];

        float3 s, e;
        if (intersect_triangle_plane(v00, v10, v11, plane, s, e)) {
          append_segment(segments, hashes, s, e, plane);
        }
        if (intersect_triangle_plane(v00, v11, v01, plane, s, e)) {
          append_segment(segments, hashes, s, e, plane);
        }
      }
    }
  }
}

/**
 * Full-editmesh CPU extraction. Edit Mode has no paint BVH and the `Mesh` used for drawing
 * carries no populated position/face arrays either (the live geometry lives in the `BMesh`), so
 * it needs its own traversal instead of #add_full_mesh_segments.
 */
void add_full_editmesh_segments(BMesh &bm,
                                const PlaneParams &plane,
                                Vector<ContourSegment> &segments,
                                Set<uint64_t> &hashes)
{
  BMFace *f;
  BMIter iter;
  BM_ITER_MESH (f, &iter, &bm, BM_FACES_OF_MESH) {
    if (f->len < 3) {
      continue;
    }
    const float3 v0 = float3(f->l_first->v->co);
    BMLoop *l = f->l_first->next;
    for (int i = 1; i < f->len - 1; i++, l = l->next) {
      float3 s, e;
      if (intersect_triangle_plane(v0, float3(l->v->co), float3(l->next->v->co), plane, s, e)) {
        append_segment(segments, hashes, s, e, plane);
      }
    }
  }
}

/** Full-mesh CPU extraction, used in edit/paint modes where no paint BVH exists. */
void add_full_mesh_segments(const Mesh &mesh,
                            const Span<float3> positions,
                            const PlaneParams &plane,
                            Vector<ContourSegment> &segments,
                            Set<uint64_t> &hashes)
{
  if (mesh.faces_num == 0 || positions.is_empty()) {
    return;
  }
  const OffsetIndices faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  for (const int face_i : faces.index_range()) {
    const IndexRange face = faces[face_i];
    if (face.size() < 3) {
      continue;
    }
    const int v0 = corner_verts[face[0]];
    for (int i = 1; i < face.size() - 1; i++) {
      const int v1 = corner_verts[face[i]];
      const int v2 = corner_verts[face[i + 1]];
      float3 s, e;
      if (intersect_triangle_plane(positions[v0], positions[v1], positions[v2], plane, s, e)) {
        append_segment(segments, hashes, s, e, plane);
      }
    }
  }
}

/** Limited Laplacian smoothing that keeps points within a small displacement of the original. */
void smooth_loop(ContourLoop &loop, const PlaneParams &plane)
{
  if (loop.points.size() < 3) {
    return;
  }
  for (int iter = 0; iter < plane.smooth_iters; iter++) {
    Vector<float3> new_points = loop.points;
    for (int i = 1; i < loop.points.size() - 1; i++) {
      const float3 target = (loop.points[i - 1] + loop.points[i + 1]) * 0.5f;
      float3 delta = (target - loop.points[i]) * plane.smooth_factor;
      const float delta_len = math::length(delta);
      if (delta_len > plane.smooth_max_disp && delta_len > 0.0f) {
        delta *= plane.smooth_max_disp / delta_len;
      }
      new_points[i] = loop.points[i] + delta;
    }
    loop.points = std::move(new_points);
  }
}

/** Chain segments into ordered contour loops by welding quantized endpoints. */
void build_loops_from_segments(const Span<ContourSegment> segments,
                               const PlaneParams &plane,
                               Vector<ContourLoop> &r_loops)
{
  if (segments.is_empty()) {
    return;
  }

  Map<uint64_t, Vector<int>> adjacency;
  adjacency.reserve(segments.size() * 2);
  for (const int i : segments.index_range()) {
    adjacency.lookup_or_add_default(QuantizedPointKeyHash{}(segments[i].key_a)).append(i);
    adjacency.lookup_or_add_default(QuantizedPointKeyHash{}(segments[i].key_b)).append(i);
  }

  Vector<bool> used(segments.size(), false);

  for (const int start_idx : segments.index_range()) {
    if (used[start_idx]) {
      continue;
    }
    const ContourSegment &seg = segments[start_idx];
    used[start_idx] = true;

    ContourLoop loop;
    loop.points.append(seg.a);
    loop.points.append(seg.b);

    const uint64_t start_hash = QuantizedPointKeyHash{}(seg.key_a);
    QuantizedPointKey current_key = seg.key_b;
    uint64_t current_hash = QuantizedPointKeyHash{}(current_key);

    while (true) {
      Vector<int> *next_list = adjacency.lookup_ptr(current_hash);
      if (next_list == nullptr) {
        break;
      }
      /* At a plain degree-2 vertex there is only one unused candidate to continue with. At a
       * junction - more than 2 segments quantized to the same point, from a genuine
       * self-intersection of the contour curve or two crossings merged by #PlaneParams.quant_step
       * - more than one unused candidate can remain. #segments arrives in a different order every
       * call (the per-node results are folded from thread-local buffers in whatever order the
       * task scheduler happens to visit them), so picking the first unused candidate in that
       * order made the resulting loop count depend on that arbitrary order instead of the
       * geometry. Continue with whichever candidate keeps the traversal straightest instead: that
       * matches the visually correct pairing at a crossing and does not depend on input order. */
      int next_idx = -1;
      const float3 &curr_point = loop.points.last();
      const float3 incoming_dir = math::normalize(curr_point - loop.points[loop.points.size() - 2]);
      float best_dot = -2.0f;
      for (const int cand : *next_list) {
        if (used[cand]) {
          continue;
        }
        const ContourSegment &cand_seg = segments[cand];
        const float3 &cand_point = (cand_seg.key_a == current_key) ? cand_seg.b : cand_seg.a;
        const float3 outgoing_dir = math::normalize(cand_point - curr_point);
        const float dot = math::dot(incoming_dir, outgoing_dir);
        if (dot > best_dot) {
          best_dot = dot;
          next_idx = cand;
        }
      }
      if (next_idx == -1) {
        break;
      }
      used[next_idx] = true;
      const ContourSegment &next_seg = segments[next_idx];
      if (next_seg.key_a == current_key) {
        loop.points.append(next_seg.b);
        current_key = next_seg.key_b;
      }
      else {
        loop.points.append(next_seg.a);
        current_key = next_seg.key_a;
      }
      current_hash = QuantizedPointKeyHash{}(current_key);
    }

    loop.length = 0.0f;
    for (int i = 1; i < loop.points.size(); i++) {
      loop.length += math::distance(loop.points[i - 1], loop.points[i]);
    }
    if (current_hash == start_hash && loop.points.size() > 2) {
      loop.is_closed = true;
    }
    if (loop.length < plane.min_loop_len) {
      continue;
    }

    smooth_loop(loop, plane);
    r_loops.append(std::move(loop));
  }
}

/**
 * Resolve object-space vertex positions for extraction: live sculpt deformation when available,
 * otherwise the evaluated mesh (which already reflects the edit cage in edit mode). Returns an
 * empty span for the Edit Mode BMesh path, where the live geometry lives in `edit_bm` instead.
 */
Span<float3> resolve_positions(const Object &ob,
                               const Mesh &mesh,
                               const bke::pbvh::Tree *pbvh,
                               const SculptSession *ss,
                               const State &state,
                               const BMesh *edit_bm)
{
  const bool sculpt_mesh = pbvh != nullptr && pbvh->type() == bke::pbvh::Type::Mesh && ss;
  if (sculpt_mesh && !DEG_is_original(&ob)) {
    return bke::pbvh::vert_positions_eval_from_eval(ob);
  }
  if (sculpt_mesh && state.depsgraph) {
    return bke::pbvh::vert_positions_eval(*state.depsgraph, ob);
  }
  if (edit_bm == nullptr) {
    return mesh.vert_positions();
  }
  return {};
}

/**
 * Object-space bounds used to scale the extraction tolerances. Prefers the PBVH bounds when
 * available to avoid scanning every vertex on heavy meshes.
 */
Bounds<float3> compute_object_bounds(const bke::pbvh::Tree *pbvh,
                                     BMesh *edit_bm,
                                     const Span<float3> positions)
{
  float3 bb_min(FLT_MAX);
  float3 bb_max(-FLT_MAX);
  if (pbvh != nullptr) {
    const Bounds<float3> bounds = bke::pbvh::bounds_get(*pbvh);
    bb_min = bounds.min;
    bb_max = bounds.max;
  }
  else if (edit_bm != nullptr) {
    BMVert *v;
    BMIter viter;
    BM_ITER_MESH (v, &viter, edit_bm, BM_VERTS_OF_MESH) {
      const float3 p(v->co);
      bb_min = math::min(bb_min, p);
      bb_max = math::max(bb_max, p);
    }
  }
  else {
    for (const float3 &p : positions) {
      bb_min = math::min(bb_min, p);
      bb_max = math::max(bb_max, p);
    }
  }
  return {bb_min, bb_max};
}

}  // namespace

/** \} */

/* -------------------------------------------------------------------- */
/** \name SymmetryContour
 * \{ */

void SymmetryContour::begin_sync(Resources &res, const State &state)
{
  contour_shader_ = res.shaders->extra_wire_contour.get();
  contour_lines_.clear();

  line_thickness_ = math::max(state.overlay.sculpt_symmetry_contour_thickness, 1.0f);

  const float4 theme_col = res.theme.colors.sculpt_symmetry_contour;
  line_color_ = float3(theme_col);
  line_alpha_ = theme_col.w;
}

void SymmetryContour::emit_loop(const ContourLoop &loop, const float4x4 &object_to_world)
{
  if (loop.points.size() < 2) {
    return;
  }
  const float4 color(line_color_, line_alpha_);
  for (int i = 0; i < loop.points.size() - 1; i++) {
    contour_lines_.append(math::transform_point(object_to_world, loop.points[i]),
                          math::transform_point(object_to_world, loop.points[i + 1]),
                          color);
  }
  if (loop.is_closed && loop.points.size() >= 3) {
    contour_lines_.append(math::transform_point(object_to_world, loop.points.last()),
                          math::transform_point(object_to_world, loop.points.first()),
                          color);
  }
}

SymmetryContour::RegenDecision SymmetryContour::compute_regen_decision(
    const Object *ob,
    const int symmetry_flags,
    const bke::pbvh::Tree *pbvh,
    const bool has_dirty_nodes) const
{
  /* Edit-mode meshes have no paint BVH and thus no dirty-node signal, so geometry edits would
   * otherwise be missed by the cache. Regenerate every sync there. Texture paint also lacks a BVH
   * but cannot change geometry, so it keeps using the cache. */
  const bool edit_mode_live = (pbvh == nullptr) && (ob->mode & OB_MODE_EDIT);

  /* A Multires display/sculpt level change (or any other full PBVH rebuild) swaps in a tree with
   * a different leaf layout while keeping the same #Object, so it changes the leaf-node count
   * without tripping `has_dirty_nodes`. Left undetected, #cached_segments_by_axis_ would keep
   * emitting segments cached under node indices that now refer to different geometry. */
  const int pbvh_nodes_num = pbvh != nullptr ? pbvh->nodes_num() : 0;
  const bool topology_changed = pbvh != nullptr && pbvh_nodes_num != prev_pbvh_nodes_num_;

  /* Fallback safety net: #positions_changed_count and #external_positions_dirty_ are always bumped
   * together by #tag_positions_changed, so in practice this agrees with #has_dirty_nodes. It only
   * diverges for a call path that changes positions without going through #tag_positions_changed
   * with a precise mask, which #positions_changed_without_detail below still needs to catch. */
  const int64_t positions_count = pbvh != nullptr ? pbvh->positions_changed_count() : 0;
  const bool positions_changed = pbvh != nullptr && positions_count != prev_positions_count_;

  const bool object_changed = ob != prev_object_ || symmetry_flags != prev_symmetry_flags_;
  /* #positions_changed without a per-node mask to go with it (#has_dirty_nodes) carries no detail
   * about which nodes moved, so every intersecting node has to be treated as dirty. This should be
   * rare now that #has_dirty_nodes is sourced from #Tree::consume_external_positions_dirty (which
   * stays accurate through a stroke instead of being cleared by the brush's own bookkeeping) - it
   * only fires for genuine no-detail events, e.g. undo/redo, mesh filters, or transforms. */
  const bool positions_changed_without_detail = positions_changed && !has_dirty_nodes;

  RegenDecision decision;
  decision.object_changed = object_changed;
  decision.need_regenerate = contours_dirty_ || enabled_ != prev_enabled_ || object_changed ||
                             has_dirty_nodes || edit_mode_live || topology_changed ||
                             positions_changed_without_detail;
  /* The per-node cache is only valid while editing the same object and PBVH topology. It must also
   * be dropped for #positions_changed_without_detail: the affected nodes are unknown, so every
   * intersecting node has to be recomputed rather than re-emitted from stale cache entries. That
   * flag should now only fire for rare one-off events (undo/redo, filters, enabling the overlay,
   * mesh transform), since #has_dirty_nodes stays precise through an active stroke - so paying for
   * a full rebuild here is acceptable. When a precise dirty mask *is* available (the normal case
   * while actively sculpting), it already pinpoints exactly which nodes to recompute, so the rest
   * of the cache stays valid instead of being thrown away and rebuilt from scratch. */
  decision.reset_cache = object_changed || topology_changed || positions_changed_without_detail;
  decision.pbvh_nodes_num = pbvh_nodes_num;
  decision.positions_count = positions_count;
  return decision;
}

void SymmetryContour::update_contours(const Object *ob,
                                      const int symmetry_flags,
                                      const State &state,
                                      BMesh *edit_bm)
{
  if (!enabled_ || ob == nullptr || ob->data == nullptr || ob->type != OB_MESH ||
      symmetry_flags == 0)
  {
    return;
  }

  const float4x4 object_to_world = ob->object_to_world();

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*const_cast<Object *>(ob));
  const SculptSession *ss = ob->runtime->sculpt_session;
  /* The BMesh path is only relevant when there is no paint BVH to draw from. */
  if (pbvh != nullptr) {
    edit_bm = nullptr;
  }

  IndexMaskMemory dirty_memory;
  IndexMask dirty_nodes;
  bool has_dirty_nodes = false;
  if (pbvh != nullptr) {
    /* Unlike #bke::pbvh::pbvh_positions_dirty_mask (which reads the tree's own transient
     * #bounds_dirty_ - already cleared by the brush's #flush_bounds_to_parents call by the time
     * this overlay runs, on essentially every stroke step), this stays precise even mid-stroke. */
    dirty_nodes = pbvh->consume_external_positions_dirty(dirty_memory);
    has_dirty_nodes = !dirty_nodes.is_empty();
  }

  const RegenDecision decision = compute_regen_decision(ob, symmetry_flags, pbvh, has_dirty_nodes);

  /* Fast path: re-emit the cached contour (transformed by the current matrix) unchanged. */
  if (!decision.need_regenerate) {
    for (const ContourLoop &loop : cached_contours_) {
      emit_loop(loop, object_to_world);
    }
    return;
  }

  cached_contours_.clear();

  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob);

  const Span<float3> positions = resolve_positions(*ob, mesh, pbvh, ss, state, edit_bm);
  if (positions.is_empty() && edit_bm == nullptr) {
    return;
  }

  /* Object size, used to scale tolerances. */
  const Bounds<float3> bounds = compute_object_bounds(pbvh, edit_bm, positions);
  const float diag = math::max(math::length(bounds.max - bounds.min), 1e-3f);

  /* Per-fragment occlusion tolerance in world units (the contour is emitted in world space and
   * the shader compares view-space depths). Scaled by the object's world-space size so it stays
   * proportional to the geometry: it absorbs the small gap where a straight contour segment cuts
   * a curved face slightly below the displayed surface, without letting genuinely back-facing
   * parts leak through. */
  const float3 world_diag = math::transform_direction(object_to_world, bounds.max - bounds.min);
  depth_bias_ = 0.0025f * math::max(math::length(world_diag), 1e-4f);

  /* View frustum for PBVH-node culling. */
  float frustum_planes[6][4];
  const int frustum_plane_len = build_frustum_planes(state.rv3d, frustum_planes);
  const bool use_frustum_cull = frustum_plane_len > 0;

  /* Cap actual recompute work (not merely visiting cached/culled nodes) by wall-clock time rather
   * than a fixed node count: a node-count cap sized for very heavy meshes leaves light ones
   * needlessly spreading their one-off full-mesh fill across dozens of stuttering frames (every
   * one of which still pays the fixed cost of walking every leaf node), while a cap sized for
   * light meshes would blow the frame budget on heavier ones. A time budget scales itself to
   * whatever the mesh actually costs to process. */
  constexpr double recompute_time_budget_ms = 4.0;

  /* Drop the whole per-node cache when it can no longer be trusted (see #compute_regen_decision
   * for when #reset_cache fires). Otherwise a precise dirty mask pinpoints exactly which nodes to
   * recompute, so the rest of the cache stays valid instead of being rebuilt from scratch. */
  if (decision.reset_cache) {
    for (int axis = 0; axis < 3; axis++) {
      cached_segments_by_axis_[axis].clear();
    }
  }

  BitVector<> dirty_lookup;
  if (has_dirty_nodes) {
    dirty_lookup.resize(pbvh->nodes_num(), false);
    dirty_nodes.to_bits(dirty_lookup);
  }

  bool pending_dirty = false;

  for (int axis = 0; axis < 3; axis++) {
    const int axis_flag = (axis == 0) ? PAINT_SYMM_X : (axis == 1) ? PAINT_SYMM_Y : PAINT_SYMM_Z;
    if ((symmetry_flags & axis_flag) == 0) {
      continue;
    }

    const PlaneParams plane = build_plane_params(axis, diag);

    Vector<ContourSegment> segments;
    Set<uint64_t> segment_hashes;

    if (pbvh != nullptr) {
      Map<int, Vector<ContourSegment>> &axis_cache = cached_segments_by_axis_[axis];

      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
      Vector<int> node_indices(node_mask.size());
      node_mask.to_indices(node_indices.as_mutable_span());

      /* Snapshot of the cached segments, indexed by node id, for lock-free lookup during the
       * parallel loop below: #axis_cache itself is only mutated afterwards - in the single-
       * threaded join below - so it is never touched concurrently with these reads. */
      Vector<const Vector<ContourSegment> *> cache_snapshot(pbvh->nodes_num(), nullptr);
      if (!axis_cache.is_empty()) {
        axis_cache.foreach_item([&](const int key, const Vector<ContourSegment> &value) {
          if (key >= 0 && key < cache_snapshot.size()) {
            cache_snapshot[key] = &value;
          }
        });
      }

      const auto recompute_deadline =
          std::chrono::steady_clock::now() +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double, std::milli>(recompute_time_budget_ms));
      std::atomic<bool> recompute_budget_exhausted(false);
      std::atomic<bool> pending_dirty_nodes(false);

      /* Per-thread accumulators so the (common) cache-read path and the (rare, budget-limited)
       * recompute path never contend on a shared lock; everything is folded back into
       * #segments / #axis_cache in a single-threaded pass once the parallel loop has joined. */
      threading::EnumerableThreadSpecific<Vector<ContourSegment>> local_segments_ts;
      threading::EnumerableThreadSpecific<Vector<std::pair<int, Vector<ContourSegment>>>>
          local_new_cache_ts;

      auto node_needs_recompute = [&](const int n) {
        if (decision.object_changed) {
          return true;
        }
        if (has_dirty_nodes && dirty_lookup[n]) {
          return true;
        }
        return cache_snapshot[n] == nullptr;
      };

      /* Shared per-node traversal for every PBVH type. Each leaf node is culled against the plane
       * and the view frustum, then either recomputed (subject to the per-frame time budget) or
       * re-used from the cache. Only the leaf-node type, the grain size and the extraction callback
       * differ between Mesh, BMesh and Grids; `extra_recompute` forces recompute regardless of the
       * dirty state (used by Grids when the CCG coordinates are dirty). */
      auto process_nodes = [&](auto nodes,
                               const int64_t grain_size,
                               const bool extra_recompute,
                               auto &&extract) {
        threading::parallel_for(
            node_indices.index_range(), grain_size, [&](const IndexRange range) {
              Vector<ContourSegment> &local_segments = local_segments_ts.local();
              for (const int n : node_indices.as_span().slice(range)) {
                const auto &node = nodes[n];
                if (!aabb_intersects_plane(node.bounds(), plane)) {
                  continue;
                }
                if (use_frustum_cull) {
                  const blender::Bounds<float3> world_bounds =
                      blender::bounds::transform_bounds<float, 4>(object_to_world, node.bounds());
                  if (isect_aabb_planes_v3(
                          frustum_planes, frustum_plane_len, world_bounds.min, world_bounds.max) ==
                      ISECT_AABB_PLANE_BEHIND_ANY)
                  {
                    continue;
                  }
                }
                if (node_needs_recompute(n) || extra_recompute) {
                  /* Never throttle a full rebuild (#reset_cache): every intersecting node is
                   * already missing from the cache in that case, so spreading the one-off cost over
                   * many frames only multiplies the fixed per-frame leaf-walk overhead instead of
                   * paying it once. Throttling only matters for the incremental case, where a live
                   * stroke could otherwise dirty an unexpectedly large number of nodes in a single
                   * step. */
                  if (!decision.reset_cache &&
                      (recompute_budget_exhausted.load(std::memory_order_relaxed) ||
                       std::chrono::steady_clock::now() >= recompute_deadline))
                  {
                    recompute_budget_exhausted.store(true, std::memory_order_relaxed);
                    pending_dirty_nodes.store(true, std::memory_order_relaxed);
                    continue;
                  }
                  Vector<ContourSegment> node_segments;
                  Set<uint64_t> local_hashes;
                  extract(node, node_segments, local_hashes);
                  local_segments.extend(node_segments);
                  local_new_cache_ts.local().append_as(n, std::move(node_segments));
                }
                else if (const Vector<ContourSegment> *cached = cache_snapshot[n]) {
                  local_segments.extend(*cached);
                }
              }
            });
      };

      switch (pbvh->type()) {
        case bke::pbvh::Type::Mesh: {
          process_nodes(pbvh->nodes<bke::pbvh::MeshNode>(),
                        256,
                        false,
                        [&](const bke::pbvh::MeshNode &node,
                            Vector<ContourSegment> &node_segments,
                            Set<uint64_t> &local_hashes) {
                          process_pbvh_mesh(
                              positions, mesh, node, plane, node_segments, local_hashes);
                        });
          break;
        }
        case bke::pbvh::Type::BMesh: {
          process_nodes(pbvh->nodes<bke::pbvh::BMeshNode>(),
                        256,
                        false,
                        [&](const bke::pbvh::BMeshNode &node,
                            Vector<ContourSegment> &node_segments,
                            Set<uint64_t> &local_hashes) {
                          process_pbvh_bmesh(node, plane, node_segments, local_hashes);
                        });
          break;
        }
        case bke::pbvh::Type::Grids: {
          if (!ss || !ss->subdiv_ccg) {
            break;
          }
          const SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
          const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
          process_nodes(pbvh->nodes<bke::pbvh::GridsNode>(),
                        1,
                        subdiv_ccg.dirty.coords,
                        [&](const bke::pbvh::GridsNode &node,
                            Vector<ContourSegment> &node_segments,
                            Set<uint64_t> &local_hashes) {
                          process_pbvh_grids(
                              subdiv_ccg, key, node, plane, node_segments, local_hashes);
                        });
          break;
        }
      }

      /* Join: fold the per-thread results into the shared containers. #axis_cache is mutated
       * here only, single-threaded, after every reader of #cache_snapshot above has finished, so
       * this is race-free despite #cache_snapshot pointing into #axis_cache's storage. */
      for (Vector<std::pair<int, Vector<ContourSegment>>> &local : local_new_cache_ts) {
        for (std::pair<int, Vector<ContourSegment>> &item : local) {
          axis_cache.add_overwrite(item.first, std::move(item.second));
        }
      }
      for (Vector<ContourSegment> &local : local_segments_ts) {
        for (const ContourSegment &seg : local) {
          if (segment_hashes.add(segment_hash(seg))) {
            segments.append(seg);
          }
        }
      }

      if (pending_dirty_nodes.load(std::memory_order_relaxed)) {
        pending_dirty = true;
      }
    }

    /* No paint BVH (edit / texture paint): extract from the full mesh. */
    if (pbvh == nullptr) {
      if (edit_bm != nullptr) {
        add_full_editmesh_segments(*edit_bm, plane, segments, segment_hashes);
      }
      else {
        add_full_mesh_segments(mesh, positions, plane, segments, segment_hashes);
      }
    }

    Vector<ContourLoop> axis_loops;
    build_loops_from_segments(segments, plane, axis_loops);

    /* Fall back to raw segments if chaining produced nothing. */
    if (axis_loops.is_empty() && !segments.is_empty()) {
      axis_loops.reserve(segments.size());
      for (const ContourSegment &seg : segments) {
        ContourLoop loop;
        loop.length = seg.length;
        loop.points.append(seg.a);
        loop.points.append(seg.b);
        axis_loops.append(std::move(loop));
      }
    }

    /* Screen-space decimation (operates in object space; persmat folds in the model matrix). */
    if (state.rv3d && state.region) {
      const float4x4 persmat = float4x4(state.rv3d->persmat) * object_to_world;
      const int2 region_size(state.region->winx, state.region->winy);
      for (ContourLoop &loop : axis_loops) {
        decimate_contour_screen(loop, persmat, region_size, 1.5f);
      }
    }

    for (const ContourLoop &loop : axis_loops) {
      emit_loop(loop, object_to_world);
    }

    cached_contours_.extend(std::move(axis_loops));
  }

  /* Recompute next frame if the per-frame budget left dirty nodes unprocessed. */
  contours_dirty_ = pending_dirty;
  prev_enabled_ = enabled_;
  prev_object_ = ob;
  prev_symmetry_flags_ = symmetry_flags;
  prev_pbvh_nodes_num_ = decision.pbvh_nodes_num;
  prev_positions_count_ = decision.positions_count;
}

void SymmetryContour::end_sync(PassSimple::Sub &pass)
{
  if (!enabled_ || contour_shader_ == nullptr) {
    contour_lines_.clear();
    return;
  }
  pass.shader_set(contour_shader_);
  pass.push_constant("contour_width", line_thickness_);
  pass.push_constant("depth_bias", depth_bias_);
  contour_lines_.end_sync(pass);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Symmetry flag conversion
 * \{ */

int symmetry_flags_from_mesh_symmetry(const char mesh_symmetry)
{
  int flags = 0;
  if (mesh_symmetry & ME_SYMMETRY_X) {
    flags |= PAINT_SYMM_X;
  }
  if (mesh_symmetry & ME_SYMMETRY_Y) {
    flags |= PAINT_SYMM_Y;
  }
  if (mesh_symmetry & ME_SYMMETRY_Z) {
    flags |= PAINT_SYMM_Z;
  }
  return flags;
}

int symmetry_flags_from_curves_symmetry(const char curves_symmetry)
{
  int flags = 0;
  if (curves_symmetry & CURVES_SYMMETRY_X) {
    flags |= PAINT_SYMM_X;
  }
  if (curves_symmetry & CURVES_SYMMETRY_Y) {
    flags |= PAINT_SYMM_Y;
  }
  if (curves_symmetry & CURVES_SYMMETRY_Z) {
    flags |= PAINT_SYMM_Z;
  }
  return flags;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SymmetryContourOverlay
 * \{ */

SymmetryContourOverlay::SymmetryContourOverlay(SelectionType selection_type, const char *pass_name)
    : pass_(pass_name), contour_(selection_type)
{
}

void SymmetryContourOverlay::begin_sync(Resources &res, const State &state, const bool show)
{
  show_ = show;
  res_ = &res;
  contour_.set_enabled(show);

  pass_.init();
  sub_ = nullptr;
  if (!show_) {
    return;
  }

  contour_.begin_sync(res, state);

  pass_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
  pass_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
  PassSimple::Sub &sub = pass_.sub("Contours");
  /* No depth test: the contour is drawn on top of the mesh and its occlusion by the surface is
   * resolved per-fragment against the sampled scene depth instead (see #overlay_extra_wire_frag).
   * This is why the draw targets the depth-less line frame-buffer in #draw_line. */
  sub.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA, state.clipping_plane_count);
  sub.shader_set(res.shaders->extra_wire_contour.get());
  sub.bind_texture("scene_depth_tx", &res.depth_tx);
  sub_ = &sub;
}

void SymmetryContourOverlay::object_sync(const Object *ob,
                                         const int symmetry_flags,
                                         const State &state,
                                         BMesh *edit_bm)
{
  if (!show_ || symmetry_flags == 0) {
    return;
  }
  contour_.update_contours(ob, symmetry_flags, state, edit_bm);
}

void SymmetryContourOverlay::end_sync()
{
  if (show_ && sub_ != nullptr) {
    contour_.end_sync(*sub_);
  }
}

void SymmetryContourOverlay::draw_line(Framebuffer & /*framebuffer*/,
                                      Manager &manager,
                                      View &view)
{
  if (!show_ || res_ == nullptr) {
    return;
  }
  /* Target the depth-less line frame-buffer (shares `line_tx` so post-AA still applies) rather than
   * the caller's line frame-buffer: the latter has the scene depth attached, which cannot be bound
   * as `scene_depth_tx` at the same time. Occlusion is done in the fragment shader instead. */
  GPU_framebuffer_bind(res_->overlay_line_only_fb);
  manager.submit(pass_, view);
}

/** \} */

}  // namespace blender::draw::overlay
