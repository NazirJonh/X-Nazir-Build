/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Paint-curve topology operators (split, join endpoints, handle types) and the
 * viewport context menu. Reuses #geometry::curves_merge_endpoints from Curves edit mode.
 */

#include "DNA_brush_types.h"
#include "DNA_screen_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLI_assert.h"
#include "BLI_index_mask.hh"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "ED_curves.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Shared Helpers
 * \{ */

static PaintCurve *paintcurve_from_context(bContext *C, Brush **r_brush = nullptr)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (r_brush) {
    *r_brush = brush;
  }
  return brush ? brush->paint_curve : nullptr;
}

static IndexMask paintcurve_selected_points_mask(const bke::CurvesGeometry &geom,
                                                 IndexMaskMemory &memory)
{
  Vector<int> indices;
  for (const int point_i : geom.points_range()) {
    if (paintcurve_geom_get_selection(geom, point_i) & 0x07) {
      indices.append(point_i);
    }
  }
  return IndexMask::from_indices<int>(indices.as_span(), memory);
}

static bool paintcurve_any_point_selected(const bke::CurvesGeometry &geom)
{
  return paintcurve_geometry_any_point_selected(geom);
}

static void paintcurve_tag_redraw(bContext *C, PaintCurve *pc)
{
  WM_paint_cursor_tag_redraw(CTX_wm_window(C), CTX_wm_region(C));
  if (paintcurve_uses_3d_geometry(pc)) {
    ED_region_tag_redraw(CTX_wm_region(C));
  }
}

static void paintcurve_finish_topology_edit(bContext *C, PaintCurve *pc, Brush *brush)
{
  pc->active_curve = paintcurve_active_curve_get(pc);
  if (paintcurve_uses_3d_geometry(pc)) {
    paintcurve_sync_to_source_object(C, pc);
  }
  BKE_brush_tag_unsaved_changes(brush);
  paintcurve_tag_redraw(C, pc);
}

static int8_t paintcurve_resolve_handle_type(const int8_t handle_type,
                                             const ed::curves::SetHandleType dst_type)
{
  switch (dst_type) {
    case ed::curves::SetHandleType::Free:
      return BEZIER_HANDLE_FREE;
    case ed::curves::SetHandleType::Auto:
      return BEZIER_HANDLE_AUTO;
    case ed::curves::SetHandleType::Vector:
      return BEZIER_HANDLE_VECTOR;
    case ed::curves::SetHandleType::Align:
      return BEZIER_HANDLE_ALIGN;
    case ed::curves::SetHandleType::Toggle:
      return handle_type == BEZIER_HANDLE_FREE ? BEZIER_HANDLE_ALIGN : BEZIER_HANDLE_FREE;
    default:
      break;
  }
  BLI_assert_unreachable();
  return BEZIER_HANDLE_FREE;
}

static bool paintcurve_geometry_split_selected_points(bke::CurvesGeometry &geom,
                                                      ReportList *reports,
                                                      int *r_selected_curve = nullptr)
{
  if (!paintcurve_any_point_selected(geom)) {
    BKE_report(reports, RPT_WARNING, "No points selected");
    return false;
  }

  return paintcurve_geometry_split_at_selected_points(geom, r_selected_curve);
}

static bool paintcurve_make_segment_selection_valid(const bke::CurvesGeometry &geom)
{
  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  const VArray<bool> cyclic = geom.cyclic();

  int endpoint_count = 0;
  int last_curve = -1;

  for (const int curve_i : geom.curves_range()) {
    if (cyclic[curve_i]) {
      return false;
    }
    const IndexRange points = points_by_curve[curve_i];
    if (points.is_empty()) {
      continue;
    }

    const bool start_sel = (paintcurve_geom_get_selection(geom, points.first()) & 0x07) != 0;
    const bool end_sel = (paintcurve_geom_get_selection(geom, points.last()) & 0x07) != 0;
    if (start_sel && end_sel) {
      return false;
    }
    if (start_sel || end_sel) {
      if (curve_i == last_curve) {
        return false;
      }
      last_curve = curve_i;
      endpoint_count++;
    }
  }

  return endpoint_count == 2;
}

static bool paintcurve_geometry_make_segment(PaintCurve *pc,
                                             bke::CurvesGeometry &geom,
                                             ReportList *reports)
{
  if (!paintcurve_make_segment_selection_valid(geom)) {
    BKE_report(reports,
               RPT_ERROR,
               "Select one endpoint on each of two different splines to join");
    return false;
  }

  const OffsetIndices<int> points_by_curve = geom.points_by_curve();
  const VArray<bool> cyclic = geom.cyclic();

  struct CurveEndpoint {
    int curve_index;
    bool is_start;
  };
  Vector<CurveEndpoint> selected_endpoints;

  for (const int curve_i : geom.curves_range()) {
    if (cyclic[curve_i]) {
      continue;
    }
    const IndexRange points = points_by_curve[curve_i];
    if (points.is_empty()) {
      continue;
    }

    const bool start_sel = (paintcurve_geom_get_selection(geom, points.first()) & 0x07) != 0;
    const bool end_sel = (paintcurve_geom_get_selection(geom, points.last()) & 0x07) != 0;
    if (start_sel) {
      selected_endpoints.append({curve_i, true});
    }
    if (end_sel) {
      selected_endpoints.append({curve_i, false});
    }
  }

  BLI_assert(selected_endpoints.size() == 2);
  if (selected_endpoints.size() != 2) {
    return false;
  }

  const CurveEndpoint &ep0 = selected_endpoints[0];
  const CurveEndpoint &ep1 = selected_endpoints[1];

  if (!paintcurve_geometry_merge_curve_endpoints(geom,
                                                 ep0.curve_index,
                                                 ep0.is_start,
                                                 ep1.curve_index,
                                                 ep1.is_start))
  {
    return false;
  }

  UNUSED_VARS(pc);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Handle Type
 * \{ */

static wmOperatorStatus paintcurve_handle_type_set_exec(bContext *C, wmOperator *op)
{
  Brush *brush = nullptr;
  PaintCurve *pc = paintcurve_from_context(C, &brush);
  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return OPERATOR_CANCELLED;
  }

  const ed::curves::SetHandleType dst_type = ed::curves::SetHandleType(
      RNA_enum_get(op->ptr, "type"));

  ED_paintcurve_undo_push_begin(C, op->type->name);

  MutableSpan<int8_t> handle_types_left = geom.handle_types_left_for_write();
  MutableSpan<int8_t> handle_types_right = geom.handle_types_right_for_write();

  for (const int point_i : geom.points_range()) {
    const uint8_t sel = paintcurve_geom_get_selection(geom, point_i);
    if (sel == 0) {
      continue;
    }
    if ((sel & 0x01) || (sel & 0x02)) {
      handle_types_left[point_i] = paintcurve_resolve_handle_type(handle_types_left[point_i],
                                                                  dst_type);
    }
    if ((sel & 0x04) || (sel & 0x02)) {
      handle_types_right[point_i] = paintcurve_resolve_handle_type(handle_types_right[point_i],
                                                                   dst_type);
    }
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_topology_changed();

  ED_paintcurve_undo_push_end(C);
  paintcurve_finish_topology_edit(C, pc, brush);
  return OPERATOR_FINISHED;
}

static bool paintcurve_handle_type_set_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }
  PaintCurve *pc = paintcurve_from_context(C);
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return false;
  }
  return paintcurve_any_point_selected(pc->geometry.wrap());
}

void PAINTCURVE_OT_handle_type_set(wmOperatorType *ot)
{
  ot->name = "Set Paint Curve Handle Type";
  ot->description = "Set the handle type for selected paint curve control points";
  ot->idname = "PAINTCURVE_OT_handle_type_set";

  ot->invoke = WM_menu_invoke;
  ot->exec = paintcurve_handle_type_set_exec;
  ot->poll = paintcurve_handle_type_set_poll;

  ot->flag = OPTYPE_REGISTER;

  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          ed::curves::rna_enum_set_handle_type_items,
                          int(ed::curves::SetHandleType::Auto),
                          "Type",
                          nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Split / Make Segment
 * \{ */

static wmOperatorStatus paintcurve_split_exec(bContext *C, wmOperator *op)
{
  Brush *brush = nullptr;
  PaintCurve *pc = paintcurve_from_context(C, &brush);
  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);
  int selected_curve = -1;
  if (!paintcurve_geometry_split_selected_points(geom, op->reports, &selected_curve)) {
    ED_paintcurve_undo_push_end(C);
    return OPERATOR_CANCELLED;
  }
  if (selected_curve >= 0) {
    pc->active_curve = selected_curve;
  }
  ED_paintcurve_undo_push_end(C);
  paintcurve_finish_topology_edit(C, pc, brush);
  return OPERATOR_FINISHED;
}

static bool paintcurve_split_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }
  PaintCurve *pc = paintcurve_from_context(C);
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return false;
  }
  return paintcurve_any_point_selected(pc->geometry.wrap());
}

void PAINTCURVE_OT_split(wmOperatorType *ot)
{
  ot->name = "Split Paint Curve";
  ot->description = "Split selected control points, disconnecting the splines";
  ot->idname = "PAINTCURVE_OT_split";

  ot->exec = paintcurve_split_exec;
  ot->poll = paintcurve_split_poll;

  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus paintcurve_make_segment_exec(bContext *C, wmOperator *op)
{
  Brush *brush = nullptr;
  PaintCurve *pc = paintcurve_from_context(C, &brush);
  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);
  if (!paintcurve_geometry_make_segment(pc, geom, op->reports)) {
    ED_paintcurve_undo_push_end(C);
    return OPERATOR_CANCELLED;
  }
  ED_paintcurve_undo_push_end(C);
  paintcurve_finish_topology_edit(C, pc, brush);
  return OPERATOR_FINISHED;
}

static bool paintcurve_make_segment_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }
  PaintCurve *pc = paintcurve_from_context(C);
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return false;
  }
  return paintcurve_make_segment_selection_valid(pc->geometry.wrap());
}

void PAINTCURVE_OT_make_segment(wmOperatorType *ot)
{
  ot->name = "Make Paint Curve Segment";
  ot->description =
      "Join two splines by connecting the selected endpoint on each (matches Curve Make Segment)";
  ot->idname = "PAINTCURVE_OT_make_segment";

  ot->exec = paintcurve_make_segment_exec;
  ot->poll = paintcurve_make_segment_poll;

  ot->flag = OPTYPE_REGISTER;
}

/* -------------------------------------------------------------------- */
/** \name Select Linked
 * \{ */

static wmOperatorStatus paintcurve_select_linked_exec(bContext *C, wmOperator *op)
{
  Brush *brush = nullptr;
  PaintCurve *pc = paintcurve_from_context(C, &brush);
  if (!pc) {
    return OPERATOR_CANCELLED;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (!paintcurve_geometry_is_valid(geom)) {
    return OPERATOR_CANCELLED;
  }

  ED_paintcurve_undo_push_begin(C, op->type->name);
  paintcurve_geometry_select_linked(geom);
  ED_paintcurve_undo_push_end(C);
  paintcurve_finish_topology_edit(C, pc, brush);
  return OPERATOR_FINISHED;
}

static bool paintcurve_select_linked_poll(bContext *C)
{
  if (!paint_curve_poll(C)) {
    return false;
  }
  PaintCurve *pc = paintcurve_from_context(C);
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return false;
  }
  return paintcurve_geometry_any_point_selected(pc->geometry.wrap());
}

void PAINTCURVE_OT_select_linked(wmOperatorType *ot)
{
  ot->name = "Select Linked Paint Curve Points";
  ot->description = "Select all control points on splines with a selected point";
  ot->idname = "PAINTCURVE_OT_select_linked";

  ot->exec = paintcurve_select_linked_exec;
  ot->poll = paintcurve_select_linked_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context Menu
 * \{ */

static bool paintcurve_context_menu_point_hit(bContext *C,
                                              const int mval[2],
                                              int *r_point_index)
{
  PaintCurve *pc = paintcurve_from_context(C);
  if (pc == nullptr || !paintcurve_geometry_is_valid(pc->geometry.wrap())) {
    return false;
  }

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  Vector<PaintCurvePoint> screen_points;
  paintcurve_build_screen_points(pc, &vc, screen_points);

  const float mval_fl[2] = {float(mval[0]), float(mval[1])};
  char selflag = 0;
  const int hit = paintcurve_find_in_screen_points(screen_points.as_span(),
                                                   mval_fl,
                                                   false,
                                                   PAINT_CURVE_POINT_SELECT_THRESHOLD,
                                                   &selflag);
  if (hit < 0) {
    return false;
  }

  if ((paintcurve_geom_get_selection(pc->geometry.wrap(), hit) & 0x07) == 0) {
    return false;
  }

  *r_point_index = hit;
  UNUSED_VARS(selflag);
  return true;
}

static wmOperatorStatus paintcurve_context_menu_invoke(bContext *C,
                                                       wmOperator * /*op*/,
                                                       const wmEvent *event)
{
  int point_index = -1;
  if (!paintcurve_context_menu_point_hit(C, event->mval, &point_index)) {
    return OPERATOR_PASS_THROUGH;
  }

  ui::PopupMenu *pup = ui::popup_menu_begin(C, IFACE_("Paint Curve"), ICON_NONE);
  ui::Layout &layout = *ui::popup_menu_layout(pup);
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);
  layout.op_menu_enum(
      C, "PAINTCURVE_OT_handle_type_set", "type", IFACE_("Handle Type"), ICON_NONE);
  layout.op("PAINTCURVE_OT_select_linked", std::nullopt, ICON_NONE);
  layout.separator();
  layout.op("PAINTCURVE_OT_duplicate", std::nullopt, ICON_NONE);
  layout.separator();
  layout.op("PAINTCURVE_OT_split", std::nullopt, ICON_NONE);
  layout.op("PAINTCURVE_OT_make_segment", std::nullopt, ICON_NONE);
  layout.separator();
  layout.op("PAINTCURVE_OT_delete_point", std::nullopt, ICON_NONE);
  ui::popup_menu_end(C, pup);

  return OPERATOR_INTERFACE;
}

void PAINTCURVE_OT_context_menu(wmOperatorType *ot)
{
  ot->name = "Paint Curve Context Menu";
  ot->description = "Context menu for selected paint curve control points";
  ot->idname = "PAINTCURVE_OT_context_menu";

  ot->invoke = paintcurve_context_menu_invoke;
  ot->poll = paint_curve_poll;
}

/** \} */

}  // namespace blender
