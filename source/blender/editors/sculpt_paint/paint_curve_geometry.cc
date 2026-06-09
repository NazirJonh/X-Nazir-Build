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

#include <algorithm>

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
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
  return paintcurve_geometry_is_valid(geom) && geom.points_num() > 0;
}

int paintcurve_curve_of_point(const PaintCurve *pc, const int point_index)
{
  if (pc == nullptr) {
    return -1;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom) || point_index < 0 ||
      point_index >= geom.points_num())
  {
    return -1;
  }
  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  for (const int curve_i : geom.curves_range()) {
    if (points_by_curve[curve_i].contains(point_index)) {
      return curve_i;
    }
  }
  return -1;
}

int paintcurve_active_curve_get(const PaintCurve *pc)
{
  if (pc == nullptr) {
    return 0;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  const int curves_num = (geom.runtime != nullptr) ? geom.curves_num() : 0;
  if (curves_num == 0) {
    return 0;
  }
  return std::clamp(pc->active_curve, 0, curves_num - 1);
}

int paintcurve_geometry_add_spline(bke::CurvesGeometry &geom, const int point_num)
{
  if (point_num <= 0) {
    return -1;
  }
  const int old_points = (geom.runtime != nullptr) ? geom.points_num() : 0;
  const int old_curves = (geom.runtime != nullptr) ? geom.curves_num() : 0;
  geom.resize(old_points + point_num, old_curves + 1);
  /* `resize` sets curve_offsets[0] = 0 and curve_offsets[new_curves_num] = new_points_num.
   * For the new spline, we need to set its starting offset. */
  geom.offsets_for_write()[old_curves] = old_points;
  geom.fill_curve_types(CURVE_TYPE_BEZIER);
  geom.handle_types_left_for_write().slice(old_points, point_num).fill(BEZIER_HANDLE_AUTO);
  geom.handle_types_right_for_write().slice(old_points, point_num).fill(BEZIER_HANDLE_AUTO);
  geom.resolution_for_write()[old_curves] = PAINT_CURVE_NUM_SEGMENTS;
  geom.tag_topology_changed();
  return old_curves;
}

void paintcurve_geometry_remove_points(bke::CurvesGeometry &geom,
                                       const IndexMask &points_to_remove)
{
  if (!paintcurve_geometry_is_valid(geom) || points_to_remove.is_empty()) {
    return;
  }

  /* Use the built-in CurvesGeometry remove_points method. */
  geom.remove_points(points_to_remove, {});

  /* Remove any curves that are now empty. */
  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  Vector<int> curves_to_remove;
  for (const int curve_i : geom.curves_range()) {
    if (points_by_curve[curve_i].is_empty()) {
      curves_to_remove.append(curve_i);
    }
  }

  if (!curves_to_remove.is_empty()) {
    IndexMaskMemory memory;
    const IndexMask curves_mask = IndexMask::from_indices<int>(curves_to_remove.as_span(),
                                                                memory);
    geom.remove_curves(curves_mask, {});
  }
}

void paintcurve_foreach_bezier_segment(
    const PaintCurve *pc, const FunctionRef<void(int point_index_a, int point_index_b)> fn)
{
  if (pc == nullptr) {
    return;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime == nullptr || geom.points_num() < 2) {
    return;
  }
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
  if (pc == nullptr || point_index < 0) {
    return;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom) || point_index >= geom.points_num()) {
    return;
  }

  const int8_t new_type = paintcurve_next_handle_type(geom.handle_types_right()[point_index]);
  geom.handle_types_left_for_write()[point_index] = new_type;
  geom.handle_types_right_for_write()[point_index] = new_type;
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();
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
  if (pc == nullptr || point_index < 0) {
    return 1.0f;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom) || point_index >= geom.points_num()) {
    return 1.0f;
  }
  return max_ff(geom.radius()[point_index], 0.0f);
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

/* -------------------------------------------------------------------- */
/** \name Per-Point Selection Attributes
 *
 * Selection state (left handle / control point / right handle) is stored as a packed
 * `int8_t` attribute per point: bit 0 = left, bit 1 = control point, bit 2 = right.
 * This keeps selection in the geometry so it persists across save/load and undo.
 * \{ */

static constexpr const char *PC_ATTR_SELECTION = "paintcurve_selection";

uint8_t paintcurve_geom_get_selection(const bke::CurvesGeometry &geom, const int point_index)
{
  const bke::AttributeAccessor attrs = geom.attributes();
  const VArray<int8_t> sel = *attrs.lookup_or_default<int8_t>(
      PC_ATTR_SELECTION, bke::AttrDomain::Point, int8_t(0));
  return uint8_t(sel[point_index]);
}

void paintcurve_geom_set_selection(bke::CurvesGeometry &geom,
                                   const int point_index,
                                   const uint8_t flags)
{
  bke::MutableAttributeAccessor attrs = geom.attributes_for_write();
  bke::SpanAttributeWriter<int8_t> sel = attrs.lookup_or_add_for_write_span<int8_t>(
      PC_ATTR_SELECTION, bke::AttrDomain::Point);
  sel.span[point_index] = int8_t(flags);
  sel.finish();
}

void paintcurve_geom_set_all_selection(bke::CurvesGeometry &geom, const uint8_t flags)
{
  if (!paintcurve_geometry_is_valid(geom) || geom.points_num() == 0) {
    return;
  }
  bke::MutableAttributeAccessor attrs = geom.attributes_for_write();
  if (flags == 0) {
    attrs.remove(PC_ATTR_SELECTION);
    return;
  }
  bke::SpanAttributeWriter<int8_t> sel = attrs.lookup_or_add_for_write_span<int8_t>(
      PC_ATTR_SELECTION, bke::AttrDomain::Point);
  sel.span.fill(int8_t(flags));
  sel.finish();
}

bool paintcurve_geom_any_selected(const bke::CurvesGeometry &geom)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }
  const bke::AttributeAccessor attrs = geom.attributes();
  const VArray<int8_t> sel = *attrs.lookup_or_default<int8_t>(
      PC_ATTR_SELECTION, bke::AttrDomain::Point, int8_t(0));
  for (const int i : geom.points_range()) {
    if (sel[i] != 0) {
      return true;
    }
  }
  return false;
}

/** \} */

}  // namespace blender
