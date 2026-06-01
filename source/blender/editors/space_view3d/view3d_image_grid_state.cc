/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_asset_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_library.hh"

#include "BLI_map.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"

#include "ED_asset_library.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

static Map<const View3D *, ImageGridUIState> g_image_grid_states;

static int image_grid_library_enum_key(const AssetLibraryReference &lib_ref)
{
  return ed::asset::library_reference_to_enum_value(&lib_ref);
}

static AssetLibraryReference image_grid_library_ref_from_filter(
    const ImageGridLibraryCatalogState &libcat_state)
{
  return libcat_state.library_ref;
}

static void image_grid_catalog_load_active(ImageGridUIState &state,
                                           const AssetLibraryReference &lib_ref)
{
  state.enabled_catalog_paths.clear();
  if (const Set<std::string> *paths = state.enabled_catalogs_by_library.lookup_ptr(
          image_grid_library_enum_key(lib_ref)))
  {
    state.enabled_catalog_paths = *paths;
  }
}

void image_grid_catalog_commit_active(ImageGridUIState &state)
{
  const int key = image_grid_library_enum_key(state.lib_ref);
  if (state.enabled_catalog_paths.is_empty()) {
    state.enabled_catalogs_by_library.remove(key);
  }
  else {
    state.enabled_catalogs_by_library.add_overwrite(key, state.enabled_catalog_paths);
  }
}

void image_grid_catalog_swap_library(ImageGridUIState &state,
                                     const AssetLibraryReference & /*old_lib_ref*/,
                                     const AssetLibraryReference &new_lib_ref)
{
  image_grid_catalog_commit_active(state);
  state.lib_ref = new_lib_ref;
  image_grid_catalog_load_active(state, new_lib_ref);
}

static void image_grid_catalog_load_from_view3d_dna(ImageGridUIState &state, const View3D &v3d)
{
  state.enabled_catalogs_by_library.clear();

  for (ImageGridLibraryCatalogState *libcat_state = static_cast<ImageGridLibraryCatalogState *>(
           v3d.image_grid_library_catalog_states.first);
       libcat_state;
       libcat_state = libcat_state->next)
  {
    const AssetLibraryReference lib_ref = image_grid_library_ref_from_filter(*libcat_state);
    Set<std::string> paths;
    for (AssetCatalogPathLink *path_link = static_cast<AssetCatalogPathLink *>(
             libcat_state->enabled_catalog_paths.first);
         path_link;
         path_link = path_link->next)
    {
      if (path_link->path && path_link->path[0] != '\0') {
        paths.add(path_link->path);
      }
    }
    if (!paths.is_empty()) {
      state.enabled_catalogs_by_library.add_overwrite(image_grid_library_enum_key(lib_ref),
                                                    std::move(paths));
    }
  }

  /* Files saved before per-library filters: migrate the legacy single list. */
  if (state.enabled_catalogs_by_library.is_empty()) {
    Set<std::string> legacy_paths;
    for (AssetCatalogPathLink *path_link = static_cast<AssetCatalogPathLink *>(
             v3d.image_grid_enabled_catalog_paths.first);
         path_link;
         path_link = path_link->next)
    {
      if (path_link->path && path_link->path[0] != '\0') {
        legacy_paths.add(path_link->path);
      }
    }
    if (!legacy_paths.is_empty()) {
      AssetLibraryReference lib_ref{};
      if (v3d.image_grid_library_type != 0) {
        lib_ref.type = eAssetLibraryType(v3d.image_grid_library_type);
        lib_ref.custom_library_index = v3d.image_grid_library_custom_index;
      }
      else {
        lib_ref = asset_system::current_file_library_reference();
      }
      state.enabled_catalogs_by_library.add_overwrite(image_grid_library_enum_key(lib_ref),
                                                    std::move(legacy_paths));
    }
  }

  image_grid_catalog_load_active(state, state.lib_ref);
}

ImageGridUIState &image_grid_state_get(const View3D &v3d)
{
  const bool is_new = !g_image_grid_states.contains(&v3d);
  ImageGridUIState &state = g_image_grid_states.lookup_or_add_default(&v3d);
  if (!is_new) {
    return state;
  }

  /* New entry: load persistent state from View3D DNA. */
  if (v3d.image_grid_library_type != 0) {
    state.lib_ref.type = eAssetLibraryType(v3d.image_grid_library_type);
    state.lib_ref.custom_library_index = v3d.image_grid_library_custom_index;
  }
  else {
    state.lib_ref = asset_system::current_file_library_reference();
  }

  image_grid_catalog_load_from_view3d_dna(state, v3d);
  return state;
}

ImageGridUIState &image_grid_state_get_from_context(const bContext &C)
{
  View3D *v3d = CTX_wm_view3d(&C);
  BLI_assert(v3d != nullptr);
  return image_grid_state_get(*v3d);
}

void image_grid_state_reset_catalog(ImageGridUIState &state)
{
  state.enabled_catalog_paths.clear();
  state.enabled_catalogs_by_library.remove(image_grid_library_enum_key(state.lib_ref));
}

void image_grid_state_remove(const View3D &v3d)
{
  g_image_grid_states.remove(&v3d);
}

}  // namespace blender::ed::view3d
