/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ED_asset_catalog.hh"
#include "ED_asset_list.hh"

#include "DNA_asset_types.h"
#include "DNA_uuid_types.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"

namespace blender::ed::asset {

AssetCatalogValidation ED_asset_catalog_validate(const AssetLibraryReference &library_ref,
                                                 bUUID candidate_catalog_id)
{
  if (!list::is_loaded(&library_ref)) {
    return AssetCatalogValidation::LibraryNotYetLoaded;
  }

  const asset_system::AssetLibrary *library = list::library_get_once_available(library_ref);
  if (!library) {
    return AssetCatalogValidation::LibraryNotYetLoaded;
  }

  const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog(
      asset_system::CatalogID(candidate_catalog_id));
  return catalog ? AssetCatalogValidation::Valid : AssetCatalogValidation::NotFound;
}

}  // namespace blender::ed::asset
