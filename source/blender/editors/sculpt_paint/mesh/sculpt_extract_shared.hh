/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Neutral, tool-agnostic data and helpers shared by the sculpt extraction
 * gesture tools (Extract Loop, Extract Region): the modal BMesh, the previewed
 * set of faces, the in-mesh extrude engine, new-object extraction, face preview
 * drawing, and hover session bookkeeping.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_paint_bvh.hh" /* bke::pbvh::Type */

#include "ED_numinput.hh" /* NumInput */

/* Complete BMesh types and #BM_elem_index_get are required because
 * #vert_position dereferences #BMVert inline below. */
#include "bmesh.hh"

struct Mesh;
struct Object;
struct ARegion;
struct RegionView3D;
struct bContext;
struct wmOperator;

namespace blender::ed::sculpt_paint::extract {

/** Tool-agnostic state shared between the idle hover system and the modal operators. */
struct ExtractSharedData {
  BMesh *bm = nullptr;
  bke::pbvh::Type pbvh_type = bke::pbvh::Type::Mesh;
  /* Evaluated vertex positions for Mesh PBVH; multires limit surface with sculpted
   * displacement for Grids PBVH (see #grids_vert_positions); empty for BMesh PBVH
   * (#vert_position falls back to #BMVert.co). */
  Span<float3> preview_positions;
  /* Backing storage for #preview_positions when it is computed rather than aliased
   * (currently only the Grids PBVH case). Unused otherwise. */
  Array<float3> preview_positions_storage;

  /* The region/strip of faces to extrude or extract. */
  Vector<BMFace *> preview_faces;

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

inline float3 vert_position(const ExtractSharedData &shared, BMVert *v)
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

/** In-mesh extrude state, shared by both tools' Extrude phase. */
struct ExtrudeState {
  ModalPhase phase = ModalPhase::Select;
  float distance = 0.0f;
  /* Center and direction of the extrude axis, in object space. */
  float3 axis_center = float3(0.0f);
  float3 avg_normal = float3(0.0f, 0.0f, 1.0f);
  /* Cursor position when the extrude drag started, in region space. */
  float2 init_mval = float2(0.0f);
  NumInput num_input{};
  bool num_input_initialized = false;

  BMesh *edit_bm = nullptr;
  Vector<BMVert *> verts;
  Vector<float3> base_co;
  Vector<float3> normals;
  Vector<BMFace *> preview_faces;
};

/* --- Modal BMesh construction (_shared.cc). --- */
BMesh *create_modal_bmesh(Object *obact, bke::pbvh::Type pbvh_type);
/* Per base-vertex positions on the multires limit surface with sculpted displacement
 * applied, indexed like #Mesh::vert_positions (Grids PBVH only). */
Array<float3> grids_vert_positions(const Object &obact);

/* --- Face preview drawing (_shared.cc). --- */
/* When \a boundary_only is true the outline is drawn only along the region
 * silhouette (edges used by a single face), instead of outlining every face. */
void draw_faces_preview(const ExtractSharedData &shared,
                        BMesh *draw_bm = nullptr,
                        const Span<BMFace *> faces_override = {},
                        bool boundary_only = false);

/* --- In-mesh extrude engine (_shared.cc). --- */
BMesh *create_edit_bmesh_for_extrude(Mesh *mesh, const Object &ob);
bool extrude_begin(bContext &C,
                   ExtractSharedData &shared,
                   ExtrudeState &ex,
                   Span<BMFace *> region_faces,
                   const float2 &init_mval);
void extrude_apply_distance(ExtractSharedData &shared, ExtrudeState &ex, float distance);
/* Project the cursor onto the extrude axis and apply the resulting distance. */
void extrude_update_from_mouse(ExtractSharedData &shared, ExtrudeState &ex, const float mval[2]);
void extrude_commit(bContext &C, wmOperator *op, ExtractSharedData &shared, ExtrudeState &ex);
void extrude_update_status_text(bContext *C, const ExtrudeState &ex);

/* --- Face region -> new mesh object (_shared.cc). --- */
/* Build a fresh tool-flagged BMesh from \a mesh, with index and element tables ensured so
 * callers can map preview data onto it by index. */
BMesh *create_source_bmesh_for_new_object(const Mesh &mesh);
/* Copy #preview_positions onto the vertices of \a target_bm (defaults to #shared.bm),
 * matched by vertex index. A no-op for BMesh PBVH, where #BMVert.co already holds the
 * live positions. \a target_bm must be built from the same base mesh topology, e.g. by
 * #create_modal_bmesh or #create_edit_bmesh_for_extrude, so indices line up. */
void update_bmesh_positions_from_preview(ExtractSharedData &shared, BMesh *target_bm = nullptr);
Mesh *build_extracted_mesh_from_faces(bContext &C, ExtractSharedData &shared);
void create_mesh_in_new_object(bContext &C, ExtractSharedData &shared);

/* --- Hover session scaffolding (_shared.cc). --- */
void hover_session_store(ExtractSharedData &shared, Object *obact);
bool hover_session_is_valid(const ExtractSharedData &shared, Object *obact);
void hover_refresh_preview_positions(bContext *C, ExtractSharedData &shared, Object *obact);
/* (Re)build the modal BMesh and preview positions for #obact, clearing the previewed faces. */
bool hover_setup_bmesh(bContext *C, ExtractSharedData &shared, Object *obact);

}  // namespace blender::ed::sculpt_paint::extract
