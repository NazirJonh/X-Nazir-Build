/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Bridge between the legacy 2D #PaintCurve::points array and the authoritative 3D
 * #PaintCurve::geometry, plus the screen-space projection used by the cursor and the
 * radius handles.
 *
 * Everything here exists only because the legacy 2D representation is still kept in sync.
 * Once paint curves are fully 3D this translation unit is meant to be removed wholesale,
 * so new 3D-only logic should not be added here.
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_object_types.h"

#include "BKE_curves.hh"

#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "ED_view3d.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

#include <algorithm>
#include <optional>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Legacy Point Array <-> 3D Geometry
 * \{ */

static void paintcurve_ensure_legacy_points(PaintCurve *pc, const int point_num)
{
  if (point_num <= 0) {
    MEM_SAFE_DELETE(pc->points);
    pc->points = nullptr;
    pc->tot_points = 0;
    return;
  }
  if (pc->tot_points == point_num && pc->points != nullptr) {
    return;
  }

  PaintCurvePoint *points_new = MEM_new_array<PaintCurvePoint>(point_num, "PaintCurvePoint");
  const int copy_count = pc->points ? (pc->tot_points < point_num ? pc->tot_points : point_num) : 0;
  for (int i = 0; i < copy_count; i++) {
    points_new[i] = pc->points[i];
  }
  for (int i = copy_count; i < point_num; i++) {
    points_new[i] = PaintCurvePoint{};
  }
  MEM_SAFE_DELETE(pc->points);
  pc->points = points_new;
  pc->tot_points = point_num;
}

static void paintcurve_geometry_project_legacy_points_to_3d(PaintCurve *pc,
                                                            const ViewContext *vc,
                                                            bke::CurvesGeometry &geom)
{
  float ob_origin_world[3];
  copy_v3_v3(ob_origin_world, vc->obact->object_to_world().location());
  const float (*world_to_ob)[4] = vc->obact->world_to_object().ptr();

  for (int i = 0; i < pc->tot_points; i++) {
    for (int j = 0; j < 3; j++) {
      const float *screen_co_2d = pc->points[i].bez.vec[j];
      const float mval_fl[2] = {screen_co_2d[0], screen_co_2d[1]};
      float world_co[3];
      ED_view3d_win_to_3d(vc->v3d, vc->region, ob_origin_world, mval_fl, world_co);
      mul_v3_m4v3(paintcurve_geom_co(geom, i, j), world_to_ob, world_co);
    }
    geom.handle_types_left_for_write()[i] = pc->points[i].bez.h1;
    geom.handle_types_right_for_write()[i] = pc->points[i].bez.h2;
    geom.radius_for_write()[i] = max_ff(pc->points[i].bez.radius, 0.0f);
  }
  geom.tag_positions_changed();
}

void paintcurve_geometry_from_2d(PaintCurve *pc, const ViewContext *vc)
{
  if (pc->tot_points == 0) {
    return;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  /* Keep existing curve topology (including multiple splines); only update positions. */
  if (geom.runtime != nullptr && geom.curves_num() > 0 && geom.points_num() > 0) {
    paintcurve_ensure_legacy_points(pc, geom.points_num());
    paintcurve_geometry_project_legacy_points_to_3d(pc, vc, geom);
    return;
  }

  bool preserve_cyclic = false;
  if (geom.runtime != nullptr && geom.curves_num() > 0) {
    preserve_cyclic = geom.cyclic()[0];
  }

  paintcurve_geometry_init_bezier(geom, pc->tot_points);
  if (preserve_cyclic) {
    geom.cyclic_for_write()[0] = true;
    geom.tag_topology_changed();
  }

  paintcurve_geometry_project_legacy_points_to_3d(pc, vc, geom);
}

void paintcurve_ensure_3d_geometry(PaintCurve *pc, const ViewContext *vc)
{
  /* Lazy-initialize from legacy 2D points when `use_3d_space` was enabled on a curve that
   * predates the 3D representation — the geometry array is empty or stale in that case. */
  if (pc->tot_points == 0) {
    return;
  }
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime != nullptr && geom.curves_num() > 0 && geom.points_num() == pc->tot_points) {
    return;
  }
  paintcurve_geometry_from_2d(pc, vc);
}

void paintcurve_sync_geometry_to_2d(PaintCurve *pc,
                                       const ViewContext *vc,
                                       const float ob_to_world[4][4])
{
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.points_num() == 0) {
    return;
  }

  paintcurve_ensure_legacy_points(pc, geom.points_num());
  for (int i = 0; i < pc->tot_points; i++) {
    for (int j = 0; j < 3; j++) {
      float world_co[3];
      mul_v3_m4v3(world_co, ob_to_world, paintcurve_geom_co(geom, i, j));
      float screen_co[2];
      ED_view3d_project_v2(vc->region, world_co, screen_co);
      pc->points[i].bez.vec[j][0] = screen_co[0];
      pc->points[i].bez.vec[j][1] = screen_co[1];
    }
    pc->points[i].bez.h1 = eBezTriple_Handle(geom.handle_types_left()[i]);
    pc->points[i].bez.h2 = eBezTriple_Handle(geom.handle_types_right()[i]);
    /* Legacy bez.radius is authoritative for paint-curve size; keep geometry in sync. */
    geom.radius_for_write()[i] = max_ff(pc->points[i].bez.radius, 0.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radius Sync
 * \{ */

void paintcurve_init_points_radius_from_geometry(PaintCurve *pc)
{
  if (pc == nullptr || pc->points == nullptr || pc->tot_points == 0) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return;
  }

  const int point_num = std::min(pc->tot_points, geom.points_num());
  MutableSpan<float> radii = geom.radius_for_write();
  for (const int i : IndexRange(point_num)) {
    const float mapped = paintcurve_radius_from_source_geometry(geom.radius()[i]);
    radii[i] = mapped;
    pc->points[i].bez.radius = mapped;
  }
}

void paintcurve_sync_geometry_radius_from_points(PaintCurve *pc)
{
  if (pc == nullptr || pc->points == nullptr) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return;
  }

  const int point_num = std::min(pc->tot_points, geom.points_num());
  MutableSpan<float> radii = geom.radius_for_write();
  for (const int i : IndexRange(point_num)) {
    radii[i] = paintcurve_get_point_radius(pc, i);
  }
  for (const int i : IndexRange(point_num, geom.points_num() - point_num)) {
    radii[i] = 1.0f;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen-Space Projection & Radius Handles
 * \{ */

void paintcurve_build_screen_points(const PaintCurve *pc,
                                    const ViewContext *vc,
                                    Vector<PaintCurvePoint> &r_screen_points)
{
  if (pc == nullptr || pc->points == nullptr || pc->tot_points == 0) {
    r_screen_points.clear();
    return;
  }

  r_screen_points.reinitialize(pc->tot_points);
  for (const int i : IndexRange(pc->tot_points)) {
    r_screen_points[i] = pc->points[i];
  }

  if (!paintcurve_uses_3d_geometry(pc)) {
    return;
  }

  Object *ob = vc ? vc->obact : nullptr;
  const float (*ob_to_world)[4] = ob ? ob->object_to_world().ptr() : nullptr;
  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  const Span<float3> positions = geom.positions();
  const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
  const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
  for (const int i : IndexRange(pc->tot_points)) {
    r_screen_points[i].bez.h1 = eBezTriple_Handle(geom.handle_types_left()[i]);
    r_screen_points[i].bez.h2 = eBezTriple_Handle(geom.handle_types_right()[i]);
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
      }
      else {
        zero_v2(screen_co);
      }
      r_screen_points[i].bez.vec[j][0] = screen_co[0];
      r_screen_points[i].bez.vec[j][1] = screen_co[1];
    }
  }
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
  const float len = radius * PAINT_CURVE_RADIUS_HANDLE_BASE_LEN;

  r_handle->point = pivot;
  r_handle->perp = perp;
  r_handle->end = pivot + perp * len;
}

int paintcurve_find_radius_handle_at_pos(const PaintCurve *pc,
                                         const PaintCurvePoint *screen_points,
                                         const float pos[2],
                                         const float threshold)
{
  if (pc == nullptr || screen_points == nullptr || pc->tot_points == 0) {
    return -1;
  }

  int best_index = -1;
  float best_dist = threshold;

  for (const int i : IndexRange(pc->tot_points)) {
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
  return max_ff(dist / PAINT_CURVE_RADIUS_HANDLE_BASE_LEN, 0.0f);
}

/** \} */

}  // namespace blender
