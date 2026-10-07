/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Feature source of the Magic Wand / Quick Selection tools: turn one UDIM tile of an image (the
 * active canvas, or a PBR channel image) into a normalized per-pixel feature vector, plus the
 * sample (seed reference) and distance metric that define "similar color" for the tools.
 *
 * Color-like features are compared in a display-like sRGB space (the space the user judges
 * colors in, matching Photoshop's tolerance semantics); non-color (data) images are compared
 * raw. Normal-map sources decode to unit vectors and compare by angle.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_image_types.h"

#include "paint_image_select_region.hh"

namespace blender {
struct Object;
struct ReportList;
struct Scene;
struct SpaceImage;
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint::image_select {

/** Which part of the pixel the feature is built from. */
enum class SourceComponent : int8_t {
  RGB = 0,
  RGBA,
  Luminance,
  Hue,
  Saturation,
  Value,
  Red,
  Green,
  Blue,
  Alpha,
};

/** How two feature vectors are compared. #Auto resolves to #NormalAngle for normal sources and
 * #MaxChannel for everything else. */
enum class SourceMetric : int8_t {
  Auto = 0,
  MaxChannel,
  Euclidean,
  OkLab,
  NormalAngle,
};

/** Per-pixel feature vectors of one UDIM tile, laid out row-major with #dims floats per pixel. */
struct FeatureImage {
  int2 size = int2(0, 0);
  int tile_number = 1001;
  /** 1 for scalar features, 3 for color / OkLab / normals, 4 for RGBA. */
  int dims = 0;
  /** The data are unit vectors (a decoded normal map); compare by angle. */
  bool is_normal = false;
  /** The single scalar is a circular quantity (hue in 0..1); compare circularly. */
  bool circular = false;
  SourceMetric metric = SourceMetric::MaxChannel;
  /** dims * w * h floats. */
  Array<float> data;

  bool is_empty() const
  {
    return size.x <= 0 || size.y <= 0 || dims <= 0;
  }
  int64_t pixel_index(const int x, const int y) const
  {
    return (int64_t(y) * size.x + x) * dims;
  }
  const float *pixel(const int x, const int y) const
  {
    return &data[pixel_index(x, y)];
  }
  MutableSpan<float> pixel_mutable(const int x, const int y)
  {
    return MutableSpan<float>(data.data() + pixel_index(x, y), dims);
  }
};

/**
 * Build the feature image of one tile.
 *
 * \param is_normal: the source is a tangent-space normal map; pixels decode to unit vectors
 * (OpenGL convention, RGB * 2 - 1) and the metric resolves to #NormalAngle.
 * \param normalize_range: stretch a scalar feature over the tile's min..max (useful for float
 * height maps with an arbitrary range); ignored for color and normals.
 *
 * Returns an empty image when the tile has no buffer.
 */
FeatureImage feature_image_build(Image *image,
                                 const ImageUser *owner_iuser,
                                 int tile_number,
                                 SourceComponent component,
                                 SourceMetric metric,
                                 bool is_normal,
                                 bool normalize_range);

/** Resolve #SourceMetric::Auto for a concrete feature. */
SourceMetric metric_resolve(SourceMetric metric, bool is_normal);

/**
 * Mean feature over a \a sample_size x \a sample_size window around \a center, clipped to the
 * tile. Normals average as vectors and re-normalize; hue averages circularly. \a r_sample must
 * hold #FeatureImage::dims floats.
 */
void feature_sample_window(const FeatureImage &feature,
                           int2 center,
                           int sample_size,
                           MutableSpan<float> r_sample);

/** Distance between two feature vectors of \a feature, normalized to 0..1. */
float feature_distance(const FeatureImage &feature, const float *a, const float *b);

/**
 * Build the UV-island map of one tile from the image's canvas objects (Mask purpose: original
 * meshes / edit-meshes, active UV map). See #UVIslandMap in paint_image_select_region.hh for
 * the overlap semantics.
 *
 * \param uv_margin_px: gutter dilation — pixels without UV within this distance of an island
 * inherit the nearest island's id, so selections can cover the texel bleed around seams
 * (0..16, 2 is a good default).
 *
 * Returns an invalid map when no canvas object has an active UV map (the caller then treats
 * every pixel as passable / outside).
 */
UVIslandMap uv_island_map_build(const bContext *C,
                                const Image *image,
                                int tile_number,
                                int width,
                                int height,
                                int uv_margin_px);

/* -------------------------------------------------------------------- */
/** \name Source resolution (wand / quick select)
 * \{ */

struct SelectSource {
  Image *image = nullptr;
  /** Owner's ImageUser copied in (the caller keeps it alive). */
  ImageUser iuser;
  int tile_number = 1001;
  /** The source is a tangent-space normal map. */
  bool is_normal = false;
  /** True when the configured source exists but resolves to no image (report + cancel). */
  bool failed = false;
  /** True when the click position has no UDIM tile in the image (report + cancel). */
  bool outside_tiles = false;
};

/**
 * Resolve the selection source of the current editor: the active image in Canvas mode, the
 * selected PBR channel's image in Material mode (#select_source_channel: a material channel, or
 * the Image Editor's active pass). \a click_uv picks the UDIM tile of \a source.image
 * (\a source.outside_tiles when the image has no tile there).
 */
SelectSource select_source_resolve(const SpaceImage *sima,
                                   const Scene *scene,
                                   Object *ob,
                                   const float2 &click_uv);

/**
 * #select_source_resolve plus the shared failure reports (no source image, failed channel,
 * click outside the image tiles) as warnings on \a reports. False when the caller must cancel.
 */
bool select_source_resolve_or_report(bContext *C,
                                     ReportList *reports,
                                     const float2 &click_uv,
                                     SelectSource &r_source);

/** \} */

}  // namespace blender::ed::sculpt_paint::image_select
