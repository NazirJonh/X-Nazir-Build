/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Neutral, tool-agnostic engine shared by the sculpt extraction gesture tools
 * (Extract Loop, Extract Region): modal BMesh construction, face preview
 * drawing, the in-mesh extrude engine, new-object extraction, and the
 * hover-session scaffolding. All functions operate on #extract::ExtractSharedData
 * / #extract::ExtrudeState and #Span<BMFace *>, with no per-tool selection logic.
 */

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_unit.hh"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_numinput.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "sculpt_extract_shared.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract {

/* -------------------------------------------------------------------- */
/** \name Modal BMesh Construction
 * \{ */

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face Preview Drawing
 * \{ */

void draw_faces_preview(const ExtractSharedData &shared,
                        BMesh *draw_bm,
                        const Span<BMFace *> faces_override,
                        const bool boundary_only)
{
  const Span<BMFace *> faces = faces_override.is_empty() ?
                                   Span<BMFace *>(shared.preview_faces) :
                                   faces_override;
  if (faces.is_empty() || !shared.obact) {
    return;
  }

  auto face_vert_co = [&](BMVert *v) -> float3 {
    if (draw_bm) {
      return float3(v->co);
    }
    return vert_position(shared, v);
  };

  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  GPU_matrix_push();
  GPU_matrix_mul(shared.obact->object_to_world().ptr());

  const GPUDepthTest prev_depth_test = GPU_depth_test_get();
  if (prev_depth_test != GPU_DEPTH_LESS_EQUAL) {
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  }

  /* Semi-transparent fill so the strip reads as surface, not wire. */
  float fill_color[4] = {0.0f, 0.8f, 1.0f, 0.25f};
  immUniformColor4fv(fill_color);
  for (BMFace *face : faces) {
    Vector<float3, 16> face_verts;
    BMLoop *l;
    BMIter l_iter;
    BM_ITER_ELEM (l, &l_iter, face, BM_LOOPS_OF_FACE) {
      face_verts.append(face_vert_co(l->v));
    }
    const int face_verts_num = face_verts.size();
    if (face_verts_num < 3) {
      continue;
    }
    immBegin(GPU_PRIM_TRIS, (face_verts_num - 2) * 3);
    for (int i = 1; i < face_verts_num - 1; i++) {
      immVertex3fv(pos, face_verts[0]);
      immVertex3fv(pos, face_verts[i]);
      immVertex3fv(pos, face_verts[i + 1]);
    }
    immEnd();
  }

  float line_color[4] = {0.0f, 0.8f, 1.0f, 0.9f};
  immUniformColor4fv(line_color);
  GPU_line_width(3.0f);

  if (boundary_only) {
    /* Region silhouette only: draw edges shared by a single previewed face, so
     * the interior grid stays hidden and just the outer border is shown. */
    Map<BMEdge *, int> edge_face_num;
    for (BMFace *face : faces) {
      BMLoop *l;
      BMIter l_iter;
      BM_ITER_ELEM (l, &l_iter, face, BM_LOOPS_OF_FACE) {
        edge_face_num.lookup_or_add(l->e, 0)++;
      }
    }
    Vector<BMEdge *> boundary_edges;
    for (const auto item : edge_face_num.items()) {
      if (item.value == 1) {
        boundary_edges.append(item.key);
      }
    }
    if (!boundary_edges.is_empty()) {
      immBegin(GPU_PRIM_LINES, boundary_edges.size() * 2);
      for (BMEdge *edge : boundary_edges) {
        immVertex3fv(pos, face_vert_co(edge->v1));
        immVertex3fv(pos, face_vert_co(edge->v2));
      }
      immEnd();
    }
  }
  else {
    /* Face polygon loops — outline of each face that will be extracted. */
    for (BMFace *face : faces) {
      Vector<float3, 16> face_verts;
      BMLoop *l;
      BMIter l_iter;
      BM_ITER_ELEM (l, &l_iter, face, BM_LOOPS_OF_FACE) {
        face_verts.append(face_vert_co(l->v));
      }
      if (face_verts.size() < 3) {
        continue;
      }
      immBegin(GPU_PRIM_LINE_LOOP, face_verts.size());
      for (const float3 &p : face_verts) {
        immVertex3fv(pos, p);
      }
      immEnd();
    }
  }

  if (prev_depth_test != GPU_DEPTH_LESS_EQUAL) {
    GPU_depth_test(prev_depth_test);
  }

  immUnbindProgram();
  GPU_matrix_pop();

  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name In-Mesh Extrude Engine
 * \{ */

static int object_active_shapekey_index(const Object &ob)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (UNLIKELY((ob.shapenr == 0) && mesh->key && !BLI_listbase_is_empty(&mesh->key->block))) {
    return 1;
  }
  return ob.shapenr;
}

BMesh *create_edit_bmesh_for_extrude(Mesh *mesh, const Object &ob)
{
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
  BMeshCreateParams bm_create_params{};
  bm_create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &bm_create_params);
  BM_mesh_elem_toolflags_ensure(bm);

  BMeshFromMeshParams mesh_to_bm_params{};
  mesh_to_bm_params.calc_face_normal = true;
  mesh_to_bm_params.use_shapekey = true;
  mesh_to_bm_params.active_shapekey = object_active_shapekey_index(ob);
  mesh_to_bm_params.add_key_index = true;
  BM_mesh_bm_from_me(bm, mesh, &mesh_to_bm_params);

  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
  return bm;
}

/**
 * Tag the faces in \a bm that correspond by index to \a modal_faces. The face
 * pointers in \a modal_faces belong to the modal BMesh; \a bm is a fresh BMesh
 * built from the same base mesh, so element indices match.
 */
static void tag_faces_on_bmesh(BMesh *bm, const Span<BMFace *> modal_faces)
{
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  for (BMFace *mf : modal_faces) {
    BMFace *bf = BM_face_at_index(bm, BM_elem_index_get(mf));
    BM_elem_flag_set(bf, BM_ELEM_TAG, true);
  }
}

void extrude_apply_distance(ExtractSharedData & /*shared*/, ExtrudeState &ex, const float distance)
{
  ex.distance = distance;
  for (const int i : ex.verts.index_range()) {
    BMVert *vert = ex.verts[i];
    const float3 co = ex.base_co[i] + ex.normals[i] * distance;
    copy_v3_v3(vert->co, co);
  }
}

void extrude_update_from_mouse(ExtractSharedData &shared, ExtrudeState &ex, const float mval[2])
{
  ARegion *region = shared.region;
  RegionView3D *rv3d = shared.rv3d;
  Object *ob = shared.obact;
  if (!region || !rv3d || !ob) {
    return;
  }

  /* Extrude axis origin and direction in world space (object transform applied to the
   * local strip center and average normal). This mirrors the projection the former
   * extrude gizmo performed, so the drag feel is unchanged. */
  const float4x4 &object_to_world = ob->object_to_world();
  float arrow_co[3];
  copy_v3_v3(arrow_co, math::transform_point(object_to_world, ex.axis_center));
  float arrow_no_world[3];
  copy_v3_v3(arrow_no_world,
             math::normalize(math::transform_direction(object_to_world, ex.avg_normal)));

  struct {
    float2 mval;
    float ray_origin[3], ray_direction[3];
    float location[3];
  } proj[2] = {};
  proj[0].mval = ex.init_mval;
  proj[1].mval = float2(mval[0], mval[1]);

  float arrow_no[3];
  copy_v3_v3(arrow_no, arrow_no_world);

  int ok = 0;
  for (int j = 0; j < 2; j++) {
    ED_view3d_win_to_ray(region, proj[j].mval, proj[j].ray_origin, proj[j].ray_direction);
    /* Force the view's up axis when the cursor ray is nearly parallel to the extrude axis,
     * otherwise the ray-plane intersection becomes unstable. */
    if (j == 0) {
      if (RAD2DEGF(acosf(dot_v3v3(proj[j].ray_direction, arrow_no_world))) < 5.0f) {
        normalize_v3_v3(arrow_no, rv3d->viewinv[1]);
      }
    }

    float arrow_no_proj[3];
    project_plane_v3_v3v3(arrow_no_proj, arrow_no, proj[j].ray_direction);
    normalize_v3(arrow_no_proj);

    float lambda;
    if (isect_ray_plane_v3_factor(arrow_co, arrow_no, proj[j].ray_origin, arrow_no_proj, &lambda)) {
      madd_v3_v3v3fl(proj[j].location, arrow_co, arrow_no, lambda);
      ok++;
    }
  }

  if (ok != 2) {
    return;
  }

  float offset[3];
  sub_v3_v3v3(offset, proj[1].location, proj[0].location);
  const float facdir = dot_v3v3(arrow_no, offset) < 0.0f ? -1.0f : 1.0f;
  extrude_apply_distance(shared, ex, facdir * len_v3(offset));
}

void extrude_update_status_text(bContext *C, const ExtrudeState &ex)
{
  const Scene *scene = CTX_data_scene(C);
  char distance_str[NUM_STR_REP_LEN];
  if (hasNumInput(&ex.num_input)) {
    outputNumInput(const_cast<NumInput *>(&ex.num_input), distance_str, scene->unit);
  }
  else {
    BKE_unit_value_as_string_scaled(distance_str,
                                    sizeof(distance_str),
                                    ex.distance,
                                    4 * -1,
                                    B_UNIT_LENGTH,
                                    scene->unit,
                                    true);
  }

  char status_str[NUM_STR_REP_LEN + 32];
  BLI_snprintf(status_str, sizeof(status_str), "%s: %s", IFACE_("Extrude"), distance_str);
  ED_workspace_status_text(C, status_str);
}

bool extrude_begin(bContext &C,
                   ExtractSharedData &shared,
                   ExtrudeState &ex,
                   const Span<BMFace *> region_faces,
                   const float2 &init_mval)
{
  if (region_faces.is_empty()) {
    return false;
  }

  if (shared.pbvh_type == bke::pbvh::Type::BMesh) {
    BKE_report(CTX_wm_reports(&C),
               RPT_ERROR,
               "Extrude requires dynamic topology to be disabled");
    return false;
  }

  Object &ob = *shared.obact;
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  ex.edit_bm = create_edit_bmesh_for_extrude(mesh, ob);
  if (!ex.edit_bm) {
    return false;
  }

  tag_faces_on_bmesh(ex.edit_bm, region_faces);

  BMOperator bmo_op;
  BMO_op_init(
      ex.edit_bm, &bmo_op, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE), "extrude_face_region");
  BMO_slot_buffer_from_enabled_hflag(
      ex.edit_bm, &bmo_op, bmo_op.slots_in, "geom", BM_FACE, BM_ELEM_TAG);
  BMO_op_exec(ex.edit_bm, &bmo_op);

  ex.verts.clear();
  ex.base_co.clear();
  ex.normals.clear();
  ex.preview_faces.clear();

  BMOIter oiter;
  BMVert *vert;
  BMO_ITER (vert, &oiter, bmo_op.slots_out, "geom.out", BM_VERT) {
    ex.verts.append(vert);
    ex.base_co.append(float3(vert->co));
    ex.normals.append(float3(vert->no));
  }

  BMFace *face;
  BMO_ITER (face, &oiter, bmo_op.slots_out, "geom.out", BM_FACE) {
    ex.preview_faces.append(face);
  }

  BMO_op_finish(ex.edit_bm, &bmo_op);

  if (ex.verts.is_empty()) {
    BM_mesh_free(ex.edit_bm);
    ex.edit_bm = nullptr;
    BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Could not extrude region");
    return false;
  }

  BM_mesh_normals_update(ex.edit_bm);
  for (const int i : ex.verts.index_range()) {
    ex.normals[i] = float3(ex.verts[i]->no);
  }

  float3 avg_normal(0.0f);
  for (const float3 &no : ex.normals) {
    avg_normal += no;
  }
  if (normalize_v3(avg_normal) == 0.0f) {
    avg_normal = float3(0.0f, 0.0f, 1.0f);
  }
  ex.avg_normal = float3(avg_normal);

  float3 center(0.0f);
  for (const float3 &co : ex.base_co) {
    center += co;
  }
  center /= float(ex.base_co.size());
  ex.axis_center = center;

  if (!ex.num_input_initialized) {
    initNumInput(&ex.num_input);
    ex.num_input.idx_max = 0;
    ex.num_input.unit_type[0] = B_UNIT_LENGTH;
    ex.num_input_initialized = true;
  }

  ex.phase = ModalPhase::Extrude;
  ex.init_mval = init_mval;
  ex.distance = 0.0f;
  extrude_apply_distance(shared, ex, 0.0f);
  extrude_update_status_text(&C, ex);
  return true;
}

void extrude_commit(bContext &C, wmOperator *op, ExtractSharedData &shared, ExtrudeState &ex)
{
  Scene &scene = *CTX_data_scene(&C);
  Object &ob = *shared.obact;
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  if (!ex.edit_bm) {
    return;
  }

  undo::geometry_begin(scene, ob, op);

  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  BM_mesh_bm_to_me(nullptr, ex.edit_bm, mesh, &bm_to_mesh_params);
  BM_mesh_free(ex.edit_bm);
  ex.edit_bm = nullptr;

  undo::geometry_end(ob);
  BKE_sculptsession_free_pbvh(ob);

  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, mesh);

  Main *bmain = CTX_data_main(&C);
  BKE_scene_graph_update_tagged(depsgraph, bmain);

  BKE_report(CTX_wm_reports(&C), RPT_INFO, "Region extruded");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Object Extraction
 * \{ */

static BMesh *create_source_bmesh_for_new_object(const Mesh &mesh)
{
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(&mesh);
  BMeshCreateParams bm_create_params{};
  bm_create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &bm_create_params);

  BMeshFromMeshParams from_mesh_params{};
  from_mesh_params.calc_face_normal = true;
  from_mesh_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(bm, &mesh, &from_mesh_params);

  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
  return bm;
}

static void update_bmesh_positions_from_preview(ExtractSharedData &shared)
{
  if (shared.pbvh_type == bke::pbvh::Type::BMesh || shared.preview_positions.is_empty()) {
    return;
  }

  BMVert *v;
  BMIter iter;
  BM_ITER_MESH (v, &iter, shared.bm, BM_VERTS_OF_MESH) {
    const int idx = BM_elem_index_get(v);
    if (idx >= 0 && idx < shared.preview_positions.size()) {
      copy_v3_v3(v->co, shared.preview_positions[idx]);
    }
  }
}

Mesh *build_extracted_mesh_from_faces(bContext &C, ExtractSharedData &shared)
{
  if (shared.preview_faces.is_empty()) {
    return nullptr;
  }

  Object *obact = shared.obact;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
  BKE_sculpt_update_object_for_edit(depsgraph, obact, false);

  Mesh *src_mesh = id_cast<Mesh *>(obact->data);

  BMesh *bm = create_source_bmesh_for_new_object(*src_mesh);

  /* Tag the previewed faces on the fresh source BMesh by index, then delete
   * everything else so only the region remains (FaceStrip isolation branch). */
  tag_faces_on_bmesh(bm, shared.preview_faces);
  BMFace *f;
  BMIter fiter;
  BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
    BM_elem_flag_toggle(f, BM_ELEM_TAG);
  }
  BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_FACES);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  /* #update_bmesh_positions_from_preview iterates #shared.bm, so point a copy at the
   * source BMesh whose surviving vertices keep their original (matching) indices. */
  ExtractSharedData pos_ctx = shared;
  pos_ctx.bm = bm;
  update_bmesh_positions_from_preview(pos_ctx);
  BM_mesh_normals_update(bm);

  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  Mesh *new_mesh = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, src_mesh);
  BM_mesh_free(bm);

  return new_mesh;
}

void create_mesh_in_new_object(bContext &C, ExtractSharedData &shared)
{
  Mesh *new_mesh = build_extracted_mesh_from_faces(C, shared);
  if (!new_mesh) {
    return;
  }
  if (new_mesh->verts_num == 0) {
    BKE_id_free(nullptr, new_mesh);
    return;
  }

  Main *bmain = CTX_data_main(&C);
  View3D *v3d = CTX_wm_view3d(&C);
  Object *obact = shared.obact;

  ushort local_view_bits = 0;
  if (v3d && v3d->localvd) {
    local_view_bits = v3d->local_view_uid;
  }
  Object *new_ob = ed::object::add_type(
      &C, OB_MESH, nullptr, obact->loc, obact->rot, false, local_view_bits);
  BKE_mesh_nomain_to_mesh(new_mesh, id_cast<Mesh *>(new_ob->data), new_ob);

  BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(new_ob->data), BKE_MESH_BATCH_DIRTY_ALL);
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, new_ob->data);
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, new_ob);

  BKE_report(CTX_wm_reports(&C), RPT_INFO, "Region extracted to new mesh object");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hover Session Scaffolding
 * \{ */

void hover_session_store(ExtractSharedData &shared, Object *obact)
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

bool hover_session_is_valid(const ExtractSharedData &shared, Object *obact)
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

void hover_refresh_preview_positions(bContext *C, ExtractSharedData &shared, Object *obact)
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

bool hover_setup_bmesh(bContext *C, ExtractSharedData &shared, Object *obact)
{
  if (shared.bm) {
    BM_mesh_free(shared.bm);
    shared.bm = nullptr;
  }

  shared.preview_faces.clear();
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

  hover_refresh_preview_positions(C, shared, obact);
  hover_session_store(shared, obact);
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::extract
