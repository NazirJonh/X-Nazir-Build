/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 */

#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>

#include <fmt/format.h>

#include "AS_asset_representation.hh"

#include "MEM_guardedalloc.h"

#include "DNA_collection_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"

#include "BLT_translation.hh"

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_object.hh"
#include "ED_outliner.hh"
#include "ED_outliner_stack_automation.hh"
#include "ED_screen.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "ED_outliner_stack_drag.hh"
#include "outliner_stack_source.hh"
#include "tree/tree_iterator.hh"

namespace blender {

namespace ed::outliner {

/** \name Data Stack Drop Operator
 *
 * A generic operator to allow drag and drop for modifiers, constraints,
 * and shader effects which all share the same UI stack layout.
 *
 * The following operations are allowed:
 * - Reordering within an object.
 * - Copying a single modifier/constraint/effect to another object.
 * - Copying (linking) an object's modifiers/constraints/effects to another.
 * \{ */

enum eDataStackDropAction {
  DATA_STACK_DROP_REORDER,
  DATA_STACK_DROP_COPY,
  DATA_STACK_DROP_LINK,
};

struct StackDropData {
  Object *ob_parent;
  bPoseChannel *pchan_parent;
  TreeStoreElem *drag_tselem;
  void *drag_directdata;
  int drag_index;

  eDataStackDropAction drop_action;
  TreeElement *drop_te;
  TreeElementInsertType insert_type;
};

void datastack_drop_data_init(wmDrag *drag,
                              Object *ob,
                              bPoseChannel *pchan,
                              TreeElement *te,
                              TreeStoreElem *tselem,
                              void *directdata)
{
  StackDropData *drop_data = MEM_new_zeroed<StackDropData>("datastack drop data");

  drop_data->ob_parent = ob;
  drop_data->pchan_parent = pchan;
  drop_data->drag_tselem = tselem;
  drop_data->drag_directdata = directdata;
  drop_data->drag_index = te->index;

  drag->poin = drop_data;
  drag->flags |= WM_DRAG_FREE_DATA;
}

static bool datastack_drop_init(bContext *C, const wmEvent *event, StackDropData *drop_data)
{
  if (!ELEM(drop_data->drag_tselem->type,
            TSE_MODIFIER,
            TSE_MODIFIER_BASE,
            TSE_CONSTRAINT,
            TSE_CONSTRAINT_BASE,
            TSE_GPENCIL_EFFECT,
            TSE_GPENCIL_EFFECT_BASE))
  {
    return false;
  }

  TreeElement *te_target = outliner_drop_insert_find(C, event->xy, &drop_data->insert_type);
  if (!te_target) {
    return false;
  }
  TreeStoreElem *tselem_target = TREESTORE(te_target);

  if (drop_data->drag_tselem == tselem_target) {
    return false;
  }

  Object *ob = nullptr;
  TreeElement *object_te = outliner_data_from_tree_element_and_parents(is_object_element,
                                                                       te_target);
  if (object_te) {
    ob = id_cast<Object *>(TREESTORE(object_te)->id);
  }

  bPoseChannel *pchan = nullptr;
  TreeElement *pchan_te = outliner_data_from_tree_element_and_parents(is_pchan_element, te_target);
  if (pchan_te) {
    pchan = static_cast<bPoseChannel *>(pchan_te->directdata);
  }
  if (pchan) {
    ob = nullptr;
  }

  if (ob && !BKE_id_is_editable(CTX_data_main(C), &ob->id)) {
    return false;
  }

  /* Drag a base for linking. */
  if (ELEM(drop_data->drag_tselem->type,
           TSE_MODIFIER_BASE,
           TSE_CONSTRAINT_BASE,
           TSE_GPENCIL_EFFECT_BASE))
  {
    drop_data->insert_type = TE_INSERT_INTO;
    drop_data->drop_action = DATA_STACK_DROP_LINK;

    if (pchan && pchan != drop_data->pchan_parent) {
      drop_data->drop_te = pchan_te;
      tselem_target = TREESTORE(pchan_te);
    }
    else if (ob && ob != drop_data->ob_parent) {
      drop_data->drop_te = object_te;
      tselem_target = TREESTORE(object_te);
    }
    else {
      return false;
    }
  }
  else if (ob || pchan) {
    /* Drag a single item. */
    if (pchan && pchan != drop_data->pchan_parent) {
      drop_data->insert_type = TE_INSERT_INTO;
      drop_data->drop_action = DATA_STACK_DROP_COPY;
      drop_data->drop_te = pchan_te;
      tselem_target = TREESTORE(pchan_te);
    }
    else if (ob && ob != drop_data->ob_parent) {
      drop_data->insert_type = TE_INSERT_INTO;
      drop_data->drop_action = DATA_STACK_DROP_COPY;
      drop_data->drop_te = object_te;
      tselem_target = TREESTORE(object_te);
    }
    else if (tselem_target->type == drop_data->drag_tselem->type) {
      if (drop_data->insert_type == TE_INSERT_INTO) {
        return false;
      }
      drop_data->drop_action = DATA_STACK_DROP_REORDER;
      drop_data->drop_te = te_target;
    }
    else {
      return false;
    }
  }
  else {
    return false;
  }

  return true;
}

/* Ensure that grease pencil and object data remain separate. */
static bool datastack_drop_are_types_valid(StackDropData *drop_data)
{
  TreeStoreElem *tselem = TREESTORE(drop_data->drop_te);
  Object *ob_parent = drop_data->ob_parent;
  Object *ob_dst = id_cast<Object *>(tselem->id);

  /* Don't allow data to be moved between objects and bones. */
  if (tselem->type == TSE_CONSTRAINT) {
  }
  else if ((drop_data->pchan_parent && tselem->type != TSE_POSE_CHANNEL) ||
           (!drop_data->pchan_parent && tselem->type == TSE_POSE_CHANNEL))
  {
    return false;
  }

  switch (drop_data->drag_tselem->type) {
    case TSE_MODIFIER_BASE:
    case TSE_MODIFIER:
      return (ob_parent->type == OB_GREASE_PENCIL) == (ob_dst->type == OB_GREASE_PENCIL);
      break;
    case TSE_CONSTRAINT_BASE:
    case TSE_CONSTRAINT:

      break;
    case TSE_GPENCIL_EFFECT_BASE:
    case TSE_GPENCIL_EFFECT:
      return ob_parent->type == OB_GREASE_PENCIL && ob_dst->type == OB_GREASE_PENCIL;
      break;
    default:
      break;
  }

  return true;
}

bool datastack_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (drag->type != WM_DRAG_DATASTACK) {
    return false;
  }

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  bool changed = outliner_flag_set(*space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);

  StackDropData *drop_data = static_cast<StackDropData *>(drag->poin);
  if (!drop_data) {
    return false;
  }

  if (!datastack_drop_init(C, event, drop_data)) {
    return false;
  }

  if (!datastack_drop_are_types_valid(drop_data)) {
    return false;
  }

  TreeStoreElem *tselem_target = TREESTORE(drop_data->drop_te);
  switch (drop_data->insert_type) {
    case TE_INSERT_BEFORE:
      tselem_target->flag |= TSE_DRAG_BEFORE;
      break;
    case TE_INSERT_AFTER:
      tselem_target->flag |= TSE_DRAG_AFTER;
      break;
    case TE_INSERT_INTO:
      tselem_target->flag |= TSE_DRAG_INTO;
      break;
  }

  if (changed) {
    ED_region_tag_redraw_no_rebuild(region);
  }

  return true;
}

std::string datastack_drop_tooltip(bContext * /*C*/,
                                          wmDrag *drag,
                                          const int /*xy*/[2],
                                          wmDropBox * /*drop*/)
{
  StackDropData *drop_data = static_cast<StackDropData *>(drag->poin);
  switch (drop_data->drop_action) {
    case DATA_STACK_DROP_REORDER:
      return TIP_("Reorder");
    case DATA_STACK_DROP_COPY:
      if (drop_data->pchan_parent) {
        return TIP_("Copy to bone");
      }
      return TIP_("Copy to object");

    case DATA_STACK_DROP_LINK:
      if (drop_data->pchan_parent) {
        return TIP_("Link all to bone");
      }
      return TIP_("Link all to object");
  }
  return {};
}

static void datastack_drop_link(bContext *C, StackDropData *drop_data)
{
  Main *bmain = CTX_data_main(C);
  TreeStoreElem *tselem = TREESTORE(drop_data->drop_te);
  Object *ob_dst = id_cast<Object *>(tselem->id);

  switch (drop_data->drag_tselem->type) {
    case TSE_MODIFIER_BASE:
      object::modifier_link(C, ob_dst, drop_data->ob_parent);
      break;
    case TSE_CONSTRAINT_BASE: {
      ListBaseT<bConstraint> *src;

      if (drop_data->pchan_parent) {
        src = &drop_data->pchan_parent->constraints;
      }
      else {
        src = &drop_data->ob_parent->constraints;
      }

      ListBaseT<bConstraint> *dst;
      if (tselem->type == TSE_POSE_CHANNEL) {
        bPoseChannel *pchan = static_cast<bPoseChannel *>(drop_data->drop_te->directdata);
        dst = &pchan->constraints;
      }
      else {
        dst = &ob_dst->constraints;
      }

      object::constraint_link(bmain, ob_dst, dst, src);
      break;
    }
    case TSE_GPENCIL_EFFECT_BASE:
      if (ob_dst->type != OB_GREASE_PENCIL) {
        return;
      }

      object::shaderfx_link(ob_dst, drop_data->ob_parent);
      break;
    default:
      break;
  }
}

static void datastack_drop_copy(bContext *C, StackDropData *drop_data)
{
  Main *bmain = CTX_data_main(C);

  TreeStoreElem *tselem = TREESTORE(drop_data->drop_te);
  Object *ob_dst = id_cast<Object *>(tselem->id);

  switch (drop_data->drag_tselem->type) {
    case TSE_MODIFIER: {
      ModifierData *md_dst = object::modifier_copy_to_object(
          bmain,
          CTX_data_scene(C),
          drop_data->ob_parent,
          static_cast<const ModifierData *>(drop_data->drag_directdata),
          ob_dst,
          CTX_wm_reports(C));
      BKE_object_modifier_set_active(ob_dst, md_dst);
      break;
    }
    case TSE_CONSTRAINT:
      if (tselem->type == TSE_POSE_CHANNEL) {
        object::constraint_copy_for_pose(
            bmain,
            ob_dst,
            static_cast<bPoseChannel *>(drop_data->drop_te->directdata),
            static_cast<bConstraint *>(drop_data->drag_directdata));
      }
      else {
        object::constraint_copy_for_object(
            bmain, ob_dst, static_cast<bConstraint *>(drop_data->drag_directdata));
      }
      break;
    case TSE_GPENCIL_EFFECT: {
      if (ob_dst->type != OB_GREASE_PENCIL) {
        return;
      }

      object::shaderfx_copy(ob_dst, static_cast<ShaderFxData *>(drop_data->drag_directdata));
      break;
    }
    default:
      break;
  }
}

static void datastack_drop_reorder(bContext *C, ReportList *reports, StackDropData *drop_data)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

  TreeElement *drag_te = outliner_find_tree_element(&space_outliner->runtime->tree,
                                                    drop_data->drag_tselem);
  if (!drag_te) {
    return;
  }

  TreeElement *drop_te = drop_data->drop_te;
  TreeElementInsertType insert_type = drop_data->insert_type;

  Object *ob = drop_data->ob_parent;

  int index = 0;
  switch (drop_data->drag_tselem->type) {
    case TSE_MODIFIER:
      index = outliner_get_insert_index(drag_te, drop_te, insert_type, &ob->modifiers);
      object::modifier_move_to_index(reports,
                                     RPT_WARNING,
                                     ob,
                                     static_cast<ModifierData *>(drop_data->drag_directdata),
                                     index,
                                     true);
      break;
    case TSE_CONSTRAINT:
      if (drop_data->pchan_parent) {
        index = outliner_get_insert_index(
            drag_te, drop_te, insert_type, &drop_data->pchan_parent->constraints);
      }
      else {
        index = outliner_get_insert_index(drag_te, drop_te, insert_type, &ob->constraints);
      }
      object::constraint_move_to_index(
          ob, static_cast<bConstraint *>(drop_data->drag_directdata), index);

      break;
    case TSE_GPENCIL_EFFECT:
      index = outliner_get_insert_index(drag_te, drop_te, insert_type, &ob->shader_fx);
      object::shaderfx_move_to_index(
          reports, ob, static_cast<ShaderFxData *>(drop_data->drag_directdata), index);
      break;
    default:
      break;
  }
}

static wmOperatorStatus datastack_drop_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }

  ListBaseT<wmDrag> *lb = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);
  StackDropData *drop_data = static_cast<StackDropData *>(drag->poin);

  switch (drop_data->drop_action) {
    case DATA_STACK_DROP_LINK:
      datastack_drop_link(C, drop_data);
      break;
    case DATA_STACK_DROP_COPY:
      datastack_drop_copy(C, drop_data);
      break;
    case DATA_STACK_DROP_REORDER:
      datastack_drop_reorder(C, op->reports, drop_data);
      break;
  }

  return OPERATOR_FINISHED;
}

void OUTLINER_OT_datastack_drop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Data Stack Drop";
  ot->description = "Copy or reorder modifiers, constraints, and effects";
  ot->idname = "OUTLINER_OT_datastack_drop";

  /* API callbacks. */
  ot->invoke = datastack_drop_invoke;

  ot->poll = ED_operator_outliner_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/** \name Stack Layer Drop Operator
 *
 * Reordering a row of the Stack Layers display mode.
 *
 * A path of its own rather than an extension of the data-stack drop above: that one models
 * ownership by an #Object or a #bPoseChannel and moves a #ListBase entry, and a paint layer is
 * neither -- it is a position in a node graph, and which data-block owns it is the business of the
 * mode's #StackSource. The two share the drop-indicator helpers and nothing else.
 * \{ */

/** #StackItemIdentity of the target #wmDragStackLayer::target_owner_uid names, or an invalid one
 * when no drop zone has resolved yet. */
static StackItemIdentity stack_layer_drag_target_get(const wmDragStackLayer &drag_data)
{
  StackItemIdentity identity;
  identity.owner_uid = drag_data.target_owner_uid;
  identity.source_type = eSpaceOutliner_StackSource(drag_data.target_source_type);
  identity.row_id = drag_data.target_row_id;
  identity.ordinal_hint = drag_data.target_ordinal_hint;
  return identity;
}

static void stack_layer_drag_target_set(wmDragStackLayer &drag_data,
                                          const StackItemIdentity &identity)
{
  drag_data.target_owner_uid = identity.owner_uid;
  drag_data.target_source_type = short(identity.source_type);
  drag_data.target_row_id = identity.row_id;
  drag_data.target_ordinal_hint = identity.ordinal_hint;
}

void stack_layer_drop_data_init(SpaceOutliner &space_outliner,
                                       wmDrag *drag,
                                       const TreeStoreElem &tselem)
{
  /* #MEM_new, as the Vector member requires non-trivial construction. Value initialization leaves
   * `target_owner_uid` at 0 until a drop zone resolves one. */
  wmDragStackLayer *drop_data = MEM_new<wmDragStackLayer>("stack layer drop data");

  /* Fill from selection: all selected rows move together. The row being dragged must be included
   * even if it is not selected (standard Blender drag behavior). */
  Vector<int> selected_ordinals;
  stack_selected_ordinals_get(space_outliner, selected_ordinals);

  /* Ensure the dragged row is in the set. */
  const int dragged_ordinal = int(tselem.nr);
  if (!selected_ordinals.contains(dragged_ordinal)) {
    selected_ordinals.append(dragged_ordinal);
  }

  /* Every selected row is carried, so the selection is whole again after the move. The rows that
   * are actually *moved* are the roots of that set -- a folder already brings its contents -- and
   * the two places that need those roots prune this list themselves. */

  /* Convert ordinals to identities and sort by ordinal to preserve stack order. */
  for (int ordinal : selected_ordinals) {
    const StackItemIdentity identity = outliner_stack_identity_of(space_outliner, ordinal);
    if (!identity.is_valid()) {
      continue;
    }
    drop_data->drag_rows.append(identity);
  }

  /* Sort by ordinal_hint to preserve relative order during the move. */
  std::sort(drop_data->drag_rows.begin(),
            drop_data->drag_rows.end(),
            [](const StackItemIdentity &a, const StackItemIdentity &b) {
              return a.ordinal_hint < b.ordinal_hint;
            });

  drag->poin = drop_data;
  drag->poin_free_fn = stack_layer_drag_payload_free;
  drag->flags |= WM_DRAG_FREE_DATA;
}

/**
 * The stack row under the cursor and which of its three zones the cursor is in.
 *
 * A path of its own rather than #outliner_drop_insert_find, which answers a different question: it
 * redirects a drop near the bottom edge of an open element to that element's first child, so the
 * area under an expanded group would mean "into the group" when the user is aiming below it, and
 * it
 * scales the "into" zone to half the row, which on a two-unit row leaves almost nowhere to aim
 * above. Here every row is split in three even parts: above, into, below.
 */
static TreeElement *stack_layer_drop_zone_find(bContext *C,
                                               const int xy[2],
                                               TreeElementInsertType *r_insert_type)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  if (space_outliner->runtime->tree.is_empty()) {
    return nullptr;
  }

  float view_mval[2];
  ui::view2d_region_to_view(&region->v2d,
                            xy[0] - region->winrct.xmin,
                            xy[1] - region->winrct.ymin,
                            &view_mval[0],
                            &view_mval[1]);
  TreeElement *te = outliner_find_item_at_y(
      space_outliner, &space_outliner->runtime->tree, view_mval[1]);
  if (te == nullptr) {
    /* Past the ends of the list: above the first row, or below the last one. */
    TreeElement *first = static_cast<TreeElement *>(space_outliner->runtime->tree.first);
    TreeElement *last = static_cast<TreeElement *>(space_outliner->runtime->tree.last);
    if (view_mval[1] < last->ys) {
      *r_insert_type = TE_INSERT_AFTER;
      return last;
    }
    *r_insert_type = TE_INSERT_BEFORE;
    return first;
  }

  const float height = float(outliner_tree_element_height(*space_outliner, *te));
  const float offset = view_mval[1] - float(te->ys);
  if (offset > height * (2.0f / 3)) {
    *r_insert_type = TE_INSERT_BEFORE;
  }
  else if (offset < height * (1.0f / 3)) {
    *r_insert_type = TE_INSERT_AFTER;
  }
  else {
    *r_insert_type = TE_INSERT_INTO;
  }
  return te;
}

/** Whether \a ordinal is \a group_ordinal itself or a row that group holds, however deeply. */
static bool stack_row_is_within(const SpaceOutliner &space_outliner,
                                const int ordinal,
                                const int group_ordinal)
{
  int walk = ordinal;
  for (int step = 0; walk >= 0 && step < 64; step++) {
    if (walk == group_ordinal) {
      return true;
    }
    const StackRow *row = outliner_stack_row_find(space_outliner, walk);
    if (row == nullptr) {
      break;
    }
    walk = row->parent_ordinal;
  }
  return false;
}

static bool stack_layer_drop_init(bContext *C, const wmEvent *event, wmDragStackLayer *drop_data)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  if (!outliner_stack_can_reorder(*C, *space_outliner)) {
    return false;
  }

  /* Check that at least one dragged row resolves in this space. Rows may have been dragged from a
   * different Outliner showing a different stack (different owner or source). Resolving against
   * *this* space's stack refuses a drop between two spaces on different stacks. */
  const StackReadContext ctx = outliner_stack_read_context(*C);
  bool any_resolved = false;
  for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
    if (outliner_stack_identity_resolve(ctx, *space_outliner, drag_row) >= 0) {
      any_resolved = true;
      break;
    }
  }
  if (!any_resolved) {
    return false;
  }

  TreeElementInsertType insert_type;
  TreeElement *te = stack_layer_drop_zone_find(C, event->xy, &insert_type);
  if (te == nullptr) {
    return false;
  }
  TreeStoreElem *tselem = TREESTORE(te);
  if (tselem->type != TSE_STACK_LAYER) {
    return false;
  }

  /* Check that the target is not inside the dragged block. The target being one of the block's
   * rows is the obvious case -- a row cannot be dropped onto itself -- but aiming between two of
   * the block's own rows is the same mistake: wherever the block goes while moving, the position
   * the drop names is one the block itself vacates and refills, and honoring it scrambles the
   * rows rather than moving them. */
  const int target_ordinal = int(tselem->nr);
  int block_low = INT_MAX;
  int block_high = INT_MIN;
  for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    if (drag_ordinal < 0) {
      /* A row that no longer resolves is not part of the block: -1 in the range would stretch it
       * over the whole stack and reject drops that belong inside it. */
      continue;
    }
    if (drag_ordinal == target_ordinal) {
      return false;
    }
    /* The block is the contiguous run of top-level rows being moved. A row inside a folder is not
     * a position in that run -- its group-child ordinal is not comparable to a top-level one, and
     * it only moves because its folder does. Aiming inside a dragged folder is caught below by
     * #stack_row_is_within. */
    const StackRow *drag_stack_row = outliner_stack_row_find(*space_outliner, drag_ordinal);
    if (drag_stack_row != nullptr && drag_stack_row->parent_ordinal >= 0) {
      continue;
    }
    block_low = std::min(block_low, drag_ordinal);
    block_high = std::max(block_high, drag_ordinal);
  }
  if (block_low <= block_high && target_ordinal > block_low && target_ordinal < block_high) {
    return false;
  }

  const StackRow *target_row = outliner_stack_row_find(*space_outliner, target_ordinal);
  if (target_row == nullptr) {
    return false;
  }

  /* A row attached to a parent's content section only ever lands among its own kind: beside a
   * sibling from the same section, and never inside anything -- "into" would read as joining the
   * target's content, which is not a move between rows at all. A plain row, in turn, never lands
   * beside an attached one: the run between a parent's attached rows is that section's own
   * ordering, not a place among the plain rows. */
  for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    const StackRow *drag_stack_row = (drag_ordinal >= 0) ?
                                         outliner_stack_row_find(*space_outliner, drag_ordinal) :
                                         nullptr;
    if (drag_stack_row == nullptr) {
      continue;
    }
    if (!drag_stack_row->parent_section_id.empty()) {
      if (!stack_rows_are_siblings(*drag_stack_row, *target_row)) {
        return false;
      }
      if (insert_type == TE_INSERT_INTO) {
        /* An attached row has no inside to land in, and its rows are only one line high, so the
         * middle third of one is a dead zone. It reads as "put it here": above or below the row by
         * the half of it the pointer is in. */
        const float height = float(outliner_tree_element_height(*space_outliner, *te));
        float view_x = 0.0f;
        float view_y = 0.0f;
        ARegion *drop_region = CTX_wm_region(C);
        ui::view2d_region_to_view(&drop_region->v2d,
                                  event->xy[0] - drop_region->winrct.xmin,
                                  event->xy[1] - drop_region->winrct.ymin,
                                  &view_x,
                                  &view_y);
        insert_type = (view_y - float(te->ys) > height * 0.5f) ? TE_INSERT_BEFORE :
                                                                 TE_INSERT_AFTER;
      }
    }
    else if (!target_row->parent_section_id.empty()) {
      return false;
    }
  }

  /* A group cannot be dropped inside itself. Check each dragged row. */
  for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    if (drag_ordinal < 0) {
      continue;
    }
    const StackRow *drag_stack_row = outliner_stack_row_find(*space_outliner, drag_ordinal);
    if (drag_stack_row != nullptr && drag_stack_row->can_hold_children &&
        stack_row_is_within(*space_outliner, target_row->ordinal, drag_stack_row->ordinal))
    {
      return false;
    }
  }

  if (insert_type == TE_INSERT_INTO && !target_row->can_hold_children) {
    /* Only a row that can hold children has an inside. Aiming at the middle of a plain row is not
     * a mistake worth refusing, though -- it reads as "put it here", above the row it points
     * at. */
    insert_type = TE_INSERT_BEFORE;
  }
  /* The row aimed at is the anchor in every case, a group included: "into that folder" is a place
   * of its own, so an empty folder -- which has no row inside to aim at -- is a destination like
   * any other. The list is drawn top of stack first, so "before" on screen is "above". */
  StackMovePlace place = StackMovePlace::Above;
  if (insert_type == TE_INSERT_INTO) {
    place = StackMovePlace::Into;
  }
  else if (insert_type == TE_INSERT_AFTER) {
    place = StackMovePlace::Below;
  }

  StackItemIdentity target_identity = outliner_stack_identity_of(*space_outliner,
                                                                  target_row->ordinal);
  stack_layer_drag_target_set(*drop_data, target_identity);
  drop_data->target_place = short(place);
  /* The indicator points at *this* space's tree: it lives here, not on the drag, which may have
   * come from a different window and outlives the tree it was born over. */
  space_outliner->runtime->stack_drop_indicator.ordinal = int(TREESTORE(te)->nr);
  space_outliner->runtime->stack_drop_indicator.insert_type = insert_type;
  return true;
}

bool stack_layer_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (drag->type != WM_DRAG_STACK_LAYER) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  const bool changed = outliner_flag_set(
      *space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);

  wmDragStackLayer *drop_data = static_cast<wmDragStackLayer *>(drag->poin);
  if (drop_data == nullptr || !stack_layer_drop_init(C, event, drop_data)) {
    /* Nowhere to drop here: the line goes away with the refusal. */
    space_outliner->runtime->stack_drop_indicator.ordinal = -1;
    if (changed) {
      ED_region_tag_redraw_no_rebuild(region);
    }
    return false;
  }

  /* What is drawn comes from the runtime indicator #stack_layer_drop_init just wrote, not from the
   * tree store's drag flags: every drop poll in the Outliner clears those before deciding, so a
   * flag set here survives only until the next poll of a box that refuses. */
  ED_region_tag_redraw_no_rebuild(region);
  return true;
}

std::string stack_layer_drop_tooltip(bContext *C,
                                            wmDrag * /*drag*/,
                                            const int /*xy*/[2],
                                            wmDropBox * /*drop*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner != nullptr && space_outliner->runtime != nullptr &&
      space_outliner->runtime->stack_drop_indicator.insert_type == TE_INSERT_INTO)
  {
    return TIP_("Move layer into group");
  }
  return TIP_("Reorder layer");
}

static wmOperatorStatus stack_layer_drop_invoke(bContext *C,
                                               wmOperator * /*op*/,
                                               const wmEvent *event)
{
  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }
  ListBaseT<wmDrag> *lb = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);
  wmDragStackLayer *drop_data = static_cast<wmDragStackLayer *>(drag->poin);
  if (drop_data == nullptr || drop_data->target_owner_uid == 0 ||
      drop_data->drag_rows.is_empty())
  {
    return OPERATOR_CANCELLED;
  }

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  const StackItemIdentity anchor_identity = stack_layer_drag_target_get(*drop_data);
  const StackMovePlace place = StackMovePlace(drop_data->target_place);

  /* Move all dragged rows, preserving their relative order. The first row goes where the drop
   * asked, and every row after it goes right below the one moved before it: moving each against
   * the original anchor would stack the block in reverse. Into a group reads the same way -- the
   * first row into the folder, the rest lined up under it inside.
   *
   * Identities are resolved before each move: #StackEditor::row_move renumbers the stack, so one
   * read at the top of the loop is stale by the time the loop reaches the second row. This is the
   * core protection #StackItemIdentity exists for. */
  /* The row that was active keeps the role through the move. The mutator hands the selection to
   * whichever row it moved last, which reads right for a single-row drop and wrong for a block:
   * a drop moves rows, it does not change which row the user is painting with. */
  SpaceOutliner_Runtime &runtime = *space_outliner->runtime;
  const int active_ordinal_before = outliner_stack_active_ordinal_get(
      ctx, *space_outliner);
  const StackItemIdentity active_identity_before = (active_ordinal_before >= 0) ?
                                                       outliner_stack_identity_of(
                                                           *space_outliner,
                                                           active_ordinal_before) :
                                                       StackItemIdentity();

  /* The rows actually moved are the roots of the dragged set: a dragged folder already carries its
   * dragged descendants, and moving those again would pull them back out of it. Pruned once, by
   * the
   * ordinals the rows have now, before the first move renumbers anything. The full set is still
   * used below to put the selection back. */
  Vector<int> root_ordinals;
  for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
    const int ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    if (ordinal >= 0) {
      root_ordinals.append(ordinal);
    }
  }
  stack_ordinals_drop_covered_descendants(*space_outliner, root_ordinals);
  Vector<StackItemIdentity> rows_to_move;
  for (const int ordinal : root_ordinals) {
    rows_to_move.append(outliner_stack_identity_of(*space_outliner, ordinal));
  }

  bool any_moved = false;
  StackItemIdentity last_moved = anchor_identity;
  StackMovePlace place_now = place;
  for (const StackItemIdentity &drag_row : rows_to_move) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    const int anchor_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, last_moved);
    if (drag_ordinal < 0 || anchor_ordinal < 0) {
      /* This row no longer resolves (stack changed mid-drag or row was deleted). Skip it rather
       * than canceling the whole operation: partial success is better than abandoning the rows
       * that
       * still exist. The user sees which ones moved. */
      continue;
    }
    int new_ordinal = -1;
    if (outliner_stack_row_move(
            C, *space_outliner, drag_ordinal, anchor_ordinal, place_now, &new_ordinal))
    {
      any_moved = true;
      if (new_ordinal >= 0) {
        last_moved = outliner_stack_identity_of(*space_outliner, new_ordinal);
        place_now = StackMovePlace::Below;
      }
      else {
        /* The editor did not report where the row landed; the best the next row can do is the
         * same spot this one was aimed at. */
        last_moved = anchor_identity;
        place_now = place;
      }
    }
  }

  /* Each move activates the row it moved, including in the source data. Put the source's active
   * row back, then restore the selection for every dragged identity. A drag of a block moves rows;
   * it must not turn that block into a single selected row after it lands below itself. */
  const int active_ordinal_after = outliner_stack_identity_resolve(
      ctx, *space_outliner, active_identity_before);
  if (any_moved) {
    if (active_ordinal_after >= 0) {
      outliner_stack_row_activate(C, *space_outliner, active_ordinal_after);
    }
    for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
      state.selected = false;
      state.active = false;
    }
    for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
      /* A row the source gives no identity for cannot be addressed by this map; keying it at nil
       * would fold every such row into one shared state. */
      if (outliner_stack_identity_resolve(ctx, *space_outliner, drag_row) < 0 ||
          BLI_uuid_is_nil(drag_row.row_id))
      {
        continue;
      }
      runtime.stack_row_ui_state.lookup_or_add_default(drag_row.row_id).selected = true;
    }
    if (active_ordinal_after >= 0 && !BLI_uuid_is_nil(active_identity_before.row_id)) {
      StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(
          active_identity_before.row_id);
      state.active = true;
      state.selected = true;
    }
  }

  return any_moved ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void OUTLINER_OT_stack_layer_drop(wmOperatorType *ot)
{
  ot->name = "Stack Layer Drop";
  ot->description = "Move a layer to another position in the stack";
  ot->idname = "OUTLINER_OT_stack_layer_drop";

  ot->invoke = stack_layer_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/** \name Stack Layer Data-Block Drop
 *
 * A data-block dropped on a stack row or in empty space becomes something of the source's own
 * making -- a new layer, a channel's map, a Material layer baked from a material, or whatever a future
 * source reads it as. Which types are worth catching, how a drag not already carrying a local
 * data-block resolves into one, and the wording of the tooltip are all the source's business,
 * asked through #StackDropHandler; nothing below this point names a domain type.
 * \{ */

/**
 * The stack row the mouse points at: the layer element under the cursor, or the parent layer
 * for one of its channel sub-rows. Null is empty space or the stack's owner breadcrumb, either
 * of which reads as "the stack itself, no row" -- what that is worth is the source's call.
 */
static TreeElement *stack_drop_target_row(bContext *C,
                                          const int xy[2],
                                          TreeElementInsertType *r_insert_type = nullptr)
{
  TreeElementInsertType insert_type;
  TreeElement *te = stack_layer_drop_zone_find(C, xy, &insert_type);
  if (te == nullptr) {
    return nullptr;
  }
  TreeStoreElem *tselem = TREESTORE(te);
  if (tselem->type == TSE_STACK_ITEM && te->parent != nullptr) {
    /* A channel sub-row stands for its layer: the zone within the sub-row says nothing about
     * where a data-block would go, so it reads as the middle of the layer it belongs to. */
    te = te->parent;
    tselem = TREESTORE(te);
    insert_type = TE_INSERT_INTO;
  }
  if (tselem->type != TSE_STACK_LAYER) {
    return nullptr;
  }
  if (r_insert_type != nullptr) {
    *r_insert_type = insert_type;
  }
  return te;
}

/**
 * Where a data-block dropped at \a insert_type on the row at \a anchor_ordinal would land, as the
 * seam names places.
 *
 * The list is drawn top of stack first, so "before" on screen is the place above the row in the
 * stack. The middle third of a row is #StackMovePlace::Into -- onto the row rather than beside
 * it, which each source reads its own way: an image becomes that layer's map for a channel, and a
 * material, which is always a row of its own, has no such reading and collapses it to a place
 * beside the row.
 *
 * Below the bottom row is a place like any other -- what a base coat under the whole stack is --
 * and the source says whether its data can hold one there.
 */
static StackMovePlace stack_drop_place_from_insert_type(const TreeElementInsertType insert_type)
{
  switch (insert_type) {
    case TE_INSERT_INTO:
      return StackMovePlace::Into;
    case TE_INSERT_AFTER:
      return StackMovePlace::Below;
    case TE_INSERT_BEFORE:
      break;
  }
  return StackMovePlace::Above;
}

/**
 * Record where the drop under way would land, for the operator that commits it to read back.
 *
 * The poll is the pass that owns the question "where is the cursor": it runs on the drop event
 * itself, right before the operator is called, and it is what drew the line the user is looking
 * at. The operator asking the tree again is the same question in a different place -- a different
 * region context, a tree that may have been re-read in between -- and two answers to one question
 * is exactly how a drop lands somewhere other than where it was promised.
 *
 * \param target_row: null when the drop names no row, which leaves the anchor invalid.
 */
static void stack_drop_aim_set(SpaceOutliner &space_outliner,
                               TreeElement *target_row,
                               const StackMovePlace place)
{
  SpaceOutliner_Runtime::StackDropAim aim;
  if (target_row != nullptr) {
    aim.anchor = outliner_stack_identity_of(space_outliner, int(TREESTORE(target_row)->nr));
    aim.place = place;
  }
  space_outliner.runtime->stack_drop_aim = aim;
}

/**
 * The row a dropped data-block would land on and how, or null when the drop names no row -- empty
 * space, or the stack's own breadcrumb, which every source reads as "the stack itself".
 *
 * \param r_place: #StackMovePlace::Into for the middle of a row (onto it), otherwise the side the
 * insertion line is drawn at.
 */
static TreeElement *stack_drop_aim_find(bContext *C, const int xy[2], StackMovePlace *r_place)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  TreeElementInsertType insert_type = TE_INSERT_INTO;
  TreeElement *target_row = stack_drop_target_row(C, xy, &insert_type);
  if (target_row == nullptr || space_outliner == nullptr) {
    return nullptr;
  }
  if (outliner_stack_row_find(*space_outliner, int(TREESTORE(target_row)->nr)) == nullptr) {
    /* A row the tree lists but the model no longer holds: the rows were re-read since the tree was
     * built, and there is nothing to aim at until the next build. */
    return nullptr;
  }
  if (r_place != nullptr) {
    *r_place = stack_drop_place_from_insert_type(insert_type);
  }
  return target_row;
}

/**
 * Mark the row a dropped data-block would land on, so the drag shows where it is going: a line
 * above or below the row for an insertion, the row's own outline for a drop onto it.
 *
 * \param place: nothing for a drop that lands on the row rather than beside it.
 */
static void stack_drop_indicator_set(SpaceOutliner &space_outliner,
                                     TreeElement *target_row,
                                     const std::optional<StackMovePlace> place)
{
  SpaceOutliner_Runtime::StackDropIndicator &indicator =
      space_outliner.runtime->stack_drop_indicator;
  if (target_row == nullptr) {
    indicator.ordinal = -1;
    return;
  }
  indicator.ordinal = int(TREESTORE(target_row)->nr);
  if (!place.has_value()) {
    indicator.insert_type = TE_INSERT_INTO;
  }
  else {
    indicator.insert_type = (*place == StackMovePlace::Below) ? TE_INSERT_AFTER :
                                                                TE_INSERT_BEFORE;
  }
}

/**
 * The topmost row of the stack, or null when the tree lists none: where a drop that named no row
 * lands, since a new layer goes on top.
 *
 * Found by walking the tree rather than assuming a particular root structure. The list is built
 * top of stack first, so the first row met walking it is the top one; a collapsed group's contents
 * are skipped, since a line drawn against a row nobody can see says nothing.
 */
static TreeElement *stack_drop_top_row_get(SpaceOutliner &space_outliner)
{
  TreeElement *top_row = nullptr;
  tree_iterator::all_open(space_outliner, [&](TreeElement *te) {
    if (top_row == nullptr && TREESTORE(te)->type == TSE_STACK_LAYER) {
      top_row = te;
    }
  });
  return top_row;
}

/**
 * The one type in \a types the drag already names -- a plain local data-block, a single asset, or
 * (the first match found) an item of a multi-asset drag -- or 0 when none does, which leaves the
 * source's own #StackDropHandler::drop_external_poll to say whether the drag might still resolve
 * into something else, such as a file dropped from outside Blender.
 */
static short stack_drag_matched_id_type(const wmDrag &drag, Span<short> types)
{
  if (drag.type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (asset_drags == nullptr) {
      return 0;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ?
                                           GS(item.asset_data.local_id->name) :
                                           ID_Type(0));
      if (types.contains(short(item_idtype))) {
        return short(item_idtype);
      }
    }
    return 0;
  }
  for (const short id_type : types) {
    if (WM_drag_is_ID_type(&drag, id_type)) {
      return id_type;
    }
  }
  return 0;
}

bool stack_id_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return false;
  }
  Vector<short> types;
  handler->drop_id_types(types);
  const short matched_type = stack_drag_matched_id_type(*drag, types);
  if (matched_type == 0 && !handler->drop_external_poll(*drag)) {
    return false;
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return false;
  }

  /* Show where the data-block is going: aimed at the middle of a row, the row is outlined, for a
   * source that reads that as landing on the row itself rather than beside it; aimed between
   * rows, or past either end, the line says where the new row would go. */
  const bool changed =
      outliner_flag_set(*space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, event->xy, &place);
  if (place == StackMovePlace::Into && !handler->drop_supports_into(matched_type)) {
    place = StackMovePlace::Above;
  }
  stack_drop_aim_set(*space_outliner, target_row, place);
  if (target_row != nullptr) {
    stack_drop_indicator_set(
        *space_outliner,
        target_row,
        (place == StackMovePlace::Into) ? std::nullopt : std::optional(place));
  }
  else {
    target_row = stack_drop_top_row_get(*space_outliner);
    stack_drop_indicator_set(*space_outliner, target_row, StackMovePlace::Above);
  }
  if (changed || target_row != nullptr) {
    ED_region_tag_redraw_no_rebuild(CTX_wm_region(C));
  }
  return true;
}

std::string stack_id_drop_tooltip(bContext *C,
                                         wmDrag *drag,
                                         const int xy[2],
                                         wmDropBox * /*drop*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return std::string();
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return std::string();
  }
  Vector<short> types;
  handler->drop_id_types(types);
  const short matched_type = stack_drag_matched_id_type(*drag, types);

  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, xy, &place);
  if (place == StackMovePlace::Into && !handler->drop_supports_into(matched_type)) {
    place = StackMovePlace::Above;
  }

  /* A plain local data-block can be judged before the drop, and what the source would refuse is
   * worth reading while dragging, not only after -- an asset or an external drag resolves only at
   * drop time, so its refusals come from the handler then. */
  if (drag->type == WM_DRAG_ID && matched_type != 0) {
    ID *local = WM_drag_get_local_ID(drag, matched_type);
    if (local != nullptr) {
      StackDropPayload payload;
      payload.id_uid = local->session_uid;
      payload.id_type = matched_type;
      StackDropTarget target;
      if (target_row != nullptr) {
        target.anchor =
            outliner_stack_identity_of(*space_outliner, int(TREESTORE(target_row)->nr));
        target.place = place;
      }
      const StackReadContext ctx = outliner_stack_read_context(*C);
      ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
      const char *hint = nullptr;
      if (owner != nullptr &&
          !handler->can_accept(ctx, *owner, payload, target, &hint) && hint != nullptr)
      {
        return hint;
      }
    }
  }

  std::string row_name;
  if (target_row != nullptr) {
    const StackRow *row = outliner_stack_row_find(*space_outliner, int(TREESTORE(target_row)->nr));
    row_name = (row != nullptr) ? row->name : std::string("layer");
  }
  StackDropTarget target;
  target.place = place;
  return handler->drop_tooltip(matched_type, WM_drag_get_item_name(drag), row_name, target);
}

static wmOperatorStatus stack_id_drop_invoke(bContext *C,
                                             wmOperator * /*op*/,
                                             const wmEvent *event)
{
  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }
  ListBaseT<wmDrag> *drags = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(drags->first);

  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* Read first, resolve second: resolving an asset imports it, and an import remaps data-blocks,
   * which is exactly the kind of event the editor answers by dropping what it has cached. */
  const SpaceOutliner_Runtime::StackDropAim aim = space_outliner->runtime->stack_drop_aim;

  ID *dropped = handler->drop_resolve(*C, *drag);
  if (dropped == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }

  StackDropPayload payload;
  payload.id_uid = dropped->session_uid;
  payload.id_type = GS(dropped->name);

  /* What the poll aimed at a moment ago on this very event -- see #stack_drop_aim_set. */
  StackDropTarget target;
  target.anchor = aim.anchor;
  target.place = aim.place;

  const char *unused_hint = nullptr;
  if (!handler->can_accept(ctx, *owner, payload, target, &unused_hint)) {
    return OPERATOR_CANCELLED;
  }
  int affected_ordinal = -1;
  if (!handler->execute(*C,
                        space_outliner->runtime->stack_focus,
                        *owner,
                        payload,
                        target,
                        event,
                        &affected_ordinal))
  {
    return OPERATOR_CANCELLED;
  }
  if (affected_ordinal >= 0) {
    outliner_stack_row_activate(C, *space_outliner, affected_ordinal);
    /* The source's own active row moved to the dropped one, but the tree store still carries the
     * previous row's TSE_ACTIVE -- and #outliner_draw lets TSE_ACTIVE win over the source
     * (`row_is_active`). Hand the remembered UI state to the dropped row too, the way the move and
     * paste paths do, or the old row keeps lighting up. Resolved after the activate above, which
     * re-read the rows the drop just renumbered. */
    SpaceOutliner_Runtime &runtime = *space_outliner->runtime;
    const StackItemIdentity affected_identity = outliner_stack_identity_of(*space_outliner,
                                                                          affected_ordinal);
    for (StackRowUiState &state : runtime.stack_row_ui_state.values()) {
      state.selected = false;
      state.active = false;
    }
    if (!BLI_uuid_is_nil(affected_identity.row_id)) {
      StackRowUiState &state = runtime.stack_row_ui_state.lookup_or_add_default(
          affected_identity.row_id);
      state.selected = true;
      state.active = true;
    }
  }
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_id_drop(wmOperatorType *ot)
{
  ot->name = "Drop Data-Block on Stack Layer";
  ot->description =
      "Assign a dropped data-block to a layer, or add it as a new layer or layer group";
  ot->idname = "OUTLINER_OT_stack_layer_id_drop";

  ot->invoke = stack_id_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  /* No #OPTYPE_UNDO: a data-block that only ever hands off to another operator (a popup that
   * picks a channel, say) has nothing of its own to push, and one that mutates directly pushes its
   * own step explicitly instead -- the automatic push would land before that mutation actually
   * happens, for a drop whose real effect is still pending in a popup. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_INTERNAL;
}

/** \} */

}  // namespace ed::outliner
}  // namespace blender
