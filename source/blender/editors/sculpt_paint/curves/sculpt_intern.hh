/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "../paint_intern.hh"

#include "sculpt_multi_object.hh"

#include "BLI_array.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

#include "ED_curves.hh"

namespace blender {

struct ARegion;
struct Brush;
struct Depsgraph;
struct Object;
struct RegionView3D;
struct Scene;
struct View3D;

namespace bke {
struct BVHTreeFromMesh;
}

struct ReportList;

namespace ed::sculpt_paint {

using bke::CurvesGeometry;
using bke::CurvesSurfaceTransforms;

struct StrokeExtension {
  bool is_first;
  float2 mouse_position;
  float pressure;
  ReportList *reports = nullptr;
  /**
   * The Curves objects this stroke acts on, resolved once at stroke start. Never null while a
   * stroke is running.
   */
  const CurvesMultiObjectStrokeContext *targets = nullptr;
};

float brush_radius_factor(const Brush &brush, const StrokeExtension &stroke_extension);
float brush_radius_get(const Paint &paint,
                       const Brush &brush,
                       const StrokeExtension &stroke_extension);

float brush_strength_factor(const Brush &brush, const StrokeExtension &stroke_extension);
float brush_strength_get(const Paint &paint,
                         const Brush &brush,
                         const StrokeExtension &stroke_extension);

/**
 * Base class for stroke based operations in curves sculpt mode.
 */
class CurvesSculptStrokeOperation {
 public:
  virtual ~CurvesSculptStrokeOperation() = default;
  virtual void on_stroke_extended(const PaintStroke &stroke,
                                  const StrokeExtension &stroke_extension) = 0;
};

std::unique_ptr<CurvesSculptStrokeOperation> new_add_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_comb_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_delete_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_snake_hook_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_grow_shrink_operation(BrushStrokeMode brush_mode,
                                                                       const Scene &scene);
std::unique_ptr<CurvesSculptStrokeOperation> new_selection_paint_operation(
    BrushStrokeMode brush_mode, BrushSwitchMode brush_switch_mode, const Scene &scene);
std::unique_ptr<CurvesSculptStrokeOperation> new_pinch_operation(BrushStrokeMode brush_mode,
                                                                 const Scene &scene);
std::unique_ptr<CurvesSculptStrokeOperation> new_smooth_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_puff_operation();
std::unique_ptr<CurvesSculptStrokeOperation> new_density_operation(BrushStrokeMode brush_mode);
std::unique_ptr<CurvesSculptStrokeOperation> new_slide_operation();

struct CurvesBrush3D {
  float3 position_cu;
  float radius_cu;
};

/**
 * Find 3d brush position based on cursor position for curves sculpting.
 */
std::optional<CurvesBrush3D> sample_curves_3d_brush(const Depsgraph &depsgraph,
                                                    const ARegion &region,
                                                    const View3D &v3d,
                                                    const RegionView3D &rv3d,
                                                    const Object &curves_object,
                                                    const float2 &brush_pos_re,
                                                    const float brush_radius_re);

/**
 * Updates the position of the stroke so that it can be used by the orbit-around-selection
 * navigation method.
 */
void remember_stroke_position(CurvesSculpt &curves_sculpt, const float3 &brush_position_wo);

Vector<float4x4> get_symmetry_brush_transforms(eCurvesSymmetryType symmetry);

/**
 * Publish the per-point brush influence of \a curves_orig for the highlight visualization
 * (see #sculpt_influence_viz.cc). \a point_weights is consumed.
 */
void curves_sculpt_influence_viz_publish(Curves &curves_orig, Array<float> point_weights);

/** Stop publishing the brush influence. Called when the stroke ends (confirmed or cancelled). */
void curves_sculpt_influence_viz_clear();

/**
 * Publish the hover (pre-stroke) zone weights of \a curves_orig into the same registry as the
 * stroke influence. Ignored while a stroke record owns the data-block; an empty \a point_weights
 * removes a stale hover record.
 */
void curves_sculpt_hover_viz_publish(Curves &curves_orig, Array<float> point_weights);

/** Remove every hover record, keeping \a keep when it is not null. Returns whether any record
 * was removed. */
bool curves_sculpt_hover_viz_clear(const Curves *keep = nullptr);

/**
 * Zone-weight formulas shared by the stroke executors and the hover preview
 * (see #sculpt_hover_preview.cc). Each returns the brush influence without the brush strength
 * and without the per-brush operational scale factors, normalized to 0..1.
 */
float comb_point_zone_weight(float curve_falloff, float radius_falloff, float point_factor);
float snake_hook_curve_zone_weight(float radius_falloff, float curve_factor);
float pinch_point_zone_weight(float radius_falloff, float point_factor);
float puff_segment_zone_weight(const Brush *brush, float dist, float radius);
float smooth_point_zone_weight(float radius_falloff, float point_factor);
float slide_curve_zone_weight(float radius_falloff, float curve_factor);
float grow_shrink_curve_zone_weight(float radius_falloff, float curve_selection_factor);

/**
 * Brush zone preview for Sculpt Curves hover (see #sculpt_hover_preview.cc). Holds the weights
 * the first step of a stroke with pressure 1.0 would publish, without mutating any geometry.
 */
struct CurvesHoverPreview {
  /** Curves the brush would affect; drawn as solid lines, with the gradient zone on top. */
  Vector<int> hit_curves;
  /** Per-point zone weights (0..1), sized by the curves point count; 0 outside the zone. */
  Array<float> zone_weights;
};

/**
 * Compute the hover preview for one Curves object. Returns false when the brush misses or the
 * data is too large to preview. Never touches the depsgraph evaluation state: all context
 * (evaluated depsgraph, region, view) comes from the paint cursor caller.
 */
bool curves_sculpt_hover_preview_compute(const Depsgraph &depsgraph,
                                         const ARegion &region,
                                         const View3D &v3d,
                                         const RegionView3D &rv3d,
                                         const Object &curves_ob_orig,
                                         Brush &brush,
                                         float radius_base_re,
                                         float radius_re,
                                         const float2 &mouse_re,
                                         CurvesHoverPreview &r_preview);

/** Drop all cached hover previews, e.g. when a stroke ends and positions may have changed. */
void curves_sculpt_hover_preview_cache_clear();

bke::SpanAttributeWriter<float> float_selection_ensure(Curves &curves_id);

/** See #move_last_point_and_resample. */
struct MoveAndResampleBuffers {
  Vector<float> orig_lengths;
  Vector<float> new_lengths;

  Vector<int> sample_indices;
  Vector<float> sample_factors;

  Vector<float3> new_positions;
};

/**
 * \param buffer: Reused memory to avoid reallocations when the function is called many times.
 */
void move_last_point_and_resample(MoveAndResampleBuffers &buffer,
                                  MutableSpan<float3> positions,
                                  const float3 &new_last_position);

class CurvesSculptCommonContext {
 public:
  const Depsgraph *depsgraph = nullptr;
  Scene *scene = nullptr;
  ARegion *region = nullptr;
  const View3D *v3d = nullptr;
  RegionView3D *rv3d = nullptr;

  CurvesSculptCommonContext(const PaintStroke &stroke);
};

std::optional<CurvesBrush3D> sample_curves_surface_3d_brush(
    const Depsgraph &depsgraph,
    const ARegion &region,
    const View3D &v3d,
    const CurvesSurfaceTransforms &transforms,
    const bke::BVHTreeFromMesh &surface_bvh,
    const float2 &brush_pos_re,
    const float brush_radius_re);

float transform_brush_radius(const float4x4 &transform,
                             const float3 &brush_position,
                             const float old_radius);

void report_empty_original_surface(ReportList *reports);
void report_empty_evaluated_surface(ReportList *reports);
void report_missing_surface(ReportList *reports);
void report_missing_uv_map_on_original_surface(ReportList *reports);
void report_missing_uv_map_on_evaluated_surface(ReportList *reports);
void report_invalid_uv_map(ReportList *reports);

/**
 * Utility class to make it easy for brushes to implement length preservation and surface
 * collision.
 */
struct CurvesConstraintSolver {
 private:
  bool use_surface_collision_;
  float surface_collision_distance_;
  Array<float3> start_positions_;
  Array<float> segment_lengths_;

 public:
  void initialize(const bke::CurvesGeometry &curves,
                  const IndexMask &curve_selection,
                  const bool use_surface_collision,
                  const float surface_collision_distance);

  void solve_step(bke::CurvesGeometry &curves,
                  const IndexMask &curve_selection,
                  const Mesh *surface,
                  const CurvesSurfaceTransforms &transforms);

  Span<float> segment_lengths() const
  {
    return segment_lengths_;
  }
};

bool curves_sculpt_poll(bContext *C);
bool curves_sculpt_poll_view3d(bContext *C);

}  // namespace ed::sculpt_paint

}  // namespace blender
