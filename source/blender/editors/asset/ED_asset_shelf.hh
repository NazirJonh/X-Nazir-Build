/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

namespace blender {

struct ARegion;
struct ARegionType;
struct AssetLibraryReference;
struct AssetShelf;
struct AssetShelfSettings;
struct AssetShelfType;
struct AssetWeakReference;
struct BlendDataReader;
struct BlendWriter;
struct Main;
struct RegionPollParams;
struct ScrArea;
struct bContext;
struct bContextDataResult;
struct wmOperatorType;
struct wmRegionListenerParams;
struct wmRegionMessageSubscribeParams;
struct wmWindowManager;

/** Real definition: `BKE_paint_types.hh`, inside `namespace blender` (not global scope -- this
 * forward declaration must match that exactly or callers passing a real #blender::PaintMode
 * will fail to compile against this header's functions). */
enum class PaintMode : int8_t;

namespace asset_system {
class AssetCatalogPath;
class AssetRepresentation;
}  // namespace asset_system

namespace ed::asset::shelf {

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Regions
 *
 * Naming conventions:
 * - #regions_xxx(): Applies to both regions (#RGN_TYPE_ASSET_SHELF and
 *   #RGN_TYPE_ASSET_SHELF_HEADER).
 * - #region_xxx(): Applies to the main shelf region (#RGN_TYPE_ASSET_SHELF).
 * - #header_region_xxx(): Applies to the shelf header region
 *   (#RGN_TYPE_ASSET_SHELF_HEADER).
 *
 * \{ */

bool regions_poll(const RegionPollParams *params);

/** Only needed for #RGN_TYPE_ASSET_SHELF (not #RGN_TYPE_ASSET_SHELF_HEADER). */
void *region_duplicate(void *regiondata);
void region_free(ARegion *region);
void region_init(wmWindowManager *wm, ARegion *region);
int region_snap(const ARegion *region, int size, int axis);
void region_on_user_resize(const ARegion *region);
void region_listen(const wmRegionListenerParams *params);
void region_message_subscribe(const wmRegionMessageSubscribeParams *params);
void region_layout(const bContext *C, ARegion *region);
void region_draw(const bContext *C, ARegion *region);
void region_on_poll_success(const bContext *C, ARegion *region);
void region_blend_read_data(BlendDataReader *reader, ARegion *region);
void region_blend_write(BlendWriter *writer, ARegion *region);
int region_prefsizey();

void header_region_init(wmWindowManager *wm, ARegion *region);
void header_region(const bContext *C, ARegion *region);
void header_region_listen(const wmRegionListenerParams *params);
int header_region_size();
void types_register(ARegionType *region_type, const int space_type);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Type
 * \{ */

void type_register(std::unique_ptr<AssetShelfType> type);
void type_unregister(const AssetShelfType &shelf_type);
/**
 * Poll an asset shelf type for display as a popup. Doesn't check for space-type (the type's
 * #bl_space_type) since popups should ignore this to allow displaying in any space.
 *
 * Permanent/non-popup asset shelf regions should use #type_poll_for_space_type() instead.
 */
bool type_poll_for_popup(const bContext &C, const AssetShelfType *shelf_type);
bool type_asset_poll(const AssetShelfType &shelf_type,
                     const asset_system::AssetRepresentation &asset);
AssetShelfType *type_find_from_idname(StringRef idname);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Popup
 * \{ */

void type_popup_unlink(const AssetShelfType &shelf_type);
void ensure_asset_library_fetched(const bContext &C, const AssetShelfType &shelf_type);

/**
 * Run \a fn on the library reference of every popup shelf. These live in a process-global
 * singleton rather than in #Main, so a walk over #Main alone would miss them.
 */
void popup_shelves_foreach_library_ref(FunctionRef<void(AssetLibraryReference &)> fn);

/**
 * Return the static popup #AssetShelf instance for \a shelf_type, creating it if
 * #type_poll_for_popup passes. Used by UI outside the default asset-shelf popover panel.
 */
AssetShelf *popup_shelf_get_or_create(const bContext &C, AssetShelfType &shelf_type);

/**
 * Per-`.blend` popup shelf size override, stored on #wmWindowManager keyed by
 * #AssetShelfType.idname (see #AssetShelfPopupSize). Overrides the global Preferences default.
 *
 * #popup_size_load leaves an output untouched when no entry exists or its stored value is 0
 * ("not set"), so callers can pre-seed with the prefs/type defaults. #popup_size_store upserts
 * the entry; the caller is responsible for tagging the file modified (#WM_file_tag_modified).
 */
void popup_size_load(const wmWindowManager &wm,
                     const char *shelf_idname,
                     short *r_width_units,
                     short *r_height_units,
                     short *r_catalog_width_units);
void popup_size_store(wmWindowManager &wm,
                      const char *shelf_idname,
                      short width_units,
                      short height_units,
                      short catalog_width_units);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset Shelf Settings
 * \{ */

AssetLibraryReference &settings_ensure_valid_library_ref(AssetShelfSettings &settings);
/** True when the settings reference a custom library that no longer resolves at all (as opposed
 * to one that resolves but is a folder or disabled, which falls back to "All" instead). */
bool settings_library_is_missing(const AssetShelfSettings &settings);
void settings_swap_asset_library(AssetShelf &shelf, const AssetLibraryReference &new_ref);
void settings_catalog_commit_active(AssetShelf &shelf,
                                    wmWindowManager *wm,
                                    bool tag_file_modified = true);
void settings_load_active_catalog_for_library(AssetShelfSettings &settings,
                                              const AssetLibraryReference &library_ref);
bool popup_library_catalog_settings_store(wmWindowManager &wm, const AssetShelf &shelf);
void popup_shelf_sync_per_file_state_from_wm(const wmWindowManager &wm, AssetShelf &shelf);
/** Re-apply per-`.blend` catalog state from \a wm for every process-global popup shelf. */
void popup_shelves_sync_per_file_state_from_wm(const wmWindowManager &wm);
void settings_set_active_catalog(AssetShelfSettings &settings,
                                 const asset_system::AssetCatalogPath &path);
void settings_set_all_catalog_active(AssetShelfSettings &settings);
bool settings_is_all_catalog_active(const AssetShelfSettings &settings);
/** Set the Recent/Favorites pseudo-catalog as the shelf's active "catalog" (sentinel path). */
void settings_set_recent_catalog_active(AssetShelfSettings &settings);
void settings_set_favorites_catalog_active(AssetShelfSettings &settings);
/** True while the Recent/Favorites pseudo-catalog is the shelf's active "catalog" (stored as a
 * reserved sentinel in #AssetShelfSettings::active_catalog_path, not a real catalog path). */
bool settings_is_recent_catalog_active(const AssetShelfSettings &settings);
bool settings_is_favorites_catalog_active(const AssetShelfSettings &settings);

/** \} */

/* -------------------------------------------------------------------- */

void type_unlink(const Main &bmain, const AssetShelfType &shelf_type);

int tile_width(const AssetShelfSettings &settings);
int tile_height(const AssetShelfSettings &settings);

AssetShelf *active_shelf_from_area(const ScrArea *area);

/**
 * Enable catalog path in all shelves visible in all windows.
 */
void show_catalog_in_visible_shelves(const bContext &C, const StringRefNull catalog_path);

int context(const bContext *C, const char *member, bContextDataResult *result);

/* -------------------------------------------------------------------- */
/** \name Shelf Asset Lists
 * \{ */

const char *brush_shelf_idname_from_paint_mode(PaintMode mode);

/** True if \a idname is one of the brush asset shelves from #brush_shelf_idname_from_paint_mode(). */
bool shelf_idname_is_brush_shelf(StringRef idname);

/** True if \a idname supports Recent/Favorites asset lists (brush shelves and image texture). */
bool shelf_supports_asset_lists(StringRef idname);

/**
 * Process-local binding of a temporary popup #ARegion to the asset-shelf type idname currently
 * drawing into it (D7). Bind overwrites; unbind removes.
 * #shelf_popup_region_idname_get returns a #StringRefNull into map storage valid until unbind for
 * that region.
 */
void shelf_popup_region_bind(const ARegion &region, StringRef shelf_idname);
void shelf_popup_region_unbind(const ARegion &region);
std::optional<StringRefNull> shelf_popup_region_idname_get(const ARegion &region);

/**
 * Resolve the shelf type idname whose Recent/Favorites lists operators should target.
 * Order (D7): asset_shelf CTX → asset_shelf_idname string → temporary-region stamp → paint-mode
 * brush shelf. Stamp is consulted before paint-mode so image popovers cannot mutate brush lists.
 */
const char *shelf_asset_lists_idname_from_context(const bContext *C);

void ASSETSHELF_OT_asset_favorite_toggle(wmOperatorType *ot);
void ASSETSHELF_OT_asset_favorite_reorder(wmOperatorType *ot);
void ASSETSHELF_OT_asset_favorite_reorder_to(wmOperatorType *ot);
void ASSETSHELF_OT_asset_recent_clear(wmOperatorType *ot);
void ASSETSHELF_OT_asset_favorites_clear(wmOperatorType *ot);

/** Return the maximum number of Recent entries configured for \a shelf_idname. */
int shelf_asset_lists_recent_max_count_get(StringRef shelf_idname);

/** Set the in-memory limit for \a shelf_idname and trim the cached Recent list. */
void shelf_asset_lists_recent_max_count_set(StringRef shelf_idname, int recent_max_count);

void shelf_asset_lists_record_recent(StringRef shelf_idname, const AssetWeakReference &weak_ref);
bool shelf_asset_lists_is_favorite(StringRef shelf_idname, const AssetWeakReference &weak_ref);
void shelf_asset_lists_toggle_favorite(StringRef shelf_idname, const AssetWeakReference &weak_ref);
void shelf_asset_lists_reorder_favorite(StringRef shelf_idname,
                                  const AssetWeakReference &weak_ref,
                                  int new_index);

/** Empty \a shelf_idname's recent/favorites list. Persists the change to disk immediately. */
void shelf_asset_lists_clear_recent(StringRef shelf_idname);
void shelf_asset_lists_clear_favorites(StringRef shelf_idname);

/** Persist any pending in-memory recent-lists change. Called once on Blender exit. */
void shelf_asset_lists_flush();

/** \} */

}  // namespace ed::asset::shelf
}  // namespace blender
