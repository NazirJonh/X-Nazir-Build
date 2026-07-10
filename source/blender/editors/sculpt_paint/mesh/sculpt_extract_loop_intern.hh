/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal shared state and cross-translation-unit helpers for the Extract Loop
 * tool. Not part of the public sculpt API.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "BKE_paint_bvh.hh" /* bke::pbvh::Type */

/* Complete BMesh types and the #BM_elem_index_get macro are required here because
 * #vert_position is defined inline below and dereferences #BMVert. */
#include "bmesh.hh"

#include "sculpt_extract_loop.hh"
#include "sculpt_extract_shared.hh"

struct Mesh;
struct Object;
struct ARegion;
struct RegionView3D;
struct wmOperator;
struct bContext;

namespace blender::ed::sculpt_paint::extract_loop {

/**
 * Loop-specific state shared between the idle hover system and the modal
 * operator, built on top of the neutral #extract::ExtractSharedData engine
 * state. Both #HoverState and #ExtractLoopModalData embed this struct.
 */
struct ExtractLoopSharedData {
  extract::ExtractSharedData base;

  Vector<BMEdge *> loop_edges;
  Vector<float3> preview_points;
  bool is_cyclic = false;
  BMEdge *seed_edge = nullptr;
  ExtractionMode mode = ExtractionMode::Loop;
  LoopOrientation loop_orientation = LoopOrientation::Horizontal;

  /* Cache of the mesh's boundary edges (edges used by exactly one face), rebuilt
   * whenever #base.bm is (re)built. Enables boundary-first seed picking without a
   * per-hover scan of the whole mesh. */
  Vector<BMEdge *> boundary_edges;
};

inline float3 vert_position(const ExtractLoopSharedData &shared, BMVert *v)
{
  return extract::vert_position(shared.base, v);
}

struct ExtractLoopModalData {
  ExtractLoopSharedData shared;
  extract::ExtrudeState extrude;

  /* Set of loop edges for O(1) membership test during extraction and face-strip collection. */
  Set<BMEdge *> loop_edges_set;
  /* True when the seed is a boundary edge, enabling loop/boundary cycling. */
  bool has_boundary_seed = false;
  /* Toggled on #LEFTMOUSE when #has_boundary_seed is true. */
  bool use_boundary_walker = false;
  bool initial_hit = false;

  void *draw_handle = nullptr;
};

/* _pick.cc */
BMEdge *find_seed_edge_screen_space(ExtractLoopSharedData &shared, const float mval[2]);
/* Rebuild #ExtractLoopSharedData.boundary_edges from #base.bm. Call after the
 * modal BMesh is (re)built. */
void rebuild_boundary_edge_cache(ExtractLoopSharedData &shared);
/* Boundary-first seed pick: nearest visible boundary edge in screen space within
 * a pixel threshold, independent of a surface hit under the cursor. Returns
 * nullptr when no boundary edge qualifies, so the caller falls back to
 * #find_seed_edge_screen_space. Needs #C for a session-safe occlusion raycast. */
BMEdge *find_boundary_seed_edge_screen_space(bContext *C,
                                             ExtractLoopSharedData &shared,
                                             const float mval[2]);

/* _walker.cc */
void run_walker(ExtractLoopSharedData &shared,
                bool use_boundary_walker,
                bool *r_has_boundary_seed);
bool extract_preview_is_valid(const ExtractLoopSharedData &shared);
/* Single source for the EDGE/FACE element choice: #BM_FACE for FaceStrip, else #BM_EDGE. */
char mode_geom_htype(ExtractionMode mode);

/* _draw.cc */
void draw_loop_preview(const ExtractLoopSharedData &shared);

/* _hover.cc */
void sync_preview_from_hover(ExtractLoopModalData &data);

/* _output.cc */
bool is_face_strip_extrude_output(const wmOperator *op, ExtractionMode mode);
void finish_extract(bContext *C, wmOperator *op, ExtractLoopModalData *data);

/* sculpt_extract_loop.cc (core) */
void gesture_data_free(bContext *C, ExtractLoopModalData *data);

}  // namespace blender::ed::sculpt_paint::extract_loop
