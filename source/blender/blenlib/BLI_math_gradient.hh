/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Gradient parameter helpers shared by every gradient evaluator (paint layers, Image Editor and
 * sculpt color gradients), so the same ramp looks the same everywhere.
 */

#include <algorithm>
#include <cmath>

namespace blender::math {

/** Usable range of the gradient midpoint; the generated shader nodes clamp to the same range. */
constexpr float gradient_midpoint_min = 0.05f;
constexpr float gradient_midpoint_max = 0.95f;

/** Wrap an unbounded gradient parameter into `[0, 1)` by repeating it. */
inline float gradient_repeat(const float t)
{
  return t - std::floor(t);
}

/** Fold an unbounded gradient parameter into `[0, 1]` as a ping-pong, for either sign of \a t. */
inline float gradient_reflect(const float t)
{
  const float wrapped = std::fmod(std::abs(t), 2.0f);
  return (wrapped > 1.0f) ? (2.0f - wrapped) : wrapped;
}

/**
 * Bend a `[0, 1]` gradient parameter so that \a midpoint maps to 0.5 (smooth, C1 continuous);
 * a centered midpoint is a pass-through.
 *
 * Two cubic Hermite segments meet at `(mid, 0.5)`. The tangents follow the Catmull-Rom rule over
 * the knots `(0, 0)`, `(mid, 0.5)`, `(1, 1)`, so all three are positive and both segments stay
 * monotone: no flat zones, no overshoot. At `mid = 0.5` the scaled tangents are all 0.5 and the
 * curve degenerates to the identity.
 */
inline float gradient_remap_midpoint(const float t, const float midpoint)
{
  if (t <= 0.0f) {
    return 0.0f;
  }
  if (t >= 1.0f) {
    return 1.0f;
  }
  const float mid = std::clamp(midpoint, gradient_midpoint_min, gradient_midpoint_max);
  if (std::abs(mid - 0.5f) < 1e-4f) {
    return t;
  }
  if (t <= mid) {
    const float u = t / mid;
    const float u2 = u * u;
    const float u3 = u2 * u;
    /* `h10 * 0.5 + h01 * 0.5 + h11 * mid`. */
    return (u3 - 2.0f * u2 + u) * 0.5f + (-2.0f * u3 + 3.0f * u2) * 0.5f + (u3 - u2) * mid;
  }
  const float im = 1.0f - mid;
  const float u = (t - mid) / im;
  const float u2 = u * u;
  const float u3 = u2 * u;
  /* `h00 * 0.5 + h10 * im + h01 + h11 * 0.5`. */
  return (2.0f * u3 - 3.0f * u2 + 1.0f) * 0.5f + (u3 - 2.0f * u2 + u) * im +
         (-2.0f * u3 + 3.0f * u2) + (u3 - u2) * 0.5f;
}

/**
 * Invert #gradient_remap_midpoint: the line parameter a ramp stop at \a position lands on. The
 * remap is monotone, so a bisection over `[0, 1]` is exact to any practical precision; the cubic
 * has no closed-form inverse worth the algebra. Used to place ramp stop markers on a gradient
 * gizmo's line, where the displayed position is the line parameter and the stop position is the
 * ramp parameter.
 */
inline float gradient_remap_midpoint_inverse(const float position, const float midpoint)
{
  if (position <= 0.0f) {
    return 0.0f;
  }
  if (position >= 1.0f) {
    return 1.0f;
  }
  float lo = 0.0f;
  float hi = 1.0f;
  for (int i = 0; i < 24; i++) {
    const float mid = 0.5f * (lo + hi);
    if (gradient_remap_midpoint(mid, midpoint) < position) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  return 0.5f * (lo + hi);
}

}  // namespace blender::math
