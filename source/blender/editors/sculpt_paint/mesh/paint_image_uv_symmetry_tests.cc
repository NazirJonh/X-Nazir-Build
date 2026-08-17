/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_image_uv_symmetry.hh"

#include "BKE_gtest_base.hh"

#include "BLI_vector.hh"

#include "testing/testing.h"

namespace blender::tests {

/**
 * The axis-combination enumeration and the guards against null input are pure logic and
 * need no Mesh, Object or BVH. They are exercised through a local mirror of the loop in
 * #image_paint_symmetry_mirror_faces; the BVH-dependent half needs a full Blender context
 * and is verified by hand after the build.
 */
static int symmetry_pass_num(const int symmetry_flags)
{
  int passes = 0;
  for (int symm_it = 1; symm_it <= 7; symm_it++) {
    if ((symm_it & symmetry_flags) == symm_it) {
      passes++;
    }
  }
  return passes;
}

TEST(ImagePaintUVSymmetry, SingleAxisYieldsOnePass)
{
  EXPECT_EQ(symmetry_pass_num(1), 1);
  EXPECT_EQ(symmetry_pass_num(2), 1);
  EXPECT_EQ(symmetry_pass_num(4), 1);
}

TEST(ImagePaintUVSymmetry, TwoAxesYieldThreePasses)
{
  /* X, Y and XY. */
  EXPECT_EQ(symmetry_pass_num(1 | 2), 3);
  EXPECT_EQ(symmetry_pass_num(1 | 4), 3);
  EXPECT_EQ(symmetry_pass_num(2 | 4), 3);
}

TEST(ImagePaintUVSymmetry, ThreeAxesYieldSevenPasses)
{
  EXPECT_EQ(symmetry_pass_num(1 | 2 | 4), 7);
}

TEST(ImagePaintUVSymmetry, NoAxisYieldsNoPass)
{
  EXPECT_EQ(symmetry_pass_num(0), 0);
}

TEST(ImagePaintUVSymmetry, MirrorFacesIsNoOpWithoutSymmetryFlags)
{
  Vector<int> seed;
  seed.append(3);
  image_paint_symmetry_mirror_faces(nullptr, nullptr, float3(0.0f), 0, seed);
  EXPECT_EQ(seed.size(), 1);
  EXPECT_EQ(seed[0], 3);
}

}  // namespace blender::tests
