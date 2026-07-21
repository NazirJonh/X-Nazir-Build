/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Roll texture mapping for paint strokes (the #BRUSH_STROKE_ROLL stroke method).
 *
 * This file implements the arc-length-parameterized spline (#RollSpline, built on top of
 * #CurvePatchSpline), virtual extension points, the surface-interpolation grid (Catmull-Clark
 * subdivision, Laplacian smoothing, LUT rasterization), and the UV lookup used by
 * sculpt_apply_texture() when the active stroke method is Roll.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
/* Must follow BLI_math_vector.hh: length_parameterize.hh pulls only scalar math (BLI_math_base.hh),
 * and MSVC resolves the vector `math::distance` used by accumulate_lengths<float2/float3> at this
 * header's definition point -- so the vector overload has to be visible first. */
#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_length_parameterize.hh"
#include "BLI_math_geom.hh"
#include "BLI_stack.hh"
#include "BLI_utildefines.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "RNA_access.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "WM_api.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "ED_view3d.hh"

#include "paint_intern.hh"

#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/* Shared tuning constants for roll texture mapping. */
static constexpr float MIN_ROLL_PRESSURE = 0.05f;
static constexpr float PRESSURE_RATIO_MIN = 0.5f;
static constexpr float PRESSURE_RATIO_MAX = 2.0f;
static constexpr int VIRTUAL_EXTENSION_POINTS = 6;
static constexpr int EVAL_ROW_MARGIN = 3;

/**
 * Extrapolate pen pressure geometrically for virtual extension points.
 * \param base_p: pressure at the anchor (real) point.
 * \param ratio: per-step multiplier (clamped from neighboring real points).
 * \param n_points: number of virtual points to generate.
 * \param farthest_first: if true, step count decreases with index (backward ext).
 * \param r_pressures: output vector to append to.
 */
static void extrapolate_pressures(float base_p,
                                  float ratio,
                                  int n_points,
                                  bool farthest_first,
                                  Vector<float> &r_pressures)
{
  for (int i = 0; i < n_points; i++) {
    const int steps = farthest_first ? (n_points - i) : (i + 1);
    r_pressures.append(std::clamp(base_p * powf(ratio, float(steps)), 0.01f, 1.0f));
  }
}

/** Project onto the roll stroke's frozen projection plane (see `roll_proj_normal_`). */
static float3 roll_point_in_projection_plane(const float3 &point,
                                             const float3 &plane_origin,
                                             const float3 &plane_normal)
{
  return point - plane_normal * math::dot(point - plane_origin, plane_normal);
}

static float roll_point_segment_distance(const float3 &point,
                                         const float3 &seg_a,
                                         const float3 &seg_b,
                                         const float3 &plane_origin,
                                         const float3 &plane_normal)
{
  if (math::length_squared(plane_normal) > 1e-12f) {
    const float3 p = roll_point_in_projection_plane(point, plane_origin, plane_normal);
    const float3 a = roll_point_in_projection_plane(seg_a, plane_origin, plane_normal);
    const float3 b = roll_point_in_projection_plane(seg_b, plane_origin, plane_normal);
    return math::sqrt(math::dist_squared_to_line_segment(p, a, b));
  }
  return math::sqrt(math::dist_squared_to_line_segment(point, seg_a, seg_b));
}

/**
 * Ramer-Douglas-Peucker on the real stroke knots. Marks interior points that can be removed without
 * deviating more than `epsilon` from the chord between kept neighbors. Endpoints are always kept.
 */
static void roll_simplify_polyline_mask(const Span<float3> positions,
                                        const float epsilon,
                                        const float3 &plane_origin,
                                        const float3 &plane_normal,
                                        MutableSpan<bool> r_keep)
{
  const int n = int(positions.size());
  BLI_assert(r_keep.size() == n);
  r_keep.fill(true);
  if (n < 3 || epsilon <= 0.0f) {
    return;
  }

  Array<bool> to_delete(n, false);
  Stack<IndexRange> stack;
  stack.push(IndexRange(n));

  while (!stack.is_empty()) {
    const IndexRange range = stack.pop();
    if (range.size() < 3) {
      continue;
    }
    const IndexRange inside = range.drop_front(1).drop_back(1);
    float max_dist = -1.0f;
    int max_index = -1;
    const float3 &a = positions[range.first()];
    const float3 &b = positions[range.last()];
    for (const int64_t index : inside) {
      const float dist = roll_point_segment_distance(
          positions[index], a, b, plane_origin, plane_normal);
      if (dist > max_dist) {
        max_dist = dist;
        max_index = int(index);
      }
    }
    if (max_dist > epsilon) {
      stack.push(IndexRange(range.first(), max_index - range.first() + 1));
      stack.push(IndexRange(max_index, range.last() - max_index + 1));
    }
    else {
      for (const int64_t index : inside) {
        to_delete[index] = true;
      }
    }
  }

  for (const int i : IndexRange(n)) {
    r_keep[i] = !to_delete[i];
  }
}

/** Uniform arc-length resample when a safety cap is still exceeded after RDP. */
static void roll_uniform_resample(const Span<float3> positions,
                                  const Span<float> radii,
                                  const int target_count,
                                  Vector<float3> &r_positions,
                                  Vector<float> &r_radii)
{
  BLI_assert(positions.size() == radii.size());
  BLI_assert(target_count >= 2);
  BLI_assert(positions.size() >= 2);

  Vector<float> len(positions.size());
  len[0] = 0.0f;
  for (const int i : IndexRange(1, positions.size() - 1)) {
    len[i] = len[i - 1] + math::distance(positions[i - 1], positions[i]);
  }
  const float total = len.last();

  r_positions.clear();
  r_radii.clear();
  if (total < 1e-6f) {
    r_positions.append(positions.first());
    r_positions.append(positions.last());
    r_radii.append(radii.first());
    r_radii.append(radii.last());
    return;
  }

  int seg = 0;
  for (int k = 0; k < target_count; k++) {
    const float s = total * float(k) / float(target_count - 1);
    while (seg < positions.size() - 2 && len[seg + 1] < s) {
      seg++;
    }
    const float seg_len = len[seg + 1] - len[seg];
    const float t = seg_len > 1e-8f ? (s - len[seg]) / seg_len : 0.0f;
    r_positions.append(math::interpolate(positions[seg], positions[seg + 1], t));
    r_radii.append(math::interpolate(radii[seg], radii[seg + 1], t));
  }
}

/* -------------------------------------------------------------------- */
/** \name RollSpline -- polyline arc-length spline on top of #CurvePatchSpline
 * \{ */

void RollSpline::clear()
{
  core.clear();
  poly_3d.clear();
  poly_2d.clear();
  lengths_2d.clear();
  lengths_3d.clear();
  tangents_3d.clear();
  pressures.clear();
  normals.clear();
}

bool RollSpline::is_empty() const
{
  return poly_3d.size() < 2;
}

float RollSpline::total_length_3d() const
{
  return core.total_length();
}

float RollSpline::total_length_2d() const
{
  return lengths_2d.is_empty() ? 0.0f : lengths_2d.last();
}

void RollSpline::update_lengths()
{
  const int n2d = int(poly_2d.size());
  const int n3d = int(poly_3d.size());
  if (n2d >= 2) {
    lengths_2d.reinitialize(length_parameterize::segments_num(n2d, false));
    length_parameterize::accumulate_lengths(poly_2d.as_span(), false, lengths_2d);
  }
  else {
    lengths_2d.clear();
  }
  if (n3d >= 2) {
    lengths_3d.reinitialize(length_parameterize::segments_num(n3d, false));
    length_parameterize::accumulate_lengths(poly_3d.as_span(), false, lengths_3d);

    /* Smooth tangents via central differences -- gives a C0 continuous tangent field instead of
     * the piecewise-constant direction per segment. */
    tangents_3d.reinitialize(n3d);
    tangents_3d[0] = math::normalize(poly_3d[1] - poly_3d[0]);
    for (int i = 1; i < n3d - 1; i++) {
      tangents_3d[i] = math::normalize(poly_3d[i + 1] - poly_3d[i - 1]);
    }
    tangents_3d[n3d - 1] = math::normalize(poly_3d[n3d - 1] - poly_3d[n3d - 2]);
  }
  else {
    lengths_3d.clear();
    tangents_3d.clear();
  }

  /* Geometric core for closest_point/evaluate delegation (keeps its own leading-zero lengths). */
  core.build_from_positions(poly_3d.as_span(), {});
}

float RollSpline::segment_length_3d(const int seg_idx) const
{
  return lengths_3d[seg_idx] - (seg_idx > 0 ? lengths_3d[seg_idx - 1] : 0.0f);
}

float3 RollSpline::evaluate_3d(const float s) const
{
  return core.evaluate(s);
}

float2 RollSpline::tangent_2d_at_index(int idx) const
{
  const int n = int(poly_2d.size());
  if (n < 2) {
    return float2(1.0f, 0.0f);
  }
  idx = std::clamp(idx, 0, n - 2);
  const float2 d = poly_2d[idx + 1] - poly_2d[idx];
  const float len = math::length(d);
  return (len > 1e-7f) ? d / len : float2(1.0f, 0.0f);
}

void RollSpline::closest_point_3d(const float3 &query,
                                  float &r_s,
                                  float3 &r_tan,
                                  float &r_dis) const
{
  core.closest_point_dist(query, r_s, r_tan, r_dis);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Roll Texture Mapping Stroke Helpers
 * \{ */

int PaintStroke::roll_max_points() const
{
  if (!need_roll_mapping_) {
    return 1;
  }

  float s = std::max(spacing_raw_, 0.05f);
  int tot = int(std::ceil(1.0f / s)) + 2;
  tot = std::max(tot, 5);
  /* 5x the point budget: keep a long tail for curve history, give more lookahead for
   * self-intersection detection on the inner side of turns, and provide extra deferral so dabs
   * are painted on stable spline data. */
  tot *= 5;
  return tot;
}

void PaintStroke::add_roll_point(const float2 &mouse_in,
                                 const float2 &mouse_out,
                                 const float3 &loc,
                                 const float3 &surface_normal,
                                 float size,
                                 float pressure,
                                 bool pen_flip,
                                 float x_tilt,
                                 float y_tilt)
{
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;

  if (need_roll_mapping_) {
    /* Budget: total knots (virtual backward + real) capped at
     * roll_max_points() + initial_backward_ext_count_. When the budget is exceeded we drop the
     * oldest knot:
     *
     *  Phase 1 -- Virtual knots still remain: drop the farthest backward extension point. No real
     *             data is lost.
     *  Phase 2 -- All virtual knots consumed: the ring buffer wraps, dropping the oldest real
     *             point.
     *
     * In both cases we accumulate the removed segment's arc length into stroke_distance_world_
     * (raw) and stroke_distance_normalized_ (pressure-weighted, for the pressure-scale feature).
     * These offsets are subtracted in spline_uv() so the texture V coordinate stays continuous
     * despite the shrinking polyline. */
    const int budget = roll_max_points() + initial_backward_ext_count_;
    const int total_knots = int(backward_ext_2d_.size()) + num_points_;
    bool wrap_ring = false;

    if (total_knots >= budget) {
      if (!backward_ext_2d_.is_empty()) {
        /* Consume the oldest virtual knot. The ring buffer keeps growing; no real stroke data is
         * lost yet. Accumulate the arc-length of the removed span so that stroke_distance_world_
         * compensates and the texture stays put. */
        float seg_len = 0.0f;
        if (!roll_spline_.lengths_3d.is_empty()) {
          seg_len = roll_spline_.segment_length_3d(0);
        }
        else if (backward_ext_3d_.size() >= 2) {
          seg_len = math::distance(backward_ext_3d_[0], backward_ext_3d_[1]);
        }
        stroke_distance_world_ += seg_len;
        /* Normalized distance: use the average pressure of the consumed segment's two endpoints,
         * matching the grid V computation. */
        if (roll_initial_radius_ > 0.0f) {
          const float p0 = (!roll_spline_.pressures.is_empty()) ? roll_spline_.pressures[0] : 1.0f;
          const float p1 = (roll_spline_.pressures.size() > 1) ? roll_spline_.pressures[1] : p0;
          const float p_avg = std::max((p0 + p1) * 0.5f, 0.01f);
          stroke_distance_normalized_ += seg_len / (roll_initial_radius_ * p_avg);
        }
        backward_ext_2d_.remove(0);
        backward_ext_3d_.remove(0);
      }
      else {
        /* All virtual knots consumed -- ring buffer wraps, oldest real point is abandoned.
         * Accumulate its distance so stroke_distance_world_ stays correct. */
        const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
        const int next_idx = (oldest_idx + 1) % buf_cap;
        const float seg_len = math::distance(points_[oldest_idx].location,
                                             points_[next_idx].location);
        stroke_distance_world_ += seg_len;
        if (roll_initial_radius_ > 0.0f) {
          const float p_avg = std::max(
              (points_[oldest_idx].pressure + points_[next_idx].pressure) * 0.5f, 0.01f);
          stroke_distance_normalized_ += seg_len / (roll_initial_radius_ * p_avg);
        }
        wrap_ring = true;
      }
    }

    PaintStrokePoint *point = &points_[cur_point_];
    point->size = size;
    point->mouse_in = mouse_in;
    point->mouse_out = mouse_out;
    point->x_tilt = x_tilt;
    point->y_tilt = y_tilt;
    point->pen_flip = pen_flip;

    point->location = loc;
    point->surface_normal = (math::length_squared(surface_normal) > 1e-8f) ?
                                math::normalize(surface_normal) :
                                float3(0.0f, 0.0f, 1.0f);
    /* Clamp pressure -- pen touch-down and lift-off report near-zero values that would create an
     * extreme width spike in the roll strip. */
    point->pressure = std::max(pressure, MIN_ROLL_PRESSURE);

    cur_point_ = (cur_point_ + 1) % buf_cap;
    if (!wrap_ring) {
      num_points_++;
    }
  }
  else {
    /* Non-roll: simple ring buffer wrapping at PAINT_MAX_INPUT_SAMPLES. */
    PaintStrokePoint *point = &points_[cur_point_];
    point->size = size;
    point->mouse_in = mouse_in;
    point->mouse_out = mouse_out;
    point->x_tilt = x_tilt;
    point->y_tilt = y_tilt;
    point->pen_flip = pen_flip;

    point->location = loc;
    point->pressure = pressure;

    cur_point_ = (cur_point_ + 1) % buf_cap;
    if (num_points_ < buf_cap) {
      num_points_++;
    }
  }
}

/**
 * Generate virtual extension points that continue the stroke's curvature beyond its start or end.
 * Used for both backward (pre-stroke) and forward (post-stroke) extensions so the spline has
 * coverage for the full brush footprint at the stroke boundaries.
 *
 * \param anchor_2d, anchor_3d: The point to extend from.
 * \param dir_2d, dir_3d: Unit direction of the stroke at the anchor.
 * \param seg1_2d, seg2_2d, seg1_3d, seg2_3d: Two consecutive segments at the anchor end, used to
 *   measure curvature. May be zero-length if < 3 points.
 * \param n_points: Number of extension points to generate.
 * \param step_2d, step_3d: Spacing between consecutive extension points.
 * \param sign: +1.0f for forward, -1.0f for backward.
 * \param r_ext_2d, r_ext_3d: Output arrays (appended in walk order, i.e. nearest-to-anchor first
 *   for forward, farthest-from-anchor first for backward).
 */
static void generate_virtual_extension(const float2 &anchor_2d,
                                       const float3 &anchor_3d,
                                       const float2 &dir_2d,
                                       const float3 &dir_3d,
                                       const float2 &seg1_2d,
                                       const float2 &seg2_2d,
                                       const float3 &seg1_3d,
                                       const float3 &seg2_3d,
                                       const int n_points,
                                       const float step_2d,
                                       const float step_3d,
                                       const float sign,
                                       Vector<float2> &r_ext_2d,
                                       Vector<float3> &r_ext_3d)
{
  /* Measure curvature from the angle between the two segments. Each virtual step rotates the
   * direction by a proportional amount so the extension follows the same arc the user started
   * drawing. */
  float angle_2d = 0.0f;
  float angle_3d = 0.0f;
  float3 rot_axis = float3(0);
  bool has_rot_3d = false;

  {
    const float cross2 = seg1_2d.x * seg2_2d.y - seg1_2d.y * seg2_2d.x;
    const float dot2 = math::dot(seg1_2d, seg2_2d);
    const float total_a2 = atan2f(cross2, dot2);
    const float ref_len2 = math::length(seg1_2d);
    if (ref_len2 > 1e-7f) {
      angle_2d = total_a2 * (step_2d / ref_len2);
    }

    const float3 n1 = math::normalize(seg1_3d);
    const float3 n2 = math::normalize(seg2_3d);
    const float3 c3 = math::cross(n1, n2);
    const float c3_len = math::length(c3);
    const float d3 = math::dot(n1, n2);
    if (c3_len > 1e-7f) {
      rot_axis = c3 / c3_len;
      const float total_a3 = atan2f(c3_len, d3);
      const float ref_len3 = math::length(seg1_3d);
      if (ref_len3 > 1e-7f) {
        angle_3d = total_a3 * (step_3d / ref_len3);
        has_rot_3d = true;
      }
    }
  }

  /* Clamp per-step angle so total curvature over all extension points stays within 90 degrees.
   * Without this, tight turns can spiral into loops. */
  const float max_per_step = float(M_PI_2) / float(std::max(1, n_points));
  angle_2d = std::clamp(angle_2d, -max_per_step, max_per_step);
  if (has_rot_3d) {
    angle_3d = std::clamp(angle_3d, -max_per_step, max_per_step);
  }

  /* Walk from anchor, rotating direction each step. */
  float2 cur_2d = dir_2d;
  float3 cur_3d = dir_3d;
  float2 prev_2d = anchor_2d;
  float3 prev_3d = anchor_3d;

  /* For backward extensions we build into a temporary array and reverse, so the output is ordered
   * farthest->nearest (matching the expected prepend order where index 0 is the farthest virtual
   * knot). */
  const bool backward = (sign < 0.0f);
  const int old_size = int(r_ext_2d.size());

  for (int i = 0; i < n_points; i++) {
    /* Rotate direction before stepping. */
    const float a2 = sign * angle_2d;
    if (a2 != 0.0f) {
      const float c = cosf(a2);
      const float s = sinf(a2);
      cur_2d = float2(cur_2d.x * c - cur_2d.y * s, cur_2d.x * s + cur_2d.y * c);
    }
    if (has_rot_3d) {
      const float a3 = sign * angle_3d;
      const float c = cosf(a3);
      const float s = sinf(a3);
      const float3 kxv = math::cross(rot_axis, cur_3d);
      const float kdv = math::dot(rot_axis, cur_3d);
      cur_3d = cur_3d * c + kxv * s + rot_axis * kdv * (1.0f - c);
    }

    prev_2d = prev_2d + cur_2d * (sign * step_2d);
    prev_3d = prev_3d + cur_3d * (sign * step_3d);
    r_ext_2d.append(prev_2d);
    r_ext_3d.append(prev_3d);
  }

  if (backward) {
    /* Reverse so output goes farthest-from-anchor -> nearest. */
    std::reverse(r_ext_2d.begin() + old_size, r_ext_2d.end());
    std::reverse(r_ext_3d.begin() + old_size, r_ext_3d.end());
  }
}

void PaintStroke::prepend_virtual_roll_points()
{
  if (num_points_ < 2) {
    return;
  }

  constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
  const int oldest = (cur_point_ - num_points_ + cap) % cap;
  const int next_oldest = (oldest + 1) % cap;

  const float3 p_start = points_[oldest].location;
  const float2 m_start = points_[oldest].mouse_out;

  float3 dir3 = points_[next_oldest].location - p_start;
  float2 dir2 = points_[next_oldest].mouse_out - m_start;
  const float len3 = math::length(dir3);
  const float len2 = math::length(dir2);
  if (len3 < 1e-7f || len2 < 1e-7f) {
    return;
  }
  dir3 /= len3;
  dir2 /= len2;

  /* Use the full (unpressured) brush radius for step sizes so the virtual extension isn't
   * compressed when the initial pen touch has low pressure. */
  const float base_pixel_r = (paint && brush) ? BKE_brush_radius_get(paint, brush) : 50.0f;
  const float r_world = paint_calc_object_space_radius(vc, p_start, base_pixel_r);
  const float r_screen = base_pixel_r;

  const int n_backward = VIRTUAL_EXTENSION_POINTS;
  const float step_3d = (r_world * 1.5f) / float(n_backward);
  const float step_2d = (r_screen * 1.5f) / float(n_backward);

  /* Curvature segments (zero-length if < 3 points). */
  float2 seg1_2d(0), seg2_2d(0);
  float3 seg1_3d(0), seg2_3d(0);
  if (num_points_ >= 3) {
    const int third = (oldest + 2) % cap;
    seg1_2d = points_[next_oldest].mouse_out - m_start;
    seg2_2d = points_[third].mouse_out - points_[next_oldest].mouse_out;
    seg1_3d = points_[next_oldest].location - p_start;
    seg2_3d = points_[third].location - points_[next_oldest].location;
  }

  backward_ext_2d_.clear();
  backward_ext_3d_.clear();
  generate_virtual_extension(m_start,
                             p_start,
                             dir2,
                             dir3,
                             seg1_2d,
                             seg2_2d,
                             seg1_3d,
                             seg2_3d,
                             n_backward,
                             step_2d,
                             step_3d,
                             -1.0f,
                             backward_ext_2d_,
                             backward_ext_3d_);
  initial_backward_ext_count_ = n_backward;
}

void PaintStroke::make_roll_spline(bContext * /*C*/)
{
  if (num_points_ < 4) {
    return;
  }

  if (!roll_virtual_prepended_) {
    roll_virtual_prepended_ = true;
    prepend_virtual_roll_points();
  }

  /* Collect real knots from ring buffer. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;

  const int oldest = (cur_point_ - num_points_ + buf_cap) % buf_cap;

  /* Combine backward extension + real knots directly as polyline. No curve evaluation -- the
   * grid/CC/LUT surface algorithm handles smoothing at a higher level. */
  roll_spline_.clear();
  roll_spline_.poly_2d.extend(backward_ext_2d_);
  roll_spline_.poly_3d.extend(backward_ext_3d_);
  /* Extrapolate pressure trend for backward virtual extension. Derive a geometric ratio from the
   * 2nd/3rd real points (skipping the 1st which may have an unreliable initial-touch pressure),
   * then multiply backward so the strip width transitions smoothly. */
  {
    const int n_back = int(backward_ext_2d_.size());
    float base_p = (num_points_ > 0) ? points_[oldest].pressure : 1.0f;
    float ratio = 1.0f;
    if (num_points_ >= 3) {
      const int i1 = (oldest + 1) % buf_cap;
      const int i2 = (oldest + 2) % buf_cap;
      const float p1 = points_[i1].pressure;
      const float p2 = points_[i2].pressure;
      if (p2 > 0.01f) {
        ratio = std::clamp(p1 / p2, PRESSURE_RATIO_MIN, PRESSURE_RATIO_MAX);
      }
    }
    extrapolate_pressures(base_p, ratio, n_back, true, roll_spline_.pressures);
    /* Virtual backward extension: use oldest real point's surface normal. */
    const float3 back_n = (num_points_ > 0) ? points_[oldest].surface_normal :
                                              float3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < n_back; i++) {
      roll_spline_.normals.append(back_n);
    }
  }
  for (int i = 0; i < num_points_; i++) {
    const int idx = (oldest + i) % buf_cap;
    roll_spline_.poly_2d.append(points_[idx].mouse_out);
    roll_spline_.poly_3d.append(points_[idx].location);
    roll_spline_.pressures.append(points_[idx].pressure);
    roll_spline_.normals.append(points_[idx].surface_normal);
  }

  n_virtual_poly_points_ = int(backward_ext_2d_.size());

  roll_spline_.update_lengths();

  /* Arc length of virtual extension -- subtracted from V so V=0 at mouse-down. Computed only once
   * (when the full virtual extension is first available). As virtual points are consumed,
   * stroke_distance_world_ compensates; updating roll_virtual_length_ here would cause
   * double-counting. */
  if (roll_virtual_length_ == 0.0f && n_virtual_poly_points_ > 0 &&
      n_virtual_poly_points_ - 1 < int(roll_spline_.lengths_3d.size()))
  {
    roll_virtual_length_ = roll_spline_.lengths_3d[n_virtual_poly_points_ - 1];
    /* Compute normalized virtual length for pressure-scaled V offset. This is the
     * pressure-weighted equivalent of roll_virtual_length_: sum of seg_len / (R * p_avg) over the
     * virtual extension. It must use the same midpoint-pressure formula as the grid V coords and
     * as stroke_distance_normalized_ in add_roll_point(), so the subtraction in spline_uv()
     * cancels exactly. */
    if (roll_initial_radius_ > 0.0f) {
      const Span<float> pres = roll_spline_.pressures.as_span();
      float accum = 0.0f;
      for (int i = 1; i <= n_virtual_poly_points_ - 1 &&
                      i < int(roll_spline_.lengths_3d.size());
           i++)
      {
        const float seg = roll_spline_.segment_length_3d(i);
        const float p = (i < int(pres.size()) && i - 1 >= 0) ?
                            std::max((pres[i] + pres[i - 1]) * 0.5f, 0.01f) :
                            1.0f;
        accum += seg / (roll_initial_radius_ * p);
      }
      roll_virtual_length_normalized_ = accum;
    }
  }
}

void PaintStroke::finish_roll_stroke(bContext *C,
                                     wmOperator *op,
                                     const float2 &mouse_up,
                                     float pressure)
{
  if (!need_roll_mapping_ || num_points_ < 4) {
    return;
  }

  /* Clamp pen lift-off pressure (same minimum as add_roll_point) to avoid a width spike on the
   * last recorded point. */
  pressure = std::max(pressure, MIN_ROLL_PRESSURE);

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(this->paint);
  bke::PaintRuntime *paint_runtime = this->paint->runtime;

  /* 1. Skip space_stroke at finish -- with a low brush spacing setting and near-zero pen-lift
   *    pressure, it can spawn thousands of expensive roll dabs (each rebuilds spline, grid, LUT,
   *    and paints). The mouse-up position is force-recorded directly in step 2 below and deferred
   *    dabs are flushed in step 4, so the stroke still ends at the correct position -- only the
   *    per-dab fill between the last mouse-move and the lift is dropped. */

  /* 2. Force-record the exact mouse-up position as a roll point (even if it doesn't fall on a
   *    spacing boundary). */
  {
    float2 mouse_out = paint_stroke_jitter_pos(
        this->paint, mode, brush, pressure, stroke_mode_, zoom_2d_, mouse_up);
    float3 location;
    bool is_location_is_set;
    update(C, brush, mode, mouse_up, mouse_out, pressure, location, &is_location_is_set);
    /* Use last point's surface normal for the finish point. */
    const int last_idx = (cur_point_ - 1 + PAINT_MAX_INPUT_SAMPLES) % PAINT_MAX_INPUT_SAMPLES;
    const float3 last_sn = (num_points_ > 0) ? points_[last_idx].surface_normal :
                                               float3(0.0f, 0.0f, 1.0f);

    if (is_location_is_set) {
      add_roll_point(mouse_up,
                     mouse_out,
                     location,
                     last_sn,
                     paint_runtime->pixel_radius,
                     pressure,
                     pen_flip_,
                     tilt_.x,
                     tilt_.y);
    }
    else if (num_points_ > 0) {
      /* Fallback: use last known location so the spline still extends. */
      add_roll_point(mouse_up,
                     mouse_out,
                     points_[last_idx].location,
                     last_sn,
                     points_[last_idx].size,
                     pressure,
                     pen_flip_,
                     tilt_.x,
                     tilt_.y);
    }
    make_roll_spline(C);
  }
  /* 3. Append a virtual forward extension so the last dab has spline coverage for the brush half
   *    that extends beyond the stroke end. Uses the same curvature-following logic as
   *    prepend_virtual_roll_points() but in the forward direction, appending directly to the
   *    polyline. */
  if (num_points_ >= 2 && !roll_spline_.is_empty()) {
    constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
    const int newest = (cur_point_ - 1 + cap) % cap;
    const int prev_newest = (newest - 1 + cap) % cap;

    const float3 p_end = points_[newest].location;
    const float2 m_end = points_[newest].mouse_out;

    float3 dir3 = p_end - points_[prev_newest].location;
    float2 dir2 = m_end - points_[prev_newest].mouse_out;
    const float len3 = math::length(dir3);
    const float len2 = math::length(dir2);

    if (len3 > 1e-7f && len2 > 1e-7f) {
      dir3 /= len3;
      dir2 /= len2;

      const float base_pixel_r = paint ? BKE_brush_radius_get(paint, &brush) : 50.0f;
      const float r_world = paint_calc_object_space_radius(vc, p_end, base_pixel_r);
      const float r_screen = base_pixel_r;

      const int n_forward = VIRTUAL_EXTENSION_POINTS;
      const float step_3d = (r_world * 1.5f) / float(n_forward);
      const float step_2d = (r_screen * 1.5f) / float(n_forward);

      float2 seg1_2d(0), seg2_2d(0);
      float3 seg1_3d(0), seg2_3d(0);
      if (num_points_ >= 3) {
        const int prev2 = (prev_newest - 1 + cap) % cap;
        seg1_2d = points_[prev_newest].mouse_out - points_[prev2].mouse_out;
        seg2_2d = m_end - points_[prev_newest].mouse_out;
        seg1_3d = points_[prev_newest].location - points_[prev2].location;
        seg2_3d = p_end - points_[prev_newest].location;
      }

      generate_virtual_extension(m_end,
                                 p_end,
                                 dir2,
                                 dir3,
                                 seg1_2d,
                                 seg2_2d,
                                 seg1_3d,
                                 seg2_3d,
                                 n_forward,
                                 step_2d,
                                 step_3d,
                                 +1.0f,
                                 roll_spline_.poly_2d,
                                 roll_spline_.poly_3d);
      /* Extrapolate pressure trend for forward virtual extension. */
      {
        const float p_new = points_[newest].pressure;
        const float p_prev = points_[prev_newest].pressure;
        float ratio = 1.0f;
        if (p_prev > 0.01f) {
          ratio = std::clamp(p_new / p_prev, PRESSURE_RATIO_MIN, PRESSURE_RATIO_MAX);
        }
        extrapolate_pressures(p_new, ratio, n_forward, false, roll_spline_.pressures);
      }
      /* Forward extension: use newest point's surface normal. */
      for (int i = 0; i < n_forward; i++) {
        roll_spline_.normals.append(points_[newest].surface_normal);
      }

      roll_spline_.update_lengths();
    }
  }

  /* 4. Flush deferred dabs forward to the stroke endpoint.
   *
   * During normal painting, dabs lag behind the mouse by `half` points so the spline has enough
   * future context for smooth UV mapping. At mouse-up we flush all remaining deferred dabs from
   * the last painted position to the newest recorded point.
   *
   * flush_start comes from one of three cases:
   *  (a) last_painted_roll_idx_ exists -- continue from there.
   *  (b) Very short stroke (num_points <= half) -- start from oldest point.
   *  (c) Normal stroke, no dabs placed yet -- start from the deferred position
   *      (cur_point_ - half). */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  const int half = roll_half_points();
  const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;

  int flush_start;
  if (last_painted_roll_idx_ >= 0) {
    flush_start = last_painted_roll_idx_;
  }
  else if (num_points_ <= half) {
    flush_start = oldest_idx;
  }
  else {
    flush_start = (cur_point_ - half + buf_cap) % buf_cap;
  }

  /* Advance one step at a time toward (but not including) the newest point. The newest entry is
   * the force-recorded mouse-up position which may sit arbitrarily close to the previous point --
   * including it would create an overlapping dab at the stroke end. */
  const int newest = (cur_point_ - 1 + buf_cap) % buf_cap;
  int idx = (flush_start + 1) % buf_cap;

  while (idx != newest) {
    PaintStrokePoint *point = &points_[idx];

    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "stroke", &itemptr);
    RNA_float_set(&itemptr, "size", point->size);
    RNA_float_set_array(&itemptr, "location", point->location);
    RNA_float_set_array(&itemptr, "mouse", point->mouse_out);
    RNA_float_set_array(&itemptr, "mouse_event", point->mouse_in);
    RNA_float_set(&itemptr, "pressure", point->pressure);
    RNA_float_set(&itemptr, "x_tilt", point->x_tilt);
    RNA_float_set(&itemptr, "y_tilt", point->y_tilt);

    this->update_step(op, &itemptr);
    RNA_collection_clear(op->ptr, "stroke");

    tot_samples_++;
    idx = (idx + 1) % buf_cap;
  }
}

/**
 * Standard Catmull-Clark subdivision on a rows x cols quad grid. Produces (2*rows-1) x (2*cols-1)
 * output. Border columns (c=0, c=cols-1) are pinned to preserve self-intersection collapse
 * geometry. All other vertices use standard CC rules:
 *   - Interior: full CC vertex rule (Q + 2R + V) / 4
 *   - Boundary rows: 1/8 cubic B-spline rule along the row
 */
static void catmull_clark_subdivide_grid(Vector<float3> &grid_pos,
                                         Vector<float2> &grid_uv,
                                         int &rows,
                                         int &cols)
{
  const int oR = rows, oC = cols;
  const int nR = 2 * oR - 1;
  const int nC = 2 * oC - 1;
  const int fR = oR - 1;
  const int fC = oC - 1;

  auto oi = [oC](int r, int c) { return r * oC + c; };
  auto ni = [nC](int r, int c) { return r * nC + c; };

  /* Face points (avg of 4 quad corners). */
  Vector<float3> fp(fR * fC);
  Vector<float2> fu(fR * fC);
  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < fC; c++) {
      fp[r * fC + c] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                grid_pos[oi(r + 1, c)] + grid_pos[oi(r + 1, c + 1)]);
      fu[r * fC + c] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                grid_uv[oi(r + 1, c)] + grid_uv[oi(r + 1, c + 1)]);
    }
  }

  Vector<float3> np(nR * nC);
  Vector<float2> nu(nR * nC);

  /* Place face points at odd,odd positions. */
  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < fC; c++) {
      np[ni(2 * r + 1, 2 * c + 1)] = fp[r * fC + c];
      nu[ni(2 * r + 1, 2 * c + 1)] = fu[r * fC + c];
    }
  }

  /* Horizontal edge points (even row, odd col). Boundary rows: midpoint. Interior: avg of
   * endpoints + face points. */
  for (int r = 0; r < oR; r++) {
    for (int c = 0; c < fC; c++) {
      if (r == 0 || r == oR - 1) {
        np[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)]);
        nu[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)]);
      }
      else {
        np[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                            fp[(r - 1) * fC + c] + fp[r * fC + c]);
        nu[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                            fu[(r - 1) * fC + c] + fu[r * fC + c]);
      }
    }
  }

  /* Vertical edge points (odd row, even col). Border columns: midpoint (preserves border
   * polyline). Interior: avg of endpoints + face points. */
  for (int r = 0; r < fR; r++) {
    for (int c = 0; c < oC; c++) {
      if (c == 0 || c == oC - 1) {
        np[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)]);
        nu[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r + 1, c)]);
      }
      else {
        np[ni(2 * r + 1, 2 * c)] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)] +
                                            fp[r * fC + c - 1] + fp[r * fC + c]);
        nu[ni(2 * r + 1, 2 * c)] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r + 1, c)] +
                                            fu[r * fC + c - 1] + fu[r * fC + c]);
      }
    }
  }

  /* Vertex points (even row, even col). Border cols: pinned. Boundary rows: 1/8 rule. Interior:
   * full CC. */
  for (int r = 0; r < oR; r++) {
    for (int c = 0; c < oC; c++) {
      if (c == 0 || c == oC - 1) {
        /* Border column: pinned (preserves self-intersection collapse). */
        np[ni(2 * r, 2 * c)] = grid_pos[oi(r, c)];
        nu[ni(2 * r, 2 * c)] = grid_uv[oi(r, c)];
      }
      else if (r == 0 || r == oR - 1) {
        /* Boundary row, interior col: 1/8 rule along row. */
        np[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_pos[oi(r, c - 1)] + 6.0f * grid_pos[oi(r, c)] +
                                                grid_pos[oi(r, c + 1)]);
        nu[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_uv[oi(r, c - 1)] + 6.0f * grid_uv[oi(r, c)] +
                                                grid_uv[oi(r, c + 1)]);
      }
      else {
        /* Interior: full CC vertex rule. n=4: V_new = (Q + 2R + V) / 4. */
        const float3 Q = 0.25f * (fp[(r - 1) * fC + (c - 1)] + fp[(r - 1) * fC + c] +
                                  fp[r * fC + (c - 1)] + fp[r * fC + c]);
        const float2 Qu = 0.25f * (fu[(r - 1) * fC + (c - 1)] + fu[(r - 1) * fC + c] +
                                   fu[r * fC + (c - 1)] + fu[r * fC + c]);
        const float3 R = 0.25f * (0.5f * (grid_pos[oi(r, c - 1)] + grid_pos[oi(r, c)]) +
                                  0.5f * (grid_pos[oi(r, c + 1)] + grid_pos[oi(r, c)]) +
                                  0.5f * (grid_pos[oi(r - 1, c)] + grid_pos[oi(r, c)]) +
                                  0.5f * (grid_pos[oi(r + 1, c)] + grid_pos[oi(r, c)]));
        const float2 Ru = 0.25f * (0.5f * (grid_uv[oi(r, c - 1)] + grid_uv[oi(r, c)]) +
                                   0.5f * (grid_uv[oi(r, c + 1)] + grid_uv[oi(r, c)]) +
                                   0.5f * (grid_uv[oi(r - 1, c)] + grid_uv[oi(r, c)]) +
                                   0.5f * (grid_uv[oi(r + 1, c)] + grid_uv[oi(r, c)]));
        np[ni(2 * r, 2 * c)] = (Q + 2.0f * R + grid_pos[oi(r, c)]) / 4.0f;
        nu[ni(2 * r, 2 * c)] = (Qu + 2.0f * Ru + grid_uv[oi(r, c)]) / 4.0f;
      }
    }
  }

  grid_pos = std::move(np);
  grid_uv = std::move(nu);
  rows = nR;
  cols = nC;
}

/* What the four stages of #PaintStroke::compute_roll_center hand to each other. Everything here is
 * derived from the dab and the stored polyline; nothing in it outlives the call. */
struct RollCenterBuild {
  /** Arc length of this dab's closest point on the polyline. */
  float raw_s = 0.0f;
  /** Polyline vertex count -- the strip is built over the WHOLE stroke, so this is all of them. */
  int count = 0;
  /** Frozen brush radius of the stroke. */
  float R = 0.0f;
  bool use_pressure_width = false;
  /** Stable projection plane, frozen on the first dab. */
  float3 proj_normal = float3(0.0f, 0.0f, 1.0f);
  /** Per-dab surface normal: what the LUT projects along and what the borders are built in. */
  float3 dab_normal = float3(0.0f, 0.0f, 1.0f);
};

/* Stage 1: where this dab sits on the stored polyline, and which stretch of it the per-vertex UV
 * lookup has to search. Returns false for an empty spline, with `roll_center_s` marked unusable. */
static bool roll_center_locate_on_spline(const RollSpline &spline,
                                         StrokeCache &cache,
                                         RollCenterBuild &build)
{
  if (spline.is_empty()) {
    cache.roll_center_s = -1.0f;
    return false;
  }
  float raw_s, dis;
  float3 tan;
  /* Full closest-point search, called once per dab on the main thread. */
  spline.closest_point_3d(cache.location, raw_s, tan, dis);
  cache.roll_center_s = raw_s;
  cache.roll_center_pos = spline.evaluate_3d(raw_s);
  cache.roll_tangent = tan;
  build.raw_s = raw_s;

  /* Precompute polyline segment search range for per-vertex UV lookups. */
  const Span<float> lengths = spline.lengths_3d.as_span();
  const int n_segs = int(spline.poly_3d.size()) - 1;

  const float search_r = cache.initial_radius * 3.0f;
  const float s_lo = std::max(0.0f, raw_s - search_r);
  /* Extend forward to include the newest stroke points (trailing segment). This ensures the grid
   * covers the area between the dab and the mouse, which is needed for proper self-intersection
   * detection and UV mapping near the stroke tip. */
  const float s_hi = std::min(lengths.last(), raw_s + search_r * 2.0f);

  float fac;
  length_parameterize::sample_at_length(lengths, s_lo, cache.roll_seg_lo, fac);
  length_parameterize::sample_at_length(lengths, s_hi, cache.roll_seg_hi, fac);
  cache.roll_seg_hi = std::min(cache.roll_seg_hi, n_segs - 1);

  length_parameterize::sample_at_length(lengths, raw_s, cache.roll_center_seg, fac);
  return true;
}

/* Stage 2: surface interpolation groundwork. Each polyline vertex gets a binormal (perpendicular
 * to the tangent in the projection plane) and left/right border points at +/- brush radius.
 *
 * `proj_normal_state` and `initial_radius_state` are the stroke's own frozen values, updated here
 * on the first dab and read back on every later one. */
static void roll_borders_build(const RollSpline &spline,
                               const Brush *brush,
                               float3 &proj_normal_state,
                               float &initial_radius_state,
                               StrokeCache &cache,
                               RollCenterBuild &build)
{
  {
    const Span<float3> poly = spline.poly_3d.as_span();
    const Span<float3> tangents = spline.tangents_3d.as_span();
    /* Build the grid over the FULL stored polyline so that self-intersections anywhere on the
     * stroke are properly detected. The per-vertex search in spline_uv() is limited to rows near
     * the dab for performance. */
    const int lo = 0;
    const int count = int(poly.size());
    const bool use_pressure_width = brush && brush->roll_pressure_scale &&
                                    BKE_brush_use_size_pressure(brush);
    const float R = cache.initial_radius;
    build.count = count;
    build.use_pressure_width = use_pressure_width;
    build.R = R;

    /* Capture initial_radius for normalized distance tracking. */
    if (initial_radius_state == 0.0f && R > 0.0f) {
      initial_radius_state = R;
    }

    /* Use a stable projection normal for binormal computation and 2D projection. Frozen on the
     * first dab so that border positions and self-intersection merges don't flicker as the brush
     * moves across a curved surface (where sculpt_normal changes per dab). */
    if (math::length_squared(proj_normal_state) < 1e-8f) {
      proj_normal_state = (math::length_squared(cache.sculpt_normal) > 1e-8f) ?
                              math::normalize(cache.sculpt_normal) :
                              math::normalize(cache.view_normal);
    }
    const float3 proj_normal = proj_normal_state;
    cache.roll_proj_normal = proj_normal;
    build.proj_normal = proj_normal;

    cache.roll_binormals.reinitialize(count);
    cache.roll_border_left.reinitialize(count);
    cache.roll_border_right.reinitialize(count);

    /* dab_normal: per-dab surface normal for the LUT projection. */
    const float3 dab_normal = (math::length_squared(cache.sculpt_normal) > 1e-8f) ?
                                  math::normalize(cache.sculpt_normal) :
                                  proj_normal;
    build.dab_normal = dab_normal;

    for (int k = 0; k < count; k++) {
      const int vi = lo + k; /* polyline vertex index */
      const float3 &T = tangents[vi];
      /* Binormal = cross(tangent, dab_normal). Using the per-dab surface normal (same as LUT
       * projection) ensures the grid is aligned with the local surface at the current dab
       * position. This is view-independent -- the same stroke from any camera angle produces the
       * same borders. */
      float3 B = math::cross(T, dab_normal);
      const float blen = math::length(B);
      if (blen > 1e-7f) {
        B /= blen;
      }
      else {
        /* Degenerate: tangent parallel to dab_normal. */
        B = math::normalize(math::cross(T, float3(0, 0, 1)));
        if (math::length_squared(B) < 1e-12f) {
          B = math::normalize(math::cross(T, float3(1, 0, 0)));
        }
      }
      cache.roll_binormals[k] = B;
      const float Rk = (use_pressure_width && vi < int(spline.pressures.size())) ?
                           std::max(R * spline.pressures[vi], R * 0.01f) :
                           R;
      cache.roll_border_left[k] = poly[vi] + B * Rk;
      cache.roll_border_right[k] = poly[vi] - B * Rk;
    }
  }
}

/* Stage 3: the border curves at distance R from the center may self-intersect on the inner side of
 * tight turns, where the radius of curvature is smaller than R. Find where each border polyline
 * actually crosses itself and collapse the loop onto that crossing.
 *
 * Deliberately unchanged from the inline version it was lifted out of: the collapse rules here --
 * the 20% validation margin, the 3R partial-collapse budget, the apex search -- are tuned, not
 * derived, and each one was arrived at against a specific artifact. */
static void roll_borders_fix_self_intersections(const RollSpline &spline,
                                                const RollCenterBuild &build,
                                                StrokeCache &cache)
{
  {
    const Span<float3> poly = spline.poly_3d.as_span();
    const int count = build.count;
    const float R = build.R;
    const float3 dab_normal = build.dab_normal;

    /* Fix inner-border self-intersections at sharp turns.
     *
     * The border curves (at distance R from center) may self-intersect on the inner side of tight
     * turns where the radius of curvature < R. We find where the border polyline actually crosses
     * itself (segment vs segment test), then collapse all loop vertices to that crossing point.
     * This turns overlapping quads into degenerate triangles that fan from the crossing point --
     * the cross-lines rotate to point toward it. */

    /* Find border polyline self-intersections via 2D segment tests.
     *
     * Uses the per-dab surface normal for projection so the merge detection is view-independent
     * and aligned with the LUT. Border stability comes from the per-vertex frozen surface normals
     * used for binormal computation, not from the merge projection normal. */
    float3 merge_x = math::cross(dab_normal, float3(0, 0, 1));
    if (math::length_squared(merge_x) < 1e-6f) {
      merge_x = math::cross(dab_normal, float3(1, 0, 0));
    }
    merge_x = math::normalize(merge_x);
    const float3 merge_y = math::normalize(math::cross(dab_normal, merge_x));

    auto fix_border_self_intersections = [count, &merge_x, &merge_y, &poly, R](
                                             Vector<float3> &border) {
      if (count < 4) {
        return;
      }

      Vector<float2> b2d(count);
      for (int k = 0; k < count; k++) {
        b2d[k] = float2(math::dot(border[k], merge_x), math::dot(border[k], merge_y));
      }

      const int max_loop = count - 1;
      Vector<float3> orig(border);
      int skip_until = -1;

      for (int i = 0; i < count - 1; i++) {
        if (i <= skip_until) {
          continue;
        }
        const float2 d1 = b2d[i + 1] - b2d[i];
        if (math::dot(d1, d1) < 1e-8f) {
          continue;
        }

        const int j_max = std::min(count - 2, i + max_loop);
        for (int j = j_max; j >= i + 2; j--) {
          const float2 d2 = b2d[j + 1] - b2d[j];
          if (math::dot(d2, d2) < 1e-8f) {
            continue;
          }

          const float2 ac = b2d[j] - b2d[i];
          const float denom = d1.x * d2.y - d1.y * d2.x;
          if (std::abs(denom) < 1e-8f) {
            continue;
          }
          const float s = (ac.x * d2.y - ac.y * d2.x) / denom;
          const float t = (ac.x * d1.y - ac.y * d1.x) / denom;

          if (s >= 0.0f && s <= 1.0f && t >= 0.0f && t <= 1.0f) {
            bool all_inside = true;
            const int m_lo = std::max(0, i - 2);
            const int m_hi = std::min(count - 2, j + 2);
            const float R_sq = (R * 1.2f) * (R * 1.2f); /* 20% margin for inclined borders */
            /* Validate all loop vertices are within R of center. Also find the apex = vertex with
             * sharpest curvature on the border (largest turning angle between consecutive
             * segments). */
            int apex_idx = (i + 1 + j) / 2; /* default to midpoint */
            float max_turn_angle = -1.0f;
            for (int k = i + 1; k <= j; k++) {
              float min_dist_sq = FLT_MAX;
              for (int m = m_lo; m <= m_hi; m++) {
                const float3 ab = poly[m + 1] - poly[m];
                const float ab_dot = math::dot(ab, ab);
                const float tp = (ab_dot > 1e-12f) ?
                                     std::clamp(math::dot(orig[k] - poly[m], ab) / ab_dot,
                                                0.0f,
                                                1.0f) :
                                     0.0f;
                const float3 proj = math::interpolate(poly[m], poly[m + 1], tp);
                min_dist_sq = std::min(min_dist_sq, math::distance_squared(orig[k], proj));
              }
              if (min_dist_sq >= R_sq) {
                all_inside = false;
                break;
              }
              /* Curvature: angle between incoming and outgoing border segments at vertex k (in 2D
               * projection). Higher = sharper turn. */
              if (k > i + 1 && k < j) {
                const float2 seg_prev = b2d[k] - b2d[k - 1];
                const float2 seg_next = b2d[k + 1] - b2d[k];
                const float lp = math::length(seg_prev);
                const float ln = math::length(seg_next);
                if (lp > 1e-7f && ln > 1e-7f) {
                  const float cos_a = math::dot(seg_prev, seg_next) / (lp * ln);
                  const float turn = 1.0f - std::clamp(cos_a, -1.0f, 1.0f);
                  if (turn > max_turn_angle) {
                    max_turn_angle = turn;
                    apex_idx = k;
                  }
                }
              }
            }
            if (!all_inside) {
              continue;
            }

            /* Standard merge point from segment intersection. */
            const float3 pa = orig[i] + (orig[i + 1] - orig[i]) * s;
            const float3 pb = orig[j] + (orig[j + 1] - orig[j]) * t;
            const float3 cross_pt = (pa + pb) * 0.5f;

            /* If the span is too long, only collapse the sharpest portion (centered on the apex
             * vertex). This prevents long gentle turns from collapsing the entire inner border.
             * The merge point for partial collapses is the midpoint of the first and last border
             * vertices in the trimmed range. */
            int collapse_lo = i + 1;
            int collapse_hi = j;
            float3 merge_pt = cross_pt;

            /* If the intersection loop arc length exceeds 3R along the center polyline, only
             * collapse the sharpest portion centered on the apex. Remaining overlap vertices are
             * linearly interpolated between the intersection endpoints and the merge point. */
            const float max_collapse_arc = 3.0f * R;
            float span_arc = 0.0f;
            for (int k = i; k < j; k++) {
              span_arc += math::distance(poly[k], poly[k + 1]);
            }
            const bool is_partial = span_arc > max_collapse_arc && (j - i) > 3;
            if (is_partial) {
              /* Center the window on the apex using arc-length balance: grow outward from the apex
               * in both directions, spending half the budget on each side. */
              const float half_arc = max_collapse_arc * 0.5f;
              collapse_lo = apex_idx;
              collapse_hi = apex_idx;
              float arc_lo = 0.0f, arc_hi = 0.0f;
              while (collapse_lo > i + 1 || collapse_hi < j) {
                const bool can_lo = (collapse_lo > i + 1) && (arc_lo <= arc_hi);
                const bool can_hi = (collapse_hi < j) && (arc_hi <= arc_lo);
                if (can_lo) {
                  const float seg = math::distance(poly[collapse_lo - 1], poly[collapse_lo]);
                  if (arc_lo + seg > half_arc) {
                    break;
                  }
                  collapse_lo--;
                  arc_lo += seg;
                }
                else if (can_hi) {
                  const float seg = math::distance(poly[collapse_hi], poly[collapse_hi + 1]);
                  if (arc_hi + seg > half_arc) {
                    break;
                  }
                  collapse_hi++;
                  arc_hi += seg;
                }
                else {
                  break;
                }
              }
              merge_pt = (orig[collapse_lo] + orig[collapse_hi]) * 0.5f;
            }

            for (int k = collapse_lo; k <= collapse_hi; k++) {
              border[k] = merge_pt;
            }
            if (is_partial) {
              /* Linearly interpolate border vertices between the real intersection endpoints and
               * the merge point. This evenly spaces the overlap zone instead of leaving it
               * overlapping. */
              const int n_before = collapse_lo - (i + 1);
              for (int k = 0; k < n_before; k++) {
                const float t2 = float(k + 1) / float(n_before + 1);
                border[i + 1 + k] = math::interpolate(orig[i + 1], merge_pt, t2);
              }
              const int n_after = j - collapse_hi;
              for (int k = 0; k < n_after; k++) {
                const float t2 = float(k + 1) / float(n_after + 1);
                border[collapse_hi + 1 + k] = math::interpolate(merge_pt, orig[j], t2);
              }
            }
            /* Skip the entire original intersection span so the uncollapsed outer portions are not
             * re-tested by later iterations. */
            skip_until = j;
            break;
          }
        }
      }
    };

    fix_border_self_intersections(cache.roll_border_left);
    fix_border_self_intersections(cache.roll_border_right);
  }
}

/* Stage 4: the subdivided poly-strip over the whole stroke polyline, and the UV lookup table
 * rasterized from it.
 *
 * The grid covers the entire recorded polyline so that CC subdivision and Laplacian smoothing
 * produce consistent results regardless of which dab is being evaluated. Only the LUT
 * rasterization is restricted to the dab neighborhood (via `roll_eval_row_lo`/`_hi`). */
static void roll_grid_and_lut_build(const RollSpline &spline,
                                    const RollCenterBuild &build,
                                    StrokeCache &cache)
{
  {
    const Span<float3> poly = spline.poly_3d.as_span();
    const int count = build.count;
    const float R = build.R;
    const bool use_pressure_width = build.use_pressure_width;
    const float3 dab_normal = build.dab_normal;
    const float raw_s = build.raw_s;
    const Span<float> lengths = spline.lengths_3d.as_span();

    {
      const int grid_lo = 0;
      const int init_rows = count;
      const int init_cols = 3;
      Vector<float3> grid_pos(init_rows * init_cols);
      Vector<float2> grid_uv(init_rows * init_cols);

      /* Precompute V (along-stroke) texture coordinates.
       *
       * Without pressure-scale: V = raw arc length. sculpt_apply_texture() later divides by
       * initial_radius to normalize.
       *
       * With pressure-scale: V = sum of (seg_len / (R * p_avg)). Dividing each segment by its
       * local pressure-scaled radius makes narrow sections advance the texture faster, so the
       * texture tiles at the same density relative to the strip width -- preserving its aspect
       * ratio regardless of pressure. This is baked per-vertex so that overlapping dabs at
       * different pressures see the same V for the same mesh vertex (no blurriness).
       *
       * The matching offset (stroke_distance_normalized_) is accumulated in add_roll_point() using
       * the same (p0+p1)/2 averaging, and applied in spline_uv() to keep the texture continuous as
       * the polyline shrinks. */
      const Span<float> pressures = spline.pressures.as_span();
      const bool have_pressures = use_pressure_width && int(pressures.size()) >= count;
      Vector<float> v_coords(init_rows);
      v_coords[0] = 0.0f;
      for (int k = 1; k < init_rows; k++) {
        const int vi = grid_lo + k;
        const float seg_len = spline.segment_length_3d(vi - 1);
        if (have_pressures) {
          const float p_avg = std::max((pressures[vi] + pressures[vi - 1]) * 0.5f, 0.01f);
          v_coords[k] = v_coords[k - 1] + seg_len / (R * p_avg);
        }
        else {
          v_coords[k] = v_coords[k - 1] + seg_len;
        }
      }

      for (int k = 0; k < init_rows; k++) {
        const int bi = grid_lo + k; /* index into border arrays */
        const int vi = bi;          /* polyline vertex index (lo=0) */
        /* Col 0 = right border, col 1 = center, col 2 = left border. */
        grid_pos[k * 3 + 0] = cache.roll_border_right[bi];
        grid_pos[k * 3 + 1] = poly[vi];
        grid_pos[k * 3 + 2] = cache.roll_border_left[bi];
        grid_uv[k * 3 + 0] = float2(-1.0f, v_coords[k]);
        grid_uv[k * 3 + 1] = float2(0.0f, v_coords[k]);
        grid_uv[k * 3 + 2] = float2(1.0f, v_coords[k]);
      }

      /* Catmull-Clark subdivision: N rows x 3 cols -> (2N-1) rows x 5 cols.
       *
       * Uses CC face/edge point rules so that cross-stroke segments curve smoothly near collapse
       * points (subsurf effect). All original vertices are PINNED (no vertex rule) so the borders
       * are not smoothed along the stroke direction -- only the NEW intermediate positions get
       * CC-averaged. The center column is inherently preserved because all its original vertices
       * are pinned and its vertical edge points use simple midpoints. */
      int cur_rows, cur_cols;
      {
        const int oR = init_rows;
        const int oC = init_cols; /* 3 */
        const int nR = 2 * oR - 1;
        const int nC = 2 * oC - 1; /* 5 */
        const int fR = oR - 1;     /* face-point row count */
        const int fC = oC - 1;     /* face-point col count = 2 */

        auto oi = [oC](int r, int c) { return r * oC + c; };
        auto ni = [nC](int r, int c) { return r * nC + c; };

        /* Step 1: Face points (odd row, odd col in new grid). F = average of the 4 corners of each
         * quad. */
        Vector<float3> fp(fR * fC);
        Vector<float2> fu(fR * fC);
        for (int r = 0; r < fR; r++) {
          for (int c = 0; c < fC; c++) {
            fp[r * fC + c] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                      grid_pos[oi(r + 1, c)] + grid_pos[oi(r + 1, c + 1)]);
            fu[r * fC + c] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                      grid_uv[oi(r + 1, c)] + grid_uv[oi(r + 1, c + 1)]);
          }
        }

        Vector<float3> np(nR * nC);
        Vector<float2> nu(nR * nC);

        /* Step 2: Fill face points. */
        for (int r = 0; r < fR; r++) {
          for (int c = 0; c < fC; c++) {
            np[ni(2 * r + 1, 2 * c + 1)] = fp[r * fC + c];
            nu[ni(2 * r + 1, 2 * c + 1)] = fu[r * fC + c];
          }
        }

        /* Step 3: Horizontal edge points (even row, odd col). Interior: average of endpoints +
         * adjacent face points. Boundary (first/last row): simple midpoint. */
        for (int r = 0; r < oR; r++) {
          for (int c = 0; c < fC; c++) {
            if (r == 0 || r == oR - 1) {
              np[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)]);
              nu[ni(2 * r, 2 * c + 1)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)]);
            }
            else {
              np[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_pos[oi(r, c)] + grid_pos[oi(r, c + 1)] +
                                                  fp[(r - 1) * fC + c] + fp[r * fC + c]);
              nu[ni(2 * r, 2 * c + 1)] = 0.25f * (grid_uv[oi(r, c)] + grid_uv[oi(r, c + 1)] +
                                                  fu[(r - 1) * fC + c] + fu[r * fC + c]);
            }
          }
        }

        /* Step 4: Vertical edge points (odd row, even col). Border columns (C=0,2) and center
         * column (C=1): simple midpoint. (Center vertical edges stay on the stroke polyline.) */
        for (int r = 0; r < fR; r++) {
          for (int c = 0; c < oC; c++) {
            np[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_pos[oi(r, c)] + grid_pos[oi(r + 1, c)]);
            nu[ni(2 * r + 1, 2 * c)] = 0.5f * (grid_uv[oi(r, c)] + grid_uv[oi(r + 1, c)]);
          }
        }

        /* Step 5: Original vertex points (even row, even col). Center column (C=1): CC boundary
         * rule (1D smoothing along stroke) so cross-stroke segments can curve near merge points.
         * Border columns + first/last rows: pinned. */
        for (int r = 0; r < oR; r++) {
          for (int c = 0; c < oC; c++) {
            if (c == 1 && r > 0 && r < oR - 1) {
              /* Center column interior: smooth along the stroke direction. V_new = (1/8)(V_above +
               * 6V + V_below) -- the CC boundary vertex rule. This shifts the center along the
               * stroke, allowing cross-segments to curve at merge points. */
              np[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_pos[oi(r - 1, c)] +
                                                      6.0f * grid_pos[oi(r, c)] +
                                                      grid_pos[oi(r + 1, c)]);
              nu[ni(2 * r, 2 * c)] = (1.0f / 8.0f) * (grid_uv[oi(r - 1, c)] +
                                                      6.0f * grid_uv[oi(r, c)] +
                                                      grid_uv[oi(r + 1, c)]);
            }
            else {
              /* Corners, boundary rows, border columns: pinned. */
              np[ni(2 * r, 2 * c)] = grid_pos[oi(r, c)];
              nu[ni(2 * r, 2 * c)] = grid_uv[oi(r, c)];
            }
          }
        }

        grid_pos = std::move(np);
        grid_uv = std::move(nu);
        cur_rows = nR;
        cur_cols = nC;
      }

      /* Second CC pass: standard rules for additional smoothing. Border columns remain pinned;
       * interior vertices get the full CC vertex rule, producing smoother cross-stroke
       * curvature. */
      catmull_clark_subdivide_grid(grid_pos, grid_uv, cur_rows, cur_cols);

      /* 2D Laplacian smoothing: pin border columns, smooth everything between them (including
       * center). This curves the cross-stroke segments near collapse points -- the perpendicular
       * lines bow toward the merge point like a subdivision surface. Uses double-buffering to
       * avoid per-iteration allocation. */
      {
        constexpr int smooth_iters = 10;
        constexpr float mix = 0.5f;
        const int grid_total = cur_rows * cur_cols;
        Vector<float3> buf_p(grid_total);
        Vector<float2> buf_u(grid_total);
        float3 *src_p = grid_pos.data(), *dst_p = buf_p.data();
        float2 *src_u = grid_uv.data(), *dst_u = buf_u.data();
        for (int iter = 0; iter < smooth_iters; iter++) {
          memcpy(dst_p, src_p, sizeof(float3) * grid_total);
          memcpy(dst_u, src_u, sizeof(float2) * grid_total);
          for (int r = 1; r < cur_rows - 1; r++) {
            for (int c = 1; c < cur_cols - 1; c++) {
              const int idx = r * cur_cols + c;
              float3 avg_p = 0.25f * (src_p[(r - 1) * cur_cols + c] + src_p[(r + 1) * cur_cols + c] +
                                      src_p[r * cur_cols + c - 1] + src_p[r * cur_cols + c + 1]);
              float2 avg_u = 0.25f * (src_u[(r - 1) * cur_cols + c] + src_u[(r + 1) * cur_cols + c] +
                                      src_u[r * cur_cols + c - 1] + src_u[r * cur_cols + c + 1]);
              dst_p[idx] = (1.0f - mix) * src_p[idx] + mix * avg_p;
              dst_u[idx] = (1.0f - mix) * src_u[idx] + mix * avg_u;
            }
          }
          std::swap(src_p, dst_p);
          std::swap(src_u, dst_u);
        }
        /* Ensure results end up in grid_pos/grid_uv. */
        if (src_p != grid_pos.data()) {
          memcpy(grid_pos.data(), src_p, sizeof(float3) * grid_total);
          memcpy(grid_uv.data(), src_u, sizeof(float2) * grid_total);
        }
      }

      /* LUT projection uses the per-dab sculpt normal -- the direction the brush "stamps" from.
       * This ensures the texture maps at correct density on slopes (combined with the
       * inclination-narrowed borders). The per-dab normal adapts to the local surface as the
       * stroke progresses across curved geometry. */
      float3 view_x = math::cross(dab_normal, float3(0, 0, 1));
      if (math::length_squared(view_x) < 1e-6f) {
        view_x = math::cross(dab_normal, float3(1, 0, 0));
      }
      view_x = math::normalize(view_x);
      const float3 view_y = math::normalize(math::cross(dab_normal, view_x));

      const int total = cur_rows * cur_cols;
      Vector<float2> grid_pos_2d(total);
      {
        for (int i = 0; i < total; i++) {
          grid_pos_2d[i] = float2(math::dot(grid_pos[i], view_x), math::dot(grid_pos[i], view_y));
        }
      }

      /* Compute eval row range: intersect arc-length window with spatial distance. The arc-length
       * constraint prevents old segments on the far side of a self-crossing stroke from polluting
       * the LUT. The spatial check ensures coverage on sharp curves where arc-length alone might
       * be too tight. Forward range is more generous to include the trailing segment and its
       * self-intersection merges.
       *
       * After two CC subdivision passes, original polyline vertex `vi` maps to grid row
       * `4 * vi`. */
      {
        const float eval_s_lo = std::max(0.0f, raw_s - R * 2.0f);
        const float eval_s_hi = std::min(lengths.last(), raw_s + R * 3.0f);

        int eval_seg_lo, eval_seg_hi;
        float fac_unused;
        length_parameterize::sample_at_length(lengths, eval_s_lo, eval_seg_lo, fac_unused);
        length_parameterize::sample_at_length(lengths, eval_s_hi, eval_seg_hi, fac_unused);

        const int arc_row_lo = std::max(0, eval_seg_lo * 4 - EVAL_ROW_MARGIN);
        const int arc_row_hi = std::min(cur_rows - 1, (eval_seg_hi + 1) * 4 + EVAL_ROW_MARGIN);

        /* Within the arc-length window, keep rows spatially near the dab. */
        const float2 dab_2d = float2(math::dot(cache.location, view_x),
                                     math::dot(cache.location, view_y));
        const float eval_r_sq = (R * 2.0f) * (R * 2.0f);
        const int center_c = cur_cols / 2;

        int eval_lo = cur_rows - 1, eval_hi = 0;
        for (int r = arc_row_lo; r <= arc_row_hi; r++) {
          if (math::distance_squared(dab_2d, grid_pos_2d[r * cur_cols + center_c]) <= eval_r_sq) {
            eval_lo = std::min(eval_lo, r);
            eval_hi = std::max(eval_hi, r);
          }
        }
        cache.roll_eval_row_lo = std::max(0, eval_lo - EVAL_ROW_MARGIN);
        cache.roll_eval_row_hi = std::min(cur_rows - 2, eval_hi + EVAL_ROW_MARGIN);
      }

      cache.roll_subdiv_rows = cur_rows;
      cache.roll_subdiv_cols = cur_cols;

      /* Build UV lookup table: rasterize eval-range grid quads into a 2D texture so per-vertex UV
       * lookup is O(1) instead of O(rows x cols). On high-poly meshes this is the dominant cost --
       * the LUT amortizes the bilinear inverse across a fixed-size image. */
      {
        constexpr int RES = StrokeCache::ROLL_LUT_RES;
        const int el = cache.roll_eval_row_lo;
        const int eh = cache.roll_eval_row_hi;

        /* 2D bounding box of eval-range grid. */
        float2 bb_min(FLT_MAX), bb_max(-FLT_MAX);
        for (int r = el; r <= std::min(eh + 1, cur_rows - 1); r++) {
          for (int c = 0; c < cur_cols; c++) {
            bb_min = math::min(bb_min, grid_pos_2d[r * cur_cols + c]);
            bb_max = math::max(bb_max, grid_pos_2d[r * cur_cols + c]);
          }
        }
        const float2 extent = bb_max - bb_min;
        const float2 margin = extent * 0.1f;
        bb_min -= margin;
        bb_max += margin;
        const float2 ext = bb_max - bb_min;
        const float2 inv_ext(ext.x > 1e-10f ? float(RES) / ext.x : 0.0f,
                             ext.y > 1e-10f ? float(RES) / ext.y : 0.0f);

        const int lut_total = RES * RES;
        cache.roll_lut_uv.reinitialize(lut_total);
        cache.roll_lut_dist_sq.reinitialize(lut_total);
        cache.roll_lut_tan.reinitialize(lut_total);
        cache.roll_lut_row.reinitialize(lut_total);
        for (int i = 0; i < lut_total; i++) {
          cache.roll_lut_uv[i] = float2(FLT_MAX, 0.0f);
          cache.roll_lut_dist_sq[i] = FLT_MAX;
          cache.roll_lut_row[i] = -1;
        }

        auto cross2d = [](float2 a, float2 b) { return a.x * b.y - a.y * b.x; };

        /* Rasterize each eval-range quad into the LUT via bilinear inverse.
         *
         * For each quad (P00, P10, P01, P11), and for each pixel in its bounding box, we solve:
         * "given a 2D point Q, find (u,v) such that
         * Q = (1-u)(1-v)P00 + u(1-v)P10 + (1-u)v*P01 + uv*P11."
         *
         * Rearranging gives a quadratic in v (A*v^2 + B*v + C = 0) using cross-products of the
         * quad's edge vectors. Once v is found, u follows from a linear solve. If (u,v) is inside
         * [0,1]^2 the pixel is inside the quad, and we bilinearly interpolate the UV + tangent.
         *
         * When multiple quads overlap (at self-intersection collapses), the quad whose bilinear
         * solution is closest to the pixel center wins (roll_lut_dist_sq comparison). */
        for (int r = el; r <= eh; r++) {
          for (int c = 0; c < cur_cols - 1; c++) {
            const float2 P00 = grid_pos_2d[r * cur_cols + c];
            const float2 P10 = grid_pos_2d[r * cur_cols + c + 1];
            const float2 P01 = grid_pos_2d[(r + 1) * cur_cols + c];
            const float2 P11 = grid_pos_2d[(r + 1) * cur_cols + c + 1];

            /* Pixel bounding box of this quad. */
            float2 qmin = math::min(math::min(P00, P10), math::min(P01, P11));
            float2 qmax = math::max(math::max(P00, P10), math::max(P01, P11));
            int px_lo = std::max(0, int((qmin.x - bb_min.x) * inv_ext.x));
            int px_hi = std::min(RES - 1, int((qmax.x - bb_min.x) * inv_ext.x) + 1);
            int py_lo = std::max(0, int((qmin.y - bb_min.y) * inv_ext.y));
            int py_hi = std::min(RES - 1, int((qmax.y - bb_min.y) * inv_ext.y) + 1);

            /* Bilinear basis vectors for the quadratic solve. */
            const float2 a = P10 - P00;
            const float2 b = P01 - P00;
            const float2 tw = P11 - P10 - P01 + P00; /* twist term */
            const float A_coeff = cross2d(b, tw);

            for (int py = py_lo; py <= py_hi; py++) {
              for (int px = px_lo; px <= px_hi; px++) {
                const float2 query = bb_min + float2(px + 0.5f, py + 0.5f) / float(RES) * ext;
                const float2 d = query - P00;

                const float B_coeff = cross2d(b, a) - cross2d(d, tw);
                const float C_coeff = -cross2d(d, a);

                float v;
                if (std::abs(A_coeff) < 1e-8f) {
                  v = (std::abs(B_coeff) > 1e-8f) ? -C_coeff / B_coeff : 0.5f;
                }
                else {
                  const float disc = B_coeff * B_coeff - 4.0f * A_coeff * C_coeff;
                  if (disc < 0.0f) {
                    continue;
                  }
                  const float sq = sqrtf(disc);
                  const float v1 = (-B_coeff + sq) / (2.0f * A_coeff);
                  const float v2 = (-B_coeff - sq) / (2.0f * A_coeff);
                  const float e1 = std::max(0.0f, std::max(-v1, v1 - 1.0f));
                  const float e2 = std::max(0.0f, std::max(-v2, v2 - 1.0f));
                  v = (e1 <= e2) ? v1 : v2;
                }
                if (v < -0.01f || v > 1.01f) {
                  continue;
                }
                v = std::clamp(v, 0.0f, 1.0f);

                const float2 dv = a + v * tw;
                float u;
                if (std::abs(dv.x) > std::abs(dv.y)) {
                  u = (std::abs(dv.x) > 1e-8f) ? (d.x - v * b.x) / dv.x : 0.5f;
                }
                else {
                  u = (std::abs(dv.y) > 1e-8f) ? (d.y - v * b.y) / dv.y : 0.5f;
                }
                if (u < -0.01f || u > 1.01f) {
                  continue;
                }
                u = std::clamp(u, 0.0f, 1.0f);

                const float2 pnt = (1 - u) * (1 - v) * P00 + u * (1 - v) * P10 +
                                   (1 - u) * v * P01 + u * v * P11;
                const float dsq = math::distance_squared(query, pnt);

                const int li = py * RES + px;
                /* Prefer older rows on overlap to keep the LUT stable.
                 *
                 * The loop iterates rows low->high, so a non-empty pixel was written by a row <= r.
                 * Cases:
                 *   - empty: write
                 *   - same/adjacent row (within 2): use dsq (better fit wins)
                 *   - far row (existing < r - 2): keep older (don't write)
                 * This way overlapping branches at sharp turns get a stable single stroke position
                 * rather than flickering between competing branches based on tiny float
                 * distances. */
                const int existing_row = cache.roll_lut_row[li];
                bool write = false;
                if (existing_row < 0) {
                  write = true;
                }
                else if (r - existing_row <= 2) {
                  write = (dsq < cache.roll_lut_dist_sq[li]);
                }
                if (write) {
                  cache.roll_lut_dist_sq[li] = dsq;
                  cache.roll_lut_row[li] = r;
                  /* Interpolate UV. */
                  const float2 &UV00 = grid_uv[r * cur_cols + c];
                  const float2 &UV10 = grid_uv[r * cur_cols + c + 1];
                  const float2 &UV01 = grid_uv[(r + 1) * cur_cols + c];
                  const float2 &UV11 = grid_uv[(r + 1) * cur_cols + c + 1];
                  cache.roll_lut_uv[li] = (1 - u) * (1 - v) * UV00 + u * (1 - v) * UV10 +
                                          (1 - u) * v * UV01 + u * v * UV11;
                  /* Interpolate tangent from 3D grid. */
                  const float3 &Q00 = grid_pos[r * cur_cols + c];
                  const float3 &Q10 = grid_pos[r * cur_cols + c + 1];
                  const float3 &Q01 = grid_pos[(r + 1) * cur_cols + c];
                  const float3 &Q11 = grid_pos[(r + 1) * cur_cols + c + 1];
                  const float3 dPdv = (1 - u) * (Q01 - Q00) + u * (Q11 - Q10);
                  cache.roll_lut_tan[li] = math::normalize(dPdv);
                }
              }
            }
          }
        }

        cache.roll_lut_min = bb_min;
        cache.roll_lut_inv_extent = inv_ext;
        cache.roll_lut_ready = true;
      }

      cache.roll_subdiv_pos = std::move(grid_pos);
      cache.roll_subdiv_pos_2d = std::move(grid_pos_2d);
      cache.roll_subdiv_uv = std::move(grid_uv);
      cache.roll_view_x = view_x;
      cache.roll_view_y = view_y;
    }

    cache.roll_surface_ready = true;
  }
}

void PaintStroke::compute_roll_center(StrokeCache &cache)
{
  RollCenterBuild build;
  if (!roll_center_locate_on_spline(roll_spline_, cache, build)) {
    return;
  }
  /* Cleared before the strip work starts and set at the very end of it, so a build that bails out
   * anywhere in between leaves the surface data marked unusable rather than half-written. */
  cache.roll_surface_ready = false;
  roll_borders_build(roll_spline_, brush, roll_proj_normal_, roll_initial_radius_, cache, build);
  roll_borders_fix_self_intersections(roll_spline_, build, cache);
  roll_grid_and_lut_build(roll_spline_, build, cache);
}

void PaintStroke::spline_uv(const StrokeCache &cache,
                            const float co[3],
                            float r_out[3],
                            float r_tan[3]) const
{
  /* LUT-based UV lookup (O(1) per vertex).
   *
   * The grid's bilinear inverse is pre-rasterized into a 2D lookup table once per dab. Per-vertex
   * evaluation is a simple 2D projection + bilinear sample of the LUT -- no per-quad search
   * needed. compute_roll_center() guarantees the LUT is ready before any dab. */

  constexpr int RES = StrokeCache::ROLL_LUT_RES;
  const float2 query = float2(math::dot(float3(co), cache.roll_view_x),
                              math::dot(float3(co), cache.roll_view_y));

  const float2 fc = (query - cache.roll_lut_min) * cache.roll_lut_inv_extent;
  /* If the query is outside the LUT bounding box (or NaN), return a large distance so this result
   * loses the mirror-symmetry comparison. Without this, edge-clamped LUT values at mirrored
   * positions can produce bogus near-zero U that incorrectly wins. */
  if (UNLIKELY(!std::isfinite(fc.x) || !std::isfinite(fc.y) || fc.x < -0.5f ||
               fc.x > float(RES) - 0.5f || fc.y < -0.5f || fc.y > float(RES) - 0.5f))
  {
    r_out[0] = FLT_MAX;
    r_out[1] = r_out[2] = 0.0f;
    zero_v3(r_tan);
    return;
  }

  const float fx = std::clamp(fc.x - 0.5f, 0.0f, float(RES - 2));
  const float fy = std::clamp(fc.y - 0.5f, 0.0f, float(RES - 2));
  const int ix = std::min(int(fx), RES - 2);
  const int iy = std::min(int(fy), RES - 2);
  const float tx = fx - float(ix);
  const float ty = fy - float(iy);

  /* Bilinear interpolation of UV from 4 nearest LUT cells. */
  float2 uv00 = cache.roll_lut_uv[iy * RES + ix];
  float2 uv10 = cache.roll_lut_uv[iy * RES + ix + 1];
  float2 uv01 = cache.roll_lut_uv[(iy + 1) * RES + ix];
  float2 uv11 = cache.roll_lut_uv[(iy + 1) * RES + ix + 1];
  float3 t00 = cache.roll_lut_tan[iy * RES + ix];
  float3 t10 = cache.roll_lut_tan[iy * RES + ix + 1];
  float3 t01 = cache.roll_lut_tan[(iy + 1) * RES + ix];
  float3 t11 = cache.roll_lut_tan[(iy + 1) * RES + ix + 1];

  /* If any of the 4 LUT pixels was never rasterized (no grid quad covered it), fill it from the
   * nearest valid neighbor among the other 3 so the bilinear sample produces a sensible UV instead
   * of a V-near-zero value that lands on tile boundaries under texture repeat. */
  constexpr float INVALID = FLT_MAX * 0.5f;
  const bool b00 = uv00.x >= INVALID;
  const bool b10 = uv10.x >= INVALID;
  const bool b01 = uv01.x >= INVALID;
  const bool b11 = uv11.x >= INVALID;
  if (UNLIKELY(b00 || b10 || b01 || b11)) {
    /* If ALL four are invalid, fall back to FLT_MAX so the symmetry comparison in
     * sculpt_apply_texture discards this sample. */
    if (b00 && b10 && b01 && b11) {
      r_out[0] = FLT_MAX;
      r_out[1] = r_out[2] = 0.0f;
      zero_v3(r_tan);
      return;
    }
    /* Find any valid neighbor to clone from. */
    const float2 fill_uv = !b00 ? uv00 : !b10 ? uv10 : !b01 ? uv01 : uv11;
    const float3 fill_t = !b00 ? t00 : !b10 ? t10 : !b01 ? t01 : t11;
    if (b00) {
      uv00 = fill_uv;
      t00 = fill_t;
    }
    if (b10) {
      uv10 = fill_uv;
      t10 = fill_t;
    }
    if (b01) {
      uv01 = fill_uv;
      t01 = fill_t;
    }
    if (b11) {
      uv11 = fill_uv;
      t11 = fill_t;
    }
  }

  /* Detect overlap discontinuity: at sharp turns, multiple grid quads compete for the same LUT
   * pixels. Adjacent pixels may pick different quads (tiny float-distance differences flip the
   * winner), making bilinear interpolation blend UVs from unrelated stroke positions.
   *
   * If the 4 V values vary too much, fall back to nearest-neighbor (the pixel with the smallest
   * dist_sq) -- at least the result is a single valid stroke position rather than a fabricated
   * midpoint.
   *
   * Threshold is "half a brush-radius worth of arc length". In pressure mode V is already
   * normalized (~1 unit per radius), so 0.5 directly. In non-pressure mode V is in world units, so
   * 0.5 x radius. */
  const float v_min = std::min({uv00.y, uv10.y, uv01.y, uv11.y});
  const float v_max = std::max({uv00.y, uv10.y, uv01.y, uv11.y});
  const bool use_norm_v_threshold = brush && brush->roll_pressure_scale &&
                                    BKE_brush_use_size_pressure(brush);
  const float v_threshold = use_norm_v_threshold ? 0.5f : 0.5f * cache.initial_radius;
  float2 uv_result;
  float3 t_result;
  if (UNLIKELY(v_max - v_min > v_threshold)) {
    /* Pick the LUT pixel with the smallest dist_sq among the 4 neighbors. */
    const float d00 = cache.roll_lut_dist_sq[iy * RES + ix];
    const float d10 = cache.roll_lut_dist_sq[iy * RES + ix + 1];
    const float d01 = cache.roll_lut_dist_sq[(iy + 1) * RES + ix];
    const float d11 = cache.roll_lut_dist_sq[(iy + 1) * RES + ix + 1];
    float best_d = d00;
    uv_result = uv00;
    t_result = t00;
    if (d10 < best_d) {
      best_d = d10;
      uv_result = uv10;
      t_result = t10;
    }
    if (d01 < best_d) {
      best_d = d01;
      uv_result = uv01;
      t_result = t01;
    }
    if (d11 < best_d) {
      uv_result = uv11;
      t_result = t11;
    }
  }
  else {
    uv_result = (1 - tx) * (1 - ty) * uv00 + tx * (1 - ty) * uv10 + (1 - tx) * ty * uv01 +
                tx * ty * uv11;
    t_result = (1 - tx) * (1 - ty) * t00 + tx * (1 - ty) * t10 + (1 - tx) * ty * t01 +
               tx * ty * t11;
  }

  r_out[0] = -uv_result.x;
  r_out[1] = uv_result.y;
  r_out[2] = 0.0f;

  copy_v3_v3(r_tan, math::normalize(t_result));

  /* Apply V offset to keep texture continuous as virtual knots are consumed. Use normalized offset
   * when pressure-scaled V is baked into the grid. */
  const bool use_norm_v = brush && brush->roll_pressure_scale &&
                          BKE_brush_use_size_pressure(brush);
  if (use_norm_v) {
    r_out[1] += stroke_distance_normalized_ - roll_virtual_length_normalized_;
  }
  else {
    r_out[1] += stroke_distance_world_ - roll_virtual_length_;
  }
}

float PaintStroke::spline_length() const
{
  return roll_spline_.total_length_3d();
}

void PaintStroke::extract_roll_control_points(Vector<float3> &r_positions,
                                              Vector<float> &r_radii) const
{
  r_positions.clear();
  r_radii.clear();
  if (num_points_ < 2) {
    return;
  }

  constexpr int cap = PAINT_MAX_INPUT_SAMPLES;
  const int oldest = (cur_point_ - num_points_ + cap) % cap;

  /* Gather the ordered REAL knots (object space). Virtual extensions live only in
   * `roll_spline_.poly_3d`, never in `points_`, so they are excluded by construction. */
  Vector<float3> pos(num_points_);
  Vector<float> pres(num_points_);
  for (int i = 0; i < num_points_; i++) {
    const int idx = (oldest + i) % cap;
    pos[i] = points_[idx].location;
    pres[i] = points_[idx].pressure;
  }

  /* Cumulative arc length along the real knots. */
  Vector<float> len(num_points_);
  len[0] = 0.0f;
  for (int i = 1; i < num_points_; i++) {
    len[i] = len[i - 1] + math::distance(pos[i - 1], pos[i]);
  }
  const float total = len.last();

  /* Radius attribute mirrors the painted width: 1.0 == full brush size (see
   * `paintcurve_geometry_add_point()`). Use pressure only when the stroke actually varied width. */
  const bool width_from_pressure = brush && brush->roll_pressure_scale &&
                                   BKE_brush_use_size_pressure(brush);
  auto radius_from_pressure = [&](const float p) {
    return width_from_pressure ? std::clamp(p, 0.05f, 1.0f) : 1.0f;
  };

  if (total < 1e-6f) {
    /* All knots coincident: emit a minimal 2-point curve so the handoff still has a valid curve. */
    r_positions.append(pos[0]);
    r_positions.append(pos.last());
    r_radii.append(radius_from_pressure(pres[0]));
    r_radii.append(radius_from_pressure(pres.last()));
    return;
  }

  /* Shape-aware simplification for the Roll -> Curve Patch handoff. The old path always emitted
   * fourteen evenly spaced knots, so a straight stroke still arrived with fourteen control points.
   * Ramer-Douglas-Peucker keeps endpoints and corners while dropping collinear dab samples.
   * Distance is measured in the roll projection plane so surface curvature does not inflate the
   * count the way a raw 3D test would. */
  const float3 plane_normal = math::length_squared(roll_proj_normal_) > 1e-12f ?
                                  math::normalize(roll_proj_normal_) :
                                  float3(0.0f);
  const float scale = roll_initial_radius_ > 1e-6f ? roll_initial_radius_ :
                                                      std::max(total * 0.05f, 1e-4f);
  const float epsilon = scale * 0.12f;

  Array<bool> keep(num_points_);
  roll_simplify_polyline_mask(pos.as_span(), epsilon, pos[0], plane_normal, keep);

  Vector<float3> simplified_positions;
  Vector<float> simplified_radii;
  simplified_positions.reserve(num_points_);
  simplified_radii.reserve(num_points_);
  for (const int i : IndexRange(num_points_)) {
    if (!keep[i]) {
      continue;
    }
    simplified_positions.append(pos[i]);
    simplified_radii.append(radius_from_pressure(pres[i]));
  }

  /* Pathological scribbles can still exceed a sane editing budget; thin uniformly only then. */
  constexpr int kMaxControlPoints = 48;
  if (simplified_positions.size() > kMaxControlPoints) {
    roll_uniform_resample(simplified_positions.as_span(),
                          simplified_radii.as_span(),
                          kMaxControlPoints,
                          r_positions,
                          r_radii);
    return;
  }

  r_positions = simplified_positions;
  r_radii = simplified_radii;
}

void PaintStroke::draw_debug_roll(bContext *C) const
{
  if (!need_roll_mapping_ || roll_spline_.is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  const float ox = float(region->winrct.xmin);
  const float oy = float(region->winrct.ymin);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  /* --- 2D overlay: center polyline + tick marks --- */
  {
    const uint pos_attr = GPU_vertformat_attr_add(
        immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    const int n_pts = int(roll_spline_.poly_2d.size());

    /* Polyline: red = virtual extension, green = real. */
    GPU_line_width(3.0f);
    if (n_virtual_poly_points_ > 1) {
      immUniformColor4ub(255, 50, 50, 200);
      const int cnt = std::min(n_virtual_poly_points_ + 1, n_pts);
      immBegin(GPU_PRIM_LINE_STRIP, cnt);
      for (int i = 0; i < cnt; i++) {
        immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
      }
      immEnd();
    }
    if (n_virtual_poly_points_ < n_pts) {
      immUniformColor4ub(50, 255, 50, 200);
      const int cnt = n_pts - n_virtual_poly_points_;
      immBegin(GPU_PRIM_LINE_STRIP, cnt);
      for (int i = n_virtual_poly_points_; i < n_pts; i++) {
        immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
      }
      immEnd();
    }

    /* Tick marks at each knot point. */
    GPU_line_width(1.5f);
    const float tick_len = 10.0f;
    for (int i = 0; i < n_pts; i++) {
      const float2 tan = roll_spline_.tangent_2d_at_index(std::min(i, n_pts - 2));
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[i];
      immUniformColor4ub(200, 200, 200, 140);
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * tick_len + ox, pt.y + perp.y * tick_len + oy);
      immVertex2f(pos_attr, pt.x - perp.x * tick_len + ox, pt.y - perp.y * tick_len + oy);
      immEnd();
    }

    /* Yellow tick at virtual/real boundary. */
    if (n_virtual_poly_points_ > 0 && n_virtual_poly_points_ < n_pts) {
      GPU_line_width(2.5f);
      immUniformColor4ub(255, 255, 0, 255);
      const float2 tan = roll_spline_.tangent_2d_at_index(n_virtual_poly_points_);
      const float2 perp(-tan.y, tan.x);
      const float2 &pt = roll_spline_.poly_2d[n_virtual_poly_points_];
      immBegin(GPU_PRIM_LINES, 2);
      immVertex2f(pos_attr, pt.x + perp.x * 25.0f + ox, pt.y + perp.y * 25.0f + oy);
      immVertex2f(pos_attr, pt.x - perp.x * 25.0f + ox, pt.y - perp.y * 25.0f + oy);
      immEnd();
    }

    immUnbindProgram();
  }

  /* --- 3D debug draw: poly-strip grid wireframe --- */

  Object *ob = CTX_data_active_object(C);
  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
  if (ob && ob->runtime && ob->runtime->sculpt_session && ob->runtime->sculpt_session->cache &&
      rv3d)
  {
    const StrokeCache &cache = *ob->runtime->sculpt_session->cache;
    if (cache.roll_surface_ready && cache.roll_subdiv_rows > 1) {
      const int rows = cache.roll_subdiv_rows;
      const int cols = cache.roll_subdiv_cols;
      const float3 *gpos = cache.roll_subdiv_pos.data();
      const int center_col = cols / 2;

      /* Save viewport state and set up 3D matrices. */
      int saved_viewport[4];
      GPU_viewport_size_get_i(saved_viewport);

      GPU_matrix_push();
      GPU_matrix_push_projection();
      wmViewport(&region->winrct);
      GPU_matrix_projection_set(rv3d->winmat);
      const float4x4 mv = float4x4(rv3d->viewmat) * ob->object_to_world();
      GPU_matrix_set(mv.ptr());

      const uint pos3d_attr = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

      const int eval_lo = cache.roll_eval_row_lo;
      const int eval_hi = cache.roll_eval_row_hi;

      /* Full-grid column lines (dim) -- shows the entire stored strip. */
      for (int c = 0; c < cols; c++) {
        GPU_line_width(1.0f);
        immUniformColor4ub(100, 100, 100, 60);
        immBegin(GPU_PRIM_LINE_STRIP, rows);
        for (int r = 0; r < rows; r++) {
          immVertex3fv(pos3d_attr, gpos[r * cols + c]);
        }
        immEnd();
      }

      /* Eval-range column lines (bright) -- the portion used for this dab. */
      const int eval_rows = std::min(eval_hi + 2, rows) - eval_lo;
      if (eval_rows > 1) {
        for (int c = 0; c < cols; c++) {
          if (c == center_col) {
            GPU_line_width(2.5f);
            immUniformColor4ub(50, 255, 50, 220);
          }
          else if (c == 0 || c == cols - 1) {
            GPU_line_width(2.0f);
            immUniformColor4ub(255, 160, 0, 220);
          }
          else {
            GPU_line_width(1.0f);
            immUniformColor4ub(180, 180, 180, 140);
          }
          immBegin(GPU_PRIM_LINE_STRIP, eval_rows);
          for (int r = eval_lo; r < eval_lo + eval_rows; r++) {
            immVertex3fv(pos3d_attr, gpos[r * cols + c]);
          }
          immEnd();
        }
      }

      /* Row lines (across stroke) in eval range only. */
      GPU_line_width(1.0f);
      immUniformColor4ub(200, 200, 200, 80);
      const int row_stride = std::max(1, eval_rows / 30);
      for (int r = eval_lo; r < eval_lo + eval_rows; r += row_stride) {
        immBegin(GPU_PRIM_LINE_STRIP, cols);
        for (int c = 0; c < cols; c++) {
          immVertex3fv(pos3d_attr, gpos[r * cols + c]);
        }
        immEnd();
      }

      /* Points must be drawn with a shader that writes `gl_PointSize`: the Vulkan backend asserts
       * otherwise (`VKShader::ensure_and_get_graphics_pipeline()`), where OpenGL silently accepted
       * the global `GPU_point_size()` state. Swap the line shader for the point shader -- both take
       * the same `pos: in vec3` input, so `pos3d_attr` stays valid across the rebind (same pattern
       * as `transform_mode_vert_slide.cc`). The trailing `immUnbindProgram()` below unbinds it. */
      immUnbindProgram();
      immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_COLOR);

      /* Cyan markers at eval range boundaries. */
      GPU_point_size(10.0f);
      immUniformColor4ub(0, 255, 255, 255);
      for (int boundary_r : {eval_lo, std::min(eval_hi + 1, rows - 1)}) {
        immBegin(GPU_PRIM_POINTS, 1);
        immVertex3fv(pos3d_attr, gpos[boundary_r * cols + center_col]);
        immEnd();
      }

      /* Red dots at collapsed border vertices (where consecutive border vertices share the same
       * position = self-intersection collapse). */
      GPU_point_size(8.0f);
      immUniformColor4ub(255, 0, 0, 255);
      for (int side = 0; side < 2; side++) {
        const Vector<float3> &border = (side == 0) ? cache.roll_border_left :
                                                     cache.roll_border_right;
        const int bcount = int(border.size());
        for (int k = 2; k < bcount - 1; k++) {
          if (math::distance_squared(border[k - 1], border[k]) < 1e-10f &&
              math::distance_squared(border[k], border[k + 1]) < 1e-10f)
          {
            if (math::distance_squared(border[k - 2], border[k - 1]) > 1e-10f) {
              immBegin(GPU_PRIM_POINTS, 1);
              immVertex3fv(pos3d_attr, border[k]);
              immEnd();
            }
          }
        }
      }

      immUnbindProgram();
      GPU_matrix_pop_projection();
      GPU_matrix_pop();

      /* Restore viewport. */
      GPU_viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    }
  }

  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_roll_debug(bContext *C,
                                  const int2 & /*xy*/,
                                  const float2 & /*tilt*/,
                                  void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);
  stroke->draw_debug_roll(C);
}

void PaintStroke::draw_roll_preview(bContext *C) const
{
  if (!need_roll_mapping_ || roll_spline_.is_empty()) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return;
  }

  const int n_pts = int(roll_spline_.poly_2d.size());

  /* Determine which polyline index corresponds to the last-painted dab. Points from painted_poly
   * onward are "unflushed" and shown as preview. */
  constexpr int buf_cap = PAINT_MAX_INPUT_SAMPLES;
  const int half = roll_half_points();

  int painted_poly;
  if (last_painted_roll_idx_ < 0 || num_points_ < half) {
    painted_poly = n_virtual_poly_points_;
  }
  else {
    const int oldest_idx = (cur_point_ - num_points_ + buf_cap) % buf_cap;
    const int dist = (last_painted_roll_idx_ - oldest_idx + buf_cap) % buf_cap;
    painted_poly = n_virtual_poly_points_ + dist;
    painted_poly = std::min(painted_poly, n_pts - 1);
  }

  if (painted_poly >= n_pts) {
    return;
  }

  const float ox = float(region->winrct.xmin);
  const float oy = float(region->winrct.ymin);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);

  const uint pos_attr = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Same color as the stabilize stroke line. */
  immUniformColor4ub(255, 100, 100, 128);

  GPU_line_width(2.0f);
  const int count = n_pts - painted_poly;
  if (count >= 2) {
    immBegin(GPU_PRIM_LINE_STRIP, count);
    for (int i = painted_poly; i < n_pts; i++) {
      immVertex2f(pos_attr, roll_spline_.poly_2d[i].x + ox, roll_spline_.poly_2d[i].y + oy);
    }
    immEnd();
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_roll_preview(bContext *C,
                                    const int2 & /*xy*/,
                                    const float2 & /*tilt*/,
                                    void *customdata)
{
  PaintStroke *stroke = static_cast<PaintStroke *>(customdata);
  stroke->draw_roll_preview(C);
}

/** \} */

void PaintStroke::init_roll_cursors()
{
  /* Always-on preview of the unflushed portion of the roll spline. */
  roll_cursor_ = WM_paint_cursor_activate(
      SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_roll_preview, this);
  /* Register the roll spline debug overlay only when the developer "Paint Debug" option is enabled
   * (Preferences > Developer Extras). */
  if (U.experimental.use_paint_debug) {
    debug_cursor_ = WM_paint_cursor_activate(
        SPACE_TYPE_ANY, RGN_TYPE_ANY, paint_brush_cursor_poll, paint_draw_roll_debug, this);
  }
}

}  // namespace blender::ed::sculpt_paint
