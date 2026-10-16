/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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
#include "BKE_paint_layers_edit.hh"
#include "BKE_report.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_fileops.h"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include <string>

namespace blender::bke::tests {

class PaintLayersDescription : public bke::BlenderGTestBase {
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
  row->source = 2;
  EXPECT_EQ(paint_layer_kind_of(*row), nullptr);
  EXPECT_FALSE(paint_layer_kind_role_allowed(*row));
  row->source = MA_PAINT_LAYER_SOURCE_IMAGE;
  row->kind_subtype = 99;
  EXPECT_EQ(paint_layer_kind_of(*row), nullptr);
}

TEST_F(PaintLayersDescription, maps_collect_reads_nested_rows_once)
{
  Material *ma = BKE_material_add(bmain, "CollectMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *folder = paint_layer_add(*ma, "Folder");
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

  /* The channel array is deep-copied, the channel's image stays owned elsewhere. */
  ASSERT_EQ(copy_first->channels_num, 1);
  ASSERT_NE(copy_first->channels, nullptr);
  EXPECT_NE(copy_first->channels, first->channels);
  EXPECT_EQ(copy_first->channels[0].image, channel_image);
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

  /* Opacity is clamped to [0, 1] and an edit marks the derived state stale. */
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 2.0f));
  EXPECT_FLOAT_EQ(layer->opacity, 1.0f);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.4f));
  EXPECT_FLOAT_EQ(layer->opacity, 0.4f);

  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  EXPECT_EQ(layer->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
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
  RNA_function_call(nullptr, &reports, &coll_ptr, func, &parms);
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
  RNA_function_call(nullptr, &reports, &coll_ptr, func, &parms);
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
  RNA_function_call(nullptr, &reports, &coll_ptr, func, &parms);
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

/* -------------------------------------------------------------------- */
/** \name Global channel set
 * \{ */

/** The build default: Base Color, Metallic, Roughness, Normal and AO. */
static uint16_t channel_set_default()
{
  return (uint16_t(1) << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
         (uint16_t(1) << PAINT_MATERIAL_CHANNEL_METALLIC) |
         (uint16_t(1) << PAINT_MATERIAL_CHANNEL_ROUGHNESS) |
         (uint16_t(1) << PAINT_MATERIAL_CHANNEL_NORMAL) |
         (uint16_t(1) << PAINT_MATERIAL_CHANNEL_AO);
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
  const uint16_t expected = channel_set_default() | (uint16_t(1) << PAINT_MATERIAL_CHANNEL_SPECULAR);

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

  /* Base Color is the material's constant and cannot be switched off. */
  EXPECT_FALSE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_TRUE(BKE_paint_layers_channel_in_set(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

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

  /* Writing routes through BKE_paint_layers_channel_set_mask_set; Base Color is forced back in. */
  RNA_property_enum_set(&ptr, prop, 1 << PAINT_MATERIAL_CHANNEL_METALLIC);
  const int after = RNA_property_enum_get(&ptr, prop);
  EXPECT_TRUE((after & (1 << PAINT_MATERIAL_CHANNEL_METALLIC)) != 0);
  EXPECT_TRUE((after & (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR)) != 0);

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

  /* The item belongs to its row, not to the stack: nothing new is a row of the material. */
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
  EXPECT_EQ(BKE_paint_layers_find(*ma, item->marker), nullptr);
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

  /* The item is not a row: structural edits stay refused. */
  EXPECT_FALSE(BKE_paint_layers_rename(*ma, item, "Renamed"));
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
  EXPECT_EQ(copy_item->channels[0].image, map);
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
  other->source = 2;
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

  /* A material copy owns its own correction and record array; the map stays the shared block. */
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
  EXPECT_EQ(copy_record->image, map);
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
}  // namespace blender::bke::tests
