/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <initializer_list>
#include <string>

#include "BKE_asset.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_import.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

/** How many tags named \a name the metadata carries; more than one means a duplicate. */
static int asset_tag_count(const AssetMetaData &meta, const char *name)
{
  int count = 0;
  for (const AssetTag &tag : meta.tags) {
    if (std::string(tag.name) == name) {
      count++;
    }
  }
  return count;
}

class PaintLayersAssetTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(
        bmain, 4, 4, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  MaterialPaintLayer *add_layer(Material &ma,
                                const char *name,
                                Image *image,
                                const eMaterialPaintChannel channel =
                                    PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *row = BKE_paint_layers_add(ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(row, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(ma, row, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(ma, row, channel, image));
    return row;
  }

  void expect_row_names(const ListBaseT<MaterialPaintLayer> &list,
                        const std::initializer_list<const char *> expected)
  {
    int index = 0;
    for (const MaterialPaintLayer &row : list) {
      ASSERT_LT(index, int(expected.size()));
      EXPECT_STREQ(row.name, *(expected.begin() + index));
      index++;
    }
    EXPECT_EQ(index, int(expected.size()));
  }

  void expect_stack_names(const Material &ma,
                          const std::initializer_list<const char *> expected)
  {
    expect_row_names(ma.paint_layers, expected);
  }
};

TEST_F(PaintLayersAssetTest, import_rows_copies_the_branch_with_maps_of_its_own)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  Image *image = add_image("SourceMap");
  MaterialPaintLayer *row = add_layer(*src, "Layer", image);

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  ASSERT_EQ(result.rows, 1);
  ASSERT_EQ(BLI_listbase_count(&dst->paint_layers), 1);
  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  EXPECT_NE(copy, row);
  EXPECT_FALSE(BLI_uuid_equal(copy->marker, row->marker));
  ASSERT_EQ(copy->channels_num, 1);
  EXPECT_NE(copy->channels[0].image, image);
  ASSERT_NE(copy->channels[0].image, nullptr);
  /* The source keeps its own row and its own map. */
  EXPECT_EQ(BLI_listbase_count(&src->paint_layers), 1);
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(src->paint_layers.first), row);
}

TEST_F(PaintLayersAssetTest, import_rows_can_widen_the_target_channel_set)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *row = add_layer(
      *src, "Height", add_image("HeightMap"), PAINT_MATERIAL_CHANNEL_HEIGHT);

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  EXPECT_EQ(result.skipped_channels, 0);
  EXPECT_TRUE((BKE_paint_layers_channel_set_mask_get(*dst) &
               (uint16_t(1) << PAINT_MATERIAL_CHANNEL_HEIGHT)) != 0);
  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  ASSERT_EQ(copy->channels_num, 1);
  EXPECT_EQ(copy->channels[0].channel, PAINT_MATERIAL_CHANNEL_HEIGHT);
}

TEST_F(PaintLayersAssetTest, import_rows_drops_channels_the_target_filter_excludes)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *row = add_layer(
      *src, "Height", add_image("HeightMap"), PAINT_MATERIAL_CHANNEL_HEIGHT);

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  EXPECT_EQ(result.skipped_channels, 1);
  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  EXPECT_EQ(copy->channels_num, 0);
}

TEST_F(PaintLayersAssetTest, import_mask_replaces_the_target_mask_and_keeps_a_base)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *source_row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, source_row, 0.25f), nullptr);
  MaterialPaintLayer *target = add_layer(*dst, "Target", add_image("TargetMap"));

  const MaterialPaintLayer *source_rows[1] = {source_row};
  PaintLayerImportParams params;
  params.mode = PaintLayerImportMode::Mask;
  params.anchor = target;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  ASSERT_EQ(result.rows, 1);
  ASSERT_NE(target->mask_stack.first, nullptr);
  EXPECT_EQ(BLI_listbase_count(&target->mask_stack), 1);
  MaterialPaintLayer *item = static_cast<MaterialPaintLayer *>(target->mask_stack.first);
  const MaterialPaintLayer *source_item =
      static_cast<const MaterialPaintLayer *>(source_row->mask_stack.first);
  EXPECT_TRUE((item->flag & MA_PAINT_LAYER_MASK_BASE) != 0);
  EXPECT_FALSE(BLI_uuid_equal(item->marker, source_item->marker));
}

TEST_F(PaintLayersAssetTest, smart_material_create_copies_the_new_material)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(
      *src, "Height", add_image("HeightMap"), PAINT_MATERIAL_CHANNEL_HEIGHT);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                   PaintLayerAssetType::Material,
                                   "SmartTest");
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Material);
  EXPECT_EQ(BLI_listbase_count(&asset->paint_layers), 1);
  EXPECT_TRUE((BKE_paint_layers_channel_set_mask_get(*asset) &
               (uint16_t(1) << PAINT_MATERIAL_CHANNEL_HEIGHT)) != 0);
  const IDProperty *type = IDP_GetPropertyFromGroup(asset->id.properties, "paint_asset_type");
  ASSERT_NE(type, nullptr);
  ASSERT_EQ(type->type, IDP_INT);
  EXPECT_EQ(IDP_int_get(type), int(PaintLayerAssetType::Material));
}

TEST_F(PaintLayersAssetTest, smart_layer_create_copies_one_stack_row)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                   *src,
                                                   Span<const MaterialPaintLayer *>(source_rows, 1),
                                                   PaintLayerAssetType::Layer,
                                                   "SmartLayerTest");
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Layer);
  EXPECT_EQ(BLI_listbase_count(&asset->paint_layers), 1);
}

TEST_F(PaintLayersAssetTest, smart_mask_create_keeps_only_the_mask)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, row, 0.25f), nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                   PaintLayerAssetType::Mask,
                                   "SmartMaskTest");
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Mask);
  ASSERT_EQ(BLI_listbase_count(&asset->paint_layers), 1);
  MaterialPaintLayer *holder = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  EXPECT_NE(holder->mask_stack.first, nullptr);
}

/** The Levels correction the tests hang on a row, named so the metadata can be told apart. */
static MaterialPaintLayer *add_levels_effect(Material &ma,
                                             MaterialPaintLayer &row,
                                             const char *name)
{
  return BKE_paint_layers_filter_add(ma, &row, MA_PAINT_LAYER_FILTER_LEVELS, name);
}

/** The first effect of \a row, or null. */
static MaterialPaintLayer *first_effect(MaterialPaintLayer &row)
{
  return static_cast<MaterialPaintLayer *>(row.effects.first);
}

/** The top-level rows of \a asset: the span a drop imports, for an import that names \a asset as
 * its source the rows have to be its own. */
static Vector<const MaterialPaintLayer *> asset_top_rows(const Material &asset)
{
  Vector<const MaterialPaintLayer *> rows;
  for (const MaterialPaintLayer &row : asset.paint_layers) {
    rows.append(&row);
  }
  return rows;
}

TEST_F(PaintLayersAssetTest, smart_material_create_exposes_every_parameter)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartParams");
  ASSERT_NE(asset, nullptr);
  ASSERT_TRUE(BKE_paint_layers_asset_params_has(*asset));

  const IDProperty *params = BKE_paint_layers_asset_params_get(*asset);
  const IDProperty *tone = IDP_GetPropertyFromGroup(params, "Tone");
  ASSERT_NE(tone, nullptr);
  ASSERT_EQ(tone->type, IDP_GROUP);
  /* One integer per schema parameter, named after it and carrying its position. */
  const IDProperty *in_black = IDP_GetPropertyFromGroup(tone, "in_black");
  ASSERT_NE(in_black, nullptr);
  EXPECT_EQ(IDP_int_get(in_black), PAINT_LAYER_LEVELS_IN_BLACK);
  const IDProperty *gamma = IDP_GetPropertyFromGroup(tone, "gamma");
  ASSERT_NE(gamma, nullptr);
  EXPECT_EQ(IDP_int_get(gamma), PAINT_LAYER_LEVELS_GAMMA);
  /* The plain row has no parameters of its own and so no entry. */
  EXPECT_EQ(IDP_GetPropertyFromGroup(params, "Layer"), nullptr);
  /* The metadata reaches the asset copy too, for the Asset Browser. */
  asset->id.asset_data = BKE_asset_metadata_create();
  BKE_paint_layers_asset_params_expose(*asset);
  const IDProperty *mirrored = BKE_asset_metadata_idprop_find(asset->id.asset_data,
                                                             "paint_asset_params");
  ASSERT_NE(mirrored, nullptr);
  ASSERT_NE(IDP_GetPropertyFromGroup(mirrored, "Tone"), nullptr);
}

TEST_F(PaintLayersAssetTest, smart_asset_import_marks_the_exposed_parameters)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  MaterialPaintLayer *tint = BKE_paint_layers_filter_add(
      *src, row, MA_PAINT_LAYER_FILTER_HSL, nullptr);
  ASSERT_NE(tint, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartMarked");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  ASSERT_EQ(result.imported.size(), 1);
  MaterialPaintLayer *copy = result.imported[0];
  EXPECT_STREQ(copy->name, "Layer");

  /* The corrections the asset exposed carry their marks; the plain row has none. */
  MaterialPaintLayer *tone_copy = first_effect(*copy);
  ASSERT_NE(tone_copy, nullptr);
  EXPECT_STREQ(tone_copy->name, "Tone");
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone_copy, "in_black"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone_copy, "out_white"));
  MaterialPaintLayer *tint_copy = tone_copy->next;
  ASSERT_NE(tint_copy, nullptr);
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tint_copy, "hue"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tint_copy, "value"));
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*copy, "hue"));
  /* The values came over as locals, beside the marks. */
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*tone_copy, PAINT_LAYER_LEVELS_GAMMA), 1.0f);
}

TEST_F(PaintLayersAssetTest, param_switch_flips_the_instance_flag)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartSwitch");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  MaterialPaintLayer *tone = first_effect(*result.imported[0]);
  ASSERT_NE(tone, nullptr);

  /* The import marks Int 1: offered and active. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*tone, "in_black"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone, "in_black"));

  /* Switching off writes the Int 0 the panel and the reload read; the mark stays, so the
   * parameter is still offered. */
  BKE_paint_layers_item_param_enabled_set(*tone, "in_black", false);
  const IDProperty *marks = IDP_GetPropertyFromGroup(tone->system_properties,
                                                     ASSET_PARAMS_PROP_NAME);
  ASSERT_NE(marks, nullptr);
  const IDProperty *mark = IDP_GetPropertyFromGroup(marks, "in_black");
  ASSERT_NE(mark, nullptr);
  EXPECT_EQ(mark->type, IDP_INT);
  EXPECT_EQ(IDP_int_get(mark), 0);
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*tone, "in_black"));
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*tone, "in_black"));

  /* Switching back on writes the 1 the import had put there. */
  BKE_paint_layers_item_param_enabled_set(*tone, "in_black", true);
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone, "in_black"));
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(marks, "in_black")), 1);
}

TEST_F(PaintLayersAssetTest, param_switch_without_a_mark_does_nothing)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);

  /* A row the import never marked: not offered, and the switch must not conjure a mark group. */
  EXPECT_FALSE(BKE_paint_layers_item_param_is_offered(*tone, "gamma"));
  BKE_paint_layers_item_param_enabled_set(*tone, "gamma", false);
  EXPECT_EQ(IDP_GetPropertyFromGroup(tone->system_properties, ASSET_PARAMS_PROP_NAME), nullptr);

  /* A row with no system properties at all must survive the switch the same way. */
  MaterialPaintLayer *plain = BKE_paint_layers_add(*src, "Plain", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(plain, nullptr);
  BKE_paint_layers_item_param_enabled_set(*plain, "gamma", false);
  EXPECT_EQ(plain->system_properties, nullptr);
}

TEST_F(PaintLayersAssetTest, param_mark_without_a_zero_int_reads_as_active)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);

  /* Marks an older file or build may carry: a non-zero Int and a mark without a usable Int value
   * at all. Both read as active -- only an Int 0 is the switched-off state. */
  IDProperty *marks = bke::idprop::create_group(ASSET_PARAMS_PROP_NAME).release();
  IDP_ReplaceInGroup(tone->system_properties, marks);
  IDP_ReplaceInGroup(marks, bke::idprop::create("in_black", 3).release());
  IDP_ReplaceInGroup(marks, bke::idprop::create("gamma", 0.5f).release());

  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*tone, "in_black"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone, "in_black"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*tone, "gamma"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone, "gamma"));

  /* The switch only writes an Int: a mark of another shape is left alone, still active. */
  BKE_paint_layers_item_param_enabled_set(*tone, "gamma", false);
  const IDProperty *float_mark = IDP_GetPropertyFromGroup(marks, "gamma");
  ASSERT_NE(float_mark, nullptr);
  EXPECT_EQ(float_mark->type, IDP_FLOAT);
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone, "gamma"));
}

TEST_F(PaintLayersAssetTest, import_marks_do_not_move_the_bake_hash)
{
  /* Two stacks built the same way: one imported from a smart asset (its marks in place), one from
   * a plain material. The label is metadata, so both rows keep the same validity key. */
  Material *plain = BKE_material_add(bmain, "Plain");
  MaterialPaintLayer *plain_row = BKE_paint_layers_add(
      *plain, "Layer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(plain_row, nullptr);
  ASSERT_NE(add_levels_effect(*plain, *plain_row, "Tone"), nullptr);

  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = BKE_paint_layers_add(*src, "Layer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartHash");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  MaterialPaintLayer *marked_row = result.imported[0];
  ASSERT_NE(marked_row, nullptr);
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*first_effect(*marked_row), "gamma"));

  uint32_t marked_hash[2];
  uint32_t plain_hash[2];
  BKE_paint_layers_bake_hash(*dst, *marked_row, marked_hash);
  BKE_paint_layers_bake_hash(*plain, *plain_row, plain_hash);
  EXPECT_EQ(marked_hash[0], plain_hash[0]);
  EXPECT_EQ(marked_hash[1], plain_hash[1]);
}

TEST_F(PaintLayersAssetTest, reload_replaces_the_rows_and_keeps_the_instance_values)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartReload");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  MaterialPaintLayer *copy = result.imported[0];
  const int instance_id = BKE_paint_layers_asset_instance_record(
      *dst, *asset, PaintLayerAssetType::Material, result.imported.as_span());
  ASSERT_EQ(instance_id, 1);
  EXPECT_TRUE(BKE_paint_layers_asset_instances_any(*dst));

  /* The instance changed a value; the asset grew a second row and moved its own value. */
  paint_layer_kind_param_set(*first_effect(*copy), PAINT_LAYER_LEVELS_IN_BLACK, 0.25f);
  MaterialPaintLayer *asset_row = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  paint_layer_kind_param_set(*first_effect(*asset_row), PAINT_LAYER_LEVELS_IN_BLACK, 0.75f);
  ASSERT_NE(BKE_paint_layers_add(*asset, "Added", nullptr, PaintLayerPlace::Above), nullptr);

  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, instance_id),
            PaintLayerAssetReloadResult::Reloaded);

  /* The structure came from the asset; the instance's own value survived the replacement, over
   * the one the asset moved. */
  ASSERT_EQ(BLI_listbase_count(&dst->paint_layers), 2);
  MaterialPaintLayer *fresh = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  const MaterialPaintLayer *added = fresh->next;
  EXPECT_STREQ(added->name, "Added");
  EXPECT_STREQ(fresh->name, "Layer");
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*first_effect(*fresh), PAINT_LAYER_LEVELS_IN_BLACK), 0.25f);
  /* The fresh rows are marked again, so the panel keeps working, and the record names them. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*first_effect(*fresh), "in_black"));
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *fresh), instance_id);
}

TEST_F(PaintLayersAssetTest, reload_reports_a_missing_source)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartGone");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  /* Without a recorded import there is nothing to reload. */
  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, 1),
            PaintLayerAssetReloadResult::NoSource);

  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  const int instance_id = BKE_paint_layers_asset_instance_record(
      *dst, *asset, PaintLayerAssetType::Material, result.imported.as_span());

  /* The asset renamed away: the label names a material this file no longer has. */
  BKE_libblock_rename(*bmain, asset->id, "SmartRenamed");
  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, instance_id),
            PaintLayerAssetReloadResult::SourceMissing);
}

TEST_F(PaintLayersAssetTest, make_unique_ends_the_binding_and_keeps_the_values)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartUnique");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  MaterialPaintLayer *copy = result.imported[0];
  const int instance_id = BKE_paint_layers_asset_instance_record(
      *dst, *asset, PaintLayerAssetType::Material, result.imported.as_span());
  paint_layer_kind_param_set(*first_effect(*copy), PAINT_LAYER_LEVELS_GAMMA, 2.0f);

  BKE_paint_layers_asset_params_unbind(*dst, instance_id);

  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*first_effect(*copy), "gamma"));
  EXPECT_FALSE(BKE_paint_layers_asset_instances_any(*dst));
  /* The value the instance carried stays: unbinding is not a reset. */
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*first_effect(*copy), PAINT_LAYER_LEVELS_GAMMA), 2.0f);
}

/** Import \a asset's rows into \a dst and record them as one instance. */
static int import_and_record(Main &bmain,
                             Material &dst,
                             Material &asset,
                             Span<const MaterialPaintLayer *> asset_rows)
{
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  if (!BKE_paint_layers_import_rows(bmain, dst, asset, asset_rows, params, &result)) {
    return -1;
  }
  return BKE_paint_layers_asset_instance_record(
      dst, asset, PaintLayerAssetType::Material, result.imported.as_span());
}

/** The instances of \a ma, in record order. */
static Vector<PaintLayerAssetInstanceInfo> instances_read(const Material &ma)
{
  Vector<PaintLayerAssetInstanceInfo> instances;
  BKE_paint_layers_asset_instances_foreach(ma, [&](const PaintLayerAssetInstanceInfo &info) {
    instances.append(info);
    return true;
  });
  return instances;
}

TEST_F(PaintLayersAssetTest, two_drops_of_one_asset_make_two_instances)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(asset_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "TwiceDropped");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  const int first_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  const int second_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  ASSERT_EQ(first_id, 1);
  ASSERT_EQ(second_id, 2);

  const Vector<PaintLayerAssetInstanceInfo> instances = instances_read(*dst);
  ASSERT_EQ(instances.size(), 2);
  EXPECT_EQ(instances[0].id, 1);
  EXPECT_EQ(instances[0].number, 1);
  EXPECT_EQ(instances[1].id, 2);
  EXPECT_EQ(instances[1].number, 2);
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, 1), "TwiceDropped #1");
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, 2), "TwiceDropped #2");

  /* Each drop laid its own rows: fresh markers, one instance each, no overlap. */
  MaterialPaintLayer *first_row = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  MaterialPaintLayer *second_row = first_row->next;
  ASSERT_NE(second_row, nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *first_row), 1);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *second_row), 2);
  EXPECT_FALSE(BLI_uuid_equal(first_row->marker, second_row->marker));
}

TEST_F(PaintLayersAssetTest, reload_of_one_instance_leaves_the_other_and_the_place_alone)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(asset_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "Neighbor");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  ASSERT_NE(BKE_paint_layers_add(*dst, "Bottom", nullptr, PaintLayerPlace::Above), nullptr);
  const int first_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  const int second_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  ASSERT_EQ(first_id, 1);
  ASSERT_EQ(second_id, 2);
  ASSERT_NE(BKE_paint_layers_add(*dst, "Top", nullptr, PaintLayerPlace::Above), nullptr);
  /* Bottom to top: Bottom, first instance, second instance, Top. */
  expect_stack_names(*dst, {"Bottom", "Layer", "Layer", "Top"});

  MaterialPaintLayer *first_row = static_cast<MaterialPaintLayer *>(dst->paint_layers.first->next);
  MaterialPaintLayer *second_row = first_row->next;
  paint_layer_kind_param_set(*first_effect(*first_row), PAINT_LAYER_LEVELS_IN_BLACK, 0.1f);
  paint_layer_kind_param_set(*first_effect(*second_row), PAINT_LAYER_LEVELS_IN_BLACK, 0.9f);

  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, first_id),
            PaintLayerAssetReloadResult::Reloaded);

  /* The second instance's rows are the very same rows with the very same value. */
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *second_row), second_id);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*first_effect(*second_row), PAINT_LAYER_LEVELS_IN_BLACK), 0.9f);
  /* The first instance's fresh rows took the block's old place between its neighbours, and the
   * value the user changed came back on them. */
  expect_stack_names(*dst, {"Bottom", "Layer", "Layer", "Top"});
  MaterialPaintLayer *fresh = static_cast<MaterialPaintLayer *>(dst->paint_layers.first->next);
  ASSERT_NE(fresh, second_row);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *fresh), first_id);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*first_effect(*fresh), PAINT_LAYER_LEVELS_IN_BLACK), 0.1f);
}

TEST_F(PaintLayersAssetTest, reload_keeps_a_switched_off_parameter_off)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartKeepOff");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  const int instance_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  ASSERT_EQ(instance_id, 1);
  MaterialPaintLayer *tone = first_effect(
      *static_cast<MaterialPaintLayer *>(dst->paint_layers.first));
  ASSERT_NE(tone, nullptr);

  /* The instance switched in_black off and moved both values; the asset moved its own too. */
  BKE_paint_layers_item_param_enabled_set(*tone, "in_black", false);
  paint_layer_kind_param_set(*tone, PAINT_LAYER_LEVELS_IN_BLACK, 0.25f);
  paint_layer_kind_param_set(*tone, PAINT_LAYER_LEVELS_GAMMA, 2.0f);
  MaterialPaintLayer *asset_row = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  paint_layer_kind_param_set(*first_effect(*asset_row), PAINT_LAYER_LEVELS_IN_BLACK, 0.75f);
  paint_layer_kind_param_set(*first_effect(*asset_row), PAINT_LAYER_LEVELS_GAMMA, 1.5f);

  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, instance_id),
            PaintLayerAssetReloadResult::Reloaded);

  MaterialPaintLayer *fresh = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  MaterialPaintLayer *fresh_tone = first_effect(*fresh);
  ASSERT_NE(fresh_tone, nullptr);
  /* The switch came back off over the fresh mark, found by the parameter's name, and the value
   * follows the asset: the carried 0.25 was of a parameter the panel greys out. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*fresh_tone, "in_black"));
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*fresh_tone, "in_black"));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fresh_tone, PAINT_LAYER_LEVELS_IN_BLACK), 0.75f);
  /* The active parameter's carried value came back, as any reload brings it. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*fresh_tone, "gamma"));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fresh_tone, PAINT_LAYER_LEVELS_GAMMA), 2.0f);
}

TEST_F(PaintLayersAssetTest, param_switch_of_one_instance_leaves_the_other_and_the_asset_alone)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(asset_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "SplitSwitch");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  const int first_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  const int second_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  ASSERT_EQ(first_id, 1);
  ASSERT_EQ(second_id, 2);
  MaterialPaintLayer *first_row = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  MaterialPaintLayer *second_row = first_row->next;
  ASSERT_NE(second_row, nullptr);

  BKE_paint_layers_item_param_enabled_set(*first_effect(*first_row), "gamma", false);

  /* The first instance's switch is off, its mark still there; the second keeps its own mark at
   * the Int 1 the import wrote -- the switch never crossed the instance border. */
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*first_effect(*first_row), "gamma"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*first_effect(*first_row), "gamma"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*first_effect(*second_row), "gamma"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_offered(*first_effect(*second_row), "gamma"));
  /* The asset's own rows never carried import marks, and a switch on an instance cannot give
   * them one. */
  EXPECT_FALSE(BKE_paint_layers_item_param_is_offered(
      *first_effect(*static_cast<MaterialPaintLayer *>(asset->paint_layers.first)), "gamma"));
}

TEST_F(PaintLayersAssetTest, make_unique_of_one_instance_leaves_the_other_bound)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(asset_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "SplitUnique");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  const int first_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  const int second_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  ASSERT_EQ(first_id, 1);
  ASSERT_EQ(second_id, 2);
  MaterialPaintLayer *first_row = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  MaterialPaintLayer *second_row = first_row->next;

  BKE_paint_layers_asset_params_unbind(*dst, first_id);

  /* The first instance's marks and record are gone; the second keeps both. */
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*first_effect(*first_row), "gamma"));
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*first_effect(*second_row), "gamma"));
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *first_row), -1);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *second_row), second_id);
  const Vector<PaintLayerAssetInstanceInfo> instances = instances_read(*dst);
  ASSERT_EQ(instances.size(), 1);
  EXPECT_EQ(instances[0].id, second_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, second_id), "SplitUnique #2");
}

TEST_F(PaintLayersAssetTest, duplicated_row_belongs_to_no_instance)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(asset_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "CopySource");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  const int instance_id = import_and_record(*bmain, *dst, *asset, asset_top_rows(*asset).as_span());
  ASSERT_EQ(instance_id, 1);
  MaterialPaintLayer *copy_row = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);

  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *dst, copy_row);
  ASSERT_NE(duplicate, nullptr);
  /* The copy has a marker of its own, and nothing written into the row names an instance: it is
   * local, whatever the record says about the row it was copied from. */
  EXPECT_FALSE(BLI_uuid_equal(duplicate->marker, copy_row->marker));
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *copy_row), instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *duplicate), -1);
}

TEST_F(PaintLayersAssetTest, legacy_source_label_reads_as_the_first_instance)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  ASSERT_NE(add_levels_effect(*src, *row, "Tone"), nullptr);
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(asset_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "OldFile");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  MaterialPaintLayer *copy = result.imported[0];

  /* Simulate the one-instance label an older build wrote: no instances group, one source group. */
  if (dst->id.properties == nullptr) {
    dst->id.properties = bke::idprop::create_group("properties").release();
  }
  IDProperty *legacy = bke::idprop::create_group("paint_asset_source").release();
  IDP_ReplaceInGroup(
      legacy, bke::idprop::create("name", StringRefNull(asset->id.name + 2)).release());
  IDP_ReplaceInGroup(legacy, bke::idprop::create("library", "").release());
  IDP_ReplaceInGroup(legacy, bke::idprop::create("type", int(PaintLayerAssetType::Material)).release());
  IDProperty *legacy_rows = bke::idprop::create_group("rows").release();
  char marker[UUID_STRING_SIZE];
  BLI_uuid_format(marker, copy->marker);
  IDP_ReplaceInGroup(legacy_rows, bke::idprop::create(marker, 1).release());
  IDP_ReplaceInGroup(legacy, legacy_rows);
  IDP_ReplaceInGroup(dst->id.properties, legacy);

  /* The read answers for it as instance 1 without touching the properties. */
  EXPECT_TRUE(BKE_paint_layers_asset_instances_any(*dst));
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *copy), 1);
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, 1), "OldFile #1");
  const Vector<PaintLayerAssetInstanceInfo> before = instances_read(*dst);
  ASSERT_EQ(before.size(), 1);
  EXPECT_EQ(before[0].id, 1);
  EXPECT_EQ(before[0].number, 1);
  EXPECT_EQ(IDP_GetPropertyFromGroup(dst->id.properties, "paint_asset_instances"), nullptr);

  /* A drop or a reload is the migration point: the label moves into the instances group as
   * instance 1, and a second drop of the same asset reads as #2. */
  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, 1), PaintLayerAssetReloadResult::Reloaded);
  EXPECT_EQ(IDP_GetPropertyFromGroup(dst->id.properties, "paint_asset_source"), nullptr);
  const IDProperty *instances = IDP_GetPropertyFromGroup(dst->id.properties,
                                                         "paint_asset_instances");
  ASSERT_NE(instances, nullptr);
  MaterialPaintLayer *fresh = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*dst, *fresh), 1);
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, 1), "OldFile #1");
  const int second_id = BKE_paint_layers_asset_instance_record(
      *dst, *asset, PaintLayerAssetType::Material, Span<const MaterialPaintLayer *>());
  ASSERT_EQ(second_id, 2);
  const Vector<PaintLayerAssetInstanceInfo> after = instances_read(*dst);
  ASSERT_EQ(after.size(), 2);
  EXPECT_EQ(after[1].number, 2);
}

TEST_F(PaintLayersAssetTest, drops_of_different_assets_count_their_numbers_separately)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  const MaterialPaintLayer *asset_rows[1] = {row};
  Material *asset_x = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(asset_rows, 1), PaintLayerAssetType::Material, "X");
  Material *asset_y = BKE_paint_layers_asset_create(
      *bmain, *src, Span<const MaterialPaintLayer *>(asset_rows, 1), PaintLayerAssetType::Material, "Y");
  ASSERT_NE(asset_x, nullptr);
  ASSERT_NE(asset_y, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  ASSERT_EQ(import_and_record(*bmain, *dst, *asset_x, asset_top_rows(*asset_x).as_span()), 1);
  /* A different asset is a first drop of its own: the numbers are per asset, not per material. */
  ASSERT_EQ(import_and_record(*bmain, *dst, *asset_y, asset_top_rows(*asset_y).as_span()), 2);
  ASSERT_EQ(import_and_record(*bmain, *dst, *asset_x, asset_top_rows(*asset_x).as_span()), 3);

  const Vector<PaintLayerAssetInstanceInfo> instances = instances_read(*dst);
  ASSERT_EQ(instances.size(), 3);
  EXPECT_EQ(instances[0].number, 1);
  EXPECT_EQ(instances[0].name, "X");
  EXPECT_EQ(instances[1].number, 1);
  EXPECT_EQ(instances[1].name, "Y");
  EXPECT_EQ(instances[2].number, 2);
  EXPECT_EQ(instances[2].name, "X");
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*dst, 3), "X #2");
}

/**
 * A Smart Material holding one "Folder" row with a "Member" map row and a "Tone" Levels
 * correction inside it. \a r_child is set to the child row of the asset's copy.
 */
static Material *make_folder_asset(PaintLayersAssetTest &t,
                                   const char *name,
                                   MaterialPaintLayer **r_child)
{
  Material *src = BKE_material_add(t.bmain, "Src");
  MaterialPaintLayer *member = t.add_layer(*src, "Member", t.add_image("MemberMap"));
  EXPECT_NE(add_levels_effect(*src, *member, "Tone"), nullptr);
  MaterialPaintLayer *folder = BKE_paint_layers_group(*src,
                                                      Span<MaterialPaintLayer *>(&member, 1));
  EXPECT_NE(folder, nullptr);
  const MaterialPaintLayer *source_rows[1] = {folder};
  Material *asset = BKE_paint_layers_asset_create(*t.bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  name);
  EXPECT_NE(asset, nullptr);
  *r_child = nullptr;
  if (asset != nullptr) {
    MaterialPaintLayer *asset_folder = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
    *r_child = (asset_folder != nullptr) ?
                   static_cast<MaterialPaintLayer *>(asset_folder->children.first) :
                   nullptr;
  }
  return asset;
}

/** One drop of a folder asset Into a folder of a fresh material: the state the nested-instance
 * tests read. The drop lays the asset's folder inside the host folder, above its "Host" row. */
struct NestedInstanceDrop {
  Material *dst = nullptr;
  MaterialPaintLayer *host_folder = nullptr;
  /** The instance root: the folder copy the drop laid down, nested in the host folder. */
  MaterialPaintLayer *root = nullptr;
  /** The root's child row, carrying the Tone correction. */
  MaterialPaintLayer *child = nullptr;
  int instance_id = 0;
};

static NestedInstanceDrop drop_folder_asset_into_host_folder(PaintLayersAssetTest &t,
                                                             Material &asset)
{
  NestedInstanceDrop drop;
  drop.dst = BKE_material_add(t.bmain, "Dst");
  EXPECT_NE(drop.dst, nullptr);
  MaterialPaintLayer *host_member = t.add_layer(*drop.dst, "Host", t.add_image("HostMap"));
  drop.host_folder = BKE_paint_layers_group(*drop.dst,
                                            Span<MaterialPaintLayer *>(&host_member, 1));
  EXPECT_NE(drop.host_folder, nullptr);
  STRNCPY(drop.host_folder->name, "HostFolder");

  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  params.anchor = drop.host_folder;
  params.place = PaintLayerPlace::Into;
  PaintLayerImportResult result;
  EXPECT_TRUE(BKE_paint_layers_import_rows(
      *t.bmain, *drop.dst, asset, asset_top_rows(asset).as_span(), params, &result));
  drop.root = (result.imported.size() == 1) ? result.imported[0] : nullptr;
  drop.instance_id = BKE_paint_layers_asset_instance_record(
      *drop.dst, asset, PaintLayerAssetType::Material, result.imported.as_span());
  drop.child = (drop.root != nullptr) ?
                   static_cast<MaterialPaintLayer *>(drop.root->children.first) :
                   nullptr;
  return drop;
}

TEST_F(PaintLayersAssetTest, drop_into_a_folder_makes_the_nested_rows_an_instance)
{
  MaterialPaintLayer *asset_child = nullptr;
  Material *asset = make_folder_asset(*this, "SmartNested", &asset_child);
  ASSERT_NE(asset, nullptr);

  NestedInstanceDrop drop = drop_folder_asset_into_host_folder(*this, *asset);
  ASSERT_NE(drop.root, nullptr);
  ASSERT_NE(drop.child, nullptr);
  ASSERT_EQ(drop.instance_id, 1);
  /* The drop landed Into the folder: the roots hang on its children, not on the top level. */
  EXPECT_EQ(BKE_paint_layers_owner_list(*drop.dst, *drop.root), &drop.host_folder->children);
  expect_row_names(drop.host_folder->children, {"Host", "Folder"});

  /* The nested root answers for itself and its child follows it; the child's own marker names no
   * instance, and the label the panel and the Outliner draw comes from the root alone. */
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *drop.root), drop.instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *drop.child), drop.instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_marker(*drop.dst, drop.root->marker),
            drop.instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_marker(*drop.dst, drop.child->marker), -1);
  EXPECT_EQ(BKE_paint_layers_asset_instance_label(*drop.dst, drop.instance_id),
            "SmartNested #1");
  /* The parameters the asset exposed are marked on the nested copy, so the panel's section finds
   * them through the same walk. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*first_effect(*drop.child), "in_black"));
}

TEST_F(PaintLayersAssetTest, reload_of_a_nested_instance_replaces_the_rows_in_the_folder)
{
  MaterialPaintLayer *asset_child = nullptr;
  Material *asset = make_folder_asset(*this, "SmartNestedReload", &asset_child);
  ASSERT_NE(asset, nullptr);
  NestedInstanceDrop drop = drop_folder_asset_into_host_folder(*this, *asset);
  ASSERT_NE(drop.root, nullptr);
  ASSERT_NE(drop.child, nullptr);

  /* The instance changed a value; the asset grew a row and moved its own value. */
  paint_layer_kind_param_set(*first_effect(*drop.child), PAINT_LAYER_LEVELS_IN_BLACK, 0.25f);
  paint_layer_kind_param_set(*first_effect(*asset_child), PAINT_LAYER_LEVELS_IN_BLACK, 0.75f);
  ASSERT_NE(BKE_paint_layers_add(*asset, "Added", nullptr, PaintLayerPlace::Above), nullptr);

  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *drop.dst, drop.instance_id),
            PaintLayerAssetReloadResult::Reloaded);

  /* The fresh rows are back inside the same folder, in the block's old place above the host row,
   * and the instance's own value came back on the nested copy over the asset's. */
  expect_row_names(drop.host_folder->children, {"Host", "Folder", "Added"});
  expect_stack_names(*drop.dst, {"HostFolder"});
  MaterialPaintLayer *fresh_root = static_cast<MaterialPaintLayer *>(
      drop.host_folder->children.last);
  ASSERT_NE(fresh_root, nullptr);
  EXPECT_STREQ(fresh_root->name, "Added");
  MaterialPaintLayer *fresh_folder = fresh_root->prev;
  ASSERT_NE(fresh_folder, nullptr);
  /* Both fresh rows are roots of the instance: the record names everything the import laid down. */
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *fresh_folder), drop.instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *fresh_root), drop.instance_id);
  MaterialPaintLayer *fresh_child = static_cast<MaterialPaintLayer *>(fresh_folder->children.first);
  ASSERT_NE(fresh_child, nullptr);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*first_effect(*fresh_child), PAINT_LAYER_LEVELS_IN_BLACK), 0.25f);
  /* The record names the fresh nested roots, so the next reload finds them again. */
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_marker(*drop.dst, fresh_folder->marker),
            drop.instance_id);
}

TEST_F(PaintLayersAssetTest, reload_of_a_nested_instance_lands_back_into_an_emptied_folder)
{
  MaterialPaintLayer *asset_child = nullptr;
  Material *asset = make_folder_asset(*this, "SmartNestedEmpty", &asset_child);
  ASSERT_NE(asset, nullptr);
  NestedInstanceDrop drop = drop_folder_asset_into_host_folder(*this, *asset);
  ASSERT_NE(drop.root, nullptr);
  /* The user moves the host row out, so the folder holds the instance's block alone: no seam is
   * left to anchor on. */
  ASSERT_TRUE(BKE_paint_layers_move(*drop.dst, drop.host_folder->children.first, nullptr,
                                    PaintLayerPlace::Above));

  ASSERT_NE(BKE_paint_layers_add(*asset, "Added", nullptr, PaintLayerPlace::Above), nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *drop.dst, drop.instance_id),
            PaintLayerAssetReloadResult::Reloaded);

  /* The fresh rows went back into the folder that held the block, whatever the stack around it. */
  expect_row_names(drop.host_folder->children, {"Folder", "Added"});
  expect_stack_names(*drop.dst, {"HostFolder", "Host"});
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(
                *drop.dst,
                *static_cast<MaterialPaintLayer *>(drop.host_folder->children.first)),
            drop.instance_id);
}

TEST_F(PaintLayersAssetTest, make_unique_of_a_nested_instance_unbinds_the_nested_rows)
{
  MaterialPaintLayer *asset_child = nullptr;
  Material *asset = make_folder_asset(*this, "SmartNestedUnique", &asset_child);
  ASSERT_NE(asset, nullptr);
  NestedInstanceDrop drop = drop_folder_asset_into_host_folder(*this, *asset);
  ASSERT_NE(drop.root, nullptr);
  ASSERT_NE(drop.child, nullptr);
  paint_layer_kind_param_set(*first_effect(*drop.child), PAINT_LAYER_LEVELS_GAMMA, 2.0f);

  BKE_paint_layers_asset_params_unbind(*drop.dst, drop.instance_id);

  /* The marks are gone from the nested copy -- the walk reached them under the folder -- and the
   * record with them; the value the instance carried stays. */
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*first_effect(*drop.child), "gamma"));
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *drop.root), -1);
  EXPECT_FALSE(BKE_paint_layers_asset_instances_any(*drop.dst));
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*first_effect(*drop.child), PAINT_LAYER_LEVELS_GAMMA), 2.0f);
}

TEST_F(PaintLayersAssetTest, root_lifted_out_of_the_folder_stays_the_instance_root)
{
  MaterialPaintLayer *asset_child = nullptr;
  Material *asset = make_folder_asset(*this, "SmartNestedLifted", &asset_child);
  ASSERT_NE(asset, nullptr);
  NestedInstanceDrop drop = drop_folder_asset_into_host_folder(*this, *asset);
  ASSERT_NE(drop.root, nullptr);
  ASSERT_NE(drop.child, nullptr);

  /* The user lifts the root onto the top of the stack: the marker is untouched, so the row keeps
   * being the instance's root, and its child keeps following it. */
  ASSERT_TRUE(BKE_paint_layers_move(*drop.dst, drop.root, nullptr, PaintLayerPlace::Above));
  EXPECT_EQ(BKE_paint_layers_owner_list(*drop.dst, *drop.root), &drop.dst->paint_layers);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *drop.root), drop.instance_id);
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *drop.child), drop.instance_id);

  ASSERT_NE(BKE_paint_layers_add(*asset, "Added", nullptr, PaintLayerPlace::Above), nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *drop.dst, drop.instance_id),
            PaintLayerAssetReloadResult::Reloaded);

  /* The fresh rows took the block's new place: the top of the stack, above the folder. */
  expect_stack_names(*drop.dst, {"HostFolder", "Folder", "Added"});
  expect_row_names(drop.host_folder->children, {"Host"});
  MaterialPaintLayer *fresh_root = static_cast<MaterialPaintLayer *>(drop.dst->paint_layers.last);
  ASSERT_NE(fresh_root, nullptr);
  EXPECT_STREQ(fresh_root->name, "Added");
  EXPECT_EQ(BKE_paint_layers_asset_instance_of_row(*drop.dst, *fresh_root->prev),
            drop.instance_id);
}

TEST_F(PaintLayersAssetTest, smart_asset_create_refuses_foreign_and_non_stack_rows)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *other = BKE_material_add(bmain, "Other");
  MaterialPaintLayer *foreign = add_layer(*other, "Foreign", add_image("ForeignMap"));
  const MaterialPaintLayer *foreign_rows[1] = {foreign};
  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(foreign_rows, 1),
                                          PaintLayerAssetType::Material,
                                          "Foreign"),
            nullptr);

  MaterialPaintLayer detached{};
  const MaterialPaintLayer *detached_rows[1] = {&detached};
  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(detached_rows, 1),
                                          PaintLayerAssetType::Material,
                                          "Detached"),
            nullptr);
}

TEST_F(PaintLayersAssetTest, smart_asset_type_rejects_invalid_type_and_future_version)
{
  Material *asset = BKE_material_add(bmain, "Asset");
  BKE_paint_layers_asset_type_set(*asset, PaintLayerAssetType::Material);
  ASSERT_NE(asset->id.properties, nullptr);

  IDP_ReplaceInGroup(asset->id.properties,
                     bke::idprop::create("paint_asset_type", int(PaintLayerAssetType::Mask) + 1)
                         .release());
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::None);

  IDP_ReplaceInGroup(asset->id.properties,
                     bke::idprop::create("paint_asset_type", int(PaintLayerAssetType::Material))
                         .release());
  IDP_ReplaceInGroup(
      asset->id.properties, bke::idprop::create("paint_asset_version", 2).release());
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::None);
}

TEST_F(PaintLayersAssetTest, import_rows_preserves_source_order_in_every_place)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *a = add_layer(*src, "A", add_image("MapA"));
  MaterialPaintLayer *b = add_layer(*src, "B", add_image("MapB"));
  MaterialPaintLayer *c = add_layer(*src, "C", add_image("MapC"));
  const MaterialPaintLayer *source_rows[3] = {a, b, c};
  const Span<const MaterialPaintLayer *> rows(source_rows, 3);

  Material *above = BKE_material_add(bmain, "Above");
  MaterialPaintLayer *anchor = add_layer(*above, "Anchor", add_image("AboveMap"));
  PaintLayerImportParams params;
  params.anchor = anchor;
  params.place = PaintLayerPlace::Above;
  ASSERT_TRUE(BKE_paint_layers_import_rows(*bmain, *above, *src, rows, params, nullptr));
  expect_stack_names(*above, {"Anchor", "A", "B", "C"});

  Material *below = BKE_material_add(bmain, "Below");
  anchor = add_layer(*below, "Anchor", add_image("BelowMap"));
  params.anchor = anchor;
  params.place = PaintLayerPlace::Below;
  ASSERT_TRUE(BKE_paint_layers_import_rows(*bmain, *below, *src, rows, params, nullptr));
  expect_stack_names(*below, {"A", "B", "C", "Anchor"});

  Material *into = BKE_material_add(bmain, "Into");
  MaterialPaintLayer *member = add_layer(*into, "Member", add_image("IntoMap"));
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *into, Span<MaterialPaintLayer *>(&member, 1));
  ASSERT_NE(folder, nullptr);
  params.anchor = folder;
  params.place = PaintLayerPlace::Into;
  ASSERT_TRUE(BKE_paint_layers_import_rows(*bmain, *into, *src, rows, params, nullptr));
  expect_row_names(folder->children, {"Member", "A", "B", "C"});

  Material *top = BKE_material_add(bmain, "Top");
  params.anchor = nullptr;
  params.place = PaintLayerPlace::Above;
  ASSERT_TRUE(BKE_paint_layers_import_rows(*bmain, *top, *src, rows, params, nullptr));
  expect_stack_names(*top, {"A", "B", "C"});
}

TEST_F(PaintLayersAssetTest, import_rows_needs_above_when_there_is_no_anchor)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *row = add_layer(*src, "A", add_image("MapA"));
  const MaterialPaintLayer *source_rows[1] = {row};

  PaintLayerImportParams params;
  params.anchor = nullptr;
  params.place = PaintLayerPlace::Below;
  EXPECT_FALSE(BKE_paint_layers_import_rows(
      *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, nullptr));
  params.place = PaintLayerPlace::Into;
  EXPECT_FALSE(BKE_paint_layers_import_rows(
      *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, nullptr));
}

TEST_F(PaintLayersAssetTest, smart_layer_create_refuses_more_than_one_row)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *a = add_layer(*src, "A", add_image("MapA"));
  MaterialPaintLayer *b = add_layer(*src, "B", add_image("MapB"));
  const MaterialPaintLayer *source_rows[2] = {a, b};
  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(source_rows, 2),
                                          PaintLayerAssetType::Layer,
                                          "TwoRows"),
            nullptr);
}

TEST_F(PaintLayersAssetTest, smart_asset_can_insert_and_insert_match)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *a = add_layer(*src, "A", add_image("MapA"));
  MaterialPaintLayer *b = add_layer(*src, "B", add_image("MapB"));
  const MaterialPaintLayer *source_rows[2] = {a, b};
  const Span<const MaterialPaintLayer *> rows(source_rows, 2);
  Material *asset = BKE_paint_layers_asset_create(
      *bmain, *src, rows, PaintLayerAssetType::Material, "SmartMaterial");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *anchor = add_layer(*dst, "Anchor", add_image("DstMap"));
  const char *reason = nullptr;
  EXPECT_TRUE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, anchor, PaintLayerPlace::Above, &reason));
  EXPECT_EQ(reason, nullptr);
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, anchor, PaintLayerPlace::Into, &reason));
  EXPECT_NE(reason, nullptr);
  reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, nullptr, PaintLayerPlace::Into, &reason));
  EXPECT_NE(reason, nullptr);

  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, anchor, PaintLayerPlace::Above, &result));
  EXPECT_EQ(result.rows, 2);
  ASSERT_NE(result.first, nullptr);
  expect_stack_names(*dst, {"Anchor", "A", "B"});
}

TEST_F(PaintLayersAssetTest, smart_asset_can_insert_refuses_empty_and_foreign_assets)
{
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *anchor = add_layer(*dst, "Anchor", add_image("DstMap"));

  Material *empty = BKE_material_add(bmain, "Empty");
  BKE_paint_layers_asset_type_set(*empty, PaintLayerAssetType::Material);
  const char *reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *dst, *empty, anchor, PaintLayerPlace::Above, &reason));
  EXPECT_NE(reason, nullptr);

  Material *ordinary = BKE_material_add(bmain, "Ordinary");
  reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *dst, *ordinary, anchor, PaintLayerPlace::Above, &reason));
  EXPECT_NE(reason, nullptr);
}

TEST_F(PaintLayersAssetTest, smart_mask_holder_and_insert_use_its_mask)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *source_row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, source_row, 0.25f), nullptr);
  const MaterialPaintLayer *source_rows[1] = {source_row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Mask,
                                                  "SmartMask");
  ASSERT_NE(asset, nullptr);
  const MaterialPaintLayer *holder = BKE_paint_layers_asset_mask_holder_find(*asset);
  ASSERT_NE(holder, nullptr);
  ASSERT_NE(holder->mask_stack.first, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *anchor = add_layer(*dst, "Anchor", add_image("DstMap"));
  const char *reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, nullptr, PaintLayerPlace::Above, &reason));
  EXPECT_NE(reason, nullptr);
  EXPECT_TRUE(BKE_paint_layers_asset_can_insert(
      *dst, *asset, anchor, PaintLayerPlace::Above, nullptr));

  PaintLayerImportResult result;
  EXPECT_TRUE(
      BKE_paint_layers_asset_insert(*bmain, *dst, *asset, anchor, PaintLayerPlace::Above, &result));
  EXPECT_EQ(result.rows, 1);
  EXPECT_EQ(result.first, anchor);
  ASSERT_NE(anchor->mask_stack.first, nullptr);
}

TEST_F(PaintLayersAssetTest, smart_asset_insert_refuses_into_a_plain_row_without_changing_dst)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(
      *src, "Height", add_image("HeightMap"), PAINT_MATERIAL_CHANNEL_HEIGHT);
  const MaterialPaintLayer *source_rows[1] = {row};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "SmartMaterial");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *anchor = add_layer(*dst, "Anchor", add_image("DstMap"));
  const int rows_before = BLI_listbase_count(&dst->paint_layers);
  const uint16_t channels_before = BKE_paint_layers_channel_set_mask_get(*dst);

  PaintLayerImportResult result;
  EXPECT_FALSE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, anchor, PaintLayerPlace::Into, &result));
  EXPECT_EQ(BLI_listbase_count(&dst->paint_layers), rows_before);
  EXPECT_EQ(BKE_paint_layers_channel_set_mask_get(*dst), channels_before);
}

TEST_F(PaintLayersAssetTest, smart_asset_type_clear_removes_custom_and_metadata_tags)
{
  Material *asset = BKE_material_add(bmain, "Asset");
  asset->id.asset_data = BKE_asset_metadata_create();
  BKE_paint_layers_asset_type_set(*asset, PaintLayerAssetType::Material);
  ASSERT_NE(BKE_asset_metadata_idprop_find(asset->id.asset_data, "paint_asset_type"), nullptr);

  BKE_paint_layers_asset_type_clear(*asset);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::None);
  EXPECT_EQ(IDP_GetPropertyFromGroup(asset->id.properties, "paint_asset_type"), nullptr);
  EXPECT_EQ(IDP_GetPropertyFromGroup(asset->id.properties, "paint_asset_version"), nullptr);
  EXPECT_EQ(BKE_asset_metadata_idprop_find(asset->id.asset_data, "paint_asset_type"), nullptr);
  EXPECT_EQ(BKE_asset_metadata_idprop_find(asset->id.asset_data, "paint_asset_version"), nullptr);

  BKE_asset_metadata_free(&asset->id.asset_data);
}

TEST_F(PaintLayersAssetTest, smart_asset_type_survives_clearing_asset_metadata)
{
  Material *asset = BKE_material_add(bmain, "Asset");
  asset->id.asset_data = BKE_asset_metadata_create();
  BKE_paint_layers_asset_type_set(*asset, PaintLayerAssetType::Layer);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Layer);

  /* "Clear Asset" drops the metadata, not the custom-property tag that makes an appended asset
   * keep its kind. */
  BKE_asset_metadata_free(&asset->id.asset_data);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Layer);

  BKE_paint_layers_asset_type_clear(*asset);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::None);
  EXPECT_EQ(IDP_GetPropertyFromGroup(asset->id.properties, "paint_asset_type"), nullptr);
  EXPECT_EQ(IDP_GetPropertyFromGroup(asset->id.properties, "paint_asset_version"), nullptr);
}

TEST_F(PaintLayersAssetTest, smart_asset_type_labels_are_the_user_facing_names)
{
  EXPECT_STREQ(BKE_paint_layers_asset_type_label(PaintLayerAssetType::Material),
               "Smart Material");
  EXPECT_STREQ(BKE_paint_layers_asset_type_label(PaintLayerAssetType::Layer), "Smart Layer");
  EXPECT_STREQ(BKE_paint_layers_asset_type_label(PaintLayerAssetType::Mask), "Smart Mask");
}

TEST_F(PaintLayersAssetTest, smart_asset_create_refuses_curve_patch_without_leaking_a_material)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Curve", add_image("CurveMap"));
  ASSERT_NE(BKE_paint_layers_curve_patch_effect_add(*src, row, "Patch", *bmain, nullptr),
            nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};
  const int materials_before = BLI_listbase_count(&bmain->materials);

  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(source_rows, 1),
                                          PaintLayerAssetType::Material,
                                          "Blocked"),
            nullptr);
  EXPECT_EQ(BLI_listbase_count(&bmain->materials), materials_before);
}

TEST_F(PaintLayersAssetTest, smart_asset_create_refuses_curve_patch_inside_a_folder)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *member = add_layer(*src, "Member", add_image("MemberMap"));
  MaterialPaintLayer *folder = BKE_paint_layers_group(
      *src, Span<MaterialPaintLayer *>(&member, 1));
  ASSERT_NE(folder, nullptr);
  ASSERT_NE(BKE_paint_layers_curve_patch_effect_add(*src, folder, "Patch", *bmain, nullptr),
            nullptr);
  const MaterialPaintLayer *source_rows[1] = {folder};
  const int materials_before = BLI_listbase_count(&bmain->materials);

  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(source_rows, 1),
                                          PaintLayerAssetType::Layer,
                                          "BlockedFolder"),
            nullptr);
  EXPECT_EQ(BLI_listbase_count(&bmain->materials), materials_before);
}

TEST_F(PaintLayersAssetTest, smart_mask_ignores_curve_patch_in_the_row_content)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Masked", add_image("MaskedMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, row, 0.5f), nullptr);
  ASSERT_NE(BKE_paint_layers_curve_patch_effect_add(*src, row, "Patch", *bmain, nullptr),
            nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Mask,
                                                  "MaskOnly");
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Mask);
}

TEST_F(PaintLayersAssetTest, smart_asset_insert_skips_curve_patch_effects)
{
  Material *asset = BKE_material_add(bmain, "LegacyAsset");
  MaterialPaintLayer *row = add_layer(*asset, "Row", add_image("RowMap"));
  ASSERT_NE(BKE_paint_layers_curve_patch_effect_add(*asset, row, "Patch", *bmain, nullptr),
            nullptr);
  ASSERT_NE(BKE_paint_layers_correction_add(
                *asset, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT),
            nullptr);
  BKE_paint_layers_asset_type_set(*asset, PaintLayerAssetType::Material);

  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *anchor = add_layer(*dst, "Anchor", add_image("DstMap"));
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_asset_insert(
      *bmain, *dst, *asset, anchor, PaintLayerPlace::Above, &result));
  EXPECT_EQ(result.dropped_curve_patches, 1);
  ASSERT_NE(result.first, nullptr);
  EXPECT_EQ(BLI_listbase_count(&result.first->effects), 1);
  const MaterialPaintLayer *effect = static_cast<const MaterialPaintLayer *>(
      result.first->effects.first);
  EXPECT_STREQ(effect->name, "Fill");
}

TEST_F(PaintLayersAssetTest, portability_counts_a_reader_pointed_outside_the_saved_rows)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved = add_layer(*src, "Saved", add_image("SavedMap"));
  MaterialPaintLayer *outside = add_layer(*src, "Outside", add_image("OutsideMap"));
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(*src,
                                                                outside,
                                                                PaintLayerAnchorPointList::Effects,
                                                                nullptr,
                                                                false,
                                                                "Outside Point",
                                                                {});
  ASSERT_NE(point, nullptr);
  ASSERT_NE(BKE_paint_layers_anchor_point_add(*src,
                                              saved,
                                              PaintLayerAnchorPointList::Effects,
                                              nullptr,
                                              true,
                                              "Outside Reader",
                                              point->marker),
            nullptr);

  const MaterialPaintLayer *saved_rows[1] = {saved};
  EXPECT_EQ(BKE_paint_layers_asset_portability_check(
                *src, Span<const MaterialPaintLayer *>(saved_rows, 1), PaintLayerAssetType::Layer)
                .foreign_reader_num,
            1);
}

TEST_F(PaintLayersAssetTest, portability_accepts_a_reader_pointed_inside_the_saved_rows)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *saved = add_layer(*src, "Saved", add_image("SavedMap"));
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(*src,
                                                                saved,
                                                                PaintLayerAnchorPointList::Effects,
                                                                nullptr,
                                                                false,
                                                                "Point",
                                                                {});
  ASSERT_NE(point, nullptr);
  ASSERT_NE(BKE_paint_layers_anchor_point_add(*src,
                                              saved,
                                              PaintLayerAnchorPointList::Effects,
                                              nullptr,
                                              true,
                                              "Reader",
                                              point->marker),
            nullptr);

  const MaterialPaintLayer *saved_rows[1] = {saved};
  EXPECT_EQ(BKE_paint_layers_asset_portability_check(
                *src, Span<const MaterialPaintLayer *>(saved_rows, 1), PaintLayerAssetType::Layer)
                .foreign_reader_num,
            0);
}

TEST_F(PaintLayersAssetTest, portability_counts_file_images_but_not_generated_ones)
{
  Material *src = BKE_material_add(bmain, "Src");
  Image *generated = add_image("Generated");
  MaterialPaintLayer *generated_row = add_layer(*src, "Generated", generated);
  const MaterialPaintLayer *generated_rows[1] = {generated_row};
  const PaintLayerAssetPortability generated_check = BKE_paint_layers_asset_portability_check(
      *src,
      Span<const MaterialPaintLayer *>(generated_rows, 1),
      PaintLayerAssetType::Layer);
  EXPECT_EQ(generated_check.external_image_num, 0);

  Image *file = add_image("File");
  file->source = IMA_SRC_FILE;
  file->packedfiles.clear_no_delete();
  MaterialPaintLayer *file_row = add_layer(*src, "File", file);
  const MaterialPaintLayer *file_rows[1] = {file_row};
  const PaintLayerAssetPortability file_check = BKE_paint_layers_asset_portability_check(
      *src, Span<const MaterialPaintLayer *>(file_rows, 1), PaintLayerAssetType::Layer);
  EXPECT_EQ(file_check.external_image_num, 1);
}

TEST_F(PaintLayersAssetTest, ordinary_duplicate_still_copies_the_curve_patch_payload)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Curve", add_image("CurveMap"));
  MaterialPaintLayer *effect = BKE_paint_layers_curve_patch_effect_add(
      *src, row, "Patch", *bmain, nullptr);
  ASSERT_NE(effect, nullptr);

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *src, row);
  ASSERT_NE(copy, nullptr);
  ASSERT_EQ(BLI_listbase_count(&copy->effects), 1);
  MaterialPaintLayer *effect_copy = static_cast<MaterialPaintLayer *>(copy->effects.first);
  EXPECT_EQ(effect_copy->source, MA_PAINT_LAYER_SOURCE_CURVE_PATCH);
  ASSERT_NE(effect_copy->curve_patch, nullptr);
  EXPECT_NE(effect_copy->curve_patch, effect->curve_patch);
  EXPECT_NE(effect_copy->curve_patch->vectors, effect->curve_patch->vectors);
  EXPECT_NE(effect_copy->curve_patch->brush, effect->curve_patch->brush);
}

TEST_F(PaintLayersAssetTest, smart_asset_default_catalog_follows_the_asset_type)
{
  EXPECT_STREQ(BKE_paint_layers_asset_default_catalog(PaintLayerAssetType::Material),
               "Paint/Smart Materials");
  EXPECT_STREQ(BKE_paint_layers_asset_default_catalog(PaintLayerAssetType::Layer),
               "Paint/Smart Layers");
  EXPECT_STREQ(BKE_paint_layers_asset_default_catalog(PaintLayerAssetType::Mask),
               "Paint/Smart Masks");
  EXPECT_STREQ(BKE_paint_layers_asset_default_catalog(PaintLayerAssetType::None), "");
}

TEST_F(PaintLayersAssetTest, saving_the_same_name_again_keeps_both_assets)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *first = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "SmartSameName");
  ASSERT_NE(first, nullptr);
  Material *second = BKE_paint_layers_asset_create(*bmain,
                                                   *src,
                                                   Span<const MaterialPaintLayer *>(source_rows, 1),
                                                   PaintLayerAssetType::Material,
                                                   "SmartSameName");
  ASSERT_NE(second, nullptr);

  EXPECT_NE(second, first);
  EXPECT_STRNE(second->id.name, first->id.name);
  EXPECT_EQ(BLI_listbase_count(&first->paint_layers), 1);
  EXPECT_EQ(BLI_listbase_count(&second->paint_layers), 1);
  EXPECT_NE(first->paint_layers.first, second->paint_layers.first);

  /* Both assets land in the same catalog and keep it: a repeated name uniquifies the ID, not the
   * catalog. */
  first->id.asset_data = BKE_asset_metadata_create();
  second->id.asset_data = BKE_asset_metadata_create();
  const bUUID catalog_id = BLI_uuid_generate_random();
  BKE_paint_layers_asset_metadata_apply(
      *first, PaintLayerAssetType::Material, "First", catalog_id, "Smart Materials");
  BKE_paint_layers_asset_metadata_apply(
      *second, PaintLayerAssetType::Material, "Second", catalog_id, "Smart Materials");
  EXPECT_TRUE(BLI_uuid_equal(first->id.asset_data->catalog_id, catalog_id));
  EXPECT_TRUE(BLI_uuid_equal(second->id.asset_data->catalog_id, catalog_id));
  EXPECT_STREQ(first->id.asset_data->catalog_simple_name, "Smart Materials");
  EXPECT_STREQ(second->id.asset_data->catalog_simple_name, "Smart Materials");

  BKE_asset_metadata_free(&first->id.asset_data);
  BKE_asset_metadata_free(&second->id.asset_data);
}

TEST_F(PaintLayersAssetTest, smart_asset_metadata_sets_catalog_type_and_tags)
{
  Material *asset = BKE_material_add(bmain, "Asset");
  asset->id.asset_data = BKE_asset_metadata_create();
  const bUUID catalog_id = BLI_uuid_generate_random();

  BKE_paint_layers_asset_metadata_apply(*asset,
                                        PaintLayerAssetType::Layer,
                                        "A layer over a neutral fill",
                                        catalog_id,
                                        "Smart Layers");

  AssetMetaData *meta = asset->id.asset_data;
  ASSERT_NE(meta, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(meta->catalog_id, catalog_id));
  EXPECT_STREQ(meta->catalog_simple_name, "Smart Layers");
  EXPECT_STREQ(meta->description, "A layer over a neutral fill");
  EXPECT_EQ(BKE_paint_layers_asset_type_get(*asset), PaintLayerAssetType::Layer);
  EXPECT_NE(BKE_asset_metadata_idprop_find(meta, "paint_asset_type"), nullptr);
  EXPECT_EQ(asset_tag_count(*meta, "Paint"), 1);
  EXPECT_EQ(asset_tag_count(*meta, "Smart Layer"), 1);

  BKE_asset_metadata_free(&asset->id.asset_data);
}

TEST_F(PaintLayersAssetTest, smart_asset_metadata_defaults_description_and_keeps_tags_unique)
{
  Material *asset = BKE_material_add(bmain, "Asset");
  asset->id.asset_data = BKE_asset_metadata_create();
  const bUUID catalog_id = BLI_uuid_generate_random();

  BKE_paint_layers_asset_metadata_apply(
      *asset, PaintLayerAssetType::Material, "", catalog_id, "Smart Materials");
  AssetMetaData *meta = asset->id.asset_data;
  ASSERT_NE(meta, nullptr);
  EXPECT_STREQ(meta->description,
               "Smart paint asset saved from a layered material's stack");
  const int tags_before = meta->tot_tags;

  BKE_paint_layers_asset_metadata_apply(
      *asset, PaintLayerAssetType::Material, "", catalog_id, "Smart Materials");
  EXPECT_EQ(meta->tot_tags, tags_before);
  EXPECT_EQ(asset_tag_count(*meta, "Paint"), 1);
  EXPECT_EQ(asset_tag_count(*meta, "Smart Material"), 1);
  EXPECT_TRUE(BLI_uuid_equal(meta->catalog_id, catalog_id));

  BKE_asset_metadata_free(&asset->id.asset_data);
}

TEST_F(PaintLayersAssetTest, import_strip_leaves_painted_maps_as_flat_values)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  Image *image = add_image("PaintedMap");
  MaterialPaintLayer *row = add_layer(*src, "Layer", image);
  const float flat[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *src, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, flat));

  const int images_before = BLI_listbase_count(&bmain->images);
  const int source_users_before = image->id.us;

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  params.image_policy = PaintLayerImageImportPolicy::Strip;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  /* A stripped map creates no image and takes no user: the record keeps its flat value alone. */
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
  EXPECT_EQ(image->id.us, source_users_before);
  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  ASSERT_EQ(copy->channels_num, 1);
  EXPECT_EQ(copy->channels[0].image, nullptr);
  for (const int i : IndexRange(4)) {
    EXPECT_FLOAT_EQ(copy->channels[0].value[i], flat[i]);
  }
  EXPECT_EQ(result.stripped_maps, 1);
}

TEST_F(PaintLayersAssetTest, import_share_reads_an_external_map_by_reference)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  Image *image = add_image("FileMap");
  /* A file-source image that was not painted on reads pixels the file keeps: external. */
  image->source = IMA_SRC_FILE;
  MaterialPaintLayer *row = add_layer(*src, "Layer", image);

  const int images_before = BLI_listbase_count(&bmain->images);
  const int source_users_before = image->id.us;

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  params.image_policy = PaintLayerImageImportPolicy::Share;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  ASSERT_EQ(copy->channels_num, 1);
  EXPECT_EQ(copy->channels[0].image, image);
  EXPECT_EQ(image->id.us, source_users_before + 1);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
  EXPECT_EQ(result.stripped_maps, 0);
}

TEST_F(PaintLayersAssetTest, import_share_copies_a_map_that_is_an_asset_itself)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  Image *image = add_image("ImageAsset");
  image->source = IMA_SRC_FILE;
  image->id.asset_data = BKE_asset_metadata_create();
  MaterialPaintLayer *row = add_layer(*src, "Layer", image);
  const int source_users_before = image->id.us;

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  params.image_policy = PaintLayerImageImportPolicy::Share;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, nullptr));

  /* An image asset is never shared with the importing material: the copy is a plain image, so a
   * Smart asset written to a library holds its data and brings no second asset along. */
  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  ASSERT_EQ(copy->channels_num, 1);
  Image *copied_image = copy->channels[0].image;
  ASSERT_NE(copied_image, nullptr);
  EXPECT_NE(copied_image, image);
  EXPECT_EQ(copied_image->id.asset_data, nullptr);
  EXPECT_EQ(image->id.us, source_users_before);
}

TEST_F(PaintLayersAssetTest, import_copy_keeps_copies_painted_maps_with_their_pixels)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  Image *image = add_image("PaintedMap");
  MaterialPaintLayer *row = add_layer(*src, "Layer", image);
  /* A texel the copy has to carry: the bare image settings would reproduce the generated blank. */
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->byte_data_for_write(), nullptr);
    ibuf->byte_data_for_write()[0] = 200;
    ibuf->byte_data_for_write()[1] = 100;
    ibuf->byte_data_for_write()[2] = 50;
    ibuf->byte_data_for_write()[3] = 255;
    BKE_image_release_ibuf(image, ibuf, lock);
  }

  const int images_before = BLI_listbase_count(&bmain->images);
  const int source_users_before = image->id.us;

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  params.image_policy = PaintLayerImageImportPolicy::Copy;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  ASSERT_EQ(copy->channels_num, 1);
  Image *copied_image = copy->channels[0].image;
  ASSERT_NE(copied_image, nullptr);
  EXPECT_NE(copied_image, image);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before + 1);
  EXPECT_EQ(copied_image->id.us, 1);
  EXPECT_EQ(image->id.us, source_users_before);
  EXPECT_EQ(result.stripped_maps, 0);

  void *copy_lock = nullptr;
  ImBuf *copy_ibuf = BKE_image_acquire_ibuf(copied_image, nullptr, &copy_lock);
  ASSERT_NE(copy_ibuf, nullptr);
  const uchar *copied_px = copy_ibuf->byte_data();
  ASSERT_NE(copied_px, nullptr);
  EXPECT_EQ(copied_px[0], 200);
  EXPECT_EQ(copied_px[1], 100);
  EXPECT_EQ(copied_px[2], 50);
  EXPECT_EQ(copied_px[3], 255);
  BKE_image_release_ibuf(copied_image, copy_ibuf, copy_lock);
}

TEST_F(PaintLayersAssetTest, import_mask_strip_reports_the_stripped_items)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *source_row = add_layer(*src, "Source", add_image("SourceMap"));
  MaterialPaintLayer *source_mask = BKE_paint_layers_mask_add(*src, source_row, 0.25f);
  ASSERT_NE(source_mask, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *src, source_mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("MaskMap")));
  MaterialPaintLayer *target = add_layer(*dst, "Target", add_image("TargetMap"));

  const MaterialPaintLayer *source_rows[1] = {source_row};
  PaintLayerImportParams params;
  params.mode = PaintLayerImportMode::Mask;
  params.anchor = target;
  params.image_policy = PaintLayerImageImportPolicy::Strip;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  EXPECT_EQ(result.stripped_maps, 1);
  ASSERT_NE(target->mask_stack.first, nullptr);
  MaterialPaintLayer *item = static_cast<MaterialPaintLayer *>(target->mask_stack.first);
  ASSERT_GE(item->channels_num, 1);
  EXPECT_EQ(item->channels[0].image, nullptr);
}

TEST_F(PaintLayersAssetTest, smart_mask_strip_keeps_a_fill_correction_texture)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, row, 0.25f), nullptr);
  MaterialPaintLayer *fill = BKE_paint_layers_mask_item_add(
      *src, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill Correction");
  ASSERT_NE(fill, nullptr);
  Image *texture = add_image("FillTexture");
  /* A texel the asset's copy has to carry. */
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(texture, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->byte_data_for_write(), nullptr);
    ibuf->byte_data_for_write()[0] = 10;
    ibuf->byte_data_for_write()[1] = 20;
    ibuf->byte_data_for_write()[2] = 30;
    ibuf->byte_data_for_write()[3] = 255;
    BKE_image_release_ibuf(texture, ibuf, lock);
  }
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *src, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, texture));

  const int images_before = BLI_listbase_count(&bmain->images);
  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportResult result;
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Mask,
                                                  "SmartMaskFill",
                                                  PaintLayerImageImportPolicy::Strip,
                                                  &result);
  ASSERT_NE(asset, nullptr);
  /* The Fill correction's map is a texture, not painted data: stripping leaves it in the asset. */
  EXPECT_EQ(result.stripped_maps, 0);
  ASSERT_EQ(BLI_listbase_count(&asset->paint_layers), 1);
  MaterialPaintLayer *holder = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  MaterialPaintLayer *imported_fill = nullptr;
  for (MaterialPaintLayer &item : holder->mask_stack) {
    if (STREQ(item.name, "Fill Correction")) {
      imported_fill = &item;
    }
  }
  ASSERT_NE(imported_fill, nullptr);
  ASSERT_EQ(imported_fill->channels_num, 1);
  Image *copied = imported_fill->channels[0].image;
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied, texture);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before + 1);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(copied, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *pixels = ibuf->byte_data();
  ASSERT_NE(pixels, nullptr);
  EXPECT_EQ(pixels[0], 10);
  EXPECT_EQ(pixels[1], 20);
  EXPECT_EQ(pixels[2], 30);
  EXPECT_EQ(pixels[3], 255);
  BKE_image_release_ibuf(copied, ibuf, lock);
}

TEST_F(PaintLayersAssetTest, smart_mask_strip_still_drops_a_painted_mask_correction)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, row, 0.25f), nullptr);
  MaterialPaintLayer *painted = BKE_paint_layers_mask_item_add(
      *src, row, MA_PAINT_LAYER_SOURCE_IMAGE, "Painted Correction");
  ASSERT_NE(painted, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *src, painted, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("PaintedMaskMap")));

  const int images_before = BLI_listbase_count(&bmain->images);
  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportResult result;
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Mask,
                                                  "SmartMaskPainted",
                                                  PaintLayerImageImportPolicy::Strip,
                                                  &result);
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(result.stripped_maps, 1);
  EXPECT_EQ(BLI_listbase_count(&bmain->images), images_before);
}

TEST_F(PaintLayersAssetTest, smart_material_share_copies_an_assigned_fill_texture)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = BKE_paint_layers_add(*src, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  row->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  Image *texture = add_image("FillTexture");
  ASSERT_NE(BKE_paint_layers_channel_add(*src, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *src, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, texture));

  const MaterialPaintLayer *source_rows[1] = {row};
  PaintLayerImportResult result;
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "SmartFill",
                                                  PaintLayerImageImportPolicy::Share,
                                                  &result);
  ASSERT_NE(asset, nullptr);
  MaterialPaintLayer *copy = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  ASSERT_EQ(copy->channels_num, 1);
  ASSERT_NE(copy->channels[0].image, nullptr);
  EXPECT_NE(copy->channels[0].image, texture);
  EXPECT_EQ(result.stripped_maps, 0);
}

TEST_F(PaintLayersAssetTest, import_mask_append_keeps_the_target_base)
{
  Material *src = BKE_material_add(bmain, "Src");
  Material *dst = BKE_material_add(bmain, "Dst");
  MaterialPaintLayer *source_row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, source_row, 0.25f), nullptr);
  ASSERT_NE(BKE_paint_layers_mask_item_add(
                *src, source_row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Source Correction"),
            nullptr);
  MaterialPaintLayer *target = add_layer(*dst, "Target", add_image("TargetMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*dst, target, 0.75f), nullptr);
  ASSERT_NE(BKE_paint_layers_mask_item_add(
                *dst, target, MA_PAINT_LAYER_SOURCE_CONSTANT, "Target Correction"),
            nullptr);

  const int target_count = BLI_listbase_count(&target->mask_stack);
  const int source_count = BLI_listbase_count(&source_row->mask_stack);
  MaterialPaintLayer *target_base = BKE_paint_layers_mask_base(*target);
  ASSERT_NE(target_base, nullptr);
  const bUUID target_base_marker = target_base->marker;

  const MaterialPaintLayer *source_rows[1] = {source_row};
  PaintLayerImportParams params;
  params.mode = PaintLayerImportMode::Mask;
  params.anchor = target;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS | PAINT_LAYER_IMPORT_APPEND_MASK;
  PaintLayerImportResult result;
  ASSERT_TRUE(
      BKE_paint_layers_import_rows(
          *bmain, *dst, *src, Span<const MaterialPaintLayer *>(source_rows, 1), params, &result));

  EXPECT_EQ(result.rows, source_count);
  ASSERT_EQ(BLI_listbase_count(&target->mask_stack), target_count + source_count);
  MaterialPaintLayer *base_after = BKE_paint_layers_mask_base(*target);
  ASSERT_NE(base_after, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(base_after->marker, target_base_marker));

  int index = 0;
  for (MaterialPaintLayer &item : target->mask_stack) {
    if (index >= target_count) {
      EXPECT_TRUE((item.flag & MA_PAINT_LAYER_MASK_BASE) == 0);
    }
    index++;
  }
}

TEST_F(PaintLayersAssetTest, smart_material_folder_keeps_anchor_points)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row_a = add_layer(*src, "RowA", add_image("MapA"));
  MaterialPaintLayer *row_b = add_layer(*src, "RowB", add_image("MapB"));

  MaterialPaintLayer *group[2] = {row_a, row_b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*src, Span<MaterialPaintLayer *>(group, 2));
  ASSERT_NE(folder, nullptr);

  MaterialPaintLayer *marker = BKE_paint_layers_anchor_point_add(
      *src, row_a, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
  ASSERT_NE(marker, nullptr);
  MaterialPaintLayer *reader = BKE_paint_layers_anchor_point_add(*src,
                                                                folder,
                                                                PaintLayerAnchorPointList::Row,
                                                                row_a,
                                                                true,
                                                                "Reader",
                                                                marker->marker);
  ASSERT_NE(reader, nullptr);

  const MaterialPaintLayer *source_rows[1] = {folder};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  *src,
                                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                                  PaintLayerAssetType::Material,
                                                  "SmartFolderTest");
  ASSERT_NE(asset, nullptr);

  MaterialPaintLayer *copied_reader = nullptr;
  MaterialPaintLayer *copied_marker = nullptr;
  BKE_paint_layers_foreach(
      *asset, [&](MaterialPaintLayer &row, MaterialPaintLayer * /*parent*/) -> bool {
        if (STREQ(row.name, "Reader")) {
          copied_reader = &row;
        }
        if (STREQ(row.name, "RowA")) {
          for (MaterialPaintLayer &effect : row.effects) {
            if (STREQ(effect.name, "Point")) {
              copied_marker = &effect;
            }
          }
        }
        return true;
      });
  ASSERT_NE(copied_reader, nullptr);
  ASSERT_NE(copied_marker, nullptr);
  EXPECT_FALSE(BLI_uuid_equal(copied_marker->marker, marker->marker));
  EXPECT_TRUE(
      BLI_uuid_equal(BKE_paint_layers_anchor_point_source_get(*copied_reader),
                     copied_marker->marker));
}

TEST_F(PaintLayersAssetTest, smart_mask_refuses_an_anchor_point_in_the_mask)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, row, 0.25f), nullptr);
  const PaintLayerAnchorPointList mask_stack = PaintLayerAnchorPointList::MaskStack;
  ASSERT_NE(BKE_paint_layers_anchor_point_add(
                *src, row, mask_stack, nullptr, false, "Mask Point", {}),
            nullptr);
  EXPECT_TRUE(BKE_paint_layers_mask_branch_has_anchor(*row));

  const MaterialPaintLayer *source_rows[1] = {row};
  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(source_rows, 1),
                                          PaintLayerAssetType::Mask,
                                          "SmartMaskAnchor"),
            nullptr);
}

TEST_F(PaintLayersAssetTest, smart_mask_refuses_an_anchor_point_reader_in_the_mask)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Source", add_image("SourceMap"));
  ASSERT_NE(BKE_paint_layers_mask_add(*src, row, 0.25f), nullptr);
  const PaintLayerAnchorPointList mask_stack = PaintLayerAnchorPointList::MaskStack;
  MaterialPaintLayer *marker = BKE_paint_layers_anchor_point_add(
      *src, row, mask_stack, nullptr, false, "Mask Point", {});
  ASSERT_NE(marker, nullptr);
  MaterialPaintLayer *reader = BKE_paint_layers_anchor_point_add(
      *src, row, mask_stack, marker, true, "Mask Reader", marker->marker);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(BKE_paint_layers_mask_branch_has_anchor(*row));

  const MaterialPaintLayer *source_rows[1] = {row};
  EXPECT_EQ(BKE_paint_layers_asset_create(*bmain,
                                          *src,
                                          Span<const MaterialPaintLayer *>(source_rows, 1),
                                          PaintLayerAssetType::Mask,
                                          "SmartMaskReader"),
            nullptr);
}

TEST_F(PaintLayersAssetTest, unexposed_parameter_imports_as_a_plain_local)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartTrimmed");
  ASSERT_NE(asset, nullptr);
  /* The expose-everything build starts from every schema parameter. */
  EXPECT_TRUE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "gamma"));

  BKE_paint_layers_asset_param_expose_set(*asset, *tone, "gamma", false);
  EXPECT_FALSE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "gamma"));

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  MaterialPaintLayer *copy = result.imported[0];
  MaterialPaintLayer *tone_copy = first_effect(*copy);
  ASSERT_NE(tone_copy, nullptr);

  /* Only the still-exposed parameters carry their marks; the trimmed one imports as a plain
   * local, edited in the row's own place and bound to nothing. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*tone_copy, "in_black"));
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*tone_copy, "gamma"));
  ASSERT_NE(tone_copy->system_properties, nullptr);
  const IDProperty *marks = IDP_GetPropertyFromGroup(tone_copy->system_properties,
                                                     ASSET_PARAMS_PROP_NAME);
  ASSERT_NE(marks, nullptr);
  EXPECT_NE(IDP_GetPropertyFromGroup(marks, "in_black"), nullptr);
  EXPECT_EQ(IDP_GetPropertyFromGroup(marks, "gamma"), nullptr);
}

TEST_F(PaintLayersAssetTest, reexposing_restores_the_entry_with_its_schema_position)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartReexposed");
  ASSERT_NE(asset, nullptr);
  BKE_paint_layers_asset_param_expose_set(*asset, *tone, "gamma", false);

  BKE_paint_layers_asset_param_expose_set(*asset, *tone, "gamma", true);
  EXPECT_TRUE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "gamma"));
  const IDProperty *params = BKE_paint_layers_asset_params_get(*asset);
  const IDProperty *tone_entry = IDP_GetPropertyFromGroup(params, "Tone");
  ASSERT_NE(tone_entry, nullptr);
  const IDProperty *gamma = IDP_GetPropertyFromGroup(tone_entry, "gamma");
  ASSERT_NE(gamma, nullptr);
  EXPECT_EQ(IDP_int_get(gamma), PAINT_LAYER_LEVELS_GAMMA);

  /* Unexposing the last parameter of an item removes the item's group with it: the import walks
   * entries, and an empty one would only look like work to do. */
  const PaintLayerKindInfo *kind = paint_layer_kind_of(*tone);
  ASSERT_NE(kind, nullptr);
  for (const int i : IndexRange(kind->params_num)) {
    BKE_paint_layers_asset_param_expose_set(*asset, *tone, kind->params[i].name, false);
  }
  EXPECT_EQ(IDP_GetPropertyFromGroup(BKE_paint_layers_asset_params_get(*asset), "Tone"),
            nullptr);
}

TEST_F(PaintLayersAssetTest, expose_toggle_mirrors_into_the_asset_metadata)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartMirrored");
  ASSERT_NE(asset, nullptr);
  asset->id.asset_data = BKE_asset_metadata_create();
  BKE_paint_layers_asset_params_expose(*asset);

  BKE_paint_layers_asset_param_expose_set(*asset, *tone, "gamma", false);
  const IDProperty *mirrored = BKE_asset_metadata_idprop_find(asset->id.asset_data,
                                                             "paint_asset_params");
  ASSERT_NE(mirrored, nullptr);
  const IDProperty *mirrored_tone = IDP_GetPropertyFromGroup(mirrored, "Tone");
  ASSERT_NE(mirrored_tone, nullptr);
  EXPECT_NE(IDP_GetPropertyFromGroup(mirrored_tone, "in_black"), nullptr);
  EXPECT_EQ(IDP_GetPropertyFromGroup(mirrored_tone, "gamma"), nullptr);

  BKE_paint_layers_asset_param_expose_set(*asset, *tone, "gamma", true);
  mirrored = BKE_asset_metadata_idprop_find(asset->id.asset_data, "paint_asset_params");
  ASSERT_NE(mirrored, nullptr);
  mirrored_tone = IDP_GetPropertyFromGroup(mirrored, "Tone");
  ASSERT_NE(mirrored_tone, nullptr);
  EXPECT_NE(IDP_GetPropertyFromGroup(mirrored_tone, "gamma"), nullptr);
}

TEST_F(PaintLayersAssetTest, expose_toggle_on_a_material_without_a_list_is_a_noop)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);

  /* A plain material has nothing to select on: the toggle must not build a half list. */
  BKE_paint_layers_asset_param_expose_set(*src, *tone, "gamma", false);
  BKE_paint_layers_asset_param_expose_set(*src, *tone, "gamma", true);
  EXPECT_FALSE(BKE_paint_layers_asset_param_is_exposed(*src, *tone, "gamma"));
  EXPECT_EQ(IDP_GetPropertyFromGroup_null(src->id.properties, "paint_asset_params"), nullptr);
}

TEST_F(PaintLayersAssetTest, resaving_an_asset_keeps_the_manual_expose_choice)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartResaved");
  ASSERT_NE(asset, nullptr);
  /* The author trims one parameter on the asset's own panel. */
  BKE_paint_layers_asset_param_expose_set(*asset, *tone, "gamma", false);

  /* The re-save runs the full expose again on the material that already carries the list: the
   * entry the author trimmed survives, instead of being rebuilt with everything exposed. */
  BKE_paint_layers_asset_params_expose(*asset);
  EXPECT_TRUE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "in_black"));
  EXPECT_FALSE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "gamma"));

  /* An effect the author added since the last save is new to the list: it arrives with everything
   * exposed, and the old item's trimming stays. */
  MaterialPaintLayer *asset_row = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  MaterialPaintLayer *extra = add_levels_effect(*asset, *asset_row, "Extra");
  ASSERT_NE(extra, nullptr);
  BKE_paint_layers_asset_params_expose(*asset);
  EXPECT_TRUE(BKE_paint_layers_asset_param_is_exposed(*asset, *extra, "in_black"));
  EXPECT_FALSE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "gamma"));

  /* An item gone from the stack takes its entry with it. */
  BKE_paint_layers_remove(*asset, extra);
  BKE_paint_layers_asset_params_expose(*asset);
  EXPECT_EQ(IDP_GetPropertyFromGroup(BKE_paint_layers_asset_params_get(*asset), "Extra"),
            nullptr);
  EXPECT_FALSE(BKE_paint_layers_asset_param_is_exposed(*asset, *tone, "gamma"));
}

TEST_F(PaintLayersAssetTest, reload_drops_a_parameter_the_asset_unexposed)
{
  Material *src = BKE_material_add(bmain, "Src");
  MaterialPaintLayer *row = add_layer(*src, "Layer", add_image("SourceMap"));
  MaterialPaintLayer *tone = add_levels_effect(*src, *row, "Tone");
  ASSERT_NE(tone, nullptr);
  const MaterialPaintLayer *source_rows[1] = {row};

  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                  *src,
                                  Span<const MaterialPaintLayer *>(source_rows, 1),
                                  PaintLayerAssetType::Material,
                                  "SmartUnexposed");
  ASSERT_NE(asset, nullptr);

  Material *dst = BKE_material_add(bmain, "Dst");
  PaintLayerImportParams params;
  params.flags = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS;
  PaintLayerImportResult result;
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      *bmain, *dst, *asset, asset_top_rows(*asset).as_span(), params, &result));
  const int instance_id = BKE_paint_layers_asset_instance_record(
      *dst, *asset, PaintLayerAssetType::Material, result.imported.as_span());
  ASSERT_EQ(instance_id, 1);

  /* The instance changed both parameters; the asset then moved one and stopped exposing the
   * other. */
  MaterialPaintLayer *copy = result.imported[0];
  MaterialPaintLayer *tone_copy = first_effect(*copy);
  ASSERT_TRUE(paint_layer_kind_param_set(*tone_copy, PAINT_LAYER_LEVELS_IN_BLACK, 0.25f));
  ASSERT_TRUE(paint_layer_kind_param_set(*tone_copy, PAINT_LAYER_LEVELS_GAMMA, 2.0f));
  MaterialPaintLayer *asset_row = static_cast<MaterialPaintLayer *>(asset->paint_layers.first);
  MaterialPaintLayer *asset_tone = first_effect(*asset_row);
  ASSERT_NE(asset_tone, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_set(*asset_tone, PAINT_LAYER_LEVELS_IN_BLACK, 0.75f));
  ASSERT_TRUE(paint_layer_kind_param_set(*asset_tone, PAINT_LAYER_LEVELS_GAMMA, 0.5f));
  BKE_paint_layers_asset_param_expose_set(*asset, *asset_tone, "gamma", false);

  EXPECT_EQ(BKE_paint_layers_asset_reload(*bmain, *dst, instance_id),
            PaintLayerAssetReloadResult::Reloaded);

  /* The still-exposed value came over; the unexposed one follows the asset again. */
  MaterialPaintLayer *fresh = static_cast<MaterialPaintLayer *>(dst->paint_layers.first);
  MaterialPaintLayer *fresh_tone = first_effect(*fresh);
  ASSERT_NE(fresh_tone, nullptr);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fresh_tone, PAINT_LAYER_LEVELS_IN_BLACK), 0.25f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fresh_tone, PAINT_LAYER_LEVELS_GAMMA), 0.5f);
  /* The fresh marks bind what the asset still exposes: in_black only. */
  EXPECT_TRUE(BKE_paint_layers_item_param_is_exposed(*fresh_tone, "in_black"));
  EXPECT_FALSE(BKE_paint_layers_item_param_is_exposed(*fresh_tone, "gamma"));
}


}  // namespace blender::bke::tests
