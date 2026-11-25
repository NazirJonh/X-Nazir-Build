/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The operator that creates a row from one of the user's own node groups: a template tree (from
 * #BKE_paint_layers_node_group_template_ensure, the builder the Add flows share), a row of the
 * node-group kind, and the row pointed at the tree. The definition is a standalone shader node
 * tree in the main database, shared by every instance that reads it.
 *
 * The material comes from the context's material, or from the active object's active material --
 * the same material the Layer Material panel edits -- so the operator works from the Add menus
 * and from the panel alike. The definition itself is edited in the Shader Editor, by picking the
 * tree.
 */

#include <string>

#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_node_group.hh"
#include "BKE_report.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name The operator
 * \{ */

enum StackNodeGroupKind {
  STACK_NODE_GROUP_FILTER = 0,
  STACK_NODE_GROUP_SOURCE_LAYER = 1,
  STACK_NODE_GROUP_GENERATOR = 2,
  STACK_NODE_GROUP_SOURCE_CORRECTION = 3,
};

static const EnumPropertyItem stack_node_group_kind_items[] = {
    {STACK_NODE_GROUP_FILTER,
     "FILTER",
     0,
     "Node Group Filter",
     "A filter whose kernel is a node group: Input in, Result out, every other input a parameter "
     "of its own"},
    {STACK_NODE_GROUP_SOURCE_LAYER,
     "SOURCE_LAYER",
     0,
     "Node Group Layer",
     "A layer whose content is a node group's paint-channel outputs"},
    {STACK_NODE_GROUP_GENERATOR,
     "GENERATOR",
     0,
     "Node Group Generator",
     "A pattern generator from a node group: Coordinate in, Fac out, ramped like every generator"},
    {STACK_NODE_GROUP_SOURCE_CORRECTION,
     "SOURCE_CORRECTION",
     0,
     "Node Group Correction",
     "A correction that lays a node group's paint-channel outputs over the row it hangs on"},
    {0, nullptr, 0, nullptr, nullptr},
};

static Material *node_group_operator_material_get(bContext *C)
{
  Material *material = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (material != nullptr) {
    return material;
  }
  Object *object = CTX_data_active_object(C);
  return (object != nullptr) ? BKE_object_material_get(object, object->actcol) : nullptr;
}

static bool node_group_operator_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  const Material *material = node_group_operator_material_get(C);
  return material != nullptr && BKE_paint_layers_is_layered(*material);
}

static wmOperatorStatus node_group_add_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Material *ma = node_group_operator_material_get(C);
  if (bmain == nullptr || ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "The active material carries no paint layer stack");
    return OPERATOR_CANCELLED;
  }
  const int kind = RNA_enum_get(op->ptr, "type");

  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    STRNCPY(name, "Node Group");
  }

  bNodeTree *tree = nullptr;
  MaterialPaintLayer *row = nullptr;
  switch (kind) {
    case STACK_NODE_GROUP_FILTER: {
      MaterialPaintLayer *owner = BKE_paint_layers_find(*ma,
                                                        BKE_paint_layers_active_get(*ma));
      if (owner == nullptr || BKE_paint_layers_is_folder(*owner)) {
        BKE_report(op->reports, RPT_ERROR, "The active row cannot carry corrections");
        return OPERATOR_CANCELLED;
      }
      tree = BKE_paint_layers_node_group_template_ensure(
          *bmain, PaintLayerNodeGroupTemplateKind::Filter, name);
      row = (tree != nullptr) ?
                BKE_paint_layers_filter_add(*ma, owner, MA_PAINT_LAYER_FILTER_NODE_GROUP, name) :
                nullptr;
      break;
    }
    case STACK_NODE_GROUP_SOURCE_LAYER: {
      tree = BKE_paint_layers_node_group_template_ensure(
          *bmain, PaintLayerNodeGroupTemplateKind::SourceLayer, name);
      row = (tree != nullptr) ? BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above) :
                                nullptr;
      if (row != nullptr) {
        /* A source row of the node-group kind: no records of its own (it reads, like a Material
         * row), the definition assigned below. Written straight to the fields on purpose: BKE has
         * no setter for switching a row's source, and a fresh row has no state a setter would have
         * to retire. When a setter appears, this operator should hand the switch over to it. */
        row->source = MA_PAINT_LAYER_SOURCE_NODE_GROUP;
        row->kind_subtype = 0;
      }
      break;
    }
    case STACK_NODE_GROUP_SOURCE_CORRECTION: {
      MaterialPaintLayer *owner = BKE_paint_layers_find(*ma,
                                                        BKE_paint_layers_active_get(*ma));
      if (owner == nullptr || BKE_paint_layers_is_folder(*owner)) {
        BKE_report(op->reports, RPT_ERROR, "The active row cannot carry corrections");
        return OPERATOR_CANCELLED;
      }
      tree = BKE_paint_layers_node_group_template_ensure(
          *bmain, PaintLayerNodeGroupTemplateKind::SourceLayer, name);
      row = (tree != nullptr) ?
                BKE_paint_layers_correction_add(
                    *ma, owner, name, MA_PAINT_LAYER_SOURCE_NODE_GROUP) :
                nullptr;
      break;
    }
    case STACK_NODE_GROUP_GENERATOR: {
      tree = BKE_paint_layers_node_group_template_ensure(
          *bmain, PaintLayerNodeGroupTemplateKind::Generator, name);
      row = (tree != nullptr) ? BKE_paint_layers_generator_add(
                                    *ma,
                                    MA_PAINT_LAYER_GENERATOR_NODE_GROUP,
                                    name,
                                    nullptr,
                                    PaintLayerPlace::Above) :
                                nullptr;
      break;
    }
    default:
      return OPERATOR_CANCELLED;
  }
  if (tree == nullptr || row == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Cannot create the node group row");
    if (tree != nullptr) {
      /* The template was made for this row: with no row there is nothing that reads it, so it
       * leaves the file instead of sitting orphaned in `Main`. */
      BKE_id_free(bmain, &tree->id);
    }
    return OPERATOR_CANCELLED;
  }
  /* The definition first: the assignment seeds the row's parameters from the tree's interface, so
   * the panel lists them before the first draw. */
  if (!BKE_paint_layers_kind_tree_set(*ma, row, tree)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot assign the node group to the row");
    if (kind == STACK_NODE_GROUP_FILTER || kind == STACK_NODE_GROUP_SOURCE_CORRECTION) {
      BKE_paint_layers_correction_remove(*ma, row);
    }
    else {
      BKE_paint_layers_remove(*ma, row);
    }
    /* The assignment was refused, so the template never gained a user: it leaves the file. */
    BKE_id_free(bmain, &tree->id);
    return OPERATOR_CANCELLED;
  }
  /* The row holds its own user on the definition now; drop the creation user the new tree came
   * with, so the instances are the only owners. */
  id_us_min(&tree->id);
  BKE_paint_layers_active_set(*ma, row->marker);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma->id);
  WM_main_add_notifier(NC_NODE | NA_EDITED, &tree->id);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_node_group_add(wmOperatorType *ot)
{
  ot->name = "Add Node Group";
  ot->idname = "OUTLINER_OT_stack_layer_node_group_add";
  ot->description =
      "Create a node group definition and a row that reads it: a filter, a layer or a generator";

  ot->poll = node_group_operator_poll;
  ot->exec = node_group_add_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "type",
                                   stack_node_group_kind_items,
                                   STACK_NODE_GROUP_FILTER,
                                   "Kind",
                                   "Which kind of row the node group serves");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  RNA_def_string(ot->srna,
                 "name",
                 "Node Group",
                 MAX_NAME,
                 "Name",
                 "The name of the new row and of its definition tree");
}

/** \} */

}  // namespace blender::ed::outliner
