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

#include <cstdint>

#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::ed::sculpt_paint {

/**
 * How far a single stamp can reach from its own center, for a patch of the given brush radius.
 *
 * A stamp is a SQUARE free to rotate by its own `angle`, so its farthest corner sits
 * `half_extent * sqrt(2)` away, and `half_extent` never exceeds the brush radius because the size
 * randomization only ever shrinks. Every consumer that has to cover a stamp's full footprint --
 * the per-vertex search window, the ribbon's end extension and the cyclic seam wrap -- must use
 * THIS one bound: they were three separate copies of the same expression, and a stamp reaching
 * past any one of them is silently clipped, which is exactly the class of defect that produced
 * two rounds of visible clipping already.
 */
inline float curve_patch_stamp_reach(const float radius)
{
  return radius * float(M_SQRT2);
}

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
  /** Surface normal under each `poly_3d` sample, as returned by the shrinkwrap. An empty vector
   * means "no normals" -- consumers then fall back to `plane_normal`. Sharp: it changes abruptly
   * across an edge, which is exactly what selecting a window's projection plane needs. */
  Vector<float3> normals_3d;
  /** `normals_3d` smoothed along the curve. The ribbon's binormals are built from THESE: a
   * discontinuous field would break the across-strip coordinate `u` wherever the curve crosses an
   * edge at an angle. */
  Vector<float3> normals_smooth_3d;
  /** Frozen projection normal for the whole patch (defines "left"/"right" of the curve for
   * `closest_point()`'s signed lateral offset). Caller sets this before calling `closest_point`. */
  float3 plane_normal = float3(0.0f, 0.0f, 1.0f);
  /** True when the polyline closes back on itself: `poly_3d.last() == poly_3d.first()` and the
   * tangents run continuously through that join. Set by #build_from_positions. Consumers that treat
   * the curve's two ends specially (end falloff, the along-length texture coordinate) must skip that
   * handling when set -- a loop has no ends. */
  bool cyclic = false;

  void clear();
  bool is_empty() const;
  float total_length() const;

  /** Rebuilds `poly_3d`/`lengths_3d`/`tangents_3d` from an already-tessellated polyline.
   * `positions.size() < 2` clears the spline (treated as empty). `radii`, when non-empty, must be
   * the same size as `positions` -- pass an empty span when the caller does not need
   * `radius_at()`.
   *
   * With `cyclic`, `positions` is taken to be a closed curve's evaluated points, which do NOT
   * repeat the first point at the end: the closing edge back to `positions[0]` is appended here,
   * and the tangents at the join are made continuous through it.
   *
   * `normals`, when non-empty, must match `positions` in size and populates `normals_3d`; the
   * closing sample of a cyclic curve repeats the first normal, so the two arrays stay index-aligned
   * with `poly_3d`. */
  void build_from_positions(Span<float3> positions,
                            Span<float> radii = {},
                            bool cyclic = false,
                            Span<float3> normals = {});

  /** Position on the spline at arc-length `s`, clamped to `[0, total_length()]`. Returns
   * `float3(0)` when `is_empty()`. */
  float3 evaluate(float s) const;

  /** Tangent at arc-length `s` (linear interpolation between the two bracketing polyline
   * tangents, re-normalized), clamped to `[0, total_length()]`. Returns `float3(1, 0, 0)` when
   * `is_empty()`. */
  float3 tangent_at(float s) const;

  /** Smoothed surface normal at arc-length `s` (linear interpolation between the two bracketing
   * `normals_smooth_3d` entries, re-normalized), clamped to `[0, total_length()]`.
   *
   * Returns `plane_normal` when `normals_smooth_3d` is not populated, which is what makes every
   * consumer degrade to the pre-surface-wrap behavior on Grids and on a failed snapshot.
   *
   * This -- NOT a window's projection normal -- is what any measurement of "how far the vertex has
   * lifted off the surface" must use. A window normal is SHARP by design and jumps where the
   * winning window changes, so a depth measured against it steps discontinuously across the join
   * even though the ribbon's own `(u, s)` runs through it smoothly. */
  float3 normal_at(float s) const;

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
 * Pick a texture slot for one stamp from a cumulative weight table.
 *
 * `weights_cdf` is non-decreasing and its last entry is the total weight. `random01` is a
 * hash-derived value in `[0, 1]`. Returns the index of the first entry strictly greater than
 * `random01 * total`, or -1 when the table is empty or its total is not positive -- which is what
 * makes a list of all-zero weights degrade to the brush's own texture rather than to nothing.
 *
 * A slot with zero weight occupies no width in the table and is therefore never returned, which is
 * what lets the UI disable a slot without deleting it.
 */
int curve_patch_stamp_pick_texture(Span<float> weights_cdf, float random01);

/**
 * World-space length onto which one texture tile (`[-1, 1]`) is mapped along the control curve's
 * arc-length, selected by `length_mode` (see #eMTex_CurvePatchLengthMode):
 * - DEFAULT: `min(total_length, 2 * radius_at_s)` — one tile on short curves, radius-tiled on long.
 * - REPEAT:  `total_length / max(1, repeat)` — a fixed integer number of tiles along the length.
 * - STRETCH: `total_length` — exactly one tile across the whole curve.
 * An unrecognized `length_mode` falls back to DEFAULT. Callers still guard the degenerate
 * (near-zero) return before dividing by it.
 *
 * With `cyclic`, the resulting span is snapped so the loop holds a whole number of tiles, which is
 * what makes the pattern meet itself at the join without a seam. Repeat and Stretch already satisfy
 * that and are unaffected; DEFAULT's radius-driven tile is resized to the nearest whole count.
 */
float curve_patch_texture_tile_span(int length_mode,
                                    int repeat,
                                    float total_length,
                                    float radius_at_s,
                                    bool cyclic = false);

/** Which of the three Ribbon CAPS stretches an arc-length position falls into. */
enum class CurvePatchTextureZone : int8_t {
  Start = 0,
  Middle = 1,
  End = 2,
};

struct CurvePatchTextureZoneSample {
  CurvePatchTextureZone zone = CurvePatchTextureZone::Middle;
  /** Texture coordinate along the curve, in the same `[-1, 1]`-per-tile domain the relief feeds into
   * the mapping's size/offset. */
  float v = 0.0f;
  /** False for a degenerate zone (a middle squeezed to zero length by two oversized caps); the
   * relief leaves such a position untouched instead of dividing by zero. */
  bool valid = true;
};

/**
 * Resolve the texture zone and along-curve texture coordinate at arc length `s`.
 *
 * With `caps_enabled == false` this returns `{Middle, <the pre-caps formula>, true}` -- including the
 * cyclic `-1` tile centering and the REPEAT sawtooth wrap -- so the original Ribbon output is
 * reproduced exactly and the caps feature cannot regress it.
 *
 * `cap_start_length` and `cap_end_length` are WORLD-space lengths; the UI stores them in brush
 * diameters and the caller resolves them. Both are clamped to `>= 0` and, when their sum exceeds
 * `total_length`, scaled by the same factor so their ratio is preserved and the middle collapses
 * rather than either cap overrunning the curve. Each cap carries exactly one tile (`v` spans
 * `[-1, 1]` across it) and never repeats.
 *
 * `radius_for_middle_tile` is used ONLY by the middle zone, and only because
 * #curve_patch_texture_tile_span's DEFAULT mode is radius-driven. The caps deliberately ignore it:
 * their extent is already fixed in world units by the caller.
 *
 * The middle applies `length_mode`/`length_repeat` over the REMAINING span, always as if the curve
 * were open -- caps sit at the seam of a cyclic curve, so the requirement that the pattern meet
 * itself across the join no longer applies. The cost is a visible seam for cyclic + REPEAT + CAPS,
 * which is an accepted trade-off.
 */
CurvePatchTextureZoneSample curve_patch_texture_zone_at(float s,
                                                        float total_length,
                                                        float radius_for_middle_tile,
                                                        bool caps_enabled,
                                                        float cap_start_length,
                                                        float cap_end_length,
                                                        int length_mode,
                                                        int length_repeat,
                                                        bool cyclic);

/** One texture stamp placed along the control curve in the ribbon's UV space, where `v` is arc
 * length along the curve and `u` is the signed lateral offset from it (both in world units). Used
 * by the Curve Patch Stamps mode (#MTEX_CURVE_PATCH_STAMP_STAMPS) in place of the single stretched
 * texture sheet Ribbon mode projects.
 *
 * `center_v`/`center_u` locate the stamp for BOTH projections; the world frame below is what the
 * PLANAR projection samples in instead of that UV space. */
struct CurvePatchStamp {
  /** Arc length of the stamp's center, after jitter. The stamp list is sorted by this. */
  float center_v = 0.0f;
  /** Lateral offset of the stamp's center from the curve, after jitter. */
  float center_u = 0.0f;
  /** Half the stamp's side length in UV space: the brush radius after the per-stamp size
   * randomization. Stamps are square, so one value covers both axes. */
  float half_extent = 0.0f;
  /** Texture rotation inside the stamp, in radians (base rotation plus the random offset). */
  float angle = 0.0f;
  /** Per-stamp relief strength multiplier in `(0, 1]`. */
  float strength = 1.0f;
  /** Index into the cache's `stamp_texture_variants`, or -1 when this stamp samples the brush's own
   * texture (SINGLE mode, an empty list, or a list whose weights all sum to zero). Ghost copies made
   * by #curve_patch_stamps_add_cyclic_wrap inherit it by whole-struct copy, so a seam stamp shows
   * the same texture on both sides of the join. */
  int tex_index = -1;
  /** World-space frame of the stamp, frozen at layout time and used only by the PLANAR projection
   * (#MTEX_CURVE_PATCH_STAMP_PROJ_PLANAR), which reads a vertex's stamp-local coordinates as
   * `dot(co - origin, axis_*)` instead of going through the ribbon's curvilinear `(s, u)`. That is
   * what keeps the texture's shape through a sharp turn: the frame is rigid, so the stamp's square
   * stays square no matter how the curve bends underneath it.
   *
   * Filled for EVERY stamp regardless of the active projection, so neither the layout nor
   * #curve_patch_stamps_add_cyclic_wrap has to branch on the mode. Derivable on the fly from
   * `center_v`/`center_u`/`angle`; cached for simplicity, not for speed. */
  float3 origin = float3(0.0f);
  /** Unit axis along the curve at `center_v`, already rotated by `angle`. */
  float3 axis_v = float3(1.0f, 0.0f, 0.0f);
  /** Unit axis across the curve, orthogonal to `axis_v` and lying in the patch's anchor plane. */
  float3 axis_u = float3(0.0f, 1.0f, 0.0f);
};

/**
 * Lay stamps out along `spline`, one every `spacing_frac * 2 * radius` of arc length, and derive
 * each stamp's jittered position, size, rotation and strength.
 *
 * Every random quantity is a pure function of `(stamp index, seed, channel)` -- deliberately NOT a
 * stateful RNG. The Curve Patch relief is recomputed from scratch on every interactive event and
 * evaluated in parallel across Paint BVH nodes, so a stateful generator would both flicker between
 * re-stamps and race across threads.
 *
 * `spacing_frac` is the brush's Spacing as a fraction (Blender's percent / 100), floored so a
 * degenerate value cannot produce an unbounded stamp count. `jitter_amount`, `size_random` and
 * `strength_random` are all fractions; size and strength randomization only ever REDUCE, which is
 * what lets the caller widen the ribbon for jitter alone.
 *
 * With `spline.cyclic`, the step is snapped so a whole number of stamps fits the loop and the final
 * stamp does not land on top of the first at the seam.
 *
 * `texture_weights_cdf` is the cumulative weight table of the brush's texture list, or empty for the
 * single-texture case. Each stamp draws its `tex_index` from it through
 * #curve_patch_stamp_pick_texture on its own hash channel, so the choice is as stable across
 * re-stamps as every other per-stamp random quantity here.
 *
 * `r_stamps` is cleared first and comes back sorted by `center_v`.
 */
void curve_patch_stamps_build(const CurvePatchSpline &spline,
                              float radius,
                              float spacing_frac,
                              float jitter_amount,
                              float size_random,
                              float strength_random,
                              float base_angle,
                              float random_angle,
                              uint32_t seed,
                              Span<float> texture_weights_cdf,
                              Vector<CurvePatchStamp> &r_stamps);

/**
 * Append wrap-around ghost copies of the stamps that straddle a closed curve's join, so the stamp
 * sitting at the seam is drawn whole.
 *
 * An open curve solves this by extending the ribbon past its ends, but a loop has no ends to
 * extend: its ribbon spans `v` in `[0, total_length]` exactly, and the half of a seam stamp that
 * reaches into `v < 0` is not outside the curve at all -- it is the stretch just before the join,
 * at `v` near `total_length`. The LUT has no negative `v`, so that half would simply be dropped.
 *
 * Every stamp whose center lies within `max_extent` of either end therefore gains a copy displaced
 * by `+total_length` (near the start) or `-total_length` (near the end), carrying the original's
 * `half_extent`, `angle`, `strength` and `center_u` unchanged -- the two are the same stamp seen
 * from the two sides of the join, which is what makes the texture meet itself exactly.
 *
 * `max_extent` must be the same conservative bound the relief's per-vertex search window uses on
 * `v`, or a ghost could sit outside every window and never be found. The ghosts are a coverage
 * device, not stamps, which is why this is separate from #curve_patch_stamps_build, whose contract
 * stays "one entry per real stamp, a whole number of them around the loop".
 *
 * `stamps` comes back re-sorted by `center_v`, since the relief binary-searches it. Call only for a
 * cyclic spline; a no-op for a degenerate `total_length` or `max_extent`.
 */
void curve_patch_stamps_add_cyclic_wrap(Vector<CurvePatchStamp> &stamps,
                                        float total_length,
                                        float max_extent);

/**
 * Smooths `spline.normals_3d` with a box filter over arc length in a `+/- smooth_length / 2` window
 * and writes the normalized result into `spline.normals_smooth_3d`.
 *
 * A box rather than N Laplacian iterations: the smoothing width is then stated in world units and
 * depends neither on the tessellation density nor on an iteration count, so the result is
 * reproducible and can be pinned down by a test.
 *
 * `smooth_length <= 0` or an empty `normals_3d` copies the input through unchanged.
 */
void curve_patch_spline_smooth_normals(CurvePatchSpline &spline, float smooth_length);

}  // namespace blender::ed::sculpt_paint
