/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See #BKE_paint_layers_composite.hh: the description -> CPU layer stack bridge.
 */

#include "BKE_paint_layers_composite.hh"

#include <cstring>
#include <iterator>

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

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
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
 * Append the rows of \a list that take part in \a channel to \a r_layers, bottom to top. A folder is
 * one entry carrying its children, which the evaluator composites in isolation; a folder nothing of
 * which takes part is left out, as it is on the generator's side.
 */
static void composite_image_layers_collect(const Material &ma,
                                           const ListBaseT<MaterialPaintLayer> &list,
                                           const int channel,
                                           Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  for (const MaterialPaintLayer &layer : list) {
    PaintMaterialCompositeImageLayer out;
    if (BKE_paint_layers_is_folder(layer)) {
      Vector<PaintMaterialCompositeImageLayer> children;
      composite_image_layers_collect(ma, layer.children, channel, children);
      if (children.is_empty()) {
        continue;
      }
      out.is_folder = true;
      out.children.assign(std::make_move_iterator(children.begin()),
                          std::make_move_iterator(children.end()));
    }
    else {
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
      if (!paint_layer_channel_contributes(ma, layer, record)) {
        continue;
      }
      if (record->image != nullptr) {
        out.color_image = record->image;
        /* The description carries no per-node #ImageUser; the default one is what a plain map
         * uses. */
        out.color_iuser = nullptr;
        /* A fresh paint map is transparent where nothing was painted; that texel must show the rows
         * below rather than cover them with the map's black. */
        out.color_alpha_coverage = true;
      }
      else {
        /* A channel with no map: a flat value (it has some alpha, or the row would not take part). */
        copy_v4_v4(out.constant_color, record->value);
        out.has_constant_color = true;
      }
    }
    out.blend = layer_channel_blend(layer, channel);
    out.opacity = BKE_paint_layers_effective_opacity(layer);
    out.enabled = true;
    /* The mask is one for all channels of the row, so the same item reaches every channel's
     * stack, exactly as the generator reads the one set of mask inputs in every channel chain. */
    if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
      if (const MaterialPaintLayerChannel *mask_record = paint_layer_channel_find(
              *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR))
      {
        PaintMaterialCompositeMaskItem item;
        item.opacity = BKE_paint_layers_effective_opacity(*mask);
        item.image = mask_record->image;
        item.iuser = nullptr;
        item.constant_gray = paint_layer_mask_constant(*mask_record);
        out.mask_items.append(item);
      }
    }
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
  composite_image_layers_collect(ma, ma.paint_layers, channel, r_layers);
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

}  // namespace blender
