/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Weight paint brush operations for Curves (Draw, Blur, Average, Smear).
 *
 * All brushes share the common stroke machinery in #CurvesPaintOperationBase /
 * #CurvesWeightPaintOperationBase. Each brush only implements its specific behavior by
 * overriding #apply_brush() (and, for the Draw brush, by relying on the default per-point
 * #apply_operation_to_point()).
 */

#include "BKE_curves.hh"
#include "BKE_curves_weight_paint.hh"

#include "DNA_brush_types.h"

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"

#include "curves_weight_paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Draw Weight Paint Operation
 *
 * Blends curve point weights towards the brush weight.
 *
 * The actual per-point math lives in #CurvesWeightPaintOperationBase::apply_operation_to_point()
 * (which already accounts for the add/subtract direction and the Invert stroke mode), so the Draw
 * brush only needs to remember the stroke mode and let the shared machinery do the rest.
 * \{ */

class DrawWeightPaintOperation : public CurvesWeightPaintOperationBase {
 public:
  DrawWeightPaintOperation(const BrushStrokeMode stroke_mode)
  {
    this->stroke_mode = stroke_mode;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blur Weight Paint Operation
 *
 * Smooths weights by blending every point under the brush towards the average weight of its
 * neighbors along the curve.
 * \{ */

class BlurWeightPaintOperation : public CurvesWeightPaintOperationBase {
 public:
  BlurWeightPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

 protected:
  void apply_brush(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/) override
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    const OffsetIndices<int> points_by_curve = curves->points_by_curve();
    const Array<int> point_to_curve = curves->point_to_curve_map();
    const VArray<bool> cyclic = curves->cyclic();

    /* Compute all target weights first, then write them, so that the blur of one point does not
     * feed back into the blur of an adjacent point in the same pass. */
    Array<float> new_weights(points_in_brush.size());

    for (const int i : points_in_brush.index_range()) {
      const BrushPoint &point = points_in_brush[i];
      const int point_index = point.point_index;
      const float old_weight = get_vertex_weight(point_index);
      new_weights[i] = old_weight;
      if (old_weight < 0.0f) {
        continue;
      }

      float neighbor_sum = 0.0f;
      int neighbor_count = 0;
      foreach_curve_neighbors(
          point_index, points_by_curve, point_to_curve, cyclic, [&](const int nb) {
            neighbor_sum += math::max(0.0f, get_vertex_weight(nb));
            neighbor_count++;
          });
      if (neighbor_count == 0) {
        continue;
      }

      const float target_weight = neighbor_sum / float(neighbor_count);
      new_weights[i] = math::clamp(
          old_weight + math::interpolate(0.0f, target_weight - old_weight, point.influence),
          0.0f,
          1.0f);
    }

    for (const int i : points_in_brush.index_range()) {
      set_vertex_weight(points_in_brush[i].point_index, new_weights[i]);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Average Weight Paint Operation
 *
 * Blends every point under the brush towards the (influence weighted) average weight of all points
 * under the brush.
 * \{ */

class AverageWeightPaintOperation : public CurvesWeightPaintOperationBase {
 public:
  AverageWeightPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

 protected:
  void apply_brush(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/) override
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    /* Compute the influence-weighted average of all points under the brush. */
    float weight_sum = 0.0f;
    float influence_sum = 0.0f;
    for (const BrushPoint &point : points_in_brush) {
      const float weight = get_vertex_weight(point.point_index);
      if (weight < 0.0f) {
        continue;
      }
      weight_sum += weight * point.influence;
      influence_sum += point.influence;
    }

    if (influence_sum < 1e-6f) {
      return;
    }
    const float average_weight = weight_sum / influence_sum;

    /* Blend each point towards the average. The brush strength is already folded into the
     * point influence during sampling. */
    for (const BrushPoint &point : points_in_brush) {
      const float old_weight = get_vertex_weight(point.point_index);
      if (old_weight < 0.0f) {
        continue;
      }
      const float new_weight = math::clamp(
          old_weight + math::interpolate(0.0f, average_weight - old_weight, point.influence),
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
 * Drags weights along with the brush movement by blending each point towards the weight it had on
 * the previous stroke sample.
 * \{ */

class SmearWeightPaintOperation : public CurvesWeightPaintOperationBase {
 private:
  /* Weights from the previous stroke sample, indexed by curve point. */
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

    /* Seed the previous-sample weights with the actual point weights so that points entering the
     * brush later are not smeared towards zero. */
    if (curves) {
      const int points_num = curves->points_num();
      previous_weights_.reinitialize(points_num);
      for (const int point_i : IndexRange(points_num)) {
        previous_weights_[point_i] = math::max(0.0f, get_vertex_weight(point_i));
      }
    }
    has_previous_sample_ = false;
  }

  void on_stroke_done(const bContext &C) override
  {
    CurvesWeightPaintOperationBase::on_stroke_done(C);
    previous_weights_ = Array<float>();
    has_previous_sample_ = false;
  }

 protected:
  void apply_brush(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/) override
  {
    /* Make sure the cache matches the current geometry (guards against a failed stroke begin). */
    if (previous_weights_.size() != curves->points_num()) {
      previous_weights_.reinitialize(curves->points_num());
      previous_weights_.fill(0.0f);
      has_previous_sample_ = false;
    }

    if (!points_in_brush.is_empty() && has_previous_sample_) {
      apply_smear();
    }

    /* Cache the resulting weights for the next stroke sample. */
    for (const BrushPoint &point : points_in_brush) {
      previous_weights_[point.point_index] = math::max(0.0f, get_vertex_weight(point.point_index));
    }
    has_previous_sample_ = true;
  }

 private:
  void apply_smear()
  {
    /* Smear strength is proportional to how far the brush moved this sample. */
    const float2 brush_direction = mouse_position - mouse_position_previous;
    const float brush_movement = math::length(brush_direction);
    if (brush_movement < 1e-6f || brush_radius < 1e-6f) {
      return;
    }
    const float smear_factor = math::min(1.0f, brush_movement / brush_radius);

    for (const BrushPoint &point : points_in_brush) {
      const float old_weight = get_vertex_weight(point.point_index);
      if (old_weight < 0.0f) {
        continue;
      }
      const float prev_weight = previous_weights_[point.point_index];

      /* Blend the current weight towards the previous-sample weight, scaled by brush movement and
       * the point influence (which already includes the brush strength). */
      const float smeared_weight = math::interpolate(old_weight, prev_weight, smear_factor);
      const float new_weight = math::clamp(
          math::interpolate(old_weight, smeared_weight, point.influence), 0.0f, 1.0f);

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
