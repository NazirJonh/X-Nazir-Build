/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Assembly of every derived Curve Patch structure from a control curve and its parameters: the
 * arc-length spline, the stamp layout, and either the whole-curve ribbon LUT or the set of local
 * tangent windows that replaces it.
 *
 * The order of the calls below is load-bearing and is preserved verbatim from the editor-side
 * re-stamp this was lifted out of -- see the notes on `stamp_search_reach`.
 */

#include "BLI_math_base.h"

#include "BKE_curve_patch.hh"

/* TEMPORARY diagnostic for the seam at a surface fold. Forces the wrap to a SINGLE window while
 * leaving the shrinkwrap, the smoothed normal field and the smoothed-binormal ribbon fully active,
 * so the window join is the only variable removed. Seam gone -> the join produces it; seam still
 * there -> it comes from the shrinkwrap or the ribbon, and the join is innocent. Set back to 0
 * once measured; grep `FORCE_SINGLE_FRAME` to remove every touch point. */
#define CURVE_PATCH_FORCE_SINGLE_FRAME 0

namespace blender::bke {

void CurvePatchGeometry::clear()
{
  this->spline.clear();
  this->ribbon.clear();
  this->frames.clear();
  this->surface.clear();
  this->stamps.clear();
  this->ribbon_radius = 0.0f;
  this->stamp_search_reach = 0.0f;
  this->ribbon_end_margin = 0.0f;
}

/* Width of the normal-field smoothing. It arbitrates "`u` stays continuous" against "the strip hugs
 * the edge": wider means a smoother texture on an oblique crossing and a looser fit. A fraction of
 * the radius rather than a world-space constant, so the behavior does not depend on scene scale. */
static float curve_patch_smooth_length(const CurvePatchParams &params)
{
  return params.radius * (params.final_quality ? 0.5f : 0.8f);
}

/* Lay the stamps out and resolve the three bounds every consumer of them shares. Ribbon mode
 * leaves the list empty and all three bounds at zero, which is what makes it bit-for-bit what it
 * was before Stamps mode existed. */
static void curve_patch_build_stamps(const CurvePatchParams &params,
                                     const Span<float> stamp_texture_weights_cdf,
                                     CurvePatchGeometry &r_geometry)
{
  curve_patch_stamps_build(r_geometry.spline,
                           params.radius,
                           params.spacing_frac,
                           params.jitter_amount,
                           params.stamp_size_random,
                           params.stamp_strength_random,
                           params.base_angle,
                           params.random_angle,
                           params.stamp_seed,
                           stamp_texture_weights_cdf,
                           r_geometry.stamps);

  /* PLANAR tests candidate vertices against a rigid WORLD-space frame, but this reach is still an
   * ARC-LENGTH window. On a bend the chord is shorter than the arc, so a vertex inside a stamp's
   * world square can have an `s` outside this window and get silently clipped. The arc/chord ratio
   * for a circular bend of turn angle theta across the stamp's reach is `(theta/2) / sin(theta/2)`;
   * this bound is sized for turns up to a 180-degree hairpin, where the ratio reaches
   * `PI / 2 ~= 1.571`, and 1.6 rounds that up. A curve that spirals tighter than a half-turn within
   * roughly one stamp's reach exceeds what this bound was designed for and is out of scope here.
   * The bound only has to be conservative within that scope: the per-stamp test in the relief's
   * candidate loop is exact, so an over-wide window just costs a few extra candidates, while a
   * too-narrow one silently clips stamps. */
  constexpr float PLANAR_BEND_SLACK = 1.6f;
  /* Resolve the one bound every consumer below shares. On top of the bend slack above, PLANAR also
   * adds `jitter_amount`: a stamp pushed sideways off the curve keeps a rigid frame, so its square
   * spans more arc length than its own corner reach accounts for. CURVE takes neither term and
   * keeps the historical value, so that projection is unaffected. */
  r_geometry.stamp_search_reach = curve_patch_stamp_reach(params.radius);
  if (params.stamp_projection == CurvePatchStampProjection::Planar) {
    r_geometry.stamp_search_reach = r_geometry.stamp_search_reach * PLANAR_BEND_SLACK +
                                    params.jitter_amount;
  }

  /* A closed curve has no ends to extend (see `ribbon_end_margin` below), so the stamp at the join
   * would lose the half that reaches into `v < 0` -- which on a loop is not outside the curve but
   * the stretch just before the join. Wrap those stamps around instead, so both halves are present
   * and meet exactly at the seam. The bound must be the same one the per-vertex search window uses,
   * hence the shared `stamp_search_reach`. */
  if (r_geometry.spline.cyclic) {
    curve_patch_stamps_add_cyclic_wrap(
        r_geometry.stamps, r_geometry.spline.total_length(), r_geometry.stamp_search_reach);
  }

  /* Stamps pushed sideways by jitter would fall outside the ribbon and be clipped by the LUT's
   * edge, so the strip has to cover the widest possible excursion. Only jitter needs this: the size
   * randomization shrinks stamps and never grows them. The widened value flows into
   * `ribbon_source_hash()` as the `brush_radius` argument, so the cached LUT invalidates correctly
   * with no extra hashing. */
  r_geometry.ribbon_radius += params.jitter_amount;

  /* The layout puts the first stamp's center exactly at `s == 0` and the last one at the last whole
   * step before `total_length`, so an end stamp reaches past the curve's end by its own
   * half-extent -- and the strip, which used to stop dead at that end, gave the overhanging half no
   * UV at all and clipped it along a hard straight edge. Extend the strip by the farthest such
   * reach instead of insetting the stamps, which would leave the ends of the curve visibly bare.
   *
   * #curve_patch_stamp_reach is a stamp's corner reach; `jitter_amount` covers a center jittered
   * further along the curve. Deliberately NOT `stamp_search_reach`: that bound also carries
   * `PLANAR_BEND_SLACK`, which exists to cover a stamp's arc/chord gap on a BEND -- but an end
   * stamp's overhang is not tested against a bend, it is rendered by the ribbon's own straight
   * extrapolation along the end tangent (see #curve_patch_stamps_build's PLANAR frame
   * extrapolation). Slack bought there would only inflate the strip, the PBVH cull tube, and the
   * whole-curve search sphere for no correctness gain, and it would force a full LUT rebuild on
   * every CURVE<->PLANAR toggle since `end_margin` feeds the ribbon's source hash. */
  r_geometry.ribbon_end_margin = curve_patch_stamp_reach(params.radius) + params.jitter_amount;
}

void curve_patch_geometry_build(const Span<float3> evaluated_positions,
                                const Span<float> evaluated_radii,
                                const Span<float3> evaluated_normals,
                                const bool cyclic,
                                const CurvePatchParams &params,
                                const Span<float> stamp_texture_weights_cdf,
                                CurvePatchGeometry &r_geometry)
{
  r_geometry.spline.plane_normal = params.plane_normal;
  r_geometry.spline.build_from_positions(
      evaluated_positions, evaluated_radii, cyclic, evaluated_normals);

  if (r_geometry.spline.is_empty()) {
    return;
  }

  /* Stamps mode lays its stamps out here, right after the spline: the relief's parallel per-vertex
   * walk only reads the result. Ribbon mode leaves the list empty. */
  r_geometry.stamps.clear();
  /* Set unconditionally, so Ribbon mode leaves the strip at the unwidened radius, unextended, and
   * with no stamp bound -- `stamp_mode` is re-synced per build, so without this a Stamps -> Ribbon
   * toggle mid-edit would leave stale values from the last Stamps-mode build. */
  r_geometry.ribbon_radius = params.radius;
  r_geometry.ribbon_end_margin = 0.0f;
  r_geometry.stamp_search_reach = 0.0f;

  if (params.stamp_mode == CurvePatchStampMode::Stamps) {
    curve_patch_build_stamps(params, stamp_texture_weights_cdf, r_geometry);
  }

  /* Rebuild the ribbon UV LUT the relief action samples in place of
   * `CurvePatchSpline::closest_point()`. */
  if (r_geometry.surface.ready) {
    curve_patch_spline_smooth_normals(r_geometry.spline, curve_patch_smooth_length(params));
    CurvePatchFramesParams frame_params;
    frame_params.min_window_length = 2.0f * params.radius;
    frame_params.turn_threshold_rad = params.final_quality ? float(M_PI) * 12.0f / 180.0f :
                                                            float(M_PI) * 25.0f / 180.0f;
    frame_params.break_threshold_rad = float(M_PI) * 60.0f / 180.0f;
    /* Half a brush radius of shared stretch on each side of an interior join. Enough that the
     * handover happens well inside both windows' tables rather than on their outermost rows, and
     * short enough that a window crossing a break does not rasterize a long stretch of the other
     * face nearly edge-on. */
    frame_params.overlap_length = 0.5f * params.radius;
#if CURVE_PATCH_FORCE_SINGLE_FRAME
    /* FORCE_SINGLE_FRAME: see the note at the top of this file. */
    frame_params.max_frames = 1;
#else
    frame_params.max_frames = CURVE_PATCH_MAX_FRAMES;
#endif
    curve_patch_frames_build(r_geometry.spline,
                             r_geometry.ribbon_radius,
                             frame_params,
                             params.final_quality,
                             r_geometry.ribbon_end_margin,
                             r_geometry.frames);
  }
  else {
    curve_patch_ribbon_build(r_geometry.spline,
                             r_geometry.ribbon_radius,
                             r_geometry.ribbon,
                             params.final_quality,
                             r_geometry.ribbon_end_margin,
                             r_geometry.ribbon_end_margin);
  }
}

}  // namespace blender::bke
