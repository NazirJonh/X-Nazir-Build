/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Arc-length-parameterized polyline used to project a texture/displacement onto a mesh surface
 * along the static, user-edited Curve Patch control curve. Trimmed port of the `RollSpline`
 * concept (see `155072.diff`, external reference, not present in this repository): no live
 * mouse-path recording, virtual backward extension, or LUT rasterization, since this curve is
 * small and fully rebuilt from scratch on every edit rather than growing continuously through a
 * live stroke.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::ed::sculpt_paint {

struct CurvePatchSpline {
  Vector<float3> poly_3d;
  /** Cumulative arc length up to and including `poly_3d[i]`; `lengths_3d[0] == 0`. */
  Vector<float> lengths_3d;
  /** Normalized tangent at each `poly_3d` vertex (central difference at interior points). */
  Vector<float3> tangents_3d;
  /** Per-`poly_3d`-vertex interpolated width (Curve Patch's per-point `radius` attribute,
   * tessellated to the same resolution as `poly_3d` via
   * `bke::CurvesGeometry::interpolate_to_evaluated()`). Empty unless `build_from_positions()` was
   * called with a non-empty `radii` -- `radius_at()` requires this to be populated. */
  Vector<float> radii;
  /** Frozen projection normal for the whole patch (defines "left"/"right" of the curve for
   * `closest_point()`'s signed lateral offset). Caller sets this before calling `closest_point`. */
  float3 plane_normal = float3(0.0f, 0.0f, 1.0f);

  void clear();
  bool is_empty() const;
  float total_length() const;

  /** Rebuilds `poly_3d`/`lengths_3d`/`tangents_3d` from an already-tessellated polyline.
   * `positions.size() < 2` clears the spline (treated as empty). `radii`, when non-empty, must be
   * the same size as `positions` -- pass an empty span when the caller does not need
   * `radius_at()`. */
  void build_from_positions(Span<float3> positions, Span<float> radii = {});

  /** Position on the spline at arc-length `s`, clamped to `[0, total_length()]`. Returns
   * `float3(0)` when `is_empty()`. */
  float3 evaluate(float s) const;

  /** Tangent at arc-length `s` (linear interpolation between the two bracketing polyline
   * tangents, re-normalized), clamped to `[0, total_length()]`. Returns `float3(1, 0, 0)` when
   * `is_empty()`. */
  float3 tangent_at(float s) const;

  /** Interpolated width at arc-length `s` (linear interpolation between the two bracketing
   * `radii` entries), clamped to `[0, total_length()]`. Requires `build_from_positions()` to have
   * been called with a non-empty `radii` matching `poly_3d` in size. */
  float radius_at(float s) const;

  /** Minimum SQUARED Euclidean distance from `query` to the polyline (nearest point on any
   * segment). Returns 0 when `is_empty()`. Cheaper than #closest_point (no tangent/lateral/normal
   * decomposition) -- used to cull pbvh nodes whose whole bounds fall outside the curve's falloff
   * tube before the per-vertex relief walk. */
  float distance_sq_to(const float3 &query) const;

  /**
   * Closest point on the polyline to `query`.
   * \param r_s: arc-length of the closest point, normally clamped to `[0, total_length()]`.
   *   Exception: when `query` projects past one of the polyline's own ends, `r_s` extends beyond
   *   that range (negative before the start, greater than `total_length()` past the end) to
   *   report the true signed distance past the nearest end instead of silently clamping to it --
   *   callers measuring end-of-strip falloff need this to avoid treating every out-of-range
   *   vertex as sitting exactly at the end. `r_tangent`/`r_lateral`/`r_normal_dist` below are
   *   unaffected and stay anchored to the true (clamped) closest point on the curve.
   * \param r_tangent: tangent at the closest point (see #tangent_at).
   * \param r_lateral: signed lateral offset — `dot(query - closest_pos, side_axis)` where
   *   `side_axis = normalize(cross(plane_normal, r_tangent))`. Positive/negative indicates which
   *   side of the curve `query` is on; magnitude is not necessarily the true 3D distance to the
   *   curve when `query` is far from the plane defined by `plane_normal`, which is expected —
   *   Curve Patch projects along the plane, not along the full 3D closest-point distance.
   * \param r_normal_dist: optional out-param for the signed offset of `query` from the curve's
   *   plane along `plane_normal` — `dot(query - closest_pos, plane_normal)`. Together with
   *   `r_lateral` this completes the 3D decomposition of `query - closest_pos` in the
   *   (side_axis, plane_normal) frame (the tangent component is ~0 by construction of the closest
   *   point). The caller uses it to reject vertices that lie on perpendicular faces of a faceted
   *   surface (e.g. the side faces of a cube when the curve sits on the top face), whose
   *   `r_lateral` alone would wrongly admit into the strip.
   * No-op (leaves outputs unchanged) when `is_empty()`.
   */
  void closest_point(const float3 &query,
                     float &r_s,
                     float3 &r_tangent,
                     float &r_lateral,
                     float *r_normal_dist = nullptr) const;

  /** Raw closest point: arc-length `r_s` (clamped to `[0, total_length()]`), unit `r_tangent`, and
   * the true 3D Euclidean distance `r_dist`. Unlike #closest_point this does no plane decomposition
   * and no past-the-end extension -- used by #RollSpline where only (s, tangent, distance) is
   * needed. No-op (leaves outputs unchanged) when #is_empty(). */
  void closest_point_dist(const float3 &query,
                          float &r_s,
                          float3 &r_tangent,
                          float &r_dist) const;
};

/**
 * World-space length onto which one texture tile (`[-1, 1]`) is mapped along the control curve's
 * arc-length, selected by `length_mode` (see #eMTex_CurvePatchLengthMode):
 * - DEFAULT: `min(total_length, 2 * radius_at_s)` — one tile on short curves, radius-tiled on long.
 * - REPEAT:  `total_length / max(1, repeat)` — a fixed integer number of tiles along the length.
 * - STRETCH: `total_length` — exactly one tile across the whole curve.
 * An unrecognized `length_mode` falls back to DEFAULT. Callers still guard the degenerate
 * (near-zero) return before dividing by it.
 */
float curve_patch_texture_tile_span(int length_mode,
                                    int repeat,
                                    float total_length,
                                    float radius_at_s);

}  // namespace blender::ed::sculpt_paint
