/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>
#include <utility>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_workspace_types.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_string_utf8.h"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_types.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "mesh/sculpt_intern.hh"
#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

#define PAINT_CURVE_SELECT_THRESHOLD 40.0f

/* Set by select when a handle-type cycle consumed the click; slide must not start a drag. */
static bool paintcurve_skip_next_slide = false;
static bool paintcurve_slide_active = false;
static int paintcurve_slide_segment_a = -1;
static int paintcurve_slide_segment_b = -1;

/* Screen-space snap indicator shown during a 3D paint-curve slide. The slide modal stores the
 * snapped target here on each mouse-move; the Overlay engine (#PaintCurveCursor) reads it via
 * #paintcurve_snap_marker_get and draws the marker. Kept here (not in #PointSlideData) because the
 * draw engine cannot access the operator's custom data. */
struct PaintCurveSnapMarker {
  bool active = false;
  float screen_pos[2] = {0.0f, 0.0f};
  /** Active geometry snap elements (#SCE_SNAP_TO_GEOM subset), for marker styling. */
  eSnapMode type = SCE_SNAP_TO_NONE;
};
static PaintCurveSnapMarker paintcurve_snap_marker = {};

static void paintcurve_snap_marker_clear_local()
{
  paintcurve_snap_marker.active = false;
}

/** Project an object-space snap hit to region-space and store it for the overlay marker. */
static void paintcurve_snap_marker_update(bContext *C,
                                          const ARegion *region,
                                          const float ob_to_world[4][4],
                                          const float hit_obj[3])
{
  if (region == nullptr) {
    paintcurve_snap_marker_clear_local();
    return;
  }
  const ToolSettings *ts = CTX_data_tool_settings(C);
  const eSnapMode type = ts ? eSnapMode(ED_paintcurve_snap_elements(ts) & SCE_SNAP_TO_GEOM) :
                              SCE_SNAP_TO_NONE;
  float hit_world[3];
  mul_v3_m4v3(hit_world, ob_to_world, hit_obj);
  float screen[2];
  ED_view3d_project_v2(region, hit_world, screen);
  paintcurve_snap_marker.active = true;
  copy_v2_v2(paintcurve_snap_marker.screen_pos, screen);
  paintcurve_snap_marker.type = type;
}

bool paintcurve_snap_marker_get(float r_screen[2], int *r_type)
{
  if (!paintcurve_snap_marker.active) {
    return false;
  }
  copy_v2_v2(r_screen, paintcurve_snap_marker.screen_pos);
  if (r_type != nullptr) {
    *r_type = int(paintcurve_snap_marker.type);
  }
  return true;
}

static constexpr double PAINT_CURVE_SEGMENT_DBLCLICK_TIME_SEC = 2.0;

struct PaintCurveShiftSegmentClickState {
  double time = 0.0;
  int point_a = -1;
  int point_b = -1;
};
static PaintCurveShiftSegmentClickState paintcurve_shift_segment_click = {};

static void paintcurve_slide_segment_clear()
{
  paintcurve_slide_segment_a = -1;
  paintcurve_slide_segment_b = -1;
}

static bool paintcurve_find_closest_segment(PaintCurve *pc,
                                            const ViewContext *vc,
                                            const Span<PaintCurvePoint> screen_points,
                                            const float pos[2],
                                            const float threshold,
                                            int *r_segment_index,
                                            int *r_segment_index_next,
                                            float *r_edge_t);
static bool paintcurve_insert_point_at_segment(
    bContext *C, wmOperator *op, PaintCurve *pc, const int segment_index, const float edge_t);
static bool paintcurve_try_insert_point_at_mouse(bContext *C,
                                                 wmOperator *op,
                                                 PaintCurve *pc,
                                                 const float loc_fl[2]);
static const bToolRef *paintcurve_tool_ref_from_context(bContext *C);

bool paintcurve_slide_is_active()
{
  return paintcurve_slide_active;
}

bool paintcurve_slide_segment_active(int *r_point_a, int *r_point_b)
{
  if (paintcurve_slide_segment_a < 0) {
    return false;
  }
  if (r_point_a) {
    *r_point_a = paintcurve_slide_segment_a;
  }
  if (r_point_b) {
    *r_point_b = paintcurve_slide_segment_b;
  }
  return true;
}

static void paintcurve_sync_to_source_if_3d(bContext *C, PaintCurve *pc)
{
  if (pc && paintcurve_uses_3d_geometry(pc)) {
    paintcurve_sync_to_source_object(C, pc);
  }
}

static void paintcurve_sync_after_handle_type_change(bContext *C, PaintCurve *pc)
{
  paintcurve_sync_to_source_if_3d(C, pc);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = paint ? BKE_paint_brush(paint) : nullptr;
  if (br) {
    BKE_brush_tag_unsaved_changes(br);
  }
}

static bool paintcurve_cycle_handle_type_at_point(bContext *C,
                                                  wmOperator *op,
                                                  PaintCurve *pc,
                                                  const int point_index)
{
  ED_paintcurve_undo_push_begin(C, op->type->name);
  paintcurve_cycle_point_handle_type(pc, point_index);
  paintcurve_sync_after_handle_type_change(C, pc);
  ED_paintcurve_undo_push_end(C);
  paintcurve_skip_next_slide = true;
  return true;
}

/**
 * When exactly one control point is selected, set #PaintCurve.active_curve and #PaintCurve.add_index
 * so the next added point extends from that end of the spline (start vs end).
 */
static bool paintcurve_update_add_index_from_selection(PaintCurve *pc,
                                                       const bke::CurvesGeometry &geom)
{
  if (pc == nullptr || !paintcurve_geometry_is_valid(geom)) {
    return false;
  }

  int selected_point = -1;
  int selected_count = 0;
  for (const int i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, i) & 0x07) {
      selected_point = i;
      selected_count++;
      if (selected_count > 1) {
        return false;
      }
    }
  }
  if (selected_count != 1) {
    return false;
  }

  const int spline_idx = paintcurve_curve_of_point(pc, selected_point);
  if (spline_idx < 0) {
    return false;
  }

  pc->active_curve = spline_idx;
  const OffsetIndices<int> pbc = geom.points_by_curve();
  const int idx_in_spline = selected_point - pbc[spline_idx].start();
  const int spline_size = pbc[spline_idx].size();
  pc->add_index = (idx_in_spline || spline_size == 1) ? (idx_in_spline + 1) : 0;
  return true;
}

/** Match #subdivide_curves.cc: preserve aligned/auto boundary handles after segment split. */
static int8_t paintcurve_aligned_or_free_handle_type(const int8_t handle_type)
{
  switch (handle_type) {
    case BEZIER_HANDLE_FREE:
      return BEZIER_HANDLE_FREE;
    case BEZIER_HANDLE_AUTO:
      return BEZIER_HANDLE_ALIGN;
    case BEZIER_HANDLE_VECTOR:
      return BEZIER_HANDLE_FREE;
    case BEZIER_HANDLE_ALIGN:
      return BEZIER_HANDLE_ALIGN;
  }
  BLI_assert_unreachable();
  return BEZIER_HANDLE_FREE;
}


bool paint_curve_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  SpaceImage *sima;

  if (rv3d && !(ob && ((ob->mode & (OB_MODE_ALL_PAINT | OB_MODE_SCULPT_CURVES |
                                    OB_MODE_SCULPT_GREASE_PENCIL)) != 0)))
  {
    return false;
  }

  sima = CTX_wm_space_image(C);

  if (sima && sima->mode != SI_MODE_PAINT) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = (paint) ? BKE_paint_brush(paint) : nullptr;

  if (brush && (brush->stroke_method == BRUSH_STROKE_CURVE)) {
    return true;
  }

  /* Also allow paint-curve operators when the standalone Curve Edit tool is active. */
  const bToolRef *tref = paintcurve_tool_ref_from_context(C);
  if (tref && STREQ(tref->idname, "builtin.curves_edit")) {
    return true;
  }

  return false;
}

/* Find the closest control-point handle in `screen_points` to `pos`.
 * When `ignore_pivot` is true, a click on the pivot (vec[1]) redirects to the nearer handle.
 * Returns the point index, or -1 if none is within `threshold`. Sets `*r_selflag` (SEL_F1/F2/F3). */
int paintcurve_find_in_screen_points(const Span<PaintCurvePoint> screen_points,
                                            const float pos[2],
                                            const bool ignore_pivot,
                                            const float threshold,
                                            char *r_selflag)
{
  int found_idx = -1;
  char found_flag = 0;
  float closest_dist = threshold;

  for (const int i : screen_points.index_range()) {
    const PaintCurvePoint &pcp = screen_points[i];
    const float dist[3] = {
        len_manhattan_v2v2(pos, pcp.bez.vec[0]),
        len_manhattan_v2v2(pos, pcp.bez.vec[1]),
        len_manhattan_v2v2(pos, pcp.bez.vec[2]),
    };
    char point_sel = 0;
    if (dist[1] < closest_dist) {
      closest_dist = dist[1];
      point_sel = SEL_F2;
    }
    if (dist[0] < closest_dist) {
      closest_dist = dist[0];
      point_sel = SEL_F1;
    }
    if (dist[2] < closest_dist) {
      closest_dist = dist[2];
      point_sel = SEL_F3;
    }
    if (point_sel) {
      found_idx = i;
      found_flag = point_sel;
    }
  }

  if (found_idx >= 0 && ignore_pivot && found_flag == SEL_F2) {
    const PaintCurvePoint &pcp = screen_points[found_idx];
    const float d0 = len_manhattan_v2v2(pos, pcp.bez.vec[0]);
    const float d2 = len_manhattan_v2v2(pos, pcp.bez.vec[2]);
    found_flag = (d0 < d2) ? SEL_F1 : SEL_F3;
  }

  if (r_selflag) {
    *r_selflag = found_flag;
  }
  return found_idx;
}

bool ED_paintcurve_cursor_on_selected_handle(bContext *C, const float mval[2])
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  if (pc == nullptr) {
    return false;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom) || !paintcurve_geom_any_selected(geom)) {
    return false;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);

  char selflag = 0;
  const int hit = paintcurve_find_in_screen_points(
      screen_points.as_span(), mval, false, PAINT_CURVE_HOVER_THRESHOLD, &selflag);
  if (hit < 0) {
    return false;
  }

  const uint8_t pt_sel = paintcurve_geom_get_selection(geom, hit);
  if (!pt_sel) {
    return false;
  }
  /* Pivot selected: all three handles participate in the transform. */
  if (pt_sel & 0x02) {
    return true;
  }
  if (selflag == SEL_F1) {
    return (pt_sel & 0x01) != 0;
  }
  if (selflag == SEL_F3) {
    return (pt_sel & 0x04) != 0;
  }
  return false;
}

static int paintcurve_point_co_index(char sel)
{
  char i = 0;
  while (sel != 1) {
    sel >>= 1;
    i++;
  }
  return i;
}

static char paintcurve_point_side_index(const BezTriple *bezt,
                                        const bool is_first,
                                        const char fallback)
{
  /* when matching, guess based on endpoint side */
  if (BEZT_ISSEL_ANY(bezt)) {
    if ((bezt->f1 & SELECT) == (bezt->f3 & SELECT)) {
      return is_first ? SEL_F1 : SEL_F3;
    }
    if (bezt->f1 & SELECT) {
      return SEL_F1;
    }
    if (bezt->f3 & SELECT) {
      return SEL_F3;
    }
    return fallback;
  }
  return 0;
}

/******************* Operators *********************************/

/** Safer than #WM_toolsystem_ref_from_context when the area tool runtime is briefly out of sync. */
static const bToolRef *paintcurve_tool_ref_from_context(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  WorkSpace *workspace = CTX_wm_workspace(C);
  ScrArea *area = CTX_wm_area(C);
  if (!(workspace && bmain && scene && area)) {
    return nullptr;
  }
  bToolKey tkey{};
  if (!WM_toolsystem_key_from_context(*bmain, scene, view_layer, area, &tkey)) {
    return nullptr;
  }
  return WM_toolsystem_ref_find(workspace, &tkey);
}

PaintCurve *paintcurve_for_brush_add(Main *bmain, const char *name, const Brush *brush)
{
  PaintCurve *curve = BKE_paint_curve_add(bmain, name);
  BKE_id_move_to_same_lib(*bmain, curve->id, brush->id);
  return curve;
}

PaintCurve *paintcurve_active_from_context(bContext *C, Brush **r_brush)
{
  if (r_brush) {
    *r_brush = nullptr;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return nullptr;
  }

  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    return nullptr;
  }

  if (r_brush) {
    *r_brush = brush;
  }

  PaintCurve *pc = brush->paint_curve;
  if (pc == nullptr) {
    Main *bmain = CTX_data_main(C);
    brush->paint_curve = pc = paintcurve_for_brush_add(bmain, DATA_("PaintCurve"), brush);
  }
  return pc;
}

static wmOperatorStatus paintcurve_new_exec(bContext *C, wmOperator * /*op*/)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = (paint) ? BKE_paint_brush(paint) : nullptr;
  Main *bmain = CTX_data_main(C);

  if (brush) {
    brush->paint_curve = paintcurve_for_brush_add(bmain, DATA_("PaintCurve"), brush);
    BKE_brush_tag_unsaved_changes(brush);
  }

  WM_event_add_notifier(C, NC_PAINTCURVE | NA_ADDED, nullptr);

  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_new(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add New Paint Curve";
  ot->description = "Add new paint curve";
  ot->idname = "PAINTCURVE_OT_new";

  /* API callbacks. */
  ot->exec = paintcurve_new_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** View direction in object space, used when no real surface normal is available. */
static float3 paintcurve_view_normal_obj(const ViewContext &vc)
{
  if (vc.rv3d == nullptr || vc.obact == nullptr) {
    return float3(0.0f, 0.0f, 1.0f);
  }
  float ob_origin_world[3];
  copy_v3_v3(ob_origin_world, vc.obact->object_to_world().location());
  float view_world[3];
  ED_view3d_global_to_vector(vc.rv3d, ob_origin_world, view_world);
  /* A direction, so no translation. The view vector points away from the viewer while a
   * surface normal points towards it, hence the negation. */
  const float3 normal_obj = math::transform_direction(vc.obact->world_to_object(),
                                                      -float3(view_world));
  const float len = math::length(normal_obj);
  return (len > 1e-6f) ? (normal_obj / len) : float3(0.0f, 0.0f, 1.0f);
}

/** Whether scene snap elements should be tried before falling back to the surface. */
static bool paintcurve_wants_element_snap(bContext *C)
{
  const ToolSettings *ts = CTX_data_tool_settings(C);
  return ts != nullptr && (ts->snap_flag & SCE_SNAP) != 0;
}

/** Ray-cast the active object's BVH. Also returns the surface normal when available. */
static bool paintcurve_raycast_active_object(bContext *C,
                                             const ViewContext &vc,
                                             const float mval[2],
                                             float r_hit_obj[3],
                                             float r_no_obj[3])
{
  if (vc.obact == nullptr || vc.obact->runtime->sculpt_session == nullptr ||
      vc.depsgraph == nullptr)
  {
    return false;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return false;
  }
  ToolSettings *ts = CTX_data_tool_settings(C);
  ViewContext vc_mut = vc;

  /* Preferred: a single ray-cast yields both the position and the normal. */
  const Base *base = CTX_data_active_base(C);
  if (base != nullptr && bke::object::pbvh_get(*vc.obact) != nullptr) {
    const std::optional<ed::sculpt_paint::CursorGeometryInfo> gi =
        ed::sculpt_paint::cursor_geometry_info_update(*vc.depsgraph,
                                                      *paint,
                                                      ts ? ts->sculpt : nullptr,
                                                      vc_mut,
                                                      base,
                                                      float2(mval[0], mval[1]),
                                                      false);
    if (!gi.has_value()) {
      return false;
    }
    copy_v3_v3(r_hit_obj, gi->location);
    copy_v3_v3(r_no_obj, gi->normal);
    return true;
  }

  /* Fallback: position only, so the normal falls back to the view direction. */
  const Brush *brush = BKE_paint_brush(paint);
  bool hit = false;
  if (ts != nullptr && ts->sculpt != nullptr) {
    hit = ed::sculpt_paint::stroke_get_location_bvh(
        *vc.depsgraph, vc_mut, *ts->sculpt, brush, r_hit_obj, mval, false);
  }
  else {
    hit = ed::sculpt_paint::stroke_get_location_bvh(
        *vc.depsgraph, vc_mut, *paint, brush, r_hit_obj, mval, false);
  }
  if (hit) {
    copy_v3_v3(r_no_obj, paintcurve_view_normal_obj(vc));
  }
  return hit;
}

/* Documented in `paint_curve_intern.hh`. */
bool paintcurve_surface_place(bContext *C,
                              PaintCurveSnapContext *snap_ctx,
                              const ViewContext &vc,
                              const float mval[2],
                              const float prev_co_world[3],
                              const bool use_depth_fallback,
                              float r_co_obj[3],
                              float r_no_obj[3])
{
  copy_v3_v3(r_no_obj, paintcurve_view_normal_obj(vc));

  /* Level 0: scene snap elements (vertex / edge / face), opt-in via the header Snap toggle. */
  if (paintcurve_wants_element_snap(C)) {
    PaintCurveSnapContext *ctx = snap_ctx;
    const bool own_ctx = (ctx == nullptr);
    if (own_ctx) {
      ctx = ED_paintcurve_snap_context_create();
    }
    const bool ok = ED_paintcurve_snap_point(
        C, ctx, vc.depsgraph, vc.v3d, vc.region, vc.obact, mval, prev_co_world, r_co_obj);
    if (own_ctx) {
      ED_paintcurve_snap_context_destroy(ctx);
    }
    if (ok) {
      /* The element snap yields a position but no normal, so ray-cast purely for the
       * normal and keep the snapped position. */
      float unused_co[3];
      float no_obj[3];
      if (paintcurve_raycast_active_object(C, vc, mval, unused_co, no_obj)) {
        copy_v3_v3(r_no_obj, no_obj);
      }
      return true;
    }
  }

  /* Levels 1-2: the active object's surface. */
  if (paintcurve_raycast_active_object(C, vc, mval, r_co_obj, r_no_obj)) {
    return true;
  }

  /* Level 3: depth of the nearest visible geometry. This needs a depth-buffer refresh, so it
   * is limited to one-shot placement and skipped during modal drags. */
  if (use_depth_fallback && vc.region != nullptr && vc.v3d != nullptr && vc.obact != nullptr &&
      vc.depsgraph != nullptr)
  {
    ViewDepths *depths = nullptr;
    ED_view3d_depth_override(
        vc.depsgraph, vc.region, vc.v3d, vc.obact, V3D_DEPTH_NO_GPENCIL, false, &depths);
    const int mval_i[2] = {int(mval[0]), int(mval[1])};
    float world_co[3];
    const bool ok = ED_view3d_autodist(vc.region, vc.v3d, mval_i, world_co, nullptr);
    ED_view3d_depths_free(depths);
    if (ok) {
      copy_v3_v3(r_co_obj,
                 math::transform_point(vc.obact->world_to_object(), float3(world_co)));
      return true;
    }
  }

  /* Level 4: plane through the object origin. Matches the pre-existing behavior. */
  if (vc.obact != nullptr) {
    float ob_origin_world[3];
    copy_v3_v3(ob_origin_world, vc.obact->object_to_world().location());
    float world_co[3];
    ED_view3d_win_to_3d(vc.v3d, vc.region, ob_origin_world, mval, world_co);
    copy_v3_v3(r_co_obj, math::transform_point(vc.obact->world_to_object(), float3(world_co)));
  }
  else {
    zero_v3(r_co_obj);
  }
  return false;
}

void paintcurve_geometry_add_point(bke::CurvesGeometry &geom,
                                   const float3 &co,
                                   const float3 &surface_normal,
                                   const bool create_new_spline,
                                   int &active_curve,
                                   int &add_index)
{
  int global_insert_idx;
  int add_index_in_spline;

  /* The shift loop below only covers built-in attributes, so surface normals are moved
   * separately. Written back after the mutable spans go out of scope: adding an attribute
   * while they are alive would invalidate them. */
  Vector<float3> normals_shifted;

  if (create_new_spline) {
    active_curve = paintcurve_geometry_add_spline(geom, 1);
    global_insert_idx = geom.points_num() - 1;
    add_index_in_spline = 0;
  }
  else {
    const OffsetIndices<int> pbc = geom.points_by_curve();
    const IndexRange spline_range = pbc[active_curve];
    add_index_in_spline = std::clamp(add_index, 0, int(spline_range.size()));
    global_insert_idx = spline_range.start() + add_index_in_spline;

    const int old_total = geom.points_num();

    normals_shifted.reserve(old_total + 1);
    for (const int i : IndexRange(old_total)) {
      normals_shifted.append(paintcurve_geom_get_surface_normal(geom, i));
    }
    normals_shifted.insert(global_insert_idx, float3(0.0f, 0.0f, 1.0f));

    geom.resize(old_total + 1, geom.curves_num());

    /* Shift offsets for all splines that come after the active one, and the sentinel.
     * `resize` with the same curve count skips the sentinel update, so we must do it here. */
    MutableSpan<int> offsets = geom.offsets_for_write();
    for (int ci = active_curve + 1; ci <= geom.curves_num(); ci++) {
      offsets[ci]++;
    }

    /* Shift point attributes to make room at global_insert_idx. */
    MutableSpan<float3> positions = geom.positions_for_write();
    MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
    MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();
    MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
    MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
    MutableSpan<float> radii = geom.radius_for_write();
    for (int i = old_total; i > global_insert_idx; i--) {
      positions[i] = positions[i - 1];
      handles_left[i] = handles_left[i - 1];
      handles_right[i] = handles_right[i - 1];
      types_left[i] = types_left[i - 1];
      types_right[i] = types_right[i - 1];
      radii[i] = radii[i - 1];
    }
  }

  for (const int i : IndexRange(normals_shifted.size())) {
    paintcurve_geom_set_surface_normal(geom, i, normals_shifted[i]);
  }

  /* Set new point data. */
  geom.positions_for_write()[global_insert_idx] = co;
  geom.handle_positions_left_for_write()[global_insert_idx] = co;
  geom.handle_positions_right_for_write()[global_insert_idx] = co;
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  types_left[global_insert_idx] = BEZIER_HANDLE_ALIGN;
  types_right[global_insert_idx] = BEZIER_HANDLE_ALIGN;
  geom.radius_for_write()[global_insert_idx] = 1.0f;
  paintcurve_geom_set_surface_normal(geom, global_insert_idx, surface_normal);

  /* Preserve junction handle types when extending an existing spline. */
  if (!create_new_spline) {
    if (add_index_in_spline > 0) {
      const int prev_idx = global_insert_idx - 1;
      types_right[prev_idx] = paintcurve_aligned_or_free_handle_type(types_right[prev_idx]);
      types_left[global_insert_idx] = BEZIER_HANDLE_ALIGN;
    }
    else if (add_index_in_spline == 0) {
      const OffsetIndices<int> pbc = geom.points_by_curve();
      if (pbc[active_curve].size() > 1) {
        const int next_idx = global_insert_idx + 1;
        types_left[next_idx] = paintcurve_aligned_or_free_handle_type(types_left[next_idx]);
        types_right[global_insert_idx] = BEZIER_HANDLE_ALIGN;
      }
    }
  }

  /* Clear all selection, then mark the new point's active handle. */
  paintcurve_geom_set_all_selection(geom, 0);
  {
    const OffsetIndices<int> pbc = geom.points_by_curve();
    const int spline_size = pbc[active_curve].size();
    uint8_t new_sel;
    if (add_index_in_spline != 0) {
      /* Appending — select right handle for next placement. */
      geom.handle_types_right_for_write()[global_insert_idx] = BEZIER_HANDLE_ALIGN;
      new_sel = 0x04; /* bit 2 = right handle */
    }
    else {
      /* Prepending — select left handle for next placement. */
      geom.handle_types_left_for_write()[global_insert_idx] = BEZIER_HANDLE_ALIGN;
      new_sel = 0x01; /* bit 0 = left handle */
    }
    paintcurve_geom_set_selection(geom, global_insert_idx, new_sel);

    /* Advance add_index within the active spline for the next click. */
    add_index = (add_index_in_spline || spline_size == 1) ? (add_index_in_spline + 1) : 0;
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();
}

static void paintcurve_point_add(bContext *C,
                                 wmOperator *op,
                                 const int loc[2],
                                 const bool snap_to_surface = false)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  Main *bmain = CTX_data_main(C);
  wmWindow *window = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);

  if (!br) {
    return;
  }

  PaintCurve *pc = br->paint_curve;
  if (!pc) {
    br->paint_curve = pc = paintcurve_for_brush_add(bmain, DATA_("PaintCurve"), br);
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  ViewContext vc = {};
  if (pc->use_3d_space && rv3d) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    vc = ED_view3d_viewcontext_init(C, depsgraph);
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  const int curves_num = paintcurve_geometry_runtime_is_initialized(geom) ? geom.curves_num() : 0;

  /* Compute 3D object-space position and the surface normal at that position. */
  float obj_co[3];
  float obj_no[3] = {0.0f, 0.0f, 1.0f};
  if (pc->use_3d_space && rv3d && vc.obact) {
    const float mval_fl[2] = {float(loc[0]), float(loc[1])};
    if (snap_to_surface) {
      /* One-shot placement, so the depth-buffer level is affordable here. */
      paintcurve_surface_place(C, nullptr, vc, mval_fl, nullptr, true, obj_co, obj_no);
    }
    else {
      float ob_origin_world[3];
      copy_v3_v3(ob_origin_world, vc.obact->object_to_world().location());
      float world_co[3];
      ED_view3d_win_to_3d(vc.v3d, vc.region, ob_origin_world, mval_fl, world_co);
      const float (*world_to_ob)[4] = vc.obact->world_to_object().ptr();
      mul_v3_m4v3(obj_co, world_to_ob, world_co);
      copy_v3_v3(obj_no, paintcurve_view_normal_obj(vc));
    }
  }
  else {
    obj_co[0] = float(loc[0]);
    obj_co[1] = float(loc[1]);
    obj_co[2] = 0.0f;
  }

  /* A sentinel value (>= curves_num) means create a new spline on this click. */
  const bool create_new_spline = (pc->active_curve >= curves_num);

  /* Respect the selected endpoint when extending (Curve Edit and Stroke Method: Curve). */
  paintcurve_update_add_index_from_selection(pc, geom);

  int active_curve = pc->active_curve;
  int add_index = pc->add_index;
  paintcurve_geometry_add_point(
      geom, float3(obj_co), float3(obj_no), create_new_spline, active_curve, add_index);
  pc->active_curve = active_curve;
  pc->add_index = add_index;

  if (pc->use_3d_space) {
    paintcurve_sync_to_source_if_3d(C, pc);
  }

  ED_paintcurve_undo_push_end(C);
  BKE_brush_tag_unsaved_changes(br);
  WM_paint_cursor_tag_redraw(window, region);
}

static wmOperatorStatus paintcurve_add_point_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  const int loc[2] = {event->mval[0], event->mval[1]};
  paintcurve_point_add(C, op, loc, true);
  RNA_int_set_array(op->ptr, "location", loc);
  RNA_boolean_set(op->ptr, "snap_to_surface", true);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus paintcurve_add_point_exec(bContext *C, wmOperator *op)
{
  int loc[2];

  if (RNA_struct_property_is_set(op->ptr, "location")) {
    RNA_int_get_array(op->ptr, "location", loc);
    paintcurve_point_add(C, op, loc, RNA_boolean_get(op->ptr, "snap_to_surface"));
    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

void PAINTCURVE_OT_add_point(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add New Paint Curve Point";
  ot->description = ot->name;
  ot->idname = "PAINTCURVE_OT_add_point";

  /* API callbacks. */
  ot->invoke = paintcurve_add_point_invoke;
  ot->exec = paintcurve_add_point_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  /* Undo is pushed explicitly; avoid a second PAINTCURVE step from #OPTYPE_UNDO. */
  ot->flag = OPTYPE_REGISTER;

  /* properties */
  RNA_def_int_vector(ot->srna,
                     "location",
                     2,
                     nullptr,
                     0,
                     SHRT_MAX,
                     "Location",
                     "Location of vertex in area space",
                     0,
                     SHRT_MAX);
  RNA_def_boolean(ot->srna,
                  "snap_to_surface",
                  true,
                  "Snap to Surface",
                  "Place the point on the active object's surface");
}

/** Invoke handle slide after adding a point (matches #PAINTCURVE_OT_add_point_slide macro). */
static wmOperatorStatus paintcurve_invoke_slide_after_point_add(bContext *C, const wmEvent *event)
{
  wmOperatorType *ot_slide = WM_operatortype_find("PAINTCURVE_OT_slide", false);
  if (ot_slide == nullptr) {
    return OPERATOR_FINISHED;
  }

  PointerRNA ptr = WM_operator_properties_create_ptr(ot_slide);
  RNA_boolean_set(&ptr, "align", true);
  RNA_boolean_set(&ptr, "select", false);
  const wmOperatorStatus ret = WM_operator_name_call_ptr(
      C, ot_slide, wm::OpCallContext::InvokeDefault, &ptr, event);
  WM_operator_properties_free(&ptr);
  return ret;
}

/** Try to insert a control point on the bezier segment under the cursor. */
static bool paintcurve_try_insert_point_at_mouse(bContext *C,
                                                 wmOperator *op,
                                                 PaintCurve *pc,
                                                 const float loc_fl[2])
{
  if (pc == nullptr) {
    return false;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);

  /* Do not insert when a control point is closer than the segment. */
  char point_selflag;
  if (paintcurve_find_in_screen_points(screen_points.as_span(),
                                       loc_fl,
                                       false,
                                       PAINT_CURVE_HOVER_THRESHOLD,
                                       &point_selflag) >= 0)
  {
    return false;
  }

  int segment_index = -1;
  float edge_t = 0.0f;
  float best_dist_sq = square_f(PAINT_CURVE_INSERT_SEGMENT_THRESHOLD);
  const float2 mval(loc_fl[0], loc_fl[1]);

  paintcurve_foreach_bezier_segment(pc, [&](const int point_a, const int point_b) {
    Vector<float2> polyline;
    paintcurve_build_screen_segment_polyline(pc, &vc, point_a, point_b, screen_points, polyline);
    const float dist_sq = ed::sculpt_paint::ED_paint_curve_polyline_distance_sq(polyline, mval);
    if (dist_sq < best_dist_sq) {
      float bezier_t = 0.0f;
      if (paintcurve_bezier_param_at_screen_pos_on_segment(
              &vc, pc, loc_fl, point_a, point_b, screen_points, bezier_t))
      {
        best_dist_sq = dist_sq;
        segment_index = point_a;
        edge_t = bezier_t;
      }
    }
  });

  if (segment_index < 0) {
    return false;
  }

  /* Clicks near segment endpoints should extend the spline, not subdivide it. */
  if (edge_t < 0.1f || edge_t > 0.9f) {
    return false;
  }

  if (!paintcurve_insert_point_at_segment(C, op, pc, segment_index, edge_t)) {
    return false;
  }
  paintcurve_skip_next_slide = true;
  return true;
}

static wmOperatorStatus paintcurve_insert_or_add_point_invoke(bContext *C,
                                                              wmOperator *op,
                                                              const wmEvent *event)
{
  Brush *br = nullptr;
  PaintCurve *pc = paintcurve_active_from_context(C, &br);

  const int loc[2] = {event->mval[0], event->mval[1]};
  const float loc_fl[2] = {float(loc[0]), float(loc[1])};

  if (paintcurve_try_insert_point_at_mouse(C, op, pc, loc_fl)) {
    return OPERATOR_FINISHED;
  }

  paintcurve_point_add(C, op, loc, true);
  RNA_int_set_array(op->ptr, "location", loc);
  return paintcurve_invoke_slide_after_point_add(C, event);
}

void PAINTCURVE_OT_insert_or_add_point(wmOperatorType *ot)
{
  ot->name = "Insert or Add Paint Curve Point";
  ot->description =
      "Insert a control point on the curve segment under the cursor, or add a new point to extend "
      "the spline; drag to adjust the new point's handle";
  ot->idname = "PAINTCURVE_OT_insert_or_add_point";

  ot->invoke = paintcurve_insert_or_add_point_invoke;
  ot->poll = paint_curve_poll;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_int_vector(ot->srna,
                     "location",
                     2,
                     nullptr,
                     0,
                     SHRT_MAX,
                     "Location",
                     "Location of vertex in area space",
                     0,
                     SHRT_MAX);
}

static wmOperatorStatus paintcurve_new_spline_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;

  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  const int curves_num = paintcurve_geometry_runtime_is_initialized(geom) ? geom.curves_num() : 0;

  /* Set sentinel: next add-point click will create the new spline. */
  pc->active_curve = curves_num;
  pc->add_index = 0;

  ED_paintcurve_undo_push_end(C);
  BKE_brush_tag_unsaved_changes(br);

  wmWindow *window = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);
  WM_paint_cursor_tag_redraw(window, region);

  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_new_spline(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "New Paint Curve Spline";
  ot->description = "Start a new independent spline in the paint curve";
  ot->idname = "PAINTCURVE_OT_new_spline";

  /* API callbacks. */
  ot->exec = paintcurve_new_spline_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus paintcurve_clear_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;

  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return OPERATOR_CANCELLED;
  }
  if (pc->geometry.wrap().points_num() == 0) {
    /* Nothing to clear: refuse rather than push an undo step that changes nothing. */
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);
  ED_paintcurve_geometry_clear(pc);
  if (pc->use_3d_space) {
    paintcurve_sync_to_source_if_3d(C, pc);
  }
  ED_paintcurve_undo_push_end(C);

  BKE_brush_tag_unsaved_changes(br);
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Paint Curve";
  ot->description = "Remove every control point, leaving the paint curve empty";
  ot->idname = "PAINTCURVE_OT_clear";

  /* API callbacks. */
  ot->exec = paintcurve_clear_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus paintcurve_delete_point_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  wmWindow *window = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);

  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  Vector<int> points_to_delete;
  for (const int i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, i) & 0x07) {
      points_to_delete.append(i);
    }
  }

  if (!points_to_delete.is_empty()) {
    IndexMaskMemory memory;
    const IndexMask delete_mask = IndexMask::from_indices<int>(points_to_delete.as_span(),
                                                               memory);
    paintcurve_geometry_remove_points(geom, delete_mask);
    pc->active_curve = paintcurve_active_curve_get(pc);

    if (pc->use_3d_space) {
      paintcurve_sync_to_source_if_3d(C, pc);
    }
  }

  ED_paintcurve_undo_push_end(C);
  BKE_brush_tag_unsaved_changes(br);
  WM_paint_cursor_tag_redraw(window, region);
  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_delete_point(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Paint Curve Point";
  ot->description = ot->name;
  ot->idname = "PAINTCURVE_OT_delete_point";

  /* API callbacks. */
  ot->exec = paintcurve_delete_point_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = 0;
}

static wmOperatorStatus paintcurve_duplicate_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  wmWindow *window = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);

  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return OPERATOR_CANCELLED;
  }

  if (!paintcurve_geometry_any_point_selected(geom)) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  const int old_curves = geom.curves_num();
  const int num_added = paintcurve_geometry_duplicate_selected_points(geom);
  if (num_added == 0) {
    ED_paintcurve_undo_push_end(C);
    return OPERATOR_CANCELLED;
  }

  pc->active_curve = old_curves;

  if (pc->use_3d_space) {
    paintcurve_sync_to_source_if_3d(C, pc);
  }

  ED_paintcurve_undo_push_end(C);
  BKE_brush_tag_unsaved_changes(br);
  WM_paint_cursor_tag_redraw(window, region);

  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_duplicate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Duplicate Paint Curve Points";
  ot->description =
      "Duplicate selected control points as new splines (contiguous selections become segments)";
  ot->idname = "PAINTCURVE_OT_duplicate";

  /* API callbacks. */
  ot->exec = paintcurve_duplicate_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool paintcurve_point_select(bContext *C,
                                    wmOperator *op,
                                    const int loc[2],
                                    bool toggle,
                                    bool extend,
                                    const bool cycle_on_pivot)
{
  wmWindow *window = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  const float loc_fl[2] = {float(loc[0]), float(loc[1])};

  if (!br) {
    return false;
  }
  PaintCurve *pc = br->paint_curve;
  if (!pc) {
    return false;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }

  /* Build transient screen-space projection for hit-testing. */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);

  if (!toggle) {
    char pre_selflag;
    const int pre_idx = paintcurve_find_in_screen_points(
        screen_points.as_span(), loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &pre_selflag);
    if (pre_idx >= 0 && cycle_on_pivot && !extend && pre_selflag == SEL_F2 &&
        (paintcurve_geom_get_selection(geom, pre_idx) & 0x07))
    {
      paintcurve_cycle_handle_type_at_point(C, op, pc, pre_idx);
      WM_paint_cursor_tag_redraw(window, region);
      return true;
    }
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  if (toggle) {
    const bool any_sel = paintcurve_geom_any_selected(geom);
    paintcurve_geom_set_all_selection(geom, any_sel ? 0x00 : 0x07);
  }
  else {
    char selflag;
    const int found_idx = paintcurve_find_in_screen_points(
        screen_points.as_span(), loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &selflag);

    if (found_idx >= 0) {
      const int spline_idx = paintcurve_curve_of_point(pc, found_idx);
      if (spline_idx >= 0) {
        pc->active_curve = spline_idx;
      }

      uint8_t sel_bit;
      if (selflag == SEL_F1) {
        sel_bit = 0x01;
      }
      else if (selflag == SEL_F2) {
        sel_bit = 0x02;
      }
      else {
        sel_bit = 0x04;
      }

      if (!extend) {
        paintcurve_geom_set_all_selection(geom, 0);
        paintcurve_geom_set_selection(geom, found_idx, sel_bit);
      }
      else {
        const uint8_t cur = paintcurve_geom_get_selection(geom, found_idx);
        paintcurve_geom_set_selection(geom, found_idx, cur ^ sel_bit);
      }
      paintcurve_update_add_index_from_selection(pc, geom);
    }
    else {
      ED_paintcurve_undo_push_end(C);
      return false;
    }
  }

  ED_paintcurve_undo_push_end(C);
  WM_paint_cursor_tag_redraw(window, region);
  return true;
}

static wmOperatorStatus paintcurve_select_point_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  const int loc[2] = {event->mval[0], event->mval[1]};
  const float loc_fl[2] = {float(loc[0]), float(loc[1])};
  bool toggle = RNA_boolean_get(op->ptr, "toggle");
  bool extend = RNA_boolean_get(op->ptr, "extend");

  if (event->val == KM_DBL_CLICK && !toggle && !extend) {
    Paint *paint = BKE_paint_get_active_from_context(C);
    Brush *br = BKE_paint_brush(paint);
    PaintCurve *pc = br ? br->paint_curve : nullptr;
    if (pc && paintcurve_geometry_is_valid(pc->geometry.wrap())) {
      Depsgraph *dg = CTX_data_depsgraph_pointer(C);
      ViewContext vc_dbl = ED_view3d_viewcontext_init(C, dg);
      Vector<PaintCurvePoint> sp_dbl;
      paintcurve_build_screen_points(pc, &vc_dbl, sp_dbl);
      char selflag;
      const int found = paintcurve_find_in_screen_points(
          sp_dbl.as_span(), loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &selflag);
      if (found >= 0 && selflag == SEL_F2) {
        ARegion *region = CTX_wm_region(C);
        wmWindow *window = CTX_wm_window(C);
        paintcurve_cycle_handle_type_at_point(C, op, pc, found);
        WM_paint_cursor_tag_redraw(window, region);
        RNA_int_set_array(op->ptr, "location", loc);
        return OPERATOR_FINISHED;
      }
    }
  }

  if (paintcurve_point_select(C, op, loc, toggle, extend, true)) {
    RNA_int_set_array(op->ptr, "location", loc);
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

static wmOperatorStatus paintcurve_select_point_exec(bContext *C, wmOperator *op)
{
  int loc[2];

  if (RNA_struct_property_is_set(op->ptr, "location")) {
    bool toggle = RNA_boolean_get(op->ptr, "toggle");
    bool extend = RNA_boolean_get(op->ptr, "extend");
    RNA_int_get_array(op->ptr, "location", loc);
    if (paintcurve_point_select(C, op, loc, toggle, extend, false)) {
      return OPERATOR_FINISHED;
    }
  }

  return OPERATOR_CANCELLED;
}

void PAINTCURVE_OT_select(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Select Paint Curve Point";
  ot->description = "Select a paint curve point";
  ot->idname = "PAINTCURVE_OT_select";

  /* API callbacks. */
  ot->invoke = paintcurve_select_point_invoke;
  ot->exec = paintcurve_select_point_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER;

  /* properties */
  RNA_def_int_vector(ot->srna,
                     "location",
                     2,
                     nullptr,
                     0,
                     SHRT_MAX,
                     "Location",
                     "Location of vertex in area space",
                     0,
                     SHRT_MAX);
  prop = RNA_def_boolean(ot->srna, "toggle", false, "Toggle", "(De)select all");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend selection");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

enum ePaintCurveSlideModal {
  PAINTCURVE_MODAL_MOVE_HANDLE = 0,
  PAINTCURVE_MODAL_MOVE_ENTIRE = 1,
  PAINTCURVE_MODAL_SNAP_ANGLE = 2,
};

/* Snaps to the closest diagonal, horizontal or vertical. Same as Curves Pen. */
static float2 paintcurve_snap_8_angles(const float2 &p)
{
  using namespace math;
  const float sin225 = sin(AngleRadian::from_degree(22.5f));
  return sign(p) * length(p) * normalize(sign(normalize(abs(p)) - sin225) + 1.0f);
}

/** Snap handle direction using #ToolSettings.snap_angle_increment_3d from #VIEW3D_PT_snapping. */
static float2 paintcurve_snap_handle_offset(const float2 &p, const float increment_rad)
{
  using namespace math;
  const float len = length(p);
  if (len == 0.0f) {
    return p;
  }
  if (increment_rad <= 0.0f) {
    return paintcurve_snap_8_angles(p);
  }
  const float angle = atan2(p.y, p.x);
  const float snapped = increment_rad * roundf(angle / increment_rad);
  return float2(cos(snapped), sin(snapped)) * len;
}

static void paintcurve_object_to_screen(const ViewContext *vc,
                                        const float ob_to_world[4][4],
                                        const float ob_co[3],
                                        float r_screen[2])
{
  float world_co[3];
  mul_v3_m4v3(world_co, ob_to_world, ob_co);
  ED_view3d_project_v2(vc->region, world_co, r_screen);
}

static void paintcurve_screen_to_object(const ViewContext *vc,
                                        const float pivot_world[3],
                                        const float world_to_ob[4][4],
                                        const float screen_co[2],
                                        float r_ob_co[3])
{
  float world_co[3];
  ED_view3d_win_to_3d(vc->v3d, vc->region, pivot_world, screen_co, world_co);
  mul_v3_m4v3(r_ob_co, world_to_ob, world_co);
}


static void paintcurve_update_edge_hit(const float point[2],
                                       const float point1[2],
                                       const float point2[2],
                                       const int segment_index,
                                       const int resolu_index,
                                       float *r_min_dist,
                                       int *r_segment_index,
                                       float *r_param)
{
  float edge[2], vec1[2], vec2[2];
  sub_v2_v2v2(edge, point1, point2);
  const float edge_len = len_v2(edge);
  /* Skip degenerate (zero-length) polyline segments to avoid division by zero. */
  if (edge_len < 1e-6f) {
    return;
  }
  sub_v2_v2v2(vec1, point1, point);
  sub_v2_v2v2(vec2, point, point2);
  const float len_vec1 = len_v2(vec1);
  const float len_vec2 = len_v2(vec2);
  const float dot1 = dot_v2v2(edge, vec1);
  const float dot2 = dot_v2v2(edge, vec2);

  if ((dot1 > 0) == (dot2 > 0)) {
    const float perp_dist = len_vec1 * sinf(angle_v2v2(vec1, edge));
    if (*r_min_dist > perp_dist) {
      *r_min_dist = perp_dist;
      *r_segment_index = segment_index;
      *r_param = resolu_index + len_vec1 * cos_v2v2v2(point, point1, point2) / edge_len;
    }
  }
  else if (*r_min_dist > len_vec2) {
    *r_min_dist = len_vec2;
    *r_segment_index = segment_index;
    *r_param = resolu_index;
  }
}

bool paintcurve_bezier_param_at_screen_pos_on_segment(const ViewContext *vc,
                                                      PaintCurve *pc,
                                                      const float pos[2],
                                                      const int point_index_a,
                                                      const int point_index_b,
                                                      const Span<PaintCurvePoint> screen_points,
                                                      float &r_bezier_t,
                                                      float *r_min_dist)
{
  Vector<float2> polyline;
  paintcurve_build_screen_segment_polyline(
      pc, vc, point_index_a, point_index_b, screen_points, polyline);
  if (polyline.size() < 2) {
    return false;
  }

  float point1[2], point2[2];
  copy_v2_v2(point1, polyline[0]);
  float segment_min_dist = len_v2v2(pos, point1);
  /* paintcurve_update_edge_hit always writes `point_index_a` into r_segment_index here;
   * the value is never read after the loop, so use a discarded local. */
  int discarded_segment_index = point_index_a;
  float param = 0.0f;

  const int segment_steps = int(polyline.size()) - 1;
  for (int j = 0; j < segment_steps; j++) {
    copy_v2_v2(point2, polyline[j + 1]);
    paintcurve_update_edge_hit(pos,
                               point1,
                               point2,
                               point_index_a,
                               j,
                               &segment_min_dist,
                               &discarded_segment_index,
                               &param);
    copy_v2_v2(point1, point2);
  }

  r_bezier_t = segment_steps > 0 ? param / float(segment_steps) : 0.0f;
  if (r_min_dist != nullptr) {
    *r_min_dist = segment_min_dist;
  }
  return true;
}

static void paintcurve_find_closest_on_bezier_segment(const ViewContext *vc,
                                                      PaintCurve *pc,
                                                      const float pos[2],
                                                      const int point_index_a,
                                                      const int point_index_b,
                                                      const Span<PaintCurvePoint> screen_points,
                                                      const int segment_index,
                                                      float *r_min_dist,
                                                      int *r_best_segment,
                                                      float *r_best_param)
{
  float bezier_t = 0.0f;
  float segment_min_dist = 0.0f;
  if (!paintcurve_bezier_param_at_screen_pos_on_segment(vc,
                                                        pc,
                                                        pos,
                                                        point_index_a,
                                                        point_index_b,
                                                        screen_points,
                                                        bezier_t,
                                                        &segment_min_dist))
  {
    return;
  }

  if (*r_min_dist > segment_min_dist) {
    *r_min_dist = segment_min_dist;
    *r_best_segment = segment_index;
    *r_best_param = bezier_t;
  }
}

static bool paintcurve_find_closest_segment(PaintCurve *pc,
                                            const ViewContext *vc,
                                            const Span<PaintCurvePoint> screen_points,
                                            const float pos[2],
                                            const float threshold,
                                            int *r_segment_index,
                                            int *r_segment_index_next,
                                            float *r_edge_t)
{
  if (screen_points.size() < 2) {
    return false;
  }

  float min_dist = threshold;
  int best_segment = -1;
  int best_segment_next = -1;
  float best_param = 0.0f;

  paintcurve_foreach_bezier_segment(pc, [&](const int point_a, const int point_b) {
    float segment_min_dist = min_dist;
    int segment_start = point_a;
    float segment_param = 0.0f;
    paintcurve_find_closest_on_bezier_segment(vc,
                                              pc,
                                              pos,
                                              point_a,
                                              point_b,
                                              screen_points,
                                              point_a,
                                              &segment_min_dist,
                                              &segment_start,
                                              &segment_param);
    if (min_dist > segment_min_dist && segment_start == point_a) {
      min_dist = segment_min_dist;
      best_segment = point_a;
      best_segment_next = point_b;
      best_param = segment_param;
    }
  });

  if (best_segment < 0 || best_segment_next < 0) {
    return false;
  }

  *r_segment_index = best_segment;
  *r_segment_index_next = best_segment_next;
  *r_edge_t = best_param;
  return true;
}

/** Match #remove_handle_movement_constraints in editcurve_pen.cc. */
static void paintcurve_remove_handle_movement_constraints(int8_t &type_left,
                                                            int8_t &type_right,
                                                            const bool adjust_left,
                                                            const bool adjust_right)
{
  if (adjust_left) {
    if (type_left == BEZIER_HANDLE_VECTOR) {
      type_left = BEZIER_HANDLE_FREE;
    }
    if (type_left == BEZIER_HANDLE_AUTO) {
      type_left = BEZIER_HANDLE_ALIGN;
      type_right = BEZIER_HANDLE_ALIGN;
    }
  }
  if (adjust_right) {
    if (type_right == BEZIER_HANDLE_VECTOR) {
      type_right = BEZIER_HANDLE_FREE;
    }
    if (type_right == BEZIER_HANDLE_AUTO) {
      type_left = BEZIER_HANDLE_ALIGN;
      type_right = BEZIER_HANDLE_ALIGN;
    }
  }
}

void paintcurve_apply_segment_move_3d(bke::CurvesGeometry &geom,
                                      const int point_i1,
                                      const int point_i2,
                                      const float segment_t,
                                      const ViewContext *vc,
                                      const float world_to_ob[4][4],
                                      const float mval[2],
                                      const float depth_world[3])
{
  MutableSpan<float3> positions = geom.positions_for_write();
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
  MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();

  const float t = max_ff(min_ff(segment_t, 0.9f), 0.1f);
  const float t_sq = t * t;
  const float t_cu = t_sq * t;
  const float one_minus_t = 1.0f - t;
  const float one_minus_t_sq = one_minus_t * one_minus_t;
  const float one_minus_t_cu = one_minus_t_sq * one_minus_t;
  const float denom = 3.0f * one_minus_t * t;
  if (denom == 0.0f) {
    return;
  }

  float3 Pm;
  paintcurve_screen_to_object(vc, depth_world, world_to_ob, mval, Pm);

  const float3 P0 = positions[point_i1];
  const float3 P3 = positions[point_i2];
  const float3 p1 = handles_right[point_i1];
  const float3 p2 = handles_left[point_i2];
  const float3 k2 = p1 - p2;

  const float3 P1 = (Pm - one_minus_t_cu * P0 - t_cu * P3) / denom + k2 * t;
  const float3 P2 = P1 - k2;

  handles_right[point_i1] = P1;
  handles_left[point_i2] = P2;

  /* Preserve handle types (match legacy Curve Pen segment move). */
  paintcurve_remove_handle_movement_constraints(
      types_left[point_i1], types_right[point_i1], false, true);
  paintcurve_remove_handle_movement_constraints(
      types_left[point_i2], types_right[point_i2], true, false);

  if (types_right[point_i1] == BEZIER_HANDLE_ALIGN) {
    handles_left[point_i1] = 2.0f * P0 - P1;
  }
  if (types_left[point_i2] == BEZIER_HANDLE_ALIGN) {
    handles_right[point_i2] = 2.0f * P3 - P2;
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
}

/** Viewport-bound segment slide: geometry stores screen-space coordinates. */
static void paintcurve_apply_segment_move_2d(bke::CurvesGeometry &geom,
                                             const int point_i1,
                                             const int point_i2,
                                             const float segment_t,
                                             const float mval[2])
{
  MutableSpan<float3> positions = geom.positions_for_write();
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
  MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();

  const float t = max_ff(min_ff(segment_t, 0.9f), 0.1f);
  const float t_sq = t * t;
  const float t_cu = t_sq * t;
  const float one_minus_t = 1.0f - t;
  const float one_minus_t_sq = one_minus_t * one_minus_t;
  const float one_minus_t_cu = one_minus_t_sq * one_minus_t;
  const float denom = 3.0f * one_minus_t * t;
  if (denom == 0.0f) {
    return;
  }

  const float3 P0 = positions[point_i1];
  const float3 P3 = positions[point_i2];
  const float3 p1 = handles_right[point_i1];
  const float3 p2 = handles_left[point_i2];
  const float3 k2 = p1 - p2;
  const float3 Pm(mval[0], mval[1], 0.0f);

  const float3 P1 = (Pm - one_minus_t_cu * P0 - t_cu * P3) / denom + k2 * t;
  const float3 P2 = P1 - k2;

  handles_right[point_i1] = P1;
  handles_left[point_i2] = P2;
  types_right[point_i1] = BEZIER_HANDLE_FREE;
  types_left[point_i2] = BEZIER_HANDLE_FREE;
  if (types_left[point_i1] == BEZIER_HANDLE_ALIGN) {
    types_left[point_i1] = BEZIER_HANDLE_FREE;
  }
  if (types_right[point_i2] == BEZIER_HANDLE_ALIGN) {
    types_right[point_i2] = BEZIER_HANDLE_FREE;
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
}

int paintcurve_geometry_insert_point_at_segment(bke::CurvesGeometry &geom,
                                                const int segment_index,
                                                const int segment_index_next,
                                                const int active_curve,
                                                const float edge_t)
{
  const int point_i2 = segment_index_next;
  const int old_tot = geom.points_num();
  const int insert_index = segment_index + 1;
  const int8_t type_prev_right = geom.handle_types_right()[segment_index];
  const int8_t type_next_left = geom.handle_types_left()[point_i2];
  const bool segment_is_vector = bke::curves::bezier::segment_is_vector(type_prev_right,
                                                                        type_next_left);

  /* Compute insertion before resize (spans become invalid after resize). */
  const std::optional<Span<float3>> handles_right_opt = geom.handle_positions_right();
  const std::optional<Span<float3>> handles_left_opt = geom.handle_positions_left();
  if (!handles_right_opt.has_value() || !handles_left_opt.has_value()) {
    return -1;
  }
  const bke::curves::bezier::Insertion inserted = bke::curves::bezier::insert(
      geom.positions()[segment_index],
      handles_right_opt.value()[segment_index],
      handles_left_opt.value()[point_i2],
      geom.positions()[point_i2],
      edge_t);
  const float radius_a = geom.radius_for_write()[segment_index];
  const float radius_b = geom.radius_for_write()[point_i2];

  geom.resize(old_tot + 1, geom.curves_num());

  MutableSpan<int> offsets = geom.offsets_for_write();
  for (int ci = active_curve + 1; ci <= geom.curves_num(); ci++) {
    offsets[ci]++;
  }

  MutableSpan<float3> pos_w = geom.positions_for_write();
  MutableSpan<float3> hl_w = geom.handle_positions_left_for_write();
  MutableSpan<float3> hr_w = geom.handle_positions_right_for_write();
  MutableSpan<int8_t> tl_w = geom.handle_types_left_for_write();
  MutableSpan<int8_t> tr_w = geom.handle_types_right_for_write();
  MutableSpan<float> radii = geom.radius_for_write();
  for (int i = old_tot; i > insert_index; i--) {
    pos_w[i] = pos_w[i - 1];
    hl_w[i] = hl_w[i - 1];
    hr_w[i] = hr_w[i - 1];
    tl_w[i] = tl_w[i - 1];
    tr_w[i] = tr_w[i - 1];
    radii[i] = radii[i - 1];
  }

  /* Update segment boundary handles (match #subdivide_bezier_segment in subdivide_curves.cc). */
  hr_w[segment_index] = inserted.handle_prev;
  pos_w[insert_index] = inserted.position;
  hl_w[insert_index] = inserted.left_handle;
  hr_w[insert_index] = inserted.right_handle;
  radii[insert_index] = radius_a + (radius_b - radius_a) * edge_t;

  if (segment_is_vector) {
    tr_w[segment_index] = BEZIER_HANDLE_VECTOR;
    tl_w[insert_index] = BEZIER_HANDLE_VECTOR;
    tr_w[insert_index] = BEZIER_HANDLE_VECTOR;
    if (insert_index + 1 < geom.points_num()) {
      hl_w[insert_index + 1] = inserted.handle_next;
      tl_w[insert_index + 1] = BEZIER_HANDLE_VECTOR;
    }
  }
  else {
    tr_w[segment_index] = paintcurve_aligned_or_free_handle_type(type_prev_right);
    tl_w[insert_index] = BEZIER_HANDLE_ALIGN;
    tr_w[insert_index] = BEZIER_HANDLE_ALIGN;
    if (insert_index + 1 < geom.points_num()) {
      hl_w[insert_index + 1] = inserted.handle_next;
      tl_w[insert_index + 1] = paintcurve_aligned_or_free_handle_type(type_next_left);
    }
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();

  paintcurve_geom_set_all_selection(geom, 0);
  paintcurve_geom_set_selection(geom, insert_index, 0x02);

  return insert_index;
}

static bool paintcurve_insert_point_at_segment(
    bContext *C, wmOperator *op, PaintCurve *pc, const int segment_index, const float edge_t)
{
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }

  int segment_index_next = -1;
  paintcurve_foreach_bezier_segment(pc, [&](const int point_a, const int point_b) {
    if (point_a == segment_index) {
      segment_index_next = point_b;
    }
  });
  if (segment_index < 0 || segment_index_next < 0) {
    return false;
  }

  /* Mirror the core's own handles-presence guard here so a failure never triggers an undo push,
   * matching this function's original early-out ordering (before #ED_paintcurve_undo_push_begin). */
  if (!geom.handle_positions_right().has_value() || !geom.handle_positions_left().has_value()) {
    return false;
  }

  const int active_curve = paintcurve_curve_of_point(pc, segment_index);
  if (active_curve < 0) {
    return false;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  const int insert_index = paintcurve_geometry_insert_point_at_segment(
      geom, segment_index, segment_index_next, active_curve, edge_t);
  /* The handles-presence guard above already mirrors the core's own -- `insert_index` should never
   * be the core's `-1` failure sentinel here; assert rather than silently corrupt `add_index` if
   * the two guards are ever changed independently of each other. */
  BLI_assert(insert_index >= 0);

  pc->active_curve = active_curve;
  {
    const OffsetIndices<int> pbc = geom.points_by_curve();
    const int idx_in_spline = insert_index - pbc[active_curve].start();
    pc->add_index = idx_in_spline + 1;
  }

  if (pc->use_3d_space) {
    paintcurve_sync_to_source_if_3d(C, pc);
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = paint ? BKE_paint_brush(paint) : nullptr;
  if (br) {
    BKE_brush_tag_unsaved_changes(br);
  }

  ED_paintcurve_undo_push_end(C);
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  /* Full region redraw so the synced source object's evaluated geometry is rebuilt immediately,
   * matching the slide path. */
  if (paintcurve_uses_3d_geometry(pc)) {
    ED_region_tag_redraw(CTX_wm_region(C));
  }
  return true;
}

struct PointSlideData {
  bool is_segment;
  int segment_index;
  int segment_index_next;
  float segment_t;
  char select;
  int point_index;
  int initial_loc[2];
  int prev_mval[2];
  float point_initial_loc[3][2];
  int event;
  bool align;
  bool move_entire;
  bool move_handle;
  bool snap_angle;
  bool handle_moved;
  /** From #ToolSettings.snap_angle_increment_3d (radians) when header Snap is enabled. */
  float snap_angle_increment_rad;
  /* 3D object-space positions at drag start; used to recompute 3D delta on each mouse move
   * without accumulating floating-point drift from repeated screen-to-3D projections. */
  float point_initial_loc_3d[3][3];
  /* True when any MOUSEMOVE event was received during the modal (user dragged). */
  bool did_move;
  /* True when a pivot was clicked that was already selected — enables cycle-on-click. */
  bool was_pivot_already_selected;
  /* Cached 3D view context for the modal drag; avoids re-initializing on every mouse move. */
  bool use_3d_view;
  ViewContext vc;
  float ob_to_world[4][4];
  float world_to_ob[4][4];
  PaintCurveSnapContext *snap_ctx;
};

static void paintcurve_point_slide_snap_context_free(PointSlideData *psd)
{
  /* The slide is ending: hide the overlay snap marker. */
  paintcurve_snap_marker_clear_local();
  if (psd == nullptr) {
    return;
  }
  ED_paintcurve_snap_context_destroy(psd->snap_ctx);
  psd->snap_ctx = nullptr;
}

static void paintcurve_slide_refresh_object_mats(PointSlideData *psd)
{
  Object *ob = psd->vc.obact;
  if (ob == nullptr) {
    return;
  }
  if (psd->vc.depsgraph) {
    Object *ob_eval = DEG_get_evaluated(psd->vc.depsgraph, ob);
    if (ob_eval) {
      ob = ob_eval;
    }
  }
  copy_m4_m4(psd->ob_to_world, ob->object_to_world().ptr());
  copy_m4_m4(psd->world_to_ob, ob->world_to_object().ptr());
}

static void paintcurve_get_prev_co_world(const PointSlideData *psd, float r_prev_co_world[3])
{
  mul_v3_m4v3(r_prev_co_world, psd->ob_to_world, psd->point_initial_loc_3d[1]);
}

static void paintcurve_point_slide_init_snap_settings(bContext *C, PointSlideData *psd)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  if (ts && (ts->snap_flag & SCE_SNAP)) {
    psd->snap_angle_increment_rad = ts->snap_angle_increment_3d;
  }
  else {
    psd->snap_angle_increment_rad = 0.0f;
  }
}

static void paintcurve_apply_surface_snap_to_point(bke::CurvesGeometry &geom,
                                                   const PointSlideData *psd,
                                                   const float hit_obj[3],
                                                   const float hit_no_obj[3])
{
  BLI_assert(psd->select == 1 || psd->move_entire);
  float snap_delta[3];
  sub_v3_v3v3(snap_delta, hit_obj, psd->point_initial_loc_3d[1]);
  for (int h = 0; h < 3; h++) {
    add_v3_v3v3(paintcurve_geom_co(geom, psd->point_index, h),
                snap_delta,
                psd->point_initial_loc_3d[h]);
  }
  paintcurve_geom_set_surface_normal(geom, psd->point_index, float3(hit_no_obj));
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
}

static void paintcurve_slide_status_set(bContext *C, wmOperator *op, const PaintCurve *pc)
{
  WorkspaceStatus status(C);
  ToolSettings *ts = CTX_data_tool_settings(C);
  if (pc != nullptr && paintcurve_uses_3d_geometry(pc) && ts != nullptr) {
    status.item_bool(
        IFACE_("Snap"), (ts->snap_flag & SCE_SNAP) != 0, ICON_SNAP_ON, ICON_SNAP_OFF);
  }
  status.opmodal(IFACE_("Snap Angle"), op->type, PAINTCURVE_MODAL_SNAP_ANGLE);
  status.opmodal(IFACE_("Move Entire Point"), op->type, PAINTCURVE_MODAL_MOVE_ENTIRE);
  status.opmodal(IFACE_("Move Current Handle"), op->type, PAINTCURVE_MODAL_MOVE_HANDLE);
}

static void paintcurve_slide_status_clear(bContext *C)
{
  ED_workspace_status_text(C, nullptr);
}

static void paintcurve_point_slide_init_3d_view(PointSlideData *psd,
                                                const ViewContext &vc,
                                                PaintCurve *pc,
                                                const int point_index)
{
  if (!paintcurve_uses_3d_geometry(pc) || vc.obact == nullptr) {
    psd->use_3d_view = false;
    return;
  }
  psd->use_3d_view = true;
  psd->vc = vc;
  copy_m4_m4(psd->ob_to_world, vc.obact->object_to_world().ptr());
  copy_m4_m4(psd->world_to_ob, vc.obact->world_to_object().ptr());
  if (point_index >= 0) {
    bke::CurvesGeometry &geom = pc->geometry.wrap();
    for (int i = 0; i < 3; i++) {
      copy_v3_v3(psd->point_initial_loc_3d[i], paintcurve_geom_co(geom, point_index, i));
    }
  }
}

/** Viewport-bound handle slide: geometry stores screen-space coordinates. */
static void paintcurve_apply_handle_move_2d(bke::CurvesGeometry &geom,
                                            const int point_index,
                                            const PointSlideData *psd,
                                            const float mval[2])
{
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();

  float3 &left = paintcurve_geom_co(geom, point_index, 0);
  float3 &center = paintcurve_geom_co(geom, point_index, 1);
  float3 &right = paintcurve_geom_co(geom, point_index, 2);

  const bool pivot_only = (psd->select == 1);
  const bool is_left = (psd->select == 0);
  const float delta[2] = {mval[0] - float(psd->prev_mval[0]), mval[1] - float(psd->prev_mval[1])};

  if (pivot_only || psd->move_entire) {
    const float total_delta[2] = {mval[0] - float(psd->initial_loc[0]),
                                  mval[1] - float(psd->initial_loc[1])};
    for (int i = 0; i < 3; i++) {
      float3 &co = paintcurve_geom_co(geom, point_index, i);
      co.x = psd->point_initial_loc[i][0] + total_delta[0];
      co.y = psd->point_initial_loc[i][1] + total_delta[1];
    }
    return;
  }

  int8_t h_left = types_left[point_index];
  int8_t h_right = types_right[point_index];

  if (psd->move_handle) {
    float3 &handle = is_left ? left : right;
    handle.x += delta[0];
    handle.y += delta[1];
    h_left = BEZIER_HANDLE_FREE;
    h_right = BEZIER_HANDLE_FREE;
  }
  else {
    if (psd->handle_moved) {
      h_left = BEZIER_HANDLE_ALIGN;
      h_right = BEZIER_HANDLE_ALIGN;
    }

    float2 offset(mval[0] - center.x, mval[1] - center.y);
    if (psd->snap_angle) {
      offset = paintcurve_snap_handle_offset(offset, psd->snap_angle_increment_rad);
    }

    if (is_left) {
      if (h_right == BEZIER_HANDLE_AUTO) {
        h_right = BEZIER_HANDLE_ALIGN;
      }
      h_left = h_right;
      if (h_right == BEZIER_HANDLE_VECTOR) {
        h_left = BEZIER_HANDLE_FREE;
      }
      left.x = center.x + offset.x;
      left.y = center.y + offset.y;
      if (h_right == BEZIER_HANDLE_ALIGN) {
        right.x = 2.0f * center.x - left.x;
        right.y = 2.0f * center.y - left.y;
      }
    }
    else {
      if (h_left == BEZIER_HANDLE_AUTO) {
        h_left = BEZIER_HANDLE_ALIGN;
      }
      h_right = h_left;
      if (h_left == BEZIER_HANDLE_VECTOR) {
        h_right = BEZIER_HANDLE_FREE;
      }
      right.x = center.x + offset.x;
      right.y = center.y + offset.y;
      if (h_left == BEZIER_HANDLE_ALIGN) {
        left.x = 2.0f * center.x - right.x;
        left.y = 2.0f * center.y - right.y;
      }
    }
  }

  types_left[point_index] = h_left;
  types_right[point_index] = h_right;
  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
}

static void paintcurve_apply_handle_move_3d(bke::CurvesGeometry &geom,
                                            PaintCurve * /*pc*/,
                                            const int point_index,
                                            const ViewContext *vc,
                                            const float ob_to_world[4][4],
                                            const float world_to_ob[4][4],
                                            const PointSlideData *psd,
                                            const float mval[2])
{
  MutableSpan<float3> positions = geom.positions_for_write();
  MutableSpan<int8_t> types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> types_right = geom.handle_types_right_for_write();
  MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
  MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();

  const float3 &pivot = positions[point_index];
  float pivot_world[3];
  mul_v3_m4v3(pivot_world, ob_to_world, pivot);

  const bool pivot_only = (psd->select == 1);
  const bool is_left = (psd->select == 0);
  const float delta[2] = {mval[0] - float(psd->prev_mval[0]), mval[1] - float(psd->prev_mval[1])};

  if (pivot_only || psd->move_entire) {
    float obj_init[3], obj_curr[3], obj_delta[3];
    float mval_init[2] = {float(psd->initial_loc[0]), float(psd->initial_loc[1])};
    float world_init[3], world_curr[3];
    ED_view3d_win_to_3d(vc->v3d, vc->region, pivot_world, mval_init, world_init);
    ED_view3d_win_to_3d(vc->v3d, vc->region, pivot_world, mval, world_curr);
    mul_v3_m4v3(obj_init, world_to_ob, world_init);
    mul_v3_m4v3(obj_curr, world_to_ob, world_curr);
    sub_v3_v3v3(obj_delta, obj_curr, obj_init);
    for (int i = 0; i < 3; i++) {
      add_v3_v3v3(
          paintcurve_geom_co(geom, point_index, i), obj_delta, psd->point_initial_loc_3d[i]);
    }
    return;
  }

  if (psd->move_handle) {
    const int handle_idx = is_left ? 0 : 2;
    float screen[2];
    paintcurve_object_to_screen(
        vc, ob_to_world, paintcurve_geom_co(geom, point_index, handle_idx), screen);
    float target_screen[2];
    add_v2_v2v2(target_screen, screen, delta);
    paintcurve_screen_to_object(vc,
                                pivot_world,
                                world_to_ob,
                                target_screen,
                                paintcurve_geom_co(geom, point_index, handle_idx));
    types_left[point_index] = BEZIER_HANDLE_FREE;
    types_right[point_index] = BEZIER_HANDLE_FREE;
  }
  else {
    if (psd->handle_moved) {
      types_left[point_index] = BEZIER_HANDLE_ALIGN;
      types_right[point_index] = BEZIER_HANDLE_ALIGN;
    }

    float pivot_screen[2];
    paintcurve_object_to_screen(vc, ob_to_world, pivot, pivot_screen);
    float2 offset(mval[0] - pivot_screen[0], mval[1] - pivot_screen[1]);
    if (psd->snap_angle) {
      offset = paintcurve_snap_handle_offset(offset, psd->snap_angle_increment_rad);
    }
    float target_screen[2];
    add_v2_v2v2(target_screen, pivot_screen, float2(offset));

    if (is_left) {
      if (types_right[point_index] == BEZIER_HANDLE_AUTO) {
        types_right[point_index] = BEZIER_HANDLE_ALIGN;
      }
      types_left[point_index] = types_right[point_index];
      if (types_right[point_index] == BEZIER_HANDLE_VECTOR) {
        types_left[point_index] = BEZIER_HANDLE_FREE;
      }
      paintcurve_screen_to_object(
          vc, pivot_world, world_to_ob, target_screen, handles_left[point_index]);
      if (types_right[point_index] == BEZIER_HANDLE_ALIGN) {
        handles_right[point_index] = 2.0f * pivot - handles_left[point_index];
      }
    }
    else {
      if (types_left[point_index] == BEZIER_HANDLE_AUTO) {
        types_left[point_index] = BEZIER_HANDLE_ALIGN;
      }
      types_right[point_index] = types_left[point_index];
      if (types_left[point_index] == BEZIER_HANDLE_VECTOR) {
        types_right[point_index] = BEZIER_HANDLE_FREE;
      }
      paintcurve_screen_to_object(
          vc, pivot_world, world_to_ob, target_screen, handles_right[point_index]);
      if (types_left[point_index] == BEZIER_HANDLE_ALIGN) {
        handles_left[point_index] = 2.0f * pivot - handles_right[point_index];
      }
    }
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
}

static void paintcurve_shift_segment_click_clear()
{
  paintcurve_shift_segment_click.point_a = -1;
  paintcurve_shift_segment_click.point_b = -1;
  paintcurve_shift_segment_click.time = 0.0;
}

/**
 * Shift+double-click on the same segment within #PAINT_CURVE_SEGMENT_DBLCLICK_TIME_SEC selects
 * every control point on the spline that contains the segment. Returns true when handled.
 */
static bool paintcurve_try_select_segment_on_shift_double_click(bContext *C,
                                                                wmOperator *op,
                                                                PaintCurve *pc,
                                                                Brush *br,
                                                                const int segment_a,
                                                                const int segment_b,
                                                                const bool extend)
{
  const double now = BLI_time_now_seconds();
  PaintCurveShiftSegmentClickState &state = paintcurve_shift_segment_click;

  const bool is_double_click = state.point_a >= 0 && state.point_a == segment_a &&
                               state.point_b == segment_b &&
                               (now - state.time) <= PAINT_CURVE_SEGMENT_DBLCLICK_TIME_SEC;

  state.time = now;
  state.point_a = segment_a;
  state.point_b = segment_b;

  if (!is_double_click) {
    return false;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  const int spline_idx = paintcurve_curve_of_point(pc, segment_a);
  if (spline_idx < 0) {
    paintcurve_shift_segment_click_clear();
    return false;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  if (!extend) {
    paintcurve_geom_set_all_selection(geom, 0);
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  for (const int point_i : points_by_curve[spline_idx]) {
    if (extend) {
      const uint8_t cur = paintcurve_geom_get_selection(geom, point_i);
      paintcurve_geom_set_selection(geom, point_i, cur | 0x07);
    }
    else {
      paintcurve_geom_set_selection(geom, point_i, 0x07);
    }
  }

  pc->active_curve = spline_idx;
  paintcurve_update_add_index_from_selection(pc, geom);

  BKE_brush_tag_unsaved_changes(br);
  ED_paintcurve_undo_push_end(C);
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  paintcurve_shift_segment_click_clear();
  return true;
}

static wmOperatorStatus paintcurve_slide_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  const bool do_select = RNA_boolean_get(op->ptr, "select");
  const bool align = RNA_boolean_get(op->ptr, "align");
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  const bool move_segment = RNA_boolean_get(op->ptr, "move_segment");
  const bool insert_point = RNA_boolean_get(op->ptr, "insert_point");
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;

  if (!extend) {
    paintcurve_shift_segment_click_clear();
  }

  if (!pc) {
    return OPERATOR_PASS_THROUGH;
  }

  if (paintcurve_skip_next_slide) {
    paintcurve_skip_next_slide = false;
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);

  if (insert_point) {
    if (paintcurve_try_insert_point_at_mouse(C, op, pc, loc_fl)) {
      return OPERATOR_FINISHED;
    }
    return OPERATOR_CANCELLED;
  }

  /* Yield to slide_radius when the cursor is within the radius handle endpoint circle.
   * slide_radius is registered right after slide in the keymap, so PASS_THROUGH hands control
   * to it without requiring a separate poll check. */
  if (pc->show_radius_handles) {
    const int radius_hit = paintcurve_find_radius_handle_at_pos(
        pc, screen_points.data(), loc_fl, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
    if (radius_hit >= 0) {
      return OPERATOR_PASS_THROUGH;
    }
  }

  int point_index = -1;
  char select = 0;

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  const bool geom_valid = paintcurve_geometry_is_valid(geom);
  const bool any_selected = geom_valid && paintcurve_geom_any_selected(geom);
  /* When a handle is already selected, use the tighter hover threshold so a click away from
   * control points does not accidentally hit a distant handle (40 px manhattan is too large). */
  const float hit_threshold = any_selected ? PAINT_CURVE_HOVER_THRESHOLD :
                                             PAINT_CURVE_SELECT_THRESHOLD;

  if (geom_valid) {
    char selflag = 0;
    const int hit = paintcurve_find_in_screen_points(
        screen_points.as_span(), loc_fl, align, hit_threshold, &selflag);
    if (hit >= 0) {
      if (extend) {
        paintcurve_shift_segment_click_clear();
      }
      if (do_select) {
        point_index = hit;
        select = selflag;
      }
      else {
        /* Slide an already-selected handle only when the cursor is over it. */
        const uint8_t sel = paintcurve_geom_get_selection(geom, hit);
        if (sel != 0) {
          /* Convert selection bits to the bit-flag form that paintcurve_point_co_index expects.
           * Using raw indices (0/1/2) here causes an infinite loop in that function when the
           * left-handle bit (0x01) is set, because paintcurve_point_co_index(0) never exits. */
          select = (sel & 0x01) ? SEL_F1 : (sel & 0x02) ? SEL_F2 : SEL_F3;
          point_index = hit;
        }
      }
    }
  }

  /* Point-editing mode: ignore stray clicks that miss control points, but still allow
   * segment slide (move_segment) when the cursor is on a curve segment. */
  if (point_index < 0 && any_selected && !move_segment) {
    return OPERATOR_PASS_THROUGH;
  }

  if (point_index < 0 && move_segment) {
    int segment_index;
    int segment_index_next;
    float edge_t;
    if (paintcurve_find_closest_segment(pc,
                                        &vc,
                                        screen_points.as_span(),
                                        loc_fl,
                                        PAINT_CURVE_SELECT_THRESHOLD,
                                        &segment_index,
                                        &segment_index_next,
                                        &edge_t))
    {
      if (extend && paintcurve_try_select_segment_on_shift_double_click(
                        C, op, pc, br, segment_index, segment_index_next, extend))
      {
        return OPERATOR_FINISHED;
      }

      ARegion *region = CTX_wm_region(C);
      wmWindow *window = CTX_wm_window(C);
      PointSlideData *psd = MEM_new<PointSlideData>(__func__);
      psd->is_segment = true;
      psd->segment_index = segment_index;
      psd->segment_index_next = segment_index_next;
      psd->segment_t = edge_t;
      psd->select = 0;
      psd->point_index = -1;
      copy_v2_v2_int(psd->initial_loc, event->mval);
      copy_v2_v2_int(psd->prev_mval, event->mval);
      psd->event = event->type;
      psd->align = false;
      psd->move_entire = false;
      psd->move_handle = false;
      psd->snap_angle = false;
      psd->handle_moved = false;
      psd->did_move = false;
      psd->was_pivot_already_selected = false;
      psd->snap_ctx = nullptr;
      paintcurve_point_slide_init_3d_view(psd, vc, pc, -1);
      if (psd->use_3d_view) {
        psd->snap_ctx = ED_paintcurve_snap_context_create();
      }
      paintcurve_point_slide_init_snap_settings(C, psd);
      op->customdata = psd;
      BKE_brush_tag_unsaved_changes(br);
      paintcurve_slide_segment_a = segment_index;
      paintcurve_slide_segment_b = segment_index_next;
      paintcurve_slide_active = true;
      WM_event_add_modal_handler(C, op);
      paintcurve_slide_status_set(C, op, pc);
      WM_paint_cursor_tag_redraw(window, region);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  if (point_index >= 0) {
    ARegion *region = CTX_wm_region(C);
    wmWindow *window = CTX_wm_window(C);
    PointSlideData *psd = MEM_new<PointSlideData>(__func__);
    psd->is_segment = false;
    psd->segment_index = -1;
    psd->segment_index_next = -1;
    psd->segment_t = 0.0f;
    psd->select = paintcurve_point_co_index(select);
    psd->point_index = point_index;
    copy_v2_v2_int(psd->initial_loc, event->mval);
    copy_v2_v2_int(psd->prev_mval, event->mval);
    psd->event = event->type;
    psd->align = align;
    psd->move_entire = false;
    psd->move_handle = false;
    psd->snap_angle = align;
    psd->handle_moved = false;
    psd->did_move = false;
    const PaintCurvePoint &pcp = screen_points[point_index];
    for (int i = 0; i < 3; i++) {
      copy_v2_v2(psd->point_initial_loc[i], pcp.bez.vec[i]);
    }
    psd->snap_ctx = nullptr;
    paintcurve_point_slide_init_3d_view(psd, vc, pc, point_index);
    if (psd->use_3d_view) {
      psd->snap_ctx = ED_paintcurve_snap_context_create();
    }
    paintcurve_point_slide_init_snap_settings(C, psd);
    op->customdata = psd;

    const uint8_t new_sel = (psd->select == 0) ? 0x01 : (psd->select == 1) ? 0x02 : 0x04;

    if (extend) {
      /* Shift+click: toggle this handle's bit while preserving other points' selection. */
      psd->was_pivot_already_selected = false;
      const uint8_t cur = paintcurve_geom_get_selection(pc->geometry.wrap(), point_index);
      paintcurve_geom_set_selection(pc->geometry.wrap(), point_index, cur ^ new_sel);
    }
    else {
      /* Check if clicking an already-selected pivot (before clearing selection).
       * Used to implement cycle-on-click when the user releases without dragging. */
      {
        const uint8_t pre_sel = paintcurve_geom_get_selection(pc->geometry.wrap(), point_index);
        psd->was_pivot_already_selected = (psd->select == 1) && ((pre_sel & 0x07) != 0);
      }
      paintcurve_geom_set_all_selection(pc->geometry.wrap(), 0);
      /* Select the active handle. */
      paintcurve_geom_set_selection(pc->geometry.wrap(), point_index, new_sel);
    }

    paintcurve_update_add_index_from_selection(pc, pc->geometry.wrap());

    BKE_brush_tag_unsaved_changes(br);
    paintcurve_slide_segment_clear();
    paintcurve_slide_active = true;
    WM_event_add_modal_handler(C, op);
    paintcurve_slide_status_set(C, op, pc);
    WM_paint_cursor_tag_redraw(window, region);
    return OPERATOR_RUNNING_MODAL;
  }

  return OPERATOR_PASS_THROUGH;
}

static wmOperatorStatus paintcurve_slide_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  PointSlideData *psd = static_cast<PointSlideData *>(op->customdata);

  if (event->type == psd->event && event->val == KM_RELEASE) {
    Paint *release_paint = BKE_paint_get_active_from_context(C);
    Brush *release_br = BKE_paint_brush(release_paint);
    PaintCurve *release_pc = release_br ? release_br->paint_curve : nullptr;

    /* Cycle handle type when the user clicked without dragging on an already-selected pivot. */
    const bool do_cycle = !psd->did_move && psd->was_pivot_already_selected && !psd->is_segment;
    const int cycle_point_index = psd->point_index;

    paintcurve_slide_status_clear(C);
    paintcurve_point_slide_snap_context_free(psd);
    MEM_delete(psd);
    op->customdata = nullptr;
    paintcurve_slide_segment_clear();
    paintcurve_slide_active = false;

    /* Commit as a single undo step. In Sculpt mode this is a no-op. */
    ED_paintcurve_undo_push_begin(C, op->type->name);
    if (do_cycle && release_pc) {
      paintcurve_cycle_point_handle_type(release_pc, cycle_point_index);
      paintcurve_sync_after_handle_type_change(C, release_pc);
      WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
    }
    ED_paintcurve_undo_push_end(C);
    paintcurve_sync_to_source_if_3d(C, release_pc);
    if (paintcurve_uses_3d_geometry(release_pc)) {
      ED_region_tag_redraw(CTX_wm_region(C));
    }
    return OPERATOR_FINISHED;
  }

  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case PAINTCURVE_MODAL_MOVE_ENTIRE:
        psd->move_entire = !psd->move_entire;
        break;
      case PAINTCURVE_MODAL_SNAP_ANGLE:
        psd->snap_angle = !psd->snap_angle;
        break;
      case PAINTCURVE_MODAL_MOVE_HANDLE:
        psd->move_handle = !psd->move_handle;
        if (psd->move_handle) {
          psd->handle_moved = true;
        }
        break;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  switch (event->type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      ARegion *region = CTX_wm_region(C);
      wmWindow *window = CTX_wm_window(C);
      Paint *paint = BKE_paint_get_active_from_context(C);
      Brush *br = BKE_paint_brush(paint);
      PaintCurve *pc = br ? br->paint_curve : nullptr;

      if (!pc) {
        break;
      }

      const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};

      bke::CurvesGeometry &geom = pc->geometry.wrap();

      if (psd->use_3d_view) {
        /* Refresh depsgraph after sync-to-source may have tagged updates. */
        psd->vc.depsgraph = CTX_data_depsgraph_pointer(C);
        paintcurve_slide_refresh_object_mats(psd);
        const bool snap_to_surface = paintcurve_uses_3d_geometry(pc);
        if (psd->is_segment) {
          float depth_world[3];
          float hit_obj[3];
          float hit_no_obj[3];
          float prev_co_world[3];
          const float3 &seg_co = geom.positions()[psd->segment_index];
          mul_v3_m4v3(prev_co_world, psd->ob_to_world, seg_co);
          /* `false`: no depth-buffer fallback inside a modal drag. */
          if (snap_to_surface && paintcurve_surface_place(C,
                                                          psd->snap_ctx,
                                                          psd->vc,
                                                          mval_fl,
                                                          prev_co_world,
                                                          false,
                                                          hit_obj,
                                                          hit_no_obj))
          {
            mul_v3_m4v3(depth_world, psd->ob_to_world, hit_obj);
            paintcurve_snap_marker_update(C, psd->vc.region, psd->ob_to_world, hit_obj);
          }
          else {
            paintcurve_snap_marker_clear_local();
            const float3 &depth_point = geom.positions()[psd->segment_index];
            mul_v3_m4v3(depth_world, psd->ob_to_world, depth_point);
          }
          paintcurve_apply_segment_move_3d(geom,
                                             psd->segment_index,
                                             psd->segment_index_next,
                                             psd->segment_t,
                                             &psd->vc,
                                             psd->world_to_ob,
                                             mval_fl,
                                             depth_world);
          geom.tag_positions_changed();
        }
        else {
          /* Surface snap applies only to control points (pivot), not tangent handles. */
          bool snapped = false;
          if (snap_to_surface && (psd->select == 1 || psd->move_entire)) {
            float hit_obj[3];
            float hit_no_obj[3];
            float prev_co_world[3];
            paintcurve_get_prev_co_world(psd, prev_co_world);
            if (paintcurve_surface_place(C,
                                         psd->snap_ctx,
                                         psd->vc,
                                         mval_fl,
                                         prev_co_world,
                                         false,
                                         hit_obj,
                                         hit_no_obj))
            {
              paintcurve_apply_surface_snap_to_point(geom, psd, hit_obj, hit_no_obj);
              snapped = true;
              paintcurve_snap_marker_update(C, psd->vc.region, psd->ob_to_world, hit_obj);
            }
          }

          if (!snapped) {
            paintcurve_snap_marker_clear_local();
            paintcurve_apply_handle_move_3d(
                geom, pc, psd->point_index, &psd->vc, psd->ob_to_world, psd->world_to_ob, psd, mval_fl);
          }

          geom.tag_positions_changed();
        }
      }
      else {
        paintcurve_snap_marker_clear_local();
        if (psd->is_segment) {
          paintcurve_apply_segment_move_2d(
              geom, psd->segment_index, psd->segment_index_next, psd->segment_t, mval_fl);
        }
        else if (psd->point_index >= 0) {
          paintcurve_apply_handle_move_2d(geom, psd->point_index, psd, mval_fl);
        }
        geom.tag_positions_changed();
      }

      psd->did_move = true;
      copy_v2_v2_int(psd->prev_mval, event->mval);
      paintcurve_sync_to_source_if_3d(C, pc);
      WM_paint_cursor_tag_redraw(window, region);
      /* The paint-cursor redraw above only re-composites the cursor. The synced source object is
       * drawn from evaluated geometry, so it needs a full region redraw to rebuild its batch. */
      if (paintcurve_uses_3d_geometry(pc)) {
        ED_region_tag_redraw(region);
      }
      break;
    }
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

wmKeyMap *paintcurve_slide_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {PAINTCURVE_MODAL_MOVE_HANDLE,
       "MOVE_HANDLE",
       0,
       "Move Current Handle",
       "Move the current handle of the control point freely (Ctrl+Shift)"},
      {PAINTCURVE_MODAL_MOVE_ENTIRE,
       "MOVE_ENTIRE",
       0,
       "Move Entire Point",
       "Move the entire point using its handles"},
      {PAINTCURVE_MODAL_SNAP_ANGLE,
       "SNAP_ANGLE",
       0,
       "Snap Angle",
       "Snap the handle angle to 45 degrees"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Paint Curve Slide Modal Map");

  if (keymap && keymap->modal_items) {
    return nullptr;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Paint Curve Slide Modal Map", modal_items);

  {
    KeyMapItem_Params params{};
    params.type = EVT_LEFTCTRLKEY;
    params.value = KM_ANY;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, PAINTCURVE_MODAL_MOVE_HANDLE);
  }
  {
    KeyMapItem_Params params{};
    params.type = EVT_LEFTALTKEY;
    params.value = KM_ANY;
    params.modifier = KM_ANY;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, PAINTCURVE_MODAL_MOVE_ENTIRE);
  }
  {
    KeyMapItem_Params params{};
    params.type = EVT_LEFTSHIFTKEY;
    params.value = KM_ANY;
    params.modifier = KM_ANY;
    params.direction = KM_ANY;
    WM_modalkeymap_add_item(keymap, &params, PAINTCURVE_MODAL_SNAP_ANGLE);
  }

  WM_modalkeymap_assign(keymap, "PAINTCURVE_OT_slide");

  return keymap;
}

struct RadiusSlideData {
  int point_index;
  PaintCurveRadiusHandleScreen handle;
  short event;
};

static void paintcurve_slide_radius_status_set(bContext *C, const float radius)
{
  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return;
  }
  char str[UI_MAX_DRAW_STR];
  SNPRINTF_UTF8(str, IFACE_("Shrink/Fatten: %3f"), radius);
  ED_area_status_text(area, str);
}

static void paintcurve_slide_radius_status_clear(bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (area) {
    ED_area_status_text(area, nullptr);
  }
}

static bool paintcurve_slide_radius_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  PaintCurve *pc = brush ? brush->paint_curve : nullptr;
  return pc && pc->show_radius_handles;
}

static wmOperatorStatus paintcurve_slide_radius_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  if (!pc || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return OPERATOR_PASS_THROUGH;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);
  if (screen_points.is_empty()) {
    return OPERATOR_PASS_THROUGH;
  }

  const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  const int point_index = paintcurve_find_radius_handle_at_pos(
      pc, screen_points.data(), loc_fl, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
  if (point_index < 0) {
    return OPERATOR_PASS_THROUGH;
  }

  RadiusSlideData *rsd = MEM_new<RadiusSlideData>(__func__);
  rsd->point_index = point_index;
  paintcurve_radius_handle_screen_get(pc, screen_points.data(), point_index, &rsd->handle);
  rsd->event = event->type;

  op->customdata = rsd;
  BKE_brush_tag_unsaved_changes(br);
  paintcurve_slide_radius_status_set(C, paintcurve_get_point_radius(pc, point_index));
  paintcurve_slide_segment_clear();
  paintcurve_slide_active = true;
  WM_event_add_modal_handler(C, op);
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus paintcurve_slide_radius_modal(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  RadiusSlideData *rsd = static_cast<RadiusSlideData *>(op->customdata);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;

  if (event->type == rsd->event && event->val == KM_RELEASE) {
    paintcurve_slide_radius_status_clear(C);
    MEM_delete(rsd);
    op->customdata = nullptr;
    paintcurve_slide_segment_clear();
    paintcurve_slide_active = false;
    ED_paintcurve_undo_push_begin(C, op->type->name);
    ED_paintcurve_undo_push_end(C);
    paintcurve_sync_to_source_if_3d(C, pc);
    return OPERATOR_FINISHED;
  }

  if (event->type == MOUSEMOVE && pc) {
    bke::CurvesGeometry &geom = pc->geometry.wrap();
    if (paintcurve_geometry_is_valid(geom) && rsd->point_index < geom.points_num()) {
      const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
      const float new_radius = paintcurve_radius_from_handle_screen_pos(&rsd->handle, mval_fl);
      geom.radius_for_write()[rsd->point_index] = new_radius;
      geom.tag_positions_changed();
      paintcurve_slide_radius_status_set(C, new_radius);
      paintcurve_sync_to_source_if_3d(C, pc);
      WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

static void paintcurve_slide_radius_cancel(bContext *C, wmOperator *op)
{
  paintcurve_slide_radius_status_clear(C);
  RadiusSlideData *rsd = static_cast<RadiusSlideData *>(op->customdata);
  MEM_SAFE_DELETE(rsd);
  op->customdata = nullptr;
  paintcurve_slide_segment_clear();
  paintcurve_slide_active = false;
}

void PAINTCURVE_OT_slide_radius(wmOperatorType *ot)
{
  ot->name = "Slide Paint Curve Radius";
  ot->description = "Adjust paint curve point radius by dragging its radius handle";
  ot->idname = "PAINTCURVE_OT_slide_radius";

  ot->invoke = paintcurve_slide_radius_invoke;
  ot->modal = paintcurve_slide_radius_modal;
  ot->cancel = paintcurve_slide_radius_cancel;
  ot->poll = paintcurve_slide_radius_poll;

  ot->flag = OPTYPE_BLOCKING;
}

static void paintcurve_slide_cancel(bContext *C, wmOperator *op)
{
  paintcurve_slide_status_clear(C);
  PointSlideData *psd = static_cast<PointSlideData *>(op->customdata);
  if (psd) {
    paintcurve_point_slide_snap_context_free(psd);
    MEM_delete(psd);
    op->customdata = nullptr;
  }
  paintcurve_slide_segment_clear();
  paintcurve_slide_active = false;
}

void PAINTCURVE_OT_slide(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Slide Paint Curve Point";
  ot->description = "Select and slide paint curve point";
  ot->idname = "PAINTCURVE_OT_slide";

  /* API callbacks. */
  ot->invoke = paintcurve_slide_invoke;
  ot->modal = paintcurve_slide_modal;
  ot->cancel = paintcurve_slide_cancel;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = 0;

  /* properties */
  PropertyRNA *prop;
  prop = RNA_def_boolean(ot->srna,
                         "align",
                         false,
                         "Snap Angle",
                         "Snap handle direction to 45 degree increments (also toggled with Shift "
                         "during drag)");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna,
                         "extend",
                         false,
                         "Extend",
                         "Add the clicked point to the current selection instead of replacing it");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(
      ot->srna, "select", true, "Select", "Attempt to select a point handle before transform");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(
      ot->srna,
      "move_segment",
      false,
      "Move Segment",
      "Move an existing curve segment when no control point is under the cursor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna,
                         "insert_point",
                         false,
                         "Insert Point",
                         "Insert a control point into the curve segment under the cursor");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static wmOperatorStatus paintcurve_draw_exec(bContext *C, wmOperator * /*op*/)
{
  PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const char *name;

  switch (mode) {
    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      name = "PAINT_OT_image_paint";
      break;
    case PaintMode::Weight:
      name = "PAINT_OT_weight_paint";
      break;
    case PaintMode::Vertex:
      name = "PAINT_OT_vertex_paint";
      break;
    case PaintMode::Sculpt:
      name = "SCULPT_OT_brush_stroke";
      break;
    case PaintMode::SculptCurves:
      name = "SCULPT_CURVES_OT_brush_stroke";
      break;
    case PaintMode::GPencil:
      name = "GREASE_PENCIL_OT_brush_stroke";
      break;
    case PaintMode::SculptGPencil:
      name = "GREASE_PENCIL_OT_sculpt_paint";
      break;
    default:
      return OPERATOR_PASS_THROUGH;
  }

  return WM_operator_name_call(C, name, wm::OpCallContext::InvokeDefault, nullptr, nullptr);
}

void PAINTCURVE_OT_draw(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Draw Curve";
  ot->description = "Draw curve";
  ot->idname = "PAINTCURVE_OT_draw";

  /* API callbacks. */
  ot->exec = paintcurve_draw_exec;
  ot->poll = paint_curve_poll;

  /* flags */
  /* No OPTYPE_UNDO: the draw operator only dispatches to the paint stroke operator,
   * which manages its own undo steps. Adding a PAINTCURVE undo step here would
   * capture stale curve positions and cause them to revert on Ctrl+Z. */
  ot->flag = 0;
}

static wmOperatorStatus paintcurve_cursor_invoke(bContext *C,
                                                 wmOperator * /*op*/,
                                                 const wmEvent *event)
{
  PaintMode mode = BKE_paintmode_get_active_from_context(C);

  switch (mode) {
    case PaintMode::Texture2D: {
      ARegion *region = CTX_wm_region(C);
      SpaceImage *sima = CTX_wm_space_image(C);
      float location[2];

      if (!sima) {
        return OPERATOR_CANCELLED;
      }

      ui::view2d_region_to_view(
          &region->v2d, event->mval[0], event->mval[1], &location[0], &location[1]);
      copy_v2_v2(sima->cursor, location);
      WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
      break;
    }
    default:
      ED_view3d_cursor3d_update(C, event->mval, true, V3D_CURSOR_ORIENT_VIEW);
      break;
  }

  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_cursor(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Place Cursor";
  ot->description = "Place cursor";
  ot->idname = "PAINTCURVE_OT_cursor";

  /* API callbacks. */
  ot->invoke = paintcurve_cursor_invoke;
  ot->poll = paint_curve_poll;

  /* flags */
  ot->flag = 0;
}

static wmOperatorStatus paintcurve_from_curve_object_exec(bContext *C, wmOperator *op)
{
  if (!ED_paintcurve_import_from_source_object(C, op->reports, true)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_from_curve_object(wmOperatorType *ot)
{
  ot->name = "Paint Curve from Curve Object";
  ot->description = "Fill the active paint curve from the source object chosen in the picker";
  ot->idname = "PAINTCURVE_OT_from_curve_object";

  ot->exec = paintcurve_from_curve_object_exec;
  ot->poll = paint_curve_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus paintcurve_to_curve_object_exec(bContext *C, wmOperator *op)
{
  Object *dst_ob = nullptr;
  const ePaintCurveExportCurveType curve_type = ePaintCurveExportCurveType(
      RNA_enum_get(op->ptr, "curve_type"));
  const bool use_selection = RNA_boolean_get(op->ptr, "use_selection");
  const bool assign_as_source = RNA_boolean_get(op->ptr, "assign_as_source");
  if (!ED_paintcurve_export_to_scene_object(
          C, op->reports, &dst_ob, curve_type, use_selection, assign_as_source))
  {
    return OPERATOR_CANCELLED;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  if (br) {
    BKE_brush_tag_unsaved_changes(br);
  }

  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER, nullptr);
  if (dst_ob) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, dst_ob);
    DEG_id_tag_update(&dst_ob->id, ID_RECALC_TRANSFORM);
  }
  return OPERATOR_FINISHED;
}

static bool paintcurve_to_curve_object_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return false;
  }

  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr || brush->paint_curve == nullptr) {
    return false;
  }

  return paintcurve_geometry_is_valid(brush->paint_curve->geometry.wrap());
}

void PAINTCURVE_OT_to_curve_object(wmOperatorType *ot)
{
  static const EnumPropertyItem curve_type_items[] = {
      {PAINT_CURVE_EXPORT_BEZIER,
       "BEZIER",
       0,
       "Bezier Curve",
       "Create a classic Bezier Curve object"},
      {PAINT_CURVE_EXPORT_CURVES,
       "CURVES",
       0,
       "Curves",
       "Create a Curves object for hair and geometry nodes"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Paint Curve to Curve Object";
  ot->description =
      "Create a new Curve or Curves object on the scene from the paint curve, optionally using "
      "only selected control points, and optionally linking it as the source curve";
  ot->idname = "PAINTCURVE_OT_to_curve_object";

  ot->exec = paintcurve_to_curve_object_exec;
  ot->poll = paintcurve_to_curve_object_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "curve_type",
               curve_type_items,
               PAINT_CURVE_EXPORT_BEZIER,
               "Curve Type",
               "Type of curve object to create on the scene");
  RNA_def_boolean(ot->srna,
                  "use_selection",
                  false,
                  "Use Selection",
                  "Export only fully selected control points instead of the entire paint curve");
  RNA_def_boolean(ot->srna,
                  "assign_as_source",
                  true,
                  "Assign as Source",
                  "Link the new object as the paint curve source so edits are kept in sync");
}

static wmOperatorStatus paintcurve_separate_to_curve_object_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return OPERATOR_CANCELLED;
  }
  if (!paintcurve_geometry_any_point_selected(pc->geometry.wrap())) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  Object *dst_ob = nullptr;
  const ePaintCurveExportCurveType curve_type = ePaintCurveExportCurveType(
      RNA_enum_get(op->ptr, "curve_type"));
  if (!ED_paintcurve_export_to_scene_object(
          C, op->reports, &dst_ob, curve_type, true, false))
  {
    ED_paintcurve_undo_push_end(C);
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_remove_selected_points(geom)) {
    ED_paintcurve_undo_push_end(C);
    return OPERATOR_CANCELLED;
  }

  pc->active_curve = paintcurve_active_curve_get(pc);
  if (paintcurve_uses_3d_geometry(pc)) {
    paintcurve_sync_to_source_object(C, pc);
  }

  BKE_brush_tag_unsaved_changes(br);
  ED_paintcurve_undo_push_end(C);

  WM_event_add_notifier(C, NC_PAINTCURVE | NA_EDITED, pc);
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER, nullptr);
  if (dst_ob) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, dst_ob);
    DEG_id_tag_update(&dst_ob->id, ID_RECALC_TRANSFORM);
  }
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  if (paintcurve_uses_3d_geometry(pc)) {
    ED_region_tag_redraw(CTX_wm_region(C));
  }
  return OPERATOR_FINISHED;
}

static bool paintcurve_separate_to_curve_object_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return false;
  }

  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr || brush->paint_curve == nullptr) {
    return false;
  }

  const bke::CurvesGeometry &geom = brush->paint_curve->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return false;
  }
  return paintcurve_geometry_any_point_selected(geom);
}

void PAINTCURVE_OT_separate_to_curve_object(wmOperatorType *ot)
{
  static const EnumPropertyItem curve_type_items[] = {
      {PAINT_CURVE_EXPORT_BEZIER,
       "BEZIER",
       0,
       "Bezier Curve",
       "Create a classic Bezier Curve object"},
      {PAINT_CURVE_EXPORT_CURVES,
       "CURVES",
       0,
       "Curves",
       "Create a Curves object for hair and geometry nodes"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Separate Paint Curve Selection to Curve Object";
  ot->description =
      "Create a new Curve or Curves object from the selected control points and remove them from "
      "the paint curve";
  ot->idname = "PAINTCURVE_OT_separate_to_curve_object";

  ot->exec = paintcurve_separate_to_curve_object_exec;
  ot->poll = paintcurve_separate_to_curve_object_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "curve_type",
               curve_type_items,
               PAINT_CURVE_EXPORT_CURVES,
               "Curve Type",
               "Type of curve object to create on the scene");
}

/* -------------------------------------------------------------------- */
/** \name Sculpt Mode Curve Edit Tool — Pick Operator
 * \{ */

static bool paintcurve_sculpt_pick_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!(ob && (ob->mode & OB_MODE_SCULPT))) {
    return false;
  }
  const bToolRef *tref = paintcurve_tool_ref_from_context(C);
  return tref && STREQ(tref->idname, "builtin.curves_edit");
}

static wmOperatorStatus paintcurve_sculpt_pick_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  /* Ctrl+RMB is reserved for inserting/extending the paint curve (#PAINTCURVE_OT_insert_or_add_point). */
  if (event->modifier & KM_CTRL) {
    return OPERATOR_PASS_THROUGH;
  }

  const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};

  /* 1. Control point under cursor → pass through so PAINTCURVE_OT_slide handles the drag.
   * This check must come before the object-under-cursor pick so that pivots that visually
   * lie ON the spline are not intercepted by the object pick and accidentally re-imported. */
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  if (pc && paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    Vector<PaintCurvePoint> screen_points;
    paintcurve_build_screen_points(pc, &vc, screen_points);
    char selflag = 0;
    const int point_index = paintcurve_find_in_screen_points(
        screen_points.as_span(), loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &selflag);
    if (point_index >= 0) {
      return OPERATOR_PASS_THROUGH;
    }
    /* Check radius handles too. */
    if (pc->show_radius_handles) {
      const int radius_hit = paintcurve_find_radius_handle_at_pos(
          pc, screen_points.data(), loc_fl, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
      if (radius_hit >= 0) {
        return OPERATOR_PASS_THROUGH;
      }
    }
  }

  /* 2. Curve under cursor (screen proximity, sees through the mesh) → pick and import.
   * Fall back to the depth-occluded object raycast for a direct click on the curve object.
   * We exclude the current source object so that clicking its segments passes through to
   * the slide operator (move_segment=True). */
  Scene *scene = CTX_data_scene(C);
  Sculpt *sculpt = (scene && scene->toolsettings) ? scene->toolsettings->sculpt : nullptr;
  const Object *source_ob = sculpt ? sculpt->paint_curve_source_object : nullptr;

  Depsgraph *pick_depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext pick_vc = ED_view3d_viewcontext_init(C, pick_depsgraph);
  Object *ob_under = paintcurve_nearest_scene_curve(
      &pick_vc, float2(loc_fl[0], loc_fl[1]), PAINT_CURVE_HOVER_THRESHOLD, source_ob, nullptr);
  if (ob_under == nullptr) {
    Object *raycast = ED_view3d_give_object_under_cursor(C, event->mval);
    if (raycast && raycast != source_ob && ELEM(raycast->type, OB_CURVES, OB_CURVES_LEGACY)) {
      ob_under = raycast;
    }
  }
  if (ob_under && ELEM(ob_under->type, OB_CURVES, OB_CURVES_LEGACY)) {
    if (sculpt == nullptr) {
      return OPERATOR_CANCELLED;
    }
    sculpt->paint_curve_source_object = ob_under;
    sculpt->paint_curve_sync_to_source = 1;
    if (!ED_paintcurve_import_from_source_object(C, op->reports, true)) {
      return OPERATOR_CANCELLED;
    }
    WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
    WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return OPERATOR_FINISHED;
  }

  /* 3. Click on empty space or mesh → pass through so slide can check for segments.
   * If slide also doesn't find anything, the curve remains active (default behavior).
   * To explicitly deactivate, the user can click where no segments exist. */
  return OPERATOR_PASS_THROUGH;
}

void PAINTCURVE_OT_sculpt_pick(wmOperatorType *ot)
{
  ot->name = "Pick and Edit Curve";
  ot->description = "Pick a scene curve object and edit its paint-curve control points";
  ot->idname = "PAINTCURVE_OT_sculpt_pick";

  ot->invoke = paintcurve_sculpt_pick_invoke;
  ot->poll = paintcurve_sculpt_pick_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender
