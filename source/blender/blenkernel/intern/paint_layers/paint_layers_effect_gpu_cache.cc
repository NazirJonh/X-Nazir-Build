/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The GPU effect result cache: the container behind the "GPU Effect Result Cache" section of
 * #BKE_paint_layers_composite.hh. The policy and the hooks are documented with the header section
 * they implement; what lives here is the container, and it holds no GPU type -- the texture is a
 * `void *` the caller's hooks made and drop, so the keying, the budget and the staleness rule are
 * reachable from the plain effect tests without a GPU context. The key, the staleness snapshot and
 * the LRU container are the CPU effect cache's (see #paint_layers_effect_cache_intern.hh); the
 * difference is what an entry holds and what letting go of one calls.
 */

#include <cstdint>
#include <mutex>

#include "BLI_assert.h"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"

#include "paint_layers_effect_cache_intern.hh"
#include "paint_layers_intern.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Paint Layer Effect GPU Result Cache
 *
 * The GPU twin of the CPU effect cache (`paint_layers_composite.cc`), for the kinds the registry
 * marks #PAINT_LAYER_KIND_GPU_COMPUTED.
 * \{ */

/**
 * What the GPU effect cache may hold: a 4K RGBA half-float canvas is 128 MiB per effect, so this
 * keeps four of them at 4K or a dozen at 2K. Deliberately smaller than the CPU effect cache beside
 * it: these bytes are VRAM, held next to the frame's own accumulators and Anchor Point slots, and an
 * entry evicted between two frames costs one dispatch -- not the main-thread whole-canvas render an
 * evicted CPU entry costs.
 */
constexpr size_t PAINT_LAYER_EFFECT_GPU_CACHE_BYTES = size_t(512) * 1024 * 1024;

/** The live budget; only tests override it, to make eviction observable without 4K fixtures. */
static size_t g_paint_effect_gpu_cache_budget = PAINT_LAYER_EFFECT_GPU_CACHE_BYTES;

/** What one canvas costs the cache: the RGBA half float texture the acquire hook is to make. */
static size_t paint_layer_effect_gpu_entry_bytes(const int width, const int height)
{
  /* Four components of two bytes each. */
  return sizeof(int16_t) * 4 * int64_t(width) * height;
}

/** One cached GPU effect result, keyed exactly like the CPU buffer of the same effect. */
struct EffectGpuCacheEntry {
  EffectCacheKey key;
  int width = 0;
  int height = 0;
  /** The `gpu::Texture *` the hooks made, with the hooks that made it: releasing a texture needs
   * its owner's deallocator, and an entry outlives the call that acquired it. */
  void *resource = nullptr;
  PaintLayerEffectGpuHooks hooks;
  /** The maps the input was rendered from, with the versions they had. */
  Vector<PaintLayerEffectSource> synced;
  /** Whether a render has been stamped onto this entry yet. An input of no map at all (a row of flat
   * colour alone) publishes an empty #synced, so emptiness cannot stand for "never rendered". */
  bool published = false;
  /** Texels of the input that changed since #synced. A neighbourhood kernel will re-render just
   * that rectangle grown by its halo; until the region phase does, the whole texture is re-rendered
   * and the rectangle is only carried. */
  rcti dirty_region = {0, 0, 0, 0};
  size_t bytes = 0;
};

/** The cache owns references only; dropping an entry hands them back to the hooks. */
static PaintLayerEffectCacheStore<EffectGpuCacheEntry> g_paint_effect_gpu_cache;
/**
 * Guards the container and the byte count, as the CPU cache's mutex does. Unlike the CPU cache this
 * one is also held across the acquire hook, which makes a texture and cannot re-enter the cache; the
 * dispatch itself is never under it, so a slow GPU cannot block another thread's lookup.
 */
static std::mutex g_paint_effect_gpu_cache_mutex;

/** Hand the cache's reference back to whoever made the texture. */
static void paint_layer_effect_gpu_release(const PaintLayerEffectGpuHooks &hooks, void *resource)
{
  if (resource == nullptr) {
    return;
  }
  BLI_assert(hooks.release != nullptr);
  if (hooks.release != nullptr) {
    hooks.release(hooks.data, resource);
  }
}

/** The release the store calls before an entry leaves the container: hand the texture back. */
static void paint_layer_effect_gpu_entry_release(EffectGpuCacheEntry &entry)
{
  paint_layer_effect_gpu_release(entry.hooks, entry.resource);
}

PaintLayerEffectGpuState BKE_paint_layers_effect_gpu_cache_get(const Material &ma,
                                                              const MaterialPaintLayer &effect,
                                                              const int channel,
                                                              const int width,
                                                              const int height,
                                                              const Span<Image *> images,
                                                              const PaintLayerEffectGpuKeyExtras &key_extras,
                                                              const PaintLayerEffectGpuHooks &hooks,
                                                              rcti *r_dirty_region,
                                                              void **r_resource)
{
  if (r_resource != nullptr) {
    *r_resource = nullptr;
  }
  if (r_dirty_region != nullptr) {
    BLI_rcti_init(r_dirty_region, 0, 0, 0, 0);
  }
  if (width <= 0 || height <= 0 || hooks.acquire == nullptr) {
    return PaintLayerEffectGpuState::Failed;
  }
  const EffectCacheKey key{ma.id.session_uid,
                           effect.marker,
                           channel,
                           paint_layer_effect_hash(effect, channel),
                           key_extras};
  const size_t bytes = paint_layer_effect_gpu_entry_bytes(width, height);

  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  for (int i = 0; i < int(g_paint_effect_gpu_cache.entries.size()); i++) {
    EffectGpuCacheEntry &entry = g_paint_effect_gpu_cache.entries[i];
    if (!paint_layer_effect_key_equal(entry.key, key)) {
      continue;
    }
    if (entry.width != width || entry.height != height) {
      /* Another size is another texel space -- the noise's texel coordinates and a neighbourhood's
       * radius both read it -- and another texture, so the entry goes and a fresh one is made. */
      paint_layer_effect_gpu_entry_release(entry);
      g_paint_effect_gpu_cache.total_bytes -= entry.bytes;
      g_paint_effect_gpu_cache.entries.remove(i);
      break;
    }
    if (entry.published) {
      rcti region;
      const ePixelChange change = anchor_slice_changes(entry.synced, images, region);
      if (change == ePixelChange::None) {
        if (r_resource != nullptr) {
          *r_resource = entry.resource;
        }
        g_paint_effect_gpu_cache.touch(i);
        return PaintLayerEffectGpuState::Hit;
      }
      /* A change that is not one rectangle -- a reload, a new map in the set, a resized one -- has no
       * region to name, and the whole canvas is the only safe claim. A rectangle is over-approximated
       * freely, so a caller may grow it by its halo. */
      if (change == ePixelChange::Full) {
        BLI_rcti_init(&region, 0, width, 0, height);
        entry.dirty_region = region;
      }
      else if (BLI_rcti_is_empty(&entry.dirty_region)) {
        entry.dirty_region = region;
      }
      else {
        BLI_rcti_union(&entry.dirty_region, &region);
      }
      if (r_dirty_region != nullptr) {
        *r_dirty_region = entry.dirty_region;
      }
      if (r_resource != nullptr) {
        *r_resource = entry.resource;
      }
      return PaintLayerEffectGpuState::Render;
    }
    /* Never published: the texture is there but says nothing yet. */
    BLI_rcti_init(&entry.dirty_region, 0, width, 0, height);
    if (r_dirty_region != nullptr) {
      *r_dirty_region = entry.dirty_region;
    }
    if (r_resource != nullptr) {
      *r_resource = entry.resource;
    }
    return PaintLayerEffectGpuState::Render;
  }

  void *resource = hooks.acquire(hooks.data, width, height);
  if (resource == nullptr) {
    return PaintLayerEffectGpuState::Failed;
  }
  g_paint_effect_gpu_cache.drop_rekeyed(key, paint_layer_effect_gpu_entry_release);
  /* An entry larger than the whole budget is still inserted rather than handed out uncached: the
   * cache must own the texture to release it, and a caller that keeps it past this frame holds a
   * reference of its own, so the budget reads as temporarily exceeded -- the same allowance the CPU
   * cache makes for a buffer a description still references. */
  g_paint_effect_gpu_cache.enforce_budget(bytes, g_paint_effect_gpu_cache_budget,
                                          paint_layer_effect_gpu_entry_release);
  EffectGpuCacheEntry entry;
  entry.key = key;
  entry.width = width;
  entry.height = height;
  entry.resource = resource;
  entry.hooks = hooks;
  entry.bytes = bytes;
  BLI_rcti_init(&entry.dirty_region, 0, width, 0, height);
  g_paint_effect_gpu_cache.total_bytes += bytes;
  g_paint_effect_gpu_cache.entries.append(std::move(entry));
  if (r_dirty_region != nullptr) {
    BLI_rcti_init(r_dirty_region, 0, width, 0, height);
  }
  if (r_resource != nullptr) {
    *r_resource = resource;
  }
  return PaintLayerEffectGpuState::Render;
}

void BKE_paint_layers_effect_gpu_cache_publish(const Material &ma,
                                               const MaterialPaintLayer &effect,
                                               const int channel,
                                               const int width,
                                               const int height,
                                               const Span<Image *> images,
                                               const PaintLayerEffectGpuKeyExtras &key_extras)
{
  const EffectCacheKey key{ma.id.session_uid,
                           effect.marker,
                           channel,
                           paint_layer_effect_hash(effect, channel),
                           key_extras};
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  for (EffectGpuCacheEntry &entry : g_paint_effect_gpu_cache.entries) {
    if (!paint_layer_effect_key_equal(entry.key, key) || entry.width != width ||
        entry.height != height)
    {
      continue;
    }
    /* Taken after the render, as the CPU cache takes them: an edit that landed while the pass ran is
     * newer than this entry and costs one more update rather than a stale result that never clears.
     */
    entry.synced = anchor_slice_sources_snapshot(images);
    entry.published = true;
    BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
    return;
  }
  /* The entry was evicted or dropped while the frame was rendering into its texture: nothing to
   * stamp, and the next frame asks again. */
}

void BKE_paint_layers_effect_gpu_cache_free_all()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  g_paint_effect_gpu_cache.free_all(paint_layer_effect_gpu_entry_release);
}

void BKE_paint_layers_effect_gpu_cache_free_material(const Material &ma)
{
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  g_paint_effect_gpu_cache.free_material(ma.id.session_uid, paint_layer_effect_gpu_entry_release);
}

size_t paint_layer_effect_gpu_cache_bytes()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  return g_paint_effect_gpu_cache.total_bytes;
}

int paint_layer_effect_gpu_cache_entry_count()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  return int(g_paint_effect_gpu_cache.entries.size());
}

void paint_layer_effect_gpu_cache_set_budget(const size_t bytes)
{
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  g_paint_effect_gpu_cache_budget = bytes;
}

size_t paint_layer_effect_gpu_cache_budget()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_gpu_cache_mutex);
  return g_paint_effect_gpu_cache_budget;
}

size_t BKE_paint_layers_effect_gpu_cache_budget()
{
  return paint_layer_effect_gpu_cache_budget();
}

/** \} */

}  // namespace blender
