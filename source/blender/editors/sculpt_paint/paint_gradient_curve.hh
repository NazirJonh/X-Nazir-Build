/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Resampled, smoothed polyline powering the Curve gradient modes.
 *
 * Shared by the Sculpt Color Gradient tool (`SCULPT_GRADIENT_CURVE`, curve in world space) and
 * the Image Editor selection gradient (`IMAGE_PAINT_GRADIENT_CURVE`, curve in the canvas pixel
 * space of the tile it was drawn on). The raw input points come from the user's drag; #build
 * removes duplicates, resamples the polyline at an even arc-length step, applies fixed-endpoint
 * Laplacian smoothing and accumulates arc lengths, so evaluation only ever deals with a clean,
 * evenly spaced polyline.
 *
 * Two parameter modes (#eGradientCurveMode) are supported, mirroring the Linear / Radial
 * behavior of the other gradient types:
 * - Along: `t = s / L` where `s` is the arc-length position of the closest point on the curve.
 *   Points projecting past either end are extrapolated along the end segment's tangent, so
 *   `t` is unbounded outside `[0, 1]` and the caller's repeat / reflect / clamp logic applies,
 *   exactly like the Linear gradient's unbounded parameter. An optional width limits the
 *   influence to a fixed distance around the curve (outside it the parameter is NaN, meaning
 *   "leave untouched", the same contract as #GradientEvaluator::t_at).
 * - Across: `t = distance / width_eff`, the distance to the curve as a fraction of the ramp
 *   width, exactly like Radial's `distance / radius`. The parameter is unbounded: past the width
 *   the caller's repeat / reflect / clamp logic paints the ramp's end color or repeated bands,
 *   so the fill covers the whole canvas instead of stopping at a hard edge. A zero width means
 *   "automatic" here (a fixed fraction of the curve's length, see #effective_across_width),
 *   unlike Along where zero means "no limit".
 *
 * Evaluation is `const` and thread-safe (nearest-point queries run through a balanced KD-tree
 * over the resampled points), so it can be called from the parallel pixel / vertex loops of the
 * apply passes.
 *
 * The implementation lives in `paint_gradient_curve.cc`; this header only declares the shared
 * interface (with `extern template` so the two instantiations are not rebuilt by every including
 * translation unit).
 */

#include <limits>
#include <memory>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_sys_types.h"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

namespace blender {

template<typename CoordT> struct KDTree;

}  // namespace blender

namespace blender::ed::sculpt_paint::gradient_curve {

/** Smoothing passes applied by #build when the caller does not request its own. */
inline constexpr int default_smooth_iterations = 4;
inline constexpr int max_smooth_iterations = 50;

/** Upper bound on the resampled point count, protecting both the KD-tree build and the
 * per-pixel / per-vertex evaluation cost of very long strokes. */
inline constexpr int max_points = 2048;

/** Ramer-Douglas-Peucker tolerance (in the space the points are measured in) and the point cap
 * used by #simplify_control_points for the editable control points left after a stroke. */
inline constexpr float simplify_tolerance_px = 4.0f;
inline constexpr int max_control_points = 32;

/** Fraction of the curve's arc length used as the automatic Across ramp width when the stored
 * width is zero, so the Across mode always lays out a visible ramp. */
inline constexpr float across_auto_width_factor = 0.25f;

/** Closest-point query result on the polyline. */
struct CurveProjection {
  /**
   * Arc-length position of the closest point. Negative or greater than #GradientCurve::length()
   * when the query point projects beyond an end (extrapolated along that end segment's tangent).
   */
  float s = 0.0f;
  /** Euclidean distance to the polyline (the segment parameter is always clamped to [0, 1]). */
  float distance = std::numeric_limits<float>::max();
};

/** Named build parameters, so adding more of them (spline resolution, simplification) does not
 * silently reinterpret a positional call. */
struct GradientCurveBuildParams {
  /**
   * Target arc-length step of the resampled polyline; non-positive picks an automatic step that
   * keeps the point count at a reasonable size for the stroke's extent and length.
   */
  float spacing = 0.0f;
  /** Laplacian smoothing strength in [0, 1] (values outside are clamped). */
  float smooth = 0.0f;
  /** Smoothing passes; clamped to [0, #max_smooth_iterations]. */
  int smooth_iterations = default_smooth_iterations;
};

/**
 * Map a closest-point projection to the gradient parameter for the given mode, or NaN when it
 * must not influence anything (outside \a width for Along with a width limit; a non-positive
 * effective width for Across). Shared by #GradientCurve::eval_t and the Sculpt evaluator, which
 * resolves the projection itself (symmetry / geodesic distance).
 */
float eval_t_from_projection(eGradientCurveMode mode,
                             float width,
                             float length,
                             const CurveProjection &projection);

/**
 * Effective ramp width for the Across mode: \a width when positive, otherwise
 * \a length * #across_auto_width_factor. Zero means "automatic" for Across (not "nothing"), so
 * switching to Across without touching the width still lays out a visible ramp.
 */
float effective_across_width(float width, float length);

/** Frees a KD-tree allocated with #kdtree_new. Defined in the `.cc` so this header does not pull
 * in `BLI_kdtree.hh`. */
template<typename T> struct KDTreeDeleter {
  void operator()(KDTree<T> *tree) const;
};

template<typename T> class GradientCurve {
 public:
  GradientCurve();
  ~GradientCurve();
  GradientCurve(const GradientCurve &) = delete;
  GradientCurve &operator=(const GradientCurve &) = delete;
  GradientCurve(GradientCurve &&) noexcept;
  GradientCurve &operator=(GradientCurve &&) noexcept;

  /**
   * Build from raw input points: drop duplicates and too-close points, resample the polyline at
   * an even arc-length step (never more than #max_points points), Laplacian-smooth it with the
   * endpoints pinned, and accumulate arc lengths. Returns #is_valid; a previously built curve is
   * always replaced (an invalid rebuild empties the curve).
   */
  bool build(Span<T> raw_points, const GradientCurveBuildParams &params = {});

  /** At least two distinct points and a non-zero total length. */
  bool is_valid() const
  {
    return points_.size() >= 2 && length_ > min_length_;
  }

  /** Total arc length, 0 for an invalid curve. */
  float length() const
  {
    return length_;
  }

  int points_num() const
  {
    return int(points_.size());
  }

  /** The resampled polyline; the first and last points are the smoothed stroke ends. */
  Span<T> points() const
  {
    return points_;
  }

  /** Cumulative arc length at each point of #points (same size). */
  Span<float> arc_lengths() const
  {
    return arc_;
  }

  /** Point at arc-length position \a s, clamped to the two ends. */
  T position_at_s(float s) const;

  /** Axis-aligned bounds of #points; an invalid curve returns empty bounds. */
  Bounds<T> bounds() const;

  /** Closest point on the polyline (see #CurveProjection). Thread-safe. */
  CurveProjection project(const T &pos) const;

  /**
   * Gradient parameter at \a pos for the given mode, or NaN when \a pos must not be touched:
   * outside \a width for Along with a width limit, never for Across on a valid curve (its
   * parameter is unbounded and the caller clamps / repeats it; see #effective_across_width for
   * the zero-width fallback), and always for an invalid curve.
   */
  float eval_t(eGradientCurveMode mode, float width, const T &pos) const;

 private:
  void rebuild_arc_lengths();
  void rebuild_kdtree();

  Vector<T> points_;
  Array<float> arc_;
  float length_ = 0.0f;
  std::unique_ptr<KDTree<T>, KDTreeDeleter<T>> kdtree_;

  /** Total length below this is treated as degenerate. */
  static constexpr float min_length_ = 1e-9f;
};

/**
 * Indices of the points to keep: Ramer-Douglas-Peucker at \a tolerance, doubling the tolerance
 * until at most \a max_num remain (uniform thinning as the last resort). The first and last
 * points are always kept.
 */
template<typename T>
Vector<int> simplify_control_points(Span<T> points, float tolerance, int max_num);

using GradientCurve2D = GradientCurve<float2>;
using GradientCurve3D = GradientCurve<float3>;

extern template class GradientCurve<float2>;
extern template class GradientCurve<float3>;
extern template Vector<int> simplify_control_points<float2>(Span<float2>, float, int);
extern template Vector<int> simplify_control_points<float3>(Span<float3>, float, int);

}  // namespace blender::ed::sculpt_paint::gradient_curve
