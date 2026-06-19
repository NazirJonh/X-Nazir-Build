/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Preview drawing for the Extract Loop tool: the loop/ring polyline and the
 * face-strip fill/outline shown while hovering and during the modal gesture.
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

void draw_face_strip_preview(const ExtractLoopSharedData &shared,
                             BMesh *draw_bm,
                             const Span<BMFace *> faces_override)
{
  const Span<BMFace *> faces = faces_override.is_empty() ?
                                   Span<BMFace *>(shared.preview_faces) :
                                   faces_override;
  if (faces.is_empty() || !shared.obact) {
    return;
  }

  auto face_vert_co = [&](BMVert *v) -> float3 {
    if (draw_bm) {
      return float3(v->co);
    }
    return vert_position(shared, v);
  };

  GPU_blend(GPU_BLEND_ALPHA);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  GPU_matrix_push();
  GPU_matrix_mul(shared.obact->object_to_world().ptr());

  const GPUDepthTest prev_depth_test = GPU_depth_test_get();
  if (prev_depth_test != GPU_DEPTH_LESS_EQUAL) {
    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  }

  /* Semi-transparent fill so the strip reads as surface, not wire. */
  float fill_color[4] = {0.0f, 0.8f, 1.0f, 0.25f};
  immUniformColor4fv(fill_color);
  for (BMFace *face : faces) {
    Vector<float3, 16> face_verts;
    BMLoop *l;
    BMIter l_iter;
    BM_ITER_ELEM (l, &l_iter, face, BM_LOOPS_OF_FACE) {
      face_verts.append(face_vert_co(l->v));
    }
    const int face_verts_num = face_verts.size();
    if (face_verts_num < 3) {
      continue;
    }
    immBegin(GPU_PRIM_TRIS, (face_verts_num - 2) * 3);
    for (int i = 1; i < face_verts_num - 1; i++) {
      immVertex3fv(pos, face_verts[0]);
      immVertex3fv(pos, face_verts[i]);
      immVertex3fv(pos, face_verts[i + 1]);
    }
    immEnd();
  }

  /* Face polygon loops — outline of each face that will be extracted. */
  float line_color[4] = {0.0f, 0.8f, 1.0f, 0.9f};
  immUniformColor4fv(line_color);
  GPU_line_width(3.0f);
  for (BMFace *face : faces) {
    Vector<float3, 16> face_verts;
    BMLoop *l;
    BMIter l_iter;
    BM_ITER_ELEM (l, &l_iter, face, BM_LOOPS_OF_FACE) {
      face_verts.append(face_vert_co(l->v));
    }
    if (face_verts.size() < 3) {
      continue;
    }
    immBegin(GPU_PRIM_LINE_LOOP, face_verts.size());
    for (const float3 &p : face_verts) {
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

void draw_loop_preview(const ExtractLoopSharedData &shared)
{
  if (shared.mode == ExtractionMode::FaceStrip) {
    draw_face_strip_preview(shared);
    return;
  }

  if (shared.preview_points.is_empty() || !shared.obact) {
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
  GPU_matrix_mul(shared.obact->object_to_world().ptr());

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
