/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** The per-curve mirror mask (#PaintVectorCurveParams::symmetry_flags) must keep only the axis
 * bits on decode and survive an encode/decode round trip unchanged. */

#include "testing/testing.h"

#include "BKE_curve_patch.hh"
#include "BKE_paint_vector.hh"

#include "DNA_paint_vector_types.h"
#include "DNA_scene_enums.h"

namespace blender::bke::tests {

TEST(paint_vector_curve_params, DecodeMasksSymmetryFlagsToAxisBits)
{
  PaintVectorCurveParams stored;

  /* Every bit set: only X, Y and Z may survive. */
  stored.symmetry_flags = 0xFF;
  EXPECT_EQ(BKE_paint_vector_curve_params_decode(stored).symmetry_flags, PAINT_SYMM_AXIS_ALL);

  /* A bit outside the axis mask (feather/tile/shared-origin range) is dropped. */
  stored.symmetry_flags = 0x8;
  EXPECT_EQ(BKE_paint_vector_curve_params_decode(stored).symmetry_flags, 0);

  /* Zero-filled data from before the field existed stays off. */
  stored.symmetry_flags = 0;
  EXPECT_EQ(BKE_paint_vector_curve_params_decode(stored).symmetry_flags, 0);
}

TEST(paint_vector_curve_params, EncodeDecodeRoundTripPreservesSymmetryFlags)
{
  const int cases[] = {PAINT_SYMM_X,
                       PAINT_SYMM_Y,
                       PAINT_SYMM_Z,
                       PAINT_SYMM_X | PAINT_SYMM_Z};
  for (const int flags : cases) {
    PaintVectorCurveParams stored;
    stored.symmetry_flags = flags;

    const bke::CurvePatchParams decoded = BKE_paint_vector_curve_params_decode(stored);
    EXPECT_EQ(decoded.symmetry_flags, flags);

    const PaintVectorCurveParams encoded = BKE_paint_vector_curve_params_encode(decoded);
    EXPECT_EQ(encoded.symmetry_flags, flags);
    EXPECT_EQ(BKE_paint_vector_curve_params_decode(encoded).symmetry_flags, flags);
  }
}

}  // namespace blender::bke::tests
