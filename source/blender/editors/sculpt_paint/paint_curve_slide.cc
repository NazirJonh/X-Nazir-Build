/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Paint-curve slide / radius-handle modal and the overlay snap marker it publishes.
 * ID operators live in `paint_curve.cc`; shared helpers in `paint_curve_intern.hh`.
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

#include "BLI_math_base.h"
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
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"
#include "ED_screen.hh"
#include "ED_util_modal_multiwin.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_types.hh"

#include "UI_interface_types.hh"
#include "UI_resources.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

/* Set by select when a handle-type cycle consumed the click; slide must not start a drag. */
static bool paintcurve_skip_next_slide_flag = false;

void paintcurve_skip_next_slide()
{
  paintcurve_skip_next_slide_flag = true;
}

static bool paintcurve_slide_active = false;
static int paintcurve_slide_segment_a = -1;
static int paintcurve_slide_segment_b = -1;

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

static int paintcurve_point_co_index(char sel)
{
  char i = 0;
  while (sel != 1) {
    sel >>= 1;
    i++;
  }
  return i;
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
  float3 point_initial_loc_3d[3];
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
  paintcurve_snap_marker_clear();
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
  paintcurve_apply_point_surface_snap(
      geom, psd->point_index, psd->point_initial_loc_3d, float3(hit_obj), float3(hit_no_obj));
}

static void paintcurve_slide_status_set(bContext *C, wmOperator *op, const PaintCurve *pc)
{
  WorkspaceStatus status(C);
  ToolSettings *ts = CTX_data_tool_settings(C);
  if (pc != nullptr && paintcurve_uses_3d_geometry(pc) && ts != nullptr) {
    status.item_bool(IFACE_("Snap"), (ts->snap_flag & SCE_SNAP) != 0, ICON_SNAP_ON, ICON_SNAP_OFF);
  }
  status.opmodal(IFACE_("Snap Angle"), op->type, PAINTCURVE_MODAL_SNAP_ANGLE);
  status.opmodal(IFACE_("Move Entire Point"), op->type, PAINTCURVE_MODAL_MOVE_ENTIRE);
  status.opmodal(IFACE_("Move Current Handle"), op->type, PAINTCURVE_MODAL_MOVE_HANDLE);
}

static void paintcurve_slide_status_clear(bContext *C)
{
  ED_workspace_status_text(C, nullptr);
}

/** Mouse coords relative to `region` (viewport-bound curves are stored in this space). */
static void paintcurve_event_mval_region(const wmEvent *event,
                                         const ARegion *region,
                                         int r_mval[2])
{
  if (region) {
    r_mval[0] = event->xy[0] - region->winrct.xmin;
    r_mval[1] = event->xy[1] - region->winrct.ymin;
  }
  else {
    r_mval[0] = event->mval[0];
    r_mval[1] = event->mval[1];
  }
}

static void paintcurve_point_slide_init_3d_view(PointSlideData *psd,
                                                const ViewContext &vc,
                                                PaintCurve *pc,
                                                const int point_index)
{
  psd->vc = vc;
  psd->use_3d_view = false;
  if (!paintcurve_uses_3d_geometry(pc) || vc.obact == nullptr) {
    return;
  }
  psd->use_3d_view = true;
  copy_m4_m4(psd->ob_to_world, vc.obact->object_to_world().ptr());
  copy_m4_m4(psd->world_to_ob, vc.obact->world_to_object().ptr());
  if (point_index >= 0) {
    bke::CurvesGeometry &geom = pc->geometry.wrap();
    for (int i = 0; i < 3; i++) {
      psd->point_initial_loc_3d[i] = paintcurve_geom_co(geom, point_index, i);
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

    /* Viewport-bound curves store screen coordinates in geometry. Rotate around the pivot
     * captured at drag start (matches the overlay diamond at vec[1]) rather than a live center
     * that may diverge after intermediate auto-handle updates. */
    const float pivot_x = psd->point_initial_loc[1][0];
    const float pivot_y = psd->point_initial_loc[1][1];
    float2 offset(mval[0] - pivot_x, mval[1] - pivot_y);
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
      left.x = pivot_x + offset.x;
      left.y = pivot_y + offset.y;
      if (h_right == BEZIER_HANDLE_ALIGN) {
        right.x = 2.0f * pivot_x - left.x;
        right.y = 2.0f * pivot_y - left.y;
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
      right.x = pivot_x + offset.x;
      right.y = pivot_y + offset.y;
      if (h_left == BEZIER_HANDLE_ALIGN) {
        left.x = 2.0f * pivot_x - right.x;
        left.y = 2.0f * pivot_y - right.y;
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
    float mval_init[2] = {float(psd->initial_loc[0]), float(psd->initial_loc[1])};
    float obj_delta[3];
    paintcurve_object_delta_from_screen_drag(
        vc, world_to_ob, pivot_world, mval_init, mval, obj_delta);
    paintcurve_apply_point_translate_3d(
        geom, point_index, psd->point_initial_loc_3d, float3(obj_delta));
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
  ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
  paintcurve_shift_segment_click_clear();
  return true;
}

static wmOperatorStatus paintcurve_slide_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
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

  if (paintcurve_skip_next_slide_flag) {
    paintcurve_skip_next_slide_flag = false;
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  int event_mval_arr[2];
  if (!pc->use_3d_space && vc.region) {
    /* 2D curves are bound to the viewport region where they were created. */
    paintcurve_event_mval_region(event, vc.region, event_mval_arr);
  }
  else {
    ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
    if (!tracker.found() && vc.region) {
      tracker.use_fallback_region(vc.region);
    }
    event_mval_arr[0] = tracker.mval().x;
    event_mval_arr[1] = tracker.mval().y;
  }
  const int2 event_mval = {event_mval_arr[0], event_mval_arr[1]};
  const float loc_fl[2] = {float(event_mval.x), float(event_mval.y)};
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
                                             PAINT_CURVE_POINT_SELECT_THRESHOLD;

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
                                        PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                        &segment_index,
                                        &segment_index_next,
                                        &edge_t))
    {
      if (extend && paintcurve_try_select_segment_on_shift_double_click(
                        C, op, pc, br, segment_index, segment_index_next, extend))
      {
        return OPERATOR_FINISHED;
      }

      PointSlideData *psd = MEM_new<PointSlideData>(__func__);
      psd->is_segment = true;
      psd->segment_index = segment_index;
      psd->segment_index_next = segment_index_next;
      psd->segment_t = edge_t;
      psd->select = 0;
      psd->point_index = -1;
      copy_v2_v2_int(psd->initial_loc, event_mval);
      copy_v2_v2_int(psd->prev_mval, event_mval);
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
      WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);
      paintcurve_slide_status_set(C, op, pc);
      ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  if (point_index >= 0) {
    PointSlideData *psd = MEM_new<PointSlideData>(__func__);
    psd->is_segment = false;
    psd->segment_index = -1;
    psd->segment_index_next = -1;
    psd->segment_t = 0.0f;
    psd->select = paintcurve_point_co_index(select);
    psd->point_index = point_index;
    copy_v2_v2_int(psd->initial_loc, event_mval);
    copy_v2_v2_int(psd->prev_mval, event_mval);
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
    WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);
    paintcurve_slide_status_set(C, op, pc);
    ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
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
      ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
    }
    ED_paintcurve_undo_push_end(C);
    paintcurve_sync_to_source_if_3d(C, release_pc);
    ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
    WM_event_remove_modal_handler_other_windows(C, op);
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
      Paint *paint = BKE_paint_get_active_from_context(C);
      Brush *br = BKE_paint_brush(paint);
      PaintCurve *pc = br ? br->paint_curve : nullptr;

      if (!pc) {
        break;
      }

      int event_mval[2];
      if (!psd->use_3d_view && psd->vc.region) {
        paintcurve_event_mval_region(event, psd->vc.region, event_mval);
      }
      else {
        ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
        if (!tracker.found() && psd->vc.region) {
          tracker.use_fallback_region(psd->vc.region);
        }
        if (tracker.found()) {
          psd->vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
          paintcurve_slide_refresh_object_mats(psd);
        }
        event_mval[0] = tracker.mval().x;
        event_mval[1] = tracker.mval().y;
      }
      const float mval_fl[2] = {float(event_mval[0]), float(event_mval[1])};

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
          if (snap_to_surface &&
              paintcurve_surface_place(
                  C, psd->snap_ctx, psd->vc, mval_fl, prev_co_world, false, hit_obj, hit_no_obj))
          {
            mul_v3_m4v3(depth_world, psd->ob_to_world, hit_obj);
            paintcurve_snap_marker_update(C, psd->ob_to_world, hit_obj);
          }
          else {
            paintcurve_snap_marker_clear();
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
            if (paintcurve_surface_place(
                    C, psd->snap_ctx, psd->vc, mval_fl, prev_co_world, false, hit_obj, hit_no_obj))
            {
              paintcurve_apply_surface_snap_to_point(geom, psd, hit_obj, hit_no_obj);
              snapped = true;
              paintcurve_snap_marker_update(C, psd->ob_to_world, hit_obj);
            }
          }

          if (!snapped) {
            paintcurve_snap_marker_clear();
            paintcurve_apply_handle_move_3d(geom,
                                            pc,
                                            psd->point_index,
                                            &psd->vc,
                                            psd->ob_to_world,
                                            psd->world_to_ob,
                                            psd,
                                            mval_fl);
          }

          geom.tag_positions_changed();
        }
      }
      else {
        paintcurve_snap_marker_clear();
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
      copy_v2_v2_int(psd->prev_mval, event_mval);
      paintcurve_sync_to_source_if_3d(C, pc);
      ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
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
  ViewContext vc;
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
  int event_mval_arr[2];
  if (!pc->use_3d_space && vc.region) {
    paintcurve_event_mval_region(event, vc.region, event_mval_arr);
  }
  else {
    ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
    if (!tracker.found() && vc.region) {
      tracker.use_fallback_region(vc.region);
    }
    event_mval_arr[0] = tracker.mval().x;
    event_mval_arr[1] = tracker.mval().y;
  }
  const int2 event_mval = {event_mval_arr[0], event_mval_arr[1]};
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);
  if (screen_points.is_empty()) {
    return OPERATOR_PASS_THROUGH;
  }

  const float loc_fl[2] = {float(event_mval.x), float(event_mval.y)};
  const int point_index = paintcurve_find_radius_handle_at_pos(
      pc, screen_points.data(), loc_fl, PAINT_CURVE_RADIUS_HANDLE_CIRCLE_RADIUS);
  if (point_index < 0) {
    return OPERATOR_PASS_THROUGH;
  }

  RadiusSlideData *rsd = MEM_new<RadiusSlideData>(__func__);
  rsd->point_index = point_index;
  paintcurve_radius_handle_screen_get(pc, screen_points.data(), point_index, &rsd->handle);
  rsd->event = event->type;
  rsd->vc = vc;

  op->customdata = rsd;
  BKE_brush_tag_unsaved_changes(br);
  paintcurve_slide_radius_status_set(C, paintcurve_get_point_radius(pc, point_index));
  paintcurve_slide_segment_clear();
  paintcurve_slide_active = true;
  WM_event_add_modal_handler_all_windows(C, op, SPACE_VIEW3D, RGN_TYPE_WINDOW);
  ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
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
    ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
    WM_event_remove_modal_handler_other_windows(C, op);
    return OPERATOR_FINISHED;
  }

  if (event->type == MOUSEMOVE && pc) {
    int event_mval[2];
    if (!pc->use_3d_space && rsd->vc.region) {
      paintcurve_event_mval_region(event, rsd->vc.region, event_mval);
    }
    else {
      ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
      if (!tracker.found() && rsd->vc.region) {
        tracker.use_fallback_region(rsd->vc.region);
      }
      event_mval[0] = tracker.mval().x;
      event_mval[1] = tracker.mval().y;
    }

    bke::CurvesGeometry &geom = pc->geometry.wrap();
    if (paintcurve_geometry_is_valid(geom) && rsd->point_index < geom.points_num()) {
      const float mval_fl[2] = {float(event_mval[0]), float(event_mval[1])};
      const float new_radius = paintcurve_radius_from_handle_screen_pos(&rsd->handle, mval_fl);
      geom.radius_for_write()[rsd->point_index] = new_radius;
      geom.tag_positions_changed();
      paintcurve_slide_radius_status_set(C, new_radius);
      paintcurve_sync_to_source_if_3d(C, pc);
      ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
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
  WM_event_remove_modal_handler_other_windows(C, op);
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
  WM_event_remove_modal_handler_other_windows(C, op);
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
}  // namespace blender
