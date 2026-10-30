/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "BKE_paint_layers_asset.hh"

#include "BKE_asset.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_import.hh"

#include "MEM_guardedalloc.h"

#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_kind.hh"

namespace blender {

namespace {

/** Custom properties on the Material: what survives an append from an asset library. */
constexpr const char *ASSET_TYPE_PROP = "paint_asset_type";
constexpr const char *ASSET_VERSION_PROP = "paint_asset_version";
/** Version 2 carries the exposed-parameters list; a v1 asset reads as before, without one. */
constexpr int ASSET_VERSION = 2;
/** The instances group: one subgroup per drop, named by its id as a string (see the header). */
constexpr const char *ASSET_INSTANCES_PROP = PAINT_ASSET_INSTANCES_PROP_NAME;
/** The instance member repeating the id of its subgroup's name. */
constexpr const char *ASSET_INSTANCE_ID_PROP = "id";
/** The instance member numbering the same-asset instances of the material, from 1. */
constexpr const char *ASSET_INSTANCE_NUMBER_PROP = "number";
/** The one-instance label the format before instances wrote; read as instance 1. */
constexpr const char *ASSET_SOURCE_PROP = PAINT_ASSET_SOURCE_PROP_NAME;
/** A Smart asset always brings the channels its rows use; this is the one policy both paths use. */
constexpr uint16_t SMART_ASSET_IMPORT_FLAGS = PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS |
                                             PAINT_LAYER_IMPORT_SKIP_CURVE_PATCH;

/** The source-label member naming the material the instance came from. */
constexpr const char *ASSET_SOURCE_NAME_PROP = "name";
/** The source-label member naming the library file, empty for a local one. */
constexpr const char *ASSET_SOURCE_LIBRARY_PROP = "library";
/** The source-label member carrying the asset type of the import. */
constexpr const char *ASSET_SOURCE_TYPE_PROP = "type";
/** The source-label subgroup of the imported rows' markers, as a set of names. */
constexpr const char *ASSET_SOURCE_ROWS_PROP = "rows";

void id_properties_ensure(Material &ma)
{
  if (ma.id.properties == nullptr) {
    ma.id.properties = bke::idprop::create_group("properties").release();
  }
}

/** Whether \a type is one of the smart asset kinds; None and out-of-range values are not. */
bool asset_type_is_valid(const PaintLayerAssetType type)
{
  switch (type) {
    case PaintLayerAssetType::None:
      return false;
    case PaintLayerAssetType::Material:
    case PaintLayerAssetType::Layer:
    case PaintLayerAssetType::Mask:
      return true;
  }
  return false;
}

void id_property_set_int(IDProperty *group, const char *name, const int value)
{
  IDP_ReplaceInGroup(group, bke::idprop::create(name, value).release());
}

void id_property_remove(IDProperty *group, const char *name)
{
  if (group == nullptr) {
    return;
  }
  if (IDProperty *prop = IDP_GetPropertyFromGroup(group, name)) {
    IDP_FreeFromGroup(group, prop);
  }
}

/* -------------------------------------------------------------------- */
/** \name Instance records
 *
 * Reading and writing the instances group. Every read also answers for the one-instance
 * #ASSET_SOURCE_PROP of an older file, as instance 1, but never writes: the record and the reload
 * below are the only writers, and they migrate the old label on their way.
 * \{ */

/** The instances group of \a ma, or null when the material holds none. */
const IDProperty *instances_group_get(const Material &ma)
{
  if (ma.id.properties == nullptr || ma.id.properties->type != IDP_GROUP) {
    return nullptr;
  }
  const IDProperty *group = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_INSTANCES_PROP);
  return (group != nullptr && group->type == IDP_GROUP) ? group : nullptr;
}

IDProperty *instances_group_get(Material &ma)
{
  if (ma.id.properties == nullptr || ma.id.properties->type != IDP_GROUP) {
    return nullptr;
  }
  IDProperty *group = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_INSTANCES_PROP);
  return (group != nullptr && group->type == IDP_GROUP) ? group : nullptr;
}

/** The instances group of \a ma, created when missing. */
IDProperty *instances_group_ensure(Material &ma)
{
  id_properties_ensure(ma);
  if (IDProperty *group = instances_group_get(ma)) {
    return group;
  }
  IDProperty *group = bke::idprop::create_group(ASSET_INSTANCES_PROP).release();
  IDP_ReplaceInGroup(ma.id.properties, group);
  return group;
}

/** The one-instance label of an older file, or null. */
const IDProperty *legacy_source_group_get(const Material &ma)
{
  if (ma.id.properties == nullptr || ma.id.properties->type != IDP_GROUP) {
    return nullptr;
  }
  const IDProperty *group = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_SOURCE_PROP);
  if (group == nullptr || group->type != IDP_GROUP) {
    return nullptr;
  }
  /* The name member is what made the old label valid; keep the same test. */
  const IDProperty *name = IDP_GetPropertyFromGroup(group, ASSET_SOURCE_NAME_PROP);
  return (name != nullptr && name->type == IDP_STRING) ? group : nullptr;
}

/** The id an instance subgroup carries, from its member or its name; 0 when neither says one. */
int instance_group_id(const IDProperty &group)
{
  if (const IDProperty *id = IDP_GetPropertyFromGroup(&group, ASSET_INSTANCE_ID_PROP);
      id != nullptr && id->type == IDP_INT)
  {
    return IDP_int_get(id);
  }
  return atoi(group.name);
}

/**
 * Read the source-label members \a group carries into \a r_info; the id is left alone. False when
 * the group names no source material, which made the old label invalid and keeps it that way.
 */
bool instance_info_source_read(const IDProperty &group, PaintLayerAssetInstanceInfo &r_info)
{
  const IDProperty *name = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_NAME_PROP);
  if (name == nullptr || name->type != IDP_STRING) {
    return false;
  }
  r_info.name = IDP_string_get(name);
  if (const IDProperty *number = IDP_GetPropertyFromGroup(&group, ASSET_INSTANCE_NUMBER_PROP);
      number != nullptr && number->type == IDP_INT)
  {
    r_info.number = IDP_int_get(number);
  }
  if (r_info.number <= 0) {
    /* The old format carries no number; the label still reads as a first drop. */
    r_info.number = 1;
  }
  if (const IDProperty *library = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_LIBRARY_PROP);
      library != nullptr && library->type == IDP_STRING)
  {
    r_info.library = IDP_string_get(library);
  }
  r_info.type = PaintLayerAssetType::Material;
  if (const IDProperty *type = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_TYPE_PROP);
      type != nullptr && type->type == IDP_INT &&
      asset_type_is_valid(PaintLayerAssetType(IDP_int_get(type))))
  {
    r_info.type = PaintLayerAssetType(IDP_int_get(type));
  }
  return true;
}

/**
 * Read \a group as one instance's info. False when the group is not an instance record (no name,
 * no usable id), so a stray member of the group is skipped rather than drawn.
 */
bool instance_info_read(const IDProperty &group, PaintLayerAssetInstanceInfo &r_info)
{
  r_info = PaintLayerAssetInstanceInfo();
  if (!instance_info_source_read(group, r_info)) {
    return false;
  }
  r_info.id = instance_group_id(group);
  return r_info.id > 0;
}

/** The markers of \a group's instance: the top-level rows the drop laid down. */
void instance_rows_read(const IDProperty &group, Set<UUID> &r_markers)
{
  if (const IDProperty *rows = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_ROWS_PROP);
      rows != nullptr && rows->type == IDP_GROUP)
  {
    for (const IDProperty &row : rows->data.group) {
      bUUID parsed;
      if (BLI_uuid_parse_string(&parsed, row.name)) {
        r_markers.add(parsed);
      }
    }
  }
}

/** The next free instance id: one past the highest any record carries. */
int instance_id_next(const IDProperty &instances)
{
  int highest = 0;
  for (const IDProperty &group : instances.data.group) {
    if (group.type != IDP_GROUP) {
      continue;
    }
    highest = std::max(highest, instance_group_id(group));
  }
  return highest + 1;
}

/** The next number among the instances of one asset (name and library), from 1. */
int instance_number_next(const IDProperty &instances,
                         const StringRefNull name,
                         const StringRefNull library)
{
  int highest = 0;
  for (const IDProperty &group : instances.data.group) {
    if (group.type != IDP_GROUP) {
      continue;
    }
    const IDProperty *other_name = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_NAME_PROP);
    if (other_name == nullptr || other_name->type != IDP_STRING ||
        !STREQ(IDP_string_get(other_name), name.c_str()))
    {
      continue;
    }
    const IDProperty *other_library = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_LIBRARY_PROP);
    const char *other_library_str = (other_library != nullptr && other_library->type == IDP_STRING) ?
                                        IDP_string_get(other_library) :
                                        "";
    if (!STREQ(other_library_str, library.c_str())) {
      continue;
    }
    PaintLayerAssetInstanceInfo info;
    if (instance_info_read(group, info)) {
      highest = std::max(highest, info.number);
    }
  }
  return highest + 1;
}

/**
 * Move a one-instance #ASSET_SOURCE_PROP into the instances group, as instance 1, and remove the
 * old label. Called by the writers (a record, a reload), so a read of an old file stays clean and
 * the first write brings the data across.
 */
void legacy_instance_migrate(Material &ma)
{
  const IDProperty *legacy = legacy_source_group_get(ma);
  if (legacy == nullptr || instances_group_get(ma) != nullptr) {
    return;
  }
  PaintLayerAssetInstanceInfo info;
  if (!instance_info_source_read(*legacy, info)) {
    return;
  }
  info.id = 1;
  info.number = 1;
  IDProperty *group = bke::idprop::create_group("1").release();
  id_property_set_int(group, ASSET_INSTANCE_ID_PROP, 1);
  id_property_set_int(group, ASSET_INSTANCE_NUMBER_PROP, 1);
  IDP_ReplaceInGroup(group,
                     bke::idprop::create(ASSET_SOURCE_NAME_PROP, info.name).release());
  IDP_ReplaceInGroup(group,
                     bke::idprop::create(ASSET_SOURCE_LIBRARY_PROP, info.library).release());
  id_property_set_int(group, ASSET_SOURCE_TYPE_PROP, int(info.type));
  if (const IDProperty *rows = IDP_GetPropertyFromGroup(legacy, ASSET_SOURCE_ROWS_PROP)) {
    IDP_ReplaceInGroup(group, IDP_CopyProperty(rows));
  }
  IDP_ReplaceInGroup(instances_group_ensure(ma), group);
  id_property_remove(ma.id.properties, ASSET_SOURCE_PROP);
}

/** Replace \a instance's `rows` with the markers of \a imported_rows, keeping id and number. */
void instance_rows_update(Material &ma,
                          const int instance_id,
                          const Span<const MaterialPaintLayer *> imported_rows)
{
  legacy_instance_migrate(ma);
  IDProperty *instances = instances_group_get(ma);
  if (instances == nullptr) {
    return;
  }
  for (IDProperty &group : instances->data.group) {
    if (group.type != IDP_GROUP || instance_group_id(group) != instance_id) {
      continue;
    }
    IDProperty *rows = bke::idprop::create_group(ASSET_SOURCE_ROWS_PROP).release();
    for (const MaterialPaintLayer *row : imported_rows) {
      if (row == nullptr) {
        continue;
      }
      /* The marker is the one identity the row keeps through undo, save and a reload of the file;
       * formatted as the property's name, a group of them reads back as a set. */
      char marker[UUID_STRING_SIZE];
      BLI_uuid_format(marker, row->marker);
      IDP_ReplaceInGroup(rows, bke::idprop::create(marker, 1).release());
    }
    IDP_ReplaceInGroup(&group, rows);
    return;
  }
}

/** \} */

/** Mirror one property into the asset metadata group, when the ID is already an asset. */
void asset_property_set(AssetMetaData *asset_data, const char *name, const int value)
{
  if (asset_data != nullptr) {
    BKE_asset_metadata_idprop_ensure(asset_data, bke::idprop::create(name, value).release());
  }
}

/** Mirror a whole group into the asset metadata, replacing the copy of an earlier build. */
void asset_property_group_set(AssetMetaData *asset_data, const IDProperty *group)
{
  if (asset_data != nullptr) {
    BKE_asset_metadata_idprop_ensure(asset_data, IDP_CopyProperty(group));
  }
}

/** Visit \a row and everything it owns: corrections, mask items and folder children. */
void paint_layer_asset_walk(const MaterialPaintLayer &row,
                            const FunctionRef<void(const MaterialPaintLayer &)> fn)
{
  fn(row);
  for (const MaterialPaintLayer &effect : row.effects) {
    paint_layer_asset_walk(effect, fn);
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    paint_layer_asset_walk(item, fn);
  }
  for (const MaterialPaintLayer &child : row.children) {
    paint_layer_asset_walk(child, fn);
  }
}

/**
 * Visit every stack row of \a list at any depth, a folder's children included. A row for which \a
 * is_root answers true is handed to \a fn whole -- walking its subtree is the caller's job
 * (#paint_layer_asset_walk) -- and its children are not visited again: a root laid under another
 * root of the same instance would otherwise make the caller act on its items twice.
 */
void paint_layer_stack_walk_list(ListBaseT<MaterialPaintLayer> &list,
                                 const FunctionRef<bool(const MaterialPaintLayer &)> is_root,
                                 const FunctionRef<void(MaterialPaintLayer &)> fn)
{
  for (MaterialPaintLayer &row : list) {
    if (is_root(row)) {
      fn(row);
      continue;
    }
    if (BKE_paint_layers_is_folder(row)) {
      paint_layer_stack_walk_list(row.children, is_root, fn);
    }
  }
}

/** The marker subgroup of \a item's kind-property group, made when it has none. */
IDProperty *item_param_marks_ensure(MaterialPaintLayer &item)
{
  if (item.system_properties == nullptr) {
    item.system_properties = bke::idprop::create_group("kind").release();
  }
  IDProperty *marks = IDP_GetPropertyFromGroup(item.system_properties, ASSET_PARAMS_PROP_NAME);
  if (marks != nullptr && marks->type == IDP_GROUP) {
    return marks;
  }
  marks = bke::idprop::create_group(ASSET_PARAMS_PROP_NAME).release();
  IDP_ReplaceInGroup(item.system_properties, marks);
  return marks;
}

/** Collect every marker saved with \a rows; a reader pointed at one of them keeps its reference. */
void portability_markers_collect(const MaterialPaintLayer &row,
                                 const bool mask_only,
                                 Vector<bUUID> &r_markers)
{
  if (mask_only) {
    for (const MaterialPaintLayer &item : row.mask_stack) {
      r_markers.append(item.marker);
    }
    return;
  }
  r_markers.append(row.marker);
  for (const MaterialPaintLayer &item : row.mask_stack) {
    portability_markers_collect(item, false, r_markers);
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    portability_markers_collect(effect, false, r_markers);
  }
  for (const MaterialPaintLayer &child : row.children) {
    portability_markers_collect(child, false, r_markers);
  }
}

bool portability_marker_saved(const Vector<bUUID> &markers, const bUUID &marker)
{
  for (const bUUID &saved : markers) {
    if (BLI_uuid_equal(saved, marker)) {
      return true;
    }
  }
  return false;
}

/** Account for one map once: a map carried by several records is still one image in the asset. */
void portability_image_note(const Image &image,
                            Set<const Image *> &r_seen,
                            PaintLayerAssetPortability &r_portability)
{
  if (!r_seen.add(&image)) {
    return;
  }
  switch (image.source) {
    case IMA_SRC_FILE:
    case IMA_SRC_TILED:
      /* A UDIM set reads its tiles from files just like a single file image does. */
      if (!BKE_image_has_packedfile(&image)) {
        r_portability.external_image_num++;
      }
      break;
    case IMA_SRC_SEQUENCE:
    case IMA_SRC_MOVIE:
    case IMA_SRC_VIEWER:
      r_portability.movie_image_num++;
      break;
    case IMA_SRC_GENERATED:
      break;
  }
}

void portability_walk(const MaterialPaintLayer &row,
                      const Vector<bUUID> &saved_markers,
                      Set<const Image *> &r_seen_images,
                      PaintLayerAssetPortability &r_portability)
{
  if (row.source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH) {
    r_portability.curve_patch_num++;
  }
  if (row.source == MA_PAINT_LAYER_SOURCE_MATERIAL && row.material != nullptr) {
    r_portability.material_source_num++;
  }
  if ((row.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
    r_portability.object_mask_num++;
  }
  if (paint_layer_kind_reads_anchor_point(row) && !BLI_uuid_is_nil(row.anchor_point_source) &&
      !portability_marker_saved(saved_markers, row.anchor_point_source))
  {
    r_portability.foreign_reader_num++;
  }
  for (const int i : IndexRange(row.channels_num)) {
    if (row.channels[i].image != nullptr) {
      portability_image_note(*row.channels[i].image, r_seen_images, r_portability);
    }
  }

  for (const MaterialPaintLayer &item : row.mask_stack) {
    portability_walk(item, saved_markers, r_seen_images, r_portability);
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    portability_walk(effect, saved_markers, r_seen_images, r_portability);
  }
  for (const MaterialPaintLayer &child : row.children) {
    portability_walk(child, saved_markers, r_seen_images, r_portability);
  }
}

void portability_mask_walk(const MaterialPaintLayer &row,
                           const Vector<bUUID> &saved_markers,
                           Set<const Image *> &r_seen_images,
                           PaintLayerAssetPortability &r_portability)
{
  for (const MaterialPaintLayer &item : row.mask_stack) {
    /* Only the mask stack is saved: the item's own content and children stay behind. */
    if (item.source == MA_PAINT_LAYER_SOURCE_CURVE_PATCH) {
      r_portability.curve_patch_num++;
    }
    if (item.source == MA_PAINT_LAYER_SOURCE_MATERIAL && item.material != nullptr) {
      r_portability.material_source_num++;
    }
    if ((item.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
      r_portability.object_mask_num++;
    }
    if (paint_layer_kind_reads_anchor_point(item) && !BLI_uuid_is_nil(item.anchor_point_source) &&
        !portability_marker_saved(saved_markers, item.anchor_point_source))
    {
      r_portability.foreign_reader_num++;
    }
    for (const int i : IndexRange(item.channels_num)) {
      if (item.channels[i].image != nullptr) {
        portability_image_note(*item.channels[i].image, r_seen_images, r_portability);
      }
    }
  }
}

}  // namespace

const char *BKE_paint_layers_asset_type_label(const PaintLayerAssetType type)
{
  switch (type) {
    case PaintLayerAssetType::Mask:
      return "Smart Mask";
    case PaintLayerAssetType::Layer:
      return "Smart Layer";
    case PaintLayerAssetType::Material:
      return "Smart Material";
    case PaintLayerAssetType::None:
      break;
  }
  return "Smart Material";
}

PaintLayerAssetType BKE_paint_layers_asset_type_get(const Material &ma)
{
  const IDProperty *prop = nullptr;
  const IDProperty *version = nullptr;
  if (ma.id.properties != nullptr && ma.id.properties->type == IDP_GROUP) {
    prop = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_TYPE_PROP);
    version = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_VERSION_PROP);
  }
  if (prop == nullptr && ma.id.asset_data != nullptr) {
    prop = BKE_asset_metadata_idprop_find(ma.id.asset_data, ASSET_TYPE_PROP);
    version = BKE_asset_metadata_idprop_find(ma.id.asset_data, ASSET_VERSION_PROP);
  }
  if (prop == nullptr || prop->type != IDP_INT) {
    return PaintLayerAssetType::None;
  }
  if (version != nullptr && version->type == IDP_INT && IDP_int_get(version) > ASSET_VERSION) {
    return PaintLayerAssetType::None;
  }
  const int value = IDP_int_get(prop);
  if (!asset_type_is_valid(PaintLayerAssetType(value))) {
    return PaintLayerAssetType::None;
  }
  return PaintLayerAssetType(value);
}

const char *BKE_paint_layers_asset_default_catalog(const PaintLayerAssetType type)
{
  switch (type) {
    case PaintLayerAssetType::Mask:
      return "Paint/Smart Masks";
    case PaintLayerAssetType::Layer:
      return "Paint/Smart Layers";
    case PaintLayerAssetType::Material:
      return "Paint/Smart Materials";
    default:
      return "";
  }
}

void BKE_paint_layers_asset_type_set(Material &ma, const PaintLayerAssetType type)
{
  id_properties_ensure(ma);
  id_property_set_int(ma.id.properties, ASSET_TYPE_PROP, int(type));
  id_property_set_int(ma.id.properties, ASSET_VERSION_PROP, ASSET_VERSION);

  asset_property_set(ma.id.asset_data, ASSET_TYPE_PROP, int(type));
  asset_property_set(ma.id.asset_data, ASSET_VERSION_PROP, ASSET_VERSION);
}

void BKE_paint_layers_asset_metadata_apply(Material &asset,
                                           const PaintLayerAssetType type,
                                           const StringRef description,
                                           const bUUID &catalog_id,
                                           const StringRef catalog_simple_name)
{
  /* Mirror the tag into the asset metadata now that it exists, so the Asset Browser sees it too. */
  BKE_paint_layers_asset_type_set(asset, type);
  AssetMetaData *meta = asset.id.asset_data;
  if (meta == nullptr) {
    return;
  }
  if (meta->description != nullptr) {
    MEM_delete_void(static_cast<void *>(meta->description));
    meta->description = nullptr;
  }
  const std::string description_text =
      description.is_empty() ? "Smart paint asset saved from a layered material's stack" :
                               std::string(description);
  meta->description = BLI_strdup(description_text.c_str());
  BKE_asset_metadata_tag_ensure(meta, "Paint");
  BKE_asset_metadata_tag_ensure(meta, BKE_paint_layers_asset_type_label(type));
  BKE_asset_metadata_catalog_id_set(
      meta, catalog_id, std::string(catalog_simple_name).c_str());
}

void BKE_paint_layers_asset_type_clear(Material &ma)
{
  id_property_remove(ma.id.properties, ASSET_TYPE_PROP);
  id_property_remove(ma.id.properties, ASSET_VERSION_PROP);
  if (ma.id.asset_data != nullptr) {
    id_property_remove(ma.id.asset_data->properties, ASSET_TYPE_PROP);
    id_property_remove(ma.id.asset_data->properties, ASSET_VERSION_PROP);
  }
}

PaintLayerAssetPortability BKE_paint_layers_asset_portability_check(
    const Material & /*source*/,
    const Span<const MaterialPaintLayer *> rows,
    const PaintLayerAssetType type)
{
  PaintLayerAssetPortability portability;
  Vector<bUUID> saved_markers;
  const bool mask_only = (type == PaintLayerAssetType::Mask);
  for (const MaterialPaintLayer *row : rows) {
    if (row != nullptr) {
      portability_markers_collect(*row, mask_only, saved_markers);
    }
  }

  Set<const Image *> seen_images;
  for (const MaterialPaintLayer *row : rows) {
    if (row == nullptr) {
      continue;
    }
    if (mask_only) {
      portability_mask_walk(*row, saved_markers, seen_images, portability);
    }
    else {
      portability_walk(*row, saved_markers, seen_images, portability);
    }
  }
  return portability;
}

Material *BKE_paint_layers_asset_create(Main &bmain,
                                        Material &source,
                                        const Span<const MaterialPaintLayer *> rows,
                                        const PaintLayerAssetType type,
                                        const char *name,
                                        const PaintLayerImageImportPolicy image_policy,
                                        PaintLayerImportResult *r_result)
{
  if (rows.is_empty() || !asset_type_is_valid(type)) {
    return nullptr;
  }
  if ((type == PaintLayerAssetType::Mask || type == PaintLayerAssetType::Layer) &&
      rows.size() != 1)
  {
    return nullptr;
  }
  for (const MaterialPaintLayer *row : rows) {
    if (row == nullptr || BKE_paint_layers_owner_list(source, *row) == nullptr) {
      return nullptr;
    }
  }
  /* Curve Patch payload is private tool state and its curves live in the source geometry; the
   * core refuses before any ID is created, so a caller cannot leak an asset material. */
  if (BKE_paint_layers_asset_portability_check(source, rows, type).blocking()) {
    return nullptr;
  }
  if (type == PaintLayerAssetType::Mask && BKE_paint_layers_mask_branch_has_anchor(*rows[0])) {
    return nullptr;
  }

  char default_name[MAX_ID_NAME - 2];
  if (name == nullptr || name[0] == '\0') {
    STRNCPY(default_name, BKE_paint_layers_asset_type_label(type));
    name = default_name;
  }

  Material *asset = BKE_material_add(&bmain, name);
  if (asset == nullptr) {
    return nullptr;
  }

  bool ok = false;
  if (type == PaintLayerAssetType::Mask) {
    /* A Material needs a row to own the mask, but the row's own content is not the asset: the
     * import below copies the mask stack and nothing else. The row is the asset's one container. */
    MaterialPaintLayer *holder = BKE_paint_layers_add(
        *asset, rows[0]->name, nullptr, PaintLayerPlace::Above);
    if (holder != nullptr) {
      /* The mask has nothing to hide on the asset's own preview unless the row under it shows
       * something: a flat white fill makes the preview read as the mask's coverage. It is not
       * imported: a Smart Mask brings #mask_stack alone. */
      holder->source = MA_PAINT_LAYER_SOURCE_CONSTANT;
      if (BKE_paint_layers_channel_add(*asset, holder, PAINT_MATERIAL_CHANNEL_BASE_COLOR) !=
          nullptr)
      {
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        BKE_paint_layers_channel_set_value(
            *asset, holder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, white);
      }
    }
    PaintLayerImportParams params;
    params.mode = PaintLayerImportMode::Mask;
    params.anchor = holder;
    /* Mask mode copies the mask stack only. The skip flag still applies to a Curve Patch correction
     * in that stack; the source-channel flag has no meaning here. */
    params.flags = SMART_ASSET_IMPORT_FLAGS;
    params.image_policy = image_policy;
    /* A Fill correction's map is a texture the user assigned, not painted data: the asset has to
     * carry it whatever the painted-map policy says. */
    params.copy_assigned_maps = true;
    ok = holder != nullptr &&
         BKE_paint_layers_import_rows(bmain, *asset, source, rows, params, r_result);
  }
  else {
    PaintLayerImportParams params;
    params.mode = PaintLayerImportMode::Rows;
    params.place = PaintLayerPlace::Above;
    params.flags = SMART_ASSET_IMPORT_FLAGS;
    params.image_policy = image_policy;
    /* Textures assigned to a Fill row or correction travel with the asset, unlike painted maps,
     * which the policy may strip. */
    params.copy_assigned_maps = true;
    ok = BKE_paint_layers_import_rows(bmain, *asset, source, rows, params, r_result);
  }

  if (!ok) {
    BKE_id_free(&bmain, asset);
    return nullptr;
  }

  /* A source that was itself a smart asset marked its exposed parameters on the way in; the new
   * asset is not an instance of anything, so the labels go before its own list is built. */
  BKE_paint_layers_asset_params_unbind(*asset);
  /* Tag the asset now, so a material that is never marked still reads as a Smart asset. */
  BKE_paint_layers_asset_type_set(*asset, type);
  BKE_paint_layers_asset_params_expose(*asset);
  return asset;
}

/** Whether \a item or anything attached to it is an Anchor Point or a reader. */
static bool paint_layer_has_anchor_point(const MaterialPaintLayer &item)
{
  if (BKE_paint_layers_is_anchor_point(item) || BKE_paint_layers_reads_anchor_point(item)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : item.effects) {
    if (paint_layer_has_anchor_point(effect)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &nested : item.mask_stack) {
    if (paint_layer_has_anchor_point(nested)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_mask_branch_has_anchor(const MaterialPaintLayer &row)
{
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (paint_layer_has_anchor_point(item)) {
      return true;
    }
  }
  return false;
}

const MaterialPaintLayer *BKE_paint_layers_asset_mask_holder_find(const Material &asset)
{
  for (const MaterialPaintLayer &row : asset.paint_layers) {
    if (row.mask_stack.first != nullptr) {
      return &row;
    }
  }
  return nullptr;
}

bool BKE_paint_layers_asset_can_insert(const Material &dst,
                                       const Material &asset,
                                       const MaterialPaintLayer *anchor,
                                       const PaintLayerPlace place,
                                       const char **r_reason)
{
  if (r_reason != nullptr) {
    *r_reason = nullptr;
  }
  const auto fail = [&](const char *reason) {
    if (r_reason != nullptr) {
      *r_reason = reason;
    }
    return false;
  };

  const PaintLayerAssetType type = BKE_paint_layers_asset_type_get(asset);
  if (type == PaintLayerAssetType::None) {
    return fail(TIP_("That material is not a Smart asset"));
  }
  if (anchor != nullptr && BKE_paint_layers_owner_list(dst, *anchor) == nullptr) {
    return fail(TIP_("A Smart asset can only land beside a layer of the stack"));
  }
  if (place == PaintLayerPlace::Into && anchor == nullptr) {
    return fail(TIP_("A layer dropped inside needs a folder to land in"));
  }
  if (place == PaintLayerPlace::Into && !BKE_paint_layers_is_folder(*anchor)) {
    return fail(TIP_("Only a folder can take a layer inside it"));
  }

  if (type == PaintLayerAssetType::Mask) {
    if (anchor == nullptr) {
      return fail(TIP_("A Smart Mask needs a layer to land on"));
    }
    const MaterialPaintLayer *holder = BKE_paint_layers_asset_mask_holder_find(asset);
    if (holder == nullptr) {
      return fail(TIP_("That Smart Mask has no mask to apply"));
    }
    if (BKE_paint_layers_mask_branch_has_anchor(*holder)) {
      return fail(TIP_("That Smart Mask contains Anchor Point rows and cannot be applied"));
    }
    return true;
  }

  if (asset.paint_layers.first == nullptr) {
    return fail(TIP_("That Smart asset holds no layers"));
  }
  return true;
}

bool BKE_paint_layers_asset_insert(Main &bmain,
                                   Material &dst,
                                   Material &asset,
                                   MaterialPaintLayer *anchor,
                                   const PaintLayerPlace place,
                                   PaintLayerImportResult *r_result,
                                   const bool append_mask)
{
  if (!BKE_paint_layers_asset_can_insert(dst, asset, anchor, place)) {
    return false;
  }

  const PaintLayerAssetType type = BKE_paint_layers_asset_type_get(asset);
  Vector<const MaterialPaintLayer *> rows;
  if (type == PaintLayerAssetType::Mask) {
    rows.append(BKE_paint_layers_asset_mask_holder_find(asset));
  }
  else {
    for (const MaterialPaintLayer &row : asset.paint_layers) {
      rows.append(&row);
    }
  }

  PaintLayerImportParams params;
  params.mode = (type == PaintLayerAssetType::Mask) ? PaintLayerImportMode::Mask :
                                                      PaintLayerImportMode::Rows;
  params.anchor = anchor;
  params.place = place;
  params.flags = SMART_ASSET_IMPORT_FLAGS;
  if (type == PaintLayerAssetType::Mask && append_mask) {
    params.flags |= PAINT_LAYER_IMPORT_APPEND_MASK;
  }
  params.image_policy = PaintLayerImageImportPolicy::Share;

  PaintLayerImportResult result;
  if (!BKE_paint_layers_import_rows(bmain, dst, asset, rows.as_span(), params, &result)) {
    return false;
  }
  if (r_result != nullptr) {
    *r_result = result;
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Exposed parameters
 * \{ */

const IDProperty *BKE_paint_layers_asset_params_get(const Material &ma)
{
  const IDProperty *prop = nullptr;
  if (ma.id.properties != nullptr && ma.id.properties->type == IDP_GROUP) {
    prop = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_PARAMS_PROP_NAME);
  }
  if (prop == nullptr && ma.id.asset_data != nullptr) {
    prop = BKE_asset_metadata_idprop_find(ma.id.asset_data, ASSET_PARAMS_PROP_NAME);
  }
  if (prop == nullptr || prop->type != IDP_GROUP) {
    return nullptr;
  }
  return prop;
}

bool BKE_paint_layers_asset_params_has(const Material &ma)
{
  return BKE_paint_layers_asset_params_get(ma) != nullptr;
}

void BKE_paint_layers_asset_params_expose(Material &ma)
{
  id_properties_ensure(ma);
  /* Rebuilt from scratch: an entry whose item lost its name, or whose parameters are all gone,
   * must not linger. */
  IDProperty *params = bke::idprop::create_group(ASSET_PARAMS_PROP_NAME).release();
  const auto collect = [&](const MaterialPaintLayer &item) {
    const PaintLayerKindInfo *kind = paint_layer_kind_of(item);
    if (kind == nullptr || kind->params_num == 0) {
      return;
    }
    /* Two items of one name share an entry: the label keys on the name that survives a copy, and
     * an asset with two same-named items exposes whichever set was collected last. */
    IDProperty *entry = IDP_GetPropertyFromGroup(params, item.name);
    if (entry == nullptr || entry->type != IDP_GROUP) {
      entry = bke::idprop::create_group(item.name).release();
      IDP_ReplaceInGroup(params, entry);
    }
    for (const int i : IndexRange(kind->params_num)) {
      IDP_ReplaceInGroup(entry, bke::idprop::create(kind->params[i].name, i).release());
    }
  };
  for (const MaterialPaintLayer &row : ma.paint_layers) {
    paint_layer_asset_walk(row, collect);
  }
  IDProperty *previous = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_PARAMS_PROP_NAME);
  if (previous != nullptr) {
    IDP_FreeFromGroup(ma.id.properties, previous);
  }
  IDP_ReplaceInGroup(ma.id.properties, params);
  asset_property_group_set(ma.id.asset_data, params);
}

void BKE_paint_layers_asset_params_mark_imported(
    const Material &src, const Span<const MaterialPaintLayer *> items)
{
  const IDProperty *asset_params = BKE_paint_layers_asset_params_get(src);
  if (asset_params == nullptr) {
    return;
  }
  const auto mark = [&](const MaterialPaintLayer &item) {
    const IDProperty *entry = IDP_GetPropertyFromGroup(asset_params, item.name);
    if (entry == nullptr || entry->type != IDP_GROUP) {
      return;
    }
    const PaintLayerKindInfo *kind = paint_layer_kind_of(item);
    if (kind == nullptr) {
      return;
    }
    for (const IDProperty &mark : entry->data.group) {
      /* The mark's value is the position the asset's schema had the parameter at; the copy's own
       * schema has to still name it there, so a list built by another build exposes nothing. */
      if (mark.type != IDP_INT) {
        continue;
      }
      const int position = IDP_int_get(&mark);
      if (position < 0 || position >= kind->params_num ||
          strcmp(kind->params[position].name, mark.name) != 0)
      {
        continue;
      }
      IDP_ReplaceInGroup(item_param_marks_ensure(const_cast<MaterialPaintLayer &>(item)),
                         bke::idprop::create(mark.name, 1).release());
    }
  };
  for (const MaterialPaintLayer *item : items) {
    if (item != nullptr) {
      paint_layer_asset_walk(*item, mark);
    }
  }
}

void BKE_paint_layers_asset_params_unbind(Material &ma, const int instance_id)
{
  /* An id of 0 is never a record's: it names every instance, the asset-create path that must not
   * leave a label of a material that is an asset of its own. */
  const bool unbind_all = (instance_id <= 0);
  Set<UUID> bound_markers;
  if (!unbind_all) {
    const IDProperty *instance = BKE_paint_layers_asset_instance_find(ma, instance_id);
    if (instance == nullptr) {
      /* No such instance: nothing to end, and the other instances stay bound. */
      return;
    }
    instance_rows_read(*instance, bound_markers);
  }
  const auto unbind = [&](const MaterialPaintLayer &row) {
    if (row.system_properties == nullptr) {
      return;
    }
    MaterialPaintLayer &item = const_cast<MaterialPaintLayer &>(row);
    if (IDProperty *marks = IDP_GetPropertyFromGroup(item.system_properties,
                                                     ASSET_PARAMS_PROP_NAME))
    {
      IDP_FreeFromGroup(item.system_properties, marks);
    }
  };
  /* A root is unbound together with everything under it, at any depth: a drop Into a folder lays
   * its rows nested, and the walk hands each root's subtree whole, so a root laid under another
   * root of the same instance is not visited twice. */
  paint_layer_stack_walk_list(
      ma.paint_layers,
      [&](const MaterialPaintLayer &row) {
        return unbind_all || bound_markers.contains(UUID(row.marker));
      },
      [&](MaterialPaintLayer &row) { paint_layer_asset_walk(row, unbind); });

  if (!unbind_all) {
    /* Drop this instance's record, and the group with it when nothing is bound any more. */
    IDProperty *instances = instances_group_get(ma);
    if (instances == nullptr) {
      /* The one-instance format: instance 1 is the old label itself. */
      id_property_remove(ma.id.properties, ASSET_SOURCE_PROP);
      return;
    }
    for (IDProperty &group : instances->data.group) {
      if (group.type == IDP_GROUP && instance_group_id(group) == instance_id) {
        IDP_FreeFromGroup(instances, &group);
        break;
      }
    }
    if (BLI_listbase_is_empty(&instances->data.group)) {
      id_property_remove(ma.id.properties, ASSET_INSTANCES_PROP);
    }
    return;
  }

  /* The whole binding ends: the records go, in either format they may exist in. */
  id_property_remove(ma.id.properties, ASSET_INSTANCES_PROP);
  id_property_remove(ma.id.properties, ASSET_SOURCE_PROP);
}

bool BKE_paint_layers_item_param_is_exposed(const MaterialPaintLayer &item, const char *param_name)
{
  if (item.system_properties == nullptr || item.system_properties->type != IDP_GROUP) {
    return false;
  }
  const IDProperty *marks = IDP_GetPropertyFromGroup(item.system_properties,
                                                     ASSET_PARAMS_PROP_NAME);
  return marks != nullptr && marks->type == IDP_GROUP &&
         IDP_GetPropertyFromGroup(marks, param_name) != nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smart-asset instances
 * \{ */

bool BKE_paint_layers_asset_instances_any(const Material &ma)
{
  if (const IDProperty *instances = instances_group_get(ma)) {
    /* Only records count; an emptied group is no instance. */
    for (const IDProperty &group : instances->data.group) {
      if (group.type == IDP_GROUP) {
        return true;
      }
    }
  }
  return legacy_source_group_get(ma) != nullptr;
}

const IDProperty *BKE_paint_layers_asset_instance_find(const Material &ma, const int instance_id)
{
  if (const IDProperty *instances = instances_group_get(ma)) {
    for (const IDProperty &group : instances->data.group) {
      if (group.type == IDP_GROUP && instance_group_id(group) == instance_id) {
        return &group;
      }
    }
    return nullptr;
  }
  /* The one-instance format reads as instance 1. */
  return (instance_id == 1) ? legacy_source_group_get(ma) : nullptr;
}

void BKE_paint_layers_asset_instances_foreach(
    const Material &ma, const FunctionRef<bool(const PaintLayerAssetInstanceInfo &)> fn)
{
  if (const IDProperty *instances = instances_group_get(ma)) {
    for (const IDProperty &group : instances->data.group) {
      if (group.type != IDP_GROUP) {
        continue;
      }
      PaintLayerAssetInstanceInfo info;
      if (!instance_info_read(group, info)) {
        continue;
      }
      if (!fn(info)) {
        return;
      }
    }
    return;
  }
  if (const IDProperty *legacy = legacy_source_group_get(ma)) {
    PaintLayerAssetInstanceInfo info;
    if (instance_info_source_read(*legacy, info)) {
      info.id = 1;
      info.number = 1;
      fn(info);
    }
  }
}

int BKE_paint_layers_asset_instance_of_marker(const Material &ma, const bUUID &marker)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, marker);
  if (const IDProperty *instances = instances_group_get(ma)) {
    for (const IDProperty &group : instances->data.group) {
      if (group.type != IDP_GROUP) {
        continue;
      }
      if (const IDProperty *rows = IDP_GetPropertyFromGroup(&group, ASSET_SOURCE_ROWS_PROP);
          rows != nullptr && rows->type == IDP_GROUP &&
          IDP_GetPropertyFromGroup(rows, formatted) != nullptr)
      {
        return instance_group_id(group);
      }
    }
    return -1;
  }
  if (const IDProperty *legacy = legacy_source_group_get(ma)) {
    if (const IDProperty *rows = IDP_GetPropertyFromGroup(legacy, ASSET_SOURCE_ROWS_PROP);
        rows != nullptr && rows->type == IDP_GROUP &&
        IDP_GetPropertyFromGroup(rows, formatted) != nullptr)
    {
      return 1;
    }
  }
  return -1;
}

/**
 * The instance of the nearest root at or above \a candidate on the path toward the row named by
 * \a row_marker, or -1 while the path has named no root yet. \a nearest is the nearest root seen
 * above \a candidate; keeping it makes the row's answer the root closest to it, not the top-level
 * one.
 */
static int paint_layer_instance_nearest_root(const Material &ma,
                                             const MaterialPaintLayer &candidate,
                                             const bUUID &row_marker,
                                             const int nearest)
{
  const int own_id = BKE_paint_layers_asset_instance_of_marker(ma, candidate.marker);
  const int nearest_here = (own_id != -1) ? own_id : nearest;
  if (BLI_uuid_equal(candidate.marker, row_marker)) {
    return nearest_here;
  }
  if (BKE_paint_layers_is_folder(candidate)) {
    for (const MaterialPaintLayer &child : candidate.children) {
      /* Markers are unique within a material, so at most one child holds the row. */
      if (BKE_paint_layers_subtree_contains(child, row_marker)) {
        return paint_layer_instance_nearest_root(ma, child, row_marker, nearest_here);
      }
    }
  }
  /* Unreachable for a row the stack holds: the walk only descends toward it. */
  return nearest_here;
}

int BKE_paint_layers_asset_instance_of_row(const Material &ma, const MaterialPaintLayer &row)
{
  if (BKE_paint_layers_owner_list(ma, row) == nullptr) {
    return -1;
  }
  /* The records name roots at any depth -- a drop Into a folder lays its rows nested -- so the
   * row's instance is the one of the nearest ancestor that is a root, the row itself included. A
   * const row has no parent lookup to climb by, so the walk descends toward it from the top
   * instead; a row lifted out of its root passes no root and answers -1. */
  for (const MaterialPaintLayer &top : ma.paint_layers) {
    if (BKE_paint_layers_subtree_contains(top, row.marker)) {
      return paint_layer_instance_nearest_root(ma, top, row.marker, -1);
    }
  }
  return -1;
}

std::string BKE_paint_layers_asset_instance_label(const Material &ma, const int instance_id)
{
  const IDProperty *group = BKE_paint_layers_asset_instance_find(ma, instance_id);
  if (group == nullptr) {
    return "";
  }
  PaintLayerAssetInstanceInfo info;
  if (instances_group_get(ma) != nullptr) {
    if (!instance_info_read(*group, info)) {
      return "";
    }
  }
  else {
    /* The old label carries no id; the instance it reads as is 1. */
    if (!instance_info_source_read(*group, info)) {
      return "";
    }
    info.id = 1;
    info.number = 1;
  }
  return info.label();
}

int BKE_paint_layers_asset_instance_record(Material &ma,
                                           const Material &source,
                                           const PaintLayerAssetType type,
                                           const Span<const MaterialPaintLayer *> imported_rows)
{
  /* A drop is a write: the one-instance label of an older file becomes instance 1 here, so this
   * drop reads as #2 instead of replacing it. */
  legacy_instance_migrate(ma);
  IDProperty *instances = instances_group_ensure(ma);

  const std::string source_name(source.id.name + 2);
  const char *source_library = (source.id.lib != nullptr) ? source.id.lib->filepath : "";
  const int id = instance_id_next(*instances);
  const int number = instance_number_next(*instances, source_name, source_library);

  IDProperty *group = bke::idprop::create_group(std::to_string(id)).release();
  id_property_set_int(group, ASSET_INSTANCE_ID_PROP, id);
  id_property_set_int(group, ASSET_INSTANCE_NUMBER_PROP, number);
  IDP_ReplaceInGroup(group, bke::idprop::create(ASSET_SOURCE_NAME_PROP, source_name).release());
  IDP_ReplaceInGroup(group,
                     bke::idprop::create(ASSET_SOURCE_LIBRARY_PROP, StringRefNull(source_library))
                         .release());
  id_property_set_int(group, ASSET_SOURCE_TYPE_PROP, int(type));
  IDProperty *rows = bke::idprop::create_group(ASSET_SOURCE_ROWS_PROP).release();
  for (const MaterialPaintLayer *row : imported_rows) {
    if (row == nullptr) {
      continue;
    }
    char marker[UUID_STRING_SIZE];
    BLI_uuid_format(marker, row->marker);
    IDP_ReplaceInGroup(rows, bke::idprop::create(marker, 1).release());
  }
  IDP_ReplaceInGroup(group, rows);
  IDP_ReplaceInGroup(instances, group);
  return id;
}

PaintLayerAssetReloadResult BKE_paint_layers_asset_reload(Main &bmain,
                                                          Material &ma,
                                                          const int instance_id)
{
  const IDProperty *instance = BKE_paint_layers_asset_instance_find(ma, instance_id);
  if (instance == nullptr) {
    return PaintLayerAssetReloadResult::NoSource;
  }
  const IDProperty *name_prop = IDP_GetPropertyFromGroup(instance, ASSET_SOURCE_NAME_PROP);
  const IDProperty *library_prop = IDP_GetPropertyFromGroup(instance, ASSET_SOURCE_LIBRARY_PROP);
  const char *source_name = IDP_string_get(name_prop);
  const char *source_library = (library_prop != nullptr && library_prop->type == IDP_STRING) ?
                                   IDP_string_get(library_prop) :
                                   "";

  /* The source is the material of that name in that library; a smart asset keeps its name through
   * an append, and a linked one keeps its library, so the pair finds it again after a save. */
  Material *source = nullptr;
  for (Material &candidate : bmain.materials) {
    if (!STREQ(candidate.id.name + 2, source_name)) {
      continue;
    }
    const char *candidate_library = (candidate.id.lib != nullptr) ? candidate.id.lib->filepath : "";
    if (STREQ(candidate_library, source_library)) {
      source = &candidate;
      break;
    }
  }
  if (source == nullptr) {
    return PaintLayerAssetReloadResult::SourceMissing;
  }
  if (source == &ma) {
    return PaintLayerAssetReloadResult::ImportFailed;
  }

  /* The captured values are keyed by the item name the label survives with, in the order the
   * schema reads them, so the re-apply can walk the fresh copies the same way. The kind's
   * parameters are floats in this version; a schema that grows typed parameters extends the
   * record with its type and value. */
  struct CarriedParam {
    std::string name;
    float scalar;
  };
  struct CarriedItem {
    std::string name;
    Vector<CarriedParam> params;
  };
  Vector<CarriedItem> carried;
  Set<UUID> reload_markers;
  instance_rows_read(*instance, reload_markers);
  /* Only this instance's rows are captured: the other instances' values are theirs, and the rows
   * go away with the delete. */
  const auto capture = [&](const MaterialPaintLayer &item) {
    const PaintLayerKindInfo *kind = paint_layer_kind_of(item);
    if (kind == nullptr || kind->params_num == 0) {
      return;
    }
    CarriedItem recorded;
    for (const int i : IndexRange(kind->params_num)) {
      const PaintLayerKindParam &param = kind->params[i];
      if (!BKE_paint_layers_item_param_is_exposed(item, param.name)) {
        continue;
      }
      CarriedParam recorded_param{};
      recorded_param.name = param.name;
      recorded_param.scalar = paint_layer_kind_param_get(item, i);
      recorded.params.append(recorded_param);
    }
    if (!recorded.params.is_empty()) {
      recorded.name = item.name;
      carried.append(std::move(recorded));
    }
  };
  bool found_any = false;
  /* The roots in walk order: the first one names the list the fresh rows seam into. */
  Vector<MaterialPaintLayer *> roots;
  /* Capture first, and only from this instance's rows: the other instances' values are theirs,
   * and the rows go away with the delete. A root is any row the record names, at any depth, and
   * the walk hands its subtree whole, so an item is captured once even when the user nested a
   * root under a root. */
  paint_layer_stack_walk_list(
      ma.paint_layers,
      [&](const MaterialPaintLayer &row) { return reload_markers.contains(UUID(row.marker)); },
      [&](MaterialPaintLayer &row) {
        found_any = true;
        roots.append(&row);
        paint_layer_asset_walk(row, capture);
      });
  if (!found_any) {
    return PaintLayerAssetReloadResult::ImportFailed;
  }

  /* The place to lay the fresh rows into: the seam between the block and its surviving
   * neighbours, so a reload does not hoist the rows to the top of the stack. The roots may sit in
   * different lists -- a drop Into a folder lays them nested, a row the user lifted out hangs on
   * the top level -- and the block of the first root's list is what keeps its place; roots in the
   * other lists go away with the delete like any row of theirs. */
  MaterialPaintLayer *anchor = nullptr;
  PaintLayerPlace place = PaintLayerPlace::Above;
  {
    ListBaseT<MaterialPaintLayer> *block_list = BKE_paint_layers_owner_list(ma, *roots[0]);
    if (block_list != nullptr) {
      MaterialPaintLayer *row_below_block = nullptr;
      for (MaterialPaintLayer &row : *block_list) {
        if (reload_markers.contains(UUID(row.marker))) {
          break;
        }
        row_below_block = &row;
      }
      if (row_below_block == nullptr) {
        /* The block sits at the bottom of its list: anchor on the first row above it, if one
         * survives. A row still named by the record is part of the block, whatever its place. */
        bool in_block = false;
        for (MaterialPaintLayer &row : *block_list) {
          if (reload_markers.contains(UUID(row.marker))) {
            in_block = true;
            continue;
          }
          if (in_block) {
            anchor = &row;
            place = PaintLayerPlace::Below;
            break;
          }
        }
      }
      else {
        /* The rows before the first root are not roots of the instance, so the anchor never is
         * one of the rows being replaced. */
        anchor = row_below_block;
        place = PaintLayerPlace::Above;
      }
    }
    if (anchor == nullptr) {
      /* No seam in the list -- the block filled it: lay the fresh rows back into the folder that
       * held them, when that folder survives the delete (a root does not); otherwise the top of
       * the stack. */
      MaterialPaintLayer *parent = BKE_paint_layers_parent(ma, *roots[0]);
      if (parent != nullptr && !reload_markers.contains(UUID(parent->marker))) {
        anchor = parent;
        place = PaintLayerPlace::Into;
      }
    }
  }

  /* Delete, then import fresh: the import refuses a half state, and the carried values are safe
   * on this side of the delete. A root nested under another root of the instance goes away with
   * the outer one, so the lookup after it answers null and the removal is skipped. */
  for (const bUUID &marker : reload_markers) {
    if (MaterialPaintLayer *row = BKE_paint_layers_find(ma, marker)) {
      BKE_paint_layers_remove(ma, row);
    }
  }

  Vector<const MaterialPaintLayer *> source_rows;
  for (const MaterialPaintLayer &row : source->paint_layers) {
    source_rows.append(&row);
  }
  if (source_rows.is_empty()) {
    return PaintLayerAssetReloadResult::ImportFailed;
  }
  /* The same import a drop performs: the asset's channels come along, its Curve Patch effects do
   * not, and its external maps are shared by reference. The instance is not an asset, so the
   * textures a Fill reads are not copied with their pixels the way an asset save copies them. */
  PaintLayerImportParams params;
  params.anchor = anchor;
  params.place = place;
  params.flags = SMART_ASSET_IMPORT_FLAGS;
  params.image_policy = PaintLayerImageImportPolicy::Share;
  PaintLayerImportResult result;
  if (!BKE_paint_layers_import_rows(bmain, ma, *source, source_rows, params, &result)) {
    return PaintLayerAssetReloadResult::ImportFailed;
  }
  /* The fresh rows carry fresh markers: the record has to name them, or the next reload would
   * find none of its rows any more. The id, number and source label stay as they were. */
  instance_rows_update(ma, instance_id, result.imported.as_span());

  /* The values the user changed come back on top of the fresh copies, found by the parameter's
   * name: the schema may have grown since the asset was saved, and a name that no longer exists
   * is skipped, not applied. */
  const auto reapply = [&](const MaterialPaintLayer &walked) {
    MaterialPaintLayer &item = const_cast<MaterialPaintLayer &>(walked);
    const PaintLayerKindInfo *kind = paint_layer_kind_of(item);
    if (kind == nullptr) {
      return;
    }
    for (const CarriedItem &recorded : carried) {
      if (recorded.name != item.name) {
        continue;
      }
      for (const CarriedParam &recorded_param : recorded.params) {
        for (const int i : IndexRange(kind->params_num)) {
          if (strcmp(kind->params[i].name, recorded_param.name.c_str()) != 0) {
            continue;
          }
          paint_layer_kind_param_set(item, i, recorded_param.scalar);
          break;
        }
      }
    }
  };
  for (MaterialPaintLayer *row : result.imported) {
    if (row != nullptr) {
      paint_layer_asset_walk(*row, reapply);
    }
  }

  BKE_paint_layers_tag_edited(ma);
  return PaintLayerAssetReloadResult::Reloaded;
}

/** \} */

}  // namespace blender
