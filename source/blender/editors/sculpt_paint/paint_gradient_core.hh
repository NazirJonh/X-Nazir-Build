/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Core gradient factor calculation engine.
 *
 * Provides a decoupled, reusable `gradient::Calculator` abstraction used by the
 * Sculpt Color Gradient tool, Vertex Color Gradient operator, and Weight Gradient
 * operator. All gradient math is encapsulated here, away from UI, paint backends,
 * or mesh data structures.
 *
 * Usage:
 * \code{.cpp}
 *   gradient::Params params;
 *   params.type = gradient::Type::Linear;
 *   params.space = gradient::Space::Screen;
 *   params.start_ss = float2(x0, y0);
 *   params.end_ss   = float2(x1, y1);
 *   auto calc = gradient::create(params);
 *   float t = calc->evaluate(position);
 * \endcode
 */

#include <memory>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

struct ARegion;
struct Brush;

namespace blender {
struct CurveMapping;
}

namespace blender::ed::sculpt_paint::gradient {

/* -------------------------------------------------------------------- */
/** \name Parameters
 * \{ */

/** Shape / projection mode of the gradient. */
enum class Type : uint8_t {
  /** t = project(P - start) onto the start→end axis, normalised by length. */
  Linear = 0,
  /** t = distance(P, start) / distance(end, start). */
  Radial = 1,
};

/**
 * Coordinate space the gradient is defined in.
 *
 * - Screen: inputs / evaluation use 2-D region-pixel coordinates.
 *   `start_ss` / `end_ss` are used; the z-component of positions passed to
 *   `Calculator::evaluate` is ignored.
 * - World: inputs use 3-D world-space positions (`start_ws` / `end_ws`).
 * - UV:    inputs / evaluation use the X-Y of whatever space the caller
 *   provides (UVs mapped into 2-D by the caller).  Like Screen, z is ignored.
 */
enum class Space : uint8_t {
  Screen = 0,
  World = 1,
  UV = 2,
};

/** Complete description of a gradient.  Passed to `create()`. */
struct Params {
  Type type = Type::Linear;
  Space space = Space::Screen;

  /* -- Screen-space and UV-space inputs -- */

  /** Start point in 2-D region-pixel (or UV-mapped) coordinates. */
  float2 start_ss = float2(0.0f);
  /** End / outer-radius point (same space as start_ss). */
  float2 end_ss = float2(0.0f);

  /* -- World-space inputs -- */

  /** Start point in 3-D world space (used when space == World). */
  float3 start_ws = float3(0.0f);
  /** End / outer-radius point in 3-D world space. */
  float3 end_ws = float3(0.0f);

  /* -- Shaping controls -- */

  /**
   * Hardness in `[0, 1)`.  Values outside this range are clamped on entry.
   * At 0.0 there is no effect.  Higher values push the transition toward the
   * end of the range (harder edge).
   */
  float hardness = 0.0f;

  /**
   * When true, the computed factor is clamped to `[0, 1]` before hardness is
   * applied inside the `[0, 1]` zone.  When false the raw un-clamped factor
   * is returned (useful for operators that want to extend or reflect).
   */
  bool clamp_to_range = true;

  /**
   * When true, any factor that falls *behind* the start point (i.e. the raw
   * value would be negative) is forced to 0.0, effectively cutting off the
   * gradient before the drag origin.
   */
  bool clip_before_start = false;

  /**
   * Optional user-supplied curve that remaps the final `[0, 1]` factor.
   * When non-null, `BKE_curvemapping_evaluate_premapped` is used.
   * Ownership is *not* transferred; the caller is responsible for lifetime.
   */
  const CurveMapping *curve = nullptr;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Calculator Interface
 * \{ */

/**
 * Abstract interface returned by `create()`.
 *
 * Implementations pre-compute all constant values derived from `Params` on
 * construction, keeping the hot `evaluate` / `evaluate_batch` paths cheap.
 */
class Calculator {
 public:
  virtual ~Calculator() = default;

  /**
   * Evaluate the gradient factor at a single position.
   *
   * For Screen / UV spaces the `position.z` component is ignored and
   * `position.xy` are treated as 2-D coordinates.
   * For World space all three components are used.
   *
   * The returned value is already processed by hardness, clamping, clipping,
   * and the optional curve as configured in the `Params` given to `create()`.
   *
   * Non-finite inputs (NaN / inf) are treated as zero factor.
   */
  virtual float evaluate(const float3 &position) const = 0;

  /**
   * Vectorised batch evaluation equivalent to calling `evaluate()` for each
   * element.  May be SIMD-optimised in concrete implementations.
   *
   * \param positions  Input positions; size must equal `r_factors`.
   * \param r_factors  Output factors, overwritten; must be pre-allocated.
   */
  virtual void evaluate_batch(Span<float3> positions,
                              MutableSpan<float> r_factors) const = 0;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Factory
 * \{ */

/**
 * Construct a `Calculator` for the given `params`.
 *
 * Never returns null; a degenerate gradient (zero-length axis) results in a
 * calculator that always returns 0.0.
 */
std::unique_ptr<Calculator> create(const Params &params);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared Utility Helpers
 * \{ */

/**
 * Compute the screen-space projected factor for `world_position`, accounting
 * for mirror and radial symmetry of the mesh.
 *
 * Iterates all symmetry axes enabled in `symmetry_flags` (bit-field
 * `PAINT_SYMM_*`), projects each symmetry-flipped position and returns the
 * maximum factor seen — matching the behaviour of other symmetry-aware
 * sculpt operators.
 *
 * \param region           3-D viewport region (must have regiondata set).
 * \param calculator       Pre-built Calculator to evaluate each projection.
 * \param world_position   Vertex / pixel world-space position.
 * \param symmetry_flags   `PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z` bits.
 * \param radial_symmetry  Per-axis radial repetition count (mesh->radial_symmetry).
 */
float paint_projected_gradient_factor_with_symmetry(const ARegion *region,
                                                    const Calculator &calculator,
                                                    const float3 &world_position,
                                                    int symmetry_flags,
                                                    const int8_t radial_symmetry[3]);

/**
 * Apply the final factor pipeline shared by all gradient operators:
 * - Apply optional brush falloff curve (`BKE_brush_curve_strength_clamped`).
 * - Multiply by `brush_alpha`.
 * - Optionally hard-clamp to `[0, brush_alpha]`.
 *
 * \param brush            Brush whose `curve` and `alpha` are used.
 * \param factor           Raw gradient factor (may be outside `[0, 1]`).
 * \param clamp_to_range   True → clamp raw factor to `[0, 1]` before curve.
 * \param brush_alpha      Brush alpha multiplier (usually `brush.alpha`).
 * \return                 Final blending weight ready for color interpolation.
 */
float paint_gradient_finalize_factor(const Brush &brush,
                                     float factor,
                                     bool clamp_to_range,
                                     float brush_alpha);

/** \} */

}  // namespace blender::ed::sculpt_paint::gradient
