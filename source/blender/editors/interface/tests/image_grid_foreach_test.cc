/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_asset_types.h"
#include "DNA_image_types.h"
#include "DNA_userdef_types.h"

#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "ED_image_grid.hh"

#include "testing/testing.h"

namespace blender::ed::image_grid::tests {

TEST(image_grid_assignable, file_and_sequence_pass_viewer_generated_and_render_fail)
{
  Image file = {};
  file.source = IMA_SRC_FILE;
  file.type = IMA_TYPE_IMAGE;
  EXPECT_TRUE(image_grid_is_assignable_texture(file));

  Image sequence = {};
  sequence.source = IMA_SRC_SEQUENCE;
  sequence.type = IMA_TYPE_IMAGE;
  EXPECT_TRUE(image_grid_is_assignable_texture(sequence));

  Image generated = {};
  generated.source = IMA_SRC_GENERATED;
  generated.type = IMA_TYPE_IMAGE;
  EXPECT_FALSE(image_grid_is_assignable_texture(generated));

  Image viewer = {};
  viewer.source = IMA_SRC_VIEWER;
  viewer.type = IMA_TYPE_IMAGE;
  EXPECT_FALSE(image_grid_is_assignable_texture(viewer));

  Image render_result = {};
  render_result.source = IMA_SRC_FILE;
  render_result.type = IMA_TYPE_R_RESULT;
  EXPECT_FALSE(image_grid_is_assignable_texture(render_result));

  Image composite = {};
  composite.source = IMA_SRC_FILE;
  composite.type = IMA_TYPE_COMPOSITE;
  EXPECT_FALSE(image_grid_is_assignable_texture(composite));
}

TEST(image_grid_assignable, internal_maps_hidden_unless_show_internal_maps)
{
  /* A stack-owned map keeps its GENERATED source only until the stack tiles it or the user packs
   * or saves it; both of those forms must stay hidden while the preference is off. */
  Image tiled_internal = {};
  tiled_internal.source = IMA_SRC_TILED;
  tiled_internal.type = IMA_TYPE_IMAGE;
  tiled_internal.flag = IMA_PAINT_LAYER_INTERNAL_MAP;
  EXPECT_FALSE(image_grid_is_assignable_texture(tiled_internal));

  Image packed_internal = {};
  packed_internal.source = IMA_SRC_FILE;
  packed_internal.type = IMA_TYPE_IMAGE;
  packed_internal.flag = IMA_PAINT_LAYER_INTERNAL_MAP;
  EXPECT_FALSE(image_grid_is_assignable_texture(packed_internal));

  /* Show Internal Paint Maps reveals the stack-owned maps, like every image browser does. */
  const char saved_show_internal = U.experimental.show_paint_layer_internal_maps;
  U.experimental.show_paint_layer_internal_maps = 1;
  EXPECT_TRUE(image_grid_is_assignable_texture(tiled_internal));
  EXPECT_TRUE(image_grid_is_assignable_texture(packed_internal));
  U.experimental.show_paint_layer_internal_maps = saved_show_internal;
}

class ImageGridForeachTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    bmain = nullptr;
  }

  Image *add_image(const char *name, const eImageSource source, const eImageType type)
  {
    Image *image = BKE_id_new<Image>(bmain, name);
    image->source = source;
    image->type = type;
    return image;
  }
};

TEST_F(ImageGridForeachTest, local_library_yields_assignable_images_in_main_order)
{
  Image *file_a = add_image("FileA", IMA_SRC_FILE, IMA_TYPE_IMAGE);
  add_image("Generated", IMA_SRC_GENERATED, IMA_TYPE_IMAGE);
  Image *file_b = add_image("FileB", IMA_SRC_FILE, IMA_TYPE_IMAGE);
  add_image("Viewer", IMA_SRC_VIEWER, IMA_TYPE_IMAGE);
  add_image("Composite", IMA_SRC_FILE, IMA_TYPE_COMPOSITE);
  Image *file_c = add_image("FileC", IMA_SRC_FILE, IMA_TYPE_IMAGE);

  AssetLibraryReference lib_ref{};
  lib_ref.type = ASSET_LIBRARY_LOCAL;
  const Set<std::string> no_catalog_filter;

  Vector<Image *> yielded;
  Vector<int> indices;
  const int count = image_grid_foreach_filtered_item(
      *bmain, lib_ref, no_catalog_filter, [&](const ImageGridFilteredItem &item, const int index) {
        EXPECT_EQ(item.asset, nullptr);
        EXPECT_NE(item.image, nullptr);
        yielded.append(item.image);
        indices.append(index);
        return true;
      });

  ASSERT_EQ(count, 3);
  ASSERT_EQ(yielded.size(), 3);
  EXPECT_EQ(yielded[0], file_a);
  EXPECT_EQ(yielded[1], file_b);
  EXPECT_EQ(yielded[2], file_c);
  EXPECT_EQ(indices[0], 0);
  EXPECT_EQ(indices[1], 1);
  EXPECT_EQ(indices[2], 2);
}

TEST_F(ImageGridForeachTest, early_stop_still_returns_full_filtered_count)
{
  add_image("FileA", IMA_SRC_FILE, IMA_TYPE_IMAGE);
  add_image("FileB", IMA_SRC_FILE, IMA_TYPE_IMAGE);
  add_image("FileC", IMA_SRC_FILE, IMA_TYPE_IMAGE);

  AssetLibraryReference lib_ref{};
  lib_ref.type = ASSET_LIBRARY_LOCAL;
  const Set<std::string> no_catalog_filter;

  int visited = 0;
  const int count = image_grid_foreach_filtered_item(
      *bmain, lib_ref, no_catalog_filter, [&](const ImageGridFilteredItem & /*item*/, const int) {
        visited++;
        return false;
      });

  EXPECT_EQ(visited, 1);
  EXPECT_EQ(count, 3);
}

TEST_F(ImageGridForeachTest, non_local_library_skips_main_images_when_asset_list_empty)
{
  add_image("FileA", IMA_SRC_FILE, IMA_TYPE_IMAGE);

  AssetLibraryReference lib_ref{};
  lib_ref.type = ASSET_LIBRARY_ESSENTIALS;
  const Set<std::string> no_catalog_filter;

  int visited = 0;
  const int count = image_grid_foreach_filtered_item(
      *bmain, lib_ref, no_catalog_filter, [&](const ImageGridFilteredItem & /*item*/, const int) {
        visited++;
        return true;
      });

  EXPECT_EQ(visited, 0);
  EXPECT_EQ(count, 0);
}

}  // namespace blender::ed::image_grid::tests
