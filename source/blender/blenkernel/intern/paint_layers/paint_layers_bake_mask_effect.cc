/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The bake side of a CPU-computed mask effect: what the planner in `paint_layers_bake.cc` runs for
 * a mask item that bakes a grey of its own. The item's grey is one map written whole -- no
 * coverage, no regions -- so its input collection, its stroke deferral and its render live here
 * beside the effect itself (`paint_layers_mask_effect.cc`), and the planner keeps only the
 * one-line branches that route to them.
 *
 * The same file holds the bake side of a CPU-computed *content* effect that owns its result
 * (#paint_layer_effect_owns_result): one map per channel the effect filters, written whole from the
 * effect cache's compute.
 */

#include <memory>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
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
  if (!bake_row_subscription_changed(ma, layer)) {
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
 * Write \a grey into \a tile_number of \a image: the nearest sample of the grey, each axis
 * independently, straight into the map's own buffer. The row scratch in #bake_write_image_rows is
 * all the extra memory, so the peak holds the grey and the map, never a third copy of the picture
 * between them. The stretch is the one a row's bake lays its source canvas by (see
 * #bake_mask_effect_size_for for the side).
 */
static void bake_mask_effect_grey_write(Image &image,
                                        const bool is_color,
                                        const ImBuf &grey,
                                        const int size,
                                        const int tile_number)
{
  const int width = grey.x;
  const int height = grey.y;
  const float *src = grey.float_data();
  bake_write_image_rows(
      image, is_color, nullptr, tile_number,
      [&](const int y, const int map_width, float *r_row) {
        const int src_y = paint_layer_nearest_resample_texel(y, size, height);
        const float *src_row = src + int64_t(src_y) * width * 4;
        for (const int x : IndexRange(map_width)) {
          const int src_x = paint_layer_nearest_resample_texel(x, map_width, width);
          const float *texel = src_row + int64_t(src_x) * 4;
          r_row[x * 4 + 0] = texel[0];
          r_row[x * 4 + 1] = texel[1];
          r_row[x * 4 + 2] = texel[2];
          r_row[x * 4 + 3] = 1.0f;
        }
      });
}

/**
 * Render the CPU-computed mask effect's grey of \a layer (a mask item) into its own bake: the grey
 * the mask effect cache holds, resampled from the row's canvas to the \a size-square map, written
 * whole. The item's bake holds the grey in its Base Color map and no coverage -- the grey is read
 * like a painted mask item's map, and the validity is the hash's and the subscription's, which a
 * coverage map plays no part in. True when the map was written.
 *
 * With \a objects (a grey that reads the surface, #paint_layers_bake_node_reads_surface) the grey is
 * one render per object and UV tile of its block, each into its own tile of one tiled map, and the
 * bake keeps the tile records and identities a row's per-object bake does. Without them it is the one
 * shared map it always was.
 */
bool bake_mask_effect_row_write(Main &bmain,
                                Material &ma,
                                MaterialPaintLayer &layer,
                                const int size,
                                const Span<PaintLayersBakeTarget> objects)
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
    std::shared_ptr<ImBuf> grey = paint_layer_mask_effect_buffer_get(
        ma, *owner, layer, /*describe_only=*/false, nullptr, PAINT_TILE_NUMBER_BASE);
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
    bake_mask_effect_grey_write(*image, base_info->is_color, *grey, size, 0);
    bake_stale_channel_maps_release(
        bmain, layer, Vector<int>({PAINT_MATERIAL_CHANNEL_BASE_COLOR}));
    BKE_paint_layers_bake_finalize(ma, layer);
    /* The bake holds the grey now: the cache entry is a copy kept for nothing while the bake
     * stands, and the composite and the graph read the bake. The cache computes the grey back when
     * the bake goes stale -- an edit or a stroke -- exactly as it did before the first bake. */
    paint_layer_mask_effect_cache_drop(ma, *owner, layer);
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
      bake_mask_effect_grey_write(*image, base_info->is_color, *grey, size, pair.image_tile);
      grey.reset();
      paint_layer_mask_effect_cache_drop(ma, *owner, layer, pair.uv_tile, painted_object);
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

bool bake_effect_row_write(Main &bmain, Material &ma, MaterialPaintLayer &effect)
{
  const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, effect);
  if (row == nullptr) {
    return false;
  }
  bke::paint_perf::ScopedTimer perf_total("effect.own_bake.total");
  Vector<int> written_channels;
  int size = 0;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const int channel = int(info.channel);
    if (!paint_layer_effect_filters(ma, effect, channel)) {
      continue;
    }
    /* One channel at a time: the compute is a whole canvas of float RGBA, and the buffer is let go
     * before the next channel's is made, so the peak holds one buffer and one map. */
    std::shared_ptr<ImBuf> result = paint_layer_owned_effect_result_compute(
        ma, *row, effect, channel, PAINT_TILE_NUMBER_BASE);
    if (result == nullptr || result->float_data() == nullptr) {
      continue;
    }
    const int width = result->x;
    const int height = result->y;
    /* The side the first channel lands at is the bake's: the maps share one size number. */
    if (size == 0) {
      size = width;
    }
    if (width <= 0 || height <= 0 || size <= 0) {
      continue;
    }
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(effect);
    bake->size = size;
    Image *image = bake_service_image(
        bmain, effect, channel, size, info.is_color, bake_channel_needs_float(effect, info), 0);
    if (image == nullptr) {
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
    bake_write_image_rows(
        *image, info.is_color, nullptr, 0,
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
    /* The bake holds the result now: the cache entry is a copy kept for nothing while it stands. */
    paint_layer_owned_effect_cache_drop(ma, effect, channel, PAINT_TILE_NUMBER_BASE);
  }
  if (written_channels.is_empty()) {
    return false;
  }
  bake_stale_channel_maps_release(bmain, effect, written_channels);
  BKE_paint_layers_bake_finalize(ma, effect);
  perf_total.note("kind=%d size=%d channels=%d",
                  int(effect.kind_subtype),
                  size,
                  int(written_channels.size()));
  return true;
}

/** \} */

}  // namespace blender
