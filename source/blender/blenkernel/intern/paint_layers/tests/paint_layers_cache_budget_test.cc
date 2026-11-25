/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The paint cache budget table and the preference that scales it: the defaults sum to the 8 GiB
 * the UI names, the scale uses that sum as its denominator, a positive preference never silently
 * switches a cache off, and the sync applies the result to every cache the table lists.
 */

#include <climits>

#include "paint_layers_test_util.hh"

#include "BKE_global.hh"
#include "BKE_paint_layers_composite.hh"

#include "DNA_userdef_types.h"

#include "../paint_layers_cache_budgets.hh"

namespace blender::bke::tests {

namespace {

constexpr int64_t mib = int64_t(1) << 20;

}  // namespace

TEST(paint_layers_cache_budget, defaults_sum_to_eight_gib)
{
  int64_t sum = 0;
  for (const paint_cache::PaintCacheBudgetEntry &entry :
       paint_cache::paint_cache_budget_entries())
  {
    EXPECT_GT(entry.default_bytes, 0);
    sum += entry.default_bytes;
  }
  /* The proportion's denominator, which the UI and DNA name as "8 GiB". */
  EXPECT_EQ(sum, int64_t(8192) * mib);
  EXPECT_EQ(paint_cache::paint_cache_budget_total_default_bytes(), sum);

  /* The list is the one the table names, and a name lookup answers that entry's default. */
  EXPECT_EQ(BKE_paint_layers_cache_budget_default_bytes("anchor_slices"), int64_t(4096) * mib);
  EXPECT_EQ(BKE_paint_layers_cache_budget_default_bytes("stroke_corrections"), int64_t(768) * mib);
  EXPECT_EQ(BKE_paint_layers_cache_budget_default_bytes("composite"), int64_t(256) * mib);
  EXPECT_EQ(BKE_paint_layers_cache_budget_default_bytes("does_not_exist"), 0);
}

class PaintCacheBudgetTest : public BlenderGTestBase {
 public:
  int pref_before_ = 0;

  void SetUp() override
  {
    pref_before_ = U.paint_layers_cache_budget_mb;
  }

  void TearDown() override
  {
    /* The preference is process-global: leave it and the caches exactly as the test found them. */
    U.paint_layers_cache_budget_mb = pref_before_;
    BKE_paint_layers_cache_budgets_sync_from_userdef();
  }
};

TEST_F(PaintCacheBudgetTest, pref_zero_keeps_defaults)
{
  U.paint_layers_cache_budget_mb = 0;
  for (const paint_cache::PaintCacheBudgetEntry &entry :
       paint_cache::paint_cache_budget_entries())
  {
    EXPECT_EQ(BKE_paint_layers_cache_budget_bytes(entry.default_bytes), entry.default_bytes);
  }
}

TEST_F(PaintCacheBudgetTest, positive_pref_scales_and_floors)
{
  /* The denominator is the table's byte sum, but the preference is MiB: the scale has to divide
   * by the sum in MiB, or every result is MiB-sized bytes and the floor hides it. */
  const int64_t total_mb = paint_cache::paint_cache_budget_total_default_bytes() >> 20;
  /* 1 and 7 MiB scale most caches to zero by integer division; the floor must keep them caching. */
  for (const int pref : {1, 7, 8, 8192}) {
    U.paint_layers_cache_budget_mb = pref;
    for (const paint_cache::PaintCacheBudgetEntry &entry :
         paint_cache::paint_cache_budget_entries())
    {
      const int64_t scaled = entry.default_bytes * int64_t(pref) / total_mb;
      const int64_t expected = scaled < mib ? mib : scaled;
      EXPECT_EQ(BKE_paint_layers_cache_budget_bytes(entry.default_bytes), expected)
          << "pref=" << pref << " id=" << entry.id;
    }
  }

  /* Explicit anchors: 8192 MiB is the default sum, so each cache gets its own default back; half
   * of it halves the larger caches. A unit error that returns MiB-sized bytes fails here. */
  U.paint_layers_cache_budget_mb = 8192;
  EXPECT_EQ(BKE_paint_layers_cache_budget_bytes(paint_cache::anchor_slice_cache_default_bytes),
            paint_cache::anchor_slice_cache_default_bytes);
  EXPECT_EQ(BKE_paint_layers_cache_budget_bytes(paint_cache::effect_cache_default_bytes),
            paint_cache::effect_cache_default_bytes);
  U.paint_layers_cache_budget_mb = 4096;
  EXPECT_EQ(BKE_paint_layers_cache_budget_bytes(paint_cache::anchor_slice_cache_default_bytes),
            paint_cache::anchor_slice_cache_default_bytes / 2);
  EXPECT_EQ(BKE_paint_layers_cache_budget_bytes(paint_cache::effect_cache_default_bytes),
            paint_cache::effect_cache_default_bytes / 2);
}

TEST_F(PaintCacheBudgetTest, extreme_pref_does_not_overflow)
{
  /* 4 GiB times INT_MAX stays inside int64; the scale is the largest default's share. */
  U.paint_layers_cache_budget_mb = INT_MAX;
  const int64_t largest = BKE_paint_layers_cache_budget_default_bytes("anchor_slices");
  const int64_t value = BKE_paint_layers_cache_budget_bytes(largest);
  EXPECT_GT(value, 0);
  /* The product is safe and the MiB division is exact for the table's sum; compare against the
   * same arithmetic written out, so a wrapped intermediate would differ. */
  EXPECT_EQ(value, largest * int64_t(INT_MAX) /
                       (paint_cache::paint_cache_budget_total_default_bytes() >> 20));
}

TEST_F(PaintCacheBudgetTest, sync_applies_to_every_cache)
{
  ScopedPaintCacheBudgets guard;

  U.paint_layers_cache_budget_mb = 4096;
  BKE_paint_layers_cache_budgets_sync_from_userdef();
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_budget(),
            size_t(BKE_paint_layers_cache_budget_bytes(
                paint_cache::anchor_slice_cache_default_bytes)));
  EXPECT_EQ(paint_layer_effect_cache_budget(),
            size_t(BKE_paint_layers_cache_budget_bytes(
                paint_cache::effect_cache_default_bytes)));
  EXPECT_EQ(paint_layer_mask_effect_cache_budget(),
            size_t(BKE_paint_layers_cache_budget_bytes(
                paint_cache::mask_effect_cache_default_bytes)));
  EXPECT_EQ(paint_layer_effect_gpu_cache_budget(),
            size_t(BKE_paint_layers_cache_budget_bytes(
                paint_cache::gpu_effect_cache_default_bytes)));
  EXPECT_EQ(paint_material_cache::combined_cache_get_budget(),
            BKE_paint_layers_cache_budget_bytes(paint_cache::combined_cache_default_bytes));
  EXPECT_EQ(paint_material_cache::composite_cache_get_budget(),
            BKE_paint_layers_cache_budget_bytes(paint_cache::composite_cache_default_bytes));

  /* Unset restores every built-in default. */
  U.paint_layers_cache_budget_mb = 0;
  BKE_paint_layers_cache_budgets_sync_from_userdef();
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_budget(),
            size_t(paint_cache::anchor_slice_cache_default_bytes));
  EXPECT_EQ(paint_layer_effect_cache_budget(), size_t(paint_cache::effect_cache_default_bytes));
  EXPECT_EQ(paint_layer_mask_effect_cache_budget(),
            size_t(paint_cache::mask_effect_cache_default_bytes));
  EXPECT_EQ(paint_layer_effect_gpu_cache_budget(),
            size_t(paint_cache::gpu_effect_cache_default_bytes));
  EXPECT_EQ(paint_material_cache::combined_cache_get_budget(),
            paint_cache::combined_cache_default_bytes);
  EXPECT_EQ(paint_material_cache::composite_cache_get_budget(),
            paint_cache::composite_cache_default_bytes);
}

}  // namespace blender::bke::tests
