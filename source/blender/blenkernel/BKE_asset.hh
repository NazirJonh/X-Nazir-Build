/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BLI_compiler_attrs.h"

#include "DNA_asset_types.h"

namespace blender {

struct AssetLibraryReference;
struct AssetMetaData;
struct AssetTag;
struct BlendDataReader;
struct BlendWriter;
struct ID;
struct IDProperty;
struct PreviewImage;

using PreSaveFn = void (*)(void *asset_ptr, AssetMetaData *asset_data);
using OnMarkAssetFn = void (*)(void *asset_ptr, AssetMetaData *asset_data);
using OnClearAssetDataFn = void (*)(void *asset_ptr, AssetMetaData *asset_data);

struct AssetTypeInfo {
  /**
   * For local assets (assets in the current .blend file), a callback to execute before the file is
   * saved.
   */
  PreSaveFn pre_save_fn;
  OnMarkAssetFn on_mark_asset_fn;
  /**
   * Should be called whenever a local asset gets cleared of its asset data but stays available
   * otherwise, i.e. when an asset data-block is turned back into a normal data-block.
   */
  OnClearAssetDataFn on_clear_asset_fn;
};

AssetMetaData *BKE_asset_metadata_create();
void BKE_asset_metadata_free(AssetMetaData **asset_data);

/**
 * Create a copy of the #AssetMetaData so that it can be assigned to another asset.
 *
 * The caller becomes the owner of the returned pointer.
 */
AssetMetaData *BKE_asset_metadata_copy(const AssetMetaData *source);

struct AssetTagEnsureResult {
  AssetTag *tag;
  /* Set to false if a tag of this name was already present. */
  bool is_new;
};

AssetTag *BKE_asset_metadata_tag_add(AssetMetaData *asset_data, const char *name)
    ATTR_NONNULL(1, 2);
/**
 * Make sure there is a tag with name \a name, create one if needed.
 */
AssetTagEnsureResult BKE_asset_metadata_tag_ensure(AssetMetaData *asset_data, const char *name);
void BKE_asset_metadata_tag_remove(AssetMetaData *asset_data, AssetTag *tag);

/** Clean up the catalog ID (white-spaces removed, length reduced, etc.) and assign it. */
void BKE_asset_metadata_catalog_id_clear(AssetMetaData *asset_data);
void BKE_asset_metadata_catalog_id_set(AssetMetaData *asset_data,
                                       bUUID catalog_id,
                                       const char *catalog_simple_name);

void BKE_asset_library_reference_init_default(AssetLibraryReference *library_ref);

void BKE_asset_metadata_idprop_ensure(AssetMetaData *asset_data, IDProperty *prop);
IDProperty *BKE_asset_metadata_idprop_find(const AssetMetaData *asset_data,
                                           const char *name) ATTR_WARN_UNUSED_RESULT;

PreviewImage *BKE_asset_metadata_preview_get_from_id(const AssetMetaData *asset_data,
                                                     const ID *owner_id);

void BKE_asset_metadata_write(BlendWriter *writer, AssetMetaData *asset_data);
void BKE_asset_metadata_read(BlendDataReader *reader, AssetMetaData *asset_data);

void BKE_asset_weak_reference_write(BlendWriter *writer, const AssetWeakReference *weak_ref);
void BKE_asset_weak_reference_read(BlendDataReader *reader, AssetWeakReference *weak_ref);

void BKE_asset_catalog_path_list_free(ListBaseT<AssetCatalogState> &catalog_path_list);
ListBaseT<AssetCatalogState> BKE_asset_catalog_path_list_duplicate(
    const ListBaseT<AssetCatalogState> &catalog_path_list);
void BKE_asset_catalog_path_list_blend_write(
    BlendWriter *writer, const ListBaseT<AssetCatalogState> &catalog_path_list);
void BKE_asset_catalog_path_list_blend_read_data(BlendDataReader *reader,
                                                 ListBaseT<AssetCatalogState> &catalog_path_list);
bool BKE_asset_catalog_path_list_has_path(const ListBaseT<AssetCatalogState> &catalog_path_list,
                                          const char *catalog_path);
void BKE_asset_catalog_path_list_add_path(ListBaseT<AssetCatalogState> &catalog_path_list,
                                          const char *catalog_path);

/**
 * Check if a catalog path is collapsed in the given list.
 * Returns false if the path is not found (default to expanded state).
 */
bool BKE_asset_catalog_path_is_collapsed(const ListBaseT<AssetCatalogState> &catalog_path_list,
                                         const char *catalog_path);

/* Asset catalog state management functions */

/**
 * Set the collapsed state for a catalog path in the given list.
 * Creates a new entry if the path doesn't exist.
 */
void BKE_asset_catalog_state_set_collapsed(ListBaseT<AssetCatalogState> &catalog_state_list,
                                           const char *catalog_path,
                                           bool collapsed);

/**
 * Get the collapsed state for a catalog path from the given list.
 * Returns default_state if the path is not found.
 */
bool BKE_asset_catalog_state_get_collapsed(const ListBaseT<AssetCatalogState> &catalog_state_list,
                                           const char *catalog_path,
                                           bool default_state);

/**
 * Toggle the collapsed state for a catalog path in the given list.
 */
void BKE_asset_catalog_state_toggle_collapsed(ListBaseT<AssetCatalogState> &catalog_state_list,
                                              const char *catalog_path,
                                              bool default_state);

/**
 * Free all catalog state entries in the list.
 */
void BKE_asset_catalog_state_list_free(ListBaseT<AssetCatalogState> &catalog_state_list);

/**
 * Duplicate a catalog state list.
 */
void BKE_asset_catalog_state_list_duplicate(ListBaseT<AssetCatalogState> &dest_list,
                                            const ListBaseT<AssetCatalogState> &src_list);

/**
 * Clean up old catalog state entries based on last used time.
 * Keeps only the most recently used entries up to max_entries.
 */
void BKE_asset_catalog_state_cleanup_old(ListBaseT<AssetCatalogState> &catalog_state_list,
                                         int max_entries);

}  // namespace blender
