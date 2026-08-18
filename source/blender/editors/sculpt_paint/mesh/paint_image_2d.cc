/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_view3d_types.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_kdopbvh.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_bits.h"
#include "BLI_math_color.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_mutex.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_stack.h"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

/* Toggle all PBR debug logging via PBR_PAINT_DEBUG_LOG in paint_debug.hh. */
#include "paint_debug.hh"

#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_colorband.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_editmesh_bvh.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"
#include "bmesh.hh"

#include "DEG_depsgraph.hh"

#include "../paint_intern.hh"
#include "paint_area_plane_2d.hh"
#include "paint_material_source.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_uvedit.hh"
#include "ED_view3d.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_view2d.hh"

#include "../paint_intern.hh"
#include "paint_image_select_gradient.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_uv_geom.hh"
#include "paint_image_uv_symmetry.hh"

namespace blender {

static float paint_2d_selection_mask_sample(
    const Scene * /*scene*/, const Image *image, int tile_number, int x, int y)
{
  if (!BKE_image_paint_selection_mask_has_any(image)) {
    return 1.0f;
  }

  return BKE_image_paint_selection_blend_sample(image, tile_number, x, y);
}

static float paint_2d_selection_blend_sample_bilinear(
    const Scene * /*scene*/, const Image *image, int tile_number, const float fx, const float fy)
{
  if (!BKE_image_paint_selection_mask_has_any(image)) {
    return 1.0f;
  }

  return BKE_image_paint_selection_blend_sample_bilinear(image, tile_number, fx, fy);
}

/* Brush constraint: binary inside-test (hard edge, no feather weighting). */
static bool paint_2d_selection_mask_is_inside(const Image *image, int tile_number, int x, int y)
{
  return BKE_image_paint_selection_mask_sample(image, tile_number, x, y) >
         IMAGE_PAINT_SELECTION_MASK_THRESHOLD;
}

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

/**
 * Affine mapping from a brush image-buffer pixel `(x, y)` to a texture sample coordinate:
 * `texco = origin + x * du + y * dv`. The full 2D basis (`du`, `dv`) can encode canvas rotation
 * (and non-uniform scaling), which an axis-aligned `rctf` cannot. With no canvas rotation this
 * reduces to `du = {xmax, 0}`, `dv = {0, ymax}`, `origin = {xmin, ymin}`.
 */
struct BrushImbufMapping {
  float origin[2];
  float du[2];
  float dv[2];
};

struct BrushPainter {
  Scene *scene;
  const Paint *paint;
  Brush *brush;

  /* Store initial starting points for perlin noise on the beginning of each stroke when using
   * color jitter. */
  std::optional<float3> initial_hsv_jitter;

  bool firsttouch; /* first paint op */

  ImagePool *pool; /* image pool */
  /* Affine mapping from a brush image-buffer pixel to a texture sample coordinate. A full 2D
   * basis (rather than an axis-aligned `rctf`) is required so that canvas rotation can be
   * represented; see #brush_imbuf_tex_co. */
  BrushImbufMapping tex_mapping;  /* texture coordinate mapping */
  BrushImbufMapping mask_mapping; /* mask texture coordinate mapping */

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
  /**
   * Dab-pixel → sample-coordinate mapping for the shared source #MTex, built per dab in
   * #brush_painter_2d_refresh_cache and consumed by #paint_2d_sample_channel_source.
   *
   * View / Random / Stencil are produced by #brush_painter_2d_tex_mapping and carry region
   * coordinates. Tiled is built separately: its origin holds the screen-space phase `region / zoom`
   * while `du`/`dv` stay at one dab pixel, because #BKE_brush_sample_tex_3d divides by
   * #PaintRuntime.start_pixel_radius rather than by a zoomed radius.
   */
  BrushImbufMapping source_mapping = {};
  /**
   * Per-dab copies of the sampled source #MTex, remapped to the 2D mapping mode and indexed by
   * #eMaterialPaintChannel. #BKE_brush_sample_tex_3d has no Area Plane branch (AREA would sample
   * (0, 0) on every pixel), so the mode has to be rewritten before sampling; caching the rewrite
   * per dab keeps a ~220 byte struct copy and the mapping-mode lookup out of the per-pixel loop.
   *
   * Refreshed by #paint_2d_source_mtex_2d_update alongside #source_mapping. Only the channels
   * #paint_2d_apply_material_sources actually reads are filled; the rest stay default.
   */
  std::array<MTex, PAINT_MATERIAL_CHANNEL_NUM> source_mtex_2d;
  /**
   * Per-dab direct-sampling plan for each channel in #source_mtex_2d, shared with the Sculpt path
   * so both agree on where a source lands. #DirectSampleKind::Disabled means this channel must go
   * through #BKE_brush_sample_tex_3d instead.
   */
  std::array<ed::sculpt_paint::material::DirectSampleLayout, PAINT_MATERIAL_CHANNEL_NUM>
      source_layouts;
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
  ARegion *region;
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

static void brush_imbuf_tex_co(const BrushImbufMapping *mapping, int x, int y, float texco[3])
{
  texco[0] = mapping->origin[0] + x * mapping->du[0] + y * mapping->dv[0];
  texco[1] = mapping->origin[1] + x * mapping->du[1] + y * mapping->dv[1];
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
 * True when #paint_2d_apply_material_sources will actually overwrite \a rgba's RGB for the
 * painter's active channel, so the brush-texture colorspace encode and Color tint below it
 * must be skipped to avoid being clobbered (or, when this is false, must still run).
 */
static bool paint_2d_material_channel_replaces_rgb(const BrushPainter *painter)
{
  return painter->use_material_channel_color &&
         paint_2d_channel_source_usable_2d(painter, painter->material_channel);
}

/**
 * True when a byte source is already stored in the canvas encoding, so decode-to-linear then
 * encode-to-canvas is an identity. Shared by the View Plane dab batch fill and Area Plane raster.
 */
static bool paint_2d_area_source_matches_canvas_encoding(
    const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source,
    const BrushPainterCache *cache)
{
  if (cache->is_data) {
    return false;
  }
  if (cache->is_srgb) {
    return source.colorspace == nullptr ||
           IMB_colormanagement_space_is_srgb(source.colorspace);
  }
  if (cache->byte_colorspace != nullptr) {
    return source.colorspace == cache->byte_colorspace;
  }
  return false;
}

/**
 * Shared PBR source mapping (not per-channel #source_mtex). Falls back to the sampled channel's
 * effective #MTex when the brush pointer is missing.
 */
static short paint_2d_shared_source_map_mode(const BrushPainter *painter)
{
  if (painter->brush != nullptr && painter->brush->material_paint != nullptr) {
    return painter->brush->material_paint->shared_source_mapping.brush_map_mode;
  }
  if (paint_2d_channel_source_usable_2d(painter, painter->material_channel)) {
    return painter->channel_sources->source(painter->material_channel).mtex->brush_map_mode;
  }
  if (paint_2d_channel_source_usable_2d(painter, PAINT_MATERIAL_CHANNEL_ALPHA)) {
    return painter->channel_sources->source(PAINT_MATERIAL_CHANNEL_ALPHA).mtex->brush_map_mode;
  }
  return MTEX_MAP_MODE_VIEW;
}

/**
 * Refresh #BrushPainter.source_mtex_2d for the channels the next dab will sample.
 *
 * The shared mapping mode is a per-stroke property, and each source #MTex only differs from its
 * stored form by that one field, so the rewrite is hoisted here out of the per-pixel sampler.
 * Call sites must keep this in step with #BrushPainter.source_mapping.
 */
static void paint_2d_source_mtex_2d_update(BrushPainter *painter)
{
  const eMTex_BrushMapMode map_mode = eMTex_BrushMapMode(
      paint_2d_source_map_mode_for_2d(paint_2d_shared_source_map_mode(painter)));
  /* Fill every usable channel so extra material painters can copy the plan instead of rebuilding
   * it. The per-pixel sampler still only reads this painter's own channel and Alpha. */
  for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
    const eMaterialPaintChannel channel = eMaterialPaintChannel(i);
    if (!paint_2d_channel_source_usable_2d(painter, channel)) {
      continue;
    }
    MTex &mtex_2d = painter->source_mtex_2d[channel];
    mtex_2d = dna::shallow_copy(*painter->channel_sources->source(channel).mtex);
    mtex_2d.brush_map_mode = map_mode;

    /* Plan the direct sample against the remapped #MTex, not the stored one, so the layout uses
     * the 2D mapping mode. Area Plane never reaches this sampler, hence the null local matrix. */
    painter->source_layouts[channel] = ed::sculpt_paint::material::make_direct_sample_layout(
        painter->channel_sources->source(channel),
        mtex_2d,
        painter->paint->runtime,
        *painter->brush,
        nullptr);
  }
}

static void paint_2d_painter_copy_shared_source(BrushPainter *dst, const BrushPainter *src)
{
  dst->source_mapping = src->source_mapping;
  dst->tex_mapping = src->tex_mapping;
  dst->mask_mapping = src->mask_mapping;
  for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
    dst->source_mtex_2d[i] = dna::shallow_copy(src->source_mtex_2d[i]);
    dst->source_layouts[i] = src->source_layouts[i];
  }
}

static const CurveMaskCache *paint_2d_matching_curve_mask(const ImagePaintState *shared_state,
                                                          const ImagePaintTile *tile,
                                                          const int diameter)
{
  if (shared_state == nullptr || shared_state->tiles == nullptr) {
    return nullptr;
  }
  const size_t expected = size_t(diameter) * size_t(diameter) * sizeof(ushort);
  for (int i = 0; i < shared_state->num_tiles; i++) {
    const ImagePaintTile *src_tile = &shared_state->tiles[i];
    if (src_tile->size[0] == tile->size[0] && src_tile->size[1] == tile->size[1] &&
        src_tile->cache.curve_mask_cache.curve_mask != nullptr &&
        src_tile->cache.curve_mask_cache.curve_mask_size == expected)
    {
      return &src_tile->cache.curve_mask_cache;
    }
  }
  return nullptr;
}

/**
 * Sample one channel source at a dab-buffer pixel using Image Editor 2D mapping
 * (View / Tiled / Stencil / Random; Area Plane and 3D are remapped to View).
 *
 * Reads the pre-remapped #BrushPainter.source_mtex_2d, so
 * #paint_2d_source_mtex_2d_update must have run for \a channel this dab.
 *
 * \return Texture intensity (mask/factor). RGB is written to \a r_rgba in scene-linear for color
 * sources; Normal skips colorspace decode.
 */
static float paint_2d_sample_channel_source(const BrushPainter *painter,
                                            const eMaterialPaintChannel channel,
                                            const int x,
                                            const int y,
                                            const int thread,
                                            float4 &r_rgba,
                                            const bool decode_linear = true)
{
  const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
      painter->channel_sources->source(channel);
  float3 texco;
  brush_imbuf_tex_co(&painter->source_mapping, x, y, texco);
  ImagePool *pool = painter->channel_sources->pool() != nullptr ?
                        painter->channel_sources->pool() :
                        painter->pool;
  const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
  float intensity;
  /* True when #BKE_brush_sample_tex_3d already ran its own decode below, which rules out the
   * source-level decode that follows. The direct sampler never decodes. */
  bool decoded_by_engine = false;

  const ed::sculpt_paint::material::DirectSampleLayout &layout = painter->source_layouts[channel];
  if (!ed::sculpt_paint::material::sample_direct_layout(
          layout, float3(0.0f), float2(texco.x, texco.y), &intensity, r_rgba))
  {
    const MTex &mtex_2d = painter->source_mtex_2d[channel];
    intensity = BKE_brush_sample_tex_3d(
        painter->paint, painter->brush, &mtex_2d, texco, r_rgba, thread, pool);
    decoded_by_engine = paint_runtime != nullptr && paint_runtime->do_linear_conversion;
  }

  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  /* #BKE_brush_sample_tex_3d decodes with the brush texture's colorspace when that flag is set,
   * which is the wrong space for a per-channel source. Only apply the source's own decode when
   * that brush-level conversion did not already run. */
  if (decode_linear && !is_normal && source.do_linear_conversion && !decoded_by_engine) {
    IMB_colormanagement_colorspace_to_scene_linear_v3(r_rgba, source.colorspace);
  }
  if (source.flip_green_channel) {
    r_rgba[1] = 1.0f - r_rgba[1];
  }
  return intensity;
}

static bool paint_2d_view_dab_material_batch_enabled(const BrushPainter *painter)
{
  if (!painter->use_material_channel_color || painter->channel_sources == nullptr) {
    return false;
  }
  if (paint_2d_channel_source_usable_2d(painter, painter->material_channel)) {
    return true;
  }
  return painter->material_alpha_masking &&
         painter->material_channel != PAINT_MATERIAL_CHANNEL_ALPHA &&
         paint_2d_channel_source_usable_2d(painter, PAINT_MATERIAL_CHANNEL_ALPHA);
}

/**
 * Brush dab coverage for the View Plane path. When a material channel source supplies RGB, the
 * brush texture is still sampled for alpha but its RGB and any post-sample colorspace conversion
 * are unused.
 */
static float paint_2d_view_dab_brush_coverage(const BrushPainter *painter,
                                              const BrushImbufMapping *tex_mapping,
                                              const MTex *mtex,
                                              const bool is_texbrush,
                                              const int x,
                                              const int y,
                                              const int thread,
                                              ImagePool *pool,
                                              float4 &r_brush_rgba)
{
  if (!is_texbrush) {
    return 1.0f;
  }
  float3 texco;
  brush_imbuf_tex_co(tex_mapping, x, y, texco);
  BKE_brush_sample_tex_3d(
      painter->paint, painter->brush, mtex, texco, r_brush_rgba, thread, pool);
  return r_brush_rgba[3];
}

/**
 * Batch fill for View Plane dabs driven by PBR channel sources.
 *
 * Mirrors the Area Plane strategy: sample raw texels in parallel, #decode_linear_batch once, then
 * encode and write. Skips per-pixel OCIO and brush RGB work when the channel source replaces color.
 */
static void paint_2d_view_dab_fill_material_batch(BrushPainter *painter,
                                                  ImagePaintTile *tile,
                                                  const BrushImbufMapping *tex_mapping,
                                                  const float brush_rgb[3],
                                                  const bool is_texbrush,
                                                  const int x0,
                                                  const int y0,
                                                  const int w,
                                                  const int h,
                                                  ImBuf *ibuf,
                                                  ImBuf *texibuf)
{
  const Brush *brush = painter->brush;
  const MTex *brush_mtex = &brush->mtex;
  BrushPainterCache *cache = &tile->cache;
  ImagePool *pool = painter->pool;
  const bool is_float = cache->is_float;
  const int ibuf_stride = ibuf->x;
  const int64_t pixel_num = int64_t(w) * int64_t(h);
  if (pixel_num <= 0) {
    return;
  }

  const eMaterialPaintChannel channel = painter->material_channel;
  const bool channel_usable = paint_2d_channel_source_usable_2d(painter, channel);
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const bool is_color = ELEM(channel,
                             PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                             PAINT_MATERIAL_CHANNEL_EMISSION,
                             PAINT_MATERIAL_CHANNEL_NORMAL);
  const bool alpha_mask = painter->material_alpha_masking &&
                          channel != PAINT_MATERIAL_CHANNEL_ALPHA;
  const bool alpha_usable = alpha_mask &&
                            paint_2d_channel_source_usable_2d(painter,
                                                              PAINT_MATERIAL_CHANNEL_ALPHA);

  Array<float3> colors(pixel_num);
  Array<float> coverages(pixel_num);
  Array<float> alpha_factors(pixel_num);

  threading::parallel_for(IndexRange(h), 8, [&](const IndexRange y_range) {
    const int thread = BLI_task_parallel_thread_id(nullptr);
    float4 brush_rgba;
    for (const int64_t y_index : y_range) {
      const int y = y0 + int(y_index);
      for (int x = x0; x < x0 + w; x++) {
        const int64_t i = y_index * w + (x - x0);
        float coverage = paint_2d_view_dab_brush_coverage(
            painter, tex_mapping, brush_mtex, is_texbrush, x, y, thread, pool, brush_rgba);

        if (channel_usable) {
          float4 sampled;
          const float intensity = paint_2d_sample_channel_source(
              painter, channel, x, y, thread, sampled, false);
          if (is_color) {
            colors[i] = float3(sampled[0], sampled[1], sampled[2]);
          }
          else {
            colors[i] = float3(intensity, intensity, intensity);
          }
        }
        else {
          colors[i] = float3(brush_rgb[0], brush_rgb[1], brush_rgb[2]);
        }

        if (alpha_usable) {
          float4 alpha_sampled;
          alpha_factors[i] = paint_2d_sample_channel_source(painter,
                                                            PAINT_MATERIAL_CHANNEL_ALPHA,
                                                            x,
                                                            y,
                                                            thread,
                                                            alpha_sampled,
                                                            false);
        }
        else if (alpha_mask) {
          alpha_factors[i] = painter->material_alpha_fallback;
        }

        coverages[i] = coverage;
      }
    }
  });

  if (channel_usable) {
    const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
        painter->channel_sources->source(channel);
    const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
    const bool batch_decode = !is_normal && source.do_linear_conversion &&
                              (paint_runtime == nullptr || !paint_runtime->do_linear_conversion);
    const bool skip_colorspace = batch_decode &&
                                 paint_2d_area_source_matches_canvas_encoding(source, cache);
    if (batch_decode && !skip_colorspace) {
      ed::sculpt_paint::material::ChannelSourceSampler::decode_linear_batch(colors,
                                                                            source.colorspace);
    }
    /* #paint_2d_sample_channel_source already flips green per-texel above; flipping the whole
     * array again here would cancel it back out. */
    if (!skip_colorspace && !is_normal && !cache->is_data && !is_float &&
        cache->byte_colorspace != nullptr)
    {
      IMB_colormanagement_scene_linear_to_colorspace(reinterpret_cast<float *>(colors.data()),
                                                     w,
                                                     h,
                                                     3,
                                                     cache->byte_colorspace);
    }
  }

  uchar *ibuf_byte_data = ibuf->byte_data_for_write();
  float *ibuf_float_data = ibuf->float_data_for_write();
  uchar *texibuf_byte_data = texibuf != nullptr ? texibuf->byte_data_for_write() : nullptr;
  float *texibuf_float_data = texibuf != nullptr ? texibuf->float_data_for_write() : nullptr;

  threading::parallel_for(IndexRange(pixel_num), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const int local_x = int(i % w);
      const int local_y = int(i / w);
      const int x = x0 + local_x;
      const int y = y0 + local_y;
      const int buf_idx = y * ibuf_stride + x;

      float coverage = coverages[i];
      if (alpha_mask) {
        coverage *= math::clamp(alpha_factors[i], 0.0f, 1.0f);
      }

      float4 rgba(colors[i].x, colors[i].y, colors[i].z, coverage);

      if (is_float) {
        float *bf = ibuf_float_data + buf_idx * 4;
        mul_v3_v3fl(bf, rgba, rgba[3]);
        bf[3] = rgba[3];
        if (texibuf_float_data != nullptr) {
          copy_v4_v4(texibuf_float_data + buf_idx * 4, rgba);
        }
      }
      else {
        uchar crgba[4];
        rgb_float_to_uchar(crgba, rgba);
        crgba[3] = unit_float_to_uchar_clamp(rgba[3]);

        ibuf_byte_data[buf_idx * 4 + 0] = crgba[0];
        ibuf_byte_data[buf_idx * 4 + 1] = crgba[1];
        ibuf_byte_data[buf_idx * 4 + 2] = crgba[2];
        ibuf_byte_data[buf_idx * 4 + 3] = crgba[3];
        if (texibuf_byte_data != nullptr) {
          texibuf_byte_data[buf_idx * 4 + 0] = crgba[0];
          texibuf_byte_data[buf_idx * 4 + 1] = crgba[1];
          texibuf_byte_data[buf_idx * 4 + 2] = crgba[2];
          texibuf_byte_data[buf_idx * 4 + 3] = crgba[3];
        }
      }
    }
  });
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
  BrushImbufMapping mask_mapping = painter->mask_mapping;
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
  BrushImbufMapping tex_mapping = painter->mask_mapping;
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
#if PBR_PAINT_2D_DAB_PROFILE
/**
 * Dab-fill totals for the current stroke, reported and reset by #paint_2d_stroke_done.
 *
 * Plain statics rather than atomics: dabs are issued from the main thread and the parallel work
 * inside a dab is joined before these are touched, so there is no concurrent update.
 */
static double g_dab_fill_seconds = 0.0;
static int64_t g_dab_fill_pixels = 0;
static int g_dab_fill_calls = 0;
#endif

#if PBR_PAINT_2D_STROKE_PROFILE
/**
 * Cross-section of one stroke, reported and reset by #paint_2d_stroke_done.
 *
 * Same main-thread rule as the dab-fill counters: parallel work inside a phase is joined before
 * these are touched.
 */
static double g_stroke_wall_seconds = 0.0;
static int g_stroke_wall_calls = 0;
static double g_stroke_single_seconds = 0.0;
static int g_stroke_single_calls = 0;
static double g_stroke_op_seconds = 0.0;
static int g_stroke_op_calls = 0;
static int64_t g_stroke_op_pixels = 0;
static double g_stroke_dab_fill_seconds = 0.0;
static int g_stroke_dab_fill_calls = 0;
static int64_t g_stroke_dab_fill_pixels = 0;
static double g_stroke_area_seconds = 0.0;
static int g_stroke_area_calls = 0;
static int64_t g_stroke_area_pixels = 0;
static double g_stroke_redraw_seconds = 0.0;
static int g_stroke_redraw_calls = 0;
static int g_stroke_painters = 0;
static int g_stroke_view_path_calls = 0;
static int g_stroke_area_path_calls = 0;
static int g_stroke_imbuf_new = 0;
static int g_stroke_imbuf_update = 0;
static int g_stroke_imbuf_partial = 0;
static double g_stroke_curve_mask_seconds = 0.0;
static int g_stroke_curve_mask_calls = 0;
static double g_stroke_source_update_seconds = 0.0;
static int g_stroke_source_update_calls = 0;
static bool g_stroke_meta_logged = false;

/** Scoped timer accumulating into a seconds/calls pair. */
struct StrokePhaseTimer {
  double *accum;
  int *calls;
  double start = BLI_time_now_seconds();

  StrokePhaseTimer(double *accum_, int *calls_) : accum(accum_), calls(calls_) {}
  ~StrokePhaseTimer()
  {
    *this->accum += BLI_time_now_seconds() - this->start;
    (*this->calls)++;
  }
};

static const char *paint_2d_direct_sample_kind_name(
    const ed::sculpt_paint::material::DirectSampleKind kind)
{
  using Kind = ed::sculpt_paint::material::DirectSampleKind;
  switch (kind) {
    case Kind::Disabled:
      return "Disabled";
    case Kind::Area:
      return "Area";
    case Kind::View:
      return "View";
    case Kind::Tiled:
      return "Tiled";
    case Kind::Random:
      return "Random";
    case Kind::Stencil:
      return "Stencil";
  }
  return "?";
}

static const char *paint_2d_map_mode_name(const short map_mode)
{
  switch (map_mode) {
    case MTEX_MAP_MODE_VIEW:
      return "view";
    case MTEX_MAP_MODE_TILED:
      return "tiled";
    case MTEX_MAP_MODE_3D:
      return "3d";
    case MTEX_MAP_MODE_AREA:
      return "area";
    case MTEX_MAP_MODE_STENCIL:
      return "stencil";
    case MTEX_MAP_MODE_RANDOM:
      return "random";
  }
  return "?";
}
#endif

#if PBR_PAINT_2D_DAB_PROFILE || PBR_PAINT_2D_STROKE_PROFILE
/** Scoped timer accumulating dab-fill into whichever profile counters are compiled in. */
struct DabFillTimer {
  double start = BLI_time_now_seconds();
  int64_t pixels = 0;

  explicit DabFillTimer(const int64_t pixel_num) : pixels(pixel_num) {}
  ~DabFillTimer()
  {
    const double elapsed = BLI_time_now_seconds() - this->start;
#if PBR_PAINT_2D_DAB_PROFILE
    g_dab_fill_seconds += elapsed;
    g_dab_fill_pixels += this->pixels;
    g_dab_fill_calls++;
#endif
#if PBR_PAINT_2D_STROKE_PROFILE
    g_stroke_dab_fill_seconds += elapsed;
    g_stroke_dab_fill_pixels += this->pixels;
    g_stroke_dab_fill_calls++;
#endif
  }
};
#endif

static void brush_painter_imbuf_fill(BrushPainter *painter,
                                     ImagePaintTile *tile,
                                     ImBuf *ibuf,
                                     float pressure,
                                     float distance)
{
  const Paint *paint = painter->paint;
  Brush *brush = painter->brush;
  BrushPainterCache *cache = &tile->cache;

  BrushImbufMapping tex_mapping = painter->tex_mapping;
  ImagePool *pool = painter->pool;

  const bool is_float = cache->is_float;
  const bool is_texbrush = cache->is_texbrush;
  const int size = ibuf->x;

  float brush_rgb[3];

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
#if PBR_PAINT_2D_DAB_PROFILE || PBR_PAINT_2D_STROKE_PROFILE
  const DabFillTimer fill_timer(int64_t(size) * int64_t(size));
#endif
  if (paint_2d_view_dab_material_batch_enabled(painter)) {
    paint_2d_view_dab_fill_material_batch(painter,
                                          tile,
                                          &tex_mapping,
                                          brush_rgb,
                                          is_texbrush,
                                          0,
                                          0,
                                          size,
                                          size,
                                          ibuf,
                                          nullptr);
    return;
  }
  /* Rows are independent: each writes its own slice of `ibuf`, and the shared #ImagePool guards
   * its own lookups. Sampling dominates the cost, so the grain stays small. */
  threading::parallel_for(IndexRange(size), 8, [&](const IndexRange y_range) {
    const int thread = BLI_task_parallel_thread_id(nullptr);
    for (const int64_t y_index : y_range) {
      const int y = int(y_index);
      for (int x = 0; x < size; x++) {
        /* sample texture and multiply with brush color */
        float3 texco;
        float4 rgba;

        if (is_texbrush) {
          brush_imbuf_tex_co(&tex_mapping, x, y, texco);
          const MTex *mtex = &brush->mtex;
          BKE_brush_sample_tex_3d(painter->paint, brush, mtex, texco, rgba, thread, pool);
          if (!paint_2d_material_channel_replaces_rgb(painter)) {
            if (cache->is_srgb) {
              IMB_colormanagement_scene_linear_to_srgb_v3(rgba, rgba);
            }
            else if (cache->byte_colorspace) {
              IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, cache->byte_colorspace);
            }

            /* Classic Image Paint tints the brush texture with Color. PBR Paint matches Sculpt:
             * a source (or the texture itself) is the color, the Color slider does not multiply. */
            mul_v3_v3(rgba, brush_rgb);
          }
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
  });
}

static ImBuf *brush_painter_imbuf_new(
    BrushPainter *painter, ImagePaintTile *tile, const int size, float pressure, float distance)
{
  const bool is_float = tile->cache.is_float;
  ImBuf *ibuf = IMB_allocImBuf(
      size, size, (is_float) ? ImBufFlags::FloatData : ImBufFlags::ByteData);
  brush_painter_imbuf_fill(painter, tile, ibuf, pressure, distance);
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

  BrushImbufMapping tex_mapping = painter->tex_mapping;
  ImagePool *pool = painter->pool;

  const bool is_float = cache->is_float;
  const bool is_texbrush = cache->is_texbrush;
  const bool use_texture_old = (oldtexibuf != nullptr);

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
  /* `oldtexibuf` is null when sampling brand-new areas (see callers); these pointers are only read
   * under `use_texture_old`, so guard the access to avoid dereferencing null. */
  const uchar *oldtexibuf_byte_data = use_texture_old ? oldtexibuf->byte_data() : nullptr;
  const float *oldtexibuf_float_data = use_texture_old ? oldtexibuf->float_data() : nullptr;
#if PBR_PAINT_2D_DAB_PROFILE || PBR_PAINT_2D_STROKE_PROFILE
  const DabFillTimer fill_timer(int64_t(std::max(0, w - origx)) * int64_t(std::max(0, h - origy)));
#endif
  const int fill_w = std::max(0, w - origx);
  const int fill_h = std::max(0, h - origy);
  if (!use_texture_old && paint_2d_view_dab_material_batch_enabled(painter)) {
    paint_2d_view_dab_fill_material_batch(painter,
                                          tile,
                                          &tex_mapping,
                                          brush_rgb,
                                          is_texbrush,
                                          origx,
                                          origy,
                                          fill_w,
                                          fill_h,
                                          ibuf,
                                          texibuf);
    return;
  }
  /* Rows are independent: each writes its own slice of `ibuf` / `texibuf`, and the shared
   * #ImagePool guards its own lookups. Sampling dominates the cost, so the grain stays
   * small. The empty-range case matches the original loop, which simply did not run. */
  threading::parallel_for(
      IndexRange(origy, fill_h), 8, [&](const IndexRange y_range) {
        const int thread = BLI_task_parallel_thread_id(nullptr);
        for (const int64_t y_index : y_range) {
          const int y = int(y_index);
          for (int x = origx; x < w; x++) {
            /* sample texture and multiply with brush color */
            float3 texco;
            float4 rgba;

            if (!use_texture_old) {
              if (is_texbrush) {
                brush_imbuf_tex_co(&tex_mapping, x, y, texco);
                BKE_brush_sample_tex_3d(painter->paint, brush, mtex, texco, rgba, thread, pool);
                if (!paint_2d_material_channel_replaces_rgb(painter)) {
                  if (cache->is_srgb) {
                    IMB_colormanagement_scene_linear_to_srgb_v3(rgba, rgba);
                  }
                  else if (cache->byte_colorspace) {
                    IMB_colormanagement_scene_linear_to_colorspace_v3(rgba, cache->byte_colorspace);
                  }

                  /* Classic Image Paint tints the brush texture with Color. PBR Paint matches Sculpt:
                   * a source (or the texture itself) is the color, the Color slider does not multiply. */
                  mul_v3_v3(rgba, brush_rgb);
                }
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
                /* Brush coverage/falloff is reused; channel sources are dab-dependent. */
                paint_2d_apply_material_sources(painter, cache, x, y, thread, rgba);
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
                rgba_uchar_to_float(rgba, crgba);
                paint_2d_apply_material_sources(painter, cache, x, y, thread, rgba);
                rgba_float_to_uchar(crgba, rgba);
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
      });
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

/**
 * Map a dab's canvas-pixel origin to Image Editor region coordinates.
 *
 * The Stencil overlay is drawn in region pixels (`stencil_pos`, the region rect) and
 * #BKE_brush_sample_tex_3d expects that same space, so the Stencil basis is derived from
 * #ED_image_point_pos__reverse: sampling it at the dab origin and at one canvas pixel along each
 * axis makes `du`/`dv` follow the displayed image, canvas rotation included.
 *
 * The Tiled branch below serves the classic brush texture and stays in canvas pixels. The PBR
 * shared source needs the screen-space phase of the overlay instead, so it builds its own Tiled
 * mapping in #brush_painter_2d_refresh_cache and never reaches this function.
 *
 * \param r_mapping: Affine map from dab-buffer pixel (x, y) to sample coordinates.
 */
static void brush_painter_2d_tex_mapping(ImagePaintState *s,
                                         ImagePaintTile *tile,
                                         const int diameter,
                                         const float pos[2],
                                         const float mouse[2],
                                         int mapmode,
                                         BrushImbufMapping *r_mapping)
{
  const float invw = 1.0f / float(tile->canvas->x);
  const float invh = 1.0f / float(tile->canvas->y);
  const float center = diameter * 0.5f;
  float start[2];

  /* find start coordinate of brush in canvas */
  start[0] = pos[0] - diameter / 2.0f;
  start[1] = pos[1] - diameter / 2.0f;

  if (mapmode == MTEX_MAP_MODE_STENCIL) {
    /* The stencil texture is pinned to a fixed region position (`stencil_pos`) with region-space
     * dimensions, so each brush pixel must sample at its true region coordinate (region pixels,
     * zoom included). Deriving the basis from #ED_image_point_pos__reverse makes the mapping
     * rotation- and zoom-aware exactly like the displayed image (a rotated canvas yields a rotated
     * `du`/`dv`). */
    auto uv_to_region = [&](const float cx, const float cy, float r_out[2]) {
      const float uv[2] = {cx * invw, cy * invh};
      ED_image_point_pos__reverse(s->sima, s->region, uv, r_out);
    };
    float origin[2], step_x[2], step_y[2];
    uv_to_region(start[0], start[1], origin);
    uv_to_region(start[0] + 1.0f, start[1], step_x);
    uv_to_region(start[0], start[1] + 1.0f, step_y);

    r_mapping->du[0] = step_x[0] - origin[0];
    r_mapping->du[1] = step_x[1] - origin[1];
    r_mapping->dv[0] = step_y[0] - origin[0];
    r_mapping->dv[1] = step_y[1] - origin[1];
    /* Offset for the tile origin (UDIM), expressed along the mapping basis. */
    const float off_u = tile->uv_origin[0] * tile->size[0];
    const float off_v = tile->uv_origin[1] * tile->size[1];
    r_mapping->origin[0] = origin[0] + off_u * r_mapping->du[0] + off_v * r_mapping->dv[0];
    r_mapping->origin[1] = origin[1] + off_u * r_mapping->du[1] + off_v * r_mapping->dv[1];
    return;
  }

  /* Canvas rotation is applied to the sample lattice as `Rot(-canvas_rotation)` (the image is
   * displayed as `Rot(-rotation)`), so the painted texture stays locked to the view. */
  const float canvas_rotation = (s->sima) ? s->sima->rotation : 0.0f;
  float cos_a = 1.0f, sin_a = 0.0f;
  if (canvas_rotation != 0.0f) {
    cos_a = cosf(canvas_rotation);
    sin_a = -sinf(canvas_rotation);
  }

  if (mapmode == MTEX_MAP_MODE_TILED) {
    /* Tiled repeats across the view (matching the overlay). Keep the canvas-pixel scale (as the
     * unrotated code does) but rotate the unit lattice into view space and anchor it globally to
     * the stroke start, so the mapping is a single consistent function of the canvas pixel. A
     * per-dab (`pos`-dependent) anchor would otherwise shift the tile phase between dabs and shear
     * the pattern. Composed with the display rotation this yields a screen-locked tiling. */
    r_mapping->du[0] = cos_a;
    r_mapping->du[1] = sin_a;
    r_mapping->dv[0] = -sin_a;
    r_mapping->dv[1] = cos_a;
    /* `texco = Rot(-a) * (P - floor(start_paintpos))`, with `P = start + (x, y)`. */
    const float anchor[2] = {start[0] - floorf(tile->start_paintpos[0]),
                             start[1] - floorf(tile->start_paintpos[1])};
    r_mapping->origin[0] = r_mapping->du[0] * anchor[0] + r_mapping->dv[0] * anchor[1];
    r_mapping->origin[1] = r_mapping->du[1] * anchor[0] + r_mapping->dv[1] * anchor[1];
    return;
  }

  /* VIEW/RANDOM re-anchor to the mouse each dab; 3D is canvas-space (no overlay). Both use an
   * axis-aligned linear mapping `texco = {xmin, ymin} + (x, y) * {xmax, ymax}`, with the lattice
   * rotated about the brush center for the view-locked modes. */
  float xmin, ymin, xmax, ymax;
  if (mapmode == MTEX_MAP_MODE_3D) {
    /* 3D mapping, just mapping to canvas 0..1. Canvas-space, so no rotation. */
    xmin = 2.0f * (start[0] * invw - 0.5f);
    ymin = 2.0f * (start[1] * invh - 0.5f);
    xmax = 2.0f * invw;
    ymax = 2.0f * invh;
    cos_a = 1.0f;
    sin_a = 0.0f;
  }
  else /* MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_RANDOM */ {
    xmin = mouse[0] - diameter * 0.5f + 0.5f;
    ymin = mouse[1] - diameter * 0.5f + 0.5f;
    xmax = 1.0f;
    ymax = 1.0f;
  }

  r_mapping->du[0] = cos_a * xmax;
  r_mapping->du[1] = sin_a * xmax;
  r_mapping->dv[0] = -sin_a * ymax;
  r_mapping->dv[1] = cos_a * ymax;

  /* Rotate the lattice about the brush center: `texco = center_texco + (p - center) * basis`. */
  const float center_texco[2] = {xmin + center * xmax, ymin + center * ymax};
  r_mapping->origin[0] = center_texco[0] - center * (r_mapping->du[0] + r_mapping->dv[0]);
  r_mapping->origin[1] = center_texco[1] - center * (r_mapping->du[1] + r_mapping->dv[1]);
}

static void brush_painter_2d_refresh_cache(ImagePaintState *s,
                                           BrushPainter *painter,
                                           ImagePaintTile *tile,
                                           const float pos[2],
                                           const float mouse[2],
                                           float pressure,
                                           float distance,
                                           float size,
                                           const ImagePaintState *shared_state)
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

  painter->pool = painter->pool ? painter->pool : BKE_image_pool_new();

  const bool use_2d_channel_source =
      paint_2d_channel_source_usable_2d(painter, painter->material_channel);
  const bool copy_shared_source = shared_state != nullptr && shared_state->painter != nullptr &&
                                  painter->channel_sources != nullptr &&
                                  painter->use_material_channel_color &&
                                  paint_2d_matching_curve_mask(shared_state, tile, diameter) !=
                                      nullptr;
  if (copy_shared_source) {
#if PBR_PAINT_2D_STROKE_PROFILE
    const StrokePhaseTimer source_timer(&g_stroke_source_update_seconds,
                                        &g_stroke_source_update_calls);
#endif
    paint_2d_painter_copy_shared_source(painter, shared_state->painter);
  }
  else if (painter->channel_sources != nullptr && painter->use_material_channel_color &&
           (use_2d_channel_source ||
            paint_2d_channel_source_usable_2d(painter, PAINT_MATERIAL_CHANNEL_ALPHA)))
  {
#if PBR_PAINT_2D_STROKE_PROFILE
    const StrokePhaseTimer source_timer(&g_stroke_source_update_seconds,
                                        &g_stroke_source_update_calls);
#endif
    const short source_map_mode = paint_2d_source_map_mode_for_2d(
        paint_2d_shared_source_map_mode(painter));
    if (source_map_mode == MTEX_MAP_MODE_TILED) {
      /* The Tiled overlay evaluates `region / (brush_radius * zoom)`, while
       * #BKE_brush_sample_tex_3d evaluates `texco / start_pixel_radius`. The dab center needs
       * the screen-space phase `region / zoom`, but neighboring dab pixels are one canvas pixel
       * apart, so their texco step is one (not `1 / zoom`). Tiled is locked to the screen-space
       * overlay, not the rotated image canvas.
       *
       * The dab is sized in this tile's texels (`diameter` already carries #radius_fac) while the
       * phase term is in tile-zero texels, so the step is divided by #radius_fac to keep both in
       * the same units on UDIM sets that mix resolutions. */
      float zoomx = 1.0f;
      float zoomy = 1.0f;
      ED_space_image_get_zoom(s->sima, s->region, &zoomx, &zoomy);
      const float inv_zoom = 1.0f / max_ff(zoomx, zoomy);
      const float inv_radius_fac = (tile->radius_fac > 0.0f) ? 1.0f / tile->radius_fac : 1.0f;
      const float start = diameter * 0.5f;
      painter->source_mapping.origin[0] = mouse[0] * inv_zoom - (start - 0.5f) * inv_radius_fac;
      painter->source_mapping.origin[1] = mouse[1] * inv_zoom - (start - 0.5f) * inv_radius_fac;
      painter->source_mapping.du[0] = inv_radius_fac;
      painter->source_mapping.du[1] = 0.0f;
      painter->source_mapping.dv[0] = 0.0f;
      painter->source_mapping.dv[1] = inv_radius_fac;
    }
    else {
      brush_painter_2d_tex_mapping(
          s, tile, diameter, pos, mouse, source_map_mode, &painter->source_mapping);
    }
    paint_2d_source_mtex_2d_update(painter);
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

    if (!copy_shared_source) {
      brush_painter_2d_tex_mapping(
          s, tile, diameter, pos, mouse, brush->mtex.brush_map_mode, &painter->tex_mapping);
    }
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

  /* Re-initialize the curve mask. Mask is always recreated due to the change of position.
   * Extra material painters share the primary's rasterized mask when the dab diameter matches. */
#if PBR_PAINT_2D_STROKE_PROFILE
  {
    const StrokePhaseTimer curve_timer(&g_stroke_curve_mask_seconds, &g_stroke_curve_mask_calls);
#endif
    const CurveMaskCache *shared_mask = paint_2d_matching_curve_mask(shared_state, tile, diameter);
    const bool has_selection_mask = BKE_image_paint_selection_mask_has_any(s->image);
    const bool shared_mask_has_selection = shared_state != nullptr &&
                                           BKE_image_paint_selection_mask_has_any(shared_state->image);
    if (shared_mask != nullptr && !has_selection_mask && !shared_mask_has_selection) {
      paint_curve_mask_cache_copy(&cache->curve_mask_cache, shared_mask);
    }
    else {
      paint_curve_mask_cache_update(&cache->curve_mask_cache, brush, diameter, size, pos);
    }
#if PBR_PAINT_2D_STROKE_PROFILE
  }
#endif

  /* Apply selection mask to the curve mask in-place after it has been recreated. The curve mask
   * must not be shared after this step: every material target may use a different image selection. */
  if (has_selection_mask) {
    const Scene *scene = s->scene;
    const int tile_number = tile->iuser.tile;
    const ImBuf *sel_mask = BKE_image_paint_selection_mask_lookup(
        const_cast<const Image *>(s->image), tile_number);
    if (!sel_mask) {
      memset(cache->curve_mask_cache.curve_mask, 0, size_t(diameter) * diameter * sizeof(ushort));
    }
    else {
      const int brush_origin_xi = int(floorf(pos[0] - float(diameter / 2)));
      const int brush_origin_yi = int(floorf(pos[1] - float(diameter / 2)));
      ushort *curve_mask = cache->curve_mask_cache.curve_mask;
      for (int y = 0; y < diameter; y++) {
        const int my = brush_origin_yi + y;
        for (int x = 0; x < diameter; x++, curve_mask++) {
          if (*curve_mask == 0) {
            continue;
          }
          const int mx = brush_origin_xi + x;
          *curve_mask = ushort(float(*curve_mask) * paint_2d_selection_blend_sample_bilinear(
                                                   scene,
                                                   s->image,
                                                   tile_number,
                                                   float(mx) + 0.5f,
                                                   float(my) + 0.5f));
        }
      }
    }
  }

  /* detect if we need to recreate image brush buffer */
  const bool stamp_shape_changed = diameter != cache->lastdiameter ||
                                   (tex_rotation != cache->last_tex_rotation) || do_random ||
                                   update_color;
  if (stamp_shape_changed || do_source_rebuild)
  {
    if (do_partial_update) {
      if (stamp_shape_changed && cache->ibuf) {
        IMB_freeImBuf(cache->ibuf);
        cache->ibuf = nullptr;
      }
      /* do partial update of texture; material sources are resampled on the reused overlap */
      brush_painter_imbuf_partial_update(painter, tile, pos, diameter);
#if PBR_PAINT_2D_STROKE_PROFILE
      g_stroke_imbuf_partial++;
#endif
    }
    else if (cache->ibuf != nullptr && cache->ibuf->x == diameter && cache->ibuf->y == diameter) {
      /* View Plane PBR sources change with the cursor, so pixels must be resampled every dab.
       * The stamp size itself usually does not: reuse the existing #ImBuf instead of allocating
       * a new one (the previous path required #texibuf, which #brush_painter_imbuf_new never
       * creates, so every dab leaked the old buffer and paid a full alloc). */
      brush_painter_imbuf_fill(painter, tile, cache->ibuf, pressure, distance);
#if PBR_PAINT_2D_STROKE_PROFILE
      g_stroke_imbuf_update++;
#endif
    }
    else {
      if (cache->ibuf) {
        IMB_freeImBuf(cache->ibuf);
        cache->ibuf = nullptr;
      }
      /* create brush from scratch */
      cache->ibuf = brush_painter_imbuf_new(painter, tile, diameter, pressure, distance);
#if PBR_PAINT_2D_STROKE_PROFILE
      g_stroke_imbuf_new++;
#endif
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
#if PBR_PAINT_2D_STROKE_PROFILE
      g_stroke_imbuf_partial++;
#endif
    }
  }
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
#if PBR_PAINT_2D_STROKE_PROFILE
  const StrokePhaseTimer op_timer(&g_stroke_op_seconds, &g_stroke_op_calls);
#endif
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

#if PBR_PAINT_2D_STROKE_PROFILE
  for (int region_i = 0; region_i < tot; region_i++) {
    g_stroke_op_pixels += int64_t(std::max(0, region[region_i].width)) *
                          int64_t(std::max(0, region[region_i].height));
  }
#endif

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

static bool paint_2d_use_area_plane_mesh(const BrushPainter *painter)
{
  return painter->area_plane_mesh != nullptr && painter->area_plane_mesh->is_valid();
}

static bool paint_2d_use_area_plane(const BrushPainter *painter)
{
  if (!painter->use_material_channel_color || !paint_2d_use_area_plane_mesh(painter)) {
    return false;
  }

  if (painter->brush == nullptr) {
    return false;
  }

  /* The mesh can also exist solely for symmetry. It must not redirect a screen-space PBR source
   * (View, Tiled, Stencil, or Random) through the Area Plane rasterizer: those sources are sampled
   * by the regular 2D dab path so their coordinates match the cursor overlay. */
  if (painter->channel_sources != nullptr || painter->brush->material_paint != nullptr) {
    return paint_2d_shared_source_map_mode(painter) == MTEX_MAP_MODE_AREA;
  }

  /* A stroke with neither channel sources nor material settings is driven by the classic brush
   * texture alone. Without such a texture there is no screen-space mapping to preserve, so keep
   * the Area Plane path it used before. */
  return painter->brush->mtex.tex == nullptr ||
         painter->brush->mtex.brush_map_mode == MTEX_MAP_MODE_AREA;
}

static void paint_2d_sample_area_mtex(
    const BrushPainter *painter,
    const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source,
    const float4x4 &local_mat,
    const float3 &position,
    const int thread,
    ImagePool *pool,
    float *r_value,
    float4 &r_rgba)
{
  float3 point = position;
  mul_m4_v3(local_mat.ptr(), point);
  const MTex *mtex = source.mtex;
  const float tex_x = point.x * mtex->size[0] + mtex->ofs[0];
  const float tex_y = point.y * mtex->size[1] + mtex->ofs[1];
  if (!painter->channel_sources->sample_image_direct(source, tex_x, tex_y, r_value, r_rgba)) {
    paint_get_tex_pixel(mtex, tex_x, tex_y, pool, thread, r_value, r_rgba);
  }
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
    const float radius_object,
    const bool mirrored,
    const float canvas_rotation)
{
  float rotation = 0.0f;
  if (paint_2d_channel_source_usable_2d(painter, channel)) {
    rotation = painter->channel_sources->source(channel).mtex->rot;
  }
  /* The Area Plane axes follow the triangle's UV (`dpdu`/`dpdv`), which the canvas rotation then
   * turns by `-canvas_rotation` on screen. Adding it back keeps the stamp locked to the view, so
   * what the brush cursor shows is what lands on the canvas: the same guarantee the View and Tiled
   * lattices get in #brush_painter_2d_tex_mapping. */
  rotation += canvas_rotation;
  return ed::sculpt_paint::area_plane_frame_from_triangle(
      tri, position, radius_object, rotation, mirrored);
}

static void paint_2d_area_sample_channel_color(const BrushPainter *painter,
                                               const BrushPainterCache *cache,
                                               const eMaterialPaintChannel channel,
                                               const float4x4 &local_mat,
                                               const float3 &position,
                                               const int thread,
                                               ImagePool *pool,
                                               const PaintModeSettings &paint_mode,
                                               const bool decode_linear,
                                               const bool encode_canvas,
                                               const bool apply_flip,
                                               float3 &r_rgb)
{
  if (!paint_2d_channel_source_usable_2d(painter, channel)) {
    r_rgb = float3(painter->material_channel_color[0],
                   painter->material_channel_color[1],
                   painter->material_channel_color[2]);
    if (encode_canvas) {
      paint_2d_area_encode_canvas_rgb(cache, channel == PAINT_MATERIAL_CHANNEL_NORMAL, r_rgb);
    }
    return;
  }

  const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
      painter->channel_sources->source(channel);
  float value;
  float4 sampled;
  paint_2d_sample_area_mtex(painter, source, local_mat, position, thread, pool, &value, sampled);

  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
  if (decode_linear && !is_normal && source.do_linear_conversion &&
      (paint_runtime == nullptr || !paint_runtime->do_linear_conversion))
  {
    IMB_colormanagement_colorspace_to_scene_linear_v3(sampled, source.colorspace);
  }
  if (apply_flip && source.flip_green_channel) {
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
  if (encode_canvas) {
    paint_2d_area_encode_canvas_rgb(cache, is_normal, r_rgb);
  }
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
                              painter->channel_sources->source(PAINT_MATERIAL_CHANNEL_ALPHA),
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
                                       const short blend,
                                       const int width,
                                       const int height)
{
  ImagePaintRegion region;
  paint_2d_set_region(&region, destx, desty, 0, 0, width, height);
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

/* The mirrored point misses the surface by however asymmetric the mesh is, so the tolerance
 * scales with the mesh, never with the brush or the destination image: one mirrored lookup is
 * shared by material channels that may paint into images of different sizes. */
static constexpr float AREA_PLANE_SYMMETRY_SNAP_FACTOR = 1.0f;
/* A mirror this close to the original writes the same texels twice, darkening the seam. */
static constexpr float AREA_PLANE_SYMMETRY_MERGE_FACTOR = 0.25f;
/* Beyond this ratio the mirrored stroke jumped UV islands; interpolating would drag a band
 * across empty UV space. */
static constexpr float AREA_PLANE_SYMMETRY_ISLAND_JUMP_FACTOR = 4.0f;

/**
 * One covered texel of an Area Plane dab. Falloff, alpha and the unfolded sample position are
 * independent of the destination channel, so extras reuse this list and only sample RGB.
 */
struct AreaPlaneCoveragePixel {
  float3 sample_position = {};
  uint16_t lx = 0;
  uint16_t ly = 0;
  uint16_t falloff = 0;
  uint16_t alpha = 65535;
};

struct AreaPlaneTriCoverage {
  int x0 = 0;
  int y0 = 0;
  int w = 0;
  int h = 0;
  Vector<AreaPlaneCoveragePixel> pixels;
};

struct AreaPlaneTileCoverage {
  int size[2] = {0, 0};
  float uv_origin[2] = {0.0f, 0.0f};
  Vector<AreaPlaneTriCoverage> tris;
};

struct AreaPlaneImBufDeleter {
  void operator()(ImBuf *ibuf) const
  {
    if (ibuf) {
      IMB_freeImBuf(ibuf);
    }
  }
};

struct AreaPlaneDabGeom {
  bool valid = false;
  ed::sculpt_paint::AreaPlaneHit hit;
  float radius_object = 0.0f;
  float4x4 object_to_brush = float4x4::identity();
  Vector<int> accepted;
  Vector<float4x4> unfold_mats;
  float3 dab_normal = {};
  ed::sculpt_paint::AreaPlaneTriangle dab_tri;
  /** Mirrored dab: the stamp is reflected, not just repositioned. */
  bool flipped = false;
  /** Built on the first destination; extra maps reuse it when the tile grid matches. */
  Vector<AreaPlaneTileCoverage> coverage_tiles;
  /**
   * Grown to the largest triangle AABB this dab has rasterized. Extra channels reuse it instead
   * of allocating a new ImBuf per triangle. #IMB_rectblend uses ImBuf.x as row stride, so writes
   * must use that stride even when the used sub-rect is smaller.
   */
  std::unique_ptr<ImBuf, AreaPlaneImBufDeleter> scratch_float;
  std::unique_ptr<ImBuf, AreaPlaneImBufDeleter> scratch_byte;
  Vector<ushort> scratch_mask;
};

/**
 * One entry of the symmetry fan, shared by every material channel of this stroke.
 * Index in the owning vector is the symmetry iteration (0 = the original dab).
 *
 * Holds only what depends on the mesh and the cursor, never anything sized by the destination
 * image: #AreaPlaneDabGeom is rebuilt per state because its radius comes from that state's
 * first tile.
 */
struct SymmetryDab {
  /** Evaluated already; channels after the first must not recompute it. */
  bool computed = false;
  /** The mirrored point landed on the surface within tolerance. */
  bool valid = false;
  /** This iteration mirrors an odd number of axes, so the stamp is flipped. */
  bool flipped = false;
  /** Mirrored dab center, Area Plane path. */
  ed::sculpt_paint::AreaPlaneHit hit;
  /** Mirrored dab center in UV, both paths. */
  float2 new_uv = float2(0.0f);
  /** Mirrored stroke origin in UV, View Plane path. */
  float2 old_uv = float2(0.0f);
  /** #old_uv is a real mirrored hit rather than a copy of #new_uv. */
  bool has_old_uv = false;
};

/** Symmetry iterations are the 3 axis bits, so 8 slots cover every combination. */
static constexpr int AREA_PLANE_SYMMETRY_SLOTS = 8;

/**
 * UV AABB of \a tri on \a tile, clipped to the dab's projection.
 * \return false when the triangle does not overlap this tile.
 */
static bool paint_2d_area_plane_triangle_aabb(const ImagePaintTile *tile,
                                              const ed::sculpt_paint::AreaPlaneTriangle &tri,
                                              const float3 &dab_origin,
                                              const float4x4 &unfold_mat,
                                              const float radius_object,
                                              int &r_x0,
                                              int &r_y0,
                                              int &r_w,
                                              int &r_h)
{
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

  /* Clip the UV AABB to the dab's projection onto this triangle. Neighbor islands sit far from
   * the cursor UV, so clipping to the cursor would drop them; the dab center mapped back to this
   * triangle's UV is the right center. The center must be folded back through #unfold_mat, the
   * same net the falloff is measured in — the raw 3D closest point is the foot of the
   * perpendicular onto a rotated face, which drifts away from the seam and would clip off the
   * far half of the disc. */
  float2 uv_c((min_u + max_u) * 0.5f, (min_v + max_v) * 0.5f);
  const float3 folded_origin = math::transform_point(math::invert(unfold_mat), dab_origin);
  float3 closest;
  closest_on_tri_to_point_v3(
      closest, folded_origin, tri.position[0], tri.position[1], tri.position[2]);
  float bary_p[3];
  interp_weights_tri_v3(bary_p, tri.position[0], tri.position[1], tri.position[2], closest);
  uv_c = tri.uv[0] * bary_p[0] + tri.uv[1] * bary_p[1] + tri.uv[2] * bary_p[2];
  const float r_uv = ed::sculpt_paint::area_plane_triangle_radius_uv(tri, radius_object);
  if (r_uv > 0.0f) {
    const float pad = r_uv * 1.05f;
    min_u = std::max(min_u, uv_c.x - pad);
    max_u = std::min(max_u, uv_c.x + pad);
    min_v = std::max(min_v, uv_c.y - pad);
    max_v = std::min(max_v, uv_c.y + pad);
  }

  int x0 = int(std::floor((min_u - tile->uv_origin[0]) * float(tile->size[0]))) - 1;
  int y0 = int(std::floor((min_v - tile->uv_origin[1]) * float(tile->size[1]))) - 1;
  int x1 = int(std::ceil((max_u - tile->uv_origin[0]) * float(tile->size[0]))) + 1;
  int y1 = int(std::ceil((max_v - tile->uv_origin[1]) * float(tile->size[1]))) + 1;
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, tile->size[0]);
  y1 = std::min(y1, tile->size[1]);
  int w = x1 - x0;
  int h = y1 - y0;
  if (w <= 0 || h <= 0) {
    return false;
  }
  /* Clamp to the max raster size around the dab UV instead of silently skipping the triangle. */
  if (w > AREA_PLANE_TRIANGLE_MAX_SIZE || h > AREA_PLANE_TRIANGLE_MAX_SIZE) {
    const int cx = int(std::floor((uv_c.x - tile->uv_origin[0]) * float(tile->size[0])));
    const int cy = int(std::floor((uv_c.y - tile->uv_origin[1]) * float(tile->size[1])));
    if (w > AREA_PLANE_TRIANGLE_MAX_SIZE) {
      x0 = std::max(x0, cx - AREA_PLANE_TRIANGLE_MAX_SIZE / 2);
      x1 = std::min(x1, x0 + AREA_PLANE_TRIANGLE_MAX_SIZE);
      x0 = std::max(0, x1 - AREA_PLANE_TRIANGLE_MAX_SIZE);
      x1 = std::min(x1, tile->size[0]);
    }
    if (h > AREA_PLANE_TRIANGLE_MAX_SIZE) {
      y0 = std::max(y0, cy - AREA_PLANE_TRIANGLE_MAX_SIZE / 2);
      y1 = std::min(y1, y0 + AREA_PLANE_TRIANGLE_MAX_SIZE);
      y0 = std::max(0, y1 - AREA_PLANE_TRIANGLE_MAX_SIZE);
      y1 = std::min(y1, tile->size[1]);
    }
    w = x1 - x0;
    h = y1 - y0;
    if (w <= 0 || h <= 0) {
      return false;
    }
  }
  r_x0 = x0;
  r_y0 = y0;
  r_w = w;
  r_h = h;
  return true;
}

static const AreaPlaneTileCoverage *paint_2d_area_plane_find_tile_coverage(
    const AreaPlaneDabGeom &geom, const ImagePaintTile *tile)
{
  for (const AreaPlaneTileCoverage &cov : geom.coverage_tiles) {
    if (cov.size[0] == tile->size[0] && cov.size[1] == tile->size[1] &&
        cov.uv_origin[0] == tile->uv_origin[0] && cov.uv_origin[1] == tile->uv_origin[1])
    {
      return &cov;
    }
  }
  return nullptr;
}

static ushort paint_2d_area_plane_pixel_strength(const AreaPlaneCoveragePixel &pixel,
                                                 const bool is_alpha_dest)
{
  const uint32_t alpha_mul = is_alpha_dest ? 65535u : uint32_t(pixel.alpha);
  return ushort((uint32_t(pixel.falloff) * alpha_mul) / 65535u);
}

static void paint_2d_area_plane_write_rgb(float *float_data,
                                          uchar *byte_data,
                                          const int idx,
                                          const float3 &rgb)
{
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
}

/** Shrink the raster AABB to the covered pixels so blend/alloc do not walk empty padding. */
static void paint_2d_area_plane_tighten_coverage(AreaPlaneTriCoverage &cov)
{
  if (cov.pixels.is_empty()) {
    cov.w = 0;
    cov.h = 0;
    return;
  }
  int min_lx = cov.w;
  int min_ly = cov.h;
  int max_lx = 0;
  int max_ly = 0;
  for (const AreaPlaneCoveragePixel &pixel : cov.pixels) {
    min_lx = std::min(min_lx, int(pixel.lx));
    min_ly = std::min(min_ly, int(pixel.ly));
    max_lx = std::max(max_lx, int(pixel.lx));
    max_ly = std::max(max_ly, int(pixel.ly));
  }
  if (min_lx == 0 && min_ly == 0 && max_lx == cov.w - 1 && max_ly == cov.h - 1) {
    return;
  }
  cov.x0 += min_lx;
  cov.y0 += min_ly;
  cov.w = max_lx - min_lx + 1;
  cov.h = max_ly - min_ly + 1;
  for (AreaPlaneCoveragePixel &pixel : cov.pixels) {
    pixel.lx = uint16_t(int(pixel.lx) - min_lx);
    pixel.ly = uint16_t(int(pixel.ly) - min_ly);
  }
}

static ImBuf *paint_2d_area_plane_ensure_scratch(AreaPlaneDabGeom &geom,
                                                 const bool is_float,
                                                 const int w,
                                                 const int h)
{
  std::unique_ptr<ImBuf, AreaPlaneImBufDeleter> &slot = is_float ? geom.scratch_float :
                                                                  geom.scratch_byte;
  if (!slot || slot->x < w || slot->y < h) {
    const int nw = slot ? std::max(w, slot->x) : w;
    const int nh = slot ? std::max(h, slot->y) : h;
    slot.reset(IMB_allocImBuf(
        nw, nh, is_float ? ImBufFlags::FloatData : ImBufFlags::ByteData));
  }
  if (!slot) {
    return nullptr;
  }
  const int stride = slot->x;
  const int64_t mask_len = int64_t(stride) * int64_t(slot->y);
  if (int64_t(geom.scratch_mask.size()) < mask_len) {
    geom.scratch_mask.resize(mask_len, ushort(0));
  }
  /* #IMB_rectblend reads mask with stride = frombuf->x. Only the used sub-rect is blended. */
  ushort *mask = geom.scratch_mask.data();
  for (int y = 0; y < h; y++) {
    memset(mask + y * stride, 0, size_t(w) * sizeof(ushort));
  }
  return slot.get();
}

/**
 * Walk the triangle AABB once: inside-test, barycentric, falloff, unfold, optional alpha.
 * Destination channels then only sample RGB at #AreaPlaneCoveragePixel.sample_position.
 */
static bool paint_2d_area_plane_build_tri_coverage(
    const ImagePaintTile *tile,
    const BrushPainter *painter,
    const ed::sculpt_paint::AreaPlaneTriangle &tri,
    const float4x4 &object_to_brush,
    const float4x4 &alpha_local_mat,
    const float4x4 &unfold_mat,
    const float3 &dab_origin,
    const float3 &dab_normal,
    const float radius_object,
    const bool capture_alpha,
    ImagePool *pool,
    AreaPlaneTriCoverage &r_cov)
{
  r_cov = AreaPlaneTriCoverage{};
  const float3 face_normal = ed::sculpt_paint::area_plane_triangle_face_normal(tri);
  if (math::length_squared(face_normal) < 1e-12f) {
    return false;
  }
  if (!paint_2d_area_plane_triangle_aabb(tile,
                                         tri,
                                         dab_origin,
                                         unfold_mat,
                                         radius_object,
                                         r_cov.x0,
                                         r_cov.y0,
                                         r_cov.w,
                                         r_cov.h))
  {
    return false;
  }

  const int w = r_cov.w;
  const int h = r_cov.h;
  const int x0 = r_cov.x0;
  const int y0 = r_cov.y0;
  const float inv_sx = 1.0f / float(tile->size[0]);
  const float inv_sy = 1.0f / float(tile->size[1]);
  const float uv_origin_x = tile->uv_origin[0];
  const float uv_origin_y = tile->uv_origin[1];
  const Brush *brush = painter->brush;
  r_cov.pixels.reserve(size_t(std::min(int64_t(w) * int64_t(h), int64_t(1 << 20))));

  Mutex mutex;
  threading::parallel_for(IndexRange(h), 8, [&](const IndexRange y_range) {
    Vector<AreaPlaneCoveragePixel> local;
    const int thread = BLI_task_parallel_thread_id(nullptr);
    for (const int64_t ly64 : y_range) {
      const int ly = int(ly64);
      const float uv_y = uv_origin_y + (float(y0 + ly) + 0.5f) * inv_sy;
      for (int lx = 0; lx < w; lx++) {
        const float2 uv(uv_origin_x + (float(x0 + lx) + 0.5f) * inv_sx, uv_y);
        float bary[3];
        if (!ed::sculpt_paint::area_plane_uv_pixel_inside_triangle(tri.uv, uv, bary)) {
          continue;
        }

        const float3 position = tri.position[0] * bary[0] + tri.position[1] * bary[1] +
                                tri.position[2] * bary[2];
        float3 sample_position = math::transform_point(unfold_mat, position);
        sample_position += dab_normal * (math::dot(dab_normal, dab_origin) -
                                         math::dot(dab_normal, sample_position));
        /* Measure the falloff in the unfolded net, the same space the source is sampled in.
         * Measuring the raw 3D distance instead gives every folded face its own disc centered on
         * its perpendicular foot, so the stamp splits into offset lobes across a crease. */
        float strength;
        if (!paint_2d_area_plane_falloff(brush, object_to_brush, sample_position, &strength)) {
          continue;
        }
        float alpha = 1.0f;
        if (capture_alpha) {
          alpha = paint_2d_area_sample_alpha_factor(
              painter, alpha_local_mat, sample_position, thread, pool);
        }

        AreaPlaneCoveragePixel pixel;
        pixel.sample_position = sample_position;
        pixel.lx = uint16_t(lx);
        pixel.ly = uint16_t(ly);
        pixel.falloff = ushort(65535.0f * strength);
        pixel.alpha = ushort(65535.0f * alpha);
        local.append(pixel);
      }
    }
    if (local.is_empty()) {
      return;
    }
    std::lock_guard lock(mutex);
    r_cov.pixels.extend(local);
  });
  paint_2d_area_plane_tighten_coverage(r_cov);
  return !r_cov.pixels.is_empty();
}

static bool paint_2d_area_plane_fill_and_blend(ImagePaintState *s,
                                               ImagePaintTile *tile,
                                               const AreaPlaneTriCoverage &cov,
                                               const float4x4 &channel_local_mat,
                                               ImagePool *pool,
                                               AreaPlaneDabGeom &geom)
{
  if (cov.w <= 0 || cov.h <= 0 || cov.pixels.is_empty()) {
    return false;
  }

  const BrushPainter *painter = s->painter;
  const BrushPainterCache *cache = &tile->cache;
  const PaintModeSettings &paint_mode = s->scene->toolsettings->paint_mode;
  const eMaterialPaintChannel channel = painter->material_channel;
  const bool is_alpha_dest = channel == PAINT_MATERIAL_CHANNEL_ALPHA;
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const bool is_color = ELEM(channel,
                             PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                             PAINT_MATERIAL_CHANNEL_EMISSION,
                             PAINT_MATERIAL_CHANNEL_NORMAL);
  const bool has_selection_mask = BKE_image_paint_selection_mask_has_any(s->image);
  const int tile_number = tile->iuser.tile;
  /* Coverage is shared between material channels, but selection masks belong to an Image. Keep
   * the filter here, where the destination image and its tile are known, rather than baking it
   * into #AreaPlaneTriCoverage for the first channel. */
  const ImBuf *selection_mask = has_selection_mask ?
                                    BKE_image_paint_selection_mask_lookup(
                                        const_cast<const Image *>(s->image), tile_number) :
                                    nullptr;
  const auto pixel_strength = [&](const AreaPlaneCoveragePixel &pixel) {
    const ushort strength = paint_2d_area_plane_pixel_strength(pixel, is_alpha_dest);
    if (!has_selection_mask) {
      return strength;
    }
    if (selection_mask == nullptr) {
      return ushort(0);
    }
    const int x = cov.x0 + int(pixel.lx);
    const int y = cov.y0 + int(pixel.ly);
    return ushort(float(strength) *
                  BKE_image_paint_selection_blend_sample(s->image, tile_number, x, y));
  };

  ImBuf *raster_buf = paint_2d_area_plane_ensure_scratch(
      geom, tile->cache.is_float, cov.w, cov.h);
  if (raster_buf == nullptr) {
    return false;
  }
  ushort *triangle_mask = geom.scratch_mask.data();
  uchar *byte_data = raster_buf->byte_data_for_write();
  float *float_data = raster_buf->float_data_for_write();
  const int stride = raster_buf->x;

  const bool usable = paint_2d_channel_source_usable_2d(painter, channel);
  if (!usable) {
    float3 rgb(painter->material_channel_color[0],
               painter->material_channel_color[1],
               painter->material_channel_color[2]);
    paint_2d_area_encode_canvas_rgb(cache, is_normal, rgb);
    threading::parallel_for(cov.pixels.index_range(), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        const AreaPlaneCoveragePixel &pixel = cov.pixels[i];
        const ushort strength = pixel_strength(pixel);
        if (strength == 0) {
          continue;
        }
        const int idx = int(pixel.ly) * stride + int(pixel.lx);
        paint_2d_area_plane_write_rgb(float_data, byte_data, idx, rgb);
        triangle_mask[idx] = strength;
      }
    });
  }
  else {
    const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
        painter->channel_sources->source(channel);
    const bke::PaintRuntime *paint_runtime = painter->paint->runtime;
    const bool batch_decode = is_color && !is_normal && source.do_linear_conversion &&
                              (paint_runtime == nullptr || !paint_runtime->do_linear_conversion);
    const bool skip_colorspace = batch_decode &&
                                 paint_2d_area_source_matches_canvas_encoding(source, cache);
    if (batch_decode && !skip_colorspace) {
      Array<float3> raw(cov.pixels.size());
      Array<ushort> strengths(cov.pixels.size());
      threading::parallel_for(cov.pixels.index_range(), 1024, [&](const IndexRange range) {
        const int thread = BLI_task_parallel_thread_id(nullptr);
        for (const int64_t i : range) {
          const AreaPlaneCoveragePixel &pixel = cov.pixels[i];
          strengths[i] = pixel_strength(pixel);
          if (strengths[i] == 0) {
            raw[i] = float3(0.0f);
            continue;
          }
          paint_2d_area_sample_channel_color(painter,
                                             cache,
                                             channel,
                                             channel_local_mat,
                                             pixel.sample_position,
                                             thread,
                                             pool,
                                             paint_mode,
                                             false,
                                             false,
                                             false,
                                             raw[i]);
        }
      });
      ed::sculpt_paint::material::ChannelSourceSampler::decode_linear_batch(raw, source.colorspace);
      threading::parallel_for(cov.pixels.index_range(), 4096, [&](const IndexRange range) {
        for (const int64_t i : range) {
          if (strengths[i] == 0) {
            continue;
          }
          float3 rgb = raw[i];
          if (source.flip_green_channel) {
            rgb.y = 1.0f - rgb.y;
          }
          paint_2d_area_encode_canvas_rgb(cache, false, rgb);
          const int idx = int(cov.pixels[i].ly) * stride + int(cov.pixels[i].lx);
          paint_2d_area_plane_write_rgb(float_data, byte_data, idx, rgb);
          triangle_mask[idx] = strengths[i];
        }
      });
    }
    else {
      threading::parallel_for(cov.pixels.index_range(), 1024, [&](const IndexRange range) {
        const int thread = BLI_task_parallel_thread_id(nullptr);
        for (const int64_t i : range) {
          const AreaPlaneCoveragePixel &pixel = cov.pixels[i];
          const ushort strength = pixel_strength(pixel);
          if (strength == 0) {
            continue;
          }
          float3 rgb;
          paint_2d_area_sample_channel_color(painter,
                                             cache,
                                             channel,
                                             channel_local_mat,
                                             pixel.sample_position,
                                             thread,
                                             pool,
                                             paint_mode,
                                             !skip_colorspace,
                                             !skip_colorspace,
                                             true,
                                             rgb);
          const int idx = int(pixel.ly) * stride + int(pixel.lx);
          paint_2d_area_plane_write_rgb(float_data, byte_data, idx, rgb);
          triangle_mask[idx] = strength;
        }
      });
    }
  }

  paint_2d_blend_area_buffer(
      s, tile, cov.x0, cov.y0, raster_buf, triangle_mask, s->blend, cov.w, cov.h);
  return true;
}

/**
 * UV of \a uv_in mirrored across the object symmetry planes of \a iteration_symm.
 *
 * Goes through 3D: the point is looked up on the surface, reflected in object space, then
 * landed back on the mesh with a nearest query. A reflected point sits exactly on the surface
 * only for a perfectly symmetric mesh, so a miss beyond the snap distance is normal and drops
 * that dab rather than painting somewhere wrong.
 *
 * Takes nothing from #ImagePaintState: the result is cached once and reused by every material
 * channel, which may paint into images of different sizes.
 *
 * \param iteration_symm: the axes of THIS iteration, not the full enabled set.
 */
static bool paint_2d_symmetry_uv(const ed::sculpt_paint::AreaPlaneMesh &mesh,
                                 const float2 &uv_in,
                                 const ePaintSymmetryFlags iteration_symm,
                                 ed::sculpt_paint::AreaPlaneHit &r_hit,
                                 float2 &r_uv)
{
  ed::sculpt_paint::AreaPlaneHit source_hit;
  if (!mesh.hit_at_uv(uv_in, source_hit)) {
    return false;
  }
  const float snap = AREA_PLANE_SYMMETRY_SNAP_FACTOR * mesh.edge_length_median();
  if (snap <= 0.0f) {
    return false;
  }
  const float3 mirrored = ed::sculpt_paint::symmetry_flip(source_hit.position, iteration_symm);
  return mesh.closest_hit(mirrored, snap, r_hit, r_uv);
}

/**
 * Fill \a dab for symmetry iteration \a index, once per stroke event.
 *
 * \param uv_new: the original dab center in UV.
 * \param uv_old: the original stroke origin in UV (the View Plane path interpolates to it).
 */
static void paint_2d_symmetry_dab_ensure(const ed::sculpt_paint::AreaPlaneMesh &mesh,
                                         const int index,
                                         const float2 &uv_new,
                                         const float2 &uv_old,
                                         SymmetryDab &dab)
{
  if (dab.computed) {
    return;
  }
  dab.computed = true;

  /* The axes to flip are this iteration's own bits. Passing the full enabled set would mirror
   * every iteration identically and collapse a three-dab fan into one dab drawn three times. */
  const ePaintSymmetryFlags iteration_symm = ePaintSymmetryFlags(index);

  if (!paint_2d_symmetry_uv(mesh, uv_new, iteration_symm, dab.hit, dab.new_uv)) {
    return;
  }
  dab.valid = true;
  dab.flipped = (count_bits_i(uint32_t(index) & 7u) & 1) != 0;

  ed::sculpt_paint::AreaPlaneHit old_hit;
  float2 old_uv;
  if (paint_2d_symmetry_uv(mesh, uv_old, iteration_symm, old_hit, old_uv)) {
    /* A mirrored stroke that crossed a UV seam lands its two ends on different islands; drawing
     * the segment between them would streak a band across empty UV space. Comparing against the
     * original segment's UV length is cheap, unlike walking the UV net per dab. */
    const float original_len = math::distance(uv_new, uv_old);
    const float mirrored_len = math::distance(dab.new_uv, old_uv);
    const bool island_jump = mirrored_len >
                             AREA_PLANE_SYMMETRY_ISLAND_JUMP_FACTOR * original_len;
    if (!island_jump) {
      dab.old_uv = old_uv;
      dab.has_old_uv = true;
    }
  }
}

/**
 * Shared tail of dab preparation: everything after the dab center is known.
 *
 * \param flipped: this is a mirrored dab, so its tangent frame is the reflected one.
 */
static bool paint_2d_area_plane_prepare_from_hit(ImagePaintState *s,
                                                 const ed::sculpt_paint::AreaPlaneHit &hit,
                                                 const float base_size,
                                                 const bool flipped,
                                                 AreaPlaneDabGeom &r_geom)
{
  r_geom = AreaPlaneDabGeom{};
  BrushPainter *painter = s->painter;
  const ed::sculpt_paint::AreaPlaneMesh &mesh = *painter->area_plane_mesh;

  r_geom.hit = hit;
  r_geom.flipped = flipped;
#if PBR_PAINT_2D_PROFILE
  const double t0 = BLI_time_now_seconds();
#endif
  if (s->tiles[0].size[0] <= 0) {
    return false;
  }

  const float radius_uv = base_size / float(s->tiles[0].size[0]);
  r_geom.radius_object = mesh.radius_object(r_geom.hit, radius_uv);
  if (r_geom.radius_object <= 1e-12f) {
    return false;
  }

  r_geom.object_to_brush = ed::sculpt_paint::area_plane_local_mat(
      r_geom.hit.position, r_geom.hit.normal, r_geom.radius_object, 0.0f);
  r_geom.dab_tri = mesh.triangle(r_geom.hit.tri_index);

#if PBR_PAINT_2D_PROFILE
  const double t1 = BLI_time_now_seconds();
#endif
  r_geom.accepted = mesh.triangles_in_sphere(r_geom.object_to_brush);
  if (r_geom.accepted.is_empty()) {
    return false;
  }

#if PBR_PAINT_2D_PROFILE
  const double t2 = BLI_time_now_seconds();
#endif
  r_geom.unfold_mats = ed::sculpt_paint::area_plane_unfold_matrices(
      mesh.triangles(), r_geom.hit.tri_index, r_geom.accepted);
  r_geom.dab_normal = ed::sculpt_paint::area_plane_triangle_face_normal(r_geom.dab_tri);
  if (math::length_squared(r_geom.dab_normal) < 1e-12f) {
    r_geom.dab_normal = r_geom.hit.normal;
  }
  r_geom.valid = true;
#if PBR_PAINT_2D_PROFILE
  printf("[pbr_paint_2d] area_plane prepare hit=%.3fms sphere=%.3fms unfold=%.3fms "
         "accepted=%d tris=%d radius_obj=%.6g\n",
         (t1 - t0) * 1000.0,
         (t2 - t1) * 1000.0,
         (BLI_time_now_seconds() - t2) * 1000.0,
         int(r_geom.accepted.size()),
         int(mesh.triangles().size()),
         double(r_geom.radius_object));
#endif
  return true;
}

/** Original (iteration 0) dab: UV center → surface hit → shared tail. */
static bool paint_2d_area_plane_prepare(ImagePaintState *s,
                                        const float uv_center[2],
                                        const float base_size,
                                        AreaPlaneDabGeom &r_geom)
{
  const ed::sculpt_paint::AreaPlaneMesh &mesh = *s->painter->area_plane_mesh;
  ed::sculpt_paint::AreaPlaneHit hit;
  if (!mesh.hit_at_uv(float2(uv_center[0], uv_center[1]), hit)) {
    r_geom = AreaPlaneDabGeom{};
    return false;
  }
  return paint_2d_area_plane_prepare_from_hit(s, hit, base_size, false, r_geom);
}

static void paint_2d_area_plane_apply(ImagePaintState *s, AreaPlaneDabGeom &geom)
{
#if PBR_PAINT_2D_STROKE_PROFILE
  const StrokePhaseTimer area_timer(&g_stroke_area_seconds, &g_stroke_area_calls);
  int64_t area_pixels = 0;
#endif
  BrushPainter *painter = s->painter;
  const ed::sculpt_paint::AreaPlaneMesh &mesh = *painter->area_plane_mesh;
  /* Read from the View2D rather than #SpaceImage: it is the value the display actually rotates by,
   * already gated to the modes that support a canvas rotation. */
  const float canvas_rotation = (s->v2d != nullptr) ? s->v2d->rotation : 0.0f;
  const ed::sculpt_paint::AreaPlaneFrame channel_frame = paint_2d_area_channel_frame(
      painter,
      painter->material_channel,
      geom.dab_tri,
      geom.hit.position,
      geom.radius_object,
      geom.flipped,
      canvas_rotation);
  const float4x4 channel_local_mat = ed::sculpt_paint::area_plane_object_to_local(channel_frame);
  const bool capture_alpha = painter->material_alpha_masking;
  const float4x4 alpha_local_mat =
      capture_alpha ? ed::sculpt_paint::area_plane_object_to_local(paint_2d_area_channel_frame(
                          painter,
                          PAINT_MATERIAL_CHANNEL_ALPHA,
                          geom.dab_tri,
                          geom.hit.position,
                          geom.radius_object,
                          geom.flipped,
                          canvas_rotation)) :
                      geom.object_to_brush;

  ImagePool *pool = painter->channel_sources != nullptr ? painter->channel_sources->pool() :
                                                          nullptr;

#if PBR_PAINT_2D_PROFILE
  const double t0 = BLI_time_now_seconds();
  double coverage_seconds = 0.0;
  int64_t raster_tri_num = 0;
  int64_t pixel_num = 0;
  int64_t built_tris = 0;
  int64_t reused_tris = 0;
#endif

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

    const AreaPlaneTileCoverage *shared_cov = paint_2d_area_plane_find_tile_coverage(geom, tile);
    AreaPlaneTileCoverage local_cov;
    const AreaPlaneTileCoverage *tile_cov = shared_cov;
    if (tile_cov == nullptr) {
      local_cov.size[0] = tile->size[0];
      local_cov.size[1] = tile->size[1];
      local_cov.uv_origin[0] = tile->uv_origin[0];
      local_cov.uv_origin[1] = tile->uv_origin[1];
      local_cov.tris.resize(geom.accepted.size());
      tile_cov = &local_cov;
    }

    for (int tri_i = 0; tri_i < geom.accepted.size(); tri_i++) {
      const AreaPlaneTriCoverage *tri_cov = &tile_cov->tris[tri_i];
      if (shared_cov == nullptr) {
#if PBR_PAINT_2D_PROFILE
        const double t_cov = BLI_time_now_seconds();
#endif
        paint_2d_area_plane_build_tri_coverage(tile,
                                               painter,
                                               mesh.triangle(geom.accepted[tri_i]),
                                               geom.object_to_brush,
                                               alpha_local_mat,
                                               geom.unfold_mats[tri_i],
                                               geom.hit.position,
                                               geom.dab_normal,
                                               geom.radius_object,
                                               capture_alpha,
                                               pool,
                                               local_cov.tris[tri_i]);
        tri_cov = &local_cov.tris[tri_i];
#if PBR_PAINT_2D_PROFILE
        coverage_seconds += BLI_time_now_seconds() - t_cov;
        built_tris++;
#endif
      }
#if PBR_PAINT_2D_PROFILE
      else {
        reused_tris++;
      }
#endif
      if (paint_2d_area_plane_fill_and_blend(s, tile, *tri_cov, channel_local_mat, pool, geom)) {
#if PBR_PAINT_2D_PROFILE
        raster_tri_num++;
        pixel_num += int64_t(tri_cov->pixels.size());
#endif
#if PBR_PAINT_2D_STROKE_PROFILE
        area_pixels += int64_t(tri_cov->pixels.size());
#endif
      }
    }

    if (shared_cov == nullptr) {
      geom.coverage_tiles.append(std::move(local_cov));
    }
  }
#if PBR_PAINT_2D_PROFILE
  printf("[pbr_paint_2d] area_plane raster channel=%d written_tris=%lld pixels=%lld "
         "built_tris=%lld reused_tris=%lld coverage=%.3fms fill=%.3fms elapsed=%.3fms\n",
         int(painter->material_channel),
         static_cast<long long>(raster_tri_num),
         static_cast<long long>(pixel_num),
         static_cast<long long>(built_tris),
         static_cast<long long>(reused_tris),
         coverage_seconds * 1000.0,
         ((BLI_time_now_seconds() - t0) - coverage_seconds) * 1000.0,
         (BLI_time_now_seconds() - t0) * 1000.0);
#endif
#if PBR_PAINT_2D_STROKE_PROFILE
  g_stroke_area_pixels += area_pixels;
#endif
}

/**
 * Draw the Area Plane dab for one symmetry iteration.
 *
 * \param dab: the shared mirrored-center cache; nullptr-equivalent for iteration 0, which uses
 * \a uv_center directly.
 * \param shared_geom: per-state geometry cache for this iteration, reused by later channels.
 */
static void paint_2d_area_plane_stroke(ImagePaintState *s,
                                       const float uv_center[2],
                                       const float base_size,
                                       const SymmetryDab *dab,
                                       const float3 &origin_position,
                                       const float2 &origin_uv,
                                       AreaPlaneDabGeom *shared_geom)
{
  if (shared_geom != nullptr && shared_geom->valid) {
    paint_2d_area_plane_apply(s, *shared_geom);
    return;
  }

  AreaPlaneDabGeom local;
  bool prepared = false;
  if (dab == nullptr) {
    prepared = paint_2d_area_plane_prepare(s, uv_center, base_size, local);
  }
  else if (dab->valid) {
    prepared = paint_2d_area_plane_prepare_from_hit(
        s, dab->hit, base_size, dab->flipped, local);
  }
  if (!prepared) {
    return;
  }

  if (dab != nullptr) {
    /* On the symmetry plane the mirror lands on the original in 3D. */
    const float dist_3d = math::distance(local.hit.position, origin_position);
    if (dist_3d < AREA_PLANE_SYMMETRY_MERGE_FACTOR * local.radius_object) {
      return;
    }
    /* Overlapping UV islands (a mirror modifier sharing one unwrap) put two different surface
     * points on the same texels; 3D distance cannot see that. */
    const float radius_uv = ed::sculpt_paint::area_plane_triangle_radius_uv(
        local.dab_tri, local.radius_object);
    const float dist_uv = math::distance(dab->new_uv, origin_uv);
    if (radius_uv > 0.0f && dist_uv < AREA_PLANE_SYMMETRY_MERGE_FACTOR * radius_uv) {
      return;
    }
  }

  if (shared_geom != nullptr) {
    *shared_geom = std::move(local);
    paint_2d_area_plane_apply(s, *shared_geom);
  }
  else {
    paint_2d_area_plane_apply(s, local);
  }
}

static void paint_2d_stroke_single(ImagePaintState *s,
                                   const float prev_mval[2],
                                   const float mval[2],
                                   const bool eraser,
                                   float pressure,
                                   float distance,
                                   float base_size,
                                   MutableSpan<SymmetryDab> dabs,
                                   MutableSpan<AreaPlaneDabGeom> geoms,
                                   const ImagePaintState *shared_state)
{
#if PBR_PAINT_2D_STROKE_PROFILE
  const StrokePhaseTimer single_timer(&g_stroke_single_seconds, &g_stroke_single_calls);
#endif
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
#if PBR_PAINT_2D_STROKE_PROFILE
    g_stroke_area_path_calls++;
#endif
    const ed::sculpt_paint::AreaPlaneMesh &mesh = *painter->area_plane_mesh;
    const char symm = char(s->symmetry & PAINT_SYMM_AXIS_ALL);
    const float2 uv_new(new_uv[0], new_uv[1]);
    const float2 uv_old(old_uv[0], old_uv[1]);

    /* Iteration 0 is the original dab; its center anchors the duplicate-suppression tests. */
    paint_2d_area_plane_stroke(
        s, new_uv, base_size, nullptr, float3(0.0f), uv_new, &geoms[0]);
    const float3 origin_position = geoms[0].valid ? geoms[0].hit.position : float3(0.0f);

    if (symm != 0 && geoms[0].valid) {
      /* Upper bound is `symm`, not the slot count: #is_symmetry_iteration_valid only tests that
       * `i` shares a bit with `symm`, so iterating past it would accept supersets — with X alone
       * it would also run XY, XZ and XYZ and mirror the dab three extra times. Every other
       * symmetry loop in the codebase bounds the same way. */
      for (int i = 1; i <= int(symm); i++) {
        if (!ed::sculpt_paint::is_symmetry_iteration_valid(char(i), symm)) {
          continue;
        }
        paint_2d_symmetry_dab_ensure(mesh, i, uv_new, uv_old, dabs[i]);
        if (!dabs[i].valid) {
          continue;
        }
        paint_2d_area_plane_stroke(
            s, new_uv, base_size, &dabs[i], origin_position, uv_new, &geoms[i]);
      }
    }
    painter->firsttouch = false;
    return;
  }

#if PBR_PAINT_2D_STROKE_PROFILE
  g_stroke_view_path_calls++;
#endif

  float start_uv[2];
  ui::view2d_region_to_view(s->v2d, 0.0f, 0.0f, &start_uv[0], &start_uv[1]);

  const char symm = char(s->symmetry & PAINT_SYMM_AXIS_ALL);
  const bool use_symmetry = symm != 0 && paint_2d_use_area_plane_mesh(painter);
  const bool firsttouch_entry = painter->firsttouch;

  /* Upper bound is `symm`, not the slot count: #is_symmetry_iteration_valid only tests that the
   * iteration shares a bit with `symm`, so iterating past it would accept supersets — with X
   * alone it would also run XY, XZ and XYZ. Every other symmetry loop in the codebase bounds the
   * same way. */
  for (int iter = 0; iter <= int(symm); iter++) {
    if (iter > 0 && !use_symmetry) {
      break;
    }
    if (!ed::sculpt_paint::is_symmetry_iteration_valid(char(iter), symm)) {
      continue;
    }

    float iter_new_uv[2];
    float iter_old_uv[2];
    float iter_mval[2] = {mval[0], mval[1]};
    if (iter == 0) {
      copy_v2_v2(iter_new_uv, new_uv);
      copy_v2_v2(iter_old_uv, old_uv);
    }
    else {
      const ed::sculpt_paint::AreaPlaneMesh &mesh = *painter->area_plane_mesh;
      paint_2d_symmetry_dab_ensure(mesh,
                                   iter,
                                   float2(new_uv[0], new_uv[1]),
                                   float2(old_uv[0], old_uv[1]),
                                   dabs[iter]);
      if (!dabs[iter].valid) {
        continue;
      }
      iter_new_uv[0] = dabs[iter].new_uv.x;
      iter_new_uv[1] = dabs[iter].new_uv.y;
      /* Without a mirrored origin this is a single dab, not a segment: interpolating from the
       * unmirrored origin would drag a stroke across the whole texture. */
      if (dabs[iter].has_old_uv) {
        iter_old_uv[0] = dabs[iter].old_uv.x;
        iter_old_uv[1] = dabs[iter].old_uv.y;
      }
      else {
        copy_v2_v2(iter_old_uv, iter_new_uv);
      }
      /* View mapping caches its source by cursor position, so every mirrored dab would sample
       * the same patch of texture if the region coordinates were not moved with it. */
      ui::view2d_view_to_region_fl(
          s->v2d, iter_new_uv[0], iter_new_uv[1], &iter_mval[0], &iter_mval[1]);
    }

    /* Each iteration starts a stroke of its own as far as first-touch bookkeeping goes. */
    painter->firsttouch = firsttouch_entry;

    float last_uv[2];
    if (painter->firsttouch) {
      /* paint exactly once on first touch */
      copy_v2_v2(last_uv, iter_new_uv);
    }
    else {
      copy_v2_v2(last_uv, iter_old_uv);
    }

    const float uv_brush_size[2] = {
        (s->symmetry & PAINT_TILE_X) ? FLT_MAX : base_size / s->tiles[0].size[0],
        (s->symmetry & PAINT_TILE_Y) ? FLT_MAX : base_size / s->tiles[0].size[1]};

    for (int i = 0; i < s->num_tiles; i++) {
      ImagePaintTile *tile = &s->tiles[i];

      /* First test: Project brush into UV space, clip against tile. */
      const int uv_size[2] = {1, 1};
      float local_new_uv[2], local_old_uv[2];
      sub_v2_v2v2(local_new_uv, iter_new_uv, tile->uv_origin);
      sub_v2_v2v2(local_old_uv, iter_old_uv, tile->uv_origin);
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
      paint_2d_uv_to_coord(tile, iter_new_uv, new_coord);
      paint_2d_uv_to_coord(tile, iter_old_uv, old_coord);
      if (iter > 0) {
        float origin_coord[2];
        paint_2d_uv_to_coord(tile, new_uv, origin_coord);
        /* Measured in this tile's texels: UV distance cannot tell whether two islands land on
         * the same pixels, and tiles may differ in resolution. */
        if (len_v2v2(new_coord, origin_coord) < AREA_PLANE_SYMMETRY_MERGE_FACTOR * size) {
          continue;
        }
      }
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

      brush_painter_2d_refresh_cache(
          s, painter, tile, new_coord, iter_mval, pressure, distance, size, shared_state);

      if (paint_2d_op(s, tile, old_coord, new_coord)) {
        tile->need_redraw = true;
      }
    }
  }

  painter->firsttouch = false;
}

#if PBR_PAINT_2D_STROKE_PROFILE
/**
 * One-shot dump of #DirectSampleLayout.kind for every usable PBR source on this stroke.
 *
 * Uses the mapping that the upcoming path will actually sample: Area Plane keeps AREA, View Plane
 * remaps AREA/3D to VIEW. Cheap enough to run once at the first dab; the kind itself does not
 * depend on cursor position.
 */
static void paint_2d_stroke_profile_log_layouts(const BrushPainter *painter)
{
  if (painter == nullptr || painter->channel_sources == nullptr) {
    printf("[PBR-STROKE] layout: no channel_sources\n");
    return;
  }
  const short raw_map = paint_2d_shared_source_map_mode(painter);
  const eMTex_BrushMapMode layout_map = eMTex_BrushMapMode(
      paint_2d_use_area_plane(painter) ? raw_map : paint_2d_source_map_mode_for_2d(raw_map));
  int logged = 0;
  for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
    const eMaterialPaintChannel channel = eMaterialPaintChannel(i);
    if (!paint_2d_channel_source_usable_2d(painter, channel)) {
      continue;
    }
    MTex mtex_2d = dna::shallow_copy(*painter->channel_sources->source(channel).mtex);
    mtex_2d.brush_map_mode = layout_map;
    const ed::sculpt_paint::material::DirectSampleLayout layout =
        ed::sculpt_paint::material::make_direct_sample_layout(
            painter->channel_sources->source(channel),
            mtex_2d,
            painter->paint != nullptr ? painter->paint->runtime : nullptr,
            *painter->brush,
            nullptr);
    printf("[PBR-STROKE] layout channel=%s kind=%s map=%s raw_map=%s ibuf=%dx%d\n",
           BKE_paint_material_channel_info(channel).ui_name,
           paint_2d_direct_sample_kind_name(layout.kind),
           paint_2d_map_mode_name(layout_map),
           paint_2d_map_mode_name(raw_map),
           layout.ibuf_x,
           layout.ibuf_y);
    logged++;
  }
  if (logged == 0) {
    printf("[PBR-STROKE] layout: channel_sources active but no usable image sources\n");
  }
}
#endif

void paint_2d_stroke(void *ps,
                     const float prev_mval[2],
                     const float mval[2],
                     const bool eraser,
                     float pressure,
                     float distance,
                     float base_size)
{
#if PBR_PAINT_2D_STROKE_PROFILE
  const StrokePhaseTimer wall_timer(&g_stroke_wall_seconds, &g_stroke_wall_calls);
#endif
  ImagePaintState *s = static_cast<ImagePaintState *>(ps);
#if PBR_PAINT_2D_STROKE_PROFILE
  if (!g_stroke_meta_logged) {
    g_stroke_meta_logged = true;
    g_stroke_painters = 1 + s->material_extra_states_num;
    printf("[PBR-STROKE] begin painters=%d area_plane=%d canvas=%dx%d\n",
           g_stroke_painters,
           int(!eraser && paint_2d_use_area_plane(s->painter)),
           (s->tiles != nullptr && s->num_tiles > 0) ? s->tiles[0].size[0] : 0,
           (s->tiles != nullptr && s->num_tiles > 0) ? s->tiles[0].size[1] : 0);
    paint_2d_stroke_profile_log_layouts(s->painter);
  }
#endif
  /* Mirrored centers are mesh-only, so every material channel shares them; the geometry beside
   * them is sized by each state's own image and is not shared. */
  Array<SymmetryDab> dabs(AREA_PLANE_SYMMETRY_SLOTS);
  Array<AreaPlaneDabGeom> geoms(AREA_PLANE_SYMMETRY_SLOTS);
  paint_2d_stroke_single(
      s, prev_mval, mval, eraser, pressure, distance, base_size, dabs, geoms, nullptr);
  for (int i = 0; i < s->material_extra_states_num; i++) {
    paint_2d_stroke_single(s->material_extra_states[i],
                           prev_mval,
                           mval,
                           eraser,
                           pressure,
                           distance,
                           base_size,
                           dabs,
                           geoms,
                           s);
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
  s->region = CTX_wm_region(C);
  s->v2d = &s->region->v2d;
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
  if (s->painter->pool) {
    BKE_image_pool_free(s->painter->pool);
    s->painter->pool = nullptr;
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
  PaintModeSettings &paint_mode = settings->paint_mode;
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
    if (brush == nullptr || brush->material_paint == nullptr) {
      return nullptr;
    }
    const BrushMaterialPaint &brush_paint = *brush->material_paint;

    /* #Material.paint_channel_cache is only refreshed on writes made through this API (see its
     * doc comment); anything that changes the node tree from outside it (undo/redo swapping in a
     * different Main, manual node edits in the Shader Editor) leaves the cache pointing at a
     * still-valid but no-longer-wired Image, so painting would keep writing into a map the
     * material's Principled BSDF no longer reads from. A once-per-stroke invalidation is cheap
     * (a handful of socket/link lookups) compared to the per-dab sampling cost already spent
     * below, and removes that whole class of staleness. */
    BKE_paint_material_channel_cache_invalidate(BKE_object_material_get(ob, ob->actcol));

    Main *bmain = CTX_data_main(C);
    BKE_paint_material_images_ensure_writable(
        *bmain, *ob, brush_paint, paint_mode, settings->imapaint.paint.visible_material_channels);
    ED_space_image_paint_auto_select_material_canvas(bmain, ob);

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (!info.supports_image_paint) {
        continue;
      }
      if (!BKE_paint_material_channel_writes_to_target(
              brush_paint,
              paint_mode,
              settings->imapaint.paint.visible_material_channels,
              info.channel))
      {
        continue;
      }
      Image *channel_image;
      ImageUser *channel_iuser;
      if (!BKE_paint_principled_channel_image_get(
              *ob, info.channel, &channel_image, &channel_iuser, &paint_mode))
      {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    TIP_("%s channel has no paintable image texture on the active material"),
                    IFACE_(info.ui_name));
      }
    }

    const Vector<PaintMaterialImageTarget> targets = BKE_paint_material_image_targets_get(
        *ob, paint_mode, &brush_paint, settings->imapaint.paint.visible_material_channels);
    if (targets.is_empty()) {
      return nullptr;
    }

    const Paint *paint = BKE_paint_get_active_from_context(C);
    const bool invert = mode == BrushStrokeMode::Invert;

    std::shared_ptr<ed::sculpt_paint::material::ChannelSourceSet> channel_sources;
    if (!invert) {
      channel_sources = std::make_shared<ed::sculpt_paint::material::ChannelSourceSet>(
          brush_paint, paint_mode, settings->imapaint.paint.visible_material_channels);
      if (!channel_sources->is_active()) {
        channel_sources.reset();
      }
    }
    const bool alpha_masking = channel_sources != nullptr &&
                                BKE_paint_material_channel_masks_stroke(
                                    brush_paint,
                                    paint_mode,
                                    settings->imapaint.paint.visible_material_channels);
    const float alpha_fallback = BKE_paint_material_channel_value(
        brush_paint, paint_mode, PAINT_MATERIAL_CHANNEL_ALPHA);

    std::shared_ptr<ed::sculpt_paint::AreaPlaneMesh> area_plane_mesh;
    {
      const bool symmetry_on = (settings->imapaint.paint.symmetry_flags &
                                PAINT_SYMM_AXIS_ALL) != 0;
      /* Symmetry mirrors a dab through the surface, so it needs the UV<->3D bridge even when no
       * channel has a source texture at all: #ChannelSourceSet is null for a flat-color stroke,
       * which still has to mirror. Area Plane's own need for the mesh is a separate condition. */
      /* Material channels can use a flat Property/Color value while the brush itself still has
       * an Area Plane texture. The mesh is needed for that regular brush texture as well; do not
       * make it depend only on PBR channel sources. */
      /* Area Plane is a projection mode, not a requirement for a brush texture. It must also
       * project a flat Property/Color value across UV islands when the brush has no texture. */
      bool use_area = symmetry_on || brush->mtex.brush_map_mode == MTEX_MAP_MODE_AREA;
      if (!use_area && channel_sources != nullptr) {
        for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
          const ed::sculpt_paint::material::ChannelSourceSet::ChannelSource &source =
              channel_sources->source(i);
          if (!source.usable || source.mtex == nullptr) {
            continue;
          }
          if (source.mtex->brush_map_mode == MTEX_MAP_MODE_AREA) {
            use_area = true;
            break;
          }
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
        const float3 rgb = BKE_paint_material_channel_color_get(
            brush_paint, *paint, *brush, target.channel, invert);
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
      BKE_image_partial_update_mark_full_update(s->image);
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
#if PBR_PAINT_2D_STROKE_PROFILE
  const StrokePhaseTimer redraw_timer(&g_stroke_redraw_seconds, &g_stroke_redraw_calls);
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

#if PBR_PAINT_2D_DAB_PROFILE
  if (g_dab_fill_calls > 0) {
    printf("[PBR-DAB] fill total=%.3f ms calls=%d pixels=%lld (%.1f Mpx/s, %.4f ms/call)\n",
           g_dab_fill_seconds * 1000.0,
           g_dab_fill_calls,
           (long long)g_dab_fill_pixels,
           g_dab_fill_seconds > 0.0 ?
               double(g_dab_fill_pixels) / g_dab_fill_seconds / 1000000.0 :
               0.0,
           g_dab_fill_seconds * 1000.0 / double(g_dab_fill_calls));
  }
  g_dab_fill_seconds = 0.0;
  g_dab_fill_pixels = 0;
  g_dab_fill_calls = 0;
#endif

#if PBR_PAINT_2D_STROKE_PROFILE
  if (g_stroke_wall_calls > 0 || g_stroke_redraw_calls > 0) {
    const double leftover = g_stroke_single_seconds - g_stroke_dab_fill_seconds -
                            g_stroke_op_seconds - g_stroke_area_seconds -
                            g_stroke_curve_mask_seconds - g_stroke_source_update_seconds;
    const char *path = "none";
    if (g_stroke_view_path_calls > 0 && g_stroke_area_path_calls > 0) {
      path = "mixed";
    }
    else if (g_stroke_area_path_calls > 0) {
      path = "area";
    }
    else if (g_stroke_view_path_calls > 0) {
      path = "view";
    }
    const double dab_mpx = g_stroke_dab_fill_seconds > 0.0 ?
                               double(g_stroke_dab_fill_pixels) / g_stroke_dab_fill_seconds /
                                   1000000.0 :
                               0.0;
    const double area_mpx = g_stroke_area_seconds > 0.0 ?
                                double(g_stroke_area_pixels) / g_stroke_area_seconds / 1000000.0 :
                                0.0;
    printf("[PBR-STROKE] painters=%d path=%s view_calls=%d area_calls=%d "
           "imbuf_new=%d imbuf_update=%d imbuf_partial=%d\n",
           g_stroke_painters,
           path,
           g_stroke_view_path_calls,
           g_stroke_area_path_calls,
           g_stroke_imbuf_new,
           g_stroke_imbuf_update,
           g_stroke_imbuf_partial);
    printf("[PBR-STROKE] wall=%.3f ms calls=%d (%.3f ms/call)  single=%.3f ms leftover=%.3f ms\n",
           g_stroke_wall_seconds * 1000.0,
           g_stroke_wall_calls,
           g_stroke_wall_calls > 0 ? g_stroke_wall_seconds * 1000.0 / double(g_stroke_wall_calls) :
                                     0.0,
           g_stroke_single_seconds * 1000.0,
           leftover * 1000.0);
    printf("[PBR-STROKE] curve_mask=%.3f ms calls=%d  source_update=%.3f ms calls=%d\n",
           g_stroke_curve_mask_seconds * 1000.0,
           g_stroke_curve_mask_calls,
           g_stroke_source_update_seconds * 1000.0,
           g_stroke_source_update_calls);
    printf("[PBR-STROKE] dab_fill=%.3f ms calls=%d px=%lld (%.1f Mpx/s)\n",
           g_stroke_dab_fill_seconds * 1000.0,
           g_stroke_dab_fill_calls,
           static_cast<long long>(g_stroke_dab_fill_pixels),
           dab_mpx);
    printf("[PBR-STROKE] blend_op=%.3f ms calls=%d px=%lld\n",
           g_stroke_op_seconds * 1000.0,
           g_stroke_op_calls,
           static_cast<long long>(g_stroke_op_pixels));
    printf("[PBR-STROKE] area_raster=%.3f ms calls=%d px=%lld (%.1f Mpx/s)\n",
           g_stroke_area_seconds * 1000.0,
           g_stroke_area_calls,
           static_cast<long long>(g_stroke_area_pixels),
           area_mpx);
    printf("[PBR-STROKE] redraw=%.3f ms calls=%d  total=%.3f ms\n",
           g_stroke_redraw_seconds * 1000.0,
           g_stroke_redraw_calls,
           (g_stroke_wall_seconds + g_stroke_redraw_seconds) * 1000.0);
  }
  g_stroke_wall_seconds = 0.0;
  g_stroke_wall_calls = 0;
  g_stroke_single_seconds = 0.0;
  g_stroke_single_calls = 0;
  g_stroke_op_seconds = 0.0;
  g_stroke_op_calls = 0;
  g_stroke_op_pixels = 0;
  g_stroke_dab_fill_seconds = 0.0;
  g_stroke_dab_fill_calls = 0;
  g_stroke_dab_fill_pixels = 0;
  g_stroke_area_seconds = 0.0;
  g_stroke_area_calls = 0;
  g_stroke_area_pixels = 0;
  g_stroke_redraw_seconds = 0.0;
  g_stroke_redraw_calls = 0;
  g_stroke_painters = 0;
  g_stroke_view_path_calls = 0;
  g_stroke_area_path_calls = 0;
  g_stroke_imbuf_new = 0;
  g_stroke_imbuf_update = 0;
  g_stroke_imbuf_partial = 0;
  g_stroke_curve_mask_seconds = 0.0;
  g_stroke_curve_mask_calls = 0;
  g_stroke_source_update_seconds = 0.0;
  g_stroke_source_update_calls = 0;
  g_stroke_meta_logged = false;
#endif
}

static void paint_2d_fill_add_pixel_byte(const Scene * /*scene*/,
                                         const Image *image,
                                         int tile_number,
                                         const int x_px,
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
    if (BKE_image_paint_selection_mask_has_any(image)) {
      if (!paint_2d_selection_mask_is_inside(image, tile_number, x_px, y_px)) {
        BLI_BITMAP_SET(touched, coordinate, true);
        return;
      }
    }
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

static void paint_2d_fill_add_pixel_float(const Scene * /*scene*/,
                                          const Image *image,
                                          int tile_number,
                                          const int x_px,
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
    if (BKE_image_paint_selection_mask_has_any(image)) {
      if (!paint_2d_selection_mask_is_inside(image, tile_number, x_px, y_px)) {
        BLI_BITMAP_SET(touched, coordinate, true);
        return;
      }
    }
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

struct ImagePaintUVObjectFaces {
  Object *object = nullptr;
  BMesh *bm = nullptr;
  bool owns_bm = false;
  BMUVOffsets offsets{};
  Vector<int> faces;
  Map<int, Vector<int>> tile_faces;
};

static void paint_2d_geometry_fill_item_discard(ImagePaintUVObjectFaces &item)
{
  if (item.owns_bm && item.bm != nullptr) {
    BM_mesh_free(item.bm);
    item.bm = nullptr;
  }
  item.owns_bm = false;
}

static void paint_2d_geometry_fill_free_items(MutableSpan<ImagePaintUVObjectFaces> items)
{
  for (ImagePaintUVObjectFaces &item : items) {
    paint_2d_geometry_fill_item_discard(item);
  }
}

static bool paint_2d_geometry_fill_init_bm(Object *ob, ImagePaintUVObjectFaces &item)
{
  item.object = ob;

  if (ob->mode & OB_MODE_EDIT) {
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (!em) {
      return false;
    }
    item.bm = em->bm;
    item.owns_bm = false;
    return item.bm != nullptr;
  }

  /* Original mesh, not evaluated: same source as selection expand. */
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (!mesh) {
    return false;
  }
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
  BMeshCreateParams create_params{};
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  item.bm = BM_mesh_create(&allocsize, &create_params);
  BM_mesh_bm_from_me(item.bm, mesh, &convert_params);
  item.owns_bm = true;
  if (item.bm->totface != mesh->faces_num) {
    /* #BM_mesh_bm_from_me skips degenerate faces, which would misalign the Mesh face
     * indices used by the raycast/selection paths below with BMesh face indices. */
    paint_2d_geometry_fill_item_discard(item);
    return false;
  }
  return item.bm != nullptr;
}

static void paint_2d_geometry_fill_expand(ImagePaintUVObjectFaces &item,
                                          Scene *scene,
                                          const Brush *br,
                                          const Span<int> seed)
{
  if (br->fill_expand == IMAGE_PAINT_SELECT_EXPAND_ISLAND) {
    Array<bool> tags(item.bm->totface, false);
    ED_uvedit_uv_islands_tag_from_face_indices(scene, item.bm, item.offsets, seed, 0, tags);
    for (int i = 0; i < item.bm->totface; i++) {
      if (tags[i]) {
        item.faces.append(i);
      }
    }
  }
  else if (br->fill_expand == IMAGE_PAINT_SELECT_EXPAND_MESH) {
    Array<bool> tags(item.bm->totface, false);
    image_paint_tag_mesh_connected_faces(item.bm, seed, tags);
    for (int i = 0; i < item.bm->totface; i++) {
      if (tags[i]) {
        item.faces.append(i);
      }
    }
  }
  else {
    item.faces.extend(seed);
  }
}

static void paint_2d_geometry_fill_bucket_tiles(ImagePaintUVObjectFaces &item)
{
  BM_mesh_elem_table_ensure(item.bm, BM_FACE);
  for (const int face_index : item.faces) {
    BMFace *efa = BM_face_at_index(item.bm, face_index);
    if (efa == nullptr) {
      continue;
    }
    rctf face_uv_bounds;
    BLI_rctf_init_minmax(&face_uv_bounds);
    BMIter liter;
    BMLoop *l;
    BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
      const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, item.offsets.uv);
      BLI_rctf_do_minmax_v(&face_uv_bounds, uv);
    }
    const int tx_min = int(floorf(face_uv_bounds.xmin));
    const int tx_max = int(floorf(face_uv_bounds.xmax));
    const int ty_min = int(floorf(face_uv_bounds.ymin));
    const int ty_max = int(floorf(face_uv_bounds.ymax));
    for (int ty = ty_min; ty <= ty_max; ty++) {
      if (ty < 0) {
        continue;
      }
      for (int tx = tx_min; tx <= tx_max; tx++) {
        /* UDIM tiles span columns 0..9; ignore UVs outside the valid grid. */
        if (tx < 0 || tx > 9) {
          continue;
        }
        const int tile_number = 1001 + ty * 10 + tx;
        if (tile_number > IMA_UDIM_MAX) {
          continue;
        }
        item.tile_faces.lookup_or_add_default(tile_number).append(face_index);
      }
    }
  }
}

static bool paint_2d_geometry_fill_commit(const bContext *C,
                                          const float color[3],
                                          Brush *br,
                                          Image *ima,
                                          SpaceImage *sima,
                                          MutableSpan<ImagePaintUVObjectFaces> items,
                                          const bool texpaint)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  const float strength = BKE_brush_alpha_get(paint, br);

  Set<int> tile_numbers;
  for (const ImagePaintUVObjectFaces &item : items) {
    for (const int tile_number : item.tile_faces.keys()) {
      tile_numbers.add(tile_number);
    }
  }

  Vector<int> tiles;
  tiles.reserve(tile_numbers.size());
  for (const int tile_number : tile_numbers) {
    tiles.append(tile_number);
  }
  std::sort(tiles.begin(), tiles.end());

  ImBuf *last_ibuf = nullptr;
  ImageUser last_iuser{};
  bool have_last = false;

  for (const int tile_number : tiles) {
    ImageUser iuser{};
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }
    if (have_last) {
      BKE_image_release_ibuf(ima, last_ibuf, nullptr);
    }
    last_ibuf = ibuf;
    last_iuser = iuser;
    have_last = true;

    const float2 origin = image_select_udim_tile_uv_origin(tile_number);
    ED_imapaint_dirty_region(ima, ibuf, &last_iuser, 0, 0, ibuf->x, ibuf->y, false);

    for (ImagePaintUVObjectFaces &item : items) {
      const Vector<int> *faces_on_tile = item.tile_faces.lookup_ptr(tile_number);
      if (faces_on_tile == nullptr || faces_on_tile->is_empty()) {
        continue;
      }
      image_paint_rasterize_faces_to_ibuf(item.bm,
                                          item.offsets,
                                          *faces_on_tile,
                                          origin,
                                          ima,
                                          tile_number,
                                          ibuf,
                                          color,
                                          strength,
                                          IMB_BlendMode(br->blend));
    }
  }

  if (have_last) {
    imapaint_image_update(sima, ima, last_ibuf, &last_iuser, texpaint);
    ED_imapaint_clear_partial_redraw();
    BKE_image_release_ibuf(ima, last_ibuf, nullptr);

    /* Geometry fill never runs `paint_proj_stroke`, so the 3D view is not tagged
     * via `need_redraw`. Force GPU texture rebuild and shading sync like
     * `paint_2d_redraw` / `PAINT_OT_project_image`. */
    if (!(sima && sima->lock)) {
      BKE_image_free_gputextures(ima);
    }
    DEG_id_tag_update(&ima->id, 0);
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);

    const Scene *scene = CTX_data_scene(C);
    Object *object = CTX_data_active_object(C);
    if (object && object->type == OB_MESH &&
        (texpaint || (scene && scene->toolsettings->imapaint.mode == IMAGEPAINT_MODE_IMAGE)))
    {
      DEG_id_tag_update(&object->id, ID_RECALC_SHADING);
    }
    if (texpaint) {
      if (ARegion *region = CTX_wm_region(C)) {
        ED_region_tag_redraw(region);
      }
    }
  }

  paint_2d_geometry_fill_free_items(items);
  return have_last;
}

static void paint_2d_geometry_fill(const bContext *C,
                                   const float color[3],
                                   Brush *br,
                                   Image *ima,
                                   SpaceImage *sima,
                                   Scene *scene,
                                   const float2 &uv_abs)
{
  Vector<ImagePaintUVObjectFaces> items;
  const Vector<Object *> objects = image_paint_selection_canvas_objects_get(
      C, ima, ImagePaintCanvasPurpose::Fill);

  for (Object *ob : objects) {
    ImagePaintUVObjectFaces item;
    if (!paint_2d_geometry_fill_init_bm(ob, item)) {
      continue;
    }

    item.offsets = image_paint_selection_uv_offsets_get(item.bm, ob, scene);
    if (item.offsets.uv < 0) {
      paint_2d_geometry_fill_item_discard(item);
      continue;
    }

    Vector<int> seed;
    image_paint_faces_at_uv(item.bm, item.offsets, uv_abs, seed);
    if (seed.is_empty()) {
      paint_2d_geometry_fill_item_discard(item);
      continue;
    }

    /* A 2D click only yields a UV. Recover the object-space point it maps to so mirroring
     * uses the same geometry the 3D viewport would. */
    const Mesh *mesh = id_cast<const Mesh *>(ob->data);
    if (mesh != nullptr && mesh->symmetry != 0) {
      BM_mesh_elem_table_ensure(item.bm, BM_FACE);
      BMFace *seed_face = BM_face_at_index(item.bm, seed[0]);
      float3 hit_position;
      if (seed_face != nullptr &&
          image_paint_uv_to_object_position(seed_face, item.offsets, uv_abs, hit_position))
      {
        image_paint_symmetry_mirror_faces(ob, item.bm, hit_position, mesh->symmetry, seed);
      }
    }

    paint_2d_geometry_fill_expand(item, scene, br, seed);
    if (item.faces.is_empty()) {
      paint_2d_geometry_fill_item_discard(item);
      continue;
    }

    paint_2d_geometry_fill_bucket_tiles(item);
    items.append(std::move(item));
  }

  if (items.is_empty()) {
    return;
  }

  paint_2d_geometry_fill_commit(C, color, br, ima, sima, items, false);
}

/**
 * Hit the original mesh / Edit BMesh under the 3D cursor, not the evaluated mesh.
 * Face indices then match #BM_face_at_index used by 2D fill and selection expand.
 */
static bool paint_image_geometry_fill_raycast_orig_face(
    const bContext *C, Object *ob, const float mouse[2], int *r_face_index, float3 *r_hit_position)
{
  *r_face_index = -1;
  *r_hit_position = float3(0.0f);
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }

  const ARegion *region = CTX_wm_region(C);
  const View3D *v3d = CTX_wm_view3d(C);
  if (region == nullptr || v3d == nullptr) {
    return false;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  float ray_start[3], ray_normal[3];
  if (!ED_view3d_win_to_ray_clipped(depsgraph, region, v3d, mouse, ray_start, ray_normal, true)) {
    return false;
  }

  float imat[4][4];
  if (!invert_m4_m4(imat, ob->object_to_world().ptr())) {
    return false;
  }
  float ray_start_ob[3], ray_normal_ob[3];
  mul_v3_m4v3(ray_start_ob, imat, ray_start);
  copy_v3_v3(ray_normal_ob, ray_normal);
  mul_mat3_m4_v3(imat, ray_normal_ob);
  if (normalize_v3(ray_normal_ob) == 0.0f) {
    return false;
  }

  if (ob->mode & OB_MODE_EDIT) {
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (em == nullptr || em->bm == nullptr) {
      return false;
    }
    BM_mesh_elem_index_ensure(em->bm, BM_FACE);
    BMBVHTree *bmbvh = BKE_bmbvh_new_from_editmesh(em, BMBVH_RESPECT_HIDDEN, nullptr, false);
    if (bmbvh == nullptr) {
      return false;
    }
    float dist = BVH_RAYCAST_DIST_MAX;
    BMFace *f = BKE_bmbvh_ray_cast(
        bmbvh, ray_start_ob, ray_normal_ob, 0.0f, &dist, nullptr, nullptr);
    BKE_bmbvh_free(bmbvh);
    if (f == nullptr) {
      return false;
    }
    *r_face_index = BM_elem_index_get(f);
    /* `dist` holds the distance along the normalized ray at the hit. */
    *r_hit_position = float3(ray_start_ob) + float3(ray_normal_ob) * dist;
    return *r_face_index >= 0;
  }

  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (mesh == nullptr) {
    return false;
  }
  bke::BVHTreeFromMesh tree_data = mesh->bvh_corner_tris_no_hidden();
  if (tree_data.tree == nullptr) {
    return false;
  }

  BVHTreeRayHit hit{};
  hit.index = -1;
  hit.dist = BVH_RAYCAST_DIST_MAX;
  BLI_bvhtree_ray_cast(tree_data.tree,
                       ray_start_ob,
                       ray_normal_ob,
                       0.0f,
                       &hit,
                       tree_data.raycast_callback,
                       &tree_data);
  if (hit.index < 0) {
    return false;
  }
  const Span<int> tri_faces = mesh->corner_tri_faces();
  if (hit.index >= tri_faces.size()) {
    return false;
  }
  *r_face_index = tri_faces[hit.index];
  *r_hit_position = float3(hit.co);
  return *r_face_index >= 0;
}

static Image *paint_image_geometry_fill_canvas_image(const Scene *scene,
                                                     Object *ob,
                                                     const short mat_nr)
{
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  if (paint_mode.canvas_source == PAINT_CANVAS_SOURCE_IMAGE) {
    return paint_mode.canvas_image;
  }

  Material *ma = BKE_object_material_get(ob, short(mat_nr + 1));
  if (ma == nullptr) {
    ma = BKE_object_material_get(ob, ob->actcol);
  }
  if (ma && ma->texpaintslot && ma->paint_active_slot < ma->tot_slots) {
    return ma->texpaintslot[ma->paint_active_slot].ima;
  }
  return nullptr;
}

static BMUVOffsets paint_image_geometry_fill_uv_offsets(BMesh *bm,
                                                        Object *ob,
                                                        const Scene *scene,
                                                        const short mat_nr)
{
  const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  if (paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) {
    Material *ma = BKE_object_material_get(ob, short(mat_nr + 1));
    if (ma && ma->texpaintslot && ma->paint_active_slot < ma->tot_slots) {
      const char *uvname = ma->texpaintslot[ma->paint_active_slot].uvname;
      if (uvname && uvname[0]) {
        const int layer = CustomData_get_named_layer_index(&bm->ldata, CD_PROP_FLOAT2, uvname);
        if (layer != -1) {
          return BM_uv_map_offsets_from_layer(bm, layer);
        }
      }
    }
  }
  return image_paint_selection_uv_offsets_get(bm, ob, scene);
}

bool paint_image_proj_geometry_fill(
    const bContext *C, const float color[3], Brush *br, Object *ob, const float mouse[2])
{
  if (br == nullptr || ob == nullptr) {
    return false;
  }

  int face_index = -1;
  float3 hit_position;
  if (!paint_image_geometry_fill_raycast_orig_face(C, ob, mouse, &face_index, &hit_position)) {
    return false;
  }

  ImagePaintUVObjectFaces item;
  if (!paint_2d_geometry_fill_init_bm(ob, item)) {
    return false;
  }

  BM_mesh_elem_table_ensure(item.bm, BM_FACE);
  BMFace *efa = BM_face_at_index(item.bm, face_index);
  if (efa == nullptr || BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
    paint_2d_geometry_fill_item_discard(item);
    return false;
  }

  Scene *scene = CTX_data_scene(C);
  Image *ima = paint_image_geometry_fill_canvas_image(scene, ob, efa->mat_nr);
  if (ima == nullptr) {
    paint_2d_geometry_fill_item_discard(item);
    return false;
  }

  item.offsets = paint_image_geometry_fill_uv_offsets(item.bm, ob, scene, efa->mat_nr);
  if (item.offsets.uv < 0) {
    paint_2d_geometry_fill_item_discard(item);
    return false;
  }

  Vector<int> seed;
  seed.append(face_index);
  /* Mirrored faces join the seed set, so expand and rasterization stay symmetry-agnostic. */
  if (const Mesh *mesh = id_cast<const Mesh *>(ob->data)) {
    image_paint_symmetry_mirror_faces(ob, item.bm, hit_position, mesh->symmetry, seed);
  }
  paint_2d_geometry_fill_expand(item, scene, br, seed);
  if (item.faces.is_empty()) {
    paint_2d_geometry_fill_item_discard(item);
    return false;
  }

  paint_2d_geometry_fill_bucket_tiles(item);
  Vector<ImagePaintUVObjectFaces> items;
  items.append(std::move(item));
  return paint_2d_geometry_fill_commit(C, color, br, ima, nullptr, items, true);
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
  Scene *scene = CTX_data_scene(C);

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
  float uv_abs[2];
  ui::view2d_region_to_view(v2d, mouse_init[0], mouse_init[1], &uv_abs[0], &uv_abs[1]);

  float image_uv[2];
  copy_v2_v2(image_uv, uv_abs);
  float uv_origin[2];
  int tile_number = BKE_image_get_tile_from_pos(ima, image_uv, image_uv, uv_origin);
  /* BKE_image_get_tile_from_pos returns 0 for non-UDIM images, but the selection mask
   * system stores masks under the actual tile number (1001 for non-UDIM). Normalize so
   * selection mask lookups use the same key as selection operators. */
  if (tile_number == 0) {
    const ImageTile *first_tile = static_cast<const ImageTile *>(ima->tiles.first);
    if (first_tile) {
      tile_number = first_tile->tile_number;
    }
  }

  if (br != nullptr && mouse_final != nullptr && (br->flag & BRUSH_USE_GRADIENT) == 0 &&
      ELEM(br->fill_expand,
           IMAGE_PAINT_SELECT_EXPAND_FACE,
           IMAGE_PAINT_SELECT_EXPAND_ISLAND,
           IMAGE_PAINT_SELECT_EXPAND_MESH))
  {
    paint_2d_geometry_fill(C, color, br, ima, sima, scene, float2(uv_abs[0], uv_abs[1]));
    return;
  }

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
          const float mask_val = paint_2d_selection_mask_sample(
              scene, ima, tile_number, x_px, y_px);
          if (mask_val <= 0.001f) {
            continue;
          }
          float color_f_masked[4];
          copy_v4_v4(color_f_masked, color_f);
          mul_v4_fl(color_f_masked, mask_val);
          blend_color_mix_float(float_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                                float_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                                color_f_masked);
        }
      }
    }
    else {
      uchar *byte_data = ibuf->byte_data_for_write();
      for (x_px = 0; x_px < ibuf->x; x_px++) {
        for (y_px = 0; y_px < ibuf->y; y_px++) {
          const float mask_val = paint_2d_selection_mask_sample(
              scene, ima, tile_number, x_px, y_px);
          if (mask_val <= 0.001f) {
            continue;
          }
          float color_f_masked[4];
          rgba_uchar_to_float(color_f_masked, reinterpret_cast<uchar *>(&color_b));
          mul_v4_fl(color_f_masked, mask_val);
          uchar color_b_masked[4];
          rgba_float_to_uchar(color_b_masked, color_f_masked);
          blend_color_mix_byte(byte_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                               byte_data + 4 * (size_t(y_px) * ibuf->x + x_px),
                               color_b_masked);
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

    x_px = image_uv[0] * ibuf->x;
    y_px = image_uv[1] * ibuf->y;

    if (x_px >= ibuf->x || x_px < 0 || y_px >= ibuf->y || y_px < 0) {
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

        /* reconstruct the coordinates here */
        x_px = coordinate % width;
        y_px = coordinate / width;

        const float mask_val_f = paint_2d_selection_mask_sample(
            scene, ima, tile_number, x_px, y_px);
        if (mask_val_f > 0.001f) {
          float color_f_masked[4];
          copy_v4_v4(color_f_masked, color_f);
          mul_v4_fl(color_f_masked, mask_val_f);

          IMB_blend_color_float(ibuf->float_data_for_write() + 4 * coordinate,
                                ibuf->float_data_for_write() + 4 * coordinate,
                                color_f_masked,
                                IMB_BlendMode(br->blend));
        }

        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px - 1,
                                      y_px - 1,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px - 1,
                                      y_px,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px - 1,
                                      y_px + 1,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px,
                                      y_px + 1,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px,
                                      y_px - 1,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px + 1,
                                      y_px - 1,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px + 1,
                                      y_px,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
        paint_2d_fill_add_pixel_float(scene,
                                      ima,
                                      tile_number,
                                      x_px + 1,
                                      y_px + 1,
                                      ibuf,
                                      stack,
                                      touched,
                                      pixel_color,
                                      threshold_sq);
      }
    }
    else {
      while (!BLI_stack_is_empty(stack)) {
        BLI_stack_pop(stack, &coordinate);

        /* reconstruct the coordinates here */
        x_px = coordinate % width;
        y_px = coordinate / width;

        const float mask_val_b = paint_2d_selection_mask_sample(
            scene, ima, tile_number, x_px, y_px);
        if (mask_val_b > 0.001f) {
          float color_f_masked_b[4];
          rgba_uchar_to_float(color_f_masked_b, reinterpret_cast<uchar *>(&color_b));
          mul_v4_fl(color_f_masked_b, mask_val_b);
          uchar color_b_masked[4];
          rgba_float_to_uchar(color_b_masked, color_f_masked_b);

          IMB_blend_color_byte(ibuf->byte_data_for_write() + 4 * coordinate,
                               ibuf->byte_data_for_write() + 4 * coordinate,
                               color_b_masked,
                               IMB_BlendMode(br->blend));
        }

        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px - 1,
                                     y_px - 1,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px - 1,
                                     y_px,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px - 1,
                                     y_px + 1,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px,
                                     y_px + 1,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px,
                                     y_px - 1,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px + 1,
                                     y_px - 1,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px + 1,
                                     y_px,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
        paint_2d_fill_add_pixel_byte(scene,
                                     ima,
                                     tile_number,
                                     x_px + 1,
                                     y_px + 1,
                                     ibuf,
                                     stack,
                                     touched,
                                     pixel_color,
                                     threshold_sq);
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
  Scene *scene = CTX_data_scene(C);

  if (ima == nullptr || s == nullptr) {
    return;
  }

  float uv_origin[2];
  float image_init[2], image_final[2];
  ui::view2d_region_to_view(
      s->v2d, mouse_final[0], mouse_final[1], &image_final[0], &image_final[1]);
  ui::view2d_region_to_view(
      s->v2d, mouse_init[0], mouse_init[1], &image_init[0], &image_init[1]);
  int tile_number = BKE_image_get_tile_from_pos(ima, image_init, image_init, uv_origin);
  /* Normalize tile_number for non-UDIM images (same reason as in paint_2d_bucket_fill). */
  if (tile_number == 0) {
    const ImageTile *first_tile = static_cast<const ImageTile *>(ima->tiles.first);
    if (first_tile) {
      tile_number = first_tile->tile_number;
    }
  }
  ImageUser *iuser = paint_2d_get_tile_iuser(s, tile_number);
  if (!iuser) {
    return;
  }

  ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, nullptr);
  if (ibuf == nullptr) {
    return;
  }

  sub_v2_v2(image_init, uv_origin);
  sub_v2_v2(image_final, uv_origin);

  image_final[0] *= ibuf->x;
  image_final[1] *= ibuf->y;
  image_init[0] *= ibuf->x;
  image_init[1] *= ibuf->y;

  const ImagePaintGradientParams params = image_paint_gradient_params_from_brush(s->paint, br);
  const float2 start_px(image_init[0], image_init[1]);
  const float2 end_px(image_final[0], image_final[1]);

  rcti work_region;
  image_paint_gradient_calc_work_region(
      scene, ima, tile_number, ibuf->x, ibuf->y, nullptr, work_region);

  if (BLI_rcti_is_empty(&work_region)) {
    BKE_image_release_ibuf(ima, ibuf, nullptr);
    return;
  }

  ED_imapaint_dirty_region(ima,
                           ibuf,
                           iuser,
                           work_region.xmin,
                           work_region.ymin,
                           BLI_rcti_size_x(&work_region),
                           BLI_rcti_size_y(&work_region),
                           false);

  image_paint_gradient_apply_region(
      scene, ima, tile_number, ibuf, params, start_px, end_px, 0.5f, work_region);

  imapaint_image_update(sima, ima, ibuf, iuser, false);
  ED_imapaint_clear_partial_redraw();

  BKE_image_release_ibuf(ima, ibuf, nullptr);

  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

}  // namespace blender
