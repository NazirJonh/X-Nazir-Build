/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers_composite.hh: the description -> CPU layer stack bridge.
 */

#include "BKE_paint_layers_composite.hh"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/**
 * Restore the unit length of an encoded tangent-space normal the partial factors shortened.
 *
 * A byte preview of Normal is only meaningful once this runs: the shader normalizes the combined
 * normal, so an export that skipped it would disagree with the render.
 */
static void composite_normal_channel_normalize(ImBuf &ibuf, const rcti *region)
{
  if (ibuf.byte_buffer.data == nullptr) {
    return;
  }
  int xmin = 0, xmax = ibuf.x, ymin = 0, ymax = ibuf.y;
  if (region != nullptr) {
    xmin = max_ii(0, region->xmin);
    xmax = min_ii(ibuf.x, region->xmax);
    ymin = max_ii(0, region->ymin);
    ymax = min_ii(ibuf.y, region->ymax);
    if (xmin >= xmax || ymin >= ymax) {
      return;
    }
  }
  uchar *pixels = ibuf.byte_data_for_write();
  for (int y = ymin; y < ymax; y++) {
    for (int x = xmin; x < xmax; x++) {
      uchar *p = pixels + (int64_t(y) * ibuf.x + x) * 4;
      float normal[3] = {
          float(p[0]) / 127.5f - 1.0f, float(p[1]) / 127.5f - 1.0f, float(p[2]) / 127.5f - 1.0f};
      const float length = len_v3(normal);
      if (length <= 0.0f) {
        continue;
      }
      mul_v3_fl(normal, 1.0f / length);
      for (const int i : IndexRange(3)) {
        p[i] = uchar(clamp_i(int((normal[i] * 0.5f + 0.5f) * 255.0f + 0.5f), 0, 255));
      }
    }
  }
}

/**
 * The CPU blend a row takes in \a channel.
 *
 * The Normal channel is always combined rather than mixed -- interpolating encoded normals would
 * flatten the relief -- whatever the row's own blend is; every other channel uses the row's blend,
 * through the one table the generator reads too.
 */
static CompositeBlend layer_channel_blend(const MaterialPaintLayer &layer, const int channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return CompositeBlend::NormalCombine;
  }
  return CompositeBlend(BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(layer.blend)));
}

/**
 * Evaluate image_layers of channel into r_linear as straight scene-linear RGBA, starting
 * from the channel's neutral value, so a partially covered row fades towards what the material
 * shows where nothing covers it rather than towards zero.
 */
static bool composite_channel_linear(const Span<PaintMaterialCompositeImageLayer> image_layers,
                                     const int channel,
                                     const PaintMaterialCompositeLinearTarget &target,
                                     const rcti *region)
{
  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
  return BKE_paint_material_composite_eval_images_linear(
      image_layers, target, region, nullptr, bottom_color);
}

/**
 * Describe \a row's UV mapping to the evaluator for the map it reads in \a channel. A mapping that
 * does not apply is left disabled, so the evaluator reads the map as it is.
 */
static void composite_mapping_describe(const Material &ma,
                                       const MaterialPaintLayer &row,
                                       const int channel,
                                       PaintMaterialCompositeMapping &out)
{
  if (!BKE_paint_layers_mapping_applies(ma, row)) {
    return;
  }
  out.enabled = true;
  copy_v2_v2(out.offset, row.mapping.offset);
  copy_v2_v2(out.scale, row.mapping.scale);
  out.rotation = row.mapping.rotation;
  out.wrap = BKE_paint_layers_mapping_wrap_get(row);
  out.tangent_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
}

/** The parameters of \a row's kind in the schema order the evaluator reads them in. */
template<size_t N> static void composite_filter_params(const MaterialPaintLayer &row, float (&r_params)[N])
{
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(row)) {
    for (const int i : IndexRange(std::min(kind->params_num, int(N)))) {
      r_params[i] = paint_layer_kind_param_get(row, i);
    }
  }
}

/**
 * Describe the source of the row \a layer (not a folder) in \a channel to the evaluator: the
 * counterpart of the generator's #PaintLayersTreeBuilder::row_source_emit, which builds the same
 * thing as nodes. False when the row takes no part, which the generator leaves out as well.
 */
static bool composite_row_source_describe(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const int channel,
                                          const bool isolating,
                                          PaintMaterialCompositeImageLayer &out)
{
  if (paint_layer_row_is_adjustment(layer)) {
    /* The filter of the stack below. Inside an isolating folder the generator leaves it out, as
     * the accumulation there is partly covered and pre-multiplied. */
    if (isolating || !paint_layer_adjustment_acts(ma, layer, channel)) {
      return false;
    }
    out.adjustment_filter = layer.kind_subtype;
    composite_filter_params(layer, out.adjustment_params);
    return true;
  }
  if (!paint_layer_row_participates(ma, layer, channel)) {
    return false;
  }
  if (paint_layer_row_empty_base(ma, layer, channel)) {
    /* Only its corrections lay here: its own constant covers nothing, so the row starts from zero
     * coverage and the corrections bring it in. */
    out.has_constant_color = true;
    out.empty_base = true;
    return true;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (record->image != nullptr) {
    out.color_image = record->image;
    /* The description carries no per-node #ImageUser; the default one is what a plain map uses. */
    out.color_iuser = nullptr;
    /* A fresh paint map is transparent where nothing was painted; that texel must show the rows
     * below rather than cover them with the map's black. */
    out.color_alpha_coverage = true;
    composite_mapping_describe(ma, layer, channel, out.mapping);
    return true;
  }
  /* A channel with no map: a flat value (it has some alpha, or the row would not take part). */
  copy_v4_v4(out.constant_color, record->value);
  out.has_constant_color = true;
  return true;
}

/**
 * Describe \a layer's mask stack to the evaluator, bottom to top: the base first, then the
 * corrections over it in the order they were added. One that lays nothing yet is not carried, as
 * the generator builds nothing for it. The mask is one for all channels of the row, so the same
 * description reaches every channel's stack.
 *
 * The one place the description is read for the mask, so the CPU composite and the strokes that
 * sample the mask themselves (#PaintLayersMaskSampler) cannot drift apart.
 */
static void composite_mask_items_describe(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  if (BKE_paint_layers_mask_base(layer) == nullptr) {
    return;
  }
  for (const MaterialPaintLayer &mask : layer.mask_stack) {
    if (paint_layer_kind_is_map_coverage(mask)) {
      /* A map-driven coverage operator: it lays its map's `match` over the coverage below,
       * whether or not it has a map yet (a missing map is `match = 0`, not a pass-through). Its
       * Base Color record is structural, so the map is read from it directly. */
      PaintMaterialCompositeMaskItem item;
      item.opacity = BKE_paint_layers_effective_opacity(mask);
      item.map_coverage = mask.kind_subtype;
      item.invert = paint_layer_mask_item_inverted(mask);
      item.coverage_tolerance = paint_layer_kind_param_get(mask, PAINT_LAYER_COVERAGE_TOLERANCE);
      item.coverage_softness = paint_layer_kind_param_get(mask, PAINT_LAYER_COVERAGE_SOFTNESS);
      item.coverage_output = BKE_paint_layers_mask_item_output_value(mask);
      if (paint_layer_map_coverage_has_map(mask)) {
        item.coverage_image = paint_layer_map_coverage_record(mask)->image;
        composite_mapping_describe(
            ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, item.coverage_mapping);
      }
      for (const MaterialPaintLayerIdColor &color : mask.id_colors) {
        PaintMaterialCompositeIdColor out_color;
        copy_v3_v3(out_color.color, color.color);
        out_color.tolerance = color.tolerance;
        out_color.own_tolerance = paint_layer_mask_item_color_id_advanced(mask);
        out_color.enabled = (color.flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0;
        item.coverage_colors.append(out_color);
      }
      r_items.append(item);
      continue;
    }
    if (paint_layer_effect_is_filter(mask)) {
      /* A filter changes the coverage so far; it owns no map or grey and no record. */
      PaintMaterialCompositeMaskItem filter;
      filter.opacity = BKE_paint_layers_effective_opacity(mask);
      filter.filter = mask.kind_subtype;
      composite_filter_params(mask, filter.filter_params);
      r_items.append(filter);
      continue;
    }
    const MaterialPaintLayerChannel *mask_record = paint_layer_channel_find(
        mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (mask_record == nullptr || !paint_layer_mask_item_lays(mask)) {
      continue;
    }
    PaintMaterialCompositeMaskItem item;
    item.opacity = BKE_paint_layers_effective_opacity(mask);
    item.image = mask_record->image;
    item.iuser = nullptr;
    item.constant_gray = paint_layer_mask_constant(*mask_record);
    item.blend = CompositeBlend(
        BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
    item.invert = paint_layer_mask_item_inverted(mask);
    if (item.image != nullptr) {
      composite_mapping_describe(ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, item.mapping);
    }
    r_items.append(item);
  }
}

/**
 * Append the rows of \a list that take part in \a channel to \a r_layers, bottom to top. A folder is
 * one entry carrying its children, which the evaluator composites in isolation; a folder nothing of
 * which takes part is left out, as it is on the generator's side.
 */
static void composite_image_layers_collect(const Material &ma,
                                           const ListBaseT<MaterialPaintLayer> &list,
                                           const int channel,
                                           const float pass_through_scale,
                                           Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                           const bool isolating = false)
{
  for (const MaterialPaintLayer &layer : list) {
    /* A Pass Through folder is flattened into this list, the way the generator expands it in the
     * chain. It has no row to carry its visibility, so a hidden one hands its rows a zero factor
     * on their opacity, accumulated on the way down (the same rule as #paint_layers_form_map). */
    if (BKE_paint_layers_folder_is_pass_through(layer)) {
      const float scale = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? pass_through_scale : 0.0f;
      composite_image_layers_collect(ma, layer.children, channel, scale, r_layers, isolating);
      continue;
    }
    PaintMaterialCompositeImageLayer out;
    if (BKE_paint_layers_is_folder(layer)) {
      Vector<PaintMaterialCompositeImageLayer> children;
      composite_image_layers_collect(
          ma, layer.children, channel, pass_through_scale, children, true);
      /* An emptied folder is left out, as the generator leaves it; one with a correction that lays
       * here is not: the correction brings the coverage in over nothing. */
      if (children.is_empty() && !paint_layer_effects_lay(ma, layer, channel)) {
        continue;
      }
      out.is_folder = true;
      out.children.assign(std::make_move_iterator(children.begin()),
                          std::make_move_iterator(children.end()));
    }
    else if (!composite_row_source_describe(ma, layer, channel, isolating, out)) {
      continue;
    }
    out.blend = layer_channel_blend(layer, channel);
    out.opacity = BKE_paint_layers_channel_opacity_effective(layer, channel) * pass_through_scale;
    out.enabled = true;
    out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
        eMaterialPaintChannel(channel));
    /* The corrections lay over the row's colour, in the order they were added. A correction that
     * lays nothing in this channel is not carried, as the generator builds nothing for it. */
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (paint_layer_effect_filters(ma, effect, channel)) {
        PaintMaterialCompositeCorrection filter;
        filter.filter = effect.kind_subtype;
        composite_filter_params(effect, filter.filter_params);
        filter.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                         pass_through_scale;
        out.content_corrections.append(filter);
        continue;
      }
      if (!paint_layer_effect_lays(ma, effect, channel)) {
        continue;
      }
      PaintMaterialCompositeCorrection correction;
      const MaterialPaintLayerChannel *effect_record = paint_layer_channel_find(effect, channel);
      if (effect_record->image != nullptr) {
        correction.image = effect_record->image;
        composite_mapping_describe(ma, effect, channel, correction.mapping);
      }
      else {
        /* Only a Fill correction lays without a map: its flat colour. */
        copy_v4_v4(correction.constant_color, effect_record->value);
        correction.has_constant_color = true;
      }
      correction.iuser = nullptr;
      correction.blend = layer_channel_blend(effect, channel);
      correction.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                           pass_through_scale;
      out.content_corrections.append(correction);
    }
    /* The mask is one for all channels of the row, so the same item reaches every channel's
     * stack, exactly as the generator reads the one set of mask inputs in every channel chain. */
    composite_mask_items_describe(ma, layer, out.mask_items);
    r_layers.append(std::move(out));
  }
}

bool BKE_paint_layers_composite_image_layers(
    const Material &ma, const int channel, Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  r_layers.clear();
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  composite_image_layers_collect(ma, ma.paint_layers, channel, 1.0f, r_layers);
  return !r_layers.is_empty();
}

bool BKE_paint_layers_composite_channel(const Material &ma,
                                        const int channel,
                                        ImBuf &r_dst,
                                        const rcti *region)
{
  Vector<PaintMaterialCompositeImageLayer> image_layers;
  if (!BKE_paint_layers_composite_image_layers(ma, channel, image_layers)) {
    return false;
  }
  int width = 0;
  int height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, width, height) || width <= 0 ||
      height <= 0 || r_dst.byte_buffer.data == nullptr || r_dst.x != width || r_dst.y != height ||
      !ELEM(r_dst.channels, 0, 4))
  {
    return false;
  }

  rcti area;
  if (!BKE_paint_material_composite_area_get(width, height, region, area)) {
    /* Nothing of the tagged region is inside the buffer; the destination is already correct. */
    return true;
  }

  /* Scratch the size of the area, never of the frame: a small refresh costs the same on any map. */
  const int64_t area_width = BLI_rcti_size_x(&area);
  Vector<float> linear(area_width * BLI_rcti_size_y(&area) * 4);
  if (!composite_channel_linear(image_layers,
                                channel,
                                PaintMaterialCompositeLinearTarget::packed(linear, area),
                                region))
  {
    return false;
  }

  /* The byte result is written in the channel's own colorspace: sRGB for a colour channel, data
   * for a scalar or Normal one. A caller that already set one -- the cache, from the map it was
   * built on -- keeps it. Set only now, so a refused composite leaves the destination untouched. */
  if (r_dst.byte_buffer.colorspace == nullptr) {
    const bool is_color = BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).is_color;
    const int role = is_color ? COLOR_ROLE_DEFAULT_BYTE : COLOR_ROLE_DATA;
    IMB_colormanagement_assign_byte_colorspace(
        &r_dst, IMB_colormanagement_role_colorspace_name_get(role));
  }

  BKE_paint_material_composite_linear_to_byte(linear, area, r_dst);

  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    composite_normal_channel_normalize(r_dst, region);
  }
  return true;
}

bool BKE_paint_layers_composite_image(const Material &ma,
                                      const int channel,
                                      Image &dst,
                                      const rcti *region,
                                      ReportList *reports)
{
  Vector<PaintMaterialCompositeImageLayer> image_layers;
  if (!BKE_paint_layers_composite_image_layers(ma, channel, image_layers)) {
    BKE_reportf(reports, RPT_ERROR, "Channel %d is not a compositable layer stack", channel);
    return false;
  }
  int width = 0, height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, width, height) || width <= 0 ||
      height <= 0)
  {
    BKE_report(reports, RPT_ERROR, "The layer stack has no dimensions to composite into");
    return false;
  }

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&dst, &iuser, &lock);
  if (ibuf == nullptr) {
    BKE_report(reports, RPT_ERROR, "The destination image has no buffer");
    return false;
  }
  if (ibuf->x != width || ibuf->y != height) {
    BKE_image_release_ibuf(&dst, ibuf, lock);
    BKE_report(reports,
               RPT_ERROR,
               "The destination image does not have the dimensions of the layer stack");
    return false;
  }

  if (ibuf->float_buffer.data != nullptr && !ELEM(ibuf->channels, 0, 4)) {
    /* The write below is RGBA; a 1- or 3-channel float buffer would be overrun. */
    BKE_image_release_ibuf(&dst, ibuf, lock);
    BKE_report(reports, RPT_ERROR, "The destination image must be RGBA");
    return false;
  }

  bool ok = false;
  if (ibuf->float_buffer.data != nullptr) {
    /* A float destination takes the linear result directly, premultiplied: that is what a float
     * ImBuf holds, and what the shader's own output compares against. */
    rcti area;
    if (!BKE_paint_material_composite_area_get(width, height, region, area)) {
      /* Nothing of the tagged region is inside the image; it is already correct. */
      ok = true;
    }
    else {
      /* Straight into the destination, no second frame-sized buffer: the area is evaluated in
       * place and premultiplied there, and the rest of the image is never touched. */
      float *dst_pixels = ibuf->float_data_for_write();
      const MutableSpan<float> storage(dst_pixels, int64_t(width) * height * 4);
      ok = composite_channel_linear(
          image_layers,
          channel,
          PaintMaterialCompositeLinearTarget::in_frame(storage, width, area),
          region);
      if (ok) {
        for (int y = area.ymin; y < area.ymax; y++) {
          float *row = dst_pixels + (int64_t(y) * width + area.xmin) * 4;
          for (int x = 0; x < BLI_rcti_size_x(&area); x++) {
            float *p = row + int64_t(x) * 4;
            p[0] *= p[3];
            p[1] *= p[3];
            p[2] *= p[3];
          }
        }
      }
    }
  }
  else {
    ok = BKE_paint_layers_composite_channel(ma, channel, *ibuf, region);
  }
  if (ok) {
    /* The write bypasses the paint path, so nothing else tells the display buffer or the GPU copy
     * of \a dst that its pixels changed. */
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(&dst, ibuf);
    BKE_image_partial_update_mark_full_update(&dst);
  }
  BKE_image_release_ibuf(&dst, ibuf, lock);

  if (!ok) {
    BKE_report(reports, RPT_ERROR, "Failed to composite the channel");
  }
  return ok;
}

bool BKE_paint_layers_mask_items(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 Vector<PaintMaterialCompositeMaskItem> &r_items)
{
  r_items.clear();
  composite_mask_items_describe(ma, layer, r_items);
  return !r_items.is_empty();
}

PaintLayersMaskSampler::PaintLayersMaskSampler(const Material &ma,
                                               const MaterialPaintLayer &layer)
{
  composite_mask_items_describe(ma, layer, items_);
  /* The evaluator's own form of the description, so #BKE_paint_material_mask_factor_eval reads the
   * same opacity/blend/invert/filter/coverage fields the composite reads. */
  buffers_.resize(items_.size());
  for (const int i : items_.index_range()) {
    const PaintMaterialCompositeMaskItem &item = items_[i];
    PaintMaterialCompositeMaskBuffer &buffer = buffers_[i];
    buffer.constant_gray = item.constant_gray;
    buffer.opacity = item.opacity;
    buffer.blend = item.blend;
    buffer.invert = item.invert;
    buffer.filter = item.filter;
    std::copy(std::begin(item.filter_params),
              std::end(item.filter_params),
              std::begin(buffer.filter_params));
    buffer.map_coverage = item.map_coverage;
    buffer.coverage_tolerance = item.coverage_tolerance;
    buffer.coverage_softness = item.coverage_softness;
    buffer.coverage_output = item.coverage_output;
    buffer.coverage_colors = item.coverage_colors;
  }
}

PaintLayersMaskSampler::~PaintLayersMaskSampler()
{
  for (Tile &tile : tiles_) {
    for (ImBuf *ibuf : tile.items) {
      if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
    for (ImBuf *ibuf : tile.coverage) {
      if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
  }
}

bool PaintLayersMaskSampler::is_empty() const
{
  return items_.is_empty();
}

void PaintLayersMaskSampler::tile_acquire(const int tile_number,
                                          const int ref_width,
                                          const int ref_height)
{
  for (const int i : tiles_.index_range()) {
    if (tiles_[i].tile_number == tile_number) {
      return;
    }
  }

  Tile tile;
  tile.tile_number = tile_number;
  tile.width = ref_width;
  tile.height = ref_height;
  const int item_count = items_.size();
  tile.items.resize(item_count, nullptr);
  tile.coverage.resize(item_count, nullptr);
  tile.item_samples.resize(item_count, nullptr);
  tile.coverage_samples.resize(item_count, nullptr);
  for (const int i : items_.index_range()) {
    const PaintMaterialCompositeMaskItem &item = items_[i];
    if (item.image != nullptr) {
      ImBuf *ibuf = BKE_paint_material_mask_map_acquire(
          item.image, tile_number, &item.mapping, ref_width, ref_height);
      /* A buffer that does not match the tile the stroke paints on is not sampled: an item of
       * another size would be read out of bounds, and the composite refuses such a stack too. */
      if (ibuf != nullptr && ibuf->x == ref_width && ibuf->y == ref_height) {
        tile.items[i] = ibuf;
        tile.item_samples[i] = ibuf->float_data();
      }
      else if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
    if (item.coverage_image != nullptr) {
      ImBuf *ibuf = BKE_paint_material_mask_map_acquire(
          item.coverage_image, tile_number, &item.coverage_mapping, ref_width, ref_height);
      if (ibuf != nullptr && ibuf->x == ref_width && ibuf->y == ref_height) {
        tile.coverage[i] = ibuf;
        tile.coverage_samples[i] = ibuf->float_data();
      }
      else if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
  }

  tiles_.append(std::move(tile));
}

float PaintLayersMaskSampler::sample(const int tile_number, const int x, const int y) const
{
  if (items_.is_empty()) {
    return 1.0f;
  }
  const Tile *found = nullptr;
  for (const Tile &candidate : tiles_) {
    if (candidate.tile_number == tile_number) {
      found = &candidate;
      break;
    }
  }
  if (found == nullptr) {
    return 1.0f;
  }
  const Tile &tile = *found;
  if (x < 0 || y < 0 || x >= tile.width || y >= tile.height) {
    return 1.0f;
  }
  const int64_t pixel_index = int64_t(y) * tile.width + x;
  return BKE_paint_material_mask_factor_eval(
      buffers_.as_span(), tile.item_samples, tile.coverage_samples, pixel_index);
}

}  // namespace blender
