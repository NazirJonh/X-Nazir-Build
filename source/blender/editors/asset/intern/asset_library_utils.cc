/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "AS_asset_representation.hh"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_preferences.h"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"
#include "ED_view3d.hh"

#include "BLI_listbase.h"
#include "BLI_string_ref.hh"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "RNA_access.hh"
#include "WM_api.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"

namespace blender::ed::asset {

static asset_system::AssetCatalog &library_ensure_catalog(
    asset_system::AssetLibrary &library, const asset_system::AssetCatalogPath &path)
{
  asset_system::AssetCatalogService &catalog_service = library.catalog_service();
  if (asset_system::AssetCatalog *catalog = catalog_service.find_catalog_by_path(path)) {
    return *catalog;
  }
  asset_system::AssetCatalog *new_catalog = catalog_service.create_catalog(path);
  catalog_service.tag_has_unsaved_changes(new_catalog);
  return *new_catalog;
}

asset_system::AssetCatalog &library_ensure_catalogs_in_path(
    asset_system::AssetLibrary &library, const asset_system::AssetCatalogPath &path)
{
  /* Adding multiple catalogs in a path at a time with #AssetCatalogService::create_catalog()
   * doesn't work; add each potentially new catalog in the hierarchy manually here. */
  asset_system::AssetCatalogPath parent = "";
  path.iterate_components([&](StringRef component_name, bool /*is_last_component*/) {
    library_ensure_catalog(library, parent / component_name);
    parent = parent / component_name;
  });
  return *library.catalog_service().find_catalog_by_path(path);
}

AssetLibraryReference user_library_to_library_ref(const bUserAssetLibrary &user_library)
{
  AssetLibraryReference library_ref{};
  BKE_preferences_asset_library_reference_set(&U, &library_ref, &user_library);
  return library_ref;
}

LibraryRefStatus library_reference_ensure_resolved(AssetLibraryReference &library_ref)
{
  if (library_ref.type != ASSET_LIBRARY_CUSTOM) {
    return LibraryRefStatus::Ok;
  }

  const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(&U,
                                                                                      &library_ref);
  if (!user_library) {
    /* Leave the reference alone: the name is the only thing left to tell the user which library
     * they are missing. */
    return LibraryRefStatus::Missing;
  }

  /* Refresh the derived members. Deliberately not tagging the file modified: this is a cache
   * update, and tagging would mark a file dirty merely for opening it. It reaches disk only if the
   * user saves for their own reasons. */
  BKE_preferences_asset_library_reference_set(&U, &library_ref, user_library);
  return LibraryRefStatus::Ok;
}

void foreach_library_reference(Main &bmain, FunctionRef<void(AssetLibraryReference &)> fn)
{
  for (wmWindowManager &wm : bmain.wm) {
    fn(wm.id_browser_asset_library_ref);
  }

  for (WorkSpace &workspace : bmain.workspaces) {
    fn(workspace.asset_library_ref);
  }

  for (bScreen &screen : bmain.screens) {
    for (ScrArea &area : screen.areabase) {
      for (SpaceLink &sl : area.spacedata) {
        if (sl.spacetype == SPACE_FILE) {
          SpaceFile &sfile = reinterpret_cast<SpaceFile &>(sl);
          if (sfile.asset_params) {
            fn(sfile.asset_params->asset_library_ref);
          }
        }
        else if (sl.spacetype == SPACE_VIEW3D) {
          View3D &v3d = reinterpret_cast<View3D &>(sl);
          for (ImageGridSlotDNA *slot : {&v3d.image_grid, &v3d.image_grid_mask}) {
            fn(slot->library_ref);
            for (ImageGridLibraryCatalogState &state : slot->library_catalog_states) {
              fn(state.library_ref);
            }
          }
          /* Also update the runtime cache (Task 6's #ImageGridUIState), which is seeded from DNA
           * once and never automatically re-synced -- without this, a rename while the grid is
           * open would leave the live filter pointing at the old name until the file is reloaded. */
          ed::view3d::image_grid_foreach_live_library_ref(v3d, fn);
        }

        /* Mirrors #type_unlink()'s walk: the active space's regions live on #ScrArea, every other
         * space's on its own #SpaceLink. */
        ListBaseT<ARegion> *regionbase = (&sl == area.spacedata.first) ? &area.regionbase :
                                                                          &sl.regionbase;
        for (ARegion &region : *regionbase) {
          if (region.regiontype != RGN_TYPE_ASSET_SHELF) {
            continue;
          }
          RegionAssetShelf *shelf_regiondata = RegionAssetShelf::get_from_asset_shelf_region(
              region);
          if (!shelf_regiondata) {
            continue;
          }
          for (AssetShelf &shelf : shelf_regiondata->shelves) {
            fn(shelf.settings.asset_library_reference);
          }
        }
      }
    }
  }

  shelf::popup_shelves_foreach_library_ref(fn);
}

void library_references_rename(Main &bmain,
                               const StringRefNull old_name,
                               const StringRefNull new_name)
{
  /* Not an #AssetLibraryReference: the asset browser's saved collapse state is keyed by the
   * library identifier string (see #BKE_preferences_asset_library_identifier_from_ref), which for
   * a custom library *is* its name. It lives in the Preferences, out of reach of the walk below. */
  BKE_preferences_asset_browser_settings_rename_library(&U, old_name.c_str(), new_name.c_str());

  foreach_library_reference(bmain, [&](AssetLibraryReference &library_ref) {
    if (library_ref.type != ASSET_LIBRARY_CUSTOM) {
      return;
    }
    if (old_name != library_ref.custom_library_name) {
      return;
    }
    new_name.copy_utf8_truncated(library_ref.custom_library_name);
  });
}

void refresh_asset_library(const bContext *C, const AssetLibraryReference &library_ref)
{
  asset::list::clear(&library_ref, C);
  /* TODO: Should the all library reference be automatically cleared? */
  AssetLibraryReference all_lib_ref = asset_system::all_library_reference();
  asset::list::clear(&all_lib_ref, C);
}

void refresh_asset_library(const bContext *C, const bUserAssetLibrary &user_library)
{
  refresh_asset_library(C, user_library_to_library_ref(user_library));
}

void refresh_asset_library_from_asset(const bContext *C,
                                      const asset_system::AssetRepresentation &asset)
{
  if (std::optional<AssetLibraryReference> library_ref =
          asset.owner_asset_library().library_reference())
  {
    refresh_asset_library(C, *library_ref);
  }
}

}  // namespace blender::ed::asset
