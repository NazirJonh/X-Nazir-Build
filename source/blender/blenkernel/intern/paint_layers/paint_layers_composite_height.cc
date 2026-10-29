/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The Height half of the description -> image-layer bridge: what a row's Height record asks of the
 * evaluator (#composite_height_settings_describe) and the Compare mask that record gates every
 * other channel of the row with (#composite_height_won_render).
 *
 * It lives apart from the rest of the bridge because the mask is the one description-time render
 * the CPU composite does, with its own scratch and its own evaluation strategy; a change to the
 * Height model should not have to touch the row builder, and the other way round.
 */

#include "paint_layers_composite_intern.hh"

#include <algorithm>
#include <utility>

#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

#include "CLG_log.h"

#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/**
 * How many rows of a Compare mask are rendered at a time. The mask compares two scalar
 * accumulations, so the strip is the scratch both of them are read from: a few megabytes at 4K
 * instead of the two canvas-sized RGBA buffers a whole-frame render would need. The two stacks
 * behind the strips are acquired once, so the strip size is a memory question only.
 */
constexpr int COMPOSITE_HEIGHT_WON_STRIP_ROWS = 64;

static CLG_LogRef LOG = {"bke.paint_layers"};

void composite_height_settings_describe(const MaterialPaintLayer &layer,
                                        const int channel,
                                        PaintMaterialCompositeRowParams &out)
{
  const int height_blend = BKE_paint_layers_channel_height_blend_effective(
      layer, PAINT_MATERIAL_CHANNEL_HEIGHT);
  out.height_compare = height_blend == MA_PAINT_LAYER_HEIGHT_BLEND_COMPARE;
  if (channel != PAINT_MATERIAL_CHANNEL_HEIGHT) {
    return;
  }
  out.height_blend = height_blend;
  out.height_scale = BKE_paint_layers_channel_height_scale(layer, channel);
}

/**
 * Say out loud that \a row's Compare mask cannot be rendered because the prefix and the row do not
 * agree on a canvas size. Once per row and pass: the mask walk is repeated for every other channel
 * of the row, and the diagnostic is about the stack, not about the channel that happened to get
 * there first.
 */
static void composite_height_won_report_mismatch(PaintLayersHeightWonCache *cache,
                                                 const MaterialPaintLayer &row,
                                                 const bool isolating,
                                                 const int prefix_width,
                                                 const int prefix_height,
                                                 const int width,
                                                 const int height)
{
  if (cache != nullptr &&
      !cache->size_mismatch_reported.add(std::pair<const MaterialPaintLayer *, bool>(&row,
                                                                                    isolating)))
  {
    return;
  }
  CLOG_WARN(&LOG,
            "Row '%s': channel Height has no Compare mask, prefix %dx%d, row %dx%d",
            row.name,
            prefix_width,
            prefix_height,
            width,
            height);
}

std::shared_ptr<ImBuf> composite_height_won_render(
    const Material &ma,
    const ListBaseT<MaterialPaintLayer> &prefix,
    const MaterialPaintLayer &row,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    const bool isolating,
    PaintLayersHeightWonCache *cache)
{
  /* The row's own Height record first: it names the canvas (its map's size) and refuses the mask
   * for a row that lays no Height at all. Described once: its stack names the canvas, and the
   * single-row evaluator reads the very same layer for the content. */
  PaintMaterialCompositeImageLayer content;
  if (!composite_image_layer_build(ma,
                                   row,
                                   PAINT_MATERIAL_CHANNEL_HEIGHT,
                                   1.0f,
                                   false,
                                   false,
                                   content,
                                   nullptr,
                                   nullptr,
                                   false,
                                   false,
                                   context_surface))
  {
    return nullptr;
  }
  int width = 0;
  int height = 0;
  {
    Span<PaintMaterialCompositeImageLayer> row_stack(&content, 1);
    if (!BKE_paint_material_composite_stack_dimensions(row_stack, width, height)) {
      return nullptr;
    }
  }

  /* The Height channel of the rows below. At the top of the stack it is evaluated like any
   * composite from the neutral bottom; inside an isolating folder the row reads the folder's own
   * straightened accumulation (the divide node of the generated chain), which is what a synthetic
   * folder holding the prefix evaluates to. */
  Vector<PaintMaterialCompositeImageLayer> prefix_layers;
  const bool has_prefix = prefix.first != nullptr || prefix.last != nullptr;
  if (has_prefix) {
    composite_image_layers_collect(ma,
                                   prefix,
                                   PAINT_MATERIAL_CHANNEL_HEIGHT,
                                   1.0f,
                                   context_surface,
                                   prefix_layers,
                                   false,
                                   nullptr,
                                   false,
                                   false);
    int prefix_width = 0;
    int prefix_height = 0;
    const bool sized = !prefix_layers.is_empty() &&
                       BKE_paint_material_composite_stack_dimensions(
                           prefix_layers, prefix_width, prefix_height);
    /* Maps of different sizes would compare texels that are not the same place. A stack shares
     * its size in practice (the material creates them that way); a mismatch keeps the row's own
     * factors rather than comparing through a resample the graph has no node for. Said out loud
     * once per row and pass, since the row then paints a shade differently from what the graph's
     * own comparison would have given and every other channel of the row pays this walk again. */
    if (!sized || prefix_width != width || prefix_height != height) {
      composite_height_won_report_mismatch(
          cache, row, isolating, prefix_width, prefix_height, width, height);
      return nullptr;
    }
  }
  /* The folder the prefix evaluates as, built once outside the strip loop: its children are moved
   * into it, so it cannot be made again per strip. */
  PaintMaterialCompositeImageLayer folder;
  if (has_prefix && isolating) {
    folder.is_folder = true;
    folder.enabled = true;
    folder.children.assign(std::make_move_iterator(prefix_layers.begin()),
                           std::make_move_iterator(prefix_layers.end()));
  }
  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(PAINT_MATERIAL_CHANNEL_HEIGHT),
                                        bottom_color);

  const float scale = BKE_paint_layers_channel_height_scale(row, PAINT_MATERIAL_CHANNEL_HEIGHT);

  /* Both sides of the comparison are acquired once for the whole mask, never once per strip: a
   * mapped layer's resample is a canvas-sized pass of its own, and a 4K mask has 64 strips. A
   * session holds the locks until it goes out of scope, so the strips below only evaluate. */
  PaintMaterialCompositeLinearSession below_session;
  if (has_prefix) {
    below_session = isolating ?
                        PaintMaterialCompositeLinearSession::session_open_row(folder) :
                        PaintMaterialCompositeLinearSession::session_open(prefix_layers,
                                                                         bottom_color);
    if (!below_session.ok()) {
      return nullptr;
    }
  }
  PaintMaterialCompositeLinearSession content_session =
      PaintMaterialCompositeLinearSession::session_open_row(content);
  if (!content_session.ok()) {
    return nullptr;
  }

  ImBuf *won = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::FloatData);
  if (won == nullptr) {
    return nullptr;
  }
  /* A gate is one number per texel: the same 0/1 in a quarter of an RGBA buffer. */
  won->channels = 1;
  if (!IMB_alloc_float_pixels(won, 1, false)) {
    IMB_freeImBuf(won);
    return nullptr;
  }
  float *out = won->float_data_for_write();

  /* A strip of rows at a time: the two accumulations are scratch of the strip's size, never of
   * the canvas, and each one is read for a single texel. */
  Vector<float> below;
  Vector<float> folder_coverage;
  Vector<float> row_color;
  Vector<float> row_coverage;
  for (int y0 = 0; y0 < height; y0 += COMPOSITE_HEIGHT_WON_STRIP_ROWS) {
    const int y1 = min_ii(y0 + COMPOSITE_HEIGHT_WON_STRIP_ROWS, height);
    const int strip_rows = y1 - y0;
    const int64_t count = int64_t(width) * strip_rows;
    rcti strip;
    BLI_rcti_init(&strip, 0, width, y0, y1);
    below.resize(count * 4);
    if (has_prefix) {
      bool ok = false;
      if (isolating) {
        folder_coverage.resize(count);
        ok = below_session.eval_row_content(
            &strip, PAINT_MATERIAL_CHANNEL_HEIGHT, below.data(), folder_coverage.data());
      }
      else {
        ok = below_session.eval(PaintMaterialCompositeLinearTarget::packed(below, strip), &strip);
      }
      if (!ok) {
        IMB_freeImBuf(won);
        return nullptr;
      }
    }
    else if (!isolating) {
      /* Nothing below at the top of the stack: the accumulation is the neutral bottom everywhere. */
      for (int64_t i = 0; i < count; i++) {
        below[i * 4 + 0] = below[i * 4 + 1] = below[i * 4 + 2] = PAINT_HEIGHT_NEUTRAL;
        below[i * 4 + 3] = 1.0f;
      }
    }
    else {
      /* The first rows of an isolating folder: nothing is below them yet, so the accumulation is
       * the empty one. That is the graph's own answer rather than a guess -- the folder's divide
       * node is `P / a` with both zero there, and a Vector Math Divide is a safe divide (zero for
       * a zero divisor) -- so the row compares against `-0.5` on both sides. */
      std::fill(below.begin(), below.end(), 0.0f);
    }

    /* The row's straight Height content, corrections included, unfactored like the graph's
     * source. */
    row_color.resize(count * 4);
    row_coverage.resize(count);
    if (!content_session.eval_row_content(
            &strip, PAINT_MATERIAL_CHANNEL_HEIGHT, row_color.data(), row_coverage.data()))
    {
      IMB_freeImBuf(won);
      return nullptr;
    }

    for (int ty = 0; ty < strip_rows; ty++) {
      for (int x = 0; x < width; x++) {
        const int64_t i = int64_t(ty) * width + x;
        const float d_below = below[i * 4] - PAINT_HEIGHT_NEUTRAL;
        const float d_layer = (row_color[i * 4] - PAINT_HEIGHT_NEUTRAL) * scale;
        out[int64_t(y0 + ty) * width + x] = height_compare_won(d_below, d_layer) ? 1.0f : 0.0f;
      }
    }
  }
  return std::shared_ptr<ImBuf>(won, IMB_freeImBuf);
}

}  // namespace blender
