/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Internal and external APIs for #AssetShelfSettings.
 */

#include "AS_asset_catalog_path.hh"

#include "DNA_asset_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "BLO_read_write.hh"

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_time.h"

#include "BKE_asset.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"

#include "MEM_guardedalloc.h"

#include "asset_shelf.hh"

using namespace blender;
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
  if (this->catalog_collapsed_states != other.catalog_collapsed_states) {
    BKE_asset_catalog_path_list_free(this->catalog_collapsed_states);
  }
  if (this->active_catalog_path != other.active_catalog_path) {
    MEM_SAFE_FREE(this->active_catalog_path);
  }

  /* Copy from 'other'. */
  this->asset_library_reference = other.asset_library_reference;
  STRNCPY_UTF8(this->search_string, other.search_string);
  this->preview_size = other.preview_size;
  this->display_flag = other.display_flag;

  if (other.active_catalog_path) {
    this->active_catalog_path = BLI_strdup(other.active_catalog_path);
  }
  this->enabled_catalog_paths = BKE_asset_catalog_path_list_duplicate(other.enabled_catalog_paths);
  this->catalog_collapsed_states = BKE_asset_catalog_path_list_duplicate(
      other.catalog_collapsed_states);

  return *this;
}

AssetShelfSettings::~AssetShelfSettings()
{
  BKE_asset_catalog_path_list_free(enabled_catalog_paths);
  BKE_asset_catalog_path_list_free(catalog_collapsed_states);
  MEM_SAFE_FREE(active_catalog_path);
}

namespace blender::ed::asset::shelf {

void settings_blend_write(BlendWriter *writer, const AssetShelfSettings &settings)
{
  BLO_write_struct(writer, AssetShelfSettings, &settings);

  BKE_asset_catalog_path_list_blend_write(writer, settings.enabled_catalog_paths);
  BKE_asset_catalog_path_list_blend_write(writer, settings.catalog_collapsed_states);
  BLO_write_string(writer, settings.active_catalog_path);
}

void settings_blend_read_data(BlendDataReader *reader, AssetShelfSettings &settings)
{
  BKE_asset_catalog_path_list_blend_read_data(reader, settings.enabled_catalog_paths);
  BKE_asset_catalog_path_list_blend_read_data(reader, settings.catalog_collapsed_states);
  BLO_read_string(reader, &settings.active_catalog_path);

  /* Restore hash values if missing (for backward compatibility) */
  LISTBASE_FOREACH (AssetCatalogState *, collapsed_state, &settings.catalog_collapsed_states) {
    if (collapsed_state && collapsed_state->path && collapsed_state->path_hash == 0) {
      collapsed_state->path_hash = BLI_hash_string(collapsed_state->path);
    }
    if (collapsed_state && collapsed_state->last_used == 0) {
      collapsed_state->last_used = uint32_t(BLI_time_now_seconds());
    }
  }
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

static bool use_enabled_catalogs_from_prefs(const AssetShelf &shelf)
{
  return shelf.type && (shelf.type->flag & ASSET_SHELF_TYPE_FLAG_STORE_CATALOGS_IN_PREFS);
}

static const ListBase *get_enabled_catalog_path_list(const AssetShelf &shelf)
{
  if (use_enabled_catalogs_from_prefs(shelf)) {
    bUserAssetShelfSettings *pref_settings = BKE_preferences_asset_shelf_settings_get(
        &U, shelf.idname);
    return pref_settings ? &pref_settings->enabled_catalog_paths : nullptr;
  }
  return &shelf.settings.enabled_catalog_paths;
}

static ListBase *get_enabled_catalog_path_list(AssetShelf &shelf)
{
  return const_cast<ListBase *>(
      get_enabled_catalog_path_list(const_cast<const AssetShelf &>(shelf)));
}

void settings_clear_enabled_catalogs(AssetShelf &shelf)
{
  ListBase *enabled_catalog_paths = get_enabled_catalog_path_list(shelf);
  if (enabled_catalog_paths) {
    BKE_asset_catalog_path_list_free(*enabled_catalog_paths);
    BLI_assert(BLI_listbase_is_empty(enabled_catalog_paths));
  }
}

bool settings_is_catalog_path_enabled(const AssetShelf &shelf,
                                      const asset_system::AssetCatalogPath &path)
{
  const ListBase *enabled_catalog_paths = get_enabled_catalog_path_list(shelf);
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
  const ListBase *enabled_catalog_paths = get_enabled_catalog_path_list(shelf);
  if (!enabled_catalog_paths) {
    return;
  }

  LISTBASE_FOREACH (const AssetCatalogState *, collapsed_state, enabled_catalog_paths) {
    fn(asset_system::AssetCatalogPath(collapsed_state->path));
  }
}

bool settings_is_catalog_path_collapsed(const AssetShelfSettings &settings,
                                        const asset_system::AssetCatalogPath &path)
{
  return BKE_asset_catalog_state_get_collapsed(
      settings.catalog_collapsed_states, path.c_str(), true);
}

void settings_set_catalog_path_collapsed(AssetShelfSettings &settings,
                                         const asset_system::AssetCatalogPath &path,
                                         bool collapsed)
{
  BKE_asset_catalog_state_set_collapsed(
      settings.catalog_collapsed_states, path.c_str(), collapsed);
}

void settings_toggle_catalog_path_collapsed(AssetShelfSettings &settings,
                                            const asset_system::AssetCatalogPath &path)
{
  BKE_asset_catalog_state_toggle_collapsed(settings.catalog_collapsed_states, path.c_str(), true);
}

/* Function to work directly with ListBase (for FileSelectParams) */
void settings_set_catalog_path_collapsed_in_listbase(ListBase &catalog_collapsed_states,
                                                     const asset_system::AssetCatalogPath &path,
                                                     bool collapsed)
{
  BKE_asset_catalog_state_set_collapsed(catalog_collapsed_states, path.c_str(), collapsed);
}

void settings_cleanup_old_catalog_states(AssetShelfSettings &settings, int keep_count)
{
  BKE_asset_catalog_state_cleanup_old(settings.catalog_collapsed_states, keep_count);
}

}  // namespace blender::ed::asset::shelf
