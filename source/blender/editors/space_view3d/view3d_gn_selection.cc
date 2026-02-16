/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_set.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_modifier.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_gn_selection.hh"
#include "ED_screen.hh"
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
    return false;
  }

  ModifierData *md = BKE_modifiers_findby_type(ob, ModifierType::eModifierType_Nodes);
  if (!md) {
    return false;
  }

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
  Object *ob = CTX_data_active_object(C);
  if (!ob) {
    return false;
  }

  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(
      BKE_modifiers_findby_type(ob, ModifierType::eModifierType_Nodes));
  if (!nmd || !nmd->node_group) {
    return false;
  }

  /* Find 3D View Selection node if not provided */
  if (!node) {
    for (bNode *n : nmd->node_group->all_nodes()) {
      if (n->type_legacy == GEO_NODE_3D_VIEW_SELECTION) {
        node = n;
        break;
      }
    }
  }
  if (!node) {
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

  /* Load existing selection */
  if (storage.selected_ids != nullptr && storage.selected_ids_num > 0) {
    for (int i = 0; i < storage.selected_ids_num; i++) {
      data->current_selection.add(storage.selected_ids[i]);
    }
  }

  /* Set object mode */
  ob->mode = eObjectMode(ob->mode | OB_MODE_GN_SELECTION);
  gn_selection_mode_data = data;

  WM_event_add_notifier(C, NC_OBJECT | ND_MODE, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);

  return true;
}

void ED_gn_selection_mode_exit(bContext *C, bool confirm)
{
  if (!gn_selection_mode_data) {
    return;
  }

  GNSelectionModeData *data = gn_selection_mode_data;
  Object *ob = data->object;

  if (confirm && data->selection_changed) {
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
      storage.selected_ids = MEM_new_array_zeroed<int>(num, __func__);
      int i = 0;
      for (const int id : data->current_selection) {
        storage.selected_ids[i++] = id;
      }
      storage.selected_ids_num = num;
    }
    else {
      storage.selected_ids_num = 0;
    }

    /* Tag for update */
    WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  }

  /* Restore previous mode */
  ob->mode = data->previous_mode;

  MEM_delete(data);
  gn_selection_mode_data = nullptr;

  WM_event_add_notifier(C, NC_OBJECT | ND_MODE, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_SELECT, ob);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static bool gn_select_poll(bContext *C)
{
  return ED_gn_selection_mode_active(CTX_data_active_object(C));
}

static wmOperatorStatus gn_select_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  GNSelectionModeData *data = ED_gn_selection_mode_data_get(ob);

  if (!data) {
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  RNA_int_get_array(op->ptr, "mouse", mval);

  const bool extend = RNA_boolean_get(op->ptr, "extend");
  const bool deselect = RNA_boolean_get(op->ptr, "deselect");

  /* TODO: Implement raycast to find element under cursor */
  /* For now, just demonstrate the structure */

  int hit_index = -1; /* Would come from raycast */

  if (hit_index >= 0) {
    if (deselect) {
      data->current_selection.remove(hit_index);
    }
    else if (extend) {
      data->current_selection.add(hit_index);
    }
    else {
      /* Toggle */
      if (data->current_selection.contains(hit_index)) {
        data->current_selection.remove(hit_index);
      }
      else {
        data->current_selection.add(hit_index);
      }
    }
    data->selection_changed = true;
  }

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

  /* Confirmation */
  keymap_item_add(keymap, "GN_OT_selection_confirm", EVT_RETKEY, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_selection_confirm", EVT_PADENTER, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "GN_OT_selection_cancel", EVT_ESCKEY, KM_PRESS, 0, KM_ANY);

  /* View navigation */
  keymap_item_add(keymap, "VIEW3D_OT_rotate", MIDDLEMOUSE, KM_PRESS, 0, KM_ANY);
  keymap_item_add(keymap, "VIEW3D_OT_move", MIDDLEMOUSE, KM_PRESS, KM_SHIFT, KM_ANY);
  keymap_item_add(keymap, "VIEW3D_OT_zoom", MIDDLEMOUSE, KM_PRESS, KM_CTRL, KM_ANY);
}

/** \} */

}  // namespace blender
