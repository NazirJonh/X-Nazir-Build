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

#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curve_legacy_convert.hh"
#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "ED_view3d.hh"

#include "mesh/sculpt_intern.hh"
#include "paint_curve_intern.hh"
#include "paint_intern.hh"

#include <optional>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Screen-Space Projection & Radius Handles
 * \{ */

static bool paintcurve_project_local_to_screen(const ViewContext *vc,
                                               const float4x4 &ob_to_world,
                                               const float3 &local_co,
                                               float2 &r_screen)
{
  if (vc == nullptr || vc->region == nullptr) {
    return false;
  }
  if (vc->rv3d) {
    const float4x4 projection = ED_view3d_ob_project_mat_get_from_obmat(vc->rv3d, ob_to_world);
    r_screen = ED_view3d_project_float_v2_m4(vc->region, local_co, projection);
  }
  else {
    float world_co[3];
    mul_v3_m4v3(world_co, ob_to_world.ptr(), local_co);
    float screen_co[2];
    ED_view3d_project_v2(vc->region, world_co, screen_co);
    r_screen = float2(screen_co[0], screen_co[1]);
  }
  return isfinite(r_screen.x) && isfinite(r_screen.y);
}

void paintcurve_build_screen_segment_polyline(const PaintCurve *pc,
                                              const ViewContext *vc,
                                              const int point_index_a,
                                              const int point_index_b,
                                              const Span<PaintCurvePoint> screen_points_fallback,
                                              Vector<float2> &r_polyline)
{
  r_polyline.clear();
  if (pc == nullptr || vc == nullptr) {
    return;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return;
  }

  if (paintcurve_uses_3d_geometry(pc)) {
    Object *ob = vc->obact;
    const float4x4 ob_to_world = ob ? ob->object_to_world() : float4x4::identity();

    const int curve_index = paintcurve_curve_of_point(pc, point_index_a);
    if (curve_index < 0) {
      return;
    }

    const OffsetIndices<int> points_by_curve = geom.points_by_curve();
    const IndexRange curve_points = points_by_curve[curve_index];
    if (!curve_points.contains(point_index_a) || !curve_points.contains(point_index_b)) {
      return;
    }

    const int local_a = point_index_a - curve_points.first();
    const int local_b = point_index_b - curve_points.first();
    /* Populate evaluated offsets/positions caches before bezier_evaluated_offsets_for_curve. */
    const Span<float3> evaluated_positions = geom.evaluated_positions();
    const IndexRange curve_evaluated = geom.evaluated_points_by_curve()[curve_index];
    const Span<int> bezier_offsets = geom.bezier_evaluated_offsets_for_curve(curve_index);
    if (local_b >= bezier_offsets.size() || local_a + 1 >= bezier_offsets.size()) {
      return;
    }

    /* Bezier offsets index tessellated spans between control points. The exact end control
     * point of a segment lives at eval index #bezier_offsets[local_b] and must be included
     * (exclusive range end is +1). Only the cyclic wrap-around segment uses the raw offset
     * without the extra point (see #curves::bezier::calculate_evaluated_offsets). */
    const bool is_cyclic = geom.cyclic()[curve_index];
    const bool is_wrap_segment = is_cyclic && local_b <= local_a;
    int range_end = bezier_offsets[local_a + 1];
    if (!is_wrap_segment) {
      range_end = bezier_offsets[local_b] + 1;
    }

    const IndexRange segment_evaluated = IndexRange::from_begin_end(bezier_offsets[local_a],
                                                                    range_end);

    if (segment_evaluated.size() <= 1) {
      /* Vector segment: only one evaluated point is stored; draw the straight edge explicitly. */
      const Span<float3> positions = geom.positions();
      r_polyline.reinitialize(2);
      if (!paintcurve_project_local_to_screen(
              vc, ob_to_world, positions[point_index_a], r_polyline[0]) ||
          !paintcurve_project_local_to_screen(
              vc, ob_to_world, positions[point_index_b], r_polyline[1]))
      {
        r_polyline.clear();
      }
      return;
    }

    r_polyline.reinitialize(segment_evaluated.size());
    for (const int i : segment_evaluated.index_range()) {
      const float3 &local_co = evaluated_positions[curve_evaluated.start() + segment_evaluated[i]];
      if (!paintcurve_project_local_to_screen(vc, ob_to_world, local_co, r_polyline[i])) {
        r_polyline.clear();
        return;
      }
    }
    return;
  }

  if (screen_points_fallback.is_empty() || point_index_a >= screen_points_fallback.size() ||
      point_index_b >= screen_points_fallback.size())
  {
    return;
  }

  const PaintCurvePoint &pcp_a = screen_points_fallback[point_index_a];
  const PaintCurvePoint &pcp_b = screen_points_fallback[point_index_b];
  float data[(PAINT_CURVE_NUM_SEGMENTS + 1) * 2];
  for (int j = 0; j < 2; j++) {
    BKE_curve_forward_diff_bezier(pcp_a.bez.vec[1][j],
                                  pcp_a.bez.vec[2][j],
                                  pcp_b.bez.vec[0][j],
                                  pcp_b.bez.vec[1][j],
                                  data + j,
                                  PAINT_CURVE_NUM_SEGMENTS,
                                  sizeof(float[2]));
  }

  r_polyline.reinitialize(PAINT_CURVE_NUM_SEGMENTS + 1);
  const float(*v)[2] = reinterpret_cast<const float(*)[2]>(data);
  for (int j = 0; j <= PAINT_CURVE_NUM_SEGMENTS; j++) {
    r_polyline[j] = float2(v[j][0], v[j][1]);
  }
}

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

    if (!pc->use_3d_space) {
      /* Viewport-bound curve: geometry stores region pixel coordinates directly. */
      const float3 &pos = positions[i];
      const float3 hl = handles_left.has_value() ? handles_left.value()[i] : pos;
      const float3 hr = handles_right.has_value() ? handles_right.value()[i] : pos;
      pcp.bez.vec[0][0] = hl.x;
      pcp.bez.vec[0][1] = hl.y;
      pcp.bez.vec[1][0] = pos.x;
      pcp.bez.vec[1][1] = pos.y;
      pcp.bez.vec[2][0] = hr.x;
      pcp.bez.vec[2][1] = hr.y;
      continue;
    }

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

bool paintcurve_convert_geometry_space(bContext *C,
                                       PaintCurve *pc,
                                       const ViewContext *vc,
                                       const bool to_3d_space)
{
  if (pc == nullptr || vc == nullptr || vc->region == nullptr || vc->rv3d == nullptr ||
      vc->obact == nullptr)
  {
    return false;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return true;
  }

  const int point_num = geom.points_num();
  Object *ob = vc->obact;
  const float4x4 ob_to_world = ob->object_to_world();
  const float4x4 world_to_ob = ob->world_to_object();
  const float (*ob_to_world_ptr)[4] = ob_to_world.ptr();
  const float (*world_to_ob_ptr)[4] = world_to_ob.ptr();

  if (to_3d_space) {
    float ob_origin_world[3];
    copy_v3_v3(ob_origin_world, ob->object_to_world().location());

    MutableSpan<float3> positions = geom.positions_for_write();
    MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
    MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();

    for (const int i : IndexRange(point_num)) {
      float3 &center = positions[i];
      float3 &left = handles_left[i];
      float3 &right = handles_right[i];

      const float center_mval[2] = {center.x, center.y};
      float pivot_world[3];
      bool has_pivot_world = false;

      if (vc->depsgraph != nullptr && ob->runtime->sculpt_session != nullptr) {
        float hit_obj[3];
        ViewContext vc_ray = *vc;
        ToolSettings *ts = C != nullptr ? CTX_data_tool_settings(C) : nullptr;
        Paint *paint = C != nullptr ? BKE_paint_get_active_from_context(C) : nullptr;
        const Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
        bool hit = false;
        if (ts && ts->sculpt) {
          hit = ed::sculpt_paint::stroke_get_location_bvh(
              *vc->depsgraph, vc_ray, *ts->sculpt, brush, hit_obj, center_mval, false);
        }
        else if (paint) {
          hit = ed::sculpt_paint::stroke_get_location_bvh(
              *vc->depsgraph, vc_ray, *paint, brush, hit_obj, center_mval, false);
        }
        if (hit) {
          center = float3(hit_obj);
          mul_v3_m4v3(pivot_world, ob_to_world_ptr, hit_obj);
          has_pivot_world = true;
        }
      }

      if (!has_pivot_world) {
        ED_view3d_win_to_3d(vc->v3d, vc->region, ob_origin_world, center_mval, pivot_world);
        center = math::transform_point(world_to_ob, float3(pivot_world));
      }

      const float left_mval[2] = {left.x, left.y};
      const float right_mval[2] = {right.x, right.y};
      float world_co[3];
      ED_view3d_win_to_3d(vc->v3d, vc->region, pivot_world, left_mval, world_co);
      left = math::transform_point(world_to_ob, float3(world_co));
      ED_view3d_win_to_3d(vc->v3d, vc->region, pivot_world, right_mval, world_co);
      right = math::transform_point(world_to_ob, float3(world_co));
    }
    geom.calculate_bezier_auto_handles();
    geom.calculate_bezier_aligned_handles();
  }
  else {
    for (const int i : IndexRange(point_num)) {
      for (const int handle_idx : IndexRange(3)) {
        float3 &co = paintcurve_geom_co(geom, i, handle_idx);
        float world_co[3];
        mul_v3_m4v3(world_co, ob_to_world_ptr, co);
        float screen_co[2];
        ED_view3d_project_v2(vc->region, world_co, screen_co);
        if (!isfinite(screen_co[0]) || !isfinite(screen_co[1])) {
          continue;
        }
        co.x = screen_co[0];
        co.y = screen_co[1];
        co.z = 0.0f;
      }
    }
  }

  geom.tag_positions_changed();
  return true;
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

  const Span<float3> evaluated_positions = geom.evaluated_positions();
  const OffsetIndices<int> evaluated_points_by_curve = geom.evaluated_points_by_curve();

  for (const int curve_i : geom.curves_range()) {
    const IndexRange evaluated_points = evaluated_points_by_curve[curve_i];
    if (evaluated_points.size() < 2) {
      continue;
    }

    Vector<float2> polyline;
    polyline.reserve(evaluated_points.size());
    for (const int eval_i : evaluated_points) {
      float2 screen;
      if (!paintcurve_project_local_to_screen(vc, ob_to_world, evaluated_positions[eval_i], screen))
      {
        polyline.clear();
        break;
      }
      polyline.append(screen);
    }

    if (polyline.size() >= 2) {
      r_polylines.append(std::move(polyline));
    }
  }
}

void paintcurve_object_screen_polylines(const ViewContext *vc,
                                        const Object *ob,
                                        Vector<Vector<float2>> &r_polylines)
{
  r_polylines.clear();
  if (ob == nullptr) {
    return;
  }

  Curves *temp_curves = nullptr;
  const bke::CurvesGeometry *geom = nullptr;
  if (ob->type == OB_CURVES) {
    geom = &id_cast<const Curves *>(ob->data)->geometry.wrap();
  }
  else if (ob->type == OB_CURVES_LEGACY) {
    temp_curves = bke::curve_legacy_to_curves(*id_cast<const Curve *>(ob->data));
    if (temp_curves) {
      geom = &temp_curves->geometry.wrap();
    }
  }

  if (geom) {
    paintcurve_build_object_screen_polylines(*geom, ob->object_to_world(), vc, r_polylines);
  }
  if (temp_curves) {
    BKE_id_free(nullptr, &temp_curves->id);
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

    paintcurve_object_screen_polylines(vc, ob, scratch);
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

  if (r_polylines && best_ob) {
    *r_polylines = std::move(best_polylines);
  }
  return best_ob;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cached polyline nearest-distance query
 * \{ */

const Object *paintcurve_nearest_from_cached_polylines(
    const float2 mval,
    const float threshold,
    const Object *exclude,
    Span<ed::sculpt_paint::PaintCurveCachedObjectSilhouette> cached,
    Vector<Vector<float2>> *r_hover_polylines)
{
  const Object *best_ob = nullptr;
  float best_dist = threshold;

  for (const ed::sculpt_paint::PaintCurveCachedObjectSilhouette &entry : cached) {
    if (entry.object == exclude) {
      continue;
    }
    for (const Vector<float2> &polyline : entry.polylines) {
      for (const int i : IndexRange(polyline.size() - 1)) {
        const float d = paintcurve_dist_point_segment(mval, polyline[i], polyline[i + 1]);
        if (d < best_dist) {
          best_dist = d;
          best_ob = entry.object;
        }
      }
    }
  }

  if (best_ob && r_hover_polylines) {
    r_hover_polylines->clear();
    for (const ed::sculpt_paint::PaintCurveCachedObjectSilhouette &entry : cached) {
      if (entry.object == best_ob) {
        *r_hover_polylines = entry.polylines;
        break;
      }
    }
  }

  return best_ob;
}

/** \} */

}  // namespace blender
