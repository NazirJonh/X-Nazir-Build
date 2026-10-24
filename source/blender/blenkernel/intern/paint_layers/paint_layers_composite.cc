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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
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
 * The Normal channel is combined rather than mixed -- interpolating encoded normals would flatten
 * the relief -- unless its channel is set to Replace, which lets the upper normal cover the lower
 * one with a plain Mix, as the generator builds it. Every other channel uses its own blend, or the
 * row's while it inherits, through the one table the generator reads too.
 */
static CompositeBlend layer_channel_blend(const MaterialPaintLayer &layer, const int channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return BKE_paint_layers_normal_replace(layer) ? CompositeBlend::Mix :
                                                    CompositeBlend::NormalCombine;
  }
  return CompositeBlend(BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
      BKE_paint_layers_channel_blend_effective(layer, channel))));
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
 * Describe \a row's Gradient payload in \a channel: the line, the ramp, the channel's scalar ends
 * and, for an Object/Generated line, the surface that gives the texel its coordinate. The one
 * place a Gradient row, a Gradient correction and a Gradient mask item are described, so the
 * evaluator cannot read them differently.
 */
static bool composite_gradient_describe(
    const Material &ma,
    const MaterialPaintLayer &row,
    const int channel,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    PaintMaterialCompositeGradient &out)
{
  if (row.gradient == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  out.enabled = true;
  out.color_channel = info.is_color;
  out.payload = dna::shallow_copy(*row.gradient);
  const int size = (ma.paint_layers_image_size > 0) ? int(ma.paint_layers_image_size) : 4096;
  out.width = size;
  out.height = size;
  if (row.gradient->space == MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
    /* A UV line already reads the texel's own UV: no surface and no approximation. */
    out.surface = nullptr;
    out.surface_key = 0;
    out.approximate = false;
  }
  else {
    /* The 2D preview needs the texel's object-space position. Without one the evaluator falls
     * back to `(u, v, 0)`, the approximate view. Whether to hand out a surface while the active
     * row is being dragged is the caller's call (#ED_space_image_gradient_preview_sync): it passes
     * none then, so this stays the single place that reads "no surface". The row holds a reference
     * to the surface, so a cache replacement cannot pull it out from under the evaluation. A
     * Generated row has a coordinate with stored CD_ORCO samples or a usable bbox; with neither it
     * falls back to the approximate preview rather than an empty layer. */
    const bool coordinate_usable = row.gradient->space !=
                                       MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED ||
                                   (context_surface &&
                                    (context_surface->generated_bbox_valid ||
                                     !context_surface->generated_positions.is_empty()));
    out.surface = context_surface;
    out.surface_key = context_surface ? context_surface->key : 0;
    out.approximate = !context_surface || !coordinate_usable;
  }
  if (!info.is_color) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(row, channel);
    if (record == nullptr) {
      return false;
    }
    out.scalar_start = record->value[0];
    out.scalar_end = record->value[1];
    if (record->gradient_ramp != nullptr) {
      out.scalar_ramp = *record->gradient_ramp;
      out.scalar_ramp_valid = true;
    }
  }
  return true;
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
                                          const std::shared_ptr<const PaintMaterialCompositeSurface>
                                              &context_surface,
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
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES)) {
    /* A Gradient: no map and no constant, only its line, colour ramp and the channel's own value
     * ramp. */
    return composite_gradient_describe(ma, layer, channel, context_surface, out.gradient);
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* The same decision the generator makes (#PaintLayersTreeBuilder::material_source_emit): the
     * source's plain value, its plain texture or the map rendered from it for the colour, and its
     * transparency, which is not the colour's alpha and so does not cover through it, as the
     * coverage. A plain-value coverage is folded into the opacity by the caller. */
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, layer, channel, PaintLayerMaterialUse::Composite);
    const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
        ma, layer, PaintLayerMaterialUse::Composite);
    if (source.kind == PaintLayerMaterialSourceKind::None ||
        coverage.kind == PaintLayerMaterialCoverageKind::Pending)
    {
      return false;
    }
    /* The source is read at the row's UV, the same transform the source wrapper and its coverage
     * chain apply in the generated graph. The mapping is described for the colour map and reused
     * for the source's separate coverage map; a constant colour still needs it when only the
     * coverage is a map. */
    composite_mapping_describe(ma, layer, channel, out.mapping);
    /* A clipping wrap leaves the map transparent outside the unit square, but a Material row
     * covers by its source's transparency rather than by the map's alpha, so the clip has to
     * reach the coverage instead. The acquisition folds it in; a Fill or painted row keeps its
     * usual clipping through the map's own alpha. */
    out.mapping_clips_coverage = out.mapping.enabled &&
                                 BKE_paint_layers_mapping_wrap_get(layer) !=
                                     MA_PAINT_LAYER_MAPPING_WRAP_REPEAT;
    if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
      copy_v4_v4(out.constant_color, source.constant);
      out.has_constant_color = true;
    }
    else {
      out.color_image = source.image;
      out.color_iuser = (source.kind == PaintLayerMaterialSourceKind::LiveImage) ? source.iuser :
                                                                                   nullptr;
    }
    out.color_alpha_coverage = false;
    if (coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha ||
        coverage.kind == PaintLayerMaterialCoverageKind::Baked)
    {
      out.mask_image = coverage.image;
      out.mask_iuser = (coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha) ?
                           coverage.iuser :
                           nullptr;
      out.mask_from_alpha = coverage.kind == PaintLayerMaterialCoverageKind::LiveAlpha;
      out.mask_influence = 1.0f;
    }
    return true;
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
    out.normal_directx = channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
                         (record->flag & MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) != 0;
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
static void composite_mask_items_describe(
    const Material &ma,
    const MaterialPaintLayer &layer,
    Vector<PaintMaterialCompositeMaskItem> &r_items,
    const bUUID *stop_before = nullptr,
    const int *slice_rect = nullptr,
    const bool dimensions_only = false,
    const bool describe_only = false,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface = nullptr)
{
  if (BKE_paint_layers_mask_base(layer) == nullptr) {
    return;
  }
  for (const MaterialPaintLayer &mask : layer.mask_stack) {
    if (stop_before != nullptr && BLI_uuid_equal(mask.marker, *stop_before)) {
      break;
    }
    if (paint_layer_kind_reads_anchor_point(mask)) {
      /* The slice a mask item reads: a mask point gives its scalar, a content point its coverage.
       * Both are grey buffers in canvas space, read like a painted map without a mapping. */
      if (!paint_layer_mask_item_lays(ma, mask)) {
        continue;
      }
      PaintMaterialCompositeMaskItem item;
      item.opacity = BKE_paint_layers_effective_opacity(mask);
      item.anchor_point_reader = mask.marker;
      const PaintLayerAnchorPointRef mask_ref = paint_layer_anchor_point_resolve(ma, mask);
      item.anchor_point_target = int8_t(mask_ref.target);
      item.anchor_point_point = (mask_ref.point != nullptr) ? mask_ref.point->marker : bUUID{};
      const int read_channel = paint_layer_anchor_point_mask_read_channel(ma, mask);
      item.anchor_point_read_channel = read_channel;
      const bool mask_target = item.anchor_point_target ==
                               int8_t(PaintLayerAnchorPointTarget::Mask);
      const bool baked = paint_layer_anchor_point_reader_baked(ma, mask) &&
                         mask.bake != nullptr &&
                         (mask_target ? mask.bake->images[read_channel] != nullptr :
                                        mask.bake->coverage != nullptr);
      if (baked) {
        /* A mask point's scalar is the grey the bake holds in its colour map; a content point's
         * coverage is the coverage map, which already holds the reader's opacity, so only its
         * visibility stays live (as for a substituted row). Read like a painted grey map. */
        if (mask_target) {
          item.image = mask.bake->images[read_channel];
        }
        else {
          item.image = mask.bake->coverage;
          item.opacity = (mask.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f;
        }
        item.iuser = nullptr;
        item.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, mask, read_channel);
        BKE_paint_layers_anchor_point_slice_images(ma, mask, item.anchor_point_images);
      }
      else {
        /* In describe-only mode the reader keeps its descriptor, hash and subscriptions; the
         * slice buffers belong to the GPU slot pass instead of the CPU cache. */
        if (!describe_only) {
          std::shared_ptr<ImBuf> slice_color;
          std::shared_ptr<ImBuf> slice_coverage;
          if (!BKE_paint_layers_slice_render(
                  ma, mask, read_channel, &slice_color, &slice_coverage, slice_rect, dimensions_only))
          {
            continue;
          }
          const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, mask);
          item.anchor_point_slice = (ref.target == PaintLayerAnchorPointTarget::Mask) ?
                                        std::move(slice_color) :
                                        std::move(slice_coverage);
        }
        item.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, mask, read_channel);
        BKE_paint_layers_anchor_point_slice_images(ma, mask, item.anchor_point_images);
      }
      item.blend = CompositeBlend(
          BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
      item.invert = paint_layer_mask_item_inverted(mask);
      r_items.append(item);
      continue;
    }
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
    if (paint_layer_kind_has_flag(mask, PAINT_LAYER_KIND_GENERATES)) {
      /* A Gradient mask item: the ramp's colour is the grey and its alpha the coverage, read from
       * the item's own line rather than from a map. */
      PaintMaterialCompositeMaskItem item;
      item.opacity = BKE_paint_layers_effective_opacity(mask);
      if (!composite_gradient_describe(
              ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, context_surface, item.gradient))
      {
        continue;
      }
      item.blend = CompositeBlend(
          BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
      item.invert = paint_layer_mask_item_inverted(mask);
      r_items.append(item);
      continue;
    }
    if (paint_layer_kind_has_flag(mask, PAINT_LAYER_KIND_READS_SOURCE)) {
      /* The grey of a mask correction that reads a material is the mean of the source's Base
       * Color, read as a painted item's map is: a plain value, a plain texture or the map rendered
       * from it. A graph with no map yet is the generator's to show alone, so nothing is carried
       * until the render lands. A plain transparency folds into the opacity. */
      if (!paint_layer_mask_item_lays(ma, mask)) {
        continue;
      }
      const int source_channel = paint_layer_mask_source_channel(mask);
      const PaintLayerMaterialSource source = paint_layer_material_channel_source(
          ma, mask, source_channel, PaintLayerMaterialUse::Composite);
      const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
          ma, mask, PaintLayerMaterialUse::Composite);
      if (source.kind == PaintLayerMaterialSourceKind::None ||
          coverage.kind == PaintLayerMaterialCoverageKind::Pending)
      {
        continue;
      }
      PaintMaterialCompositeMaskItem item;
      item.opacity = BKE_paint_layers_effective_opacity(mask);
      if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
        item.constant_gray = (source.constant[0] + source.constant[1] + source.constant[2]) / 3.0f;
      }
      else {
        item.image = source.image;
        item.iuser = (source.kind == PaintLayerMaterialSourceKind::LiveImage) ? source.iuser :
                                                                                nullptr;
        /* Read at the item's UV like a painted item's map is, so the Mapping and its gizmo move
         * the mask in the Image Editor's composite too. */
        composite_mapping_describe(ma, mask, source_channel, item.mapping);
      }
      if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
        item.opacity *= clamp_f(coverage.constant, 0.0f, 1.0f);
      }
      item.blend = CompositeBlend(
          BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
      item.invert = paint_layer_mask_item_inverted(mask);
      r_items.append(item);
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

static void composite_image_layers_collect(const Material &ma,
                                           const ListBaseT<MaterialPaintLayer> &list,
                                           int channel,
                                           float pass_through_scale,
                                           const std::shared_ptr<const PaintMaterialCompositeSurface>
                                               &context_surface,
                                           Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                           bool isolating = false,
                                           const int *slice_rect = nullptr,
                                           const bool dimensions_only = false,
                                           const bool describe_only = false);

/**
 * Append the content correction \a effect to \a out as the evaluator reads it in \a channel: a filter,
 * or a map, flat colour, source or slice laid by the correction's own blend and opacity. Nothing is
 * appended for a correction that lays nothing here, as the generator builds nothing for it.
 */
static void composite_effect_append(
    const Material &ma,
    const MaterialPaintLayer &effect,
    const int channel,
    const float pass_through_scale,
    const int *slice_rect,
    const bool dimensions_only,
    const bool describe_only,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    PaintMaterialCompositeImageLayer &out)
{
  if (paint_layer_effect_filters(ma, effect, channel)) {
    PaintMaterialCompositeCorrection filter;
    filter.filter = effect.kind_subtype;
    composite_filter_params(effect, filter.filter_params);
    filter.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                     pass_through_scale;
    out.content_corrections.append(filter);
    return;
  }
  if (!paint_layer_effect_lays(ma, effect, channel)) {
    return;
  }
  PaintMaterialCompositeCorrection correction;
  correction.iuser = nullptr;
  correction.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                       pass_through_scale;
  if (paint_layer_kind_reads_anchor_point(effect)) {
    correction.anchor_point_reader = effect.marker;
    const PaintLayerAnchorPointRef effect_ref = paint_layer_anchor_point_resolve(ma, effect);
    correction.anchor_point_target = int8_t(effect_ref.target);
    correction.anchor_point_point = (effect_ref.point != nullptr) ? effect_ref.point->marker :
                                                                      bUUID{};
    /* What the slice shows, laid by the reader's own blend and opacity. A baked reader reads the
     * maps its slice was rendered into. Only the row whose bake is being rendered ignores its own
     * bake; a correction nested in it describes its own bake, which is valid and cheaper than
     * rendering its slice live again. */
    const bool baked = paint_layer_anchor_point_reader_baked(ma, effect) &&
                       effect.bake != nullptr && effect.bake->images[channel] != nullptr &&
                       effect.bake->coverage != nullptr;
    if (baked) {
      correction.image = effect.bake->images[channel];
      correction.iuser = nullptr;
      correction.anchor_point_coverage_image = effect.bake->coverage;
      correction.anchor_point_coverage_iuser = nullptr;
      correction.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, effect, channel);
      /* The coverage map already holds the reader's opacity; only its visibility stays live. */
      correction.opacity = ((effect.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                           pass_through_scale;
    }
    else {
      if (!describe_only &&
          !BKE_paint_layers_slice_render(ma,
                                         effect,
                                         channel,
                                         &correction.anchor_point_slice,
                                         &correction.anchor_point_coverage,
                                         slice_rect,
                                         dimensions_only))
      {
        return;
      }
      correction.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, effect, channel);
      BKE_paint_layers_anchor_point_slice_images(ma, effect, correction.anchor_point_images);
    }
  }
  else if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_GENERATES)) {
    /* A Gradient correction has no map: its ramp is laid by its own blend and opacity. */
    if (!composite_gradient_describe(ma, effect, channel, context_surface, correction.gradient)) {
      return;
    }
  }
  else if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* What the source supplies, as a Material row's colour: its plain value, its plain texture
     * or the map rendered from it. A graph with no map yet is the generator's to show alone, so
     * nothing is carried until the render lands. The correction's transparency is a plain value
     * or none (see #paint_layer_correction_reads_source), and a value folds into the opacity. */
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, effect, channel, PaintLayerMaterialUse::Composite);
    const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
        ma, effect, PaintLayerMaterialUse::Composite);
    if (source.kind == PaintLayerMaterialSourceKind::None ||
        coverage.kind == PaintLayerMaterialCoverageKind::Pending)
    {
      return;
    }
    if (source.kind == PaintLayerMaterialSourceKind::LiveConstant) {
      copy_v4_v4(correction.constant_color, source.constant);
      correction.has_constant_color = true;
    }
    else {
      correction.image = source.image;
      correction.iuser = (source.kind == PaintLayerMaterialSourceKind::LiveImage) ?
                             source.iuser :
                             nullptr;
      /* The source is read at the correction's UV, as the generator's source wrapper does and as
       * a Material row's map is: the Image Editor's composite and the gizmo go through this. */
      composite_mapping_describe(ma, effect, channel, correction.mapping);
    }
    if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
      correction.opacity *= clamp_f(coverage.constant, 0.0f, 1.0f);
    }
  }
  else {
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
  }
  correction.blend = layer_channel_blend(effect, channel);
  out.content_corrections.append(correction);
}

/**
 * Describe the row \a layer (not a Pass Through folder) in \a channel to the evaluator. False when
 * the row takes no part, which the generator leaves out as well.
 *
 * A row whose bake stands in (#paint_layer_row_substituted) is one plain layer of its baked colour
 * and coverage maps: the bake already holds the row's corrections, mask and opacity, so nothing of
 * them is described again. \a ignore_own_bake describes the live row instead, which is what the
 * bake itself renders from.
 */
static bool composite_image_layer_build(const Material &ma,
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
                                            &context_surface = nullptr)
{
  /* A reader lays the slice its point publishes, described as a substituted row: the slice's
   * colour, its coverage and its content alpha, with the reader's own blend. Its own mapping does
   * not remap the slice. */
  bool reader_live = false;
  if (paint_layer_kind_reads_anchor_point(layer)) {
    if (!paint_layer_row_reads_anchor_point(ma, layer, channel)) {
      return false;
    }
    const PaintLayerAnchorPointRef reader_ref = paint_layer_anchor_point_resolve(ma, layer);
    out.anchor_point_reader = layer.marker;
    out.anchor_point_target = int8_t(reader_ref.target);
    out.anchor_point_point = (reader_ref.point != nullptr) ? reader_ref.point->marker : bUUID{};
    /* A baked reader reads the maps its slice was rendered into; the row itself is never
     * substituted, since the description decides what the slice is. The bake renders from the live
     * description (\a ignore_own_bake), so it never reads its own stale maps. */
    if (!ignore_own_bake && paint_layer_anchor_point_reader_baked(ma, layer) &&
        layer.bake != nullptr && layer.bake->images[channel] != nullptr &&
        layer.bake->coverage != nullptr)
    {
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
      out.color_image = layer.bake->images[channel];
      out.color_iuser = nullptr;
      out.mask_image = layer.bake->coverage;
      out.mask_iuser = nullptr;
      out.mask_from_alpha = false;
      out.mask_influence = 1.0f;
      out.color_alpha_coverage = record != nullptr && record->image != nullptr;
      out.blend = layer_channel_blend(layer, channel);
      out.opacity = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                    pass_through_scale;
      out.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, layer, channel);
      out.enabled = true;
      out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
          eMaterialPaintChannel(channel));
      /* The bake already holds the reader's corrections, mask and opacity, as a substituted row's
       * does, so none of them is described again. */
      return true;
    }
    if (!describe_only &&
        !BKE_paint_layers_slice_render(ma,
                                       layer,
                                       channel,
                                       &out.anchor_point_color,
                                       &out.anchor_point_coverage,
                                       slice_rect,
                                       dimensions_only))
    {
      return false;
    }
    /* Describe-only keeps the reader even where #BKE_paint_layers_slice_render would refuse it for
     * a zero-sized source. The GPU plan captures the slot and the screen-space pass needs no source
     * size; a caller that later falls back to the CPU rebuilds the full description, which refuses
     * it there. */
    out.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, layer, channel);
    BKE_paint_layers_anchor_point_slice_images(ma, layer, out.anchor_point_images);
    /* The reader's own corrections and mask go on below like any row's, so a point inside this
     * row slices them too. */
    reader_live = true;
  }
  if (!reader_live && !ignore_own_bake && paint_layer_row_substituted(ma, layer)) {
    Image *baked_color = nullptr;
    if (!paint_layer_row_baked_channel(ma, layer, channel, &baked_color)) {
      return false;
    }
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    out.color_image = baked_color;
    out.color_iuser = nullptr;
    out.mask_image = layer.bake->coverage;
    out.mask_iuser = nullptr;
    out.mask_from_alpha = false;
    out.mask_influence = 1.0f;
    /* The colour map's alpha is the content alpha the live row had (a folder's subtree, or its
     * map); a flat constant had none and covers by the shared coverage alone, as it does live. */
    out.color_alpha_coverage = BKE_paint_layers_is_folder(layer) ||
                               (record != nullptr && record->image != nullptr);
    out.blend = layer_channel_blend(layer, channel);
    /* The coverage map already holds the row's opacity and mask; only visibility stays live, the
     * same 0/1 the generated graph multiplies in. */
    out.opacity = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) * pass_through_scale;
    out.enabled = true;
    out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
        eMaterialPaintChannel(channel));
    return true;
  }
  /* A live reader's slice is described above; every other row describes its own source. */
  if (!reader_live) {
    if (BKE_paint_layers_is_folder(layer)) {
      Vector<PaintMaterialCompositeImageLayer> children;
      composite_image_layers_collect(ma,
                                     layer.children,
                                     channel,
                                     pass_through_scale,
                                     context_surface,
                                     children,
                                     true,
                                     slice_rect,
                                     dimensions_only,
                                     describe_only);
      /* An emptied folder is left out, as the generator leaves it; one with a correction that lays
       * here is not: the correction brings the coverage in over nothing. */
      if (children.is_empty() && !paint_layer_effects_lay(ma, layer, channel)) {
        return false;
      }
      out.is_folder = true;
      out.children.assign(std::make_move_iterator(children.begin()),
                          std::make_move_iterator(children.end()));
    }
    else if (!composite_row_source_describe(
                 ma, layer, channel, isolating, context_surface, out))
    {
      return false;
    }
  }
  out.blend = layer_channel_blend(layer, channel);
  out.opacity = BKE_paint_layers_channel_opacity_effective(layer, channel) * pass_through_scale;
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
        ma, layer, PaintLayerMaterialUse::Composite);
    if (coverage.kind == PaintLayerMaterialCoverageKind::LiveConstant) {
      out.opacity *= clamp_f(coverage.constant, 0.0f, 1.0f);
    }
  }
  out.enabled = true;
  out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
      eMaterialPaintChannel(channel));
  /* The corrections lay over the row's colour, in the order they were added. A correction that
   * lays nothing in this channel is not carried, as the generator builds nothing for it. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (stop_before != nullptr && BLI_uuid_equal(effect.marker, *stop_before)) {
      break;
    }
    composite_effect_append(ma,
                            effect,
                            channel,
                            pass_through_scale,
                            slice_rect,
                            dimensions_only,
                            describe_only,
                            context_surface,
                            out);
  }
  /* The mask is one for all channels of the row, so the same item reaches every channel's stack,
   * exactly as the generator reads the one set of mask inputs in every channel chain. */
  composite_mask_items_describe(ma,
                               layer,
                               out.mask_items,
                               stop_before,
                               slice_rect,
                               dimensions_only,
                               describe_only,
                               context_surface);
  /* Mark the positions the GPU has to capture: a marker is a producer only while at least one
   * reader lays its slice. A content marker sits in the effects, a mask marker in the mask stack. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (paint_layer_kind_is_anchor_point_marker(effect) &&
        paint_layer_anchor_point_is_read(ma, effect.marker))
    {
      out.anchor_point_content_producers.append(effect.marker);
    }
  }
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if (paint_layer_kind_is_anchor_point_marker(item) &&
        paint_layer_anchor_point_is_read(ma, item.marker))
    {
      out.anchor_point_mask_producers.append(item.marker);
    }
  }
  return true;
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
                                           const std::shared_ptr<const PaintMaterialCompositeSurface>
                                               &context_surface,
                                           Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                           const bool isolating,
                                           const int *slice_rect,
                                           const bool dimensions_only,
                                           const bool describe_only)
{
  for (const MaterialPaintLayer &layer : list) {
    /* A Pass Through folder is flattened into this list, the way the generator expands it in the
     * chain. It has no row to carry its visibility, so a hidden one hands its rows a zero factor
     * on their opacity, accumulated on the way down (the same rule as #paint_layers_form_map). */
    if (BKE_paint_layers_folder_is_pass_through(layer)) {
      const float scale = (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? pass_through_scale : 0.0f;
      composite_image_layers_collect(ma,
                                     layer.children,
                                     channel,
                                     scale,
                                     context_surface,
                                     r_layers,
                                     isolating,
                                     slice_rect,
                                     dimensions_only,
                                     describe_only);
      continue;
    }
    PaintMaterialCompositeImageLayer out;
    if (composite_image_layer_build(ma,
                                   layer,
                                   channel,
                                   pass_through_scale,
                                   isolating,
                                   false,
                                   out,
                                   nullptr,
                                   slice_rect,
                                   dimensions_only,
                                   describe_only,
                                   context_surface))
    {
      r_layers.append(std::move(out));
    }
  }
}

bool BKE_paint_layers_composite_image_layers(
    const Material &ma,
    const int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    const bool describe_only,
    std::shared_ptr<const PaintMaterialCompositeSurface> surface)
{
  r_layers.clear();
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  composite_image_layers_collect(
      ma, ma.paint_layers, channel, 1.0f, surface, r_layers, false, nullptr, false, describe_only);
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
                                 Vector<PaintMaterialCompositeMaskItem> &r_items,
                                 const bUUID *stop_before)
{
  r_items.clear();
  composite_mask_items_describe(ma, layer, r_items, stop_before);
  return !r_items.is_empty();
}

PaintLayersMaskSampler::PaintLayersMaskSampler(const Material &ma,
                                               const MaterialPaintLayer &layer,
                                               const bUUID *stop_before)
{
  composite_mask_items_describe(ma, layer, items_, stop_before);
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
    buffer.gradient = item.gradient;
    if (buffer.gradient.enabled && buffer.gradient.payload.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV)
    {
      /* An Object/Generated line needs the object's surface to turn a texel into the coordinate
       * the render reads, which a stroke has no way to get here. Reading the texel's UV instead
       * would hide other texels than the render does, so the item takes no part in the stroke's
       * mask: the render still applies it.
       * TODO: hand the sampler the surface once the strokes can reach one. */
      buffer.opacity = 0.0f;
    }
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
    if (item.anchor_point_slice != nullptr) {
      /* A slice is already in canvas space and the description keeps its buffer alive: take an
       * ImBuf reference so the tile's destructor can release it like any other acquired map. A
       * slice of another size than the tile is not sampled, as for a painted map below. */
      if (item.anchor_point_slice->x == ref_width && item.anchor_point_slice->y == ref_height) {
        IMB_refImBuf(item.anchor_point_slice.get());
        tile.items[i] = item.anchor_point_slice.get();
        tile.item_samples[i] = item.anchor_point_slice->float_data();
      }
    }
    else if (item.image != nullptr) {
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
  return BKE_paint_material_mask_factor_eval(buffers_.as_span(),
                                             tile.item_samples,
                                             tile.coverage_samples,
                                             pixel_index,
                                             {x, y, tile.width, tile.height});
}

/**
 * The slice cache: one pair of buffers per reader, channel and description hash. The description
 * hash holds no pixels, so each entry also remembers the version of every map the slice was
 * rendered from (see #ImageRuntime::pixel_version). A lookup compares them: unchanged is a hit, an
 * edit of a rectangle re-renders that rectangle into the buffers, and anything else renders the
 * whole slice again. The cache holds a reference, never the only one: the description that renders
 * a slice shares the buffers for its own lifetime, so clearing or evicting an entry can never pull
 * pixels out from under an evaluation, and an update of an entry someone still holds works on a
 * copy. The byte budget accounts for what the cache itself retains; a buffer still referenced
 * elsewhere temporarily exceeds it, which is allowed.
 */
struct AnchorSliceCacheKey {
  uint32_t material_uid = 0;
  bUUID reader = {};
  int channel = 0;
  uint64_t hash = 0;
};

/** A map a slice was last made from, and the version of its pixels at that time. */
struct AnchorSliceSource {
  uint32_t image_uid = 0;
  uint64_t version = 0;
};

/** The maps the slice of \a reader is rendered from, once each and none of them null. */
static void anchor_slice_images_get(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    Vector<Image *> &r_images)
{
  Vector<Image *> all;
  BKE_paint_layers_anchor_point_slice_images(ma, reader, all);
  for (Image *image : all) {
    if (image != nullptr && image->runtime != nullptr && !r_images.contains(image)) {
      r_images.append(image);
    }
  }
}

static Vector<AnchorSliceSource> anchor_slice_sources_snapshot(const Span<Image *> images)
{
  Vector<AnchorSliceSource> sources;
  for (const Image *image : images) {
    sources.append({image->id.session_uid,
                    image->runtime->pixel_version.load(std::memory_order_relaxed)});
  }
  return sources;
}

/**
 * What happened to the maps of a slice since \a synced was taken: nothing, one rectangle (in
 * \a r_region), or anything. A map the slice did not read before counts as anything.
 */
static ePixelChange anchor_slice_changes(const Span<AnchorSliceSource> synced,
                                         const Span<Image *> images,
                                         rcti &r_region)
{
  BLI_rcti_init(&r_region, 0, 0, 0, 0);
  if (synced.size() != images.size()) {
    return ePixelChange::Full;
  }
  ePixelChange result = ePixelChange::None;
  for (const Image *image : images) {
    const AnchorSliceSource *known = nullptr;
    for (const AnchorSliceSource &source : synced) {
      if (source.image_uid == image->id.session_uid) {
        known = &source;
        break;
      }
    }
    if (known == nullptr) {
      return ePixelChange::Full;
    }
    rcti region;
    switch (BKE_image_pixel_changes_since(*image, known->version, region)) {
      case ePixelChange::None:
        break;
      case ePixelChange::Full:
        return ePixelChange::Full;
      case ePixelChange::Region:
        if (result == ePixelChange::None) {
          r_region = region;
        }
        else {
          BLI_rcti_union(&r_region, &region);
        }
        result = ePixelChange::Region;
        break;
    }
  }
  return result;
}

/**
 * Total memory the slice cache may hold, counted as it is allocated: a colour and a coverage buffer
 * of four floats per pixel each, so one 4K pair is 512 MiB. A composite of the shaded channels
 * needs one pair per channel of a reader at once, and an entry evicted between two channels is
 * rendered whole again; 4 GiB holds the working set of a reader at 4K and keeps the few most
 * recent others. A pair larger than the budget is never cached.
 */
constexpr size_t PAINT_LAYER_ANCHOR_SLICE_CACHE_BYTES = size_t(4096) * 1024 * 1024;

/** The live budget; only tests override it, to make eviction observable without 4K fixtures. */
static size_t g_anchor_slice_cache_budget = PAINT_LAYER_ANCHOR_SLICE_CACHE_BYTES;

struct AnchorSliceCacheEntry {
  AnchorSliceCacheKey key;
  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  /** The maps the buffers were rendered from, with the versions they had. */
  Vector<AnchorSliceSource> synced;
  size_t bytes = 0;
};

static size_t anchor_slice_color_bytes(const ImBuf *ibuf)
{
  return ibuf != nullptr ? size_t(ibuf->x) * size_t(ibuf->y) * sizeof(float[4]) : 0;
}

/** Coverage is stored as RGBA too: consumers read every channel. */
static size_t anchor_slice_coverage_bytes(const ImBuf *ibuf)
{
  return ibuf != nullptr ? size_t(ibuf->x) * size_t(ibuf->y) * sizeof(float[4]) : 0;
}

/** The cache owns references only; dropping an entry simply releases them. */
struct AnchorSliceCache {
  Vector<AnchorSliceCacheEntry> entries;
  size_t total_bytes = 0;
};

static AnchorSliceCache g_anchor_slice_cache;
/**
 * Guards the container and the byte count only. A bake job renders on a worker thread while the
 * main thread describes the composite, and both reach the cache. It is never held while a slice
 * renders, so nested renders cannot deadlock on it.
 */
static std::mutex g_anchor_slice_cache_mutex;

void paint_layer_anchor_point_slice_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  /* The buffer goes too: the cache is a static, so a kept buffer would outlive the leak check that
   * runs when the program shuts down, and a cleared cache has nothing to reuse it for. */
  g_anchor_slice_cache.entries.clear_and_shrink();
  g_anchor_slice_cache.total_bytes = 0;
}

void BKE_paint_layers_slice_cache_free_all()
{
  paint_layer_anchor_point_slice_cache_clear();
}

void BKE_paint_layers_slice_cache_free_material(const Material &ma)
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  for (int i = int(g_anchor_slice_cache.entries.size()) - 1; i >= 0; i--) {
    if (g_anchor_slice_cache.entries[i].key.material_uid == ma.id.session_uid) {
      g_anchor_slice_cache.total_bytes -= g_anchor_slice_cache.entries[i].bytes;
      g_anchor_slice_cache.entries.remove(i);
    }
  }
}

size_t paint_layer_anchor_point_slice_cache_bytes()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  return g_anchor_slice_cache.total_bytes;
}

void paint_layer_anchor_point_slice_cache_set_budget(const size_t bytes)
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  g_anchor_slice_cache_budget = bytes;
}

size_t paint_layer_anchor_point_slice_cache_budget()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  return g_anchor_slice_cache_budget;
}

int paint_layer_anchor_point_slice_cache_entry_count()
{
  std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
  return int(g_anchor_slice_cache.entries.size());
}

static bool anchor_slice_key_equal(const AnchorSliceCacheKey &a, const AnchorSliceCacheKey &b)
{
  return a.material_uid == b.material_uid && BLI_uuid_equal(a.reader, b.reader) &&
         a.channel == b.channel && a.hash == b.hash;
}

/** An RGBA float buffer of \a width x \a height, owned by the returned shared pointer. */
static std::shared_ptr<ImBuf> anchor_slice_buffer_new(const int width, const int height)
{
  ImBuf *ibuf = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    return {};
  }
  return std::shared_ptr<ImBuf>(ibuf, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

/**
 * The pixels a slice render writes: the whole buffer for a complete render, else \a dst_rect
 * (x0, y0, x1, y1) clamped to it. False when nothing is left to render.
 */
static bool anchor_slice_region_get(const int *dst_rect,
                                    const int width,
                                    const int height,
                                    int &r_x0,
                                    int &r_y0,
                                    int &r_x1,
                                    int &r_y1)
{
  r_x0 = 0;
  r_y0 = 0;
  r_x1 = width;
  r_y1 = height;
  if (dst_rect != nullptr) {
    r_x0 = clamp_i(dst_rect[0], 0, width);
    r_y0 = clamp_i(dst_rect[1], 0, height);
    r_x1 = clamp_i(dst_rect[2], 0, width);
    r_y1 = clamp_i(dst_rect[3], 0, height);
  }
  return r_x1 > r_x0 && r_y1 > r_y0;
}

/** The unwritten part of a partial render is defined as zero, never uninitialized memory. A
 * complete render writes every pixel, so it skips the pass. */
static void anchor_slice_buffers_clear_if_partial(const int *dst_rect,
                                                  ImBuf &color,
                                                  ImBuf &coverage)
{
  if (dst_rect == nullptr) {
    return;
  }
  const size_t size = size_t(color.x) * color.y * sizeof(float[4]);
  memset(color.float_data_for_write(), 0, size);
  memset(coverage.float_data_for_write(), 0, size);
}

bool BKE_paint_layers_slice_describe(const Material &ma,
                                     const MaterialPaintLayer &reader,
                                     const int channel,
                                     PaintMaterialCompositeImageLayer &r_entry,
                                     const int *slice_rect,
                                     const bool dimensions_only,
                                     const bool allow_empty_mask)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr ||
      ref.point == nullptr)
  {
    return false;
  }
  r_entry = PaintMaterialCompositeImageLayer{};
  if (ref.target == PaintLayerAnchorPointTarget::Mask) {
    /* A mask slice is the source row's mask stack up to the point, the base mask included as its
     * first item. It has no opacity, blend or mask of its own. */
    composite_mask_items_describe(ma,
                                  *ref.source_row,
                                  r_entry.mask_items,
                                  &ref.point->marker,
                                  slice_rect,
                                  dimensions_only);
    if (r_entry.mask_items.is_empty() && !allow_empty_mask) {
      return false;
    }
  }
  else {
    if (!composite_image_layer_build(ma,
                                     *ref.source_row,
                                     channel,
                                     1.0f,
                                     false,
                                     true,
                                     r_entry,
                                     &ref.point->marker,
                                     slice_rect,
                                     dimensions_only))
    {
      return false;
    }
    /* The slice is the row's content before its opacity, mask, object mask and blend: only what
     * the corrections above the marker left. */
    r_entry.mask_items.clear();
    r_entry.mask_image = nullptr;
    r_entry.mask_iuser = nullptr;
    r_entry.mask_from_alpha = false;
    r_entry.mask_influence = 0.0f;
  }
  r_entry.opacity = 1.0f;
  r_entry.enabled = true;
  return true;
}

/** The size of the slice of \a reader: the source row's, like a bake of that row. */
static bool anchor_slice_dimensions(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    const int channel,
                                    const PaintLayerAnchorPointRef &ref,
                                    int &r_width,
                                    int &r_height)
{
  r_width = 0;
  r_height = 0;
  if (ref.target == PaintLayerAnchorPointTarget::Mask) {
    return BKE_paint_layers_row_dimensions(
               ma, *ref.source_row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, r_width, r_height) &&
           r_width > 0 && r_height > 0;
  }
  /* Described without rendering, so the nested slices this one reads are not rendered (or
   * allocated) just to learn a size. */
  PaintMaterialCompositeImageLayer entry;
  return BKE_paint_layers_slice_describe(ma, reader, channel, entry, nullptr, true) &&
         BKE_paint_material_composite_stack_dimensions(
             Span<PaintMaterialCompositeImageLayer>(&entry, 1), r_width, r_height) &&
         r_width > 0 && r_height > 0;
}

/**
 * Render the slice of \a reader into the buffers, which are already of the slice's size: the whole
 * of them when \a rect is null, else the rectangle (x0, y0, x1, y1), leaving the rest as it is.
 * False when the slice cannot be rendered there.
 */
static bool anchor_slice_fill(const Material &ma,
                              const MaterialPaintLayer &reader,
                              const int channel,
                              const PaintLayerAnchorPointRef &ref,
                              const int *rect,
                              ImBuf &color,
                              ImBuf &coverage)
{
  const int width = color.x;
  const int height = color.y;
  int rx0, ry0, rx1, ry1;
  if (!anchor_slice_region_get(rect, width, height, rx0, ry0, rx1, ry1)) {
    return false;
  }
  float *color_pixels = color.float_data_for_write();
  float *coverage_pixels = coverage.float_data_for_write();

  if (ref.target == PaintLayerAnchorPointTarget::Mask) {
    PaintLayersMaskSampler sampler(ma, *ref.source_row, &ref.point->marker);
    sampler.tile_acquire(0, width, height);
    for (int y = ry0; y < ry1; y++) {
      for (int x = rx0; x < rx1; x++) {
        const int64_t index = int64_t(y) * width + x;
        const float gray = sampler.sample(0, x, y);
        color_pixels[index * 4 + 0] = gray;
        color_pixels[index * 4 + 1] = gray;
        color_pixels[index * 4 + 2] = gray;
        /* A mask point read as content is opaque grey; read as a mask item it is the scalar. */
        color_pixels[index * 4 + 3] = 1.0f;
        coverage_pixels[index * 4 + 0] = 1.0f;
        coverage_pixels[index * 4 + 1] = 1.0f;
        coverage_pixels[index * 4 + 2] = 1.0f;
        coverage_pixels[index * 4 + 3] = 1.0f;
      }
    }
    return true;
  }

  PaintMaterialCompositeImageLayer entry;
  /* Nested readers inside the content slice must see the same rectangle: without it every nested
   * slice would render fully even when only a rectangle is wanted. */
  if (!BKE_paint_layers_slice_describe(ma, reader, channel, entry, rect)) {
    return false;
  }
  int entry_width = 0;
  int entry_height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(
          Span<PaintMaterialCompositeImageLayer>(&entry, 1), entry_width, entry_height) ||
      entry_width != width || entry_height != height)
  {
    return false;
  }
  const int region_w = rx1 - rx0;
  const int region_h = ry1 - ry0;
  const bool full = rect == nullptr;
  /* A complete render evaluates straight into the colour buffer; only a partial one needs a
   * region-sized scratch to copy from. The coverage is a scalar plane either way, widened below. */
  Vector<float> region_color;
  rcti region;
  BLI_rcti_init(&region, rx0, rx1, ry0, ry1);
  float *color_out = color_pixels;
  if (!full) {
    region_color.resize(int64_t(region_w) * region_h * 4);
    color_out = region_color.data();
  }
  Vector<float> region_coverage(int64_t(region_w) * region_h);
  if (!BKE_paint_material_composite_eval_row_content(
          entry, color_out, region_coverage.data(), full ? nullptr : &region, channel))
  {
    return false;
  }
  for (int y = ry0; y < ry1; y++) {
    for (int x = rx0; x < rx1; x++) {
      const int64_t src = int64_t(y - ry0) * region_w + (x - rx0);
      const int64_t dst = int64_t(y) * width + x;
      if (!full) {
        memcpy(color_pixels + dst * 4, region_color.data() + src * 4, sizeof(float[4]));
      }
      /* The consumers read the coverage buffer as RGBA with the value in each colour channel. */
      const float value = region_coverage[src];
      coverage_pixels[dst * 4 + 0] = value;
      coverage_pixels[dst * 4 + 1] = value;
      coverage_pixels[dst * 4 + 2] = value;
      coverage_pixels[dst * 4 + 3] = 1.0f;
    }
  }
  return true;
}

/** Render the slice into buffers of its own: all of it for a null \a dst_rect, else that rectangle
 * with the rest zero. */
static bool anchor_slice_render_new(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    const int channel,
                                    const PaintLayerAnchorPointRef &ref,
                                    const int *dst_rect,
                                    std::shared_ptr<ImBuf> &r_color,
                                    std::shared_ptr<ImBuf> &r_coverage)
{
  int width = 0;
  int height = 0;
  if (!anchor_slice_dimensions(ma, reader, channel, ref, width, height)) {
    return false;
  }
  int rx0, ry0, rx1, ry1;
  if (!anchor_slice_region_get(dst_rect, width, height, rx0, ry0, rx1, ry1)) {
    return false;
  }
  std::shared_ptr<ImBuf> color = anchor_slice_buffer_new(width, height);
  std::shared_ptr<ImBuf> coverage = anchor_slice_buffer_new(width, height);
  if (color == nullptr || coverage == nullptr) {
    return false;
  }
  anchor_slice_buffers_clear_if_partial(dst_rect, *color, *coverage);
  if (!anchor_slice_fill(ma, reader, channel, ref, dst_rect, *color, *coverage)) {
    return false;
  }
  r_color = std::move(color);
  r_coverage = std::move(coverage);
  return true;
}

/**
 * Whether nothing in \a layer moves a texel of a map: no mapping on the row, its corrections or
 * its mask items, and no reader whose own source would have to be looked into. Every filter and
 * blend a slice can hold works on the texel it is given, so with this a rectangle edited in a map
 * is the same rectangle of the slice.
 */
static bool anchor_slice_layer_is_texel_exact(const PaintMaterialCompositeImageLayer &layer)
{
  if (layer.mapping.enabled || !BLI_uuid_is_nil(layer.anchor_point_reader)) {
    return false;
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.mapping.enabled || !BLI_uuid_is_nil(correction.anchor_point_reader)) {
      return false;
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    if (item.mapping.enabled || item.coverage_mapping.enabled ||
        !BLI_uuid_is_nil(item.anchor_point_reader))
    {
      return false;
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    if (!anchor_slice_layer_is_texel_exact(child)) {
      return false;
    }
  }
  return true;
}

/**
 * Whether a rectangle reported on one of \a images can be applied to the slice as it is: the slice
 * is described without remapping any map, and no map that was ever edited has another size than the
 * slice. Anything else is a full render, which is always right.
 */
static bool anchor_slice_region_is_exact(const Material &ma,
                                         const MaterialPaintLayer &reader,
                                         const int channel,
                                         const Span<Image *> images,
                                         const int width,
                                         const int height)
{
  PaintMaterialCompositeImageLayer entry;
  if (!BKE_paint_layers_slice_describe(ma, reader, channel, entry, nullptr, true) ||
      !anchor_slice_layer_is_texel_exact(entry))
  {
    return false;
  }
  for (const Image *image : images) {
    int image_width = 0;
    int image_height = 0;
    BKE_image_pixel_journal_size(*image, image_width, image_height);
    /* An image no edit was marked on is unchanged, whatever its size. */
    if (image_width > 0 && (image_width != width || image_height != height)) {
      return false;
    }
  }
  return true;
}

/** A private copy of \a source, for an update of a buffer someone else still reads. */
static std::shared_ptr<ImBuf> anchor_slice_buffer_clone(const ImBuf &source)
{
  ImBuf *copy = IMB_dupImBuf(&source);
  if (copy == nullptr) {
    return {};
  }
  return std::shared_ptr<ImBuf>(copy, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

bool BKE_paint_layers_slice_render(const Material &ma,
                                   const MaterialPaintLayer &reader,
                                   const int channel,
                                   std::shared_ptr<ImBuf> *r_color,
                                   std::shared_ptr<ImBuf> *r_coverage,
                                   const int *dst_rect,
                                   const bool dimensions_only)
{
  *r_color = {};
  *r_coverage = {};
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr) {
    return false;
  }
  if (dimensions_only) {
    /* Sizing only: every nested slice is left unrendered and unallocated, and the buffers are not
     * needed. */
    int width = 0;
    int height = 0;
    return anchor_slice_dimensions(ma, reader, channel, ref, width, height);
  }
  if (dst_rect != nullptr) {
    /* A partial render is not a complete slice and is never cached: it is only produced for a bake
     * job that writes the requested rectangle into its own full-size map. */
    if (!anchor_slice_render_new(ma, reader, channel, ref, dst_rect, *r_color, *r_coverage)) {
      return false;
    }
    return true;
  }

  AnchorSliceCacheKey key;
  Vector<Image *> images;
  key.material_uid = ma.id.session_uid;
  key.reader = reader.marker;
  /* A mask slice is the same grey for every channel: one cache entry per reader. */
  key.channel = (ref.target == PaintLayerAnchorPointTarget::Mask) ? -1 : channel;
  key.hash = paint_layer_anchor_point_slice_hash(ma, reader, channel);
  anchor_slice_images_get(ma, reader, images);
  /* Read before anything is rendered: an edit that lands while the slice renders is then seen as
   * newer than the entry, and costs one more update rather than a stale slice. */
  Vector<AnchorSliceSource> sources_now = anchor_slice_sources_snapshot(images);

  /* An entry that needs work is taken out of the cache while it is worked on, so no other thread
   * can pick up buffers that are being written; it goes back afterwards. */
  AnchorSliceCacheEntry held;
  bool have_entry = false;
  ePixelChange change = ePixelChange::Full;
  rcti region;
  BLI_rcti_init(&region, 0, 0, 0, 0);
  {
    std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
    for (const int i : g_anchor_slice_cache.entries.index_range()) {
      if (!anchor_slice_key_equal(g_anchor_slice_cache.entries[i].key, key)) {
        continue;
      }
      change = anchor_slice_changes(g_anchor_slice_cache.entries[i].synced, images, region);
      if (change == ePixelChange::None) {
        /* LRU: move the hit to the newest end. The buffers stay alive through the shared pointers
         * the caller receives, whatever the cache does next. */
        AnchorSliceCacheEntry hit = std::move(g_anchor_slice_cache.entries[i]);
        g_anchor_slice_cache.entries.remove(i);
        *r_color = hit.color;
        *r_coverage = hit.coverage;
        g_anchor_slice_cache.entries.append(std::move(hit));
        return true;
      }
      held = std::move(g_anchor_slice_cache.entries[i]);
      g_anchor_slice_cache.total_bytes -= held.bytes;
      g_anchor_slice_cache.entries.remove(i);
      have_entry = true;
      break;
    }
  }

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  bool updated = false;
  if (have_entry && change == ePixelChange::Region && held.color != nullptr &&
      held.coverage != nullptr &&
      anchor_slice_region_is_exact(ma, reader, channel, images, held.color->x, held.color->y))
  {
    /* Only the edited rectangle is rendered again. The buffers are written in place when the cache
     * is their only owner; a description that still reads them gets them unchanged, and the
     * update works on a copy. */
    color = std::move(held.color);
    coverage = std::move(held.coverage);
    if (color.use_count() > 1) {
      color = anchor_slice_buffer_clone(*color);
    }
    if (coverage.use_count() > 1) {
      coverage = anchor_slice_buffer_clone(*coverage);
    }
    if (color != nullptr && coverage != nullptr) {
      int rx0, ry0, rx1, ry1;
      const int rect[4] = {region.xmin, region.ymin, region.xmax, region.ymax};
      if (!anchor_slice_region_get(rect, color->x, color->y, rx0, ry0, rx1, ry1)) {
        /* The edit lies outside the slice: nothing of it is in there to change. */
        updated = true;
      }
      else {
        updated = anchor_slice_fill(ma, reader, channel, ref, rect, *color, *coverage);
      }
    }
    if (!updated) {
      /* A half-written buffer is not reused. */
      color = {};
      coverage = {};
    }
  }
  if (!updated &&
      !anchor_slice_render_new(ma, reader, channel, ref, nullptr, color, coverage))
  {
    return false;
  }

  {
    AnchorSliceCacheEntry new_entry;
    new_entry.key = key;
    new_entry.color = color;
    new_entry.coverage = coverage;
    new_entry.synced = std::move(sources_now);
    new_entry.bytes = anchor_slice_color_bytes(color.get()) +
                      anchor_slice_coverage_bytes(coverage.get());
    std::lock_guard<std::mutex> lock(g_anchor_slice_cache_mutex);
    /* Bound the cache by memory, not by a number of entries: one 4K pair is far larger than a
     * handful of small slices. The oldest entry is the LRU one; a hit moves to the newest end
     * above. Evicting only drops the cache's reference: a description or a caller that still holds
     * the buffers keeps them alive. Another thread may have rendered the same slice meanwhile; the
     * newer one replaces it. */
    for (int i = int(g_anchor_slice_cache.entries.size()) - 1; i >= 0; i--) {
      if (anchor_slice_key_equal(g_anchor_slice_cache.entries[i].key, key)) {
        g_anchor_slice_cache.total_bytes -= g_anchor_slice_cache.entries[i].bytes;
        g_anchor_slice_cache.entries.remove(i);
      }
    }
    /* A slice larger than the whole budget is not kept: it would evict everything and then stay
     * pinned after the description that needed it is gone. The caller still receives it. */
    if (new_entry.bytes <= g_anchor_slice_cache_budget) {
      while (!g_anchor_slice_cache.entries.is_empty() &&
             g_anchor_slice_cache.total_bytes + new_entry.bytes > g_anchor_slice_cache_budget)
      {
        g_anchor_slice_cache.total_bytes -= g_anchor_slice_cache.entries[0].bytes;
        g_anchor_slice_cache.entries.remove(0);
      }
      g_anchor_slice_cache.total_bytes += new_entry.bytes;
      g_anchor_slice_cache.entries.append(std::move(new_entry));
    }
  }
  *r_color = std::move(color);
  *r_coverage = std::move(coverage);
  return true;
}

/**
 * The live description of \a layer in \a channel, as the evaluator reads one row: what a bake of
 * the row renders. A Pass Through folder has no row of its own to describe.
 */
static bool composite_bake_entry(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 const int channel,
                                 PaintMaterialCompositeImageLayer &r_entry,
                                 const int *slice_rect = nullptr,
                                 const bool dimensions_only = false)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      (BKE_paint_layers_is_folder(layer) && BKE_paint_layers_folder_is_pass_through(layer)))
  {
    return false;
  }
  return composite_image_layer_build(
      ma, layer, channel, 1.0f, false, true, r_entry, nullptr, slice_rect, dimensions_only);
}

bool BKE_paint_layers_bake_render_node(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const int channel,
                                       const int size,
                                       float *r_color_rgba,
                                       float *r_coverage_gray,
                                       const int *dst_rect)
{
  if (size <= 0 || r_color_rgba == nullptr || r_coverage_gray == nullptr) {
    return false;
  }
  /* A row's bake is a shared, object-independent map. An Object/Generated Gradient reads geometry
   * the map cannot carry, and a folder containing one has the same problem for its whole subtree. */
  if (!BKE_paint_layers_gradient_subtree_is_shared_bakeable(layer)) {
    return false;
  }
  /* Learn the stack dimensions without rendering or allocating a single nested slice. */
  PaintMaterialCompositeImageLayer dims_entry;
  if (!composite_bake_entry(ma, layer, channel, dims_entry, nullptr, true)) {
    return false;
  }
  /* The row's content, straight and with its own factor, from the one shared isolated-group
   * formula -- for a leaf and a folder alike. */
  int width = 0;
  int height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(
          Span<PaintMaterialCompositeImageLayer>(&dims_entry, 1), width, height) ||
      width <= 0 || height <= 0)
  {
    return false;
  }
  /* The rectangle of the `size`-square result to produce, and the source pixels it samples. A full
   * render produces all of it; a partial re-bake passes the changed tile so the composite runs over
   * that source rectangle alone. */
  int dx0 = 0, dy0 = 0, dx1 = size, dy1 = size;
  if (dst_rect != nullptr) {
    dx0 = clamp_i(dst_rect[0], 0, size);
    dy0 = clamp_i(dst_rect[1], 0, size);
    dx1 = clamp_i(dst_rect[2], 0, size);
    dy1 = clamp_i(dst_rect[3], 0, size);
  }
  if (dx0 >= dx1 || dy0 >= dy1) {
    return false;
  }
  /* The source pixels nearest-sampled into `[dx0, dx1) x [dy0, dy1)`: the first and last output
   * pixel of the rectangle map to the source span that has to be composited. */
  const int sx0 = int(int64_t(dx0) * width / size);
  const int sx1 = (dx1 >= size) ? width : int((int64_t(dx1) * width + size - 1) / size);
  const int sy0 = int(int64_t(dy0) * height / size);
  const int sy1 = (dy1 >= size) ? height : int((int64_t(dy1) * height + size - 1) / size);
  rcti area;
  BLI_rcti_init(&area, max_ii(0, sx0), min_ii(width, sx1), max_ii(0, sy0), min_ii(height, sy1));
  const int area_w = BLI_rcti_size_x(&area);
  const int area_h = BLI_rcti_size_y(&area);
  if (area_w <= 0 || area_h <= 0) {
    return false;
  }
  /* A partial re-bake only needs the nested slices over the source rectangle the area maps to; a
   * full one renders them completely. The area is only known from the sizes found above, which is
   * why the row is described a second time. */
  const int slice_rect[4] = {area.xmin, area.ymin, area.xmax, area.ymax};
  PaintMaterialCompositeImageLayer entry;
  if (!composite_bake_entry(ma, layer, channel, entry, dst_rect != nullptr ? slice_rect : nullptr)) {
    return false;
  }
  Vector<float> content_color(int64_t(area_w) * area_h * 4);
  Vector<float> content_coverage(int64_t(area_w) * area_h);
  const rcti region = area;
  if (!BKE_paint_material_composite_eval_row_content(
          entry, content_color.data(), content_coverage.data(), &region, channel))
  {
    return false;
  }
  /* Resample only the requested rectangle: each output pixel reads the composited source span. */
  for (int y = dy0; y < dy1; y++) {
    const int src_y = min_ii(int(int64_t(y) * height / size), height - 1);
    const int local_y = src_y - area.ymin;
    for (int x = dx0; x < dx1; x++) {
      const int src_x = min_ii(int(int64_t(x) * width / size), width - 1);
      const int64_t src = int64_t(local_y) * area_w + (src_x - area.xmin);
      const int64_t dst = int64_t(y) * size + x;
      r_color_rgba[dst * 4 + 0] = content_color[src * 4 + 0];
      r_color_rgba[dst * 4 + 1] = content_color[src * 4 + 1];
      r_color_rgba[dst * 4 + 2] = content_color[src * 4 + 2];
      /* The content alpha travels with the colour, so a substituted row reads its own transparency
       * back instead of turning opaque. */
      r_color_rgba[dst * 4 + 3] = content_color[src * 4 + 3];
      r_coverage_gray[dst] = content_coverage[src];
    }
  }
  return true;
}

bool BKE_paint_layers_row_dimensions(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     const int channel,
                                     int &r_width,
                                     int &r_height)
{
  PaintMaterialCompositeImageLayer entry;
  /* Only the size is wanted: nested readers must not render and cache their slices for it. */
  if (!composite_bake_entry(ma, layer, channel, entry, nullptr, true)) {
    return false;
  }
  return BKE_paint_material_composite_stack_dimensions(
      Span<PaintMaterialCompositeImageLayer>(&entry, 1), r_width, r_height);
}

const char *BKE_paint_layers_gpu_unsupported_reason(const PaintMaterialCompositeImageLayer &layer,
                                                    const bool mask_slice)
{
  if (!layer.enabled || layer.opacity <= 0.0f) {
    /* The composite skips it as well. */
    return nullptr;
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    if (child.adjustment_filter != 0) {
      return "an adjustment row is inside a folder";
    }
    if (const char *reason = BKE_paint_layers_gpu_unsupported_reason(child, mask_slice)) {
      return reason;
    }
  }
  const bool reader = !BLI_uuid_is_nil(layer.anchor_point_reader);
  const bool mask_only_slice = mask_slice && !layer.mask_items.is_empty();
  /* A Gradient generator has no map and no flat colour of its own: its pixels come from the ramp at
   * a coordinate the CPU evaluator computes. Stated explicitly rather than left to the map/constant
   * check below, so a future shader that gains a Gradient branch cannot silently shade a different
   * approximation than the CPU exact preview. The caller falls back to the CPU display override. */
  if (layer.gradient.enabled) {
    return "a Gradient generator row is not laid by the GPU path";
  }
  if (!layer.is_folder && layer.adjustment_filter == 0 && layer.color_image == nullptr &&
      layer.anchor_point_color == nullptr && !layer.has_constant_color && !reader &&
      !mask_only_slice)
  {
    return "a row has neither a map, a flat colour nor an Anchor Point slice";
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    /* A sliced correction reads a captured slot. A baked one has no slice buffer but covers by a
     * coverage map the GPU correction pass does not read. */
    if (correction.anchor_point_coverage_image != nullptr) {
      return "a baked Anchor Point correction is read (no coverage map on the GPU)";
    }
  }
  return nullptr;
}

/* -------------------------------------------------------------------- */
/** \name GPU Anchor Point Capture Plan
 * \{ */

namespace {

bool gpu_entry_has_adjustment(const PaintMaterialCompositeImageLayer &entry)
{
  if (entry.adjustment_filter != 0) {
    return true;
  }
  for (const PaintMaterialCompositeImageLayer &child : entry.children) {
    if (gpu_entry_has_adjustment(child)) {
      return true;
    }
  }
  return false;
}

/** A correction that lays a live Anchor Point slice. */
bool gpu_correction_is_live_reader(const PaintMaterialCompositeCorrection &correction)
{
  return !BLI_uuid_is_nil(correction.anchor_point_reader) && correction.image == nullptr &&
         !correction.has_constant_color && correction.filter == 0;
}

/** A mask item that lays a live Anchor Point slice. */
bool gpu_mask_item_is_live_reader(const PaintMaterialCompositeMaskItem &item)
{
  return item.opacity > 0.0f && item.image == nullptr && item.filter == 0 &&
         !BLI_uuid_is_nil(item.anchor_point_reader);
}

/** The capture a row or correction reader needs: a mask point read as content is a grey pair. */
PaintLayersGpuCaptureKind gpu_content_capture_kind(const int8_t target)
{
  return target == int8_t(PaintLayerAnchorPointTarget::Mask) ?
             PaintLayersGpuCaptureKind::MaskContent :
             PaintLayersGpuCaptureKind::Content;
}

}  // namespace

bool BKE_paint_layers_gpu_capture_plan(
    const Material &ma,
    const Span<const Span<const PaintMaterialCompositeImageLayer>> stacks,
    const Span<const int> channels,
    Vector<PaintLayersGpuCaptureJob> &r_jobs,
    const char **r_reason)
{
  r_jobs.clear();
  if (r_reason != nullptr) {
    *r_reason = nullptr;
  }
  if (stacks.size() != channels.size()) {
    if (r_reason != nullptr) {
      *r_reason = "the capture plan's stacks and channels differ in size";
    }
    return false;
  }

  const auto fail = [&](const char *reason) {
    if (r_reason != nullptr) {
      *r_reason = reason;
    }
    return false;
  };

  /* One depth per job, parallel to #r_jobs: a job discovered inside another's description is one
   * level deeper. The hard limit bounds both recursion and work on a malicious description. */
  Vector<int> depths;

  /* Add \a reader's job unless the same slot is already planned. Returns false only when the plan
   * cannot be built. */
  const auto add_job = [&](const bUUID &reader,
                           const bUUID &point,
                           const int channel,
                           const PaintLayersGpuCaptureKind kind,
                           const int depth) -> bool {
    if (BLI_uuid_is_nil(reader) || BLI_uuid_is_nil(point)) {
      return true;
    }
    const bool mask_item = kind == PaintLayersGpuCaptureKind::MaskItem;
    for (PaintLayersGpuCaptureJob &job : r_jobs) {
      if (job.kind != kind || !BLI_uuid_equal(job.point, point)) {
        continue;
      }
      if (!mask_item && job.channel != channel) {
        continue;
      }
      /* A shared slot is one capture; only count a reader that was not already named for it. */
      bool seen = false;
      for (const bUUID &known : job.readers) {
        if (BLI_uuid_equal(known, reader)) {
          seen = true;
          break;
        }
      }
      if (!seen) {
        job.readers.append(reader);
        job.reader_count = int(job.readers.size());
      }
      return true;
    }
    if (depth > PAINT_LAYER_ANCHOR_POINT_DEPTH_HARD) {
      return fail("the Anchor Point chain is too deep for the GPU path");
    }
    if (int(r_jobs.size()) >= PAINT_LAYER_ANCHOR_POINT_LIMIT * int(channels.size())) {
      return fail("too many Anchor Point slices for the GPU path");
    }
    PaintLayersGpuCaptureJob job;
    job.reader = reader;
    job.point = point;
    job.channel = channel;
    job.kind = kind;
    job.rank = paint_layer_anchor_point_rank(ma, point);
    job.readers.append(reader);
    r_jobs.append(std::move(job));
    depths.append(depth);
    return true;
  };

  /* The readers a row (or a described slice entry) lays through its corrections and mask items. */
  const auto collect_corrections_and_mask = [&](const PaintMaterialCompositeImageLayer &row,
                                                const int channel,
                                                const int depth) -> bool {
    for (const PaintMaterialCompositeCorrection &correction : row.content_corrections) {
      if (correction.opacity <= 0.0f || !gpu_correction_is_live_reader(correction)) {
        continue;
      }
      if (!add_job(correction.anchor_point_reader,
                   correction.anchor_point_point,
                   channel,
                   gpu_content_capture_kind(correction.anchor_point_target),
                   depth))
      {
        return false;
      }
    }
    for (const PaintMaterialCompositeMaskItem &item : row.mask_items) {
      if (!gpu_mask_item_is_live_reader(item)) {
        continue;
      }
      const bool mask_target = item.anchor_point_target ==
                               int8_t(PaintLayerAnchorPointTarget::Mask);
      /* A content point read by a mask item is captured in the channel its coverage comes from. */
      if (!add_job(item.anchor_point_reader,
                   item.anchor_point_point,
                   mask_target ? channel : item.anchor_point_read_channel,
                   mask_target ? PaintLayersGpuCaptureKind::MaskItem :
                                 PaintLayersGpuCaptureKind::Content,
                   depth))
      {
        return false;
      }
    }
    return true;
  };

  /* Collect a row that the passes will lay, including its own reader. A hidden or zero-opacity row
   * is skipped: nothing reads it unless another reader's slice contains it, which
   * #collect_nested_entry handles. */
  std::function<bool(const PaintMaterialCompositeImageLayer &, int, int)> collect_layer;
  collect_layer = [&](const PaintMaterialCompositeImageLayer &layer,
                      const int channel,
                      const int depth) -> bool {
    if (!layer.enabled || layer.opacity <= 0.0f) {
      return true;
    }
    if (!BLI_uuid_is_nil(layer.anchor_point_reader) && layer.color_image == nullptr &&
        !add_job(layer.anchor_point_reader,
                 layer.anchor_point_point,
                 channel,
                 gpu_content_capture_kind(layer.anchor_point_target),
                 depth))
    {
      return false;
    }
    if (!collect_corrections_and_mask(layer, channel, depth)) {
      return false;
    }
    for (const PaintMaterialCompositeImageLayer &child : layer.children) {
      if (!collect_layer(child, channel, depth)) {
        return false;
      }
    }
    return true;
  };

  /* The readers inside an already described slice. The entry's own reader is the source row's own
   * reader, which is a nested reader unless it is the one this job was made for. */
  const auto collect_nested_entry = [&](const PaintMaterialCompositeImageLayer &entry,
                                        const int channel,
                                        const int depth,
                                        const bUUID &job_reader) -> bool {
    if (!BLI_uuid_is_nil(entry.anchor_point_reader) && entry.color_image == nullptr &&
        !BLI_uuid_equal(entry.anchor_point_reader, job_reader) &&
        !add_job(entry.anchor_point_reader,
                 entry.anchor_point_point,
                 channel,
                 gpu_content_capture_kind(entry.anchor_point_target),
                 depth))
    {
      return false;
    }
    if (!collect_corrections_and_mask(entry, channel, depth)) {
      return false;
    }
    for (const PaintMaterialCompositeImageLayer &child : entry.children) {
      if (!collect_layer(child, channel, depth)) {
        return false;
      }
    }
    return true;
  };

  for (const int stack_index : stacks.index_range()) {
    const int channel = channels[stack_index];
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : stacks[stack_index]) {
      if (!collect_layer(layer, channel, 0)) {
        return false;
      }
    }
  }

  /* Describe each job once. Jobs discovered while describing are appended and described by the
   * same loop; the entry stays on the job, so the gather never describes a slice twice. */
  for (int index = 0; index < int(r_jobs.size()); index++) {
    const bUUID reader_marker = r_jobs[index].reader;
    const int channel = r_jobs[index].channel;
    const PaintLayersGpuCaptureKind kind = r_jobs[index].kind;
    const int depth = depths[index];
    Material &ma_mut = const_cast<Material &>(ma);
    MaterialPaintLayer *reader = BKE_paint_layers_find(ma_mut, reader_marker);
    if (reader == nullptr) {
      return fail("an Anchor Point reader cannot be found");
    }
    const bool mask_kind = kind != PaintLayersGpuCaptureKind::Content;
    PaintMaterialCompositeImageLayer entry;
    if (!BKE_paint_layers_slice_describe(
            ma, *reader, channel, entry, nullptr, true, mask_kind))
    {
      return fail("an Anchor Point slice cannot be described");
    }
    const bool clear_to_one = mask_kind && entry.mask_items.is_empty();
    if (gpu_entry_has_adjustment(entry)) {
      return fail("an adjustment row is read as an Anchor Point source");
    }
    if (!clear_to_one) {
      if (const char *reason = BKE_paint_layers_gpu_unsupported_reason(entry, mask_kind)) {
        return fail(reason);
      }
    }
    /* Nested readers are collected from the description before it is moved onto the job: the job
     * vector may reallocate while they are appended. */
    if (!collect_nested_entry(entry, channel, depth + 1, reader_marker)) {
      return false;
    }
    r_jobs[index].clear_to_one = clear_to_one;
    r_jobs[index].entry = std::move(entry);
  }

  /* A reader nested in a slice sits above the point it names, so its own point must be captured
   * first. The rank of the point, not of the reader, orders the passes. */
  std::stable_sort(r_jobs.begin(),
                   r_jobs.end(),
                   [](const PaintLayersGpuCaptureJob &a, const PaintLayersGpuCaptureJob &b) {
                     return a.rank < b.rank;
                   });
  return true;
}

bool BKE_paint_layers_gpu_slot_key_equal(const PaintLayersGpuSlotKey &a,
                                         const PaintLayersGpuSlotKey &b)
{
  /* The channel is the #eMaterialPaintChannel value, never the combined-stack index: Alpha and
   * Emission occupy the same position in one order and none in the other. */
  return a.channel == b.channel && BLI_uuid_equal(a.point, b.point);
}

int BKE_paint_layers_gpu_slot_find(const Span<const PaintLayersGpuSlotKey> slots,
                                   const bUUID &point,
                                   const int channel)
{
  const PaintLayersGpuSlotKey wanted{point, channel};
  for (const int i : slots.index_range()) {
    if (BKE_paint_layers_gpu_slot_key_equal(slots[i], wanted)) {
      return i;
    }
  }
  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Verification
 * \{ */

PaintLayersGpuVerifyDiff BKE_paint_layers_gpu_verify_compare_slot(const float *gpu,
                                                                  const float *cpu,
                                                                  const int64_t sample_count,
                                                                  const int components,
                                                                  const float tolerance)
{
  PaintLayersGpuVerifyDiff result;
  result.ok = true;
  result.max_diff = 0.0f;
  result.samples = 0;
  if (gpu == nullptr || cpu == nullptr || sample_count <= 0 || components <= 0) {
    return result;
  }
  result.samples = sample_count;
  double sample_diff_sum = 0.0;
  for (const int64_t sample : IndexRange(sample_count)) {
    float sample_diff = 0.0f;
    for (const int component : IndexRange(components)) {
      const int64_t index = sample * components + component;
      const float diff = std::abs(gpu[index] - cpu[index]);
      if (!(diff == diff)) {
        /* A NaN compares false with everything, so std::max would drop it: a broken slot must not
         * pass. */
        result.has_nan = true;
        continue;
      }
      sample_diff = std::max(sample_diff, diff);
    }
    result.max_diff = std::max(result.max_diff, sample_diff);
    sample_diff_sum += double(sample_diff);
  }
  /* The GPU slot is sampled bilinearly in screen space and the CPU slice is read at the nearest
   * texel, so at the edge of a stroke a lone sample can differ by a lot while the slot is right.
   * Judge the mean of the per-sample worst component; keep the maximum for the report. */
  result.mean_diff = float(sample_diff_sum / double(sample_count));
  result.ok = !result.has_nan && result.mean_diff <= tolerance;
  return result;
}

PaintLayersGpuSlotGuard::PaintLayersGpuSlotGuard(PaintLayersGpuSlotGuard &&other) noexcept
    : release_(other.release_),
      release_data_(other.release_data_),
      resources_(std::move(other.resources_))
{
  other.release_ = nullptr;
  other.release_data_ = nullptr;
}

PaintLayersGpuSlotGuard &PaintLayersGpuSlotGuard::operator=(
    PaintLayersGpuSlotGuard &&other) noexcept
{
  if (this == &other) {
    return *this;
  }
  release();
  release_ = other.release_;
  release_data_ = other.release_data_;
  resources_ = std::move(other.resources_);
  other.release_ = nullptr;
  other.release_data_ = nullptr;
  return *this;
}

PaintLayersGpuSlotGuard::~PaintLayersGpuSlotGuard()
{
  release();
}

void PaintLayersGpuSlotGuard::set_release(const ReleaseFn fn, void *release_data)
{
  release();
  release_ = fn;
  release_data_ = release_data;
}

void PaintLayersGpuSlotGuard::own(void *resource)
{
  if (resource != nullptr) {
    resources_.append(resource);
  }
}

void PaintLayersGpuSlotGuard::release()
{
  if (release_ != nullptr) {
    for (void *resource : resources_) {
      release_(release_data_, resource);
    }
  }
  resources_.clear();
}

/** \} */

bool BKE_paint_layers_correction_blend_is_normal(const MaterialPaintLayer &effect,
                                                 const int channel)
{
  const CompositeBlend blend = layer_channel_blend(effect, channel);
  /* Normal has no Mix of its own: combining the normals is what its plain blend means. */
  return ELEM(blend, CompositeBlend::Mix, CompositeBlend::NormalCombine);
}

bool BKE_paint_layers_corrections_row_describe(const Material &ma,
                                               const Span<MaterialPaintLayer *> corrections,
                                               const int channel,
                                               const int size,
                                               const ImageUser *iuser,
                                               const int tile_number,
                                               PaintMaterialCompositeImageLayer &r_row)
{
  r_row = PaintMaterialCompositeImageLayer();
  /* The same shape a Paint row kept only for its corrections takes (#composite_row_source_describe):
   * it covers nothing of its own, so the corrections bring the coverage in. */
  r_row.enabled = true;
  r_row.has_constant_color = true;
  r_row.empty_base = true;
  r_row.empty_base_size = size;
  r_row.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
      eMaterialPaintChannel(channel));
  for (const MaterialPaintLayer *effect : corrections) {
    const int64_t before = r_row.content_corrections.size();
    composite_effect_append(ma, *effect, channel, 1.0f, nullptr, false, false, nullptr, r_row);
    if (r_row.content_corrections.size() == before) {
      continue;
    }
    PaintMaterialCompositeCorrection &correction = r_row.content_corrections.last();
    if (correction.image == nullptr) {
      continue;
    }
    /* A map without this tile lays nothing on it. */
    const bool has_tile = (correction.image->source == IMA_SRC_TILED) ?
                              BKE_image_get_tile(correction.image, tile_number) != nullptr :
                              tile_number == 1001;
    if (has_tile) {
      /* A Material correction reads the map its source was rendered into, at the render's own side:
       * unlike a painted map it need not be the canvas's. An identity mapping resamples it to the
       * canvas, where the evaluator would refuse the mismatch. */
      if (paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_READS_SOURCE) &&
          !correction.mapping.enabled)
      {
        ImageUser probe_user;
        BKE_imageuser_default(&probe_user);
        probe_user.tile = tile_number;
        void *lock = nullptr;
        const ImBuf *probe = BKE_image_acquire_ibuf(correction.image, &probe_user, &lock);
        if (probe != nullptr && (probe->x != size || probe->y != size)) {
          correction.mapping.enabled = true;
          correction.mapping.tangent_normal = false;
        }
        BKE_image_release_ibuf(correction.image, const_cast<ImBuf *>(probe), lock);
      }
      correction.iuser = iuser;
    }
    else {
      r_row.content_corrections.remove_last();
    }
  }
  return !r_row.content_corrections.is_empty();
}

}  // namespace blender
