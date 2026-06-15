/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "DNA_ID.h"
#include "DNA_image_types.h"

#include "BKE_asset.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_preview_image.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_library.hh"
#include "ED_asset_mark_clear.hh"

namespace blender::ed::asset {

bool image_can_be_asset(const Image *image)
{
  if (!image) {
    return false;
  }
  const ID *id = &image->id;
  if (id->asset_data) {
    return false;
  }
  if (!BKE_id_can_be_asset(id)) {
    return false;
  }
  if (ELEM(image->source, IMA_SRC_VIEWER, IMA_SRC_GENERATED)) {
    return false;
  }
  if (ELEM(image->type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
    return false;
  }
  return true;
}

bool image_mark_as_asset(Image *image)
{
  if (!image_can_be_asset(image)) {
    return false;
  }

  ID *id = &image->id;
  if (!mark_id(id)) {
    return false;
  }

  asset_system::AssetLibrary *library = AS_asset_library_load(
      G_MAIN, asset_system::current_file_library_reference());
  if (library) {
    const asset_system::AssetCatalogPath catalog_path("Images");
    const asset_system::AssetCatalog &catalog = library_ensure_catalogs_in_path(*library,
                                                                                catalog_path);
    BKE_asset_metadata_catalog_id_set(
        id->asset_data, catalog.catalog_id, catalog.simple_name.c_str());
  }

  BKE_previewimg_id_ensure(id);

  return true;
}

}  // namespace blender::ed::asset
