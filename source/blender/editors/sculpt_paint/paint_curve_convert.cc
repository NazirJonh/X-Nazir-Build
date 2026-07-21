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
#include "BKE_curve.hh"
#include "BKE_curve_legacy_convert.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_brush.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

static bool paintcurve_viewcontext_for_conversion(bContext *C,
                                                  Depsgraph *depsgraph,
                                                  ViewContext &r_vc)
{
  if (C == nullptr || depsgraph == nullptr) {
    return false;
  }

  r_vc = ED_view3d_viewcontext_init(C, depsgraph);

  View3D *v3d = nullptr;
  ARegion *region = nullptr;
  if (!ED_view3d_context_user_region(C, &v3d, &region)) {
    return false;
  }

  r_vc.v3d = v3d;
  r_vc.region = region;
  r_vc.rv3d = static_cast<RegionView3D *>(region->regiondata);
  r_vc.win = CTX_wm_window(C);

  return r_vc.rv3d != nullptr && r_vc.obact != nullptr;
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
  /* Legacy BezTriple.radius=0.0 is the factory default and means "not yet customised —
   * use full brush size". However, when a spline was already edited and only SOME points
   * carry radius=0.0, those zeros represent an intentional "minimum stroke" rather than an
   * unset sentinel; converting them to 1.0 would corrupt user data.
   *
   * Heuristic applied per spline: if ALL points in a spline have radius <= 0.0 the spline
   * is a fresh/unedited import and every zero maps to 1.0 (full brush). If ANY point in a
   * spline has a positive radius the spline has been customised and zeros are preserved as
   * "minimum size". */
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.points_num() > 0) {
    MutableSpan<float> radii = geom.radius_for_write();
    const OffsetIndices<int> points_by_curve = geom.points_by_curve();
    for (const int curve_i : geom.curves_range()) {
      const IndexRange pts = points_by_curve[curve_i];
      bool all_zero = true;
      for (const int pt_i : pts) {
        if (radii[pt_i] > 0.0f) {
          all_zero = false;
          break;
        }
      }
      if (all_zero) {
        for (const int pt_i : pts) {
          radii[pt_i] = 1.0f;
        }
      }
    }
  }
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
  dst.update_curve_types();
  dst.tag_positions_changed();
  dst.tag_topology_changed();
}

/* Copy display / shape settings from `src` to `dst`, leaving geometry (nurb list) untouched.
 * Called when creating a new OB_CURVES_LEGACY destination so it mirrors the source exactly.
 * Object pointers (bevel / taper objects) get an extra user so the data-block keeps its
 * reference count in sync when both curves exist in the scene at the same time. */
static void legacy_curve_copy_settings(Curve &dst, const Curve &src)
{
  /* 2D/3D mode, front/back fill, path, follow, bevel caps, etc. */
  dst.flag = src.flag;
  /* Tessellation resolution for viewport and render. */
  dst.resolu = src.resolu;
  dst.resolv = src.resolv;
  dst.resolu_ren = src.resolu_ren;
  dst.resolv_ren = src.resolv_ren;
  /* Twist calculation. */
  dst.twist_mode = src.twist_mode;
  dst.twist_smooth = src.twist_smooth;
  /* Bevel / extrusion / offset. */
  dst.bevel_radius = src.bevel_radius;
  dst.bevel_mode = src.bevel_mode;
  dst.bevresol = src.bevresol;
  dst.offset = src.offset;
  dst.extrude = src.extrude;
  /* Path animation length. */
  dst.pathlen = src.pathlen;
  /* Taper radius blending mode. */
  dst.taper_radius_mode = src.taper_radius_mode;
  /* Referenced objects — increment user count so the scene graph stays consistent. */
  dst.bevobj = src.bevobj;
  if (dst.bevobj) {
    id_us_plus(&dst.bevobj->id);
  }
  dst.taperobj = src.taperobj;
  if (dst.taperobj) {
    id_us_plus(&dst.taperobj->id);
  }
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

  /* The paint curve may have gained splines (e.g. duplicate); create matching Bezier Nurbs so
   * the new splines are written back to the source instead of being silently dropped. New Nurbs
   * inherit resolution from an existing Bezier Nurb when present, otherwise from the curve. */
  if (src_curve_num > int(nurbs.size())) {
    const short resolu = nurbs.is_empty() ? curve.resolu : nurbs[0]->resolu;
    const short resolv = nurbs.is_empty() ? curve.resolv : nurbs[0]->resolv;
    while (int(nurbs.size()) < src_curve_num) {
      Nurb *nu = MEM_new<Nurb>(__func__);
      nu->type = CU_BEZIER;
      nu->pntsv = 0;
      nu->resolu = resolu;
      nu->resolv = resolv;
      nu->flag |= CU_SMOOTH;
      BLI_addtail(&curve.nurb, nu);
      nurbs.append(nu);
    }
  }

  /* Remove surplus Bezier Nurbs after merge/split reduced the spline count. */
  while (int(nurbs.size()) > src_curve_num) {
    Nurb *nu = nurbs.pop_last();
    BLI_remlink(&curve.nurb, nu);
    BKE_nurb_free(nu);
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
      /* Avoid writing exactly 0: legacy re-import treats radius <= 0 as "never set" and
       * remaps it to 1.0 (full brush). Clamping to FLT_EPSILON preserves near-zero intent
       * while keeping the value distinguishable from the uninitialized sentinel. */
      bezt.radius = max_ff(src_geom.radius()[src_i], FLT_EPSILON);
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
  if (!paintcurve_geometry_is_valid(geom)) {
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
  /* Use WM_toolsystem_key_from_context + WM_toolsystem_ref_find instead of
   * WM_toolsystem_ref_from_context to avoid BLI_assert(tref == area->runtime.tool) which fires
   * during mode transitions: area->runtime.tool is not yet synced when this function is called
   * right after object_sculpt_mode_enter in sculpt_mode_toggle_exec. */
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *sa = CTX_wm_area(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const bToolRef *tref = nullptr;
  if (workspace && sa) {
    bToolKey tkey{};
    if (WM_toolsystem_key_from_context(*bmain, scene, view_layer, sa, &tkey)) {
      tref = WM_toolsystem_ref_find(workspace, &tkey);
    }
  }
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

static void paintcurve_assign_source_object(Sculpt *sculpt, Object *src_ob)
{
  if (sculpt->paint_curve_source_object == src_ob) {
    sculpt->paint_curve_sync_to_source = 1;
    return;
  }
  if (sculpt->paint_curve_source_object != nullptr) {
    id_us_min(&sculpt->paint_curve_source_object->id);
  }
  if (src_ob != nullptr) {
    id_us_plus(&src_ob->id);
  }
  sculpt->paint_curve_source_object = src_ob;
  sculpt->paint_curve_sync_to_source = 1;
}

bool ED_paintcurve_export_to_scene_object(bContext *C,
                                          ReportList *reports,
                                          Object **r_dst_ob,
                                          const ePaintCurveExportCurveType curve_type,
                                          const bool use_selection,
                                          const bool assign_as_source)
{
  if (r_dst_ob) {
    *r_dst_ob = nullptr;
  }

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *active = CTX_data_active_object(C);

  if (active == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "No active object");
    }
    return false;
  }
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr)
  {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Sculpt tool settings not available");
    }
    return false;
  }

  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Sculpt paint settings not available");
    }
    return false;
  }

  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    BKE_paint_brush_set_default(bmain, paint);
    brush = BKE_paint_brush(paint);
  }
  if (brush == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "No active sculpt brush");
    }
    return false;
  }

  PaintCurve *pc = brush->paint_curve;
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Paint curve has no points to export");
    }
    return false;
  }

  pc->use_3d_space = 1;
  WM_event_add_notifier(C, NC_PAINTCURVE | NA_EDITED, pc);

  const bke::CurvesGeometry &src_geom = pc->geometry.wrap();
  const bke::CurvesGeometry export_geom = use_selection ?
                                              paintcurve_geometry_build_from_selected_points(
                                                  src_geom) :
                                              src_geom;
  if (!paintcurve_geometry_is_valid(export_geom)) {
    if (reports) {
      BKE_report(reports,
                 RPT_ERROR,
                 use_selection ? "No valid points in the current selection" :
                                 "Paint curve has no points to export");
    }
    return false;
  }

  char ob_name[MAX_ID_NAME];
  SNPRINTF(ob_name, "%s Curve", brush->id.name + 2);

  const Object *existing_src = scene->toolsettings->sculpt->paint_curve_source_object;
  const bool use_legacy_curve = (curve_type == PAINT_CURVE_EXPORT_BEZIER);
  Object *dst_ob = BKE_object_add(
      bmain, scene, view_layer, use_legacy_curve ? OB_CURVES_LEGACY : OB_CURVES, ob_name);
  if (dst_ob == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Could not create curve object on the scene");
    }
    return false;
  }

  /* Match the sculpt object transform so paint-curve local coordinates map 1:1. */
  BKE_object_apply_mat4(dst_ob, active->object_to_world().ptr(), true, false);

  const float4x4 src_from_paint = paintcurve_source_from_paint_transform(active, dst_ob);
  if (use_legacy_curve) {
    Curve &curve = *id_cast<Curve *>(dst_ob->data);
    if (existing_src != nullptr && existing_src->type == OB_CURVES_LEGACY) {
      const Curve &src_curve = *id_cast<const Curve *>(existing_src->data);
      legacy_curve_copy_settings(curve, src_curve);
    }
    paintcurve_geometry_to_legacy_curve(curve, export_geom, src_from_paint);
  }
  else {
    Curves &curves_id = *id_cast<Curves *>(dst_ob->data);
    paintcurve_geometry_to_curves(curves_id, export_geom, src_from_paint);
  }

  /* Copy modifiers from the original source curves object as linked references: data-blocks
   * referenced by each modifier (objects, textures, etc.) are shared rather than duplicated.
   * Only modifier types supported by the destination object type are transferred. */
  if (existing_src != nullptr && !existing_src->modifiers.is_empty()) {
    BKE_object_link_modifiers(dst_ob, existing_src);
  }

  Sculpt *sculpt = scene->toolsettings->sculpt;
  if (assign_as_source) {
    paintcurve_assign_source_object(sculpt, dst_ob);
  }

  DEG_id_tag_update(static_cast<ID *>(dst_ob->data), ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&dst_ob->id, ID_RECALC_GEOMETRY);
  DEG_relations_tag_update(bmain);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  if (reports) {
    BKE_reportf(reports,
                RPT_INFO,
                "Created %s object \"%s\" from %s",
                use_legacy_curve ? "Curve" : "Curves",
                dst_ob->id.name + 2,
                use_selection ? "selection" : "paint curve");
  }

  if (r_dst_ob) {
    *r_dst_ob = dst_ob;
  }
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
  Sculpt *sculpt = scene->toolsettings->sculpt;
  if (sculpt->paint_curve_source_object == nullptr) {
    /* Source object was deleted while we were in Object Mode: clear the intermediate paint curve
     * so stale geometry from the deleted object is not shown. Only wipe when the paint curve was
     * previously synced to a source (paint_curve_sync_to_source != 0); otherwise the curve may
     * have been drawn from scratch and we must not touch it. */
    if (sculpt->paint_curve_sync_to_source != 0) {
      Paint *paint = BKE_paint_get_active_from_context(C);
      Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
      if (brush && brush->paint_curve) {
        brush->paint_curve->geometry.wrap() = bke::CurvesGeometry();
        WM_event_add_notifier(C, NC_PAINTCURVE | NA_EDITED, brush->paint_curve);
      }
      sculpt->paint_curve_sync_to_source = 0;
    }
    return;
  }
  ED_paintcurve_import_from_source_object(C, nullptr, false);
}

void ED_paintcurve_detach_source(bContext *C)
{
  if (C == nullptr) {
    return;
  }
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr)
  {
    return;
  }
  Sculpt *sculpt = scene->toolsettings->sculpt;

  /* Deliberately leaves the paint curve alone. This runs from an RNA property assignment, and
   * clearing the source there used to destroy the user's curve as a side effect -- invisible from
   * Python, and reachable from any generic property copy or preset. Wiping the canvas is
   * #PAINTCURVE_OT_clear's job now. */
  sculpt->paint_curve_sync_to_source = 0;

  /* Force the paint-cursor overlay to repaint so the control-point display clears immediately.
   * NC_PAINTCURVE notifiers are not observed by the 3D viewport, so we go through the paint
   * cursor redraw path which sets screen->do_draw_paintcursor directly. */
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
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

bool ED_paintcurve_convert_space_on_toggle(bContext *C, PaintCurve *pc)
{
  if (C == nullptr || pc == nullptr) {
    return false;
  }

  if (!paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return true;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "Need an active 3D Viewport to convert paint curve space");
    return false;
  }

  ViewContext vc;
  if (!paintcurve_viewcontext_for_conversion(C, depsgraph, vc)) {
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "Need an active 3D Viewport to convert paint curve space");
    return false;
  }

  /* Push undo BEFORE modifying geometry so Ctrl+Z restores both the coordinates
   * and the flag to their pre-toggle state. Temporarily revert use_3d_space so
   * the snapshot captures the complete pre-operation state. */
  const bool can_undo = paint_curve_poll(C);
  if (can_undo) {
    const char new_flag = pc->use_3d_space;
    pc->use_3d_space = !new_flag;
    ED_paintcurve_undo_push_begin(C, "Toggle 3D Curve");
    pc->use_3d_space = new_flag;
  }

  if (!paintcurve_convert_geometry_space(C, pc, &vc, pc->use_3d_space != 0)) {
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "Need an active 3D Viewport and object to convert paint curve space");
    if (can_undo) {
      ED_paintcurve_undo_push_end(C);
    }
    return false;
  }

  if (pc->use_3d_space) {
    paintcurve_sync_to_source_object(C, pc);
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush) {
    BKE_brush_tag_unsaved_changes(brush);
  }

  wmWindow *win = CTX_wm_window(C);
  if (win && vc.region) {
    WM_paint_cursor_tag_redraw(win, vc.region);
    ED_region_tag_redraw(vc.region);
  }

  if (can_undo) {
    ED_paintcurve_undo_push_end(C);
  }

  return true;
}

}  // namespace blender
