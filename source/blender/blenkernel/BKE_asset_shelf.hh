/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Blend I/O and lifecycle for #AssetShelfLibraryCatalogState (asset shelf UI state in DNA).
 */

#pragma once

#include "DNA_screen_types.h"

namespace blender {

struct BlendDataReader;
struct BlendWriter;

void BKE_asset_shelf_library_catalog_state_list_free(
    ListBaseT<AssetShelfLibraryCatalogState> &library_catalog_state_list);
void BKE_asset_shelf_library_catalog_state_list_duplicate(
    ListBaseT<AssetShelfLibraryCatalogState> &dest_list,
    const ListBaseT<AssetShelfLibraryCatalogState> &src_list);
void BKE_asset_shelf_library_catalog_state_list_blend_write(
    BlendWriter *writer,
    const ListBaseT<AssetShelfLibraryCatalogState> &library_catalog_state_list);
void BKE_asset_shelf_library_catalog_state_list_blend_read_data(
    BlendDataReader *reader, ListBaseT<AssetShelfLibraryCatalogState> &library_catalog_state_list);

}  // namespace blender
