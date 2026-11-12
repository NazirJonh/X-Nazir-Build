/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_report.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_fileops.h"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include <cstring>

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include <cmath>
#include <limits>
#include <string>

namespace blender::bke::tests {

class PaintLayersDescription : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  /** The cache budget the test found, restored on teardown. */
  size_t slice_cache_budget_before_ = 0;
  /** Scratch for the composite descriptions the height tests read. */
  Vector<PaintMaterialCompositeImageLayer> layers_;

  void SetUp() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    slice_cache_budget_before_ = paint_layer_anchor_point_slice_cache_budget();
    bmain = BKE_main_new();
    BKE_blender_globals_main_replace(bmain);
    BKE_tempdir_init(nullptr);
  }

  void TearDown() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    paint_layer_anchor_point_slice_cache_set_budget(slice_cache_budget_before_);
    G_MAIN = nullptr;
    BKE_main_free(bmain);
    BKE_tempdir_session_purge();
  }

  /** A row linked straight into the list, the way a file read leaves it: no BKE call involved. */
  MaterialPaintLayer *paint_layer_add(Material &ma, const char *name)
  {
    MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
    STRNCPY(layer->name, name);
    layer->marker = BLI_uuid_generate_random();
    layer->flag = MA_PAINT_LAYER_ENABLED;
    layer->opacity = 1.0f;
    BLI_addtail(&ma.paint_layers, layer);
    return layer;
  }

  MaterialPaintLayerChannel &paint_layer_channel_add(MaterialPaintLayer &layer, Image &image)
  {
    layer.channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
    layer.channels_num = 1;
    layer.channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
    layer.channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    layer.channels[0].image = &image;
    layer.channels[0].gradient_ramp = nullptr;
    return layer.channels[0];
  }
};

/* The first row of the stack, or null when it is empty. */
static MaterialPaintLayer *paint_layers_first(Material &ma)
{
  return ma.paint_layers.is_empty() ? nullptr : &*ma.paint_layers.begin();
}

/* -------------------------------------------------------------------- */
/** \name Data model and lifecycle
 * \{ */

TEST_F(PaintLayersDescription, fresh_material_has_no_layers)
{
  Material *ma = BKE_material_add(bmain, "Mat");
  EXPECT_TRUE(BLI_listbase_is_empty(&ma->paint_layers));
  EXPECT_FALSE(BKE_paint_layers_is_layered(*ma));
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(*ma)));

  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_TRUE(BKE_paint_layers_is_layered(*ma));

  ma->paint_layers_flag = {};
  EXPECT_FALSE(BKE_paint_layers_is_layered(*ma));
}

TEST_F(PaintLayersDescription, channel_opacity_multiplies_the_rows_own_for_every_row_kind)
{
  Material *ma = BKE_material_add(bmain, "ChannelOpacityMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");
  layer->opacity = 0.5f;
  MaterialPaintLayer *folder = paint_layer_add(*ma, "Folder");
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;

  /* No override reads as the row's own opacity. */
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_opacity_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), 0.5f);

  EXPECT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.5f));
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_opacity_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), 0.25f);
  /* The row's own opacity and the other channels are untouched. */
  EXPECT_FLOAT_EQ(layer->opacity, 0.5f);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_opacity_effective(*layer, PAINT_MATERIAL_CHANNEL_METALLIC), 0.5f);

  /* A folder holds no channel records and takes the override the same way. */
  EXPECT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 0.4f));
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_opacity_effective(*folder, PAINT_MATERIAL_CHANNEL_ROUGHNESS), 0.4f);
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));

  EXPECT_FALSE(BKE_paint_layers_channel_opacity_set(*ma, *layer, PAINT_MATERIAL_CHANNEL_NUM, 0.5f));
}

TEST_F(PaintLayersDescription, channel_blend_overrides_the_rows_own_per_channel)
{
  Material *ma = BKE_material_add(bmain, "ChannelBlendMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_MULTIPLY));
  /* The first blend edit after idle keeps the static Mix: it is a node property, so only the sync
   * is asked for. The channel enters the runtime tier with the second edit. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  /* No override reads as the row's own blend. */
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            MA_PAINT_LAYER_BLEND_MULTIPLY);

  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_ADD));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC, 0);
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            MA_PAINT_LAYER_BLEND_ADD);
  /* The row's own blend and the other channels are untouched. */
  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_METALLIC),
            MA_PAINT_LAYER_BLEND_MULTIPLY);

  /* Inherit puts the channel back on the row's blend. */
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, -1));
  EXPECT_EQ(BKE_paint_layers_channel_blend_effective(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            MA_PAINT_LAYER_BLEND_MULTIPLY);

  /* The Normal channel has its own two choices: the combine (inherit) and Replace. Replace is a
   * different node, so this one is a rebuild. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  EXPECT_FALSE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_normal_replace(*layer));
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE));
  EXPECT_TRUE(BKE_paint_layers_normal_replace(*layer));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(*ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL, -1));
  EXPECT_FALSE(BKE_paint_layers_normal_replace(*layer));

  /* A folder holds no channel records and takes the override the same way; it stops inlining. */
  MaterialPaintLayer *folder = paint_layer_add(*ma, "Folder");
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  /* A channel blend that takes the folder out of Pass Through changes the graph. */
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* Replace belongs to the Normal channel's per-channel setting alone. */
  EXPECT_FALSE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_NORMAL_REPLACE));
  EXPECT_FALSE(BKE_paint_layers_channel_blend_set(*ma, *layer, PAINT_MATERIAL_CHANNEL_NUM, 0));
}

TEST_F(PaintLayersDescription, blend_round_trip_keeps_layers)
{
  Material *ma = BKE_material_add(bmain, "RoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "RoundTripImage"));
  id_us_plus(&image->id);

  MaterialPaintLayer *first = paint_layer_add(*ma, "First");
  const bUUID first_marker = first->marker;
  first->opacity = 0.25f;
  paint_layer_channel_add(*first, *image);
  first->channels[0].value[0] = 0.5f;
  first->kind_subtype = 7;
  first->system_properties = bke::idprop::create_group("kind").release();
  IDP_AddToGroup(first->system_properties, bke::idprop::create("gain", 0.75f).release());
  IDP_AddToGroup(first->system_properties, bke::idprop::create("steps", 3).release());
  MaterialPaintLayer *second = paint_layer_add(*ma, "Second");
  const bUUID second_marker = second->marker;
  second->flag = 0;
  ma->active_layer_marker = second_marker;

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "RoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_layered(*reloaded));
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*reloaded), second_marker));

  ASSERT_EQ(BLI_listbase_count(&reloaded->paint_layers), 2);
  MaterialPaintLayer *reloaded_first = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_first, nullptr);
  EXPECT_STREQ(reloaded_first->name, "First");
  EXPECT_TRUE(BLI_uuid_equal(reloaded_first->marker, first_marker));
  EXPECT_FLOAT_EQ(reloaded_first->opacity, 0.25f);
  EXPECT_NE(reloaded_first->flag & MA_PAINT_LAYER_ENABLED, 0);

  /* The kind's parameters travel with the row; a row that set none reads back null. */
  EXPECT_EQ(reloaded_first->kind_subtype, 7);
  ASSERT_NE(reloaded_first->system_properties, nullptr);
  const IDProperty *gain = IDP_GetPropertyFromGroup(reloaded_first->system_properties, "gain");
  const IDProperty *steps = IDP_GetPropertyFromGroup(reloaded_first->system_properties, "steps");
  ASSERT_NE(gain, nullptr);
  ASSERT_NE(steps, nullptr);
  EXPECT_FLOAT_EQ(IDP_float_get(gain), 0.75f);
  EXPECT_EQ(IDP_int_get(steps), 3);
  EXPECT_EQ(reloaded_first->next->system_properties, nullptr);

  ASSERT_EQ(reloaded_first->channels_num, 1);
  ASSERT_NE(reloaded_first->channels, nullptr);
  EXPECT_EQ(reloaded_first->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(reloaded_first->channels[0].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_FLOAT_EQ(reloaded_first->channels[0].value[0], 0.5f);
  /* The image reference is remapped on read, not left as a stale address. */
  ASSERT_NE(reloaded_first->channels[0].image, nullptr);
  EXPECT_STREQ(reloaded_first->channels[0].image->id.name + 2, "RoundTripImage");

  MaterialPaintLayer *reloaded_second = reloaded_first->next;
  ASSERT_NE(reloaded_second, nullptr);
  EXPECT_STREQ(reloaded_second->name, "Second");
  EXPECT_TRUE(BLI_uuid_equal(reloaded_second->marker, second_marker));
  EXPECT_EQ(reloaded_second->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_EQ(reloaded_second->channels_num, 0);
  EXPECT_EQ(reloaded_second->channels, nullptr);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersDescription, foreach_id_walks_channel_images)
{
  Material *ma = BKE_material_add(bmain, "WalkMat");
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "WalkImage"));
  Image *other_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "WalkOtherImage"));

  MaterialPaintLayer *first = paint_layer_add(*ma, "WalkFirst");
  paint_layer_channel_add(*first, *image);
  MaterialPaintLayer *second = paint_layer_add(*ma, "WalkSecond");
  paint_layer_channel_add(*second, *other_image);

  Vector<ID *> visited;
  auto collector = [](LibraryIDLinkCallbackData *data) -> int {
    if (*data->id_pointer != nullptr) {
      static_cast<Vector<ID *> *>(data->user_data)->append(*data->id_pointer);
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(bmain, &ma->id, collector, &visited, IDWALK_READONLY);

  EXPECT_TRUE(visited.contains(&image->id));
  EXPECT_TRUE(visited.contains(&other_image->id));
}

TEST_F(PaintLayersDescription, dependencies_list_each_channel_map_with_its_channel)
{
  Material *ma = BKE_material_add(bmain, "DepsMat");
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "DepsImage"));
  paint_layer_channel_add(*layer, *image);

  Vector<PaintLayerDependency> found;
  paint_layer_dependencies(*layer,
                           [&](const PaintLayerDependency &dep) { found.append(dep); });
  ASSERT_EQ(found.size(), 1);
  EXPECT_EQ(found[0].kind, PaintLayerDependencyKind::ImageMap);
  EXPECT_EQ(found[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(*found[0].slot, &image->id);
  /* The slot is the row's own pointer, so a write through it is what the copy remap relies on. */
  EXPECT_EQ(found[0].slot, reinterpret_cast<ID **>(&layer->channels[0].image));

  MaterialPaintLayer *bare = paint_layer_add(*ma, "Bare");
  int bare_num = 0;
  paint_layer_dependencies(*bare, [&](const PaintLayerDependency & /*dep*/) { bare_num++; });
  EXPECT_EQ(bare_num, 0);
}

TEST_F(PaintLayersDescription, kind_registry_describes_the_built_in_kinds)
{
  Material *ma = BKE_material_add(bmain, "KindMat");
  MaterialPaintLayer *row = paint_layer_add(*ma, "Row");

  const PaintLayerKindInfo *image = paint_layer_kind_of(*row);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->category, PaintLayerKindCategory::Source);
  row->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  EXPECT_TRUE(paint_layer_kind_role_allowed(*row));

  row->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  ASSERT_NE(paint_layer_kind_of(*row), nullptr);
  EXPECT_EQ(paint_layer_kind_of(*row)->category, PaintLayerKindCategory::Source);

  /* A folder is a layer only: its children have no place in a correction or a mask. */
  row->source = MA_PAINT_LAYER_SOURCE_STACK;
  ASSERT_NE(paint_layer_kind_of(*row), nullptr);
  EXPECT_EQ(paint_layer_kind_of(*row)->category, PaintLayerKindCategory::Container);
  EXPECT_FALSE(paint_layer_kind_role_allowed(*row));
  row->role = MA_PAINT_LAYER_ROLE_LAYER;
  EXPECT_TRUE(paint_layer_kind_role_allowed(*row));

  /* A source or subtype this build does not know is reported as unknown, never as a built-in. */
  /* 3 is reserved for the Node Group source, which this build does not have. */
  row->source = 3;
  EXPECT_EQ(paint_layer_kind_of(*row), nullptr);
  EXPECT_FALSE(paint_layer_kind_role_allowed(*row));
  row->source = MA_PAINT_LAYER_SOURCE_IMAGE;
  row->kind_subtype = 99;
  EXPECT_EQ(paint_layer_kind_of(*row), nullptr);
}

TEST_F(PaintLayersDescription, kind_flags_say_what_a_kind_lays_and_what_it_changes)
{
  Material *ma = BKE_material_add(bmain, "KindFlagMat");
  MaterialPaintLayer *row = paint_layer_add(*ma, "Row");

  EXPECT_FALSE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_FLAT_VALUE));
  row->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  EXPECT_TRUE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_FLAT_VALUE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_ACTS_ON_BELOW));

  row->source = MA_PAINT_LAYER_SOURCE_FILTER;
  for (const int16_t filter : {MA_PAINT_LAYER_FILTER_INVERT,
                               MA_PAINT_LAYER_FILTER_LEVELS,
                               MA_PAINT_LAYER_FILTER_HSL})
  {
    row->kind_subtype = filter;
    EXPECT_TRUE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_ACTS_ON_BELOW));
    EXPECT_FALSE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_FLAT_VALUE));
  }

  /* A kind this build does not know has no flags: it neither lays a value nor changes a colour. */
  row->kind_subtype = 99;
  EXPECT_FALSE(paint_layer_kind_has_flag(*row, PAINT_LAYER_KIND_ACTS_ON_BELOW));
}

TEST_F(PaintLayersDescription, add_policy_makes_a_filter_correction_on_the_row_it_hangs_on)
{
  Material &ma = *BKE_material_add(bmain, "FilterAddMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Filter;
  params.anchor = row;
  params.filter = MA_PAINT_LAYER_FILTER_INVERT;
  MaterialPaintLayer *first = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(first->source, MA_PAINT_LAYER_SOURCE_FILTER);
  EXPECT_EQ(first->kind_subtype, MA_PAINT_LAYER_FILTER_INVERT);
  EXPECT_STREQ(first->name, "Invert 1");
  EXPECT_EQ(BLI_listbase_count(&row->effects), 1);
  EXPECT_TRUE(paint_layer_kind_role_allowed(*first));

  /* An Add aimed at the filter means another one on the same row. */
  params.anchor = first;
  MaterialPaintLayer *second = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(second->name, "Invert 2");
  EXPECT_EQ(BLI_listbase_count(&row->effects), 2);

  /* A filter this build does not know adds nothing. */
  params.filter = 99;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(ma, params), nullptr);
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(ma, issues);
  EXPECT_TRUE(issues.is_empty());
}

TEST_F(PaintLayersDescription, a_filter_added_with_the_base_mask_active_lands_in_the_mask_stack)
{
  Material &ma = *BKE_material_add(bmain, "MaskEffectAddMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, row, 1.0f), nullptr);
  MaterialPaintLayer *base = BKE_paint_layers_mask_base(*row);
  ASSERT_NE(base, nullptr);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Filter;
  params.anchor = base;
  params.filter = MA_PAINT_LAYER_FILTER_BLUR;
  MaterialPaintLayer *effect = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_FILTER);
  EXPECT_EQ(effect->kind_subtype, MA_PAINT_LAYER_FILTER_BLUR);
  EXPECT_STREQ(effect->name, "Blur 1");
  EXPECT_TRUE(paint_layer_kind_role_allowed(*effect));
  /* A mask effect reads a single grey, the coverage built up to it: it owns no channel records. */
  EXPECT_EQ(effect->channels, nullptr);
  EXPECT_EQ(effect->channels_num, 0);
  /* It landed in the mask stack, not in the row's content corrections. */
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 2);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 0);
  EXPECT_EQ(BLI_findindex(&row->mask_stack, effect), 1);
}

TEST_F(PaintLayersDescription, a_filter_added_with_the_row_active_lands_in_the_effects)
{
  Material &ma = *BKE_material_add(bmain, "RowEffectAddMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, row, 1.0f), nullptr);

  /* The same Add aimed at the row itself still means the row's colour. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Filter;
  params.anchor = row;
  params.filter = MA_PAINT_LAYER_FILTER_BLUR;
  MaterialPaintLayer *effect = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 1);
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 1);

  /* And one aimed at that correction means another correction of the same row. */
  params.anchor = effect;
  MaterialPaintLayer *second = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 2);
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 1);
}

TEST_F(PaintLayersDescription, a_mask_item_effect_keeps_the_stack_order)
{
  Material &ma = *BKE_material_add(bmain, "MaskEffectOrderMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, row, 1.0f), nullptr);
  MaterialPaintLayer *base = BKE_paint_layers_mask_base(*row);
  ASSERT_NE(base, nullptr);

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Filter;
  params.anchor = base;
  params.filter = MA_PAINT_LAYER_FILTER_BLUR;
  MaterialPaintLayer *blur = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(blur, nullptr);

  /* A painted item between two effects, so the order is worth checking. */
  MaterialPaintLayer *paint = BKE_paint_layers_mask_item_add(
      ma, row, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask Paint 1");
  ASSERT_NE(paint, nullptr);

  /* An Add aimed at the first mask effect means another one on the row that owns it, appended
   * after what the stack already holds. */
  params.anchor = blur;
  params.filter = MA_PAINT_LAYER_FILTER_NOISE;
  MaterialPaintLayer *noise = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(noise, nullptr);
  EXPECT_EQ(noise->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_STREQ(noise->name, "Noise 1");

  int index = 0;
  int blur_index = -1;
  int paint_index = -1;
  int noise_index = -1;
  for (MaterialPaintLayer &item : row->mask_stack) {
    if (&item == base) {
      EXPECT_EQ(index, 0);
    }
    else if (&item == blur) {
      blur_index = index;
    }
    else if (&item == paint) {
      paint_index = index;
    }
    else if (&item == noise) {
      noise_index = index;
    }
    index++;
  }
  EXPECT_EQ(index, 4);
  EXPECT_EQ(blur_index, 1);
  EXPECT_EQ(paint_index, 2);
  EXPECT_EQ(noise_index, 3);
}

TEST_F(PaintLayersDescription, mask_filter_add_takes_a_cpu_effect_and_refuses_the_unknown)
{
  Material &ma = *BKE_material_add(bmain, "MaskFilterAddMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, row, 1.0f), nullptr);

  /* The RNA's mask_filter_add is what the Add Effect menu calls, and every CPU-computed kind is
   * legal as a mask item: it must take the kinds the mask can bake a grey for. */
  MaterialPaintLayer *blur = BKE_paint_layers_mask_filter_add(ma, row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  EXPECT_EQ(blur->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(blur->kind_subtype, MA_PAINT_LAYER_FILTER_BLUR);
  /* The numbering is the Add Effect routing's; the bare call names the item by its kind. */
  EXPECT_STREQ(blur->name, "Blur");
  EXPECT_TRUE(paint_layer_kind_role_allowed(*blur));

  /* A kind the registry does not know answers null: no exception, no crash, only the report the
   * RNA wrapper raises. */
  EXPECT_EQ(BKE_paint_layers_mask_filter_add(ma, row, eMaterialPaintLayerFilter(999)), nullptr);
  /* And the mask stack holds only the one effect beside its base. */
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 2);
}

TEST_F(PaintLayersDescription, maps_collect_reads_nested_rows_once)
{
  Material *ma = BKE_material_add(bmain, "CollectMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *folder = paint_layer_add(*ma, "Folder");
  /* Only a folder is walked into: a row linked by hand has to say what it is. */
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  Image *shared = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CollectShared"));
  Image *item_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CollectItem"));

  MaterialPaintLayer *child = MEM_new<MaterialPaintLayer>(__func__);
  child->marker = BLI_uuid_generate_random();
  BLI_addtail(&folder->children, child);
  paint_layer_channel_add(*child, *shared);

  MaterialPaintLayer *item = MEM_new<MaterialPaintLayer>(__func__);
  item->marker = BLI_uuid_generate_random();
  item->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  BLI_addtail(&child->mask_stack, item);
  paint_layer_channel_add(*item, *item_image);

  MaterialPaintLayer *effect = MEM_new<MaterialPaintLayer>(__func__);
  effect->marker = BLI_uuid_generate_random();
  effect->role = MA_PAINT_LAYER_ROLE_EFFECT;
  BLI_addtail(&child->effects, effect);
  paint_layer_channel_add(*effect, *shared);

  Vector<Image *> images;
  BKE_paint_layers_maps_collect(*bmain, images);
  ASSERT_EQ(images.size(), 2);
  EXPECT_TRUE(images.contains(shared));
  EXPECT_TRUE(images.contains(item_image));
}

TEST_F(PaintLayersDescription, copy_keeps_layers)
{
  Material *ma = BKE_material_add(bmain, "CopyLayersMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *first = paint_layer_add(*ma, "First");
  const bUUID first_marker = first->marker;
  Image *channel_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CopyChannel"));
  paint_layer_channel_add(*first, *channel_image);
  first->kind_subtype = 4;
  first->system_properties = bke::idprop::create_group("kind").release();
  IDP_AddToGroup(first->system_properties, bke::idprop::create("gain", 0.5f).release());
  MaterialPaintLayer *second = paint_layer_add(*ma, "Second");
  const bUUID second_marker = second->marker;
  ma->active_layer_marker = second_marker;

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);

  /* The parameters are owned by the row: the copy has its own group, equal in value. */
  MaterialPaintLayer *copy_params_row = paint_layers_first(*copy);
  ASSERT_NE(copy_params_row, nullptr);
  EXPECT_EQ(copy_params_row->kind_subtype, 4);
  ASSERT_NE(copy_params_row->system_properties, nullptr);
  EXPECT_NE(copy_params_row->system_properties, first->system_properties);
  EXPECT_FLOAT_EQ(IDP_float_get(IDP_GetPropertyFromGroup(copy_params_row->system_properties, "gain")),
                  0.5f);

  EXPECT_TRUE(BKE_paint_layers_is_layered(*copy));
  EXPECT_TRUE(BLI_uuid_equal(copy->active_layer_marker, second_marker));

  MaterialPaintLayer *copy_first = paint_layers_first(*copy);
  ASSERT_NE(copy_first, nullptr);
  EXPECT_NE(copy_first, first);
  EXPECT_STREQ(copy_first->name, "First");
  EXPECT_TRUE(BLI_uuid_equal(copy_first->marker, first_marker));

  /* The channel array is deep-copied, and an editable map is cloned: painting on the copy must
   * never reach the original. */
  ASSERT_EQ(copy_first->channels_num, 1);
  ASSERT_NE(copy_first->channels, nullptr);
  EXPECT_NE(copy_first->channels, first->channels);
  ASSERT_NE(copy_first->channels[0].image, nullptr);
  EXPECT_NE(copy_first->channels[0].image, channel_image);
  EXPECT_EQ(copy_first->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  MaterialPaintLayer *copy_second = copy_first->next;
  ASSERT_NE(copy_second, nullptr);
  EXPECT_NE(copy_second, second);
  EXPECT_STREQ(copy_second->name, "Second");
  EXPECT_TRUE(BLI_uuid_equal(copy_second->marker, second_marker));

  /* Deep copy: an edit on the copy must not reach the source. */
  STRNCPY(copy_first->name, "Renamed");
  EXPECT_STREQ(first->name, "First");

  BKE_id_free(bmain, copy);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Description edits
 * \{ */

TEST_F(PaintLayersDescription, add_appends_and_assigns_unique_marker)
{
  Material *ma = BKE_material_add(bmain, "AddMat");
  ma->paint_layers_flag = {};

  MaterialPaintLayer *first = BKE_paint_layers_add(*ma, "First", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(first, nullptr);
  EXPECT_STREQ(first->name, "First");
  EXPECT_NE(first->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_FLOAT_EQ(first->opacity, 1.0f);
  EXPECT_FALSE(BLI_uuid_is_nil(first->marker));
  EXPECT_EQ(paint_layers_first(*ma), first);

  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, "Second", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(second->name, "Second");
  /* A fresh marker, distinct from the first row. */
  EXPECT_FALSE(BLI_uuid_equal(first->marker, second->marker));
  /* Appended on top: the list is bottom-to-top, so the new row is the tail. */
  EXPECT_EQ(paint_layers_first(*ma), first);
  EXPECT_EQ(first->next, second);
  EXPECT_EQ(second->prev, first);
  EXPECT_EQ(second->next, nullptr);

  /* A material that owns a description is a layered material. */
  EXPECT_TRUE(BKE_paint_layers_is_layered(*ma));
  /* A description mutation marks the derived state stale. */
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersDescription, add_places_relative_to_the_anchor)
{
  Material *ma = BKE_material_add(bmain, "PlaceMat");
  MaterialPaintLayer *middle = BKE_paint_layers_add(
      *ma, "Middle", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *above = BKE_paint_layers_add(*ma, "Above", middle, PaintLayerPlace::Above);
  MaterialPaintLayer *below = BKE_paint_layers_add(*ma, "Below", middle, PaintLayerPlace::Below);
  ASSERT_NE(above, nullptr);
  ASSERT_NE(below, nullptr);

  /* Bottom to top: Below, Middle, Above. */
  EXPECT_EQ(paint_layers_first(*ma), below);
  EXPECT_EQ(below->next, middle);
  EXPECT_EQ(middle->next, above);
  EXPECT_EQ(above->next, nullptr);
}

TEST_F(PaintLayersDescription, remove_frees_and_clears_active_when_needed)
{
  Material *ma = BKE_material_add(bmain, "RemoveMat");
  MaterialPaintLayer *first = BKE_paint_layers_add(*ma, "First", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, "Second", nullptr, PaintLayerPlace::Above);
  const bUUID first_marker = first->marker;
  const bUUID second_marker = second->marker;

  BKE_paint_layers_active_set(*ma, second_marker);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), second_marker));

  /* Removing the active row unlinks and frees it, and clears the active marker since it named a row
   * that no longer exists. */
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, second));
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
  EXPECT_EQ(paint_layers_first(*ma), first);
  EXPECT_EQ(BKE_paint_layers_find(*ma, second_marker), nullptr);
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(*ma)));

  /* The surviving row is intact and findable by marker. */
  EXPECT_EQ(BKE_paint_layers_find(*ma, first_marker), first);

  /* Removing a row that is not active leaves the active marker alone. */
  BKE_paint_layers_active_set(*ma, first_marker);
  MaterialPaintLayer *other = BKE_paint_layers_add(*ma, "Other", nullptr, PaintLayerPlace::Above);
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, other));
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), first_marker));

  /* A row that is not part of the stack is refused. */
  EXPECT_FALSE(BKE_paint_layers_remove(*ma, nullptr));
}

TEST_F(PaintLayersDescription, remove_releases_the_users_of_channel_images)
{
  Material *ma = BKE_material_add(bmain, "ReleaseMat");
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ReleaseImage"));
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  const int users_before = image->id.us;
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
  EXPECT_EQ(image->id.us, users_before + 1);

  /* A map swapped for another gives its user back. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr));
  EXPECT_EQ(image->id.us, users_before);

  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, layer));
  EXPECT_EQ(image->id.us, users_before);
}

TEST_F(PaintLayersDescription, channel_remove_releases_the_user_of_the_image)
{
  Material *ma = BKE_material_add(bmain, "ChannelReleaseMat");
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ChannelReleaseImage"));
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  const int users_before = image->id.us;
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image));
  EXPECT_EQ(image->id.us, users_before + 1);

  EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(image->id.us, users_before);
}

TEST_F(PaintLayersDescription, find_resolves_by_marker_never_by_position)
{
  Material *ma = BKE_material_add(bmain, "FindMat");
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *top = BKE_paint_layers_add(*ma, "Top", nullptr, PaintLayerPlace::Above);

  EXPECT_EQ(BKE_paint_layers_find(*ma, bottom->marker), bottom);
  EXPECT_EQ(BKE_paint_layers_find(*ma, top->marker), top);

  /* The marker keeps resolving to the same row after a reorder. */
  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, top, 0));
  EXPECT_EQ(BKE_paint_layers_find(*ma, top->marker), top);

  /* A marker no row carries resolves to nothing, even though rows exist. */
  const bUUID unknown = BLI_uuid_generate_random();
  EXPECT_EQ(BKE_paint_layers_find(*ma, unknown), nullptr);
}

TEST_F(PaintLayersDescription, duplicate_markers_resolve_deterministically_and_are_not_reused)
{
  Material *ma = BKE_material_add(bmain, "DuplicateMarkerMat");
  MaterialPaintLayer *first = BKE_paint_layers_add(*ma, "First", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, "Second", nullptr, PaintLayerPlace::Above);

  /* Force the pathological state the uniqueness guarantee exists to avoid: one marker on two
   * rows. */
  const bUUID duplicate = BLI_uuid_generate_random();
  first->marker = duplicate;
  second->marker = duplicate;

  /* find() walks the list in order, so the first row is the deterministic winner. */
  EXPECT_EQ(BKE_paint_layers_find(*ma, duplicate), first);

  MaterialPaintLayer *added = BKE_paint_layers_add(*ma, "Added", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(added, nullptr);
  EXPECT_FALSE(BLI_uuid_equal(duplicate, added->marker));
}

TEST_F(PaintLayersDescription, add_rejects_foreign_anchor)
{
  Material *owner = BKE_material_add(bmain, "AnchorOwnerMat");
  Material *foreign = BKE_material_add(bmain, "AnchorForeignMat");
  MaterialPaintLayer *top = BKE_paint_layers_add(*owner, "Top", nullptr, PaintLayerPlace::Above);

  /* A row may never be linked above or below a row of another material. */
  EXPECT_EQ(BKE_paint_layers_add(*foreign, "Above", top, PaintLayerPlace::Above), nullptr);
  EXPECT_EQ(BKE_paint_layers_add(*foreign, "Below", top, PaintLayerPlace::Below), nullptr);
  EXPECT_TRUE(BLI_listbase_is_empty(&foreign->paint_layers));
  EXPECT_FALSE(BKE_paint_layers_is_layered(*foreign));
}

TEST_F(PaintLayersDescription, move_relocates_row_and_refuses_foreign_anchor)
{
  Material *ma = BKE_material_add(bmain, "MoveMat");
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *middle = BKE_paint_layers_add(
      *ma, "Middle", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *top = BKE_paint_layers_add(*ma, "Top", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, middle->marker);

  /* Below the bottom row: markers untouched, so the active marker keeps resolving. */
  EXPECT_TRUE(BKE_paint_layers_move(*ma, middle, bottom, PaintLayerPlace::Below));
  EXPECT_EQ(paint_layers_first(*ma), middle);
  EXPECT_EQ(middle->next, bottom);
  EXPECT_EQ(bottom->next, top);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), middle->marker));

  /* A row may not be its own anchor. */
  EXPECT_FALSE(BKE_paint_layers_move(*ma, middle, middle, PaintLayerPlace::Above));

  /* A null anchor parks the row on top of the stack. */
  EXPECT_TRUE(BKE_paint_layers_move(*ma, middle, nullptr, PaintLayerPlace::Above));
  EXPECT_EQ(paint_layers_first(*ma), bottom);
  EXPECT_EQ(bottom->next, top);
  EXPECT_EQ(top->next, middle);

  /* Above an anchor in the middle. */
  EXPECT_TRUE(BKE_paint_layers_move(*ma, middle, bottom, PaintLayerPlace::Above));
  EXPECT_EQ(paint_layers_first(*ma), bottom);
  EXPECT_EQ(bottom->next, middle);
  EXPECT_EQ(middle->next, top);

  /* A foreign anchor is refused, like in add(). */
  Material *other = BKE_material_add(bmain, "MoveOtherMat");
  MaterialPaintLayer *foreign = BKE_paint_layers_add(
      *other, "Foreign", nullptr, PaintLayerPlace::Above);
  EXPECT_FALSE(BKE_paint_layers_move(*ma, middle, foreign, PaintLayerPlace::Above));
  EXPECT_FALSE(BKE_paint_layers_move(*ma, foreign, bottom, PaintLayerPlace::Above));
}

TEST_F(PaintLayersDescription, reorder_moves_row_and_clamps_the_index)
{
  Material *ma = BKE_material_add(bmain, "ReorderMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(*ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(*ma, "C", nullptr, PaintLayerPlace::Above);

  /* Bottom-to-top: index 0 is the bottom row. The index is clamped to the list. */
  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, a, 99));
  EXPECT_EQ(paint_layers_first(*ma), b);
  EXPECT_EQ(b->next, c);
  EXPECT_EQ(c->next, a);

  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, a, 0));
  EXPECT_EQ(paint_layers_first(*ma), a);
  EXPECT_EQ(a->next, b);
  EXPECT_EQ(b->next, c);

  /* A middle slot. */
  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, c, 1));
  EXPECT_EQ(paint_layers_first(*ma), a);
  EXPECT_EQ(a->next, c);
  EXPECT_EQ(c->next, b);

  EXPECT_FALSE(BKE_paint_layers_reorder(*ma, nullptr, 0));
}

TEST_F(PaintLayersDescription, channel_add_remove_and_set_enabled)
{
  Material *ma = BKE_material_add(bmain, "ChannelMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  EXPECT_EQ(layer->channels_num, 0);

  MaterialPaintLayerChannel *base = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(base->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(base->image, nullptr);

  MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(layer->channels_num, 2);

  /* Adding a channel twice leaves one record, the one already there. */
  EXPECT_EQ(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS), rough);
  EXPECT_EQ(layer->channels_num, 2);

  /* Disabling keeps the record; enabling an absent channel is refused -- adding one is an explicit
   * step. */
  EXPECT_TRUE(
      BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_EQ(layer->channels[0].state, MA_PAINT_LAYER_CHANNEL_DISABLED);
  EXPECT_FALSE(
      BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL, true));

  EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_EQ(layer->channels_num, 1);
  EXPECT_EQ(layer->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_FALSE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(layer->channels_num, 0);
  EXPECT_EQ(layer->channels, nullptr);
}

TEST_F(PaintLayersDescription, value_setters_write_and_tag)
{
  Material *ma = BKE_material_add(bmain, "ValueMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  /* Opacity is clamped to [0, 1]. A value edit asks the bake planner for a re-check and leaves the
   * graph shape alone, so it is not a regeneration. */
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 2.0f));
  EXPECT_FLOAT_EQ(layer->opacity, 1.0f);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.4f));
  EXPECT_FLOAT_EQ(layer->opacity, 0.4f);

  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(layer->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, true));
  EXPECT_NE(layer->flag & MA_PAINT_LAYER_ENABLED, 0);

  EXPECT_TRUE(BKE_paint_layers_rename(*ma, layer, "Renamed"));
  EXPECT_STREQ(layer->name, "Renamed");

  const float value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  EXPECT_TRUE(BKE_paint_layers_channel_set_value(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value));
  EXPECT_FLOAT_EQ(layer->channels[0].value[0], 0.25f);
  EXPECT_FLOAT_EQ(layer->channels[0].value[2], 0.75f);
  /* A channel the layer has no record for is refused. */
  EXPECT_FALSE(
      BKE_paint_layers_channel_set_value(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
}

TEST_F(PaintLayersDescription, active_set_moves_the_cursor_without_marking_the_stack)
{
  Material *ma = BKE_material_add(bmain, "ActiveMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ma->paint_layers_flag = MA_PAINT_LAYERED;

  BKE_paint_layers_active_set(*ma, layer->marker);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), layer->marker));
  /* The cursor is not part of the stack: nothing derived from the description goes stale. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  BKE_paint_layers_active_set(*ma, {});
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(*ma)));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RNA data API
 * \{ */

/* The collection's own RNA type wraps the owning material, so call its functions through the
 * pointer that #RNA_property_collection_type_get stamps with the collection's #StructRNA. */
static PointerRNA paint_layers_collection_ptr(PointerRNA &ma_ptr)
{
  PropertyRNA *prop = RNA_struct_find_property(&ma_ptr, "paint_layers");
  PointerRNA coll_ptr = PointerRNA_NULL;
  if (prop != nullptr) {
    RNA_property_collection_type_get(&ma_ptr, prop, &coll_ptr);
  }
  return coll_ptr;
}

static MaterialPaintLayer *rna_paint_layers_new(PointerRNA &coll_ptr, const char *name)
{
  FunctionRNA *func = RNA_struct_find_function(coll_ptr.type, "new");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &coll_ptr, func);
  const char *name_arg = name;
  RNA_parameter_set_lookup(&parms, "name", &name_arg);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  bContext *C = CTX_create();
  CTX_data_main_set(C, G_MAIN);
  RNA_function_call(C, &reports, &coll_ptr, func, &parms);
  CTX_free(C);
  BKE_reports_free(&reports);

  void *ret = nullptr;
  RNA_parameter_get_lookup(&parms, "layer", &ret);
  MaterialPaintLayer *layer = ret != nullptr ? *static_cast<MaterialPaintLayer **>(ret) : nullptr;
  RNA_parameter_list_free(&parms);
  return layer;
}

static MaterialPaintLayer *rna_paint_layers_find(PointerRNA &coll_ptr, const char *marker)
{
  FunctionRNA *func = RNA_struct_find_function(coll_ptr.type, "find");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &coll_ptr, func);
  const char *marker_arg = marker;
  RNA_parameter_set_lookup(&parms, "marker", &marker_arg);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  bContext *C = CTX_create();
  CTX_data_main_set(C, G_MAIN);
  RNA_function_call(C, &reports, &coll_ptr, func, &parms);
  CTX_free(C);
  BKE_reports_free(&reports);

  void *ret = nullptr;
  RNA_parameter_get_lookup(&parms, "layer", &ret);
  MaterialPaintLayer *layer = ret != nullptr ? *static_cast<MaterialPaintLayer **>(ret) : nullptr;
  RNA_parameter_list_free(&parms);
  return layer;
}

static void rna_paint_layers_remove(PointerRNA &coll_ptr, MaterialPaintLayer &layer)
{
  FunctionRNA *func = RNA_struct_find_function(coll_ptr.type, "remove");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &coll_ptr, func);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, &layer);
  /* A non-thick-wrapped #PARM_RNAPTR pointer parameter is stored as a pointer to a
   * #PointerRNA, so hand #RNA_parameter_set the address of that pointer. */
  PointerRNA *layer_ptr_p = &layer_ptr;
  RNA_parameter_set_lookup(&parms, "layer", &layer_ptr_p);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  bContext *C = CTX_create();
  CTX_data_main_set(C, G_MAIN);
  RNA_function_call(C, &reports, &coll_ptr, func, &parms);
  CTX_free(C);
  BKE_reports_free(&reports);
  RNA_parameter_list_free(&parms);
}

static std::string rna_paint_layer_marker(PointerRNA &layer_ptr)
{
  PropertyRNA *prop = RNA_struct_find_property(&layer_ptr, "marker");
  return RNA_property_string_get(&layer_ptr, prop);
}

TEST_F(PaintLayersDescription, rna_paint_layers_collection_is_registered)
{
  Material *ma = BKE_material_add(bmain, "RnaLayersMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  ASSERT_TRUE(RNA_struct_is_a(ma_ptr.type, RNA_Material));

  PropertyRNA *prop = RNA_struct_find_property(&ma_ptr, "paint_layers");
  ASSERT_NE(prop, nullptr);
  EXPECT_EQ(std::string(RNA_property_identifier(prop)), "paint_layers");
  EXPECT_EQ(RNA_property_type(prop), PROP_COLLECTION);
  EXPECT_EQ(RNA_property_collection_length(&ma_ptr, prop), 0);

  PropertyRNA *is_layered = RNA_struct_find_property(&ma_ptr, "is_layered");
  ASSERT_NE(is_layered, nullptr);
  EXPECT_FALSE(RNA_property_boolean_get(&ma_ptr, is_layered));

  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);
  EXPECT_EQ(std::string(RNA_struct_identifier(coll_ptr.type)), "MaterialPaintLayers");

  const StructRNA *item_type = RNA_struct_find("MaterialPaintLayer");
  ASSERT_NE(item_type, nullptr);
  EXPECT_EQ(std::string(RNA_struct_identifier(item_type)), "MaterialPaintLayer");

  /* Every property of a row exists with the expected RNA type and editability. */
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  PointerRNA dummy = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);

  PropertyRNA *layer_name = RNA_struct_find_property(&dummy, "name");
  ASSERT_NE(layer_name, nullptr);
  EXPECT_EQ(RNA_property_type(layer_name), PROP_STRING);
  EXPECT_NE(RNA_property_flag(layer_name) & PROP_EDITABLE, 0);

  PropertyRNA *layer_marker = RNA_struct_find_property(&dummy, "marker");
  ASSERT_NE(layer_marker, nullptr);
  EXPECT_EQ(RNA_property_type(layer_marker), PROP_STRING);
  EXPECT_EQ(RNA_property_flag(layer_marker) & PROP_EDITABLE, 0);

  PropertyRNA *layer_opacity = RNA_struct_find_property(&dummy, "opacity");
  ASSERT_NE(layer_opacity, nullptr);
  EXPECT_EQ(RNA_property_type(layer_opacity), PROP_FLOAT);
  /* Opacity is exposed as a percentage, so the default reads as 100. */
  EXPECT_FLOAT_EQ(RNA_property_float_get(&dummy, layer_opacity), 100.0f);

  PropertyRNA *layer_enabled = RNA_struct_find_property(&dummy, "enabled");
  ASSERT_NE(layer_enabled, nullptr);
  EXPECT_EQ(RNA_property_type(layer_enabled), PROP_BOOLEAN);
  EXPECT_TRUE(RNA_property_boolean_get(&dummy, layer_enabled));

  PropertyRNA *layer_channels = RNA_struct_find_property(&dummy, "channels");
  ASSERT_NE(layer_channels, nullptr);
  EXPECT_EQ(RNA_property_type(layer_channels), PROP_COLLECTION);
  EXPECT_EQ(RNA_property_collection_length(&dummy, layer_channels), 0);

  PropertyRNA *active = RNA_struct_find_property(&coll_ptr, "active");
  ASSERT_NE(active, nullptr);
  EXPECT_EQ(RNA_property_type(active), PROP_POINTER);
  EXPECT_EQ(RNA_property_pointer_type(&coll_ptr, active), item_type);
}

TEST_F(PaintLayersDescription, rna_opacity_and_enabled_reach_dna)
{
  Material *ma = BKE_material_add(bmain, "RnaValueMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, "L");
  ASSERT_NE(layer, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);

  PropertyRNA *opacity = RNA_struct_find_property(&layer_ptr, "opacity");
  PropertyRNA *enabled = RNA_struct_find_property(&layer_ptr, "enabled");
  PropertyRNA *name = RNA_struct_find_property(&layer_ptr, "name");
  ASSERT_NE(opacity, nullptr);
  ASSERT_NE(enabled, nullptr);
  ASSERT_NE(name, nullptr);

  /* Opacity is exposed as a percentage: 50 (percent) stores 0.5. */
  RNA_property_float_set(&layer_ptr, opacity, 50.0f);
  EXPECT_FLOAT_EQ(layer->opacity, 0.5f);

  RNA_property_boolean_set(&layer_ptr, enabled, false);
  EXPECT_EQ(layer->flag & MA_PAINT_LAYER_ENABLED, 0);

  RNA_property_string_set(&layer_ptr, name, "Renamed");
  EXPECT_STREQ(layer->name, "Renamed");
}

TEST_F(PaintLayersDescription, rna_channel_add_and_image_reach_dna)
{
  Material *ma = BKE_material_add(bmain, "RnaChannelMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, "L");
  ASSERT_NE(layer, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);

  /* The Add policy gave the new row its default channels, which the collection exposes. */
  PropertyRNA *channels = RNA_struct_find_property(&layer_ptr, "channels");
  ASSERT_NE(channels, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&layer_ptr, channels), layer->channels_num);
  EXPECT_GT(layer->channels_num, 0);

  MaterialPaintLayerChannel *base = paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(base, nullptr);
  PointerRNA channel_ptr = RNA_pointer_create_with_parent(
      layer_ptr, RNA_MaterialPaintLayerChannel, base);

  PropertyRNA *state = RNA_struct_find_property(&channel_ptr, "state");
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(RNA_property_enum_get(&channel_ptr, state), MA_PAINT_LAYER_CHANNEL_ENABLED);

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "RnaChannelImage"));
  PropertyRNA *image_prop = RNA_struct_find_property(&channel_ptr, "image");
  ASSERT_NE(image_prop, nullptr);
  PointerRNA image_ptr = RNA_id_pointer_create(&image->id);
  RNA_property_pointer_set(&channel_ptr, image_prop, image_ptr, nullptr);
  EXPECT_EQ(base->image, image);

  PropertyRNA *value = RNA_struct_find_property(&channel_ptr, "value");
  ASSERT_NE(value, nullptr);
  const float color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  RNA_property_float_set_array(&channel_ptr, value, color);
  EXPECT_FLOAT_EQ(base->value[1], 0.2f);
}

TEST_F(PaintLayersDescription, rna_paint_layers_new_find_remove_round_trip)
{
  Material *ma = BKE_material_add(bmain, "RnaRoundTripMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PropertyRNA *prop = RNA_struct_find_property(&ma_ptr, "paint_layers");
  ASSERT_NE(prop, nullptr);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);

  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, "L1");
  ASSERT_NE(layer, nullptr);
  EXPECT_STREQ(layer->name, "L1");
  EXPECT_EQ(RNA_property_collection_length(&ma_ptr, prop), 1);
  EXPECT_TRUE(BKE_paint_layers_is_layered(*ma));

  /* The new row is exposed as the collection's only item. */
  CollectionPropertyIterator iter;
  RNA_property_collection_begin(&ma_ptr, prop, &iter);
  ASSERT_TRUE(iter.valid);
  EXPECT_EQ(iter.ptr.data, layer);
  EXPECT_EQ(std::string(RNA_struct_identifier(iter.ptr.type)), "MaterialPaintLayer");

  const std::string marker = rna_paint_layer_marker(iter.ptr);
  EXPECT_FALSE(marker.empty());
  EXPECT_NE(marker, "00000000-0000-0000-0000-000000000000");
  const bUUID marker_uuid = layer->marker;

  /* The marker round-trips through find() and resolves to the same DNA row. */
  MaterialPaintLayer *found = rna_paint_layers_find(coll_ptr, marker.c_str());
  EXPECT_EQ(found, layer);

  /* find() on an unknown marker returns nothing rather than falling back to a position. */
  EXPECT_EQ(rna_paint_layers_find(coll_ptr, "11111111-1111-1111-1111-111111111111"), nullptr);

  /* new() moves the active cursor onto the fresh row. */
  PointerRNA active_ptr = RNA_property_pointer_get(
      &coll_ptr, RNA_struct_find_property(&coll_ptr, "active"));
  EXPECT_EQ(active_ptr.data, layer);
  EXPECT_EQ(active_ptr.type, RNA_MaterialPaintLayer);

  RNA_property_collection_end(&iter);

  rna_paint_layers_remove(coll_ptr, *layer);
  EXPECT_EQ(RNA_property_collection_length(&ma_ptr, prop), 0);
  EXPECT_EQ(BKE_paint_layers_find(*ma, marker_uuid), nullptr);
}

/** \} */

TEST_F(PaintLayersDescription, add_fill_layer_lays_its_colour)
{
  Material *ma = BKE_material_add(bmain, "FillMat");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  params.fill_color = color;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_STREQ(fill->name, "Fill 1");

  float read[4];
  BKE_paint_layers_base_color_get(*fill, read);
  EXPECT_FLOAT_EQ(read[0], 1.0f);
  EXPECT_FLOAT_EQ(read[3], 1.0f);
  EXPECT_TRUE(BKE_paint_layers_base_color_active(*fill));

  /* A Fill lays its constant whatever its alpha, so a transparent colour is still its value. */
  const float transparent[4] = {0.2f, 0.4f, 0.6f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, transparent));
  BKE_paint_layers_base_color_get(*fill, read);
  EXPECT_FLOAT_EQ(read[1], 0.4f);
  EXPECT_FLOAT_EQ(read[3], 0.0f);
}

TEST_F(PaintLayersDescription, add_empty_fill_layer_starts_with_no_channels)
{
  Material *ma = BKE_material_add(bmain, "EmptyFillMat");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.empty = true;
  /* Ignored by an empty Fill: there are no channel records to write a colour into. */
  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  params.fill_color = color;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_STREQ(fill->name, "Fill 1");
  EXPECT_EQ(fill->channels_num, 0);
  EXPECT_FALSE(BKE_paint_layers_base_color_active(*fill));
}

TEST_F(PaintLayersDescription, add_empty_fill_layer_starts_with_the_chosen_channels)
{
  Material *ma = BKE_material_add(bmain, "EmptyFillChannelsMat");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.empty = true;
  params.channels = (uint32_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC) |
                    (uint32_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->channels_num, 2);
  EXPECT_NE(paint_layer_channel_find(*fill, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  EXPECT_NE(paint_layer_channel_find(*fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
}

TEST_F(PaintLayersDescription, add_empty_fill_layer_skips_a_channel_outside_the_set)
{
  Material *ma = BKE_material_add(bmain, "EmptyFillOutsideSetMat");
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC)));

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  params.empty = true;
  params.channels = uint32_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(fill, nullptr);
  /* The channel is outside the material's set, so the row gets no inert record for it. */
  EXPECT_EQ(fill->channels_num, 0);
  EXPECT_EQ(paint_layer_channel_find(*fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
}

TEST_F(PaintLayersDescription, set_source_flips_between_paint_and_fill)
{
  Material *ma = BKE_material_add(bmain, "SrcMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_default_channels_apply(*ma, *layer);
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  float fill[4];
  BKE_paint_layers_base_color_get(*layer, fill);
  EXPECT_FLOAT_EQ(fill[3], 1.0f) << "a transparent Base Color becomes an opaque Fill";

  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  /* A Paint row lays nothing until painted: the Fill's colour must not stay behind. */
  float paint[4];
  BKE_paint_layers_base_color_get(*layer, paint);
  EXPECT_FLOAT_EQ(paint[3], 0.0f);
}

TEST_F(PaintLayersDescription, set_blend_writes_and_refuses_the_internal_normal_combine)
{
  Material *ma = BKE_material_add(bmain, "BlendMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, "L", nullptr, PaintLayerPlace::Above);

  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);

  /* The Normal channel forces the combine operation on its own; a stored value would only ever be
   * a switch the user cannot make effective. */
  EXPECT_FALSE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_NORMAL_COMBINE));
  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
}

TEST_F(PaintLayersDescription, blend_to_ramp_covers_every_description_blend)
{
  /* Every description blend must translate to a code an engine reads: a Mix node mode, or one of
   * the extended paint modes past its range. The code-level and description-level "is extended"
   * tests must agree, and nothing may land in the gap between the two ranges, which is the normal
   * combine's. */
  for (int blend = MA_PAINT_LAYER_BLEND_MIX; blend <= MA_PAINT_LAYER_BLEND_DARKER_COLOR; blend++) {
    const eMaterialPaintLayerBlend mode = eMaterialPaintLayerBlend(blend);
    const int ramp = BKE_paint_layers_blend_to_ramp(mode);
    const bool extended = BKE_paint_layers_blend_is_extended(mode);
    EXPECT_EQ(paint_blend_code_is_extended(ramp), extended) << "blend " << blend;
    if (extended) {
      EXPECT_GE(ramp, MA_PAINT_BLEND_HARD_LIGHT) << "blend " << blend;
      EXPECT_LE(ramp, MA_PAINT_BLEND_DARKER_COLOR) << "blend " << blend;
    }
    else {
      EXPECT_GE(ramp, MA_RAMP_BLEND) << "blend " << blend;
      EXPECT_LE(ramp, MA_RAMP_EXCLUSION) << "blend " << blend;
    }
  }

  /* The shared ".PBR Blend" group's Mode input is what selects one of those codes at draw time: its
   * bounds must reach both ends of the two ranges, so no description blend is unreadable. */
  bNodeTree *group = BKE_paint_material_blend_group_ensure(*bmain);
  ASSERT_NE(group, nullptr);
  const bNodeTreeInterfaceSocket *mode_socket = nullptr;
  for (const bNodeTreeInterfaceItem *item : group->tree_interface.root_panel.items()) {
    if (item->item_type != NodeTreeInterfaceItemType::Socket) {
      continue;
    }
    const bNodeTreeInterfaceSocket *socket = reinterpret_cast<const bNodeTreeInterfaceSocket *>(
        item);
    if (STREQ(socket->name, "Mode")) {
      mode_socket = socket;
      break;
    }
  }
  ASSERT_NE(mode_socket, nullptr);
  const auto &mode_data = *static_cast<const bNodeSocketValueInt *>(mode_socket->socket_data);
  EXPECT_EQ(mode_data.min, MA_RAMP_BLEND);
  EXPECT_EQ(mode_data.max, MA_PAINT_BLEND_DARKER_COLOR);
}

/* -------------------------------------------------------------------- */
/** \name Global channel set
 * \{ */

/** The build default: Base Color, Metallic, Roughness, Normal and AO. */
static uint32_t channel_set_default()
{
  return (uint32_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
         (uint32_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC) |
         (uint32_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS) |
         (uint32_t(1) << PAINT_MATERIAL_CHANNEL_NORMAL) |
         (uint32_t(1) << PAINT_MATERIAL_CHANNEL_AO);
}

TEST_F(PaintLayersDescription, add_materializes_default_channel_set)
{
  Material *ma = BKE_material_add(bmain, "MaterializeOnAdd");
  EXPECT_EQ(ma->paint_layers_channels, 0);

  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(ma->paint_layers_channels, channel_set_default());
}

TEST_F(PaintLayersDescription, materialize_freezes_derived_set_and_keeps_records)
{
  /* A row linked straight in stands in for a file written before the field existed: the field is
   * still zero, so the set is derived from the records and the build default. */
  Material *ma = BKE_material_add(bmain, "MaterializeOldFile");
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Paint");
  ASSERT_NE(layer, nullptr);
  ASSERT_EQ(ma->paint_layers_channels, 0);

  MaterialPaintLayerChannel *base = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  MaterialPaintLayerChannel *spec = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_SPECULAR);
  ASSERT_NE(base, nullptr);
  ASSERT_NE(spec, nullptr);
  /* A zero field is still derived, so adding a record must not have frozen anything yet. */
  ASSERT_EQ(ma->paint_layers_channels, 0);
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_SPECULAR));

  const int channels_before = layer->channels_num;
  const uint32_t expected = channel_set_default() | (uint32_t(1) << PAINT_MATERIAL_CHANNEL_SPECULAR);

  BKE_paint_layers_channels_materialize(*ma);
  EXPECT_EQ(ma->paint_layers_channels, expected);
  /* The records and their count are untouched. */
  EXPECT_EQ(layer->channels_num, channels_before);
  EXPECT_EQ(paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_SPECULAR), spec);

  /* Idempotent: a second call neither changes the set nor touches the records. */
  BKE_paint_layers_channels_materialize(*ma);
  EXPECT_EQ(ma->paint_layers_channels, expected);
  EXPECT_EQ(layer->channels_num, channels_before);
}

TEST_F(PaintLayersDescription, material_channel_set_preserves_layer_data)
{
  Material *ma = BKE_material_add(bmain, "ChannelSetKeeps");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *layer);
  ASSERT_EQ(layer->channels_num, 5);

  /* Base Color can be switched off as long as there are other channels in the set. */
  EXPECT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_FALSE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *map = BKE_image_add_generated(
      bmain, 4, 4, "SetMap", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(map, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_METALLIC);
  ASSERT_NE(record, nullptr);
  record->image = map;

  /* Dropping the channel only edits the set: the record and its map stay on the layer, and the
   * per-channel filter now answers false for it. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, false));
  EXPECT_FALSE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_EQ(paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_METALLIC), record);
  EXPECT_EQ(record->image, map);
  EXPECT_TRUE(paint_layer_channel_filtered(*ma, PAINT_MATERIAL_CHANNEL_METALLIC));

  /* Adding it back restores participation with the same map. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_METALLIC, true));
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(paint_layer_channel_filtered(*ma, PAINT_MATERIAL_CHANNEL_METALLIC));
}

TEST_F(PaintLayersDescription, material_channel_set_keeps_at_least_one_channel)
{
  Material *ma = BKE_material_add(bmain, "ChannelSetNeverEmpty");
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)));

  /* The last channel of the set cannot be removed. */
  EXPECT_FALSE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* A whole-mask write of zero is refused and leaves the stored set alone. */
  EXPECT_FALSE(BKE_paint_layers_channel_set_mask_set(*ma, 0));
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintLayersDescription, channel_add_does_not_widen_the_set)
{
  Material *ma = BKE_material_add(bmain, "ChannelAddKeepsSet");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)));

  /* A record outside the set is legal: it stays on the row and becomes visible again when the
   * channel is put back into the set. It must not silently widen the set itself. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_FALSE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NE(paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
}

TEST_F(PaintLayersDescription, filter_starts_in_the_first_channel_of_the_set)
{
  Material *ma = BKE_material_add(bmain, "FilterFirstChannel");
  MaterialPaintLayer *bottom = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)));

  /* An adjustment used to start in Base Color, which would leave it inert in a material whose set
   * does not contain Base Color. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Adjustment;
  params.filter = MA_PAINT_LAYER_FILTER_INVERT;
  MaterialPaintLayer *adjustment = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(adjustment, nullptr);
  EXPECT_NE(paint_layer_channel_find(*adjustment, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*adjustment, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
}

TEST_F(PaintLayersDescription, filter_starts_in_the_owners_only_active_channel)
{
  Material *ma = BKE_material_add(bmain, "FilterOwnerChannel");
  MaterialPaintLayer *fill = BKE_paint_layers_add(*ma, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  fill->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);

  /* The material's set has Base Color too, so the old first-of-the-set rule would pick it. The
   * Fill carries only Roughness, and the filter has to work there and nowhere else. */
  MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
      *ma, fill, MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(filter, nullptr);
  EXPECT_NE(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
}

TEST_F(PaintLayersDescription, filter_falls_back_when_the_owners_only_channel_is_outside_the_set)
{
  Material *ma = BKE_material_add(bmain, "FilterOwnerOutsideSet");
  MaterialPaintLayer *fill = BKE_paint_layers_add(*ma, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  fill->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  /* The owner's one live channel no longer takes part, so it has no channel to give the filter:
   * the first channel of the set answers instead. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma, uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC)));

  MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
      *ma, fill, MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(filter, nullptr);
  EXPECT_NE(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
}

TEST_F(PaintLayersDescription, filter_with_two_live_owner_channels_uses_the_first_of_the_set)
{
  Material *ma = BKE_material_add(bmain, "FilterOwnerTwoChannels");
  MaterialPaintLayer *fill = BKE_paint_layers_add(*ma, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  fill->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  /* Two live channels are a choice the filter cannot make; the material's set decides, and its
   * first channel is neither of the owner's. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      *ma,
      (uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC)) |
          (uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)) |
          (uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_SPECULAR))));

  MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
      *ma, fill, MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(filter, nullptr);
  EXPECT_NE(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_METALLIC), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);
  EXPECT_EQ(paint_layer_channel_find(*filter, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
}

TEST_F(PaintLayersDescription, default_channels_apply_follows_the_set)
{
  Material *ma = BKE_material_add(bmain, "ApplySet");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  /* A channel added to the set before any record is authored joins the default. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_SPECULAR, true));
  BKE_paint_layers_default_channels_apply(*ma, *layer);
  EXPECT_NE(paint_layer_channel_find(*layer, PAINT_MATERIAL_CHANNEL_SPECULAR), nullptr);
  EXPECT_EQ(layer->channels_num, 6);
}

TEST_F(PaintLayersDescription, material_channel_set_round_trip)
{
  Material *ma = BKE_material_add(bmain, "RnaChannels");
  BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);

  PointerRNA ptr = RNA_id_pointer_create(&ma->id);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "paint_layers_channels");
  ASSERT_NE(prop, nullptr);

  const int set = RNA_property_enum_get(&ptr, prop);
  EXPECT_TRUE((set & (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR)) != 0);
  EXPECT_TRUE((set & (1 << PAINT_MATERIAL_CHANNEL_AO)) != 0);

  /* Writing routes through BKE_paint_layers_channel_set_mask_set; Base Color is not forced back in. */
  RNA_property_enum_set(&ptr, prop, 1 << PAINT_MATERIAL_CHANNEL_METALLIC);
  const int after = RNA_property_enum_get(&ptr, prop);
  EXPECT_TRUE((after & (1 << PAINT_MATERIAL_CHANNEL_METALLIC)) != 0);
  EXPECT_FALSE((after & (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR)) != 0);

  /* The RNA helper answers from the same set. */
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_METALLIC));
  EXPECT_FALSE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_AO));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layer mask
 * \{ */

TEST_F(PaintLayersDescription, mask_add_creates_a_constant_base_item)
{
  Material *ma = BKE_material_add(bmain, "MaskMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_base(*layer), nullptr);

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.4f);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_base(*layer), item);
  EXPECT_EQ(item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_NE(item->flag & MA_PAINT_LAYER_MASK_BASE, 0);
  EXPECT_NE(item->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_EQ(item->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(item->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_FALSE(BLI_uuid_is_nil(item->marker));
  EXPECT_FALSE(BLI_uuid_equal(item->marker, layer->marker));

  /* The constant is the item's Base Color record: opaque grey of the given value, no map yet. */
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(record->image, nullptr);
  EXPECT_FLOAT_EQ(record->value[0], 0.4f);
  EXPECT_FLOAT_EQ(record->value[2], 0.4f);
  EXPECT_FLOAT_EQ(record->value[3], 1.0f);

  /* The item belongs to its row, not to the stack: nothing new is a row of the material, though
   * the generator addresses the item by its own marker. */
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
  EXPECT_EQ(BKE_paint_layers_find(*ma, item->marker), item);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* One base mask per row; a foreign row is refused. */
  EXPECT_EQ(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
  Material *other = BKE_material_add(bmain, "OtherMaskMat");
  MaterialPaintLayer *other_layer = BKE_paint_layers_add(
      *other, "O", nullptr, PaintLayerPlace::Above);
  EXPECT_EQ(BKE_paint_layers_mask_add(*ma, other_layer, 1.0f), nullptr);
}

TEST_F(PaintLayersDescription, mask_edits_go_through_the_item)
{
  Material *ma = BKE_material_add(bmain, "MaskEditMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);

  const float dark[4] = {0.1f, 0.1f, 0.1f, 1.0f};
  EXPECT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, dark));
  EXPECT_FLOAT_EQ(paint_layer_mask_constant(*paint_layer_channel_find(
                      *item, PAINT_MATERIAL_CHANNEL_BASE_COLOR)),
                  0.1f);

  /* The enabled flag is folded into the item's opacity, like a row's. */
  EXPECT_FLOAT_EQ(BKE_paint_layers_effective_opacity(*item), 1.0f);
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, item, false));
  EXPECT_FLOAT_EQ(BKE_paint_layers_effective_opacity(*item), 0.0f);
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, item, true));
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, item, 0.5f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_effective_opacity(*item), 0.5f);

  /* The item can be named like a correction, but it is not a row: structural edits stay refused. */
  EXPECT_TRUE(BKE_paint_layers_rename(*ma, item, "Renamed"));
  EXPECT_STREQ(item->name, "Renamed");
  EXPECT_FALSE(BKE_paint_layers_remove(*ma, item));
  EXPECT_FALSE(BKE_paint_layers_set_source(*ma, item, MA_PAINT_LAYER_SOURCE_IMAGE));
}

TEST_F(PaintLayersDescription, mask_remove_lets_go_of_its_map)
{
  Material *ma = BKE_material_add(bmain, "MaskRemoveMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);

  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "MaskMap"));
  const int users_before = map->id.us;
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  EXPECT_EQ(map->id.us, users_before + 1);

  EXPECT_TRUE(BKE_paint_layers_mask_remove(*ma, layer));
  EXPECT_EQ(BKE_paint_layers_mask_base(*layer), nullptr);
  EXPECT_EQ(map->id.us, users_before);
  EXPECT_FALSE(BKE_paint_layers_mask_remove(*ma, layer));

  /* Removing the row takes its mask with it, and its map's user too. */
  item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  EXPECT_EQ(map->id.us, users_before + 1);
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, layer));
  EXPECT_EQ(map->id.us, users_before);
}

TEST_F(PaintLayersDescription, mask_is_deep_copied_with_material_and_duplicate)
{
  Material *ma = BKE_material_add(bmain, "MaskCopyMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.3f);
  ASSERT_NE(item, nullptr);
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "MaskCopyMap"));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  /* A material copy owns its own item and record array; the map stays the shared data-block. */
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  MaterialPaintLayer *copy_layer = paint_layers_first(*copy);
  ASSERT_NE(copy_layer, nullptr);
  const MaterialPaintLayer *copy_item = BKE_paint_layers_mask_base(*copy_layer);
  ASSERT_NE(copy_item, nullptr);
  EXPECT_NE(copy_item, item);
  EXPECT_NE(copy_item->channels, item->channels);
  EXPECT_TRUE(BLI_uuid_equal(copy_item->marker, item->marker));
  ASSERT_EQ(copy_item->channels_num, 1);
  /* An editable map is cloned with the material, so the copy never paints into the original. */
  ASSERT_NE(copy_item->channels[0].image, nullptr);
  EXPECT_NE(copy_item->channels[0].image, map);
  EXPECT_NE(copy_item->flag & MA_PAINT_LAYER_MASK_BASE, 0);
  BKE_id_free(bmain, copy);

  /* Duplicating a row gives the copy its own mask and its own map: a stroke on one must not paint
   * the other, and the copy's markers stay unique. */
  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, layer);
  ASSERT_NE(duplicate, nullptr);
  const MaterialPaintLayer *duplicate_item = BKE_paint_layers_mask_base(*duplicate);
  ASSERT_NE(duplicate_item, nullptr);
  EXPECT_NE(duplicate_item, item);
  EXPECT_FALSE(BLI_uuid_equal(duplicate_item->marker, item->marker));
  ASSERT_EQ(duplicate_item->channels_num, 1);
  ASSERT_NE(duplicate_item->channels[0].image, nullptr);
  EXPECT_NE(duplicate_item->channels[0].image, map);
  EXPECT_FLOAT_EQ(duplicate_item->channels[0].value[0], 0.3f);
}

TEST_F(PaintLayersDescription, mask_foreach_id_walks_its_map)
{
  Material *ma = BKE_material_add(bmain, "MaskWalkMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 1.0f);
  ASSERT_NE(item, nullptr);
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "MaskWalkMap"));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  Vector<ID *> visited;
  auto collector = [](LibraryIDLinkCallbackData *data) -> int {
    if (*data->id_pointer != nullptr) {
      static_cast<Vector<ID *> *>(data->user_data)->append(*data->id_pointer);
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(bmain, &ma->id, collector, &visited, IDWALK_READONLY);
  EXPECT_TRUE(visited.contains(&map->id));
}

TEST_F(PaintLayersDescription, mask_blend_round_trip_keeps_item_and_map)
{
  Material *ma = BKE_material_add(bmain, "MaskRoundTripMat");
  id_us_plus(&ma->id);
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.6f);
  ASSERT_NE(item, nullptr);
  const bUUID item_marker = item->marker;
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "MaskRoundTripMap"));
  id_us_plus(&map->id);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, item, 0.5f));

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_mask_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);
  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "MaskRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_layer = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_layer, nullptr);
  const MaterialPaintLayer *reloaded_item = BKE_paint_layers_mask_base(*reloaded_layer);
  ASSERT_NE(reloaded_item, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(reloaded_item->marker, item_marker));
  EXPECT_EQ(reloaded_item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_FLOAT_EQ(reloaded_item->opacity, 0.5f);
  ASSERT_EQ(reloaded_item->channels_num, 1);
  ASSERT_NE(reloaded_item->channels, nullptr);
  EXPECT_FLOAT_EQ(reloaded_item->channels[0].value[0], 0.6f);
  /* The map is remapped on read, not left as a stale address. */
  ASSERT_NE(reloaded_item->channels[0].image, nullptr);
  EXPECT_STREQ(reloaded_item->channels[0].image->id.name + 2, "MaskRoundTripMap");

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersDescription, rna_mask_items_expose_the_item)
{
  Material *ma = BKE_material_add(bmain, "RnaMaskMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, "L");
  ASSERT_NE(layer, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);

  PropertyRNA *items = RNA_struct_find_property(&layer_ptr, "mask_items");
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&layer_ptr, items), 0);

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.7f);
  ASSERT_NE(item, nullptr);
  ASSERT_EQ(RNA_property_collection_length(&layer_ptr, items), 1);

  CollectionPropertyIterator iter;
  RNA_property_collection_begin(&layer_ptr, items, &iter);
  ASSERT_TRUE(iter.valid);
  EXPECT_EQ(iter.ptr.data, item);
  PointerRNA item_ptr = iter.ptr;
  RNA_property_collection_end(&iter);

  PropertyRNA *is_base = RNA_struct_find_property(&item_ptr, "is_mask_base");
  ASSERT_NE(is_base, nullptr);
  EXPECT_TRUE(RNA_property_boolean_get(&item_ptr, is_base));

  /* The item is addressed like a row, so its opacity and enabled flag reach DNA through RNA. */
  PropertyRNA *opacity = RNA_struct_find_property(&item_ptr, "opacity");
  PropertyRNA *enabled = RNA_struct_find_property(&item_ptr, "enabled");
  ASSERT_NE(opacity, nullptr);
  ASSERT_NE(enabled, nullptr);
  RNA_property_float_set(&item_ptr, opacity, 25.0f);
  EXPECT_FLOAT_EQ(item->opacity, 0.25f);
  RNA_property_boolean_set(&item_ptr, enabled, false);
  EXPECT_EQ(item->flag & MA_PAINT_LAYER_ENABLED, 0);

  /* Its Base Color record is reachable through the item's own channels. */
  PropertyRNA *channels = RNA_struct_find_property(&item_ptr, "channels");
  ASSERT_NE(channels, nullptr);
  ASSERT_EQ(RNA_property_collection_length(&item_ptr, channels), 1);
  MaterialPaintLayerChannel *record = &item->channels[0];
  PointerRNA record_ptr = RNA_pointer_create_with_parent(
      item_ptr, RNA_MaterialPaintLayerChannel, record);
  PropertyRNA *value = RNA_struct_find_property(&record_ptr, "value");
  ASSERT_NE(value, nullptr);
  const float color[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  RNA_property_float_set_array(&record_ptr, value, color);
  EXPECT_FLOAT_EQ(record->value[0], 0.2f);
}

TEST_F(PaintLayersDescription, rna_mask_value_is_the_constant_of_the_mask_item)
{
  Material *ma = BKE_material_add(bmain, "RnaMaskValueMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, "L");
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.4f);
  ASSERT_NE(item, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);
  PointerRNA item_ptr = RNA_pointer_create_with_parent(layer_ptr, RNA_MaterialPaintLayer, item);

  PropertyRNA *mask_value = RNA_struct_find_property(&item_ptr, "mask_value");
  ASSERT_NE(mask_value, nullptr);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&item_ptr, mask_value), 0.4f);

  /* A write lays an opaque grey in the item's record, and is a value edit: no rebuild is asked. */
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  RNA_property_float_set(&item_ptr, mask_value, 0.9f);
  EXPECT_FLOAT_EQ(item->channels[0].value[0], 0.9f);
  EXPECT_FLOAT_EQ(item->channels[0].value[2], 0.9f);
  EXPECT_FLOAT_EQ(item->channels[0].value[3], 1.0f);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* A row is not a mask: the property reads one and ignores a write. */
  EXPECT_FLOAT_EQ(RNA_property_float_get(&layer_ptr, mask_value), 1.0f);
  RNA_property_float_set(&layer_ptr, mask_value, 0.1f);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&layer_ptr, mask_value), 1.0f);
}

/** \} */

TEST_F(PaintLayersDescription, group_folds_rows_into_a_folder_at_the_lowest_position)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(ma, "C", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *members[2] = {b, c};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder));
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 2);
  EXPECT_EQ(ma.paint_layers.first, a);
  EXPECT_EQ(ma.paint_layers.last, folder);
  EXPECT_EQ(BLI_listbase_count(&folder->children), 2);
  EXPECT_EQ(BKE_paint_layers_parent(ma, *b), folder);
  EXPECT_EQ(BKE_paint_layers_find(ma, c->marker), c);
}

TEST_F(PaintLayersDescription, group_keeps_list_order_whatever_order_the_members_are_named_in)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  /* Named top first and with a repeat: the folder still holds A below B, each once. */
  MaterialPaintLayer *members[3] = {b, a, b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 3));
  ASSERT_NE(folder, nullptr);
  ASSERT_EQ(BLI_listbase_count(&folder->children), 2);
  EXPECT_EQ(folder->children.first, a);
  EXPECT_EQ(folder->children.last, b);
}

TEST_F(PaintLayersDescription, reorder_and_move_inside_a_folder_touch_only_its_children)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *low = BKE_paint_layers_add(ma, "Low", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(ma, "C", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *high = BKE_paint_layers_add(ma, "High", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *members[3] = {a, b, c};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 3));
  ASSERT_NE(folder, nullptr);
  ASSERT_EQ(folder->children.first, a);
  ASSERT_EQ(folder->children.last, c);

  /* A reorder to the bottom of the folder. */
  ASSERT_TRUE(BKE_paint_layers_reorder(ma, c, 0));
  EXPECT_EQ(folder->children.first, c);
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(folder->children.first)->next, a);
  EXPECT_EQ(folder->children.last, b);

  /* A move next to a sibling, both directions. */
  ASSERT_TRUE(BKE_paint_layers_move(ma, c, b, PaintLayerPlace::Above));
  EXPECT_EQ(folder->children.first, a);
  EXPECT_EQ(folder->children.last, c);
  ASSERT_TRUE(BKE_paint_layers_move(ma, c, a, PaintLayerPlace::Below));
  EXPECT_EQ(folder->children.first, c);
  EXPECT_EQ(folder->children.last, b);

  /* The folder keeps its members and its place; the root list is as it was. */
  EXPECT_EQ(BLI_listbase_count(&folder->children), 3);
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 3);
  EXPECT_EQ(ma.paint_layers.first, low);
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(ma.paint_layers.first)->next, folder);
  EXPECT_EQ(ma.paint_layers.last, high);
  EXPECT_EQ(BKE_paint_layers_parent(ma, *c), folder);
}

TEST_F(PaintLayersDescription, group_refuses_rows_of_different_lists)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *pair[1] = {b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(pair, 1));
  MaterialPaintLayer *mixed[2] = {a, b};
  EXPECT_EQ(BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(mixed, 2)), nullptr);
  EXPECT_NE(folder, nullptr);
}

TEST_F(PaintLayersDescription, ungroup_lifts_children_in_place_and_keeps_markers)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(ma, "C", nullptr, PaintLayerPlace::Above);
  const bUUID b_marker = b->marker;
  MaterialPaintLayer *members[1] = {b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_TRUE(BKE_paint_layers_ungroup(ma, folder));
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 3);
  EXPECT_EQ(ma.paint_layers.first, a);
  EXPECT_EQ(ma.paint_layers.last, c);
  EXPECT_EQ(BKE_paint_layers_find(ma, b_marker), b);
  EXPECT_EQ(BKE_paint_layers_parent(ma, *b), nullptr);
}

TEST_F(PaintLayersDescription, move_into_a_folder_and_never_into_its_own_subtree)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *members[1] = {b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
  EXPECT_TRUE(BKE_paint_layers_move(ma, a, folder, PaintLayerPlace::Into));
  EXPECT_EQ(BKE_paint_layers_parent(ma, *a), folder);
  /* A plain row is no place to move into, and a folder is no place inside itself. */
  EXPECT_FALSE(BKE_paint_layers_move(ma, folder, a, PaintLayerPlace::Above));
  EXPECT_FALSE(BKE_paint_layers_move(ma, b, a, PaintLayerPlace::Into));
}

TEST_F(PaintLayersDescription, folder_refuses_channel_records_and_source_change)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(folder, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder));
  EXPECT_EQ(folder->channels_num, 0);
  EXPECT_EQ(BKE_paint_layers_channel_add(ma, folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_FALSE(BKE_paint_layers_set_source(ma, folder, MA_PAINT_LAYER_SOURCE_IMAGE));
}

TEST_F(PaintLayersDescription, add_onto_a_folder_goes_inside_it)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  PaintLayerAddParams folder_params;
  folder_params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(ma, folder_params);
  PaintLayerAddParams params;
  params.anchor = folder;
  MaterialPaintLayer *child = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(BKE_paint_layers_parent(ma, *child), folder);
}

TEST_F(PaintLayersDescription, an_add_that_names_a_side_stays_beside_a_folder)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  PaintLayerAddParams folder_params;
  folder_params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(ma, folder_params);
  ASSERT_NE(folder, nullptr);

  PaintLayerAddParams params;
  params.anchor = folder;
  /* What a drop passes: its line names a side, and the folder does not swallow it. */
  params.fill_folder = false;
  MaterialPaintLayer *sibling = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(sibling, nullptr);
  EXPECT_EQ(BKE_paint_layers_parent(ma, *sibling), nullptr);
}

TEST_F(PaintLayersDescription, a_dropped_image_is_taken_by_fill_rows_only)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  PaintLayerAddParams params;
  MaterialPaintLayer *paint = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(paint, nullptr);
  params.kind = PaintLayerAddKind::Fill;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(fill, nullptr);
  params.kind = PaintLayerAddKind::CorrectionFill;
  params.anchor = paint;
  MaterialPaintLayer *fill_correction = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(fill_correction, nullptr);
  params.kind = PaintLayerAddKind::Folder;
  params.anchor = nullptr;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(folder, nullptr);

  EXPECT_EQ(BKE_paint_layers_row_image_map_issue(*paint),
            PaintLayerImageMapIssue::PaintRowOwnsMaps);
  EXPECT_EQ(BKE_paint_layers_row_image_map_issue(*fill), PaintLayerImageMapIssue::None);
  EXPECT_EQ(BKE_paint_layers_row_image_map_issue(*fill_correction), PaintLayerImageMapIssue::None);
  EXPECT_EQ(BKE_paint_layers_row_image_map_issue(*folder), PaintLayerImageMapIssue::TakesNoImage);
}

TEST_F(PaintLayersDescription, remove_and_duplicate_take_the_whole_branch)
{
  Material &ma = *BKE_material_add(bmain, "Mat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *members[1] = {a};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, ma, folder);
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*copy));
  ASSERT_EQ(BLI_listbase_count(&copy->children), 1);
  const MaterialPaintLayer *child_copy = static_cast<MaterialPaintLayer *>(copy->children.first);
  EXPECT_FALSE(BLI_uuid_equal(child_copy->marker, a->marker));
  BKE_paint_layers_active_set(ma, child_copy->marker);
  EXPECT_TRUE(BKE_paint_layers_remove(ma, copy));
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(ma)));
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 1);
}

TEST_F(PaintLayersDescription, correction_hangs_on_its_row_and_is_not_a_row_of_the_stack)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_STREQ(effect->name, "Corr");
  EXPECT_EQ(BLI_listbase_count(&row->effects), 1);
  /* Corrections are not rows: the stack, its walk and the ordering never see them. */
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 1);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *effect), row);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *row), nullptr);
  EXPECT_EQ(BKE_paint_layers_find(ma, effect->marker), effect);
  EXPECT_FALSE(BLI_uuid_equal(effect->marker, row->marker));

  /* The material's channels, transparent and without a map: nothing is laid until it is painted. */
  ASSERT_GT(effect->channels_num, 0);
  for (const int i : IndexRange(effect->channels_num)) {
    EXPECT_EQ(effect->channels[i].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
    EXPECT_EQ(effect->channels[i].image, nullptr);
    EXPECT_EQ(effect->channels[i].value[3], 0.0f);
  }
  EXPECT_FALSE(paint_layer_effects_lay(ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintLayersDescription, correction_refuses_a_correction_as_owner)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionRefuseMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *members[1] = {a};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add(ma, nullptr, "Corr"), nullptr);

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, a, "Corr");
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add(ma, effect, "Nested"), nullptr);
  /* A correction is not removed as a row, and a row not as a correction. */
  EXPECT_FALSE(BKE_paint_layers_remove(ma, effect));
  EXPECT_FALSE(BKE_paint_layers_correction_remove(ma, a));
}

TEST_F(PaintLayersDescription, a_folder_takes_a_correction_and_stops_being_pass_through)
{
  Material &ma = *BKE_material_add(bmain, "FolderCorrectionMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *members[1] = {a};
  MaterialPaintLayer *folder = BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, folder, "Corr");
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *effect), folder);
  /* Read from the list, not from visibility: a hidden correction keeps the mode (a value edit). */
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(ma, effect, false));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 1);

  ASSERT_TRUE(BKE_paint_layers_correction_remove(ma, effect));
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));

  /* Participation: a folder lays through a correction that has a map even holding nothing that
   * does, and through none otherwise. */
  EXPECT_FALSE(paint_layer_effects_lay(ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  MaterialPaintLayer *fill = BKE_paint_layers_correction_add(
      ma, folder, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(fill, nullptr);
  EXPECT_TRUE(paint_layer_effects_lay(ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
}

TEST_F(PaintLayersDescription, issues_report_a_fill_correction_that_has_no_effect_on_normal)
{
  Material &ma = *BKE_material_add(bmain, "IssueNormalMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *fill = BKE_paint_layers_correction_add(
      ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(fill, nullptr);
  MaterialPaintLayer *paint = BKE_paint_layers_correction_add(ma, row, "Paint");
  ASSERT_NE(paint, nullptr);

  Vector<PaintLayersIssue> issues;
  /* A Fill that was never given Normal does not claim to lay in it: nothing to say. */
  BKE_paint_layers_issues_get(ma, issues);
  EXPECT_TRUE(issues.is_empty());

  ASSERT_NE(BKE_paint_layers_channel_add(ma, fill, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  BKE_paint_layers_issues_get(ma, issues);
  ASSERT_EQ(issues.size(), 1);
  EXPECT_EQ(issues[0].code, PaintLayersIssueCode::FillCorrectionOnNormal);
  EXPECT_TRUE(BLI_uuid_equal(issues[0].layer, row->marker));
  EXPECT_TRUE(BLI_uuid_equal(issues[0].correction, fill->marker));
  EXPECT_EQ(issues[0].channel, int(PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_NE(issues[0].text, nullptr);

  /* A hidden one is not a problem, and a map in Normal makes it do something. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(ma, fill, false));
  issues.clear();
  BKE_paint_layers_issues_get(ma, issues);
  EXPECT_TRUE(issues.is_empty());
  ASSERT_TRUE(BKE_paint_layers_set_enabled(ma, fill, true));
  const float flat[4] = {0.5f, 0.5f, 1.0f, 1.0f};
  Image &map = *BKE_image_add_generated(
      bmain, 4, 4, "NormalMap", 32, false, IMA_GENTYPE_BLANK, flat, false, true, false);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, fill, PAINT_MATERIAL_CHANNEL_NORMAL, &map));
  issues.clear();
  BKE_paint_layers_issues_get(ma, issues);
  EXPECT_TRUE(issues.is_empty());
}

TEST_F(PaintLayersDescription, issues_report_a_fill_without_base_color_in_the_set)
{
  Material &ma = *BKE_material_add(bmain, "IssueFillNoBase");
  MaterialPaintLayer *paint_row = BKE_paint_layers_add(
      ma, "Paint", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *fill = BKE_paint_layers_add(ma, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_source(ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  MaterialPaintLayer *fill_correction = BKE_paint_layers_correction_add(
      ma, paint_row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(fill_correction, nullptr);

  /* Base Color outside the set: both Fill shapes have nothing to lay their colour through. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_mask_set(
      ma, uint32_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)));
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(ma, issues);
  ASSERT_EQ(issues.size(), 2);
  bool fill_issue = false;
  bool correction_issue = false;
  for (const PaintLayersIssue &issue : issues) {
    EXPECT_EQ(issue.code, PaintLayersIssueCode::FillWithoutBaseColor);
    EXPECT_EQ(issue.channel, int(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
    EXPECT_NE(issue.text, nullptr);
    fill_issue |= BLI_uuid_equal(issue.layer, fill->marker) &&
                  BLI_uuid_is_nil(issue.correction);
    correction_issue |= BLI_uuid_equal(issue.layer, paint_row->marker) &&
                        BLI_uuid_equal(issue.correction, fill_correction->marker);
  }
  EXPECT_TRUE(fill_issue);
  EXPECT_TRUE(correction_issue);

  /* Base Color back in the set makes both meaningful again: nothing to report. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));
  issues.clear();
  BKE_paint_layers_issues_get(ma, issues);
  EXPECT_TRUE(issues.is_empty());
}

TEST_F(PaintLayersDescription, issues_report_a_folder_with_a_map_and_a_row_with_children)
{
  Material &ma = *BKE_material_add(bmain, "IssueShapeMat");
  /* Rows linked straight into the list, the way a file read or an edit around the API leaves them. */
  MaterialPaintLayer *folder = paint_layer_add(ma, "Folder");
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  folder->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
  folder->channels_num = 1;
  folder->channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  folder->channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  folder->channels[0].gradient_ramp = nullptr;
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  folder->channels[0].image = BKE_image_add_generated(
      bmain, 4, 4, "FolderMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  MaterialPaintLayer *row = paint_layer_add(ma, "Row");
  MaterialPaintLayer *stray = MEM_new<MaterialPaintLayer>(__func__);
  STRNCPY(stray->name, "Stray");
  stray->marker = BLI_uuid_generate_random();
  stray->flag = MA_PAINT_LAYER_ENABLED;
  stray->opacity = 1.0f;
  BLI_addtail(&row->children, stray);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(ma, issues);
  ASSERT_EQ(issues.size(), 2);
  bool folder_maps = false;
  bool children = false;
  for (const PaintLayersIssue &issue : issues) {
    folder_maps |= issue.code == PaintLayersIssueCode::FolderHasMaps &&
                   BLI_uuid_equal(issue.layer, folder->marker);
    children |= issue.code == PaintLayersIssueCode::NonFolderHasChildren &&
                BLI_uuid_equal(issue.layer, row->marker);
  }
  EXPECT_TRUE(folder_maps);
  EXPECT_TRUE(children);
}

TEST_F(PaintLayersDescription, issues_report_a_kind_that_is_unknown_or_not_allowed_in_its_role)
{
  Material &ma = *BKE_material_add(bmain, "IssueKindMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  MaterialPaintLayer *other = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(ma, issues);
  EXPECT_TRUE(issues.is_empty());

  /* A folder as a correction, and a kind a newer build wrote: both are skipped and both say so. */
  effect->source = MA_PAINT_LAYER_SOURCE_STACK;
  other->source = 3;
  BKE_paint_layers_issues_get(ma, issues);
  ASSERT_EQ(issues.size(), 2);
  bool correction = false;
  bool unknown = false;
  for (const PaintLayersIssue &issue : issues) {
    EXPECT_EQ(issue.code, PaintLayersIssueCode::KindNotAllowed);
    correction |= BLI_uuid_equal(issue.layer, row->marker) &&
                  BLI_uuid_equal(issue.correction, effect->marker);
    unknown |= BLI_uuid_equal(issue.layer, other->marker) && BLI_uuid_is_nil(issue.correction);
  }
  EXPECT_TRUE(correction);
  EXPECT_TRUE(unknown);
}

TEST_F(PaintLayersDescription, correction_paste_lands_a_copy_with_maps_and_settings_of_its_own)
{
  Material &ma = *BKE_material_add(bmain, "PasteCorrectionMat");
  Material &other = *BKE_material_add(bmain, "PasteCorrectionOtherMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(other, "C", nullptr, PaintLayerPlace::Above);

  /* A content correction with a map, from this material and from another. */
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, a, "Corr");
  ASSERT_NE(effect, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "CorrMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(ma, effect, 0.4f));

  MaterialPaintLayer *copy = BKE_paint_layers_correction_paste(*bmain, ma, *effect, b);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *copy), b);
  EXPECT_FALSE(BLI_uuid_equal(copy->marker, effect->marker));
  EXPECT_EQ(copy->opacity, 0.4f);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *copy, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  ASSERT_NE(record->image, nullptr);
  EXPECT_NE(record->image, &image);
  EXPECT_EQ(BLI_listbase_count(&a->effects), 1);
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 2);

  /* Into another material: its own marker among that one's, the same rules. */
  MaterialPaintLayer *carried = BKE_paint_layers_correction_paste(*bmain, other, *effect, c);
  ASSERT_NE(carried, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_owner(other, *carried), c);

  /* A row of the stack is not a correction, and a correction is not a target. */
  EXPECT_EQ(BKE_paint_layers_correction_paste(*bmain, ma, *a, b), nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_paste(*bmain, ma, *effect, copy), nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_paste(*bmain, ma, *effect, nullptr), nullptr);
}

TEST_F(PaintLayersDescription, mask_correction_paste_needs_a_base_and_keeps_its_inversion)
{
  Material &ma = *BKE_material_add(bmain, "PasteMaskCorrectionMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, a, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      ma, a, MA_PAINT_LAYER_SOURCE_CONSTANT, "Flat", 0.3f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_invert(
      ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, true));

  /* B has no mask, so a correction has nothing to sit over. */
  EXPECT_FALSE(BKE_paint_layers_correction_can_paste(*item, *b));
  EXPECT_EQ(BKE_paint_layers_correction_paste(*bmain, ma, *item, b), nullptr);

  ASSERT_NE(BKE_paint_layers_mask_add(ma, b, 0.5f), nullptr);
  MaterialPaintLayer *copy = BKE_paint_layers_correction_paste(*bmain, ma, *item, b);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(ma, *copy), b);
  EXPECT_EQ(copy->flag & MA_PAINT_LAYER_MASK_BASE, 0);
  EXPECT_EQ(b->mask_stack.last, copy);
  EXPECT_EQ(copy->channels[0].value[0], 0.3f);
  EXPECT_NE(copy->channels[0].flag & MA_PAINT_LAYER_CHANNEL_INVERT, 0);

  /* The base of another layer is welcome: a plain item over this one's own. */
  MaterialPaintLayer *base_copy = BKE_paint_layers_correction_paste(
      *bmain, ma, *BKE_paint_layers_mask_base(*a), b);
  ASSERT_NE(base_copy, nullptr);
  EXPECT_EQ(base_copy->flag & MA_PAINT_LAYER_MASK_BASE, 0);
  EXPECT_NE(BKE_paint_layers_mask_base(*b), base_copy);
}

TEST_F(PaintLayersDescription, correction_edits_go_through_the_same_setters_as_a_row)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionEditMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  EXPECT_TRUE(BKE_paint_layers_set_opacity(ma, effect, 0.4f));
  EXPECT_FLOAT_EQ(effect->opacity, 0.4f);
  EXPECT_TRUE(BKE_paint_layers_set_enabled(ma, effect, false));
  EXPECT_FLOAT_EQ(BKE_paint_layers_effective_opacity(*effect), 0.0f);
  EXPECT_TRUE(BKE_paint_layers_set_blend(ma, effect, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_EQ(effect->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_TRUE(BKE_paint_layers_rename(ma, effect, "Renamed"));
  EXPECT_STREQ(effect->name, "Renamed");
  /* A correction is never moved, grouped or turned into another source through the row's calls. */
  EXPECT_FALSE(BKE_paint_layers_move(ma, effect, nullptr, PaintLayerPlace::Above));
  EXPECT_FALSE(BKE_paint_layers_set_source(ma, effect, MA_PAINT_LAYER_SOURCE_CONSTANT));
}

TEST_F(PaintLayersDescription, correction_remove_lets_go_of_its_map_and_the_active_marker)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionRemoveMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CorrectionRemoveMap"));
  const int users_before = map->id.us;
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  EXPECT_EQ(map->id.us, users_before + 1);
  BKE_paint_layers_active_set(ma, effect->marker);

  ASSERT_TRUE(BKE_paint_layers_correction_remove(ma, effect));
  EXPECT_EQ(map->id.us, users_before);
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(ma)));
  EXPECT_TRUE(BLI_listbase_is_empty(&row->effects));

  /* Removing the row takes its corrections along, and lets go of their maps too. */
  MaterialPaintLayer *second = BKE_paint_layers_correction_add(ma, row, "Second");
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(ma, second, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  EXPECT_EQ(map->id.us, users_before + 1);
  ASSERT_TRUE(BKE_paint_layers_remove(ma, row));
  EXPECT_EQ(map->id.us, users_before);
}

TEST_F(PaintLayersDescription, correction_is_deep_copied_with_material_and_duplicate)
{
  Material *ma = BKE_material_add(bmain, "CorrectionCopyMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.3f));
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CorrectionCopyMap"));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  /* A material copy owns its own correction and record array, and a clone of the editable map. */
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  MaterialPaintLayer *copy_row = paint_layers_first(*copy);
  ASSERT_NE(copy_row, nullptr);
  ASSERT_EQ(BLI_listbase_count(&copy_row->effects), 1);
  const MaterialPaintLayer *copy_effect = static_cast<MaterialPaintLayer *>(
      copy_row->effects.first);
  EXPECT_NE(copy_effect, effect);
  EXPECT_NE(copy_effect->channels, effect->channels);
  EXPECT_TRUE(BLI_uuid_equal(copy_effect->marker, effect->marker));
  EXPECT_EQ(copy_effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  const MaterialPaintLayerChannel *copy_record = paint_layer_channel_find(
      *copy_effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(copy_record, nullptr);
  ASSERT_NE(copy_record->image, nullptr);
  EXPECT_NE(copy_record->image, map);
  BKE_id_free(bmain, copy);

  /* Duplicating a row gives the copy corrections of its own, with maps of their own. */
  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, row);
  ASSERT_NE(duplicate, nullptr);
  ASSERT_EQ(BLI_listbase_count(&duplicate->effects), 1);
  const MaterialPaintLayer *duplicate_effect = static_cast<MaterialPaintLayer *>(
      duplicate->effects.first);
  EXPECT_NE(duplicate_effect, effect);
  EXPECT_FALSE(BLI_uuid_equal(duplicate_effect->marker, effect->marker));
  EXPECT_EQ(duplicate_effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_FLOAT_EQ(duplicate_effect->opacity, 0.3f);
  const MaterialPaintLayerChannel *duplicate_record = paint_layer_channel_find(
      *duplicate_effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(duplicate_record, nullptr);
  ASSERT_NE(duplicate_record->image, nullptr);
  EXPECT_NE(duplicate_record->image, map);
}

TEST_F(PaintLayersDescription, correction_foreach_id_walks_its_map)
{
  Material *ma = BKE_material_add(bmain, "CorrectionWalkMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CorrectionWalkMap"));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  Vector<ID *> visited;
  auto collector = [](LibraryIDLinkCallbackData *data) -> int {
    if (*data->id_pointer != nullptr) {
      static_cast<Vector<ID *> *>(data->user_data)->append(*data->id_pointer);
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(bmain, &ma->id, collector, &visited, IDWALK_READONLY);
  EXPECT_TRUE(visited.contains(&map->id));
}

TEST_F(PaintLayersDescription, correction_blend_round_trip_keeps_it_and_its_map)
{
  Material *ma = BKE_material_add(bmain, "CorrectionRoundTripMat");
  id_us_plus(&ma->id);
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Corr");
  ASSERT_NE(effect, nullptr);
  const bUUID effect_marker = effect->marker;
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CorrectionRoundTripMap"));
  id_us_plus(&map->id);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, effect, 0.5f));

  char filepath[FILE_MAX];
  BLI_path_join(
      filepath, sizeof(filepath), BKE_tempdir_session(), "paint_layers_correction_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);
  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "CorrectionRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_row = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_row, nullptr);
  ASSERT_EQ(BLI_listbase_count(&reloaded_row->effects), 1);
  const MaterialPaintLayer *reloaded_effect = static_cast<MaterialPaintLayer *>(
      reloaded_row->effects.first);
  EXPECT_TRUE(BLI_uuid_equal(reloaded_effect->marker, effect_marker));
  EXPECT_EQ(reloaded_effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_FLOAT_EQ(reloaded_effect->opacity, 0.5f);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      *reloaded_effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  /* The map is remapped on read, not left as a stale address. */
  ASSERT_NE(record->image, nullptr);
  EXPECT_STREQ(record->image->id.name + 2, "CorrectionRoundTripMap");

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersDescription, pack_maps_packs_a_generated_map_once_and_again_only_when_modified)
{
  Material *ma = BKE_material_add(bmain, "PackMapsMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *map = BKE_image_add_generated(
      bmain, 8, 8, "PackMapsMap", 24, false, IMA_GENTYPE_BLANK, clear, false, true, false);
  ASSERT_NE(map, nullptr);
  /* A bare row owns no record yet: the map needs one to land in. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  /* A generated map holds its pixels only in the buffer: it is packed even when never painted. */
  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 1);
  EXPECT_EQ(map->source, IMA_SRC_FILE);
  EXPECT_TRUE(BKE_image_has_packedfile(map));

  /* Packed and not modified since: the file already has it as it stands. */
  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 0);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ibuf->byte_data_for_write()[0] = 200;
  BKE_image_mark_dirty(map, ibuf);
  BKE_image_release_ibuf(map, ibuf, lock);

  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 1);
  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 0);
}

TEST_F(PaintLayersDescription, pack_maps_packs_a_modified_assigned_map_and_leaves_a_clean_one)
{
  Material *ma = BKE_material_add(bmain, "PackAssignedMat");
  MaterialPaintLayer *painted_row = BKE_paint_layers_add(*ma, "P", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *clean_row = BKE_paint_layers_add(*ma, "C", nullptr, PaintLayerPlace::Above);
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *painted = BKE_image_add_generated(
      bmain, 8, 8, "PackAssignedPainted", 24, false, IMA_GENTYPE_BLANK, clear, false, true, false);
  Image *clean = BKE_image_add_generated(
      bmain, 8, 8, "PackAssignedClean", 24, false, IMA_GENTYPE_BLANK, clear, false, true, false);
  ASSERT_NE(painted, nullptr);
  ASSERT_NE(clean, nullptr);
  /* A bare row owns no record yet: the map needs one to land in. */
  ASSERT_NE(
      BKE_paint_layers_channel_add(*ma, painted_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_NE(
      BKE_paint_layers_channel_add(*ma, clean_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, painted_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, painted));
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, clean_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, clean));

  /* Both now read as textures the user assigned from disk; painting modified one of them. The
   * names stand for paths only -- nothing here reads from disk. A FILE source with no file on
   * disk holds no buffer of its own: the modified map gets the painted pixels installed
   * directly. */
  STRNCPY(painted->filepath, "paint_layers_assigned_painted.png");
  STRNCPY(clean->filepath, "paint_layers_assigned_clean.png");
  painted->source = IMA_SRC_FILE;
  clean->source = IMA_SRC_FILE;
  ImBuf *ibuf = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
  ASSERT_NE(ibuf, nullptr);
  ibuf->byte_data_for_write()[0] = 200;
  BKE_image_mark_dirty(painted, ibuf);
  BKE_image_replace_imbuf(painted, ibuf);
  /* The image took its own reference; this one is done. */
  IMB_freeImBuf(ibuf);

  /* The assigned maps are project data either way, so the close prompt never offers them; only a
   * modified one needs its pixels embedded. */
  Set<const Image *> internal_images;
  BKE_paint_layers_internal_images_collect(*bmain, internal_images);
  EXPECT_TRUE(internal_images.contains(painted));
  EXPECT_TRUE(internal_images.contains(clean));
  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 1);
  /* The pack embeds the buffer and keeps both images file sources: the project carries the pixels,
   * and no user file was touched. */
  EXPECT_EQ(painted->source, IMA_SRC_FILE);
  EXPECT_EQ(clean->source, IMA_SRC_FILE);
  EXPECT_TRUE(BKE_image_has_packedfile(painted));
  EXPECT_FALSE(BKE_image_has_packedfile(clean));

  /* Packed and not modified since: the file already has it as it stands. */
  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 0);
}

TEST_F(PaintLayersDescription, bake_cache_maps_collect_keeps_caches_out_of_the_packs)
{
  Material *ma = BKE_material_add(bmain, "BakeCollectMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *row = paint_layer_add(*ma, "Baked");
  Image *cache = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "BakeCacheImg"));
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "BakeChannelImg"));
  paint_layer_channel_add(*row, *map);
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*row);
  bake->images[0] = cache;

  Vector<Image *> caches;
  BKE_paint_layers_bake_cache_maps_collect(*bmain, caches);
  ASSERT_EQ(caches.size(), 1);
  EXPECT_TRUE(caches.contains(cache));

  Vector<Image *> maps;
  BKE_paint_layers_maps_collect(*bmain, maps);
  EXPECT_TRUE(maps.contains(map));
  EXPECT_FALSE(maps.contains(cache));

  /* The planner re-bakes a loaded row (the read clears its hash), so a cache is never packed. The
   * plain generated map is, like any generated map nothing has touched (a file would reopen it
   * empty). */
  EXPECT_EQ(BKE_paint_layers_pack_maps(*bmain, false, nullptr), 1);
  EXPECT_FALSE(BKE_image_has_packedfile(cache));
}

TEST_F(PaintLayersDescription, internal_images_collect_leaves_a_linked_map_to_the_save_prompt)
{
  Library *lib = BKE_id_new<Library>(bmain, "InternalImagesLib");
  Material *ma = BKE_material_add(bmain, "InternalImagesMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *owned_row = paint_layer_add(*ma, "Owned");
  Image *owned_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "InternalOwnedMap"));
  paint_layer_channel_add(*owned_row, *owned_map);
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*owned_row);
  Image *cache = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "InternalBakeCache"));
  bake->images[0] = cache;

  MaterialPaintLayer *linked_row = paint_layer_add(*ma, "Linked");
  Image *linked_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "InternalLinkedMap"));
  paint_layer_channel_add(*linked_row, *linked_map);
  /* A copy a library holds keeps its pixels outside the project: not editable, so neither the pack
   * nor the internal set claims it, and the save prompt treats it like any other image. */
  linked_map->id.lib = lib;
  linked_map->id.tag |= ID_TAG_EXTERN;
  ASSERT_FALSE(ID_IS_EDITABLE(&linked_map->id));

  Set<const Image *> internal_images;
  BKE_paint_layers_internal_images_collect(*bmain, internal_images);
  EXPECT_TRUE(internal_images.contains(owned_map));
  EXPECT_TRUE(internal_images.contains(cache));
  EXPECT_FALSE(internal_images.contains(linked_map));

  /* An image no layer holds a map in stays out, editable as it is. */
  Image *loose = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "InternalLooseMap"));
  EXPECT_FALSE(internal_images.contains(loose));
}

TEST_F(PaintLayersDescription, rna_effects_expose_the_correction_and_add_one)
{
  Material *ma = BKE_material_add(bmain, "RnaCorrectionMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, "L");
  ASSERT_NE(layer, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);

  PropertyRNA *effects = RNA_struct_find_property(&layer_ptr, "effects");
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&layer_ptr, effects), 0);

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, layer, "Corr");
  ASSERT_NE(effect, nullptr);
  ASSERT_EQ(RNA_property_collection_length(&layer_ptr, effects), 1);

  CollectionPropertyIterator iter;
  RNA_property_collection_begin(&layer_ptr, effects, &iter);
  ASSERT_TRUE(iter.valid);
  EXPECT_EQ(iter.ptr.data, effect);
  RNA_property_collection_end(&iter);
}

TEST_F(PaintLayersDescription, correction_default_names_are_numbered_within_their_layer)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionNameMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Correction;

  params.anchor = a;
  MaterialPaintLayer *a_first = BKE_paint_layers_add_with_policy(ma, params);
  MaterialPaintLayer *a_second = BKE_paint_layers_add_with_policy(ma, params);
  params.anchor = b;
  MaterialPaintLayer *b_first = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(a_first, nullptr);
  ASSERT_NE(a_second, nullptr);
  ASSERT_NE(b_first, nullptr);
  EXPECT_STREQ(a_first->name, "Correction Paint 1");
  EXPECT_STREQ(a_second->name, "Correction Paint 2");
  /* The count belongs to the layer: the other one starts over. */
  EXPECT_STREQ(b_first->name, "Correction Paint 1");
}

TEST_F(PaintLayersDescription, correction_moves_within_its_layer_and_joins_another)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionMoveMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *first = BKE_paint_layers_correction_add(ma, a, "First");
  MaterialPaintLayer *second = BKE_paint_layers_correction_add(ma, a, "Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  /* Above the later one puts it last: the list is bottom to top, as the layers' is. */
  ASSERT_TRUE(BKE_paint_layers_edit_move(ma, *first, second, PaintLayerPlace::Above));
  EXPECT_EQ(a->effects.first, second);
  EXPECT_EQ(a->effects.last, first);

  /* Dropped on another layer, it joins that layer's corrections and leaves this one's. */
  ASSERT_TRUE(BKE_paint_layers_edit_move(ma, *first, b, PaintLayerPlace::Above));
  EXPECT_EQ(BLI_listbase_count(&a->effects), 1);
  EXPECT_EQ(b->effects.first, first);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *first), b);

  /* It is never a row of the stack, so a move that names no layer to hold it is refused. */
  EXPECT_FALSE(BKE_paint_layers_edit_move(ma, *first, nullptr, PaintLayerPlace::Above));
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 2);
}

TEST_F(PaintLayersDescription, duplicate_copies_the_painted_pixels_of_a_generated_map)
{
  Material &ma = *BKE_material_add(bmain, "PixelCopyMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(ma, "L", nullptr, PaintLayerPlace::Above);
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *map = BKE_image_add_generated(
      bmain, 4, 4, "PixelCopyMap", 32, false, IMA_GENTYPE_BLANK, clear, false, false, false);
  ASSERT_NE(map, nullptr);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  uchar *pixels = ibuf->byte_data_for_write();
  ASSERT_NE(pixels, nullptr);
  pixels[0] = 10;
  pixels[1] = 120;
  pixels[2] = 200;
  pixels[3] = 255;
  BKE_image_mark_dirty(map, ibuf);
  BKE_image_release_ibuf(map, ibuf, lock);
  /* A bare row owns no record yet: the map needs one to land in. */
  ASSERT_NE(BKE_paint_layers_channel_add(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, ma, layer);
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(*copy,
                                                                     PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  ASSERT_NE(record->image, nullptr);
  EXPECT_NE(record->image, map);

  void *copy_lock = nullptr;
  ImBuf *copied = BKE_image_acquire_ibuf(record->image, nullptr, &copy_lock);
  ASSERT_NE(copied, nullptr);
  const uchar *copied_pixels = copied->byte_data();
  ASSERT_NE(copied_pixels, nullptr);
  EXPECT_EQ(copied_pixels[0], 10);
  EXPECT_EQ(copied_pixels[1], 120);
  EXPECT_EQ(copied_pixels[2], 200);
  EXPECT_EQ(copied_pixels[3], 255);
  BKE_image_release_ibuf(record->image, copied, copy_lock);
}

TEST_F(PaintLayersDescription, mask_copy_replaces_the_target_mask_with_items_of_its_own)
{
  Material &ma = *BKE_material_add(bmain, "MaskCopyMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *source_mask = BKE_paint_layers_mask_add(ma, a, 0.25f);
  ASSERT_NE(source_mask, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, b, 1.0f), nullptr);

  ASSERT_TRUE(BKE_paint_layers_mask_copy(*bmain, ma, *a, *b));
  ASSERT_EQ(BLI_listbase_count(&b->mask_stack), 1);
  const MaterialPaintLayer *copied = BKE_paint_layers_mask_base(*b);
  ASSERT_NE(copied, nullptr);
  EXPECT_NE(copied, source_mask);
  EXPECT_FALSE(BLI_uuid_equal(copied->marker, source_mask->marker));
  /* The source keeps its own mask, and a row is never its own source. */
  EXPECT_EQ(BLI_listbase_count(&a->mask_stack), 1);
  EXPECT_FALSE(BKE_paint_layers_mask_copy(*bmain, ma, *a, *a));
  /* A row with no mask has nothing to give. */
  MaterialPaintLayer *c = BKE_paint_layers_add(ma, "C", nullptr, PaintLayerPlace::Above);
  EXPECT_FALSE(BKE_paint_layers_mask_copy(*bmain, ma, *c, *a));
}

TEST_F(PaintLayersDescription, mask_correction_is_an_item_over_the_base_and_not_a_row)
{
  Material &ma = *BKE_material_add(bmain, "MaskCorrectionMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  /* Over the base, so there has to be one. */
  EXPECT_EQ(BKE_paint_layers_mask_item_add(ma, row, MA_PAINT_LAYER_SOURCE_IMAGE, "P"), nullptr);
  MaterialPaintLayer *base = BKE_paint_layers_mask_add(ma, row, 0.5f);
  ASSERT_NE(base, nullptr);

  MaterialPaintLayer *paint = BKE_paint_layers_mask_item_add(
      ma, row, MA_PAINT_LAYER_SOURCE_IMAGE, "P");
  MaterialPaintLayer *fill = BKE_paint_layers_mask_item_add(
      ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", 0.25f);
  ASSERT_NE(paint, nullptr);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_item_add(ma, row, MA_PAINT_LAYER_SOURCE_STACK, "S"), nullptr);

  /* The base stays first and the base; the corrections follow in the order they were added. */
  EXPECT_EQ(row->mask_stack.first, base);
  EXPECT_EQ(base->next, paint);
  EXPECT_EQ(paint->next, fill);
  EXPECT_EQ(BKE_paint_layers_mask_base(*row), base);
  EXPECT_EQ(paint->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(paint->blend, MA_PAINT_LAYER_BLEND_MIX);
  EXPECT_EQ(paint->flag & MA_PAINT_LAYER_MASK_BASE, 0);
  ASSERT_EQ(paint->channels_num, 1);
  EXPECT_EQ(paint->channels[0].value[3], 0.0f);
  EXPECT_EQ(fill->channels[0].value[0], 0.25f);
  EXPECT_EQ(fill->channels[0].value[3], 1.0f);

  /* Never a row of the stack, found by its marker, owned by its row. */
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 1);
  EXPECT_EQ(BKE_paint_layers_find(ma, fill->marker), fill);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(ma, *fill), row);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(ma, *row), nullptr);
  EXPECT_TRUE(BKE_paint_layers_subtree_contains(*row, fill->marker));

  /* Paint lays nothing until it has a map; Fill does, and so does the base. */
  EXPECT_TRUE(paint_layer_mask_item_lays(*base));
  EXPECT_FALSE(paint_layer_mask_item_lays(*paint));
  EXPECT_TRUE(paint_layer_mask_item_lays(*fill));
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "MaskMap", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image));
  EXPECT_TRUE(paint_layer_mask_item_lays(*paint));
}

TEST_F(PaintLayersDescription, mask_correction_is_worked_on_through_its_row)
{
  Material &ma = *BKE_material_add(bmain, "MaskCorrectionActiveMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "F");
  ASSERT_NE(item, nullptr);

  BKE_paint_layers_active_set(ma, item->marker);
  /* Every caller that wants "the layer" gets the row; the correction is its own question. */
  EXPECT_EQ(BKE_paint_layers_active_layer_get(ma), row);
  EXPECT_EQ(BKE_paint_layers_active_mask_item_get(ma), item);
  BKE_paint_layers_active_set(ma, row->marker);
  EXPECT_EQ(BKE_paint_layers_active_mask_item_get(ma), nullptr);

  /* Removing it keeps the row the active one, and the base stays. */
  BKE_paint_layers_active_set(ma, item->marker);
  ASSERT_TRUE(BKE_paint_layers_mask_item_remove(ma, item));
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(ma), row->marker));
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 1);
  EXPECT_FALSE(BKE_paint_layers_mask_item_remove(ma, BKE_paint_layers_mask_base(*row)));
  EXPECT_FALSE(BKE_paint_layers_mask_item_remove(ma, row));
}

TEST_F(PaintLayersDescription, mask_correction_add_policy_makes_the_base_and_numbers_apart)
{
  Material &ma = *BKE_material_add(bmain, "MaskCorrectionPolicyMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);

  const float color[4] = {0.7f, 0.7f, 0.7f, 1.0f};
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MaskCorrectionFill;
  params.anchor = row;
  params.fill_color = color;
  MaterialPaintLayer *first = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(first, nullptr);
  /* A row with no mask got a white one first, so the correction has something to lie over. */
  const MaterialPaintLayer *base = BKE_paint_layers_mask_base(*row);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(BKE_paint_layers_mask_constant(*base), 1.0f);
  EXPECT_STREQ(first->name, "Mask Fill 1");
  EXPECT_EQ(first->channels[0].value[0], 0.7f);

  /* Aimed at a correction, of either kind, an Add lands on its row. */
  params.anchor = first;
  params.kind = PaintLayerAddKind::MaskCorrection;
  params.fill_color = nullptr;
  MaterialPaintLayer *second = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(second->name, "Mask Paint 1");
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(ma, *second), row);
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 3);
}

TEST_F(PaintLayersDescription, mask_add_image_makes_a_white_mask_and_a_fill_correction_that_reads_it)
{
  Material &ma = *BKE_material_add(bmain, "MaskImageMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "MaskImage.png", 32, false, IMA_GENTYPE_BLANK, black, false, true, false);
  const int users_before = image.id.us;

  MaterialPaintLayer *correction = BKE_paint_layers_mask_add_image(ma, *row, image);
  ASSERT_NE(correction, nullptr);

  /* Add White Mask first: the base is the layer's own mask slot, white until a correction says
   * otherwise, and it carries no map of its own. */
  const MaterialPaintLayer *base = BKE_paint_layers_mask_base(*row);
  ASSERT_NE(base, nullptr);
  EXPECT_FLOAT_EQ(BKE_paint_layers_mask_constant(*base), 1.0f);
  ASSERT_EQ(base->channels_num, 1);
  EXPECT_EQ(base->channels[0].image, nullptr);

  /* The correction reads the image, and is named after it without the file extension. */
  EXPECT_NE(correction, base);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ((correction->flag & MA_PAINT_LAYER_MASK_BASE), 0);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_STREQ(correction->name, "MaskImage");
  ASSERT_EQ(correction->channels_num, 1);
  EXPECT_EQ(correction->channels[0].image, &image);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(ma, *correction), row);
  /* The record took its own user, as any channel's map does. */
  EXPECT_EQ(image.id.us, users_before + 1);

  /* One mask per row: a second add leaves the first as it was. */
  EXPECT_EQ(BKE_paint_layers_mask_add_image(ma, *row, image), nullptr);
  EXPECT_EQ(BLI_listbase_count(&row->mask_stack), 2);
  EXPECT_EQ(image.id.us, users_before + 1);
}

TEST_F(PaintLayersDescription, mask_correction_is_copied_reordered_and_carried_with_the_mask)
{
  Material &ma = *BKE_material_add(bmain, "MaskCorrectionEditMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(ma, "B", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_mask_add(ma, a, 1.0f), nullptr);
  MaterialPaintLayer *one = BKE_paint_layers_mask_item_add(
      ma, a, MA_PAINT_LAYER_SOURCE_CONSTANT, "One");
  MaterialPaintLayer *two = BKE_paint_layers_mask_item_add(
      ma, a, MA_PAINT_LAYER_SOURCE_CONSTANT, "Two");
  ASSERT_NE(one, nullptr);
  ASSERT_NE(two, nullptr);

  /* A copy lands beside it, in the same mask, with a marker of its own. */
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, ma, one);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(one->next, copy);
  EXPECT_EQ(BKE_paint_layers_mask_item_owner(ma, *copy), a);
  EXPECT_FALSE(BLI_uuid_equal(copy->marker, one->marker));
  EXPECT_EQ(copy->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(BKE_paint_layers_duplicate(*bmain, ma, BKE_paint_layers_mask_base(*a)), nullptr);
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 2);

  /* It reorders inside its mask, below the base never, and does not leave it. */
  ASSERT_TRUE(BKE_paint_layers_edit_move(ma, *two, one, PaintLayerPlace::Below));
  EXPECT_EQ(BKE_paint_layers_mask_base(*a)->next, two);
  EXPECT_EQ(two->next, one);
  ASSERT_TRUE(BKE_paint_layers_edit_move(
      ma, *two, BKE_paint_layers_mask_base(*a), PaintLayerPlace::Below));
  EXPECT_EQ(BKE_paint_layers_mask_base(*a)->next, two);
  EXPECT_FALSE(BKE_paint_layers_edit_move(ma, *two, b, PaintLayerPlace::Above));

  /* The mask goes with its corrections, and copies with them. */
  ASSERT_TRUE(BKE_paint_layers_mask_copy(*bmain, ma, *a, *b));
  EXPECT_EQ(BLI_listbase_count(&b->mask_stack), 4);
  ASSERT_TRUE(BKE_paint_layers_mask_remove(ma, a));
  EXPECT_EQ(BLI_listbase_count(&a->mask_stack), 0);
}

TEST_F(PaintLayersDescription, correction_duplicate_lands_beside_it_with_maps_of_its_own)
{
  Material &ma = *BKE_material_add(bmain, "CorrectionDupMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, a, "Corr");
  ASSERT_NE(effect, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(ma, effect, 0.4f));

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, ma, effect);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(copy->opacity, 0.4f);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *copy), a);
  EXPECT_EQ(BLI_listbase_count(&a->effects), 2);
  EXPECT_EQ(a->effects.last, copy);
  EXPECT_FALSE(BLI_uuid_equal(copy->marker, effect->marker));
  /* A correction is not a row of the stack, so the stack is as it was. */
  EXPECT_EQ(BLI_listbase_count(&ma.paint_layers), 1);
}

TEST_F(PaintLayersDescription, fill_correction_is_one_opaque_base_color_record)
{
  Material &ma = *BKE_material_add(bmain, "FillCorrectionMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      ma, row, nullptr, MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_STREQ(effect->name, "Correction Fill");

  /* A flat colour is one value: Base Color alone, opaque black until it is given another. */
  ASSERT_EQ(effect->channels_num, 1);
  const MaterialPaintLayerChannel &base = effect->channels[0];
  EXPECT_EQ(base.channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(base.state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  EXPECT_EQ(base.image, nullptr);
  EXPECT_EQ(base.value[0], 0.0f);
  EXPECT_EQ(base.value[3], 1.0f);

  /* It lays in the channel it has a record for, and the row it hangs on takes part through it. */
  EXPECT_TRUE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_TRUE(paint_layer_effects_lay(ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(paint_layer_row_empty_base(ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  /* An Image or a Constant source and nothing else. */
  EXPECT_EQ(BKE_paint_layers_correction_add(ma, row, "Folder", MA_PAINT_LAYER_SOURCE_STACK), nullptr);
}

TEST_F(PaintLayersDescription, fill_correction_colour_is_written_through_set_fill_color)
{
  Material &ma = *BKE_material_add(bmain, "FillCorrectionColourMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);

  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(ma, effect, red));
  float read[4];
  BKE_paint_layers_base_color_get(*effect, read);
  EXPECT_EQ(read[0], 1.0f);
  EXPECT_EQ(read[1], 0.0f);
  /* Never a row of the stack, so the record is the only place the colour is. */
  EXPECT_EQ(effect->channels_num, 1);
  EXPECT_EQ(effect->channels[0].value[0], 1.0f);
}

TEST_F(PaintLayersDescription, fill_correction_channel_starts_at_the_colour_and_normal_lays_nothing)
{
  Material &ma = *BKE_material_add(bmain, "FillCorrectionChannelMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);
  const float color[4] = {0.2f, 0.5f, 0.8f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(ma, effect, color));

  /* Switching a channel on does not change the view until its own value is edited. */
  const MaterialPaintLayerChannel *rough = BKE_paint_layers_channel_add(
      ma, effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(rough->value[0], 0.2f);
  EXPECT_EQ(rough->value[1], 0.5f);
  EXPECT_TRUE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  /* A flat value is not a tangent normal: the record is kept at the flat tangent and lays nothing. */
  const MaterialPaintLayerChannel *normal = BKE_paint_layers_channel_add(
      ma, effect, PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_NE(normal, nullptr);
  EXPECT_EQ(normal->value[0], 0.5f);
  EXPECT_EQ(normal->value[2], 1.0f);
  EXPECT_FALSE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_FALSE(paint_layer_effects_lay(ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));
}

TEST_F(PaintLayersDescription, fill_correction_lays_the_map_of_a_channel_that_holds_one)
{
  Material &ma = *BKE_material_add(bmain, "FillCorrectionMapMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      ma, row, "Fill", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(ma, effect, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  EXPECT_FALSE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL));

  /* A map in a channel is read like a Paint correction's, Normal included; the other channels keep
   * their flat colour, and the source is still a Fill's. */
  const float color[4] = {0.5f, 0.5f, 1.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "NormalMap", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, effect, PAINT_MATERIAL_CHANNEL_NORMAL, &image));
  EXPECT_TRUE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(paint_layer_effect_lays(ma, *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
}

TEST_F(PaintLayersDescription, fill_correction_add_policy_numbers_apart_from_paint_corrections)
{
  Material &ma = *BKE_material_add(bmain, "FillCorrectionPolicyMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "A", nullptr, PaintLayerPlace::Above);

  const float color[4] = {0.9f, 0.4f, 0.1f, 1.0f};
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::CorrectionFill;
  params.anchor = row;
  params.fill_color = color;
  MaterialPaintLayer *first = BKE_paint_layers_add_with_policy(ma, params);
  MaterialPaintLayer *second = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(first->name, "Correction Fill 1");
  EXPECT_STREQ(second->name, "Correction Fill 2");
  EXPECT_EQ(first->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(first->channels[0].value[0], 0.9f);
  EXPECT_EQ(first->channels[0].value[1], 0.4f);

  params.kind = PaintLayerAddKind::Correction;
  params.fill_color = nullptr;
  MaterialPaintLayer *paint = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(paint, nullptr);
  EXPECT_STREQ(paint->name, "Correction Paint 1");
  EXPECT_EQ(paint->source, MA_PAINT_LAYER_SOURCE_IMAGE);

  /* Aimed at a correction, an Add lands on the row it hangs on. */
  params.kind = PaintLayerAddKind::CorrectionFill;
  params.anchor = first;
  MaterialPaintLayer *third = BKE_paint_layers_add_with_policy(ma, params);
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_owner(ma, *third), row);
  EXPECT_EQ(BLI_listbase_count(&row->effects), 4);
}

/* -------------------------------------------------------------------- */
/** \name Row UV mapping
 * \{ */

/** A Fill row whose Base Color record carries a map, the one shape that supports a mapping. */
static MaterialPaintLayer *mapping_fill_with_map(Main &bmain, Material &ma, const char *name)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(ma, name, nullptr, PaintLayerPlace::Above);
  EXPECT_TRUE(BKE_paint_layers_set_source(ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT));
  Image *map = static_cast<Image *>(BKE_id_new(&bmain, ID_IM, name));
  /* A bare row owns no record yet: the map needs one to land in. */
  EXPECT_NE(BKE_paint_layers_channel_add(ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  EXPECT_TRUE(
      BKE_paint_layers_channel_set_image(ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  return fill;
}

TEST_F(PaintLayersDescription, mapping_is_carried_by_a_fill_before_it_has_a_map)
{
  Material &ma = *BKE_material_add(bmain, "MappingSupportMat");

  /* A Fill keeps its mapping from the start: the Mapping panel and the gizmo are there before the
   * first texture, and nothing is remapped until a map arrives. */
  MaterialPaintLayer *bare = BKE_paint_layers_add(ma, "Bare", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_source(ma, bare, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*bare)) << "a new row is made mapped";
  EXPECT_TRUE(BKE_paint_layers_mapping_available(ma, *bare));
  EXPECT_FALSE(BKE_paint_layers_mapping_supported(ma, *bare));
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(ma, *bare)) << "no map to move yet";

  /* Slotting a map makes the mapping that is already on apply. */
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  EXPECT_TRUE(BKE_paint_layers_fill_reads_map(ma, *fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(BKE_paint_layers_fill_reads_map(ma, *fill, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(BKE_paint_layers_mapping_supported(ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(ma, *fill));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, fill, false));
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(ma, *fill)) << "the switch still turns it off";
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, fill, true));

  /* A painted map is written by strokes at the raw UV: a Paint row never maps. */
  MaterialPaintLayer *paint = BKE_paint_layers_add(ma, "Paint", nullptr, PaintLayerPlace::Above);
  Image *paint_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "PaintMap"));
  ASSERT_NE(BKE_paint_layers_channel_add(ma, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, paint_map));
  EXPECT_FALSE(BKE_paint_layers_mapping_supported(ma, *paint));
  EXPECT_FALSE(BKE_paint_layers_mapping_available(ma, *paint));
  EXPECT_FALSE(BKE_paint_layers_mapping_set_enabled(ma, paint, true));

  /* A folder and a filter lay no map of their own and keep no mapping either. */
  PaintLayerAddParams folder_params;
  folder_params.kind = PaintLayerAddKind::Folder;
  MaterialPaintLayer *folder = BKE_paint_layers_add_with_policy(ma, folder_params);
  ASSERT_NE(folder, nullptr);
  EXPECT_FALSE(BKE_paint_layers_mapping_available(ma, *folder));
  MaterialPaintLayer *filter = BKE_paint_layers_filter_add(
      ma, fill, MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(filter, nullptr);
  EXPECT_FALSE(BKE_paint_layers_mapping_available(ma, *filter));

  /* Taking the map away stops the remap but keeps the setting: the Fill waits for the next map,
   * and the toggle can still be cleared. */
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, nullptr));
  EXPECT_FALSE(BKE_paint_layers_mapping_applies(ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_available(ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, fill, false));
}

TEST_F(PaintLayersDescription, mapping_is_supported_by_a_fill_correction_and_a_fill_mask)
{
  Material &ma = *BKE_material_add(bmain, "MappingItemsMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "Row", nullptr, PaintLayerPlace::Above);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      ma, row, "C", MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_NE(correction, nullptr);
  EXPECT_FALSE(BKE_paint_layers_mapping_supported(ma, *correction));
  ASSERT_NE(BKE_paint_layers_channel_add(ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  Image *map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CorrectionMap"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  EXPECT_TRUE(BKE_paint_layers_mapping_supported(ma, *correction));
  EXPECT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, correction, true));

  MaterialPaintLayer *mask_base = BKE_paint_layers_mask_add(ma, row, 1.0f);
  ASSERT_NE(mask_base, nullptr);
  /* The base mask is painted by the Mask brush and takes no map of its own: there is nothing for a
   * remap to move, even when a map is on it. */
  Image *base_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "BaseMaskMap"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, mask_base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, base_map));
  EXPECT_FALSE(BKE_paint_layers_mapping_supported(ma, *mask_base));
  EXPECT_FALSE(BKE_paint_layers_mapping_set_enabled(ma, mask_base, true));
  MaterialPaintLayer *mask_fill = BKE_paint_layers_mask_item_add(
      ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", 0.5f);
  ASSERT_NE(mask_fill, nullptr);
  EXPECT_FALSE(BKE_paint_layers_mapping_supported(ma, *mask_fill));
  Image *mask_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "MaskMap"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, mask_fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, mask_map));
  EXPECT_TRUE(BKE_paint_layers_mapping_supported(ma, *mask_fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, mask_fill, true));

  /* A painted mask is written by strokes at the raw UV. */
  MaterialPaintLayer *mask_paint = BKE_paint_layers_mask_item_add(
      ma, row, MA_PAINT_LAYER_SOURCE_IMAGE, "P");
  ASSERT_NE(mask_paint, nullptr);
  Image *painted = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "PaintedMask"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, mask_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, painted));
  EXPECT_FALSE(BKE_paint_layers_mapping_supported(ma, *mask_paint));
}

TEST_F(PaintLayersDescription, mapping_enable_is_structural_and_values_are_normalized)
{
  Material &ma = *BKE_material_add(bmain, "MappingValuesMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");

  /* A new row is mapped already: switching it off and on again is what changes the shape. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, fill, false));
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, fill, true));
  EXPECT_NE(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "the row gains a remap, which changes the shape of the stack";

  /* A zero axis reads back as one, a tiny one is lifted to the minimum and keeps its sign. */
  EXPECT_FLOAT_EQ(BKE_paint_layers_mapping_scale_normalize(0.0f), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_mapping_scale_normalize(1e-6f), PAINT_LAYER_MAPPING_SCALE_MIN);
  EXPECT_FLOAT_EQ(BKE_paint_layers_mapping_scale_normalize(-1e-6f),
                  -PAINT_LAYER_MAPPING_SCALE_MIN);
  EXPECT_FLOAT_EQ(BKE_paint_layers_mapping_scale_normalize(-2.0f), -2.0f);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_mapping_scale_normalize(std::numeric_limits<float>::quiet_NaN()), 1.0f);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_mapping_scale_normalize(std::numeric_limits<float>::infinity()), 1.0f);

  const float zero_scale[2] = {0.0f, -3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, false));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, zero_scale));
  EXPECT_FLOAT_EQ(fill->mapping.scale[0], 1.0f);
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], -3.0f);

  const float offset[2] = {0.25f, -0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, fill, offset));
  EXPECT_FLOAT_EQ(fill->mapping.offset[0], 0.25f);
  EXPECT_FLOAT_EQ(fill->mapping.offset[1], -0.5f);
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(ma, fill, 0.5f));
  EXPECT_FLOAT_EQ(fill->mapping.rotation, 0.5f);

  /* A row of another material is refused. */
  Material &other = *BKE_material_add(bmain, "MappingOtherMat");
  EXPECT_FALSE(BKE_paint_layers_mapping_set_offset(other, fill, offset));
  EXPECT_FALSE(BKE_paint_layers_mapping_set_enabled(other, fill, false));
}

TEST_F(PaintLayersDescription, mapping_scale_lock_moves_both_axes_together)
{
  Material &ma = *BKE_material_add(bmain, "MappingLockMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");

  /* A new row starts locked: the axes follow each other. */
  EXPECT_TRUE(BKE_paint_layers_mapping_scale_lock_get(*fill));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, false));

  const float apart[2] = {2.0f, 5.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, apart));
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], 5.0f);

  /* Locking joins the axes at X at once. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, true));
  EXPECT_TRUE(BKE_paint_layers_mapping_scale_lock_get(*fill));
  EXPECT_FLOAT_EQ(fill->mapping.scale[0], 2.0f);
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], 2.0f);

  /* The axis that moved from the stored value leads. */
  const float y_moved[2] = {2.0f, 4.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, y_moved));
  EXPECT_FLOAT_EQ(fill->mapping.scale[0], 4.0f);
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], 4.0f);
  const float x_moved[2] = {3.0f, 4.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, x_moved));
  EXPECT_FLOAT_EQ(fill->mapping.scale[0], 3.0f);
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], 3.0f);

  /* Unlocking keeps the values and lets the axes part again. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, false));
  EXPECT_FALSE(BKE_paint_layers_mapping_scale_lock_get(*fill));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, apart));
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], 5.0f);
}

TEST_F(PaintLayersDescription, mapping_block_copies_with_the_row)
{
  Material *ma = BKE_material_add(bmain, "MappingCopyMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, *ma, "Mapped");
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  const float offset[2] = {0.25f, 0.5f};
  const float scale[2] = {2.0f, 3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, fill, offset));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(*ma, fill, false));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, fill, 0.75f));

  auto expect_mapping = [](const MaterialPaintLayer &layer) {
    EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(layer));
    EXPECT_FLOAT_EQ(layer.mapping.offset[0], 0.25f);
    EXPECT_FLOAT_EQ(layer.mapping.offset[1], 0.5f);
    EXPECT_FLOAT_EQ(layer.mapping.scale[0], 2.0f);
    EXPECT_FLOAT_EQ(layer.mapping.scale[1], 3.0f);
    EXPECT_FLOAT_EQ(layer.mapping.rotation, 0.75f);
  };

  /* The block is stored by value, so a material copy and a row duplicate carry it. */
  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copy_layer = paint_layers_first(*copy);
  ASSERT_NE(copy_layer, nullptr);
  expect_mapping(*copy_layer);
  BKE_id_free(bmain, copy);

  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, fill);
  ASSERT_NE(duplicate, nullptr);
  expect_mapping(*duplicate);
}

TEST_F(PaintLayersDescription, a_scale_or_rotation_edit_keeps_the_tile_on_the_canvas)
{
  Material &ma = *BKE_material_add(bmain, "MappingTileMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");

  /* Where the middle of the map lands on the canvas: the inverse of the sample transform, the one
   * the gizmo draws its cage from. */
  const auto canvas_center = [](const MaterialPaintLayer &layer, float r_center[2]) {
    const float scale_x = BKE_paint_layers_mapping_scale_normalize(layer.mapping.scale[0]);
    const float scale_y = BKE_paint_layers_mapping_scale_normalize(layer.mapping.scale[1]);
    const float cos_rot = std::cos(layer.mapping.rotation);
    const float sin_rot = std::sin(layer.mapping.rotation);
    const float u = (0.5f - layer.mapping.offset[0]) / scale_x;
    const float v = (0.5f - layer.mapping.offset[1]) / scale_y;
    r_center[0] = cos_rot * u + sin_rot * v;
    r_center[1] = -sin_rot * u + cos_rot * v;
  };

  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, false));
  const float offset[2] = {0.2f, -0.1f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, fill, offset));
  float center[2];
  canvas_center(*fill, center);
  EXPECT_NEAR(center[0], 0.3f, 1e-5f);
  EXPECT_NEAR(center[1], 0.6f, 1e-5f);
  /* The public read answers the same, which is what the panel's fields ask through RNA. */
  float center_api[2];
  BKE_paint_layers_mapping_tile_center(*fill, center_api);
  EXPECT_NEAR(center_api[0], center[0], 1e-6f);
  EXPECT_NEAR(center_api[1], center[1], 1e-6f);

  /* The scale field edits the tile as the canvas shows it: it grows around its centre, so the
   * gizmo's rectangle does not jump while the offset reads a value that follows the scale. */
  const float scale[2] = {2.0f, 3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_holding_tile(ma, fill, scale));
  EXPECT_FLOAT_EQ(fill->mapping.scale[0], 2.0f);
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], 3.0f);
  float after_scale[2];
  canvas_center(*fill, after_scale);
  EXPECT_NEAR(after_scale[0], center[0], 1e-5f);
  EXPECT_NEAR(after_scale[1], center[1], 1e-5f);

  /* The same for the rotation, and for locking the axes (which joins them at the X value). */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation_holding_tile(ma, fill, 0.7f));
  float after_rotation[2];
  canvas_center(*fill, after_rotation);
  EXPECT_NEAR(after_rotation[0], center[0], 1e-5f);
  EXPECT_NEAR(after_rotation[1], center[1], 1e-5f);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock_holding_tile(ma, fill, true));
  EXPECT_FLOAT_EQ(fill->mapping.scale[1], fill->mapping.scale[0]);
  float after_lock[2];
  canvas_center(*fill, after_lock);
  EXPECT_NEAR(after_lock[0], center[0], 1e-5f);
  EXPECT_NEAR(after_lock[1], center[1], 1e-5f);

  /* The raw setter still writes what it is told: the gizmo computes its own offset for a drag. */
  const float offset_before[2] = {fill->mapping.offset[0], fill->mapping.offset[1]};
  const float other_scale[2] = {0.5f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, other_scale));
  EXPECT_FLOAT_EQ(fill->mapping.offset[0], offset_before[0]);
  EXPECT_FLOAT_EQ(fill->mapping.offset[1], offset_before[1]);
  float after_raw[2];
  canvas_center(*fill, after_raw);
  EXPECT_GT(std::fabs(after_raw[0] - center[0]) + std::fabs(after_raw[1] - center[1]), 1e-3f)
      << "a raw scale change is what moves the tile; the panel's fields do not do it";
}

TEST_F(PaintLayersDescription, tile_center_roundtrips_for_an_arbitrary_mapping)
{
  Material &ma = *BKE_material_add(bmain, "MappingRoundtripMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, false));
  const float offset[2] = {0.17f, -0.23f};
  const float scale[2] = {2.5f, 0.75f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, fill, offset));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(ma, fill, 1.1f));

  float center[2];
  BKE_paint_layers_mapping_tile_center(*fill, center);
  EXPECT_TRUE(std::isfinite(center[0]));
  EXPECT_TRUE(std::isfinite(center[1]));

  /* Re-applying the same scale and rotation asks the inverse to put the centre back exactly where
   * it already is: centre -> offset_for_tile_center -> centre. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_holding_tile(ma, fill, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation_holding_tile(ma, fill, 1.1f));
  float after[2];
  BKE_paint_layers_mapping_tile_center(*fill, after);
  EXPECT_NEAR(after[0], center[0], 1e-5f);
  EXPECT_NEAR(after[1], center[1], 1e-5f);

  /* A non-finite scale cannot poison the centre: normalize reads it as one. */
  fill->mapping.scale[0] = std::numeric_limits<float>::quiet_NaN();
  float finite_center[2];
  BKE_paint_layers_mapping_tile_center(*fill, finite_center);
  EXPECT_TRUE(std::isfinite(finite_center[0]));
  EXPECT_TRUE(std::isfinite(finite_center[1]));
}

TEST_F(PaintLayersDescription, mapping_remaps_only_a_transform_or_a_clip)
{
  Material &ma = *BKE_material_add(bmain, "MappingRemapsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(ma, *fill));
  EXPECT_FALSE(BKE_paint_layers_mapping_remaps(ma, *fill))
      << "a new row is enabled but still at the identity transform";

  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, fill, offset));
  EXPECT_TRUE(BKE_paint_layers_mapping_remaps(ma, *fill));

  const float zero[2] = {0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, fill, zero));
  EXPECT_FALSE(BKE_paint_layers_mapping_remaps(ma, *fill));

  /* An identity transform still clips: the unit-square test needs the mapping's output. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  EXPECT_TRUE(BKE_paint_layers_mapping_remaps(ma, *fill));
}

TEST_F(PaintLayersDescription, a_zeroed_mapping_block_reads_as_enabled)
{
  /* The enabled flag is stored inverted, so a block written before the flag existed -- a row the
   * fixture links in the way a file read leaves it -- reads as enabled, exactly like a row made
   * now. Old and new files answer the panel, the gizmo and the graph alike. */
  Material &ma = *BKE_material_add(bmain, "MappingLegacyMat");
  MaterialPaintLayer *legacy = paint_layer_add(ma, "Legacy");
  EXPECT_EQ(legacy->mapping.flag, 0);
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*legacy))
      << "a zeroed block reads as enabled, as a new row's does";

  /* The old "enabled" bit is reserved: a file that stored it must not read as disabled now. */
  legacy->mapping.flag = MA_PAINT_LAYER_MAPPING_ENABLED_DEPRECATED;
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*legacy));
  legacy->mapping.flag = 0;

  /* Switching it off stores the inverted bit, whatever the row's kind. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, legacy, false));
  EXPECT_FALSE(BKE_paint_layers_mapping_enabled_get(*legacy));
  EXPECT_TRUE((legacy->mapping.flag & MA_PAINT_LAYER_MAPPING_DISABLED) != 0);

  /* Switching it back on is refused on a row that may carry no mapping, and works on a Fill. */
  EXPECT_FALSE(BKE_paint_layers_mapping_set_enabled(ma, legacy, true));
  ASSERT_TRUE(BKE_paint_layers_set_source(ma, legacy, MA_PAINT_LAYER_SOURCE_CONSTANT));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, legacy, true));
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*legacy));
  EXPECT_EQ(legacy->mapping.flag & MA_PAINT_LAYER_MAPPING_DISABLED, 0);
}

TEST_F(PaintLayersDescription, the_raw_setters_touch_only_their_own_value)
{
  /* The raw setters are the data's own meaning: a scale, a rotation or a lock written through
   * them changes nothing else, so a script's assignment order cannot matter. Keeping the tile in
   * place is the UI's job (properties_material.py) and the reset's and the gizmo's, through the
   * *_holding_tile calls. */
  Material &ma = *BKE_material_add(bmain, "MappingRawMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  const float offset[2] = {0.3f, -0.2f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(ma, fill, offset));

  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, false));
  const float scale[2] = {2.0f, 3.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(ma, fill, scale));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(ma, fill, 0.9f));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale_lock(ma, fill, true));
  EXPECT_FLOAT_EQ(fill->mapping.offset[0], 0.3f);
  EXPECT_FLOAT_EQ(fill->mapping.offset[1], -0.2f);
}

TEST_F(PaintLayersDescription, mapping_wrap_defaults_to_repeat_and_refuses_unknown_modes)
{
  Material &ma = *BKE_material_add(bmain, "MappingWrapMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  EXPECT_EQ(BKE_paint_layers_mapping_wrap_get(*fill), MA_PAINT_LAYER_MAPPING_WRAP_REPEAT);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  EXPECT_EQ(BKE_paint_layers_mapping_wrap_get(*fill), MA_PAINT_LAYER_MAPPING_WRAP_CLIP);
  EXPECT_FALSE(BKE_paint_layers_mapping_set_wrap(ma, fill, 4));
  EXPECT_FALSE(BKE_paint_layers_mapping_set_wrap(ma, fill, -1));
  EXPECT_EQ(BKE_paint_layers_mapping_wrap_get(*fill), MA_PAINT_LAYER_MAPPING_WRAP_CLIP);

  /* A damaged stored value reads as Repeat instead of clipping a map. */
  fill->mapping.wrap = 77;
  EXPECT_EQ(BKE_paint_layers_mapping_wrap_get(*fill), MA_PAINT_LAYER_MAPPING_WRAP_REPEAT);

  /* A row of another material is refused. */
  Material &other = *BKE_material_add(bmain, "MappingWrapOther");
  EXPECT_FALSE(BKE_paint_layers_mapping_set_wrap(other, fill, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
}

TEST_F(PaintLayersDescription, mapping_wrap_is_structural_only_when_it_changes)
{
  Material &ma = *BKE_material_add(bmain, "MappingWrapShapeMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");

  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT));
  EXPECT_EQ(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0) << "the same mode changes nothing";
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X));
  EXPECT_NE(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "a clip adds the factor's nodes, which is topology";
}

TEST_F(PaintLayersDescription, mapping_wrap_axes_name_the_axes_that_tile)
{
  bool repeat_u = false;
  bool repeat_v = false;
  BKE_paint_layers_mapping_wrap_axes(MA_PAINT_LAYER_MAPPING_WRAP_REPEAT, repeat_u, repeat_v);
  EXPECT_TRUE(repeat_u);
  EXPECT_TRUE(repeat_v);
  BKE_paint_layers_mapping_wrap_axes(MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_X, repeat_u, repeat_v);
  EXPECT_TRUE(repeat_u);
  EXPECT_FALSE(repeat_v);
  BKE_paint_layers_mapping_wrap_axes(MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y, repeat_u, repeat_v);
  EXPECT_FALSE(repeat_u);
  EXPECT_TRUE(repeat_v);
  BKE_paint_layers_mapping_wrap_axes(MA_PAINT_LAYER_MAPPING_WRAP_CLIP, repeat_u, repeat_v);
  EXPECT_FALSE(repeat_u);
  EXPECT_FALSE(repeat_v);
}

TEST_F(PaintLayersDescription, mapping_wrap_copies_with_the_row)
{
  Material *ma = BKE_material_add(bmain, "MappingWrapCopyMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, *ma, "Mapped");
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, fill, MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copy_layer = paint_layers_first(*copy);
  ASSERT_NE(copy_layer, nullptr);
  EXPECT_EQ(BKE_paint_layers_mapping_wrap_get(*copy_layer), MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y);
  BKE_id_free(bmain, copy);

  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, fill);
  ASSERT_NE(duplicate, nullptr);
  EXPECT_EQ(BKE_paint_layers_mapping_wrap_get(*duplicate), MA_PAINT_LAYER_MAPPING_WRAP_REPEAT_Y);
}

TEST_F(PaintLayersDescription, a_mirror_fold_remaps_an_identity_repeat_mapping)
{
  Material &ma = *BKE_material_add(bmain, "MappingMirrorRemapsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  EXPECT_FALSE(BKE_paint_layers_mapping_remaps(ma, *fill));

  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(ma, fill, MA_PAINT_LAYER_MAPPING_MIRROR_U));
  EXPECT_TRUE(BKE_paint_layers_mapping_is_identity(*fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_remaps(ma, *fill))
      << "the fold moves the UVs on its own, so the identity transform still builds a chain";
  EXPECT_TRUE(BKE_paint_layers_mapping_mirror_applies(ma, *fill));

  /* A switched-off mapping folds nothing, whatever axes are stored. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, fill, false));
  EXPECT_FALSE(BKE_paint_layers_mapping_remaps(ma, *fill));
  EXPECT_FALSE(BKE_paint_layers_mapping_mirror_applies(ma, *fill));
}

TEST_F(PaintLayersDescription, a_mirror_is_refused_on_a_row_that_supports_no_mapping)
{
  Material &ma = *BKE_material_add(bmain, "MappingMirrorRefusedMat");
  MaterialPaintLayer *bare = BKE_paint_layers_add(ma, "Bare", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_source(ma, bare, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_FALSE(BKE_paint_layers_mapping_set_mirror(ma, bare, MA_PAINT_LAYER_MAPPING_MIRROR_U))
      << "no map to fold yet";
  EXPECT_EQ(BKE_paint_layers_mapping_mirror_get(*bare), 0);
  EXPECT_TRUE(BKE_paint_layers_mapping_set_mirror(ma, bare, 0)) << "clearing is always allowed";
}

TEST_F(PaintLayersDescription, a_mirror_axis_leaves_the_mapping_enabled_and_the_other_axis_alone)
{
  Material &ma = *BKE_material_add(bmain, "MappingMirrorFlagsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Mapped");
  float center[2];
  BKE_paint_layers_mapping_mirror_center_get(*fill, center);
  EXPECT_EQ(center[0], 0.5f);
  EXPECT_EQ(center[1], 0.5f);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(ma, fill, MA_PAINT_LAYER_MAPPING_MIRROR_V));
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*fill));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(
      ma, fill, MA_PAINT_LAYER_MAPPING_MIRROR_U | MA_PAINT_LAYER_MAPPING_MIRROR_V));
  EXPECT_TRUE(BKE_paint_layers_mapping_enabled_get(*fill)) << "U must not share the Disabled bit";
  EXPECT_TRUE(BKE_paint_layers_mapping_applies(ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_scale_lock_get(*fill));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(ma, fill, MA_PAINT_LAYER_MAPPING_MIRROR_U));
  EXPECT_EQ(BKE_paint_layers_mapping_mirror_get(*fill), MA_PAINT_LAYER_MAPPING_MIRROR_U);

  /* A center placed at the origin survives the axes being switched. */
  const float origin[2] = {0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror_center(ma, fill, origin));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(ma, fill, 0));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_mirror(ma, fill, MA_PAINT_LAYER_MAPPING_MIRROR_V));
  BKE_paint_layers_mapping_mirror_center_get(*fill, center);
  EXPECT_EQ(center[0], 0.0f);
  EXPECT_EQ(center[1], 0.0f);
}

TEST_F(PaintLayersDescription, mesh_map_row_adds_base_color_and_reads_the_atlas)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::MeshMap;
  MaterialPaintLayer *row = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MESH_MAP);
  EXPECT_EQ(row->mesh_map_type, MA_MESH_MAP_AO);
  /* A mesh-map row defaults to Base Color alone. */
  ASSERT_EQ(row->channels_num, 1);
  EXPECT_NE(paint_layer_channel_find(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  ASSERT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, row, MA_MESH_MAP_CURVATURE));
  EXPECT_EQ(row->mesh_map_type, MA_MESH_MAP_CURVATURE);
  EXPECT_EQ(BKE_paint_layers_mesh_map_type_get(*row), MA_MESH_MAP_CURVATURE);
  EXPECT_FALSE(BKE_paint_layers_mesh_map_type_set(*ma, row, MA_MESH_MAP_TYPE_NUM));

  /* Only a MESH_MAP row can name a geometry map. */
  MaterialPaintLayer *paint = paint_layer_add(*ma, "Paint");
  EXPECT_FALSE(BKE_paint_layers_mesh_map_type_set(*ma, paint, MA_MESH_MAP_AO));

  /* A duplicate and a copied row carry the type: the copy reads the owner material's slot for the
   * same type, which is what an import between materials relies on too. */
  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, row);
  ASSERT_NE(duplicate, nullptr);
  EXPECT_EQ(duplicate->source, MA_PAINT_LAYER_SOURCE_MESH_MAP);
  EXPECT_EQ(duplicate->mesh_map_type, MA_MESH_MAP_CURVATURE);
  EXPECT_EQ(BKE_paint_layers_mesh_map_type_get(*duplicate), MA_MESH_MAP_CURVATURE);

  /* The atlas resolver is structural: no slot, no contribution; a slot with an Image, the atlas. */
  EXPECT_EQ(paint_layer_mesh_map_image(*ma, *row), nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *atlas = BKE_image_add_generated(
      bmain, 8, 8, "Atlas", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_CURVATURE, atlas));
  EXPECT_EQ(paint_layer_mesh_map_image(*ma, *row), atlas);
}

TEST_F(PaintLayersDescription, row_lock_is_a_flag_and_refuses_foreign_rows)
{
  Material *ma = BKE_material_add(bmain, "LockMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  EXPECT_FALSE(BKE_paint_layers_is_locked(*layer));
  EXPECT_TRUE(BKE_paint_layers_set_locked(*ma, layer, true));
  EXPECT_TRUE(BKE_paint_layers_is_locked(*layer));
  EXPECT_TRUE(BKE_paint_layers_set_locked(*ma, layer, false));
  EXPECT_FALSE(BKE_paint_layers_is_locked(*layer));

  Material *other = BKE_material_add(bmain, "LockOther");
  EXPECT_FALSE(BKE_paint_layers_set_locked(*other, layer, true));
  EXPECT_FALSE(BKE_paint_layers_is_locked(*layer));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RNA Anchor Point
 * \{ */

static PointerRNA rna_anchor_layer_ptr(Material &ma, MaterialPaintLayer &layer)
{
  return RNA_pointer_create_discrete(&ma.id, RNA_MaterialPaintLayer, &layer);
}

TEST_F(PaintLayersDescription, rna_valid_points_lists_only_earlier_points)
{
  Material *ma = BKE_material_add(bmain, "RnaAnchorValid");
  MaterialPaintLayer *lower = BKE_paint_layers_add(*ma, "Lower", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *lower_point = BKE_paint_layers_anchor_point_add(
      *ma, lower, PaintLayerAnchorPointList::Effects, nullptr, false, "Lower Point", {});
  ASSERT_NE(lower_point, nullptr);
  MaterialPaintLayer *upper = BKE_paint_layers_add(*ma, "Upper", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = BKE_paint_layers_anchor_point_add(
      *ma, upper, PaintLayerAnchorPointList::Effects, nullptr, true, "Reader", {});
  MaterialPaintLayer *upper_point = BKE_paint_layers_anchor_point_add(
      *ma, upper, PaintLayerAnchorPointList::Effects, nullptr, false, "Upper Point", {});
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(upper_point, nullptr);

  PointerRNA reader_ptr = rna_anchor_layer_ptr(*ma, *reader);
  PropertyRNA *target = RNA_struct_find_property(&reader_ptr, "anchor_point_target");
  ASSERT_NE(target, nullptr);
  const EnumPropertyItem *items = nullptr;
  int totitem = 0;
  bool free = false;
  RNA_property_enum_items(nullptr, &reader_ptr, target, &items, &totitem, &free);
  const int real_items = (totitem > 0 && items[totitem - 1].identifier == nullptr) ? totitem - 1 :
                                                                                   totitem;
  ASSERT_EQ(real_items, 2) << "None plus the one point before the reader";
  EXPECT_STREQ(items[0].identifier, "NONE");
  EXPECT_STREQ(items[1].identifier, "ANCHOR_1");
  EXPECT_STREQ(items[1].name, "Lower Point (Lower, Content)");
  for (int i = 0; i < real_items; i++) {
    ASSERT_NE(items[i].name, nullptr);
    EXPECT_EQ(std::string(items[i].name).find("Upper Point"), std::string::npos)
        << "a point above the reader is never offered";
  }
  if (free) {
    MEM_delete_void(static_cast<void *>(const_cast<EnumPropertyItem *>(items)));
  }
}

TEST_F(PaintLayersDescription, rna_source_set_rejects_non_marker)
{
  Material *ma = BKE_material_add(bmain, "RnaAnchorSource");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = BKE_paint_layers_anchor_point_add(
      *ma, row, PaintLayerAnchorPointList::Effects, nullptr, true, "Reader", {});
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(*ma, row, "Correction");
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(correction, nullptr);

  PointerRNA reader_ptr = rna_anchor_layer_ptr(*ma, *reader);
  PropertyRNA *source = RNA_struct_find_property(&reader_ptr, "anchor_point_source");
  ASSERT_NE(source, nullptr);
  char marker[UUID_STRING_SIZE];
  BLI_uuid_format(marker, correction->marker);
  RNA_property_string_set(&reader_ptr, source, marker);
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_anchor_point_source_get(*reader)))
      << "a correction is not a marker and the real setter refuses it";

  /* The positive control: a real point's UUID is written through the same property. */
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(
      *ma, row, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
  ASSERT_NE(point, nullptr);
  BLI_uuid_format(marker, point->marker);
  RNA_property_string_set(&reader_ptr, source, marker);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_anchor_point_source_get(*reader), point->marker));
}

TEST_F(PaintLayersDescription, rna_enum_items_outlive_callback)
{
  Material *ma = BKE_material_add(bmain, "RnaAnchorEnum");
  MaterialPaintLayer *lower = BKE_paint_layers_add(*ma, "Lower", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(
      *ma, lower, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
  MaterialPaintLayer *upper = BKE_paint_layers_add(*ma, "Upper", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = BKE_paint_layers_anchor_point_add(
      *ma, upper, PaintLayerAnchorPointList::Effects, nullptr, true, "Reader", {});
  ASSERT_NE(point, nullptr);
  ASSERT_NE(reader, nullptr);

  PointerRNA reader_ptr = rna_anchor_layer_ptr(*ma, *reader);
  PropertyRNA *target = RNA_struct_find_property(&reader_ptr, "anchor_point_target");
  const EnumPropertyItem *first = nullptr;
  int first_count = 0;
  bool first_free = false;
  RNA_property_enum_items(nullptr, &reader_ptr, target, &first, &first_count, &first_free);
  ASSERT_GE(first_count, 2);
  ASSERT_NE(first[1].name, nullptr);
  ASSERT_NE(first[1].identifier, nullptr);
  /* A copy that outlives the callback's array: the second call must produce the same strings. */
  const std::string first_name(first[1].name);
  const std::string first_identifier(first[1].identifier);
  EXPECT_EQ(first_identifier, "ANCHOR_1");
  EXPECT_EQ(first_name, "Point (Lower, Content)");
  if (first_free) {
    MEM_delete_void(static_cast<void *>(const_cast<EnumPropertyItem *>(first)));
  }

  const EnumPropertyItem *second = nullptr;
  int second_count = 0;
  bool second_free = false;
  RNA_property_enum_items(nullptr, &reader_ptr, target, &second, &second_count, &second_free);
  ASSERT_GE(second_count, 2);
  EXPECT_EQ(std::string(second[1].name), first_name)
      << "the item strings live in a persistent pool, not in the callback's return value";
  EXPECT_EQ(std::string(second[1].identifier), first_identifier);
  if (second_free) {
    MEM_delete_void(static_cast<void *>(const_cast<EnumPropertyItem *>(second)));
  }
}

TEST_F(PaintLayersDescription, reader_count_matches_resolve)
{
  Material *ma = BKE_material_add(bmain, "RnaAnchorReaders");
  MaterialPaintLayer *source = BKE_paint_layers_add(*ma, "Source", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(
      *ma, source, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
  MaterialPaintLayer *first_row = BKE_paint_layers_add(
      *ma, "First", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *second_row = BKE_paint_layers_add(
      *ma, "Second", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_anchor_point_add(*ma,
                                              first_row,
                                              PaintLayerAnchorPointList::Effects,
                                              nullptr,
                                              true,
                                              "A",
                                              point->marker),
            nullptr);
  ASSERT_NE(BKE_paint_layers_anchor_point_add(*ma,
                                              second_row,
                                              PaintLayerAnchorPointList::Effects,
                                              nullptr,
                                              true,
                                              "B",
                                              point->marker),
            nullptr);

  PointerRNA point_ptr = rna_anchor_layer_ptr(*ma, *point);
  PropertyRNA *count = RNA_struct_find_property(&point_ptr, "anchor_point_reader_count");
  ASSERT_NE(count, nullptr);
  const int expected = paint_layer_anchor_point_readers(*ma, point->marker).size();
  EXPECT_EQ(RNA_property_int_get(&point_ptr, count), expected);
  EXPECT_EQ(expected, 2);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Height model and Normal & Height response
 * \{ */

TEST_F(PaintLayersDescription, height_channel_settings_default_to_neutral)
{
  Material *ma = BKE_material_add(bmain, "HeightDefaultMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");

  /* A row written before the height fields (and every fresh one) reads as: inherit the channel's
   * ordinary blend, scale one. */
  EXPECT_EQ(BKE_paint_layers_channel_height_blend(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT),
            MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_height_scale(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT), 1.0f);

  /* The two settings are the Height channel's alone. */
  EXPECT_FALSE(BKE_paint_layers_channel_height_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_HEIGHT_BLEND_ADD));
  EXPECT_FALSE(BKE_paint_layers_channel_height_scale_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 2.0f));

  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  EXPECT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_ADD));
  /* The height modes are different nodes of the generated chain, like the colour blends are types
   * of the Mix node: a structural edit. */
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(BKE_paint_layers_channel_height_blend(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT),
            MA_PAINT_LAYER_HEIGHT_BLEND_ADD);
  EXPECT_FALSE(BKE_paint_layers_channel_height_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE + 1));

  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  EXPECT_TRUE(BKE_paint_layers_channel_height_scale_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, 3.0f));
  /* The scale is an input of the chain, like the row's opacity: a value edit, no rebuild. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_height_scale(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT), 3.0f);
  /* An out-of-range scale clamps to the stored range rather than being refused. */
  EXPECT_TRUE(BKE_paint_layers_channel_height_scale_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, 9.0f));
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_height_scale(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT), 4.0f);
  EXPECT_TRUE(BKE_paint_layers_channel_height_scale_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, -1.0f));
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_height_scale(*layer, PAINT_MATERIAL_CHANNEL_HEIGHT), 0.0f);

  /* A folder lays a Height blend of its own through its own row, so it stops inlining: even the
   * height Mix is another operation than the colour Mix the inline lays. */
  MaterialPaintLayer *folder = paint_layer_add(*ma, "Folder");
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  EXPECT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder));
  EXPECT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_MIX));
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*folder));
}

/** The one rule every Height reader uses: an explicit mode wins, otherwise the row's ordinary
 * blend maps Mix to Inherit (the same step, GPU preview kept), Add and Subtract to themselves, and
 * everything else to Mix. */
TEST_F(PaintLayersDescription, height_effective_blend_follows_the_row)
{
  Material *ma = BKE_material_add(bmain, "HeightEffectiveMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");

  for (const int mode : {MA_PAINT_LAYER_HEIGHT_BLEND_MIX,
                         MA_PAINT_LAYER_HEIGHT_BLEND_ADD,
                         MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT})
  {
    SCOPED_TRACE(::testing::Message() << "explicit mode=" << mode);
    ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
        *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, mode));
    EXPECT_EQ(BKE_paint_layers_channel_height_blend_effective(*layer,
                                                              PAINT_MATERIAL_CHANNEL_HEIGHT),
              mode);
  }

  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT));
  struct Case {
    int row_blend;
    int effective;
  };
  const Case cases[] = {
      {MA_PAINT_LAYER_BLEND_MIX, MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT},
      {MA_PAINT_LAYER_BLEND_ADD, MA_PAINT_LAYER_HEIGHT_BLEND_ADD},
      {MA_PAINT_LAYER_BLEND_SUBTRACT, MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT},
      {MA_PAINT_LAYER_BLEND_MULTIPLY, MA_PAINT_LAYER_HEIGHT_BLEND_MIX},
      {MA_PAINT_LAYER_BLEND_OVERLAY, MA_PAINT_LAYER_HEIGHT_BLEND_MIX},
      {MA_PAINT_LAYER_BLEND_SCREEN, MA_PAINT_LAYER_HEIGHT_BLEND_MIX},
  };
  for (const Case &test_case : cases) {
    SCOPED_TRACE(::testing::Message() << "row blend=" << test_case.row_blend);
    ASSERT_TRUE(BKE_paint_layers_set_blend(
        *ma, layer, eMaterialPaintLayerBlend(test_case.row_blend)));
    EXPECT_EQ(BKE_paint_layers_channel_height_blend_effective(*layer,
                                                              PAINT_MATERIAL_CHANNEL_HEIGHT),
              test_case.effective);
  }

  /* An explicit mode is the record's own choice and does not follow the row's blend. */
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_MULTIPLY));
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT));
  EXPECT_EQ(BKE_paint_layers_channel_height_blend_effective(*layer,
                                                            PAINT_MATERIAL_CHANNEL_HEIGHT),
            MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT);

  /* No other channel has an effective Height mode. */
  EXPECT_EQ(BKE_paint_layers_channel_height_blend_effective(*layer,
                                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT);
}

TEST_F(PaintLayersDescription, response_values_default_to_one_and_clamp)
{
  Material *ma = BKE_material_add(bmain, "ResponseDefaultMat");
  /* A new material already owns a tree; the edits below must leave it exactly as it is. */
  const bNodeTree *tree_before = ma->nodetree;
  /* A material written before the response fields carries zeroes, which read as the node's own
   * defaults: one. */
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*ma), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_distance(*ma), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_normal_strength(*ma), 1.0f);

  /* The response belongs to a layered material; a plain one refuses the edit. */
  EXPECT_FALSE(BKE_paint_layers_bump_strength_set(*ma, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_bump_distance_set(*ma, 2.0f));
  EXPECT_FALSE(BKE_paint_layers_normal_strength_set(*ma, 0.5f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*ma), 1.0f);

  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_TRUE(BKE_paint_layers_bump_strength_set(*ma, 0.25f));
  EXPECT_TRUE(BKE_paint_layers_bump_distance_set(*ma, 4.0f));
  EXPECT_TRUE(BKE_paint_layers_normal_strength_set(*ma, 2.0f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*ma), 0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_distance(*ma), 4.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_normal_strength(*ma), 2.0f);
  /* No node was created and nothing was regenerated: the edit waits for the owned node. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_EQ(ma->nodetree, tree_before);

  /* Out-of-range values clamp to the stored range rather than being refused. */
  EXPECT_TRUE(BKE_paint_layers_bump_strength_set(*ma, 500.0f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*ma), 100.0f);
  EXPECT_TRUE(BKE_paint_layers_bump_distance_set(*ma, -5.0f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_distance(*ma), 0.0f);
}

TEST_F(PaintLayersDescription, new_material_response_defaults_are_visible)
{
  Material *fresh = BKE_material_add(bmain, "FreshResponseMat");
  BKE_paint_layers_new_material_response_apply(*fresh);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*fresh), 5.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_distance(*fresh), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_normal_strength(*fresh), 1.0f);

  /* A material written before the fields existed keeps zeroes, which still read as one. */
  Material *legacy = BKE_material_add(bmain, "LegacyResponseMat");
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*legacy), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_distance(*legacy), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_normal_strength(*legacy), 1.0f);
}

TEST_F(PaintLayersDescription, height_settings_and_response_round_trip)
{
  Material *ma = BKE_material_add(bmain, "HeightRoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);

  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");
  const bUUID layer_marker = layer->marker;
  ASSERT_TRUE(BKE_paint_layers_channel_height_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT));
  ASSERT_TRUE(BKE_paint_layers_channel_height_scale_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_HEIGHT, 2.5f));
  ASSERT_TRUE(BKE_paint_layers_bump_strength_set(*ma, 0.25f));
  ASSERT_TRUE(BKE_paint_layers_bump_distance_set(*ma, 4.0f));
  ASSERT_TRUE(BKE_paint_layers_normal_strength_set(*ma, 2.0f));

  char filepath[FILE_MAX];
  BLI_path_join(filepath,
                sizeof(filepath),
                BKE_tempdir_session(),
                "paint_layers_height_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "HeightRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_layer = BKE_paint_layers_find(*reloaded, layer_marker);
  ASSERT_NE(reloaded_layer, nullptr);
  EXPECT_EQ(BKE_paint_layers_channel_height_blend(*reloaded_layer, PAINT_MATERIAL_CHANNEL_HEIGHT),
            MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT);
  EXPECT_FLOAT_EQ(
      BKE_paint_layers_channel_height_scale(*reloaded_layer, PAINT_MATERIAL_CHANNEL_HEIGHT), 2.5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_strength(*reloaded), 0.25f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_bump_distance(*reloaded), 4.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_normal_strength(*reloaded), 2.0f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersDescription, duplicate_and_paste_keep_channel_settings)
{
  Material &ma = *BKE_material_add(bmain, "HeightCopyMat");
  ma.paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *target = BKE_paint_layers_add(ma, "Target", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(ma, row, "Corr");
  ASSERT_NE(row, nullptr);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(effect, nullptr);

  row->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_blend =
      MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE;
  row->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_scale_delta = 0.5f;
  row->channel_settings[PAINT_MATERIAL_CHANNEL_ROUGHNESS].opacity = 0.3f;
  effect->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_blend =
      MA_PAINT_LAYER_HEIGHT_BLEND_ADD;
  effect->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_scale_delta = -0.75f;

  /* A duplicated row keeps its per (row, channel) overrides, the way the copy of a material does. */
  MaterialPaintLayer *row_copy = BKE_paint_layers_duplicate(*bmain, ma, row);
  ASSERT_NE(row_copy, nullptr);
  EXPECT_EQ(row_copy->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_blend,
            MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE);
  EXPECT_FLOAT_EQ(
      row_copy->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_scale_delta, 0.5f);
  EXPECT_FLOAT_EQ(row_copy->channel_settings[PAINT_MATERIAL_CHANNEL_ROUGHNESS].opacity, 0.3f);

  /* A pasted correction keeps its own: its blend and scale travel with it onto the other row. */
  MaterialPaintLayer *effect_copy = BKE_paint_layers_correction_paste(*bmain, ma, *effect, target);
  ASSERT_NE(effect_copy, nullptr);
  EXPECT_EQ(effect_copy->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_blend,
            MA_PAINT_LAYER_HEIGHT_BLEND_ADD);
  EXPECT_FLOAT_EQ(
      effect_copy->channel_settings[PAINT_MATERIAL_CHANNEL_HEIGHT].height_scale_delta, -0.75f);
}

TEST_F(PaintLayersDescription, height_write_skip_takes_the_row_out_of_height_alone)
{
  Material *ma = BKE_material_add(bmain, "HeightSkipMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "HeightSkipImage"));
  MaterialPaintLayer *layer = paint_layer_add(*ma, "Layer");
  paint_layer_channel_add(*layer, *image);
  /* A Height record of the row's own: a flat value, so the row lays height without a map. */
  MaterialPaintLayerChannel *height_record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_NE(height_record, nullptr);
  height_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  height_record->value[0] = 0.8f;
  height_record->value[3] = 1.0f;

  /* By default the row writes its height. */
  EXPECT_TRUE(BKE_paint_layers_channel_height_write(*layer));
  EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_HEIGHT, layers_));
  EXPECT_EQ(layers_.size(), 1);

  /* Skipping keeps the record and the Base Color channel, and takes only the height out. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  EXPECT_TRUE(BKE_paint_layers_channel_height_write_set(*ma, *layer, false));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_FALSE(BKE_paint_layers_channel_height_write(*layer));
  ASSERT_EQ(layer->channels_num, 2);
  EXPECT_EQ(layer->channels[1].channel, PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_EQ(layer->channels[1].state, MA_PAINT_LAYER_CHANNEL_ENABLED);

  layers_.clear();
  EXPECT_FALSE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_HEIGHT, layers_));
  layers_.clear();
  EXPECT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers_));
  ASSERT_EQ(layers_.size(), 1);

  /* Writing again puts the row back. */
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);
  EXPECT_TRUE(BKE_paint_layers_channel_height_write_set(*ma, *layer, true));
  EXPECT_TRUE(BKE_paint_layers_channel_height_write(*layer));
  layers_.clear();
  EXPECT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_HEIGHT, layers_));
  EXPECT_EQ(layers_.size(), 1);

  /* The other channels' records stay untouched by the toggle, and the bake hash sees the flag. */
  MaterialPaintLayer *other = paint_layer_add(*ma, "Other");
  paint_layer_channel_add(*other, *image);
  EXPECT_TRUE(BKE_paint_layers_channel_height_write_set(*ma, *other, false));
  EXPECT_TRUE(BKE_paint_layers_channel_height_write(*layer));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Map encoding (what a record's map stores)
 * \{ */

/** A Fill row of \a ma carrying \a map under its \a channel record. */
static MaterialPaintLayer &map_encoding_fill_row(Material &ma,
                                                 Image &map,
                                                 const eMaterialPaintChannel channel)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(ma, "Fill", nullptr, PaintLayerPlace::Above);
  BLI_assert(fill != nullptr);
  BKE_paint_layers_set_source(ma, fill, MA_PAINT_LAYER_SOURCE_CONSTANT);
  BKE_paint_layers_channel_add(ma, fill, channel);
  BKE_paint_layers_channel_set_image(ma, fill, channel, &map);
  return *fill;
}

TEST_F(PaintLayersDescription, map_encoding_setter_takes_the_pairs_the_channel_supports)
{
  Material &ma = *BKE_material_add(bmain, "MapEncodingSet");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  Image &map = *BKE_image_add_generated(
      bmain, 4, 4, "RoughMap", 32, false, IMA_GENTYPE_BLANK, grey, false, true, false);
  MaterialPaintLayer &fill = map_encoding_fill_row(ma, map, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  /* A glossiness is a roughness upside down; an F0 colour is not. */
  EXPECT_TRUE(BKE_paint_layers_channel_map_encoding_allowed(PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                            MA_PAINT_LAYER_MAP_ENCODING_CHANNEL));
  EXPECT_TRUE(BKE_paint_layers_channel_map_encoding_allowed(
      PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));
  EXPECT_TRUE(BKE_paint_layers_channel_map_encoding_allowed(
      PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA));
  EXPECT_FALSE(BKE_paint_layers_channel_map_encoding_allowed(
      PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_F0));
  EXPECT_TRUE(BKE_paint_layers_channel_map_encoding_allowed(PAINT_MATERIAL_CHANNEL_SPECULAR,
                                                            MA_PAINT_LAYER_MAP_ENCODING_F0));
  EXPECT_FALSE(BKE_paint_layers_channel_map_encoding_allowed(
      PAINT_MATERIAL_CHANNEL_SPECULAR, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));
  EXPECT_FALSE(BKE_paint_layers_channel_map_encoding_allowed(
      PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));

  EXPECT_TRUE(BKE_paint_layers_channel_set_map_encoding(
      ma, &fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));
  EXPECT_EQ(BKE_paint_layers_channel_map_encoding_get(fill.channels[0]),
            MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS);
  EXPECT_FALSE(BKE_paint_layers_channel_set_map_encoding(
      ma, &fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_F0));
  EXPECT_EQ(BKE_paint_layers_channel_map_encoding_get(fill.channels[0]),
            MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS);
  /* A record that is not of the stack refuses. */
  MaterialPaintLayer stray;
  EXPECT_FALSE(BKE_paint_layers_channel_set_map_encoding(
      ma, &stray, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));

  /* An out-of-range value reads as the channel's own encoding, never as a conversion. */
  MaterialPaintLayerChannel &record = fill.channels[0];
  record.map_encoding = 99;
  EXPECT_EQ(BKE_paint_layers_channel_map_encoding_get(record),
            MA_PAINT_LAYER_MAP_ENCODING_CHANNEL);
}

TEST_F(PaintLayersDescription, map_encoding_survives_a_duplicate)
{
  Material &ma = *BKE_material_add(bmain, "MapEncodingCopy");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  Image &map = *BKE_image_add_generated(
      bmain, 4, 4, "GlossMap", 32, false, IMA_GENTYPE_BLANK, grey, false, true, false);
  MaterialPaintLayer &fill = map_encoding_fill_row(ma, map, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_channel_set_map_encoding(
      ma, &fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA));

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(bmain, ma, &fill);
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayerChannel *rough = paint_layer_channel_find(
      *copy, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(rough->map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA));
  EXPECT_EQ(BKE_paint_layers_channel_map_encoding_get(*rough),
            MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA);
}

TEST_F(PaintLayersDescription, map_encoding_moves_the_composite_hash_and_the_description)
{
  Material &ma = *BKE_material_add(bmain, "MapEncodingHash");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  Image &map = *BKE_image_add_generated(
      bmain, 4, 4, "RoughHashMap", 32, false, IMA_GENTYPE_BLANK, grey, false, true, false);
  MaterialPaintLayer &fill = map_encoding_fill_row(ma, map, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_EQ(layers[0].map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_CHANNEL));
  EXPECT_TRUE(layers[0].color_alpha_coverage);
  const uint64_t hash_channel = BKE_paint_material_composite_stack_hash(layers);

  ASSERT_TRUE(BKE_paint_layers_channel_set_map_encoding(
      ma, &fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));
  layers.clear();
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  EXPECT_EQ(layers[0].map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), hash_channel);

  /* The spec-gloss packing also takes the map's alpha out of the coverage. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_map_encoding(ma,
                                                        &fill,
                                                        PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                                        MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA));
  layers.clear();
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  EXPECT_FALSE(layers[0].color_alpha_coverage);
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), hash_channel);
}

TEST_F(PaintLayersDescription, normal_directx_moves_the_composite_hash)
{
  /* The green flip is part of what the composite shows, so the cache hash has to see the switch
   * (the regression of the DirectX flag missing from the hash). */
  Material &ma = *BKE_material_add(bmain, "NormalDirectXHash");
  const float flat_normal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
  Image &map = *BKE_image_add_generated(bmain,
                                        4,
                                        4,
                                        "NormalHashMap",
                                        32,
                                        false,
                                        IMA_GENTYPE_BLANK,
                                        flat_normal,
                                        false,
                                        true,
                                        false);
  MaterialPaintLayer &fill = map_encoding_fill_row(ma, map, PAINT_MATERIAL_CHANNEL_NORMAL);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(ma, PAINT_MATERIAL_CHANNEL_NORMAL, layers));
  ASSERT_EQ(layers.size(), 1);
  EXPECT_FALSE(layers[0].normal_directx);
  const uint64_t hash_opengl = BKE_paint_material_composite_stack_hash(layers);

  ASSERT_TRUE(BKE_paint_layers_normal_set_directx(ma, &fill, true));
  layers.clear();
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(ma, PAINT_MATERIAL_CHANNEL_NORMAL, layers));
  EXPECT_TRUE(layers[0].normal_directx);
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), hash_opengl);
}

TEST_F(PaintLayersDescription, spec_gloss_set_reads_the_map_twice_and_bakes_nothing)
{
  Material &ma = *BKE_material_add(bmain, "SpecGlossSet");
  /* A spec-gloss map: a mid specular colour and an alpha that varies, which is the glossiness the
   * roughness reads. */
  const float mid[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  Image &spec_map = *BKE_image_add_generated(bmain,
                                             2,
                                             2,
                                             "SpecGlossMap",
                                             32,
                                             false,
                                             IMA_GENTYPE_BLANK,
                                             mid,
                                             false,
                                             true,
                                             false);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&spec_map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int i : IndexRange(4)) {
      pixels[i * 4 + 0] = 10;
      pixels[i * 4 + 1] = 10;
      pixels[i * 4 + 2] = 10;
      pixels[i * 4 + 3] = uchar(51 + i * 20);
    }
    BKE_image_release_ibuf(&spec_map, ibuf, lock);
  }
  MaterialPaintLayer &fill = map_encoding_fill_row(
      ma, spec_map, PAINT_MATERIAL_CHANNEL_SPECULAR);
  const int images_before = int(BLI_listbase_count(&bmain->images));

  const char *reason = nullptr;
  ASSERT_TRUE(BKE_paint_layers_specular_map_convert(*bmain,
                                                    ma,
                                                    fill,
                                                    spec_map,
                                                    PaintSpecularMapKind::SpecGlossSet,
                                                    true,
                                                    nullptr,
                                                    nullptr,
                                                    &reason));
  EXPECT_EQ(reason, nullptr);
  /* No image was made: the same map now serves both records. */
  EXPECT_EQ(int(BLI_listbase_count(&bmain->images)), images_before);
  const MaterialPaintLayerChannel *spec = paint_layer_channel_find(
      fill, PAINT_MATERIAL_CHANNEL_SPECULAR);
  const MaterialPaintLayerChannel *rough = paint_layer_channel_find(
      fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(spec, nullptr);
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(spec->image, &spec_map);
  EXPECT_EQ(spec->map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_F0_GLOSSINESS_ALPHA));
  EXPECT_EQ(rough->image, &spec_map);
  EXPECT_EQ(rough->map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA));
  EXPECT_EQ(spec_map.id.us, 2);

  /* A constant alpha carries no glossiness: the roughness half refuses the whole set. */
  Image &flat_map = *BKE_image_add_generated(bmain,
                                             2,
                                             2,
                                             "SpecGlossFlat",
                                             32,
                                             false,
                                             IMA_GENTYPE_BLANK,
                                             mid,
                                             false,
                                             true,
                                             false);
  MaterialPaintLayer &flat_fill = map_encoding_fill_row(
      ma, flat_map, PAINT_MATERIAL_CHANNEL_SPECULAR);
  reason = nullptr;
  EXPECT_FALSE(BKE_paint_layers_specular_map_convert(*bmain,
                                                     ma,
                                                     flat_fill,
                                                     flat_map,
                                                     PaintSpecularMapKind::SpecGlossSet,
                                                     true,
                                                     nullptr,
                                                     nullptr,
                                                     &reason));
  ASSERT_NE(reason, nullptr);
  EXPECT_NE(std::strstr(reason, "alpha"), nullptr);
}

TEST_F(PaintLayersDescription, map_encoding_round_trips_a_smart_asset)
{
  /* The asset save copies rows structurally and the import copies them again; the encoding is
   * part of the record, so the glossiness read survives both -- the round trip the asset save
   * promises. */
  Material &source = *BKE_material_add(bmain, "AssetEncodingSource");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  Image &map = *BKE_image_add_generated(
      bmain, 4, 4, "AssetGlossMap", 32, false, IMA_GENTYPE_BLANK, grey, false, true, false);
  MaterialPaintLayer &fill = map_encoding_fill_row(source, map, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_TRUE(BKE_paint_layers_channel_set_map_encoding(
      source, &fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));

  const MaterialPaintLayer *rows[1] = {&fill};
  Material *asset = BKE_paint_layers_asset_create(*bmain,
                                                  source,
                                                  Span<const MaterialPaintLayer *>(rows, 1),
                                                  PaintLayerAssetType::Layer,
                                                  "EncodingAsset");
  ASSERT_NE(asset, nullptr);
  const MaterialPaintLayer *asset_row = static_cast<const MaterialPaintLayer *>(
      asset->paint_layers.first);
  ASSERT_NE(asset_row, nullptr);
  const MaterialPaintLayerChannel *asset_rough = paint_layer_channel_find(
      *asset_row, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(asset_rough, nullptr);
  EXPECT_EQ(asset_rough->map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));

  /* The import into another material reads the map the same way. */
  Material &target = *BKE_material_add(bmain, "AssetEncodingTarget");
  PaintLayerImportParams params;
  const MaterialPaintLayer *asset_rows[1] = {asset_row};
  ASSERT_TRUE(BKE_paint_layers_import_rows(
      bmain, target, *asset, Span<const MaterialPaintLayer *>(asset_rows, 1), params));
  MaterialPaintLayer *imported = static_cast<MaterialPaintLayer *>(target.paint_layers.first);
  ASSERT_NE(imported, nullptr);
  const MaterialPaintLayerChannel *imported_rough = paint_layer_channel_find(
      *imported, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(imported_rough, nullptr);
  EXPECT_EQ(imported_rough->map_encoding, int8_t(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS));
  EXPECT_NE(imported_rough->image, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Row projection
 * \{ */

TEST_F(PaintLayersDescription, projection_is_supported_by_a_fill_that_reads_a_map)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionSupportMat");

  MaterialPaintLayer *bare = BKE_paint_layers_add(ma, "Bare", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_source(ma, bare, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *bare));
  EXPECT_FALSE(BKE_paint_layers_projection_set_space(
      ma, bare, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  EXPECT_TRUE(BKE_paint_layers_projection_supported(ma, *fill));
  EXPECT_FALSE(BKE_paint_layers_projection_applies(ma, *fill))
      << "supported is not switched on";
  EXPECT_TRUE(fill->projection == nullptr) << "the payload is on demand";

  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR));
  EXPECT_EQ(fill->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR);
  EXPECT_NE(fill->projection, nullptr);
  EXPECT_TRUE(BKE_paint_layers_projection_applies(ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_EQ(BKE_paint_layers_projection_axis_get(*fill), MA_PAINT_LAYER_PROJECTION_AXIS_Z);

  /* A painted map is written by strokes at the raw UV: no projection either. */
  MaterialPaintLayer *paint = BKE_paint_layers_add(ma, "Paint", nullptr, PaintLayerPlace::Above);
  Image *paint_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ProjectedPaintMap"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, paint_map));
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *paint));
  EXPECT_FALSE(BKE_paint_layers_projection_set_space(
      ma, paint, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  EXPECT_EQ(paint->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_UV);
}

TEST_F(PaintLayersDescription, projection_frame_values_are_normalized_and_value_only)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionValuesMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_BOX));

  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  const float center[3] = {1.0f, 2.0f, 3.0f};
  const float size[3] = {2.0f, 0.0f, -4.0f};
  const float rotation[3] = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(ma, fill, center, size, rotation));
  EXPECT_EQ(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "a frame move never rebuilds the graph";
  float stored_euler[3];
  BKE_paint_layers_projection_frame_euler(
      fill->projection->normal, fill->projection->roll, stored_euler);
  for (int i = 0; i < 3; i++) {
    EXPECT_FLOAT_EQ(fill->projection->center[i], center[i]);
    EXPECT_FLOAT_EQ(stored_euler[i], rotation[i]);
  }
  EXPECT_FLOAT_EQ(fill->projection->size[0], 2.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[1], 1.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[2], -4.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_size_normalize(0.0f), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_size_normalize(1e-6f),
                  PAINT_LAYER_PROJECTION_SIZE_MIN);

  /* A zero or unknown frame cannot be written, and the clip toggle is a value too. */
  const float bad[3] = {0.0f, 0.0f, std::numeric_limits<float>::infinity()};
  EXPECT_FALSE(BKE_paint_layers_projection_set_center(ma, fill, bad));
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  ASSERT_TRUE(BKE_paint_layers_projection_set_clip(ma, fill, false));
  EXPECT_FALSE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_EQ(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "the clip toggle is a group value, not topology";
  ASSERT_TRUE(BKE_paint_layers_projection_set_axis(ma, fill, MA_PAINT_LAYER_PROJECTION_AXIS_X));
  EXPECT_NE(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0) << "the axis changes the UV math";
}

TEST_F(PaintLayersDescription, projection_frame_inside_matches_the_graph_rule)
{
  /* Why the CPU calls this: the generated graph builds the same two Multiply Add factors per axis
   * (see `projection_axis_factor`), one inside the frame and fading over
   * #PAINT_LAYER_PROJECTION_EDGE_SOFT, so a frame-space point on either side of the frame has to
   * answer the same on the CPU. */
  const float inside[3] = {0.0f, 0.99f, -0.99f};
  const float on_edge[3] = {1.0f, 0.0f, 0.0f};
  const float halfway_out[3] = {1.0f + 0.25f * PAINT_LAYER_PROJECTION_EDGE_SOFT, 0.0f, 0.0f};
  const float outside[3] = {1.01f, 0.0f, 0.0f};
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_frame_factor(inside), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_frame_factor(on_edge), 0.0f);
  EXPECT_NEAR(BKE_paint_layers_projection_frame_factor(halfway_out), 0.75f, 1e-5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_frame_factor(outside), 0.0f);
}

TEST_F(PaintLayersDescription, projection_facing_weight_matches_the_graph_rule)
{
  /* The graph runs the frame-space normal component through one clamped Multiply Add and the
   * t * t * (3 - 2 t) hermite (see `projection_facing_hermite_entry`); the CPU repeats both in
   * #BKE_paint_layers_projection_facing_weight. The scale comes from the one helper the value
   * sync and the bake share, so the sign it carries is the side the fade keeps. */
  const float angle = 80.0f;
  const float cos_min = cosf(DEG2RADF(angle));
  const float cos_max = cosf(DEG2RADF(angle - PAINT_LAYER_PROJECTION_ANGLE_BAND));

  /* Without Flip: the frame's +Z looks at the observer, so the faces whose normal aligns with
   * +Z (the positive component) are kept at full weight. */
  float scale = 0.0f;
  float offset = 0.0f;
  BKE_paint_layers_projection_facing_weights(angle, false, scale, offset);
  EXPECT_GT(scale, 0.0f) << "without flip, positive component is the kept side";
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, 1.0f), 1.0f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, cos_max), 1.0f, 1e-5f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, cos_min), 0.0f, 1e-5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, -1.0f), 0.0f);
  /* The band is symmetric: the fading rim's midpoint sits halfway up the hermite. */
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(
                  scale, offset, 0.5f * (cos_min + cos_max)),
              0.5f,
              1e-4f);

  /* Flip mirrors the weight onto the other side of the surface without moving the limit. */
  BKE_paint_layers_projection_facing_weights(angle, true, scale, offset);
  EXPECT_LT(scale, 0.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, -1.0f), 1.0f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, -cos_max), 1.0f, 1e-5f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, -cos_min), 0.0f, 1e-5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, 1.0f), 0.0f);
}

TEST_F(PaintLayersDescription, projection_angle_is_stored_away_from_the_file_marker)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionAngleMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* A zero limit would fade the whole projection out, so a stored limit never reaches it; a half
   * turn or more is the widest limit there is. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 0.0f));
  EXPECT_FLOAT_EQ(fill->projection->angle, 1.0f);
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, -30.0f));
  EXPECT_FLOAT_EQ(fill->projection->angle, 1.0f);
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 240.0f));
  EXPECT_FLOAT_EQ(fill->projection->angle, 180.0f);
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 55.0f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_angle_get(*fill), 55.0f);
}

TEST_F(PaintLayersDescription, projection_angle_zero_reads_back_as_the_default)
{
  /* A zero limit would fade the whole projection out; the normalized read maps it, a negative
   * value and anything past a half turn to the default. */
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(0.0f),
            PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(-4.0f),
            PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(250.0f),
            PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(65.0f), 65.0f);
}

TEST_F(PaintLayersDescription, projection_mode_table_covers_every_space)
{
  for (int space = 0; space < MA_PAINT_LAYER_MAPPING_SPACE_NUM; space++) {
    EXPECT_EQ(BKE_paint_layers_projection_mode_info(int16_t(space)).space, space);
  }
  EXPECT_FALSE(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_UV).uses_axis);
  EXPECT_TRUE(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_PLANAR).uses_axis);
  EXPECT_TRUE(
      BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER).uses_axis);
  /* Every mode names a map read count and a controller display; only the table decides who blends
   * faces and who reads an atlas. */
  int blend_modes = 0;
  int cube_modes = 0;
  for (int space = 0; space < MA_PAINT_LAYER_MAPPING_SPACE_NUM; space++) {
    const ProjectionModeInfo &info = BKE_paint_layers_projection_mode_info(int16_t(space));
    EXPECT_TRUE(info.samples_num == 1 || info.samples_num == 3) << "space " << space;
    EXPECT_NE(info.controller_display, 0) << "space " << space;
    blend_modes += info.uses_blend ? 1 : 0;
    cube_modes += info.uses_cube_layout ? 1 : 0;
  }
  EXPECT_EQ(blend_modes, 1);
  EXPECT_EQ(cube_modes, 1);
  EXPECT_TRUE(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR)
                  .uses_blend);
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_BOX).samples_num, 3);
  /* A stored value outside the table reads as UV, which has no frame. */
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_NUM).space,
            MA_PAINT_LAYER_MAPPING_SPACE_UV);
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(-1).space, MA_PAINT_LAYER_MAPPING_SPACE_UV);
}

TEST_F(PaintLayersDescription, projection_snap_and_controller_scale_are_off_and_value_only)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionOptionsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* Both options keep the decal behaviour of every earlier frame until the user asks for them. */
  EXPECT_FALSE(BKE_paint_layers_projection_snap_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_size_from_controller_get(*fill));
  ASSERT_TRUE(BKE_paint_layers_projection_set_snap(ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_projection_set_size_from_controller(ma, fill, true));
  EXPECT_TRUE(BKE_paint_layers_projection_snap_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_size_from_controller_get(*fill));

  const float center[3] = {0.0f, 0.0f, 0.0f};
  const float size[3] = {2.0f, 3.0f, 4.0f};
  const float rotation[3] = {0.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(ma, fill, center, size, rotation));

  /* The frame reads one on every axis while the controller carries the scale, but the inline
   * extents stay stored so turning the option off restores them. */
  float effective[3];
  BKE_paint_layers_projection_size_effective(*fill, effective);
  EXPECT_FLOAT_EQ(effective[0], 1.0f);
  EXPECT_FLOAT_EQ(effective[1], 1.0f);
  EXPECT_FLOAT_EQ(effective[2], 1.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[0], 2.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[1], 3.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[2], 4.0f);

  ASSERT_TRUE(BKE_paint_layers_projection_set_size_from_controller(ma, fill, false));
  BKE_paint_layers_projection_size_effective(*fill, effective);
  EXPECT_FLOAT_EQ(effective[0], 2.0f);
  EXPECT_FLOAT_EQ(effective[1], 3.0f);
  EXPECT_FLOAT_EQ(effective[2], 4.0f);
}

TEST_F(PaintLayersDescription, projection_aspect_factors_follow_the_image)
{
  /* Ucupaint's decal Scale: the footprint of the image's wider axis grows by the aspect, so a
   * non-square image is not stretched; a square image and an unknown one read one. */
  float factors[2] = {0.0f, 0.0f};
  BKE_paint_layers_projection_aspect_factors(nullptr, factors);
  EXPECT_FLOAT_EQ(factors[0], 1.0f);
  EXPECT_FLOAT_EQ(factors[1], 1.0f);

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *wide = BKE_image_add_generated(
      bmain, 2000, 1000, "AspectWide", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(wide, nullptr);
  BKE_paint_layers_projection_aspect_factors(wide, factors);
  EXPECT_FLOAT_EQ(factors[0], 0.5f);
  EXPECT_FLOAT_EQ(factors[1], 1.0f);

  Image *square = BKE_image_add_generated(
      bmain, 512, 512, "AspectSquare", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(square, nullptr);
  BKE_paint_layers_projection_aspect_factors(square, factors);
  EXPECT_FLOAT_EQ(factors[0], 1.0f);
  EXPECT_FLOAT_EQ(factors[1], 1.0f);

  /* A tall image stretches its Y footprint instead, on the same rule. */
  Image *tall = BKE_image_add_generated(
      bmain, 1000, 2000, "AspectTall", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(tall, nullptr);
  BKE_paint_layers_projection_aspect_factors(tall, factors);
  EXPECT_FLOAT_EQ(factors[0], 1.0f);
  EXPECT_FLOAT_EQ(factors[1], 0.5f);
}

TEST_F(PaintLayersDescription, projection_payload_copies_with_the_row_and_clears_with_uv)
{
  Material *ma = BKE_material_add(bmain, "ProjectionCopyMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, *ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER));
  const float center[3] = {0.5f, -1.0f, 2.0f};
  const float size[3] = {3.0f, 4.0f, 5.0f};
  const float rotation[3] = {0.25f, 0.5f, 0.75f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(
      *ma, fill, center, size, rotation));
  ASSERT_TRUE(BKE_paint_layers_projection_set_blend(*ma, fill, 0.5f));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copy_layer = paint_layers_first(*copy);
  ASSERT_NE(copy_layer, nullptr);
  ASSERT_NE(copy_layer->projection, nullptr);
  EXPECT_EQ(copy_layer->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER);
  EXPECT_FLOAT_EQ(copy_layer->projection->center[2], 2.0f);
  EXPECT_FLOAT_EQ(copy_layer->projection->size[1], 4.0f);
  EXPECT_FLOAT_EQ(copy_layer->projection->blend, 0.5f);
  BKE_id_free(bmain, copy);

  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, fill);
  ASSERT_NE(duplicate, nullptr);
  ASSERT_NE(duplicate->projection, nullptr);
  EXPECT_EQ(duplicate->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER);
  /* Freeing a row with a frame releases the payload with it. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, duplicate));
  EXPECT_EQ(paint_layers_first(*ma), fill);

  /* Returning to UV frees the frame; the row keeps its mapping values. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_UV));
  EXPECT_EQ(fill->projection, nullptr);
  EXPECT_FALSE(BKE_paint_layers_projection_applies(*ma, *fill));
}

TEST_F(PaintLayersDescription, projection_space_round_trip_resets_the_controller)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionRoundTripStateMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  Object *controller = BKE_object_add_only_object(bmain, OB_EMPTY, "ProjectionController");
  ASSERT_NE(controller, nullptr);

  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_NE(fill->projection, nullptr);
  ASSERT_TRUE(BKE_paint_layers_projection_set_object(ma, fill, controller));
  EXPECT_EQ(fill->projection_object, controller);

  /* UV frees the frame and lets the controller go with it. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_UV));
  EXPECT_EQ(fill->projection, nullptr);
  EXPECT_EQ(fill->projection_object, nullptr);

  /* Switching back allocates a fresh frame with defaults and no controller. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_NE(fill->projection, nullptr);
  EXPECT_EQ(fill->projection_object, nullptr);
  EXPECT_TRUE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_EQ(BKE_paint_layers_projection_axis_get(*fill), MA_PAINT_LAYER_PROJECTION_AXIS_Z);
}

TEST_F(PaintLayersDescription, projection_duplicate_owns_its_payload_and_takes_a_controller_user)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionDuplicateMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  Object *controller = BKE_object_add_only_object(bmain, OB_EMPTY, "ProjectionController");
  ASSERT_NE(controller, nullptr);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_set_object(ma, fill, controller));
  const int users = controller->id.us;

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, ma, fill);
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->projection, nullptr);
  EXPECT_NE(copy->projection, fill->projection);
  EXPECT_EQ(copy->projection_object, controller);
  EXPECT_EQ(controller->id.us, users + 1);
}

TEST_F(PaintLayersDescription, projection_round_trips_through_a_file)
{
  Material *ma = BKE_material_add(bmain, "ProjectionRoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ProjectionRoundTripImage"));
  id_us_plus(&image->id);

  MaterialPaintLayer *layer = paint_layer_add(*ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  paint_layer_channel_add(*layer, *image);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  const float center[3] = {1.0f, -2.0f, 0.5f};
  const float size[3] = {2.0f, 3.0f, 4.0f};
  const float rotation[3] = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(*ma, layer, center, size, rotation));
  ASSERT_TRUE(BKE_paint_layers_projection_set_blend(*ma, layer, 0.25f));
  ASSERT_TRUE(BKE_paint_layers_projection_set_clip(*ma, layer, false));

  char filepath[FILE_MAX];
  BLI_path_join(filepath,
                sizeof(filepath),
                BKE_tempdir_session(),
                "paint_layers_projection_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "ProjectionRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_layer = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_layer, nullptr);
  EXPECT_EQ(reloaded_layer->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_BOX);
  ASSERT_NE(reloaded_layer->projection, nullptr);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->center[1], -2.0f);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->size[2], 4.0f);
  float reloaded_euler[3];
  BKE_paint_layers_projection_frame_euler(
      reloaded_layer->projection->normal, reloaded_layer->projection->roll, reloaded_euler);
  EXPECT_NEAR(reloaded_euler[2], 0.3f, 1e-5f);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->blend, 0.25f);
  EXPECT_FALSE(BKE_paint_layers_projection_clip_get(*reloaded_layer));
  EXPECT_EQ(reloaded_layer->projection_object, nullptr);
}

TEST_F(PaintLayersDescription, projection_old_file_reads_the_facing_defaults)
{
  /* A file written before the facing limit and its flags existed carries the zeroed angle and
   * none of the flags: the read has to give it the defaults the payload gives a new frame,
   * except the flip, which stays off so the arrow still enters the surface. */
  Material *ma = BKE_material_add(bmain, "ProjectionFacingMigrationMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ProjectionFacingMigrationImage"));
  id_us_plus(&image->id);

  MaterialPaintLayer *layer = paint_layer_add(*ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  paint_layer_channel_add(*layer, *image);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  layer->projection->angle = 0.0f;
  layer->projection->flag = MA_PAINT_LAYER_PROJECTION_CLIP;

  char filepath[FILE_MAX];
  BLI_path_join(filepath,
                sizeof(filepath),
                BKE_tempdir_session(),
                "paint_layers_projection_facing_migration.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "ProjectionFacingMigrationMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_layer = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_layer, nullptr);
  ASSERT_NE(reloaded_layer->projection, nullptr);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->angle, PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_TRUE(BKE_paint_layers_projection_depth_get(*reloaded_layer));
  EXPECT_TRUE(BKE_paint_layers_projection_fit_aspect_get(*reloaded_layer));
  EXPECT_TRUE(BKE_paint_layers_projection_facing_get(*reloaded_layer));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_invert_get(*reloaded_layer));
  /* The two newer bits were not in the file: both read as their off state. */
  EXPECT_FALSE(BKE_paint_layers_projection_snap_get(*reloaded_layer));
  EXPECT_FALSE(BKE_paint_layers_projection_size_from_controller_get(*reloaded_layer));

  BLO_blendfiledata_free(bfd);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection frame orientation helpers (WP9)
 * \{ */

TEST_F(PaintLayersDescription, projection_frame_matrix_orthonormal_and_round_trips)
{
  /* For a set of representative normals and rolls, frame_matrix produces an orthonormal matrix
   * with det=+1, and frame_from_matrix recovers the normal+roll. */
  const float pi = M_PI;
  /* Normals: ±axes, diagonal, near-threshold (|n·Z| just below and just above 0.999). */
  const float near_below[3] = {0.0f, sqrtf(1.0f - 0.998f * 0.998f), 0.998f};  /* dot≈ 0.998 < 0.999. */
  const float near_above[3] = {0.0f, sqrtf(1.0f - 0.9995f * 0.9995f), 0.9995f};  /* dot≈ 0.9995 > 0.999. */
  const float diag[3] = {0.577f, 0.577f, 0.577f};
  const float nx[3] = {1.0f, 0.0f, 0.0f};
  const float nnx[3] = {-1.0f, 0.0f, 0.0f};
  const float ny[3] = {0.0f, 1.0f, 0.0f};
  const float nny[3] = {0.0f, -1.0f, 0.0f};
  const float nz[3] = {0.0f, 0.0f, 1.0f};
  const float nnz[3] = {0.0f, 0.0f, -1.0f};
  const float rolls[] = {0.0f, pi / 2.0f, -pi / 2.0f, pi, 0.3f};
  const int num_rolls = 5;
  const float *normals[] = {nx, nnx, ny, nny, nz, nnz, diag, near_below, near_above};
  const int num_normals = 9;
  for (const int ni : IndexRange(num_normals)) {
    for (const int ri : IndexRange(num_rolls)) {
      const float *n = normals[ni];
      const float r = rolls[ri];
      float mat[3][3];
      BKE_paint_layers_projection_frame_matrix(n, r, mat);
      /* Orthonormality: rows unit and perpendicular. */
      for (const int i : IndexRange(3)) {
        const float len_sq = mat[i][0] * mat[i][0] + mat[i][1] * mat[i][1] +
                             mat[i][2] * mat[i][2];
        EXPECT_NEAR(len_sq, 1.0f, 1e-5f)
            << "normal[" << ni << "] roll=" << r << " row " << i << " not unit";
        for (const int j : IndexRange(i + 1, 3)) {
          const float dot_val = mat[i][0] * mat[j][0] + mat[i][1] * mat[j][1] +
                                mat[i][2] * mat[j][2];
          EXPECT_NEAR(dot_val, 0.0f, 1e-5f)
              << "normal[" << ni << "] rows " << i << "," << j << " not perpendicular";
        }
      }
      /* det = +1: cross(row0, row1) dot row2 = 1. */
      float cr[3];
      cross_v3_v3v3(cr, mat[0], mat[1]);
      const float det_val = dot_v3v3(cr, mat[2]);
      EXPECT_NEAR(det_val, 1.0f, 1e-5f)
          << "normal[" << ni << "] roll=" << r << " det not +1";
      /* Axis 2 (`mat[2]`) = the normalized normal. */
      float expected[3];
      copy_v3_v3(expected, n);
      normalize_v3(expected);
      EXPECT_NEAR(mat[2][0], expected[0], 1e-5f);
      EXPECT_NEAR(mat[2][1], expected[1], 1e-5f);
      EXPECT_NEAR(mat[2][2], expected[2], 1e-5f);
      /* Round-trip. */
      float rec_n[3];
      float rec_r;
      BKE_paint_layers_projection_frame_from_matrix(mat, rec_n, &rec_r);
      for (const int i : IndexRange(3)) {
        EXPECT_NEAR(rec_n[i], expected[i], 1e-5f) << "normal round-trip axis " << i;
      }
      /* Compare roll as angle mod 2*pi. */
      EXPECT_NEAR(sinf(rec_r), sinf(r), 1e-5f);
      EXPECT_NEAR(cosf(rec_r), cosf(r), 1e-5f);
    }
  }
}

TEST_F(PaintLayersDescription, projection_frame_euler_agrees_with_frame_matrix)
{
  /* frame_euler gives the same rotation as frame_matrix when fed to eul_to_mat3. */
  const float normals[][3] = {
      {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}};
  const float rolls[] = {0.0f, M_PI / 4.0f, -M_PI / 2.0f};
  for (const auto &n : normals) {
    for (const float roll : rolls) {
      float mat_direct[3][3];
      BKE_paint_layers_projection_frame_matrix(n, roll, mat_direct);
      float euler[3];
      BKE_paint_layers_projection_frame_euler(n, roll, euler);
      float mat_from_euler[3][3];
      eul_to_mat3(mat_from_euler, euler);
      for (const int i : IndexRange(3)) {
        for (const int j : IndexRange(3)) {
          EXPECT_NEAR(mat_direct[i][j], mat_from_euler[i][j], 1e-5f)
              << " mismatch at (" << i << "," << j << ")";
        }
      }
    }
  }
}

TEST_F(PaintLayersDescription, projection_frame_y_points_up_for_horizontal_normal)
{
  /* With roll=0 and normal = +X, column 1 (the frame Y axis) should point in the +Z direction
   * (the reference up), because the image top faces up for a wall projected along +X. */
  const float normal[3] = {1.0f, 0.0f, 0.0f};
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(normal, 0.0f, mat);
  /* `mat[1]` = Y axis in parent space. */
  EXPECT_NEAR(mat[1][0], 0.0f, 1e-5f);
  EXPECT_NEAR(mat[1][1], 0.0f, 1e-5f);
  /* +Z or -Z depending on cross product orientation; the important thing is it's unit in Z. */
  EXPECT_GT(fabsf(mat[1][2]), 0.99f) << "Y axis should be nearly vertical for horizontal normal";
}

TEST_F(PaintLayersDescription, projection_set_orientation_normalizes_and_rejects_invalid)
{
  Material &ma = *BKE_material_add(bmain, "SetOrientMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "OrientFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* Non-normalized normal is normalized before storing. */
  const float long_n[3] = {0.0f, 0.0f, 5.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_orientation(ma, fill, long_n, 0.0f));
  EXPECT_NEAR(fill->projection->normal[2], 1.0f, 1e-6f);

  /* Zero normal becomes (0,0,1). */
  const float zero_n[3] = {0.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_orientation(ma, fill, zero_n, 1.0f));
  EXPECT_NEAR(fill->projection->normal[0], 0.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->normal[2], 1.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->roll, 1.0f, 1e-6f);

  /* NaN rejected, values unchanged. */
  const float nan_n[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f};
  EXPECT_FALSE(BKE_paint_layers_projection_set_orientation(ma, fill, nan_n, 0.0f));
  EXPECT_NEAR(fill->projection->roll, 1.0f, 1e-6f);

  /* Inf in roll rejected. */
  const float good_n[3] = {1.0f, 0.0f, 0.0f};
  EXPECT_FALSE(BKE_paint_layers_projection_set_orientation(
      ma, fill, good_n, std::numeric_limits<float>::infinity()));
}

TEST_F(PaintLayersDescription, projection_set_roll_wraps_to_pm_pi)
{
  Material &ma = *BKE_material_add(bmain, "SetRollMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "RollFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* 3pi/2 wraps to -pi/2. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_roll(ma, fill, 3.0f * M_PI / 2.0f));
  EXPECT_NEAR(fill->projection->roll, -M_PI / 2.0f, 1e-5f);

  /* -3pi/2 wraps to pi/2. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_roll(ma, fill, -3.0f * M_PI / 2.0f));
  EXPECT_NEAR(fill->projection->roll, M_PI / 2.0f, 1e-5f);

  /* pi stays pi. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_roll(ma, fill, M_PI));
  EXPECT_NEAR(fill->projection->roll, M_PI, 1e-5f);
}

TEST_F(PaintLayersDescription, projection_set_values_rejects_foreign_and_normalizes_size)
{
  Material &ma = *BKE_material_add(bmain, "SetValuesMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "ValuesFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* Size normalization: zero axis reads as 1.0. */
  const float center[3] = {0.0f, 0.0f, 0.0f};
  const float size[3] = {0.0f, -4.0f, 2.0f};
  const float normal[3] = {0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values(ma, fill, center, size, normal, 0.0f));
  EXPECT_NEAR(fill->projection->size[0], 1.0f, 1e-6f);
  EXPECT_NEAR(fabsf(fill->projection->size[1]), 4.0f, 1e-3f);

  /* A layer not owned by ma returns false. */
  Material &other = *BKE_material_add(bmain, "OtherMat");
  EXPECT_FALSE(BKE_paint_layers_projection_set_values(
      other, fill, center, size, normal, 0.0f));
}

TEST_F(PaintLayersDescription, projection_set_rotation_wrapper)
{
  /* set_rotation(Euler) stores normal+roll; frame_euler round-trips to the same matrix. */
  Material &ma = *BKE_material_add(bmain, "SetRotWrapMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "RotWrapFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  const float euler[3] = {0.4f, -0.3f, 1.2f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_rotation(ma, fill, euler));

  /* Compare matrices rather than angles (Euler is ambiguous). */
  float mat_orig[3][3];
  eul_to_mat3(mat_orig, euler);
  float mat_stored[3][3];
  BKE_paint_layers_projection_frame_matrix(
      fill->projection->normal, fill->projection->roll, mat_stored);
  for (const int i : IndexRange(3)) {
    for (const int j : IndexRange(3)) {
      EXPECT_NEAR(mat_orig[i][j], mat_stored[i][j], 1e-5f)
          << "(" << i << "," << j << ")";
    }
  }
}

TEST_F(PaintLayersDescription, projection_set_rotation_round_trips_near_the_poles)
{
  /* Euler triples whose frame +Z is on or next to the pole (the +/-0.999 reference switch of the
   * frame basis): the stored normal+roll must give back the same rotation. */
  Material &ma = *BKE_material_add(bmain, "PoleRotMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PoleRotFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  const float eulers[][3] = {{0.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f},
                             {0.03f, 0.02f, 0.5f},
                             {0.04f, -0.03f, -2.0f},
                             {float(M_PI), 0.0f, 0.3f},
                             {float(M_PI) - 0.03f, 0.02f, 0.3f}};
  for (const auto &euler : eulers) {
    ASSERT_TRUE(BKE_paint_layers_projection_set_rotation(ma, fill, euler));
    float expected[3][3];
    eul_to_mat3(expected, euler);
    float read_euler[3];
    float normal[3];
    BKE_paint_layers_projection_normal_get(*fill, normal);
    BKE_paint_layers_projection_frame_euler(normal, fill->projection->roll, read_euler);
    float actual[3][3];
    eul_to_mat3(actual, read_euler);
    for (const int i : IndexRange(3)) {
      for (const int j : IndexRange(3)) {
        EXPECT_NEAR(expected[i][j], actual[i][j], 1e-4f)
            << "euler " << euler[0] << " " << euler[1] << " " << euler[2] << " at (" << i << ","
            << j << ")";
      }
    }
  }
}

TEST_F(PaintLayersDescription, projection_normal_get_reads_zero_as_up)
{
  Material &ma = *BKE_material_add(bmain, "NormalGetMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "NormalGetFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  float normal[3];
  /* A payload that never went through a setter (a damaged file) may hold a zero or a long normal. */
  zero_v3(fill->projection->normal);
  BKE_paint_layers_projection_normal_get(*fill, normal);
  EXPECT_NEAR(normal[2], 1.0f, 1e-6f);
  fill->projection->normal[0] = 3.0f;
  BKE_paint_layers_projection_normal_get(*fill, normal);
  EXPECT_NEAR(len_v3(normal), 1.0f, 1e-6f);
  EXPECT_NEAR(normal[0], 1.0f, 1e-6f);
}

TEST_F(PaintLayersDescription, projection_cube_layout_unknown_value_reads_as_default)
{
  Material &ma = *BKE_material_add(bmain, "CubeLayoutMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "CubeLayoutFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_CUBE));
  EXPECT_TRUE(BKE_paint_layers_projection_set_cube_layout(
      ma, fill, MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3));
  EXPECT_FALSE(BKE_paint_layers_projection_set_cube_layout(
      ma, fill, MA_PAINT_LAYER_PROJECTION_CUBE_LAYOUT_NUM));
  EXPECT_FALSE(BKE_paint_layers_projection_set_cube_layout(ma, fill, -1));
  fill->projection->cube_layout = 99;
  EXPECT_EQ(BKE_paint_layers_projection_cube_layout_get(*fill),
            MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3);
  /* The table lookup agrees: an unknown layout is the first row. */
  EXPECT_EQ(&BKE_paint_layers_projection_cube_layout_info(99),
            &BKE_paint_layers_projection_cube_layout_info(MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3));
}

TEST_F(PaintLayersDescription, projection_preset_decal_resets_angle_and_flat_keeps_it)
{
  Material &ma = *BKE_material_add(bmain, "PresetAngleMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetAngleFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 45.0f));
  /* The angle is not part of what the preset getter reads. */
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_FLAT));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_FLAT);
  EXPECT_NEAR(BKE_paint_layers_projection_angle_get(*fill), 45.0f, 1e-5f);
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);
  EXPECT_NEAR(BKE_paint_layers_projection_angle_get(*fill),
              PAINT_LAYER_PROJECTION_ANGLE_DEFAULT,
              1e-5f);
}

TEST_F(PaintLayersDescription, projection_ensure_defaults)
{
  Material &ma = *BKE_material_add(bmain, "EnsureDefaultsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "DefaultsFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_NE(fill->projection, nullptr);
  EXPECT_NEAR(fill->projection->normal[0], 0.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->normal[1], 0.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->normal[2], 1.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->roll, 0.0f, 1e-6f);
  /* frame_matrix(0,0,1; 0) gives the identity. */
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(fill->projection->normal, fill->projection->roll, mat);
  for (const int i : IndexRange(3)) {
    for (const int j : IndexRange(3)) {
      EXPECT_NEAR(mat[i][j], (i == j) ? 1.0f : 0.0f, 1e-5f);
    }
  }
}

TEST_F(PaintLayersDescription, projection_normal_roll_blend_round_trip)
{
  /* Write a row with normal=(1,0,0) roll=0.7, save/read .blend, verify values. */
  Material *ma = BKE_material_add(bmain, "NormalRollRoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "NormalRollRTImg"));
  id_us_plus(&image->id);
  MaterialPaintLayer *layer = paint_layer_add(*ma, "NRRTRow");
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  paint_layer_channel_add(*layer, *image);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  const float center[3] = {0.5f, -1.0f, 2.0f};
  const float size[3] = {2.0f, 3.0f, 4.0f};
  const float normal[3] = {1.0f, 0.0f, 0.0f};
  const float roll = 0.7f;
  ASSERT_TRUE(BKE_paint_layers_projection_set_values(*ma, layer, center, size, normal, roll));

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(),
                "normal_roll_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);
  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "NormalRollRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *rl = paint_layers_first(*reloaded);
  ASSERT_NE(rl, nullptr);
  ASSERT_NE(rl->projection, nullptr);
  EXPECT_NEAR(rl->projection->normal[0], 1.0f, 1e-6f);
  EXPECT_NEAR(rl->projection->normal[1], 0.0f, 1e-6f);
  EXPECT_NEAR(rl->projection->normal[2], 0.0f, 1e-6f);
  EXPECT_NEAR(rl->projection->roll, 0.7f, 1e-6f);
  EXPECT_NEAR(rl->projection->center[0], 0.5f, 1e-6f);
  BLO_blendfiledata_free(bfd);
}

TEST_F(PaintLayersDescription, projection_preset_decal_survives_snap_and_size_from_controller)
{
  Material &ma = *BKE_material_add(bmain, "PresetSnapMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetSnapFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);

  /* Turning on SNAP and SIZE_FROM_CONTROLLER does not change the preset to CUSTOM. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_snap(ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_projection_set_size_from_controller(ma, fill, true));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);
}

TEST_F(PaintLayersDescription, projection_preset_flat_turns_off_four_flags)
{
  Material &ma = *BKE_material_add(bmain, "PresetFlatMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetFlatFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  /* Start from Decal (all 4 on) and switch to Flat. */
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  ASSERT_TRUE(BKE_paint_layers_projection_set_facing_invert(ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_FLAT));
  EXPECT_FALSE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_depth_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_fit_aspect_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_invert_get(*fill));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_FLAT);
}

TEST_F(PaintLayersDescription, projection_preset_decal_turns_on_four_flags)
{
  Material &ma = *BKE_material_add(bmain, "PresetDecalMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetDecalFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_FLAT));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  EXPECT_TRUE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_depth_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_fit_aspect_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_facing_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_invert_get(*fill));
  EXPECT_NEAR(BKE_paint_layers_projection_angle_get(*fill),
              PAINT_LAYER_PROJECTION_ANGLE_DEFAULT, 1e-5f);
}

TEST_F(PaintLayersDescription, projection_preset_custom_on_manual_change)
{
  Material &ma = *BKE_material_add(bmain, "PresetCustomMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetCustomFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  /* Manually turn off CLIP -> no longer Decal or Flat. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_clip(ma, fill, false));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_CUSTOM);
  /* apply_preset(CUSTOM) does not change anything. */
  const int16_t before = fill->projection->flag;
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_CUSTOM));
  EXPECT_EQ(fill->projection->flag, before);
}

TEST_F(PaintLayersDescription, projection_orientation_from_surface)
{
  /* ±X, ±Y, ±Z, diagonal surface normals → orientation_from_surface gives r_normal = +surface,
   * roll = 0. frame_matrix(r_normal, 0) is orthonormal with det=+1. */
  const float normals[][3] = {
      {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
      {0.577f, 0.577f, 0.577f}};
  for (const auto &sn : normals) {
    float r_normal[3];
    float r_roll;
    BKE_paint_layers_projection_orientation_from_surface(sn, r_normal, &r_roll);
    /* r_normal = normalize(surface_normal). */
    float expected[3];
    copy_v3_v3(expected, sn);
    normalize_v3(expected);
    for (const int i : IndexRange(3)) {
      EXPECT_NEAR(r_normal[i], expected[i], 1e-5f);
    }
    EXPECT_NEAR(r_roll, 0.0f, 1e-6f);
    /* Orthonormality of frame_matrix. */
    float mat[3][3];
    BKE_paint_layers_projection_frame_matrix(r_normal, r_roll, mat);
    float cr[3];
    cross_v3_v3v3(cr, mat[0], mat[1]);
    EXPECT_NEAR(dot_v3v3(cr, mat[2]), 1.0f, 1e-5f) << "det not +1";
  }
}

TEST_F(PaintLayersDescription, projection_orientation_from_surface_wall_x_axis_right)
{
  /* A vertical wall with surface_normal = +X: observer at +X looking in -X direction.
   * Frame X should be to the observer's right = +Y world (no mirror). */
  const float sn[3] = {1.0f, 0.0f, 0.0f};
  float r_normal[3];
  float r_roll;
  BKE_paint_layers_projection_orientation_from_surface(sn, r_normal, &r_roll);
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(r_normal, r_roll, mat);
  /* `mat[0]` = frame X axis in parent space. */
  const float *frame_x = mat[0];
  /* Observer right = +Y for wall at +X. */
  EXPECT_NEAR(frame_x[0], 0.0f, 1e-5f);
  EXPECT_NEAR(frame_x[1], 1.0f, 1e-5f) << "frame X should be +Y (not mirrored)";
  EXPECT_NEAR(frame_x[2], 0.0f, 1e-5f);
}

TEST_F(PaintLayersDescription, projection_facing_weight_positive_normal_toward_z)
{
  /* A face whose normal equals the frame +Z (dot = +1) must have weight = 1 without Flip.
   * frame_Z = normalize(surface_normal) from orientation_from_surface. */
  const float surface_normal[3] = {0.0f, 0.0f, 1.0f};
  float r_normal[3];
  float r_roll;
  BKE_paint_layers_projection_orientation_from_surface(surface_normal, r_normal, &r_roll);
  float scale, offset;
  BKE_paint_layers_projection_facing_weights(80.0f, false, scale, offset);
  /* component = dot(face_world_normal, frame_Z) = dot(surface_normal, r_normal) ≈ 1. */
  const float component = dot_v3v3(surface_normal, r_normal);
  const float weight = BKE_paint_layers_projection_facing_weight(scale, offset, component);
  EXPECT_NEAR(weight, 1.0f, 1e-4f) << "face looking at +Z should have weight 1";
}

TEST_F(PaintLayersDescription, projection_flip_column_signs)
{
  /* Flip: negate columns X and Z of frame_matrix(n, roll), keep Y. Then
   * frame_from_matrix gives the new normal/roll. Verify column properties. */
  const float n[3] = {0.6f, 0.0f, 0.8f};
  const float roll = 0.4f;
  float mat_orig[3][3];
  BKE_paint_layers_projection_frame_matrix(n, roll, mat_orig);
  /* Build flipped: negate axes 0 and 2. */
  float mat_flip[3][3];
  copy_m3_m3(mat_flip, mat_orig);
  negate_v3(mat_flip[0]);
  negate_v3(mat_flip[2]);
  float flipped_n[3], flipped_roll;
  BKE_paint_layers_projection_frame_from_matrix(mat_flip, flipped_n, &flipped_roll);
  /* Check: new normal = -original normal (column 2 negated). */
  for (const int i : IndexRange(3)) {
    EXPECT_NEAR(flipped_n[i], -n[i] / len_v3(n), 1e-5f) << "Z column sign";
  }
  /* Check: column 1 (Y) unchanged: frame_matrix(flipped_n, flipped_roll)[col1] == mat_orig[col1]. */
  float mat_verify[3][3];
  BKE_paint_layers_projection_frame_matrix(flipped_n, flipped_roll, mat_verify);
  for (const int i : IndexRange(3)) {
    EXPECT_NEAR(mat_verify[1][i], mat_orig[1][i], 1e-5f) << "Y axis preserved";
    EXPECT_NEAR(mat_verify[0][i], -mat_orig[0][i], 1e-5f) << "X axis negated";
    EXPECT_NEAR(mat_verify[2][i], -mat_orig[2][i], 1e-5f) << "Z axis negated";
  }
}

/** \} */

}  // namespace blender::bke::tests
