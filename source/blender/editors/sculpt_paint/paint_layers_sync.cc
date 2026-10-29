/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The paint-side half of the material layer edits made outside a stroke: which target a stroke
 * paints (content or mask), the maps the Image Editor's slots hold, and what a fresh mask starts
 * from. The Stack Layers source and the Outliner's shared mask helper call these as clients, so
 * the Outliner owns no paint logic.
 */

#include "BKE_context.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_report.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "ED_paint.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

namespace blender::ed::sculpt_paint {

void paint_stack_texpaint_slots_refresh(bContext &C)
{
  Scene *scene = CTX_data_scene(&C);
  Object *object = CTX_data_active_object(&C);
  if (scene != nullptr && object != nullptr) {
    BKE_texpaint_slots_refresh_object(scene, object);
  }
  /* Every change of the active row, added row or mode passes here, so the PBR Paint tab never
   * waits for a separate opt-in button while a layer is active. */
  if (Paint *paint = BKE_paint_get_active_from_context(&C)) {
    if (BKE_paint_material_brush_auto_ensure(scene, paint, object)) {
      WM_event_add_notifier(&C, NC_BRUSH | NA_EDITED, BKE_paint_brush(paint));
    }
  }
}

bool paint_stack_target_mode_set(bContext &C, const ePaintLayerTargetMode mode)
{
  Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  /* Choosing a paint target (Mask or content) ends object-mask editing, even when the target is
   * already the one asked for. */
  if (paint_mode.layer_object_mask_edit != 0) {
    paint_mode.layer_object_mask_edit = 0;
    WM_event_add_notifier(&C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  }
  if (paint_mode.layer_target_mode == mode) {
    return false;
  }
  const bool leaving_mask = paint_mode.layer_target_mode == PAINT_LAYER_TARGET_MASK &&
                            mode != PAINT_LAYER_TARGET_MASK;
  if (Paint *paint = BKE_paint_get_active_from_context(&C)) {
    /* Puts the Mask brush in hand on the way in and the remembered content brush on the way
     * out. */
    BKE_paint_material_layer_target_mode_set(*CTX_data_main(&C), *scene, *paint, paint_mode, mode);
    if (leaving_mask && paint->brush != nullptr) {
      /* The restored content brush may belong to another tool, so that tool becomes active too. */
      WM_toolsystem_activate_brush_and_tool(&C, paint, paint->brush);
    }
  }
  else {
    paint_mode.layer_target_mode = int8_t(mode);
  }
  paint_stack_texpaint_slots_refresh(C);
  WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, scene);
  return true;
}

void paint_layers_mask_base_map_prepare(bContext &C, Material &material, MaterialPaintLayer &row)
{
  const Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }
  /* Made ahead of the stroke that would make it, so the first dab does not grow the description
   * and regenerate the graph before it can write (see #BKE_paint_layers_mask_base_map_ensure). */
  const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  if (BKE_paint_layers_mask_base_map_ensure(
          *CTX_data_main(&C), material, row, paint_mode.new_channel_image_size) != nullptr)
  {
    paint_stack_texpaint_slots_refresh(C);
  }
}

MaterialPaintLayer *paint_layers_mask_add_image(bContext &C,
                                                Material &material,
                                                MaterialPaintLayer &layer,
                                                Image &image)
{
  if (BKE_paint_layers_mask_base(layer) != nullptr) {
    BKE_report(CTX_wm_reports(&C), RPT_WARNING, "This layer already has a mask");
    return nullptr;
  }
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add_image(material, layer, image);
  if (mask == nullptr) {
    return nullptr;
  }
  /* The correction the pick just made is what the user works on: it holds the texture, so it is
   * the active mask item and the one Mask mode paints. */
  BKE_paint_layers_active_set(material, mask->marker);
  paint_stack_target_mode_set(C, PAINT_LAYER_TARGET_MASK);
  /* The base mask's map is made together with the mask, so a stroke on that base later does not
   * lose its first dab. The refresh also covers the mode having already been Mask, where the mode
   * set refreshed nothing. */
  paint_layers_mask_base_map_prepare(C, material, layer);
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
  return mask;
}

}  // namespace blender::ed::sculpt_paint
