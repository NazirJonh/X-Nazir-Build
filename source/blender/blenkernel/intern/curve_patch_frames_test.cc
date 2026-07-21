/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "BKE_curve_patch.hh"

namespace blender::bke::tests {

/** A straight line along X of length 2.0, whose normal turns 90 degrees abruptly at the middle. */
static void build_edge_crossing(CurvePatchSpline &spline)
{
  Vector<float3> points, normals;
  for (const int i : IndexRange(21)) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
    normals.append(i < 10 ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f));
  }
  spline.build_from_positions(points.as_span(), {}, false, normals.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);
}

static CurvePatchFramesParams default_params()
{
  CurvePatchFramesParams params;
  params.min_window_length = 1.0f;
  params.turn_threshold_rad = float(M_PI) * 25.0f / 180.0f;
  params.break_threshold_rad = float(M_PI) * 60.0f / 180.0f;
  /* Non-zero on purpose: overlapping windows are the production configuration, so the cases below
   * exercise the blended handover rather than the edge-to-edge cut it replaced. */
  params.overlap_length = 0.2f;
  params.max_frames = 32;
  return params;
}

/** The pre-overlap configuration, kept so the cut points themselves stay under test independently
 * of how far the windows are then grown. */
static CurvePatchFramesParams no_overlap_params()
{
  CurvePatchFramesParams params = default_params();
  params.overlap_length = 0.0f;
  return params;
}

TEST(paint_curve_patch_frames, flat_surface_single_window)
{
  CurvePatchSpline spline;
  Vector<float3> points, normals;
  for (const int i : IndexRange(21)) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
    normals.append(float3(0.0f, 0.0f, 1.0f));
  }
  spline.build_from_positions(points.as_span(), {}, false, normals.as_span());

  Vector<CurvePatchFrameRange> ranges;
  bool capped = false;
  ASSERT_TRUE(curve_patch_frames_partition(spline, default_params(), ranges, capped));
  EXPECT_EQ(ranges.size(), 1);
  EXPECT_FALSE(capped);
  EXPECT_V3_NEAR(ranges[0].normal, float3(0.0f, 0.0f, 1.0f), 1e-5f);
}

TEST(paint_curve_patch_frames, sharp_edge_splits_despite_min_length)
{
  CurvePatchSpline spline;
  build_edge_crossing(spline);

  /* The break spans zero arc length and is therefore shorter than `min_window_length` -- it still
   * has to cut the window, otherwise one window would straddle both faces. */
  Vector<CurvePatchFrameRange> ranges;
  bool capped = false;
  ASSERT_TRUE(curve_patch_frames_partition(spline, default_params(), ranges, capped));
  ASSERT_GE(ranges.size(), 2);

  /* No window normal may come out as the averaged 45-degree one: that is the worst plane for both
   * faces at once, and exactly the defect the break's priority exists to prevent. */
  for (const CurvePatchFrameRange &range : ranges) {
    const float to_top = math::dot(range.normal, float3(0.0f, 0.0f, 1.0f));
    const float to_side = math::dot(range.normal, float3(1.0f, 0.0f, 0.0f));
    EXPECT_GT(std::max(to_top, to_side), 0.9f);
  }
  const float first_top = math::dot(ranges.first().normal, float3(0.0f, 0.0f, 1.0f));
  const float last_side = math::dot(ranges.last().normal, float3(1.0f, 0.0f, 0.0f));
  EXPECT_GT(first_top, 0.9f);
  EXPECT_GT(last_side, 0.9f);
}

TEST(paint_curve_patch_frames, overlap_grows_windows_past_the_break)
{
  CurvePatchSpline spline;
  build_edge_crossing(spline);

  Vector<CurvePatchFrameRange> tight, grown;
  bool capped = false;
  ASSERT_TRUE(curve_patch_frames_partition(spline, no_overlap_params(), tight, capped));
  ASSERT_TRUE(curve_patch_frames_partition(spline, default_params(), grown, capped));
  ASSERT_EQ(tight.size(), grown.size());
  ASSERT_GE(tight.size(), 2);

  /* Without overlap two windows share a single sample, so there is no stretch to cross-fade over
   * and the handover between them is a step by construction. With overlap they genuinely share
   * one. */
  EXPECT_EQ(tight[0].end, tight[1].begin);
  EXPECT_GT(grown[0].end, grown[1].begin);

  /* Growing must not move a window's projection plane: the grown part lies on the OTHER face, and
   * letting it vote is exactly the averaging this design exists to avoid. */
  for (const int i : tight.index_range()) {
    EXPECT_V3_NEAR(grown[i].normal, tight[i].normal, 1e-5f);
  }
}

TEST(paint_curve_patch_frames, noisy_normals_do_not_shatter)
{
  CurvePatchSpline spline;
  Vector<float3> points, normals;
  for (const int i : IndexRange(41)) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
    /* Noise within ~10 degrees: below the smooth-turn threshold, so it must not split windows. */
    const float wobble = 0.15f * std::sin(float(i) * 1.7f);
    normals.append(math::normalize(float3(wobble, 0.0f, 1.0f)));
  }
  spline.build_from_positions(points.as_span(), {}, false, normals.as_span());

  Vector<CurvePatchFrameRange> ranges;
  bool capped = false;
  ASSERT_TRUE(curve_patch_frames_partition(spline, default_params(), ranges, capped));
  /* The curve is 4.0 long at a `min_window_length` of 1.0 -- more than 8 windows means it split on
   * the noise. */
  EXPECT_LE(ranges.size(), 8);
}

TEST(paint_curve_patch_frames, frame_cap_reports_capped)
{
  CurvePatchSpline spline;
  Vector<float3> points, normals;
  /* A sawtooth of breaks: every sample tears the window open, so without a cap the window count
   * would run past the ceiling. */
  for (const int i : IndexRange(41)) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
    normals.append(i % 2 == 0 ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f));
  }
  spline.build_from_positions(points.as_span(), {}, false, normals.as_span());

  CurvePatchFramesParams params = default_params();
  params.max_frames = 4;
  Vector<CurvePatchFrameRange> ranges;
  bool capped = false;
  ASSERT_TRUE(curve_patch_frames_partition(spline, params, ranges, capped));
  EXPECT_LE(ranges.size(), 4);
  EXPECT_TRUE(capped);
  /* The tail folds into the last window, so coverage stays complete. */
  EXPECT_EQ(ranges.last().end, int(spline.poly_3d.size()) - 1);
}

/** A curve crossing an edge (the edge running along Y) at `cross_angle_deg` to the edge line. */
static void build_angled_edge(CurvePatchSpline &spline, const float cross_angle_deg)
{
  const float a = float(M_PI) * cross_angle_deg / 180.0f;
  Vector<float3> points, normals;
  for (int i = -10; i <= 10; i++) {
    const float t = float(i) * 0.1f;
    if (i < 0) {
      points.append(float3(t * std::cos(a), t * std::sin(a), 0.0f));
      normals.append(float3(0.0f, 0.0f, 1.0f));
    }
    else {
      points.append(float3(0.0f, t * std::sin(a), -t * std::cos(a)));
      normals.append(float3(1.0f, 0.0f, 0.0f));
    }
  }
  spline.build_from_positions(points.as_span(), {}, false, normals.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);
}

TEST(paint_curve_patch_frames, u_is_continuous_across_join)
{
  /* 0 degrees -- a perpendicular crossing -- is protected by the geometry itself and passes even
   * without smoothing; the oblique angles are the real check, hence the parameterization. */
  for (const float angle : {0.0f, 30.0f, 45.0f, 60.0f}) {
    CurvePatchSpline spline;
    build_angled_edge(spline, angle);
    curve_patch_spline_smooth_normals(spline, 0.4f);

    CurvePatchFramesParams params = default_params();
    params.min_window_length = 0.4f;
    CurvePatchFrameSet frames;
    curve_patch_frames_build(spline, 0.25f, params, false, 0.0f, frames);
    ASSERT_TRUE(frames.ready) << "angle " << angle;

    /* Walk across the curve in the join zone and require `u` not to jump. */
    float prev_u = 0.0f;
    bool have_prev = false;
    for (int i = -6; i <= 6; i++) {
      const float3 probe = spline.evaluate(spline.total_length() * 0.5f) +
                           float3(0.0f, float(i) * 0.02f, 0.0f);
      float2 uv[2];
      float3 frame_normal[2];
      if (frames.sample(probe, float3(0.0f, 0.0f, 1.0f), uv, frame_normal) == 0) {
        continue;
      }
      if (have_prev) {
        EXPECT_LT(std::abs(uv[0].x - prev_u), 0.35f) << "angle " << angle << " step " << i;
      }
      prev_u = uv[0].x;
      have_prev = true;
    }
  }
}

TEST(paint_curve_patch_frames, s_is_continuous_across_join)
{
  CurvePatchSpline spline;
  build_edge_crossing(spline);
  curve_patch_spline_smooth_normals(spline, 0.4f);

  CurvePatchFramesParams params = default_params();
  params.min_window_length = 0.4f;
  CurvePatchFrameSet frames;
  curve_patch_frames_build(spline, 0.25f, params, false, 0.0f, frames);
  ASSERT_TRUE(frames.ready);
  ASSERT_GE(frames.frames.size(), 2);

  /* Walk ALONG the curve through the window join: the global arc length has to grow monotonically,
   * without a jump at the boundary. Without `s_offset` each window would restart its count. */
  float prev_s = -FLT_MAX;
  for (int i = 4; i <= 16; i++) {
    const float3 probe = spline.evaluate(spline.total_length() * float(i) / 20.0f);
    float2 uv[2];
    float3 frame_normal[2];
    /* The probe's normal comes from the curve itself, so it passes the orientation culling in the
     * window this stretch belongs to. */
    const float3 probe_normal =
        spline.normals_3d[std::min(int(spline.normals_3d.size()) - 1,
                                   i * int(spline.poly_3d.size()) / 20)];
    if (frames.sample(probe, probe_normal, uv, frame_normal) == 0) {
      continue;
    }
    EXPECT_GT(uv[0].y, prev_s) << "step " << i;
    prev_s = uv[0].y;
  }
}

TEST(paint_curve_patch_frames, vertex_on_side_face_gets_uv)
{
  /* Regression on the original bug: before the normal split, a vertex on a face turned 90 degrees
   * away from the frozen plane was culled globally and the relief broke off there. */
  CurvePatchSpline spline;
  build_angled_edge(spline, 0.0f);
  curve_patch_spline_smooth_normals(spline, 0.4f);

  CurvePatchFramesParams params = default_params();
  params.min_window_length = 0.4f;
  CurvePatchFrameSet frames;
  curve_patch_frames_build(spline, 0.25f, params, false, 0.0f, frames);
  ASSERT_TRUE(frames.ready);

  /* A point on the side face (normal +X), squarely perpendicular to the frozen +Z plane. */
  const float3 probe = spline.evaluate(spline.total_length() * 0.75f);
  float2 uv[2];
  float3 frame_normal[2];
  EXPECT_GE(frames.sample(probe, float3(1.0f, 0.0f, 0.0f), uv, frame_normal), 1);
  /* And the window that served it really is the side one, not an averaged plane. */
  EXPECT_GT(math::dot(frame_normal[0], float3(1.0f, 0.0f, 0.0f)), 0.9f);
}

TEST(paint_curve_patch_frames, single_window_matches_ribbon)
{
  CurvePatchSpline spline;
  Vector<float3> points, normals;
  for (const int i : IndexRange(21)) {
    points.append(float3(float(i) * 0.1f, 0.0f, 0.0f));
    normals.append(float3(0.0f, 0.0f, 1.0f));
  }
  spline.build_from_positions(points.as_span(), {}, false, normals.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);
  curve_patch_spline_smooth_normals(spline, 0.4f);

  CurvePatchFrameSet frames;
  curve_patch_frames_build(spline, 0.5f, default_params(), false, 0.0f, frames);
  ASSERT_TRUE(frames.ready);
  ASSERT_EQ(frames.frames.size(), 1);

  CurvePatchRibbonLut lut;
  curve_patch_ribbon_build(spline, 0.5f, lut);
  ASSERT_TRUE(lut.ready);

  /* The vertex passes the orientation culling by construction, otherwise the two semantics would
   * not line up: `CurvePatchFrameSet::sample()` culls, `CurvePatchRibbonLut` does not. */
  const float3 probe(1.0f, -0.25f, 0.0f);
  float2 uv_frames[2], uv_lut[2];
  float3 frame_normal[2];
  ASSERT_EQ(frames.sample(probe, float3(0.0f, 0.0f, 1.0f), uv_frames, frame_normal),
            lut.sample(probe, uv_lut));
  EXPECT_NEAR(uv_frames[0].x, uv_lut[0].x, 1e-4f);
  EXPECT_NEAR(uv_frames[0].y, uv_lut[0].y, 1e-4f);
}

TEST(paint_curve_patch_frames, full_cyclic_frame_retains_closed_ribbon)
{
  /* Surface snapshots use CurvePatchFrameSet rather than the single ribbon LUT. The full frame of
   * a cyclic spline must retain that topology; rebuilding its already-closed positions as open
   * disables the ribbon's cyclic inward cap and lets opposite sides fight over the loop interior. */
  constexpr float loop_radius = 2.0f;
  Vector<float3> points, normals;
  for (const int i : IndexRange(64)) {
    const float angle = 2.0f * float(M_PI) * float(i) / 64.0f;
    points.append(float3(loop_radius * std::cos(angle), loop_radius * std::sin(angle), 0.0f));
    normals.append(float3(0.0f, 0.0f, 1.0f));
  }

  CurvePatchSpline spline;
  spline.build_from_positions(points.as_span(), {}, true, normals.as_span());
  spline.plane_normal = float3(0.0f, 0.0f, 1.0f);
  curve_patch_spline_smooth_normals(spline, 0.4f);

  CurvePatchFrameSet frames;
  curve_patch_frames_build(spline, 2.5f, default_params(), false, 0.0f, frames);
  ASSERT_TRUE(frames.ready);
  ASSERT_EQ(frames.frames.size(), 1);

  float2 uv[2];
  float3 frame_normal[2];
  EXPECT_EQ(frames.sample(float3(0.0f), float3(0.0f, 0.0f, 1.0f), uv, frame_normal), 0);
}

}  // namespace blender::bke::tests
