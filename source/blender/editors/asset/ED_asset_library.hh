/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"
#include "DNA_asset_types.h"

namespace blender {

struct bUserAssetLibrary;
struct bContext;
struct AssetLibraryReference;
struct EnumPropertyItem;
struct Main;
struct StringPropertySearchVisitParams;

namespace asset_system {
class AssetCatalog;
class AssetCatalogPath;
class AssetRepresentation;
}  // namespace asset_system

namespace ed::asset {

/**
 * Return an index that can be used to uniquely identify \a library, assuming
 * that all relevant indices were created with this function.
 */
int library_reference_to_enum_value(const AssetLibraryReference *library);
/**
 * Return an asset library reference matching the index returned by
 * #library_reference_to_enum_value().
 */
AssetLibraryReference library_reference_from_enum_value(int value);
/**
 * Translate all available asset libraries to an RNA enum, whereby the enum values match the result
 * of #library_reference_to_enum_value() for any given library.
 *
 * Since this is meant for UI display, skips non-displayable libraries, that is, libraries with an
 * empty name or path.
 *
 * \param include_readonly: If set, the "All" and "Essentials" asset libraries will be added, which
 * cannot be written to.
 * \param include_current_file: If set, "Current File" asset library will be added.
 * \param include_remote_libraries: If set, all online asset libraries with a URL set will be
 * added.
 * \param include_separate_online_essentials: If set, the online essentials will be added as a
 *    separate library from the normal Essentials. Usually they are a part of the normal Essentials
 *    library.
 * \param exclude_image_libraries: If set, custom libraries set up via "Add Image Library" (which
 *    only ever contain image assets) are left out of the list. Use for surfaces dedicated to a
 *    different asset type where a plain, untagged library is still a perfectly valid source (e.g.
 *    the brush shelf: most brush libraries are never explicitly tagged).
 * \param only_image_libraries: If set, only custom libraries set up via "Add Image Library" are
 *    included; every other custom library (tagged or not) is left out. Use for surfaces where an
 *    untagged library can never actually contribute anything -- image indexing itself is opt-in
 *    (see #image_library_needs_reindex()), so a plain library never has image assets to show.
 */
const EnumPropertyItem *library_reference_to_rna_enum_itemf(
    bool include_readonly,
    bool include_current_file,
    bool include_remote_libraries,
    bool include_separate_online_essentials,
    bool exclude_image_libraries = false,
    bool only_image_libraries = false);
/**
 * Same as #library_reference_to_rna_enum_itemf(), but only includes custom on-disk asset libraries
 * (libraries on disk, configured in the Preferences). Online asset libraries will be excluded,
 * their on-disk location is just a cache.
 */
const EnumPropertyItem *custom_libraries_rna_enum_itemf(bool only_image_libraries = false);

/**
 * Find the catalog with the given path in the library. Creates it in case it doesn't exist.
 */
asset_system::AssetCatalog &library_ensure_catalogs_in_path(
    asset_system::AssetLibrary &library, const asset_system::AssetCatalogPath &path);

AssetLibraryReference user_library_to_library_ref(const bUserAssetLibrary &user_library);

/** Result of #library_reference_ensure_resolved(). */
enum class LibraryRefStatus {
  /** The reference resolves; #AssetLibraryReference.custom_library_index is now up to date. */
  Ok,
  /** The named library is gone from the Preferences (renamed, removed, or the file comes from
   * another machine). The reference is left untouched so the UI can name it. */
  Missing,
};

/**
 * Resolve \a library_ref against the current Preferences and bring its cached members up to date.
 *
 * Does three things at once:
 * - refreshes #AssetLibraryReference.custom_library_index from the name, so a `.blend` saved after
 *   this still resolves in Blender versions that only understand the index;
 * - back-fills #AssetLibraryReference.custom_library_name for legacy references, which also
 *   canonicalises them as #Map keys (see #DefaultHash<AssetLibraryReference>);
 * - reports #LibraryRefStatus::Missing when nothing resolves.
 *
 * Call from the few gates that are about to use a reference (and can tell the user about it).
 * Everything that merely needs the library should use
 * #BKE_preferences_asset_library_find_from_ref() instead, which is const and side-effect free.
 *
 * \note Folders and disabled libraries are *not* Missing: they resolve, and each caller keeps its
 * own existing policy for them (the asset shelf falls back to "All").
 */
LibraryRefStatus library_reference_ensure_resolved(AssetLibraryReference &library_ref);

/**
 * Run \a fn on every #AssetLibraryReference stored in \a bmain, plus the popup asset shelves.
 *
 * \warning Add new stored references here. One that is missed is not propagated on rename and will
 * report Missing instead -- degraded, but visible, never a silent switch to another library.
 */
void foreach_library_reference(Main &bmain, FunctionRef<void(AssetLibraryReference &)> fn);

/**
 * Re-point every stored reference naming \a old_name at \a new_name, so renaming a library in the
 * Preferences does not break the references in open files.
 *
 * Also moves the asset browser's saved catalog collapse state (#UserDef::asset_browser_settings),
 * which is keyed by the library name rather than by an #AssetLibraryReference.
 *
 * Best effort by construction: references in files that are not open cannot be reached and will
 * report Missing. This mirrors the contract #AssetWeakReference already documents.
 */
void library_references_rename(Main &bmain, StringRefNull old_name, StringRefNull new_name);

/**
 * Call after changes to an asset library have been made to reflect the changes in the UI.
 */
void refresh_asset_library(const bContext *C, const AssetLibraryReference &library_ref);
void refresh_asset_library(const bContext *C, const bUserAssetLibrary &user_library);
void refresh_asset_library_from_asset(const bContext *C,
                                      const asset_system::AssetRepresentation &asset);

}  // namespace ed::asset
}  // namespace blender
