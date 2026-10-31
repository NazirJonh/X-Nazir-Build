/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for the material-canvas owner of the user-authored paint selection
 * (#BKE_image_paint_selection_owner_bind and friends).
 */

#include "testing/testing.h"

#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_material.hh"

#include "BLI_index_range.hh"

#include "DNA_material_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_layers/tests/paint_layers_test_util.hh"

namespace blender::bke::tests {

class ImagePaintSelectionOwnerTest : public PaintLayersTestBase {
 public:
  Image *add_bound_image(const char *name)
  {
    Image *image = add_solid_image(name);
    BKE_image_paint_selection_owner_bind(image, ma);
    return image;
  }
};

TEST_F(ImagePaintSelectionOwnerTest, two_images_of_one_material_share_the_mask)
{
  Image *first = add_bound_image("First");
  Image *second = add_bound_image("Second");
  ASSERT_TRUE(BKE_image_paint_selection_is_shared(first));
  ASSERT_TRUE(BKE_image_paint_selection_is_shared(second));
  EXPECT_EQ(BKE_image_paint_selection_owner_uid_get(first),
            BKE_image_paint_selection_owner_uid_get(second));

  ImBuf *mask = BKE_image_paint_selection_mask_get(first, 1001, 8, 8);
  ASSERT_NE(mask, nullptr);
  mask->float_data_for_write()[0] = 1.0f;

  /* Written through the first image, read through the second: one canvas-wide mask. */
  EXPECT_FLOAT_EQ(BKE_image_paint_selection_mask_sample(second, 1001, 0, 0), 1.0f);
}

TEST_F(ImagePaintSelectionOwnerTest, release_returns_images_to_their_local_state)
{
  Image *image = add_bound_image("Image");
  const uint32_t uid = BKE_image_paint_selection_owner_uid_get(image);
  ASSERT_NE(uid, 0u);

  BKE_image_paint_selection_owner_release(uid);

  EXPECT_FALSE(BKE_image_paint_selection_is_shared(image));
  EXPECT_EQ(BKE_image_paint_selection_owner_uid_get(image), 0u);
  /* The owner is gone, so an undo re-bind by its old UID cannot resurrect it. */
  EXPECT_FALSE(BKE_image_paint_selection_owner_bind_by_uid(image, uid));
}

TEST_F(ImagePaintSelectionOwnerTest, state_discard_keeps_the_shared_mask)
{
  Image *discarded = add_bound_image("Discarded");
  Image *survivor = add_bound_image("Survivor");
  ImBuf *mask = BKE_image_paint_selection_mask_get(discarded, 1001, 8, 8);
  ASSERT_NE(mask, nullptr);
  mask->float_data_for_write()[0] = 1.0f;

  BKE_image_paint_selection_state_discard(discarded);

  EXPECT_FALSE(BKE_image_paint_selection_is_shared(discarded));
  EXPECT_TRUE(BKE_image_paint_selection_is_shared(survivor));
  /* One map going away is not a deselect: the material's mask survives. */
  EXPECT_FLOAT_EQ(BKE_image_paint_selection_mask_sample(survivor, 1001, 0, 0), 1.0f);
}

TEST_F(ImagePaintSelectionOwnerTest, bind_to_a_plain_material_unbinds)
{
  Image *image = add_bound_image("Image");
  ASSERT_TRUE(BKE_image_paint_selection_is_shared(image));

  Material *plain = BKE_material_add(bmain, "Plain");
  BKE_image_paint_selection_owner_bind(image, plain);

  EXPECT_FALSE(BKE_image_paint_selection_is_shared(image));
  EXPECT_EQ(BKE_image_paint_selection_owner_uid_get(image), 0u);
}

TEST_F(ImagePaintSelectionOwnerTest, shared_mask_resamples_to_the_requested_size)
{
  Image *image = add_bound_image("Image");
  ImBuf *mask = BKE_image_paint_selection_mask_get(image, 1001, 8, 8);
  ASSERT_NE(mask, nullptr);
  /* The left half selected: the boundary must survive the nearest resample. */
  float *pixels = mask->float_data_for_write();
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(4)) {
      pixels[y * 8 + x] = 1.0f;
    }
  }

  ImBuf *resampled = BKE_image_paint_selection_mask_get(image, 1001, 4, 4);
  ASSERT_NE(resampled, nullptr);
  EXPECT_EQ(resampled->x, 4);
  EXPECT_EQ(resampled->y, 4);

  EXPECT_FLOAT_EQ(BKE_image_paint_selection_mask_sample(image, 1001, 0, 0), 1.0f);
  EXPECT_FLOAT_EQ(BKE_image_paint_selection_mask_sample(image, 1001, 1, 0), 1.0f);
  EXPECT_FLOAT_EQ(BKE_image_paint_selection_mask_sample(image, 1001, 2, 0), 0.0f);
  EXPECT_FLOAT_EQ(BKE_image_paint_selection_mask_sample(image, 1001, 3, 3), 0.0f);
}

}  // namespace blender::bke::tests
