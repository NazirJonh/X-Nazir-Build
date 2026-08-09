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
#include "BKE_attribute_math.hh"
#include "BKE_brush.hh"
#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_paint.hh"

#include "BLI_array_utils.hh"
#include "BLI_assert.h"
#include "BLI_function_ref.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

#include "ED_paint.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Geometry Validity & Queries
 * \{ */

bool paintcurve_geometry_runtime_is_initialized(const bke::CurvesGeometry &geom)
{
  return geom.runtime != nullptr;
}

bool paintcurve_geometry_is_valid(const bke::CurvesGeometry &geom)
{
  /* `runtime` is null while the embedded geometry has never been allocated; in that state none
   * of the attribute accessors are safe to call. An empty geometry is treated as invalid too. */
  return paintcurve_geometry_runtime_is_initialized(geom) && geom.points_num() > 0;
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
  if (!paintcurve_geometry_runtime_is_initialized(geom) || geom.curves_num() == 0) {
    return false;
  }
  if (curve_index < 0 || curve_index >= geom.curves_num()) {
    return false;
  }
  return geom.cyclic()[curve_index];
}

bool paintcurve_geometry_toggle_cyclic(bke::CurvesGeometry &geom, const int curve_index)
{
  if (!paintcurve_geometry_is_valid(geom) || curve_index < 0 || curve_index >= geom.curves_num()) {
    return false;
  }
  const bool was_cyclic = geom.cyclic()[curve_index];
  geom.cyclic_for_write()[curve_index] = !was_cyclic;
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();
  return true;
}

bool paintcurve_has_multi_curves(const PaintCurve *pc)
{
  if (pc == nullptr) {
    return false;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  return paintcurve_geometry_runtime_is_initialized(geom) && geom.curves_num() > 1;
}

bool paintcurve_uses_3d_geometry(const PaintCurve *pc)
{
  if (pc == nullptr || !pc->use_3d_space) {
    return false;
  }
  return paintcurve_geometry_is_valid(pc->geometry.wrap());
}

int paintcurve_curve_of_point_from_geometry(const bke::CurvesGeometry &geom, const int point_index)
{
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

int paintcurve_curve_of_point(const PaintCurve *pc, const int point_index)
{
  if (pc == nullptr) {
    return -1;
  }
  return paintcurve_curve_of_point_from_geometry(pc->geometry.wrap(), point_index);
}

int paintcurve_active_curve_get(const PaintCurve *pc)
{
  if (pc == nullptr) {
    return 0;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  const int curves_num = paintcurve_geometry_runtime_is_initialized(geom) ? geom.curves_num() : 0;
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
  const int old_points = paintcurve_geometry_runtime_is_initialized(geom) ? geom.points_num() : 0;
  const int old_curves = paintcurve_geometry_runtime_is_initialized(geom) ? geom.curves_num() : 0;
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

/** Match #curve_offsets_from_selection in curves_edit.cc (used by #duplicate_points). */
static void paintcurve_curve_offsets_from_selection(const Span<IndexRange> selected_points,
                                                    const IndexRange points,
                                                    const int curve,
                                                    const bool cyclic,
                                                    Vector<int> &r_new_curve_offsets,
                                                    Vector<bool> &r_new_cyclic,
                                                    Vector<IndexRange> &r_src_ranges,
                                                    Vector<int> &r_dst_offsets,
                                                    Vector<int> &r_dst_to_src_curve)
{
  if (selected_points.is_empty()) {
    return;
  }
  const bool merge_loop = cyclic && selected_points.first().size() < points.size() &&
                          selected_points.first().first() == points.first() &&
                          selected_points.last().last() == points.last();

  int last_dst_offset = r_dst_offsets.last();
  int last_curve_offset = r_new_curve_offsets.last();
  for (const IndexRange range : selected_points.drop_front(merge_loop)) {
    r_src_ranges.append(range);
    last_dst_offset += range.size();
    r_dst_offsets.append(last_dst_offset);
    last_curve_offset += range.size();
    r_new_curve_offsets.append(last_curve_offset);
  }
  if (merge_loop) {
    const IndexRange merge_to_end = selected_points.first();
    r_src_ranges.append(merge_to_end);
    r_dst_offsets.append(last_dst_offset + merge_to_end.size());
    r_new_curve_offsets.last() += merge_to_end.size();
  }
  const int curves_added = selected_points.size() - merge_loop;
  r_dst_to_src_curve.append_n_times(curve, curves_added);
  r_new_cyclic.append_n_times(cyclic && selected_points.first().size() == points.size(),
                              curves_added);
}

int paintcurve_geometry_duplicate_selected_points(bke::CurvesGeometry &geom)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return 0;
  }

  IndexMaskMemory memory;
  Vector<int> selected_indices;
  for (const int point_i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, point_i) & 0x07) {
      selected_indices.append(point_i);
    }
  }
  const IndexMask mask = IndexMask::from_indices<int>(selected_indices.as_span(), memory);
  if (mask.is_empty()) {
    return 0;
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  const VArray<bool> src_cyclic = geom.cyclic();

  Vector<int> dst_to_src_curve;
  Vector<int> new_curve_offsets({points_by_curve.data().last()});
  Vector<IndexRange> src_ranges;
  Vector<int> dst_offsets({0});
  Vector<bool> dst_cyclic;

  bke::curves::foreach_selected_point_ranges_per_curve(
      mask, points_by_curve, [&](const int curve, const IndexRange points, const Span<IndexRange> ranges) {
        paintcurve_curve_offsets_from_selection(ranges,
                                                points,
                                                curve,
                                                src_cyclic[curve],
                                                new_curve_offsets,
                                                dst_cyclic,
                                                src_ranges,
                                                dst_offsets,
                                                dst_to_src_curve);
      });

  const int num_curves_to_add = dst_to_src_curve.size();
  if (num_curves_to_add == 0) {
    return 0;
  }
  const int num_points_to_add = mask.size();

  const int old_curves_num = geom.curves_num();
  const int old_points_num = geom.points_num();

  geom.resize(old_points_num + num_points_to_add, old_curves_num + num_curves_to_add);

  array_utils::copy(new_curve_offsets.as_span(),
                    geom.offsets_for_write().drop_front(old_curves_num));

  bke::MutableAttributeAccessor attributes = geom.attributes_for_write();
  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.storage_type == bke::AttrStorageType::Single) {
      return;
    }
    bke::GSpanAttributeWriter attribute = attributes.lookup_for_write_span(iter.name);
    if (!attribute) {
      return;
    }

    switch (iter.domain) {
      case bke::AttrDomain::Curve: {
        if (iter.name == "cyclic") {
          attribute.finish();
          return;
        }
        bke::attribute_math::gather(
            attribute.span,
            dst_to_src_curve,
            attribute.span.slice(IndexRange(old_curves_num, num_curves_to_add)));
        break;
      }
      case bke::AttrDomain::Point: {
        bke::attribute_math::gather_ranges_to_groups(src_ranges.as_span(),
                                                    dst_offsets.as_span(),
                                                    attribute.span,
                                                    attribute.span.slice(
                                                        IndexRange(old_points_num, num_points_to_add)));
        break;
      }
      default: {
        attribute.finish();
        BLI_assert_unreachable();
        return;
      }
    }

    attribute.finish();
  });

  if (!(src_cyclic.is_single() && !src_cyclic.get_internal_single())) {
    array_utils::copy(dst_cyclic.as_span(), geom.cyclic_for_write().drop_front(old_curves_num));
  }

  paintcurve_geom_set_all_selection(geom, 0);
  for (const int pt_i : IndexRange(old_points_num, num_points_to_add)) {
    paintcurve_geom_set_selection(geom, pt_i, 0x07);
  }

  geom.update_curve_types();
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();
  return num_curves_to_add;
}

bke::CurvesGeometry paintcurve_geometry_build_from_selected_points(const bke::CurvesGeometry &geom)
{
  if (!paintcurve_geometry_is_valid(geom) || !paintcurve_geometry_any_point_selected(geom)) {
    return bke::CurvesGeometry();
  }

  IndexMaskMemory memory;
  Vector<int> selected_indices;
  for (const int point_i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, point_i) & 0x07) {
      selected_indices.append(point_i);
    }
  }
  const IndexMask mask = IndexMask::from_indices<int>(selected_indices.as_span(), memory);
  if (mask.is_empty()) {
    return bke::CurvesGeometry();
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  const VArray<bool> src_cyclic = geom.cyclic();

  Vector<int> dst_to_src_curve;
  Vector<int> curve_offsets({0});
  Vector<IndexRange> src_ranges;
  Vector<int> dst_offsets({0});
  Vector<bool> dst_cyclic;

  bke::curves::foreach_selected_point_ranges_per_curve(
      mask,
      points_by_curve,
      [&](const int curve, const IndexRange points, const Span<IndexRange> ranges) {
        paintcurve_curve_offsets_from_selection(ranges,
                                                points,
                                                curve,
                                                src_cyclic[curve],
                                                curve_offsets,
                                                dst_cyclic,
                                                src_ranges,
                                                dst_offsets,
                                                dst_to_src_curve);
      });

  const int num_curves = dst_to_src_curve.size();
  if (num_curves == 0) {
    return bke::CurvesGeometry();
  }
  const int num_points = curve_offsets.last();

  bke::CurvesGeometry dst(num_points, num_curves);
  array_utils::copy(curve_offsets.as_span(), dst.offsets_for_write());
  if (!dst_cyclic.is_empty()) {
    array_utils::copy(dst_cyclic.as_span(), dst.cyclic_for_write());
  }

  const bke::AttributeAccessor src_attributes = geom.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  /* Match #copy_data_to_geometry in curves_edit.cc: gather_attributes handles single-value
   * attributes (curve_type, uniform handle types, etc.) that foreach_attribute would skip. */
  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         dst_to_src_curve,
                         dst_attributes);

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes,
           dst_attributes,
           {bke::AttrDomain::Point},
           bke::attribute_filter_from_skip_ref({"paintcurve_selection"})))
  {
    bke::attribute_math::gather_ranges_to_groups(
        src_ranges.as_span(), OffsetIndices<int>(dst_offsets), attribute.src, attribute.dst.span);
    attribute.dst.finish();
  }

  dst.update_curve_types();
  dst.tag_topology_changed();
  return dst;
}

bool paintcurve_geometry_remove_selected_points(bke::CurvesGeometry &geom)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }

  Vector<int> selected_indices;
  for (const int point_i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, point_i) & 0x07) {
      selected_indices.append(point_i);
    }
  }
  if (selected_indices.is_empty()) {
    return false;
  }

  IndexMaskMemory remove_memory;
  const IndexMask remove_mask = IndexMask::from_indices<int>(selected_indices.as_span(),
                                                             remove_memory);
  paintcurve_geometry_remove_points(geom, remove_mask);

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();
  return true;
}

void paintcurve_geometry_select_linked(bke::CurvesGeometry &geom)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return;
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  for (const int curve_i : geom.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    bool any_selected = false;
    for (const int point_i : points) {
      if (paintcurve_geom_get_selection(geom, point_i) & 0x07) {
        any_selected = true;
        break;
      }
    }
    if (!any_selected) {
      continue;
    }
    for (const int point_i : points) {
      paintcurve_geom_set_selection(geom, point_i, 0x07);
    }
  }
}

bool paintcurve_geometry_any_point_selected(const bke::CurvesGeometry &geom)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }
  for (const int point_i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, point_i) & 0x07) {
      return true;
    }
  }
  return false;
}

void paintcurve_foreach_bezier_segment_from_geometry(
    const bke::CurvesGeometry &geom, const FunctionRef<void(int point_index_a, int point_index_b)> fn)
{
  if (!paintcurve_geometry_runtime_is_initialized(geom) || geom.points_num() < 2) {
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

void paintcurve_foreach_bezier_segment(
    const PaintCurve *pc, const FunctionRef<void(int point_index_a, int point_index_b)> fn)
{
  if (pc == nullptr) {
    return;
  }
  paintcurve_foreach_bezier_segment_from_geometry(pc->geometry.wrap(), fn);
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
  return source_radius;
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

float3 paintcurve_geom_get_surface_normal(const bke::CurvesGeometry &geom, const int point_index)
{
  const bke::AttributeAccessor attrs = geom.attributes();
  const VArray<float3> normals = *attrs.lookup_or_default<float3>(
      bke::CURVE_PATCH_ATTR_SURFACE_NORMAL, bke::AttrDomain::Point, float3(0.0f, 0.0f, 1.0f));
  return normals[point_index];
}

void paintcurve_geom_set_surface_normal(bke::CurvesGeometry &geom,
                                        const int point_index,
                                        const float3 &normal)
{
  bke::MutableAttributeAccessor attrs = geom.attributes_for_write();
  bke::SpanAttributeWriter<float3> normals = attrs.lookup_or_add_for_write_span<float3>(
      bke::CURVE_PATCH_ATTR_SURFACE_NORMAL, bke::AttrDomain::Point);
  const float len = math::length(normal);
  normals.span[point_index] = (len > 1e-6f) ? (normal / len) : float3(0.0f, 0.0f, 1.0f);
  normals.finish();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split at Selected Points
 * \{ */

static void paintcurve_copy_point_range_to_dst(const GSpan src,
                                               const IndexRange src_range,
                                               GMutableSpan dst,
                                               const int dst_start)
{
  for (const int i : src_range.index_range()) {
    src.type().copy_assign(src[src_range[i]], dst[dst_start + i]);
  }
}

static void paintcurve_translate_point_and_handles(bke::CurvesGeometry &geom,
                                                   const int point,
                                                   const float3 &delta)
{
  MutableSpan<float3> positions = geom.positions_for_write();
  positions[point] += delta;
  if (!geom.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    return;
  }
  geom.handle_positions_left_for_write()[point] += delta;
  geom.handle_positions_right_for_write()[point] += delta;
}

static bool paintcurve_endpoint_inward_direction(const bke::CurvesGeometry &geom,
                                                 const int point,
                                                 const IndexRange curve_points,
                                                 const bool is_curve_end,
                                                 float3 &r_direction)
{
  const Span<float3> positions = geom.positions();

  if (geom.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
    const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
    if (handles_left && handles_right) {
      const float3 &pos = positions[point];
      if (is_curve_end) {
        const float3 approach = pos - handles_left.value()[point];
        if (math::length_squared(approach) > 1e-10f) {
          r_direction = math::normalize(-approach);
          return true;
        }
      }
      else {
        const float3 tangent = handles_right.value()[point] - pos;
        if (math::length_squared(tangent) > 1e-10f) {
          r_direction = math::normalize(tangent);
          return true;
        }
      }
    }
  }

  if (is_curve_end) {
    if (curve_points.size() >= 2) {
      r_direction = math::normalize(positions[curve_points[curve_points.size() - 2]] -
                                    positions[point]);
      return true;
    }
  }
  else if (curve_points.size() >= 2) {
    r_direction = math::normalize(positions[curve_points[1]] - positions[point]);
    return true;
  }

  return false;
}

static void paintcurve_separate_split_curve_endpoints(bke::CurvesGeometry &geom,
                                                      const int left_curve,
                                                      const int right_curve)
{
  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  const IndexRange left_points = points_by_curve[left_curve];
  const IndexRange right_points = points_by_curve[right_curve];
  if (left_points.is_empty() || right_points.is_empty()) {
    return;
  }

  const int left_end = left_points.last();
  const int right_start = right_points.first();
  const Span<float3> positions = geom.positions();

  float3 left_inward;
  float3 right_inward;
  if (!paintcurve_endpoint_inward_direction(geom, left_end, left_points, true, left_inward) ||
      !paintcurve_endpoint_inward_direction(geom, right_start, right_points, false, right_inward))
  {
    return;
  }

  float ref_len = 1e-4f;
  if (geom.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
    const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
    if (handles_left && handles_right) {
      ref_len = math::max(
          ref_len,
          math::max(math::distance(positions[left_end], handles_left.value()[left_end]),
                    math::distance(handles_right.value()[right_start], positions[right_start])));
    }
  }
  if (left_points.size() >= 2) {
    ref_len = max_ff(
        ref_len, math::distance(positions[left_end], positions[left_points[left_points.size() - 2]]));
  }
  if (right_points.size() >= 2) {
    ref_len = max_ff(ref_len, math::distance(positions[right_points[1]], positions[right_start]));
  }

  const float offset = ref_len * PAINT_CURVE_SPLIT_ENDPOINT_SEPARATION;
  const float half_offset = offset * 0.5f;

  paintcurve_translate_point_and_handles(geom, left_end, left_inward * half_offset);
  paintcurve_translate_point_and_handles(geom, right_start, right_inward * half_offset);

  if (geom.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
    MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
    types_right[left_end] = BEZIER_HANDLE_FREE;
    types_left[right_start] = BEZIER_HANDLE_FREE;
  }
}

bool paintcurve_geometry_split_at_selected_points(bke::CurvesGeometry &geom,
                                                  int *r_selected_curve)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  const VArray<bool> src_cyclic = geom.cyclic();

  /* Collect split points per curve (local indices). */
  Vector<Vector<int>> split_indices_per_curve(geom.curves_num());
  for (const int curve_i : geom.curves_range()) {
    const IndexRange curve_points = points_by_curve[curve_i];
    for (const int pt_i : curve_points) {
      if (paintcurve_geom_get_selection(geom, pt_i) & 0x07) {
        split_indices_per_curve[curve_i].append(pt_i - curve_points.first());
      }
    }
    if (!split_indices_per_curve[curve_i].is_empty()) {
      std::sort(split_indices_per_curve[curve_i].begin(),
                split_indices_per_curve[curve_i].end());
    }
  }

  bool has_splits = false;
  for (const Vector<int> &split_pts : split_indices_per_curve) {
    if (!split_pts.is_empty()) {
      has_splits = true;
      break;
    }
  }
  if (!has_splits) {
    return false;
  }

  Vector<int> new_curve_offsets({0});
  Vector<IndexRange> src_ranges;
  Vector<int> dst_offsets({0});
  Vector<int> dst_to_src_curve;
  Vector<bool> dst_cyclic;
  Vector<int2> split_groups;

  for (const int curve_i : geom.curves_range()) {
    const IndexRange curve_points = points_by_curve[curve_i];
    const Span<int> split_pts = split_indices_per_curve[curve_i];

    if (split_pts.is_empty()) {
      src_ranges.append(curve_points);
      dst_offsets.append(dst_offsets.last() + curve_points.size());
      new_curve_offsets.append(new_curve_offsets.last() + curve_points.size());
      dst_to_src_curve.append(curve_i);
      dst_cyclic.append(src_cyclic[curve_i]);
      continue;
    }

    /* Original curve truncated to the first selected point (inclusive). */
    const int group_start = dst_to_src_curve.size();
    const IndexRange truncated = curve_points.take_front(split_pts[0] + 1);
    src_ranges.append(truncated);
    dst_offsets.append(dst_offsets.last() + truncated.size());
    new_curve_offsets.append(new_curve_offsets.last() + truncated.size());
    dst_to_src_curve.append(curve_i);
    dst_cyclic.append(false);

    /* Each selected point starts a new spline copied through the original end. */
    for (const int local_split : split_pts) {
      const IndexRange tail = curve_points.drop_front(local_split);
      src_ranges.append(tail);
      dst_offsets.append(dst_offsets.last() + tail.size());
      new_curve_offsets.append(new_curve_offsets.last() + tail.size());
      dst_to_src_curve.append(curve_i);
      dst_cyclic.append(false);
    }
    split_groups.append(int2(group_start, 1 + split_pts.size()));
  }

  const int new_curves_num = dst_to_src_curve.size();
  const int new_points_num = dst_offsets.last();
  if (new_curves_num == geom.curves_num() && new_points_num == geom.points_num()) {
    return false;
  }

  bke::CurvesGeometry new_geom = bke::CurvesGeometry(new_points_num, new_curves_num);
  array_utils::copy(new_curve_offsets.as_span(), new_geom.offsets_for_write());

  bke::MutableAttributeAccessor dst_attributes = new_geom.attributes_for_write();
  const bke::AttributeAccessor src_attributes = geom.attributes();

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         dst_to_src_curve,
                         dst_attributes);

  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes, dst_attributes, {bke::AttrDomain::Point}, {}))
  {
    bke::attribute_math::gather_ranges_to_groups(
        src_ranges.as_span(), dst_offsets.as_span(), attribute.src, attribute.dst.span);
    attribute.dst.finish();
  }

  if (!(src_cyclic.is_single() && !src_cyclic.get_internal_single())) {
    array_utils::copy(dst_cyclic.as_span(), new_geom.cyclic_for_write());
  }

  new_geom.fill_curve_types(CURVE_TYPE_BEZIER);
  new_geom.resolution_for_write().fill(PAINT_CURVE_NUM_SEGMENTS);

  int selected_curve = -1;
  for (const int2 &group : split_groups) {
    const int group_start = group.x;
    const int group_num = group.y;
    if (selected_curve < 0 && group_num > 1) {
      selected_curve = group_start + 1;
    }
    for (const int i : IndexRange(group_num - 1)) {
      paintcurve_separate_split_curve_endpoints(new_geom, group_start + i, group_start + i + 1);
    }
  }

  paintcurve_geom_set_all_selection(new_geom, 0);
  if (selected_curve >= 0) {
    const IndexRange selected_points = new_geom.points_by_curve()[selected_curve];
    for (const int point_i : selected_points) {
      paintcurve_geom_set_selection(new_geom, point_i, 0x07);
    }
  }

  new_geom.calculate_bezier_auto_handles();
  new_geom.calculate_bezier_aligned_handles();
  new_geom.tag_topology_changed();

  geom = std::move(new_geom);
  if (r_selected_curve != nullptr) {
    *r_selected_curve = selected_curve;
  }
  return true;
}

bool paintcurve_geometry_merge_curve_endpoints(bke::CurvesGeometry &geom,
                                               const int curve_a,
                                               const bool a_is_start,
                                               const int curve_b,
                                               const bool b_is_start)
{
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }
  if (curve_a == curve_b) {
    return false;
  }

  int extend_curve;
  int remove_curve;
  bool reverse_removed;

  if (!a_is_start && b_is_start) {
    extend_curve = curve_a;
    remove_curve = curve_b;
    reverse_removed = false;
  }
  else if (a_is_start && !b_is_start) {
    extend_curve = curve_b;
    remove_curve = curve_a;
    reverse_removed = false;
  }
  else if (!a_is_start && !b_is_start) {
    extend_curve = curve_a;
    remove_curve = curve_b;
    reverse_removed = true;
  }
  else {
    extend_curve = curve_b;
    remove_curve = curve_a;
    reverse_removed = true;
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  if (!points_by_curve.index_range().contains(extend_curve) ||
      !points_by_curve.index_range().contains(remove_curve))
  {
    return false;
  }

  const IndexRange extend_points = points_by_curve[extend_curve];
  const IndexRange remove_points = points_by_curve[remove_curve];
  if (extend_points.is_empty() || remove_points.is_empty()) {
    return false;
  }

  if (reverse_removed) {
    IndexMaskMemory reverse_memory;
    const IndexMask reverse_mask = IndexMask::from_indices<int>(
        Span<int>(&remove_curve, 1), reverse_memory);
    geom.reverse_curves(reverse_mask);
  }

  const int append_count = remove_points.size();
  const int old_points_num = geom.points_num();
  const int old_curves_num = geom.curves_num();

  geom.resize(old_points_num + append_count, old_curves_num);

  MutableSpan<int> offsets = geom.offsets_for_write();
  offsets[extend_curve + 1] += append_count;
  for (const int offset_i : offsets.index_range().drop_front(extend_curve + 2)) {
    offsets[offset_i] += append_count;
  }

  bke::MutableAttributeAccessor attributes = geom.attributes_for_write();
  attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain != bke::AttrDomain::Point ||
        iter.storage_type == bke::AttrStorageType::Single)
    {
      return;
    }
    bke::GSpanAttributeWriter dst_writer = attributes.lookup_for_write_span(iter.name);
    if (!dst_writer) {
      return;
    }
    paintcurve_copy_point_range_to_dst(dst_writer.span,
                                       remove_points,
                                       dst_writer.span,
                                       old_points_num);
    dst_writer.finish();
  });

  IndexMaskMemory memory;
  const IndexMask remove_mask = IndexMask::from_indices<int>(
      Span<int>(&remove_curve, 1), memory);
  geom.remove_curves(remove_mask, {});

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Entry Points
 *
 * Thin wrappers for callers outside this module -- today only the RNA layer, which cannot include
 * the module-private `paint_curve_intern.hh`. Declared in `ED_paint.hh`.
 * \{ */

void ED_paintcurve_geometry_update_after_edit(PaintCurve *pc)
{
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  /* Both calls silently early-out when the handle POSITION attributes were never created, so a
   * geometry that only ever had handle types is left alone rather than half-updated. */
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();
}

int ED_paintcurve_geometry_add_point(PaintCurve *pc, const float position[3], const float radius)
{
  if (pc == nullptr) {
    return -1;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  const bool create_new_spline = !paintcurve_geometry_is_valid(geom);

  int active_curve = create_new_spline ? 0 : paintcurve_active_curve_get(pc);
  /* Append, rather than insert wherever the last interactive click left `add_index`: a caller
   * reached through this function has no way to see or set that cursor, so "add a point" can only
   * sensibly mean "at the end of the active spline". */
  int add_index = create_new_spline ? 0 : int(geom.points_by_curve()[active_curve].size());

  paintcurve_geometry_add_point(
      geom, float3(position), float3(0.0f, 0.0f, 1.0f), create_new_spline, active_curve, add_index);

  pc->active_curve = active_curve;
  pc->add_index = add_index;

  const int point_index = geom.points_by_curve()[active_curve].last();
  /* `paintcurve_geometry_add_point()` fills 1.0, this overrides it with the caller's value. */
  geom.radius_for_write()[point_index] = radius;

  /* AUTO handles, where the interactive click leaves ALIGN ones sitting on top of the point. A
   * click is only the first half of a gesture -- the user drags the handle out next -- while a
   * script hands over positions and nothing else, and ALIGN handles left at the point would make
   * the curve a polyline. This also makes the recompute in
   * #ED_paintcurve_geometry_update_after_edit() meaningful: it derives AUTO handles from the
   * neighbors, and has nothing to derive for ALIGN ones. Same choice as
   * `curve_patch_control_curve_from_points()` makes for its own control curves. */
  geom.handle_types_left_for_write()[point_index] = BEZIER_HANDLE_AUTO;
  geom.handle_types_right_for_write()[point_index] = BEZIER_HANDLE_AUTO;
  geom.calculate_bezier_auto_handles();
  geom.tag_positions_changed();

  return point_index;
}

void ED_paintcurve_geometry_clear(PaintCurve *pc)
{
  if (pc == nullptr || !paintcurve_geometry_runtime_is_initialized(pc->geometry.wrap())) {
    return;
  }
  /* Reuses the bezier initializer with zero points rather than resizing in place: it is the one
   * path that leaves an empty geometry in the exact state the paint curve operators expect. */
  paintcurve_geometry_init_bezier(pc->geometry.wrap(), 0);
  pc->active_curve = 0;
  pc->add_index = 0;
}

void ED_paintcurve_geometry_points_set(PaintCurve *pc,
                                       const Span<float3> positions,
                                       const Span<float> radii,
                                       const bool cyclic)
{
  if (pc == nullptr) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  /* Leaves exactly the state the paint curve operators expect of a fresh curve: one bezier spline,
   * the paint-curve resolution and AUTO handle types. It reassigns the geometry outright, which is
   * what makes this a rebuild rather than an edit -- every previous attribute goes with it. */
  paintcurve_geometry_init_bezier(geom, int(positions.size()));
  pc->active_curve = 0;
  pc->add_index = int(positions.size());
  if (positions.is_empty()) {
    return;
  }

  geom.positions_for_write().copy_from(positions);
  MutableSpan<float> dst_radii = geom.radius_for_write();
  if (radii.is_empty()) {
    dst_radii.fill(1.0f);
  }
  else {
    dst_radii.copy_from(radii);
  }
  if (cyclic) {
    geom.cyclic_for_write().fill(true);
  }

  /* The same order #ED_paintcurve_control_curve_for_patch relies on: the handle position attributes
   * have to exist before the recompute, which otherwise returns silently. One call for the whole
   * curve is the entire point of this function. */
  geom.handle_positions_left_for_write();
  geom.handle_positions_right_for_write();
  geom.calculate_bezier_auto_handles();
  geom.tag_positions_changed();
}

bke::CurvesGeometry ED_paintcurve_control_curve_for_patch(const PaintCurve &pc,
                                                          const int spline_index)
{
  const bke::CurvesGeometry &src = pc.geometry.wrap();
  if (!paintcurve_geometry_is_valid(src) || src.curves_num() == 0) {
    return {};
  }
  const int index = spline_index < 0 ? paintcurve_active_curve_get(&pc) :
                                       std::clamp(spline_index, 0, src.curves_num() - 1);

  /* The same call the Separate Geometry node's curve branch makes
   * (`geometry/intern/separate_geometry.cc`): it carries both domains over whole -- radius, handle
   * positions and types, `.selection`, `.cyclic`, resolution, and whatever attribute a script
   * added -- which is what a hand-written gather would have to be kept in sync with forever. */
  bke::CurvesGeometry curve = bke::curves_copy_curve_selection(
      src, IndexMask(IndexRange(index, 1)), {});

  /* Order matters: the handle position attributes have to exist before the two recomputes, which
   * otherwise return silently and leave the bezier collapsed at the origin. AUTO and ALIGNED
   * handles are derived, FREE ones are left as the user set them. */
  curve.handle_positions_left_for_write();
  curve.handle_positions_right_for_write();
  curve.calculate_bezier_auto_handles();
  curve.calculate_bezier_aligned_handles();
  curve.tag_positions_changed();
  /* Paint curves carry a per-point radius on this codebase's own convention (1.0 = full brush
   * size), but a curve that reached #PaintCurve by some other route may not, and
   * #blender::bke::CurvesGeometry::radius() then answers its generic hair-curve default of 0.01. */
  if (!curve.attributes().contains("radius")) {
    curve.radius_for_write().fill(1.0f);
  }
  return curve;
}

/** \} */

}  // namespace blender
