/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Lifecycle helpers for #bUserAssetCatalogMemory, the per-(library, domain) remembered-catalog
 * table in the user preferences. See docs/superpowers/specs/2026-08-08-per-library-catalog-memory-design.md
 * for the full design (Section A: data model and invariants, Section B: API layer).
 */

#include <optional>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_uuid_types.h"

struct BlendWriter;
struct BlendDataReader;

namespace blender {

template<typename T> struct ListBaseT;
struct AssetLibraryReference;
struct UserDef;
struct bUserAssetCatalogMemory;
enum eAssetCatalogMemoryMode : int8_t;

void BKE_asset_catalog_memory_list_free(ListBaseT<bUserAssetCatalogMemory> &list);
/** Frees only #entry's owned data (its #catalog_id_set), not #entry itself or its list links. */
void BKE_asset_catalog_memory_list_free_single(bUserAssetCatalogMemory &entry);
void BKE_asset_catalog_memory_list_duplicate(ListBaseT<bUserAssetCatalogMemory> &dst,
                                              const ListBaseT<bUserAssetCatalogMemory> &src);
void BKE_asset_catalog_memory_list_blend_write(BlendWriter *writer,
                                                const ListBaseT<bUserAssetCatalogMemory> &list);
void BKE_asset_catalog_memory_list_blend_read_data(BlendDataReader *reader,
                                                    ListBaseT<bUserAssetCatalogMemory> &list);

/**
 * Enforce #bUserAssetCatalogMemory's read-time invariants on one entry: unknown `mode` values
 * reset to ASSET_CATALOG_MEMORY_ALL; a torn (half-null) `catalog_id_set` list head is reset to
 * empty without being walked or freed; nil (all-zero) UUID entries are dropped from the set.
 * Duplicate UUIDs within the set are left as-is (harmless -- the set API has set semantics).
 * Called once per entry from #BKE_asset_catalog_memory_list_blend_read_data.
 */
void BKE_asset_catalog_memory_entry_sanitize(bUserAssetCatalogMemory &entry);

/* Single-select (Asset Browser, AssetShelf popup snapshot). Writing sets mode = SINGLE and this
 * field only; catalog_id_set is untouched. */
std::optional<bUUID> BKE_asset_catalog_memory_get_single(const UserDef *userdef,
                                                          const AssetLibraryReference &library_ref,
                                                          StringRef domain);
void BKE_asset_catalog_memory_set_single(UserDef *userdef,
                                          const AssetLibraryReference &library_ref,
                                          StringRef domain,
                                          bUUID catalog_id);

/* Sets mode = ALL. Does not touch single_catalog_id or catalog_id_set. */
void BKE_asset_catalog_memory_set_all(UserDef *userdef,
                                       const AssetLibraryReference &library_ref,
                                       StringRef domain);

/* Multi-select set (ID Browser, Image Grid). Writing sets mode = SET and this field only;
 * single_catalog_id is untouched. */
Vector<bUUID> BKE_asset_catalog_memory_get_set(const UserDef *userdef,
                                                const AssetLibraryReference &library_ref,
                                                StringRef domain);
void BKE_asset_catalog_memory_set_set(UserDef *userdef,
                                       const AssetLibraryReference &library_ref,
                                       StringRef domain,
                                       Span<bUUID> catalog_ids);

/* Recent / Favorites (ID Browser, Image Grid). Writes ONLY the mode field -- never touches
 * single_catalog_id or catalog_id_set. Callers pass library_ref == ASSET_LIBRARY_ALL for these
 * two modes, per the domain key table in the design spec Section A. */
void BKE_asset_catalog_memory_set_mode(UserDef *userdef,
                                        const AssetLibraryReference &library_ref,
                                        StringRef domain,
                                        eAssetCatalogMemoryMode mode);
eAssetCatalogMemoryMode BKE_asset_catalog_memory_get_mode(const UserDef *userdef,
                                                           const AssetLibraryReference &library_ref,
                                                           StringRef domain);

void BKE_asset_catalog_memory_rename_library(UserDef *userdef,
                                              const char *old_identifier,
                                              const char *new_identifier);

}  // namespace blender
