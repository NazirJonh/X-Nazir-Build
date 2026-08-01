/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 *
 * Shared free/duplicate/blend_read/blend_write logic for #ImageGridSlotDNA,
 * factored out so both #View3D (its own two slots) and #SpaceImage can call
 * the same code instead of duplicating it.
 */

#include "DNA_view3d_types.h"

#include "BKE_asset.hh"

#include "BLI_listbase.h"

#include "BLO_read_write.hh"

#include "MEM_guardedalloc.h"

#include "ED_image_grid.hh"

namespace blender::ed::image_grid {

void image_grid_slot_dna_free(ImageGridSlotDNA &slot)
{
  BKE_asset_catalog_path_list_free(slot.enabled_catalog_paths_legacy);
  while (ImageGridLibraryCatalogState *libcat_state =
             static_cast<ImageGridLibraryCatalogState *>(BLI_pophead(&slot.library_catalog_states)))
  {
    BKE_asset_catalog_path_list_free(libcat_state->enabled_catalog_paths);
    MEM_delete(libcat_state);
  }
}

void image_grid_slot_dna_duplicate(ImageGridSlotDNA &dst, const ImageGridSlotDNA &src)
{
  dst.rows = src.rows;
  dst.catalog_mode = src.catalog_mode;
  dst.library_ref = src.library_ref;
  dst.library_type_legacy = src.library_type_legacy;
  dst.library_custom_index_legacy = src.library_custom_index_legacy;
  dst.enabled_catalog_paths_legacy = BKE_asset_catalog_path_list_duplicate(
      src.enabled_catalog_paths_legacy);

  BLI_listbase_clear(&dst.library_catalog_states);
  for (const ImageGridLibraryCatalogState &libcat_state_src : src.library_catalog_states) {
    ImageGridLibraryCatalogState *libcat_state_dst = MEM_new<ImageGridLibraryCatalogState>(
        __func__);
    libcat_state_dst->library_ref = libcat_state_src.library_ref;
    libcat_state_dst->enabled_catalog_paths = BKE_asset_catalog_path_list_duplicate(
        libcat_state_src.enabled_catalog_paths);
    BLI_addtail(&dst.library_catalog_states, libcat_state_dst);
  }
}

void image_grid_slot_dna_blend_read(BlendDataReader *reader, ImageGridSlotDNA &slot)
{
  BKE_asset_catalog_path_list_blend_read_data(reader, slot.enabled_catalog_paths_legacy);
  BLO_read_struct_list(reader, ImageGridLibraryCatalogState, &slot.library_catalog_states);
  for (ImageGridLibraryCatalogState &libcat_state : slot.library_catalog_states) {
    BKE_asset_catalog_path_list_blend_read_data(reader, libcat_state.enabled_catalog_paths);
  }
}

void image_grid_slot_dna_blend_write(BlendWriter *writer, const ImageGridSlotDNA &slot)
{
  BKE_asset_catalog_path_list_blend_write(writer, slot.enabled_catalog_paths_legacy);
  for (const ImageGridLibraryCatalogState &libcat_state : slot.library_catalog_states) {
    writer->write_struct(&libcat_state);
    BKE_asset_catalog_path_list_blend_write(writer, libcat_state.enabled_catalog_paths);
  }
}

}  // namespace blender::ed::image_grid
