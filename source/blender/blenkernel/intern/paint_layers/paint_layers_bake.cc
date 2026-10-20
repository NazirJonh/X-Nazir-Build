/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The row bake of a layered material: the cache key, the service maps the generator and the CPU
 * substitute for a row's live subtree, the planner that fills them and the worker job that does
 * the heavy ones off the main thread. The description and its edits live in `paint_layers.cc`; the
 * helpers both files need are in `paint_layers_intern.hh`.
 *
 * A baked row is a *cache*: the description plus its live maps stay what the row is, and the bake is
 * read instead of the subtree only while its stored hash matches the description and no pixel of a
 * source map changed since (see #BKE_paint_layers_bake_is_valid). Everything that decides whether a
 * row is substituted goes through #paint_layer_row_substituted, so the generator and the CPU
 * composite cannot disagree about it.
 */

#include "BKE_paint_layers.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_uuid_types.h"

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

int BKE_paint_layers_bake_mode_get(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr ? int(layer.bake->mode) : int(MA_PAINT_LAYER_BAKE_AUTO);
}

bool BKE_paint_layers_bake_mode_set(Material &ma, MaterialPaintLayer &layer, const int mode)
{
  if (mode < MA_PAINT_LAYER_BAKE_AUTO || mode > MA_PAINT_LAYER_BAKE_NEVER ||
      BKE_paint_layers_owner_list(ma, layer) == nullptr)
  {
    return false;
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
  if (size <= 0 || BKE_paint_layers_owner_list(ma, layer) == nullptr) {
    return false;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  bake->size = size;
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
  if (BKE_paint_layers_owner_list(ma, layer) == nullptr) {
    return false;
  }
  if (layer.bake != nullptr) {
    bake_release_users(*layer.bake);
    MEM_SAFE_DELETE(layer.bake);
  }
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_bake_set_map(Material &ma,
                                   MaterialPaintLayer &layer,
                                   const int channel,
                                   Image *image)
{
  if (channel < -1 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      BKE_paint_layers_owner_list(ma, layer) == nullptr)
  {
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
 * Every map \a layer's bake reads: its own channel maps and those of its corrections, mask items
 * and children. A Material row reads the images of its source's node trees when its own bake is the
 * question, and the maps it shows when it is a child of the row being baked.
 */
static void bake_collect_source_images(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       Vector<Image *> &r_images,
                                       const bool is_child = false)
{
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    if (!is_child) {
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
    /* The textures it shows live: a stroke on one changes the folder's result at once. A rendered
     * map is not one: it is written by the row's own render, which would then read as an edit of
     * its source and start the render again. */
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
    /* What a Material row renders is its source alone: its mask, effects and children stay live
     * over the maps, so a stroke on them is no edit of what the render reads. */
    if (!is_child) {
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
    bake_collect_source_images(ma, effect, r_images, true);
  }
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
    bake_collect_source_images(ma, mask_item, r_images, true);
  }
  for (const MaterialPaintLayer &child : layer.children) {
    bake_collect_source_images(ma, child, r_images, true);
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
  bake_collect_source_images(ma, layer, images);
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

bool BKE_paint_layers_bake_changed_region(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int r_region[4])
{
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime == nullptr) {
    return false;
  }
  for (const BakeLayerSubscription &entry : runtime->layers) {
    if (BLI_uuid_equal(entry.marker, layer.marker) && entry.changed && entry.has_region) {
      r_region[0] = entry.xmin;
      r_region[1] = entry.xmax;
      r_region[2] = entry.ymin;
      r_region[3] = entry.ymax;
      return true;
    }
  }
  return false;
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
  /* Bumped when what a bake stores changes meaning, so older caches invalidate once. */
  h = bake_hash_mix(h, uint64_t(0x9e3779b97f4a7c15ull));
  h = bake_hash_mix(h, uint8_t(layer.source));
  h = bake_hash_mix(h, uint16_t(layer.kind_subtype));
  h = bake_hash_mix(h, uint8_t(layer.blend));
  /* The non-visibility flags of the row itself. Visibility is appended only for a child, so the
   * row's own cache does not depend on its own on/off state. */
  h = bake_hash_mix(h, uint16_t(layer.flag & ~MA_PAINT_LAYER_ENABLED));
  h = bake_hash_mix(h, uint8_t(layer.role));
  h = bake_hash_float(h, layer.opacity);
  /* Only the fact that the mapping applies: such a row stays live and is never baked
   * (#BKE_paint_layers_bake_blocked), so the slider values must not move the hash. A Material row
   * is rendered and its mapping is applied when its maps are read, so toggling it must not move a
   * parent folder's hash either; the folder is kept live by the same block while the mapping is
   * on. */
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    h = bake_hash_mix(h, BKE_paint_layers_mapping_applies(ma, layer) ? 1 : 0);
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
  }
  /* The per (row, channel) blend and opacity overrides are baked into the row's result too, and
   * exist whether or not the pair has a record. */
  for (const MaterialPaintLayerChannelSettings &settings : layer.channel_settings) {
    h = bake_hash_mix(h, uint8_t(settings.blend));
    h = bake_hash_float(h, settings.opacity);
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
    h = bake_hash_mix(h, layer.bake != nullptr ? uint32_t(layer.bake->size) : 0);
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
  /* A kind's parameters (a filter's, a coverage operator's) change the pixels. */
  h = bake_hash_idprops(h, layer.system_properties);
  /* Effects first, then mask items, then children, each in its own storage order. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    h = bake_hash_layer(h, ma, effect, true);
  }
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
    h = bake_hash_layer(h, ma, mask_item, true);
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
  if (layer.bake == nullptr || BKE_paint_layers_owner_list(ma, layer) == nullptr) {
    return;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  layer.bake->hash[0] = hash[0];
  layer.bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
}

/** Whether \a layer or anything under it applies a UV mapping: a mapped row stays live. */
static bool bake_mapping_blocks(const Material &ma, const MaterialPaintLayer &layer)
{
  if (BKE_paint_layers_mapping_applies(ma, layer)) {
    return true;
  }
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (bake_mapping_blocks(ma, effect)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
    if (bake_mapping_blocks(ma, mask_item)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (bake_mapping_blocks(ma, child)) {
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
  return bake_mapping_blocks(ma, layer) || bake_object_mask_blocks(layer);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Substitution
 * \{ */

bool paint_layer_row_substituted(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A Material row's maps are its source, not a cache of its subtree: it is laid like a painted row
   * (see #paint_layer_row_material_map), with its corrections and mask live over them. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
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

static Image *bake_service_image(Main &bmain,
                                 MaterialPaintLayer &layer,
                                 const int channel,
                                 const int size,
                                 const bool is_color)
{
  Image **slot = bake_service_slot(layer, channel);
  if (*slot != nullptr) {
    int width = 0;
    int height = 0;
    BKE_image_get_size(*slot, nullptr, &width, &height);
    if (width == size && height == size) {
      return *slot;
    }
    /* The map was created at another size (the row's `bake_size` changed): the pixel writers index
     * the render buffer by the image's own width, so a map of a different size cannot be reused. The
     * creation reference is the bake's alone, so the old map goes with it. */
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
      &bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, black, false, !is_color, false);
  if (image != nullptr) {
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
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

/** Write \a values into \a image, or only into \a rect (x0, y0, x1, y1 in image pixels) when given. */
static void bake_write_image(Image &image, const float *values, const bool is_color, const int *rect)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  uchar *pixels = ibuf->byte_data_for_write();
  const auto *colorspace = ibuf->byte_buffer.colorspace;
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  if (pixels != nullptr) {
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const int64_t i = int64_t(y) * width + x;
        /* The colour's fourth component carries the row's content alpha. */
        float rgba[4] = {values[i * 4 + 0],
                         values[i * 4 + 1],
                         values[i * 4 + 2],
                         clamp_f(values[i * 4 + 3], 0.0f, 1.0f)};
        if (is_color && colorspace != nullptr) {
          IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, colorspace);
        }
        pixels[i * 4 + 0] = uchar(clamp_i(int(rgba[0] * 255.0f + 0.5f), 0, 255));
        pixels[i * 4 + 1] = uchar(clamp_i(int(rgba[1] * 255.0f + 0.5f), 0, 255));
        pixels[i * 4 + 2] = uchar(clamp_i(int(rgba[2] * 255.0f + 0.5f), 0, 255));
        pixels[i * 4 + 3] = uchar(clamp_i(int(rgba[3] * 255.0f + 0.5f), 0, 255));
      }
    }
  }
  bake_image_pixels_changed(image, *ibuf);
  BKE_image_release_ibuf(&image, ibuf, lock);
}

/** The coverage map keeps its grey in all three colour channels and is opaque. */
static void bake_write_coverage_image(Image &image, const float *coverage, const int *rect)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
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
  for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
    weight += 4 + bake_subtree_weight(ma, mask_item) / 2;
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

bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer)
{
  if (bake_subtree_weight(ma, layer) > PAINT_LAYERS_AUTO_BAKE_NODES) {
    return true;
  }
  return layer.bake != nullptr && layer.bake->size >= PAINT_LAYERS_HEAVY_BAKE_SIZE;
}

bool BKE_paint_layers_bake_row_is_deferred(const Material &ma, const MaterialPaintLayer &layer)
{
  /* The active row and its ancestors stay live: the user is editing inside them, and the bake
   * catches up once the active marker leaves the subtree. */
  return BKE_paint_layers_subtree_contains(layer, ma.active_layer_marker);
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
  bake_hidden_find(ma.paint_layers, &layer, false, hidden);
  return hidden;
}

/**
 * Free \a layer's bake structure and the service maps only it owns.
 *
 * A map whose last user was the bake is deleted; one still named by the generated tree keeps that
 * other user and is simply detached, because freeing it there would leave the tree's node with a
 * dangling pointer. The caller tags #MA_PAINT_LAYERS_REGEN, so the rebuild that follows drops the
 * node before anything is evaluated.
 */
static void bake_structure_free(Main &bmain, Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return;
  }
  const auto release_map = [&](Image *&slot) {
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
  };
  for (Image *&image : layer.bake->images) {
    release_map(image);
  }
  release_map(layer.bake->coverage);
  if (ma.paint_layers_tree != nullptr) {
    DEG_id_tag_update(&ma.paint_layers_tree->id, ID_RECALC_SYNC_TO_EVAL);
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

/** The side \a row is baked at: its stored size, the one an AUTO release kept, then its content's. */
static int bake_size_for(const Material &ma, const MaterialPaintLayer &row)
{
  int size = (row.bake != nullptr) ? row.bake->size : 0;
  if (size <= 0) {
    size = bake_dropped_size(ma, row);
  }
  if (size <= 0) {
    size = bake_size_from_content(ma, row);
  }
  return size;
}

/**
 * Whether the planner considers \a layer at all: a Layer row that is not a filter, not a Pass
 * Through folder and not kept live by a mapping or an object mask somewhere inside it.
 */
static bool bake_row_is_candidate(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.role != MA_PAINT_LAYER_ROLE_LAYER || paint_layer_row_is_adjustment(layer)) {
    return false;
  }
  /* A row that reads a material has no live subtree to cache; what it shows is rendered from its
   * source, which is not the planner's to do from the row's own description. */
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    return false;
  }
  return !BKE_paint_layers_bake_blocked(ma, layer);
}

/**
 * Whether \a layer is a row the planner has to bake now: a candidate, not switched off or hidden,
 * not the row being edited, with no valid bake, and -- for AUTO -- heavy enough and, for a folder,
 * nested deep enough to be worth a cache.
 *
 * \param r_release: set when the row carries a bake that the AUTO rules no longer want, which the
 * caller frees.
 */
static bool bake_row_is_due(const Material &ma, const MaterialPaintLayer &layer, bool &r_release)
{
  r_release = false;
  if (!bake_row_is_candidate(ma, layer)) {
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
  if (!folder && mode == MA_PAINT_LAYER_BAKE_AUTO) {
    /* AUTO caches subgraphs: a folder is the heavy thing, a single row is already one map per
     * channel and a cache of it would only double its memory. Always is the way to ask for a row
     * to be baked; a cache carried from an earlier Always keeps standing in while it is valid and
     * is released once it is not. */
    r_release = layer.bake != nullptr && !BKE_paint_layers_bake_is_valid(ma, layer);
    return false;
  }
  if (folder && mode == MA_PAINT_LAYER_BAKE_AUTO &&
      !BKE_paint_layers_folder_auto_bake_allowed(ma, layer))
  {
    /* A folder in the stack root stays live for responsiveness; a bake it carried from before the
     * rule is released. A manual ALWAYS is the user's and is left alone. */
    r_release = layer.bake != nullptr;
    return false;
  }
  if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
    return false;
  }
  if (BKE_paint_layers_bake_row_is_deferred(ma, layer) || bake_row_is_hidden(ma, layer)) {
    return false;
  }
  if (mode == MA_PAINT_LAYER_BAKE_AUTO && bake_subtree_weight(ma, layer) <= PAINT_LAYERS_AUTO_BAKE_NODES)
  {
    /* A light subtree is cheaper live; a bake carried over from when it was heavy is released. */
    r_release = layer.bake != nullptr;
    return false;
  }
  return true;
}

bool BKE_paint_layers_bake_plan_run(Main &bmain, Material &ma, bool *r_changed)
{
  bool changed = false;
  if (BKE_paint_layers_is_layered(ma)) {
    Vector<MaterialPaintLayer *> rows;
    BKE_paint_layers_foreach(ma,
                             [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
                               rows.append(&layer);
                               return true;
                             });
    /* Children before their parents: a parent's render composites its children, and a child that is
     * already baked stands in for its whole subtree, so the reverse of the pre-order walk does the
     * least work. */
    for (int index = int(rows.size()) - 1; index >= 0; index--) {
      MaterialPaintLayer &layer = *rows[index];
      bool release = false;
      if (!bake_row_is_due(ma, layer, release)) {
        if (release) {
          bake_dropped_size_keep(ma, layer);
          bake_structure_free(bmain, ma, layer);
          changed = true;
        }
        continue;
      }
      /* A heavy row leaves the main thread: the worker job picks it up from the same stale signal
       * this function leaves set. */
      if (BKE_paint_layers_bake_is_heavy(ma, layer)) {
        continue;
      }
      const int size = bake_size_for(ma, layer);
      if (size <= 0) {
        /* No map has known dimensions yet: nothing to allocate a bake structure for. */
        continue;
      }
      /* Every gate has passed and this row renders right now: only here is it safe to allocate the
       * bake structure, so a row nothing ever bakes never carries a permanently invalid one. */
      MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
      bake->size = size;

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

      Vector<float> color(int64_t(size) * size * 4);
      Vector<float> coverage(int64_t(size) * size);
      /* The channel-independent row factor `common = mask x opacity`, the max of
       * `coverage / content` over channels, written once below so channel order cannot matter. A
       * partial re-bake folds only its rectangle; untouched texels keep the stored common. */
      Vector<float> common(int64_t(size) * size, 0.0f);
      bool any_channel = false;
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        const int channel = int(info.channel);
        /* A partial re-bake needs the maps to already exist: an image created just now cannot take a
         * partial write, its untouched region would stay blank. */
        const int *channel_rect = (rect_ptr != nullptr && bake->images[channel] != nullptr) ?
                                      rect_ptr :
                                      nullptr;
        if (!BKE_paint_layers_bake_render_node(
                ma, layer, channel, size, color.data(), coverage.data(), channel_rect))
        {
          continue;
        }
        Image *color_image = bake_service_image(bmain, layer, channel, size, info.is_color);
        if (color_image == nullptr) {
          continue;
        }
        bake_write_image(*color_image, color.data(), info.is_color, channel_rect);
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
        continue;
      }
      /* One shared coverage write for all channels; a partial re-bake touches only its rectangle,
       * so the stored common outside it survives. */
      if (Image *coverage_image = bake_service_image(bmain, layer, -1, size, false)) {
        bake_write_coverage_image(*coverage_image,
                                  common.data(),
                                  (rect_ptr != nullptr && reuse_cache) ? rect_ptr : nullptr);
      }
      BKE_paint_layers_bake_finalize(ma, layer);
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
              bake_row_is_candidate(ma, layer) && !BKE_paint_layers_bake_is_valid(ma, layer) &&
              !bake_row_is_hidden(ma, layer))
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

bool BKE_paint_layers_bake_heavy_pending(const Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma) || (ma.paint_layers_flag & MA_PAINT_LAYERS_BAKE_CANCELLED) != 0) {
    return false;
  }
  bool pending = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        bool release = false;
        pending = bake_row_is_due(ma, layer, release) && BKE_paint_layers_bake_is_heavy(ma, layer);
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
      layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER || layer.role != MA_PAINT_LAYER_ROLE_LAYER)
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

bool BKE_paint_layers_material_row_due(const Material &ma, const MaterialPaintLayer &layer)
{
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ||
      layer.role != MA_PAINT_LAYER_ROLE_LAYER || layer.material == nullptr ||
      layer.material->nodetree == nullptr ||
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
  BKE_paint_layers_foreach(
      consumer, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) {
        if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) ||
            row.material != &source || !BKE_paint_layers_bake_row_is_deferred(consumer, row))
        {
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
  BKE_paint_layers_foreach(
      static_cast<const Material &>(ma),
      [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) {
        if (paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE)) {
          signatures.append({row.marker, paint_layer_material_live_signature(ma, row)});
        }
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
    BKE_paint_layers_foreach(static_cast<const Material &>(ma),
                             [&](const MaterialPaintLayer &row, const MaterialPaintLayer *) {
                               reads = paint_layer_kind_has_flag(row,
                                                                 PAINT_LAYER_KIND_READS_SOURCE) &&
                                       row.material == &source;
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
  BKE_paint_layers_foreach(ma, [&](const MaterialPaintLayer &row, const MaterialPaintLayer *) {
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
      channels.size() != images.size() || BKE_paint_layers_owner_list(ma, layer) == nullptr)
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
  /** Localized on the main thread at creation so the worker never reads a material being edited. */
  Material *material_copy = nullptr;

  struct ChannelResult {
    int channel = 0;
    bool is_color = false;
    Vector<float> color;
    Vector<float> coverage;
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
    Vector<ChannelResult> channels;
  };
  Vector<RowResult> rows;
  /** The worker stopped early; nothing it produced is written. */
  bool cancelled = false;
};

/**
 * The channel-independent row factor: `common = mask x opacity`, recovered from the per-channel
 * renders without a new map. Every channel rendered `coverage = common x content` with its own
 * `content` in the colour alpha. Where that content is present the quotient is the common factor;
 * the maximum over channels does not depend on channel order, and a texel no channel covers keeps
 * zero.
 */
static void bake_common_from_job_channels(Span<PaintLayersBakeJob::ChannelResult> channels,
                                          const int64_t pixel_num,
                                          float *r_common)
{
  for (int64_t i = 0; i < pixel_num; i++) {
    float common = 0.0f;
    for (const PaintLayersBakeJob::ChannelResult &channel : channels) {
      const float content = channel.color[i * 4 + 3];
      if (content > 0.002f) {
        common = std::max(common, clamp_f(channel.coverage[i] / content, 0.0f, 1.0f));
      }
    }
    r_common[i] = common;
  }
}

PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain, Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return nullptr;
  }
  Vector<MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
    rows.append(&layer);
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
    bool release = false;
    if (!bake_row_is_due(ma, layer, release) || !BKE_paint_layers_bake_is_heavy(ma, layer)) {
      continue;
    }
    const int size = bake_size_for(ma, layer);
    if (size <= 0) {
      continue;
    }
    /* Every gate has passed and this row is queued right now: only here is it safe to allocate the
     * bake structure. */
    BKE_paint_layers_bake_struct_ensure(layer)->size = size;
    PaintLayersBakeJob::RowResult row;
    row.marker = layer.marker;
    row.size = size;
    BKE_paint_layers_bake_hash(ma, layer, row.hash);
    job->rows.append(std::move(row));
  }
  if (job->rows.is_empty()) {
    MEM_delete(job);
    return nullptr;
  }
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
  /* Every row is weighted by the same fixed channel count, whether or not each channel actually
   * renders, so progress only ever needs the two totals below. */
  const int64_t total_pairs = int64_t(job.rows.size()) * BKE_paint_material_channels().size();
  int64_t done_pairs = 0;
  const auto report = [&]() {
    if (report_progress) {
      report_progress(total_pairs > 0 ? float(done_pairs) / float(total_pairs) : 1.0f);
    }
  };
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(ma, row.marker);
    if (layer == nullptr) {
      done_pairs += BKE_paint_material_channels().size();
      report();
      continue;
    }
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      /* The render of one channel is not interruptible; checking between channels keeps the cancel
       * button responsive to within one channel's render. */
      if (is_cancelled && is_cancelled()) {
        job.cancelled = true;
        return;
      }
      const int channel = int(info.channel);
      Vector<float> color(int64_t(row.size) * row.size * 4);
      Vector<float> coverage(int64_t(row.size) * row.size);
      const bool rendered = BKE_paint_layers_bake_render_node(
          ma, *layer, channel, row.size, color.data(), coverage.data());
      done_pairs++;
      report();
      if (!rendered) {
        continue;
      }
      PaintLayersBakeJob::ChannelResult result;
      result.channel = channel;
      result.is_color = info.is_color;
      result.color = std::move(color);
      result.coverage = std::move(coverage);
      row.channels.append(std::move(result));
    }
  }
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
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, row.marker);
    /* The row was removed, its bake dropped or switched off while the worker ran: nothing to
     * write. */
    if (layer == nullptr || layer->bake == nullptr || layer->bake->mode == MA_PAINT_LAYER_BAKE_NEVER)
    {
      continue;
    }
    bool row_written = false;
    for (PaintLayersBakeJob::ChannelResult &channel : row.channels) {
      Image *color_image = bake_service_image(
          bmain, *layer, channel.channel, row.size, channel.is_color);
      if (color_image == nullptr) {
        continue;
      }
      bake_write_image(*color_image, channel.color.data(), channel.is_color, nullptr);
      row_written = true;
    }
    if (!row_written) {
      continue;
    }
    {
      const int64_t pixel_num = int64_t(row.size) * row.size;
      Vector<float> common(pixel_num);
      bake_common_from_job_channels(row.channels, pixel_num, common.data());
      if (Image *coverage_image = bake_service_image(bmain, *layer, -1, row.size, false)) {
        bake_write_coverage_image(*coverage_image, common.data(), nullptr);
      }
    }
    layer->bake->hash[0] = row.hash[0];
    layer->bake->hash[1] = row.hash[1];
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
                  bake_row_is_candidate(*ma, layer) &&
                  !BKE_paint_layers_bake_is_valid(*ma, layer) && !bake_row_is_hidden(*ma, layer);
        return !pending;
      });
  if (!pending) {
    ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
  }
  return changed;
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

}  // namespace blender
