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
#include "paint_shape_space.hh"

namespace blender {

struct wmKeyConfig;
struct wmKeyMap;
struct wmOperator;
struct wmOperatorType;

/** Item ids of the "Image Paint Shape Modal" keymap (the name is kept for user keyconfigs). */
enum {
  PAINT_SHAPE_MODAL_CONFIRM = 1,
  PAINT_SHAPE_MODAL_CANCEL = 2,
  PAINT_SHAPE_MODAL_UNDO = 3,
  PAINT_SHAPE_MODAL_REDO = 4,
  PAINT_SHAPE_MODAL_MOVE = 5,
  PAINT_SHAPE_MODAL_ROTATE = 6,
  PAINT_SHAPE_MODAL_SCALE = 7,
  PAINT_SHAPE_MODAL_STROKE_WIDTH = 8,
  PAINT_SHAPE_MODAL_STROKE_OPACITY = 9,
  PAINT_SHAPE_MODAL_EXTRUDE = 10,
  PAINT_SHAPE_MODAL_SELECT_NEXT = 11,
  PAINT_SHAPE_MODAL_AXIS_X = 12,
  PAINT_SHAPE_MODAL_AXIS_Y = 13,
  /** No key assigned yet : the item exists so a user keyconfig can bind it. */
  PAINT_SHAPE_MODAL_ORIGIN_RESET = 14,
};
/* Defined in mesh/paint_image_shape_ops.cc; registered from
 * #ED_keymap_paint. */
wmKeyMap *paint_shape_modal_keymap(wmKeyConfig *keyconf);

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

/** Store the shape's space \a desc into the operator properties (hidden), so `exec` / Redo (F9)
 * rebuild the same frozen view. */
void space_to_op_props(wmOperator *op, const ShapeSpaceDesc &desc);
/** Read the space stored by #space_to_op_props. False when absent (a fresh invocation). */
bool space_from_op_props(wmOperator *op, ShapeSpaceDesc &r_desc);
/** Register the hidden space serialization properties on \a ot. */
void space_op_properties_register(wmOperatorType *ot);

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
