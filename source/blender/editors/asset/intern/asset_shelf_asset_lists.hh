/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_asset_types.h"

namespace blender {
struct ARegion;
struct AssetWeakReference;
/** Real definition: `BKE_paint_types.hh`, inside `namespace blender` (not global scope -- this
 * forward declaration must match that exactly or callers passing a real #blender::PaintMode
 * will fail to compile against this header's functions). */
enum class PaintMode : int8_t;
namespace io::serialize {
class DictionaryValue;
class Value;
}  // namespace io::serialize
}  // namespace blender

namespace blender::ed::asset::shelf {

/**
 * Owned, hashable identity of a shelf asset. Mirrors #AssetWeakReference's fields, but owns its
 * strings as `std::string` instead of raw asset-system-managed `const char *`, so it can safely
 * live inside a long-lived cache/#Set/#Map key.
 */
struct ShelfAssetRef {
  eAssetLibraryType library_type = ASSET_LIBRARY_LOCAL;
  std::string library_identifier;
  std::string relative_identifier;
  /**
   * Path of the .blend file an #ASSET_LIBRARY_LOCAL ("Current File") asset lives in; empty for
   * every other library type. Part of the identity so that local assets, which are only addressed
   * by their ID name, don't collide across .blend files -- a favorite brush named `Clay` in
   * `a.blend` must not show up as a favorite in `b.blend`. Empty for an unsaved file.
   */
  std::string blend_filepath;

  /** Reads the current .blend file path from the global #Main for local assets. */
  static ShelfAssetRef from_weak_reference(const AssetWeakReference &weak_ref);

  uint64_t hash() const;
  bool operator==(const ShelfAssetRef &other) const;
};

/** Recent/favorite asset lists for a single asset shelf type. */
struct ShelfAssetLists {
  /** Most-recently-used first. Capped at the configured recent limit (see
   * #shelf_asset_lists_recent_max_count_get()). */
  Vector<ShelfAssetRef> recent;
  VectorSet<ShelfAssetRef> favorites;
};

/** Default fallback for the maximum number of entries kept in #ShelfAssetLists::recent. */
constexpr int SHELF_ASSET_LISTS_RECENT_MAX = 20;

/**
 * Insert/move \a ref to the front of \a recent (most-recently-used first), trimming the list to
 * \a max_count. Pure function, no file I/O -- exercised directly by unit tests.
 */
void record_recent_into(Vector<ShelfAssetRef> &recent, ShelfAssetRef ref, int max_count);

/**
 * Add \a ref to \a favorites if not already present, otherwise remove it. Pure function, no file
 * I/O -- exercised directly by unit tests.
 */
void toggle_favorite_into(VectorSet<ShelfAssetRef> &favorites, const ShelfAssetRef &ref);

/**
 * Move \a ref to \a new_index in \a favorites (0 = front). Returns false if \a ref is absent or
 * \a new_index is unchanged after clamping.
 */
bool reorder_favorite_into(VectorSet<ShelfAssetRef> &favorites,
                           const ShelfAssetRef &ref,
                           int new_index);

/**
 * Index of the favorite whose #ShelfAssetRef::relative_identifier matches \a target_identifier,
 * scanning the *complete* current list (not any UI-visible window) -- the same string
 * #AssetView::add_asset_item() uses to build each tile's own identifier, so an identifier
 * round-trips correctly here without needing to resolve back through the asset library system.
 * Returns -1 if not found (e.g. the asset was removed from Favorites between drag-start and
 * drop). Pure function, no file I/O -- exercised directly by unit tests.
 */
int favorite_index_from_identifier(Span<ShelfAssetRef> favorites, StringRefNull target_identifier);

/** Serialize all shelves' lists to a JSON document value (`{"version", "shelves"}`). */
std::shared_ptr<io::serialize::DictionaryValue> lists_to_json(
    const Map<std::string, ShelfAssetLists> &shelves);

/**
 * Deserialize a JSON document value produced by #lists_to_json(). Unresolvable/malformed entries
 * are silently skipped rather than failing the whole document.
 */
Map<std::string, ShelfAssetLists> lists_from_json(const io::serialize::Value &root);

/** Canonical brush asset-shelf idname for \a mode, or nullptr if \a mode has no brush shelf. */
const char *brush_shelf_idname_from_paint_mode(PaintMode mode);

/** True if \a idname is one of the brush asset shelves from #brush_shelf_idname_from_paint_mode(). */
bool shelf_idname_is_brush_shelf(StringRef idname);

/** True if \a idname supports Recent/Favorites asset lists (brush shelves and image texture). */
bool shelf_supports_asset_lists(StringRef idname);

/**
 * Process-local binding of a temporary popup #ARegion to the asset-shelf type idname currently
 * drawing into it (D7). Bind overwrites; unbind removes. #shelf_popup_region_idname_get returns a
 * #StringRefNull into map storage that remains valid until #shelf_popup_region_unbind for that
 * region (or process exit).
 */
void shelf_popup_region_bind(const ARegion &region, StringRef shelf_idname);
void shelf_popup_region_unbind(const ARegion &region);
std::optional<StringRefNull> shelf_popup_region_idname_get(const ARegion &region);

/**
 * Return the maximum number of Recent entries configured for \a shelf_idname. Falls back to
 * #SHELF_ASSET_LISTS_RECENT_MAX when the user has never stored a per-shelf limit.
 */
int shelf_asset_lists_recent_max_count_get(StringRef shelf_idname);

/**
 * Set the in-memory limit for \a shelf_idname and trim the cached Recent list to that size.
 * The caller is responsible for persisting the value to User Preferences.
 */
void shelf_asset_lists_recent_max_count_set(StringRef shelf_idname, int recent_max_count);

/**
 * Record \a weak_ref as the most-recently-used brush asset for \a shelf_idname, trimming to the
 * configured limit (#shelf_asset_lists_recent_max_count_get()). Loads the cache from disk on first call
 * this session and persists the change back to disk immediately.
 */
void shelf_asset_lists_record_recent(StringRef shelf_idname, const AssetWeakReference &weak_ref);

bool shelf_asset_lists_is_favorite(StringRef shelf_idname, const AssetWeakReference &weak_ref);

/** Add/remove \a weak_ref from \a shelf_idname's favorites. Persists the change to disk. */
void shelf_asset_lists_toggle_favorite(StringRef shelf_idname, const AssetWeakReference &weak_ref);

/** Persist reorder for \a shelf_idname. No-op if ref not in list or reorder unchanged. */
void shelf_asset_lists_reorder_favorite(StringRef shelf_idname,
                                  const AssetWeakReference &weak_ref,
                                  int new_index);

/** Empty \a shelf_idname's recent list. Persists the change to disk immediately (a no-op if the
 * list is already empty or unknown). */
void shelf_asset_lists_clear_recent(StringRef shelf_idname);

/** Empty \a shelf_idname's favorites list. Persists the change to disk immediately (a no-op if the
 * list is already empty or unknown). */
void shelf_asset_lists_clear_favorites(StringRef shelf_idname);

/** Write out any lists change that was only recorded in memory (see #shelf_asset_lists_record_recent).
 * Called once on Blender exit; a no-op if nothing is pending. */
void shelf_asset_lists_flush();

Span<ShelfAssetRef> shelf_asset_lists_recent(StringRef shelf_idname);
Span<ShelfAssetRef> shelf_asset_lists_favorites(StringRef shelf_idname);

}  // namespace blender::ed::asset::shelf
