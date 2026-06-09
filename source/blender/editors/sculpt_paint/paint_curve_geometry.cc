/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Geometry access, queries and radius policy for paint curves.
 *
 * This is the single place that understands the embedded #PaintCurve::geometry
 * (a #blender::bke::CurvesGeometry). Multi-spline support, point attributes and
 * additional curve types are meant to grow here rather than in the operator or
 * legacy-2D bridge translation units.
 */

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"

#include "BLI_assert.h"
#include "BLI_function_ref.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Geometry Validity & Queries
 * \{ */

bool paintcurve_geometry_is_valid(const bke::CurvesGeometry &geom)
{
  /* `runtime` is null while the embedded geometry has never been allocated; in that state none
   * of the attribute accessors are safe to call. An empty geometry is treated as invalid too. */
  return geom.runtime != nullptr && geom.points_num() > 0;
}

bool paintcurve_is_cyclic(const PaintCurve *pc)
{
  return paintcurve_is_curve_cyclic(pc, 0);
}

bool paintcurve_is_curve_cyclic(const PaintCurve *pc, const int curve_index)
{
  if (pc == nullptr) {
    return false;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime == nullptr || geom.curves_num() == 0) {
    return false;
  }
  if (curve_index < 0 || curve_index >= geom.curves_num()) {
    return false;
  }
  return geom.cyclic()[curve_index];
}

bool paintcurve_has_multi_curves(const PaintCurve *pc)
{
  if (pc == nullptr) {
    return false;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  return geom.runtime != nullptr && geom.curves_num() > 1;
}

bool paintcurve_uses_3d_geometry(const PaintCurve *pc)
{
  if (pc == nullptr || !pc->use_3d_space) {
    return false;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  return paintcurve_geometry_is_valid(geom) && geom.points_num() == pc->tot_points;
}

void paintcurve_foreach_bezier_segment(
    const PaintCurve *pc, const FunctionRef<void(int point_index_a, int point_index_b)> fn)
{
  if (pc == nullptr) {
    return;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime != nullptr && geom.curves_num() > 0 && geom.points_num() >= 2) {
    const OffsetIndices<int> points_by_curve = geom.points_by_curve();
    const VArray<bool> cyclic = geom.cyclic();
    for (const int curve_i : geom.curves_range()) {
      const IndexRange points = points_by_curve[curve_i];
      if (points.size() < 2 && !cyclic[curve_i]) {
        continue;
      }
      const int segment_num = bke::curves::segments_num(points.size(), cyclic[curve_i]);
      for (const int seg_i : IndexRange(segment_num)) {
        const int local_a = seg_i;
        const int local_b = (seg_i + 1) % points.size();
        fn(points[local_a], points[local_b]);
      }
    }
    return;
  }

  if (pc->tot_points < 2) {
    return;
  }

  /* Refuse to connect curves in the flat fallback when geometry marks this as multi-spline.
   * Topology collapse elsewhere may have left geom invalid (points_num < 2) but still with
   * curves_num > 1; generating a cross-spline segment would be a silent data corruption. */
  if (geom.runtime != nullptr && geom.curves_num() > 1) {
    return;
  }

  const bool cyclic = paintcurve_is_curve_cyclic(pc, 0);
  const int segment_num = cyclic ? pc->tot_points : pc->tot_points - 1;
  for (int i = 0; i < segment_num; i++) {
    fn(i, (i + 1) % pc->tot_points);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Control-Point Access
 * \{ */

float3 &paintcurve_geom_co(bke::CurvesGeometry &geom, const int point_idx, const int handle_idx)
{
  BLI_assert(point_idx >= 0 && point_idx < geom.points_num());
  BLI_assert(handle_idx >= 0 && handle_idx < 3);

  if (handle_idx == 0) {
    return geom.handle_positions_left_for_write()[point_idx];
  }
  if (handle_idx == 2) {
    return geom.handle_positions_right_for_write()[point_idx];
  }
  return geom.positions_for_write()[point_idx];
}

void paintcurve_geometry_init_bezier(bke::CurvesGeometry &geom, const int point_num)
{
  geom = bke::CurvesGeometry(point_num, point_num > 0 ? 1 : 0);
  if (point_num == 0) {
    return;
  }

  geom.offsets_for_write()[0] = 0;
  geom.offsets_for_write()[1] = point_num;
  geom.fill_curve_types(CURVE_TYPE_BEZIER);
  geom.resolution_for_write().fill(PAINT_CURVE_NUM_SEGMENTS);
  geom.handle_types_left_for_write().fill(BEZIER_HANDLE_AUTO);
  geom.handle_types_right_for_write().fill(BEZIER_HANDLE_AUTO);
  geom.tag_topology_changed();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Handle Types
 * \{ */

static int8_t paintcurve_next_handle_type(const int8_t handle_type)
{
  /* Same order as Curves Pen: Free → Auto → Vector → Align → Free. */
  return (handle_type + 1) % 4;
}

void paintcurve_cycle_point_handle_type(PaintCurve *pc, const int point_index)
{
  if (pc == nullptr || point_index < 0 || point_index >= pc->tot_points) {
    return;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (paintcurve_uses_3d_geometry(pc)) {
    const int8_t new_type = paintcurve_next_handle_type(geom.handle_types_right()[point_index]);
    geom.handle_types_left_for_write()[point_index] = new_type;
    geom.handle_types_right_for_write()[point_index] = new_type;
    geom.calculate_bezier_auto_handles();
    geom.calculate_bezier_aligned_handles();
    pc->points[point_index].bez.h1 = eBezTriple_Handle(new_type);
    pc->points[point_index].bez.h2 = eBezTriple_Handle(new_type);
  }
  else {
    const int8_t new_type = paintcurve_next_handle_type(int8_t(pc->points[point_index].bez.h2));
    pc->points[point_index].bez.h1 = eBezTriple_Handle(new_type);
    pc->points[point_index].bez.h2 = eBezTriple_Handle(new_type);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radius Policy
 * \{ */

float paintcurve_radius_from_source_geometry(const float source_radius)
{
  /* Legacy curve points are often stored as 0 until the user edits radius explicitly. */
  if (source_radius <= 0.0f) {
    return 1.0f;
  }
  return max_ff(source_radius, 0.0f);
}

float paintcurve_get_point_radius(const PaintCurve *pc, const int point_index)
{
  if (pc == nullptr || point_index < 0 || point_index >= pc->tot_points || pc->points == nullptr) {
    return 1.0f;
  }
  return max_ff(pc->points[point_index].bez.radius, 0.0f);
}

float paintcurve_radius_to_pixel_radius(const Paint *paint,
                                        const Brush *brush,
                                        const float point_radius)
{
  const float brush_radius = max_ff(float(BKE_brush_radius_get(paint, brush)), 1.0f);
  const float radius = max_ff(point_radius, 0.0f);
  if (radius <= 1.0f) {
    return math::interpolate(1.0f, brush_radius, radius);
  }
  return brush_radius * radius;
}

float paintcurve_radius_to_size_factor(const Paint *paint,
                                       const Brush *brush,
                                       const float point_radius)
{
  const float brush_radius = max_ff(float(BKE_brush_radius_get(paint, brush)), 1.0f);
  return paintcurve_radius_to_pixel_radius(paint, brush, point_radius) / brush_radius;
}

/** \} */

}  // namespace blender
