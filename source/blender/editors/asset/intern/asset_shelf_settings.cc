/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Internal and external APIs for #AssetShelfSettings.
 */

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

#include "asset_library_reference.hh"

#include "BKE_asset.hh"
#include "BKE_asset_shelf.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "WM_api.hh"

#include "ED_asset_library.hh"

#include "asset_shelf.hh"

namespace blender {

using namespace blender::ed::asset;

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

  return *this;
}

AssetShelfSettings::~AssetShelfSettings()
{
  BKE_asset_catalog_path_list_free(enabled_catalog_paths);
  BKE_asset_catalog_state_list_free(catalog_states);
  BKE_asset_shelf_library_catalog_state_list_free(library_catalog_states);
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
void library_catalog_state_list_migrate_legacy_active_path(
    ListBaseT<AssetShelfLibraryCatalogState> &list,
    const AssetLibraryReference &library_ref,
    const char *active_catalog_path);

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
  writer->write_string(settings.active_catalog_path);
}

void settings_blend_read_data(BlendDataReader *reader, AssetShelfSettings &settings)
{
  BKE_asset_catalog_path_list_blend_read_data(reader, settings.enabled_catalog_paths);
  BKE_asset_catalog_state_list_blend_read_data(reader, settings.catalog_states);
  library_catalog_state_list_blend_read_data(reader, settings.library_catalog_states);
  BLO_read_string(reader, &settings.active_catalog_path);

  /* New in 5.x: per-library catalog map. Empty list in older files is intentional; migrate the
   * legacy single-library #active_catalog_path (including Recent/Favorites sentinels). */
  library_catalog_state_list_migrate_legacy_active_path(settings.library_catalog_states,
                                                        settings.asset_library_reference,
                                                        settings.active_catalog_path);

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

void apply_active_catalog_path(AssetShelfSettings &settings, const StringRef path)
{
  if (path.is_empty()) {
    settings_set_all_catalog_active(settings);
    return;
  }
  if (path == catalog_sentinel_recent) {
    settings_set_recent_catalog_active(settings);
    return;
  }
  if (path == catalog_sentinel_favorites) {
    settings_set_favorites_catalog_active(settings);
    return;
  }
  settings_set_active_catalog(settings, asset_system::AssetCatalogPath(path));
}

std::string active_catalog_path_as_string(const AssetShelfSettings &settings)
{
  if (settings_is_all_catalog_active(settings)) {
    return {};
  }
  return settings.active_catalog_path;
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
  const std::string path = active_catalog_path_as_string(settings);
  AssetShelfLibraryCatalogState *state = library_catalog_state_find(list, library_ref);
  if (path.empty()) {
    if (state) {
      MEM_SAFE_DELETE(state->active_catalog_path);
      BLI_freelinkN(&list, state);
    }
    return;
  }
  if (!state) {
    state = MEM_new<AssetShelfLibraryCatalogState>(__func__);
    state->library_ref = library_ref;
    BLI_addtail(&list, state);
  }
  MEM_SAFE_DELETE(state->active_catalog_path);
  state->active_catalog_path = BLI_strdup(path.c_str());
}

void library_catalog_state_list_load_active(ListBaseT<AssetShelfLibraryCatalogState> &list,
                                            AssetShelfSettings &settings,
                                            const AssetLibraryReference &library_ref)
{
  const AssetShelfLibraryCatalogState *state = library_catalog_state_find(list, library_ref);
  if (state && state->active_catalog_path && state->active_catalog_path[0] != '\0') {
    apply_active_catalog_path(settings, state->active_catalog_path);
    return;
  }
  settings_set_all_catalog_active(settings);
}

void library_catalog_state_list_migrate_legacy_active_path(
    ListBaseT<AssetShelfLibraryCatalogState> &list,
    const AssetLibraryReference &library_ref,
    const char *active_catalog_path)
{
  if (!list.is_empty()) {
    return;
  }
  if (!active_catalog_path || active_catalog_path[0] == '\0') {
    return;
  }
  AssetShelfLibraryCatalogState *state = MEM_new<AssetShelfLibraryCatalogState>(__func__);
  state->library_ref = library_ref;
  state->active_catalog_path = BLI_strdup(active_catalog_path);
  BLI_addtail(&list, state);
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

static const char *library_catalog_path_or_empty(const char *path)
{
  return (path && path[0]) ? path : "";
}

static bool library_catalog_state_lists_equal(
    const ListBaseT<AssetShelfLibraryCatalogState> &a,
    const ListBaseT<AssetShelfLibraryCatalogState> &b)
{
  int count_a = 0;
  for (const AssetShelfLibraryCatalogState &state_a : a) {
    const AssetShelfLibraryCatalogState *state_b = library_catalog_state_find(b, state_a.library_ref);
    if (!state_b) {
      return false;
    }
    if (!STREQ(library_catalog_path_or_empty(state_a.active_catalog_path),
                library_catalog_path_or_empty(state_b->active_catalog_path)))
    {
      return false;
    }
    count_a++;
  }
  int count_b = 0;
  for (const AssetShelfLibraryCatalogState &state_b : b) {
    (void)state_b;
    count_b++;
  }
  return count_a == count_b;
}

}  // namespace

bool popup_library_catalog_settings_store(wmWindowManager &wm, const AssetShelf &shelf)
{
  const AssetShelfPopupLibraryCatalogs *existing = static_cast<
      const AssetShelfPopupLibraryCatalogs *>(BLI_findstring(&wm.asset_shelf_popup_library_catalogs,
                                                             shelf.idname,
                                                             offsetof(AssetShelfPopupLibraryCatalogs,
                                                                      idname)));
  if (existing &&
      library_catalog_state_lists_equal(existing->library_catalog_states,
                                        shelf.settings.library_catalog_states))
  {
    return false;
  }

  AssetShelfPopupLibraryCatalogs *entry = const_cast<AssetShelfPopupLibraryCatalogs *>(existing);
  if (!entry) {
    entry = MEM_new<AssetShelfPopupLibraryCatalogs>(__func__);
    STRNCPY_UTF8(entry->idname, shelf.idname);
    BLI_addtail(&wm.asset_shelf_popup_library_catalogs, entry);
  }
  BKE_asset_shelf_library_catalog_state_list_free(entry->library_catalog_states);
  BKE_asset_shelf_library_catalog_state_list_duplicate(entry->library_catalog_states,
                                                       shelf.settings.library_catalog_states);
  return true;
}

void popup_library_catalog_settings_load(const wmWindowManager &wm,
                                         const char *shelf_idname,
                                         AssetShelfSettings &settings)
{
  const AssetShelfPopupLibraryCatalogs *entry = static_cast<const AssetShelfPopupLibraryCatalogs *>(
      BLI_findstring(&wm.asset_shelf_popup_library_catalogs,
                     shelf_idname,
                     offsetof(AssetShelfPopupLibraryCatalogs, idname)));
  BKE_asset_shelf_library_catalog_state_list_free(settings.library_catalog_states);
  if (!entry) {
    return;
  }
  BKE_asset_shelf_library_catalog_state_list_duplicate(settings.library_catalog_states,
                                                       entry->library_catalog_states);
}

void popup_shelf_sync_per_file_state_from_wm(const wmWindowManager &wm, AssetShelf &shelf)
{
  popup_library_catalog_settings_load(wm, shelf.idname, shelf.settings);
  settings_load_active_catalog_for_library(shelf.settings,
                                           shelf.settings.asset_library_reference);
}

void settings_load_active_catalog_for_library(AssetShelfSettings &settings,
                                              const AssetLibraryReference &library_ref)
{
  library_catalog_state_list_load_active(settings.library_catalog_states, settings, library_ref);
}

void settings_catalog_commit_active(AssetShelf &shelf,
                                    wmWindowManager *wm,
                                    const bool tag_file_modified)
{
  AssetShelfSettings &settings = shelf.settings;
  library_catalog_state_list_commit_active(settings.library_catalog_states,
                                           settings,
                                           settings.asset_library_reference);
  if (!shelf.is_popup || !wm) {
    return;
  }
  if (popup_library_catalog_settings_store(*wm, shelf) && tag_file_modified) {
    WM_file_tag_modified();
  }
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
  shelf.settings.asset_library_reference = new_ref;
  library_catalog_state_list_load_active(
      shelf.settings.library_catalog_states, shelf.settings, new_ref);
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

}  // namespace ed::asset::shelf
}  // namespace blender
