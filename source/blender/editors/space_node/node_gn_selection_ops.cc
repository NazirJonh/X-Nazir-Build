/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include "DNA_node_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"

#include "ED_gn_selection.hh"
#include "ED_node_c.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Enter Selection Mode Operator
 * \{ */

static bool node_gn_selection_enter_poll(bContext *C)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  if (!snode || !snode->edittree) {
    return false;
  }

  const bNode *node = static_cast<const bNode *>(CTX_data_pointer_get_type(C, "node", RNA_Node).data);
  if (!node || node->type_legacy != GEO_NODE_3D_VIEW_SELECTION) {
    return false;
  }

  return ED_gn_selection_mode_poll(C);
}

static wmOperatorStatus node_gn_selection_enter_exec(bContext *C, wmOperator * /*op*/)
{
  bNode *node = static_cast<bNode *>(CTX_data_pointer_get_type(C, "node", RNA_Node).data);

  if (ED_gn_selection_mode_enter(C, node)) {
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void NODE_OT_gn_selection_enter(wmOperatorType *ot)
{
  ot->name = "Enter Selection Mode";
  ot->idname = "NODE_OT_gn_selection_enter";
  ot->description = "Enter 3D View selection mode for this node";

  ot->exec = node_gn_selection_enter_exec;
  ot->poll = node_gn_selection_enter_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clear Selection Operator
 * \{ */

static wmOperatorStatus node_gn_selection_clear_exec(bContext *C, wmOperator * /*op*/)
{
  bNode *node = static_cast<bNode *>(CTX_data_pointer_get_type(C, "node", RNA_Node).data);
  if (!node || node->type_legacy != GEO_NODE_3D_VIEW_SELECTION) {
    return OPERATOR_CANCELLED;
  }

  NodeGeometry3DViewSelection &storage =
      *static_cast<NodeGeometry3DViewSelection *>(node->storage);

  if (storage.selected_ids) {
    MEM_delete_void<void>(storage.selected_ids);
    storage.selected_ids = nullptr;
  }
  storage.selected_ids_num = 0;

  WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  return OPERATOR_FINISHED;
}

void NODE_OT_gn_selection_clear(wmOperatorType *ot)
{
  ot->name = "Clear Selection";
  ot->idname = "NODE_OT_gn_selection_clear";
  ot->description = "Clear the selection in this node";

  ot->exec = node_gn_selection_clear_exec;
  ot->poll = node_gn_selection_enter_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender
