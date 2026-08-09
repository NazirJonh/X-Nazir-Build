/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_material.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "WM_toolsystem.hh"

#include "ED_paint.hh"

namespace blender {

namespace ed::sculpt_paint::canvas {
static TexPaintSlot *get_active_slot(Object &ob)
{
  Material *mat = BKE_object_material_get(&ob, ob.actcol);
  if (mat == nullptr) {
    return nullptr;
  }
  if (mat->texpaintslot == nullptr) {
    return nullptr;
  }
  if (mat->paint_active_slot >= mat->tot_slots) {
    return nullptr;
  }

  TexPaintSlot *slot = &mat->texpaintslot[mat->paint_active_slot];
  return slot;
}

}  // namespace ed::sculpt_paint::canvas

using namespace blender::ed::sculpt_paint::canvas;

/* Does the paint tool with the given idname use a canvas. */
static bool paint_tool_uses_canvas(StringRef idname)
{
  return ELEM(idname, "builtin.color_filter");
}

static bool paint_brush_uses_canvas(bContext *C)
{
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr) {
    return false;
  }

  return ELEM(brush->sculpt_brush_type,
              SCULPT_BRUSH_TYPE_PAINT,
              SCULPT_BRUSH_TYPE_SMEAR,
              SCULPT_BRUSH_TYPE_BLUR);
}

static bool paint_brush_type_shading_color_follows_last_used(StringRef idname)
{
  /* TODO(jbakker): complete this list. */
  return ELEM(idname, "builtin_brush.Mask");
}

void ED_paint_brush_type_update_sticky_shading_color(bContext *C, Object *ob)
{
  if (ob == nullptr || ob->runtime->sculpt_session == nullptr) {
    return;
  }

  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (tref == nullptr) {
    return;
  }
  /* Do not modify when tool follows lat used tool. */
  if (paint_brush_type_shading_color_follows_last_used(tref->idname)) {
    return;
  }

  ob->runtime->sculpt_session->sticky_shading_color = paint_tool_uses_canvas(tref->idname) ||
                                                      paint_brush_uses_canvas(C);
}

static bool paint_brush_type_shading_color_follows_last_used_tool(bContext *C, Object *ob)
{
  if (ob == nullptr || ob->runtime->sculpt_session == nullptr) {
    return false;
  }

  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (tref == nullptr) {
    return false;
  }

  return paint_brush_type_shading_color_follows_last_used(tref->idname);
}

bool ED_paint_brush_type_use_canvas(bContext *C, bToolRef *tref)
{
  BLI_assert(C || tref);

  if (tref == nullptr) {
    tref = WM_toolsystem_ref_from_context(C);
  }
  if (tref == nullptr) {
    return false;
  }

  return paint_tool_uses_canvas(tref->idname) || (C && paint_brush_uses_canvas(C));
}

eV3DShadingColorType ED_paint_shading_color_override(bContext *C,
                                                     const PaintModeSettings *settings,
                                                     Object &ob,
                                                     eV3DShadingColorType orig_color_type)
{
  /* NOTE: This early exit is temporarily, until a paint mode has been added.
   * For better integration with the vertex paint in sculpt mode we sticky
   * with the last stoke when using tools like masking.
   */
  if (!ED_paint_brush_type_use_canvas(C, nullptr) &&
      !(paint_brush_type_shading_color_follows_last_used_tool(C, &ob) &&
        ob.runtime->sculpt_session->sticky_shading_color))
  {
    return orig_color_type;
  }

  eV3DShadingColorType color_type = orig_color_type;
  switch (settings->canvas_source) {
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      color_type = V3D_SHADING_VERTEX_COLOR;
      break;
    case PAINT_CANVAS_SOURCE_IMAGE:
      color_type = V3D_SHADING_TEXTURE_COLOR;
      break;
    case PAINT_CANVAS_SOURCE_MATERIAL: {
      TexPaintSlot *slot = get_active_slot(ob);
      if (slot == nullptr) {
        break;
      }

      if (slot->ima) {
        color_type = V3D_SHADING_TEXTURE_COLOR;
      }
      if (slot->attribute_name) {
        color_type = V3D_SHADING_VERTEX_COLOR;
      }

      break;
    }
    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT: {
      /* Base Color paints into the mesh's active color attribute, which Workbench only reads for
       * the base surface color when shading is in Vertex Color mode (see
       * #Workbench::get_material): under Material Color mode the material's own Base Color node
       * is shown instead, and the scalar channels (Metallic/Roughness/...) are overlaid on top of
       * it via push constants instead. So Base Color needs the same shading override the other
       * canvases get above, or a painted "Color" attribute would never be visible.
       *
       * Driven by #PaintModeSettings.material_shader_visible_channels, not by whether Base Color
       * is currently enabled for painting: display and painting are independent, so toggling
       * Base Color's paint-enable off must not hide already-painted colors, and toggling it on
       * must not force them to display before the corresponding shader-visible toggle is on. */
      const bool base_color_shader_visible = (settings->material_shader_visible_channels &
                                             (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR)) != 0;
      color_type = base_color_shader_visible ? V3D_SHADING_VERTEX_COLOR :
                                              V3D_SHADING_MATERIAL_COLOR;
      break;
    }
  }

  return color_type;
}

}  // namespace blender
