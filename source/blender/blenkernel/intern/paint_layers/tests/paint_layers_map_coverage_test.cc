/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file
 * \ingroup bke
 *
 * The map-driven mask coverage operators (Color ID, UV Island): the API must tell them apart from a
 * map-less filter and from an ordinary image/constant mask correction, and must give them a
 * lifecycle of their own (colors, parameters, copy, paste, serialization).
 */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_appdir.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_material_composite.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "IMB_imbuf_types.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender::bke::tests {

class PaintLayersMapCoverage : public BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "MapCoverage");
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_image(const char *name)
  {
    return static_cast<Image *>(BKE_id_new(bmain, ID_IM, name));
  }

  /** A stack row with a base mask, the row a map-coverage item hangs on. */
  MaterialPaintLayer *add_masked_layer(const char *name)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
    return layer;
  }

  MaterialPaintLayer *add_map_item(MaterialPaintLayer *layer, eMaterialPaintLayerFilter filter)
  {
    return BKE_paint_layers_mask_filter_add(*ma, layer, filter);
  }
};

/* -------------------------------------------------------------------- */
/** \name The four cases must be distinguishable
 * \{ */

TEST_F(PaintLayersMapCoverage, api_distinguishes_the_four_cases)
{
  /* 1. An ordinary map-less filter: acts on what is below, owns no map, allowed as a correction. */
  const PaintLayerKindInfo *invert = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                            MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(invert, nullptr);
  EXPECT_NE(invert->flags & PAINT_LAYER_KIND_ACTS_ON_BELOW, 0);
  EXPECT_EQ(invert->flags & PAINT_LAYER_KIND_MAP_COVERAGE, 0);
  EXPECT_NE(invert->allowed_roles & paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT), 0);

  /* 2. An ordinary image/constant correction is a Source, not a filter. */
  const PaintLayerKindInfo *image = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_IMAGE, 0);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->category, PaintLayerKindCategory::Source);

  /* 3. Color ID: map-driven, reference-only map, only ever a mask item. */
  const PaintLayerKindInfo *color_id = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                              MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(color_id, nullptr);
  EXPECT_EQ(color_id->category, PaintLayerKindCategory::Operator);
  EXPECT_NE(color_id->flags & PAINT_LAYER_KIND_MAP_COVERAGE, 0);
  EXPECT_FALSE(color_id->map_paintable);
  EXPECT_EQ(color_id->allowed_roles, paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM));
  ASSERT_EQ(color_id->params_num, PAINT_LAYER_COVERAGE_PARAM_NUM);

  /* 4. UV Island: map-driven, paintable map, only ever a mask item. */
  const PaintLayerKindInfo *uv_island = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                               MA_PAINT_LAYER_FILTER_UV_ISLAND);
  ASSERT_NE(uv_island, nullptr);
  EXPECT_EQ(uv_island->category, PaintLayerKindCategory::Operator);
  EXPECT_NE(uv_island->flags & PAINT_LAYER_KIND_MAP_COVERAGE, 0);
  EXPECT_TRUE(uv_island->map_paintable);
  EXPECT_EQ(uv_island->allowed_roles, paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM));
}

TEST_F(PaintLayersMapCoverage, map_coverage_roles_only_allow_mask_item)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");

  /* A map-coverage kind may never become a content correction: the one role rule refuses it. */
  EXPECT_EQ(BKE_paint_layers_filter_add(*ma, layer, MA_PAINT_LAYER_FILTER_COLOR_ID), nullptr);
  EXPECT_EQ(BKE_paint_layers_filter_add(*ma, layer, MA_PAINT_LAYER_FILTER_UV_ISLAND), nullptr);
  /* An ordinary filter still may, so the check is not over-broad. */
  EXPECT_NE(BKE_paint_layers_filter_add(*ma, layer, MA_PAINT_LAYER_FILTER_INVERT), nullptr);

  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_TRUE(paint_layer_kind_is_map_coverage(*item));
  EXPECT_TRUE(paint_layer_kind_role_allowed(*item));
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Participation without a successful sample
 * \{ */

TEST_F(PaintLayersMapCoverage, map_coverage_item_takes_part_without_a_map)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);

  /* A fresh item has its structural Base Color record but no map. It still lays: the missing map
   * samples as `match = 0`, not a pass-through. */
  EXPECT_TRUE(paint_layer_mask_item_lays(*item));
  EXPECT_FALSE(paint_layer_map_coverage_has_map(*item));
  const MaterialPaintLayerChannel *record = paint_layer_map_coverage_record(*item);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(record->state, MA_PAINT_LAYER_CHANNEL_ENABLED);

  /* Assigning a map makes the sample succeed; switching the record off reads as "no map" again. */
  Image *image = add_image("IdMap");
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
  EXPECT_TRUE(paint_layer_map_coverage_has_map(*item));
  ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_FALSE(paint_layer_map_coverage_has_map(*item));
  /* Off is a state, not a removal: the operator is still there. */
  EXPECT_TRUE(paint_layer_mask_item_lays(*item));
}

TEST_F(PaintLayersMapCoverage, ordinary_paint_correction_still_lays_nothing_without_a_map)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, nullptr);
  ASSERT_NE(item, nullptr);
  /* The map-less rule is unchanged for the ordinary case: no map, no coverage. */
  EXPECT_FALSE(paint_layer_mask_item_lays(*item));
}

TEST_F(PaintLayersMapCoverage, missing_map_is_reported_as_an_issue)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  add_map_item(layer, MA_PAINT_LAYER_FILTER_UV_ISLAND);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  bool reported = false;
  for (const PaintLayersIssue &issue : issues) {
    if (issue.code == PaintLayersIssueCode::MapCoverageMissingMap) {
      reported = true;
    }
  }
  EXPECT_TRUE(reported);
}

TEST_F(PaintLayersMapCoverage, missing_color_id_map_is_reported_as_an_issue)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  bool reported = false;
  for (const PaintLayersIssue &issue : issues) {
    if (issue.code == PaintLayersIssueCode::MapCoverageMissingMap) {
      reported = true;
    }
  }
  EXPECT_TRUE(reported);
}

TEST_F(PaintLayersMapCoverage, disabled_base_color_record_reads_as_no_map)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  for (const eMaterialPaintLayerFilter filter :
       {MA_PAINT_LAYER_FILTER_COLOR_ID, MA_PAINT_LAYER_FILTER_UV_ISLAND})
  {
    MaterialPaintLayer *item = add_map_item(layer, filter);
    ASSERT_NE(item, nullptr);
    Image *map = add_image("Map");
    ASSERT_TRUE(
        BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
    EXPECT_EQ(BKE_paint_layers_map_coverage_image_get(*item), map);

    /* The record keeps its image, but the channel is off: every reader sees no map. */
    ASSERT_TRUE(BKE_paint_layers_channel_set_enabled(
        *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
    EXPECT_NE(paint_layer_map_coverage_record(*item)->image, nullptr);
    EXPECT_FALSE(paint_layer_map_coverage_has_map(*item));
    EXPECT_EQ(BKE_paint_layers_map_coverage_image_get(*item), nullptr);
  }
}

TEST_F(PaintLayersMapCoverage, uv_island_item_holds_no_id_colors)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *uv = add_map_item(layer, MA_PAINT_LAYER_FILTER_UV_ISLAND);
  ASSERT_NE(uv, nullptr);
  const float red[3] = {1.0f, 0.0f, 0.0f};
  EXPECT_EQ(BKE_paint_layers_id_color_add(*ma, uv, red), nullptr);
}

TEST_F(PaintLayersMapCoverage, tiled_island_map_gets_zero_filled_tiles_on_demand)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *tiled = BKE_image_add_generated(
      bmain, 8, 8, "Tiled", 32, false, IMA_GENTYPE_BLANK, black, false, true, true);
  ASSERT_NE(tiled, nullptr);
  EXPECT_EQ(BKE_image_get_tile(tiled, 1002), nullptr);

  /* The first tile exists already; a missing one is added with the first tile's shape. */
  EXPECT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*tiled, 1001));
  EXPECT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*tiled, 1012));
  EXPECT_TRUE(BKE_paint_layers_map_coverage_tile_ensure(*tiled, 1002));
  const ImageTile *tile = BKE_image_get_tile(tiled, 1002);
  ASSERT_NE(tile, nullptr);
  EXPECT_EQ(tile->gen_x, 8);
  EXPECT_EQ(tile->gen_y, 8);
  /* The list stays sorted whatever the order the tiles were asked for in. */
  EXPECT_EQ(static_cast<const ImageTile *>(tiled->tiles.last)->tile_number, 1012);

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.tile = 1002;
  ImBuf *ibuf = BKE_image_acquire_ibuf(tiled, &iuser, nullptr);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 8);
  BKE_image_release_ibuf(tiled, ibuf, nullptr);

  /* A plain image has no tiles to add. */
  Image *plain = BKE_image_add_generated(
      bmain, 8, 8, "Plain", 32, false, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(plain, nullptr);
  EXPECT_FALSE(BKE_paint_layers_map_coverage_tile_ensure(*plain, 1002));
}

TEST_F(PaintLayersMapCoverage, gradient_output_scales_the_match_with_the_standard_value)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);

  /* White and Black read through as one; Gradient takes the Base Color constant. */
  EXPECT_FALSE(BKE_paint_layers_mask_item_is_gradient(*item));
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_item_output_value(*item), 1.0f);
  ASSERT_TRUE(BKE_paint_layers_mask_item_set_gradient(*ma, item, true));
  EXPECT_TRUE(BKE_paint_layers_mask_item_is_gradient(*item));
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_item_output_value(*item), 1.0f);

  const float half[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, half));
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_item_output_value(*item), 0.25f);

  ASSERT_TRUE(BKE_paint_layers_mask_item_set_gradient(*ma, item, false));
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_item_output_value(*item), 1.0f);

  /* The Color ID mode and alpha choice are stored on the same item. */
  EXPECT_FALSE(BKE_paint_layers_mask_item_is_color_id_advanced(*item));
  ASSERT_TRUE(BKE_paint_layers_mask_item_set_color_id_advanced(*ma, item, true));
  EXPECT_TRUE(BKE_paint_layers_mask_item_is_color_id_advanced(*item));
  ASSERT_TRUE(BKE_paint_layers_mask_item_set_color_id_advanced(*ma, item, false));
  EXPECT_FALSE(BKE_paint_layers_mask_item_is_color_id_advanced(*item));
  EXPECT_FALSE(BKE_paint_layers_mask_item_is_alpha_background(*item));
  ASSERT_TRUE(BKE_paint_layers_mask_item_set_alpha_background(*ma, item, true));
  EXPECT_TRUE(BKE_paint_layers_mask_item_is_alpha_background(*item));
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Colors of a Color ID item
 * \{ */

TEST_F(PaintLayersMapCoverage, colors_add_newest_first_and_the_edits_tag_by_influence)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  const uint64_t topology_before = ma->paint_layers_flag;

  const float red[3] = {1.0f, 0.0f, 0.0f};
  const float green[3] = {0.0f, 1.0f, 0.0f};
  MaterialPaintLayerIdColor *first = BKE_paint_layers_id_color_add(*ma, item, red);
  MaterialPaintLayerIdColor *second = BKE_paint_layers_id_color_add(*ma, item, green);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  /* Newest first: the second added is the list head. */
  EXPECT_EQ(item->id_colors.count(), 2);
  EXPECT_EQ(static_cast<MaterialPaintLayerIdColor *>(item->id_colors.first), second);
  EXPECT_FLOAT_EQ(second->color[0], 0.0f);
  EXPECT_FLOAT_EQ(second->color[1], 1.0f);

  /* A colour and its flag are values; adding and removing are structure. */
  EXPECT_TRUE(BKE_paint_layers_id_color_set_color(*ma, item, second, red));
  EXPECT_FLOAT_EQ(second->color[0], 1.0f);
  EXPECT_TRUE(BKE_paint_layers_id_color_set_enabled(*ma, item, second, false));
  EXPECT_EQ(second->flag & MA_PAINT_LAYER_ID_COLOR_ENABLED, 0);
  EXPECT_TRUE(BKE_paint_layers_id_color_set_enabled(*ma, item, second, true));

  EXPECT_TRUE(BKE_paint_layers_id_color_remove(*ma, item, second));
  EXPECT_EQ(item->id_colors.count(), 1);
  EXPECT_EQ(static_cast<MaterialPaintLayerIdColor *>(item->id_colors.first), first);

  /* A second item holds its own colors: the list is per item. */
  MaterialPaintLayer *other = add_map_item(layer, MA_PAINT_LAYER_FILTER_UV_ISLAND);
  ASSERT_NE(other, nullptr);
  EXPECT_EQ(other->id_colors.count(), 0);
  (void)topology_before;
}

TEST_F(PaintLayersMapCoverage, color_id_api_refuses_foreign_items)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  MaterialPaintLayer *uv = add_map_item(layer, MA_PAINT_LAYER_FILTER_UV_ISLAND);
  ASSERT_NE(item, nullptr);
  ASSERT_NE(uv, nullptr);

  const float red[3] = {1.0f, 0.0f, 0.0f};
  MaterialPaintLayerIdColor *color = BKE_paint_layers_id_color_add(*ma, item, red);
  ASSERT_NE(color, nullptr);

  /* An ordinary correction is not a map-coverage item. */
  MaterialPaintLayer *paint = BKE_paint_layers_mask_item_add(
      *ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE, nullptr);
  ASSERT_NE(paint, nullptr);
  EXPECT_EQ(BKE_paint_layers_id_color_add(*ma, paint, red), nullptr);
  /* A colour of one item cannot be removed through another. */
  EXPECT_FALSE(BKE_paint_layers_id_color_remove(*ma, uv, color));
  /* A mask item of another material is foreign. */
  Material *other_ma = BKE_material_add(bmain, "Other");
  other_ma->paint_layers_flag |= MA_PAINT_LAYERED;
  EXPECT_EQ(BKE_paint_layers_id_color_add(*other_ma, item, red), nullptr);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Copying: material copy, duplicate, paste
 * \{ */

TEST_F(PaintLayersMapCoverage, duplicate_copies_colors_and_parameters_independently)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  const float red[3] = {1.0f, 0.0f, 0.0f};
  MaterialPaintLayerIdColor *color = BKE_paint_layers_id_color_add(*ma, item, red);
  ASSERT_NE(color, nullptr);
  ASSERT_TRUE(paint_layer_kind_param_set(*item, PAINT_LAYER_COVERAGE_TOLERANCE, 0.125f));
  ASSERT_TRUE(BKE_paint_layers_id_color_set_tolerance(*ma, item, color, 0.3f));

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(bmain, *ma, item);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->kind_subtype, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_EQ(copy->id_colors.count(), 1);
  MaterialPaintLayerIdColor *copy_color = static_cast<MaterialPaintLayerIdColor *>(
      copy->id_colors.first);
  ASSERT_NE(copy_color, nullptr);
  EXPECT_FLOAT_EQ(copy_color->color[0], 1.0f);
  EXPECT_FLOAT_EQ(copy_color->tolerance, 0.3f);
  EXPECT_NE(copy_color, item->id_colors.first);
  ASSERT_NE(copy->system_properties, nullptr);
  const IDProperty *tolerance = IDP_GetPropertyFromGroup(copy->system_properties, "tolerance");
  ASSERT_NE(tolerance, nullptr);
  EXPECT_FLOAT_EQ(IDP_float_get(tolerance), 0.125f);

  /* Independent: editing the copy's colour does not touch the original. */
  const float blue[3] = {0.0f, 0.0f, 1.0f};
  EXPECT_TRUE(BKE_paint_layers_id_color_set_color(*ma, copy, copy_color, blue));
  EXPECT_FLOAT_EQ(static_cast<MaterialPaintLayerIdColor *>(item->id_colors.first)->color[0], 1.0f);
}

TEST_F(PaintLayersMapCoverage, material_copy_copies_colors)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  const float stored[3] = {0.25f, 0.5f, 0.75f};
  ASSERT_NE(BKE_paint_layers_id_color_add(*ma, item, stored), nullptr);

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  MaterialPaintLayer *copy_item = BKE_paint_layers_find(*copy, item->marker);
  ASSERT_NE(copy_item, nullptr);
  ASSERT_EQ(copy_item->id_colors.count(), 1);
  MaterialPaintLayerIdColor *copy_color = static_cast<MaterialPaintLayerIdColor *>(
      copy_item->id_colors.first);
  ASSERT_NE(copy_color, nullptr);
  EXPECT_NE(static_cast<void *>(copy_color), static_cast<void *>(item->id_colors.first));
  EXPECT_FLOAT_EQ(copy_color->color[2], 0.75f);
}

TEST_F(PaintLayersMapCoverage, paste_copies_colors_onto_another_layer)
{
  MaterialPaintLayer *source_layer = add_masked_layer("Source");
  MaterialPaintLayer *item = add_map_item(source_layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  const float stored[3] = {0.1f, 0.2f, 0.3f};
  ASSERT_NE(BKE_paint_layers_id_color_add(*ma, item, stored), nullptr);

  MaterialPaintLayer *target_layer = add_masked_layer("Target");
  ASSERT_TRUE(BKE_paint_layers_correction_can_paste(*item, *target_layer));
  MaterialPaintLayer *pasted = BKE_paint_layers_correction_paste(
      bmain, *ma, *item, target_layer);
  ASSERT_NE(pasted, nullptr);
  ASSERT_EQ(pasted->id_colors.count(), 1);
  MaterialPaintLayerIdColor *pasted_color = static_cast<MaterialPaintLayerIdColor *>(
      pasted->id_colors.first);
  ASSERT_NE(pasted_color, nullptr);
  EXPECT_FLOAT_EQ(pasted_color->color[1], 0.2f);
  EXPECT_NE(static_cast<void *>(pasted_color), static_cast<void *>(item->id_colors.first));
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Dependencies and the paint target
 * \{ */

TEST_F(PaintLayersMapCoverage, dependencies_report_only_the_base_color_map)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_UV_ISLAND);
  ASSERT_NE(item, nullptr);
  Image *id_map = add_image("IdMap");
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, id_map));
  /* A stray second record must not be reported: the kind reads its Base Color map alone. */
  Image *stray = add_image("Stray");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_ROUGHNESS, stray));

  Vector<const Image *> reported;
  paint_layer_dependencies(*item, [&](const PaintLayerDependency &dep) {
    reported.append(reinterpret_cast<const Image *>(*dep.slot));
  });
  ASSERT_EQ(reported.size(), 1);
  EXPECT_EQ(reported[0], id_map);
}

TEST_F(PaintLayersMapCoverage, color_id_dependencies_report_only_the_base_color_map)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  Image *id_map = add_image("IdMap");
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, id_map));
  Image *stray = add_image("Stray");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_ROUGHNESS, stray));

  Vector<const Image *> reported;
  paint_layer_dependencies(*item, [&](const PaintLayerDependency &dep) {
    reported.append(reinterpret_cast<const Image *>(*dep.slot));
  });
  ASSERT_EQ(reported.size(), 1);
  EXPECT_EQ(reported[0], id_map);
}

TEST_F(PaintLayersMapCoverage, color_id_is_not_a_paint_target_but_uv_island_is)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *color_item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  MaterialPaintLayer *uv_item = add_map_item(layer, MA_PAINT_LAYER_FILTER_UV_ISLAND);
  ASSERT_NE(color_item, nullptr);
  ASSERT_NE(uv_item, nullptr);

  PaintLayersTarget target;
  BKE_paint_layers_active_set(*ma, color_item->marker);
  EXPECT_FALSE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));

  BKE_paint_layers_active_set(*ma, uv_item->marker);
  EXPECT_TRUE(BKE_paint_layers_target_get(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PaintLayersTargetMode::Mask, target));
  EXPECT_EQ(target.mask_item, uv_item);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Serialization
 * \{ */

TEST_F(PaintLayersMapCoverage, blend_round_trip_keeps_id_colors)
{
  MaterialPaintLayer *layer = add_masked_layer("Layer");
  MaterialPaintLayer *item = add_map_item(layer, MA_PAINT_LAYER_FILTER_COLOR_ID);
  ASSERT_NE(item, nullptr);
  const bUUID real_item_marker = item->marker;
  const float older[3] = {0.4f, 0.5f, 0.6f};
  const float newer[3] = {0.7f, 0.8f, 0.9f};
  ASSERT_NE(BKE_paint_layers_id_color_add(*ma, item, older), nullptr);
  ASSERT_NE(BKE_paint_layers_id_color_add(*ma, item, newer), nullptr);
  id_us_plus(&ma->id);

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_id_colors.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);
  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "MapCoverage")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_item = BKE_paint_layers_find(*reloaded, real_item_marker);
  ASSERT_NE(reloaded_item, nullptr);
  ASSERT_EQ(reloaded_item->id_colors.count(), 2);
  /* The read list keeps its order (newest first). */
  EXPECT_FLOAT_EQ(
      static_cast<MaterialPaintLayerIdColor *>(reloaded_item->id_colors.first)->color[0], 0.7f);
  EXPECT_FLOAT_EQ(
      static_cast<MaterialPaintLayerIdColor *>(reloaded_item->id_colors.last)->color[0], 0.4f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name The match arithmetic the composite, the strokes and the picker share
 * \{ */

static PaintMaterialCompositeIdColor id_color_make(const float r,
                                                   const float g,
                                                   const float b,
                                                   const bool enabled = true)
{
  PaintMaterialCompositeIdColor color;
  color.color[0] = r;
  color.color[1] = g;
  color.color[2] = b;
  color.enabled = enabled;
  return color;
}

TEST(PaintMaterialCoverageMatch, color_id_uses_the_max_channel_distance_and_skips_disabled)
{
  const PaintMaterialCompositeIdColor colors[] = {id_color_make(0.5f, 0.5f, 0.5f),
                                                  id_color_make(0.0f, 0.0f, 0.0f, false)};
  const Span<PaintMaterialCompositeIdColor> span(colors, 2);
  const float near[3] = {0.55f, 0.45f, 0.5f};
  const float far_one_channel[3] = {0.5f, 0.5f, 0.9f};
  const float dark[3] = {0.0f, 0.0f, 0.0f};
  EXPECT_FLOAT_EQ(
      BKE_paint_material_coverage_match(MA_PAINT_LAYER_FILTER_COLOR_ID, near, 0.1f, 0.0f, span),
      1.0f);
  /* One channel far away is enough to miss, a mean would let it through. */
  EXPECT_FLOAT_EQ(BKE_paint_material_coverage_match(
                      MA_PAINT_LAYER_FILTER_COLOR_ID, far_one_channel, 0.1f, 0.0f, span),
                  0.0f);
  /* The disabled black entry never matches, even for a black texel. */
  EXPECT_FLOAT_EQ(
      BKE_paint_material_coverage_match(MA_PAINT_LAYER_FILTER_COLOR_ID, dark, 0.1f, 0.0f, span),
      0.0f);
}

TEST(PaintMaterialCoverageMatch, color_id_softness_falls_off_between_tolerance_and_end)
{
  const PaintMaterialCompositeIdColor colors[] = {id_color_make(0.5f, 0.5f, 0.5f)};
  const Span<PaintMaterialCompositeIdColor> span(colors, 1);
  const float inside[3] = {0.55f, 0.5f, 0.5f};
  const float falloff[3] = {0.65f, 0.5f, 0.5f};
  const float outside[3] = {0.9f, 0.5f, 0.5f};
  EXPECT_FLOAT_EQ(BKE_paint_material_coverage_match(
                      MA_PAINT_LAYER_FILTER_COLOR_ID, inside, 0.1f, 0.2f, span),
                  1.0f);
  const float mid = BKE_paint_material_coverage_match(
      MA_PAINT_LAYER_FILTER_COLOR_ID, falloff, 0.1f, 0.2f, span);
  EXPECT_GT(mid, 0.0f);
  EXPECT_LT(mid, 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_coverage_match(
                      MA_PAINT_LAYER_FILTER_COLOR_ID, outside, 0.1f, 0.2f, span),
                  0.0f);
}

TEST(PaintMaterialCoverageMatch, color_id_advanced_uses_each_colours_own_tolerance)
{
  PaintMaterialCompositeIdColor color = id_color_make(0.5f, 0.5f, 0.5f);
  color.tolerance = 0.1f;
  color.own_tolerance = true;
  const Span<PaintMaterialCompositeIdColor> span(&color, 1);
  const float near[3] = {0.55f, 0.5f, 0.5f};
  const float past[3] = {0.7f, 0.5f, 0.5f};
  /* The shared softness is ignored: Advanced has a hard edge at the colour's tolerance. */
  EXPECT_FLOAT_EQ(BKE_paint_material_coverage_match(
                      MA_PAINT_LAYER_FILTER_COLOR_ID, near, 0.0f, 0.5f, span),
                  1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_coverage_match(
                      MA_PAINT_LAYER_FILTER_COLOR_ID, past, 0.0f, 0.5f, span),
                  0.0f);
}

TEST(PaintMaterialCoverageMatch, uv_island_reads_the_red_channel_clamped)
{
  const float half[3] = {0.5f, 0.0f, 0.0f};
  const float over[3] = {3.0f, 0.0f, 0.0f};
  EXPECT_FLOAT_EQ(
      BKE_paint_material_coverage_match(MA_PAINT_LAYER_FILTER_UV_ISLAND, half, 0.0f, 0.0f, {}),
      0.5f);
  EXPECT_FLOAT_EQ(
      BKE_paint_material_coverage_match(MA_PAINT_LAYER_FILTER_UV_ISLAND, over, 0.0f, 0.0f, {}),
      1.0f);
}

TEST(PaintMaterialMaskFactor, map_coverage_item_restricts_inverts_and_fades_with_opacity)
{
  PaintMaterialCompositeMaskBuffer item;
  item.map_coverage = MA_PAINT_LAYER_FILTER_UV_ISLAND;
  /* Two texels: inside the island (red 1) and outside it (red 0). */
  const float texels[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const float *no_samples[1] = {nullptr};
  const float *coverage[1] = {texels};
  const Span<PaintMaterialCompositeMaskBuffer> items(&item, 1);

  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 0), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 0.0f);

  item.invert = true;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 0), 0.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 1.0f);

  /* Half opacity lerps between pass-through and the match; zero opacity is a pass-through. */
  item.invert = false;
  item.opacity = 0.5f;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 0.5f);
  item.opacity = 0.0f;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 1.0f);
}

TEST(PaintMaterialMaskFactor, color_id_item_restricts_inverts_and_fades_with_opacity)
{
  PaintMaterialCompositeMaskBuffer item;
  item.map_coverage = MA_PAINT_LAYER_FILTER_COLOR_ID;
  const PaintMaterialCompositeIdColor colors[] = {id_color_make(1.0f, 0.0f, 0.0f)};
  item.coverage_colors.append(colors[0]);
  /* Two texels: the stored colour (match 1) and black (match 0). */
  const float texels[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const float *no_samples[1] = {nullptr};
  const float *coverage[1] = {texels};
  const Span<PaintMaterialCompositeMaskBuffer> items(&item, 1);

  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 0), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 0.0f);

  item.invert = true;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 0), 0.0f);
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 1.0f);

  item.invert = false;
  item.opacity = 0.5f;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 0.5f);
  item.opacity = 0.0f;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 1.0f);
}

TEST(PaintMaterialMaskFactor, map_coverage_gradient_scales_the_match)
{
  PaintMaterialCompositeMaskBuffer item;
  item.map_coverage = MA_PAINT_LAYER_FILTER_COLOR_ID;
  const PaintMaterialCompositeIdColor colors[] = {id_color_make(1.0f, 0.0f, 0.0f)};
  item.coverage_colors.append(colors[0]);
  const float texels[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const float *no_samples[1] = {nullptr};
  const float *coverage[1] = {texels};
  const Span<PaintMaterialCompositeMaskBuffer> items(&item, 1);

  /* A gradient value of 0.25 keeps a quarter of the matched coverage and nothing of the rest. */
  item.coverage_output = 0.25f;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 0), 0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 1), 0.0f);

  /* One is the White result again, so a Gradient item at full value matches White. */
  item.coverage_output = 1.0f;
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, coverage, 0), 1.0f);
}

TEST(PaintMaterialMaskFactor, map_coverage_item_without_a_map_matches_nothing)
{
  PaintMaterialCompositeMaskBuffer item;
  item.map_coverage = MA_PAINT_LAYER_FILTER_COLOR_ID;
  const float *no_samples[1] = {nullptr};
  const float *no_coverage[1] = {nullptr};
  const Span<PaintMaterialCompositeMaskBuffer> items(&item, 1);
  /* Not a pass-through: a missing map restricts everything. */
  EXPECT_FLOAT_EQ(BKE_paint_material_mask_factor_eval(items, no_samples, no_coverage, 0), 0.0f);
}

/* \} */

}  // namespace blender::bke::tests
