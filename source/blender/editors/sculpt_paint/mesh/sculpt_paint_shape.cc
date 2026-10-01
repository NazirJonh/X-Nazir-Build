/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D vertex backend dispatcher of the shape drawing tools; see #sculpt_paint_shape.hh.
 *
 * The shape is defined in region pixels, so every mesh element is projected into the region, the
 * shared #ShapeEvaluator is sampled there, and the value is shaded with the same shader the Image
 * Editor compositor uses. This file only picks the backend for the paint mode's canvas source;
 * the sample / attribute / pixel work lives in `sculpt_paint_shape_sample.cc`,
 * `sculpt_paint_shape_attr.cc` and `sculpt_paint_shape_pixels.cc`.
 *
 * Two canvas families are written here:
 * - Mesh attributes: Material Paint (the enabled PBR channel attributes) and Color Attribute.
 * - Image maps: the Material channel maps and the Image canvas, through the PBVH pixels.
 *
 * Mesh symmetry mirrors the vertex position and normal before sampling, exactly like the color
 * gradient gesture: the shape itself is never copied, its mirror image appears where the flipped
 * vertex lands in the region. Canvas symmetry does not apply in 3D.
 *
 * Back-facing vertices are rejected, so a shape drawn over a solid surface does not paint the far
 * side through the model.
 */

#include "sculpt_paint_shape_intern.hh"

#include "BKE_scene.hh"

namespace blender::ed::sculpt_paint::shape {

bool shape_canvas_is_image(const ToolSettings &toolsettings)
{
  const ePaintCanvasSource src = toolsettings.paint_mode.canvas_source;
  return src == PAINT_CANVAS_SOURCE_MATERIAL || src == PAINT_CANVAS_SOURCE_IMAGE;
}

bool shape_canvas_supports_preview(const ToolSettings &toolsettings)
{
  switch (toolsettings.paint_mode.canvas_source) {
    case PAINT_CANVAS_SOURCE_MATERIAL:
    case PAINT_CANVAS_SOURCE_IMAGE:
    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT:
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      return true;
    default:
      return false;
  }
}

const Depsgraph *Sculpt3DTargetContext::depsgraph_get(const bContext *C) const
{
  if (C != nullptr) {
    if (const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C)) {
      return depsgraph;
    }
  }
  return BKE_scene_get_depsgraph(scene, view_layer);
}

std::unique_ptr<ShapeTargetBackend> shape_backend_create(Object &ob,
                                                         Scene &scene,
                                                         ViewLayer &view_layer,
                                                         const ShapeSpace &space,
                                                         ToolSettings &toolsettings,
                                                         Paint &paint,
                                                         const PaintShapeSettings &settings)
{
  Sculpt3DTargetContext ctx;
  ctx.ob = &ob;
  ctx.scene = &scene;
  ctx.view_layer = &view_layer;
  ctx.space = &space;
  ctx.toolsettings = &toolsettings;
  ctx.paint = &paint;
  ctx.settings = &settings;
  switch (toolsettings.paint_mode.canvas_source) {
    case PAINT_CANVAS_SOURCE_MATERIAL_PAINT:
      return std::make_unique<SculptMaterialPaintBackend>(ctx);
    case PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE:
      return std::make_unique<SculptColorAttributeBackend>(ctx);
    default:
      return std::make_unique<SculptImageBackend>(ctx);
  }
}

bool shape_apply_3d(bContext *C,
                    wmOperator *op,
                    Object &ob,
                    const ShapeSpaceDesc &space_desc,
                    const Span<PaintShape> shapes,
                    ShapeStyle style)
{
  Scene *scene = CTX_data_scene(C);
  ToolSettings *toolsettings = scene ? scene->toolsettings : nullptr;
  if (shapes.is_empty() || toolsettings == nullptr || scene == nullptr) {
    return false;
  }
  /* An open shape draws only through its stroke; force it on so a Line / Polyline / open Curve
   * is never invisible (same rule the 2D compositor applies). */
  style = style_resolve_for_shapes(shapes, style);
  if (!material::paint_supported_on_object(*scene, ob)) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Paint Shape: the active object does not support material paint (Dyntopo or "
               "Multires)");
    return false;
  }

  const PaintModeSettings &mode_settings = toolsettings->paint_mode;
  const bool use_material_paint = mode_settings.canvas_source ==
                                  PAINT_CANVAS_SOURCE_MATERIAL_PAINT;
  const bool use_color_attribute = mode_settings.canvas_source ==
                                   PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE;
  const bool use_image = mode_settings.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL ||
                         mode_settings.canvas_source == PAINT_CANVAS_SOURCE_IMAGE;
  if (!use_material_paint && !use_color_attribute && !use_image) {
    BKE_report(op->reports, RPT_WARNING, "Paint Shape: unsupported paint canvas");
    return false;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Paint Shape: no dependency graph in this context");
    return false;
  }

  /* The 3D space captured at draw time; every backend below works from it. SurfaceAnchored
   * restores its anchor on the current evaluated surface first (the mesh may have been sculpted
   * or deformed since the draw), so F9 after a sculpt edit stays on the surface. A failure
   * means the surface cannot be reproduced (topology changed, UV missing or ambiguous): refuse
   * instead of baking the shape onto a stale anchor. */
  ShapeSpaceDesc resolved_space = space_desc;
  if (resolved_space.type == ShapeSpaceType::SurfaceAnchored &&
      (resolved_space.anchor.tri >= 0 || resolved_space.anchor.has_surface_uv))
  {
    if (!surface_anchor_restore(ob, *depsgraph, resolved_space.anchor)) {
      BKE_report(op->reports,
                 RPT_WARNING,
                 "Paint Shape: the surface the shape was anchored to cannot be restored (topology "
                 "changed or ambiguous UV); nothing was drawn");
      return false;
    }
  }
  const std::unique_ptr<ShapeSpace> space = shape_space_create(resolved_space);

  /* Sculpt Mode channels belong to the Sculpt paint (brush + visible channel set), exactly the
   * source #SCULPT_OT_color_gradient samples; the Image Editor uses the image-paint paint. */
  Paint &paint = toolsettings->sculpt != nullptr ? toolsettings->sculpt->paint :
                                                   toolsettings->imapaint.paint;

  /* Build the write backend for the canvas source and drive the shared lifecycle. */
  ViewLayer *view_layer = CTX_data_view_layer(C);
  if (view_layer == nullptr) {
    return false;
  }
  std::unique_ptr<ShapeTargetBackend> backend = shape_backend_create(
      ob,
      *scene,
      *view_layer,
      *space,
      *toolsettings,
      paint,
      BKE_paint_shape_settings_get(*toolsettings));

  if (!backend->begin(*C, op->reports)) {
    return false;
  }
  if (!backend->commit(C, op->reports, shapes, style, op->type->name)) {
    return false;
  }

  /* The owner region redraw is the caller's job (the dispatcher holds no ARegion); the notifier
   * reaches every view showing the object. */
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  return true;
}

}  // namespace blender::ed::sculpt_paint::shape
