/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * What the CPU effect cache (`paint_layers_composite.cc`) and the GPU effect cache
 * (`paint_layers_effect_gpu_cache.cc`) share: the key of one cached effect, the staleness snapshot
 * of the maps an input renders from, and the LRU container both caches keep their entries in. The
 * container is a template on the entry, and every operation that has to let go of an entry takes
 * the release callable the entry kind needs -- the CPU one releases nothing (the buffer dies with
 * the entry), the GPU one hands the texture back to the hooks that made it.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"

#include "DNA_image_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Effect staleness
 * \{ */

/**
 * The maps of an input, once each, with the version of their pixels now. An edit that lands after
 * this is read by #anchor_slice_changes as newer than whatever was stamped from the snapshot.
 */
inline Vector<PaintLayerEffectSource> anchor_slice_sources_snapshot(const Span<Image *> images)
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
inline ePixelChange anchor_slice_changes(const Span<PaintLayerEffectSource> synced,
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
 * One cached effect: the result, keyed by the material, the effect, the channel and the
 * parameters. Both effect caches key their entries on it.
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
  /** The effect kind and its parameters; the input's pixels are the sources' versions below. */
  uint64_t hash = 0;
  /**
   * The GPU-only key halves. The CPU cache leaves both at zero, so they cannot split a CPU key; the
   * GPU key folds in the chain hash (the effects that precede this one in the row, whose results
   * feed this effect's input) and the input scalar hash (the input's non-map scalar content -- a
   * flat colour, an adjustment, a filter's parameters -- that no map version carries). A row of one
   * effect with a plain map input has both zero, exactly the CPU picture.
   */
  PaintLayerEffectGpuKeyExtras gpu_extras;
};

/** Whether \a a and \a b name the same effect instance, whatever its parameters say. */
inline bool paint_layer_effect_key_same_effect(const EffectCacheKey &a, const EffectCacheKey &b)
{
  return a.material_uid == b.material_uid && BLI_uuid_equal(a.effect_marker, b.effect_marker) &&
         a.channel == b.channel;
}

inline bool paint_layer_effect_key_equal(const EffectCacheKey &a, const EffectCacheKey &b)
{
  return paint_layer_effect_key_same_effect(a, b) && a.hash == b.hash &&
         a.gpu_extras == b.gpu_extras;
}

/**
 * A description hash of the effect: the kind and the parameters in its schema order. The input's
 * pixels are not part of it -- the sources' versions stand in for those -- so an edit of a value
 * moves it and an edit of a map does not. Defined in `paint_layers_composite.cc`, beside the
 * schema readers it folds.
 */
uint64_t paint_layer_effect_hash(const MaterialPaintLayer &effect, int channel);

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
   * Drop the entry of the same effect instance with other parameters: the description can only
   * ever ask for the parameters the effect carries now, so an old result is memory held for
   * nothing. A frame still laying it keeps its own reference, exactly as it does against the
   * budget eviction.
   */
  template<typename Release> void drop_rekeyed(const EffectCacheKey &key, Release &&release)
  {
    for (int i = int(entries.size()) - 1; i >= 0; i--) {
      if (paint_layer_effect_key_same_effect(entries[i].key, key)) {
        release(entries[i]);
        total_bytes -= entries[i].bytes;
        entries.remove(i);
        break;
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

}  // namespace blender
