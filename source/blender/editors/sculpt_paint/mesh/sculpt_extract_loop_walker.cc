/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * BMesh loop/ring/face-loop walkers for the Extract Loop tool, plus the
 * per-mode helpers (#mode_geom_htype, #extract_preview_is_valid) that centralize
 * the EDGE/FACE element choice.
 */

#include "BLI_math_vector.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "sculpt_extract_loop_intern.hh"

namespace blender::ed::sculpt_paint::extract_loop {

/**
 * Walk a face loop from #shared.seed_edge using #BMW_FACELOOP — the same walker
 * used by Loop Select in Edit Mode with Face select mode enabled.
 */
static void run_face_loop_walker(ExtractLoopSharedData &shared, bool *r_has_boundary_seed)
{
  shared.loop_edges.clear();
  shared.preview_points.clear();
  shared.base.preview_faces.clear();
  shared.is_cyclic = false;
  if (r_has_boundary_seed) {
    *r_has_boundary_seed = false;
  }

  if (!shared.seed_edge) {
    return;
  }

  BMWalker walker;
  BMW_init(&walker,
           shared.base.bm,
           BMW_FACELOOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_FLAG_TEST_HIDDEN,
           BMW_NIL_LAY,
           BMW_DELIMIT_NONE);

  for (BMFace *f = static_cast<BMFace *>(BMW_begin(&walker, shared.seed_edge)); f;
       f = static_cast<BMFace *>(BMW_step(&walker)))
  {
    shared.base.preview_faces.append(f);
  }
  BMW_end(&walker);
}

bool extract_preview_is_valid(const ExtractLoopSharedData &shared)
{
  if (shared.mode == ExtractionMode::FaceStrip) {
    return !shared.base.preview_faces.is_empty();
  }
  return !shared.loop_edges.is_empty();
}

char mode_geom_htype(const ExtractionMode mode)
{
  return mode == ExtractionMode::FaceStrip ? BM_FACE : BM_EDGE;
}

/**
 * Run the edge-loop/ring BMesh walker and fill #shared.loop_edges,
 * #shared.preview_points, and #shared.is_cyclic.
 *
 * \param use_boundary_walker: When the seed is a boundary edge, use
 *   #BMW_EDGEBOUNDARY instead of #BMW_EDGELOOP. Hover always passes `true`;
 *   the modal operator toggles this on #LEFTMOUSE.
 * \param r_has_boundary_seed: Set to `true` when the seed edge is a boundary
 *   edge. May be `nullptr` (hover does not need it).
 */
void run_walker(ExtractLoopSharedData &shared,
                bool use_boundary_walker,
                bool *r_has_boundary_seed)
{
  shared.loop_edges.clear();
  shared.preview_points.clear();
  shared.base.preview_faces.clear();
  shared.is_cyclic = false;
  if (r_has_boundary_seed) {
    *r_has_boundary_seed = false;
  }

  if (!shared.seed_edge) {
    return;
  }

  if (shared.mode == ExtractionMode::FaceStrip) {
    run_face_loop_walker(shared, r_has_boundary_seed);
    return;
  }

  int walker_type;
  BMWDelimitFlag delimit;

  if (shared.mode == ExtractionMode::Ring) {
    walker_type = BMW_EDGERING;
    delimit = BMW_DELIMIT_EDGE_RING_NGONS;
  }
  else {
    const bool non_manifold = BM_edge_face_count_is_over(shared.seed_edge, 2);
    const bool is_boundary = BM_edge_is_boundary(shared.seed_edge);
    if (r_has_boundary_seed) {
      *r_has_boundary_seed = is_boundary;
    }

    if (non_manifold) {
      walker_type = BMW_EDGELOOP_NONMANIFOLD;
      delimit = BMWDelimitFlag(BMW_DELIMIT_EDGE_LOOP_OUTER_CORNERS | BMW_DELIMIT_EDGE_LOOP_NGONS);
    }
    else if (use_boundary_walker && is_boundary) {
      walker_type = BMW_EDGEBOUNDARY;
      delimit = BMW_DELIMIT_NONE;
    }
    else {
      walker_type = BMW_EDGELOOP;
      delimit = BMWDelimitFlag(BMW_DELIMIT_EDGE_LOOP_OUTER_CORNERS | BMW_DELIMIT_EDGE_LOOP_NGONS);
    }
  }

  BMWalker walker;
  BMW_init(&walker,
           shared.base.bm,
           walker_type,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_MASK_NOP,
           BMW_FLAG_TEST_HIDDEN,
           BMW_NIL_LAY,
           delimit);

  for (BMEdge *e = static_cast<BMEdge *>(BMW_begin(&walker, shared.seed_edge)); e;
       e = static_cast<BMEdge *>(BMW_step(&walker)))
  {
    shared.loop_edges.append(e);
  }
  BMW_end(&walker);

  if (shared.loop_edges.is_empty()) {
    return;
  }

  if (shared.mode == ExtractionMode::Loop) {
    /* Order vertices along the loop to draw a continuous polyline. */
    Vector<BMVert *> verts;
    if (shared.loop_edges.size() == 1) {
      verts.append(shared.loop_edges[0]->v1);
      verts.append(shared.loop_edges[0]->v2);
    }
    else {
      BMEdge *first = shared.loop_edges[0];
      BMEdge *second = shared.loop_edges[1];
      BMVert *current_v;
      if (first->v1 == second->v1 || first->v1 == second->v2) {
        current_v = first->v2;
      }
      else {
        current_v = first->v1;
      }
      verts.append(current_v);
      for (BMEdge *e : shared.loop_edges) {
        current_v = (e->v1 == current_v) ? e->v2 : e->v1;
        verts.append(current_v);
      }
      /* Detect cyclic loop by pointer identity. */
      if (verts.size() > 1 && verts.first() == verts.last()) {
        shared.is_cyclic = true;
        verts.remove_last();
      }
    }
    for (BMVert *v : verts) {
      shared.preview_points.append(vert_position(shared, v));
    }
  }
  else {
    /* Ring: disconnected edge pairs. */
    for (BMEdge *e : shared.loop_edges) {
      shared.preview_points.append(vert_position(shared, e->v1));
      shared.preview_points.append(vert_position(shared, e->v2));
    }
  }
}

}  // namespace blender::ed::sculpt_paint::extract_loop
