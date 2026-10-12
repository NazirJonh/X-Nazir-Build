/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Per-object visibility of layered-material rows (Object / UV Tiles mask).
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <optional>

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_object_mask.hh"

#include "DEG_depsgraph.hh"

namespace blender {
using namespace blender::bke;

/** SplitMix64 finalizer: spreads every input bit over the whole word. */
static uint64_t hash_mix64(uint64_t x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

std::array<char, PAINT_LAYER_OBJECT_MASK_KEY_LEN + 1> BKE_paint_layer_object_mask_key(
    const bUUID &owner_uid, const bUUID &marker)
{
  /* A 64-bit hash of both uids rather than their full hex: the key is part of an Attribute node
   * name, and GPU uniform attribute names are cut at 67 characters (`GPUUniformAttr::name`), which
   * two full uids would overflow -- the cut path then resolves to nothing and reads as "visible". */
  uint64_t words[4];
  static_assert(sizeof(bUUID) == 2 * sizeof(uint64_t));
  memcpy(&words[0], &owner_uid, sizeof(bUUID));
  memcpy(&words[2], &marker, sizeof(bUUID));
  uint64_t hash = 0x9e3779b97f4a7c15ull;
  for (const uint64_t word : words) {
    hash = hash_mix64(hash ^ word);
  }
  std::array<char, PAINT_LAYER_OBJECT_MASK_KEY_LEN + 1> key;
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < PAINT_LAYER_OBJECT_MASK_KEY_LEN; i++) {
    key[i] = digits[(hash >> (60 - 4 * i)) & 0xF];
  }
  key[PAINT_LAYER_OBJECT_MASK_KEY_LEN] = '\0';
  return key;
}

std::string BKE_paint_layer_object_mask_attribute_path(const Material &ma,
                                                       const MaterialPaintLayer &layer)
{
  const auto key = BKE_paint_layer_object_mask_key(ma.paint_layers_owner_uid, layer.marker);
  const char *field = (layer.object_mask_type == MA_PAINT_LAYER_OBJECT_MASK_UV_TILES) ?
                          "excluded_tiles_value" :
                          "excluded_value";
  return fmt::format("paint_layer_masks[\"{}\"].{}", key.data(), field);
}

const ObjectPaintLayerMask *BKE_paint_layer_object_mask_find(const Object &ob,
                                                             const bUUID &owner_uid,
                                                             const bUUID &marker)
{
  for (const ObjectPaintLayerMask &rec : ob.paint_layer_masks) {
    if (BLI_uuid_equal(rec.owner_uid, owner_uid) && BLI_uuid_equal(rec.marker, marker)) {
      return &rec;
    }
  }
  return nullptr;
}

ObjectPaintLayerMask *BKE_paint_layer_object_mask_find_by_key(Object &ob, const char *key)
{
  if (key == nullptr || strlen(key) != PAINT_LAYER_OBJECT_MASK_KEY_LEN) {
    return nullptr;
  }
  for (ObjectPaintLayerMask &rec : ob.paint_layer_masks) {
    if (STREQ(BKE_paint_layer_object_mask_key(rec.owner_uid, rec.marker).data(), key)) {
      return &rec;
    }
  }
  return nullptr;
}

/** A material that has rows but never got an owner uid gets one: the key needs it.
 *
 * Routes through #BKE_paint_layers_owner_uid_changed so a generated tree or instance node keyed on
 * the old (nil) uid follows the new one instead of being orphaned. */
static void owner_uid_ensure(Main &bmain, Material &ma)
{
  if (BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    const bUUID old_uid = ma.paint_layers_owner_uid;
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
    BKE_paint_layers_owner_uid_changed(bmain, ma, old_uid);
  }
}

/* Sync the shader-facing mirrors; prune the record when it hides nothing. Returns true when the
 * record was removed, so the caller's reference is dead afterwards. */
static bool record_sync_and_prune(Object &ob, ObjectPaintLayerMask &rec)
{
  rec.excluded_tiles &= 0xFFFFFF;
  rec.excluded_value = (rec.flag & OB_PAINT_LAYER_MASK_EXCLUDED) ? 1.0f : 0.0f;
  rec.excluded_tiles_value = float(uint32_t(rec.excluded_tiles));
  /* Only deviations are stored, so a record that hides nothing goes away. */
  if (rec.flag == 0 && rec.excluded_tiles == 0) {
    BLI_remlink(&ob.paint_layer_masks, &rec);
    MEM_delete(&rec);
    return true;
  }
  return false;
}

static ObjectPaintLayerMask &record_ensure(Main &bmain,
                                           Object &ob,
                                           Material &ma,
                                           const MaterialPaintLayer &layer)
{
  owner_uid_ensure(bmain, ma);
  if (ObjectPaintLayerMask *rec = const_cast<ObjectPaintLayerMask *>(
          BKE_paint_layer_object_mask_find(ob, ma.paint_layers_owner_uid, layer.marker)))
  {
    return *rec;
  }
  ObjectPaintLayerMask *rec = MEM_new<ObjectPaintLayerMask>(__func__);
  rec->owner_uid = ma.paint_layers_owner_uid;
  rec->marker = layer.marker;
  BLI_addtail(&ob.paint_layer_masks, rec);
  return *rec;
}

bool BKE_paint_layer_object_mask_visible_set(
    Main &bmain, Object &ob, Material &ma, MaterialPaintLayer &layer, const bool visible)
{
  /* Showing a row with no record changes nothing: skip the record, the uid and the tagging. */
  if (visible && BKE_paint_layer_object_mask_find(ob, ma.paint_layers_owner_uid, layer.marker) ==
                     nullptr)
  {
    return false;
  }
  ObjectPaintLayerMask &rec = record_ensure(bmain, ob, ma, layer);
  const int8_t old_flag = rec.flag;
  const bool was_active = (layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0;
  SET_FLAG_FROM_TEST(rec.flag, !visible, OB_PAINT_LAYER_MASK_EXCLUDED);
  const bool changed = rec.flag != old_flag;
  const bool removed = record_sync_and_prune(ob, rec);
  /* A deviation makes the row masked, which is topology for the generator. The flag is sticky:
   * showing the last hidden object again only changes the Attribute value, so toggling never
   * rebuilds the graph. Unused flags are dropped by #BKE_paint_layer_object_masks_cleanup_all. */
  if (!removed && changed) {
    layer.object_mask_flag |= MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  }
  const bool now_active = (layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0;
  return changed || (was_active != now_active);
}

bool BKE_paint_layer_object_mask_tile_set(Main &bmain,
                                           Object &ob,
                                           Material &ma,
                                           MaterialPaintLayer &layer,
                                           const int tile,
                                           const bool visible)
{
  const int bit = tile - PAINT_LAYER_OBJECT_MASK_TILE_FIRST;
  if (bit < 0 || bit >= PAINT_LAYER_OBJECT_MASK_TILE_NUM) {
    return false;
  }
  if (visible && BKE_paint_layer_object_mask_find(ob, ma.paint_layers_owner_uid, layer.marker) ==
                     nullptr)
  {
    return false;
  }
  ObjectPaintLayerMask &rec = record_ensure(bmain, ob, ma, layer);
  const int old_tiles = rec.excluded_tiles;
  const bool was_active = (layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0;
  SET_FLAG_FROM_TEST(rec.excluded_tiles, !visible, 1u << uint32_t(bit));
  const bool changed = rec.excluded_tiles != old_tiles;
  const bool removed = record_sync_and_prune(ob, rec);
  /* Sticky flag, see #BKE_paint_layer_object_mask_visible_set. */
  if (!removed && changed) {
    layer.object_mask_flag |= MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  }
  const bool now_active = (layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0;
  return changed || (was_active != now_active);
}

void BKE_paint_layer_object_mask_active_update(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer &layer)
{
  if ((layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) == 0) {
    return;
  }
  for (const Object &ob : bmain.objects) {
    if (BKE_paint_layer_object_mask_find(ob, ma.paint_layers_owner_uid, layer.marker) != nullptr) {
      return;
    }
  }
  layer.object_mask_flag &= ~MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
}

bool BKE_paint_layer_object_mask_object_visible_get(const Object &ob,
                                                    const Material &ma,
                                                    const MaterialPaintLayer &layer)
{
  const ObjectPaintLayerMask *rec = BKE_paint_layer_object_mask_find(
      ob, ma.paint_layers_owner_uid, layer.marker);
  return rec == nullptr || (rec->flag & OB_PAINT_LAYER_MASK_EXCLUDED) == 0;
}

bool BKE_paint_layer_object_mask_tile_visible_get(const Object &ob,
                                                  const Material &ma,
                                                  const MaterialPaintLayer &layer,
                                                  const int tile)
{
  if (tile < PAINT_LAYER_OBJECT_MASK_TILE_FIRST ||
      tile >= PAINT_LAYER_OBJECT_MASK_TILE_FIRST + PAINT_LAYER_OBJECT_MASK_TILE_NUM)
  {
    return true;
  }
  const ObjectPaintLayerMask *rec = BKE_paint_layer_object_mask_find(
      ob, ma.paint_layers_owner_uid, layer.marker);
  return rec == nullptr ||
         !BKE_paint_layer_object_mask_tile_excluded(uint32_t(rec->excluded_tiles), tile);
}

/* Parent lookup only reads the description. */
static const MaterialPaintLayer *parent_of(const Material &ma, const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_parent(const_cast<Material &>(ma), layer);
}

bool BKE_paint_layer_object_mask_visible(const Object &ob,
                                         const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         const int tile)
{
  for (const MaterialPaintLayer *cur = &layer; cur != nullptr; cur = parent_of(ma, *cur)) {
    if ((cur->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) == 0) {
      continue;
    }
    switch (cur->object_mask_type) {
      case MA_PAINT_LAYER_OBJECT_MASK_MESH:
        if (!BKE_paint_layer_object_mask_object_visible_get(ob, ma, *cur)) {
          return false;
        }
        break;
      case MA_PAINT_LAYER_OBJECT_MASK_UV_TILES:
        if (tile != 0 && !BKE_paint_layer_object_mask_tile_visible_get(ob, ma, *cur, tile)) {
          return false;
        }
        break;
    }
  }
  return true;
}

uint32_t BKE_paint_layer_object_mask_excluded_tiles(const Object &ob,
                                                    const Material &ma,
                                                    const MaterialPaintLayer &layer)
{
  uint32_t bits = 0;
  for (const MaterialPaintLayer *cur = &layer; cur != nullptr; cur = parent_of(ma, *cur)) {
    if ((cur->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) == 0 ||
        cur->object_mask_type != MA_PAINT_LAYER_OBJECT_MASK_UV_TILES)
    {
      continue;
    }
    if (const ObjectPaintLayerMask *rec = BKE_paint_layer_object_mask_find(
            ob, ma.paint_layers_owner_uid, cur->marker))
    {
      bits |= uint32_t(rec->excluded_tiles) & 0xFFFFFFu;
    }
  }
  return bits;
}

Vector<int> BKE_paint_layer_object_mask_object_tiles(const Object &ob, const Material &ma)
{
  if (ob.type != OB_MESH || ob.data == nullptr) {
    return {};
  }
  /* Original mesh by design: the panel lists tiles of the authored UVs, not modifier output. */
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  bool uv_missing = false;
  const char *uv_name = BKE_paint_layers_uv_map_resolve(mesh, ma, &uv_missing);
  if (uv_name == nullptr || uv_missing) {
    return {};
  }
  const auto &attributes = mesh.attributes();
  const auto uv_attr = attributes.lookup<float2>(uv_name, bke::AttrDomain::Corner);
  if (!uv_attr) {
    return {};
  }
  const VArraySpan<float2> uvs(*uv_attr);

  Set<int> slot_indices;
  Object *ob_mut = const_cast<Object *>(&ob);
  for (int slot = 0; slot < ob.totcol; slot++) {
    if (BKE_object_material_get(ob_mut, short(slot + 1)) == &ma) {
      slot_indices.add(slot);
    }
  }
  if (slot_indices.is_empty()) {
    return {};
  }

  const VArray<int> mat_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Face, 0);

  Set<int> found_tiles;
  const OffsetIndices<int> faces = mesh.faces();
  for (const int face_i : faces.index_range()) {
    if (!slot_indices.contains(mat_indices[face_i])) {
      continue;
    }
    const IndexRange face_corners = faces[face_i];
    if (face_corners.is_empty()) {
      continue;
    }
    /* Every tile a corner touches: the shader tests per fragment and the stroke paints per tile,
     * so a straddling face must list all of them to stay toggleable. */
    for (const int corner_i : face_corners) {
      const float2 uv = uvs[corner_i];
      const int u_floor = int(std::floor(uv.x));
      const int v_floor = int(std::floor(uv.y));
      if (u_floor >= 0 && u_floor < PAINT_LAYER_OBJECT_MASK_UDIM_COLUMNS && v_floor >= 0) {
        const int tile = PAINT_LAYER_OBJECT_MASK_TILE_FIRST + u_floor +
                         PAINT_LAYER_OBJECT_MASK_UDIM_COLUMNS * v_floor;
        found_tiles.add(tile);
      }
    }
  }

  Vector<int> sorted_tiles(found_tiles.begin(), found_tiles.end());
  std::sort(sorted_tiles.begin(), sorted_tiles.end());
  return sorted_tiles;
}

Vector<int> BKE_paint_layer_object_mask_object_tiles_cached(const Object &ob,
                                                            const Material &ma,
                                                            const bool refresh)
{
  if (ob.type != OB_MESH || ob.data == nullptr || ob.runtime == nullptr) {
    return {};
  }
  /* The UV map name is part of the key so that switching the material's UV map misses the cache. */
  bool uv_missing = false;
  const char *uv_name = BKE_paint_layers_uv_map_resolve(
      *id_cast<const Mesh *>(ob.data), ma, &uv_missing);
  char uid_str[UUID_STRING_SIZE];
  BLI_uuid_format(uid_str, ma.paint_layers_owner_uid);
  const std::string key = fmt::format("{}|{}", uid_str, uv_name ? uv_name : "");

  /* The runtime is shared by const access: this is a cache, written from the main thread only. */
  Map<std::string, Vector<int>> &cache = ob.runtime->paint_layer_mask_tiles;
  if (!refresh) {
    if (const Vector<int> *found = cache.lookup_ptr(key)) {
      return *found;
    }
  }
  Vector<int> tiles = BKE_paint_layer_object_mask_object_tiles(ob, ma);
  cache.add_overwrite(key, tiles);
  return tiles;
}

void BKE_paint_layer_object_mask_tiles_prefetch(Main &bmain)
{
  for (Object &ob : bmain.objects) {
    if (ob.type != OB_MESH) {
      continue;
    }
    for (int slot = 0; slot < ob.totcol; slot++) {
      const Material *ma = BKE_object_material_get(&ob, short(slot + 1));
      if (ma != nullptr && !BLI_uuid_is_nil(ma->paint_layers_owner_uid)) {
        BKE_paint_layer_object_mask_object_tiles_cached(ob, *ma, false);
      }
    }
  }
}

bool BKE_paint_layers_object_mask_blocks_bake(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &child : layer.children) {
    if ((child.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
        BKE_paint_layers_object_mask_blocks_bake(child))
    {
      return true;
    }
  }
  return false;
}

static Map<UUID, Material *> owner_map_build(Main &bmain)
{
  Map<UUID, Material *> owner_map;
  for (Material &ma : bmain.materials) {
    if (!BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
      owner_map.add(UUID(ma.paint_layers_owner_uid), &ma);
    }
  }
  return owner_map;
}

static void masks_cleanup_with_map(const Map<UUID, Material *> &owner_map, Object &ob)
{
  for (ObjectPaintLayerMask *rec = static_cast<ObjectPaintLayerMask *>(ob.paint_layer_masks.first),
                             *next = nullptr;
       rec != nullptr;
       rec = next)
  {
    next = rec->next;
    Material *ma = owner_map.lookup_default(UUID(rec->owner_uid), nullptr);
    if (ma == nullptr || BKE_paint_layers_find(*ma, rec->marker) == nullptr) {
      BLI_remlink(&ob.paint_layer_masks, rec);
      MEM_delete(rec);
    }
  }
}

void BKE_paint_layer_object_masks_cleanup(Main &bmain, Object &ob)
{
  if (BLI_listbase_is_empty(&ob.paint_layer_masks)) {
    return;
  }
  masks_cleanup_with_map(owner_map_build(bmain), ob);
}

void BKE_paint_layer_object_masks_cleanup_all(Main &bmain)
{
  /* One owner map for all objects, built lazily: most objects have no records. */
  std::optional<Map<UUID, Material *>> owner_map;
  for (Object &ob : bmain.objects) {
    if (BLI_listbase_is_empty(&ob.paint_layer_masks)) {
      continue;
    }
    if (!owner_map) {
      owner_map = owner_map_build(bmain);
    }
    masks_cleanup_with_map(*owner_map, ob);
  }

  /* The mask flag is sticky while editing (so toggles never rebuild the graph); here, at a
   * non-interactive point, rows that no object hides anything for drop it and their nodes. */
  Set<std::pair<UUID, UUID>> used;
  for (const Object &ob : bmain.objects) {
    for (const ObjectPaintLayerMask &rec : ob.paint_layer_masks) {
      used.add({UUID(rec.owner_uid), UUID(rec.marker)});
    }
  }
  for (Material &ma : bmain.materials) {
    if (BLI_uuid_is_nil(ma.paint_layers_owner_uid) || !ID_IS_EDITABLE(&ma.id)) {
      continue;
    }
    Vector<const MaterialPaintLayer *> rows;
    BKE_paint_layers_flatten_all(ma, rows);
    bool cleared = false;
    for (const MaterialPaintLayer *row : rows) {
      if ((row->object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) == 0 ||
          used.contains({UUID(ma.paint_layers_owner_uid), UUID(row->marker)}))
      {
        continue;
      }
      /* The rows belong to `ma`, which is mutable here; the flatten helper is const only. */
      const_cast<MaterialPaintLayer *>(row)->object_mask_flag &= ~MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
      cleared = true;
    }
    if (cleared) {
      BKE_paint_layers_tag_edited(ma);
      DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
    }
  }
}

void BKE_paint_layer_object_masks_owner_remap(Main &bmain,
                                               const bUUID &old_uid,
                                               Material &new_owner)
{
  /* Records follow the new owner only on objects that use it and not the old one: on an object
   * using both, the records stay with the old owner (the only unresolvable case). */
  for (Object &ob : bmain.objects) {
    bool uses_new = false, uses_old = false;
    for (int slot = 1; slot <= ob.totcol; slot++) {
      const Material *slot_ma = BKE_object_material_get(&ob, short(slot));
      if (slot_ma == nullptr) {
        continue;
      }
      uses_new |= (slot_ma == &new_owner);
      uses_old |= BLI_uuid_equal(slot_ma->paint_layers_owner_uid, old_uid);
    }
    if (!uses_new || uses_old) {
      continue;
    }
    for (ObjectPaintLayerMask &rec : ob.paint_layer_masks) {
      if (BLI_uuid_equal(rec.owner_uid, old_uid)) {
        rec.owner_uid = new_owner.paint_layers_owner_uid;
      }
    }
    DEG_id_tag_update(&ob.id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_SHADING);
  }
}

void BKE_paint_layer_object_masks_duplicate_remap(Object &ob_dst, const Object &ob_src)
{
  const int slots = std::min(ob_dst.totcol, ob_src.totcol);
  for (int slot = 1; slot <= slots; slot++) {
    const Material *ma_src = BKE_object_material_get(const_cast<Object *>(&ob_src), short(slot));
    Material *ma_dst = BKE_object_material_get(&ob_dst, short(slot));
    if (ma_src == nullptr || ma_dst == nullptr || ma_src == ma_dst) {
      continue;
    }
    if (BLI_uuid_is_nil(ma_src->paint_layers_owner_uid) ||
        BLI_uuid_is_nil(ma_dst->paint_layers_owner_uid) ||
        BLI_uuid_equal(ma_src->paint_layers_owner_uid, ma_dst->paint_layers_owner_uid))
    {
      continue;
    }
    for (ObjectPaintLayerMask &rec : ob_dst.paint_layer_masks) {
      if (BLI_uuid_equal(rec.owner_uid, ma_src->paint_layers_owner_uid)) {
        rec.owner_uid = ma_dst->paint_layers_owner_uid;
      }
    }
  }
}

void BKE_paint_layer_object_masks_remap_from_newid(Main &bmain)
{
  for (Object &ob : bmain.objects) {
    if (ob.id.newid != nullptr && GS(ob.id.newid->name) == ID_OB) {
      BKE_paint_layer_object_masks_duplicate_remap(*id_cast<Object *>(ob.id.newid), ob);
    }
  }
}

void BKE_paint_layers_owner_uid_ensure_unique(Main &bmain)
{
  Map<UUID, Material *> first_owner;
  /* Linked materials own their uid by priority and are never renamed; register them first so a
   * local material sharing the uid is the one that gets a fresh one. Library overrides share the
   * uid of their reference by design and are skipped like their linked source. */
  for (Material &ma : bmain.materials) {
    if (BLI_uuid_is_nil(ma.paint_layers_owner_uid) || !ID_IS_LINKED(&ma.id)) {
      continue;
    }
    first_owner.add(UUID(ma.paint_layers_owner_uid), &ma);
  }
  /* Deterministic order: bmain.materials iterates by name, so the "first" owner is stable. */
  for (Material &ma : bmain.materials) {
    if (BLI_uuid_is_nil(ma.paint_layers_owner_uid) || !ID_IS_EDITABLE(&ma.id) ||
        ID_IS_LINKED(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id))
    {
      continue;
    }
    Material *&first = first_owner.lookup_or_add(UUID(ma.paint_layers_owner_uid), &ma);
    if (first == &ma) {
      continue;
    }
    const bUUID old_uid = ma.paint_layers_owner_uid;
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
    BKE_paint_layers_owner_uid_changed(bmain, ma, old_uid);
    BKE_paint_layer_object_masks_owner_remap(bmain, old_uid, ma);
  }
}

void BKE_paint_layer_object_masks_copy(Object &ob_dst, const Object &ob_src)
{
  ob_dst.paint_layer_masks = {nullptr, nullptr};
  for (const ObjectPaintLayerMask &src :
       *reinterpret_cast<const ListBaseT<ObjectPaintLayerMask> *>(&ob_src.paint_layer_masks))
  {
    ObjectPaintLayerMask *dst = MEM_dupalloc(&src);
    dst->next = nullptr;
    dst->prev = nullptr;
    BLI_addtail(&ob_dst.paint_layer_masks, dst);
  }
}

void BKE_paint_layer_object_masks_free(Object &ob)
{
  for (ObjectPaintLayerMask *rec = static_cast<ObjectPaintLayerMask *>(ob.paint_layer_masks.first),
                            *next = nullptr;
       rec != nullptr;
       rec = next)
  {
    next = rec->next;
    MEM_delete(rec);
  }
  ob.paint_layer_masks = {nullptr, nullptr};
}

}  // namespace blender
