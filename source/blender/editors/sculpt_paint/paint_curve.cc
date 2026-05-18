/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <climits>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_math_vector.h"
#include "BLI_math_matrix.h"

#include "BLT_translation.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "paint_intern.hh"
#include "mesh/sculpt_intern.hh"

namespace blender {

#define PAINT_CURVE_SELECT_THRESHOLD 40.0f
#define PAINT_CURVE_POINT_SELECT(pcp, i) (*(&pcp->bez.f1 + i) = BEZT_FLAG_SELECT)

/* Helper to get mutable reference or pointer to a point in points_3d.
 * Layout: [left[3], pivot[3], right[3]] for each of the tot_points. */
inline float *paintcurve_3d_co(PaintCurve *pc, int point_idx, int handle_idx)
{
  BLI_assert(pc->points_3d != nullptr);
  BLI_assert(point_idx >= 0 && point_idx < pc->tot_points);
  BLI_assert(handle_idx >= 0 && handle_idx < 3);
  return &pc->points_3d[(point_idx * 3 + handle_idx) * 3];
}

/* Ensure 3D coordinate buffer is allocated and initialized to zero. */
void paintcurve_ensure_3d_buffer(PaintCurve *pc)
{
  if (pc->points_3d == nullptr && pc->tot_points > 0) {
    pc->points_3d = MEM_new_array_zeroed<float>(pc->tot_points * 9, "PaintCurve.points_3d");
  }
}

/* Initialize points_3d by unprojecting existing 2D points onto the plane
 * parallel to the viewport passing through the active object's origin. */
void paintcurve_init_3d_from_2d(PaintCurve *pc, const ViewContext *vc)
{
  if (pc->tot_points == 0) {
    return;
  }
  paintcurve_ensure_3d_buffer(pc);
  if (pc->points_3d == nullptr) {
    return;
  }

  /* Get evaluated object location in world space. */
  float ob_origin_world[3];
  copy_v3_v3(ob_origin_world, vc->obact->object_to_world().location());
  float ob_origin_screen[2];
  ED_view3d_project_v2(vc->region, ob_origin_world, ob_origin_screen);

  /* For each handle, unproject the 2D screen coord to 3D world space,
   * then convert to object space. */
  const float (*world_to_ob)[4] = vc->obact->world_to_object().ptr();
  for (int i = 0; i < pc->tot_points; i++) {
    for (int j = 0; j < 3; j++) {
      const float *screen_co_2d = pc->points[i].bez.vec[j];
      float world_co[3];
      float mval_fl[2] = {screen_co_2d[0], screen_co_2d[1]};
      ED_view3d_win_to_3d(vc->v3d, vc->region, ob_origin_world, mval_fl, world_co);
      mul_v3_m4v3(paintcurve_3d_co(pc, i, j), world_to_ob, world_co);
    }
  }
}

/* Sync 3D coordinates back to 2D screen coordinates.
 * This is used to maintain backward compatibility with 2D paint brush code,
 * overlay drawing, and standard UI. */
void ED_paintcurve_sync_3d_to_2d(PaintCurve *pc,
                                 const ViewContext *vc,
                                 const float ob_to_world[4][4])
{
  if (pc->points_3d == nullptr || pc->tot_points == 0) {
    return;
  }

  for (int i = 0; i < pc->tot_points; i++) {
    for (int j = 0; j < 3; j++) {
      float world_co[3];
      mul_v3_m4v3(world_co, ob_to_world, paintcurve_3d_co(pc, i, j));
      float screen_co[2];
      ED_view3d_project_v2(vc->region, world_co, screen_co);
      pc->points[i].bez.vec[j][0] = screen_co[0];
      pc->points[i].bez.vec[j][1] = screen_co[1];
    }
  }
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

static PaintCurvePoint *paintcurve_find_point(bContext *C,
                                              PaintCurve *pc,
                                              const float pos[2],
                                              bool ignore_pivot,
                                              const float threshold,
                                              char *point)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (pc->use_3d_space && rv3d && pc->points_3d != nullptr) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_3d_to_2d(pc, &vc, ob_to_world);
  }
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

  ED_paintcurve_undo_push_begin(op->type->name);

  /* Lazy-init existing 3D buffer if needed. */
  if (pc->use_3d_space && rv3d) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    if (pc->points_3d == nullptr && pc->tot_points > 0) {
      paintcurve_init_3d_from_2d(pc, &vc);
    }
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

  /* Handle 3D coordinates insertion. */
  if (pc->use_3d_space && rv3d) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

    float mval_fl[2] = {float(loc[0]), float(loc[1])};
    float obj_co[3];
    bool used_snap = false;

    if (snap_to_surface && vc.obact->runtime->sculpt_session != nullptr) {
      used_snap = ed::sculpt_paint::stroke_get_location_bvh(C, obj_co, mval_fl, false);
    }

    if (!used_snap) {
      /* Fallback: unproject onto plane at object origin depth. */
      float ob_origin_world[3];
      copy_v3_v3(ob_origin_world, vc.obact->object_to_world().location());
      float world_co[3];
      ED_view3d_win_to_3d(vc.v3d, vc.region, ob_origin_world, mval_fl, world_co);
      const float (*world_to_ob)[4] = vc.obact->world_to_object().ptr();
      mul_v3_m4v3(obj_co, world_to_ob, world_co);
    }

    float *points_3d_new = MEM_new_array_zeroed<float>((pc->tot_points + 1) * 9, "PaintCurve.points_3d");

    if (pc->points_3d != nullptr) {
      if (add_index > 0) {
        memcpy(points_3d_new, pc->points_3d, add_index * 9 * sizeof(float));
      }
      if (add_index < pc->tot_points) {
        memcpy(points_3d_new + (add_index + 1) * 9,
               pc->points_3d + add_index * 9,
               (pc->tot_points - add_index) * 9 * sizeof(float));
      }
      MEM_SAFE_DELETE(pc->points_3d);
    }
    pc->points_3d = points_3d_new;

    /* Initialize the 3D handles of the new point. */
    for (int j = 0; j < 3; j++) {
      copy_v3_v3(&pc->points_3d[(add_index * 3 + j) * 3], obj_co);
    }
  }

  pc->tot_points++;

  /* initialize new point */
  pcp[add_index] = PaintCurvePoint{};
  copy_v3_v3(pcp[add_index].bez.vec[0], vec);
  copy_v3_v3(pcp[add_index].bez.vec[1], vec);
  copy_v3_v3(pcp[add_index].bez.vec[2], vec);

  /* sync to 2D to ensure pixel-perfect coordinates matching 3D location */
  if (pc->use_3d_space && rv3d && pc->points_3d != nullptr) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
    float ob_to_world[4][4];
    copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
    ED_paintcurve_sync_3d_to_2d(pc, &vc, ob_to_world);
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
  ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;

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

  ED_paintcurve_undo_push_begin(op->type->name);

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
    /* Shrink the 3D buffer as well if it exists. */
    if (pc->points_3d != nullptr) {
      float *points_3d_new = nullptr;
      if (new_tot > 0) {
        points_3d_new = MEM_new_array_zeroed<float>(new_tot * 9, "PaintCurve.points_3d");
        int k = 0;
        for (i = 0, pcp = pc->points; i < pc->tot_points; i++, pcp++) {
          if (!(pcp->bez.f2 & DELETE_TAG)) {
            memcpy(points_3d_new + k * 9, pc->points_3d + i * 9, 9 * sizeof(float));
            k++;
          }
        }
      }
      MEM_SAFE_DELETE(pc->points_3d);
      pc->points_3d = points_3d_new;
    }

    MEM_delete(pc->points);

    pc->points = points_new;
    pc->tot_points = new_tot;
  }

#undef DELETE_TAG

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
  ot->flag = OPTYPE_UNDO;
}

static bool paintcurve_point_select(
    bContext *C, wmOperator *op, const int loc[2], bool toggle, bool extend)
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

  ED_paintcurve_undo_push_begin(op->type->name);

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
      BKE_paint_curve_clamp_endpoint_add_index(pc, pcp - pc->points);

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
  bool toggle = RNA_boolean_get(op->ptr, "toggle");
  bool extend = RNA_boolean_get(op->ptr, "extend");
  if (paintcurve_point_select(C, op, loc, toggle, extend)) {
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
    if (paintcurve_point_select(C, op, loc, toggle, extend)) {
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
  ot->flag = OPTYPE_UNDO | OPTYPE_REGISTER;

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

struct PointSlideData {
  PaintCurvePoint *pcp;
  char select;
  int initial_loc[2];
  float point_initial_loc[3][2];
  int event;
  bool align;
  /* NEW: */
  float point_initial_loc_3d[3][3];
};

static wmOperatorStatus paintcurve_slide_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  const float loc_fl[2] = {float(event->mval[0]), float(event->mval[1])};
  char select;
  int i;
  bool do_select = RNA_boolean_get(op->ptr, "select");
  bool align = RNA_boolean_get(op->ptr, "align");
  Brush *br = BKE_paint_brush(paint);
  PaintCurve *pc = br->paint_curve;
  PaintCurvePoint *pcp;

  if (!pc) {
    return OPERATOR_PASS_THROUGH;
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

  if (pcp) {
    ARegion *region = CTX_wm_region(C);
    wmWindow *window = CTX_wm_window(C);
    PointSlideData *psd = MEM_new_uninitialized<PointSlideData>("PointSlideData");
    copy_v2_v2_int(psd->initial_loc, event->mval);
    psd->event = event->type;
    psd->pcp = pcp;
    psd->select = paintcurve_point_co_index(select);
    for (i = 0; i < 3; i++) {
      copy_v2_v2(psd->point_initial_loc[i], pcp->bez.vec[i]);
    }
    psd->align = align;
    if (pc->points_3d != nullptr) {
      for (i = 0; i < 3; i++) {
        copy_v3_v3(psd->point_initial_loc_3d[i], paintcurve_3d_co(pc, pcp - pc->points, i));
      }
    }
    op->customdata = psd;

    /* first, clear all selection from points */
    for (i = 0; i < pc->tot_points; i++) {
      pc->points[i].bez.f1 = pc->points[i].bez.f3 = pc->points[i].bez.f2 = eBezTriple_Flag{};
    }

    /* only select the active point */
    PAINT_CURVE_POINT_SELECT(pcp, psd->select);
    BKE_paint_curve_clamp_endpoint_add_index(pc, pcp - pc->points);
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
    MEM_delete(psd);
    ED_paintcurve_undo_push_begin(op->type->name);
    ED_paintcurve_undo_push_end(C);
    return OPERATOR_FINISHED;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      ARegion *region = CTX_wm_region(C);
      wmWindow *window = CTX_wm_window(C);
      RegionView3D *rv3d = CTX_wm_region_view3d(C);
      Paint *paint = BKE_paint_get_active_from_context(C);
      Brush *br = BKE_paint_brush(paint);
      PaintCurve *pc = br ? br->paint_curve : nullptr;

      if (pc && pc->use_3d_space && rv3d && pc->points_3d != nullptr) {
        Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
        ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

        float ob_to_world[4][4];
        float world_to_ob[4][4];
        copy_m4_m4(ob_to_world, vc.obact->object_to_world().ptr());
        copy_m4_m4(world_to_ob, vc.obact->world_to_object().ptr());

        float mval_curr_fl[2] = {float(event->mval[0]), float(event->mval[1])};
        const bool snap_to_surface = (event->modifier & KM_CTRL) != 0;

        int pcp_idx = psd->pcp - pc->points;

        /* Try surface snap via PBVH raycast when Ctrl is held. Falls back to plane-drag on miss. */
        bool snapped = false;
        if (snap_to_surface && vc.obact->runtime->sculpt_session != nullptr) {
          float hit_obj[3];
          if (ed::sculpt_paint::stroke_get_location_bvh(C, hit_obj, mval_curr_fl, false)) {
            snapped = true;
            if (psd->select == 1) {
              /* Pivot snapped to surface; offset all three handles by the same delta. */
              float snap_delta[3];
              sub_v3_v3v3(snap_delta, hit_obj, psd->point_initial_loc_3d[1]);
              for (int i = 0; i < 3; i++) {
                add_v3_v3v3(
                    paintcurve_3d_co(pc, pcp_idx, i), snap_delta, psd->point_initial_loc_3d[i]);
              }
            }
            else {
              /* Individual handle snapped to surface. */
              copy_v3_v3(paintcurve_3d_co(pc, pcp_idx, psd->select), hit_obj);
              if (psd->align) {
                /* Mirror the opposite handle through the pivot. */
                float dir[3];
                sub_v3_v3v3(dir,
                             paintcurve_3d_co(pc, pcp_idx, psd->select),
                             paintcurve_3d_co(pc, pcp_idx, 1));
                const char opposite = (psd->select == 0) ? 2 : 0;
                sub_v3_v3v3(paintcurve_3d_co(pc, pcp_idx, opposite),
                             paintcurve_3d_co(pc, pcp_idx, 1),
                             dir);
              }
            }
          }
        }

        if (!snapped) {
          /* Default: move along the plane perpendicular to the view through the initial pivot. */
          float initial_pivot_world[3];
          mul_v3_m4v3(initial_pivot_world, ob_to_world, psd->point_initial_loc_3d[1]);

          float mval_init_fl[2] = {float(psd->initial_loc[0]), float(psd->initial_loc[1])};

          float world_init[3], world_curr[3];
          ED_view3d_win_to_3d(vc.v3d, vc.region, initial_pivot_world, mval_init_fl, world_init);
          ED_view3d_win_to_3d(vc.v3d, vc.region, initial_pivot_world, mval_curr_fl, world_curr);

          float obj_init[3], obj_curr[3], obj_delta[3];
          mul_v3_m4v3(obj_init, world_to_ob, world_init);
          mul_v3_m4v3(obj_curr, world_to_ob, world_curr);
          sub_v3_v3v3(obj_delta, obj_curr, obj_init);

          if (psd->select == 1) {
            for (int i = 0; i < 3; i++) {
              add_v3_v3v3(
                  paintcurve_3d_co(pc, pcp_idx, i), obj_delta, psd->point_initial_loc_3d[i]);
            }
          }
          else {
            add_v3_v3v3(paintcurve_3d_co(pc, pcp_idx, psd->select),
                        obj_delta,
                        psd->point_initial_loc_3d[psd->select]);

            if (psd->align) {
              const char opposite = (psd->select == 0) ? 2 : 0;
              float opposite_delta[3];
              sub_v3_v3v3(opposite_delta,
                          paintcurve_3d_co(pc, pcp_idx, 1),
                          paintcurve_3d_co(pc, pcp_idx, psd->select));
              add_v3_v3v3(paintcurve_3d_co(pc, pcp_idx, opposite),
                          paintcurve_3d_co(pc, pcp_idx, 1),
                          opposite_delta);
            }
          }
        }

        ED_paintcurve_sync_3d_to_2d(pc, &vc, ob_to_world);
      }
      else {
        float diff[2] = {float(event->mval[0] - psd->initial_loc[0]),
                         float(event->mval[1] - psd->initial_loc[1])};
        if (psd->select == 1) {
          int i;
          for (i = 0; i < 3; i++) {
            add_v2_v2v2(psd->pcp->bez.vec[i], diff, psd->point_initial_loc[i]);
          }
        }
        else {
          add_v2_v2(diff, psd->point_initial_loc[psd->select]);
          copy_v2_v2(psd->pcp->bez.vec[psd->select], diff);

          if (psd->align) {
            char opposite = (psd->select == 0) ? 2 : 0;
            sub_v2_v2v2(diff, psd->pcp->bez.vec[1], psd->pcp->bez.vec[psd->select]);
            add_v2_v2v2(psd->pcp->bez.vec[opposite], psd->pcp->bez.vec[1], diff);
          }
        }
      }
      WM_paint_cursor_tag_redraw(window, region);
      break;
    }
    default:
      break;
  }

  return OPERATOR_RUNNING_MODAL;
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
  ot->flag = OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(
      ot->srna, "align", false, "Align Handles", "Aligns opposite point handle during transform");
  RNA_def_boolean(
      ot->srna, "select", true, "Select", "Attempt to select a point handle before transform");
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
  ot->flag = OPTYPE_UNDO;
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

}  // namespace blender
