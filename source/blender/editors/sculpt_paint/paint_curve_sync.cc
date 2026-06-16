/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Screen-space projection of paint-curve geometry and radius-handle helpers.
 */

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_curve.hh"
#include "BKE_curve_legacy_convert.hh"
#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "ED_view3d.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

#include <optional>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Screen-Space Projection & Radius Handles
 * \{ */

void paintcurve_build_screen_points(const PaintCurve *pc,
                                    const ViewContext *vc,
                                    Vector<PaintCurvePoint> &r_screen_points)
{
  if (pc == nullptr) {
    r_screen_points.clear();
    return;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    r_screen_points.clear();
    return;
  }

  const int point_num = geom.points_num();
  r_screen_points.reinitialize(point_num);

  Object *ob = vc ? vc->obact : nullptr;
  const float (*ob_to_world)[4] = ob ? ob->object_to_world().ptr() : nullptr;

  const Span<float3> positions = geom.positions();
  const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
  const std::optional<Span<float3>> handles_right = geom.handle_positions_right();

  for (const int i : IndexRange(point_num)) {
    PaintCurvePoint &pcp = r_screen_points[i];
    pcp = PaintCurvePoint{};

    pcp.bez.h1 = eBezTriple_Handle(geom.handle_types_left()[i]);
    pcp.bez.h2 = eBezTriple_Handle(geom.handle_types_right()[i]);

    const uint8_t sel = paintcurve_geom_get_selection(geom, i);
    pcp.bez.f1 = (sel & 0x01) ? BEZT_FLAG_SELECT : eBezTriple_Flag(0);
    pcp.bez.f2 = (sel & 0x02) ? BEZT_FLAG_SELECT : eBezTriple_Flag(0);
    pcp.bez.f3 = (sel & 0x04) ? BEZT_FLAG_SELECT : eBezTriple_Flag(0);

    pcp.bez.radius = paintcurve_get_point_radius(pc, i);

    for (const int j : IndexRange(3)) {
      float3 obj_co;
      if (j == 0) {
        /* Use handle if available, otherwise fall back to point position. */
        obj_co = handles_left.has_value() ? handles_left.value()[i] : positions[i];
      }
      else if (j == 2) {
        /* Use handle if available, otherwise fall back to point position. */
        obj_co = handles_right.has_value() ? handles_right.value()[i] : positions[i];
      }
      else {
        obj_co = positions[i];
      }
      float world_co[3];
      if (ob_to_world) {
        mul_v3_m4v3(world_co, ob_to_world, obj_co);
      }
      else {
        copy_v3_v3(world_co, obj_co);
      }
      float screen_co[2];
      if (vc && vc->region) {
        ED_view3d_project_v2(vc->region, world_co, screen_co);
        /* Guard against NaN/Inf from points behind or at the camera plane.
         * Corrupted GPU vertex coordinates can crash the GPU driver on Windows. */
        if (!isfinite(screen_co[0]) || !isfinite(screen_co[1])) {
          screen_co[0] = -1e6f;
          screen_co[1] = -1e6f;
        }
      }
      else {
        zero_v2(screen_co);
      }
      pcp.bez.vec[j][0] = screen_co[0];
      pcp.bez.vec[j][1] = screen_co[1];
    }
  }
}

int paintcurve_find_point_index(const Span<PaintCurvePoint> screen_points,
                                const float pos[2],
                                const float threshold)
{
  if (screen_points.is_empty()) {
    return -1;
  }

  int best_index = -1;
  float best_dist = threshold;

  for (const int i : screen_points.index_range()) {
    const float dist = len_v2v2(pos, screen_points[i].bez.vec[1]);
    if (dist < best_dist) {
      best_dist = dist;
      best_index = i;
    }
  }

  return best_index;
}

void paintcurve_radius_handle_screen_get(const PaintCurve *pc,
                                         const PaintCurvePoint *screen_points,
                                         const int point_index,
                                         PaintCurveRadiusHandleScreen *r_handle)
{
  const BezTriple &bez = screen_points[point_index].bez;
  const float2 pivot(bez.vec[1]);
  float2 tangent = float2(bez.vec[2]) - float2(bez.vec[0]);
  if (math::length_squared(tangent) < 1e-6f) {
    tangent = float2(1.0f, 0.0f);
  }
  else {
    tangent = math::normalize(tangent);
  }

  const float2 perp(-tangent.y, tangent.x);
  const float radius = paintcurve_get_point_radius(pc, point_index);
  /* Offset the endpoint by a fixed minimum so it never collapses onto the pivot. Without this a
   * near-zero radius would draw and hit-test the handle directly on top of the pivot, shadowing
   * every click meant to grab the pivot itself. */
  const float len = PAINT_CURVE_RADIUS_HANDLE_MIN_OFFSET + radius * PAINT_CURVE_RADIUS_HANDLE_BASE_LEN;

  r_handle->point = pivot;
  r_handle->perp = perp;
  r_handle->end = pivot + perp * len;
}

int paintcurve_find_radius_handle_at_pos(const PaintCurve *pc,
                                         const PaintCurvePoint *screen_points,
                                         const float pos[2],
                                         const float threshold)
{
  if (pc == nullptr || screen_points == nullptr) {
    return -1;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return -1;
  }

  int best_index = -1;
  float best_dist = threshold;

  for (const int i : IndexRange(geom.points_num())) {
    PaintCurveRadiusHandleScreen handle;
    paintcurve_radius_handle_screen_get(pc, screen_points, i, &handle);

    const float end[2] = {handle.end.x, handle.end.y};
    const float dist_end = len_v2v2(pos, end);
    if (dist_end < best_dist) {
      best_dist = dist_end;
      best_index = i;
    }
  }

  return best_index;
}

float paintcurve_radius_from_handle_screen_pos(const PaintCurveRadiusHandleScreen *handle,
                                               const float pos[2])
{
  const float2 delta = float2(pos) - handle->point;
  const float dist = math::dot(delta, handle->perp);
  /* Invert the forward mapping in paintcurve_radius_handle_screen_get, which offsets the endpoint
   * by a fixed minimum so the handle clears the pivot. */
  return max_ff((dist - PAINT_CURVE_RADIUS_HANDLE_MIN_OFFSET) / PAINT_CURVE_RADIUS_HANDLE_BASE_LEN,
                0.0f);
}

void paintcurve_build_object_screen_polylines(const bke::CurvesGeometry &geom,
                                              const float4x4 &ob_to_world,
                                              const ViewContext *vc,
                                              Vector<Vector<float2>> &r_polylines)
{
  r_polylines.clear();
  if (!paintcurve_geometry_is_valid(geom) || vc == nullptr || vc->region == nullptr) {
    return;
  }

  const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
  const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
  if (!handles_left.has_value() || !handles_right.has_value()) {
    return;
  }

  const Span<float3> positions = geom.positions();
  const Span<float3> hl = handles_left.value();
  const Span<float3> hr = handles_right.value();
  const VArray<bool> cyclic = geom.cyclic();
  const OffsetIndices<int> points_by_curve = geom.points_by_curve();

  auto project = [&](const float3 &local_co, float2 &r_screen) -> bool {
    float world_co[3];
    mul_v3_m4v3(world_co, ob_to_world.ptr(), local_co);
    float screen_co[2];
    ED_view3d_project_v2(vc->region, world_co, screen_co);
    if (!isfinite(screen_co[0]) || !isfinite(screen_co[1])) {
      return false;
    }
    r_screen = float2(screen_co[0], screen_co[1]);
    return true;
  };

  for (const int curve_i : points_by_curve.index_range()) {
    const IndexRange pts = points_by_curve[curve_i];
    if (pts.size() < 2) {
      continue;
    }
    Vector<float2> polyline;
    const int segment_num = cyclic[curve_i] ? int(pts.size()) : int(pts.size()) - 1;

    for (int seg = 0; seg < segment_num; seg++) {
      const int a = pts[seg];
      const int b = pts[(seg + 1) % int(pts.size())];

      float data[(PAINT_CURVE_NUM_SEGMENTS + 1) * 2];
      for (int j = 0; j < 2; j++) {
        float2 p1, h1, h2, p2;
        if (!project(positions[a], p1) || !project(hr[a], h1) ||
            !project(hl[b], h2) || !project(positions[b], p2))
        {
          goto next_segment;
        }
        BKE_curve_forward_diff_bezier(p1[j],
                                      h1[j],
                                      h2[j],
                                      p2[j],
                                      data + j,
                                      PAINT_CURVE_NUM_SEGMENTS,
                                      sizeof(float[2]));
      }

      {
        float (*v)[2] = reinterpret_cast<float (*)[2]>(data);
        const int start = polyline.is_empty() ? 0 : 1;
        for (int k = start; k <= PAINT_CURVE_NUM_SEGMENTS; k++) {
          polyline.append(float2(v[k][0], v[k][1]));
        }
      }
    next_segment:;
    }

    if (polyline.size() >= 2) {
      r_polylines.append(std::move(polyline));
    }
  }
}

/* Shortest distance from `p` to segment [a, b] in screen space. */
static float paintcurve_dist_point_segment(const float2 p, const float2 a, const float2 b)
{
  const float2 ab = b - a;
  const float len_sq = math::length_squared(ab);
  if (len_sq < 1e-8f) {
    return math::distance(p, a);
  }
  const float t = math::clamp(math::dot(p - a, ab) / len_sq, 0.0f, 1.0f);
  return math::distance(p, a + ab * t);
}

Object *paintcurve_nearest_scene_curve(const ViewContext *vc,
                                       const float2 mval,
                                       const float threshold,
                                       const Object *exclude,
                                       Vector<Vector<float2>> *r_polylines)
{
  if (vc == nullptr || vc->scene == nullptr || vc->view_layer == nullptr) {
    return nullptr;
  }

  BKE_view_layer_synced_ensure(*vc->bmain, vc->scene, vc->view_layer);

  Object *best_ob = nullptr;
  float best_dist = threshold;
  Vector<Vector<float2>> best_polylines;
  Vector<Vector<float2>> scratch;

  for (Base &base : *BKE_view_layer_object_bases_get(vc->view_layer)) {
    Object *ob = base.object;
    if (ob == exclude || !ELEM(ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
      continue;
    }
    if ((base.flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) == 0) {
      continue;
    }

    Curves *temp_curves = nullptr;
    const bke::CurvesGeometry *geom = nullptr;
    if (ob->type == OB_CURVES) {
      geom = &id_cast<const Curves *>(ob->data)->geometry.wrap();
    }
    else {
      temp_curves = bke::curve_legacy_to_curves(*id_cast<const Curve *>(ob->data));
      if (temp_curves) {
        geom = &temp_curves->geometry.wrap();
      }
    }

    if (geom) {
      paintcurve_build_object_screen_polylines(*geom, ob->object_to_world(), vc, scratch);
      for (const Vector<float2> &polyline : scratch) {
        for (const int i : IndexRange(polyline.size() - 1)) {
          const float d = paintcurve_dist_point_segment(mval, polyline[i], polyline[i + 1]);
          if (d < best_dist) {
            best_dist = d;
            best_ob = ob;
          }
        }
      }
      if (best_ob == ob && r_polylines) {
        best_polylines = scratch;
      }
    }

    if (temp_curves) {
      BKE_id_free(nullptr, &temp_curves->id);
    }
  }

  if (r_polylines && best_ob) {
    *r_polylines = std::move(best_polylines);
  }
  return best_ob;
}

/** \} */

}  // namespace blender
