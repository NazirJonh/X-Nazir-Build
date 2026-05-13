/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Draw vertex paint operation for Curves.
 * Applies brush color to curve points under the brush.
 */

#include "BLI_math_vector.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"

#include "DNA_brush_types.h"

#include "WM_api.hh"

#include "DEG_depsgraph.hh"

#include "curves_vertex_paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Draw Vertex Paint Operation
 *
 * Paints color values directly onto curve points under the brush.
 * \{ */

class DrawVertexPaintOperation : public CurvesVertexPaintOperationBase {
 public:
  DrawVertexPaintOperation(const BrushStrokeMode stroke_mode)
  {
    this->stroke_mode = stroke_mode;
  }

  void on_stroke_begin(const bContext &C, const StrokeExtension &start_extension) override
  {
    /* Call base class implementation. */
    CurvesVertexPaintOperationBase::on_stroke_begin(C, start_extension);

    /* Get the invert mode of the brush. */
    invert_brush = (brush && (brush->flag & BRUSH_DIR_IN)) != 0;
    if (stroke_mode == BrushStrokeMode::Invert) {
      invert_brush = !invert_brush;
    }
  }

  void on_stroke_extended(const bContext &C, const StrokeExtension &stroke_extension) override
  {
    /* Update brush settings for this stroke sample. */
    get_brush_settings(C, stroke_extension);

    if (!object || !curves_id || !curves || !brush) {
      return;
    }

    /* Update mouse input and bounding box. */
    get_mouse_input(stroke_extension);

    if (!curves || curves->is_empty()) {
      return;
    }

    /* Sample points under the brush. */
    sample_curves_3d_brush(C, stroke_extension);

    /* Apply draw operation to all points under the brush. */
    for (const CurvesBrushPoint &point : points_in_brush) {
      apply_draw_color(point);
    }

    /* Notify about geometry changes. */
    DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, object);
    WM_event_add_notifier(&C, NC_GEOM | ND_DATA, curves_id);
  }

  void on_stroke_done(const bContext &C) override
  {
    /* Call base class implementation. */
    CurvesVertexPaintOperationBase::on_stroke_done(C);
  }

 private:
  /**
   * Apply draw color to a single brush point.
   */
  void apply_draw_color(const CurvesBrushPoint &point)
  {
    if (invert_brush) {
      /* Erase color (reduce alpha). */
      ColorGeometry4f current = get_point_color(point.point_index);
      current.a -= point.influence;
      current.a = math::max(current.a, 0.0f);
      set_point_color(point.point_index, current);
    }
    else {
      /* Paint color with blending. */
      apply_color_to_point(point.point_index, brush_color, point.influence);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Factory Function
 * \{ */

std::unique_ptr<CurvesPaintStrokeOperation> new_vertex_paint_draw_operation(
    const BrushStrokeMode &stroke_mode)
{
  return std::make_unique<DrawVertexPaintOperation>(stroke_mode);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
