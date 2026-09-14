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
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "ED_paint_material_layer.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_intern.hh" /* own include */

namespace blender {

using ed::sculpt_paint::material_layer::channel_toggle;
using ed::sculpt_paint::material_layer::channel_unlink;
using ed::sculpt_paint::material_layer::channel_value_set;
using ed::sculpt_paint::material_layer::rebake;
using ed::sculpt_paint::material_layer::resize;
using ed::sculpt_paint::material_layer::use_layer_result;

/* -------------------------------------------------------------------- */
/** \name Material Paint Layer Settings
 *
 * The Layer Material tab's controls. The active layer is the one the scene's channel bindings
 * point at (#BKE_paint_material_active_layer_get). Its channel states are read from the stack; a
 * Material layer's source and resolution are read back from its maps' bake links.
 * \{ */

static bool paint_layer_settings_poll(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  if (bmain == nullptr || scene == nullptr || scene->toolsettings == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No active paint layer");
    return false;
  }
  const std::optional<PaintMaterialActiveLayer> layer = BKE_paint_material_active_layer_get(
      *bmain, scene->toolsettings->paint_mode);
  if (!layer.has_value()) {
    CTX_wm_operator_poll_msg_set(C, "No active paint layer");
    return false;
  }
  if (!ID_IS_EDITABLE(&layer->owner->id) ||
      (layer->source != nullptr && !ID_IS_EDITABLE(&layer->source->id)))
  {
    CTX_wm_operator_poll_msg_set(C, "The layer's material is not editable");
    return false;
  }
  return true;
}

static bool paint_layer_material_settings_poll(bContext *C)
{
  /* Cheap on the repeat: the resolver caches the previous call's answer for as long as the
   * bindings and the owner's stack stay put. */
  if (!paint_layer_settings_poll(C)) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  const std::optional<PaintMaterialActiveLayer> layer = BKE_paint_material_active_layer_get(
      *bmain, scene->toolsettings->paint_mode);
  if (!layer.has_value() || !layer->is_material()) {
    CTX_wm_operator_poll_msg_set(C, "The active layer is not a Material layer");
    return false;
  }
  return true;
}

static wmOperatorStatus paint_layer_channel_toggle_exec(bContext *C, wmOperator *op)
{
  const int channel = RNA_enum_get(op->ptr, "channel");
  if (!channel_toggle(*C, *op->reports, channel)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_channel_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Paint Layer Channel";
  ot->description =
      "Switch this channel of the active paint layer on or off. Off keeps the map and leaves the "
      "channel to the layers below; on restores it, or gives the layer the channel when it has "
      "none";
  ot->idname = "MATERIAL_OT_paint_layer_channel_toggle";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_channel_toggle_exec;
  ot->poll = paint_layer_settings_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "channel",
                                   rna_enum_material_paint_channel_items,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   "Channel",
                                   "Channel to switch on or off");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static wmOperatorStatus paint_layer_channel_value_set_exec(bContext *C, wmOperator *op)
{
  const int channel = RNA_enum_get(op->ptr, "channel");
  float value[4];
  RNA_float_get_array(op->ptr, "value", value);
  if (!channel_value_set(*C, *op->reports, channel, value)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_channel_value_set(wmOperatorType *ot)
{
  ot->name = "Set Paint Layer Channel Value";
  ot->description = "Fill this channel of the active paint layer with a flat value";
  ot->idname = "MATERIAL_OT_paint_layer_channel_value_set";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_channel_value_set_exec;
  ot->poll = paint_layer_settings_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "channel",
                                   rna_enum_material_paint_channel_items,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   "Channel",
                                   "Channel to set the value of");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  static const float value_default[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  prop = RNA_def_float_color(ot->srna,
                             "value",
                             4,
                             value_default,
                             0.0f,
                             1.0f,
                             "Value",
                             "Flat value to fill the channel's map with",
                             0.0f,
                             1.0f);
  RNA_def_property_subtype(prop, PROP_COLOR);
}

static wmOperatorStatus paint_layer_channel_unlink_exec(bContext *C, wmOperator *op)
{
  const int channel = RNA_enum_get(op->ptr, "channel");
  if (!channel_unlink(*C, *op->reports, channel)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_channel_unlink(wmOperatorType *ot)
{
  ot->name = "Unlink Paint Layer Channel";
  ot->description = "Detach the image this channel of the active paint layer shows, and give the "
                    "channel a flat map at its last value instead";
  ot->idname = "MATERIAL_OT_paint_layer_channel_unlink";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_channel_unlink_exec;
  ot->poll = paint_layer_settings_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "channel",
                                   rna_enum_material_paint_channel_items,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   "Channel",
                                   "Channel to unlink");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/**
 * The rows a "Use Layer Result" bake can take a result from: every row of the active row's stack
 * but the active one itself -- baking the active row into itself is pointless. When the active row
 * is a correction nothing is skipped: it bakes the row it hangs on as readily as any other.
 */
static const EnumPropertyItem *paint_layer_source_ordinal_itemf(bContext *C,
                                                               PointerRNA * /*ptr*/,
                                                               PropertyRNA * /*prop*/,
                                                               bool *r_free)
{
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  Main *bmain = (C != nullptr) ? CTX_data_main(C) : nullptr;
  Scene *scene = (C != nullptr) ? CTX_data_scene(C) : nullptr;
  const std::optional<PaintMaterialActiveLayer> active =
      (bmain != nullptr && scene != nullptr && scene->toolsettings != nullptr) ?
          BKE_paint_material_active_layer_get(*bmain, scene->toolsettings->paint_mode) :
          std::nullopt;
  if (active.has_value()) {
    const int skip_ordinal = BLI_uuid_is_nil(active->correction) ? active->ordinal : -1;
    Vector<PaintMaterialLayerStackEntry> entries;
    BKE_paint_material_layer_stack_from_material(*bmain, *active->owner, entries);
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal == skip_ordinal) {
        continue;
      }
      std::string name = entry.name;
      if (name.empty()) {
        /* Rows the user never named read as their kind, the way the stack UI labels them. */
        if (entry.is_group) {
          name = IFACE_("Layer Group");
        }
        else if (entry.kind == PaintMaterialLayerKind::Fill) {
          name = IFACE_("Fill Layer");
        }
        else if (entry.kind == PaintMaterialLayerKind::Material) {
          name = IFACE_("Material Layer");
        }
        else {
          name = IFACE_("Paint Layer");
        }
      }
      char identifier[8];
      SNPRINTF_UTF8(identifier, "%d", int(entry.ordinal));
      EnumPropertyItem item = {};
      item.value = entry.ordinal;
      item.identifier = BLI_strdup(identifier);
      item.name = BLI_strdup(name.c_str());
      item.description = "Bake this row's result";
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  *r_free = true;
  return items;
}

static wmOperatorStatus paint_layer_use_layer_result_exec(bContext *C, wmOperator *op)
{
  const int channel = RNA_enum_get(op->ptr, "channel");
  const int source_ordinal = RNA_enum_get(op->ptr, "source_ordinal");
  if (!use_layer_result(*C, *op->reports, channel, source_ordinal)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_use_layer_result(wmOperatorType *ot)
{
  ot->name = "Use Layer Result";
  ot->description = "Bake another stack row's result for this channel and use it as the channel's "
                    "texture";
  ot->idname = "MATERIAL_OT_paint_layer_use_layer_result";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_use_layer_result_exec;
  ot->poll = paint_layer_settings_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "channel",
                                   rna_enum_material_paint_channel_items,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   "Channel",
                                   "Channel to bake the row's result into");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_enum(ot->srna,
                      "source_ordinal",
                      rna_enum_dummy_NULL_items,
                      0,
                      "Source Row",
                      "Position in the stack of the row to bake");
  RNA_def_enum_funcs(prop, paint_layer_source_ordinal_itemf);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus paint_layer_rebake_exec(bContext *C, wmOperator *op)
{
  if (!rebake(*C, *op->reports)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_rebake(wmOperatorType *ot)
{
  ot->name = "Re-bake Paint Layer";
  ot->description = "Bake every map of the active Material paint layer from its source "
                      "material again";
  ot->idname = "MATERIAL_OT_paint_layer_rebake";
  /* No undo step: the maps' pixels are not part of undo, and nothing else changes. */
  ot->flag = OPTYPE_REGISTER;
  ot->exec = paint_layer_rebake_exec;
  ot->poll = paint_layer_material_settings_poll;
}

static const EnumPropertyItem paint_layer_bake_size_items[] = {
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_256, "SIZE_256", 0, "256", "256 x 256"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_512, "SIZE_512", 0, "512", "512 x 512"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_1K, "SIZE_1K", 0, "1K", "1024 x 1024"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_2K, "SIZE_2K", 0, "2K", "2048 x 2048"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_4K, "SIZE_4K", 0, "4K", "4096 x 4096"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_8K, "SIZE_8K", 0, "8K", "8192 x 8192"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus paint_layer_bake_size_set_exec(bContext *C, wmOperator *op)
{
  const int size = RNA_enum_get(op->ptr, "size");
  if (!resize(*C, *op->reports, size)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_bake_size_set(wmOperatorType *ot)
{
  ot->name = "Set Paint Layer Resolution";
  ot->description = "Resize the maps of the active Material paint layer and bake them again";
  ot->idname = "MATERIAL_OT_paint_layer_bake_size_set";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_bake_size_set_exec;
  ot->poll = paint_layer_material_settings_poll;

  ot->prop = RNA_def_enum(ot->srna,
                          "size",
                          paint_layer_bake_size_items,
                          PAINT_NEW_CHANNEL_IMAGE_SIZE_2K,
                          "Resolution",
                          "Square side of the layer's maps");
}

/** \} */

}  // namespace blender
