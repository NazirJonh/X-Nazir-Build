/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_asset_shelf.hh"

#include "BLO_read_write.hh"

#include "BLI_listbase.h"

#include "DNA_screen_types.h"

#include "MEM_guardedalloc.h"

namespace blender {

void BKE_asset_shelf_library_catalog_state_list_free(
    ListBaseT<AssetShelfLibraryCatalogState> &library_catalog_state_list)
{
  while (AssetShelfLibraryCatalogState *state = static_cast<AssetShelfLibraryCatalogState *>(
             BLI_pophead(&library_catalog_state_list)))
  {
    MEM_delete(state);
  }
}

void BKE_asset_shelf_library_catalog_state_list_duplicate(
    ListBaseT<AssetShelfLibraryCatalogState> &dest_list,
    const ListBaseT<AssetShelfLibraryCatalogState> &src_list)
{
  BKE_asset_shelf_library_catalog_state_list_free(dest_list);
  for (const AssetShelfLibraryCatalogState &state_src : src_list) {
    AssetShelfLibraryCatalogState *state_dst = MEM_new<AssetShelfLibraryCatalogState>(__func__);
    state_dst->library_ref = state_src.library_ref;
    state_dst->active_catalog_id = state_src.active_catalog_id;
    BLI_addtail(&dest_list, state_dst);
  }
}

void BKE_asset_shelf_library_catalog_state_list_blend_write(
    BlendWriter *writer,
    const ListBaseT<AssetShelfLibraryCatalogState> &library_catalog_state_list)
{
  for (const AssetShelfLibraryCatalogState &state : library_catalog_state_list) {
    writer->write_struct(&state);
  }
}

void BKE_asset_shelf_library_catalog_state_list_blend_read_data(
    BlendDataReader *reader, ListBaseT<AssetShelfLibraryCatalogState> &library_catalog_state_list)
{
  BLO_read_struct_list(reader, AssetShelfLibraryCatalogState, &library_catalog_state_list);
}

}  // namespace blender
