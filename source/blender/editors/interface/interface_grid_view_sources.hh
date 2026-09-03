/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edinterface
 *
 * Concrete #GridDataSource implementations:
 * - #AssetGridDataSource — asset library + catalog filter backed.
 * - #PyCallbackGridDataSource — bridges a registered Python #UIGrid type.
 */

#include <optional>

#include "interface_grid_view.hh"

#include "BKE_name_matching.hh"

#include "AS_asset_library.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "RNA_types.hh"

namespace blender {
struct uiGridType;

namespace ui {

/* -------------------------------------------------------------------- */
/** \name AssetGridDataSource
 * \{ */

/**
 * The parts of an asset grid's configuration a single tile carries. Copied into every
 * #AssetGridItem, so it stays valid for the tile's own (per-redraw) lifetime.
 */
struct AssetGridItemParams {
  /** Invoked on activation, with the standard asset-reference properties set on the operator. */
  std::string activate_operator;
  /**
   * Invoked (with the same asset-reference properties) instead of the default asset/ID
   * drag-and-drop when the user starts dragging an item. Empty uses the default
   * #WM_DRAG_ID / #WM_DRAG_ASSET drag.
   */
  std::string drag_operator;
  /**
   * Host-defined target string, set on the activate operator's `context_id` property if it has
   * one. Lets a host tell the operator *what* the click applies to without a layout context.
   */
  std::string activate_context_id;
  /** When false the items carry no drag controller at all; see #template_grid_view_asset. */
  bool use_drag = true;
  /** Whether tiles show the item name; see #GridViewSettings.show_names. */
  bool show_names = false;
};

/**
 * Everything an #AssetGridDataSource lists from. Grouped rather than passed as a dozen positional
 * constructor arguments, most of which most callers leave at their default.
 */
struct AssetGridDataSourceConfig {
  AssetLibraryReference library_ref = {};
  /** Catalog paths to list; empty means "show all". */
  Set<std::string> enabled_catalogs;
  /**
   * Asset ID types to list; empty means "show all", so any asset type (Image, Material, Object, a
   * custom asset type, ...) is shown unless this narrows it down.
   */
  Set<short> filter_id_types;
  NameMatchFilterState name_match;
  /** Free-text "contains" name filter from the grip row's search field, matched
   * case-insensitively. Empty means "show all"; see #GridViewSettings.filter_search. */
  std::string search;
  /** When non-empty, the matching item is highlighted as active. */
  std::string active_identifier;
  /**
   * Recent / Favorites browsing. Not a library and not a catalog filter: the items come from the
   * named asset shelf's membership lists, in list order. Empty means the normal library listing.
   */
  std::string membership_shelf_idname;
  bool membership_is_favorites = false;
  /** Handed to every tile this source builds. */
  AssetGridItemParams item;
};

/**
 * Grid source backed by an asset library, see #AssetGridDataSourceConfig.
 *
 * One instance is built per redraw (and per query call) and thrown away, so it treats the asset
 * lists it reads as fixed for its own lifetime -- which is what lets #membership_assets cache its
 * resolved listing.
 */
class AssetGridDataSource : public GridDataSource {
  AssetGridDataSourceConfig config_;
  /**
   * Resolved Recent / Favorites listing, built on first use. Only a cache: resolving it walks the
   * combined "All Libraries" list once, and several passes over the same source (count, then
   * index lookup, then build) would otherwise each pay for it.
   */
  mutable std::optional<Vector<asset_system::AssetRepresentation *>> membership_assets_;

 public:
  explicit AssetGridDataSource(AssetGridDataSourceConfig config);

  int item_count(const bContext &C) const override;
  bool item_count_ready(const bContext &C) const override;
  /**
   * Position of \a identifier in the filtered list, or -1 when it is filtered out or unknown.
   * Used to reveal the host's active item; the filtering lives here, so the index cannot be
   * derived by the caller.
   */
  int filtered_index_of(const bContext &C, StringRef identifier) const;
  /**
   * #filtered_index_of plus the total item count, from a single pass over the filtered list.
   * \a r_count is set even when the identifier is absent.
   */
  int filtered_index_of_with_count(const bContext &C, StringRef identifier, int *r_count) const;
  /**
   * Identifier at \a index in the filtered list, empty when out of range or the library has not
   * finished reading. The inverse of #filtered_index_of, and the pair the Python stepping API is
   * built on (see #grid_query::asset_step).
   */
  std::string filtered_identifier_at(const bContext &C, int index) const;

  void build_window(const bContext &C, AbstractGridView &view, IndexRange window) override;
  int build_window_and_count(const bContext &C,
                             AbstractGridView &view,
                             IndexRange window) override;

 private:
  /** Library whose list must be read before anything can be enumerated. */
  AssetLibraryReference listed_library_ref() const;
  /** Every asset this source shows, in display order, with its index. */
  void foreach_filtered(FunctionRef<bool(asset_system::AssetRepresentation &, int)> fn) const;
  /** Assets of the shelf's Recent / Favorites list, in list order; see #membership_assets_. */
  Span<asset_system::AssetRepresentation *> membership_assets() const;
};

/**
 * Everything about an asset grid that does not come from its #GridViewSettings. Mirrors the
 * corresponding fields of #GridViewAssetParams; a query that only reads the list can leave all of
 * it at the defaults except the two membership/catalog-memory fields, which change *which* items
 * the grid lists and so must match what the drawing call passes.
 */
struct AssetGridSourceParams {
  const char *activate_operator = nullptr;
  const char *drag_operator = nullptr;
  const char *active_identifier = nullptr;
  bool use_drag = true;
  const char *activate_context_id = nullptr;
  /** See #GridViewAssetParams::membership_shelf_idname. */
  const char *membership_shelf_idname = nullptr;
  /** See #GridViewAssetParams::catalog_memory_domain. */
  const char *catalog_memory_domain = nullptr;
  /** See #GridViewAssetParams::catalog_filter_domain. */
  const char *catalog_filter_domain = nullptr;
};

/**
 * The library an asset grid with these \a settings and \a params actually lists: normally
 * #GridViewSettings.asset_library_reference, but the combined library while the host's
 * Recent/Favorites membership mode is active (that mode spans every library).
 */
AssetLibraryReference asset_grid_library_from_settings(PointerRNA &settings,
                                                       const AssetGridSourceParams &params);

/**
 * Build the source an asset grid with these \a settings and \a params lists, so the drawing path
 * and the query API (#grid_query) can never disagree about which items a grid holds or in what
 * order. Resolves the library, the Recent/Favorites membership override, the catalog, type and
 * name-match filters, and the "show names" flag from \a settings.
 *
 * \param r_library_ref: the library the source ended up listing, which is the membership override
 * rather than #GridViewSettings.asset_library_reference while Recent/Favorites is active.
 */
std::unique_ptr<AssetGridDataSource> asset_grid_source_from_settings(
    PointerRNA &settings,
    const AssetGridSourceParams &params,
    AssetLibraryReference *r_library_ref = nullptr);

/** \} */

/* -------------------------------------------------------------------- */
/** \name PyCallbackGridDataSource
 * \{ */

/**
 * Bridges a registered Python #UIGrid type to the generic #GridDataSource interface.
 */
class PyCallbackGridDataSource : public GridDataSource {
  uiGridType *grid_type_;
  PointerRNA dataptr_;
  std::string propname_;
  /** Whether tiles show the item name; see #GridViewSettings.show_names. */
  bool show_names_ = false;
  /** Identifier of the host's active item; empty leaves the active tile to the view's own
   * click history. */
  std::string active_identifier_;
  /** Free-text "contains" name filter from the grip row's search field, matched
   * case-insensitively. Empty means "show all"; see #GridViewSettings.filter_search. */
  std::string search_;

  /** Number of items the Python type reports, before the search filter. */
  int py_item_count(const bContext &C) const;
  /**
   * Indices of the items whose label passes #search_, in listing order. Only called while the
   * search is non-empty: matching means asking the Python type for every item's label, which is
   * exactly the per-redraw cost the windowed #build_window exists to avoid. Never called more
   * than once per redraw -- see #build_window_and_count.
   */
  Vector<int> filtered_indices(const bContext &C) const;

 public:
  PyCallbackGridDataSource(uiGridType *grid_type,
                           PointerRNA dataptr,
                           StringRef propname,
                           bool show_names = false,
                           StringRef active_identifier = "",
                           std::string search = "");

  int item_count(const bContext &C) const override;
  void build_window(const bContext &C, AbstractGridView &view, IndexRange window) override;
  /**
   * Overridden because the default (#item_count then #build_window) would resolve the search
   * filter twice, and resolving it means one Python `get_item` call per item in the whole list.
   */
  int build_window_and_count(const bContext &C,
                             AbstractGridView &view,
                             IndexRange window) override;
};

/** \} */

} /* namespace ui */
} /* namespace blender */
