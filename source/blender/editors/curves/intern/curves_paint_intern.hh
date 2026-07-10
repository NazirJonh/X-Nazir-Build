/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

#include "BLI_function_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"
#include "BLI_rect.h"
#include "BKE_context.hh"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"

#include "WM_types.hh"

#include "../../sculpt_paint/curves/sculpt_intern.hh"

struct bContext;
struct Depsgraph;
struct Brush;
struct wmOperator;
struct wmEvent;

namespace blender::ed::sculpt_paint {

using bke::CurvesGeometry;

/**
 * Brush point data structure for paint operations.
 * Contains influence factor and point index for paint application.
 */
struct CurvesBrushPoint {
  /** Influence factor based on brush falloff curve (0.0 - 1.0). */
  float influence;
  /** Index of the curve point in the geometry. */
  int point_index;
};

/**
 * Base interface for stroke based operations in curves paint mode.
 * Used by Weight Paint, Vertex Paint, and other paint operations on Curves objects.
 */
class CurvesPaintStrokeOperation {
 public:
  virtual ~CurvesPaintStrokeOperation() = default;

  /**
   * Called at the beginning of a stroke.
   * Initialize brush settings and mode-specific data.
   */
  virtual void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) = 0;

  /**
   * Called for each stroke sample during the stroke.
   * Apply paint operation to points under the brush.
   */
  virtual void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) = 0;

  /**
   * Called when the stroke is finished.
   * Perform cleanup and final operations.
   */
  virtual void on_stroke_done(const bContext &C) = 0;
};

/**
 * Base class for curves paint operations with common utilities.
 * Provides shared functionality for all paint brush types (Draw, Blur, Average, Smear).
 * Subclasses must implement apply_operation_to_point() for specific paint behavior.
 */
class CurvesPaintOperationBase : public CurvesPaintStrokeOperation {
 protected:
  /* ----- Object and brush data ----- */
  Object *object = nullptr;
  Curves *curves_id = nullptr;
  Brush *brush = nullptr;
  bke::CurvesGeometry *curves = nullptr;

  /* ----- Brush parameters ----- */
  float initial_brush_radius = 0.0f;
  float brush_radius = 0.0f;
  float initial_brush_strength = 0.0f;
  float brush_strength = 0.0f;

  /* ----- Mouse/stroke state ----- */
  float2 mouse_position;
  float2 mouse_position_previous;
  rctf brush_bbox;

  /* ----- Paint mode settings ----- */
  BrushStrokeMode stroke_mode = BrushStrokeMode::Normal;
  bool invert_brush = false;

  /* ----- Collected points under brush ----- */
  Vector<CurvesBrushPoint> points_in_brush;

 public:
  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override;
  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override;
  void on_stroke_done(const bContext &C) override;

 protected:
  /* ----- Utility methods ----- */

  /**
   * Get brush settings from context and stroke extension.
   * Updates radius, strength based on pressure if enabled.
   * Virtual so paint modes (weight, vertex) can extend it with mode-specific settings.
   */
  virtual void get_brush_settings(const bContext &C, const StrokeExtension &stroke_extension);

  /**
   * Update mouse input and calculate brush bounding box.
   * @param stroke_extension Current stroke sample
   * @param brush_widen_factor Factor to widen brush for neighbor sampling (default 1.0)
   */
  void get_mouse_input(const StrokeExtension &stroke_extension, float brush_widen_factor = 1.0f);

  /**
   * Sample curve points under the brush and add them to points_in_brush buffer.
   * Uses screen-space projection for point-to-brush distance calculation.
   */
  void sample_curves_3d_brush(const bContext &C, const StrokeExtension &stroke_extension);

  /**
   * Factor used to widen the sampling brush relative to the visible brush radius.
   * Some brushes (e.g. Blur) need to look at a slightly larger neighborhood.
   * Defaults to 1.0 (no widening).
   */
  virtual float brush_widen_factor() const
  {
    return 1.0f;
  }

  /**
   * Calls \a fn for the index of each direct curve neighbor of \a point_index.
   * Accounts for cyclic curves. Visits 0, 1, or 2 neighbors.
   */
  static void foreach_curve_neighbors(int point_index,
                                      const OffsetIndices<int> &points_by_curve,
                                      Span<int> point_to_curve,
                                      const VArray<bool> &cyclic,
                                      FunctionRef<void(int)> fn);

  /**
   * Check if a point is within the brush radius.
   * @param point_position_re Screen-space position of the point
   * @return true if point is under the brush
   */
  bool is_point_in_brush(const float2 &point_position_re) const;

  /**
   * Calculate brush falloff for a point based on distance.
   * @param distance_re Screen-space distance from brush center
   * @return Falloff factor (0.0 - 1.0)
   */
  float calculate_brush_falloff(float distance_re) const;

  /* ----- Virtual methods for specific paint behavior ----- */

  /**
   * Apply the brush to all points collected in points_in_brush.
   *
   * This is the main extension point for brush behavior. The default implementation
   * applies the operation independently to every point under the brush (suitable for
   * the Draw brush). Brushes that need to consider the whole set of points at once
   * (Blur, Average, Smear) override this method instead.
   */
  virtual void apply_brush(const bContext &C, const StrokeExtension &stroke_extension);

  /**
   * Apply the paint operation to a single point.
   * Used by the default apply_brush() implementation. Overridden by subclasses to
   * implement per-point paint behavior (weight, color, etc).
   * @param point Brush point data with influence factor
   */
  virtual void apply_operation_to_point(const CurvesBrushPoint & /*point*/) {}

  /**
   * Initialize paint mode before the stroke begins.
   * Override in subclasses if mode-specific setup is needed.
   */
  virtual void init_paint_mode(const bContext & /*C*/) {}

  /**
   * Finalize paint mode after the stroke ends.
   * Override in subclasses if mode-specific cleanup is needed.
   */
  virtual void finalize_paint_mode(const bContext & /*C*/) {}
};

/* -------------------------------------------------------------------- */
/** \name Shared Paint Operator Helpers
 * \{ */

/**
 * Resolve the original (non-evaluated) active object from context.
 * Returns nullptr when there is no active object.
 */
Object *curves_paint_original_object_get(const bContext *C);

/**
 * Creates the paint operation for the active brush of a specific curves paint mode.
 * Returns nullptr when no operation could be created (e.g. no brush).
 */
using CurvesPaintStartStrokeFn = std::unique_ptr<CurvesPaintStrokeOperation> (*)(
    BrushStrokeMode brush_mode, BrushSwitchMode brush_switch_mode, const bContext &C);

/**
 * Shared PaintStroke operator callbacks for the curves paint modes. The stroke object is created
 * lazily through \a start_fn, which is the only part that differs between paint modes; modal and
 * cancel are mode independent.
 */
wmOperatorStatus curves_paint_brush_stroke_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event,
                                                  CurvesPaintStartStrokeFn start_fn);
wmOperatorStatus curves_paint_brush_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event);
void curves_paint_brush_stroke_cancel(bContext *C, wmOperator *op);

/** \} */

}  // namespace blender::ed::sculpt_paint
