/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the wand / quick-select feature source; see #paint_image_select_source.hh.
 */

#include "paint_image_select_source.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "ED_uvedit.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "bmesh.hh"

#include "paint_image_select_intern.hh"
#include "paint_image_uv_geom.hh"

namespace blender::ed::sculpt_paint::image_select {

/* -------------------------------------------------------------------- */
/** \name Color space helpers
 * \{ */

/** Perceptual OkLab of an sRGB color; L in 0..1, a/b scaled by 2 so channel-wise tolerances
 * stay comparable to the RGB metrics. */
static float3 rgb_to_oklab(const float3 &srgb)
{
  const float r = srgb_to_linearrgb(srgb.x);
  const float g = srgb_to_linearrgb(srgb.y);
  const float b = srgb_to_linearrgb(srgb.z);

  const float l = std::cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
  const float m = std::cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
  const float s = std::cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

  const float L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  const float A = (1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s) * 2.0f;
  const float B = (0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s) * 2.0f;
  return float3(L, A, B);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Feature building
 * \{ */

SourceMetric metric_resolve(const SourceMetric metric, const bool is_normal)
{
  if (metric == SourceMetric::Auto) {
    return is_normal ? SourceMetric::NormalAngle : SourceMetric::MaxChannel;
  }
  return metric;
}

/**
 * Pixel access of one tile buffer, resolved once: which buffer is read, how many channels it has
 * and how its values map to display-like sRGB.
 */
struct PixelReader {
  const float *float_data = nullptr;
  const uint8_t *byte_data = nullptr;
  int channels = 4;
  /** Non-color image: values are compared raw. */
  bool is_data = false;
  /** Byte image in a color space other than sRGB (needs the color-managed conversion). */
  bool byte_needs_conversion = false;
  const ColorSpace *byte_colorspace = nullptr;

  explicit PixelReader(const ImBuf &ibuf)
  {
    this->is_data = ibuf.colorspace_is_data();
    if (ibuf.float_data()) {
      this->float_data = ibuf.float_data();
      this->channels = (ibuf.channels == 0) ? 4 : ibuf.channels;
    }
    else {
      this->byte_data = ibuf.byte_data();
      this->channels = 4;
      this->byte_colorspace = ibuf.byte_buffer.colorspace;
      this->byte_needs_conversion = !this->is_data && this->byte_colorspace &&
                                    !IMB_colormanagement_space_is_srgb(this->byte_colorspace);
    }
  }
};

/**
 * One pixel as display-like sRGB 0..1 (the space the user judges colors in). Float buffers are
 * scene linear and convert; byte buffers hold their values as authored, converted only when
 * their color space is not sRGB. Data (non-color) images are returned raw.
 */
static float3 pixel_display_rgb(const PixelReader &reader, const int64_t pixel)
{
  if (reader.float_data) {
    const float *src = &reader.float_data[pixel * reader.channels];
    float3 rgb = (reader.channels >= 3) ? float3(src[0], src[1], src[2]) : float3(src[0]);
    if (!reader.is_data) {
      IMB_colormanagement_scene_linear_to_srgb_v3(rgb, rgb);
    }
    return rgb;
  }
  float4 rgba;
  rgba_uchar_to_float(rgba, &reader.byte_data[pixel * 4]);
  float3 rgb(rgba.x, rgba.y, rgba.z);
  if (reader.byte_needs_conversion) {
    IMB_colormanagement_colorspace_to_scene_linear_v3(rgb, reader.byte_colorspace);
    IMB_colormanagement_scene_linear_to_srgb_v3(rgb, rgb);
  }
  return rgb;
}

static float pixel_alpha(const PixelReader &reader, const int64_t pixel)
{
  if (reader.float_data) {
    if (reader.channels == 2) {
      return reader.float_data[pixel * 2 + 1];
    }
    return (reader.channels >= 4) ? reader.float_data[pixel * reader.channels + 3] : 1.0f;
  }
  return reader.byte_data[pixel * 4 + 3] / 255.0f;
}

static void rgb_to_hsv_clamped(const float3 &rgb, float3 &r_hsv)
{
  float rgb_arr[3] = {math::clamp(rgb.x, 0.0f, 1.0f),
                      math::clamp(rgb.y, 0.0f, 1.0f),
                      math::clamp(rgb.z, 0.0f, 1.0f)};
  float hsv_arr[3];
  rgb_to_hsv(rgb_arr[0], rgb_arr[1], rgb_arr[2], &hsv_arr[0], &hsv_arr[1], &hsv_arr[2]);
  r_hsv = float3(hsv_arr[0], hsv_arr[1], hsv_arr[2]);
}

FeatureImage feature_image_build(Image *image,
                                 const ImageUser *owner_iuser,
                                 const int tile_number,
                                 const SourceComponent component,
                                 const SourceMetric metric,
                                 const bool is_normal,
                                 const bool normalize_range)
{
  FeatureImage feature;
  feature.tile_number = tile_number;
  feature.is_normal = is_normal;
  feature.metric = metric_resolve(metric, is_normal);

  ImageUser iuser;
  if (owner_iuser) {
    iuser = *owner_iuser;
  }
  iuser.tile = tile_number;

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf || ibuf->x <= 0 || ibuf->y <= 0) {
    BKE_image_release_ibuf(image, ibuf, lock);
    return feature;
  }
  const int64_t pixel_num = int64_t(ibuf->x) * ibuf->y;
  const PixelReader reader(*ibuf);

  /* Dimensions and per-pixel extraction. */
  switch (component) {
    case SourceComponent::RGB:
    case SourceComponent::RGBA:
      feature.dims = (component == SourceComponent::RGBA) ? 4 : 3;
      break;
    case SourceComponent::Luminance:
    case SourceComponent::Hue:
    case SourceComponent::Saturation:
    case SourceComponent::Value:
    case SourceComponent::Red:
    case SourceComponent::Green:
    case SourceComponent::Blue:
    case SourceComponent::Alpha:
      feature.dims = 1;
      break;
  }
  if (is_normal) {
    feature.dims = 3;
  }
  /* An OkLab metric on a color feature converts the whole image once, here. */
  const bool to_oklab = (feature.metric == SourceMetric::OkLab) && !is_normal &&
                        ELEM(component, SourceComponent::RGB, SourceComponent::RGBA);
  if (to_oklab) {
    feature.dims = 3;
  }

  feature.size = int2(ibuf->x, ibuf->y);
  feature.data.reinitialize(pixel_num * feature.dims);
  feature.circular = (component == SourceComponent::Hue) && !is_normal;

  threading::parallel_for(IndexRange(pixel_num), 4096, [&](const IndexRange range) {
    for (const int64_t pixel : range) {
      float *out = &feature.data[pixel * feature.dims];
      if (is_normal) {
        /* Decode OpenGL-style tangent-space normal and normalize. */
        const float3 raw = pixel_display_rgb(reader, pixel);
        float3 normal = raw * 2.0f - 1.0f;
        const float len = math::length(normal);
        if (len > 1e-6f) {
          normal /= len;
        }
        else {
          normal = float3(0.0f, 0.0f, 1.0f);
        }
        out[0] = normal.x;
        out[1] = normal.y;
        out[2] = normal.z;
        continue;
      }
      if (to_oklab) {
        const float3 lab = rgb_to_oklab(pixel_display_rgb(reader, pixel));
        out[0] = lab.x;
        out[1] = lab.y;
        out[2] = lab.z;
        continue;
      }
      switch (component) {
        case SourceComponent::RGBA: {
          const float3 rgb = pixel_display_rgb(reader, pixel);
          out[0] = rgb.x;
          out[1] = rgb.y;
          out[2] = rgb.z;
          out[3] = pixel_alpha(reader, pixel);
          break;
        }
        case SourceComponent::RGB: {
          const float3 rgb = pixel_display_rgb(reader, pixel);
          out[0] = rgb.x;
          out[1] = rgb.y;
          out[2] = rgb.z;
          break;
        }
        case SourceComponent::Luminance: {
          /* NOTE: the values are display sRGB here, while the color-management luma coefficients
           * are defined on scene-linear values — an accepted approximation, the tools only need a
           * perceptually even grayscale. */
          const float3 rgb = pixel_display_rgb(reader, pixel);
          out[0] = IMB_colormanagement_get_luminance(rgb);
          break;
        }
        case SourceComponent::Hue:
        case SourceComponent::Saturation:
        case SourceComponent::Value: {
          float3 hsv;
          rgb_to_hsv_clamped(pixel_display_rgb(reader, pixel), hsv);
          out[0] = (component == SourceComponent::Hue) ?
                       hsv.x :
                       ((component == SourceComponent::Saturation) ? hsv.y : hsv.z);
          break;
        }
        case SourceComponent::Red:
        case SourceComponent::Green:
        case SourceComponent::Blue: {
          const float3 rgb = pixel_display_rgb(reader, pixel);
          out[0] = (component == SourceComponent::Red) ?
                       rgb.x :
                       ((component == SourceComponent::Green) ? rgb.y : rgb.z);
          break;
        }
        case SourceComponent::Alpha:
          out[0] = pixel_alpha(reader, pixel);
          break;
      }
    }
  });
  BKE_image_release_ibuf(image, ibuf, lock);

  /* Normalize_range: stretch a scalar feature over the tile's min..max. */
  if (normalize_range && feature.dims == 1 && !is_normal) {
    float min_value = FLT_MAX, max_value = -FLT_MAX;
    for (const float value : feature.data) {
      min_value = std::min(min_value, value);
      max_value = std::max(max_value, value);
    }
    const float range = max_value - min_value;
    if (range > 1e-9f) {
      for (float &value : feature.data) {
        value = (value - min_value) / range;
      }
    }
  }

  return feature;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling and distance
 * \{ */

void feature_sample_window(const FeatureImage &feature,
                           const int2 center,
                           const int sample_size,
                           MutableSpan<float> r_sample)
{
  BLI_assert(r_sample.size() >= feature.dims);
  for (float &value : r_sample.take_front(feature.dims)) {
    value = 0.0f;
  }
  if (feature.is_empty()) {
    return;
  }
  const int radius = std::max(0, sample_size / 2);
  const int x0 = std::clamp(center.x - radius, 0, feature.size.x - 1);
  const int x1 = std::clamp(center.x + radius, 0, feature.size.x - 1);
  const int y0 = std::clamp(center.y - radius, 0, feature.size.y - 1);
  const int y1 = std::clamp(center.y + radius, 0, feature.size.y - 1);

  /* Hue: circular mean via sin/cos accumulation; normals: vector mean + normalize. */
  float sin_sum = 0.0f, cos_sum = 0.0f;
  float3 vec_sum(0.0f);
  int count = 0;

  for (const int y : IndexRange(y0, y1 - y0 + 1)) {
    for (const int x : IndexRange(x0, x1 - x0 + 1)) {
      const float *p = feature.pixel(x, y);
      if (feature.circular) {
        sin_sum += std::sin(p[0] * float(2.0 * M_PI));
        cos_sum += std::cos(p[0] * float(2.0 * M_PI));
      }
      else if (feature.is_normal) {
        vec_sum += float3(p[0], p[1], p[2]);
      }
      else {
        for (const int c : IndexRange(feature.dims)) {
          r_sample[c] += p[c];
        }
      }
      count++;
    }
  }
  if (count == 0) {
    return;
  }

  if (feature.circular) {
    const float angle = std::atan2(sin_sum / count, cos_sum / count);
    r_sample[0] = (angle < 0.0f) ? angle / float(2.0 * M_PI) + 1.0f : angle / float(2.0 * M_PI);
    return;
  }
  if (feature.is_normal) {
    float3 mean = vec_sum / float(count);
    const float len = math::length(mean);
    if (len > 1e-6f) {
      mean /= len;
    }
    else {
      mean = float3(0.0f, 0.0f, 1.0f);
    }
    r_sample[0] = mean.x;
    r_sample[1] = mean.y;
    r_sample[2] = mean.z;
    return;
  }
  for (const int c : IndexRange(feature.dims)) {
    r_sample[c] /= float(count);
  }
}

float feature_distance(const FeatureImage &feature, const float *a, const float *b)
{
  if (feature.is_normal) {
    const float3 va(a[0], a[1], a[2]);
    const float3 vb(b[0], b[1], b[2]);
    const float dot = math::clamp(math::dot(va, vb), -1.0f, 1.0f);
    return std::acos(dot) / float(M_PI);
  }
  if (feature.circular) {
    const float d = std::abs(a[0] - b[0]);
    return std::min(d, 1.0f - d) * 2.0f;
  }
  switch (feature.metric) {
    case SourceMetric::OkLab:
    case SourceMetric::Euclidean: {
      float sum = 0.0f;
      for (const int c : IndexRange(feature.dims)) {
        const float d = a[c] - b[c];
        sum += d * d;
      }
      return std::sqrt(sum) / std::sqrt(float(feature.dims));
    }
    case SourceMetric::MaxChannel:
    case SourceMetric::Auto:
    case SourceMetric::NormalAngle:
    default: {
      float max_d = 0.0f;
      for (const int c : IndexRange(feature.dims)) {
        max_d = std::max(max_d, std::abs(a[c] - b[c]));
      }
      return max_d;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source resolution
 * \{ */

SelectSource select_source_resolve(const SpaceImage *sima,
                                   const Scene *scene,
                                   Object *ob,
                                   const float2 &click_uv)
{
  SelectSource source;

  /* Same test as #image_paint_selection_targets_get (the canvas source, not
   * #ImagePaintSettings::mode). */
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  if (!ob || scene->toolsettings->paint_mode.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL) {
    /* Canvas: the active image. */
    source.image = sima->image;
    if (source.image) {
      source.iuser = sima->iuser;
    }
  }
  else {
    /* PBR: the selected channel. */
    const int channel = int(imapaint.select_source_channel);
    if (imapaint.select_source_channel == IMAGE_PAINT_SELECT_SOURCE_ACTIVE_PASS) {
      /* The image shown in this editor (a channel image, or any external image opened in it). */
      source.image = sima->image;
      if (source.image) {
        source.iuser = sima->iuser;
      }
      source.is_normal = (int(sima->material_paint_pass) == PAINT_MATERIAL_CHANNEL_NORMAL);
    }
    else {
      source.is_normal = (channel == PAINT_MATERIAL_CHANNEL_NORMAL);
      Image *image = nullptr;
      ImageUser *iuser = nullptr;
      if (!BKE_paint_principled_channel_image_get(
              *ob, eMaterialPaintChannel(channel), &image, &iuser))
      {
        source.failed = true;
        return source;
      }
      source.image = image;
      if (iuser) {
        source.iuser = *iuser;
      }
    }
  }

  if (!source.image) {
    return source;
  }

  /* The UDIM tile under the click, resolved on the source image (not on some other image).
   * Non-tiled images have the single tile 1001 and #BKE_image_get_tile_from_pos reports 0 for
   * them. */
  float tile_uv[2], tile_ofs[2];
  const int tile_number = BKE_image_get_tile_from_pos(
      source.image, float2(click_uv), tile_uv, tile_ofs);
  if (tile_number == 0) {
    if (source.image->source == IMA_SRC_TILED) {
      source.outside_tiles = true;
    }
    else {
      source.tile_number = 1001;
    }
    return source;
  }
  source.tile_number = tile_number;
  return source;
}

bool select_source_resolve_or_report(bContext *C,
                                     ReportList *reports,
                                     const float2 &click_uv,
                                     SelectSource &r_source)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  const Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  r_source = select_source_resolve(sima, scene, ob, click_uv);
  if (r_source.outside_tiles) {
    BKE_report(reports, RPT_WARNING, "Click is outside the image tiles");
    return false;
  }
  if (r_source.failed || !r_source.image) {
    BKE_report(reports,
               RPT_WARNING,
               r_source.failed ? "The selected source has no image to sample" :
                                 "No image to sample");
    return false;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV-island map building
 * \{ */

UVIslandMap uv_island_map_build(const bContext *C,
                                const Image *image,
                                const int tile_number,
                                const int width,
                                const int height,
                                const int uv_margin_px)
{
  UVIslandMap map;
  if (width <= 0 || height <= 0) {
    return map;
  }

  Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return map;
  }
  Vector<Object *> objects = image_paint_selection_canvas_objects_get(
      C, image, ImagePaintCanvasPurpose::Mask);
  if (objects.is_empty()) {
    return map;
  }

  map.size = int2(width, height);
  map.tile_number = tile_number;
  map.island.reinitialize(int64_t(width) * height);
  map.island.fill(-1);
  map.overlap.reinitialize(int64_t(width) * height);
  map.overlap.fill(0);

  const float2 uv_origin = image_select_udim_tile_uv_origin(tile_number);

  /* Overlapping islands keep the first id and set the overlap flag on the pixels they re-claim.
   * Ids are unique across objects. */
  int32_t island_num = 0;
  bool any_uv = false;
  for (Object *ob : objects) {
    BMesh *bm = nullptr;
    bool owns_bm = false;
    if (ob->mode & OB_MODE_EDIT) {
      BMEditMesh *em = BKE_editmesh_from_object(ob);
      if (!em) {
        continue;
      }
      bm = em->bm;
    }
    else {
      const Mesh *mesh = id_cast<const Mesh *>(ob->data);
      if (!mesh) {
        continue;
      }
      const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
      BMeshCreateParams create_params{};
      BMeshFromMeshParams convert_params{};
      bm = BM_mesh_create(&allocsize, &create_params);
      BM_mesh_bm_from_me(bm, mesh, &convert_params);
      owns_bm = true;
    }

    const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm, ob, scene);
    if (offsets.uv < 0) {
      if (owns_bm) {
        BM_mesh_free(bm);
      }
      continue;
    }
    any_uv = true;

    /* All islands of the mesh in one pass (UV connectivity, hidden faces skipped). */
    ListBaseT<FaceIsland> island_list = {nullptr};
    bm_mesh_calc_uv_islands(scene, bm, &island_list, false, false, false, 1.0f, offsets);

    for (FaceIsland &island : island_list.items_mutable()) {
      const int32_t island_id = island_num++;
      for (BMFace *efa : Span<BMFace *>(island.faces, island.faces_len)) {
        foreach_face_pixel(efa,
                           offsets,
                           uv_origin,
                           width,
                           height,
                           [&](const int x, const int y, const bool /*strict*/) {
                             const int64_t idx = int64_t(y) * width + x;
                             if (map.island[idx] >= 0 && map.island[idx] != island_id) {
                               map.overlap[idx] = 1;
                             }
                             else {
                               map.island[idx] = island_id;
                             }
                             return true;
                           });
      }
      BLI_remlink(&island_list, &island);
      MEM_delete(island.faces);
      MEM_delete(&island);
    }

    if (owns_bm) {
      BM_mesh_free(bm);
    }
  }
  if (!any_uv) {
    /* No canvas object has an active UV map: an invalid map means "no barrier". */
    return UVIslandMap();
  }
  map.island_num = island_num;

  uv_island_map_dilate(map, uv_margin_px);
  return map;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::image_select
