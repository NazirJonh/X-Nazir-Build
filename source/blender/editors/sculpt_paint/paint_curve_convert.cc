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
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"

#include "DEG_depsgraph.hh"

#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_intern.hh"

namespace blender {

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
  geom = bke::CurvesGeometry(point_num, point_num > 0 ? 1 : 0);
  if (point_num == 0) {
    return;
  }

  geom.offsets_for_write()[0] = 0;
  geom.offsets_for_write()[1] = point_num;
  geom.fill_curve_types(CURVE_TYPE_BEZIER);
  geom.resolution_for_write().fill(PAINT_CURVE_NUM_SEGMENTS);
  geom.handle_types_left_for_write().fill(BEZIER_HANDLE_FREE);
  geom.handle_types_right_for_write().fill(BEZIER_HANDLE_FREE);
  geom.tag_topology_changed();
}

void paintcurve_geometry_from_2d(PaintCurve *pc, const ViewContext *vc)
{
  if (pc->tot_points == 0) {
    return;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  paintcurve_geometry_init_bezier(geom, pc->tot_points);

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
  }
  geom.tag_positions_changed();
}

void ED_paintcurve_sync_geometry_to_2d(PaintCurve *pc,
                                       const ViewContext *vc,
                                       const float ob_to_world[4][4])
{
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.points_num() == 0) {
    return;
  }

  pc->tot_points = geom.points_num();
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
  }
}

void paintcurve_geometry_from_curves(PaintCurve *pc,
                                     const bke::CurvesGeometry &src,
                                     const float4x4 &transform)
{
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
}

void paintcurve_geometry_from_legacy_curve(PaintCurve *pc,
                                           const Curve *curve,
                                           const float4x4 &transform)
{
  const Nurb *nu = static_cast<const Nurb *>(curve->nurb.first);
  while (nu != nullptr && (nu->type != CU_BEZIER || nu->bezt == nullptr)) {
    nu = nu->next;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (nu == nullptr || nu->pntsu == 0) {
    paintcurve_geometry_init_bezier(geom, 0);
    pc->tot_points = 0;
    return;
  }

  const int point_num = nu->pntsu;
  paintcurve_geometry_init_bezier(geom, point_num);
  for (int i = 0; i < point_num; i++) {
    const BezTriple &bezt = nu->bezt[i];
    for (int j = 0; j < 3; j++) {
      float3 co(bezt.vec[j][0], bezt.vec[j][1], bezt.vec[j][2]);
      co = math::transform_point(transform, co);
      copy_v3_v3(paintcurve_geom_co(geom, i, j), co);
    }
    geom.handle_types_left_for_write()[i] = int8_t(bezt.h1);
    geom.handle_types_right_for_write()[i] = int8_t(bezt.h2);
  }
  geom.tag_positions_changed();
  pc->tot_points = point_num;
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
  dst = src_geom;

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

/* Write the paint-curve geometry back to the first Bezier Nurb of an OB_CURVES_LEGACY object.
 * Resizes the spline when the point count has changed (full sync). */
static void paintcurve_geometry_to_legacy_curve(Curve &curve,
                                                const bke::CurvesGeometry &src_geom,
                                                const float4x4 &src_from_paint)
{
  Nurb *nu = static_cast<Nurb *>(curve.nurb.first);
  while (nu != nullptr && (nu->type != CU_BEZIER || nu->bezt == nullptr)) {
    nu = nu->next;
  }
  if (nu == nullptr) {
    return;
  }

  const int point_num = src_geom.points_num();
  if (point_num == 0) {
    return;
  }

  if (nu->pntsu != point_num) {
    MEM_delete_void(static_cast<void *>(nu->bezt));
    nu->bezt = static_cast<BezTriple *>(
        MEM_new_array_zeroed(point_num, sizeof(BezTriple), "BezTriple"));
    nu->pntsu = point_num;
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

  for (int i = 0; i < point_num; i++) {
    BezTriple &bezt = nu->bezt[i];

    const float3 h1 = math::transform_point(src_from_paint, handles_left[i]);
    const float3 pv = math::transform_point(src_from_paint, positions[i]);
    const float3 h2 = math::transform_point(src_from_paint, handles_right[i]);

    copy_v3_v3(bezt.vec[0], h1);
    copy_v3_v3(bezt.vec[1], pv);
    copy_v3_v3(bezt.vec[2], h2);

    bezt.h1 = eBezTriple_Handle(types_left[i]);
    bezt.h2 = eBezTriple_Handle(types_right[i]);
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

}  // namespace blender
