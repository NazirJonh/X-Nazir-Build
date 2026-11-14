/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** The parameter hash a secondary target mirrors must be deterministic and must react to the
 * fields it covers, otherwise a change would not reach the mirror. */

#include "../paint_curve_patch_document.hh"

#include "DNA_scene_enums.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint {

TEST(paint_curve_patch_params_revision, Deterministic)
{
  const CurvePatchParams params{};
  EXPECT_EQ(curve_patch_params_revision(params), curve_patch_params_revision(params));
}

TEST(paint_curve_patch_params_revision, SymmetryFlagsChangeHash)
{
  CurvePatchParams params{};
  params.symmetry_flags = 0;
  const uint64_t hash_none = curve_patch_params_revision(params);

  params.symmetry_flags = PAINT_SYMM_X;
  const uint64_t hash_x = curve_patch_params_revision(params);
  EXPECT_NE(hash_none, hash_x);

  params.symmetry_flags = PAINT_SYMM_X | PAINT_SYMM_Z;
  const uint64_t hash_xz = curve_patch_params_revision(params);
  EXPECT_NE(hash_x, hash_xz);
}

TEST(paint_curve_patch_params_revision, OpacityChangesHashControl)
{
  CurvePatchParams params{};
  params.opacity = 1.0f;
  const uint64_t hash_a = curve_patch_params_revision(params);

  params.opacity = 0.5f;
  EXPECT_NE(hash_a, curve_patch_params_revision(params));
}

}  // namespace blender::ed::sculpt_paint
