/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_layers_test_util.hh"

#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_brush_layer_source.hh"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender::bke::tests {

class BrushLayerSourceTest : public PaintLayersTestBase {};

TEST_F(BrushLayerSourceTest, invalid_sources_hash_to_zero)
{
  EXPECT_EQ(
      BKE_brush_layer_source_content_hash(*ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW, {}),
      0);
  EXPECT_EQ(
      BKE_brush_layer_source_content_hash(*ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL, {}),
      0);

  Material *plain = BKE_material_add(bmain, "Plain");
  EXPECT_EQ(BKE_brush_layer_source_content_hash(
                *plain, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL, {}),
            0);

  const bUUID missing = BLI_uuid_generate_random();
  EXPECT_EQ(BKE_brush_layer_source_content_hash(
                *ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW, missing),
            0);
}

TEST_F(BrushLayerSourceTest, row_hash_tracks_value_edits)
{
  Image *image = add_solid_image("Base");
  MaterialPaintLayer *row = add_paint_layer("Row", image);
  const bUUID marker = row->marker;

  const uint64_t before =
      BKE_brush_layer_source_content_hash(*ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW, marker);
  ASSERT_NE(before, 0);

  const float value[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value));
  EXPECT_NE(
      BKE_brush_layer_source_content_hash(*ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW, marker),
      before);
}

TEST_F(BrushLayerSourceTest, full_hash_tracks_stack_structure)
{
  Image *image = add_solid_image("Base");
  add_paint_layer("Row", image);

  const uint64_t before = BKE_brush_layer_source_content_hash(
      *ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL, {});
  ASSERT_NE(before, 0);

  add_paint_layer("Row2", image);
  EXPECT_NE(BKE_brush_layer_source_content_hash(
                *ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_FULL, {}),
            before);
}

TEST_F(BrushLayerSourceTest, row_images_are_collected)
{
  Image *image = add_solid_image("Base");
  MaterialPaintLayer *row = add_paint_layer("Row", image);

  Vector<Image *> images;
  BKE_brush_layer_source_images_collect(
      *ma, BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW, row->marker, images);
  EXPECT_TRUE(images.contains(image));
}

}  // namespace blender::bke::tests
