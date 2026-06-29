/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Seed-face picking and the topological face flood-fill for the Extract Region
 * tool: grow the connected face island under the cursor by face set id or by
 * sculpt mask. This is the only logic unique to Extract Region; the output and
 * preview paths reuse the shared #extract engine.
 */

#include "BKE_customdata.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_subdiv_ccg.hh"

#include "DNA_customdata_types.h"
#include "DNA_mesh_types.h"

#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "bmesh.hh"

#include <variant>

#include "sculpt_extract_region_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_region {

BMFace *find_seed_face(extract::ExtractSharedData &shared, const float /*mval*/[2])
{
  Object *obact = shared.obact;
  SculptSession *ss = obact->runtime->sculpt_session;
  if (!ss || !shared.bm) {
    return nullptr;
  }
  Mesh *mesh = id_cast<Mesh *>(obact->data);

  if (shared.pbvh_type == bke::pbvh::Type::Mesh) {
    if (ss->active_face_index.has_value()) {
      const int fi = *ss->active_face_index;
      if (fi >= 0 && fi < mesh->faces().size()) {
        return BM_face_at_index(shared.bm, fi);
      }
    }
  }
  else if (shared.pbvh_type == bke::pbvh::Type::Grids) {
    if (ss->active_grid_index.has_value() && ss->subdiv_ccg) {
      const int fi = BKE_subdiv_ccg_grid_to_face_index(*ss->subdiv_ccg,
                                                       *ss->active_grid_index);
      if (fi >= 0 && fi < mesh->faces().size()) {
        return BM_face_at_index(shared.bm, fi);
      }
    }
  }
  else { /* BMesh PBVH — mirror _pick.cc BMesh branch. */
    const ActiveVert av = ss->active_vert();
    if (std::holds_alternative<BMVert *>(av)) {
      BMVert *av_in_ss = std::get<BMVert *>(av);
      const int vi = BM_elem_index_get(av_in_ss);
      if (vi >= 0) {
        BMVert *v = BM_vert_at_index(shared.bm, vi);
        if (v) {
          BMFace *f;
          BMIter iter;
          BM_ITER_ELEM (f, &iter, v, BM_FACES_OF_VERT) {
            return f;
          }
        }
      }
    }
    /* Fallback: active vertex index (valid for all PBVH types). */
    const int av_idx = ss->active_vert_index();
    if (av_idx >= 0 && av_idx < shared.bm->totvert) {
      BMVert *v = BM_vert_at_index(shared.bm, av_idx);
      if (v) {
        BMFace *f;
        BMIter iter;
        BM_ITER_ELEM (f, &iter, v, BM_FACES_OF_VERT) {
          return f;
        }
      }
    }
  }
  return nullptr;
}

static bool face_passes_mask(BMFace *face, int cd_mask_offset, float threshold)
{
  if (cd_mask_offset == -1) {
    return false; /* No mask attribute: nothing to extract. */
  }
  BMLoop *l;
  BMIter iter;
  BM_ITER_ELEM (l, &iter, face, BM_LOOPS_OF_FACE) {
    if (BM_ELEM_CD_GET_FLOAT(l->v, cd_mask_offset) < threshold) {
      return false;
    }
  }
  return true;
}

void select_region(extract::ExtractSharedData &shared,
                   RegionSource source,
                   float mask_threshold,
                   BMFace *seed_face)
{
  shared.preview_faces.clear();
  if (!seed_face || BM_elem_flag_test(seed_face, BM_ELEM_HIDDEN)) {
    return;
  }

  BMesh *bm = shared.bm;
  const int cd_face_set = CustomData_get_offset_named(
      &bm->pdata, CD_PROP_INT32, ".sculpt_face_set");
  const int cd_mask = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  int seed_id = 0;
  if (source == RegionSource::FaceSet) {
    /* The default (undifferentiated) face set covers the whole mesh; extracting or
     * extruding it is meaningless, so treat the default set as an empty region: no
     * preview is shown and the extrude/extract is suppressed. The same applies when
     * the mesh has no face-set attribute (the whole mesh is implicitly default). */
    if (cd_face_set == -1) {
      return;
    }
    const Mesh *mesh = id_cast<const Mesh *>(shared.obact->data);
    seed_id = BM_ELEM_CD_GET_INT(seed_face, cd_face_set);
    if (seed_id == mesh->face_sets_color_default) {
      return;
    }
  }

  auto passes = [&](BMFace *f) -> bool {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      return false;
    }
    if (source == RegionSource::FaceSet) {
      return BM_ELEM_CD_GET_INT(f, cd_face_set) == seed_id;
    }
    return face_passes_mask(f, cd_mask, mask_threshold);
  };

  if (!passes(seed_face)) {
    return;
  }

  Set<BMFace *> visited;
  Vector<BMFace *> stack;
  visited.add(seed_face);
  stack.append(seed_face);
  while (!stack.is_empty()) {
    BMFace *face = stack.pop_last();
    shared.preview_faces.append(face);
    BMLoop *l;
    BMIter liter;
    BM_ITER_ELEM (l, &liter, face, BM_LOOPS_OF_FACE) {
      BMLoop *radial = l->radial_next;
      while (radial != l) {
        BMFace *other = radial->f;
        if (!visited.contains(other) && passes(other)) {
          visited.add(other);
          stack.append(other);
        }
        radial = radial->radial_next;
      }
    }
  }
}

bool region_is_valid(const extract::ExtractSharedData &shared)
{
  return !shared.preview_faces.is_empty();
}

}  // namespace blender::ed::sculpt_paint::extract_region
