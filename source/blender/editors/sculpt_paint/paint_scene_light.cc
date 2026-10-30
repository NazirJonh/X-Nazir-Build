/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Drags #View3DShading.studiolight_rot_z -- the Rotation the Viewport Shading popover shows for
 * the studio light -- so a painted material's response can be swept by the light without leaving
 * the brush. The 3D Viewport counterpart of the Image Editor's Combined-preview light drag.
 */

#include "BLI_math_constants.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_workspace_types.h"

#include "ED_util_dial_drag.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_intern.hh"

namespace blender {

/**
 * The turn of the light, in radians, for a cursor travel of the whole region width: half a turn, so
 * one sweep across the viewport turns the light by 180 degrees, reachable from any start point. It
 * follows horizontal travel from the press point rather than a bearing around it, so the result
 * does not depend on where in the viewport the drag started.
 */
constexpr float SCENE_LIGHT_ROTATE_SPAN = float(M_PI);

/** Whether the shading currently shown is one whose light the rotation actually moves. */
static bool scene_light_rotate_shading_uses_rotation(const View3DShading &shading)
{
  switch (shading.type) {
    case OB_SOLID:
      /* Workbench turns the studio light by this only when the lighting is fixed in the world --
       * the World toggle beside the slider (#use_world_space_lighting). */
      return shading.light == V3D_LIGHTING_STUDIO &&
             (shading.flag & V3D_SHADING_WORLD_ORIENTATION) != 0;
    case OB_MATERIAL:
      /* Material Preview reads the rotation for a studio world, not the scene's. */
      return (shading.flag & V3D_SHADING_SCENE_WORLD) == 0;
    case OB_RENDER:
      return (shading.flag & V3D_SHADING_SCENE_WORLD_RENDER) == 0;
    default:
      return false;
  }
}

static bool scene_light_rotate_poll(bContext *C)
{
  const SpaceLink *space = CTX_wm_space_data(C);
  if (space == nullptr || space->spacetype != SPACE_VIEW3D) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH || ob->mode != OB_MODE_SCULPT) {
    return false;
  }
  /* The Paint toolbar group, the PBR painting this gesture is an inspection aid for. The tool
   * reference rather than the active brush alone: the brush stays the last used one while an
   * operator tool is active (see #material_view_pass_cycle_poll for the same reasoning). */
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (tref == nullptr || tref->runtime == nullptr ||
      (tref->runtime->flag & TOOLREF_FLAG_USE_BRUSHES) == 0 || !paint_pbr_brush_type_active(C))
  {
    return false;
  }
  /* Only where the shading's own light takes the rotation: elsewhere the drag would move a value
   * nothing displays, and the press must fall through to the stencil controls beneath it in the
   * keymap. */
  const View3D *v3d = CTX_wm_view3d(C);
  return v3d != nullptr && scene_light_rotate_shading_uses_rotation(v3d->shading);
}

static float scene_light_angle_get(bContext *C)
{
  return CTX_wm_view3d(C)->shading.studiolight_rot_z;
}

static void scene_light_angle_set(bContext *C, const float angle)
{
  CTX_wm_view3d(C)->shading.studiolight_rot_z = angle;
}

static const ed::DialDragParams scene_light_drag_params = {
    scene_light_angle_get,
    scene_light_angle_set,
    N_("Scene Light Rotation"),
    SCENE_LIGHT_ROTATE_SPAN,
    NC_SPACE | ND_SPACE_VIEW3D | NS_VIEW3D_SHADING,
    true,
};

static wmOperatorStatus scene_light_rotate_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return ed::dial_drag_invoke(C, op, event, scene_light_drag_params);
}

void PAINT_OT_scene_light_rotate(wmOperatorType *ot)
{
  ot->name = "Rotate Scene Light";
  ot->idname = "PAINT_OT_scene_light_rotate";
  ot->description =
      "Drag to turn the viewport shading's studio light (the Rotation in the Viewport Shading "
      "popover), checking the painted material under another lighting (PBR Paint)";

  ot->invoke = scene_light_rotate_invoke;
  ot->modal = ed::dial_drag_modal;
  ot->cancel = ed::dial_drag_cancel;
  ot->poll = scene_light_rotate_poll;

  /* A per-viewport shading setting, like #IMAGE_OT_combined_light_rotate's per-editor one: not
   * scene data, so no undo step. */
  ot->flag = OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_XY;
}

}  // namespace blender
