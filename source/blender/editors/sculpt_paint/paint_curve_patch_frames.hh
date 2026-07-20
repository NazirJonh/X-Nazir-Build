/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Local tangent-plane windows along the Curve Patch control curve.
 *
 * A single frozen projection plane cannot cover a curve that runs over a sharp edge: past the edge
 * the surface turns away from the plane, the relief stretches and then breaks off entirely. The
 * curve is therefore cut into overlapping windows, each with its own projection plane taken from
 * the dominant face of that stretch.
 *
 * The role of the normal splits in two. The SMOOTHED field
 * (#CurvePatchSpline::normals_smooth_3d) supplies the binormals of ONE ribbon spanning the whole
 * curve, which is what keeps the across-strip coordinate `u` continuous and independent of where
 * the window boundaries happen to fall. The SHARP field (#CurvePatchSpline::normals_3d) picks the
 * window planes that ribbon is projected into for rasterization.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "paint_curve_patch_ribbon.hh"

namespace blender::ed::sculpt_paint {

struct CurvePatchSpline;

/** Ceiling on the number of windows. On reaching it the tail of the curve folds into the last
 * window and #CurvePatchFrameSet::capped reports it -- silently degrading quality is not
 * acceptable. */
constexpr int CURVE_PATCH_MAX_FRAMES = 32;
/** Total LUT pixel budget across all windows (~32 MB at 32 bytes per pixel). Capping the window
 * count alone is not enough: `res` is chosen adaptively from the extent, and a short slice does not
 * shrink it proportionally. */
constexpr int64_t CURVE_PATCH_MAX_LUT_PIXELS = 1024 * 1024;

/** One window along the curve: a range of `poly_3d` indices and the SHARP normal of its dominant
 * face.
 *
 * Sharp rather than averaged over the samples: across an edge the average lands at 45 degrees --
 * the worst projection plane for both faces at once. Only the ribbon's binormals need smoothness,
 * and a separate smoothed field (#CurvePatchSpline::normals_smooth_3d) provides it. */
struct CurvePatchFrameRange {
  /** First `poly_3d` index in the window, inclusive. */
  int begin = 0;
  /** Last `poly_3d` index in the window, inclusive. */
  int end = 0;
  float3 normal = float3(0.0f, 0.0f, 1.0f);
};

struct CurvePatchFramesParams {
  /** Usually `2 * frozen_params.radius`. */
  float min_window_length = 0.0f;
  /** Threshold for a gradual turn; only splits once `min_window_length` is covered. */
  float turn_threshold_rad = 0.0f;
  /** A break: tears the window open REGARDLESS of `min_window_length`. */
  float break_threshold_rad = 0.0f;
  int max_frames = CURVE_PATCH_MAX_FRAMES;
};

/**
 * Cuts the polyline into overlapping windows. Returns false when there are no normals or the input
 * is degenerate -- the caller then builds a single window over the whole curve. `r_capped` reports
 * that `max_frames` was reached and the tail was folded into the last window.
 */
bool curve_patch_frames_partition(const CurvePatchSpline &spline,
                                  const CurvePatchFramesParams &params,
                                  Vector<CurvePatchFrameRange> &r_ranges,
                                  bool &r_capped);

/** One window's rasterized ribbon, plus what is needed to place its `(u, s)` back into the curve's
 * global coordinates. */
struct CurvePatchFrame {
  CurvePatchRibbonLut lut;
  /** Projection plane of this window (see #CurvePatchFrameRange::normal). */
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  /** The local LUT numbers arc length from zero at its own slice; adding this offset puts `s` back
   * into the curve's global arc length, which is the same for every window. That is precisely why
   * a change of winning window does not shift the texture along the length. */
  float s_offset = 0.0f;
  /** Global arc length at the window's midpoint; the tie-break when several windows cover a
   * vertex. */
  float s_center = 0.0f;
  float3 bb_min = float3(0.0f);
  float3 bb_max = float3(0.0f);
};

struct CurvePatchFrameSet {
  Vector<CurvePatchFrame> frames;
  bool ready = false;
  /** The window count or the LUT pixel budget was hit and wrap quality was reduced. */
  bool capped = false;

  /**
   * Up to two global `(u, s)` branches for `co`.
   *
   * Only the ORIENTATION culling happens in here: the depth culling needs `falloff_radius_at_s` and
   * is applied by the caller per branch, after the relief has been evaluated -- moving it in here
   * would change how many branches survive to the merge by max `|height|`.
   *
   * `r_frame_normal` reports the plane of the window that served each branch. Without it the caller
   * would measure `normal_dist` against the frozen global plane, and the depth cut-off would reject
   * exactly the vertices past the edge that this whole mechanism exists to reach.
   */
  int sample(const float3 &co,
             const float3 &vertex_normal,
             float2 r_uv[2],
             float3 r_frame_normal[2]) const;
  void clear();
};

/**
 * Partitions `spline` into windows and rasterizes one ribbon LUT per window.
 *
 * The ribbon is conceptually ONE strip over the whole curve: each window's binormals are taken from
 * the GLOBAL smoothed normal field and `u` is normalized by the half-width, so the pieces coincide
 * with the matching stretches of a single continuous strip.
 *
 * `end_margin` is applied only where a window borders a REAL end of the curve; interior joins are
 * never extended, since that would push the strip outside the window serving it.
 *
 * `r_frames.frames` is reused between re-stamps rather than rebuilt, so each LUT keeps its own
 * `source_hash` cache.
 */
void curve_patch_frames_build(const CurvePatchSpline &spline,
                              float brush_radius,
                              const CurvePatchFramesParams &params,
                              bool high_quality,
                              float end_margin,
                              CurvePatchFrameSet &r_frames);

}  // namespace blender::ed::sculpt_paint
