/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include <optional>

#include "BLI_function_ref.hh"

namespace blender {

struct ARegion;
struct ARegionType;
struct AssetLibraryReference;
struct AssetShelf;
struct AssetShelfType;
struct AssetShelfSettings;
struct bContext;
struct BlendDataReader;
struct BlendWriter;
struct RegionAssetShelf;

namespace asset_system {
class AssetCatalogPath;
}

namespace ui {
struct Layout;
}  // namespace ui

namespace ed::asset::shelf {

void build_asset_view(ui::Layout &layout,
                      const AssetLibraryReference &library_ref,
                      const AssetShelf &shelf,
                      const bContext &C,
                      std::optional<int> popup_grid_viewport_height_px = std::nullopt,
                      std::optional<int> cols_hint = std::nullopt);

void catalog_selector_panel_register(ARegionType *region_type);
void popover_panel_register(ARegionType *region_type);

AssetShelf *active_shelf_from_context(const bContext *C);

void send_redraw_notifier(const bContext &C);

AssetShelfType *ensure_shelf_has_type(AssetShelf &shelf);
AssetShelf *create_shelf_from_type(AssetShelfType &type);

void library_selector_draw(const bContext *C, ui::Layout &layout, AssetShelf &shelf);

/**
 * Deep-copies \a shelf_regiondata into newly allocated memory. Must be freed using
 * #regiondata_free().
 */
RegionAssetShelf *regiondata_duplicate(const RegionAssetShelf *shelf_regiondata);
/** Frees the contained data and \a shelf_regiondata itself. */
void regiondata_free(RegionAssetShelf *shelf_regiondata);
void regiondata_blend_write(BlendWriter *writer, const RegionAssetShelf *shelf_regiondata);
void regiondata_blend_read_data(BlendDataReader *reader, RegionAssetShelf **shelf_regiondata);

void settings_blend_write(BlendWriter *writer, const AssetShelfSettings &settings);
void settings_blend_read_data(BlendDataReader *reader, AssetShelfSettings &settings);

/**
 * Important: Must be called before #AssetShelfSettings.asset_library_reference is used. It will
 * make sure to fall back to the "All" library if the reference refers to a deleted library. An
 * invalid reference would make loading the asset listing fail.
 *
 * The library reference in \a settings will be updated and returned (for convenience).
 */
AssetLibraryReference &settings_ensure_valid_library_ref(AssetShelfSettings &settings);
/** True when the settings reference a custom library that no longer resolves at all (as opposed
 * to one that resolves but is a folder or disabled, which falls back to "All" instead). */
bool settings_library_is_missing(const AssetShelfSettings &settings);

void settings_set_active_catalog(AssetShelfSettings &settings,
                                 const asset_system::AssetCatalogPath &path);
void settings_set_all_catalog_active(AssetShelfSettings &settings);
bool settings_is_active_catalog(const AssetShelfSettings &settings,
                                const asset_system::AssetCatalogPath &path);
bool settings_is_all_catalog_active(const AssetShelfSettings &settings);
void settings_set_recent_catalog_active(AssetShelfSettings &settings);
bool settings_is_recent_catalog_active(const AssetShelfSettings &settings);
void settings_set_favorites_catalog_active(AssetShelfSettings &settings);
bool settings_is_favorites_catalog_active(const AssetShelfSettings &settings);
/**
 * Clears the list of enabled catalogs in either the Preferences (if any) or the asset shelf
 * settings (if any), depending on the #ASSET_SHELF_TYPE_FLAG_STORE_CATALOGS_IN_PREFS flag.
 */
void settings_clear_enabled_catalogs(AssetShelf &shelf);
bool settings_is_catalog_path_enabled(const AssetShelf &shelf,
                                      const asset_system::AssetCatalogPath &path);
void settings_set_catalog_path_enabled(AssetShelf &shelf,
                                       const asset_system::AssetCatalogPath &path);

void settings_foreach_enabled_catalog_path(
    const AssetShelf &shelf,
    FunctionRef<void(const asset_system::AssetCatalogPath &catalog_path)> fn);

/** Collapsed state of a catalog path in the shelf, or nullopt when not saved yet. */
std::optional<bool> settings_get_catalog_path_collapsed(
    const AssetShelfSettings &settings, const asset_system::AssetCatalogPath &path);

/** Save the collapsed state of a catalog path in the shelf settings. */
void settings_set_catalog_path_collapsed(AssetShelfSettings &settings,
                                         const asset_system::AssetCatalogPath &path,
                                         bool collapsed);

}  // namespace ed::asset::shelf
}  // namespace blender
