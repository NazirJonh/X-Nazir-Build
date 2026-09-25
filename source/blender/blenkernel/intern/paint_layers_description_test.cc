/* SPDX-FileCopyrightText: 2026 Blender Authors
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
#include "BKE_attribute.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_report.hh"

#include "paint_layers_intern.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "IMB_imbuf_types.hh"

#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include <algorithm>
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

  MaterialPaintLayer *paint_layer_add(Material &ma, const char *name)
  {
    MaterialPaintLayer *layer = MEM_new<MaterialPaintLayer>(__func__);
    STRNCPY(layer->name, name);
    layer->marker = BLI_uuid_generate_random();
    layer->source = MA_PAINT_LAYER_SOURCE_IMAGE;
    layer->role = MA_PAINT_LAYER_ROLE_LAYER;
    layer->blend = MA_PAINT_LAYER_BLEND_MIX;
    layer->flag = MA_PAINT_LAYER_ENABLED;
    layer->opacity = 1.0f;
    BLI_addtail(&ma.paint_layers, layer);
    return layer;
  }

  MaterialPaintLayer *paint_layer_add_child(MaterialPaintLayer &parent, const char *name)
  {
    MaterialPaintLayer *child = MEM_new<MaterialPaintLayer>(__func__);
    STRNCPY(child->name, name);
    child->marker = BLI_uuid_generate_random();
    child->source = MA_PAINT_LAYER_SOURCE_IMAGE;
    child->role = MA_PAINT_LAYER_ROLE_LAYER;
    child->blend = MA_PAINT_LAYER_BLEND_MIX;
    child->flag = MA_PAINT_LAYER_ENABLED;
    child->opacity = 1.0f;
    /* A parent that holds a child is a folder, so the fixture matches the invariants. */
    parent.source = MA_PAINT_LAYER_SOURCE_STACK;
    BLI_addtail(&parent.children, child);
    return child;
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

  MaterialPaintLayer *paint_layer_add_correction(MaterialPaintLayer &parent, const char *name)
  {
    MaterialPaintLayer *correction = MEM_new<MaterialPaintLayer>(__func__);
    STRNCPY(correction->name, name);
    correction->marker = BLI_uuid_generate_random();
    correction->source = MA_PAINT_LAYER_SOURCE_IMAGE;
    correction->role = MA_PAINT_LAYER_ROLE_EFFECT;
    BLI_addtail(&parent.effects, correction);
    return correction;
  }

  MaterialPaintLayer *paint_layer_add_mask_item(MaterialPaintLayer &parent, const char *name)
  {
    MaterialPaintLayer *correction = MEM_new<MaterialPaintLayer>(__func__);
    STRNCPY(correction->name, name);
    correction->marker = BLI_uuid_generate_random();
    correction->source = MA_PAINT_LAYER_SOURCE_IMAGE;
    correction->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
    BLI_addtail(&parent.mask_stack, correction);
    return correction;
  }

  /** A mask item whose map lives in its Base-Color channel record, with a constant value too. */
  MaterialPaintLayer *paint_layer_add_mask_map(Material &ma,
                                               MaterialPaintLayer &parent,
                                               Image &image,
                                               const float value = 0.25f)
  {
    MaterialPaintLayer *item = paint_layer_add_mask_item(parent, "Mask");
    item->fill_color[0] = item->fill_color[1] = item->fill_color[2] = value;
    item->fill_color[3] = 1.0f;
    item->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
    if (BKE_paint_layers_channel_add(ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR) != nullptr) {
      BKE_paint_layers_channel_set_image(
          ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image);
    }
    return item;
  }

  static Image *paint_layer_mask_map(const MaterialPaintLayer &item)
  {
    for (int i = 0; i < item.channels_num; i++) {
      if (item.channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        return item.channels[i].image;
      }
    }
    return nullptr;
  }

  IDProperty *properties_add_string(IDProperty *&properties, const char *key, const char *value)
  {
    IDProperty *group = properties_ensure(properties);
    IDProperty *prop = IDP_NewString(value, key);
    IDP_AddToGroup(group, prop);
    return prop;
  }

  IDProperty *properties_add_id(IDProperty *&properties, const char *key, ID *id)
  {
    IDProperty *group = properties_ensure(properties);
    IDProperty *prop = bke::idprop::create(key, id).release();
    IDP_AddToGroup(group, prop);
    return prop;
  }

 private:
  IDProperty *properties_ensure(IDProperty *&properties)
  {
    if (properties == nullptr) {
      IDPropertyTemplate val = {0};
      properties = IDP_New(IDP_GROUP, &val, "LayerProperties");
    }
    return properties;
  }
};

/* Walk the description's layers through the untyped #ListBase fields. */
static MaterialPaintLayer *paint_layers_first(Material &ma)
{
  return static_cast<MaterialPaintLayer *>(ma.paint_layers.first);
}

/* Collect every marker of the stack, children, effects and mask items included, by walking. */
static void paint_layers_collect_markers(ListBase &list, Vector<bUUID> &r_markers)
{
  for (MaterialPaintLayer *layer = static_cast<MaterialPaintLayer *>(list.first); layer != nullptr;
       layer = layer->next)
  {
    r_markers.append(layer->marker);
    paint_layers_collect_markers(layer->children, r_markers);
    paint_layers_collect_markers(layer->effects, r_markers);
    paint_layers_collect_markers(layer->mask_stack, r_markers);
  }
}

TEST_F(PaintLayersDescription, fresh_material_has_no_layers)
{
  Material *ma = BKE_material_add(bmain, "Mat");
  EXPECT_TRUE(BLI_listbase_is_empty(&ma->paint_layers));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERED, 0);

  ma->paint_layers_flag = MA_PAINT_LAYERED;
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERED, MA_PAINT_LAYERED);

  ma->paint_layers_flag = {};
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERED, 0);
}

TEST_F(PaintLayersDescription, blend_round_trip_keeps_layers)
{
  Material *ma = BKE_material_add(bmain, "RoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);
  /* The generated tree is not persisted; the read path must reset it. */
  ma->paint_layers_tree = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "RoundTripTree"));

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "RoundTripImage"));
  id_us_plus(&image->id);

  Image *mask_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "RoundTripMask"));
  id_us_plus(&mask_image->id);

  Material *source = BKE_material_add(bmain, "RoundTripSource");
  id_us_plus(&source->id);

  MaterialPaintLayer *parent = paint_layer_add(*ma, "Parent");
  const bUUID parent_marker = parent->marker;
  MaterialPaintLayer *child = paint_layer_add_child(*parent, "Child");
  const bUUID child_marker = child->marker;
  paint_layer_channel_add(*parent, *image);
  paint_layer_add_mask_map(*ma, *parent, *mask_image, 0.25f);
  parent->material = source;
  parent->bake = MEM_new<MaterialPaintLayerBake>(__func__);
  parent->bake->size = 1024;
  parent->bake->hash[0] = 0xDEADBEEF;
  parent->bake->hash[1] = 0x01234567;
  parent->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = image;
  paint_layer_add_correction(*parent, "Correction");
  properties_add_string(parent->properties, "greeting", "hello");

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
  EXPECT_EQ(reloaded->paint_layers_flag & MA_PAINT_LAYERED, MA_PAINT_LAYERED);
  /* The generated tree is a regular node group in Main and is persisted with the file, so the file
   * renders without any regeneration. */
  ASSERT_NE(reloaded->paint_layers_tree, nullptr);
  EXPECT_STREQ(reloaded->paint_layers_tree->id.name + 2, "RoundTripTree");

  MaterialPaintLayer *reloaded_parent = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_parent, nullptr);
  EXPECT_STREQ(reloaded_parent->name, "Parent");
  EXPECT_TRUE(BLI_uuid_equal(reloaded_parent->marker, parent_marker));

  MaterialPaintLayer *reloaded_child = static_cast<MaterialPaintLayer *>(
      reloaded_parent->children.first);
  ASSERT_NE(reloaded_child, nullptr);
  EXPECT_STREQ(reloaded_child->name, "Child");
  EXPECT_TRUE(BLI_uuid_equal(reloaded_child->marker, child_marker));

  ASSERT_EQ(reloaded_parent->channels_num, 1);
  ASSERT_NE(reloaded_parent->channels, nullptr);
  ASSERT_NE(reloaded_parent->channels[0].image, nullptr);
  EXPECT_STREQ(reloaded_parent->channels[0].image->id.name + 2, "RoundTripImage");

  MaterialPaintLayer *reloaded_mask = static_cast<MaterialPaintLayer *>(
      reloaded_parent->mask_stack.first);
  ASSERT_NE(reloaded_mask, nullptr);
  ASSERT_NE(paint_layer_mask_map(*reloaded_mask), nullptr);
  EXPECT_STREQ(paint_layer_mask_map(*reloaded_mask)->id.name + 2, "RoundTripMask");
  EXPECT_FLOAT_EQ(reloaded_mask->fill_color[0], 0.25f);

  /* The source material reference is remapped on read, not left as a stale address. */
  ASSERT_NE(reloaded_parent->material, nullptr);
  EXPECT_STREQ(reloaded_parent->material->id.name + 2, "RoundTripSource");
  ASSERT_NE(reloaded_parent->bake, nullptr);
  EXPECT_EQ(reloaded_parent->bake->size, 1024);
  EXPECT_EQ(reloaded_parent->bake->hash[0], 0xDEADBEEF);
  EXPECT_EQ(reloaded_parent->bake->hash[1], 0x01234567);
  ASSERT_NE(reloaded_parent->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_STREQ(reloaded_parent->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]->id.name + 2,
               "RoundTripImage");

  MaterialPaintLayer *reloaded_correction = static_cast<MaterialPaintLayer *>(
      reloaded_parent->effects.first);
  ASSERT_NE(reloaded_correction, nullptr);
  EXPECT_STREQ(reloaded_correction->name, "Correction");

  ASSERT_NE(reloaded_parent->properties, nullptr);
  const IDProperty *greeting = IDP_GetPropertyTypeFromGroup(
      reloaded_parent->properties, "greeting", IDP_STRING);
  ASSERT_NE(greeting, nullptr);
  EXPECT_STREQ(IDP_string_get(greeting), "hello");

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(PaintLayersDescription, foreach_id_walks_channel_images)
{
  Material *ma = BKE_material_add(bmain, "WalkMat");
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "WalkImage"));
  Image *mask_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "WalkMaskImage"));
  Image *property_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "WalkPropertyImage"));
  bNodeTree *group = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "WalkGroup"));
  Material *source = BKE_material_add(bmain, "WalkSource");

  MaterialPaintLayer *layer = paint_layer_add(*ma, "WalkLayer");
  paint_layer_channel_add(*layer, *image);
  paint_layer_add_mask_map(*ma, *layer, *mask_image);
  layer->custom_group = group;
  layer->material = source;
  properties_add_id(layer->properties, "image_ref", &property_image->id);

  Vector<ID *> visited;
  auto collector = [](LibraryIDLinkCallbackData *data) -> int {
    if (*data->id_pointer != nullptr) {
      static_cast<Vector<ID *> *>(data->user_data)->append(*data->id_pointer);
    }
    return IDWALK_RET_NOP;
  };
  BKE_library_foreach_ID_link(bmain, &ma->id, collector, &visited, IDWALK_READONLY);

  EXPECT_TRUE(visited.contains(&image->id));
  EXPECT_TRUE(visited.contains(&mask_image->id));
  EXPECT_TRUE(visited.contains(&group->id));
  EXPECT_TRUE(visited.contains(&source->id));
  EXPECT_TRUE(visited.contains(&property_image->id));
}

TEST_F(PaintLayersDescription, copy_does_not_share_generated_tree)
{
  Material *ma = BKE_material_add(bmain, "CopyTreeMat");
  bNodeTree *generated = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "LayerTree"));
  ma->paint_layers_tree = generated;

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(ma->paint_layers_tree, generated);
  /* A copied material owns its own generated tree: sharing it would let either material's stack
   * rewrite the other's render. */
  ASSERT_NE(copy->paint_layers_tree, nullptr);
  EXPECT_NE(copy->paint_layers_tree, generated);

  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersDescription, copy_keeps_layers_but_not_tree)
{
  Material *ma = BKE_material_add(bmain, "CopyLayersMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  MaterialPaintLayer *first = paint_layer_add(*ma, "First");
  const bUUID first_marker = first->marker;
  Image *mask_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CopyMask"));
  Image *channel_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "CopyChannel"));
  paint_layer_add_mask_map(*ma, *first, *mask_image);
  paint_layer_channel_add(*first, *channel_image);
  MaterialPaintLayer *first_child = paint_layer_add_child(*first, "FirstChild");
  MaterialPaintLayer *first_correction = paint_layer_add_correction(*first, "FirstCorrection");
  properties_add_string(first->properties, "key", "value");
  MaterialPaintLayer *second = paint_layer_add(*ma, "Second");
  const bUUID second_marker = second->marker;
  ma->active_layer_marker = second_marker;

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);

  EXPECT_TRUE(BLI_uuid_equal(copy->active_layer_marker, second_marker));
  EXPECT_EQ(copy->paint_layers_tree, nullptr);

  MaterialPaintLayer *copy_first = paint_layers_first(*copy);
  ASSERT_NE(copy_first, nullptr);
  EXPECT_NE(copy_first, first);
  EXPECT_STREQ(copy_first->name, "First");
  EXPECT_TRUE(BLI_uuid_equal(copy_first->marker, first_marker));

  /* The mask stack and the properties are owned sub-data too, and must be a deep copy; the image
   * they reference stays owned by whoever owned it before. */
  MaterialPaintLayer *copy_mask = static_cast<MaterialPaintLayer *>(copy_first->mask_stack.first);
  MaterialPaintLayer *first_mask = static_cast<MaterialPaintLayer *>(first->mask_stack.first);
  ASSERT_NE(copy_mask, nullptr);
  EXPECT_NE(copy_mask, first_mask);
  EXPECT_EQ(paint_layer_mask_map(*copy_mask), mask_image);

  /* The channel array is deep-copied, the channel's image stays owned elsewhere. */
  ASSERT_EQ(copy_first->channels_num, 1);
  ASSERT_NE(copy_first->channels, nullptr);
  EXPECT_NE(copy_first->channels, first->channels);
  EXPECT_EQ(copy_first->channels[0].image, channel_image);
  EXPECT_EQ(copy_first->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* Nested children and effects are copied recursively, never aliased. */
  MaterialPaintLayer *copy_child = static_cast<MaterialPaintLayer *>(copy_first->children.first);
  ASSERT_NE(copy_child, nullptr);
  EXPECT_NE(copy_child, first_child);
  EXPECT_STREQ(copy_child->name, "FirstChild");
  MaterialPaintLayer *copy_correction = static_cast<MaterialPaintLayer *>(
      copy_first->effects.first);
  ASSERT_NE(copy_correction, nullptr);
  EXPECT_NE(copy_correction, first_correction);
  EXPECT_STREQ(copy_correction->name, "FirstCorrection");

  ASSERT_NE(copy_first->properties, nullptr);
  EXPECT_NE(copy_first->properties, first->properties);
  const IDProperty *copied_value = IDP_GetPropertyTypeFromGroup(
      copy_first->properties, "key", IDP_STRING);
  ASSERT_NE(copied_value, nullptr);
  EXPECT_STREQ(IDP_string_get(copied_value), "value");

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

TEST_F(PaintLayersDescription, add_appends_and_assigns_unique_marker)
{
  Material *ma = BKE_material_add(bmain, "AddMat");
  ma->paint_layers_flag = {};

  MaterialPaintLayer *first = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "First", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(first, nullptr);
  EXPECT_STREQ(first->name, "First");
  EXPECT_EQ(first->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_FALSE(BLI_uuid_is_nil(first->marker));
  EXPECT_EQ(paint_layers_first(*ma), first);

  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Second", nullptr, PaintLayerPlace::Above);
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
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERED, 0);
  /* A description mutation marks the generated tree stale. */
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersDescription, remove_frees_and_clears_active_when_needed)
{
  Material *ma = BKE_material_add(bmain, "RemoveMat");
  MaterialPaintLayer *first = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "First", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Second", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", second, PaintLayerPlace::Into);
  const bUUID first_marker = first->marker;
  const bUUID second_marker = second->marker;
  const bUUID child_marker = child->marker;

  BKE_paint_layers_active_set(*ma, second_marker);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), second_marker));

  /* Removing the active row (and its children) unlinks and frees the subtree, and clears
   * the active marker since it named a row that no longer exists. */
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, second));
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 1);
  EXPECT_EQ(paint_layers_first(*ma), first);
  EXPECT_EQ(BKE_paint_layers_find(*ma, second_marker), nullptr);
  EXPECT_EQ(BKE_paint_layers_find(*ma, child_marker), nullptr);
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(*ma)));

  /* The surviving row is intact and findable by marker. */
  EXPECT_EQ(BKE_paint_layers_find(*ma, first_marker), first);

  /* Removing a row that is not active leaves the active marker alone. */
  BKE_paint_layers_active_set(*ma, first_marker);
  MaterialPaintLayer *other = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Other", nullptr, PaintLayerPlace::Above);
  EXPECT_TRUE(BKE_paint_layers_remove(*ma, other));
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), first_marker));
}

TEST_F(PaintLayersDescription, find_resolves_by_marker_never_by_position)
{
  Material *ma = BKE_material_add(bmain, "FindMat");
  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Top", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", top, PaintLayerPlace::Into);
  MaterialPaintLayer *above = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Above", top, PaintLayerPlace::Above);
  MaterialPaintLayer *correction = paint_layer_add_correction(*top, "Correction");

  /* A marker resolves to the exact row, wherever it sits: nested, a sibling, a correction. */
  EXPECT_EQ(BKE_paint_layers_find(*ma, child->marker), child);
  EXPECT_EQ(BKE_paint_layers_find(*ma, top->marker), top);
  EXPECT_EQ(BKE_paint_layers_find(*ma, above->marker), above);
  EXPECT_EQ(BKE_paint_layers_find(*ma, correction->marker), correction);

  /* A marker no row carries resolves to nothing, even though rows exist. */
  const bUUID unknown = BLI_uuid_generate_random();
  EXPECT_EQ(BKE_paint_layers_find(*ma, unknown), nullptr);
}

TEST_F(PaintLayersDescription, duplicate_markers_resolve_deterministically_and_are_not_reused)
{
  Material *ma = BKE_material_add(bmain, "DuplicateMarkerMat");
  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Top", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", top, PaintLayerPlace::Into);
  MaterialPaintLayer *correction = paint_layer_add_correction(*top, "Correction");

  /* Force the pathological state the uniqueness guarantee exists to avoid: one marker on a
   * top-level row, a nested child and a correction. */
  const bUUID duplicate = BLI_uuid_generate_random();
  top->marker = duplicate;
  child->marker = duplicate;
  correction->marker = duplicate;

  /* find() is a depth-first pre-order walk -- a row before its children and corrections, rows in
   * list order -- so the top-level row is the deterministic winner. */
  EXPECT_EQ(BKE_paint_layers_find(*ma, duplicate), top);

  /* Every marker already in the stack, walked by hand: the new row must reuse none of them. */
  Vector<bUUID> markers;
  paint_layers_collect_markers(ma->paint_layers, markers);
  ASSERT_EQ(markers.size(), 3);

  MaterialPaintLayer *added = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Added", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(added, nullptr);
  for (const bUUID &marker : markers) {
    EXPECT_FALSE(BLI_uuid_equal(marker, added->marker));
  }
}

TEST_F(PaintLayersDescription, add_rejects_foreign_anchor)
{
  Material *owner = BKE_material_add(bmain, "AnchorOwnerMat");
  Material *foreign = BKE_material_add(bmain, "AnchorForeignMat");
  MaterialPaintLayer *top = BKE_paint_layers_add(
      *owner, MA_PAINT_LAYER_SOURCE_IMAGE, "Top", nullptr, PaintLayerPlace::Above);

  /* A row may never be linked under, above or below a row of another material. */
  EXPECT_EQ(BKE_paint_layers_add(
                *foreign, MA_PAINT_LAYER_SOURCE_IMAGE, "Above", top, PaintLayerPlace::Above),
            nullptr);
  EXPECT_EQ(BKE_paint_layers_add(
                *foreign, MA_PAINT_LAYER_SOURCE_IMAGE, "Below", top, PaintLayerPlace::Below),
            nullptr);
  EXPECT_EQ(BKE_paint_layers_add(
                *foreign, MA_PAINT_LAYER_SOURCE_IMAGE, "Into", top, PaintLayerPlace::Into),
            nullptr);
  EXPECT_TRUE(BLI_listbase_is_empty(&foreign->paint_layers));
  EXPECT_EQ(top->children.first, nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Structural edits: move, reorder, group, ungroup, duplicate
 * \{ */

TEST_F(PaintLayersDescription, move_relocates_subtree_and_refuses_own_subtree)
{
  Material *ma = BKE_material_add(bmain, "MoveMat");
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *middle = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Middle", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Top", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", middle, PaintLayerPlace::Into);
  BKE_paint_layers_active_set(*ma, child->marker);

  /* Below the bottom row: the whole subtree travels, markers untouched. */
  EXPECT_TRUE(BKE_paint_layers_move(*ma, middle, bottom, PaintLayerPlace::Below));
  EXPECT_EQ(paint_layers_first(*ma), middle);
  EXPECT_EQ(middle->next, bottom);
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(middle->children.first), child);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), child->marker));

  /* A row may not be its own anchor, nor move under its own subtree. */
  EXPECT_FALSE(BKE_paint_layers_move(*ma, middle, middle, PaintLayerPlace::Above));
  EXPECT_FALSE(BKE_paint_layers_move(*ma, middle, child, PaintLayerPlace::Into));
  EXPECT_FALSE(BKE_paint_layers_move(*ma, middle, child, PaintLayerPlace::Above));

  /* A null anchor parks the row on top of the top-level list. */
  EXPECT_TRUE(BKE_paint_layers_move(*ma, middle, nullptr, PaintLayerPlace::Above));
  EXPECT_EQ(paint_layers_first(*ma), bottom);
  EXPECT_EQ(bottom->next, top);
  EXPECT_EQ(top->next, middle);

  /* A foreign anchor is refused, like in add(). */
  Material *other = BKE_material_add(bmain, "MoveOtherMat");
  MaterialPaintLayer *foreign = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_SOURCE_IMAGE, "Foreign", nullptr, PaintLayerPlace::Above);
  EXPECT_FALSE(BKE_paint_layers_move(*ma, middle, foreign, PaintLayerPlace::Above));
}

TEST_F(PaintLayersDescription, reorder_moves_row_within_its_own_list_only)
{
  Material *ma = BKE_material_add(bmain, "ReorderMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "C", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", b, PaintLayerPlace::Into);

  /* Bottom-to-top: index 0 is the bottom row. The index is clamped to the list. */
  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, a, 99));
  EXPECT_EQ(paint_layers_first(*ma), b);
  EXPECT_EQ(b->next, c);
  EXPECT_EQ(c->next, a);

  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, a, 0));
  EXPECT_EQ(paint_layers_first(*ma), a);
  EXPECT_EQ(a->next, b);
  EXPECT_EQ(b->next, c);

  /* Reordering a nested row keeps its parent; index 0 is the first slot among siblings. */
  MaterialPaintLayer *child2 = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child2", b, PaintLayerPlace::Into);
  EXPECT_TRUE(BKE_paint_layers_reorder(*ma, child2, 0));
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(b->children.first), child2);
  EXPECT_EQ(child2->next, child);
  EXPECT_EQ(paint_layers_first(*ma), a);

  EXPECT_FALSE(BKE_paint_layers_reorder(*ma, nullptr, 0));
}

TEST_F(PaintLayersDescription, group_folds_rows_into_folder_and_ungroup_lifts_them_back)
{
  Material *ma = BKE_material_add(bmain, "GroupMat");
  MaterialPaintLayer *a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *c = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "C", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, b->marker);

  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, {a, b});
  ASSERT_NE(folder, nullptr);
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 2);
  EXPECT_EQ(paint_layers_first(*ma), folder);
  EXPECT_EQ(folder->next, c);
  /* Members keep their markers and order; the active marker still resolves. */
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(folder->children.first), a);
  EXPECT_EQ(a->next, b);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), b->marker));

  /* A folder is addressed like any row: Into puts a row inside it. */
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Inner", folder, PaintLayerPlace::Into);
  EXPECT_EQ(inner->prev, b);

  /* Ungrouping lifts the children into the folder's slot and frees the folder. */
  EXPECT_TRUE(BKE_paint_layers_ungroup(*ma, folder));
  EXPECT_EQ(BLI_listbase_count(&ma->paint_layers), 4);
  EXPECT_EQ(paint_layers_first(*ma), a);
  EXPECT_EQ(a->next, b);
  EXPECT_EQ(b->next, inner);
  EXPECT_EQ(inner->next, c);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), b->marker));

  /* Grouping rows from different lists, an empty selection and foreign rows are refused. */
  EXPECT_EQ(BKE_paint_layers_group(*ma, {}), nullptr);
  /* Siblings group even when not adjacent; the folder takes the lower member's slot. */
  MaterialPaintLayer *mix_folder = BKE_paint_layers_group(*ma, {a, inner});
  ASSERT_NE(mix_folder, nullptr);
  EXPECT_TRUE(BKE_paint_layers_ungroup(*ma, mix_folder));
  Material *other = BKE_material_add(bmain, "GroupOtherMat");
  MaterialPaintLayer *foreign = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_SOURCE_IMAGE, "Foreign", nullptr, PaintLayerPlace::Above);
  EXPECT_EQ(BKE_paint_layers_group(*ma, {a, foreign}), nullptr);
  EXPECT_FALSE(BKE_paint_layers_ungroup(*ma, nullptr));

  /* Ungrouping a folder whose active marker named the folder itself clears the cursor. */
  MaterialPaintLayer *folder2 = BKE_paint_layers_group(*ma, {a, b});
  ASSERT_NE(folder2, nullptr);
  BKE_paint_layers_active_set(*ma, folder2->marker);
  EXPECT_TRUE(BKE_paint_layers_ungroup(*ma, folder2));
  EXPECT_TRUE(BLI_uuid_is_nil(BKE_paint_layers_active_get(*ma)));
}

TEST_F(PaintLayersDescription, duplicate_copies_branch_with_fresh_markers_and_images)
{
  Material *ma = BKE_material_add(bmain, "DupMat");
  Image *channel_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "DupChannel"));
  Image *mask_image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "DupMask"));
  bNodeTree *group = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "DupGroup"));

  /* A folder with one Custom child: the child carries the data a deep copy has to handle. */
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "Layer", folder, PaintLayerPlace::Into);
  ASSERT_NE(layer, nullptr);
  MaterialPaintLayerChannel *channel = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(channel, nullptr);
  channel->image = channel_image;
  paint_layer_add_mask_map(*ma, *layer, *mask_image, 0.5f);
  layer->custom_group = group;
  id_us_plus(&group->id);
  properties_add_string(layer->properties, "key", "value");

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, folder);
  ASSERT_NE(copy, nullptr);

  /* The copy sits directly above the original, at the same nesting level. */
  EXPECT_EQ(copy->prev, folder);
  EXPECT_EQ(folder->next, copy);
  EXPECT_EQ(copy->next, nullptr);

  /* Every row of the branch has a fresh marker, distinct from its original. */
  EXPECT_FALSE(BLI_uuid_equal(copy->marker, folder->marker));
  MaterialPaintLayer *copy_layer = static_cast<MaterialPaintLayer *>(copy->children.first);
  ASSERT_NE(copy_layer, nullptr);
  EXPECT_FALSE(BLI_uuid_equal(copy_layer->marker, layer->marker));

  /* Channel and mask images are copies, not shares; the custom group is shared. */
  ASSERT_EQ(copy_layer->channels_num, 1);
  EXPECT_NE(copy_layer->channels[0].image, nullptr);
  EXPECT_NE(copy_layer->channels[0].image, channel_image);
  MaterialPaintLayer *copy_mask_item = static_cast<MaterialPaintLayer *>(
      copy_layer->mask_stack.first);
  ASSERT_NE(copy_mask_item, nullptr);
  ASSERT_NE(paint_layer_mask_map(*copy_mask_item), nullptr);
  EXPECT_NE(paint_layer_mask_map(*copy_mask_item), mask_image);
  EXPECT_FLOAT_EQ(copy_mask_item->fill_color[0], 0.5f);
  EXPECT_EQ(copy_layer->custom_group, group);

  /* The IDProperty group is a copy: editing it leaves the original alone. */
  ASSERT_NE(copy_layer->properties, nullptr);
  EXPECT_NE(copy_layer->properties, layer->properties);
  IDProperty *copy_key = IDP_GetPropertyTypeFromGroup(copy_layer->properties, "key", IDP_STRING);
  ASSERT_NE(copy_key, nullptr);
  EXPECT_STREQ(IDP_string_get(copy_key), "value");

  /* The originals still point at their own images. */
  EXPECT_EQ(layer->channels[0].image, channel_image);
  EXPECT_EQ(paint_layer_mask_map(
                *static_cast<MaterialPaintLayer *>(layer->mask_stack.first)),
            mask_image);

  BKE_paint_layers_remove(*ma, copy);
  EXPECT_EQ(BKE_paint_layers_find(*ma, folder->marker), folder);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name In-place edits: mask, channels, kind, values
 * \{ */

TEST_F(PaintLayersDescription, mask_add_inserts_a_base_item_and_toggles)
{
  Material *ma = BKE_material_add(bmain, "MaskMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);

  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.75f);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(BKE_paint_layers_role(*item), PaintLayerRole::MaskItem);
  EXPECT_FLOAT_EQ(item->fill_color[0], 0.75f);
  EXPECT_EQ(item->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_FLOAT_EQ(item->opacity, 1.0f);
  EXPECT_NE(item->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  /* The base mask is inserted first in the stack. */
  EXPECT_EQ(layer->mask_stack.first, item);

  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, item, false));
  EXPECT_EQ(item->flag & MA_PAINT_LAYER_ENABLED, 0);
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, item, true));
  EXPECT_NE(item->flag & MA_PAINT_LAYER_ENABLED, 0);

  /* A second mask item is inserted first too, so the newest mask is the base. */
  MaterialPaintLayer *second = BKE_paint_layers_mask_add(*ma, layer, 0.25f);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(layer->mask_stack.first, second);
  EXPECT_FLOAT_EQ(second->fill_color[0], 0.25f);
  EXPECT_EQ(BLI_listbase_count(&layer->mask_stack), 2);

  EXPECT_TRUE(BKE_paint_layers_remove(*ma, second));
  EXPECT_EQ(BLI_listbase_count(&layer->mask_stack), 1);
  EXPECT_EQ(layer->mask_stack.first, item);
}

TEST_F(PaintLayersDescription, fill_channel_value_is_value_only)
{
  Material *ma = BKE_material_add(bmain, "FillValue");
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "F", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  ma->paint_layers_flag = {};

  const float rough[4] = {0.75f, 0.75f, 0.75f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough));
  /* A Fill's channel value is a group input: value-only, no rebuild. */
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_TRUE((ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0);

  /* A fresh Fill started from the Principled defaults: Metallic 0, Roughness 0.5. */
  const MaterialPaintLayerChannel *record = nullptr;
  for (int i = 0; i < fill->channels_num; i++) {
    if (fill->channels[i].channel == PAINT_MATERIAL_CHANNEL_METALLIC) {
      record = &fill->channels[i];
    }
  }
  ASSERT_NE(record, nullptr);
  EXPECT_FLOAT_EQ(record->value[0], 0.0f);
}

TEST_F(PaintLayersDescription, authored_default_channels_set)
{
  Material *ma = BKE_material_add(bmain, "AuthoredChannels");

  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *paint);
  ASSERT_EQ(paint->channels_num, 3);
  for (int i = 0; i < paint->channels_num; i++) {
    EXPECT_EQ(paint->channels[i].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
    EXPECT_EQ(paint->channels[i].image, nullptr);
    EXPECT_FLOAT_EQ(paint->channels[i].value[3], 0.0f);
  }

  /* Idempotent, and an existing record is left alone. */
  BKE_paint_layers_channel_set_enabled(*ma, paint, PAINT_MATERIAL_CHANNEL_ROUGHNESS, false);
  BKE_paint_layers_default_channels_apply(*ma, *paint);
  EXPECT_EQ(paint->channels_num, 3);
  const MaterialPaintLayerChannel *rough = nullptr;
  for (int i = 0; i < paint->channels_num; i++) {
    if (paint->channels[i].channel == PAINT_MATERIAL_CHANNEL_ROUGHNESS) {
      rough = &paint->channels[i];
    }
  }
  ASSERT_NE(rough, nullptr);
  EXPECT_EQ(rough->state, MA_PAINT_LAYER_CHANNEL_DISABLED);

  /* A folder and a correction carry no participation of their own. */
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_default_channels_apply(*ma, *folder);
  EXPECT_EQ(folder->channels_num, 0);
  MaterialPaintLayer *correction = paint_layer_add_correction(*paint, "Correction");
  BKE_paint_layers_default_channels_apply(*ma, *correction);
  EXPECT_EQ(correction->channels_num, 0);
}

TEST_F(PaintLayersDescription, material_layer_source_is_validated)
{
  Material *ma = BKE_material_add(bmain, "MaterialOwner");
  Material *source = BKE_material_add(bmain, "MaterialSource");
  Material *other = BKE_material_add(bmain, "MaterialOther");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "M", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  /* Setting the source keeps its user count. */
  const int source_us = source->id.us;
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, layer, source));
  EXPECT_EQ(layer->material, source);
  EXPECT_EQ(source->id.us, source_us + 1);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* Switching moves the user count from the old source to the new one. */
  const int other_us = other->id.us;
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, layer, other));
  EXPECT_EQ(layer->material, other);
  EXPECT_EQ(source->id.us, source_us);
  EXPECT_EQ(other->id.us, other_us + 1);

  /* Setting the same source again is a no-op. */
  const int other_us_again = other->id.us;
  EXPECT_TRUE(BKE_paint_layers_set_material(*ma, layer, other));
  EXPECT_EQ(other->id.us, other_us_again);

  /* A null source and the material itself are refused, and leave the user count alone. */
  EXPECT_FALSE(BKE_paint_layers_set_material(*ma, layer, nullptr));
  EXPECT_FALSE(BKE_paint_layers_set_material(*ma, layer, ma));
  EXPECT_EQ(other->id.us, other_us_again);

  /* A row that is not a Material layer refuses the setter. */
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  EXPECT_FALSE(BKE_paint_layers_set_material(*ma, paint, source));
  EXPECT_EQ(paint->material, nullptr);

  /* A source that already bakes this material would form a cycle and is refused. Point the owner
   * back at the first source, so the reverse dependency exists. */
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, layer, source));
  MaterialPaintLayer *back = BKE_paint_layers_add(
      *source, MA_PAINT_LAYER_SOURCE_MATERIAL, "Back", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(back, nullptr);
  EXPECT_FALSE(BKE_paint_layers_set_material(*source, back, ma));
  EXPECT_EQ(back->material, nullptr);
  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*ma, *source));
  EXPECT_FALSE(BKE_paint_layers_material_depends_on(*source, *ma));
}

TEST_F(PaintLayersDescription, channel_add_remove_and_set_enabled)
{
  Material *ma = BKE_material_add(bmain, "ChannelMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
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

  /* Adding the Roughness record reallocated the channels array, so the earlier pointer into it is
   * stale; read the record back through the array. */
  base = &layer->channels[0];
  ASSERT_EQ(base->channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_TRUE(BKE_paint_layers_channel_set_enabled(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, false));
  EXPECT_EQ(layer->channels[0].state, MA_PAINT_LAYER_CHANNEL_DISABLED);
  /* Enabling an absent channel is refused -- adding one is an explicit step. */
  EXPECT_FALSE(BKE_paint_layers_channel_set_enabled(
      *ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL, true));

  EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  EXPECT_EQ(layer->channels_num, 1);
  EXPECT_EQ(layer->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_FALSE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_EQ(layer->channels_num, 0);
  EXPECT_EQ(layer->channels, nullptr);
}

TEST_F(PaintLayersDescription, source_change_converts_between_image_and_constant)
{
  Material *ma = BKE_material_add(bmain, "KindMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_EQ(layer->channels_num, 1);

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "KindMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  record->image = &image;
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.5f));

  /* Paint to Fill: the map goes, the color stays with an opaque alpha, and the pair's per-channel
   * blend/opacity override survives. */
  layer->fill_color[0] = 0.1f;
  layer->fill_color[1] = 0.2f;
  layer->fill_color[2] = 0.3f;
  layer->fill_color[3] = 0.0f;
  EXPECT_TRUE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  ASSERT_EQ(layer->channels_num, 1);
  EXPECT_EQ(layer->channels[0].image, nullptr);
  EXPECT_EQ(layer->channels[0].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
  /* The per (row, channel) override lives on the fixed settings array, untouched by the kind
   * change. */
  EXPECT_EQ(layer->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].blend,
            MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_FLOAT_EQ(layer->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].opacity, 0.5f);
  EXPECT_FLOAT_EQ(layer->fill_color[3], 1.0f);

  /* Fill to Paint: the color resets to a clean starting point for the first stroke; the record and
   * its overrides stay. */
  EXPECT_TRUE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_FLOAT_EQ(layer->fill_color[0], 0.0f);
  EXPECT_FLOAT_EQ(layer->fill_color[3], 1.0f);
  ASSERT_EQ(layer->channels_num, 1);
  EXPECT_FLOAT_EQ(layer->channel_settings[PAINT_MATERIAL_CHANNEL_BASE_COLOR].opacity, 0.5f);

  /* Same-source change is a no-op success; other sources are refused. */
  EXPECT_TRUE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_MATERIAL));
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_STACK));
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_NODE_GROUP));

  /* A correction is not a stack Layer, so source_change() refuses it even for Image/Constant. */
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  /* GUARD (RED-verified 2026-09-24, paint_layers.cc: BKE_paint_layers_source_change's
   * `role(*layer) != Layer` guard): removing it made this call return true instead of false. */
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, correction, MA_PAINT_LAYER_SOURCE_CONSTANT));
}

TEST_F(PaintLayersDescription, value_setters_write_and_tag)
{
  Material *ma = BKE_material_add(bmain, "SetterMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);

  const float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
  EXPECT_TRUE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.4f));
  EXPECT_TRUE(BKE_paint_layers_set_fill_color(*ma, layer, color));
  EXPECT_TRUE(BKE_paint_layers_set_color_tag(*ma, layer, 3));
  EXPECT_TRUE(BKE_paint_layers_rename(*ma, layer, "Renamed"));
  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));

  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_FLOAT_EQ(layer->opacity, 0.4f);
  EXPECT_FLOAT_EQ(layer->fill_color[1], 0.5f);
  EXPECT_EQ(layer->color_tag, 3);
  EXPECT_STREQ(layer->name, "Renamed");
  EXPECT_EQ(layer->flag & MA_PAINT_LAYER_ENABLED, 0);

  /* One of the topology setters among these -- blend, colour tag, rename, enabled -- marked the
   * tree stale. The value setters (opacity, fill colour) do not: they are group inputs. */
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  EXPECT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, true));
  EXPECT_NE(layer->flag & MA_PAINT_LAYER_ENABLED, 0);
}

TEST_F(PaintLayersDescription, set_blend_refuses_the_internal_normal_combine)
{
  Material *ma = BKE_material_add(bmain, "BlendMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);

  /* The Normal channel forces the operation; a stored value would be a switch the user cannot
   * make effective. */
  EXPECT_FALSE(BKE_paint_layers_set_blend(*ma, layer, MA_PAINT_LAYER_BLEND_NORMAL_COMBINE));
  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MIX);
}

TEST_F(PaintLayersDescription, issues_report_a_fill_correction_on_normal)
{
  Material *ma = BKE_material_add(bmain, "IssueMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  ASSERT_EQ(issues.size(), 1);
  EXPECT_EQ(issues[0].code, PaintLayersIssueCode::FillCorrectionOnNormal);
  EXPECT_EQ(issues[0].channel, int(PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(BLI_uuid_equal(issues[0].layer, layer->marker));
  EXPECT_TRUE(BLI_uuid_equal(issues[0].correction, correction->marker));
  ASSERT_NE(issues[0].text, nullptr);

  /* A Paint correction on Normal is supported, so it clears the issue. */
  EXPECT_TRUE(BKE_paint_layers_correction_source_set(*ma, correction, MA_PAINT_LAYER_SOURCE_IMAGE));
  BKE_paint_layers_issues_get(*ma, issues);
  EXPECT_TRUE(issues.is_empty());
}

TEST_F(PaintLayersDescription, colorspace_sample_round_trips)
{
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 2, 2, "sRGB", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(image, nullptr);
  STRNCPY(image->colorspace_settings.name, "sRGB");

  float rgba[4] = {0.5f, 0.25f, 0.75f, 1.0f};
  BKE_paint_layers_sample_to_linear(*image, rgba);
  /* sRGB decodes a midtone to a smaller linear value; this is what makes a CPU composite in scene
   * linear agree with the shader rather than staying in the map's encoded space. */
  EXPECT_LT(rgba[0], 0.5f);
  EXPECT_GT(rgba[0], 0.0f);

  BKE_paint_layers_sample_from_linear(*image, rgba);
  EXPECT_NEAR(rgba[0], 0.5f, 1e-3f);
  EXPECT_NEAR(rgba[1], 0.25f, 1e-3f);
  EXPECT_NEAR(rgba[2], 0.75f, 1e-3f);
  EXPECT_NEAR(rgba[3], 1.0f, 1e-6f);
}

TEST_F(PaintLayersDescription, composite_image_writes_a_float_channel)
{
  Material *ma = BKE_material_add(bmain, "CompositeMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *map = BKE_image_add_generated(
      bmain, 4, 4, "Map", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(map, nullptr);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  /* A float destination: the composite is written scene linear, premultiplied. */
  Image *dst = BKE_image_add_generated(
      bmain, 4, 4, "Dst", 32, true, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(dst, nullptr);
  ASSERT_TRUE(BKE_paint_layers_composite_image(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, *dst, nullptr, nullptr));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const float *pixels = ibuf->float_data();
  ASSERT_NE(pixels, nullptr);
  EXPECT_FLOAT_EQ(pixels[3], 1.0f);
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersDescription, folder_refuses_its_own_channels_and_reports_them)
{
  Material *ma = BKE_material_add(bmain, "FolderMat");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder));
  EXPECT_EQ(folder->source, MA_PAINT_LAYER_SOURCE_STACK);

  /* A folder has no maps: adding a channel to one is refused. */
  EXPECT_EQ(BKE_paint_layers_channel_add(*ma, folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  /* A settings-only record on a folder -- a per-channel blend/opacity override -- has no map and is
   * not an issue. */
  folder->channels = MEM_new_array<MaterialPaintLayerChannel>(1, __func__);
  folder->channels_num = 1;
  folder->channels[0].channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  folder->channels[0].state = MA_PAINT_LAYER_CHANNEL_ABSENT;
  folder->channels[0].image = nullptr;

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  EXPECT_TRUE(issues.is_empty());

  /* A record that somehow carries a map is reported. */
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *map = BKE_image_add_generated(
      bmain, 2, 2, "FolderMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(map, nullptr);
  folder->channels[0].image = map;
  folder->channels[0].state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  BKE_paint_layers_issues_get(*ma, issues);
  ASSERT_EQ(issues.size(), 1);
  EXPECT_EQ(issues[0].code, PaintLayersIssueCode::FolderHasMaps);
  EXPECT_TRUE(BLI_uuid_equal(issues[0].layer, folder->marker));
}

TEST_F(PaintLayersDescription, folder_kind_is_explicit_and_sticky)
{
  Material *ma = BKE_material_add(bmain, "StickyFolderMat");

  /* An explicitly created folder is a folder before it holds anything. */
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder));

  /* An empty folder accepts Into ... */
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(folder->children.first), child);

  /* ... and refuses its own channels. */
  EXPECT_EQ(BKE_paint_layers_channel_add(*ma, folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);

  /* Moving the last child out does not demote the folder: folder-ness is the kind, not the list. */
  ASSERT_TRUE(BKE_paint_layers_move(*ma, child, nullptr, PaintLayerPlace::Above));
  EXPECT_TRUE(BLI_listbase_is_empty(&folder->children));
  EXPECT_TRUE(BKE_paint_layers_is_folder(*folder));
  EXPECT_EQ(folder->source, MA_PAINT_LAYER_SOURCE_STACK);

  /* A non-folder that could never be a container refuses Into: correction, material and custom. */
  MaterialPaintLayer *correction = paint_layer_add_correction(*child, "Correction");
  EXPECT_EQ(BKE_paint_layers_add(
                *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "IntoCorr", correction, PaintLayerPlace::Into),
            nullptr);
  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  EXPECT_EQ(BKE_paint_layers_add(
                *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "IntoMat", material, PaintLayerPlace::Into),
            nullptr);
  MaterialPaintLayer *custom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "Custom", nullptr, PaintLayerPlace::Above);
  EXPECT_EQ(BKE_paint_layers_add(
                *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "IntoCustom", custom, PaintLayerPlace::Into),
            nullptr);
}

TEST_F(PaintLayersDescription, into_a_plain_row_is_refused_and_leaves_it_unchanged)
{
  Material *ma = BKE_material_add(bmain, "StrictIntoMat");
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Target", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *target_channel = BKE_paint_layers_channel_add(
      *ma, target, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(target_channel, nullptr);
  const int channels_before = target->channels_num;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Row", nullptr, PaintLayerPlace::Above);

  /* Into a plain row is refused: BKE never promotes a kind implicitly, and the target's map stays
   * exactly where it was, so nothing silently disappears from the render. */
  EXPECT_EQ(BKE_paint_layers_add(
                *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Into", target, PaintLayerPlace::Into),
            nullptr);
  EXPECT_FALSE(BKE_paint_layers_move(*ma, row, target, PaintLayerPlace::Into));
  EXPECT_EQ(target->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(target->channels_num, channels_before);
  EXPECT_EQ(target_channel,
            BKE_paint_layers_channel_add(*ma, target, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_TRUE(BLI_listbase_is_empty(&target->children));
  EXPECT_EQ(BKE_paint_layers_find(*ma, row->marker), row);

  /* An explicitly created folder, empty or not, accepts Into. */
  MaterialPaintLayer *empty_folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Empty", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *into_empty = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "IntoEmpty", empty_folder, PaintLayerPlace::Into);
  ASSERT_NE(into_empty, nullptr);
  EXPECT_EQ(static_cast<MaterialPaintLayer *>(empty_folder->children.first), into_empty);
}

TEST_F(PaintLayersDescription, non_folder_with_children_is_reported)
{
  Material *ma = BKE_material_add(bmain, "StrayChildrenMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *stray = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Stray", nullptr, PaintLayerPlace::Above);

  /* A malformed state the API never makes: a leaf that holds a child. */
  BLI_remlink(&ma->paint_layers, stray);
  BLI_addtail(&layer->children, stray);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  ASSERT_EQ(issues.size(), 1);
  EXPECT_EQ(issues[0].code, PaintLayersIssueCode::NonFolderHasChildren);
  EXPECT_TRUE(BLI_uuid_equal(issues[0].layer, layer->marker));
}

TEST_F(PaintLayersDescription, custom_group_setter_keeps_user_counts)
{
  Material *ma = BKE_material_add(bmain, "GroupSetterMat");
  bNodeTree *first = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "GroupSetterFirst"));
  bNodeTree *second = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "GroupSetterSecond"));
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "L", nullptr, PaintLayerPlace::Above);

  /* The setter adds exactly one reference; whether the freshly created ID starts at zero or one is
   * BKE_id_new's business, so the counts are compared relative to that baseline. */
  const int first_base = first->id.us;
  const int second_base = second->id.us;
  EXPECT_TRUE(BKE_paint_layers_set_custom_group(*ma, layer, first));
  EXPECT_EQ(layer->custom_group, first);
  EXPECT_EQ(first->id.us, first_base + 1);

  EXPECT_TRUE(BKE_paint_layers_set_custom_group(*ma, layer, second));
  EXPECT_EQ(layer->custom_group, second);
  EXPECT_EQ(first->id.us, first_base);
  EXPECT_EQ(second->id.us, second_base + 1);

  EXPECT_TRUE(BKE_paint_layers_set_custom_group(*ma, layer, nullptr));
  EXPECT_EQ(layer->custom_group, nullptr);
  EXPECT_EQ(second->id.us, second_base);
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

static MaterialPaintLayer *rna_paint_layers_new(PointerRNA &coll_ptr, int source, const char *name)
{
  FunctionRNA *func = RNA_struct_find_function(coll_ptr.type, "new");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &coll_ptr, func);
  int source_arg = source;
  const char *name_arg = name;
  RNA_parameter_set_lookup(&parms, "source", &source_arg);
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

  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);
  EXPECT_EQ(std::string(RNA_struct_identifier(coll_ptr.type)), "MaterialPaintLayers");

  const StructRNA *item_type = RNA_struct_find("MaterialPaintLayer");
  ASSERT_NE(item_type, nullptr);
  EXPECT_EQ(std::string(RNA_struct_identifier(item_type)), "MaterialPaintLayer");

  /* Every property the task requires exists with the expected RNA type and editability. */
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
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

  /* The property is declared editable at the struct level (a correction's source is a plain
   * setting), but #rna_MaterialPaintLayer_source_editable reports it as not editable at runtime
   * for a stack Layer -- changed only by conversion, through #MaterialPaintLayer.source_change()
   * -- so a UI widget greys out rather than silently doing nothing. */
  PropertyRNA *layer_source = RNA_struct_find_property(&dummy, "source");
  ASSERT_NE(layer_source, nullptr);
  EXPECT_EQ(RNA_property_type(layer_source), PROP_ENUM);
  EXPECT_NE(RNA_property_flag(layer_source) & PROP_EDITABLE, 0);
  /* GUARD (RED-verified 2026-09-24, rna_material.cc:
   * rna_MaterialPaintLayer_source_editable's `role(*layer) == Layer` check): temporarily making
   * the function always return PROP_EDITABLE made this EXPECT_FALSE fail (turned true). */
  EXPECT_FALSE(RNA_property_editable(&dummy, layer_source));
  EXPECT_EQ(RNA_property_enum_get(&dummy, layer_source), MA_PAINT_LAYER_SOURCE_IMAGE);
  RNA_property_enum_set(&dummy, layer_source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(RNA_property_enum_get(&dummy, layer_source), MA_PAINT_LAYER_SOURCE_IMAGE);

  /* A correction's source is editable both statically and at runtime. */
  MaterialPaintLayer *correction_for_editable = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "SourceEditableC");
  ASSERT_NE(correction_for_editable, nullptr);
  PointerRNA correction_dummy = RNA_pointer_create_with_parent(
      coll_ptr, RNA_MaterialPaintLayer, correction_for_editable);
  EXPECT_TRUE(RNA_property_editable(&correction_dummy, layer_source));
  RNA_property_enum_set(&correction_dummy, layer_source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(RNA_property_enum_get(&correction_dummy, layer_source),
            MA_PAINT_LAYER_SOURCE_CONSTANT);

  PropertyRNA *layer_opacity = RNA_struct_find_property(&dummy, "opacity");
  ASSERT_NE(layer_opacity, nullptr);
  EXPECT_EQ(RNA_property_type(layer_opacity), PROP_FLOAT);
  /* Opacity is exposed as a percentage, so the default reads as 100. */
  EXPECT_FLOAT_EQ(RNA_property_float_get(&dummy, layer_opacity), 100.0f);

  PropertyRNA *layer_enabled = RNA_struct_find_property(&dummy, "enabled");
  ASSERT_NE(layer_enabled, nullptr);
  EXPECT_EQ(RNA_property_type(layer_enabled), PROP_BOOLEAN);
  EXPECT_TRUE(RNA_property_boolean_get(&dummy, layer_enabled));

  PropertyRNA *layer_color = RNA_struct_find_property(&dummy, "color_tag");
  ASSERT_NE(layer_color, nullptr);
  EXPECT_EQ(RNA_property_type(layer_color), PROP_INT);

  PropertyRNA *active = RNA_struct_find_property(&coll_ptr, "active");
  ASSERT_NE(active, nullptr);
  EXPECT_EQ(RNA_property_type(active), PROP_POINTER);
  EXPECT_EQ(RNA_property_pointer_type(&coll_ptr, active), item_type);
}

TEST_F(PaintLayersDescription, rna_opacity_is_value_only_and_blend_is_structural)
{
  Material *ma = BKE_material_add(bmain, "RnaValueMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "L");
  ASSERT_NE(layer, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);

  PropertyRNA *opacity = RNA_struct_find_property(&layer_ptr, "opacity");
  PropertyRNA *blend = RNA_struct_find_property(&layer_ptr, "blend_type");
  ASSERT_NE(opacity, nullptr);
  ASSERT_NE(blend, nullptr);

  /* Opacity is a group input, exposed as a percentage: 50 (percent) stores 0.5, and animating it
   * must not rebuild the topology. */
  ma->paint_layers_flag = {};
  RNA_property_float_set(&layer_ptr, opacity, 50.0f);
  EXPECT_FLOAT_EQ(layer->opacity, 0.5f);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* Blend is topology: it changes the generated node. */
  RNA_property_enum_set(&layer_ptr, blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_EQ(layer->blend, MA_PAINT_LAYER_BLEND_MULTIPLY);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersDescription, rna_paint_layers_new_find_remove_round_trip)
{
  Material *ma = BKE_material_add(bmain, "RnaRoundTripMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PropertyRNA *prop = RNA_struct_find_property(&ma_ptr, "paint_layers");
  ASSERT_NE(prop, nullptr);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);

  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "L1");
  ASSERT_NE(layer, nullptr);
  EXPECT_STREQ(layer->name, "L1");
  EXPECT_EQ(RNA_property_collection_length(&ma_ptr, prop), 1);

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

TEST_F(PaintLayersDescription, correction_add_sets_role_and_source)
{
  Material *ma = BKE_material_add(bmain, "CorrectionMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  EXPECT_NE(BKE_paint_layers_role(*correction), PaintLayerRole::Layer);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  /* A correction is its own list on the owner, not a folder child, and adding one never turns the
   * owner into a folder. The list it lands in is chosen by its role. */
  EXPECT_EQ(BLI_listbase_count(&layer->mask_stack), 1);
  EXPECT_EQ(BLI_listbase_count(&layer->effects), 0);
  EXPECT_EQ(BLI_listbase_count(&layer->children), 0);
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_FALSE(BKE_paint_layers_is_folder(*layer));

  /* Changing the role moves the row between the two lists. */
  EXPECT_TRUE(
      BKE_paint_layers_role_set(*ma, correction, MA_PAINT_LAYER_ROLE_EFFECT));
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(BLI_listbase_count(&layer->mask_stack), 0);
  EXPECT_EQ(BLI_listbase_count(&layer->effects), 1);

  /* A row that is not a correction refuses the setters: role_set() never turns a Layer into a
   * correction, and correction_source_set() never touches a Layer's source. */
  /* GUARD (RED-verified 2026-09-24, paint_layers.cc: BKE_paint_layers_role_set's first check):
   * removing the `correction == nullptr || role(*correction) == Layer` guard made this call
   * return true instead of false -- see the RED log in the handback report. */
  EXPECT_FALSE(BKE_paint_layers_role_set(*ma, layer, MA_PAINT_LAYER_ROLE_EFFECT));
  /* GUARD (RED-verified 2026-09-24, paint_layers.cc: BKE_paint_layers_correction_source_set's
   * `role(*correction) == Layer` guard): removing it made this call return true instead of
   * false. */
  EXPECT_FALSE(BKE_paint_layers_correction_source_set(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  /* role_set() also refuses to convert a correction back into a stack Layer. */
  /* GUARD (RED-verified 2026-09-24, paint_layers.cc: BKE_paint_layers_role_set's second check,
   * `!ELEM(role, Effect, MaskItem)`): removing it made this call return true (and move the row to
   * the Layer role) instead of false. */
  EXPECT_FALSE(BKE_paint_layers_role_set(*ma, correction, MA_PAINT_LAYER_ROLE_LAYER));
  /* An out-of-range role, and a Material/NodeGroup/Stack source, are both refused. */
  EXPECT_FALSE(BKE_paint_layers_correction_add(
      *ma, layer, 99, MA_PAINT_LAYER_SOURCE_IMAGE, nullptr));
  /* GUARD (RED-verified 2026-09-24, paint_layers.cc: BKE_paint_layers_correction_add's
   * `!ELEM(source, Image, Constant)` half of its validity check): removing it made these three
   * calls return a real correction (non-null) instead of nullptr. */
  EXPECT_FALSE(BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, nullptr));
  EXPECT_FALSE(BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, nullptr));
  EXPECT_FALSE(BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, nullptr));
}

/**
 * GUARD (RED-verified 2026-09-24, paint_layers.cc: BKE_paint_layers_role_set's first check).
 *
 * A role==Layer row is never itself found by #paint_layer_correction_owner through the public
 * API (only #BKE_paint_layers_correction_add links a non-Layer role into an owner's effects/
 * mask_stack list), so #correction_add_sets_role_and_source's plain top-level-layer case cannot
 * tell this explicit role check apart from the owner lookup simply failing to find an unrelated
 * row. This builds the malformed shape by hand -- a role==Layer row manually linked into an
 * owner's effects list -- to isolate the explicit check on its own.
 */
TEST_F(PaintLayersDescription, role_set_refuses_a_role_layer_row_even_if_reachable_via_owner)
{
  Material *ma = BKE_material_add(bmain, "RoleSetMalformedMat");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);

  MaterialPaintLayer *malformed = MEM_new<MaterialPaintLayer>(__func__);
  malformed->marker = BLI_uuid_generate_random();
  malformed->source = MA_PAINT_LAYER_SOURCE_IMAGE;
  malformed->role = MA_PAINT_LAYER_ROLE_LAYER;
  BLI_addtail(&owner->effects, malformed);

  EXPECT_FALSE(BKE_paint_layers_role_set(*ma, malformed, MA_PAINT_LAYER_ROLE_EFFECT));
  EXPECT_EQ(malformed->role, MA_PAINT_LAYER_ROLE_LAYER);
}

/** Call the layer-level RNA function \a name with the given correction arguments. */
static MaterialPaintLayer *rna_paint_layer_correction_add(PointerRNA &layer_ptr,
                                                          int role,
                                                          int source,
                                                          const char *name)
{
  FunctionRNA *func = RNA_struct_find_function(layer_ptr.type, "correction_add");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &layer_ptr, func);
  int role_arg = role;
  int source_arg = source;
  const char *name_arg = name;
  RNA_parameter_set_lookup(&parms, "role", &role_arg);
  RNA_parameter_set_lookup(&parms, "source", &source_arg);
  RNA_parameter_set_lookup(&parms, "name", &name_arg);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  RNA_function_call(nullptr, &reports, &layer_ptr, func, &parms);
  BKE_reports_free(&reports);

  void *ret = nullptr;
  RNA_parameter_get_lookup(&parms, "correction", &ret);
  MaterialPaintLayer *correction = ret != nullptr ? *static_cast<MaterialPaintLayer **>(ret) :
                                                    nullptr;
  RNA_parameter_list_free(&parms);
  return correction;
}

TEST_F(PaintLayersDescription, rna_property_setters_and_correction_add)
{
  Material *ma = BKE_material_add(bmain, "RnaApiMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "L");
  ASSERT_NE(layer, nullptr);
  PointerRNA layer_ptr = RNA_pointer_create_with_parent(
      coll_ptr, RNA_MaterialPaintLayer, layer);

  PropertyRNA *opacity = RNA_struct_find_property(&layer_ptr, "opacity");
  ASSERT_NE(opacity, nullptr);
  RNA_property_float_set(&layer_ptr, opacity, 0.5f);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&layer_ptr, opacity), 0.5f);

  PropertyRNA *enabled = RNA_struct_find_property(&layer_ptr, "enabled");
  ASSERT_NE(enabled, nullptr);
  RNA_property_boolean_set(&layer_ptr, enabled, false);
  EXPECT_FALSE(RNA_property_boolean_get(&layer_ptr, enabled));

  MaterialPaintLayer *correction = rna_paint_layer_correction_add(
      layer_ptr, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "C");
  ASSERT_NE(correction, nullptr);
  EXPECT_NE(BKE_paint_layers_role(*correction), PaintLayerRole::Layer);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
}

TEST_F(PaintLayersDescription, bake_hash_tracks_structure_and_is_valid)
{
  Material *ma = BKE_material_add(bmain, "BakeHash");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*layer);
  bake->size = 64;
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*layer, hash);
  /* Nothing stored yet: no bake is current. */
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* A parameter the bake captures moves the hash, invalidates the stored one, and leaves the row
   * queued for re-bake: the value-only path does not rebuild the tree, so it must mark the bake. */
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*ma));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.5f));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_TRUE(BKE_paint_layers_bake_stale_get(*ma));

  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  BKE_paint_layers_bake_stale_clear(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*ma));

  /* Swapping a channel map (its session_uid) does too. */
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "BakeHashMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* A per-channel blend or opacity override is part of the row's baked result as well. */
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, MA_PAINT_LAYER_BLEND_MULTIPLY));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, 0.25f));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  BKE_paint_layers_bake_clear(*ma, *layer);
  EXPECT_EQ(layer->bake, nullptr);
}

TEST_F(PaintLayersDescription, bake_subscription_sees_pixel_changes)
{
  Material *ma = BKE_material_add(bmain, "BakeSub");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image &image = *BKE_image_add_generated(
      bmain, 4, 4, "BakeSubMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &image));

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*layer);
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];

  BKE_paint_layers_bake_subscribe(*ma, *layer);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));

  /* A pixel edit is invisible to the hash until the subscription is drained. */
  BKE_image_partial_update_mark_full_update(&image);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_TRUE(BKE_paint_layers_bake_stale_get(*ma));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  /* A full update carries no trustworthy rectangle, so the planner re-bakes the whole row. */
  int region[4];
  EXPECT_FALSE(BKE_paint_layers_bake_changed_region(*ma, *layer, region));
}

TEST_F(PaintLayersDescription, bake_substitute_only_when_valid_and_present)
{
  Material *ma = BKE_material_add(bmain, "BakeSubstitute");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  const float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  Image &baked = *BKE_image_add_generated(
      bmain, 8, 8, "BakedMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*layer);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = &baked;
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];

  Image *out = nullptr;
  EXPECT_TRUE(BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out));
  EXPECT_EQ(out, &baked);

  /* An invalid bake is not substituted even though a map is stored. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, 0.25f));
  out = nullptr;
  EXPECT_FALSE(
      BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out));
  EXPECT_EQ(out, nullptr);

  /* A channel with no baked map is not substituted either. */
  BKE_paint_layers_bake_hash(*layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  EXPECT_FALSE(
      BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, &out));
}

TEST_F(PaintLayersDescription, value_edit_regens_only_a_baked_row_or_its_ancestor)
{
  Material *ma = BKE_material_add(bmain, "ValueRegen");
  MaterialPaintLayer *baked = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Baked", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *plain = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Plain", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(baked, nullptr);
  ASSERT_NE(plain, nullptr);
  BKE_paint_layers_bake_ensure(*baked);
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  /* A value edit on the unbaked sibling is not topology: the animation stays free. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, plain, 0.5f));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_TRUE(BKE_paint_layers_bake_stale_get(*ma));

  /* The baked row itself is substituted, so its own value edit must rebuild. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, baked, 0.5f));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersDescription, custom_role_validation_reports_issues)
{
  Material *ma = BKE_material_add(bmain, "CustomRoles");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  bNodeTree *group = static_cast<bNodeTree *>(BKE_id_new(bmain, ID_NT, "CustomRoleGroup"));
  layer->custom_group = group;

  auto set_role = [group](bNodeTreeInterfaceSocket *socket, const char *role) {
    if (socket->properties == nullptr) {
      IDPropertyTemplate val = {};
      socket->properties = IDP_New(IDP_GROUP, &val, "props");
    }
    IDProperty *prop = IDP_GetPropertyTypeFromGroup(
        socket->properties, "pbr_custom_role", IDP_STRING);
    if (prop != nullptr) {
      IDP_AssignString(prop, role);
    }
    else {
      IDP_AddToGroup(socket->properties, IDP_NewString(role, "pbr_custom_role"));
    }
    group->ensure_interface_cache();
  };

  /* A valid contract: BELOW:BASE_COLOR (Color in), UV (Vector in), COLOR:BASE_COLOR (Color out)
   * and COVERAGE (Float out). */
  bNodeTreeInterfaceSocket *below = group->tree_interface.add_socket(
      "Below", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *uv = group->tree_interface.add_socket(
      "UV", "", "NodeSocketVector", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *color = group->tree_interface.add_socket(
      "Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNodeTreeInterfaceSocket *coverage = group->tree_interface.add_socket(
      "Coverage", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  ASSERT_NE(below, nullptr);
  ASSERT_NE(uv, nullptr);
  ASSERT_NE(color, nullptr);
  ASSERT_NE(coverage, nullptr);
  set_role(below, "BELOW:BASE_COLOR");
  set_role(uv, "UV");
  set_role(color, "COLOR:BASE_COLOR");
  set_role(coverage, "COVERAGE");
  group->ensure_interface_cache();

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  EXPECT_TRUE(issues.is_empty());

  Vector<int> channels;
  BKE_paint_layers_custom_channels_get(*layer, channels);
  ASSERT_EQ(channels.size(), 1);
  EXPECT_EQ(channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* An unknown role is reported. */
  set_role(group->tree_interface.add_socket(
               "Bad", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr),
           "NOPE");
  BKE_paint_layers_issues_get(*ma, issues);
  ASSERT_EQ(issues.size(), 1);
  EXPECT_EQ(issues[0].code, PaintLayersIssueCode::CustomUnknownRole);

  /* A role on the wrong direction is reported. */
  set_role(group->tree_interface.add_socket(
               "WrongWay", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr),
           "COLOR:BASE_COLOR");
  BKE_paint_layers_issues_get(*ma, issues);
  ASSERT_EQ(issues.size(), 2);
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const PaintLayersIssue &issue) {
    return issue.code == PaintLayersIssueCode::CustomRoleDirection;
  }));

  /* A socket type that does not match the channel is reported. */
  set_role(group->tree_interface.add_socket(
               "WrongType", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr),
           "COLOR:EMISSION");
  BKE_paint_layers_issues_get(*ma, issues);
  ASSERT_EQ(issues.size(), 3);
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const PaintLayersIssue &issue) {
    return issue.code == PaintLayersIssueCode::CustomSocketType;
  }));

  /* An unknown channel name is reported. */
  set_role(group->tree_interface.add_socket(
               "UnknownChannel", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr),
           "COLOR:NOPE");
  BKE_paint_layers_issues_get(*ma, issues);
  EXPECT_TRUE(std::any_of(issues.begin(), issues.end(), [](const PaintLayersIssue &issue) {
    return issue.code == PaintLayersIssueCode::CustomUnknownChannel;
  }));

  /* The Custom row keeps the rest of the stack building: the issues are non-fatal. */
  EXPECT_FALSE(issues.is_empty());
}

TEST_F(PaintLayersDescription, custom_template_and_channel_add)
{
  Material *ma = BKE_material_add(bmain, "CustomTemplate");
  MaterialPaintLayer *layer = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_NODE_GROUP);
  ASSERT_NE(layer->custom_group, nullptr);
  EXPECT_EQ(layer->custom_group->id.us, 1);
  EXPECT_TRUE(BLI_uuid_equal(BKE_paint_layers_active_get(*ma), layer->marker));

  /* The template declares BASE_COLOR through its COLOR role. */
  Vector<int> channels;
  BKE_paint_layers_custom_channels_get(*layer, channels);
  ASSERT_EQ(channels.size(), 1);
  EXPECT_EQ(channels[0], PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  EXPECT_TRUE(issues.is_empty());

  /* Adding a channel appends the pair. */
  ASSERT_TRUE(BKE_paint_layers_custom_channel_add(
      *bmain, *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  BKE_paint_layers_custom_channels_get(*layer, channels);
  EXPECT_EQ(channels.size(), 2);
  EXPECT_TRUE(channels.contains(PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  /* The same channel cannot be declared twice. */
  EXPECT_FALSE(BKE_paint_layers_custom_channel_add(
      *bmain, *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));

  /* A role-less input becomes a stored user parameter with its socket default. */
  bNodeTreeInterfaceSocket *amount = layer->custom_group->tree_interface.add_socket(
      "Amount", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  ASSERT_NE(amount, nullptr);
  ASSERT_NE(amount->socket_data, nullptr);
  static_cast<bNodeSocketValueFloat *>(amount->socket_data)->value = 0.25f;
  layer->custom_group->ensure_interface_cache();
  BKE_paint_layers_custom_properties_sync(*ma);
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      layer->properties, amount->identifier, IDP_DOUBLE);
  ASSERT_NE(prop, nullptr);
  EXPECT_NEAR(IDP_double_get(prop), 0.25, 1e-6);
}

TEST_F(PaintLayersDescription, custom_bake_hash_tracks_interface)
{
  Material *ma = BKE_material_add(bmain, "CustomHash");
  MaterialPaintLayer *layer = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);

  uint32_t before[2];
  BKE_paint_layers_bake_hash(*layer, before);

  /* Declaring one more channel changes the group interface, so the bake is invalidated. */
  ASSERT_TRUE(BKE_paint_layers_custom_channel_add(
      *bmain, *ma, *layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS));
  uint32_t after[2];
  BKE_paint_layers_bake_hash(*layer, after);
  EXPECT_TRUE(before[0] != after[0] || before[1] != after[1]);

  /* A property value on the layer is part of the key too. */
  if (layer->properties == nullptr) {
    IDPropertyTemplate group_val = {};
    layer->properties = IDP_New(IDP_GROUP, &group_val, "properties");
  }
  IDPropertyTemplate val = {};
  val.i = 7;
  IDP_AddToGroup(layer->properties, IDP_New(IDP_INT, &val, "amount"));
  uint32_t with_prop[2];
  BKE_paint_layers_bake_hash(*layer, with_prop);
  EXPECT_TRUE(with_prop[0] != after[0] || with_prop[1] != after[1]);
}

/** The per-source table is the one place the generator, CPU and bake read their source switches. */
TEST_F(PaintLayersDescription, kind_info_table_matches_the_source_contract)
{
  const PaintLayerKindInfo &folder = BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_STACK);
  EXPECT_TRUE(folder.is_folder);
  EXPECT_FALSE(folder.uses_fill_color);
  EXPECT_FALSE(folder.needs_external_bake);

  EXPECT_TRUE(BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_CONSTANT).uses_fill_color);
  EXPECT_TRUE(BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_MATERIAL).needs_external_bake);
  EXPECT_TRUE(BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_NODE_GROUP).needs_external_bake);
  EXPECT_FALSE(BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_IMAGE).needs_external_bake);
  EXPECT_FALSE(BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_IMAGE).is_folder);
  /* A correction's source is always Image or Constant, never Stack, so it is never a folder --
   * #BKE_paint_layers_is_folder reads #MaterialPaintLayer::source, not a "kind"; there is no
   * separate "Correction" source to probe here any more. */

  /* An unknown source reads back as Image, the DNA enum's compatibility rule. */
  const PaintLayerKindInfo &unknown = BKE_paint_layers_kind_info(99);
  EXPECT_EQ(unknown.source, MA_PAINT_LAYER_SOURCE_IMAGE);
}

/** \} */

/** \name Source and role accessors
 * \{ */

/** #BKE_paint_layers_source_type() and #BKE_paint_layers_role() simply read the stored
 * #MaterialPaintLayer::source/#role fields; there is no derivation step of its own to regress
 * independently of the fields themselves. */
TEST_F(PaintLayersDescription, source_type_and_role_read_the_stored_fields_directly)
{
  Material *ma = BKE_material_add(bmain, "SourceTypeMat");
  MaterialPaintLayer *layer = paint_layer_add(*ma, "L");

  layer->source = MA_PAINT_LAYER_SOURCE_IMAGE;
  EXPECT_EQ(BKE_paint_layers_source_type(*layer), PaintLayerSourceType::Image);
  layer->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
  EXPECT_EQ(BKE_paint_layers_source_type(*layer), PaintLayerSourceType::Constant);
  layer->source = MA_PAINT_LAYER_SOURCE_MATERIAL;
  EXPECT_EQ(BKE_paint_layers_source_type(*layer), PaintLayerSourceType::Material);
  layer->source = MA_PAINT_LAYER_SOURCE_NODE_GROUP;
  EXPECT_EQ(BKE_paint_layers_source_type(*layer), PaintLayerSourceType::NodeGroup);
  layer->source = MA_PAINT_LAYER_SOURCE_STACK;
  EXPECT_EQ(BKE_paint_layers_source_type(*layer), PaintLayerSourceType::Stack);

  layer->role = MA_PAINT_LAYER_ROLE_EFFECT;
  EXPECT_EQ(BKE_paint_layers_role(*layer), PaintLayerRole::Effect);
  layer->role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  EXPECT_EQ(BKE_paint_layers_role(*layer), PaintLayerRole::MaskItem);
  layer->role = MA_PAINT_LAYER_ROLE_LAYER;
  EXPECT_EQ(BKE_paint_layers_role(*layer), PaintLayerRole::Layer);
}

/** A correction's role follows its section; every other row is a stack member. */
TEST_F(PaintLayersDescription, role_splits_effects_and_mask_items_from_stack_members)
{
  Material *ma = BKE_material_add(bmain, "RoleMat");
  MaterialPaintLayer *layer = paint_layer_add(*ma, "L");
  EXPECT_EQ(BKE_paint_layers_role(*layer), PaintLayerRole::Layer);

  MaterialPaintLayer *effect = paint_layer_add_correction(*layer, "E");
  EXPECT_EQ(BKE_paint_layers_role(*effect), PaintLayerRole::Effect);

  MaterialPaintLayer *mask_item = paint_layer_add_mask_item(*layer, "M");
  EXPECT_EQ(BKE_paint_layers_role(*mask_item), PaintLayerRole::MaskItem);
}

/** The two accessors read their own list, bottom to top. */
TEST_F(PaintLayersDescription, effects_and_mask_items_read_their_lists_in_order)
{
  Material *ma = BKE_material_add(bmain, "TwoListsMat");
  MaterialPaintLayer *layer = paint_layer_add(*ma, "L");

  MaterialPaintLayer *e1 = paint_layer_add_correction(*layer, "E1");
  MaterialPaintLayer *e2 = paint_layer_add_correction(*layer, "E2");
  MaterialPaintLayer *m1 = paint_layer_add_mask_item(*layer, "M1");
  MaterialPaintLayer *m2 = paint_layer_add_mask_item(*layer, "M2");

  const Vector<MaterialPaintLayer *> effects = BKE_paint_layers_effects(*layer);
  ASSERT_EQ(effects.size(), 2);
  EXPECT_EQ(effects[0], e1);
  EXPECT_EQ(effects[1], e2);

  const Vector<MaterialPaintLayer *> mask_items = BKE_paint_layers_mask_items(*layer);
  ASSERT_EQ(mask_items.size(), 2);
  EXPECT_EQ(mask_items[0], m1);
  EXPECT_EQ(mask_items[1], m2);

  /* The const overloads report the same rows. */
  const MaterialPaintLayer &const_layer = *layer;
  const Vector<const MaterialPaintLayer *> const_effects = BKE_paint_layers_effects(const_layer);
  ASSERT_EQ(const_effects.size(), 2);
  EXPECT_EQ(const_effects[0], e1);
  EXPECT_EQ(const_effects[1], e2);
  const Vector<const MaterialPaintLayer *> const_mask_items = BKE_paint_layers_mask_items(
      const_layer);
  ASSERT_EQ(const_mask_items.size(), 2);
  EXPECT_EQ(const_mask_items[0], m1);
  EXPECT_EQ(const_mask_items[1], m2);
}

/** The mask is a stack of items; adding one makes it the base (first) element. */
TEST_F(PaintLayersDescription, mask_base_is_the_first_mask_item)
{
  Material *ma = BKE_material_add(bmain, "MaskBaseMat");
  MaterialPaintLayer *layer = paint_layer_add(*ma, "L");
  EXPECT_TRUE(BKE_paint_layers_mask_items(*layer).is_empty());

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, layer, 0.25f);
  ASSERT_NE(item, nullptr);
  ASSERT_FALSE(BKE_paint_layers_mask_items(*layer).is_empty());
  EXPECT_EQ(BKE_paint_layers_mask_items(*layer).first(), item);
  EXPECT_FLOAT_EQ(item->fill_color[0], 0.25f);
  EXPECT_EQ(BKE_paint_layers_role(*item), PaintLayerRole::MaskItem);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source/role stored fields (2.4 final)
 *
 * #MaterialPaintLayer::source and #MaterialPaintLayer::role are the row's only stored kind/place
 * fields now; #kind, #section and #effect are gone. These tests check the *stored* fields
 * directly against literal DNA constants -- not through #BKE_paint_layers_source_type() /
 * #BKE_paint_layers_role(), which now simply return them -- so a regression in a writer cannot
 * hide behind the accessor also being wrong the same way.
 * \{ */

TEST_F(PaintLayersDescription, source_role_stored_by_add_for_every_kind)
{
  Material *ma = BKE_material_add(bmain, "SourceRoleAddMat");

  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  EXPECT_EQ(paint->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(paint->role, MA_PAINT_LAYER_ROLE_LAYER);

  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(fill->role, MA_PAINT_LAYER_ROLE_LAYER);

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(material, nullptr);
  EXPECT_EQ(material->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(material->role, MA_PAINT_LAYER_ROLE_LAYER);

  MaterialPaintLayer *custom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);
  EXPECT_EQ(custom->source, MA_PAINT_LAYER_SOURCE_NODE_GROUP);
  EXPECT_EQ(custom->role, MA_PAINT_LAYER_ROLE_LAYER);

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  EXPECT_EQ(folder->source, MA_PAINT_LAYER_SOURCE_STACK);
  EXPECT_EQ(folder->role, MA_PAINT_LAYER_ROLE_LAYER);
}

TEST_F(PaintLayersDescription, source_role_stored_by_correction_add_for_every_section_and_effect)
{
  Material *ma = BKE_material_add(bmain, "SourceRoleCorrectionMat");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);

  MaterialPaintLayer *content_paint = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "ContentPaint");
  ASSERT_NE(content_paint, nullptr);
  EXPECT_EQ(content_paint->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(content_paint->role, MA_PAINT_LAYER_ROLE_EFFECT);

  MaterialPaintLayer *content_fill = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "ContentFill");
  ASSERT_NE(content_fill, nullptr);
  EXPECT_EQ(content_fill->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(content_fill->role, MA_PAINT_LAYER_ROLE_EFFECT);

  MaterialPaintLayer *mask_paint = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "MaskPaint");
  ASSERT_NE(mask_paint, nullptr);
  EXPECT_EQ(mask_paint->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(mask_paint->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);

  MaterialPaintLayer *mask_fill = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "MaskFill");
  ASSERT_NE(mask_fill, nullptr);
  EXPECT_EQ(mask_fill->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(mask_fill->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
}

TEST_F(PaintLayersDescription, source_role_stored_by_source_change_image_constant_round_trip)
{
  Material *ma = BKE_material_add(bmain, "SourceRoleKindChangeMat");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(layer->role, MA_PAINT_LAYER_ROLE_LAYER);

  ASSERT_TRUE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(layer->role, MA_PAINT_LAYER_ROLE_LAYER);

  ASSERT_TRUE(BKE_paint_layers_source_change(*ma, layer, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(layer->role, MA_PAINT_LAYER_ROLE_LAYER);
}

TEST_F(PaintLayersDescription, source_role_stored_by_correction_set_section_and_set_effect)
{
  Material *ma = BKE_material_add(bmain, "SourceRoleSetSectionMat");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "Correction");
  ASSERT_NE(correction, nullptr);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_EFFECT);

  /* Moving a content correction to the mask section changes its role but not its source: the
   * effect (what it reads from) and the section (where it lives) are independent axes. */
  ASSERT_TRUE(BKE_paint_layers_role_set(*ma, correction, MA_PAINT_LAYER_ROLE_MASK_ITEM));
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);

  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, correction, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
}

TEST_F(PaintLayersDescription, source_role_survives_layer_duplicate)
{
  Material *ma = BKE_material_add(bmain, "SourceRoleDuplicateMat");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  MaterialPaintLayer *mask_item = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask");
  ASSERT_NE(mask_item, nullptr);

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, owner);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->source, MA_PAINT_LAYER_SOURCE_STACK);
  EXPECT_EQ(copy->role, MA_PAINT_LAYER_ROLE_LAYER);
  ASSERT_FALSE(BLI_listbase_is_empty(&copy->mask_stack));
  MaterialPaintLayer *copy_mask_item = static_cast<MaterialPaintLayer *>(copy->mask_stack.first);
  EXPECT_EQ(copy_mask_item->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(copy_mask_item->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
}

TEST_F(PaintLayersDescription, source_role_survives_material_copy)
{
  Material *ma = BKE_material_add(bmain, "SourceRoleMaterialCopyMat");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "Effect");
  ASSERT_NE(effect, nullptr);

  Material *copy_ma = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy_ma, nullptr);
  ASSERT_FALSE(BLI_listbase_is_empty(&copy_ma->paint_layers));
  MaterialPaintLayer *copy_owner = static_cast<MaterialPaintLayer *>(copy_ma->paint_layers.first);
  EXPECT_EQ(copy_owner->source, MA_PAINT_LAYER_SOURCE_MATERIAL);
  EXPECT_EQ(copy_owner->role, MA_PAINT_LAYER_ROLE_LAYER);
  ASSERT_FALSE(BLI_listbase_is_empty(&copy_owner->effects));
  MaterialPaintLayer *copy_effect = static_cast<MaterialPaintLayer *>(copy_owner->effects.first);
  EXPECT_EQ(copy_effect->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(copy_effect->role, MA_PAINT_LAYER_ROLE_EFFECT);
}

static void rna_paint_layer_source_change(PointerRNA &layer_ptr, int source)
{
  FunctionRNA *func = RNA_struct_find_function(layer_ptr.type, "source_change");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &layer_ptr, func);
  int source_arg = source;
  RNA_parameter_set_lookup(&parms, "source", &source_arg);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  RNA_function_call(nullptr, &reports, &layer_ptr, func, &parms);
  BKE_reports_free(&reports);
  RNA_parameter_list_free(&parms);
}

/** The RNA user path (source_change()) writes #source/#role the same way the BKE call does. */
TEST_F(PaintLayersDescription, rna_source_change_syncs_source_and_role)
{
  Material *ma = BKE_material_add(bmain, "RnaSourceChangeSourceRoleMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *layer = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "L");
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_IMAGE);

  PointerRNA layer_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, layer);
  rna_paint_layer_source_change(layer_ptr, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(layer->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(layer->role, MA_PAINT_LAYER_ROLE_LAYER);
}

/** The RNA user path (correction_add() plus the role/source setters) writes #source/#role. */
TEST_F(PaintLayersDescription, rna_correction_add_and_role_source_setters_sync_source_and_role)
{
  Material *ma = BKE_material_add(bmain, "RnaCorrectionSourceRoleMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *owner = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner");
  ASSERT_NE(owner, nullptr);
  PointerRNA owner_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, owner);

  MaterialPaintLayer *correction = rna_paint_layer_correction_add(
      owner_ptr, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "Correction");
  ASSERT_NE(correction, nullptr);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_EFFECT);

  PointerRNA correction_ptr = RNA_pointer_create_with_parent(
      coll_ptr, RNA_MaterialPaintLayer, correction);
  PropertyRNA *role_prop = RNA_struct_find_property(&correction_ptr, "role");
  PropertyRNA *source_prop = RNA_struct_find_property(&correction_ptr, "source");
  ASSERT_NE(role_prop, nullptr);
  ASSERT_NE(source_prop, nullptr);

  RNA_property_enum_set(&correction_ptr, role_prop, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);

  RNA_property_enum_set(&correction_ptr, source_prop, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(correction->source, MA_PAINT_LAYER_SOURCE_CONSTANT);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);

  /* role_set() refuses to move a correction back to Layer through the property too. */
  RNA_property_enum_set(&correction_ptr, role_prop, MA_PAINT_LAYER_ROLE_LAYER);
  EXPECT_EQ(correction->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Deferred bake
 * \{ */

TEST_F(PaintLayersDescription, bake_row_is_deferred_follows_the_active_row_subtree)
{
  Material *ma = BKE_material_add(bmain, "DeferredMat");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *effect = paint_layer_add_correction(*folder, "Effect");
  MaterialPaintLayer *mask_item = paint_layer_add_mask_item(*child, "Mask");
  MaterialPaintLayer *sibling = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Sibling", nullptr, PaintLayerPlace::Above);

  /* The active row itself is deferred. */
  BKE_paint_layers_active_set(*ma, child->marker);
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *child));

  /* An ancestor of the active row is deferred as well. */
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  /* A row nested under the active row -- here a mask item -- is reached by the walk. */
  BKE_paint_layers_active_set(*ma, mask_item->marker);
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *mask_item));
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *child));
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  /* A row outside the active subtree -- a sibling or a correction under the folder -- is not. */
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *sibling));
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *effect));

  /* No active row defers nothing: a nil marker matches no row, so the check is plain false. */
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *child));
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *sibling));
}

TEST_F(PaintLayersDescription, active_set_marks_material_bake_due_only_when_the_row_changes)
{
  Material *ma = BKE_material_add(bmain, "ActiveMaterialDueMat");
  MaterialPaintLayer *first = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "First", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *second = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Second", nullptr, PaintLayerPlace::Above);

  BKE_paint_layers_active_set(*ma, first->marker);
  BKE_paint_layers_material_bake_due_clear(*ma);

  /* Leaving a row is when its deferred bake becomes due, so the marker is raised. */
  BKE_paint_layers_active_set(*ma, second->marker);
  EXPECT_TRUE(BKE_paint_layers_material_bake_due_get(*ma));

  /* Re-selecting the same row moves nothing, so it must not schedule a bake. */
  BKE_paint_layers_material_bake_due_clear(*ma);
  BKE_paint_layers_active_set(*ma, second->marker);
  EXPECT_FALSE(BKE_paint_layers_material_bake_due_get(*ma));
}

TEST_F(PaintLayersDescription, material_bake_due_survives_the_cpu_bake_ensure)
{
  Material *ma = BKE_material_add(bmain, "MaterialDueSurvivesMat");
  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *other = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Other", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(material, nullptr);
  BKE_paint_layers_bake_ensure(*material)->mode = MA_PAINT_LAYER_BAKE_ALWAYS;

  /* Move off the Material row, which raises the editor's due mark. */
  BKE_paint_layers_active_set(*ma, material->marker);
  BKE_paint_layers_material_bake_due_clear(*ma);
  BKE_paint_layers_bake_stale_clear(*ma);
  BKE_paint_layers_active_set(*ma, other->marker);
  EXPECT_TRUE(BKE_paint_layers_material_bake_due_get(*ma));

  /* The K-1 CPU pass runs before the editor update and clears its own BAKE_STALE once no CPU row
   * is pending. The Material-row signal must not be riding on that flag, or it would be gone by
   * the time the editor planner reads it. */
  ma->paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  BKE_paint_layers_bake_ensure(*bmain, *ma);
  EXPECT_FALSE(BKE_paint_layers_bake_stale_get(*ma));
  EXPECT_TRUE(BKE_paint_layers_material_bake_due_get(*ma));

  /* The editor planner clears the mark once it has run. */
  BKE_paint_layers_material_bake_due_clear(*ma);
  EXPECT_FALSE(BKE_paint_layers_material_bake_due_get(*ma));
}

/** A source material whose Principled is linked to its output, so the resolver can read it. */
static Material *paint_layer_source_material(Main &bmain, const char *name)
{
  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  return source;
}

static bNode *source_principled(Material &source)
{
  for (bNode &node : source.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      return &node;
    }
  }
  return nullptr;
}

TEST_F(PaintLayersDescription, material_live_constant_reads_the_active_row_source)
{
  Material *ma = BKE_material_add(bmain, "LiveConstantMat");
  Material *source = paint_layer_source_material(*bmain, "LiveConstantSource");
  bNode *principled = source_principled(*source);
  ASSERT_NE(principled, nullptr);
  /* Roughness stays unlinked: the resolver calls it Constant and answers its socket default. */
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.42f;

  /* Base Color is driven by an Image Texture: that channel resolves as Image, not Constant. */
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 4, 4, "LiveSourceMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  bNodeTree &source_tree = *source->nodetree;
  bNode *texture = bke::node_add_static_node(nullptr, source_tree, SH_NODE_TEX_IMAGE);
  texture->id = &image->id;
  bke::node_add_link(source_tree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  float value[4];
  /* Not active and no baked map for the channel: still answered, so the row's channel set cannot
   * depend on where the focus is. */
  EXPECT_TRUE(
      BKE_paint_layers_material_live_constant(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
  EXPECT_FLOAT_EQ(value[0], 0.42f);

  /* A baked map for another constant channel wins while the row is not active. */
  Image *metallic_map = BKE_image_add_generated(
      bmain, 4, 4, "LiveMetallicMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_TRUE(
      BKE_paint_layers_bake_set_map(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC, metallic_map));
  /* A map is only shown once its bake is valid; without the stamp the row would stay live. */
  BKE_paint_layers_bake_finalize(*ma, *row);
  EXPECT_FALSE(
      BKE_paint_layers_material_live_constant(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC, value));

  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(
      BKE_paint_layers_material_live_constant(*ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
  EXPECT_FLOAT_EQ(value[0], 0.42f);
  /* Active wins even over a baked map. */
  EXPECT_TRUE(
      BKE_paint_layers_material_live_constant(*ma, *row, PAINT_MATERIAL_CHANNEL_METALLIC, value));

  /* An Image channel stays on its baked map; the source of this row is otherwise all constants, so
   * the row is in Hybrid mode and the helper is allowed to answer. */
  EXPECT_FALSE(
      BKE_paint_layers_material_live_constant(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, value));

  /* A row of another kind is refused even while it is the active one. */
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, paint->marker);
  EXPECT_FALSE(
      BKE_paint_layers_material_live_constant(*ma, *paint, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));

  /* A Material row with no source is refused too. */
  MaterialPaintLayer *no_source = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "NoSource", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_active_set(*ma, no_source->marker);
  EXPECT_FALSE(BKE_paint_layers_material_live_constant(
      *ma, *no_source, PAINT_MATERIAL_CHANNEL_ROUGHNESS, value));
}

TEST_F(PaintLayersDescription, material_mode_picks_hybrid_or_source_group)
{
  Material *ma = BKE_material_add(bmain, "ModeMat");
  Material *source = paint_layer_source_material(*bmain, "ModeSource");
  bNode *principled = source_principled(*source);
  ASSERT_NE(principled, nullptr);
  bNodeTree &tree = *source->nodetree;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  /* Every resolvable channel is a constant: the CPU can reproduce the row. */
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  /* A plainly mapped texture keeps Hybrid. */
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 4, 4, "ModeMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  bNode *texture = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  texture->id = &image->id;
  bke::node_add_link(tree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  /* A Mapping on the Vector input cannot be reproduced by the CPU: SourceGroup. */
  bNode *mapping = bke::node_add_static_node(nullptr, tree, SH_NODE_MAPPING);
  bke::node_add_link(tree,
                     *mapping,
                     *bke::node_find_socket(*mapping, SOCK_OUT, "Vector"_ustr),
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_IN, "Vector"_ustr));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  bke::node_remove_socket_links(tree, *bke::node_find_socket(*texture, SOCK_IN, "Vector"_ustr));

  /* A Boolean graph on Base Color is a Baked channel: SourceGroup. */
  bke::node_remove_socket_links(tree, *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bNode *noise = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_NOISE);
  bke::node_add_link(tree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  /* Not active and every channel baked: nothing lives from the source. Alpha has no slot of its
   * own in #MaterialPaintLayerBake::images -- it reads from #coverage instead (see
   * #paint_layer_material_source_map), so it must be set too or the channel would still read as
   * unmapped and keep the row live. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, channel, image));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, image));
  BKE_paint_layers_bake_finalize(*ma, *row);
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
}

TEST_F(PaintLayersDescription, material_mode_is_baked_without_a_principled)
{
  Material *ma = BKE_material_add(bmain, "ModeNoPrincipledMat");
  Material *source = BKE_material_add(bmain, "ModeNoPrincipledSource");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  /* With no Principled there is no live channel at all, so the mode is Baked and the wrapper
   * factory is never asked (its refusal is therefore moot for this source). */
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
}

TEST_F(PaintLayersDescription, material_live_image_requires_a_trivial_flat_texture)
{
  Material *ma = BKE_material_add(bmain, "LiveImageMat");
  Material *source = paint_layer_source_material(*bmain, "LiveImageSource");
  bNode *principled = source_principled(*source);
  ASSERT_NE(principled, nullptr);
  bNodeTree &tree = *source->nodetree;

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 4, 4, "LiveImageMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  bNode *texture = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  texture->id = &image->id;
  bke::node_add_link(tree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  NodeTexImage *storage = static_cast<NodeTexImage *>(texture->storage);
  ASSERT_NE(storage, nullptr);
  storage->projection = SHD_PROJ_FLAT;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  Image *out_image = nullptr;
  const ImageUser *out_iuser = nullptr;
  /* A flat texture with nothing on its Vector input qualifies and hands back the same image. */
  ASSERT_TRUE(BKE_paint_layers_material_live_image(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out_image, &out_iuser));
  EXPECT_EQ(out_image, image);
  EXPECT_EQ(out_iuser, &storage->iuser);

  /* A Mapping on the Vector input disqualifies it. */
  bNode *mapping = bke::node_add_static_node(nullptr, tree, SH_NODE_MAPPING);
  bke::node_add_link(tree,
                     *mapping,
                     *bke::node_find_socket(*mapping, SOCK_OUT, "Vector"_ustr),
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_IN, "Vector"_ustr));
  EXPECT_FALSE(BKE_paint_layers_material_live_image(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out_image, &out_iuser));

  /* A non-flat projection disqualifies too, once the Vector link is gone. */
  bke::node_remove_socket_links(tree, *bke::node_find_socket(*texture, SOCK_IN, "Vector"_ustr));
  storage->projection = SHD_PROJ_BOX;
  EXPECT_FALSE(BKE_paint_layers_material_live_image(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out_image, &out_iuser));
  storage->projection = SHD_PROJ_FLAT;

  /* UDIM: the resolver reports Unavailable, so nothing is answered. */
  image->source = IMA_SRC_TILED;
  EXPECT_FALSE(BKE_paint_layers_material_live_image(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out_image, &out_iuser));
  image->source = IMA_SRC_GENERATED;

  /* Not deferred and a baked map exists for the channel: the map wins. */
  BKE_paint_layers_active_set(*ma, {});
  const float color2[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *baked = BKE_image_add_generated(
      bmain, 4, 4, "LiveImageBake", 32, false, IMA_GENTYPE_BLANK, color2, false, false, false);
  ASSERT_TRUE(
      BKE_paint_layers_bake_set_map(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, baked));
  BKE_paint_layers_bake_finalize(*ma, *row);
  EXPECT_FALSE(BKE_paint_layers_material_live_image(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out_image, &out_iuser));
}

TEST_F(PaintLayersDescription, material_lives_from_source_tracks_any_live_channel)
{
  Material *ma = BKE_material_add(bmain, "LivesFromSourceMat");
  Material *source = paint_layer_source_material(*bmain, "LivesFromSourceSource");
  bNode *principled = source_principled(*source);
  ASSERT_NE(principled, nullptr);
  bNodeTree &tree = *source->nodetree;

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 4, 4, "LivesFromSourceMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  bNode *texture = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  texture->id = &image->id;
  bke::node_add_link(tree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  EXPECT_TRUE(BKE_paint_layers_material_lives_from_source(*ma, *row));

  /* Bake every channel and leave the row: nothing is live any more. Alpha reads from #coverage,
   * not #images[ALPHA] (see #paint_layer_material_source_map), so it must be set too. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, channel, image));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, image));
  BKE_paint_layers_bake_finalize(*ma, *row);
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_FALSE(BKE_paint_layers_material_lives_from_source(*ma, *row));
}

TEST_F(PaintLayersDescription, bake_image_is_deferred_follows_the_active_row_maps)
{
  Material *ma = BKE_material_add(bmain, "DeferredImageMat");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *sibling = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Sibling", nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_bake_ensure(*folder);
  BKE_paint_layers_bake_ensure(*child);
  BKE_paint_layers_bake_ensure(*sibling);

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *folder_map = BKE_image_add_generated(
      bmain, 4, 4, "DeferredFolderMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  Image *child_map = BKE_image_add_generated(
      bmain, 4, 4, "DeferredChildMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  Image *child_coverage = BKE_image_add_generated(
      bmain, 4, 4, "DeferredChildCoverage", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  Image *sibling_map = BKE_image_add_generated(
      bmain, 4, 4, "DeferredSiblingMap", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  Image *orphan = BKE_image_add_generated(
      bmain, 4, 4, "DeferredOrphan", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, folder_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, child_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *child, -1, child_coverage));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *sibling, PAINT_MATERIAL_CHANNEL_BASE_COLOR, sibling_map));

  BKE_paint_layers_active_set(*ma, child->marker);

  /* A channel map of the active row, and its coverage map, stay live. */
  EXPECT_TRUE(BKE_paint_layers_bake_image_is_deferred(*bmain, *child_map));
  EXPECT_TRUE(BKE_paint_layers_bake_image_is_deferred(*bmain, *child_coverage));
  /* A map of an ancestor of the active row stays live too. */
  EXPECT_TRUE(BKE_paint_layers_bake_image_is_deferred(*bmain, *folder_map));
  /* A map of a row outside the active subtree, and an image no row owns, do not. */
  EXPECT_FALSE(BKE_paint_layers_bake_image_is_deferred(*bmain, *sibling_map));
  EXPECT_FALSE(BKE_paint_layers_bake_image_is_deferred(*bmain, *orphan));

  /* No active row means no map is deferred. */
  BKE_paint_layers_active_set(*ma, {});
  EXPECT_FALSE(BKE_paint_layers_bake_image_is_deferred(*bmain, *child_map));
  EXPECT_FALSE(BKE_paint_layers_bake_image_is_deferred(*bmain, *child_coverage));
  EXPECT_FALSE(BKE_paint_layers_bake_image_is_deferred(*bmain, *folder_map));
  EXPECT_FALSE(BKE_paint_layers_bake_image_is_deferred(*bmain, *sibling_map));
}

TEST_F(PaintLayersDescription, source_material_is_live_tracks_the_deferred_material_row)
{
  Material *layered = BKE_material_add(bmain, "LiveSourceOwner");
  Material *source = paint_layer_source_material(*bmain, "LiveSourceMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, row, source));

  /* (a) The source is read live while the Material row is active. */
  BKE_paint_layers_active_set(*layered, row->marker);
  EXPECT_NE(BKE_paint_layers_material_mode(*layered, *row), PaintLayerMaterialMode::Baked);
  EXPECT_TRUE(BKE_paint_layers_source_material_is_live(*bmain, *source));

  /* (b) Moving the active marker to a sibling leaves the source. */
  MaterialPaintLayer *sibling = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_IMAGE, "Sibling", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(sibling, nullptr);
  BKE_paint_layers_active_set(*layered, sibling->marker);
  EXPECT_FALSE(BKE_paint_layers_source_material_is_live(*bmain, *source));

  /* (c) Two layered materials read the source; only one has its row active. */
  Material *other = BKE_material_add(bmain, "OtherLiveSourceOwner");
  MaterialPaintLayer *other_row = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_SOURCE_MATERIAL, "OtherSource", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(other_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*other, other_row, source));
  BKE_paint_layers_active_set(*other, other_row->marker);
  EXPECT_TRUE(BKE_paint_layers_source_material_is_live(*bmain, *source));
  BKE_paint_layers_active_set(*other, {});
  EXPECT_FALSE(BKE_paint_layers_source_material_is_live(*bmain, *source));
}

TEST_F(PaintLayersDescription, source_material_is_live_is_false_in_baked_mode)
{
  Material *layered = BKE_material_add(bmain, "BakedSourceOwner");
  /* No Principled on the source: the row falls back to its baked maps. */
  Material *source = BKE_material_add(bmain, "BakedSourceNoPrincipled");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *layered, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*layered, row, source));
  BKE_paint_layers_active_set(*layered, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*layered, *row), PaintLayerMaterialMode::Baked);
  EXPECT_FALSE(BKE_paint_layers_source_material_is_live(*bmain, *source));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh map rows
 * \{ */

TEST_F(PaintLayersDescription, mesh_map_row_kind_and_type)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(BKE_paint_layers_source_type(*row), PaintLayerSourceType::MeshMap);
  EXPECT_FALSE(BKE_paint_layers_is_folder(*row));

  const PaintLayerKindInfo &info = BKE_paint_layers_kind_info(MA_PAINT_LAYER_SOURCE_MESH_MAP);
  EXPECT_EQ(info.source, MA_PAINT_LAYER_SOURCE_MESH_MAP);
  EXPECT_FALSE(info.is_folder);
  EXPECT_FALSE(info.uses_fill_color);
  EXPECT_FALSE(info.needs_external_bake);

  EXPECT_EQ(row->mesh_map_type, MA_MESH_MAP_AO);
  EXPECT_EQ(BKE_paint_layers_mesh_map_type_get(*row), MA_MESH_MAP_AO);

  ASSERT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, row, MA_MESH_MAP_EDGE));
  EXPECT_EQ(row->mesh_map_type, MA_MESH_MAP_EDGE);
  EXPECT_EQ(BKE_paint_layers_mesh_map_type_get(*row), MA_MESH_MAP_EDGE);
  EXPECT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, row, MA_MESH_MAP_EDGE));
  EXPECT_FALSE(BKE_paint_layers_mesh_map_type_set(*ma, row, MA_MESH_MAP_TYPE_NUM));
  EXPECT_EQ(row->mesh_map_type, MA_MESH_MAP_EDGE);

  BKE_paint_layers_default_channels_apply(*ma, *row);
  /* A MESH_MAP row paints the material's shared atlas: it defaults to Base Color alone, and the
   * user opts into more channels like on a Paint row (spec M2). */
  ASSERT_EQ(row->channels_num, 1);
  EXPECT_EQ(row->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersDescription, mesh_map_type_set_refuses_other_sources)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");
  MaterialPaintLayer *image = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(BKE_paint_layers_mesh_map_type_get(*image), -1);
  EXPECT_FALSE(BKE_paint_layers_mesh_map_type_set(*ma, image, MA_MESH_MAP_EDGE));
  EXPECT_EQ(image->mesh_map_type, MA_MESH_MAP_AO);
}

TEST_F(PaintLayersDescription, mesh_map_corrections_allow_mesh_map_only)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");
  MaterialPaintLayer *owner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(owner, nullptr);

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Effect");
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_MESH_MAP);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT);

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MESH_MAP, "Mask");
  ASSERT_NE(mask, nullptr);
  EXPECT_EQ(mask->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);

  EXPECT_EQ(BKE_paint_layers_correction_add(
                *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "M"),
            nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add(
                *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "N"),
            nullptr);
  EXPECT_EQ(BKE_paint_layers_correction_add(
                *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "S"),
            nullptr);

  ASSERT_TRUE(BKE_paint_layers_correction_source_set(
      *ma, effect, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_TRUE(BKE_paint_layers_correction_source_set(
      *ma, effect, MA_PAINT_LAYER_SOURCE_MESH_MAP));
  EXPECT_FALSE(BKE_paint_layers_correction_source_set(
      *ma, effect, MA_PAINT_LAYER_SOURCE_MATERIAL));
}

TEST_F(PaintLayersDescription, source_change_still_refuses_mesh_map)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");
  MaterialPaintLayer *image = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(image, nullptr);
  /* MESH_MAP is not a conversion target: source_change still only accepts Image and Constant. */
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, image, MA_PAINT_LAYER_SOURCE_MESH_MAP));
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, image, MA_PAINT_LAYER_SOURCE_MATERIAL));
  EXPECT_EQ(image->source, MA_PAINT_LAYER_SOURCE_IMAGE);

  /* A Mesh Map row is a different kind of source: it is not converted to Image/Constant. */
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, mesh_map, MA_MESH_MAP_EDGE));
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, mesh_map, MA_PAINT_LAYER_SOURCE_IMAGE));
  EXPECT_FALSE(BKE_paint_layers_source_change(*ma, mesh_map, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(mesh_map->source, MA_PAINT_LAYER_SOURCE_MESH_MAP);
  EXPECT_EQ(mesh_map->mesh_map_type, MA_MESH_MAP_EDGE);
}

TEST_F(PaintLayersDescription, rna_mesh_map_row_and_editable_type)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);

  MaterialPaintLayer *row = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_MESH_MAP);

  PointerRNA row_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, row);
  PropertyRNA *prop = RNA_struct_find_property(&row_ptr, "mesh_map_type");
  ASSERT_NE(prop, nullptr);
  EXPECT_EQ(RNA_property_type(prop), PROP_ENUM);
  EXPECT_TRUE(RNA_property_editable(&row_ptr, prop));

  RNA_property_enum_set(&row_ptr, prop, MA_MESH_MAP_EDGE);
  EXPECT_EQ(row->mesh_map_type, MA_MESH_MAP_EDGE);

  MaterialPaintLayer *image = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "Paint");
  ASSERT_NE(image, nullptr);
  PointerRNA image_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, image);
  EXPECT_FALSE(RNA_property_editable(&image_ptr, prop));
  RNA_property_enum_set(&image_ptr, prop, MA_MESH_MAP_EDGE);
  EXPECT_EQ(image->mesh_map_type, MA_MESH_MAP_AO);
}

static MaterialPaintLayer *rna_mesh_map_correction_add(PointerRNA &layer_ptr,
                                                       int role,
                                                       int source,
                                                       const char *name)
{
  FunctionRNA *func = RNA_struct_find_function(layer_ptr.type, "correction_add");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &layer_ptr, func);
  int role_arg = role;
  int source_arg = source;
  const char *name_arg = name;
  RNA_parameter_set_lookup(&parms, "role", &role_arg);
  RNA_parameter_set_lookup(&parms, "source", &source_arg);
  RNA_parameter_set_lookup(&parms, "name", &name_arg);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  RNA_function_call(nullptr, &reports, &layer_ptr, func, &parms);
  BKE_reports_free(&reports);

  void *ret = nullptr;
  RNA_parameter_get_lookup(&parms, "correction", &ret);
  MaterialPaintLayer *correction = ret != nullptr ? *static_cast<MaterialPaintLayer **>(ret) :
                                                    nullptr;
  RNA_parameter_list_free(&parms);
  return correction;
}

TEST_F(PaintLayersDescription, rna_correction_add_accepts_mesh_map)
{
  Material *ma = BKE_material_add(bmain, "MeshMapMat");
  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = paint_layers_collection_ptr(ma_ptr);
  MaterialPaintLayer *owner = rna_paint_layers_new(coll_ptr, MA_PAINT_LAYER_SOURCE_IMAGE, "Owner");
  ASSERT_NE(owner, nullptr);
  PointerRNA owner_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialPaintLayer, owner);

  MaterialPaintLayer *effect = rna_mesh_map_correction_add(
      owner_ptr, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO");
  ASSERT_NE(effect, nullptr);
  EXPECT_EQ(effect->source, MA_PAINT_LAYER_SOURCE_MESH_MAP);

  MaterialPaintLayer *refused = rna_mesh_map_correction_add(
      owner_ptr, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MATERIAL, "M");
  EXPECT_EQ(refused, nullptr);
}

/** Guard: the MESH_MAP branch of #BKE_paint_layers_default_channels_apply — a new row paints the
 * material's shared atlas in Base Color and opts into more like a Paint row; revert it and the row
 * carries no channel. */
TEST_F(PaintLayersDescription, mesh_map_row_gets_base_color_channel)
{
  Material *ma = BKE_material_add(bmain, "MeshMapBaseColor");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->channels_num, 0);

  /* The row-creation paths that apply the defaults (RNA add, the outliner, the shading editor)
   * leave the row painting Base Color alone, enabled. */
  BKE_paint_layers_default_channels_apply(*ma, *row);
  ASSERT_EQ(row->channels_num, 1);
  EXPECT_EQ(row->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(row->channels[0].state, MA_PAINT_LAYER_CHANNEL_ENABLED);

  /* A second apply does not duplicate the record. */
  BKE_paint_layers_default_channels_apply(*ma, *row);
  EXPECT_EQ(row->channels_num, 1);
}

/** Guard: #BKE_mesh_maps_slot_image_set tags the material edited (mesh_maps.cc) — the atlas is a
 * map a MESH_MAP row reads, so re-pointing a slot is a structural edit that rebuilds the tree. */
TEST_F(PaintLayersDescription, mesh_map_slot_image_set_marks_the_material_edited)
{
  Material *ma = BKE_material_add(bmain, "MeshMapSlotTag");
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_SLOTS_STALE);

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *atlas = BKE_image_add_generated(
      bmain, 4, 4, "Atlas", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(atlas, nullptr);

  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  EXPECT_TRUE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN);
  EXPECT_TRUE(ma->paint_layers_flag & MA_PAINT_LAYERS_SLOTS_STALE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name RNA mesh map slots
 * \{ */

static PointerRNA mesh_map_slots_collection_ptr(PointerRNA &ma_ptr)
{
  PropertyRNA *prop = RNA_struct_find_property(&ma_ptr, "mesh_map_slots");
  PointerRNA coll_ptr = PointerRNA_NULL;
  if (prop != nullptr) {
    RNA_property_collection_type_get(&ma_ptr, prop, &coll_ptr);
  }
  return coll_ptr;
}

static MaterialMeshMapSlot *rna_mesh_map_slots_ensure(PointerRNA &coll_ptr, int type)
{
  FunctionRNA *func = RNA_struct_find_function(coll_ptr.type, "ensure");
  BLI_assert(func != nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &coll_ptr, func);
  int type_arg = type;
  RNA_parameter_set_lookup(&parms, "type", &type_arg);

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  RNA_function_call(nullptr, &reports, &coll_ptr, func, &parms);
  BKE_reports_free(&reports);

  void *ret = nullptr;
  RNA_parameter_get_lookup(&parms, "slot", &ret);
  MaterialMeshMapSlot *slot = ret != nullptr ? *static_cast<MaterialMeshMapSlot **>(ret) : nullptr;
  RNA_parameter_list_free(&parms);
  return slot;
}

TEST_F(PaintLayersDescription, rna_slot_image_set_maintains_user_counts)
{
  Material *ma = BKE_material_add(bmain, "MeshMapRna");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *first = BKE_image_add_generated(
      bmain, 8, 8, "RnaFirst", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  Image *second = BKE_image_add_generated(
      bmain, 8, 8, "RnaSecond", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);

  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PointerRNA coll_ptr = mesh_map_slots_collection_ptr(ma_ptr);
  ASSERT_NE(coll_ptr.type, nullptr);
  MaterialMeshMapSlot *slot = rna_mesh_map_slots_ensure(coll_ptr, MA_MESH_MAP_AO);
  ASSERT_NE(slot, nullptr);
  PointerRNA slot_ptr = RNA_pointer_create_with_parent(coll_ptr, RNA_MaterialMeshMapSlot, slot);
  PropertyRNA *image_prop = RNA_struct_find_property(&slot_ptr, "image");
  ASSERT_NE(image_prop, nullptr);

  const int first_base = first->id.us;
  PointerRNA first_ptr = RNA_id_pointer_create(&first->id);
  RNA_property_pointer_set(&slot_ptr, image_prop, first_ptr, nullptr);
  EXPECT_EQ(slot->image, first);
  EXPECT_EQ(first->id.us, first_base + 1);

  /* The same image again does not add a second reference. */
  RNA_property_pointer_set(&slot_ptr, image_prop, first_ptr, nullptr);
  EXPECT_EQ(first->id.us, first_base + 1);

  const int second_base = second->id.us;
  PointerRNA second_ptr = RNA_id_pointer_create(&second->id);
  RNA_property_pointer_set(&slot_ptr, image_prop, second_ptr, nullptr);
  EXPECT_EQ(first->id.us, first_base);
  EXPECT_EQ(second->id.us, second_base + 1);

  /* None releases the reference. */
  RNA_property_pointer_set(&slot_ptr, image_prop, PointerRNA_NULL, nullptr);
  EXPECT_EQ(slot->image, nullptr);
  EXPECT_EQ(second->id.us, second_base);
}

/* -------------------------------------------------------------------- */
/** \name Paint-layers UV map
 * \{ */

TEST_F(PaintLayersDescription, uv_map_name_copies_with_material)
{
  Material *ma = BKE_material_add(bmain, "UvCopy");
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  EXPECT_STREQ(copy->paint_layers_uv_map, "UVMap");
  BKE_id_free(bmain, copy);
}

TEST_F(PaintLayersDescription, uv_map_resolve_all_three_cases)
{
  Mesh *mesh = BKE_mesh_add(bmain, "UvResolveMesh");
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.lookup_or_add_for_write_only_span<float2>("UVMap", bke::AttrDomain::Corner);
  attributes.lookup_or_add_for_write_only_span<float2>("Second", bke::AttrDomain::Corner);
  mesh->uv_maps_active_set("UVMap");

  Material *ma = BKE_material_add(bmain, "UvResolve");
  bool missing = false;

  /* No name set: the active UV map, the behavior before names existed. */
  EXPECT_STREQ(BKE_paint_layers_uv_map_resolve(*mesh, *ma, &missing), "UVMap");
  EXPECT_FALSE(missing);

  /* A name that exists on the mesh. */
  BLI_strncpy(ma->paint_layers_uv_map, "Second", sizeof(ma->paint_layers_uv_map));
  EXPECT_STREQ(BKE_paint_layers_uv_map_resolve(*mesh, *ma, &missing), "Second");
  EXPECT_FALSE(missing);

  /* A name the mesh does not have: null and missing, never a silent substitute. */
  BLI_strncpy(ma->paint_layers_uv_map, "Missing", sizeof(ma->paint_layers_uv_map));
  EXPECT_EQ(BKE_paint_layers_uv_map_resolve(*mesh, *ma, &missing), nullptr);
  EXPECT_TRUE(missing);

  BKE_id_free(bmain, mesh);
}

TEST_F(PaintLayersDescription, rna_uv_map_setter_marks_tree_stale)
{
  Material *ma = BKE_material_add(bmain, "UvRna");
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  PropertyRNA *prop = RNA_struct_find_property(&ma_ptr, "paint_layers_uv_map");
  ASSERT_NE(prop, nullptr);

  RNA_property_string_set(&ma_ptr, prop, "UVMap");
  EXPECT_STREQ(ma->paint_layers_uv_map, "UVMap");

  RNA_property_update_main(bmain, nullptr, &ma_ptr, prop);
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

TEST_F(PaintLayersDescription, rna_uv_map_autofill_fills_from_the_object_active_uv)
{
  Mesh *mesh = BKE_mesh_add(bmain, "UvAutofillMesh");
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.lookup_or_add_for_write_only_span<float2>("UVMap", bke::AttrDomain::Corner);
  mesh->uv_maps_active_set("UVMap");

  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "UvAutofillObject");
  ob->data = &mesh->id;
  id_us_plus(&mesh->id);

  Material *ma = BKE_material_add(bmain, "UvAutofill");
  BKE_object_material_slot_add(bmain, ob);
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBJECT);
  ob->actcol = 1;
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  PointerRNA ma_ptr = RNA_id_pointer_create(&ma->id);
  FunctionRNA *func = RNA_struct_find_function(ma_ptr.type, "paint_layers_uv_map_autofill");
  ASSERT_NE(func, nullptr);
  ParameterList parms;
  RNA_parameter_list_create(&parms, &ma_ptr, func);
  Object *ob_raw = ob;
  RNA_parameter_set_lookup(&parms, "object", &ob_raw);
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);
  RNA_function_call(nullptr, &reports, &ma_ptr, func, &parms);
  BKE_reports_free(&reports);
  RNA_parameter_list_free(&parms);

  EXPECT_STREQ(ma->paint_layers_uv_map, "UVMap");
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
}

/** \} */

/** \} */

}  // namespace blender::bke::tests
