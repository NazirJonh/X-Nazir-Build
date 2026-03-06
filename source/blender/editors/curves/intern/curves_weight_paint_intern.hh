/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

#include "BLI_vector.hh"
#include "BLI_set.hh"
#include "BLI_rect.h"
#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"

#include "../../sculpt_paint/curves/sculpt_intern.hh"

struct bContext;
struct Depsgraph;
struct Object;
struct Brush;
struct Scene;
struct ReportList;
struct bDeformGroup;

namespace blender::ed::sculpt_paint {

using bke::CurvesGeometry;

/**
 * Brush point data structure for weight paint operations.
 * Contains influence factor and point index for weight application.
 */
struct BrushPoint {
  /** Influence factor based on brush falloff curve (0.0 - 1.0). */
  float influence;
  /** Index of the curve point in the geometry. */
  int point_index;
};

/**
 * Base class for stroke based operations in curves weight paint mode.
 * Similar to GreasePencilStrokeOperation but for Curves objects.
 */
class CurvesWeightPaintStrokeOperation {
 public:
  virtual ~CurvesWeightPaintStrokeOperation() = default;
  
  /**
   * Called at the beginning of a stroke.
   * Initialize brush settings, ensure vertex groups exist, etc.
   */
  virtual void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) = 0;
  
  /**
   * Called for each stroke sample during the stroke.
   * Apply weight paint to points under the brush.
   */
  virtual void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) = 0;
  
  /**
   * Called when the stroke is finished.
   * Perform cleanup and final operations.
   */
  virtual void on_stroke_done(const bContext &C) = 0;
};

/**
 * Base class for curves weight paint operations with common utilities.
 * Provides shared functionality for all weight paint brush types (Draw, Blur, Average, Smear).
 */
class CurvesWeightPaintOperationBase : public CurvesWeightPaintStrokeOperation {
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
  float brush_weight = 0.0f;
  
  /* ----- Mouse/stroke state ----- */
  float2 mouse_position;
  float2 mouse_position_previous;
  rctf brush_bbox;
  
  /* ----- Weight paint settings ----- */
  bool auto_normalize = false;
  BrushStrokeMode stroke_mode = BrushStrokeMode::Normal;
  bool invert_brush_weight = false;
  int active_vertex_group = -1;
  bDeformGroup *object_defgroup = nullptr;
  
  /* ----- Locked vertex groups (object level) ----- */
  Set<std::string> object_locked_defgroups;
  
  /* ----- Collected points under brush ----- */
  Vector<BrushPoint> points_in_brush;

 public:
  /* ----- Virtual interface with default implementations ----- */
  
  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override;
  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override;
  void on_stroke_done(const bContext &C) override;

  /* ----- Utility methods ----- */
  
  /**
   * Get brush settings from context and stroke extension.
   * Updates radius, strength, weight based on pressure if enabled.
   */
  void get_brush_settings(const bContext &C, const StrokeExtension &stroke_extension);
  
  /**
   * Ensure active vertex group exists in the object.
   * Creates a default group if none exists.
   */
  void ensure_active_vertex_group_in_object();
  
  /**
   * Get list of locked vertex groups from the object.
   */
  void get_locked_vertex_groups();
  
  /**
   * Apply weight to a specific point with given influence.
   * @param point_index Index of the curve point
   * @param target_weight Target weight value (0.0 - 1.0)
   * @param influence Brush influence factor (0.0 - 1.0)
   */
  void apply_weight_to_point(int point_index, float target_weight, float influence);

 protected:
  /**
   * Get current weight of a point in the active vertex group.
   * @return Weight value (0.0 - 1.0), or 0.0 if not in group
   */
  float get_vertex_weight(int point_index);
  
  /**
   * Set weight of a point in the active vertex group.
   * @param weight Weight value (0.0 - 1.0)
   */
  void set_vertex_weight(int point_index, float weight);
  
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
   * Check if a point is within the brush radius.
   * @param point_position_re Screen-space position of the point
   * @return true if point is under the brush
   */
  bool is_point_in_brush(const float2 &point_position_re);
  
  /**
   * Calculate brush falloff for a point based on distance.
   * @param distance_re Screen-space distance from brush center
   * @return Falloff factor (0.0 - 1.0)
   */
  float calculate_brush_falloff(float distance_re);
};

/**
 * Factory functions for different weight paint operations.
 * Create appropriate operation based on brush type.
 */

/** Create a Draw weight paint operation. */
std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_draw_operation(
    const BrushStrokeMode &stroke_mode);

/** Create a Blur weight paint operation. */
std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_blur_operation();

/** Create an Average weight paint operation. */
std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_average_operation();

/** Create a Smear weight paint operation. */
std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_smear_operation();

/**
 * Common context for curves weight paint operations.
 * Provides quick access to frequently needed data from bContext.
 */
class CurvesWeightPaintCommonContext {
 public:
  const Depsgraph *depsgraph = nullptr;
  Scene *scene = nullptr;
  Object *object = nullptr;
  CurvesGeometry *curves = nullptr;

  CurvesWeightPaintCommonContext(const bContext &C);
};

/**
 * Poll functions for weight paint mode operators.
 */

/** Check if curves weight paint mode is active. */
bool curves_weight_paint_poll(bContext *C);

/** Check if curves weight paint mode is active in 3D view. */
bool curves_weight_paint_poll_view3d(bContext *C);

/** Check if object is in curves weight paint mode. */
bool curves_weight_paint_mode_poll(bContext *C);

/**
 * Operator registration function.
 * Called from ED_operatortypes_curves() to register all weight paint operators.
 */
void ED_operatortypes_curves_weight_paint();

}  // namespace blender::ed::sculpt_paint

extern "C" {
void ED_operatortypes_curves_weight_paint();
}
