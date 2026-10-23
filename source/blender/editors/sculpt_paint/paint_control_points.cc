/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared control-point screen layer; see `ED_control_points.hh`.
 */

#include "ED_control_points.hh"

#include <cfloat>

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "DNA_curve_types.h"
#include "DNA_space_types.h"

#include "UI_resources.hh"

namespace blender::ed::sculpt_paint::control_points {

namespace {

float point_distance(const float2 &a, const float2 &b, const Metric metric)
{
  if (metric == Metric::Manhattan) {
    return math::abs(a.x - b.x) + math::abs(a.y - b.y);
  }
  return math::distance(a, b);
}

/* A hidden handle is never the closest one. */
float handle_distance(
    const float2 &handle, const bool visible, const float2 &pos, const Metric metric)
{
  return visible ? point_distance(handle, pos, metric) : FLT_MAX;
}

}  // namespace

void handles_build(const Source &source,
                   const BuildOptions &options,
                   PaintCurveScreenHandles &r_out)
{
  r_out.points.clear();
  r_out.segments.clear();

  const int point_num = source.points_num();
  r_out.points.reserve(point_num);
  for (const int i : IndexRange(point_num)) {
    PaintCurveHandleDrawData hd;
    hd.position = source.point_position(i);
    source.point_handles(i, hd.handle_left, hd.handle_right);
    hd.h1 = source.handle_type(i, true);
    hd.h2 = source.handle_type(i, false);
    hd.show_handle_left = source.handle_visible(i, true);
    hd.show_handle_right = source.handle_visible(i, false);

    const uint8_t selection = source.selection(i);
    hd.selected_center = (selection & SEL_F2) != 0;
    /* A selected control point highlights both of its handles too, like the curves editor. */
    hd.selected_left = (selection & SEL_F1) != 0 || hd.selected_center;
    hd.selected_right = (selection & SEL_F3) != 0 || hd.selected_center;

    /* Zero rather than a stale value for a caller that only picks and never draws. */
    hd.color_left = float4(0.0f);
    hd.color_right = float4(0.0f);
    if (options.compute_colors) {
      handle_color_get(hd.h1, hd.selected_left, hd.color_left);
      handle_color_get(hd.h2, hd.selected_right, hd.color_right);
    }

    r_out.points.append(hd);
  }

  const int segment_num = source.segments_num();
  r_out.segments.reserve(segment_num);
  float wire_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  if (options.compute_colors) {
    ui::theme::get_color_type_4fv(TH_WIRE, SPACE_VIEW3D, wire_color);
  }
  for (const int i : IndexRange(segment_num)) {
    PaintCurveSegmentDrawData seg;
    source.segment_polyline(i, seg.polyline);
    if (seg.polyline.size() < 2) {
      continue;
    }
    copy_v4_v4(seg.wire_color, wire_color);
    seg.outline_color = float4(0.0f, 0.0f, 0.0f, 0.5f);
    r_out.segments.append(std::move(seg));
  }

  /* Hover rules. A hit on any part of a control point suppresses the wire highlight, as the
   * Curve Patch has always done: the marker under the cursor is the more precise affordance. */
  bool point_hit = false;
  if (options.hover_point || options.hover_segment) {
    const Pick pick = pick_point(
        r_out.points, options.cursor, options.point_hover_radius, options.hover_metric);
    point_hit = pick.point >= 0;
    if (pick.point >= 0 && pick.part == Part::Point) {
      r_out.points[pick.point].hovered_center = true;
    }
  }
  if (options.hover_segment && !point_hit) {
    int segment = -1;
    float t = 0.0f;
    if (pick_segment(
            r_out.segments, options.cursor, options.segment_hover_radius, segment, t))
    {
      r_out.segments[segment].hovered = true;
    }
  }
}

Pick pick_point(const int points_num,
                const FunctionRef<PointGeometry(int)> point_get,
                const float2 pos,
                const float threshold,
                const Metric metric,
                const bool ignore_pivot)
{
  Pick result;
  float closest = threshold;
  PointGeometry result_point;
  for (const int i : IndexRange(points_num)) {
    const PointGeometry point = point_get(i);
    const float dist_center = point_distance(point.position, pos, metric);
    const float dist_left = handle_distance(point.handle_left, point.show_left, pos, metric);
    const float dist_right = handle_distance(point.handle_right, point.show_right, pos, metric);

    Part part = Part::None;
    float dist = closest;
    if (dist_center < dist) {
      dist = dist_center;
      part = Part::Point;
    }
    if (dist_left < dist) {
      dist = dist_left;
      part = Part::Left;
    }
    if (dist_right < dist) {
      dist = dist_right;
      part = Part::Right;
    }
    if (part != Part::None) {
      closest = dist;
      result.point = i;
      result.part = part;
      result.distance = dist;
      result_point = point;
    }
  }

  if (result.point >= 0 && ignore_pivot && result.part == Part::Point) {
    const float dist_left = handle_distance(
        result_point.handle_left, result_point.show_left, pos, metric);
    const float dist_right = handle_distance(
        result_point.handle_right, result_point.show_right, pos, metric);
    result.part = (dist_left < dist_right) ? Part::Left : Part::Right;
  }
  return result;
}

Pick pick_point(const Span<PaintCurveHandleDrawData> points,
                const float2 pos,
                const float threshold,
                const Metric metric,
                const bool ignore_pivot)
{
  return pick_point(
      int(points.size()),
      [&](const int i) {
        const PaintCurveHandleDrawData &hd = points[i];
        return PointGeometry{hd.position,
                             hd.handle_left,
                             hd.handle_right,
                             hd.show_handle_left,
                             hd.show_handle_right};
      },
      pos,
      threshold,
      metric,
      ignore_pivot);
}

Pick pick_point(const Source &source,
                const float2 pos,
                const float threshold,
                const Metric metric,
                const bool ignore_pivot)
{
  return pick_point(
      source.points_num(),
      [&](const int i) {
        PointGeometry point;
        point.position = source.point_position(i);
        source.point_handles(i, point.handle_left, point.handle_right);
        point.show_left = source.handle_visible(i, true);
        point.show_right = source.handle_visible(i, false);
        return point;
      },
      pos,
      threshold,
      metric,
      ignore_pivot);
}

bool pick_segment(const Span<PaintCurveSegmentDrawData> segments,
                  const float2 pos,
                  const float threshold,
                  int &r_segment,
                  float &r_t,
                  float *r_distance_sq)
{
  r_segment = -1;
  r_t = 0.0f;
  if (r_distance_sq != nullptr) {
    *r_distance_sq = FLT_MAX;
  }
  float best_dist_sq = square_f(threshold);
  PaintCurvePolylineHit best_hit;

  for (const int i : segments.index_range()) {
    PaintCurvePolylineHit hit;
    if (ED_paint_curve_polyline_closest(segments[i].polyline, pos, hit) &&
        hit.dist_sq < best_dist_sq)
    {
      best_dist_sq = hit.dist_sq;
      best_hit = hit;
      r_segment = i;
    }
  }

  if (r_segment < 0) {
    return false;
  }
  r_t = best_hit.arc_t;
  if (r_distance_sq != nullptr) {
    *r_distance_sq = best_dist_sq;
  }
  return true;
}

void handle_color_get(const int8_t handle_type, const bool selected, float r_color[4])
{
  int color;
  if (selected) {
    switch (handle_type) {
      case BEZIER_HANDLE_AUTO:
        color = TH_HANDLE_SEL_AUTO;
        break;
      case BEZIER_HANDLE_VECTOR:
        color = TH_HANDLE_SEL_VECT;
        break;
      case BEZIER_HANDLE_ALIGN:
        color = TH_HANDLE_SEL_ALIGN;
        break;
      default:
        color = TH_HANDLE_SEL_FREE;
        break;
    }
  }
  else {
    switch (handle_type) {
      case BEZIER_HANDLE_AUTO:
        color = TH_HANDLE_AUTO;
        break;
      case BEZIER_HANDLE_VECTOR:
        color = TH_HANDLE_VECT;
        break;
      case BEZIER_HANDLE_ALIGN:
        color = TH_HANDLE_ALIGN;
        break;
      default:
        color = TH_HANDLE_FREE;
        break;
    }
  }
  ui::theme::get_color_type_4fv(color, SPACE_VIEW3D, r_color);
}

}  // namespace blender::ed::sculpt_paint::control_points
