/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Vertex paint specific utilities and base class implementation for Curves Vertex Paint operations.
 */

#include "curves_vertex_paint_intern.hh"

#include "BLI_math_vector.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"

#include "DNA_brush_types.h"

#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DEG_depsgraph.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Color Access
 * \{ */

ColorGeometry4f CurvesVertexPaintOperationBase::get_point_color(int point_index)
{
  /* Return white as default color. */
  return ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void CurvesVertexPaintOperationBase::set_point_color(int point_index,
                                                      const ColorGeometry4f &color)
{
  /* Stub implementation. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Color Blending
 * \{ */

ColorPaint4f CurvesVertexPaintOperationBase::blend_colors(const ColorPaint4f &src,
                                                          const ColorPaint4f &dst,
                                                          float influence)
{
  if (influence <= 0.0f) {
    return dst;
  }
  if (influence >= 1.0f) {
    return src;
  }

  /* Simple linear interpolation. */
  ColorPaint4f result;
  result.r = dst.r + (src.r - dst.r) * influence;
  result.g = dst.g + (src.g - dst.g) * influence;
  result.b = dst.b + (src.b - dst.b) * influence;
  result.a = dst.a + (src.a - dst.a) * influence;

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Paint Operation Override
 * \{ */

void CurvesVertexPaintOperationBase::apply_color_to_point(int point_index,
                                                          const ColorPaint4f &color,
                                                          float influence)
{
  /* Stub implementation. */
}

void CurvesVertexPaintOperationBase::apply_operation_to_point(const CurvesBrushPoint &point)
{
  /* Default vertex paint behavior: apply brush color to points. */
  apply_color_to_point(point.point_index, brush_color, point.influence);
}

void CurvesVertexPaintOperationBase::init_paint_mode(const bContext &C)
{
  /* Stub implementation. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Stroke Callbacks
 * \{ */

void CurvesVertexPaintOperationBase::on_stroke_begin(const bContext &C,
                                                      const StrokeExtension &start_extension)
{
  /* Initialize paint operation. */
  if (!object || !curves_id || !curves || !brush) {
    return;
  }

  /* Get brush color from context. */
  Paint *paint = BKE_paint_get_active_from_context(&C);
  if (paint == nullptr) {
    return;
  }

  float color_linear[3];
  copy_v3_v3(color_linear, BKE_brush_color_get(paint, brush));
  brush_color = ColorPaint4f(color_linear[0], color_linear[1], color_linear[2], 1.0f);

  /* Get blend mode from brush. */
  blend_mode = static_cast<IMB_BlendMode>(brush->blend);
}

void CurvesVertexPaintOperationBase::on_stroke_extended(const bContext &C,
                                                         const StrokeExtension &stroke_extension)
{
  /* Extend paint stroke. */
}

void CurvesVertexPaintOperationBase::on_stroke_done(const bContext &C)
{
  /* Finish paint stroke. */
}

/** \} */

}  // namespace blender::ed::sculpt_paint
