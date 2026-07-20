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

#include <cstdint>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::ed::sculpt_paint {

struct CurvePatchSpline;

struct CurvePatchRibbonLut {
  /** The LUT is `res * res` pixels; 0 until built. Chosen adaptively from the curve's extent so U
   * precision stays sub-strip even for long thin curves (see #curve_patch_ribbon_build). */
  int res = 0;
  /** Per-pixel `(u, s)` of the PRIMARY branch: `u` in `[-1, 1]` across the strip (sign matches the
   * old `-lateral / radius` convention, positive toward `cross(tangent, plane_normal)`), `s` the
   * world-space arc length along the curve. `u == FLT_MAX` marks a pixel no ribbon quad covered. */
  Vector<float2> uv;
  /** Squared distance between the pixel center and its bilinear fit -- overlap arbitration during
   * rasterization and anchor selection during sampling. */
  Vector<float> dist_sq;
  /** Subdivided-grid row that wrote each pixel. -1 = empty. */
  Vector<int> row;

  /** SECONDARY branch covering the same pixel: a second, distant stretch of the curve that also
   * overlaps here (the curve running close to itself). Same encoding and empty marker as the
   * primary arrays above; empty whenever only one stretch covers the pixel.
   *
   * Keeping the runner-up rather than discarding it is what lets the relief merge two parallel
   * stretches instead of picking one and leaving a hard seam along their medial axis -- see
   * #sample and the branch loop in `curve_patch_apply_relief_action()`. */
  Vector<float2> uv2;
  Vector<float> dist_sq2;
  Vector<int> row2;
  /** 2D bounding box min of the projected grid (with margin). */
  float2 bb_min = {};
  /** `res / (bb_max - bb_min)` per axis. */
  float2 inv_extent = {};
  /** Orthonormal in-plane axes used to project queries; both perpendicular to the patch's frozen
   * `plane_normal`. */
  float3 axis_x = {};
  float3 axis_y = {};
  /** Max arc-length spread within which two sampled candidates are treated as the same stretch of
   * the curve (half a brush radius -- same rule as Roll's `spline_uv()`). */
  float v_threshold = 0.0f;
  bool ready = false;

  /** Hash of the inputs this LUT was built from (polyline, radii, plane normal, brush radius,
   * quality setting).
   * #curve_patch_ribbon_build returns immediately when it matches, so the re-stamps that do not
   * touch the curve at all -- a strength-slider drag, a Length-mode change, a re-stamp triggered
   * by an event that moved nothing -- reuse the LUT instead of rebuilding it. */
  uint64_t source_hash = 0;

  void clear();

  /**
   * Projects `co` onto the ribbon plane and bilinearly samples the LUT.
   *
   * Where the curve runs close to itself, two distinct stretches can legitimately cover `co`, and
   * committing to one of them leaves a hard seam along their medial axis (the texture's
   * along-length coordinate jumps from one stretch's arc length to the other's). Both are reported
   * instead so the caller can evaluate the relief for each and merge them.
   *
   * \param r_uv: filled with up to two `(u, s)` pairs as documented on #uv, ordered best-fitting
   * first. Entries past the return value are untouched.
   * \return the number of distinct stretches covering `co` (0, 1 or 2). 0 means the LUT is not
   * ready, `co` projects outside the rasterized ribbon, or every neighboring pixel is empty --
   * callers reject such vertices, which reproduces the "outside the strip / past the curve ends"
   * rejections by construction.
   */
  int sample(const float3 &co, float2 r_uv[2]) const;
};

/**
 * Builds the whole-curve ribbon LUT from an already-rebuilt spline. Reads `spline.poly_3d`,
 * `spline.tangents_3d`, `spline.lengths_3d`, `spline.radii` and `spline.plane_normal`. The
 * world-space half-width at vertex `i` is `spline.radii[i] * brush_radius` (`brush_radius` alone
 * when `radii` is empty). Leaves `r_lut` unusable (`ready == false`) when the spline is empty or
 * degenerate.
 *
 * \param high_quality: builds at roughly double the pixel density (and a higher cap) for the
 * one-off re-stamp taken when a patch is committed. The supersampled relief that pass uses places
 * its samples a fraction of a strip-width apart, which the interactive resolution cannot resolve.
 * Interactive re-stamps pass false and keep the cheaper table.
 *
 * \param end_margin_start, end_margin_end: world-space distances to extend the strip PAST the
 * corresponding end of a non-cyclic curve, along that end's tangent. Stamps mode needs this because
 * a stamp centered on the very first or last point reaches half its own size beyond the curve, and
 * the part outside the rasterized strip would get no UV and be clipped by a hard straight edge. The
 * extension carries the end radius, so the strip keeps its width through it, and the arc length it
 * reports runs from `-end_margin_start` to `total_length + end_margin_end` -- `v` is raw arc length
 * and is deliberately allowed outside `[0, total_length]` there. A cyclic curve has no ends and is
 * never extended. Both zero (the default) reproduces the unextended strip exactly.
 *
 * The two are separate because a window of a multi-window build borders the curve's real end on one
 * side only -- extending the interior join would push the strip outside the window that serves it.
 *
 * \param binormals: the across-curve direction for each `poly_3d` sample. An empty span means
 * "derive them as `cross(T, plane_normal)`" -- the default, bit-for-bit the previous behavior. A
 * non-empty span must match `spline.poly_3d` in size; it is what allows building the strip over a
 * CONTINUOUS binormal field, whereas deriving from a single `plane_normal` breaks `u` wherever the
 * curve crosses an edge at an angle.
 */
void curve_patch_ribbon_build(const CurvePatchSpline &spline,
                              float brush_radius,
                              CurvePatchRibbonLut &r_lut,
                              bool high_quality = false,
                              float end_margin_start = 0.0f,
                              float end_margin_end = 0.0f,
                              Span<float3> binormals = {});

}  // namespace blender::ed::sculpt_paint
