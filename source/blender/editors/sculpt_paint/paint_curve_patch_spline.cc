/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cfloat>

#include "paint_curve_patch_spline.hh"

#include "DNA_texture_types.h"

#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

namespace blender::ed::sculpt_paint {

void CurvePatchSpline::clear()
{
  poly_3d.clear();
  lengths_3d.clear();
  tangents_3d.clear();
  radii.clear();
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
                                            const Span<float> radii_in)
{
  clear();
  if (positions.size() < 2) {
    return;
  }

  poly_3d.reserve(positions.size());
  poly_3d.extend(positions);

  lengths_3d.reserve(poly_3d.size());
  float accum = 0.0f;
  lengths_3d.append(0.0f);
  for (const int i : IndexRange(1, poly_3d.size() - 1)) {
    accum += math::distance(poly_3d[i - 1], poly_3d[i]);
    lengths_3d.append(accum);
  }

  tangents_3d.reserve(poly_3d.size());
  for (const int i : IndexRange(poly_3d.size())) {
    float3 dir;
    if (i == 0) {
      dir = poly_3d[1] - poly_3d[0];
    }
    else if (i == poly_3d.size() - 1) {
      dir = poly_3d[i] - poly_3d[i - 1];
    }
    else {
      dir = poly_3d[i + 1] - poly_3d[i - 1];
    }
    const float len = math::length(dir);
    tangents_3d.append(len > 1e-8f ? dir / len : float3(1.0f, 0.0f, 0.0f));
  }

  if (!radii_in.is_empty()) {
    BLI_assert(radii_in.size() == positions.size());
    radii.reserve(radii_in.size());
    radii.extend(radii_in);
  }
}

/** Finds the segment index `i` such that `lengths_3d[i] <= s <= lengths_3d[i + 1]`, and the
 * fraction `t` in `[0, 1]` of `s` within that segment. Clamps `s` to `[0, total_length()]`. */
static void find_segment(const Span<float> lengths_3d, float s, int &r_i, float &r_t)
{
  const float total = lengths_3d.last();
  s = math::clamp(s, 0.0f, total);
  for (const int i : IndexRange(lengths_3d.size() - 1)) {
    if (s <= lengths_3d[i + 1] || i == lengths_3d.size() - 2) {
      const float seg_len = lengths_3d[i + 1] - lengths_3d[i];
      r_i = i;
      r_t = seg_len > 1e-8f ? (s - lengths_3d[i]) / seg_len : 0.0f;
      return;
    }
  }
  r_i = 0;
  r_t = 0.0f;
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

void CurvePatchSpline::closest_point(const float3 &query,
                                     float &r_s,
                                     float3 &r_tangent,
                                     float &r_lateral,
                                     float *r_normal_dist) const
{
  if (is_empty()) {
    return;
  }

  float best_dist_sq = FLT_MAX;
  int best_i = 0;
  float best_t = 0.0f;
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
      best_i = i;
      best_t = t;
    }
  }

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

float curve_patch_texture_tile_span(const int length_mode,
                                    const int repeat,
                                    const float total_length,
                                    const float radius_at_s)
{
  switch (eMTex_CurvePatchLengthMode(length_mode)) {
    case MTEX_CURVE_PATCH_LENGTH_REPEAT:
      /* `max(1, repeat)` guards a repeat count that bypassed RNA's 1..64 range (Python API, an
       * older/edited file) from producing a divide-by-zero or a negative span. */
      return total_length / float(std::max(1, repeat));
    case MTEX_CURVE_PATCH_LENGTH_STRETCH:
      return total_length;
    case MTEX_CURVE_PATCH_LENGTH_DEFAULT:
    default:
      return std::min(total_length, 2.0f * radius_at_s);
  }
}

}  // namespace blender::ed::sculpt_paint
