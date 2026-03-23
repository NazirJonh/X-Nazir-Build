/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>
#include <limits>

#include "paint_gradient_core.hh"

#include "BLI_array.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::tests {

using namespace gradient;

TEST(sculpt_paint_gradient_core, linear_projection_and_clamp)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::World;
  params.start_ws = float3(0.0f, 0.0f, 0.0f);
  params.end_ws = float3(2.0f, 0.0f, 0.0f);
  params.clamp_to_range = true;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(0.0f, 0.0f, 0.0f)), 0.0f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(3.0f, 0.0f, 0.0f)), 1.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, linear_projection_without_clamp)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::World;
  params.start_ws = float3(0.0f, 0.0f, 0.0f);
  params.end_ws = float3(2.0f, 0.0f, 0.0f);
  params.clamp_to_range = false;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(-1.0f, 0.0f, 0.0f)), -0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(3.0f, 0.0f, 0.0f)), 1.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, radial_normalization)
{
  Params params;
  params.type = Type::Radial;
  params.space = Space::Screen;
  params.start_ss = float2(10.0f, 10.0f);
  params.end_ss = float2(20.0f, 10.0f);
  params.clamp_to_range = true;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(10.0f, 10.0f, 0.0f)), 0.0f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(15.0f, 10.0f, 0.0f)), 0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(20.0f, 10.0f, 0.0f)), 1.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, radial_without_clamp_can_exceed_one)
{
  Params params;
  params.type = Type::Radial;
  params.space = Space::Screen;
  params.start_ss = float2(10.0f, 10.0f);
  params.end_ss = float2(20.0f, 10.0f);
  params.clamp_to_range = false;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(30.0f, 10.0f, 0.0f)), 2.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, uv_space_uses_2d_evaluation)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::UV;
  params.start_ss = float2(0.0f, 0.0f);
  params.end_ss = float2(2.0f, 0.0f);
  params.clamp_to_range = true;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(1.0f, 0.0f, 123.0f)), 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, uv_radial_ignores_z)
{
  Params params;
  params.type = Type::Radial;
  params.space = Space::UV;
  params.start_ss = float2(0.0f, 0.0f);
  params.end_ss = float2(0.0f, 2.0f);
  params.clamp_to_range = true;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(0.0f, 1.0f, 999.0f)), 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, degenerate_start_end_is_finite)
{
  Params linear;
  linear.type = Type::Linear;
  linear.space = Space::World;
  linear.start_ws = float3(1.0f, 2.0f, 3.0f);
  linear.end_ws = linear.start_ws;
  const std::unique_ptr<Calculator> linear_calc = create(linear);
  const float linear_value = linear_calc->evaluate(float3(4.0f, 5.0f, 6.0f));
  EXPECT_TRUE(std::isfinite(linear_value));
  EXPECT_EQ(linear_value, 0.0f);

  Params radial;
  radial.type = Type::Radial;
  radial.space = Space::Screen;
  radial.start_ss = float2(4.0f, 5.0f);
  radial.end_ss = radial.start_ss;
  const std::unique_ptr<Calculator> radial_calc = create(radial);
  const float radial_value = radial_calc->evaluate(float3(9.0f, 9.0f, 0.0f));
  EXPECT_TRUE(std::isfinite(radial_value));
  EXPECT_EQ(radial_value, 0.0f);
}

TEST(sculpt_paint_gradient_core, non_finite_input_is_safely_zero)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::World;
  params.start_ws = float3(0.0f, 0.0f, 0.0f);
  params.end_ws = float3(1.0f, 0.0f, 0.0f);
  params.clamp_to_range = false;

  const std::unique_ptr<Calculator> calc = create(params);

  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();

  EXPECT_EQ(calc->evaluate(float3(inf, 0.0f, 0.0f)), 0.0f);
  EXPECT_EQ(calc->evaluate(float3(nan, 0.0f, 0.0f)), 0.0f);
}

TEST(sculpt_paint_gradient_core, hardness_is_clamped_to_zero_one)
{
  Params low_hardness;
  low_hardness.type = Type::Linear;
  low_hardness.space = Space::World;
  low_hardness.start_ws = float3(0.0f);
  low_hardness.end_ws = float3(1.0f, 0.0f, 0.0f);
  low_hardness.hardness = -5.0f;

  Params high_hardness = low_hardness;
  high_hardness.hardness = 5.0f;

  const std::unique_ptr<Calculator> calc_low = create(low_hardness);
  const std::unique_ptr<Calculator> calc_high = create(high_hardness);

  const float sample_low = calc_low->evaluate(float3(0.5f, 0.0f, 0.0f));
  const float sample_high = calc_high->evaluate(float3(0.5f, 0.0f, 0.0f));

  EXPECT_TRUE(std::isfinite(sample_low));
  EXPECT_TRUE(std::isfinite(sample_high));
  EXPECT_GE(sample_low, 0.0f);
  EXPECT_LE(sample_low, 1.0f);
  EXPECT_NEAR(sample_high, 0.5f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, hardness_does_not_modify_outside_zero_one)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::World;
  params.start_ws = float3(0.0f, 0.0f, 0.0f);
  params.end_ws = float3(1.0f, 0.0f, 0.0f);
  params.clamp_to_range = false;
  params.hardness = 0.25f;

  const std::unique_ptr<Calculator> calc = create(params);

  EXPECT_NEAR(calc->evaluate(float3(-0.5f, 0.0f, 0.0f)), -0.5f, 1e-6f);
  EXPECT_NEAR(calc->evaluate(float3(2.0f, 0.0f, 0.0f)), 2.0f, 1e-6f);
}

TEST(sculpt_paint_gradient_core, linear_batch_matches_single)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::World;
  params.start_ws = float3(0.0f, 0.0f, 0.0f);
  params.end_ws = float3(1.0f, 0.0f, 0.0f);

  const std::unique_ptr<Calculator> calc = create(params);

  const Array<float3> positions = {float3(0.0f, 0.0f, 0.0f),
                                   float3(0.5f, 0.0f, 0.0f),
                                   float3(1.0f, 0.0f, 0.0f)};
  Array<float> factors(positions.size());
  calc->evaluate_batch(positions.as_span(), factors.as_mutable_span());

  for (const int i : positions.index_range()) {
    EXPECT_NEAR(factors[i], calc->evaluate(positions[i]), 1e-6f);
  }
}

TEST(sculpt_paint_gradient_core, uv_batch_matches_single)
{
  Params params;
  params.type = Type::Linear;
  params.space = Space::UV;
  params.start_ss = float2(0.0f, 0.0f);
  params.end_ss = float2(2.0f, 0.0f);

  const std::unique_ptr<Calculator> calc = create(params);

  const Array<float3> positions = {float3(0.0f, 0.0f, -5.0f),
                                   float3(1.0f, 0.0f, 9.0f),
                                   float3(2.0f, 0.0f, 42.0f)};
  Array<float> factors(positions.size());
  calc->evaluate_batch(positions.as_span(), factors.as_mutable_span());

  for (const int i : positions.index_range()) {
    EXPECT_NEAR(factors[i], calc->evaluate(positions[i]), 1e-6f);
  }
}

}  // namespace blender::ed::sculpt_paint::tests

