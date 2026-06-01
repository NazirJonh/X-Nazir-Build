/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_screen_types.h"
#include "DNA_texture_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "BKE_asset.hh"
#include "BKE_asset_edit.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_view3d.hh"
#include "ED_asset_shelf.hh"

#include "AS_asset_catalog_path.hh"

#include "UI_interface_c.hh"

#include "intern/asset_library_reference.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

static bool image_grid_browse_popover_is_open(const bContext &C)
{
  return ui::region_popup_has_panel(&C, "ASSETSHELF_PT_popover_panel");
}

static bool image_grid_is_assignable_texture(const Image &image)
{
  if (ELEM(image.source, IMA_SRC_VIEWER, IMA_SRC_GENERATED)) {
    return false;
  }
  if (ELEM(image.type, IMA_TYPE_R_RESULT, IMA_TYPE_COMPOSITE)) {
    return false;
  }
  return true;
}

static bool image_grid_asset_passes_catalog_filter(
    const asset_system::AssetRepresentation &asset,
    const bool catalog_filtering_enabled,
    const Vector<asset_system::AssetCatalogFilter> &catalog_filters)
{
  if (!catalog_filtering_enabled) {
    return true;
  }
  if (catalog_filters.is_empty()) {
    return true;
  }
  for (const asset_system::AssetCatalogFilter &filter : catalog_filters) {
    if (filter.contains(asset.get_metadata().catalog_id)) {
      return true;
    }
  }
  return false;
}

static Vector<asset_system::AssetCatalogFilter> catalog_filters_for_state(
    const asset_system::AssetLibrary &library, const ImageGridUIState &state)
{
  Vector<asset_system::AssetCatalogFilter> filters;
  if (state.enabled_catalog_paths.is_empty()) {
    return filters;
  }
  filters.reserve(state.enabled_catalog_paths.size());
  for (const std::string &path : state.enabled_catalog_paths) {
    asset_system::AssetCatalog *catalog = library.catalog_service().find_catalog_by_path(
        path.c_str());
    if (!catalog) {
      continue;
    }
    filters.append(library.catalog_service().create_catalog_filter(catalog->catalog_id));
  }
  return filters;
}

static int image_grid_find_asset_filtered_index(const bContext &C,
                                              const ImageGridUIState &state,
                                              const std::string &asset_identifier)
{
  const AssetLibraryReference &lib_ref = state.lib_ref;
  const bool catalog_filtering_enabled = !state.enabled_catalog_paths.is_empty();

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      lib_ref);
  Vector<asset_system::AssetCatalogFilter> catalog_filters;
  if (library && catalog_filtering_enabled) {
    catalog_filters = catalog_filters_for_state(*library, state);
  }

  int filtered_index = 0;
  int result = -1;

  if (ed::asset::list::library_get_once_available(lib_ref)) {
    ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
      if (result >= 0) {
        return true;
      }
      if (asset.get_id_type() != ID_IM) {
        return true;
      }
      if (!image_grid_asset_passes_catalog_filter(
              asset, catalog_filtering_enabled, catalog_filters))
      {
        return true;
      }
      if (ID *id = asset.local_id()) {
        if (GS(id->name) == ID_IM) {
          const Image *image = reinterpret_cast<const Image *>(id);
          if (!image_grid_is_assignable_texture(*image)) {
            return true;
          }
        }
      }
      if (asset.library_relative_identifier() == asset_identifier) {
        result = filtered_index;
        return false;
      }
      filtered_index++;
      return true;
    });
  }

  if (result >= 0) {
    return result;
  }

  if (lib_ref.type != ASSET_LIBRARY_LOCAL) {
    return -1;
  }

  Main *bmain = CTX_data_main(&C);
  Set<ID *> seen_ids;
  filtered_index = 0;

  ID *id;
  FOREACH_MAIN_ID_BEGIN (bmain, id) {
    if (GS(id->name) != ID_IM) {
      continue;
    }
    if (id->asset_data) {
      continue;
    }
    if (seen_ids.contains(id)) {
      continue;
    }
    Image *image = reinterpret_cast<Image *>(id);
    if (!image_grid_is_assignable_texture(*image)) {
      continue;
    }
    seen_ids.add_new(id);

    const std::string identifier = image->id.name + 2;
    if (identifier == asset_identifier) {
      return filtered_index;
    }
    filtered_index++;
  }
  FOREACH_MAIN_ID_END;

  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pending state
 * \{ */

void image_grid_pending_clear(ImageGridUIState &state)
{
  state.pending_apply_after_popover = false;
  state.pending_lib_ref = {};
  state.pending_use_all_catalogs = false;
  state.pending_catalog_path.clear();
  state.pending_focus_asset_identifier.clear();
  state.pending_focus_filtered_index = -1;
}

void image_grid_request_scroll_to_asset(ImageGridUIState &state, const std::string &asset_identifier)
{
  state.focus_asset_identifier = asset_identifier;
}

bool image_grid_apply_focus_scroll(const bContext &C,
                                   View3D & /*v3d*/,
                                   ImageGridUIState &state,
                                   const int cols)
{
  if (state.focus_asset_identifier.empty()) {
    return true;
  }

  const int cols_clamped = max_ii(1, cols);

  /* Never block the draw thread waiting for a library load. #build_items already called
   * #storage_fetch (async); the #NC_ASSET notifier will trigger another redraw once ready. */
  if (!ed::asset::list::library_get_once_available(state.lib_ref)) {
    return false;
  }

  const int filtered_index = image_grid_find_asset_filtered_index(
      C, state, state.focus_asset_identifier);
  if (filtered_index < 0) {
    /* Asset absent from the filtered list. If the library is fully loaded there is no point
     * retrying — clear the request so future draws are not wasted. */
    if (ed::asset::list::is_loaded(&state.lib_ref)) {
      state.focus_asset_identifier.clear();
    }
    return false;
  }

  state.scroll_row = filtered_index / cols_clamped;
  /* Do NOT clamp here: cached_item_count may be stale (from a previous library). A premature
   * clamp would cap scroll_row below the true max_scroll_row, causing the wrong rows to be
   * built. The post-build clamp in build_image_grid uses the correct cached_item_count. */
  state.focus_asset_identifier.clear();
  return true;
}

bool image_grid_asset_is_visible_in_state(const ImageGridUIState &state,
                                          const AssetLibraryReference &asset_lib_ref,
                                          const std::optional<std::string> &asset_catalog_path)
{
  if (!(state.lib_ref == asset_lib_ref)) {
    return false;
  }
  if (state.enabled_catalog_paths.is_empty()) {
    return true;
  }
  if (!asset_catalog_path) {
    return false;
  }
  return state.enabled_catalog_paths.contains(*asset_catalog_path);
}

void image_grid_state_persist_to_view3d(View3D &v3d, ImageGridUIState &state)
{
  v3d.image_grid_library_type = short(state.lib_ref.type);
  v3d.image_grid_library_custom_index = state.lib_ref.custom_library_index;

  image_grid_catalog_commit_active(state);

  while (ImageGridLibraryCatalogState *libcat_state = static_cast<ImageGridLibraryCatalogState *>(
             BLI_pophead(&v3d.image_grid_library_catalog_states)))
  {
    BKE_asset_catalog_path_list_free(libcat_state->enabled_catalog_paths);
    MEM_delete(libcat_state);
  }

  BKE_asset_catalog_path_list_free(v3d.image_grid_enabled_catalog_paths);

  for (const auto item : state.enabled_catalogs_by_library.items()) {
    const Set<std::string> &paths = item.value;
    if (paths.is_empty()) {
      continue;
    }

    const AssetLibraryReference lib_ref = ed::asset::library_reference_from_enum_value(
        item.key);
    ImageGridLibraryCatalogState *libcat_state = MEM_new<ImageGridLibraryCatalogState>(
        __func__);
    libcat_state->library_ref = lib_ref;
    for (const std::string &path : paths) {
      BKE_asset_catalog_path_list_add_path(libcat_state->enabled_catalog_paths, path.c_str());
    }
    BLI_addtail(&v3d.image_grid_library_catalog_states, libcat_state);
  }
}

void image_grid_pending_schedule_from_asset(ImageGridUIState &state,
                                            const AssetLibraryReference &lib_ref,
                                            const std::optional<std::string> &catalog_path,
                                            const std::string &asset_identifier)
{
  state.pending_apply_after_popover = true;
  state.pending_lib_ref = lib_ref;
  state.pending_use_all_catalogs = !catalog_path.has_value();
  state.pending_catalog_path = catalog_path.value_or("");
  state.pending_focus_asset_identifier = asset_identifier;
  state.pending_focus_filtered_index = -1;
}

std::optional<std::string> image_grid_catalog_path_for_asset(
    const asset_system::AssetRepresentation &asset, const AssetLibraryReference &lib_ref)
{
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      lib_ref);
  if (!library) {
    return std::nullopt;
  }
  const asset_system::CatalogID &catalog_id = asset.get_metadata().catalog_id;
  const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog(catalog_id);
  if (!catalog) {
    return std::nullopt;
  }
  return catalog->path.str();
}

void image_grid_pending_apply_if_ready(bContext &C, View3D &v3d)
{
  ImageGridUIState &state = image_grid_state_get(v3d);
  if (!state.pending_apply_after_popover) {
    return;
  }
  if (image_grid_browse_popover_is_open(C)) {
    return;
  }

  const AssetLibraryReference old_lib_ref = state.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, state.pending_lib_ref);
  state.enabled_catalog_paths.clear();
  if (!state.pending_use_all_catalogs && !state.pending_catalog_path.empty()) {
    state.enabled_catalog_paths.add(state.pending_catalog_path);
  }
  image_grid_catalog_commit_active(state);

  ed::asset::list::storage_fetch(&state.lib_ref, &C);

  if (!state.pending_focus_asset_identifier.empty()) {
    state.focus_asset_identifier = state.pending_focus_asset_identifier;
  }

  image_grid_state_persist_to_view3d(v3d, state);
  image_grid_pending_clear(state);
  image_grid_notify_change(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shelf sync
 * \{ */

static const char *IMAGE_TEXTURE_SHELF_IDNAME = "VIEW3D_AST_image_texture";

/* Global storage for the AssetWeakReference returned by the shelf type callback.
 * The callback must return a pointer that stays valid until the next call. */
static AssetWeakReference g_image_shelf_active_asset_storage;

static bool image_grid_asset_represents_image(const asset_system::AssetRepresentation &asset,
                                              const Image &image)
{
  if (const ID *local_id = asset.local_id()) {
    return local_id == &image.id;
  }
  const std::string asset_path = asset.full_path();
  if (asset_path.empty()) {
    return false;
  }
  return BLI_path_cmp_normalized(asset_path.c_str(), image.filepath) == 0;
}

static const Image *image_grid_active_image_from_context(const bContext &C)
{
  const PointerRNA target_ptr = CTX_data_pointer_get(&C, "image_grid_target");
  if (!target_ptr.data || !target_ptr.owner_id || GS(target_ptr.owner_id->name) != ID_BR) {
    return nullptr;
  }
  const MTex *mtex = static_cast<const MTex *>(target_ptr.data);
  if (!mtex->tex || mtex->tex->type != TEX_IMAGE) {
    return nullptr;
  }
  return mtex->tex->ima;
}

std::optional<AssetWeakReference> image_grid_shelf_active_asset_weak_ref(
    const bContext &C, const AssetLibraryReference &library_ref)
{
  const Image *image = image_grid_active_image_from_context(C);
  if (!image) {
    return std::nullopt;
  }

  /* First try to find the matching AssetRepresentation in the library (gives the correct
   * library-relative weak ref that matches shelf item identifiers). */
  std::optional<AssetWeakReference> weak_ref;
  ed::asset::list::iterate(library_ref, [&](asset_system::AssetRepresentation &asset) {
    if (image_grid_asset_represents_image(asset, *image)) {
      weak_ref = asset.make_weak_reference();
      return false;
    }
    return true;
  });

  if (weak_ref) {
    return weak_ref;
  }
  /* Fallback: local-file image — produces "Image/<id-name>" format. */
  return bke::asset_edit_weak_reference_from_id(image->id);
}

static const AssetWeakReference *image_texture_shelf_active_asset_type_callback(
    const AssetShelfType * /*shelf_type*/, const bContext *C)
{
  if (!C) {
    return nullptr;
  }

  std::optional<AssetWeakReference> weak_ref;
  if (image_grid_active_image_from_context(*C)) {
    AssetShelfType *type = ed::asset::shelf::type_find_from_idname(IMAGE_TEXTURE_SHELF_IDNAME);
    if (!type) {
      return nullptr;
    }
    AssetShelf *shelf = ed::asset::shelf::popup_shelf_get_or_create(*C, *type);
    if (!shelf) {
      return nullptr;
    }
    weak_ref = image_grid_shelf_active_asset_weak_ref(
        *C, shelf->settings.asset_library_reference);
  }
  else if (const View3D *v3d = CTX_wm_view3d(C)) {
    const ImageGridUIState &state = image_grid_state_get(*v3d);
    if (state.shelf_active_asset_valid) {
      weak_ref = state.shelf_active_asset;
    }
  }

  if (!weak_ref) {
    return nullptr;
  }
  g_image_shelf_active_asset_storage = *weak_ref;
  return &g_image_shelf_active_asset_storage;
}

void image_grid_shelf_sync_register()
{
  AssetShelfType *type = ed::asset::shelf::type_find_from_idname(IMAGE_TEXTURE_SHELF_IDNAME);
  if (!type) {
    return;
  }
  /* Re-apply after Python class re-registration replaces the #AssetShelfType. */
  type->get_active_asset_from_context = image_texture_shelf_active_asset_type_callback;
}

void image_grid_sync_shelf_from_state(AssetShelf &shelf, const ImageGridUIState &state)
{
  shelf.settings.asset_library_reference = state.lib_ref;
  ed::asset::shelf::settings_ensure_valid_library_ref(shelf.settings);

  if (state.enabled_catalog_paths.is_empty()) {
    ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
    return;
  }
  if (state.enabled_catalog_paths.size() == 1) {
    const std::string &path = *state.enabled_catalog_paths.begin();
    ed::asset::shelf::settings_set_active_catalog(
        shelf.settings, asset_system::AssetCatalogPath(path));
    return;
  }
  ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
}

void image_grid_sync_state_from_shelf(ImageGridUIState &state, const AssetShelf &shelf)
{
  const AssetLibraryReference new_lib_ref = shelf.settings.asset_library_reference;
  const AssetLibraryReference old_lib_ref = state.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, new_lib_ref);
  state.enabled_catalog_paths.clear();
  if (!ed::asset::shelf::settings_is_all_catalog_active(shelf.settings)) {
    if (shelf.settings.active_catalog_path && shelf.settings.active_catalog_path[0] != '\0') {
      state.enabled_catalog_paths.add(shelf.settings.active_catalog_path);
    }
  }
  image_grid_catalog_commit_active(state);
  state.scroll_row = 0;
}

AssetShelf *image_grid_prepare_browse_shelf(const bContext &C,
                                            ImageGridUIState &state,
                                            const char *shelf_idname)
{
  image_grid_shelf_sync_register();

  AssetShelfType *shelf_type = ed::asset::shelf::type_find_from_idname(shelf_idname);
  if (!shelf_type) {
    return nullptr;
  }
  AssetShelf *shelf = ed::asset::shelf::popup_shelf_get_or_create(C, *shelf_type);
  if (!shelf) {
    return nullptr;
  }
  image_grid_sync_shelf_from_state(*shelf, state);
  if (View3D *v3d = CTX_wm_view3d(&C)) {
    shelf->settings.preview_size = image_grid_preview_size_get(*v3d);
  }

  if (std::optional<AssetWeakReference> weak_ref = image_grid_shelf_active_asset_weak_ref(
          C, state.lib_ref))
  {
    state.shelf_active_asset = *weak_ref;
    state.shelf_active_asset_valid = true;
  }
  else {
    state.shelf_active_asset_valid = false;
  }

  ed::asset::shelf::ensure_asset_library_fetched(C, *shelf_type);
  return shelf;
}

/** \} */

}  // namespace blender::ed::view3d
