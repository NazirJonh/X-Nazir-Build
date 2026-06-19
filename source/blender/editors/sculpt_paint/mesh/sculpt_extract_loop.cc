/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BKE_context.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_screen.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "ED_numinput.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DEG_depsgraph.hh"

#include "BLT_translation.hh"

#include "UI_resources.hh"

#include "MEM_guardedalloc.h"

#include "bmesh.hh"

#include "sculpt_extract_loop.hh"
#include "sculpt_extract_loop_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_loop {

static EnumPropertyItem extraction_mode_items[] = {
    {int(ExtractionMode::Loop), "LOOP", 0, "Loop", "Extract edge loop as a wireframe object"},
    {int(ExtractionMode::Ring), "RING", 0, "Ring", "Extract edge ring as a wireframe object"},
    {int(ExtractionMode::FaceStrip),
     "FACE_STRIP",
     0,
     "Face Strip",
     "Extract the strip of quads in the face loop under the cursor"},
    {0, nullptr, 0, nullptr, nullptr},
};

static EnumPropertyItem loop_orientation_items[] = {
    {int(LoopOrientation::Horizontal),
     "HORIZONTAL",
     0,
     "Horizontal",
     "Select loops that run horizontally on screen"},
    {int(LoopOrientation::Vertical),
     "VERTICAL",
     0,
     "Vertical",
     "Select loops that run vertically on screen"},
    {0, nullptr, 0, nullptr, nullptr},
};

static EnumPropertyItem extraction_output_type_items[] = {
    {int(ExtractionOutputType::Mesh), "MESH", 0, "Mesh", "Extract as a new mesh object"},
    {int(ExtractionOutputType::Curves),
     "CURVES",
     0,
     "Curves",
     "Extract as a new curves object (Loop and Ring modes only)"},
    {int(ExtractionOutputType::Extrude),
     "EXTRUDE",
     0,
     "Extrude",
     "Extrude the face strip along face normals inside the active mesh (Face Strip mode only)"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Region draw callback — signature fixed by Blender's draw callback system. */
static void draw_preview_cb(const bContext * /*C*/, ARegion * /*region*/, void *userdata)
{
  const ExtractLoopModalData *data = static_cast<const ExtractLoopModalData *>(userdata);
  if (data->phase == ModalPhase::Extrude && data->edit_bm) {
    draw_face_strip_preview(data->shared, data->edit_bm, data->extrude_preview_faces);
    return;
  }
  draw_loop_preview(data->shared);
}

static void update_preview(bContext &C, ExtractLoopModalData &data, const float mval[2])
{
  data.shared.preview_points.clear();
  data.shared.preview_faces.clear();
  data.shared.loop_edges.clear();
  data.loop_edges_set.clear();
  data.shared.is_cyclic = false;
  data.shared.seed_edge = nullptr;
  data.has_boundary_seed = false;
  data.initial_hit = false;

  CursorGeometryInfo cgi;
  if (!cursor_geometry_info_update(&C, &cgi, mval, false)) {
    return;
  }
  data.initial_hit = true;
  data.shared.hit_location = cgi.location;

  data.shared.seed_edge = find_seed_edge_screen_space(data.shared, mval);
  if (!data.shared.seed_edge) {
    return;
  }

  run_walker(data.shared, data.use_boundary_walker, &data.has_boundary_seed);
  if (data.shared.mode != ExtractionMode::FaceStrip) {
    for (BMEdge *e : data.shared.loop_edges) {
      data.loop_edges_set.add(e);
    }
  }
}

void gesture_data_free(bContext *C, ExtractLoopModalData *data)
{
  if (!data) {
    return;
  }
  if (data->draw_handle) {
    ED_region_draw_cb_exit(CTX_wm_region(C)->runtime->type, data->draw_handle);
    data->draw_handle = nullptr;
  }
  if (data->edit_bm) {
    BM_mesh_free(data->edit_bm);
    data->edit_bm = nullptr;
  }
  if (data->shared.bm) {
    BM_mesh_free(data->shared.bm);
    data->shared.bm = nullptr;
  }
  MEM_delete(data);
}

static void gesture_cancel(bContext *C, wmOperator *op)
{
  gesture_data_free(C, static_cast<ExtractLoopModalData *>(op->customdata));
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  extract_loop_hover_deactivate();
}

static void update_status_bar(bContext *C, ExtractLoopModalData *data, const wmOperator *op)
{
  if (data->phase == ModalPhase::Extrude) {
    WorkspaceStatus status(C);
    status.item(IFACE_("Confirm"), ICON_MOUSE_LMB, ICON_EVENT_RETURN);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
    status.item(IFACE_("Distance"), ICON_EVENT_PADPLUS, ICON_NONE);
    extrude_update_status_text(C, *data);
    return;
  }

  WorkspaceStatus status(C);
  status.item(IFACE_("Confirm"), ICON_EVENT_RETURN, ICON_NONE);
  if (is_face_strip_extrude_output(op, data->shared.mode)) {
    status.item(IFACE_("Extrude"), ICON_MOUSE_LMB, ICON_NONE);
  }
  else if (data->has_boundary_seed) {
    status.item(IFACE_("Cycle"), ICON_MOUSE_LMB, ICON_NONE);
  }
  else {
    status.item(IFACE_("Extract"), ICON_MOUSE_LMB, ICON_NONE);
  }
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
  status.item(IFACE_("Orientation"), ICON_EVENT_T, ICON_NONE);
  if (data->initial_hit) {
    status.item_bool(IFACE_("Horizontal"),
                     data->shared.loop_orientation == LoopOrientation::Horizontal,
                     ICON_NONE);
    status.item_bool(
        IFACE_("Vertical"), data->shared.loop_orientation == LoopOrientation::Vertical, ICON_NONE);
    status.item_bool(IFACE_("Loop"), data->shared.mode == ExtractionMode::Loop, ICON_NONE);
    status.item_bool(IFACE_("Ring"), data->shared.mode == ExtractionMode::Ring, ICON_NONE);
    status.item_bool(
        IFACE_("Face Strip"), data->shared.mode == ExtractionMode::FaceStrip, ICON_NONE);
    if (data->has_boundary_seed && data->use_boundary_walker) {
      status.item_bool(IFACE_("Boundary"), true, ICON_NONE);
    }
  }
}

static wmOperatorStatus modal_phase_extrude(bContext *C,
                                            wmOperator *op,
                                            ExtractLoopModalData *data,
                                            const wmEvent *event)
{
  const bool has_numinput = hasNumInput(&data->num_input);

  if (event->val == KM_PRESS && has_numinput && handleNumInput(C, &data->num_input, event)) {
    float distance = data->extrude_distance;
    if (applyNumInput(&data->num_input, &distance)) {
      extrude_apply_distance(*data, distance);
      extrude_update_status_text(C, *data);
      ED_region_tag_redraw(CTX_wm_region(C));
    }
    return OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      if (!has_numinput) {
        const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        extrude_update_from_mouse(*data, mval_fl);
        extrude_update_status_text(C, *data);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      /* Releasing the button that started the drag (or a fresh click) confirms the
       * extrude directly, so no separate Enter press is needed. */
      if (ELEM(event->val, KM_RELEASE, KM_PRESS, KM_CLICK)) {
        finish_extract(C, op, data);
        return OPERATOR_FINISHED;
      }
      break;
    }
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        finish_extract(C, op, data);
        return OPERATOR_FINISHED;
      }
      break;
    }
    case RIGHTMOUSE:
    case EVT_ESCKEY: {
      if (event->val == KM_PRESS) {
        gesture_cancel(C, op);
        ED_region_tag_redraw(CTX_wm_region(C));
        return OPERATOR_CANCELLED;
      }
      break;
    }
    default: {
      if (event->val == KM_PRESS && handleNumInput(C, &data->num_input, event)) {
        float distance = data->extrude_distance;
        applyNumInput(&data->num_input, &distance);
        extrude_apply_distance(*data, distance);
        extrude_update_status_text(C, *data);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
  }

  return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
}

static wmOperatorStatus modal_phase_select(bContext *C,
                                           wmOperator *op,
                                           ExtractLoopModalData *data,
                                           const wmEvent *event)
{
  const bool face_strip_extrude = is_face_strip_extrude_output(op, data->shared.mode);

  switch (event->type) {
    case MOUSEMOVE: {
      const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
      update_preview(*C, *data, mval_fl);
      update_status_bar(C, data, op);
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE: {
      if (event->val == KM_PRESS) {
        if (data->has_boundary_seed) {
          data->use_boundary_walker = !data->use_boundary_walker;
          data->loop_edges_set.clear();
          run_walker(data->shared, data->use_boundary_walker, &data->has_boundary_seed);
          for (BMEdge *e : data->shared.loop_edges) {
            data->loop_edges_set.add(e);
          }
          update_status_bar(C, data, op);
          ED_region_tag_redraw(CTX_wm_region(C));
        }
        else if (face_strip_extrude) {
          if (!extract_preview_is_valid(data->shared)) {
            break;
          }
          data->extrude_init_mval = float2(float(event->mval[0]), float(event->mval[1]));
          if (extrude_begin(*C, op, *data)) {
            update_status_bar(C, data, op);
            ED_region_tag_redraw(CTX_wm_region(C));
          }
        }
        else {
          finish_extract(C, op, data);
          return OPERATOR_FINISHED;
        }
      }
      break;
    }
    case EVT_TKEY: {
      if (event->val == KM_PRESS) {
        data->shared.loop_orientation = data->shared.loop_orientation ==
                                                LoopOrientation::Horizontal ?
                                            LoopOrientation::Vertical :
                                            LoopOrientation::Horizontal;
        const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        update_preview(*C, *data, mval_fl);
        update_status_bar(C, data, op);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        if (face_strip_extrude) {
          data->extrude_init_mval = float2(float(event->mval[0]), float(event->mval[1]));
          if (extrude_begin(*C, op, *data)) {
            update_status_bar(C, data, op);
            ED_region_tag_redraw(CTX_wm_region(C));
          }
        }
        else {
          finish_extract(C, op, data);
          return OPERATOR_FINISHED;
        }
      }
      break;
    }
    case RIGHTMOUSE:
    case EVT_ESCKEY: {
      if (event->val == KM_PRESS) {
        gesture_cancel(C, op);
        ED_region_tag_redraw(CTX_wm_region(C));
        return OPERATOR_CANCELLED;
      }
      break;
    }
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus gesture_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ExtractLoopModalData *data = static_cast<ExtractLoopModalData *>(op->customdata);
  if (data->phase == ModalPhase::Extrude) {
    return modal_phase_extrude(C, op, data, event);
  }
  return modal_phase_select(C, op, data, event);
}

static wmOperatorStatus gesture_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ExtractLoopModalData *data = MEM_new<ExtractLoopModalData>(__func__);
  data->shared.mode = ExtractionMode(RNA_enum_get(op->ptr, "mode"));
  data->shared.loop_orientation = LoopOrientation(RNA_enum_get(op->ptr, "loop_orientation"));
  data->shared.obact = CTX_data_active_object(C);
  data->shared.region = CTX_wm_region(C);
  data->shared.rv3d = CTX_wm_region_view3d(C);
  data->use_boundary_walker = false;

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*data->shared.obact);
  if (!pbvh) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  data->shared.pbvh_type = pbvh->type();

  data->shared.bm = create_modal_bmesh(data->shared.obact, data->shared.pbvh_type);
  if (!data->shared.bm) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }

  if (data->shared.pbvh_type == bke::pbvh::Type::Mesh) {
    const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
    data->shared.preview_positions = bke::pbvh::vert_positions_eval(depsgraph,
                                                                    *data->shared.obact);
  }
  else if (data->shared.pbvh_type == bke::pbvh::Type::Grids) {
    Mesh *mesh = id_cast<Mesh *>(data->shared.obact->data);
    data->shared.preview_positions = mesh->vert_positions();
  }

  op->customdata = data;

  data->draw_handle = ED_region_draw_cb_activate(
      CTX_wm_region(C)->runtime->type, draw_preview_cb, data, REGION_DRAW_POST_VIEW);

  float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  update_preview(*C, *data, mval_fl);
  if (!extract_preview_is_valid(data->shared)) {
    sync_preview_from_hover(*data);
  }

  const bool face_strip_extrude = is_face_strip_extrude_output(op, data->shared.mode);

  if (extract_preview_is_valid(data->shared) && !data->has_boundary_seed && !face_strip_extrude) {
    finish_extract(C, op, data);
    return OPERATOR_FINISHED;
  }

  if (!extract_preview_is_valid(data->shared)) {
    gesture_data_free(C, data);
    op->customdata = nullptr;
    extract_loop_hover_deactivate();
    return OPERATOR_CANCELLED;
  }

  update_status_bar(C, data, op);
  extract_loop_hover_activate();
  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

static void operator_properties(blender::wmOperatorType *ot)
{
  RNA_def_enum(ot->srna, "mode", extraction_mode_items, int(ExtractionMode::Loop), "Mode", "");
  RNA_def_enum(ot->srna,
               "loop_orientation",
               loop_orientation_items,
               int(LoopOrientation::Horizontal),
               "Orientation",
               "Prefer horizontal or vertical loops in screen space");
  RNA_def_enum(ot->srna,
               "output_type",
               extraction_output_type_items,
               int(ExtractionOutputType::Mesh),
               "Output Type",
               "Type of object to create when extracting to a new object");
  RNA_def_boolean(ot->srna,
                  "new_object",
                  true,
                  "New Object",
                  "Create a new object instead of duplicating geometry inside the active mesh");
  RNA_def_boolean(
      ot->srna,
      "mask_selection",
      true,
      "Mask Selection",
      "Mask the rest of the mesh and fully unmask the duplicated geometry for editing");
}

wmKeyMap *modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {0, "CONFIRM", 0, "Confirm", ""},
      {1, "CANCEL", 0, "Cancel", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Sculpt Extract Loop Modal";
  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);
  if (keymap && keymap->modal_items) {
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  WM_modalkeymap_assign(keymap, "SCULPT_OT_extract_loop_gesture");
  return keymap;
}

void SCULPT_OT_extract_loop_gesture(blender::wmOperatorType *ot)
{
  ot->name = "Extract Loop";
  ot->idname = "SCULPT_OT_extract_loop_gesture";
  ot->description =
      "Extract an edge loop, ring, or face strip as a new object, duplicate it inside the mesh, "
      "or extrude a face strip along normals";

  ot->invoke = gesture_invoke;
  ot->modal = gesture_modal;
  ot->cancel = gesture_cancel;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  operator_properties(ot);
}

}  // namespace blender::ed::sculpt_paint::extract_loop
