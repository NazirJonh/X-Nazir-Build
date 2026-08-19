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
#include "BKE_object.hh"
#include "BKE_object_types.hh"

#include "DNA_curve_types.h"
#include "DNA_key_types.h"
#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"

namespace blender::bke::tests {

class LatticeDeformUpdatePointTest : public BlenderGTestBase {};

struct LatticeDeformTestIDs {
  Lattice lattice = {dna::shallow_zero_initialize()};
  Object ob_lattice = {dna::shallow_zero_initialize()};
  Mesh mesh = {dna::shallow_zero_initialize()};
  Object ob_mesh = {dna::shallow_zero_initialize()};

  LatticeDeformTestIDs()
  {
    IDType_ID_LT.init_data(&lattice.id);
    STRNCPY(lattice.id.name, "LTLattice");
    IDType_ID_OB.init_data(&ob_lattice.id);
    ob_lattice.type = OB_LATTICE;
    ob_lattice.data = &lattice;
    IDType_ID_OB.init_data(&ob_mesh.id);
    IDType_ID_ME.init_data(&mesh.id);
    ob_mesh.type = OB_MESH;
    ob_mesh.data = &mesh;
    BKE_object_to_mat4(&ob_lattice, ob_lattice.runtime->object_to_world.ptr());
    BKE_object_to_mat4(&ob_mesh, ob_mesh.runtime->object_to_world.ptr());
  }

  LatticeDeformTestIDs(const LatticeDeformTestIDs &) = delete;
  LatticeDeformTestIDs &operator=(const LatticeDeformTestIDs &) = delete;

  ~LatticeDeformTestIDs()
  {
    IDType_ID_LT.free_data(&lattice.id);
    IDType_ID_OB.free_data(&ob_lattice.id);
    IDType_ID_OB.free_data(&ob_mesh.id);
    IDType_ID_ME.free_data(&mesh.id);
  }
};

static void offset_point(Lattice &lattice, const int index, const float3 &delta)
{
  lattice.def[index].vec[0] += delta.x;
  lattice.def[index].vec[1] += delta.y;
  lattice.def[index].vec[2] += delta.z;
}

TEST_F(LatticeDeformUpdatePointTest, MatchesFullRebuildCorner)
{
  LatticeDeformTestIDs ids;
  ASSERT_NE(ids.lattice.def, nullptr);

  LatticeDeformData *ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  ASSERT_NE(ldd, nullptr);

  const int index = 0;
  offset_point(ids.lattice, index, float3(0.25f, 0.15f, -0.10f));
  BKE_lattice_deform_data_update_point(ldd, index);

  float co_update[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_update, 1.0f);

  BKE_lattice_deform_data_destroy(ldd);
  ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  float co_rebuild[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_rebuild, 1.0f);

  EXPECT_V3_NEAR(co_update, co_rebuild, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd);
}

TEST_F(LatticeDeformUpdatePointTest, MatchesFullRebuildInteriorLinear)
{
  LatticeDeformTestIDs ids;
  BKE_lattice_resize(&ids.lattice, 3, 3, 3, nullptr);
  ids.lattice.typeu = ids.lattice.typev = ids.lattice.typew = KEY_LINEAR;

  LatticeDeformData *ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  ASSERT_NE(ldd, nullptr);

  const int index = BKE_lattice_index_from_uvw(&ids.lattice, 1, 1, 1);
  offset_point(ids.lattice, index, float3(0.20f, -0.10f, 0.05f));
  BKE_lattice_deform_data_update_point(ldd, index);

  float co_update[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_update, 1.0f);

  BKE_lattice_deform_data_destroy(ldd);
  ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  float co_rebuild[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_rebuild, 1.0f);

  EXPECT_V3_NEAR(co_update, co_rebuild, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd);
}

TEST_F(LatticeDeformUpdatePointTest, MatchesFullRebuildNonIdentityTransform)
{
  LatticeDeformTestIDs ids;
  ids.ob_lattice.loc[0] = 2.0f;
  ids.ob_lattice.loc[1] = -1.0f;
  ids.ob_lattice.scale[0] = 2.0f;
  BKE_object_to_mat4(&ids.ob_lattice, ids.ob_lattice.runtime->object_to_world.ptr());

  LatticeDeformData *ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  ASSERT_NE(ldd, nullptr);

  offset_point(ids.lattice, 0, float3(0.25f, 0.0f, 0.0f));
  BKE_lattice_deform_data_update_point(ldd, 0);

  float co_update[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_update, 1.0f);

  BKE_lattice_deform_data_destroy(ldd);
  ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  float co_rebuild[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_rebuild, 1.0f);

  EXPECT_V3_NEAR(co_update, co_rebuild, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd);
}

TEST_F(LatticeDeformUpdatePointTest, NeutralCageLeavesCoordinateUnchanged)
{
  LatticeDeformTestIDs ids;
  ids.lattice.typeu = ids.lattice.typev = ids.lattice.typew = KEY_LINEAR;
  LatticeDeformData *ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  ASSERT_NE(ldd, nullptr);

  float co[3] = {0.1f, -0.2f, 0.3f};
  const float expected[3] = {0.1f, -0.2f, 0.3f};
  BKE_lattice_deform_data_eval_co(ldd, co, 1.0f);
  EXPECT_V3_NEAR(co, expected, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd);
}

TEST_F(LatticeDeformUpdatePointTest, RestoreSecondPointMatchesFirstOnly)
{
  /* Sequential drags: confirm A, start B, restore B. The evaluator after restoring B must match
   * the state after A alone — the same contract live drag-cancel must keep. */
  LatticeDeformTestIDs ids;
  ids.lattice.typeu = ids.lattice.typev = ids.lattice.typew = KEY_LINEAR;

  const int index_a = 0;
  const int index_b = BKE_lattice_index_from_uvw(&ids.lattice,
                                                 ids.lattice.pntsu - 1,
                                                 ids.lattice.pntsv - 1,
                                                 ids.lattice.pntsw - 1);
  ASSERT_NE(index_a, index_b);

  float point_b_orig[3];
  copy_v3_v3(point_b_orig, ids.lattice.def[index_b].vec);

  LatticeDeformData *ldd = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  ASSERT_NE(ldd, nullptr);

  offset_point(ids.lattice, index_a, float3(0.30f, 0.0f, 0.0f));
  BKE_lattice_deform_data_update_point(ldd, index_a);
  float co_after_a[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_after_a, 1.0f);

  offset_point(ids.lattice, index_b, float3(0.0f, 0.40f, 0.0f));
  BKE_lattice_deform_data_update_point(ldd, index_b);
  float co_after_b[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_after_b, 1.0f);
  EXPECT_GT(len_v3v3(co_after_a, co_after_b), 1e-4f);

  copy_v3_v3(ids.lattice.def[index_b].vec, point_b_orig);
  BKE_lattice_deform_data_update_point(ldd, index_b);
  float co_restored[3] = {0.0f, 0.0f, 0.0f};
  BKE_lattice_deform_data_eval_co(ldd, co_restored, 1.0f);

  EXPECT_V3_NEAR(co_restored, co_after_a, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd);
}

TEST_F(LatticeDeformUpdatePointTest, FromLatticeMatchesObjectCreate)
{
  LatticeDeformTestIDs ids;
  ids.ob_lattice.loc[0] = 1.5f;
  ids.ob_mesh.loc[1] = -0.5f;
  BKE_object_to_mat4(&ids.ob_lattice, ids.ob_lattice.runtime->object_to_world.ptr());
  BKE_object_to_mat4(&ids.ob_mesh, ids.ob_mesh.runtime->object_to_world.ptr());

  LatticeDeformData *ldd_ob = BKE_lattice_deform_data_create(&ids.ob_lattice, &ids.ob_mesh);
  LatticeDeformData *ldd_lt = BKE_lattice_deform_data_create_from_lattice(
      &ids.lattice,
      ids.ob_lattice.object_to_world().ptr(),
      ids.ob_mesh.object_to_world().ptr());
  ASSERT_NE(ldd_ob, nullptr);
  ASSERT_NE(ldd_lt, nullptr);

  offset_point(ids.lattice, 0, float3(0.20f, -0.10f, 0.05f));
  BKE_lattice_deform_data_update_point(ldd_ob, 0);
  BKE_lattice_deform_data_update_point(ldd_lt, 0);

  float co_ob[3] = {0.1f, 0.2f, -0.3f};
  float co_lt[3] = {0.1f, 0.2f, -0.3f};
  BKE_lattice_deform_data_eval_co(ldd_ob, co_ob, 1.0f);
  BKE_lattice_deform_data_eval_co(ldd_lt, co_lt, 1.0f);
  EXPECT_V3_NEAR(co_ob, co_lt, 1e-5f);

  BKE_lattice_deform_data_destroy(ldd_ob);
  BKE_lattice_deform_data_destroy(ldd_lt);
}

}  // namespace blender::bke::tests

