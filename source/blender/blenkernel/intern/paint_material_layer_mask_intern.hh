/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The mask half of #BKE_paint_material_layer_edit.hh, implemented in
 * `paint_material_layer_mask.cc`: finding a row's mask by its tag, reading the switched-on state,
 * and the coverage the row falls back to while its mask is off. Shared with
 * `paint_material_layer_channels.cc` and `paint_material_layer_corrections.cc`, which read the
 * same mask facts when they rebuild the graph; keeping them here is what makes one mask have one
 * meaning across the three files.
 */

#include "paint_material_layer_edit_intern.hh"

namespace blender {

/**
 * The mask image #Image::paint_layer_channel tags as the mask of the layer carrying \a marker,
 * across \a bmain's images, or null. A row's mask is one image shared by every channel, so a
 * reader that only has the marker finds it here -- a switched-off mask has no link to follow.
 */
Image *paint_layer_mask_image_find(Main &bmain, const bUUID &marker);

/** The mask Image Texture node of the layer carrying \a marker, in \a tree, linked or not, or
 * null. */
bNode *layer_mask_node_find(bNodeTree &tree, const bUUID &marker);

/**
 * Whether \a image, a mask image, is switched on. The state is stored inverted in DNA
 * (#Image::paint_layer_mask_disabled); every reader goes through here so the inversion lives in
 * one place.
 */
bool paint_layer_mask_is_enabled(const Image &image);

/**
 * What covers the row when its mask is off, the order #layer_mask_corrections_sync and the mask
 * toggle agree on: what its content corrections accumulate over the map, else a folder's top link
 * Alpha, else the row's own map Alpha (#ChainLayer::base_map, which sits below the corrections).
 * Both null when nothing covers the row.
 */
void layer_coverage_source_without_mask(const ChainLayer &layer,
                                        bNode *&r_node,
                                        bNodeSocket *&r_socket);

}  // namespace blender
