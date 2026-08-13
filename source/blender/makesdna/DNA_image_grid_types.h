/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Persisted per-slot state for the brush-texture image grid. Shared by #View3D and #SpaceImage
 * so space DNA does not depend on viewport DNA.
 */

#pragma once

#include "DNA_asset_types.h"
#include "DNA_listBase.h"

namespace blender {

/**
 * Per-library enabled catalog paths stored on #ImageGridSlotDNA.
 *
 * Live catalog filters are UserDef catalog memory (domain #"image_grid"), not this list.
 * The list is kept so old .blend files still SDNA-map; persist and load treat it as legacy
 * and clear it rather than using it as the working filter.
 */
struct ImageGridLibraryCatalogState {
  ImageGridLibraryCatalogState *next = nullptr, *prev = nullptr;
  AssetLibraryReference library_ref;
  /** Enabled catalog paths (empty = show all catalogs for this library). */
  ListBaseT<AssetCatalogPathLink> enabled_catalog_paths = {nullptr, nullptr};
};

/**
 * Membership / catalog follow mode persisted on #ImageGridSlotDNA::catalog_mode.
 * Values match #blender::ed::image_grid::ImageGridCatalogMode (All / CatalogPath /
 * Recent / Favorites). Old files had zero padding here → #IMAGE_GRID_CATALOG_MODE_ALL.
 */
typedef enum eImageGridCatalogMode {
  IMAGE_GRID_CATALOG_MODE_ALL = 0,
  IMAGE_GRID_CATALOG_MODE_CATALOG_PATH = 1,
  IMAGE_GRID_CATALOG_MODE_RECENT = 2,
  IMAGE_GRID_CATALOG_MODE_FAVORITES = 3,
} eImageGridCatalogMode;

/**
 * Persisted per-slot state for the sculpt/paint brush-texture image grid. Each host space
 * (#View3D, #SpaceImage) keeps one instance per independent slot instead of duplicating each
 * field per slot.
 *
 * Catalog path filters are not live here: see #ImageGridLibraryCatalogState. #library_ref,
 * #catalog_mode, rows, and name-match IDs are the per-slot DNA that still round-trips.
 */
struct ImageGridSlotDNA {
  /** Number of visible rows for the image grid. 0 = use default (1). */
  short rows = 0;
  /**
   * #eImageGridCatalogMode. Uses former `_pad_rows` bytes so old .blend files (zero padding)
   * load as #IMAGE_GRID_CATALOG_MODE_ALL without do_version.
   */
  short catalog_mode = IMAGE_GRID_CATALOG_MODE_ALL;
  /** Struct padding: #library_ref is struct-typed and needs 8-byte native alignment. */
  char _pad_rows[4] = {};
  /** Asset library browsed by this slot. */
  AssetLibraryReference library_ref;
  /**
   * Legacy library selection (migrated to #library_ref). Kept for do-version migration from files
   * written before 5.2 subversion 47. 0 = unset, meaning "current file".
   */
  short library_type_legacy = 0;
  char _pad_lib[2] = {};
  int library_custom_index_legacy = 0;
  /**
   * Legacy per-view catalog filter (migrated to #library_catalog_states).
   * Kept for do-version migration from files written before 5.2 subversion 40.
   */
  ListBaseT<AssetCatalogPathLink> enabled_catalog_paths_legacy = {nullptr, nullptr};
  /**
   * Legacy per-asset-library catalog selection. Not the live filter: UserDef catalog memory
   * (domain #"image_grid") is. Persist clears this list; blend read still remaps it for old files.
   */
  ListBaseT<ImageGridLibraryCatalogState> library_catalog_states = {nullptr, nullptr};
  /**
   * Name Matching master toggle (0/1). Old files: zero → off.
   */
  char filter_name_match_enabled = 0;
  char _pad_name_match[7] = {};
  /** Active map-type identifiers (#AssetNameMatchIdLink). Empty = no selection. */
  ListBaseT<AssetNameMatchIdLink> filter_name_match_map_types = {nullptr, nullptr};
};

}  // namespace blender
