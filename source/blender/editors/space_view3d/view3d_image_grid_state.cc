/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_view3d_types.h"

#include "AS_asset_library.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

static Map<const View3D *, ImageGridUIState> g_image_grid_states;

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

  for (const AssetCatalogPathLink &path_link : v3d.image_grid_enabled_catalog_paths) {
    if (path_link.path && path_link.path[0] != '\0') {
      state.enabled_catalog_paths.add(path_link.path);
    }
  }
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
}

void image_grid_state_remove(const View3D &v3d)
{
  g_image_grid_states.remove(&v3d);
}

}  // namespace blender::ed::view3d
