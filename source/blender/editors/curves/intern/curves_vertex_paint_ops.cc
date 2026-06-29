/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Additional vertex paint operations for Curves (Blur, Average, Smear, Replace).
 */

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DNA_brush_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_array.hh"
#include "BLI_color_mix.hh"
#include "BLI_index_mask.hh"
#include "BLI_linear_allocator.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_message.hh"
#include "WM_types.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_curves.hh"
#include "ED_object.hh"
#include "ED_paint.hh"
#include "ED_image.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "MEM_guardedalloc.h"

#include "../sculpt_paint/paint_intern.hh"
#include "curves_vertex_paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Blur Vertex Paint Operation
 *
 * Blurs/smooths color values by averaging nearby points.
 * \{ */

class BlurVertexPaintOperation : public CurvesVertexPaintOperationBase {
 public:
  BlurVertexPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

 protected:
  void apply_brush(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/) override
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    const OffsetIndices<int> points_by_curve = curves->points_by_curve();
    const Array<int> point_to_curve = curves->point_to_curve_map();
    const VArray<bool> cyclic = curves->cyclic();

    /* Compute all target colors first, then write them, so that the blur of one point does not
     * feed back into the blur of an adjacent point in the same pass. */
    Array<ColorGeometry4f> new_colors(points_in_brush.size());

    for (const int i : points_in_brush.index_range()) {
      const CurvesBrushPoint &point = points_in_brush[i];
      const int point_index = point.point_index;
      const ColorGeometry4f old_color = get_point_color(point_index);
      new_colors[i] = old_color;

      float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
      int neighbor_count = 0;
      foreach_curve_neighbors(
          point_index, points_by_curve, point_to_curve, cyclic, [&](const int nb) {
            const ColorGeometry4f c = get_point_color(nb);
            r += c.r;
            g += c.g;
            b += c.b;
            a += c.a;
            neighbor_count++;
          });
      if (neighbor_count == 0) {
        continue;
      }

      const float inv_count = 1.0f / float(neighbor_count);
      const float t = point.influence;
      new_colors[i] = ColorGeometry4f(math::interpolate(old_color.r, r * inv_count, t),
                                      math::interpolate(old_color.g, g * inv_count, t),
                                      math::interpolate(old_color.b, b * inv_count, t),
                                      math::interpolate(old_color.a, a * inv_count, t));
    }

    for (const int i : points_in_brush.index_range()) {
      set_point_color(points_in_brush[i].point_index, new_colors[i]);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Average Vertex Paint Operation
 *
 * Averages color values of all points under the brush.
 * \{ */

class AverageVertexPaintOperation : public CurvesVertexPaintOperationBase {
 public:
  AverageVertexPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

 protected:
  void apply_brush(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/) override
  {
    if (points_in_brush.is_empty()) {
      return;
    }

    /* Calculate the influence-weighted average color of all points under the brush. */
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float influence_sum = 0.0f;
    for (const CurvesBrushPoint &point : points_in_brush) {
      const ColorGeometry4f color = get_point_color(point.point_index);
      r += color.r * point.influence;
      g += color.g * point.influence;
      b += color.b * point.influence;
      a += color.a * point.influence;
      influence_sum += point.influence;
    }

    if (influence_sum < 1e-6f) {
      return;
    }

    const float inv = 1.0f / influence_sum;
    const ColorGeometry4f average(r * inv, g * inv, b * inv, a * inv);

    /* Blend each point towards the average. The brush strength is already folded into the point
     * influence during sampling. */
    for (const CurvesBrushPoint &point : points_in_brush) {
      const ColorGeometry4f old_color = get_point_color(point.point_index);
      const float t = point.influence;
      set_point_color(point.point_index,
                      ColorGeometry4f(math::interpolate(old_color.r, average.r, t),
                                      math::interpolate(old_color.g, average.g, t),
                                      math::interpolate(old_color.b, average.b, t),
                                      math::interpolate(old_color.a, average.a, t)));
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smear Vertex Paint Operation
 *
 * Smears color values in the direction of brush movement.
 * \{ */

class SmearVertexPaintOperation : public CurvesVertexPaintOperationBase {
 private:
  /* Cached colors from the previous stroke sample for smearing. */
  Array<ColorGeometry4f> previous_colors_;
  bool has_previous_sample_ = false;

 public:
  SmearVertexPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override
  {
    CurvesVertexPaintOperationBase::on_stroke_begin(C, start_extension);

    /* Seed the previous-sample colors with the actual point colors so that points entering the
     * brush later are not smeared towards black. */
    if (curves) {
      const int points_num = curves->points_num();
      previous_colors_.reinitialize(points_num);
      for (const int point_i : IndexRange(points_num)) {
        previous_colors_[point_i] = get_point_color(point_i);
      }
    }
    has_previous_sample_ = false;
  }

  void on_stroke_done(const bContext &C) override
  {
    CurvesVertexPaintOperationBase::on_stroke_done(C);
    previous_colors_ = Array<ColorGeometry4f>();
    has_previous_sample_ = false;
  }

 protected:
  void apply_brush(const bContext & /*C*/, const StrokeExtension & /*stroke_extension*/) override
  {
    /* Make sure the cache matches the current geometry (guards against a failed stroke begin). */
    if (previous_colors_.size() != curves->points_num()) {
      previous_colors_.reinitialize(curves->points_num());
      previous_colors_.fill(ColorGeometry4f());
      has_previous_sample_ = false;
    }

    if (!points_in_brush.is_empty() && has_previous_sample_) {
      apply_smear();
    }

    /* Cache the resulting colors for the next stroke sample. */
    for (const CurvesBrushPoint &point : points_in_brush) {
      previous_colors_[point.point_index] = get_point_color(point.point_index);
    }
    has_previous_sample_ = true;
  }

 private:
  void apply_smear()
  {
    /* Smear strength is proportional to how far the brush moved this sample. */
    const float2 brush_direction = mouse_position - mouse_position_previous;
    const float brush_movement = math::length(brush_direction);
    if (brush_movement < 1e-6f || brush_radius < 1e-6f) {
      return;
    }
    const float smear_factor = math::min(1.0f, brush_movement / brush_radius);

    for (const CurvesBrushPoint &point : points_in_brush) {
      const ColorGeometry4f current_color = get_point_color(point.point_index);
      const ColorGeometry4f prev_color = previous_colors_[point.point_index];

      /* Drag the previous-sample color towards the current point, scaled by brush movement and
       * the point influence (which already includes the brush strength). */
      const ColorGeometry4f smeared(
          math::interpolate(current_color.r, prev_color.r, smear_factor),
          math::interpolate(current_color.g, prev_color.g, smear_factor),
          math::interpolate(current_color.b, prev_color.b, smear_factor),
          math::interpolate(current_color.a, prev_color.a, smear_factor));

      const float t = point.influence;
      set_point_color(point.point_index,
                      ColorGeometry4f(math::interpolate(current_color.r, smeared.r, t),
                                      math::interpolate(current_color.g, smeared.g, t),
                                      math::interpolate(current_color.b, smeared.b, t),
                                      math::interpolate(current_color.a, smeared.a, t)));
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Replace Vertex Paint Operation
 *
 * Replaces colors with brush color regardless of previous color.
 * \{ */

class ReplaceVertexPaintOperation : public CurvesVertexPaintOperationBase {
 public:
  ReplaceVertexPaintOperation()
  {
    this->stroke_mode = BrushStrokeMode::Normal;
  }

 protected:
  void apply_operation_to_point(const CurvesBrushPoint &point) override
  {
    /* Replace the RGB channels towards the brush color, keeping the existing alpha. */
    ColorGeometry4f current = get_point_color(point.point_index);
    current.r = math::interpolate(current.r, brush_color.r, point.influence);
    current.g = math::interpolate(current.g, brush_color.g, point.influence);
    current.b = math::interpolate(current.b, brush_color.b, point.influence);
    set_point_color(point.point_index, current);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Factory Functions
 * \{ */

std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_blur_operation()
{
  return std::make_unique<BlurVertexPaintOperation>();
}

std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_average_operation()
{
  return std::make_unique<AverageVertexPaintOperation>();
}

std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_smear_operation()
{
  return std::make_unique<SmearVertexPaintOperation>();
}

std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_replace_operation()
{
  return std::make_unique<ReplaceVertexPaintOperation>();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Paint Mode Functions
 * \{ */

static void curves_vertex_paint_mode_enter(bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);
  Object *ob = CTX_data_active_object(C);

  /* Ensure vertex paint data exists */
  BKE_paint_ensure(scene->toolsettings, (Paint **)&scene->toolsettings->curves_vertex_paint);
  CurvesVertexPaint *curves_vertex_paint = scene->toolsettings->curves_vertex_paint;

  /* Set object mode */
  ob->mode = OB_MODE_VERTEX_CURVES;

  /* Set paint mode */
  Paint *paint = BKE_paint_get_active_from_paintmode(scene, PaintMode::VertexCurves);

  /* Ensure brushes exist */
  BKE_paint_brushes_ensure(CTX_data_main(C), paint);

  curves_vertex_paint_ensure_color_attribute(ob);

  /* Start paint cursor */
  ED_paint_cursor_start(&curves_vertex_paint->paint, curves_vertex_paint_poll);

  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
}

static void curves_vertex_paint_mode_exit(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);

  /* Set object mode back to object */
  ob->mode = OB_MODE_OBJECT;

  /* Update dependency graph and notify */
  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex Paint Mode Toggle Operator
 * \{ */

static bool curves_vertex_paint_toggle_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->type == OB_CURVES && ob->data;
}

static wmOperatorStatus curves_vertex_paint_toggle_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  wmMsgBus *mbus = CTX_wm_message_bus(C);

  const bool is_mode_set = ob->mode == OB_MODE_VERTEX_CURVES;

  if (!is_mode_set) {
    if (!blender::ed::object::mode_compat_set(C, ob, OB_MODE_VERTEX_CURVES, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  if (is_mode_set) {
    curves_vertex_paint_mode_exit(C);
  }
  else {
    curves_vertex_paint_mode_enter(C);
  }

  WM_toolsystem_update_from_context_view3d(C);

  DEG_id_tag_update(&ob->id, ID_RECALC_SYNC_TO_EVAL);
  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);
  WM_event_add_notifier(C, NC_SCENE | ND_MODE, nullptr);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_paint_toggle(wmOperatorType *ot)
{
  ot->name = "Curves Vertex Paint Mode";
  ot->idname = "CURVES_OT_vertex_paint_toggle";
  ot->description = "Toggle curves vertex paint mode in 3D view";

  ot->exec = curves_vertex_paint_toggle_exec;
  ot->poll = curves_vertex_paint_toggle_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sample Vertex Color Operator
 * \{ */

static wmOperatorStatus curves_vertex_paint_sample_invoke(bContext *C,
                                                          wmOperator * /*op*/,
                                                          const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  if (vc.rv3d == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (!vc.obact || vc.obact->type != OB_CURVES || vc.obact->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Object *object_orig = reinterpret_cast<const Object *>(DEG_get_original_id(&vc.obact->id));
  if (object_orig == nullptr || object_orig->type != OB_CURVES || object_orig->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Object *object_eval = DEG_get_evaluated(vc.depsgraph, object_orig);
  if (object_eval == nullptr || object_eval->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Curves *curves_id = id_cast<const Curves *>(object_orig->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  if (curves.points_num() == 0) {
    return OPERATOR_CANCELLED;
  }

  const bke::crazyspace::GeometryDeformation deformation =
      bke::crazyspace::get_evaluated_curves_deformation(object_eval, *object_orig);
  const Span<float3> deformed_positions = deformation.positions.is_empty() ? curves.positions() :
                                                                             deformation.positions;
  const float4x4 projection = ED_view3d_ob_project_mat_get(vc.rv3d, object_eval);
  const IndexMask points_mask(curves.points_num());

  const std::optional<ed::curves::FindClosestData> closest =
      ed::curves::closest_elem_find_screen_space(vc,
                                                 curves.points_by_curve(),
                                                 deformed_positions,
                                                 curves.cyclic(),
                                                 projection,
                                                 points_mask,
                                                 bke::AttrDomain::Point,
                                                 event->mval,
                                                 {});
  if (!closest) {
    return OPERATOR_CANCELLED;
  }

  const VArray<ColorGeometry4f> colors = *curves.attributes().lookup_or_default<ColorGeometry4f>(
      "vertex_color", bke::AttrDomain::Point, ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f));
  const ColorPaint4f sampled = color::unpremultiply_alpha(colors[closest->index]);

  Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return OPERATOR_CANCELLED;
  }
  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  BKE_brush_color_set(paint, brush, float3(sampled.r, sampled.g, sampled.b));
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_paint_sample(wmOperatorType *ot)
{
  ot->name = "Sample Vertex Color";
  ot->idname = "CURVES_OT_vertex_paint_sample";
  ot->description = "Set the active brush color to the color of the vertex under the cursor";

  ot->poll = curves_vertex_paint_mode_poll;
  ot->invoke = curves_vertex_paint_sample_invoke;

  ot->flag = OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Vertex Colors Operator
 * \{ */

static wmOperatorStatus curves_vertex_color_set_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (ID *original_id = DEG_get_original_id(&ob->id)) {
    ob = reinterpret_cast<Object *>(original_id);
  }
  if (ob->type != OB_CURVES || ob->data == nullptr) {
    return OPERATOR_CANCELLED;
  }

  Curves *curves_id = id_cast<Curves *>(ob->data);
  if (curves_id == nullptr) {
    return OPERATOR_CANCELLED;
  }
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  if (curves.points_num() == 0) {
    return OPERATOR_CANCELLED;
  }

  curves_vertex_paint_ensure_color_attribute(ob);

  /* Resolve the brush color (linear, straight alpha) and store it premultiplied. */
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  const float3 brush_color_linear = brush ? BKE_brush_color_get(paint, brush) : float3(1.0f);
  const ColorGeometry4f color = color::premultiply_alpha(ColorPaint4f(
      brush_color_linear[0], brush_color_linear[1], brush_color_linear[2], 1.0f));

  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
  bke::SpanAttributeWriter<ColorGeometry4f> writer =
      attributes.lookup_or_add_for_write_span<ColorGeometry4f>("vertex_color",
                                                               bke::AttrDomain::Point);
  if (!writer) {
    return OPERATOR_CANCELLED;
  }

  /* Apply to the selected points, or to all points when nothing is selected. */
  LinearAllocator<> memory;
  const IndexMask selected_points = ed::curves::retrieve_selected_points(*curves_id, memory);
  const IndexMask mask = selected_points.is_empty() ? IndexMask(curves.points_num()) :
                                                      selected_points;
  mask.foreach_index([&](const int point_i) { writer.span[point_i] = color; });
  writer.finish();

  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, curves_id);

  return OPERATOR_FINISHED;
}

void CURVES_OT_vertex_color_set(wmOperatorType *ot)
{
  ot->name = "Set Vertex Colors";
  ot->idname = "CURVES_OT_vertex_color_set";
  ot->description = "Set the vertex color of the selected points (or all points) to the brush color";

  ot->poll = curves_vertex_paint_mode_poll;
  ot->exec = curves_vertex_color_set_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Stroke Operators
 * \{ */

static std::unique_ptr<CurvesPaintStrokeOperation> start_stroke_operation_vertex_paint(
    const BrushStrokeMode brush_mode, const BrushSwitchMode brush_switch_mode, const bContext &C)
{
  const Object *object = CTX_data_active_object(&C);
  if (!object || object->type != OB_CURVES) {
    return nullptr;
  }

  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (!brush) {
    return nullptr;
  }

  if (brush_switch_mode == BrushSwitchMode::Smooth) {
    return new_vertex_paint_blur_operation();
  }

  switch (eBrushVertexPaintType(brush->vertex_brush_type)) {
    case VPAINT_BRUSH_TYPE_DRAW:
      return new_vertex_paint_draw_operation(brush_mode);
    case VPAINT_BRUSH_TYPE_BLUR:
      return new_vertex_paint_blur_operation();
    case VPAINT_BRUSH_TYPE_AVERAGE:
      return new_vertex_paint_average_operation();
    case VPAINT_BRUSH_TYPE_SMEAR:
      return new_vertex_paint_smear_operation();
    default:
      break;
  }

  return nullptr;
}

struct CurvesVertexPaintBrushStroke final : public PaintStroke {
  CurvesVertexPaintBrushStroke(bContext *C, wmOperator *op, const int event_type)
      : PaintStroke(C, op, event_type)
  {
  }

  bool get_location(float out[3], const float mouse[2], bool /*force_original*/) override
  {
    out[0] = mouse[0];
    out[1] = mouse[1];
    out[2] = 0.0f;
    return true;
  }

  bool test_start(wmOperator * /*op*/, const float /*mouse*/[2]) override
  {
    return true;
  }

  void update_step(wmOperator *op, PointerRNA *stroke_element) override
  {
    StrokeExtension stroke_extension;
    RNA_float_get_array(stroke_element, "mouse", stroke_extension.mouse_position);
    stroke_extension.pressure = RNA_float_get(stroke_element, "pressure");
    stroke_extension.reports = op->reports;

    if (!operation_) {
      stroke_extension.is_first = true;
      operation_ = start_stroke_operation_vertex_paint(
          BrushStrokeMode(RNA_enum_get(op->ptr, "mode")),
          BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle")),
          *this->evil_C);
      if (!operation_) {
        return;
      }
      operation_->on_stroke_begin(*this->evil_C, stroke_extension);
    }
    else {
      stroke_extension.is_first = false;
    }

    operation_->on_stroke_extended(*this->evil_C, stroke_extension);
  }

  void redraw(bool /*final*/) override {}

  bool test_cancel() override
  {
    return false;
  }

  void done(bool /*is_cancel*/, bool /*stroke_started*/) override
  {
    if (operation_) {
      operation_->on_stroke_done(*this->evil_C);
    }
  }

 private:
  std::unique_ptr<CurvesPaintStrokeOperation> operation_;
};

static bool curves_vertex_paint_brush_stroke_poll(bContext *C)
{
  const bool mode_ok = (CTX_data_active_object(C) &&
                        CTX_data_active_object(C)->type == OB_CURVES &&
                        CTX_data_active_object(C)->mode == OB_MODE_VERTEX_CURVES);
  const bool tool_ok = WM_toolsystem_active_tool_is_brush(C);

  return mode_ok && tool_ok;
}

static wmOperatorStatus curves_vertex_paint_brush_stroke_invoke(bContext *C,
                                                                wmOperator *op,
                                                                const wmEvent *event)
{
  const Object *object = CTX_data_active_object(C);
  if (!object || object->type != OB_CURVES) {
    return OPERATOR_CANCELLED;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = BKE_paint_brush_for_read(paint);
  if (brush == nullptr) {
    return OPERATOR_CANCELLED;
  }

  CurvesVertexPaintBrushStroke *stroke = MEM_new<CurvesVertexPaintBrushStroke>(
      __func__, C, op, event->type);
  op->customdata = stroke;

  const wmOperatorStatus retval = op->type->modal(C, op, event);
  OPERATOR_RETVAL_CHECK(retval);

  if (retval == OPERATOR_FINISHED) {
    MEM_delete(stroke);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus curves_vertex_paint_brush_stroke_modal(bContext *C,
                                                               wmOperator *op,
                                                               const wmEvent *event)
{
  CurvesVertexPaintBrushStroke *stroke = static_cast<CurvesVertexPaintBrushStroke *>(
      op->customdata);

  if (!stroke) {
    return OPERATOR_CANCELLED;
  }

  const wmOperatorStatus retval = stroke->modal(C, op, event);
  OPERATOR_RETVAL_CHECK(retval);

  if (retval != OPERATOR_RUNNING_MODAL) {
    MEM_delete(stroke);
    op->customdata = nullptr;
  }

  return retval;
}

static void curves_vertex_paint_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata != nullptr) {
    CurvesVertexPaintBrushStroke *stroke = static_cast<CurvesVertexPaintBrushStroke *>(
        op->customdata);
    stroke->cancel(C);
    MEM_delete(stroke);
    op->customdata = nullptr;
  }
}

static void CURVES_OT_vertex_paint_brush_stroke(wmOperatorType *ot)
{
  ot->name = "Curves Vertex Paint Brush Stroke";
  ot->idname = "CURVES_OT_vertex_paint_brush_stroke";
  ot->description = "Paint vertex colors on curves points";

  ot->poll = curves_vertex_paint_brush_stroke_poll;
  ot->invoke = curves_vertex_paint_brush_stroke_invoke;
  ot->modal = curves_vertex_paint_brush_stroke_modal;
  ot->cancel = curves_vertex_paint_brush_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}

/** \} */

}  // namespace blender::ed::sculpt_paint

/* -------------------------------------------------------------------- */
/** \name Registration
 * \{ */

void ED_operatortypes_curves_vertex_paint()
{
  using namespace blender::ed::sculpt_paint;
  WM_operatortype_append(CURVES_OT_vertex_paint_toggle);
  WM_operatortype_append(CURVES_OT_vertex_paint_sample);
  WM_operatortype_append(CURVES_OT_vertex_color_set);
  WM_operatortype_append(CURVES_OT_vertex_paint_brush_stroke);
}

/** \} */
