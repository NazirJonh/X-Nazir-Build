/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image-side helpers of the shape drawing tools: the canvas-space symmetry (UV space) and the UDIM tile <-> UV <-> reference-tile-pixel coordinate mapping. The pure
 * geometry of the shapes lives in `paint_shape.hh`.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "../paint_shape.hh"

namespace blender {

struct ToolSettings;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Canvas symmetry
 * \{ */

/** \a shape followed by its canvas-space symmetry copies (when enabled and affecting painting).
 * Copies are spline-based: a non-affine symmetry (circle inversion) densifies parametric shapes.
 * Empty input yields empty output. */
Vector<PaintShape> shape_apply_canvas_symmetry(const PaintShape &shape,
                                               const CanvasTile &tile,
                                               const ToolSettings &tool_settings);

/** Every shape in \a shapes followed by its canvas-space symmetry copies. Empty input yields
 * empty output. */
Vector<PaintShape> shapes_expand_symmetry(Span<PaintShape> shapes,
                                          const CanvasTile &tile,
                                          const ToolSettings &tool_settings);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coordinate helpers
 * \{ */

/** UV origin (bottom-left corner) of the UDIM tile \a tile. */
float2 tile_uv_origin(int tile);

/** Reference-tile pixels <-> UV. */
float2 shape_px_to_uv(const CanvasTile &tile, const float2 &px);
float2 shape_uv_to_px(const CanvasTile &tile, const float2 &uv);

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
