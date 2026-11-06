/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Internals of the bake (`paint_layers_bake.cc`) and of the bake side of a CPU-computed mask
 * effect (`paint_layers_bake_mask_effect.cc`), shared between the two. They stay out of
 * `paint_layers_intern.hh` because nothing beside the bake reads them: the planner's branches are
 * their only callers.
 */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_uuid.h"

#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"

namespace blender {

struct Image;
struct Main;
struct MaterialPaintChannelInfo;

/**
 * The map of \a layer in \a channel at \a size, made when it is missing or of another size or
 * format.
 *
 * \param floatbuf: keep the values as float. A map of the wrong format cannot be reused: the pixel
 *                  writers index one buffer or the other, and a byte map read as float (or back)
 *                  would be a different picture.
 */
Image *bake_service_image(Main &bmain,
                          MaterialPaintLayer &layer,
                          int channel,
                          int size,
                          bool is_color,
                          bool floatbuf,
                          int object_num);

/** The map \a channel's bake stores as float rather than bytes; see #bake_service_image. */
bool bake_channel_needs_float(const MaterialPaintLayer &layer,
                              const MaterialPaintChannelInfo &info);

/**
 * Release the maps of \a layer's bake in the channels a render just left out. A channel the row no
 * longer takes part in (switched off, or a record the row no longer lays) is not rendered again, so
 * its map from an earlier bake would otherwise stand in for the row and show what the row stopped
 * laying. The caller tags #MA_PAINT_LAYERS_REGEN, as for #bake_maps_release.
 */
void bake_stale_channel_maps_release(Main &bmain,
                                     MaterialPaintLayer &layer,
                                     const Span<int> written_channels);

/**
 * Whether the row's bake subscription reports pending pixel edits. Unlike
 * #BKE_paint_layers_bake_changed_region, a full-update mark counts too: it sets
 * #BakeLayerSubscription::changed with no rectangle (#BKE_paint_layers_bake_notice_changes).
 */
bool bake_row_subscription_changed(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether a mask effect item waits for the running stroke to end before its re-bake: the same
 * policy a row's content effect runs on, with the grey's map standing where the row's maps do.
 */
bool mask_effect_deferred_by_stroke(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Append the maps \a row itself reads to \a r_images: its channel maps, the maps of its bake and,
 * for a row that reads a material, the textures that material shows live. A reader also brings the
 * maps of the slice it reads (see #bake_collect_slice_images).
 */
void bake_collect_row_maps(const Material &ma,
                           const MaterialPaintLayer &row,
                           blender::Vector<Image *> &r_images,
                           blender::Vector<bUUID> &visited);

/**
 * The structural hash of what the mask effect item \a layer's grey reads: the items before it and
 * what they render from. Zero when the item is in no mask stack yet, so there is no input to hash;
 * the caller folds the value in only then, keeping the mix step where the other hashes live.
 */
uint64_t bake_mask_effect_input_hash(const Material &ma, const MaterialPaintLayer &layer);

/**
 * The side \a row's grey lays on: the mask's canvas, which the base mask's map (or the material
 * canvas) decides. Zero when no map of the owner has dimensions yet.
 */
int bake_mask_effect_size_for(const Material &ma, const MaterialPaintLayer &row);

/**
 * As #bake_write_image, for a source sampled as it is written: \a fill_row is called per row with
 * the row's \a y and the buffer's width, and fills \a width straight RGBA floats into \a r_row. A
 * row scratch is all the source holds -- a map's width wide -- so a source that already exists as
 * a buffer of its own proportions (the mask effect's grey) never needs a second copy of the whole
 * picture to land in the map. The store path -- the buffer choice, the encoding, the premultiply
 * and the changed mark -- is the flat writer's own, so a row source and a flat one land in the map
 * alike.
 */
void bake_write_image_rows(
    Image &image,
    const bool is_color,
    const int *rect,
    const int tile_number,
    const FunctionRef<void(const int y, const int width, float *r_row)> fill_row);

/**
 * Render the CPU-computed mask effect's grey of \a layer (a mask item) into its own bake: the grey
 * the mask effect cache holds, resampled from the row's canvas to the \a size-square map, written
 * whole. True when the map was written.
 */
bool bake_mask_effect_row_write(Main &bmain, Material &ma, MaterialPaintLayer &layer, int size);

}  // namespace blender
