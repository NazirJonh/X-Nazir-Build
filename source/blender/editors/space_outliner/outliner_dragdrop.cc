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
TreeElement *outliner_drop_insert_find(bContext *C,
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

TreeElement *outliner_data_from_tree_element_and_parents(CheckTypeFn check_type,
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

bool is_object_element(TreeElement *te)
{
  TreeStoreElem *tselem = TREESTORE(te);
  return (tselem->type == TSE_SOME_ID) && te->idcode == ID_OB;
}

bool is_pchan_element(TreeElement *te)
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

/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */

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
 * #TreeElementStackItem puts the sub-row's own data-block in #TreeElement.directdata -- a map of
 * the paint source's, say -- not the #StackSubRow and not the stack's owner the treestore points
 * at. What is worth dragging out is the same thing #StackDropHandler::drop_id_types already names
 * as worth dropping back in; a sub-row of some other type drags nothing.
 */
static ID *outliner_stack_item_drag_id_get(const SpaceOutliner &space_outliner,
                                           const TreeElement &te)
{
  const TreeStoreElem *tselem = TREESTORE(&te);
  if (tselem->type != TSE_STACK_ITEM) {
    return nullptr;
  }
  ID *stack_item_id = static_cast<ID *>(te.directdata);
  if (stack_item_id == nullptr) {
    return nullptr;
  }
  const StackDropHandler *handler = stack_source_for_space(space_outliner)->drop_handler();
  if (handler == nullptr) {
    return nullptr;
  }
  Vector<short> types;
  handler->drop_id_types(types);
  return types.contains(short(GS(stack_item_id->name))) ? stack_item_id : nullptr;
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
    stack_item_id = outliner_stack_item_drag_id_get(*space_outliner, *te);
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
                 "OUTLINER_OT_stack_layer_id_drop",
                 stack_id_drop_poll,
                 nullptr,
                 nullptr,
                 stack_id_drop_tooltip);
  WM_dropbox_add(lb,
                 "OUTLINER_OT_collection_drop",
                 collection_drop_poll,
                 nullptr,
                 nullptr,
                 collection_drop_tooltip);
}

/** \} */

}  // namespace blender::ed::outliner
