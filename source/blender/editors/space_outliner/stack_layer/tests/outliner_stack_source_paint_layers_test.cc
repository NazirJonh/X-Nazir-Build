/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_uuid.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_space_types.h"

#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {
namespace tests {

/**
 * The paint-layers source: rows come from the DNA description, and the Add vocabulary and the
 * columns are what the source declares. Driving the operators needs a full Outliner, so the store
 * itself is exercised through the description helpers the source is built on.
 */
class OutlinerStackPaintLayersSourceTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *material = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    material = BKE_material_add(bmain, "M");
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  const StackSource &source()
  {
    const StackSource *source = stack_source_get(SO_STACK_SRC_PAINT_MATERIAL);
    BLI_assert(source != nullptr);
    return *source;
  }

  MaterialPaintLayer *add_layer(const char *name)
  {
    PaintLayerAddParams params;
    params.name = name;
    return BKE_paint_layers_add_with_policy(*material, params);
  }
};

TEST_F(OutlinerStackPaintLayersSourceTest, source_is_registered_as_paint_layers)
{
  EXPECT_EQ(source().type(), SO_STACK_SRC_PAINT_MATERIAL);
}

TEST_F(OutlinerStackPaintLayersSourceTest, add_kinds_offers_only_paint)
{
  Vector<StackAddKindInfo> kinds;
  const StackEditor *editor = source().editor();
  ASSERT_NE(editor, nullptr);
  editor->add_kinds(kinds);
  ASSERT_EQ(kinds.size(), 1u);
  EXPECT_EQ(kinds[0].identifier, "PAINT");
}

TEST_F(OutlinerStackPaintLayersSourceTest, value_column_edits_the_row_opacity)
{
  add_layer("A");
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_TRUE(rows[0].value_ptr.has_value());
  ASSERT_NE(rows[0].value_prop, nullptr);
  EXPECT_STREQ(rows[0].value_prop, "opacity");
  /* A row left at its default reads as inherited rather than deliberately set. */
  EXPECT_TRUE(rows[0].value_inherited);

  BKE_paint_layers_set_opacity(*material, BKE_paint_layers_find(*material, rows[0].stable_id), 0.5f);
  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows[0].value_inherited);
}

TEST_F(OutlinerStackPaintLayersSourceTest, rows_follow_the_stack_order)
{
  add_layer("Bottom");
  add_layer("Top");
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].ordinal, 0);
  EXPECT_EQ(rows[0].name, "Bottom");
  EXPECT_EQ(rows[1].ordinal, 1);
  EXPECT_EQ(rows[1].name, "Top");
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[0].parent_ordinal, -1);
}

TEST_F(OutlinerStackPaintLayersSourceTest, ordinal_and_row_resolve_each_way)
{
  MaterialPaintLayer *bottom = add_layer("Bottom");
  MaterialPaintLayer *top = add_layer("Top");
  EXPECT_EQ(paint_layers_ordinal_of(*material, bottom), 0);
  EXPECT_EQ(paint_layers_ordinal_of(*material, top), 1);
  EXPECT_EQ(paint_description_row_for_ordinal(*material, 0), bottom);
  EXPECT_EQ(paint_description_row_for_ordinal(*material, 1), top);
  EXPECT_EQ(paint_description_row_for_ordinal(*material, 2), nullptr);
}

TEST_F(OutlinerStackPaintLayersSourceTest, disabled_row_reports_disabled)
{
  MaterialPaintLayer *layer = add_layer("A");
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows[0].enabled);
  BKE_paint_layers_set_enabled(*material, layer, false);
  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows[0].enabled);
}

TEST_F(OutlinerStackPaintLayersSourceTest, columns_show_value_and_hide_mode)
{
  const StackColumnLayout layout = source().column_layout();
  EXPECT_GT(layout.value_width, 0.0f);
  EXPECT_LE(layout.mode_width, 0.0f);
}

TEST_F(OutlinerStackPaintLayersSourceTest, rename_writes_the_row_name)
{
  MaterialPaintLayer *layer = add_layer("A");
  const bUUID marker = layer->marker;
  ASSERT_TRUE(paint_layers_edit_rename(*material, 0, "Renamed"));
  const MaterialPaintLayer *renamed = BKE_paint_layers_find(*material, marker);
  ASSERT_NE(renamed, nullptr);
  EXPECT_STREQ(renamed->name, "Renamed");
}

TEST_F(OutlinerStackPaintLayersSourceTest, duplicate_copies_the_row_and_its_map)
{
  MaterialPaintLayer *layer = add_layer("A");
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 8, 8, "Map", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *material, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*material, layer, 0.25f));

  const int copy_ordinal = paint_layers_edit_duplicate(*bmain, *material, 0);
  ASSERT_EQ(copy_ordinal, 1);
  MaterialPaintLayer *copy = paint_description_row_for_ordinal(*material, 1);
  ASSERT_NE(copy, nullptr);
  /* A fresh marker and the row's own state; the name is copied verbatim. */
  EXPECT_FALSE(BLI_uuid_equal(copy->marker, layer->marker));
  EXPECT_STREQ(copy->name, "A");
  EXPECT_FLOAT_EQ(copy->opacity, 0.25f);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_FALSE(BLI_uuid_equal(rows[0].stable_id, rows[1].stable_id));

  /* The copy gets its own map, never the shared one. */
  MaterialPaintLayerChannel *copy_channel = nullptr;
  for (int c = 0; c < copy->channels_num; c++) {
    if (copy->channels[c].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      copy_channel = &copy->channels[c];
    }
  }
  ASSERT_NE(copy_channel, nullptr);
  ASSERT_NE(copy_channel->image, nullptr);
  EXPECT_NE(copy_channel->image, image);
}

}  // namespace tests
}  // namespace blender::ed::outliner
