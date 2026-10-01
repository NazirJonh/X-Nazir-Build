/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared GPU overlay; see #paint_shape_draw.hh.
 */

#include "paint_shape_draw.hh"

#include <cmath>

#include "BLI_math_vector.hh"
#include "BLI_time.h"

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

}  // namespace blender::ed::sculpt_paint::shape
