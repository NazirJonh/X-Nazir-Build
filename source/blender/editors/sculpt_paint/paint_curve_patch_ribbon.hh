/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Ribbon-based UV parameterization for the Curve Patch relief.
 *
 * The former parameterization (`CurvePatchSpline::closest_point()`) mapped every mesh vertex
 * through a GLOBAL nearest-segment search. On the concave side of a sharp turn several curve
 * segments are near-equidistant, so neighboring vertices snapped to different segments and got
 * discontinuous arc-length values -- the texture visibly tore and fanned there (the classic
 * medial-axis fold of a closest-point parameterization).
 *
 * This module ports the approach proven by the Roll stroke method (`paint_stroke_roll.cc`): build
 * a quad-strip "ribbon" over the whole control polyline (borders at +/-radius along the binormal),
 * collapse inner-border self-intersections at sharp turns, smooth the grid with Catmull-Clark
 * passes + Laplacian iterations, then rasterize the grid's bilinear inverse into a 2D lookup
 * table. Per-vertex evaluation becomes a plane projection + one bilinear LUT sample -- every
 * vertex gets its UV from the specific ribbon quad covering it, so the mapping stays single-valued
 * and continuous through arbitrarily sharp turns.
 *
 * Differences from Roll's implementation (deliberate, driven by Curve Patch's static whole-curve
 * nature): the ribbon covers the ENTIRE curve (no per-dab evaluation window), the half-width
 * varies per vertex with the curve's `radius` attribute (no pressure), V is raw arc length, the
 * projection plane is the patch's frozen `plane_normal`, and no tangent field is stored (the
 * relief displaces along each vertex's own normal).
 */

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender::ed::sculpt_paint {

struct CurvePatchSpline;

struct CurvePatchRibbonLut {
  /** The LUT is `res * res` pixels; 0 until built. Chosen adaptively from the curve's extent so U
   * precision stays sub-strip even for long thin curves (see #curve_patch_ribbon_build). */
  int res = 0;
  /** Per-pixel `(u, s)`: `u` in `[-1, 1]` across the strip (sign matches the old
   * `-lateral / radius` convention, positive toward `cross(tangent, plane_normal)`), `s` the
   * world-space arc length along the curve. `u == FLT_MAX` marks a pixel no ribbon quad covered. */
  Vector<float2> uv;
  /** Squared distance between the pixel center and its bilinear fit -- overlap arbitration during
   * rasterization and the nearest-neighbor fallback during sampling. */
  Vector<float> dist_sq;
  /** Subdivided-grid row that wrote each pixel; earlier rows win on far-apart overlaps so
   * competing branches at a sharp turn do not flicker per pixel. -1 = empty. */
  Vector<int> row;
  /** 2D bounding box min of the projected grid (with margin). */
  float2 bb_min = {};
  /** `res / (bb_max - bb_min)` per axis. */
  float2 inv_extent = {};
  /** Orthonormal in-plane axes used to project queries; both perpendicular to the patch's frozen
   * `plane_normal`. */
  float3 axis_x = {};
  float3 axis_y = {};
  /** Max spread of the 4 sampled neighbors' V before #sample falls back to nearest-neighbor
   * (half a brush radius of arc length -- same rule as Roll's `spline_uv()`). */
  float v_threshold = 0.0f;
  bool ready = false;

  void clear();

  /**
   * Projects `co` onto the ribbon plane and bilinearly samples the LUT.
   * \param r_uv: `(u, s)` as documented on #uv.
   * \return false when the LUT is not ready, `co` projects outside the rasterized ribbon, or all
   * four neighboring pixels are empty -- callers reject such vertices, which reproduces the old
   * "outside the strip / past the curve ends" rejections by construction.
   */
  bool sample(const float3 &co, float2 &r_uv) const;
};

/**
 * Builds the whole-curve ribbon LUT from an already-rebuilt spline. Reads `spline.poly_3d`,
 * `spline.tangents_3d`, `spline.lengths_3d`, `spline.radii` and `spline.plane_normal`. The
 * world-space half-width at vertex `i` is `spline.radii[i] * brush_radius` (`brush_radius` alone
 * when `radii` is empty). Clears `r_lut` when the spline is empty or degenerate.
 */
void curve_patch_ribbon_build(const CurvePatchSpline &spline,
                              float brush_radius,
                              CurvePatchRibbonLut &r_lut);

}  // namespace blender::ed::sculpt_paint
