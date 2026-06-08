/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <climits>
#include <cstring>
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

#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_matrix.h"
#include "BLI_string_utf8.h"
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
#include "BKE_paint_types.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_types.hh"
#include "UI_view2d.hh"

#include "paint_intern.hh"
#include "mesh/sculpt_intern.hh"

namespace blender {

#define PAINT_CURVE_SELECT_THRESHOLD 40.0f
#define PAINT_CURVE_POINT_SELECT(pcp, i) (*(&pcp->bez.f1 + i) = BEZT_FLAG_SELECT)

/* Set by select when a handle-type cycle consumed the click; slide must not start a drag. */
static bool paintcurve_skip_next_slide = false;

static void paintcurve_sync_after_handle_type_change(bContext *C, PaintCurve *pc)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (pc->use_3d_space && rv3d && pc->geometry.wrap().points_num() > 0) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
    ED_paintcurve_sync_to_source_object(C, pc);
  }
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

  return false;
}

#define SEL_F1 (1 << 0)
#define SEL_F2 (1 << 1)
#define SEL_F3 (1 << 2)

/* returns 0, 1, or 2 in point according to handle 1, pivot or handle 2 */
static PaintCurvePoint *paintcurve_point_get_closest(
    PaintCurve *pc, const float pos[2], bool ignore_pivot, const float threshold, char *point)
{
  PaintCurvePoint *pcp, *closest = nullptr;
  int i;
  float closest_dist = threshold;

  for (i = 0, pcp = pc->points; i < pc->tot_points; i++, pcp++) {
    float dist[3];
    char point_sel = 0;

    dist[0] = len_manhattan_v2v2(pos, pcp->bez.vec[0]);
    dist[1] = len_manhattan_v2v2(pos, pcp->bez.vec[1]);
    dist[2] = len_manhattan_v2v2(pos, pcp->bez.vec[2]);

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
      closest = pcp;
      if (point) {
        if (ignore_pivot && point_sel == SEL_F2) {
          point_sel = (dist[0] < dist[2]) ? SEL_F1 : SEL_F3;
        }
        *point = point_sel;
      }
    }
  }

  return closest;
}

static void paintcurve_sync_geometry_to_2d_for_pick(bContext *C, PaintCurve *pc)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (pc->use_3d_space && rv3d && pc->geometry.wrap().points_num() > 0) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
  }
}

static PaintCurvePoint *paintcurve_find_point(bContext *C,
                                              PaintCurve *pc,
                                              const float pos[2],
                                              bool ignore_pivot,
                                              const float threshold,
                                              char *point)
{
  paintcurve_sync_geometry_to_2d_for_pick(C, pc);
  return paintcurve_point_get_closest(pc, pos, ignore_pivot, threshold, point);
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

static PaintCurve *paintcurve_for_brush_add(Main *bmain, const char *name, const Brush *brush)
{
  PaintCurve *curve = BKE_paint_curve_add(bmain, name);
  BKE_id_move_to_same_lib(*bmain, curve->id, brush->id);
  return curve;
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
  const float vec[3] = {float(loc[0]), float(loc[1]), 0.0f};

  PaintCurve *pc = br->paint_curve;
  if (!pc) {
    br->paint_curve = pc = paintcurve_for_brush_add(bmain, DATA_("PaintCurve"), br);
  }

  if (paintcurve_has_multi_curves(pc)) {
    return;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  /* Initialize ViewContext once; used in all 3D-mode branches below. */
  ViewContext vc = {};
  if (pc->use_3d_space && rv3d) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    vc = ED_view3d_viewcontext_init(C, depsgraph);
    paintcurve_ensure_3d_geometry(pc, &vc);
  }

  PaintCurvePoint *pcp = MEM_new_array<PaintCurvePoint>((pc->tot_points + 1), "PaintCurvePoint");
  int add_index = pc->add_index;

  if (pc->points) {
    if (add_index > 0) {
      memcpy(pcp, pc->points, add_index * sizeof(PaintCurvePoint));
    }
    if (add_index < pc->tot_points) {
      memcpy(pcp + add_index + 1,
             pc->points + add_index,
             (pc->tot_points - add_index) * sizeof(PaintCurvePoint));
    }

    MEM_delete(pc->points);
  }
  pc->points = pcp;

  /* Handle 3D coordinates insertion into the embedded geometry. */
  if (pc->use_3d_space && rv3d) {
    float mval_fl[2] = {float(loc[0]), float(loc[1])};
    float obj_co[3];
    bool used_snap = false;

    if (snap_to_surface && vc.obact->runtime->sculpt_session != nullptr) {
      used_snap = ed::sculpt_paint::stroke_get_location_bvh(C, obj_co, mval_fl, false);
    }

    if (!used_snap) {
      float ob_origin_world[3];
      copy_v3_v3(ob_origin_world, vc.obact->object_to_world().location());
      float world_co[3];
      ED_view3d_win_to_3d(vc.v3d, vc.region, ob_origin_world, mval_fl, world_co);
      const float (*world_to_ob)[4] = vc.obact->world_to_object().ptr();
      mul_v3_m4v3(obj_co, world_to_ob, world_co);
    }

    bke::CurvesGeometry &geom = pc->geometry.wrap();
    bke::CurvesGeometry new_geom;
    paintcurve_geometry_init_bezier(new_geom, pc->tot_points + 1);

    const int old_num = geom.points_num();
    MutableSpan<int8_t> new_types_left = new_geom.handle_types_left_for_write();
    MutableSpan<int8_t> new_types_right = new_geom.handle_types_right_for_write();
    for (int dst = 0, src = 0; dst < pc->tot_points + 1; dst++) {
      if (dst == add_index) {
        for (int j = 0; j < 3; j++) {
          copy_v3_v3(paintcurve_geom_co(new_geom, dst, j), obj_co);
        }
        new_types_left[dst] = BEZIER_HANDLE_ALIGN;
        new_types_right[dst] = BEZIER_HANDLE_ALIGN;
        continue;
      }
      if (src < old_num) {
        for (int j = 0; j < 3; j++) {
          copy_v3_v3(paintcurve_geom_co(new_geom, dst, j), paintcurve_geom_co(geom, src, j));
        }
        new_types_left[dst] = geom.handle_types_left()[src];
        new_types_right[dst] = geom.handle_types_right()[src];
        src++;
      }
    }
    new_geom.tag_positions_changed();
    geom = std::move(new_geom);
  }

  pc->tot_points++;

  /* initialize new point */
  pcp[add_index] = PaintCurvePoint{};
  pcp[add_index].bez.radius = 1.0f;
  copy_v3_v3(pcp[add_index].bez.vec[0], vec);
  copy_v3_v3(pcp[add_index].bez.vec[1], vec);
  copy_v3_v3(pcp[add_index].bez.vec[2], vec);

  /* Sync to 2D to ensure pixel-perfect coordinates matching 3D location. */
  if (pc->use_3d_space && rv3d && pc->geometry.wrap().points_num() > 0) {
    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
  }

  if (pc->use_3d_space) {
    ED_paintcurve_sync_to_source_object(C, pc);
  }

  /* last step, clear selection from all bezier handles expect the next */
  for (int i = 0; i < pc->tot_points; i++) {
    pcp[i].bez.f1 = pcp[i].bez.f2 = pcp[i].bez.f3 = eBezTriple_Flag{};
  }

  BKE_paint_curve_clamp_endpoint_add_index(pc, add_index);

  if (pc->add_index != 0) {
    pcp[add_index].bez.f3 = BEZT_FLAG_SELECT;
    pcp[add_index].bez.h2 = HD_ALIGN;
  }
  else {
    pcp[add_index].bez.f1 = BEZT_FLAG_SELECT;
    pcp[add_index].bez.h1 = HD_ALIGN;
  }

  if (pc->use_3d_space && pc->geometry.wrap().points_num() > 0) {
    bke::CurvesGeometry &geom = pc->geometry.wrap();
    geom.handle_types_left_for_write()[add_index] = int8_t(pcp[add_index].bez.h1);
    geom.handle_types_right_for_write()[add_index] = int8_t(pcp[add_index].bez.h2);
    geom.calculate_bezier_auto_handles();
    geom.calculate_bezier_aligned_handles();
    if (rv3d) {
      float ob_to_world[4][4];
      copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
      ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
    }
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
  const bool snap_to_surface = (event->modifier & KM_CTRL) != 0;
  paintcurve_point_add(C, op, loc, snap_to_surface);
  RNA_int_set_array(op->ptr, "location", loc);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus paintcurve_add_point_exec(bContext *C, wmOperator *op)
{
  int loc[2];

  if (RNA_struct_property_is_set(op->ptr, "location")) {
    RNA_int_get_array(op->ptr, "location", loc);
    paintcurve_point_add(C, op, loc);
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
}

static wmOperatorStatus paintcurve_delete_point_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc;
  PaintCurvePoint *pcp;
  wmWindow *window = CTX_wm_window(C);
  ARegion *region = CTX_wm_region(C);
  int i;
  int tot_del = 0;
  pc = br->paint_curve;

  if (!pc || pc->tot_points == 0) {
    return OPERATOR_CANCELLED;
  }

  if (paintcurve_has_multi_curves(pc)) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

#define DELETE_TAG 2

  for (i = 0, pcp = pc->points; i < pc->tot_points; i++, pcp++) {
    if (BEZT_ISSEL_ANY(&pcp->bez)) {
      pcp->bez.f2 |= eBezTriple_Flag(DELETE_TAG);
      tot_del++;
    }
  }

  if (tot_del > 0) {
    int j = 0;
    int new_tot = pc->tot_points - tot_del;
    PaintCurvePoint *points_new = nullptr;
    if (new_tot > 0) {
      points_new = MEM_new_array<PaintCurvePoint>(new_tot, "PaintCurvePoint");
    }

    for (i = 0, pcp = pc->points; i < pc->tot_points; i++, pcp++) {
      if (!(pcp->bez.f2 & DELETE_TAG)) {
        points_new[j] = pc->points[i];

        if ((i + 1) == pc->add_index) {
          BKE_paint_curve_clamp_endpoint_add_index(pc, j);
        }
        j++;
      }
      else if ((i + 1) == pc->add_index) {
        /* prefer previous point */
        pc->add_index = j;
      }
    }
    if (pc->use_3d_space && pc->geometry.wrap().points_num() > 0) {
      bke::CurvesGeometry &geom = pc->geometry.wrap();
      bke::CurvesGeometry new_geom;
      paintcurve_geometry_init_bezier(new_geom, new_tot);
      int k = 0;
      for (i = 0, pcp = pc->points; i < pc->tot_points; i++, pcp++) {
        if (!(pcp->bez.f2 & DELETE_TAG)) {
          for (int j = 0; j < 3; j++) {
            copy_v3_v3(paintcurve_geom_co(new_geom, k, j), paintcurve_geom_co(geom, i, j));
          }
          k++;
        }
      }
      new_geom.tag_positions_changed();
      geom = std::move(new_geom);
    }

    MEM_delete(pc->points);

    pc->points = points_new;
    pc->tot_points = new_tot;
  }

#undef DELETE_TAG

  if (pc->use_3d_space) {
    ED_paintcurve_sync_to_source_object(C, pc);
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
  PaintCurve *pc;
  int i;
  const float loc_fl[2] = {float(loc[0]), float(loc[1])};

  pc = br->paint_curve;

  if (!pc) {
    return false;
  }

  if (!toggle) {
    char pre_selflag;
    PaintCurvePoint *pre_pcp = paintcurve_find_point(
        C, pc, loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &pre_selflag);
    if (pre_pcp && cycle_on_pivot && !extend && pre_selflag == SEL_F2 &&
        BEZT_ISSEL_ANY(&pre_pcp->bez))
    {
      paintcurve_cycle_handle_type_at_point(C, op, pc, int(pre_pcp - pc->points));
      WM_paint_cursor_tag_redraw(window, region);
      return true;
    }
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  if (toggle) {
    PaintCurvePoint *pcp;
    eBezTriple_Flag select = eBezTriple_Flag{};
    bool selected = false;

    pcp = pc->points;

    for (i = 0; i < pc->tot_points; i++) {
      if ((pcp[i].bez.f1 & BEZT_FLAG_SELECT) || (pcp[i].bez.f2 & BEZT_FLAG_SELECT) ||
          (pcp[i].bez.f3 & BEZT_FLAG_SELECT))
      {
        selected = true;
        break;
      }
    }

    if (!selected) {
      select = BEZT_FLAG_SELECT;
    }

    for (i = 0; i < pc->tot_points; i++) {
      pc->points[i].bez.f1 = pc->points[i].bez.f2 = pc->points[i].bez.f3 = select;
    }
  }
  else {
    PaintCurvePoint *pcp;
    char selflag;

    pcp = paintcurve_find_point(C, pc, loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &selflag);

    if (pcp) {
      if (!paintcurve_has_multi_curves(pc)) {
        BKE_paint_curve_clamp_endpoint_add_index(pc, pcp - pc->points);
      }

      if (selflag == SEL_F2) {
        if (extend) {
          pcp->bez.f2 ^= BEZT_FLAG_SELECT;
        }
        else {
          pcp->bez.f2 |= BEZT_FLAG_SELECT;
        }
      }
      else if (selflag == SEL_F1) {
        if (extend) {
          pcp->bez.f1 ^= BEZT_FLAG_SELECT;
        }
        else {
          pcp->bez.f1 |= BEZT_FLAG_SELECT;
        }
      }
      else if (selflag == SEL_F3) {
        if (extend) {
          pcp->bez.f3 ^= BEZT_FLAG_SELECT;
        }
        else {
          pcp->bez.f3 |= BEZT_FLAG_SELECT;
        }
      }
    }

    /* clear selection for unselected points if not extending and if a point has been selected */
    if (!extend && pcp) {
      for (i = 0; i < pc->tot_points; i++) {
        pc->points[i].bez.f1 = pc->points[i].bez.f2 = pc->points[i].bez.f3 = eBezTriple_Flag{};

        if ((pc->points + i) == pcp) {
          char index = paintcurve_point_co_index(selflag);
          PAINT_CURVE_POINT_SELECT(pcp, index);
        }
      }
    }

    if (!pcp) {
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
    if (pc) {
      char selflag;
      PaintCurvePoint *pcp = paintcurve_find_point(
          C, pc, loc_fl, false, PAINT_CURVE_SELECT_THRESHOLD, &selflag);
      if (pcp && selflag == SEL_F2) {
        ARegion *region = CTX_wm_region(C);
        wmWindow *window = CTX_wm_window(C);
        paintcurve_cycle_handle_type_at_point(C, op, pc, int(pcp - pc->points));
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

static void paintcurve_sync_legacy_handle_types_from_geometry(PaintCurve *pc, const int point_index)
{
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  pc->points[point_index].bez.h1 = eBezTriple_Handle(geom.handle_types_left()[point_index]);
  pc->points[point_index].bez.h2 = eBezTriple_Handle(geom.handle_types_right()[point_index]);
}

static void paintcurve_sync_legacy_bez_from_geometry_point(PaintCurve *pc, const int point_index)
{
  bke::CurvesGeometry &geom = pc->geometry.wrap();
  BezTriple &bezt = pc->points[point_index].bez;
  const Span<float3> handles_left = *geom.handle_positions_left();
  const Span<float3> handles_right = *geom.handle_positions_right();
  copy_v3_v3(bezt.vec[0], handles_left[point_index]);
  copy_v3_v3(bezt.vec[1], geom.positions()[point_index]);
  copy_v3_v3(bezt.vec[2], handles_right[point_index]);
  paintcurve_sync_legacy_handle_types_from_geometry(pc, point_index);
}

static void paintcurve_sync_legacy_bez_from_geometry_segment(PaintCurve *pc,
                                                             const int point_i1,
                                                             const int point_i2)
{
  paintcurve_sync_legacy_bez_from_geometry_point(pc, point_i1);
  paintcurve_sync_legacy_bez_from_geometry_point(pc, point_i2);
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
      *r_param = resolu_index + len_vec1 * cos_v2v2v2(point, point1, point2) / len_v2(edge);
    }
  }
  else if (*r_min_dist > len_vec2) {
    *r_min_dist = len_vec2;
    *r_segment_index = segment_index;
    *r_param = resolu_index;
  }
}

static void paintcurve_find_closest_on_bezier_segment(const float pos[2],
                                                      const PaintCurvePoint &pcp_a,
                                                      const PaintCurvePoint &pcp_b,
                                                      const int segment_index,
                                                      float *r_min_dist,
                                                      int *r_best_segment,
                                                      float *r_best_param)
{
  const BezTriple *bezt1 = &pcp_a.bez;
  const BezTriple *bezt2 = &pcp_b.bez;
  float *points = MEM_new_array_uninitialized<float>(3 * (PAINT_CURVE_NUM_SEGMENTS + 1),
                                                     "paintcurve_segment_eval");

  for (int j = 0; j < 3; j++) {
    BKE_curve_forward_diff_bezier(bezt1->vec[1][j],
                                  bezt1->vec[2][j],
                                  bezt2->vec[0][j],
                                  bezt2->vec[1][j],
                                  points + j,
                                  PAINT_CURVE_NUM_SEGMENTS,
                                  sizeof(float[3]));
  }

  float point1[2], point2[2];
  copy_v2_v2(point1, points);
  const float len_vec1 = len_v2v2(pos, point1);
  float segment_min_dist = *r_min_dist;
  int local_segment_index = segment_index;
  float param = 0.0f;

  if (segment_min_dist > len_vec1) {
    segment_min_dist = len_vec1;
    param = 0.0f;
  }

  for (int j = 0; j < PAINT_CURVE_NUM_SEGMENTS; j++) {
    copy_v2_v2(point2, points + 3 * (j + 1));
    paintcurve_update_edge_hit(
        pos, point1, point2, segment_index, j, &segment_min_dist, &local_segment_index, &param);
    copy_v2_v2(point1, point2);
  }

  MEM_delete(points);

  if (*r_min_dist > segment_min_dist) {
    *r_min_dist = segment_min_dist;
    *r_best_segment = local_segment_index;
    *r_best_param = param / float(PAINT_CURVE_NUM_SEGMENTS);
  }
}

static bool paintcurve_find_closest_segment(PaintCurve *pc,
                                            const float pos[2],
                                            const float threshold,
                                            int *r_segment_index,
                                            int *r_segment_index_next,
                                            float *r_edge_t)
{
  if (pc->tot_points < 2) {
    return false;
  }

  float min_dist = threshold;
  int best_segment = -1;
  int best_segment_next = -1;
  float best_param = 0.0f;

  paintcurve_foreach_bezier_segment(pc, [&](const int point_a, const int point_b) {
    float segment_min_dist = min_dist;
    int segment_start = -1;
    float segment_param = 0.0f;
    paintcurve_find_closest_on_bezier_segment(pos,
                                              pc->points[point_a],
                                              pc->points[point_b],
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

static void paintcurve_apply_segment_move_2d(PaintCurve *pc,
                                             const int point_i1,
                                             const int point_i2,
                                             const float segment_t,
                                             const float mval[2])
{
  BezTriple *bezt1 = &pc->points[point_i1].bez;
  BezTriple *bezt2 = &pc->points[point_i2].bez;
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

  const float3 k2(bezt1->vec[2][0] - bezt2->vec[0][0],
                  bezt1->vec[2][1] - bezt2->vec[0][1],
                  bezt1->vec[2][2] - bezt2->vec[0][2]);
  const float3 P0(bezt1->vec[1]);
  const float3 P3(bezt2->vec[1]);
  const float3 Pm(mval[0], mval[1], 0.0f);
  const float3 P1 = (Pm - one_minus_t_cu * P0 - t_cu * P3) / denom + k2 * t;
  const float3 P2 = P1 - k2;

  copy_v3_v3(bezt1->vec[2], P1);
  copy_v3_v3(bezt2->vec[0], P2);
  bezt1->h2 = HD_FREE;
  bezt2->h1 = HD_FREE;
  if (bezt1->h1 == HD_ALIGN) {
    bezt1->h1 = HD_FREE;
  }
  if (bezt2->h2 == HD_ALIGN) {
    bezt2->h2 = HD_FREE;
  }
}

static void paintcurve_apply_segment_move_3d(bke::CurvesGeometry &geom,
                                             PaintCurve *pc,
                                             const int point_i1,
                                             const int point_i2,
                                             const float segment_t,
                                             const ViewContext *vc,
                                             const float ob_to_world[4][4],
                                             const float world_to_ob[4][4],
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

  const float3 depth_point = positions[point_i1];
  float depth_world[3];
  mul_v3_m4v3(depth_world, ob_to_world, depth_point);
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
  types_right[point_i1] = BEZIER_HANDLE_FREE;
  types_left[point_i2] = BEZIER_HANDLE_FREE;
  if (types_left[point_i1] == BEZIER_HANDLE_ALIGN) {
    types_left[point_i1] = BEZIER_HANDLE_FREE;
  }
  if (types_right[point_i2] == BEZIER_HANDLE_ALIGN) {
    types_right[point_i2] = BEZIER_HANDLE_FREE;
  }

  geom.calculate_bezier_auto_handles();
  paintcurve_sync_legacy_bez_from_geometry_segment(pc, point_i1, point_i2);
}

static bool paintcurve_insert_point_at_segment(bContext *C,
                                               wmOperator *op,
                                               PaintCurve *pc,
                                               const int segment_index,
                                               const float edge_t)
{
  if (paintcurve_has_multi_curves(pc)) {
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

  const int old_tot = pc->tot_points;
  const int insert_index = segment_index + 1;
  const int point_i2 = segment_index_next;
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  ViewContext vc = {};
  bke::CurvesGeometry *geom_ptr = nullptr;
  if (pc->use_3d_space && rv3d) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    vc = ED_view3d_viewcontext_init(C, depsgraph);
    paintcurve_ensure_3d_geometry(pc, &vc);
    if (pc->geometry.wrap().points_num() == old_tot) {
      geom_ptr = &pc->geometry.wrap();
    }
  }

  bke::curves::bezier::Insertion inserted;
  if (geom_ptr != nullptr) {
    const Span<float3> positions = geom_ptr->positions();
    const Span<float3> handles_left = *geom_ptr->handle_positions_left();
    const Span<float3> handles_right = *geom_ptr->handle_positions_right();
    inserted = bke::curves::bezier::insert(positions[segment_index],
                                           handles_right[segment_index],
                                           handles_left[point_i2],
                                           positions[point_i2],
                                           edge_t);
  }
  else {
    const BezTriple &bezt1 = pc->points[segment_index].bez;
    const BezTriple &bezt2 = pc->points[point_i2].bez;
    inserted = bke::curves::bezier::insert(float3(bezt1.vec[1]),
                                           float3(bezt1.vec[2]),
                                           float3(bezt2.vec[0]),
                                           float3(bezt2.vec[1]),
                                           edge_t);
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);

  PaintCurvePoint *points_new = MEM_new_array<PaintCurvePoint>(old_tot + 1, "PaintCurvePoint");
  for (int i = 0; i < insert_index; i++) {
    points_new[i] = pc->points[i];
  }
  for (int i = insert_index; i < old_tot; i++) {
    points_new[i + 1] = pc->points[i];
  }

  copy_v3_v3(points_new[segment_index].bez.vec[2], inserted.handle_prev);
  points_new[segment_index].bez.h2 = HD_FREE;

  points_new[insert_index] = PaintCurvePoint{};
  copy_v3_v3(points_new[insert_index].bez.vec[0], inserted.left_handle);
  copy_v3_v3(points_new[insert_index].bez.vec[1], inserted.position);
  copy_v3_v3(points_new[insert_index].bez.vec[2], inserted.right_handle);
  points_new[insert_index].bez.h1 = HD_ALIGN;
  points_new[insert_index].bez.h2 = HD_ALIGN;

  copy_v3_v3(points_new[insert_index + 1].bez.vec[0], inserted.handle_next);
  points_new[insert_index + 1].bez.h1 = HD_FREE;

  MEM_delete(pc->points);
  pc->points = points_new;
  pc->tot_points = old_tot + 1;
  if (pc->add_index >= insert_index) {
    pc->add_index++;
  }

  if (geom_ptr != nullptr) {
    bke::CurvesGeometry &geom = *geom_ptr;
    bke::CurvesGeometry new_geom;
    paintcurve_geometry_init_bezier(new_geom, old_tot + 1);
    for (int i = 0; i < insert_index; i++) {
      for (int h = 0; h < 3; h++) {
        copy_v3_v3(paintcurve_geom_co(new_geom, i, h), paintcurve_geom_co(geom, i, h));
      }
      new_geom.handle_types_left_for_write()[i] = geom.handle_types_left()[i];
      new_geom.handle_types_right_for_write()[i] = geom.handle_types_right()[i];
    }
    for (int i = insert_index; i < old_tot; i++) {
      for (int h = 0; h < 3; h++) {
        copy_v3_v3(paintcurve_geom_co(new_geom, i + 1, h), paintcurve_geom_co(geom, i, h));
      }
      new_geom.handle_types_left_for_write()[i + 1] = geom.handle_types_left()[i];
      new_geom.handle_types_right_for_write()[i + 1] = geom.handle_types_right()[i];
    }
    copy_v3_v3(new_geom.positions_for_write()[insert_index], inserted.position);
    copy_v3_v3(new_geom.handle_positions_left_for_write()[insert_index], inserted.left_handle);
    copy_v3_v3(new_geom.handle_positions_right_for_write()[insert_index], inserted.right_handle);
    copy_v3_v3(new_geom.handle_positions_right_for_write()[segment_index], inserted.handle_prev);
    copy_v3_v3(new_geom.handle_positions_left_for_write()[insert_index + 1], inserted.handle_next);
    new_geom.handle_types_left_for_write()[insert_index] = BEZIER_HANDLE_ALIGN;
    new_geom.handle_types_right_for_write()[insert_index] = BEZIER_HANDLE_ALIGN;
    new_geom.handle_types_right_for_write()[segment_index] = BEZIER_HANDLE_FREE;
    new_geom.handle_types_left_for_write()[insert_index + 1] = BEZIER_HANDLE_FREE;
    new_geom.calculate_bezier_auto_handles();
    new_geom.calculate_bezier_aligned_handles();
    new_geom.tag_positions_changed();
    geom = std::move(new_geom);

    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
    ED_paintcurve_sync_to_source_object(C, pc);
  }

  for (int i = 0; i < pc->tot_points; i++) {
    pc->points[i].bez.f1 = pc->points[i].bez.f2 = pc->points[i].bez.f3 = eBezTriple_Flag{};
  }
  pc->points[insert_index].bez.f2 = BEZT_FLAG_SELECT;

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = paint ? BKE_paint_brush(paint) : nullptr;
  if (br) {
    BKE_brush_tag_unsaved_changes(br);
  }

  ED_paintcurve_undo_push_end(C);
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  return true;
}

struct PointSlideData {
  bool is_segment;
  int segment_index;
  int segment_index_next;
  float segment_t;
  PaintCurvePoint *pcp;
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
  /* 3D object-space positions at drag start; used to recompute 3D delta on each mouse move
   * without accumulating floating-point drift from repeated screen-to-3D projections. */
  float point_initial_loc_3d[3][3];
};

static void paintcurve_apply_handle_move_2d(BezTriple *bezt,
                                            int8_t types_left,
                                            int8_t types_right,
                                            const PointSlideData *psd,
                                            const float mval[2])
{
  const bool pivot_only = (psd->select == 1);
  const bool is_left = (psd->select == 0);
  const float delta[2] = {mval[0] - float(psd->prev_mval[0]), mval[1] - float(psd->prev_mval[1])};

  if (pivot_only || psd->move_entire) {
    const float total_delta[2] = {mval[0] - float(psd->initial_loc[0]),
                                  mval[1] - float(psd->initial_loc[1])};
    for (int i = 0; i < 3; i++) {
      add_v2_v2v2(bezt->vec[i], total_delta, psd->point_initial_loc[i]);
    }
    return;
  }

  int8_t h_left = types_left;
  int8_t h_right = types_right;

  if (psd->move_handle) {
    const int handle_idx = is_left ? 0 : 2;
    bezt->vec[handle_idx][0] += delta[0];
    bezt->vec[handle_idx][1] += delta[1];
    h_left = BEZIER_HANDLE_FREE;
    h_right = BEZIER_HANDLE_FREE;
  }
  else {
    if (psd->handle_moved) {
      h_left = BEZIER_HANDLE_ALIGN;
      h_right = BEZIER_HANDLE_ALIGN;
    }

    float2 offset(mval[0] - bezt->vec[1][0], mval[1] - bezt->vec[1][1]);
    if (psd->snap_angle) {
      offset = paintcurve_snap_8_angles(offset);
    }

    if (is_left) {
      if (h_right == BEZIER_HANDLE_AUTO) {
        h_right = BEZIER_HANDLE_ALIGN;
      }
      h_left = h_right;
      if (h_right == BEZIER_HANDLE_VECTOR) {
        h_left = BEZIER_HANDLE_FREE;
      }
      bezt->vec[0][0] = bezt->vec[1][0] + offset.x;
      bezt->vec[0][1] = bezt->vec[1][1] + offset.y;
      if (h_right == BEZIER_HANDLE_ALIGN) {
        bezt->vec[2][0] = 2.0f * bezt->vec[1][0] - bezt->vec[0][0];
        bezt->vec[2][1] = 2.0f * bezt->vec[1][1] - bezt->vec[0][1];
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
      bezt->vec[2][0] = bezt->vec[1][0] + offset.x;
      bezt->vec[2][1] = bezt->vec[1][1] + offset.y;
      if (h_left == BEZIER_HANDLE_ALIGN) {
        bezt->vec[0][0] = 2.0f * bezt->vec[1][0] - bezt->vec[2][0];
        bezt->vec[0][1] = 2.0f * bezt->vec[1][1] - bezt->vec[2][1];
      }
    }
  }

  bezt->h1 = eBezTriple_Handle(h_left);
  bezt->h2 = eBezTriple_Handle(h_right);
}

static void paintcurve_apply_handle_move_3d(bke::CurvesGeometry &geom,
                                            PaintCurve *pc,
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
      add_v3_v3v3(paintcurve_geom_co(geom, point_index, i), obj_delta, psd->point_initial_loc_3d[i]);
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
    paintcurve_screen_to_object(vc, pivot_world, world_to_ob, target_screen, paintcurve_geom_co(geom, point_index, handle_idx));
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
      offset = paintcurve_snap_8_angles(offset);
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
  paintcurve_sync_legacy_handle_types_from_geometry(pc, point_index);
}

static wmOperatorStatus paintcurve_slide_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  char select;
  int i;
  bool do_select = RNA_boolean_get(op->ptr, "select");
  bool align = RNA_boolean_get(op->ptr, "align");
  const bool move_segment = RNA_boolean_get(op->ptr, "move_segment");
  const bool insert_point = RNA_boolean_get(op->ptr, "insert_point");
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br->paint_curve;
  PaintCurvePoint *pcp;

  if (!pc) {
    return OPERATOR_PASS_THROUGH;
  }

  paintcurve_sync_geometry_to_2d_for_pick(C, pc);

  if (paintcurve_skip_next_slide) {
    paintcurve_skip_next_slide = false;
    return OPERATOR_CANCELLED;
  }

  if (insert_point) {
    if (paintcurve_has_multi_curves(pc)) {
      return OPERATOR_CANCELLED;
    }
    paintcurve_sync_geometry_to_2d_for_pick(C, pc);
    int segment_index;
    int segment_index_next;
    float edge_t;
    if (paintcurve_find_closest_segment(pc,
                                        loc_fl,
                                        PAINT_CURVE_SELECT_THRESHOLD,
                                        &segment_index,
                                        &segment_index_next,
                                        &edge_t))
    {
      if (paintcurve_insert_point_at_segment(C, op, pc, segment_index, edge_t)) {
        paintcurve_skip_next_slide = true;
        return OPERATOR_FINISHED;
      }
    }
    return OPERATOR_CANCELLED;
  }

  if (do_select) {
    pcp = paintcurve_find_point(C, pc, loc_fl, align, PAINT_CURVE_SELECT_THRESHOLD, &select);
  }
  else {
    pcp = nullptr;
    /* just find first selected point */
    for (i = 0; i < pc->tot_points; i++) {
      if ((select = paintcurve_point_side_index(&pc->points[i].bez, i == 0, SEL_F3))) {
        pcp = &pc->points[i];
        break;
      }
    }
  }

  if (!pcp && move_segment) {
    paintcurve_sync_geometry_to_2d_for_pick(C, pc);
    int segment_index;
    int segment_index_next;
    float edge_t;
    if (paintcurve_find_closest_segment(pc,
                                        loc_fl,
                                        PAINT_CURVE_SELECT_THRESHOLD,
                                        &segment_index,
                                        &segment_index_next,
                                        &edge_t))
    {
      ARegion *region = CTX_wm_region(C);
      wmWindow *window = CTX_wm_window(C);
      PointSlideData *psd = MEM_new_uninitialized<PointSlideData>("PointSlideData");
      psd->is_segment = true;
      psd->segment_index = segment_index;
      psd->segment_index_next = segment_index_next;
      psd->segment_t = edge_t;
      psd->pcp = nullptr;
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
      op->customdata = psd;
      BKE_brush_tag_unsaved_changes(br);
      WM_event_add_modal_handler(C, op);
      WM_paint_cursor_tag_redraw(window, region);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  if (pcp) {
    ARegion *region = CTX_wm_region(C);
    wmWindow *window = CTX_wm_window(C);
    PointSlideData *psd = MEM_new_uninitialized<PointSlideData>("PointSlideData");
    psd->is_segment = false;
    psd->segment_index = -1;
    psd->segment_index_next = -1;
    psd->segment_t = 0.0f;
    psd->pcp = pcp;
    psd->select = paintcurve_point_co_index(select);
    psd->point_index = int(pcp - pc->points);
    copy_v2_v2_int(psd->initial_loc, event->mval);
    copy_v2_v2_int(psd->prev_mval, event->mval);
    psd->event = event->type;
    psd->align = align;
    psd->move_entire = false;
    psd->move_handle = false;
    psd->snap_angle = align;
    psd->handle_moved = false;
    for (i = 0; i < 3; i++) {
      copy_v2_v2(psd->point_initial_loc[i], pcp->bez.vec[i]);
    }
    RegionView3D *rv3d = CTX_wm_region_view3d(C);
    if (paintcurve_uses_3d_geometry(pc) && rv3d) {
      bke::CurvesGeometry &geom = pc->geometry.wrap();
      const int pcp_idx = int(pcp - pc->points);
      for (i = 0; i < 3; i++) {
        copy_v3_v3(psd->point_initial_loc_3d[i], paintcurve_geom_co(geom, pcp_idx, i));
      }
    }
    op->customdata = psd;

    /* first, clear all selection from points */
    for (i = 0; i < pc->tot_points; i++) {
      pc->points[i].bez.f1 = pc->points[i].bez.f3 = pc->points[i].bez.f2 = eBezTriple_Flag{};
    }

    /* only select the active point */
    PAINT_CURVE_POINT_SELECT(pcp, psd->select);
    if (!paintcurve_has_multi_curves(pc)) {
      BKE_paint_curve_clamp_endpoint_add_index(pc, pcp - pc->points);
    }
    BKE_brush_tag_unsaved_changes(br);

    WM_event_add_modal_handler(C, op);
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
    MEM_delete(psd);
    /* Commit the slide as a single undo step. In Sculpt mode the paint-curve undo
     * system poll returns false, so this is a no-op there. */
    ED_paintcurve_undo_push_begin(C, op->type->name);
    ED_paintcurve_undo_push_end(C);
    if (release_pc && release_pc->use_3d_space) {
      ED_paintcurve_sync_to_source_object(C, release_pc);
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
    case MOUSEMOVE: {
      ARegion *region = CTX_wm_region(C);
      wmWindow *window = CTX_wm_window(C);
      RegionView3D *rv3d = CTX_wm_region_view3d(C);
      Paint *paint = BKE_paint_get_active_from_context(C);
      Brush *br = BKE_paint_brush(paint);
      PaintCurve *pc = br ? br->paint_curve : nullptr;

      if (!pc) {
        break;
      }

      const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};

      if (psd->is_segment) {
        if (paintcurve_uses_3d_geometry(pc) && rv3d) {
          Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
          ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
          bke::CurvesGeometry &geom = pc->geometry.wrap();
          float ob_to_world[4][4];
          float world_to_ob[4][4];
          copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
          copy_m4_m4(world_to_ob, vc.obact->world_to_object().ptr());
          paintcurve_apply_segment_move_3d(geom,
                                           pc,
                                           psd->segment_index,
                                           psd->segment_index_next,
                                           psd->segment_t,
                                           &vc,
                                           ob_to_world,
                                           world_to_ob,
                                           mval_fl);
          geom.tag_positions_changed();
          ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
        }
        else {
          paintcurve_apply_segment_move_2d(pc,
                                           psd->segment_index,
                                           psd->segment_index_next,
                                           psd->segment_t,
                                           mval_fl);
        }
      }
      else if (paintcurve_uses_3d_geometry(pc) && rv3d) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
        bke::CurvesGeometry &geom = pc->geometry.wrap();

        float ob_to_world[4][4];
        float world_to_ob[4][4];
        copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
        copy_m4_m4(world_to_ob, vc.obact->world_to_object().ptr());

        /* Shift+Ctrl: optional surface snap in sculpt mode. */
        const bool snap_to_surface = (event->modifier & (KM_CTRL | KM_SHIFT)) == (KM_CTRL | KM_SHIFT);
        bool snapped = false;
        if (snap_to_surface && vc.obact->runtime->sculpt_session != nullptr) {
          float hit_obj[3];
          if (ed::sculpt_paint::stroke_get_location_bvh(C, hit_obj, mval_fl, false)) {
            snapped = true;
            if (psd->select == 1 || psd->move_entire) {
              float snap_delta[3];
              sub_v3_v3v3(snap_delta, hit_obj, psd->point_initial_loc_3d[1]);
              for (int h = 0; h < 3; h++) {
                add_v3_v3v3(paintcurve_geom_co(geom, psd->point_index, h),
                             snap_delta,
                             psd->point_initial_loc_3d[h]);
              }
            }
            else {
              copy_v3_v3(paintcurve_geom_co(geom, psd->point_index, psd->select), hit_obj);
            }
            geom.calculate_bezier_auto_handles();
            geom.calculate_bezier_aligned_handles();
            paintcurve_sync_legacy_handle_types_from_geometry(pc, psd->point_index);
          }
        }

        if (!snapped) {
          paintcurve_apply_handle_move_3d(
              geom, pc, psd->point_index, &vc, ob_to_world, world_to_ob, psd, mval_fl);
        }

        geom.tag_positions_changed();
        ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
      }
      else if (psd->pcp) {
        const int8_t types_left = int8_t(psd->pcp->bez.h1);
        const int8_t types_right = int8_t(psd->pcp->bez.h2);
        paintcurve_apply_handle_move_2d(
            &psd->pcp->bez, types_left, types_right, psd, mval_fl);
      }

      copy_v2_v2_int(psd->prev_mval, event->mval);
      WM_paint_cursor_tag_redraw(window, region);
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
       "Move the current handle of the control point freely"},
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
    params.modifier = KM_ANY;
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
  return pc && pc->show_radius_handles && !paintcurve_has_multi_curves(pc);
}

static wmOperatorStatus paintcurve_slide_radius_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  if (!pc || !pc->points) {
    return OPERATOR_PASS_THROUGH;
  }

  paintcurve_sync_geometry_to_2d_for_pick(C, pc);

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

  RadiusSlideData *rsd = MEM_new_uninitialized<RadiusSlideData>("RadiusSlideData");
  rsd->point_index = point_index;
  paintcurve_radius_handle_screen_get(pc, screen_points.data(), point_index, &rsd->handle);
  rsd->event = event->type;

  op->customdata = rsd;
  BKE_brush_tag_unsaved_changes(br);
  paintcurve_slide_radius_status_set(C, paintcurve_get_point_radius(pc, point_index));
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
    ED_paintcurve_undo_push_begin(C, op->type->name);
    ED_paintcurve_undo_push_end(C);
    if (pc && pc->use_3d_space) {
      ED_paintcurve_sync_to_source_object(C, pc);
    }
    return OPERATOR_FINISHED;
  }

  if (event->type == MOUSEMOVE && pc && pc->points) {
    const float mval_fl[2] = {float(event->mval[0]), float(event->mval[1])};
    const float new_radius = paintcurve_radius_from_handle_screen_pos(&rsd->handle, mval_fl);
    pc->points[rsd->point_index].bez.radius = new_radius;
    paintcurve_sync_geometry_radius_from_points(pc);
    paintcurve_slide_radius_status_set(C, new_radius);
    WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  }

  return OPERATOR_RUNNING_MODAL;
}

static void paintcurve_slide_radius_cancel(bContext *C, wmOperator *op)
{
  paintcurve_slide_radius_status_clear(C);
  RadiusSlideData *rsd = static_cast<RadiusSlideData *>(op->customdata);
  MEM_SAFE_DELETE(rsd);
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

void PAINTCURVE_OT_slide(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Slide Paint Curve Point";
  ot->description = "Select and slide paint curve point";
  ot->idname = "PAINTCURVE_OT_slide";

  /* API callbacks. */
  ot->invoke = paintcurve_slide_invoke;
  ot->modal = paintcurve_slide_modal;
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
  prop = RNA_def_boolean(
      ot->srna, "select", true, "Select", "Attempt to select a point handle before transform");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(ot->srna,
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
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr) {
    if (reports) {
      BKE_report(reports, RPT_ERROR, "Sculpt tool settings not available");
    }
    return false;
  }

  if (br == nullptr || br->stroke_method != BRUSH_STROKE_CURVE) {
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
      for (const Nurb *nu = static_cast<const Nurb *>(curve.nurb.first); nu;
           nu = nu->next) {
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

  MEM_SAFE_DELETE(pc->points);
  pc->points = nullptr;
  if (pc->tot_points > 0) {
    pc->points = MEM_new_array<PaintCurvePoint>(pc->tot_points, "PaintCurvePoint");
    for (int i = 0; i < pc->tot_points; i++) {
      pc->points[i] = PaintCurvePoint{};
    }
    paintcurve_init_points_radius_from_geometry(pc);
  }
  pc->add_index = pc->tot_points;
  pc->use_3d_space = 1;

  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (rv3d) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_geometry_to_2d(pc, &vc, ob_to_world);
  }
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
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr) {
    return;
  }
  if (scene->toolsettings->sculpt->paint_curve_source_object == nullptr) {
    return;
  }
  ED_paintcurve_import_from_source_object(C, nullptr, false);
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
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  Object *active = CTX_data_active_object(C);

  if (active == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active object");
    return OPERATOR_CANCELLED;
  }
  if (scene == nullptr || scene->toolsettings == nullptr || scene->toolsettings->sculpt == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Sculpt tool settings not available");
    return OPERATOR_CANCELLED;
  }
  if (br == nullptr || br->paint_curve == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active paint curve on the brush");
    return OPERATOR_CANCELLED;
  }

  PaintCurve *pc = br->paint_curve;
  Sculpt *sculpt = scene->toolsettings->sculpt;

  if (pc->use_3d_space && pc->geometry.wrap().points_num() == 0 && pc->tot_points >= 2) {
    RegionView3D *rv3d = CTX_wm_region_view3d(C);
    if (rv3d) {
      Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
      ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
      paintcurve_geometry_from_2d(pc, &vc);
    }
  }

  if (pc->geometry.wrap().points_num() == 0) {
    BKE_report(op->reports, RPT_ERROR, "Paint curve has no points to export");
    return OPERATOR_CANCELLED;
  }

  Object *src_ob = sculpt->paint_curve_source_object;
  if (src_ob == nullptr || !ELEM(src_ob->type, OB_CURVES, OB_CURVES_LEGACY) ||
      !ID_IS_EDITABLE(&src_ob->id))
  {
    src_ob = BKE_object_add(bmain, scene, view_layer, OB_CURVES, DATA_("PaintCurve"));
    if (src_ob == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "Could not create curve object on the scene");
      return OPERATOR_CANCELLED;
    }
    sculpt->paint_curve_source_object = src_ob;
    sculpt->paint_curve_sync_to_source = 1;
  }

  if (!ED_paintcurve_sync_to_source_object(C, pc)) {
    BKE_report(op->reports, RPT_ERROR, "Could not write paint curve to the scene object");
    return OPERATOR_CANCELLED;
  }

  BKE_brush_tag_unsaved_changes(br);
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, src_ob);
  return OPERATOR_FINISHED;
}

void PAINTCURVE_OT_to_curve_object(wmOperatorType *ot)
{
  ot->name = "Paint Curve to Curve Object";
  ot->description =
      "Create or update a Curves object on the scene from the active paint curve geometry";
  ot->idname = "PAINTCURVE_OT_to_curve_object";

  ot->exec = paintcurve_to_curve_object_exec;
  ot->poll = paint_curve_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender
