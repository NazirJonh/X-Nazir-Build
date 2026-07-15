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
 * Owned, hashable identity of a brush asset. Mirrors #AssetWeakReference's fields, but owns its
 * strings as `std::string` instead of raw asset-system-managed `const char *`, so it can safely
 * live inside a long-lived cache/#Set/#Map key.
 */
struct BrushAssetRef {
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
  static BrushAssetRef from_weak_reference(const AssetWeakReference &weak_ref);

  uint64_t hash() const;
  bool operator==(const BrushAssetRef &other) const;
};

/** Recent/favorite brush-asset lists for a single asset shelf type. */
struct ShelfBrushLists {
  /** Most-recently-used first. Capped at #BRUSH_ASSET_LISTS_RECENT_MAX. */
  Vector<BrushAssetRef> recent;
  VectorSet<BrushAssetRef> favorites;
};

/** Maximum number of entries kept in #ShelfBrushLists::recent. */
constexpr int BRUSH_ASSET_LISTS_RECENT_MAX = 20;

/**
 * Insert/move \a ref to the front of \a recent (most-recently-used first), trimming the list to
 * \a max_count. Pure function, no file I/O -- exercised directly by unit tests.
 */
void record_recent_into(Vector<BrushAssetRef> &recent, BrushAssetRef ref, int max_count);

/**
 * Add \a ref to \a favorites if not already present, otherwise remove it. Pure function, no file
 * I/O -- exercised directly by unit tests.
 */
void toggle_favorite_into(VectorSet<BrushAssetRef> &favorites, const BrushAssetRef &ref);

/** Serialize all shelves' lists to a JSON document value (`{"version", "shelves"}`). */
std::shared_ptr<io::serialize::DictionaryValue> lists_to_json(
    const Map<std::string, ShelfBrushLists> &shelves);

/**
 * Deserialize a JSON document value produced by #lists_to_json(). Unresolvable/malformed entries
 * are silently skipped rather than failing the whole document.
 */
Map<std::string, ShelfBrushLists> lists_from_json(const io::serialize::Value &root);

/** Canonical brush asset-shelf idname for \a mode, or nullptr if \a mode has no brush shelf. */
const char *brush_shelf_idname_from_paint_mode(PaintMode mode);

/** True if \a idname is one of the brush asset shelves from #brush_shelf_idname_from_paint_mode(). */
bool shelf_idname_is_brush_shelf(StringRef idname);

/**
 * Record \a weak_ref as the most-recently-used brush asset for \a shelf_idname, trimming to
 * #BRUSH_ASSET_LISTS_RECENT_MAX. Loads the cache from disk on first call this session and
 * persists the change back to disk immediately.
 */
void brush_lists_record_recent(StringRef shelf_idname, const AssetWeakReference &weak_ref);

bool brush_lists_is_favorite(StringRef shelf_idname, const AssetWeakReference &weak_ref);

/** Add/remove \a weak_ref from \a shelf_idname's favorites. Persists the change to disk. */
void brush_lists_toggle_favorite(StringRef shelf_idname, const AssetWeakReference &weak_ref);

Span<BrushAssetRef> brush_lists_recent(StringRef shelf_idname);
Span<BrushAssetRef> brush_lists_favorites(StringRef shelf_idname);

}  // namespace blender::ed::asset::shelf
