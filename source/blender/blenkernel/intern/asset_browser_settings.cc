/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Asset Browser settings management functions (per-library catalog state persistence).
 */

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_time.h"

#include "BKE_asset.hh"
#include "BKE_preferences.h"

#include "MEM_guardedalloc.h"

#include "BLO_read_write.hh"

namespace blender {

/* Returns a stable string identifier for a given library reference so catalog states
 * can be keyed per-library in #UserDef::asset_browser_settings. */
static const char *asset_library_identifier_from_library_ref(
    const UserDef *userdef, const AssetLibraryReference *library_ref)
{
  if (!library_ref) {
    return "default";
  }

  switch (eAssetLibraryType(library_ref->type)) {
    case ASSET_LIBRARY_LOCAL:
      return "local";
    case ASSET_LIBRARY_ESSENTIALS:
      return "essentials";
    case ASSET_LIBRARY_ONLINE_ESSENTIALS:
      return "online_essentials";
    case ASSET_LIBRARY_ALL:
      return "all";
    case ASSET_LIBRARY_CUSTOM: {
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_index(
          userdef, library_ref->custom_library_index);
      if (user_library) {
        return user_library->name;
      }
      return "default";
    }
  }
  return "default";
}

static bUserAssetBrowserSettings *asset_browser_settings_ensure(UserDef *userdef,
                                                                const char *library_name)
{
  bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(userdef,
                                                                                   library_name);
  if (settings) {
    return settings;
  }

  settings = MEM_new<bUserAssetBrowserSettings>(__func__);
  STRNCPY(settings->library_name, library_name);
  BLI_addtail(&userdef->asset_browser_settings, settings);

  return settings;
}

bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get(
    const UserDef *userdef, const char *library_identifier)
{
  for (bUserAssetBrowserSettings &settings : userdef->asset_browser_settings) {
    if (STREQ(settings.library_name, library_identifier)) {
      return &settings;
    }
  }
  return nullptr;
}

bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get_from_library_ref(
    UserDef *userdef, const AssetLibraryReference *library_ref)
{
  const char *library_identifier = asset_library_identifier_from_library_ref(userdef, library_ref);
  bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);

  /* Auto-create a settings entry so the first collapse interaction has somewhere to save. */
  if (!settings && library_ref) {
    eAssetLibraryType lib_type = eAssetLibraryType(library_ref->type);
    if (lib_type == ASSET_LIBRARY_ALL || lib_type == ASSET_LIBRARY_LOCAL ||
        lib_type == ASSET_LIBRARY_ESSENTIALS || lib_type == ASSET_LIBRARY_ONLINE_ESSENTIALS ||
        lib_type == ASSET_LIBRARY_CUSTOM)
    {
      settings = asset_browser_settings_ensure(userdef, library_identifier);
    }
  }

  return settings;
}

bool BKE_preferences_asset_browser_settings_is_catalog_collapsed(const UserDef *userdef,
                                                                 const char *library_identifier,
                                                                 const char *catalog_path)
{
  const bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);
  if (!settings) {
    /* Default to expanded when no state has been saved yet. */
    return false;
  }

  /* Default to expanded (false) for paths not yet explicitly saved, so catalogs remain
   * accessible on first use without requiring manual interaction. */
  return BKE_asset_catalog_state_get_collapsed(settings->catalog_states, catalog_path, false);
}

void BKE_preferences_asset_browser_settings_set_catalog_collapsed(UserDef *userdef,
                                                                  const char *library_identifier,
                                                                  const char *catalog_path,
                                                                  bool collapsed)
{
  if (!userdef || !library_identifier || !catalog_path || !catalog_path[0]) {
    return;
  }

  bUserAssetBrowserSettings *settings = asset_browser_settings_ensure(userdef, library_identifier);

  /* Evict oldest entries when approaching the per-library cap. */
  const int max_catalog_states = 1000;
  const int current_count = BLI_listbase_count(&settings->catalog_states);

  if (current_count >= max_catalog_states) {
    BKE_asset_catalog_state_cleanup_old(settings->catalog_states, max_catalog_states / 2);
  }

  BKE_asset_catalog_state_set_collapsed(settings->catalog_states, catalog_path, collapsed);
}

void BKE_preferences_asset_browser_settings_toggle_catalog_collapsed(
    UserDef *userdef, const char *library_identifier, const char *catalog_path)
{
  const bool current_state = BKE_preferences_asset_browser_settings_is_catalog_collapsed(
      userdef, library_identifier, catalog_path);
  BKE_preferences_asset_browser_settings_set_catalog_collapsed(
      userdef, library_identifier, catalog_path, !current_state);
}

void BKE_preferences_asset_browser_settings_cleanup_old(UserDef *userdef,
                                                        const char *library_identifier,
                                                        int target_count)
{
  bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);
  if (!settings) {
    return;
  }

  BKE_asset_catalog_state_cleanup_old(settings->catalog_states, target_count);
}

static void asset_browser_settings_cleanup_old_states(UserDef *userdef,
                                                      const uint32_t max_age_seconds,
                                                      const int max_removed_per_library)
{
  const uint32_t current_time = uint32_t(BLI_time_now_seconds());

  for (bUserAssetBrowserSettings &settings : userdef->asset_browser_settings.items_mutable()) {
    int removed_count = 0;
    for (AssetCatalogState &collapsed_state : settings.catalog_states.items_mutable()) {
      if ((current_time - collapsed_state.last_used) > max_age_seconds) {
        MEM_delete(collapsed_state.path);
        BLI_freelinkN(&settings.catalog_states, &collapsed_state);
        removed_count++;

        if (removed_count >= max_removed_per_library) {
          break;
        }
      }
    }
  }
}

void BKE_preferences_asset_browser_settings_cleanup_all_old(UserDef *userdef)
{
  const uint32_t max_age_days = 30;
  const uint32_t max_age_seconds = max_age_days * 24 * 60 * 60;
  asset_browser_settings_cleanup_old_states(userdef, max_age_seconds, 100);
}

void BKE_preferences_asset_browser_settings_cleanup_startup(UserDef *userdef)
{
  const uint32_t max_age_days = 30;
  const uint32_t max_age_seconds = max_age_days * 24 * 60 * 60;
  asset_browser_settings_cleanup_old_states(userdef, max_age_seconds, 50);
}

void BKE_preferences_asset_browser_settings_cleanup(UserDef *userdef)
{
  BKE_preferences_asset_browser_settings_clear_all(userdef);
}

void BKE_preferences_asset_browser_settings_clear_all(UserDef *userdef)
{
  for (bUserAssetBrowserSettings &settings : userdef->asset_browser_settings.items_mutable()) {
    BKE_asset_catalog_path_list_free(settings.catalog_states);
    BLI_freelinkN(&userdef->asset_browser_settings, &settings);
  }
}

void BKE_preferences_asset_browser_settings_blend_write(BlendWriter *writer,
                                                        const bUserAssetBrowserSettings *settings)
{
  writer->write_struct(settings);
  BKE_asset_catalog_path_list_blend_write(writer, settings->catalog_states);
}

void BKE_preferences_asset_browser_settings_blend_read_data(BlendDataReader *reader,
                                                            bUserAssetBrowserSettings *settings)
{
  BKE_asset_catalog_path_list_blend_read_data(reader, settings->catalog_states);

  /* Validate and fix data after loading. */
  for (AssetCatalogState &collapsed_state : settings->catalog_states.items_mutable()) {
    if (!collapsed_state.path || !collapsed_state.path[0]) {
      MEM_delete(collapsed_state.path);
      BLI_freelinkN(&settings->catalog_states, &collapsed_state);
    }
    else {
      if (collapsed_state.path_hash == 0) {
        collapsed_state.path_hash = BLI_hash_string(collapsed_state.path);
      }
      if (collapsed_state.last_used == 0) {
        collapsed_state.last_used = uint32_t(BLI_time_now_seconds());
      }
    }
  }
}

int BKE_preferences_asset_browser_settings_get_catalog_count(const UserDef *userdef,
                                                             const char *library_identifier)
{
  const bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);
  return settings ? BLI_listbase_count(&settings->catalog_states) : 0;
}

}  // namespace blender
