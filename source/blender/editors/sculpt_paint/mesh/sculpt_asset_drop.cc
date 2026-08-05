/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Drop a mesh-object asset from the Asset Browser into the active Sculpt Mode object.
 *
 * Two modes, controlled by the `join_to_active` property:
 * - Join (default): merge the dropped mesh into the active sculpt mesh, reusing the same
 *   geometry-join path as #SCULPT_OT_trim_box_gesture.
 * - Separate: keep the imported asset as an independent object placed at the snap location, mirror
 *   of the Object Mode #OBJECT_OT_transform_to_mouse behavior. For a dropped collection, all its
 *   mesh objects are still merged together into a single new object (not touching the active
 *   sculpt mesh) rather than being kept as separate objects.
 *
 * Every path here uses a plain #ED_undo_push rather than the sculpt-mode geometry undo type
 * (#ed::sculpt_paint::undo::geometry_begin/geometry_end): all paths can add, duplicate, or remove
 * a whole object (or collection), which that undo type cannot represent since it only diffs the
 * active mesh's node geometry. A step it can't fully revert would leave the redo panel unable to
 * re-run #sculpt_mesh_asset_drop_exec after undoing (the source object/collection would stay
 * gone), which is exactly the class of bug this must avoid.
 *
 * The drop box, poll and snap-matrix plumbing live in `view3d_dropboxes.cc`; this file only owns
 * the operator that runs after the drop callback.
 */

#include <algorithm>
#include <optional>

#include "GEO_join_geometries.hh"

#include "BLI_index_mask.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"

#include "DNA_collection_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "sculpt_face_set.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::asset_drop {

/* -------------------------------------------------------------------- */
/** \name Shared Helpers
 * \{ */

static Object *dropped_object_lookup(Main &bmain, wmOperator &op)
{
  const uint32_t session_uid = uint32_t(RNA_int_get(op.ptr, "session_uid"));
  if (session_uid == 0) {
    return nullptr;
  }
  return reinterpret_cast<Object *>(BKE_libblock_find_session_uid(&bmain, ID_OB, session_uid));
}

static bool snap_matrix_get(wmOperator &op, float4x4 &r_matrix)
{
  PropertyRNA *prop = RNA_struct_find_property(op.ptr, "matrix");
  if (!RNA_property_is_set(op.ptr, prop)) {
    return false;
  }
  RNA_property_float_get_array(op.ptr, prop, r_matrix.base_ptr());
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Join Into Active Sculpt Mesh
 * \{ */

/**
 * Build the dropped asset's evaluated geometry in the active object's local space.
 *
 * The snap matrix places the asset in world space; the active object's #world_to_object brings it
 * into the local space the sculpt mesh lives in, so the joined geometry lands where the user
 * dropped it. The returned mesh is a no-main copy owned by the caller.
 */
static Mesh *asset_mesh_in_active_space(const Depsgraph &depsgraph,
                                        const Object &active_ob,
                                        Object &asset_ob,
                                        const float4x4 *snap_matrix)
{
  const Object *asset_eval = DEG_get_evaluated(&depsgraph, &asset_ob);
  const Mesh *asset_mesh_eval = BKE_object_get_evaluated_mesh(asset_eval);
  if (!asset_mesh_eval || asset_mesh_eval->faces_num == 0) {
    return nullptr;
  }

  Mesh *asset_mesh = BKE_mesh_copy_for_eval(*asset_mesh_eval);

  /* Keep the asset's own face sets. They are shifted into a fresh, collision-free ID range right
   * before the join (see #prepare_asset_face_sets), preserving the asset's internal set structure
   * instead of collapsing it into a single set. */

  /* Use the evaluated active object's transform: the original (non-evaluated) transform can be
   * stale if it is driven by a constraint, driver, or an animated parent. */
  const Object *active_ob_eval = DEG_get_evaluated(&depsgraph, &active_ob);
  if (snap_matrix) {
    const float4x4 transform = active_ob_eval->world_to_object() * (*snap_matrix);
    bke::mesh_transform(*asset_mesh, transform, false);
  }
  else {
    /* No snap: place the asset using its own evaluated world transform. */
    const float4x4 transform = active_ob_eval->world_to_object() * asset_eval->object_to_world();
    bke::mesh_transform(*asset_mesh, transform, false);
  }

  return asset_mesh;
}

/**
 * Assign face sets to a to-be-joined asset mesh, shifting them into a fresh ID range starting at
 * `offset` so they never collide with the sets already on the active mesh. The active mesh's own
 * face sets are never touched — "Replace Face Sets" only affects the dropped geometry.
 *
 * - `collapse` (Replace Face Sets): the whole dropped mesh becomes a single new set (`offset`).
 * - otherwise: the asset keeps its internal set structure, every value shifted by `offset`
 * (mirrors #join_face_sets in `mesh_join.cc`). Faces without a set attribute get a single new set.
 *
 * Returns the next free ID, so a caller joining several asset meshes can keep the offset running.
 */
static int prepare_asset_face_sets(Mesh &asset_mesh, const int offset, const bool collapse)
{
  bke::MutableAttributeAccessor attributes = asset_mesh.attributes_for_write();

  if (collapse) {
    attributes.remove(".sculpt_face_set");
    attributes.add<int>(
        ".sculpt_face_set", bke::AttrDomain::Face, bke::AttributeInitValue(offset));
    return offset + 1;
  }

  bke::SpanAttributeWriter<int> face_sets = attributes.lookup_for_write_span<int>(
      ".sculpt_face_set");
  if (!face_sets) {
    /* Asset has no face sets of its own: give the whole mesh a single fresh set. */
    attributes.add<int>(
        ".sculpt_face_set", bke::AttrDomain::Face, bke::AttributeInitValue(offset));
    return offset + 1;
  }

  int max_id = offset;
  for (const int i : face_sets.span.index_range()) {
    face_sets.span[i] += offset;
    max_id = std::max(max_id, face_sets.span[i]);
  }
  face_sets.finish();
  return max_id + 1;
}

/**
 * Mask everything on `mesh` up to `existing_verts_num`, leaving the vertices past that point
 * unmasked. #join_geometries concatenates the active sculpt mesh first, so the vertices dropped in
 * occupy the tail of the array; masking the front isolates the just-added geometry for sculpting.
 *
 * Passing `mesh.verts_num` for `existing_verts_num` masks the whole mesh, which is what the
 * separate-object path wants (the added asset lives in its own object, so "everything except the
 * new geometry" is the entire active sculpt object).
 */
static void mask_existing_geometry(Mesh &mesh, const int existing_verts_num)
{
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_only_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);
  if (!mask) {
    return;
  }
  mask.span.take_front(existing_verts_num).fill(1.0f);
  mask.span.drop_front(existing_verts_num).fill(0.0f);
  mask.finish();
}

/**
 * Rebuild PBVH from the mesh (after #BKE_sculptsession_free_pbvh) and sync mask / face-set draw
 * caches so the viewport overlay matches `.sculpt_mask` / `.sculpt_face_set` on the original mesh.
 */
static void refresh_sculpt_overlays_after_drop(bContext &C, wmOperator &op, Object &object)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
  BKE_sculpt_update_object_for_edit(depsgraph, &object, false);

  if (bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object)) {
    if (pbvh->type() == bke::pbvh::Type::Mesh) {
      IndexMaskMemory memory;
      const IndexMask nodes = bke::pbvh::all_leaf_nodes(*pbvh, memory);
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      bke::pbvh::update_mask_mesh(mesh, nodes, *pbvh);
      pbvh->tag_masks_changed(nodes);
      pbvh->tag_face_sets_changed(nodes);
    }
  }

  mask_overlay_check(C, op);
  tag_update_overlays(&C);
}

static void join_asset_into_active(Object &active_ob,
                                   Mesh &asset_mesh,
                                   const bool replace_face_sets,
                                   const bool apply_mask)
{
  Mesh &sculpt_mesh = *id_cast<Mesh *>(active_ob.data);

  /* Make sure the active mesh carries a face set attribute so existing geometry keeps a valid ID
   * across the join (matches #SCULPT_OT_trim_box_gesture). */
  face_set::create_face_sets_mesh(active_ob);

  /* Assign the dropped mesh's face sets above the active mesh's IDs. In replace mode the asset
   * collapses to a single new set; otherwise its own set structure is preserved. Either way the
   * active mesh's existing sets are left untouched. */
  prepare_asset_face_sets(
      asset_mesh, face_set::find_next_available_id(active_ob), replace_face_sets);

  /* Vertex count of the active mesh before the join; the appended asset vertices start here. */
  const int existing_verts_num = sculpt_mesh.verts_num;

  bke::GeometrySet joined = geometry::join_geometries(
      {bke::GeometrySet::from_mesh(&sculpt_mesh, bke::GeometryOwnershipType::ReadOnly),
       bke::GeometrySet::from_mesh(&asset_mesh, bke::GeometryOwnershipType::ReadOnly)},
      {});
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  BKE_mesh_nomain_to_mesh(result, &sculpt_mesh, &active_ob);

  /* Mask the pre-existing geometry so only the dropped asset is left sculptable. Skipped
   * entirely when the user disabled Mask during hover, leaving any existing mask untouched. */
  if (apply_mask) {
    mask_existing_geometry(sculpt_mesh, existing_verts_num);
  }

  BKE_sculptsession_free_pbvh(active_ob);
  BKE_mesh_batch_cache_dirty_tag(&sculpt_mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&active_ob.id, ID_RECALC_GEOMETRY);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static bool sculpt_mesh_asset_drop_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_MESH || ob->mode != OB_MODE_SCULPT) {
    return false;
  }
  if (!ED_operator_scene_editable(C)) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  if (!BKE_id_is_editable(bmain, &ob->id) ||
      !BKE_id_is_editable(bmain, static_cast<const ID *>(ob->data)))
  {
    return false;
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Collection Drop Helper
 * \{ */

/**
 * Join all mesh objects from a dropped collection either into the active sculpt mesh, or into a
 * newly created, separate merged object (controlled by `join_to_active`, mirroring the
 * single-object drop path).
 *
 * When joining, each object's geometry is baked directly into the active mesh's local space:
 *   `active.world_to_object * snap_matrix * translate(-collection_center) * ob.object_to_world`
 * When building a separate object, `snap_matrix` is applied once to the new object's own
 * transform (via #BKE_object_apply_mat4) instead of being baked into the mesh, so the mesh only
 * needs the collection-center offset:
 *   `translate(-collection_center) * ob.object_to_world`
 *
 * `snap_matrix` carries the surface orientation + user rotation + user scale set during hover.
 * `collection_center` is the mean of evaluated object origins so the whole collection lands at
 * the snap location.
 */
static wmOperatorStatus sculpt_collection_drop_exec(bContext *C,
                                                    wmOperator *op,
                                                    Object &active_ob,
                                                    ViewLayer &view_layer,
                                                    Main *bmain,
                                                    const float4x4 *snap_matrix,
                                                    const bool join_to_active,
                                                    const bool replace_face_sets,
                                                    const bool keep_source,
                                                    const bool apply_mask,
                                                    Object *&r_target_ob)
{
  const uint32_t collection_uid = uint32_t(RNA_int_get(op->ptr, "collection_uid"));
  Collection *collection = reinterpret_cast<Collection *>(
      BKE_libblock_find_session_uid(bmain, ID_GR, collection_uid));
  if (!collection) {
    BKE_report(op->reports, RPT_ERROR, "Dropped collection not found");
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  /* Use the evaluated active object's transform: the original (non-evaluated) transform can be
   * stale if it is driven by a constraint, driver, or an animated parent. Only needed when
   * joining; a separate object is placed directly at `snap_matrix`. */
  const Object *active_ob_eval = join_to_active ? DEG_get_evaluated(depsgraph, &active_ob) :
                                                  nullptr;

  /* Compute collection center as the mean of evaluated mesh-object origins. */
  float3 collection_center{0.0f};
  int mesh_count = 0;
  for (CollectionObject *cob = static_cast<CollectionObject *>(collection->gobject.first); cob;
       cob = cob->next)
  {
    if (!cob->ob || cob->ob->type != OB_MESH) {
      continue;
    }
    const Object *eval_ob = DEG_get_evaluated(depsgraph, cob->ob);
    collection_center += float3(eval_ob->object_to_world().location());
    mesh_count++;
  }

  if (mesh_count == 0) {
    BKE_report(op->reports, RPT_ERROR, "Dropped collection contains no mesh objects");
    return OPERATOR_CANCELLED;
  }
  collection_center /= float(mesh_count);

  /* Build a translation matrix that moves world-space vertices by -collection_center. */
  float4x4 translate_neg_center = float4x4::identity();
  translate_neg_center.location() = -collection_center;

  /* When joining, ensure the active mesh has face sets and assign fresh IDs to the dropped
   * geometry above the active mesh's IDs, so the active mesh's own sets are never touched. When
   * building a separate object there is no active mesh to avoid colliding with, so IDs simply
   * start at 0. In replace mode the whole dropped collection shares one new set (`base_id`);
   * otherwise a running offset gives every mesh its own non-colliding range (mirrors
   * #join_face_sets). */
  int base_id = 0;
  if (join_to_active) {
    face_set::create_face_sets_mesh(active_ob);
    base_id = face_set::find_next_available_id(active_ob);
  }
  int face_set_offset = base_id;

  /* Copy, transform, and collect geometry from every mesh object in the collection. */
  Vector<Mesh *> temp_meshes;
  Vector<bke::GeometrySet> geosets;

  for (CollectionObject *cob = static_cast<CollectionObject *>(collection->gobject.first); cob;
       cob = cob->next)
  {
    if (!cob->ob || cob->ob->type != OB_MESH) {
      continue;
    }
    const Object *eval_ob = DEG_get_evaluated(depsgraph, cob->ob);
    const Mesh *eval_mesh = BKE_object_get_evaluated_mesh(eval_ob);
    if (!eval_mesh || eval_mesh->faces_num == 0) {
      continue;
    }

    Mesh *mesh_copy = BKE_mesh_copy_for_eval(*eval_mesh);
    /* Replace: collapse this mesh into the shared `base_id` set. Otherwise preserve its own sets,
     * shifting them past everything used so far via the running offset. */
    if (replace_face_sets) {
      prepare_asset_face_sets(*mesh_copy, base_id, true);
    }
    else {
      face_set_offset = prepare_asset_face_sets(*mesh_copy, face_set_offset, false);
    }

    /* Joining: bake the full placement (active-local space + snap) into the mesh, since the
     * active object's own transform is left untouched. Separate object: the new object's own
     * transform carries `snap_matrix` (applied once, after the join, via #BKE_object_apply_mat4
     * below), so the mesh only needs the collection-center offset here — baking `snap_matrix` into
     * both would place the geometry twice over. */
    float4x4 transform;
    if (join_to_active) {
      transform = snap_matrix ? active_ob_eval->world_to_object() * (*snap_matrix) *
                                    translate_neg_center * eval_ob->object_to_world() :
                                active_ob_eval->world_to_object() * eval_ob->object_to_world();
    }
    else {
      transform = snap_matrix ? translate_neg_center * eval_ob->object_to_world() :
                                eval_ob->object_to_world();
    }
    bke::mesh_transform(*mesh_copy, transform, false);

    temp_meshes.append(mesh_copy);
    geosets.append(bke::GeometrySet::from_mesh(mesh_copy, bke::GeometryOwnershipType::ReadOnly));
  }

  if (geosets.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Dropped collection has no usable mesh geometry");
    return OPERATOR_CANCELLED;
  }

  Object *target_ob = &active_ob;
  Mesh *target_mesh = nullptr;

  if (join_to_active) {
    target_mesh = id_cast<Mesh *>(active_ob.data);

    /* Prepend the sculpt mesh so it participates in the join. */
    Vector<bke::GeometrySet> all_geosets;
    all_geosets.reserve(1 + geosets.size());
    all_geosets.append(
        bke::GeometrySet::from_mesh(target_mesh, bke::GeometryOwnershipType::ReadOnly));
    for (bke::GeometrySet &gs : geosets) {
      all_geosets.append(std::move(gs));
    }

    /* Vertex count of the active mesh before the join; the appended collection vertices start
     * here. */
    const int existing_verts_num = target_mesh->verts_num;

    bke::GeometrySet joined = geometry::join_geometries(all_geosets.as_span(), {});
    Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
    BKE_mesh_nomain_to_mesh(result, target_mesh, &active_ob);

    /* Mask the pre-existing geometry so only the dropped collection is left sculptable. Skipped
     * entirely when the user disabled Mask during hover, leaving any existing mask untouched. */
    if (apply_mask) {
      mask_existing_geometry(*target_mesh, existing_verts_num);
    }

    BKE_sculptsession_free_pbvh(active_ob);
    BKE_mesh_batch_cache_dirty_tag(target_mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&active_ob.id, ID_RECALC_GEOMETRY);
  }
  else {
    /* Build a new, separate object holding the merged collection geometry, placed directly at
     * `snap_matrix` (or the identity, when no snap was in effect and the meshes already carry
     * their own world-space transform). The active sculpt object and mesh are left untouched. */
    bke::GeometrySet joined = geometry::join_geometries(geosets.as_span(), {});
    Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();

    Object *new_ob = BKE_object_add_only_object(bmain, OB_MESH, collection->id.name + 2);
    new_ob->data = static_cast<ID *>(
        BKE_object_obdata_add_from_type(bmain, OB_MESH, new_ob->id.name + 2));
    LayerCollection *layer_collection = BKE_layer_collection_get_active(&view_layer);
    BKE_collection_viewlayer_object_add(bmain, &view_layer, layer_collection->collection, new_ob);

    target_mesh = id_cast<Mesh *>(new_ob->data);
    BKE_mesh_nomain_to_mesh(result, target_mesh, new_ob);

    if (snap_matrix) {
      BKE_object_apply_mat4(new_ob, snap_matrix->ptr(), false, true);
    }

    BKE_mesh_batch_cache_dirty_tag(target_mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);

    /* Mask the active sculpt mesh's pre-existing geometry entirely, mirroring the single-object
     * separate path: the dropped-in geometry lives in its own object here, so "everything except
     * the new geometry" is the whole active sculpt object. Skipped when Mask was disabled during
     * hover. */
    if (apply_mask) {
      Mesh &active_mesh = *id_cast<Mesh *>(active_ob.data);
      mask_existing_geometry(active_mesh, active_mesh.verts_num);
      BKE_sculptsession_free_pbvh(active_ob);
      BKE_mesh_batch_cache_dirty_tag(&active_mesh, BKE_MESH_BATCH_DIRTY_ALL);
      DEG_id_tag_update(&active_ob.id, ID_RECALC_GEOMETRY);
    }

    target_ob = new_ob;
  }

  for (Mesh *m : temp_meshes) {
    BKE_id_free(nullptr, m);
  }

  if (!keep_source) {
    BKE_collection_delete(bmain, collection, true);
  }

  /* Plain #ED_undo_push, not the sculpt geometry undo type: this step can also add/remove the
   * dropped collection or a whole new object, which that undo type cannot represent. See the
   * matching comment in #sculpt_mesh_asset_drop_exec. */
  ED_undo_push(C, op->type->name);

  r_target_ob = target_ob;
  return OPERATOR_FINISHED;
}

/** \} */

static wmOperatorStatus sculpt_mesh_asset_drop_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  Object *active_ob = CTX_data_active_object(C);
  if (!active_ob || active_ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "No active sculpt mesh");
    return OPERATOR_CANCELLED;
  }

  /* Plain mesh sculpt target only (no multires/dyntopo). Checked directly on the mesh/modifier
   * state rather than the PBVH, since the PBVH may not be built yet (e.g. right after entering
   * Sculpt Mode, before any stroke or redraw). */
  const bool is_dyntopo = active_ob->runtime->sculpt_session &&
                          active_ob->runtime->sculpt_session->bm;
  if (is_dyntopo || BKE_sculpt_multires_active(scene, active_ob)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Dropping mesh assets is only supported on plain meshes (no multires or dynamic "
               "topology)");
    return OPERATOR_CANCELLED;
  }

  float4x4 snap_matrix;
  const bool has_snap = snap_matrix_get(*op, snap_matrix);
  const bool join_to_active = RNA_boolean_get(op->ptr, "join_to_active");
  const bool replace_face_sets = RNA_boolean_get(op->ptr, "replace_face_sets");
  const bool keep_source = RNA_boolean_get(op->ptr, "keep_source");
  const bool apply_mask = RNA_boolean_get(op->ptr, "apply_mask");

  /* Collection drop path: exec was triggered by the sculpt collection dropbox. */
  const uint32_t collection_uid = uint32_t(RNA_int_get(op->ptr, "collection_uid"));
  if (collection_uid != 0) {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Object *target_ob = active_ob;
    const wmOperatorStatus status = sculpt_collection_drop_exec(C,
                                                                op,
                                                                *active_ob,
                                                                *view_layer,
                                                                bmain,
                                                                has_snap ? &snap_matrix : nullptr,
                                                                join_to_active,
                                                                replace_face_sets,
                                                                keep_source,
                                                                apply_mask,
                                                                target_ob);
    if (status == OPERATOR_FINISHED) {
      refresh_sculpt_overlays_after_drop(*C, *op, *active_ob);
      DEG_relations_tag_update(bmain);
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, target_ob);
      WM_event_add_notifier(C, NC_GEOM | ND_DATA, target_ob->data);
      WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, const_cast<Scene *>(scene));
    }
    return status;
  }

  /* Single object drop path. */
  Object *asset_ob = dropped_object_lookup(*bmain, *op);
  if (!asset_ob || asset_ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "Dropped asset is not a mesh object");
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  if (join_to_active) {
    Mesh *asset_mesh = asset_mesh_in_active_space(
        *depsgraph, *active_ob, *asset_ob, has_snap ? &snap_matrix : nullptr);
    if (!asset_mesh) {
      BKE_report(op->reports, RPT_ERROR, "Dropped asset has no faces to join");
      return OPERATOR_CANCELLED;
    }

    join_asset_into_active(*active_ob, *asset_mesh, replace_face_sets, apply_mask);
    BKE_id_free(nullptr, asset_mesh);

    /* Remove the carrier object from the scene only if it was imported for this drop.
     * Local assets already existed in the file and must not be deleted. */
    if (!keep_source) {
      ed::object::base_free_and_unlink(bmain, const_cast<Scene *>(scene), asset_ob);
    }

    /* A plain #ED_undo_push (rather than #ed::sculpt_paint::undo::geometry_begin/geometry_end) is
     * used here: this step can also remove/duplicate a separate object (the asset carrier),
     * which the sculpt-mode geometry undo type cannot represent (it only diffs the active mesh's
     * node geometry). Mirrors the same fallback #ed::sculpt_paint::undo::push_multires_mesh_end
     * uses when a change doesn't fit the sculpt undo model. Without this, redo-panel tweaks (e.g.
     * toggling Join to Active/Keep Source) fail: #ED_undo_operator_repeat() reverts only the last
     * pushed step, so an untracked object deletion would stay applied and the next exec() would
     * try to look up an object that no longer exists. */
    ED_undo_push(C, op->type->name);

    refresh_sculpt_overlays_after_drop(*C, *op, *active_ob);
    DEG_relations_tag_update(bmain);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, active_ob);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, active_ob->data);
    WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, const_cast<Scene *>(scene));
    return OPERATOR_FINISHED;
  }

  /* Separate object: add the asset as an independent object without touching the sculpt mesh.
   *
   * For a local asset (`keep_source`) the looked-up object is the original already living in the
   * scene, so create a linked duplicate and place that instead — moving the original would
   * displace the user's existing object. A freshly imported external asset is already a new
   * object, so it is placed directly. Mirrors #OBJECT_OT_add_named. */
  Object *place_ob = asset_ob;
  if (keep_source) {
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
    if (Base *base = BKE_view_layer_base_find(view_layer, asset_ob)) {
      /* Linked (`dupflag == 0`): the duplicate shares the original's data. Otherwise fall back to
       * the user's preferences for a full independent copy, matching #OBJECT_OT_add_named. */
      const bool linked = RNA_boolean_get(op->ptr, "linked");
      const eDupli_ID_Flags dupflag = linked ? eDupli_ID_Flags{} : eDupli_ID_Flags(U.dupflag);
      if (Base *dupe_base = ed::object::add_duplicate(
              bmain, const_cast<Scene *>(scene), view_layer, base, dupflag))
      {
        place_ob = dupe_base->object;
      }
    }
  }

  /* The snap matrix already is the desired world placement, so apply it directly. */
  if (has_snap) {
    BKE_object_apply_mat4(place_ob, snap_matrix.ptr(), false, true);
    DEG_id_tag_update(&place_ob->id, ID_RECALC_TRANSFORM);
  }

  /* The dropped asset stays in its own object here, so "mask everything except the new geometry"
   * means masking the whole active sculpt mesh; free the PBVH so it rebuilds with the new mask.
   * Skipped entirely when the user disabled Mask during hover, leaving any existing mask
   * untouched. */
  if (apply_mask) {
    Mesh &active_mesh = *id_cast<Mesh *>(active_ob->data);
    mask_existing_geometry(active_mesh, active_mesh.verts_num);
    BKE_sculptsession_free_pbvh(*active_ob);
    BKE_mesh_batch_cache_dirty_tag(&active_mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&active_ob->id, ID_RECALC_GEOMETRY);
  }

  /* Plain #ED_undo_push, not the sculpt geometry undo type: this path adds/duplicates a separate
   * object, which that undo type cannot represent. See the matching comment in the join branch
   * above. */
  ED_undo_push(C, op->type->name);

  refresh_sculpt_overlays_after_drop(*C, *op, *active_ob);
  DEG_relations_tag_update(bmain);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, place_ob);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, const_cast<Scene *>(scene));
  return OPERATOR_FINISHED;
}

/**
 * Informational only: these properties are decided interactively during hover (J/F/L keys, see
 * #view3d_sculpt_on_event_while_hover in view3d_dropboxes.cc) and handed to the operator by
 * #copy before it runs. The panel is shown disabled (not #layout.enabled_set) because exec() only
 * ever runs once — the operator's redo panel cannot safely re-run it with different property
 * values, since by the time redo would fire, exec() may have already joined, duplicated, or
 * deleted the referenced object/collection (see the file-level comment above for the underlying
 * undo limitation).
 */
static void sculpt_mesh_asset_drop_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  layout.enabled_set(false);

  const bool join = RNA_boolean_get(op->ptr, "join_to_active");
  layout.prop(op->ptr, "join_to_active", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  /* Only relevant for the separate-object path, which duplicates a local asset. */
  ui::Layout &linked_col = layout.column(false);
  linked_col.active_set(!join);
  linked_col.prop(op->ptr, "linked", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &col = layout.column(false);
  col.active_set(join);
  col.prop(op->ptr, "replace_face_sets", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.prop(op->ptr, "apply_mask", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void SCULPT_OT_mesh_asset_drop(wmOperatorType *ot)
{
  ot->name = "Drop Mesh Asset (Sculpt)";
  ot->description = "Add a mesh-object asset to the active sculpt object";
  ot->idname = "SCULPT_OT_mesh_asset_drop";

  ot->exec = sculpt_mesh_asset_drop_exec;
  ot->poll = sculpt_mesh_asset_drop_poll;
  ot->ui = sculpt_mesh_asset_drop_ui;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop;
  prop = RNA_def_int(ot->srna,
                     "session_uid",
                     0,
                     INT32_MIN,
                     INT32_MAX,
                     "Session UUID",
                     "Session UUID of the imported asset object to place",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_float_matrix(
      ot->srna, "matrix", 4, 4, nullptr, 0.0f, 0.0f, "Matrix", "", 0.0f, 0.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "join_to_active",
                         true,
                         "Join to Active",
                         "Merge the dropped mesh into the active sculpt object instead of adding "
                         "it as a separate object");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "replace_face_sets",
                         false,
                         "Replace Face Sets",
                         "Collapse the dropped mesh into a single new face set instead of keeping "
                         "its own face sets (the active mesh's face sets are always preserved)");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "linked",
                         true,
                         "Linked",
                         "When adding a local asset as a separate object, share (link) its mesh "
                         "data with the original instead of making a full independent copy");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "apply_mask",
                         false,
                         "Mask",
                         "Mask the active mesh's pre-existing geometry so only the dropped-in "
                         "geometry stays sculptable; when off, its existing mask is left "
                         "untouched");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "keep_source",
                         false,
                         "Keep Source",
                         "Keep the source object in the scene after joining its mesh "
                         "(used for local asset drops where the object already existed)");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_int(ot->srna,
                     "collection_uid",
                     0,
                     INT32_MIN,
                     INT32_MAX,
                     "Collection Session UUID",
                     "Session UUID of the dropped collection (non-zero activates the collection "
                     "drop path instead of the single-object path)",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::asset_drop
