/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D write backends of the shape drawing tools (Sculpt Mode PBR Paint). The shape is built in
 * region pixels by the frontend (`sculpt_paint_shape_ops.cc`); these backends project mesh
 * elements into that region, sample the shared #ShapeEvaluator and shade the channels with the
 * same `paint_shape_render.hh` math the Image Editor compositor uses. Nothing here allocates an
 * ImBuf or a second copy of the shape pipeline.
 */

#pragma once

#include <memory>

#include "BLI_span.hh"

#include "../paint_shape.hh"
#include "../paint_shape_space.hh"
#include "../paint_shape_target.hh"

namespace blender {

struct Depsgraph;
struct Object;
struct Paint;
struct ToolSettings;
struct ViewLayer;
struct bContext;
struct wmOperator;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

/**
 * Whether the paint mode's canvas source is an image canvas (Material channel maps or the Image
 * canvas) rather than a mesh attribute canvas. Only the image canvas has a live preview, so it is
 * the only one offered a Vector session.
 */
bool shape_canvas_is_image(const ToolSettings &toolsettings);

/** Whether the canvas source's write backend implements the live preview (all three canvases do:
 * image maps, Material Paint PBR vertex attributes, Color Attribute). Used to offer the Vector
 * session instead of a one-shot bake. */
bool shape_canvas_supports_preview(const ToolSettings &toolsettings);

/**
 * Build (but do not #begin) the write backend for \a ob's canvas source. \a space must outlive the
 * backend: the backend stores a pointer to it (the live Vector session owns it). \a settings
 * decides the target channel set and must outlive the backend too (the session's own copy, or the
 * shared tool settings block for a one-shot bake). The backend keeps \a view_layer, not a
 * #Depsgraph: the graph is looked up per call (see #Sculpt3DTargetContext::depsgraph_get).
 */
std::unique_ptr<ShapeTargetBackend> shape_backend_create(Object &ob,
                                                         Scene &scene,
                                                         ViewLayer &view_layer,
                                                         const ShapeSpace &space,
                                                         ToolSettings &toolsettings,
                                                         Paint &paint,
                                                         const PaintShapeSettings &settings);

/**
 * Apply \a shapes (defined in the 2D space of \a space, a #ShapeSpaceDesc captured at draw time)
 * to \a ob's paint data, according to the paint mode's canvas source:
 * - #PAINT_CANVAS_SOURCE_MATERIAL_PAINT: the enabled PBR channel attributes.
 * - #PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE: the active color attribute.
 * - #PAINT_CANVAS_SOURCE_MATERIAL: the PBR channel images through the PBVH pixels.
 * - #PAINT_CANVAS_SOURCE_IMAGE: the active image canvas through the PBVH pixels.
 *
 * The vertex-attribute bakes are one sculpt undo step; the image bakes are one image undo step.
 * \a style must be the snapshot of the tool settings (the backend applies
 * #style_channels_from_brush itself so the brush's channel values are the default source).
 * \a op is the drawing operator, used as the undo-step owner and the report target: it receives
 * a reason when nothing could be painted.
 *
 * \return true when at least one value was written.
 */
bool shape_apply_3d(bContext *C,
                    wmOperator *op,
                    Object &ob,
                    const ShapeSpaceDesc &space,
                    Span<PaintShape> shapes,
                    ShapeStyle style);

}  // namespace blender::ed::sculpt_paint::shape
