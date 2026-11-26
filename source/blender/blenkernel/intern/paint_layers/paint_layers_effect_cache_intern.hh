/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * What the CPU effect cache (`paint_layers_composite.cc`) and the GPU effect cache
 * (`paint_layers_effect_gpu_cache.cc`) share: the key of one cached effect, the entry the row
 * cache keeps it in, the staleness snapshot of the maps an input renders from, the LRU container
 * both caches keep their entries in, and the get-or-compute cycle both run -- with the regional
 * update the row cache offers when its maps moved by one rectangle. The container is a template on
 * the entry, and every operation that has to let go of an entry takes the release callable the
 * entry kind needs -- the CPU one releases nothing (the buffer dies with the entry), the GPU one
 * hands the texture back to the hooks that made it.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"

#include "DNA_image_types.h"
#include "IMB_imbuf_types.hh"

#include "paint_layers_effect_half.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Effect staleness
 * \{ */

/**
 * The maps of an input, once each, with the version of their pixels now. An edit that lands after
 * this is read by #map_sources_changes as newer than whatever was stamped from the snapshot.
 */
inline Vector<PaintLayerEffectSource> map_sources_snapshot(const Span<Image *> images)
{
  Vector<PaintLayerEffectSource> sources;
  for (const Image *image : images) {
    sources.append(
        {image->id.session_uid, image->runtime->pixel_version.load(std::memory_order_relaxed)});
  }
  return sources;
}

/**
 * What happened to the maps of an input since \a synced was taken: nothing, one rectangle (in
 * \a r_region), or anything. A map the input did not read before counts as anything.
 */
inline ePixelChange map_sources_changes(const Span<PaintLayerEffectSource> synced,
                                         const Span<Image *> images,
                                         rcti &r_region)
{
  BLI_rcti_init(&r_region, 0, 0, 0, 0);
  if (synced.size() != images.size()) {
    return ePixelChange::Full;
  }
  ePixelChange result = ePixelChange::None;
  for (const Image *image : images) {
    const PaintLayerEffectSource *known = nullptr;
    for (const PaintLayerEffectSource &source : synced) {
      if (source.image_uid == image->id.session_uid) {
        known = &source;
        break;
      }
    }
    if (known == nullptr) {
      return ePixelChange::Full;
    }
    rcti region;
    switch (BKE_image_pixel_changes_since(*image, known->version, region)) {
      case ePixelChange::None:
        break;
      case ePixelChange::Full:
        return ePixelChange::Full;
      case ePixelChange::Region:
        if (result == ePixelChange::None) {
          r_region = region;
        }
        else {
          BLI_rcti_union(&r_region, &region);
        }
        result = ePixelChange::Region;
        break;
    }
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Effect cache key
 * \{ */

/**
 * One cached effect: the result, keyed by the material, the effect, the channel, the UDIM tile and
 * the parameters. Both effect caches key their entries on it.
 */
struct EffectCacheKey {
  uint32_t material_uid = 0;
  /**
   * The marker of the effect itself, not of the row it hangs on: a marker is unique within the
   * material, so two effects of one row -- two Blurs with the same parameters -- key apart, and a
   * re-added effect (a new marker) cannot be served a dead instance's result.
   */
  bUUID effect_marker = {};
  int channel = 0;
  /**
   * The UDIM tile the effect's input was rendered for: one entry per tile, like the slice cache's.
   * A non-tiled stack keeps every effect at the base tile's entry.
   */
  int tile_number = PAINT_TILE_NUMBER_BASE;
  /** The effect kind and its parameters; the input's pixels are the sources' versions below. */
  uint64_t hash = 0;
  /**
   * The key halves beyond the effect's own parameters, shared by every cache (CPU row, CPU mask,
   * GPU): the chain hash (the effects that precede this one in the row, whose results feed this
   * effect's input) and the input scalar hash (the input's non-map scalar content -- a projection
   * frame, a mapping, a flat colour, an adjustment, transform steps -- that no map version
   * carries). The CPU row cache folds both from the described input it computes from, the GPU
   * cache from its jobs' description, so an entry kept from before an edit of the row's mapping or
   * of the effects below misses instead of answering.
   * The mask effect cache (CPU) uses the chain half for what its input is made of: the mask built
   * up to the item, which no map version carries either.
   */
  PaintLayerEffectKeyExtras key_extras;
};

/**
 * The channel marker of a mask effect's entry: outside the channel range, so the grey a mask item
 * computes can never share an entry with a colour channel of the same row's effects.
 */
inline constexpr int PAINT_LAYER_EFFECT_CHANNEL_MASK = -1;

/** Whether \a a and \a b name the same effect instance, whatever its parameters or tile say. */
inline bool paint_layer_effect_key_same_effect(const EffectCacheKey &a, const EffectCacheKey &b)
{
  return a.material_uid == b.material_uid && BLI_uuid_equal(a.effect_marker, b.effect_marker) &&
         a.channel == b.channel;
}

inline bool paint_layer_effect_key_equal(const EffectCacheKey &a, const EffectCacheKey &b)
{
  return paint_layer_effect_key_same_effect(a, b) && a.tile_number == b.tile_number &&
         a.hash == b.hash && a.key_extras == b.key_extras;
}

/**
 * A description hash of the effect: the kind and the parameters in its schema order. The input's
 * pixels are not part of it -- the sources' versions stand in for those -- so an edit of a value
 * moves it and an edit of a map does not. Defined in `paint_layers_composite.cc`, beside the
 * schema readers it folds.
 *
 * \param instance_marker: the composite marker a Smart Filter chain's member is keyed under. A
 * member row is the definition's own, so two instances of one asset carry the same kind, the same
 * parameters and the same DNA marker; folding the composite marker -- which hashes the chain row's
 * own marker in -- is the smallest fold that makes their cache keys differ, here and in the GPU
 * cache, which builds its key from the same function. Nil (every direct effect) folds nothing and
 * the hash is the plain one.
 */
uint64_t paint_layer_effect_hash(const MaterialPaintLayer &effect,
                                 int channel,
                                 const bUUID &instance_marker = {});

/** \} */

/* -------------------------------------------------------------------- */
/** \name Effect cache entry
 * \{ */

/**
 * One entry of the CPU row effect cache (`paint_layers_composite.cc`): the result buffer, the maps
 * the input was rendered from, the size and the byte cost, keyed by #EffectCacheKey. The mask
 * effect cache keeps its own entry type beside its own store; the shared cycle below reads only
 * the fields named here of both. The input's fingerprints live in the key alone -- a hit already
 * implies their equality -- so no entry carries a second copy.
 */
struct EffectCacheEntry {
  EffectCacheKey key;
  int width = 0;
  int height = 0;
  /**
   * The result in its resting form: FP16 for the kinds whose reach is not #EffectReach::WholeMap,
   * float for the whole-map ones (see #PaintLayerEffectBuffer). A caller always receives a float
   * #ImBuf through #paint_layer_effect_buffer_read; the float copy of a half entry lives only as
   * long as the caller's reference, never beside the half storage.
   */
  PaintLayerEffectBuffer color;
  /** The maps the input was rendered from, with the versions they had. */
  Vector<PaintLayerEffectSource> synced;
  size_t bytes = 0;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Effect cache store
 * \{ */

/**
 * The LRU container both effect caches keep their entries in: a vector ordered oldest first, the
 * byte sum over it, and the operations the cache policy runs on it. The vector is the LRU order --
 * a hit moves its entry to the back, the budget evicts from the front. Every operation that lets
 * go of an entry calls \a release on it first: the CPU cache's release is a no-op (the buffer's
 * last reference dies with the entry), the GPU cache's hands the texture back to the hooks that
 * made it. Neither cache is thread-safe on its own; each guards the store with its own mutex.
 */
template<typename Entry> struct PaintLayerEffectCacheStore {
  /** The entries, oldest first: the vector is the LRU order. */
  Vector<Entry> entries;
  /** The sum of the entries' #Entry::bytes. */
  size_t total_bytes = 0;

  /** Take the entry at \a index out of the LRU front and put it at the back. */
  void touch(const int index)
  {
    Entry hit = std::move(entries[index]);
    entries.remove(index);
    entries.append(std::move(hit));
  }

  /** Evict least-recently-used entries from the front until \a incoming fits in \a budget. */
  template<typename Release>
  void enforce_budget(const size_t incoming, const size_t budget, Release &&release)
  {
    while (!entries.is_empty() && total_bytes + incoming > budget) {
      release(entries[0]);
      total_bytes -= entries[0].bytes;
      entries.remove(0);
    }
  }

  /**
   * Drop the entries of the same effect instance and the same UDIM tile as \a key with other
   * parameters: the description can only ever ask for the parameters the effect carries now, so an
   * old result is memory held for nothing. The entries of the effect's other tiles stay, each is
   * the live answer of its own tile. A frame still laying a dropped result keeps its own
   * reference, exactly as it does against the budget eviction.
   */
  template<typename Release> void drop_stale(const EffectCacheKey &key, Release &&release)
  {
    for (int i = int(entries.size()) - 1; i >= 0; i--) {
      if (paint_layer_effect_key_same_effect(entries[i].key, key) &&
          entries[i].key.tile_number == key.tile_number)
      {
        release(entries[i]);
        total_bytes -= entries[i].bytes;
        entries.remove(i);
      }
    }
  }

  /**
   * Drop the entries of the effect instance of \a key on every UDIM tile, whatever their
   * parameters: for the explicit reset of an effect (it was baked, changed or removed).
   */
  template<typename Release> void drop_instance(const EffectCacheKey &key, Release &&release)
  {
    for (int i = int(entries.size()) - 1; i >= 0; i--) {
      if (paint_layer_effect_key_same_effect(entries[i].key, key)) {
        release(entries[i]);
        total_bytes -= entries[i].bytes;
        entries.remove(i);
      }
    }
  }

  /** Drop every entry of the material with the session UID \a material_uid. */
  template<typename Release> void free_material(const uint32_t material_uid, Release &&release)
  {
    for (int i = int(entries.size()) - 1; i >= 0; i--) {
      if (entries[i].key.material_uid == material_uid) {
        release(entries[i]);
        total_bytes -= entries[i].bytes;
        entries.remove(i);
      }
    }
  }

  /** Drop every entry, handing each one to \a release first. */
  template<typename Release> void free_all(Release &&release)
  {
    for (Entry &entry : entries) {
      release(entry);
    }
    entries.clear_and_shrink();
    total_bytes = 0;
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Effect cache get or compute
 * \{ */

/** The bytes \a buffer costs: one float per texel per channel. The row effect cache keeps an RGBA
 * buffer and the mask effect cache one scalar channel, so the count reads the buffer itself rather
 * than assuming RGBA. A half effect entry is counted by #paint_layer_effect_buffer_bytes below at
 * its real FP16 cost, not at this float cost. */
inline size_t paint_layer_effect_buffer_bytes(const ImBuf &buffer)
{
  const size_t channels = buffer.channels == 0 ? 4 : size_t(buffer.channels);
  return size_t(buffer.x) * size_t(buffer.y) * channels * sizeof(float);
}

/** The bytes a stored buffer costs: the mask cache's float entry is counted at its real float cost,
 * and a row effect entry at its resting form -- FP16 is exactly half of an RGBA float buffer. The
 * budget and every accounting test read this, so the saving is the one the cache actually holds. */
inline size_t paint_layer_effect_buffer_bytes(const std::shared_ptr<ImBuf> &stored)
{
  return stored != nullptr ? paint_layer_effect_buffer_bytes(*stored) : 0;
}

inline size_t paint_layer_effect_buffer_bytes(const PaintLayerEffectBuffer &stored)
{
  if (stored.halfs != nullptr) {
    /* Four FP16 components per texel: half of an RGBA float texel. */
    return size_t(stored.halfs->width) * size_t(stored.halfs->height) * 4 * sizeof(uint16_t);
  }
  return paint_layer_effect_buffer_bytes(stored.floats);
}

/* Declared first: the no-region wrapper below forwards to it, and the unqualified call must find
 * the name at the wrapper's definition, not only at instantiation. */
template<typename Entry, typename Compute, typename RegionUpdate>
std::shared_ptr<ImBuf> paint_layer_effect_cache_get_or_compute_region(
    PaintLayerEffectCacheStore<Entry> &store,
    std::mutex &mutex,
    const EffectCacheKey &key,
    const int width,
    const int height,
    const Span<Image *> images,
    const size_t budget,
    Compute &&compute,
    const rcti *changed_region,
    rcti *r_valid_region,
    RegionUpdate &&region_update);

/**
 * The one cycle both effect caches run -- the row's (`paint_layer_effect_buffer_get`) and the mask
 * item's (`paint_layer_mask_effect_buffer_get`): find a live entry of \a key at \a width x
 * \a height whose maps have not changed since they were rendered from \a images, or compute one
 * and insert it. \a compute fills the result and the staleness snapshot of its maps, and, when it
 * already holds the float buffer the kernel produced, hands that buffer over in \a r_answer -- the
 * cache then serves it without a second materialization. (A mask compute answers its stored scalar
 * grey there too; a compute that leaves it null falls back to #paint_layer_effect_buffer_read of
 * the stored form.) \a compute runs outside the mutex, so a nested cache taking \a mutex cannot
 * deadlock against it, and two threads that miss together may both run it -- a benign duplicate
 * compute, the second result replacing the first. The returned buffer goes out before the entry
 * goes back, so the caller keeps it alive through its own reference whatever the cache does next.
 */
template<typename Entry, typename Compute>
std::shared_ptr<ImBuf> paint_layer_effect_cache_get_or_compute(
    PaintLayerEffectCacheStore<Entry> &store,
    std::mutex &mutex,
    const EffectCacheKey &key,
    const int width,
    const int height,
    const Span<Image *> images,
    const size_t budget,
    Compute &&compute)
{
  /* No region is offered, so the cycle below never takes its regional branch: a hit still requires
   * the maps to have moved by nothing, exactly as it did before the branch existed. */
  return paint_layer_effect_cache_get_or_compute_region(
      store,
      mutex,
      key,
      width,
      height,
      images,
      budget,
      std::forward<Compute>(compute),
      static_cast<const rcti *>(nullptr),
      static_cast<rcti *>(nullptr),
      [](const auto &,
         const rcti &,
         auto &,
         Vector<PaintLayerEffectSource> &,
         rcti &,
         std::shared_ptr<ImBuf> &) { return false; });
}

/**
 * The same cycle with the regional update: when a live entry's maps moved by one rectangle
 * (#ePixelChange::Region) and the caller offers \a changed_region, \a region_update may patch the
 * entry instead of the whole-canvas recompute. The update receives the entry's own result buffer
 * (a private copy of the shared pointer, taken under the mutex) and the rectangle the maps moved
 * by -- the one \a map_sources_changes reported, which is the exact truth the staleness came from,
 * not the caller's earlier estimate -- and fills \a r_color with the patched result and \a
 * r_synced with the maps' fresh snapshot, naming the rectangle the patch made correct in \a
 * r_valid. When the patch holds the float buffer it wrote, it hands it over in \a r_answer so the
 * cache serves it without materializing the stored form again; a null answer falls back to
 * #paint_layer_effect_buffer_read. False, or an update refused by the caller's own gates (a kind
 * whose kernel is not a local function of the neighbourhood), leaves the entry to \a compute as
 * before.
 *
 * The update runs outside the mutex, like \a compute: the copy taken under it is what it patches,
 * and the patched result is put back with the same drop-and-insert the full compute ends with, so
 * an entry another thread replaced or evicted meanwhile costs the patch nothing but its work. Two
 * threads that patch together may both run -- the same benign duplicate the compute allows, the
 * second result replacing the first. \a r_valid_region, when asked, is filled on every answer with
 * the rectangle of the answer a map-writing caller can lay from it: the whole canvas after a full
 * compute, the caller's own staleness band on a standing result (current as a whole, so whatever
 * band the caller names can be written from it), the patch's band after an update. Empty only when
 * the offered band was empty (nothing moved that a write could narrow) or the compute failed.
 */
template<typename Entry, typename Compute, typename RegionUpdate>
std::shared_ptr<ImBuf> paint_layer_effect_cache_get_or_compute_region(
    PaintLayerEffectCacheStore<Entry> &store,
    std::mutex &mutex,
    const EffectCacheKey &key,
    const int width,
    const int height,
    const Span<Image *> images,
    const size_t budget,
    Compute &&compute,
    const rcti *changed_region,
    rcti *r_valid_region,
    RegionUpdate &&region_update)
{
  /* The stored form of an entry: a float #ImBuf for the mask cache, #PaintLayerEffectBuffer for the
   * row cache (float or FP16). The cycle only ever moves this value around; #read below is the one
   * place it turns into the float buffer a caller receives. */
  using Stored = std::decay_t<decltype(store.entries[0].color)>;
  Stored cached;
  rcti maps_region;
  bool region_offered = false;
  /* A standing result answered inside the mutex but read outside it: materializing a half entry is
   * a whole-canvas allocation the lock must not cover. */
  Stored standing;
  bool standing_hit = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < int(store.entries.size()); i++) {
      Entry &entry = store.entries[i];
      if (!paint_layer_effect_key_equal(entry.key, key)) {
        continue;
      }
      if (entry.width != width || entry.height != height) {
        break;
      }
      const ePixelChange change = map_sources_changes(entry.synced, images, maps_region);
      if (change == ePixelChange::None) {
        /* The result stands: the maps it was rendered from have not moved since. The valid region
         * is the caller's own staleness band when it offered one -- the answer is current as a
         * whole, so the band a caller that lays a map from it is missing can be written straight
         * from it -- and the whole canvas otherwise, the caller knowing no narrower band to name.
         * #paint_layer_effect_buffer_get grows an offered band by the halo and clamps it. */
        standing = entry.color;
        if (r_valid_region != nullptr) {
          if (changed_region != nullptr) {
            *r_valid_region = *changed_region;
          }
          else {
            BLI_rcti_init(r_valid_region, 0, width, 0, height);
          }
        }
        /* Move to the back: the vector is the LRU order. */
        store.touch(i);
        standing_hit = true;
        break;
      }
      if (change == ePixelChange::Region && changed_region == nullptr &&
          BKE_paint_layers_bake_stroke_active_get())
      {
        /* The preview asks for the effect while a stroke paints its input. Recomputing the whole
         * canvas at every dab is what the stroke deferral already avoids for the bake; the
         * composite keeps the last result until the stroke ends, then the next ask recomputes it
         * once. A region-offering caller (the bake) still patches / recomputes as before. */
        standing = entry.color;
        if (r_valid_region != nullptr) {
          BLI_rcti_init(r_valid_region, 0, 0, 0, 0);
        }
        store.touch(i);
        standing_hit = true;
        break;
      }
      if (change == ePixelChange::Region && changed_region != nullptr) {
        /* The entry was found by the full key equality above -- parameters, tile and both input
         * fingerprints -- so it names this call's own base: the only thing left to judge is how
         * its maps moved, which #map_sources_changes just reported. */
        /* A private copy of the shared pointer: the update runs outside the mutex, and the store
         * may touch, replace or evict the entry while it runs. The patched result is put back
         * below under a fresh lock, whatever became of the entry meanwhile. */
        cached = entry.color;
        region_offered = true;
        break;
      }
      /* A full change, a region the caller offers no patch for, or a size that no longer fits:
       * the entry is stale, the whole compute answers. */
      break;
    }
  }

  if (standing_hit) {
    return paint_layer_effect_buffer_read(standing);
  }

  if (region_offered) {
    Stored patched;
    Vector<PaintLayerEffectSource> patched_synced;
    rcti patched_valid;
    /* The patch's own float buffer, when it has one: the cache serves it instead of materializing
     * the stored half again. A patch that answers only its stored form leaves this null. */
    std::shared_ptr<ImBuf> patched_answer;
    if (region_update(cached,
                      maps_region,
                      patched,
                      patched_synced,
                      patched_valid,
                      patched_answer))
    {
      int patched_width = 0;
      int patched_height = 0;
      paint_layer_effect_buffer_dimensions(patched, patched_width, patched_height);
      const size_t bytes = paint_layer_effect_buffer_bytes(patched);
      std::shared_ptr<ImBuf> answer = patched_answer != nullptr ?
                                           patched_answer :
                                           paint_layer_effect_buffer_read(patched);
      std::lock_guard<std::mutex> lock(mutex);
      /* The same put-back the full compute ends with: an entry of the effect and tile is replaced
       * by the patch, the other tiles' entries stay, and a buffer still shared with a live
       * description keeps its pixels through its own reference. */
      const auto no_release = [](Entry &) {};
      store.drop_stale(key, no_release);
      if (bytes <= budget) {
        store.enforce_budget(bytes, budget, no_release);
        Entry entry;
        entry.key = key;
        entry.width = patched_width;
        entry.height = patched_height;
        entry.color = std::move(patched);
        entry.synced = std::move(patched_synced);
        entry.bytes = bytes;
        store.total_bytes += bytes;
        store.entries.append(std::move(entry));
      }
      if (r_valid_region != nullptr) {
        *r_valid_region = patched_valid;
      }
      return answer;
    }
    /* The patch was refused (the kind, the region or the input): the full compute answers, the
     * cached buffer stays until it returns. */
  }

  Stored color;
  Vector<PaintLayerEffectSource> synced;
  /* The float buffer a row compute already produced: the one the kernels ran on. It is served to
   * the caller as is, so a miss on a half entry never round-trips through FP16. A mask compute
   * answers its stored grey here; null falls back to materializing the stored form. */
  std::shared_ptr<ImBuf> answer;
  if (!compute(color, synced, answer)) {
    if (r_valid_region != nullptr) {
      BLI_rcti_init(r_valid_region, 0, 0, 0, 0);
    }
    return {};
  }
  int color_width = 0;
  int color_height = 0;
  paint_layer_effect_buffer_dimensions(color, color_width, color_height);
  const size_t bytes = paint_layer_effect_buffer_bytes(color);
  if (answer == nullptr) {
    /* No compute buffer was offered (the mask cache's stored scalar, or a compute that packed
     * without handing the float source back): materialize the stored form outside the lock. */
    answer = paint_layer_effect_buffer_read(color);
  }

  std::lock_guard<std::mutex> lock(mutex);
  /* An entry of the same effect and tile with other parameters is stale -- the description can only
   * ever ask for the parameters the effect carries now -- so it is replaced rather than kept until
   * the LRU evicts it. The other tiles' entries are live answers and stay. A buffer still shared
   * with a live description keeps its pixels through its own reference. */
  const auto no_release = [](Entry &) {};
  store.drop_stale(key, no_release);
  if (bytes <= budget) {
    store.enforce_budget(bytes, budget, no_release);
    Entry entry;
    entry.key = key;
    entry.width = color_width;
    entry.height = color_height;
    entry.color = std::move(color);
    entry.synced = std::move(synced);
    entry.bytes = bytes;
    store.total_bytes += bytes;
    store.entries.append(std::move(entry));
  }
  if (r_valid_region != nullptr) {
    /* The whole result is new: every texel of it is worth writing where a caller keeps one. */
    BLI_rcti_init(r_valid_region, 0, width, 0, height);
  }
  return answer;
}

/** \} */

}  // namespace blender
