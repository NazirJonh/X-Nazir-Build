/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Region algorithms of the Magic Wand / Quick Selection tools, on raw per-pixel buffers so they
 * are unit-testable without a running editor:
 *
 * - a bucket-queue **minimax (bottleneck) cost map**: the wand computes once which minimal
 *   tolerance would reach every pixel from the seed, so changing the tolerance afterwards is a
 *   linear threshold instead of a new flood fill (and equals a classic flood fill for the same
 *   tolerance);
 * - a **geodesic (additive) cost map** in a bounded window for the Quick Selection spread;
 * - the **UV-island map** of one UDIM tile (the "UV Borders" barrier shared by the tools);
 * - post-processing of the soft mask: soft threshold, barrier-aware Gaussian, hole filling,
 *   Euclidean grow/shrink (Felzenszwalb distance transform), bilinear resampling between tile
 *   resolutions, and the selection-operation combine.
 *
 * Indexing convention: row-major buffers of `size.x * size.y` entries; the algorithms work in
 * buffer-local coordinates (0..size-1), the callers translate to tile pixels.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"

#include "ED_select_utils.hh"

namespace blender::ed::sculpt_paint::image_select {

/* -------------------------------------------------------------------- */
/** \name UV-island map
 *
 * UV-island map of one UDIM tile: the island id of every pixel (or -1 outside UV), shared by
 * the Magic Wand and Quick Selection as their "UV Borders" barrier, and reusable by other
 * selection/paint features that must not cross a UV seam or leave the charts.
 *
 * The ids come from UV connectivity (the same flood-fill rules as UV island selection, seams
 * included) over every canvas object of the image; each object's faces are rasterized into the
 * tile with the watertight pixel-center rule of #foreach_face_pixel (see
 * #uv_island_map_build in paint_image_select_source.hh).
 *
 * Overlapping UVs (mirrored charts, stacked islands): a pixel written by a second island keeps
 * its first id and gets the overlap flag; every consumer must treat an overlap pixel as
 * passable from *any* of the islands that claimed it (the conservative choice — a seam between
 * stacked charts is not a color boundary the tools should respect).
 * \{ */

struct UVIslandMap {
  int2 size = int2(0, 0);
  int tile_number = 1001;
  /** Island id per pixel, -1 = no UV here (empty area / gutter). */
  Array<int32_t> island;
  /** Per-pixel flag: more than one island claims this pixel. */
  Array<uint8_t> overlap;
  int32_t island_num = 0;

  bool is_valid() const
  {
    return size.x > 0 && size.y > 0;
  }
  int64_t index(const int x, const int y) const
  {
    return int64_t(y) * size.x + x;
  }
  /** Island id at \a px, or -1 outside the tile. */
  int32_t island_at(const int2 &px) const
  {
    if (px.x < 0 || px.y < 0 || px.x >= size.x || px.y >= size.y) {
      return -1;
    }
    return island[index(px.x, px.y)];
  }
  bool overlap_at(const int2 &px) const
  {
    if (px.x < 0 || px.y < 0 || px.x >= size.x || px.y >= size.y) {
      return false;
    }
    return overlap[index(px.x, px.y)] != 0;
  }
  /**
   * The barrier test of the flood algorithms: the step \a from -> \a to is allowed when both
   * pixels belong to the same island, or either pixel is flagged as overlapped, or both are
   * outside UV (the empty area floods freely; only island borders are walls).
   */
  bool passable(const int2 &from, const int2 &to) const;
};

/** Grow island ids into neighboring -1 pixels by \a margin_px layers (BFS). */
void uv_island_map_dilate(UVIslandMap &map, int margin_px);

/** \} */

/** Quantization levels of a 0..1 distance for the bucket queue. */
constexpr int COST_LEVELS = 1024;
/** Cost-map value of pixels the flood never reached (or that are behind a barrier). */
constexpr uint16_t COST_UNREACHABLE = 0xFFFF;

/**
 * Minimax (bottleneck) cost map from \a seed: `cost(p) = min over paths seed->p of (max over
 * path pixels of quantized dist)`. Pixels with `dist > max_tolerance` are walls, so the cost
 * map of a plain click covers only the reachable neighborhood; pass 1.0 to build the whole
 * image (the drag-tolerance mode).
 *
 * \param dist_at: quantized 0..1 distance of a buffer index; evaluated lazily, only for pixels
 * the flood visits.
 * \param passable: optional barrier callback (buffer indices); with a UV-island map, a step is
 * allowed only inside one island. Distances are quantized to #COST_LEVELS levels.
 */
void minimax_cost_map(blender::FunctionRef<float(int64_t)> dist_at,
                      int2 size,
                      int2 seed,
                      bool connect_8,
                      float max_tolerance,
                      blender::FunctionRef<bool(int2, int2)> passable,
                      MutableSpan<uint16_t> r_cost);

/** Soft threshold of a cost map: 1 at cost <= tolerance - softness, 0 at cost >= tolerance +
 * softness, linear in between (sub-pixel anti-aliased edge). The seed pixel is forced to 1. */
void soft_threshold(Span<uint16_t> cost,
                    int2 size,
                    int2 seed,
                    float tolerance,
                    float softness,
                    MutableSpan<float> r_mask);

/** Non-contiguous selection: every pixel with dist <= tolerance, in parallel. */
void threshold_global(Span<float> dist, float tolerance, MutableSpan<float> r_mask);

/**
 * Additive geodesic cost map from \a seed (Quick Selection spread): `cost(p) = min over paths
 * of sum(step_cost)`, grown until the budget is exhausted. Costs beyond \a budget stay
 * unreachable. #step_cost is the cost of entering a pixel (>= 0), evaluated only for visited
 * pixels. The windowed caller passes a small window; a binary heap is fine at that size.
 */
void geodesic_cost_map(blender::FunctionRef<float(int2)> step_cost,
                       int2 size,
                       int2 seed,
                       bool connect_8,
                       float budget,
                       blender::FunctionRef<bool(int2, int2)> passable,
                       MutableSpan<float> r_cost);

/** |sobel| of \a data (dims interleaved per pixel), max over components, normalized so the 95th
 * percentile of the tile is 1. Border pixels copy their nearest interior neighbor. */
void sobel_edge_map(Span<float> data, int dims, int2 size, MutableSpan<float> r_edge);

/** Exact Euclidean distance from every pixel to the nearest pixel where \a binary is true
 * (Felzenszwalb two-pass squared transform). */
void distance_transform_true(Span<uint8_t> binary, int2 size, MutableSpan<float> r_distance);

/** Normalized separable Gaussian; with a barrier, weights of steps crossing an island border
 * are zero and the kernel renormalizes, so the blur never leaks across a seam. */
void gaussian_blur_barrier(
    Span<float> src, int2 size, float sigma, const UVIslandMap *barrier, MutableSpan<float> r_dst);

/** Fill unselected connected components smaller than \a min_area that do not touch the tile
 * border (4-connectivity on the binary part of the mask). */
void mask_fill_holes(MutableSpan<float> mask, int2 size, int min_area);

/** Grow the selection by \a grow_px: pixels within that distance of a selected pixel join it. */
void mask_grow(MutableSpan<float> mask, int2 size, int grow_px);

/** Shrink the selection by \a shrink_px: selected pixels within that distance of an unselected
 * pixel are dropped. */
void mask_shrink(MutableSpan<float> mask, int2 size, int shrink_px);

/** Bilinear resample of a soft mask between tile resolutions (UV-aligned: pixel centers map
 * through the normalized tile coordinates). */
void mask_resample(Span<float> src, int2 src_size, MutableSpan<float> r_dst, int2 dst_size);

/** Combine \a src into \a dst by the selection operation (SET replaces, ADD maxes, SUB removes,
 * AND intersects, XOR differs). */
void mask_combine(MutableSpan<float> dst, Span<float> src, eSelectOp sel_op);

}  // namespace blender::ed::sculpt_paint::image_select
