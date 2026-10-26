/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The gradient tools' (Sculpt Color Gradient, Image Editor selection gradient) gizmo channel and
 * per-channel value ramps: the same channel picker and ramp creation the Gradient Layer's panel
 * has, so the tools can edit a scalar material channel through its own grey ramp instead of the
 * single-value fade. The ramps live scene-wide on #PaintModeSettings and are shared by both
 * tools, which paint the same channel maps of the same material.
 */

#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "DNA_scene_types.h"

#include "paint_intern.hh"

namespace blender {

static bool gradient_tool_scene_poll(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr;
}

static wmOperatorStatus gradient_tool_gizmo_channel_set_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  scene->toolsettings->paint_mode.gradient_gizmo_channel = int8_t(channel);
  /* A viewport aid like the Gradient Layer's same-named operator: redraw the gizmo, no rebuild. */
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
  return OPERATOR_FINISHED;
}

void PAINT_OT_gradient_tool_gizmo_channel_set(wmOperatorType *ot)
{
  ot->name = "Set Gradient Gizmo Channel";
  ot->idname = "PAINT_OT_gradient_tool_gizmo_channel_set";
  ot->description =
      "Choose which channel's ramp the gradient tools' gizmos draw: a colour channel shows the "
      "colour ramp, a scalar channel its value ramp (reading as grey)";

  ot->exec = gradient_tool_gizmo_channel_set_exec;
  ot->poll = gradient_tool_scene_poll;

  ot->flag = 0;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_gradient_tool_gizmo_channel_items,
                          PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                          "Channel",
                          "The channel whose ramp the gizmos draw");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus gradient_tool_value_ramp_add_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  if (BKE_paint_gradient_value_ramp_get(scene->toolsettings->paint_mode, channel) != nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (BKE_paint_gradient_value_ramp_ensure(scene->toolsettings->paint_mode, channel) == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "This channel cannot have a value ramp");
    return OPERATOR_CANCELLED;
  }
  /* The Image Editor's live gradient preview repaints from the bumped revision; the Sculpt tool's
   * interactive session sees the change through its settings fingerprint. */
  ED_image_paint_select_gradient_settings_revision_bump();
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, scene);
  /* OPTYPE_UNDO already opens and closes the step around this call; a manual push here would
   * record the same change twice. */
  return OPERATOR_FINISHED;
}

void PAINT_OT_gradient_tool_value_ramp_add(wmOperatorType *ot)
{
  ot->name = "Add Gradient Value Ramp";
  ot->idname = "PAINT_OT_gradient_tool_value_ramp_add";
  ot->description =
      "Create this scalar channel's value ramp for the gradient tools: the channel then paints "
      "through the ramp's grey stops instead of fading between its two single values";

  ot->exec = gradient_tool_value_ramp_add_exec;
  ot->poll = gradient_tool_scene_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_gradient_tool_gizmo_channel_items,
                          PAINT_MATERIAL_CHANNEL_METALLIC,
                          "Channel",
                          "The scalar channel to create the value ramp for");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender
