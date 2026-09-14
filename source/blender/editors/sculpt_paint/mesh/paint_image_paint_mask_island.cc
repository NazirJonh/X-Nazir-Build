/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Paint Mask Island: click a UV island in the Image Editor and make its faces the face selection
 * paint mask (#Mesh.editflag & #ME_EDIT_PAINT_FACE_SEL, backed by the `.select_poly` face
 * attribute) of the 3D Viewport, so the next strokes reach exactly that island.
 *
 * The island definition is the one the Image Editor already uses for its own
 * "Selection Expand → Island" mode (#ED_uvedit_uv_islands_tag_from_face_indices): UV connectivity,
 * i.e. what the user sees as one island in the editor.
 */

#include <climits>
#include <utility>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"
#include "BLI_virtual_array.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
/* #ID_IS_EDITABLE needs the #LibraryRuntime definition and #LIBRARY_ASSET_EDITABLE. */
#include "BKE_library.hh"
#include "BKE_mesh.hh"

#include "bmesh.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_uvedit.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "paint_image_select_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Island pick
 * \{ */

/**
 * `UVDelimitMode::Seam` (the enum is local to `uvedit_select.cc`, so its value is used directly).
 *
 * The flood fill groups faces by *welded UV coordinates*, which alone is not what a user means by
 * an island: marking seams does not split the UVs, only unwrapping does, so a mesh whose seams were
 * marked after (or without) a re-unwrap still has one welded UV component. Delimiting by seams on
 * top makes both cases agree -- the flood fill stops at a seam edge as well as at a UV split, which
 * is also how the 3D Viewport's "Select Linked" in paint modes defines linked faces
 * (`paintface_select_linked_faces`).
 */
constexpr int UV_ISLAND_DELIMIT_SEAM = 1 << 0;

/**
 * True when \a uv lies inside the UV polygon of \a face. Concave faces are handled by the
 * even-odd rule of #isect_point_poly_v2.
 */
static bool face_uv_contains_point(const BMFace &face, const BMUVOffsets &offsets, const float2 &uv)
{
  const int face_size = face.len;
  if (face_size < 3) {
    return false;
  }

  /* Faces are small; the inline buffer covers the common triangle/quad case. */
  Array<float2, 8> corner_uvs(face_size);
  int i = 0;
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(&face), BM_LOOPS_OF_FACE) {
    const float *corner_uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    corner_uvs[i++] = float2(corner_uv[0], corner_uv[1]);
  }

  const float point[2] = {uv.x, uv.y};
  return isect_point_poly_v2(
      point, reinterpret_cast<const float(*)[2]>(corner_uvs.data()), uint(face_size));
}

/**
 * Collect the faces of the UV island under \a uv on \a ob.
 *
 * \param r_island: Receives a per-face mask sized `mesh->faces_num`; all false when nothing was
 * hit. Hidden faces are never part of an island.
 * \return True when \a uv hit at least one face of \a ob.
 */
static bool paint_mask_island_find_object(const Scene &scene,
                                          Object &ob,
                                          const float2 &uv,
                                          Array<bool> &r_island)
{
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  r_island.reinitialize(mesh->faces_num);
  r_island.fill(false);

  /* Same conversion the Image Editor selection tools use: deliberately the *original* mesh,
   * because the island is defined by the UV layout the user authored and sees in the editor. The
   * evaluated mesh can have post-modifier topology whose face indices do not match it. */
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
  BMeshCreateParams create_params{};
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = false;
  convert_params.calc_vert_normal = false;
  BMesh *bm = BM_mesh_create(&allocsize, &create_params);
  BM_mesh_bm_from_me(bm, mesh, &convert_params);

  const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm, &ob, &scene);

  Vector<int> seed_faces;
  if (offsets.uv >= 0) {
    seed_faces.reserve(64);
    const float point[2] = {uv.x, uv.y};
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
        continue;
      }
      /* Cheap reject: only a face whose UV bounds contain the pick can contain it. */
      rctf face_uv_bounds;
      BLI_rctf_init_minmax(&face_uv_bounds);
      BMIter liter;
      BMLoop *l;
      BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
        BLI_rctf_do_minmax_v(&face_uv_bounds, BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv));
      }
      if (!BLI_rctf_isect_pt_v(&face_uv_bounds, point)) {
        continue;
      }
      if (face_uv_contains_point(*efa, offsets, uv)) {
        seed_faces.append(face_index);
      }
    }
  }

  if (seed_faces.is_empty()) {
    BM_mesh_free(bm);
    return false;
  }

  /* Faces of the islands these seeds belong to. The flood fill skips hidden faces itself and is
   * delimited by seams, see #UV_ISLAND_DELIMIT_SEAM. */
  Array<bool> island_faces(bm->totface, false);
  ED_uvedit_uv_islands_tag_from_face_indices(
      &scene, bm, offsets, seed_faces, UV_ISLAND_DELIMIT_SEAM, island_faces);

  /* The BMesh is a conversion of the mesh without topology changes, so `BM_face_at_index()` order
   * is the mesh's face order -- the same assumption the Image Editor selection tools make. */
  BLI_assert(island_faces.size() == int64_t(r_island.size()));
  r_island.as_mutable_span().copy_from(island_faces);

  BM_mesh_free(bm);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face selection writes
 * \{ */

/** True when at least one face of \a mesh is selected. */
static bool mesh_has_selected_faces(const Mesh &mesh)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  /* Deliberately a #VArray and not a #VArraySpan: `booleans_mix_calc()` only takes the former
   * (#VArraySpan derives from #Span, not from #VArray). */
  const VArray<bool> select_poly = *attributes.lookup<bool>(".select_poly",
                                                           bke::AttrDomain::Face);
  if (select_poly.is_empty()) {
    return false;
  }
  return array_utils::booleans_mix_calc(select_poly) != array_utils::BooleanMix::AllFalse;
}

/** Apply the island of one object to its face selection according to \a sel_op. */
static void paint_mask_island_apply(Mesh &mesh, const Span<bool> island, const eSelectOp sel_op)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> select_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".select_poly", bke::AttrDomain::Face);
  BLI_assert(select_poly.span.size() == island.size());

  switch (sel_op) {
    case SEL_OP_ADD:
      for (const int i : select_poly.span.index_range()) {
        select_poly.span[i] = select_poly.span[i] || island[i];
      }
      break;
    case SEL_OP_SUB:
      for (const int i : select_poly.span.index_range()) {
        if (island[i]) {
          select_poly.span[i] = false;
        }
      }
      break;
    case SEL_OP_SET:
    default:
      select_poly.span.copy_from(island);
      break;
  }
  select_poly.finish();
}

/** Clear the face selection of \a mesh. \return True when something was selected before. */
static bool paint_mask_island_clear(Mesh &mesh)
{
  if (!mesh_has_selected_faces(mesh)) {
    return false;
  }
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> select_poly = attributes.lookup_for_write_span<bool>(
      ".select_poly");
  select_poly.span.fill(false);
  select_poly.finish();
  return true;
}

/**
 * Turn the face selection paint mask of \a mesh on or off.
 *
 * Enabling mirrors the mutual exclusion of #rna_Mesh_update_facemask: the face and the vertex
 * masks cannot be active at the same time.
 */
static void paint_mask_island_mask_set(Mesh &mesh, const bool enabled)
{
  if (enabled) {
    mesh.editflag &= ~ME_EDIT_PAINT_VERT_SEL;
    mesh.editflag |= ME_EDIT_PAINT_FACE_SEL;
  }
  else {
    mesh.editflag &= ~ME_EDIT_PAINT_FACE_SEL;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pick handling
 * \{ */

/** Objects the tool operates on: editable canvas meshes outside of Edit Mode. */
static Vector<Object *> paint_mask_island_targets_get(bContext *C, const Image *image)
{
  Vector<Object *> targets;
  for (Object *ob : image_paint_selection_canvas_objects_get(
           C, image, ImagePaintCanvasPurpose::Mask))
  {
    if (ob == nullptr || ob->type != OB_MESH || (ob->mode & OB_MODE_EDIT)) {
      /* In Edit Mode the face selection lives in the BMesh, not in `.select_poly`. */
      continue;
    }
    Mesh *mesh = id_cast<Mesh *>(ob->data);
    if (mesh == nullptr || mesh->faces_num == 0 || !ID_IS_EDITABLE(&mesh->id)) {
      continue;
    }
    targets.append(ob);
  }
  return targets;
}

/** True when any of \a objects still has a selected face. */
static bool paint_mask_island_any_selected(const Span<Object *> objects)
{
  for (Object *ob : objects) {
    if (mesh_has_selected_faces(*id_cast<Mesh *>(ob->data))) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus paint_mask_island_apply_pick(bContext *C,
                                                     const int2 region_mval,
                                                     const eSelectOp sel_op)
{
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  if (scene == nullptr || sima == nullptr || region == nullptr || sima->image == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Region pixels to UV, through the same rotation-aware mapping the box/lasso gestures use. */
  float uv[2];
  ui::view2d_region_to_view(
      &region->v2d, float(region_mval[0]), float(region_mval[1]), &uv[0], &uv[1]);
  const float2 pick_uv(uv[0], uv[1]);

  const Vector<Object *> targets = paint_mask_island_targets_get(C, sima->image);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  Vector<Object *> hit_objects;
  Vector<Array<bool>> hit_islands;
  for (Object *ob : targets) {
    Array<bool> island;
    if (paint_mask_island_find_object(*scene, *ob, pick_uv, island)) {
      hit_objects.append(ob);
      hit_islands.append(std::move(island));
    }
  }

  bool changed = false;
  /* Objects whose face selection or mask flag was touched; each is flushed exactly once. */
  VectorSet<Object *> modified;
  const auto mark_modified = [&](Object *ob) {
    if (!modified.contains(ob)) {
      modified.add_new(ob);
    }
  };

  if (sel_op == SEL_OP_SET) {
    /* "Set" replaces the whole mask: clear the face selection of every object that currently
     * carries it, so the islands under the cursor really are the only selected geometry. */
    for (Object *ob : targets) {
      Mesh &mesh = *id_cast<Mesh *>(ob->data);
      if (!(mesh.editflag & ME_EDIT_PAINT_FACE_SEL)) {
        continue;
      }
      if (paint_mask_island_clear(mesh)) {
        changed = true;
        mark_modified(ob);
      }
    }
  }

  for (const int i : hit_objects.index_range()) {
    Object *ob = hit_objects[i];
    Mesh &mesh = *id_cast<Mesh *>(ob->data);
    paint_mask_island_apply(mesh, hit_islands[i], sel_op);
    paint_mask_island_mask_set(mesh, true);
    changed = true;
    mark_modified(ob);
  }

  /* Set or subtract can empty the mask -- a click that hit no island is exactly that case. */
  if ((sel_op == SEL_OP_SET || sel_op == SEL_OP_SUB) &&
      !paint_mask_island_any_selected(targets))
  {
    /* An enabled mask with nothing selected silently blocks every stroke, so switch the masking
     * off together with the (now empty) selection. */
    for (Object *ob : targets) {
      Mesh &mesh = *id_cast<Mesh *>(ob->data);
      if (!(mesh.editflag & ME_EDIT_PAINT_FACE_SEL)) {
        continue;
      }
      paint_mask_island_mask_set(mesh, false);
      changed = true;
      mark_modified(ob);
    }
  }

  if (!changed) {
    return OPERATOR_CANCELLED;
  }

  /* Publishes the new selection: `.select_poly` of the evaluated mesh, the PBVH face selection
   * the 3D overlay draws from, the paint overlay batches and the `NC_GEOM | ND_SELECT` notifier
   * both editors listen to. */
  for (Object *ob : modified) {
    paintface_flush_flags(C, ob, true, false);
  }

  ED_region_tag_redraw(region);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus paint_mask_island_exec(bContext *C, wmOperator *op)
{
  int2 region_mval;
  RNA_int_get_array(op->ptr, "location", region_mval);
  return paint_mask_island_apply_pick(C, region_mval, eSelectOp(RNA_enum_get(op->ptr, "mode")));
}

static wmOperatorStatus paint_mask_island_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  /* The keymap supplies `mode`: SET, ADD on Shift and SUB on Ctrl. */
  RNA_int_set_array(op->ptr, "location", event->mval);
  return paint_mask_island_exec(C, op);
}

void PAINT_OT_paint_mask_island(wmOperatorType *ot)
{
  ot->name = "Paint Mask Island";
  ot->idname = "PAINT_OT_paint_mask_island";
  ot->description =
      "Click a UV island in the Image Editor to use its faces as the face selection paint mask";

  ot->invoke = paint_mask_island_invoke;
  ot->exec = paint_mask_island_exec;
  ot->poll = image_paint_selection_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->cursor_pending = WM_CURSOR_EYEDROPPER;

  WM_operator_properties_select_operation_simple(ot);

  ot->prop = RNA_def_int_array(ot->srna,
                               "location",
                               2,
                               nullptr,
                               0,
                               SHRT_MAX,
                               "Location",
                               "Region coordinates of the picked island",
                               0,
                               SHRT_MAX);
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

}  // namespace blender
