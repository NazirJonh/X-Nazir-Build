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

#include "BKE_context.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

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
using ed::sculpt_paint::material_layer::rebake;
using ed::sculpt_paint::material_layer::resize;

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
