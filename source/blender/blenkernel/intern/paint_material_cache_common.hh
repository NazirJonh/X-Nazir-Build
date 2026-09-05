/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * What the two PBR paint caches -- the per-channel composite and the Combined preview -- have in
 * common.
 *
 * They are separate caches on purpose: one holds byte, display-referred channel buffers keyed by
 * material and channel, the other one scene-linear float preview keyed by material alone, and the
 * second is derived from the first. But they are the same *kind* of cache, and were written twice:
 * the same owning handles, the same least-recently-used eviction, and the same subscribe-and-poll
 * against each source image's partial-update log. Kept here so that a change to how an edit is
 * discovered is made once rather than in two places that have to be noticed together.
 *
 * Internal to blenkernel; nothing outside the two implementations should include this.
 */

#include <cstdint>
#include <memory>

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "IMB_imbuf.hh"

/* For #PartialUpdateUser and the create/free pair, which live in `blender` rather than in the
 * `bke::image::partial_update` namespace the collection API is in. */
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"

namespace blender::bke::paint_material_cache {

/**
 * Owning handles for the two resources a cache entry holds.
 *
 * By value rather than raw pointers freed by hand, because an entry is destroyed from four places
 * -- eviction, a failed evaluation, a per-material drop and the teardown -- and a path added later
 * that forgets is a leak nothing reports.
 */
struct ImBufDeleter {
  void operator()(ImBuf *ibuf) const
  {
    IMB_freeImBuf(ibuf);
  }
};
using ImBufPtr = std::unique_ptr<ImBuf, ImBufDeleter>;

struct PartialUpdateUserDeleter {
  void operator()(PartialUpdateUser *user) const
  {
    BKE_image_partial_update_free(user);
  }
};
using PartialUpdateUserPtr = std::unique_ptr<PartialUpdateUser, PartialUpdateUserDeleter>;

/** One image's worth of subscriptions, keyed by #ID.session_uid. Never holds an #Image pointer:
 * the images arrive with every cache call, so a poll always has a fresh one, and a cache outliving
 * an ID it pointed at would be a crash rather than a stale pixel. */
using SubscriptionMap = Map<uint32_t, PartialUpdateUserPtr>;

/** What one poll of a cache's source images found. */
struct PollResult {
  /** Nothing about the previous contents can be trusted; recompute the whole buffer. */
  bool needs_full_update = false;
  /** Union of every rectangle reported, already in the cache's own coordinates. Empty when none. */
  rcti changed_region = {0, 0, 0, 0};
};

/**
 * Subscribe \a subscriptions to every image in \a images, drop the ones no longer needed, and
 * report what the images say changed since the last poll.
 *
 * Polled rather than reported, which is the whole reason both caches work this way: a caller that
 * edits an image has nothing to tell them, and -- the point -- a caller that tags the image ID for
 * unrelated reasons can no longer turn a known rectangle into "the whole canvas". A paint stroke
 * does exactly that on every dab, and the blanket answer used to win over the precise one.
 *
 * A brand new subscription reports #PollResult.needs_full_update, which is right: nothing of that
 * image has been folded into the cache yet.
 *
 * \param images: may contain nulls and duplicates. One subscription answers for every use of an
 *                image, so polling a duplicate would hand the second poll an empty changeset.
 * \param to_cache_space: maps a rectangle from the image's texels into the cache's own
 *                        coordinates, for a cache whose buffer is not the canvas -- the Combined
 *                        preview is shaded at a fraction of it. Empty for a cache that stores
 *                        texels, which is the identity.
 */
PollResult subscriptions_poll(SubscriptionMap &subscriptions,
                              Span<Image *> images,
                              FunctionRef<rcti(const rcti &)> to_cache_space = {});

/**
 * Evict least recently used entries until the cache fits \a budget_bytes, never the one at \a keep.
 *
 * \a Entry has to carry a monotonic `last_use`. The key is copied before the removal rather than
 * pointed at inside the map, so nothing here depends on how #Map lays its slots out.
 *
 * The budget only exists to bound a pathological case: in practice either cache holds a handful of
 * entries and this loop never runs.
 */
template<typename Key, typename Entry, typename SizeFn>
void enforce_budget(Map<Key, Entry> &entries,
                    const Key &keep,
                    const int64_t budget_bytes,
                    SizeFn size_fn)
{
  int64_t total = 0;
  for (const Entry &entry : entries.values()) {
    total += size_fn(entry);
  }
  while (total > budget_bytes) {
    Key oldest_key{};
    bool found = false;
    int64_t oldest_use = INT64_MAX;
    for (const auto item : entries.items()) {
      if (item.key == keep) {
        continue;
      }
      if (item.value.last_use < oldest_use) {
        oldest_use = item.value.last_use;
        oldest_key = item.key;
        found = true;
      }
    }
    if (!found) {
      break;
    }
    total -= size_fn(entries.lookup(oldest_key));
    entries.remove(oldest_key);
  }
}

}  // namespace blender::bke::paint_material_cache
