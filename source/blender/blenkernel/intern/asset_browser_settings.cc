/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Per-library asset browser catalog collapse-state persistence in the user preferences.
 */

#include <optional>

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_asset.hh"
#include "BKE_preferences.h"

#include "MEM_guardedalloc.h"

#include "BLO_read_write.hh"

namespace blender {

/* Stable per-library identifier used as the key in #UserDef::asset_browser_settings. */
const char *BKE_preferences_asset_library_identifier_from_ref(
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
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(
          userdef, library_ref);
      return user_library ? user_library->name : "default";
    }
  }
  return "default";
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

bUserAssetBrowserSettings *BKE_preferences_asset_browser_settings_get_from_library_ref(
    UserDef *userdef, const AssetLibraryReference *library_ref)
{
  const char *library_identifier = BKE_preferences_asset_library_identifier_from_ref(userdef,
                                                                                     library_ref);
  bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);

  /* Auto-create an entry so the first collapse interaction has somewhere to save. */
  if (!settings && library_ref) {
    settings = asset_browser_settings_ensure(userdef, library_identifier);
  }

  return settings;
}

std::optional<bool> BKE_preferences_asset_browser_settings_is_catalog_collapsed(
    const UserDef *userdef, const char *library_identifier, const char *catalog_path)
{
  const bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(
      userdef, library_identifier);
  if (!settings) {
    return std::nullopt;
  }
  return BKE_asset_catalog_state_get_collapsed(settings->catalog_states, catalog_path);
}

void BKE_preferences_asset_browser_settings_set_catalog_collapsed(UserDef *userdef,
                                                                  const char *library_identifier,
                                                                  const char *catalog_path,
                                                                  const bool collapsed)
{
  if (!userdef || !library_identifier || !catalog_path || !catalog_path[0]) {
    return;
  }

  bUserAssetBrowserSettings *settings = asset_browser_settings_ensure(userdef, library_identifier);
  BKE_asset_catalog_state_set_collapsed(settings->catalog_states, catalog_path, collapsed);
}

void BKE_preferences_asset_browser_settings_rename_library(UserDef *userdef,
                                                           const char *old_name,
                                                           const char *new_name)
{
  if (!userdef || !old_name || !new_name || !old_name[0] || !new_name[0] ||
      STREQ(old_name, new_name))
  {
    return;
  }

  bUserAssetBrowserSettings *settings = BKE_preferences_asset_browser_settings_get(userdef,
                                                                                   old_name);
  if (!settings) {
    return;
  }

  /* The key may still be held by a removed library's leftovers; the live library wins. */
  if (bUserAssetBrowserSettings *stale = BKE_preferences_asset_browser_settings_get(userdef,
                                                                                    new_name))
  {
    BKE_asset_catalog_state_list_free(stale->catalog_states);
    BLI_remlink(&userdef->asset_browser_settings, stale);
    MEM_delete(stale);
  }

  STRNCPY(settings->library_name, new_name);
}

void BKE_preferences_asset_browser_settings_blend_write(BlendWriter *writer,
                                                        const bUserAssetBrowserSettings *settings)
{
  writer->write_struct(settings);
  BKE_asset_catalog_state_list_blend_write(writer, settings->catalog_states);
}

void BKE_preferences_asset_browser_settings_blend_read_data(BlendDataReader *reader,
                                                            bUserAssetBrowserSettings *settings)
{
  BKE_asset_catalog_state_list_blend_read_data(reader, settings->catalog_states);

  /* Drop entries that lost their path string on load. */
  for (AssetCatalogState &state : settings->catalog_states.items_mutable()) {
    if (!state.path || !state.path[0]) {
      MEM_delete(state.path);
      BLI_freelinkN(&settings->catalog_states, &state);
    }
  }
}

}  // namespace blender
