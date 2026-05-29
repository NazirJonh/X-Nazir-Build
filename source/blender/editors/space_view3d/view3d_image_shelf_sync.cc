/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_screen_types.h"

#include "ED_view3d.hh"
#include "ED_asset_shelf.hh"

#include "AS_asset_catalog_path.hh"

namespace blender::ed::view3d {

void image_grid_sync_shelf_from_state(AssetShelf &shelf, const ImageGridUIState &state)
{
  shelf.settings.asset_library_reference = state.lib_ref;
  ed::asset::shelf::settings_ensure_valid_library_ref(shelf.settings);

  if (state.enabled_catalog_paths.is_empty()) {
    ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
    return;
  }
  if (state.enabled_catalog_paths.size() == 1) {
    const std::string &path = *state.enabled_catalog_paths.begin();
    ed::asset::shelf::settings_set_active_catalog(
        shelf.settings, asset_system::AssetCatalogPath(path));
    return;
  }
  /* Multiple catalog paths in grid → show all in shelf (single-select model). */
  ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
}

void image_grid_sync_state_from_shelf(ImageGridUIState &state, const AssetShelf &shelf)
{
  state.lib_ref = shelf.settings.asset_library_reference;
  state.enabled_catalog_paths.clear();
  if (!ed::asset::shelf::settings_is_all_catalog_active(shelf.settings)) {
    if (shelf.settings.active_catalog_path && shelf.settings.active_catalog_path[0] != '\0') {
      state.enabled_catalog_paths.add(shelf.settings.active_catalog_path);
    }
  }
  state.scroll_row = 0;
}

AssetShelf *image_grid_prepare_browse_shelf(const bContext &C,
                                            ImageGridUIState &state,
                                            const char *shelf_idname)
{
  AssetShelfType *shelf_type = ed::asset::shelf::type_find_from_idname(shelf_idname);
  if (!shelf_type) {
    return nullptr;
  }
  AssetShelf *shelf = ed::asset::shelf::popup_shelf_get_or_create(C, *shelf_type);
  if (!shelf) {
    return nullptr;
  }
  image_grid_sync_shelf_from_state(*shelf, state);
  ed::asset::shelf::ensure_asset_library_fetched(C, *shelf_type);
  return shelf;
}

}  // namespace blender::ed::view3d
