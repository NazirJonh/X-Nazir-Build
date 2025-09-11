/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Asset Browser settings management functions.
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

/* Structure for sorting catalog states by last used time */
struct CatalogStateInfo {
  AssetCatalogState *state;
  uint32_t last_used;
};

/* Comparison function for qsort */
static int catalog_state_info_compare(const void *a, const void *b)
{
  const CatalogStateInfo *info_a = (const CatalogStateInfo *)a;
  const CatalogStateInfo *info_b = (const CatalogStateInfo *)b;
  return (info_a->last_used > info_b->last_used) ? 1 : -1;
}

/* Helper function to get library identifier from AssetLibraryReference */
static const char *asset_library_identifier_from_library_ref(
    const AssetLibraryReference *library_ref)
{
  if (!library_ref) {
    return "default";
  }

  switch (eAssetLibraryType(library_ref->type)) {
    case ASSET_LIBRARY_LOCAL:
      return "local";
    case ASSET_LIBRARY_ESSENTIALS:
      return "essentials";
    case ASSET_LIBRARY_ALL:
      return "all";
    case ASSET_LIBRARY_CUSTOM: {
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_index(
          &U, library_ref->custom_library_index);
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

  settings = static_cast<bUserAssetBrowserSettings *>(
      MEM_callocN(sizeof(bUserAssetBrowserSettings), __func__));
  STRNCPY(settings->library_name, library_name);
  BLI_listbase_clear(&settings->catalog_collapsed_states);
  BLI_addtail(&userdef->asset_browser_settings, settings);

  return settings;
}

bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get(
    const UserDef *userdef, const char *library_identifier)
{
  LISTBASE_FOREACH (bUserAssetBrowserSettings *, settings, &userdef->asset_browser_settings) {
    if (STREQ(settings->library_name, library_identifier)) {
      return settings;
    }
  }
  return nullptr;
}

bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get_from_library_ref(
    const UserDef *userdef, const AssetLibraryReference *library_ref)
{
  const char *library_identifier = asset_library_identifier_from_library_ref(library_ref);
  bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);

  /* If settings not found, create them automatically for all libraries */
  if (!settings && library_ref) {
    eAssetLibraryType lib_type = eAssetLibraryType(library_ref->type);
    if (lib_type == ASSET_LIBRARY_ALL || lib_type == ASSET_LIBRARY_LOCAL ||
        lib_type == ASSET_LIBRARY_ESSENTIALS || lib_type == ASSET_LIBRARY_CUSTOM)
    {
      settings = asset_browser_settings_ensure(const_cast<UserDef *>(userdef), library_identifier);
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
    return true; /* Default to collapsed state */
  }

  return BKE_asset_catalog_path_is_collapsed(settings->catalog_collapsed_states, catalog_path);
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

  /* Check limit before adding new entries */
  const int MAX_CATALOG_STATES = 1000;
  const int current_count = BLI_listbase_count(&settings->catalog_collapsed_states);

  if (current_count >= MAX_CATALOG_STATES) {
    BKE_asset_catalog_state_cleanup_old(settings->catalog_collapsed_states,
                                        MAX_CATALOG_STATES / 2);
  }

  /* Use the new generalized function */
  BKE_asset_catalog_state_set_collapsed(
      settings->catalog_collapsed_states, catalog_path, collapsed);
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

  /* Use the new generalized cleanup function */
  BKE_asset_catalog_state_cleanup_old(settings->catalog_collapsed_states, target_count);
}

void BKE_preferences_asset_browser_settings_cleanup_all_old(UserDef *userdef)
{
  const uint32_t current_time = uint32_t(BLI_time_now_seconds());
  const uint32_t max_age_days = 30;  // Remove records older than 30 days
  const uint32_t max_age_seconds = max_age_days * 24 * 60 * 60;

  LISTBASE_FOREACH (bUserAssetBrowserSettings *, settings, &userdef->asset_browser_settings) {
    int removed_count = 0;
    LISTBASE_FOREACH_MUTABLE (
        AssetCatalogState *, collapsed_state, &settings->catalog_collapsed_states)
    {
      if (collapsed_state && (current_time - collapsed_state->last_used) > max_age_seconds) {
        MEM_freeN(collapsed_state->path);
        MEM_freeN(collapsed_state);
        BLI_freelinkN(&settings->catalog_collapsed_states, collapsed_state);
        removed_count++;

        // Limit cleanup per library to avoid blocking UI
        if (removed_count >= 100) {
          break;
        }
      }
    }
  }
}

void BKE_preferences_asset_browser_settings_cleanup_startup(UserDef *userdef)
{
  // Cleanup old records during startup (when user is not actively working)
  const uint32_t current_time = uint32_t(BLI_time_now_seconds());
  const uint32_t max_age_days = 30;  // Remove records older than 30 days
  const uint32_t max_age_seconds = max_age_days * 24 * 60 * 60;

  LISTBASE_FOREACH (bUserAssetBrowserSettings *, settings, &userdef->asset_browser_settings) {
    int removed_count = 0;
    LISTBASE_FOREACH_MUTABLE (
        AssetCatalogState *, collapsed_state, &settings->catalog_collapsed_states)
    {
      if (collapsed_state && (current_time - collapsed_state->last_used) > max_age_seconds) {
        MEM_freeN(collapsed_state->path);
        MEM_freeN(collapsed_state);
        BLI_freelinkN(&settings->catalog_collapsed_states, collapsed_state);
        removed_count++;

        // Limit cleanup per library to avoid blocking startup
        if (removed_count >= 50) {
          break;
        }
      }
    }
  }
}

void BKE_preferences_asset_browser_settings_cleanup(UserDef *userdef)
{
  BKE_preferences_asset_browser_settings_clear_all(userdef);
}

static void asset_browser_settings_free(bUserAssetBrowserSettings *settings)
{
  if (!settings) {
    return;
  }

  BKE_asset_catalog_path_list_free(settings->catalog_collapsed_states);
  MEM_freeN(settings);
}

void BKE_preferences_asset_browser_settings_clear_all(UserDef *userdef)
{
  LISTBASE_FOREACH_MUTABLE (
      bUserAssetBrowserSettings *, settings, &userdef->asset_browser_settings)
  {
    asset_browser_settings_free(settings);
    BLI_freelinkN(&userdef->asset_browser_settings, settings);
  }
}

void BKE_preferences_asset_browser_settings_blend_write(BlendWriter *writer,
                                                        const bUserAssetBrowserSettings *settings)
{
  BLO_write_struct(writer, bUserAssetBrowserSettings, settings);
  BKE_asset_catalog_path_list_blend_write(writer, settings->catalog_collapsed_states);
}

void BKE_preferences_asset_browser_settings_blend_read_data(BlendDataReader *reader,
                                                            bUserAssetBrowserSettings *settings)
{
  BKE_asset_catalog_path_list_blend_read_data(reader, settings->catalog_collapsed_states);

  // Validate and fix data after loading
  LISTBASE_FOREACH_MUTABLE (
      AssetCatalogState *, collapsed_state, &settings->catalog_collapsed_states)
  {
    if (!collapsed_state || !collapsed_state->path || !collapsed_state->path[0]) {
      // Remove invalid records
      if (collapsed_state) {
        MEM_freeN(collapsed_state->path);
        MEM_freeN(collapsed_state);
        BLI_freelinkN(&settings->catalog_collapsed_states, collapsed_state);
      }
    }
    else {
      // Restore hash if missing
      if (collapsed_state->path_hash == 0) {
        collapsed_state->path_hash = BLI_hash_string(collapsed_state->path);
      }
      // Set last_used if missing
      if (collapsed_state->last_used == 0) {
        collapsed_state->last_used = uint32_t(BLI_time_now_seconds());
      }
    }
  }
}

int BKE_preferences_asset_browser_settings_get_catalog_count(const UserDef *userdef,
                                                             const char *library_identifier)
{
  const bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);
  return settings ? BLI_listbase_count(&settings->catalog_collapsed_states) : 0;
}
