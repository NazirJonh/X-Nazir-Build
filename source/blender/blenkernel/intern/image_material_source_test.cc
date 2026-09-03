/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

namespace blender::bke::tests {

class ImageMaterialSourceTest : public BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }
  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  Image *image_add(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(bmain,
                                   4,
                                   4,
                                   name,
                                   32,
                                   /*floatbuf=*/true,
                                   IMA_GENTYPE_BLANK,
                                   color,
                                   /*stereo3d=*/false,
                                   /*is_data=*/false,
                                   /*tiled=*/false);
  }
};

TEST_F(ImageMaterialSourceTest, get_returns_false_when_unlinked)
{
  Image *image = this->image_add("img");
  ImageMaterialSource source;
  EXPECT_FALSE(BKE_image_material_source_get(*image, source));
}

TEST_F(ImageMaterialSourceTest, set_then_get_roundtrips)
{
  Image *image = this->image_add("img");
  Material *material = BKE_material_add(bmain, "mat");

  ImageMaterialSource in;
  in.material = material;
  in.channel = 2;
  in.node_tree_hash = 0x0123456789abcdefULL;
  in.bake_size = 2048;
  BKE_image_material_source_set(*image, in);

  ImageMaterialSource out;
  ASSERT_TRUE(BKE_image_material_source_get(*image, out));
  EXPECT_EQ(out.material, material);
  EXPECT_EQ(out.channel, 2);
  EXPECT_EQ(out.node_tree_hash, 0x0123456789abcdefULL);
  EXPECT_EQ(out.bake_size, 2048);
}

TEST_F(ImageMaterialSourceTest, clear_removes_link)
{
  Image *image = this->image_add("img");
  Material *material = BKE_material_add(bmain, "mat");
  ImageMaterialSource in{material, 0, 1ULL, 1024};
  BKE_image_material_source_set(*image, in);
  BKE_image_material_source_clear(*image);
  ImageMaterialSource out;
  EXPECT_FALSE(BKE_image_material_source_get(*image, out));
}

TEST_F(ImageMaterialSourceTest, copy_strips_link)
{
  Image *image = this->image_add("img");
  Material *material = BKE_material_add(bmain, "mat");
  ImageMaterialSource in{material, 1, 7ULL, 1024};
  BKE_image_material_source_set(*image, in);

  Image *copy = id_cast<Image *>(BKE_id_copy(bmain, &image->id));

  ImageMaterialSource out;
  EXPECT_FALSE(BKE_image_material_source_get(*copy, out));
  /* The original is untouched. */
  EXPECT_TRUE(BKE_image_material_source_get(*image, out));
}

}  // namespace blender::bke::tests
