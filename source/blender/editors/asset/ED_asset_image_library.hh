/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Catalog indexer and image index for on-disk image asset libraries.
 *
 * \note Until FileList integration is active, files on disk may not appear in every UI surface
 * that uses a different loading path than #ed::asset::list::iterate.
 */

#pragma once

#include "BLI_uuid.h"

struct bContext;

namespace blender::asset_system {
class AssetLibrary;
}

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

}  // namespace blender::ed::asset
