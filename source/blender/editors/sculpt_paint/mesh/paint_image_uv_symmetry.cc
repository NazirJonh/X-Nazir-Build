/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_image_uv_symmetry.hh"

#include <algorithm>
#include <limits>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_kdopbvh.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BKE_bvhutils.hh"
#include "BKE_editmesh.hh"
#include "BKE_editmesh_bvh.hh"
#include "BKE_mesh.hh"

#include "bmesh.hh"

namespace blender {

/** Fraction of the mesh bounding-box diagonal a mirrored point may miss the surface by. */
static constexpr float SYMMETRY_SNAP_TOLERANCE_FACTOR = 1e-4f;

static float symmetry_snap_tolerance(BMesh *bm)
{
  float3 min(std::numeric_limits<float>::max());
  float3 max(std::numeric_limits<float>::lowest());
  BMIter viter;
  BMVert *v;
  BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
    const float3 co(v->co);
    min = math::min(min, co);
    max = math::max(max, co);
  }
  if (bm->totvert == 0) {
    return 0.0f;
  }
  const float diagonal = math::distance(min, max);
  /* A degenerate mesh (all vertices coincident) still deserves a usable epsilon. */
  return std::max(diagonal * SYMMETRY_SNAP_TOLERANCE_FACTOR, 1e-6f);
}

/**
 * Locate the face nearest \a position, or -1 when nothing lies within \a tolerance.
 *
 * Object Mode resolves through the Mesh BVH and relies on #BM_mesh_bm_from_me creating
 * faces in Mesh order, which it does — except that a degenerate face is skipped, shifting
 * every later index. Blender reports such a mesh as "Bad face in mesh" on conversion.
 * The existing fill ray-cast already depends on the same property.
 */
static int symmetry_nearest_face(Object *ob,
                                 BMesh *bm,
                                 const float3 &position,
                                 const float tolerance)
{
  if (ob->mode & OB_MODE_EDIT) {
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (em == nullptr || em->bm == nullptr) {
      return -1;
    }
    BLI_assert(em->bm == bm);
    BM_mesh_elem_index_ensure(em->bm, BM_FACE);
    BMBVHTree *bmbvh = BKE_bmbvh_new_from_editmesh(em, BMBVH_RESPECT_HIDDEN, nullptr, false);
    if (bmbvh == nullptr) {
      return -1;
    }
    BMFace *efa = BKE_bmbvh_find_face_closest(bmbvh, position, tolerance);
    BKE_bmbvh_free(bmbvh);
    return efa != nullptr ? BM_elem_index_get(efa) : -1;
  }

  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (mesh == nullptr) {
    return -1;
  }
  if (bm->totface != mesh->faces_num) {
    /* #BM_mesh_bm_from_me skips degenerate faces, which would misalign BMesh face
     * indices with the Mesh corner-tri face indices returned below. Bail out rather
     * than risk mirroring onto the wrong face. */
    return -1;
  }

  bke::BVHTreeFromMesh tree_data = mesh->bvh_corner_tris_no_hidden();
  if (tree_data.tree == nullptr) {
    return -1;
  }

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = tolerance * tolerance;
  BLI_bvhtree_find_nearest(
      tree_data.tree, position, &nearest, tree_data.nearest_callback, &tree_data);
  if (nearest.index < 0) {
    return -1;
  }
  const Span<int> tri_faces = mesh->corner_tri_faces();
  if (nearest.index >= tri_faces.size()) {
    return -1;
  }
  return tri_faces[nearest.index];
}

void image_paint_symmetry_mirror_faces(Object *ob,
                                       BMesh *bm,
                                       const float3 &hit_position,
                                       const int symmetry_flags,
                                       Vector<int> &r_seed_faces)
{
  if (ob == nullptr || bm == nullptr || symmetry_flags == 0) {
    return;
  }

  const float tolerance = symmetry_snap_tolerance(bm);
  if (tolerance <= 0.0f) {
    return;
  }

  /* 1..7 enumerates X, Y, Z, XY, XZ, YZ and XYZ, the standard Blender mirror passes. */
  for (int symm_it = 1; symm_it <= 7; symm_it++) {
    if ((symm_it & symmetry_flags) != symm_it) {
      continue;
    }
    float3 mirrored = hit_position;
    if (symm_it & 1) {
      mirrored.x = -mirrored.x;
    }
    if (symm_it & 2) {
      mirrored.y = -mirrored.y;
    }
    if (symm_it & 4) {
      mirrored.z = -mirrored.z;
    }

    const int face_index = symmetry_nearest_face(ob, bm, mirrored, tolerance);
    if (face_index < 0 || face_index >= bm->totface) {
      continue;
    }
    if (r_seed_faces.contains(face_index)) {
      continue;
    }
    r_seed_faces.append(face_index);
  }
}

}  // namespace blender
