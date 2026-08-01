/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Internal and external APIs for #AssetShelfSettings.
 */

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"

#include "DNA_defs.h"
#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BLO_read_write.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_utildefines.h"

#include "asset_library_reference.hh"

#include "BKE_asset.hh"
#include "BKE_asset_catalog_memory.hh"
#include "BKE_asset_shelf.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "WM_api.hh"

#include "ED_asset_catalog.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"

#include "asset_shelf.hh"

#include <cstddef>
#include <optional>
#include <string>

namespace blender {

using namespace blender::ed::asset;

namespace {
/** Safe even if \a list's head is garbage (see #BLI_listbase_head_is_plausible): falls back to forgetting
 * the (untrusted) items rather than dereferencing them. */
template<typename T> void free_name_match_list_safe(ListBaseT<T> &list)
{
  if (BLI_listbase_head_is_plausible(&list)) {
    list.free_no_destruct();
  }
  else {
    list.clear_no_delete();
  }
}
}  // namespace

AssetShelfSettings::AssetShelfSettings() = default;

AssetShelfSettings::AssetShelfSettings(const AssetShelfSettings &other)
{
  operator=(other);
}

AssetShelfSettings &AssetShelfSettings::operator=(const AssetShelfSettings &other)
{
  if (this == &other) {
    return *this; /* Handle self-assignment safely. */
  }

  /* Free existing properties. Check if they point to the same memory first, #AssetShelfSettings
   * might have been shallow copied before. */
  if (this->enabled_catalog_paths != other.enabled_catalog_paths) {
    BKE_asset_catalog_path_list_free(this->enabled_catalog_paths);
  }
  if (this->catalog_states != other.catalog_states) {
    BKE_asset_catalog_state_list_free(this->catalog_states);
  }
  if (this->library_catalog_states != other.library_catalog_states) {
    BKE_asset_shelf_library_catalog_state_list_free(this->library_catalog_states);
  }
  if (this->filter_name_match_map_types != other.filter_name_match_map_types) {
    free_name_match_list_safe(this->filter_name_match_map_types);
  }
  if (this->filter_name_match_tags != other.filter_name_match_tags) {
    free_name_match_list_safe(this->filter_name_match_tags);
  }
  if (this->active_catalog_path != other.active_catalog_path) {
    MEM_SAFE_DELETE(this->active_catalog_path);
  }

  /* Copy from 'other'. */
  this->asset_library_reference = other.asset_library_reference;
  STRNCPY_UTF8(this->search_string, other.search_string);
  this->preview_size = other.preview_size;
  this->recent_max_count = other.recent_max_count;
  this->display_flag = other.display_flag;
  this->popup_width_units = other.popup_width_units;
  this->popup_height_units = other.popup_height_units;
  this->popup_catalog_width_units = other.popup_catalog_width_units;

  if (other.active_catalog_path) {
    this->active_catalog_path = BLI_strdup(other.active_catalog_path);
  }
  this->enabled_catalog_paths = BKE_asset_catalog_path_list_duplicate(other.enabled_catalog_paths);
  BKE_asset_catalog_state_list_duplicate(this->catalog_states, other.catalog_states);
  BKE_asset_shelf_library_catalog_state_list_duplicate(this->library_catalog_states,
                                                       other.library_catalog_states);

  this->filter_name_match_map_types = {nullptr, nullptr};
  for (const AssetNameMatchIdLink &link : other.filter_name_match_map_types) {
    AssetNameMatchIdLink *copy = MEM_new<AssetNameMatchIdLink>(__func__);
    STRNCPY_UTF8(copy->id, link.id);
    BLI_addtail(&this->filter_name_match_map_types, copy);
  }
  this->filter_name_match_tags = {nullptr, nullptr};
  for (const AssetNameMatchTagLink &link : other.filter_name_match_tags) {
    AssetNameMatchTagLink *copy = MEM_new<AssetNameMatchTagLink>(__func__);
    STRNCPY_UTF8(copy->name, link.name);
    BLI_addtail(&this->filter_name_match_tags, copy);
  }

  return *this;
}

AssetShelfSettings::~AssetShelfSettings()
{
  BKE_asset_catalog_path_list_free(enabled_catalog_paths);
  BKE_asset_catalog_state_list_free(catalog_states);
  BKE_asset_shelf_library_catalog_state_list_free(library_catalog_states);
  free_name_match_list_safe(filter_name_match_map_types);
  free_name_match_list_safe(filter_name_match_tags);
  MEM_SAFE_DELETE(active_catalog_path);
}

namespace ed::asset::shelf {

void library_catalog_state_list_commit_active(
    ListBaseT<AssetShelfLibraryCatalogState> &list,
    const AssetShelfSettings &settings,
    const AssetLibraryReference &library_ref);
void library_catalog_state_list_blend_write(BlendWriter *writer,
                                            const ListBaseT<AssetShelfLibraryCatalogState> &list);
void library_catalog_state_list_blend_read_data(BlendDataReader *reader,
                                               ListBaseT<AssetShelfLibraryCatalogState> &list);

void settings_commit_catalog_states_for_file_save(AssetShelfSettings &settings)
{
  library_catalog_state_list_commit_active(settings.library_catalog_states,
                                           settings,
                                           settings.asset_library_reference);
}

void settings_blend_write(BlendWriter *writer, const AssetShelfSettings &settings)
{
  writer->write_struct(&settings);

  BKE_asset_catalog_path_list_blend_write(writer, settings.enabled_catalog_paths);
  BKE_asset_catalog_state_list_blend_write(writer, settings.catalog_states);
  library_catalog_state_list_blend_write(writer, settings.library_catalog_states);
  writer->write_struct_list(&settings.filter_name_match_map_types);
  writer->write_struct_list(&settings.filter_name_match_tags);
  writer->write_string(settings.active_catalog_path);
}

void settings_blend_read_data(BlendDataReader *reader, AssetShelfSettings &settings)
{
  BKE_asset_catalog_path_list_blend_read_data(reader, settings.enabled_catalog_paths);
  BKE_asset_catalog_state_list_blend_read_data(reader, settings.catalog_states);
  library_catalog_state_list_blend_read_data(reader, settings.library_catalog_states);
  BLO_read_struct_list(reader, AssetNameMatchIdLink, &settings.filter_name_match_map_types);
  BLO_read_struct_list(reader, AssetNameMatchTagLink, &settings.filter_name_match_tags);
  BLO_read_string(reader, &settings.active_catalog_path);

  /* Older files may leave garbage list heads where these fields did not exist yet. */
  if (!BLI_listbase_head_is_plausible(&settings.filter_name_match_map_types)) {
    settings.filter_name_match_map_types.clear_no_delete();
  }
  if (!BLI_listbase_head_is_plausible(&settings.filter_name_match_tags)) {
    settings.filter_name_match_tags.clear_no_delete();
  }

  /* Preserve filtering for shelves that already had criteria before the master toggle existed. */
  if ((BLI_listbase_head_is_plausible(&settings.filter_name_match_map_types) &&
       !settings.filter_name_match_map_types.is_empty()) ||
      (BLI_listbase_head_is_plausible(&settings.filter_name_match_tags) &&
       !settings.filter_name_match_tags.is_empty()))
  {
    settings.display_flag |= ASSETSHELF_FILTER_NAME_MATCH_ENABLED;
  }

  /* Older files didn't store the recent-brush limit; default it to 20 instead of 0. */
  if (settings.recent_max_count <= 0) {
    settings.recent_max_count = 20;
  }
}

void popup_size_load(const wmWindowManager &wm,
                     const char *shelf_idname,
                     short *r_width_units,
                     short *r_height_units,
                     short *r_catalog_width_units)
{
  const AssetShelfPopupSize *size = static_cast<const AssetShelfPopupSize *>(BLI_findstring(
      &wm.asset_shelf_popup_sizes, shelf_idname, offsetof(AssetShelfPopupSize, idname)));
  if (!size) {
    return;
  }
  /* A stored 0 means "not set"; leave the caller's pre-seeded default untouched. */
  if (r_width_units && size->width_units) {
    *r_width_units = size->width_units;
  }
  if (r_height_units && size->height_units) {
    *r_height_units = size->height_units;
  }
  if (r_catalog_width_units && size->catalog_width_units) {
    *r_catalog_width_units = size->catalog_width_units;
  }
}

void popup_size_store(wmWindowManager &wm,
                      const char *shelf_idname,
                      const short width_units,
                      const short height_units,
                      const short catalog_width_units)
{
  AssetShelfPopupSize *size = static_cast<AssetShelfPopupSize *>(BLI_findstring(
      &wm.asset_shelf_popup_sizes, shelf_idname, offsetof(AssetShelfPopupSize, idname)));
  if (!size) {
    size = MEM_new<AssetShelfPopupSize>(__func__);
    STRNCPY_UTF8(size->idname, shelf_idname);
    BLI_addtail(&wm.asset_shelf_popup_sizes, size);
  }
  size->width_units = width_units;
  size->height_units = height_units;
  size->catalog_width_units = catalog_width_units;
}

AssetLibraryReference &settings_ensure_valid_library_ref(AssetShelfSettings &settings)
{
  if (settings.asset_library_reference.type != ASSET_LIBRARY_CUSTOM) {
    /* Nothing to validate, all good. */
    return settings.asset_library_reference;
  }

  if (ed::asset::library_reference_ensure_resolved(settings.asset_library_reference) ==
      ed::asset::LibraryRefStatus::Missing)
  {
    /* The library is gone, not merely unusable. Keep the reference so #settings_library_is_missing
     * can name it; #storage_fetch() simply fetches nothing for it. */
    return settings.asset_library_reference;
  }

  const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(
      &U, &settings.asset_library_reference);
  /* Resolvable but not selectable. Unchanged behaviour: fall back to "All". */
  if (BKE_preferences_asset_library_is_folder(user_library) ||
      !BKE_preferences_asset_library_is_effectively_enabled(user_library))
  {
    settings.asset_library_reference = asset_system::all_library_reference();
  }
  return settings.asset_library_reference;
}

bool settings_library_is_missing(const AssetShelfSettings &settings)
{
  if (settings.asset_library_reference.type != ASSET_LIBRARY_CUSTOM) {
    return false;
  }
  return BKE_preferences_asset_library_find_from_ref(&U, &settings.asset_library_reference) ==
         nullptr;
}

void settings_set_active_catalog(AssetShelfSettings &settings,
                                 const asset_system::AssetCatalogPath &path)
{
  MEM_delete(settings.active_catalog_path);
  settings.active_catalog_path = BLI_strdupn(path.c_str(), path.length());
}

void settings_set_all_catalog_active(AssetShelfSettings &settings)
{
  MEM_delete(settings.active_catalog_path);
  settings.active_catalog_path = nullptr;
}

bool settings_is_active_catalog(const AssetShelfSettings &settings,
                                const asset_system::AssetCatalogPath &path)
{
  return settings.active_catalog_path && settings.active_catalog_path == path.str();
}

bool settings_is_all_catalog_active(const AssetShelfSettings &settings)
{
  return !settings.active_catalog_path || !settings.active_catalog_path[0];
}

namespace {
/* Reserved sentinel values for #AssetShelfSettings::active_catalog_path, distinguishing the
 * Recent/Favorites pseudo-catalogs from a real catalog path (which is always a sequence of
 * user-visible names joined by '/' and can never start with a control character) or from the
 * "All" pseudo-catalog (empty/null). Mirrors how "All" already overloads this one field instead
 * of adding a new DNA field.
 *
 * The escape is deliberately a separate string literal from the text: `\x` consumes every
 * following hex digit, so `"\x01FAVORITES"` would parse as the single (out of range) character
 * `\x01FA` instead of `\x01` followed by "FAVORITES". */
constexpr const char *catalog_sentinel_recent = "\x01" "RECENT";
constexpr const char *catalog_sentinel_favorites = "\x01" "FAVORITES";
}  // namespace

void settings_set_recent_catalog_active(AssetShelfSettings &settings)
{
  MEM_delete(settings.active_catalog_path);
  settings.active_catalog_path = BLI_strdup(catalog_sentinel_recent);
}

bool settings_is_recent_catalog_active(const AssetShelfSettings &settings)
{
  return settings.active_catalog_path && STREQ(settings.active_catalog_path, catalog_sentinel_recent);
}

void settings_set_favorites_catalog_active(AssetShelfSettings &settings)
{
  MEM_delete(settings.active_catalog_path);
  settings.active_catalog_path = BLI_strdup(catalog_sentinel_favorites);
}

bool settings_is_favorites_catalog_active(const AssetShelfSettings &settings)
{
  return settings.active_catalog_path &&
         STREQ(settings.active_catalog_path, catalog_sentinel_favorites);
}

namespace {

/**
 * Resolve the working #AssetShelfSettings::active_catalog_path for permanent per-library memory.
 *
 * - Returns a nil UUID when the working selection is All / Recent / Favorites (clear memory).
 * - Returns a non-nil UUID when path→UUID lookup succeeds (update memory).
 * - Returns nullopt when a real catalog path is selected but path→UUID cannot run yet (library not
 *   loaded) or the path is not found — leave any existing #AssetShelfLibraryCatalogState entry
 *   unchanged. Old path-based commit stored the path without needing a loaded library; wiping the
 *   remembered UUID on a transient unload would regress that.
 */
std::optional<bUUID> active_catalog_id_for_commit(const AssetShelfSettings &settings,
                                                  const AssetLibraryReference &library_ref)
{
  if (settings_is_all_catalog_active(settings) || settings_is_recent_catalog_active(settings) ||
      settings_is_favorites_catalog_active(settings))
  {
    return BLI_uuid_nil();
  }
  if (!settings.active_catalog_path || !settings.active_catalog_path[0]) {
    return BLI_uuid_nil();
  }

  const asset_system::AssetLibrary *library = list::library_get_once_available(library_ref);
  if (!library) {
    return std::nullopt;
  }
  const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
      asset_system::AssetCatalogPath(settings.active_catalog_path));
  if (!catalog) {
    return std::nullopt;
  }
  return catalog->catalog_id;
}

void apply_active_catalog_id(AssetShelfSettings &settings,
                             const AssetLibraryReference &library_ref,
                             const bUUID catalog_id)
{
  if (BLI_uuid_is_nil(catalog_id)) {
    settings_set_all_catalog_active(settings);
    return;
  }

  const asset_system::AssetLibrary *library = list::library_get_once_available(library_ref);
  if (library) {
    if (const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog(
            asset_system::CatalogID(catalog_id)))
    {
      settings_set_active_catalog(settings, catalog->path);
      return;
    }
  }

  /* Library not loaded yet (optimistic apply): working selection stays path-based and cannot hold
   * a bare UUID. Leave All until the catalog tree is available; remembered UUID remains in
   * #library_catalog_states for a later load/revalidate. */
  settings_set_all_catalog_active(settings);
}

}  // namespace

static const AssetShelfLibraryCatalogState *library_catalog_state_find(
    const ListBaseT<AssetShelfLibraryCatalogState> &list, const AssetLibraryReference &library_ref)
{
  for (const AssetShelfLibraryCatalogState &state : list) {
    if (state.library_ref == library_ref) {
      return &state;
    }
  }
  return nullptr;
}

static AssetShelfLibraryCatalogState *library_catalog_state_find(
    ListBaseT<AssetShelfLibraryCatalogState> &list, const AssetLibraryReference &library_ref)
{
  return const_cast<AssetShelfLibraryCatalogState *>(
      library_catalog_state_find(static_cast<const ListBaseT<AssetShelfLibraryCatalogState> &>(list),
                                 library_ref));
}

void library_catalog_state_list_commit_active(
    ListBaseT<AssetShelfLibraryCatalogState> &list,
    const AssetShelfSettings &settings,
    const AssetLibraryReference &library_ref)
{
  const std::optional<bUUID> catalog_id = active_catalog_id_for_commit(settings, library_ref);
  if (!catalog_id) {
    /* Path selected but unresolved — keep any existing remembered UUID. */
    return;
  }

  AssetShelfLibraryCatalogState *state = library_catalog_state_find(list, library_ref);
  if (BLI_uuid_is_nil(*catalog_id)) {
    if (state) {
      BLI_freelinkN(&list, state);
    }
    return;
  }
  if (!state) {
    state = MEM_new<AssetShelfLibraryCatalogState>(__func__);
    state->library_ref = library_ref;
    BLI_addtail(&list, state);
  }
  state->active_catalog_id = *catalog_id;
}

void library_catalog_state_list_load_active(ListBaseT<AssetShelfLibraryCatalogState> &list,
                                            AssetShelfSettings &settings,
                                            const AssetLibraryReference &library_ref)
{
  const AssetShelfLibraryCatalogState *state = library_catalog_state_find(list, library_ref);
  if (!state || BLI_uuid_is_nil(state->active_catalog_id)) {
    settings_set_all_catalog_active(settings);
    return;
  }

  switch (ED_asset_catalog_validate(library_ref, state->active_catalog_id)) {
    case AssetCatalogValidation::Valid:
    case AssetCatalogValidation::LibraryNotYetLoaded:
      apply_active_catalog_id(settings, library_ref, state->active_catalog_id);
      break;
    case AssetCatalogValidation::NotFound:
      settings_set_all_catalog_active(settings);
      break;
  }
}

void library_catalog_state_list_blend_write(BlendWriter *writer,
                                            const ListBaseT<AssetShelfLibraryCatalogState> &list)
{
  BKE_asset_shelf_library_catalog_state_list_blend_write(writer, list);
}

void library_catalog_state_list_blend_read_data(BlendDataReader *reader,
                                               ListBaseT<AssetShelfLibraryCatalogState> &list)
{
  BKE_asset_shelf_library_catalog_state_list_blend_read_data(reader, list);
}

namespace {

/** Write a remembered catalog UUID into the per-instance list (Task 7 commit takes settings). */
static void library_catalog_state_list_set_active_id(
    ListBaseT<AssetShelfLibraryCatalogState> &list,
    const AssetLibraryReference &library_ref,
    const bUUID catalog_id)
{
  AssetShelfLibraryCatalogState *state = library_catalog_state_find(list, library_ref);
  if (BLI_uuid_is_nil(catalog_id)) {
    if (state) {
      BLI_freelinkN(&list, state);
    }
    return;
  }
  if (!state) {
    state = MEM_new<AssetShelfLibraryCatalogState>(__func__);
    state->library_ref = library_ref;
    BLI_addtail(&list, state);
  }
  state->active_catalog_id = catalog_id;
}

static std::string popup_domain_for_shelf_type(const AssetShelfType &shelf_type)
{
  return "asset_shelf_popup:" + std::string(shelf_type.idname);
}

}  // namespace

void popup_library_catalog_settings_store(const AssetShelf &shelf)
{
  if (!shelf.is_popup || !shelf.type) {
    return;
  }
  const std::string domain = popup_domain_for_shelf_type(*shelf.type);
  const AssetLibraryReference &current_ref = shelf.settings.asset_library_reference;
  const bool membership = settings_is_recent_catalog_active(shelf.settings) ||
                          settings_is_favorites_catalog_active(shelf.settings);

  for (const AssetShelfLibraryCatalogState &state : shelf.settings.library_catalog_states) {
    if (membership && state.library_ref == current_ref) {
      /* Recent/Favorites is not a UUID -- the mode write below covers the current library
       * instead of a stale #active_catalog_id left over from before membership was entered. */
      continue;
    }
    if (BLI_uuid_is_nil(state.active_catalog_id)) {
      BKE_asset_catalog_memory_set_all(&U, state.library_ref, domain);
    }
    else {
      BKE_asset_catalog_memory_set_single(
          &U, state.library_ref, domain, state.active_catalog_id);
    }
  }

  if (membership) {
    /* Mode-only write (rev-4 independence guard, same as image_grid/id_browser): never touches
     * #single_catalog_id / #catalog_id_set, so leaving Recent/Favorites restores whichever
     * catalog was last remembered for this library. */
    BKE_asset_catalog_memory_set_mode(&U,
                                      current_ref,
                                      domain,
                                      settings_is_recent_catalog_active(shelf.settings) ?
                                          ASSET_CATALOG_MEMORY_RECENT :
                                          ASSET_CATALOG_MEMORY_FAVORITES);
  }
  /* Commit removes the list entry for All; still must clear any prior UserDef SINGLE for the
   * current library or the next open restores the old catalog. */
  else if (!library_catalog_state_find(shelf.settings.library_catalog_states, current_ref)) {
    BKE_asset_catalog_memory_set_all(&U, current_ref, domain);
  }
  U.runtime.is_dirty = true;
}

static void popup_library_catalog_settings_load_for_library(
    AssetShelf &shelf, const AssetLibraryReference &library_ref)
{
  if (!shelf.is_popup || !shelf.type) {
    return;
  }
  const std::string domain = popup_domain_for_shelf_type(*shelf.type);
  const eAssetCatalogMemoryMode mode = BKE_asset_catalog_memory_get_mode(&U, library_ref, domain);
  switch (mode) {
    case ASSET_CATALOG_MEMORY_SINGLE: {
      const std::optional<bUUID> remembered = BKE_asset_catalog_memory_get_single(
          &U, library_ref, domain);
      if (remembered) {
        library_catalog_state_list_set_active_id(
            shelf.settings.library_catalog_states, library_ref, *remembered);
      }
      break;
    }
    case ASSET_CATALOG_MEMORY_ALL:
      /* Includes "no UserDef entry" (get_mode default). Clear any stale instance UUID. */
      library_catalog_state_list_set_active_id(
          shelf.settings.library_catalog_states, library_ref, BLI_uuid_nil());
      break;
    case ASSET_CATALOG_MEMORY_SET:
      /* Popup domain's per-instance list only ever holds ALL/SINGLE. */
      break;
    case ASSET_CATALOG_MEMORY_RECENT:
    case ASSET_CATALOG_MEMORY_FAVORITES:
      /* Not a UUID -- #popup_library_membership_restore applies these directly to the working
       * settings after the UUID-based restore below has run. */
      break;
  }
}

/**
 * Apply a remembered Recent/Favorites mode directly to the working settings. Must run *after*
 * #library_catalog_state_list_load_active / #settings_load_active_catalog_for_library, which
 * otherwise unconditionally resolve the per-instance UUID entry and would clobber this back to
 * All -- those functions have no notion of the Recent/Favorites pseudo-catalogs.
 */
static void popup_library_membership_restore(AssetShelf &shelf,
                                              const AssetLibraryReference &library_ref)
{
  if (!shelf.is_popup || !shelf.type) {
    return;
  }
  const std::string domain = popup_domain_for_shelf_type(*shelf.type);
  switch (BKE_asset_catalog_memory_get_mode(&U, library_ref, domain)) {
    case ASSET_CATALOG_MEMORY_RECENT:
      settings_set_recent_catalog_active(shelf.settings);
      break;
    case ASSET_CATALOG_MEMORY_FAVORITES:
      settings_set_favorites_catalog_active(shelf.settings);
      break;
    case ASSET_CATALOG_MEMORY_ALL:
    case ASSET_CATALOG_MEMORY_SINGLE:
    case ASSET_CATALOG_MEMORY_SET:
      break;
  }
}

void popup_library_catalog_settings_load(AssetShelf &shelf)
{
  popup_library_catalog_settings_load_for_library(shelf, shelf.settings.asset_library_reference);
}

void popup_shelf_sync_per_file_state_from_wm(const wmWindowManager & /*wm*/, AssetShelf &shelf)
{
  popup_library_catalog_settings_load(shelf);
  settings_load_active_catalog_for_library(shelf.settings,
                                           shelf.settings.asset_library_reference);
  popup_library_membership_restore(shelf, shelf.settings.asset_library_reference);
}

void settings_load_active_catalog_for_library(AssetShelfSettings &settings,
                                              const AssetLibraryReference &library_ref)
{
  library_catalog_state_list_load_active(settings.library_catalog_states, settings, library_ref);
}

void shelf_ensure_catalog_revalidated(AssetShelf &shelf)
{
  const AssetLibraryReference &library_ref = shelf.settings.asset_library_reference;
  if (!(shelf.catalog_validated_library_ref == library_ref)) {
    shelf.catalog_validated_library_ref = library_ref;
    shelf.catalog_validated = 0;
  }
  if (shelf.catalog_validated) {
    return;
  }
  if (settings_is_recent_catalog_active(shelf.settings) ||
      settings_is_favorites_catalog_active(shelf.settings))
  {
    /* Not a catalog-UUID selection -- nothing to validate against the (possibly still loading)
     * library. #settings_load_active_catalog_for_library has no notion of Recent/Favorites and
     * would clobber the active pseudo-catalog back to All the moment any
     * #ND_ASSET_LIST_READING notifier fires while the popover is open. */
    shelf.catalog_validated = 1;
    return;
  }
  /* Idempotent once the library is loaded; while still indexing, #apply_active_catalog_id leaves
   * All and listeners clear #catalog_validated so this runs again after load completes. */
  settings_load_active_catalog_for_library(shelf.settings, library_ref);
  shelf.catalog_validated = 1;
}

void settings_catalog_commit_active(AssetShelf &shelf)
{
  AssetShelfSettings &settings = shelf.settings;
  library_catalog_state_list_commit_active(settings.library_catalog_states,
                                           settings,
                                           settings.asset_library_reference);
  if (!shelf.is_popup) {
    return;
  }
  popup_library_catalog_settings_store(shelf);
}

void settings_swap_asset_library(AssetShelf &shelf, const AssetLibraryReference &new_ref)
{
  const AssetLibraryReference old_ref = shelf.settings.asset_library_reference;
  if (old_ref == new_ref) {
    return;
  }
  library_catalog_state_list_commit_active(shelf.settings.library_catalog_states,
                                           shelf.settings,
                                           old_ref);
  if (shelf.is_popup) {
    /* Persist old library into UserDef before leaving it (commit may have removed an All entry). */
    popup_library_catalog_settings_store(shelf);
  }
  shelf.settings.asset_library_reference = new_ref;
  if (shelf.is_popup) {
    /* Instance list is not a full UserDef mirror; pull remembered catalog for the target library. */
    popup_library_catalog_settings_load_for_library(shelf, new_ref);
  }
  library_catalog_state_list_load_active(
      shelf.settings.library_catalog_states, shelf.settings, new_ref);
  if (shelf.is_popup) {
    /* Must run after the UUID-based restore above -- see #popup_library_membership_restore. */
    popup_library_membership_restore(shelf, new_ref);
  }
}

static bool use_enabled_catalogs_from_prefs(const AssetShelf &shelf)
{
  return shelf.type && (shelf.type->flag & ASSET_SHELF_TYPE_FLAG_STORE_CATALOGS_IN_PREFS);
}

static const ListBaseT<AssetCatalogPathLink> *get_enabled_catalog_path_list(
    const AssetShelf &shelf)
{
  if (use_enabled_catalogs_from_prefs(shelf)) {
    bUserAssetShelfSettings *pref_settings = BKE_preferences_asset_shelf_settings_get(
        &U, shelf.idname);
    return pref_settings ? &pref_settings->enabled_catalog_paths : nullptr;
  }
  return &shelf.settings.enabled_catalog_paths;
}

static ListBaseT<AssetCatalogPathLink> *get_enabled_catalog_path_list(AssetShelf &shelf)
{
  return const_cast<ListBaseT<AssetCatalogPathLink> *>(
      get_enabled_catalog_path_list(const_cast<const AssetShelf &>(shelf)));
}

void settings_clear_enabled_catalogs(AssetShelf &shelf)
{
  ListBaseT<AssetCatalogPathLink> *enabled_catalog_paths = get_enabled_catalog_path_list(shelf);
  if (enabled_catalog_paths) {
    BKE_asset_catalog_path_list_free(*enabled_catalog_paths);
    BLI_assert(enabled_catalog_paths->is_empty());
  }
}

bool settings_is_catalog_path_enabled(const AssetShelf &shelf,
                                      const asset_system::AssetCatalogPath &path)
{
  const ListBaseT<AssetCatalogPathLink> *enabled_catalog_paths = get_enabled_catalog_path_list(
      shelf);
  if (!enabled_catalog_paths) {
    return false;
  }

  return BKE_asset_catalog_path_list_has_path(*enabled_catalog_paths, path.c_str());
}

void settings_set_catalog_path_enabled(AssetShelf &shelf,
                                       const asset_system::AssetCatalogPath &path)
{
  if (use_enabled_catalogs_from_prefs(shelf)) {
    if (BKE_preferences_asset_shelf_settings_ensure_catalog_path_enabled(
            &U, shelf.idname, path.c_str()))
    {
      U.runtime.is_dirty = true;
    }
  }
  else {
    if (!BKE_asset_catalog_path_list_has_path(shelf.settings.enabled_catalog_paths, path.c_str()))
    {
      BKE_asset_catalog_path_list_add_path(shelf.settings.enabled_catalog_paths, path.c_str());
    }
  }
}

void settings_foreach_enabled_catalog_path(
    const AssetShelf &shelf,
    FunctionRef<void(const asset_system::AssetCatalogPath &catalog_path)> fn)
{
  const ListBaseT<AssetCatalogPathLink> *enabled_catalog_paths = get_enabled_catalog_path_list(
      shelf);
  if (!enabled_catalog_paths) {
    return;
  }

  for (const AssetCatalogPathLink &path_link : *enabled_catalog_paths) {
    fn(asset_system::AssetCatalogPath(path_link.path));
  }
}

std::optional<bool> settings_get_catalog_path_collapsed(const AssetShelfSettings &settings,
                                                        const asset_system::AssetCatalogPath &path)
{
  return BKE_asset_catalog_state_get_collapsed(settings.catalog_states, path.c_str());
}

void settings_set_catalog_path_collapsed(AssetShelfSettings &settings,
                                         const asset_system::AssetCatalogPath &path,
                                         const bool collapsed)
{
  BKE_asset_catalog_state_set_collapsed(settings.catalog_states, path.c_str(), collapsed);
}

/* -------------------------------------------------------------------- */
/** \name Name Matching Filter
 * \{ */

bool settings_name_match_map_type_is_active(const AssetShelfSettings &settings,
                                            const char *identifier)
{
  if (identifier == nullptr || identifier[0] == '\0' ||
      !BLI_listbase_head_is_plausible(&settings.filter_name_match_map_types))
  {
    return false;
  }
  return BLI_findstring(&settings.filter_name_match_map_types,
                        identifier,
                        offsetof(AssetNameMatchIdLink, id)) != nullptr;
}

AssetNameMatchIdLink *settings_name_match_map_type_activate(AssetShelfSettings &settings,
                                                            const char *identifier)
{
  if (identifier == nullptr || identifier[0] == '\0' ||
      settings_name_match_map_type_is_active(settings, identifier))
  {
    return nullptr;
  }
  if (!BLI_listbase_head_is_plausible(&settings.filter_name_match_map_types)) {
    settings.filter_name_match_map_types.clear_no_delete();
  }
  AssetNameMatchIdLink *link = MEM_new<AssetNameMatchIdLink>(__func__);
  STRNCPY_UTF8(link->id, identifier);
  BLI_addtail(&settings.filter_name_match_map_types, link);
  settings.display_flag |= ASSETSHELF_FILTER_NAME_MATCH_ENABLED;
  return link;
}

bool settings_name_match_map_type_deactivate(AssetShelfSettings &settings,
                                             const char *identifier)
{
  if (identifier == nullptr || identifier[0] == '\0' ||
      !BLI_listbase_head_is_plausible(&settings.filter_name_match_map_types))
  {
    return false;
  }
  AssetNameMatchIdLink *link = static_cast<AssetNameMatchIdLink *>(BLI_findstring(
      &settings.filter_name_match_map_types, identifier, offsetof(AssetNameMatchIdLink, id)));
  if (link == nullptr) {
    return false;
  }
  BLI_freelinkN(&settings.filter_name_match_map_types, link);
  return true;
}

void settings_name_match_map_type_toggle(AssetShelfSettings &settings, const char *identifier)
{
  if (!settings_name_match_map_type_deactivate(settings, identifier)) {
    settings_name_match_map_type_activate(settings, identifier);
  }
}

void settings_name_match_map_type_clear(AssetShelfSettings &settings)
{
  free_name_match_list_safe(settings.filter_name_match_map_types);
}

bool settings_name_match_tag_is_active(const AssetShelfSettings &settings, const char *name)
{
  if (name == nullptr || name[0] == '\0' ||
      !BLI_listbase_head_is_plausible(&settings.filter_name_match_tags))
  {
    return false;
  }
  for (const AssetNameMatchTagLink &link : settings.filter_name_match_tags) {
    if (BLI_strcasecmp(link.name, name) == 0) {
      return true;
    }
  }
  return false;
}

AssetNameMatchTagLink *settings_name_match_tag_activate(AssetShelfSettings &settings,
                                                        const char *name)
{
  if (name == nullptr || name[0] == '\0' || settings_name_match_tag_is_active(settings, name)) {
    return nullptr;
  }
  if (!BLI_listbase_head_is_plausible(&settings.filter_name_match_tags)) {
    settings.filter_name_match_tags.clear_no_delete();
  }
  AssetNameMatchTagLink *link = MEM_new<AssetNameMatchTagLink>(__func__);
  STRNCPY_UTF8(link->name, name);
  BLI_addtail(&settings.filter_name_match_tags, link);
  settings.display_flag |= ASSETSHELF_FILTER_NAME_MATCH_ENABLED;
  return link;
}

bool settings_name_match_tag_deactivate(AssetShelfSettings &settings, const char *name)
{
  if (name == nullptr || name[0] == '\0' ||
      !BLI_listbase_head_is_plausible(&settings.filter_name_match_tags))
  {
    return false;
  }
  for (AssetNameMatchTagLink &link : settings.filter_name_match_tags.items_mutable()) {
    if (BLI_strcasecmp(link.name, name) == 0) {
      BLI_freelinkN(&settings.filter_name_match_tags, &link);
      return true;
    }
  }
  return false;
}

void settings_name_match_tag_toggle(AssetShelfSettings &settings, const char *name)
{
  if (!settings_name_match_tag_deactivate(settings, name)) {
    settings_name_match_tag_activate(settings, name);
  }
}

void settings_name_match_tag_clear(AssetShelfSettings &settings)
{
  free_name_match_list_safe(settings.filter_name_match_tags);
}

bool settings_name_match_filter_enabled(const AssetShelfSettings &settings)
{
  return (settings.display_flag & ASSETSHELF_FILTER_NAME_MATCH_ENABLED) != 0;
}

bool settings_name_match_filter_is_active(const AssetShelfSettings &settings)
{
  if (!settings_name_match_filter_enabled(settings)) {
    return false;
  }
  return (BLI_listbase_head_is_plausible(&settings.filter_name_match_map_types) &&
         !settings.filter_name_match_map_types.is_empty()) ||
         (BLI_listbase_head_is_plausible(&settings.filter_name_match_tags) &&
          !settings.filter_name_match_tags.is_empty());
}

void settings_name_match_filter_clear(AssetShelfSettings &settings)
{
  settings_name_match_map_type_clear(settings);
  settings_name_match_tag_clear(settings);
}

/** \} */

}  // namespace ed::asset::shelf
}  // namespace blender
