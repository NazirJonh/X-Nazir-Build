/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

#include "BLI_vector.hh"
#include "BLI_set.hh"
#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"

#include "curves_paint_intern.hh"

struct bContext;
struct Depsgraph;
struct Object;
struct Brush;
struct Scene;
struct ReportList;
struct bDeformGroup;

namespace blender::ed::sculpt_paint {

using bke::CurvesGeometry;

/* For backward compatibility, alias BrushPoint to CurvesBrushPoint. */
using BrushPoint = CurvesBrushPoint;

/* For backward compatibility, alias the stroke operation interface. */
using CurvesWeightPaintStrokeOperation = CurvesPaintStrokeOperation;

/**
 * Base class for curves weight paint operations with common weight-specific utilities.
 * Inherits common paint functionality from CurvesPaintOperationBase.
 * Provides weight-specific functionality for all weight paint brush types (Draw, Blur, Average, Smear).
 */
class CurvesWeightPaintOperationBase : public CurvesPaintOperationBase {
 protected:
  /* ----- Weight paint specific settings ----- */
  float brush_weight = 0.0f;
  bool auto_normalize = false;
  bool invert_brush_weight = false;
  int active_vertex_group = -1;
  bDeformGroup *object_defgroup = nullptr;

  /* ----- Locked vertex groups (object level) ----- */
  Set<std::string> object_locked_defgroups;

 public:
  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override;
  void on_stroke_done(const bContext &C) override;

  /* ----- Weight paint specific utilities ----- */

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
  /* ----- Weight access utilities ----- */

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
   * Extend the shared brush settings with weight-specific settings
   * (brush weight, auto-normalize, add/subtract direction).
   */
  void get_brush_settings(const bContext &C, const StrokeExtension &stroke_extension) override;

  /**
   * Default per-point weight paint behavior: blend the point weight towards the brush weight.
   * Used by the Draw brush through the base apply_brush() implementation.
   */
  void apply_operation_to_point(const CurvesBrushPoint &point) override;

  /**
   * Initialize weight paint mode (vertex group setup).
   */
  void init_paint_mode(const bContext & /*C*/) override;
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
