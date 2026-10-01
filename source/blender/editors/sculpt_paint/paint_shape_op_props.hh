/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Serialization of a #PaintShape and its #ShapeStyle into operator properties, so `exec` replays
 * the modal apply and Python with the same data, and Redo (F9) replays the stored style.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "paint_shape.hh"

namespace blender {

struct wmOperator;
struct wmOperatorType;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Operator property serialization
 *
 * The shape is stored into operator properties so `exec` replays the modal apply and Python
 * with the same data, together with the style for Redo (F9).
 * \{ */

/** The reference tile of a 2D (Image Editor) shape, kept apart from the shape itself. */
void canvas_tile_to_op_props(wmOperator *op, const CanvasTile &tile);
CanvasTile canvas_tile_from_op_props(wmOperator *op);

void shape_to_op_props(wmOperator *op, const PaintShape &shape);
/** #std::nullopt when the properties hold no valid shape (e.g. a fresh repeat invocation). */
std::optional<PaintShape> shape_from_op_props(wmOperator *op);

/** Store \a style's appearance fields into the operator properties, so Redo (F9) and Python
 * replay the same look. */
void style_to_op_props(wmOperator *op, const ShapeStyle &style);
/**
 * Overlay the style fields stored by #style_to_op_props onto \a r_style. False when the
 * properties hold no style (a fresh invocation): the caller keeps the settings-built style.
 *
 * Limitation: the #PAINT_SHAPE_CHANNELS_OVERRIDE flag is stored with the style, but the
 * per-channel values are not — a redo re-reads them from the current settings and brush. That
 * matches the stored result as long as the brush is not changed between drawing and the redo.
 */
bool style_from_op_props(wmOperator *op, ShapeStyle &r_style);

/** Register the hidden serialization properties ("shape_*") on \a ot. */
void shape_op_properties_register(wmOperatorType *ot);

/** Register the style serialization properties on \a ot; the Redo-editable subset (colors,
 * width, opacities) stays visible in the redo panel. */
void style_op_properties_register(wmOperatorType *ot);

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
