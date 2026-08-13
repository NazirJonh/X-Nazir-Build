/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Keep the image-texture asset shelf in sync with the compact image grid.
 */

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_screen_types.h"
#include "DNA_texture_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_path_utils.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include <algorithm>

#include "BKE_asset.hh"
#include "BKE_asset_catalog_memory.hh"
#include "BKE_asset_edit.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_screen.hh"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"
#include "ED_image_grid.hh"

#include "AS_asset_catalog_path.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "interface_grid_view.hh"

#include "intern/asset_library_reference.hh"

namespace blender::ed::image_grid {

static bool image_grid_catalog_path_enabled_for_library(
    const AssetLibraryReference &lib_ref, const std::optional<std::string> &asset_catalog_path)
{
  if (BKE_asset_catalog_memory_get_mode(&U, lib_ref, image_grid_catalog_memory_domain) !=
      ASSET_CATALOG_MEMORY_SET)
  {
    return true;
  }
  const Vector<bUUID> enabled_ids = BKE_asset_catalog_memory_get_set(
      &U, lib_ref, image_grid_catalog_memory_domain);
  if (enabled_ids.is_empty()) {
    return true;
  }
  if (!asset_catalog_path) {
    return false;
  }
  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(lib_ref);
  if (!library) {
    /* Library still loading: keep the asset hidden rather than flashing an unfiltered list. */
    return false;
  }
  const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
      asset_catalog_path->c_str());
  if (!catalog) {
    return false;
  }
  for (const bUUID &id : enabled_ids) {
    if (BLI_uuid_equal(id, catalog->catalog_id)) {
      return true;
    }
  }
  return false;
}

static int image_grid_cols_clamp(const int cols)
{
  return clamp_i(cols, 1, 16);
}

/* Map a grid layout to its per-layout scroll/focus bucket index, keyed by column count only.
 *
 * The row count (grid height) deliberately does NOT take part in the key. Resizing the grip
 * changes only the visible row count; if the bucket were keyed by rows too, each height would
 * restore its own saved scroll, so dragging the grip would snap the grid to a stale row (typically
 * the top) instead of keeping the current first visible row. Keying by columns still separates the
 * N-Panel from the (usually narrower) Texture popover — the distinction that genuinely needs an
 * independent scroll position — while a height change pins the displayed rows and only re-clips.
 */
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

static bool image_grid_browse_popover_is_open(const bContext &C)
{
  /* Fast path: the browse popover is the current popup context (e.g. an event handled inside it). */
  if (ui::region_popup_has_panel(CTX_wm_region_popup(&C), "ASSETSHELF_PT_popover_panel")) {
    return true;
  }
  /* The catalog clobber runs from the N-panel image-grid draw, where the popup is not the context
   * region (#CTX_wm_region_popup is null). Popup regions live in #bScreen::regionbase as
   * #RGN_TYPE_TEMPORARY, so scan them to detect the open browse popover window-wide. */
  const bScreen *screen = CTX_wm_screen(&C);
  if (screen) {
    for (const ARegion &region : screen->regionbase) {
      if (region.regiontype == RGN_TYPE_TEMPORARY &&
          ui::region_popup_has_panel(&region, "ASSETSHELF_PT_popover_panel"))
      {
        return true;
      }
    }
  }
  return false;
}

static int image_grid_find_asset_filtered_index(const bContext &C,
                                                const ImageGridUIState &state,
                                                const std::string &asset_identifier)
{
  Main *bmain = CTX_data_main(&C);
  int result = -1;
  image_grid_foreach_filtered_item(
      *bmain, state, [&](const ImageGridFilteredItem &item, int filtered_index) -> bool {
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
  state.pending.catalog_mode = ImageGridCatalogMode::All;
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

int image_grid_apply_focus_scroll(const bContext &C,
                                  ImageGridUIState &state,
                                  const int cols,
                                  const int effective_rows_hint)
{
  if (state.viewport.focus_asset_identifier.empty()) {
    return -1;
  }

  const int cols_clamped = image_grid_cols_clamp(cols);
  const int rows_clamped = clamp_i(effective_rows_hint, 1, ui::GRID_VIEW_DEFAULT_MAX_ROWS);
  const int layout_idx = image_grid_layout_bucket_index(cols_clamped, rows_clamped);

  /* Skip if already applied for this layout — the session scroll is already correct.
   * A different grid layout (N-Panel vs. Texture Popover) may have a different (cols, rows) and
   * will re-apply when it draws, so we must not clear the identifier here. */
  if (state.viewport.focus_applied_by_layout[layout_idx]) {
    return -1;
  }

  /* Never block the draw thread waiting for a library load. #build_items already called
   * #storage_fetch (async); the #NC_ASSET notifier will trigger another redraw once ready. The
   * request stays pending, so the next redraw retries. */
  if (!ed::asset::list::library_get_once_available(state.filter.lib_ref)) {
    return -1;
  }

  const int filtered_index = image_grid_find_asset_filtered_index(
      C, state, state.viewport.focus_asset_identifier);
  if (filtered_index < 0) {
    /* Asset absent from the filtered list. If the library is fully loaded there is no point
     * retrying — clear the request so future draws are not wasted. */
    if (ed::asset::list::is_loaded(&state.filter.lib_ref)) {
      image_grid_focus_clear(state.viewport);
    }
    return -1;
  }

  const int target_row = filtered_index / cols_clamped;
  const int effective_rows = max_ii(1, rows_clamped);

  /* Bring the focused asset into view, centered vertically, so selecting it in one grid scrolls
   * every other grid to show it in the middle. Applied once per (cols, rows) layout (see
   * #focus_applied_by_layout) so it does not fight manual scrolling afterwards. An asset on the
   * first "page" shows the top without scrolling; otherwise center it. The caller turns the row
   * into the session's pixel position and applies the post-build clamp. */
  const int scroll_target_row = (target_row < effective_rows) ?
                                    0 :
                                    max_ii(0, target_row - effective_rows / 2);

  state.viewport.focus_applied_by_layout[layout_idx] = true;
  return scroll_target_row;
}

bool image_grid_asset_is_visible_in_state(const ImageGridUIState &state,
                                          const AssetLibraryReference &asset_lib_ref,
                                          const std::optional<std::string> &asset_catalog_path)
{
  /* Membership follow modes span #ASSET_LIBRARY_ALL; catalog-path visibility does not apply. */
  if (state.filter.catalog_mode == ImageGridCatalogMode::Recent ||
      state.filter.catalog_mode == ImageGridCatalogMode::Favorites)
  {
    return state.filter.lib_ref.type == ASSET_LIBRARY_ALL;
  }
  if (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    return image_grid_catalog_path_enabled_for_library(asset_lib_ref, asset_catalog_path);
  }
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
  /* Recent/Favorites are pseudo-catalogs addressed by a reserved sentinel in #active_catalog_path,
   * not a real catalog path; never hand the raw sentinel back as if it were one. Callers that need
   * to distinguish All from Recent/Favorites must consult #image_grid_shelf_catalog_mode first —
   * the image-texture shelf uses these sentinels for its popover and N-panel follow-filter. */
  if (ed::asset::shelf::settings_is_recent_catalog_active(shelf.settings) ||
      ed::asset::shelf::settings_is_favorites_catalog_active(shelf.settings))
  {
    return std::nullopt;
  }
  if (shelf.settings.active_catalog_path && shelf.settings.active_catalog_path[0] != '\0') {
    return std::string(shelf.settings.active_catalog_path);
  }
  return std::nullopt;
}

ImageGridCatalogMode image_grid_shelf_catalog_mode(const AssetShelf &shelf)
{
  if (ed::asset::shelf::settings_is_recent_catalog_active(shelf.settings)) {
    return ImageGridCatalogMode::Recent;
  }
  if (ed::asset::shelf::settings_is_favorites_catalog_active(shelf.settings)) {
    return ImageGridCatalogMode::Favorites;
  }
  if (ed::asset::shelf::settings_is_all_catalog_active(shelf.settings)) {
    return ImageGridCatalogMode::All;
  }
  return ImageGridCatalogMode::CatalogPath;
}

bool image_grid_filter_matches_shelf(const ImageGridUIState &state, const AssetShelf &shelf)
{
  const ImageGridCatalogMode shelf_mode = image_grid_shelf_catalog_mode(shelf);
  switch (shelf_mode) {
    case ImageGridCatalogMode::Recent:
      return state.filter.catalog_mode == ImageGridCatalogMode::Recent &&
             state.filter.lib_ref.type == ASSET_LIBRARY_ALL &&
             state.filter.enabled_catalog_paths.is_empty();
    case ImageGridCatalogMode::Favorites:
      return state.filter.catalog_mode == ImageGridCatalogMode::Favorites &&
             state.filter.lib_ref.type == ASSET_LIBRARY_ALL &&
             state.filter.enabled_catalog_paths.is_empty();
    case ImageGridCatalogMode::All:
    case ImageGridCatalogMode::CatalogPath:
      break;
  }

  /* Pseudo follow must not look like All (empty catalog paths). */
  if (state.filter.catalog_mode == ImageGridCatalogMode::Recent ||
      state.filter.catalog_mode == ImageGridCatalogMode::Favorites)
  {
    return false;
  }

  if (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    /* "Active filter" means a non-empty per-library SET entry, matching Decision 1's "empty =
     * show all" rule -- an absent or ALL-mode entry does not count. */
    AssetLibraryReference filtered_library_ref{};
    Vector<bUUID> filtered_ids;
    int filtered_library_count = 0;
    for (asset_system::AssetLibrary *library : image_grid_all_mode_libraries()) {
      const std::optional<AssetLibraryReference> lib_ref_opt = library->library_reference();
      if (!lib_ref_opt) {
        continue;
      }
      const AssetLibraryReference lib_ref = *lib_ref_opt;
      if (BKE_asset_catalog_memory_get_mode(&U, lib_ref, image_grid_catalog_memory_domain) !=
          ASSET_CATALOG_MEMORY_SET)
      {
        continue;
      }
      Vector<bUUID> ids = BKE_asset_catalog_memory_get_set(
          &U, lib_ref, image_grid_catalog_memory_domain);
      if (ids.is_empty()) {
        continue;
      }
      ++filtered_library_count;
      if (filtered_library_count > 1) {
        /* More than one library has an active filter -- grid state cannot be represented by a
         * single shelf catalog selection, so report "not synced" rather than a partial match. */
        return false;
      }
      filtered_library_ref = lib_ref;
      filtered_ids = std::move(ids);
    }
    if (filtered_library_count == 0) {
      /* No library has an active filter: shelf must also be showing All with no restriction. */
      return image_grid_catalog_path_from_shelf(shelf) == std::nullopt &&
             shelf.settings.asset_library_reference.type == ASSET_LIBRARY_ALL;
    }
    if (!(shelf.settings.asset_library_reference == filtered_library_ref)) {
      return false;
    }
    const std::optional<std::string> shelf_catalog_path = image_grid_catalog_path_from_shelf(
        shelf);
    if (!shelf_catalog_path) {
      return false;
    }
    const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
        filtered_library_ref);
    if (!library) {
      return false;
    }
    const asset_system::AssetCatalog *catalog = library->catalog_service().find_catalog_by_path(
        shelf_catalog_path->c_str());
    if (!catalog) {
      return false;
    }
    if (filtered_ids.size() != 1) {
      return false;
    }
    return BLI_uuid_equal(filtered_ids[0], catalog->catalog_id);
  }

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

void image_grid_state_persist(const ImageGridOwner owner,
                              ImageGridUIState &state,
                              const bool is_mask_slot)
{
  ImageGridSlotDNA &slot = owner.slot_dna(is_mask_slot);
  AssetLibraryReference &library_ref = slot.library_ref;
  ListBaseT<ImageGridLibraryCatalogState> &library_catalog_states = slot.library_catalog_states;
  ListBaseT<AssetCatalogPathLink> &legacy_enabled_catalog_paths = slot.enabled_catalog_paths_legacy;

  library_ref = state.filter.lib_ref;
  slot.catalog_mode = short(state.filter.catalog_mode);

  const bool membership = state.filter.catalog_mode == ImageGridCatalogMode::Recent ||
                          state.filter.catalog_mode == ImageGridCatalogMode::Favorites;
  /* Membership never commits the working catalog set into UserDef (would wipe saved filters for
   * real libraries via an empty #ASSET_LIBRARY_ALL write). Mode is written from membership entry
   * points via #BKE_asset_catalog_memory_set_mode; filter edits call #image_grid_catalog_commit_active
   * / #BKE_asset_catalog_memory_set_set. Keep the guard so persist cannot defeat that invariant. */
  if (!membership) {
    image_grid_catalog_commit_active(state);
  }

  /* Clear legacy DNA catalog lists — UserDef (domain #"image_grid") is the source of truth. */
  while (ImageGridLibraryCatalogState *libcat_state = static_cast<ImageGridLibraryCatalogState *>(
             BLI_pophead(&library_catalog_states)))
  {
    BKE_asset_catalog_path_list_free(libcat_state->enabled_catalog_paths);
    MEM_delete(libcat_state);
  }

  BKE_asset_catalog_path_list_free(legacy_enabled_catalog_paths);

  slot.filter_name_match_enabled = state.filter.name_match.enabled ? 1 : 0;
  if (!BLI_listbase_head_is_plausible(&slot.filter_name_match_map_types)) {
    slot.filter_name_match_map_types.clear_no_delete();
  }
  while (AssetNameMatchIdLink *link = static_cast<AssetNameMatchIdLink *>(
             BLI_pophead(&slot.filter_name_match_map_types)))
  {
    MEM_delete(link);
  }
  /* #Set has no stable iteration order; sort before writing so the same active selection
   * round-trips to the same on-disk order instead of shuffling on every save. */
  Vector<std::string> sorted_ids;
  sorted_ids.reserve(state.filter.name_match.active_map_type_ids.size());
  for (const std::string &id : state.filter.name_match.active_map_type_ids) {
    sorted_ids.append(id);
  }
  std::sort(sorted_ids.begin(), sorted_ids.end());
  for (const std::string &id : sorted_ids) {
    AssetNameMatchIdLink *link = MEM_new<AssetNameMatchIdLink>(__func__);
    STRNCPY_UTF8(link->id, id.c_str());
    BLI_addtail(&slot.filter_name_match_map_types, link);
  }
}

void image_grid_pending_schedule_from_asset(ImageGridUIState &state,
                                            const AssetLibraryReference &lib_ref,
                                            const std::optional<std::string> &catalog_path,
                                            const std::string &asset_identifier,
                                            const ImageGridCatalogMode catalog_mode)
{
  state.pending.apply_after_popover = true;
  state.pending.catalog_mode = catalog_mode;
  if (catalog_mode == ImageGridCatalogMode::Recent ||
      catalog_mode == ImageGridCatalogMode::Favorites)
  {
    /* Membership modes always browse across all libraries (mirror AssetView). */
    state.pending.lib_ref = asset_system::all_library_reference();
    state.pending.use_all_catalogs = true;
    state.pending.catalog_path.clear();
  }
  else {
    state.pending.lib_ref = lib_ref;
    state.pending.use_all_catalogs = !catalog_path.has_value();
    state.pending.catalog_path = catalog_path.value_or("");
  }
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

static void image_grid_pending_apply_slot(bContext &C, ImageGridUIState &state)
{
  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, state.pending.lib_ref);

  if (state.pending.catalog_mode == ImageGridCatalogMode::Recent ||
      state.pending.catalog_mode == ImageGridCatalogMode::Favorites)
  {
    /* Do not write Recent/Favorites sentinels into #enabled_catalog_paths, and do not commit the
     * cleared filter into the per-library set (would wipe saved catalog filters). Mode-only write
     * via #BKE_asset_catalog_memory_set_mode preserves catalog_id_set (rev-4 guard). */
    state.filter.catalog_mode = state.pending.catalog_mode;
    state.filter.enabled_catalog_paths.clear();
    BKE_asset_catalog_memory_set_mode(
        &U,
        state.filter.lib_ref,
        image_grid_catalog_memory_domain,
        (state.pending.catalog_mode == ImageGridCatalogMode::Recent) ?
            ASSET_CATALOG_MEMORY_RECENT :
            ASSET_CATALOG_MEMORY_FAVORITES);
  }
  else {
    state.filter.enabled_catalog_paths.clear();
    if (!state.pending.use_all_catalogs && !state.pending.catalog_path.empty()) {
      state.filter.enabled_catalog_paths.add(state.pending.catalog_path);
      state.filter.catalog_mode = ImageGridCatalogMode::CatalogPath;
    }
    else {
      state.filter.catalog_mode = ImageGridCatalogMode::All;
    }
    image_grid_catalog_commit_active(state);
  }

  ed::asset::list::storage_fetch(&state.filter.lib_ref, &C);

  if (!state.pending.focus_asset_identifier.empty()) {
    state.viewport.focus_asset_identifier = state.pending.focus_asset_identifier;
    image_grid_focus_reset_applied(state.viewport);
  }

  image_grid_pending_clear(state);
}

void image_grid_pending_apply_if_ready(bContext &C)
{
  if (image_grid_browse_popover_is_open(C)) {
    return;
  }

  const std::optional<image_grid::ImageGridOwner> owner = image_grid::image_grid_owner_from_context(
      C);
  if (!owner) {
    return;
  }

  bool applied_texture = false;
  bool applied_mask = false;
  for (const bool is_mask_slot : {false, true}) {
    ImageGridUIState &state = image_grid::image_grid_state_get(*owner, is_mask_slot);
    if (!state.pending.apply_after_popover) {
      continue;
    }
    image_grid_pending_apply_slot(C, state);
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
    image_grid::image_grid_state_persist(
        *owner, image_grid::image_grid_state_get(*owner, false), false);
    image_grid_notify_change(C, false);
  }
  if (applied_mask) {
    image_grid::image_grid_state_persist(
        *owner, image_grid::image_grid_state_get(*owner, true), true);
    image_grid_notify_change(C, true);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shelf sync
 * \{ */

/* Two storage slots: [0] for N-Panel draw, [1] for Popover draw.
 * Both may fire in the same frame; separate slots prevent the second call
 * from overwriting the first caller's returned pointer.
 *
 * Raw pointers, not a value-typed static array: #AssetWeakReference owns MEM_-allocated strings
 * and its destructor frees them, but a namespace-scope static's destructor runs during the CRT's
 * exit-time static teardown (after #WM_exit calls #exit()), in an order relative to guardedalloc's
 * own static teardown that C++ does not guarantee -- freeing here at that point crashes
 * (confirmed: EXCEPTION_ACCESS_VIOLATION in #AssetWeakReference::~AssetWeakReference via
 * MEM_lockfree_freeN, reached through the CRT's onexit table). Heap-allocate lazily and never
 * free; two small blocks reported "unfreed" at exit is the accepted trade-off for not crashing. */
static AssetWeakReference *g_image_shelf_active_asset_storage[2] = {nullptr, nullptr};

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
  /* #Image.filepath may use Blender's `//` relative convention, but #BLI_path_cmp_normalized
   * asserts both arguments are absolute. Resolve against the image's blend-file base (mirrors
   * #BKE_image_user_file_path_ex) so relative paths compare correctly instead of aborting. */
  char image_path[FILE_MAX];
  BLI_strncpy(image_path, image.filepath, sizeof(image_path));
  BLI_path_abs(image_path, ID_BLEND_PATH_FROM_GLOBAL(&image.id));
  if (image_path[0] == '\0') {
    return false;
  }
  return BLI_path_cmp_normalized(asset_path.c_str(), image_path) == 0;
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
  else if (const std::optional<image_grid::ImageGridOwner> owner =
               image_grid::image_grid_owner_from_context(*C))
  {
    const PointerRNA target_ptr = CTX_data_pointer_get(C, "image_grid_target");
    const bool is_mask_slot = image_grid_slot_is_mask(target_ptr);
    const ImageGridUIState &state = image_grid::image_grid_state_get(*owner, is_mask_slot);
    if (state.shelf_active_asset_valid) {
      weak_ref = state.shelf_active_asset;
    }
  }

  if (!weak_ref) {
    return nullptr;
  }
  const int slot = (CTX_wm_region_popup(C) != nullptr) ? 1 : 0;
  if (!g_image_shelf_active_asset_storage[slot]) {
    g_image_shelf_active_asset_storage[slot] = MEM_new<AssetWeakReference>(__func__);
  }
  *g_image_shelf_active_asset_storage[slot] = *weak_ref;
  return g_image_shelf_active_asset_storage[slot];
}

static void image_texture_shelf_pre_popover_invoke(bContext &C, AssetShelfType * /*shelf_type*/)
{
  if (const std::optional<image_grid::ImageGridOwner> owner =
          image_grid::image_grid_owner_from_context(C))
  {
    ImageGridUIState &state = image_grid::image_grid_state_get(*owner, false);
    image_grid_prepare_browse_shelf(C, state, IMAGE_TEXTURE_SHELF_IDNAME);
  }
}

static void image_texture_shelf_setup_popover_layout(bContext &C, ui::Layout &layout)
{
  image_grid_popover_layout_context_set(layout, C, false);
}

/**
 * Read the brush texture-slot target pushed into the layout context by
 * #image_texture_shelf_setup_popover_layout, so grid-tile activation does not depend on a live
 * UI context store (popover refresh can invalidate context inherited from the opening button).
 */
static void image_texture_shelf_grid_tile_activate_extra_params(const ui::Layout &layout,
                                                                uint32_t &r_session_uid,
                                                                bool &r_use_mask_slot)
{
  r_session_uid = MAIN_ID_SESSION_UID_UNSET;
  r_use_mask_slot = false;

  const PointerRNA *target_ptr = layout.context_ptr_get("image_grid_target", nullptr);
  if (!target_ptr || !target_ptr->data || !target_ptr->owner_id) {
    return;
  }
  if (GS(target_ptr->owner_id->name) != ID_BR) {
    return;
  }
  r_session_uid = target_ptr->owner_id->session_uid;
  r_use_mask_slot = layout.context_int_get("image_grid_is_mask_slot").value_or(0) != 0;
}

/**
 * #bke::asset_edit_weak_reference_from_id() uses "Image/<id-name>" while shelf assets use the
 * library-relative path from #AssetRepresentation::make_weak_reference(). Match by local ID name
 * as a fallback for the image-texture browse popover.
 */
static bool image_texture_shelf_active_asset_name_fallback_matches(
    const AssetShelfType * /*shelf_type*/,
    const ID *local_id,
    const AssetWeakReference &active_asset)
{
  const char *active_identifier = active_asset.relative_asset_identifier;
  if (!active_identifier || !STRPREFIX(active_identifier, "Image/")) {
    return false;
  }
  return STREQ(local_id->name + 2, active_identifier + 6);
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
  type->active_asset_name_fallback_matches = image_texture_shelf_active_asset_name_fallback_matches;
  type->grid_tile_activate_extra_params = image_texture_shelf_grid_tile_activate_extra_params;
  type->flag |= ASSET_SHELF_TYPE_FLAG_CENTER_ACTIVE_ASSET_ON_OPEN;
}

void image_grid_sync_shelf_from_state(AssetShelf &shelf, const ImageGridUIState &state)
{
  /* Recent/Favorites membership follows AssetView: ALL is only for asset enumeration on the grid.
   * Do not push that ALL #lib_ref onto the shelf library tab — keep the user's library for the
   * catalog tree / selector, and only restore the pseudo-catalog sentinel. */
  if (state.filter.catalog_mode == ImageGridCatalogMode::Recent) {
    ed::asset::shelf::settings_set_recent_catalog_active(shelf.settings);
    ed::asset::shelf::settings_catalog_commit_active(shelf);
    return;
  }
  if (state.filter.catalog_mode == ImageGridCatalogMode::Favorites) {
    ed::asset::shelf::settings_set_favorites_catalog_active(shelf.settings);
    ed::asset::shelf::settings_catalog_commit_active(shelf);
    return;
  }

  ed::asset::shelf::settings_swap_asset_library(shelf, state.filter.lib_ref);
  ed::asset::shelf::settings_ensure_valid_library_ref(shelf.settings);

  if (state.filter.enabled_catalog_paths.is_empty()) {
    ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
  }
  else if (state.filter.enabled_catalog_paths.size() == 1) {
    const std::string &path = *state.filter.enabled_catalog_paths.begin();
    ed::asset::shelf::settings_set_active_catalog(shelf.settings,
                                                  asset_system::AssetCatalogPath(path));
  }
  else {
    ed::asset::shelf::settings_set_all_catalog_active(shelf.settings);
  }
  ed::asset::shelf::settings_catalog_commit_active(shelf);
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
  /* While the browse popover is open it owns the library/catalog selection (the user picks catalogs
   * in its tree, written straight to #shelf.settings). This prep runs on every N-panel image-grid
   * redraw; syncing shelf←state here would clobber that selection back on the next frame. Skip it
   * while open, and resume once the popover closes: the shelf then falls back to the grid's own
   * library/catalog, so browsing catalogs without activating an asset leaves the grid untouched. */
  if (!image_grid_browse_popover_is_open(C)) {
    image_grid_sync_shelf_from_state(*shelf, state);
  }
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
  if (!r_target_ptr || !image_grid::image_grid_owner_from_context(C)) {
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

  /* Only push the brush-slot target the shelf's activate operator needs. The "Browse Image" button
   * inherits it from its own layout context store (#add_browse_image_button); the keymap path
   * (#WM_OT_call_asset_shelf_popover) has no button, so this hook is its only source.
   *
   * Deliberately does *not* reconcile the shelf's library/catalog back into #ImageGridUIState:
   * picking a catalog in the popover must not move the image grid off the user's current catalog
   * (and scroll away from the assigned texture). The grid follows the shelf only once an asset is
   * actually activated — #VIEW3D_OT_image_shelf_activate_asset schedules it via
   * #image_grid_pending_schedule_from_asset, applied by #image_grid_pending_apply_if_ready after
   * the popover closes. */
  layout.context_ptr_set("image_grid_target", &target_ptr);
  layout.context_int_set("image_grid_is_mask_slot", is_mask_slot ? 1 : 0);
}

/** \} */

}  // namespace blender::ed::image_grid
