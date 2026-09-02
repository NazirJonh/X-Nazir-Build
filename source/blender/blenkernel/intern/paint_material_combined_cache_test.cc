/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_material_combined.hh"

#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Combined Cache
 * \{ */

class CombinedCacheTest : public bke::BlenderGTestBase {
 public:
  static constexpr int size = 8;

  Main *bmain = nullptr;
  Material *material_ = nullptr;
  Material *other_material_ = nullptr;
  Image *image_a_ = nullptr;
  Image *image_b_ = nullptr;

  CombinedInputs inputs_;
  CombinedPreviewLighting lighting_;
  rcti empty_region_ = {0, 0, 0, 0};

  void SetUp() override
  {
    bmain = BKE_main_new();
    material_ = BKE_material_add(bmain, "Mat");
    other_material_ = BKE_material_add(bmain, "Other");

    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    image_a_ = BKE_image_add_generated(
        bmain, size, size, "A", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image_b_ = BKE_image_add_generated(
        bmain, size, size, "B", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);

    inputs_.width = size;
    inputs_.height = size;
    for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      inputs_.channels[i].constant = BKE_paint_material_combined_default_value(
          eMaterialPaintChannel(i));
    }
    lighting_ = BKE_paint_material_combined_lighting_default();
  }

  void TearDown() override
  {
    BKE_paint_material_combined_cache_free_all();
    BKE_main_free(bmain);
  }
};

TEST_F(CombinedCacheTest, unchanged_inputs_are_not_re_evaluated)
{
  CombinedEvalStats stats;
  uint64_t first_revision = 0;
  ImBuf *a = BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {}, &first_revision, &stats);
  ASSERT_NE(a, nullptr);
  EXPECT_GT(stats.pixels_processed, 0);

  uint64_t second_revision = 0;
  stats = CombinedEvalStats{};
  ImBuf *b = BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {}, &second_revision, &stats);
  /* The steady state during pan and zoom: same buffer, same revision, no work at all. */
  EXPECT_EQ(a, b);
  EXPECT_EQ(first_revision, second_revision);
  EXPECT_EQ(stats.pixels_processed, 0);
}

TEST_F(CombinedCacheTest, structural_change_forces_a_full_rebuild)
{
  uint64_t revision_a = 0;
  uint64_t revision_b = 0;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {}, &revision_a);
  CombinedEvalStats stats;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 5678, empty_region_, {}, &revision_b, &stats);
  EXPECT_NE(revision_a, revision_b);
  EXPECT_EQ(stats.pixels_processed, int64_t(inputs_.width) * inputs_.height);
}

TEST_F(CombinedCacheTest, lighting_change_forces_a_full_rebuild)
{
  uint64_t revision_a = 0;
  uint64_t revision_b = 0;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {}, &revision_a);
  CombinedPreviewLighting moved = lighting_;
  moved.lights[0].direction = math::normalize(float3(1.0f, 1.0f, 1.0f));
  ASSERT_NE(moved.hash(), lighting_.hash());
  CombinedEvalStats stats;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, moved, 1234, empty_region_, {}, &revision_b, &stats);
  EXPECT_NE(revision_a, revision_b);
  EXPECT_EQ(stats.pixels_processed, int64_t(inputs_.width) * inputs_.height);
}

TEST_F(CombinedCacheTest, changed_region_rebuilds_only_that_rectangle)
{
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {});
  rcti dab;
  BLI_rcti_init(&dab, 2, 6, 2, 6);
  CombinedEvalStats stats;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, dab, {}, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, 16);
}

TEST_F(CombinedCacheTest, tagged_region_reaches_only_declared_dependencies)
{
  const uint32_t declared = image_a_->id.session_uid;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, Span<uint32_t>(&declared, 1));

  rcti dab;
  BLI_rcti_init(&dab, 0, 4, 0, 4);
  BKE_paint_material_combined_cache_tag_image_region(*image_b_, dab); /* Not a dependency. */
  CombinedEvalStats stats;
  BKE_paint_material_combined_cache_ensure(*material_,
                                           inputs_,
                                           lighting_,
                                           1234,
                                           empty_region_,
                                           Span<uint32_t>(&declared, 1),
                                           nullptr,
                                           &stats);
  EXPECT_EQ(stats.pixels_processed, 0);

  BKE_paint_material_combined_cache_tag_image_region(*image_a_, dab); /* Is a dependency. */
  stats = CombinedEvalStats{};
  BKE_paint_material_combined_cache_ensure(*material_,
                                           inputs_,
                                           lighting_,
                                           1234,
                                           empty_region_,
                                           Span<uint32_t>(&declared, 1),
                                           nullptr,
                                           &stats);
  EXPECT_EQ(stats.pixels_processed, 16);
}

TEST_F(CombinedCacheTest, free_material_drops_only_that_entry)
{
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {});
  BKE_paint_material_combined_cache_ensure(
      *other_material_, inputs_, lighting_, 1234, empty_region_, {});
  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*material_));
  BKE_paint_material_combined_cache_free_material(*material_);
  EXPECT_FALSE(BKE_paint_material_combined_cache_contains(*material_));
  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*other_material_));
  BKE_paint_material_combined_cache_free_all();
  EXPECT_FALSE(BKE_paint_material_combined_cache_contains(*other_material_));
}

TEST_F(CombinedCacheTest, invalidate_marks_rather_than_drops)
{
  ImBuf *before = BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {});
  ASSERT_NE(before, nullptr);
  BKE_paint_material_combined_cache_invalidate(material_);
  /* Marking, not dropping: a caller asking in between must get the previous pixels rather than
   * nothing, exactly as the composite cache behaves. */
  EXPECT_TRUE(BKE_paint_material_combined_cache_contains(*material_));
  CombinedEvalStats stats;
  BKE_paint_material_combined_cache_ensure(
      *material_, inputs_, lighting_, 1234, empty_region_, {}, nullptr, &stats);
  EXPECT_EQ(stats.pixels_processed, int64_t(inputs_.width) * inputs_.height);
}

TEST_F(CombinedCacheTest, zero_sized_inputs_create_no_entry)
{
  CombinedInputs empty = inputs_;
  empty.width = 0;
  empty.height = 0;
  EXPECT_EQ(BKE_paint_material_combined_cache_ensure(
                *material_, empty, lighting_, 1234, empty_region_, {}),
            nullptr);
  EXPECT_FALSE(BKE_paint_material_combined_cache_contains(*material_));
}

/** \} */

}  // namespace blender::bke::tests
