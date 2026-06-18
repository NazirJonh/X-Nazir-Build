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
#include "BKE_paint.hh"
#include "BKE_screen.hh"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"
#include "ED_view3d.hh"

#include "AS_asset_catalog_path.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "intern/asset_library_reference.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

static int image_grid_cols_clamp(const int cols)
{
  return clamp_i(cols, 1, 16);
}

/* Map a grid layout to its per-layout scroll/focus bucket index, keyed by column count only.
 *
 * The row count (grid height) deliberately does NOT take part in the key. Resizing the grip changes
 * only the visible row count; if the bucket were keyed by rows too, each height would restore its
 * own saved scroll, so dragging the grip would snap the grid to a stale row (typically the top)
 * instead of keeping the current first visible row. Keying by columns still separates the N-Panel
 * from the (usually narrower) Texture popover — the distinction that genuinely needs an independent
 * scroll position — while a height change pins the displayed rows and only re-clips. */
static int image_grid_layout_bucket_index(const int cols, const int /*rows*/)
{
  return image_grid_cols_clamp(cols) - 1;
}

static void image_grid_focus_reset_applied(ImageGridViewport &viewport)
{
  for (bool &applied : viewport.focus_applied_by_layout) {
    applied = false;
  }
}

void image_grid_focus_clear(ImageGridViewport &viewport)
{
  viewport.focus_asset_identifier.clear();
  image_grid_focus_reset_applied(viewport);
}

void image_grid_focus_mark_applied(ImageGridViewport &viewport, const int cols, const int rows)
{
  viewport.focus_applied_by_layout[image_grid_layout_bucket_index(cols, rows)] = true;
}

void image_grid_viewport_restore_scroll_for_layout(ImageGridViewport &viewport,
                                                   const int cols,
                                                   const int rows)
{
  const int idx = image_grid_layout_bucket_index(cols, rows);
  if (viewport.scroll_row_by_layout_init[idx]) {
    viewport.scroll_row = viewport.scroll_row_by_layout[idx];
    viewport.scroll_offset_px = viewport.scroll_offset_px_by_layout[idx];
  }
}

void image_grid_viewport_store_scroll_for_layout(ImageGridViewport &viewport,
                                                 const int cols,
                                                 const int rows)
{
  const int idx = image_grid_layout_bucket_index(cols, rows);
  viewport.scroll_row_by_layout[idx] = viewport.scroll_row;
  viewport.scroll_offset_px_by_layout[idx] = viewport.scroll_offset_px;
  viewport.scroll_row_by_layout_init[idx] = true;
}

static bool image_grid_browse_popover_is_open(const bContext &C)
{
  return ui::region_popup_has_panel(CTX_wm_region_popup(&C), "ASSETSHELF_PT_popover_panel");
}

static int image_grid_find_asset_filtered_index(const bContext &C,
                                                const ImageGridUIState &state,
                                                const std::string &asset_identifier)
{
  Main *bmain = CTX_data_main(&C);
  int result = -1;
  image_grid_foreach_filtered_item(
      *bmain,
      state.filter.lib_ref,
      state.filter.enabled_catalog_paths,
      [&](const ImageGridFilteredItem &item, int filtered_index) -> bool {
        const bool matches = item.asset ?
                                 item.asset->library_relative_identifier() == asset_identifier :
                                 asset_identifier == (item.image->id.name + 2);
        if (matches) {
          result = filtered_index;
          return false;
        }
        return true;
      });
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pending state
 * \{ */

void image_grid_pending_clear(ImageGridUIState &state)
{
  state.pending.apply_after_popover = false;
  state.pending.lib_ref = {};
  state.pending.use_all_catalogs = false;
  state.pending.catalog_path.clear();
  state.pending.focus_asset_identifier.clear();
  state.pending.focus_filtered_index = -1;
}

void image_grid_request_scroll_to_asset(ImageGridUIState &state,
                                        const std::string &asset_identifier)
{
  state.viewport.focus_asset_identifier = asset_identifier;
  /* Reset so every grid layout (N-Panel, Texture Popover) re-applies for its own (cols, rows). */
  image_grid_focus_reset_applied(state.viewport);
}

bool image_grid_apply_focus_scroll(const bContext &C,
                                   View3D &v3d,
                                   ImageGridUIState &state,
                                   const int cols,
                                   const int effective_rows_hint)
{
  if (state.viewport.focus_asset_identifier.empty()) {
    return true;
  }

  const int cols_clamped = image_grid_cols_clamp(cols);
  const int rows_clamped = clamp_i(effective_rows_hint, 1, 16);
  const int layout_idx = image_grid_layout_bucket_index(cols_clamped, rows_clamped);

  /* Skip if already applied for this layout — #scroll_row is already correct.
   * A different grid layout (N-Panel vs. Texture Popover) may have a different (cols, rows) and
   * will re-apply when it draws, so we must not clear the identifier here. */
  if (state.viewport.focus_applied_by_layout[layout_idx]) {
    return true;
  }

  /* Never block the draw thread waiting for a library load. #build_items already called
   * #storage_fetch (async); the #NC_ASSET notifier will trigger another redraw once ready. */
  if (!ed::asset::list::library_get_once_available(state.filter.lib_ref)) {
    return false;
  }

  const int filtered_index = image_grid_find_asset_filtered_index(
      C, state, state.viewport.focus_asset_identifier);
  if (filtered_index < 0) {
    /* Asset absent from the filtered list. If the library is fully loaded there is no point
     * retrying — clear the request so future draws are not wasted. */
    if (ed::asset::list::is_loaded(&state.filter.lib_ref)) {
      image_grid_focus_clear(state.viewport);
    }
    return false;
  }

  const int target_row = filtered_index / cols_clamped;
  const int effective_rows = max_ii(1, rows_clamped);

  /* Always bring the focused asset into view, centered vertically, so selecting it in one grid
   * scrolls every other grid (and this one) to show it in the middle. This is applied once per
   * (cols, rows) layout — see #focus_applied_by_layout below — so it does not fight manual
   * scrolling afterwards. */
  if (target_row < effective_rows) {
    /* Asset on the first "page": show the top of the grid without scrolling. Hiding the first rows
     * just to mathematically center an asset near the top feels unnatural to the user. */
    state.viewport.scroll_row = 0;
  }
  else {
    /* Center the active asset vertically in the visible area.
     * NOTE: Do NOT clamp here — cached_item_count may be stale (from a previous library). A
     * premature clamp would cap scroll_row below the true max_scroll_row, causing the wrong
     * rows to be built. The post-build clamp in build_image_grid uses the correct
     * cached_item_count. */
    state.viewport.scroll_row = max_ii(0, target_row - effective_rows / 2);
  }
  /* Land on a whole-row boundary when focusing an asset. */
  state.viewport.scroll_offset_px = 0;

  /* Record this layout so it does not re-apply every frame (which would fight manual scrolling).
   * A second layout (e.g. Texture Popover vs. N-Panel) will still re-apply for its own
   * (cols, rows). The identifier is kept alive until the user manually scrolls or the asset
   * disappears. */
  state.viewport.focus_applied_by_layout[layout_idx] = true;
  return true;
}

bool image_grid_asset_is_visible_in_state(const ImageGridUIState &state,
                                          const AssetLibraryReference &asset_lib_ref,
                                          const std::optional<std::string> &asset_catalog_path)
{
  if (!(state.filter.lib_ref == asset_lib_ref)) {
    return false;
  }
  if (state.filter.enabled_catalog_paths.is_empty()) {
    return true;
  }
  if (!asset_catalog_path) {
    return false;
  }
  return state.filter.enabled_catalog_paths.contains(*asset_catalog_path);
}

std::optional<std::string> image_grid_catalog_path_from_shelf(const AssetShelf &shelf)
{
  if (ed::asset::shelf::settings_is_all_catalog_active(shelf.settings)) {
    return std::nullopt;
  }
  if (shelf.settings.active_catalog_path && shelf.settings.active_catalog_path[0] != '\0') {
    return std::string(shelf.settings.active_catalog_path);
  }
  return std::nullopt;
}

bool image_grid_filter_matches_shelf(const ImageGridUIState &state, const AssetShelf &shelf)
{
  if (!(state.filter.lib_ref == shelf.settings.asset_library_reference)) {
    return false;
  }
  const std::optional<std::string> shelf_catalog_path = image_grid_catalog_path_from_shelf(shelf);
  if (!shelf_catalog_path) {
    return state.filter.enabled_catalog_paths.is_empty();
  }
  return state.filter.enabled_catalog_paths.size() == 1 &&
         state.filter.enabled_catalog_paths.contains(*shelf_catalog_path);
}

void image_grid_state_persist_to_view3d(View3D &v3d,
                                        ImageGridUIState &state,
                                        const bool is_mask_slot)
{
  short &library_type = is_mask_slot ? v3d.image_grid_mask_library_type :
                                         v3d.image_grid_library_type;
  int &library_custom_index = is_mask_slot ? v3d.image_grid_mask_library_custom_index :
                                             v3d.image_grid_library_custom_index;
  ListBaseT<ImageGridLibraryCatalogState> &library_catalog_states =
      is_mask_slot ? v3d.image_grid_mask_library_catalog_states :
                     v3d.image_grid_library_catalog_states;
  ListBaseT<AssetCatalogPathLink> &legacy_enabled_catalog_paths =
      is_mask_slot ? v3d.image_grid_mask_enabled_catalog_paths :
                     v3d.image_grid_enabled_catalog_paths;

  library_type = short(state.filter.lib_ref.type);
  library_custom_index = state.filter.lib_ref.custom_library_index;

  image_grid_catalog_commit_active(state);

  while (ImageGridLibraryCatalogState *libcat_state = static_cast<ImageGridLibraryCatalogState *>(
             BLI_pophead(&library_catalog_states)))
  {
    BKE_asset_catalog_path_list_free(libcat_state->enabled_catalog_paths);
    MEM_delete(libcat_state);
  }

  BKE_asset_catalog_path_list_free(legacy_enabled_catalog_paths);

  for (const auto item : state.filter.enabled_catalogs_by_library.items()) {
    const Set<std::string> &paths = item.value;
    if (paths.is_empty()) {
      continue;
    }

    const AssetLibraryReference lib_ref = ed::asset::library_reference_from_enum_value(item.key);
    ImageGridLibraryCatalogState *libcat_state = MEM_new<ImageGridLibraryCatalogState>(__func__);
    libcat_state->library_ref = lib_ref;
    for (const std::string &path : paths) {
      BKE_asset_catalog_path_list_add_path(libcat_state->enabled_catalog_paths, path.c_str());
    }
    BLI_addtail(&library_catalog_states, libcat_state);
  }
}

void image_grid_pending_schedule_from_asset(ImageGridUIState &state,
                                            const AssetLibraryReference &lib_ref,
                                            const std::optional<std::string> &catalog_path,
                                            const std::string &asset_identifier)
{
  state.pending.apply_after_popover = true;
  state.pending.lib_ref = lib_ref;
  state.pending.use_all_catalogs = !catalog_path.has_value();
  state.pending.catalog_path = catalog_path.value_or("");
  state.pending.focus_asset_identifier = asset_identifier;
  state.pending.focus_filtered_index = -1;
}

std::optional<std::string> image_grid_catalog_path_for_asset(
    const asset_system::AssetRepresentation &asset, const AssetLibraryReference &lib_ref)
{
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
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

static void image_grid_pending_apply_slot(bContext &C, View3D &v3d, ImageGridUIState &state)
{
  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, state.pending.lib_ref);
  state.filter.enabled_catalog_paths.clear();
  if (!state.pending.use_all_catalogs && !state.pending.catalog_path.empty()) {
    state.filter.enabled_catalog_paths.add(state.pending.catalog_path);
  }
  image_grid_catalog_commit_active(state);

  ed::asset::list::storage_fetch(&state.filter.lib_ref, &C);

  if (!state.pending.focus_asset_identifier.empty()) {
    state.viewport.focus_asset_identifier = state.pending.focus_asset_identifier;
    image_grid_focus_reset_applied(state.viewport);
  }

  image_grid_pending_clear(state);
}

void image_grid_pending_apply_if_ready(bContext &C, View3D &v3d)
{
  if (image_grid_browse_popover_is_open(C)) {
    return;
  }

  bool applied_texture = false;
  bool applied_mask = false;
  for (const bool is_mask_slot : {false, true}) {
    ImageGridUIState &state = image_grid_state_get(v3d, is_mask_slot);
    if (!state.pending.apply_after_popover) {
      continue;
    }
    image_grid_pending_apply_slot(C, v3d, state);
    if (is_mask_slot) {
      applied_mask = true;
    }
    else {
      applied_texture = true;
    }
  }

  if (!applied_texture && !applied_mask) {
    return;
  }

  if (applied_texture) {
    image_grid_state_persist_to_view3d(v3d, image_grid_state_get(v3d, false), false);
  }
  if (applied_mask) {
    image_grid_state_persist_to_view3d(v3d, image_grid_state_get(v3d, true), true);
  }
  image_grid_notify_change(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shelf sync
 * \{ */

/* Global storage for the AssetWeakReference returned by the shelf type callback.
 * The callback must return a pointer that stays valid until the next call. */
static AssetWeakReference g_image_shelf_active_asset_storage;

bool image_grid_asset_represents_image(const asset_system::AssetRepresentation &asset,
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
    weak_ref = image_grid_shelf_active_asset_weak_ref(*C, shelf->settings.asset_library_reference);
  }
  else if (const View3D *v3d = CTX_wm_view3d(C)) {
    const PointerRNA target_ptr = CTX_data_pointer_get(C, "image_grid_target");
    const bool is_mask_slot = image_grid_slot_is_mask(target_ptr);
    const ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
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

static void image_texture_shelf_pre_popover_invoke(bContext &C, AssetShelfType * /*shelf_type*/)
{
  if (View3D *v3d = CTX_wm_view3d(&C)) {
    ImageGridUIState &state = image_grid_state_get(*v3d, false);
    image_grid_prepare_browse_shelf(C, state, IMAGE_TEXTURE_SHELF_IDNAME);
  }
}

static void image_texture_shelf_setup_popover_layout(bContext &C, ui::Layout &layout)
{
  image_grid_popover_layout_context_set(layout, C, false);
}

void image_grid_shelf_sync_register()
{
  AssetShelfType *type = ed::asset::shelf::type_find_from_idname(IMAGE_TEXTURE_SHELF_IDNAME);
  if (!type) {
    return;
  }
  /* Re-apply after Python class re-registration replaces the #AssetShelfType. */
  type->get_active_asset_from_context = image_texture_shelf_active_asset_type_callback;
  type->pre_popover_invoke = image_texture_shelf_pre_popover_invoke;
  type->setup_popover_layout = image_texture_shelf_setup_popover_layout;
}

void image_grid_sync_shelf_from_state(AssetShelf &shelf, const ImageGridUIState &state)
{
  shelf.settings.asset_library_reference = state.filter.lib_ref;
  ed::asset::shelf::settings_ensure_valid_library_ref(shelf.settings);

  if (state.filter.enabled_catalog_paths.is_empty()) {
    ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
    return;
  }
  if (state.filter.enabled_catalog_paths.size() == 1) {
    const std::string &path = *state.filter.enabled_catalog_paths.begin();
    ed::asset::shelf::settings_set_active_catalog(shelf.settings,
                                                  asset_system::AssetCatalogPath(path));
    return;
  }
  ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
}

void image_grid_sync_state_from_shelf(ImageGridUIState &state, const AssetShelf &shelf)
{
  const AssetLibraryReference new_lib_ref = shelf.settings.asset_library_reference;
  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, new_lib_ref);
  state.filter.enabled_catalog_paths.clear();
  if (!ed::asset::shelf::settings_is_all_catalog_active(shelf.settings)) {
    if (shelf.settings.active_catalog_path && shelf.settings.active_catalog_path[0] != '\0') {
      state.filter.enabled_catalog_paths.add(shelf.settings.active_catalog_path);
    }
  }
  image_grid_catalog_commit_active(state);
  state.viewport.scroll_row = 0;
  /* Library or catalog changed — focus position is no longer valid. */
  image_grid_focus_clear(state.viewport);
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
  /* The popover keeps its own preview size (persisted per shelf type in the Preferences), so it is
   * intentionally not overwritten from the N-panel image grid's #View3D preview size here. */

  if (std::optional<AssetWeakReference> weak_ref = image_grid_shelf_active_asset_weak_ref(
          C, state.filter.lib_ref))
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

bool image_grid_brush_target_pointer_get(const bContext &C,
                                         PointerRNA *r_target_ptr,
                                         const bool is_mask_slot)
{
  if (!r_target_ptr || !CTX_wm_view3d(&C)) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(const_cast<bContext *>(&C));
  if (!paint) {
    return false;
  }
  Brush *brush = BKE_paint_brush(paint);
  if (!brush) {
    return false;
  }

  MTex *mtex = is_mask_slot ? &brush->mask_mtex : &brush->mtex;
  *r_target_ptr = RNA_pointer_create_discrete(&brush->id, RNA_BrushTextureSlot, mtex);
  return r_target_ptr->data && r_target_ptr->owner_id && GS(r_target_ptr->owner_id->name) == ID_BR;
}

void image_grid_popover_layout_context_set(ui::Layout &layout,
                                         bContext &C,
                                         const bool is_mask_slot)
{
  PointerRNA target_ptr;
  if (!image_grid_brush_target_pointer_get(C, &target_ptr, is_mask_slot)) {
    return;
  }

  layout.context_ptr_set("image_grid_target", &target_ptr);
  layout.context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);

  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }
  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);

  /* Popover refresh (e.g. after #NC_ASSET) must not call #image_grid_prepare_browse_shelf, which
   * overwrites shelf library/catalog from #ImageGridUIState and resets the user's selection.
   * When the user changed library or catalog in the popover, sync those settings back to state. */
  image_grid_shelf_sync_register();
  AssetShelfType *shelf_type = ed::asset::shelf::type_find_from_idname(IMAGE_TEXTURE_SHELF_IDNAME);
  if (!shelf_type) {
    return;
  }
  AssetShelf *shelf = ed::asset::shelf::popup_shelf_get_or_create(C, *shelf_type);
  if (!shelf) {
    return;
  }
  if (!image_grid_filter_matches_shelf(state, *shelf)) {
    image_grid_sync_state_from_shelf(state, *shelf);
    image_grid_state_persist_to_view3d(*v3d, state, is_mask_slot);
    image_grid_notify_change(C, is_mask_slot);
  }
}

/** \} */

}  // namespace blender::ed::view3d
