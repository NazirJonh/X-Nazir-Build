/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the Curve gradient polyline declared in `paint_gradient_curve.hh`, plus its
 * two instantiations. Keeping the definitions here avoids rebuilding this code (and pulling in
 * `BLI_kdtree.hh`) in every translation unit that only evaluates a curve.
 */

#include <algorithm>
#include <array>
#include <cmath>

#include "BLI_array.hh"
#include "BLI_kdtree.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_stack.hh"

#include "BKE_curves.hh"

#include "paint_gradient_curve.hh"

namespace blender::ed::sculpt_paint::gradient_curve {

template<typename T> static bool gradient_curve_all_finite(const T &co)
{
  for (const int i : IndexRange(T::type_length)) {
    if (!std::isfinite(co[i])) {
      return false;
    }
  }
  return true;
}

template<typename T>
static float project_on_segment(const T &pos, const T &pa, const T &ab, float &r_h_raw)
{
  const float len_sq = math::length_squared(ab);
  if (len_sq <= 0.0f) {
    r_h_raw = 0.0f;
    return math::distance(pos, pa);
  }
  r_h_raw = math::dot(pos - pa, ab) / len_sq;
  const T q = pa + ab * std::clamp(r_h_raw, 0.0f, 1.0f);
  return math::distance(pos, q);
}

float effective_across_width(const float width, const float length)
{
  return (width > 0.0f) ? width : length * across_auto_width_factor;
}

float eval_t_from_projection(const eGradientCurveMode mode,
                             const float width,
                             const float length,
                             const CurveProjection &projection)
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  switch (mode) {
    case GRADIENT_CURVE_MODE_ALONG:
      /* Zero width means no influence limit; the parameter is extrapolated past the ends and the
       * caller's repeat / clamp logic handles it, like the Linear gradient. */
      if (width > 0.0f && projection.distance > width) {
        return nan;
      }
      return projection.s / length;
    case GRADIENT_CURVE_MODE_ACROSS: {
      /* Across is the distance to the curve as a fraction of the ramp width, like Radial's
       * `distance / radius`: the parameter is NOT bounded, so the caller's repeat / clamp logic
       * gives the ramp's end color (Repeat None) or repeats / reflects it past the width.
       * Returning NaN here used to cut the fill off with a hard edge. A non-positive effective
       * width (an invalid / degenerate curve) still means "leave untouched". */
      const float eff_width = effective_across_width(width, length);
      if (!(eff_width > 0.0f)) {
        return nan;
      }
      return projection.distance / eff_width;
    }
  }
  return nan;
}

template<typename T> void KDTreeDeleter<T>::operator()(KDTree<T> *tree) const
{
  if (tree != nullptr) {
    kdtree_free<T>(tree);
  }
}

template<typename T> GradientCurve<T>::GradientCurve() = default;

template<typename T> GradientCurve<T>::~GradientCurve() = default;

template<typename T> GradientCurve<T>::GradientCurve(GradientCurve &&) noexcept = default;

template<typename T>
GradientCurve<T> &GradientCurve<T>::operator=(GradientCurve &&) noexcept = default;

template<typename T>
bool GradientCurve<T>::build(const Span<T> raw_points, const GradientCurveBuildParams &params)
{
  points_.clear();
  length_ = 0.0f;

  /* Drop duplicates and points closer together than a small fraction of the stroke extent, so
   * stationary input does not crowd the polyline. Non-finite input poisons every derived value,
   * so it rejects the build outright. */
  float raw_length = 0.0f;
  Vector<T> filtered;
  for (const T &co : raw_points) {
    if (!gradient_curve_all_finite(co)) {
      return false;
    }
    if (!filtered.is_empty()) {
      const float step = math::distance(filtered.last(), co);
      if (step <= min_length_ * 10.0f) {
        continue;
      }
      raw_length += step;
    }
    filtered.append(co);
  }
  if (filtered.size() < 2) {
    return false;
  }

  /* Target arc-length step: a dense but bounded resample -- at least ~1/64 of the stroke's
   * bounding-box diagonal, never more than #max_points over the whole drawn length. Picked here
   * (rather than by each backend) so the two curve gradients share one density policy. */
  float spacing = params.spacing;
  if (!(spacing > 0.0f)) {
    T bmin = filtered.first();
    T bmax = filtered.first();
    for (const T &co : filtered) {
      for (const int i : IndexRange(T::type_length)) {
        bmin[i] = std::min(bmin[i], co[i]);
        bmax[i] = std::max(bmax[i], co[i]);
      }
    }
    spacing = std::max(math::length(bmax - bmin) / 64.0f, raw_length / float(max_points - 1));
  }
  if (!(spacing > 0.0f)) {
    spacing = raw_length / 64.0f;
  }

  /* Smooth the control points with a Catmull-Rom spline before resampling, so thinning the raw
   * samples (see #simplify_control_points) leaves a smooth curve instead of a polyline through
   * the few kept points. Catmull-Rom interpolates its control points, so the drawn start / end
   * and any deliberately placed point stay on the curve; a two-point input remains a straight
   * line. The per-segment resolution targets one evaluated point per spacing step, bounded so a
   * very long stroke cannot explode the intermediate array. */
  Array<int> evaluated_offsets(filtered.size() + 1);
  evaluated_offsets[0] = 0;
  for (const int segment : IndexRange(filtered.size() - 1)) {
    const float segment_length = math::distance(filtered[segment], filtered[segment + 1]);
    const int resolution = std::clamp(int(std::ceil(segment_length / spacing)), 1, 16);
    evaluated_offsets[segment + 1] = evaluated_offsets[segment] + resolution;
  }
  evaluated_offsets.last() = evaluated_offsets[filtered.size() - 1] + 1;
  const OffsetIndices<int> evaluated_ranges(evaluated_offsets.as_span());
  const int evaluated_num = evaluated_offsets.last();
  Vector<T> spline(evaluated_num);
  bke::curves::catmull_rom::interpolate_to_evaluated(
      GSpan(filtered.as_span()), false, evaluated_ranges, GMutableSpan(spline.as_mutable_span()));

  /* Actual arc length of the smoothed polyline (smoothing changes it slightly). */
  float poly_length = 0.0f;
  for (const int i : IndexRange(1, spline.size() - 1)) {
    poly_length += math::distance(spline[i - 1], spline[i]);
  }

  int segments = int(std::round(poly_length / spacing));
  segments = std::clamp(segments, 1, max_points - 1);
  const float step = poly_length / float(segments);

  /* Resample at an even arc-length step; the endpoints are always kept exactly. */
  Vector<T> resampled;
  resampled.reserve(segments + 1);
  resampled.append(spline.first());
  int src = 0;
  float src_seg_len = math::distance(spline[0], spline[1]);
  float carried = 0.0f;
  for (const int i : IndexRange(1, segments - 1)) {
    const float target = float(i) * step;
    while (carried + src_seg_len < target && src + 2 < spline.size()) {
      carried += src_seg_len;
      src++;
      src_seg_len = math::distance(spline[src], spline[src + 1]);
    }
    const float local = (src_seg_len > 0.0f) ? (target - carried) / src_seg_len : 0.0f;
    resampled.append(
        math::interpolate(spline[src], spline[src + 1], std::clamp(local, 0.0f, 1.0f)));
  }
  resampled.append(spline.last());

  /* Laplacian smoothing with the endpoints pinned, so the drawn start / end stay under the
   * cursor. */
  const float weight = std::clamp(params.smooth, 0.0f, 1.0f);
  const int iterations = std::clamp(params.smooth_iterations, 0, max_smooth_iterations);
  if (weight > 0.0f && iterations > 0) {
    Vector<T> smoothed = resampled;
    for (int iter = 0; iter < iterations; iter++) {
      for (const int i : IndexRange(1, resampled.size() - 2)) {
        const T average = (resampled[i - 1] + resampled[i + 1]) * 0.5f;
        smoothed[i] = math::interpolate(resampled[i], average, weight);
      }
      resampled = smoothed;
    }
  }

  points_ = std::move(resampled);
  rebuild_arc_lengths();
  rebuild_kdtree();
  return is_valid();
}

template<typename T> void GradientCurve<T>::rebuild_arc_lengths()
{
  arc_ = Array<float>(points_.size());
  float total = 0.0f;
  for (const int i : points_.index_range()) {
    arc_[i] = total;
    if (i + 1 < points_.size()) {
      total += math::distance(points_[i], points_[i + 1]);
    }
  }
  length_ = total;
}

template<typename T> void GradientCurve<T>::rebuild_kdtree()
{
  kdtree_.reset();
  if (points_.is_empty()) {
    return;
  }
  KDTree<T> *tree = kdtree_new<T>(uint(points_.size()));
  for (const int i : points_.index_range()) {
    kdtree_insert<T>(tree, i, points_[i]);
  }
  kdtree_balance<T>(tree);
  kdtree_.reset(tree);
}

template<typename T> T GradientCurve<T>::position_at_s(const float s) const
{
  if (!is_valid()) {
    return T(0.0f);
  }
  const float clamped = std::clamp(s, 0.0f, length_);
  /* Binary search for the segment containing \a clamped. */
  int lo = 0, hi = points_.size() - 1;
  while (lo + 1 < hi) {
    const int mid = (lo + hi) / 2;
    if (arc_[mid] <= clamped) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  const float seg_len = math::distance(points_[lo], points_[hi]);
  const float local = (seg_len > 0.0f) ? (clamped - arc_[lo]) / seg_len : 0.0f;
  return math::interpolate(points_[lo], points_[hi], std::clamp(local, 0.0f, 1.0f));
}

template<typename T> Bounds<T> GradientCurve<T>::bounds() const
{
  Bounds<T> result;
  if (!is_valid()) {
    result.min = T(0.0f);
    result.max = T(0.0f);
    return result;
  }
  result.min = points_.first();
  result.max = points_.first();
  for (const T &co : points_) {
    for (const int i : IndexRange(T::type_length)) {
      result.min[i] = std::min(result.min[i], co[i]);
      result.max[i] = std::max(result.max[i], co[i]);
    }
  }
  return result;
}

template<typename T> CurveProjection GradientCurve<T>::project(const T &pos) const
{
  CurveProjection best;
  if (!is_valid()) {
    return best;
  }

  /* Nearest resampled points, then the segments adjacent to each of them. With the even
   * resampling step the closest vertex alone would be enough; the extra candidates guard against
   * numerical corner cases AND against the closest segment being missed when two branches of a
   * curve almost touch (a U-turn narrower than the resampling step). The residual error there is
   * bounded by roughly half the step, which is acceptable; NOTE: raise #k_nearest if that ever
   * needs to be tighter. */
  constexpr int k_nearest = 3;
  std::array<KDTreeNearest<T>, k_nearest> neighbors;
  const int found = kdtree_find_nearest_n<T>(
      kdtree_.get(), pos, neighbors.data(), uint(k_nearest));
  const int last_seg = points_.size() - 2;
  for (const int n : IndexRange(found)) {
    const int nearest = neighbors[n].index;
    for (const int seg : {nearest - 1, nearest}) {
      if (seg < 0 || seg > last_seg) {
        continue;
      }
      const T &pa = points_[seg];
      const T ab = points_[seg + 1] - pa;
      float h_raw;
      const float dist = project_on_segment(pos, pa, ab, h_raw);
      /* The distance is always measured to the true polyline (h clamped). The arc-length `s`
       * extrapolates past the stroke ends for Along, but the first segments must not be
       * extrapolated *inward* (and the last ones not before their start), or a point near an
       * inner corner would project onto the wrong branch with a shorter distance. A single-
       * segment (two-point) curve extrapolates freely at both ends. */
      float h_for_s = (last_seg == 0) ? h_raw : std::clamp(h_raw, 0.0f, 1.0f);
      if (last_seg > 0) {
        if (seg == 0) {
          h_for_s = std::min(h_raw, 1.0f);
        }
        if (seg == last_seg) {
          h_for_s = std::max(h_raw, 0.0f);
        }
      }
      if (dist < best.distance) {
        best.distance = dist;
        best.s = arc_[seg] + h_for_s * math::distance(points_[seg], points_[seg + 1]);
      }
    }
  }
  return best;
}

template<typename T>
float GradientCurve<T>::eval_t(const eGradientCurveMode mode,
                               const float width,
                               const T &pos) const
{
  if (!is_valid()) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  return eval_t_from_projection(mode, width, length_, project(pos));
}

/* -------------------------------------------------------------------- */
/** \name Control-point simplification
 * \{ */

/**
 * Squared deviation of \a p from the chord \a a - \a b. A degenerate chord (a gesture that looped
 * back onto its start) falls back to the radial distance to \a a, so a returning stroke still
 * keeps its farthest point rather than collapsing to the endpoints.
 */
template<typename T> static float simplify_chord_dist_sq(const T &p, const T &a, const T &b)
{
  const T ab = b - a;
  if (math::length_squared(ab) <= FLT_EPSILON) {
    return math::length_squared(p - a);
  }
  return math::dist_squared_to_line_segment(p, a, b);
}

/** Non-recursive Ramer-Douglas-Peucker: appends every kept interior index to \a r_kept, in order,
 * the endpoints excluded. */
template<typename T>
static void simplify_rdp(
    Span<T> points, const int first, const int last, const float tolerance_sq, Vector<int> &r_kept)
{
  Stack<IndexRange> stack;
  stack.push(IndexRange(first, last - first + 1));
  while (!stack.is_empty()) {
    const IndexRange range = stack.pop();
    if (range.size() < 3) {
      continue;
    }
    const int range_last = range.last();
    const T &a = points[range.first()];
    const T &b = points[range_last];
    int worst_index = -1;
    float worst_dist_sq = 0.0f;
    for (int i = range.first() + 1; i < range_last; i++) {
      const float dist_sq = simplify_chord_dist_sq(points[i], a, b);
      if (dist_sq > worst_dist_sq) {
        worst_dist_sq = dist_sq;
        worst_index = i;
      }
    }
    if (worst_index < 0 || worst_dist_sq <= tolerance_sq) {
      continue;
    }
    r_kept.append(worst_index);
    stack.push(IndexRange(range.first(), worst_index - range.first() + 1));
    stack.push(IndexRange(worst_index, range_last - worst_index + 1));
  }
  /* The stack visits sub-ranges out of order; sort the kept interior indices into ascending
   * order so the caller can read the polyline in stroke order. */
  std::sort(r_kept.begin(), r_kept.end());
}

template<typename T>
Vector<int> simplify_control_points(const Span<T> points, const float tolerance, const int max_num)
{
  Vector<int> kept;
  const int last = int(points.size()) - 1;
  if (points.is_empty()) {
    return kept;
  }
  if (last < 2) {
    for (const int i : IndexRange(points.size())) {
      kept.append(i);
    }
    return kept;
  }

  const int cap = std::max(max_num, 2);
  float tol = std::max(tolerance, FLT_EPSILON);
  for (int attempt = 0; attempt < 16; attempt++) {
    kept.clear();
    simplify_rdp<T>(points, 0, last, tol * tol, kept);
    if (kept.size() + 2 <= cap) {
      break;
    }
    tol *= 2.0f;
  }

  /* Assemble the final indices with the endpoints, always kept. */
  Vector<int> result;
  result.reserve(kept.size() + 2);
  result.append(0);
  result.extend(kept);
  result.append(last);

  if (result.size() > cap) {
    /* The tolerance growth did not converge (all samples equidistant from every chord): keep the
     * endpoints plus an evenly spaced subset so the caller always gets an editable curve. */
    Vector<int> thinned;
    thinned.reserve(cap);
    for (const int i : IndexRange(cap)) {
      const int index = int(int64_t(i) * (result.size() - 1) / (cap - 1));
      thinned.append(result[index]);
    }
    result = std::move(thinned);
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Explicit instantiations
 * \{ */

template class GradientCurve<float2>;
template class GradientCurve<float3>;

template Vector<int> simplify_control_points<float2>(Span<float2>, float, int);
template Vector<int> simplify_control_points<float3>(Span<float3>, float, int);

/** \} */

}  // namespace blender::ed::sculpt_paint::gradient_curve
