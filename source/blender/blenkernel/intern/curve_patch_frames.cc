/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_curve_patch.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"

namespace blender::bke {

static float3 dominant_normal(const CurvePatchSpline &spline, const int begin, const int end)
{
  /* The dominant face rather than the mean: cluster against the window's first sample and let the
   * group covering more samples win. On a clean edge that is exactly one face's normal. */
  const float3 seed = spline.normals_3d[begin];
  float3 accum_near(0.0f), accum_far(0.0f);
  int count_near = 0, count_far = 0;
  for (int i = begin; i <= end; i++) {
    if (math::dot(spline.normals_3d[i], seed) > 0.9f) {
      accum_near += spline.normals_3d[i];
      count_near++;
    }
    else {
      accum_far += spline.normals_3d[i];
      count_far++;
    }
  }
  const float3 winner = count_near >= count_far ? accum_near : accum_far;
  const float len = math::length(winner);
  return len > 1e-6f ? winner / len : seed;
}

bool curve_patch_frames_partition(const CurvePatchSpline &spline,
                                  const CurvePatchFramesParams &params,
                                  Vector<CurvePatchFrameRange> &r_ranges,
                                  bool &r_capped)
{
  r_ranges.clear();
  r_capped = false;
  const int count = int(spline.poly_3d.size());
  if (spline.normals_3d.size() != count || count < 2) {
    return false;
  }
  const bool have_lengths = spline.lengths_3d.size() == count;
  const float turn_cos = std::cos(params.turn_threshold_rad);
  const float break_cos = std::cos(params.break_threshold_rad);

  int begin = 0;
  while (begin < count - 1) {
    int end = begin + 1;
    while (end < count - 1) {
      const float d = math::dot(spline.normals_3d[end], spline.normals_3d[begin]);
      /* A break tears the window open unconditionally: across an edge the turn spans zero arc
       * length, and applying the minimum length to it would force one window over both faces. */
      if (d < break_cos) {
        break;
      }
      const float len = have_lengths ? spline.lengths_3d[end] - spline.lengths_3d[begin] : 0.0f;
      /* A gradual turn only splits once the minimum length is covered -- otherwise normal noise on
       * a dense mesh would shatter the curve into hundreds of windows. */
      if (d < turn_cos && len >= params.min_window_length) {
        break;
      }
      end++;
    }
    const bool last_allowed = r_ranges.size() + 1 >= params.max_frames;
    if (last_allowed && end < count - 1) {
      r_capped = true;
      end = count - 1;
    }
    r_ranges.append({begin, end, dominant_normal(spline, begin, end)});
    if (end >= count - 1) {
      break;
    }
    /* Half-overlap -- but only across a GRADUAL join. Across a break the windows meet edge to edge:
     * each face's vertices are already rejected by orientation in the other's window, and an
     * overlap would only make them compete. */
    const bool broke_on_edge = math::dot(spline.normals_3d[end], spline.normals_3d[begin]) <
                               break_cos;
    begin = broke_on_edge ? end : std::max(begin + 1, (begin + end) / 2);
  }

  /* Grow every window past its boundaries so neighbours share a stretch to cross-fade over. Done as
   * a separate pass AFTER `dominant_normal` has already run on the cut-out (core) ranges: the grown
   * part of a window that crosses a break lies on the OTHER face, and letting it vote would drag a
   * short window's projection plane toward the very average this design exists to avoid. */
  if (have_lengths && params.overlap_length > 0.0f) {
    for (CurvePatchFrameRange &range : r_ranges) {
      int grown_begin = range.begin;
      while (grown_begin > 0 &&
             spline.lengths_3d[range.begin] - spline.lengths_3d[grown_begin - 1] <
                 params.overlap_length)
      {
        grown_begin--;
      }
      int grown_end = range.end;
      while (grown_end < count - 1 &&
             spline.lengths_3d[grown_end + 1] - spline.lengths_3d[range.end] <
                 params.overlap_length)
      {
        grown_end++;
      }
      range.begin = grown_begin;
      range.end = grown_end;
    }
  }
  return !r_ranges.is_empty();
}

void curve_patch_frames_build(const CurvePatchSpline &spline,
                              const float brush_radius,
                              const CurvePatchFramesParams &params,
                              const bool high_quality,
                              const float end_margin,
                              CurvePatchFrameSet &r_frames)
{
  r_frames.ready = false;
  r_frames.capped = false;
  if (spline.is_empty() || spline.lengths_3d.size() != spline.poly_3d.size()) {
    return;
  }

  Vector<CurvePatchFrameRange> ranges;
  bool capped = false;
  if (!curve_patch_frames_partition(spline, params, ranges, capped)) {
    /* No normals: a single window over the whole curve with the frozen plane -- current behavior. */
    ranges.clear();
    ranges.append({0, int(spline.poly_3d.size()) - 1, math::normalize(spline.plane_normal)});
  }
  r_frames.capped = capped;

  /* The vector is reused between re-stamps: #CurvePatchRibbonLut holds six internal vectors, and
   * recreating the set would throw away their per-LUT `source_hash` cache. */
  if (r_frames.frames.size() != ranges.size()) {
    r_frames.frames.resize(ranges.size());
  }

  const bool have_smooth = spline.normals_smooth_3d.size() == spline.poly_3d.size();
  const bool have_radii = spline.radii.size() == spline.poly_3d.size();
  const int last_index = int(spline.poly_3d.size()) - 1;

  for (const int fi : ranges.index_range()) {
    const CurvePatchFrameRange &range = ranges[fi];
    CurvePatchFrame &frame = r_frames.frames[fi];
    frame.normal = range.normal;

    /* A full cyclic frame includes the spline's duplicated closing sample. Rebuild it from the
     * unique samples and retain `cyclic`, otherwise the LUT sees a geometrically closed but
     * topologically open strip: the cyclic join rules and inward cap are silently disabled. A
     * partial frame remains open because it is bounded by a surface-normal handover. */
    const bool full_cyclic_frame = spline.cyclic && range.begin == 0 && range.end == last_index;
    const int n = range.end - range.begin + 1 - int(full_cyclic_frame);
    Vector<float3> sub_pos(n), sub_binormals(n);
    Vector<float> sub_radii;
    if (have_radii) {
      sub_radii.resize(n);
    }
    for (const int k : IndexRange(n)) {
      const int i = range.begin + k;
      sub_pos[k] = spline.poly_3d[i];
      /* The binormal comes from the SMOOTHED field: a discontinuous one would break `u` at an
       * oblique crossing. */
      const float3 nrm = have_smooth ? spline.normals_smooth_3d[i] : spline.plane_normal;
      const float3 b = math::cross(spline.tangents_3d[i], nrm);
      const float blen = math::length(b);
      sub_binormals[k] = blen > 1e-7f ?
                             b / blen :
                             math::normalize(math::cross(spline.tangents_3d[i], range.normal));
      if (have_radii) {
        sub_radii[k] = spline.radii[i];
      }
    }
    if (full_cyclic_frame) {
      sub_binormals.append(sub_binormals.first());
    }

    CurvePatchSpline sub;
    sub.build_from_positions(sub_pos.as_span(), sub_radii.as_span(), full_cyclic_frame);
    sub.plane_normal = range.normal;

    frame.s_offset = spline.lengths_3d[range.begin];
    frame.s_end = spline.lengths_3d[range.end];
    frame.s_center = spline.lengths_3d[(range.begin + range.end) / 2];
    /* Ramp only where this window hands over to a neighbour. At a real end of the curve there is
     * nobody to hand over to, so the weight stays 1 and the relief keeps full strength to the tip. */
    frame.fade_start = (range.begin == 0) ? 0.0f : params.overlap_length;
    frame.fade_end = (range.end == last_index) ? 0.0f : params.overlap_length;

    /* The margin applies only where the window borders a real end of the curve: extending an
     * interior join would push the strip outside the window that serves it. */
    const float margin_start = (range.begin == 0) ? end_margin : 0.0f;
    const float margin_end = (range.end == last_index) ? end_margin : 0.0f;

    curve_patch_ribbon_build(sub,
                             brush_radius,
                             frame.lut,
                             high_quality,
                             margin_start,
                             margin_end,
                             sub_binormals.as_span());

    frame.bb_min = float3(FLT_MAX);
    frame.bb_max = float3(-FLT_MAX);
    for (const float3 &p : sub_pos) {
      frame.bb_min = math::min(frame.bb_min, p);
      frame.bb_max = math::max(frame.bb_max, p);
    }
    const float pad = brush_radius * 2.0f;
    frame.bb_min -= float3(pad);
    frame.bb_max += float3(pad);
  }

  /* Capping the window count alone does not bound memory: `res` is chosen adaptively from each
   * slice's extent, so many short windows can still add up past the budget. */
  int64_t total_pixels = 0;
  for (const CurvePatchFrame &frame : r_frames.frames) {
    total_pixels += int64_t(frame.lut.res) * int64_t(frame.lut.res);
  }
  if (total_pixels > CURVE_PATCH_MAX_LUT_PIXELS) {
    r_frames.capped = true;
  }

  r_frames.ready = !r_frames.frames.is_empty() && r_frames.frames.first().lut.ready;
}

/* Hermite ramp on `[0, 1]`, flat at both ends. Used for every blend weight below so a window's
 * contribution reaches zero with zero slope -- a linear ramp still leaves a visible crease where it
 * meets the flat region, which is the whole class of defect this blending exists to remove. */
static float smoothstep01(const float t)
{
  if (!(t > 0.0f)) {
    return 0.0f;
  }
  if (t >= 1.0f) {
    return 1.0f;
  }
  return t * t * (3.0f - 2.0f * t);
}

/* How much this window's opinion counts at global arc length `s`: full inside its own span, ramping
 * to nothing across each of its interior boundaries. */
static float frame_span_weight(const CurvePatchFrame &frame, const float s)
{
  float weight = 1.0f;
  if (frame.fade_start > 0.0f) {
    weight *= smoothstep01((s - frame.s_offset) / frame.fade_start);
  }
  if (frame.fade_end > 0.0f) {
    weight *= smoothstep01((frame.s_end - s) / frame.fade_end);
  }
  return weight;
}

/* How much this window's opinion counts for a vertex whose surface normal is `vertex_normal`.
 *
 * This replaces a hard `dot <= 0.3` rejection. The rejection was correct in intent -- a vertex on
 * the side face has no business being projected through the top face's plane -- but being binary it
 * put a step exactly where the two windows hand over, on top of the handover the span weight was
 * already smoothing. Both have to ramp or neither helps. */
static float frame_orientation_weight(const float3 &vertex_normal, const float3 &frame_normal)
{
  constexpr float reject_below = 0.3f;
  constexpr float accept_above = 0.6f;
  const float alignment = math::dot(vertex_normal, frame_normal);
  return smoothstep01((alignment - reject_below) / (accept_above - reject_below));
}

int CurvePatchFrameSet::sample(const float3 &co,
                               const float3 &vertex_normal,
                               float2 r_uv[2],
                               float3 r_frame_normal[2]) const
{
  if (!ready) {
    return 0;
  }
  struct Candidate {
    float2 uv;
    float3 normal;
    float weight;
  };
  Vector<Candidate, 8> candidates;
  for (const CurvePatchFrame &frame : frames) {
    if (co.x < frame.bb_min.x || co.y < frame.bb_min.y || co.z < frame.bb_min.z ||
        co.x > frame.bb_max.x || co.y > frame.bb_max.y || co.z > frame.bb_max.z)
    {
      continue;
    }
    /* Weighed PER WINDOW -- that is the whole point: a vertex on the side face weighs nothing in
     * the top face's window and full in the side face's own, instead of being rejected globally. */
    const float orientation_weight = frame_orientation_weight(vertex_normal, frame.normal);
    if (!(orientation_weight > 0.0f)) {
      continue;
    }
    float2 local[2];
    const int num = frame.lut.sample(co, local);
    for (const int b : IndexRange(num)) {
      const float2 global_uv(local[b].x, local[b].y + frame.s_offset);
      const float weight = orientation_weight * frame_span_weight(frame, global_uv.y);
      if (!(weight > 0.0f)) {
        continue;
      }
      candidates.append({global_uv, frame.normal, weight});
    }
  }
  if (candidates.is_empty()) {
    return 0;
  }
  /* Heaviest first, so the first candidate to open a group anchors it and supplies its reported
   * normal. `uv.y` breaks ties, which keeps the result independent of the order the windows were
   * walked in (`std::sort` is not stable). */
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.weight != b.weight ? a.weight > b.weight : a.uv.y < b.uv.y;
  });

  /* Grouping by `s`: two distant stretches of a self-crossing curve are separate branches and must
   * not be collapsed -- the caller resolves those by max `|height|`. Windows covering the SAME
   * stretch land in one group and are averaged, which is what carries `u` continuously through a
   * window join instead of stepping from one window's parameterization to the next. */
  struct Group {
    float2 uv_sum;
    float3 normal;
    float weight_sum;
    float anchor_s;
  };
  const float v_threshold = frames.first().lut.v_threshold;
  Vector<Group, 2> groups;
  for (const Candidate &candidate : candidates) {
    int group = -1;
    for (const int g : groups.index_range()) {
      if (std::abs(groups[g].anchor_s - candidate.uv.y) <= v_threshold) {
        group = g;
        break;
      }
    }
    if (group < 0) {
      if (groups.size() >= 2) {
        continue;
      }
      groups.append({candidate.uv * candidate.weight,
                     candidate.normal,
                     candidate.weight,
                     candidate.uv.y});
    }
    else {
      groups[group].uv_sum += candidate.uv * candidate.weight;
      groups[group].weight_sum += candidate.weight;
    }
  }
  for (const int g : groups.index_range()) {
    r_uv[g] = groups[g].uv_sum / groups[g].weight_sum;
    r_frame_normal[g] = groups[g].normal;
  }
  return int(groups.size());
}

void CurvePatchFrameSet::clear()
{
  frames.clear();
  ready = false;
  capped = false;
}

}  // namespace blender::bke
