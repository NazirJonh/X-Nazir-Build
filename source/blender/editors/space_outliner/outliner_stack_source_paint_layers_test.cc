/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "UI_resources.hh"

#include "ED_paint_material_layer.hh"

#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {
namespace tests {

class OutlinerStackPaintLayersSourceTest : public bke::BlenderGTestBase {
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

  const StackSource &source()
  {
    const StackSource *source = stack_source_get(SO_STACK_SRC_PAINT_MATERIAL);
    BLI_assert(source != nullptr);
    return *source;
  }

  static Image &add_image(Main &bmain, const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return *BKE_image_add_generated(
        &bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }
};

/**
 * Phase 6, goal 1: the Add menu (Python operator and Outliner alike) offers every source a Layer
 * row can have for a correction/mask too. #PaintStackAddKind's new entries and their positions in
 * #StackEditor::add_kinds are what #row_add reads back by index, so the two must stay aligned.
 */
TEST_F(OutlinerStackPaintLayersSourceTest, add_kinds_offers_every_correction_and_mask_source)
{
  const StackEditor *editor = source().editor();
  ASSERT_NE(editor, nullptr);
  Vector<StackAddKindInfo> kinds;
  editor->add_kinds(kinds);

  auto has = [&](const char *identifier) {
    for (const StackAddKindInfo &kind : kinds) {
      if (kind.identifier == identifier) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(has("CORRECTION_MESH_MAP"));
  EXPECT_TRUE(has("CORRECTION_MATERIAL"));
  EXPECT_TRUE(has("CORRECTION_NODE_GROUP"));
  EXPECT_TRUE(has("CORRECTION_STACK"));
  EXPECT_TRUE(has("MASK_CORRECTION_MESH_MAP"));
  EXPECT_TRUE(has("MASK_CORRECTION_MATERIAL"));
  EXPECT_TRUE(has("MASK_CORRECTION_NODE_GROUP"));
  EXPECT_TRUE(has("MASK_CORRECTION_STACK"));

  /* Index alignment with #PaintStackAddKind: row_add reads a kind back by its position here. */
  ASSERT_GT(kinds.size(), int(PAINT_STACK_ADD_MASK_CORRECTION_STACK));
  EXPECT_EQ(kinds[PAINT_STACK_ADD_CORRECTION_MATERIAL].identifier, "CORRECTION_MATERIAL");
  EXPECT_EQ(kinds[PAINT_STACK_ADD_CORRECTION_MATERIAL].source_id_type, ID_MA);
  EXPECT_EQ(kinds[PAINT_STACK_ADD_CORRECTION_NODE_GROUP].identifier, "CORRECTION_NODE_GROUP");
  EXPECT_EQ(kinds[PAINT_STACK_ADD_CORRECTION_NODE_GROUP].source_id_type, ID_NT);
}

TEST_F(OutlinerStackPaintLayersSourceTest, layered_material_rows_come_from_the_description)
{
  Material *ma = BKE_material_add(bmain, "Layered");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  ASSERT_EQ(rows.size(), 2);
  /* The tree is built from the last row down and hangs a child only off a folder it already made,
   * so the folder's row follows its contents; its ordinal still comes first. */
  EXPECT_STREQ(rows[1].name.c_str(), "Folder");
  EXPECT_TRUE(rows[1].can_hold_children);
  EXPECT_TRUE(BLI_uuid_equal(rows[1].stable_id, folder->marker));
  EXPECT_LT(rows[1].ordinal, rows[0].ordinal);
  EXPECT_EQ(rows[0].parent_ordinal, rows[1].ordinal);
  EXPECT_TRUE(BLI_uuid_equal(rows[0].stable_id, child->marker));
}

TEST_F(OutlinerStackPaintLayersSourceTest, fresh_mask_is_shown_by_the_layer_row)
{
  /* A base mask is drawn by its layer row's own mask slot, not as a hierarchy row of its own. */
  Material *ma = BKE_material_add(bmain, "Layered");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_EQ(item->channels_num, 0);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  const StackRow *layer_row = nullptr;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, layer->marker)) {
      layer_row = &row;
    }
    /* The base mask itself is not a row. */
    EXPECT_FALSE(BLI_uuid_equal(row.stable_id, item->marker));
  }
  ASSERT_NE(layer_row, nullptr);
  /* The layer row carries the mask slot that names the MASK section. */
  bool has_mask_slot = false;
  for (const StackRowPreview &slot : layer_row->preview_slots) {
    if (slot.section_id == "MASK") {
      has_mask_slot = true;
    }
  }
  EXPECT_TRUE(has_mask_slot);
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_section_appears_with_the_first_item)
{
  /* The MASK section's presence is what a click uses to switch the brush to the mask target, so
   * it has to exist as soon as the row has a mask item -- even a constant one with no map. */
  Material *ma = BKE_material_add(bmain, "MaskSection");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  auto find_layer_row = [&]() -> const StackRow * {
    for (const StackRow &row : rows) {
      if (BLI_uuid_equal(row.stable_id, layer->marker)) {
        return &row;
      }
    }
    return nullptr;
  };
  auto has_mask_section = [](const StackRow &row) {
    for (const StackContentSection &section : row.content_sections) {
      if (section.identifier == "MASK") {
        return true;
      }
    }
    return false;
  };

  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  const StackRow *before = find_layer_row();
  ASSERT_NE(before, nullptr);
  EXPECT_FALSE(has_mask_section(*before));

  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
  rows.clear();
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  const StackRow *after = find_layer_row();
  ASSERT_NE(after, nullptr);
  bool found = false;
  for (const StackContentSection &section : after->content_sections) {
    if (section.identifier == "MASK") {
      found = true;
      /* A constant item has no map, so the section carries no sub-rows. */
      EXPECT_TRUE(section.sub_rows.is_empty());
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_section_leaves_item_maps_to_the_item_row)
{
  Material *ma = BKE_material_add(bmain, "MaskMaps");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Layer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);
  Image *map = &add_image(*bmain, "MaskMap");
  ASSERT_TRUE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  const StackContentSection *mask = nullptr;
  for (const StackRow &row : rows) {
    if (!BLI_uuid_equal(row.stable_id, layer->marker)) {
      continue;
    }
    for (const StackContentSection &section : row.content_sections) {
      if (section.identifier == "MASK") {
        mask = &section;
      }
    }
  }
  ASSERT_NE(mask, nullptr);
  /* The item's map is shown by the item's own row -- attached to this section -- not relisted as
   * a section sub-row, which would put a second "Mask" row beside it. */
  EXPECT_TRUE(mask->sub_rows.is_empty());
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_correction_creates_and_keeps_a_base)
{
  Material *ma = BKE_material_add(bmain, "MaskBase");
  const int layer_ordinal = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(layer_ordinal, 0);
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(*ma, layer_ordinal);
  ASSERT_NE(layer, nullptr);
  ASSERT_EQ(BKE_paint_layers_mask_base(*layer), nullptr);

  /* Adding a mask correction to a layer with no base creates the base first. */
  const int corr = paint_layers_edit_add(
      *ma, PAINT_STACK_ADD_MASK_CORRECTION_FILL, layer_ordinal, {});
  ASSERT_GE(corr, 0);
  MaterialPaintLayer *base = BKE_paint_layers_mask_base(*layer);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(layer->mask_stack.first, base);
  const Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
  ASSERT_EQ(items.size(), 2);
  EXPECT_EQ(items.first(), base);

  /* A second Add Mask is refused; the base stays single. */
  EXPECT_FALSE(paint_layers_edit_mask_set(*ma, layer_ordinal, true));
  EXPECT_EQ(BKE_paint_layers_mask_items(*layer).size(), 2);

  /* The base is shown by the layer row's mask slot, not as a row; the correction is a compact
   * row of its own. */
  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  bool base_row = false;
  bool correction_row = false;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, base->marker)) {
      base_row = true;
    }
    if (BLI_uuid_equal(row.stable_id, items.last()->marker)) {
      correction_row = true;
      EXPECT_TRUE(row.compact);
      EXPECT_EQ(row.parent_section_id, "MASK");
    }
  }
  EXPECT_FALSE(base_row);
  EXPECT_TRUE(correction_row);

  /* Remove Mask takes the base and every correction over it together. */
  EXPECT_TRUE(paint_layers_edit_mask_set(*ma, layer_ordinal, false));
  EXPECT_TRUE(BKE_paint_layers_mask_items(*layer).is_empty());
}

TEST_F(OutlinerStackPaintLayersSourceTest, layered_state_hash_moves_with_a_rename)
{
  Material *ma = BKE_material_add(bmain, "HashLayered");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const uint64_t before = source().state_hash(ctx, ma->id);
  ASSERT_TRUE(BKE_paint_layers_rename(*ma, layer, "Renamed"));
  const uint64_t after = source().state_hash(ctx, ma->id);
  EXPECT_NE(before, after);
}

TEST_F(OutlinerStackPaintLayersSourceTest, layered_active_row_reads_the_active_marker)
{
  Material *ma = BKE_material_add(bmain, "ActiveLayered");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  ASSERT_EQ(rows.size(), 1);

  EXPECT_FALSE(source().row_is_active(ctx, {}, ma->id, rows[0]));
  BKE_paint_layers_active_set(*ma, layer->marker);
  EXPECT_TRUE(source().row_is_active(ctx, {}, ma->id, rows[0]));
}

TEST_F(OutlinerStackPaintLayersSourceTest, layered_material_is_editable)
{
  Material *ma = BKE_material_add(bmain, "EditableLayered");
  BKE_paint_layers_add(*ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  EXPECT_TRUE(source().is_editable(ma->id));
}

TEST_F(OutlinerStackPaintLayersSourceTest, layered_folder_is_not_a_paint_target)
{
  Material *ma = BKE_material_add(bmain, "FolderTarget");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  BKE_paint_layers_active_set(*ma, folder->marker);

  /* The target resolver refuses a folder: it carries no maps of its own. */
  PaintLayersTarget target;
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Content, target));
}

TEST_F(OutlinerStackPaintLayersSourceTest, empty_channel_row_has_no_preview_id)
{
  Material *ma = BKE_material_add(bmain, "EmptyChannel");
  BKE_paint_layers_add(*ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Empty", nullptr, PaintLayerPlace::Above);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  ASSERT_EQ(rows.size(), 1);
  ASSERT_GE(rows[0].preview_slots.size(), 1);
  /* A channel with no map yet has no preview id: the draw falls back to the row's plain icon. */
  EXPECT_EQ(rows[0].preview_slots[0].id_uid, 0u);
  EXPECT_STREQ(rows[0].preview_slots[0].section_id.c_str(), "CHANNELS");
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_add_material_creates_a_row_with_a_source)
{
  Material *ma = BKE_material_add(bmain, "MaterialOwner");
  Material *source = BKE_material_add(bmain, "MaterialSource");
  StackAddArgs args;
  args.source = &source->id;
  const int ordinal = paint_layers_edit_add(*ma, PAINT_STACK_ADD_MATERIAL, -1, args);
  ASSERT_GE(ordinal, 0);
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(*ma, ordinal);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(layer->material, source);
  /* A bake source is a map, not a channel of the layer's own. */
  EXPECT_EQ(layer->channels_num, 0);

  /* No source is refused rather than silently ignored. */
  EXPECT_EQ(paint_layers_edit_add(*ma, PAINT_STACK_ADD_MATERIAL, -1, {}), -1);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_add_makes_paint_fill_folder_and_corrections)
{
  Material *ma = BKE_material_add(bmain, "AddKinds");

  const int paint = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(paint, 0);
  MaterialPaintLayer *paint_layer = paint_description_row_for_ordinal(*ma, paint);
  EXPECT_EQ(paint_layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  /* The Add gives a Paint or Fill row the default channel set, so it is not inert. */
  EXPECT_EQ(paint_layer->channels_num, 5);

  StackAddArgs fill_args;
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  fill_args.color = color;
  const int fill = paint_layers_edit_add(*ma, PAINT_STACK_ADD_FILL, -1, fill_args);
  ASSERT_GE(fill, 0);
  MaterialPaintLayer *fill_layer = paint_description_row_for_ordinal(*ma, fill);
  ASSERT_NE(fill_layer, nullptr);
  EXPECT_EQ(fill_layer->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  float base_color[4];
  BKE_paint_layers_base_color_get(*fill_layer, base_color);
  EXPECT_NEAR(base_color[0], 0.25f, 1e-6f);
  EXPECT_EQ(fill_layer->channels_num, 5);

  const int folder = paint_layers_edit_add(*ma, PAINT_STACK_ADD_FOLDER, -1, {});
  ASSERT_GE(folder, 0);
  MaterialPaintLayer *folder_layer = paint_description_row_for_ordinal(*ma, folder);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder_layer));
  EXPECT_EQ(folder_layer->channels_num, 0);

  /* A content correction hangs off the anchor row. */
  const int correction = paint_layers_edit_add(*ma, PAINT_STACK_ADD_CORRECTION_PAINT, paint, {});
  ASSERT_GE(correction, 0);
  MaterialPaintLayer *corr = paint_description_row_for_ordinal(*ma, correction);
  ASSERT_NE(corr, nullptr);
  EXPECT_NE(BKE_paint_layers_role(*corr), PaintLayerRole::Layer);
  EXPECT_EQ(corr->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(corr->role, MA_PAINT_LAYER_ROLE_EFFECT);

  /* A mask Fill correction: one source, not a role plus a source argument (S-1). */
  const int mask_correction = paint_layers_edit_add(
      *ma, PAINT_STACK_ADD_MASK_CORRECTION_FILL, paint, {});
  ASSERT_GE(mask_correction, 0);
  MaterialPaintLayer *mcorr = paint_description_row_for_ordinal(*ma, mask_correction);
  ASSERT_NE(mcorr, nullptr);
  EXPECT_EQ(mcorr->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(mcorr->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_add_into_folder_nests_and_into_non_folder_groups)
{
  Material *ma = BKE_material_add(bmain, "IntoKinds");
  const int folder = paint_layers_edit_add(*ma, PAINT_STACK_ADD_FOLDER, -1, {});
  ASSERT_GE(folder, 0);
  const int child = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, folder, {});
  ASSERT_GE(child, 0);
  MaterialPaintLayer *folder_layer = paint_description_row_for_ordinal(*ma, folder);
  MaterialPaintLayer *child_layer = paint_description_row_for_ordinal(*ma, child);
  ASSERT_NE(folder_layer, nullptr);
  ASSERT_NE(child_layer, nullptr);
  /* The child was added into the folder, not beside it. */
  EXPECT_EQ(BLI_findindex(&folder_layer->children, child_layer), 0);
}

/**
 * Phase 6, goal 4: `row_add` with an Outliner anchor that is itself a Stack correction/mask item
 * (not a Layer row) places the new row inside that correction's own #children, exactly as it
 * already does for a folder Layer row (#BKE_paint_layers_is_folder does not care which list the
 * folder hangs off). This was already true from phase 4 (#paint_layers_edit_add's non-correction
 * branch never special-cased the anchor's role, only whether it is a folder); the assertion here
 * locks that in rather than introducing it.
 */
TEST_F(OutlinerStackPaintLayersSourceTest, edit_add_anchored_on_a_stack_correction_nests_inside_it)
{
  Material *ma = BKE_material_add(bmain, "AddIntoCorrection");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(effect, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  int effect_ordinal = -1;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, effect->marker)) {
      effect_ordinal = row.ordinal;
    }
  }
  ASSERT_GE(effect_ordinal, 0);

  const int child = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, effect_ordinal, {});
  ASSERT_GE(child, 0);
  MaterialPaintLayer *child_layer = paint_description_row_for_ordinal(*ma, child);
  ASSERT_NE(child_layer, nullptr);
  EXPECT_EQ(BLI_findindex(&effect->children, child_layer), 0);
}

/**
 * Phase 6, goal 4, ordinal lockstep: #paint_stack_rows_from_description_impl now recurses into a
 * folder correction's own children (#append_corrections), which consumes ordinals that
 * #paint_description_row_for_ordinal and #layers_ordinal_of did not previously account for. Every
 * row *after* a correction-with-children in storage order would otherwise resolve to the wrong
 * ordinal between what the Outliner shows (rows_build) and what an edit verb resolves
 * (paint_description_row_for_ordinal) -- silently misdirecting rename/move/remove/reorder on any
 * row past such a correction. Both helpers were fixed in the same change that added the recursion.
 */
TEST_F(OutlinerStackPaintLayersSourceTest,
      ordinals_stay_in_lockstep_with_rows_after_a_correction_folder)
{
  Material *ma = BKE_material_add(bmain, "OrdinalLockstep");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(effect, nullptr);
  MaterialPaintLayer *effect_child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "FxChild", effect, PaintLayerPlace::Into);
  ASSERT_NE(effect_child, nullptr);
  /* A sibling layer stored after the correction-carrying owner. */
  MaterialPaintLayer *later = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Later", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(later, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  int later_row_ordinal = -1;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, later->marker)) {
      later_row_ordinal = row.ordinal;
    }
  }
  ASSERT_GE(later_row_ordinal, 0);

  /* #paint_description_row_for_ordinal must agree with rows_build's numbering, or an edit on this
   * ordinal silently lands on the wrong row. */
  EXPECT_EQ(paint_description_row_for_ordinal(*ma, later_row_ordinal), later);

  ASSERT_TRUE(paint_layers_edit_rename(*ma, later_row_ordinal, "Renamed"));
  EXPECT_STREQ(later->name, "Renamed");
  EXPECT_STREQ(owner->name, "Owner");

  /* #layers_ordinal_of (the sibling helper `paint_layers_edit_add`'s return value goes through) is
   * TU-local, so it is exercised indirectly here: a brand new top-level row's returned ordinal must
   * also agree with where rows_build places it, which it can only do if the two counters stayed in
   * lockstep across the correction's children. */
  const int fresh = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(fresh, 0);
  MaterialPaintLayer *fresh_layer = paint_description_row_for_ordinal(*ma, fresh);
  ASSERT_NE(fresh_layer, nullptr);
  rows.clear();
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  int fresh_row_ordinal = -1;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, fresh_layer->marker)) {
      fresh_row_ordinal = row.ordinal;
    }
  }
  EXPECT_EQ(fresh_row_ordinal, fresh);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_remove_clears_the_row)
{
  Material *ma = BKE_material_add(bmain, "Remove");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  const int b = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_TRUE(paint_layers_edit_remove(*ma, a));
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_move_below_reorders_layers)
{
  Material *ma = BKE_material_add(bmain, "Move");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  const int b = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  /* Move the top layer below the bottom one. */
  int moved = -1;
  ASSERT_TRUE(paint_layers_edit_move(*ma, b, a, StackMovePlace::Below, &moved));
  ASSERT_GE(moved, 0);
  EXPECT_EQ(BLI_findindex(&ma->paint_layers, paint_description_row_for_ordinal(*ma, moved)), 0);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_move_into_non_folder_groups_the_pair)
{
  Material *ma = BKE_material_add(bmain, "IntoGroups");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  const int b = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  ASSERT_TRUE(paint_layers_edit_move(*ma, b, a, StackMovePlace::Into, nullptr));
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
  MaterialPaintLayer *folder =
      ma->paint_layers.is_empty() ? nullptr : &*ma->paint_layers.begin();
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder));
  EXPECT_EQ(BLI_listbase_count(&folder->children), 2);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_visibility_and_rename)
{
  Material *ma = BKE_material_add(bmain, "Edit");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_TRUE(paint_layers_edit_set_enabled(*ma, a, false));
  EXPECT_EQ(paint_description_row_for_ordinal(*ma, a)->flag & MA_PAINT_LAYER_ENABLED, 0);
  ASSERT_TRUE(paint_layers_edit_rename(*ma, a, "Renamed"));
  EXPECT_STREQ(paint_description_row_for_ordinal(*ma, a)->name, "Renamed");
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_duplicate_inserts_above_with_fresh_marker)
{
  Material *ma = BKE_material_add(bmain, "Dup");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  const bUUID marker = paint_description_row_for_ordinal(*ma, a)->marker;
  const int copy = paint_layers_edit_duplicate(*bmain, *ma, a);
  ASSERT_GE(copy, 0);
  MaterialPaintLayer *copy_layer = paint_description_row_for_ordinal(*ma, copy);
  ASSERT_NE(copy_layer, nullptr);
  EXPECT_FALSE(BLI_uuid_equal(copy_layer->marker, marker));
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 2);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_mask_add_remove_and_toggle)
{
  Material *ma = BKE_material_add(bmain, "Mask");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_TRUE(paint_layers_edit_mask_set(*ma, a, true));
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(*ma, a);
  Vector<MaterialPaintLayer *> items = BKE_paint_layers_mask_items(*layer);
  ASSERT_FALSE(items.is_empty());
  EXPECT_NE(items.first()->flag & MA_PAINT_LAYER_ENABLED, 0);
  ASSERT_TRUE(paint_layers_edit_mask_toggle(*ma, a));
  EXPECT_EQ(items.first()->flag & MA_PAINT_LAYER_ENABLED, 0);
  ASSERT_TRUE(paint_layers_edit_mask_set(*ma, a, false));
  EXPECT_TRUE(BKE_paint_layers_mask_items(*layer).is_empty());
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_group_and_ungroup)
{
  Material *ma = BKE_material_add(bmain, "Group");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  const int b = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  const int folder = paint_layers_edit_group_range(*ma, a, b);
  ASSERT_GE(folder, 0);
  MaterialPaintLayer *folder_layer = paint_description_row_for_ordinal(*ma, folder);
  ASSERT_NE(folder_layer, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder_layer));
  EXPECT_EQ(BLI_listbase_count(&folder_layer->children), 2);
  EXPECT_EQ(paint_layers_edit_ungroup(*ma, folder), 2);
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 2);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_group_add_and_color_tag)
{
  Material *ma = BKE_material_add(bmain, "GroupAdd");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  const int folder = paint_layers_edit_group_add(*ma, a);
  ASSERT_GE(folder, 0);
  MaterialPaintLayer *folder_layer = paint_description_row_for_ordinal(*ma, folder);
  ASSERT_NE(folder_layer, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder_layer));
  /* A newly created folder must have no color tag (-1 = none). */
  EXPECT_EQ(folder_layer->color_tag, -1);
  ASSERT_TRUE(paint_layers_edit_color_tag(*ma, folder, 3));
  EXPECT_EQ(folder_layer->color_tag, 3);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_fill_color_only_touches_fill_layers)
{
  Material *ma = BKE_material_add(bmain, "FillColor");
  StackAddArgs args;
  const float first[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  args.color = first;
  const int fill = paint_layers_edit_add(*ma, PAINT_STACK_ADD_FILL, -1, args);
  ASSERT_GE(fill, 0);
  const float second[4] = {0.9f, 0.8f, 0.7f, 1.0f};
  ASSERT_TRUE(paint_layers_edit_fill_color(*ma, fill, second));
  float base_color[4];
  BKE_paint_layers_base_color_get(*paint_description_row_for_ordinal(*ma, fill), base_color);
  EXPECT_NEAR(base_color[0], 0.9f, 1e-6f);

  /* A Paint layer refuses a fill-colour write. */
  const int paint = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(paint, 0);
  EXPECT_FALSE(paint_layers_edit_fill_color(*ma, paint, second));
}

TEST_F(OutlinerStackPaintLayersSourceTest, fill_without_base_color_draws_no_swatch)
{
  Material *ma = BKE_material_add(bmain, "FillSwatch");
  StackAddArgs args;
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  args.color = color;
  const int fill = paint_layers_edit_add(*ma, PAINT_STACK_ADD_FILL, -1, args);
  ASSERT_GE(fill, 0);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  auto fill_swatch = [&]() -> const StackRowPreview * {
    rows.clear();
    if (!source().rows_build(ctx, {}, ma->id, rows)) {
      return nullptr;
    }
    for (const StackRowPreview &slot : rows[fill].preview_slots) {
      if (slot.is_color_swatch) {
        return &slot;
      }
    }
    return nullptr;
  };

  /* A live Fill shows its colour. */
  const StackRowPreview *swatch = fill_swatch();
  ASSERT_NE(swatch, nullptr);
  EXPECT_NEAR(swatch->color[0], 0.25f, 1e-6f);

  /* With the Base Color switched off (or removed) the row paints no colour, so no swatch is
   * drawn; the swatch comes back with the same stored colour once the channel is re-enabled. */
  MaterialPaintLayer *layer = paint_description_row_for_ordinal(*ma, fill);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_EQ(fill_swatch(), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(fill_swatch(), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  swatch = fill_swatch();
  ASSERT_NE(swatch, nullptr);
  EXPECT_NEAR(swatch->color[3], 0.0f, 1e-6f);
}

TEST_F(OutlinerStackPaintLayersSourceTest, edit_reorder_swaps_siblings)
{
  Material *ma = BKE_material_add(bmain, "Reorder");
  const int a = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  const int b = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, -1, {});
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  MaterialPaintLayer *top = paint_description_row_for_ordinal(*ma, b);
  ASSERT_TRUE(paint_layers_edit_reorder(*ma, b, a));
  EXPECT_EQ(BLI_findindex(&ma->paint_layers, top), 0);
}

TEST_F(OutlinerStackPaintLayersSourceTest, description_issues_show_on_the_row_they_are_about)
{
  Material *ma = BKE_material_add(bmain, "Issues");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill");
  ASSERT_NE(correction, nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2);

  const StackRow *corr_row = nullptr;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, correction->marker)) {
      corr_row = &row;
    }
  }
  ASSERT_NE(corr_row, nullptr);
  bool warning_found = false;
  for (const StackRowPreview &slot : corr_row->preview_slots) {
    if (slot.icon == ICON_ERROR) {
      warning_found = true;
      EXPECT_FALSE(slot.label.empty());
    }
  }
  EXPECT_TRUE(warning_found);
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_target_is_removed_only_for_the_active_subtree)
{
  Material *ma = BKE_material_add(bmain, "MaskTarget");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *active = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Active", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *other = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Other", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(active, nullptr);
  ASSERT_NE(other, nullptr);
  BKE_paint_layers_active_set(*ma, active->marker);

  using sculpt_paint::material_layer::mask_target_is_removed;

  /* Removing the active layer, or the folder holding it, loses the mask target. */
  EXPECT_TRUE(mask_target_is_removed(*ma, active->marker, PAINT_LAYER_TARGET_MASK));
  EXPECT_TRUE(mask_target_is_removed(*ma, folder->marker, PAINT_LAYER_TARGET_MASK));
  /* Removing a neighbour does not. */
  EXPECT_FALSE(mask_target_is_removed(*ma, other->marker, PAINT_LAYER_TARGET_MASK));
  /* And content mode has no mask target to lose. */
  EXPECT_FALSE(mask_target_is_removed(*ma, folder->marker, PAINT_LAYER_TARGET_CONTENT));

  /* With no active row there is no target to lose either. */
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_FALSE(mask_target_is_removed(*ma, folder->marker, PAINT_LAYER_TARGET_MASK));
}

TEST_F(OutlinerStackPaintLayersSourceTest, rows_edit_the_selected_channel_and_never_write_on_build)
{
  Material *ma = BKE_material_add(bmain, "PerChannelRows");
  MaterialPaintLayer *with_record = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "With", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *without = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Without", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(with_record, nullptr);
  ASSERT_NE(without, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, with_record, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);

  uint32_t before[2];
  uint32_t after[2];
  BKE_paint_layers_bake_hash(*with_record, before);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_EQ(rows.size(), 2);

  const StackRow *with_row = nullptr;
  const StackRow *without_row = nullptr;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, with_record->marker)) {
      with_row = &row;
    }
    if (BLI_uuid_equal(row.stable_id, without->marker)) {
      without_row = &row;
    }
  }
  ASSERT_NE(with_row, nullptr);
  ASSERT_NE(without_row, nullptr);

  /* Every pair has a settings entry, record or not: both rows point their columns at it, and only
   * the inherited default is flagged for dimming. */
  EXPECT_TRUE(with_row->value_ptr.has_value());
  EXPECT_TRUE(with_row->value_inherited);
  EXPECT_TRUE(without_row->value_ptr.has_value());
  EXPECT_TRUE(without_row->value_inherited);
  EXPECT_TRUE(without_row->mode_inherited);

  /* Building the rows is read-only: the description is untouched (rule K-1). */
  BKE_paint_layers_bake_hash(*with_record, after);
  EXPECT_EQ(before[0], after[0]);
  EXPECT_EQ(before[1], after[1]);
  EXPECT_EQ(without->channels_num, 0);

  /* A value edit changes the settings array, never a channel record, and the rebuild is no longer
   * inherited. */
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *without, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.25f));
  EXPECT_EQ(without->channels_num, 0);
  EXPECT_FLOAT_EQ(without->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].opacity, 0.25f);

  Vector<StackRow> rebuilt;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rebuilt);
  const StackRow *rebuilt_row = nullptr;
  for (const StackRow &row : rebuilt) {
    if (BLI_uuid_equal(row.stable_id, without->marker)) {
      rebuilt_row = &row;
    }
  }
  ASSERT_NE(rebuilt_row, nullptr);
  EXPECT_TRUE(rebuilt_row->value_ptr.has_value());
  EXPECT_FALSE(rebuilt_row->value_inherited);
}

TEST_F(OutlinerStackPaintLayersSourceTest, folder_rows_offer_channel_overrides_without_maps)
{
  Material *ma = BKE_material_add(bmain, "FolderChannelRows");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(child, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  ASSERT_GE(rows.size(), 2u);
  const StackRow *folder_row = nullptr;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, folder->marker)) {
      folder_row = &row;
    }
  }
  ASSERT_NE(folder_row, nullptr);
  EXPECT_TRUE(folder_row->value_inherited);
  EXPECT_TRUE(folder_row->mode_inherited);

  /* A settings-only override on a folder is not a map, so it is not an issue. */
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  EXPECT_TRUE(issues.is_empty());

  Vector<StackRow> rebuilt;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rebuilt);
  for (const StackRow &row : rebuilt) {
    if (BLI_uuid_equal(row.stable_id, folder->marker)) {
      EXPECT_TRUE(row.value_ptr.has_value());
      EXPECT_TRUE(row.mode_ptr.has_value());
      EXPECT_FALSE(row.mode_inherited);
    }
  }
}

TEST_F(OutlinerStackPaintLayersSourceTest, normal_channel_has_opacity_and_a_mix_replace_column)
{
  Material *ma = BKE_material_add(bmain, "NormalColumns");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Normal", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);

  Vector<StackRow> rows;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, rows);
  ASSERT_EQ(rows.size(), 1u);
  /* Opacity is a real per-pair setting, and the mode column offers Mix or Replace; Mix is the
   * stored inherit value, so the column is never dimmed as inherited. */
  EXPECT_TRUE(rows[0].value_ptr.has_value());
  EXPECT_TRUE(rows[0].mode_ptr.has_value());
  EXPECT_FALSE(rows[0].mode_inherited);
}

TEST_F(OutlinerStackPaintLayersSourceTest, mask_correction_columns_point_at_the_row)
{
  Material *ma = BKE_material_add(bmain, "MaskCorrColumns");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  /* A base mask first, so the correction below is a real mask item with a row of its own. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
  ASSERT_NE(correction, nullptr);

  /* A mask correction lays coverage, not colour, so its blend is one of the coverage modes; it
   * carries both the row opacity and the mode columns. */
  Vector<StackRow> rows;
  paint_stack_rows_from_description(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rows);
  const StackRow *row = nullptr;
  for (const StackRow &candidate : rows) {
    if (BLI_uuid_equal(candidate.stable_id, correction->marker)) {
      row = &candidate;
    }
  }
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(row->value_ptr.has_value());
  EXPECT_STREQ(row->value_prop, "opacity");
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(row->value_ptr->data), correction);
  ASSERT_TRUE(row->mode_ptr.has_value());
  EXPECT_STREQ(row->mode_prop, "blend_type");
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(row->mode_ptr->data), correction);
}

TEST_F(OutlinerStackPaintLayersSourceTest, folder_mask_shows_on_the_folder_row)
{
  Material *ma = BKE_material_add(bmain, "FolderMaskUI");
  const int folder_ordinal = paint_layers_edit_add(*ma, PAINT_STACK_ADD_FOLDER, -1, {});
  ASSERT_GE(folder_ordinal, 0);
  const int child_ordinal = paint_layers_edit_add(*ma, PAINT_STACK_ADD_PAINT, folder_ordinal, {});
  ASSERT_GE(child_ordinal, 0);
  MaterialPaintLayer *folder = paint_description_row_for_ordinal(*ma, folder_ordinal);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_is_folder(*folder));

  /* A folder takes a mask; a correction would not (its coverage is its mask section). */
  ASSERT_TRUE(paint_layers_edit_mask_set(*ma, folder_ordinal, true));
  const Vector<MaterialPaintLayer *> folder_masks = BKE_paint_layers_mask_items(*folder);
  ASSERT_FALSE(folder_masks.is_empty());

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));
  const StackRow *folder_row = nullptr;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, folder->marker)) {
      folder_row = &row;
    }
    /* The base mask is not a row of its own. */
    EXPECT_FALSE(BLI_uuid_equal(row.stable_id, folder_masks.first()->marker));
  }
  ASSERT_NE(folder_row, nullptr);
  bool has_mask_slot = false;
  for (const StackRowPreview &slot : folder_row->preview_slots) {
    if (slot.section_id == "MASK") {
      has_mask_slot = true;
    }
  }
  EXPECT_TRUE(has_mask_slot);
}

TEST_F(OutlinerStackPaintLayersSourceTest, state_hash_tracks_a_blank_map_becoming_non_blank)
{
  Material *ma = BKE_material_add(bmain, "BlankHash");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  /* A generated blank map starts transparent: the row shows a placeholder thumbnail. */
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *image = BKE_image_add_generated(
      bmain, 8, 8, "Blank", 32, false, IMA_GENTYPE_BLANK, clear, false, false, false);
  ASSERT_NE(image, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));

  const StackReadContext ctx{bmain, nullptr, nullptr};
  const uint64_t before = source().state_hash(ctx, ma->id);
  EXPECT_EQ(source().state_hash(ctx, ma->id), before);

  /* The first pixel makes the blank map non-blank; the rows must notice and rebuild. */
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  BKE_image_mark_dirty(image, ibuf);
  BKE_image_release_ibuf(image, ibuf, lock);
  EXPECT_NE(source().state_hash(ctx, ma->id), before);
}

/**
 * Phase 6, goal 4: a Stack (content) correction and a Stack mask item may each be a folder with
 * children of their own (phases 3/4). #append_corrections (outliner_stack_source_paint_layers.cc)
 * now recurses into a folder correction/mask item's own #children exactly as it does for a folder
 * Layer row: its child becomes its own row, nested one level deeper, parented at the
 * correction/mask row's ordinal.
 */
TEST_F(OutlinerStackPaintLayersSourceTest, stack_correction_and_mask_children_become_nested_rows)
{
  Material *ma = BKE_material_add(bmain, "StackFolderCorrections");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  /* A base mask first, so the Stack mask below it is a real correction rather than the base the
   * layer row's own slot stands for. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, owner, 1.0f), nullptr);

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(effect, nullptr);
  MaterialPaintLayer *effect_child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "FxChild", effect, PaintLayerPlace::Into);
  ASSERT_NE(effect_child, nullptr);

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  MaterialPaintLayer *mask_child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "MaskChild", mask, PaintLayerPlace::Into);
  ASSERT_NE(mask_child, nullptr);

  const StackReadContext ctx{bmain, nullptr, nullptr};
  Vector<StackRow> rows;
  ASSERT_TRUE(source().rows_build(ctx, {}, ma->id, rows));

  /* The owner row, the effect row, the mask row, and now one row each for their children. */
  ASSERT_EQ(rows.size(), 5);
  int owner_rows = 0;
  int effect_rows = 0;
  int mask_rows = 0;
  int effect_child_rows = 0;
  int mask_child_rows = 0;
  int16_t effect_ordinal = -1;
  int16_t mask_ordinal = -1;
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, owner->marker)) {
      owner_rows++;
    }
    else if (BLI_uuid_equal(row.stable_id, effect->marker)) {
      effect_rows++;
      effect_ordinal = row.ordinal;
      EXPECT_EQ(row.parent_section_id, "CHANNELS");
      EXPECT_TRUE(row.can_hold_children);
      EXPECT_TRUE(row.has_children);
    }
    else if (BLI_uuid_equal(row.stable_id, mask->marker)) {
      mask_rows++;
      mask_ordinal = row.ordinal;
      EXPECT_EQ(row.parent_section_id, "MASK");
      EXPECT_TRUE(row.can_hold_children);
      EXPECT_TRUE(row.has_children);
    }
    else if (BLI_uuid_equal(row.stable_id, effect_child->marker)) {
      effect_child_rows++;
    }
    else if (BLI_uuid_equal(row.stable_id, mask_child->marker)) {
      mask_child_rows++;
    }
  }
  EXPECT_EQ(owner_rows, 1);
  EXPECT_EQ(effect_rows, 1);
  EXPECT_EQ(mask_rows, 1);
  EXPECT_EQ(effect_child_rows, 1);
  EXPECT_EQ(mask_child_rows, 1);

  /* Each child is parented at its own correction/mask row, not at the owner or at each other. */
  for (const StackRow &row : rows) {
    if (BLI_uuid_equal(row.stable_id, effect_child->marker)) {
      EXPECT_EQ(row.parent_ordinal, effect_ordinal);
    }
    else if (BLI_uuid_equal(row.stable_id, mask_child->marker)) {
      EXPECT_EQ(row.parent_ordinal, mask_ordinal);
    }
  }

  /* An edit inside a correction's own subtree must invalidate the cached rows, exactly as an edit
   * inside a folder Layer's children already does. */
  const uint64_t before = source().state_hash(ctx, ma->id);
  effect_child->opacity = 0.5f;
  EXPECT_NE(source().state_hash(ctx, ma->id), before);
}

}  // namespace tests
}  // namespace blender::ed::outliner
