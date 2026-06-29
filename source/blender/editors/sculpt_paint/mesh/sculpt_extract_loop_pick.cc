/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Seed-edge picking for the Extract Loop tool: screen-space distance and
 * orientation metrics that select the edge under the cursor.
 */

#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_subdiv_ccg.hh"

#include "DNA_mesh_types.h"

#include "ED_view3d.hh"

#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "bmesh.hh"

#include <variant>

#include "sculpt_extract_loop_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_loop {

/**
 * Project an object-space point to screen (region pixel) coordinates.
 * Returns true on success.
 *
 * Uses #V3D_PROJ_TEST_NOP (no clipping) because the points we project are
 * vertices of the face/edge under the cursor, which is by construction in
 * front of the camera. Using #V3D_PROJ_TEST_CLIP_DEFAULT here caused every
 * candidate edge to return #FLT_MAX in some camera orientations (endpoints
 * barely outside the clip window), leaving the preview empty.
 */
static bool project_object_to_screen(const ARegion *region, const float3 &co, float r_screen[2])
{
  return ED_view3d_project_float_object(region, co, r_screen, V3D_PROJ_TEST_NOP) ==
         V3D_PROJ_RET_OK;
}

/**
 * Screen-space squared distance from `mval` to the 2D projection of the edge
 * `(co1, co2)`. Returns #FLT_MAX when either endpoint cannot be projected
 * (e.g. behind the camera), so such edges are never picked.
 *
 * This mirrors the metric used by #EDBM_unified_findnearest in Edit Mode: the
 * edge under the cursor is the one whose screen projection is closest to the
 * mouse, not the one whose 3D distance to the surface hit-point is smallest.
 * Using 3D distance (as the previous implementation did) is unreliable because
 * it ignores occlusion and perspective, often picking an edge on the back of
 * the mesh instead of the one visible under the cursor.
 */
static float screen_dist_sq_to_edge(const ARegion *region,
                                    const float mval[2],
                                    const float3 &co1,
                                    const float3 &co2)
{
  float a[2], b[2];
  if (!project_object_to_screen(region, co1, a)) {
    return FLT_MAX;
  }
  if (!project_object_to_screen(region, co2, b)) {
    return FLT_MAX;
  }
  return dist_squared_to_line_segment_v2(mval, a, b);
}

/**
 * True when the screen-space projection of the edge is closer to horizontal
 * than vertical (used to pick the loop orientation on a quad grid).
 */
static bool edge_is_horizontal_screen(const ARegion *region, const float3 &co1, const float3 &co2)
{
  float a[2], b[2];
  if (!project_object_to_screen(region, co1, a) || !project_object_to_screen(region, co2, b)) {
    return true;
  }
  return fabsf(b[0] - a[0]) >= fabsf(b[1] - a[1]);
}

static BMEdge *pick_closest_edge_screen_space(ExtractLoopSharedData &shared,
                                              const float mval[2],
                                              const Vector<BMEdge *> &candidates)
{
  BMEdge *best = nullptr;
  float best_dist_sq = FLT_MAX;
  for (BMEdge *e : candidates) {
    const float3 co1 = vert_position(shared, e->v1);
    const float3 co2 = vert_position(shared, e->v2);
    const float d = screen_dist_sq_to_edge(shared.base.region, mval, co1, co2);
    if (d < best_dist_sq) {
      best_dist_sq = d;
      best = e;
    }
  }
  return best;
}

/**
 * Pick the screen-closest edge from #candidates, preferring edges that match
 * #shared.loop_orientation (horizontal vs vertical in screen space).
 */
static BMEdge *pick_oriented_edge_screen_space(ExtractLoopSharedData &shared,
                                               const float mval[2],
                                               const Vector<BMEdge *> &candidates)
{
  if (candidates.is_empty()) {
    return nullptr;
  }

  const bool want_horizontal = shared.loop_orientation == LoopOrientation::Horizontal;
  Vector<BMEdge *> oriented;
  oriented.reserve(candidates.size());
  for (BMEdge *e : candidates) {
    const float3 co1 = vert_position(shared, e->v1);
    const float3 co2 = vert_position(shared, e->v2);
    if (edge_is_horizontal_screen(shared.base.region, co1, co2) == want_horizontal) {
      oriented.append(e);
    }
  }

  const Vector<BMEdge *> &pool = oriented.is_empty() ? candidates : oriented;
  return pick_closest_edge_screen_space(shared, mval, pool);
}

/**
 * Find the seed edge under the cursor using screen-space closest-point metric.
 * Uses PBVH raycast results from #cursor_geometry_info_update (already accounts
 * for occlusion) to narrow candidates, then picks the screen-space closest edge.
 * Falls back to active vertex index, then brute-force 3D distance.
 */
BMEdge *find_seed_edge_screen_space(ExtractLoopSharedData &shared, const float mval[2])
{
  Object *obact = shared.base.obact;
  SculptSession *ss = obact->runtime->sculpt_session;
  if (!ss || !shared.base.bm) {
    return nullptr;
  }

  ED_view3d_init_mats_rv3d(obact, shared.base.rv3d);

  Mesh *mesh = id_cast<Mesh *>(obact->data);
  Vector<BMEdge *> candidates;

  if (shared.base.pbvh_type == bke::pbvh::Type::Mesh) {
    if (ss->active_face_index.has_value()) {
      const int fi = *ss->active_face_index;
      if (fi >= 0 && fi < mesh->faces().size()) {
        const IndexRange face = mesh->faces()[fi];
        for (const int corner : face) {
          const int edge_i = mesh->corner_edges()[corner];
          candidates.append(BM_edge_at_index(shared.base.bm, edge_i));
        }
      }
    }
  }
  else if (shared.base.pbvh_type == bke::pbvh::Type::Grids) {
    if (ss->active_grid_index.has_value() && ss->subdiv_ccg) {
      const int grid_i = *ss->active_grid_index;
      const int fi = BKE_subdiv_ccg_grid_to_face_index(*ss->subdiv_ccg, grid_i);
      if (fi >= 0 && fi < mesh->faces().size()) {
        const IndexRange face = mesh->faces()[fi];
        for (const int corner : face) {
          const int edge_i = mesh->corner_edges()[corner];
          candidates.append(BM_edge_at_index(shared.base.bm, edge_i));
        }
      }
    }
  }
  else { /* BMesh PBVH. */
    const ActiveVert av = ss->active_vert();
    if (std::holds_alternative<BMVert *>(av)) {
      BMVert *av_in_ss = std::get<BMVert *>(av);
      const int vi = BM_elem_index_get(av_in_ss);
      if (vi >= 0) {
        BMVert *v = BM_vert_at_index(shared.base.bm, vi);
        if (v) {
          BMEdge *e;
          BMIter iter;
          BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
            candidates.append(e);
          }
        }
      }
    }
  }

  if (!candidates.is_empty()) {
    BMEdge *best = pick_oriented_edge_screen_space(shared, mval, candidates);
    if (best) {
      return best;
    }
  }

  /* Fallback 1: active vertex index (valid for all PBVH types). */
  const int av_idx = ss->active_vert_index();
  if (av_idx >= 0 && av_idx < shared.base.bm->totvert) {
    BMVert *v = BM_vert_at_index(shared.base.bm, av_idx);
    if (v) {
      candidates.clear();
      BMEdge *e;
      BMIter iter;
      BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
        candidates.append(e);
      }
      if (!candidates.is_empty()) {
        BMEdge *best = pick_oriented_edge_screen_space(shared, mval, candidates);
        if (best) {
          return best;
        }
      }
    }
  }

  /* Fallback 2: brute-force 3D distance from cursor hit location. */
  if (shared.base.hit_location == float3(0.0f)) {
    return nullptr;
  }
  const float3 hit_local = math::transform_point(obact->world_to_object(), shared.base.hit_location);

  BMEdge *best = nullptr;
  float best_dist_sq = FLT_MAX;
  const bool want_horizontal = shared.loop_orientation == LoopOrientation::Horizontal;
  BMEdge *e;
  BMIter iter;
  BM_ITER_MESH (e, &iter, shared.base.bm, BM_EDGES_OF_MESH) {
    const float3 co1 = vert_position(shared, e->v1);
    const float3 co2 = vert_position(shared, e->v2);
    if (edge_is_horizontal_screen(shared.base.region, co1, co2) != want_horizontal) {
      continue;
    }
    float closest[3];
    closest_to_line_segment_v3(closest, hit_local, co1, co2);
    const float dist_sq = math::distance_squared(float3(closest), hit_local);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = e;
    }
  }
  if (best) {
    return best;
  }

  best_dist_sq = FLT_MAX;
  BM_ITER_MESH (e, &iter, shared.base.bm, BM_EDGES_OF_MESH) {
    const float3 co1 = vert_position(shared, e->v1);
    const float3 co2 = vert_position(shared, e->v2);
    float closest[3];
    closest_to_line_segment_v3(closest, hit_local, co1, co2);
    const float dist_sq = math::distance_squared(float3(closest), hit_local);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = e;
    }
  }
  return best;
}

}  // namespace blender::ed::sculpt_paint::extract_loop
