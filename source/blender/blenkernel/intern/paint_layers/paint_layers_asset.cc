/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

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
constexpr const char *ASSET_SOURCE_PROP = "paint_asset_source";
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

void BKE_paint_layers_asset_params_unbind(Material &ma)
{
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
  for (const MaterialPaintLayer &row : ma.paint_layers) {
    paint_layer_asset_walk(row, unbind);
  }
  if (ma.id.properties != nullptr) {
    if (IDProperty *source = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_SOURCE_PROP)) {
      IDP_FreeFromGroup(ma.id.properties, source);
    }
  }
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

const IDProperty *BKE_paint_layers_asset_source_get(const Material &ma)
{
  if (ma.id.properties == nullptr || ma.id.properties->type != IDP_GROUP) {
    return nullptr;
  }
  const IDProperty *prop = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_SOURCE_PROP);
  if (prop == nullptr || prop->type != IDP_GROUP) {
    return nullptr;
  }
  const IDProperty *name = IDP_GetPropertyFromGroup(prop, ASSET_SOURCE_NAME_PROP);
  return (name != nullptr && name->type == IDP_STRING) ? prop : nullptr;
}

void BKE_paint_layers_asset_source_record(Material &ma,
                                          const Material &source,
                                          const PaintLayerAssetType type,
                                          const Span<const MaterialPaintLayer *> imported_rows)
{
  id_properties_ensure(ma);
  IDProperty *label = bke::idprop::create_group(ASSET_SOURCE_PROP).release();
  IDP_ReplaceInGroup(
      label,
      bke::idprop::create(ASSET_SOURCE_NAME_PROP, StringRefNull(source.id.name + 2)).release());
  const Library *library = source.id.lib;
  IDP_ReplaceInGroup(label,
                     bke::idprop::create(ASSET_SOURCE_LIBRARY_PROP,
                                         StringRefNull(library ? library->filepath : ""))
                         .release());
  id_property_set_int(label, ASSET_SOURCE_TYPE_PROP, int(type));
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
  IDP_ReplaceInGroup(label, rows);
  if (IDProperty *previous = IDP_GetPropertyFromGroup(ma.id.properties, ASSET_SOURCE_PROP)) {
    IDP_FreeFromGroup(ma.id.properties, previous);
  }
  IDP_ReplaceInGroup(ma.id.properties, label);
}

PaintLayerAssetReloadResult BKE_paint_layers_asset_reload(Main &bmain, Material &ma)
{
  const IDProperty *label = BKE_paint_layers_asset_source_get(ma);
  if (label == nullptr) {
    return PaintLayerAssetReloadResult::NoSource;
  }
  const IDProperty *name_prop = IDP_GetPropertyFromGroup(label, ASSET_SOURCE_NAME_PROP);
  const IDProperty *library_prop = IDP_GetPropertyFromGroup(label, ASSET_SOURCE_LIBRARY_PROP);
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
  if (const IDProperty *rows = IDP_GetPropertyFromGroup(label, ASSET_SOURCE_ROWS_PROP);
      rows != nullptr && rows->type == IDP_GROUP)
  {
    for (const IDProperty &row : rows->data.group) {
      bUUID parsed;
      if (BLI_uuid_parse_string(&parsed, row.name)) {
        reload_markers.add(parsed);
      }
    }
  }
  /* Capture first: the rows go away with the delete, and the values have to outlive them. */
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
  for (const MaterialPaintLayer &row : ma.paint_layers) {
    if (reload_markers.contains(row.marker)) {
      found_any = true;
      paint_layer_asset_walk(row, capture);
    }
  }
  if (!found_any) {
    return PaintLayerAssetReloadResult::ImportFailed;
  }

  /* Delete, then import fresh: the import refuses a half state, and the carried values are safe
   * on this side of the delete. */
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
  params.flags = SMART_ASSET_IMPORT_FLAGS;
  params.image_policy = PaintLayerImageImportPolicy::Share;
  PaintLayerImportResult result;
  if (!BKE_paint_layers_import_rows(bmain, ma, *source, source_rows, params, &result)) {
    return PaintLayerAssetReloadResult::ImportFailed;
  }
  /* The fresh rows carry fresh markers: the record has to name them, or the next reload would
   * find none of its rows any more. The type is the one the import recorded. */
  const IDProperty *type_prop = IDP_GetPropertyFromGroup(label, ASSET_SOURCE_TYPE_PROP);
  const PaintLayerAssetType recorded_type = PaintLayerAssetType(
      (type_prop != nullptr && type_prop->type == IDP_INT) ?
          IDP_int_get(type_prop) :
          int(PaintLayerAssetType::Material));
  BKE_paint_layers_asset_source_record(
      ma, *source, recorded_type, result.imported.as_span());

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

}  // namespace blender
