/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Draw vertex paint operation for Curves.
 * Applies brush color to curve points under the brush.
 *
 * The Draw brush reuses the shared stroke machinery in CurvesPaintOperationBase and only
 * overrides the per-point hook (apply_operation_to_point) with its color/erase behavior.
 */

#include "BLI_math_base.hh"

#include "DNA_brush_types.h"

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

    /* Get the invert (erase) mode of the brush. */
    invert_brush = (brush && (brush->flag & BRUSH_DIR_IN)) != 0;
    if (stroke_mode == BrushStrokeMode::Invert) {
      invert_brush = !invert_brush;
    }
  }

 protected:
  /**
   * Apply draw color to a single brush point. In invert mode the brush erases by reducing the
   * point alpha instead of blending in the brush color.
   */
  void apply_operation_to_point(const CurvesBrushPoint &point) override
  {
    if (invert_brush) {
      /* Erase color (reduce alpha). */
      ColorGeometry4f current = get_point_color(point.point_index);
      current.a = math::max(current.a - point.influence, 0.0f);
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
