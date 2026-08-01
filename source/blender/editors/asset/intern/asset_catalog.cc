/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "AS_asset_library.hh"

#include "AS_asset_catalog.hh"

#include "BKE_main.hh"

#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utils.hh"

#include "RNA_access.hh"

#include "ED_asset_catalog.hh"
#include "ED_asset_image_library.hh"

#include "WM_api.hh"

#include "DNA_userdef_types.h"

namespace blender::ed::asset {

using namespace blender::asset_system;

bool catalogs_read_only(const AssetLibrary &library)
{
  const asset_system::AssetCatalogService &catalog_service = library.catalog_service();
  return catalog_service.is_read_only();
}

static std::string catalog_name_ensure_unique(AssetCatalogService &catalog_service,
                                              StringRefNull name,
                                              StringRef parent_path)
{
  char unique_name[MAX_NAME] = "";
  BLI_uniquename_cb(
      [&](const StringRef check_name) {
        AssetCatalogPath fullpath = AssetCatalogPath(parent_path) / check_name;
        return catalog_service.find_catalog_by_path(fullpath);
      },
      name.c_str(),
      '.',
      unique_name,
      sizeof(unique_name));

  return unique_name;
}

asset_system::AssetCatalog *catalog_add(AssetLibrary *library,
                                        StringRefNull name,
                                        StringRef parent_path)
{
  asset_system::AssetCatalogService &catalog_service = library->catalog_service();
  if (catalog_service.is_read_only()) {
    return nullptr;
  }

  std::string unique_name = catalog_name_ensure_unique(catalog_service, name, parent_path);
  AssetCatalogPath fullpath = AssetCatalogPath(parent_path) / unique_name;

  catalog_service.undo_push();
  asset_system::AssetCatalog *new_catalog = catalog_service.create_catalog(fullpath);
  if (!new_catalog) {
    return nullptr;
  }
  catalog_service.tag_has_unsaved_changes(new_catalog);

  if (const char *library_root = image_library_editable_root_from_asset_library(*library)) {
    image_library_catalog_directory_ensure(library_root, new_catalog->path.c_str());
  }

  WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, nullptr);
  return new_catalog;
}

void catalog_remove(AssetLibrary *library, const CatalogID &catalog_id)
{
  asset_system::AssetCatalogService &catalog_service = library->catalog_service();
  if (catalog_service.is_read_only()) {
    return;
  }

  /* Capture the catalog path before pruning; used to clean up the on-disk mirror folder below. */
  std::string removed_path;
  if (const AssetCatalog *catalog = catalog_service.find_catalog(catalog_id)) {
    removed_path = catalog->path.c_str();
  }

  catalog_service.undo_push();
  catalog_service.tag_has_unsaved_changes(nullptr);
  catalog_service.prune_catalogs_by_id(catalog_id);

  /* Symmetric to #catalog_add, which creates the mirror folder. Only removes it when empty and
   * inside the library root, so image files moved into the catalog are never destroyed. NOTE:
   * child-catalog folders nested inside are not recursed here (kept minimal); their own removal
   * cleans them up if empty. */
  if (!removed_path.empty()) {
    if (const char *library_root = image_library_editable_root_from_asset_library(*library)) {
      image_library_catalog_directory_remove_if_empty(library_root, removed_path);
    }
  }

  WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, nullptr);
}

void catalog_rename(AssetLibrary *library,
                    const CatalogID catalog_id,
                    const StringRefNull new_name)
{
  asset_system::AssetCatalogService &catalog_service = library->catalog_service();
  if (catalog_service.is_read_only()) {
    return;
  }

  AssetCatalog *catalog = catalog_service.find_catalog(catalog_id);
  if (!catalog) {
    return;
  }

  const AssetCatalogPath new_path = catalog->path.parent() / StringRef(new_name);
  const AssetCatalogPath clean_new_path = new_path.cleanup();

  if (new_path == catalog->path || clean_new_path == catalog->path) {
    /* Nothing changed, so don't bother renaming for nothing. */
    return;
  }

  const AssetCatalogPath old_path = catalog->path;

  /* Rename the on-disk mirror folder before touching the in-memory catalog state so that a failed
   * rename (destination already exists) aborts the operation and leaves both disk and memory
   * consistent.  NOTE: the catalog undo stack is memory-only; pressing Ctrl+Z will revert the
   * in-memory path but will not rename the folder back on disk. */
  if (const char *library_root = image_library_editable_root_from_asset_library(*library)) {
    if (!image_library_catalog_directory_relocate(
            library_root, old_path.c_str(), clean_new_path.c_str()))
    {
      return;
    }
  }

  catalog_service.undo_push();
  catalog_service.tag_has_unsaved_changes(catalog);
  catalog_service.update_catalog_path(catalog_id, clean_new_path);
  WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, nullptr);
}

void catalog_move(AssetLibrary *library,
                  const CatalogID src_catalog_id,
                  const std::optional<CatalogID> dst_parent_catalog_id)
{
  asset_system::AssetCatalogService &catalog_service = library->catalog_service();
  if (catalog_service.is_read_only()) {
    return;
  }

  AssetCatalog *src_catalog = catalog_service.find_catalog(src_catalog_id);
  if (!src_catalog) {
    BLI_assert_unreachable();
    return;
  }
  AssetCatalog *dst_catalog = dst_parent_catalog_id ?
                                  catalog_service.find_catalog(*dst_parent_catalog_id) :
                                  nullptr;
  if (!dst_catalog && dst_parent_catalog_id) {
    BLI_assert_unreachable();
    return;
  }

  std::string unique_name = catalog_name_ensure_unique(
      catalog_service, src_catalog->path.name(), dst_catalog ? dst_catalog->path.c_str() : "");
  /* If a destination catalog was given, construct the path using that. Otherwise, the path is just
   * the name of the catalog to be moved, which means it ends up at the root level. */
  const AssetCatalogPath new_path = dst_catalog ? (dst_catalog->path / unique_name) :
                                                  AssetCatalogPath{unique_name};
  const AssetCatalogPath clean_new_path = new_path.cleanup();

  if (new_path == src_catalog->path || clean_new_path == src_catalog->path) {
    /* Nothing changed, so don't bother renaming for nothing. */
    return;
  }

  const AssetCatalogPath old_path = src_catalog->path;

  /* See comment in catalog_rename: disk rename must succeed before memory state changes. */
  if (const char *library_root = image_library_editable_root_from_asset_library(*library)) {
    if (!image_library_catalog_directory_relocate(
            library_root, old_path.c_str(), clean_new_path.c_str()))
    {
      return;
    }
  }

  catalog_service.undo_push();
  catalog_service.tag_has_unsaved_changes(src_catalog);
  catalog_service.update_catalog_path(src_catalog_id, clean_new_path);
  WM_main_add_notifier(NC_SPACE | ND_SPACE_ASSET_PARAMS, nullptr);
}

void catalogs_save_from_main_path(AssetLibrary *library, const Main *bmain)
{
  asset_system::AssetCatalogService &catalog_service = library->catalog_service();
  if (catalog_service.is_read_only()) {
    return;
  }

  /* Since writing to disk also means loading any on-disk changes, it may be a good idea to store
   * an undo step. */
  catalog_service.undo_push();
  catalog_service.write_to_disk(bmain->filepath);
}

void catalogs_save_from_asset_reference(AssetLibrary &library, const AssetWeakReference &reference)
{
  asset_system::AssetCatalogService &catalog_service = library.catalog_service();
  if (catalog_service.is_read_only()) {
    return;
  }

  char asset_full_path_buffer[1024 + MAX_ID_NAME /*FILE_MAX_LIBEXTRA*/];
  char *file_path = nullptr;
  AS_asset_full_path_explode_from_weak_ref(
      &reference, asset_full_path_buffer, &file_path, nullptr, nullptr);
  if (!file_path) {
    BLI_assert_unreachable();
    return;
  }

  /* Since writing to disk also means loading any on-disk changes, it may be a good idea to store
   * an undo step. */
  catalog_service.undo_push();
  catalog_service.write_to_disk(file_path);
}

void catalogs_set_save_catalogs_when_file_is_saved(const bool should_save)
{
  asset_system::AssetLibrary::save_catalogs_when_file_is_saved = should_save;
}

bool catalogs_get_save_catalogs_when_file_is_saved()
{
  return asset_system::AssetLibrary::save_catalogs_when_file_is_saved;
}

/* --------------------------------------------------------------------------
 * Root catalog name validation helpers.
 * -------------------------------------------------------------------------- */

CatalogNameValidateResult ED_asset_catalog_root_name_sanitize(const StringRef input,
                                                              std::string &r_sanitized)
{
  /* Trim leading and trailing ASCII spaces. */
  std::string s = std::string(input.trim());

  if (s.empty()) {
    return CatalogNameValidateResult::Empty;
  }
  /* Reject reserved path component names. */
  if (s == "." || s == "..") {
    return CatalogNameValidateResult::InvalidChars;
  }
  /* Reject any path separator characters — a root catalog name must be a single component. */
  if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) {
    return CatalogNameValidateResult::InvalidChars;
  }
  /* Reject names that are too long to fit in a catalog's simple_name field. */
  if (s.size() >= MAX_NAME) {
    return CatalogNameValidateResult::TooLong;
  }
  r_sanitized = std::move(s);
  return CatalogNameValidateResult::Ok;
}

bool ED_asset_catalog_root_path_exists(const AssetLibrary &library,
                                       const StringRef sanitized_name)
{
  const AssetCatalogPath path(sanitized_name);
  return library.catalog_service().find_catalog_by_path(path) != nullptr;
}

}  // namespace blender::ed::asset
