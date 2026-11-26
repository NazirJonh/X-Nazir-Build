/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The bake side of a CPU-computed mask effect: what the planner in `paint_layers_bake.cc` runs for
 * a mask item that bakes a grey of its own. The item's grey is one map -- no coverage -- written
 * whole on a fresh map and over the band the item's subscription reports when only pixel edits are
 * pending (see #bake_mask_effect_region_resolve), so its input collection, its stroke deferral and
 * its render live here beside the effect itself (`paint_layers_mask_effect.cc`), and the planner
 * keeps only the one-line branches that route to them.
 *
 * The same file holds the bake side of a CPU-computed *content* effect that owns its result
 * (#paint_layer_effect_owns_result): one map per channel the effect filters, written from the
 * effect cache's compute -- whole on a fresh bake, and over the rectangle the compute answers for
 * when only pixel edits are pending (see #bake_effect_row_write).
 */

#include <memory>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_perf_debug.hh"

#include "DNA_material_types.h"

#include "IMB_imbuf_types.hh"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"

namespace blender {

uint64_t bake_mask_effect_input_hash(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A kind whose grey reads an Anchor Point's slice (a Normal to Mask) reads the input channel of
   * the slice its reference names, not the mask built up to it: the slice's own description hash
   * is the input's, the same one the mask effect cache keys the grey by, so an edit above the
   * marker invalidates the bake with the cache. */
  if (paint_layer_kind_reads_anchor_point(layer)) {
    return paint_layer_anchor_point_slice_hash(
        ma, layer, paint_layer_effect_input_channel(layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  }
  /* The grey the item bakes reads the mask built up to it: which items stand before it and what
   * they render from is the input's description, the way a reader's slice hash is. */
  const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, layer);
  return owner != nullptr ? paint_layer_mask_prefix_hash(ma, *owner, layer) : 0;
}

void BKE_paint_layers_mask_effect_input_images(const Material &ma,
                                               const MaterialPaintLayer &row,
                                               const MaterialPaintLayer &effect,
                                               Vector<Image *> &r_images)
{
  Vector<bUUID> visited;
  /* An Anchor Point kind is the reader its grey reads through: the slice's own maps are the
   * input's, and the mask below it plays no part in the grey. */
  if (paint_layer_kind_reads_anchor_point(effect)) {
    BKE_paint_layers_anchor_point_slice_images(ma, effect, r_images);
    return;
  }
  /* The grey the effect reads is the base mask and the items before it, laid live: the maps each
   * of them renders from are the input's maps, a nested reader among them bringing its own slice's.
   * The effect item itself reads nothing, so the walk stops before it. A kind that reads its own
   * slot (a Channel Split) samples that image beside the grey's input; the common append takes it
   * so this reader cannot forget it. */
  paint_layer_effect_input_images_append(
      effect, r_images, [&](Vector<Image *> &r_input_images) {
        for (const MaterialPaintLayer &item : row.mask_stack) {
          if (&item == &effect) {
            break;
          }
          bake_collect_row_maps(ma, item, r_input_images, visited);
        }
      });
}

/* Whether a mask effect item waits for the running stroke to end before its re-bake: the same
 * policy a row's content effect runs on, with the grey's map standing where the row's maps do. The
 * bake is stale exactly while the subscription reports the pixel edits the grey has not absorbed,
 * and the hash, the expensive check, comes last, so outside a stroke it is not computed at all. */
bool mask_effect_deferred_by_stroke(const Material &ma, const MaterialPaintLayer &layer)
{
  if (!BKE_paint_layers_bake_stroke_active_get()) {
    return false;
  }
  /* The first bake has nothing to defer to. */
  /* A mask effect's grey lives in Base Color; an effect that owns its result may hold any channel's
   * map, so the first map it holds decides. */
  const MaterialPaintLayerBake *bake = layer.bake;
  bool has_map = false;
  if (bake != nullptr) {
    for (const Image *image : bake->images) {
      has_map = has_map || image != nullptr;
    }
  }
  if (!has_map) {
    return false;
  }
  bool changed = bake_row_subscription_changed(ma, layer);
  if (!changed) {
    /* An effect of a Material row reads the row's source, and a stroke paints that source: the
     * row's subscription reports those pixels even though the effect's own subscription does not
     * hold the live source map. The effect waits for the same stroke the row does. */
    const MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(ma, layer);
    changed = owner != nullptr && paint_layer_kind_has_flag(*owner, PAINT_LAYER_KIND_READS_SOURCE) &&
              bake_row_subscription_changed(ma, *owner);
  }
  if (!changed) {
    return false;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  return hash[0] == bake->hash[0] && hash[1] == bake->hash[1];
}

/**
 * The side \a row's grey lays on: the mask's canvas, which the base mask's map (or the material
 * canvas) decides. The owner is only asked here, once per plan per item.
 */
int bake_mask_effect_size_for(const Material &ma, const MaterialPaintLayer &row)
{
  const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, row);
  if (owner == nullptr) {
    return 0;
  }
  int width = 0;
  int height = 0;
  if (BKE_paint_layers_anchor_point_takes_row(row)) {
    /* The grey of a kind that reads a slice is the slice's own canvas (see
     * #mask_effect_dimensions): the owner's Base Color says nothing about it, and a mask over a row
     * with a flat colour would bake the grey into one texel. */
    PaintMaterialCompositeImageLayer entry;
    const bool slice_ok = BKE_paint_layers_slice_describe(
                              ma,
                              row,
                              paint_layer_effect_input_channel(row, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                              entry,
                              nullptr,
                              /*dimensions_only=*/true,
                              /*allow_empty_mask=*/false,
                              PaintLayerTileContext{}) &&
                          BKE_paint_material_composite_stack_dimensions(
                              Span<PaintMaterialCompositeImageLayer>(&entry, 1), width, height);
    return slice_ok ? width : 0;
  }
  if (BKE_paint_layers_row_dimensions(
          ma, *owner, PAINT_MATERIAL_CHANNEL_BASE_COLOR, width, height) &&
      width > 0 && height > 0)
  {
    /* The side the map takes is the content's width, its height ignored: a bake map is square by
     * construction -- one size number travels with the bake (#bake_size_for picks a row's side the
     * same way), and a source of any proportion lands in it stretched. For the mask the system
     * paints that is the canvas side; an assigned map of another proportion bakes stretched, the
     * same as a row's own map does. */
    return width;
  }
  return 0;
}

/**
 * The map rectangle the band \a valid of a \a width x \a height result lands in on a \a size-square
 * map: a map texel x samples result texel min(x * width / size, width - 1), the writes' own
 * resample, so the result columns [xmin, xmax) are read by the map columns [xmin * size / width,
 * xmax * size / width) rounded up. Rounding either end down (or out) would leave a map column
 * sampling a texel the result left as it was, over a map texel that already holds that stale value.
 * Both writers -- the mask's grey and an owned effect's channel -- derive the same numbers here.
 */
static void bake_valid_to_map_rect(const rcti &valid,
                                   const int size,
                                   const int width,
                                   const int height,
                                   int r_rect[4])
{
  r_rect[0] = clamp_i(int((int64_t(valid.xmin) * size + width - 1) / width), 0, size);
  r_rect[1] = clamp_i(int((int64_t(valid.ymin) * size + height - 1) / height), 0, size);
  r_rect[2] = clamp_i(int((int64_t(valid.xmax) * size + width - 1) / width), 0, size);
  r_rect[3] = clamp_i(int((int64_t(valid.ymax) * size + height - 1) / height), 0, size);
}

/**
 * Write \a grey into \a tile_number of \a image: the nearest sample of the grey, each axis
 * independently, straight into the map's own buffer. The row scratch in #bake_write_image_rows is
 * all the extra memory, so the peak holds the grey and the map, never a third copy of the picture
 * between them. The stretch is the one a row's bake lays its source canvas by (see
 * #bake_mask_effect_size_for for the side).
 *
 * \param rect: the map rectangle to write, or null for the whole map. A regional write lays the
 *        band the grey answers for; every pixel outside it is the previous result, untouched.
 */
static void bake_mask_effect_grey_write(Image &image,
                                        const bool is_color,
                                        const ImBuf &grey,
                                        const int size,
                                        const int tile_number,
                                        const int *rect)
{
  const int width = grey.x;
  const int height = grey.y;
  /* The cache keeps one scalar float per texel; a bake map is RGBA, so every channel of the row
   * takes the grey and the map stays opaque. */
  const float *src = grey.float_data();
  const int channels = grey.channels == 0 ? 4 : grey.channels;
  bake_write_image_rows(
      image, is_color, rect, tile_number,
      [&](const int y, const int map_width, float *r_row) {
        const int src_y = paint_layer_nearest_resample_texel(y, size, height);
        const float *src_row = src + int64_t(src_y) * width * channels;
        for (const int x : IndexRange(map_width)) {
          const int src_x = paint_layer_nearest_resample_texel(x, map_width, width);
          const float value = src_row[int64_t(src_x) * channels];
          r_row[x * 4 + 0] = value;
          r_row[x * 4 + 1] = value;
          r_row[x * 4 + 2] = value;
          r_row[x * 4 + 3] = 1.0f;
        }
      });
}

/**
 * Render the CPU-computed mask effect's grey of \a layer (a mask item) into its own bake: the grey
 * the mask effect cache holds, resampled from the row's canvas to the \a size-square map. The
 * item's bake holds the grey in its Base Color map and no coverage -- the grey is read like a
 * painted mask item's map, and the validity is the hash's and the subscription's, which a coverage
 * map plays no part in. True when the map was written.
 *
 * With \a objects (a grey that reads the surface, #paint_layers_bake_node_reads_surface) the grey is
 * one render per object and UV tile of its block, each into its own tile of one tiled map, and the
 * bake keeps the tile records and identities a row's per-object bake does. Without them it is the one
 * shared map it always was.
 *
 * \param changed_region: the rectangle whose inputs changed since the grey map was last written, or
 *        null for the whole canvas (see #bake_mask_effect_region_resolve). Only the shared map takes
 *        it: the cache answers with the band grown by the kernel's halo, and the map keeps every
 *        texel outside it. The per-object path always writes whole -- a tiled map holds one result
 *        per object and tile, and no single band names all of them. A fresh or replaced map also
 *        writes whole, since it holds no previous result outside the band.
 */
bool bake_mask_effect_row_write(Main &bmain,
                                Material &ma,
                                MaterialPaintLayer &layer,
                                const int size,
                                const Span<PaintLayersBakeTarget> objects,
                                const rcti *changed_region)
{
  const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, layer);
  if (owner == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo *base_info = nullptr;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      base_info = &info;
      break;
    }
  }
  if (base_info == nullptr) {
    return false;
  }
  const bool floatbuf = bake_channel_needs_float(layer, *base_info);

  if (objects.is_empty()) {
    /* Only a map that already stood may take a band: an image created just now holds no previous
     * result outside the write to keep, so such a bake is written whole whatever the caller asked.
     * The identity is what tells a reuse from a replacement below (#bake_service_image may make a
     * fresh map of another size or format), the same check the owned effect's writer makes. */
    Image *slot_before = layer.bake != nullptr ?
                             layer.bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] :
                             nullptr;
    const Image *map_before = (slot_before != nullptr &&
                               bake_service_image_is_live(bmain, slot_before)) ?
                                  slot_before :
                                  nullptr;
    const bool has_map = map_before != nullptr;
    const uint32_t map_uid_before = has_map ? map_before->id.session_uid : 0;
    rcti valid_region;
    BLI_rcti_init(&valid_region, 0, 0, 0, 0);
    std::shared_ptr<ImBuf> grey = paint_layer_mask_effect_buffer_get(
        ma,
        *owner,
        layer,
        /*describe_only=*/false,
        nullptr,
        PAINT_TILE_NUMBER_BASE,
        nullptr,
        (changed_region != nullptr && has_map) ? changed_region : nullptr,
        &valid_region);
    if (grey == nullptr || grey->float_data() == nullptr) {
      return false;
    }
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
    bake->size = size;
    if (!BLI_listbase_is_empty(&bake->tiles) || bake->object_count != 0) {
      /* The grey stopped reading the surface: the per-object records describe a map that goes. */
      paint_layers_bake_tiles_clear(*bake);
      paint_layers_bake_tile_indices_clear_all(ma);
    }
    Image *image = bake_service_image(
        bmain, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, base_info->is_color, floatbuf, 0);
    if (image == nullptr) {
      return false;
    }
    /* An empty valid region says the current inputs move no texel of the grey: the map already is
     * the picture the bake owes, and only the subscription is left holding the edits that turned out
     * not to move it. Nothing is written; the finalize below re-creates the subscription with a
     * drain, which is what lets the row stop being stale. */
    if (changed_region != nullptr && BLI_rcti_is_empty(&valid_region)) {
      BKE_paint_layers_bake_finalize(ma, layer);
      return false;
    }
    int map_rect[4] = {0, 0, 0, 0};
    const int *rect = nullptr;
    if (changed_region != nullptr && has_map && image->id.session_uid == map_uid_before) {
      /* The region the grey answers for, clamped to its own texels first: the answer's band is in
       * its grid, the map's rectangle derived from it below. A map the write would replace (it did
       * not stand before this bake) leaves `changed_region` out of the ask above, so its band is the
       * whole canvas and the write is whole. */
      valid_region.xmin = clamp_i(valid_region.xmin, 0, grey->x);
      valid_region.ymin = clamp_i(valid_region.ymin, 0, grey->y);
      valid_region.xmax = clamp_i(valid_region.xmax, 0, grey->x);
      valid_region.ymax = clamp_i(valid_region.ymax, 0, grey->y);
      bake_valid_to_map_rect(valid_region, size, grey->x, grey->y, map_rect);
      rect = map_rect;
    }
    bake_mask_effect_grey_write(*image, base_info->is_color, *grey, size, 0, rect);
    bake_stale_channel_maps_release(
        bmain, layer, Vector<int>({PAINT_MATERIAL_CHANNEL_BASE_COLOR}));
    BKE_paint_layers_bake_finalize(ma, layer);
    /* The cache entry the write went through stays: the live grey is the base the next regional
     * patch builds on, and dropping it would turn every bake into a full recompute again. The LRU
     * budget, not the bake, decides how long it stands -- the same rule the owned effects keep. */
    return true;
  }

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  bake->size = size;
  paint_layers_bake_tiles_clear(*bake);
  const Vector<int> image_tiles = bake_targets_image_tiles(objects);
  for (const int object_index : objects.index_range()) {
    const PaintLayersBakeTarget &target = objects[object_index];
    for (const PaintLayersBakeTilePair &pair : bake_target_tile_pairs(target, object_index)) {
      /* The UV tile is named, as for a row: the cache keys the grey on it and a projected item
       * rasterizes into it. The object is the surface the projection is read through. */
      const PaintLayerSurfaceRef painted_object(target.object);
      std::shared_ptr<ImBuf> grey = paint_layer_mask_effect_buffer_get(
          ma, *owner, layer, /*describe_only=*/false, nullptr, pair.uv_tile, painted_object);
      if (grey == nullptr || grey->float_data() == nullptr) {
        return false;
      }
      Image *image = bake_service_image(bmain,
                                        layer,
                                        PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                        size,
                                        base_info->is_color,
                                        floatbuf,
                                        int(objects.size()),
                                        image_tiles);
      if (image == nullptr) {
        return false;
      }
      bake_mask_effect_grey_write(
          *image, base_info->is_color, *grey, size, pair.image_tile, nullptr);
      grey.reset();
      /* The tile's cache entry stays, as on the shared map above: the live grey is the base the next
       * patch builds on, and only the LRU budget retires it. */
    }
    MaterialPaintLayerBakeTile *tile = bake_tile_ensure(
        *bake, target.object_uid, target.block_origin[0], target.block_origin[1]);
    const uint64_t identity = paint_layers_bake_row_identity(ma, layer, target, size);
    tile->hash[0] = uint32_t(identity & 0xFFFFFFFFu);
    tile->hash[1] = uint32_t(identity >> 32);
  }
  bake->object_count = int(objects.size());
  bake_stale_channel_maps_release(bmain, layer, Vector<int>({PAINT_MATERIAL_CHANNEL_BASE_COLOR}));
  BKE_paint_layers_bake_finalize(ma, layer);
  bake_object_identity_record(ma, layer, paint_layers_bake_row_identity(ma, layer, objects[0], size));
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Effect that owns its result
 * \{ */

int bake_effect_size_for(const Material &ma, const MaterialPaintLayer &effect)
{
  const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, effect);
  if (row == nullptr) {
    return 0;
  }
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    int width = 0;
    int height = 0;
    if (paint_layer_effect_filters(ma, effect, channel) &&
        BKE_paint_layers_row_dimensions(
            ma, *row, paint_layer_effect_input_channel(effect, channel), width, height) &&
        width > 0 &&
        height > 0)
    {
      /* The side is the content's width, as a mask effect's grey and a row's bake take it (see
       * #bake_mask_effect_size_for): one size number travels with a bake and its maps are square. */
      return width;
    }
  }
  return 0;
}

bool bake_effect_row_write(Main &bmain,
                           Material &ma,
                           MaterialPaintLayer &effect,
                           const rcti *changed_region)
{
  const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, effect);
  if (row == nullptr) {
    return false;
  }
  bke::paint_perf::ScopedTimer perf_total("effect.own_bake.total");
  Vector<int> written_channels;
  int size = 0;
  /* Whether every channel the effect filters ended current: one the compute answered with a valid
   * region is written below, one it answered with an empty one already holds the result in its map,
   * and only a channel the compute could not answer leaves the bake stale. */
  bool all_channels_current = true;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const int channel = int(info.channel);
    if (!paint_layer_effect_filters(ma, effect, channel)) {
      continue;
    }
    /* One channel at a time: the compute is a whole canvas of float RGBA, and the buffer is let go
     * before the next channel's is made, so the peak holds one buffer and one map. */
    /* A channel whose map the bake does not hold yet cannot take a partial write -- an image created
     * just now has no previous result for the region outside the write to keep -- so such a channel
     * computes whole whatever the caller asked. */
    const bool has_map = effect.bake != nullptr && effect.bake->images[channel] != nullptr;
    rcti valid_region;
    BLI_rcti_init(&valid_region, 0, 0, 0, 0);
    std::shared_ptr<ImBuf> result = paint_layer_owned_effect_result_compute(
        ma,
        *row,
        effect,
        channel,
        PAINT_TILE_NUMBER_BASE,
        (changed_region != nullptr && has_map) ? changed_region : nullptr,
        &valid_region);
    if (result == nullptr || result->float_data() == nullptr) {
      /* The input cannot be rendered: the map keeps what it holds, which is stale until a plan the
       * input can answer. */
      all_channels_current = false;
      continue;
    }
    const int width = result->x;
    const int height = result->y;
    if (width <= 0 || height <= 0) {
      /* A buffer with no texels is no answer either: the map keeps what it holds, stale the same
       * way. */
      all_channels_current = false;
      continue;
    }
    /* The compute answers for texels of the buffer it returned; keep the region it names within. */
    valid_region.xmin = clamp_i(valid_region.xmin, 0, width);
    valid_region.xmax = clamp_i(valid_region.xmax, 0, width);
    valid_region.ymin = clamp_i(valid_region.ymin, 0, height);
    valid_region.ymax = clamp_i(valid_region.ymax, 0, height);
    /* An empty valid region says the current inputs move no texel of the result: the map already is
     * the picture the bake owes, and only the subscription is left holding the edits that turned out
     * not to move it. Nothing is written here; the finalize below re-creates the subscription with a
     * drain, which is what lets the row stop being stale. */
    if (BLI_rcti_is_empty(&valid_region)) {
      /* Nothing moved: the cache entry stays, and the next regional patch builds on it the way it
       * would after a write -- the LRU budget, not the bake, decides how long it stands. */
      continue;
    }
    /* The side the first channel lands at is the bake's: the maps share one size number. */
    if (size == 0) {
      size = width;
    }
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(effect);
    bake->size = size;
    /* The map the write would land in, and the identity that tells a reuse from a replacement: a
     * map #bake_service_image finds of another size or format is freed and a fresh blank one made,
     * whose texels outside the rectangle hold no previous result. The session id survives a reuse
     * and a fresh image gets a new one, so it is trusted where a freed map may hand its address
     * back to the allocator. A slot an undo or a reset left pointing at a freed image is read as no
     * map: its id would be a use after free, and the service would make a new one anyway. */
    Image *slot_before = bake->images[channel];
    Image *map_before = (slot_before != nullptr && bake_service_image_is_live(bmain, slot_before)) ?
                            slot_before :
                            nullptr;
    const uint32_t map_uid_before = map_before != nullptr ? map_before->id.session_uid : 0;
    Image *image = bake_service_image(
        bmain, effect, channel, size, info.is_color, bake_channel_needs_float(effect, info), 0);
    if (image == nullptr) {
      all_channels_current = false;
      continue;
    }
    /* The nearest sample of the result, each axis independently, written straight into the map's own
     * buffer (see #bake_mask_effect_row_write). The effect cache's buffer is premultiplied by the
     * input's content alpha, the composite straightens it before it mixes, and the map holds the
     * straight colour the same way: the alpha is not part of the result -- except for a kind whose
     * result is a layer (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA), whose alpha is what it covers by and
     * is kept in the map. */
    const bool keeps_alpha = paint_layer_kind_result_has_alpha(effect);
    const float *src = result->float_data();
    /* The map rectangle the valid region of the result lands in, or whole when no region was asked
     * or the map cannot take one -- #bake_valid_to_map_rect derives the numbers from the resample
     * both writers share (see #bake_mask_effect_grey_write).
     *
     * Only a partial write into the map that was already there is safe. A map #bake_service_image
     * replaced (its size or format moved) is written whole: the cache answers for the whole
     * buffer, so the full write is correct without a second compute, and a partial one would leave
     * the fresh map black outside the rectangle. Whole-canvas valid regions round to the whole
     * map, and a channel that was computed whole -- no region asked, or no map for the region to
     * land in -- writes whole, as it did before the region path. */
    const bool regional = changed_region != nullptr && has_map && map_before != nullptr &&
                          image->id.session_uid == map_uid_before;
    int map_rect[4] = {0, 0, 0, 0};
    const int *rect = nullptr;
    if (regional) {
      bake_valid_to_map_rect(valid_region, size, width, height, map_rect);
      rect = map_rect;
    }
    bake_write_image_rows(
        *image, info.is_color, rect, 0,
        [&](const int y, const int map_width, float *r_row) {
          const int src_y = paint_layer_nearest_resample_texel(y, size, height);
          const float *src_row = src + int64_t(src_y) * width * 4;
          for (const int x : IndexRange(map_width)) {
            const int src_x = paint_layer_nearest_resample_texel(x, map_width, width);
            const float *texel = src_row + int64_t(src_x) * 4;
            const float inv = texel[3] > 0.0f ? 1.0f / texel[3] : 1.0f;
            r_row[x * 4 + 0] = texel[0] * inv;
            r_row[x * 4 + 1] = texel[1] * inv;
            r_row[x * 4 + 2] = texel[2] * inv;
            r_row[x * 4 + 3] = keeps_alpha ? texel[3] : 1.0f;
          }
        });
    written_channels.append(channel);
    result.reset();
    /* The entry the bake computed through stays: it is the living result the next regional patch
     * builds on -- dropping it would turn every bake into a full recompute again. The LRU budget
     * is what retires it, the same way it retires the preview's own results. */
  }
  /* A channel the compute could not answer -- no result, no texels, no map -- leaves the bake
   * stale: finalizing would stamp the hash and re-create the subscription as if every filtered map
   * held the current result, while one of them still shows an older picture. The channels already
   * written stay (they are current), no map is released, and the next plan computes the row again
   * -- whole when its description moved, by region otherwise. */
  if (!all_channels_current) {
    if (effect.bake != nullptr) {
      /* The hash the bake still carries may describe an older, successful pass -- a map restored
       * by an undo keeps the hash stored with it, while the input needed to fill it is not ready.
       * Stamping nothing here would leave #BKE_paint_layers_bake_is_valid answering true for a map
       * this pass could not confirm, so the caller would keep showing the restored pixels. Dropping
       * the hash makes the next plan ask for the channels again. */
      effect.bake->hash[0] = 0;
      effect.bake->hash[1] = 0;
    }
    perf_total.note("kind=%d channels=%d incomplete",
                    int(effect.kind_subtype),
                    int(written_channels.size()));
    return !written_channels.is_empty();
  }
  if (written_channels.is_empty()) {
    /* Every filtered channel ended current without a write: the maps hold the result the current
     * inputs compute, so the bake is done -- only its subscription still reports the edits the
     * compute answered with empty regions. The finalize stamps the hash and re-creates the
     * subscription with a drain, and the caller learns false, as it does for a bake that wrote
     * nothing: no map changed, so no map needs the commit's updates. */
    BKE_paint_layers_bake_finalize(ma, effect);
    perf_total.note("kind=%d channels=0 up_to_date", int(effect.kind_subtype));
    return false;
  }
  /* Only the whole-canvas compute releases the maps it left out. A regional write runs only while
   * the description stands -- a change of the effect's channel set moves the structural hash and
   * comes to the writer as a whole compute -- so every filtered channel of a regional bake is
   * current: written, or already holding the result. A map the write left out then is not a record
   * the effect stopped laying but a current map, and releasing it would drop it from the row. */
  if (changed_region == nullptr) {
    bake_stale_channel_maps_release(bmain, effect, written_channels);
  }
  BKE_paint_layers_bake_finalize(ma, effect);
  perf_total.note("kind=%d size=%d channels=%d",
                  int(effect.kind_subtype),
                  size,
                  int(written_channels.size()));
  return true;
}

/** \} */

}  // namespace blender
