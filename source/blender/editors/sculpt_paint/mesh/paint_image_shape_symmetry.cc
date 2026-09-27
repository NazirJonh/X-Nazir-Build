/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image-side implementation of the shape drawing tools' canvas-space symmetry and the UDIM
 * tile <-> UV <-> reference-tile-pixel coordinate mapping; see #mesh/paint_image_shape.hh.
 */

#include <algorithm>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "BKE_image.hh"

#include "ED_image_paint_symmetry.hh"

#include "paint_image_shape.hh"
#include "../paint_shape.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Coordinate helpers
 * \{ */

float2 tile_uv_origin(const int tile)
{
  return BKE_image_get_tile_uv_origin(tile);
}

float2 shape_px_to_uv(const CanvasTile &tile, const float2 &px)
{
  const float2 size = float2(tile.ref_tile_size);
  return tile_uv_origin(tile.ref_tile) + px / size;
}

float2 shape_uv_to_px(const CanvasTile &tile, const float2 &uv)
{
  const float2 size = float2(tile.ref_tile_size);
  return (uv - tile_uv_origin(tile.ref_tile)) * size;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas symmetry
 * \{ */

Vector<PaintShape> shape_apply_canvas_symmetry(const PaintShape &shape,
                                               const CanvasTile &tile,
                                               const ToolSettings &tool_settings)
{
  using namespace blender::ed::image_paint_symmetry;

  Vector<PaintShape> result;
  if (shape.is_empty()) {
    return result;
  }
  result.append(shape);

  const std::optional<CanvasSymmetry> symmetry = from_settings(
      tool_settings, IMAGE_PAINT_SYMMETRY_LINE_AFFECT_BRUSH);
  if (!symmetry) {
    return result;
  }

  /* Dense outlines of the original in UV space; every copy maps them pointwise, which also
   * handles the non-affine circle inversion (a parametric shape bends into a curve). */
  const Vector<ShapePolyline> outlines = shape_flatten(shape, 1.0f);
  Array<Vector<float2>> uv_outlines(outlines.size());
  for (const int i : outlines.index_range()) {
    uv_outlines[i].reserve(outlines[i].points.size());
    for (const float2 &co : outlines[i].points) {
      uv_outlines[i].append(shape_px_to_uv(tile, co));
    }
  }

  for (const int copy : IndexRange(symmetry->copies_num())) {
    PaintShape mapped = shape;
    mapped.splines.clear();
    for (const int i : outlines.index_range()) {
      ShapeSpline spline;
      spline.is_bezier = false;
      spline.cyclic = outlines[i].cyclic;
      const int point_count = outlines[i].points.size();
      for (const int j : IndexRange(point_count)) {
        const float2 &uv = uv_outlines[i][j];
        ShapePoint point;
        point.co = shape_uv_to_px(tile, symmetry->apply(copy, uv));
        /* The stroke width follows the mapping's local scale (the circle inversion is not
         * isometric); clamped so extreme copies stay paintable. */
        point.width_factor = std::clamp(
            outlines[i].width[j] * symmetry->scale_at(uv), 0.01f, 100.0f);
        spline.points.append(point);
      }
      if (spline.points.size() >= 2) {
        mapped.splines.append(std::move(spline));
      }
    }
    if (!mapped.splines.is_empty()) {
      result.append(std::move(mapped));
    }
  }

  return result;
}

Vector<PaintShape> shapes_expand_symmetry(const Span<PaintShape> shapes,
                                          const CanvasTile &tile,
                                          const ToolSettings &tool_settings)
{
  Vector<PaintShape> result;
  for (const PaintShape &shape : shapes) {
    result.extend(shape_apply_canvas_symmetry(shape, tile, tool_settings));
  }
  return result;
}
/** \} */

}  // namespace blender::ed::sculpt_paint::shape
