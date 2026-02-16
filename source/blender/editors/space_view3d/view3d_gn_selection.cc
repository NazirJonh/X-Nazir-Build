/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include <cstdio>

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_lasso_2d.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_modifier.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

/* Debug logging - set to 1 to enable */
#define GN_SELECTION_DEBUG 1

#if GN_SELECTION_DEBUG
#  define GN_DEBUG_PRINT(...) printf("[GN Selection] " __VA_ARGS__)
#else
#  define GN_DEBUG_PRINT(...) ((void)0)
#endif

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_gn_selection.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_transform_snap_object_context.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_resources.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Runtime Data
 * \{ */

/**
 * Runtime data for GN Selection Mode.
 * Allocated when entering mode, freed when exiting.
 */
struct GNSelectionModeData {
  Object *object = nullptr;
  NodesModifierData *nmd = nullptr;
  bNode *selection_node = nullptr;

  Set<int> current_selection;

  bke::AttrDomain domain = bke::AttrDomain::Face;
  int hover_index = -1;
  bool selection_changed = false;

  eObjectMode previous_mode = OB_MODE_OBJECT;
};

static GNSelectionModeData *gn_selection_mode_data = nullptr;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mode Entry/Exit
 * \{ */

bool ED_gn_selection_mode_poll(const bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    GN_DEBUG_PRINT("poll: No active object\n");
    return false;
  }

  ModifierData *md = BKE_modifiers_findby_type(ob, ModifierType::eModifierType_Nodes);
  if (!md) {
    GN_DEBUG_PRINT("poll: No Nodes modifier on object '%s'\n", ob->id.name + 2);
    return false;
  }

  GN_DEBUG_PRINT("poll: OK for object '%s'\n", ob->id.name + 2);
  return true;
}

bool ED_gn_selection_mode_active(const Object *ob)
{
  return ob && (ob->mode & OB_MODE_GN_SELECTION) != 0 && gn_selection_mode_data != nullptr;
}

static GNSelectionModeData *ED_gn_selection_mode_data_get(const Object *ob)
{
  if (ED_gn_selection_mode_active(ob)) {
    return gn_selection_mode_data;
  }
  return nullptr;
}

bool ED_gn_selection_mode_enter(bContext *C, bNode *node)
{
  GN_DEBUG_PRINT("=== ED_gn_selection_mode_enter START ===\n");

  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    GN_DEBUG_PRINT("ERROR: No active object\n");
    return false;
  }
  GN_DEBUG_PRINT("Active object: '%s'\n", ob->id.name + 2);

  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(
      BKE_modifiers_findby_type(ob, ModifierType::eModifierType_Nodes));
  if (!nmd || !nmd->node_group) {
    GN_DEBUG_PRINT("ERROR: No Nodes modifier or node group\n");
    return false;
  }
  GN_DEBUG_PRINT("Found Nodes modifier with node group: '%s'\n", nmd->node_group->id.name + 2);

  /* Find 3D View Selection node if not provided */
  if (!node) {
    GN_DEBUG_PRINT("Searching for 3D View Selection node...\n");
    for (bNode *n : nmd->node_group->all_nodes()) {
      GN_DEBUG_PRINT("  Checking node: '%s' (type=%d)\n", n->name, n->type_legacy);
      if (n->type_legacy == GEO_NODE_3D_VIEW_SELECTION) {
        node = n;
        GN_DEBUG_PRINT("  FOUND 3D View Selection node: '%s'\n", n->name);
        break;
      }
    }
  }
  else {
    GN_DEBUG_PRINT("Using provided node: '%s'\n", node->name);
  }

  if (!node) {
    GN_DEBUG_PRINT("ERROR: No 3D View Selection node found\n");
    BKE_report(CTX_wm_reports(C), RPT_ERROR, "No 3D View Selection node found");
    return false;
  }

  /* Allocate runtime data */
  GNSelectionModeData *data = MEM_new<GNSelectionModeData>(__func__);
  data->object = ob;
  data->nmd = nmd;
  data->selection_node = node;
  data->previous_mode = eObjectMode(ob->mode);

  const NodeGeometry3DViewSelection &storage =
      *reinterpret_cast<const NodeGeometry3DViewSelection *>(node->storage);
  data->domain = bke::AttrDomain(storage.domain);
  GN_DEBUG_PRINT("Domain from storage: %d\n", int(storage.domain));

  /* Load existing selection */
  if (storage.selected_ids != nullptr && storage.selected_ids_num > 0) {
    GN_DEBUG_PRINT("Loading %d existing selected IDs\n", storage.selected_ids_num);
    for (int i = 0; i < storage.selected_ids_num; i++) {
      data->current_selection.add(storage.selected_ids[i]);
    }
  }
  else {
    GN_DEBUG_PRINT("No existing selection to load\n");
  }

  /* Set object mode */
  ob->mode = eObjectMode(ob->mode | OB_MODE_GN_SELECTION);
  gn_selection_mode_data = data;

  GN_DEBUG_PRINT("GN Selection Mode ENTERED successfully\n");
  GN_DEBUG_PRINT("=== ED_gn_selection_mode_enter END ===\n");

  WM_event_add_notifier(C, NC_OBJECT | ND_MODE, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);

  return true;
}

void ED_gn_selection_mode_exit(bContext *C, bool confirm)
{
  GN_DEBUG_PRINT("=== ED_gn_selection_mode_exit START (confirm=%d) ===\n", confirm);

  if (!gn_selection_mode_data) {
    GN_DEBUG_PRINT("WARNING: No gn_selection_mode_data\n");
    return;
  }

  GNSelectionModeData *data = gn_selection_mode_data;
  Object *ob = data->object;

  GN_DEBUG_PRINT("Object: '%s'\n", ob->id.name + 2);
  GN_DEBUG_PRINT("Selection changed: %d\n", data->selection_changed);
  GN_DEBUG_PRINT("Current selection size: %zu\n", data->current_selection.size());

  if (confirm && data->selection_changed) {
    GN_DEBUG_PRINT("Saving selection to node storage...\n");
    NodeGeometry3DViewSelection &storage =
        *reinterpret_cast<NodeGeometry3DViewSelection *>(data->selection_node->storage);

    /* Free old selection */
    if (storage.selected_ids) {
      MEM_delete_void<void>(storage.selected_ids);
      storage.selected_ids = nullptr;
    }

    /* Store new selection */
    const int num = data->current_selection.size();
    if (num > 0) {
      GN_DEBUG_PRINT("Storing %d selected IDs\n", num);
      storage.selected_ids = MEM_new_array_zeroed<int>(num, __func__);
      int i = 0;
      for (const int id : data->current_selection) {
        GN_DEBUG_PRINT("  ID[%d] = %d\n", i, id);
        storage.selected_ids[i++] = id;
      }
      storage.selected_ids_num = num;
    }
    else {
      GN_DEBUG_PRINT("Clearing selection (no IDs)\n");
      storage.selected_ids_num = 0;
    }

    /* Tag for update */
    WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  }

  /* Restore previous mode */
  GN_DEBUG_PRINT("Restoring previous mode: %d\n", data->previous_mode);
  ob->mode = data->previous_mode;

  MEM_delete(data);
  gn_selection_mode_data = nullptr;

  GN_DEBUG_PRINT("=== ED_gn_selection_mode_exit END ===\n");

  WM_event_add_notifier(C, NC_OBJECT | ND_MODE, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Selection Utilities
 * \{ */

/**
 * Get the evaluated mesh from the object with Geometry Nodes modifier applied.
 */
static const Mesh *gn_selection_get_evaluated_mesh(const Scene *scene,
                                                   Object *ob,
                                                   Depsgraph *depsgraph)
{
  if (!ob) {
    return nullptr;
  }

  /* Get the evaluated object */
  Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
  if (!ob_eval) {
    return nullptr;
  }

  /* Get the evaluated mesh */
  return BKE_object_get_evaluated_mesh(ob_eval);
}

/**
 * Apply selection operation to the selection set.
 * Returns true if selection changed.
 */
static bool gn_selection_apply(Set<int> &selection,
                               int index,
                               const eSelectOp sel_op,
                               bool is_inside)
{
  const bool is_selected = selection.contains(index);
  const int sel_op_result = ED_select_op_action_deselected(sel_op, is_selected, is_inside);

  if (sel_op_result == 1) {
    selection.add(index);
    return true;
  }
  if (sel_op_result == 0) {
    selection.remove(index);
    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Single Click Selection (Raycast)
 * \{ */

static bool gn_select_poll(bContext *C)
{
  return ED_gn_selection_mode_active(CTX_data_active_object(C));
}

static wmOperatorStatus gn_select_exec(bContext *C, wmOperator *op)
{
  using namespace blender::ed::transform;

  GN_DEBUG_PRINT("=== gn_select_exec (single click) START ===\n");

  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  GNSelectionModeData *data = ED_gn_selection_mode_data_get(ob);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  if (!data) {
    GN_DEBUG_PRINT("ERROR: No GN selection mode data\n");
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  RNA_int_get_array(op->ptr, "mouse", mval);
  float mval_fl[2] = {float(mval[0]), float(mval[1])};
  GN_DEBUG_PRINT("Mouse position: (%d, %d)\n", mval[0], mval[1]);

  const bool extend = RNA_boolean_get(op->ptr, "extend");
  const bool deselect = RNA_boolean_get(op->ptr, "deselect");
  GN_DEBUG_PRINT("extend=%d, deselect=%d\n", extend, deselect);

  const eSelectOp sel_op = (deselect) ? SEL_OP_SUB :
                            (extend) ? SEL_OP_ADD : SEL_OP_SET;
  GN_DEBUG_PRINT("Selection operation: %d\n", sel_op);

  /* Get view context */
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  /* Setup ray from mouse position */
  float ray_start[3], ray_normal[3];
  if (!ED_view3d_win_to_ray_clipped(depsgraph, vc.region, vc.v3d, mval_fl, ray_start, ray_normal, true))
  {
    GN_DEBUG_PRINT("ERROR: Failed to create ray from mouse position\n");
    return OPERATOR_CANCELLED;
  }
  GN_DEBUG_PRINT("Ray: start=(%.3f, %.3f, %.3f), normal=(%.3f, %.3f, %.3f)\n",
         ray_start[0], ray_start[1], ray_start[2],
         ray_normal[0], ray_normal[1], ray_normal[2]);

  /* Create snap context for raycast */
  SnapObjectContext *sctx = ed::transform::snap_object_context_create();

  SnapObjectParams params = {};
  params.snap_target_select = SCE_SNAP_TARGET_ALL;
  params.edit_mode_type = SNAP_GEOM_FINAL;
  params.occlusion_test = SNAP_OCCLUSION_ALWAYS;

  /* Perform raycast */
  float hit_location[3], hit_normal[3];
  int hit_index = -1;
  const Object *hit_object = nullptr;

  GN_DEBUG_PRINT("Performing raycast...\n");
  const bool hit = snap_object_project_ray_ex(sctx,
                                              depsgraph,
                                              vc.v3d,
                                              &params,
                                              ray_start,
                                              ray_normal,
                                              nullptr,
                                              hit_location,
                                              hit_normal,
                                              &hit_index,
                                              &hit_object,
                                              nullptr);

  GN_DEBUG_PRINT("Raycast result: hit=%d, hit_index=%d\n", hit, hit_index);
  if (hit_object) {
    GN_DEBUG_PRINT("Hit object: '%s'\n", hit_object->id.name + 2);
  }

  snap_object_context_destroy(sctx);

  if (hit && hit_object == ob && hit_index >= 0) {
    GN_DEBUG_PRINT("HIT! Element index: %d\n", hit_index);
    GN_DEBUG_PRINT("Domain: %d\n", int(data->domain));

    /* Clear selection if not extending */
    if (sel_op == SEL_OP_SET) {
      GN_DEBUG_PRINT("Clearing previous selection (SET mode)\n");
      data->current_selection.clear();
    }

    /* Apply selection */
    if (gn_selection_apply(data->current_selection, hit_index, sel_op, true)) {
      GN_DEBUG_PRINT("Selection changed! New size: %zu\n", data->current_selection.size());
      data->selection_changed = true;
    }
  }
  else {
    GN_DEBUG_PRINT("NO HIT or wrong object\n");
    if (hit && hit_object != ob) {
      GN_DEBUG_PRINT("  Hit different object than expected\n");
    }
    if (hit && hit_index < 0) {
      GN_DEBUG_PRINT("  Invalid hit index\n");
    }
  }

  GN_DEBUG_PRINT("=== gn_select_exec END ===\n");

  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gn_select_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "mouse", event->mval);
  return gn_select_exec(C, op);
}

void GN_OT_select(wmOperatorType *ot)
{
  ot->name = "Select";
  ot->idname = "GN_OT_select";
  ot->description = "Select elements in GN Selection Mode";

  ot->invoke = gn_select_invoke;
  ot->exec = gn_select_exec;
  ot->poll = gn_select_poll;

  /* IMPORTANT: No OPTYPE_UNDO - undo is session-based */
  ot->flag = OPTYPE_BLOCKING;

  RNA_def_int_vector(ot->srna, "mouse", 2, nullptr, INT_MIN, INT_MAX, "Mouse", "", INT_MIN, INT_MAX);
  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend selection");
  RNA_def_boolean(ot->srna, "deselect", false, "Deselect", "Remove from selection");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Box Select
 * \{ */

struct GNBoxSelectUserData {
  ViewContext *vc;
  GNSelectionModeData *data;
  rcti rect;
  rctf rect_fl;
  eSelectOp sel_op;
  bool changed;
};

static void gn_select_box_vert_cb(void *user_data, const float screen_co[2], int index)
{
  GNBoxSelectUserData *data = static_cast<GNBoxSelectUserData *>(user_data);

  if (BLI_rctf_isect_pt_v(&data->rect_fl, screen_co)) {
    if (gn_selection_apply(data->data->current_selection, index, data->sel_op, true)) {
      data->changed = true;
    }
  }
}

static wmOperatorStatus gn_select_box_exec(bContext *C, wmOperator *op)
{
  GN_DEBUG_PRINT("=== gn_select_box_exec START ===\n");

  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  GNSelectionModeData *data = ED_gn_selection_mode_data_get(ob);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  if (!data) {
    GN_DEBUG_PRINT("ERROR: No GN selection mode data\n");
    return OPERATOR_CANCELLED;
  }

  /* Get selection rect */
  rcti rect;
  WM_operator_properties_border_to_rcti(op, &rect);
  GN_DEBUG_PRINT("Box rect: (%d, %d) - (%d, %d)\n", rect.xmin, rect.ymin, rect.xmax, rect.ymax);

  /* Get selection action */
  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  GN_DEBUG_PRINT("Selection operation: %d, Domain: %d\n", sel_op, int(data->domain));

  /* Clear selection if in SET mode */
  if (sel_op == SEL_OP_SET) {
    if (!data->current_selection.is_empty()) {
      GN_DEBUG_PRINT("Clearing previous selection\n");
      data->current_selection.clear();
      data->selection_changed = true;
    }
  }

  /* Get view context */
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  ED_view3d_viewcontext_init_object(&vc, ob);

  /* Setup user data */
  GNBoxSelectUserData userdata;
  userdata.vc = &vc;
  userdata.data = data;
  userdata.rect = rect;
  BLI_rctf_rcti_copy(&userdata.rect_fl, &rect);
  userdata.sel_op = sel_op;
  userdata.changed = false;

  /* Get evaluated mesh */
  const Mesh *mesh = gn_selection_get_evaluated_mesh(scene, ob, depsgraph);
  if (!mesh) {
    GN_DEBUG_PRINT("ERROR: No evaluated mesh\n");
    return OPERATOR_CANCELLED;
  }
  GN_DEBUG_PRINT("Mesh: verts=%d, edges=%d, faces=%d\n",
         mesh->verts_num, mesh->edges_num, mesh->faces_num);

  ED_view3d_init_mats_rv3d(ob, vc.rv3d);

  int elements_processed = 0;

  /* Select based on domain */
  switch (data->domain) {
    case bke::AttrDomain::Point: {
      GN_DEBUG_PRINT("Processing VERTEX domain\n");
      /* Iterate over vertices */
      meshobject_foreachScreenVert(
          &vc,
          [](void *user_data, const float screen_co[2], int index) {
            gn_select_box_vert_cb(user_data, screen_co, index);
          },
          &userdata,
          V3D_PROJ_TEST_CLIP_DEFAULT);
      elements_processed = mesh->verts_num;
      break;
    }
    case bke::AttrDomain::Edge: {
      GN_DEBUG_PRINT("Processing EDGE domain\n");
      /* For edges, use the face center approach but map to edges */
      /* Since we're working with evaluated mesh, iterate edges */
      const float4x4 projection = ED_view3d_ob_project_mat_get(vc.rv3d, ob);

      for (const int edge_idx : IndexRange(mesh->edges_num)) {
        const blender::int2 edge = mesh->edges()[edge_idx];
        const float3 v1 = mesh->vert_positions()[edge[0]];
        const float3 v2 = mesh->vert_positions()[edge[1]];

        float screen_co_a[2], screen_co_b[2];
        if (ED_view3d_project_float_object(vc.region, v1, screen_co_a, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK &&
            ED_view3d_project_float_object(vc.region, v2, screen_co_b, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK)
        {
          /* Check if edge is fully or partially inside rect */
          if (BLI_rctf_isect_segment(&userdata.rect_fl, screen_co_a, screen_co_b)) {
            if (gn_selection_apply(data->current_selection, edge_idx, sel_op, true)) {
              userdata.changed = true;
            }
          }
        }
      }
      elements_processed = mesh->edges_num;
      break;
    }
    case bke::AttrDomain::Face: {
      GN_DEBUG_PRINT("Processing FACE domain\n");
      /* For faces, use face centers */
      const float4x4 projection = ED_view3d_ob_project_mat_get(vc.rv3d, ob);
      const blender::Span<float3> positions = mesh->vert_positions();
      const blender::OffsetIndices faces = mesh->faces();
      const blender::Span<int> corner_verts = mesh->corner_verts();

      for (const int face_idx : faces.index_range()) {
        /* Calculate face center */
        float3 center(0.0f);
        for (const int corner : faces[face_idx]) {
          center += positions[corner_verts[corner]];
        }
        center /= float(faces[face_idx].size());

        float screen_co[2];
        if (ED_view3d_project_float_object(vc.region, center, screen_co, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK) {
          if (BLI_rctf_isect_pt_v(&userdata.rect_fl, screen_co)) {
            if (gn_selection_apply(data->current_selection, face_idx, sel_op, true)) {
              userdata.changed = true;
            }
          }
        }
      }
      elements_processed = mesh->faces_num;
      break;
    }
    default:
      GN_DEBUG_PRINT("WARNING: Unknown domain %d\n", int(data->domain));
      break;
  }

  GN_DEBUG_PRINT("Elements processed: %d, Changed: %d\n", elements_processed, userdata.changed);

  if (userdata.changed) {
    data->selection_changed = true;
    GN_DEBUG_PRINT("Selection size now: %zu\n", data->current_selection.size());
  }

  GN_DEBUG_PRINT("=== gn_select_box_exec END ===\n");

  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gn_select_box_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus gn_select_box_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  return WM_gesture_box_modal(C, op, event);
}

static void gn_select_box_cancel(bContext *C, wmOperator *op)
{
  WM_gesture_box_cancel(C, op);
}

void GN_OT_select_box(wmOperatorType *ot)
{
  ot->name = "Box Select";
  ot->idname = "GN_OT_select_box";
  ot->description = "Select elements using box selection in GN Selection Mode";

  ot->invoke = gn_select_box_invoke;
  ot->modal = gn_select_box_modal;
  ot->exec = gn_select_box_exec;
  ot->cancel = gn_select_box_cancel;
  ot->poll = gn_select_poll;

  ot->flag = OPTYPE_BLOCKING;

  WM_operator_properties_border(ot);
  WM_operator_properties_select_operation_simple(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lasso Select
 * \{ */

struct GNLassoSelectUserData {
  ViewContext *vc;
  GNSelectionModeData *data;
  rcti rect;
  rctf rect_fl;
  Span<int2> mcoords;
  eSelectOp sel_op;
  bool changed;
};

static void gn_select_lasso_vert_cb(void *user_data, const float screen_co[2], int index)
{
  GNLassoSelectUserData *data = static_cast<GNLassoSelectUserData *>(user_data);

  if (BLI_rctf_isect_pt_v(&data->rect_fl, screen_co) &&
      BLI_lasso_is_point_inside(data->mcoords, int(screen_co[0]), int(screen_co[1]), IS_CLIPPED))
  {
    if (gn_selection_apply(data->data->current_selection, index, data->sel_op, true)) {
      data->changed = true;
    }
  }
}

static wmOperatorStatus gn_select_lasso_exec(bContext *C, wmOperator *op)
{
  GN_DEBUG_PRINT("=== gn_select_lasso_exec START ===\n");

  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  GNSelectionModeData *data = ED_gn_selection_mode_data_get(ob);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  if (!data) {
    GN_DEBUG_PRINT("ERROR: No GN selection mode data\n");
    return OPERATOR_CANCELLED;
  }

  /* Get lasso coordinates */
  const Array<int2> mcoords = WM_gesture_lasso_path_to_array(C, op);
  if (mcoords.is_empty()) {
    GN_DEBUG_PRINT("No lasso coordinates\n");
    return OPERATOR_CANCELLED;
  }
  GN_DEBUG_PRINT("Lasso points: %zu\n", mcoords.size());

  /* Get bounding rect */
  rcti rect;
  BLI_lasso_boundbox(&rect, mcoords);
  GN_DEBUG_PRINT("Lasso bounding box: (%d, %d) - (%d, %d)\n", rect.xmin, rect.ymin, rect.xmax, rect.ymax);

  /* Get selection action */
  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  GN_DEBUG_PRINT("Selection operation: %d, Domain: %d\n", sel_op, int(data->domain));

  /* Clear selection if in SET mode */
  if (sel_op == SEL_OP_SET) {
    if (!data->current_selection.is_empty()) {
      GN_DEBUG_PRINT("Clearing previous selection\n");
      data->current_selection.clear();
      data->selection_changed = true;
    }
  }

  /* Get view context */
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  ED_view3d_viewcontext_init_object(&vc, ob);

  /* Setup user data */
  GNLassoSelectUserData userdata;
  userdata.vc = &vc;
  userdata.data = data;
  userdata.rect = rect;
  BLI_rctf_rcti_copy(&userdata.rect_fl, &rect);
  userdata.mcoords = mcoords;
  userdata.sel_op = sel_op;
  userdata.changed = false;

  /* Get evaluated mesh */
  const Mesh *mesh = gn_selection_get_evaluated_mesh(scene, ob, depsgraph);
  if (!mesh) {
    GN_DEBUG_PRINT("ERROR: No evaluated mesh\n");
    return OPERATOR_CANCELLED;
  }
  GN_DEBUG_PRINT("Mesh: verts=%d, edges=%d, faces=%d\n",
         mesh->verts_num, mesh->edges_num, mesh->faces_num);

  ED_view3d_init_mats_rv3d(ob, vc.rv3d);

  /* Select based on domain */
  switch (data->domain) {
    case bke::AttrDomain::Point: {
      GN_DEBUG_PRINT("Processing VERTEX domain\n");
      meshobject_foreachScreenVert(
          &vc,
          [](void *user_data, const float screen_co[2], int index) {
            gn_select_lasso_vert_cb(user_data, screen_co, index);
          },
          &userdata,
          V3D_PROJ_TEST_CLIP_DEFAULT);
      break;
    }
    case bke::AttrDomain::Edge: {
      GN_DEBUG_PRINT("Processing EDGE domain\n");
      const blender::Span<float3> positions = mesh->vert_positions();
      const blender::Span<blender::int2> edges = mesh->edges();

      for (const int edge_idx : edges.index_range()) {
        const blender::int2 edge = edges[edge_idx];
        const float3 v1 = positions[edge[0]];
        const float3 v2 = positions[edge[1]];

        float screen_co_a[2], screen_co_b[2];
        if (ED_view3d_project_float_object(vc.region, v1, screen_co_a, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK &&
            ED_view3d_project_float_object(vc.region, v2, screen_co_b, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK)
        {
          if (BLI_rctf_isect_segment(&userdata.rect_fl, screen_co_a, screen_co_b) &&
              BLI_lasso_is_edge_inside(mcoords, int(screen_co_a[0]), int(screen_co_a[1]),
                                       int(screen_co_b[0]), int(screen_co_b[1]), IS_CLIPPED))
          {
            if (gn_selection_apply(data->current_selection, edge_idx, sel_op, true)) {
              userdata.changed = true;
            }
          }
        }
      }
      break;
    }
    case bke::AttrDomain::Face: {
      GN_DEBUG_PRINT("Processing FACE domain\n");
      const blender::Span<float3> positions = mesh->vert_positions();
      const blender::OffsetIndices faces = mesh->faces();
      const blender::Span<int> corner_verts = mesh->corner_verts();

      for (const int face_idx : faces.index_range()) {
        float3 center(0.0f);
        for (const int corner : faces[face_idx]) {
          center += positions[corner_verts[corner]];
        }
        center /= float(faces[face_idx].size());

        float screen_co[2];
        if (ED_view3d_project_float_object(vc.region, center, screen_co, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK) {
          if (BLI_rctf_isect_pt_v(&userdata.rect_fl, screen_co) &&
              BLI_lasso_is_point_inside(mcoords, int(screen_co[0]), int(screen_co[1]), IS_CLIPPED))
          {
            if (gn_selection_apply(data->current_selection, face_idx, sel_op, true)) {
              userdata.changed = true;
            }
          }
        }
      }
      break;
    }
    default:
      GN_DEBUG_PRINT("WARNING: Unknown domain\n");
      break;
  }

  GN_DEBUG_PRINT("Changed: %d\n", userdata.changed);

  if (userdata.changed) {
    data->selection_changed = true;
    GN_DEBUG_PRINT("Selection size now: %zu\n", data->current_selection.size());
  }

  GN_DEBUG_PRINT("=== gn_select_lasso_exec END ===\n");

  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);
  return OPERATOR_FINISHED;
}

void GN_OT_select_lasso(wmOperatorType *ot)
{
  ot->name = "Lasso Select";
  ot->idname = "GN_OT_select_lasso";
  ot->description = "Select elements using lasso selection in GN Selection Mode";

  ot->invoke = WM_gesture_lasso_invoke;
  ot->modal = WM_gesture_lasso_modal;
  ot->exec = gn_select_lasso_exec;
  ot->poll = gn_select_poll;
  ot->cancel = WM_gesture_lasso_cancel;

  ot->flag = OPTYPE_BLOCKING;

  WM_operator_properties_gesture_lasso(ot);
  WM_operator_properties_select_operation_simple(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Circle Select
 * \{ */

struct GNCircleSelectUserData {
  ViewContext *vc;
  GNSelectionModeData *data;
  int2 center;
  int radius;
  eSelectOp sel_op;
  bool changed;
};

static void gn_select_circle_vert_cb(void *user_data, const float screen_co[2], int index)
{
  GNCircleSelectUserData *data = static_cast<GNCircleSelectUserData *>(user_data);

  const int dx = int(screen_co[0]) - data->center[0];
  const int dy = int(screen_co[1]) - data->center[1];
  const bool is_inside = (dx * dx + dy * dy) <= (data->radius * data->radius);

  if (is_inside) {
    if (gn_selection_apply(data->data->current_selection, index, data->sel_op, true)) {
      data->changed = true;
    }
  }
}

static wmOperatorStatus gn_select_circle_exec(bContext *C, wmOperator *op)
{
  GN_DEBUG_PRINT("=== gn_select_circle_exec START ===\n");

  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  GNSelectionModeData *data = ED_gn_selection_mode_data_get(ob);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  if (!data) {
    GN_DEBUG_PRINT("ERROR: No GN selection mode data\n");
    return OPERATOR_CANCELLED;
  }

  /* Get circle parameters */
  const int center[2] = {RNA_int_get(op->ptr, "x"), RNA_int_get(op->ptr, "y")};
  const int radius = RNA_int_get(op->ptr, "radius");
  GN_DEBUG_PRINT("Circle: center=(%d, %d), radius=%d\n", center[0], center[1], radius);

  /* Get selection action */
  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  /* For modal operation, use gesture to determine if first call */
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  const bool is_modal_first = WM_gesture_is_modal_first(gesture);
  const eSelectOp effective_sel_op = ED_select_op_modal(sel_op, is_modal_first);
  GN_DEBUG_PRINT("sel_op=%d, is_modal_first=%d, effective_sel_op=%d\n",
         sel_op, is_modal_first, effective_sel_op);

  /* Clear selection if in SET mode on first call */
  if (effective_sel_op == SEL_OP_SET && is_modal_first) {
    if (!data->current_selection.is_empty()) {
      GN_DEBUG_PRINT("Clearing previous selection\n");
      data->current_selection.clear();
      data->selection_changed = true;
    }
  }

  /* Get view context */
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  ED_view3d_viewcontext_init_object(&vc, ob);

  /* Setup user data */
  GNCircleSelectUserData userdata;
  userdata.vc = &vc;
  userdata.data = data;
  userdata.center = int2(center[0], center[1]);
  userdata.radius = radius;
  userdata.sel_op = effective_sel_op;
  userdata.changed = false;

  /* Get evaluated mesh */
  const Mesh *mesh = gn_selection_get_evaluated_mesh(scene, ob, depsgraph);
  if (!mesh) {
    GN_DEBUG_PRINT("ERROR: No evaluated mesh\n");
    return OPERATOR_CANCELLED;
  }
  GN_DEBUG_PRINT("Mesh: verts=%d, edges=%d, faces=%d, Domain: %d\n",
         mesh->verts_num, mesh->edges_num, mesh->faces_num, int(data->domain));

  ED_view3d_init_mats_rv3d(ob, vc.rv3d);

  /* Helper lambda to check if point is inside circle */
  auto is_inside_circle = [&](const float screen_co[2]) -> bool {
    const int dx = int(screen_co[0]) - userdata.center[0];
    const int dy = int(screen_co[1]) - userdata.center[1];
    return (dx * dx + dy * dy) <= (userdata.radius * userdata.radius);
  };

  /* Select based on domain */
  switch (data->domain) {
    case bke::AttrDomain::Point: {
      GN_DEBUG_PRINT("Processing VERTEX domain\n");
      meshobject_foreachScreenVert(
          &vc,
          [](void *user_data, const float screen_co[2], int index) {
            gn_select_circle_vert_cb(user_data, screen_co, index);
          },
          &userdata,
          V3D_PROJ_TEST_CLIP_DEFAULT);
      break;
    }
    case bke::AttrDomain::Edge: {
      GN_DEBUG_PRINT("Processing EDGE domain\n");
      const blender::Span<float3> positions = mesh->vert_positions();
      const blender::Span<blender::int2> edges = mesh->edges();

      for (const int edge_idx : edges.index_range()) {
        const blender::int2 edge = edges[edge_idx];
        const float3 v1 = positions[edge[0]];
        const float3 v2 = positions[edge[1]];

        float screen_co_a[2], screen_co_b[2];
        if (ED_view3d_project_float_object(vc.region, v1, screen_co_a, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK &&
            ED_view3d_project_float_object(vc.region, v2, screen_co_b, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK)
        {
          /* Check if either endpoint is inside circle */
          if (is_inside_circle(screen_co_a) || is_inside_circle(screen_co_b))
          {
            if (gn_selection_apply(data->current_selection, edge_idx, effective_sel_op, true)) {
              userdata.changed = true;
            }
          }
        }
      }
      break;
    }
    case bke::AttrDomain::Face: {
      GN_DEBUG_PRINT("Processing FACE domain\n");
      const blender::Span<float3> positions = mesh->vert_positions();
      const blender::OffsetIndices faces = mesh->faces();
      const blender::Span<int> corner_verts = mesh->corner_verts();

      for (const int face_idx : faces.index_range()) {
        float3 center_3d(0.0f);
        for (const int corner : faces[face_idx]) {
          center_3d += positions[corner_verts[corner]];
        }
        center_3d /= float(faces[face_idx].size());

        float screen_co[2];
        if (ED_view3d_project_float_object(vc.region, center_3d, screen_co, V3D_PROJ_TEST_CLIP_DEFAULT) == V3D_PROJ_RET_OK) {
          if (is_inside_circle(screen_co)) {
            if (gn_selection_apply(data->current_selection, face_idx, effective_sel_op, true)) {
              userdata.changed = true;
            }
          }
        }
      }
      break;
    }
    default:
      GN_DEBUG_PRINT("WARNING: Unknown domain\n");
      break;
  }

  GN_DEBUG_PRINT("Changed: %d\n", userdata.changed);

  if (userdata.changed) {
    data->selection_changed = true;
    GN_DEBUG_PRINT("Selection size now: %zu\n", data->current_selection.size());
  }

  GN_DEBUG_PRINT("=== gn_select_circle_exec END ===\n");

  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gn_select_circle_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return WM_gesture_circle_invoke(C, op, event);
}

static wmOperatorStatus gn_select_circle_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmOperatorStatus ret = WM_gesture_circle_modal(C, op, event);

  if (ret & OPERATOR_RUNNING_MODAL) {
    /* Execute selection on mouse move */
    if (event->type == MOUSEMOVE) {
      gn_select_circle_exec(C, op);
    }
  }

  return ret;
}

static void gn_select_circle_cancel(bContext *C, wmOperator *op)
{
  WM_gesture_circle_cancel(C, op);
}

void GN_OT_select_circle(wmOperatorType *ot)
{
  ot->name = "Circle Select";
  ot->idname = "GN_OT_select_circle";
  ot->description = "Select elements using circle selection in GN Selection Mode";

  ot->invoke = gn_select_circle_invoke;
  ot->modal = gn_select_circle_modal;
  ot->exec = gn_select_circle_exec;
  ot->cancel = gn_select_circle_cancel;
  ot->poll = gn_select_poll;

  ot->flag = OPTYPE_BLOCKING;

  WM_operator_properties_gesture_circle(ot);
  WM_operator_properties_select_operation_simple(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Confirmation Operators
 * \{ */

static wmOperatorStatus gn_selection_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  ED_gn_selection_mode_exit(C, true);
  return OPERATOR_FINISHED;
}

void GN_OT_selection_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Selection";
  ot->idname = "GN_OT_selection_confirm";
  ot->description = "Confirm selection and exit GN Selection Mode";

  ot->exec = gn_selection_confirm_exec;
  ot->poll = gn_select_poll;

  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus gn_selection_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  ED_gn_selection_mode_exit(C, false);
  return OPERATOR_FINISHED;
}

void GN_OT_selection_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Selection";
  ot->idname = "GN_OT_selection_cancel";
  ot->description = "Cancel selection and exit GN Selection Mode";

  ot->exec = gn_selection_cancel_exec;
  ot->poll = gn_select_poll;

  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus gn_selection_mode_set_exec(bContext *C, wmOperator * /*op*/)
{
  return ED_gn_selection_mode_enter(C, nullptr) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static bool gn_selection_mode_set_poll(bContext *C)
{
  return ED_gn_selection_mode_poll(C);
}

void OBJECT_OT_gn_selection_mode_set(wmOperatorType *ot)
{
  ot->name = "Set GN Selection Mode";
  ot->idname = "OBJECT_OT_gn_selection_mode_set";
  ot->description = "Enter Geometry Nodes selection mode";

  ot->exec = gn_selection_mode_set_exec;
  ot->poll = gn_selection_mode_set_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Mode Operator
 * \{ */

static wmOperatorStatus gn_select_mode_exec(bContext *C, wmOperator *op)
{
  using namespace blender;
  const int select_mode = RNA_int_get(op->ptr, "type");

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  if (!ob || !ED_gn_selection_mode_active(ob)) {
    return OPERATOR_CANCELLED;
  }

  GNSelectionModeData *data = gn_selection_mode_data;
  if (!data || !data->selection_node) {
    return OPERATOR_CANCELLED;
  }

  /* Update domain in node storage */
  NodeGeometry3DViewSelection *storage = static_cast<NodeGeometry3DViewSelection *>(
      data->selection_node->storage);
  if (storage) {
    storage->domain = int8_t(select_mode);
    /* Clear existing selection when mode changes */
    storage->selected_ids_num = 0;
    storage->selected_indices_num = 0;
  }

  /* Update runtime data domain */
  data->domain = bke::AttrDomain(select_mode);
  data->current_selection.clear();
  data->selection_changed = true;

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  return OPERATOR_FINISHED;
}

static bool gn_select_mode_poll(bContext *C)
{
  return ED_gn_selection_mode_poll(C);
}

void GN_OT_select_mode(wmOperatorType *ot)
{
  /* Identifiers */
  ot->name = "GN Select Mode";
  ot->idname = "GN_OT_select_mode";
  ot->description = "Switch selection domain for Geometry Nodes selection";

  /* Callbacks */
  ot->exec = gn_select_mode_exec;
  ot->poll = gn_select_mode_poll;

  /* Flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Properties */
  RNA_def_int(ot->srna, "type", 0, 0, INT_MAX, "Selection Type", "Selection mode", 0, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keymap
 * \{ */

static void keymap_item_add(wmKeyMap *keymap,
                            const char *idname,
                            int type,
                            int value,
                            int modifier,
                            int direction)
{
  KeyMapItem_Params params{};
  params.type = type;
  params.value = value;
  params.modifier = modifier;
  params.direction = direction;
  WM_keymap_add_item(keymap, idname, &params);
}

void view3d_keymap_gn_selection(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(keyconf, "GN Selection Mode", SPACE_EMPTY, RGN_TYPE_WINDOW);

  /* Selection */
  keymap_item_add(keymap, "GN_OT_select", LEFTMOUSE, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_select", LEFTMOUSE, KM_PRESS, KM_SHIFT, KM_ANY);
  keymap_item_add(keymap, "GN_OT_select", LEFTMOUSE, KM_PRESS, KM_CTRL, KM_ANY);

  /* Box select - B key */
  keymap_item_add(keymap, "GN_OT_select_box", EVT_BKEY, KM_PRESS, 0, KM_ANY);

  /* Lasso select - Ctrl+Shift+RMB */
  keymap_item_add(keymap, "GN_OT_select_lasso", RIGHTMOUSE, KM_PRESS, KM_CTRL | KM_SHIFT, KM_ANY);
  keymap_item_add(keymap, "GN_OT_select_lasso", RIGHTMOUSE, KM_PRESS, KM_CTRL | KM_SHIFT | KM_SHIFT, KM_ANY);

  /* Circle select - C key */
  keymap_item_add(keymap, "GN_OT_select_circle", EVT_CKEY, KM_PRESS, 0, KM_ANY);

  /* Confirmation */
  keymap_item_add(keymap, "GN_OT_selection_confirm", EVT_RETKEY, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_selection_confirm", EVT_PADENTER, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_selection_cancel", EVT_ESCKEY, KM_PRESS, 0, KM_ANY);

  /* View navigation */
  keymap_item_add(keymap, "VIEW3D_OT_rotate", MIDDLEMOUSE, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "VIEW3D_OT_move", MIDDLEMOUSE, KM_PRESS, KM_SHIFT, KM_ANY);
  keymap_item_add(keymap, "VIEW3D_OT_zoom", MIDDLEMOUSE, KM_PRESS, KM_CTRL, KM_ANY);

  /* Selection mode switching */
  keymap_item_add(keymap, "GN_OT_select_mode", EVT_ONEKEY, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_select_mode", EVT_TWOKEY, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_select_mode", EVT_THREEKEY, KM_PRESS, 0, KM_ANY);
}

/** \} */

}  // namespace blender
