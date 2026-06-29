/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Operator core for the Sculpt "Extract Region" gesture tool: invoke/modal/
 * cancel, source toggle, status bar, properties, and registration. The selection
 * flood-fill lives in #sculpt_extract_region_select.cc; the extrude / new-mesh
 * output and preview drawing are reused from the shared #extract engine.
 */

#include "BKE_context.hh"
#include "BKE_object.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
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

#include "sculpt_extract_region.hh"
#include "sculpt_extract_region_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::extract_region {

enum class OutputType {
  Extrude = 0,
  Mesh = 1,
};

static EnumPropertyItem region_source_items[] = {
    {int(RegionSource::FaceSet),
     "FACE_SET",
     0,
     "Face Set",
     "Select the connected face-set island under the cursor"},
    {int(RegionSource::Mask),
     "MASK",
     0,
     "Mask",
     "Select the connected masked island under the cursor"},
    {0, nullptr, 0, nullptr, nullptr},
};

static EnumPropertyItem output_type_items[] = {
    {int(OutputType::Extrude),
     "EXTRUDE",
     0,
     "Extrude",
     "Extrude the region along face normals inside the active mesh"},
    {int(OutputType::Mesh), "MESH", 0, "Mesh", "Extract the region as a new mesh object"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Region draw callback — signature fixed by Blender's draw callback system. */
static void draw_preview_cb(const bContext * /*C*/, ARegion * /*region*/, void *userdata)
{
  const ExtractRegionModalData *data = static_cast<const ExtractRegionModalData *>(userdata);
  if (data->extrude.phase == extract::ModalPhase::Extrude && data->extrude.edit_bm) {
    /* During extrude show only the region border, not the full face grid. */
    extract::draw_faces_preview(
        data->shared, data->extrude.edit_bm, data->extrude.preview_faces, true);
    return;
  }
  extract::draw_faces_preview(data->shared);
}

static void update_preview(bContext &C, ExtractRegionModalData &data, const float mval[2])
{
  data.shared.preview_faces.clear();
  data.seed_face = nullptr;

  CursorGeometryInfo cgi;
  if (!cursor_geometry_info_update(&C, &cgi, mval, false)) {
    return;
  }
  data.shared.hit_location = cgi.location;

  data.seed_face = find_seed_face(data.shared, mval);
  select_region(data.shared, data.source, data.mask_threshold, data.seed_face);
}

void gesture_data_free(bContext *C, ExtractRegionModalData *data)
{
  if (!data) {
    return;
  }
  if (data->draw_handle) {
    ED_region_draw_cb_exit(CTX_wm_region(C)->runtime->type, data->draw_handle);
    data->draw_handle = nullptr;
  }
  if (data->extrude.edit_bm) {
    BM_mesh_free(data->extrude.edit_bm);
    data->extrude.edit_bm = nullptr;
  }
  if (data->shared.bm) {
    BM_mesh_free(data->shared.bm);
    data->shared.bm = nullptr;
  }
  MEM_delete(data);
}

/**
 * Commit the active output and tear down the modal state. In the Extrude phase
 * the in-mesh extrude is committed; otherwise (Mesh output) a new object is
 * created from the previewed region.
 */
static void finish(bContext *C, wmOperator *op, ExtractRegionModalData *data)
{
  if (data->extrude.phase == extract::ModalPhase::Extrude) {
    extract::extrude_commit(*C, op, data->shared, data->extrude);
  }
  else if (OutputType(RNA_enum_get(op->ptr, "output_type")) == OutputType::Mesh) {
    extract::create_mesh_in_new_object(*C, data->shared);
  }
  gesture_data_free(C, data);
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  extract_region_hover_deactivate();
  ED_region_tag_redraw(CTX_wm_region(C));
}

static void gesture_cancel(bContext *C, wmOperator *op)
{
  gesture_data_free(C, static_cast<ExtractRegionModalData *>(op->customdata));
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  extract_region_hover_deactivate();
}

static void update_status_bar(bContext *C, ExtractRegionModalData *data, const wmOperator *op)
{
  if (data->extrude.phase == extract::ModalPhase::Extrude) {
    WorkspaceStatus status(C);
    status.item(IFACE_("Confirm"), ICON_MOUSE_LMB, ICON_EVENT_RETURN);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
    status.item(IFACE_("Distance"), ICON_EVENT_PADPLUS, ICON_NONE);
    extract::extrude_update_status_text(C, data->extrude);
    return;
  }

  const bool is_extrude = OutputType(RNA_enum_get(op->ptr, "output_type")) == OutputType::Extrude;

  WorkspaceStatus status(C);
  status.item(IFACE_("Confirm"), ICON_EVENT_RETURN, ICON_NONE);
  status.item(is_extrude ? IFACE_("Extrude") : IFACE_("Extract"), ICON_MOUSE_LMB, ICON_NONE);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
  status.item(IFACE_("Source"), ICON_EVENT_S, ICON_NONE);
  status.item_bool(IFACE_("Face Set"), data->source == RegionSource::FaceSet, ICON_NONE);
  status.item_bool(IFACE_("Mask"), data->source == RegionSource::Mask, ICON_NONE);
}

static wmOperatorStatus modal_phase_extrude(bContext *C,
                                            wmOperator *op,
                                            ExtractRegionModalData *data,
                                            const wmEvent *event)
{
  const bool has_numinput = hasNumInput(&data->extrude.num_input);

  if (event->val == KM_PRESS && has_numinput && handleNumInput(C, &data->extrude.num_input, event))
  {
    float distance = data->extrude.distance;
    if (applyNumInput(&data->extrude.num_input, &distance)) {
      extract::extrude_apply_distance(data->shared, data->extrude, distance);
      extract::extrude_update_status_text(C, data->extrude);
      ED_region_tag_redraw(CTX_wm_region(C));
    }
    return OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      if (!has_numinput) {
        const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        extract::extrude_update_from_mouse(data->shared, data->extrude, mval_fl);
        extract::extrude_update_status_text(C, data->extrude);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      /* Releasing the button that started the drag (or a fresh click) confirms the
       * extrude directly, so no separate Enter press is needed. */
      if (ELEM(event->val, KM_RELEASE, KM_PRESS, KM_CLICK)) {
        finish(C, op, data);
        return OPERATOR_FINISHED;
      }
      break;
    }
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        finish(C, op, data);
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
      if (event->val == KM_PRESS && handleNumInput(C, &data->extrude.num_input, event)) {
        float distance = data->extrude.distance;
        applyNumInput(&data->extrude.num_input, &distance);
        extract::extrude_apply_distance(data->shared, data->extrude, distance);
        extract::extrude_update_status_text(C, data->extrude);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
  }

  return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
}

static bool begin_extrude(bContext *C, ExtractRegionModalData *data, const wmEvent *event)
{
  if (!region_is_valid(data->shared)) {
    return false;
  }
  data->extrude.init_mval = float2(float(event->mval[0]), float(event->mval[1]));
  return extract::extrude_begin(
      *C, data->shared, data->extrude, data->shared.preview_faces, data->extrude.init_mval);
}

static wmOperatorStatus modal_phase_select(bContext *C,
                                           wmOperator *op,
                                           ExtractRegionModalData *data,
                                           const wmEvent *event)
{
  const bool is_extrude = OutputType(RNA_enum_get(op->ptr, "output_type")) == OutputType::Extrude;

  switch (event->type) {
    case MOUSEMOVE: {
      const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
      update_preview(*C, *data, mval_fl);
      update_status_bar(C, data, op);
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case EVT_SKEY: {
      if (event->val == KM_PRESS) {
        data->source = data->source == RegionSource::FaceSet ? RegionSource::Mask :
                                                               RegionSource::FaceSet;
        const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        update_preview(*C, *data, mval_fl);
        update_status_bar(C, data, op);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case LEFTMOUSE: {
      if (event->val == KM_PRESS) {
        if (!region_is_valid(data->shared)) {
          break;
        }
        if (is_extrude) {
          if (begin_extrude(C, data, event)) {
            update_status_bar(C, data, op);
            ED_region_tag_redraw(CTX_wm_region(C));
          }
        }
        else {
          finish(C, op, data);
          return OPERATOR_FINISHED;
        }
      }
      break;
    }
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        if (!region_is_valid(data->shared)) {
          break;
        }
        if (is_extrude) {
          if (begin_extrude(C, data, event)) {
            update_status_bar(C, data, op);
            ED_region_tag_redraw(CTX_wm_region(C));
          }
        }
        else {
          finish(C, op, data);
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
  ExtractRegionModalData *data = static_cast<ExtractRegionModalData *>(op->customdata);
  if (data->extrude.phase == extract::ModalPhase::Extrude) {
    return modal_phase_extrude(C, op, data, event);
  }
  return modal_phase_select(C, op, data, event);
}

static wmOperatorStatus gesture_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ExtractRegionModalData *data = MEM_new<ExtractRegionModalData>(__func__);
  data->source = RegionSource(RNA_enum_get(op->ptr, "region_source"));
  data->mask_threshold = RNA_float_get(op->ptr, "mask_threshold");
  data->shared.obact = CTX_data_active_object(C);
  data->shared.region = CTX_wm_region(C);
  data->shared.rv3d = CTX_wm_region_view3d(C);

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*data->shared.obact);
  if (!pbvh) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }
  data->shared.pbvh_type = pbvh->type();

  data->shared.bm = extract::create_modal_bmesh(data->shared.obact, data->shared.pbvh_type);
  if (!data->shared.bm) {
    MEM_delete(data);
    return OPERATOR_CANCELLED;
  }

  if (data->shared.pbvh_type == bke::pbvh::Type::Mesh) {
    const Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
    data->shared.preview_positions = bke::pbvh::vert_positions_eval(depsgraph, *data->shared.obact);
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
  if (!region_is_valid(data->shared)) {
    sync_preview_from_hover(*data);
  }

  if (!region_is_valid(data->shared)) {
    BKE_report(CTX_wm_reports(C), RPT_WARNING, "No region under cursor");
    gesture_data_free(C, data);
    op->customdata = nullptr;
    extract_region_hover_deactivate();
    return OPERATOR_CANCELLED;
  }

  const bool is_extrude = OutputType(RNA_enum_get(op->ptr, "output_type")) == OutputType::Extrude;
  if (!is_extrude) {
    finish(C, op, data);
    return OPERATOR_FINISHED;
  }

  /* Begin the extrude immediately from the invoke press so the user can drag with the
   * mouse held down to set the distance — no separate click is needed to start it. */
  data->extrude.init_mval = float2(float(event->mval[0]), float(event->mval[1]));
  if (!extract::extrude_begin(
          *C, data->shared, data->extrude, data->shared.preview_faces, data->extrude.init_mval))
  {
    gesture_data_free(C, data);
    op->customdata = nullptr;
    extract_region_hover_deactivate();
    return OPERATOR_CANCELLED;
  }

  update_status_bar(C, data, op);
  extract_region_hover_activate();
  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

static void operator_properties(wmOperatorType *ot)
{
  RNA_def_enum(
      ot->srna, "region_source", region_source_items, int(RegionSource::FaceSet), "Source", "");
  RNA_def_enum(ot->srna, "output_type", output_type_items, int(OutputType::Extrude), "Output Type", "");
  RNA_def_float(ot->srna,
                "mask_threshold",
                0.5f,
                0.0f,
                1.0f,
                "Mask Threshold",
                "Minimum mask value for a face to be part of the region",
                0.0f,
                1.0f);
}

wmKeyMap *modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {0, "CONFIRM", 0, "Confirm", ""},
      {1, "CANCEL", 0, "Cancel", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Sculpt Extract Region Modal";
  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);
  if (keymap && keymap->modal_items) {
    return keymap;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  WM_modalkeymap_assign(keymap, "SCULPT_OT_extract_region");
  return keymap;
}

void SCULPT_OT_extract_region(wmOperatorType *ot)
{
  ot->name = "Extract Region";
  ot->idname = "SCULPT_OT_extract_region";
  ot->description =
      "Select the connected face region under the cursor by face set or mask, then extrude it "
      "along normals or extract it as a new mesh object";

  ot->invoke = gesture_invoke;
  ot->modal = gesture_modal;
  ot->cancel = gesture_cancel;
  ot->poll = sculpt_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  operator_properties(ot);
}

}  // namespace blender::ed::sculpt_paint::extract_region
