/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Idle-hover state for the Extract Loop tool: the #g_hover_state singleton, the
 * modal BMesh construction, session validity tracking, and the hover API
 * consumed by the paint cursor.
 */

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "DNA_mesh_types.h"

#include "DEG_depsgraph.hh"

#include "ED_view3d.hh"

#include "bmesh.hh"

#include "sculpt_extract_loop_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_loop {

/* -------------------------------------------------------------------- */
/** \name Hover State
 * \{ */

struct HoverState {
  ExtractLoopSharedData shared;
  bool is_initialized = false;
  bool is_activated = false;
};

static HoverState &g_hover_state_get()
{
  static HoverState state;
  return state;
}

/** \} */

/**
 * Build the modal-persistent BMesh from the active object.
 *
 * - Mesh & Grids PBVH: build from the base (cage) mesh. The vertex/edge/face
 *   order matches #Mesh::vert_positions / #Mesh::edges / #Mesh::faces, so a
 *   face or edge index obtained from the active-element cursor info can be
 *   mapped directly to a #BMVert / #BMEdge via #BM_vert_at_index /
 *   #BM_edge_at_index.
 * - BMesh PBVH (dynamic topology): #SculptSession::bm carries the live
 *   dynamic-topology geometry. We round-trip it through a temporary #Mesh
 *   (#BKE_mesh_from_bmesh_nomain then #BM_mesh_bm_from_me) to get an
 *   independent copy whose element order matches the original, so a vertex
 *   index from #SculptSession::active_vert maps to the same vertex in our
 *   copy. Modifying this copy does not affect the sculpt session.
 */
BMesh *create_modal_bmesh(Object *obact, bke::pbvh::Type pbvh_type)
{
  BMeshCreateParams bm_create_params{};
  bm_create_params.use_toolflags = true;
  BMesh *bm = nullptr;

  if (pbvh_type == bke::pbvh::Type::BMesh) {
    SculptSession *ss = obact->runtime->sculpt_session;
    if (!ss || !ss->bm) {
      return nullptr;
    }
    BMeshToMeshParams to_mesh_params{};
    to_mesh_params.calc_object_remap = false;
    Mesh *tmp_mesh = BKE_mesh_from_bmesh_nomain(
        ss->bm, &to_mesh_params, id_cast<Mesh *>(obact->data));
    const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(tmp_mesh);
    bm = BM_mesh_create(&allocsize, &bm_create_params);
    BMeshFromMeshParams from_mesh_params{};
    from_mesh_params.calc_face_normal = true;
    BM_mesh_bm_from_me(bm, tmp_mesh, &from_mesh_params);
    BKE_id_free(nullptr, tmp_mesh);
  }
  else {
    /* Mesh and Grids PBVH both use the base cage mesh as their topology
     * source. For Grids the cage is the low-resolution mesh the user edits;
     * the subdivision surface is only a display/deform layer, and extraction
     * must yield cage topology. */
    Mesh *mesh = id_cast<Mesh *>(obact->data);
    const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
    bm = BM_mesh_create(&allocsize, &bm_create_params);
    BMeshFromMeshParams from_mesh_params{};
    from_mesh_params.calc_face_normal = true;
    BM_mesh_bm_from_me(bm, mesh, &from_mesh_params);
  }

  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
  return bm;
}

static void extract_loop_hover_session_store(ExtractLoopSharedData &shared, Object *obact)
{
  shared.session_mesh = BKE_object_get_original_mesh(obact);
  if (shared.session_mesh) {
    shared.session_totvert = shared.session_mesh->verts_num;
    shared.session_totedge = shared.session_mesh->edges_num;
    shared.session_totface = shared.session_mesh->faces_num;
  }
  else {
    shared.session_totvert = 0;
    shared.session_totedge = 0;
    shared.session_totface = 0;
  }
}

static bool extract_loop_hover_session_is_valid(const ExtractLoopSharedData &shared, Object *obact)
{
  if (!shared.bm || !obact || shared.obact != obact) {
    return false;
  }

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*obact);
  if (!pbvh || pbvh->type() != shared.pbvh_type) {
    return false;
  }

  if (shared.pbvh_type == bke::pbvh::Type::BMesh) {
    const SculptSession *ss = obact->runtime->sculpt_session;
    if (!ss || !ss->bm) {
      return false;
    }
    return shared.bm->totvert == ss->bm->totvert && shared.bm->totedge == ss->bm->totedge &&
           shared.bm->totface == ss->bm->totface;
  }

  if (!shared.session_mesh) {
    return false;
  }
  const Mesh *mesh = BKE_object_get_original_mesh(obact);
  return mesh == shared.session_mesh && mesh->verts_num == shared.session_totvert &&
         mesh->edges_num == shared.session_totedge && mesh->faces_num == shared.session_totface;
}

static void extract_loop_hover_refresh_preview_positions(bContext *C,
                                                         ExtractLoopSharedData &shared,
                                                         Object *obact)
{
  if (shared.pbvh_type == bke::pbvh::Type::Mesh) {
    const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    if (depsgraph) {
      shared.preview_positions = bke::pbvh::vert_positions_eval(*depsgraph, *obact);
    }
  }
  else if (shared.pbvh_type == bke::pbvh::Type::Grids) {
    Mesh *mesh = BKE_object_get_original_mesh(obact);
    if (mesh) {
      shared.preview_positions = mesh->vert_positions();
    }
  }
}

/**
 * (Re)build the modal BMesh and preview positions for #obact.
 * Clears any previous walker/preview results.
 */
static bool extract_loop_hover_setup_bmesh(bContext *C, ExtractLoopSharedData &shared, Object *obact)
{
  if (shared.bm) {
    BM_mesh_free(shared.bm);
    shared.bm = nullptr;
  }

  shared.preview_faces.clear();
  shared.preview_points.clear();
  shared.loop_edges.clear();
  shared.seed_edge = nullptr;
  shared.is_cyclic = false;

  shared.obact = obact;

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*obact);
  if (!pbvh) {
    return false;
  }
  shared.pbvh_type = pbvh->type();

  shared.bm = create_modal_bmesh(obact, shared.pbvh_type);
  if (!shared.bm) {
    return false;
  }

  extract_loop_hover_refresh_preview_positions(C, shared, obact);
  extract_loop_hover_session_store(shared, obact);
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Hover API
 * \{ */

void extract_loop_hover_init(bContext *C,
                             Object *ob,
                             ARegion *region,
                             RegionView3D *rv3d,
                             ExtractionMode mode,
                             LoopOrientation loop_orientation)
{
  HoverState &state = g_hover_state_get();

  if (state.shared.bm) {
    BM_mesh_free(state.shared.bm);
    state = HoverState{};
  }

  state.shared.region = region;
  state.shared.rv3d = rv3d;
  state.shared.mode = mode;
  state.shared.loop_orientation = loop_orientation;

  if (!extract_loop_hover_setup_bmesh(C, state.shared, ob)) {
    return;
  }

  state.is_initialized = true;
}

void extract_loop_hover_free()
{
  HoverState &state = g_hover_state_get();
  if (state.shared.bm) {
    BM_mesh_free(state.shared.bm);
    state.shared.bm = nullptr;
  }
  state.shared.preview_faces.clear();
  state.shared.preview_points.clear();
  state.shared.loop_edges.clear();
  state.shared.seed_edge = nullptr;
  state.shared.obact = nullptr;
  state.shared.session_mesh = nullptr;
  state.is_initialized = false;
  state.is_activated = false;
}

void extract_loop_hover_update(bContext *C,
                               const float2 &mval,
                               ExtractionMode mode,
                               LoopOrientation loop_orientation)
{
  HoverState &state = g_hover_state_get();
  if (!state.is_initialized) {
    return;
  }

  Object *obact = CTX_data_active_object(C);
  if (!obact) {
    extract_loop_hover_free();
    return;
  }

  state.shared.mode = mode;
  state.shared.loop_orientation = loop_orientation;
  state.shared.region = CTX_wm_region(C);
  state.shared.rv3d = CTX_wm_region_view3d(C);

  if (!extract_loop_hover_session_is_valid(state.shared, obact)) {
    if (!extract_loop_hover_setup_bmesh(C, state.shared, obact)) {
      extract_loop_hover_free();
      return;
    }
  }
  else {
    extract_loop_hover_refresh_preview_positions(C, state.shared, obact);
  }

  if (!state.shared.bm) {
    extract_loop_hover_free();
    return;
  }

  state.shared.preview_points.clear();
  state.shared.preview_faces.clear();
  state.shared.loop_edges.clear();
  state.shared.seed_edge = nullptr;
  state.shared.is_cyclic = false;

  CursorGeometryInfo cgi;
  if (!cursor_geometry_info_update(C, &cgi, mval, false)) {
    return;
  }
  state.shared.hit_location = cgi.location;

  state.shared.seed_edge = find_seed_edge_screen_space(state.shared, mval);
  if (!state.shared.seed_edge) {
    return;
  }

  bool dummy = false;
  run_walker(state.shared, true /* hover always follows boundary */, &dummy);
}

void extract_loop_hover_draw()
{
  HoverState &state = g_hover_state_get();
  if (!state.is_initialized || !state.shared.bm || !state.shared.obact) {
    return;
  }
  if (!extract_loop_hover_session_is_valid(state.shared, state.shared.obact)) {
    return;
  }
  draw_loop_preview(state.shared);
}

bool extract_loop_hover_is_enabled()
{
  return g_hover_state_get().is_initialized;
}

void extract_loop_hover_activate()
{
  g_hover_state_get().is_activated = true;
}

void extract_loop_hover_deactivate()
{
  g_hover_state_get().is_activated = false;
  if (extract_loop_hover_is_enabled()) {
    extract_loop_hover_free();
  }
}

bool extract_loop_hover_is_activated()
{
  return g_hover_state_get().is_activated;
}

/** \} */

/**
 * When idle hover already resolved the loop under the cursor, reuse its seed edge
 * in the modal BMesh (element indices match because both are built from the same mesh).
 */
void sync_preview_from_hover(ExtractLoopModalData &data)
{
  const HoverState &hover = g_hover_state_get();
  const bool hover_has_preview = hover.shared.mode == ExtractionMode::FaceStrip ?
                                     !hover.shared.preview_faces.is_empty() :
                                     !hover.shared.loop_edges.is_empty();
  if (!hover.is_initialized || !hover_has_preview || !hover.shared.bm || !data.shared.bm)
  {
    return;
  }
  if (hover.shared.obact != data.shared.obact || !hover.shared.seed_edge) {
    return;
  }

  data.shared.hit_location = hover.shared.hit_location;
  data.shared.loop_orientation = hover.shared.loop_orientation;
  data.initial_hit = true;
  data.shared.seed_edge = BM_edge_at_index(data.shared.bm,
                                            BM_elem_index_get(hover.shared.seed_edge));
  run_walker(data.shared, true, &data.has_boundary_seed);
  if (data.shared.mode != ExtractionMode::FaceStrip) {
    for (BMEdge *e : data.shared.loop_edges) {
      data.loop_edges_set.add(e);
    }
  }
}

}  // namespace blender::ed::sculpt_paint::extract_loop
