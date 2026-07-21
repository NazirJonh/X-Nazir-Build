/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <array>
#include <string>

#include "BLI_math_base.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.hh"

#include "BKE_curve_patch.hh"

#include "paint_curve_patch_sampler.hh"

namespace blender::ed::sculpt_paint::tests {

/* -------------------------------------------------------------------- */
/** \name Symmetry Canonicalization
 *
 * The sampler and both node culls map world positions into the patch's canonical frame through the
 * same helper. They MUST agree: a cull that canonicalizes differently drops nodes the sampler would
 * have accepted, which shows up as relief missing from whole regions of a mirrored pass rather than
 * as anything obviously wrong.
 * \{ */

TEST(paint_curve_patch_sampler, canonicalize_is_identity_for_the_first_pass)
{
  const CurvePatchStrokeContext ctx;
  const float3 co(1.5f, -2.25f, 3.0f);
  EXPECT_V3_NEAR(curve_patch_canonicalize(ctx, co), co, 1e-6f);
}

TEST(paint_curve_patch_sampler, canonicalize_mirrors_the_axes_the_pass_names)
{
  const float3 co(1.5f, -2.25f, 3.0f);
  const struct {
    ePaintSymmetryFlags symm;
    float3 expected;
  } cases[] = {
      {PAINT_SYMM_X, float3(-1.5f, -2.25f, 3.0f)},
      {PAINT_SYMM_Y, float3(1.5f, 2.25f, 3.0f)},
      {PAINT_SYMM_Z, float3(1.5f, -2.25f, -3.0f)},
      {ePaintSymmetryFlags(PAINT_SYMM_X | PAINT_SYMM_Z), float3(-1.5f, -2.25f, -3.0f)},
      {ePaintSymmetryFlags(PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z),
       float3(-1.5f, 2.25f, -3.0f)},
  };
  for (const auto &test_case : cases) {
    SCOPED_TRACE("symmetry flags: " + std::to_string(int(test_case.symm)));
    CurvePatchStrokeContext ctx;
    ctx.mirror_symmetry_pass = test_case.symm;
    EXPECT_V3_NEAR(curve_patch_canonicalize(ctx, co), test_case.expected, 1e-6f);
  }
}

TEST(paint_curve_patch_sampler, canonicalize_ignores_the_rotation_on_the_zeroth_radial_pass)
{
  /* `radial_symmetry_pass == 0` is the pass the patch was built in, so its matrix must not be
   * applied even when one is present -- the sampler and the culls both gate on the counter, not on
   * whether the matrix happens to be the identity. */
  CurvePatchStrokeContext ctx;
  ctx.radial_symmetry_pass = 0;
  ctx.symm_rot_mat_inv = math::from_rotation<float4x4>(
      math::AxisAngle(float3(0.0f, 0.0f, 1.0f), float(M_PI_2)));
  const float3 co(1.0f, 0.0f, 0.0f);
  EXPECT_V3_NEAR(curve_patch_canonicalize(ctx, co), co, 1e-6f);
}

TEST(paint_curve_patch_sampler, canonicalize_applies_the_radial_rotation_after_the_mirror)
{
  /* Order is observable: mirroring and rotating do not commute. The helper flips first and rotates
   * the flipped point, which is the order the sampler used before it was extracted. */
  CurvePatchStrokeContext ctx;
  ctx.mirror_symmetry_pass = PAINT_SYMM_X;
  ctx.radial_symmetry_pass = 1;
  ctx.symm_rot_mat_inv = math::from_rotation<float4x4>(
      math::AxisAngle(float3(0.0f, 0.0f, 1.0f), float(M_PI_2)));

  const float3 co(1.0f, 0.0f, 0.0f);
  /* Flip about X gives (-1, 0, 0); a quarter turn about +Z then takes it to (0, -1, 0). */
  EXPECT_V3_NEAR(curve_patch_canonicalize(ctx, co), float3(0.0f, -1.0f, 0.0f), 1e-6f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cull / Sampler Agreement
 * \{ */

/** A geometry carrying only what the two reach helpers read: the per-point radius scalars, the
 * radius the ribbon was built with, and the end extension. Building it directly is the whole point
 * of both helpers taking a #bke::CurvePatchGeometry -- no sculpt session, no mesh, no Paint BVH. */
static bke::CurvePatchGeometry geometry_with_reach(const Span<float> radii,
                                                   const float ribbon_radius,
                                                   const float ribbon_end_margin)
{
  bke::CurvePatchGeometry geometry;
  geometry.spline.radii.extend(radii);
  geometry.ribbon_radius = ribbon_radius;
  geometry.ribbon_end_margin = ribbon_end_margin;
  return geometry;
}

TEST(paint_curve_patch_sampler, max_radius_takes_the_largest_scalar_scaled_by_the_ribbon_radius)
{
  /* `std::array`, not a raw C array: `blender::Span` has no constructor for the latter. */
  const std::array<float, 3> radii = {0.25f, 1.0f, 0.5f};
  const bke::CurvePatchGeometry geometry = geometry_with_reach(radii, 2.0f, 0.0f);
  EXPECT_FLOAT_EQ(curve_patch_max_radius(geometry), 2.0f);
}

TEST(paint_curve_patch_sampler, max_radius_is_zero_for_a_spline_with_no_radii)
{
  const bke::CurvePatchGeometry geometry = geometry_with_reach({}, 2.0f, 0.0f);
  EXPECT_FLOAT_EQ(curve_patch_max_radius(geometry), 0.0f);
}

TEST(paint_curve_patch_sampler, cull_tube_covers_everything_the_sampler_can_accept)
{
  /* The invariant the two helpers exist to protect: whatever the sampler can accept, the cull must
   * keep. The sampler rejects a sample once its distance from the curve exceeds
   * `radius_at(s) * params.radius`, and admits arc lengths out to `ribbon_end_margin` past either
   * end; the cull keeps a node whose bounds come within `curve_patch_cull_tube_radius()` of the
   * polyline. So the tube has to cover the widest falloff radius PLUS the end extension.
   *
   * `params.radius` is never greater than `ribbon_radius` -- Stamps mode only ever widens the
   * ribbon, by the jitter amount -- so scaling the largest scalar by `ribbon_radius` is already an
   * upper bound on the sampler's own reach. */
  const std::array<float, 3> radii = {0.25f, 1.0f, 0.5f};
  for (const float ribbon_radius : {0.5f, 1.0f, 4.0f}) {
    for (const float end_margin : {0.0f, 0.3f, 2.0f}) {
      SCOPED_TRACE("ribbon_radius " + std::to_string(ribbon_radius) + ", end_margin " +
                   std::to_string(end_margin));
      const bke::CurvePatchGeometry geometry = geometry_with_reach(
          radii, ribbon_radius, end_margin);
      const float max_radius = curve_patch_max_radius(geometry);
      const float sampler_reach = max_radius + end_margin;
      EXPECT_GT(curve_patch_cull_tube_radius(geometry, max_radius), sampler_reach);
    }
  }
}

TEST(paint_curve_patch_sampler, cull_tube_still_covers_the_end_extension_on_a_zero_width_curve)
{
  /* A degenerate curve contributes no falloff radius at all, so the end extension is the ONLY term
   * left -- and dropping it here would clip exactly the overhanging halves of the end stamps the
   * extension exists to render. */
  const bke::CurvePatchGeometry geometry = geometry_with_reach({}, 1.0f, 0.75f);
  EXPECT_FLOAT_EQ(curve_patch_cull_tube_radius(geometry, curve_patch_max_radius(geometry)), 0.75f);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::tests
