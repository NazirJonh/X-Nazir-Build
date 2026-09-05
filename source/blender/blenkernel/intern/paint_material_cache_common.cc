/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "paint_material_cache_common.hh"

#include "BLI_set.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"

namespace blender::bke::paint_material_cache {

using namespace image::partial_update;

PollResult subscriptions_poll(SubscriptionMap &subscriptions,
                              const Span<Image *> images,
                              const FunctionRef<rcti(const rcti &)> to_cache_space)
{
  PollResult result;
  BLI_rcti_init(&result.changed_region, 0, 0, 0, 0);

  Set<uint32_t> live;
  for (Image *image : images) {
    if (image == nullptr) {
      continue;
    }
    const uint32_t session_uid = image->id.session_uid;
    if (!live.add(session_uid)) {
      /* The same image can feed several layers or channels; one subscription answers for all of
       * them, and polling it twice would hand the second poll an empty changeset. */
      continue;
    }

    PartialUpdateUser *user =
        subscriptions
            .lookup_or_add_cb(session_uid,
                              [&]() { return PartialUpdateUserPtr(
                                  BKE_image_partial_update_create(image)); })
            .get();

    switch (BKE_image_partial_update_collect_changes(image, user)) {
      case ePartialUpdateCollectResult::FullUpdateNeeded:
        /* A brand new subscription lands here too, which is right: nothing of this image has been
         * folded into the cache yet. */
        result.needs_full_update = true;
        break;
      case ePartialUpdateCollectResult::NoChangesDetected:
        break;
      case ePartialUpdateCollectResult::PartialChangesDetected: {
        PartialUpdateRegion change;
        while (BKE_image_partial_update_get_next_change(user, &change) ==
               ePartialUpdateIterResult::ChangeAvailable)
        {
          /* Neither cache is tiled, so a change reported for any tile but the first would land at
           * the wrong place in a single-canvas buffer. Give up precision rather than put pixels
           * somewhere they do not belong. */
          if (change.tile_number != 1001) {
            result.needs_full_update = true;
            break;
          }
          const rcti region = to_cache_space ? to_cache_space(change.region) : change.region;
          if (BLI_rcti_is_empty(&result.changed_region)) {
            result.changed_region = region;
          }
          else {
            BLI_rcti_union(&result.changed_region, &region);
          }
        }
        break;
      }
    }
  }

  /* Something that stopped being read stops being watched, or the cache keeps an allocation and a
   * poll per frame for an image it no longer looks at. */
  Vector<uint32_t> stale;
  for (const uint32_t session_uid : subscriptions.keys()) {
    if (!live.contains(session_uid)) {
      stale.append(session_uid);
    }
  }
  for (const uint32_t session_uid : stale) {
    subscriptions.remove(session_uid);
  }

  return result;
}

}  // namespace blender::bke::paint_material_cache
