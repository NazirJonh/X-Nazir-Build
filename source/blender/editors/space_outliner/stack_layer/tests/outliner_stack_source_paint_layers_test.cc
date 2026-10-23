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

#include "UI_resources.hh"

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

TEST_F(OutlinerStackPaintLayersSourceTest, add_kinds_offers_the_rows_and_both_corrections)
{
  Vector<StackAddKindInfo> kinds;
  const StackEditor *editor = source().editor();
  ASSERT_NE(editor, nullptr);
  editor->add_kinds(kinds);
  /* The order #StackEditor::row_add dispatches by index. */
  ASSERT_EQ(kinds.size(), 25u);
  EXPECT_EQ(kinds[0].identifier, "PAINT");
  EXPECT_FALSE(kinds[0].takes_color);
  EXPECT_EQ(kinds[1].identifier, "FILL");
  EXPECT_TRUE(kinds[1].takes_color);
  EXPECT_EQ(kinds[2].identifier, "FOLDER");
  EXPECT_EQ(kinds[3].identifier, "CORRECTION");
  EXPECT_FALSE(kinds[3].takes_color);
  EXPECT_EQ(kinds[4].identifier, "CORRECTION_FILL");
  EXPECT_TRUE(kinds[4].takes_color);
  EXPECT_EQ(kinds[5].identifier, "MASK_CORRECTION");
  EXPECT_FALSE(kinds[5].takes_color);
  EXPECT_EQ(kinds[6].identifier, "MASK_CORRECTION_FILL");
  EXPECT_TRUE(kinds[6].takes_color);
  EXPECT_EQ(kinds[7].identifier, "FILTER_INVERT");
  EXPECT_FALSE(kinds[7].takes_color);
  EXPECT_TRUE(kinds[7].is_effect);
  EXPECT_EQ(kinds[8].identifier, "FILTER_LEVELS");
  EXPECT_TRUE(kinds[8].is_effect);
  EXPECT_EQ(kinds[9].identifier, "FILTER_HSL");
  EXPECT_TRUE(kinds[9].is_effect);
  /* The same filters again as rows of the stack, in the same order. */
  EXPECT_EQ(kinds[10].identifier, "ADJUSTMENT_INVERT");
  EXPECT_FALSE(kinds[10].is_effect);
  EXPECT_EQ(kinds[11].identifier, "ADJUSTMENT_LEVELS");
  EXPECT_EQ(kinds[12].identifier, "ADJUSTMENT_HSL");
  EXPECT_FALSE(kinds[12].is_effect);
  /* And as corrections of the mask, in no channel. */
  EXPECT_EQ(kinds[13].identifier, "MASK_FILTER_INVERT");
  EXPECT_TRUE(kinds[13].is_effect);
  EXPECT_EQ(kinds[15].identifier, "MASK_FILTER_HSL");
  /* The map-driven filters follow, only as corrections of the mask. */
  EXPECT_EQ(kinds[16].identifier, "MASK_FILTER_COLOR_ID");
  EXPECT_TRUE(kinds[16].is_effect);
  EXPECT_EQ(kinds[17].identifier, "MASK_FILTER_UV_ISLAND");
  EXPECT_TRUE(kinds[17].is_effect);
  /* Last, so no index above moves: the kind a dropped material is made into. */
  EXPECT_EQ(kinds[18].identifier, "MATERIAL");
  EXPECT_EQ(kinds[18].source_id_type, ID_MA);
  EXPECT_EQ(kinds[19].identifier, "CORRECTION_MATERIAL");
  EXPECT_EQ(kinds[19].source_id_type, ID_MA);
  EXPECT_TRUE(kinds[19].is_effect) << "listed with the other corrections";
  EXPECT_EQ(kinds[20].identifier, "MASK_CORRECTION_MATERIAL");
  EXPECT_EQ(kinds[20].source_id_type, ID_MA);
  EXPECT_TRUE(kinds[20].is_effect);
  /* The marker and the three places its reader can land are last, so no index above moves. */
  EXPECT_EQ(kinds[21].identifier, "ANCHOR_POINT");
  EXPECT_TRUE(kinds[21].is_effect);
  EXPECT_EQ(kinds[22].identifier, "ANCHOR_POINT_LAYER");
  EXPECT_FALSE(kinds[22].is_effect);
  EXPECT_EQ(kinds[23].identifier, "ANCHOR_POINT_CORRECTION");
  EXPECT_TRUE(kinds[23].is_effect);
  EXPECT_EQ(kinds[24].identifier, "ANCHOR_POINT_MASK_CORRECTION");
  EXPECT_TRUE(kinds[24].is_effect);
  EXPECT_FALSE(kinds[18].is_effect);
  EXPECT_FALSE(kinds[15].takes_color);
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_corrections_are_rows_under_the_mask_section)
{
  MaterialPaintLayer *layer = add_layer("A");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskCorrectionFill;
  params.anchor = layer;
  const float color[4] = {0.4f, 0.4f, 0.4f, 1.0f};
  params.fill_color = color;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(fill, nullptr);
  params.kind = PaintLayerAddKind::MaskCorrection;
  params.fill_color = nullptr;
  MaterialPaintLayer *paint = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(paint, nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  /* A row's own comes after what hangs off it, the corrections in the order they were added. */
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(paint_description_row_for_ordinal(*material, 1), fill);
  EXPECT_EQ(paint_description_row_for_ordinal(*material, 2), paint);
  EXPECT_EQ(paint_layers_ordinal_of(*material, paint), 2);

  const StackRow &flat = rows[0];
  EXPECT_EQ(flat.name, "Mask Fill 1");
  EXPECT_EQ(flat.parent_section_id, "MASK");
  EXPECT_EQ(flat.parent_ordinal, 0);
  EXPECT_TRUE(flat.compact);
  ASSERT_EQ(flat.preview_slots.size(), 1u);
  EXPECT_TRUE(flat.preview_slots[0].is_color_swatch);
  EXPECT_EQ(flat.preview_slots[0].section_id, "MASK");
  EXPECT_FLOAT_EQ(flat.preview_slots[0].color[0], 0.4f);
  ASSERT_EQ(flat.content_sections.size(), 1u);
  EXPECT_EQ(flat.content_sections[0].identifier, "MASK");

  const StackRow &painted = rows[1];
  EXPECT_EQ(painted.name, "Mask Paint 1");
  ASSERT_EQ(painted.preview_slots.size(), 1u);
  EXPECT_FALSE(painted.preview_slots[0].is_color_swatch);
  EXPECT_EQ(painted.icon, int(ICON_MOD_MASK));
}

TEST_F(OutlinerStackPaintLayersSourceTest, an_issue_is_a_warning_slot_on_the_row_it_is_about)
{
  MaterialPaintLayer *layer = add_layer("A");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::CorrectionFill;
  params.anchor = layer;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(fill, nullptr);
  params.kind = PaintLayerAddKind::Correction;
  ASSERT_NE(BKE_paint_layers_add_with_policy(*material, params), nullptr);

  /* The layer carries Normal, the Fill was never given it: nothing to say. Once it is, the Fill
   * correction's row, and only that one, says why it does nothing there. */
  const auto count_warnings = [&](const char *expected_name) {
    Vector<StackRow> rows;
    paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
    EXPECT_EQ(rows.size(), 3u);
    int warnings = 0;
    for (const StackRow &row : rows) {
      for (const StackRowPreview &slot : row.preview_slots) {
        if (slot.icon == int(ICON_ERROR)) {
          warnings++;
          EXPECT_EQ(row.name, expected_name);
          EXPECT_FALSE(slot.label.empty());
        }
      }
    }
    return warnings;
  };
  EXPECT_EQ(count_warnings(""), 0);
  ASSERT_NE(BKE_paint_layers_channel_add(*material, fill, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  EXPECT_EQ(count_warnings("Correction Fill 1"), 1);
  ASSERT_TRUE(BKE_paint_layers_channel_remove(*material, fill, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_EQ(count_warnings(""), 0);
}

TEST_F(OutlinerStackPaintLayersSourceTest, a_folder_takes_a_correction_row)
{
  MaterialPaintLayer *layer = add_layer("A");
  MaterialPaintLayer *members[1] = {layer};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *material, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Correction;
  params.anchor = folder;
  MaterialPaintLayer *effect = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(effect, nullptr);

  /* The folder's own correction is a row of the folder, numbered right after it. */
  EXPECT_EQ(paint_layers_ordinal_of(*material, folder), 0);
  EXPECT_EQ(paint_layers_ordinal_of(*material, effect), 1);
  EXPECT_EQ(paint_description_row_for_ordinal(*material, 1), effect);
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  bool found = false;
  for (const StackRow &row : rows) {
    if (row.ordinal == 1) {
      found = true;
      EXPECT_EQ(row.parent_ordinal, 0);
      EXPECT_EQ(row.parent_section_id, "CHANNELS");
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(OutlinerStackPaintLayersSourceTest, state_hash_follows_a_mask_correction)
{
  MaterialPaintLayer *layer = add_layer("A");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskCorrectionFill;
  params.anchor = layer;
  MaterialPaintLayer *item = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(item, nullptr);
  StackReadContext ctx;
  const uint64_t before = source().state_hash(ctx, material->id);

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*material, item, 0.5f));
  const uint64_t with_opacity = source().state_hash(ctx, material->id);
  EXPECT_NE(with_opacity, before);

  /* The swatch shows the grey, so an edit of it has to reach the rows. */
  const float dark[4] = {0.1f, 0.1f, 0.1f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *material, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, dark));
  EXPECT_NE(source().state_hash(ctx, material->id), with_opacity);
}

TEST_F(OutlinerStackPaintLayersSourceTest, a_fill_correction_row_is_its_colour_swatch)
{
  MaterialPaintLayer *layer = add_layer("A");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::CorrectionFill;
  params.anchor = layer;
  const float color[4] = {0.9f, 0.4f, 0.1f, 1.0f};
  params.fill_color = color;
  MaterialPaintLayer *effect = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(effect, nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2u);
  /* The layer's own row follows what hangs off it. */
  const StackRow &row = rows[0];
  EXPECT_EQ(row.name, "Correction Fill 1");
  EXPECT_EQ(row.icon, int(ICON_GP_DRAW_FILL));
  EXPECT_TRUE(row.compact);
  ASSERT_EQ(row.preview_slots.size(), 1u);
  const StackRowPreview &slot = row.preview_slots[0];
  EXPECT_TRUE(slot.is_color_swatch);
  EXPECT_EQ(slot.id_uid, 0u);
  EXPECT_FLOAT_EQ(slot.color[0], 0.9f);
  EXPECT_FLOAT_EQ(slot.color[1], 0.4f);

  /* A Paint correction keeps the map slot. */
  params.kind = PaintLayerAddKind::Correction;
  params.fill_color = nullptr;
  ASSERT_NE(BKE_paint_layers_add_with_policy(*material, params), nullptr);
  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 3u);
  ASSERT_EQ(rows[1].preview_slots.size(), 1u);
  EXPECT_FALSE(rows[1].preview_slots[0].is_color_swatch);
}

TEST_F(OutlinerStackPaintLayersSourceTest, value_column_edits_the_selected_channels_opacity)
{
  MaterialPaintLayer *layer = add_layer("A");
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_TRUE(rows[0].value_ptr.has_value());
  ASSERT_NE(rows[0].value_prop, nullptr);
  EXPECT_STREQ(rows[0].value_prop, "opacity");
  /* A row left at its default reads as inherited rather than deliberately set. */
  EXPECT_TRUE(rows[0].value_inherited);

  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(
      *material, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.5f));
  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows[0].value_inherited);
}

TEST_F(OutlinerStackPaintLayersSourceTest, mode_column_edits_the_selected_channels_blend)
{
  MaterialPaintLayer *layer = add_layer("A");
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_TRUE(rows[0].mode_ptr.has_value());
  ASSERT_NE(rows[0].mode_prop, nullptr);
  EXPECT_STREQ(rows[0].mode_prop, "blend_type");
  EXPECT_EQ(static_cast<const MaterialPaintLayerChannelSettings *>(rows[0].mode_ptr->data),
            &layer->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);
  /* Inherit is the row's own blend, so the control dims until the user picks another mode. */
  EXPECT_TRUE(rows[0].mode_inherited);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *material, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_ADD));
  /* The row's own blend is the base the other channels inherit, not what the column wrote. */
  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MIX);
  EXPECT_EQ(layer->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].blend,
            MA_PAINT_LAYER_BLEND_ADD);

  /* The header's cursor moves the column to the other channel's own pair. */
  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(static_cast<const MaterialPaintLayerChannelSettings *>(rows[0].mode_ptr->data),
            &layer->channel_settings[PAINT_MATERIAL_CHANNEL_ROUGHNESS]);
  EXPECT_TRUE(rows[0].mode_inherited);

  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows[0].mode_inherited);
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

TEST_F(OutlinerStackPaintLayersSourceTest, columns_show_value_and_mode)
{
  const StackColumnLayout layout = source().column_layout();
  EXPECT_GT(layout.value_width, 0.0f);
  EXPECT_GT(layout.mode_width, 0.0f);
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

TEST_F(OutlinerStackPaintLayersSourceTest, color_tag_round_trips_to_a_folder_row)
{
  MaterialPaintLayer *layer = add_layer("A");
  MaterialPaintLayer *members[1] = {layer};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *material, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  /* A fresh folder carries no tag, which is zero rather than a "none" color: the field is carved
   * out of padding and old files read it as zero. */
  EXPECT_EQ(folder->color_tag, MA_PAINT_LAYER_COLOR_NONE);
  const int folder_ordinal = paint_layers_ordinal_of(*material, folder);
  ASSERT_GE(folder_ordinal, 0);

  const auto row_tag = [&]() {
    Vector<StackRow> rows;
    paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
    for (const StackRow &row : rows) {
      if (row.ordinal == folder_ordinal) {
        return int(row.color_tag);
      }
    }
    return -1;
  };
  EXPECT_EQ(row_tag(), MA_PAINT_LAYER_COLOR_NONE);

  ASSERT_TRUE(paint_layers_edit_color_tag(*material, folder_ordinal, 3));
  EXPECT_EQ(folder->color_tag, 3);
  EXPECT_EQ(row_tag(), 3);

  /* Zero clears it. */
  ASSERT_TRUE(paint_layers_edit_color_tag(*material, folder_ordinal, MA_PAINT_LAYER_COLOR_NONE));
  EXPECT_EQ(folder->color_tag, MA_PAINT_LAYER_COLOR_NONE);
  EXPECT_EQ(row_tag(), MA_PAINT_LAYER_COLOR_NONE);

  /* A plain layer is not a group: a color is refused, clearing it is allowed. */
  const int layer_ordinal = paint_layers_ordinal_of(*material, layer);
  ASSERT_GE(layer_ordinal, 0);
  EXPECT_FALSE(paint_layers_edit_color_tag(*material, layer_ordinal, 2));
  EXPECT_EQ(layer->color_tag, MA_PAINT_LAYER_COLOR_NONE);
  EXPECT_TRUE(paint_layers_edit_color_tag(*material, layer_ordinal, MA_PAINT_LAYER_COLOR_NONE));

  /* An unknown ordinal, an out-of-range color and the old -1 sentinel are all refused. */
  EXPECT_FALSE(paint_layers_edit_color_tag(*material, 99, 2));
  EXPECT_FALSE(
      paint_layers_edit_color_tag(*material, folder_ordinal, MA_PAINT_LAYER_COLOR_TOT));
  EXPECT_FALSE(paint_layers_edit_color_tag(*material, folder_ordinal, -1));
}

TEST_F(OutlinerStackPaintLayersSourceTest, target_clear_drops_the_active_row)
{
  MaterialPaintLayer *layer = add_layer("A");
  BKE_paint_layers_active_set(*material, layer->marker);
  ASSERT_FALSE(BLI_uuid_is_nil(material->active_layer_marker));

  EXPECT_TRUE(paint_layers_edit_target_clear(*material));
  EXPECT_TRUE(BLI_uuid_is_nil(material->active_layer_marker));
  /* The row itself stays; only the aim goes. */
  EXPECT_NE(BKE_paint_layers_find(*material, layer->marker), nullptr);

  /* Nothing to clear the second time, so the operator cancels instead of pushing an undo step. */
  EXPECT_FALSE(paint_layers_edit_target_clear(*material));
}

TEST_F(OutlinerStackPaintLayersSourceTest, duplicate_keeps_a_folders_color_tag)
{
  MaterialPaintLayer *layer = add_layer("A");
  MaterialPaintLayer *members[1] = {layer};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *material, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(paint_layers_edit_color_tag(
      *material, paint_layers_ordinal_of(*material, folder), MA_PAINT_LAYER_COLOR_05));

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *material, folder);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->color_tag, MA_PAINT_LAYER_COLOR_05);
}

TEST_F(OutlinerStackPaintLayersSourceTest, state_hash_follows_the_color_tag)
{
  MaterialPaintLayer *layer = add_layer("A");
  MaterialPaintLayer *members[1] = {layer};
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *material, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  const int folder_ordinal = paint_layers_ordinal_of(*material, folder);
  ASSERT_GE(folder_ordinal, 0);

  StackReadContext ctx;
  const uint64_t before = source().state_hash(ctx, material->id);
  ASSERT_TRUE(paint_layers_edit_color_tag(*material, folder_ordinal, 4));
  /* The folder icon and the hierarchy line are drawn from the tag, so a change has to reach the
   * rows even though nothing evaluated reads it. */
  EXPECT_NE(source().state_hash(ctx, material->id), before);
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

TEST_F(OutlinerStackPaintLayersSourceTest, grouping_declares_the_mask_section)
{
  const StackEditor *editor = source().editor();
  ASSERT_NE(editor, nullptr);
  const StackGroupingEditor *grouping = editor->grouping();
  ASSERT_NE(grouping, nullptr);
  EXPECT_EQ(grouping->aux_section_id(), "MASK");
}

TEST_F(OutlinerStackPaintLayersSourceTest, row_without_a_mask_has_no_mask_slot_or_section)
{
  add_layer("A");
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  for (const StackRowPreview &slot : rows[0].preview_slots) {
    EXPECT_NE(slot.section_id, "MASK");
  }
  for (const StackContentSection &section : rows[0].content_sections) {
    EXPECT_NE(section.identifier, "MASK");
  }
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_adds_a_slot_a_section_and_its_switch)
{
  MaterialPaintLayer *layer = add_layer("A");
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*material, layer, 0.25f);
  ASSERT_NE(mask, nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  const StackRow &row = rows[0];

  const StackRowPreview *mask_slot = nullptr;
  for (const StackRowPreview &slot : row.preview_slots) {
    if (slot.section_id == "MASK") {
      mask_slot = &slot;
    }
  }
  ASSERT_NE(mask_slot, nullptr);
  /* The slot is the Mask icon: no swatch of the constant, no preview of a map. */
  EXPECT_FALSE(mask_slot->is_color_swatch);
  EXPECT_EQ(mask_slot->id_uid, 0u);
  EXPECT_EQ(mask_slot->icon, int(ICON_MOD_MASK));

  bool has_section = false;
  for (const StackContentSection &section : row.content_sections) {
    has_section |= section.identifier == "MASK";
  }
  EXPECT_TRUE(has_section);
  EXPECT_TRUE(row.aux_enabled);

  /* A switched-off mask is kept and listed; only the row's switch reads off. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*material, mask, false));
  rows.clear();
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows[0].aux_enabled);
  /* The icon says the same: the mask no longer shapes the coverage. */
  for (const StackRowPreview &slot : rows[0].preview_slots) {
    if (slot.section_id == "MASK") {
      EXPECT_EQ(slot.icon, int(ICON_CLIPUV_HLT));
    }
  }
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_slot_keeps_the_mask_icon_once_it_has_a_map)
{
  MaterialPaintLayer *layer = add_layer("A");
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*material, layer, 1.0f);
  ASSERT_NE(mask, nullptr);
  const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *map = BKE_image_add_generated(
      bmain, 8, 8, "MaskMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *material, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 1u);
  const StackRowPreview *mask_slot = nullptr;
  for (const StackRowPreview &slot : rows[0].preview_slots) {
    if (slot.section_id == "MASK") {
      mask_slot = &slot;
    }
  }
  ASSERT_NE(mask_slot, nullptr);
  /* A painted mask is still the Mask icon: the slot names what it opens, not what it holds. */
  EXPECT_FALSE(mask_slot->is_color_swatch);
  EXPECT_FALSE(mask_slot->is_blank);
  EXPECT_EQ(mask_slot->id_uid, 0u);
  EXPECT_EQ(mask_slot->icon, int(ICON_MOD_MASK));
}

TEST_F(OutlinerStackPaintLayersSourceTest, state_hash_follows_a_channel_blend)
{
  MaterialPaintLayer *layer = add_layer("A");
  StackReadContext ctx;
  const uint64_t before = source().state_hash(ctx, material->id);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *material, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_NE(source().state_hash(ctx, material->id), before);
}

TEST_F(OutlinerStackPaintLayersSourceTest, state_hash_follows_the_mask)
{
  MaterialPaintLayer *layer = add_layer("A");
  StackReadContext ctx;
  const uint64_t before = source().state_hash(ctx, material->id);

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*material, layer, 1.0f);
  ASSERT_NE(mask, nullptr);
  const uint64_t with_mask = source().state_hash(ctx, material->id);
  EXPECT_NE(with_mask, before);

  /* The slot's swatch shows the constant, so an edit of it has to reach the rows. */
  const float dark[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *material, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, dark));
  EXPECT_NE(source().state_hash(ctx, material->id), with_mask);
}

TEST_F(OutlinerStackPaintLayersSourceTest, state_hash_follows_a_fill_correction_colour)
{
  MaterialPaintLayer *layer = add_layer("A");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::CorrectionFill;
  params.anchor = layer;
  MaterialPaintLayer *effect = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(effect, nullptr);
  StackReadContext ctx;
  const uint64_t before = source().state_hash(ctx, material->id);

  /* The swatch shows the colour, so an edit of it has to reach the rows. */
  const float color[4] = {0.2f, 0.6f, 0.9f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*material, effect, color));
  EXPECT_NE(source().state_hash(ctx, material->id), before);
}

TEST_F(OutlinerStackPaintLayersSourceTest,
       a_material_dropped_on_the_stack_cannot_read_itself_or_cycle)
{
  const StackDropHandler *handler = source().drop_handler();
  ASSERT_NE(handler, nullptr);
  Material *other = BKE_material_add(bmain, "Other");
  StackReadContext ctx;
  ctx.bmain = bmain;
  StackDropTarget target;
  StackDropPayload payload;
  payload.id_type = ID_MA;
  const char *hint = nullptr;

  payload.id_uid = other->id.session_uid;
  EXPECT_TRUE(handler->can_accept(ctx, material->id, payload, target, &hint));
  EXPECT_EQ(hint, nullptr) << "an accepted drop has nothing to explain";

  payload.id_uid = material->id.session_uid;
  EXPECT_FALSE(handler->can_accept(ctx, material->id, payload, target, &hint));
  EXPECT_NE(hint, nullptr) << "a refusal is worth a word while dragging";

  /* The other material reads this one, so reading it back would be a cycle. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Material;
  params.source = material;
  ASSERT_NE(BKE_paint_layers_add_with_policy(*other, params), nullptr);
  payload.id_uid = other->id.session_uid;
  hint = nullptr;
  EXPECT_FALSE(handler->can_accept(ctx, material->id, payload, target, &hint));
  EXPECT_NE(hint, nullptr);

  /* Anything that is not a material is not this drop's business. */
  payload.id_type = ID_IM;
  payload.id_uid = 1;
  hint = nullptr;
  EXPECT_FALSE(handler->can_accept(ctx, material->id, payload, target, &hint));
  EXPECT_EQ(hint, nullptr);
}

TEST_F(OutlinerStackPaintLayersSourceTest, the_drop_catches_materials_and_words_where_they_land)
{
  const StackDropHandler *handler = source().drop_handler();
  ASSERT_NE(handler, nullptr);
  Vector<short> types;
  handler->drop_id_types(types);
  ASSERT_EQ(types.size(), 2);
  EXPECT_TRUE(types.contains(ID_IM));
  EXPECT_TRUE(types.contains(ID_MA));
  EXPECT_FALSE(handler->drop_supports_into(ID_MA)) << "a material is always a row of its own";
  EXPECT_TRUE(handler->drop_supports_into(ID_IM)) << "an image can be assigned to a row";

  StackDropTarget target;
  StackReadContext ctx;
  ctx.bmain = bmain;
  StackDropPayload payload;
  payload.id_type = ID_MA;
  EXPECT_EQ(handler->drop_tooltip(ctx, &material->id, payload, "Wood", "", target),
            "Add Wood as a Material layer on top");
  target.place = StackMovePlace::Above;
  EXPECT_EQ(handler->drop_tooltip(ctx, &material->id, payload, "Wood", "Base", target),
            "Add Wood as a Material layer above Base");
  target.place = StackMovePlace::Below;
  EXPECT_EQ(handler->drop_tooltip(ctx, &material->id, payload, "Wood", "Base", target),
            "Add Wood as a Material layer below Base");
}

TEST_F(OutlinerStackPaintLayersSourceTest,
       an_image_is_assigned_to_a_fill_row_and_never_replaces_a_paint_map)
{
  const StackDropHandler *handler = source().drop_handler();
  ASSERT_NE(handler, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 4, 4, "Map", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(image, nullptr);

  MaterialPaintLayer *paint = add_layer("Paint");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(fill, nullptr);
  params.kind = PaintLayerAddKind::Material;
  Material *other = BKE_material_add(bmain, "Other");
  params.source = other;
  MaterialPaintLayer *material_row = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(material_row, nullptr);

  StackReadContext ctx;
  ctx.bmain = bmain;
  StackDropPayload payload;
  payload.id_type = ID_IM;
  payload.id_uid = image->id.session_uid;
  StackDropTarget target;
  const char *hint = nullptr;

  /* Beside a row, or over empty space, the image becomes a new Fill layer. */
  EXPECT_TRUE(handler->can_accept(ctx, material->id, payload, target, &hint));
  target.anchor.row_id = paint->marker;
  target.place = StackMovePlace::Below;
  EXPECT_TRUE(handler->can_accept(ctx, material->id, payload, target, &hint));

  /* Onto a row, it is that row's map: a Fill row takes it. */
  target.place = StackMovePlace::Into;
  target.anchor.row_id = fill->marker;
  EXPECT_TRUE(handler->can_accept(ctx, material->id, payload, target, &hint));

  /* A Paint row owns the maps its brush writes into, and a Material row shows its source. */
  target.anchor.row_id = paint->marker;
  hint = nullptr;
  EXPECT_FALSE(handler->can_accept(ctx, material->id, payload, target, &hint));
  EXPECT_NE(hint, nullptr);
  target.anchor.row_id = material_row->marker;
  hint = nullptr;
  EXPECT_FALSE(handler->can_accept(ctx, material->id, payload, target, &hint));
  EXPECT_NE(hint, nullptr);

  StackDropPayload tooltip_payload;
  tooltip_payload.id_type = ID_IM;
  tooltip_payload.id_uid = image->id.session_uid;
  /* The tooltip is the one of the action that would take the drop, so it is asked at a target that
   * accepts it: the row it was refused on has only the disabled hint above. */
  target.anchor.row_id = fill->marker;
  target.place = StackMovePlace::Into;
  EXPECT_EQ(handler->drop_tooltip(ctx, &material->id, tooltip_payload, "Map", "Fill 1", target),
            "Assign Map to Fill 1");
  target.anchor = StackItemIdentity{};
  target.place = StackMovePlace::Above;
  EXPECT_EQ(handler->drop_tooltip(ctx, &material->id, tooltip_payload, "Map", "", target),
            "Add Map as a new Fill layer on top");
  target.anchor.row_id = fill->marker;
  target.place = StackMovePlace::Below;
  EXPECT_EQ(handler->drop_tooltip(ctx, &material->id, tooltip_payload, "Map", "Fill 1", target),
            "Add Map as a new Fill layer below Fill 1");
}

TEST_F(OutlinerStackPaintLayersSourceTest, a_material_correction_is_a_row_with_no_map_to_show)
{
  MaterialPaintLayer *layer = add_layer("A");
  Material *other = BKE_material_add(bmain, "Other");
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add_material(
      *material, layer, "Wood", other);
  ASSERT_NE(effect, nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2u);
  const StackRow &row = rows[0].name == "Wood" ? rows[0] : rows[1];
  EXPECT_EQ(row.name, "Wood");
  EXPECT_EQ(row.icon, ICON_MATERIAL);
  EXPECT_TRUE(row.compact) << "a correction reads as a setting of its layer";
  EXPECT_TRUE(row.preview_slots.is_empty()) << "it shows what its source shows";
  EXPECT_TRUE(row.content_sections.is_empty()) << "and owns no map to switch the brush to";
  EXPECT_STREQ(row.value_prop, "opacity");
  EXPECT_STREQ(row.mode_prop, "blend_type");
}

TEST_F(OutlinerStackPaintLayersSourceTest, a_dropped_material_may_be_added_as_a_correction)
{
  Vector<StackAddKindInfo> kinds;
  source().editor()->add_kinds(kinds);
  int material_kind = -1;
  int correction_kind = -1;
  for (const int i : kinds.index_range()) {
    if (kinds[i].identifier == "MATERIAL") {
      material_kind = i;
    }
    if (kinds[i].identifier == "CORRECTION_MATERIAL") {
      correction_kind = i;
    }
  }
  ASSERT_GE(material_kind, 0);
  ASSERT_EQ(correction_kind, material_kind + 1) << "row_add dispatches by index";
}

TEST_F(OutlinerStackPaintLayersSourceTest,
       a_material_mask_correction_is_a_row_with_no_map_to_paint)
{
  MaterialPaintLayer *layer = add_layer("A");
  Material *other = BKE_material_add(bmain, "Other");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskCorrectionMaterial;
  params.anchor = layer;
  params.name = "Wood";
  params.source = other;
  MaterialPaintLayer *item = BKE_paint_layers_add_with_policy(*material, params);
  ASSERT_NE(item, nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*material, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2u);
  const StackRow &row = rows[0].name == "Wood" ? rows[0] : rows[1];
  EXPECT_EQ(row.name, "Wood");
  EXPECT_EQ(row.parent_section_id, "MASK");
  EXPECT_EQ(row.icon, int(ICON_MATERIAL));
  EXPECT_TRUE(row.compact);
  EXPECT_TRUE(row.preview_slots.is_empty()) << "its grey is its source's Base Color";
  EXPECT_TRUE(row.content_sections.is_empty()) << "and it owns no map to switch the brush to";
  EXPECT_STREQ(row.value_prop, "opacity");
  EXPECT_STREQ(row.mode_prop, "blend_type");

  Vector<StackAddKindInfo> kinds;
  source().editor()->add_kinds(kinds);
  ASSERT_EQ(kinds.size(), 25u);
  EXPECT_EQ(kinds[24].identifier, "ANCHOR_POINT_MASK_CORRECTION")
      << "row_add dispatches by index, last of the Anchor Point kinds";
}

}  // namespace tests
}  // namespace blender::ed::outliner
