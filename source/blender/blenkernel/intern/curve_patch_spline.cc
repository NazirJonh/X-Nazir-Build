/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Arc-length spline geometry for Curve Patch: polyline rebuild, evaluation, closest-point queries,
 * and the normal-field smoothing the ribbon takes its binormals from. Pure geometry -- the texture
 * semantics that used to share this file live in `curve_patch_texture_map.cc`.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BKE_curve_patch.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

namespace blender::bke {
void CurvePatchSpline::clear()
{
  poly_3d.clear();
  lengths_3d.clear();
  tangents_3d.clear();
  radii.clear();
  normals_3d.clear();
  normals_smooth_3d.clear();
  cyclic = false;
}

bool CurvePatchSpline::is_empty() const
{
  return poly_3d.size() < 2;
}

float CurvePatchSpline::total_length() const
{
  return lengths_3d.is_empty() ? 0.0f : lengths_3d.last();
}

void CurvePatchSpline::build_from_positions(const Span<float3> positions,
                                            const Span<float> radii_in,
                                            const bool cyclic_in,
                                            const Span<float3> normals_in)
{
  clear();
  if (positions.size() < 2) {
    return;
  }
  cyclic = cyclic_in;

  poly_3d.reserve(positions.size() + (cyclic ? 1 : 0));
  poly_3d.extend(positions);
  if (cyclic) {
    /* `bke::CurvesGeometry::evaluated_positions()` of a cyclic curve stops just short of wrapping:
     * it never repeats the first point at the end. Append it so the closing edge exists, otherwise
     * `total_length()` under-counts by a whole segment and the strip stops short of the join. */
    poly_3d.append(positions[0]);
  }

  lengths_3d.reserve(poly_3d.size());
  float accum = 0.0f;
  lengths_3d.append(0.0f);
  for (const int i : IndexRange(1, poly_3d.size() - 1)) {
    accum += math::distance(poly_3d[i - 1], poly_3d[i]);
    lengths_3d.append(accum);
  }

  const int last = int(poly_3d.size()) - 1;
  /* On a closed curve `poly_3d[0]` and `poly_3d[last]` are the SAME point, so both take the same
   * through-the-join central difference. One-sided differences there would make the tangent flip
   * direction across the join, creasing the ribbon's UV exactly where the pattern has to meet
   * itself seamlessly. `last - 1` is the vertex before the join, `1` the one after it. */
  const bool wrap_ends = cyclic && poly_3d.size() >= 3;
  tangents_3d.reserve(poly_3d.size());
  for (const int i : IndexRange(poly_3d.size())) {
    float3 dir;
    if (i == 0 || i == last) {
      dir = wrap_ends ? poly_3d[1] - poly_3d[last - 1] :
                        (i == 0 ? poly_3d[1] - poly_3d[0] : poly_3d[last] - poly_3d[last - 1]);
    }
    else {
      dir = poly_3d[i + 1] - poly_3d[i - 1];
    }
    const float len = math::length(dir);
    tangents_3d.append(len > 1e-8f ? dir / len : float3(1.0f, 0.0f, 0.0f));
  }

  if (!radii_in.is_empty()) {
    BLI_assert(radii_in.size() == positions.size());
    radii.reserve(poly_3d.size());
    radii.extend(radii_in);
    if (cyclic) {
      radii.append(radii_in[0]);
    }
  }

  if (!normals_in.is_empty()) {
    BLI_assert(normals_in.size() == positions.size());
    normals_3d.reserve(poly_3d.size());
    normals_3d.extend(normals_in);
    if (cyclic) {
      normals_3d.append(normals_in[0]);
    }
  }
}

/** Finds the segment index `i` such that `lengths_3d[i] <= s <= lengths_3d[i + 1]`, and the
 * fraction `t` in `[0, 1]` of `s` within that segment. Clamps `s` to `[0, total_length()]`.
 *
 * Binary search rather than a linear scan: `lengths_3d` is monotonically increasing by
 * construction, and the relief walk calls this once per surviving mesh vertex (via `evaluate()`
 * and `radius_at()`) -- a linear scan made that per-vertex cost grow with the curve's tessellated
 * length, which defeats the point of the O(1) ribbon LUT lookup that precedes it. */
static void find_segment(const Span<float> lengths_3d, float s, int &r_i, float &r_t)
{
  const int last_seg = int(lengths_3d.size()) - 2;
  const float total = lengths_3d.last();
  s = math::clamp(s, 0.0f, total);

  /* First index whose cumulative length is strictly greater than `s`; the segment starting one
   * index below it is the one containing `s`. */
  const float *upper = std::upper_bound(lengths_3d.begin(), lengths_3d.end(), s);
  const int i = std::clamp(int(upper - lengths_3d.begin()) - 1, 0, last_seg);

  const float seg_len = lengths_3d[i + 1] - lengths_3d[i];
  r_i = i;
  r_t = seg_len > 1e-8f ? (s - lengths_3d[i]) / seg_len : 0.0f;
}

float3 CurvePatchSpline::evaluate(const float s) const
{
  if (is_empty()) {
    return float3(0.0f);
  }
  int i;
  float t;
  find_segment(lengths_3d, s, i, t);
  return math::interpolate(poly_3d[i], poly_3d[i + 1], t);
}

float3 CurvePatchSpline::tangent_at(const float s) const
{
  if (is_empty()) {
    return float3(1.0f, 0.0f, 0.0f);
  }
  int i;
  float t;
  find_segment(lengths_3d, s, i, t);
  const float3 blended = math::interpolate(tangents_3d[i], tangents_3d[i + 1], t);
  const float len = math::length(blended);
  return len > 1e-8f ? blended / len : tangents_3d[i];
}

float3 CurvePatchSpline::normal_at(const float s) const
{
  if (is_empty() || normals_smooth_3d.size() != poly_3d.size()) {
    return math::normalize(plane_normal);
  }
  int i;
  float t;
  find_segment(lengths_3d, s, i, t);
  const float3 blended = math::interpolate(normals_smooth_3d[i], normals_smooth_3d[i + 1], t);
  const float len = math::length(blended);
  return len > 1e-8f ? blended / len : normals_smooth_3d[i];
}

float CurvePatchSpline::radius_at(const float s) const
{
  BLI_assert(!radii.is_empty());
  BLI_assert(radii.size() == poly_3d.size());
  if (is_empty()) {
    return 0.0f;
  }
  int i;
  float t;
  find_segment(lengths_3d, s, i, t);
  return math::interpolate(radii[i], radii[i + 1], t);
}

float CurvePatchSpline::distance_sq_to(const float3 &query) const
{
  if (is_empty()) {
    return 0.0f;
  }
  float best_dist_sq = FLT_MAX;
  for (const int i : IndexRange(poly_3d.size() - 1)) {
    const float3 &a = poly_3d[i];
    const float3 &b = poly_3d[i + 1];
    const float3 ab = b - a;
    const float ab_len_sq = math::length_squared(ab);
    const float t = ab_len_sq > 1e-8f ?
                        math::clamp(math::dot(query - a, ab) / ab_len_sq, 0.0f, 1.0f) :
                        0.0f;
    const float3 closest = a + ab * t;
    const float dist_sq = math::length_squared(query - closest);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
    }
  }
  return best_dist_sq;
}

/* Shared nearest-segment search: fills the winning segment index, its clamped parameter, and the
 * squared distance. Both public closest-point variants build on this so the math lives in one
 * place. */
static void closest_segment(const Span<float3> poly_3d,
                            const float3 &query,
                            int &r_i,
                            float &r_t,
                            float &r_dist_sq)
{
  r_dist_sq = FLT_MAX;
  r_i = 0;
  r_t = 0.0f;
  for (const int i : IndexRange(poly_3d.size() - 1)) {
    const float3 &a = poly_3d[i];
    const float3 &b = poly_3d[i + 1];
    const float3 ab = b - a;
    const float ab_len_sq = math::length_squared(ab);
    const float t = ab_len_sq > 1e-8f ?
                        math::clamp(math::dot(query - a, ab) / ab_len_sq, 0.0f, 1.0f) :
                        0.0f;
    const float3 closest = a + ab * t;
    const float dist_sq = math::length_squared(query - closest);
    if (dist_sq < r_dist_sq) {
      r_dist_sq = dist_sq;
      r_i = i;
      r_t = t;
    }
  }
}

void CurvePatchSpline::closest_point(const float3 &query,
                                     float &r_s,
                                     float3 &r_tangent,
                                     float &r_lateral,
                                     float *r_normal_dist) const
{
  if (is_empty()) {
    return;
  }

  int best_i;
  float best_t;
  [[maybe_unused]] float best_dist_sq;
  closest_segment(poly_3d.as_span(), query, best_i, best_t, best_dist_sq);

  r_s = lengths_3d[best_i] + best_t * (lengths_3d[best_i + 1] - lengths_3d[best_i]);
  r_tangent = this->tangent_at(r_s);

  const float3 closest_pos = math::interpolate(poly_3d[best_i], poly_3d[best_i + 1], best_t);
  const float3 offset = query - closest_pos;
  float3 side_axis = math::cross(plane_normal, r_tangent);
  const float side_len = math::length(side_axis);
  side_axis = side_len > 1e-8f ? side_axis / side_len : float3(0.0f, 1.0f, 0.0f);
  r_lateral = math::dot(offset, side_axis);
  if (r_normal_dist != nullptr) {
    *r_normal_dist = math::dot(offset, plane_normal);
  }

  /* `best_t` above is clamped to `[0, 1]`, so `r_s` never leaves `[0, total_length()]` even when
   * `query` sits past one of the polyline's own ends -- the clamped segment endpoint is still
   * reported as the "closest point", silently discarding how far past that endpoint `query`
   * actually is. Callers that measure distance to the nearest end of the curve (end-of-strip
   * falloff) need that discarded distance, or every such vertex looks like it sits exactly at the
   * end regardless of how far outside the curve it really is. Recover it here by re-projecting
   * onto the winning boundary segment WITHOUT clamping `t`, but only when that segment is a true
   * polyline extremity (`best_i == 0` or `best_i == poly_3d.size() - 2`) and the unclamped `t`
   * actually falls outside it -- an interior segment can never be the true nearest one if its own
   * unclamped closest point would land outside `[0, 1]`, so this never fires except at the two
   * ends. Does not affect `r_tangent`/`r_lateral`/`r_normal_dist` above, which stay anchored to
   * the true (clamped) closest point on the curve. */
  if (best_i == 0 || best_i == poly_3d.size() - 2) {
    const float3 &a = poly_3d[best_i];
    const float3 &b = poly_3d[best_i + 1];
    const float3 ab = b - a;
    const float ab_len_sq = math::length_squared(ab);
    if (ab_len_sq > 1e-8f) {
      const float t_raw = math::dot(query - a, ab) / ab_len_sq;
      const float seg_len = lengths_3d[best_i + 1] - lengths_3d[best_i];
      if (best_i == 0 && t_raw < 0.0f) {
        r_s = t_raw * seg_len;
      }
      else if (best_i == poly_3d.size() - 2 && t_raw > 1.0f) {
        r_s = lengths_3d[best_i] + t_raw * seg_len;
      }
    }
  }
}

void CurvePatchSpline::closest_point_dist(const float3 &query,
                                          float &r_s,
                                          float3 &r_tangent,
                                          float &r_dist) const
{
  if (is_empty()) {
    return;
  }
  int best_i;
  float best_t, best_dist_sq;
  closest_segment(poly_3d.as_span(), query, best_i, best_t, best_dist_sq);
  r_s = lengths_3d[best_i] + best_t * (lengths_3d[best_i + 1] - lengths_3d[best_i]);
  r_tangent = this->tangent_at(r_s);
  r_dist = std::sqrt(best_dist_sq);
}

void curve_patch_spline_smooth_normals(CurvePatchSpline &spline, const float smooth_length)
{
  spline.normals_smooth_3d.clear();
  if (spline.normals_3d.is_empty()) {
    return;
  }
  const int count = int(spline.normals_3d.size());
  spline.normals_smooth_3d.resize(count);
  if (!(smooth_length > 0.0f) || spline.lengths_3d.size() != count) {
    spline.normals_smooth_3d.as_mutable_span().copy_from(spline.normals_3d.as_span());
    return;
  }
  const float half = smooth_length * 0.5f;

  /* `lengths_3d` is monotonically increasing, so the window bounds only ever move forward: the two
   * cursors below visit each sample once instead of rescanning the whole curve per sample. The
   * summed terms and their order are unchanged, so the result is identical to the naive scan. */
  int lo = 0;
  int hi = 0;
  for (const int i : IndexRange(count)) {
    const float s = spline.lengths_3d[i];
    while (lo < count && spline.lengths_3d[lo] < s - half) {
      lo++;
    }
    hi = std::max(hi, lo);
    while (hi < count && spline.lengths_3d[hi] - s <= half) {
      hi++;
    }
    float3 accum(0.0f);
    for (const int j : IndexRange(lo, hi - lo)) {
      accum += spline.normals_3d[j];
    }
    const float len = math::length(accum);
    /* A window whose normals cancelled out (a turn of exactly 180 degrees) leaves the sample
     * unsmoothed: every direction is equally arbitrary there, and the original at least agrees with
     * its neighbours. */
    spline.normals_smooth_3d[i] = len > 1e-6f ? accum / len : spline.normals_3d[i];
  }
}

}  // namespace blender::bke
