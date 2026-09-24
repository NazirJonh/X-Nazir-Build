/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers_composite.hh: the description -> CPU layer stack bridge.
 */

#include "BKE_paint_layers_composite.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "paint_layers_intern.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

namespace {

/**
 * Restore the unit length of an encoded tangent-space normal the partial factors shortened.
 *
 * The generated chain ends with the same decode-normalize-encode, so a CPU export and the shader
 * agree; the Normal Map node normalizes for the render, which is why a byte preview of Normal is
 * only meaningful once this runs.
 */
void composite_normal_channel_normalize(ImBuf &ibuf, const rcti *region)
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

/** The CPU blend a description blend stands for, with no channel forcing. */
CompositeBlend blend_from_description(const int8_t blend)
{
  const eMaterialPaintLayerBlend description = eMaterialPaintLayerBlend(blend);
  if (description == MA_PAINT_LAYER_BLEND_NORMAL_COMBINE) {
    return CompositeBlend::NormalCombine;
  }
  /* The one table both the CPU and the generator read; the enum values line up with `MA_RAMP_*`,
   * so the code casts straight across. */
  return CompositeBlend(BKE_paint_layers_blend_to_ramp(description));
}

/**
 * The CPU blend a row's description blend stands for in \a channel.
 *
 * The generator routes every Normal row -- the layer itself and a content correction -- through the
 * Normal Combine group, whatever the row's own blend says, so the CPU has to read the same operation
 * here. Mask corrections are the exception: they only change the factor scalar, which does not
 * depend on the colour channel, so their own blend is kept.
 */
CompositeBlend layer_channel_blend(const MaterialPaintLayer &layer, const int channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return CompositeBlend::NormalCombine;
  }
  return blend_from_description(
      eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(layer, channel)));
}

}  // namespace

namespace {

/**
 * Whether \a layer or anything nested under it takes part in \a channel. A valid bake counts even
 * without a channel record -- a Material layer's channels exist only as its baked maps -- so this
 * matches the generator's `layer_subtree_has_channel`.
 */
bool composite_layer_subtree_has_channel(const Material &material,
                                         const MaterialPaintLayer &layer,
                                         const int channel)
{
  Image *baked = nullptr;
  if (BKE_paint_layers_bake_substitute(material, layer, channel, &baked)) {
    return true;
  }
  /* C-7: a Custom row with a stale saved bake still takes part. */
  if (BKE_paint_layers_bake_substitute_custom(material, layer, channel, &baked, nullptr)) {
    return true;
  }
  /* A deferred Material row with a constant source channel takes part through that constant. */
  float live_value[4];
  if (BKE_paint_layers_material_live_constant(material, layer, channel, live_value)) {
    return true;
  }
  /* Or through its source's own texture, for the same reason: it paints through that map rather
   * than a channel record. Mirrors the generator's `layer_subtree_has_channel`. */
  Image *live_image = nullptr;
  const ImageUser *live_iuser = nullptr;
  if (BKE_paint_layers_material_live_image(material, layer, channel, &live_image, &live_iuser)) {
    return true;
  }
  if (paint_layer_channel_present(layer, channel)) {
    return true;
  }
  if (!BKE_paint_layers_is_folder(layer)) {
    return false;
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    if (composite_layer_subtree_has_channel(material, child, channel)) {
      return true;
    }
  }
  return false;
}

/**
 * Fill \a r_layers from one list of rows, recursing into a folder's children.
 *
 * An isolating folder is emitted as one layer carrying its children, not flattened: the evaluator
 * composites the children in isolation and lays the result over the rest with the folder's own
 * blend, opacity and mask (design §5). A Pass Through folder is instead flattened into \a r_layers
 * (with its visibility scaling its children), matching the generator's inlined chain.
 *
 * \param isolate_pass_through: emit Pass Through folders as isolating ones anyway. The bake of a
 * folder has to render the row itself, and the isolated formula gives the same pixels, so
 * #BKE_paint_layers_bake_render_node asks for this.
 */
bool composite_image_layers_build(const Material &material,
                                  const ListBase &list,
                                  const int channel,
                                  const MaterialPaintLayer *stop_at,
                                  Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                  const bool isolate_pass_through = false)
{
  for (const MaterialPaintLayer &layer_ref :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    const MaterialPaintLayer *layer = &layer_ref;
    /* The row whose "below" is being collected is not part of it, and neither is anything above
     * it: the walk stops exactly at it and reports that it found it. */
    if (stop_at != nullptr && BLI_uuid_equal(layer->marker, stop_at->marker)) {
      return true;
    }
    if (BKE_paint_layers_role(*layer) != PaintLayerRole::Layer) {
      /* A correction row is not composited here; a Custom and a Material layer are, either
       * through their bake below or, without one, by dropping out with no channel records. */
      continue;
    }
    if (layer->source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
      /* v1 draws a geometry map nowhere: the row contributes no pixels on either side. */
      continue;
    }

    /* A baked, current row stands in for its whole subtree: its colour and its coverage were
     * composited with the mask, corrections and opacity already folded in, so the row becomes a
     * plain layer with those two maps and nothing is applied twice. */
    Image *baked_color = nullptr;
    const bool baked = (BKE_paint_layers_bake_substitute(material, *layer, channel, &baked_color) &&
                        layer->bake->coverage != nullptr) ||
                       /* C-7: a Custom row with a stale saved bake still shows it rather than
                        * dropping out while no GPU context can refresh it. */
                       BKE_paint_layers_bake_substitute_custom(
                           material, *layer, channel, &baked_color, nullptr);
    if (baked)
    {
      PaintMaterialCompositeImageLayer out;
      out.tracks_content_alpha =
          BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel)) &&
          layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL;
      out.color_image = baked_color;
      out.color_iuser = nullptr;
      out.mask_image = layer->bake->coverage;
      out.mask_iuser = nullptr;
      out.mask_from_alpha = false;
      out.mask_influence = 1.0f;
      out.blend = layer_channel_blend(*layer, channel);
      /* The coverage map already carries the opacity and the mask. */
      out.opacity = 1.0f;
      out.enabled = true;
      out.is_bare_base = false;
      out.marker = layer->marker;
      r_layers.append(out);
      continue;
    }

    const bool is_folder = BKE_paint_layers_is_folder(*layer);
    /* The active Material row's source constant, when the channel has one; it takes part and paints
     * through that constant rather than through a map. */
    float live_value[4];
    const bool live = BKE_paint_layers_material_live_constant(
        material, *layer, channel, live_value);
    /* The row may instead show its source's own texture. It takes part for the same reason a live
     * constant does: it paints through that map, not through a channel record, so the drop gates
     * below must see it or the row leaves the CPU stack while the generator keeps it. */
    Image *live_map_image = nullptr;
    const ImageUser *live_map_iuser = nullptr;
    const bool live_map = !live && BKE_paint_layers_material_live_image(
                                      material, *layer, channel, &live_map_image, &live_map_iuser);
    if (is_folder) {
      if (!composite_layer_subtree_has_channel(material, *layer, channel)) {
        continue;
      }
    }
    else if (!live && !live_map && !paint_layer_channel_present(*layer, channel)) {
      continue;
    }
    else if (!live && !live_map && paint_layer_channel_image(*layer, channel) == nullptr &&
             !BKE_paint_layers_kind_info(layer->source).uses_fill_color)
    {
      /* Mirrors the generator: a non-Fill row with no map covers nothing unless its flat value is
       * set (a Fill converted to Paint keeps its colour there until the first stroke). */
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer, channel);
      if (record == nullptr || record->value[3] <= 0.0f) {
        continue;
      }
    }
    if (is_folder && !isolate_pass_through &&
        BKE_paint_layers_folder_is_pass_through(material, *layer))
    {
      /* A Pass Through folder is flattened into the parent stack: its children compose exactly as if
       * the folder were absent, which keeps this result equal to the generator's inlined chain. The
       * folder's visibility, whose only effect is a zero factor, scales every child it contributes. */
      const int64_t first = r_layers.size();
      const bool stop_found = composite_image_layers_build(
          material, layer->children, channel, stop_at, r_layers, isolate_pass_through);
      const float scale = ((layer->flag & MA_PAINT_LAYER_ENABLED) != 0) ? 1.0f : 0.0f;
      for (int64_t i = first; i < r_layers.size(); i++) {
        r_layers[i].opacity *= scale;
      }
      if (stop_found) {
        return true;
      }
      continue;
    }
    const bool normal_channel = (channel == PAINT_MATERIAL_CHANNEL_NORMAL);
    Vector<PaintMaterialCompositeCorrection> content_corrections;
    Vector<PaintMaterialCompositeCorrection> mask_corrections;
    /* One correction row -- an effect when \a is_content, a mask item otherwise -- as the CPU
     * expects it. */
    auto append_correction = [&](const MaterialPaintLayer &correction, const bool is_content) {
      if ((correction.flag & MA_PAINT_LAYER_ENABLED) == 0) {
        return;
      }
      if (!ELEM(correction.source, MA_PAINT_LAYER_SOURCE_IMAGE, MA_PAINT_LAYER_SOURCE_CONSTANT)) {
        return;
      }
      const bool fill = BKE_paint_layers_source_type(correction) ==
                        PaintLayerSourceType::Constant;
      /* A constant normal makes no sense, so a content Fill correction is unsupported in the Normal
       * channel; the generator leaves it out too. */
      if (normal_channel && is_content && fill) {
        return;
      }
      PaintMaterialCompositeCorrection out_correction;
      if (fill) {
        float constant[4];
        BKE_paint_layers_correction_constant(correction, eMaterialPaintChannel(channel), constant);
        BKE_paint_layers_constant_to_linear(
            eMaterialPaintChannel(channel), constant, out_correction.constant_color);
        out_correction.has_constant_color = true;
      }
      else {
        Image *correction_image = is_content ?
                                      paint_layer_channel_image(correction, channel) :
                                      paint_layer_mask_correction_image(correction, channel);
        if (correction_image == nullptr) {
          /* Absent in this channel: nothing to composite for it. */
          return;
        }
        out_correction.image = correction_image;
        out_correction.iuser = nullptr;
      }
      /* A content correction changes the colour, so the Normal channel routes it through the
       * combine. A mask item lays its coverage over the factor: MIX replaces the factor with the
       * item's grey, MULTIPLY darkens it by that grey; every other mode reads as MIX. The mask row
       * is one for all channels, so its opacity comes from the row itself rather than the
       * per-channel override the content path uses. */
      out_correction.blend = is_content ?
                                 layer_channel_blend(correction, channel) :
                                 ((correction.blend == MA_PAINT_LAYER_BLEND_MULTIPLY) ?
                                      CompositeBlend::Multiply :
                                      CompositeBlend::Mix);
      out_correction.opacity = is_content ? BKE_paint_layers_channel_opacity_effective(
                                                correction,
                                                channel) :
                                            BKE_paint_layers_effective_opacity(correction);
      out_correction.enabled = true;
      out_correction.row_enabled = true;
      out_correction.marker = correction.marker;
      if (is_content) {
        content_corrections.append(out_correction);
      }
      else {
        mask_corrections.append(out_correction);
      }
    };
    for (const MaterialPaintLayer *correction : BKE_paint_layers_effects(*layer)) {
      append_correction(*correction, true);
    }
    for (const MaterialPaintLayer *correction : BKE_paint_layers_mask_items(*layer)) {
      append_correction(*correction, false);
    }
    PaintMaterialCompositeImageLayer out;
    /* The generated chain tracks a content alpha exactly where this is set: an image-paint
     * channel, and a row that is not Material. A folder tracks it by its channel, like the
     * generator's own gate at #channel_tracks_content_alpha. */
    out.tracks_content_alpha =
        BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(channel)) &&
        layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL;
    /* Set when the row being collected sits inside this folder: the folder is still emitted, but
     * with only the part of its contents that lies below the row, and the walk stops after it. */
    bool stop_found_inside = false;
    if (is_folder) {
      out.is_folder = true;
      Vector<PaintMaterialCompositeImageLayer> child_layers;
      stop_found_inside = composite_image_layers_build(
          material, layer->children, channel, stop_at, child_layers, isolate_pass_through);
      out.children.assign(child_layers.begin(), child_layers.end());
    }
    else {
      if (live) {
        /* The active Material row shows its source's live constant, the same the generator builds;
         * its baked map is ignored while the row is deferred. */
        BKE_paint_layers_constant_to_linear(
            eMaterialPaintChannel(channel), live_value, out.constant_color);
        out.has_constant_color = true;
      }
      else if (live_map) {
        /* The row shows the source's own texture, sampled in UV space like the generated node.
         * A Material row's transparency is the Alpha input, not the channel map's own alpha, so
         * its content coverage stays 1; the map alpha kept its Paint/Fill meaning. */
        out.color_image = live_map_image;
        out.color_iuser = live_map_iuser;
        out.color_alpha_coverage = layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL;
      }
      else {
        Image *image = paint_layer_channel_image(*layer, channel);
        if (image != nullptr) {
          out.color_image = image;
          /* The description carries no per-node #ImageUser; the default one is what a plain map
           * uses. */
          out.color_iuser = nullptr;
          /* A fresh map is transparent where nothing was painted; that texel must show the rows
           * below rather than cover them with the map's black, as the generated chain does. A
           * Material row is the exception: its channel map (a real bake is opaque) never carries
           * the row's coverage, so its content coverage stays 1. */
          out.color_alpha_coverage = layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL;
        }
        else {
          /* A constant row: a Fill's colour, or a channel with no map. */
          float constant[4];
          paint_layer_channel_constant(*layer, channel, constant);
          BKE_paint_layers_constant_to_linear(
              eMaterialPaintChannel(channel), constant, out.constant_color);
          out.has_constant_color = true;
        }
      }
    }
    out.blend = layer_channel_blend(*layer, channel);
    /* The row's effective opacity times the channel's own multiplier and the disabled case; the
     * same helper the generator's per (row, channel) input reads. The mask stack is its own list of
     * corrections below. */
    out.opacity = BKE_paint_layers_channel_opacity_effective(*layer, channel);
    float live_alpha[4];
    Image *live_alpha_image = nullptr;
    const ImageUser *live_alpha_iuser = nullptr;
    if (BKE_paint_layers_material_live_constant(
            material, *layer, PAINT_MATERIAL_CHANNEL_ALPHA, live_alpha))
    {
      /* The source alpha is live too, so the row's coverage is the constant the generator builds
       * rather than the baked map. An alpha that is not constant keeps its last bake. */
      out.coverage_constant = live_alpha[0];
      out.has_coverage_constant = true;
    }
    else if (BKE_paint_layers_material_live_image(
                 material, *layer, PAINT_MATERIAL_CHANNEL_ALPHA, &live_alpha_image, &live_alpha_iuser))
    {
      /* A live source alpha map: read as its alpha, the output the generator uses as the factor. */
      out.coverage_image = live_alpha_image;
      out.coverage_iuser = live_alpha_iuser;
      out.coverage_from_alpha = true;
    }
    else if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer->bake != nullptr &&
             layer->bake->coverage != nullptr)
    {
      /* A Material layer's source transparency, baked; the row's own mask items stay live on top. */
      out.coverage_image = layer->bake->coverage;
    }
    out.enabled = true;
    out.content_corrections = content_corrections;
    out.mask_corrections = mask_corrections;
    /* Not a bare base: the generated chain blends every row over the constant bottom, so the
     * evaluator clears the buffer and blends this row like any other. */
    out.is_bare_base = false;
    out.marker = layer->marker;
    r_layers.append(out);
    if (stop_found_inside) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool BKE_paint_layers_composite_image_layers(
    const Material &ma, const int channel, Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  r_layers.clear();
  composite_image_layers_build(ma, ma.paint_layers, channel, nullptr, r_layers);
  return !r_layers.is_empty();
}

Image *BKE_paint_layers_below_image(Main &bmain,
                                    const Material &ma,
                                    const MaterialPaintLayer &layer,
                                    const int channel,
                                    const int fallback_size)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || fallback_size <= 0) {
    return nullptr;
  }
  Vector<PaintMaterialCompositeImageLayer> below;
  composite_image_layers_build(ma, ma.paint_layers, channel, &layer, below);

  int width = fallback_size;
  int height = fallback_size;
  if (!below.is_empty()) {
    int stack_width = 0;
    int stack_height = 0;
    if (BKE_paint_material_composite_stack_dimensions(below, stack_width, stack_height) &&
        stack_width > 0 && stack_height > 0)
    {
      width = stack_width;
      height = stack_height;
    }
  }

  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
  Vector<float> linear(int64_t(width) * height * 4);
  if (!below.is_empty()) {
    if (!BKE_paint_material_composite_eval_images_linear(
            below, linear.data(), nullptr, nullptr, bottom_color))
    {
      return nullptr;
    }
  }
  else {
    /* Nothing below the row: the chain starts from the channel's own bottom constant, fully
     * covering, exactly as the generated chain does. */
    for (const int64_t i : IndexRange(int64_t(width) * height)) {
      copy_v3_v3(&linear[i * 4], bottom_color);
      linear[i * 4 + 3] = 1.0f;
    }
  }

  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char name[192];
  SNPRINTF(name, "%s %s Below", layer.name[0] != '\0' ? layer.name : "Layer", info.ui_name);
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(&bmain,
                                         width,
                                         height,
                                         name,
                                         32,
                                         true,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         false,
                                         /*is_data=*/!info.is_color,
                                         false);
  if (image == nullptr) {
    return nullptr;
  }
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  STRNCPY(image->colorspace_settings.name,
          IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf == nullptr || ibuf->float_data() == nullptr) {
    BKE_image_release_ibuf(image, ibuf, lock);
    BKE_id_free(&bmain, &image->id);
    return nullptr;
  }
  ibuf->channels = 4;
  float *dst = ibuf->float_data_for_write();
  const int64_t texel_num = int64_t(width) * height;
  memcpy(dst, linear.data(), size_t(texel_num) * 4 * sizeof(float));
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(image, ibuf);
  BKE_image_release_ibuf(image, ibuf, lock);
  BKE_image_partial_update_mark_full_update(image);
  return image;
}

/** The list that directly owns \a target, or null. */
static ListBase *paint_layer_owner_list_for(ListBase *list, const MaterialPaintLayer *target)
{
  for (MaterialPaintLayer &layer : *reinterpret_cast<ListBaseT<MaterialPaintLayer> *>(list)) {
    if (&layer == target) {
      return list;
    }
    if (ListBase *found = paint_layer_owner_list_for(&layer.children, target)) {
      return found;
    }
    if (ListBase *found = paint_layer_owner_list_for(&layer.effects, target)) {
      return found;
    }
    if (ListBase *found = paint_layer_owner_list_for(&layer.mask_stack, target)) {
      return found;
    }
  }
  return nullptr;
}

bool BKE_paint_layers_bake_render_node(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const int channel,
                                       const int size,
                                       float *r_color_rgba,
                                       float *r_coverage_gray,
                                       const int *dst_rect)
{
  if (size <= 0 || r_color_rgba == nullptr || r_coverage_gray == nullptr || channel < 0 ||
      channel >= PAINT_MATERIAL_CHANNEL_NUM)
  {
    return false;
  }
  ListBase *owner = paint_layer_owner_list_for(
      &const_cast<Material &>(ma).paint_layers, &layer);
  if (owner == nullptr) {
    return false;
  }
  Vector<PaintMaterialCompositeImageLayer> entries;
  /* The bake renders the row itself, so a Pass Through folder is emitted as an isolating one here:
   * the isolated formula yields the same pixels as the inlined chain would. */
  composite_image_layers_build(ma, *owner, channel, nullptr, entries, true);
  const PaintMaterialCompositeImageLayer *found = nullptr;
  for (const PaintMaterialCompositeImageLayer &entry : entries) {
    if (BLI_uuid_equal(entry.marker, layer.marker)) {
      found = &entry;
      break;
    }
  }
  if (found == nullptr) {
    return false;
  }
  /* The row's content, straight and with its own factor, from the one shared isolated-group
   * formula -- for a leaf and a folder alike. */
  Vector<PaintMaterialCompositeImageLayer> one;
  one.append(*found);
  int width = 0;
  int height = 0;
  if (!BKE_paint_material_composite_stack_dimensions(one, width, height) || width <= 0 ||
      height <= 0)
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
  BLI_rcti_init(
      &area, max_ii(0, sx0), min_ii(width, sx1), max_ii(0, sy0), min_ii(height, sy1));
  const int area_w = BLI_rcti_size_x(&area);
  const int area_h = BLI_rcti_size_y(&area);
  if (area_w <= 0 || area_h <= 0) {
    return false;
  }
  Vector<float> content_color(int64_t(area_w) * area_h * 4);
  Vector<float> content_coverage(int64_t(area_w) * area_h);
  rcti region = area;
  if (!BKE_paint_material_composite_eval_row_content(
          one, layer.marker, content_color.data(), content_coverage.data(), &region))
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
      /* The content alpha the row export carried travels with the colour, so a substituted row
       * reads its own transparency back instead of turning opaque (F2-C6). */
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
  ListBase *owner = paint_layer_owner_list_for(
      &const_cast<Material &>(ma).paint_layers, &layer);
  if (owner == nullptr) {
    return false;
  }
  Vector<PaintMaterialCompositeImageLayer> entries;
  /* A folder needs its own emitted row to measure; its isolated form has the same content. */
  composite_image_layers_build(ma, *owner, channel, nullptr, entries, true);
  for (const PaintMaterialCompositeImageLayer &entry : entries) {
    if (BLI_uuid_equal(entry.marker, layer.marker)) {
      Vector<PaintMaterialCompositeImageLayer> one;
      one.append(entry);
      return BKE_paint_material_composite_stack_dimensions(one, r_width, r_height);
    }
  }
  return false;
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
  /* The byte result is written in the channel's own colorspace: sRGB for a colour channel, data
   * for a scalar or Normal one. A caller that already set one -- the cache, from the map it was
   * built on -- keeps it. */
  if (r_dst.byte_buffer.colorspace == nullptr) {
    const bool is_color = BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).is_color;
    const int role = is_color ? COLOR_ROLE_DEFAULT_BYTE : COLOR_ROLE_DATA;
    IMB_colormanagement_assign_byte_colorspace(
        &r_dst, IMB_colormanagement_role_colorspace_name_get(role));
  }
  /* Start from the same value the generated chain's bottom constant holds, or a partially covered
   * row would fade towards a different colour than it renders with. */
  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
  if (!BKE_paint_material_composite_eval_images(
          image_layers, &r_dst, region, nullptr, bottom_color))
  {
    return false;
  }
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    composite_normal_channel_normalize(r_dst, region);
  }
  return true;
}

bool BKE_paint_layers_composite_image(Material &ma,
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
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, width, height) ||
      width <= 0 || height <= 0)
  {
    /* A stack with no maps yet -- a single Fill, say -- has no pixel size of its own; the
     * destination image is the canvas the caller asked for, so its size is the natural one. */
    ImageUser iuser_probe;
    BKE_imageuser_default(&iuser_probe);
    void *lock_probe = nullptr;
    ImBuf *probe = BKE_image_acquire_ibuf(&dst, &iuser_probe, &lock_probe);
    if (probe != nullptr) {
      width = probe->x;
      height = probe->y;
    }
    BKE_image_release_ibuf(&dst, probe, lock_probe);
    if (width <= 0 || height <= 0) {
      BKE_report(reports, RPT_ERROR, "The layer stack has no dimensions to composite into");
      return false;
    }
  }

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&dst, &iuser, &lock);
  if (ibuf == nullptr) {
    BKE_report(reports, RPT_ERROR, "The destination image has no buffer");
    return false;
  }
  if (ibuf->x < width || ibuf->y < height) {
    BKE_image_release_ibuf(&dst, ibuf, lock);
    BKE_report(reports,
               RPT_ERROR,
               "The destination image is smaller than the layer stack");
    return false;
  }

  bool ok = false;
  if (ibuf->float_buffer.data != nullptr) {
    /* A float destination takes the linear result directly, premultiplied: that is what a float
     * ImBuf holds, and what the shader's own output compares against. */
    Vector<float> linear(int64_t(width) * height * 4);
    float bottom_color[4];
    BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
    ok = BKE_paint_material_composite_eval_images_linear(
        image_layers, linear.data(), region, nullptr, bottom_color);
    if (ok) {
      ibuf->channels = 4;
      float *dst_pixels = ibuf->float_data_for_write();
      for (int64_t i = 0; i < int64_t(width) * height; i++) {
        const float alpha = linear[i * 4 + 3];
        dst_pixels[i * 4 + 0] = linear[i * 4 + 0] * alpha;
        dst_pixels[i * 4 + 1] = linear[i * 4 + 1] * alpha;
        dst_pixels[i * 4 + 2] = linear[i * 4 + 2] * alpha;
        dst_pixels[i * 4 + 3] = alpha;
      }
    }
  }
  else {
    ok = BKE_paint_layers_composite_channel(ma, channel, *ibuf, region);
  }
  BKE_image_release_ibuf(&dst, ibuf, lock);

  if (!ok) {
    BKE_report(reports, RPT_ERROR, "Failed to composite the channel");
  }
  return ok;
}

}  // namespace blender
