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

/**
 * The stack row \a marker names, or null when it names nothing or a correction/mask item.
 * Corrections and mask items are addressed by marker too, but they are not rows of the stack and
 * cannot be a brush source of their own.
 */
MaterialPaintLayer *source_row_get(const Material &owner, const bUUID &marker)
{
  if (BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  MaterialPaintLayer *row = BKE_paint_layers_find(const_cast<Material &>(owner), marker);
  if (row == nullptr ||
      BKE_paint_layers_owner_list(const_cast<Material &>(owner), *row) == nullptr)
  {
    return nullptr;
  }
  return row;
}

uint64_t row_hash(const Material &owner, const MaterialPaintLayer &row)
{
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(owner, row, hash);
  return (uint64_t(hash[1]) << 32) | uint64_t(hash[0]);
}

/**
 * Mapping values of \a row and everything under it.
 *
 * The row-bake hash deliberately ignores them: a mapped row stays live and is never baked by the
 * planner. A brush source does bake the row's content with its mapping applied, so for this cache
 * a moved slider has to change the key.
 */
uint64_t mapping_hash(const Material &owner, const MaterialPaintLayer &row, uint64_t hash)
{
  if (row.material != nullptr) {
    /* A Material row's structural hash only names its source; the source's graph and values are
     * what its content actually is. */
    hash = get_default_hash(
        hash, BKE_paint_layers_source_material_tree_hash_cached(*row.material));
  }
  if (BKE_paint_layers_mapping_remaps(owner, row)) {
    hash = get_default_hash(hash,
                            row.mapping.offset[0],
                            row.mapping.offset[1],
                            row.mapping.scale[0],
                            row.mapping.scale[1]);
    hash = get_default_hash(hash,
                            row.mapping.rotation,
                            int(row.mapping.space),
                            int(row.mapping.flag),
                            int(BKE_paint_layers_mapping_wrap_get(row)));
  }
  for (const MaterialPaintLayer &effect : row.effects) {
    hash = mapping_hash(owner, effect, hash);
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    hash = mapping_hash(owner, item, hash);
  }
  for (const MaterialPaintLayer &child : row.children) {
    hash = mapping_hash(owner, child, hash);
  }
  return hash;
}

}  // namespace

uint64_t BKE_brush_layer_source_content_hash(const Material &owner,
                                             const eBrushMaterialPaintLayerSourceScope scope,
                                             const bUUID &marker)
{
  if (!BKE_paint_layers_is_layered(owner)) {
    return 0;
  }

  if (scope == BRUSH_MATERIAL_PAINT_LAYER_SOURCE_SCOPE_ROW) {
    const MaterialPaintLayer *row = source_row_get(owner, marker);
    if (row == nullptr) {
      return 0;
    }
    /* A zero pair is the planner's "nothing rendered yet" state, not a usable key. */
    return mapping_hash(owner, *row, row_hash(owner, *row));
  }

  uint64_t hash = layer_source_fnv_offset;
  bool any_row = false;
  BKE_paint_layers_foreach(
      owner, [&](const MaterialPaintLayer &row, const MaterialPaintLayer * /*parent*/) {
        hash = get_default_hash(hash, mapping_hash(owner, row, row_hash(owner, row)));
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
    const MaterialPaintLayer *row = source_row_get(owner, marker);
    if (row != nullptr) {
      BKE_paint_layers_bake_collect_source_images(owner, *row, r_images);
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
