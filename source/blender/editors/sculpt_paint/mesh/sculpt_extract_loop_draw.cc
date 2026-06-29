/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Preview drawing for the Extract Loop tool: the loop/ring polyline shown while
 * hovering and during the modal gesture. The face-strip fill/outline is drawn by
 * the shared #extract::draw_faces_preview engine.
 */

#include "BKE_object_types.hh"

#include "DNA_object_types.h"

#include "BLI_math_vector.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "bmesh.hh"

#include "sculpt_extract_loop_intern.hh"

namespace blender::ed::sculpt_paint::extract_loop {

void draw_loop_preview(const ExtractLoopSharedData &shared)
{
  if (shared.mode == ExtractionMode::FaceStrip) {
    extract::draw_faces_preview(shared.base);
    return;
  }

  if (shared.preview_points.is_empty() || !shared.base.obact) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(3.0f);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  float color[4] = {0.0f, 0.8f, 1.0f, 0.9f};
  immUniformColor4fv(color);

  GPU_matrix_push();
  GPU_matrix_mul(shared.base.obact->object_to_world().ptr());

  const GPUDepthTest prev_depth_test = GPU_depth_test_get();
  if (prev_depth_test != GPU_DEPTH_LESS_EQUAL) {
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  }

  if (shared.mode == ExtractionMode::Loop) {
    immBegin(shared.is_cyclic ? GPU_PRIM_LINE_LOOP : GPU_PRIM_LINE_STRIP,
             shared.preview_points.size());
    for (const float3 &p : shared.preview_points) {
      immVertex3fv(pos, p);
    }
    immEnd();
  }
  else {
    immBegin(GPU_PRIM_LINES, shared.preview_points.size());
    for (const float3 &p : shared.preview_points) {
      immVertex3fv(pos, p);
    }
    immEnd();
  }

  if (prev_depth_test != GPU_DEPTH_LESS_EQUAL) {
    GPU_depth_test(prev_depth_test);
  }

  immUnbindProgram();
  GPU_matrix_pop();

  GPU_blend(GPU_BLEND_NONE);
}

}  // namespace blender::ed::sculpt_paint::extract_loop
