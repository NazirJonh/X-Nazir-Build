/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_shrinkwrap.hh"

#include "BLI_math_vector_c.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"

#include "bmesh_tools.hh"

#include "MEM_guardedalloc.h"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

static bool geometry_extract_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob != nullptr && ob->mode == OB_MODE_SCULPT) {
    if (ob->runtime->sculpt_session->bm) {
      CTX_wm_operator_poll_msg_set(C, "The geometry cannot be extracted with dyntopo activated");
      return false;
    }
    return ED_operator_object_active_editable_mesh(C);
  }
  return false;
}

struct GeometryExtractParams {
  /* For extracting face sets. */
  int active_face_set;

  /* For extracting mask. */
  float mask_threshold;

  /* Common parameters. */
  bool add_boundary_loop;
  int num_smooth_iterations;
  bool apply_shrinkwrap;
  bool add_solidify;
};

/* Function that tags in BMesh the faces that should be deleted in the extracted object. */
using GeometryExtractTagMeshFunc = void(BMesh *, GeometryExtractParams *);

/* Return true when every vertex of \a f has a mask value at or above \a threshold. */
static bool face_verts_all_masked(BMFace *f, const int cd_vert_mask_offset, const float threshold)
{
  BMVert *v;
  BMIter iter;
  BM_ITER_ELEM (v, &iter, f, BM_VERTS_OF_FACE) {
    if (BM_ELEM_CD_GET_FLOAT(v, cd_vert_mask_offset) < threshold) {
      return false;
    }
  }
  return true;
}

static wmOperatorStatus geometry_extract_apply(bContext *C,
                                               wmOperator *op,
                                               GeometryExtractTagMeshFunc *tag_fn,
                                               GeometryExtractParams *params)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);
  Depsgraph &depsgraph = *CTX_data_depsgraph_on_load(C);

  ed::sculpt_paint::object_sculpt_mode_exit(C, depsgraph);

  /* Ensures that deformation from sculpt mode is taken into account before duplicating the mesh to
   * extract the geometry. */
  CTX_data_ensure_evaluated_depsgraph(C);

  Mesh *mesh = id_cast<Mesh *>(ob->data);
  Mesh *new_mesh = id_cast<Mesh *>(BKE_id_copy(bmain, &mesh->id));

  BMeshFromMeshParams mesh_to_bm_params{};
  mesh_to_bm_params.calc_face_normal = true;
  mesh_to_bm_params.calc_vert_normal = true;
  BMesh *bm = bmesh_from_mesh_with_toolflags(*new_mesh, mesh_to_bm_params);

  BMEditMesh *em = BKE_editmesh_create(bm);

  /* Generate the tags for deleting geometry in the extracted object. */
  tag_fn(bm, params);

  /* Delete all tagged faces. */
  BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_FACES);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BMVert *v;
  BMEdge *ed;
  BMIter iter;
  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    mul_v3_v3(v->co, ob->scale);
  }

  if (params->add_boundary_loop) {
    BM_ITER_MESH (ed, &iter, bm, BM_EDGES_OF_MESH) {
      BM_elem_flag_set(ed, BM_ELEM_TAG, BM_edge_is_boundary(ed));
    }
    EDBM_extrude_edges_indiv(em, op, BM_ELEM_TAG, false);

    for (int repeat = 0; repeat < params->num_smooth_iterations; repeat++) {
      BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
      BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
        BM_elem_flag_set(v, BM_ELEM_TAG, !BM_vert_is_boundary(v));
      }
      for (int i = 0; i < 3; i++) {
        if (!EDBM_smooth_vert(em, op)) {
          continue;
        }
      }

      BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
      BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
        BM_elem_flag_set(v, BM_ELEM_TAG, BM_vert_is_boundary(v));
      }
      for (int i = 0; i < 1; i++) {
        if (!EDBM_smooth_vert(em, op)) {
          continue;
        }
      }
    }
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);

  BKE_id_free(bmain, new_mesh);
  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  new_mesh = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, mesh);

  /* Remove the face sets as they need to be recreated when entering Sculpt Mode in the new object.
   * TODO(pablodobarro): In the future we can try to preserve them from the original mesh. */
  new_mesh->attributes_for_write().remove(".sculpt_face_set");

  /* Remove the mask from the new object so it can be sculpted directly after extracting. */
  new_mesh->attributes_for_write().remove(".sculpt_mask");

  BKE_editmesh_free_data(em);
  MEM_delete(em);

  if (new_mesh->verts_num == 0) {
    BKE_id_free(bmain, new_mesh);
    return OPERATOR_FINISHED;
  }

  ushort local_view_bits = 0;
  if (v3d && v3d->localvd) {
    local_view_bits = v3d->local_view_uid;
  }
  Object *new_ob = ed::object::add_type(
      C, OB_MESH, nullptr, ob->loc, ob->rot, false, local_view_bits);
  BKE_mesh_nomain_to_mesh(new_mesh, id_cast<Mesh *>(new_ob->data), new_ob);

  if (params->apply_shrinkwrap) {
    BKE_shrinkwrap_mesh_nearest_surface_deform(CTX_data_depsgraph_pointer(C), scene, new_ob, ob);
  }

  if (params->add_solidify) {
    ed::object::modifier_add(
        op->reports, bmain, scene, new_ob, "geometry_extract_solidify", eModifierType_Solidify);
    SolidifyModifierData *sfmd = reinterpret_cast<SolidifyModifierData *>(
        BKE_modifiers_findby_name(new_ob, "mask_extract_solidify"));
    if (sfmd) {
      sfmd->offset = -0.05f;
    }
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, new_ob);
  BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(new_ob->data), BKE_MESH_BATCH_DIRTY_ALL);
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, new_ob->data);

  return OPERATOR_FINISHED;
}

static void geometry_extract_tag_masked_faces(BMesh *bm, GeometryExtractParams *params)
{
  const float threshold = params->mask_threshold;

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  const int cd_vert_mask_offset = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");

  BMFace *f;
  BMIter iter;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test_bool(f, BM_ELEM_HIDDEN)) {
      BM_elem_flag_set(f, BM_ELEM_TAG, true);
      continue;
    }
    BM_elem_flag_set(f, BM_ELEM_TAG, !face_verts_all_masked(f, cd_vert_mask_offset, threshold));
  }
}

static void geometry_extract_tag_face_set(BMesh *bm, GeometryExtractParams *params)
{
  const int tag_face_set_id = params->active_face_set;

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  const int cd_face_sets_offset = CustomData_get_offset_named(
      &bm->pdata, CD_PROP_INT32, ".sculpt_face_set");

  BMFace *f;
  BMIter iter;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    const int face_set = BM_ELEM_CD_GET_INT(f, cd_face_sets_offset);
    BM_elem_flag_set(f, BM_ELEM_TAG, face_set != tag_face_set_id);
  }
}

static wmOperatorStatus paint_mask_extract_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = id_cast<Mesh *>(ob->data);
  if (!mesh->attributes().contains(".sculpt_mask")) {
    return OPERATOR_CANCELLED;
  }

  GeometryExtractParams params;
  params.mask_threshold = RNA_float_get(op->ptr, "mask_threshold");
  params.num_smooth_iterations = RNA_int_get(op->ptr, "smooth_iterations");
  params.add_boundary_loop = RNA_boolean_get(op->ptr, "add_boundary_loop");
  params.apply_shrinkwrap = RNA_boolean_get(op->ptr, "apply_shrinkwrap");
  params.add_solidify = RNA_boolean_get(op->ptr, "add_solidify");

  /* Push an undo step prior to extraction.
   * NOTE: A second push happens after the operator due to
   * the OPTYPE_UNDO flag; having an initial undo step here
   * is just needed to preserve the active object pointer.
   *
   * Fixes #103261.
   */
  ED_undo_push_op(C, op);

  return geometry_extract_apply(C, op, geometry_extract_tag_masked_faces, &params);
}

static wmOperatorStatus paint_mask_extract_invoke(bContext *C, wmOperator *op, const wmEvent *e)
{
  return WM_operator_props_popup_confirm_ex(
      C, op, e, IFACE_("Create Mesh From Paint Mask"), IFACE_("Extract"));
}

static void geometry_extract_props(StructRNA *srna)
{
  RNA_def_boolean(srna,
                  "add_boundary_loop",
                  true,
                  "Add Boundary Loop",
                  "Add an extra edge loop to better preserve the shape when applying a "
                  "subdivision surface modifier");
  RNA_def_int(srna,
              "smooth_iterations",
              4,
              0,
              INT_MAX,
              "Smooth Iterations",
              "Smooth iterations applied to the extracted mesh",
              0,
              20);
  RNA_def_boolean(srna,
                  "apply_shrinkwrap",
                  true,
                  "Project to Sculpt",
                  "Project the extracted mesh into the original sculpt");
  RNA_def_boolean(srna,
                  "add_solidify",
                  true,
                  "Extract as Solid",
                  "Extract the mask as a solid object with a solidify modifier");
}

void SCULPT_OT_paint_mask_extract(wmOperatorType *ot)
{
  ot->name = "Mask Extract";
  ot->description = "Create a new mesh object from the current paint mask";
  ot->idname = "SCULPT_OT_paint_mask_extract";

  ot->poll = geometry_extract_poll;
  ot->invoke = paint_mask_extract_invoke;
  ot->exec = paint_mask_extract_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float_factor(
      ot->srna,
      "mask_threshold",
      0.5f,
      0.0f,
      1.0f,
      "Threshold",
      "Minimum mask value to consider the vertex valid to extract a face from the original mesh",
      0.0f,
      1.0f);

  geometry_extract_props(ot->srna);
}

static wmOperatorStatus face_set_extract_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  using namespace blender::ed;
  if (!CTX_wm_region_view3d(C)) {
    return OPERATOR_CANCELLED;
  }
  ARegion *region = CTX_wm_region(C);

  const float mval[2] = {float(event->xy[0] - region->winrct.xmin),
                         float(event->xy[1] - region->winrct.ymin)};

  Object &ob = *CTX_data_active_object(C);
  const int face_set_id = sculpt_paint::face_set::active_update_and_get(C, ob, mval);
  if (face_set_id == face_set_none_id) {
    return OPERATOR_CANCELLED;
  }

  GeometryExtractParams params;
  params.active_face_set = face_set_id;
  params.num_smooth_iterations = 0;
  params.add_boundary_loop = false;
  params.apply_shrinkwrap = true;
  params.add_solidify = true;
  return geometry_extract_apply(C, op, geometry_extract_tag_face_set, &params);
}

void SCULPT_OT_face_set_extract(wmOperatorType *ot)
{
  ot->name = "Face Set Extract";
  ot->description = "Create a new mesh object from the selected face set";
  ot->idname = "SCULPT_OT_face_set_extract";

  ot->poll = geometry_extract_poll;
  ot->invoke = face_set_extract_invoke;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  geometry_extract_props(ot->srna);
}

static void slice_paint_mask(BMesh *bm, bool invert, bool fill_holes, float mask_threshold)
{
  BMFace *f;
  BMIter iter;

  /* Delete all masked faces */
  const int cd_vert_mask_offset = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  BLI_assert(cd_vert_mask_offset != -1);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    bool keep_face = face_verts_all_masked(f, cd_vert_mask_offset, mask_threshold);
    if (BM_elem_flag_test_bool(f, BM_ELEM_HIDDEN)) {
      keep_face = false;
    }
    /* This invert behavior is fragile, as it potentially marks faces which are hidden */
    if (invert) {
      keep_face = !keep_face;
    }
    BM_elem_flag_set(f, BM_ELEM_TAG, keep_face);
  }

  BM_mesh_delete_hflag_context(bm, BM_ELEM_TAG, DEL_FACES);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BM_mesh_elem_hflag_enable_all(bm, BM_EDGE, BM_ELEM_TAG, false);

  if (fill_holes) {
    BM_mesh_edgenet(bm, false, true);
    BM_mesh_normals_update(bm);
    BMO_op_callf(bm,
                 (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
                 "triangulate faces=%hf quad_method=%i ngon_method=%i",
                 BM_ELEM_TAG,
                 0,
                 0);

    BM_mesh_elem_hflag_enable_all(bm, BM_FACE, BM_ELEM_TAG, false);
    BMO_op_callf(bm,
                 (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
                 "recalc_face_normals faces=%hf",
                 BM_ELEM_TAG);
    BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  }
}

static wmOperatorStatus paint_mask_slice_exec(bContext *C, wmOperator *op)
{
  using namespace blender::ed;
  const Scene &scene = *CTX_data_scene(C);
  Main &bmain = *CTX_data_main(C);
  Object &ob = *CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  Mesh *mesh = id_cast<Mesh *>(ob.data);

  if (!mesh->attributes().contains(".sculpt_mask")) {
    return OPERATOR_CANCELLED;
  }

  bool create_new_object = RNA_boolean_get(op->ptr, "new_object");
  bool fill_holes = RNA_boolean_get(op->ptr, "fill_holes");
  float mask_threshold = RNA_float_get(op->ptr, "mask_threshold");

  Mesh *new_mesh = id_cast<Mesh *>(BKE_id_copy(&bmain, &mesh->id));

  /* Undo crashes when new object is created in the middle of a sculpt, see #87243. */
  if (ob.mode == OB_MODE_SCULPT && !create_new_object) {
    sculpt_paint::undo::geometry_begin(scene, ob, op);
  }

  BMeshFromMeshParams mesh_to_bm_params{};
  mesh_to_bm_params.calc_face_normal = true;
  BMesh *bm = bmesh_from_mesh_with_toolflags(*new_mesh, mesh_to_bm_params);

  slice_paint_mask(bm, false, fill_holes, mask_threshold);
  BKE_id_free(&bmain, new_mesh);
  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  new_mesh = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, mesh);
  BM_mesh_free(bm);

  if (create_new_object) {
    ushort local_view_bits = 0;
    if (v3d && v3d->localvd) {
      local_view_bits = v3d->local_view_uid;
    }
    Object *new_ob = ed::object::add_type(
        C, OB_MESH, nullptr, ob.loc, ob.rot, false, local_view_bits);
    Mesh *new_ob_mesh = id_cast<Mesh *>(BKE_id_copy(&bmain, &mesh->id));

    bm = bmesh_from_mesh_with_toolflags(*new_ob_mesh, mesh_to_bm_params);

    slice_paint_mask(bm, true, fill_holes, mask_threshold);
    BKE_id_free(&bmain, new_ob_mesh);
    new_ob_mesh = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, mesh);
    BM_mesh_free(bm);

    /* Remove the mask from the new object so it can be sculpted directly after slicing. */
    new_ob_mesh->attributes_for_write().remove(".sculpt_mask");

    Mesh *new_mesh = id_cast<Mesh *>(new_ob->data);
    BKE_mesh_nomain_to_mesh(new_ob_mesh, new_mesh, new_ob);
    WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, new_ob);
    BKE_mesh_batch_cache_dirty_tag(new_mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_relations_tag_update(&bmain);
    DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, new_mesh);
  }

  mesh = id_cast<Mesh *>(ob.data);
  BKE_mesh_nomain_to_mesh(new_mesh, mesh, &ob);

  if (ob.mode == OB_MODE_SCULPT) {
    if (mesh->attributes().contains(".sculpt_face_set")) {
      /* Assign a new face set ID to the new faces created by the slice operation. */
      const int next_face_set_id = sculpt_paint::face_set::find_next_available_id(ob);
      sculpt_paint::face_set::initialize_none_to_id(mesh, next_face_set_id);
    }
    if (!create_new_object) {
      sculpt_paint::undo::geometry_end(ob);
      BKE_sculptsession_free_pbvh(ob);
    }
  }

  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, mesh);

  return OPERATOR_FINISHED;
}

void SCULPT_OT_paint_mask_slice(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Mask Slice";
  ot->description = "Slices the paint mask from the mesh";
  ot->idname = "SCULPT_OT_paint_mask_slice";

  ot->poll = geometry_extract_poll;
  ot->exec = paint_mask_slice_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_float(
      ot->srna,
      "mask_threshold",
      0.5f,
      0.0f,
      1.0f,
      "Threshold",
      "Minimum mask value to consider the vertex valid to extract a face from the original mesh",
      0.0f,
      1.0f);
  prop = RNA_def_boolean(
      ot->srna, "fill_holes", true, "Fill Holes", "Fill holes after slicing the mask");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna,
                         "new_object",
                         true,
                         "Slice to New Object",
                         "Create a new object from the sliced mask");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* -------------------------------------------------------------------- */
/** \name Mask Duplicate/Cut Operator
 * \{ */

enum class MaskDuplicateMode {
  DUPLICATE = 0,
  CUT = 1,
};

/* Tag faces whose vertices are all masked above the threshold. Returns whether any face was
 * tagged. */
static bool tag_masked_faces_for_duplicate(BMesh *bm, const float mask_threshold)
{
  const int cd_vert_mask_offset = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  BLI_assert(cd_vert_mask_offset != -1);

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BMFace *f;
  BMIter iter;
  bool any_tagged = false;

  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test_bool(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    bool all_masked = true;
    BMVert *v;
    BMIter face_iter;
    BM_ITER_ELEM (v, &face_iter, f, BM_VERTS_OF_FACE) {
      const float mask = BM_ELEM_CD_GET_FLOAT(v, cd_vert_mask_offset);
      if (mask < mask_threshold) {
        all_masked = false;
        break;
      }
    }

    if (all_masked) {
      BM_elem_flag_set(f, BM_ELEM_TAG, true);
      any_tagged = true;
    }
  }

  return any_tagged;
}

static int next_bm_face_set_id(BMesh *bm)
{
  const int cd_face_sets_offset = CustomData_get_offset_named(
      &bm->pdata, CD_PROP_INT32, ".sculpt_face_set");
  if (cd_face_sets_offset == -1) {
    return -1;
  }

  int next_face_set = 1;
  BMFace *face;
  BMIter iter;
  BM_ITER_MESH (face, &iter, bm, BM_FACES_OF_MESH) {
    next_face_set = std::max(next_face_set, BM_ELEM_CD_GET_INT(face, cd_face_sets_offset));
  }
  return next_face_set + 1;
}

static void assign_bm_face_set_to_tagged(BMesh *bm, const int face_set_id)
{
  const int cd_face_sets_offset = CustomData_get_offset_named(
      &bm->pdata, CD_PROP_INT32, ".sculpt_face_set");
  if (cd_face_sets_offset == -1) {
    return;
  }

  BMFace *face;
  BMIter iter;
  BM_ITER_MESH (face, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test_bool(face, BM_ELEM_TAG)) {
      BM_ELEM_CD_SET_INT(face, cd_face_sets_offset, face_set_id);
    }
  }
}

static void edgenet_fill_tagged_boundary_edges(BMesh *bm, const bool use_face_sets)
{
  BM_mesh_edgenet(bm, true, true);
  BM_mesh_normals_update(bm);
  BMO_op_callf(bm,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "triangulate faces=%hf quad_method=%i ngon_method=%i",
               BM_ELEM_TAG,
               0,
               0);

  /* Assign face sets to new fill geometry before tagging all faces for normal recalc. */
  if (use_face_sets) {
    const int face_set_id = next_bm_face_set_id(bm);
    if (face_set_id != -1) {
      assign_bm_face_set_to_tagged(bm, face_set_id);
    }
  }

  BM_mesh_elem_hflag_enable_all(bm, BM_FACE, BM_ELEM_TAG, false);
  BMO_op_callf(bm,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "recalc_face_normals faces=%hf",
               BM_ELEM_TAG);

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
}

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

static int object_active_shapekey_index(const Object &ob)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if ((ob.shapenr == 0) && mesh->key && !mesh->key->block.is_empty()) {
    return 1;
  }
  return ob.shapenr;
}

/**
 * Fill open boundary holes on either the separated piece or the remaining mesh.
 *
 * \param fill_piece: true  -> fill boundaries where both edge verts belong to \a verts
 *                            (i.e. the piece's open boundary).
 *                    false -> fill boundaries where NOT both verts belong to \a verts
 *                            (i.e. the hole left in the remaining mesh after a cut).
 */
static void fill_boundary_holes(BMesh *bm,
                                const Span<BMVert *> verts,
                                const bool fill_piece,
                                const bool use_face_sets)
{
  Set<BMVert *> vert_set;
  for (BMVert *vert : verts) {
    vert_set.add(vert);
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BMEdge *e;
  BMIter iter;
  BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
    if (!BM_edge_is_boundary(e)) {
      continue;
    }
    const bool both_in_set = vert_set.contains(e->v1) && vert_set.contains(e->v2);
    if (fill_piece != both_in_set) {
      continue;
    }
    BM_elem_flag_set(e, BM_ELEM_TAG, true);
  }

  edgenet_fill_tagged_boundary_edges(bm, use_face_sets);
}

static wmOperatorStatus mask_duplicate_exec(bContext *C, wmOperator *op)
{
  using namespace blender::ed;

  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  if (!mesh->attributes().contains(".sculpt_mask")) {
    BKE_report(op->reports, RPT_WARNING, "The mesh has no paint mask");
    return OPERATOR_CANCELLED;
  }

  /* Get operator properties. */
  const MaskDuplicateMode mode = MaskDuplicateMode(RNA_enum_get(op->ptr, "mode"));
  const bool fill_holes = RNA_boolean_get(op->ptr, "fill_holes");
  const bool fill_piece_holes = RNA_boolean_get(op->ptr, "fill_piece_holes");
  const float mask_threshold = RNA_float_get(op->ptr, "mask_threshold");
  const bool use_face_sets = mesh->attributes().contains(".sculpt_face_set");

  /* Sync sculpt session state. */
  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  /* Create BMesh. */
  BMeshFromMeshParams mesh_to_bm_params{};
  mesh_to_bm_params.calc_face_normal = true;
  mesh_to_bm_params.use_shapekey = true;
  mesh_to_bm_params.active_shapekey = object_active_shapekey_index(ob);
  mesh_to_bm_params.add_key_index = true;
  BMesh *bm = bmesh_from_mesh_with_toolflags(*mesh, mesh_to_bm_params);
  BM_mesh_elem_toolflags_ensure(bm);

  /* Tag masked faces. */
  if (!tag_masked_faces_for_duplicate(bm, mask_threshold)) {
    BM_mesh_free(bm);
    return OPERATOR_CANCELLED;
  }

  /* Get mask offset for later mask inversion. */
  const int cd_vert_mask_offset = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
  if (cd_vert_mask_offset == -1) {
    /* Shouldn't happen: we checked mesh->attributes().contains(".sculpt_mask") above. */
    BM_mesh_free(bm);
    return OPERATOR_CANCELLED;
  }

  /* Begin geometry undo only after confirming the operation will proceed. */
  sculpt_paint::undo::geometry_begin(scene, ob, op);

  /* Execute duplicate or split operation. */
  BMOperator bmo_op;
  if (mode == MaskDuplicateMode::CUT) {
    BMO_op_init(bm, &bmo_op, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE), "split");
    BMO_slot_bool_set(bmo_op.slots_in, "use_only_faces", true);
  }
  else {
    BMO_op_init(bm, &bmo_op, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE), "duplicate");
  }
  BMO_slot_buffer_from_enabled_hflag(
      bm, &bmo_op, bmo_op.slots_in, "geom", BM_FACE, BM_ELEM_TAG);
  BMO_op_exec(bm, &bmo_op);

  Vector<BMVert *> split_verts;
  BMOIter oiter;
  BMVert *v;
  BMO_ITER (v, &oiter, bmo_op.slots_out, "geom.out", BM_VERT) {
    split_verts.append(v);
  }

  BMO_op_finish(bm, &bmo_op);

  if (fill_piece_holes) {
    fill_boundary_holes(bm, split_verts, true, use_face_sets);
  }

  if (mode == MaskDuplicateMode::CUT && fill_holes) {
    fill_boundary_holes(bm, split_verts, false, use_face_sets);
  }

  /* Mask inversion: piece island unmasked, everything else masked for transform. */
  Set<BMVert *> piece_verts;
  collect_verts_in_face_islands(split_verts, piece_verts);

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

  /* Write back to the object's mesh directly so existing shape keys are updated. */
  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;
  BM_mesh_bm_to_me(nullptr, bm, mesh, &bm_to_mesh_params);
  BM_mesh_free(bm);

  /* Assign a face set only to new faces that still have the default none id. */
  if (use_face_sets) {
    const int next_face_set_id = sculpt_paint::face_set::find_next_available_id(ob);
    sculpt_paint::face_set::initialize_none_to_id(mesh, next_face_set_id);
  }

  /* End geometry undo and rebuild PBVH. */
  sculpt_paint::undo::geometry_end(ob);
  BKE_sculptsession_free_pbvh(ob);

  /* The vertex count changed, so drop the cached deform coordinates. The sculpt session only
   * rebuilds #SculptSession.deform_cos when it is empty, so leaving the stale array (sized to the
   * old vertex count) would make #cache_source_get assert during the next depsgraph evaluation on
   * shape-keyed or deform-modifier meshes. */
  BKE_sculptsession_free_deformMats(ob.runtime->sculpt_session);

  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, mesh);

  /* Re-evaluate the modified mesh so the pivot computation below operates on the new geometry
   * (and updated shape keys) instead of stale evaluated data. */
  Main *bmain = CTX_data_main(C);
  BKE_scene_graph_update_tagged(depsgraph, bmain);

  /* Set pivot to center of unmasked geometry (the duplicated/cut piece). Call the shared helper
   * directly rather than invoking #SCULPT_OT_set_pivot_position, which would push a redundant
   * undo step from inside this operator. */
  set_pivot_to_unmasked_position(C, ob);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus mask_duplicate_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const wmOperatorStatus retval = mask_duplicate_exec(C, op);
  OPERATOR_RETVAL_CHECK(retval);

  if (retval & OPERATOR_CANCELLED) {
    return retval;
  }
  if (!RNA_boolean_get(op->ptr, "move_away")) {
    return retval;
  }

  return WM_operator_name_call(
      C, "TRANSFORM_OT_translate", wm::OpCallContext::InvokeDefault, nullptr, event);
}

void SCULPT_OT_paint_mask_duplicate(wmOperatorType *ot)
{
  static const EnumPropertyItem mode_items[] = {
      {int(MaskDuplicateMode::DUPLICATE),
       "DUPLICATE",
       0,
       "Duplicate",
       "Copy the masked region as new geometry inside the same object. Original masked faces "
       "remain"},
      {int(MaskDuplicateMode::CUT),
       "CUT",
       0,
       "Cut",
       "Extract the masked region as new geometry and delete the original masked faces"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Mask Duplicate";
  ot->description =
      "Duplicate or cut the masked region as new geometry within the same object and prepare for "
      "transformation";
  ot->idname = "SCULPT_OT_paint_mask_duplicate";

  ot->poll = geometry_extract_poll;
  ot->invoke = mask_duplicate_invoke;
  ot->exec = mask_duplicate_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "mode",
               mode_items,
               int(MaskDuplicateMode::DUPLICATE),
               "Mode",
               "Operation mode: duplicate or cut");

  RNA_def_float_factor(ot->srna,
                       "mask_threshold",
                       0.5f,
                       0.0f,
                       1.0f,
                       "Threshold",
                       "Minimum mask value to consider the vertex valid to extract a face",
                       0.0f,
                       1.0f);

  RNA_def_boolean(ot->srna,
                  "fill_holes",
                  false,
                  "Fill Holes",
                  "Fill the hole left in the remaining mesh after cutting (only for CUT mode)");

  RNA_def_boolean(ot->srna,
                  "fill_piece_holes",
                  true,
                  "Fill Piece Holes",
                  "Fill open boundaries on the duplicated or separated geometry");

  RNA_def_boolean(ot->srna,
                  "move_away",
                  true,
                  "Move",
                  "Start transform to move the duplicated or cut geometry after the operation");
}

/** \} */

}  // namespace blender::ed::sculpt_paint
