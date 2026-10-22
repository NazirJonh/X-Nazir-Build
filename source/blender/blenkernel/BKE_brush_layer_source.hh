/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Identity of what a PBR Paint brush's Layer source would bake: a row's isolated result, or a
 * layered material's whole stack. The editor caches the pixels; this is the part that has to read
 * the description, and so belongs beside it.
 */

#include <cstdint>

#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"

namespace blender {

struct Image;
struct Material;

namespace bke {

/**
 * Hash of everything the source's pixels depend on except the pixels themselves, or zero when the
 * source cannot resolve at all (no layered owner, no such row).
 *
 * ROW hashes the row's subtree. FULL folds every row of the stack in order and adds the material's
 * channel set and map size. Pixel edits are deliberately absent: an image has no content version to
 * hash, so consumers mark their cache stale from the image-change notification instead.
 */
uint64_t BKE_brush_layer_source_content_hash(const Material &owner,
                                             eBrushMaterialPaintLayerSourceScope scope,
                                             const bUUID &marker);

/**
 * Every #Image the source reads, once each, for a consumer that has to notice pixel edits.
 *
 * ROW follows the row's own subtree. FULL visits every row: a Material row contributes its source
 * material's node-tree images, every other row its own maps and those of its corrections, mask and
 * children.
 */
void BKE_brush_layer_source_images_collect(const Material &owner,
                                           eBrushMaterialPaintLayerSourceScope scope,
                                           const bUUID &marker,
                                           Vector<Image *> &r_images);

}  // namespace bke
}  // namespace blender
