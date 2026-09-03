/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Catalog indexer and image index for on-disk image asset libraries.
 *
 * Image assets are loaded into FileList via #filelist_readjob_image_files_add_items (single
 * library and the All Libraries nested walk). Surfaces that iterate a different path without that
 * call will not see indexed image files.
 */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_uuid.h"

struct bContext;

namespace blender::asset_system {
class AssetLibrary;
class AssetRepresentation;
}  // namespace blender::asset_system

namespace blender::ed::asset {

/** Case-insensitive extension check for supported image formats. */
bool image_library_is_image_filepath(const char *filepath);

/**
 * Catalog path for images directly in library root (no subfolder).
 * Must be a valid single-segment AssetCatalogPath.
 */
constexpr const char *IMAGE_LIBRARY_ROOT_CATALOG_PATH = "Root";

/** JSON index file name in the library root directory. */
constexpr const char *IMAGE_LIBRARY_INDEX_FILENAME = "blender_image_index.json";

/**
 * Recursive scan; create catalogs; write blender_assets.cats.txt and blender_image_index.json.
 * \return Image count, or -1 on I/O error, or 0 if no images.
 */
int image_library_scan_and_index(const char *library_root_path,
                                 asset_system::AssetLibrary *library = nullptr);

/**
 * Compare on-disk images with blender_image_index.json (and catalog file as fallback).
 */
bool image_library_needs_reindex(const char *library_root_path);

/** Called after library registered in U.asset_libraries or when path changes. */
void image_library_on_library_added(const bContext *C, const char *library_root_path);

/**
 * Scan all registered local image libraries at Blender startup.
 * Updates any stale blender_image_index.json files without sending UI notifications,
 * so that the index is current before the first asset list read job runs.
 */
void image_library_on_startup();

/** Invalidate catalog caches and refresh open UI. */
void image_library_notify_catalogs_changed(const bContext *C, const char *library_root_path);

/**
 * Drop cached #PreviewImage entries for all images in the library index so the next
 * #AssetRepresentation::ensure_previewable() reloads thumbnails from disk.
 */
void image_library_invalidate_cached_previews(const char *library_root_path);

using ImageLibraryForeachCallback = bool (*)(void *userdata,
                                             const char *library_root,
                                             const char *relative_image_path,
                                             const char *image_name,
                                             const bUUID &catalog_id);

/**
 * Iterate all image files recorded in \a library_root's JSON index.
 * Resolve catalog_id from folder path + blender_assets.cats.txt when scanning.
 * \return false if callback returned false (early exit).
 */
bool image_library_foreach_image(const char *library_root,
                                 ImageLibraryForeachCallback callback,
                                 void *userdata);

/**
 * Local custom asset library path that supports mirroring catalogs to folders on disk.
 * Returns null if \a library is not such a library.
 */
const char *image_library_editable_root_from_asset_library(
    const asset_system::AssetLibrary &library);

/**
 * On-disk image asset from a local image library (external #ID_IM under library root).
 */
bool image_library_asset_is_movable_on_disk(const asset_system::AssetRepresentation &asset);

/**
 * Create the directory that mirrors \a catalog_path under \a library_root_path.
 */
bool image_library_catalog_directory_ensure(const char *library_root_path, StringRef catalog_path);

/**
 * Rename or move the on-disk folder when a catalog path changes.
 */
bool image_library_catalog_directory_relocate(const char *library_root_path,
                                              StringRef old_catalog_path,
                                              StringRef new_catalog_path);

/**
 * Remove the on-disk folder mirroring \a catalog_path under \a library_root_path, but only when
 * the folder is empty and inside the library root. Non-empty folders (holding moved image files)
 * are left untouched. Symmetric to #image_library_catalog_directory_ensure.
 */
bool image_library_catalog_directory_remove_if_empty(const char *library_root_path,
                                                     StringRef catalog_path);

/**
 * Move an indexed image file into the folder for \a catalog_id and update the JSON index.
 */
bool image_library_assign_image_to_catalog(const char *library_root_path,
                                           asset_system::AssetLibrary &library,
                                           StringRef relative_image_path,
                                           const bUUID &catalog_id);

}  // namespace blender::ed::asset
