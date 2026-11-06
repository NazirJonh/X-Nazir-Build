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
 */

#include <memory>

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_vector.hh"

#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"

#include "IMB_imbuf_types.hh"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"

namespace blender {

uint64_t bake_mask_effect_input_hash(const Material &ma, const MaterialPaintLayer &layer)
{
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
  /* The grey the effect reads is the base mask and the items before it, laid live: the maps each
   * of them renders from are the input's maps, a nested reader among them bringing its own slice's.
   * The effect item itself reads nothing, so the walk stops before it. */
  for (const MaterialPaintLayer &item : row.mask_stack) {
    if (&item == &effect) {
      break;
    }
    bake_collect_row_maps(ma, item, r_images, visited);
  }
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
  const MaterialPaintLayerBake *bake = layer.bake;
  if (bake == nullptr || bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] == nullptr) {
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
 * Render the CPU-computed mask effect's grey of \a layer (a mask item) into its own bake: the grey
 * the mask effect cache holds, resampled from the row's canvas to the \a size-square map, written
 * whole. The item's bake holds the grey in its Base Color map and no coverage -- the grey is read
 * like a painted mask item's map, and the validity is the hash's and the subscription's, which a
 * coverage map plays no part in. True when the map was written.
 */
bool bake_mask_effect_row_write(Main &bmain,
                                Material &ma,
                                MaterialPaintLayer &layer,
                                const int size)
{
  const MaterialPaintLayer *owner = BKE_paint_layers_mask_item_owner(ma, layer);
  if (owner == nullptr) {
    return false;
  }
  std::shared_ptr<ImBuf> grey = paint_layer_mask_effect_buffer_get(
      ma, *owner, layer, /*describe_only=*/false, nullptr);
  if (grey == nullptr || grey->float_buffer.data == nullptr) {
    return false;
  }
  const int width = grey->x;
  const int height = grey->y;
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
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  bake->size = size;
  Image *image = bake_service_image(bmain,
                                    layer,
                                    PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                    size,
                                    base_info->is_color,
                                    bake_channel_needs_float(layer, *base_info),
                                    0);
  if (image == nullptr) {
    return false;
  }
  /* The nearest sample of the grey, each axis independently, written straight into the map's own
   * buffer: the row scratch in #bake_write_image_rows is all the extra memory, so the peak holds
   * the grey and the map, never a third copy of the picture between them. The stretch is the one a
   * row's bake lays its source canvas by (see #bake_mask_effect_size_for for the side). */
  const float *src = grey->float_buffer.data;
  bake_write_image_rows(
      *image, base_info->is_color, nullptr, 0,
      [&](const int y, const int map_width, float *r_row) {
        const int src_y = min_ii(int(int64_t(y) * height / size), height - 1);
        const float *src_row = src + int64_t(src_y) * width * 4;
        for (const int x : IndexRange(map_width)) {
          const int src_x = min_ii(int(int64_t(x) * width / size), width - 1);
          const float *texel = src_row + int64_t(src_x) * 4;
          r_row[x * 4 + 0] = texel[0];
          r_row[x * 4 + 1] = texel[1];
          r_row[x * 4 + 2] = texel[2];
          r_row[x * 4 + 3] = 1.0f;
        }
      });
  bake_stale_channel_maps_release(bmain, layer, Vector<int>({PAINT_MATERIAL_CHANNEL_BASE_COLOR}));
  BKE_paint_layers_bake_finalize(ma, layer);
  /* The bake holds the grey now: the cache entry is a copy kept for nothing while the bake stands,
   * and the composite and the graph read the bake. The cache computes the grey back when the bake
   * goes stale -- an edit or a stroke -- exactly as it did before the first bake. */
  paint_layer_mask_effect_cache_drop(ma, *owner, layer);
  return true;
}

}  // namespace blender
