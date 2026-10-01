/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared GPU overlay; see #paint_shape_edit.hh.
 */

#include "paint_shape_edit.hh"

#include <cmath>

#include "BLI_math_vector.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "DNA_userdef_types.h" /* UI_SCALE_FAC */

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Dashed outline
 * \{ */

void shape_draw_dashed_outline(Span<float2> region_points, const bool loop)
{
  if (region_points.size() < 2) {
    return;
  }
  /* A non-finite point (a degenerate mapping) would draw a spurious segment from the region
   * corner; drop the whole outline instead. */
  for (const float2 &co : region_points) {
    if (!std::isfinite(co.x) || !std::isfinite(co.y)) {
      return;
    }
  }
  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR_ANIMATED);
  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2] / UI_SCALE_FAC, viewport_size[3] / UI_SCALE_FAC);
  immUniform1i("colors_len", 2);
  immUniform4f("color", 0.4f, 0.4f, 0.4f, 1.0f);
  immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
  immUniform1f("dash_width", 8.0f);
  immUniform1f("udash_factor", 0.5f);
  immUniform1f("dash_phase", float(fmod(BLI_time_now_seconds(), 1.0)));
  GPU_line_width(1.0f);
  immBegin(loop ? GPU_PRIM_LINE_LOOP : GPU_PRIM_LINE_STRIP, int(region_points.size()));
  for (const float2 &co : region_points) {
    immVertex2f(pos, co.x, co.y);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Plain outline
 * \{ */

void shape_draw_plain_outline(Span<float2> region_points, const bool loop)
{
  if (region_points.size() < 2) {
    return;
  }
  for (const float2 &co : region_points) {
    if (!std::isfinite(co.x) || !std::isfinite(co.y)) {
      return;
    }
  }
  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4f(0.5f, 0.5f, 0.5f, 0.35f);
  GPU_line_width(1.0f);
  immBegin(loop ? GPU_PRIM_LINE_LOOP : GPU_PRIM_LINE_STRIP, int(region_points.size()));
  for (const float2 &co : region_points) {
    immVertex2f(pos, co.x, co.y);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Control points
 * \{ */

/** One point square as a triangle fan around \a center. */
static void point_square_draw(const uint pos, const float2 &center, const float half)
{
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, center.x - half, center.y - half);
  immVertex2f(pos, center.x + half, center.y - half);
  immVertex2f(pos, center.x + half, center.y + half);
  immVertex2f(pos, center.x - half, center.y + half);
  immEnd();
}

void shape_draw_control_points(Span<float2> region_cos)
{
  if (region_cos.is_empty()) {
    return;
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* The dark border then the light core, all points per batch (the Vector session's per-point
   * pairs; squares of one point never overlap another's, so the batching is visual-identical). */
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
  for (const float2 &co : region_cos) {
    point_square_draw(pos, co, 4.5f);
  }
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.95f);
  for (const float2 &co : region_cos) {
    point_square_draw(pos, co, 3.0f);
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Origin markers
 * \{ */

/** One upright diamond as a triangle fan around \a center. */
static void origin_diamond_draw(const uint pos, const float2 &center, const float radius)
{
  immBegin(GPU_PRIM_TRI_FAN, 4);
  immVertex2f(pos, center.x, center.y - radius);
  immVertex2f(pos, center.x + radius, center.y);
  immVertex2f(pos, center.x, center.y + radius);
  immVertex2f(pos, center.x - radius, center.y);
  immEnd();
}

void shape_draw_origin_markers(Span<float2> region_cos)
{
  if (region_cos.is_empty()) {
    return;
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Dark diamond outline, then the orange core: distinct from the white control-point squares
   * so the pivot is readable at a glance. */
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.85f);
  for (const float2 &co : region_cos) {
    origin_diamond_draw(pos, co, 6.0f);
  }
  immUniformColor4f(1.0f, 0.55f, 0.1f, 1.0f);
  for (const float2 &co : region_cos) {
    origin_diamond_draw(pos, co, 4.0f);
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session overlay
 * \{ */

/** Flattening error (shape pixels) of the live session outline. */
constexpr float SHAPE_OVERLAY_ERROR_PX = 2.0f;

void shape_draw_session_overlay(
    Span<VectorItem> items,
    const int active_index,
    FunctionRef<float2(const PaintShape &, const float2 &)> to_region)
{
  for (const VectorItem &item : items) {
    for (const ShapePolyline &poly : shape_flatten(item.shape, SHAPE_OVERLAY_ERROR_PX)) {
      Vector<float2> region_points(poly.points.size());
      for (const int i : poly.points.index_range()) {
        region_points[i] = to_region(item.shape, poly.points[i]);
      }
      shape_draw_dashed_outline(region_points, poly.cyclic);
    }
  }

  if (active_index < 0 || active_index >= items.size()) {
    return;
  }
  const PaintShape &active = items[active_index].shape;

  Vector<float2> region_cos;
  for (const ShapeSpline &spline : active.splines) {
    for (const ShapePoint &point : spline.points) {
      region_cos.append(to_region(active, point.co));
    }
  }
  shape_draw_control_points(region_cos);

  float2 origin_region = to_region(active, shape_effective_origin(active));
  shape_draw_origin_markers(Span<float2>(&origin_region, 1));
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
