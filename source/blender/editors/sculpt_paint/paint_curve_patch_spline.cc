/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "paint_curve_patch_spline.hh"

#include "DNA_texture_types.h"

#include "BLI_hash.h"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

namespace blender::ed::sculpt_paint {

void CurvePatchSpline::clear()
{
  poly_3d.clear();
  lengths_3d.clear();
  tangents_3d.clear();
  radii.clear();
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
                                            const bool cyclic_in)
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

float curve_patch_texture_tile_span(const int length_mode,
                                    const int repeat,
                                    const float total_length,
                                    const float radius_at_s,
                                    const bool cyclic)
{
  float span;
  switch (eMTex_CurvePatchLengthMode(length_mode)) {
    case MTEX_CURVE_PATCH_LENGTH_REPEAT:
      /* `max(1, repeat)` guards a repeat count that bypassed RNA's 1..64 range (Python API, an
       * older/edited file) from producing a divide-by-zero or a negative span. */
      span = total_length / float(std::max(1, repeat));
      break;
    case MTEX_CURVE_PATCH_LENGTH_STRETCH:
      span = total_length;
      break;
    case MTEX_CURVE_PATCH_LENGTH_DEFAULT:
    default:
      span = std::min(total_length, 2.0f * radius_at_s);
      break;
  }

  /* On a closed curve the pattern has to meet itself at `s == 0`, which only happens when the loop
   * holds a WHOLE number of tiles -- otherwise the last tile is cut mid-pattern and shows as a seam.
   * Repeat and Stretch already divide the length into an integer count and come out of the snap
   * unchanged; Default, whose tile follows the brush radius, generally does not, and gets its tile
   * stretched or squeezed by up to ~1.5x to the nearest whole count. */
  if (cyclic && span > 1e-8f && total_length > 1e-8f) {
    const float tiles = std::max(1.0f, std::round(total_length / span));
    span = total_length / tiles;
  }
  return span;
}

/* Hash channels, so the same stamp's size / angle / strength / two jitter axes stay independent. */
enum {
  STAMP_HASH_SIZE = 0,
  STAMP_HASH_ANGLE = 1,
  STAMP_HASH_STRENGTH = 2,
  STAMP_HASH_JITTER_V = 3,
  STAMP_HASH_JITTER_U = 4,
};

static float stamp_random(const int index, const uint32_t seed, const int channel)
{
  return BLI_hash_int_3d_to_float(uint32_t(index), seed, uint32_t(channel));
}

/* Absolute ceiling on the number of stamps a single build can produce. The 1% Spacing floor used
 * below is relative to the brush radius, not an absolute step size, so it does not by itself bound
 * `stamp_num`: a long curve paired with a very small radius still yields a tiny step and asks for
 * millions of stamps (e.g. a 1000-unit curve at radius 0.001 wants ~50 million). An interactive
 * patch tops out at a few hundred stamps even for a full-screen curve at default spacing, so 10000
 * is generous headroom while keeping the `reserve()` below bounded and the narrowing to `int` safe
 * from overflow. */
static constexpr int CURVE_PATCH_STAMP_NUM_MAX = 10000;

void curve_patch_stamps_build(const CurvePatchSpline &spline,
                              const float radius,
                              const float spacing_frac,
                              const float jitter_amount,
                              const float size_random,
                              const float strength_random,
                              const float base_angle,
                              const float random_angle,
                              const uint32_t seed,
                              Vector<CurvePatchStamp> &r_stamps)
{
  r_stamps.clear();
  const float total_length = spline.total_length();
  if (spline.is_empty() || total_length <= 1e-6f || radius <= 1e-6f) {
    return;
  }

  /* Floor Spacing at 1% of the diameter, the same lower bound the brush's own Spacing slider
   * enforces, so a zero or near-zero Spacing does not produce a zero-length `step`. This floor is
   * relative to `radius`, though, not an absolute distance, so it does not by itself bound the
   * stamp count -- see `CURVE_PATCH_STAMP_NUM_MAX`, which is what actually does. */
  const float step = std::max(spacing_frac, 0.01f) * 2.0f * radius;

  int stamp_num;
  float used_step;
  if (spline.cyclic) {
    /* A loop has no ends: placing a stamp at `total_length` would double up on the one at 0. Fit a
     * whole number of steps instead and stop one step short of the seam. Compute in `double` and
     * clamp before narrowing to `int`, since `total_length / step` can be arbitrarily large for a
     * long curve with a small radius, and an `int` overflow here is undefined behavior. */
    const double raw_num = std::round(double(total_length) / double(step));
    stamp_num = int(std::clamp(raw_num, 1.0, double(CURVE_PATCH_STAMP_NUM_MAX)));
    /* `used_step` is derived FROM `stamp_num` on this branch, so it must be recomputed after the
     * clamp above -- otherwise a clamped count would leave a gap short of the seam. */
    used_step = total_length / float(stamp_num);
  }
  else {
    const double raw_num = std::floor(double(total_length) / double(step)) + 1.0;
    stamp_num = int(std::clamp(raw_num, 1.0, double(CURVE_PATCH_STAMP_NUM_MAX)));
    /* `used_step` is the requested step and is independent of `stamp_num` on this branch, so
     * clamping the count simply truncates the stamp row instead of leaving a gap. */
    used_step = step;
  }

  r_stamps.reserve(stamp_num);
  for (const int i : IndexRange(stamp_num)) {
    CurvePatchStamp stamp;
    const float s = float(i) * used_step;
    stamp.center_v = s + jitter_amount * (2.0f * stamp_random(i, seed, STAMP_HASH_JITTER_V) - 1.0f);
    stamp.center_u = jitter_amount * (2.0f * stamp_random(i, seed, STAMP_HASH_JITTER_U) - 1.0f);
    /* Shrink only. Growing past the brush radius would make the visible patch wider than the brush
     * cursor promises and would force the ribbon to widen for the size setting too. The 0.05 floor
     * keeps a fully-randomized stamp from collapsing to nothing. */
    const float size_factor = 1.0f - size_random * stamp_random(i, seed, STAMP_HASH_SIZE);
    stamp.half_extent = radius * std::max(size_factor, 0.05f);
    stamp.angle = base_angle + random_angle * stamp_random(i, seed, STAMP_HASH_ANGLE);
    const float strength_factor = 1.0f -
                                  strength_random * stamp_random(i, seed, STAMP_HASH_STRENGTH);
    stamp.strength = std::max(strength_factor, 0.05f);
    r_stamps.append(stamp);
  }

  /* Jitter along the curve can reorder neighbours; the relief binary-searches this list by
   * `center_v`, so restore the ordering rather than assume it. */
  std::sort(r_stamps.begin(), r_stamps.end(), [](const CurvePatchStamp &a, const CurvePatchStamp &b) {
    return a.center_v < b.center_v;
  });
}

void curve_patch_stamps_add_cyclic_wrap(Vector<CurvePatchStamp> &stamps,
                                        const float total_length,
                                        const float max_extent)
{
  if (stamps.is_empty() || total_length <= 1e-6f || max_extent <= 0.0f) {
    return;
  }

  /* A loop shorter than the search window would otherwise qualify every stamp on both sides at
   * once, and a truly faithful answer there would need ghosts at every multiple of the length --
   * an unbounded tiling for a loop that is smaller than a single stamp. Clamping the reach to one
   * period keeps the result bounded (at most two ghosts per stamp) and deterministic; such a loop
   * is degenerate for Stamps mode either way, since its stamps already overlap themselves. */
  const float reach = std::min(max_extent, total_length);

  /* Snapshot the original count: the loop appends to the same vector it reads, and ghosts must
   * never spawn ghosts of their own. */
  const int real_num = stamps.size();
  for (const int i : IndexRange(real_num)) {
    /* Taken by value -- `append()` below may reallocate and invalidate a reference. */
    const CurvePatchStamp stamp = stamps[i];
    if (stamp.center_v < reach) {
      CurvePatchStamp ghost = stamp;
      ghost.center_v = stamp.center_v + total_length;
      stamps.append(ghost);
    }
    if (stamp.center_v > total_length - reach) {
      CurvePatchStamp ghost = stamp;
      ghost.center_v = stamp.center_v - total_length;
      stamps.append(ghost);
    }
  }

  /* The ghosts land outside `[0, total_length]` on both sides, so the list is no longer ordered by
   * arc length -- restore it, since the relief binary-searches by `center_v`. */
  std::sort(stamps.begin(), stamps.end(), [](const CurvePatchStamp &a, const CurvePatchStamp &b) {
    return a.center_v < b.center_v;
  });
}

}  // namespace blender::ed::sculpt_paint
