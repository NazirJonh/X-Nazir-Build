/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Functions for filtering assets.
 */

#pragma once

#include <optional>
#include <string>

#include "DNA_listBase.h"
#include "DNA_uuid_types.h"

#include "BLI_function_ref.hh"
#include "BLI_multi_value_map.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_path.hh"
#include "AS_asset_catalog_tree.hh"

namespace blender {

struct AssetLibraryReference;
struct AssetMetaData;
struct AssetTag;
struct bContext;
namespace asset_system {
class AssetLibrary;
class AssetRepresentation;
}  // namespace asset_system

namespace ed::asset {

struct AssetFilterSettings {
  /** Tags to match against. These are newly allocated, and compared against the
   * #AssetMetaData.tags. */
  ListBaseT<AssetTag> tags;
  uint64_t id_types; /* rna_enum_id_type_filter_items */
};

/**
 * Compare \a asset against the settings of \a filter.
 *
 * Individual filter parameters are ORed with the asset properties. That means:
 * * The asset type must be one of the ID types filtered by, and
 * * The asset must contain at least one of the tags filtered by.
 * However for an asset to be matching it must have one match in each of the parameters. I.e. one
 * matching type __and__ at least one matching tag.
 *
 * \returns True if the asset should be visible with these filter settings (parameters match).
 * Otherwise returns false (mismatch).
 */
bool filter_matches_asset(const AssetFilterSettings *filter,
                          const asset_system::AssetRepresentation &asset);

struct AssetItemTree {
  asset_system::AssetCatalogTree catalogs;
  MultiValueMap<asset_system::AssetCatalogPath, asset_system::AssetRepresentation *>
      assets_per_path;
  /** Assets not added to a catalog, not part of #assets_per_path. */
  Vector<asset_system::AssetRepresentation *> unassigned_assets;
  /** True if the tree is out of date compared to asset libraries and must be rebuilt. */
  bool dirty = true;
};

asset_system::AssetCatalogTree build_filtered_catalog_tree(
    const asset_system::AssetLibrary &library,
    const AssetLibraryReference &library_ref,
    FunctionRef<bool(const asset_system::AssetRepresentation &)> is_asset_visible_fn);
AssetItemTree build_filtered_all_catalog_tree(
    const AssetLibraryReference &library_ref,
    const bContext &C,
    const AssetFilterSettings &filter_settings,
    FunctionRef<bool(const AssetMetaData &)> meta_data_filter = {},
    const std::optional<StringRef> skip_prefix = std::nullopt);

/**
 * Collect unique metadata tag names from assets in \a library_ref.
 * First-seen casing is preserved; uniqueness is case-insensitive. Result is sorted.
 */
Vector<std::string> collect_unique_asset_tag_names(const AssetLibraryReference &library_ref,
                                                   const bContext &C);

/**
 * How a selected catalog SET treats assets in descendant catalogs.
 *
 * #Exact: the asset's catalog must itself be in the SET.
 * #IncludeChildren: the asset is visible when its catalog is in the SET or is a descendant of
 * one that is (the rule #asset_system::AssetCatalogFilter / #create_catalog_filter encode).
 *
 * Grid hosts (image grid foreach, #AssetGridDataSource, ID browser) use #IncludeChildren.
 * Do not mix policies in one host: visibility helpers and iterators must share this enum.
 */
enum class CatalogContainment {
  Exact,
  IncludeChildren,
};

/**
 * True when \a asset_catalog_path is covered by \a enabled_paths.
 * An empty SET means "no catalog restriction" (every path is visible, including unassigned).
 *
 * Path-only: does not need a loaded #AssetLibrary. #IncludeChildren uses
 * #AssetCatalogPath::is_contained_in (the same parent/child relation as
 * #AssetCatalogService::create_catalog_filter).
 */
bool catalog_path_is_in_enabled_set(StringRef asset_catalog_path,
                                    const Set<std::string> &enabled_paths,
                                    CatalogContainment containment);

/**
 * UUID form of #catalog_path_is_in_enabled_set.
 * An empty \a enabled_ids means no restriction.
 * When \a library is null and \a enabled_ids is non-empty, returns false (hide while loading,
 * matching image-grid foreach: do not flash an unfiltered list).
 */
bool catalog_id_is_in_enabled_set(const asset_system::AssetLibrary *library,
                                  asset_system::CatalogID catalog_id,
                                  Span<bUUID> enabled_ids,
                                  CatalogContainment containment);

/**
 * Build #AssetCatalogFilter objects for each enabled path that resolves in \a library
 * (#IncludeChildren). Paths that do not resolve are skipped; an empty result with a non-empty
 * input means the library is still loading or the SET is stale.
 */
Vector<asset_system::AssetCatalogFilter> catalog_filters_from_enabled_paths(
    const asset_system::AssetLibrary &library, const Set<std::string> &enabled_paths);

Vector<asset_system::AssetCatalogFilter> catalog_filters_from_enabled_ids(
    const asset_system::AssetLibrary &library, Span<bUUID> enabled_ids);

/**
 * Walk assets in \a lib_ref in list order, applying catalog SET + extra poll.
 *
 * \param enabled_catalog_paths: null or empty means no catalog restriction. Non-empty uses
 *        \a containment; when filters cannot be built (library still loading), no assets are
 *        yielded.
 * \param extra_poll: optional extra gate (ID type, assignability, name-match, ...). Empty means
 *        pass.
 * \param fn: receives each passing asset and its zero-based filtered index. Return false to stop
 *        early; the returned count still includes skipped-after-stop items only if \a fn already
 *        incremented via previous true returns — same contract as image-grid foreach: the count
 *        is the number of times \a fn was invoked (or would have been, on a full walk). This
 *        iterator stops calling \a fn after a false return and does not count remaining assets.
 *
 * \return Number of assets that passed the filter (and were handed to \a fn, including the one
 *         that returned false).
 */
int foreach_filtered_asset(
    const AssetLibraryReference &lib_ref,
    const Set<std::string> *enabled_catalog_paths,
    CatalogContainment containment,
    FunctionRef<bool(const asset_system::AssetRepresentation &asset)> extra_poll,
    FunctionRef<bool(asset_system::AssetRepresentation &asset, int filtered_index)> fn);

/**
 * UserDef catalog-memory SET helpers parameterized by domain (`"image_grid"`, `"id_browser"`,
 * ...). Empty SET / non-SET mode means "not in an include-list" for checkbox UI (unchecked).
 */
bool catalog_memory_id_is_enabled(const AssetLibraryReference &lib_ref,
                                  StringRef domain,
                                  asset_system::CatalogID catalog_id);
void catalog_memory_id_set_enabled(const AssetLibraryReference &lib_ref,
                                   StringRef domain,
                                   asset_system::CatalogID catalog_id,
                                   bool enabled);

}  // namespace ed::asset
}  // namespace blender
