/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Unit-tests for the gradient core engine (`paint_gradient_core.hh`).
 * Run via ctest or `blender_test --gtest_filter=sculpt_paint_gradient_core*`.
 */

#include <cmath>
#include <limits>

#include "testing/testing.h"

#include "BLI_array.hh"

#include "../paint_gradient_core.hh"

namespace blender::ed::sculpt_paint::tests {

using namespace gradient;

/* -------------------------------------------------------------------- */
/** \name Linear – Screen-space (and UV-space)
 * \{ */

TEST(sculpt_paint_gradient_core, linear_ss_midpoint)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(2.0f, 0.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 0.0f)), 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, linear_ss_clamped_outside_range)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(2.0f, 0.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(-1.0f, 0.0f, 0.0f)), 0.0f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(3.0f, 0.0f, 0.0f)), 1.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, linear_ss_unclamped_outside_range)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(2.0f, 0.0f);
  p.clamp_to_range = false;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(-1.0f, 0.0f, 0.0f)), -0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(3.0f, 0.0f, 0.0f)), 1.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, linear_ss_clip_before_start)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(2.0f, 0.0f);
  p.clamp_to_range = false;
  p.clip_before_start = true;

  auto calc = create(p);
  /* Behind start: should be exactly 0 due to clipping. */
  EXPECT_NEAR(calc->evaluate(float3(-1.0f, 0.0f, 0.0f)), 0.0f, 1e-6f);
  /* In front of start: unchanged. */
  EXPECT_NEAR(calc->evaluate(float3(3.0f, 0.0f, 0.0f)), 1.5f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Radial – Screen-space
 * \{ */

TEST(sculpt_paint_gradient_core, radial_ss_center_is_zero)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::Screen;
  p.start_ss = float2(10.0f, 10.0f);
  p.end_ss = float2(20.0f, 10.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(10.0f, 10.0f, 0.0f)), 0.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, radial_ss_midpoint)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::Screen;
  p.start_ss = float2(10.0f, 10.0f);
  p.end_ss = float2(20.0f, 10.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(15.0f, 10.0f, 0.0f)), 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, radial_ss_edge_is_one)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::Screen;
  p.start_ss = float2(10.0f, 10.0f);
  p.end_ss = float2(20.0f, 10.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(20.0f, 10.0f, 0.0f)), 1.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, radial_ss_unclamped_can_exceed_one)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::Screen;
  p.start_ss = float2(10.0f, 10.0f);
  p.end_ss = float2(20.0f, 10.0f);
  p.clamp_to_range = false;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(30.0f, 10.0f, 0.0f)), 2.0f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV space: z component must be ignored
 * \{ */

TEST(sculpt_paint_gradient_core, uv_linear_ignores_z)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::UV;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(2.0f, 0.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  /* Different z values must yield identical factors. */
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 123.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, -999.0f)), 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, uv_radial_ignores_z)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::UV;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(0.0f, 2.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(0.0f, 1.0f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(0.0f, 1.0f, 999.0f)), 0.5f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Degenerate input (start == end)
 * \{ */

TEST(sculpt_paint_gradient_core, degenerate_linear_ss_is_zero)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(5.0f, 5.0f);
  p.end_ss = float2(5.0f, 5.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  const float v = calc->evaluate(float3(10.0f, 10.0f, 0.0f));
  EXPECT_TRUE(std::isfinite(v));
  EXPECT_EQ(v, 0.0f);
}

TEST(sculpt_paint_gradient_core, degenerate_radial_ss_is_zero)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::Screen;
  p.start_ss = float2(5.0f, 5.0f);
  p.end_ss = float2(5.0f, 5.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  const float v = calc->evaluate(float3(10.0f, 10.0f, 0.0f));
  EXPECT_TRUE(std::isfinite(v));
  EXPECT_EQ(v, 0.0f);
}

TEST(sculpt_paint_gradient_core, degenerate_linear_world_is_zero)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::World;
  p.start_ws = float3(1.0f, 2.0f, 3.0f);
  p.end_ws = float3(1.0f, 2.0f, 3.0f);

  auto calc = create(p);
  const float v = calc->evaluate(float3(4.0f, 5.0f, 6.0f));
  EXPECT_TRUE(std::isfinite(v));
  EXPECT_EQ(v, 0.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Non-finite input protection
 * \{ */

TEST(sculpt_paint_gradient_core, non_finite_input_returns_zero)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(1.0f, 0.0f);
  p.clamp_to_range = false;

  auto calc = create(p);

  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();

  EXPECT_EQ(calc->evaluate(float3(inf, 0.0f, 0.0f)), 0.0f);
  EXPECT_EQ(calc->evaluate(float3(nan, 0.0f, 0.0f)), 0.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hardness
 * \{ */

TEST(sculpt_paint_gradient_core, hardness_compresses_range)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(1.0f, 0.0f);
  p.clamp_to_range = true;
  p.hardness = 0.5f;

  auto calc = create(p);
  /* At t=0.5 with H=0.5: (0.5 - 0.5) / (1.0 - 0.5) = 0.0 */
  EXPECT_NEAR(calc->evaluate(float3(0.5f, 0.0f, 0.0f)), 0.0f, 1e-6f);
  /* At t=0.75: (0.75 - 0.5) / 0.5 = 0.5 */
  EXPECT_NEAR(calc->evaluate(float3(0.75f, 0.0f, 0.0f)), 0.5f, 1e-6f);
  /* At t=1.0: (1.0 - 0.5) / 0.5 = 1.0 */
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 0.0f)), 1.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, hardness_zero_has_no_effect)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(1.0f, 0.0f);
  p.clamp_to_range = true;
  p.hardness = 0.0f;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(0.5f, 0.0f, 0.0f)), 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, hardness_out_of_range_clamped_internally)
{
  /* Negative or > 1 hardness values must not produce NaN / inf. */
  for (const float h : {-5.0f, 5.0f}) {
    Params p;
    p.type = Type::Linear;
    p.space = Space::Screen;
    p.start_ss = float2(0.0f, 0.0f);
    p.end_ss = float2(1.0f, 0.0f);
    p.clamp_to_range = true;
    p.hardness = h;

    auto calc = create(p);
    const float v = calc->evaluate(float3(0.5f, 0.0f, 0.0f));
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_GE(v, 0.0f);
    EXPECT_LE(v, 1.0f);
  }
}

TEST(sculpt_paint_gradient_core, hardness_does_not_affect_out_of_range_factor)
{
  /* When clamp_to_range is false, hardness should not distort values outside [0,1]. */
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(1.0f, 0.0f);
  p.clamp_to_range = false;
  p.hardness = 0.25f;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(-0.5f, 0.0f, 0.0f)), -0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(2.0f, 0.0f, 0.0f)), 2.0f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name World-space Linear
 * \{ */

TEST(sculpt_paint_gradient_core, linear_world_midpoint)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::World;
  p.start_ws = float3(0.0f, 0.0f, 0.0f);
  p.end_ws = float3(2.0f, 0.0f, 0.0f);
  p.clamp_to_range = true;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(0.0f, 0.0f, 0.0f)), 0.0f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(2.0f, 0.0f, 0.0f)), 1.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, linear_world_unclamped)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::World;
  p.start_ws = float3(0.0f, 0.0f, 0.0f);
  p.end_ws = float3(2.0f, 0.0f, 0.0f);
  p.clamp_to_range = false;

  auto calc = create(p);
  EXPECT_NEAR(calc->evaluate(float3(-1.0f, 0.0f, 0.0f)), -0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(3.0f, 0.0f, 0.0f)), 1.5f, 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Batch evaluation must match single evaluation
 * \{ */

TEST(sculpt_paint_gradient_core, batch_linear_ss_matches_single)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::Screen;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(1.0f, 0.0f);
  p.clamp_to_range = true;

  auto calc = create(p);

  const Array<float3> positions = {float3(0.0f, 0.0f, 0.0f),
                                   float3(0.5f, 0.0f, 0.0f),
                                   float3(1.0f, 0.0f, 0.0f),
                                   float3(-0.5f, 0.0f, 0.0f),
                                   float3(1.5f, 0.0f, 0.0f)};
  Array<float> factors(positions.size());
  calc->evaluate_batch(positions.as_span(), factors.as_mutable_span());

  for (const int i : positions.index_range()) {
    EXPECT_NEAR(factors[i], calc->evaluate(positions[i]), 1e-6f);
  }
}

TEST(sculpt_paint_gradient_core, batch_radial_ss_matches_single)
{
  Params p;
  p.type = Type::Radial;
  p.space = Space::Screen;
  p.start_ss = float2(5.0f, 5.0f);
  p.end_ss = float2(10.0f, 5.0f);
  p.clamp_to_range = false;

  auto calc = create(p);

  const Array<float3> positions = {
      float3(5.0f, 5.0f, 0.0f),
      float3(7.5f, 5.0f, 0.0f),
      float3(10.0f, 5.0f, 0.0f),
      float3(15.0f, 5.0f, 0.0f),
  };
  Array<float> factors(positions.size());
  calc->evaluate_batch(positions.as_span(), factors.as_mutable_span());

  for (const int i : positions.index_range()) {
    EXPECT_NEAR(factors[i], calc->evaluate(positions[i]), 1e-6f);
  }
}

TEST(sculpt_paint_gradient_core, batch_uv_matches_single)
{
  Params p;
  p.type = Type::Linear;
  p.space = Space::UV;
  p.start_ss = float2(0.0f, 0.0f);
  p.end_ss = float2(2.0f, 0.0f);
  p.clamp_to_range = true;

  auto calc = create(p);

  const Array<float3> positions = {float3(0.0f, 0.0f, -5.0f),
                                   float3(1.0f, 0.0f, 9.0f),
                                   float3(2.0f, 0.0f, 42.0f)};
  Array<float> factors(positions.size());
  calc->evaluate_batch(positions.as_span(), factors.as_mutable_span());

  for (const int i : positions.index_range()) {
    EXPECT_NEAR(factors[i], calc->evaluate(positions[i]), 1e-6f);
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint::tests
