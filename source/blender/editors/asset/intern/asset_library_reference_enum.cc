/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Helpers to convert asset library references from and to enum values and RNA enums.
 * In some cases it's simply not possible to reference an asset library with
 * #AssetLibraryReferences. This API guarantees a safe translation to indices/enum values for as
 * long as there is no change in the order of registered custom asset libraries.
 */

#include "BLI_listbase.h"
#include "BLI_map.hh"

#include <optional>
#include <string>

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_preferences.h"

#include "DNA_ID.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "UI_resources.hh"

#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"

namespace blender::ed::asset {

int library_reference_to_enum_value(const AssetLibraryReference *library)
{
  /* Simple case: Predefined repository, just set the value. */
  if (library->type < ASSET_LIBRARY_CUSTOM) {
    return library->type;
  }

  /* Resolve by identity, not by the stored index: the index may be stale, and the enum value must
   * describe the list as it is right now (that is what the menu items were built from). */
  const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(&U, library);
  if (user_library) {
    return ASSET_LIBRARY_CUSTOM + BLI_findindex(&U.asset_libraries, user_library);
  }

  return ASSET_LIBRARY_LOCAL;
}

static bool custom_library_is_valid(const bUserAssetLibrary *user_library)
{
  if (!BKE_preferences_asset_library_is_effectively_enabled(user_library)) {
    return false;
  }
  if (!user_library->name[0]) {
    return false;
  }

  return BKE_preferences_asset_library_is_valid(
      &U,
      user_library,
      /* Don't check if the path exists on disk. If an invalid library path is used, the Asset
       * Browser can give a nice hint on what's wrong, so include such items in menus the user can
       * choose from. */
      /*check_directory_exists=*/false);
}

AssetLibraryReference library_reference_from_enum_value(int value)
{
  AssetLibraryReference library;

  /* Simple case: Predefined repository, just set the value. */
  if (value < ASSET_LIBRARY_CUSTOM) {
    /* Callers are expected to pass a value that came from the asset library enum, so an unknown
     * builtin is a bug in the caller and must be caught in debug builds. Release builds still
     * fall back to a valid library rather than storing a garbage type, mirroring the "custom
     * library not found" fallback below. */
    if (!ELEM(value,
              ASSET_LIBRARY_ALL,
              ASSET_LIBRARY_LOCAL,
              ASSET_LIBRARY_ESSENTIALS,
              ASSET_LIBRARY_ONLINE_ESSENTIALS))
    {
      BLI_assert_unreachable();
      library.type = ASSET_LIBRARY_ALL;
      library.custom_library_index = -1;
      return library;
    }
    library.type = eAssetLibraryType(value);
    library.custom_library_index = -1;
    return library;
  }

  const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_index(
      &U, value - ASSET_LIBRARY_CUSTOM);

  if (!user_library) {
    library.type = ASSET_LIBRARY_ALL;
    library.custom_library_index = -1;
  }
  else if (custom_library_is_valid(user_library)) {
    BKE_preferences_asset_library_reference_set(&U, &library, user_library);
  }
  return library;
}

/* -------------------------------------------------------------------- */
/** \name Material content check
 *
 * Backs the material narrowing of the library selector (#LibraryEnumFilterOptions with
 * #require_material_content): plain, untagged libraries are only offered when they are known to
 * contain at least one material asset. Results are cached per library until the asset list is
 * cleared again (#list::clear(), #list::clear_all_library(), or a library rename), so redrawing
 * never walks a whole asset list repeatedly.
 * Only accessed from the main thread (UI drawing), so the cache needs no locking. */
/** \{ */

static Map<std::string, bool> &library_material_content_cache()
{
  /* Cleared explicitly on exit (#list::storage_exit) so the guarded allocator doesn't report the
   * statically allocated memory. Absence of a key means "unknown"; only definite results are
   * stored. */
  static Map<std::string, bool> cache;
  return cache;
}

/* The identifier may point straight into #UserDef (e.g. the library name), so it must be copied
 * for use as cache key. */
static std::string library_material_content_cache_key(
    const AssetLibraryReference &library_reference)
{
  return BKE_preferences_asset_library_identifier_from_ref(&U, &library_reference);
}

std::optional<bool> library_material_content_cached(const AssetLibraryReference &library_reference)
{
  const std::string key = library_material_content_cache_key(library_reference);
  if (const bool *cached = library_material_content_cache().lookup_ptr(key)) {
    return *cached;
  }
  return std::nullopt;
}

bool library_contains_material(const bContext *C, const AssetLibraryReference &library_reference)
{
  const std::string key = library_material_content_cache_key(library_reference);
  Map<std::string, bool> &cache = library_material_content_cache();

  if (const bool *cached = cache.lookup_ptr(key)) {
    return *cached;
  }

  if (list::is_loaded(&library_reference)) {
    bool has_materials = false;
    list::iterate(library_reference, [&](asset_system::AssetRepresentation &asset) {
      if (asset.get_id_type() == ID_MA) {
        has_materials = true;
        return false; /* Stop iterating. */
      }
      return true;
    });
    cache.add(key, has_materials);
    return has_materials;
  }

  /* Without a context (e.g. RNA introspection) only the cache may be consulted; never start
   * background jobs from there. */
  if (C == nullptr) {
    return false;
  }

  /* Start loading the asset list in the background. Asset lists notify on completion, so the next
   * redraw of the selector will re-evaluate and cache the result. */
  list::storage_fetch(&library_reference, C);
  return false;
}

void library_material_content_cache_reset()
{
  library_material_content_cache().clear();
}

void library_material_content_cache_clear(const AssetLibraryReference &library_reference)
{
  const std::string key = library_material_content_cache_key(library_reference);
  library_material_content_cache().remove(key);
}

/** \} */

/* Single point of truth for whether a leaf library should appear in a selector, used both when
 * emitting the item and when deciding if a folder's subtree has anything to show. Keeping the two
 * in sync avoids ever drawing an empty folder heading. */
static bool custom_leaf_passes_filter(const bUserAssetLibrary &user_library,
                                      const bool include_remote_libraries,
                                      const bool exclude_image_libraries,
                                      const bool only_image_libraries,
                                      const LibraryEnumFilterOptions *options)
{
  /* Folders are containers, never selectable leaves. */
  if (user_library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
    return false;
  }
  if (!include_remote_libraries && (user_library.flag & ASSET_LIBRARY_USE_REMOTE_URL)) {
    return false;
  }
  if (!custom_library_is_valid(&user_library)) {
    return false;
  }
  /* Libraries set up via "Add Image Library" only ever contain image assets, so they never
   * contribute anything to UI surfaces that filter on a different asset type (e.g. the brush
   * shelf); skip listing them there rather than showing a library that always looks empty. A
   * plain, untagged library is still a valid brush source, so this is an exclusion, not a
   * requirement. */
  if (exclude_image_libraries && (user_library.flag & ASSET_LIBRARY_IS_IMAGE_LIBRARY)) {
    return false;
  }
  /* Texture-only surfaces (image grid, Texture asset shelf) go the other way: only libraries
   * explicitly tagged "Add Image Library" are shown at all. Image indexing itself is opt-in
   * (#image_library_needs_reindex()), so an untagged library -- Local, Brush, or otherwise --
   * can never actually contain a discoverable image asset there; listing it would just be a
   * picker option that always resolves to an empty grid. */
  if (only_image_libraries && !(user_library.flag & ASSET_LIBRARY_IS_IMAGE_LIBRARY)) {
    return false;
  }

  if (options == nullptr) {
    return true;
  }
  if (options->exclude_brush_libraries && (user_library.flag & ASSET_LIBRARY_IS_BRUSH_LIBRARY)) {
    return false;
  }
  if (options->exclude_material_libraries &&
      (user_library.flag & ASSET_LIBRARY_IS_MATERIAL_LIBRARY))
  {
    return false;
  }
  /* Material browsing: plain, untagged libraries are only offered once they are known to hold at
   * least one material asset; libraries explicitly tagged "Add Material Library" are always
   * listed, even when still empty. Checked last so no background fetch is ever started for a
   * library that is invalid or excluded anyway. */
  if (options->require_material_content &&
      !(user_library.flag & ASSET_LIBRARY_IS_MATERIAL_LIBRARY))
  {
    AssetLibraryReference library_reference;
    BKE_preferences_asset_library_reference_set(&U, &library_reference, &user_library);
    if (!library_contains_material(options->C, library_reference)) {
      return false;
    }
  }
  return true;
}

/* Whether the folder's subtree contains at least one visible leaf under the current filter.
 * Recurses into nested folders. Used to prune folders that would otherwise show an empty heading. */
static bool folder_subtree_has_visible_leaf(const bUserAssetLibrary *folder,
                                            const bool include_remote_libraries,
                                            const bool exclude_image_libraries,
                                            const bool only_image_libraries,
                                            const LibraryEnumFilterOptions *options)
{
  for (const bUserAssetLibrary &lib : U.asset_libraries) {
    if (lib.parent != folder) {
      continue;
    }
    if (lib.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      if (folder_subtree_has_visible_leaf(&lib,
                                          include_remote_libraries,
                                          exclude_image_libraries,
                                          only_image_libraries,
                                          options))
      {
        return true;
      }
    }
    else if (custom_leaf_passes_filter(lib,
                                       include_remote_libraries,
                                       exclude_image_libraries,
                                       only_image_libraries,
                                       options))
    {
      return true;
    }
  }
  return false;
}

/* Emit the children of `parent` (nullptr for the root level) in listbase order, mirroring the
 * Preferences tree (#build_user_items_recursive). Folders become non-selectable section headings
 * followed by their contents; empty folders are skipped entirely. Folders never nest inside other
 * folders (enforced in #BKE_preferences_asset_library_reorder and folder creation), so the
 * hierarchy is at most one level deep -- no folder heading ever appears under another. */
static void add_custom_libraries_recursive(EnumPropertyItem **item,
                                           int *totitem,
                                           const bUserAssetLibrary *parent,
                                           const bool include_remote_libraries,
                                           const bool exclude_image_libraries,
                                           const bool only_image_libraries,
                                           const LibraryEnumFilterOptions *options)
{
  for (const auto [i, user_library] : U.asset_libraries.enumerate()) {
    if (user_library.parent != parent) {
      continue;
    }

    if (user_library.type == USER_ASSET_LIBRARY_ITEM_TYPE_FOLDER) {
      if (!folder_subtree_has_visible_leaf(&user_library,
                                           include_remote_libraries,
                                           exclude_image_libraries,
                                           only_image_libraries,
                                           options))
      {
        continue;
      }
      /* NOTE: A heading has value 0 and an empty identifier. Value 0 numerically equals
       * #ASSET_LIBRARY_ALL, but the empty identifier makes the item a non-selectable label (see
       * #ui_def_but_rna__menu), so get/set never resolve it -- no collision. */
      const EnumPropertyItem heading = RNA_ENUM_ITEM_HEADING(user_library.name, nullptr);
      RNA_enum_item_add(item, totitem, &heading);

      add_custom_libraries_recursive(item,
                                     totitem,
                                     &user_library,
                                     include_remote_libraries,
                                     exclude_image_libraries,
                                     only_image_libraries,
                                     options);
      continue;
    }

    if (!custom_leaf_passes_filter(user_library,
                                       include_remote_libraries,
                                       exclude_image_libraries,
                                       only_image_libraries,
                                       options))
    {
      continue;
    }

    AssetLibraryReference library_reference;
    BKE_preferences_asset_library_reference_set(&U, &library_reference, &user_library);

    const int enum_value = library_reference_to_enum_value(&library_reference);
    EnumPropertyItem tmp = {
        enum_value,
        user_library.name,
        ICON_NONE,
        user_library.name,
        /* Use library path or URL as description, it's a nice hint for users. */
        (user_library.flag & ASSET_LIBRARY_USE_REMOTE_URL) ? user_library.remote_url :
                                                             user_library.dirpath};
    RNA_enum_item_add(item, totitem, &tmp);
  }
}

static void rna_enum_add_custom_libraries(EnumPropertyItem **item,
                                          int *totitem,
                                          const bool include_remote_libraries,
                                          const bool exclude_image_libraries,
                                          const bool only_image_libraries,
                                          const LibraryEnumFilterOptions *options)
{
  /* Walk the folder hierarchy from the root so the selector order matches the Preferences tree. */
  add_custom_libraries_recursive(item,
                                 totitem,
                                 /*parent=*/nullptr,
                                 include_remote_libraries,
                                 exclude_image_libraries,
                                 only_image_libraries,
                                 options);
}

const EnumPropertyItem *library_reference_to_rna_enum_itemf(
    const bool include_readonly,
    const bool include_current_file,
    const bool include_remote_libraries,
    const bool include_separate_online_essentials,
    const bool exclude_image_libraries,
    const bool only_image_libraries,
    const LibraryEnumFilterOptions *options)
{
  EnumPropertyItem *item = nullptr;
  int totitem = 0;

  if (include_readonly) {
    BLI_assert(rna_enum_asset_library_type_items[0].value == ASSET_LIBRARY_ALL);
    RNA_enum_item_add(&item, &totitem, &rna_enum_asset_library_type_items[0]);
    RNA_enum_item_add_separator(&item, &totitem);
  }
  if (include_current_file) {
    BLI_assert(rna_enum_asset_library_type_items[1].value == ASSET_LIBRARY_LOCAL);
    RNA_enum_item_add(&item, &totitem, &rna_enum_asset_library_type_items[1]);
  }
  /* The Essentials ship brushes and node groups, never image assets, so a caller listing image
   * libraries only (the brush Texture panel's grid and the paint-source grids that share its list)
   * would offer a library that can only ever come up empty. */
  if (include_readonly && !only_image_libraries) {
    BLI_assert(rna_enum_asset_library_type_items[2].value == ASSET_LIBRARY_ESSENTIALS);
    RNA_enum_item_add(&item, &totitem, &rna_enum_asset_library_type_items[2]);
  }
  if (include_separate_online_essentials && !only_image_libraries) {
    BLI_assert(rna_enum_asset_library_type_items[3].value == ASSET_LIBRARY_ONLINE_ESSENTIALS);
    RNA_enum_item_add(&item, &totitem, &rna_enum_asset_library_type_items[3]);
  }

  {
    EnumPropertyItem *custom_item = nullptr;
    int tot_custom_item = 0;
    rna_enum_add_custom_libraries(&custom_item,
                                  &tot_custom_item,
                                  include_remote_libraries,
                                  exclude_image_libraries,
                                  only_image_libraries,
                                  options);

    /* Add separator if needed. */
    if ((tot_custom_item > 0) && (include_readonly || include_current_file)) {
      RNA_enum_item_add_separator(&item, &totitem);
    }
    RNA_enum_item_end(&custom_item, &tot_custom_item);
    RNA_enum_items_add(&item, &totitem, custom_item);

    MEM_delete(custom_item);
  }

  RNA_enum_item_end(&item, &totitem);
  return item;
}

const EnumPropertyItem *custom_libraries_rna_enum_itemf(const bool only_image_libraries)
{
  EnumPropertyItem *item = nullptr;
  int totitem = 0;

  rna_enum_add_custom_libraries(&item,
                                &totitem,
                                /* This function should return local/on-disk libraries only, so
                                 * skip remote ones. */
                                /*include_remote_libraries=*/false,
                                /*exclude_image_libraries=*/false,
                                only_image_libraries,
                                /*options=*/nullptr);

  RNA_enum_item_end(&item, &totitem);
  return item;
}

Vector<asset_system::AssetLibrary *> all_mode_libraries(const bool exclude_image_libraries,
                                                        const bool only_image_libraries)
{
  const bool skip_remote_libraries = !USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries);

  Map<std::string, asset_system::AssetLibrary *> loaded_by_key;
  asset_system::AssetLibrary::foreach_loaded(
      [&](asset_system::AssetLibrary &library) {
        if (skip_remote_libraries && library.remote_url().has_value()) {
          return;
        }
        const std::optional<AssetLibraryReference> lib_ref = library.library_reference();
        if (!lib_ref.has_value()) {
          return;
        }
        loaded_by_key.add(BKE_preferences_asset_library_identifier_from_ref(&U, &*lib_ref),
                          &library);
      },
      /*include_all_library=*/false);

  Vector<asset_system::AssetLibrary *> libraries;
  Set<std::string> emitted;

  const EnumPropertyItem *items = library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      exclude_image_libraries,
      only_image_libraries,
      /*options=*/nullptr);
  if (items) {
    for (const EnumPropertyItem *item = items; item->identifier; item++) {
      if (item->identifier[0] == '\0') {
        continue; /* Separator or folder heading. */
      }
      if (item->value == ASSET_LIBRARY_ALL) {
        continue;
      }
      const AssetLibraryReference ref = library_reference_from_enum_value(item->value);
      const std::string key = BKE_preferences_asset_library_identifier_from_ref(&U, &ref);
      if (asset_system::AssetLibrary *const *library = loaded_by_key.lookup_ptr(key)) {
        libraries.append(*library);
        emitted.add(key);
      }
    }
    MEM_delete(items);
  }

  /* Keep filtering complete for loaded libraries outside the selector's own filter flags. */
  for (const auto item : loaded_by_key.items()) {
    if (!emitted.contains(item.key)) {
      libraries.append(item.value);
    }
  }
  return libraries;
}

void fetch_all_mode_libraries(const bContext &C,
                              const bool exclude_image_libraries,
                              const bool only_image_libraries)
{
  const EnumPropertyItem *items = library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false,
      exclude_image_libraries,
      only_image_libraries,
      /*options=*/nullptr);
  if (!items) {
    return;
  }
  for (const EnumPropertyItem *item = items; item->identifier; item++) {
    if (item->identifier[0] == '\0' || item->value == ASSET_LIBRARY_ALL) {
      continue;
    }
    const AssetLibraryReference ref = library_reference_from_enum_value(item->value);
    list::storage_fetch(&ref, &C);
  }
  MEM_delete(items);
}

}  // namespace blender::ed::asset
