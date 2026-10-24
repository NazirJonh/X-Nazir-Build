/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_gradient_gizmo.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

namespace blender::ed::sculpt_paint::gradient_gizmo {

/** Segment count of the handle discs. */
static constexpr int HANDLE_SEGMENTS = 16;

float2 midpoint_position(const float2 &start_px, const float2 &end_px, const float midpoint)
{
  return math::interpolate(start_px, end_px, midpoint);
}

int hit_test(const float2 &start_px,
             const float2 &end_px,
             const float midpoint,
             const float2 &mval,
             const float hit_radius)
{
  /* Nearest marker wins, with each marker's drawn size as its hit radius. Picking the midpoint
   * first, as this used to, made the start handle unreachable on a short gradient: the midpoint
   * marker sits on top of it and every press near the start moved the midpoint instead. */
  struct Handle {
    int id;
    float2 point;
    float radius;
  };
  const Handle handles[3] = {
      {START, start_px, hit_radius},
      {END, end_px, hit_radius},
      {MID, midpoint_position(start_px, end_px, midpoint), hit_radius * 0.75f},
  };
  int best = NONE;
  float best_dist = hit_radius;
  for (const Handle &handle : handles) {
    const float dist = math::distance(handle.point, mval);
    if (dist <= handle.radius && dist <= best_dist) {
      best = handle.id;
      best_dist = dist;
    }
  }
  return best;
}

void draw(const float2 &start_px, const float2 &end_px, const float midpoint)
{
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  /* The handles float over the scene; a depth test would be meaningless in pixel space. */
  const GPUDepthTest depth_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_NONE);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  /* This fork has no 2D uniform-color builtin; the 3D one with z = 0 in pixel space is the same
   * pattern the curve gradient's overlay uses. */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Dark under-stroke then light core, matching the curve gradient's overlay. */
  const float2 mid = midpoint_position(start_px, end_px, midpoint);
  for (const int pass : IndexRange(2)) {
    GPU_line_width((pass == 0) ? 3.0f : 1.5f);
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.55f);
    if (pass == 1) {
      immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    }
    immBegin(GPU_PRIM_LINE_STRIP, 3);
    immVertex3f(pos, start_px.x, start_px.y, 0.0f);
    immVertex3f(pos, mid.x, mid.y, 0.0f);
    immVertex3f(pos, end_px.x, end_px.y, 0.0f);
    immEnd();
  }

  /* Handles: filled discs with a contrasting outline. */
  const float radius = HANDLE_RADIUS;
  immUniformColor4f(0.9f, 0.9f, 0.9f, 0.9f);
  imm_draw_circle_fill_3d(pos, start_px.x, start_px.y, radius * 0.6f, HANDLE_SEGMENTS);
  imm_draw_circle_fill_3d(pos, end_px.x, end_px.y, radius * 0.6f, HANDLE_SEGMENTS);
  imm_draw_circle_fill_3d(pos, mid.x, mid.y, radius * 0.45f, HANDLE_SEGMENTS);
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.8f);
  GPU_line_width(1.0f);
  for (const float2 &center : {start_px, end_px, mid}) {
    imm_draw_circle_wire_3d(pos, center.x, center.y, radius * 0.6f, HANDLE_SEGMENTS);
  }

  immUnbindProgram();
  GPU_depth_test(depth_prev);
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

}  // namespace blender::ed::sculpt_paint::gradient_gizmo
