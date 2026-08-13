/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_stack.h"
#include "BLI_task.h"
#include "BLI_time.h"
#include "BLI_vector.hh"

/* Toggle all PBR debug logging via PBR_PAINT_DEBUG_LOG in paint_debug.hh. */
#include "paint_debug.hh"

#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "../paint_intern.hh"
#include "paint_area_plane_2d.hh"
#include "paint_material_source.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_view2d.hh"

namespace blender {

/* Brush Painting for 2D image editor */

/* Defines and Structs */

struct BrushPainterCache {
  bool is_float; /* need float imbuf? */
  bool is_data;  /* is non-color data? */
  bool is_srgb;  /* is the byte colorspace sRGB? */
  const ColorSpace *byte_colorspace;
  bool invert;

  bool is_texbrush;
  bool is_maskbrush;

  int lastdiameter;
  float last_tex_rotation;
  float last_mask_rotation;
  float last_pressure;

  ImBuf *ibuf;
  ImBuf *texibuf;
  ushort *tex_mask;
  ushort *tex_mask_old;
  uint tex_mask_old_w;
  uint tex_mask_old_h;

  CurveMaskCache curve_mask_cache;

  // int image_size[2]; /* UNUSED. */
};

struct BrushPainter {
  Scene *scene;
  const Paint *paint;
  Brush *brush;

  /* Store initial starting points for perlin noise on the beginning of each stroke when using
   * color jitter. */
  std::optional<float3> initial_hsv_jitter;

  bool firsttouch; /* first paint op */

  ImagePool *pool;   /* image pool */
  rctf tex_mapping;  /* texture coordinate mapping */
  rctf mask_mapping; /* mask texture coordinate mapping */

  bool cache_invert;

  /**
   * When set, Mode=`Material` overrides draw color with a channel color:
   * RGB for Base Color / Normal, or grayscale `(v, v, v)` for scalar channels.
   */
  bool use_material_channel_color = false;
  /**
   * The #IMB_BlendMode this material channel paints with, or -1 outside material channel strokes,
   * where #Brush.blend applies. Resolved by #BKE_paint_material_channel_blend_mode so that the
   * Image Editor and the Sculpt viewport cannot drift apart on what a channel blends like.
   */
  short material_channel_blend = -1;
  float material_channel_color[3] = {};

  /**
   * Shared across the root Image Editor stroke and every extra material-map painter. Null when
   * this is not a Mode=`Material` stroke, or when invert/erase must not read sources.
   */
  std::shared_ptr<ed::sculpt_paint::material::ChannelSourceSet> channel_sources;
  /**
   * Evaluated-mesh triangles for #MTEX_MAP_MODE_AREA in the Image Editor. Shared by extra
   * material-map painters. Null when this stroke does not use Area Plane, or when the mesh/UV
   * could not be copied (then AREA falls back to View Plane).
   */
  std::shared_ptr<ed::sculpt_paint::AreaPlaneMesh> area_plane_mesh;
  /** Which material paint channel this painter writes. Only meaningful when
   * #use_material_channel_color is set. */
  eMaterialPaintChannel material_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  /** True when Alpha is set to mask the stroke; other channels' dab coverage is scaled by the
   * Alpha source (or its slider fallback). The Alpha channel itself is never clipped this way. */
  bool material_alpha_masking = false;
  float material_alpha_fallback = 1.0f;
  /** Canvas-space mapping for the shared source #MTex (View / Tiled / Stencil). */
  rctf source_mapping = {};
};

struct ImagePaintRegion {
  int destx, desty;
  int srcx, srcy;
  int width, height;
};

enum ImagePaintTileState {
  PAINT2D_TILE_UNINITIALIZED = 0,
  PAINT2D_TILE_MISSING,
  PAINT2D_TILE_READY,
};

struct ImagePaintTile {
  ImageUser iuser;
  ImBuf *canvas = nullptr;
  float radius_fac = 0.0f;
  int size[2] = {};
  float uv_origin[2] = {}; /* Stores the position of this tile in UV space. */
  bool need_redraw = false;
  BrushPainterCache cache = {};

  ImagePaintTileState state = PAINT2D_TILE_UNINITIALIZED;

  float last_paintpos[2] = {};  /* position of last paint op */
  float start_paintpos[2] = {}; /* position of first paint */
};

struct ImagePaintState {
  BrushPainter *painter;
  SpaceImage *sima;
  View2D *v2d;
  Scene *scene;
  const Paint *paint;

  Brush *brush;
  short brush_type, blend;
  Image *image;
  ImBuf *clonecanvas;

  bool do_masking;

  int symmetry;

  ImagePaintTile *tiles;
  int num_tiles;

  BlurKernel *blurkernel;

  /**
   * Owned secondary stroke states for Mode=MATERIAL multi-map write.
   * Primary target is this state; extras are additional Principled maps.
   * Only the root stroke handle owns extras (extras themselves have none).
   */
  ImagePaintState **material_extra_states;
  int material_extra_states_num;
};

static BrushPainter *brush_painter_2d_new(Scene *scene,
                                          const Paint *paint,
                                          Brush *brush,
                                          bool invert)
{
  BrushPainter *painter = MEM_new<BrushPainter>(__func__);

  painter->brush = brush;
  painter->scene = scene;
  painter->paint = paint;
  if (BKE_brush_color_jitter_get_settings(paint, brush)) {
    painter->initial_hsv_jitter = seed_hsv_jitter();
  }
  painter->firsttouch = true;
  painter->cache_invert = invert;

  return painter;
}

static void brush_painter_2d_require_imbuf(Brush *brush,
                                           ImagePaintTile *tile,
                                           bool is_float,
                                           bool is_data,
                                           bool is_srgb,
                                           const ColorSpace *byte_colorspace,
                                           bool invert)
{
  BrushPainterCache *cache = &tile->cache;

  if (cache->is_float != is_float) {
    if (cache->ibuf) {
      IMB_freeImBuf(cache->ibuf);
    }
    if (cache->tex_mask) {
      MEM_delete(cache->tex_mask);
    }
    if (cache->tex_mask_old) {
      MEM_delete(cache->tex_mask_old);
    }
    cache->ibuf = nullptr;
    cache->tex_mask = nullptr;
    cache->lastdiameter = -1; /* force ibuf create in refresh */
  }

  cache->is_float = is_float;
  cache->is_data = is_data;
  cache->is_srgb = is_srgb;
  cache->byte_colorspace = byte_colorspace;
  cache->invert = invert;
  cache->is_texbrush = (brush->mtex.tex &&
                        brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_DRAW) ?
                           true :
                           false;
  cache->is_maskbrush = (brush->mask_mtex.tex) ? true : false;
}

static void brush_painter_cache_2d_free(BrushPainterCache *cache)
{
  if (cache->ibuf) {
    IMB_freeImBuf(cache->ibuf);
  }
  if (cache->texibuf) {
    IMB_freeImBuf(cache->texibuf);
  }
  paint_curve_mask_cache_free_data(&cache->curve_mask_cache);
  if (cache->tex_mask) {
    MEM_delete(cache->tex_mask);
  }
  if (cache->tex_mask_old) {
    MEM_delete(cache->tex_mask_old);
  }
}

static void brush_imbuf_tex_co(const rctf *mapping, int x, int y, float texco[3])
{
  texco[0] = mapping->xmin + x * mapping->xmax;
  texco[1] = mapping->ymin + y * mapping->ymax;
  texco[2] = 0.0f;
}

/**
 * Image Editor 3D mapping has no view ray. Area Plane is handled by the surface→UV rasterizer
 * when #BrushPainter.area_plane_mesh is valid; this remap is only the fallback (no mesh/UV, or
 * 3D mapping which is out of scope for this path). Shared mapping defaults to Area Plane, so
 * skipping the fallback would silently paint the slider instead of the assigned source image.
 */
static short paint_2d_source_map_mode_for_2d(const short map_mode)
{
  if (ELEM(map_mode, MTEX_MAP_MODE_AREA, MTEX_MAP_MODE_3D)) {
    return MTEX_MAP_MODE_VIEW;
  }
  return map_mode;
}

static bool paint_2d_channel_source_usable_2d(const BrushPainter *painter,
                                              const eMaterialPaintChannel channel)
{
  if (painter->channel_sources == nullptr) {
    return false;
  }
  const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
      painter->channel_sources->source(channel);
  return source.usable && source.mtex != nullptr;
}

/**
 * Sample one channel source at a dab-buffer pixel using Image Editor 2D mapping
 * (View / Tiled / Stencil / Random; Area Plane and 3D are remapped to View).
 *
 * \return Texture intensity (mask/factor). RGB is written to \a r_rgba in scene-linear for color
 * sources; Normal skips colorspace decode.
 */
static float paint_2d_sample_channel_source(const BrushPainter *painter,
                                            const eMaterialPaintChannel channel,
                                            const int x,
                                            const int y,
                                            const int thread,
                                            float4 &r_rgba)
{
  const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
      painter->channel_sources->source(channel);
  float3 texco;
  brush_imbuf_tex_co(&painter->source_mapping, x, y, texco);
  ImagePool *pool = painter->channel_sources->pool() != nullptr ?
                        painter->channel_sources->pool() :
                        painter->pool;
  /* #BKE_brush_sample_tex_3d has no Area Plane branch: AREA would sample (0,0) on every pixel.
   * Copy and remap so the default shared mapping actually reads the source image. */
  MTex mtex_2d = dna::shallow_copy(*source.mtex);
  mtex_2d.brush_map_mode = eMTex_BrushMapMode(
      paint_2d_source_map_mode_for_2d(mtex_2d.brush_map_mode));
  const float intensity = BKE_brush_sample_tex_3d(
      painter->paint, painter->brush, &mtex_2d, texco, r_rgba, thread, pool);

  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
  /* #BKE_brush_sample_tex_3d decodes with the brush texture's colorspace when that flag is set,
   * which is the wrong space for a per-channel source. Only apply the source's own decode when
   * that brush-level conversion did not already run. */
  if (!is_normal && source.do_linear_conversion &&
      (paint_runtime == nullptr || !paint_runtime->do_linear_conversion))
  {
    IMB_colormanagement_colorspace_to_scene_linear_v3(r_rgba, source.colorspace);
  }
  if (source.flip_green_channel) {
    r_rgba[1] = 1.0f - r_rgba[1];
  }
  return intensity;
}

/** Replace slider RGB with the channel source (canvas colorspace), then clip coverage by Alpha. */
static void paint_2d_apply_material_sources(const BrushPainter *painter,
                                            const BrushPainterCache *cache,
                                            const int x,
                                            const int y,
                                            const int thread,
                                            float4 &rgba)
{
  if (!painter->use_material_channel_color || painter->channel_sources == nullptr) {
    return;
  }

  if (paint_2d_channel_source_usable_2d(painter, painter->material_channel)) {
    const float coverage = rgba[3];
    float4 sampled;
    const float intensity = paint_2d_sample_channel_source(
        painter, painter->material_channel, x, y, thread, sampled);
    const bool is_normal = painter->material_channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    const bool is_color = ELEM(painter->material_channel,
                               PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                               PAINT_MATERIAL_CHANNEL_EMISSION,
                               PAINT_MATERIAL_CHANNEL_NORMAL);
    if (is_color) {
      rgba[0] = sampled[0];
      rgba[1] = sampled[1];
      rgba[2] = sampled[2];
    }
    else {
      rgba[0] = intensity;
      rgba[1] = intensity;
      rgba[2] = intensity;
    }
    if (!is_normal && !cache->is_data) {
      if (cache->is_srgb) {
        IMB_colormanagement_scene_linear_to_srgb_v3(rgba, rgba);
      }
      else if (cache->byte_colorspace) {
        IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, cache->byte_colorspace);
      }
    }
    rgba[3] = coverage;
  }

  if (!painter->material_alpha_masking ||
      painter->material_channel == PAINT_MATERIAL_CHANNEL_ALPHA)
  {
    return;
  }

  float alpha_factor = painter->material_alpha_fallback;
  if (paint_2d_channel_source_usable_2d(painter, PAINT_MATERIAL_CHANNEL_ALPHA)) {
    float4 sampled;
    alpha_factor = paint_2d_sample_channel_source(
        painter, PAINT_MATERIAL_CHANNEL_ALPHA, x, y, thread, sampled);
  }
  alpha_factor = math::clamp(alpha_factor, 0.0f, 1.0f);
  rgba[3] *= alpha_factor;
}

/* create a mask with the mask texture */
static ushort *brush_painter_mask_ibuf_new(BrushPainter *painter, const int size)
{
  Brush *brush = painter->brush;
  rctf mask_mapping = painter->mask_mapping;
  ImagePool *pool = painter->pool;

  float texco[3];
  ushort *mask, *m;
  int x, y, thread = 0;

  mask = MEM_new_array_uninitialized<ushort>(size * size, __func__);
  m = mask;

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++, m++) {
      float res;
      brush_imbuf_tex_co(&mask_mapping, x, y, texco);
      res = BKE_brush_sample_masktex(painter->paint, brush, texco, thread, pool);
      *m = ushort(65535.0f * res);
    }
  }

  return mask;
}

/* update rectangular section of the brush image */
static void brush_painter_mask_imbuf_update(BrushPainter *painter,
                                            ImagePaintTile *tile,
                                            const ushort *tex_mask_old,
                                            int origx,
                                            int origy,
                                            int w,
                                            int h,
                                            int xt,
                                            int yt,
                                            const int diameter)
{
  Brush *brush = painter->brush;
  BrushPainterCache *cache = &tile->cache;
  rctf tex_mapping = painter->mask_mapping;
  ImagePool *pool = painter->pool;
  ushort res;

  bool use_texture_old = (tex_mask_old != nullptr);

  int x, y, thread = 0;

  ushort *tex_mask = cache->tex_mask;
  ushort *tex_mask_cur = cache->tex_mask_old;

  /* fill pixels */
  for (y = origy; y < h; y++) {
    for (x = origx; x < w; x++) {
      /* sample texture */
      float texco[3];

      /* handle byte pixel */
      ushort *b = tex_mask + (y * diameter + x);
      ushort *t = tex_mask_cur + (y * diameter + x);

      if (!use_texture_old) {
        brush_imbuf_tex_co(&tex_mapping, x, y, texco);
        res = ushort(65535.0f *
                     BKE_brush_sample_masktex(painter->paint, brush, texco, thread, pool));
      }

      /* read from old texture buffer */
      if (use_texture_old) {
        res = *(tex_mask_old + ((y - origy + yt) * cache->tex_mask_old_w + (x - origx + xt)));
      }

      /* write to new texture mask */
      *t = res;
      /* write to mask image buffer */
      *b = res;
    }
  }
}

/**
 * Update the brush mask image by trying to reuse the cached texture result.
 * This can be considerably faster for brushes that change size due to pressure or
 * textures that stick to the surface where only part of the pixels are new
 */
static void brush_painter_mask_imbuf_partial_update(BrushPainter *painter,
                                                    ImagePaintTile *tile,
                                                    const float pos[2],
                                                    const int diameter)
{
  BrushPainterCache *cache = &tile->cache;
  ushort *tex_mask_old;
  int destx, desty, srcx, srcy, w, h, x1, y1, x2, y2;

  /* create brush image buffer if it didn't exist yet */
  if (!cache->tex_mask) {
    cache->tex_mask = MEM_new_array_uninitialized<ushort>(diameter * diameter, __func__);
  }

  /* create new texture image buffer with coordinates relative to old */
  tex_mask_old = cache->tex_mask_old;
  cache->tex_mask_old = MEM_new_array_uninitialized<ushort>(diameter * diameter, __func__);

  if (tex_mask_old) {
    ImBuf maskibuf;
    ImBuf maskibuf_old;
    maskibuf.x = diameter;
    maskibuf.y = diameter;
    maskibuf_old.x = cache->tex_mask_old_w;
    maskibuf_old.y = cache->tex_mask_old_h;

    srcx = srcy = 0;
    w = cache->tex_mask_old_w;
    h = cache->tex_mask_old_h;
    destx = int(floorf(tile->last_paintpos[0])) - int(floorf(pos[0])) + (diameter / 2 - w / 2);
    desty = int(floorf(tile->last_paintpos[1])) - int(floorf(pos[1])) + (diameter / 2 - h / 2);

    /* hack, use temporary rects so that clipping works */
    IMB_rectclip(&maskibuf, &maskibuf_old, &destx, &desty, &srcx, &srcy, &w, &h);
  }
  else {
    srcx = srcy = 0;
    destx = desty = 0;
    w = h = 0;
  }

  x1 = min_ii(destx, diameter);
  y1 = min_ii(desty, diameter);
  x2 = min_ii(destx + w, diameter);
  y2 = min_ii(desty + h, diameter);

  /* blend existing texture in new position */
  if ((x1 < x2) && (y1 < y2)) {
    brush_painter_mask_imbuf_update(
        painter, tile, tex_mask_old, x1, y1, x2, y2, srcx, srcy, diameter);
  }

  if (tex_mask_old) {
    MEM_delete(tex_mask_old);
  }

  /* sample texture in new areas */
  if ((0 < x1) && (0 < diameter)) {
    brush_painter_mask_imbuf_update(painter, tile, nullptr, 0, 0, x1, diameter, 0, 0, diameter);
  }
  if ((x2 < diameter) && (0 < diameter)) {
    brush_painter_mask_imbuf_update(
        painter, tile, nullptr, x2, 0, diameter, diameter, 0, 0, diameter);
  }
  if ((x1 < x2) && (0 < y1)) {
    brush_painter_mask_imbuf_update(painter, tile, nullptr, x1, 0, x2, y1, 0, 0, diameter);
  }
  if ((x1 < x2) && (y2 < diameter)) {
    brush_painter_mask_imbuf_update(painter, tile, nullptr, x1, y2, x2, diameter, 0, 0, diameter);
  }

  /* through with sampling, now update sizes */
  cache->tex_mask_old_w = diameter;
  cache->tex_mask_old_h = diameter;
}

/* create imbuf with brush color */
static ImBuf *brush_painter_imbuf_new(
    BrushPainter *painter, ImagePaintTile *tile, const int size, float pressure, float distance)
{
  const Paint *paint = painter->paint;
  Brush *brush = painter->brush;
  BrushPainterCache *cache = &tile->cache;

  rctf tex_mapping = painter->tex_mapping;
  ImagePool *pool = painter->pool;

  const bool is_float = cache->is_float;
  const bool is_texbrush = cache->is_texbrush;

  int x, y, thread = 0;
  float brush_rgb[3];

  /* allocate image buffer */
  ImBuf *ibuf = IMB_allocImBuf(
      size, size, (is_float) ? ImBufFlags::FloatData : ImBufFlags::ByteData);

  /* get brush color */
  if (brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_DRAW) {
    if (painter->use_material_channel_color) {
      copy_v3_v3(brush_rgb, painter->material_channel_color);
    }
    else {
      paint_brush_color_get(
          paint, brush, painter->initial_hsv_jitter, cache->invert, distance, pressure, brush_rgb);
    }

    if (cache->is_srgb) {
      IMB_colormanagement_scene_linear_to_srgb_v3(brush_rgb, brush_rgb);
    }
    else if (cache->byte_colorspace) {
      IMB_colormanagement_scene_linear_to_colorspace_v3(brush_rgb, cache->byte_colorspace);
    }
  }
  else {
    brush_rgb[0] = 1.0f;
    brush_rgb[1] = 1.0f;
    brush_rgb[2] = 1.0f;
  }

  /* fill image buffer */
  uchar *byte_data = ibuf->byte_data_for_write();
  float *float_data = ibuf->float_data_for_write();
  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      /* sample texture and multiply with brush color */
      float3 texco;
      float4 rgba;

      if (is_texbrush) {
        brush_imbuf_tex_co(&tex_mapping, x, y, texco);
        const MTex *mtex = &brush->mtex;
        BKE_brush_sample_tex_3d(painter->paint, brush, mtex, texco, rgba, thread, pool);
        if (cache->is_srgb) {
          IMB_colormanagement_scene_linear_to_srgb_v3(rgba, rgba);
        }
        else if (cache->byte_colorspace) {
          IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, cache->byte_colorspace);
        }

        mul_v3_v3(rgba, brush_rgb);
      }
      else {
        copy_v3_v3(rgba, brush_rgb);
        rgba[3] = 1.0f;
      }

      paint_2d_apply_material_sources(painter, cache, x, y, thread, rgba);

      if (is_float) {
        /* write to float pixel */
        float *dstf = float_data + (y * size + x) * 4;
        mul_v3_v3fl(dstf, rgba, rgba[3]); /* premultiply */
        dstf[3] = rgba[3];
      }
      else {
        /* write to byte pixel */
        uchar *dst = byte_data + (y * size + x) * 4;

        rgb_float_to_uchar(dst, rgba);
        dst[3] = unit_float_to_uchar_clamp(rgba[3]);
      }
    }
  }

  return ibuf;
}

/* update rectangular section of the brush image */
static void brush_painter_imbuf_update(BrushPainter *painter,
                                       ImagePaintTile *tile,
                                       ImBuf *oldtexibuf,
                                       int origx,
                                       int origy,
                                       int w,
                                       int h,
                                       int xt,
                                       int yt)
{
  const Paint *paint = painter->paint;
  Brush *brush = painter->brush;
  const MTex *mtex = &brush->mtex;
  BrushPainterCache *cache = &tile->cache;

  rctf tex_mapping = painter->tex_mapping;
  ImagePool *pool = painter->pool;

  const bool is_float = cache->is_float;
  const bool is_texbrush = cache->is_texbrush;
  const bool use_texture_old = (oldtexibuf != nullptr);

  int x, y, thread = 0;
  float brush_rgb[3];

  ImBuf *ibuf = cache->ibuf;
  ImBuf *texibuf = cache->texibuf;

  /* get brush color */
  if (brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_DRAW) {
    if (painter->use_material_channel_color) {
      copy_v3_v3(brush_rgb, painter->material_channel_color);
    }
    else {
      paint_brush_color_get(
          paint, brush, painter->initial_hsv_jitter, cache->invert, 0.0f, 1.0f, brush_rgb);
    }

    if (cache->is_srgb) {
      IMB_colormanagement_scene_linear_to_srgb_v3(brush_rgb, brush_rgb);
    }
    else if (cache->byte_colorspace) {
      IMB_colormanagement_scene_linear_to_colorspace_v3(brush_rgb, cache->byte_colorspace);
    }
  }
  else {
    brush_rgb[0] = 1.0f;
    brush_rgb[1] = 1.0f;
    brush_rgb[2] = 1.0f;
  }

  /* fill pixels */
  uchar *ibuf_byte_data = ibuf->byte_data_for_write();
  float *ibuf_float_data = ibuf->float_data_for_write();
  uchar *texibuf_byte_data = texibuf->byte_data_for_write();
  float *texibuf_float_data = texibuf->float_data_for_write();
  const uchar *oldtexibuf_byte_data = (oldtexibuf) ? oldtexibuf->byte_data() : nullptr;
  const float *oldtexibuf_float_data = (oldtexibuf) ? oldtexibuf->float_data() : nullptr;
  for (y = origy; y < h; y++) {
    for (x = origx; x < w; x++) {
      /* sample texture and multiply with brush color */
      float3 texco;
      float4 rgba;

      if (!use_texture_old) {
        if (is_texbrush) {
          brush_imbuf_tex_co(&tex_mapping, x, y, texco);
          BKE_brush_sample_tex_3d(painter->paint, brush, mtex, texco, rgba, thread, pool);
          if (cache->is_srgb) {
            IMB_colormanagement_scene_linear_to_srgb_v3(rgba, rgba);
          }
          else if (cache->byte_colorspace) {
            IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, cache->byte_colorspace);
          }

          mul_v3_v3(rgba, brush_rgb);
        }
        else {
          copy_v3_v3(rgba, brush_rgb);
          rgba[3] = 1.0f;
        }

        paint_2d_apply_material_sources(painter, cache, x, y, thread, rgba);
      }

      if (is_float) {
        /* handle float pixel */
        float *bf = ibuf_float_data + (y * ibuf->x + x) * 4;
        float *tf = texibuf_float_data + (y * texibuf->x + x) * 4;

        /* read from old texture buffer */
        if (use_texture_old) {
          const float *otf = oldtexibuf_float_data +
                             ((y - origy + yt) * oldtexibuf->x + (x - origx + xt)) * 4;
          copy_v4_v4(rgba, otf);
        }

        /* write to new texture buffer */
        copy_v4_v4(tf, rgba);

        /* output premultiplied float image, mf was already premultiplied */
        mul_v3_v3fl(bf, rgba, rgba[3]);
        bf[3] = rgba[3];
      }
      else {
        uchar crgba[4];

        /* handle byte pixel */
        uchar *b = ibuf_byte_data + (y * ibuf->x + x) * 4;
        uchar *t = texibuf_byte_data + (y * texibuf->x + x) * 4;

        /* read from old texture buffer */
        if (use_texture_old) {
          const uchar *ot = oldtexibuf_byte_data +
                            ((y - origy + yt) * oldtexibuf->x + (x - origx + xt)) * 4;
          crgba[0] = ot[0];
          crgba[1] = ot[1];
          crgba[2] = ot[2];
          crgba[3] = ot[3];
        }
        else {
          rgba_float_to_uchar(crgba, rgba);
        }

        /* write to new texture buffer */
        t[0] = crgba[0];
        t[1] = crgba[1];
        t[2] = crgba[2];
        t[3] = crgba[3];

        /* write to brush image buffer */
        b[0] = crgba[0];
        b[1] = crgba[1];
        b[2] = crgba[2];
        b[3] = crgba[3];
      }
    }
  }
}

/* update the brush image by trying to reuse the cached texture result. this
 * can be considerably faster for brushes that change size due to pressure or
 * textures that stick to the surface where only part of the pixels are new */
static void brush_painter_imbuf_partial_update(BrushPainter *painter,
                                               ImagePaintTile *tile,
                                               const float pos[2],
                                               const int diameter)
{
  BrushPainterCache *cache = &tile->cache;
  ImBuf *oldtexibuf, *ibuf;
  int destx, desty, srcx, srcy, w, h, x1, y1, x2, y2;

  /* create brush image buffer if it didn't exist yet */
  ImBufFlags imbflag = (cache->is_float) ? ImBufFlags::FloatData : ImBufFlags::ByteData;
  if (!cache->ibuf) {
    cache->ibuf = IMB_allocImBuf(diameter, diameter, imbflag);
  }
  ibuf = cache->ibuf;

  /* create new texture image buffer with coordinates relative to old */
  oldtexibuf = cache->texibuf;
  cache->texibuf = IMB_allocImBuf(diameter, diameter, imbflag);

  if (oldtexibuf) {
    srcx = srcy = 0;
    w = oldtexibuf->x;
    h = oldtexibuf->y;
    destx = int(floorf(tile->last_paintpos[0])) - int(floorf(pos[0])) + (diameter / 2 - w / 2);
    desty = int(floorf(tile->last_paintpos[1])) - int(floorf(pos[1])) + (diameter / 2 - h / 2);

    IMB_rectclip(cache->texibuf, oldtexibuf, &destx, &desty, &srcx, &srcy, &w, &h);
  }
  else {
    srcx = srcy = 0;
    destx = desty = 0;
    w = h = 0;
  }

  x1 = min_ii(destx, ibuf->x);
  y1 = min_ii(desty, ibuf->y);
  x2 = min_ii(destx + w, ibuf->x);
  y2 = min_ii(desty + h, ibuf->y);

  /* blend existing texture in new position */
  if ((x1 < x2) && (y1 < y2)) {
    brush_painter_imbuf_update(painter, tile, oldtexibuf, x1, y1, x2, y2, srcx, srcy);
  }

  if (oldtexibuf) {
    IMB_freeImBuf(oldtexibuf);
  }

  /* sample texture in new areas */
  if ((0 < x1) && (0 < ibuf->y)) {
    brush_painter_imbuf_update(painter, tile, nullptr, 0, 0, x1, ibuf->y, 0, 0);
  }
  if ((x2 < ibuf->x) && (0 < ibuf->y)) {
    brush_painter_imbuf_update(painter, tile, nullptr, x2, 0, ibuf->x, ibuf->y, 0, 0);
  }
  if ((x1 < x2) && (0 < y1)) {
    brush_painter_imbuf_update(painter, tile, nullptr, x1, 0, x2, y1, 0, 0);
  }
  if ((x1 < x2) && (y2 < ibuf->y)) {
    brush_painter_imbuf_update(painter, tile, nullptr, x1, y2, x2, ibuf->y, 0, 0);
  }
}

static void brush_painter_2d_tex_mapping(ImagePaintState *s,
                                         ImagePaintTile *tile,
                                         const int diameter,
                                         const float pos[2],
                                         const float mouse[2],
                                         int mapmode,
                                         rctf *r_mapping)
{
  float invw = 1.0f / float(tile->canvas->x);
  float invh = 1.0f / float(tile->canvas->y);
  float start[2];

  /* find start coordinate of brush in canvas */
  start[0] = pos[0] - diameter / 2.0f;
  start[1] = pos[1] - diameter / 2.0f;

  if (mapmode == MTEX_MAP_MODE_STENCIL) {
    /* map from view coordinates of brush to region coordinates */
    float xmin, ymin, xmax, ymax;
    ui::view2d_view_to_region_fl(s->v2d, start[0] * invw, start[1] * invh, &xmin, &ymin);
    ui::view2d_view_to_region_fl(
        s->v2d, (start[0] + diameter) * invw, (start[1] + diameter) * invh, &xmax, &ymax);

    /* output r_mapping from brush ibuf x/y to region coordinates */
    r_mapping->xmax = (xmax - xmin) / float(diameter);
    r_mapping->ymax = (ymax - ymin) / float(diameter);
    r_mapping->xmin = xmin + (tile->uv_origin[0] * tile->size[0] * r_mapping->xmax);
    r_mapping->ymin = ymin + (tile->uv_origin[1] * tile->size[1] * r_mapping->ymax);
  }
  else if (mapmode == MTEX_MAP_MODE_3D) {
    /* 3D mapping, just mapping to canvas 0..1. */
    r_mapping->xmin = 2.0f * (start[0] * invw - 0.5f);
    r_mapping->ymin = 2.0f * (start[1] * invh - 0.5f);
    r_mapping->xmax = 2.0f * invw;
    r_mapping->ymax = 2.0f * invh;
  }
  else if (ELEM(mapmode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_RANDOM)) {
    /* view mapping */
    r_mapping->xmin = mouse[0] - diameter * 0.5f + 0.5f;
    r_mapping->ymin = mouse[1] - diameter * 0.5f + 0.5f;
    r_mapping->xmax = 1.0f;
    r_mapping->ymax = 1.0f;
  }
  else /* if (mapmode == MTEX_MAP_MODE_TILED) */ {
    r_mapping->xmin = int(-diameter * 0.5) + int(floorf(pos[0])) -
                      int(floorf(tile->start_paintpos[0]));
    r_mapping->ymin = int(-diameter * 0.5) + int(floorf(pos[1])) -
                      int(floorf(tile->start_paintpos[1]));
    r_mapping->xmax = 1.0f;
    r_mapping->ymax = 1.0f;
  }
}

static void brush_painter_2d_refresh_cache(ImagePaintState *s,
                                           BrushPainter *painter,
                                           ImagePaintTile *tile,
                                           const float pos[2],
                                           const float mouse[2],
                                           float pressure,
                                           float distance,
                                           float size)
{
  const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
  Brush *brush = painter->brush;
  BrushPainterCache *cache = &tile->cache;
  /* Adding 4 pixels of padding for brush anti-aliasing. */
  const int diameter = std::max(1, int(size * 2)) + 4;

  bool do_random = false;
  bool do_partial_update = false;
  bool update_color = ((brush->flag & BRUSH_USE_GRADIENT) &&
                       (ELEM(brush->gradient_stroke_mode,
                             BRUSH_GRADIENT_SPACING_REPEAT,
                             BRUSH_GRADIENT_SPACING_CLAMP) ||
                        (cache->last_pressure != pressure))) ||
                      BKE_brush_color_jitter_get_settings(painter->paint, brush);
  float tex_rotation = -brush->mtex.rot;
  float mask_rotation = -brush->mask_mtex.rot;

  painter->pool = BKE_image_pool_new();

  const bool use_2d_channel_source =
      paint_2d_channel_source_usable_2d(painter, painter->material_channel);
  if (painter->channel_sources != nullptr && painter->use_material_channel_color) {
    short source_map_mode = MTEX_MAP_MODE_VIEW;
    bool have_map = false;
    if (use_2d_channel_source) {
      source_map_mode = paint_2d_source_map_mode_for_2d(
          painter->channel_sources->source(painter->material_channel).mtex->brush_map_mode);
      have_map = true;
    }
    else if (paint_2d_channel_source_usable_2d(painter, PAINT_MATERIAL_CHANNEL_ALPHA)) {
      source_map_mode = paint_2d_source_map_mode_for_2d(
          painter->channel_sources->source(PAINT_MATERIAL_CHANNEL_ALPHA).mtex->brush_map_mode);
      have_map = true;
    }
    if (have_map) {
      brush_painter_2d_tex_mapping(
          s, tile, diameter, pos, mouse, source_map_mode, &painter->source_mapping);
    }
  }
  /* Source samples change with the dab; never reuse a previous buffer. */
  const bool do_source_rebuild = painter->channel_sources != nullptr &&
                                 painter->use_material_channel_color &&
                                 (use_2d_channel_source || painter->material_alpha_masking);

  /* determine how can update based on textures used */
  if (cache->is_texbrush) {
    if (brush->mtex.brush_map_mode == MTEX_MAP_MODE_VIEW) {
      tex_rotation += paint_runtime->brush_rotation;
    }
    else if (brush->mtex.brush_map_mode == MTEX_MAP_MODE_RANDOM) {
      do_random = true;
    }
    else if (!((brush->stroke_method == BRUSH_STROKE_ANCHORED) || update_color)) {
      do_partial_update = true;
    }

    brush_painter_2d_tex_mapping(
        s, tile, diameter, pos, mouse, brush->mtex.brush_map_mode, &painter->tex_mapping);
  }

  if (cache->is_maskbrush) {
    bool renew_maxmask = false;
    bool do_partial_update_mask = false;
    /* invalidate case for all mapping modes */
    if (brush->mask_mtex.brush_map_mode == MTEX_MAP_MODE_VIEW) {
      mask_rotation += paint_runtime->brush_rotation_sec;
    }
    else if (brush->mask_mtex.brush_map_mode == MTEX_MAP_MODE_RANDOM) {
      renew_maxmask = true;
    }
    else if (!(brush->stroke_method == BRUSH_STROKE_ANCHORED)) {
      do_partial_update_mask = true;
      renew_maxmask = true;
    }
    /* explicitly disable partial update even if it has been enabled above */
    if (brush->mask_pressure) {
      do_partial_update_mask = false;
      renew_maxmask = true;
    }

    if (diameter != cache->lastdiameter || (mask_rotation != cache->last_mask_rotation) ||
        renew_maxmask)
    {
      MEM_SAFE_DELETE(cache->tex_mask);

      brush_painter_2d_tex_mapping(
          s, tile, diameter, pos, mouse, brush->mask_mtex.brush_map_mode, &painter->mask_mapping);

      if (do_partial_update_mask) {
        brush_painter_mask_imbuf_partial_update(painter, tile, pos, diameter);
      }
      else {
        cache->tex_mask = brush_painter_mask_ibuf_new(painter, diameter);
      }
      cache->last_mask_rotation = mask_rotation;
    }
  }

  /* Re-initialize the curve mask. Mask is always recreated due to the change of position. */
  paint_curve_mask_cache_update(&cache->curve_mask_cache, brush, diameter, size, pos);

  /* detect if we need to recreate image brush buffer */
  if (do_source_rebuild) {
    do_partial_update = false;
  }
  if (diameter != cache->lastdiameter || (tex_rotation != cache->last_tex_rotation) || do_random ||
      do_source_rebuild || update_color)
  {
    if (cache->ibuf) {
      IMB_freeImBuf(cache->ibuf);
      cache->ibuf = nullptr;
    }

    if (do_partial_update) {
      /* do partial update of texture */
      brush_painter_imbuf_partial_update(painter, tile, pos, diameter);
    }
    else {
      /* create brush from scratch */
      cache->ibuf = brush_painter_imbuf_new(painter, tile, diameter, pressure, distance);
    }

    cache->lastdiameter = diameter;
    cache->last_tex_rotation = tex_rotation;
    cache->last_pressure = pressure;
  }
  else if (do_partial_update) {
    /* do only partial update of texture */
    int dx = int(floorf(tile->last_paintpos[0])) - int(floorf(pos[0]));
    int dy = int(floorf(tile->last_paintpos[1])) - int(floorf(pos[1]));

    if ((dx != 0) || (dy != 0)) {
      brush_painter_imbuf_partial_update(painter, tile, pos, diameter);
    }
  }

  BKE_image_pool_free(painter->pool);
  painter->pool = nullptr;
}

static bool paint_2d_ensure_tile_canvas(ImagePaintState *s, int i)
{
  if (i == 0) {
    return true;
  }
  if (i >= s->num_tiles) {
    return false;
  }

  if (s->tiles[i].state == PAINT2D_TILE_READY) {
    return true;
  }
  if (s->tiles[i].state == PAINT2D_TILE_MISSING) {
    return false;
  }

  s->tiles[i].cache.lastdiameter = -1;

  ImBuf *ibuf = BKE_image_acquire_ibuf(s->image, &s->tiles[i].iuser, nullptr);
  if (ibuf != nullptr) {
    if (ibuf->channels != 4) {
      s->tiles[i].state = PAINT2D_TILE_MISSING;
    }
    else if ((s->tiles[0].canvas->byte_data() && !ibuf->byte_data()) ||
             (s->tiles[0].canvas->float_data() && !ibuf->float_data()))
    {
      s->tiles[i].state = PAINT2D_TILE_MISSING;
    }
    else {
      s->tiles[i].size[0] = ibuf->x;
      s->tiles[i].size[1] = ibuf->y;
      s->tiles[i].radius_fac = sqrtf((float(ibuf->x) * float(ibuf->y)) /
                                     (s->tiles[0].size[0] * s->tiles[0].size[1]));
      s->tiles[i].state = PAINT2D_TILE_READY;
    }
  }
  else {
    s->tiles[i].state = PAINT2D_TILE_MISSING;
  }

  if (s->tiles[i].state == PAINT2D_TILE_MISSING) {
    BKE_image_release_ibuf(s->image, ibuf, nullptr);
    return false;
  }

  s->tiles[i].canvas = ibuf;
  return true;
}

/* keep these functions in sync */
static void paint_2d_ibuf_rgb_get(ImBuf *ibuf, int x, int y, float r_rgb[4])
{
  if (ibuf->float_data()) {
    const float *rrgbf = ibuf->float_data() + (ibuf->x * y + x) * 4;
    copy_v4_v4(r_rgb, rrgbf);
  }
  else {
    const uchar *rrgb = ibuf->byte_data() + (ibuf->x * y + x) * 4;
    straight_uchar_to_premul_float(r_rgb, rrgb);
  }
}
static void paint_2d_ibuf_rgb_set(
    ImBuf *ibuf, int x, int y, const bool is_torus, const float rgb[4])
{
  if (is_torus) {
    x %= ibuf->x;
    if (x < 0) {
      x += ibuf->x;
    }
    y %= ibuf->y;
    if (y < 0) {
      y += ibuf->y;
    }
  }

  if (float *float_data = ibuf->float_data_for_write()) {
    float *rrgbf = float_data + (ibuf->x * y + x) * 4;
    float map_alpha = (rgb[3] == 0.0f) ? rrgbf[3] : rrgbf[3] / rgb[3];

    mul_v3_v3fl(rrgbf, rgb, map_alpha);
    rrgbf[3] = rgb[3];
  }
  else {
    uchar straight[4];
    uchar *rrgb = ibuf->byte_data_for_write() + (ibuf->x * y + x) * 4;

    premul_float_to_straight_uchar(straight, rgb);
    rrgb[0] = straight[0];
    rrgb[1] = straight[1];
    rrgb[2] = straight[2];
    rrgb[3] = straight[3];
  }
}

static void paint_2d_ibuf_tile_convert(ImBuf *ibuf, int *x, int *y, short paint_tile)
{
  if (paint_tile & PAINT_TILE_X) {
    *x %= ibuf->x;
    if (*x < 0) {
      *x += ibuf->x;
    }
  }
  if (paint_tile & PAINT_TILE_Y) {
    *y %= ibuf->y;
    if (*y < 0) {
      *y += ibuf->y;
    }
  }
}

static float paint_2d_ibuf_add_if(
    ImBuf *ibuf, int x, int y, float *outrgb, short paint_tile, float w)
{
  float inrgb[4];

  if (paint_tile) {
    paint_2d_ibuf_tile_convert(ibuf, &x, &y, paint_tile);
  }
  /* need to also do clipping here always since tiled coordinates
   * are not always within bounds */
  if (x < ibuf->x && x >= 0 && y < ibuf->y && y >= 0) {
    paint_2d_ibuf_rgb_get(ibuf, x, y, inrgb);
  }
  else {
    return 0.0f;
  }

  mul_v4_fl(inrgb, w);
  add_v4_v4(outrgb, inrgb);

  return w;
}

static void paint_2d_lift_soften(ImagePaintState *s,
                                 ImagePaintTile *tile,
                                 ImBuf *ibuf,
                                 ImBuf *ibufb,
                                 const int *pos,
                                 const short paint_tile)
{
  bool sharpen = (tile->cache.invert ^ ((s->brush->flag & BRUSH_DIR_IN) != 0));
  float threshold = s->brush->sharp_threshold;
  int x, y, xi, yi, xo, yo, xk, yk;
  float count;
  int out_off[2], in_off[2], dim[2];
  int diff_pos[2];
  float outrgb[4];
  float rgba[4];
  BlurKernel *kernel = s->blurkernel;

  dim[0] = ibufb->x;
  dim[1] = ibufb->y;
  in_off[0] = pos[0];
  in_off[1] = pos[1];
  out_off[0] = out_off[1] = 0;

  if (!paint_tile) {
    IMB_rectclip(ibuf, ibufb, &in_off[0], &in_off[1], &out_off[0], &out_off[1], &dim[0], &dim[1]);

    if ((dim[0] == 0) || (dim[1] == 0)) {
      return;
    }
  }

  /* find offset inside mask buffers to sample them */
  sub_v2_v2v2_int(diff_pos, out_off, in_off);

  for (y = 0; y < dim[1]; y++) {
    for (x = 0; x < dim[0]; x++) {
      /* get input pixel */
      xi = in_off[0] + x;
      yi = in_off[1] + y;

      count = 0.0;
      if (paint_tile) {
        paint_2d_ibuf_tile_convert(ibuf, &xi, &yi, paint_tile);
        if (xi < ibuf->x && xi >= 0 && yi < ibuf->y && yi >= 0) {
          paint_2d_ibuf_rgb_get(ibuf, xi, yi, rgba);
        }
        else {
          zero_v4(rgba);
        }
      }
      else {
        /* coordinates have been clipped properly here, it should be safe to do this */
        paint_2d_ibuf_rgb_get(ibuf, xi, yi, rgba);
      }
      zero_v4(outrgb);

      for (yk = 0; yk < kernel->side; yk++) {
        for (xk = 0; xk < kernel->side; xk++) {
          count += paint_2d_ibuf_add_if(ibuf,
                                        xi + xk - kernel->pixel_len,
                                        yi + yk - kernel->pixel_len,
                                        outrgb,
                                        paint_tile,
                                        kernel->wdata[xk + yk * kernel->side]);
        }
      }

      if (count > 0.0f) {
        mul_v4_fl(outrgb, 1.0f / count);

        if (sharpen) {
          /* subtract blurred image from normal image gives high pass filter */
          sub_v3_v3v3(outrgb, rgba, outrgb);

          /* Now rgba_ub contains the edge result, but this should be converted to luminance to
           * avoid colored speckles appearing in final image, and also to check for threshold. */
          outrgb[0] = outrgb[1] = outrgb[2] = IMB_colormanagement_get_luminance(outrgb);
          if (fabsf(outrgb[0]) > threshold) {
            float mask = BKE_brush_alpha_get(s->paint, s->brush);
            float alpha = rgba[3];
            rgba[3] = outrgb[3] = mask;

            /* add to enhance edges */
            blend_color_add_float(outrgb, rgba, outrgb);
            outrgb[3] = alpha;
          }
          else {
            copy_v4_v4(outrgb, rgba);
          }
        }
      }
      else {
        copy_v4_v4(outrgb, rgba);
      }
      /* write into brush buffer */
      xo = out_off[0] + x;
      yo = out_off[1] + y;
      paint_2d_ibuf_rgb_set(ibufb, xo, yo, false, outrgb);
    }
  }
}

static void paint_2d_set_region(
    ImagePaintRegion *region, int destx, int desty, int srcx, int srcy, int width, int height)
{
  region->destx = destx;
  region->desty = desty;
  region->srcx = srcx;
  region->srcy = srcy;
  region->width = width;
  region->height = height;
}

static int paint_2d_torus_split_region(ImagePaintRegion region[4],
                                       ImBuf *dbuf,
                                       const ImBuf *sbuf,
                                       short paint_tile)
{
  int destx = region->destx;
  int desty = region->desty;
  int srcx = region->srcx;
  int srcy = region->srcy;
  int width = region->width;
  int height = region->height;
  int origw, origh, w, h, tot = 0;

  /* convert destination and source coordinates to be within image */
  if (paint_tile & PAINT_TILE_X) {
    destx = destx % dbuf->x;
    if (destx < 0) {
      destx += dbuf->x;
    }
    srcx = srcx % sbuf->x;
    if (srcx < 0) {
      srcx += sbuf->x;
    }
  }
  if (paint_tile & PAINT_TILE_Y) {
    desty = desty % dbuf->y;
    if (desty < 0) {
      desty += dbuf->y;
    }
    srcy = srcy % sbuf->y;
    if (srcy < 0) {
      srcy += sbuf->y;
    }
  }
  /* clip width of blending area to destination imbuf, to avoid writing the
   * same pixel twice */
  origw = w = (width > dbuf->x) ? dbuf->x : width;
  origh = h = (height > dbuf->y) ? dbuf->y : height;

  /* clip within image */
  IMB_rectclip(dbuf, sbuf, &destx, &desty, &srcx, &srcy, &w, &h);
  paint_2d_set_region(&region[tot++], destx, desty, srcx, srcy, w, h);

  /* do 3 other rects if needed */
  if ((paint_tile & PAINT_TILE_X) && w < origw) {
    paint_2d_set_region(
        &region[tot++], (destx + w) % dbuf->x, desty, (srcx + w) % sbuf->x, srcy, origw - w, h);
  }
  if ((paint_tile & PAINT_TILE_Y) && h < origh) {
    paint_2d_set_region(
        &region[tot++], destx, (desty + h) % dbuf->y, srcx, (srcy + h) % sbuf->y, w, origh - h);
  }
  if ((paint_tile & PAINT_TILE_X) && (paint_tile & PAINT_TILE_Y) && (w < origw) && (h < origh)) {
    paint_2d_set_region(&region[tot++],
                        (destx + w) % dbuf->x,
                        (desty + h) % dbuf->y,
                        (srcx + w) % sbuf->x,
                        (srcy + h) % sbuf->y,
                        origw - w,
                        origh - h);
  }

  return tot;
}

static void paint_2d_lift_smear(const ImBuf *ibuf, ImBuf *ibufb, int *pos, short paint_tile)
{
  ImagePaintRegion region[4];
  int a, tot;

  paint_2d_set_region(region, 0, 0, pos[0], pos[1], ibufb->x, ibufb->y);
  tot = paint_2d_torus_split_region(region, ibufb, ibuf, paint_tile);

  for (a = 0; a < tot; a++) {
    IMB_copy_rect(ibufb,
                  ibuf,
                  int2(region[a].srcx, region[a].srcy),
                  int2(region[a].destx, region[a].desty),
                  int2(region[a].width, region[a].height));
  }
}

static ImBuf *paint_2d_lift_clone(ImBuf *ibuf, ImBuf *ibufb, const int *pos)
{
  /* NOTE: #allocImbuf returns zeroed memory, so regions outside image will
   * have zero alpha, and hence not be blended onto the image */
  int w = ibufb->x, h = ibufb->y, destx = 0, desty = 0, srcx = pos[0], srcy = pos[1];
  ImBufFlags ibflags = ibufb->flags;
  if (ibufb->byte_data()) {
    ibflags |= ImBufFlags::ByteData;
  }
  if (ibufb->float_data()) {
    ibflags |= ImBufFlags::FloatData;
  }
  ImBuf *clonebuf = IMB_allocImBuf(w, h, ibflags);
  clonebuf->color_mode = ibufb->color_mode;

  IMB_rectclip(clonebuf, ibuf, &destx, &desty, &srcx, &srcy, &w, &h);
  IMB_rectblend(clonebuf,
                clonebuf,
                ibufb,
                nullptr,
                nullptr,
                nullptr,
                0,
                destx,
                desty,
                destx,
                desty,
                destx,
                desty,
                w,
                h,
                IMB_BLEND_COPY_ALPHA,
                false);
  IMB_rectblend(clonebuf,
                clonebuf,
                ibuf,
                nullptr,
                nullptr,
                nullptr,
                0,
                destx,
                desty,
                destx,
                desty,
                srcx,
                srcy,
                w,
                h,
                IMB_BLEND_COPY_RGB,
                false);

  return clonebuf;
}

static void paint_2d_convert_brushco(ImBuf *ibufb, const float pos[2], int ipos[2])
{
  ipos[0] = int(floorf(pos[0] - ibufb->x / 2));
  ipos[1] = int(floorf(pos[1] - ibufb->y / 2));
}

static void paint_2d_do_making_brush(ImagePaintState *s,
                                     ImagePaintTile *tile,
                                     ImagePaintRegion *region,
                                     ImBuf *frombuf,
                                     float mask_max,
                                     short blend,
                                     int tilex,
                                     int tiley,
                                     int tilew,
                                     int tileh)
{
  ImBuf tmpbuf;
  IMB_initImBuf(&tmpbuf, ED_IMAGE_UNDO_TILE_SIZE, ED_IMAGE_UNDO_TILE_SIZE, ImBufFlags::Zero);

  PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();

  for (int ty = tiley; ty <= tileh; ty++) {
    for (int tx = tilex; tx <= tilew; tx++) {
      /* retrieve original pixels + mask from undo buffer */
      ushort *mask;
      int origx = region->destx - tx * ED_IMAGE_UNDO_TILE_SIZE;
      int origy = region->desty - ty * ED_IMAGE_UNDO_TILE_SIZE;

      if (const ImBuf *data = ED_image_paint_tile_find(
              undo_tiles, s->image, tile->canvas, &tile->iuser, tx, ty, &mask, false))
      {
        if (tile->canvas->float_data()) {
          tmpbuf.float_buffer = data->float_buffer;
        }
        else {
          tmpbuf.byte_buffer = data->byte_buffer;
        }
      }

      IMB_rectblend(tile->canvas,
                    &tmpbuf,
                    frombuf,
                    mask,
                    tile->cache.curve_mask_cache.curve_mask,
                    tile->cache.tex_mask,
                    mask_max,
                    region->destx,
                    region->desty,
                    origx,
                    origy,
                    region->srcx,
                    region->srcy,
                    region->width,
                    region->height,
                    IMB_BlendMode(blend),
                    ((s->brush->flag & BRUSH_ACCUMULATE) != 0));
    }
  }
}

struct Paint2DForeachData {
  ImagePaintState *s;
  ImagePaintTile *tile;
  ImagePaintRegion *region;
  ImBuf *frombuf;
  float mask_max;
  short blend;
  int tilex;
  int tilew;
};

static void paint_2d_op_foreach_do(void *__restrict data_v,
                                   const int iter,
                                   const TaskParallelTLS *__restrict /*tls*/)
{
  Paint2DForeachData *data = static_cast<Paint2DForeachData *>(data_v);
  paint_2d_do_making_brush(data->s,
                           data->tile,
                           data->region,
                           data->frombuf,
                           data->mask_max,
                           data->blend,
                           data->tilex,
                           iter,
                           data->tilew,
                           iter);
}

static int paint_2d_op(void *state,
                       ImagePaintTile *tile,
                       const float lastpos[2],
                       const float pos[2])
{
  ImagePaintState *s = (static_cast<ImagePaintState *>(state));
  const ImagePaintSettings &image_paint_settings = s->scene->toolsettings->imapaint;
  ImBuf *clonebuf = nullptr, *frombuf;
  ImBuf *canvas = tile->canvas;
  ImBuf *ibufb = tile->cache.ibuf;
  ImagePaintRegion region[4];
  short paint_tile = s->symmetry & (PAINT_TILE_X | PAINT_TILE_Y);
  short blend = s->blend;
  const float *offset = image_paint_settings.clone_offset;
  float liftpos[2];
  float mask_max = BKE_brush_alpha_get(s->paint, s->brush);
  int bpos[2], blastpos[2], bliftpos[2];
  int a, tot;

  paint_2d_convert_brushco(ibufb, pos, bpos);

  /* lift from canvas */
  if (s->brush_type == IMAGE_PAINT_BRUSH_TYPE_SOFTEN) {
    paint_2d_lift_soften(s, tile, canvas, ibufb, bpos, paint_tile);
    blend = IMB_BLEND_INTERPOLATE;
  }
  else if (s->brush_type == IMAGE_PAINT_BRUSH_TYPE_SMEAR) {
    if (lastpos[0] == pos[0] && lastpos[1] == pos[1]) {
      return 0;
    }

    paint_2d_convert_brushco(ibufb, lastpos, blastpos);
    paint_2d_lift_smear(canvas, ibufb, blastpos, paint_tile);
    blend = IMB_BLEND_INTERPOLATE;
  }
  else if (s->brush_type == IMAGE_PAINT_BRUSH_TYPE_CLONE && s->clonecanvas) {
    liftpos[0] = pos[0] - offset[0] * canvas->x;
    liftpos[1] = pos[1] - offset[1] * canvas->y;

    paint_2d_convert_brushco(ibufb, liftpos, bliftpos);
    clonebuf = paint_2d_lift_clone(s->clonecanvas, ibufb, bliftpos);
  }

  frombuf = (clonebuf) ? clonebuf : ibufb;

  if (paint_tile) {
    paint_2d_set_region(region, bpos[0], bpos[1], 0, 0, frombuf->x, frombuf->y);
    tot = paint_2d_torus_split_region(region, canvas, frombuf, paint_tile);
  }
  else {
    paint_2d_set_region(region, bpos[0], bpos[1], 0, 0, frombuf->x, frombuf->y);
    tot = 1;
  }

  /* blend into canvas */
  for (a = 0; a < tot; a++) {
    ED_imapaint_dirty_region(s->image,
                             canvas,
                             &tile->iuser,
                             region[a].destx,
                             region[a].desty,
                             region[a].width,
                             region[a].height,
                             true);

    if (s->do_masking) {
      /* masking, find original pixels tiles from undo buffer to composite over */
      int tilex, tiley, tilew, tileh;

      imapaint_region_tiles(canvas,
                            region[a].destx,
                            region[a].desty,
                            region[a].width,
                            region[a].height,
                            &tilex,
                            &tiley,
                            &tilew,
                            &tileh);

      if (tiley == tileh) {
        paint_2d_do_making_brush(
            s, tile, &region[a], frombuf, mask_max, blend, tilex, tiley, tilew, tileh);
      }
      else {
        Paint2DForeachData data;
        data.s = s;
        data.tile = tile;
        data.region = &region[a];
        data.frombuf = frombuf;
        data.mask_max = mask_max;
        data.blend = blend;
        data.tilex = tilex;
        data.tilew = tilew;

        TaskParallelSettings settings;
        BLI_parallel_range_settings_defaults(&settings);
        BLI_task_parallel_range(tiley, tileh + 1, &data, paint_2d_op_foreach_do, &settings);
      }
    }
    else {
      /* no masking, composite brush directly onto canvas */
      IMB_rectblend_threaded(canvas,
                             canvas,
                             frombuf,
                             nullptr,
                             tile->cache.curve_mask_cache.curve_mask,
                             tile->cache.tex_mask,
                             mask_max,
                             region[a].destx,
                             region[a].desty,
                             region[a].destx,
                             region[a].desty,
                             region[a].srcx,
                             region[a].srcy,
                             region[a].width,
                             region[a].height,
                             IMB_BlendMode(blend),
                             false);
    }
  }

  if (clonebuf) {
    IMB_freeImBuf(clonebuf);
  }

  return 1;
}

static int paint_2d_canvas_set(ImagePaintState *s, const Paint *paint)
{
  /* set clone canvas */
  if (s->brush_type == IMAGE_PAINT_BRUSH_TYPE_CLONE) {
    const ImagePaintSettings &image_paint_settings = s->scene->toolsettings->imapaint;
    Image *ima = image_paint_settings.clone;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, nullptr, nullptr);

    if (!ima || !ibuf || !(ibuf->byte_data() || ibuf->float_data())) {
      BKE_image_release_ibuf(ima, ibuf, nullptr);
      return 0;
    }

    s->clonecanvas = ibuf;

    /* temporarily add float rect for cloning */
    if (s->tiles[0].canvas->float_data() && !s->clonecanvas->float_data()) {
      IMB_float_from_byte(s->clonecanvas);
    }
    else if (!s->tiles[0].canvas->float_data() && !s->clonecanvas->byte_data()) {
      IMB_byte_from_float(s->clonecanvas);
    }
  }

  /* set masking */
  s->do_masking = paint_use_opacity_masking(paint, s->brush);

  return 1;
}

static void paint_2d_canvas_free(ImagePaintState *s)
{
  for (int i = 0; i < s->num_tiles; i++) {
    BKE_image_release_ibuf(s->image, s->tiles[i].canvas, nullptr);
  }
  const ImagePaintSettings &image_paint_settings = s->scene->toolsettings->imapaint;
  BKE_image_release_ibuf(image_paint_settings.clone, s->clonecanvas, nullptr);

  if (s->blurkernel) {
    paint_delete_blur_kernel(s->blurkernel);
    MEM_delete(s->blurkernel);
  }
}

static void paint_2d_transform_mouse(View2D *v2d, const float in[2], float out[2])
{
  ui::view2d_region_to_view(v2d, in[0], in[1], &out[0], &out[1]);
}

static bool is_inside_tile(const int size[2], const float pos[2], const float brush[2])
{
  return (pos[0] >= -brush[0]) && (pos[0] < size[0] + brush[0]) && (pos[1] >= -brush[1]) &&
         (pos[1] < size[1] + brush[1]);
}

static void paint_2d_uv_to_coord(ImagePaintTile *tile, const float uv[2], float coord[2])
{
  coord[0] = (uv[0] - tile->uv_origin[0]) * tile->size[0];
  coord[1] = (uv[1] - tile->uv_origin[1]) * tile->size[1];
}

static bool paint_2d_use_area_plane(const BrushPainter *painter)
{
  return painter->use_material_channel_color && painter->area_plane_mesh != nullptr &&
         painter->area_plane_mesh->is_valid();
}

static void paint_2d_sample_area_mtex(const BrushPainter *painter,
                                      const MTex *mtex,
                                      const float4x4 &local_mat,
                                      const float3 &position,
                                      const int thread,
                                      ImagePool *pool,
                                      float *r_value,
                                      float4 &r_rgba)
{
  float3 point = position;
  mul_m4_v3(local_mat.ptr(), point);
  const float tex_x = point.x * mtex->size[0] + mtex->ofs[0];
  const float tex_y = point.y * mtex->size[1] + mtex->ofs[1];
  paint_get_tex_pixel(mtex, tex_x, tex_y, pool, thread, r_value, r_rgba);
  add_v3_fl(r_rgba, painter->brush->texture_sample_bias);
  *r_value -= painter->brush->texture_sample_bias;
}

static bool paint_2d_area_plane_falloff(const Brush *brush,
                                        const float4x4 &object_to_brush,
                                        const float3 &position,
                                        float *r_strength)
{
  float3 local = position;
  mul_m4_v3(object_to_brush.ptr(), local);
  const float distance = math::length(local);
  if (distance > 1.0f) {
    return false;
  }
  *r_strength = BKE_brush_curve_strength_clamped(brush, distance, 1.0f);
  return *r_strength > 0.0f;
}

static void paint_2d_area_encode_canvas_rgb(const BrushPainterCache *cache,
                                            const bool is_normal,
                                            float rgb[3])
{
  if (is_normal || cache->is_data) {
    return;
  }
  if (cache->is_srgb) {
    IMB_colormanagement_scene_linear_to_srgb_v3(rgb, rgb);
  }
  else if (cache->byte_colorspace) {
    IMB_colormanagement_scene_linear_to_colorspace_v3(rgb, cache->byte_colorspace);
  }
}

static ed::sculpt_paint::AreaPlaneFrame paint_2d_area_channel_frame(
    const BrushPainter *painter,
    const eMaterialPaintChannel channel,
    const ed::sculpt_paint::AreaPlaneTriangle &tri,
    const float3 &position,
    const float radius_object)
{
  float rotation = 0.0f;
  if (paint_2d_channel_source_usable_2d(painter, channel)) {
    rotation = painter->channel_sources->source(channel).mtex->rot;
  }
  return ed::sculpt_paint::area_plane_frame_from_triangle(
      tri, position, radius_object, rotation);
}

static void paint_2d_area_sample_channel_color(const BrushPainter *painter,
                                               const BrushPainterCache *cache,
                                               const eMaterialPaintChannel channel,
                                               const float4x4 &local_mat,
                                               const float3 &position,
                                               const int thread,
                                               ImagePool *pool,
                                               const PaintModeSettings &paint_mode,
                                               float3 &r_rgb)
{
  if (!paint_2d_channel_source_usable_2d(painter, channel)) {
    r_rgb = float3(painter->material_channel_color[0],
                   painter->material_channel_color[1],
                   painter->material_channel_color[2]);
    paint_2d_area_encode_canvas_rgb(cache, channel == PAINT_MATERIAL_CHANNEL_NORMAL, r_rgb);
    return;
  }

  const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
      painter->channel_sources->source(channel);
  float value;
  float4 sampled;
  paint_2d_sample_area_mtex(painter, source.mtex, local_mat, position, thread, pool, &value, sampled);

  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
  if (!is_normal && source.do_linear_conversion &&
      (paint_runtime == nullptr || !paint_runtime->do_linear_conversion))
  {
    IMB_colormanagement_colorspace_to_scene_linear_v3(sampled, source.colorspace);
  }
  if (source.flip_green_channel) {
    sampled[1] = 1.0f - sampled[1];
  }

  const bool is_color = ELEM(channel,
                             PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                             PAINT_MATERIAL_CHANNEL_EMISSION,
                             PAINT_MATERIAL_CHANNEL_NORMAL);
  if (is_color) {
    r_rgb = float3(sampled[0], sampled[1], sampled[2]);
  }
  else {
    const float2 range = BKE_paint_material_channel_range(paint_mode, channel);
    const float intensity = math::clamp(value, range.x, range.y);
    r_rgb = float3(intensity, intensity, intensity);
  }
  paint_2d_area_encode_canvas_rgb(cache, is_normal, r_rgb);
}

static float paint_2d_area_sample_alpha_factor(const BrushPainter *painter,
                                               const float4x4 &alpha_local_mat,
                                               const float3 &position,
                                               const int thread,
                                               ImagePool *pool)
{
  float alpha_factor = painter->material_alpha_fallback;
  if (paint_2d_channel_source_usable_2d(painter, PAINT_MATERIAL_CHANNEL_ALPHA)) {
    float value;
    float4 sampled;
    paint_2d_sample_area_mtex(painter,
                              painter->channel_sources->source(PAINT_MATERIAL_CHANNEL_ALPHA).mtex,
                              alpha_local_mat,
                              position,
                              thread,
                              pool,
                              &value,
                              sampled);
    alpha_factor = value;
  }
  return math::clamp(alpha_factor, 0.0f, 1.0f);
}

static void paint_2d_blend_area_buffer(ImagePaintState *s,
                                       ImagePaintTile *tile,
                                       const int destx,
                                       const int desty,
                                       ImBuf *frombuf,
                                       ushort *mask,
                                       const short blend)
{
  ImagePaintRegion region;
  paint_2d_set_region(&region, destx, desty, 0, 0, frombuf->x, frombuf->y);
  ED_imapaint_dirty_region(s->image,
                           tile->canvas,
                           &tile->iuser,
                           region.destx,
                           region.desty,
                           region.width,
                           region.height,
                           true);

  const float mask_max = BKE_brush_alpha_get(s->paint, s->brush);
  if (s->do_masking) {
    ushort *saved_curve_mask = tile->cache.curve_mask_cache.curve_mask;
    ushort *saved_tex_mask = tile->cache.tex_mask;
    tile->cache.curve_mask_cache.curve_mask = mask;
    tile->cache.tex_mask = nullptr;

    int tilex, tiley, tilew, tileh;
    imapaint_region_tiles(tile->canvas,
                          region.destx,
                          region.desty,
                          region.width,
                          region.height,
                          &tilex,
                          &tiley,
                          &tilew,
                          &tileh);
    paint_2d_do_making_brush(
        s, tile, &region, frombuf, mask_max, blend, tilex, tiley, tilew, tileh);

    tile->cache.curve_mask_cache.curve_mask = saved_curve_mask;
    tile->cache.tex_mask = saved_tex_mask;
  }
  else {
    IMB_rectblend_threaded(tile->canvas,
                           tile->canvas,
                           frombuf,
                           nullptr,
                           mask,
                           nullptr,
                           mask_max,
                           region.destx,
                           region.desty,
                           region.destx,
                           region.desty,
                           region.srcx,
                           region.srcy,
                           region.width,
                           region.height,
                           IMB_BlendMode(blend),
                           false);
  }
  tile->need_redraw = true;
}

/** Largest rasterized triangle bbox along one axis, in texels. */
static constexpr int AREA_PLANE_TRIANGLE_MAX_SIZE = 8192;

static bool paint_2d_area_plane_rasterize_triangle(
    ImagePaintState *s,
    ImagePaintTile *tile,
    const ed::sculpt_paint::AreaPlaneTriangle &tri,
    const float4x4 &object_to_brush,
    const float4x4 &channel_local_mat,
    const float4x4 &alpha_local_mat,
    const float4x4 &unfold_mat,
    const float3 &dab_origin,
    const float3 &dab_normal,
    const bool sample_alpha,
    ImagePool *pool)
{
  const BrushPainter *painter = s->painter;
  const BrushPainterCache *cache = &tile->cache;
  const PaintModeSettings &paint_mode = s->scene->toolsettings->paint_mode;
  const int thread = 0;

  const float3 face_normal = ed::sculpt_paint::area_plane_triangle_face_normal(tri);
  if (math::length_squared(face_normal) < 1e-12f) {
    return false;
  }

  float min_u = tri.uv[0].x;
  float max_u = tri.uv[0].x;
  float min_v = tri.uv[0].y;
  float max_v = tri.uv[0].y;
  for (int i = 1; i < 3; i++) {
    min_u = std::min(min_u, tri.uv[i].x);
    max_u = std::max(max_u, tri.uv[i].x);
    min_v = std::min(min_v, tri.uv[i].y);
    max_v = std::max(max_v, tri.uv[i].y);
  }
  if (max_u < tile->uv_origin[0] || min_u > tile->uv_origin[0] + 1.0f ||
      max_v < tile->uv_origin[1] || min_v > tile->uv_origin[1] + 1.0f)
  {
    return false;
  }

  int x0 = int(std::floor((min_u - tile->uv_origin[0]) * float(tile->size[0]))) - 1;
  int y0 = int(std::floor((min_v - tile->uv_origin[1]) * float(tile->size[1]))) - 1;
  int x1 = int(std::ceil((max_u - tile->uv_origin[0]) * float(tile->size[0]))) + 1;
  int y1 = int(std::ceil((max_v - tile->uv_origin[1]) * float(tile->size[1]))) + 1;
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, tile->size[0]);
  y1 = std::min(y1, tile->size[1]);
  const int w = x1 - x0;
  const int h = y1 - y0;
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (w > AREA_PLANE_TRIANGLE_MAX_SIZE || h > AREA_PLANE_TRIANGLE_MAX_SIZE) {
    return false;
  }

  ImBuf *raster_buf = IMB_allocImBuf(
      w, h, tile->cache.is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  Array<ushort> triangle_mask(w * h, ushort(0));
  uchar *byte_data = raster_buf->byte_data_for_write();
  float *float_data = raster_buf->float_data_for_write();
  bool any_written = false;

  for (int ly = 0; ly < h; ly++) {
    for (int lx = 0; lx < w; lx++) {
      const float2 uv(tile->uv_origin[0] + (float(x0 + lx) + 0.5f) / float(tile->size[0]),
                      tile->uv_origin[1] + (float(y0 + ly) + 0.5f) / float(tile->size[1]));
      if (!ed::sculpt_paint::area_plane_uv_pixel_inside_triangle(tri.uv, uv)) {
        continue;
      }

      float bary[3];
      barycentric_weights_v2(tri.uv[0], tri.uv[1], tri.uv[2], uv, bary);
      const float3 position = tri.position[0] * bary[0] + tri.position[1] * bary[1] +
                              tri.position[2] * bary[2];
      float strength;
      if (!paint_2d_area_plane_falloff(painter->brush, object_to_brush, position, &strength)) {
        continue;
      }
      float3 sample_position = math::transform_point(unfold_mat, position);
      sample_position += dab_normal *
                         (math::dot(dab_normal, dab_origin) - math::dot(dab_normal, sample_position));
      if (sample_alpha) {
        strength *= paint_2d_area_sample_alpha_factor(
            painter, alpha_local_mat, sample_position, thread, pool);
      }

      float3 rgb;
      paint_2d_area_sample_channel_color(painter,
                                         cache,
                                         painter->material_channel,
                                         channel_local_mat,
                                         sample_position,
                                         thread,
                                         pool,
                                         paint_mode,
                                         rgb);

      const int idx = ly * w + lx;
      if (float_data) {
        float *dst = float_data + idx * 4;
        dst[0] = rgb.x;
        dst[1] = rgb.y;
        dst[2] = rgb.z;
        dst[3] = 1.0f;
      }
      else {
        uchar *dst = byte_data + idx * 4;
        rgb_float_to_uchar(dst, rgb);
        dst[3] = 255;
      }
      triangle_mask[idx] = ushort(65535.0f * strength);
      any_written = true;
    }
  }

  if (any_written) {
    paint_2d_blend_area_buffer(
        s, tile, x0, y0, raster_buf, triangle_mask.data(), s->blend);
  }
  IMB_freeImBuf(raster_buf);
  return any_written;
}

static void paint_2d_area_plane_stroke(ImagePaintState *s,
                                       const float uv_center[2],
                                       const float base_size)
{
  BrushPainter *painter = s->painter;
  const ed::sculpt_paint::AreaPlaneMesh &mesh = *painter->area_plane_mesh;

  ed::sculpt_paint::AreaPlaneHit hit;
  if (!mesh.hit_at_uv(float2(uv_center[0], uv_center[1]), hit)) {
    return;
  }
  if (s->tiles[0].size[0] <= 0) {
    return;
  }

  const float radius_uv = base_size / float(s->tiles[0].size[0]);
  const float radius_object = mesh.radius_object(hit, radius_uv);
  if (radius_object <= 1e-12f) {
    return;
  }

  const float4x4 object_to_brush = ed::sculpt_paint::area_plane_local_mat(
      hit.position, hit.normal, radius_object, 0.0f);
  const ed::sculpt_paint::AreaPlaneTriangle dab_tri = mesh.triangle(hit.tri_index);
  const ed::sculpt_paint::AreaPlaneFrame channel_frame = paint_2d_area_channel_frame(
      painter, painter->material_channel, dab_tri, hit.position, radius_object);
  const float4x4 channel_local_mat = ed::sculpt_paint::area_plane_object_to_local(channel_frame);
  const bool sample_alpha = painter->material_alpha_masking &&
                            painter->material_channel != PAINT_MATERIAL_CHANNEL_ALPHA;
  const float4x4 alpha_local_mat =
      sample_alpha ? ed::sculpt_paint::area_plane_object_to_local(paint_2d_area_channel_frame(
                         painter,
                         PAINT_MATERIAL_CHANNEL_ALPHA,
                         dab_tri,
                         hit.position,
                         radius_object)) :
                     object_to_brush;

  const Vector<int> accepted = mesh.triangles_in_sphere(object_to_brush);
  if (accepted.is_empty()) {
    return;
  }

  const Vector<float4x4> unfold_mats = ed::sculpt_paint::area_plane_unfold_matrices(
      mesh.triangles(), hit.tri_index, accepted);
  float3 dab_normal = ed::sculpt_paint::area_plane_triangle_face_normal(dab_tri);
  if (math::length_squared(dab_normal) < 1e-12f) {
    dab_normal = hit.normal;
  }

  ImagePool *pool = painter->channel_sources != nullptr ? painter->channel_sources->pool() :
                                                          nullptr;

  for (int i = 0; i < s->num_tiles; i++) {
    if (!paint_2d_ensure_tile_canvas(s, i)) {
      continue;
    }
    ImagePaintTile *tile = &s->tiles[i];
    ImBuf *ibuf = tile->canvas;
    const bool is_data = ibuf->colorspace_is_data();
    const bool is_float = (ibuf->float_data() != nullptr);
    const ColorSpace *byte_colorspace = (is_float || is_data) ? nullptr :
                                                                ibuf->byte_buffer.colorspace;
    const bool is_srgb = (is_float || is_data) ?
                             false :
                             IMB_colormanagement_space_is_srgb(byte_colorspace);
    brush_painter_2d_require_imbuf(painter->brush,
                                   tile,
                                   is_float,
                                   is_data,
                                   is_srgb,
                                   byte_colorspace,
                                   painter->cache_invert);

    for (int tri_i = 0; tri_i < accepted.size(); tri_i++) {
      paint_2d_area_plane_rasterize_triangle(s,
                                             tile,
                                             mesh.triangle(accepted[tri_i]),
                                             object_to_brush,
                                             channel_local_mat,
                                             alpha_local_mat,
                                             unfold_mats[tri_i],
                                             hit.position,
                                             dab_normal,
                                             sample_alpha,
                                             pool);
    }
  }
}

static void paint_2d_stroke_single(ImagePaintState *s,
                                   const float prev_mval[2],
                                   const float mval[2],
                                   const bool eraser,
                                   float pressure,
                                   float distance,
                                   float base_size)
{
  float new_uv[2], old_uv[2];
  BrushPainter *painter = s->painter;

  s->blend = s->brush->blend;
  if (eraser) {
    s->blend = IMB_BLEND_ERASE_ALPHA;
  }
  else if (painter->material_channel_blend >= 0) {
    /* Material channels carry their own mode, including the Normal and erase special cases. */
    s->blend = painter->material_channel_blend;
  }

  ui::view2d_region_to_view(s->v2d, mval[0], mval[1], &new_uv[0], &new_uv[1]);
  ui::view2d_region_to_view(s->v2d, prev_mval[0], prev_mval[1], &old_uv[0], &old_uv[1]);

  if (!eraser && paint_2d_use_area_plane(painter)) {
    paint_2d_area_plane_stroke(s, new_uv, base_size);
    painter->firsttouch = false;
    return;
  }

  float last_uv[2], start_uv[2];
  ui::view2d_region_to_view(s->v2d, 0.0f, 0.0f, &start_uv[0], &start_uv[1]);
  if (painter->firsttouch) {
    /* paint exactly once on first touch */
    copy_v2_v2(last_uv, new_uv);
  }
  else {
    copy_v2_v2(last_uv, old_uv);
  }

  const float uv_brush_size[2] = {
      (s->symmetry & PAINT_TILE_X) ? FLT_MAX : base_size / s->tiles[0].size[0],
      (s->symmetry & PAINT_TILE_Y) ? FLT_MAX : base_size / s->tiles[0].size[1]};

  for (int i = 0; i < s->num_tiles; i++) {
    ImagePaintTile *tile = &s->tiles[i];

    /* First test: Project brush into UV space, clip against tile. */
    const int uv_size[2] = {1, 1};
    float local_new_uv[2], local_old_uv[2];
    sub_v2_v2v2(local_new_uv, new_uv, tile->uv_origin);
    sub_v2_v2v2(local_old_uv, old_uv, tile->uv_origin);
    if (!(is_inside_tile(uv_size, local_new_uv, uv_brush_size) ||
          is_inside_tile(uv_size, local_old_uv, uv_brush_size)))
    {
      continue;
    }

    /* Lazy tile loading to get size in pixels. */
    if (!paint_2d_ensure_tile_canvas(s, i)) {
      continue;
    }

    float size = base_size * tile->radius_fac;

    float new_coord[2], old_coord[2];
    paint_2d_uv_to_coord(tile, new_uv, new_coord);
    paint_2d_uv_to_coord(tile, old_uv, old_coord);
    if (painter->firsttouch) {
      paint_2d_uv_to_coord(tile, start_uv, tile->start_paintpos);
    }
    paint_2d_uv_to_coord(tile, last_uv, tile->last_paintpos);

    /* Second check in pixel coordinates. */
    const float pixel_brush_size[] = {(s->symmetry & PAINT_TILE_X) ? FLT_MAX : size,
                                      (s->symmetry & PAINT_TILE_Y) ? FLT_MAX : size};
    if (!(is_inside_tile(tile->size, new_coord, pixel_brush_size) ||
          is_inside_tile(tile->size, old_coord, pixel_brush_size)))
    {
      continue;
    }

    ImBuf *ibuf = tile->canvas;

    const bool is_data = ibuf->colorspace_is_data();
    const bool is_float = (ibuf->float_data() != nullptr);
    const ColorSpace *byte_colorspace = (is_float || is_data) ? nullptr :
                                                                ibuf->byte_buffer.colorspace;
    const bool is_srgb = (is_float || is_data) ?
                             false :
                             IMB_colormanagement_space_is_srgb(byte_colorspace);

    /* OCIO_TODO: float buffers are now always linear, so always use color correction
     *            this should probably be changed when texture painting color space is supported
     */
    brush_painter_2d_require_imbuf(painter->brush,
                                   tile,
                                   (ibuf->float_data() != nullptr),
                                   is_data,
                                   is_srgb,
                                   byte_colorspace,
                                   painter->cache_invert);

    brush_painter_2d_refresh_cache(s, painter, tile, new_coord, mval, pressure, distance, size);

    if (paint_2d_op(s, tile, old_coord, new_coord)) {
      tile->need_redraw = true;
    }
  }

  painter->firsttouch = false;
}

void paint_2d_stroke(void *ps,
                     const float prev_mval[2],
                     const float mval[2],
                     const bool eraser,
                     float pressure,
                     float distance,
                     float base_size)
{
  ImagePaintState *s = static_cast<ImagePaintState *>(ps);
  paint_2d_stroke_single(s, prev_mval, mval, eraser, pressure, distance, base_size);
  for (int i = 0; i < s->material_extra_states_num; i++) {
    paint_2d_stroke_single(
        s->material_extra_states[i], prev_mval, mval, eraser, pressure, distance, base_size);
  }
}

/**
 * Initialize a single 2D paint stroke state for \a image.
 * \param material_iuser: Optional ImageUser from Material target resolve (may be null).
 * \param material_channel_value: When >= 0 and \a material_channel_rgb is null,
 *   override brush RGB with (v,v,v).
 * \param material_channel_rgb: When non-null, override brush RGB with this color
 *   (Mode=`Material` Base Color or packed Normal). Takes precedence over \a material_channel_value.
 * \param material_channel_blend: The #IMB_BlendMode this material channel paints with, as resolved
 *   by #BKE_paint_material_channel_blend_mode, or -1 when this is not a material channel stroke and
 *   #Brush.blend applies.
 * \param init_brush_tex: When true, begin brush texture nodetree exec (once per stroke root).
 * \return Stroke state or null on failure (caller owns success).
 */
static ImagePaintState *paint_2d_new_stroke_for_image(bContext *C,
                                                     wmOperator *op,
                                                     const BrushStrokeMode mode,
                                                     Image *image,
                                                     ImageUser *material_iuser,
                                                     const float material_channel_value,
                                                     const float *material_channel_rgb,
                                                     const short material_channel_blend,
                                                     const bool init_brush_tex)
{
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  ToolSettings *settings = scene->toolsettings;
  const Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(&settings->imapaint.paint);

  if (image == nullptr) {
    return nullptr;
  }

  ImagePaintState *s = MEM_new_zeroed<ImagePaintState>(__func__);

  s->sima = sima;
  s->v2d = &CTX_wm_region(C)->v2d;
  s->scene = scene;
  s->paint = paint;

  s->brush = brush;
  s->brush_type = brush->image_brush_type;
  s->blend = brush->blend;
  s->image = image;

  s->symmetry = settings->imapaint.paint.symmetry_flags;

  if (BKE_image_has_packedfile(s->image) && s->image->rr != nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Packed MultiLayer files cannot be painted");
    MEM_delete(s);
    return nullptr;
  }

  s->num_tiles = s->image->tiles.count();
  s->tiles = MEM_new_array<ImagePaintTile>(s->num_tiles, __func__);
  for (int i = 0; i < s->num_tiles; i++) {
    s->tiles[i].iuser = material_iuser ? *material_iuser : sima->iuser;
  }

  zero_v2(s->tiles[0].uv_origin);

  ImBuf *ibuf = BKE_image_acquire_ibuf(s->image, &s->tiles[0].iuser, nullptr);
  if (ibuf == nullptr) {
    MEM_delete(s->tiles);
    MEM_delete(s);
    return nullptr;
  }

  if (ibuf->channels != 4) {
    BKE_image_release_ibuf(s->image, ibuf, nullptr);
    BKE_report(op->reports, RPT_WARNING, "Image requires 4 color channels to paint");
    MEM_delete(s->tiles);
    MEM_delete(s);
    return nullptr;
  }

  s->tiles[0].size[0] = ibuf->x;
  s->tiles[0].size[1] = ibuf->y;
  s->tiles[0].radius_fac = 1.0f;

  s->tiles[0].canvas = ibuf;
  s->tiles[0].state = PAINT2D_TILE_READY;

  /* Initialize offsets here, they're needed for the uv space clip test before lazy-loading the
   * tile properly. */
  int tile_idx = 0;
  for (ImageTile *tile = static_cast<ImageTile *>(s->image->tiles.first); tile;
       tile = tile->next, tile_idx++)
  {
    s->tiles[tile_idx].iuser.tile = tile->tile_number;
    s->tiles[tile_idx].uv_origin[0] = ((tile->tile_number - 1001) % 10);
    s->tiles[tile_idx].uv_origin[1] = ((tile->tile_number - 1001) / 10);
  }

  if (!paint_2d_canvas_set(s, paint)) {
    /* Release acquired tile canvas before freeing the state. */
    BKE_image_release_ibuf(s->image, s->tiles[0].canvas, nullptr);
    s->tiles[0].canvas = nullptr;
    MEM_delete(s->tiles);

    MEM_delete(s);
    return nullptr;
  }

  if (brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_SOFTEN) {
    s->blurkernel = paint_new_blur_kernel(brush, false);
  }

  if (init_brush_tex) {
    paint_brush_init_tex(s->brush);
  }

  /* create painter */
  s->painter = brush_painter_2d_new(scene, paint, s->brush, mode == BrushStrokeMode::Invert);
  s->painter->material_channel_blend = material_channel_blend;
  if (material_channel_rgb != nullptr) {
    s->painter->use_material_channel_color = true;
    copy_v3_v3(s->painter->material_channel_color, material_channel_rgb);
  }
  else if (material_channel_value >= 0.0f) {
    s->painter->use_material_channel_color = true;
    s->painter->material_channel_color[0] = material_channel_value;
    s->painter->material_channel_color[1] = material_channel_value;
    s->painter->material_channel_color[2] = material_channel_value;
  }

  return s;
}

static void paint_2d_stroke_done_single(ImagePaintState *s, const bool exit_brush_tex)
{
  paint_2d_canvas_free(s);
  for (int i = 0; i < s->num_tiles; i++) {
    brush_painter_cache_2d_free(&s->tiles[i].cache);
  }
  MEM_delete(s->painter);
  MEM_delete(s->tiles);
  if (exit_brush_tex) {
    paint_brush_exit_tex(s->brush);
  }

  MEM_delete(s);
}

void *paint_2d_new_stroke(bContext *C, wmOperator *op, const BrushStrokeMode mode)
{
  Scene *scene = CTX_data_scene(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  ToolSettings *settings = scene->toolsettings;
  const PaintModeSettings &paint_mode = settings->paint_mode;
  Object *ob = CTX_data_active_object(C);

  /* Mesh-attribute modes are not Image Editor 2D canvases — never write sima->image. */
  if (paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL_PAINT ||
      paint_mode.canvas_source == PAINT_CANVAS_SOURCE_COLOR_ATTRIBUTE)
  {
    return nullptr;
  }

  if (paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) {
    if (ob == nullptr) {
      return nullptr;
    }

    Brush *brush = BKE_paint_brush(&settings->imapaint.paint);
    if (brush == nullptr) {
      return nullptr;
    }
    /* Per-channel settings are lazily allocated. Seeding them here instead of bailing keeps a
     * first stroke on a fresh brush from silently doing nothing. */
    BKE_brush_material_paint_ensure(brush);
    const BrushMaterialPaint &brush_paint = *brush->material_paint;

    /* #Material.paint_channel_cache is only refreshed on writes made through this API (see its
     * doc comment); anything that changes the node tree from outside it (undo/redo swapping in a
     * different Main, manual node edits in the Shader Editor) leaves the cache pointing at a
     * still-valid but no-longer-wired Image, so painting would keep writing into a map the
     * material's Principled BSDF no longer reads from. A once-per-stroke invalidation is cheap
     * (a handful of socket/link lookups) compared to the per-dab sampling cost already spent
     * below, and removes that whole class of staleness. */
    BKE_paint_material_channel_cache_invalidate(BKE_object_material_get(ob, ob->actcol));

    /* Auto-create and wire up an Image Texture on the Principled BSDF for each enabled
     * channel that doesn't already resolve to one, matching the View3D Sculpt path. */
    Main *bmain = CTX_data_main(C);
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (info.socket_name == nullptr) {
        continue;
      }
      if (!BKE_paint_material_channel_writes_to_target(brush_paint, paint_mode, info.channel)) {
        continue;
      }
      Image *channel_image;
      ImageUser *channel_iuser;
      BKE_paint_principled_channel_image_ensure(*bmain,
                                                *ob,
                                                info.channel,
                                                paint_mode.new_channel_image_size,
                                                &channel_image,
                                                &channel_iuser);
    }

    const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
        *ob, paint_mode, &brush_paint);
    if (targets.is_empty()) {
      return nullptr;
    }

    const Paint *paint = BKE_paint_get_active_from_context(C);
    const bool invert = mode == BrushStrokeMode::Invert;

    std::shared_ptr<ed::sculpt_paint::material::ChannelSourceSet> channel_sources;
    if (!invert) {
      channel_sources = std::make_shared<ed::sculpt_paint::material::ChannelSourceSet>(
          brush_paint, paint_mode);
      if (!channel_sources->is_active()) {
        channel_sources.reset();
      }
    }
    const bool alpha_masking = channel_sources != nullptr &&
                               BKE_paint_material_channel_masks_stroke(brush_paint, paint_mode);
    const float alpha_fallback = BKE_paint_material_channel_value(
        brush_paint, paint_mode, PAINT_MATERIAL_CHANNEL_ALPHA);

    std::shared_ptr<ed::sculpt_paint::AreaPlaneMesh> area_plane_mesh;
    if (channel_sources != nullptr) {
      bool use_area = false;
      for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
        const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
            channel_sources->source(i);
        if (source.usable && source.mtex != nullptr &&
            source.mtex->brush_map_mode == MTEX_MAP_MODE_AREA)
        {
          use_area = true;
          break;
        }
      }
      if (use_area && ob != nullptr && ob->type == OB_MESH) {
        Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
        if (depsgraph != nullptr) {
          area_plane_mesh = std::make_shared<ed::sculpt_paint::AreaPlaneMesh>(
              *depsgraph, *ob, "");
          if (!area_plane_mesh->is_valid()) {
            area_plane_mesh.reset();
          }
        }
      }
    }

    ImagePaintState *primary = nullptr;
    Vector<ImagePaintState *> extras;
    for (const PaintMaterialImageTarget &target : targets) {
      float rgb_storage[3];
      const float *rgb_ptr = nullptr;
      float value = -1.0f;
      /* Resolved once here so the 2D path and the Sculpt path agree on the mode per channel. */
      const short channel_blend = BKE_paint_material_channel_blend_mode(
          brush_paint, target.channel, invert);
      if (target.is_color_channel) {
        const float3 rgb = BKE_paint_material_base_color_get(brush_paint, *paint, *brush, invert);
        copy_v3_v3(rgb_storage, rgb);
        rgb_ptr = rgb_storage;
      }
      else if (target.is_normal_channel) {
        float tangent[3] = {0.0f, 0.0f, 1.0f};
        if (!invert) {
          copy_v3_v3(tangent, target.color);
        }
        BKE_pbr_normal_pack(tangent, false, rgb_storage);
        rgb_ptr = rgb_storage;
      }
      else {
        if (invert) {
          value = BKE_paint_material_channel_default_value(target.channel);
        }
        else {
          value = target.value;
        }
      }
      ImagePaintState *state = paint_2d_new_stroke_for_image(C,
                                                            op,
                                                            mode,
                                                            target.image,
                                                            target.iuser,
                                                            value,
                                                            rgb_ptr,
                                                            channel_blend,
                                                            primary == nullptr);
      if (state == nullptr) {
        /* Surfaced rather than silently dropped: a channel quietly not painting is exactly the
         * kind of no-op a user has no way to notice on their own. */
        BKE_reportf(op->reports,
                   RPT_WARNING,
                   "Material Texture: %s channel failed to start (missing/unloadable image)",
                   BKE_paint_material_channel_info(target.channel).ui_name);
        continue; /* Skip missing / unloadable maps. */
      }
      state->painter->material_channel = target.channel;
      state->painter->channel_sources = channel_sources;
      state->painter->area_plane_mesh = area_plane_mesh;
      state->painter->material_alpha_masking = alpha_masking;
      state->painter->material_alpha_fallback = alpha_fallback;
      if (primary == nullptr) {
        primary = state;
      }
      else {
        extras.append(state);
      }
    }
    if (primary == nullptr) {
      return nullptr;
    }
    if (!extras.is_empty()) {
      primary->material_extra_states_num = int(extras.size());
      primary->material_extra_states = MEM_new_array<ImagePaintState *>(extras.size(), __func__);
      for (const int i : extras.index_range()) {
        primary->material_extra_states[i] = extras[i];
      }
    }
    return primary;
  }

  Image *image = sima->image;
  if (paint_mode.canvas_source == PAINT_CANVAS_SOURCE_IMAGE && paint_mode.canvas_image != nullptr) {
    image = paint_mode.canvas_image;
  }
  return paint_2d_new_stroke_for_image(C, op, mode, image, nullptr, -1.0f, nullptr, -1, true);
}

static void paint_2d_redraw_single(const bContext *C, ImagePaintState *s, bool final)
{
#if PBR_PAINT_IMAGE_UPDATE_PROFILE
  const double perf_start = BLI_time_now_seconds();
#endif
  bool had_redraw = false;
  int redraw_tiles = 0;
  for (int i = 0; i < s->num_tiles; i++) {
    if (s->tiles[i].need_redraw) {
      ImBuf *ibuf = BKE_image_acquire_ibuf(s->image, &s->tiles[i].iuser, nullptr);

      imapaint_image_update(s->sima, s->image, ibuf, &s->tiles[i].iuser, false);

      BKE_image_release_ibuf(s->image, ibuf, nullptr);

      s->tiles[i].need_redraw = false;
      had_redraw = true;
      redraw_tiles++;
    }
  }

  if (had_redraw) {
    /* Notify every editor showing this image (not just the current region), so a channel
     * painted while a different image is displayed elsewhere still gets a live update signal. */
    WM_event_add_notifier(C, NC_IMAGE | NA_PAINTING, s->image);
    if (s->sima == nullptr || !s->sima->lock) {
      ED_region_tag_redraw(CTX_wm_region(C));
    }
    /* Explicit flags rather than the legacy `0` ("tag everything") — the shading/material
     * consumers of this image's pixels need to actually see a shading-relevant tag to rebuild
     * their GPU material, not just any tag on the ID. */
    DEG_id_tag_update(&s->image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);
  }

  if (final) {
    if (s->image && !(s->sima && s->sima->lock)) {
      BKE_image_free_gputextures(s->image);
    }

    /* compositor listener deals with updating */
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, s->image);
    DEG_id_tag_update(&s->image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);

    /* Ideally, we shouldn't have to tag the object as needing to be recalculated if using this
     * paint mode, however, because the image isn't connected as part of the shader nodes, the draw
     * code is unaware of the corresponding image tag. See #150957 for more details.
     *
     * The Material Texture canvas (#PAINT_CANVAS_SOURCE_MATERIAL) hits the same symptom for a
     * different reason: the image *is* wired into the Principled BSDF, but only the image that
     * happens to be displayed in this Image Editor gets its GPU material/shading state refreshed
     * as a side effect of the region redraw. A channel that was never opened in this editor (e.g.
     * an "extra" channel painted alongside the primary one) would otherwise keep showing stale
     * shading in the 3D Viewport/Shader Editor until something else forces a full GPU material
     * rebuild (such as a file reload) even though the painted pixels were saved correctly. */
    const Scene *scene = CTX_data_scene(C);
    Object *object = CTX_data_active_object(C);
    if (object && object->type == OB_MESH && scene &&
        (scene->toolsettings->imapaint.mode == IMAGEPAINT_MODE_IMAGE ||
         scene->toolsettings->paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL))
    {
      DEG_id_tag_update(&object->id, ID_RECALC_SHADING);
    }
  }

#if PBR_PAINT_IMAGE_UPDATE_PROFILE
  printf("[PBR-PERF] redraw_single image=%s final=%d tiles=%d elapsed=%.3f ms\n",
         s->image ? s->image->id.name + 2 : "<null>",
         int(final),
         redraw_tiles,
         (BLI_time_now_seconds() - perf_start) * 1000.0);
#endif
}

void paint_2d_redraw(const bContext *C, void *ps, bool final)
{
#if PBR_PAINT_IMAGE_UPDATE_PROFILE
  const double perf_start = BLI_time_now_seconds();
#endif
  ImagePaintState *s = static_cast<ImagePaintState *>(ps);

  paint_2d_redraw_single(C, s, final);
  for (int i = 0; i < s->material_extra_states_num; i++) {
    paint_2d_redraw_single(C, s->material_extra_states[i], final);
  }
  /* The dirty region is shared by all material-channel states. Clear it only after every image
   * has consumed it; clearing it in paint_2d_redraw_single() makes extra channels miss their GPU
   * update after the primary channel is processed. */
  ED_imapaint_clear_partial_redraw();
#if PBR_PAINT_IMAGE_UPDATE_PROFILE
  printf("[PBR-PERF] redraw total final=%d states=%d elapsed=%.3f ms\n",
         int(final),
         1 + s->material_extra_states_num,
         (BLI_time_now_seconds() - perf_start) * 1000.0);
#endif
}

void paint_2d_stroke_done(void *ps)
{
  ImagePaintState *s = static_cast<ImagePaintState *>(ps);

  for (int i = 0; i < s->material_extra_states_num; i++) {
    paint_2d_stroke_done_single(s->material_extra_states[i], false);
  }
  MEM_delete(s->material_extra_states);
  s->material_extra_states = nullptr;
  s->material_extra_states_num = 0;

  paint_2d_stroke_done_single(s, true);
}

static void paint_2d_fill_add_pixel_byte(const int x_px,
                                         const int y_px,
                                         ImBuf *ibuf,
                                         BLI_Stack *stack,
                                         BLI_bitmap *touched,
                                         const float color[4],
                                         float threshold_sq)
{
  size_t coordinate;

  if (x_px >= ibuf->x || x_px < 0 || y_px >= ibuf->y || y_px < 0) {
    return;
  }

  coordinate = size_t(y_px) * ibuf->x + x_px;

  if (!BLI_BITMAP_TEST(touched, coordinate)) {
    float color_f[4];
    const uchar *color_b = ibuf->byte_data() + 4 * coordinate;
    rgba_uchar_to_float(color_f, color_b);
    straight_to_premul_v4(color_f);

    if (len_squared_v4v4(color_f, color) <= threshold_sq) {
      BLI_stack_push(stack, &coordinate);
    }
    BLI_BITMAP_SET(touched, coordinate, true);
  }
}

static void paint_2d_fill_add_pixel_float(const int x_px,
                                          const int y_px,
                                          ImBuf *ibuf,
                                          BLI_Stack *stack,
                                          BLI_bitmap *touched,
                                          const float color[4],
                                          float threshold_sq)
{
  size_t coordinate;

  if (x_px >= ibuf->x || x_px < 0 || y_px >= ibuf->y || y_px < 0) {
    return;
  }

  coordinate = size_t(y_px) * ibuf->x + x_px;

  if (!BLI_BITMAP_TEST(touched, coordinate)) {
    if (len_squared_v4v4(ibuf->float_data() + 4 * coordinate, color) <= threshold_sq) {
      BLI_stack_push(stack, &coordinate);
    }
    BLI_BITMAP_SET(touched, coordinate, true);
  }
}

static ImageUser *paint_2d_get_tile_iuser(ImagePaintState *s, int tile_number)
{
  ImageUser *iuser = &s->tiles[0].iuser;
  for (int i = 0; i < s->num_tiles; i++) {
    if (s->tiles[i].iuser.tile == tile_number) {
      if (!paint_2d_ensure_tile_canvas(s, i)) {
        return nullptr;
      }
      iuser = &s->tiles[i].iuser;
      break;
    }
  }

  return iuser;
}

void paint_2d_bucket_fill(const bContext *C,
                          const float color[3],
                          Brush *br,
                          const float mouse_init[2],
                          const float mouse_final[2],
                          void *ps)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  Image *ima = sima->image;

  ImagePaintState *s = static_cast<ImagePaintState *>(ps);

  ImBuf *ibuf;
  int x_px, y_px;
  uint color_b;
  float color_f[4];
  float strength = (s && br) ? BKE_brush_alpha_get(paint, br) : 1.0f;

  bool do_float;

  if (!ima) {
    return;
  }

  View2D *v2d = s ? s->v2d : &CTX_wm_region(C)->v2d;
  float uv_origin[2];
  float image_init[2];
  paint_2d_transform_mouse(v2d, mouse_init, image_init);

  int tile_number = BKE_image_get_tile_from_pos(ima, image_init, image_init, uv_origin);

  ImageUser local_iuser, *iuser;
  if (s != nullptr) {
    iuser = paint_2d_get_tile_iuser(s, tile_number);
    if (iuser == nullptr) {
      return;
    }
  }
  else {
    iuser = &local_iuser;
    BKE_imageuser_default(iuser);
    iuser->tile = tile_number;
  }

  ibuf = BKE_image_acquire_ibuf(ima, iuser, nullptr);
  if (!ibuf) {
    return;
  }

  do_float = (ibuf->float_data() != nullptr);
  /* First check if our image is float. If it is we should correct the color to be in linear space.
   */
  if (!do_float) {
    float3 ibuf_color = color;
    IMB_colormanagement_scene_linear_to_colorspace_v3(ibuf_color, ibuf->byte_buffer.colorspace);
    rgb_float_to_uchar(reinterpret_cast<uchar *>(&color_b), ibuf_color);
    *((reinterpret_cast<char *>(&color_b)) + 3) = strength * 255;
  }
  else {
    copy_v3_v3(color_f, color);
    color_f[3] = strength;
  }

  if (!mouse_final || !br) {
    /* first case, no image UV, fill the whole image */
    ED_imapaint_dirty_region(ima, ibuf, iuser, 0, 0, ibuf->x, ibuf->y, false);

    if (do_float) {
      float *float_data = ibuf->float_data_for_write();
      for (x_px = 0; x_px < ibuf->x; x_px++) {
        for (y_px = 0; y_px < ibuf->y; y_px++) {
          blend_color_mix_float(float_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                                float_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                                color_f);
        }
      }
    }
    else {
      uchar *byte_data = ibuf->byte_data_for_write();
      for (x_px = 0; x_px < ibuf->x; x_px++) {
        for (y_px = 0; y_px < ibuf->y; y_px++) {
          blend_color_mix_byte(byte_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                               byte_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                               reinterpret_cast<uchar *>(&color_b));
        }
      }
    }
  }
  else {
    /* second case, start sweeping the neighboring pixels, looking for pixels whose
     * value is within the brush fill threshold from the fill color */
    BLI_Stack *stack;
    BLI_bitmap *touched;
    size_t coordinate;
    int width = ibuf->x;
    float pixel_color[4];
    /* We are comparing to sum of three squared values
     * (assumed in range [0,1]), so need to multiply... */
    float threshold_sq = br->fill_threshold * br->fill_threshold * 3;

    x_px = image_init[0] * ibuf->x;
    y_px = image_init[1] * ibuf->y;

    if (x_px >= ibuf->x || x_px < 0 || y_px > ibuf->y || y_px < 0) {
      BKE_image_release_ibuf(ima, ibuf, nullptr);
      return;
    }

    /* change image invalidation method later */
    ED_imapaint_dirty_region(ima, ibuf, iuser, 0, 0, ibuf->x, ibuf->y, false);

    stack = BLI_stack_new(sizeof(size_t), __func__);
    touched = BLI_BITMAP_NEW(size_t(ibuf->x) * ibuf->y, "bucket_fill_bitmap");

    coordinate = (size_t(y_px) * ibuf->x + x_px);

    if (do_float) {
      copy_v4_v4(pixel_color, ibuf->float_data() + 4 * coordinate);
    }
    else {
      const uchar *pixel_color_b = ibuf->byte_data() + 4 * coordinate;
      rgba_uchar_to_float(pixel_color, pixel_color_b);
      straight_to_premul_v4(pixel_color);
    }

    BLI_stack_push(stack, &coordinate);
    BLI_BITMAP_SET(touched, coordinate, true);

    if (do_float) {
      while (!BLI_stack_is_empty(stack)) {
        BLI_stack_pop(stack, &coordinate);

        IMB_blend_color_float(ibuf->float_data_for_write() + 4 * (coordinate),
                              ibuf->float_data_for_write() + 4 * (coordinate),
                              color_f,
                              IMB_BlendMode(br->blend));

        /* reconstruct the coordinates here */
        x_px = coordinate % width;
        y_px = coordinate / width;

        paint_2d_fill_add_pixel_float(
            x_px - 1, y_px - 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px - 1, y_px, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px - 1, y_px + 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px, y_px + 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px, y_px - 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px + 1, y_px - 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px + 1, y_px, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_float(
            x_px + 1, y_px + 1, ibuf, stack, touched, pixel_color, threshold_sq);
      }
    }
    else {
      while (!BLI_stack_is_empty(stack)) {
        BLI_stack_pop(stack, &coordinate);

        IMB_blend_color_byte(ibuf->byte_data_for_write() + 4 * coordinate,
                             ibuf->byte_data_for_write() + 4 * coordinate,
                             reinterpret_cast<uchar *>(&color_b),
                             IMB_BlendMode(br->blend));

        /* reconstruct the coordinates here */
        x_px = coordinate % width;
        y_px = coordinate / width;

        paint_2d_fill_add_pixel_byte(
            x_px - 1, y_px - 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px - 1, y_px, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px - 1, y_px + 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px, y_px + 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px, y_px - 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px + 1, y_px - 1, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px + 1, y_px, ibuf, stack, touched, pixel_color, threshold_sq);
        paint_2d_fill_add_pixel_byte(
            x_px + 1, y_px + 1, ibuf, stack, touched, pixel_color, threshold_sq);
      }
    }

    MEM_delete(touched);
    BLI_stack_free(stack);
  }

  imapaint_image_update(sima, ima, ibuf, iuser, false);
  ED_imapaint_clear_partial_redraw();

  BKE_image_release_ibuf(ima, ibuf, nullptr);

  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

void paint_2d_gradient_fill(
    const bContext *C, Brush *br, const float mouse_init[2], const float mouse_final[2], void *ps)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  Image *ima = sima->image;
  ImagePaintState *s = static_cast<ImagePaintState *>(ps);

  ImBuf *ibuf;
  int x_px, y_px;
  uint color_b;
  float color_f[4];
  float image_init[2], image_final[2];
  float tangent[2];
  float line_len_sq_inv, line_len;
  const float brush_alpha = BKE_brush_alpha_get(s->paint, br);

  bool do_float;

  if (ima == nullptr) {
    return;
  }

  float uv_origin[2];
  int tile_number = BKE_image_get_tile_from_pos(ima, image_init, image_init, uv_origin);
  ImageUser *iuser = paint_2d_get_tile_iuser(s, tile_number);
  if (!iuser) {
    return;
  }

  ibuf = BKE_image_acquire_ibuf(ima, iuser, nullptr);
  if (ibuf == nullptr) {
    return;
  }

  paint_2d_transform_mouse(s->v2d, mouse_final, image_final);
  paint_2d_transform_mouse(s->v2d, mouse_init, image_init);
  sub_v2_v2(image_init, uv_origin);
  sub_v2_v2(image_final, uv_origin);

  image_final[0] *= ibuf->x;
  image_final[1] *= ibuf->y;

  image_init[0] *= ibuf->x;
  image_init[1] *= ibuf->y;

  /* some math to get needed gradient variables */
  sub_v2_v2v2(tangent, image_final, image_init);
  line_len = len_squared_v2(tangent);
  line_len_sq_inv = 1.0f / line_len;
  line_len = sqrtf(line_len);

  do_float = (ibuf->float_data() != nullptr);

  /* this will be substituted by something else when selection is available */
  ED_imapaint_dirty_region(ima, ibuf, iuser, 0, 0, ibuf->x, ibuf->y, false);

  if (do_float) {
    float *float_data = ibuf->float_data_for_write();
    for (x_px = 0; x_px < ibuf->x; x_px++) {
      for (y_px = 0; y_px < ibuf->y; y_px++) {
        float f;
        const float p[2] = {x_px - image_init[0], y_px - image_init[1]};

        switch (br->gradient_fill_mode) {
          case BRUSH_GRADIENT_LINEAR: {
            f = dot_v2v2(p, tangent) * line_len_sq_inv;
            break;
          }
          case BRUSH_GRADIENT_RADIAL:
          default: {
            f = len_v2(p) / line_len;
            break;
          }
        }
        BKE_colorband_evaluate(br->gradient, f, color_f);
        /* convert to premultiplied */
        mul_v3_fl(color_f, color_f[3]);
        color_f[3] *= brush_alpha;
        IMB_blend_color_float(float_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                              float_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                              color_f,
                              IMB_BlendMode(br->blend));
      }
    }
  }
  else {
    uchar *byte_data = ibuf->byte_data_for_write();
    for (x_px = 0; x_px < ibuf->x; x_px++) {
      for (y_px = 0; y_px < ibuf->y; y_px++) {
        float f;
        const float p[2] = {x_px - image_init[0], y_px - image_init[1]};

        switch (br->gradient_fill_mode) {
          case BRUSH_GRADIENT_LINEAR: {
            f = dot_v2v2(p, tangent) * line_len_sq_inv;
            break;
          }
          case BRUSH_GRADIENT_RADIAL:
          default: {
            f = len_v2(p) / line_len;
            break;
          }
        }

        BKE_colorband_evaluate(br->gradient, f, color_f);
        IMB_colormanagement_scene_linear_to_colorspace_v3(color_f, ibuf->byte_buffer.colorspace);
        rgba_float_to_uchar(reinterpret_cast<uchar *>(&color_b), color_f);
        (reinterpret_cast<uchar *>(&color_b))[3] *= brush_alpha;
        IMB_blend_color_byte(byte_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                             byte_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                             reinterpret_cast<uchar *>(&color_b),
                             IMB_BlendMode(br->blend));
      }
    }
  }

  imapaint_image_update(sima, ima, ibuf, iuser, false);
  ED_imapaint_clear_partial_redraw();

  BKE_image_release_ibuf(ima, ibuf, nullptr);

  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

}  // namespace blender
