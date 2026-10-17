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
  ASSERT_EQ(kinds.size(), 17u);
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

}  // namespace tests
}  // namespace blender::ed::outliner
