/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Stamp layout along the Curve Patch control curve, including the wrap-around ghosts a closed
 * curve needs at its join. Every random quantity is a pure function of (index, seed, channel) --
 * see #curve_patch_stamps_build for why a stateful generator is not usable here.
 */

#include <algorithm>
#include <cmath>

#include "BKE_curve_patch.hh"

#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"

namespace blender::bke {

/* Hash channels, so the same stamp's size / angle / strength / two jitter axes stay independent. */
enum {
  STAMP_HASH_SIZE = 0,
  STAMP_HASH_ANGLE = 1,
  STAMP_HASH_STRENGTH = 2,
  STAMP_HASH_JITTER_V = 3,
  STAMP_HASH_JITTER_U = 4,
  STAMP_HASH_TEX = 5,
};

static float stamp_random(const int index, const uint32_t seed, const int channel)
{
  return BLI_hash_int_3d_to_float(uint32_t(index), seed, uint32_t(channel));
}

/* Absolute ceiling on the number of stamps a single build can produce. The 1% Spacing floor used
 * below is relative to the brush radius, not an absolute step size, so it does not by itself bound
 * `stamp_num`: a long curve paired with a very small radius still yields a tiny step and asks for
 * millions of stamps (e.g. a 1000-unit curve at radius 0.001 wants ~50 million). An interactive
 * patch tops out at a few hundred stamps even for a full-screen curve at default spacing, so 10000
 * is generous headroom while keeping the `reserve()` below bounded and the narrowing to `int` safe
 * from overflow. */
static constexpr int CURVE_PATCH_STAMP_NUM_MAX = 10000;

void curve_patch_stamps_build(const CurvePatchSpline &spline,
                              const float radius,
                              const float spacing_frac,
                              const float jitter_amount,
                              const float size_random,
                              const float strength_random,
                              const float base_angle,
                              const float random_angle,
                              const uint32_t seed,
                              const Span<float> texture_weights_cdf,
                              Vector<CurvePatchStamp> &r_stamps)
{
  r_stamps.clear();
  const float total_length = spline.total_length();
  if (spline.is_empty() || total_length <= 1e-6f || radius <= 1e-6f) {
    return;
  }

  /* Floor Spacing at 1% of the diameter, the same lower bound the brush's own Spacing slider
   * enforces, so a zero or near-zero Spacing does not produce a zero-length `step`. This floor is
   * relative to `radius`, though, not an absolute distance, so it does not by itself bound the
   * stamp count -- see `CURVE_PATCH_STAMP_NUM_MAX`, which is what actually does. */
  const float step = std::max(spacing_frac, 0.01f) * 2.0f * radius;

  int stamp_num;
  float used_step;
  if (spline.cyclic) {
    /* A loop has no ends: placing a stamp at `total_length` would double up on the one at 0. Fit a
     * whole number of steps instead and stop one step short of the seam. Compute in `double` and
     * clamp before narrowing to `int`, since `total_length / step` can be arbitrarily large for a
     * long curve with a small radius, and an `int` overflow here is undefined behavior. */
    const double raw_num = std::round(double(total_length) / double(step));
    stamp_num = int(std::clamp(raw_num, 1.0, double(CURVE_PATCH_STAMP_NUM_MAX)));
    /* `used_step` is derived FROM `stamp_num` on this branch, so it must be recomputed after the
     * clamp above -- otherwise a clamped count would leave a gap short of the seam. */
    used_step = total_length / float(stamp_num);
  }
  else {
    const double raw_num = std::floor(double(total_length) / double(step)) + 1.0;
    stamp_num = int(std::clamp(raw_num, 1.0, double(CURVE_PATCH_STAMP_NUM_MAX)));
    /* `used_step` is the requested step and is independent of `stamp_num` on this branch, so
     * clamping the count simply truncates the stamp row instead of leaving a gap. */
    used_step = step;
  }

  r_stamps.reserve(stamp_num);
  for (const int i : IndexRange(stamp_num)) {
    CurvePatchStamp stamp;
    const float s = float(i) * used_step;
    stamp.center_v = s + jitter_amount * (2.0f * stamp_random(i, seed, STAMP_HASH_JITTER_V) - 1.0f);
    stamp.center_u = jitter_amount * (2.0f * stamp_random(i, seed, STAMP_HASH_JITTER_U) - 1.0f);
    /* Shrink only. Growing past the brush radius would make the visible patch wider than the brush
     * cursor promises and would force the ribbon to widen for the size setting too. The 0.05 floor
     * keeps a fully-randomized stamp from collapsing to nothing. */
    const float size_factor = 1.0f - size_random * stamp_random(i, seed, STAMP_HASH_SIZE);
    stamp.half_extent = radius * std::max(size_factor, 0.05f);
    stamp.angle = base_angle + random_angle * stamp_random(i, seed, STAMP_HASH_ANGLE);
    const float strength_factor = 1.0f -
                                  strength_random * stamp_random(i, seed, STAMP_HASH_STRENGTH);
    stamp.strength = std::max(strength_factor, 0.05f);
    stamp.tex_index = curve_patch_stamp_pick_texture(texture_weights_cdf,
                                                     stamp_random(i, seed, STAMP_HASH_TEX));

    /* `center_v` is jittered and routinely lands outside `[0, total_length]` -- the first stamp goes
     * negative about half the time, the last overshoots. `evaluate()`/`tangent_at()` CLAMP, which
     * would collapse those stamps' frames -- both position AND orientation -- onto the curve's
     * endpoints instead of leaving them overhanging where the ribbon's `end_margin` extension renders
     * them. Resolve a single `frame_s` first and sample both position and tangent from it, so the
     * frame that ends up rotated is the same one that ends up placed. A loop has no ends, so wrap; an
     * open curve clamps to the end and keeps that end's tangent for both the extrapolated position and
     * the orientation, matching how the ribbon extends its own strip as a rigid straight continuation. */
    float frame_s = stamp.center_v;
    float3 frame_base;
    float3 tangent;
    if (spline.cyclic) {
      frame_s -= std::floor(frame_s / total_length) * total_length;
      frame_base = spline.evaluate(frame_s);
      tangent = spline.tangent_at(frame_s);
    }
    else {
      frame_s = std::clamp(frame_s, 0.0f, total_length);
      tangent = spline.tangent_at(frame_s);
      frame_base = spline.evaluate(frame_s) + tangent * (stamp.center_v - frame_s);
    }

    /* Freeze the stamp's rigid world frame. `side` uses `cross(T, plane_normal)`, matching the
     * ribbon's own convention (`curve_patch_ribbon_build()`: the `+B` side carries `u = +1`), so a
     * stamp's `center_u` means the same direction in both projections. Getting this backwards would
     * mirror every PLANAR stamp against its CURVE counterpart. */
    float3 side = math::cross(tangent, spline.plane_normal);
    const float side_len = math::length(side);
    if (side_len > 1e-7f) {
      side /= side_len;
    }
    else {
      /* The tangent is parallel to the plane normal, so the cross product carries no direction.
       * Any in-plane axis will do: the stamp is degenerate here either way, and a finite frame
       * keeps the relief from producing NaNs. Mirrors the ribbon's `axis_x` construction
       * (`curve_patch_ribbon_build()`), not its degenerate-`B` border fallback -- both are simply
       * some vector perpendicular to `plane_normal`, which is all that is required here. */
      float3 fallback = math::cross(spline.plane_normal, float3(0.0f, 0.0f, 1.0f));
      if (math::length_squared(fallback) < 1e-6f) {
        fallback = math::cross(spline.plane_normal, float3(1.0f, 0.0f, 0.0f));
      }
      side = math::normalize(fallback);
    }
    /* Re-derive the along-curve axis from `side` rather than reusing `tangent`: `cross(N, side)` is
     * `tangent` projected into the anchor plane and re-normalized in one step, which is what makes
     * the pair exactly orthonormal even where the curve tilts out of the plane. */
    const float3 tangent_in_plane = math::cross(spline.plane_normal, side);
    const float cos_a = std::cos(stamp.angle);
    const float sin_a = std::sin(stamp.angle);
    stamp.origin = frame_base + side * stamp.center_u;
    stamp.axis_v = tangent_in_plane * cos_a + side * sin_a;
    stamp.axis_u = -tangent_in_plane * sin_a + side * cos_a;

    r_stamps.append(stamp);
  }

  /* Jitter along the curve can reorder neighbours; the relief binary-searches this list by
   * `center_v`, so restore the ordering rather than assume it. */
  std::sort(r_stamps.begin(), r_stamps.end(), [](const CurvePatchStamp &a, const CurvePatchStamp &b) {
    return a.center_v < b.center_v;
  });
}

void curve_patch_stamps_add_cyclic_wrap(Vector<CurvePatchStamp> &stamps,
                                        const float total_length,
                                        const float max_extent)
{
  if (stamps.is_empty() || total_length <= 1e-6f || max_extent <= 0.0f) {
    return;
  }

  /* A loop shorter than the search window would otherwise qualify every stamp on both sides at
   * once, and a truly faithful answer there would need ghosts at every multiple of the length --
   * an unbounded tiling for a loop that is smaller than a single stamp. Clamping the reach to one
   * period keeps the result bounded (at most two ghosts per stamp) and deterministic; such a loop
   * is degenerate for Stamps mode either way, since its stamps already overlap themselves. */
  const float reach = std::min(max_extent, total_length);

  /* Snapshot the original count: the loop appends to the same vector it reads, and ghosts must
   * never spawn ghosts of their own. */
  const int real_num = stamps.size();
  for (const int i : IndexRange(real_num)) {
    /* Taken by value -- `append()` below may reallocate and invalidate a reference. */
    const CurvePatchStamp stamp = stamps[i];
    if (stamp.center_v < reach) {
      CurvePatchStamp ghost = stamp;
      ghost.center_v = stamp.center_v + total_length;
      stamps.append(ghost);
    }
    if (stamp.center_v > total_length - reach) {
      CurvePatchStamp ghost = stamp;
      ghost.center_v = stamp.center_v - total_length;
      stamps.append(ghost);
    }
  }

  /* The ghosts land outside `[0, total_length]` on both sides, so the list is no longer ordered by
   * arc length -- restore it, since the relief binary-searches by `center_v`. */
  std::sort(stamps.begin(), stamps.end(), [](const CurvePatchStamp &a, const CurvePatchStamp &b) {
    return a.center_v < b.center_v;
  });
}

}  // namespace blender::bke
