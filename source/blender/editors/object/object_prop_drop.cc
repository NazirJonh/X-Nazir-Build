/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 * \brief Property drag & drop to animatable fields operator.
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_anim_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_anim_data.hh"
#include "BKE_context.hh"
#include "BKE_fcurve.hh"
#include "BKE_fcurve_driver.h"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_object.hh"
#include "ED_keyframing.hh"

#include "ANIM_animdata.hh"
#include "ANIM_fcurve.hh"

#include "object_intern.hh"

namespace blender::ed::object {

/* ------------------------------------------------------------------- */
/** \name Property Drag & Drop Utilities
 * \{ */

/**
 * Parse property drag payload string.
 * Format:
 *   "ID_TYPE:ID_NAME:PROP_NAME"                - property directly on an ID
 *   "ID_TYPE:ID_NAME:NODE_NAME:PROP_NAME"    - property on a node inside a NodeTree
 *
 * r_node_name is set to empty string when the property is directly on the ID.
 */
static bool prop_drag_string_parse(const std::string &s,
                                   std::string &r_id_type,
                                   std::string &r_id_name,
                                   std::string &r_node_name,
                                   std::string &r_prop_name)
{
  size_t p1 = s.find(':');
  if (p1 == std::string::npos) {
    return false;
  }
  size_t p2 = s.find(':', p1 + 1);
  if (p2 == std::string::npos) {
    return false;
  }
  r_id_type = s.substr(0, p1);
  r_id_name = s.substr(p1 + 1, p2 - p1 - 1);

  /* Check for optional 4th field (node name). */
  size_t p3 = s.find(':', p2 + 1);
  if (p3 != std::string::npos) {
    r_node_name = s.substr(p2 + 1, p3 - p2 - 1);
    r_prop_name = s.substr(p3 + 1);
  }
  else {
    r_node_name.clear();
    r_prop_name = s.substr(p2 + 1);
  }

  return !r_id_type.empty() && !r_id_name.empty() && !r_prop_name.empty();
}

/** Resolve ID from type short name (e.g. "OB" -> ID_OB) and name. */
static ID *prop_drag_resolve_id(Main *bmain,
                                const std::string &id_type_str,
                                const std::string &id_name)
{
  /* BKE_idtype_idcode_from_name() can assert in debug builds when given invalid input.
   * Since the drag payload can be malformed (or come from older versions), guard it.
   */
  if (id_type_str.empty() || id_type_str.size() > 64) {
    return nullptr;
  }

  /* The drag payload encodes the ID type as a 2-letter code taken directly from id->name[0..1]
   * (e.g. "OB", "MA", "NT").  This is NOT the human-readable type name that
   * BKE_idtype_idcode_from_name() expects ("Object", "NodeTree", …).
   *
   * The correct way to turn a 2-letter code back into a short idcode is the GS() macro,
   * which simply packs the two chars into a short integer — exactly what id->name stores.
   * We then validate the result with BKE_idtype_idcode_is_valid() before using it, which
   * is safe in debug builds (no assert).
   */
  short idcode = 0;
  if (id_type_str.size() == 2 && (id_type_str[0] >= 'A' && id_type_str[0] <= 'Z') &&
      (id_type_str[1] >= 'A' && id_type_str[1] <= 'Z'))
  {
    /* GS() packs two chars into a short — the same encoding id->name uses. */
    const short candidate = GS(id_type_str.c_str());
    if (BKE_idtype_idcode_is_valid(candidate)) {
      idcode = candidate;
    }
  }

  if (idcode == 0) {
    return nullptr;
  }

  ID *res = BKE_libblock_find_name(bmain, idcode, id_name.c_str());
  if (res) {
    return res;
  }

  /* BKE_libblock_find_name only searches top-level (non-embedded) ID blocks.
   * Embedded NodeTrees (e.g. a Shader NodeTree inside a Material) are stored as
   * ma->nodetree / ob->nodetree / etc. and carry ID_FLAG_EMBEDDED_DATA — they are
   * NOT in bmain->nodetrees.  Search for them by walking every ID in bmain and
   * checking whether it owns an embedded NodeTree with the requested name.
   */
  if (idcode == ID_NT) {
    MainListsArray lbarray = BKE_main_lists_get(*bmain);
    for (ListBase *lb : lbarray) {
      for (ID *owner = static_cast<ID *>(lb->first); owner;
           owner = static_cast<ID *>(owner->next))
      {
        /* node_tree_from_id() returns the embedded nodetree for any ID type that has one. */
        bNodeTree *ntree = bke::node_tree_from_id(owner);
        if (ntree && (ntree->id.name + 2 == id_name)) {
          return &ntree->id;
        }
      }
    }
  }

  return nullptr;
}

/** \} */

/* ------------------------------------------------------------------- */
/** \name Property Drop to Animatable Field Operator
 * \{ */

static wmOperatorStatus object_prop_drop_to_field_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  /* Step 1: Extract operator properties. */
  char drag_string_buf[512];
  char target_property[MAX_NAME];
  char target_id_name[MAX_NAME];
  char target_id_type[3];
  int target_array_index;

  RNA_string_get(op->ptr, "drag_string", drag_string_buf);
  RNA_string_get(op->ptr, "target_property", target_property);
  RNA_string_get(op->ptr, "target_id_name", target_id_name);
  RNA_string_get(op->ptr, "target_id_type", target_id_type);
  target_array_index = RNA_int_get(op->ptr, "target_array_index");

  printf("[DEBUG FIELD EXEC] drag='%s' target_prop='%s' target_id='%s' type='%s' idx=%d\n",
         drag_string_buf, target_property, target_id_name, target_id_type, target_array_index);

  /* Step 2: Parse drag payload. */
  std::string id_type_str, id_name_str, node_name_str, prop_name_str;
  if (!prop_drag_string_parse(
          drag_string_buf, id_type_str, id_name_str, node_name_str, prop_name_str))
  {
    BKE_report(op->reports, RPT_ERROR, "Invalid drag string format");
    return OPERATOR_CANCELLED;
  }

  /* Step 3: Resolve source ID. */
  ID *source_id = prop_drag_resolve_id(bmain, id_type_str, id_name_str);
  if (!source_id) {
    BKE_reportf(op->reports, RPT_ERROR, "Cannot find source ID '%s'", id_name_str.c_str());
    return OPERATOR_CANCELLED;
  }

  /* Step 4: Resolve target ID (supports embedded NodeTrees). */
  ID *target_id = prop_drag_resolve_id(bmain, std::string(target_id_type), std::string(target_id_name));
  if (!target_id) {
    BKE_reportf(op->reports, RPT_ERROR, "Cannot find target ID '%s'", target_id_name);
    return OPERATOR_CANCELLED;
  }

  /* Step 5: Build RNA paths. */
  char source_rna_path[512];
  if (!node_name_str.empty()) {
    BLI_snprintf(source_rna_path,
                 sizeof(source_rna_path),
                 "nodes[\"%s\"][\"%s\"]",
                 node_name_str.c_str(),
                 prop_name_str.c_str());
  }
  else {
    BLI_snprintf(source_rna_path, sizeof(source_rna_path), "[\"%s\"]", prop_name_str.c_str());
  }

  char target_rna_path[512];
  /* NOTE: array_index is passed separately to ANIM_add_driver — do NOT embed [N] in the path. */
  BLI_strncpy(target_rna_path, target_property, sizeof(target_rna_path));
  int driver_array_index = (target_array_index >= 0) ? target_array_index : 0;

  /* Step 6: Create driver. */
  int result = ANIM_add_driver(
      op->reports, target_id, target_rna_path, driver_array_index, 0, DRIVER_TYPE_PYTHON);
  printf("[DEBUG FIELD EXEC] ANIM_add_driver('%s', '%s', idx=%d) = %d\n",
         target_id->name, target_rna_path, driver_array_index, result);
  if (result <= 0) {
    BKE_report(op->reports, RPT_ERROR, "Failed to create driver");
    return OPERATOR_CANCELLED;
  }

  /* Step 7: Configure driver variable. */
  AnimData *adt = BKE_animdata_from_id(target_id);
  if (!adt) {
    BKE_report(op->reports, RPT_ERROR, "Failed to get animation data");
    return OPERATOR_CANCELLED;
  }

  FCurve *fcurve = BKE_fcurve_find(&adt->drivers, target_rna_path, driver_array_index);
  if (!fcurve || !fcurve->driver) {
    BKE_report(op->reports, RPT_ERROR, "Failed to find created driver");
    return OPERATOR_CANCELLED;
  }

  ChannelDriver *driver = fcurve->driver;
  driver->type = DRIVER_TYPE_PYTHON;

  /* Clear auto-generated variables. */
  for (DriverVar *dvar = static_cast<DriverVar *>(driver->variables.first), *next; dvar;
       dvar = next)
  {
    next = dvar->next;
    driver_free_variable(&driver->variables, dvar);
  }

  /* Add new variable. */
  DriverVar *var = driver_add_new_variable(driver);
  if (var) {
    var->type = DVAR_TYPE_SINGLE_PROP;
    BLI_strncpy(var->name, "var", sizeof(var->name));
    var->num_targets = 1;

    DriverTarget *target = &var->targets[0];
    target->idtype = GS(source_id->name);
    target->id = source_id;
    target->rna_path = BLI_strdup(source_rna_path);

    BLI_strncpy(driver->expression, "var", sizeof(driver->expression));
  }

  /* Step 8: Update dependency graph. */
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(target_id, ID_RECALC_ANIMATION);
  WM_event_add_notifier(C, NC_ANIMATION | ND_FCURVES_ORDER, nullptr);

  return OPERATOR_FINISHED;
}

static bool object_prop_drop_to_field_poll(bContext *C)
{
  /* Basic validation - just check we have a context. */
  return CTX_data_main(C) != nullptr;
}

void OBJECT_OT_prop_drop_to_field(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Drop Property to Animatable Field";
  ot->idname = "OBJECT_OT_prop_drop_to_field";
  ot->description = "Create driver from custom property to any animatable field";

  /* API callbacks. */
  ot->exec = object_prop_drop_to_field_exec;
  ot->poll = object_prop_drop_to_field_poll;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  /* Properties. */
  RNA_def_string(ot->srna,
                 "drag_string",
                 nullptr,
                 512,
                 "Drag String",
                 "Encoded property reference");
  RNA_def_string(ot->srna,
                 "target_property",
                 nullptr,
                 MAX_NAME,
                 "Target Property",
                 "Target property identifier");
  RNA_def_string(ot->srna,
                 "target_id_name",
                 nullptr,
                 MAX_NAME,
                 "Target ID Name",
                 "Name of target ID block");
  RNA_def_string(
      ot->srna, "target_id_type", nullptr, 3, "Target ID Type", "Type code of target ID");
  RNA_def_int(ot->srna,
              "target_array_index",
              -1,
              -1,
              100,
              "Array Index",
              "Component index for vector properties",
              -1,
              100);
}

/** \} */

}  // namespace blender::ed::object
