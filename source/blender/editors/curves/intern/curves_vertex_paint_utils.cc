/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcurves
 *
 * Vertex paint specific utilities and base class implementation for Curves Vertex Paint operations.
 */

#include "curves_vertex_paint_intern.hh"

#include "BLI_color_mix.hh"
#include "BLI_math_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"

#include "DNA_brush_types.h"
#include "DNA_curves_types.h"
#include "DNA_object_types.h"

#include "IMB_imbuf.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DEG_depsgraph.hh"

namespace blender::ed::sculpt_paint {

static constexpr const char *vertex_color_attr_name = "vertex_color";

/* -------------------------------------------------------------------- */
/** \name Poll Functions
 * \{ */

bool curves_vertex_paint_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_CURVES) {
    return false;
  }
  return ob->mode == OB_MODE_VERTEX_CURVES;
}

bool curves_vertex_paint_mode_poll(bContext *C)
{
  return curves_vertex_paint_poll(C);
}

void curves_vertex_paint_ensure_color_attribute(Object *ob)
{
  if (!ob || ob->type != OB_CURVES || ob->data == nullptr) {
    return;
  }

  Curves *curves_id = reinterpret_cast<Curves *>(ob->data);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  /* Keep the existing colors when the attribute is already present. */
  if (attributes.contains(vertex_color_attr_name)) {
    return;
  }

  /* Create the attribute initialized to opaque white, matching mesh vertex paint, so that the
   * curves are visible in vertex paint mode before anything is painted. */
  bke::SpanAttributeWriter<ColorGeometry4f> writer =
      attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(vertex_color_attr_name,
                                                                    bke::AttrDomain::Point);
  if (writer) {
    writer.span.fill(ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f));
    writer.finish();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Color Access
 * \{ */

ColorGeometry4f CurvesVertexPaintOperationBase::get_point_color(const int point_index)
{
  if (vertex_colors_writer_) {
    return vertex_colors_writer_->span[point_index];
  }
  /* Writer is not active (e.g. called from on_stroke_begin before the first extended step).
   * Fall back to reading the attribute directly from the geometry. */
  if (!curves) {
    return ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f);
  }
  const VArray<ColorGeometry4f> colors =
      *curves->attributes().lookup_or_default<ColorGeometry4f>(
          ATTR_VERTEX_COLOR, bke::AttrDomain::Point, ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f));
  return colors[point_index];
}

void CurvesVertexPaintOperationBase::set_point_color(const int point_index,
                                                     const ColorGeometry4f &color)
{
  if (vertex_colors_writer_) {
    vertex_colors_writer_->span[point_index] = color;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Color Blending
 * \{ */

ColorPaint4f CurvesVertexPaintOperationBase::blend_colors(const ColorPaint4f &src,
                                                          const ColorPaint4f &dst,
                                                          const float influence)
{
  if (influence <= 0.0f) {
    return dst;
  }

  using Color = ColorPaint4f;
  using Traits = color::Traits<Color>;

  return color::BLI_mix_colors<Color, Traits>(
      blend_mode, dst, src, Traits::range * influence);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Paint Operation Override
 * \{ */

void CurvesVertexPaintOperationBase::apply_color_to_point(const int point_index,
                                                        const ColorPaint4f &color,
                                                        const float influence)
{
  if (!vertex_colors_writer_) {
    return;
  }

  ColorGeometry4f &dst = vertex_colors_writer_->span[point_index];
  const ColorPaint4f dst_paint = color::unpremultiply_alpha(dst);
  const ColorPaint4f result = blend_colors(color, dst_paint, influence);
  dst = color::premultiply_alpha(result);
}

void CurvesVertexPaintOperationBase::apply_operation_to_point(const CurvesBrushPoint &point)
{
  apply_color_to_point(point.point_index, brush_color, point.influence);
}

void CurvesVertexPaintOperationBase::init_paint_mode(const bContext & /*C*/)
{
  /* Ensure the vertex_color attribute exists so it can be opened per-step in on_stroke_extended.
   * We do NOT keep a writer open here because SpanAttributeWriter must not be alive across
   * depsgraph evaluations (which happen between stroke steps). */
  if (object) {
    curves_vertex_paint_ensure_color_attribute(object);
  }
}

void CurvesVertexPaintOperationBase::finalize_paint_mode(const bContext & /*C*/)
{
  /* The writer is opened and closed within each on_stroke_extended call, so nothing to do here. */
  vertex_colors_writer_.reset();
}

void CurvesVertexPaintOperationBase::on_stroke_extended(
    const bContext &C, const StrokeExtension &stroke_extension)
{
  if (!curves) {
    CurvesPaintOperationBase::on_stroke_extended(C, stroke_extension);
    return;
  }

  /* Open the attribute writer for this stroke step. */
  bke::MutableAttributeAccessor attributes = curves->attributes_for_write();
  vertex_colors_writer_ = attributes.lookup_or_add_for_write_span<ColorGeometry4f>(
      ATTR_VERTEX_COLOR, bke::AttrDomain::Point);

  /* Run the shared brush logic (sample points, apply_brush, tag depsgraph, send notifier). */
  CurvesPaintOperationBase::on_stroke_extended(C, stroke_extension);

  /* Commit the changes so the depsgraph evaluation sees the new colors immediately. */
  if (vertex_colors_writer_) {
    vertex_colors_writer_->finish();
    vertex_colors_writer_.reset();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CurvesVertexPaintOperationBase - Stroke Callbacks
 * \{ */

void CurvesVertexPaintOperationBase::on_stroke_begin(const bContext &C,
                                                     const StrokeExtension &start_extension)
{
  CurvesPaintOperationBase::on_stroke_begin(C, start_extension);

  if (!object || !brush) {
    return;
  }

  Paint *paint = BKE_paint_get_active_from_context(&C);
  if (paint == nullptr) {
    return;
  }

  float color_linear[3];
  copy_v3_v3(color_linear, BKE_brush_color_get(paint, brush));
  brush_color = ColorPaint4f(color_linear[0], color_linear[1], color_linear[2], 1.0f);
  blend_mode = IMB_BlendMode(brush->blend);
}

void CurvesVertexPaintOperationBase::on_stroke_done(const bContext &C)
{
  CurvesPaintOperationBase::on_stroke_done(C);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
