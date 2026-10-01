/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../shapes/paint_shape_uv_trace.hh"

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

TEST(ShapeUVTrace, SplitRunsKeepsContinuousCyclicLoop)
{
  const Vector<float2> uvs = {
      float2(0.0f, 0.0f), float2(0.1f, 0.0f), float2(0.1f, 0.1f), float2(0.0f, 0.1f)};
  const Vector<ShapeUVTraceRun> runs = shape_uv_trace_split_runs(uvs, true);
  ASSERT_EQ(runs.size(), 1);
  EXPECT_EQ(runs[0].indices, Vector<int>({0, 1, 2, 3}));
  EXPECT_TRUE(runs[0].cyclic);
}

TEST(ShapeUVTrace, SplitRunsSplitsAtSeam)
{
  /* A loop crossing one seam: two jumps (the seam and the wrap) split it into two open runs, and
   * the runs keep the outline's order. */
  const Vector<float2> uvs = {float2(0.0f, 0.0f),
                              float2(0.1f, 0.0f),
                              float2(0.1f, 0.1f),
                              float2(0.7f, 0.3f),
                              float2(0.8f, 0.3f),
                              float2(0.9f, 0.3f)};
  const Vector<ShapeUVTraceRun> runs = shape_uv_trace_split_runs(uvs, true);
  ASSERT_EQ(runs.size(), 2);
  EXPECT_FALSE(runs[0].cyclic);
  EXPECT_FALSE(runs[1].cyclic);
  /* Both runs are continuous: their first and last samples sit on either side of the seam. */
  for (const ShapeUVTraceRun &run : runs) {
    ASSERT_EQ(run.indices.size(), 3);
  }
  EXPECT_EQ(runs[0].indices, Vector<int>({0, 1, 2}));
  EXPECT_EQ(runs[1].indices, Vector<int>({3, 4, 5}));
}

TEST(ShapeUVTrace, SplitRunsOpenPolylineStaysOneRun)
{
  const Vector<float2> uvs = {float2(0.0f, 0.0f),
                              float2(0.05f, 0.0f),
                              float2(0.1f, 0.0f),
                              float2(0.15f, 0.0f)};
  const Vector<ShapeUVTraceRun> runs = shape_uv_trace_split_runs(uvs, false);
  ASSERT_EQ(runs.size(), 1);
  EXPECT_FALSE(runs[0].cyclic);
  EXPECT_EQ(runs[0].indices.size(), 4);
}

TEST(ShapeUVTrace, JacobianMapsTriangleExactly)
{
  bool ok = false;
  const float2x2 jacobian = shape_uv_trace_jacobian(float2(0.0f, 0.0f),
                                                    float2(100.0f, 0.0f),
                                                    float2(0.0f, 50.0f),
                                                    float2(0.1f, 0.2f),
                                                    float2(0.2f, 0.2f),
                                                    float2(0.1f, 0.4f),
                                                    ok);
  ASSERT_TRUE(ok);
  /* One hundred plane px along X is 0.1 in U; fifty along Y is 0.2 in V. */
  const float2 uv = float2(0.1f, 0.2f) + jacobian * float2(50.0f, 25.0f);
  EXPECT_NEAR(uv.x, 0.15f, 1e-5f);
  EXPECT_NEAR(uv.y, 0.30f, 1e-5f);
}

TEST(ShapeUVTrace, JacobianRejectsDegenerateTriangle)
{
  bool ok = true;
  shape_uv_trace_jacobian(float2(0.0f, 0.0f),
                          float2(10.0f, 0.0f),
                          float2(20.0f, 0.0f),
                          float2(0.0f, 0.0f),
                          float2(0.1f, 0.0f),
                          float2(0.2f, 0.0f),
                          ok);
  EXPECT_FALSE(ok);
}

}  // namespace blender::ed::sculpt_paint::shape
