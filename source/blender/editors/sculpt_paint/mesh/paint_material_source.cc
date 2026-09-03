/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_resolve.hh"
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

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ED_material_bake.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"
/* Toggle all PBR debug logging via PBR_PAINT_DEBUG_LOG in paint_debug.hh. */
#include "paint_debug.hh"
#include "paint_material_source.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::material {

/**
 * True when the #ImBuf behind a #ShaderNodeTexImage source can be bilinear-sampled through
 * \a mtex's placement.
 *
 * Shorter than #channel_source_image_direct_ok because there is no #Tex: an Image Texture node
 * has no crop, filter, colorband or repeat of its own, so only the shared placement and the
 * buffer's own layout can rule it out.
 */
static bool channel_source_image_node_direct_ok(const MTex &mtex, const ImBuf *ibuf)
{
  if (mtex.mapping != MTEX_FLAT) {
    return false;
  }
  if (mtex.projx != PROJ_X || mtex.projy != PROJ_Y) {
    return false;
  }
  if (ibuf->x <= 0 || ibuf->y <= 0) {
    return false;
  }
  if (ibuf->float_data() != nullptr) {
    return ibuf->channels == 4;
  }
  return ibuf->byte_data() != nullptr;
}

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
  const MaterialSourceResolve material_resolve =
      brush_paint.source_mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL ?
          BKE_paint_material_source_resolve(brush_paint.source_material) :
          MaterialSourceResolve{};
  if (brush_paint.source_mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL &&
      brush_paint.source_material != nullptr)
  {
    bool any_baked = false;
    for (const ChannelResolution resolution : material_resolve.channels) {
      any_baked |= resolution == ChannelResolution::Baked;
    }
    if (any_baked) {
      /* Whatever is cached right now; the bake itself runs in a job (see
       * #material_source_bake_ensure), so a stroke never waits on a render. A channel with no bake
       * yet keeps painting its own value until one lands. */
      bake_ = ed::material_bake::material_source_bake_get(*brush_paint.source_material,
                                                          brush_paint.source_bake_size);
#if PBR_PAINT_BAKE_DEBUG
      printf("[PBR-BAKE] stroke: source_mode=MATERIAL mat='%s' bake_size=%d cached=%d layout=%d\n",
             brush_paint.source_material->id.name + 2,
             brush_paint.source_bake_size,
             int(bake_ != nullptr),
             int(brush_paint.source_layout));
      fflush(stdout);
#endif
    }
  }
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
    ChannelSource &source = sources_[info.channel];
    if (brush_paint.source_mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL) {
      switch (material_resolve.channels[info.channel]) {
        case ChannelResolution::Constant: {
          source.kind = ChannelSourceKind::Constant;
          source.constant_value = material_resolve.constants[info.channel];
          source.usable = true;
          source.flip_green_channel = false;
          any_source = true;
          break;
        }
        case ChannelResolution::Image: {
          const ChannelSourceImage &resolved = material_resolve.images[info.channel];
          source.kind = ChannelSourceKind::Image;
          source.image = resolved.image;
          source.image_iuser = resolved.iuser;
          /* The node carries no placement of its own, so the source is laid out by the mapping
           * every channel shares - the same one the Maps slots use. Built through the same helper
           * rather than from #shared_source_mapping directly: that #MTex only carries
           * `brush_map_mode`, and projection and size come from the channel's own defaulted slot.
           * The #Tex is dropped because the pixels come from the node's #Image instead. */
          BKE_paint_material_channel_effective_mtex(brush_paint, channel, source.effective_mtex);
          source.effective_mtex.tex = nullptr;
          source.mtex = &source.effective_mtex;
          source.flip_green_channel = info.channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
                                      channel.normal_space ==
                                          BRUSH_MATERIAL_PAINT_NORMAL_SPACE_DIRECTX;
          any_source = true;
          break;
        }
        case ChannelResolution::Baked: {
          source.kind = ChannelSourceKind::Baked;
          source.ibuf = bake_ != nullptr ?
                            const_cast<ImBuf *>(bake_->channel_image(info.channel)) :
                            nullptr;
          BKE_paint_material_channel_effective_mtex(brush_paint, channel, source.effective_mtex);
          source.effective_mtex.tex = nullptr;
          source.mtex = &source.effective_mtex;
          source.baked_target_uv =
              brush_paint.source_layout == BRUSH_MATERIAL_PAINT_SOURCE_LAYOUT_TARGET_UV;
          /* Target UV placement is sampled through #baked_target_uv, not the brush-relative
           * direct layout below: #sample_channel_source_with_layout tries the direct layout
           * first, so leaving this true would silently sample the bake with the brush's Area/View
           * mapping instead of the mesh UV it was actually baked against. */
          /* The direct samplers stride a float buffer by four, so a buffer of any other shape
           * would be read out of bounds. #bake_requests_render already normalizes every baked
           * buffer to four channels; this mirrors the identical guard the Mtex and Image kinds
           * apply, so a buffer that ever stops conforming disables direct sampling instead of
           * crashing. */
          const bool baked_buffer_ok = source.ibuf != nullptr &&
                                       source.ibuf->float_data() != nullptr &&
                                       source.ibuf->channels == 4;
          source.image_direct_sample = baked_buffer_ok && !source.baked_target_uv;
          /* A baked source has no #Tex for the texture engine to fall back on, so it can only be
           * sampled through a direct layout -- and #build_direct_layout supports every map mode
           * except 3D. Left usable, a 3D-mapped channel would reach the samplers, find no layout
           * and write zeros for the whole stroke: black Base Color, zero Roughness. Unusable is
           * the documented answer instead, and leaves the channel on its own value.
           * Target UV placement carries its own coordinates and needs no layout. */
          const bool map_mode_supported = source.baked_target_uv ||
                                          source.effective_mtex.brush_map_mode !=
                                              MTEX_MAP_MODE_3D;
          source.usable = baked_buffer_ok && map_mode_supported;
#if PBR_PAINT_BAKE_DEBUG
          printf("[PBR-BAKE] stroke: channel=%d kind=Baked ibuf=%p %dx%d ch=%d float=%p "
                 "first=(%.4f, %.4f, %.4f) usable=%d direct=%d map_mode=%d size=(%.3f, %.3f)\n",
                 int(info.channel),
                 (void *)source.ibuf,
                 source.ibuf != nullptr ? source.ibuf->x : 0,
                 source.ibuf != nullptr ? source.ibuf->y : 0,
                 source.ibuf != nullptr ? source.ibuf->channels : 0,
                 source.ibuf != nullptr ? (void *)source.ibuf->float_data() : nullptr,
                 (source.ibuf != nullptr && source.ibuf->float_data() != nullptr) ?
                     source.ibuf->float_data()[0] :
                     -1.0f,
                 (source.ibuf != nullptr && source.ibuf->float_data() != nullptr) ?
                     source.ibuf->float_data()[1] :
                     -1.0f,
                 (source.ibuf != nullptr && source.ibuf->float_data() != nullptr) ?
                     source.ibuf->float_data()[2] :
                     -1.0f,
                 int(source.usable),
                 int(source.image_direct_sample),
                 int(source.effective_mtex.brush_map_mode),
                 source.effective_mtex.size[0],
                 source.effective_mtex.size[1]);
          fflush(stdout);
#endif
          source.flip_green_channel = info.channel == PAINT_MATERIAL_CHANNEL_NORMAL &&
                                      channel.normal_space ==
                                          BRUSH_MATERIAL_PAINT_NORMAL_SPACE_DIRECTX;
          any_source |= source.usable;
          break;
        }
        default:
          /* In Material mode the per-channel slots are not the configured source. */
          break;
      }
      continue;
    }
    if (!BKE_paint_material_channel_has_source(channel)) {
      continue;
    }
    source.kind = ChannelSourceKind::Mtex;
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
    if (source.kind == ChannelSourceKind::Image) {
      /* An Image source has no #Tex to fall back on, so it is usable only when the pinned buffer
       * can be sampled directly. Anything else leaves the channel on its own value rather than
       * silently painting something the material does not describe. */
      ImageUser iuser = *source.image_iuser;
      ImBuf *ibuf = BKE_image_pool_acquire_ibuf(source.image, &iuser, pool_);
      const bool direct_sample = ibuf != nullptr &&
                                 channel_source_image_node_direct_ok(*source.mtex, ibuf);
      if (direct_sample) {
        if (ibuf->float_data() == nullptr) {
          source.do_linear_conversion = true;
          source.colorspace = ibuf->byte_buffer.colorspace;
        }
        source.ibuf = ibuf;
        source.image_direct_sample = true;
        source.usable = true;
      }
      else if (ibuf != nullptr) {
        BKE_image_pool_release_ibuf(source.image, ibuf, pool_);
      }
      active_ |= source.usable;
      continue;
    }
    if (source.kind != ChannelSourceKind::Mtex) {
      /* Constant and Baked settled their usability in the Material-mode branch above and own no
       * pooled #ImBuf to probe here, but they still make the sampler active. Without this the
       * whole sampler reported #is_active false for a Material-mode stroke, callers dropped it
       * (see #active_sampler in `sculpt_paint_image.cc`), and nothing ever sampled the bake. */
      active_ |= source.usable;
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
        source.image = tex->ima;
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

#if PBR_PAINT_BAKE_DEBUG
  /* Why the whole sampler ends up active or not, which is what decides between painting the source
   * and painting nothing at all. */
  if (brush_paint.source_mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL) {
    printf("[PBR-BAKE] stroke: SUMMARY active=%d bake=%p bake_size=%d\n",
           int(active_),
           (const void *)bake_.get(),
           brush_paint.source_bake_size);
    for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      const ChannelSource &s = sources_[i];
      printf("[PBR-BAKE] stroke:   channel=%d resolved=%d kind=%d usable=%d ibuf=%p\n",
             i,
             int(material_resolve.channels[i]),
             int(s.kind),
             int(s.usable),
             (void *)s.ibuf);
    }
    fflush(stdout);
  }
#endif

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
      /* #image is set alongside #ibuf for every kind that pins one, so the release does not have
       * to know which kind acquired it. */
      if (source.ibuf == nullptr || source.image == nullptr) {
        continue;
      }
      BKE_image_pool_release_ibuf(source.image, source.ibuf, pool_);
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
  return (source.mtex != nullptr || source.kind == ChannelSourceKind::Baked) && !source.usable;
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
  /* Image and Baked sources have no #Tex: their buffer is the whole description, so only #Mtex
   * has to prove that its texture is still there. */
  const bool has_buffer = source.ibuf != nullptr &&
                          ((source.kind == ChannelSourceKind::Image ||
                            source.kind == ChannelSourceKind::Baked) ||
                           (source.kind == ChannelSourceKind::Mtex && mtex.tex != nullptr));
  if (!has_buffer) {
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
  /* Image and Baked have no #Tex, so repeat wrapping is their complete extension description.
   * Kept in step with #ChannelSourceSet::sample_image_direct. */
  const Tex *tex = mtex.tex;
  layout.wrap = tex == nullptr || tex->extend == TEX_REPEAT;
  layout.clip = tex != nullptr && tex->extend == TEX_CLIP;
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
#if PBR_PAINT_BAKE_DEBUG
    if (source.kind == ChannelSourceKind::Baked) {
      static std::atomic_bool logged = false;
      if (!logged.exchange(true)) {
        printf("[PBR-BAKE] sculpt: direct sample OK layout.kind=%d rgba=(%.4f, %.4f, %.4f)\n",
               int(layout.kind),
               r_rgba.x,
               r_rgba.y,
               r_rgba.z);
        fflush(stdout);
      }
    }
#endif
    return;
  }
  if (source.kind == ChannelSourceKind::Baked && source.baked_target_uv) {
    /* Target UV is an explicit alternative to brush placement for baked sources. */
    if (ctx.uv.x >= 0.0f && sources.sample_baked(source, ctx.uv.x, ctx.uv.y, r_value, r_rgba)) {
      return;
    }
    r_rgba = float4(0.0f);
    if (r_value != nullptr) {
      *r_value = 0.0f;
    }
    return;
  }
  if (source.kind == ChannelSourceKind::Image || source.kind == ChannelSourceKind::Baked) {
    /* #RE_texture_evaluate needs a #Tex, while Image and brush-mapped Baked sources have none.
     * Reaching here means the direct placement could not be built, so report nothing rather than
     * dereferencing the null texture. */
#if PBR_PAINT_BAKE_DEBUG
    static std::atomic_bool black_return_logged = false;
    if (!black_return_logged.exchange(true)) {
      printf("[PBR-BAKE] sample: returning BLACK -- direct layout disabled. kind=%d "
             "layout.kind=%d direct=%d ibuf=%p map_mode=%d\n",
             int(source.kind),
             int(layout.kind),
             int(source.image_direct_sample),
             (void *)source.ibuf,
             source.mtex != nullptr ? int(source.mtex->brush_map_mode) : -1);
      fflush(stdout);
    }
#endif
    r_rgba = float4(0.0f);
    if (r_value != nullptr) {
      *r_value = 0.0f;
    }
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
  /* Image and Baked sources have no #Tex, so repeat is their complete extension description. */
  const Tex *tex = mtex->tex;
  if (tex == nullptr && source.kind != ChannelSourceKind::Image &&
      source.kind != ChannelSourceKind::Baked)
  {
    return false;
  }
  const ImBuf *ibuf = source.ibuf;
  if (ibuf->x <= 0 || ibuf->y <= 0) {
    return false;
  }

  /* Same placement as #RE_texture_evaluate: size/ofs applied again on top of the already
   * mapped \a tex_x / \a tex_y, then #do_2d_mapping for #MTEX_FLAT. */
  const float vx = mtex->size[0] * (tex_x + mtex->ofs[0]);
  const float vy = mtex->size[1] * (tex_y + mtex->ofs[1]);
  const float u = (vx + 1.0f) * 0.5f;
  const float v = (vy + 1.0f) * 0.5f;

  if (tex != nullptr && tex->extend == TEX_CLIP) {
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
  const bool wrap = tex == nullptr || tex->extend == TEX_REPEAT;

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

bool ChannelSourceSet::sample_baked(const ChannelSource &source,
                                    const float u,
                                    const float v,
                                    float *r_value,
                                    float4 &r_rgba) const
{
  if (source.kind != ChannelSourceKind::Baked || source.ibuf == nullptr || source.ibuf->x <= 0 ||
      source.ibuf->y <= 0)
  {
    return false;
  }

  const float px = u * float(source.ibuf->x);
  const float py = v * float(source.ibuf->y);
  if (source.ibuf->float_data() != nullptr) {
    r_rgba = imbuf::interpolate_bilinear_wrap_fl(source.ibuf, px, py);
  }
  else if (source.ibuf->byte_data() != nullptr) {
    const uchar4 col = imbuf::interpolate_bilinear_wrap_byte(source.ibuf, px, py);
    rgba_uchar_to_float(r_rgba, col);
  }
  else {
    return false;
  }
  if (r_value != nullptr) {
    *r_value = r_rgba.x;
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
    if (!source.usable || !channel_source_kind_has_placement(source.kind) ||
        source.mtex->brush_map_mode != MTEX_MAP_MODE_AREA)
    {
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

bool ChannelSourceSampler::uses_baked_target_uv() const
{
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const ChannelSource &source = sources_.source(i);
    if (source.usable && source.kind == ChannelSourceKind::Baked && source.baked_target_uv) {
      return true;
    }
  }
  return false;
}

const float4x4 *ChannelSourceSampler::area_local_mat_for(const int channel,
                                                         const ChannelSource &source) const
{
  if (!channel_source_kind_has_placement(source.kind) ||
      source.mtex->brush_map_mode != MTEX_MAP_MODE_AREA)
  {
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
  if (source.kind == ChannelSourceKind::Constant) {
    return math::clamp(source.constant_value.x, range.x, range.y);
  }
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
  if (source.kind == ChannelSourceKind::Constant) {
    return math::clamp(source.constant_value.x, range.x, range.y);
  }
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
                                     const Paint &paint,
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
    return BKE_paint_material_base_color_get(brush_paint, paint, brush, false);
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

/**
 * Sample \a source at explicit 2D coordinates in the caller's own parametrization.
 *
 * The channel's own Size / Offset are applied here, so the mapping controls behave the same as
 * on the position-based path; only the coordinate SOURCE differs. The direct image path is tried
 * first for the same reason it is everywhere else, and #paint_get_tex_pixel covers procedural
 * textures and any image the direct path declines.
 */
static bool sample_channel_source_at_uv(const ChannelSourceSet &sources,
                                        const ChannelSourceSet::ChannelSource &source,
                                        const float2 &uv,
                                        const int thread,
                                        float *r_value,
                                        float4 &r_rgba)
{
  const MTex *mtex = source.mtex;
  if (mtex == nullptr) {
    return false;
  }
  const float tex_x = uv.x * mtex->size[0] + mtex->ofs[0];
  const float tex_y = uv.y * mtex->size[1] + mtex->ofs[1];
  if (source.kind == ChannelSourceKind::Baked) {
    /* Placed like every other kind. Sampling the bake at the raw UV instead would make the same
     * channel jump as soon as the material stops resolving to a plain image and starts being
     * baked, since the Image path here has always applied the placement. */
    return sources.sample_baked(source, tex_x, tex_y, r_value, r_rgba);
  }
  float dummy_value = 0.0f;
  float *value_ptr = r_value != nullptr ? r_value : &dummy_value;
  if (sources.sample_image_direct(source, tex_x, tex_y, value_ptr, r_rgba)) {
    return true;
  }
  paint_get_tex_pixel(mtex, tex_x, tex_y, sources.pool(), thread, value_ptr, r_rgba);
  return true;
}

bool ChannelUvSampler::has_usable_source(const eMaterialPaintChannel channel) const
{
  return sources_.source(channel).usable;
}

float ChannelUvSampler::scalar_at_uv(const eMaterialPaintChannel channel,
                                     const float2 &uv,
                                     const int thread) const
{
  const float2 range = BKE_paint_material_channel_range(settings_, channel);
  const float fallback = BKE_paint_material_channel_value(brush_paint_, settings_, channel);

  const ChannelSourceSet::ChannelSource &source = sources_.source(channel);
  if (source.kind == ChannelSourceKind::Constant) {
    return source.constant_value.x;
  }
  if (!source.usable) {
    return fallback;
  }
  float value = fallback;
  float4 rgba;
  if (!sample_channel_source_at_uv(sources_, source, uv, thread, &value, rgba)) {
    return fallback;
  }
  return math::clamp(value, range.x, range.y);
}

float3 ChannelUvSampler::color_at_uv(const eMaterialPaintChannel channel,
                                     const float2 &uv,
                                     const int thread,
                                     const bool decode_linear) const
{
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const float3 fallback = color_channel_fallback(channel, brush_paint_, paint_, brush_);

  const ChannelSourceSet::ChannelSource &source = sources_.source(channel);
  if (source.kind == ChannelSourceKind::Constant) {
    return float3(source.constant_value);
  }
  if (!source.usable) {
    return fallback;
  }
  float4 rgba;
  if (!sample_channel_source_at_uv(sources_, source, uv, thread, nullptr, rgba)) {
    return fallback;
  }
  return finish_color_sample(is_normal,
                             fallback,
                             source.do_linear_conversion,
                             source.colorspace,
                             rgba,
                             decode_linear,
                             source.flip_green_channel);
}

const Paint &ChannelSourceSampler::paint() const
{
  return *ss_.cache->paint;
}

/** The UV view of this sampler; every `*_at_uv` method is exactly this object's counterpart. */
static ChannelUvSampler uv_view(const ChannelSourceSet &sources,
                                const BrushMaterialPaint &brush_paint,
                                const PaintModeSettings &settings,
                                const Paint &paint,
                                const Brush &brush)
{
  return ChannelUvSampler(sources, brush_paint, settings, paint, brush);
}

float ChannelSourceSampler::scalar_at_uv(const eMaterialPaintChannel channel,
                                         const float2 &uv,
                                         const int thread) const
{
  return uv_view(sources_, brush_paint_, settings_, this->paint(), brush_)
      .scalar_at_uv(channel, uv, thread);
}

float3 ChannelSourceSampler::color_at_uv(const eMaterialPaintChannel channel,
                                         const float2 &uv,
                                         const int thread,
                                         const bool decode_linear) const
{
  return uv_view(sources_, brush_paint_, settings_, this->paint(), brush_)
      .color_at_uv(channel, uv, thread, decode_linear);
}

float3 ChannelSourceSampler::color(const eMaterialPaintChannel channel,
                                   const float3 &position,
                                   const int thread,
                                   const bool decode_linear) const
{
  const bool is_normal = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  const float3 fallback = color_channel_fallback(channel, brush_paint_, this->paint(), brush_);

  const ChannelSource &source = sources_.source(channel);
  if (source.kind == ChannelSourceKind::Constant) {
    return float3(source.constant_value);
  }
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
  const float3 fallback = color_channel_fallback(channel, brush_paint_, this->paint(), brush_);

  const ChannelSource &source = sources_.source(channel);
  if (source.kind == ChannelSourceKind::Constant) {
    return float3(source.constant_value);
  }
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
  const float3 fallback = color_channel_fallback(channel, brush_paint_, this->paint(), brush_);
  const ChannelSource &source = sources_.source(channel);
  if (source.kind == ChannelSourceKind::Constant) {
    for (const int i : contexts.index_range()) {
      r_colors[i] = factors[i] == 0.0f ? float3(0.0f) : float3(source.constant_value);
    }
    return;
  }
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
  if (source.kind == ChannelSourceKind::Constant) {
    for (const int i : contexts.index_range()) {
      r_values[i] = factors[i] == 0.0f ? 0.0f :
                    math::clamp(source.constant_value.x, range.x, range.y);
    }
    return;
  }
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

void build_normal_write_basis(const float3 &tri_tangent,
                              const float tri_bitangent_sign,
                              const Span<float3> tri_positions,
                              const float3 &view_right,
                              const ARegion *region,
                              const float4x4 &projection_mat,
                              float3 &r_t_screen,
                              float3 &r_b_screen,
                              float3 &r_n_m,
                              float3 &r_t_m,
                              float3 &r_b_m)
{
  BLI_assert(tri_positions.size() == 3);
  const float3 edge1 = tri_positions[1] - tri_positions[0];
  const float3 edge2 = tri_positions[2] - tri_positions[0];
  r_n_m = math::normalize(math::cross(edge1, edge2));

  /* Screen right/up carried onto the surface, obtained from how the primitive's own screen
   * projection relates to its object-space edges. */
  bool screen_basis_ready = false;
  if (region != nullptr) {
    const float2 screen0 = ED_view3d_project_float_v2_m4(region, tri_positions[0], projection_mat);
    const float2 screen1 = ED_view3d_project_float_v2_m4(region, tri_positions[1], projection_mat);
    const float2 screen2 = ED_view3d_project_float_v2_m4(region, tri_positions[2], projection_mat);
    const float2 sx = screen1 - screen0;
    const float2 sy = screen2 - screen0;
    const float det = sx.x * sy.y - sx.y * sy.x;
    if (math::abs(det) > 1e-8f) {
      const float3 dp_dx = (edge1 * sy.y - edge2 * sx.y) / det;
      r_t_screen = dp_dx - r_n_m * math::dot(dp_dx, r_n_m);
      r_b_screen = math::cross(r_n_m, r_t_screen);
      const float t_screen_len = math::length(r_t_screen);
      const float b_screen_len = math::length(r_b_screen);
      if (t_screen_len > 1e-6f) {
        r_t_screen /= t_screen_len;
      }
      if (b_screen_len > 1e-6f) {
        r_b_screen /= b_screen_len;
      }
      screen_basis_ready = true;
    }
  }
  if (!screen_basis_ready) {
    /* Edge-on, or no view to project into: the view's own right vector flattened onto the
     * surface is the closest thing to "screen right" still available. A caller with no view at
     * all leaves `view_right` zero, so the flattening can come back empty -- any in-plane axis
     * then keeps the basis finite rather than filling the map with NaNs. */
    const float3 flat_right = view_right - r_n_m * math::dot(view_right, r_n_m);
    const float flat_right_len = math::length(flat_right);
    if (flat_right_len > 1e-6f) {
      r_t_screen = flat_right / flat_right_len;
    }
    else {
      float fallback[3];
      ortho_v3_v3(fallback, r_n_m);
      r_t_screen = math::normalize(float3(fallback));
    }
    r_b_screen = math::cross(r_n_m, r_t_screen);
  }

  r_t_m = tri_tangent - r_n_m * math::dot(tri_tangent, r_n_m);
  const float t_len = math::length(r_t_m);
  if (t_len > 1e-6f) {
    r_t_m /= t_len;
  }
  else {
    /* A tangent parallel to the normal carries no direction; any in-plane axis keeps the basis
     * finite, and a UV-less primitive has no authored orientation to preserve anyway. */
    float fallback[3];
    ortho_v3_v3(fallback, r_n_m);
    r_t_m = math::normalize(float3(fallback));
  }
  r_b_m = math::cross(r_n_m, r_t_m) * tri_bitangent_sign;
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

float3 ChannelSourceSampler::tangent_normal_packed(const eMaterialPaintChannel channel,
                                                   const TexelSampleContext &ctx,
                                                   const int thread,
                                                   const float3 &t_decal,
                                                   const float3 &b_decal,
                                                   const float3 &n_m,
                                                   const float3 &t_m,
                                                   const float3 &b_m) const
{
  BLI_assert(channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  /* #color already returns the UNPACKED decal normal for this channel -- DirectX green flip and
   * the no-source fallback included -- so only the basis change is left to do here. */
  return remap_decal_normal_to_packed_tangent(
      this->color(channel, ctx, thread), t_decal, b_decal, n_m, t_m, b_m);
}

float3 ChannelUvSampler::tangent_normal_packed_at_uv(const eMaterialPaintChannel channel,
                                                     const float2 &uv,
                                                     const int thread,
                                                     const float3 &t_decal,
                                                     const float3 &b_decal,
                                                     const float3 &n_m,
                                                     const float3 &t_m,
                                                     const float3 &b_m) const
{
  BLI_assert(channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  return remap_decal_normal_to_packed_tangent(
      this->color_at_uv(channel, uv, thread), t_decal, b_decal, n_m, t_m, b_m);
}

float3 ChannelSourceSampler::tangent_normal_packed_at_uv(const eMaterialPaintChannel channel,
                                                         const float2 &uv,
                                                         const int thread,
                                                         const float3 &t_decal,
                                                         const float3 &b_decal,
                                                         const float3 &n_m,
                                                         const float3 &t_m,
                                                         const float3 &b_m) const
{
  return uv_view(sources_, brush_paint_, settings_, this->paint(), brush_)
      .tangent_normal_packed_at_uv(channel, uv, thread, t_decal, b_decal, n_m, t_m, b_m);
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

  const float3 fallback = color_channel_fallback(channel, brush_paint_, this->paint(), brush_);
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
