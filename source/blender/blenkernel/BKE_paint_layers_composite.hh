/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * \file
 * \ingroup bke
 *
 * The CPU half of a layered material's channel: the description (`Material::paint_layers`) is
 * flattened into the image-layer model the compositor evaluates, with no node graph involved.
 *
 * It exists so the pixels a stroke is judged against and the pixels the render produces come from
 * the same description by construction. The generator builds the GPU graph from that description;
 * this builds the CPU stack from it, and both read the rows through #BKE_paint_layers_flatten.
 */

#include "BKE_paint_material_composite.hh"

namespace blender {

struct Image;
struct ImBuf;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct ReportList;
struct rcti;

/**
 * The layer stack \a channel of \a ma's description resolves to, bottom to top.
 *
 * A row takes part when it carries the channel: a map becomes an image layer, a Fill colour or a
 * flat channel value becomes a constant layer. The GPU chain starts from a constant and blends
 * every row through a Mix, so the stack starts from transparency the same way and no row is a
 * bare base.
 *
 * A row's mask is carried as per-pixel coverage, the same factor the generated chain multiplies
 * in, and a constant mask is folded into the row's opacity. Corrections are carried along too: a
 * mask map with content and mask corrections is the spec-18 coverage model, and both sides compute
 * it the same way.
 *
 * On the Normal channel a content Paint correction rides the same Normal Combine the row does, and
 * a mask correction only scales the factor, so both are carried; a content Fill correction (a
 * constant normal) is unsupported and left out, exactly as the generator leaves it out. See
 * #BKE_paint_layers_issues_get, which reports that case.
 *
 * \return false when the channel has no participating row at all.
 */
bool BKE_paint_layers_composite_image_layers(
    const Material &ma, int channel, Vector<PaintMaterialCompositeImageLayer> &r_layers);

/**
 * Composite \a channel of \a ma into \a r_dst, which must be byte RGBA of the stack's dimensions.
 *
 * \param region: when given, only this rectangle is recomputed, exactly as
 *                #BKE_paint_material_composite_eval_images takes it.
 * \return false when the channel is not compositable from the description (see
 *         #BKE_paint_layers_composite_image_layers).
 */
bool BKE_paint_layers_composite_channel(const Material &ma,
                                        int channel,
                                        ImBuf &r_dst,
                                        const rcti *region = nullptr);

/**
 * Composite \a channel of \a ma into \a dst, for a caller that holds an #Image rather than a bare
 * buffer -- the description's public export path (`Material.paint_layers.composite`), map export
 * and tests all share this one call.
 *
 * A float destination is written as straight scene-linear, premultiplied like every float ImBuf, so
 * it compares one to one with what the shader produced; a byte destination goes through the byte
 * wrapper and is encoded into the image's colorspace (or the channel's own when it names none).
 * The image must already have a buffer at least as large as the stack.
 *
 * \param region: when given, only this rectangle is written.
 * \return false, with a report, when the channel is not a compositable stack or the destination
 *         has no usable buffer.
 */
bool BKE_paint_layers_composite_image(Material &ma,
                                      int channel,
                                      Image &dst,
                                      const rcti *region = nullptr,
                                      ReportList *reports = nullptr);

/**
 * Composite everything *below* \a layer in \a channel into a new float #Image in \a bmain.
 *
 * This is what a Custom group's `BELOW:<CHANNEL>` input is fed during its GPU bake: the row sees the
 * stack as it stands under it, not the finished channel. The image holds straight scene-linear RGB
 * with the below coverage in alpha; \a fallback_size is used when there is nothing below to take the
 * dimensions from. An empty below still produces an image holding the channel's bottom constant,
 * fully covering, so a caller always gets something to sample.
 *
 * \return null when the channel is out of range, \a fallback_size is not positive or the composite
 *         could not be evaluated.
 */
Image *BKE_paint_layers_below_image(Main &bmain,
                                    const Material &ma,
                                    const MaterialPaintLayer &layer,
                                    int channel,
                                    int fallback_size);

}  // namespace blender
