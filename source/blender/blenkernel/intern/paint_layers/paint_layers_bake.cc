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
 * `paint_layers_bake_intern.hh`.
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
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"
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
#include "BKE_mesh_maps.hh"
#include "BKE_modifier.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers_gradient_surface.hh"
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

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/**
 * Seconds without an edit of the source before the active row is rendered for a channel that no
 * live form stands in for (the Normal of a Bump). Counted from the last edit, so dragging a value
 * that moves it renders nothing until the hand stops.
 */
static constexpr float MAP_ONLY_EDIT_DELAY_SECONDS = 3.0f;

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

namespace {

struct BakeSubscriptionSource {
  Image *image = nullptr;
  PartialUpdateUser *user = nullptr;
};

struct BakeLayerSubscription {
  bUUID marker = {};
  bool changed = false;
  /** Union of the changed rectangles, valid while #has_region; false means a full re-bake. */
  bool has_region = false;
  int xmin = 0, xmax = 0, ymin = 0, ymax = 0;
  Vector<BakeSubscriptionSource> sources;
};

/** The hashes a Material row's bake was started from and did not land from: not retried as is. */
struct BakeFailure {
  uint32_t hash[2] = {};
  uint32_t source[2] = {};
  /** The render that failed was a preview; the render at the row's own size is another one. */
  bool preview = false;
};

struct BakeMaterialRuntime {
  /**
   * The material that owns this entry. Used so the free path of a same-`session_uid` material (the
   * old ID during a memfile undo) does not delete the entry the re-read ID still uses.
   */
  const Material *owner = nullptr;
  Vector<BakeLayerSubscription> layers;
  /**
   * The bake size of a row whose bake structure the AUTO rules released: the structure is the only
   * copy of the size, but the size is the user's choice rather than part of the cache, so the row's
   * next bake takes it from here instead of the content dimensions.
   */
  Map<UUID, int> dropped_size;
  /**
   * Material rows whose last bake was cancelled or failed, by marker. A render that cannot finish
   * would otherwise be started again by every scene update; the row waits for something to change
   * (the source, the row) or for an explicit retry.
   */
  Map<UUID, BakeFailure> failed;
  /**
   * The shape each Material row last gave its group (#paint_layer_material_live_signature), by
   * marker: what the scene update compares against to know a source edit changed what the
   * generator builds.
   */
  Map<UUID, uint64_t> live_signature;
  /**
   * What the Normal read in the source when the last landed render of each Material row started, by
   * marker: a source edit that leaves it alone is no reason to render when only the Normal is.
   * Runtime only; a row read from a file renders once on its first edit.
   */
  Map<UUID, uint64_t> normal_hash;
  /**
   * Material rows whose last landed render was partial (#bake_row_maps_partial): the maps of the
   * channels the shader shows live were not rendered, so what reads them (an Image Editor) needs a
   * full render before it can trust them.
   */
  Set<UUID> partial_rows;
  /** What a started render recorded about itself, by the full source hash it started from. */
  struct StartedRender {
    uint64_t normal_hash = 0;
    bool partial = false;
  };
  Map<uint64_t, StartedRender> started;
  /**
   * The surface identity each Object/Generated row was rendered from, by row marker
   * (#BKE_paint_layers_gradient_surface_identity). An edit of the geometry or of the UV layer
   * moves it and the row's bake is stale without any description change; a moved object does not,
   * since object space is the object's own.
   */
  Map<UUID, uint64_t> object_identity;
  /**
   * Why an Object/Generated row cannot be baked right now, by row marker
   * (#ePaintLayersBakeObjectIssue). What the Bake panel shows beside a row that stays live.
   */
  Map<UUID, int> object_issue;
};

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
  BKE_paint_layers_source_material_hash_forget(ma);
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  /* Only the owning material may drop the entry: a same-uid old ID freed during a memfile undo must
   * leave the re-read ID's subscriptions (and their `PartialUpdateUser`s) in place. */
  if (runtime == nullptr || runtime->owner != &ma) {
    return;
  }
  for (BakeLayerSubscription &entry : runtime->layers) {
    for (BakeSubscriptionSource &source : entry.sources) {
      BKE_image_partial_update_free(source.user);
    }
  }
  bake_runtime().remove(ma.id.session_uid);
}

void BKE_paint_layers_bake_runtime_owner_transfer(Material &dst, Material &src)
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(src.id.session_uid);
  if (runtime != nullptr && runtime->owner == &src) {
    runtime->owner = &dst;
  }
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

static MaterialPaintLayerBakeTile *bake_tile_ensure(MaterialPaintLayerBake &bake,
                                                    const uint32_t object_uid,
                                                    const int index)
{
  if (MaterialPaintLayerBakeTile *tile = bake_tile_find(bake, object_uid)) {
    return tile;
  }
  MaterialPaintLayerBakeTile *tile = MEM_new<MaterialPaintLayerBakeTile>(__func__);
  tile->object_uid = object_uid;
  tile->tile[0] = index % 10;
  tile->tile[1] = index / 10;
  BLI_addtail(&bake.tiles, tile);
  return tile;
}

/**
 * Whether \a layer's maps carry a tile for every object the planner last resolved. The shader may
 * stand in only then: a missing tile would show another object's pixels.
 */
static bool bake_tiles_ready(const Material &ma, const MaterialPaintLayer &layer)
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

/** Write \a index into \a object's tile property so the shader can offset into its tile. */
static void bake_tile_object_index_set(const Object &object, const char *path, const int index)
{
  const Object *original = DEG_get_original(&object);
  Object *object_mut = const_cast<Object *>(original != nullptr ? original : &object);
  IDProperty *properties = IDP_EnsureProperties(&object_mut->id);
  IDProperty *property = IDP_GetPropertyFromGroup(properties, path);
  if (property == nullptr) {
    IDP_AddToGroup(properties, IDP_NewInt(index, path));
  }
  else if (property->type == IDP_INT) {
    if (IDP_int_get(property) == index) {
      return;
    }
    IDP_int_set(property, index);
  }
  else {
    return;
  }
  /* The shader reads the property through an Attribute node: let the depsgraph copy it. */
  DEG_id_tag_update(&object_mut->id, ID_RECALC_SHADING);
}

/** Remove the tile index \a path left on \a object; no bake of the material reads it any more. */
static void bake_tile_object_index_remove(const Object &object,
                                          const char *path,
                                          const bool tag_update)
{
  const Object *original = DEG_get_original(&object);
  Object *object_mut = const_cast<Object *>(original != nullptr ? original : &object);
  IDProperty *properties = IDP_GetProperties(&object_mut->id);
  if (properties == nullptr) {
    return;
  }
  IDProperty *property = IDP_GetPropertyFromGroup(properties, path);
  if (property == nullptr) {
    return;
  }
  IDP_FreeFromGroup(properties, property);
  if (tag_update) {
    /* The shader no longer reads the property: let the depsgraph drop its copy too. */
    DEG_id_tag_update(&object_mut->id, ID_RECALC_SHADING);
  }
}

/** The fixed prefix of the object property the shader reads as its bake tile index. */
constexpr char BAKE_TILE_PROPERTY_PREFIX[] = "paint_layer_bake_tile_";

/**
 * Parse a tile-index property name into the 64-bit key \a paint_layers_bake_tile_attribute_path()
 * encodes after its prefix. Returns false for any other property, so the lazy prune never touches
 * an unrelated Custom Property.
 */
static bool bake_tile_property_hash(const char *name, uint64_t &r_hash)
{
  constexpr size_t prefix_len = sizeof(BAKE_TILE_PROPERTY_PREFIX) - 1;
  if (strncmp(name, BAKE_TILE_PROPERTY_PREFIX, prefix_len) != 0) {
    return false;
  }
  const char *hex = name + prefix_len;
  if (strlen(hex) != 16) {
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

/** Give every target object its tile index, in the order the bake writes the tiles. */
static void bake_tile_object_indices_set(const Material &ma,
                                         const PaintLayersBakeTargets &targets)
{
  const std::string path = paint_layers_bake_tile_attribute_path(ma);
  for (int index = 0; index < targets.objects.size(); index++) {
    bake_tile_object_index_set(*targets.objects[index].object, path.c_str(), index);
  }
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
 * or a mask effect item, whose grey is a cache of its own keyed under the item's marker.
 */
static bool bake_row_owned(Material &ma, const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_owner_list(ma, layer) != nullptr ||
         (paint_layer_mask_item_bakes_effect(layer) &&
          BKE_paint_layers_mask_item_owner(ma, layer) != nullptr) ||
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

void BKE_paint_layers_bake_notice_changes(Material &ma)
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr) {
    return;
  }
  bool any = false;
  /* A baked row stops being substituted the moment a source map changes, which is a different shape
   * of generated graph. */
  bool newly_changed = false;
  for (BakeLayerSubscription &entry : runtime->layers) {
    const bool was_changed = entry.changed;
    for (BakeSubscriptionSource &source : entry.sources) {
      const auto result = bke::image::partial_update::BKE_image_partial_update_collect_changes(
          source.image, source.user);
      if (result == bke::image::partial_update::ePartialUpdateCollectResult::FullUpdateNeeded) {
        /* No rectangle to trust: the planner re-bakes the whole row. */
        entry.changed = true;
        entry.has_region = false;
        any = true;
      }
      else if (result ==
               bke::image::partial_update::ePartialUpdateCollectResult::PartialChangesDetected)
      {
        entry.changed = true;
        any = true;
        bke::image::partial_update::PartialUpdateRegion region;
        while (bke::image::partial_update::BKE_image_partial_update_get_next_change(
                   source.user, &region) ==
               bke::image::partial_update::ePartialUpdateIterResult::ChangeAvailable)
        {
          if (!entry.has_region) {
            entry.xmin = region.region.xmin;
            entry.xmax = region.region.xmax;
            entry.ymin = region.region.ymin;
            entry.ymax = region.region.ymax;
            entry.has_region = true;
          }
          else {
            entry.xmin = std::min(entry.xmin, region.region.xmin);
            entry.xmax = std::max(entry.xmax, region.region.xmax);
            entry.ymin = std::min(entry.ymin, region.region.ymin);
            entry.ymax = std::max(entry.ymax, region.region.ymax);
          }
        }
      }
    }
    newly_changed |= entry.changed && !was_changed;
  }
  if (any) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  }
  if (newly_changed) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
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
/** \name Validity
 *
 * The hash is the structural half of validity: everything in the description that changes the
 * pixels of the row's result, and nothing that is only a view of them. Pixel edits of the source
 * maps do not move it; the subscription above reports those.
 * \{ */

static uint64_t bake_hash_mix(uint64_t h, const uint64_t value)
{
  return h * 1000003u ^ value;
}

static uint64_t bake_hash_float(uint64_t h, const float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bake_hash_mix(h, bits);
}

static uint64_t bake_hash_idprops(uint64_t h, const IDProperty *group)
{
  if (group == nullptr || group->type != IDP_GROUP) {
    return h;
  }
  for (const IDProperty *prop = static_cast<const IDProperty *>(group->data.group.first);
       prop != nullptr;
       prop = prop->next)
  {
    /* The smart-asset binding labels are metadata, not pixels: making an instance unique or
     * re-binding it must not invalidate its bakes, so the marks never reach the hash. */
    if (STREQ(prop->name, ASSET_PARAMS_PROP_NAME)) {
      continue;
    }
    for (const char *c = prop->name; *c != '\0'; c++) {
      h = bake_hash_mix(h, uint8_t(*c));
    }
    h = bake_hash_mix(h, prop->type);
    switch (prop->type) {
      case IDP_STRING: {
        const char *text = static_cast<const char *>(prop->data.pointer);
        if (text != nullptr) {
          for (const char *c = text; *c != '\0'; c++) {
            h = bake_hash_mix(h, uint8_t(*c));
          }
        }
        break;
      }
      case IDP_GROUP:
        h = bake_hash_idprops(h, prop);
        break;
      default:
        /* Scalars live in the two int words (a double occupies both). */
        h = bake_hash_mix(h, uint32_t(prop->data.val));
        h = bake_hash_mix(h, uint32_t(prop->data.val2));
        break;
    }
  }
  return h;
}

static uint64_t bake_hash_layer(uint64_t h,
                                const Material &ma,
                                const MaterialPaintLayer &layer,
                                const bool is_child)
{
  /* A content bake's mask, its opacities and its blend are live over the substituted map, so none
   * of them may move the hash of what was rendered; a child of another row's cache keeps its mask
   * inside, being part of the picture that cache holds (#paint_layer_bake_scope). */
  const PaintLayerBakeScope scope = paint_layer_bake_scope(layer, is_child);
  /* Bumped when what a bake stores changes meaning, so older caches invalidate once. */
  h = bake_hash_mix(h, uint64_t(0x9e3779b97f4a7c15ull));
  h = bake_hash_mix(h, uint8_t(layer.source));
  h = bake_hash_mix(h, uint16_t(layer.kind_subtype));
  /* Which geometry map a MESH_MAP row reads decides its result; only those rows carry the field.
   * The atlas Image's identity is folded in when the owning material is known, so pointing the slot
   * at another Image invalidates the row's bake while a pixel edit (which does not change the UID)
   * does not. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    h = bake_hash_mix(h, uint8_t(layer.mesh_map_type));
    const MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, layer.mesh_map_type);
    h = bake_hash_mix(h,
                      (slot != nullptr && slot->image != nullptr) ?
                          uint64_t(slot->image->id.session_uid) :
                          uint64_t(0));
  }
  if (BKE_paint_layers_gradient_supported(layer) && layer.gradient != nullptr) {
    /* A Gradient's pixels are its payload: the line, the shape and the ramp. The channel records'
     * value ramps below carry the scalar channels. The leading version invalidates the caches if
     * the generated formula changes. */
    h = bake_hash_mix(h, BKE_PAINT_LAYERS_GRADIENT_HASH_VERSION);
    h = bake_hash_mix(h, uint8_t(layer.gradient->type));
    h = bake_hash_mix(h, uint8_t(layer.gradient->space));
    h = bake_hash_mix(h, uint8_t(layer.gradient->repeat));
    for (const float value : layer.gradient->start) {
      h = bake_hash_float(h, value);
    }
    for (const float value : layer.gradient->end) {
      h = bake_hash_float(h, value);
    }
    h = bake_hash_float(h, BKE_paint_layers_gradient_midpoint_effective(*layer.gradient));
    h = bake_hash_mix(h, BKE_paint_layers_gradient_ramp_hash(layer.gradient->ramp));
  }
  /* A row's own blend is applied over its cache, so it moves no hash of that cache. A child's is
   * inside the result its parent bakes, so it does. */
  if (is_child) {
    h = bake_hash_mix(h, uint8_t(layer.blend));
  }
  /* The non-visibility flags of the row itself. Visibility is appended only for a child, so the
   * row's own cache does not depend on its own on/off state. The lock is left out with it: it
   * changes no pixel, so putting it in would invalidate a bake for nothing. */
  h = bake_hash_mix(h,
                    uint16_t(layer.flag & ~(int(MA_PAINT_LAYER_ENABLED) |
                                            int(MA_PAINT_LAYER_LOCKED))));
  h = bake_hash_mix(h, uint8_t(layer.role));
  /* Whether the opacity stays live over the cached map rather than being folded into it: a
   * content-only bake holds the content alone, and a mask effect's grey is cached before its own
   * opacity -- the reader lays that live over the map. A child is rendered inside the parent's
   * picture, so the parent's opacity is baked with it. */
  const bool opacity_is_live_over_map = scope == PaintLayerBakeScope::ContentOnly ||
                                        (paint_layer_mask_item_bakes_effect(layer) && !is_child);
  if (!opacity_is_live_over_map) {
    h = bake_hash_float(h, layer.opacity);
  }
  /* Only the fact that the mapping applies: such a row stays live and is never baked
   * (#BKE_paint_layers_bake_blocked), so the slider values must not move the hash. A Material row
   * is rendered and its mapping is applied when its maps are read, so toggling it must not move a
   * parent folder's hash either; the folder is kept live by the same block while the mapping is
   * on. */
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    h = bake_hash_mix(h, BKE_paint_layers_mapping_remaps(ma, layer) ? 1 : 0);
  }
  for (int i = 0; i < layer.channels_num; i++) {
    const MaterialPaintLayerChannel &record = layer.channels[i];
    /* A channel outside the material's set is not part of the result, so its record must not move
     * the hash; re-adding the channel changes the hash and forces the re-bake then. */
    if (paint_layer_channel_filtered(ma, record.channel)) {
      continue;
    }
    h = bake_hash_mix(h, uint8_t(record.channel));
    h = bake_hash_mix(h, uint8_t(record.state));
    h = bake_hash_mix(h, uint8_t(record.flag));
    h = bake_hash_mix(h, record.image != nullptr ? record.image->id.session_uid : 0);
    for (const float value : record.value) {
      h = bake_hash_float(h, value);
    }
    if (record.gradient_ramp != nullptr) {
      h = bake_hash_mix(h, BKE_paint_layers_gradient_ramp_hash(*record.gradient_ramp));
    }
  }
  /* The per (row, channel) opacity overrides are baked into the row's result too, and exist whether
   * or not the pair has a record. The blend follows the same rule as the row's own: a child's is
   * inside the parent's result, the row's own is applied over its cache. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];
    if (is_child) {
      h = bake_hash_mix(h, uint8_t(settings.blend));
    }
    if (scope != PaintLayerBakeScope::ContentOnly) {
      h = bake_hash_float(h, settings.opacity);
    }
    /* The Height record's own mode and scale are baked into the row's Height result the same way,
     * and the Compare mask reaches every other channel of the row through the stack that renders
     * one -- the whole row's cache is stale without them. The effective mode includes the row's
     * ordinary blend: with Inherit, that blend decides the Height step itself, inside the cache, so
     * the row's own blend moves the cache even though the colour blend does not. */
    h = bake_hash_mix(h,
                      uint8_t(BKE_paint_layers_channel_height_blend_effective(layer, channel)));
    h = bake_hash_float(h, settings.height_scale_delta);
    h = bake_hash_mix(h, uint8_t(settings.height_flag));
  }
  /* The Color ID colours of a map-coverage item decide its match. */
  for (const MaterialPaintLayerIdColor &color : layer.id_colors) {
    h = bake_hash_mix(h, uint8_t(color.flag));
    for (const float component : color.color) {
      h = bake_hash_float(h, component);
    }
    h = bake_hash_float(h, color.tolerance);
  }
  /* A child's own bake size is not part of its parent's pixels, and allocating or releasing the
   * child's cache would otherwise invalidate the parent for no visible change. */
  if (!is_child) {
    /* A generated row has no size of its own: its pixels are laid on the material's canvas (see
     * #bake_size_for), so that canvas is what the cache was rendered at. Hashing the stored size
     * instead would keep a bake made before a resize, and the preview would resample it. */
    const uint32_t bake_size =
        paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES) ?
            uint32_t(std::max(int(ma.paint_layers_image_size), 0)) :
            uint32_t(layer.bake != nullptr ? layer.bake->size : 0);
    h = bake_hash_mix(h, bake_size);
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* Inside a baked folder a Material row is read through the maps it shows, so which source and
     * which maps it has move the folder's cache; their pixels are the subscription's. */
    h = bake_hash_mix(h, layer.material != nullptr ? layer.material->id.session_uid : 0);
    h = bake_hash_mix(h, paint_layer_material_live_signature(ma, layer));
    if (layer.bake != nullptr) {
      for (const Image *image : layer.bake->images) {
        h = bake_hash_mix(h, image != nullptr ? image->id.session_uid : 0);
      }
      h = bake_hash_mix(h,
                        layer.bake->coverage != nullptr ? layer.bake->coverage->id.session_uid : 0);
    }
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* The slice's description, channel by channel: what it is made of, never its pixels (those are
     * the subscription's). A cache for one channel therefore follows an edit above its marker. */
    for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      h = bake_hash_mix(h, paint_layer_anchor_point_slice_hash(ma, layer, channel));
    }
  }
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* The grey the item bakes reads the mask built up to it: its description folds in. Zero when
     * the item is in no mask stack, which folds nothing in -- the mix step stays here, beside the
     * other hashes. */
    if (const uint64_t input_hash = bake_mask_effect_input_hash(ma, layer)) {
      h = bake_hash_mix(h, input_hash);
    }
  }
  /* A kind's parameters (a filter's, a coverage operator's) change the pixels. A ramp filter keeps
   * its stops outside the properties, in its own payload; a curves filter its Bézier points. */
  h = bake_hash_idprops(h, layer.system_properties);
  if (layer.ramp != nullptr) {
    h = bake_hash_mix(h, paint_layer_ramp_hash(layer.ramp->band));
  }
  if (layer.curves != nullptr) {
    h = bake_hash_mix(h, paint_layer_curves_hash(layer.curves->curve_mapping));
  }
  /* The folded numbers reach every stack hash through the filter parameters, but the bake reads the
   * payloads directly, so a matrix or balance is part of what the baked map was made from. */
  h = bake_hash_mix(h, paint_layer_payload_hash(layer));
  /* Effects first, then mask items, then children, each in its own storage order. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    h = bake_hash_layer(h, ma, effect, true);
  }
  /* The mask of a content bake is laid live over the map and reaches none of its pixels: a stroke
   * into it must leave the cache current. */
  if (scope != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
      h = bake_hash_layer(h, ma, mask_item, true);
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    h = bake_hash_layer(h, ma, child, true);
  }
  /* The row's own visibility is a live factor and must not move its own bake. A parent's bake
   * renders its children, so a child's visibility does change the parent's result. */
  if (is_child) {
    h = bake_hash_mix(h, (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1 : 0);
  }
  return h;
}

/**
 * The structural hash of a Material row: which material it reads, the side its maps are rendered at
 * and which channels the material asks for. Nothing of the source's content, which only the main
 * thread may read (see #BKE_paint_layers_material_row_stale).
 */
static uint64_t bake_hash_material_row(const Material &ma, const MaterialPaintLayer &layer)
{
  uint64_t h = 1469598103934665603ull;
  h = bake_hash_mix(h, uint64_t(0x4d4154u));
  h = bake_hash_mix(h, layer.material != nullptr ? layer.material->id.session_uid : 0);
  h = bake_hash_mix(h, layer.bake != nullptr ? uint32_t(layer.bake->size) : 0);
  h = bake_hash_mix(h, BKE_paint_layers_channel_set_mask_get(ma));
  return h;
}

void BKE_paint_layers_bake_hash(const Material &ma,
                                const MaterialPaintLayer &layer,
                                uint32_t r_hash[2])
{
  /* A Material row's maps belong to its source and its size; its opacity, blend, mask and
   * corrections stay live over them and are not part of what was rendered. */
  const uint64_t hash = paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ?
                            bake_hash_material_row(ma, layer) :
                            bake_hash_layer(1469598103934665603ull, ma, layer, false);
  r_hash[0] = uint32_t(hash & 0xFFFFFFFFu);
  r_hash[1] = uint32_t(hash >> 32);
}

bool BKE_paint_layers_bake_is_valid(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return false;
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* Reads the source's node trees: main thread only. The generator and the CPU composite ask
     * #paint_layer_row_material_map instead, which reads the row alone. */
    /* A row with nothing to render is as current as it can be. */
    return !BKE_paint_layers_material_row_needs_render(ma, layer) ||
           (paint_layer_row_material_shown(ma, layer) &&
            !BKE_paint_layers_material_row_stale(ma, layer));
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  if (hash[0] != layer.bake->hash[0] || hash[1] != layer.bake->hash[1] ||
      (hash[0] == 0 && hash[1] == 0))
  {
    return false;
  }
  if (!BKE_paint_layers_generator_subtree_is_shared_bakeable(layer) &&
      !bake_tiles_ready(ma, layer))
  {
    return false;
  }
  /* The structural hash cannot see pixel edits; the runtime subscription, drained by
   * #BKE_paint_layers_bake_notice_changes, reports those. A layer with no subscription is trusted
   * on its hash alone. */
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime != nullptr) {
    for (const BakeLayerSubscription &entry : runtime->layers) {
      if (BLI_uuid_equal(entry.marker, layer.marker) && entry.changed) {
        return false;
      }
    }
  }
  return true;
}

void BKE_paint_layers_bake_finalize(Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || !bake_row_owned(ma, layer)) {
    return;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  layer.bake->hash[0] = hash[0];
  layer.bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
}

/**
 * Whether \a layer or anything under it applies a UV mapping: a mapped row stays live.
 *
 * \a scope is what the bake the question is asked for holds (#PaintLayerBakeScope): the own mask
 * of a content bake is laid live over the map, where the graph reads a mapped mask as well as the
 * CPU composite does, so a mapping there is no reason to keep the row out of the bake. Everything
 * rendered into the cache keeps blocking, and so does the row's own mapping.
 */
static bool bake_mapping_blocks(const Material &ma,
                                const MaterialPaintLayer &layer,
                                const PaintLayerBakeScope scope)
{
  if (BKE_paint_layers_mapping_remaps(ma, layer)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (bake_mapping_blocks(ma, effect, PaintLayerBakeScope::Child)) {
      return true;
    }
  }
  if (scope != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
      if (bake_mapping_blocks(ma, mask_item, PaintLayerBakeScope::Child)) {
        return true;
      }
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (bake_mapping_blocks(ma, child, PaintLayerBakeScope::Child)) {
      return true;
    }
  }
  return false;
}

/** Whether any descendant of \a layer has an object mask: the choice is applied in the shader, after every shared cache. */
static bool bake_object_mask_blocks(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &child : layer.children) {
    if ((child.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
        bake_object_mask_blocks(child))
    {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_bake_blocked(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A row holding a read Anchor Point (or a folder above one) is never substituted: the cache
   * would swallow the point. A reader whose slice contains an object mask stays live when its mode
   * allows it; the effective-mode helper decides which. */
  return bake_mapping_blocks(ma, layer, paint_layer_bake_scope(layer, false)) ||
         bake_object_mask_blocks(layer) || paint_layer_anchor_point_blocks_bake(ma, layer);
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
  /* A UDIM mask slice is unsupported rather than bake-able: the reader does not take part. A
   * content slice is refused when a nested mask reader in it has a tiled map. */
  if (paint_layer_anchor_point_slice_is_udim(ma, reader)) {
    return false;
  }
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
/** \name Service maps
 * \{ */

/** The service map of \a layer: its \a channel bake, or the coverage one for a negative channel. */
static Image **bake_service_slot(MaterialPaintLayer &layer, const int channel)
{
  return (channel >= 0) ? &layer.bake->images[channel] : &layer.bake->coverage;
}

/** Whether \a image's generated tile holds a float buffer rather than a byte one. */
static bool bake_image_is_float(Image &image)
{
  const ImageTile *tile = BKE_image_get_tile(&image, 0);
  return tile != nullptr && (tile->gen_flag & IMA_GEN_FLOAT) != 0;
}

/**
 * Whether a bake map of \a layer in \a info's channel is kept as float.
 *
 * A generated row qualifies: its pixels are a formula the shader evaluates in float, so a scalar
 * channel stored as bytes reads back as terraces. Height qualifies everywhere: it is a continuous
 * field, and its painted map is a float buffer too (see
 * #BKE_paint_layers_target_ensure_writable), so a byte bake would leave the merged correction in a
 * different format from the row it replaces. Every other row bakes a map it already holds (or a
 * flat value), which is a byte texture like the maps around it.
 */
bool bake_channel_needs_float(const MaterialPaintLayer &layer, const MaterialPaintChannelInfo &info)
{
  if (info.map_float) {
    return true;
  }
  return info.bake_float && paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES);
}

/**
 * The map of \a layer in \a channel at \a size, made when it is missing or of another size or
 * format.
 *
 * \param floatbuf: keep the values as float. A map of the wrong format cannot be reused: the pixel
 *                  writers index one buffer or the other, and a byte map read as float (or back)
 *                  would be a different picture.
 */
Image *bake_service_image(Main &bmain,
                          MaterialPaintLayer &layer,
                          const int channel,
                          const int size,
                          const bool is_color,
                          const bool floatbuf,
                          const int object_num)
{
  const bool tiled = object_num > 0;
  Image **slot = bake_service_slot(layer, channel);
  if (*slot != nullptr) {
    int width = 0;
    int height = 0;
    BKE_image_get_size(*slot, nullptr, &width, &height);
    const bool tiled_ok = ((*slot)->source == IMA_SRC_TILED) == tiled;
    if (width == size && height == size && bake_image_is_float(**slot) == floatbuf && tiled_ok) {
      if (!tiled) {
        return *slot;
      }
      /* A precreated per-object map has tile 1001 only: grow it to the number of objects the bake
       * resolved instead of throwing the canvas away. */
      bool all_tiles = true;
      for (int index = 0; index < object_num; index++) {
        if (!BKE_paint_layers_map_coverage_tile_ensure(**slot, 1001 + index)) {
          all_tiles = false;
          break;
        }
      }
      if (all_tiles) {
        return *slot;
      }
    }
    /* The map was created at another size (the row's `bake_size` changed) or in another format (a
     * generated row's channel moved to float): the pixel writers index the render buffer by the
     * image's own width, so neither can be reused. The creation reference is the bake's alone, so
     * the old map goes with it. */
    Image *stale = *slot;
    *slot = nullptr;
    if (stale->id.us > 0) {
      id_us_min(&stale->id);
    }
    if (stale->id.us == 0 && ID_FAKE_USERS(&stale->id) == 0) {
      BKE_id_free(&bmain, &stale->id);
    }
  }
  char name[192];
  if (channel >= 0) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    SNPRINTF(name, "%s %s Bake", layer.name[0] != '\0' ? layer.name : "Layer", info.ui_name);
  }
  else {
    SNPRINTF(name, "%s Coverage Bake", layer.name[0] != '\0' ? layer.name : "Layer");
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  /* The creation reference is the one the row holds: unlike #BKE_paint_layers_bake_set_map this
   * takes no user of its own. */
  Image *image = BKE_image_add_generated(
      &bmain, size, size, name, 32, floatbuf, IMA_GENTYPE_BLANK, black, false, !is_color, tiled);
  if (image != nullptr) {
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    if (tiled) {
      /* The first tile is created by the image itself; the rest carry the same canvas and format,
       * because a UDIM image requires every tile to have the same resolution. */
      for (int index = 1; index < object_num; index++) {
        ImageTile *tile = BKE_image_add_tile(image, 1001 + index, nullptr);
        if (tile == nullptr) {
          continue;
        }
        tile->gen_x = size;
        tile->gen_y = size;
        tile->gen_type = IMA_GENTYPE_BLANK;
        tile->gen_flag |= eImage_GenFlag(floatbuf ? IMA_GEN_FLOAT : 0);
        tile->gen_depth = 32;
        copy_v4_v4(tile->gen_color, black);
      }
    }
    /* A bake is the stack's own read of a row, so the browsers hide it like every other map. */
    BKE_paint_layers_image_mark_internal(*image);
  }
  *slot = image;
  return image;
}

/**
 * Tell the image system that \a ibuf's pixels were rewritten behind its back. Without this a map
 * that already reached the GPU keeps showing its previous bake, since nothing else invalidates the
 * texture.
 */
static void bake_image_pixels_changed(Image &image, ImBuf &ibuf)
{
  ibuf.userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(&image, &ibuf);
  BKE_image_partial_update_mark_full_update(&image);
  BKE_image_free_gputextures(&image);
}

/**
 * The float map's storage of one straight-RGBA value: premultiplied by its own alpha, which is what
 * the composite and the renderer both read.
 */
static void bake_pixel_store_float(const float rgba[4], float r_store[4])
{
  const float alpha = clamp_f(rgba[3], 0.0f, 1.0f);
  r_store[0] = rgba[0] * alpha;
  r_store[1] = rgba[1] * alpha;
  r_store[2] = rgba[2] * alpha;
  r_store[3] = alpha;
}

/**
 * The byte map's storage of one straight-RGBA value: the colour encoded into the map's colorspace
 * and quantized, the alpha clamped. Only a colour channel is encoded; a data channel keeps the
 * scene-linear value quantized as it came.
 */
static void bake_pixel_store_byte(const float rgba[4],
                                  const bool is_color,
                                  const ColorSpace *colorspace,
                                  uchar r_store[4])
{
  float encoded[4] = {rgba[0], rgba[1], rgba[2], clamp_f(rgba[3], 0.0f, 1.0f)};
  if (is_color && colorspace != nullptr) {
    IMB_colormanagement_scene_linear_to_colorspace_v3(encoded, colorspace);
  }
  r_store[0] = uchar(clamp_i(int(encoded[0] * 255.0f + 0.5f), 0, 255));
  r_store[1] = uchar(clamp_i(int(encoded[1] * 255.0f + 0.5f), 0, 255));
  r_store[2] = uchar(clamp_i(int(encoded[2] * 255.0f + 0.5f), 0, 255));
  r_store[3] = uchar(clamp_i(int(encoded[3] * 255.0f + 0.5f), 0, 255));
}

/**
 * Write \a image, or only \a rect (x0, y0, x1, y1 in image pixels) when given, from a source that
 * fills one row at a time: \a fill_row is called per row with the row's \a y and the buffer's
 * width, and fills \a width straight RGBA floats into \a r_row. A row scratch is all the source
 * needs -- a map's width wide, nothing next to the map itself -- so a source that is sampled as it
 * is written never holds a second copy of the whole picture.
 *
 * The map's own buffer decides the format: a byte map stores the value encoded into its colorspace,
 * a float map stores it as it came, which is what a precise channel of a generated row is for.
 * Shared with the bake tests, which hold the worker's stored-buffer write to this same encoding.
 */
void bake_write_image_rows(
    Image &image,
    const bool is_color,
    const int *rect,
    const int tile_number,
    const FunctionRef<void(const int y, const int width, float *r_row)> fill_row)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile_number > 0 ? &iuser : nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  /* The buffer that exists decides, which is the rule `composite_decode_tile` reads a map by: a
   * byte buffer wins over a float one there, so writing the other here would leave the map showing
   * one picture to the renderer and another to the composite. */
  float *floats = (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr) ?
                      ibuf->float_data_for_write() :
                      nullptr;
  if (floats != nullptr) {
    /* A float buffer is premultiplied -- the composite and the renderer both read it that way -- so
     * the straight colour it is given is stored multiplied by the alpha that travels with it. */
    Vector<float> row(int64_t(width) * 4);
    for (int y = y0; y < y1; y++) {
      fill_row(y, width, row.data());
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        bake_pixel_store_float(&row[x * 4], &floats[i * 4]);
      }
    }
  }
  else if (uchar *pixels = ibuf->byte_data_for_write()) {
    const auto *colorspace = ibuf->byte_buffer.colorspace;
    Vector<float> row(int64_t(width) * 4);
    for (int y = y0; y < y1; y++) {
      fill_row(y, width, row.data());
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        bake_pixel_store_byte(&row[x * 4], is_color, colorspace, &pixels[i * 4]);
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/**
 * Write \a values into \a image, or only into \a rect (x0, y0, x1, y1 in image pixels) when given.
 *
 * The map's own buffer decides the format: a byte map stores the value encoded into its colorspace,
 * a float map stores it as it came, which is what a precise channel of a generated row is for.
 * Shared with the bake tests, which hold the worker's stored-buffer write to this same encoding.
 */
void bake_write_image(Image &image,
                      const float *values,
                      const bool is_color,
                      const int *rect,
                      const int tile_number /* = 0 */)
{
  bake_write_image_rows(image, is_color, rect, tile_number,
                        [&](const int y, const int width, float *r_row) {
    memcpy(r_row, values + int64_t(y) * width * 4, sizeof(float[4]) * size_t(width));
  });
}

/**
 * Write a stored buffer -- the per-pixel format #bake_pixel_store_float or #bake_pixel_store_byte
 * produce, which a worker job holds instead of its straight-RGBA render -- into \a image, or only
 * into \a rect. No conversion happens here: the values are already the map's own.
 */
static void bake_write_stored_image(Image &image,
                                    const bool stored_float,
                                    const void *stored,
                                    const int *rect,
                                    const int tile_number = 0)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile_number > 0 ? &iuser : nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  if (stored_float) {
    if (float *floats = (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr) ?
                            ibuf->float_data_for_write() :
                            nullptr)
    {
      const float *values = static_cast<const float *>(stored);
      for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
          const int64_t i = int64_t(y) * width + x;
          floats[i * 4 + 0] = values[i * 4 + 0];
          floats[i * 4 + 1] = values[i * 4 + 1];
          floats[i * 4 + 2] = values[i * 4 + 2];
          floats[i * 4 + 3] = values[i * 4 + 3];
        }
      }
    }
  }
  else if (uchar *pixels = ibuf->byte_data_for_write()) {
    const uchar *values = static_cast<const uchar *>(stored);
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        pixels[i * 4 + 0] = values[i * 4 + 0];
        pixels[i * 4 + 1] = values[i * 4 + 1];
        pixels[i * 4 + 2] = values[i * 4 + 2];
        pixels[i * 4 + 3] = values[i * 4 + 3];
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/** The coverage map keeps its grey in all three colour channels and is opaque. */
void bake_write_coverage_image(Image &image,
                               const float *coverage,
                               const int *rect,
                               const int tile_number /* = 0 */)
{
  ImageUser iuser{};
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, tile_number > 0 ? &iuser : nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  uchar *pixels = ibuf->byte_data_for_write();
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  if (pixels != nullptr) {
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        const uchar gray = uchar(
            clamp_i(int(clamp_f(coverage[i], 0.0f, 1.0f) * 255.0f + 0.5f), 0, 255));
        pixels[i * 4 + 0] = gray;
        pixels[i * 4 + 1] = gray;
        pixels[i * 4 + 2] = gray;
        pixels[i * 4 + 3] = 255;
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf);
  BKE_image_release_ibuf(&image, ibuf, lock);
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
        const int row_objects = std::max(1, row.bake->object_count);
        bytes += int64_t(row_objects) * bake_object_cache_bytes(ma, row, row.bake->size);
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
      bake_tile_object_index_remove(*object_mut, property->name, true);
    }
    property = next;
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
  }
}

/**
 * Remove the shader's tile index from the objects of \a scene and \a view_layer once no row of
 * \a ma has a tile left. The property is keyed on the material, so keeping it would leave an
 * orphaned Custom Property on objects the material no longer bakes for.
 */
static void bake_tile_object_indices_clear(const Material &ma,
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
  if (!paint_layers_bake_subtree_needs_surface(layer)) {
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
  r_targets.objects = view.objects;

  /* The budget is one per material, not one per row: every Object/Generated row of the material
   * holds one cache set per object, and the sum is what the file's caches can grow to. Existing
   * rows are counted at the size and object count they were baked at, the row being resolved at
   * the size and object count it is about to use. */
  if (bake_object_budget_bytes(ma, layer, size, int64_t(view.objects.size())) >
      PAINT_LAYERS_BAKE_OBJECTS_BUDGET)
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
static void bake_object_identity_record(const Material &ma,
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

bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer)
{
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* The worker job renders a row's subtree from a localized copy; a mask effect's grey is the
     * mask effect cache's compute, which only the main thread reaches. Calling it heavy would leave
     * it to a job that never picks it up, so after its first bake an edit would not bake it again. */
    return false;
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

/** Whether \a layer, or a folder it sits in, is switched off: a hidden row is never baked. */
static bool bake_row_is_hidden(const Material &ma, const MaterialPaintLayer &layer)
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
static bool bake_row_release_apply(Main &bmain,
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
static int bake_size_for(const Material &ma, const MaterialPaintLayer &row)
{
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
static int bake_row_object_issue(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 const PaintLayersBakeView &view)
{
  if (!paint_layers_bake_subtree_needs_surface(layer)) {
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
static void bake_object_identities_notice(Material &ma, const PaintLayersBakeView &view)
{
  if (view.issue == PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT || !BKE_paint_layers_is_layered(ma)) {
    return;
  }
  bool changed = false;
  BKE_paint_layers_foreach(
      ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
        if (layer.bake == nullptr || layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER ||
            !paint_layers_bake_subtree_needs_surface(layer))
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
          const uint64_t identity = paint_layers_bake_target_identity(target, layer.bake->size);
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
static bool bake_row_is_candidate(const Material &ma,
                                  const MaterialPaintLayer &layer,
                                  const int object_issue)
{
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* A reader owns its slice's cache; a substitution of the row it reads is blocked separately. */
    return paint_layer_anchor_point_reader_baked(ma, layer);
  }
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* A CPU-computed mask effect owns the grey it lays: the cache is what the live graph reads,
     * like a reader's, so the mode is its only gate (the due check asks the rest). */
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
    if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
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
               PAINT_LAYERS_BAKE_OBJECTS_OVER_BUDGET))
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
static void bake_gradient_row_report(const MaterialPaintLayer &layer,
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

/**
 * The surface an Object or Generated bake of \a layer renders through, or null when its subtree
 * reads no object geometry at all. \a targets is what the caller resolved; the islands are grown
 * outward so the map's bilinear filtering finds an edge rather than empty texels, and the orco grid
 * is kept only when a Generated generator reads it.
 */
static std::shared_ptr<const PaintMaterialCompositeSurface>
    bake_surface_build(const MaterialPaintLayer &layer,
                       const PaintLayersBakeTarget &target,
                       const int size)
{
  const int padding = std::clamp(size / 64, 1, 16);
  return BKE_paint_layers_gradient_surface_build(*target.object,
                                                *target.mesh,
                                                target.uv_map_name,
                                                size,
                                                padding,
                                                BKE_paint_layers_generator_subtree_holds_generated(
                                                    layer));
}

/** Whether any row of \a ma reads object geometry, so a view has to be resolved for the plan. */
static bool bake_any_row_needs_surface(const Material &ma)
{
  bool needs = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        needs = paint_layers_bake_subtree_needs_surface(layer);
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
    const int tile_number,
    const int *rect,
    const bool reuse_cache,
    Vector<int> &r_written_channels)
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
            ma, layer, channel, size, color.data(), coverage.data(), channel_rect, surface))
    {
      continue;
    }
    Image *color_image = bake_service_image(bmain,
                                            layer,
                                            channel,
                                            size,
                                            info.is_color,
                                            bake_channel_needs_float(layer, info),
                                            object_num);
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
          bmain, layer, -1, size, false, false, object_num))
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
                                    const ViewLayer *view_layer)
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
    bake_object_identities_notice(ma, view);
    Vector<MaterialPaintLayer *> rows;
    BKE_paint_layers_foreach(ma,
                             [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
                               rows.append(&layer);
                               for (MaterialPaintLayer &effect : layer.effects) {
                                 if (paint_layer_kind_reads_anchor_point(effect)) {
                                   rows.append(&effect);
                                 }
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
      if (heavy) {
        continue;
      }
      const int size = bake_size_for(ma, layer);
      if (size <= 0) {
        /* No map has known dimensions yet: nothing to allocate a bake structure for. */
        continue;
      }
      if (paint_layer_mask_item_bakes_effect(layer)) {
        /* The item's grey is one map of its own, written whole: a stroke into the mask defers it
         * (#BKE_paint_layers_bake_row_is_deferred), so no region path is wanted here. */
        if (bake_mask_effect_row_write(bmain, ma, layer, size)) {
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
      /* Build the first surface before the bake record exists: a row whose object cannot supply one
       * must not be left with an empty, permanently invalid cache. The remaining surfaces are built
       * one at a time in the loop below, after the previous one was written and released. */
      std::shared_ptr<const PaintMaterialCompositeSurface> first_surface;
      if (needs_surface) {
        first_surface = bake_surface_build(layer, targets.objects[0], size);
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
        std::shared_ptr<const PaintMaterialCompositeSurface> surface;
        int tile_number = 0;
        if (object_num > 0) {
          surface = (object_index == 0 && first_surface != nullptr) ?
                        first_surface :
                        bake_surface_build(layer, targets.objects[object_index], size);
          if (surface == nullptr) {
            all_objects = false;
            continue;
          }
          tile_number = 1001 + object_index;
        }
        Vector<int> tile_channels;
        if (!bake_object_tile_write(bmain,
                                    ma,
                                    layer,
                                    surface,
                                    size,
                                    object_num,
                                    tile_number,
                                    rect_ptr,
                                    reuse_cache,
                                    tile_channels))
        {
          all_objects = false;
          continue;
        }
        any_channel = true;
        for (const int channel : tile_channels) {
          if (!written_channels.contains(channel)) {
            written_channels.append(channel);
          }
        }
        if (object_num > 0) {
          const PaintLayersBakeTarget &target = targets.objects[object_index];
          MaterialPaintLayerBakeTile *tile = bake_tile_ensure(
              *bake, target.object_uid, object_index);
          const uint64_t identity = paint_layers_bake_target_identity(target, size);
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
            ma, layer, paint_layers_bake_target_identity(targets.objects[0], size));
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
        return !pending;
      });
  return pending;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material rows
 *
 * A Material row shows maps rendered from its source material (`material_bake_layered_rows_ensure`
 * on the editor side). They are the row's source the way a painted map is a painted row's: the
 * row's opacity, blend, mask and corrections stay live over them, which is why such a row is not
 * the cache of a subtree (#paint_layer_row_substituted) and its hash is only structural.
 *
 * Two questions, two readers. Whether the maps are the row's to show is the structural hash, a few
 * integers, answerable from an evaluated copy. Whether they still describe the source is the
 * source's content hash and the pixel subscription, which only the main thread may read; while
 * they do not, the old maps keep showing until the new ones land.
 * \{ */

bool paint_layer_row_material_shown(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || layer.material == nullptr ||
      layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER ||
      !ELEM(layer.role,
            MA_PAINT_LAYER_ROLE_LAYER,
            MA_PAINT_LAYER_ROLE_EFFECT,
            MA_PAINT_LAYER_ROLE_MASK_ITEM))
  {
    return false;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  return (hash[0] != 0 || hash[1] != 0) &&
         (hash[0] == layer.bake->hash[0] && hash[1] == layer.bake->hash[1]);
}

bool paint_layer_row_material_map(const Material &ma,
                                  const MaterialPaintLayer &layer,
                                  const int channel,
                                  Image **r_color)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      !paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) || layer.bake == nullptr ||
      layer.bake->images[channel] == nullptr || paint_layer_channel_filtered(ma, channel) ||
      !paint_layer_row_material_shown(ma, layer))
  {
    return false;
  }
  if (r_color != nullptr) {
    *r_color = layer.bake->images[channel];
  }
  return true;
}

static void bake_source_hash_split(const uint64_t hash, uint32_t r_words[2])
{
  r_words[0] = uint32_t(hash & 0xFFFFFFFFu);
  r_words[1] = uint32_t(hash >> 32);
}

/**
 * Whether the render of \a layer is partial: Auto, no Image Editor reading the maps, and the source
 * supplies everything but the Normal live. Then only the Normal is rendered, only what the Normal
 * reads can make the map stale, and an edit of a colour or a roughness renders nothing. The maps
 * of the channels shown live are left as they are until something reads them.
 */
static bool bake_row_maps_partial(const Material &ma, const MaterialPaintLayer &layer)
{
  if (BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_ALWAYS ||
      BKE_paint_layers_preview_wanted(ma))
  {
    return false;
  }
  bool normal_needs_map = false;
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!BKE_paint_layers_material_channel_needs_render(ma, layer, channel) ||
        paint_layer_material_source_group_channel(ma, layer, channel))
    {
      continue;
    }
    if (channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      return false;
    }
    normal_needs_map = true;
  }
  return normal_needs_map;
}

bool BKE_paint_layers_material_channel_render_wanted(const Material &ma,
                                                     const MaterialPaintLayer &layer,
                                                     const int channel)
{
  if (!BKE_paint_layers_material_channel_needs_render(ma, layer, channel)) {
    return false;
  }
  return !bake_row_maps_partial(ma, layer) ||
         !paint_layer_material_source_group_channel(ma, layer, channel);
}

bool BKE_paint_layers_material_row_stale(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || layer.material == nullptr) {
    return true;
  }
  const uint64_t full = BKE_paint_layers_source_material_tree_hash_cached(*layer.material);
  uint32_t current[2];
  bake_source_hash_split(full, current);
  if (current[0] == 0 && current[1] == 0) {
    return true;
  }
  /* The stamp always holds the full hash, whatever state the editor is in, so closing an Image
   * Editor does not make a row look edited. A full hash that moved is still no reason to render
   * when the maps are only of the Normal and what the Normal reads is what the last render saw. */
  const bool source_moved = current[0] != layer.bake->source_hash[0] ||
                            current[1] != layer.bake->source_hash[1];
  const bool partial = bake_row_maps_partial(ma, layer);
  const bool normal_only = source_moved && partial;
  const uint64_t normal = normal_only ?
                              BKE_paint_layers_source_normal_hash_cached(*layer.material) :
                              0;
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime != nullptr && !partial && runtime->partial_rows.contains(UUID(layer.marker))) {
    /* The last render left the maps of the live channels as they were, and something reads them now
     * (an Image Editor opened, the Always mode). */
    return true;
  }
  if (source_moved) {
    const uint64_t *rendered = (normal_only && runtime != nullptr) ?
                                   runtime->normal_hash.lookup_ptr(UUID(layer.marker)) :
                                   nullptr;
    if (rendered == nullptr || *rendered != normal) {
      return true;
    }
  }
  if (runtime != nullptr) {
    for (const BakeLayerSubscription &entry : runtime->layers) {
      if (BLI_uuid_equal(entry.marker, layer.marker) && entry.changed) {
        return true;
      }
    }
  }
  return false;
}

void BKE_paint_layers_material_bake_stamp_get(const Material &ma,
                                              const MaterialPaintLayer &layer,
                                              uint32_t r_hash[2],
                                              uint32_t r_source_hash[2])
{
  BKE_paint_layers_bake_hash(ma, layer, r_hash);
  const uint64_t full = layer.material != nullptr ?
                            BKE_paint_layers_source_material_tree_hash_cached(*layer.material) :
                            0;
  bake_source_hash_split(full, r_source_hash);
  if (full == 0) {
    return;
  }
  /* What the Normal reads at the start of the render, kept against the full hash it goes with:
   * the render that lands from this stamp makes it the one its maps reflect. */
  const uint64_t normal = BKE_paint_layers_source_normal_hash_cached(*layer.material);
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                              BakeMaterialRuntime{});
  runtime.owner = &ma;
  if (runtime.started.size() > 16) {
    runtime.started.clear();
  }
  runtime.started.add_overwrite(
      full, BakeMaterialRuntime::StartedRender{normal, bake_row_maps_partial(ma, layer)});
}

bool BKE_paint_layers_material_row_render_forced_wanted(const Material &ma,
                                                        const MaterialPaintLayer &row)
{
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) ||
      !ELEM(row.role,
            MA_PAINT_LAYER_ROLE_LAYER,
            MA_PAINT_LAYER_ROLE_EFFECT,
            MA_PAINT_LAYER_ROLE_MASK_ITEM) ||
      row.material == nullptr || row.material->nodetree == nullptr ||
      BKE_paint_layers_bake_mode_get(row) == MA_PAINT_LAYER_BAKE_NEVER ||
      !BKE_paint_layers_material_row_needs_render(ma, row))
  {
    return false;
  }
  if (!paint_layer_row_material_shown(ma, row) || BKE_paint_layers_material_row_stale(ma, row)) {
    return true;
  }
  /* A partial render (Auto, nothing reading the maps) is fresh and still holds the Normal alone. */
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!BKE_paint_layers_material_channel_needs_render(ma, row, channel)) {
      continue;
    }
    const Image *map = (channel == PAINT_MATERIAL_CHANNEL_ALPHA) ? row.bake->coverage :
                                                                   row.bake->images[channel];
    if (map == nullptr) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_material_row_due(const Material &ma, const MaterialPaintLayer &layer)
{
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ||
      !ELEM(layer.role,
            MA_PAINT_LAYER_ROLE_LAYER,
            MA_PAINT_LAYER_ROLE_EFFECT,
            MA_PAINT_LAYER_ROLE_MASK_ITEM) ||
      layer.material == nullptr || layer.material->nodetree == nullptr ||
      BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_NEVER)
  {
    return false;
  }
  /* A hidden row shows nothing, so its maps are rendered when it is shown. */
  if (bake_row_is_hidden(ma, layer)) {
    return false;
  }
  /* The active row is the one being edited. In Auto its graph is shown live in the shader, but the
   * Image Editor's composite reads only the rendered maps, so while one shows this material the row
   * is rendered as a preview; with none open there is nothing to keep current and the render waits
   * for the active marker to leave the row. Always has no live form (its maps are what the row
   * shows), so it renders on every edit. */
  const bool editing = BKE_paint_layers_bake_row_is_deferred(ma, layer);
  const bool always = BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_ALWAYS;
  if (editing && !always && !BKE_paint_layers_preview_wanted(ma) &&
      !paint_layer_material_row_needs_map(ma, layer))
  {
    return false;
  }
  /* Everything the source supplies is a value or a plain texture: there is nothing to render. */
  if (!BKE_paint_layers_material_row_needs_render(ma, layer)) {
    return false;
  }
  /* Maps of preview quality are current for an edited row as long as they are of the side the
   * material asks for now, and due to be replaced once it is left or that side changed. Maps of the
   * row's own size are never replaced by a preview before an edit asks for one. */
  const int preview_side = (layer.bake != nullptr) ? int(layer.bake->preview_side16) * 16 : 0;
  const int wanted_side = (editing && !always) ?
                              BKE_paint_layers_preview_size_get(
                                  ma, BKE_paint_layers_material_row_size(ma, layer)) :
                              0;
  const bool preview_grade_left = preview_side != 0 && preview_side != wanted_side;
  if (paint_layer_row_material_shown(ma, layer) && !BKE_paint_layers_material_row_stale(ma, layer) &&
      !preview_grade_left)
  {
    return false;
  }
  uint32_t hash[2];
  uint32_t source_hash[2];
  BKE_paint_layers_material_bake_stamp_get(ma, layer, hash, source_hash);
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime != nullptr) {
    if (const BakeFailure *failure = runtime->failed.lookup_ptr(UUID(layer.marker))) {
      if (failure->hash[0] == hash[0] && failure->hash[1] == hash[1] &&
          failure->source[0] == source_hash[0] && failure->source[1] == source_hash[1] &&
          failure->preview == (editing && !always))
      {
        return false;
      }
    }
  }
  return true;
}

bool BKE_paint_layers_material_row_preview_active(const Material &ma,
                                                  const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_bake_row_is_deferred(ma, layer) &&
         BKE_paint_layers_bake_mode_get(layer) != MA_PAINT_LAYER_BAKE_ALWAYS &&
         BKE_paint_layers_preview_wanted(ma);
}

int BKE_paint_layers_material_row_size(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake != nullptr && layer.bake->size > 0) {
    return layer.bake->size;
  }
  return ma.paint_layers_image_size > 0 ? int(ma.paint_layers_image_size) : 4096;
}

bool BKE_paint_layers_material_row_is_preview(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr && layer.bake->preview_side16 != 0;
}

int BKE_paint_layers_preview_size_get(const Material &ma, const int final_size)
{
  /* The side a preview is asked for, before it is capped. */
  int side = 0;
  switch (ma.paint_layers_preview_size) {
    case MA_PAINT_PREVIEW_SIZE_256:
      side = 256;
      break;
    case MA_PAINT_PREVIEW_SIZE_512:
      side = 512;
      break;
    case MA_PAINT_PREVIEW_SIZE_1024:
      side = 1024;
      break;
    case MA_PAINT_PREVIEW_SIZE_2048:
      side = 2048;
      break;
    case MA_PAINT_PREVIEW_SIZE_4096:
      side = 4096;
      break;
    default:
      side = std::max(final_size / 4, 256);
      break;
  }
  /* Never more than the row will be rendered at, nor than the size the material's layers are
   * created with: a larger preview would cost more than the render it stands in for. */
  if (ma.paint_layers_image_size > 0) {
    side = std::min(side, int(ma.paint_layers_image_size));
  }
  /* A multiple of 16, which is what #MaterialPaintLayerBake::preview_side16 can record. */
  side = std::clamp(side, 16, std::max(final_size, 16));
  return std::max(side - side % 16, 16);
}

float BKE_paint_layers_source_edit_delay(const Material &consumer, const Material &source)
{
  const bool delayed_preview = (consumer.paint_layers_preview_flag & MA_PAINT_PREVIEW_DELAYED) != 0;
  /* Only an edit made through the active row waits: the row being edited is the one a preview is
   * rendered for; any other consumer renders after the editor's short pause. A row with a channel
   * that only a render can show (a Bump's Normal) waits whatever the Sync setting says: dragging
   * a value that moves it would otherwise render at every pause of the hand. */
  bool active_reads_source = false;
  bool preview_wait = false;
  /* The Material corrections count as well: one that is being edited waits like a row does. */
  BKE_paint_layers_foreach_source_row(
      consumer, [&](const MaterialPaintLayer &row) {
        if (row.material != &source || !BKE_paint_layers_bake_row_is_deferred(consumer, row)) {
          return true;
        }
        if (delayed_preview && BKE_paint_layers_material_row_preview_active(consumer, row)) {
          preview_wait = true;
        }
        if (preview_wait || paint_layer_material_row_needs_map(consumer, row)) {
          active_reads_source = true;
        }
        return !active_reads_source;
      });
  if (!active_reads_source) {
    return 0.0f;
  }
  if (!preview_wait) {
    /* Only a map no live form stands in for is waited on: short, since nothing shows its edit until
     * it lands. */
    return MAP_ONLY_EDIT_DELAY_SECONDS;
  }
  switch (consumer.paint_layers_preview_delay) {
    case MA_PAINT_PREVIEW_DELAY_10S:
      return 10.0f;
    case MA_PAINT_PREVIEW_DELAY_15S:
      return 15.0f;
    case MA_PAINT_PREVIEW_DELAY_25S:
      return 25.0f;
    default:
      return 5.0f;
  }
}

bool BKE_paint_layers_preview_wanted(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_PREVIEW_WANTED) != 0;
}

void BKE_paint_layers_preview_wanted_set(Material &ma, const bool wanted)
{
  if (wanted) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_PREVIEW_WANTED;
  }
  else {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_PREVIEW_WANTED;
  }
}

void BKE_paint_layers_material_row_log(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const char *event)
{
  /* Why a render of a Material row starts: which of the three answers made it due. */
  uint32_t hash[2];
  uint32_t source_hash[2];
  BKE_paint_layers_material_bake_stamp_get(ma, layer, hash, source_hash);
  bool changed = false;
  {
    std::lock_guard lock(bake_runtime_mutex());
    if (const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid)) {
      for (const BakeLayerSubscription &entry : runtime->layers) {
        if (BLI_uuid_equal(entry.marker, layer.marker)) {
          changed = changed || entry.changed;
        }
      }
    }
  }
  const MaterialPaintLayerBake *bake = layer.bake;
  printf(
      "[paint-layers material row] %s: row '%s' shown=%d needs_render=%d struct_hash=%08x%08x "
      "stored_struct=%08x%08x source_hash=%08x%08x stored_source=%08x%08x pixels_changed=%d\n",
      event,
      layer.name,
      int(paint_layer_row_material_shown(ma, layer)),
      int(BKE_paint_layers_material_row_needs_render(ma, layer)),
      hash[1],
      hash[0],
      bake != nullptr ? bake->hash[1] : 0u,
      bake != nullptr ? bake->hash[0] : 0u,
      source_hash[1],
      source_hash[0],
      bake != nullptr ? bake->source_hash[1] : 0u,
      bake != nullptr ? bake->source_hash[0] : 0u,
      int(changed));
  if (layer.material == nullptr || layer.material->nodetree == nullptr) {
    printf("[paint-layers material row]   source has no node tree\n");
    return;
  }
  const std::shared_ptr<const MaterialSourceResolve> resolve = BKE_paint_layers_source_resolve(
      *layer.material);
  printf("[paint-layers material row]   mode=%d group_wrappable=%d\n",
         int(BKE_paint_layers_bake_mode_get(layer)),
         int(BKE_paint_layers_source_group_wrappable(*layer.material)));
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, layer, channel);
    printf(
        "[paint-layers material row]   channel %d: resolved=%d group_ok=%d shown_kind=%d "
        "needs_render=%d\n",
        channel,
        int(resolve->channels[channel]),
        int(paint_layer_material_source_group_channel(ma, layer, channel)),
        int(source.kind),
        int(BKE_paint_layers_material_channel_needs_render(ma, layer, channel)));
  }
}

bool BKE_paint_layers_material_sources_notice(Material &ma)
{
  Vector<std::pair<bUUID, uint64_t>> signatures;
  BKE_paint_layers_foreach_source_row(
      static_cast<const Material &>(ma), [&](const MaterialPaintLayer &row) {
        signatures.append({row.marker, paint_layer_material_live_signature(ma, row)});
        return true;
      });
  if (signatures.is_empty()) {
    return false;
  }
  bool changed = false;
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                                BakeMaterialRuntime{});
    runtime.owner = &ma;
    for (const auto &[marker, signature] : signatures) {
      uint64_t *previous = runtime.live_signature.lookup_ptr(UUID(marker));
      if (previous == nullptr) {
        /* A row seen for the first time was built with the state it has: nothing moved. */
        runtime.live_signature.add(UUID(marker), signature);
      }
      else if (*previous != signature) {
        *previous = signature;
        changed = true;
      }
    }
  }
  if (changed) {
    /* The shape of a row's group follows its source: a value that became a texture is other nodes.
     */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  return changed;
}

void BKE_paint_layers_material_bake_retry(Material &ma, MaterialPaintLayer &layer)
{
  {
    std::lock_guard lock(bake_runtime_mutex());
    if (BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid)) {
      runtime->failed.remove(UUID(layer.marker));
    }
  }
  if (layer.bake != nullptr) {
    layer.bake->source_hash[0] = 0;
    layer.bake->source_hash[1] = 0;
  }
}

void BKE_paint_layers_source_material_consumers(Main &bmain,
                                                const Material &source,
                                                Vector<Material *> &r_consumers)
{
  const int copy_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & copy_tags) != 0 || !BKE_paint_layers_is_layered(ma)) {
      continue;
    }
    bool reads = false;
    BKE_paint_layers_foreach_source_row(static_cast<const Material &>(ma),
                                        [&](const MaterialPaintLayer &row) {
                                          reads = row.material == &source;
                                          return !reads;
                                        });
    if (reads) {
      r_consumers.append(&ma);
    }
  }
}

bool BKE_paint_layers_bake_outstanding(const Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  if (BKE_paint_layers_bake_heavy_pending(ma)) {
    return true;
  }
  bool due = false;
  BKE_paint_layers_foreach_source_row(ma, [&](const MaterialPaintLayer &row) {
    due = BKE_paint_layers_material_row_due(ma, row);
    return !due;
  });
  return due;
}

bool BKE_paint_layers_is_stale(const Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  /* The editor's mark covers whatever it has queued or running that has not resolved into a flag
   * or a pending render yet. */
  return BKE_paint_layers_bake_stale_get(ma) || BKE_paint_layers_bake_scheduled_get(ma) ||
         BKE_paint_layers_bake_outstanding(ma);
}

PaintLayersBakeGateAction BKE_paint_layers_bake_gate_decide(const bool stale,
                                                            const bool is_invoke,
                                                            const bool headless)
{
  /* Headless first: nothing there ever makes a stale result fresh by itself. */
  if (headless || !stale) {
    return PaintLayersBakeGateAction::Proceed;
  }
  return is_invoke ? PaintLayersBakeGateAction::Wait : PaintLayersBakeGateAction::Refuse;
}

void BKE_paint_layers_material_bake_apply(Main & /*bmain*/,
                                          Material &ma,
                                          MaterialPaintLayer &layer,
                                          const int size,
                                          const Span<int> channels,
                                          const Span<Image *> images)
{
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) || size <= 0 ||
      channels.size() != images.size() || !bake_row_owned(ma, layer))
  {
    return;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  bake->size = size;
  Image *alpha = nullptr;
  Vector<bool> kept(PAINT_MATERIAL_CHANNEL_NUM, false);
  for (const int64_t i : channels.index_range()) {
    if (channels[i] == PAINT_MATERIAL_CHANNEL_ALPHA) {
      /* The source's transparency is the row's coverage, not a channel of its own. */
      alpha = images[i];
      continue;
    }
    if (channels[i] >= 0 && channels[i] < PAINT_MATERIAL_CHANNEL_NUM) {
      BKE_paint_layers_bake_set_map(ma, layer, channels[i], images[i]);
      kept[channels[i]] = true;
    }
  }
  /* Every channel the render did not hand a map for is one the source no longer feeds. */
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!kept[channel] && bake->images[channel] != nullptr) {
      BKE_paint_layers_bake_set_map(ma, layer, channel, nullptr);
    }
  }
  /* A coverage map only exists while the source's transparency is rendered: when it is a value or
   * a plain texture, or the source has none, it is shown as it is and the old map goes. */
  BKE_paint_layers_bake_set_map(ma, layer, -1, alpha);
  /* The structural hash is stamped now so the generator wires the new maps in while they render;
   * the source hash only when they land. The subscription starts here too, so a pixel edit of the
   * source made during the render is seen after it. */
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
}

bool BKE_paint_layers_material_bake_land(Material &ma,
                                         const bUUID &marker,
                                         const uint32_t hash[2],
                                         const uint32_t source_hash[2],
                                         const bool landed,
                                         const int preview_side)
{
  MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  if (layer == nullptr || layer->bake == nullptr ||
      !paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_READS_SOURCE))
  {
    return false;
  }
  if (!landed) {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                                BakeMaterialRuntime{});
    runtime.owner = &ma;
    BakeFailure failure;
    failure.hash[0] = hash[0];
    failure.hash[1] = hash[1];
    failure.source[0] = source_hash[0];
    failure.source[1] = source_hash[1];
    failure.preview = preview_side != 0;
    runtime.failed.add_overwrite(UUID(marker), failure);
    return false;
  }
  uint32_t now[2];
  BKE_paint_layers_bake_hash(ma, *layer, now);
  if (now[0] != hash[0] || now[1] != hash[1]) {
    /* The row was pointed elsewhere or resized meanwhile: these pixels are not its own. */
    return false;
  }
  /* What the pixels reflect is what the render started from. A source that moved on since is
   * stale against it, and the next scene update renders it again. */
  layer->bake->source_hash[0] = source_hash[0];
  layer->bake->source_hash[1] = source_hash[1];
  layer->bake->preview_side16 = int16_t(std::clamp(preview_side / 16, 0, 512));
  {
    std::lock_guard lock(bake_runtime_mutex());
    if (BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid)) {
      runtime->failed.remove(UUID(marker));
      const uint64_t full = (uint64_t(source_hash[1]) << 32) | uint64_t(source_hash[0]);
      if (const BakeMaterialRuntime::StartedRender *started = runtime->started.lookup_ptr(full)) {
        runtime->normal_hash.add_overwrite(UUID(marker), started->normal_hash);
        if (started->partial) {
          runtime->partial_rows.add(UUID(marker));
        }
        else {
          runtime->partial_rows.remove(UUID(marker));
        }
      }
      else {
        runtime->normal_hash.remove(UUID(marker));
        runtime->partial_rows.remove(UUID(marker));
      }
    }
  }
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Worker job
 *
 * A heavy row is rendered off the main thread from a localized copy of the description and written
 * back on the main thread, so the window keeps redrawing while it bakes.
 * \{ */

struct PaintLayersBakeJob {
  Main *bmain = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
  /**
   * The scene and view layer whose objects an Object or Generated row is baked for. Main thread
   * data, re-read at the commit to check the objects are still the ones the worker rendered; null
   * for a material none of whose rows reads object geometry.
   */
  const Scene *scene = nullptr;
  const ViewLayer *view_layer = nullptr;
  /** Localized on the main thread at creation so the worker never reads a material being edited. */
  Material *material_copy = nullptr;

  struct ChannelResult {
    int channel = 0;
    bool is_color = false;
    /**
     * The render stored in its map's own format, not the straight-RGBA render: #color holds a float
     * map's premultiplied texels and #color_bytes a byte map's quantized ones. This is
     * #bake_write_image's transformation, done on the worker right after the render -- at 4K the
     * float4 render is 256 MiB where the byte map's storage is 64, and the job holds every result
     * until the commit.
     */
    bool stored_float = false;
    Vector<float> color;
    Vector<uchar> color_bytes;
  };
  struct ObjectResult {
    /** The object of this tile, or zero for a shared (UV) row's single map. */
    uint32_t object_uid = 0;
    uint32_t mesh_uid = 0;
    uint64_t identity = 0;
    /** UDIM tile number, 0 for a non-tiled map. */
    int tile_number = 0;
    /**
     * The surface of this object, rasterized on the main thread at creation and read-only on the
     * worker; null for a row reading no object geometry.
     */
    std::shared_ptr<const PaintMaterialCompositeSurface> surface;
    Vector<ChannelResult> channels;
    /**
     * The row's common factor (`mask x opacity`), folded from every channel's render as it lands on
     * the worker. The commit writes it as the coverage map, so the per-channel coverage need not be
     * held: at 4K that is 64 MiB a channel the job would otherwise keep until the commit.
     */
    Vector<float> common;
  };
  struct RowResult {
    bUUID marker = {};
    int size = 0;
    /**
     * The bake hash of the row when the job was created. The commit stamps this and not the live
     * one: an edit made while the worker ran would otherwise label a render of the old state as
     * current. With the creation hash the row is simply invalid again and the planner queues it
     * anew.
     */
    uint32_t hash[2] = {};
    /** One entry per object tile; a shared (UV) row has one entry with a null surface. */
    Vector<ObjectResult> objects;
  };
  Vector<RowResult> rows;
  /** The worker stopped early; nothing it produced is written. */
  bool cancelled = false;
};

/**
 * Fold one channel's render into \a common, the channel-independent row factor: `common =
 * mask x opacity`, recovered from the per-channel renders without a new map. The channel rendered
 * `coverage = common x content` with its own `content` in the colour alpha. Where that content is
 * present the quotient is the common factor; the maximum over channels does not depend on channel
 * order, and a texel no channel covers keeps zero.
 *
 * A row baked for its content alone rendered no mask and no opacity, so its quotient is the
 * content's own coverage: what the substitution reads back as the row's mask is then the content,
 * and the row's mask and opacity are laid live over it.
 */
static void bake_common_from_channel(Span<float> color,
                                     Span<float> coverage,
                                     MutableSpan<float> common)
{
  for (const int64_t i : common.index_range()) {
    const float content = color[i * 4 + 3];
    if (content > 0.002f) {
      common[i] = std::max(common[i], clamp_f(coverage[i] / content, 0.0f, 1.0f));
    }
  }
}

PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain,
                                                     Material &ma,
                                                     const Scene *scene,
                                                     const ViewLayer *view_layer)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return nullptr;
  }
  /* The objects the view layer produced are what an Object or Generated row is rendered from, and
   * every row of the material shares them: resolve the list once and notice every edited surface
   * against it. */
  PaintLayersBakeView view;
  if (scene == nullptr || view_layer == nullptr) {
    view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  }
  else if (bake_any_row_needs_surface(ma)) {
    paint_layers_bake_view_resolve(ma, &bmain, scene, view_layer, view);
  }
  bake_object_identities_notice(ma, view);
  Vector<MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
    rows.append(&layer);
    for (MaterialPaintLayer &effect : layer.effects) {
      if (paint_layer_kind_reads_anchor_point(effect)) {
        rows.append(&effect);
      }
    }
    for (MaterialPaintLayer &item : layer.mask_stack) {
      if (paint_layer_kind_reads_anchor_point(item)) {
        rows.append(&item);
      }
    }
    return true;
  });

  PaintLayersBakeJob *job = MEM_new<PaintLayersBakeJob>(__func__);
  job->bmain = &bmain;
  job->material = &ma;
  job->material_session_uid = ma.id.session_uid;

  /* Children before their parents: the job renders the queued rows of the copy in this order, and a
   * parent's render composites its children. */
  for (int index = int(rows.size()) - 1; index >= 0; index--) {
    MaterialPaintLayer &layer = *rows[index];
    BakeRowRelease release = BakeRowRelease::Keep;
    if (!bake_row_is_due(ma, layer, view, release)) {
      /* The row can no longer be served by its maps (no UV, too many objects, an animated
       * deformation...): this is the pass that has the view layer to judge it by. */
      bake_row_release_apply(bmain, ma, layer, release);
      continue;
    }
    if (!BKE_paint_layers_bake_is_heavy(ma, layer)) {
      continue;
    }
    const int size = bake_size_for(ma, layer);
    if (size <= 0) {
      continue;
    }

    /* An Object or Generated row is rendered through the surface of each object it serves, and the
     * surfaces are rasterized here: the worker reads the grids and never touches the depsgraph. */
    PaintLayersBakeTargets targets;
    const bool needs_surface = paint_layers_bake_subtree_needs_surface(layer);
    if (needs_surface) {
      paint_layers_bake_targets_resolve(ma, layer, view, size, targets);
      if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK || targets.objects.is_empty()) {
        continue;
      }
    }

    /* The surfaces are built before the bake record: a row whose objects cannot supply them must
     * not be left with an empty, permanently invalid cache. The job needs them all, so they stay
     * here; only the synchronous planner writes and releases one at a time. */
    PaintLayersBakeJob::RowResult row;
    row.marker = layer.marker;
    row.size = size;
    bool surfaces_ok = true;
    if (needs_surface) {
      for (int object_index = 0; object_index < targets.objects.size(); object_index++) {
        const PaintLayersBakeTarget &target = targets.objects[object_index];
        std::shared_ptr<const PaintMaterialCompositeSurface> surface = bake_surface_build(
            layer, target, size);
        if (surface == nullptr) {
          surfaces_ok = false;
          break;
        }
        PaintLayersBakeJob::ObjectResult object;
        object.object_uid = target.object_uid;
        object.mesh_uid = target.mesh_uid;
        object.identity = paint_layers_bake_target_identity(target, size);
        object.tile_number = 1001 + object_index;
        object.surface = std::move(surface);
        row.objects.append(std::move(object));
      }
    }
    else {
      PaintLayersBakeJob::ObjectResult object;
      row.objects.append(std::move(object));
    }
    if (!surfaces_ok) {
      continue;
    }

    /* Every gate has passed and this row is queued right now: only here is it safe to allocate the
     * bake structure. */
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
    bake->size = size;
    if (needs_surface) {
      paint_layers_bake_tiles_clear(*bake);
      bake_tile_object_indices_clear(ma, scene, view_layer, &view);
      /* The shader reads the tile index from each object; write it before the worker renders. */
      bake_tile_object_indices_set(ma, targets);
    }
    BKE_paint_layers_bake_hash(ma, layer, row.hash);
    job->rows.append(std::move(row));
  }

  if (job->rows.is_empty()) {
    MEM_delete(job);
    return nullptr;
  }
  job->scene = scene;
  job->view_layer = view_layer;
  job->material_copy = id_cast<Material *>(BKE_id_copy_ex(
      nullptr, &ma.id, nullptr, LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  if (job->material_copy == nullptr) {
    MEM_delete(job);
    return nullptr;
  }
  return job;
}

void BKE_paint_layers_bake_job_compute(PaintLayersBakeJob &job,
                                       const FunctionRef<void(float progress)> report_progress,
                                       const FunctionRef<bool()> is_cancelled)
{
  if (job.material_copy == nullptr) {
    return;
  }
  Material &ma = *job.material_copy;
  /* Every object tile is weighted by the same fixed channel count, whether or not each channel
   * actually renders, so progress only ever needs the two totals below. */
  int64_t total_pairs = 0;
  for (const PaintLayersBakeJob::RowResult &row : job.rows) {
    total_pairs += int64_t(row.objects.size()) * BKE_paint_material_channels().size();
  }
  int64_t done_pairs = 0;
  const auto report = [&]() {
    if (report_progress) {
      report_progress(total_pairs > 0 ? float(done_pairs) / float(total_pairs) : 1.0f);
    }
  };
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(ma, row.marker);
    if (layer == nullptr) {
      done_pairs += int64_t(row.objects.size()) * BKE_paint_material_channels().size();
      report();
      continue;
    }
    const bool gradient_row = paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_GENERATES);
    const double gradient_start = gradient_row ? BLI_time_now_seconds() : 0.0;
    const size_t row_memory_start = gradient_row ? MEM_get_memory_in_use() : 0;
    Vector<int> gradient_channels;
    for (PaintLayersBakeJob::ObjectResult &object : row.objects) {
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        /* The render of one channel is not interruptible; checking between channels keeps the
         * cancel button responsive to within one channel's render. */
        if (is_cancelled && is_cancelled()) {
          /* A cancelled job writes nothing, and what it holds is hundreds of MiB at 4K: the results
           * and the surfaces they were rendered from go now, not at the free. The commit reads no
           * rows once this mark is set. */
          job.cancelled = true;
          job.rows = Vector<PaintLayersBakeJob::RowResult>();
          return;
        }
        const int channel = int(info.channel);
        Vector<float> color(int64_t(row.size) * row.size * 4);
        Vector<float> coverage(int64_t(row.size) * row.size);
        const bool rendered = BKE_paint_layers_bake_render_node(
            ma, *layer, channel, row.size, color.data(), coverage.data(), nullptr, object.surface);
        done_pairs++;
        report();
        if (!rendered) {
          continue;
        }
        const int64_t pixel_num = int64_t(row.size) * row.size;
        /* The common factor folds in from the straight render, before the pixels take the map's
         * own format: a byte one quantizes the alpha the quotient divides by. The per-channel
         * coverage is not held: the fold-in is the only thing the commit needs it for. */
        if (object.common.is_empty()) {
          object.common = Vector<float>(pixel_num, 0.0f);
        }
        bake_common_from_channel(color, coverage, object.common);
        PaintLayersBakeJob::ChannelResult result;
        result.channel = channel;
        result.is_color = info.is_color;
        result.stored_float = bake_channel_needs_float(*layer, info);
        if (result.stored_float) {
          result.color = Vector<float>(pixel_num * 4);
          for (const int64_t i : IndexRange(pixel_num)) {
            bake_pixel_store_float(&color[i * 4], &result.color[i * 4]);
          }
        }
        else {
          /* The colorspace a byte map's buffer carries, from the same roles the commit's
           * #bake_service_image gives one (see #add_ibuf_for_tile): a colour channel is encoded
           * into the default byte colorspace, a data channel is quantized as it came. Only a
           * colour channel is encoded here too. */
          const ColorSpace *byte_colorspace =
              info.is_color ?
                  IMB_colormanagement_space_get_named(IMB_colormanagement_role_colorspace_name_get(
                      COLOR_ROLE_DEFAULT_BYTE)) :
                  nullptr;
          result.color_bytes = Vector<uchar>(pixel_num * 4);
          for (const int64_t i : IndexRange(pixel_num)) {
            bake_pixel_store_byte(
                &color[i * 4], info.is_color, byte_colorspace, &result.color_bytes[i * 4]);
          }
        }
        object.channels.append(std::move(result));
        if (!gradient_channels.contains(channel)) {
          gradient_channels.append(channel);
        }
      }
    }
    if (gradient_row) {
      const double delta_mib = double(int64_t(MEM_get_memory_in_use()) -
                                      int64_t(row_memory_start)) /
                               (1024.0 * 1024.0);
      int retained_results = 0;
      for (const PaintLayersBakeJob::RowResult &held : job.rows) {
        for (const PaintLayersBakeJob::ObjectResult &held_object : held.objects) {
          retained_results += held_object.channels.size();
        }
      }
      const double retained_mib = double(BKE_paint_layers_bake_job_retained_bytes(job)) /
                                  (1024.0 * 1024.0);
      bake_gradient_row_report(*layer,
                               row.size,
                               false,
                               BLI_time_now_seconds() - gradient_start,
                               gradient_channels,
                               delta_mib,
                               retained_results,
                               retained_mib);
    }
  }
}

/**
 * The scene a job was started for, or null when it (or its view layer) left \a bmain while the
 * worker ran: the pointers the job kept are then not to be touched.
 */
static const Scene *bake_job_scene_get(Main &bmain,
                                       const Scene *scene,
                                       const ViewLayer *view_layer)
{
  if (scene == nullptr || view_layer == nullptr) {
    return nullptr;
  }
  for (Scene &candidate : bmain.scenes) {
    if (&candidate != scene) {
      continue;
    }
    for (ViewLayer &layer : candidate.view_layers) {
      if (&layer == view_layer) {
        return &candidate;
      }
    }
    break;
  }
  return nullptr;
}

bool BKE_paint_layers_bake_job_commit(PaintLayersBakeJob &job)
{
  if (job.bmain == nullptr) {
    return false;
  }
  Main &bmain = *job.bmain;
  Material *ma = nullptr;
  for (Material &candidate : bmain.materials) {
    if (candidate.id.session_uid == job.material_session_uid && &candidate == job.material) {
      ma = &candidate;
      break;
    }
  }
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  if (job.cancelled) {
    /* Without this mark the rows left stale would start the same job again on the next update. An
     * edit clears it (see #BKE_paint_layers_tag_edited). */
    ma->paint_layers_flag |= MA_PAINT_LAYERS_BAKE_CANCELLED;
    return false;
  }
  bool changed = false;
  const Scene *scene = bake_job_scene_get(bmain, job.scene, job.view_layer);
  PaintLayersBakeView view;
  if (scene == nullptr || job.view_layer == nullptr) {
    view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  }
  else if (bake_any_row_needs_surface(*ma)) {
    paint_layers_bake_view_resolve(*ma, &bmain, scene, job.view_layer, view);
  }
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, row.marker);
    /* The row was removed, its bake dropped or switched off while the worker ran: nothing to
     * write. */
    if (layer == nullptr || layer->bake == nullptr || layer->bake->mode == MA_PAINT_LAYER_BAKE_NEVER)
    {
      continue;
    }
    uint32_t current_hash[2];
    BKE_paint_layers_bake_hash(*ma, *layer, current_hash);
    if (current_hash[0] != row.hash[0] || current_hash[1] != row.hash[1]) {
      /* The row changed while the worker ran: the pixels describe the old description, which the
       * stored hash would label as current. Dropping them leaves the row due for a new render. */
      continue;
    }
    const bool tiled_row = !row.objects.is_empty() && row.objects[0].surface != nullptr;
    PaintLayersBakeTargets targets;
    if (tiled_row) {
      /* The objects or their geometry may have moved while the worker ran: the pixels then describe
       * surfaces the view layer no longer has, so they are dropped and the row re-bakes. */
      paint_layers_bake_targets_resolve(*ma, *layer, view, row.size, targets);
      if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK ||
          targets.objects.size() != row.objects.size())
      {
        continue;
      }
      bool matches = true;
      for (int index = 0; index < row.objects.size(); index++) {
        const PaintLayersBakeJob::ObjectResult &object = row.objects[index];
        const PaintLayersBakeTarget &target = targets.objects[index];
        if (target.object_uid != object.object_uid || target.mesh_uid != object.mesh_uid ||
            paint_layers_bake_target_identity(target, row.size) != object.identity)
        {
          matches = false;
          break;
        }
      }
      if (!matches) {
        continue;
      }
      paint_layers_bake_tiles_clear(*layer->bake);
      bake_tile_object_indices_clear(*ma, scene, job.view_layer, &view);
      bake_tile_object_indices_set(*ma, targets);
    }
    const int object_num = tiled_row ? int(row.objects.size()) : 0;
    bool row_written = false;
    bool all_objects = true;
    Vector<int> written_channels;
    for (int object_index = 0; object_index < row.objects.size(); object_index++) {
      PaintLayersBakeJob::ObjectResult &object = row.objects[object_index];
      bool object_written = false;
      for (PaintLayersBakeJob::ChannelResult &channel : object.channels) {
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(channel.channel));
        Image *color_image = bake_service_image(bmain,
                                               *layer,
                                               channel.channel,
                                               row.size,
                                               channel.is_color,
                                               bake_channel_needs_float(*layer, info),
                                               object_num);
        if (color_image == nullptr) {
          continue;
        }
        /* The stored buffer is the map's own format, so this write converts nothing: it puts down
         * the render the worker held, not its float4. The buffer is freed here and not at the job's
         * teardown, so the held memory falls as the maps are written. */
        bake_write_stored_image(
            *color_image,
            channel.stored_float,
            channel.stored_float ? static_cast<const void *>(channel.color.data()) :
                                   static_cast<const void *>(channel.color_bytes.data()),
            nullptr,
            object.tile_number);
        channel.color = Vector<float>();
        channel.color_bytes = Vector<uchar>();
        if (!written_channels.contains(channel.channel)) {
          written_channels.append(channel.channel);
        }
        object_written = true;
      }
      if (!object_written) {
        all_objects = false;
        continue;
      }
      row_written = true;
      {
        /* The common factor was folded on the worker as each channel rendered; the per-channel
         * coverages it came from are gone, which is the point. */
        if (Image *coverage_image = bake_service_image(
                bmain, *layer, -1, row.size, false, false, object_num))
        {
          bake_write_coverage_image(
              *coverage_image, object.common.data(), nullptr, object.tile_number);
        }
        object.common = Vector<float>();
      }
      if (tiled_row) {
        MaterialPaintLayerBakeTile *tile = bake_tile_ensure(
            *layer->bake, object.object_uid, object_index);
        tile->hash[0] = uint32_t(object.identity & 0xFFFFFFFFu);
        tile->hash[1] = uint32_t(object.identity >> 32);
      }
    }
    if (!row_written || !all_objects) {
      continue;
    }
    if (tiled_row) {
      layer->bake->object_count = int(row.objects.size());
    }
    bake_stale_channel_maps_release(bmain, *layer, written_channels);
    layer->bake->hash[0] = row.hash[0];
    layer->bake->hash[1] = row.hash[1];
    if (tiled_row) {
      /* The maps are the surfaces': the next plan compares those identities to notice an edit of
       * the geometry or of the UV layer. */
      bake_object_identity_record(*ma, *layer, row.objects[0].identity);
    }
    BKE_paint_layers_bake_subscribe(*ma, *layer);
    changed = true;
  }
  if (changed) {
    ma->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    /* New bake maps were written into the layers outside any operator. Tagging the material marks
     * it changed for the next undo push; otherwise a memfile undo reuses it as it is and frees the
     * maps, leaving the layers' bake slots dangling. */
    DEG_id_tag_update(&ma->id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  }
  /* The queue is drained once no baked row is invalid any more, as the synchronous planner decides
   * it. */
  bool pending = false;
  BKE_paint_layers_foreach(
      *ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        pending = layer.bake != nullptr && layer.bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
                  bake_row_is_candidate(*ma, layer, bake_row_object_issue(*ma, layer, view)) &&
                  !BKE_paint_layers_bake_is_valid(*ma, layer) && !bake_row_is_hidden(*ma, layer);
        return !pending;
      });
  if (!pending) {
    ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
  }
  return changed;
}

int64_t BKE_paint_layers_bake_job_retained_bytes(const PaintLayersBakeJob &job)
{
  int64_t bytes = 0;
  for (const PaintLayersBakeJob::RowResult &row : job.rows) {
    for (const PaintLayersBakeJob::ObjectResult &object : row.objects) {
      for (const PaintLayersBakeJob::ChannelResult &channel : object.channels) {
        bytes += channel.color.size() * int64_t(sizeof(float)) +
                 channel.color_bytes.size() * int64_t(sizeof(uchar));
      }
      bytes += object.common.size() * int64_t(sizeof(float));
    }
  }
  return bytes;
}

void BKE_paint_layers_bake_job_free(PaintLayersBakeJob &job)
{
  if (job.material_copy != nullptr) {
    BKE_id_free(nullptr, &job.material_copy->id);
    job.material_copy = nullptr;
  }
  MEM_delete(&job);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Row Result Job
 * \{ */

struct PaintLayersRowResultJob {
  Main *bmain = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
  /** Localized on the main thread at creation so the worker never reads a material being edited.
   */
  Material *material_copy = nullptr;
  bUUID source_marker = {};
  bUUID target_marker = {};
  int size = 0;
  struct ChannelResult {
    int channel = 0;
    bool is_color = false;
    Vector<float> color;
  };
  Vector<ChannelResult> channels;
};

PaintLayersRowResultJob *BKE_paint_layers_row_result_job_create(Main &bmain,
                                                                Material &ma,
                                                                const bUUID &source_marker,
                                                                const bUUID &target_marker,
                                                                const int size)
{
  if (!BKE_paint_layers_is_layered(ma) || size <= 0 ||
      BKE_paint_layers_find(ma, source_marker) == nullptr ||
      BKE_paint_layers_find(ma, target_marker) == nullptr)
  {
    return nullptr;
  }
  PaintLayersRowResultJob *job = MEM_new<PaintLayersRowResultJob>(__func__);
  job->bmain = &bmain;
  job->material = &ma;
  job->material_session_uid = ma.id.session_uid;
  job->source_marker = source_marker;
  job->target_marker = target_marker;
  job->size = size;
  job->material_copy = id_cast<Material *>(
      BKE_id_copy_ex(nullptr,
                     &ma.id,
                     nullptr,
                     LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  if (job->material_copy == nullptr) {
    MEM_delete(job);
    return nullptr;
  }
  return job;
}

void BKE_paint_layers_row_result_job_compute(PaintLayersRowResultJob &job)
{
  if (job.material_copy == nullptr) {
    return;
  }
  Material &ma = *job.material_copy;
  const MaterialPaintLayer *source = BKE_paint_layers_find(ma, job.source_marker);
  if (source == nullptr) {
    return;
  }
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const int channel = int(info.channel);
    Vector<float> color(int64_t(job.size) * job.size * 4);
    Vector<float> coverage(int64_t(job.size) * job.size);
    if (!BKE_paint_layers_bake_render_node(
            ma, *source, channel, job.size, color.data(), coverage.data()))
    {
      continue;
    }
    PaintLayersRowResultJob::ChannelResult result;
    result.channel = channel;
    result.is_color = info.is_color;
    result.color = std::move(color);
    job.channels.append(std::move(result));
  }
}

bool BKE_paint_layers_row_result_job_commit(PaintLayersRowResultJob &job)
{
  if (job.bmain == nullptr || job.channels.is_empty()) {
    return false;
  }
  Main &bmain = *job.bmain;
  Material *ma = nullptr;
  for (Material &candidate : bmain.materials) {
    if (candidate.id.session_uid == job.material_session_uid && &candidate == job.material) {
      ma = &candidate;
      break;
    }
  }
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  MaterialPaintLayer *target = BKE_paint_layers_find(*ma, job.target_marker);
  if (target == nullptr) {
    return false;
  }
  bool any = false;
  for (PaintLayersRowResultJob::ChannelResult &result : job.channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(result.channel));
    char name[192];
    SNPRINTF(name, "%s %s", target->name[0] != '\0' ? target->name : "Layer", info.ui_name);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(&bmain,
                                           job.size,
                                           job.size,
                                           name,
                                           32,
                                           false,
                                           IMA_GENTYPE_BLANK,
                                           black,
                                           false,
                                           /*is_data=*/!info.is_color,
                                           false);
    if (image == nullptr) {
      continue;
    }
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    /* A row result is a bake the stack owns, so the browsers hide it like every other map. */
    BKE_paint_layers_image_mark_internal(*image);
    /* The render is the CPU one the bake planner uses, so the colour's alpha already is the source
     * row's content alpha. Written through unchanged: the target map then means the same thing any
     * other paint-layer map does. */
    bake_write_image(*image, result.color.data(), info.is_color, nullptr);
    if (BKE_paint_layers_channel_add(*ma, target, eMaterialPaintChannel(result.channel)) ==
            nullptr ||
        !BKE_paint_layers_channel_set_image(
            *ma, target, eMaterialPaintChannel(result.channel), image))
    {
      BKE_id_free(&bmain, &image->id);
      continue;
    }
    any = true;
  }
  if (any) {
    /* Remembered so the target can be refreshed from the same row without choosing it again. */
    target->row_result_source = job.source_marker;
  }
  return any;
}

void BKE_paint_layers_row_result_job_free(PaintLayersRowResultJob &job)
{
  if (job.material_copy != nullptr) {
    BKE_id_free(nullptr, &job.material_copy->id);
    job.material_copy = nullptr;
  }
  MEM_delete(&job);
}

/** \} */

}  // namespace blender
