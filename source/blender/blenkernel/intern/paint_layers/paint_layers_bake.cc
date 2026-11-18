/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The row bake of a layered material: the cache key, the service maps the generator and the CPU
 * substitute for a row's live subtree, the planner that fills them and the worker job that does
 * the heavy ones off the main thread. The description and its edits live in `paint_layers.cc`; the
 * helpers both files need are in `paint_layers_intern.hh`, the bake's own in
 * `paint_layers_bake_intern.hh`. The structural validity of a bake (the hash) lives in
 * `paint_layers_bake_hash.cc`, and the pixel half of it (the subscription drain and the coverage
 * culling) in `paint_layers_bake_coverage.cc`.
 *
 * A baked row is a *cache*: the description plus its live maps stay what the row is, and the bake is
 * read instead of the subtree only while its stored hash matches the description and no pixel of a
 * source map changed since (see #BKE_paint_layers_bake_is_valid). Everything that decides whether a
 * row is substituted goes through #paint_layer_row_substituted, so the generator and the CPU
 * composite cannot disagree about it.
 *
 * What the cache holds is the other question (#paint_layer_row_bakes_content), and it has two
 * answers: a row that carries a CPU-computed effect caches its *content* alone, while every other
 * row caches the row as it looks. Every layer of that answer reads the one scope
 * (#paint_layer_bake_scope), so a mask stroke costs a content bake nothing.
 *
 * TODO: this file is close to the project's size limit; the next split moves #bake_tiles_ready and
 * #bake_node_is_shared (with the planner) into `paint_layers_bake_plan.cc`.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_target.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_anim_data.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_key.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_composite_surface.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_scene.hh"

#include "BLI_hash.hh"
#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "../paint_material_composite_projection_internal.hh"
#include "paint_layers_bake_intern.hh"
#include "paint_layers_hash_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"
#include "paint_layers_text_decal_intern.hh"

namespace blender {

static_assert(sizeof(MaterialPaintLayerBake::images) / sizeof(MaterialPaintLayerBake::images[0]) ==
                  PAINT_MATERIAL_CHANNEL_NUM,
              "MaterialPaintLayerBake::images is one map per channel");

/* -------------------------------------------------------------------- */
/** \name Runtime
 *
 * What the bake keeps outside the file, per material and keyed by #ID::session_uid: the pixel-edit
 * subscriptions of the baked rows and the size an AUTO release remembered. Never saved; a load
 * starts empty.
 * \{ */

Map<uint32_t, BakeMaterialRuntime> &bake_runtime()
{
  static Map<uint32_t, BakeMaterialRuntime> map;
  return map;
}

/**
 * Guards #bake_runtime(). The map is written by the main-thread regeneration and read from
 * `BKE_material_eval` through #BKE_paint_layers_bake_is_valid, which runs in the depsgraph task pool
 * and on evaluated copies, so an unlocked Map would be a data race. Every access holds the lock only
 * around the map itself and never calls a function that takes it again.
 */
std::mutex &bake_runtime_mutex()
{
  static std::mutex mutex;
  return mutex;
}

namespace {

/* Whether a paint or sculpt stroke is running right now. The stroke code in the editors hands the
 * state over (see #BKE_paint_layers_bake_stroke_active_set): the bake planner works from a material
 * alone and cannot reach the active Paint without a Scene. Only the deferral of a row with a
 * CPU-computed effect reads it, so a stroke that edits no pixel map changes nothing. Runtime only,
 * never saved. */
std::atomic<bool> &paint_layers_bake_stroke_active()
{
  static std::atomic<bool> active = false;
  return active;
}

}  // namespace

void BKE_paint_layers_bake_runtime_free(Material &ma)
{
  bake_drag_forget_material(ma.id.session_uid);
  BKE_paint_layers_source_material_hash_forget(ma);
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
    /* Only the owning material may drop the entry: a same-uid old ID freed during a memfile undo
     * must leave the re-read ID's subscriptions (and their `PartialUpdateUser`s) in place. */
    if (runtime != nullptr && runtime->owner == &ma) {
      for (BakeLayerSubscription &entry : runtime->layers) {
        for (BakeSubscriptionSource &source : entry.sources) {
          BKE_image_partial_update_free(source.user);
        }
      }
      bake_runtime().remove(ma.id.session_uid);
    }
  }
  text_decal_runtime_free(ma);
}

void BKE_paint_layers_bake_runtime_owner_transfer(Material &dst, Material &src)
{
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(src.id.session_uid);
    if (runtime != nullptr && runtime->owner == &src) {
      runtime->owner = &dst;
    }
  }
  text_decal_runtime_owner_transfer(dst, src);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Row accessors
 * \{ */

MaterialPaintLayerBake *BKE_paint_layers_bake_struct_ensure(MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    layer.bake = MEM_new<MaterialPaintLayerBake>(__func__);
  }
  return layer.bake;
}

/** Drop the per-object tile records of \a bake; the maps and their users are untouched. */
void paint_layers_bake_tiles_clear(MaterialPaintLayerBake &bake)
{
  MaterialPaintLayerBakeTile *tile = static_cast<MaterialPaintLayerBakeTile *>(bake.tiles.first);
  while (tile != nullptr) {
    MaterialPaintLayerBakeTile *next = static_cast<MaterialPaintLayerBakeTile *>(tile->next);
    MEM_delete(tile);
    tile = next;
  }
  BLI_listbase_clear(&bake.tiles);
  bake.object_count = 0;
}

static MaterialPaintLayerBakeTile *bake_tile_find(MaterialPaintLayerBake &bake,
                                                  const uint32_t object_uid)
{
  for (MaterialPaintLayerBakeTile &tile : bake.tiles) {
    if (tile.object_uid == object_uid) {
      return &tile;
    }
  }
  return nullptr;
}

MaterialPaintLayerBakeTile *bake_tile_ensure(MaterialPaintLayerBake &bake,
                                             const uint32_t object_uid,
                                             const int tile_x,
                                             const int tile_y)
{
  if (MaterialPaintLayerBakeTile *tile = bake_tile_find(bake, object_uid)) {
    return tile;
  }
  MaterialPaintLayerBakeTile *tile = MEM_new<MaterialPaintLayerBakeTile>(__func__);
  tile->object_uid = object_uid;
  /* The block origin. A one-tile block at index `i` is `(i % 10, i / 10)`, the placement a record
   * had before blocks, so a saved record reads the same. */
  tile->tile[0] = tile_x;
  tile->tile[1] = tile_y;
  BLI_addtail(&bake.tiles, tile);
  return tile;
}

/**
 * Whether \a layer's maps carry a tile for every object the planner last resolved. The shader may
 * stand in only then: a missing tile would show another object's pixels.
 */
bool bake_tiles_ready(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || layer.bake->object_count <= 0) {
    return false;
  }
  if (BKE_paint_layers_bake_object_issue_get(ma, layer) != PAINT_LAYERS_BAKE_OBJECTS_OK) {
    return false;
  }
  const int tiles = BLI_listbase_count(&layer.bake->tiles);
  return tiles == layer.bake->object_count;
}

/** SplitMix64 finalizer: spreads every input bit over the whole word. */
static uint64_t bake_hash_mix64(uint64_t x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

/**
 * The object property the generated shader reads as its tile index. Keyed on the material's stable
 * owner UUID, so two materials sharing an object do not overwrite each other's index.
 */
std::string paint_layers_bake_tile_attribute_path(const Material &ma)
{
  uint64_t hash = 0x9e3779b97f4a7c15ull;
  if (!BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    uint64_t words[2];
    static_assert(sizeof(bUUID) == sizeof(words));
    memcpy(words, &ma.paint_layers_owner_uid, sizeof(words));
    hash = bake_hash_mix64(hash ^ words[0]);
    hash = bake_hash_mix64(hash ^ words[1]);
  }
  else {
    /* A generated material without an owner UUID: the name is the only stable key it has. */
    for (const char *c = ma.id.name; *c != '\0'; c++) {
      hash = bake_hash_mix64(hash ^ uint64_t(uint8_t(*c)));
    }
  }
  static const char digits[] = "0123456789abcdef";
  std::array<char, 17> key;
  for (int i = 0; i < 16; i++) {
    key[i] = digits[(hash >> (60 - 4 * i)) & 0xF];
  }
  key[16] = '\0';
  return std::string("paint_layer_bake_tile_") + key.data();
}

/**
 * The suffix of the property that carries the row shift of an object's tile block, next to the
 * column shift the plain path carries. A second scalar rather than a vector or a re-typed property:
 * a file saved before blocks holds the column shift as an integer under the plain path, and a shader
 * that reads the row shift finds nothing (zero) there, which is exactly the placement that file's
 * bake was written with.
 */
constexpr char BAKE_TILE_ROW_SUFFIX[] = "_v";

std::string paint_layers_bake_tile_row_attribute_path(const Material &ma)
{
  return paint_layers_bake_tile_attribute_path(ma) + BAKE_TILE_ROW_SUFFIX;
}

/** Set the integer property \a path of \a object_mut to \a value; true when it changed. */
static bool bake_tile_property_int_set(Object &object_mut, const char *path, const int value)
{
  IDProperty *properties = IDP_EnsureProperties(&object_mut.id);
  IDProperty *property = IDP_GetPropertyFromGroup(properties, path);
  if (property == nullptr) {
    IDP_AddToGroup(properties, IDP_NewInt(value, path));
    return true;
  }
  if (property->type != IDP_INT || IDP_int_get(property) == value) {
    return false;
  }
  IDP_int_set(property, value);
  return true;
}

/**
 * Write the shift of \a object's tile block into its two tile properties so the shader can move the
 * UV into the block: \a shift_x columns and \a shift_y rows.
 */
static void bake_tile_object_shift_set(const Object &object,
                                       const char *path,
                                       const char *row_path,
                                       const int shift_x,
                                       const int shift_y)
{
  const Object *original = DEG_get_original(&object);
  Object *object_mut = const_cast<Object *>(original != nullptr ? original : &object);
  const bool changed_x = bake_tile_property_int_set(*object_mut, path, shift_x);
  const bool changed_y = bake_tile_property_int_set(*object_mut, row_path, shift_y);
  if (changed_x || changed_y) {
    /* The shader reads the properties through Attribute nodes: let the depsgraph copy them. */
    DEG_id_tag_update(&object_mut->id, ID_RECALC_SHADING);
  }
}

/** Remove the single tile property \a name \a object carries; true when there was one. */
static bool bake_tile_property_remove(Object &object_mut, const char *name)
{
  IDProperty *properties = IDP_GetProperties(&object_mut.id);
  if (properties == nullptr) {
    return false;
  }
  IDProperty *property = IDP_GetPropertyFromGroup(properties, name);
  if (property == nullptr) {
    return false;
  }
  IDP_FreeFromGroup(properties, property);
  return true;
}

/**
 * Remove the tile shift \a path (and its row companion) left on \a object; no bake of the material
 * reads it any more.
 */
static void bake_tile_object_index_remove(const Object &object,
                                          const char *path,
                                          const bool tag_update)
{
  const Object *original = DEG_get_original(&object);
  Object *object_mut = const_cast<Object *>(original != nullptr ? original : &object);
  const std::string row_path = std::string(path) + BAKE_TILE_ROW_SUFFIX;
  const bool removed_x = bake_tile_property_remove(*object_mut, path);
  const bool removed_y = bake_tile_property_remove(*object_mut, row_path.c_str());
  if (tag_update && (removed_x || removed_y)) {
    /* The shader no longer reads the property: let the depsgraph drop its copy too. */
    DEG_id_tag_update(&object_mut->id, ID_RECALC_SHADING);
  }
}

/** The fixed prefix of the object property the shader reads as its bake tile index. */
constexpr char BAKE_TILE_PROPERTY_PREFIX[] = "paint_layer_bake_tile_";

/**
 * Parse a tile-index property name into the 64-bit key \a paint_layers_bake_tile_attribute_path()
 * encodes after its prefix. Returns false for any other property, so the lazy prune never touches
 * an unrelated Custom Property. The row companion (#BAKE_TILE_ROW_SUFFIX) names the same key.
 */
static bool bake_tile_property_hash(const char *name, uint64_t &r_hash)
{
  constexpr size_t prefix_len = sizeof(BAKE_TILE_PROPERTY_PREFIX) - 1;
  if (strncmp(name, BAKE_TILE_PROPERTY_PREFIX, prefix_len) != 0) {
    return false;
  }
  const char *hex = name + prefix_len;
  const size_t hex_len = strlen(hex);
  if (hex_len != 16 &&
      !(hex_len == 16 + sizeof(BAKE_TILE_ROW_SUFFIX) - 1 &&
        strcmp(hex + 16, BAKE_TILE_ROW_SUFFIX) == 0))
  {
    return false;
  }
  uint64_t value = 0;
  for (int i = 0; i < 16; i++) {
    const char c = hex[i];
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    }
    else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    }
    else {
      return false;
    }
    value = (value << 4) | uint64_t(digit);
  }
  r_hash = value;
  return true;
}

/**
 * Give every target object the shift of its tile block, in the order the bake writes the tiles.
 * The single-tile scheme has the origin `(index, 0)` and no UV minimum, so its column shift is the
 * index the property held before blocks and the row shift is zero.
 */
void bake_tile_object_indices_set(const Material &ma,
                                  const PaintLayersBakeTargets &targets)
{
  const std::string path = paint_layers_bake_tile_attribute_path(ma);
  const std::string row_path = paint_layers_bake_tile_row_attribute_path(ma);
  for (const PaintLayersBakeTarget &target : targets.objects) {
    bake_tile_object_shift_set(*target.object,
                               path.c_str(),
                               row_path.c_str(),
                               target.block_origin[0] - target.uv_tile_min[0],
                               target.block_origin[1] - target.uv_tile_min[1]);
  }
}

Vector<PaintLayersBakeTilePair> bake_target_tile_pairs(const PaintLayersBakeTarget &target,
                                                       const int object_index)
{
  Vector<PaintLayersBakeTilePair> pairs;
  if (target.uv_tiles.is_empty()) {
    /* The single-tile scheme: the object's UVs are rendered where they sit and written to the tile
     * of the object's index. */
    pairs.append({BKE_paint_layers_gradient_surface_uv_tile(*target.mesh, target.uv_map_name),
                  PAINT_TILE_NUMBER_BASE + object_index});
    return pairs;
  }
  for (const int uv_tile : target.uv_tiles) {
    const int column = (uv_tile - PAINT_TILE_NUMBER_BASE) % 10 - target.uv_tile_min[0];
    const int row = (uv_tile - PAINT_TILE_NUMBER_BASE) / 10 - target.uv_tile_min[1];
    pairs.append({uv_tile,
                  PAINT_TILE_NUMBER_BASE + (target.block_origin[0] + column) +
                      10 * (target.block_origin[1] + row)});
  }
  return pairs;
}

Vector<int> bake_targets_image_tiles(const Span<PaintLayersBakeTarget> objects)
{
  Vector<int> tiles;
  for (const int index : objects.index_range()) {
    const PaintLayersBakeTarget &target = objects[index];
    if (target.uv_tiles.is_empty()) {
      tiles.append(PAINT_TILE_NUMBER_BASE + index);
      continue;
    }
    for (const int row : IndexRange(target.block_size[1])) {
      for (const int column : IndexRange(target.block_size[0])) {
        tiles.append(PAINT_TILE_NUMBER_BASE + (target.block_origin[0] + column) +
                     10 * (target.block_origin[1] + row));
      }
    }
  }
  return tiles;
}

/** Whether any row of \a ma still carries per-object tile records. */
static bool bake_material_has_tiles(const Material &ma)
{
  bool has_tiles = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        if (row.bake != nullptr && !BLI_listbase_is_empty(&row.bake->tiles)) {
          has_tiles = true;
          return false;
        }
        /* A mask effect item whose grey reads the surface keeps tiles of its own. */
        for (const MaterialPaintLayer &item : row.mask_stack) {
          if (item.bake != nullptr && !BLI_listbase_is_empty(&item.bake->tiles)) {
            has_tiles = true;
            return false;
          }
        }
        return true;
      });
  return has_tiles;
}

void paint_layers_bake_tile_indices_clear_all(const Material &ma)
{
  if (bake_material_has_tiles(ma)) {
    return;
  }
  paint_layers_bake_tile_indices_remove_everywhere(ma);
}

void paint_layers_bake_tile_index_remove_from_object(Object &object, const Material &ma)
{
  const std::string path = paint_layers_bake_tile_attribute_path(ma);
  bake_tile_object_index_remove(object, path.c_str(), true);
}

void paint_layers_bake_tile_indices_remove_everywhere(const Material &ma, const bool tag_objects)
{
  if (G_MAIN == nullptr) {
    return;
  }
  const std::string path = paint_layers_bake_tile_attribute_path(ma);
  for (Object &object : G_MAIN->objects) {
    if (object.type == OB_MESH) {
      bake_tile_object_index_remove(object, path.c_str(), tag_objects);
    }
  }
}

int BKE_paint_layers_bake_mode_get(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr ? int(layer.bake->mode) : int(MA_PAINT_LAYER_BAKE_AUTO);
}

/**
 * Whether \a layer is a row \a ma keeps a bake for: a row of its stack, a content or mask
 * correction that reads a material, whose maps are rendered from the source like a Material row's,
 * a mask effect item, whose grey is a cache of its own keyed under the item's marker, or a
 * CPU-computed effect that owns its result (#paint_layer_effect_owns_result), whose bake is the
 * effect's result alone.
 */
bool bake_row_owned(Material &ma, const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_owner_list(ma, layer) != nullptr ||
         (paint_layer_mask_item_bakes_effect(layer) &&
          BKE_paint_layers_mask_item_owner(ma, layer) != nullptr) ||
         paint_layer_is_owned_effect(ma, layer) ||
         ((paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ||
           paint_layer_kind_reads_anchor_point(layer)) &&
          (BKE_paint_layers_correction_owner(ma, layer) != nullptr ||
           BKE_paint_layers_mask_item_owner(ma, layer) != nullptr));
}

bool BKE_paint_layers_bake_mode_set(Material &ma, MaterialPaintLayer &layer, const int mode)
{
  if (mode < MA_PAINT_LAYER_BAKE_AUTO || mode > MA_PAINT_LAYER_BAKE_NEVER ||
      !bake_row_owned(ma, layer))
  {
    return false;
  }
  if (mode == MA_PAINT_LAYER_BAKE_NEVER && layer.bake != nullptr) {
    /* Never reads no cache: drop the maps a precreated or earlier bake left, then keep the mode. */
    BKE_paint_layers_bake_clear(ma, layer);
  }
  BKE_paint_layers_bake_struct_ensure(layer)->mode = int8_t(mode);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

int BKE_paint_layers_bake_size_get(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr ? layer.bake->size : 0;
}

bool BKE_paint_layers_bake_size_set(Material &ma, MaterialPaintLayer &layer, const int size)
{
  if (size <= 0 || !bake_row_owned(ma, layer)) {
    return false;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  bake->size = size;
  /* The side is the user's choice, not the size a bake happened to land at: the bit keeps it apart
   * from the canvas a Gradient row would otherwise follow, and dies with the bake structure -- a
   * cleared or Never row follows the canvas and content again. */
  bake->flag |= MA_PAINT_LAYER_BAKE_SIZE_EXPLICIT;
  /* A different resolution invalidates whatever was baked before it. */
  bake->hash[0] = 0;
  bake->hash[1] = 0;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

void BKE_paint_layers_bake_request(MaterialPaintLayer &layer)
{
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* The maps it has keep showing while the new ones render: only the source is called stale. */
    bake->source_hash[0] = 0;
    bake->source_hash[1] = 0;
    return;
  }
  bake->hash[0] = 0;
  bake->hash[1] = 0;
}

bool BKE_paint_layers_bake_stale_get(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0;
}

void BKE_paint_layers_bake_stale_clear(Material &ma)
{
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
}

bool BKE_paint_layers_bake_scheduled_get(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_BAKE_SCHEDULED) != 0;
}

void BKE_paint_layers_bake_scheduled_set(Material &ma, const bool scheduled)
{
  if (scheduled) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_SCHEDULED;
  }
  else {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_SCHEDULED;
  }
}

void BKE_paint_layers_bake_stroke_active_set(const bool active)
{
  paint_layers_bake_stroke_active().store(active);
}

bool BKE_paint_layers_bake_stroke_active_get()
{
  return paint_layers_bake_stroke_active().load();
}

/* The bake of a Decal row is re-rendered at every step of a handle drag; at the canvas side that is
 * hundreds of milliseconds on the main thread, so the drag bakes at this side at most. */
constexpr int BAKE_DRAG_SIZE_MAX = 1024;
/* A drag bake still has to be a valid map side, and the reduced build must cost less than the
 * full one it stands in for. */
static_assert(BAKE_DRAG_SIZE_MAX >= 64 && BAKE_DRAG_SIZE_MAX <= 4096);

/** Let go of the users \a layer's bake holds; the maps stay in `Main` as orphans if nothing else uses them. */
static void bake_release_users(MaterialPaintLayerBake &bake)
{
  for (Image *&image : bake.images) {
    if (image != nullptr) {
      id_us_min(&image->id);
      image = nullptr;
    }
  }
  if (bake.coverage != nullptr) {
    id_us_min(&bake.coverage->id);
    bake.coverage = nullptr;
  }
}

bool BKE_paint_layers_bake_clear(Material &ma, MaterialPaintLayer &layer)
{
  /* A row that goes away cannot owe a full bake later. */
  bake_drag_reduced_forget(ma, layer);
  if (!bake_row_owned(ma, layer)) {
    return false;
  }
  if (layer.bake != nullptr) {
    bake_release_users(*layer.bake);
    paint_layers_bake_tiles_clear(*layer.bake);
    MEM_SAFE_DELETE(layer.bake);
  }
  paint_layers_bake_tile_indices_clear_all(ma);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_bake_set_map(Material &ma,
                                   MaterialPaintLayer &layer,
                                   const int channel,
                                   Image *image)
{
  if (channel < -1 || channel >= PAINT_MATERIAL_CHANNEL_NUM || !bake_row_owned(ma, layer)) {
    return false;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  Image **slot = (channel >= 0) ? &bake->images[channel] : &bake->coverage;
  if (*slot == image) {
    return true;
  }
  if (*slot != nullptr) {
    id_us_min(&(*slot)->id);
  }
  *slot = image;
  if (image != nullptr) {
    id_us_plus(&image->id);
  }
  /* A manually assigned map is not a set of object tiles: whatever coverage the bake had is
   * gone. */
  paint_layers_bake_tiles_clear(*bake);
  paint_layers_bake_tile_indices_clear_all(ma);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pixel-edit subscription
 * \{ */

/** The images the nodes of \a tree read, and those of the groups it reaches, once each. */
static void bake_source_tree_images(const bNodeTree &tree,
                                    Set<const bNodeTree *> &visited,
                                    Vector<Image *> &r_images)
{
  if (!visited.add(&tree)) {
    return;
  }
  for (const bNode *node : tree.all_nodes()) {
    if (node->id == nullptr) {
      continue;
    }
    if (GS(node->id->name) == ID_IM) {
      Image *image = id_cast<Image *>(node->id);
      if (!r_images.contains(image)) {
        r_images.append(image);
      }
    }
    else if (node->is_group()) {
      if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
        bake_source_tree_images(*group_tree, visited, r_images);
      }
    }
  }
}

/**
 * Append the textures a row that reads a material shows live: a stroke on one changes the result
 * at once. A rendered map is not one: it is written by the row's own render, which would then read
 * as an edit of its source and start the render again.
 */
static void bake_collect_live_source_images(const Material &ma,
                                            const MaterialPaintLayer &layer,
                                            Vector<Image *> &r_images)
{
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, layer, channel, PaintLayerMaterialUse::Composite);
    if (source.kind == PaintLayerMaterialSourceKind::LiveImage && source.image != nullptr &&
        !r_images.contains(source.image))
    {
      r_images.append(source.image);
    }
  }
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
      ma, layer, PaintLayerMaterialUse::Composite);
  if (coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha && coverage.image != nullptr &&
      !r_images.contains(coverage.image))
  {
    r_images.append(coverage.image);
  }
}

static void bake_collect_slice_images(const Material &ma,
                                      const MaterialPaintLayer &reader,
                                      Vector<Image *> &r_images,
                                      Vector<bUUID> &visited);

/**
 * Append the maps \a row itself reads to \a r_images: its channel maps, the maps of its bake and,
 * for a row that reads a material, the textures that material shows live. A reader also brings the
 * maps of the slice it reads (see #bake_collect_slice_images).
 */
void bake_collect_row_maps(const Material &ma,
                           const MaterialPaintLayer &row,
                           Vector<Image *> &r_images,
                           Vector<bUUID> &visited)
{
  const auto add = [&](Image *image) {
    if (image != nullptr && !r_images.contains(image)) {
      r_images.append(image);
    }
  };
  for (int i = 0; i < row.channels_num; i++) {
    add(row.channels[i].image);
  }
  /* A kind that reads its own slot (a Channel Split) samples that image: a pixel edit of it moves
   * the bake of every row that carries the kind, as an edit of a channel map does. */
  add(paint_layer_effect_slot_image(row));
  if (row.bake != nullptr) {
    for (Image *image : row.bake->images) {
      add(image);
    }
    add(row.bake->coverage);
  }
  if (paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE)) {
    bake_collect_live_source_images(ma, row, r_images);
  }
  if (paint_layer_kind_reads_anchor_point(row)) {
    bake_collect_slice_images(ma, row, r_images, visited);
  }
}

/** The maps of \a row, its corrections, its mask items and every row below it. */
static void bake_collect_row_tree_maps(const Material &ma,
                                       const MaterialPaintLayer &row,
                                       Vector<Image *> &r_images,
                                       Vector<bUUID> &visited)
{
  bake_collect_row_maps(ma, row, r_images, visited);
  for (const MaterialPaintLayer &effect : row.effects) {
    bake_collect_row_maps(ma, effect, r_images, visited);
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    bake_collect_row_maps(ma, item, r_images, visited);
  }
  for (const MaterialPaintLayer &child : row.children) {
    bake_collect_row_tree_maps(ma, child, r_images, visited);
  }
}

/**
 * The maps \a reader's slice reads: the source row's channels and corrections above the marker,
 * its mask items above the marker, its children, and the maps of nested readers (recursively).
 * The pixel edits to any of them have to mark the reader's cache changed.
 */
static void bake_collect_slice_images(const Material &ma,
                                      const MaterialPaintLayer &reader,
                                      Vector<Image *> &r_images,
                                      Vector<bUUID> &visited)
{
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr) {
    return;
  }
  for (const bUUID &marker : visited) {
    if (BLI_uuid_equal(marker, ref.source_row->marker)) {
      return;
    }
  }
  visited.append(ref.source_row->marker);

  bake_collect_row_maps(ma, *ref.source_row, r_images, visited);
  for (const MaterialPaintLayer &effect : ref.source_row->effects) {
    if (ref.target == PaintLayerAnchorPointTarget::Content && &effect == ref.point) {
      break;
    }
    bake_collect_row_maps(ma, effect, r_images, visited);
  }
  for (const MaterialPaintLayer &item : ref.source_row->mask_stack) {
    if (&item == ref.point) {
      break;
    }
    if (ref.target == PaintLayerAnchorPointTarget::Mask) {
      /* A mask slice includes the base mask. */
      bake_collect_row_maps(ma, item, r_images, visited);
    }
    else if (paint_layer_kind_reads_anchor_point(item)) {
      /* A content slice does not read the mask, but a reader there still names a slice. */
      bake_collect_slice_images(ma, item, r_images, visited);
    }
  }
  for (const MaterialPaintLayer &child : ref.source_row->children) {
    bake_collect_row_tree_maps(ma, child, r_images, visited);
  }
}

void BKE_paint_layers_anchor_point_slice_images(const Material &ma,
                                                const MaterialPaintLayer &reader,
                                                Vector<Image *> &r_images)
{
  Vector<bUUID> visited;
  bake_collect_slice_images(ma, reader, r_images, visited);
}

/**
 * Every map \a layer's bake reads: its own channel maps and those of its corrections, mask items
 * and children. A Material row reads the images of its source's node trees when its own bake is the
 * question, and the maps it shows when it is a child of the row being baked.
 *
 * \a scope says what the bake the maps are collected for holds (#PaintLayerBakeScope): the own
 * mask of a content bake is laid live over the map and renders into none of its pixels, so the
 * mask's maps are not sources and a stroke into them must not mark the cache changed. A child
 * keeps its mask as a source either way -- the parent's cache is a picture of the stack as it
 * looks.
 */
void BKE_paint_layers_bake_collect_source_images(const Material &ma,
                                                 const MaterialPaintLayer &layer,
                                                 Vector<Image *> &r_images,
                                                 const PaintLayerBakeScope scope)
{
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* The grey the item bakes reads the mask built up to it: the base mask's and every item before
     * it's own maps, a nested reader among them bringing its slice's. */
    if (const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, layer)) {
      BKE_paint_layers_mask_effect_input_images(ma, *owner, layer, r_images);
    }
    return;
  }
  if (paint_layer_is_owned_effect(ma, layer)) {
    /* The result the effect bakes reads the row's content below it: the maps that input is rendered
     * from are the effect's sources. Its own bake maps are not -- the planner's write would read as
     * an edit of its source and start the compute again. */
    if (const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, layer)) {
      paint_layer_owned_effect_input_images(ma, *row, layer, r_images);
    }
    return;
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    Vector<bUUID> visited;
    bake_collect_slice_images(ma, layer, r_images, visited);
    /* What it lays is the slice alone; its own effects and mask are live over it, as a Material
     * row's are over its maps. */
    return;
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    if (scope != PaintLayerBakeScope::Child) {
      if (layer.material != nullptr && layer.material->nodetree != nullptr) {
        Set<const bNodeTree *> visited;
        bake_source_tree_images(*layer.material->nodetree, visited, r_images);
      }
    }
    else if (layer.bake != nullptr) {
      for (Image *image : layer.bake->images) {
        if (image != nullptr && !r_images.contains(image)) {
          r_images.append(image);
        }
      }
      if (layer.bake->coverage != nullptr && !r_images.contains(layer.bake->coverage)) {
        r_images.append(layer.bake->coverage);
      }
    }
    bake_collect_live_source_images(ma, layer, r_images);
    /* What a Material row renders is its source alone: its mask, effects and children stay live
     * over the maps, so a stroke on them is no edit of what the render reads. */
    if (scope != PaintLayerBakeScope::Child) {
      return;
    }
  }
  paint_layer_dependencies(layer, [&](const PaintLayerDependency &dep) {
    /* A channel outside the material's set is not rendered, so its map is not a dependency. */
    if (dep.kind != PaintLayerDependencyKind::ImageMap || *dep.slot == nullptr ||
        paint_layer_channel_filtered(ma, dep.channel))
    {
      return;
    }
    Image *image = reinterpret_cast<Image *>(*dep.slot);
    if (!r_images.contains(image)) {
      r_images.append(image);
    }
  });
  for (const MaterialPaintLayer &effect : layer.effects) {
    BKE_paint_layers_bake_collect_source_images(ma, effect, r_images, PaintLayerBakeScope::Child);
  }
  if (scope != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
      BKE_paint_layers_bake_collect_source_images(
          ma, mask_item, r_images, PaintLayerBakeScope::Child);
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    BKE_paint_layers_bake_collect_source_images(ma, child, r_images, PaintLayerBakeScope::Child);
  }
}

/**
 * Drain the first answer of a freshly created partial-update user.
 *
 * A new user starts at changeset -1 and its first #BKE_image_partial_update_collect_changes answers
 * `FullUpdateNeeded` because the changeset can no longer be constructed. Read as a change, that made
 * every row invalid right after its own bake subscribed it, and the planner re-baked in a loop.
 * Collecting once here moves the user's baseline to the image's current changeset, so the next
 * collect reports only edits made after this subscribe.
 */
static void bake_subscription_user_drain(Image &image, PartialUpdateUser *user)
{
  const auto result = bke::image::partial_update::BKE_image_partial_update_collect_changes(&image,
                                                                                           user);
  if (result !=
      bke::image::partial_update::ePartialUpdateCollectResult::PartialChangesDetected)
  {
    return;
  }
  bke::image::partial_update::PartialUpdateRegion region;
  while (bke::image::partial_update::BKE_image_partial_update_get_next_change(user, &region) ==
         bke::image::partial_update::ePartialUpdateIterResult::ChangeAvailable)
  {
  }
}

void BKE_paint_layers_bake_subscribe(Material &ma, MaterialPaintLayer &layer)
{
  /* The users are created outside the lock: they only touch the image partial-update system, never
   * this map. */
  BakeLayerSubscription entry;
  entry.marker = layer.marker;
  Vector<Image *> images;
  /* A row whose bake holds its content alone reads none of its own mask: the mask is live over the
   * map, so a stroke into it leaves the cache current and the view unblinking. */
  BKE_paint_layers_bake_collect_source_images(
      ma, layer, images, paint_layer_bake_scope(layer, false));
  for (Image *image : images) {
    PartialUpdateUser *user = BKE_image_partial_update_create(image);
    bake_subscription_user_drain(*image, user);
    entry.sources.append({image, user});
  }
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                                BakeMaterialRuntime{});
    runtime.owner = &ma;
    for (int i = 0; i < runtime.layers.size();) {
      if (BLI_uuid_equal(runtime.layers[i].marker, layer.marker)) {
        for (BakeSubscriptionSource &source : runtime.layers[i].sources) {
          BKE_image_partial_update_free(source.user);
        }
        runtime.layers.remove(i);
      }
      else {
        i++;
      }
    }
    runtime.layers.append(std::move(entry));
  }
  /* Writing a bake changes whether the row is substituted, which is topology: rebuild. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
}

void BKE_paint_layers_bake_subscriptions_validate(Main &bmain)
{
  /* Compared by address only, a freed image is never dereferenced. */
  Set<const Image *> images;
  for (const Image &image : bmain.images) {
    images.add(&image);
  }
  std::lock_guard lock(bake_runtime_mutex());
  for (BakeMaterialRuntime &runtime : bake_runtime().values()) {
    for (BakeLayerSubscription &entry : runtime.layers) {
      for (int i = 0; i < entry.sources.size();) {
        if (images.contains(entry.sources[i].image)) {
          i++;
          continue;
        }
        BKE_image_partial_update_free(entry.sources[i].user);
        entry.sources.remove_and_reorder(i);
        entry.changed = true;
        entry.has_region = false;
      }
    }
  }
}

/* The subscription record of \a marker's bake, copied out under the runtime lock; false when the
 * material has no runtime or the row holds none. A copy, because the entry does not outlive the
 * lock: #BakeMaterialRuntime::layers is rewritten by #BKE_paint_layers_bake_subscribe. */
static bool bake_subscription_find(const Material &ma,
                                   const bUUID &marker,
                                   BakeLayerSubscription &r_entry)
{
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr) {
    return false;
  }
  for (const BakeLayerSubscription &entry : runtime->layers) {
    if (BLI_uuid_equal(entry.marker, marker)) {
      r_entry = entry;
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_bake_changed_region(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int r_region[4])
{
  BakeLayerSubscription entry;
  if (!bake_subscription_find(ma, layer.marker, entry) || !entry.changed || !entry.has_region) {
    return false;
  }
  r_region[0] = entry.xmin;
  r_region[1] = entry.xmax;
  r_region[2] = entry.ymin;
  r_region[3] = entry.ymax;
  return true;
}

/* Whether the row's bake subscription reports pending pixel edits. Unlike
 * #BKE_paint_layers_bake_changed_region, a full-update mark counts too: it sets
 * #BakeLayerSubscription::changed with no rectangle (#BKE_paint_layers_bake_notice_changes). */
bool bake_row_subscription_changed(const Material &ma, const MaterialPaintLayer &layer)
{
  BakeLayerSubscription entry;
  return bake_subscription_find(ma, layer.marker, entry) && entry.changed;
}

/* Whether a row that carries a CPU-computed effect waits for the running stroke to end before its
 * re-bake. The stroke keeps it live -- fresh color, without the effect -- instead of recomputing
 * the whole-canvas effect at every dab, and the stale bake is not substituted, because the pixels
 * changed under it. Only a pixel edit waits: a structural change (the seed, the effect's
 * parameters) and the first bake are what the user asks to see recomputed. The hash, the expensive
 * check, comes last, so outside a stroke it is not computed at all. */
static bool bake_content_row_deferred_by_stroke(const Material &ma, const MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_bake_stroke_active_get()) {
    return false;
  }
  /* The first bake has nothing to defer to. */
  const MaterialPaintLayerBake *bake = layer.bake;
  if (bake == nullptr || bake->coverage == nullptr) {
    return false;
  }
  if (!bake_row_subscription_changed(ma, layer)) {
    return false;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  return hash[0] == bake->hash[0] && hash[1] == bake->hash[1];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Substitution
 * \{ */

bool paint_layer_anchor_point_reader_baked(const Material &ma, const MaterialPaintLayer &reader)
{
  if (!paint_layer_kind_reads_anchor_point(reader)) {
    return false;
  }
  /* Asked per reader on every describe and bake plan: the common Never case answers before any
   * of the walks below. */
  const int mode = BKE_paint_layers_bake_mode_get(reader);
  if (mode == MA_PAINT_LAYER_BAKE_NEVER) {
    return false;
  }
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None) {
    return false;
  }
  /* A slice that depends on a tiled mask map is bake-able: the bake renders it per UDIM tile, the
   * way the row bakes are tiled. */
  const bool has_object_mask = paint_layer_anchor_point_slice_has_object_mask(ma, reader);
  if (mode == MA_PAINT_LAYER_BAKE_ALWAYS) {
    /* Always asks for a cache, but an object mask cannot be represented in one. */
    return !has_object_mask;
  }
  /* Auto: live while the chain is light. An object mask in the slice keeps it live, since a bake
   * would show it everywhere. Over the depth or element limit it moves to the baked slice. */
  if (has_object_mask) {
    return false;
  }
  int points = 0;
  int readers = 0;
  paint_layer_anchor_point_counts(ma, points, readers);
  if (points > PAINT_LAYER_ANCHOR_POINT_LIMIT || readers > PAINT_LAYER_ANCHOR_POINT_LIMIT) {
    return true;
  }
  /* The chain depth counts the reader itself; 3 is the last depth that stays live. */
  return paint_layer_anchor_point_chain_depth(ma, reader) > PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX;
}

bool paint_layer_row_substituted(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A Material row's maps are its source, not a cache of its subtree: it is laid like a painted row
   * (see #paint_layer_row_material_map), with its corrections and mask live over them. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    return false;
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* A reader shows the maps a bake of its slice produced; substituting it would replace the
     * description that decides what it reads. */
    return false;
  }
  /* Object/Generated generators read per-object geometry. Their maps are one UDIM image with a tile
   * per object of the current scene/view layer: only a complete tile set may stand in, and a folder
   * containing such a row needs the same coverage. */
  if (!BKE_paint_layers_generator_subtree_is_shared_bakeable(layer) &&
      !bake_tiles_ready(ma, layer))
  {
    return false;
  }
  if (layer.bake == nullptr || layer.bake->coverage == nullptr ||
      layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER)
  {
    return false;
  }
  /* Only a Layer row is baked, never a correction or a mask item, and never a filter row: it works
   * on what is below it, which no cache of the row alone can hold. */
  if (layer.role != MA_PAINT_LAYER_ROLE_LAYER || paint_layer_row_is_adjustment(layer)) {
    return false;
  }
  /* A Pass Through folder has no row of its own to hold a bake; its rows are laid as if it were
   * not there. */
  if (BKE_paint_layers_is_folder(layer) && BKE_paint_layers_folder_is_pass_through(layer)) {
    return false;
  }
  if (BKE_paint_layers_bake_blocked(ma, layer)) {
    return false;
  }
  return BKE_paint_layers_bake_is_valid(ma, layer);
}

bool paint_layer_row_baked_channel(const Material &ma,
                                   const MaterialPaintLayer &layer,
                                   const int channel,
                                   Image **r_color)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || layer.bake == nullptr ||
      layer.bake->images[channel] == nullptr || paint_layer_channel_filtered(ma, channel) ||
      !paint_layer_row_substituted(ma, layer))
  {
    return false;
  }
  if (r_color != nullptr) {
    *r_color = layer.bake->images[channel];
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Planner
 * \{ */

/** The generated nodes \a layer's subtree would add, the AUTO threshold's unit. */
static int bake_subtree_weight(const Material &ma, const MaterialPaintLayer &layer)
{
  /* Only channels that take part cost generator nodes; a channel outside the set is not built. */
  int channels = 0;
  for (int i = 0; i < layer.channels_num; i++) {
    if (!paint_layer_channel_filtered(ma, layer.channels[i].channel)) {
      channels++;
    }
  }
  int weight = 4 + channels * 6;
  for (const MaterialPaintLayer &effect : layer.effects) {
    weight += 4 + bake_subtree_weight(ma, effect) / 2;
  }
  /* A content bake leaves the mask live over the substituted map, so the bake pays nothing for it. */
  if (!paint_layer_row_bakes_content(layer)) {
    for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
      weight += 4 + bake_subtree_weight(ma, mask_item) / 2;
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    /* A valid bake stands in for the child's whole subtree in the generated graph, so the parent
     * pays for one map instead of everything below it. */
    const bool substituted = paint_layer_row_substituted(ma, child);
    weight += 8 + (substituted ? PAINT_LAYERS_BAKED_CHILD_WEIGHT : bake_subtree_weight(ma, child));
  }
  return weight;
}

/**
 * The level of \a target inside \a list, whose rows are enclosed by \a enclosing folders; -1 when
 * \a target is not in it. A folder's own level counts itself, so a folder in the stack root is 1.
 */
static int bake_level_in_list(const ListBaseT<MaterialPaintLayer> &list,
                              const MaterialPaintLayer *target,
                              const int enclosing)
{
  for (const MaterialPaintLayer &layer : list) {
    const bool folder = BKE_paint_layers_is_folder(layer);
    const int own = enclosing + (folder ? 1 : 0);
    if (&layer == target) {
      return own;
    }
    if (folder) {
      const int found = bake_level_in_list(layer.children, target, own);
      if (found >= 0) {
        return found;
      }
    }
  }
  return -1;
}

int BKE_paint_layers_folder_level(const Material &ma, const MaterialPaintLayer &layer)
{
  return bake_level_in_list(ma.paint_layers, &layer, 0);
}

bool BKE_paint_layers_folder_auto_bake_allowed(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A folder in the stack root is what the user keeps working in; only folders nested in another
   * folder are cold enough to be worth a cache. */
  return BKE_paint_layers_folder_level(ma, layer) >= 2;
}

/* -------------------------------------------------------------------- */
/** \name Objects of a bake
 *
 * An Object or Generated generator reads the geometry of the object it is shaded on, so a cache of
 * such a row describes one object. The objects come from the evaluated depsgraph of the scene and
 * view layer the bake was started for; nothing here looks at the rest of `bmain`, and every
 * function runs on the main thread, where evaluated data may be read.
 * \{ */

/**
 * How many objects of one material an Object/Generated row serves. One UDIM row of tiles holds
 * them (1001..1008), so the tile map such an image keeps stays compact.
 */
constexpr int PAINT_LAYERS_BAKE_OBJECTS_MAX = 8;

/**
 * The cache budget of one material's Object/Generated rows, in bytes. A 4K map is the standard
 * canvas: one object's Base Color and coverage alone are 128 MiB at that side, so the budget has to
 * hold several of them.
 */
constexpr int64_t PAINT_LAYERS_BAKE_OBJECTS_BUDGET = int64_t(2048) * 1024 * 1024;

/**
 * The largest side an Object/Generated row may be baked at; larger is refused, never reduced. It is
 * the largest surface #BKE_paint_layers_gradient_surface_build rasterizes.
 */
constexpr int PAINT_LAYERS_BAKE_OBJECTS_SIZE_MAX = 4096;

bool paint_layers_bake_subtree_needs_surface(const MaterialPaintLayer &layer)
{
  /* A reader's cache is the slice it points at, rendered by the slice machinery, not a bake of
   * this row's own subtree. */
  if (paint_layer_kind_reads_anchor_point(layer)) {
    return false;
  }
  /* A row reading a material is laid through the maps rendered from that source; its own graph is
   * never composited into a bake. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    return false;
  }
  /* A subtree whose generators all read the texel's own UV is one map for every object of the
   * material; one holding an Object or a Generated pattern reads geometry a shared map cannot
   * carry. */
  return !BKE_paint_layers_generator_subtree_is_shared_bakeable(layer);
}

bool paint_layers_bake_node_reads_surface(const Material &ma, const MaterialPaintLayer &node)
{
  if (paint_layer_mask_item_bakes_effect(node)) {
    /* The grey is computed from the mask built up to the item. A projected item before it reads
     * the object's surface, which one map for every object cannot carry. An item after the
     * effect is not part of its input, so it does not count. */
    const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, node);
    if (owner == nullptr) {
      return false;
    }
    if (paint_layer_kind_reads_anchor_point(node)) {
      /* The grey is computed from the slice of another row, not from the mask before the item: it
       * reads the surface when that row projects (a Projection Box Fill), and the items before it
       * play no part. */
      const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, node);
      return ref.source_row != nullptr && BKE_paint_layers_projection_applies(ma, *ref.source_row);
    }
    for (const MaterialPaintLayer &item : owner->mask_stack) {
      if (&item == &node) {
        break;
      }
      if (BKE_paint_layers_projection_applies(ma, item)) {
        return true;
      }
    }
    return false;
  }
  if (paint_layer_is_owned_effect(ma, node)) {
    const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, node);
    return row != nullptr && BKE_paint_layers_projection_applies(ma, *row);
  }
  return paint_layers_bake_subtree_needs_surface(node);
}

/**
 * Whether the bake of \a layer is one map for every object. Same as
 * #BKE_paint_layers_generator_subtree_is_shared_bakeable, which sees only the node's own subtree
 * and so cannot know a mask effect item reads the surface through the items before it.
 */
bool bake_node_is_shared(const Material &ma, const MaterialPaintLayer &layer)
{
  if (paint_layer_mask_item_bakes_effect(layer) && paint_layers_bake_node_reads_surface(ma, layer)) {
    return false;
  }
  return BKE_paint_layers_generator_subtree_is_shared_bakeable(layer);
}

/**
 * Whether \a object has \a ma in one of its slots, linked to the object or to its data. The
 * evaluated getter is used because \a object comes from the depsgraph: its mesh may be an edit-mode
 * copy whose material array the original does not have.
 *
 * The slots of an evaluated object name the evaluated (copy-on-eval) materials, while \a ma is the
 * original of the database, so the slot is compared through its original.
 */
static bool bake_object_uses_material(const Object &object, const Material &ma)
{
  Object *object_mut = const_cast<Object *>(&object);
  for (int slot = 0; slot < object.totcol; slot++) {
    const Material *slot_material = BKE_object_material_get_eval(object_mut, short(slot + 1));
    if (slot_material == nullptr) {
      continue;
    }
    const Material *original = DEG_get_original(slot_material);
    if (slot_material == &ma || original == &ma) {
      return true;
    }
  }
  return false;
}

/**
 * Whether \a object's evaluated mesh is driven by time: an animated armature or shape keys, a
 * modifier that declares a time dependency (Wave, Ocean, Cloth, Softbody, Dynamic Paint, Geometry
 * Nodes...), or a particle system.
 *
 * Such a row's cache would be stale the frame it landed and would be re-baked every frame, so the
 * row stays live instead. A posed but still armature is an ordinary edit the surface identity sees.
 */
static bool bake_object_deforms_over_time(const Object &object, const Mesh &mesh, Scene *scene)
{
  /* The evaluated mesh carries no AnimData of its own: the animation that deforms it lives on the
   * original datablock. Drivers and NLA tracks are part of #BKE_animdata_id_is_animated. */
  const Mesh *mesh_original = DEG_get_original(&mesh);
  if (BKE_animdata_id_is_animated(&(mesh_original != nullptr ? *mesh_original : mesh).id)) {
    return true;
  }
  const Object *original = DEG_get_original(&object);
  if (const Key *key = BKE_key_from_object(
          const_cast<Object *>(original != nullptr ? original : &object)))
  {
    if (BKE_animdata_id_is_animated(&key->id)) {
      return true;
    }
  }
  for (const ModifierData &md : object.modifiers) {
    if (BKE_modifier_depends_ontime(scene, const_cast<ModifierData *>(&md))) {
      return true;
    }
    if (md.type != eModifierType_Armature) {
      continue;
    }
    const ArmatureModifierData &armature = reinterpret_cast<const ArmatureModifierData &>(md);
    if (armature.object != nullptr &&
        (BKE_animdata_id_is_animated(&armature.object->id) ||
         (armature.object->data != nullptr &&
          BKE_animdata_id_is_animated(armature.object->data))))
    {
      return true;
    }
  }
  /* Particles are not a modifier on the object's modifier stack in the legacy system. */
  if (!BLI_listbase_is_empty(&object.particlesystem)) {
    return true;
  }
  return false;
}

/**
 * The bytes one object's maps cost at \a size: a plane per channel the row lays in, in the format
 * that channel is baked at, and the shared coverage plane.
 */
static int64_t bake_object_cache_bytes(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const int size)
{
  if (size <= 0) {
    return 0;
  }
  const int64_t texels = int64_t(size) * size;
  int64_t bytes = 0;
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* A mask effect's grey is one Base Color map and no coverage (#bake_mask_effect_row_write). */
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (info.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        return texels * (bake_channel_needs_float(layer, info) ? 16 : 4);
      }
    }
    return 0;
  }
  if (layer.channels != nullptr && layer.channels_num > 0) {
    /* Only the channels the row lays get a map (the same set the precreation walks); counting the
     * material's whole channel set would charge a Base Color Gradient for Height and Metallic maps
     * it never writes, and refuse a bake that costs a fraction of that. */
    for (const int index : IndexRange(layer.channels_num)) {
      const int channel = int(layer.channels[index].channel);
      if (paint_layer_channel_filtered(ma, channel)) {
        continue;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      bytes += texels * (bake_channel_needs_float(layer, info) ? 16 : 4);
    }
  }
  else {
    /* A row without channel records (a folder) is charged for every channel it may fold in. */
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (paint_layer_channel_filtered(ma, int(info.channel))) {
        continue;
      }
      bytes += texels * (bake_channel_needs_float(layer, info) ? 16 : 4);
    }
  }
  /* The coverage map is one byte RGBA plane whatever the channels are. */
  return bytes + texels * 4;
}

/** Whether \a layer's bake record owns any map; a record with none only keeps the mode and size. */
static bool bake_row_has_maps(const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return false;
  }
  if (layer.bake->coverage != nullptr) {
    return true;
  }
  for (const Image *image : layer.bake->images) {
    if (image != nullptr) {
      return true;
    }
  }
  return false;
}

/**
 * The bytes the Object/Generated caches of \a ma would take with \a layer's row at \a size for
 * \a objects objects. The row itself is counted at the passed size and count; its current maps, if
 * any, are replaced rather than added.
 */
static int64_t bake_object_budget_bytes(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const int size,
                                        const int64_t objects)
{
  int64_t bytes = objects * bake_object_cache_bytes(ma, layer, size);
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        if (&row == &layer || !bake_row_has_maps(row) || row.bake->size <= 0 ||
            !paint_layers_bake_subtree_needs_surface(row))
        {
          return true;
        }
        /* A row baked with tile blocks holds a tile per block tile, not per object: the coverage map
         * is tiled exactly like the colour maps, so it tells how many. */
        int row_tiles = std::max(1, row.bake->object_count);
        if (row.bake->coverage != nullptr && row.bake->coverage->source == IMA_SRC_TILED) {
          row_tiles = std::max(row_tiles, BLI_listbase_count(&row.bake->coverage->tiles));
        }
        bytes += int64_t(row_tiles) * bake_object_cache_bytes(ma, row, row.bake->size);
        return true;
      });
  return bytes;
}

uint64_t paint_layers_bake_target_identity(const PaintLayersBakeTarget &target, const int size)
{
  return BKE_paint_layers_gradient_surface_identity(
      *target.object, *target.mesh, target.uv_map_name, size);
}

/**
 * Whether \a node or anything a bake of it renders (its corrections, its mask items, its children)
 * applies a projection: the Material-aware half of #paint_layer_subtree_holds_frame, over the same
 * nodes, so the two cannot disagree about what a bake reads from the surface.
 */
static bool bake_subtree_projects(const Material &ma,
                                  const MaterialPaintLayer &node,
                                  const bool is_child)
{
  if (paint_layer_node_holds_frame(node) && BKE_paint_layers_projection_applies(ma, node)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : node.effects) {
    if (bake_subtree_projects(ma, effect, true)) {
      return true;
    }
  }
  if (paint_layer_bake_scope(node, is_child) != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &item : node.mask_stack) {
      if (bake_subtree_projects(ma, item, true)) {
        return true;
      }
    }
  }
  for (const MaterialPaintLayer &child : node.children) {
    if (bake_subtree_projects(ma, child, true)) {
      return true;
    }
  }
  return false;
}

/** Whether \a layer or a row below it folds a projection into its bake. */
bool bake_subtree_folds_projection(const Material &ma, const MaterialPaintLayer &layer)
{
  if (paint_layer_row_folds_into_bake(ma, layer)) {
    return true;
  }
  /* A CPU effect anywhere in the subtree makes every projection under it part of the bake: the
   * effect runs over what the surface projects, whatever node holds the frame. */
  if (paint_layer_subtree_has_cpu_effect(layer) && bake_subtree_projects(ma, layer, false)) {
    return true;
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (bake_subtree_folds_projection(ma, child)) {
      return true;
    }
  }
  return false;
}

/**
 * The controller Empties of every projecting node a bake of \a node renders, over the same nodes as
 * #bake_subtree_projects. They are what the bake's identity has to follow: a controller of a child
 * or of a mask item inside the picture moves the pixels of the whole row.
 */
static void bake_subtree_controllers_collect(const Material &ma,
                                             const MaterialPaintLayer &node,
                                             const bool is_child,
                                             Vector<const Object *> &r_controllers)
{
  if (node.projection_object != nullptr && paint_layer_node_holds_frame(node) &&
      BKE_paint_layers_projection_applies(ma, node) &&
      !r_controllers.contains(node.projection_object))
  {
    r_controllers.append(node.projection_object);
  }
  for (const MaterialPaintLayer &effect : node.effects) {
    bake_subtree_controllers_collect(ma, effect, true, r_controllers);
  }
  if (paint_layer_bake_scope(node, is_child) != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &item : node.mask_stack) {
      bake_subtree_controllers_collect(ma, item, true, r_controllers);
    }
  }
  for (const MaterialPaintLayer &child : node.children) {
    bake_subtree_controllers_collect(ma, child, true, r_controllers);
  }
}

void bake_projection_controllers_collect(const Material &ma,
                                          Vector<const Object *> &r_controllers)
{
  const auto add = [&](const MaterialPaintLayer &row) {
    if (row.projection_object != nullptr && !r_controllers.contains(row.projection_object)) {
      r_controllers.append(row.projection_object);
    }
  };
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        add(layer);
        for (const MaterialPaintLayer &effect : layer.effects) {
          add(effect);
        }
        for (const MaterialPaintLayer &item : layer.mask_stack) {
          add(item);
        }
        return true;
      });
}

uint64_t paint_layers_bake_row_identity(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const PaintLayersBakeTarget &target,
                                        const int size)
{
  uint64_t identity = paint_layers_bake_target_identity(target, size);
  /* A block of several tiles moves what the maps hold (which UV tiles, and where they lie), which
   * the geometry hash does not see when only the material's rows changed. A one-tile block adds
   * nothing: the tile hash is saved in the file, and those bakes keep the value they always had. */
  if (target.block_size[0] * target.block_size[1] > 1) {
    identity = get_default_hash(identity,
                                target.uv_tile_min[0],
                                target.uv_tile_min[1],
                                target.block_size[0],
                                target.block_size[1]);
  }
  /* The tile hash is saved in the file, so a row that folds no projection keeps the value it always
   * had: adding the slot signature to every tile would re-bake every saved bake once. */
  const bool mask_effect_reads_surface = paint_layer_mask_item_bakes_effect(layer) &&
                                         paint_layers_bake_node_reads_surface(ma, layer);
  if (!mask_effect_reads_surface && !bake_subtree_folds_projection(ma, layer)) {
    return identity;
  }
  /* A projected read covers the faces of the slots holding the material, which no mesh hash sees. */
  identity = get_default_hash(identity, composite_projection_surface_identity(target.object, &ma));
  /* The frame of a row with a controller Empty moves with the Empty (and with the painted object
   * seen from it), which the frame's own numbers do not say. Every projecting node of the subtree
   * adds its controller, so a nested row's Empty invalidates the bake like the row's own does. Only
   * a subtree with a controller adds anything, so the identity of the others stays as it was. */
  Vector<const Object *> controllers;
  if (mask_effect_reads_surface) {
    /* The grey reads the items before the effect, not the effect's own subtree. */
    if (const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, layer)) {
      for (const MaterialPaintLayer &item : owner->mask_stack) {
        if (&item == &layer) {
          break;
        }
        bake_subtree_controllers_collect(ma, item, true, controllers);
      }
    }
  }
  else {
    bake_subtree_controllers_collect(ma, layer, false, controllers);
  }
  if (!controllers.is_empty()) {
    const auto mix_matrix = [&](const float4x4 &matrix) {
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          identity = bake_hash_float(identity, matrix[i][j]);
        }
      }
    };
    mix_matrix(target.object->object_to_world());
    for (const Object *controller : controllers) {
      identity = get_default_hash(identity, controller->id.session_uid);
      mix_matrix(controller->object_to_world());
    }
  }
  return identity;
}

/**
 * Collect the tile-index keys of every layered material in \a bmain once per view pass. The lazy
 * prune needs this set for every mesh object, and rebuilding the paths per object would make the
 * pass quadratic in the material count.
 */
static void bake_tile_keys_collect(Main &bmain, Set<uint64_t> &r_keys)
{
  for (const Material &ma : bmain.materials) {
    if (!BKE_paint_layers_is_layered(ma)) {
      continue;
    }
    const std::string path = paint_layers_bake_tile_attribute_path(ma);
    uint64_t key;
    if (bake_tile_property_hash(path.c_str(), key)) {
      r_keys.add(key);
    }
  }
}

/**
 * Remove the tile-index properties of \a object whose material no longer exists. The property is
 * keyed only by a material UUID, so after a material is deleted nothing can map the key back to it;
 * the planner's view pass is the one place that already visits every mesh object of the view layer.
 * Reads the original object: the evaluated copy carries a copied property group, not the one a file
 * keeps.
 */
static void bake_tile_properties_prune(const Object &object, const Set<uint64_t> &keys)
{
  const Object *original = DEG_get_original(&object);
  Object *object_mut = const_cast<Object *>(original != nullptr ? original : &object);
  IDProperty *properties = IDP_GetProperties(&object_mut->id);
  if (properties == nullptr || properties->type != IDP_GROUP) {
    return;
  }
  IDProperty *property = static_cast<IDProperty *>(properties->data.group.first);
  while (property != nullptr) {
    IDProperty *next = property->next;
    uint64_t key;
    if (bake_tile_property_hash(property->name, key) && !keys.contains(key)) {
      /* One property per step: the walk holds the next one, which may be this one's row companion,
       * so removing the pair at once would free it. */
      if (bake_tile_property_remove(*object_mut, property->name)) {
        DEG_id_tag_update(&object_mut->id, ID_RECALC_SHADING);
      }
    }
    property = next;
  }
}

/** The columns of the UDIM grid, and the tiles it holds in all: the limits of one bake's blocks. */
constexpr int BAKE_TILE_BLOCK_COLUMNS = 10;
constexpr int BAKE_TILE_BLOCK_TILES_MAX = 100;

/**
 * Whether any row of \a ma folds a projection into its bake, or a mask effect item bakes a grey
 * read through the surface: both read the object's UVs by tile, so both want the blocks.
 */
static bool bake_any_row_folds_projection(const Material &ma)
{
  bool folds = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) -> bool {
        folds = bake_subtree_folds_projection(ma, row);
        for (const MaterialPaintLayer &item : row.mask_stack) {
          folds = folds ||
                  (paint_layer_mask_item_bakes_effect(item) &&
                   paint_layers_bake_node_reads_surface(ma, item));
        }
        return !folds;
      });
  return folds;
}

/**
 * Place the objects of \a r_view in the UDIM maps their bakes share.
 *
 * Every object first gets the single-tile scheme: tile `1001 + index`, which is all a bake had
 * before blocks and stays what a one-tile object gets below, so the maps and records of a saved
 * bake read the same. A material holding a row that folds a projection then gives each object a
 * block instead: the bounding box of the UDIM tiles its UVs have geometry in, blocks laid left to
 * right in rows of ten tiles. The shift into a block is one constant per object (the shader adds it
 * to the UV), so a block is a rigid copy of that box, tiles without geometry included.
 *
 * The scheme is the material's, not a row's: the shift lives on the object and every row of the
 * material reads it. A bake that cannot be placed (a box wider than the ten columns, more than 100
 * tiles in all, a UV outside the UDIM grid) sets #PaintLayersBakeView::tiles_limited; the rows that
 * fold a projection then stay live and the others keep the single-tile scheme.
 */
static void bake_view_tile_blocks_assign(const Material &ma, PaintLayersBakeView &r_view)
{
  r_view.tiles_limited = false;
  for (const int index : r_view.objects.index_range()) {
    PaintLayersBakeTarget &target = r_view.objects[index];
    target.uv_tiles.clear();
    target.uv_tile_min[0] = target.uv_tile_min[1] = 0;
    target.block_size[0] = target.block_size[1] = 1;
    target.block_origin[0] = index % BAKE_TILE_BLOCK_COLUMNS;
    target.block_origin[1] = index / BAKE_TILE_BLOCK_COLUMNS;
  }
  if (r_view.objects.is_empty() || !bake_any_row_folds_projection(ma)) {
    return;
  }

  struct Block {
    Vector<int> uv_tiles;
    int uv_min[2];
    int size[2];
    int origin[2];
  };
  Vector<Block> blocks;
  for (const PaintLayersBakeTarget &target : r_view.objects) {
    bool outside_udim = false;
    Block block;
    block.uv_tiles = BKE_paint_layers_gradient_surface_uv_tiles(
        *target.mesh, target.uv_map_name, outside_udim);
    if (outside_udim) {
      r_view.tiles_limited = true;
      return;
    }
    int uv_max[2] = {0, 0};
    block.uv_min[0] = block.uv_min[1] = BAKE_TILE_BLOCK_COLUMNS;
    for (const int tile : block.uv_tiles) {
      const int column = (tile - PAINT_TILE_NUMBER_BASE) % BAKE_TILE_BLOCK_COLUMNS;
      const int row = (tile - PAINT_TILE_NUMBER_BASE) / BAKE_TILE_BLOCK_COLUMNS;
      block.uv_min[0] = std::min(block.uv_min[0], column);
      block.uv_min[1] = std::min(block.uv_min[1], row);
      uv_max[0] = std::max(uv_max[0], column);
      uv_max[1] = std::max(uv_max[1], row);
    }
    block.size[0] = uv_max[0] - block.uv_min[0] + 1;
    block.size[1] = uv_max[1] - block.uv_min[1] + 1;
    blocks.append(std::move(block));
  }

  /* Shelf packing: a block that does not fit the rest of the row starts the next one. One-tile
   * blocks in a row come out as `(index % 10, index / 10)`, the single-tile scheme. */
  int column = 0;
  int row = 0;
  int row_height = 0;
  int tiles_total = 0;
  for (Block &block : blocks) {
    if (column + block.size[0] > BAKE_TILE_BLOCK_COLUMNS) {
      row += row_height;
      column = 0;
      row_height = 0;
    }
    block.origin[0] = column;
    block.origin[1] = row;
    column += block.size[0];
    row_height = std::max(row_height, block.size[1]);
    tiles_total += block.size[0] * block.size[1];
    if (block.size[0] > BAKE_TILE_BLOCK_COLUMNS || tiles_total > BAKE_TILE_BLOCK_TILES_MAX ||
        row + block.size[1] > BAKE_TILE_BLOCK_COLUMNS)
    {
      r_view.tiles_limited = true;
      return;
    }
  }
  for (const int index : r_view.objects.index_range()) {
    PaintLayersBakeTarget &target = r_view.objects[index];
    Block &block = blocks[index];
    target.uv_tiles = std::move(block.uv_tiles);
    for (const int axis : IndexRange(2)) {
      target.uv_tile_min[axis] = block.uv_min[axis];
      target.block_size[axis] = block.size[axis];
      target.block_origin[axis] = block.origin[axis];
    }
  }
}

void paint_layers_bake_view_resolve(const Material &ma,
                                    Main *bmain,
                                    const Scene *scene,
                                    const ViewLayer *view_layer,
                                    PaintLayersBakeView &r_view)
{
  r_view.objects.clear();
  r_view.meshes.clear();
  r_view.issue = PAINT_LAYERS_BAKE_OBJECTS_OK;
  r_view.tiles_limited = false;
  if (scene == nullptr || view_layer == nullptr) {
    r_view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
    return;
  }
  Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer);
  if (depsgraph == nullptr) {
    r_view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
    return;
  }
  /* The keys are collected once for the whole object pass below. */
  Set<uint64_t> tile_keys;
  Main *keys_main = (bmain != nullptr) ? bmain : G_MAIN;
  if (keys_main != nullptr) {
    bake_tile_keys_collect(*keys_main, tile_keys);
  }
  /* The evaluated objects may be edited by a time-dependent modifier while the bake runs: the
   * surface is built once, so refuse the row before it can go stale on the next frame. */
  Scene *scene_mut = const_cast<Scene *>(scene);

  int issue = PAINT_LAYERS_BAKE_OBJECTS_OK;
  DEGObjectIterSettings settings{};
  settings.depsgraph = depsgraph;
  settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY | DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET |
                   DEG_ITER_OBJECT_FLAG_VISIBLE;
  DEG_OBJECT_ITER_BEGIN (&settings, object) {
    if (object->type != OB_MESH) {
      continue;
    }
    r_view.meshes.append(object);
    /* Drop indices of materials that no longer exist before this object's bake is considered. */
    bake_tile_properties_prune(*object, tile_keys);
    if (!bake_object_uses_material(*object, ma)) {
      continue;
    }
    const Mesh *mesh = BKE_object_get_evaluated_mesh(object);
    if (mesh == nullptr) {
      issue = PAINT_LAYERS_BAKE_OBJECTS_NO_MESH;
      break;
    }
    const StringRefNull uv_name = BKE_paint_layers_gradient_uv_map_name(*mesh);
    if (uv_name.is_empty()) {
      issue = PAINT_LAYERS_BAKE_OBJECTS_NO_UV;
      break;
    }
    if (bake_object_deforms_over_time(*object, *mesh, scene_mut)) {
      issue = PAINT_LAYERS_BAKE_OBJECTS_ANIMATED;
      break;
    }
    PaintLayersBakeTarget target;
    target.object = object;
    target.mesh = mesh;
    /* The evaluated copies carry no session uid of their own (it reads zero), so two objects would
     * share one tile; the uid that names an object and its mesh is the original's. */
    const Object *original_object = DEG_get_original(object);
    target.object_uid = original_object->id.session_uid;
    target.mesh_uid = original_object->data != nullptr ?
                          static_cast<const ID *>(original_object->data)->session_uid :
                          0;
    target.uv_map_name = uv_name.c_str();
    r_view.objects.append(target);
    if (r_view.objects.size() > PAINT_LAYERS_BAKE_OBJECTS_MAX) {
      issue = PAINT_LAYERS_BAKE_OBJECTS_TOO_MANY;
      break;
    }
  }
  DEG_OBJECT_ITER_END;

  if (issue != PAINT_LAYERS_BAKE_OBJECTS_OK) {
    r_view.objects.clear();
    r_view.issue = issue;
    return;
  }
  if (r_view.objects.is_empty()) {
    r_view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_OBJECT;
    return;
  }
  bake_view_tile_blocks_assign(ma, r_view);
}

/**
 * Remove the shader's tile index from the objects of \a scene and \a view_layer once no row of
 * \a ma has a tile left. The property is keyed on the material, so keeping it would leave an
 * orphaned Custom Property on objects the material no longer bakes for.
 */
void bake_tile_object_indices_clear(const Material &ma,
                                    const Scene *scene,
                                    const ViewLayer *view_layer,
                                    const PaintLayersBakeView *view)
{
  if (bake_material_has_tiles(ma)) {
    return;
  }
  const std::string path = paint_layers_bake_tile_attribute_path(ma);
  if (view != nullptr && view->issue == PAINT_LAYERS_BAKE_OBJECTS_OK) {
    /* The caller already resolved every mesh object of the view: no second walk of the depsgraph.
     * A mesh that no longer uses the material is included: its index is stale by definition. */
    for (const Object *object : view->meshes) {
      bake_tile_object_index_remove(*object, path.c_str(), true);
    }
    return;
  }
  if (scene == nullptr || view_layer == nullptr) {
    return;
  }
  Depsgraph *depsgraph = BKE_scene_get_depsgraph(scene, view_layer);
  if (depsgraph == nullptr) {
    return;
  }
  DEGObjectIterSettings settings{};
  settings.depsgraph = depsgraph;
  settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY | DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET |
                   DEG_ITER_OBJECT_FLAG_VISIBLE;
  DEG_OBJECT_ITER_BEGIN (&settings, object) {
    if (object->type == OB_MESH) {
      bake_tile_object_index_remove(*object, path.c_str(), true);
    }
  }
  DEG_OBJECT_ITER_END;
}

void paint_layers_bake_targets_resolve(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const PaintLayersBakeView &view,
                                       const int size,
                                       PaintLayersBakeTargets &r_targets)
{
  r_targets.objects.clear();
  r_targets.issue = PAINT_LAYERS_BAKE_OBJECTS_OK;
  if (!paint_layers_bake_node_reads_surface(ma, layer)) {
    /* A UV-only subtree is one map for every object: it needs no surface and no object. */
    return;
  }
  if (view.issue != PAINT_LAYERS_BAKE_OBJECTS_OK) {
    r_targets.issue = view.issue;
    return;
  }
  if (size <= 0) {
    /* Nothing to allocate a map at yet; the planner finds no size either. */
    r_targets.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_SIZE;
    return;
  }
  if (size > PAINT_LAYERS_BAKE_OBJECTS_SIZE_MAX) {
    /* Refused rather than baked smaller: a resampled cache would be a silent loss of quality. */
    r_targets.issue = PAINT_LAYERS_BAKE_OBJECTS_TOO_LARGE;
    return;
  }
  if (view.objects.is_empty()) {
    r_targets.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_OBJECT;
    return;
  }
  /* A mask effect reading the surface places its tiles like a projected row's. */
  const bool folds_projection = bake_subtree_folds_projection(ma, layer) ||
                                paint_layer_mask_item_bakes_effect(layer);
  if (view.tiles_limited && folds_projection) {
    /* The UVs have no placement in the UDIM maps (see #bake_view_tile_blocks_assign): the row has
     * nothing to be baked into and stays live, without its effect. */
    r_targets.issue = PAINT_LAYERS_BAKE_OBJECTS_TILES;
    return;
  }
  r_targets.objects = view.objects;

  /* The budget is one per material, not one per row: every Object/Generated row of the material
   * holds one cache set per object tile, and the sum is what the file's caches can grow to.
   * Existing rows are counted at the size and tile count they were baked at, the row being
   * resolved at the size and tile count it is about to use. */
  int64_t tiles_num = 0;
  for (const PaintLayersBakeTarget &target : view.objects) {
    tiles_num += target.uv_tiles.is_empty() ? 1 : target.block_size[0] * target.block_size[1];
  }
  int64_t pairs_num = 0;
  for (const int index : view.objects.index_range()) {
    pairs_num += std::max<int64_t>(1, view.objects[index].uv_tiles.size());
  }
  /* A background job holds the surface of every render until the worker has used it, which the
   * cache budget does not count. A bake of one tile per object is the case that always had this
   * cost and is left alone; only the extra renders of tile blocks are held to the limit. */
  const int64_t surface_bytes = pairs_num * int64_t(size) * size * 13;
  if (bake_object_budget_bytes(ma, layer, size, tiles_num) > PAINT_LAYERS_BAKE_OBJECTS_BUDGET ||
      (folds_projection && !paint_layer_mask_item_bakes_effect(layer) &&
       pairs_num > int64_t(view.objects.size()) && surface_bytes > PAINT_LAYERS_BAKE_OBJECTS_BUDGET))
  {
    r_targets.objects.clear();
    r_targets.issue = PAINT_LAYERS_BAKE_OBJECTS_OVER_BUDGET;
  }
}

/** Record why \a layer's Object/Generated bake is not running, for the Bake panel to show. */
static void bake_object_issue_record(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     const int issue)
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                              BakeMaterialRuntime{});
  runtime.owner = &ma;
  const UUID marker(layer.marker);
  const int *stored = runtime.object_issue.lookup_ptr(marker);
  if (stored != nullptr && *stored == issue) {
    return;
  }
  runtime.object_issue.add_overwrite(marker, issue);
}

int BKE_paint_layers_bake_object_issue_get(const Material &ma, const MaterialPaintLayer &layer)
{
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr) {
    return PAINT_LAYERS_BAKE_OBJECTS_OK;
  }
  return runtime->object_issue.lookup_default(UUID(layer.marker),
                                              int(PAINT_LAYERS_BAKE_OBJECTS_OK));
}

/** Remember the surface identity \a layer's maps were rendered from. */
void bake_object_identity_record(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 const uint64_t identity)
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                              BakeMaterialRuntime{});
  runtime.owner = &ma;
  runtime.object_identity.add_overwrite(UUID(layer.marker), identity);
}

/** Forget what \a layer's maps were rendered from: a bake was released, the next one is new. */
static void bake_object_identity_forget(const Material &ma, const MaterialPaintLayer &layer)
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr) {
    return;
  }
  runtime->object_identity.remove(UUID(layer.marker));
}

/** \} */

/**
 * Whether \a layer's bake pays per texel rather than per node: a Gradient row itself, or a row a
 * live Gradient is laid over (see #BKE_paint_layers_generator_subtree_holds_laid_generator), whose
 * bake folds that Gradient in. Both a shared (UV) and a per-object (Object/Generated) bake have
 * that cost; only a subtree the bake cannot evaluate at all is left out.
 */
static bool bake_cost_is_texels(const MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_generator_subtree_is_bakeable(layer)) {
    return false;
  }
  return paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES) ||
         BKE_paint_layers_generator_subtree_holds_laid_generator(layer);
}

/**
 * Whether the material lets an automatic bake of \a layer's Gradient stand: the Gradient answer of
 * #Material::paint_layers_gradient_bake speaks only for a row in AUTO -- an Always or Never the
 * user chose on the row itself outranks it -- and "Only Complex Graphs" hands the question to the
 * node-count rules a plain AUTO row answers (#bake_subtree_weight against
 * #PAINT_LAYERS_AUTO_BAKE_NODES), so a light graph stays live and only a heavy one is cached.
 *
 * This stays a Gradient-named path, not a generator-table branch: the setting is the Gradient
 * bake policy the user picked for the material, so the subtype switch below is over the policy's
 * own answers, not over generator kinds. Which rows are generators at all is the kind schema's
 * #PAINT_LAYER_KIND_GENERATES flag, read by the callers.
 */
static bool bake_generator_auto_allowed(const Material &ma, const MaterialPaintLayer &layer)
{
  /* The setting answers only the automatic bake. */
  if (BKE_paint_layers_bake_mode_get(layer) != MA_PAINT_LAYER_BAKE_AUTO) {
    return true;
  }
  switch (eMaterialPaintLayersGradientBake(ma.paint_layers_gradient_bake)) {
    case MA_PAINT_LAYERS_GRADIENT_BAKE_NEVER:
      return false;
    case MA_PAINT_LAYERS_GRADIENT_BAKE_ONLY_COMPLEX:
      return bake_subtree_weight(ma, layer) > PAINT_LAYERS_AUTO_BAKE_NODES;
    case MA_PAINT_LAYERS_GRADIENT_BAKE_ALWAYS:
      return true;
  }
  /* An answer from a later version reads as the zero it was saved without: Always. */
  return true;
}

/** Whether anything in \a layer's subtree is a transform effect acting in its Decal frame. */
static bool bake_subtree_has_decal_effect(const MaterialPaintLayer &layer)
{
  /* A projected row with a CPU effect is baked synchronously per object, like a Decal effect
   * (never true while #PAINT_LAYERS_BAKE_PROJECTED_EFFECT is off); so is one whose CPU effect runs
   * over a projection nested in its subtree. */
  if (paint_layer_subtree_may_fold_into_bake(layer)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (paint_layer_effect_acts_in_decal_frame(effect)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (bake_subtree_has_decal_effect(child)) {
      return true;
    }
  }
  return false;
}

/** Whether anything in \a layer's subtree acts in a Decal frame, which needs the Decal map. */
static bool bake_subtree_has_decal_frame_effect(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (paint_layer_effect_acts_in_decal_frame(effect)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (bake_subtree_has_decal_frame_effect(child)) {
      return true;
    }
  }
  return false;
}

/**
 * Whether a bake of \a node renders a frame in the Decal space (a Text Decal in a row's corrections,
 * a decal-mapped child or mask item). That frame reads through the Decal map, which is built from
 * the evaluated mesh and so can only be had on the main thread.
 */
static bool bake_subtree_projects_in_decal_space(const MaterialPaintLayer &node,
                                                 const bool is_child)
{
  if (paint_layer_node_holds_frame(node) &&
      node.mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL)
  {
    return true;
  }
  for (const MaterialPaintLayer &effect : node.effects) {
    if (bake_subtree_projects_in_decal_space(effect, true)) {
      return true;
    }
  }
  if (paint_layer_bake_scope(node, is_child) != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &item : node.mask_stack) {
      if (bake_subtree_projects_in_decal_space(item, true)) {
        return true;
      }
    }
  }
  for (const MaterialPaintLayer &child : node.children) {
    if (bake_subtree_projects_in_decal_space(child, true)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer)
{
  if (bake_subtree_has_decal_frame_effect(layer)) {
    /* The Decal map and its inverse are cached per object and built from the evaluated mesh, which
     * only the main thread may read: the worker job gets the rasterized surface and nothing else.
     * Such a row is baked by the planner, synchronously, one object at a time. */
    return false;
  }
  if (bake_subtree_has_decal_effect(layer) && bake_subtree_projects_in_decal_space(layer, false)) {
    /* The same holds for a frame in the Decal space under a row that folds projections: the snapshot
     * a worker projects through carries no Decal map. The planner bakes it on the main thread. */
    return false;
  }
  if (bake_subtree_folds_projection(ma, layer)) {
    /* A projected row with a CPU effect reads the object's mesh, which the job takes as a snapshot
     * on the main thread (#PaintMaterialProjectionSnapshot), so a 4K Noise or Blur no longer
     * freezes the UI. The planner keeps the synchronous path for what a job cannot serve (a handle
     * drag at its coarse size, a session without windows). */
    return true;
  }
  if (bake_subtree_has_decal_effect(layer)) {
    /* A row that may fold a projection but does not (its frame does not apply) keeps the
     * synchronous path it always had. */
    return false;
  }
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* The worker job renders a row's subtree from a localized copy; a mask effect's grey is the
     * mask effect cache's compute, which only the main thread reaches. Calling it heavy would leave
     * it to a job that never picks it up, so after its first bake an edit would not bake it again.
     * A grey that reads the surface (#paint_layers_bake_node_reads_surface) stays the planner's
     * too: it reads the evaluated object through the projection, which only the main thread may,
     * and the job has no result type for a mask's grey. #BKE_paint_layers_bake_job_create asks the
     * planner for it, as for a Decal effect. */
    return false;
  }
  if (paint_layer_is_owned_effect(ma, layer)) {
    /* An effect's own result is a whole-canvas compute (a 4K Blur): it leaves the main thread. The
     * worker job computes it from the localized copy and the commit writes the map. */
    return true;
  }
  if (BKE_paint_layers_generator_subtree_is_bakeable(layer) &&
      paint_layers_bake_subtree_needs_surface(layer))
  {
    /* An Object or Generated row is rasterized from the surface of the objects it serves, which is
     * main-thread work of its own before a single texel is composited: such a row always leaves the
     * main thread, whatever its canvas. */
    return true;
  }
  if (bake_subtree_weight(ma, layer) > PAINT_LAYERS_AUTO_BAKE_NODES) {
    return true;
  }
  if (bake_cost_is_texels(layer) && bake_generator_auto_allowed(ma, layer)) {
    /* A generator row is cheap by node count and expensive by texels: its cost is the canvas it is
     * laid on, and the first bake has no stored size to weigh yet. Left on the main thread it would
     * hitch the moment the user leaves the row. A row a generator is laid over pays the same cost,
     * since its bake folds the pattern in. The material's Gradient answer takes part in the
     * question: a row whose generator it keeps live is not heavy for that pattern's sake. */
    const int canvas = int(ma.paint_layers_image_size);
    return canvas <= 0 || canvas >= PAINT_LAYERS_HEAVY_BAKE_SIZE;
  }
  return layer.bake != nullptr && layer.bake->size >= PAINT_LAYERS_HEAVY_BAKE_SIZE;
}

bool BKE_paint_layers_bake_row_is_deferred(const Material &ma, const MaterialPaintLayer &layer)
{
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* A CPU-computed mask effect defers the way a row's content effect does: the stroke keeps the
     * mask live -- fresh, without the effect -- instead of recomputing the whole-canvas grey at
     * every dab. Which marker is active does not decide it: the subscription only reports the maps
     * the grey reads, so a stroke into the row's colour never holds it. */
    return mask_effect_deferred_by_stroke(ma, layer);
  }
  if (paint_layer_is_owned_effect(ma, layer)) {
    /* The same policy for the result an effect keeps on a row that cannot hold it: a stroke into a
     * map the effect reads must not recompute the whole canvas at every dab. */
    return mask_effect_deferred_by_stroke(ma, layer);
  }
  /* The active row and its ancestors stay live: the user is editing inside them, and the bake
   * catches up once the active marker leaves the subtree. */
  if (!BKE_paint_layers_subtree_contains(layer, ma.active_layer_marker)) {
    return false;
  }
  if (paint_layer_row_bakes_content(layer)) {
    /* A CPU-computed effect (a Blur, a Noise) has no live form, but its compute runs over the whole
     * canvas: while a stroke edits the row's pixels, the row is not re-rendered at every dab, it
     * waits for the stroke to end -- see #bake_content_row_deferred_by_stroke. Outside a stroke it
     * is baked under the debounce like an edit of any other value. */
    return bake_content_row_deferred_by_stroke(ma, layer);
  }
  return true;
}

/** Depth-first search for \a target; true when found with it or an ancestor not enabled. */
static bool bake_hidden_find(const ListBaseT<MaterialPaintLayer> &list,
                             const MaterialPaintLayer *target,
                             const bool ancestor_hidden,
                             bool &r_hidden)
{
  for (const MaterialPaintLayer &layer : list) {
    const bool hidden = ancestor_hidden || (layer.flag & MA_PAINT_LAYER_ENABLED) == 0;
    if (&layer == target) {
      r_hidden = hidden;
      return true;
    }
    if (bake_hidden_find(layer.children, target, hidden, r_hidden)) {
      return true;
    }
  }
  return false;
}

bool bake_owned_effect_pending(const Material &ma, const MaterialPaintLayer &effect)
{
  /* The size test keeps an effect whose row has no map yet from holding the stale signal up for a
   * result nobody can compute. */
  return paint_layer_is_owned_effect(ma, effect) &&
         BKE_paint_layers_bake_mode_get(effect) != MA_PAINT_LAYER_BAKE_NEVER &&
         !BKE_paint_layers_bake_is_valid(ma, effect) && !bake_row_is_hidden(ma, effect) &&
         bake_size_for(ma, effect) > 0;
}

/** Whether \a layer, or a folder it sits in, is switched off: a hidden row is never baked. */
bool bake_row_is_hidden(const Material &ma, const MaterialPaintLayer &layer)
{
  bool hidden = false;
  if (bake_hidden_find(ma.paint_layers, &layer, false, hidden)) {
    return hidden;
  }
  /* A content correction is hidden by its own switch or by the row it hangs on. */
  if (const MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(
          const_cast<Material &>(ma), layer))
  {
    bake_hidden_find(ma.paint_layers, owner, false, hidden);
    hidden = hidden || (layer.flag & MA_PAINT_LAYER_ENABLED) == 0;
  }
  else if (const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(
               const_cast<Material &>(ma), layer))
  {
    /* A mask correction is hidden by its own switch or by the row whose mask it shapes. */
    bake_hidden_find(ma.paint_layers, owner, false, hidden);
    hidden = hidden || (layer.flag & MA_PAINT_LAYER_ENABLED) == 0;
  }
  return hidden;
}

/**
 * Release one bake map. A map whose last user was the bake is deleted; one still named by the
 * generated tree keeps that other user and is simply detached, because freeing it there would leave
 * the tree's node with a dangling pointer. The caller tags #MA_PAINT_LAYERS_REGEN, so the rebuild
 * that follows drops the node before anything is evaluated.
 */
static void bake_map_release(Main &bmain, Image *&slot)
{
  Image *image = slot;
  if (image == nullptr) {
    return;
  }
  slot = nullptr;
  /* A synchronous service map is handed to the bake with the creation reference and no extra
   * user (unlike #BKE_paint_layers_bake_set_map); only decrement when the bake holds one. */
  if (image->id.us > 0) {
    id_us_min(&image->id);
  }
  if (image->id.us == 0 && ID_FAKE_USERS(&image->id) == 0) {
    BKE_id_free(&bmain, &image->id);
  }
}

/**
 * Release the maps of \a layer's bake in the channels a render just left out. A channel the row no
 * longer takes part in (switched off, or a record the row no longer lays) is not rendered again, so
 * its map from an earlier bake would otherwise stand in for the row and show what the row stopped
 * laying. The caller tags #MA_PAINT_LAYERS_REGEN, as for #bake_maps_release.
 */
void bake_stale_channel_maps_release(Main &bmain,
                                     MaterialPaintLayer &layer,
                                     const Span<int> written_channels)
{
  if (layer.bake == nullptr) {
    return;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!written_channels.contains(channel)) {
      bake_map_release(bmain, layer.bake->images[channel]);
    }
  }
}

/** Release the maps \a layer's bake holds, as #bake_map_release does for each. */
static void bake_maps_release(Main &bmain, Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return;
  }
  for (Image *&image : layer.bake->images) {
    bake_map_release(bmain, image);
  }
  bake_map_release(bmain, layer.bake->coverage);
  /* The per-object tile records describe those maps, and the surface they were rendered from goes
   * with them: a later bake starts from its own. */
  paint_layers_bake_tiles_clear(*layer.bake);
  paint_layers_bake_tile_indices_clear_all(ma);
  bake_object_identity_forget(ma, layer);
  if (ma.paint_layers_tree != nullptr) {
    DEG_id_tag_update(&ma.paint_layers_tree->id, ID_RECALC_SYNC_TO_EVAL);
  }
}

/**
 * Free \a layer's bake maps and, unless the user chose a mode, the record itself.
 *
 * The record is the only copy of the mode and size the user chose. An AUTO record may go with its
 * maps (the size survives in #BakeMaterialRuntime::dropped_size), but an Always or Never one stays,
 * with no maps and no hash, so releasing a cache never resets the mode.
 */
static void bake_structure_free(Main &bmain, Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return;
  }
  bake_maps_release(bmain, ma, layer);
  if (layer.bake->mode != MA_PAINT_LAYER_BAKE_AUTO) {
    layer.bake->hash[0] = 0;
    layer.bake->hash[1] = 0;
    layer.bake->source_hash[0] = 0;
    layer.bake->source_hash[1] = 0;
    layer.bake->preview_side16 = 0;
    return;
  }
  MEM_SAFE_DELETE(layer.bake);
}

/** Remember \a layer's bake size before its structure is released (see #BakeMaterialRuntime). */
static void bake_dropped_size_keep(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || layer.bake->size <= 0) {
    return;
  }
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                              BakeMaterialRuntime{});
  runtime.owner = &ma;
  runtime.dropped_size.add_overwrite(UUID(layer.marker), layer.bake->size);
}

/**
 * Carry out what #bake_row_is_due decided for a row that is not due.
 * \return true when something was released, so the caller knows the tree needs a rebuild.
 */
bool bake_row_release_apply(Main &bmain,
                            Material &ma,
                            MaterialPaintLayer &layer,
                            const BakeRowRelease release)
{
  switch (release) {
    case BakeRowRelease::FreeStructure:
      bake_dropped_size_keep(ma, layer);
      bake_structure_free(bmain, ma, layer);
      return true;
    case BakeRowRelease::ReleaseMaps:
      bake_maps_release(bmain, ma, layer);
      /* The maps are gone; the record keeping the mode is not. A hash for maps that no longer exist
       * would vouch for them, so it is dropped too. */
      layer.bake->hash[0] = 0;
      layer.bake->hash[1] = 0;
      return true;
    case BakeRowRelease::Keep:
      break;
  }
  return false;
}

/** The bake size an AUTO release remembered for \a layer, or 0 when none was kept. */
static int bake_dropped_size(const Material &ma, const MaterialPaintLayer &layer)
{
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr) {
    return 0;
  }
  return runtime->dropped_size.lookup_default(UUID(layer.marker), 0);
}

/** The side \a row bakes at when it has none stored: the first map it has dimensions for. */
static int bake_size_from_content(const Material &ma, const MaterialPaintLayer &row)
{
  int width = 0;
  int height = 0;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (BKE_paint_layers_row_dimensions(ma, row, int(info.channel), width, height) && width > 0 &&
        height > 0)
    {
      return width;
    }
  }
  return 0;
}

/**
 * The side \a row is baked at: the side the user set (the explicit bit of the bake structure, so it
 * survives a save, a load and an undo), the size it was baked at (or an AUTO release kept), then
 * its content's. A generator row has no map of its own, so unless the user chose a side its cache
 * follows the material canvas.
 */
int bake_size_for(const Material &ma, const MaterialPaintLayer &row)
{
  /* The stored size of a row baked reduced for a drag is the drag's, not the row's. */
  if (const int drag_full = bake_drag_full_size(ma, row); drag_full > 0) {
    return drag_full;
  }
  if (row.bake != nullptr && (row.bake->flag & MA_PAINT_LAYER_BAKE_SIZE_EXPLICIT) != 0 &&
      row.bake->size > 0)
  {
    return row.bake->size;
  }
  int size = 0;
  if (paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_GENERATES)) {
    /* A generator lays its pixels on the material canvas unless the user chose a side. */
    size = int(ma.paint_layers_image_size);
  }
  else if (paint_layer_mask_item_bakes_effect(row)) {
    size = bake_mask_effect_size_for(ma, row);
  }
  else if (paint_layer_is_owned_effect(ma, row)) {
    size = bake_effect_size_for(ma, row);
  }
  else if (row.bake != nullptr) {
    size = row.bake->size;
  }
  if (size <= 0) {
    size = bake_dropped_size(ma, row);
  }
  if (size <= 0) {
    size = bake_size_from_content(ma, row);
  }
  return size;
}

void paint_layers_bake_precreate_texture(Material &ma, MaterialPaintLayer &edited)
{
  /* Creating Image IDs inside an edit path is why this is guarded: only an original of the local
   * Main reaches here, the same restriction the planner's own map creation works under, and the
   * maps are referenced by the row's DNA bake structure, so the undo step of the edit that asked
   * for them keeps them. */
  if (G_MAIN == nullptr || !BKE_id_is_in_global_main(&ma.id)) {
    return;
  }
  /* A correction or a mask item has no cache of its own: its pattern is folded into the bake of
   * the row it lies over, so that row is the one that gets the maps. */
  MaterialPaintLayer *row = &edited;
  if (BKE_paint_layers_owner_list(ma, edited) == nullptr) {
    row = BKE_paint_layers_correction_owner(ma, edited);
    if (row == nullptr) {
      row = BKE_paint_layers_mask_item_owner(ma, edited);
    }
    if (row == nullptr) {
      return;
    }
  }
  MaterialPaintLayer &layer = *row;
  if (BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_NEVER ||
      bake_row_is_hidden(ma, layer))
  {
    /* A Never or hidden row never reads a cache; a record for it would be dead weight. */
    return;
  }
  const bool shared = BKE_paint_layers_generator_subtree_is_shared_bakeable(layer);
  if (!shared && !paint_layers_bake_subtree_needs_surface(layer)) {
    /* A subtree the bake cannot evaluate at all: no map would ever be read. */
    return;
  }
  if (!bake_generator_auto_allowed(ma, layer)) {
    /* The material keeps this generator live: the planner would release maps created now. */
    return;
  }
  const int size = bake_size_for(ma, layer);
  if (size <= 0) {
    return;
  }
  /* A per-object row is one tiled image. Its tile count is not known before a view layer resolves
   * the objects, so only the first tile is created here; the bake adds the rest. The size cap and
   * the material budget are still checked, so a row that can never be baked gets no record. */
  const int object_num = shared ? 0 : 1;
  if (!shared && (size > PAINT_LAYERS_BAKE_OBJECTS_SIZE_MAX ||
                  bake_object_budget_bytes(ma, layer, size, 1) > PAINT_LAYERS_BAKE_OBJECTS_BUDGET))
  {
    return;
  }
  BKE_paint_layers_bake_struct_ensure(layer)->size = size;
  /* Only the channels the row takes part in get a map: the planner writes a map per channel its
   * render covers, and a precreated one outside that set would sit full-size and blank. The
   * participation test is the one the generator and the CPU composite share, so it cannot drift
   * from what the render lays; where it under-asks (a folder whose children carry the channels) a
   * map skipped here is created by the first bake as usual. */
  bool any_channel = false;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!paint_layer_row_participates(ma, layer, int(info.channel))) {
      continue;
    }
    bake_service_image(*G_MAIN,
                       layer,
                       int(info.channel),
                       size,
                       info.is_color,
                       bake_channel_needs_float(layer, info),
                       object_num);
    any_channel = true;
  }
  /* The coverage map travels with the channel maps: the planner writes it once a channel
   * rendered. */
  if (any_channel) {
    bake_service_image(*G_MAIN, layer, -1, size, false, false, object_num);
  }
}

/**
 * Why an Object or Generated row cannot be baked for the objects of \a view, recorded for the Bake
 * panel; #PAINT_LAYERS_BAKE_OBJECTS_OK when it can, or when its subtree reads no object geometry.
 */
int bake_row_object_issue(const Material &ma,
                          const MaterialPaintLayer &layer,
                          const PaintLayersBakeView &view)
{
  if (!paint_layers_bake_node_reads_surface(ma, layer)) {
    /* A row that stopped reading geometry has no Object/Generated reason to show any more. */
    bake_object_issue_record(ma, layer, PAINT_LAYERS_BAKE_OBJECTS_OK);
    return PAINT_LAYERS_BAKE_OBJECTS_OK;
  }
  PaintLayersBakeTargets targets;
  paint_layers_bake_targets_resolve(ma, layer, view, bake_size_for(ma, layer), targets);
  if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT) {
    /* A contextless plan must not replace the reason a real view layer found: the same row is due
     * checked here from the tagged update, which has no scene to resolve objects with. */
    bake_object_issue_record(ma, layer, targets.issue);
  }
  return targets.issue;
}

/**
 * Drop the bakes of the Object/Generated rows whose objects moved under them: an edit of the mesh
 * or of its UV layer changes the surface a row was rendered from without touching the description,
 * so the stored hash alone would keep the maps current forever.
 *
 * Runs on the main thread from the planner, against the objects the evaluated depsgraph of the
 * view just produced (the depsgraph re-evaluates the geometry on `ID_RECALC_GEOMETRY` or a modifier
 * change, which is what brings the new surface here). A moved object moves nothing: object space
 * and generated space are the object's own. Main thread only.
 */
void bake_object_identities_notice(Material &ma, const PaintLayersBakeView &view)
{
  if (view.issue == PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT || !BKE_paint_layers_is_layered(ma)) {
    return;
  }
  bool changed = false;
  /* A row, or a mask effect item whose grey is baked per object like one. */
  const auto notice_node = [&](MaterialPaintLayer &layer) -> bool {
        if (layer.bake == nullptr || layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER ||
            !paint_layers_bake_node_reads_surface(ma, layer))
        {
          return true;
        }
        if (layer.bake->object_count == 0 && layer.bake->hash[0] == 0 && layer.bake->hash[1] == 0) {
          /* Never baked for objects (maps precreated with the Gradient, or released): there is
           * nothing a changed object could have made stale, and marking it again would tag the
           * material on every pass, which brings the next pass, without a bake ever starting. */
          return true;
        }
        PaintLayersBakeTargets targets;
        paint_layers_bake_targets_resolve(ma, layer, view, layer.bake->size, targets);
        if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK || targets.objects.is_empty()) {
          /* Nothing to compare: the due path decides what such a row owes (see
           * #bake_row_is_due), and it records the reason. */
          return true;
        }
        bool stale = int(targets.objects.size()) != layer.bake->object_count;
        for (const PaintLayersBakeTarget &target : targets.objects) {
          const MaterialPaintLayerBakeTile *tile = bake_tile_find(*layer.bake, target.object_uid);
          if (tile == nullptr) {
            stale = true;
            break;
          }
          const uint64_t identity = paint_layers_bake_row_identity(
              ma, layer, target, layer.bake->size);
          if (tile->hash[0] != uint32_t(identity & 0xFFFFFFFFu) ||
              tile->hash[1] != uint32_t(identity >> 32))
          {
            stale = true;
            break;
          }
        }
        if (stale) {
          /* The maps describe another surface than the one the view layer now has: they are stale,
           * and the next plan re-renders the row. Only the bake is invalidated; the generated tree
           * keeps its shape, so this is not a regeneration and not a user edit. */
          BKE_paint_layers_bake_request(layer);
          changed = true;
        }
        return true;
      };
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        notice_node(layer);
        for (MaterialPaintLayer &item : layer.mask_stack) {
          if (paint_layer_mask_item_bakes_effect(item)) {
            notice_node(item);
          }
        }
        return true;
      });
  if (changed) {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_CANCELLED;
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  }
}

/**
 * Whether the planner considers \a layer at all: a Layer row that is not a filter, not a Pass
 * Through folder and not kept live by a mapping or an object mask somewhere inside it. An Object or
 * Generated row is only a candidate while \a object_issue says its objects can supply a surface.
 */
bool bake_row_is_candidate(const Material &ma,
                           const MaterialPaintLayer &layer,
                           const int object_issue)
{
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* A CPU-computed mask effect owns the grey it lays: the cache is what the live graph reads,
     * like a reader's, so the mode is its only gate (the due check asks the rest). One that reads
     * the surface needs the object itself, like a row does. The branch stands ahead of the
     * reader's, because a Normal to Mask is both -- and what it owns is its own grey, not the
     * slice a reader's bake would hold. */
    return BKE_paint_layers_bake_mode_get(layer) != MA_PAINT_LAYER_BAKE_NEVER &&
           (object_issue == PAINT_LAYERS_BAKE_OBJECTS_OK ||
            !paint_layers_bake_node_reads_surface(ma, layer));
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* A reader owns its slice's cache; a substitution of the row it reads is blocked separately. */
    return paint_layer_anchor_point_reader_baked(ma, layer);
  }
  if (paint_layer_is_owned_effect(ma, layer)) {
    /* An effect that owns its result is baked like a mask effect: the cache is what the live graph
     * reads, so the mode is its only gate (the due check asks the rest). */
    return BKE_paint_layers_bake_mode_get(layer) != MA_PAINT_LAYER_BAKE_NEVER;
  }
  if (layer.role != MA_PAINT_LAYER_ROLE_LAYER || paint_layer_row_is_adjustment(layer)) {
    return false;
  }
  /* A row that reads a material has no live subtree to cache; what it shows is rendered from its
   * source, which is not the planner's to do from the row's own description. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    return false;
  }
  /* A subtree holding a generator the bake cannot evaluate is never cached. */
  if (!BKE_paint_layers_generator_subtree_is_bakeable(layer)) {
    return false;
  }
  /* An Object or Generated row reads the geometry of each object it is shaded on, so it needs the
   * object itself: without one, or with one that cannot supply a surface, the row stays live. */
  if (object_issue != PAINT_LAYERS_BAKE_OBJECTS_OK) {
    return false;
  }
  return !BKE_paint_layers_bake_blocked(ma, layer);
}

/**
 * Whether \a layer is a row the planner has to bake now: a candidate, not switched off or hidden,
 * not the row being edited, with no valid bake, and -- for AUTO -- heavy enough and, for a folder,
 * nested deep enough to be worth a cache.
 *
 * \param view: the view an Object or Generated row is baked for.
 * \param r_release: set when the row carries a bake the rules no longer want, which the caller
 * releases (#BakeRowRelease).
 */
bool bake_row_is_due(const Material &ma,
                     const MaterialPaintLayer &layer,
                     const PaintLayersBakeView &view,
                     BakeRowRelease &r_release)
{
  r_release = BakeRowRelease::Keep;
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* A CPU-computed mask effect is due like a row that carries one: its grey has no live form the
     * graph could lay, so the mode is the only standing gate -- NEVER stands, AUTO and ALWAYS bake.
     * The deferred and hidden checks stand too: a stroke defers the grey the same way it defers a
     * row's content effect (#BKE_paint_layers_bake_row_is_deferred). */
    if (BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_NEVER) {
      r_release = bake_row_has_maps(layer) ? BakeRowRelease::ReleaseMaps : BakeRowRelease::Keep;
      return false;
    }
    /* A grey that reads the surface (a projected item before this one) is baked per object, so it
     * has the same gate as a row that does: objects that can supply a surface. A reason a real
     * view layer found drops the maps, as it does for a row; without a view nothing is dropped. */
    if (paint_layers_bake_node_reads_surface(ma, layer)) {
      const int object_issue = bake_row_object_issue(ma, layer, view);
      if (object_issue != PAINT_LAYERS_BAKE_OBJECTS_OK) {
        r_release = (object_issue != PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT &&
                     bake_row_has_maps(layer)) ?
                        BakeRowRelease::ReleaseMaps :
                        BakeRowRelease::Keep;
        return false;
      }
    }
    if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
      return false;
    }
    return !BKE_paint_layers_bake_row_is_deferred(ma, layer) && !bake_row_is_hidden(ma, layer);
  }
  if (layer.role == MA_PAINT_LAYER_ROLE_EFFECT && paint_layer_effect_is_cpu_computed(layer)) {
    if (!paint_layer_is_owned_effect(ma, layer)) {
      /* The row stopped being one the effect keeps a result for (a projection was switched on): the
       * result it baked is not read by anything, so the maps go. */
      r_release = bake_row_has_maps(layer) ? BakeRowRelease::FreeStructure : BakeRowRelease::Keep;
      return false;
    }
    /* An effect that owns its result is due like a mask effect: its result has no live form the graph
     * could lay, so the mode is the only standing gate. The visibility is not part of the hash: a
     * hidden effect keeps its bake (Keep), and showing it again finds the map still current. */
    if (BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_NEVER) {
      r_release = bake_row_has_maps(layer) ? BakeRowRelease::ReleaseMaps : BakeRowRelease::Keep;
      return false;
    }
    const bool valid = layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer);
    if (valid) {
      return false;
    }
    return !BKE_paint_layers_bake_row_is_deferred(ma, layer) && !bake_row_is_hidden(ma, layer);
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* A reader owns a cache only while its effective mode reads one. Switching it back to Live
     * (Never, or Auto after the slice became light again) releases the maps it no longer reads. A
     * record with no maps is only the user's mode and size: it is let alone. */
    if (!paint_layer_anchor_point_reader_baked(ma, layer)) {
      r_release = bake_row_has_maps(layer) ? BakeRowRelease::FreeStructure : BakeRowRelease::Keep;
      return false;
    }
  }
  /* A subtree whose generators the bake cannot evaluate carries nothing worth keeping. The record
   * itself stays: it holds the mode a user chose even when no map was ever made, and the size to
   * bake at if the row becomes bakeable again. */
  if (!BKE_paint_layers_generator_subtree_is_bakeable(layer)) {
    r_release = bake_row_has_maps(layer) ? BakeRowRelease::ReleaseMaps : BakeRowRelease::Keep;
    return false;
  }
  /* An Object or Generated row whose objects cannot supply a surface -- none in the view layer, no
   * mesh, no UV map, too many objects, a size or memory over the limits, a deformation that moves
   * per frame -- keeps no cache: its maps describe one object's geometry and would show it
   * everywhere. Without a view there is nothing to judge the objects by, so nothing is dropped
   * then; the maps are released only for a reason a real view layer found. */
  const int object_issue = bake_row_object_issue(ma, layer, view);
  if (object_issue != PAINT_LAYERS_BAKE_OBJECTS_OK) {
    r_release = (object_issue != PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT && bake_row_has_maps(layer)) ?
                    BakeRowRelease::ReleaseMaps :
                    BakeRowRelease::Keep;
    return false;
  }
  if (!bake_row_is_candidate(ma, layer, object_issue)) {
    return false;
  }
  const bool folder = BKE_paint_layers_is_folder(layer);
  const int mode = BKE_paint_layers_bake_mode_get(layer);
  if (mode == MA_PAINT_LAYER_BAKE_NEVER) {
    return false;
  }
  if (folder && BKE_paint_layers_folder_is_pass_through(layer)) {
    return false;
  }
  /* A generator row is cached for the same reason a reader is: it has no map of its own, so its
   * cache does not double anything, and it is laid per texel -- the cost a cache exists to avoid.
   * The AUTO exemptions below name it beside the reader for that reason. A row a generator is laid
   * over is cached the same way: the bake folds the pattern in, so the stack stops evaluating it
   * per texel the moment the active marker leaves the row. Only the AUTO rules read this, so a
   * switch that already said no never pays for the walk over the row's subtree. */
  const bool generator_texels = bake_cost_is_texels(layer);
  if (mode == MA_PAINT_LAYER_BAKE_AUTO && generator_texels &&
      !bake_generator_auto_allowed(ma, layer))
  {
    /* The material's Gradient answer refuses the automatic bake: the row stays live, and a cache
     * the earlier answer left is released with its size remembered. */
    r_release = (layer.bake != nullptr) ? BakeRowRelease::FreeStructure : BakeRowRelease::Keep;
    return false;
  }
  const bool cheap_live = mode == MA_PAINT_LAYER_BAKE_AUTO &&
                          (paint_layer_kind_reads_anchor_point(layer) || generator_texels);
  /* A row that carries a CPU-computed effect is baked whatever its AUTO gates say: the live graph
   * cannot express the effect, so the bake is what the graph samples instead. The gates below keep
   * every other rule -- the blockers bind through #bake_row_is_candidate, NEVER and the deferred
   * and hidden rows stand. */
  const bool needs_bake = paint_layer_row_needs_bake(layer);
  if (!folder && mode == MA_PAINT_LAYER_BAKE_AUTO && !cheap_live && !needs_bake)
  {
    /* AUTO caches subgraphs: a folder is the heavy thing, a single row is already one map per
     * channel and a cache of it would only double its memory. Always is the way to ask for a row
     * to be baked; a cache carried from an earlier Always keeps standing in while it is valid and
     * is released once it is not. A CPU-computed effect asks the same without the mode: the live
     * graph cannot express it. */
    r_release = (layer.bake != nullptr && !BKE_paint_layers_bake_is_valid(ma, layer)) ?
                    BakeRowRelease::FreeStructure :
                    BakeRowRelease::Keep;
    return false;
  }
  if (folder && mode == MA_PAINT_LAYER_BAKE_AUTO &&
      !BKE_paint_layers_folder_auto_bake_allowed(ma, layer) && !needs_bake)
  {
    /* A folder in the stack root stays live for responsiveness; a bake it carried from before the
     * rule is released. A manual ALWAYS is the user's and is left alone. A CPU-computed effect on
     * the folder asks for the bake the same way it does on a row. */
    r_release = (layer.bake != nullptr) ? BakeRowRelease::FreeStructure : BakeRowRelease::Keep;
    return false;
  }
  if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
    return false;
  }
  if (BKE_paint_layers_bake_row_is_deferred(ma, layer) || bake_row_is_hidden(ma, layer)) {
    return false;
  }
  if (mode == MA_PAINT_LAYER_BAKE_AUTO && !cheap_live && !needs_bake &&
      bake_subtree_weight(ma, layer) <= PAINT_LAYERS_AUTO_BAKE_NODES)
  {
    /* A light subtree is cheaper live; a bake carried over from when it was heavy is released. A
     * reader is exempt: its bake is the slice cache the depth rules asked for, not a cache of a
     * subtree, so a light row deep in a chain still gets one. Neither has a map the live path could
     * read instead, which is the whole reason the exemption is safe. A CPU-computed effect is
     * exempt the same way: its bake is what the live graph samples. */
    r_release = (layer.bake != nullptr) ? BakeRowRelease::FreeStructure : BakeRowRelease::Keep;
    return false;
  }
  return true;
}

/**
 * The stack rows from the root down to the row \a marker names -- the row itself, or the row whose
 * corrections or mask items hold it -- outermost first; false when nothing of \a ma carries it.
 * The only walk a switch needs: a row whose subtree holds the marker being left is on this path.
 */
static bool bake_path_to_marker(const PaintLayerList &list,
                                const bUUID &marker,
                                Vector<const MaterialPaintLayer *> &r_path)
{
  for (const MaterialPaintLayer &layer : list) {
    r_path.append(&layer);
    if (BLI_uuid_equal(layer.marker, marker)) {
      return true;
    }
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (BLI_uuid_equal(effect.marker, marker)) {
        return true;
      }
    }
    for (const MaterialPaintLayer &item : layer.mask_stack) {
      if (BLI_uuid_equal(item.marker, marker)) {
        return true;
      }
    }
    if (BKE_paint_layers_is_folder(layer) && bake_path_to_marker(layer.children, marker, r_path)) {
      return true;
    }
    r_path.remove_last();
  }
  return false;
}

/**
 * Whether moving the active marker from \a old_marker to \a new_marker leaves a bake due: the rows
 * a switch stops deferring (see #BKE_paint_layers_bake_row_is_deferred) are the ones whose cache
 * may have to catch up now. Read after the marker moved, so the answer is what the next plan would
 * find; a row either marker keeps deferred owes nothing to the switch.
 *
 * Only a row on the old marker's ancestor path can turn due -- every other row's subtree holds
 * neither marker -- so the walk follows that path instead of the whole material. Going upward from
 * the row left behind, the first ancestor that still holds the new marker ends the question: every
 * one above it holds the new marker too, and is as deferred as it was.
 *
 * Answered without a scene: an Object or Generated row cannot be proven due here, since a bake of
 * it needs the objects of a view layer. The last reason the planner recorded decides instead: a
 * permanent one (no UV, animated geometry, too many objects, a size or memory overflow) is not
 * helped by the switch and does not tag the material; anything else may be, and does.
 */
bool paint_layers_bake_pending_after_switch(const Material &ma,
                                            const bUUID &old_marker,
                                            const bUUID &new_marker)
{
  if (!BKE_paint_layers_is_layered(ma) || BLI_uuid_equal(old_marker, new_marker)) {
    return false;
  }
  Vector<const MaterialPaintLayer *> path;
  if (!bake_path_to_marker(ma.paint_layers, old_marker, path)) {
    return false;
  }
  PaintLayersBakeView no_context;
  no_context.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  for (int index = int(path.size()) - 1; index >= 0; index--) {
    const MaterialPaintLayer &layer = *path[index];
    if (BKE_paint_layers_subtree_contains(layer, new_marker)) {
      break;
    }
    BakeRowRelease release = BakeRowRelease::Keep;
    if (paint_layers_bake_subtree_needs_surface(layer) &&
        BKE_paint_layers_bake_mode_get(layer) != MA_PAINT_LAYER_BAKE_NEVER &&
        !bake_row_is_hidden(ma, layer) &&
        bake_row_is_candidate(ma, layer, PAINT_LAYERS_BAKE_OBJECTS_OK))
    {
      if (BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_AUTO &&
          bake_cost_is_texels(layer) && !bake_generator_auto_allowed(ma, layer))
      {
        /* The material's Gradient answer keeps this row live: the switch changes nothing. */
        continue;
      }
      if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
        /* A valid cache owes nothing to the switch; a changed surface is noticed by the plan. */
        continue;
      }
      const int issue = BKE_paint_layers_bake_object_issue_get(ma, layer);
      if (ELEM(issue,
               PAINT_LAYERS_BAKE_OBJECTS_NO_UV,
               PAINT_LAYERS_BAKE_OBJECTS_ANIMATED,
               PAINT_LAYERS_BAKE_OBJECTS_TOO_MANY,
               PAINT_LAYERS_BAKE_OBJECTS_TOO_LARGE,
               PAINT_LAYERS_BAKE_OBJECTS_OVER_BUDGET,
               PAINT_LAYERS_BAKE_OBJECTS_TILES))
      {
        /* The switch changes no object: the reason would be the same after the update. */
        continue;
      }
      if (!bake_row_is_due(ma, layer, no_context, release)) {
        return true;
      }
      continue;
    }
    if (bake_row_is_due(ma, layer, no_context, release)) {
      return true;
    }
  }
  return false;
}

/**
 * Report one bake of a generated row: the time it took, the maps it left and what they cost.
 *
 * The cost is what the formats decide: at 4K a float RGBA map is 256 MiB against a byte one's 64,
 * so a row that lays in several precise channels is a large allocation and the line says so before
 * the user has to wonder where the memory went. \a delta_mib is what the row's bake changed in the
 * allocator's live total, and \a retained_results with \a retained_mib is what a worker held at the
 * moment of the report -- the results the commit has not written yet; a synchronous bake holds none.
 */
void bake_gradient_row_report(const MaterialPaintLayer &layer,
                              const int size,
                              const bool sync,
                              const double seconds,
                              const Span<int> channels,
                              const double delta_mib,
                              const int retained_results,
                              const double retained_mib)
{
  char formats[256] = "";
  double mib = 0.0;
  for (const int channel : channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    const bool floatbuf = bake_channel_needs_float(layer, info);
    mib += double(size) * double(size) * (floatbuf ? 16.0 : 4.0) / (1024.0 * 1024.0);
    char entry[64];
    SNPRINTF(entry, "%s:%s ", info.ui_name, floatbuf ? "float" : "byte");
    BLI_strncat(formats, entry, sizeof(formats));
  }
  /* The coverage map is one byte RGBA plane whatever the channels are. */
  mib += double(size) * double(size) * 4.0 / (1024.0 * 1024.0);
  BLI_strncat(formats, "coverage:byte", sizeof(formats));
  printf("[paint-layers gradient row bake] row \"%s\" size=%d sync=%d time=%.1f ms maps=%.1fMiB "
         "[%s] delta=%.1fMiB held=%d(%.1fMiB)\n",
         layer.name,
         size,
         int(sync),
         seconds * 1000.0,
         mib,
         formats,
         delta_mib,
         retained_results,
         retained_mib);
  fflush(stdout);
}

/* The islands are grown outward so the map's bilinear filtering finds an edge rather than empty
 * texels, and the orco grid is kept only when a Generated generator reads it. */
std::shared_ptr<const PaintMaterialCompositeSurface> bake_surface_build(
    const MaterialPaintLayer &layer,
    const PaintLayersBakeTarget &target,
    const int size,
    const int uv_tile)
{
  const int padding = BKE_paint_composite_island_padding(size);
  return BKE_paint_layers_gradient_surface_build(*target.object,
                                                *target.mesh,
                                                target.uv_map_name,
                                                size,
                                                padding,
                                                BKE_paint_layers_generator_subtree_holds_generated(
                                                    layer),
                                                /* The tile of the object's UVs, not the tile the
                                                 * result is written to: the surface and the source
                                                 * maps read through it follow the UVs. A caller
                                                 * that names none gets the single-tile answer. */
                                                uv_tile != 0 ?
                                                    uv_tile :
                                                    BKE_paint_layers_gradient_surface_uv_tile(
                                                        *target.mesh, target.uv_map_name));
}

/** Whether any row of \a ma reads object geometry, so a view has to be resolved for the plan. */
bool bake_any_row_needs_surface(const Material &ma)
{
  bool needs = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        needs = paint_layers_bake_subtree_needs_surface(layer);
        /* A mask effect item reading the surface wants the objects too. */
        for (const MaterialPaintLayer &item : layer.mask_stack) {
          needs = needs ||
                  (paint_layer_mask_item_bakes_effect(item) &&
                   paint_layers_bake_node_reads_surface(ma, item));
        }
        return !needs;
      });
  return needs;
}

/**
 * Render and write one object's tile of \a layer: its channels into the tiled maps at
 * \a tile_number, then the shared coverage. The surface is the one object's and the caller drops it
 * after this call, so only one exists at a time.
 * \return false when the row has nothing to render in any channel.
 */
static bool bake_object_tile_write(
    Main &bmain,
    const Material &ma,
    MaterialPaintLayer &layer,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &surface,
    const int size,
    const int object_num,
    const Span<int> tile_numbers,
    const int tile_number,
    const int uv_tile,
    const int *rect,
    const bool reuse_cache,
    Vector<int> &r_written_channels,
    const Object *painted_object)
{
  Vector<float> color(int64_t(size) * size * 4);
  Vector<float> coverage(int64_t(size) * size);
  Vector<float> common(int64_t(size) * size, 0.0f);
  bool any_channel = false;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const int channel = int(info.channel);
    /* A partial re-bake needs the maps to already exist: an image created just now cannot take a
     * partial write, its untouched region would stay blank. */
    const int *channel_rect = (rect != nullptr && layer.bake->images[channel] != nullptr) ? rect :
                                                                                           nullptr;
    if (!BKE_paint_layers_bake_render_node(
            ma,
            layer,
            channel,
            size,
            color.data(),
            coverage.data(),
            channel_rect,
            surface,
            /* The UV tile is named, not left to the surface: the effect cache keys the result on
             * it and a projection rasterizes into it, so each tile of a block is its own render. */
            PaintLayerTileContext{.painted_object = painted_object, .tile_number = uv_tile}))
    {
      continue;
    }
    Image *color_image = bake_service_image(bmain,
                                            layer,
                                            channel,
                                            size,
                                            info.is_color,
                                            bake_channel_needs_float(layer, info),
                                            object_num,
                                            tile_numbers);
    if (color_image == nullptr) {
      continue;
    }
    bake_write_image(*color_image, color.data(), info.is_color, channel_rect, tile_number);
    r_written_channels.append(channel);
    const int ax0 = (channel_rect != nullptr) ? channel_rect[0] : 0;
    const int ay0 = (channel_rect != nullptr) ? channel_rect[1] : 0;
    const int ax1 = (channel_rect != nullptr) ? channel_rect[2] : size;
    const int ay1 = (channel_rect != nullptr) ? channel_rect[3] : size;
    for (int y = ay0; y < ay1; y++) {
      for (int x = ax0; x < ax1; x++) {
        const int64_t i = int64_t(y) * size + x;
        const float content = color[i * 4 + 3];
        if (content > 0.002f) {
          common[i] = std::max(common[i], clamp_f(coverage[i] / content, 0.0f, 1.0f));
        }
      }
    }
    any_channel = true;
  }
  if (!any_channel) {
    return false;
  }
  /* One shared coverage write for all channels; a partial re-bake touches only its rectangle, so
   * the stored common outside it survives. */
  if (Image *coverage_image = bake_service_image(
          bmain, layer, -1, size, false, false, object_num, tile_numbers))
  {
    bake_write_coverage_image(*coverage_image,
                              common.data(),
                              (rect != nullptr && reuse_cache) ? rect : nullptr,
                              tile_number);
  }
  return true;
}

bool BKE_paint_layers_bake_plan_run(Main &bmain,
                                    Material &ma,
                                    bool *r_changed,
                                    const Scene *scene,
                                    const ViewLayer *view_layer,
                                    const bool notice_surface_edits)
{
  bool changed = false;
  PaintLayersBakeView view;
  if (BKE_paint_layers_is_layered(ma)) {
    /* The objects the view layer just produced are what an Object or Generated row is rendered
     * from, and every row of the material reads the same list: resolve it once for the whole plan.
     * An edited mesh or UV layer invalidates its rows here, before anything asks what is due. */
    const bool any_surface = bake_any_row_needs_surface(ma);
    if (scene == nullptr || view_layer == nullptr) {
      view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
    }
    else if (any_surface) {
      paint_layers_bake_view_resolve(ma, &bmain, scene, view_layer, view);
    }
    if (any_surface) {
      /* A row that lost its last tile -- cleared from the UI, or released below -- must not leave
       * the shader's index property behind on the objects of this view. The resolved objects are
       * reused; only an unresolvable view walks the depsgraph again. */
      bake_tile_object_indices_clear(ma, scene, view_layer, &view);
    }
    if (notice_surface_edits) {
      bake_object_identities_notice(ma, view);
    }
    Vector<MaterialPaintLayer *> rows;
    BKE_paint_layers_foreach(ma,
                             [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
                               rows.append(&layer);
                               for (MaterialPaintLayer &effect : layer.effects) {
                                 if (paint_layer_kind_reads_anchor_point(effect)) {
                                   rows.append(&effect);
                                 }
                               }
                               /* A CPU-computed effect that owns its result is a row of the plan,
                                * and so is one that holds a bake it no longer has a reason for (the
                                * due check releases it). Appended last-first, because the plan walks
                                * the rows backwards: the lower effect bakes before the one above it
                                * reads it as its input, and is not computed twice. */
                               Vector<MaterialPaintLayer *> effect_rows;
                               for (MaterialPaintLayer &effect : layer.effects) {
                                 if (paint_layer_effect_is_cpu_computed(effect) &&
                                     (effect.bake != nullptr ||
                                      paint_layer_effect_owns_result(ma, layer, effect)))
                                 {
                                   effect_rows.append(&effect);
                                 }
                               }
                               for (int i = int(effect_rows.size()) - 1; i >= 0; i--) {
                                 rows.append(effect_rows[i]);
                               }
                               for (MaterialPaintLayer &item : layer.mask_stack) {
                                 if (paint_layer_kind_reads_anchor_point(item) ||
                                     paint_layer_mask_item_bakes_effect(item))
                                 {
                                   rows.append(&item);
                                 }
                               }
                               return true;
                             });
    /* Children before their parents: a parent's render composites its children, and a child that is
     * already baked stands in for its whole subtree, so the reverse of the pre-order walk does the
     * least work. */
    for (int index = int(rows.size()) - 1; index >= 0; index--) {
      MaterialPaintLayer &layer = *rows[index];
      BakeRowRelease release = BakeRowRelease::Keep;
      if (!bake_row_is_due(ma, layer, view, release)) {
        changed |= bake_row_release_apply(bmain, ma, layer, release);
        continue;
      }
      /* A heavy row leaves the main thread: the worker job picks it up from the same stale signal
       * this function leaves set. */
      const bool heavy = BKE_paint_layers_bake_is_heavy(ma, layer);
      PBR_LAYERS_LOG("[PBR-Layers bake] row \"%s\" is due for a bake (heavy=%d => %s)\n",
             layer.name,
             int(heavy),
             heavy ? "left to the worker job" : "baked now on the main thread");
      /* A background session has no window to own a worker job, so nothing would ever bake the
       * effect: it computes here, as it did before it left the main thread. */
      const bool owned_effect_inline = G.background && paint_layer_is_owned_effect(ma, layer);
      /* A projected row with a CPU effect is a job's too, except where a job cannot serve it: a
       * handle drag re-renders it at every step at the coarse drag size, and a session without a
       * window has no job to own it. */
      const bool projection_inline = heavy && (G.background || bake_drag_active(ma)) &&
                                     bake_subtree_folds_projection(ma, layer);
      if (heavy && !owned_effect_inline && !projection_inline) {
        continue;
      }
      if (paint_layer_is_owned_effect(ma, layer)) {
        /* The effect's result is one map per channel it filters, written whole from the effect
         * cache's compute: a stroke defers it (#BKE_paint_layers_bake_row_is_deferred), so no region
         * path is wanted here. */
        if (bake_effect_row_write(bmain, ma, layer)) {
          changed = true;
        }
        continue;
      }
      int size = bake_size_for(ma, layer);
      if (size <= 0) {
        /* No map has known dimensions yet: nothing to allocate a bake structure for. */
        continue;
      }
      const int full_size = size;
      const bool mask_effect = paint_layer_mask_item_bakes_effect(layer);
      /* A grey that reads the surface follows a dragged projection frame like a projected row. */
      if (size > BAKE_DRAG_SIZE_MAX && bake_drag_active(ma) &&
          (bake_subtree_has_decal_effect(layer) ||
           (mask_effect && paint_layers_bake_node_reads_surface(ma, layer))))
      {
        /* A handle drag re-renders this row at every step: the maps are re-created at the drag
         * size, and the full bake is owed once the drag ends (#BKE_paint_layers_bake_drag_set). */
        size = BAKE_DRAG_SIZE_MAX;
        bake_drag_reduced_remember(ma, layer, full_size);
      }
      if (mask_effect) {
        /* The item's grey is one map of its own, written whole: a stroke into the mask defers it
         * (#BKE_paint_layers_bake_row_is_deferred), so no region path is wanted here. A grey that
         * reads the surface is written per object and UV tile, into the tiles the objects' blocks
         * name, like a row's per-object bake. */
        PaintLayersBakeTargets targets;
        if (paint_layers_bake_node_reads_surface(ma, layer)) {
          paint_layers_bake_targets_resolve(ma, layer, view, size, targets);
          if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK || targets.objects.is_empty()) {
            /* The objects or their surface moved between the plan and this render. */
            continue;
          }
          /* The shader reads the tile index from each object; write it before any map is sampled. */
          MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
          paint_layers_bake_tiles_clear(*bake);
          bake_tile_object_indices_clear(ma, scene, view_layer, &view);
          bake_tile_object_indices_set(ma, targets);
        }
        if (bake_mask_effect_row_write(bmain, ma, layer, size, targets.objects)) {
          if (size == full_size) {
            bake_drag_reduced_forget(ma, layer);
          }
          changed = true;
        }
        continue;
      }
      PaintLayersBakeTargets targets;
      paint_layers_bake_targets_resolve(ma, layer, view, size, targets);
      const bool needs_surface = paint_layers_bake_subtree_needs_surface(layer);
      if (needs_surface && targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK) {
        /* The objects or their surface moved between the plan and this render. */
        continue;
      }
      const int object_num = int(targets.objects.size());
      /* The tiles of the objects' blocks, and the renders each object owes (one per UV tile with
       * geometry; one tile per object in the single-tile scheme). */
      const Vector<int> image_tiles = bake_targets_image_tiles(targets.objects);
      Vector<Vector<PaintLayersBakeTilePair>> object_pairs;
      for (const int object_index : targets.objects.index_range()) {
        object_pairs.append(bake_target_tile_pairs(targets.objects[object_index], object_index));
      }
      /* Build the first surface before the bake record exists: a row whose object cannot supply one
       * must not be left with an empty, permanently invalid cache. The remaining surfaces are built
       * one at a time in the loop below, after the previous one was written and released. */
      std::shared_ptr<const PaintMaterialCompositeSurface> first_surface;
      if (needs_surface) {
        first_surface = bake_surface_build(
            layer, targets.objects[0], size, object_pairs[0][0].uv_tile);
        if (first_surface == nullptr) {
          continue;
        }
      }

      /* Every gate has passed and this row renders right now: only here is it safe to allocate the
       * bake structure, so a row nothing ever bakes never carries a permanently invalid one. */
      MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
      bake->size = size;
      if (needs_surface) {
        paint_layers_bake_tiles_clear(*bake);
        bake_tile_object_indices_clear(ma, scene, view_layer, &view);
        /* The shader reads the tile index from each object; write it before any map is sampled. */
        bake_tile_object_indices_set(ma, targets);
      }

      /* A changed rectangle may update only that part of an existing cache; a fresh cache needs the
       * whole render. */
      int region[4];
      bool has_region = BKE_paint_layers_bake_changed_region(ma, layer, region);
      /* A rectangle only describes pixel edits. When the structural hash moved too, the whole cache
       * is out of date and a rectangle-only write would keep the old content outside it. */
      uint32_t current_hash[2];
      BKE_paint_layers_bake_hash(ma, layer, current_hash);
      if (current_hash[0] != bake->hash[0] || current_hash[1] != bake->hash[1]) {
        has_region = false;
      }
      /* A Decal step moves texels across the surface (a fold, a rotation): a changed rectangle of the
       * source lands anywhere in the result, so the whole cache is rendered again. */
      if (bake_subtree_has_decal_effect(layer)) {
        has_region = false;
      }
      /* The same holds for a projection folded into the bake: a texel edited in a source map lands
       * wherever the frame sends it, in any tile of any object's block. */
      if (bake_subtree_folds_projection(ma, layer)) {
        has_region = false;
      }
      const bool reuse_cache = bake->coverage != nullptr;
      int src_width = 0;
      int src_height = 0;
      bool have_source_dims = false;
      if (has_region && reuse_cache) {
        for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
          if (BKE_paint_layers_row_dimensions(ma, layer, int(info.channel), src_width, src_height) &&
              src_width > 0 && src_height > 0)
          {
            have_source_dims = true;
            break;
          }
        }
      }
      int rect[4] = {};
      const int *rect_ptr = nullptr;
      if (has_region && reuse_cache && have_source_dims) {
        /* Expand the changed rectangle to tile boundaries, in bake pixels. */
        constexpr int tile = 256;
        rect[0] = clamp_i((int(int64_t(region[0]) * size / src_width) / tile) * tile, 0, size);
        rect[1] = clamp_i((int(int64_t(region[2]) * size / src_height) / tile) * tile, 0, size);
        rect[2] = clamp_i(((int(int64_t(region[1]) * size / src_width) / tile) + 1) * tile, 0, size);
        rect[3] = clamp_i(((int(int64_t(region[3]) * size / src_height) / tile) + 1) * tile, 0, size);
        rect_ptr = rect;
      }

      const bool gradient_row = paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES);
      const double gradient_start = gradient_row ? BLI_time_now_seconds() : 0.0;
      const size_t gradient_memory_start = gradient_row ? MEM_get_memory_in_use() : 0;
      Vector<int> written_channels;
      bool any_channel = false;
      bool all_objects = true;
      /* The surface is built for one object and dropped before the next: decision 4 of the spec,
       * the transient canvas never holds every object at once. */
      for (int object_index = 0; object_index < std::max(1, object_num); object_index++) {
        /* A row reading no geometry is one render without a surface; an object is one render per
         * UV tile of its block, each into its own tile of the maps. */
        const int pairs_num = object_num > 0 ? int(object_pairs[object_index].size()) : 1;
        bool object_ok = true;
        for (int pair_index = 0; pair_index < pairs_num; pair_index++) {
          std::shared_ptr<const PaintMaterialCompositeSurface> surface;
          int tile_number = 0;
          int uv_tile = 0;
          if (object_num > 0) {
            const PaintLayersBakeTilePair pair = object_pairs[object_index][pair_index];
            surface = (object_index == 0 && pair_index == 0 && first_surface != nullptr) ?
                          first_surface :
                          bake_surface_build(
                              layer, targets.objects[object_index], size, pair.uv_tile);
            if (surface == nullptr) {
              object_ok = false;
              break;
            }
            tile_number = pair.image_tile;
            uv_tile = pair.uv_tile;
          }
          Vector<int> tile_channels;
          if (!bake_object_tile_write(bmain,
                                      ma,
                                      layer,
                                      surface,
                                      size,
                                      object_num,
                                      image_tiles,
                                      tile_number,
                                      uv_tile,
                                      rect_ptr,
                                      reuse_cache,
                                      tile_channels,
                                      object_num > 0 ? targets.objects[object_index].object :
                                                       nullptr))
          {
            object_ok = false;
            break;
          }
          any_channel = true;
          for (const int channel : tile_channels) {
            if (!written_channels.contains(channel)) {
              written_channels.append(channel);
            }
          }
        }
        if (!object_ok) {
          all_objects = false;
          continue;
        }
        if (object_num > 0) {
          const PaintLayersBakeTarget &target = targets.objects[object_index];
          MaterialPaintLayerBakeTile *tile = bake_tile_ensure(
              *bake, target.object_uid, target.block_origin[0], target.block_origin[1]);
          const uint64_t identity = paint_layers_bake_row_identity(ma, layer, target, size);
          tile->hash[0] = uint32_t(identity & 0xFFFFFFFFu);
          tile->hash[1] = uint32_t(identity >> 32);
        }
      }
      if (!any_channel || !all_objects) {
        continue;
      }
      if (needs_surface) {
        bake->object_count = object_num;
      }
      bake_stale_channel_maps_release(bmain, layer, written_channels);
      BKE_paint_layers_bake_finalize(ma, layer);
      if (object_num > 0) {
        bake_object_identity_record(
            ma, layer, paint_layers_bake_row_identity(ma, layer, targets.objects[0], size));
      }
      if (size == full_size) {
        bake_drag_reduced_forget(ma, layer);
      }
      if (gradient_row) {
        /* A synchronous bake writes and drops each channel as it renders, so it holds no results
         * for a commit, and the delta is the render of the row itself. */
        const double delta_mib = double(int64_t(MEM_get_memory_in_use()) -
                                        int64_t(gradient_memory_start)) /
                                 (1024.0 * 1024.0);
        bake_gradient_row_report(layer,
                                 size,
                                 true,
                                 BLI_time_now_seconds() - gradient_start,
                                 written_channels,
                                 delta_mib,
                                 0,
                                 0.0);
      }
      changed = true;
    }
  }
  if (changed) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    /* Bake maps were written into the layers outside any operator: the undo system must see the
     * material as changed or it keeps the stored one, pointing at maps the restored state lacks. */
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  }
  /* The queue is drained once no baked row is invalid any more; only then is the signal cleared. A
   * hidden row stays stale without holding it: showing it marks it again. */
  bool pending = false;
  if (BKE_paint_layers_is_layered(ma)) {
    BKE_paint_layers_foreach(
        ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
          if (layer.bake != nullptr && layer.bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
              bake_row_is_candidate(ma, layer, bake_row_object_issue(ma, layer, view)) &&
              !BKE_paint_layers_bake_is_valid(ma, layer) && !bake_row_is_hidden(ma, layer))
          {
            pending = true;
          }
          /* An effect that owns its result and waits for the stroke to end is pending like a
           * deferred row: the signal stays up until the plan after the stroke bakes it. */
          for (const MaterialPaintLayer &effect : layer.effects) {
            pending = pending || bake_owned_effect_pending(ma, effect);
          }
          return !pending;
        });
  }
  if (!pending) {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
  }
  if (r_changed != nullptr) {
    *r_changed = changed;
  }
  return changed;
}

bool BKE_paint_layers_bake_heavy_pending(const Material &ma,
                                         const Scene *scene,
                                         const ViewLayer *view_layer)
{
  if (!BKE_paint_layers_is_layered(ma) || (ma.paint_layers_flag & MA_PAINT_LAYERS_BAKE_CANCELLED) != 0) {
    return false;
  }
  PaintLayersBakeView view;
  if (scene == nullptr || view_layer == nullptr) {
    view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  }
  else if (bake_any_row_needs_surface(ma)) {
    paint_layers_bake_view_resolve(ma, G_MAIN, scene, view_layer, view);
  }
  bool pending = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        BakeRowRelease release = BakeRowRelease::Keep;
        pending = bake_row_is_due(ma, layer, view, release) &&
                  BKE_paint_layers_bake_is_heavy(ma, layer);
        for (const MaterialPaintLayer &effect : layer.effects) {
          /* The worker job computes an effect's own result like a heavy row's render. An effect
           * with no canvas size yet is no work for it (the job skips it too). */
          pending = pending || (paint_layer_is_owned_effect(ma, effect) &&
                                bake_row_is_due(ma, effect, view, release) &&
                                bake_size_for(ma, effect) > 0);
        }
        return !pending;
      });
  return pending;
}

/** \} */

}  // namespace blender
