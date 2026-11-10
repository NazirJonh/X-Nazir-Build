/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The CPU-composite cache of a source-graph material, as pure decisions: how a landed bake is
 * applied to the material (its maps, its claim hash, the stale mark it leaves), and whether a bake
 * is due at all. The editors keep the window-manager half -- the jobs, the buffers, the debounce
 * -- and ask here; the answers are testable without one.
 */

#include <algorithm>

#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_source_graph.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Source-graph cache images and the conversion fill
 * \{ */

Image *BKE_paint_layers_source_cache_image_get(const Material &ma, const int channel)
{
  if ((ma.paint_layers_flag & MA_PAINT_LAYERS_SOURCE_GRAPH) == 0) {
    return nullptr;
  }
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return nullptr;
  }
  /* Normal and Height are never taken from the graph (#bke::paint_layers::source_channel_adoptable;
   * the author chains stay on the Principled), and a channel without a Principled socket (Custom,
   * AO) has nothing the graph could feed. */
  if (!bke::paint_layers::source_channel_adoptable(channel)) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return nullptr;
  }
  /* Stale is still served: the hash is the invalidation's question, not the reader's. */
  return ma.paint_layers_source_images[channel];
}

/** Whether \a image's pixels can not have survived a file load. */
static bool source_cache_image_lost_pixels(const Image &image)
{
  /* The cache is deliberately not packed (see #BKE_paint_layers_maps_collect): a generated image
   * with no packed file carries no pixels across a load, and nothing will bring them back but a
   * re-bake. */
  return image.source == IMA_SRC_GENERATED && !BKE_image_has_packedfile(&image);
}

bool BKE_paint_layers_source_cache_validate(Material &ma, const bool check_unpacked)
{
  if (!BKE_paint_layers_source_graph_get(ma) || !BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  const uint64_t live = BKE_paint_layers_source_channel_graph_hash_cached(ma);
  bool stale = live != ma.paint_layers_source_hash;
  /* A stored hash is a bake's claim, and a landing stamps it whatever it baked -- zero when the
   * graph feeds nothing. So a nonzero hash over an empty array means the images were taken away
   * under the material (a Python removal remaps the pointers): stale, and the next bake remakes
   * them. A hash over a partly empty array heals the missing channel at the same next bake; until
   * then the helper serves the constant for it. */
  if (!stale && ma.paint_layers_source_hash != 0) {
    bool any_image = false;
    for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      any_image |= ma.paint_layers_source_images[i] != nullptr;
    }
    stale = !any_image;
  }
  if (check_unpacked) {
    for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      Image *image = ma.paint_layers_source_images[i];
      if (image == nullptr || !source_cache_image_lost_pixels(*image)) {
        continue;
      }
      /* The pixels are gone for good: hand the empty image back (it goes the way of any orphan at
       * the next purge) rather than serve a blank bottom under every row until the re-bake. The
       * claim goes with it -- a fresh bake re-derives the hash from scratch. */
      id_us_min(&image->id);
      ma.paint_layers_source_images[i] = nullptr;
      stale = true;
    }
    if (stale) {
      ma.paint_layers_source_hash = 0;
    }
  }
  SET_FLAG_FROM_TEST(ma.paint_layers_flag, stale, MA_PAINT_LAYERS_SOURCE_STALE);
  return stale;
}

void BKE_paint_layers_source_cache_validate_all(Main &bmain)
{
  for (Material &ma : bmain.materials) {
    BKE_paint_layers_source_cache_validate(ma, true);
  }
}

MaterialPaintLayer *BKE_paint_layers_conversion_fill_find(Material &ma)
{
  auto *bottom = static_cast<MaterialPaintLayer *>(ma.paint_layers.first);
  if (bottom != nullptr && bottom->source == MA_PAINT_LAYER_SOURCE_CONSTANT &&
      (bottom->flag & MA_PAINT_LAYER_CONVERSION_FILL) != 0)
  {
    return bottom;
  }
  return nullptr;
}

void BKE_paint_layers_conversion_fill_channels_align(Material &ma,
                                                     MaterialPaintLayer &fill,
                                                     const PaintChannelMask baked_mask)
{
  if (!paint_layer_owned(ma, &fill)) {
    return;
  }
  /* Off first: a re-bake over a smaller set switches the channels the bake left out instead of
   * leaving them composited. The records keep their maps and values, so aligning back with the
   * old mask restores the channels untouched. */
  for (const int i : IndexRange(fill.channels_num)) {
    const eMaterialPaintChannel channel = eMaterialPaintChannel(int(fill.channels[i].channel));
    if ((baked_mask & (uint32_t(1) << int(channel))) == 0) {
      BKE_paint_layers_channel_set_enabled(ma, &fill, channel, false);
    }
  }
  /* The channels of this bake are on: a record the mask names may sit disabled from an earlier
   * bake, and the maps the bake links next have to land on live channels. */
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if ((baked_mask & (uint32_t(1) << i)) == 0) {
      continue;
    }
    const eMaterialPaintChannel channel = eMaterialPaintChannel(i);
    if (paint_layer_channel_find(fill, channel) == nullptr) {
      /* The record arrives enabled and without a map; the bake links its own right after. */
      BKE_paint_layers_channel_add(ma, &fill, channel);
    }
    BKE_paint_layers_channel_set_enabled(ma, &fill, channel, true);
  }
}

bool BKE_paint_layers_source_cache_stale_get(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_SOURCE_STALE) != 0;
}

PaintLayersSourceCacheLandState BKE_paint_layers_source_cache_land_apply(
    Material &ma,
    const uint64_t hash,
    const Span<eMaterialPaintChannel> requested,
    const Span<PaintLayersSourceCacheLandedChannel> landed)
{
  for (const PaintLayersSourceCacheLandedChannel &entry : landed) {
    const int index = int(entry.channel);
    BLI_assert(index >= 0 && index < PAINT_MATERIAL_CHANNEL_NUM);
    if (ma.paint_layers_source_images[index] != entry.image) {
      if (Image *old = ma.paint_layers_source_images[index]) {
        id_us_min(&old->id);
      }
      /* A newly created image holds the one user its owner is. */
      ma.paint_layers_source_images[index] = entry.image;
    }
  }
  /* The claim stamps only for a bake that landed every channel it was asked for: the validation
   * reads the whole graph, so a claim over a partial landing would clear the stale mark over
   * channels whose pixels never arrived, and their old pixels (or the constant) would read as
   * fresh. Left unstamped, the mark stays on until a bake lands all of it. */
  const bool all_landed =
      requested.size() == landed.size() &&
      std::all_of(requested.begin(), requested.end(), [&](const eMaterialPaintChannel channel) {
        return std::any_of(landed.begin(),
                           landed.end(),
                           [&](const PaintLayersSourceCacheLandedChannel &entry) {
                             return entry.channel == channel;
                           });
      });
  if (all_landed) {
    ma.paint_layers_source_hash = hash;
  }
  const bool stale = BKE_paint_layers_source_cache_validate(ma);
  if (!all_landed) {
    return PaintLayersSourceCacheLandState::Partial;
  }
  return stale ? PaintLayersSourceCacheLandState::Stale : PaintLayersSourceCacheLandState::Fresh;
}

/** The graph hashes a cache bake already failed to land at, by `ID::session_uid`. Runtime only
 * (the claim of a failure says nothing a file could want back), main thread only. An entry dies
 * with its material (#BKE_paint_layers_source_cache_failure_forget, the free-data call): a leaked
 * entry would both hold its hash forever and answer a session uid reuse that arrived later. */
static Map<uint32_t, uint64_t> &source_cache_failures()
{
  static Map<uint32_t, uint64_t> failures;
  return failures;
}

void BKE_paint_layers_source_cache_failure_remember(const Material &ma, const uint64_t hash)
{
  source_cache_failures().add_overwrite(ma.id.session_uid, hash);
}

void BKE_paint_layers_source_cache_failure_forget(const Material &ma)
{
  source_cache_failures().remove(ma.id.session_uid);
}

void BKE_paint_layers_source_cache_failure_free_all()
{
  source_cache_failures() = Map<uint32_t, uint64_t>();
}

bool BKE_paint_layers_source_cache_failure_blocks(const Material &ma)
{
  const uint64_t *failed = source_cache_failures().lookup_ptr(ma.id.session_uid);
  return failed != nullptr && *failed == BKE_paint_layers_source_channel_graph_hash_cached(ma);
}

bool BKE_paint_layers_source_cache_bake_due(const Material &ma, const bool manual)
{
  if (!BKE_paint_layers_source_cache_stale_get(ma)) {
    return false;
  }
  PaintLayersSourceChannelFeed feeds[PAINT_MATERIAL_CHANNEL_NUM];
  if (BKE_paint_layers_source_graph_feeds_find(
          ma, MutableSpan<PaintLayersSourceChannelFeed>(feeds, PAINT_MATERIAL_CHANNEL_NUM)) == 0)
  {
    return false;
  }
  /* A bake that already failed to land this graph is not retried on its own: the auto path would
   * bake the same failure every sweep. The user asking again (the manual update) is what gets the
   * graph past it, and the graph moving on makes the memory moot by no longer matching. */
  if (!manual && BKE_paint_layers_source_cache_failure_blocks(ma)) {
    return false;
  }
  return true;
}

/** \} */

}  // namespace blender
