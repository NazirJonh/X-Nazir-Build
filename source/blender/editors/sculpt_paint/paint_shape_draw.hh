/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared GPU overlay of the shape drawing tools: the animated dashed outline and the control
 * point squares, common to the Image Editor frontends and the 3D viewport. The functions take
 * points already translated into region pixels; the coordinate mapping stays with the caller.
 */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_span.hh"

#include "BLI_math_vector_types.hh"

#include "paint_shape.hh"
#include "paint_vector_document.hh"

namespace blender::ed::sculpt_paint::shape {

/** Animated dashed outline through \a region_points (already in region pixels). */
void shape_draw_dashed_outline(Span<float2> region_points, bool loop);

/** A thin, muted solid outline: the seam siblings of a 3D shape's contour in the Image Editor
 * (the cage part keeps the animated dashed style). */
void shape_draw_plain_outline(Span<float2> region_points, bool loop);

/** Control-point squares for \a region_cos (already in region pixels): a dark border and a
 * light core per point, drawn in one batch. */
void shape_draw_control_points(Span<float2> region_cos);

/** Diamond markers for shape origins at \a region_cos (already in region pixels). */
void shape_draw_origin_markers(Span<float2> region_cos);

/**
 * The live Vector session overlay: the dashed outline of every \a item, then the control points
 * and the origin marker of the active item. \a to_region maps a shape-space pixel (of the given
 * shape) to a region pixel; the caller owns that mapping, so the same code serves the Image
 * Editor and the 3D viewport.
 */
void shape_draw_session_overlay(
    Span<VectorItem> items,
    int active_index,
    FunctionRef<float2(const PaintShape &, const float2 &)> to_region);

}  // namespace blender::ed::sculpt_paint::shape
