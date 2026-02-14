/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <limits>

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

#include "DNA_brush_types.h"

#include "WM_api.hh"

#include "BLI_array_utils.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_math_base.hh"
#include "BLI_noise.hh"
#include "BLI_task.hh"

#include "GEO_trim_curves.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

class CutOperation : public CurvesSculptStrokeOperation {
 private:
  /** Only used when a 3D brush is used. */
  CurvesBrush3D brush_3d_;
  /**
   * Need to store these in case the brush is evaluated more than once before the curves are
   * evaluated again. This can happen when the mouse is moved quickly and the brush spacing is
   * small.
   */
  Vector<float3> deformed_positions_;

  friend struct CutOperationExecutor;

 public:
  void on_stroke_extended(const PaintStroke &stroke,
                          const StrokeExtension &stroke_extension) override;
};

struct BrushProjectionInfo {
  float distance = std::numeric_limits<float>::max();
  float4x4 brush_transform_inv;
};

/**
 * Utility class that actually executes the update when the stroke is updated. That's useful
 * because it avoids passing a very large number of parameters between functions.
 */
struct CutOperationExecutor {
  CutOperation *self_ = nullptr;
  CurvesSculptCommonContext ctx_;

  Object *object_ = nullptr;
  Curves *curves_id_ = nullptr;
  CurvesGeometry *curves_ = nullptr;

  VArray<float> point_factors_;
  IndexMaskMemory selected_curve_memory_;
  IndexMask curve_selection_;

  CurvesSculpt *curves_sculpt_ = nullptr;
  const Brush *brush_ = nullptr;
  const BrushCurvesSculptSettings *brush_settings_ = nullptr;
  float brush_radius_base_re_;
  float brush_radius_factor_;
  float2 brush_pos_re_;
  float brush_strength_;

  CurvesSurfaceTransforms transforms_;

  CutOperationExecutor(const PaintStroke &stroke) : ctx_(stroke) {}

  void execute(CutOperation &self, const StrokeExtension &stroke_extension)
  {
    UNUSED_VARS(stroke_extension);
    self_ = &self;

    object_ = ctx_.object;
    curves_id_ = id_cast<Curves *>(object_->data);
    curves_ = &curves_id_->geometry.wrap();
    if (curves_->is_empty()) {
      return;
    }

    curve_selection_ = curves::retrieve_selected_curves(*curves_id_, selected_curve_memory_);
    if (curve_selection_.is_empty()) {
      return;
    }

    curves_sculpt_ = ctx_.scene->toolsettings->curves_sculpt;
    brush_ = BKE_paint_brush_for_read(&curves_sculpt_->paint);
    brush_settings_ = brush_->curves_sculpt_settings;
    brush_radius_base_re_ = BKE_brush_radius_get(&curves_sculpt_->paint, brush_);
    brush_radius_factor_ = brush_radius_factor(*brush_, stroke_extension);
    brush_pos_re_ = stroke_extension.mouse_position;
    brush_strength_ = brush_strength_get(curves_sculpt_->paint, *brush_, stroke_extension);

    point_factors_ = *curves_->attributes().lookup_or_default<float>(
        ".selection", bke::AttrDomain::Point, 1.0f);
    transforms_ = CurvesSurfaceTransforms(*object_, curves_id_->surface);

    const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush_->falloff_shape);
    if (stroke_extension.is_first) {
      if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
        std::optional<CurvesBrush3D> brush_3d = sample_curves_3d_brush(*ctx_.depsgraph,
                                                                       *ctx_.region,
                                                                       *ctx_.v3d,
                                                                       *ctx_.rv3d,
                                                                       *object_,
                                                                       brush_pos_re_,
                                                                       brush_radius_base_re_);
        if (!brush_3d.has_value()) {
          /* No valid brush position found (e.g. curves too short or no curves under brush). */
          return;
        }
        self.brush_3d_ = *brush_3d;
      }
      /* Store deformed positions on the first stroke to avoid issues when the brush is evaluated
       * multiple times before the curves are evaluated again. */
      const bke::crazyspace::GeometryDeformation deformation =
          bke::crazyspace::get_evaluated_curves_deformation(*ctx_.depsgraph, *object_);
      self.deformed_positions_ = deformation.positions;
    }

    const VArray<bool> cyclic = curves_->cyclic();
    Array<bool> cyclic_array(cyclic.size());
    array_utils::copy(cyclic, cyclic_array.as_mutable_span());
    IndexMask curve_selection_without_cyclic = IndexMask::from_bools_inverse(
        curve_selection_, cyclic_array, selected_curve_memory_);
    const bool selection_has_cyclic = curve_selection_without_cyclic.size() <
                                      curve_selection_.size();
    curve_selection_ = curve_selection_without_cyclic;

    Array<BrushProjectionInfo> brush_projection_info(curves_->points_num());
    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      this->find_projected_points_in_stroke_with_symmetry(brush_projection_info);
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      this->find_spherical_points_in_stroke_with_symmetry(brush_projection_info);
    }
    else {
      BLI_assert_unreachable();
    }

    Array<bool> curves_to_keep(curves_->curves_num(), true);
    Array<float> ends(curves_->curves_num(), std::numeric_limits<float>::max());

    this->cut_curves(brush_projection_info, curves_to_keep, ends);

    const bool keep_points = brush_settings_->flag & BRUSH_CURVES_SCULPT_FLAG_CUT_KEEP_POINTS;

    if (keep_points) {
      /* Shrink curves while keeping point count - proportionally compress points. */
      this->shrink_curves_keep_points(curves_to_keep, ends);
      /* Update stored positions after shrink_curves_keep_points modifies positions in place. */
      if (!curves_->is_empty()) {
        self_->deformed_positions_.clear();
        self_->deformed_positions_.extend(curves_->positions());
      }
    }
    else {
      /* Default behavior: delete curves and trim with resampling. */
      IndexMaskMemory mask_memory;
      const IndexMask mask_to_keep = IndexMask::from_bools(curves_to_keep, mask_memory);

      /* Update deformed_positions_ before curves_copy_curve_selection changes the topology. */
      const OffsetIndices points_by_curve = curves_->points_by_curve();
      Vector<float3> new_deformed_positions;
      mask_to_keep.foreach_index([&](const int64_t i) {
        new_deformed_positions.extend(
            self_->deformed_positions_.as_span().slice(points_by_curve[i]));
      });
      self_->deformed_positions_ = std::move(new_deformed_positions);

      *curves_ = bke::curves_copy_curve_selection(*curves_, mask_to_keep, {});
      curves_->tag_topology_changed();

      Array<float> kept_ends(mask_to_keep.size());
      array_utils::gather(ends.as_span(), mask_to_keep, kept_ends.as_mutable_span());

      if (!curves_->is_empty()) {
        *curves_ = geometry::trim_curves(*curves_,
                                         IndexMask(curves_->curves_num()),
                                         VArray<float>::from_single(0.0f, curves_->curves_num()),
                                         VArray<float>::from_span(kept_ends),
                                         GeometryNodeCurveSampleMode::GEO_NODE_CURVE_SAMPLE_LENGTH,
                                         {});
        /* Update stored positions after trim_curves since it creates new geometry. */
        self_->deformed_positions_.clear();
        self_->deformed_positions_.extend(curves_->positions());
      }
    }

    if (selection_has_cyclic) {
      report_cyclic_not_supported(stroke_extension.reports);
    }

    DEG_id_tag_update(&curves_id_->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &curves_id_->id);
    ED_region_tag_redraw(ctx_.region);
  }

  /**
   * Shrink curves by moving endpoints to the cut boundary while keeping the number of points.
   * Points are proportionally compressed towards the root of the curve.
   */
  void shrink_curves_keep_points(const Span<bool> curves_to_change, const Span<float> new_ends)
  {
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    MutableSpan<float3> positions = curves_->positions_for_write();

    /* First pass: delete curves where the root is under the brush. */
    IndexMaskMemory mask_memory;
    const IndexMask mask_to_keep = IndexMask::from_bools(curves_to_change, mask_memory);

    /* Update deformed_positions_ before curves_copy_curve_selection changes the topology. */
    Vector<float3> new_deformed_positions;
    mask_to_keep.foreach_index([&](const int64_t i) {
      new_deformed_positions.extend(
          self_->deformed_positions_.as_span().slice(points_by_curve[i]));
    });
    self_->deformed_positions_ = std::move(new_deformed_positions);

    *curves_ = bke::curves_copy_curve_selection(*curves_, mask_to_keep, {});
    curves_->tag_topology_changed();

    if (curves_->is_empty()) {
      return;
    }

    /* Gather the ends for kept curves. */
    Array<float> kept_ends(mask_to_keep.size());
    array_utils::gather(new_ends, mask_to_keep, kept_ends.as_mutable_span());

    /* Re-get positions after topology change. */
    positions = curves_->positions_for_write();
    const OffsetIndices new_points_by_curve = curves_->points_by_curve();

    /* Second pass: proportionally compress points for each curve. */
    threading::parallel_for(curves_->curves_range(), 256, [&](const IndexRange range) {
      /* Pre-allocate reusable buffers. */
      Vector<float> accumulated_lengths;
      Vector<float> new_lengths;
      Vector<int> segment_indices;
      Vector<float> factors;
      Vector<float3> new_positions;

      for (const int curve_i : range) {
        const float new_end = kept_ends[curve_i];
        if (new_end == std::numeric_limits<float>::max()) {
          /* Curve was not cut, skip. */
          continue;
        }

        const IndexRange points = new_points_by_curve[curve_i];
        if (points.size() < 2) {
          continue;
        }

        const int points_num = points.size();
        const int num_segments = points_num - 1;

        accumulated_lengths.reinitialize(num_segments);
        new_lengths.reinitialize(points_num);
        segment_indices.reinitialize(points_num);
        factors.reinitialize(points_num);
        new_positions.reinitialize(points_num);

        /* Calculate accumulated segment lengths for the original curve. */
        length_parameterize::accumulate_lengths<float3>(
            positions.slice(points), false, accumulated_lengths.as_mutable_span());

        const float old_total_length = accumulated_lengths.last();
        if (old_total_length < 1e-6f) {
          /* Curve is too short, skip. */
          continue;
        }

        /* Calculate new lengths by scaling proportionally. */
        const float scale_factor = new_end / old_total_length;
        for (const int i : IndexRange(points_num)) {
          const float old_length = (i == 0) ? 0.0f :
                                     (i < num_segments ? accumulated_lengths[i - 1] :
                                                         accumulated_lengths.last());
          new_lengths[i] = old_length * scale_factor;
        }

        /* Sample at new lengths to get segment indices and factors. */
        length_parameterize::sample_at_lengths(accumulated_lengths.as_span(),
                                               new_lengths.as_span(),
                                               segment_indices.as_mutable_span(),
                                               factors.as_mutable_span());

        /* Interpolate new positions based on sampled indices and factors. */
        length_parameterize::interpolate<float3>(positions.slice(points),
                                         segment_indices.as_span(),
                                         factors.as_span(),
                                         new_positions.as_mutable_span());

        /* Copy new positions back. */
        positions.slice(points).copy_from(new_positions.as_span());
      }
    });
  }

  void find_projected_points_in_stroke_with_symmetry(
      MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_projected_points_in_stroke(math::invert(brush_transform),
                                            r_brush_projection_info);
    }
  }

  void find_projected_points_in_stroke(const float4x4 &brush_transform_inv,
                                       MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    const float brush_radius_re = brush_radius_base_re_ * brush_radius_factor_;
    const float brush_radius_sq_re = math::square(brush_radius_re);

    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    const Span<float3> positions = self_->deformed_positions_;
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int i : IndexRange(points.size())) {
        const int point_i = points[i];

        const float3 &pos_cu = math::transform_point(brush_transform_inv,
                                                     positions[point_i]);
        const float2 pos_re = ED_view3d_project_float_v2_m4(ctx_.region, pos_cu, projection);
        const float dist_to_brush_sq_re = math::distance_squared(pos_re, brush_pos_re_);
        if (dist_to_brush_sq_re < r_brush_projection_info[point_i].distance) {
          r_brush_projection_info[point_i].distance = dist_to_brush_sq_re;
          r_brush_projection_info[point_i].brush_transform_inv = brush_transform_inv;
        }
      }
    });
  }

  void find_spherical_points_in_stroke_with_symmetry(
      MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    float3 brush_pos_wo;
    ED_view3d_win_to_3d(
        ctx_.v3d,
        ctx_.region,
        math::transform_point(transforms_.curves_to_world, self_->brush_3d_.position_cu),
        brush_pos_re_,
        brush_pos_wo);
    const float3 brush_pos_cu = math::transform_point(transforms_.world_to_curves, brush_pos_wo);

    const Vector<float4x4> symmetry_brush_transforms = get_symmetry_brush_transforms(
        eCurvesSymmetryType(curves_id_->symmetry));
    for (const float4x4 &brush_transform : symmetry_brush_transforms) {
      this->find_spherical_points_in_stroke(math::invert(brush_transform),
                                            math::transform_point(brush_transform, brush_pos_cu),
                                            r_brush_projection_info);
    }
  }

  void find_spherical_points_in_stroke(const float4x4 &brush_transform_inv,
                                       const float3 &brush_pos_cu,
                                       MutableSpan<BrushProjectionInfo> r_brush_projection_info)
  {
    const float brush_radius_cu = self_->brush_3d_.radius_cu * brush_radius_factor_;
    const float brush_radius_sq_cu = math::square(brush_radius_cu);
    const Span<float3> positions = self_->deformed_positions_;
    const OffsetIndices points_by_curve = curves_->points_by_curve();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      for (const int i : IndexRange(points.size())) {
        const int point_i = points[i];

        const float3 &pos_cu = positions[point_i];
        const float dist_to_brush_sq_cu = math::distance_squared(pos_cu, brush_pos_cu);
        if (dist_to_brush_sq_cu < r_brush_projection_info[point_i].distance) {
          r_brush_projection_info[point_i].distance = dist_to_brush_sq_cu;
          r_brush_projection_info[point_i].brush_transform_inv = brush_transform_inv;
        }
      }
    });
  }

  /**
   * \param stroke_hash: A hash which should try to return different results for each iteration of
   * the cut. Used for "blunt scissors" mode, based on brush strength.
   */
  bool should_point_be_cut(const int point_i, const uint32_t stroke_hash)
  {
    if (point_factors_[point_i] <= 0.0f) {
      return false;
    }

    if (brush_strength_ < 1.0f) {
      const float stroke_chance = noise::hash_to_float(stroke_hash);
      if (stroke_chance > brush_strength_) {
        return false;
      }
    }

    return true;
  }

  float3 find_projected_cut_boundary(const float3 &point_outside_cu,
                                     const float3 &point_inside_cu,
                                     const float2 &brush_pos_re,
                                     const float brush_radius_re,
                                     const float4x4 &transform_cu_to_re)
  {
    const float2 point_outside_re = ED_view3d_project_float_v2_m4(
        ctx_.region, point_outside_cu, transform_cu_to_re);
    const float2 point_inside_re = ED_view3d_project_float_v2_m4(
        ctx_.region, point_inside_cu, transform_cu_to_re);

    const float2 line_re = point_inside_re - point_outside_re;
    const float2 brush_to_outside_re = point_outside_re - brush_pos_re;

    const float a = math::dot(line_re, line_re);
    const float b = 2.0f * math::dot(line_re, brush_to_outside_re);
    const float c = math::dot(brush_to_outside_re, brush_to_outside_re) -
                    math::square(brush_radius_re);

    float d = b * b - 4.0f * a * c;
    /* It shouldn't be possible to have no intersection (d < 0), so assume to be tangential. */
    d = math::max(d, 0.0f);
    const float t = (-b - math::sqrt(d)) / (2.0f * a);

    const float3 line_cu = point_inside_cu - point_outside_cu;
    const float3 intersection_cu = point_outside_cu + t * line_cu;
    return intersection_cu;
  }

  float3 find_spherical_cut_boundary(const float3 &point_outside_cu,
                                     const float3 &point_inside_cu,
                                     const float3 &brush_pos_cu,
                                     const float brush_radius_cu)
  {
    const float3 line_cu = point_inside_cu - point_outside_cu;
    const float3 brush_to_outside_cu = point_outside_cu - brush_pos_cu;

    const float a = math::dot(line_cu, line_cu);
    const float b = 2.0f * math::dot(line_cu, brush_to_outside_cu);
    const float c = math::dot(brush_to_outside_cu, brush_to_outside_cu) -
                    math::square(brush_radius_cu);

    float d = b * b - 4.0f * a * c;
    /* It shouldn't be possible to have no intersection (d < 0), so assume to be tangential. */
    d = math::max(d, 0.0f);
    const float t = (-b - math::sqrt(d)) / (2.0f * a);

    const float3 intersection_cu = point_outside_cu + t * line_cu;
    return intersection_cu;
  }

  void cut_curves(const Span<BrushProjectionInfo> brush_projection_info,
                  MutableSpan<bool> r_curves_to_keep,
                  MutableSpan<float> r_ends)
  {
    const OffsetIndices points_by_curve = curves_->points_by_curve();
    const Span<float3> positions = self_->deformed_positions_;

    Array<float> segment_lengths(curves_->points_num());
    curve_selection_.foreach_segment(GrainSize(256), [&](const IndexMaskSegment segment) {
      for (const int curve_i : segment) {
        const IndexRange points = points_by_curve[curve_i];
        float accumulated_length = 0.0f;
        for (const int i : points.index_range()) {
          const int point_i = points[i];
          if (i == 0) {
            segment_lengths[point_i] = 0.0f;
            continue;
          }
          const float3 &p1 = positions[point_i - 1];
          const float3 &p2 = positions[point_i];
          accumulated_length += math::distance(p1, p2);
          segment_lengths[point_i] = accumulated_length;
        }
      }
    });

    const float4x4 projection = ED_view3d_ob_project_mat_get(ctx_.rv3d, object_);

    /* cu or re, depending on falloff shape */
    float brush_radius = 0.0f;
    /* Only used for sphere falloff. */
    float3 brush_pos_cu{0.0f, 0.0f, 0.0f};

    const eBrushFalloffShape falloff_shape = eBrushFalloffShape(brush_->falloff_shape);
    if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      brush_radius = brush_radius_base_re_ * brush_radius_factor_;
    }
    else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
      brush_radius = self_->brush_3d_.radius_cu * brush_radius_factor_;

      float3 brush_pos_wo;
      ED_view3d_win_to_3d(
          ctx_.v3d,
          ctx_.region,
          math::transform_point(transforms_.curves_to_world, self_->brush_3d_.position_cu),
          brush_pos_re_,
          brush_pos_wo);
      brush_pos_cu = math::transform_point(transforms_.world_to_curves, brush_pos_wo);
    }
    else {
      BLI_assert_unreachable();
    }

    const float brush_radius_sq = math::square(brush_radius);
    const uint64_t brush_pos_hash = brush_pos_re_.hash();

    curve_selection_.foreach_index(GrainSize(256), [&](const int curve_i) {
      const IndexRange points = points_by_curve[curve_i];
      const Span<BrushProjectionInfo> brush_projection_info_slice = brush_projection_info.slice(
          points);

      const BrushProjectionInfo *first_point_in_stroke_bpi = std::find_if(
          brush_projection_info_slice.begin(),
          brush_projection_info_slice.end(),
          [&](const BrushProjectionInfo projection_info) {
            return projection_info.distance <= brush_radius_sq;
          });
      if (first_point_in_stroke_bpi == brush_projection_info_slice.end()) {
        return;
      }
      const int first_point_in_stroke = std::distance(brush_projection_info_slice.begin(),
                                                      first_point_in_stroke_bpi);

      const IndexRange::Iterator point_to_cut_iter = std::find_if(
          points.begin(), points.end(), [&](const int point_i) {
            const BrushProjectionInfo &bpi = brush_projection_info[point_i];
            if (bpi.distance > brush_radius_sq) {
              return false;
            }

            const uint32_t point_hash = noise::hash(
                noise::hash_float(first_point_in_stroke_bpi->distance), brush_pos_hash);
            return should_point_be_cut(point_i, point_hash);
          });
      if (point_to_cut_iter == points.end()) {
        return;
      }
      const int point_to_cut = std::distance(points.begin(), point_to_cut_iter);

      if (point_to_cut == 0) {
        /* Entire curve is under the brush. */
        const float min_length = brush_settings_->minimum_trim_length;
        if (min_length > 0.0f) {
          /* Keep curve but shrink to minimum length. */
          r_ends[curve_i] = min_length;
        }
        else {
          /* Delete entire curve. Simply trimming would leave behind the root control point. */
          r_curves_to_keep[curve_i] = false;
        }
      }
      else if (first_point_in_stroke == point_to_cut) {
        /* Brush boundary is cutting straight through previous and current point.
         * Delete all points after current. */
        const int current_point = points[point_to_cut];
        const int previous_point = points[point_to_cut - 1];
        const float3 &curr_pos_cu = positions[current_point];
        const float3 &prev_pos_cu = positions[previous_point];
        float3 boundary_cu;

        if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
          boundary_cu = math::transform_point(
              first_point_in_stroke_bpi->brush_transform_inv,
              find_projected_cut_boundary(
                  prev_pos_cu, curr_pos_cu, brush_pos_re_, brush_radius, projection));
        }
        else if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
          boundary_cu = find_spherical_cut_boundary(
              prev_pos_cu,
              curr_pos_cu,
              math::transform_point(first_point_in_stroke_bpi->brush_transform_inv, brush_pos_cu),
              brush_radius);
        }
        else {
          BLI_assert_unreachable();
        }

        const float boundary_length = math::distance(curr_pos_cu, boundary_cu);
        r_ends[curve_i] = segment_lengths[current_point] - boundary_length;
      }
      else {
        /* Brush is encompassing a boundary between selected and unselected points. */
        const int previous_point = points[point_to_cut - 1];
        r_ends[curve_i] = segment_lengths[previous_point];
      }
    });
  }
};

void CutOperation::on_stroke_extended(const PaintStroke &stroke,
                                      const StrokeExtension &stroke_extension)
{
  CutOperationExecutor executor{stroke};
  executor.execute(*this, stroke_extension);
}

std::unique_ptr<CurvesSculptStrokeOperation> new_cut_operation()
{
  return std::make_unique<CutOperation>();
}

}  // namespace blender::ed::sculpt_paint
