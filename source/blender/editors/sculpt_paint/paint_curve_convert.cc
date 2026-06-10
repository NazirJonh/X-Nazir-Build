/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Import and export between a #PaintCurve and a Curves/Curve source object:
 * building the paint-curve geometry from a picked object, writing edits back to it,
 * and the operators' shared entry points (#ED_paintcurve_import_from_source_object).
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_curve_legacy_convert.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "BKE_brush.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

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

bool paintcurve_sync_to_source_object(bContext *C, PaintCurve *pc)
{
  if (pc == nullptr) {
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr)
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

bool ED_paintcurve_import_from_source_object(bContext *C, ReportList *reports, const bool use_undo)
{
  Main *bmain = CTX_data_main(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  Object *active = CTX_data_active_object(C);

  if (active == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "No active object");
    }
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr)
  {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Sculpt tool settings not available");
    }
    return false;
  }

  const bool is_curve_stroke_brush = br && br->stroke_method == BRUSH_STROKE_CURVE;
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  const bool is_curve_edit_tool = tref && STREQ(tref->idname, "builtin.curves_edit");
  if (!is_curve_stroke_brush && !is_curve_edit_tool) {
    return false;
  }

  Object *src_ob = scene->toolsettings->sculpt->paint_curve_source_object;
  if (src_ob == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Pick a source Curves or Curve object");
    }
    return false;
  }

  PaintCurve *pc = br->paint_curve;
  if (!pc) {
    br->paint_curve = pc = paintcurve_for_brush_add(bmain, DATA_("PaintCurve"), br);
  }
  if (!ELEM(src_ob->type, OB_CURVES, OB_CURVES_LEGACY)) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Source object must be a Curves or Curve object");
    }
    return false;
  }

  if (BKE_paintmode_get_active_from_context(C) == PaintMode::SculptCurves) {
    if (src_ob->type == OB_CURVES) {
      const Curves &curves_id = *id_cast<const Curves *>(src_ob->data);
      if (curves_id.geometry.wrap().curves_num() > 1) {
        if (reports) {
          BKE_report(reports,
                     RPT_ERROR,
                     "Sculpt Curves paint curve supports only one spline in the source object");
        }
        return false;
      }
    }
    else {
      const Curve &curve = *id_cast<const Curve *>(src_ob->data);
      int spline_count = 0;
      for (const Nurb *nu = static_cast<const Nurb *>(curve.nurb.first); nu; nu = nu->next) {
        if (nu->pntsu >= 1) {
          spline_count++;
        }
      }
      if (spline_count > 1) {
        if (reports) {
          BKE_report(reports,
                     RPT_ERROR,
                     "Sculpt Curves paint curve supports only one spline in the source object");
        }
        return false;
      }
    }
  }

  const float4x4 transform = active->world_to_object() * src_ob->object_to_world();

  if (use_undo) {
    ED_paintcurve_undo_push_begin(C, "Paint Curve from Curve Object");
  }

  if (src_ob->type == OB_CURVES) {
    const Curves &curves_id = *id_cast<const Curves *>(src_ob->data);
    paintcurve_geometry_from_curves(pc, curves_id.geometry.wrap(), transform);
  }
  else {
    const Curve &curve = *id_cast<const Curve *>(src_ob->data);
    paintcurve_geometry_from_legacy_curve(pc, &curve, transform);
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  pc->add_index = geom.points_num();
  pc->use_3d_space = 1;
  scene->toolsettings->sculpt->paint_curve_sync_to_source = 1;

  if (use_undo) {
    ED_paintcurve_undo_push_end(C);
  }
  BKE_brush_tag_unsaved_changes(br);
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  return true;
}

void ED_paintcurve_refresh_on_sculpt_mode_enter(bContext *C)
{
  if (C == nullptr) {
    return;
  }
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr)
  {
    return;
  }
  if (scene->toolsettings->sculpt->paint_curve_source_object == nullptr) {
    return;
  }
  ED_paintcurve_import_from_source_object(C, nullptr, false);
}

void ED_paintcurve_flush_radius_transform(bContext *C, PaintCurve *pc)
{
  if (pc == nullptr) {
    return;
  }
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return;
  }
  MutableSpan<float> radii = geom.radius_for_write();
  for (float &r : radii) {
    r = max_ff(r, 0.0f);
  }
  paintcurve_sync_to_source_object(C, pc);
}

bool ED_paintcurve_sync_to_source(bContext *C, PaintCurve *pc)
{
  return paintcurve_sync_to_source_object(C, pc);
}

}  // namespace blender
