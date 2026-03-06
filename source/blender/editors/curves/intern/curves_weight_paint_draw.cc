/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Draw weight paint operation for Curves.
 * Similar to Grease Pencil's DrawWeightPaintOperation but adapted for Curves objects.
 */

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_weight_paint.hh"
#include "BKE_paint.hh"
#include "BKE_object_deform.h"
#include "BKE_deform.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"
#include "ED_object_vgroup.hh"

#include "DEG_depsgraph.hh"

#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"

#include "WM_api.hh"

#include "BLI_task.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "curves_weight_paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Draw Weight Paint Operation
 *
 * Paints weight values directly onto curve points under the brush.
 * Similar to the standard mesh weight paint draw brush.
 * \{ */

class DrawWeightPaintOperation : public CurvesWeightPaintOperationBase {
 public:
  DrawWeightPaintOperation(const BrushStrokeMode stroke_mode)
  {
    this->stroke_mode = stroke_mode;
  }

  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override
  {
    /* Call base class implementation. */
    CurvesWeightPaintOperationBase::on_stroke_begin(C, start_extension);

    /* Get the add/subtract mode of the draw brush. */
    invert_brush_weight = (brush->flag & BRUSH_DIR_IN) != 0;
    if (stroke_mode == BrushStrokeMode::Invert) {
      invert_brush_weight = !invert_brush_weight;
    }
  }

  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override
  {
    /* Update brush settings for this stroke sample. */
    get_brush_settings(C, stroke_extension);

    if (!object || !curves_id || !curves || !brush) {
      static int debug_invalid_context_count = 0;
      debug_invalid_context_count++;
      if (debug_invalid_context_count <= 20 || (debug_invalid_context_count % 100) == 0) {
        printf("[DEBUG] CurvesWeightPaint draw::on_stroke_extended: invalid context "
               "(object=%p curves_id=%p curves=%p brush=%p)\n",
               object,
               curves_id,
               curves,
               brush);
      }
      return;
    }

    /* Update mouse input and bounding box. */
    get_mouse_input(stroke_extension);

    if (!curves || curves->is_empty()) {
      return;
    }

    /* Sample points under the brush. */
    sample_curves_3d_brush(C, stroke_extension);

    /* Apply draw operation to all points under the brush. */
    apply_draw_operation(C, stroke_extension);

    /* Notify about geometry changes. */
    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
  }

  void on_stroke_done(const bContext &C) override
  {
    /* Call base class implementation. */
    CurvesWeightPaintOperationBase::on_stroke_done(C);
  }

 private:
  /**
   * Apply draw weight to all points collected in points_in_brush.
   */
  void apply_draw_operation(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/)
  {
    int changed_points = 0;
    int invalid_points = 0;

    for (const BrushPoint &point : points_in_brush) {
      apply_draw_weight(point, changed_points, invalid_points);
    }

    if (invalid_points > 0) {
      printf("[DEBUG] CurvesWeightPaint draw::apply_draw_operation: points_in_brush=%d changed=%d "
             "invalid=%d\n",
             int(points_in_brush.size()),
             changed_points,
             invalid_points);
    }
  }

  /**
   * Apply weight to a single brush point.
   * Uses brush weight as target and influence for blending.
   */
  void apply_draw_weight(const BrushPoint &point,
                         int &r_changed_points,
                         int &r_invalid_points)
  {
    const float old_weight = get_vertex_weight(point.point_index);

    if (old_weight < 0.0f) {
      r_invalid_points++;
      return;
    }

    /* Calculate effective target weight based on invert mode. */
    const float effective_target = invert_brush_weight ? (1.0f - brush_weight) : brush_weight;

    /* Calculate weight delta. */
    const float weight_delta = effective_target - old_weight;

    /* Blend current weight towards target using influence. */
    const float new_weight = math::clamp(
        old_weight + math::interpolate(0.0f, weight_delta, point.influence), 0.0f, 1.0f);

    if (new_weight == old_weight) {
      return;
    }

    set_vertex_weight(point.point_index, new_weight);
    r_changed_points++;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blur Weight Paint Operation
 *
 * Blurs weight values by averaging nearby points.
 * \{ */

class BlurWeightPaintOperation : public CurvesWeightPaintOperationBase {
 private:
  static constexpr int BLUR_ITERATIONS = 1;

 public:
  BlurWeightPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override
  {
    CurvesWeightPaintOperationBase::on_stroke_begin(C, start_extension);
  }

  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override
  {
    get_brush_settings(C, stroke_extension);
    get_mouse_input(stroke_extension, 1.5f); /* Wider brush for neighbor sampling. */

    if (!curves || curves->is_empty()) {
      return;
    }

    sample_curves_3d_brush(C, stroke_extension);
    apply_blur_operation(C);

    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
  }

  void on_stroke_done(const bContext &C) override
  {
    CurvesWeightPaintOperationBase::on_stroke_done(C);
  }

 private:
  void apply_blur_operation(const bContext & /*C*/)
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    /* Calculate average weight of all points under the brush. */
    float weight_sum = 0.0f;
    for (const BrushPoint &point : points_in_brush) {
      weight_sum += get_vertex_weight(point.point_index);
    }
    const float average_weight = weight_sum / float(points_in_brush.size());

    /* Apply blurred (averaged) weight to all points. */
    for (const BrushPoint &point : points_in_brush) {
      const float old_weight = get_vertex_weight(point.point_index);
      const float weight_delta = average_weight - old_weight;
      const float new_weight = math::clamp(
          old_weight + math::interpolate(0.0f, weight_delta, point.influence), 0.0f, 1.0f);
      set_vertex_weight(point.point_index, new_weight);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Average Weight Paint Operation
 *
 * Averages weight values of all points under the brush.
 * \{ */

class AverageWeightPaintOperation : public CurvesWeightPaintOperationBase {
 public:
  AverageWeightPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override
  {
    CurvesWeightPaintOperationBase::on_stroke_begin(C, start_extension);
  }

  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override
  {
    get_brush_settings(C, stroke_extension);
    get_mouse_input(stroke_extension);

    if (!curves || curves->is_empty()) {
      return;
    }

    sample_curves_3d_brush(C, stroke_extension);
    apply_average_operation(C);

    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
  }

  void on_stroke_done(const bContext &C) override
  {
    CurvesWeightPaintOperationBase::on_stroke_done(C);
  }

 private:
  void apply_average_operation(const bContext & /*C*/)
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    /* Calculate weighted average based on influence. */
    float weight_sum = 0.0f;
    float influence_sum = 0.0f;

    for (const BrushPoint &point : points_in_brush) {
      const float weight = get_vertex_weight(point.point_index);
      weight_sum += weight * point.influence;
      influence_sum += point.influence;
    }

    if (influence_sum < 1e-6f) {
      return;
    }

    const float average_weight = weight_sum / influence_sum;

    /* Apply averaged weight to all points. */
    for (const BrushPoint &point : points_in_brush) {
      const float old_weight = get_vertex_weight(point.point_index);
      const float weight_delta = average_weight - old_weight;
      const float new_weight = math::clamp(
          old_weight + math::interpolate(0.0f, weight_delta, point.influence * brush_strength),
          0.0f,
          1.0f);
      set_vertex_weight(point.point_index, new_weight);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smear Weight Paint Operation
 *
 * Smears weight values in the direction of brush movement.
 * \{ */

class SmearWeightPaintOperation : public CurvesWeightPaintOperationBase {
 private:
  /* Cached weights from the previous stroke sample for smearing. */
  Array<float> previous_weights_;
  bool has_previous_sample_ = false;

 public:
  SmearWeightPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override
  {
    CurvesWeightPaintOperationBase::on_stroke_begin(C, start_extension);

    /* Initialize previous weights array. */
    if (curves) {
      previous_weights_.reinitialize(curves->points_num());
      previous_weights_.fill(0.0f);
    }
    has_previous_sample_ = false;
  }

  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override
  {
    get_brush_settings(C, stroke_extension);
    get_mouse_input(stroke_extension);

    if (!curves || curves->is_empty()) {
      return;
    }

    sample_curves_3d_brush(C, stroke_extension);

    if (has_previous_sample_) {
      apply_smear_operation(C);
    }

    /* Cache current weights for next sample. */
    cache_current_weights();
    has_previous_sample_ = true;

    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
  }

  void on_stroke_done(const bContext &C) override
  {
    CurvesWeightPaintOperationBase::on_stroke_done(C);
    previous_weights_ = Array<float>();
    has_previous_sample_ = false;
  }

 private:
  void cache_current_weights()
  {
    for (const BrushPoint &point : points_in_brush) {
      previous_weights_[point.point_index] = get_vertex_weight(point.point_index);
    }
  }

  void apply_smear_operation(const bContext & /*C*/)
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    /* Calculate brush movement direction. */
    const float2 brush_direction = mouse_position - mouse_position_previous;
    const float brush_movement = math::length(brush_direction);

    if (brush_movement < 1e-6f) {
      return;
    }

    /* Apply smeared weights. */
    for (const BrushPoint &point : points_in_brush) {
      const float old_weight = get_vertex_weight(point.point_index);
      const float prev_weight = previous_weights_[point.point_index];

      /* Smear factor based on brush movement. */
      const float smear_factor = math::min(1.0f, brush_movement / brush_radius);

      /* Blend between current weight and previous cached weight. */
      const float smeared_weight = math::interpolate(old_weight, prev_weight, smear_factor);
      const float new_weight = math::clamp(
          math::interpolate(old_weight, smeared_weight, point.influence * brush_strength),
          0.0f,
          1.0f);

      set_vertex_weight(point.point_index, new_weight);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Factory Functions
 * \{ */

std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_draw_operation(
    const BrushStrokeMode &stroke_mode)
{
  return std::make_unique<DrawWeightPaintOperation>(stroke_mode);
}

std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_blur_operation()
{
  return std::make_unique<BlurWeightPaintOperation>();
}

std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_average_operation()
{
  return std::make_unique<AverageWeightPaintOperation>();
}

std::unique_ptr<CurvesWeightPaintStrokeOperation> new_weight_paint_smear_operation()
{
  return std::make_unique<SmearWeightPaintOperation>();
}

/** \} */

}  // namespace blender::ed::sculpt_paint
