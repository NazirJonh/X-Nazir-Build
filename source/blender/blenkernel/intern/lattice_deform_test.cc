/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "BKE_gtest_base.hh"

#define DO_PERF_TESTS 0

#if DO_PERF_TESTS

#  include "BKE_idtype.hh"
#  include "BKE_lattice.hh"

#  include "MEM_guardedalloc.h"

#  include "DNA_lattice_types.h"
#  include "DNA_mesh_types.h"
#  include "DNA_object_types.h"

#  include "BLI_rand.hh"

namespace blender::bke::tests {

struct LatticeDeformTestContext {
  Lattice lattice;
  Object ob_lattice;
  Mesh mesh;
  Object ob_mesh;
  float (*coords)[3];
  LatticeDeformData *ldd;
};

static void test_lattice_deform_init(LatticeDeformTestContext *ctx,
                                     RandomNumberGenerator *rng,
                                     int32_t num_items)
{
  /* Generate random input data between -5 and 5. */
  ctx->coords = MEM_new_array_uninitialized<float[3]>(size_t(num_items), __func__);
  for (uint32_t index = 0; index < num_items; index++) {
    ctx->coords[index][0] = (rng->get_float() - 0.5f) * 10;
    ctx->coords[index][1] = (rng->get_float() - 0.5f) * 10;
    ctx->coords[index][2] = (rng->get_float() - 0.5f) * 10;
  }
  IDType_ID_LT.init_data(&ctx->lattice.id);
  STRNCPY(ctx->lattice.id.name, "LTLattice");
  IDType_ID_OB.init_data(&ctx->ob_lattice.id);
  ctx->ob_lattice.type = OB_LATTICE;
  ctx->ob_lattice.data = &ctx->lattice;
  IDType_ID_OB.init_data(&ctx->ob_mesh.id);
  IDType_ID_ME.init_data(&ctx->mesh.id);
  ctx->ob_mesh.type = OB_MESH;
  ctx->ob_mesh.data = &ctx->mesh;

  ctx->ldd = BKE_lattice_deform_data_create(&ctx->ob_lattice, &ctx->ob_mesh);
}

static void test_lattice_deform(LatticeDeformTestContext *ctx, int32_t num_items)
{
  for (int i = 0; i < num_items; i++) {
    float *co = &ctx->coords[i][0];
    BKE_lattice_deform_data_eval_co(ctx->ldd, co, 1.0f);
  }
}

static void test_lattice_deform_free(LatticeDeformTestContext *ctx)
{
  BKE_lattice_deform_data_destroy(ctx->ldd);
  MEM_delete(ctx->coords);
  IDType_ID_LT.free_data(&ctx->lattice.id);
  IDType_ID_OB.free_data(&ctx->ob_lattice.id);
  IDType_ID_OB.free_data(&ctx->ob_mesh.id);
  IDType_ID_ME.free_data(&ctx->mesh.id);
}

class LatticeDeformPerformanceTest : public BlenderGTestBase {};

TEST_F(LatticeDeformPerformanceTest, performance_no_dvert_1)
{
  const int32_t num_items = 1;
  LatticeDeformTestContext ctx = {dna::shallow_zero_initialize()};
  RandomNumberGenerator rng;
  test_lattice_deform_init(&ctx, &rng, num_items);
  test_lattice_deform(&ctx, num_items);
  test_lattice_deform_free(&ctx);
}
TEST_F(LatticeDeformPerformanceTest, performance_no_dvert_1000)
{
  const int32_t num_items = 1000;
  LatticeDeformTestContext ctx = {dna::shallow_zero_initialize()};
  RandomNumberGenerator rng;
  test_lattice_deform_init(&ctx, &rng, num_items);
  test_lattice_deform(&ctx, num_items);
  test_lattice_deform_free(&ctx);
}
TEST_F(LatticeDeformPerformanceTest, performance_no_dvert_10000)
{
  const int32_t num_items = 10000;
  LatticeDeformTestContext ctx = {dna::shallow_zero_initialize()};
  RandomNumberGenerator rng;
  test_lattice_deform_init(&ctx, &rng, num_items);
  test_lattice_deform(&ctx, num_items);
  test_lattice_deform_free(&ctx);
}
TEST_F(LatticeDeformPerformanceTest, performance_no_dvert_100000)
{
  const int32_t num_items = 100000;
  LatticeDeformTestContext ctx = {dna::shallow_zero_initialize()};
  RandomNumberGenerator rng;
  test_lattice_deform_init(&ctx, &rng, num_items);
  test_lattice_deform(&ctx, num_items);
  test_lattice_deform_free(&ctx);
}
TEST_F(LatticeDeformPerformanceTest, performance_no_dvert_1000000)
{
  const int32_t num_items = 1000000;
  LatticeDeformTestContext ctx = {dna::shallow_zero_initialize()};
  RandomNumberGenerator rng;
  test_lattice_deform_init(&ctx, &rng, num_items);
  test_lattice_deform(&ctx, num_items);
  test_lattice_deform_free(&ctx);
}
TEST_F(LatticeDeformPerformanceTest, performance_no_dvert_10000000)
{
  const int32_t num_items = 10000000;
  LatticeDeformTestContext ctx = {dna::shallow_zero_initialize()};
  RandomNumberGenerator rng;
  test_lattice_deform_init(&ctx, &rng, num_items);
  test_lattice_deform(&ctx, num_items);
  test_lattice_deform_free(&ctx);
}

}  // namespace blender::bke::tests
#endif

#include "BKE_idtype.hh"
#include "BKE_lattice.hh"

#include "DNA_curve_types.h"
#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_string.h"

namespace blender::bke::tests {

class LatticeDeformUpdatePointTest : public BlenderGTestBase {};

TEST_F(LatticeDeformUpdatePointTest, MatchesFullRebuild)
{
  Lattice lattice = {dna::shallow_zero_initialize()};
  Object ob_lattice = {dna::shallow_zero_initialize()};
  Mesh mesh = {dna::shallow_zero_initialize()};
  Object ob_mesh = {dna::shallow_zero_initialize()};

  IDType_ID_LT.init_data(&lattice.id);
  STRNCPY(lattice.id.name, "LTLattice");
  IDType_ID_OB.init_data(&ob_lattice.id);
  ob_lattice.type = OB_LATTICE;
  ob_lattice.data = &lattice;
  IDType_ID_OB.init_data(&ob_mesh.id);
  IDType_ID_ME.init_data(&mesh.id);
  ob_mesh.type = OB_MESH;
  ob_mesh.data = &mesh;

  LatticeDeformData *ldd = BKE_lattice_deform_data_create(&ob_lattice, &ob_mesh);
  ASSERT_NE(ldd, nullptr);
  ASSERT_NE(lattice.def, nullptr);

  const int index = 0;
  lattice.def[index].vec[0] += 0.25f;
  lattice.def[index].vec[1] += 0.15f;
  lattice.def[index].vec[2] -= 0.10f;

  BKE_lattice_deform_data_update_point(ldd, index);

  float co_update[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_update, 1.0f);

  BKE_lattice_deform_data_destroy(ldd);
  ldd = BKE_lattice_deform_data_create(&ob_lattice, &ob_mesh);
  float co_rebuild[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_rebuild, 1.0f);

  EXPECT_V3_NEAR(co_update, co_rebuild, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd);
  IDType_ID_LT.free_data(&lattice.id);
  IDType_ID_OB.free_data(&ob_lattice.id);
  IDType_ID_OB.free_data(&ob_mesh.id);
  IDType_ID_ME.free_data(&mesh.id);
}

}  // namespace blender::bke::tests

