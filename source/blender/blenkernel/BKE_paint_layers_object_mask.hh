/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"
#include "DNA_uuid_types.h"

namespace blender {

struct Material;
struct MaterialPaintLayer;
struct Object;
struct ObjectPaintLayerMask;
struct Main;

namespace bke {

/** First and last UDIM tile a mask can address: 24 bits fit a float exactly. */
inline constexpr int PAINT_LAYER_OBJECT_MASK_TILE_FIRST = 1001;
inline constexpr int PAINT_LAYER_OBJECT_MASK_TILE_NUM = 24;
inline constexpr int PAINT_LAYER_OBJECT_MASK_TILE_LAST = PAINT_LAYER_OBJECT_MASK_TILE_FIRST +
                                                         PAINT_LAYER_OBJECT_MASK_TILE_NUM - 1;
/** Tiles per UDIM row (u = 0..9); shared by the tile scan and the shader build chain. */
inline constexpr int PAINT_LAYER_OBJECT_MASK_UDIM_COLUMNS = 10;
/**
 * Length of #BKE_paint_layer_object_mask_key without the terminator: 16 hex digits of a 64-bit
 * hash, short enough that the Attribute path fits a GPU uniform attribute name (67 characters).
 */
inline constexpr int PAINT_LAYER_OBJECT_MASK_KEY_LEN = 16;

}  // namespace bke

/** The record key `<owner_uid hex>_<marker hex>`; the single place the format is defined. */
std::array<char, bke::PAINT_LAYER_OBJECT_MASK_KEY_LEN + 1> BKE_paint_layer_object_mask_key(
    const bUUID &owner_uid, const bUUID &marker);

/** RNA path of the shader-facing float for \a layer of \a ma, Mesh or UV Tiles by its type. */
std::string BKE_paint_layer_object_mask_attribute_path(const Material &ma,
                                                       const MaterialPaintLayer &layer);

const ObjectPaintLayerMask *BKE_paint_layer_object_mask_find(const Object &ob,
                                                             const bUUID &owner_uid,
                                                             const bUUID &marker);
/** Find by the RNA key string; null for a malformed key or no record. */
ObjectPaintLayerMask *BKE_paint_layer_object_mask_find_by_key(Object &ob, const char *key);

/** Hide/show \a layer on \a ob (Mesh mode). Ensures \a ma has an owner uid. Returns true on change.
 *
 * Sets #MA_PAINT_LAYER_OBJECT_MASK_ACTIVE on first deviation, so a BKE-only caller builds the same
 * topology as the RNA path. The flag flip itself counts as a change. The flag is sticky: showing the
 * last hidden object keeps it, so toggling never rebuilds the graph; see
 * #BKE_paint_layer_object_masks_cleanup_all. */
bool BKE_paint_layer_object_mask_visible_set(
    Main &bmain, Object &ob, Material &ma, MaterialPaintLayer &layer, bool visible);
/** Hide/show UDIM \a tile of \a layer on \a ob (UV Tiles mode). False for a tile out of range.
 * See #BKE_paint_layer_object_mask_visible_set for the flag contract. */
bool BKE_paint_layer_object_mask_tile_set(
    Main &bmain, Object &ob, Material &ma, MaterialPaintLayer &layer, int tile, bool visible);
/** Clear #MA_PAINT_LAYER_OBJECT_MASK_ACTIVE when no object in \a bmain holds a record for the row. */
void BKE_paint_layer_object_mask_active_update(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer &layer);
/** True when UDIM \a tile is hidden by \a bits. The single place the tile-to-bit rule lives. */
inline bool BKE_paint_layer_object_mask_tile_excluded(const uint32_t bits, const int tile)
{
  const int bit = tile - bke::PAINT_LAYER_OBJECT_MASK_TILE_FIRST;
  if (bit < 0 || bit >= bke::PAINT_LAYER_OBJECT_MASK_TILE_NUM) {
    return false;
  }
  return (bits & (1u << uint32_t(bit))) != 0;
}
bool BKE_paint_layer_object_mask_object_visible_get(const Object &ob,
                                                    const Material &ma,
                                                    const MaterialPaintLayer &layer);
bool BKE_paint_layer_object_mask_tile_visible_get(const Object &ob,
                                                  const Material &ma,
                                                  const MaterialPaintLayer &layer,
                                                  int tile);

/**
 * The one visibility answer for \a layer on \a ob at UDIM \a tile (0 = any tile / not a tile
 * question): the row's own mask and every ancestor folder's, each read in its own mode.
 */
bool BKE_paint_layer_object_mask_visible(const Object &ob,
                                         const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         int tile);
/** Bits of the tiles hidden for \a layer on \a ob, OR-ed over the row and its ancestors in UV Tiles mode. */
uint32_t BKE_paint_layer_object_mask_excluded_tiles(const Object &ob,
                                                    const Material &ma,
                                                    const MaterialPaintLayer &layer);

/** Sorted UDIM tiles \a ob's faces using \a ma occupy in the material's UV map. */
Vector<int> BKE_paint_layer_object_mask_object_tiles(const Object &ob, const Material &ma);
/**
 * Same as #BKE_paint_layer_object_mask_object_tiles, memoized in the object's runtime data (not
 * saved, dropped with undo/file load). With \a refresh the scan is redone and the entry replaced.
 */
Vector<int> BKE_paint_layer_object_mask_object_tiles_cached(const Object &ob,
                                                            const Material &ma,
                                                            bool refresh);
/** Fill the tile cache of every mesh object for each layered material it uses: run on entering
 * mask editing so the panel redraw only reads the cache. */
void BKE_paint_layer_object_mask_tiles_prefetch(Main &bmain);

/** Whether any descendant (children, recursively) of \a layer has an object mask: the folder's own
 * bake would freeze one object's choice for all of them. The row's own mask does not block. */
bool BKE_paint_layers_object_mask_blocks_bake(const MaterialPaintLayer &layer);

/** Drop records whose owner uid names no layered material in \a bmain or whose marker names no row. */
void BKE_paint_layer_object_masks_cleanup(Main &bmain, Object &ob);
/** Run #BKE_paint_layer_object_masks_cleanup over every object: pre-save hook; the RNA row remove
 * and the Outliner stack row remove call it too. Other C++ row-delete paths leave stale records
 * until the next save. Also clears the
 * sticky #MA_PAINT_LAYER_OBJECT_MASK_ACTIVE of rows no object holds a record for (tags a regen). */
void BKE_paint_layer_object_masks_cleanup_all(Main &bmain);

/** UID-2: give every second-or-later local layered material sharing an owner uid a fresh one.
 *
 * Linked materials own their uid by priority and are never renamed; only local editable duplicates
 * are. Library overrides share the uid of their reference by design. */
void BKE_paint_layers_owner_uid_ensure_unique(Main &bmain);
/** Rewrite records from \a old_uid to the uid of \a new_owner on objects using it without the old.
 *
 * The shared duplicate rule: an object using both keeps the old records (unresolvable), one using
 * only the new owner follows it. Used by #BKE_paint_layers_owner_uid_ensure_unique and the
 * object/material duplicate paths. */
void BKE_paint_layer_object_masks_owner_remap(Main &bmain,
                                               const bUUID &old_uid,
                                               Material &new_owner);
/** Rewrite \a ob_dst records from source materials to their duplicated counterparts.
 *
 * Compares material slots of \a ob_src and \a ob_dst: every slot whose material changed (a
 * USER_DUP_MAT duplicate with a fresh owner uid) remaps that object's records from the source uid
 * to the copy's uid. Call after #BKE_libblock_relink_to_newid swapped the duplicate's slots. */
void BKE_paint_layer_object_masks_duplicate_remap(Object &ob_dst, const Object &ob_src);
/** Run #BKE_paint_layer_object_masks_duplicate_remap for every object that has an ID::newid copy.
 *
 * For paths that relink through #BKE_libblock_relink_to_newid (collection / scene copy): call it
 * after the relink and before #BKE_main_id_newptr_and_tag_clear. The remap is idempotent, so
 * editor paths that already remap per object may overlap with it. */
void BKE_paint_layer_object_masks_remap_from_newid(Main &bmain);

void BKE_paint_layer_object_masks_copy(Object &ob_dst, const Object &ob_src);
void BKE_paint_layer_object_masks_free(Object &ob);

}  // namespace blender
