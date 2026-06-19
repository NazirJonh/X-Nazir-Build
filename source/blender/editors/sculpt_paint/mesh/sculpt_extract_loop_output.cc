/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Output paths for the Extract Loop tool: in-mesh duplication, face-strip
 * extrude (begin/commit), extraction to a new mesh or curves object, and the
 * #finish_extract dispatch over the selected output type.
 */

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_screen.hh"
#include "BKE_unit.hh"

#include "DNA_curves_types.h"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "ED_numinput.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "sculpt_extract_loop_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::face_set {
int find_next_available_id(Object &object);
void initialize_none_to_id(Mesh *mesh, int new_id);
}  // namespace blender::ed::sculpt_paint::face_set

namespace blender::ed::sculpt_paint::extract_loop {

bool is_face_strip_extrude_output(const wmOperator *op, const ExtractionMode mode)
{
  if (mode != ExtractionMode::FaceStrip) {
    return false;
  }
  const ExtractionOutputType output_type = ExtractionOutputType(
      RNA_enum_get(op->ptr, "output_type"));
  return output_type == ExtractionOutputType::Extrude;
}

/* Re-run the walker on a fresh BMesh so the tagged element pointers belong to the
 * mesh that will be written back. The seed is matched by index because all these
 * BMeshes are built from the same base mesh in the same element order. */
static ExtractLoopSharedData rewalk_on_bmesh(const ExtractLoopSharedData &src,
                                             BMesh *dst_bm,
                                             const bool use_boundary_walker)
{
  ExtractLoopSharedData work = src;
  work.bm = dst_bm;
  work.preview_faces.clear();
  work.loop_edges.clear();
  work.preview_points.clear();
  work.seed_edge = BM_edge_at_index(dst_bm, BM_elem_index_get(src.seed_edge));
  run_walker(work, use_boundary_walker, nullptr);
  return work;
}

/* -------------------------------------------------------------------- */
/** \name In-Mesh Duplication
 * \{ */

static int object_active_shapekey_index(const Object &ob)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (UNLIKELY((ob.shapenr == 0) && mesh->key && !BLI_listbase_is_empty(&mesh->key->block))) {
    return 1;
  }
  return ob.shapenr;
}

/**
 * Tag walked geometry in \a bm using element pointers from \a shared (same BMesh).
 */
static bool tag_walked_geometry(BMesh *bm, const ExtractLoopSharedData &shared)
{
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  if (shared.mode == ExtractionMode::FaceStrip) {
    if (shared.preview_faces.is_empty()) {
      return false;
    }
    for (BMFace *face : shared.preview_faces) {
      BM_elem_flag_set(face, BM_ELEM_TAG, true);
    }
    return true;
  }

  if (shared.loop_edges.is_empty()) {
    return false;
  }
  for (BMEdge *edge : shared.loop_edges) {
    BM_elem_flag_set(edge, BM_ELEM_TAG, true);
  }
  return true;
}

/**
 * Expand \a seeds to all vertices in their face-connected islands.
 */
static void collect_verts_in_face_islands(const Span<BMVert *> seeds, Set<BMVert *> &r_verts)
{
  for (BMVert *vert : seeds) {
    r_verts.add(vert);
  }

  Vector<BMFace *> face_stack;
  for (BMVert *vert : seeds) {
    BMIter iter;
    BMFace *face;
    BM_ITER_ELEM (face, &iter, vert, BM_FACES_OF_VERT) {
      face_stack.append(face);
    }
  }

  Set<BMFace *> visited_faces;
  while (!face_stack.is_empty()) {
    BMFace *face = face_stack.pop_last();
    if (visited_faces.contains(face)) {
      continue;
    }
    visited_faces.add(face);

    BMIter iter;
    BMVert *face_vert;
    BM_ITER_ELEM (face_vert, &iter, face, BM_VERTS_OF_FACE) {
      if (r_verts.add(face_vert)) {
        BMIter face_iter;
        BMFace *other_face;
        BM_ITER_ELEM (other_face, &face_iter, face_vert, BM_FACES_OF_VERT) {
          face_stack.append(other_face);
        }
      }
    }
  }
}

/**
 * Mask the whole mesh and unmask the duplicated geometry so it is ready to sculpt or transform.
 */
static void apply_mask_selection_to_duplicated(BMesh *bm,
                                             const Span<BMVert *> duplicate_verts,
                                             const ExtractionMode mode)
{
  if (duplicate_verts.is_empty()) {
    return;
  }

  BM_data_layer_ensure_named(bm, &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  const int cd_vert_mask_offset = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  if (cd_vert_mask_offset == -1) {
    return;
  }

  Set<BMVert *> piece_verts;
  if (mode == ExtractionMode::FaceStrip) {
    collect_verts_in_face_islands(duplicate_verts, piece_verts);
  }
  else {
    for (BMVert *vert : duplicate_verts) {
      piece_verts.add(vert);
    }
  }

  BMVert *v;
  BMIter iter;
  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    if (!BM_elem_flag_test_bool(v, BM_ELEM_HIDDEN)) {
      BM_ELEM_CD_SET_FLOAT(v, cd_vert_mask_offset, 1.0f);
    }
  }
  for (BMVert *piece_vert : piece_verts) {
    if (!BM_elem_flag_test_bool(piece_vert, BM_ELEM_HIDDEN)) {
      BM_ELEM_CD_SET_FLOAT(piece_vert, cd_vert_mask_offset, 0.0f);
    }
  }
}

static BMesh *create_edit_bmesh_for_duplicate(Mesh *mesh, const Object &ob)
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
 * Duplicate the previewed loop or face strip inside the active mesh using #BMO_duplicate,
 * keeping the original geometry (same approach as mask duplicate).
 */
static void duplicate_geometry_in_object(bContext &C, wmOperator *op, ExtractLoopModalData &data)
{
  Scene &scene = *CTX_data_scene(&C);
  Object &ob = *data.shared.obact;
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  if (data.shared.pbvh_type == bke::pbvh::Type::BMesh) {
    BKE_report(CTX_wm_reports(&C),
               RPT_ERROR,
               "Duplicate inside mesh requires dynamic topology to be disabled");
    return;
  }

  if (!extract_preview_is_valid(data.shared) || !data.shared.seed_edge) {
    return;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);
  undo::geometry_begin(scene, ob, op);

  BMesh *bm = create_edit_bmesh_for_duplicate(mesh, ob);
  if (!bm) {
    undo::geometry_end(ob);
    return;
  }

  /* Re-walk on the edit BMesh so tagged pointers belong to the mesh we write back. */
  ExtractLoopSharedData work = rewalk_on_bmesh(data.shared, bm, data.use_boundary_walker);

  if (!tag_walked_geometry(bm, work)) {
    BM_mesh_free(bm);
    undo::geometry_end(ob);
    return;
  }

  const char geom_type = mode_geom_htype(work.mode);
  const bool use_mask_selection = RNA_boolean_get(op->ptr, "mask_selection");

  BMOperator bmo_op;
  BMO_op_init(bm, &bmo_op, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE), "duplicate");
  BMO_slot_buffer_from_enabled_hflag(bm, &bmo_op, bmo_op.slots_in, "geom", geom_type, BM_ELEM_TAG);
  BMO_op_exec(bm, &bmo_op);

  Vector<BMVert *> duplicate_verts;
  if (use_mask_selection) {
    BMOIter oiter;
    BMVert *v;
    BMO_ITER (v, &oiter, bmo_op.slots_out, "geom.out", BM_VERT) {
      duplicate_verts.append(v);
    }
  }

  BMO_op_finish(bm, &bmo_op);

  if (use_mask_selection) {
    apply_mask_selection_to_duplicated(bm, duplicate_verts, work.mode);
  }

  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  BM_mesh_bm_to_me(nullptr, bm, mesh, &bm_to_mesh_params);
  BM_mesh_free(bm);

  if (mesh->attributes().contains(".sculpt_face_set")) {
    const int next_face_set_id = face_set::find_next_available_id(ob);
    face_set::initialize_none_to_id(mesh, next_face_set_id);
  }

  undo::geometry_end(ob);
  BKE_sculptsession_free_pbvh(ob);

  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, mesh);

  Main *bmain = CTX_data_main(&C);
  BKE_scene_graph_update_tagged(depsgraph, bmain);

  if (work.mode == ExtractionMode::FaceStrip) {
    BKE_report(CTX_wm_reports(&C), RPT_INFO, "Face strip duplicated inside mesh");
  }
  else {
    BKE_report(CTX_wm_reports(&C), RPT_INFO, "Loop duplicated inside mesh");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face Strip Extrude (In-Mesh)
 * \{ */

void extrude_apply_distance(ExtractLoopModalData &data, const float distance)
{
  data.extrude_distance = distance;
  for (const int i : data.extrude_verts.index_range()) {
    BMVert *vert = data.extrude_verts[i];
    const float3 co = data.extrude_base_co[i] + data.extrude_normals[i] * distance;
    copy_v3_v3(vert->co, co);
  }
}

void extrude_update_from_mouse(ExtractLoopModalData &data, const float mval[2])
{
  ARegion *region = data.shared.region;
  RegionView3D *rv3d = data.shared.rv3d;
  Object *ob = data.shared.obact;
  if (!region || !rv3d || !ob) {
    return;
  }

  /* Extrude axis origin and direction in world space (object transform applied to the
   * local strip center and average normal). This mirrors the projection the former
   * extrude gizmo performed, so the drag feel is unchanged. */
  const float4x4 &object_to_world = ob->object_to_world();
  float arrow_co[3];
  copy_v3_v3(arrow_co, math::transform_point(object_to_world, data.extrude_axis_center));
  float arrow_no_world[3];
  copy_v3_v3(arrow_no_world,
             math::normalize(math::transform_direction(object_to_world, data.extrude_avg_normal)));

  struct {
    float2 mval;
    float ray_origin[3], ray_direction[3];
    float location[3];
  } proj[2] = {};
  proj[0].mval = data.extrude_init_mval;
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
  extrude_apply_distance(data, facdir * len_v3(offset));
}

void extrude_update_status_text(bContext *C, const ExtractLoopModalData &data)
{
  const Scene *scene = CTX_data_scene(C);
  char distance_str[NUM_STR_REP_LEN];
  if (hasNumInput(&data.num_input)) {
    outputNumInput(const_cast<NumInput *>(&data.num_input), distance_str, scene->unit);
  }
  else {
    BKE_unit_value_as_string_scaled(distance_str,
                                    sizeof(distance_str),
                                    data.extrude_distance,
                                    4 * -1,
                                    B_UNIT_LENGTH,
                                    scene->unit,
                                    true);
  }

  char status_str[NUM_STR_REP_LEN + 32];
  BLI_snprintf(status_str, sizeof(status_str), "%s: %s", IFACE_("Extrude"), distance_str);
  ED_workspace_status_text(C, status_str);
}

bool extrude_begin(bContext &C, wmOperator *op, ExtractLoopModalData &data)
{
  Object &ob = *data.shared.obact;
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  if (data.shared.pbvh_type == bke::pbvh::Type::BMesh) {
    BKE_report(CTX_wm_reports(&C),
               RPT_ERROR,
               "Face strip extrude requires dynamic topology to be disabled");
    return false;
  }

  if (!extract_preview_is_valid(data.shared) || !data.shared.seed_edge) {
    return false;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  data.edit_bm = create_edit_bmesh_for_duplicate(mesh, ob);
  if (!data.edit_bm) {
    return false;
  }

  ExtractLoopSharedData work = rewalk_on_bmesh(data.shared, data.edit_bm, data.use_boundary_walker);

  if (!tag_walked_geometry(data.edit_bm, work)) {
    BM_mesh_free(data.edit_bm);
    data.edit_bm = nullptr;
    return false;
  }

  BMOperator bmo_op;
  BMO_op_init(data.edit_bm, &bmo_op, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE), "extrude_face_region");
  BMO_slot_buffer_from_enabled_hflag(
      data.edit_bm, &bmo_op, bmo_op.slots_in, "geom", BM_FACE, BM_ELEM_TAG);
  BMO_op_exec(data.edit_bm, &bmo_op);

  data.extrude_verts.clear();
  data.extrude_base_co.clear();
  data.extrude_normals.clear();
  data.extrude_preview_faces.clear();

  BMOIter oiter;
  BMVert *vert;
  BMO_ITER (vert, &oiter, bmo_op.slots_out, "geom.out", BM_VERT) {
    data.extrude_verts.append(vert);
    data.extrude_base_co.append(float3(vert->co));
    data.extrude_normals.append(float3(vert->no));
  }

  BMFace *face;
  BMO_ITER (face, &oiter, bmo_op.slots_out, "geom.out", BM_FACE) {
    data.extrude_preview_faces.append(face);
  }

  BMO_op_finish(data.edit_bm, &bmo_op);

  if (data.extrude_verts.is_empty()) {
    BM_mesh_free(data.edit_bm);
    data.edit_bm = nullptr;
    BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Could not extrude face strip");
    return false;
  }

  BM_mesh_normals_update(data.edit_bm);
  for (const int i : data.extrude_verts.index_range()) {
    data.extrude_normals[i] = float3(data.extrude_verts[i]->no);
  }

  float3 avg_normal(0.0f);
  for (const float3 &no : data.extrude_normals) {
    avg_normal += no;
  }
  if (normalize_v3(avg_normal) == 0.0f) {
    avg_normal = float3(0.0f, 0.0f, 1.0f);
  }
  data.extrude_avg_normal = float3(avg_normal);

  float3 center(0.0f);
  for (const float3 &co : data.extrude_base_co) {
    center += co;
  }
  center /= float(data.extrude_base_co.size());
  data.extrude_axis_center = center;

  if (!data.num_input_initialized) {
    initNumInput(&data.num_input);
    data.num_input.idx_max = 0;
    data.num_input.unit_type[0] = B_UNIT_LENGTH;
    data.num_input_initialized = true;
  }

  data.phase = ModalPhase::Extrude;
  data.extrude_distance = 0.0f;
  extrude_apply_distance(data, 0.0f);
  extrude_update_status_text(&C, data);
  return true;
}

void extrude_commit(bContext &C, wmOperator *op, ExtractLoopModalData &data)
{
  Scene &scene = *CTX_data_scene(&C);
  Object &ob = *data.shared.obact;
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);

  if (!data.edit_bm) {
    return;
  }

  undo::geometry_begin(scene, ob, op);

  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  BM_mesh_bm_to_me(nullptr, data.edit_bm, mesh, &bm_to_mesh_params);
  BM_mesh_free(data.edit_bm);
  data.edit_bm = nullptr;

  undo::geometry_end(ob);
  BKE_sculptsession_free_pbvh(ob);

  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, mesh);

  Main *bmain = CTX_data_main(&C);
  BKE_scene_graph_update_tagged(depsgraph, bmain);

  BKE_report(CTX_wm_reports(&C), RPT_INFO, "Face strip extruded");
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

static void update_bmesh_positions_from_preview(ExtractLoopSharedData &shared)
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

static void isolate_extraction_geometry_in_bmesh(ExtractLoopSharedData &shared)
{
  BMesh *bm = shared.bm;
  if (!tag_walked_geometry(bm, shared)) {
    return;
  }

  if (shared.mode == ExtractionMode::FaceStrip) {
    BMFace *f;
    BMIter fiter;
    BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
      BM_elem_flag_toggle(f, BM_ELEM_TAG);
    }
    BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_FACES);
  }
  else {
    /* Remove faces only — do not use #DEL_FACES here: that context also deletes edges and
     * vertices that belonged to tagged faces. With every face tagged, the whole mesh is removed. */
    BMFace *f, *f_next;
    BMIter fiter;
    BM_ITER_MESH_MUTABLE (f, f_next, &fiter, bm, BM_FACES_OF_MESH) {
      BM_face_kill(bm, f);
    }

    BMEdge *e;
    BMIter eiter;
    BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
      BM_elem_flag_toggle(e, BM_ELEM_TAG);
    }
    BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_EDGES);

    BM_mesh_elem_hflag_enable_all(bm, BM_VERT, BM_ELEM_TAG, false);
    BMVert *v;
    BMIter viter;
    BM_ITER_MESH (v, &viter, bm, BM_VERTS_OF_MESH) {
      if (BM_vert_edge_count(v) > 0) {
        BM_elem_flag_set(v, BM_ELEM_TAG, false);
      }
    }
    BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_VERTS);
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
}

static Mesh *build_extracted_mesh_for_new_object(bContext &C, ExtractLoopModalData &data)
{
  if (!data.shared.seed_edge || !extract_preview_is_valid(data.shared)) {
    return nullptr;
  }

  Object *obact = data.shared.obact;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
  BKE_sculpt_update_object_for_edit(depsgraph, obact, false);

  Mesh *src_mesh = id_cast<Mesh *>(obact->data);

  BMesh *bm = create_source_bmesh_for_new_object(*src_mesh);
  ExtractLoopSharedData work = rewalk_on_bmesh(data.shared, bm, data.use_boundary_walker);

  if (!extract_preview_is_valid(work)) {
    BM_mesh_free(bm);
    return nullptr;
  }

  isolate_extraction_geometry_in_bmesh(work);
  update_bmesh_positions_from_preview(work);
  BM_mesh_normals_update(bm);

  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  Mesh *new_mesh = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, src_mesh);
  BM_mesh_free(bm);

  return new_mesh;
}

static void create_mesh_in_new_object(bContext &C, ExtractLoopModalData &data)
{
  Mesh *new_mesh = build_extracted_mesh_for_new_object(C, data);
  if (!new_mesh) {
    return;
  }
  if (new_mesh->verts_num == 0) {
    BKE_id_free(nullptr, new_mesh);
    return;
  }

  Main *bmain = CTX_data_main(&C);
  View3D *v3d = CTX_wm_view3d(&C);
  Object *obact = data.shared.obact;

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

  if (data.shared.mode == ExtractionMode::FaceStrip) {
    BKE_report(CTX_wm_reports(&C), RPT_INFO, "Face strip extracted to new mesh object");
  }
  else {
    BKE_report(CTX_wm_reports(&C), RPT_INFO, "Loop extracted to new mesh object");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curves Extraction
 * \{ */

/**
 * Extract the previewed loop or ring as a new curves object.
 * Uses `preview_points` directly (already in object local space, current at confirmation time).
 * Loop mode produces one polyline curve; Ring mode produces one 2-point curve per edge.
 * FaceStrip mode is not supported — falls back to mesh extraction with a warning.
 */
static void create_curves_in_new_object(bContext &C, ExtractLoopModalData &data)
{
  if (!data.shared.seed_edge || !extract_preview_is_valid(data.shared)) {
    return;
  }

  if (data.shared.mode == ExtractionMode::FaceStrip) {
    BKE_report(CTX_wm_reports(&C),
               RPT_WARNING,
               "Curves output is not supported for Face Strip mode, extracting as mesh instead");
    create_mesh_in_new_object(C, data);
    return;
  }

  const Span<float3> pts = data.shared.preview_points;
  if (pts.is_empty()) {
    return;
  }

  bke::CurvesGeometry curves_geom;

  if (data.shared.mode == ExtractionMode::Loop) {
    curves_geom.resize(pts.size(), 1);
    curves_geom.offsets_for_write()[0] = 0;
    curves_geom.offsets_for_write()[1] = pts.size();
    curves_geom.fill_curve_types(CURVE_TYPE_POLY);
    curves_geom.positions_for_write().copy_from(pts);
    if (data.shared.is_cyclic) {
      curves_geom.cyclic_for_write().fill(true);
    }
  }
  else { /* Ring: preview_points contains (v1, v2) pairs, one per edge. */
    BLI_assert(pts.size() % 2 == 0);
    const int edges_num = int(pts.size()) / 2;
    curves_geom.resize(pts.size(), edges_num);
    MutableSpan<int> offsets = curves_geom.offsets_for_write();
    for (int i = 0; i <= edges_num; i++) {
      offsets[i] = i * 2;
    }
    curves_geom.fill_curve_types(CURVE_TYPE_POLY);
    curves_geom.positions_for_write().copy_from(pts);
  }

  curves_geom.tag_topology_changed();
  Curves *curves_id = bke::curves_new_nomain(std::move(curves_geom));

  Main *bmain = CTX_data_main(&C);
  View3D *v3d = CTX_wm_view3d(&C);
  Object *obact = data.shared.obact;

  ushort local_view_bits = 0;
  if (v3d && v3d->localvd) {
    local_view_bits = v3d->local_view_uid;
  }

  Object *new_ob = ed::object::add_type(
      &C, OB_CURVES, nullptr, obact->loc, obact->rot, false, local_view_bits);
  Curves *ob_curves_id = id_cast<Curves *>(new_ob->data);
  ob_curves_id->geometry.wrap() = std::move(curves_id->geometry.wrap());
  BKE_id_free(nullptr, curves_id);

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(&C, NC_GEOM | ND_DATA, new_ob->data);
  WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, new_ob);

  if (data.shared.mode == ExtractionMode::Ring) {
    BKE_report(CTX_wm_reports(&C), RPT_INFO, "Ring extracted to new curves object");
  }
  else {
    BKE_report(CTX_wm_reports(&C), RPT_INFO, "Loop extracted to new curves object");
  }
}

/** \} */

void finish_extract(bContext *C, wmOperator *op, ExtractLoopModalData *data)
{
  if (is_face_strip_extrude_output(op, data->shared.mode)) {
    extrude_commit(*C, op, *data);
  }
  else if (RNA_boolean_get(op->ptr, "new_object")) {
    if (ExtractionOutputType(RNA_enum_get(op->ptr, "output_type")) ==
        ExtractionOutputType::Curves)
    {
      create_curves_in_new_object(*C, *data);
    }
    else {
      create_mesh_in_new_object(*C, *data);
    }
  }
  else {
    duplicate_geometry_in_object(*C, op, *data);
  }
  gesture_data_free(C, data);
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  extract_loop_hover_deactivate();
  ED_region_tag_redraw(CTX_wm_region(C));
}

}  // namespace blender::ed::sculpt_paint::extract_loop
