/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_curve_patch.hh"

#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"

#include "BKE_mesh.hh"

namespace blender::bke {

void CurvePatchSurfaceSnapshot::clear()
{
  /* The tree goes first: it holds spans into `positions` and the topology arrays, so releasing
   * those while it is still alive would leave it pointing at freed memory for as long as this
   * function runs. */
  bvh = {};
  positions.reinitialize(0);
  vert_normals.reinitialize(0);
  face_offsets.reinitialize(0);
  corner_verts.reinitialize(0);
  corner_tris.reinitialize(0);
  ready = false;
}

bool curve_patch_surface_snapshot_build(const Mesh &mesh, CurvePatchSurfaceSnapshot &r_snapshot)
{
  r_snapshot.clear();
  if (mesh.verts_num == 0 || mesh.corner_tris().is_empty()) {
    return false;
  }
  r_snapshot.positions = mesh.vert_positions();
  r_snapshot.vert_normals = mesh.vert_normals();
  r_snapshot.face_offsets = mesh.face_offsets();
  r_snapshot.corner_verts = mesh.corner_verts();
  r_snapshot.corner_tris = mesh.corner_tris();
  r_snapshot.bvh = bvhtree_from_mesh_corner_tris_ex(
      r_snapshot.positions,
      OffsetIndices<int>(r_snapshot.face_offsets.as_span()),
      r_snapshot.corner_verts,
      r_snapshot.corner_tris,
      IndexMask(mesh.faces_num));
  r_snapshot.ready = r_snapshot.bvh.tree != nullptr;
  return r_snapshot.ready;
}

/** Nearest-triangle query that skips triangles facing away from a reference normal. */
struct OrientedNearestData {
  const BVHTreeFromMesh *bvh;
  float3 reference_normal;
  float min_dot;
};

static void oriented_nearest_callback(void *userdata,
                                      const int index,
                                      const float co[3],
                                      BVHTreeNearest *nearest)
{
  /* The callback receives the user data as a bare `void *`, but it never writes through it: read
   * it back as const and keep the mutability at the call site. */
  const OrientedNearestData &data = *static_cast<const OrientedNearestData *>(userdata);
  /* The mesh callback only overwrites its argument when the triangle is closer, so run it on a
   * copy and adopt the result when the triangle both improved it and is acceptably oriented. */
  BVHTreeNearest candidate = *nearest;
  /* The BVH callback interface takes a mutable user-data pointer, which is the one const_cast the
   * tree API leaves no way around; the snapshot itself stays const. */
  data.bvh->nearest_callback(const_cast<BVHTreeFromMesh *>(data.bvh), index, co, &candidate);
  if (candidate.index != index || !(candidate.dist_sq < nearest->dist_sq)) {
    return;
  }
  if (math::dot(math::normalize(float3(candidate.no)), data.reference_normal) < data.min_dot) {
    return;
  }
  *nearest = candidate;
}

void curve_patch_surface_shrinkwrap(const CurvePatchSurfaceSnapshot &snapshot,
                                    const float max_dist,
                                    MutableSpan<float3> positions,
                                    MutableSpan<float3> r_normals,
                                    const Span<float3> reference_normals)
{
  BLI_assert(positions.size() == r_normals.size());
  r_normals.fill(float3(0.0f));
  if (!snapshot.ready || !(max_dist > 0.0f)) {
    return;
  }
  const bool oriented = snapshot.min_normal_dot > -1.0f &&
                        reference_normals.size() == positions.size();
  for (const int i : positions.index_range()) {
    BVHTreeNearest nearest{};
    nearest.index = -1;
    nearest.dist_sq = max_dist * max_dist;
    if (oriented) {
      /* The callback takes a bare `void *`, so the local cannot be const even though the callback
       * only reads it. */
      OrientedNearestData data{&snapshot.bvh, reference_normals[i], snapshot.min_normal_dot};
      BLI_bvhtree_find_nearest(
          snapshot.bvh.tree, positions[i], &nearest, oriented_nearest_callback, &data);
    }
    else {
      BLI_bvhtree_find_nearest(snapshot.bvh.tree,
                               positions[i],
                               &nearest,
                               snapshot.bvh.nearest_callback,
                               const_cast<BVHTreeFromMesh *>(&snapshot.bvh));
    }
    if (nearest.index < 0) {
      /* The sample hangs out of reach: dragging it onto distant geometry is worse than leaving it
       * where the user put it. */
      continue;
    }
    if (oriented) {
      /* A sample beside the mesh (a curve running off its edge onto a neighbor) would otherwise be
       * clamped onto the edge line; leave it where it is, like an out-of-reach sample. */
      const float3 offset = float3(nearest.co) - positions[i];
      const float3 lateral = offset - reference_normals[i] * math::dot(offset, reference_normals[i]);
      if (math::length(lateral) >
          CURVE_PATCH_SHRINKWRAP_MAX_LATERAL_FRACTION * max_dist)
      {
        continue;
      }
    }
    positions[i] = float3(nearest.co);
    /* The normal comes from the SNAPSHOT's triangle rather than through `Mesh::face_normals()`,
     * which reflects positions the patch has already displaced. */
    r_normals[i] = math::normalize(float3(nearest.no));
  }
}

void curve_patch_surface_fill_invalid_normals(MutableSpan<float3> normals, const float3 &fallback)
{
  const int count = int(normals.size());
  int first_valid = -1;
  for (const int i : IndexRange(count)) {
    if (math::length_squared(normals[i]) > 1e-8f) {
      first_valid = i;
      break;
    }
  }
  if (first_valid < 0) {
    normals.fill(fallback);
    return;
  }
  for (const int i : IndexRange(first_valid)) {
    normals[i] = normals[first_valid];
  }
  int prev_valid = first_valid;
  for (int i = first_valid + 1; i < count; i++) {
    if (math::length_squared(normals[i]) > 1e-8f) {
      const int gap = i - prev_valid;
      for (int k = 1; k < gap; k++) {
        const float t = float(k) / float(gap);
        const float3 blended = math::interpolate(normals[prev_valid], normals[i], t);
        /* Two exactly opposed neighbours cancel at the midpoint; holding the previous valid normal
         * there is arbitrary but finite, which is all the consumers require. */
        normals[prev_valid + k] = math::length_squared(blended) > 1e-8f ?
                                      math::normalize(blended) :
                                      normals[prev_valid];
      }
      prev_valid = i;
    }
  }
  for (int i = prev_valid + 1; i < count; i++) {
    normals[i] = normals[prev_valid];
  }
}

}  // namespace blender::bke
