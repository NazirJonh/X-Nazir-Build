/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_view3d_types.h"

#include "AS_asset_library.hh"

#include "BLI_map.hh"

#include "BKE_context.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

static Map<const View3D *, ImageGridUIState> g_image_grid_states;

ImageGridUIState &image_grid_state_get(const View3D &v3d)
{
  ImageGridUIState &state = g_image_grid_states.lookup_or_add_default(&v3d);
  if (state.lib_ref.type == eAssetLibraryType(0)) {
    state.lib_ref = asset_system::current_file_library_reference();
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
  state.active_catalog_path.clear();
}

void image_grid_state_remove(const View3D &v3d)
{
  g_image_grid_states.remove(&v3d);
}

}  // namespace blender::ed::view3d
