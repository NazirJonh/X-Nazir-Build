/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_frames.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"

#include "paint_curve_patch_spline.hh"

namespace blender::ed::sculpt_paint {

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

    const int n = range.end - range.begin + 1;
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

    CurvePatchSpline sub;
    sub.build_from_positions(sub_pos.as_span(), sub_radii.as_span(), false);
    sub.plane_normal = range.normal;

    frame.s_offset = spline.lengths_3d[range.begin];
    frame.s_center = spline.lengths_3d[(range.begin + range.end) / 2];

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
    float score;
  };
  Vector<Candidate, 8> candidates;
  for (const CurvePatchFrame &frame : frames) {
    if (co.x < frame.bb_min.x || co.y < frame.bb_min.y || co.z < frame.bb_min.z ||
        co.x > frame.bb_max.x || co.y > frame.bb_max.y || co.z > frame.bb_max.z)
    {
      continue;
    }
    /* The orientation culling is PER WINDOW -- that is the whole point: a vertex on the side face
     * drops out of the top face's window and passes in the side face's own, instead of being
     * rejected globally. */
    if (math::dot(vertex_normal, frame.normal) <= 0.3f) {
      continue;
    }
    float2 local[2];
    const int num = frame.lut.sample(co, local);
    for (const int b : IndexRange(num)) {
      const float2 global_uv(local[b].x, local[b].y + frame.s_offset);
      candidates.append({global_uv, frame.normal, std::abs(global_uv.y - frame.s_center)});
    }
  }
  if (candidates.is_empty()) {
    return 0;
  }
  /* The best candidate within a group is the one with the smallest `score`. Sorting by it before
   * grouping makes the winner independent of the order the windows were walked in. */
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
    return a.score < b.score;
  });
  /* Grouping by `s`: two distant stretches of a self-crossing curve are separate branches and must
   * not be collapsed. Within a group the window whose center the vertex sits deepest in wins, since
   * the projection distortion is smallest there. `u` does not depend on that choice, so a change of
   * winner does not move it. */
  const float v_threshold = frames.first().lut.v_threshold;
  int found = 0;
  for (const Candidate &candidate : candidates) {
    int group = -1;
    for (const int g : IndexRange(found)) {
      if (std::abs(r_uv[g].y - candidate.uv.y) <= v_threshold) {
        group = g;
        break;
      }
    }
    if (group < 0) {
      if (found >= 2) {
        continue;
      }
      r_uv[found] = candidate.uv;
      r_frame_normal[found] = candidate.normal;
      found++;
    }
  }
  return found;
}

void CurvePatchFrameSet::clear()
{
  frames.clear();
  ready = false;
  capped = false;
}

}  // namespace blender::ed::sculpt_paint
