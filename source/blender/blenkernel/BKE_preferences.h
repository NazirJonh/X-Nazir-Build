/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>

#include "BLI_compiler_attrs.h"
#include "BLI_sys_types.h"

namespace blender {

struct AssetCatalogState;
struct AssetLibraryReference;
struct BlendDataReader;
struct BlendWriter;
struct UserDef;
struct bUserAssetBrowserSettings;
struct bUserExtensionRepo;
struct bUserAssetLibrary;
struct bUserAssetShelfSettings;
class StringRef;
struct EnumPropertyItem;

/* -------------------------------------------------------------------- */
/** \name Preferences File
 * \{ */

namespace bke::preferences {

/**
 * Return true if a preferences file exists for this Blender version.
 */
bool exists();

}  // namespace bke::preferences

/** \} */

/* -------------------------------------------------------------------- */
/** \name Assert Libraries
 * \{ */

/** Name of the asset library added by default. Needs translation with `DATA_()` still. */
#define BKE_PREFS_ASSET_LIBRARY_DEFAULT_NAME N_("User Library")

/**
 * \note For remote asset libraries, use #BKE_preferences_remote_asset_library_add().
 */
struct bUserAssetLibrary *BKE_preferences_asset_library_add(struct UserDef *userdef,
                                                            const char *name,
                                                            const char *dirpath) ATTR_NONNULL(1);
struct bUserAssetLibrary *BKE_preferences_remote_asset_library_add(struct UserDef *userdef,
                                                                   const char *name,
                                                                   const char *remote_url)
    ATTR_NONNULL(1, 3);

/**
 * \brief Update the remote URL and the cache directory derived from the URL.
 *
 * - Copies \a remote_url into #bUserAssetLibrary.remote_url, shortening to #FILE_MAX bytes if
 *   necessary.
 * - Adds a trailing slash if not present, and if the URL doesn't point directly to the
 *   `/_asset-library-meta.json` already.
 * - Updates #bUserAssetLibrary.dirpath to the cache path derived from the new URL. See
 *   #asset_system::remote_library_cache_directory_path_from_url() (or
 *   #asset_system::online_essentials_cache_directory_path() in case of the online essentials URL).
 */
void BKE_preferences_remote_asset_library_url_set(bUserAssetLibrary *library,
                                                  StringRef remote_url);

/**
 * Unlink and free a library preference member.
 * \note Free's \a library itself.
 */
void BKE_preferences_asset_library_remove(struct UserDef *userdef,
                                          struct bUserAssetLibrary *library) ATTR_NONNULL();

void BKE_preferences_asset_library_name_set(struct UserDef *userdef,
                                            struct bUserAssetLibrary *library,
                                            const char *name) ATTR_NONNULL();

/**
 * Set the library path, ensuring it is pointing to a directory.
 * Single blend files can only act as "Current File" library; libraries on disk
 * should always be directories. If the path does not exist, that's fine; it can
 * created as directory if necessary later.
 */
void BKE_preferences_asset_library_path_set(struct bUserAssetLibrary *library, const char *path)
    ATTR_NONNULL();

struct bUserAssetLibrary *BKE_preferences_asset_library_find_index(const struct UserDef *userdef,
                                                                   int index)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;
struct bUserAssetLibrary *BKE_preferences_asset_library_find_by_name(const struct UserDef *userdef,
                                                                     const char *name)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/**
 * Resolve \a ref to the #bUserAssetLibrary it refers to.
 *
 * Uses #AssetLibraryReference.custom_library_name (the persistent identity), falling back to
 * #AssetLibraryReference.custom_library_index only for references written before that field
 * existed.
 *
 * Prefer this over #BKE_preferences_asset_library_find_index() for anything holding an
 * #AssetLibraryReference: the index is a position in #UserDef.asset_libraries and shifts whenever
 * the list is reordered or an entry removed.
 *
 * \return null if \a ref is not #ASSET_LIBRARY_CUSTOM, or if the library it names is gone.
 */
struct bUserAssetLibrary *BKE_preferences_asset_library_find_from_ref(
    const struct UserDef *userdef, const struct AssetLibraryReference *ref)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/**
 * Point \a r_ref at \a user_library. Type, name (the identity) and index (its cache) are set
 * together, so the two can never disagree.
 *
 * This is the only supported way to build a custom library reference. It cannot construct a
 * reference to a library that does not exist, which is what makes "a custom reference without a
 * name can only come from a legacy file" an invariant.
 */
void BKE_preferences_asset_library_reference_set(const struct UserDef *userdef,
                                                 struct AssetLibraryReference *r_ref,
                                                 const struct bUserAssetLibrary *user_library)
    ATTR_NONNULL();

/**
 * Return the bUserAssetLibrary that contains the given file/directory path. The given path can be
 * the library's top-level directory, or any path inside that directory.
 *
 * When more than one asset libraries match, the first matching one is returned (no smartness when
 * there nested asset libraries).
 *
 * Return NULL when no such asset library is found.
 */
struct bUserAssetLibrary *BKE_preferences_asset_library_containing_path(
    const struct UserDef *userdef, const char *path) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

int BKE_preferences_asset_library_get_index(const struct UserDef *userdef,
                                            const struct bUserAssetLibrary *library)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/**
 * Check if the asset library defined in \a library has enough data to be loadable.
 * \param check_directory_exists: When true, a library is required to point to a valid path on disk
 * as its root, otherwise the library is considered invalid.
 */
bool BKE_preferences_asset_library_is_valid(const UserDef *userdef,
                                            const struct bUserAssetLibrary *library,
                                            const bool check_directory_exists) ATTR_NONNULL();

void BKE_preferences_asset_library_default_add(struct UserDef *userdef) ATTR_NONNULL();

/**
 * Create a new folder for organizing asset libraries.
 * \param parent: Parent folder (nullptr for root level).
 */
struct bUserAssetLibrary *BKE_preferences_asset_library_folder_add(
    struct UserDef *userdef, const char *name, struct bUserAssetLibrary *parent) ATTR_NONNULL(1);

/**
 * Move an asset library or folder to a different parent folder.
 * \param library: The library or folder to move.
 * \param new_parent: The new parent folder (nullptr for root level).
 */
void BKE_preferences_asset_library_move_to_folder(struct UserDef *userdef,
                                                  struct bUserAssetLibrary *library,
                                                  struct bUserAssetLibrary *new_parent)
    ATTR_NONNULL(1, 2);

/**
 * Check if the library is a folder (container for other libraries).
 */
bool BKE_preferences_asset_library_is_folder(const struct bUserAssetLibrary *library)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/**
 * Check if the library can be deleted.
 * Folders can only be deleted if they are empty.
 */
bool BKE_preferences_asset_library_can_delete(const struct UserDef *userdef,
                                              const struct bUserAssetLibrary *library)
    ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/** Where to move an asset library or folder relative to the target. */
enum eBKE_AssetLibraryMoveLocation {
  /** Move into the target folder (target must be a folder). */
  ASSET_LIBRARY_MOVE_INTO = 0,
  /** Move immediately before the target. */
  ASSET_LIBRARY_MOVE_BEFORE = 1,
  /** Move immediately after the target. */
  ASSET_LIBRARY_MOVE_AFTER = 2,
};

/**
 * Reorder an asset library or folder within its parent.
 * When moving a folder, all its descendants move with it.
 */
bool BKE_preferences_asset_library_reorder(UserDef *userdef,
                                           bUserAssetLibrary *library,
                                           bUserAssetLibrary *target,
                                           eBKE_AssetLibraryMoveLocation location)
    ATTR_NONNULL(1, 2, 3);

/**
 * Restore parent pointers from parent_name strings in the asset library hierarchy.
 *
 * This function must be called after reading UserDef from a file to reconstruct
 * the parent-child relationships between asset libraries. It restores the runtime
 * parent pointers from the parent_name strings that are saved in the DNA.
 *
 * Also performs cycle detection and breaks cycles if found (defensive programming).
 *
 * \param userdef: The UserDef structure with asset libraries.
 */
void BKE_preferences_asset_library_restore_hierarchy(UserDef *userdef);

/**
 * Return true only if the library and every ancestor folder are enabled.
 * Walks the #bUserAssetLibrary.parent chain checking #ASSET_LIBRARY_DISABLED.
 */
bool BKE_preferences_asset_library_is_effectively_enabled(
    const struct bUserAssetLibrary *library) ATTR_NONNULL() ATTR_WARN_UNUSED_RESULT;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extension Repositories
 * \{ */

bUserExtensionRepo *BKE_preferences_extension_repo_add(UserDef *userdef,
                                                       const char *name,
                                                       const char *module,
                                                       const char *custom_dirpath);
void BKE_preferences_extension_repo_remove(UserDef *userdef, bUserExtensionRepo *repo);
bUserExtensionRepo *BKE_preferences_extension_repo_add_default_remote(UserDef *userdef);
bUserExtensionRepo *BKE_preferences_extension_repo_add_default_user(UserDef *userdef);
bUserExtensionRepo *BKE_preferences_extension_repo_add_default_system(UserDef *userdef);
/** Create all default repositories, only use when repositories are empty. */
void BKE_preferences_extension_repo_add_defaults_all(UserDef *userdef);

void BKE_preferences_extension_repo_name_set(UserDef *userdef,
                                             bUserExtensionRepo *repo,
                                             const char *name);
void BKE_preferences_extension_repo_module_set(UserDef *userdef,
                                               bUserExtensionRepo *repo,
                                               const char *module);

void BKE_preferences_extension_repo_custom_dirpath_set(bUserExtensionRepo *repo, const char *path);
size_t BKE_preferences_extension_repo_dirpath_get(const bUserExtensionRepo *repo,
                                                  char *dirpath,
                                                  int dirpath_maxncpy);

/**
 * Returns a user editable directory associated with this repository.
 * Needed so extensions may have local data.
 */
size_t BKE_preferences_extension_repo_user_dirpath_get(const bUserExtensionRepo *repo,
                                                       char *dirpath,
                                                       const int dirpath_maxncpy);

/**
 * Check the module name is valid, while this should always be the case,
 * use this as an additional safely check before performing destructive operations
 * such as recursive file removal to prevent file/memory corruption causing user data loss.
 */
bool BKE_preferences_extension_repo_module_is_valid(const bUserExtensionRepo *repo);

bUserExtensionRepo *BKE_preferences_extension_repo_find_index(const UserDef *userdef, int index);
bUserExtensionRepo *BKE_preferences_extension_repo_find_by_module(const UserDef *userdef,
                                                                  const char *module);
/**
 * Using a full URL/remote path to find a repository that shares its prefix.
 */
bUserExtensionRepo *BKE_preferences_extension_repo_find_by_remote_url_prefix(
    const UserDef *userdef, const char *remote_url_full, const bool only_enabled);
int BKE_preferences_extension_repo_get_index(const UserDef *userdef,
                                             const bUserExtensionRepo *repo);

void BKE_preferences_extension_repo_read_data(struct BlendDataReader *reader,
                                              bUserExtensionRepo *repo);
void BKE_preferences_extension_repo_write_data(struct BlendWriter *writer,
                                               const bUserExtensionRepo *repo);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Web/remote utilities
 *
 *  For extension and online asset library remotes.
 * \{ */

/**
 * Skip the `https` or `http` part of a URL `https://`, return zero if none is found.
 */
int BKE_preferences_remote_scheme_end(const char *url);
/**
 * Set a name based on a URL, e.g. `https://www.example.com/path` -> `example.com`.
 */
void BKE_preferences_remote_to_name(const char *remote_url, char name[64 /*MAX_NAME*/]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name #bUserAssetShelvesSettings
 * \{ */

bUserAssetShelfSettings *BKE_preferences_asset_shelf_settings_get(const UserDef *userdef,
                                                                  const char *shelf_idname);
bool BKE_preferences_asset_shelf_settings_is_catalog_path_enabled(const UserDef *userdef,
                                                                  const char *shelf_idname,
                                                                  const char *catalog_path);
/**
 * Enable a catalog path for a asset shelf identified by \a shelf_idname. Will create the shelf
 * settings in the Preferences if necessary.
 * \return true if the catalog was newly enabled. The Preferences should be tagged as dirty then.
 */
bool BKE_preferences_asset_shelf_settings_ensure_catalog_path_enabled(UserDef *userdef,
                                                                      const char *shelf_idname,
                                                                      const char *catalog_path);

/**
 * Read the popup-shelf view preferences (preview size, display flags, popup width and height)
 * stored for the given shelf type. Fields whose stored value is 0 ("not set") are left untouched
 * in the outputs, so callers can pre-seed them with the shelf type's defaults.
 */
void BKE_preferences_asset_shelf_popup_view_load(const UserDef *userdef,
                                                 const char *shelf_idname,
                                                 short *r_preview_size,
                                                 short *r_display_flag,
                                                 short *r_width_units,
                                                 short *r_height_units);

/**
 * Persist the popup-shelf view preferences for the given shelf type. Creates the per-type
 * settings entry in the Preferences when missing. Caller is responsible for tagging the
 * Preferences as dirty (`U.runtime.is_dirty`).
 */
void BKE_preferences_asset_shelf_popup_view_store(UserDef *userdef,
                                                  const char *shelf_idname,
                                                  short preview_size,
                                                  short display_flag,
                                                  short width_units,
                                                  short height_units);

const EnumPropertyItem *BKE_preferences_active_section_itemf(const UserDef *userdef, bool *r_free);
/** \} */

/* -------------------------------------------------------------------- */
/** \name #bUserAssetBrowserSettings
 *
 * Per-library persistent collapse state of asset catalog paths in the asset browser.
 * \{ */

/**
 * A stable, position-independent identifier for the library \a ref refers to: "local", "all",
 * "essentials", "online_essentials", or the custom library's (unique) name. "default" when \a ref
 * names nothing.
 *
 * Use as a map key or a settings key. Unlike #AssetLibraryReference.custom_library_index it does
 * not change when the Preferences list is reordered.
 */
const char *BKE_preferences_asset_library_identifier_from_ref(
    const struct UserDef *userdef, const struct AssetLibraryReference *ref)
    ATTR_WARN_UNUSED_RESULT;

bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get(
    const UserDef *userdef, const char *library_identifier);
/**
 * Resolve (and lazily create) the settings entry for the given asset library reference.
 */
bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get_from_library_ref(
    UserDef *userdef, const AssetLibraryReference *library_ref);
/** Collapsed state for a catalog path in a library, or nullopt when not saved yet. */
std::optional<bool> BKE_preferences_asset_browser_settings_is_catalog_collapsed(
    const UserDef *userdef, const char *library_identifier, const char *catalog_path);
void BKE_preferences_asset_browser_settings_set_catalog_collapsed(UserDef *userdef,
                                                                  const char *library_identifier,
                                                                  const char *catalog_path,
                                                                  bool collapsed);
/**
 * Move the settings entry keyed by \a old_name over to \a new_name, so renaming a custom library
 * keeps its saved catalog collapse state instead of silently orphaning it.
 *
 * Entries outlive the library they were created for (removing a library does not remove them), so
 * \a new_name may already be taken by leftovers; those are dropped in favor of the live library.
 */
void BKE_preferences_asset_browser_settings_rename_library(UserDef *userdef,
                                                           const char *old_name,
                                                           const char *new_name);

void BKE_preferences_asset_browser_settings_blend_write(BlendWriter *writer,
                                                        const bUserAssetBrowserSettings *settings);
void BKE_preferences_asset_browser_settings_blend_read_data(BlendDataReader *reader,
                                                            bUserAssetBrowserSettings *settings);

/** \} */

}  // namespace blender
