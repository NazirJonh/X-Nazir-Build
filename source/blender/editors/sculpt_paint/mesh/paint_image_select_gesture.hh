/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared pipeline for the gesture-driven Image Paint selection operators
 * (box / lasso / circle).
 *
 * Every gesture operator runs the exact same sequence: simple-click detection, committing a
 * floating move-selection fragment, opening an image undo step, seeding UV-island expansion,
 * clearing the mask in SET mode, rasterizing into each intersecting UDIM tile, expanding islands
 * again for ADD/SET, and finally tagging updates and closing the undo step.
 *
 * Only two things differ per tool: how the gesture's UV bounds are derived, and how its pixels are
 * rasterized into one tile. #ImageSelectGestureShape captures exactly those two decisions (plus
 * the edge policy the tool wants), so that adding a new selection shape -- polyline, magic wand,
 * select-by-color -- costs roughly 40 lines of shape implementation instead of ~120 lines of
 * copy-pasted operator body.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "DNA_windowmanager_enums.h"

#include "ED_select_utils.hh"

struct ARegion;
struct ImBuf;
struct Image;
struct bContext;
struct rctf;
struct wmEvent;
struct wmOperator;
struct wmOperatorType;

namespace blender {

/* Defined in BKE_image.hh, which this header deliberately does not pull in. */
struct PaintSelectionEdgePolicy;

/**
 * Per-tool part of a gesture selection: the gesture's UV extent and its rasterization.
 *
 * Instances are short-lived; #image_select_gesture_exec_generic constructs nothing and stores
 * nothing, it only calls back into the shape while executing the shared sequence.
 */
class ImageSelectGestureShape {
 public:
  virtual ~ImageSelectGestureShape() = default;

  /** Label for the image undo step this gesture opens, e.g. `"Box Select"`. */
  virtual const char *undo_name() const = 0;

  /**
   * Derive the gesture's bounding box in UV space from the operator properties.
   *
   * Shapes that need to cache intermediate data for #rasterize_tile (lasso points, circle center
   * and radii) compute it here and keep it in their own members.
   *
   * \return false when the gesture is degenerate (zero-sized box, fewer than three lasso points,
   * non-positive circle radius). The caller then treats the gesture as a plain deselect, exactly
   * as a simple click does.
   */
  virtual bool uv_bounds_calc(const ARegion *region, wmOperator *op, rctf &r_uv_bounds) = 0;

  /**
   * Write \a fill_value into every mask pixel this shape covers within one UDIM tile.
   *
   * \param uv_origin: bottom-left UV corner of the tile.
   * \param tile_uv_rect: intersection of the tile with the gesture's UV bounds. Only shapes whose
   * coverage is exactly that rectangle (the box) use it; others rasterize from their own cached
   * geometry and ignore it.
   */
  virtual void rasterize_tile(const float2 &uv_origin,
                              const rctf &tile_uv_rect,
                              ImBuf *mask,
                              float fill_value) const = 0;

  /** Edge policy applied to the resulting mask once the gesture completes. */
  virtual PaintSelectionEdgePolicy edge_policy() const = 0;
};

/**
 * Run the complete shared gesture-selection sequence for \a shape.
 *
 * This is the single implementation behind `PAINT_OT_image_select_box`, `_lasso` and `_circle`;
 * their `exec` callbacks only build a shape and forward here.
 */
wmOperatorStatus image_select_gesture_exec_generic(bContext *C,
                                                   wmOperator *op,
                                                   ImageSelectGestureShape &shape);

/**
 * Common gesture invoke: hand off to the move operator if the cursor is over a floating fragment,
 * otherwise record the press location and assume a simple click until the cursor moves far enough.
 * Returns true when the event was delegated (the caller should return #OPERATOR_FINISHED).
 */
bool image_select_gesture_invoke_begin(bContext *C, wmOperator *op, const wmEvent *event);

/** Clear the simple-click flag once the cursor has travelled past the drag threshold. */
void image_select_gesture_drag_detect(wmOperator *op, const wmEvent *event);

/** Register the simple-click bookkeeping properties shared by every gesture operator. */
void image_select_gesture_properties(wmOperatorType *ot);

/**
 * Grow the selection to the full UV islands touched by \a gesture_uv_bounds.
 *
 * Implemented in `paint_image_select_mask.cc` next to the BMesh/UV machinery it needs; declared
 * here because the shared gesture sequence drives it.
 */
void image_paint_selection_expand_uv_islands(bContext *C,
                                             Image *image,
                                             eSelectOp sel_op,
                                             const rctf *gesture_uv_bounds);

}  // namespace blender
