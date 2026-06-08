/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_curve_legacy_convert.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"

#include "BLI_function_ref.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"

#include "BKE_brush.hh"
#include "BKE_paint.hh"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_intern.hh"

#include <cstdio>

namespace blender {

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
  return geom.runtime != nullptr && geom.points_num() == pc->tot_points && geom.points_num() > 0;
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

float *paintcurve_geom_co(bke::CurvesGeometry &geom, const int point_idx, const int handle_idx)
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
  /* DEBUG: every call here collapses topology to a single spline. */
  printf("[PC_DBG] paintcurve_geometry_init_bezier: point_num=%d, old curves_num=%d\n",
         point_num,
         geom.curves_num());
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

void paintcurve_geometry_from_2d(PaintCurve *pc, const ViewContext *vc)
{
  if (pc->tot_points == 0) {
    return;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  /* Keep existing curve topology (including multiple splines); only update positions. */
  if (geom.runtime != nullptr && geom.curves_num() > 0 && geom.points_num() > 0) {
    printf("[PC_DBG] paintcurve_geometry_from_2d: PRESERVE path, curves_num=%d points_num=%d\n",
           geom.curves_num(), geom.points_num());
    paintcurve_ensure_legacy_points(pc, geom.points_num());
    paintcurve_geometry_project_legacy_points_to_3d(pc, vc, geom);
    return;
  }

  printf("[PC_DBG] paintcurve_geometry_from_2d: COLLAPSE path, "
         "runtime=%p curves_num=%d points_num=%d tot_points=%d\n",
         static_cast<void *>(geom.runtime), geom.curves_num(), geom.points_num(), pc->tot_points);

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

float paintcurve_radius_from_source_geometry(const float source_radius)
{
  /* Legacy curve points are often stored as 0 until the user edits radius explicitly. */
  if (source_radius <= 0.0f) {
    return 1.0f;
  }
  return max_ff(source_radius, 0.0f);
}

void paintcurve_init_points_radius_from_geometry(PaintCurve *pc)
{
  if (pc == nullptr || pc->points == nullptr || pc->tot_points == 0) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime == nullptr || geom.points_num() == 0) {
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

void ED_paintcurve_sync_geometry_to_2d(PaintCurve *pc,
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

void paintcurve_geometry_from_curves(PaintCurve *pc,
                                     const bke::CurvesGeometry &src,
                                     const float4x4 &transform)
{
  printf("[PC_DBG] paintcurve_geometry_from_curves (IMPORT): src curves_num=%d points_num=%d\n",
         src.curves_num(), src.points_num());
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  geom = src;

  for (float3 &p : geom.positions_for_write()) {
    p = math::transform_point(transform, p);
  }
  if (geom.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    for (float3 &h : geom.handle_positions_left_for_write()) {
      h = math::transform_point(transform, h);
    }
    for (float3 &h : geom.handle_positions_right_for_write()) {
      h = math::transform_point(transform, h);
    }
  }
  geom.tag_positions_changed();

  pc->tot_points = geom.points_num();

  if (geom.points_num() > 0) {
    MutableSpan<float> radii = geom.radius_for_write();
    for (float &r : radii) {
      r = paintcurve_radius_from_source_geometry(r);
    }
  }
}

void paintcurve_geometry_from_legacy_curve(PaintCurve *pc,
                                           const Curve *curve,
                                           const float4x4 &transform)
{
  Curves *temp_curves = bke::curve_legacy_to_curves(*curve);
  if (temp_curves == nullptr) {
    paintcurve_geometry_init_bezier(pc->geometry.wrap(), 0);
    pc->tot_points = 0;
    return;
  }
  paintcurve_geometry_from_curves(pc, temp_curves->geometry.wrap(), transform);
  BKE_id_free(nullptr, &temp_curves->id);
}

/* Compute the transform that maps points from the active sculpt object's local space
 * into the source object's local space (used when writing paint curve geometry back). */
static float4x4 paintcurve_source_from_paint_transform(const Object *active, const Object *src)
{
  return src->world_to_object() * active->object_to_world();
}

/* Write the paint-curve geometry back to an OB_CURVES (Hair) data-block.
 * Performs a full copy of `src_geom` and applies the inverse transform so
 * positions land in `curves_id` local space. */
static void paintcurve_geometry_to_curves(Curves &curves_id,
                                          const bke::CurvesGeometry &src_geom,
                                          const float4x4 &src_from_paint)
{
  bke::CurvesGeometry &dst = curves_id.geometry.wrap();

  printf("[PC_DBG] paintcurve_geometry_to_curves: src curves_num=%d points_num=%d, "
         "dst BEFORE curves_num=%d points_num=%d\n",
         src_geom.curves_num(), src_geom.points_num(),
         dst.curves_num(), dst.points_num());

  dst = src_geom;

  printf("[PC_DBG] paintcurve_geometry_to_curves: dst AFTER curves_num=%d points_num=%d\n",
         dst.curves_num(), dst.points_num());
  if (dst.curve_offsets != nullptr) {
    printf("[PC_DBG]   curve_offsets:");
    for (int i = 0; i <= dst.curves_num(); i++) {
      printf(" %d", dst.curve_offsets[i]);
    }
    printf("\n");
  }

  for (float3 &p : dst.positions_for_write()) {
    p = math::transform_point(src_from_paint, p);
  }
  if (dst.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    for (float3 &h : dst.handle_positions_left_for_write()) {
      h = math::transform_point(src_from_paint, h);
    }
    for (float3 &h : dst.handle_positions_right_for_write()) {
      h = math::transform_point(src_from_paint, h);
    }
  }
  dst.tag_positions_changed();
}

/* Write the paint-curve geometry back to the Bezier Nurbs of an OB_CURVES_LEGACY object.
 * Each spline in `src_geom` is written to the corresponding Bezier Nurb in the curve.
 * Resizes individual Nurbs when a spline's point count has changed. */
static void paintcurve_geometry_to_legacy_curve(Curve &curve,
                                                const bke::CurvesGeometry &src_geom,
                                                const float4x4 &src_from_paint)
{
  const int src_curve_num = src_geom.curves_num();
  if (src_curve_num == 0 || src_geom.points_num() == 0) {
    return;
  }

  const std::optional<Span<float3>> handles_left_opt = src_geom.handle_positions_left();
  const std::optional<Span<float3>> handles_right_opt = src_geom.handle_positions_right();
  if (!handles_left_opt.has_value() || !handles_right_opt.has_value()) {
    return;
  }

  const Span<float3> positions = src_geom.positions();
  const Span<float3> handles_left = handles_left_opt.value();
  const Span<float3> handles_right = handles_right_opt.value();
  const VArray<int8_t> types_left = src_geom.handle_types_left();
  const VArray<int8_t> types_right = src_geom.handle_types_right();
  const VArray<bool> cyclic = src_geom.cyclic();
  const OffsetIndices<int> points_by_curve = src_geom.points_by_curve();

  /* Collect Bezier Nurbs from the linked list in order. */
  blender::Vector<Nurb *> nurbs;
  for (Nurb *nu = static_cast<Nurb *>(curve.nurb.first); nu; nu = nu->next) {
    if (nu->type == CU_BEZIER) {
      nurbs.append(nu);
    }
  }

  /* Write each src spline to its matching Nurb. Stop at whichever runs out first. */
  const int write_num = src_curve_num < int(nurbs.size()) ? src_curve_num : int(nurbs.size());
  printf("[PC_DBG] paintcurve_geometry_to_legacy_curve: src_curves=%d nurbs=%d write=%d\n",
         src_curve_num, int(nurbs.size()), write_num);
  for (int spline_i = 0; spline_i < write_num; spline_i++) {
    Nurb *nu = nurbs[spline_i];
    const IndexRange spline_pts = points_by_curve[spline_i];
    const int spline_point_num = int(spline_pts.size());

    if (spline_point_num == 0) {
      continue;
    }

    if (nu->pntsu != spline_point_num) {
      MEM_delete(nu->bezt);
      nu->bezt = MEM_new_array_zeroed<BezTriple>(size_t(spline_point_num), "BezTriple");
      nu->pntsu = spline_point_num;
    }

    for (int i = 0; i < spline_point_num; i++) {
      const int src_i = spline_pts[i];
      BezTriple &bezt = nu->bezt[i];

      const float3 h1 = math::transform_point(src_from_paint, handles_left[src_i]);
      const float3 pv = math::transform_point(src_from_paint, positions[src_i]);
      const float3 h2 = math::transform_point(src_from_paint, handles_right[src_i]);

      copy_v3_v3(bezt.vec[0], h1);
      copy_v3_v3(bezt.vec[1], pv);
      copy_v3_v3(bezt.vec[2], h2);

      bezt.h1 = eBezTriple_Handle(types_left[src_i]);
      bezt.h2 = eBezTriple_Handle(types_right[src_i]);
      bezt.radius = src_geom.radius()[src_i];
    }

    if (cyclic[spline_i]) {
      nu->flagu |= CU_NURB_CYCLIC;
    }
    else {
      nu->flagu &= ~CU_NURB_CYCLIC;
    }
  }
}

bool ED_paintcurve_sync_to_source_object(bContext *C, PaintCurve *pc)
{
  if (pc == nullptr) {
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr ||
      scene->toolsettings->sculpt == nullptr)
  {
    return false;
  }
  Sculpt *sculpt = scene->toolsettings->sculpt;

  if (!sculpt->paint_curve_sync_to_source) {
    return false;
  }

  Object *src_ob = sculpt->paint_curve_source_object;
  if (src_ob == nullptr || !ELEM(src_ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
    return false;
  }

  if (!ID_IS_EDITABLE(&src_ob->id)) {
    return false;
  }

  Object *active = CTX_data_active_object(C);
  if (active == nullptr) {
    return false;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.points_num() == 0) {
    return false;
  }

  printf("[PC_DBG] ED_paintcurve_sync_to_source_object: curves_num=%d points_num=%d\n",
         geom.curves_num(), geom.points_num());

  const float4x4 src_from_paint = paintcurve_source_from_paint_transform(active, src_ob);

  if (src_ob->type == OB_CURVES) {
    Curves &curves_id = *id_cast<Curves *>(src_ob->data);
    paintcurve_geometry_to_curves(curves_id, geom, src_from_paint);
  }
  else {
    Curve &curve = *id_cast<Curve *>(src_ob->data);
    paintcurve_geometry_to_legacy_curve(curve, geom, src_from_paint);
  }

  DEG_id_tag_update(&src_ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, src_ob->data);

  return true;
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
  printf("[PC_DBG] paintcurve_ensure_3d_geometry: EARLY RETURN SKIPPED, "
         "runtime=%p curves_num=%d points_num=%d tot_points=%d\n",
         static_cast<void *>(geom.runtime), geom.curves_num(), geom.points_num(), pc->tot_points);
  paintcurve_geometry_from_2d(pc, vc);
}

static int8_t paintcurve_next_handle_type(const int8_t handle_type)
{
  /* Same order as Curves Pen: Free → Auto → Vector → Align → Free. */
  return (handle_type + 1) % 4;
}

float paintcurve_get_point_radius(const PaintCurve *pc, const int point_index)
{
  if (pc == nullptr || point_index < 0 || point_index >= pc->tot_points || pc->points == nullptr) {
    return 1.0f;
  }
  return max_ff(pc->points[point_index].bez.radius, 0.0f);
}

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

void ED_paintcurve_flush_radius_transform(bContext *C, PaintCurve *pc)
{
  if (pc == nullptr || pc->points == nullptr) {
    return;
  }
  for (int i = 0; i < pc->tot_points; i++) {
    pc->points[i].bez.radius = max_ff(pc->points[i].bez.radius, 0.0f);
  }
  paintcurve_sync_geometry_radius_from_points(pc);
  ED_paintcurve_sync_to_source_object(C, pc);
}

void paintcurve_sync_geometry_radius_from_points(PaintCurve *pc)
{
  if (pc == nullptr || pc->points == nullptr) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime == nullptr || geom.points_num() == 0) {
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

}  // namespace blender
