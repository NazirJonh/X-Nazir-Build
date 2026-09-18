/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_gradient_core.hh"

#include <cmath>
#include <limits>
#include <memory>

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"

#include "DNA_brush_types.h"

#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"

#include "ED_view3d.hh"

#include "paint_intern.hh"

namespace blender::ed::sculpt_paint::gradient {

/* -------------------------------------------------------------------- */
/** \name Internal helpers
 * \{ */

/**
 * Apply the hardness re-mapping to a raw factor value.
 *
 * Hardness `H ∈ [0,1)` maps the range `[H, 1]` onto `[0, 1]`:
 *   t' = (t - H) / (1 - H)  for  H ≤ t ≤ 1
 * Values outside `[0, 1]` are returned unchanged (hardness only operates
 * inside the clamped zone).
 */
static float apply_hardness(const float t, const float hardness)
{
  /* Fast-paths for the common trivial cases. */
  if (hardness <= 0.0f) {
    return t;
  }
  const float h = math::min(hardness, 0.9999f); /* prevent division by zero */
  if (t < 0.0f || t > 1.0f) {
    /* Outside [0,1]: hardness has no effect. */
    return t;
  }
  if (t < h) {
    return 0.0f;
  }
  return (t - h) / (1.0f - h);
}

/** Return 0.0 for any non-finite input, otherwise identity. */
static inline float safe_finite(const float v)
{
  return std::isfinite(v) ? v : 0.0f;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Calculator implementations
 * \{ */

/** Linear gradient in screen-space (or UV-space — both use 2-D XY). */
class LinearScreen2DCalculator final : public Calculator {
  float2 axis_;       /* (end - start), unit-length */
  float inv_len_sq_;  /* 1 / |end - start|² — zero when degenerate */
  float start_x_;
  float start_y_;
  float hardness_;
  bool clamp_;
  bool clip_;
  const CurveMapping *curve_;

 public:
  LinearScreen2DCalculator(const Params &p)
      : start_x_(p.start_ss.x),
        start_y_(p.start_ss.y),
        hardness_(math::clamp(p.hardness, 0.0f, 1.0f)),
        clamp_(p.clamp_to_range),
        clip_(p.clip_before_start),
        curve_(p.curve)
  {
    axis_ = p.end_ss - p.start_ss;
    const float len_sq = math::length_squared(axis_);
    inv_len_sq_ = (len_sq > 1e-12f) ? (1.0f / len_sq) : 0.0f;
  }

  float evaluate(const float3 &position) const override
  {
    if (inv_len_sq_ == 0.0f) {
      return 0.0f;
    }
    const float px = safe_finite(position.x) - start_x_;
    const float py = safe_finite(position.y) - start_y_;
    float t = (px * axis_.x + py * axis_.y) * inv_len_sq_;

    if (clip_ && t < 0.0f) {
      t = 0.0f;
    }
    if (clamp_) {
      t = math::clamp(t, 0.0f, 1.0f);
      t = apply_hardness(t, hardness_);
    }
    if (curve_ && clamp_) {
      t = BKE_curvemapping_evaluateF(curve_, 0, t);
    }
    return t;
  }

  void evaluate_batch(Span<float3> positions, MutableSpan<float> r_factors) const override
  {
    BLI_assert(positions.size() == r_factors.size());
    for (const int i : positions.index_range()) {
      r_factors[i] = this->evaluate(positions[i]);
    }
  }
};

/** Radial gradient in screen-space (or UV-space — both use 2-D XY). */
class RadialScreen2DCalculator final : public Calculator {
  float2 start_;
  float inv_radius_;  /* 1 / |end - start| — zero when degenerate */
  float hardness_;
  bool clamp_;
  const CurveMapping *curve_;

 public:
  RadialScreen2DCalculator(const Params &p)
      : start_(p.start_ss),
        hardness_(math::clamp(p.hardness, 0.0f, 1.0f)),
        clamp_(p.clamp_to_range),
        curve_(p.curve)
  {
    const float radius = math::length(p.end_ss - p.start_ss);
    inv_radius_ = (radius > 1e-12f) ? (1.0f / radius) : 0.0f;
  }

  float evaluate(const float3 &position) const override
  {
    if (inv_radius_ == 0.0f) {
      return 0.0f;
    }
    const float dx = safe_finite(position.x) - start_.x;
    const float dy = safe_finite(position.y) - start_.y;
    float t = std::sqrt(dx * dx + dy * dy) * inv_radius_;

    if (clamp_) {
      t = math::clamp(t, 0.0f, 1.0f);
      t = apply_hardness(t, hardness_);
    }
    if (curve_ && clamp_) {
      t = BKE_curvemapping_evaluateF(curve_, 0, t);
    }
    return t;
  }

  void evaluate_batch(Span<float3> positions, MutableSpan<float> r_factors) const override
  {
    BLI_assert(positions.size() == r_factors.size());
    for (const int i : positions.index_range()) {
      r_factors[i] = this->evaluate(positions[i]);
    }
  }
};

/** Linear gradient in 3-D world space. */
class LinearWorld3DCalculator final : public Calculator {
  float3 axis_;
  float inv_len_sq_;
  float3 start_;
  float hardness_;
  bool clamp_;
  bool clip_;
  const CurveMapping *curve_;

 public:
  LinearWorld3DCalculator(const Params &p)
      : start_(p.start_ws),
        hardness_(math::clamp(p.hardness, 0.0f, 1.0f)),
        clamp_(p.clamp_to_range),
        clip_(p.clip_before_start),
        curve_(p.curve)
  {
    axis_ = p.end_ws - p.start_ws;
    const float len_sq = math::length_squared(axis_);
    inv_len_sq_ = (len_sq > 1e-20f) ? (1.0f / len_sq) : 0.0f;
  }

  float evaluate(const float3 &position) const override
  {
    if (inv_len_sq_ == 0.0f) {
      return 0.0f;
    }
    const float3 p = float3(safe_finite(position.x) - start_.x,
                            safe_finite(position.y) - start_.y,
                            safe_finite(position.z) - start_.z);
    float t = math::dot(p, axis_) * inv_len_sq_;

    if (clip_ && t < 0.0f) {
      t = 0.0f;
    }
    if (clamp_) {
      t = math::clamp(t, 0.0f, 1.0f);
      t = apply_hardness(t, hardness_);
    }
    if (curve_ && clamp_) {
      t = BKE_curvemapping_evaluateF(curve_, 0, t);
    }
    return t;
  }

  void evaluate_batch(Span<float3> positions, MutableSpan<float> r_factors) const override
  {
    BLI_assert(positions.size() == r_factors.size());
    for (const int i : positions.index_range()) {
      r_factors[i] = this->evaluate(positions[i]);
    }
  }
};

/** Radial gradient in 3-D world space. */
class RadialWorld3DCalculator final : public Calculator {
  float3 start_;
  float inv_radius_;
  float hardness_;
  bool clamp_;
  const CurveMapping *curve_;

 public:
  RadialWorld3DCalculator(const Params &p)
      : start_(p.start_ws),
        hardness_(math::clamp(p.hardness, 0.0f, 1.0f)),
        clamp_(p.clamp_to_range),
        curve_(p.curve)
  {
    const float radius = math::length(p.end_ws - p.start_ws);
    inv_radius_ = (radius > 1e-20f) ? (1.0f / radius) : 0.0f;
  }

  float evaluate(const float3 &position) const override
  {
    if (inv_radius_ == 0.0f) {
      return 0.0f;
    }
    const float3 p(safe_finite(position.x) - start_.x,
                   safe_finite(position.y) - start_.y,
                   safe_finite(position.z) - start_.z);
    float t = math::length(p) * inv_radius_;

    if (clamp_) {
      t = math::clamp(t, 0.0f, 1.0f);
      t = apply_hardness(t, hardness_);
    }
    if (curve_ && clamp_) {
      t = BKE_curvemapping_evaluateF(curve_, 0, t);
    }
    return t;
  }

  void evaluate_batch(Span<float3> positions, MutableSpan<float> r_factors) const override
  {
    BLI_assert(positions.size() == r_factors.size());
    for (const int i : positions.index_range()) {
      r_factors[i] = this->evaluate(positions[i]);
    }
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Factory
 * \{ */

std::unique_ptr<Calculator> create(const Params &params)
{
  switch (params.type) {
    case Type::Linear: {
      switch (params.space) {
        case Space::World:
          return std::make_unique<LinearWorld3DCalculator>(params);
        case Space::Screen:
        case Space::UV:
        default:
          return std::make_unique<LinearScreen2DCalculator>(params);
      }
    }
    case Type::Radial: {
      switch (params.space) {
        case Space::World:
          return std::make_unique<RadialWorld3DCalculator>(params);
        case Space::Screen:
        case Space::UV:
        default:
          return std::make_unique<RadialScreen2DCalculator>(params);
      }
    }
  }
  /* Should never be reached. */
  return std::make_unique<LinearScreen2DCalculator>(params);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared Utility Helpers
 * \{ */

float paint_projected_gradient_factor_with_symmetry(const ARegion *region,
                                                    const Calculator &calculator,
                                                    const float3 &world_position,
                                                    const int symmetry_flags,
                                                    const int8_t radial_symmetry[3])
{
  float max_factor = 0.0f;

  /* Project to screen space if needed, evaluate, and fold into `max_factor`. */
  auto project_and_evaluate = [&](const float3 &pos) {
    float2 screen_co;
    if (region != nullptr &&
        ED_view3d_project_float_object(
            region, pos, screen_co, V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR) !=
            V3D_PROJ_RET_OK)
    {
      return;
    }

    /* Build a float3 suitable for both 2D and 3D calculators. */
    const float3 eval_pos = (region != nullptr) ? float3(screen_co.x, screen_co.y, 0.0f) : pos;

    const float factor = calculator.evaluate(eval_pos);
    max_factor = math::max(max_factor, factor);
  };

  /* Iterate all active symmetry passes (0 = no symmetry, 1-7 = XYZ combos). */
  for (int i = 0; i <= symmetry_flags; i++) {
    if (!is_symmetry_iteration_valid(i, symmetry_flags)) {
      continue;
    }

    const ePaintSymmetryFlags symm_pass = ePaintSymmetryFlags(i);
    const float3 flipped = symmetry_flip(world_position, symm_pass);

    project_and_evaluate(flipped);

    /* Radial repetitions, matching #do_radial_symmetry: one axis at a time (never combined
     * across axes), only when that axis's mirror flag is active for this symmetry pass. */
    for (int axis = 0; axis < 3; axis++) {
      const ePaintSymmetryFlags axis_flag = ePaintSymmetryFlags(PAINT_SYMM_X << axis);
      if (!(symm_pass & axis_flag)) {
        continue;
      }
      const int count = math::clamp(int(radial_symmetry[axis]), 1, 64);
      for (int r = 1; r < count; r++) {
        const float angle = float(r) * float(M_PI * 2.0) / float(count);
        float rot[3][3];
        axis_angle_to_mat3_single(rot, 'X' + axis, angle);
        float3 pos = flipped;
        mul_m3_v3(rot, pos);
        project_and_evaluate(pos);
      }
    }
  }

  return max_factor;
}

float paint_gradient_finalize_factor(const Brush &brush,
                                     const float factor,
                                     const bool clamp_to_range,
                                     const float brush_alpha)
{
  float t = factor;

  if (clamp_to_range) {
    t = math::clamp(t, 0.0f, 1.0f);
  }
  else if (t < 0.0f) {
    t = 0.0f;
  }

  /* Apply brush falloff curve.  The curve maps normalised distance [0,1] →
   * [0,1], which matches the factor range when clamped. */
  t = BKE_brush_curve_strength_clamped(&brush, t, 1.0f);

  /* Scale by brush alpha. */
  return t * brush_alpha;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::gradient
