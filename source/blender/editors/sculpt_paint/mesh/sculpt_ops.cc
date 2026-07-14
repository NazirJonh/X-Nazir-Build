/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode tools.
 */

#include "MEM_guardedalloc.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "BLT_translation.hh"

#include "DNA_brush_types.h"
#include "DNA_listBase.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mirror.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph.hh"

#include "IMB_colormanagement.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"

#include "../paint_intern.hh"
#include "mesh_brush_common.hh"
#include "paint_mask.hh"
#include "sculpt_automask.hh"
#include "sculpt_color.hh"
#include "sculpt_dyntopo.hh"
#include "sculpt_flood_fill.hh"
#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstring>

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Set Persistent Base Operator
 * \{ */

static wmOperatorStatus set_persistent_base_exec(bContext *C, wmOperator * /*op*/)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession *ss = ob.runtime->sculpt_session;

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  if (!ss) {
    return OPERATOR_CANCELLED;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  switch (bke::object::pbvh_get(ob)->type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(ob.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      attributes.remove(".sculpt_persistent_co");
      attributes.remove(".sculpt_persistent_no");
      attributes.remove(".sculpt_persistent_disp");

      const bke::AttributeReader positions = attributes.lookup<float3>("position");
      if (positions.sharing_info && positions.varray.is_span()) {
        attributes.add<float3>(
            ".sculpt_persistent_co",
            bke::AttrDomain::Point,
            bke::AttributeInitShared(positions.varray.get_internal_span().data(),
                                     *positions.sharing_info));
      }
      else {
        attributes.add<float3>(".sculpt_persistent_co",
                               bke::AttrDomain::Point,
                               bke::AttributeInitVArray(positions.varray));
      }

      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(*depsgraph, ob);
      attributes.add<float3>(".sculpt_persistent_no",
                             bke::AttrDomain::Point,
                             bke::AttributeInitVArray(VArray<float3>::from_span(vert_normals)));
      break;
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
      ss->persistent.sculpt_persistent_co = subdiv_ccg.positions;
      ss->persistent.sculpt_persistent_no = subdiv_ccg.normals;
      ss->persistent.sculpt_persistent_disp = Array<float>(subdiv_ccg.positions.size(), 0.0f);
      ss->persistent.grid_size = subdiv_ccg.grid_size;
      ss->persistent.grids_num = subdiv_ccg.grids_num;
      break;
    }
    case bke::pbvh::Type::BMesh: {
      return OPERATOR_CANCELLED;
    }
  }

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_set_persistent_base(wmOperatorType *ot)
{
  ot->name = "Set Persistent Base";
  ot->idname = "SCULPT_OT_set_persistent_base";
  ot->description = "Reset the copy of the mesh that is being sculpted on";

  ot->exec = set_persistent_base_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Optimize Operator
 * \{ */

static wmOperatorStatus optimize_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);

  BKE_sculptsession_free_pbvh(ob);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);

  return OPERATOR_FINISHED;
}

/* The BVH gets less optimal more quickly with dynamic topology than
 * regular sculpting. There is no doubt more clever stuff we can do to
 * optimize it on the fly, but for now this gives the user a nicer way
 * to recalculate it than toggling modes. */
static void SCULPT_OT_optimize(wmOperatorType *ot)
{
  ot->name = "Rebuild BVH";
  ot->idname = "SCULPT_OT_optimize";
  ot->description = "Recalculate the sculpt BVH to improve performance";

  ot->exec = optimize_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Symmetrize Operator
 * \{ */

static bool no_multires_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob);
  if (sculpt_mode_poll(C) && ob->runtime->sculpt_session && pbvh) {
    return pbvh->type() != bke::pbvh::Type::Grids;
  }
  return false;
}

static wmOperatorStatus symmetrize_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const Scene &scene = *CTX_data_scene(C);
  const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  const float dist = RNA_float_get(op->ptr, "merge_tolerance");

  if (!pbvh) {
    return OPERATOR_CANCELLED;
  }

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  /* A weight-mask editing session must be refused in its own right, not left to the bake check
   * below: #destructive_edit_check counts *layers* (see #bke::sculpt_layers::layers, which never
   * collects folders), while a session can be anchored on a folder — including an empty one. On a
   * mesh carrying no layers at all, an empty folder holding a mask therefore passes the bake check
   * while a session is open, and the changed element count then sends #mask_edit_end down its
   * destructive branch, which removes the user's own `.sculpt_mask` for good. Left un-gated by
   * `ss.bm`: a session cannot legitimately exist under dyntopo, and refusing is the safe answer if
   * one somehow does. */
  if (layers::mask_edit_refuse_deform(ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  /* Mirroring changes the element count, which leaves every sculpt layer describing a domain that no
   * longer exists — the same reason trim, dyntopo and the remesh operators refuse. Conditioned on
   * `!ss.bm` exactly as #sculpt_dyntopo.cc's own check is: under dyntopo the live geometry is the
   * BMesh and the stored layers do not describe it. */
  if (!ss.bm && !layers::destructive_edit_check(*id_cast<const Mesh *>(ob.data), op->reports)) {
    return OPERATOR_CANCELLED;
  }

  switch (pbvh->type()) {
    case bke::pbvh::Type::BMesh: {
      /* Dyntopo Symmetrize. */

      /* To simplify undo for symmetrize, all BMesh elements are logged
       * as deleted, then after symmetrize operation all BMesh elements
       * are logged as added (as opposed to attempting to store just the
       * parts that symmetrize modifies). */
      undo::push_begin(scene, ob, op);
      undo::push_node(depsgraph, ob, nullptr, undo::Type::Geometry);
      BM_log_before_all_removed(ss.bm, ss.bm_log);

      BM_mesh_toolflags_set(ss.bm, true);

      /* Symmetrize and re-triangulate. */
      BMO_op_callf(ss.bm,
                   (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
                   "symmetrize input=%avef direction=%i dist=%f use_shapekey=%b",
                   sd.symmetrize_direction,
                   dist,
                   true);
      dyntopo::triangulate(ss.bm);

      /* Bisect operator flags edges (keep tags clean for edge queue). */
      BM_mesh_elem_hflag_disable_all(ss.bm, BM_EDGE, BM_ELEM_TAG, false);

      BM_mesh_toolflags_set(ss.bm, false);

      /* Finish undo. */
      BM_log_all_added(ss.bm, ss.bm_log);
      undo::push_end(ob);

      break;
    }
    case bke::pbvh::Type::Mesh: {
      /* Mesh Symmetrize. */
      undo::geometry_begin(scene, ob, op);
      Mesh *mesh = id_cast<Mesh *>(ob.data);

      BKE_mesh_mirror_apply_mirror_on_axis(bmain, mesh, sd.symmetrize_direction, dist);

      undo::geometry_end(ob);
      BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);

      break;
    }
    case bke::pbvh::Type::Grids:
      return OPERATOR_CANCELLED;
  }

  BKE_sculptsession_free_pbvh(ob);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_symmetrize(wmOperatorType *ot)
{
  ot->name = "Symmetrize";
  ot->idname = "SCULPT_OT_symmetrize";
  ot->description = "Symmetrize the topology modifications";

  ot->exec = symmetrize_exec;
  ot->poll = no_multires_poll;

  PropertyRNA *prop = RNA_def_float(ot->srna,
                                    "merge_tolerance",
                                    0.0005f,
                                    0.0f,
                                    std::numeric_limits<float>::max(),
                                    "Merge Distance",
                                    "Distance within which symmetrical vertices are merged",
                                    0.0f,
                                    1.0f);

  RNA_def_property_ui_range(prop, 0.0, std::numeric_limits<float>::max(), 0.001, 5);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Mode Toggle Operator
 * \{ */

static void init_sculpt_mode_session(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob)
{
  /* Create persistent sculpt mode data. */
  BKE_sculpt_toolsettings_data_ensure(&bmain, &scene);

  /* Create sculpt mode session data. */
  if (ob.runtime->sculpt_session != nullptr) {
    /* Entering sculpt mode over a session that was never exited — a script setting `object.mode`
     * directly, or a mode switch that skipped #object_sculpt_mode_exit. A leftover weight-mask
     * session would otherwise be discarded with the #SculptSession while its weights sat in the
     * mesh's `.sculpt_mask` attribute, permanently passing for the user's own mask. A no-op in the
     * ordinary case and for the vertex/weight paint sessions that also land here, which can never
     * have one open. */
    layers::mask_edit_end(ob);
    BKE_sculptsession_free(&ob);
  }
  ob.runtime->sculpt_session = MEM_new<SculptSession>(__func__);
  ob.runtime->sculpt_session->mode_type = OB_MODE_SCULPT;

  /* REC is restored from the mesh before the refresh below, and this is the only site that reads
   * #SCULPT_LAYER_REC_ARMED. The session is the authority for as long as it exists, but it was just
   * created with REC off, so the bit the previous session left behind is the only thing that knows
   * the user was recording when they stepped out into object mode.
   *
   * Strictly before #rec_exemption_refresh: that call mirrors the session onto the mesh, so running
   * it first would read "no REC" out of the fresh session and erase the very bit being read here.
   *
   * The question asked is "was REC on", not "was REC on *this* layer": the bit is looked for anywhere
   * in the tree and REC is then re-armed on whatever layer is active now. That is what makes changing
   * the active layer in object mode (#SCULPT_OT_layer_select is polled there) behave exactly as it
   * does inside sculpt mode, where #SculptSession::layers::rec_active is not tied to a layer at all
   * and #layer_select_exec simply lets the exemption follow the new active layer. Anything narrower
   * would make a trip through object mode silently switch recording off, which is the bug this whole
   * mirror exists to prevent.
   *
   * Restored only if the layer can still receive a stroke — the same refusal #layer_toggle_rec_exec
   * makes against arming REC on a folder-hidden layer, plus the lock, since both can be set from the
   * mesh properties UI while the object sits in object mode. When it cannot, REC simply stays off and
   * the refresh below clears the stale bit; the user sees an unpressed REC button rather than a
   * stroke silently landing in the base.
   *
   * Deliberately not routed through #rec_active_set: that call captures the runtime base, measures
   * the layer's mask, flushes multires and recomposes, none of which can run here (there is no PBVH
   * and no runtime base yet). It also pins the layer to enabled / influence 1.0, which must *not* be
   * repeated — the pin happened when REC was first armed, and an influence the user changed in the
   * meantime is an edit to respect, not to overwrite. The full geometry re-evaluation a few lines
   * below composes the surface from scratch under the answer the refresh settles. */
  if (ob.type == OB_MESH && ob.data != nullptr) {
    const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
    bool was_armed = false;
    for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
      if (layer->base.flag & SCULPT_LAYER_REC_ARMED) {
        was_armed = true;
        break;
      }
    }
    const SculptLayer *active = bke::sculpt_layers::active_get(mesh);
    if (was_armed && active != nullptr &&
        !(active->base.flag & (SCULPT_LAYER_GROUP_HIDDEN | SCULPT_LAYER_LOCKED)))
    {
      ob.runtime->sculpt_session->layers.rec_active = true;
    }
  }

  /* Every entry, not just the skipped-exit case above. #SCULPT_LAYER_REC_EXEMPT lives on the mesh's
   * own layer nodes, so it is not lost when the #SculptSession is, and any path that armed REC and
   * never reached #object_sculpt_mode_exit — a mode switch that bypassed the exit, a script
   * assigning `object.mode` — leaves this mesh with a layer still exempt while nothing is
   * recording. The freshly built session has no REC armed, so reading it back here is what makes
   * entering sculpt mode self-healing.
   *
   * Nothing here can recompose on the result, and nothing has to: there is no runtime base and no
   * PBVH yet at this point, and the full geometry re-evaluation two lines below composes the surface
   * from scratch under whatever answer this just settled. */
  layers::rec_exemption_refresh(ob);

  /* Trigger evaluation of modifier stack to ensure
   * multires modifier sets .runtime.ccg in
   * the evaluated mesh.
   */
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);

  BKE_scene_graph_evaluated_ensure(&depsgraph, &bmain);

  /* This function expects a fully evaluated depsgraph. */
  BKE_sculpt_update_object_for_edit(&depsgraph, &ob, false);

  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  if (mesh.attributes().contains(".sculpt_face_set")) {
    /* Here we can detect geometry that was just added to Sculpt Mode as it has the
     * face_set_none assigned, so we can create a new face set for it. */
    /* In sculpt mode all geometry that is assigned to face_set_none is considered as not
     * initialized, which is used is some operators that modify the mesh topology to perform
     * certain actions in the new faces. After these operations are finished, all faces should have
     * a valid face set ID assigned (different from face_set_none) to manage their
     * visibility correctly. */
    /* TODO(pablodp606): Based on this we can improve the UX in future tools for creating new
     * objects, like moving the transform pivot position to the new area or masking existing
     * geometry. */
    const int new_face_set = face_set::find_next_available_id(ob);
    face_set::initialize_none_to_id(id_cast<Mesh *>(ob.data), new_face_set);
  }
}

void ensure_valid_pivot(const Object &ob, Paint &paint)
{
  bke::PaintRuntime &paint_runtime = *paint.runtime;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);

  /* Account for the case where no objects are evaluated. */
  if (!pbvh) {
    return;
  }

  /* No valid pivot? Use bounding box center. */
  if (paint_runtime.average_stroke_counter == 0 || !paint_runtime.last_stroke_valid) {
    const Bounds<float3> bounds = bke::pbvh::bounds_get(*pbvh);
    const float3 center = math::midpoint(bounds.min, bounds.max);
    const float3 location = math::transform_point(ob.object_to_world(), center);

    copy_v3_v3(paint_runtime.average_stroke_accum, location);
    paint_runtime.average_stroke_counter = 1;

    /* Update last stroke position. */
    paint_runtime.last_stroke_valid = true;
  }
}

void object_sculpt_mode_enter(Main &bmain,
                              Depsgraph &depsgraph,
                              Scene &scene,
                              Object &ob,
                              const bool force_dyntopo,
                              ReportList *reports)
{
  const eObjectMode mode_flag = OB_MODE_SCULPT;
  Mesh *mesh = BKE_mesh_from_object(&ob);

  /* Re-triangulating the mesh for position changes in sculpt mode isn't worth the performance
   * impact, so delay triangulation updates until the user exits sculpt mode. */
  mesh->runtime->corner_tris_cache.freeze();

  /* Enter sculpt mode. */
  ob.mode |= mode_flag;

  init_sculpt_mode_session(bmain, depsgraph, scene, ob);

  if (!(fabsf(ob.scale[0] - ob.scale[1]) < 1e-4f && fabsf(ob.scale[1] - ob.scale[2]) < 1e-4f)) {
    BKE_report(
        reports, RPT_WARNING, "Object has non-uniform scale, sculpting may be unpredictable");
  }
  else if (is_negative_m4(ob.object_to_world().ptr())) {
    BKE_report(reports, RPT_WARNING, "Object has negative scale, sculpting may be unpredictable");
  }

  if (USER_EXPERIMENTAL_TEST(&U, use_sculpt_texture_paint)) {
    BKE_texpaint_slots_refresh_object(&scene, &ob);

    PaintModeSettings *paint_settings = &scene.toolsettings->paint_mode;
    Image *image;
    ImageUser *image_user;

    if (BKE_paint_canvas_image_get(paint_settings, &ob, &image, &image_user)) {
      ED_space_image_sync(&bmain, image, false);
    }
  }

  Paint *paint = BKE_paint_get_active_from_paintmode(&scene, PaintMode::Sculpt);
  BKE_paint_init(&bmain, &scene, PaintMode::Sculpt);

  ED_paint_cursor_start(paint, brush_cursor_poll);

  /* Check dynamic-topology flag; re-enter dynamic-topology mode when changing modes,
   * As long as no data was added that is not supported. */
  /* A saved file can pair the dyntopo flag with un-baked sculpt layers, a combination
   * #sculpt_dynamic_topology_toggle_exec refuses to create interactively. Entering sculpt mode must
   * still succeed, so only dyntopo is skipped here, and #ME_SCULPT_DYNAMIC_TOPOLOGY is deliberately
   * left set: the obstruction is temporary — baking the layers lifts it — and quietly rewriting DNA
   * during a mode switch would cost the user a saved setting for a reason never shown to them.
   * Unlike the unsupported-data cases below, this is not overridable by \a force_dyntopo, which
   * exists to push past attribute warnings rather than past data that dyntopo would silently ignore:
   * under a BMesh every layer turns stale and each consumer skips it. */
  const bool layers_block_dyntopo = (mesh->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) &&
                                    !layers::destructive_edit_check(*mesh, nullptr);
  if (layers_block_dyntopo) {
    /* Reported here rather than by #destructive_edit_check, whose message names a destructive mesh
     * edit — the user only switched modes, and nothing in that text would connect the refusal to
     * dyntopo. */
    BKE_report(reports, RPT_INFO, "Dynamic Topology disabled: bake the sculpt layers first");
  }
  if ((mesh->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) && !layers_block_dyntopo) {
    MultiresModifierData *mmd = BKE_sculpt_multires_active(&scene, &ob);

    const char *message_unsupported = nullptr;
    if (mesh->corners_num != mesh->faces_num * 3) {
      message_unsupported = RPT_("non-triangle face");
    }
    else if (mmd != nullptr) {
      message_unsupported = RPT_("multi-res modifier");
    }
    else {
      const dyntopo::WarnFlag flag = dyntopo::check_attribute_warning(scene, ob);
      if (flag == 0) {
        /* pass */
      }
      else if (flag & dyntopo::ATTRIBUTES) {
        BKE_report(reports,
                   RPT_WARNING,
                   "Dyntopo will not preserve face sets, colors, UVs, or other attributes");
      }
      else if (flag & dyntopo::MODIFIER) {
        message_unsupported = RPT_("constructive modifier");
      }
      else {
        BLI_assert_unreachable();
      }
    }

    if ((message_unsupported == nullptr) || force_dyntopo) {
      /* Needed because we may be entering this mode before the undo system loads. */
      wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
      const bool has_undo = wm->runtime->undo_stack != nullptr;
      /* Undo push is needed to prevent memory leak. */
      if (has_undo) {
        undo::push_begin_ex(scene, ob, "Dynamic topology enable");
      }
      dyntopo::enable_ex(bmain, depsgraph, ob);
      if (has_undo) {
        undo::push_node(depsgraph, ob, nullptr, undo::Type::DyntopoBegin);
        undo::push_end(ob);
      }
    }
    else {
      BKE_reportf(
          reports, RPT_WARNING, "Dynamic Topology found: %s, disabled", message_unsupported);
      mesh->flag &= ~ME_SCULPT_DYNAMIC_TOPOLOGY;
    }
  }

  ensure_valid_pivot(ob, *paint);

  /* Capture the un-layered runtime base for the mesh (vertex) domain and validate grid-domain
   * layer data against the current top level. This has to happen while the geometry still matches
   * the stored per-layer influences, i.e. before the user can change anything. Only force-build
   * the BVH when layers actually exist. */
  if (mesh && !bke::sculpt_layers::layers(*mesh).is_empty()) {
    bke::object::pbvh_ensure(depsgraph, ob);
  }
  layers::session_state_ensure(ob);

  /* Flush object mode. */
  DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL);
}

void object_sculpt_mode_enter(bContext *C, Depsgraph &depsgraph, ReportList *reports)
{
  Main &bmain = *CTX_data_main(C);
  Scene &scene = *CTX_data_scene(C);
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Object &ob = *BKE_view_layer_active_object_get(&view_layer);
  object_sculpt_mode_enter(bmain, depsgraph, scene, ob, false, reports);
}

void object_sculpt_mode_exit(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob)
{
  const eObjectMode mode_flag = OB_MODE_SCULPT;
  Mesh *mesh = BKE_mesh_from_object(&ob);

  mesh->runtime->corner_tris_cache.unfreeze();

  /* Close any open weight-mask editing session BEFORE the flush below, and this ordering is
   * load-bearing rather than incidental: while a session is open the layer's weights occupy
   * #SubdivCCG::masks, and #multires_flush_sculpt_updates copies that array straight into the base
   * mesh's persistent `CD_GRID_PAINT_MASK` layer. Flushing first would overwrite the user's paint
   * mask with the layer's weights permanently — the session's own restore cannot undo a write to
   * CustomData, and the next CCG rebuild re-derives `masks` from the corrupted base layer.
   *
   * This is also the last point at which the session *can* be closed. #BKE_sculptsession_free below
   * is blenkernel and must not call into the editors, and by the time it runs the derived caches
   * that carry the CCG are about to go; here the mesh, the attribute API, the #SubdivCCG and the
   * tree are all still alive. */
  layers::mask_edit_end(ob);

  multires_flush_sculpt_updates(&ob);

  /* Not needed for now. */
#if 0
  MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, ob);
  const int flush_recalc = ed_object_sculptmode_flush_recalc_flag(scene, ob, mmd);
#endif

  /* Always for now, so leaving sculpt mode always ensures scene is in
   * a consistent state. */
  if (true || /* flush_recalc || */ (ob.runtime->sculpt_session && ob.runtime->sculpt_session->bm))
  {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }

  if (mesh->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) {
    /* Dynamic topology must be disabled before exiting sculpt
     * mode to ensure the undo stack stays in a consistent
     * state. */
    dyntopo::disable_with_undo(bmain, depsgraph, scene, ob);

    /* Store so we know to re-enable when entering sculpt mode. */
    mesh->flag |= ME_SCULPT_DYNAMIC_TOPOLOGY;
  }

  /* Leave sculpt mode. */
  ob.mode &= ~mode_flag;

  BKE_sculptsession_free(&ob);

  /* After the session is gone, which is what makes this a *clear*: with no session to mirror, the
   * refresh drops the exemption. It has to be dropped, because
   * #SCULPT_LAYER_REC_EXEMPT lives on the mesh's layer nodes rather than on the session and would
   * otherwise outlive the mode — leaving this mesh rendering, and every later operator composing,
   * with a layer whose weight map is silently absent.
   *
   * This is the one caller that *cannot* recompose on the result even in principle: the session it
   * would need is already freed above, so there is no runtime base to recompose from. It does not
   * have to. The mode exit tears the sculpt PBVH down and tags the object for a full re-evaluation
   * below, so the next composed surface is built from stored data under the settled answer.
   *
   * #SCULPT_LAYER_REC_ARMED goes the other way and is left exactly as it stands. It composes nothing,
   * so it is harmless outside the mode, and it is the only surviving record that REC was on —
   * #init_sculpt_mode_session reads it back, which is what stops a trip through object mode from
   * silently switching recording off. */
  layers::rec_exemption_refresh(ob);

  paint_cursor_delete_textures();

  /* Never leave derived meshes behind. */
  BKE_object_free_derived_caches(&ob);

  /* Flush object mode. */
  DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL);
}

void object_sculpt_mode_exit(bContext *C, Depsgraph &depsgraph)
{
  Main &bmain = *CTX_data_main(C);
  Scene &scene = *CTX_data_scene(C);
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Object &ob = *BKE_view_layer_active_object_get(&view_layer);
  object_sculpt_mode_exit(bmain, depsgraph, scene, ob);
}

static wmOperatorStatus sculpt_mode_toggle_exec(bContext *C, wmOperator *op)
{
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  Main &bmain = *CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  Scene &scene = *CTX_data_scene(C);
  ToolSettings &ts = *scene.toolsettings;
  ViewLayer &view_layer = *CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Object &ob = *BKE_view_layer_active_object_get(&view_layer);
  const eObjectMode mode_flag = OB_MODE_SCULPT;
  const bool is_mode_set = (ob.mode & mode_flag) != 0;

  if (!is_mode_set) {
    if (!object::mode_compat_set(C, &ob, eObjectMode(mode_flag), op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  if (is_mode_set) {
    object_sculpt_mode_exit(bmain, *depsgraph, scene, ob);
  }
  else {
    if (depsgraph) {
      depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    }
    object_sculpt_mode_enter(bmain, *depsgraph, scene, ob, false, op->reports);
    BKE_paint_brushes_validate(&bmain, &ts.sculpt->paint);

    if (ob.mode & mode_flag) {
      /* Dyntopo adds its own undo step. Asked of the live session rather than of
       * #ME_SCULPT_DYNAMIC_TOPOLOGY: the flag is only a request. #object_sculpt_mode_enter leaves it
       * set while skipping dyntopo when un-baked sculpt layers block it, so the flag no longer
       * answers "did dyntopo actually turn on" — and trusting it there would skip this push as well,
       * leaving the mode switch with no sculpt undo step at all. */
      const SculptSession *ss = ob.runtime->sculpt_session;
      if (ss == nullptr || ss->bm == nullptr) {
        /* Without this the memfile undo step is used,
         * while it works it causes lag when undoing the first undo step, see #71564. */
        wmWindowManager *wm = CTX_wm_manager(C);
        if (wm->op_undo_depth <= 1) {
          undo::push_enter_sculpt_mode(scene, ob, op);
          undo::push_end(ob);
        }
      }
    }
  }

  WM_event_add_notifier(C, NC_SCENE | ND_MODE, &scene);

  WM_msg_publish_rna_prop(mbus, &ob.id, &ob, Object, mode);

  WM_toolsystem_update_from_context_view3d(C);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_sculptmode_toggle(wmOperatorType *ot)
{
  ot->name = "Sculpt Mode";
  ot->idname = "SCULPT_OT_sculptmode_toggle";
  ot->description = "Toggle sculpt mode in 3D view";

  ot->exec = sculpt_mode_toggle_exec;
  ot->poll = ED_operator_object_active_editable_mesh_from_view_layer;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

namespace mask {

/* -------------------------------------------------------------------- */
/** \name Mask By Color
 * \{ */

/**
 * #sculpt_mask_by_color_delta_get returns values in the (0,1) range that are used to generate the
 * mask based on the difference between two colors (the active color and the color of any other
 * vertex). Ideally, a threshold of 0 should mask only the colors that are equal to the active
 * color and threshold of 1 should mask all colors. In order to avoid artifacts and produce softer
 * falloffs in the mask, the MASK_BY_COLOR_SLOPE defines the size of the transition values between
 * masked and unmasked vertices. The smaller this value is, the sharper the generated mask is going
 * to be.
 */
#define MASK_BY_COLOR_SLOPE 0.25f

static float color_delta_get(const float3 &color_a,
                             const float3 &color_b,
                             const float threshold,
                             const bool invert)
{
  float len = math::distance(color_a, color_b);
  /* Normalize len to the (0, 1) range. */
  len = len / std::numbers::sqrt3_v<float>;

  if (len < threshold - MASK_BY_COLOR_SLOPE) {
    len = 1.0f;
  }
  else if (len >= threshold) {
    len = 0.0f;
  }
  else {
    len = (-len + threshold) / MASK_BY_COLOR_SLOPE;
  }

  if (invert) {
    return 1.0f - len;
  }
  return len;
}

static float final_mask_get(const float current_mask,
                            const float new_mask,
                            const bool invert,
                            const bool preserve_mask)
{
  if (preserve_mask) {
    if (invert) {
      return std::min(current_mask, new_mask);
    }
    return std::max(current_mask, new_mask);
  }
  return new_mask;
}

static void mask_by_color_contiguous_mesh(const Depsgraph &depsgraph,
                                          Object &object,
                                          const int vert,
                                          const float threshold,
                                          const bool invert,
                                          const bool preserve_mask)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan colors = *attributes.lookup_or_default<ColorGeometry4f>(
      mesh.active_color_attribute, bke::AttrDomain::Point, {});
  const float4 active_color = float4(colors[vert]);

  Array<float> new_mask(mesh.verts_num, invert ? 1.0f : 0.0f);

  flood_fill::FillDataMesh flood(mesh.verts_num);
  flood.add_initial(vert);

  flood.execute(object, vert_to_face_map, [&](int /*from_v*/, int to_v) {
    const float4 current_color = float4(colors[to_v]);

    float new_vertex_mask = color_delta_get(
        current_color.xyz(), active_color.xyz(), threshold, invert);
    new_mask[to_v] = new_vertex_mask;

    float len = math::distance(current_color.xyz(), active_color.xyz());
    len = len / std::numbers::sqrt3_v<float>;
    return len <= threshold;
  });

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  update_mask_mesh(
      depsgraph, object, node_mask, [&](MutableSpan<float> node_masks, const Span<int> verts) {
        for (const int i : verts.index_range()) {
          node_masks[i] = final_mask_get(node_masks[i], new_mask[verts[i]], invert, preserve_mask);
        }
      });
}

static void mask_by_color_full_mesh(const Depsgraph &depsgraph,
                                    Object &object,
                                    const int vert,
                                    const float threshold,
                                    const bool invert,
                                    const bool preserve_mask)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan colors = *attributes.lookup_or_default<ColorGeometry4f>(
      mesh.active_color_attribute, bke::AttrDomain::Point, {});
  const float4 active_color = float4(colors[vert]);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  update_mask_mesh(
      depsgraph, object, node_mask, [&](MutableSpan<float> node_masks, const Span<int> verts) {
        for (const int i : verts.index_range()) {
          const float4 current_color = float4(colors[verts[i]]);
          const float current_mask = node_masks[i];
          const float new_mask = color_delta_get(
              active_color.xyz(), current_color.xyz(), threshold, invert);
          node_masks[i] = final_mask_get(current_mask, new_mask, invert, preserve_mask);
        }
      });
}

static wmOperatorStatus mask_by_color(bContext *C, wmOperator *op, const float2 region_location)
{
  const Scene &scene = *CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.runtime->sculpt_session;
  View3D *v3d = CTX_wm_view3d(C);

  {
    if (v3d && v3d->shading.type == OB_SOLID) {
      v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
    }
  }

  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  ed::sculpt_paint::mask_overlay_check(*C, *op);

  /* Color data is not available in multi-resolution or dynamic topology. */
  if (!color_supported_check(scene, ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  /* Tools that are not brushes do not have the brush gizmo to update the vertex as the mouse move,
   * so it needs to be updated here. */
  cursor_geometry_info_update(C, region_location, false);

  if (std::holds_alternative<std::monostate>(ss.active_vert())) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);
  BKE_sculpt_color_layer_create_if_needed(&ob);

  const float threshold = RNA_float_get(op->ptr, "threshold");
  const bool invert = RNA_boolean_get(op->ptr, "invert");
  const bool preserve_mask = RNA_boolean_get(op->ptr, "preserve_previous_mask");

  const int active_vert = std::get<int>(ss.active_vert());
  if (RNA_boolean_get(op->ptr, "contiguous")) {
    mask_by_color_contiguous_mesh(*depsgraph, ob, active_vert, threshold, invert, preserve_mask);
  }
  else {
    mask_by_color_full_mesh(*depsgraph, ob, active_vert, threshold, invert, preserve_mask);
  }

  undo::push_end(ob);

  flush_update_done(C, ob, UpdateType::Mask);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus mask_by_color_exec(bContext *C, wmOperator *op)
{
  int2 mval;
  RNA_int_get_array(op->ptr, "location", mval);
  return mask_by_color(C, op, float2(mval[0], mval[1]));
}

static wmOperatorStatus mask_by_color_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "location", event->mval);
  return mask_by_color(C, op, float2(event->mval[0], event->mval[1]));
}

static void SCULPT_OT_mask_by_color(wmOperatorType *ot)
{
  ot->name = "Mask by Color";
  ot->idname = "SCULPT_OT_mask_by_color";
  ot->description = "Creates a mask based on the active color attribute";

  ot->invoke = mask_by_color_invoke;
  ot->exec = mask_by_color_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  ot->prop = RNA_def_boolean(
      ot->srna, "contiguous", false, "Contiguous", "Mask only contiguous color areas");

  ot->prop = RNA_def_boolean(ot->srna, "invert", false, "Invert", "Invert the generated mask");
  ot->prop = RNA_def_boolean(
      ot->srna,
      "preserve_previous_mask",
      false,
      "Preserve Previous Mask",
      "Preserve the previous mask and add or subtract the new one generated by the colors");

  RNA_def_float(ot->srna,
                "threshold",
                0.35f,
                0.0f,
                1.0f,
                "Threshold",
                "How much changes in color affect the mask generation",
                0.0f,
                1.0f);

  ot->prop = RNA_def_int_array(ot->srna,
                               "location",
                               2,
                               nullptr,
                               0,
                               SHRT_MAX,
                               "Location",
                               "Region coordinates of sampling",
                               0,
                               SHRT_MAX);
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask from Cavity
 * \{ */

enum class ApplyMaskMode : int8_t {
  Mix,
  Multiply,
  Divide,
  Add,
  Subtract,
};

static EnumPropertyItem mix_modes[] = {
    {int(ApplyMaskMode::Mix), "MIX", ICON_NONE, "Mix", ""},
    {int(ApplyMaskMode::Multiply), "MULTIPLY", ICON_NONE, "Multiply", ""},
    {int(ApplyMaskMode::Divide), "DIVIDE", ICON_NONE, "Divide", ""},
    {int(ApplyMaskMode::Add), "ADD", ICON_NONE, "Add", ""},
    {int(ApplyMaskMode::Subtract), "SUBTRACT", ICON_NONE, "Subtract", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class MaskSettingsSource : int8_t { Operator, Scene, Brush };

static EnumPropertyItem settings_sources[] = {
    {int(MaskSettingsSource::Operator),
     "OPERATOR",
     ICON_NONE,
     "Operator",
     "Use settings from operator properties"},
    {int(MaskSettingsSource::Brush), "BRUSH", ICON_NONE, "Brush", "Use settings from brush"},
    {int(MaskSettingsSource::Scene), "SCENE", ICON_NONE, "Scene", "Use settings from scene"},
    {0, nullptr, 0, nullptr, nullptr}};

struct LocalData {
  Vector<float> mask;
  Vector<float> factors;
  Vector<float> new_mask;
};

static void calc_new_masks(const ApplyMaskMode mode,
                           const Span<float> node_mask,
                           const MutableSpan<float> new_mask)
{
  switch (mode) {
    case ApplyMaskMode::Mix:
      break;
    case ApplyMaskMode::Multiply:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = node_mask[i] * new_mask[i];
      }
      break;
    case ApplyMaskMode::Divide:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = new_mask[i] > 0.00001f ? node_mask[i] / new_mask[i] : 0.0f;
      }
      break;
    case ApplyMaskMode::Add:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = node_mask[i] + new_mask[i];
      }
      break;
    case ApplyMaskMode::Subtract:
      for (const int i : node_mask.index_range()) {
        new_mask[i] = node_mask[i] - new_mask[i];
      }
      break;
  }
  mask::clamp_mask(new_mask);
}

static void apply_mask_mesh(const Depsgraph &depsgraph,
                            const Object &object,
                            const Span<bool> hide_vert,
                            const auto_mask::Cache &automasking,
                            const ApplyMaskMode mode,
                            const float factor,
                            const bke::pbvh::MeshNode &node,
                            LocalData &tls,
                            const MutableSpan<float> mask)
{
  const Span<int> verts = node.verts();

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide(hide_vert, verts, factors);
  scale_factors(factors, factor);

  tls.new_mask.resize(verts.size());
  const MutableSpan<float> new_mask = tls.new_mask;
  new_mask.fill(1.0f);
  auto_mask::calc_vert_factors(depsgraph, object, automasking, node, verts, new_mask);

  mask::invert_mask(new_mask);

  tls.mask.resize(verts.size());
  const MutableSpan<float> node_mask = tls.mask;
  gather_data_mesh(mask.as_span(), verts, node_mask);

  calc_new_masks(mode, node_mask, new_mask);
  mix_new_masks(new_mask, factors, node_mask);

  scatter_data_mesh(node_mask.as_span(), verts, mask);
}

static void apply_mask_grids(const Depsgraph &depsgraph,
                             Object &object,
                             const auto_mask::Cache &automasking,
                             const ApplyMaskMode mode,
                             const float factor,
                             const bke::pbvh::GridsNode &node,
                             LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  const Span<int> grids = node.grids();
  const int grid_verts_num = grids.size() * key.grid_area;

  tls.factors.resize(grid_verts_num);
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide(subdiv_ccg, grids, factors);
  scale_factors(factors, factor);

  tls.new_mask.resize(grid_verts_num);
  const MutableSpan<float> new_mask = tls.new_mask;
  new_mask.fill(1.0f);
  auto_mask::calc_grids_factors(depsgraph, object, automasking, node, grids, new_mask);

  mask::invert_mask(new_mask);

  tls.mask.resize(grid_verts_num);
  const MutableSpan<float> node_mask = tls.mask;
  gather_mask_grids(subdiv_ccg, grids, node_mask);

  calc_new_masks(mode, node_mask, new_mask);
  mix_new_masks(new_mask, factors, node_mask);

  scatter_mask_grids(node_mask.as_span(), subdiv_ccg, grids);
}

static void apply_mask_bmesh(const Depsgraph &depsgraph,
                             Object &object,
                             const auto_mask::Cache &automasking,
                             const ApplyMaskMode mode,
                             const float factor,
                             bke::pbvh::BMeshNode &node,
                             LocalData &tls)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);

  tls.factors.resize(verts.size());
  const MutableSpan<float> factors = tls.factors;
  fill_factor_from_hide(verts, factors);
  scale_factors(factors, factor);

  tls.new_mask.resize(verts.size());
  const MutableSpan<float> new_mask = tls.new_mask;
  new_mask.fill(1.0f);
  auto_mask::calc_vert_factors(depsgraph, object, automasking, node, verts, new_mask);

  mask::invert_mask(new_mask);

  tls.mask.resize(verts.size());
  const MutableSpan<float> node_mask = tls.mask;
  gather_mask_bmesh(*ss.bm, verts, node_mask);

  calc_new_masks(mode, node_mask, new_mask);
  mix_new_masks(new_mask, factors, node_mask);

  scatter_mask_bmesh(node_mask.as_span(), *ss.bm, verts);
}

static void apply_mask_from_settings(const Depsgraph &depsgraph,
                                     Object &object,
                                     bke::pbvh::Tree &pbvh,
                                     const IndexMask &node_mask,
                                     const auto_mask::Cache &automasking,
                                     const ApplyMaskMode mode,
                                     const float factor)
{
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *id_cast<Mesh *>(object.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter mask = attributes.lookup_or_add_for_write_span<float>(
          ".sculpt_mask", bke::AttrDomain::Point);
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            apply_mask_mesh(
                depsgraph, object, hide_vert, automasking, mode, factor, nodes[i], tls, mask.span);
            bke::pbvh::node_update_mask_mesh(mask.span, nodes[i]);
          },
          exec_mode::grain_size(1));
      mask.finish();
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      MutableSpan<float> masks = subdiv_ccg.masks;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            apply_mask_grids(depsgraph, object, automasking, mode, factor, nodes[i], tls);
            bke::pbvh::node_update_mask_grids(key, masks, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const int mask_offset = CustomData_get_offset_named(
          &object.runtime->sculpt_session->bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            apply_mask_bmesh(depsgraph, object, automasking, mode, factor, nodes[i], tls);
            bke::pbvh::node_update_mask_bmesh(mask_offset, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
}

static wmOperatorStatus mask_from_cavity_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Paint &paint = sd.paint;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), &ob);
  BKE_sculpt_mask_layers_ensure(depsgraph, CTX_data_main(C), &ob, mmd);

  ed::sculpt_paint::mask_overlay_check(*C, *op);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);
  vert_random_access_ensure(ob);

  const ApplyMaskMode mode = ApplyMaskMode(RNA_enum_get(op->ptr, "mix_mode"));
  const float factor = RNA_float_get(op->ptr, "mix_factor");

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  /* Set up automasking settings. */
  Paint scene_copy = dna::shallow_copy(sd.paint);
  /* We don't do a deep copy of the automasking settings, we simply need a new one so that the
   * canonical pointer isn't overwritten. */
  MeshAutomaskingSettings automasking_settings;
  scene_copy.mesh_automasking_settings = &automasking_settings;

  /* TODO: This pattern of recreating the scene / brush and using them as the "settings" is weak
   * and can cause hard to find bugs due to modifying actual data. This should be refactored to
   * take in a options struct */
  MaskSettingsSource src = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));
  switch (src) {
    case MaskSettingsSource::Operator:
      if (RNA_boolean_get(op->ptr, "invert")) {
        scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_CAVITY_INVERTED;
      }
      else {
        scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_CAVITY_NORMAL;
      }

      if (RNA_boolean_get(op->ptr, "use_curve")) {
        scene_copy.mesh_automasking_settings->flags |= BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
      }

      scene_copy.mesh_automasking_settings->cavity_blur_steps = RNA_int_get(op->ptr, "blur_steps");
      scene_copy.mesh_automasking_settings->cavity_factor = RNA_float_get(op->ptr, "factor");

      scene_copy.mesh_automasking_settings->cavity_curve =
          paint.mesh_automasking_settings->cavity_curve_op;
      break;
    case MaskSettingsSource::Brush:
      if (brush) {
        scene_copy.mesh_automasking_settings->flags = brush->mesh_automasking_settings->flags;
        scene_copy.mesh_automasking_settings->cavity_factor =
            brush->mesh_automasking_settings->cavity_factor;
        scene_copy.mesh_automasking_settings->cavity_curve =
            brush->mesh_automasking_settings->cavity_curve;
        scene_copy.mesh_automasking_settings->cavity_blur_steps =
            brush->mesh_automasking_settings->cavity_blur_steps;

        /* Ensure only cavity masking is enabled. */
        scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_CAVITY_ALL |
                                                       BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
      }
      else {
        scene_copy.mesh_automasking_settings->flags = 0;
        BKE_report(op->reports, RPT_WARNING, "No active brush");

        return OPERATOR_CANCELLED;
      }

      break;
    case MaskSettingsSource::Scene:
      /* Ensure only cavity masking is enabled. */
      scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_CAVITY_ALL |
                                                     BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
      break;
  }

  /* Ensure cavity mask is actually enabled. */
  if (!(scene_copy.mesh_automasking_settings->flags & BRUSH_AUTOMASKING_CAVITY_ALL)) {
    scene_copy.mesh_automasking_settings->flags |= BRUSH_AUTOMASKING_CAVITY_NORMAL;
  }

  /* Create copy of brush with cleared automasking settings. */
  Brush brush_copy = dna::shallow_copy(*brush);
  MeshAutomaskingSettings brush_settings;
  brush_settings.flags = 0;
  brush_settings.boundary_edges_propagation_steps = 1;
  brush_settings.cavity_curve = scene_copy.mesh_automasking_settings->cavity_curve;

  brush_copy.mesh_automasking_settings = &brush_settings;
  /* Set a brush type that doesn't change topology so automasking isn't "disabled". */
  brush_copy.sculpt_brush_type = SCULPT_BRUSH_TYPE_SMOOTH;

  std::unique_ptr<auto_mask::Cache> automasking = auto_mask::cache_init(
      *depsgraph, scene_copy, &brush_copy, ob);

  if (!automasking) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);
  undo::push_nodes(*depsgraph, ob, node_mask, undo::Type::Mask);

  automasking->calc_cavity_factor(*depsgraph, ob, node_mask);
  apply_mask_from_settings(*depsgraph, ob, pbvh, node_mask, *automasking, mode, factor);

  undo::push_end(ob);

  pbvh.tag_masks_changed(node_mask);
  flush_update_done(C, ob, UpdateType::Mask);
  tag_update_overlays(C);

  return OPERATOR_FINISHED;
}

static void mask_from_cavity_ui(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  Scene *scene = CTX_data_scene(C);
  Sculpt *sd = scene->toolsettings ? scene->toolsettings->sculpt : nullptr;

  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  MaskSettingsSource source = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));

  if (!sd) {
    source = MaskSettingsSource::Operator;
  }

  switch (source) {
    case MaskSettingsSource::Operator: {
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "blur_steps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "invert", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "use_curve", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      if (sd && RNA_boolean_get(op->ptr, "use_curve")) {
        PointerRNA sculpt_ptr = RNA_pointer_create_discrete(&scene->id, RNA_Sculpt, sd);
        template_curve_mapping(&layout,
                               &sculpt_ptr,
                               "automasking_cavity_curve_op",
                               'v',
                               false,
                               false,
                               false,
                               false,
                               false);
      }
      break;
    }
    case MaskSettingsSource::Brush:
    case MaskSettingsSource::Scene:
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      break;
  }
}

static void SCULPT_OT_mask_from_cavity(wmOperatorType *ot)
{
  ot->name = "Mask From Cavity";
  ot->idname = "SCULPT_OT_mask_from_cavity";
  ot->description = "Creates a mask based on the curvature of the surface";

  ot->ui = mask_from_cavity_ui;
  ot->exec = mask_from_cavity_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mix_mode", mix_modes, int(ApplyMaskMode::Mix), "Mode", "Mix mode");
  RNA_def_float(ot->srna, "mix_factor", 1.0f, 0.0f, 5.0f, "Mix Factor", "", 0.0f, 1.0f);
  RNA_def_enum(ot->srna,
               "settings_source",
               settings_sources,
               int(MaskSettingsSource::Operator),
               "Settings",
               "Use settings from here");
  RNA_def_float(ot->srna,
                "factor",
                0.5f,
                0.0f,
                5.0f,
                "Factor",
                "The contrast of the cavity mask",
                0.0f,
                1.0f);
  RNA_def_int(ot->srna,
              "blur_steps",
              2,
              0,
              25,
              "Blur",
              "The number of times the cavity mask is blurred",
              0,
              25);
  RNA_def_boolean(ot->srna, "use_curve", false, "Custom Curve", "");
  RNA_def_boolean(ot->srna, "invert", false, "Cavity (Inverted)", "");
}

enum class MaskBoundaryMode : int8_t { Mesh, FaceSets };

static wmOperatorStatus mask_from_boundary_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Scene &scene = *CTX_data_scene(C);
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), &ob);
  BKE_sculpt_mask_layers_ensure(depsgraph, CTX_data_main(C), &ob, mmd);

  ed::sculpt_paint::mask_overlay_check(*C, *op);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);
  vert_random_access_ensure(ob);

  const ApplyMaskMode mode = ApplyMaskMode(RNA_enum_get(op->ptr, "mix_mode"));
  const float factor = RNA_float_get(op->ptr, "mix_factor");

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);

  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

  /* Set up automasking settings. */
  Paint scene_copy = dna::shallow_copy(sd.paint);
  /* We don't do a deep copy of the automasking settings, we simply need a new one so that the
   * canonical pointer isn't overwritten. */
  MeshAutomaskingSettings automasking_settings;
  scene_copy.mesh_automasking_settings = &automasking_settings;

  MaskSettingsSource src = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));
  switch (src) {
    case MaskSettingsSource::Operator: {
      const MaskBoundaryMode boundary_mode = MaskBoundaryMode(
          RNA_enum_get(op->ptr, "boundary_mode"));
      switch (boundary_mode) {
        case MaskBoundaryMode::Mesh:
          scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_BOUNDARY_EDGES;
          break;
        case MaskBoundaryMode::FaceSets:
          scene_copy.mesh_automasking_settings->flags = BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS;
          break;
      }
      scene_copy.mesh_automasking_settings->boundary_edges_propagation_steps = RNA_int_get(
          op->ptr, "propagation_steps");
      break;
    }
    case MaskSettingsSource::Brush:
      if (brush) {
        scene_copy.mesh_automasking_settings->flags = brush->mesh_automasking_settings->flags;
        scene_copy.mesh_automasking_settings->boundary_edges_propagation_steps =
            brush->mesh_automasking_settings->boundary_edges_propagation_steps;

        scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_BOUNDARY_EDGES |
                                                       BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS;
      }
      else {
        scene_copy.mesh_automasking_settings->flags = 0;
        BKE_report(op->reports, RPT_WARNING, "No active brush");

        return OPERATOR_CANCELLED;
      }

      break;
    case MaskSettingsSource::Scene:
      scene_copy.mesh_automasking_settings->flags &= BRUSH_AUTOMASKING_BOUNDARY_EDGES |
                                                     BRUSH_AUTOMASKING_BOUNDARY_FACE_SETS;
      break;
  }

  /* Create copy of brush with cleared automasking settings. */
  Brush brush_copy = dna::shallow_copy(*brush);
  MeshAutomaskingSettings brush_settings;
  brush_settings.flags = 0;
  brush_settings.boundary_edges_propagation_steps = 1;
  /* Set a brush type that doesn't change topology so automasking isn't "disabled". */
  brush_copy.mesh_automasking_settings = &brush_settings;
  brush_copy.sculpt_brush_type = SCULPT_BRUSH_TYPE_SMOOTH;

  std::unique_ptr<auto_mask::Cache> automasking = auto_mask::cache_init(
      *depsgraph, scene_copy, &brush_copy, ob);

  if (!automasking) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);
  undo::push_nodes(*depsgraph, ob, node_mask, undo::Type::Mask);

  apply_mask_from_settings(*depsgraph, ob, pbvh, node_mask, *automasking, mode, factor);

  undo::push_end(ob);

  pbvh.tag_masks_changed(node_mask);
  flush_update_done(C, ob, UpdateType::Mask);
  tag_update_overlays(C);

  return OPERATOR_FINISHED;
}

static void mask_from_boundary_ui(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  Scene *scene = CTX_data_scene(C);
  Sculpt *sd = scene->toolsettings ? scene->toolsettings->sculpt : nullptr;

  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);
  MaskSettingsSource source = MaskSettingsSource(RNA_enum_get(op->ptr, "settings_source"));

  if (!sd) {
    source = MaskSettingsSource::Operator;
  }

  switch (source) {
    case MaskSettingsSource::Operator: {
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "boundary_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "propagation_steps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
    }
    case MaskSettingsSource::Brush:
    case MaskSettingsSource::Scene:
      layout.prop(op->ptr, "mix_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(op->ptr, "mix_factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      break;
  }
}

static void SCULPT_OT_mask_from_boundary(wmOperatorType *ot)
{
  ot->name = "Mask From Boundary";
  ot->idname = "SCULPT_OT_mask_from_boundary";
  ot->description = "Creates a mask based on the boundaries of the surface";

  ot->ui = mask_from_boundary_ui;
  ot->exec = mask_from_boundary_exec;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mix_mode", mix_modes, int(ApplyMaskMode::Mix), "Mode", "Mix mode");
  RNA_def_float(ot->srna, "mix_factor", 1.0f, 0.0f, 5.0f, "Mix Factor", "", 0.0f, 1.0f);
  RNA_def_enum(ot->srna,
               "settings_source",
               settings_sources,
               int(MaskSettingsSource::Operator),
               "Settings",
               "Use settings from here");

  static EnumPropertyItem mask_boundary_modes[] = {
      {int(MaskBoundaryMode::Mesh),
       "MESH",
       ICON_NONE,
       "Mesh",
       "Calculate the boundary mask based on disconnected mesh topology islands"},
      {int(MaskBoundaryMode::FaceSets),
       "FACE_SETS",
       ICON_NONE,
       "Face Sets",
       "Calculate the boundary mask between face sets"},
      {0, nullptr, 0, nullptr, nullptr}};

  RNA_def_enum(ot->srna,
               "boundary_mode",
               mask_boundary_modes,
               int(MaskBoundaryMode::Mesh),
               "Mode",
               "Boundary type to mask");
  RNA_def_int(ot->srna, "propagation_steps", 1, 1, 20, "Propagation Steps", "", 1, 20);
}

/** \} */

}  // namespace mask

void operatortypes_sculpt()
{
  WM_operatortype_append(SCULPT_OT_brush_stroke);
  WM_operatortype_append(SCULPT_OT_sculptmode_toggle);
  WM_operatortype_append(SCULPT_OT_set_persistent_base);
  WM_operatortype_append(dyntopo::SCULPT_OT_dynamic_topology_toggle);
  WM_operatortype_append(SCULPT_OT_optimize);
  WM_operatortype_append(SCULPT_OT_symmetrize);
  WM_operatortype_append(dyntopo::SCULPT_OT_detail_flood_fill);
  WM_operatortype_append(dyntopo::SCULPT_OT_sample_detail_size);
  WM_operatortype_append(filter::SCULPT_OT_mesh_filter);
  WM_operatortype_append(mask::SCULPT_OT_mask_filter);
  WM_operatortype_append(SCULPT_OT_set_pivot_position);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_create);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_change_visibility);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_randomize_colors);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_init);
  WM_operatortype_append(face_set::SCULPT_OT_face_sets_edit);
  WM_operatortype_append(cloth::SCULPT_OT_cloth_filter);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_lasso_gesture);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_box_gesture);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_line_gesture);
  WM_operatortype_append(face_set::SCULPT_OT_face_set_polyline_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_box_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_lasso_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_line_gesture);
  WM_operatortype_append(trim::SCULPT_OT_trim_polyline_gesture);
  WM_operatortype_append(project::SCULPT_OT_project_line_gesture);

  WM_operatortype_append(color::SCULPT_OT_color_filter);
  WM_operatortype_append(mask::SCULPT_OT_mask_by_color);
  WM_operatortype_append(dyntopo::SCULPT_OT_dyntopo_detail_size_edit);
  WM_operatortype_append(mask::SCULPT_OT_mask_init);

  WM_operatortype_append(expand::SCULPT_OT_expand);
  WM_operatortype_append(mask::SCULPT_OT_mask_from_cavity);
  WM_operatortype_append(mask::SCULPT_OT_mask_from_boundary);
  WM_operatortype_append(SCULPT_OT_paint_mask_extract);
  WM_operatortype_append(SCULPT_OT_face_set_extract);
  WM_operatortype_append(SCULPT_OT_paint_mask_slice);

  WM_operatortype_append(layers::SCULPT_OT_layer_add);
  WM_operatortype_append(layers::SCULPT_OT_layer_remove);
  WM_operatortype_append(layers::SCULPT_OT_layer_move);
  WM_operatortype_append(layers::SCULPT_OT_layer_move_to);
  WM_operatortype_append(layers::SCULPT_OT_layer_duplicate);
  WM_operatortype_append(layers::SCULPT_OT_layer_merge_down);
  WM_operatortype_append(layers::SCULPT_OT_layer_merge_selected);
  WM_operatortype_append(layers::SCULPT_OT_layer_bake);
  WM_operatortype_append(layers::SCULPT_OT_layer_bake_to_shape_key);
  WM_operatortype_append(layers::SCULPT_OT_layer_bake_and_editmode_enter);
  WM_operatortype_append(layers::SCULPT_OT_layer_clear);
  WM_operatortype_append(layers::SCULPT_OT_layer_invert);
  WM_operatortype_append(layers::SCULPT_OT_layer_validate);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_isolate);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_add);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_remove);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_invert);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_apply);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_clear);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_fill);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_edit_toggle);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_edit_finish);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_edit_cancel);
  WM_operatortype_append(layers::SCULPT_OT_layer_mask_toggle);
  WM_operatortype_append(layers::SCULPT_OT_layer_set_influence);
  WM_operatortype_append(layers::SCULPT_OT_layer_influence_drag);
  WM_operatortype_append(layers::SCULPT_OT_layer_toggle_visibility);
  WM_operatortype_append(layers::SCULPT_OT_layer_select);
  WM_operatortype_append(layers::SCULPT_OT_layer_toggle_rec);
  WM_operatortype_append(layers::SCULPT_OT_layer_solo_base);
  WM_operatortype_append(layers::SCULPT_OT_layer_group_add);
  WM_operatortype_append(layers::SCULPT_OT_layer_group_remove);
  WM_operatortype_append(layers::SCULPT_OT_layer_group_merge);
  WM_operatortype_append(layers::SCULPT_OT_layer_group_delete);
  WM_operatortype_append(layers::SCULPT_OT_layer_group_toggle_visibility);
  WM_operatortype_append(layers::SCULPT_OT_layer_group_color_tag);
}

void keymap_sculpt(wmKeyConfig *keyconf)
{
  filter::modal_keymap(keyconf);
}

}  // namespace blender::ed::sculpt_paint
