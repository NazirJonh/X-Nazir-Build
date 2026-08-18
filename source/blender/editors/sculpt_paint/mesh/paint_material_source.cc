/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_texture_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_interp.hh"

#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_math_color.h"
#include "BLI_math_interp.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "paint_material_source.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::material {

/* WORKAROUND: temporary printf profiling to find where per-channel source-texture sampling
 * spends time on strokes with many source images. Remove once the perf work is done.
 * Toggle all logging via PBR_PAINT_DEBUG_LOG in paint_debug.hh. */
#include "paint_debug.hh"

/**
 * True when a #TEX_IMAGE source can be bilinear-sampled from its pinned #ImBuf instead of
 * going through #RE_texture_evaluate. Anything that would change the look (nodes, UDIM, crop,
 * colorband, non-flat mapping, non-default filter/color) stays on the texture engine.
 */
static bool channel_source_image_direct_ok(const Tex *tex, const MTex *mtex, const ImBuf *ibuf)
{
  if (tex->use_nodes && tex->nodetree != nullptr) {
    return false;
  }
  if (tex->ima == nullptr || tex->ima->source == IMA_SRC_TILED) {
    return false;
  }
  if (mtex->mapping != MTEX_FLAT) {
    return false;
  }
  if (mtex->projx != PROJ_X || mtex->projy != PROJ_Y) {
    return false;
  }
  if (!ELEM(tex->extend, TEX_REPEAT, TEX_EXTEND, TEX_CLIP)) {
    return false;
  }
  if (tex->flag & TEX_COLORBAND) {
    return false;
  }
  if (tex->imaflag & TEX_IMAROT) {
    return false;
  }
  if ((tex->imaflag & TEX_INTERPOL) == 0) {
    return false;
  }
  if (tex->xrepeat > 1 || tex->yrepeat > 1) {
    return false;
  }
  if (tex->cropxmin != 0.0f || tex->cropxmax != 1.0f || tex->cropymin != 0.0f ||
      tex->cropymax != 1.0f)
  {
    return false;
  }
  if (tex->filtersize != 1.0f) {
    return false;
  }
  if (tex->bright != 1.0f || tex->contrast != 1.0f || tex->saturation != 1.0f ||
      tex->rfac != 1.0f || tex->gfac != 1.0f || tex->bfac != 1.0f)
  {
    return false;
  }
  if (ibuf->float_data() != nullptr) {
    if (ibuf->channels != 4) {
      return false;
    }
  }
  else if (ibuf->byte_data() == nullptr) {
    return false;
  }
  return true;
}

ChannelSourceSet::ChannelSourceSet(const BrushMaterialPaint &brush_paint,
                                   const PaintModeSettings &settings,
                                   const int visible_material_channels)
{
  bool any_source = false;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const bool channel_enabled = BKE_paint_material_channel_is_enabled(
        brush_paint, settings, visible_material_channels, info.channel);
    const bool alpha_masks = info.channel == PAINT_MATERIAL_CHANNEL_ALPHA &&
                              BKE_paint_material_channel_masks_stroke(
                                  brush_paint, settings, visible_material_channels);
    if (!channel_enabled && !alpha_masks) {
      continue;
    }
    const BrushMaterialPaintChannel &channel = brush_paint.channels[info.channel];
    if (!BKE_paint_material_channel_has_source(channel)) {
      continue;
    }
    ChannelSource &source = sources_[info.channel];
    BKE_paint_material_channel_effective_mtex(brush_paint, channel, source.effective_mtex);
    source.mtex = &source.effective_mtex;
    /* Resolved once per stroke, from the DNA flag, instead of on every sample. */
    source.flip_green_channel = info.channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
                                channel.normal_space == BRUSH_MATERIAL_PAINT_NORMAL_SPACE_DIRECTX;
    any_source = true;
  }

  if (!any_source) {
    return;
  }

  pool_ = BKE_image_pool_new();

#if PBR_PAINT_SOURCE_PROFILE
  const double construct_start = BLI_time_now_seconds();
  int probed_image_num = 0;
#endif

  /* Usability is decided once here rather than per sample, so the UI warning and the engine can
   * never disagree in the middle of a stroke. For IMAGE sources, probe ImBuf acquisition via the
   * stroke pool; a failed acquire marks the source unusable for the whole stroke. */
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    ChannelSource &source = sources_[i];
    if (source.mtex == nullptr) {
      continue;
    }
    const Tex *tex = source.mtex->tex;
    if (tex == nullptr) {
      source.usable = false;
      continue;
    }
    if (tex->type == TEX_IMAGE) {
      if (tex->ima == nullptr) {
        source.usable = false;
        continue;
      }
      /* Acquire may update ImageUser; keep a local copy so the const Tex on the brush is not
       * mutated through a const path. */
      ImageUser iuser = tex->iuser;
#if PBR_PAINT_SOURCE_PROFILE
      const double acquire_start = BLI_time_now_seconds();
#endif
      ImBuf *ibuf = BKE_image_pool_acquire_ibuf(tex->ima, &iuser, pool_);
#if PBR_PAINT_SOURCE_PROFILE
      /* A slow acquire here means this image was not yet in the pool (first probe for it this
       * stroke, so a real decode happened); a fast one means another channel already pulled the
       * same image in and the pool served it from cache. Compare the two to see whether the
       * per-channel Tex duplication (each channel owns its own Tex/MTex even when pointing at
       * the same Image) is causing redundant decodes across channels. */
      const double acquire_seconds = BLI_time_now_seconds() - acquire_start;
#endif
      source.usable = ibuf != nullptr;
      if (ibuf && ibuf->float_data() == nullptr) {
        source.do_linear_conversion = true;
        source.colorspace = ibuf->byte_buffer.colorspace;
      }
      /* Keep the ImBuf pinned for the stroke when a direct bilinear sample can replace
       * #RE_texture_evaluate. Otherwise release as before; usability is already decided. */
      const bool direct_sample = ibuf != nullptr &&
                                 channel_source_image_direct_ok(tex, source.mtex, ibuf);
      if (direct_sample) {
        source.ibuf = ibuf;
        source.image_direct_sample = true;
      }
      else {
        BKE_image_pool_release_ibuf(tex->ima, ibuf, pool_);
      }
#if PBR_PAINT_SOURCE_PROFILE
      printf("[pbr_paint] source probe: channel=%d image=%s size=%dx%d acquire=%.3fms %s%s\n",
             i,
             tex->ima->id.name + 2,
             ibuf != nullptr ? ibuf->x : 0,
             ibuf != nullptr ? ibuf->y : 0,
             acquire_seconds * 1000.0,
             acquire_seconds > 0.001 ? "(likely decode)" : "(cache hit)",
             direct_sample ? " direct_sample" : "");
      probed_image_num++;
#endif
    }
    else {
      source.usable = true;
    }
    active_ |= source.usable;
  }

#if PBR_PAINT_SOURCE_PROFILE
  const double construct_seconds = BLI_time_now_seconds() - construct_start;
  printf("[pbr_paint] ChannelSourceSet construct: %d image probe(s), %.3fms total\n",
         probed_image_num,
         construct_seconds * 1000.0);
#endif
}

ChannelSourceSet::~ChannelSourceSet()
{
  if (pool_ != nullptr) {
    for (ChannelSource &source : sources_) {
      if (source.ibuf == nullptr || source.mtex == nullptr || source.mtex->tex == nullptr ||
          source.mtex->tex->ima == nullptr)
      {
        continue;
      }
      BKE_image_pool_release_ibuf(source.mtex->tex->ima, source.ibuf, pool_);
      source.ibuf = nullptr;
    }
    BKE_image_pool_free(pool_);
  }
}

bool ChannelSourceSet::is_active() const
{
  return active_;
}

bool ChannelSourceSet::channel_source_failed(const eMaterialPaintChannel channel) const
{
  const ChannelSource &source = sources_[channel];
  return source.mtex != nullptr && !source.usable;
}

const ChannelSourceSet::ChannelSource &ChannelSourceSet::source(const int channel) const
{
  return sources_[channel];
}

ImagePool *ChannelSourceSet::pool() const
{
  return pool_;
}

static void attach_direct_sample_image(DirectSampleLayout &layout,
                                       const ChannelSourceSet::ChannelSource &source,
                                       const MTex &mtex)
{
  if (layout.kind == DirectSampleKind::Disabled) {
    return;
  }
  if (source.ibuf == nullptr || mtex.tex == nullptr) {
    layout.kind = DirectSampleKind::Disabled;
    return;
  }
  const ImBuf *ibuf = source.ibuf;
  if (ibuf->x <= 0 || ibuf->y <= 0) {
    layout.kind = DirectSampleKind::Disabled;
    return;
  }
  layout.float_pixels = ibuf->float_data();
  layout.byte_pixels = reinterpret_cast<const uchar *>(ibuf->byte_data());
  if (layout.float_pixels == nullptr && layout.byte_pixels == nullptr) {
    layout.kind = DirectSampleKind::Disabled;
    return;
  }
  layout.ibuf_x = ibuf->x;
  layout.ibuf_y = ibuf->y;
  const Tex *tex = mtex.tex;
  layout.wrap = tex->extend == TEX_REPEAT;
  layout.clip = tex->extend == TEX_CLIP;
  layout.eval_size_x = mtex.size[0];
  layout.eval_size_y = mtex.size[1];
  layout.eval_ofs_x = mtex.ofs[0];
  layout.eval_ofs_y = mtex.ofs[1];
}

DirectSampleLayout make_direct_sample_layout(const ChannelSourceSet::ChannelSource &source,
                                             const MTex &mtex,
                                             const bke::PaintRuntime *paint_runtime,
                                             const Brush &brush,
                                             const float4x4 *area_local_mat)
{
  DirectSampleLayout layout;
  if (!source.image_direct_sample) {
    return layout;
  }

  layout.size_x = mtex.size[0];
  layout.size_y = mtex.size[1];
  layout.ofs_x = mtex.ofs[0];
  layout.ofs_y = mtex.ofs[1];
  layout.sample_bias = brush.texture_sample_bias;

  if (mtex.brush_map_mode == MTEX_MAP_MODE_AREA) {
    if (area_local_mat == nullptr) {
      return layout;
    }
    layout.kind = DirectSampleKind::Area;
    layout.local_mat = area_local_mat;
  }
  else if (mtex.brush_map_mode == MTEX_MAP_MODE_3D) {
    return layout;
  }
  else if (mtex.brush_map_mode == MTEX_MAP_MODE_STENCIL) {
    layout.kind = DirectSampleKind::Stencil;
    layout.rotation = -mtex.rot;
    layout.rotate = layout.rotation > 0.001f || layout.rotation < -0.001f;
    layout.stencil_pos_x = brush.stencil_pos[0];
    layout.stencil_pos_y = brush.stencil_pos[1];
    layout.stencil_dim_x = brush.stencil_dimension[0];
    layout.stencil_dim_y = brush.stencil_dimension[1];
  }
  else {
    if (paint_runtime == nullptr) {
      return layout;
    }
    layout.rotation = -mtex.rot;
    if (mtex.brush_map_mode == MTEX_MAP_MODE_VIEW) {
      layout.kind = DirectSampleKind::View;
      layout.rotation -= paint_runtime->brush_rotation;
      layout.tex_mouse_x = paint_runtime->tex_mouse[0];
      layout.tex_mouse_y = paint_runtime->tex_mouse[1];
      layout.invradius = 1.0f / paint_runtime->pixel_radius;
    }
    else if (mtex.brush_map_mode == MTEX_MAP_MODE_TILED) {
      layout.kind = DirectSampleKind::Tiled;
      layout.invradius = 1.0f / paint_runtime->start_pixel_radius;
    }
    else if (mtex.brush_map_mode == MTEX_MAP_MODE_RANDOM) {
      layout.kind = DirectSampleKind::Random;
      layout.rotation -= paint_runtime->brush_rotation;
      layout.tex_mouse_x = paint_runtime->tex_mouse[0];
      layout.tex_mouse_y = paint_runtime->tex_mouse[1];
      layout.invradius = 1.0f / paint_runtime->pixel_radius;
    }
    else {
      return layout;
    }
    layout.rotate = layout.rotation > 0.001f || layout.rotation < -0.001f;
  }

  attach_direct_sample_image(layout, source, mtex);
  return layout;
}

static void apply_direct_sample_rotation(const DirectSampleLayout &layout, float &x, float &y)
{
  if (!layout.rotate) {
    return;
  }
  const float angle = atan2f(y, x) + layout.rotation;
  const float flen = sqrtf(x * x + y * y);
  x = flen * cosf(angle);
  y = flen * sinf(angle);
}

static bool sample_layout_image(const DirectSampleLayout &layout,
                                const float tex_x,
                                const float tex_y,
                                float *r_value,
                                float4 &r_rgba)
{
  if (layout.ibuf_x <= 0 || layout.ibuf_y <= 0) {
    return false;
  }

  /* Same placement as #RE_texture_evaluate: size/ofs applied again on top of the already
   * mapped \a tex_x / \a tex_y, then #do_2d_mapping for #MTEX_FLAT. */
  const float vx = layout.eval_size_x * (tex_x + layout.eval_ofs_x);
  const float vy = layout.eval_size_y * (tex_y + layout.eval_ofs_y);
  const float u = (vx + 1.0f) * 0.5f;
  const float v = (vy + 1.0f) * 0.5f;

  if (layout.clip) {
    const int x = int(math::floor(u * float(layout.ibuf_x)));
    const int y = int(math::floor(v * float(layout.ibuf_y)));
    if (x < 0 || y < 0 || x >= layout.ibuf_x || y >= layout.ibuf_y) {
      r_rgba = float4(0.0f);
      if (r_value != nullptr) {
        *r_value = 0.0f;
      }
      return true;
    }
  }

  /* Pixel coordinates matching #imagewrap's `fx * ibuf->x` (no half-texel offset). */
  const float px = u * float(layout.ibuf_x);
  const float py = v * float(layout.ibuf_y);

  if (layout.float_pixels != nullptr) {
    r_rgba = layout.wrap ? math::interpolate_bilinear_wrap_fl(
                               layout.float_pixels, layout.ibuf_x, layout.ibuf_y, px, py) :
                           math::interpolate_bilinear_fl(
                               layout.float_pixels, layout.ibuf_x, layout.ibuf_y, px, py);
  }
  else {
    const uchar4 col = layout.wrap ? math::interpolate_bilinear_wrap_byte(
                                         layout.byte_pixels, layout.ibuf_x, layout.ibuf_y, px, py) :
                                     math::interpolate_bilinear_byte(
                                         layout.byte_pixels, layout.ibuf_x, layout.ibuf_y, px, py);
    rgba_uchar_to_float(r_rgba, col);
  }

  if (r_value != nullptr) {
    *r_value = IMB_colormanagement_get_luminance(r_rgba);
  }
  return true;
}

/**
 * #make_direct_sample_layout for the Sculpt path, which reaches the mapping state through the
 * stroke cache and defaults #MTEX_MAP_MODE_AREA to the brush's shared local matrix.
 */
static DirectSampleLayout make_direct_sample_layout_sculpt(
    const ChannelSourceSet::ChannelSource &source,
    const SculptSession &ss,
    const Brush &brush,
    const float4x4 *area_local_mat)
{
  if (ss.cache == nullptr || source.mtex == nullptr) {
    return {};
  }
  const bke::PaintRuntime *paint_runtime = ss.cache->paint != nullptr ? ss.cache->paint->runtime :
                                                                       nullptr;
  return make_direct_sample_layout(
      source,
      *source.mtex,
      paint_runtime,
      brush,
      area_local_mat != nullptr ? area_local_mat : &ss.cache->brush_local_mat);
}

bool sample_direct_layout(const DirectSampleLayout &layout,
                          const float3 &symm_point,
                          const float2 &view_point_2d,
                          float *r_value,
                          float4 &r_rgba)
{
  float tex_x = 0.0f;
  float tex_y = 0.0f;
  switch (layout.kind) {
    case DirectSampleKind::Disabled: {
      return false;
    }
    case DirectSampleKind::Area: {
      float3 point = symm_point;
      mul_m4_v3(layout.local_mat->ptr(), point);
      tex_x = point.x * layout.size_x + layout.ofs_x;
      tex_y = point.y * layout.size_y + layout.ofs_y;
      break;
    }
    case DirectSampleKind::Stencil: {
      float x = view_point_2d[0] - layout.stencil_pos_x;
      float y = view_point_2d[1] - layout.stencil_pos_y;
      apply_direct_sample_rotation(layout, x, y);
      if (fabsf(x) > layout.stencil_dim_x || fabsf(y) > layout.stencil_dim_y) {
        zero_v4(r_rgba);
        if (r_value != nullptr) {
          *r_value = 0.0f;
        }
        return true;
      }
      tex_x = x / layout.stencil_dim_x;
      tex_y = y / layout.stencil_dim_y;
      break;
    }
    case DirectSampleKind::View:
    case DirectSampleKind::Random: {
      float x = (view_point_2d[0] - layout.tex_mouse_x) * layout.invradius;
      float y = (view_point_2d[1] - layout.tex_mouse_y) * layout.invradius;
      apply_direct_sample_rotation(layout, x, y);
      tex_x = x;
      tex_y = y;
      break;
    }
    case DirectSampleKind::Tiled: {
      float x = view_point_2d[0] * layout.invradius;
      float y = view_point_2d[1] * layout.invradius;
      apply_direct_sample_rotation(layout, x, y);
      tex_x = x;
      tex_y = y;
      break;
    }
  }

  if (!sample_layout_image(layout, tex_x, tex_y, r_value, r_rgba)) {
    return false;
  }
  /* Match #sculpt_apply_texture (Area) vs #BKE_brush_sample_tex_3d (view-relative) bias. */
  if (layout.kind == DirectSampleKind::Area) {
    add_v3_fl(r_rgba, layout.sample_bias);
    if (r_value != nullptr) {
      *r_value -= layout.sample_bias;
    }
  }
  else if (r_value != nullptr) {
    *r_value += layout.sample_bias;
  }
  return true;
}

static void sample_channel_source_with_layout(const DirectSampleLayout &layout,
                                              const ChannelSourceSet &sources,
                                              const ChannelSourceSet::ChannelSource &source,
                                              const SculptSession &ss,
                                              const Brush &brush,
                                              const TexelSampleContext &ctx,
                                              const int thread,
                                              const float4x4 *area_local_mat,
                                              float *r_value,
                                              float4 &r_rgba)
{
  if (sample_direct_layout(layout, ctx.symm_point, ctx.view_point_2d, r_value, r_rgba)) {
    return;
  }
  float dummy_value = 0.0f;
  float *value_ptr = r_value != nullptr ? r_value : &dummy_value;
  sculpt_apply_texture(ss,
                       brush,
                       *source.mtex,
                       ctx,
                       thread,
                       value_ptr,
                       r_rgba,
                       sources.pool(),
                       area_local_mat);
}

static void sample_channel_source(const ChannelSourceSet &sources,
                                  const ChannelSourceSet::ChannelSource &source,
                                  const SculptSession &ss,
                                  const Brush &brush,
                                  const TexelSampleContext &ctx,
                                  const int thread,
                                  const float4x4 *area_local_mat,
                                  float *r_value,
                                  float4 &r_rgba)
{
  const DirectSampleLayout layout = make_direct_sample_layout_sculpt(
      source, ss, brush, area_local_mat);
  sample_channel_source_with_layout(
      layout, sources, source, ss, brush, ctx, thread, area_local_mat, r_value, r_rgba);
}

bool ChannelSourceSet::sample_image_direct(const ChannelSource &source,
                                           const float tex_x,
                                           const float tex_y,
                                           float *r_value,
                                           float4 &r_rgba) const
{
  if (!source.image_direct_sample || source.ibuf == nullptr || source.mtex == nullptr) {
    return false;
  }

  const MTex *mtex = source.mtex;
  const Tex *tex = mtex->tex;
  const ImBuf *ibuf = source.ibuf;
  if (tex == nullptr || ibuf->x <= 0 || ibuf->y <= 0) {
    return false;
  }

  /* Same placement as #RE_texture_evaluate: size/ofs applied again on top of the already
   * mapped \a tex_x / \a tex_y, then #do_2d_mapping for #MTEX_FLAT. */
  const float vx = mtex->size[0] * (tex_x + mtex->ofs[0]);
  const float vy = mtex->size[1] * (tex_y + mtex->ofs[1]);
  const float u = (vx + 1.0f) * 0.5f;
  const float v = (vy + 1.0f) * 0.5f;

  if (tex->extend == TEX_CLIP) {
    const int x = int(math::floor(u * float(ibuf->x)));
    const int y = int(math::floor(v * float(ibuf->y)));
    if (x < 0 || y < 0 || x >= ibuf->x || y >= ibuf->y) {
      r_rgba = float4(0.0f);
      if (r_value != nullptr) {
        *r_value = 0.0f;
      }
      return true;
    }
  }

  /* Pixel coordinates matching #imagewrap's `fx * ibuf->x` (no half-texel offset). */
  const float px = u * float(ibuf->x);
  const float py = v * float(ibuf->y);
  const bool wrap = tex->extend == TEX_REPEAT;

  if (ibuf->float_data() != nullptr) {
    r_rgba = wrap ? imbuf::interpolate_bilinear_wrap_fl(ibuf, px, py) :
                    imbuf::interpolate_bilinear_fl(ibuf, px, py);
  }
  else {
    const uchar4 col = wrap ? imbuf::interpolate_bilinear_wrap_byte(ibuf, px, py) :
                              imbuf::interpolate_bilinear_byte(ibuf, px, py);
    rgba_uchar_to_float(r_rgba, col);
  }

  if (r_value != nullptr) {
    *r_value = IMB_colormanagement_get_luminance(r_rgba);
  }
  return true;
}

ChannelSourceSampler::ChannelSourceSampler(const SculptSession &ss,
                                            const Brush &brush,
                                            const BrushMaterialPaint &brush_paint,
                                            const PaintModeSettings &settings,
                                            const int visible_material_channels)
    : ss_(ss),
      brush_(brush),
      settings_(settings),
      brush_paint_(brush_paint),
      sources_(brush_paint, settings, visible_material_channels)
{
}

bool ChannelSourceSampler::is_active() const
{
  return sources_.is_active();
}

void ChannelSourceSampler::update_area_local_mats(const Object &ob)
{
  if (!sources_.is_active()) {
    return;
  }
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const ChannelSource &source = sources_.source(i);
    if (!source.usable || source.mtex->brush_map_mode != MTEX_MAP_MODE_AREA) {
      continue;
    }
    area_local_mats_[i] = calc_area_local_mat(ob, source.mtex->rot);
  }
}

bool ChannelSourceSampler::channel_source_failed(const eMaterialPaintChannel channel) const
{
  return sources_.channel_source_failed(channel);
}

bool ChannelSourceSampler::has_usable_source(const eMaterialPaintChannel channel) const
{
  return sources_.source(channel).usable;
}

const float4x4 *ChannelSourceSampler::area_local_mat_for(const int channel,
                                                         const ChannelSource &source) const
{
  if (source.mtex->brush_map_mode != MTEX_MAP_MODE_AREA) {
    return nullptr;
  }
  return &area_local_mats_[channel];
}

float ChannelSourceSampler::scalar(const eMaterialPaintChannel channel,
                                   const float3 &position,
                                   const int thread) const
{
  const float2 range = BKE_paint_material_channel_range(settings_, channel);
  const float fallback = BKE_paint_material_channel_value(brush_paint_, settings_, channel);

  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    return fallback;
  }

  float value;
  float4 rgba;
  const TexelSampleContext ctx = sculpt_texel_sample_context(ss_, position);
  sample_channel_source(sources_,
                        source,
                        ss_,
                        brush_,
                        ctx,
                        thread,
                        area_local_mat_for(channel, source),
                        &value,
                        rgba);
  /* Scalar channels use the texture's intensity (mask/factor), not its color, so unlike
   * #color() below there is no RGB to decode here. */
  return math::clamp(value, range.x, range.y);
}

float ChannelSourceSampler::scalar(const eMaterialPaintChannel channel,
                                   const TexelSampleContext &ctx,
                                   const int thread) const
{
  const float2 range = BKE_paint_material_channel_range(settings_, channel);
  const float fallback = BKE_paint_material_channel_value(brush_paint_, settings_, channel);

  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    return fallback;
  }

  float value;
  float4 rgba;
  sample_channel_source(sources_,
                        source,
                        ss_,
                        brush_,
                        ctx,
                        thread,
                        area_local_mat_for(channel, source),
                        &value,
                        rgba);
  return math::clamp(value, range.x, range.y);
}

bool ChannelSourceSampler::needs_linear_conversion(const eMaterialPaintChannel channel) const
{
  return sources_.source(channel).do_linear_conversion;
}

const ocio::ColorSpace *ChannelSourceSampler::colorspace(const eMaterialPaintChannel channel) const
{
  return sources_.source(channel).colorspace;
}

void ChannelSourceSampler::decode_linear_batch(MutableSpan<float3> colors,
                                               const ocio::ColorSpace *colorspace)
{
  if (colors.is_empty()) {
    return;
  }
  IMB_colormanagement_colorspace_to_scene_linear(
      reinterpret_cast<float *>(colors.data()), int(colors.size()), 1, 3, colorspace, false);
}

static float3 color_channel_fallback(const eMaterialPaintChannel channel,
                                     const BrushMaterialPaint &brush_paint,
                                     const SculptSession &ss,
                                     const Brush &brush)
{
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const bool is_base_color = channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const bool is_emission = channel == PAINT_MATERIAL_CHANNEL_EMISSION;
  UNUSED_VARS_NDEBUG(is_emission);
  /* Base Color and Emission are the non-Normal channels that sample as a color; any other
   * channel reaching here would need its own fallback rather than silently reusing Base Color's.
   */
  BLI_assert(is_normal || is_base_color || is_emission);
  if (is_normal) {
    return float3(brush_paint.channels[channel].value);
  }
  if (is_base_color) {
    return BKE_paint_material_base_color_get(brush_paint, *ss.cache->paint, brush, false);
  }
  return float3(brush_paint.channels[channel].value);
}

/** Shared tail of both #color overloads: turns a raw \a rgba sample into the channel's target
 * color/normal. Factored out so the \a position and #TexelSampleContext overloads only differ in
 * how they call #sculpt_apply_texture, not in how the result is interpreted. */
static float3 finish_color_sample(const bool is_normal,
                                  const float3 &fallback,
                                  const bool do_linear_conversion,
                                  const ocio::ColorSpace *colorspace,
                                  const float4 &rgba,
                                  const bool decode_linear,
                                  const bool flip_green)
{
  if (!is_normal) {
    /* Base Color is the only color channel here; the Normal map below is tangent data and must
     * never be gamma-decoded. */
    if (do_linear_conversion && decode_linear) {
      float4 decoded = rgba;
      IMB_colormanagement_colorspace_to_scene_linear_v3(decoded, colorspace);
      return float3(decoded.x, decoded.y, decoded.z);
    }
    return float3(rgba.x, rgba.y, rgba.z);
  }

  float3 decal_normal;
  /* DirectX-convention normal maps store the green channel flipped relative to OpenGL; flip it
   * back before unpacking so both conventions land in the same tangent-space basis. */
  const float rgb[3] = {rgba.x, flip_green ? 1.0f - rgba.y : rgba.y, rgba.z};
  /* A degenerate sample cannot be normalized; the channel's own tangent is the safe answer. */
  if (!BKE_paint_material_normal_from_sample(rgb, decal_normal)) {
    return fallback;
  }
  /* Still in the decal's own basis (screen right/up/backward for a View-mapped texture); the
   * caller re-expresses this relative to the destination surface (surface-attached: a neutral
   * decal stays neutral regardless of the surface's orientation to the camera), which needs the
   * surface normal this function does not have access to. */
  return decal_normal;
}

float3 ChannelSourceSampler::color(const eMaterialPaintChannel channel,
                                   const float3 &position,
                                   const int thread,
                                   const bool decode_linear) const
{
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const float3 fallback = color_channel_fallback(channel, brush_paint_, ss_, brush_);

  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    return fallback;
  }

  float4 rgba;
  const TexelSampleContext ctx = sculpt_texel_sample_context(ss_, position);
  sample_channel_source(sources_,
                        source,
                        ss_,
                        brush_,
                        ctx,
                        thread,
                        area_local_mat_for(channel, source),
                        nullptr,
                        rgba);
  return finish_color_sample(is_normal,
                             fallback,
                             source.do_linear_conversion,
                             source.colorspace,
                             rgba,
                             decode_linear,
                             source.flip_green_channel);
}

float3 ChannelSourceSampler::color(const eMaterialPaintChannel channel,
                                   const TexelSampleContext &ctx,
                                   const int thread,
                                   const bool decode_linear) const
{
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const float3 fallback = color_channel_fallback(channel, brush_paint_, ss_, brush_);

  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    return fallback;
  }

  float4 rgba;
  sample_channel_source(sources_,
                        source,
                        ss_,
                        brush_,
                        ctx,
                        thread,
                        area_local_mat_for(channel, source),
                        nullptr,
                        rgba);
  return finish_color_sample(is_normal,
                             fallback,
                             source.do_linear_conversion,
                             source.colorspace,
                             rgba,
                             decode_linear,
                             source.flip_green_channel);
}

void ChannelSourceSampler::gather_colors(const eMaterialPaintChannel channel,
                                         const Span<TexelSampleContext> contexts,
                                         const Span<float> factors,
                                         const int thread,
                                         const bool decode_linear,
                                         MutableSpan<float3> r_colors) const
{
  BLI_assert(contexts.size() == factors.size());
  BLI_assert(r_colors.size() == contexts.size());

  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const float3 fallback = color_channel_fallback(channel, brush_paint_, ss_, brush_);
  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    for (const int i : contexts.index_range()) {
      r_colors[i] = factors[i] == 0.0f ? float3(0.0f) : fallback;
    }
    return;
  }

  const float4x4 *area_local_mat = area_local_mat_for(channel, source);
  const DirectSampleLayout layout = make_direct_sample_layout_sculpt(
      source, ss_, brush_, area_local_mat);

  for (const int i : contexts.index_range()) {
    if (factors[i] == 0.0f) {
      r_colors[i] = float3(0.0f);
      continue;
    }
    float4 rgba;
    sample_channel_source_with_layout(layout,
                                      sources_,
                                      source,
                                      ss_,
                                      brush_,
                                      contexts[i],
                                      thread,
                                      area_local_mat,
                                      nullptr,
                                      rgba);
    r_colors[i] = finish_color_sample(is_normal,
                                      fallback,
                                      source.do_linear_conversion,
                                      source.colorspace,
                                      rgba,
                                      decode_linear,
                                      source.flip_green_channel);
  }
}

void ChannelSourceSampler::gather_scalars(const eMaterialPaintChannel channel,
                                          const Span<TexelSampleContext> contexts,
                                          const Span<float> factors,
                                          const int thread,
                                          MutableSpan<float> r_values) const
{
  BLI_assert(contexts.size() == factors.size());
  BLI_assert(r_values.size() == contexts.size());

  const float2 range = BKE_paint_material_channel_range(settings_, channel);
  const float fallback = BKE_paint_material_channel_value(brush_paint_, settings_, channel);
  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    for (const int i : contexts.index_range()) {
      r_values[i] = factors[i] == 0.0f ? 0.0f : fallback;
    }
    return;
  }

  const float4x4 *area_local_mat = area_local_mat_for(channel, source);
  const DirectSampleLayout layout = make_direct_sample_layout_sculpt(
      source, ss_, brush_, area_local_mat);

  for (const int i : contexts.index_range()) {
    if (factors[i] == 0.0f) {
      r_values[i] = 0.0f;
      continue;
    }
    float value;
    float4 rgba;
    sample_channel_source_with_layout(layout,
                                      sources_,
                                      source,
                                      ss_,
                                      brush_,
                                      contexts[i],
                                      thread,
                                      area_local_mat,
                                      &value,
                                      rgba);
    r_values[i] = math::clamp(value, range.x, range.y);
  }
}

static float3 remap_decal_normal_to_packed_tangent(const float3 &n_d,
                                                   const float3 &t_screen,
                                                   const float3 &b_screen,
                                                   const float3 &n_m,
                                                   const float3 &t_m,
                                                   const float3 &b_m)
{
  const float3 n_local = n_d.x * t_screen + n_d.y * b_screen + n_d.z * n_m;
  float3 n_t(math::dot(n_local, t_m), math::dot(n_local, b_m), math::dot(n_local, n_m));
  const float n_t_len = math::length(n_t);
  n_t = n_t_len > 1e-6f ? n_t / n_t_len : float3(0.0f, 0.0f, 1.0f);
  float packed[3];
  BKE_pbr_normal_pack(n_t, false, packed);
  return float3(packed[0], packed[1], packed[2]);
}

void ChannelSourceSampler::gather_tangent_normals_packed(const eMaterialPaintChannel channel,
                                                         const Span<TexelSampleContext> contexts,
                                                         const Span<float> factors,
                                                         const int thread,
                                                         const float3 &t_screen,
                                                         const float3 &b_screen,
                                                         const float3 &n_m,
                                                         const float3 &t_m,
                                                         const float3 &b_m,
                                                         MutableSpan<float3> r_packed) const
{
  BLI_assert(channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  BLI_assert(contexts.size() == factors.size());
  BLI_assert(r_packed.size() == contexts.size());

  const float3 fallback = color_channel_fallback(channel, brush_paint_, ss_, brush_);
  const ChannelSource &source = sources_.source(channel);
  if (!source.usable) {
    for (const int i : contexts.index_range()) {
      if (factors[i] == 0.0f) {
        r_packed[i] = float3(0.0f);
        continue;
      }
      r_packed[i] = remap_decal_normal_to_packed_tangent(
          fallback, t_screen, b_screen, n_m, t_m, b_m);
    }
    return;
  }

  const float4x4 *area_local_mat = area_local_mat_for(channel, source);
  const DirectSampleLayout layout = make_direct_sample_layout_sculpt(
      source, ss_, brush_, area_local_mat);

  for (const int i : contexts.index_range()) {
    if (factors[i] == 0.0f) {
      r_packed[i] = float3(0.0f);
      continue;
    }
    float4 rgba;
    sample_channel_source_with_layout(layout,
                                      sources_,
                                      source,
                                      ss_,
                                      brush_,
                                      contexts[i],
                                      thread,
                                      area_local_mat,
                                      nullptr,
                                      rgba);
    const float3 n_d = finish_color_sample(true,
                                           fallback,
                                           source.do_linear_conversion,
                                           source.colorspace,
                                           rgba,
                                           true,
                                           source.flip_green_channel);
    r_packed[i] = remap_decal_normal_to_packed_tangent(
        n_d, t_screen, b_screen, n_m, t_m, b_m);
  }
}

}  // namespace blender::ed::sculpt_paint::material
