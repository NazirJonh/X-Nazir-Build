/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_image_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_main.hh"

#include "MEM_guardedalloc.h"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

struct ImageGridStatesPerView3D {
  ImageGridUIState texture;
  ImageGridUIState mask;
  bool texture_initialized = false;
  bool mask_initialized = false;
  ImageGridDragScrollState drag_scroll;
  ImageGridWheelLatch wheel_latch;
};

/**
 * Return the per-View3D grid state stored in #View3D_Runtime, creating it on first access. Keeping
 * it on the View3D (instead of a global map keyed by raw pointer) ties its lifetime to the owner
 * and avoids aliasing when a freed View3D's address is reused.
 */
static ImageGridStatesPerView3D &image_grid_states_ensure(const View3D &v3d)
{
  /* Logically a mutable cache attached to the View3D; #image_grid_state_get takes it by const ref
   * to match call sites that only have a const View3D. */
  View3D &v3d_mut = const_cast<View3D &>(v3d);
  if (!v3d_mut.runtime.image_grid_state) {
    v3d_mut.runtime.image_grid_state = MEM_new<ImageGridStatesPerView3D>(__func__);
  }
  return *static_cast<ImageGridStatesPerView3D *>(v3d_mut.runtime.image_grid_state);
}

struct ImageGridView3DDNAFields {
  short library_type;
  int library_custom_index;
  ListBaseT<AssetCatalogPathLink> &legacy_enabled_catalog_paths;
  ListBaseT<ImageGridLibraryCatalogState> &library_catalog_states;
};

static ImageGridView3DDNAFields image_grid_view3d_dna_fields(View3D &v3d, const bool is_mask_slot)
{
  ImageGridSlotDNA &slot = is_mask_slot ? v3d.image_grid_mask : v3d.image_grid;
  return {slot.library_type,
          slot.library_custom_index,
          slot.enabled_catalog_paths_legacy,
          slot.library_catalog_states};
}

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
  state.filter.enabled_catalog_paths.clear();
  if (const Set<std::string> *paths = state.filter.enabled_catalogs_by_library.lookup_ptr(
          image_grid_library_enum_key(lib_ref)))
  {
    state.filter.enabled_catalog_paths = *paths;
  }
}

void image_grid_catalog_commit_active(ImageGridUIState &state)
{
  const int key = image_grid_library_enum_key(state.filter.lib_ref);
  if (state.filter.enabled_catalog_paths.is_empty()) {
    state.filter.enabled_catalogs_by_library.remove(key);
  }
  else {
    state.filter.enabled_catalogs_by_library.add_overwrite(key,
                                                           state.filter.enabled_catalog_paths);
  }
}

void image_grid_catalog_swap_library(ImageGridUIState &state,
                                     const AssetLibraryReference & /*old_lib_ref*/,
                                     const AssetLibraryReference &new_lib_ref)
{
  image_grid_catalog_commit_active(state);
  state.filter.lib_ref = new_lib_ref;
  image_grid_catalog_load_active(state, new_lib_ref);
}

static void image_grid_catalog_load_from_view3d_dna(ImageGridUIState &state,
                                                    const View3D & /*v3d*/,
                                                    const ImageGridView3DDNAFields &dna)
{
  state.filter.enabled_catalogs_by_library.clear();

  for (ImageGridLibraryCatalogState *libcat_state =
           static_cast<ImageGridLibraryCatalogState *>(dna.library_catalog_states.first);
       libcat_state;
       libcat_state = libcat_state->next)
  {
    const AssetLibraryReference lib_ref = image_grid_library_ref_from_filter(*libcat_state);
    Set<std::string> paths;
    for (AssetCatalogPathLink *path_link =
             static_cast<AssetCatalogPathLink *>(libcat_state->enabled_catalog_paths.first);
         path_link;
         path_link = path_link->next)
    {
      if (path_link->path && path_link->path[0] != '\0') {
        paths.add(path_link->path);
      }
    }
    if (!paths.is_empty()) {
      state.filter.enabled_catalogs_by_library.add_overwrite(image_grid_library_enum_key(lib_ref),
                                                             std::move(paths));
    }
  }

  /* Files saved before per-library filters: migrate the legacy single list. */
  if (state.filter.enabled_catalogs_by_library.is_empty()) {
    Set<std::string> legacy_paths;
    for (AssetCatalogPathLink *path_link =
             static_cast<AssetCatalogPathLink *>(dna.legacy_enabled_catalog_paths.first);
         path_link;
         path_link = path_link->next)
    {
      if (path_link->path && path_link->path[0] != '\0') {
        legacy_paths.add(path_link->path);
      }
    }
    if (!legacy_paths.is_empty()) {
      AssetLibraryReference lib_ref{};
      if (dna.library_type != 0) {
        lib_ref.type = eAssetLibraryType(dna.library_type);
        lib_ref.custom_library_index = dna.library_custom_index;
      }
      else {
        lib_ref = asset_system::current_file_library_reference();
      }
      state.filter.enabled_catalogs_by_library.add_overwrite(image_grid_library_enum_key(lib_ref),
                                                             std::move(legacy_paths));
    }
  }

  image_grid_catalog_load_active(state, state.filter.lib_ref);
}

static void image_grid_init_state_from_view3d_dna(ImageGridUIState &state,
                                                  const View3D &v3d,
                                                  const bool is_mask_slot)
{
  const ImageGridView3DDNAFields dna = image_grid_view3d_dna_fields(const_cast<View3D &>(v3d),
                                                                    is_mask_slot);

  if (dna.library_type != 0) {
    state.filter.lib_ref.type = eAssetLibraryType(dna.library_type);
    state.filter.lib_ref.custom_library_index = dna.library_custom_index;
  }
  else {
    state.filter.lib_ref = asset_system::current_file_library_reference();
  }

  image_grid_catalog_load_from_view3d_dna(state, v3d, dna);
  image_grid_catalog_sanitize_selection(state);
}

void image_grid_catalog_sanitize_selection(ImageGridUIState &state)
{
  if (state.filter.enabled_catalog_paths.is_empty()) {
    return;
  }

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      state.filter.lib_ref);
  if (!library) {
    return;
  }

  Set<std::string> valid_paths;
  for (const std::string &path : state.filter.enabled_catalog_paths) {
    if (library->catalog_service().find_catalog_by_path(path.c_str())) {
      valid_paths.add(path);
    }
  }

  if (valid_paths == state.filter.enabled_catalog_paths) {
    return;
  }

  state.filter.enabled_catalog_paths = std::move(valid_paths);
  image_grid_catalog_commit_active(state);
}

ImageGridUIState &image_grid_state_get(const View3D &v3d, const bool is_mask_slot)
{
  ImageGridStatesPerView3D &per_v3d = image_grid_states_ensure(v3d);
  ImageGridUIState &state = is_mask_slot ? per_v3d.mask : per_v3d.texture;
  bool &initialized = is_mask_slot ? per_v3d.mask_initialized : per_v3d.texture_initialized;
  if (initialized) {
    return state;
  }

  initialized = true;
  image_grid_init_state_from_view3d_dna(state, v3d, is_mask_slot);
  return state;
}

bool image_grid_is_mask_slot_from_context(const bContext &C)
{
  return CTX_data_int_get(&C, "image_grid_is_mask_slot") != 0;
}

ImageGridUIState &image_grid_state_get_from_context(const bContext &C)
{
  View3D *v3d = CTX_wm_view3d(&C);
  BLI_assert(v3d != nullptr);
  return image_grid_state_get(*v3d, image_grid_is_mask_slot_from_context(C));
}

void image_grid_state_reset_catalog(ImageGridUIState &state)
{
  state.filter.enabled_catalog_paths.clear();
  state.filter.enabled_catalogs_by_library.remove(
      image_grid_library_enum_key(state.filter.lib_ref));
}

ImageGridDragScrollState &image_grid_drag_scroll_state(View3D &v3d)
{
  return image_grid_states_ensure(v3d).drag_scroll;
}

ImageGridWheelLatch &image_grid_wheel_latch(View3D &v3d)
{
  return image_grid_states_ensure(v3d).wheel_latch;
}

void image_grid_state_remove(const View3D &v3d)
{
  View3D &v3d_mut = const_cast<View3D &>(v3d);
  if (v3d_mut.runtime.image_grid_state) {
    MEM_delete(static_cast<ImageGridStatesPerView3D *>(v3d_mut.runtime.image_grid_state));
    v3d_mut.runtime.image_grid_state = nullptr;
  }
}

/* -------------------------------------------------------------------- */
/** \name Image Grid Filtered Sequence
 * \{ */

bool image_grid_is_assignable_texture(const Image &image)
{
  if (ELEM(image.source, IMA_SRC_VIEWER, IMA_SRC_GENERATED)) {
    return false;
  }
  if (ELEM(image.type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
    return false;
  }
  return true;
}

int image_grid_foreach_filtered_item(
    Main &bmain,
    const AssetLibraryReference &lib_ref,
    const blender::Set<std::string> &enabled_catalog_paths,
    blender::FunctionRef<bool(const ImageGridFilteredItem &, int)> fn)
{
  const bool catalog_filtering_enabled = !enabled_catalog_paths.is_empty();

  /* Build catalog filters once from the enabled paths; reused for every asset. */
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  blender::Vector<asset_system::AssetCatalogFilter> catalog_filters;
  if (catalog_filtering_enabled && library) {
    catalog_filters.reserve(enabled_catalog_paths.size());
    for (const std::string &path : enabled_catalog_paths) {
      asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
          path.c_str());
      if (catalog) {
        catalog_filters.append(
            library->catalog_service().create_catalog_filter(catalog->catalog_id));
      }
    }
  }

  blender::Set<ID *> seen_ids;
  int filtered_index = 0;
  bool continue_iter = true;

  /* Phase 1: image assets from the library list. */
  if (library) {
    ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
      if (!continue_iter) {
        return false;
      }
      if (asset.get_id_type() != ID_IM) {
        return true;
      }
      /* Catalog filter: when enabled but filters could not be built (library still loading),
       * skip all assets so the display remains consistent with filter intent. */
      if (catalog_filtering_enabled) {
        if (catalog_filters.is_empty()) {
          return true;
        }
        bool in_any = false;
        for (const asset_system::AssetCatalogFilter &f : catalog_filters) {
          if (f.contains(asset.get_metadata().catalog_id)) {
            in_any = true;
            break;
          }
        }
        if (!in_any) {
          return true;
        }
      }
      /* Assignability check and dedup for locally-loaded assets. */
      if (ID *id = asset.local_id()) {
        if (GS(id->name) == ID_IM) {
          const Image *image = id_cast<const Image *>(id);
          if (!image_grid_is_assignable_texture(*image)) {
            return true;
          }
        }
        if (seen_ids.contains(id)) {
          return true;
        }
        seen_ids.add_new(id);
      }

      ImageGridFilteredItem item;
      item.asset = &asset;
      if (!fn(item, filtered_index)) {
        continue_iter = false;
      }
      filtered_index++;
      return continue_iter;
    });
  }

  /* Phase 2: non-asset images from the current file (LOCAL library only). */
  if (lib_ref.type == ASSET_LIBRARY_LOCAL) {
    ID *id;
    FOREACH_MAIN_ID_BEGIN (&bmain, id) {
      if (GS(id->name) != ID_IM) {
        continue;
      }
      if (id->asset_data) {
        /* Already iterated as an asset in phase 1. */
        continue;
      }
      if (seen_ids.contains(id)) {
        continue;
      }
      Image *image = id_cast<Image *>(id);
      if (!image_grid_is_assignable_texture(*image)) {
        continue;
      }
      seen_ids.add_new(id);

      if (continue_iter) {
        ImageGridFilteredItem item;
        item.image = image;
        if (!fn(item, filtered_index)) {
          continue_iter = false;
        }
      }
      filtered_index++;
    }
    FOREACH_MAIN_ID_END;
  }

  return filtered_index;
}

/** \} */

}  // namespace blender::ed::view3d
