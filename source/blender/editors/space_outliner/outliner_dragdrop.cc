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

#include "MEM_guardedalloc.h"

#include "AS_asset_representation.hh"

#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"

#include "BLI_fileops.hh"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"

#include "BLT_translation.hh"

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_object.hh"
#include "ED_outliner.hh"
#include "ED_outliner_stack_automation.hh"
#include "ED_screen.hh"
#include "IMB_imbuf_types.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "outliner_intern.hh"
#include "outliner_stack_source.hh"
#include "tree/tree_iterator.hh"

namespace blender::ed::outliner {

static Collection *collection_parent_from_ID(ID *id);

/* -------------------------------------------------------------------- */
/** \name Drop Target Find
 * \{ */

static TreeElement *outliner_dropzone_element(TreeElement *te,
                                              const float fmval[2],
                                              const bool children)
{
  if ((fmval[1] > te->ys) && (fmval[1] < (te->ys + UI_UNIT_Y))) {
    /* name and first icon */
    if ((fmval[0] > te->xs + UI_UNIT_X) && (fmval[0] < te->xend)) {
      return te;
    }
  }
  /* Not it.  Let's look at its children. */
  if (children && (TREESTORE(te)->flag & TSE_CLOSED) == 0 && (te->subtree.first)) {
    for (TreeElement &te_sub : te->subtree) {
      TreeElement *te_valid = outliner_dropzone_element(&te_sub, fmval, children);
      if (te_valid) {
        return te_valid;
      }
    }
  }
  return nullptr;
}

/* Find tree element to drop into. */
static TreeElement *outliner_dropzone_find(const SpaceOutliner *space_outliner,
                                           const float fmval[2],
                                           const bool children)
{
  for (TreeElement &te : space_outliner->runtime->tree) {
    TreeElement *te_valid = outliner_dropzone_element(&te, fmval, children);
    if (te_valid) {
      return te_valid;
    }
  }
  return nullptr;
}

static TreeElement *outliner_drop_find(bContext *C, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  float fmval[2];
  ui::view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &fmval[0], &fmval[1]);

  return outliner_dropzone_find(space_outliner, fmval, true);
}

static ID *outliner_ID_drop_find(bContext *C, const wmEvent *event, short idcode)
{
  TreeElement *te = outliner_drop_find(C, event);
  TreeStoreElem *tselem = (te) ? TREESTORE(te) : nullptr;

  if (te && (te->idcode == idcode) && (tselem->type == TSE_SOME_ID)) {
    return tselem->id;
  }
  return nullptr;
}

/* Find tree element to drop into, with additional before and after reorder support. */
static TreeElement *outliner_drop_insert_find(bContext *C,
                                              const int xy[2],
                                              TreeElementInsertType *r_insert_type)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  TreeElement *te_hovered;
  float view_mval[2];

  /* Empty tree, e.g. while filtered. */
  if (space_outliner->runtime->tree.is_empty()) {
    return nullptr;
  }

  int mval[2];
  mval[0] = xy[0] - region->winrct.xmin;
  mval[1] = xy[1] - region->winrct.ymin;

  ui::view2d_region_to_view(&region->v2d, mval[0], mval[1], &view_mval[0], &view_mval[1]);
  te_hovered = outliner_find_item_at_y(
      space_outliner, &space_outliner->runtime->tree, view_mval[1]);

  if (te_hovered) {
    /* Mouse hovers an element (ignoring x-axis),
     * now find out how to insert the dragged item exactly. */
    /* A quarter of the row this time, not of a fixed unit: the insert margins have to scale with
     * the row or a tall row is nearly all "into". */
    const float margin = outliner_tree_element_height(*space_outliner, *te_hovered) * (1.0f / 4);

    if (view_mval[1] < (te_hovered->ys + margin)) {
      if (TSELEM_OPEN(TREESTORE(te_hovered), space_outliner) && !te_hovered->subtree.is_empty()) {
        /* inserting after a open item means we insert into it, but as first child */
        if (te_hovered->subtree.is_empty()) {
          *r_insert_type = TE_INSERT_INTO;
          return te_hovered;
        }
        *r_insert_type = TE_INSERT_BEFORE;
        return static_cast<TreeElement *>(te_hovered->subtree.first);
      }
      *r_insert_type = TE_INSERT_AFTER;
      return te_hovered;
    }
    if (view_mval[1] > (te_hovered->ys + (3 * margin))) {
      *r_insert_type = TE_INSERT_BEFORE;
      return te_hovered;
    }
    *r_insert_type = TE_INSERT_INTO;
    return te_hovered;
  }

  /* Mouse doesn't hover any item (ignoring x-axis),
   * so it's either above list bounds or below. */
  TreeElement *first = static_cast<TreeElement *>(space_outliner->runtime->tree.first);
  TreeElement *last = static_cast<TreeElement *>(space_outliner->runtime->tree.last);

  if (view_mval[1] < last->ys) {
    *r_insert_type = TE_INSERT_AFTER;
    return last;
  }
  if (view_mval[1] > (first->ys + outliner_tree_element_height(*space_outliner, *first))) {
    *r_insert_type = TE_INSERT_BEFORE;
    return first;
  }

  BLI_assert_unreachable();
  return nullptr;
}

using CheckTypeFn = bool (*)(TreeElement *te);

static TreeElement *outliner_data_from_tree_element_and_parents(CheckTypeFn check_type,
                                                                TreeElement *te)
{
  while (te != nullptr) {
    if (check_type(te)) {
      return te;
    }
    te = te->parent;
  }
  return nullptr;
}

static bool is_collection_element(TreeElement *te)
{
  return outliner_is_collection_tree_element(te);
}

/* Check if a collection is being dragged inside its own hierarchy. */
static bool outliner_is_collection_dragged_into_itself(TreeElement *drop_target_te, ID *dragged_id)
{
  if (!(drop_target_te && dragged_id && GS(dragged_id->name) == ID_GR)) {
    return false;
  }

  /* The drop_target_te could be anything. So, traverse up to get the
   * parent tree_element that represents a collection. */
  TreeElement *coll_te = outliner_data_from_tree_element_and_parents(is_collection_element,
                                                                     drop_target_te);

  while (coll_te && coll_te->parent != nullptr) {
    /* Get the actual collection type. */
    Collection *te_parent_coll = outliner_collection_from_tree_element(coll_te->parent);

    if (&te_parent_coll->id == dragged_id) {
      /* The destination te is inside the dragged collection's hierarchy. */
      return true;
    }

    /* Keep going up the hierarchy */
    coll_te = coll_te->parent;
  }
  return false;
}

static bool is_object_element(TreeElement *te)
{
  TreeStoreElem *tselem = TREESTORE(te);
  return (tselem->type == TSE_SOME_ID) && te->idcode == ID_OB;
}

static bool is_pchan_element(TreeElement *te)
{
  TreeStoreElem *tselem = TREESTORE(te);
  return tselem->type == TSE_POSE_CHANNEL;
}

static TreeElement *outliner_drop_insert_collection_find(bContext *C,
                                                         const int xy[2],
                                                         TreeElementInsertType *r_insert_type)
{
  TreeElement *te = outliner_drop_insert_find(C, xy, r_insert_type);
  if (!te) {
    return nullptr;
  }

  TreeElement *collection_te = outliner_data_from_tree_element_and_parents(is_collection_element,
                                                                           te);
  if (!collection_te) {
    return nullptr;
  }

  /* We can't insert before/after/into a collection that itself is selected/dragged. */
  TreeStoreElem *collection_tselem = TREESTORE(collection_te);
  if ((collection_tselem->flag & TSE_SELECTED) != 0) {
    return nullptr;
  }

  Collection *collection = outliner_collection_from_tree_element(collection_te);

  if (collection_te != te) {
    *r_insert_type = TE_INSERT_INTO;
  }

  /* We can't insert before/after master collection. */
  if (collection->flag & COLLECTION_IS_MASTER) {
    *r_insert_type = TE_INSERT_INTO;
  }

  return collection_te;
}

template<typename T>
static int outliner_get_insert_index(TreeElement *drag_te,
                                     TreeElement *drop_te,
                                     TreeElementInsertType insert_type,
                                     ListBaseT<T> *listbase)
{
  /* Find the element to insert after. Null is the start of the list. */
  if (drag_te->index < drop_te->index) {
    if (insert_type == TE_INSERT_BEFORE) {
      drop_te = drop_te->prev;
    }
  }
  else {
    if (insert_type == TE_INSERT_AFTER) {
      drop_te = drop_te->next;
    }
  }

  if (drop_te == nullptr) {
    return 0;
  }

  return BLI_findindex(listbase, drop_te->directdata);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parent Drop Operator
 * \{ */

static bool parent_drop_allowed(const Main &bmain, TreeElement *te, Object *potential_child)
{
  TreeStoreElem *tselem = TREESTORE(te);
  if ((te->idcode != ID_OB) || (tselem->type != TSE_SOME_ID)) {
    return false;
  }

  Object *potential_parent = id_cast<Object *>(tselem->id);

  if (potential_parent == potential_child) {
    return false;
  }
  if (BKE_object_is_child_recursive(potential_child, potential_parent)) {
    return false;
  }
  if (potential_parent == potential_child->parent) {
    return false;
  }

  /* check that parent/child are both in the same scene */
  Scene *scene = id_cast<Scene *>(outliner_search_back(te, ID_SCE));

  /* currently outliner organized in a way that if there's no parent scene
   * element for object it means that all displayed objects belong to
   * active scene and parenting them is allowed (sergey) */
  if (scene) {
    for (ViewLayer &view_layer : scene->view_layers) {
      BKE_view_layer_synced_ensure(bmain, scene, &view_layer);
      if (BKE_view_layer_base_find(&view_layer, potential_child)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

static bool allow_parenting_without_modifier_key(SpaceOutliner *space_outliner)
{
  switch (space_outliner->outlinevis) {
    case SO_VIEW_LAYER:
      return space_outliner->filter & SO_FILTER_NO_COLLECTION;
    case SO_SCENES:
      return true;
    default:
      return false;
  }
}

static bool parent_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

  bool changed = outliner_flag_set(*space_outliner, TSE_DRAG_ANY, false);
  if (changed) {
    ED_region_tag_redraw_no_rebuild(CTX_wm_region(C));
  }

  Object *potential_child = id_cast<Object *>(WM_drag_get_local_ID(drag, ID_OB));
  if (!potential_child) {
    return false;
  }

  if (!allow_parenting_without_modifier_key(space_outliner)) {
    if ((event->modifier & KM_SHIFT) == 0) {
      return false;
    }
  }

  TreeElement *te = outliner_drop_find(C, event);
  if (!te) {
    return false;
  }

  const Main *bmain = CTX_data_main(C);

  if (parent_drop_allowed(*bmain, te, potential_child)) {
    TREESTORE(te)->flag |= TSE_DRAG_INTO;
    ED_region_tag_redraw_no_rebuild(CTX_wm_region(C));
    return true;
  }

  return false;
}

static void parent_drop_set_parents(bContext *C,
                                    ReportList *reports,
                                    wmDragID *drag,
                                    Object *parent,
                                    short parent_type,
                                    const bool keep_transform)
{
  Main *bmain = CTX_data_main(C);
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

  TreeElement *te = outliner_find_id(
      space_outliner, &space_outliner->runtime->tree, &parent->id, TreeElementFlag(0));
  Scene *scene = id_cast<Scene *>(outliner_search_back(te, ID_SCE));

  if (scene == nullptr) {
    /* currently outliner organized in a way, that if there's no parent scene
     * element for object it means that all displayed objects belong to
     * active scene and parenting them is allowed (sergey)
     */

    scene = CTX_data_scene(C);
  }

  bool parent_set = false;
  bool linked_objects = false;

  for (wmDragID *drag_id = drag; drag_id; drag_id = drag_id->next) {
    if (GS(drag_id->id->name) == ID_OB) {
      Object *object = id_cast<Object *>(drag_id->id);

      /* Do nothing to linked data */
      if (!BKE_id_is_editable(bmain, &object->id)) {
        linked_objects = true;
        continue;
      }

      if (object::parent_set(
              reports, C, scene, object, parent, parent_type, false, keep_transform, nullptr))
      {
        parent_set = true;
      }
    }
  }

  if (linked_objects) {
    BKE_report(reports, RPT_INFO, "Cannot edit library linked or non-editable override object(s)");
  }

  if (parent_set) {
    DEG_relations_tag_update(bmain);
    WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);
    WM_event_add_notifier(C, NC_OBJECT | ND_PARENT, nullptr);
  }
}

static wmOperatorStatus parent_drop_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  TreeElement *te = outliner_drop_find(C, event);
  TreeStoreElem *tselem = te ? TREESTORE(te) : nullptr;

  if (!(te && (te->idcode == ID_OB) && (tselem->type == TSE_SOME_ID))) {
    return OPERATOR_CANCELLED;
  }

  Object *par = id_cast<Object *>(tselem->id);
  Object *ob = id_cast<Object *>(WM_drag_get_local_ID_from_event(event, ID_OB));

  if (ELEM(nullptr, ob, par)) {
    return OPERATOR_CANCELLED;
  }
  if (ob == par) {
    return OPERATOR_CANCELLED;
  }

  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }

  ListBaseT<wmDrag> *lb = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);

  parent_drop_set_parents(C,
                          op->reports,
                          static_cast<wmDragID *>(drag->ids.first),
                          par,
                          object::PAR_OBJECT,
                          !(event->modifier & KM_ALT));

  return OPERATOR_FINISHED;
}

void OUTLINER_OT_parent_drop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop to Set Parent (hold Alt to not keep transforms)";
  ot->description = "Drag to parent in Outliner";
  ot->idname = "OUTLINER_OT_parent_drop";

  /* API callbacks. */
  ot->invoke = parent_drop_invoke;

  ot->poll = ED_operator_region_outliner_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parent Clear Operator
 * \{ */

static bool parent_clear_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

  if (!allow_parenting_without_modifier_key(space_outliner)) {
    if ((event->modifier & KM_SHIFT) == 0) {
      return false;
    }
  }

  Object *ob = id_cast<Object *>(WM_drag_get_local_ID(drag, ID_OB));
  if (!ob) {
    return false;
  }
  if (!ob->parent) {
    return false;
  }

  TreeElement *te = outliner_drop_find(C, event);
  if (te) {
    TreeStoreElem *tselem = TREESTORE(te);
    ID *id = tselem->id;
    if (!id) {
      return true;
    }

    switch (GS(id->name)) {
      case ID_OB:
        return ELEM(tselem->type, TSE_MODIFIER_BASE, TSE_CONSTRAINT_BASE);
      case ID_GR:
        return (event->modifier & KM_SHIFT) || ELEM(tselem->type, TSE_LIBRARY_OVERRIDE_BASE);
      default:
        return true;
    }
  }
  else {
    return true;
  }
}

static wmOperatorStatus parent_clear_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);

  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }

  ListBaseT<wmDrag> *lb = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);

  for (wmDragID &drag_id : drag->ids) {
    if (GS(drag_id.id->name) == ID_OB) {
      Object *object = id_cast<Object *>(drag_id.id);

      object::parent_clear(object,
                           (event->modifier & KM_ALT) ? object::CLEAR_PARENT_ALL :
                                                        object::CLEAR_PARENT_KEEP_TRANSFORM);
    }
  }

  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);
  WM_event_add_notifier(C, NC_OBJECT | ND_PARENT, nullptr);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_parent_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop to Clear Parent (hold Alt to not keep transforms)";
  ot->description = "Drag to clear parent in Outliner";
  ot->idname = "OUTLINER_OT_parent_clear";

  /* API callbacks. */
  ot->invoke = parent_clear_invoke;

  ot->poll = ED_operator_outliner_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scene Drop Operator
 * \{ */

static bool scene_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  /* Ensure item under cursor is valid drop target */
  Object *ob = id_cast<Object *>(WM_drag_get_local_ID(drag, ID_OB));
  return (ob && (outliner_ID_drop_find(C, event, ID_SCE) != nullptr));
}

static wmOperatorStatus scene_drop_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = id_cast<Scene *>(outliner_ID_drop_find(C, event, ID_SCE));
  Object *ob = id_cast<Object *>(WM_drag_get_local_ID_from_event(event, ID_OB));

  if (ELEM(nullptr, ob, scene) || !BKE_id_is_editable(bmain, &scene->id)) {
    return OPERATOR_CANCELLED;
  }

  if (BKE_scene_has_object(*bmain, scene, ob)) {
    return OPERATOR_CANCELLED;
  }

  Collection *collection;
  if (scene != CTX_data_scene(C)) {
    /* when linking to an inactive scene link to the master collection */
    collection = scene->master_collection;
  }
  else {
    collection = CTX_data_collection(C);
  }

  BKE_collection_object_add(bmain, collection, ob);

  for (ViewLayer &view_layer : scene->view_layers) {
    BKE_view_layer_synced_ensure(*bmain, scene, &view_layer);
    Base *base = BKE_view_layer_base_find(&view_layer, ob);
    if (base) {
      object::base_select(base, object::BA_SELECT);
    }
  }

  ED_region_tag_redraw(CTX_wm_region(C));
  DEG_relations_tag_update(bmain);

  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_main_add_notifier(NC_SCENE | ND_OB_SELECT, scene);

  return OPERATOR_FINISHED;
}

void OUTLINER_OT_scene_drop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop Object to Scene";
  ot->description = "Drag object to scene in Outliner";
  ot->idname = "OUTLINER_OT_scene_drop";

  /* API callbacks. */
  ot->invoke = scene_drop_invoke;

  ot->poll = ED_operator_region_outliner_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material Drop Operator
 * \{ */

static bool material_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  /* Ensure item under cursor is valid drop target */
  Material *ma = id_cast<Material *>(WM_drag_get_local_ID(drag, ID_MA));
  Object *ob = reinterpret_cast<Object *>(outliner_ID_drop_find(C, event, ID_OB));

  return (!ELEM(nullptr, ob, ma) && ID_IS_EDITABLE(&ob->id) && !ID_IS_OVERRIDE_LIBRARY(&ob->id));
}

static wmOperatorStatus material_drop_invoke(bContext *C,
                                             wmOperator * /*op*/,
                                             const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = id_cast<Object *>(outliner_ID_drop_find(C, event, ID_OB));
  Material *ma = id_cast<Material *>(WM_drag_get_local_ID_from_event(event, ID_MA));

  if (ELEM(nullptr, ob, ma) || !BKE_id_is_editable(bmain, &ob->id)) {
    return OPERATOR_CANCELLED;
  }

  /* only drop grease pencil material on grease pencil objects */
  if ((ma->gp_style != nullptr) && (ob->type != OB_GREASE_PENCIL)) {
    return OPERATOR_CANCELLED;
  }

  BKE_object_material_assign(bmain, ob, ma, ob->totcol + 1, BKE_MAT_ASSIGN_USERPREF);

  WM_event_add_notifier(C, NC_OBJECT | ND_OB_SHADING, ob);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING_LINKS, ma);

  return OPERATOR_FINISHED;
}

void OUTLINER_OT_material_drop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop Material on Object";
  ot->description = "Drag material to object in Outliner";
  ot->idname = "OUTLINER_OT_material_drop";

  /* API callbacks. */
  ot->invoke = material_drop_invoke;

  ot->poll = ED_operator_region_outliner_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
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

static void datastack_drop_data_init(wmDrag *drag,
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

static bool datastack_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
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

static std::string datastack_drop_tooltip(bContext * /*C*/,
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

/* -------------------------------------------------------------------- */
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

static void stack_layer_drag_target_set(wmDragStackLayer &drag_data, const StackItemIdentity &identity)
{
  drag_data.target_owner_uid = identity.owner_uid;
  drag_data.target_source_type = short(identity.source_type);
  drag_data.target_row_id = identity.row_id;
  drag_data.target_ordinal_hint = identity.ordinal_hint;
}

static void stack_layer_drop_data_init(SpaceOutliner &space_outliner,
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
  drag->flags |= WM_DRAG_FREE_DATA;
}

/**
 * The stack row under the cursor and which of its three zones the cursor is in.
 *
 * A path of its own rather than #outliner_drop_insert_find, which answers a different question: it
 * redirects a drop near the bottom edge of an open element to that element's first child, so the
 * area under an expanded group would mean "into the group" when the user is aiming below it, and it
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
    block_low = std::min(block_low, drag_ordinal);
    block_high = std::max(block_high, drag_ordinal);
  }
  if (target_ordinal > block_low && target_ordinal < block_high) {
    return false;
  }

  const StackRow *target_row = outliner_stack_row_find(*space_outliner, target_ordinal);
  if (target_row == nullptr) {
    return false;
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
     * a mistake worth refusing, though -- it reads as "put it here", above the row it points at. */
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

static bool stack_layer_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
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

static std::string stack_layer_drop_tooltip(bContext *C,
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
  if (drop_data == nullptr || drop_data->target_owner_uid == 0 || drop_data->drag_rows.is_empty()) {
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
  const int active_ordinal_before = outliner_stack_active_ordinal_get(ctx, *space_outliner);
  const StackItemIdentity active_identity_before = (active_ordinal_before >= 0) ?
                                                       outliner_stack_identity_of(
                                                           *space_outliner, active_ordinal_before) :
                                                       StackItemIdentity();

  bool any_moved = false;
  StackItemIdentity last_moved = anchor_identity;
  StackMovePlace place_now = place;
  for (const StackItemIdentity &drag_row : drop_data->drag_rows) {
    const int drag_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, drag_row);
    const int anchor_ordinal = outliner_stack_identity_resolve(ctx, *space_outliner, last_moved);
    if (drag_ordinal < 0 || anchor_ordinal < 0) {
      /* This row no longer resolves (stack changed mid-drag or row was deleted). Skip it rather
       * than canceling the whole operation: partial success is better than abandoning the rows that
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

/* -------------------------------------------------------------------- */
/** \name Stack Layer Image Drop
 *
 * An image dropped on a stack row becomes that row's map for a channel the user picks in the
 * popup the source's drop handler opens; dropped in empty space it becomes a new layer. What the
 * drag carries is resolved into a local image here and handed over as a #StackDropPayload, so
 * everything image-specific below this point is the source's own business.
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
 * The image a drag carries, resolved into a local data-block. A local image or an asset comes
 * through the window manager's own resolver, which imports an asset on demand; a file comes
 * through #BKE_image_load_exists, whose extra user is handed back at once -- from here on the
 * layer's map node owns the image, exactly as a brush-slot drop does.
 */
static Image *stack_image_drop_image_resolve(bContext *C, wmDrag *drag)
{
  Main *bmain = CTX_data_main(C);
  if (drag->type == WM_DRAG_PATH) {
    const char *path = WM_drag_get_single_path(drag);
    if (path == nullptr) {
      return nullptr;
    }
    Image *image = BKE_image_load_exists(bmain, path, nullptr);
    if (image != nullptr) {
      id_us_min(&image->id);
    }
    return image;
  }

  ID *imported = WM_drag_get_local_ID_or_import_from_asset(C, drag, ID_IM);
  if (imported != nullptr) {
    return id_cast<Image *>(imported);
  }
  if (drag->type == WM_DRAG_ASSET) {
    wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, ID_IM);
    if (asset_drag == nullptr) {
      return nullptr;
    }
    Image *image = ed::asset::resolve_image_from_asset(*bmain, *asset_drag->asset);
    if (image != nullptr && image->id.asset_data == nullptr) {
      ed::asset::image_mark_as_asset(image);
    }
    return image;
  }
  if (drag->type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(drag);
    if (asset_drags == nullptr) {
      return nullptr;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ?
                                           GS(item.asset_data.local_id->name) :
                                           ID_Type(0));
      if (item_idtype != ID_IM) {
        continue;
      }
      if (item.is_external) {
        Image *image = ed::asset::resolve_image_from_asset(
            *bmain, *item.asset_data.external_info->asset);
        if (image != nullptr && image->id.asset_data == nullptr) {
          ed::asset::image_mark_as_asset(image);
        }
        return image;
      }
      return id_cast<Image *>(item.asset_data.local_id);
    }
  }
  return nullptr;
}

static bool stack_image_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  /* What the drag may carry: a local image data-block, an image asset, or an image file. A file
   * that is not there is refused before a drop zone exists -- a layer with a missing map is
   * worse than a refusal the user can see coming. */
  if (drag->type == WM_DRAG_PATH) {
    const char *path = WM_drag_get_single_path(drag);
    if (path == nullptr || !BLI_path_extension_check_array(path, imb_ext_image) ||
        !BLI_exists(path))
    {
      return false;
    }
  }
  else if (!WM_drag_is_ID_type(drag, ID_IM)) {
    return false;
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(*space_outliner);
  if (source.drop_handler() == nullptr || !source.is_editable(*owner)) {
    return false;
  }

  /* Show where the image is going. Aimed at the middle of a row, it becomes that layer's map for
   * a channel -- the row is outlined, since nothing is inserted. Aimed between rows, it becomes a
   * new layer there, and the line says where; past the ends of the list, that is the top. */
  const bool changed = outliner_flag_set(*space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, event->xy, &place);
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

static std::string stack_image_drop_tooltip(bContext *C,
                                            wmDrag *drag,
                                            const int xy[2],
                                            wmDropBox * /*drop*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr) {
    return std::string();
  }
  const std::string image_name = WM_drag_get_item_name(drag);
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, xy, &place);

  /* A local image can be judged before the drop, and what the source would refuse is worth
   * reading while dragging, not only after -- the same deal the material drop makes. An external
   * one imports at drop time, so its refusals come from the handler then. */
  Image *image = (drag->type == WM_DRAG_ID) ?
                     id_cast<Image *>(WM_drag_get_local_ID(drag, ID_IM)) :
                     nullptr;
  if (image != nullptr) {
    StackDropPayload payload;
    payload.id_uid = image->id.session_uid;
    payload.id_type = ID_IM;
    StackDropTarget target;
    if (target_row != nullptr) {
      target.anchor = outliner_stack_identity_of(*space_outliner, int(TREESTORE(target_row)->nr));
      target.place = place;
    }
    const StackReadContext ctx = outliner_stack_read_context(*C);
    ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
    const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
    const char *hint = nullptr;
    if (owner != nullptr && handler != nullptr &&
        !handler->can_accept(ctx, *owner, payload, target, &hint) && hint != nullptr)
    {
      return hint;
    }
  }

  if (target_row == nullptr) {
    return fmt::format(fmt::runtime(TIP_("Add {} as a new layer on top")), image_name);
  }
  const StackRow *row = outliner_stack_row_find(*space_outliner, int(TREESTORE(target_row)->nr));
  const std::string row_name = (row != nullptr) ? row->name : std::string("layer");
  switch (place) {
    case StackMovePlace::Into:
      return fmt::format(fmt::runtime(TIP_("Assign {} to {}")), image_name, row_name);
    case StackMovePlace::Below:
      return fmt::format(
          fmt::runtime(TIP_("Add {} as a new layer below {}")), image_name, row_name);
    case StackMovePlace::Above:
      break;
  }
  return fmt::format(fmt::runtime(TIP_("Add {} as a new layer above {}")), image_name, row_name);
}

static wmOperatorStatus stack_image_drop_invoke(bContext *C,
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
  /* Read first, resolve second: resolving an asset imports it, and an import remaps data-blocks,
   * which is exactly the kind of event the editor answers by dropping what it has cached. */
  const SpaceOutliner_Runtime::StackDropAim aim = space_outliner->runtime->stack_drop_aim;

  Image *image = stack_image_drop_image_resolve(C, drag);
  if (image == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return OPERATOR_CANCELLED;
  }

  StackDropPayload payload;
  payload.id_uid = image->id.session_uid;
  payload.id_type = ID_IM;

  /* What the poll aimed at a moment ago on this very event -- see #stack_drop_aim_set. */
  StackDropTarget target;
  target.anchor = aim.anchor;
  target.place = aim.place;

  const char *unused_hint = nullptr;
  if (!handler->can_accept(ctx, *owner, payload, target, &unused_hint)) {
    return OPERATOR_CANCELLED;
  }
  if (!handler->execute(
          *C, space_outliner->runtime->stack_focus, *owner, payload, target, event, nullptr))
  {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_image_drop(wmOperatorType *ot)
{
  ot->name = "Drop Image on Stack Layer";
  ot->description = "Assign a dropped image to a layer's channel, or add it as a new layer";
  ot->idname = "OUTLINER_OT_stack_layer_image_drop";

  ot->invoke = stack_image_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stack Layer Material Drop
 *
 * A material dropped anywhere on the stack -- a row, the owner breadcrumb, empty space; the
 * landing spot is not the point -- becomes a new group on top of it that stands for the
 * material. What the drag carries is resolved into a local material here and handed over as a
 * #StackDropPayload, so everything material-specific below this point is the source's own
 * business.
 * \{ */

/**
 * Whether the drag carries a material at all: a local material, a material asset, or a drag of
 * several assets holding one. Cheap type checks only -- importing an asset is a drop-time act,
 * never a poll-time one.
 */
static bool stack_material_drop_carries_material(const wmDrag &drag)
{
  if (drag.type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (asset_drags == nullptr) {
      return false;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ?
                                           GS(item.asset_data.local_id->name) :
                                           ID_Type(0));
      if (item_idtype == ID_MA) {
        return true;
      }
    }
    return false;
  }
  if (drag.type == WM_DRAG_ASSET) {
    return WM_drag_get_asset_data(&drag, ID_MA) != nullptr;
  }
  return WM_drag_is_ID_type(&drag, ID_MA);
}

/**
 * The material a drag carries, resolved into a local data-block. A local material comes as it
 * is; an external asset is appended with the reuse policy, the way a brush-slot drop brings its
 * material in. Of several assets only the first material is taken.
 */
static Material *stack_material_drop_material_resolve(bContext *C, const wmDrag &drag)
{
  Main &bmain = *CTX_data_main(C);

  if (drag.type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (asset_drags == nullptr) {
      return nullptr;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ?
                                           GS(item.asset_data.local_id->name) :
                                           ID_Type(0));
      if (item_idtype != ID_MA) {
        continue;
      }
      if (!item.is_external) {
        return id_cast<Material *>(item.asset_data.local_id);
      }
      ID *imported = ed::asset::asset_local_id_ensure_imported(
          bmain,
          *item.asset_data.external_info->asset,
          0,
          ASSET_IMPORT_APPEND_REUSE,
          std::nullopt,
          CTX_wm_reports(C));
      return (imported != nullptr && GS(imported->name) == ID_MA) ? id_cast<Material *>(imported) :
                                                                    nullptr;
    }
    return nullptr;
  }

  ID *local = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_MA);
  if (local != nullptr) {
    return id_cast<Material *>(local);
  }
  if (drag.type == WM_DRAG_ASSET) {
    wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_MA);
    if (asset_drag == nullptr) {
      return nullptr;
    }
    ID *imported = ed::asset::asset_local_id_ensure_imported(
        bmain, *asset_drag->asset, 0, ASSET_IMPORT_APPEND_REUSE, std::nullopt, CTX_wm_reports(C));
    return (imported != nullptr && GS(imported->name) == ID_MA) ? id_cast<Material *>(imported) :
                                                                  nullptr;
  }
  return nullptr;
}

/**
 * The row a dropped material would be inserted next to, and on which side, or null when the drop
 * names no row at all -- empty space, or the stack's own breadcrumb -- which reads as "on top of
 * the stack", where a group with nothing in it belongs by default.
 *
 * A material is always a row of its own, so the middle of a row means nothing here: it reads as
 * the place above, which is what the line then promises.
 */
static TreeElement *stack_material_drop_place_find(bContext *C,
                                                   const int xy[2],
                                                   StackMovePlace *r_place)
{
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_drop_aim_find(C, xy, &place);
  if (r_place != nullptr) {
    *r_place = (place == StackMovePlace::Into) ? StackMovePlace::Above : place;
  }
  return target_row;
}

static bool stack_material_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (space_outliner == nullptr || space_outliner->outlinevis != SO_STACK_LAYERS ||
      space_outliner->stack_layers_view != SO_SL_VIEW_STACK)
  {
    return false;
  }
  if (!stack_material_drop_carries_material(*drag)) {
    return false;
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return false;
  }
  const StackSource &source = *stack_source_for_space(*space_outliner);
  if (source.drop_handler() == nullptr) {
    return false;
  }

  /* A material becomes a row of its own, so the drag shows the line it would be inserted at,
   * the way reordering a layer does -- not the outline of a row it would land in. */
  const bool changed = outliner_flag_set(*space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = stack_material_drop_place_find(C, event->xy, &place);
  /* Where the group will be inserted, for the drop to read back: exactly the row and side the
   * line below is about to promise. */
  stack_drop_aim_set(*space_outliner, target_row, place);
  if (target_row == nullptr) {
    /* Nothing named: the group goes on top, and the line says so. */
    target_row = stack_drop_top_row_get(*space_outliner);
  }
  stack_drop_indicator_set(*space_outliner, target_row, place);
  if (changed || target_row != nullptr) {
    ED_region_tag_redraw_no_rebuild(CTX_wm_region(C));
  }
  return true;
}

static std::string stack_material_drop_tooltip(bContext *C,
                                               wmDrag *drag,
                                               const int xy[2],
                                               wmDropBox * /*drop*/)
{
  /* A local material can be judged before the drop, and what the source would refuse is worth
   * reading while dragging, not only after. An asset cannot -- it imports on drop -- so its
   * refusals come from the handler at drop time. */
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  Material *material = (drag->type == WM_DRAG_ID && space_outliner != nullptr) ?
                           id_cast<Material *>(WM_drag_get_local_ID(drag, ID_MA)) :
                           nullptr;
  if (material != nullptr) {
    StackDropPayload payload;
    payload.id_uid = material->id.session_uid;
    payload.id_type = ID_MA;
    const StackReadContext ctx = outliner_stack_read_context(*C);
    ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
    const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
    const char *hint = nullptr;
    if (owner != nullptr && handler != nullptr &&
        !handler->can_accept(ctx, *owner, payload, StackDropTarget{}, &hint) && hint != nullptr)
    {
      return hint;
    }
  }

  /* Say where it lands, since that is now what the line under the cursor is promising. */
  const std::string material_name = WM_drag_get_item_name(drag);
  StackMovePlace place = StackMovePlace::Above;
  TreeElement *target_row = (space_outliner != nullptr) ?
                                stack_material_drop_place_find(C, xy, &place) :
                                nullptr;
  if (target_row == nullptr) {
    return fmt::format(fmt::runtime(TIP_("Add {} as a layer group on top")), material_name);
  }
  const StackRow *row = outliner_stack_row_find(*space_outliner, int(TREESTORE(target_row)->nr));
  const std::string row_name = (row != nullptr) ? row->name : std::string("layer");
  return (place == StackMovePlace::Below) ?
             fmt::format(fmt::runtime(TIP_("Add {} as a layer group below {}")),
                         material_name,
                         row_name) :
             fmt::format(fmt::runtime(TIP_("Add {} as a layer group above {}")),
                         material_name,
                         row_name);
}

static wmOperatorStatus stack_material_drop_invoke(bContext *C,
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
  /* Read first, resolve second: importing a dragged asset remaps data-blocks, and the editor
   * answers a remap by dropping its cached rows -- which is where this used to lose its aim and
   * fall back to the top of the stack. */
  const SpaceOutliner_Runtime::StackDropAim aim = space_outliner->runtime->stack_drop_aim;

  Material *material = stack_material_drop_material_resolve(C, *drag);
  if (material == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const StackDropHandler *handler = stack_source_for_space(*space_outliner)->drop_handler();
  if (handler == nullptr) {
    return OPERATOR_CANCELLED;
  }

  StackDropPayload payload;
  payload.id_uid = material->id.session_uid;
  payload.id_type = ID_MA;

  /* The same row and side the drag drew its line at -- read back from where the poll left it, not
   * resolved again here (see #stack_drop_aim_set). A drop that named no row leaves the anchor
   * invalid, which is the source's cue to put the group on top. */
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
  }
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_material_drop(wmOperatorType *ot)
{
  ot->name = "Drop Material on Stack";
  ot->description = "Add a dropped material to the stack as a layer group that stands for it";
  ot->idname = "OUTLINER_OT_stack_layer_material_drop";

  ot->invoke = stack_material_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Collection Drop Operator
 * \{ */

struct CollectionDrop {
  Collection *from;
  Collection *to;

  TreeElement *te;
  TreeElementInsertType insert_type;
};

static Collection *collection_parent_from_ID(ID *id)
{
  /* Can't change linked or override parent collections. */
  if (!id || !ID_IS_EDITABLE(id) || ID_IS_OVERRIDE_LIBRARY(id)) {
    return nullptr;
  }

  /* Also support dropping into/from scene collection. */
  if (GS(id->name) == ID_SCE) {
    return (id_cast<Scene *>(id))->master_collection;
  }
  if (GS(id->name) == ID_GR) {
    return id_cast<Collection *>(id);
  }

  return nullptr;
}

static bool collection_drop_init(bContext *C, wmDrag *drag, const int xy[2], CollectionDrop *data)
{
  /* Get collection to drop into. */
  TreeElementInsertType insert_type;
  TreeElement *te = outliner_drop_insert_collection_find(C, xy, &insert_type);
  if (!te) {
    return false;
  }

  Collection *to_collection = outliner_collection_from_tree_element(te);
  if (!ID_IS_EDITABLE(to_collection) || ID_IS_OVERRIDE_LIBRARY(to_collection)) {
    if (insert_type == TE_INSERT_INTO) {
      return false;
    }
  }

  /* Get drag datablocks. */
  if (drag->type != WM_DRAG_ID) {
    return false;
  }

  wmDragID *drag_id = static_cast<wmDragID *>(drag->ids.first);
  if (drag_id == nullptr) {
    return false;
  }

  ID *id = drag_id->id;
  if (!(id && ELEM(GS(id->name), ID_GR, ID_OB))) {
    return false;
  }

  if (outliner_is_collection_dragged_into_itself(te, id)) {
    return false;
  }

  /* Get collection to drag out of. */
  ID *parent = drag_id->from_parent;
  Collection *from_collection = collection_parent_from_ID(parent);

  /* Currently this should not be allowed, cannot edit items in an override of a Collection. */
  if (from_collection != nullptr && ID_IS_OVERRIDE_LIBRARY(from_collection)) {
    return false;
  }

  /* Get collections. */
  if (GS(id->name) == ID_GR) {
    if (id == &to_collection->id) {
      return false;
    }
  }
  else {
    insert_type = TE_INSERT_INTO;
  }

  /* Currently this should not be allowed, cannot edit items in an override of a Collection. */
  if (ID_IS_OVERRIDE_LIBRARY(to_collection) &&
      !ELEM(insert_type, TE_INSERT_AFTER, TE_INSERT_BEFORE))
  {
    return false;
  }

  data->from = from_collection;
  data->to = to_collection;
  data->te = te;
  data->insert_type = insert_type;

  return true;
}

static bool collection_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  ARegion *region = CTX_wm_region(C);
  bool changed = outliner_flag_set(*space_outliner, TSE_HIGHLIGHTED_ANY | TSE_DRAG_ANY, false);

  CollectionDrop data;
  if (((event->modifier & KM_SHIFT) == 0) && collection_drop_init(C, drag, event->xy, &data)) {
    TreeElement *te = data.te;
    TreeStoreElem *tselem = TREESTORE(te);
    switch (data.insert_type) {
      case TE_INSERT_BEFORE:
        tselem->flag |= TSE_DRAG_BEFORE;
        changed = true;
        break;
      case TE_INSERT_AFTER:
        tselem->flag |= TSE_DRAG_AFTER;
        changed = true;
        break;
      case TE_INSERT_INTO: {
        tselem->flag |= TSE_DRAG_INTO;
        changed = true;
        break;
      }
    }
    if (changed) {
      ED_region_tag_redraw_no_rebuild(region);
    }
    return true;
  }
  if (changed) {
    ED_region_tag_redraw_no_rebuild(region);
  }
  return false;
}

static std::string collection_drop_tooltip(bContext *C,
                                           wmDrag *drag,
                                           const int xy[2],
                                           wmDropBox * /*drop*/)
{
  wmWindow *win = CTX_wm_window(C);
  const wmEvent *event = win ? win->runtime->eventstate : nullptr;

  CollectionDrop data;
  if (event && ((event->modifier & KM_SHIFT) == 0) && collection_drop_init(C, drag, xy, &data)) {
    const bool is_link = !data.from || (event->modifier & KM_CTRL);

    /* Test if we are moving within same parent collection. */
    bool same_level = false;
    for (CollectionParent &parent : data.to->runtime->parents) {
      if (data.from == parent.collection) {
        same_level = true;
      }
    }

    /* Tooltips when not moving directly into another collection i.e. mouse on border of
     * collections. Later we will decide which tooltip to return. */
    const bool tooltip_link = (is_link && !same_level);
    const char *tooltip_before = tooltip_link ? TIP_("Link before collection") :
                                                TIP_("Move before collection");
    const char *tooltip_between = tooltip_link ? TIP_("Link between collections") :
                                                 TIP_("Move between collections");
    const char *tooltip_after = tooltip_link ? TIP_("Link after collection") :
                                               TIP_("Move after collection");

    TreeElement *te = data.te;
    switch (data.insert_type) {
      case TE_INSERT_BEFORE:
        if (te->prev && outliner_is_collection_tree_element(te->prev)) {
          return tooltip_between;
        }
        return tooltip_before;
      case TE_INSERT_AFTER:
        if (te->next && outliner_is_collection_tree_element(te->next)) {
          return tooltip_between;
        }
        return tooltip_after;
      case TE_INSERT_INTO: {
        if (is_link) {
          return TIP_("Link inside collection");
        }

        /* Check the type of the drag IDs to avoid the incorrect "Shift to parent"
         * for collections. Checking the type of the first ID works fine here since
         * all drag IDs are the same type. */
        wmDragID *drag_id = static_cast<wmDragID *>(drag->ids.first);
        const bool is_object = (GS(drag_id->id->name) == ID_OB);
        if (is_object) {
          return TIP_("Move inside collection (Ctrl to link, Shift to parent)");
        }
        return TIP_("Move inside collection (Ctrl to link)");
      }
    }
  }
  return {};
}

static wmOperatorStatus collection_drop_invoke(bContext *C,
                                               wmOperator * /*op*/,
                                               const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  if (event->custom != EVT_DATA_DRAGDROP) {
    return OPERATOR_CANCELLED;
  }

  ListBaseT<wmDrag> *lb = static_cast<ListBaseT<wmDrag> *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);

  CollectionDrop data;
  if (!collection_drop_init(C, drag, event->xy, &data)) {
    return OPERATOR_CANCELLED;
  }

  /* Before/after insert handling. */
  Collection *relative = nullptr;
  bool relative_after = false;

  if (ELEM(data.insert_type, TE_INSERT_BEFORE, TE_INSERT_AFTER)) {
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);

    relative = data.to;
    relative_after = (data.insert_type == TE_INSERT_AFTER);

    TreeElement *parent_te = outliner_find_parent_element(
        &space_outliner->runtime->tree, nullptr, data.te);
    data.to = (parent_te) ? outliner_collection_from_tree_element(parent_te) : nullptr;
  }

  if (!data.to) {
    return OPERATOR_CANCELLED;
  }

  if (BKE_collection_is_empty(data.to)) {
    TREESTORE(data.te)->flag &= ~TSE_CLOSED;
  }

  if (relative_after) {
    BLI_listbase_reverse(&drag->ids);
  }

  for (wmDragID &drag_id : drag->ids) {
    /* Ctrl enables linking, so we don't need a from collection then. */
    Collection *from = (event->modifier & KM_CTRL) ?
                           nullptr :
                           collection_parent_from_ID(drag_id.from_parent);

    if (GS(drag_id.id->name) == ID_OB) {
      /* Move/link object into collection. */
      Object *object = id_cast<Object *>(drag_id.id);

      if (from) {
        BKE_collection_object_move(bmain, scene, data.to, from, object);
      }
      else {
        BKE_collection_object_add(bmain, data.to, object);
      }
    }
    else if (GS(drag_id.id->name) == ID_GR) {
      /* Move/link collection into collection. */
      Collection *collection = id_cast<Collection *>(drag_id.id);

      if (collection != from) {
        BKE_collection_move(bmain, data.to, from, relative, relative_after, collection);
      }
    }

    if (from) {
      DEG_id_tag_update(&from->id,
                        ID_RECALC_SYNC_TO_EVAL | ID_RECALC_GEOMETRY | ID_RECALC_HIERARCHY);
    }
  }

  /* Update dependency graph. */
  DEG_id_tag_update(&data.to->id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_HIERARCHY);
  DEG_relations_tag_update(bmain);
  /* NOTE: It is possible to drag-and-drop between different windows, which means that the source
   * window/Outliner may also need to be updated. So do not pass the current window in this
   * notifier (unless there is a way to get the drag source window as well?). */
  WM_event_add_notifier_ex(CTX_wm_manager(C), nullptr, NC_SCENE | ND_LAYER, nullptr);

  return OPERATOR_FINISHED;
}

void OUTLINER_OT_collection_drop(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Move to Collection";
  ot->description = "Drag to move to collection in Outliner";
  ot->idname = "OUTLINER_OT_collection_drop";

  /* API callbacks. */
  ot->invoke = collection_drop_invoke;
  ot->poll = ED_operator_outliner_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Outliner Drag Operator
 * \{ */

#define OUTLINER_DRAG_SCOLL_OUTSIDE_PAD 7 /* In UI units */

static TreeElement *outliner_item_drag_element_find(SpaceOutliner *space_outliner,
                                                    ARegion *region,
                                                    const wmEvent *event)
{
  /* NOTE: using click-drag events to trigger dragging is fine,
   * it sends coordinates from where dragging was started */
  int mval[2];
  WM_event_drag_start_mval(event, region, mval);

  const float my = ui::view2d_region_to_view_y(&region->v2d, mval[1]);
  return outliner_find_item_at_y(space_outliner, &space_outliner->runtime->tree, my);
}

/**
 * The data-block a #TSE_STACK_ITEM row's drag would carry, or null when it has none.
 *
 * #TreeElementStackItem puts the sub-row's own data-block in #TreeElement.directdata -- the map's
 * image for the paint source, not the #StackSubRow and not the stack's owner the treestore points
 * at. The drops that accept this drag are image drops, so anything but an image drags nothing.
 */
static ID *outliner_stack_item_drag_id_get(const TreeElement &te)
{
  const TreeStoreElem *tselem = TREESTORE(&te);
  if (tselem->type != TSE_STACK_ITEM) {
    return nullptr;
  }
  ID *stack_item_id = static_cast<ID *>(te.directdata);
  if (stack_item_id == nullptr || GS(stack_item_id->name) != ID_IM) {
    return nullptr;
  }
  return stack_item_id;
}

uint32_t outliner_stack_item_debug_drag_id(SpaceOutliner &space_outliner,
                                           const int ordinal,
                                           const int role)
{
  const int nr = ordinal * STACK_ROW_SUB_ROW_STRIDE + role;
  TreeElement *item_te = nullptr;
  tree_iterator::all(space_outliner, [&](TreeElement *te) {
    const TreeStoreElem *tselem = TREESTORE(te);
    if (tselem->type == TSE_STACK_ITEM && int(tselem->nr) == nr && item_te == nullptr) {
      item_te = te;
    }
  });
  const ID *stack_item_id = (item_te != nullptr) ? outliner_stack_item_drag_id_get(*item_te) :
                                                   nullptr;
  if (stack_item_id == nullptr) {
    return 0;
  }
  /* Build the drag the invoke itself would build, and answer with what it ended up carrying.
   * The null context is safe for #WM_DRAG_ID: #WM_drag_data_create only reads the context for
   * an asset-list drag. */
  TreeElementIcon data = tree_element_get_icon(TREESTORE(item_te), item_te);
  wmDrag *drag = WM_drag_data_create(nullptr, data.icon, WM_DRAG_ID, nullptr, WM_DRAG_NOP);
  WM_drag_add_local_ID(drag, const_cast<ID *>(stack_item_id), nullptr);
  const wmDragID *drag_id = static_cast<const wmDragID *>(drag->ids.first);
  const uint32_t dragged_uid = (drag_id != nullptr) ? drag_id->id->session_uid : 0;
  WM_drag_free(drag);
  return dragged_uid;
}

static wmOperatorStatus outliner_item_drag_drop_invoke(bContext *C,
                                                       wmOperator * /*op*/,
                                                       const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  TreeElement *te = outliner_item_drag_element_find(space_outliner, region, event);

  int mval[2];
  WM_event_drag_start_mval(event, region, mval);

  if (!te) {
    return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
  }

  TreeStoreElem *tselem = TREESTORE(te);
  /* A stack row has no draggable ID of its own -- see #tree_element_get_icon -- so it is the one
   * kind of row that may start a drag without one. */
  const bool use_stack_layer_drag = tselem->type == TSE_STACK_LAYER;
  /* A channel sub-row names the map the layer uses; what the drag carries is that image, not the
   * stack's own data-block the treestore happens to point at. */
  const bool use_stack_item_drag = tselem->type == TSE_STACK_ITEM;
  ID *stack_item_id = nullptr;
  if (use_stack_item_drag) {
    stack_item_id = outliner_stack_item_drag_id_get(*te);
    if (stack_item_id == nullptr) {
      return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
    }
  }
  TreeElementIcon data = tree_element_get_icon(tselem, te);
  if (!use_stack_layer_drag && !use_stack_item_drag && !data.drag_id) {
    return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
  }

  float view_mval[2];
  ui::view2d_region_to_view(&region->v2d, mval[0], mval[1], &view_mval[0], &view_mval[1]);
  if (outliner_item_is_co_within_close_toggle(te, view_mval[0])) {
    return (OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH);
  }
  if (outliner_is_co_within_mode_column(space_outliner, view_mval)) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  if (use_stack_layer_drag && (tselem->flag & TSE_SELECTED) == 0) {
    /* Match the regular Outliner's drag behavior: a row that was not part of the current block
     * becomes the only selected and active row before its drag payload is collected. */
    outliner_item_select(C, space_outliner, te, OL_ITEM_SELECT | OL_ITEM_ACTIVATE);
  }

  /* Scroll the view when dragging near edges, but not
   * when the drag goes too far outside the region. */
  {
    wmOperatorType *ot = WM_operatortype_find("VIEW2D_OT_edge_pan", true);
    PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);
    RNA_float_set(&op_ptr, "outside_padding", OUTLINER_DRAG_SCOLL_OUTSIDE_PAD);
    WM_operator_name_call_ptr(C, ot, wm::OpCallContext::InvokeDefault, &op_ptr, event);
    WM_operator_properties_free(&op_ptr);
  }

  const bool use_datastack_drag = ELEM(tselem->type,
                                       TSE_MODIFIER,
                                       TSE_MODIFIER_BASE,
                                       TSE_CONSTRAINT,
                                       TSE_CONSTRAINT_BASE,
                                       TSE_GPENCIL_EFFECT,
                                       TSE_GPENCIL_EFFECT_BASE);

  const eWM_DragDataType wm_drag_type = use_stack_layer_drag ? WM_DRAG_STACK_LAYER :
                                        use_datastack_drag  ? WM_DRAG_DATASTACK :
                                                              WM_DRAG_ID;
  wmDrag *drag = WM_drag_data_create(C, data.icon, wm_drag_type, nullptr, WM_DRAG_NOP);

  if (use_stack_layer_drag) {
    stack_layer_drop_data_init(*space_outliner, drag, *tselem);
  }
  else if (use_datastack_drag) {
    TreeElement *te_bone = nullptr;
    bPoseChannel *pchan = outliner_find_parent_bone(te, &te_bone);
    datastack_drop_data_init(
        drag, id_cast<Object *>(tselem->id), pchan, te, tselem, te->directdata);
  }
  else if (use_stack_item_drag) {
    /* Dropping the image on a layer assigns it there through the channel popup, and assignment
     * is a copy: the layer the map was dragged from keeps its own. */
    WM_drag_add_local_ID(drag, stack_item_id, nullptr);
  }
  else if (ELEM(GS(data.drag_id->name), ID_OB, ID_GR)) {
    /* For collections and objects we cheat and drag all selected. */

    /* Only drag element under mouse if it was not selected before. */
    if ((tselem->flag & TSE_SELECTED) == 0) {
      outliner_flag_set(*space_outliner, TSE_SELECTED, 0);
      tselem->flag |= TSE_SELECTED;
    }

    /* Gather all selected elements. */
    IDsSelectedData selected{};

    if (GS(data.drag_id->name) == ID_OB) {
      outliner_tree_traverse(space_outliner,
                             &space_outliner->runtime->tree,
                             0,
                             TSE_SELECTED,
                             outliner_collect_selected_objects,
                             &selected);
    }
    else {
      outliner_tree_traverse(space_outliner,
                             &space_outliner->runtime->tree,
                             0,
                             TSE_SELECTED,
                             outliner_collect_selected_collections,
                             &selected);
    }

    for (LinkData &link : selected.selected_array) {
      TreeElement *te_selected = static_cast<TreeElement *>(link.data);
      ID *id;

      if (GS(data.drag_id->name) == ID_OB) {
        id = TREESTORE(te_selected)->id;
      }
      else {
        /* Keep collection hierarchies intact when dragging. */
        bool parent_selected = false;
        for (TreeElement *te_parent = te_selected->parent; te_parent;
             te_parent = te_parent->parent)
        {
          if (outliner_is_collection_tree_element(te_parent)) {
            if (TREESTORE(te_parent)->flag & TSE_SELECTED) {
              parent_selected = true;
              break;
            }
          }
        }

        if (parent_selected) {
          continue;
        }

        id = &outliner_collection_from_tree_element(te_selected)->id;
      }

      /* Find parent collection. */
      Collection *parent = nullptr;

      if (te_selected->parent) {
        for (TreeElement *te_parent = te_selected->parent; te_parent;
             te_parent = te_parent->parent)
        {
          if (outliner_is_collection_tree_element(te_parent)) {
            parent = outliner_collection_from_tree_element(te_parent);
            break;
          }
        }
      }
      else {
        Scene *scene = CTX_data_scene(C);
        parent = scene->master_collection;
      }

      WM_drag_add_local_ID(drag, id, &parent->id);
    }

    selected.selected_array.free_no_destruct();
  }
  else {
    /* Add single ID. */
    WM_drag_add_local_ID(drag, data.drag_id, data.drag_parent);
  }

  WM_event_start_prepared_drag(C, drag);

  ED_outliner_select_sync_from_outliner(C, space_outliner);

  return (OPERATOR_FINISHED | OPERATOR_PASS_THROUGH);
}

/* Outliner drag and drop. This operator mostly exists to support dragging
 * from outliner text instead of only from the icon, and also to show a
 * hint in the status-bar key-map. */

void OUTLINER_OT_item_drag_drop(wmOperatorType *ot)
{
  ot->name = "Drag and Drop";
  ot->idname = "OUTLINER_OT_item_drag_drop";
  ot->description = "Drag and drop element to another place";

  ot->invoke = outliner_item_drag_drop_invoke;
  ot->poll = ED_operator_outliner_active;
}

#undef OUTLINER_DRAG_SCOLL_OUTSIDE_PAD

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Boxes
 * \{ */

void outliner_dropboxes()
{
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find("Outliner", SPACE_OUTLINER, RGN_TYPE_WINDOW);

  WM_dropbox_add(lb, "OUTLINER_OT_parent_drop", parent_drop_poll, nullptr, nullptr, nullptr);
  WM_dropbox_add(lb, "OUTLINER_OT_parent_clear", parent_clear_poll, nullptr, nullptr, nullptr);
  WM_dropbox_add(lb, "OUTLINER_OT_scene_drop", scene_drop_poll, nullptr, nullptr, nullptr);
  WM_dropbox_add(lb, "OUTLINER_OT_material_drop", material_drop_poll, nullptr, nullptr, nullptr);
  WM_dropbox_add(lb,
                 "OUTLINER_OT_datastack_drop",
                 datastack_drop_poll,
                 nullptr,
                 nullptr,
                 datastack_drop_tooltip);
  WM_dropbox_add(lb,
                 "OUTLINER_OT_stack_layer_drop",
                 stack_layer_drop_poll,
                 nullptr,
                 nullptr,
                 stack_layer_drop_tooltip);
  WM_dropbox_add(lb,
                 "OUTLINER_OT_stack_layer_image_drop",
                 stack_image_drop_poll,
                 nullptr,
                 nullptr,
                 stack_image_drop_tooltip);
  WM_dropbox_add(lb,
                 "OUTLINER_OT_stack_layer_material_drop",
                 stack_material_drop_poll,
                 nullptr,
                 nullptr,
                 stack_material_drop_tooltip);
  WM_dropbox_add(lb,
                 "OUTLINER_OT_collection_drop",
                 collection_drop_poll,
                 nullptr,
                 nullptr,
                 collection_drop_tooltip);
}

/** \} */

}  // namespace blender::ed::outliner
