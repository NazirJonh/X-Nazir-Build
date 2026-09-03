/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Layer Material tab's operators, thin wrappers over #ED_paint_material_layer.hh: the tab
 * only turns its own controls into a call there and back.
 */

#include <optional>
#include <string>

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "ED_paint_material_layer.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_intern.hh" /* own include */

namespace blender {

using ed::sculpt_paint::material_layer::add_material_layer_from_material;

/* Values must match #PaintLayerPlace (a runtime-only type, unavailable in RNA). */
static const EnumPropertyItem paint_layer_place_items[] = {
    {0, "ABOVE", 0, "Above", "Directly above the anchor"},
    {1, "BELOW", 0, "Below", "Directly below the anchor"},
    {2, "INTO", 0, "Into", "Inside the anchor folder"},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool paint_layer_add_material_poll(bContext *C)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (owner == nullptr || !paint_layers_is_layered(*owner)) {
    CTX_wm_operator_poll_msg_set(C, "No layered material");
    return false;
  }
  if (ID_IS_LINKED(&owner->id) || !ID_IS_EDITABLE(&owner->id)) {
    CTX_wm_operator_poll_msg_set(C, "The material is not editable");
    return false;
  }
  return true;
}

/** Every material in the file, keyed by session UID, so the pick survives a reorder. */
static const EnumPropertyItem *paint_layer_source_material_itemf(bContext *C,
                                                                 PointerRNA * /*ptr*/,
                                                                 PropertyRNA * /*prop*/,
                                                                 bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  Main *bmain = (C != nullptr) ? CTX_data_main(C) : nullptr;
  if (bmain != nullptr) {
    for (Material &ma : bmain->materials) {
      EnumPropertyItem item = {};
      /* The value is the session UID, not the position: the lookup at exec time is stable. */
      item.value = int(ma.id.session_uid);
      item.identifier = BLI_strdup(ma.id.name + 2);
      item.name = BLI_strdup(ma.id.name + 2);
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static wmOperatorStatus paint_layer_add_material_exec(bContext *C, wmOperator *op)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (owner == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No layered material");
    return OPERATOR_CANCELLED;
  }
  const int source_uid = RNA_enum_get(op->ptr, "source");
  Material *source = nullptr;
  Main *bmain = CTX_data_main(C);
  for (Material &ma : bmain->materials) {
    if (int(ma.id.session_uid) == source_uid) {
      source = &ma;
      break;
    }
  }
  if (source == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No material to bake from");
    return OPERATOR_CANCELLED;
  }
  /* Placed relative to the active row, like the stack UI's Add; the top of the stack when none. */
  MaterialPaintLayer *anchor = BKE_paint_layers_find(
      *owner, BKE_paint_layers_active_get(*owner));
  const int place = RNA_enum_get(op->ptr, "place");
  if (add_material_layer_from_material(
          *C, *owner, *source, anchor, PaintLayerPlace(place)) == nullptr)
  {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_add_material(wmOperatorType *ot)
{
  ot->name = "Add Material Paint Layer";
  ot->description = "Add a row on top of the stack, baked from a material's Principled channels";
  ot->idname = "MATERIAL_OT_paint_layer_add_material";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_add_material_exec;
  ot->poll = paint_layer_add_material_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "source",
                                   rna_enum_dummy_NULL_items,
                                   0,
                                   "Source Material",
                                   "The material to bake the row from");
  RNA_def_enum_funcs(prop, paint_layer_source_material_itemf);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_enum(ot->srna,
                      "place",
                      paint_layer_place_items,
                      0,
                      "Place",
                      "Where the new row lands relative to the anchor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** The flattened description rows, in the order the operator's enum hands out. */
static const EnumPropertyItem *paint_layer_use_row_source_itemf(bContext *C,
                                                                PointerRNA * /*ptr*/,
                                                                PropertyRNA * /*prop*/,
                                                                bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  Material *owner = (C != nullptr) ? static_cast<Material *>(
                        CTX_data_pointer_get_type(C, "material", RNA_Material).data) :
                                     nullptr;
  if (owner != nullptr && paint_layers_is_layered(*owner)) {
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(*owner, layers);
    for (const int i : layers.index_range()) {
      const MaterialPaintLayer &layer = *layers[i];
      char identifier[16];
      SNPRINTF(identifier, "%d", i);
      EnumPropertyItem item = {};
      item.value = i;
      item.identifier = BLI_strdup(identifier);
      item.name = BLI_strdup(layer.name[0] != '\0' ? layer.name : "Layer");
      item.description = "Bake this row's result into the active row's channel maps";
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static bool paint_layer_use_row_result_poll(bContext *C)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (owner == nullptr || !paint_layers_is_layered(*owner) ||
      BLI_uuid_is_nil(BKE_paint_layers_active_get(*owner)))
  {
    CTX_wm_operator_poll_msg_set(C, "No active paint layer");
    return false;
  }
  return true;
}

static void paint_layer_row_result_start(void *customdata, wmJobWorkerStatus * /*status*/)
{
  BKE_paint_layers_row_result_job_compute(
      *static_cast<PaintLayersRowResultJob *>(customdata));
}

static void paint_layer_row_result_end(void *customdata)
{
  PaintLayersRowResultJob *job = static_cast<PaintLayersRowResultJob *>(customdata);
  if (BKE_paint_layers_row_result_job_commit(*job)) {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
  }
}

static void paint_layer_row_result_free(void *customdata)
{
  BKE_paint_layers_row_result_job_free(*static_cast<PaintLayersRowResultJob *>(customdata));
}

static wmOperatorStatus paint_layer_use_row_result_exec(bContext *C, wmOperator *op)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  Main *bmain = CTX_data_main(C);
  if (owner == nullptr || bmain == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *active = BKE_paint_layers_find(
      *owner, BKE_paint_layers_active_get(*owner));
  if (active == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active paint layer");
    return OPERATOR_CANCELLED;
  }
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(*owner, layers);
  const int source_index = RNA_enum_get(op->ptr, "source");
  if (source_index < 0 || source_index >= layers.size()) {
    BKE_report(op->reports, RPT_ERROR, "No row to take the result from");
    return OPERATOR_CANCELLED;
  }
  const MaterialPaintLayer &source = *layers[source_index];

  int size = 1024;
  Scene *scene = CTX_data_scene(C);
  if (scene != nullptr && scene->toolsettings != nullptr) {
    size = scene->toolsettings->paint_mode.new_channel_image_size;
  }
  if (size <= 0) {
    size = 1024;
  }

  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      *bmain, *owner, source.marker, active->marker, size);
  if (job == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No row to take the result from");
    return OPERATOR_CANCELLED;
  }

  /* The same threshold the bake planner uses: a heavy row at 2048+ pixels leaves the main thread,
   * a light one is rendered inline so the operator can report its outcome immediately. */
  const bool heavy = BKE_paint_layers_bake_is_heavy(*owner, source) ||
                     size >= PAINT_LAYERS_HEAVY_BAKE_SIZE;
  wmWindowManager *wm = CTX_wm_manager(C);
  if (heavy && wm != nullptr) {
    wmWindow *win = CTX_wm_window(C);
    wmJob *wm_job = WM_jobs_get(wm,
                                win,
                                owner,
                                "Rendering row result...",
                                WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                                WM_JOB_TYPE_PAINT_LAYERS_ROW_RESULT);
    WM_jobs_customdata_set(wm_job, job, paint_layer_row_result_free);
    WM_jobs_timer(wm_job, 0.2, NC_MATERIAL, NC_MATERIAL);
    WM_jobs_callbacks(
        wm_job, paint_layer_row_result_start, nullptr, nullptr, paint_layer_row_result_end);
    WM_jobs_start(wm, wm_job);
    return OPERATOR_FINISHED;
  }

  BKE_paint_layers_row_result_job_compute(*job);
  const bool any = BKE_paint_layers_row_result_job_commit(*job);
  BKE_paint_layers_row_result_job_free(*job);
  if (!any) {
    BKE_report(op->reports, RPT_ERROR, "The row contributes to no channel here");
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &owner->id);
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_use_row_result(wmOperatorType *ot)
{
  ot->name = "Use Row Result";
  ot->description =
      "Bake another row's result for every channel it takes part in into this row's channel maps";
  ot->idname = "MATERIAL_OT_paint_layer_use_row_result";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_use_row_result_exec;
  ot->poll = paint_layer_use_row_result_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "source",
                                   rna_enum_dummy_NULL_items,
                                   0,
                                   "Source Row",
                                   "The row whose result is baked");
  RNA_def_enum_funcs(prop, paint_layer_use_row_source_itemf);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static bool paint_layer_custom_poll(bContext *C)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (owner == nullptr || !paint_layers_is_layered(*owner)) {
    CTX_wm_operator_poll_msg_set(C, "No layered material");
    return false;
  }
  return true;
}

static wmOperatorStatus paint_layer_add_custom_exec(bContext *C, wmOperator *op)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  Main *bmain = CTX_data_main(C);
  if (owner == nullptr || bmain == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *anchor = BKE_paint_layers_find(
      *owner, BKE_paint_layers_active_get(*owner));
  char name[MAX_NAME];
  RNA_string_get(op->ptr, "name", name);
  MaterialPaintLayer *layer = BKE_paint_layers_custom_layer_add(
      *bmain, *owner, name, anchor, PaintLayerPlace::Above);
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not create a Custom layer");
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &owner->id);
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_add_custom(wmOperatorType *ot)
{
  ot->name = "New Custom Layer";
  ot->description = "Add a Custom layer backed by a new node group template";
  ot->idname = "MATERIAL_OT_paint_layer_add_custom";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_add_custom_exec;
  ot->poll = paint_layer_custom_poll;

  RNA_def_string(ot->srna, "name", "Custom", MAX_NAME, "Name", "Name of the new layer");
}

static wmOperatorStatus paint_layer_custom_channel_add_exec(bContext *C, wmOperator *op)
{
  Material *owner = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  Main *bmain = CTX_data_main(C);
  if (owner == nullptr || bmain == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(
      *owner, BKE_paint_layers_active_get(*owner));
  if (layer == nullptr || layer->source != MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
    BKE_report(op->reports, RPT_ERROR, "No active Custom layer");
    return OPERATOR_CANCELLED;
  }
  const int channel = RNA_enum_get(op->ptr, "channel");
  if (!BKE_paint_layers_custom_channel_add(
          *bmain, *owner, *layer, eMaterialPaintChannel(channel)))
  {
    BKE_report(op->reports, RPT_ERROR, "The Custom layer already declares this channel");
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &owner->id);
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_custom_channel_add(wmOperatorType *ot)
{
  ot->name = "Add Custom Channel";
  ot->description = "Declare one more BELOW/COLOR channel pair on the active Custom layer";
  ot->idname = "MATERIAL_OT_paint_layer_custom_channel_add";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_custom_channel_add_exec;
  ot->poll = paint_layer_custom_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "channel",
                                   rna_enum_material_paint_channel_items,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   "Channel",
                                   "Channel to declare");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

}  // namespace blender
