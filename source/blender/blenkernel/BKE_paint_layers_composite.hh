/* SPDX-FileCopyrightText: 2026 Nazir Galimov
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
 * the same description by construction: both read the rows of the description, the one building a
 * GPU graph and this one a CPU stack.
 */

#include "BKE_paint_material_composite.hh"

namespace blender {

struct Image;
struct ImBuf;
struct Material;
struct MaterialPaintLayer;
struct ReportList;
struct rcti;

/**
 * The layer stack \a channel of \a ma's description resolves to, bottom to top.
 *
 * A row takes part when it carries a live record for the channel: its map becomes an image layer, a
 * flat channel value with some coverage becomes a constant layer. A record without a map and
 * without coverage lays nothing down and is left out. The stack starts from the channel's bottom
 * colour (#BKE_paint_layers_channel_bottom_color) and every row is blended over it with its
 * opacity.
 *
 * Alpha is not one thing. A map's own alpha is its coverage: a texel the painter left transparent
 * shows what is below. A flat value's alpha is never coverage: the row covers by its opacity (and
 * mask) alone, and the value's alpha only does two things -- it decides whether the row takes part
 * (an alpha of 0 lays nothing down) and it travels, as the row's content alpha, in the result's
 * alpha component, mixed in exactly like its colour:
 * `a = a_below * (1 - f) + a_row * f` with `f = opacity * mask`.
 * The result's alpha is therefore "how much of the rows' own content is present", not a coverage
 * of the stack: consumers decide what to do with it (the float export premultiplies by it, a byte
 * export writes it as is). The generated graph's factor follows the same rule.
 *
 * \return false when the channel has no participating row at all.
 */
bool BKE_paint_layers_composite_image_layers(
    const Material &ma, int channel, Vector<PaintMaterialCompositeImageLayer> &r_layers);

/**
 * Composite \a channel of \a ma into \a r_dst, which must be byte RGBA of the stack's dimensions.
 *
 * The result is encoded in \a r_dst's own colorspace; a buffer that names none gets the channel's
 * own (sRGB for a colour channel, data for a scalar or Normal one).
 *
 * \param region: when given, only this rectangle is recomputed, exactly as
 *                #BKE_paint_material_composite_eval_linear takes it.
 * \return false when the channel is not compositable from the description (see
 *         #BKE_paint_layers_composite_image_layers).
 */
bool BKE_paint_layers_composite_channel(const Material &ma,
                                        int channel,
                                        ImBuf &r_dst,
                                        const rcti *region = nullptr);

/**
 * Composite \a channel of \a ma into \a dst, for a caller that holds an #Image rather than a bare
 * buffer: the one call an export of a channel's result goes through.
 *
 * A float destination is written as straight scene-linear, premultiplied like every float ImBuf, so
 * it compares one to one with what the shader produced; a byte destination goes through
 * #BKE_paint_layers_composite_channel. The image must already have a buffer of the stack's
 * dimensions; a float buffer must be RGBA.
 *
 * On success \a dst is marked dirty and its display and partial-update state stale, so the caller
 * does not have to.
 *
 * \param region: when given, only this rectangle is written.
 * \return false, with a report, when the channel is not a compositable stack or the destination
 *         has no usable buffer.
 */
bool BKE_paint_layers_composite_image(const Material &ma,
                                      int channel,
                                      Image &dst,
                                      const rcti *region = nullptr,
                                      ReportList *reports = nullptr);

/**
 * The row's mask items as the evaluator reads them, bottom to top: the same description
 * #BKE_paint_layers_composite_image_layers carries per layer, exposed for a caller that samples the
 * mask itself (the strokes, see #PaintLayersMaskSampler).
 *
 * \return false when the row has no mask.
 */
bool BKE_paint_layers_mask_items(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 Vector<PaintMaterialCompositeMaskItem> &r_items,
                                 const bUUID *stop_before = nullptr);

/**
 * Describe the slice \a reader lays in \a channel, without rendering it: the same entry
 * #BKE_paint_layers_slice_render evaluates. A mask target describes the source row's mask stack up
 * to the point, a content target the row's content up to it. The row's own opacity, mask and
 * visibility are cleared: a slice is read straight. \a slice_rect limits nested slices to the
 * rectangle a partial render needs.
 *
 * \param dimensions_only: describe only what the slice is made of, so a caller finding the stack
 * dimensions does not render (or allocate) a single nested slice. The entry must not be evaluated.
 *
 * \return false when the reference does not resolve or the row cannot be described.
 */
bool BKE_paint_layers_slice_describe(const Material &ma,
                                     const MaterialPaintLayer &reader,
                                     int channel,
                                     PaintMaterialCompositeImageLayer &r_entry,
                                     const int *slice_rect = nullptr,
                                     bool dimensions_only = false);

/**
 * A CPU sampler of one row's effective mask, for the paint strokes.
 *
 * The row's mask stack is described once through the same reader the composite uses
 * (#BKE_paint_layers_mask_items), and its maps are acquired per UDIM tile through the same decode
 * and mapping the composite acquires them with. #sample returns the scalar factor `M` the composite
 * lays the row's coverage under (#BKE_paint_material_mask_factor_eval), so a stroke cannot hide what
 * the render shows: one reading of the mask rules for the canvas and for the render.
 *
 * A row with no mask items is empty and samples as one. A map that cannot be read is treated as an
 * absent one, as in the composite: a laid item falls back to its constant, a map-driven operator to
 * `match = 0`.
 */
class PaintLayersMaskSampler {
 public:
  PaintLayersMaskSampler(const Material &ma,
                         const MaterialPaintLayer &layer,
                         const bUUID *stop_before = nullptr);
  ~PaintLayersMaskSampler();

  PaintLayersMaskSampler(const PaintLayersMaskSampler &) = delete;
  PaintLayersMaskSampler &operator=(const PaintLayersMaskSampler &) = delete;

  /** Whether the row has any mask item to apply. */
  bool is_empty() const;

  /**
   * Acquire the maps of UDIM \a tile_number, the painted map's tile being \a ref_width x
   * \a ref_height. Repeating an already acquired tile is free. Not thread safe: acquire every tile
   * on one thread before the threads that call #sample start.
   */
  void tile_acquire(int tile_number, int ref_width, int ref_height);

  /**
   * The mask factor at texel (\a x, \a y) of UDIM \a tile_number; 1 for a tile that was not
   * acquired. Read only, so any number of threads may call it once the tiles are acquired.
   */
  float sample(int tile_number, int x, int y) const;

 private:
  struct Tile {
    int tile_number = -1;
    int width = 0;
    int height = 0;
    /** Owned decoded buffers per item (mask map / coverage map), or null where the item has none. */
    Vector<ImBuf *> items;
    Vector<ImBuf *> coverage;
    /** Pointers into the buffers above, for the evaluator; null where a buffer is absent. */
    Vector<const float *> item_samples;
    Vector<const float *> coverage_samples;
  };

  Vector<PaintMaterialCompositeMaskItem> items_;
  Vector<PaintMaterialCompositeMaskBuffer> buffers_;
  Vector<Tile> tiles_;
};

}  // namespace blender
