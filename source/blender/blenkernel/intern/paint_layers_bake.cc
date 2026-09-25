/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers.hh: the bake side of a layered material -- the cache key, the service maps
 * the generator and the CPU substitute, and the worker jobs that fill them. The description and
 * its edits live in `paint_layers.cc`; the helpers both files need are in `paint_layers_intern.hh`.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_debug.hh"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_color_types.h"
#include "DNA_genfile.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_intern.hh"

namespace blender {
int BKE_paint_layers_bake_mode_get(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr ? int(layer.bake->mode) : int(MA_PAINT_LAYER_BAKE_AUTO);
}

bool BKE_paint_layers_bake_mode_set(Material &ma, MaterialPaintLayer &layer, const int mode)
{
  if (mode < MA_PAINT_LAYER_BAKE_AUTO || mode > MA_PAINT_LAYER_BAKE_NEVER ||
      paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr)
  {
    return false;
  }
  BKE_paint_layers_bake_ensure(layer)->mode = int8_t(mode);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

MaterialPaintLayerBake *BKE_paint_layers_bake_ensure(MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    layer.bake = MEM_new<MaterialPaintLayerBake>(__func__);
  }
  return layer.bake;
}

int BKE_paint_layers_bake_size_get(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr ? layer.bake->size : 0;
}

bool BKE_paint_layers_bake_size_set(Material &ma, MaterialPaintLayer &layer, const int size)
{
  if (size <= 0 || paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr) {
    return false;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(layer);
  bake->size = size;
  /* A different resolution invalidates whatever was baked before it. */
  bake->hash[0] = 0;
  bake->hash[1] = 0;
  BKE_paint_layers_tag_edited(ma);
  return true;
}

void BKE_paint_layers_bake_request(MaterialPaintLayer &layer)
{
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(layer);
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

bool BKE_paint_layers_material_bake_due_get(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_MATERIAL_BAKE_DUE) != 0;
}

void BKE_paint_layers_material_bake_due_clear(Material &ma)
{
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_MATERIAL_BAKE_DUE;
}

bool BKE_paint_layers_bake_clear(Material &ma, MaterialPaintLayer &layer)
{
  if (paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr) {
    return false;
  }
  MEM_SAFE_DELETE(layer.bake);
  BKE_paint_layers_tag_edited(ma);
  return true;
}

bool BKE_paint_layers_bake_set_map(Material &ma,
                                   MaterialPaintLayer &layer,
                                   const int channel,
                                   Image *image)
{
  if (channel < -1 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr)
  {
    return false;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(layer);
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

void BKE_paint_layers_bake_finalize(Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr) {
    return;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  layer.bake->hash[0] = hash[0];
  layer.bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
}

void BKE_paint_layers_material_bake_apply(Main &bmain,
                                          Material &ma,
                                          MaterialPaintLayer &layer,
                                          const int size,
                                          const Span<int> channels,
                                          const Span<Image *> images)
{
  if (paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr || size <= 0 ||
      channels.size() != images.size())
  {
    return;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(layer);
  bake->size = size;
  Image *alpha_coverage = nullptr;
  for (const int64_t i : channels.index_range()) {
    if (channels[i] == PAINT_MATERIAL_CHANNEL_ALPHA) {
      /* The source's transparency is the row's coverage, not a channel of its own. */
      alpha_coverage = images[i];
      continue;
    }
    BKE_paint_layers_bake_set_map(ma, layer, channels[i], images[i]);
  }
  if (alpha_coverage != nullptr) {
    BKE_paint_layers_bake_set_map(ma, layer, -1, alpha_coverage);
  }
  else if (bake->coverage == nullptr) {
    char name[192];
    SNPRINTF(name, "%s Coverage Bake", layer.name[0] != '\0' ? layer.name : "Layer");
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    Image *coverage = BKE_image_add_generated(
        &bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, white, false, true, false);
    if (coverage != nullptr) {
      coverage->alpha_mode = IMA_ALPHA_STRAIGHT;
      BKE_paint_layers_bake_set_map(ma, layer, -1, coverage);
    }
  }
  /* Not finalised here: a material bake hands its target images over before the worker has
   * rendered anything into them (#material_bake_to_images calls #before_render ahead of the job so
   * the generator can wire the new images in while the job runs). Stamping the hash at that point
   * would mark the row valid over pixels that do not exist yet; if the job is then cancelled or
   * fails, nothing ever rewrites them, and the row would show blank/stale maps as if baked. The
   * caller finalises once the render actually lands -- see #material_bake_rows_finalize. */
}

static Image *bake_service_image(Main &bmain,
                                 MaterialPaintLayer &layer,
                                 int channel,
                                 int size,
                                 bool is_color);
static void bake_write_image(Image &image,
                             const float *values,
                             const bool is_color,
                             const int *rect,
                             const bool use_alpha = true);
static void bake_write_coverage_image(Image &image, const float *coverage, const int *rect);

void BKE_paint_layers_custom_bake_apply(Main &bmain,
                                        Material &ma,
                                        MaterialPaintLayer &layer,
                                        const int size,
                                        const Span<int> channels,
                                        const Span<const ImBuf *> color_buffers,
                                        const ImBuf *coverage_buffer)
{
  if (size <= 0 || channels.size() != color_buffers.size() ||
      paint_layer_owner_list(&ma.paint_layers, &layer) == nullptr)
  {
    return;
  }
  bool any = false;
  for (const int64_t i : channels.index_range()) {
    const int channel = channels[i];
    const ImBuf *buffer = color_buffers[i];
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || buffer == nullptr ||
        buffer->float_data() == nullptr)
    {
      continue;
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    Image *color_image = bake_service_image(bmain, layer, channel, size, info.is_color);
    if (color_image == nullptr) {
      continue;
    }
    /* The custom render's alpha is the row's coverage, written to the coverage map below; it is
     * not a content alpha, so the colour map stays opaque. */
    bake_write_image(*color_image, buffer->float_data(), info.is_color, nullptr, false);
    any = true;
  }
  if (!any) {
    return;
  }
  Image *coverage_image = bake_service_image(bmain, layer, -1, size, false);
  if (coverage_image != nullptr) {
    const int64_t texel_num = int64_t(size) * size;
    Vector<float> gray(texel_num, 1.0f);
    if (coverage_buffer != nullptr && coverage_buffer->float_data() != nullptr) {
      const float *src = coverage_buffer->float_data();
      for (const int64_t texel : IndexRange(texel_num)) {
        gray[texel] = src[texel * 4 + 3];
      }
    }
    bake_write_coverage_image(*coverage_image, gray.data(), nullptr);
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(layer, hash);
  layer.bake->hash[0] = hash[0];
  layer.bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
}

/* --- Runtime partial-update subscription of the bake cache. Not saved; a load starts empty. --- */

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
struct BakeMaterialSubscription {
  Vector<BakeLayerSubscription> layers;
};

static Map<uint32_t, BakeMaterialSubscription> &bake_subscriptions()
{
  static Map<uint32_t, BakeMaterialSubscription> map;
  return map;
}

static void bake_collect_source_images(const MaterialPaintLayer &layer, Vector<Image *> &r_images)
{
  auto add = [&](Image *image) {
    if (image != nullptr && !r_images.contains(image)) {
      r_images.append(image);
    }
  };
  for (int i = 0; i < layer.channels_num; i++) {
    add(layer.channels[i].image);
  }
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    bake_collect_source_images(effect, r_images);
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    bake_collect_source_images(mask_item, r_images);
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    bake_collect_source_images(child, r_images);
  }
}

/**
 * Drain the first answer of a freshly created partial-update user.
 *
 * A new user starts at changeset -1 and its first #BKE_image_partial_update_collect_changes answers
 * `FullUpdateNeeded` because the changeset can no longer be constructed
 * (`image_partial_update.cc:485-488`, `:546-559`). Read as a change, that made every row invalid
 * right after its own bake subscribed it, and the planner re-baked in a loop. Collecting once here
 * moves the user's baseline to the image's current changeset, so the next collect reports only
 * edits made after this subscribe.
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
  BakeMaterialSubscription &sub = bake_subscriptions().lookup_or_add(
      ma.id.session_uid, BakeMaterialSubscription{});
  for (int i = 0; i < sub.layers.size();) {
    if (BLI_uuid_equal(sub.layers[i].marker, layer.marker)) {
      for (BakeSubscriptionSource &src : sub.layers[i].sources) {
        BKE_image_partial_update_free(src.user);
      }
      sub.layers.remove(i);
    }
    else {
      i++;
    }
  }
  BakeLayerSubscription entry;
  entry.marker = layer.marker;
  /* A Material row's bake is its source material rendered into maps; the row's own channel images,
   * effects and masks are composited over those maps live and never enter the render. Subscribing
   * to them would invalidate the source maps on every mask stroke, so the row has no sources. */
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    Vector<Image *> images;
    bake_collect_source_images(layer, images);
    for (Image *image : images) {
      PartialUpdateUser *user = BKE_image_partial_update_create(image);
      bake_subscription_user_drain(*image, user);
      entry.sources.append({image, user});
    }
  }
  sub.layers.append(std::move(entry));
  /* Writing a bake changes whether the row is substituted, which is topology: rebuild. */
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
}

#if PAINT_LAYERS_DEBUG_LOG
/** The name of the row \a marker belongs to, or a placeholder. For the diagnostic only. */
static const char *bake_subscription_row_name(const Material &ma, const bUUID &marker)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (BLI_uuid_equal(layer->marker, marker)) {
      return layer->name;
    }
  }
  return "<unknown>";
}
#endif

void BKE_paint_layers_bake_notice_changes(Material &ma)
{
  BakeMaterialSubscription *sub = bake_subscriptions().lookup_ptr(ma.id.session_uid);
  if (sub == nullptr) {
    return;
  }
  bool any = false;
  for (BakeLayerSubscription &entry : sub->layers) {
    for (BakeSubscriptionSource &src : entry.sources) {
      const auto result = bke::image::partial_update::BKE_image_partial_update_collect_changes(
          src.image, src.user);
      if (result == bke::image::partial_update::ePartialUpdateCollectResult::FullUpdateNeeded) {
        PL_DEBUG_PRINTF("paint layers bake: subscription changed row='%s' image='%s' result=Full\n",
                        bake_subscription_row_name(ma, entry.marker),
                        src.image->id.name + 2);
        /* No rectangle to trust: the planner re-bakes the whole row. */
        entry.changed = true;
        entry.has_region = false;
        any = true;
      }
      else if (result ==
               bke::image::partial_update::ePartialUpdateCollectResult::PartialChangesDetected)
      {
        PL_DEBUG_PRINTF(
            "paint layers bake: subscription changed row='%s' image='%s' result=Partial\n",
            bake_subscription_row_name(ma, entry.marker),
            src.image->id.name + 2);
        entry.changed = true;
        any = true;
        bke::image::partial_update::PartialUpdateRegion region;
        while (bke::image::partial_update::BKE_image_partial_update_get_next_change(
                   src.user, &region) ==
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
  }
  if (any) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  }
}

void BKE_paint_layers_bake_runtime_free(Material &ma)
{
  BakeMaterialSubscription *sub = bake_subscriptions().lookup_ptr(ma.id.session_uid);
  if (sub == nullptr) {
    return;
  }
  for (BakeLayerSubscription &entry : sub->layers) {
    for (BakeSubscriptionSource &src : entry.sources) {
      BKE_image_partial_update_free(src.user);
    }
  }
  bake_subscriptions().remove(ma.id.session_uid);
}

bool BKE_paint_layers_bake_changed_region(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int r_region[4])
{
  BakeMaterialSubscription *sub = bake_subscriptions().lookup_ptr(ma.id.session_uid);
  if (sub == nullptr) {
    return false;
  }
  for (const BakeLayerSubscription &entry : sub->layers) {
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

bool BKE_paint_layers_bake_substitute(const Material &ma,
                                      const MaterialPaintLayer &layer,
                                      const int channel,
                                      Image **r_image)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || r_image == nullptr) {
    return false;
  }
  /* A Material layer's bake is its source baked into maps, not a cache of the row: those maps are
   * the row's content (#paint_layer_channel_image) and its mask, corrections and opacity apply to
   * them live, so the row is never substituted whole. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return false;
  }
  if (!BKE_paint_layers_bake_is_valid(ma, layer)) {
    return false;
  }
  Image *image = layer.bake->images[channel];
  if (image == nullptr) {
    return false;
  }
  *r_image = image;
  return true;
}

bool BKE_paint_layers_bake_substitute_custom(const Material &ma,
                                             const MaterialPaintLayer &layer,
                                             const int channel,
                                             Image **r_image,
                                             bool *r_stale)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_NODE_GROUP || layer.bake == nullptr || r_image == nullptr ||
      channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM)
  {
    return false;
  }
  Image *image = layer.bake->images[channel];
  /* Without a map there is nothing to show: the row is skipped and the result below passes. */
  if (image == nullptr || layer.bake->coverage == nullptr) {
    return false;
  }
  *r_image = image;
  if (r_stale != nullptr) {
    *r_stale = !BKE_paint_layers_bake_is_valid(ma, layer);
  }
  return true;
}

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
    return *slot;
  }
  char name[192];
  if (channel >= 0) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    SNPRINTF(name,
             "%s %s Bake",
             layer.name[0] != '\0' ? layer.name : "Layer",
             info.ui_name);
  }
  else {
    SNPRINTF(name, "%s Coverage Bake", layer.name[0] != '\0' ? layer.name : "Layer");
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, black, false, !is_color, false);
  if (image != nullptr) {
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
  }
  *slot = image;
  return image;
}

/** Write \a values into \a image, or only into \a rect (x0,y0,x1,y1 in image pixels) when given. */
static void bake_write_image(Image &image,
                             const float *values,
                             const bool is_color,
                             const int *rect,
                             const bool use_alpha)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf == nullptr) {
    return;
  }
  uchar *pixels = ibuf->byte_data_for_write();
  const ColorSpace *colorspace = ibuf->byte_buffer.colorspace;
  const int width = ibuf->x;
  const int x0 = rect != nullptr ? clamp_i(rect[0], 0, width) : 0;
  const int x1 = rect != nullptr ? clamp_i(rect[2], 0, width) : width;
  const int y0 = rect != nullptr ? clamp_i(rect[1], 0, ibuf->y) : 0;
  const int y1 = rect != nullptr ? clamp_i(rect[3], 0, ibuf->y) : ibuf->y;
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      const int64_t i = int64_t(y) * width + x;
      /* The colour's fourth component carries the row's content alpha (F2-C6); a caller whose
       * pixels have no such meaning asks for an opaque map with #use_alpha false. */
      float rgba[4] = {values[i * 4 + 0],
                       values[i * 4 + 1],
                       values[i * 4 + 2],
                       use_alpha ? clamp_f(values[i * 4 + 3], 0.0f, 1.0f) : 1.0f};
      if (is_color && colorspace != nullptr) {
        IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, colorspace);
      }
      pixels[i * 4 + 0] = uchar(clamp_i(int(rgba[0] * 255.0f + 0.5f), 0, 255));
      pixels[i * 4 + 1] = uchar(clamp_i(int(rgba[1] * 255.0f + 0.5f), 0, 255));
      pixels[i * 4 + 2] = uchar(clamp_i(int(rgba[2] * 255.0f + 0.5f), 0, 255));
      pixels[i * 4 + 3] = uchar(clamp_i(int(rgba[3] * 255.0f + 0.5f), 0, 255));
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
}

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
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      const int64_t i = int64_t(y) * width + x;
      const uchar gray = uchar(clamp_i(int(clamp_f(coverage[i], 0.0f, 1.0f) * 255.0f + 0.5f),
                                       0,
                                       255));
      pixels[i * 4 + 0] = gray;
      pixels[i * 4 + 1] = gray;
      pixels[i * 4 + 2] = gray;
      pixels[i * 4 + 3] = 255;
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
}

bool BKE_paint_layers_bake_row_to_image(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        const int channel,
                                        const int size,
                                        Image &dst)
{
  if (size <= 0 || channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  Vector<float> color(int64_t(size) * size * 4);
  Vector<float> coverage(int64_t(size) * size);
  if (!BKE_paint_layers_bake_render_node(ma, row, channel, size, color.data(), coverage.data())) {
    return false;
  }
  /* The exported map carries the row's content alpha straight (F2-C6), so a later read of it as a
   * paint-layer map sees the same transparency the row had live. */
  bake_write_image(dst, color.data(), info.is_color, nullptr, true);
  return true;
}

/** The generated nodes \a layer's subtree would add, the AUTO threshold's unit. */
static int paint_layer_subtree_weight(const MaterialPaintLayer &layer)
{
  int weight = 4 + layer.channels_num * 6;
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    weight += 4 + paint_layer_subtree_weight(effect) / 2;
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    weight += 4 + paint_layer_subtree_weight(mask_item) / 2;
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    weight += 8 + paint_layer_subtree_weight(child);
  }
  return weight;
}

/**
 * Free \a layer's bake structure and the service maps only it owns.
 *
 * A map whose last user was the bake is deleted; one still named by the generated tree keeps that
 * other user and is simply detached, because freeing it there would leave the tree's node with a
 * dangling pointer. The caller tags #MA_PAINT_LAYERS_REGEN, so the rebuild that follows drops the
 * node before anything is evaluated; the tree is also tagged by itself so a node still naming a
 * detached map resyncs. The bake subscription and the pending-job maps are deliberately not touched:
 * a light row is never the one a heavy job renders.
 */
static void paint_layer_bake_structure_free(Main &bmain, Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return;
  }
  auto release_map = [&](Image *&slot) {
    Image *image = slot;
    if (image == nullptr) {
      return;
    }
    slot = nullptr;
    /* A synchronous service map is handed to the bake without an extra user (unlike
     * #BKE_paint_layers_bake_set_map); only decrement when the bake actually holds one. */
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

bool BKE_paint_layers_bake_ensure(Main &bmain, Material &ma, bool *r_changed)
{
  bool changed = false;
  /* An AUTO row that no longer passes the heavy gate is cheaper live and never re-bakes: the bake
   * left from when it was heavy is stale, its maps sit dead in the file, and the next edit to the
   * row re-tags #MA_PAINT_LAYERS_REGEN for nothing (#paint_layer_or_ancestor_has_bake). Free it in
   * the one pass that owns the weight gates. A manual ALWAYS/NEVER choice is the user's and is left
   * alone; the callers below check the mode before invoking this. */
  const auto drop_light_bake = [&](MaterialPaintLayer &light_layer) {
    if (light_layer.bake != nullptr) {
      paint_layer_bake_structure_free(bmain, ma, light_layer);
      changed = true;
    }
  };
  if (paint_layers_is_layered(ma)) {
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(ma, layers);
    for (const MaterialPaintLayer *layer_const : layers) {
      MaterialPaintLayer &layer = *const_cast<MaterialPaintLayer *>(layer_const);
      /* A Material layer is baked by the material bake job, not the CPU compositor: its channels
       * have no live description representation to render. */
      if (BKE_paint_layers_kind_info(layer.source).needs_external_bake) {
        continue;
      }

      MaterialPaintLayerBake *bake = nullptr;
      int size = 0;

      if (BKE_paint_layers_is_folder(layer)) {
        const int mode = BKE_paint_layers_bake_mode_get(layer);
        if (mode == MA_PAINT_LAYER_BAKE_NEVER) {
          continue;
        }
        if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
          continue;
        }
        /* A folder with no bake yet is a candidate only when it is structurally isolating: a Pass
         * Through folder must never be baked, or its mode flips and every child edit costs two
         * compiles. Without a bake, #BKE_paint_layers_folder_is_pass_through's valid-bake early-out
         * cannot fire, so it answers the structure alone; a folder that already carries a manual
         * bake keeps its previous path. */
        if (layer.bake == nullptr && BKE_paint_layers_folder_is_pass_through(ma, layer)) {
          continue;
        }
        /* The active row and its ancestors stay live for every mode: the user is editing inside
         * them, and the bake catches up once the active marker leaves the subtree
         * (#MA_PAINT_LAYERS_MATERIAL_BAKE_DUE). Tested before heavy so a heavy deferred row is not
         * queued either. */
        if (BKE_paint_layers_bake_row_is_deferred(ma, layer)) {
          continue;
        }
        /* A heavy row leaves the main thread: the wmJob scheduler picks it up from the same
         * pending/bake-stale signal this function leaves set. */
        if (BKE_paint_layers_bake_is_heavy(ma, layer)) {
          continue;
        }
        if (mode == MA_PAINT_LAYER_BAKE_AUTO &&
            paint_layer_subtree_weight(layer) <= PAINT_LAYERS_AUTO_BAKE_NODES)
        {
          /* A light subtree is cheaper live; a bake carried over from when it was heavy is
           * released and never re-rendered. */
          drop_light_bake(layer);
          continue;
        }
        size = (layer.bake != nullptr) ? layer.bake->size : 0;
        if (size <= 0) {
          /* A zero size means "the node's own map size": read it from the content. */
          int width = 0;
          int height = 0;
          for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
            if (BKE_paint_layers_row_dimensions(ma, layer, int(info.channel), width, height) &&
                width > 0 && height > 0)
            {
              size = width;
              break;
            }
          }
        }
        if (size <= 0) {
          continue;
        }
        /* Every gate has passed and this folder is about to render: only here is it safe to
         * allocate the bake structure. */
        bake = BKE_paint_layers_bake_ensure(layer);
      }
      else {
        /* A light non-folder row never gets a bake structure: allocating one before it is about
         * to render would leave a permanently-invalid bake behind for a row nothing ever bakes,
         * which stalls the #MA_PAINT_LAYERS_BAKE_STALE drain and re-tags #MA_PAINT_LAYERS_REGEN
         * on every unrelated edit. #BKE_paint_layers_bake_mode_get and #BKE_paint_layers_bake_is_valid
         * both already read a null bake safely, so the gates below need no allocation either. */
        const int mode = BKE_paint_layers_bake_mode_get(layer);
        if (mode == MA_PAINT_LAYER_BAKE_NEVER) {
          continue;
        }
        if (layer.bake != nullptr && BKE_paint_layers_bake_is_valid(ma, layer)) {
          continue;
        }
        if (BKE_paint_layers_bake_row_is_deferred(ma, layer)) {
          continue;
        }
        if (BKE_paint_layers_bake_is_heavy(ma, layer)) {
          continue;
        }
        if (mode == MA_PAINT_LAYER_BAKE_AUTO &&
            paint_layer_subtree_weight(layer) <= PAINT_LAYERS_AUTO_BAKE_NODES)
        {
          /* A light subtree is cheaper live, and stays without a bake structure: this is a final
           * state for it, not a step toward one. A stale bake from a heavier past is released. */
          drop_light_bake(layer);
          continue;
        }
        size = (layer.bake != nullptr) ? layer.bake->size : 0;
        if (size <= 0) {
          /* A zero size means "the node's own map size": read it from the content. */
          int width = 0;
          int height = 0;
          for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
            if (BKE_paint_layers_row_dimensions(ma, layer, int(info.channel), width, height) &&
                width > 0 && height > 0)
            {
              size = width;
              break;
            }
          }
        }
        if (size <= 0) {
          /* No map has known dimensions yet: nothing to allocate a bake structure for. */
          continue;
        }
        /* Every gate has passed and this row renders right now: only here is it safe to
         * allocate the bake structure. */
        bake = BKE_paint_layers_bake_ensure(layer);
      }

      PL_DEBUG_PRINTF("paint layers bake: start kind=sync material='%s' row='%s' reason=stale\n",
                      ma.id.name + 2,
                      layer.name);
      /* A changed rectangle may update only that part of an existing cache; a fresh cache needs
       * the whole render. */
      int region[4];
      const bool has_region = BKE_paint_layers_bake_changed_region(ma, layer, region);
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
        const int x0 = (int(int64_t(region[0]) * size / src_width) / tile) * tile;
        const int y0 = (int(int64_t(region[2]) * size / src_height) / tile) * tile;
        const int x1 = ((int(int64_t(region[1]) * size / src_width) / tile) + 1) * tile;
        const int y1 = ((int(int64_t(region[3]) * size / src_height) / tile) + 1) * tile;
        rect[0] = clamp_i(x0, 0, size);
        rect[1] = clamp_i(y0, 0, size);
        rect[2] = clamp_i(x1, 0, size);
        rect[3] = clamp_i(y1, 0, size);
        rect_ptr = rect;
      }

      Vector<float> color(int64_t(size) * size * 4);
      Vector<float> coverage(int64_t(size) * size);
      bool any_channel = false;
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        const int channel = int(info.channel);
        /* A partial re-bake needs the maps to already exist: an image created just now cannot take
         * a partial write, its untouched region would stay blank. So the changed rectangle is
         * computed and written only for a channel with a live cache; a fresh channel renders
         * whole. */
        const int *channel_rect = (rect_ptr != nullptr && bake->images[channel] != nullptr) ?
                                       rect_ptr :
                                       nullptr;
        if (!BKE_paint_layers_bake_render_node(
                ma, layer, channel, size, color.data(), coverage.data(), channel_rect))
        {
          continue;
        }
        Image *color_image = bake_service_image(bmain, layer, channel, size, info.is_color);
        Image *coverage_image = bake_service_image(bmain, layer, -1, size, false);
        if (color_image == nullptr || coverage_image == nullptr) {
          continue;
        }
        /* The colour map's alpha carries the row's content alpha (F2-C6). */
        bake_write_image(*color_image, color.data(), info.is_color, channel_rect, true);
        bake_write_coverage_image(*coverage_image, coverage.data(), channel_rect);
        any_channel = true;
      }
      if (!any_channel) {
        continue;
      }
      uint32_t hash[2];
      BKE_paint_layers_bake_hash(ma, layer, hash);
      bake->hash[0] = hash[0];
      bake->hash[1] = hash[1];
      BKE_paint_layers_bake_subscribe(ma, layer);
      changed = true;
    }
  }
  if (changed) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  /* The queue is drained once no baked row is invalid any more; only then is the signal cleared. */
  bool pending = false;
  if (paint_layers_is_layered(ma)) {
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (BKE_paint_layers_kind_info(layer->source).needs_external_bake) {
        continue;
      }
      if (layer->bake != nullptr && layer->bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
          !BKE_paint_layers_bake_is_valid(ma, *layer))
      {
        pending = true;
        break;
      }
    }
  }
  if (!pending) {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
  }
  if (r_changed != nullptr) {
    *r_changed = changed;
  }
  return changed;
}

bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer)
{
  UNUSED_VARS(ma);
  if (paint_layer_subtree_weight(layer) > PAINT_LAYERS_AUTO_BAKE_NODES) {
    return true;
  }
  return layer.bake != nullptr && layer.bake->size >= PAINT_LAYERS_HEAVY_BAKE_SIZE;
}

bool BKE_paint_layers_bake_row_is_deferred(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A row the sampler budget pinned onto its baked maps is never shown live, so the planner must be
   * free to bake it even when it is the active row or sits in the active chain: leaving it on a stale
   * bake would show the user the wrong picture. Its ancestors are unaffected and stay live. */
  if (BKE_paint_layers_material_forced_bake(ma, layer)) {
    return false;
  }
  return BKE_paint_layers_subtree_contains(layer, ma.active_layer_marker);
}

/* Maps a bake job is rendering right now; the editor's job code fills it. */
static std::mutex &bake_pending_mutex()
{
  static std::mutex mutex;
  return mutex;
}

static Map<uint32_t, int> &bake_pending_images()
{
  static Map<uint32_t, int> map;
  return map;
}

void BKE_paint_layers_bake_image_pending_add(const uint32_t image_session_uid)
{
  std::lock_guard lock(bake_pending_mutex());
  bake_pending_images().lookup_or_add(image_session_uid, 0)++;
}

void BKE_paint_layers_bake_image_pending_remove(const uint32_t image_session_uid)
{
  std::lock_guard lock(bake_pending_mutex());
  int *count = bake_pending_images().lookup_ptr(image_session_uid);
  if (count != nullptr && --*count <= 0) {
    bake_pending_images().remove(image_session_uid);
  }
}

static bool material_row_bake_in_flight(const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return false;
  }
  std::lock_guard lock(bake_pending_mutex());
  const Map<uint32_t, int> &pending = bake_pending_images();
  if (pending.is_empty()) {
    return false;
  }
  if (layer.bake->coverage != nullptr && pending.contains(layer.bake->coverage->id.session_uid)) {
    return true;
  }
  for (const Image *image : layer.bake->images) {
    if (image != nullptr && pending.contains(image->id.session_uid)) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_material_bake_ready(const Material &ma, const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_bake_is_valid(ma, layer) && !material_row_bake_in_flight(layer);
}

/**
 * The one eligibility rule behind both live helpers: a channel of a Material row lives from its
 * source when the row is deferred, when this channel has no baked map yet, or when the row's bake
 * cannot be shown yet (#BKE_paint_layers_material_bake_ready). Otherwise it falls back to its map.
 * The last term is what keeps a row that just left the active chain live until its bake lands, so it
 * changes mode once, when the maps are complete, instead of passing through a mode that shows old
 * or blank pixels. A row whose bake is switched off never bakes, so it keeps the map it has.
 * Keeping this in one place is what makes the row's channel set independent of where the focus is.
 */
static bool material_live_row_eligible(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const int channel,
                                       const PaintLayersRegenCache *cache)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer.material == nullptr || channel < 0 ||
      channel >= PAINT_MATERIAL_CHANNEL_NUM)
  {
    return false;
  }
  if (BKE_paint_layers_bake_row_is_deferred(ma, layer) ||
      paint_layer_material_source_map(layer, channel) == nullptr)
  {
    return true;
  }
  if (layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER) {
    return false;
  }
  if (cache == nullptr || !cache->modes_frozen) {
    return !BKE_paint_layers_material_bake_ready(ma, layer);
  }
  if (const bool *found = cache->bake_ready.lookup_ptr(&layer)) {
    return !*found;
  }
  const bool ready = BKE_paint_layers_material_bake_ready(ma, layer);
  cache->bake_ready.add(&layer, ready);
  return !ready;
}

/**
 * Whether \a source names an Image Texture both sides can show in UV space: a flat projection with
 * nothing linked to its Vector input. The CPU samples the buffer straight and reproduces neither a
 * mapping chain nor a non-flat projection, so anything else belongs to the wrapper group.
 */
static bool material_image_is_trivial(const ChannelSourceImage &source)
{
  if (source.image == nullptr || source.node == nullptr) {
    return false;
  }
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(source.node->storage);
  if (storage == nullptr || storage->projection != SHD_PROJ_FLAT) {
    return false;
  }
  const bNodeSocket *vector = bke::node_find_socket(
      const_cast<bNode &>(*source.node), SOCK_IN, "Vector"_ustr);
  return vector == nullptr || !vector->is_directly_linked();
}

const MaterialSourceResolve &PaintLayersRegenCache::resolve(const Material *source) const
{
  std::unique_ptr<MaterialSourceResolve> &entry = this->resolves_.lookup_or_add_cb(source, [&]() {
    return std::make_unique<MaterialSourceResolve>(BKE_paint_material_source_resolve(source));
  });
  return *entry;
}

void PaintLayersRegenCache::invalidate_for_owner(const Material &owner)
{
  this->modes.clear();
  this->bake_ready.clear();
  this->resolves_.remove(&owner);
}

const MaterialSourceResolve &PaintLayersRegenCache::resolve_get(const Material *source,
                                                                const PaintLayersRegenCache *cache,
                                                                MaterialSourceResolve &r_local)
{
  if (cache != nullptr) {
    return cache->resolve(source);
  }
  r_local = BKE_paint_material_source_resolve(source);
  return r_local;
}

static PaintLayerMaterialMode material_mode_compute(const Material &ma,
                                                    const MaterialPaintLayer &layer,
                                                    const PaintLayersRegenCache *cache)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer.material == nullptr ||
      layer.material->nodetree == nullptr)
  {
    return PaintLayerMaterialMode::Baked;
  }
  /* A sampler-budget fallback pins the row onto its baked maps for this session. Routing it through
   * here is what the topology hash reads, so the rebuild sees the mode change once. */
  if (BKE_paint_layers_material_forced_bake(ma, layer)) {
    return PaintLayerMaterialMode::Baked;
  }
  /* One resolver pass for the whole row; the live helpers then answer a specific channel. */
  MaterialSourceResolve resolve_local;
  const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(layer.material, cache, resolve_local);
  bool any_live = false;
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    if (!material_live_row_eligible(ma, layer, channel, cache)) {
      continue;
    }
    const ChannelResolution resolution = resolve.channels[channel];
    if (resolution == ChannelResolution::Unavailable) {
      /* Nothing to show in this channel; it does not decide the mode. */
      continue;
    }
    any_live = true;
    if (resolution == ChannelResolution::Constant) {
      continue;
    }
    if (resolution == ChannelResolution::Image && material_image_is_trivial(resolve.images[channel]))
    {
      continue;
    }
    /* A Baked channel, or a mapped/projected texture: the CPU cannot reproduce it. */
    return PaintLayerMaterialMode::SourceGroup;
  }
  return any_live ? PaintLayerMaterialMode::Hybrid : PaintLayerMaterialMode::Baked;
}

PaintLayerMaterialMode BKE_paint_layers_material_mode(const Material &ma,
                                                      const MaterialPaintLayer &layer,
                                                      const PaintLayersRegenCache *cache)
{
  if (cache == nullptr || !cache->modes_frozen) {
    return material_mode_compute(ma, layer, cache);
  }
  if (const PaintLayerMaterialMode *found = cache->modes.lookup_ptr(&layer)) {
    return *found;
  }
  const PaintLayerMaterialMode mode = material_mode_compute(ma, layer, cache);
  cache->modes.add(&layer, mode);
  return mode;
}

bool BKE_paint_layers_material_live_constant(const Material &ma,
                                             const MaterialPaintLayer &layer,
                                             const int channel,
                                             float r_value[4],
                                             const PaintLayersRegenCache *cache)
{
  /* The live helpers are the Hybrid path only: in SourceGroup the generator shows the wrapper and
   * the CPU must stay on the baked maps, so nothing here may answer. */
  if (BKE_paint_layers_material_mode(ma, layer, cache) != PaintLayerMaterialMode::Hybrid ||
      !material_live_row_eligible(ma, layer, channel, cache))
  {
    return false;
  }
  MaterialSourceResolve resolve_local;
  const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(layer.material, cache, resolve_local);
  if (resolve.channels[channel] != ChannelResolution::Constant) {
    return false;
  }
  copy_v4_v4(r_value, resolve.constants[channel]);
  return true;
}

bool BKE_paint_layers_material_live_image(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const int channel,
                                          Image **r_image,
                                          const ImageUser **r_iuser,
                                          const PaintLayersRegenCache *cache)
{
  if (r_image == nullptr || r_iuser == nullptr ||
      BKE_paint_layers_material_mode(ma, layer, cache) != PaintLayerMaterialMode::Hybrid ||
      !material_live_row_eligible(ma, layer, channel, cache))
  {
    return false;
  }
  MaterialSourceResolve resolve_local;
  const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(layer.material, cache, resolve_local);
  if (resolve.channels[channel] != ChannelResolution::Image) {
    return false;
  }
  const ChannelSourceImage &source = resolve.images[channel];
  if (!material_image_is_trivial(source)) {
    return false;
  }
  *r_image = source.image;
  *r_iuser = source.iuser;
  return true;
}

bool BKE_paint_layers_material_lives_from_source(const Material &ma,
                                                 const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_material_mode(ma, layer) != PaintLayerMaterialMode::Baked;
}

bool BKE_paint_layers_bake_image_is_deferred(const Main &bmain, const Image &image)
{
  for (const Material &ma : bmain.materials) {
    if (!paint_layers_is_layered(ma)) {
      continue;
    }
    Vector<const MaterialPaintLayer *> layers;
    /* An Effect correction now also owns an external bake (Material/Node Group), so its maps must
     * be found here too, or they would never be recognised as deferred. */
    BKE_paint_layers_flatten_all(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (layer->bake == nullptr || !BKE_paint_layers_bake_row_is_deferred(ma, *layer)) {
        continue;
      }
      if (layer->bake->coverage == &image) {
        return true;
      }
      for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
        if (layer->bake->images[i] == &image) {
          return true;
        }
      }
    }
  }
  return false;
}

bool BKE_paint_layers_source_material_is_live(const Main &bmain, const Material &source)
{
  /* Localized and evaluated copies share the description with the original and must not be asked:
   * only the material that owns its stack answers. */
  const int no_regen_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (const Material &ma : bmain.materials) {
    if ((ma.id.tag & no_regen_tags) != 0 || !paint_layers_is_layered(ma)) {
      continue;
    }
    Vector<const MaterialPaintLayer *> layers;
    /* An Effect correction reading this source is as live as a Layer row reading it, so it must
     * count here too. */
    BKE_paint_layers_flatten_all(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer->material != &source) {
        continue;
      }
      if (!BKE_paint_layers_bake_row_is_deferred(ma, *layer)) {
        continue;
      }
      if (BKE_paint_layers_material_mode(ma, *layer) == PaintLayerMaterialMode::Baked) {
        continue;
      }
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_bake_heavy_pending(const Material &ma)
{
  if (!paint_layers_is_layered(ma)) {
    return false;
  }
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (BKE_paint_layers_kind_info(layer->source).needs_external_bake) {
      continue;
    }
    /* The active row and its ancestors are not queued: their bake catches up once the marker
     * leaves the subtree, like the synchronous planner. */
    if (BKE_paint_layers_bake_row_is_deferred(ma, *layer)) {
      continue;
    }
    if (BKE_paint_layers_is_folder(*layer)) {
      const int mode = BKE_paint_layers_bake_mode_get(*layer);
      if (mode == MA_PAINT_LAYER_BAKE_NEVER) {
        continue;
      }
      if (layer->bake != nullptr && BKE_paint_layers_bake_is_valid(ma, *layer)) {
        continue;
      }
      /* See #BKE_paint_layers_bake_ensure: a bake-less Pass Through folder is never queued. */
      if (layer->bake == nullptr && BKE_paint_layers_folder_is_pass_through(ma, *layer)) {
        continue;
      }
      if (!BKE_paint_layers_bake_is_heavy(ma, *layer)) {
        continue;
      }
      return true;
    }
    /* is_heavy needs no bake structure, so it is checked first: a heavy-by-weight row with no
     * explicit bake can now be seen as pending, where the old bake-gated formula never queued it. */
    if (!BKE_paint_layers_bake_is_heavy(ma, *layer)) {
      continue;
    }
    if (BKE_paint_layers_bake_mode_get(*layer) == MA_PAINT_LAYER_BAKE_NEVER) {
      continue;
    }
    if (layer->bake != nullptr && BKE_paint_layers_bake_is_valid(ma, *layer)) {
      continue;
    }
    return true;
  }
  return false;
}

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
    Vector<ChannelResult> channels;
  };
  Vector<RowResult> rows;
};

PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain, Material &ma)
{
  if (!paint_layers_is_layered(ma)) {
    return nullptr;
  }
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);

  PaintLayersBakeJob *job = MEM_new<PaintLayersBakeJob>(__func__);
  job->bmain = &bmain;
  job->material = &ma;
  job->material_session_uid = ma.id.session_uid;

  for (const MaterialPaintLayer *layer : layers) {
    if (BKE_paint_layers_kind_info(layer->source).needs_external_bake) {
      continue;
    }
    /* The row the user is editing inside is left live; it is queued only after the active marker
     * leaves its subtree. */
    if (BKE_paint_layers_bake_row_is_deferred(ma, *layer)) {
      continue;
    }
    if (BKE_paint_layers_is_folder(*layer)) {
      const int mode = BKE_paint_layers_bake_mode_get(*layer);
      if (mode == MA_PAINT_LAYER_BAKE_NEVER) {
        continue;
      }
      if (layer->bake != nullptr && BKE_paint_layers_bake_is_valid(ma, *layer)) {
        continue;
      }
      /* See #BKE_paint_layers_bake_ensure: a bake-less Pass Through folder is never queued. */
      if (layer->bake == nullptr && BKE_paint_layers_folder_is_pass_through(ma, *layer)) {
        continue;
      }
      if (!BKE_paint_layers_bake_is_heavy(ma, *layer)) {
        continue;
      }
    }
    else {
      if ((layer->bake != nullptr && layer->bake->mode == MA_PAINT_LAYER_BAKE_NEVER) ||
          (layer->bake != nullptr && BKE_paint_layers_bake_is_valid(ma, *layer)) ||
          !BKE_paint_layers_bake_is_heavy(ma, *layer))
      {
        continue;
      }
    }
    int size = (layer->bake != nullptr) ? layer->bake->size : 0;
    if (size <= 0) {
      int width = 0;
      int height = 0;
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        if (BKE_paint_layers_row_dimensions(ma, *layer, int(info.channel), width, height) &&
            width > 0 && height > 0)
        {
          size = width;
          break;
        }
      }
    }
    if (size <= 0) {
      /* No map has known dimensions yet: nothing to allocate a bake structure for. */
      continue;
    }
    /* Every gate has passed and this row is queued right now: only here is it safe to allocate
     * the bake structure (a folder without one gets it here too). */
    MaterialPaintLayer &mutable_layer = *const_cast<MaterialPaintLayer *>(layer);
    BKE_paint_layers_bake_ensure(mutable_layer);
    PaintLayersBakeJob::RowResult row;
    row.marker = mutable_layer.marker;
    row.size = size;
    job->rows.append(row);
  }
  if (job->rows.is_empty()) {
    MEM_delete(job);
    return nullptr;
  }
  job->material_copy = id_cast<Material *>(BKE_id_copy_ex(
      nullptr,
      &ma.id,
      nullptr,
      LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  if (job->material_copy == nullptr) {
    MEM_delete(job);
    return nullptr;
  }
  return job;
}

void BKE_paint_layers_bake_job_compute(PaintLayersBakeJob &job)
{
  if (job.material_copy == nullptr) {
    return;
  }
  Material &ma = *job.material_copy;
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(ma, row.marker);
    if (layer == nullptr) {
      continue;
    }
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      Vector<float> color(int64_t(row.size) * row.size * 4);
      Vector<float> coverage(int64_t(row.size) * row.size);
      if (!BKE_paint_layers_bake_render_node(
              ma, *layer, channel, row.size, color.data(), coverage.data()))
      {
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
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
    return false;
  }
  bool changed = false;
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, row.marker);
    /* The row was removed, its bake dropped or switched off while the worker ran: nothing to
     * write. */
    if (layer == nullptr || layer->bake == nullptr ||
        layer->bake->mode == MA_PAINT_LAYER_BAKE_NEVER)
    {
      continue;
    }
    bool row_written = false;
    for (PaintLayersBakeJob::ChannelResult &channel : row.channels) {
      Image *color_image = bake_service_image(
          bmain, *layer, channel.channel, row.size, channel.is_color);
      Image *coverage_image = bake_service_image(bmain, *layer, -1, row.size, false);
      if (color_image == nullptr || coverage_image == nullptr) {
        continue;
      }
      /* The colour map's alpha carries the row's content alpha (F2-C6). */
      bake_write_image(*color_image, channel.color.data(), channel.is_color, nullptr, true);
      bake_write_coverage_image(*coverage_image, channel.coverage.data(), nullptr);
      row_written = true;
    }
    if (!row_written) {
      continue;
    }
    uint32_t hash[2];
    BKE_paint_layers_bake_hash(*ma, *layer, hash);
    layer->bake->hash[0] = hash[0];
    layer->bake->hash[1] = hash[1];
    BKE_paint_layers_bake_subscribe(*ma, *layer);
    changed = true;
  }
  if (changed) {
    ma->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  /* The queue is drained once no baked row is invalid any more, exactly as the synchronous
   * planner decides it. */
  bool pending = false;
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(*ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (BKE_paint_layers_kind_info(layer->source).needs_external_bake) {
      continue;
    }
    if (layer->bake != nullptr && layer->bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
        !BKE_paint_layers_bake_is_valid(*ma, *layer))
    {
      pending = true;
      break;
    }
  }
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

struct PaintLayersRowResultJob {
  Main *bmain = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
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
  if (!paint_layers_is_layered(ma) || size <= 0 ||
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
  job->material_copy = id_cast<Material *>(BKE_id_copy_ex(
      nullptr,
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
  MaterialPaintLayer *source = BKE_paint_layers_find(ma, job.source_marker);
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
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
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
    SNPRINTF(name,
             "%s %s",
             target->name[0] != '\0' ? target->name : "Layer",
             info.ui_name);
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
    /* Use Row Result renders \a source with #BKE_paint_layers_bake_render_node (CPU, not EEVEE), so
     * the colour's alpha is already the source row's content alpha -- 1.0 when the source tracks
     * none (Material/Normal), its straight alpha when it does. Written through unchanged so the
     * target map means the same thing any other paint-layer map does. */
    bake_write_image(*image, result.color.data(), info.is_color, nullptr, true);
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
        const char *str = static_cast<const char *>(prop->data.pointer);
        if (str != nullptr) {
          for (const char *c = str; *c != '\0'; c++) {
            h = bake_hash_mix(h, uint8_t(*c));
          }
        }
        break;
      }
      case IDP_GROUP:
        h = bake_hash_idprops(h, prop);
        break;
      case IDP_ARRAY: {
        h = bake_hash_mix(h, uint32_t(prop->len));
        if (prop->subtype == IDP_FLOAT) {
          const float *values = static_cast<const float *>(prop->data.pointer);
          for (int i = 0; i < prop->len; i++) {
            h = bake_hash_float(h, values[i]);
          }
        }
        else if (prop->subtype == IDP_INT) {
          const int *values = static_cast<const int *>(prop->data.pointer);
          for (int i = 0; i < prop->len; i++) {
            h = bake_hash_mix(h, uint32_t(values[i]));
          }
        }
        break;
      }
      default:
        /* Scalars live in the two int words (a double occupies both). */
        h = bake_hash_mix(h, uint32_t(prop->data.val));
        h = bake_hash_mix(h, uint32_t(prop->data.val2));
        break;
    }
  }
  return h;
}

/* The source hash is a hash of the source's *content*: topology plus the values that change what
 * the graph evaluates to. It deliberately does not read `runtime->previews_refresh_state`, which
 * any node tree update bumps -- including an update of a *different* tree that only shares a nested
 * group with this one. Reading it made the hash move whenever the layered tree or the bake's
 * localized copy was updated, which drove the editor's "source edited -> rebuild -> re-bake" loop.
 *
 * Included: node types/names, links with socket identifiers and mute state, group IDs, group
 * interfaces (topology), plus node custom1/custom2/custom3/custom4, `node->id`, the node's DNA
 * storage bytes, the default values of every input socket, and the default values of group
 * interface sockets. Excluded: location, parent, selection and UI flags. */

static uint64_t source_hash_string(uint64_t h, const char *text)
{
  if (text == nullptr) {
    return bake_hash_mix(h, 0);
  }
  for (const char *c = text; *c != '\0'; c++) {
    h = bake_hash_mix(h, uint8_t(*c));
  }
  return bake_hash_mix(h, 1);
}

/** Hash the scalar \a data of a socket of \a type, as #bNodeSocket::default_value stores it. */
static uint64_t source_hash_socket_value(uint64_t h,
                                         const eNodeSocketDatatype type,
                                         const void *data)
{
  if (data == nullptr) {
    return bake_hash_mix(h, 0xFFFFFFFFu);
  }
  switch (type) {
    case SOCK_FLOAT:
      return bake_hash_float(h, static_cast<const bNodeSocketValueFloat *>(data)->value);
    case SOCK_INT:
      return bake_hash_mix(h, uint32_t(static_cast<const bNodeSocketValueInt *>(data)->value));
    case SOCK_BOOLEAN:
      return bake_hash_mix(
          h, uint32_t(static_cast<const bNodeSocketValueBoolean *>(data)->value));
    case SOCK_VECTOR: {
      const bNodeSocketValueVector *value = static_cast<const bNodeSocketValueVector *>(data);
      for (const int i : IndexRange(3)) {
        h = bake_hash_float(h, value->value[i]);
      }
      return bake_hash_mix(h, uint32_t(value->dimensions));
    }
    case SOCK_INT_VECTOR: {
      const bNodeSocketValueIntVector *value = static_cast<const bNodeSocketValueIntVector *>(data);
      for (const int i : IndexRange(3)) {
        h = bake_hash_mix(h, uint32_t(value->value[i]));
      }
      return bake_hash_mix(h, uint32_t(value->dimensions));
    }
    case SOCK_RGBA: {
      const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(data);
      for (const int i : IndexRange(4)) {
        h = bake_hash_float(h, value->value[i]);
      }
      return h;
    }
    case SOCK_ROTATION: {
      const bNodeSocketValueRotation *value = static_cast<const bNodeSocketValueRotation *>(data);
      for (const int i : IndexRange(3)) {
        h = bake_hash_float(h, value->value_euler[i]);
      }
      return h;
    }
    case SOCK_STRING:
      return source_hash_string(h, static_cast<const bNodeSocketValueString *>(data)->value);
    case SOCK_MENU:
      return bake_hash_mix(h, uint32_t(static_cast<const bNodeSocketValueMenu *>(data)->value));
    default:
      /* Datablock, shader, geometry and other pointer-backed sockets carry no scalar value. */
      return bake_hash_mix(h, 0xFFFFFFFEu);
  }
}

/**
 * Hash a #CurveMapping's points and ranges. Its DNA bytes only carry pointers to the allocated
 * points, so the raw-byte route would freeze the hash on the allocation address and miss a curve
 * edit. Used by the RGB/Vector/Float Curves shader nodes.
 */
static uint64_t source_hash_curve_mapping(uint64_t h, const CurveMapping &mapping)
{
  h = bake_hash_mix(h, uint32_t(mapping.flag));
  h = bake_hash_mix(h, uint32_t(mapping.cur));
  h = bake_hash_mix(h, uint32_t(mapping.preset));
  h = bake_hash_mix(h, uint32_t(mapping.tone));
  for (const CurveMap &map : mapping.cm) {
    h = bake_hash_mix(h, uint32_t(uint16_t(map.totpoint)));
    h = bake_hash_mix(h, uint32_t(int16_t(map.flag)));
    for (int point = 0; point < map.totpoint && map.curve != nullptr; point++) {
      const CurveMapPoint &pt = map.curve[point];
      h = bake_hash_float(h, pt.x);
      h = bake_hash_float(h, pt.y);
      h = bake_hash_mix(h, uint32_t(uint16_t(pt.flag)));
    }
  }
  for (const int i : IndexRange(3)) {
    h = bake_hash_float(h, mapping.black[i]);
    h = bake_hash_float(h, mapping.white[i]);
    h = bake_hash_float(h, mapping.bwmul[i]);
  }
  return h;
}

/**
 * Hash a node's DNA \a storage, sized through the runtime SDNA by the type's `storagename`.
 *
 * There is no shared hash/compare helper for node storage in the code base; this is the generic
 * route the file read/write code already uses to know the struct. Storage is allocated zeroed and
 * is not reallocated by node updates, so raw bytes are stable across the wrapper rebuilds this
 * hash has to ignore. #CurveMapping is the exception: its bytes hold pointers, so it is hashed by
 * its points instead.
 */
static uint64_t source_hash_node_storage(uint64_t h, const bNode &node)
{
  if (node.storage == nullptr || node.typeinfo == nullptr || node.typeinfo->storagename.empty()) {
    return bake_hash_mix(h, 0);
  }
  if (node.typeinfo->storagename == "CurveMapping") {
    return source_hash_curve_mapping(h, *static_cast<const CurveMapping *>(node.storage));
  }
  const SDNA *sdna = DNA_sdna_current_get();
  const int struct_index = DNA_struct_find_index_without_alias(
      sdna, node.typeinfo->storagename.c_str());
  if (struct_index < 0) {
    return bake_hash_mix(h, 1);
  }
  const int size = DNA_struct_size(sdna, struct_index);
  const uint8_t *bytes = static_cast<const uint8_t *>(node.storage);
  h = bake_hash_mix(h, uint32_t(size));
  for (const int i : IndexRange(size)) {
    h = bake_hash_mix(h, bytes[i]);
  }
  return h;
}

uint64_t BKE_paint_layers_source_node_storage_hash(const bNode &node)
{
  /* One seed for both operands so the comparison is stable; CurveMapping goes through its points. */
  return source_hash_node_storage(1469598103934665603ull, node);
}

static void source_tree_topology_hash_recursive(const bNodeTree &tree,
                                                Set<const bNodeTree *> &visited,
                                                uint64_t &r_hash);
static void source_tree_values_hash_recursive(const bNodeTree &tree,
                                              Set<const bNodeTree *> &visited,
                                              uint64_t &r_hash);

#if PAINT_LAYERS_DEBUG_LOG

/**
 * Remembers the last content hash of a few source materials so the diagnostic below can name the
 * trees whose `previews_refresh_state` moved it. A fixed POD table rather than a `blender::Map`:
 * a lazily allocated global container is reported as a leak at exit by guardedalloc, because its
 * destructor runs after the leak check. The hash may be asked from a job thread as well as the main
 * one, so the table is guarded by #g_source_hash_trace_mutex.
 */
struct SourceHashTrace {
  uint32_t session_uid = 0;
  uint64_t hash = 0;
  int tree_num = 0;
  const bNodeTree *trees[64] = {};
  uint32_t previews[64] = {};
};
static SourceHashTrace g_source_hash_trace[8];
static std::mutex g_source_hash_trace_mutex;

/** Trace, named in the log, of which nested tree an update came from. */
static void source_hash_trace(const Material &ma,
                              const uint64_t hash,
                              const Set<const bNodeTree *> &visited)
{
  std::lock_guard<std::mutex> lock(g_source_hash_trace_mutex);
  SourceHashTrace *slot = nullptr;
  SourceHashTrace *empty = nullptr;
  for (SourceHashTrace &candidate : g_source_hash_trace) {
    if (candidate.session_uid == ma.id.session_uid) {
      slot = &candidate;
      break;
    }
    if (candidate.session_uid == 0 && empty == nullptr) {
      empty = &candidate;
    }
  }
  if (slot == nullptr) {
    slot = empty;
    if (slot == nullptr) {
      /* More dirty sources than the table holds; the trace is best-effort. */
      return;
    }
    slot->session_uid = ma.id.session_uid;
  }
  if (slot->hash != 0 && slot->hash != hash) {
    printf("paint layers hash: source='%s' old=%llx new=%llx changed_trees=",
           ma.id.name + 2,
           static_cast<unsigned long long>(slot->hash),
           static_cast<unsigned long long>(hash));
    bool any = false;
    for (const bNodeTree *tree : visited) {
      const uint32_t now = tree->runtime->previews_refresh_state;
      bool changed = true;
      for (int i = 0; i < slot->tree_num; i++) {
        if (slot->trees[i] == tree) {
          changed = slot->previews[i] != now;
          break;
        }
      }
      if (changed) {
        printf("%s%s", any ? ", " : "", tree->id.name + 2);
        any = true;
      }
    }
    printf("%s\n", any ? "" : "<none>");
  }
  slot->hash = hash;
  slot->tree_num = 0;
  for (const bNodeTree *tree : visited) {
    if (slot->tree_num >= int(ARRAY_SIZE(slot->trees))) {
      break;
    }
    slot->trees[slot->tree_num] = tree;
    slot->previews[slot->tree_num] = tree->runtime->previews_refresh_state;
    slot->tree_num++;
  }
}

#endif /* PAINT_LAYERS_DEBUG_LOG */

/** The content hash of \a ma's node tree, or zero when it has none. */
uint64_t BKE_paint_layers_source_material_tree_hash(const Material &ma)
{
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  Set<const bNodeTree *> visited;
  uint64_t topology_hash = 0;
  source_tree_topology_hash_recursive(*ma.nodetree, visited, topology_hash);
  uint64_t hash = bake_hash_mix(0, topology_hash);
  Set<const bNodeTree *> value_visited;
  source_tree_values_hash_recursive(*ma.nodetree, value_visited, hash);
#if PAINT_LAYERS_DEBUG_LOG
  source_hash_trace(ma, hash, visited);
#endif
  return hash;
}

/**
 * A topology-only hash of \a tree and every group it reaches, once each: node types and names, the
 * links with their socket identifiers and muted state, the group IDs and every group interface.
 * Values are deliberately left out -- socket defaults, node storage, custom properties and
 * locations -- so moving a slider does not move this. The source-group wrapper compares it to
 * decide between a rebuild (topology) and a sync of values in place.
 */
static void source_tree_topology_hash_recursive(const bNodeTree &tree,
                                                Set<const bNodeTree *> &visited,
                                                uint64_t &r_hash)
{
  if (!visited.add(&tree)) {
    return;
  }
  auto hash_string = [&](const char *text) {
    if (text == nullptr) {
      r_hash = bake_hash_mix(r_hash, 0);
      return;
    }
    for (const char *c = text; *c != '\0'; c++) {
      r_hash = bake_hash_mix(r_hash, uint8_t(*c));
    }
    r_hash = bake_hash_mix(r_hash, 1);
  };

  for (const bNode *node : tree.all_nodes()) {
    r_hash = bake_hash_mix(r_hash, uint32_t(node->typeinfo != nullptr ? node->type_legacy : 0));
    hash_string(node->typeinfo != nullptr ? node->typeinfo->idname.c_str() : nullptr);
    hash_string(node->name);
    if (node->is_group() && node->id != nullptr) {
      r_hash = bake_hash_mix(r_hash, node->id->session_uid);
    }
  }
  for (const bNodeLink &link : tree.links) {
    if (link.fromnode == nullptr || link.tonode == nullptr || link.fromsock == nullptr ||
        link.tosock == nullptr)
    {
      continue;
    }
    hash_string(link.fromnode->name);
    hash_string(link.fromsock->identifier);
    hash_string(link.tonode->name);
    hash_string(link.tosock->identifier);
    r_hash = bake_hash_mix(r_hash, link.is_muted() ? 1 : 0);
  }
  tree.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    r_hash = bake_hash_mix(r_hash, 1);
    hash_string(socket->identifier);
    hash_string(socket->socket_type);
  }
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    r_hash = bake_hash_mix(r_hash, 2);
    hash_string(socket->identifier);
    hash_string(socket->socket_type);
  }
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      source_tree_topology_hash_recursive(*group_tree, visited, r_hash);
    }
  }
}

/** Hash a group interface socket's default value, typed through its socket typeinfo. */
static uint64_t source_hash_interface_socket(uint64_t h, const bNodeTreeInterfaceSocket &socket)
{
  const bke::bNodeSocketType *typeinfo = socket.socket_typeinfo();
  if (typeinfo == nullptr) {
    return bake_hash_mix(h, 0xFFFFFFFDu);
  }
  return source_hash_socket_value(h, typeinfo->type, socket.socket_data);
}

/**
 * The value half of the content hash: node custom fields, `node->id`, DNA storage and the default
 * values of every input and interface socket of \a tree and every group it reaches, once each.
 * Locations, parents, selection and UI flags are left out on purpose.
 */
static void source_tree_values_hash_recursive(const bNodeTree &tree,
                                              Set<const bNodeTree *> &visited,
                                              uint64_t &r_hash)
{
  if (!visited.add(&tree)) {
    return;
  }
  tree.ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    r_hash = source_hash_interface_socket(r_hash, *socket);
  }
  for (const bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    r_hash = source_hash_interface_socket(r_hash, *socket);
  }
  for (const bNode *node : tree.all_nodes()) {
    r_hash = bake_hash_mix(r_hash, uint16_t(node->custom1));
    r_hash = bake_hash_mix(r_hash, uint16_t(node->custom2));
    r_hash = bake_hash_float(r_hash, node->custom3);
    r_hash = bake_hash_float(r_hash, node->custom4);
    r_hash = node->id != nullptr ? source_hash_string(r_hash, node->id->name) :
                                   bake_hash_mix(r_hash, 0);
    r_hash = source_hash_node_storage(r_hash, *node);
    for (const bNodeSocket &socket : node->inputs) {
      r_hash = source_hash_socket_value(r_hash, socket.type, socket.default_value);
    }
    /* Output defaults carry the value of constant nodes such as Value and RGB. */
    for (const bNodeSocket &socket : node->outputs) {
      r_hash = source_hash_socket_value(r_hash, socket.type, socket.default_value);
    }
  }
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      source_tree_values_hash_recursive(*group_tree, visited, r_hash);
    }
  }
}

/** The topology-only hash of \a ma's node tree, or zero when it has none. */
uint64_t BKE_paint_layers_source_material_topology_hash(const Material &ma)
{
  if (ma.nodetree == nullptr) {
    return 0;
  }
  ma.nodetree->ensure_topology_cache();
  Set<const bNodeTree *> visited;
  uint64_t hash = 0;
  source_tree_topology_hash_recursive(*ma.nodetree, visited, hash);
  return hash;
}

/** Extend \a h with a Custom group's interface: socket order, identifiers, types and roles. */
static uint64_t bake_hash_custom_interface(uint64_t h, const bNodeTree &group)
{
  auto hash_string = [&](const char *text) {
    if (text == nullptr) {
      return bake_hash_mix(h, 0);
    }
    for (const char *c = text; *c != '\0'; c++) {
      h = bake_hash_mix(h, uint8_t(*c));
    }
    return bake_hash_mix(h, 1);
  };
  auto hash_socket = [&](const bNodeTreeInterfaceSocket &socket) {
    h = hash_string(socket.identifier);
    h = hash_string(socket.socket_type);
    h = hash_string(custom_role_get(socket.properties));
  };
  for (const bNodeTreeInterfaceSocket *socket : group.interface_inputs()) {
    h = bake_hash_mix(h, 1);
    hash_socket(*socket);
  }
  for (const bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    h = bake_hash_mix(h, 2);
    hash_socket(*socket);
  }
  return h;
}

static uint64_t bake_hash_layer(uint64_t h,
                                const Material *ma,
                                const MaterialPaintLayer &layer,
                                const bool is_child)
{
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    /* A Material layer's bake is its source material rendered into maps, and depends on nothing
     * else: the row's mask, corrections, opacity and blend apply to those maps live. Hashing them
     * here would re-render the source through EEVEE on every mask stroke or slider drag. */
    h = bake_hash_mix(h, uint8_t(layer.source));
    h = bake_hash_mix(h, layer.material != nullptr ? layer.material->id.session_uid : 0);
    if (layer.material != nullptr) {
      h = bake_hash_mix(h, BKE_paint_layers_source_material_tree_hash(*layer.material));
    }
    h = bake_hash_mix(h, layer.bake != nullptr ? uint32_t(layer.bake->size) : 0);
    /* Visibility of a top-level row is a live factor and must not move its own bake. A parent
     * folder's bake renders its children, so a child's visibility does change the parent's result
     * and is appended here. */
    if (is_child) {
      h = bake_hash_mix(h, (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1 : 0);
    }
    return h;
  }
  h = bake_hash_mix(h, uint8_t(layer.source));
  /* Which geometry map a MESH_MAP row reads decides its result; only those rows carry the field.
   * The atlas Image's identity is folded in when the owning material is known, so pointing the slot
   * at another Image invalidates the row's bake while a pixel edit (which does not change the UID)
   * does not. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    h = bake_hash_mix(h, uint8_t(layer.mesh_map_type));
    if (ma != nullptr) {
      const MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(*ma, layer.mesh_map_type);
      h = bake_hash_mix(h,
                        (slot != nullptr && slot->image != nullptr) ?
                            slot->image->id.session_uid :
                            0);
    }
  }
  h = bake_hash_mix(h, uint8_t(layer.blend));
  /* The non-visibility flags of the row itself. Visibility is appended only for a child, so the
   * row's own cache does not depend on its own on/off state. */
  h = bake_hash_mix(h, uint16_t(layer.flag & ~MA_PAINT_LAYER_ENABLED));
  /* NOTE: the hash reads `role` directly and walks the effects and mask_stack lists itself; it is
   * the serialized identity of the row, not a role query in the normal sense. A file written
   * before the split had its rows interleaved in one list and hashes differently once, so its
   * bakes are invalidated and re-rendered on first load; files written after it hash stably. */
  h = bake_hash_mix(h, uint8_t(layer.role));
  h = bake_hash_float(h, layer.opacity);
  for (const float channel : layer.fill_color) {
    h = bake_hash_float(h, channel);
  }
  for (int i = 0; i < layer.channels_num; i++) {
    const MaterialPaintLayerChannel &record = layer.channels[i];
    h = bake_hash_mix(h, uint8_t(record.channel));
    h = bake_hash_mix(h, uint8_t(record.state));
    h = bake_hash_mix(h, record.image != nullptr ? record.image->id.session_uid : 0);
    for (const float value : record.value) {
      h = bake_hash_float(h, value);
    }
  }
  /* The per (row, channel) blend/opacity overrides are baked into the row's result too, and exist
   * whether or not the pair has a record. */
  for (const MaterialPaintLayerChannelSettings &settings : layer.channel_settings) {
    h = bake_hash_mix(h, uint8_t(settings.blend));
    h = bake_hash_float(h, settings.opacity);
  }
  h = bake_hash_mix(h, layer.material != nullptr ? layer.material->id.session_uid : 0);
  /* A Material layer's bake follows edits to the source graph, which do not change its session
   * UID, so the source tree's state is part of what the bake is valid for. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material != nullptr) {
    h = bake_hash_mix(h, BKE_paint_layers_source_material_tree_hash(*layer.material));
  }
  h = bake_hash_mix(h, layer.custom_group != nullptr ? layer.custom_group->id.session_uid : 0);
  if (layer.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP && layer.custom_group != nullptr) {
    h = bake_hash_custom_interface(h, *layer.custom_group);
  }
  h = bake_hash_mix(h, layer.bake != nullptr ? uint32_t(layer.bake->size) : 0);
  h = bake_hash_idprops(h, layer.properties);
  /* Effects first, then mask items, each in its own storage order. A file whose corrections were
   * interleaved hashes differently from before the split, so its bake is invalidated once and
   * re-rendered; that is expected and settles after one re-bake. */
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    h = bake_hash_layer(h, ma, effect, true);
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    h = bake_hash_layer(h, ma, mask_item, true);
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    h = bake_hash_layer(h, ma, child, true);
  }
  if (is_child) {
    h = bake_hash_mix(h, (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1 : 0);
  }
  return h;
}

void BKE_paint_layers_bake_hash(const MaterialPaintLayer &layer, uint32_t r_hash[2])
{
  const uint64_t hash = bake_hash_layer(1469598103934665603ull, nullptr, layer, false);
  r_hash[0] = uint32_t(hash & 0xFFFFFFFFu);
  r_hash[1] = uint32_t(hash >> 32);
}

void BKE_paint_layers_bake_hash(const Material &ma,
                                const MaterialPaintLayer &layer,
                                uint32_t r_hash[2])
{
  /* The atlas identity is folded in, so a MESH_MAP row's bake invalidates when its slot changes. */
  const uint64_t hash = bake_hash_layer(1469598103934665603ull, &ma, layer, false);
  r_hash[0] = uint32_t(hash & 0xFFFFFFFFu);
  r_hash[1] = uint32_t(hash >> 32);
}

bool BKE_paint_layers_bake_is_valid(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return false;
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
  BakeMaterialSubscription *sub = bake_subscriptions().lookup_ptr(ma.id.session_uid);
  if (sub != nullptr) {
    for (const BakeLayerSubscription &entry : sub->layers) {
      if (BLI_uuid_equal(entry.marker, layer.marker) && entry.changed) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace blender