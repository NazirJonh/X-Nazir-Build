/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Curve Patch core geometry: arc-length spline, ribbon LUT, window frames, stamp layout
 * and surface snapshot. Pure geometry -- no operator, brush, PBVH or context dependency.
 */

#include <cstdint>

#include "BLI_array.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_bvhutils.hh"
#include "BKE_curves.hh"

namespace blender {
/* The two geometry data-blocks a built patch can be read back out as; see
 * #blender::bke::curve_patch_geometry_to_mesh and
 * #blender::bke::curve_patch_geometry_to_stamp_points. Both live in `namespace blender`, so
 * declaring #Mesh at global scope would name an unrelated type. */
struct Mesh;
struct PointCloud;
}  // namespace blender

namespace blender::bke {

/** Per-point float3 attribute on the control curve, set by the editor at each point's placement
 * (`paintcurve_geom_set_surface_normal()`), normalized. Shared here so the core build can read it
 * without depending on the editor module -- see #curve_patch_build_from_control_curve. */
constexpr const char *CURVE_PATCH_ATTR_SURFACE_NORMAL = "paintcurve_surface_normal";

/* -------------------------------------------------------------------- */
/** \name Build Parameters
 * \{ */

/* Curve Patch parameter enums. Deliberately independent of the matching brush-texture enums in
 * DNA: the core must stay usable from a caller that has no brush texture at all (the Python API
 * of Stage 5-7). The editor layer converts, in curve_patch_params_from_brush(). */
enum class CurvePatchLengthMode : int8_t {
  Default = 0,
  Repeat = 1,
  Stretch = 2,
};

enum class CurvePatchEndFalloff : int8_t {
  None = 0,
  Smooth = 1,
};

enum class CurvePatchStampMode : int8_t {
  Ribbon = 0,
  Stamps = 1,
};

enum class CurvePatchStampProjection : int8_t {
  Curve = 0,
  Planar = 1,
};

/**
 * Everything a Curve Patch build needs to know that is not the curve itself.
 *
 * Three producers: the brush (curve_patch_params_from_brush(), editor layer), a Python caller
 * (Stage 5+), and the session's own frozen copy. Having one type for all three is what lets the
 * core be driven without a brush at all.
 */
struct CurvePatchParams {
  /** World-space brush radius, frozen when the patch starts. */
  float radius = 0.0f;
  /** World-space radius per unit of the brush Size slider. Converts a live Size change and
   * pixel-space jitter into world units. */
  float radius_per_size = 0.0f;
  bool swap_axis = false;

  CurvePatchLengthMode length_mode = CurvePatchLengthMode::Default;
  int length_repeat = 1;

  CurvePatchEndFalloff end_falloff_mode = CurvePatchEndFalloff::None;
  /** Fade length at each end, as a percentage of the curve's total arc length. */
  int end_falloff_percent = 0;

  CurvePatchStampMode stamp_mode = CurvePatchStampMode::Ribbon;
  /** Per-stamp randomization, as fractions in [0, 1] (the DNA fields are percentages). */
  float stamp_size_random = 0.0f;
  float stamp_strength_random = 0.0f;
  CurvePatchStampProjection stamp_projection = CurvePatchStampProjection::Curve;
  /** Rolled once per patch; re-rolled only by the explicit Reseed action. */
  uint32_t stamp_seed = 0;

  /** Stamp spacing as a fraction of the brush DIAMETER (the brush UI stores a percentage). */
  float spacing_frac = 0.0f;
  /** World-space per-stamp positional jitter. The editor layer resolves the brush's relative or
   * absolute (pixel) jitter into world units before it gets here, since only that layer knows the
   * pixel-to-world ratio. */
  float jitter_amount = 0.0f;
  /** Base texture rotation inside a stamp, in radians. */
  float base_angle = 0.0f;
  /** Per-stamp random rotation amount added on top of #base_angle, in radians. */
  float random_angle = 0.0f;

  /** Projection plane, frozen at patch start from the anchor dab's sculpt normal. */
  float3 plane_normal = float3(0.0f, 0.0f, 1.0f);

  /** Commit-time build: supersampled texture reads and a higher-resolution ribbon. An input to
   * the build, not session state -- which is why it lives here and not on the session. */
  bool final_quality = false;

  friend bool operator==(const CurvePatchParams &a, const CurvePatchParams &b) = default;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Arc-Length Spline
 *
 * Arc-length-parameterized polyline used to project a texture/displacement onto a mesh surface
 * along the static, user-edited Curve Patch control curve. Trimmed port of the `RollSpline`
 * concept (see `155072.diff`, external reference, not present in this repository): no live
 * mouse-path recording, virtual backward extension, or LUT rasterization, since this curve is
 * small and fully rebuilt from scratch on every edit rather than growing continuously through a
 * live stroke.
 * \{ */

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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Mapping Along the Curve
 * \{ */

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
 * arc-length, selected by `length_mode` (see #CurvePatchLengthMode):
 * - Default: `min(total_length, 2 * radius_at_s)` — one tile on short curves, radius-tiled on long.
 * - Repeat:  `total_length / max(1, repeat)` — a fixed integer number of tiles along the length.
 * - Stretch: `total_length` — exactly one tile across the whole curve.
 * An unrecognized `length_mode` falls back to Default. Callers still guard the degenerate
 * (near-zero) return before dividing by it.
 *
 * With `cyclic`, the resulting span is snapped so the loop holds a whole number of tiles, which is
 * what makes the pattern meet itself at the join without a seam. Repeat and Stretch already satisfy
 * that and are unaffected; DEFAULT's radius-driven tile is resized to the nearest whole count.
 */
float curve_patch_texture_tile_span(CurvePatchLengthMode length_mode,
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
                                                        CurvePatchLengthMode length_mode,
                                                        int length_repeat,
                                                        bool cyclic);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stamp Layout
 * \{ */

/** One texture stamp placed along the control curve in the ribbon's UV space, where `v` is arc
 * length along the curve and `u` is the signed lateral offset from it (both in world units). Used
 * by the Curve Patch Stamps mode (#CurvePatchStampMode::Stamps) in place of the single stretched
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
   * (#CurvePatchStampProjection::Planar), which reads a vertex's stamp-local coordinates as
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ribbon UV Lookup Table
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
 * \{ */

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

/**
 * The polygonal strip #curve_patch_ribbon_build rasterizes its LUT from: a `rows * cols` grid of
 * world positions, each carrying `(u, v)` where `u` runs `[-1, 1]` across the strip and `v` is RAW
 * arc length along the curve -- negative inside a start extension, past the total length inside an
 * end one.
 *
 * Row-major: vertex `(r, c)` is at index `r * cols + c`. Rows run along the curve, columns across
 * it, so column 0 is the `-u` border and column `cols - 1` the `+u` one.
 */
struct CurvePatchRibbonGrid {
  Vector<float3> positions;
  Vector<float2> uv;
  int rows = 0;
  int cols = 0;
  /** In-plane projection axes, both perpendicular to the spline's `plane_normal`. */
  float3 axis_x = float3(1.0f, 0.0f, 0.0f);
  float3 axis_y = float3(0.0f, 1.0f, 0.0f);
  /** Largest world half-width over the strip. Drives the LUT resolution. */
  float max_halfwidth = 0.0f;

  void clear();
};

/**
 * Builds the strip: extends the ends, offsets the two borders by the per-sample half-width along
 * the binormals, collapses border self-intersections, then subdivides twice and smooths.
 *
 * Split out of #curve_patch_ribbon_build so that the two consumers -- the rasterizer and the mesh
 * the Python API hands out -- describe the same strip by construction. Building a second one
 * elsewhere would make the mesh a script reads diverge from what the patch actually stamps along.
 *
 * Parameters carry the meaning documented on #curve_patch_ribbon_build. Returns false, leaving
 * `r_grid` cleared, when the spline is empty or the strip would be degenerate.
 */
bool curve_patch_ribbon_grid_build(const CurvePatchSpline &spline,
                                   float brush_radius,
                                   Span<float3> binormals,
                                   float end_margin_start,
                                   float end_margin_end,
                                   CurvePatchRibbonGrid &r_grid);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Tangent-Plane Windows
 *
 * A single frozen projection plane cannot cover a curve that runs over a sharp edge: past the edge
 * the surface turns away from the plane, the relief stretches and then breaks off entirely. The
 * curve is therefore cut into overlapping windows, each with its own projection plane taken from
 * the dominant face of that stretch.
 *
 * The role of the normal splits in two. The SMOOTHED field
 * (#CurvePatchSpline::normals_smooth_3d) supplies the binormals of ONE ribbon spanning the whole
 * curve, which is what keeps the across-strip coordinate `u` continuous and independent of where
 * the window boundaries happen to fall. The SHARP field (#CurvePatchSpline::normals_3d) picks the
 * window planes that ribbon is projected into for rasterization.
 * \{ */

/** Ceiling on the number of windows. On reaching it the tail of the curve folds into the last
 * window and #CurvePatchFrameSet::capped reports it -- silently degrading quality is not
 * acceptable. */
constexpr int CURVE_PATCH_MAX_FRAMES = 32;
/** Total LUT pixel budget across all windows (~32 MB at 32 bytes per pixel). Capping the window
 * count alone is not enough: `res` is chosen adaptively from the extent, and a short slice does not
 * shrink it proportionally. */
constexpr int64_t CURVE_PATCH_MAX_LUT_PIXELS = 1024 * 1024;

/** One window along the curve: a range of `poly_3d` indices and the SHARP normal of its dominant
 * face.
 *
 * Sharp rather than averaged over the samples: across an edge the average lands at 45 degrees --
 * the worst projection plane for both faces at once. Only the ribbon's binormals need smoothness,
 * and a separate smoothed field (#CurvePatchSpline::normals_smooth_3d) provides it. */
struct CurvePatchFrameRange {
  /** First `poly_3d` index in the window, inclusive. */
  int begin = 0;
  /** Last `poly_3d` index in the window, inclusive. */
  int end = 0;
  float3 normal = float3(0.0f, 0.0f, 1.0f);
};

struct CurvePatchFramesParams {
  /** Usually `2 * CurvePatchParams::radius`. */
  float min_window_length = 0.0f;
  /** Threshold for a gradual turn; only splits once `min_window_length` is covered. */
  float turn_threshold_rad = 0.0f;
  /** A break: tears the window open REGARDLESS of `min_window_length`. */
  float break_threshold_rad = 0.0f;
  /** Arc length by which every window is grown past each of its INTERIOR boundaries, so two
   * neighbours share a stretch to cross-fade over.
   *
   * Windows used to meet edge to edge across a break, on the argument that each face's vertices are
   * rejected by orientation in the other's window anyway. That turned out to be what produced the
   * visible seam: with no shared stretch there is nothing to blend, so the handover from one window
   * to the next is a step by construction -- and it lands exactly where BOTH LUTs sit on their
   * outermost row, the worst place either can be sampled. 0 reproduces the old edge-to-edge cut. */
  float overlap_length = 0.0f;
  int max_frames = CURVE_PATCH_MAX_FRAMES;
};

/**
 * Cuts the polyline into overlapping windows. Returns false when there are no normals or the input
 * is degenerate -- the caller then builds a single window over the whole curve. `r_capped` reports
 * that `max_frames` was reached and the tail was folded into the last window.
 */
bool curve_patch_frames_partition(const CurvePatchSpline &spline,
                                  const CurvePatchFramesParams &params,
                                  Vector<CurvePatchFrameRange> &r_ranges,
                                  bool &r_capped);

/** One window's rasterized ribbon, plus what is needed to place its `(u, s)` back into the curve's
 * global coordinates. */
struct CurvePatchFrame {
  CurvePatchRibbonLut lut;
  /** Projection plane of this window (see #CurvePatchFrameRange::normal). */
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  /** The local LUT numbers arc length from zero at its own slice; adding this offset puts `s` back
   * into the curve's global arc length, which is the same for every window. That is precisely why
   * a change of winning window does not shift the texture along the length. Also the window's
   * global start. */
  float s_offset = 0.0f;
  /** Global arc length at the window's end. */
  float s_end = 0.0f;
  /** Global arc length at the window's midpoint. */
  float s_center = 0.0f;
  /** Arc length over which this window's blend weight ramps in at its start / out at its end. Zero
   * at a REAL end of the curve: fading there would thin the relief out at the curve's own tips
   * instead of handing it over to a neighbour that does not exist. */
  float fade_start = 0.0f;
  float fade_end = 0.0f;
  float3 bb_min = float3(0.0f);
  float3 bb_max = float3(0.0f);
};

struct CurvePatchFrameSet {
  Vector<CurvePatchFrame> frames;
  bool ready = false;
  /** The window count or the LUT pixel budget was hit and wrap quality was reduced. */
  bool capped = false;

  /**
   * Up to two global `(u, s)` branches for `co`.
   *
   * Windows covering the same stretch are BLENDED, not picked between: their `(u, s)` are averaged
   * with weights that fade both with distance from the window's own span and with how far the
   * vertex normal has turned from the window's plane. Picking one winner put a step wherever the
   * winner changed -- a hard seam right across the strip at every window join -- because neither
   * the parameterization nor the rasterization of two windows agrees exactly.
   *
   * Two branches survive only when they are genuinely DIFFERENT stretches of a self-crossing curve
   * (their `s` differ by more than the LUT's `v_threshold`); those must not be merged, since the
   * caller resolves them by max `|height|`.
   *
   * The depth culling is deliberately NOT done here: it needs `falloff_radius_at_s` and is applied
   * by the caller per branch, after the relief has been evaluated -- moving it in here would change
   * how many branches survive to that merge.
   *
   * `r_frame_normal` reports the plane of the heaviest-weighted window behind each branch. It is an
   * observable for tests and diagnostics only -- feeding it into the relief's own depth measurement
   * is what produced the first version of the seam (see `CurvePatchSpline::normal_at`).
   */
  int sample(const float3 &co,
             const float3 &vertex_normal,
             float2 r_uv[2],
             float3 r_frame_normal[2]) const;
  void clear();
};

/**
 * Partitions `spline` into windows and rasterizes one ribbon LUT per window.
 *
 * The ribbon is conceptually ONE strip over the whole curve: each window's binormals are taken from
 * the GLOBAL smoothed normal field and `u` is normalized by the half-width, so the pieces coincide
 * with the matching stretches of a single continuous strip.
 *
 * `end_margin` is applied only where a window borders a REAL end of the curve; interior joins are
 * never extended, since that would push the strip outside the window serving it.
 *
 * `r_frames.frames` is reused between re-stamps rather than rebuilt, so each LUT keeps its own
 * `source_hash` cache.
 */
void curve_patch_frames_build(const CurvePatchSpline &spline,
                              float brush_radius,
                              const CurvePatchFramesParams &params,
                              bool high_quality,
                              float end_margin,
                              CurvePatchFrameSet &r_frames);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface Snapshot
 *
 * Snapshot of the surface the Curve Patch ribbon is wrapped onto.
 *
 * A snapshot rather than the live mesh: the patch displaces vertices itself, so
 * `Mesh::bvh_corner_tris()` would be invalidated on every re-stamp and the tree rebuilt from
 * scratch. Semantically it is also the only correct choice -- the ribbon has to wrap the ORIGINAL
 * geometry, otherwise it wraps the relief it just applied and a feedback loop appears.
 * \{ */

struct CurvePatchSurfaceSnapshot {
  Array<float3> positions;
  /** Vertex normals as of the snapshot. The orientation culling has to compare against THESE: the
   * live normals already carry the relief the patch applied, so the culling would end up depending
   * on its own result. */
  Array<float3> vert_normals;
  /**
   * Topology the BVH callbacks index into. Copied rather than borrowed from the live mesh: the
   * tree stores `Span`s (`BVHTreeFromMesh`), and a remesh / CustomData rebuild can free the live
   * arrays while this snapshot is still queried. Counts may stay the same, so `verts_num` alone
   * does not detect that.
   */
  Array<int> face_offsets;
  Array<int> corner_verts;
  Array<int3> corner_tris;
  BVHTreeFromMesh bvh;
  bool ready = false;

  void clear();
};

/**
 * Builds a snapshot from the mesh's current (pristine) positions. Returns false for an empty mesh
 * or a failed BVH build -- the caller then stays on the single-window path.
 */
bool curve_patch_surface_snapshot_build(const Mesh &mesh, CurvePatchSurfaceSnapshot &r_snapshot);

/**
 * Pulls each position onto the nearest point of the snapshot and reports the normal of the triangle
 * it hit. A sample farther away than `max_dist` is left where it is and its normal stays zero -- the
 * "no normal here" marker #curve_patch_surface_fill_invalid_normals looks for.
 */
void curve_patch_surface_shrinkwrap(const CurvePatchSurfaceSnapshot &snapshot,
                                    float max_dist,
                                    MutableSpan<float3> positions,
                                    MutableSpan<float3> r_normals);

/**
 * Fills the zero (invalid) normals by interpolating between the nearest valid neighbours; writes
 * `fallback` everywhere when there is no valid normal at all.
 */
void curve_patch_surface_fill_invalid_normals(MutableSpan<float3> normals, const float3 &fallback);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Derived Geometry
 * \{ */

/**
 * Everything a Curve Patch build produces. Rebuilt wholesale from a control curve plus
 * #CurvePatchParams; holds no session, brush, PBVH or context state, so it can be built and read
 * without a sculpt session at all.
 */
struct CurvePatchGeometry {
  CurvePatchSpline spline;

  /** Whole-curve ribbon UV lookup table. Used when `frames` is empty. */
  CurvePatchRibbonLut ribbon;
  /** Local tangent windows replacing the single `ribbon` when the surface snapshot is ready.
   * Empty on the single-window path. */
  CurvePatchFrameSet frames;
  /** Snapshot of the pristine surface. `ready == false` selects the single-window path. */
  CurvePatchSurfaceSnapshot surface;

  /** Stamps-mode layout. Empty in Ribbon mode. */
  Vector<CurvePatchStamp> stamps;

  /** World-space radius the ribbon was actually built from. Equals `CurvePatchParams::radius`
   * in Ribbon mode; WIDENED by the layout's jitter in Stamps mode, so anything reconstructing a
   * world-space lateral offset from the ribbon's normalized `u` must scale by THIS value. */
  float ribbon_radius = 0.0f;

  /** Conservative arc-length half-window covering one stamp's footprint, resolved once per build
   * and shared by the per-vertex search window, the cyclic seam wrap and the ribbon's end
   * extension. Their agreement is a correctness requirement, not a coincidence. 0 in Ribbon mode. */
  float stamp_search_reach = 0.0f;

  /** World-space distance the ribbon was extended past each end of a non-cyclic curve, so stamps
   * centered on an end point render whole. Everything bounding reach ALONG the curve must add it.
   * 0 in Ribbon mode. */
  float ribbon_end_margin = 0.0f;

  void clear();
};

/**
 * Build every derived structure from a control curve and its parameters.
 *
 * `evaluated_positions`/`evaluated_radii`/`evaluated_normals` are the tessellated, already
 * surface-projected control curve. `r_geometry.surface` must be built (or left not-ready) by the
 * caller beforehand: the snapshot outlives individual builds.
 *
 * `stamp_texture_weights_cdf` is the cumulative weight table each stamp draws its texture slot
 * from, or empty for the single-texture case. It is an argument rather than a #CurvePatchParams
 * field because the table is resolved from the brush's texture list, which lives in DNA and
 * therefore in the editor layer -- the core only ever sees the already-resolved weights.
 *
 * Returns with an empty `r_geometry.spline` when the input polyline is degenerate; callers must
 * check that, and the readiness of `frames`/`ribbon`, before consuming the result.
 */
/**
 * Tessellate a single-spline control curve and build every derived structure from it.
 *
 * Wraps #curve_patch_geometry_build with the tessellation that every caller performed identically:
 * evaluating the curve, interpolating the per-point `radius` to the evaluated resolution, reading
 * `cyclic` off curve 0, and -- when the snapshot is ready -- pulling the polyline onto it.
 *
 * `r_geometry.surface` is an INPUT. When it is ready the polyline is projected onto it before the
 * spline is built; otherwise the strip stays in the curve's own plane. WHICH mesh the snapshot was
 * taken from is deliberately the caller's decision: a sculpt session snapshots the original mesh,
 * while a script reading a patch back may ask for the evaluated one.
 *
 * `control_curve` must hold a single spline. Slicing one spline out of a multi-spline paint curve
 * is the editor's job, because only it knows about `PaintCurve`.
 */
void curve_patch_build_from_control_curve(const CurvesGeometry &control_curve,
                                          const CurvePatchParams &params,
                                          Span<float> stamp_texture_weights_cdf,
                                          CurvePatchGeometry &r_geometry);

void curve_patch_geometry_build(Span<float3> evaluated_positions,
                                Span<float> evaluated_radii,
                                Span<float3> evaluated_normals,
                                bool cyclic,
                                const CurvePatchParams &params,
                                Span<float> stamp_texture_weights_cdf,
                                CurvePatchGeometry &r_geometry);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reading a Build Back Out as Geometry
 * \{ */

/**
 * The ribbon as a mesh: one quad per cell of the strip #curve_patch_ribbon_grid_build produces,
 * with a `"UVMap"` corner layer carrying exactly the coordinates the relief samples its texture at.
 *
 * The texture slot's own size/offset are NOT applied -- those are mapping settings, and the mesh
 * carries the patch's coordinates for the caller to map as they please.
 *
 * `caps_enabled`, `world_cap_start` and `world_cap_end` are the RIBBON CAPS inputs, already
 * resolved to world units by the caller (the UI stores them in brush diameters).
 *
 * Returns null for a degenerate curve or strip. The caller owns the result.
 */
Mesh *curve_patch_geometry_to_mesh(const CurvePatchGeometry &geometry,
                                   const CurvePatchParams &params,
                                   bool caps_enabled,
                                   float world_cap_start,
                                   float world_cap_end);

/**
 * The stamp layout as a point cloud: one point per stamp at its world origin, carrying `radius`
 * (the stamp's half extent), `rotation` (radians), `strength`, `texture_index` (-1 when the stamp
 * samples the brush's own texture) and the rigid frame `axis_v` / `axis_u`.
 *
 * Returns null in Ribbon mode, which lays out no stamps. The caller owns the result.
 */
PointCloud *curve_patch_geometry_to_stamp_points(const CurvePatchGeometry &geometry);

/** \} */

}  // namespace blender::bke
