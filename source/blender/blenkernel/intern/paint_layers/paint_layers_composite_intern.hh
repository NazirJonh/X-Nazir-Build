/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * What the description -> image-layer bridge spreads over more than one file: the row builder and
 * the list walk (`paint_layers_composite.cc`), and the Compare mask renderer
 * (`paint_layers_composite_height.cc`), which needs both. Internal to the module, so it lives
 * beside the implementation rather than in a public BKE header: nothing outside these two files
 * builds a stack from a description.
 */

#pragma once

#include <memory>

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"

namespace blender {

/**
 * Describe the row \a layer (not a Pass Through folder) in \a channel to the evaluator. False when
 * the row takes no part, which the generator leaves out as well.
 *
 * A row whose bake stands in (#paint_layer_row_substituted) is one plain layer of its baked colour
 * and coverage maps: the bake already holds the row's corrections, mask and opacity, so nothing of
 * them is described again. \a ignore_own_bake describes the live row instead, which is what the
 * bake itself renders from.
 */
bool composite_image_layer_build(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 const int channel,
                                 const float pass_through_scale,
                                 const bool isolating,
                                 const bool ignore_own_bake,
                                 PaintMaterialCompositeImageLayer &out,
                                 const bUUID *stop_before = nullptr,
                                 const int *slice_rect = nullptr,
                                 const bool dimensions_only = false,
                                 const bool describe_only = false,
                                 const std::shared_ptr<const PaintMaterialCompositeSurface>
                                     &context_surface = nullptr,
                                 PaintLayersHeightWonCache *height_won_cache = nullptr);

/**
 * Append the rows of \a list that take part in \a channel to \a r_layers, bottom to top. A folder is
 * one entry carrying its children, which the evaluator composites in isolation; a folder nothing of
 * which takes part is left out, as it is on the generator's side.
 */
void composite_image_layers_collect(const Material &ma,
                                    const ListBaseT<MaterialPaintLayer> &list,
                                    int channel,
                                    float pass_through_scale,
                                    const std::shared_ptr<const PaintMaterialCompositeSurface>
                                        &context_surface,
                                    Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                    bool isolating = false,
                                    const int *slice_rect = nullptr,
                                    const bool dimensions_only = false,
                                    const bool describe_only = false,
                                    PaintLayersHeightWonCache *height_won_cache = nullptr);

/**
 * The Height record's own settings of \a layer in \a channel, as the evaluator reads them.
 *
 * The mode is #BKE_paint_layers_channel_height_blend_effective, so an Inherit row over Add,
 * Subtract or a colour blend is described as the step it really takes. The Compare mode is
 * described on every channel: it gates all of them, not just Height. The mode and the scale are
 * described on the Height channel alone, which is the one whose step is taken in delta space -- the
 * evaluator then needs no channel of its own to apply them, and every other channel carries the
 * ordinary blend (#height_blend zero).
 */
void composite_height_settings_describe(const MaterialPaintLayer &layer,
                                        int channel,
                                        PaintMaterialCompositeRowParams &out);

/**
 * Render the Compare mask of \a row's Height record: one where the record is greater than the
 * height the rows below it (the \a prefix of its own list) accumulated, zero where it lost. The
 * Height channel of the prefix is described and evaluated like any composite -- from the neutral
 * bottom at the top of the stack, or as the straightened isolated accumulation \a isolating names
 * (the very quantity the graph's divide node hands the row inside a folder) -- and the row's own
 * content comes through the single-row evaluator, corrections included; neither side carries the
 * row's factor -- the comparison is of the heights themselves, the factors weight the step and
 * the other channels afterwards.
 *
 * The mask is a gate, so it is rendered a strip of rows at a time into a one-channel buffer: a
 * canvas-sized RGBA accumulation (a quarter of a gigabyte at 4K, twice) would dwarf everything
 * else a composite does, and neither side needs a colour. Both sides are acquired once for the
 * whole mask, so the strips share one stack each and a mapped layer is resampled once.
 *
 * Null when the mask cannot be rendered (the row lays no Height, or the prefix and the row cannot
 * agree on a canvas size); the caller then lays the row by its own factors alone.
 *
 * \param cache: the pass's mask cache when there is one, so a size mismatch is said out loud once
 *               per pass rather than once per channel.
 */
std::shared_ptr<ImBuf> composite_height_won_render(
    const Material &ma,
    const ListBaseT<MaterialPaintLayer> &prefix,
    const MaterialPaintLayer &row,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    bool isolating,
    PaintLayersHeightWonCache *cache);

}  // namespace blender
