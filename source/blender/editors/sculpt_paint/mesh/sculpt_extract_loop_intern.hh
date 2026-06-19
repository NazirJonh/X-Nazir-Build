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

#include "ED_numinput.hh" /* NumInput */

/* Complete BMesh types and the #BM_elem_index_get macro are required here because
 * #vert_position is defined inline below and dereferences #BMVert. */
#include "bmesh.hh"

#include "sculpt_extract_loop.hh"

struct Mesh;
struct Object;
struct ARegion;
struct RegionView3D;
struct wmOperator;
struct bContext;

namespace blender::ed::sculpt_paint::extract_loop {

/**
 * Fields shared between the idle hover system and the modal operator.
 * Both #HoverState and #ExtractLoopModalData embed this struct.
 */
struct ExtractLoopSharedData {
  BMesh *bm = nullptr;
  bke::pbvh::Type pbvh_type = bke::pbvh::Type::Mesh;
  /* Evaluated vertex positions for Mesh PBVH; cage positions for Grids PBVH;
   * empty for BMesh PBVH (#vert_position falls back to #BMVert.co). */
  Span<float3> preview_positions;

  Vector<BMEdge *> loop_edges;
  Vector<float3> preview_points;
  /* FaceStrip mode: face loop from #BMW_FACELOOP (Edit Mode face loop select). */
  Vector<BMFace *> preview_faces;
  bool is_cyclic = false;
  BMEdge *seed_edge = nullptr;
  ExtractionMode mode = ExtractionMode::Loop;
  LoopOrientation loop_orientation = LoopOrientation::Horizontal;
  float3 hit_location = float3(0.0f);

  Object *obact = nullptr;
  ARegion *region = nullptr;
  RegionView3D *rv3d = nullptr;

  /* Cached mesh identity — rebuild #bm when topology or object changes. */
  const Mesh *session_mesh = nullptr;
  int session_totvert = 0;
  int session_totedge = 0;
  int session_totface = 0;
};

inline float3 vert_position(const ExtractLoopSharedData &shared, BMVert *v)
{
  if (shared.pbvh_type == bke::pbvh::Type::BMesh) {
    return float3(v->co);
  }
  return shared.preview_positions[BM_elem_index_get(v)];
}

enum class ModalPhase {
  Select,
  Extrude,
};

struct ExtractLoopModalData {
  ExtractLoopSharedData shared;

  /* Set of loop edges for O(1) membership test during extraction and face-strip collection. */
  Set<BMEdge *> loop_edges_set;
  /* True when the seed is a boundary edge, enabling loop/boundary cycling. */
  bool has_boundary_seed = false;
  /* Toggled on #LEFTMOUSE when #has_boundary_seed is true. */
  bool use_boundary_walker = false;
  bool initial_hit = false;

  ModalPhase phase = ModalPhase::Select;
  float extrude_distance = 0.0f;
  /* Center and direction of the extrude axis, in object space. */
  float3 extrude_axis_center = float3(0.0f);
  float3 extrude_avg_normal = float3(0.0f, 0.0f, 1.0f);
  /* Cursor position when the extrude drag started, in region space. */
  float2 extrude_init_mval = float2(0.0f);
  NumInput num_input{};
  bool num_input_initialized = false;

  BMesh *edit_bm = nullptr;
  Vector<BMVert *> extrude_verts;
  Vector<float3> extrude_base_co;
  Vector<float3> extrude_normals;
  Vector<BMFace *> extrude_preview_faces;

  void *draw_handle = nullptr;
};

/* _pick.cc */
BMEdge *find_seed_edge_screen_space(ExtractLoopSharedData &shared, const float mval[2]);

/* _walker.cc */
void run_walker(ExtractLoopSharedData &shared,
                bool use_boundary_walker,
                bool *r_has_boundary_seed);
bool extract_preview_is_valid(const ExtractLoopSharedData &shared);
/* Single source for the EDGE/FACE element choice: #BM_FACE for FaceStrip, else #BM_EDGE. */
char mode_geom_htype(ExtractionMode mode);

/* _draw.cc */
void draw_loop_preview(const ExtractLoopSharedData &shared);
void draw_face_strip_preview(const ExtractLoopSharedData &shared,
                             BMesh *draw_bm = nullptr,
                             const Span<BMFace *> faces_override = {});

/* _hover.cc */
BMesh *create_modal_bmesh(Object *obact, bke::pbvh::Type pbvh_type);
void sync_preview_from_hover(ExtractLoopModalData &data);

/* _output.cc */
bool is_face_strip_extrude_output(const wmOperator *op, ExtractionMode mode);
bool extrude_begin(bContext &C, wmOperator *op, ExtractLoopModalData &data);
void extrude_commit(bContext &C, wmOperator *op, ExtractLoopModalData &data);
void extrude_apply_distance(ExtractLoopModalData &data, float distance);
/* Project the cursor onto the extrude axis and apply the resulting distance. */
void extrude_update_from_mouse(ExtractLoopModalData &data, const float mval[2]);
void extrude_update_status_text(bContext *C, const ExtractLoopModalData &data);
void finish_extract(bContext *C, wmOperator *op, ExtractLoopModalData *data);

/* sculpt_extract_loop.cc (core) */
void gesture_data_free(bContext *C, ExtractLoopModalData *data);

}  // namespace blender::ed::sculpt_paint::extract_loop
