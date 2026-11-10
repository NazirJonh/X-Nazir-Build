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
#include <fmt/format.h>
#include <string>
#include <unordered_map>
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
#include "BLI_task.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_generator_registry.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_report.hh"

#include "paint_layers_composite_intern.hh"
#include "paint_layers_effect_cache_intern.hh"
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
  if (!BKE_paint_layers_mapping_remaps(ma, row)) {
    return;
  }
  out.enabled = true;
  copy_v2_v2(out.offset, row.mapping.offset);
  copy_v2_v2(out.scale, row.mapping.scale);
  out.rotation = row.mapping.rotation;
  out.wrap = BKE_paint_layers_mapping_wrap_get(row);
  out.mirror_axes = BKE_paint_layers_mapping_mirror_get(row);
  BKE_paint_layers_mapping_mirror_center_get(row, out.mirror_center);
  out.tangent_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
}

/** The parameters of \a row's kind in the schema order the evaluator reads them in. */
template<size_t N> static void composite_filter_params(const MaterialPaintLayer &row, float (&r_params)[N])
{
  if (paint_layer_kind_has_color_balance_param(row)) {
    /* The balance is not a scalar schema: its payload folds into the 12 numbers the composite, the
     * graph and the GPU share. A caller whose array is shorter would truncate parameters, so
     * compilation fails if N is too small. */
    static_assert(N >= PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM,
                  "filter_params array is too short for the Color Balance fold");
    paint_layer_color_balance_fold(paint_layer_color_balance_or_neutral(row), r_params);
    return;
  }
  if (paint_layer_kind_has_channel_mixer_param(row)) {
    /* The matrix folds into the same 12-float layout, the monochrome switch included, so a null
     * payload reads as the neutral identity matrix. No payload pointer travels with the filter: the
     * composite, the graph and the GPU read these numbers alone. */
    if constexpr (N >= PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM) {
      static const MaterialPaintLayerChannelMixer neutral = {};
      paint_layer_channel_mixer_fold(
          row.channel_mixer != nullptr ? *row.channel_mixer : neutral,
          paint_layer_kind_param_bool_get(row, PAINT_LAYER_CHANNEL_MIXER_MONOCHROME),
          r_params);
    }
    return;
  }
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(row)) {
    for (const int i : IndexRange(std::min(kind->params_num, int(N)))) {
      r_params[i] = paint_layer_kind_param_get(row, i);
    }
  }
}

/**
 * Describe \a row's generator in \a channel: the line and ramp of a Gradient, the pattern, the
 * placement and the colour band of the others, the channel's scalar ends and, for an
 * Object/Generated coordinate, the surface that gives the texel its position. The one place a
 * generator row, a generator correction and a generator mask item are described, so the evaluator
 * cannot read them differently.
 */
static bool composite_generator_describe(
    const Material &ma,
    const MaterialPaintLayer &row,
    const int channel,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    PaintMaterialCompositeGradient &out)
{
  const int kind = int(row.kind_subtype);
  const bool is_gradient = kind == MA_PAINT_LAYER_GENERATOR_GRADIENT;
  /* The registry knows the kind before its CPU factor lands: a row whose generator has no table
   * entry yet lays nothing, the same answer the build's dispatch gives it, rather than reading as
   * the Gradient its default subtype names. */
  if (BKE_paint_layers_generator_of(row) == nullptr) {
    return false;
  }
  if (is_gradient && row.gradient == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  out.enabled = true;
  /* The subtype rides with the payload, so the composite's sample dispatches through the
   * generator table the way the generated graph does. A generator the schema does not know is
   * refused by the kind lookup before its row is described. */
  out.subtype = int16_t(kind);
  out.color_channel = info.is_color;
  out.params = {};
  if (is_gradient) {
    /* The Gradient keeps its whole description in its payload; it has no schema. */
    out.payload = dna::shallow_copy(*row.gradient);
    out.space = int16_t(row.gradient->space);
  }
  else {
    out.space = int16_t(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_SPACE));
    if (const PaintLayerKindInfo *schema = paint_layer_kind_of(row)) {
      /* The Ramp-typed slot reads as zero: the band itself comes from the row below. */
      for (const int i : IndexRange(std::min(schema->params_num, int(out.params.size())))) {
        out.params[i] = paint_layer_kind_param_get(row, i);
      }
    }
    if (const ColorBand *band = BKE_paint_layers_ramp_get(row); band != nullptr) {
      out.color_ramp = *band;
      out.color_ramp_valid = true;
    }
  }
  const int size = (ma.paint_layers_image_size > 0) ? int(ma.paint_layers_image_size) : 4096;
  out.width = size;
  out.height = size;
  if (out.space == MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
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
    const bool coordinate_usable = out.space != MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED ||
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
    /* A generator authors its two ends as relief (for Height, -1..1), while the composite works in
     * storage. The value ramp path needs no conversion: its grey stops are the normalized
     * parameter of the channel's range, which for Height is the stored value itself. */
    out.scalar_start = BKE_paint_material_channel_value_encode(eMaterialPaintChannel(channel),
                                                               record->value[0]);
    out.scalar_end = BKE_paint_material_channel_value_encode(eMaterialPaintChannel(channel),
                                                             record->value[1]);
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
    /* A NEEDS_BAKE kind computes itself against the row's own bake through the effect cache, which
     * an adjustment has no path to: a corrupted row of one is left out, not silently described with
     * the parameters the cache never sees -- those carry no colour. */
    BLI_assert(!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_NEEDS_BAKE));
    if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_NEEDS_BAKE)) {
      return false;
    }
    out.adjustment_filter = layer.kind_subtype;
    composite_filter_params(layer, out.adjustment_params);
    out.adjustment_ramp = BKE_paint_layers_ramp_get(layer);
    out.adjustment_curves = BKE_paint_layers_curves_get(layer);
    return true;
  }
  if (!paint_layer_row_participates(ma, layer, channel)) {
    return false;
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES)) {
    /* A generator: no map and no constant, only its pattern, colour ramp and the channel's own
     * value ramp. */
    return composite_generator_describe(ma, layer, channel, context_surface, out.gradient);
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
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* The material's shared atlas, already in the stack's UV space: no Mapping, and its alpha is
     * data rather than painted coverage. */
    Image *atlas = paint_layer_mesh_map_image(ma, layer);
    if (atlas == nullptr) {
      return false;
    }
    out.color_image = atlas;
    out.color_iuser = nullptr;
    out.color_alpha_coverage = false;
    out.is_mesh_map = true;
    out.is_mesh_map_scalar = paint_layer_mesh_map_is_scalar(layer.mesh_map_type);
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
 *
 * \param hidden_with_base: the base's switch carries the whole stack -- with it off, no item of the
 * stack lays and the mask reads as one, the same mix-out the graph's gate applies over the chain
 * (see #PaintLayersTreeBuilder::mask_factor_ensure). The items themselves are left untouched, so
 * switching the base back restores every correction at once. A straight slice read passes false: a
 * tap publishes the stack as it stands, independent of the row's visibility.
 */
static void composite_mask_items_describe(
    const Material &ma,
    const MaterialPaintLayer &layer,
    Vector<PaintMaterialCompositeMaskItem> &r_items,
    const bUUID *stop_before = nullptr,
    const int *slice_rect = nullptr,
    const bool dimensions_only = false,
    const bool describe_only = false,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface = nullptr,
    const bool hidden_with_base = true)
{
  const MaterialPaintLayer *base = BKE_paint_layers_mask_base(layer);
  if (base == nullptr) {
    return;
  }
  if (hidden_with_base && (base->flag & MA_PAINT_LAYER_ENABLED) == 0) {
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
        BKE_paint_layers_anchor_point_slice_images(ma, mask, item.grey_input_images);
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
          item.grey_buffer = (ref.target == PaintLayerAnchorPointTarget::Mask) ?
                                        std::move(slice_color) :
                                        std::move(slice_coverage);
        }
        item.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, mask, read_channel);
        BKE_paint_layers_anchor_point_slice_images(ma, mask, item.grey_input_images);
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
      /* A filter changes the coverage so far; it owns no map or grey and no record. A CPU-computed
       * kind lays its own grey instead: the mask effect cache computes the mask built up to the
       * item with the kernel over it, and the item reads like a painted map with its invert, blend
       * and opacity. In describe-only the grey is left out and the kind named, so the combined
       * preview refuses the stack instead of shading the item as a no-op. */
      if (paint_layer_kind_has_flag(mask, PAINT_LAYER_KIND_NEEDS_BAKE)) {
        PaintMaterialCompositeMaskItem effect;
        effect.opacity = BKE_paint_layers_effective_opacity(mask);
        effect.blend = CompositeBlend(
            BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
        effect.invert = paint_layer_mask_item_inverted(mask);
        effect.effect_filter = mask.kind_subtype;
        /* A fresh bake stands in, the way a substituted row's does: its map is read like a painted
         * item's, the item's own mapping over it. Without one the grey is computed through the mask
         * effect cache from the mask built up to the item. */
        if (Image *bake_map = paint_layer_mask_effect_bake_map(ma, mask)) {
          effect.image = bake_map;
          composite_mapping_describe(ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, effect.mapping);
        }
        else {
          effect.grey_buffer = paint_layer_mask_effect_buffer_get(
              ma, layer, mask, describe_only, &effect.grey_input_images);
          if (!describe_only && effect.grey_buffer == nullptr) {
            /* The input could not be rendered: the item is left out, as the generator leaves a
             * filter it cannot express out of the live graph. */
            continue;
          }
        }
        r_items.append(effect);
        continue;
      }
      PaintMaterialCompositeMaskItem filter;
      filter.opacity = BKE_paint_layers_effective_opacity(mask);
      filter.filter = mask.kind_subtype;
      composite_filter_params(mask, filter.filter_params);
      filter.filter_ramp = BKE_paint_layers_ramp_get(mask);
      filter.filter_curves = BKE_paint_layers_curves_get(mask);
      r_items.append(filter);
      continue;
    }
    if (paint_layer_kind_has_flag(mask, PAINT_LAYER_KIND_GENERATES)) {
      /* A generator mask item: the ramp's colour is the grey and its alpha the coverage, read from
       * the item's own pattern rather than from a map. */
      PaintMaterialCompositeMaskItem item;
      item.opacity = BKE_paint_layers_effective_opacity(mask);
      if (!composite_generator_describe(
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
    if (mask.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
      /* The atlas' R is the grey; it is read with Extend, no Mapping, and its alpha is data. */
      if (!paint_layer_mask_item_lays(ma, mask)) {
        continue;
      }
      Image *atlas = paint_layer_mesh_map_image(ma, mask);
      if (atlas == nullptr) {
        continue;
      }
      PaintMaterialCompositeMaskItem item;
      item.opacity = BKE_paint_layers_effective_opacity(mask);
      item.image = atlas;
      item.is_mesh_map = true;
      item.is_mesh_map_scalar = paint_layer_mesh_map_is_scalar(mask.mesh_map_type);
      item.blend = CompositeBlend(
          BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(mask.blend)));
      item.invert = paint_layer_mask_item_inverted(mask);
      r_items.append(item);
      continue;
    }
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
 * The mask of \a row, rendered once per build pass when \a cache says it is shared.
 *
 * A null mask is cached as well: "this row cannot be compared" is as stable an answer as a
 * rendered one, and re-deriving it for every channel would pay the description walk each time.
 */
static std::shared_ptr<ImBuf> composite_height_won_get(
    const Material &ma,
    const ListBaseT<MaterialPaintLayer> &prefix,
    const MaterialPaintLayer &row,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    const bool isolating,
    PaintLayersHeightWonCache *cache)
{
  if (cache == nullptr) {
    return composite_height_won_render(ma, prefix, row, context_surface, isolating, nullptr);
  }
  const std::pair<const MaterialPaintLayer *, bool> key(&row, isolating);
  if (const std::shared_ptr<ImBuf> *found = cache->masks.lookup_ptr(key)) {
    return *found;
  }
  std::shared_ptr<ImBuf> mask = composite_height_won_render(
      ma, prefix, row, context_surface, isolating, cache);
  cache->masks.add_new(key, mask);
  return mask;
}

static bool anchor_slice_dimensions(const Material &ma,
                                    const MaterialPaintLayer &reader,
                                    const int channel,
                                    const PaintLayerAnchorPointRef &ref,
                                    int &r_width,
                                    int &r_height);

/** The effect cache, defined beside the slice cache it shares the machinery of; the hash is
 * declared with the key in #paint_layers_effect_cache_intern.hh. */
static std::shared_ptr<ImBuf> paint_layer_effect_buffer_get(const Material &ma,
                                                            const MaterialPaintLayer &row,
                                                            const MaterialPaintLayer &effect,
                                                            const int channel,
                                                            const bool describe_only,
                                                            Vector<Image *> *r_images);

/**
 * Append the content correction \a effect to \a out as the evaluator reads it in \a channel: a filter,
 * or a map, flat colour, source or slice laid by the correction's own blend and opacity. Nothing is
 * appended for a correction that lays nothing here, as the generator builds nothing for it.
 */
static void composite_effect_append(
    const Material &ma,
    const MaterialPaintLayer *owner_row,
    const MaterialPaintLayer &effect,
    const int channel,
    const float pass_through_scale,
    const int *slice_rect,
    const bool dimensions_only,
    const bool describe_only,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &context_surface,
    PaintMaterialCompositeImageLayer &out)
{
  if (paint_layer_effect_is_cpu_computed(effect)) {
    /* A CPU-computed effect reads the row's own content, so it needs the row it hangs on: the
     * standalone corrections-row description has none and leaves it out. */
    if (owner_row == nullptr || !paint_layer_effect_filters(ma, effect, channel)) {
      return;
    }
    /* A CPU-computed effect carries its result as a map the effect cache computed: the kind and
     * the parameters travel like a filter's, the buffer beside them. In describe_only only the
     * kind is known -- the buffer is not computed, and the combined preview falls back to the CPU
     * when it sees the kind. */
    PaintMaterialCompositeCorrection effect_correction;
    effect_correction.filter = effect.kind_subtype;
    composite_filter_params(effect, effect_correction.filter_params);
    effect_correction.effect_hash = paint_layer_effect_hash(effect, channel);
    /* The GPU path keys its result texture on the effect instance, which a parameter hash cannot do
     * across rows: name the marker the correction was made from. */
    effect_correction.effect_marker = effect.marker;
    effect_correction.effect_color = paint_layer_effect_buffer_get(
        ma, *owner_row, effect, channel, describe_only, &effect_correction.effect_images);
    if (!describe_only && effect_correction.effect_color == nullptr) {
      /* The input could not be rendered: the correction is left out, as the generator leaves it
       * out of the live graph. */
      return;
    }
    effect_correction.opacity = BKE_paint_layers_channel_opacity_effective(effect, channel) *
                                pass_through_scale;
    out.content_corrections.append(effect_correction);
    return;
  }
  if (paint_layer_effect_filters(ma, effect, channel)) {
    PaintMaterialCompositeCorrection filter;
    filter.filter = effect.kind_subtype;
    composite_filter_params(effect, filter.filter_params);
    filter.filter_ramp = BKE_paint_layers_ramp_get(effect);
    filter.filter_curves = BKE_paint_layers_curves_get(effect);
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
                                         &correction.grey_buffer,
                                         &correction.anchor_point_coverage,
                                         slice_rect,
                                         dimensions_only))
      {
        return;
      }
      correction.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, effect, channel);
      BKE_paint_layers_anchor_point_slice_images(ma, effect, correction.grey_input_images);
    }
  }
  else if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_GENERATES)) {
    /* A generator correction has no map: its ramp is laid by its own blend and opacity. */
    if (!composite_generator_describe(ma, effect, channel, context_surface, correction.gradient)) {
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
    if (effect.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
      /* A mesh-map correction reads the material's shared atlas; no Mapping, alpha is data. */
      Image *atlas = paint_layer_mesh_map_image(ma, effect);
      if (atlas == nullptr) {
        return;
      }
      correction.image = atlas;
      correction.is_mesh_map = true;
      correction.is_mesh_map_scalar = paint_layer_mesh_map_is_scalar(effect.mesh_map_type);
    }
    else if (effect_record->image != nullptr) {
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

bool composite_image_layer_build(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 const int channel,
                                 const float pass_through_scale,
                                 const bool isolating,
                                 const bool ignore_own_bake,
                                 PaintMaterialCompositeImageLayer &out,
                                 const bUUID *stop_before,
                                 const int *slice_rect,
                                 const bool dimensions_only,
                                 const bool describe_only,
                                 const std::shared_ptr<const PaintMaterialCompositeSurface>
                                     &context_surface,
                                 PaintLayersHeightWonCache *height_won_cache,
                                 const PaintLayerBakeScope scope)
{
  /* A reader lays the slice its point publishes, described as a substituted row: the slice's
   * colour, its coverage and its content alpha, with the reader's own blend. Its own mapping does
   * not remap the slice. */
  bool reader_live = false;
  /* The mode the description runs in: which of it a content bake leaves out. One mode where two
   * flags would have to be kept exclusive by hand. */
  enum class ContentMode {
    /** A live row, or a reader over its live slice: the source, the corrections, the mask and the
     * opacity are all described. */
    Live,
    /** The bake render of a content bake: its mask and its opacity are laid live over the map, so
     * neither is folded into these pixels. */
    RenderContentBake,
    /** The description of a row standing in over its content bake: the source and the corrections
     * are inside the bake, so neither is described again. */
    SubstituteContentBake,
  };
  ContentMode content_mode = scope == PaintLayerBakeScope::ContentOnly ?
                                 ContentMode::RenderContentBake :
                                 ContentMode::Live;
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
      composite_height_settings_describe(layer, channel, out);
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
    if (dimensions_only) {
      /* Sizing only: the buffers stay unrendered, but the slice's side is known and is what the
       * bake of the reader is rendered at. Carried the way an empty-base row's canvas is, so the
       * stack's dimensions find the reader without rendering a single nested slice. */
      int slice_width = 0;
      int slice_height = 0;
      if (anchor_slice_dimensions(ma, layer, channel, reader_ref, slice_width, slice_height)) {
        out.empty_base = true;
        out.empty_base_size = slice_width;
      }
    }
    /* Describe-only keeps the reader even where #BKE_paint_layers_slice_render would refuse it for
     * a zero-sized source. The GPU plan captures the slot and the screen-space pass needs no source
     * size; a caller that later falls back to the CPU rebuilds the full description, which refuses
     * it there. */
    out.anchor_point_hash = paint_layer_anchor_point_slice_hash(ma, layer, channel);
    BKE_paint_layers_anchor_point_slice_images(ma, layer, out.grey_input_images);
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
    composite_height_settings_describe(layer, channel, out);
    /* The coverage map holds what the content covers rather than the mask times the opacity; the
     * map keeps its place as the pass's mask, since for a row whose colour carries its own alpha
     * the quotient the bake stored is one wherever the content reaches, so nothing is counted
     * twice, and a row that carries none is covered by the map alone. */
    if (paint_layer_bake_scope(layer, false) == PaintLayerBakeScope::ContentOnly) {
      content_mode = ContentMode::SubstituteContentBake;
    }
    else {
      /* The coverage map already holds the row's opacity and mask; only visibility stays live, the
       * same 0/1 the generated graph multiplies in. */
      out.opacity = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) *
                    pass_through_scale;
      out.enabled = true;
      out.tracks_content_alpha = BKE_paint_material_channel_tracks_content_alpha(
          eMaterialPaintChannel(channel));
      return true;
    }
  }
  /* A live reader's slice is described above; every other row describes its own source. */
  if (!reader_live && content_mode != ContentMode::SubstituteContentBake) {
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
                                     describe_only,
                                     height_won_cache);
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
  composite_height_settings_describe(layer, channel, out);
  /* The content bake holds the row's content alone, so its own opacity -- the visibility with it --
   * is not folded in: the substitution lays it live over the map. What the row's source gives to the
   * transparency stays, being part of the content itself. */
  const float live_opacity = BKE_paint_layers_channel_opacity_effective(layer, channel) *
                             pass_through_scale;
  out.opacity = content_mode == ContentMode::RenderContentBake ? 1.0f : live_opacity;
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
   * lays nothing in this channel is not carried, as the generator builds nothing for it. A row
   * substituted by its content bake has its corrections inside that bake already, and a content
   * bake renders them, being the content the cache holds. */
  if (content_mode != ContentMode::SubstituteContentBake) {
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (stop_before != nullptr && BLI_uuid_equal(effect.marker, *stop_before)) {
        break;
      }
      composite_effect_append(ma,
                              &layer,
                              effect,
                              channel,
                              pass_through_scale,
                              slice_rect,
                              dimensions_only,
                              describe_only,
                              context_surface,
                              out);
    }
  }
  /* The mask is one for all channels of the row, so the same item reaches every channel's stack,
   * exactly as the generator reads the one set of mask inputs in every channel chain. A content
   * bake renders none of it: the mask stays live over the map, where its own mapping is read as
   * well. */
  if (content_mode != ContentMode::RenderContentBake) {
    composite_mask_items_describe(ma,
                                 layer,
                                 out.mask_items,
                                 stop_before,
                                 slice_rect,
                                 dimensions_only,
                                 describe_only,
                                 context_surface);
  }
  /* Mark the positions the GPU has to capture: a marker is a producer only while at least one
   * reader lays its slice. A content marker sits in the effects, a mask marker in the mask stack. */
  if (content_mode != ContentMode::SubstituteContentBake) {
    for (const MaterialPaintLayer &effect : layer.effects) {
      if (paint_layer_kind_is_anchor_point_marker(effect) &&
          paint_layer_anchor_point_is_read(ma, effect.marker))
      {
        out.anchor_point_content_producers.append(effect.marker);
      }
    }
  }
  if (content_mode != ContentMode::RenderContentBake) {
    for (const MaterialPaintLayer &item : layer.mask_stack) {
      if (paint_layer_kind_is_anchor_point_marker(item) &&
          paint_layer_anchor_point_is_read(ma, item.marker))
      {
        out.anchor_point_mask_producers.append(item.marker);
      }
    }
  }
  return true;
}

void composite_image_layers_collect(const Material &ma,
                                           const ListBaseT<MaterialPaintLayer> &list,
                                           const int channel,
                                           const float pass_through_scale,
                                           const std::shared_ptr<const PaintMaterialCompositeSurface>
                                               &context_surface,
                                           Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                           const bool isolating,
                                           const int *slice_rect,
                                           const bool dimensions_only,
                                    const bool describe_only,
                                    PaintLayersHeightWonCache *height_won_cache)
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
                                     describe_only,
                                     height_won_cache);
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
                                   context_surface,
                                   height_won_cache))
    {
      /* Where the row's Height record Compares, its win or loss gates every other channel of the
       * row; the mask is rendered once here, from the Height description of the rows below it in
       * this very list (pass-through folders flattened, isolation boundaries respected by the
       * recursion that got here). The Height channel itself never reads it back. */
      if (channel != PAINT_MATERIAL_CHANNEL_HEIGHT && !dimensions_only && !describe_only &&
          out.height_compare)
      {
        ListBaseT<MaterialPaintLayer> prefix;
        prefix.first = (list.first == &layer) ? nullptr : list.first;
        prefix.last = layer.prev;
        out.height_won = composite_height_won_get(
            ma, prefix, layer, context_surface, isolating, height_won_cache);
      }
      r_layers.append(std::move(out));
    }
  }
}

bool BKE_paint_layers_composite_image_layers(
    const Material &ma,
    const int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    const bool describe_only,
    std::shared_ptr<const PaintMaterialCompositeSurface> surface,
    PaintLayersHeightWonCache *height_won_cache)
{
  r_layers.clear();
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  composite_image_layers_collect(ma,
                                 ma.paint_layers,
                                 channel,
                                 1.0f,
                                 surface,
                                 r_layers,
                                 false,
                                 nullptr,
                                 false,
                                 describe_only,
                                 height_won_cache);
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
                                               const bUUID *stop_before,
                                               const bool hidden_with_base)
{
  composite_mask_items_describe(ma, layer, items_, stop_before, nullptr, false, false, nullptr,
                                hidden_with_base);
  /* The evaluator's own form of the description, so #BKE_paint_material_mask_factor_eval reads the
   * same opacity/blend/invert/filter/coverage fields the composite reads. */
  buffers_.resize(items_.size());
  for (const int i : items_.index_range()) {
    const PaintMaterialCompositeMaskItem &item = items_[i];
    PaintMaterialCompositeMaskBuffer &buffer = buffers_[i];
    /* A MESH_MAP atlas' alpha is data: the shared mask arithmetic (#BKE_paint_material_mask_factor_eval)
     * weights the item by its opacity alone and reads the scalar atlas' R, like the composite does. */
    buffer.is_mesh_map = item.is_mesh_map;
    buffer.is_mesh_map_scalar = item.is_mesh_map_scalar;
    buffer.constant_gray = item.constant_gray;
    buffer.opacity = item.opacity;
    buffer.blend = item.blend;
    buffer.invert = item.invert;
    buffer.filter = item.filter;
    buffer.filter_ramp = item.filter_ramp;
    buffer.filter_curves = item.filter_curves;
    std::copy(std::begin(item.filter_params),
              std::end(item.filter_params),
              std::begin(buffer.filter_params));
    buffer.map_coverage = item.map_coverage;
    buffer.coverage_tolerance = item.coverage_tolerance;
    buffer.coverage_softness = item.coverage_softness;
    buffer.coverage_output = item.coverage_output;
    buffer.coverage_colors = item.coverage_colors;
    buffer.gradient = item.gradient;
    if (buffer.gradient.enabled && buffer.gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV)
    {
      /* An Object/Generated coordinate needs the object's surface to turn a texel into the position
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
    if (item.grey_buffer != nullptr) {
      /* A slice is already in canvas space and the description keeps its buffer alive: take an
       * ImBuf reference so the tile's destructor can release it like any other acquired map. A
       * slice of another size than the tile is not sampled, as for a painted map below. */
      if (item.grey_buffer->x == ref_width && item.grey_buffer->y == ref_height) {
        IMB_refImBuf(item.grey_buffer.get());
        tile.items[i] = item.grey_buffer.get();
        tile.item_samples[i] = item.grey_buffer->float_data();
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

/**
 * A map a slice was last made from, and the version of its pixels at that time. The same pair the
 * GPU effect cache keys its staleness on (see #PaintLayerEffectSource), so one snapshot serves both.
 */
using AnchorSliceSource = PaintLayerEffectSource;

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
     * first item. It has no opacity, blend or mask of its own, and reads the stack straight: the
     * base's switch hides the mask from the row, not from a reader of the slice. */
    composite_mask_items_describe(ma,
                                  *ref.source_row,
                                  r_entry.mask_items,
                                  &ref.point->marker,
                                  slice_rect,
                                  dimensions_only,
                                  /*describe_only=*/false,
                                  /*context_surface=*/nullptr,
                                  /*hidden_with_base=*/false);
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
    /* The slice reads the stack straight, the base's switch included (see
     * #BKE_paint_layers_slice_describe). */
    PaintLayersMaskSampler sampler(ma, *ref.source_row, &ref.point->marker, false);
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

/* -------------------------------------------------------------------- */
/** \name Paint Layer Effect Cache
 *
 * A CPU-computed effect (a Blur, a Noise) cannot be built as nodes, and its result is not a
 * pointwise function of one texel: the whole input map is read. The cache holds the result, one
 * float buffer per (material, row, channel, parameters), with the input rendered through the same
 * description the bake renders -- the row's content up to the effect, before its mask and opacity
 * -- and the kernel applied over it. Freshness follows the slice cache: the entry remembers the
 * version of every map the input was rendered from, and any edit of one recomputes the whole
 * buffer (a neighbourhood reads past an edited rectangle, so a region update would have to carry a
 * halo; a recompute is the simple correct thing until that is wanted).
 * \{ */

/** One cached effect: the result buffer, the maps the input was rendered from, the size and the
 * byte cost, keyed by #EffectCacheKey. */
struct EffectCacheEntry {
  EffectCacheKey key;
  int width = 0;
  int height = 0;
  std::shared_ptr<ImBuf> color;
  /** The maps the input was rendered from, with the versions they had. */
  Vector<AnchorSliceSource> synced;
  size_t bytes = 0;
};

/**
 * Total memory the effect cache may hold: a 4K RGBA float buffer is 256 MiB per effect, and this
 * cache stands beside the slice cache (4 GiB) and the composite cache, whose working sets overlap
 * on the same strokes. One GiB holds a couple of recent effects at 4K and the small ones at lower
 * sizes; an entry evicted between two evaluations is computed whole again, and an entry larger than
 * the budget is never cached.
 */
constexpr size_t PAINT_LAYER_EFFECT_CACHE_BYTES = size_t(1024) * 1024 * 1024;

/** The live budget; only tests override it, to make eviction observable without 4K fixtures. */
static size_t g_paint_effect_cache_budget = PAINT_LAYER_EFFECT_CACHE_BYTES;

/** The cache owns references only; dropping an entry simply releases them. The release callable of
 * the shared store is a no-op here: the buffer's last reference dies with the entry. */
static PaintLayerEffectCacheStore<EffectCacheEntry> g_paint_effect_cache;
/**
 * Guards the container and the byte count only. A bake job computes effects on a worker thread
 * while the main thread describes the composite, and both reach the cache. It is never held while
 * an effect computes, so the input's own nested renders cannot deadlock on it.
 */
static std::mutex g_paint_effect_cache_mutex;

/**
 * A description hash of the effect: the kind and the parameters in its schema order. The input's
 * pixels are not part of it -- the sources' versions stand in for those -- so an edit of a value
 * moves it and an edit of a map does not.
 */
uint64_t paint_layer_effect_hash(const MaterialPaintLayer &effect, const int channel)
{
  uint64_t hash = 1469598103934665603ull;
  const auto mix_u32 = [&](const uint32_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto mix_float = [&](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    mix_u32(bits);
  };
  mix_u32(uint32_t(effect.kind_subtype));
  mix_u32(uint32_t(channel));
  float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  composite_filter_params(effect, params);
  for (const float param : params) {
    mix_float(param);
  }
  /* A Color parameter reads as zero in the scalar fold above, so its channels join the hash here;
   * otherwise a Glow's or a Color Match's colour edit would keep the cached result. */
  if (const PaintLayerKindInfo *kind = paint_layer_kind_of(effect)) {
    for (const int i : IndexRange(kind->params_num)) {
      if (kind->params[i].type == PaintLayerKindParamType::Color) {
        float color[4];
        paint_layer_kind_param_color_get(effect, i, color);
        for (const float component : color) {
          mix_float(component);
        }
      }
    }
  }
  return hash;
}

bool operator==(const PaintLayerEffectGpuKeyExtras &a, const PaintLayerEffectGpuKeyExtras &b)
{
  return a.chain_hash == b.chain_hash && a.input_scalar_hash == b.input_scalar_hash;
}

bool paint_correction_is_effect(const PaintMaterialCompositeCorrection &correction)
{
  return !BLI_uuid_is_nil(correction.effect_marker);
}

bool BKE_paint_layers_effect_input_describe(const Material &ma,
                                            const MaterialPaintLayer &row,
                                            const MaterialPaintLayer &effect,
                                            const int channel,
                                            const bool dimensions_only,
                                            const bool describe_only,
                                            PaintMaterialCompositeImageLayer &r_entry)
{
  r_entry = PaintMaterialCompositeImageLayer{};
  if (!composite_image_layer_build(ma,
                                   row,
                                   channel,
                                   1.0f,
                                   false,
                                   true,
                                   r_entry,
                                   &effect.marker,
                                   nullptr,
                                   dimensions_only,
                                   describe_only,
                                   nullptr,
                                   nullptr))
  {
    return false;
  }
  /* The input is the row's content before its opacity, mask, object mask and blend: only what the
   * corrections below the effect left. */
  r_entry.mask_items.clear();
  r_entry.mask_image = nullptr;
  r_entry.mask_iuser = nullptr;
  r_entry.mask_from_alpha = false;
  r_entry.mask_influence = 0.0f;
  r_entry.opacity = 1.0f;
  r_entry.enabled = true;
  return true;
}

void BKE_paint_layers_effect_input_images(const PaintMaterialCompositeImageLayer &layer,
                                          Vector<Image *> &r_images)
{
  const auto add = [&](Image *image) {
    if (image != nullptr && image->runtime != nullptr && !r_images.contains(image)) {
      r_images.append(image);
    }
  };
  add(layer.color_image);
  add(layer.mask_image);
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    add(correction.image);
    add(correction.anchor_point_coverage_image);
    for (Image *image : correction.grey_input_images) {
      add(image);
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    add(item.image);
    add(item.coverage_image);
    for (Image *image : item.grey_input_images) {
      add(image);
    }
  }
  for (const PaintMaterialCompositeImageLayer &child : layer.children) {
    BKE_paint_layers_effect_input_images(child, r_images);
  }
}

/** Render the input of \a effect and apply its kernel, into a buffer of the input's own size. */
static bool paint_layer_effect_compute(const Material &ma,
                                       const MaterialPaintLayer &row,
                                       const MaterialPaintLayer &effect,
                                       const int channel,
                                       std::shared_ptr<ImBuf> &r_color,
                                       Vector<AnchorSliceSource> &r_synced)
{
  /* Timed in two halves because the GPU plan replaces them differently: the kernel is what an
   * effect pass computes instead, while the input render is what the gather lays with its own
   * passes. The whole recompute is measured too, so a phase's win is this line stopping to print.
   * The region is always the full buffer: see the note above the cache this feeds. */
  bke::paint_perf::ScopedTimer perf_total("effect.cpu.total");
  PaintMaterialCompositeImageLayer entry;
  std::shared_ptr<ImBuf> color;
  int width = 0;
  int height = 0;
  {
    bke::paint_perf::ScopedTimer perf_input("effect.cpu.input");
    if (!BKE_paint_layers_effect_input_describe(
            ma, row, effect, channel, /*dimensions_only=*/false, /*describe_only=*/false, entry))
    {
      return false;
    }
    if (!BKE_paint_material_composite_stack_dimensions(
            Span<PaintMaterialCompositeImageLayer>(&entry, 1), width, height) ||
        width <= 0 || height <= 0)
    {
      return false;
    }
    color = anchor_slice_buffer_new(width, height);
    if (color == nullptr) {
      return false;
    }
    Vector<float> coverage(int64_t(width) * height);
    if (!BKE_paint_material_composite_eval_row_content(
            entry, color->float_data_for_write(), coverage.data(), nullptr, channel))
    {
      return false;
    }
    perf_input.note("kind=%d size=%dx%d region=full", int(effect.kind_subtype), width, height);
  }
  /* The kernel reads the buffer the block above filled and leaves the alpha alone; the scope ends
   * before it, so the two halves of the recompute are not timed together. */
  paint_layer_effect_kernel_apply(*color, effect);
  /* The versions are taken after the render: an edit that landed while it ran is read next time. */
  Vector<Image *> images;
  BKE_paint_layers_effect_input_images(entry, images);
  r_synced = map_sources_snapshot(images);
  r_color = std::move(color);
  perf_total.note("kind=%d size=%dx%d region=full", int(effect.kind_subtype), width, height);
  return true;
}

/** Drop every cached effect buffer, releasing the canvases (a static would outlive the leak check). */
static void paint_layer_effect_cache_clear()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  const auto no_release = [](EffectCacheEntry &) {};
  g_paint_effect_cache.free_all(no_release);
}

void BKE_paint_layers_effect_cache_free_all()
{
  paint_layer_effect_cache_clear();
}

void BKE_paint_layers_effect_cache_free_material(const Material &ma)
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  const auto no_release = [](EffectCacheEntry &) {};
  g_paint_effect_cache.free_material(ma.id.session_uid, no_release);
}

size_t paint_layer_effect_cache_bytes()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  return g_paint_effect_cache.total_bytes;
}

void paint_layer_effect_cache_set_budget(const size_t bytes)
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  g_paint_effect_cache_budget = bytes;
}

size_t paint_layer_effect_cache_budget()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  return g_paint_effect_cache_budget;
}

int paint_layer_effect_cache_entry_count()
{
  std::lock_guard<std::mutex> lock(g_paint_effect_cache_mutex);
  return int(g_paint_effect_cache.entries.size());
}

/**
 * The result of the CPU-computed \a effect on \a row's content in \a channel, as a float RGBA
 * buffer at the input's size, scene linear, the input's content alpha in the fourth component.
 * Cached; the kernel and the input's maps decide when it is computed again.
 *
 * \param describe_only: learn nothing but the maps the input reads (into \a r_images) -- the
 *        combined preview asks for this, since it falls back to the CPU for these kinds anyway.
 * \param r_images: the maps the input was rendered from; the composite cache subscribes to them,
 *        so a stroke on any of them reaches it even though the effect reads no Image of its own.
 * \return null when the input could not be rendered or \a describe_only was asked.
 */
static std::shared_ptr<ImBuf> paint_layer_effect_buffer_get(const Material &ma,
                                                            const MaterialPaintLayer &row,
                                                            const MaterialPaintLayer &effect,
                                                            const int channel,
                                                            const bool describe_only,
                                                            Vector<Image *> *r_images)
{
  /* The maps the input reads and its size, described without rendering anything: the maps go to
   * the composite cache's subscriptions, the size decides hits. */
  PaintMaterialCompositeImageLayer dims_entry;
  Vector<Image *> images;
  int width = 0;
  int height = 0;
  if (!BKE_paint_layers_effect_input_describe(
          ma, row, effect, channel, true, false, dims_entry) ||
      !BKE_paint_material_composite_stack_dimensions(
          Span<PaintMaterialCompositeImageLayer>(&dims_entry, 1), width, height) ||
      width <= 0 || height <= 0)
  {
    return {};
  }
  BKE_paint_layers_effect_input_images(dims_entry, images);
  if (r_images != nullptr) {
    *r_images = images;
  }
  if (describe_only) {
    return {};
  }

  const EffectCacheKey key{ma.id.session_uid,
                           effect.marker,
                           channel,
                           paint_layer_effect_hash(effect, channel)};
  return paint_layer_effect_cache_get_or_compute(
      g_paint_effect_cache,
      g_paint_effect_cache_mutex,
      key,
      width,
      height,
      images,
      g_paint_effect_cache_budget,
      [&](std::shared_ptr<ImBuf> &r_color, Vector<PaintLayerEffectSource> &r_synced) {
        return paint_layer_effect_compute(ma, row, effect, channel, r_color, r_synced);
      });
}

/** \} */

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
  Vector<AnchorSliceSource> sources_now = map_sources_snapshot(images);

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
      change = map_sources_changes(g_anchor_slice_cache.entries[i].synced, images, region);
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
      color = paint_layer_anchor_slice_buffer_clone(*color);
    }
    if (coverage.use_count() > 1) {
      coverage = paint_layer_anchor_slice_buffer_clone(*coverage);
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
 *
 * \a scope is what the bake holds (#PaintLayerBakeScope): a content bake renders the row's content
 * alone, its own opacity and its mask left out to be laid live over the map.
 */
static bool composite_bake_entry(
    const Material &ma,
    const MaterialPaintLayer &layer,
    const int channel,
    PaintMaterialCompositeImageLayer &r_entry,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &surface = nullptr,
    const int *slice_rect = nullptr,
    const bool dimensions_only = false,
    const PaintLayerBakeScope scope = PaintLayerBakeScope::WholeRow)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      (BKE_paint_layers_is_folder(layer) && BKE_paint_layers_folder_is_pass_through(layer)))
  {
    return false;
  }
  /* The surface is a row input of the description, not a stop point: it rides the context
   * arguments after the size ones. */
  return composite_image_layer_build(ma,
                                    layer,
                                    channel,
                                    1.0f,
                                    false,
                                    true,
                                    r_entry,
                                    nullptr,
                                    slice_rect,
                                    dimensions_only,
                                    false,
                                    surface,
                                    nullptr,
                                    scope);
}

bool BKE_paint_layers_bake_render_node(
    const Material &ma,
    const MaterialPaintLayer &layer,
    const int channel,
    const int size,
    float *r_color_rgba,
    float *r_coverage_gray,
    const int *dst_rect,
    const std::shared_ptr<const PaintMaterialCompositeSurface> &surface)
{
  if (size <= 0 || r_color_rgba == nullptr || r_coverage_gray == nullptr) {
    return false;
  }
  /* A subtree whose generators the bake cannot evaluate is never a cache. */
  if (!BKE_paint_layers_generator_subtree_is_bakeable(layer)) {
    return false;
  }
  /* An Object or Generated pattern reads the geometry of the object it is shaded on, which only
   * the surface of that object carries: without one the texel's own UV would be the approximate
   * preview, not a bake, so the render is refused rather than written as a wrong picture. A reader
   * is exempt: its cache is the slice of the row it points at, rendered by the slice machinery. */
  if (surface == nullptr && !BKE_paint_layers_generator_subtree_is_shared_bakeable(layer) &&
      !paint_layer_kind_reads_anchor_point(layer))
  {
    return false;
  }
  /* Learn the stack dimensions without rendering or allocating a single nested slice. A row
   * carrying a CPU-computed effect is rendered as its content alone: its mask and its opacity are
   * laid live over the map the result becomes, so a stroke into the mask never asks for this. */
  const PaintLayerBakeScope scope = paint_layer_bake_scope(layer, false);
  PaintMaterialCompositeImageLayer dims_entry;
  if (!composite_bake_entry(ma, layer, channel, dims_entry, surface, nullptr, true, scope))
  {
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
  if (!composite_bake_entry(ma,
                            layer,
                            channel,
                            entry,
                            surface,
                            dst_rect != nullptr ? slice_rect : nullptr,
                            false,
                            scope))
  {
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
       * back instead of turning opaque. Over a content bake the coverage is that same quantity, the
       * mask and the opacity having been left out: what the quotient below keeps is the texel's
       * coverage by the content alone. */
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
  if (!composite_bake_entry(ma, layer, channel, entry, nullptr, nullptr, true)) {
    return false;
  }
  return BKE_paint_material_composite_stack_dimensions(
      Span<PaintMaterialCompositeImageLayer>(&entry, 1), r_width, r_height);
}

/**
 * The fallback reason a CPU-computed effect gives: its kind's name, with the article a name in
 * English takes. The text is built from the registry rather than listing every effect a second
 * time, and cached so the returned pointer stays valid like a literal.
 */
static const char *paint_layer_cpu_computed_reason(const PaintLayerKindInfo &kind)
{
  /* One string per kind, so two reasons held at once never share storage; the map's nodes keep
   * their address while it grows. */
  static thread_local std::unordered_map<const PaintLayerKindInfo *, std::string> reasons;
  auto it = reasons.find(&kind);
  if (it == reasons.end()) {
    const bool vowel = ELEM(kind.name[0], 'A', 'E', 'I', 'O', 'U');
    it = reasons
             .emplace(&kind,
                      fmt::format("{} {} correction is read", vowel ? "an" : "a", kind.name))
             .first;
  }
  return it->second.c_str();
}

/**
 * Whether the combined preview shader has no branch for the pointwise \a filter: the registry's
 * #PAINT_LAYER_KIND_NO_GPU_BRANCH flag says so, so a new kind is marked where it is described and
 * cannot be silently shaded as a no-op here.
 */
static bool gpu_filter_lacks_branch(const int16_t filter)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER, filter);
  return kind != nullptr && (kind->flags & PAINT_LAYER_KIND_NO_GPU_BRANCH) != 0;
}

bool BKE_paint_layers_effect_is_gpu_computed(const MaterialPaintLayer &effect)
{
  return paint_layer_effect_is_gpu_computed(effect);
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
  /* The height modes are a delta-space step no shader branch reproduces, and a Compare row's win
   * or loss gates every channel of the row through a rendered mask. The description carries the
   * effective mode, so an Inherit row over Add, Subtract or a non-Mix colour blend is refused
   * here too. Until the preview shader grows both, a material using either falls back to the CPU
   * composite instead of shading a different layer than the one the bake and the Image Editor
   * show. */
  if (layer.height_blend != MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT || layer.height_compare) {
    return "a row blends Height with a mode of its own";
  }
  if (!layer.is_folder && layer.adjustment_filter == 0 && layer.color_image == nullptr &&
      layer.anchor_point_color == nullptr && !layer.has_constant_color && !reader &&
      !mask_only_slice)
  {
    return "a row has neither a map, a flat colour nor an Anchor Point slice";
  }
  /* The combined pass knows the pointwise filters it can express in its shader, and reads a Color
   * Ramp or RGB Curves from a table the gather bakes into a texture. What still has no shader
   * branch is a Gradient, a baked Anchor Point correction's coverage map, and the row shapes
   * refused above; those fall back to the CPU rather than shading something different. */
  /* Corrections and mask items first, because a Gradient one is not merely unsupported by a pass
   * that would otherwise lay its map: there is no map, so the gather's own "a correction that
   * changes something" check does not even count it and the row would be shaded without it. The
   * refusal has to come from here for the CPU preview to take over. Gated on the opacity that
   * decides the CPU lays it too, so a zero-opacity Gradient element does not cost the GPU path. */
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.opacity > 0.0f && correction.gradient.enabled) {
      return "a Gradient correction is not laid by the GPU path";
    }
  }
  for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
    /* A CPU-computed mask effect's grey is computed on the CPU: no pass lays it yet, so the stack
     * falls back rather than shading the item as a no-op. The field names the kind, so a new one
     * needs no edit here. */
    if (item.opacity > 0.0f && item.effect_filter != 0) {
      const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                             item.effect_filter);
      if (kind != nullptr) {
        return paint_layer_cpu_computed_reason(*kind);
      }
    }
    if (item.opacity > 0.0f && item.gradient.enabled) {
      return "a Gradient mask item is not laid by the GPU path";
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    /* A sliced correction reads a captured slot. A baked one has no slice buffer but covers by a
     * coverage map the GPU correction pass does not read. */
    if (correction.anchor_point_coverage_image != nullptr) {
      return "a baked Anchor Point correction is read (no coverage map on the GPU)";
    }
    /* A CPU-computed effect has no pointwise shader branch: its result is a map the effect cache
     * computes on the CPU, so the whole stack falls back rather than shading without it. The
     * registry's #PAINT_LAYER_KIND_NEEDS_BAKE flag decides, so a new one needs no edit here -- except
     * for a kind the preview now computes on the GPU (#PAINT_LAYER_KIND_GPU_COMPUTED): the gather lays
     * an effect pass into the GPU result cache and reads the texture as a correction's map, which is
     * the same picture the CPU cache holds. Gated on the opacity that decides the CPU lays it too. */
    if (correction.opacity > 0.0f && paint_layer_filter_is_cpu_computed(correction.filter) &&
        !paint_layer_filter_is_gpu_computed(correction.filter))
    {
      const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                             correction.filter);
      if (kind != nullptr) {
        return paint_layer_cpu_computed_reason(*kind);
      }
    }
    if (correction.opacity > 0.0f && gpu_filter_lacks_branch(correction.filter)) {
      return "a correction filter the preview shader has no branch for is read";
    }
  }
  /* An adjustment row has no per-kind refusal above, so a filter without a shader branch would
   * otherwise be shaded as a silent no-op. */
  if (gpu_filter_lacks_branch(layer.adjustment_filter)) {
    return "an adjustment filter the preview shader has no branch for is read";
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
    const Span<Span<const PaintMaterialCompositeImageLayer>> stacks,
    const Span<int> channels,
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

int BKE_paint_layers_gpu_slot_find(const Span<PaintLayersGpuSlotKey> slots,
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
                                               PaintMaterialCompositeImageLayer &r_row,
                                               const std::shared_ptr<const PaintMaterialCompositeSurface>
                                                   &context_surface)
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
    composite_effect_append(
        ma, nullptr, *effect, channel, 1.0f, nullptr, false, false, context_surface, r_row);
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
