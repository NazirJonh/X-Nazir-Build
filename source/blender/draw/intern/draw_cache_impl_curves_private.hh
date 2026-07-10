/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Private declarations for Curves batch cache
 */

#pragma once

#include "GPU_batch.hh"
#include "GPU_index_buffer.hh"
#include "GPU_vertex_buffer.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "DNA_curves_types.h"
#include "draw_curves_private.hh"

namespace blender::bke {
class CurvesGeometry;
}

namespace blender::draw {

struct CurvesBatchCache {
  CurvesEvalCache eval_cache;

  gpu::Batch *edit_points;
  gpu::Batch *edit_handles;

  gpu::Batch *sculpt_cage;
  gpu::IndexBuf *sculpt_cage_ibo;

  /* Crazy-space point positions for original points. */
  gpu::VertBuf *edit_points_pos;
  gpu::VertBuf *edit_points_rad;

  /* Additional data needed for shader to choose color for each point in edit_points_pos. */
  gpu::VertBuf *edit_points_data;

  /* Selection of original points. */
  gpu::VertBuf *edit_points_selection;

  gpu::IndexBuf *edit_handles_ibo;

  gpu::Batch *edit_curves_lines;
  gpu::VertBuf *edit_curves_lines_pos;
  gpu::IndexBuf *edit_curves_lines_ibo;

  /* Weight paint batches */
  gpu::Batch *weight_points = nullptr;
  gpu::Batch *weight_lines = nullptr;

  /* Weight paint vertex buffers */
  gpu::VertBuf *weight_points_pos = nullptr;

  /* Weight paint index buffers */
  gpu::IndexBuf *weight_points_indices = nullptr;
  gpu::IndexBuf *weight_lines_indices = nullptr;

  /* Vertex paint batches */
  gpu::Batch *vertex_paint_points = nullptr;
  gpu::Batch *vertex_paint_lines = nullptr;
  gpu::VertBuf *vertex_paint_points_pos = nullptr;
  gpu::IndexBuf *vertex_paint_points_indices = nullptr;
  gpu::IndexBuf *vertex_paint_lines_indices = nullptr;

  /* Whether the cache is invalid. */
  bool is_dirty;
};

/**
 * Gets the batch cache for a Curves object, creating it if necessary.
 */
CurvesBatchCache &get_batch_cache(Curves &curves);

/**
 * Compute a per-point tangent for every curve point, shared by the weight and vertex paint
 * overlays. \a r_tangents must have `curves.points_num()` elements.
 */
void curves_paint_compute_tangents(const bke::CurvesGeometry &curves,
                                   MutableSpan<float3> r_tangents);

/**
 * Build the point and line index buffers and their batches from an already filled vertex buffer
 * \a vbo. Both batches reference \a vbo. The line batch is only created for curves that have more
 * than one point. Shared by the weight and vertex paint overlays.
 */
void curves_paint_build_point_and_line_batches(const bke::CurvesGeometry &curves,
                                               gpu::VertBuf *vbo,
                                               gpu::IndexBuf **r_points_ibo,
                                               gpu::IndexBuf **r_lines_ibo,
                                               gpu::Batch **r_points_batch,
                                               gpu::Batch **r_lines_batch);

}  // namespace blender::draw
