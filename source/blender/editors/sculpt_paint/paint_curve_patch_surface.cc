/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_surface.hh"

#include "BLI_index_range.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_vector.hh"

#include "BKE_mesh.hh"

namespace blender::ed::sculpt_paint {

void CurvePatchSurfaceSnapshot::clear()
{
  /* The tree goes first: it holds spans into `positions`, so releasing the arrays while it is still
   * alive would leave it pointing at freed memory for as long as this function runs. */
  bvh = {};
  positions.reinitialize(0);
  vert_normals.reinitialize(0);
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
  r_snapshot.bvh = bke::bvhtree_from_mesh_corner_tris_ex(r_snapshot.positions,
                                                         mesh.faces(),
                                                         mesh.corner_verts(),
                                                         mesh.corner_tris(),
                                                         IndexMask(mesh.faces_num));
  r_snapshot.ready = r_snapshot.bvh.tree != nullptr;
  return r_snapshot.ready;
}

void curve_patch_surface_shrinkwrap(const CurvePatchSurfaceSnapshot &snapshot,
                                    const float max_dist,
                                    MutableSpan<float3> positions,
                                    MutableSpan<float3> r_normals)
{
  BLI_assert(positions.size() == r_normals.size());
  r_normals.fill(float3(0.0f));
  if (!snapshot.ready || !(max_dist > 0.0f)) {
    return;
  }
  for (const int i : positions.index_range()) {
    BVHTreeNearest nearest{};
    nearest.index = -1;
    nearest.dist_sq = max_dist * max_dist;
    BLI_bvhtree_find_nearest(snapshot.bvh.tree,
                             positions[i],
                             &nearest,
                             snapshot.bvh.nearest_callback,
                             const_cast<bke::BVHTreeFromMesh *>(&snapshot.bvh));
    if (nearest.index < 0) {
      /* The sample hangs out of reach: dragging it onto distant geometry is worse than leaving it
       * where the user put it. */
      continue;
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

}  // namespace blender::ed::sculpt_paint
