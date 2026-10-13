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
 * flatten the relief -- whatever the row's own blend is; every other channel mixes.
 */
static CompositeBlend layer_channel_blend(const int channel)
{
  return (channel == PAINT_MATERIAL_CHANNEL_NORMAL) ? CompositeBlend::NormalCombine :
                                                      CompositeBlend::Mix;
}

/**
 * Evaluate image_layers of channel into r_linear as straight scene-linear RGBA, starting
 * from the channel's neutral value, so a partially covered row fades towards what the material
 * shows where nothing covers it rather than towards zero.
 */
static bool composite_channel_linear(const Span<PaintMaterialCompositeImageLayer> image_layers,
                                     const int channel,
                                     const int width,
                                     const int height,
                                     const rcti *region,
                                     Vector<float> &r_linear)
{
  float bottom_color[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
  r_linear.resize(int64_t(width) * height * 4);
  return BKE_paint_material_composite_eval_images_linear(
      image_layers, r_linear.data(), region, nullptr, bottom_color);
}

bool BKE_paint_layers_composite_image_layers(
    const Material &ma, const int channel, Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  r_layers.clear();
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  for (const MaterialPaintLayer &layer : ma.paint_layers) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    if (!paint_layer_channel_live(record)) {
      continue;
    }
    PaintMaterialCompositeImageLayer out;
    if (record->image != nullptr) {
      out.color_image = record->image;
      /* The description carries no per-node #ImageUser; the default one is what a plain map uses. */
      out.color_iuser = nullptr;
      /* A fresh paint map is transparent where nothing was painted; that texel must show the rows
       * below rather than cover them with the map's black. */
      out.color_alpha_coverage = true;
    }
    else {
      /* A channel with no map: a flat value. One with no coverage covers nothing, so a row nobody
       * has painted yet drops out instead of filling the channel with its empty (black) constant. */
      if (record->value[3] <= 0.0f) {
        continue;
      }
      copy_v4_v4(out.constant_color, record->value);
      out.has_constant_color = true;
    }
    out.blend = layer_channel_blend(channel);
    out.opacity = BKE_paint_layers_effective_opacity(layer);
    out.enabled = true;
    r_layers.append(out);
  }
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

  Vector<float> linear;
  if (!composite_channel_linear(image_layers, channel, width, height, region, linear)) {
    return false;
  }

  rcti area;
  if (!BKE_paint_material_composite_area_get(width, height, region, area)) {
    /* Nothing of the tagged region is inside the buffer; the destination is already correct. */
    return true;
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

  /* Encode into the buffer's own colorspace, so the preview and a saved map are the same pixels. */
  const char *colorspace_name = IMB_colormanagement_get_byte_colorspace(&r_dst);
  const ColorSpace *colorspace = (colorspace_name != nullptr) ?
                                     IMB_colormanagement_space_get_named(colorspace_name) :
                                     nullptr;
  const bool encode = colorspace != nullptr && !IMB_colormanagement_space_is_data(colorspace) &&
                      !IMB_colormanagement_space_is_scene_linear(colorspace);

  uchar *pixels = r_dst.byte_data_for_write();
  const int64_t area_width = BLI_rcti_size_x(&area);
  for (int y = area.ymin; y < area.ymax; y++) {
    float *row = linear.data() + (int64_t(y) * width + area.xmin) * 4;
    if (encode) {
      IMB_colormanagement_scene_linear_to_colorspace(row, int(area_width), 1, 4, colorspace);
    }
    uchar *out = pixels + (int64_t(y) * width + area.xmin) * 4;
    for (int64_t i = 0; i < area_width * 4; i++) {
      out[i] = uchar(clamp_i(int(row[i] * 255.0f + 0.5f), 0, 255));
    }
  }

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
    Vector<float> linear;
    ok = composite_channel_linear(image_layers, channel, width, height, region, linear);
    if (ok) {
      float *dst_pixels = ibuf->float_data_for_write();
      rcti area;
      if (!BKE_paint_material_composite_area_get(width, height, region, area)) {
        BLI_rcti_init(&area, 0, 0, 0, 0);
      }
      for (int y = area.ymin; y < area.ymax; y++) {
        for (int x = area.xmin; x < area.xmax; x++) {
          const int64_t i = int64_t(y) * width + x;
          const float alpha = linear[i * 4 + 3];
          dst_pixels[i * 4 + 0] = linear[i * 4 + 0] * alpha;
          dst_pixels[i * 4 + 1] = linear[i * 4 + 1] * alpha;
          dst_pixels[i * 4 + 2] = linear[i * 4 + 2] * alpha;
          dst_pixels[i * 4 + 3] = alpha;
        }
      }
    }
  }
  else {
    ok = BKE_paint_layers_composite_channel(ma, channel, *ibuf, region);
  }
  if (ok) {
    /* The write bypasses the paint path, so nothing else tells the display buffer or the GPU copy
     * of  dst that its pixels changed. */
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
