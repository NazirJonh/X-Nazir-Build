/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "paint_material_cache_common.hh"

#include <cmath>

#include "BLI_math_base.h"
#include "BLI_set.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"

namespace blender::bke::paint_material_cache {

using namespace image::partial_update;

int2 octave_size_for(const int2 canvas, const int2 max_output, const int2 previous)
{
  BLI_assert(canvas.x > 0 && canvas.y > 0);
  int2 size = canvas;
  const bool previous_usable = previous.x > 0 && previous.y > 0 &&
                               canvas.x % previous.x == 0 && canvas.y % previous.y == 0 &&
                               is_power_of_2_i(canvas.x / previous.x) &&
                               is_power_of_2_i(canvas.y / previous.y);
  if (previous_usable) {
    size = previous;
  }
  if (max_output.x <= 0 || max_output.y <= 0) {
    return size;
  }
  /* An octave that no longer covers what has to be shown is not a starting point: climb back to the
   * canvas and let the halving below settle on the right one. */
  if (previous_usable && (size.x < max_output.x || size.y < max_output.y)) {
    size = canvas;
  }
  constexpr float octave_margin = 1.4f;
  while (size.x % 2 == 0 && size.y % 2 == 0 &&
         float(size.x / 2) >= float(max_output.x) * octave_margin &&
         float(size.y / 2) >= float(max_output.y) * octave_margin)
  {
    size.x /= 2;
    size.y /= 2;
  }
  return size;
}

rcti rect_scale(const rcti &rect, const int2 from, const int2 to)
{
  if (from == to || BLI_rcti_is_empty(&rect)) {
    return rect;
  }
  BLI_assert(from.x > 0 && from.y > 0);
  const float scale_x = float(to.x) / float(from.x);
  const float scale_y = float(to.y) / float(from.y);
  rcti scaled;
  BLI_rcti_init(&scaled,
                int(floorf(float(rect.xmin) * scale_x)),
                int(ceilf(float(rect.xmax) * scale_x)),
                int(floorf(float(rect.ymin) * scale_y)),
                int(ceilf(float(rect.ymax) * scale_y)));
  return scaled;
}

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
