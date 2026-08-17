/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 * \brief Create a driver from a custom property by picking a target with an eyedropper.
 *
 * The operator is started from the custom-property UI (see `rna_prop_ui.py`). It resolves the
 * source custom property up front and enters a modal "eyedropper" mode. The user then clicks a
 * node input socket or any animatable field in another editor; a Python driver referencing the
 * source property is created on that target.
 */

#include <algorithm>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_anim_types.h"
#include "DNA_node_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_anim_data.hh"
#include "BKE_context.hh"
#include "BKE_fcurve.hh"
#include "BKE_node_runtime.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_keyframing.hh"
#include "ED_screen.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "node_intern.hh" /* own include */

namespace blender::ed::space_node {

/* -------------------------------------------------------------------- */
/** \name Modal State
 * \{ */

/** Modal state for the "create driver from property" eyedropper. */
struct PropertyDriverDrag {
  /** The source custom property, resolved at invoke time. */
  ID *source_id = nullptr;
  std::string source_rna_path;
  PropertyType source_type = PROP_FLOAT;

  /** Node editor under the cursor (updated while hovering), used for socket highlighting. */
  SpaceNode *snode = nullptr;
  ARegion *region = nullptr;

  /** Whether at least one driver was created (drives the cancel/finish return code for undo). */
  bool any_applied = false;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Driver Creation
 * \{ */

/**
 * Create (replacing any existing) a Python driver on the destination channel, driven by a single
 * variable referencing the dragged source property.
 */
static bool driver_create_from_source(ReportList *reports,
                                      ID *dst_id,
                                      const char *dst_path,
                                      const int dst_index,
                                      const PropertyDriverDrag &src)
{
  /* Replace any existing driver on this exact channel so variables don't accumulate. */
  if (AnimData *adt = BKE_animdata_from_id(dst_id)) {
    if (FCurve *existing = BKE_fcurve_find(&adt->drivers, dst_path, dst_index)) {
      BLI_remlink(&adt->drivers, existing);
      BKE_fcurve_free(existing);
    }
  }

  return ANIM_add_driver_with_target(reports,
                                     dst_id,
                                     dst_path,
                                     dst_index,
                                     src.source_id,
                                     src.source_rna_path.c_str(),
                                     0,
                                     0,
                                     DRIVER_TYPE_PYTHON,
                                     CREATEDRIVER_MAPPING_1_1) > 0;
}

static void tag_driver_update(bContext *C, ID *dst_id, const bool node_edited)
{
  DEG_relations_tag_update(CTX_data_main(C));
  DEG_id_tag_update(dst_id, ID_RECALC_ANIMATION);
  WM_event_add_notifier(C, NC_ANIMATION | ND_FCURVES_ORDER, nullptr);
  if (node_edited) {
    WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Target Resolution
 * \{ */

static bool socket_accepts_property(const PropertyType prop_type,
                                    const eNodeSocketDatatype socket_type)
{
  switch (prop_type) {
    case PROP_FLOAT:
      return ELEM(socket_type, SOCK_FLOAT, SOCK_INT, SOCK_VECTOR, SOCK_RGBA);
    case PROP_INT:
      return ELEM(socket_type, SOCK_FLOAT, SOCK_INT);
    case PROP_BOOLEAN:
      return socket_type == SOCK_BOOLEAN;
    default:
      return false;
  }
}

/** Find the node input socket under the cursor, updating the drag's node-editor references. */
static bNodeSocket *driver_drag_find_socket(bContext &C,
                                            const wmEvent &event,
                                            PropertyDriverDrag &drag)
{
  wmWindow *win = CTX_wm_window(&C);
  if (!win) {
    return nullptr;
  }
  bScreen *screen = WM_window_get_active_screen(win);
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_NODE, event.xy);
  if (!area) {
    return nullptr;
  }
  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event.xy);
  if (!region) {
    return nullptr;
  }
  SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
  if (!snode || !snode->edittree) {
    return nullptr;
  }

  const int2 mval = {event.xy[0] - region->winrct.xmin, event.xy[1] - region->winrct.ymin};
  float2 cursor;
  ui::view2d_region_to_view(&region->v2d, mval[0], mval[1], &cursor.x, &cursor.y);

  drag.snode = snode;
  drag.region = region;
  return node_find_indicated_socket(*snode, *region, cursor, SOCK_IN);
}

/** Create driver(s) on a node input socket (looping over vector/color components). */
static bool driver_apply_to_socket(bContext *C,
                                   PropertyDriverDrag &drag,
                                   bNodeSocket &socket,
                                   ReportList *reports)
{
  SpaceNode *snode = drag.snode;
  if (!snode || !snode->edittree) {
    return false;
  }
  if (!socket_accepts_property(drag.source_type, eNodeSocketDatatype(socket.type))) {
    BKE_report(reports, RPT_ERROR, "Property type is not compatible with the socket type");
    return false;
  }

  const bNode &node = socket.owner_node();
  const int sock_index = BLI_findindex(&node.inputs, &socket);
  if (sock_index == -1) {
    BKE_report(reports, RPT_ERROR, "Cannot add a driver to an output socket");
    return false;
  }

  ID *dst_id = &snode->edittree->id;
  const std::string dst_path = fmt::format(
      "nodes[\"{}\"].inputs[{}].default_value", node.name, sock_index);

  /* Vector and color sockets expose multiple components; drive each from the scalar source. */
  int array_len = 1;
  PointerRNA id_ptr = RNA_id_pointer_create(dst_id);
  PointerRNA ptr;
  PropertyRNA *prop;
  if (RNA_path_resolve_property(&id_ptr, dst_path.c_str(), &ptr, &prop)) {
    if (RNA_property_array_check(prop)) {
      array_len = RNA_property_array_length(&ptr, prop);
    }
  }

  int created = 0;
  for (int i = 0; i < array_len; i++) {
    if (driver_create_from_source(reports, dst_id, dst_path.c_str(), i, drag)) {
      created++;
    }
  }
  if (created == 0) {
    BKE_report(reports, RPT_ERROR, "Failed to create driver");
    return false;
  }

  tag_driver_update(C, dst_id, true);
  return true;
}

/** Create a driver on an animatable field (button) under the cursor. */
static bool driver_apply_to_button(bContext *C,
                                   PropertyDriverDrag &drag,
                                   PointerRNA &but_ptr,
                                   PropertyRNA *but_prop,
                                   const int but_index,
                                   ReportList *reports)
{
  ID *dst_id = but_ptr.owner_id;
  if (!dst_id) {
    return false;
  }

  const PropertyType prop_type = RNA_property_type(but_prop);
  if (!RNA_property_animateable(&but_ptr, but_prop) ||
      !ELEM(prop_type, PROP_FLOAT, PROP_INT, PROP_BOOLEAN))
  {
    BKE_report(reports, RPT_WARNING, "This field does not support drivers");
    return false;
  }

  /* The full path handles nested data (modifiers, constraints, shape keys, …). */
  const std::optional<std::string> dst_path = RNA_path_from_ID_to_property(&but_ptr, but_prop);
  if (!dst_path) {
    return false;
  }

  const int dst_index = RNA_property_array_check(but_prop) ? std::max(but_index, 0) : 0;
  if (!driver_create_from_source(reports, dst_id, dst_path->c_str(), dst_index, drag)) {
    BKE_report(reports, RPT_ERROR, "Failed to create driver");
    return false;
  }

  tag_driver_update(C, dst_id, false);
  return true;
}

/** Create a driver on an animatable field under the cursor in any editor. */
static bool driver_apply_to_field_under_cursor(bContext *C,
                                               PropertyDriverDrag &drag,
                                               const wmEvent *event,
                                               ReportList *reports)
{
  wmWindow *win = CTX_wm_window(C);
  if (!win) {
    return false;
  }
  bScreen *screen = WM_window_get_active_screen(win);
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event->xy);
  if (!area) {
    return false;
  }

  /* The property may live in any region of the area (main window, N-panel, …). */
  ARegion *region = nullptr;
  for (ARegion *ar = static_cast<ARegion *>(area->regionbase.first); ar;
       ar = static_cast<ARegion *>(ar->next))
  {
    if (BLI_rcti_isect_pt_v(&ar->winrct, event->xy)) {
      region = ar;
      break;
    }
  }
  if (!region) {
    return false;
  }

  PointerRNA but_ptr;
  PropertyRNA *but_prop;
  int but_index;
  if (!ui::but_mouse_over_prop_get(region, event, &but_ptr, &but_prop, &but_index)) {
    return false;
  }
  return driver_apply_to_button(C, drag, but_ptr, but_prop, but_index, reports);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modal Operator
 * \{ */

static void driver_drag_set_status(bContext *C)
{
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EYEDROPPER);
  WorkspaceStatus status(C);
  status.item(IFACE_("Assign Driver"), ICON_MOUSE_LMB);
  status.item(IFACE_("Multiple"), ICON_EVENT_SHIFT, ICON_MOUSE_LMB);
  status.item(IFACE_("Finish"), ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
}

static void driver_from_property_exit(bContext *C, wmOperator *op)
{
  PropertyDriverDrag *drag = static_cast<PropertyDriverDrag *>(op->customdata);
  if (drag) {
    if (drag->snode && drag->snode->runtime) {
      drag->snode->runtime->highlighted_socket = nullptr;
    }
    if (drag->region) {
      ED_region_tag_redraw(drag->region);
    }
    MEM_delete(drag);
    op->customdata = nullptr;
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_workspace_status_text(C, nullptr);
}

static wmOperatorStatus driver_from_property_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  PropertyDriverDrag *drag = static_cast<PropertyDriverDrag *>(op->customdata);
  if (!drag) {
    return OPERATOR_CANCELLED;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      bNodeSocket *socket = driver_drag_find_socket(*C, *event, *drag);
      if (drag->snode && drag->snode->runtime &&
          drag->snode->runtime->highlighted_socket != socket)
      {
        drag->snode->runtime->highlighted_socket = socket;
        if (drag->region) {
          ED_region_tag_redraw(drag->region);
        }
      }
      driver_drag_set_status(C);
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      if (event->val != KM_PRESS) {
        return OPERATOR_RUNNING_MODAL;
      }
      bool applied = false;
      if (bNodeSocket *socket = driver_drag_find_socket(*C, *event, *drag)) {
        applied = driver_apply_to_socket(C, *drag, *socket, op->reports);
      }
      else {
        applied = driver_apply_to_field_under_cursor(C, *drag, event, op->reports);
      }
      drag->any_applied |= applied;

      if (applied && !(event->modifier & KM_SHIFT)) {
        driver_from_property_exit(C, op);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_RETKEY: {
      const bool any_applied = drag->any_applied;
      driver_from_property_exit(C, op);
      return any_applied ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
    }
    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      /* Keep already-created drivers (Shift mode) so they land in a single undo step. */
      const bool any_applied = drag->any_applied;
      driver_from_property_exit(C, op);
      return any_applied ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
    }
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus driver_from_property_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent * /*event*/)
{
  char data_path[256];
  char property_name[MAX_IDPROP_NAME];
  RNA_string_get(op->ptr, "data_path", data_path);
  RNA_string_get(op->ptr, "property_name", property_name);
  if (!data_path[0] || !property_name[0]) {
    return OPERATOR_CANCELLED;
  }

  /* The data-path is a context member, optionally followed by an RNA sub-path. Resolve the member
   * first, then the remainder relative to it. */
  PointerRNA ptr = {};
  char *dot = strchr(data_path, '.');
  if (dot) {
    *dot = '\0';
  }
  ptr = CTX_data_pointer_get(C, data_path);
  if (ptr.type == nullptr) {
    PointerRNA ctx_ptr = RNA_pointer_create_discrete(nullptr, RNA_Context, (void *)C);
    if (!RNA_path_resolve(&ctx_ptr, data_path, &ptr, nullptr)) {
      ptr = {};
    }
  }
  if (dot) {
    *dot = '.';
    if (ptr.type != nullptr) {
      PointerRNA resolved;
      if (RNA_path_resolve(&ptr, dot + 1, &resolved, nullptr)) {
        ptr = resolved;
      }
      else {
        ptr = {};
      }
    }
  }

  if (!ptr.data || !ptr.owner_id) {
    return OPERATOR_CANCELLED;
  }

  /* Build the source RNA path. Custom properties live either directly on an ID (`["prop"]`) or on
   * a node inside a node tree (`nodes["Node"]["prop"]`). */
  std::string source_rna_path;
  if (ptr.type && RNA_struct_is_a(ptr.type, RNA_Node)) {
    const bNode *node = static_cast<const bNode *>(ptr.data);
    source_rna_path = fmt::format("nodes[\"{}\"][\"{}\"]", node->name, property_name);
  }
  else {
    source_rna_path = fmt::format("[\"{}\"]", property_name);
  }

  /* Resolve the source property to record its type (needed for socket compatibility). */
  PropertyType source_type = PROP_FLOAT;
  PointerRNA src_id_ptr = RNA_id_pointer_create(ptr.owner_id);
  PointerRNA src_ptr;
  PropertyRNA *src_prop;
  if (RNA_path_resolve_property(&src_id_ptr, source_rna_path.c_str(), &src_ptr, &src_prop)) {
    source_type = RNA_property_type(src_prop);
  }

  PropertyDriverDrag *drag = MEM_new<PropertyDriverDrag>(__func__);
  drag->source_id = ptr.owner_id;
  drag->source_rna_path = std::move(source_rna_path);
  drag->source_type = source_type;
  drag->snode = CTX_wm_space_node(C);
  drag->region = CTX_wm_region(C);

  op->customdata = drag;
  WM_event_add_modal_handler(C, op);
  driver_drag_set_status(C);
  return OPERATOR_RUNNING_MODAL;
}

static bool driver_from_property_poll(bContext *C)
{
  return CTX_wm_window(C) != nullptr;
}

void NODE_OT_driver_from_property(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Create Driver from Property";
  ot->idname = "NODE_OT_driver_from_property";
  ot->description = "Pick a node socket or animatable field to drive with this custom property";

  /* API callbacks. */
  ot->invoke = driver_from_property_invoke;
  ot->modal = driver_from_property_modal;
  ot->cancel = driver_from_property_exit;
  ot->poll = driver_from_property_poll;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_INTERNAL;

  /* Properties. */
  RNA_def_string(
      ot->srna, "data_path", nullptr, 256, "Data Path", "Context path to the property owner");
  RNA_def_string(ot->srna,
                 "property_name",
                 nullptr,
                 MAX_IDPROP_NAME,
                 "Property Name",
                 "Name of the custom property");
}

/** \} */

}  // namespace blender::ed::space_node
