/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * UI/Editor level API for catalog operations, creating richer functionality than the asset system
 * catalog API provides (which this uses internally).
 *
 * Functions can be expected to not perform any change when #catalogs_read_only() returns
 * true. Generally UI code should disable such functionality in this case, so these functions are
 * not called at all.
 *
 * Note that `ED_asset_catalog.hh` is part of this API.
 */

#pragma once

#include <optional>
#include <string>

#include "AS_asset_catalog.hh"

#include "BLI_string_ref.hh"

namespace blender {

struct AssetLibraryReference;
struct AssetWeakReference;
struct Main;
struct bUUID;

namespace asset_system {
class AssetLibrary;
}

namespace ed::asset {

void catalogs_save_from_main_path(asset_system::AssetLibrary *library, const Main *bmain);
void catalogs_save_from_asset_reference(asset_system::AssetLibrary &library,
                                        const AssetWeakReference &reference);

/**
 * Saving catalog edits when the file is saved is a global option shared for each asset library,
 * and as such ignores the per asset library #catalogs_read_only().
 */
void catalogs_set_save_catalogs_when_file_is_saved(bool should_save);
bool catalogs_get_save_catalogs_when_file_is_saved();

/**
 * Returns if the catalogs of \a library are allowed to be editable, or if the UI should forbid
 * edits.
 */
[[nodiscard]] bool catalogs_read_only(const asset_system::AssetLibrary &library);

asset_system::AssetCatalog *catalog_add(asset_system::AssetLibrary *library,
                                        StringRefNull name,
                                        StringRef parent_path = nullptr);
void catalog_remove(asset_system::AssetLibrary *library,
                    const asset_system::CatalogID &catalog_id);

void catalog_rename(asset_system::AssetLibrary *library,
                    asset_system::CatalogID catalog_id,
                    StringRefNull new_name);
/**
 * Reinsert catalog identified by \a src_catalog_id as child to catalog identified by \a
 * dst_parent_catalog_id. If \a dst_parent_catalog_id is not set, the catalog is moved to the root
 * level of the tree.
 * The name of the reinserted catalog is made unique within the parent. Note that moving a catalog
 * to the same level it was before will also change its name, since the name uniqueness check isn't
 * smart enough to ignore the item to be reinserted. So the caller is expected to handle this case
 * to avoid unwanted renames.
 *
 * Nothing is done (debug builds run into an assert) if the given catalog IDs can't be identified.
 */
void catalog_move(asset_system::AssetLibrary *library,
                  asset_system::CatalogID src_catalog_id,
                  std::optional<asset_system::CatalogID> dst_parent_catalog_id = std::nullopt);

/* --------------------------------------------------------------------------
 * Root catalog name validation helpers (used by import-mark operator).
 * -------------------------------------------------------------------------- */

enum class CatalogNameValidateResult {
  /** Name is valid and sanitized. */
  Ok,
  /** Input was empty or whitespace-only. */
  Empty,
  /** Name contains path separators (`/` or `\\`) or reserved names (`.`, `..`). */
  InvalidChars,
  /** Name length (after trimming) meets or exceeds #MAX_NAME. */
  TooLong,
};

/**
 * Trim leading/trailing ASCII spaces from \\a input; reject empty, path-separator characters
 * (`/` `\\`), and reserved names (`.` `..`); reject length ≥ MAX_NAME.
 *
 * On success (\\a CatalogNameValidateResult::Ok) \\a r_sanitized receives the trimmed name.
 * On failure \\a r_sanitized is left unchanged.
 */
CatalogNameValidateResult ED_asset_catalog_root_name_sanitize(StringRef input,
                                                              std::string &r_sanitized);

/**
 * Returns true when a root-level catalog whose simple name equals \\a sanitized_name already
 * exists in \\a library (i.e. #AssetCatalogPath(sanitized_name) resolves to a known catalog).
 * \\a sanitized_name must already have been validated via #ED_asset_catalog_root_name_sanitize.
 */
bool ED_asset_catalog_root_path_exists(const asset_system::AssetLibrary &library,
                                       StringRef sanitized_name);

enum class AssetCatalogValidation {
  /** The candidate catalog exists in the (loaded) library. */
  Valid,
  /** The library is loaded and the candidate catalog is confirmed absent. */
  NotFound,
  /** The library hasn't finished loading yet -- neither Valid nor NotFound can be determined. */
  LibraryNotYetLoaded,
};

/**
 * Check whether #candidate_catalog_id exists in #library_ref's catalog tree. Does not touch any
 * consumer's storage -- callers apply the result to their own storage using their own setter.
 * See docs/superpowers/specs/2026-08-08-per-library-catalog-memory-design.md Section D for the
 * full async-loading protocol this is one building block of.
 */
AssetCatalogValidation ED_asset_catalog_validate(const AssetLibraryReference &library_ref,
                                                 bUUID candidate_catalog_id);

}  // namespace ed::asset
}  // namespace blender
