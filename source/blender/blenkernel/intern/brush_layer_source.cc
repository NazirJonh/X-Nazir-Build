/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_brush_layer_source.hh"

#include "BLI_hash.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_set.hh"

#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"

namespace blender::bke {

namespace {

constexpr uint64_t layer_source_fnv_offset = 1469598103934665603ull;

}  // namespace

uint64_t BKE_brush_layer_source_content_hash(const Material &owner,
                                             const eBrushMaterialPaintLayerSourceScope scope,
                                             const bUUID &marker)
{
  if (!BKE_paint_layers_is_layered(owner)) {
    return 0;
  }

  if (scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW) {
    /* The row the marker names, or nothing: only rows of the stack can be a brush source, the same
     * test a narrowed Material row's reference passes. */
    const MaterialPaintLayer *row = BKE_paint_layers_source_row_find(owner, marker);
    if (row == nullptr) {
      return 0;
    }
    /* A zero pair is the planner's "nothing rendered yet" state, not a usable key. */
    return BKE_paint_layers_source_row_content_fold(owner, *row);
  }

  uint64_t hash = layer_source_fnv_offset;
  bool any_row = false;
  BKE_paint_layers_foreach(
      owner, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) {
        hash = get_default_hash(hash, BKE_paint_layers_source_row_content_fold(owner, row));
        any_row = true;
        return true;
      });
  if (!any_row) {
    return 0;
  }
  /* The channel set decides which rows are in the generated tree at all; the map size decides
   * the resolution a Material row's service maps are rendered at. */
  hash = get_default_hash(hash, owner.paint_layers_channels, owner.paint_layers_image_size);
  return hash;
}

void BKE_brush_layer_source_images_collect(const Material &owner,
                                           const eBrushMaterialPaintLayerSourceScope scope,
                                           const bUUID &marker,
                                           Vector<Image *> &r_images)
{
  if (!BKE_paint_layers_is_layered(owner)) {
    return;
  }
  if (scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW) {
    const MaterialPaintLayer *row = BKE_paint_layers_source_row_find(owner, marker);
    if (row != nullptr) {
      /* The brush bakes the row as it looks, so its own corrections, mask and children are part of
       * what the bake reads -- a Material row's own collector stops at its source and would leave
       * them out. */
      BKE_paint_layers_bake_collect_row_tree_images(owner, *row, r_images);
    }
    return;
  }

  /* A Material row's own collector stops at its source tree; the rows under it, and the masks and
   * corrections of every row, are visited here in their own right. */
  Set<Image *> seen;
  BKE_paint_layers_foreach(
      owner, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) {
        Vector<Image *> row_images;
        BKE_paint_layers_bake_collect_source_images(owner, row, row_images);
        for (Image *image : row_images) {
          if (seen.add(image)) {
            r_images.append(image);
          }
        }
        return true;
      });
}

}  // namespace blender::bke
