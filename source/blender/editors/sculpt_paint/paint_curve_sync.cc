/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Screen-space projection of paint-curve geometry and radius-handle helpers.
 */

#include "DNA_brush_types.h"
#include "DNA_object_types.h"

#include "BKE_curves.hh"

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
        obj_co = handles_left.value()[i];
      }
      else if (j == 2) {
        obj_co = handles_right.value()[i];
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

/** \} */

}  // namespace blender
