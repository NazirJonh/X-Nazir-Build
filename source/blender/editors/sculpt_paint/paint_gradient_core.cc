/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_gradient_core.hh"

#include <algorithm>
#include <cmath>
#include <limits>

#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "BKE_colortools.hh"

namespace blender::ed::sculpt_paint::gradient {

namespace {

constexpr float kEpsilon = 1.0e-8f;

float clamp_factor(const float value, const bool clamp_to_range)
{
  return clamp_to_range ? std::clamp(value, 0.0f, 1.0f) : value;
}

float apply_hardness(const float value, const float hardness)
{
  const float hardness_clamped = std::clamp(hardness, 0.0f, 1.0f);
  if (hardness_clamped >= 1.0f || value <= 0.0f || value >= 1.0f) {
    return value;
  }

  /* Keep hardness = 1.0 as identity for parity with existing paths.
   * Lower hardness values compress mid-range factors towards zero.
   *
   * Important: Handle negative values (pixels "before" gradient start) specially.
   * std::pow with negative base and non-integer exponent returns NaN.
   * For gradient tools, we preserve the sign but apply hardness to the magnitude. */
  const float abs_value = std::abs(value);
  const float sign = (value >= 0.0f) ? 1.0f : -1.0f;

  const float exponent = 1.0f / std::max(hardness_clamped, kEpsilon);
  const float result = std::pow(abs_value, exponent);

  return sign * result;
}

float evaluate_curve(const CurveMapping *curve, const float value)
{
  if (curve == nullptr) {
    return value;
  }
  /* Curve mapping may not handle negative values correctly.
   * For gradient tools, we want to preserve the sign but apply the curve to the magnitude. */
  const float abs_value = std::abs(value);
  const float sign = (value >= 0.0f) ? 1.0f : -1.0f;
  const float result = BKE_curvemapping_evaluateF(curve, 0, abs_value);
  return sign * result;
}

float evaluate_linear_world(const Params &params, const float3 &position)
{
  const float3 axis = params.end_ws - params.start_ws;
  const float axis_len_sq = math::length_squared(axis);
  if (axis_len_sq <= kEpsilon) {
    return 0.0f;
  }
  return math::dot(position - params.start_ws, axis) / axis_len_sq;
}

float evaluate_linear_screen(const Params &params, const float3 &position)
{
  const float2 axis = params.end_ss - params.start_ss;
  const float axis_len_sq = math::length_squared(axis);
  if (axis_len_sq <= kEpsilon) {
    return 0.0f;
  }
  const float2 sample = position.xy();
  return math::dot(sample - params.start_ss, axis) / axis_len_sq;
}

float evaluate_radial_world(const Params &params, const float3 &position)
{
  const float radius = math::distance(params.start_ws, params.end_ws);
  if (radius <= kEpsilon || !std::isfinite(radius)) {
    return 0.0f;
  }
  return math::distance(params.start_ws, position) / radius;
}

float evaluate_radial_screen(const Params &params, const float3 &position)
{
  const float radius = math::distance(params.start_ss, params.end_ss);
  if (radius <= kEpsilon || !std::isfinite(radius)) {
    return 0.0f;
  }
  return math::distance(params.start_ss, position.xy()) / radius;
}

class GradientCalculator : public Calculator {
  public:
   explicit GradientCalculator(const Params &params) : params_(params) {}

   float evaluate(const float3 &position) const override
   {
     float factor = 0.0f;

     if (params_.type == Type::Linear) {
       switch (params_.space) {
         case Space::World:
           factor = evaluate_linear_world(params_, position);
           break;
         case Space::Screen:
         case Space::UV:
           factor = evaluate_linear_screen(params_, position);
           break;
       }
     }
     else {
       switch (params_.space) {
         case Space::World:
           factor = evaluate_radial_world(params_, position);
           break;
         case Space::Screen:
         case Space::UV:
           factor = evaluate_radial_screen(params_, position);
           break;
       }
     }

      if (!std::isfinite(factor)) {
        factor = 0.0f;
      }

      factor = clamp_factor(factor, params_.clamp_to_range);
      factor = apply_hardness(factor, params_.hardness);
      factor = evaluate_curve(params_.curve, factor);
      return clamp_factor(factor, params_.clamp_to_range);
   }

  float2 get_start_ss() const override
  {
    return params_.start_ss;
  }

  float2 get_end_ss() const override
  {
    return params_.end_ss;
  }

  bool is_radial() const override
  {
    return params_.type == Type::Radial;
  }

  bool get_clip_before_start() const override
  {
    return params_.clip_before_start;
  }

 private:
  Params params_;
};

}  // namespace

void Calculator::evaluate_batch(const Span<float3> positions, const MutableSpan<float> factors) const
{
  BLI_assert(positions.size() == factors.size());

  const int size = std::min(positions.size(), factors.size());
  for (const int i : IndexRange(size)) {
    factors[i] = this->evaluate(positions[i]);
  }
}

std::unique_ptr<Calculator> create(const Params &params)
{
  return std::make_unique<GradientCalculator>(params);
}

}  // namespace blender::ed::sculpt_paint::gradient

