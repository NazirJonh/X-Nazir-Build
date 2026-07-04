/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BKE_brush.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"

#include "DNA_brush_types.h"
#include "DNA_texture_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_math_vector.hh"
#include "BLI_time.h"

#include <cstdio>

#include "paint_material_source.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::material {

/* WORKAROUND: temporary printf profiling to find where per-channel source-texture sampling
 * spends time on strokes with many source images. Remove once the perf work is done. */
#define PBR_PAINT_SOURCE_PROFILE 0

ChannelSourceSampler::ChannelSourceSampler(const SculptSession &ss,
                                           const Brush &brush,
                                           const BrushMaterialPaint &brush_paint,
                                           const PaintModeSettings &settings)
    : ss_(ss), brush_(brush), settings_(settings), brush_paint_(brush_paint)
{
  bool any_source = false;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const bool channel_enabled = BKE_paint_material_channel_is_enabled(
        brush_paint, settings, info.channel);
    const bool alpha_masks = info.channel == PAINT_MATERIAL_CHANNEL_ALPHA &&
                             BKE_paint_material_channel_masks_stroke(brush_paint, settings);
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

#ifdef PBR_PAINT_SOURCE_PROFILE
  const double construct_start = BLI_time_now_seconds();
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
#ifdef PBR_PAINT_SOURCE_PROFILE
      const double acquire_start = BLI_time_now_seconds();
#endif
      ImBuf *ibuf = BKE_image_pool_acquire_ibuf(tex->ima, &iuser, pool_);
#ifdef PBR_PAINT_SOURCE_PROFILE
      /* A slow acquire here means this image was not yet in the pool (first probe for it this
       * stroke, so a real decode happened); a fast one means another channel already pulled the
       * same image in and the pool served it from cache. Compare the two to see whether the
       * per-channel Tex duplication (each channel owns its own Tex/MTex even when pointing at
       * the same Image) is causing redundant decodes across channels. */
      const double acquire_seconds = BLI_time_now_seconds() - acquire_start;
      printf("[pbr_paint] source probe: channel=%d image=%s acquire=%.3fms %s\n",
             i,
             tex->ima->id.name + 2,
             acquire_seconds * 1000.0,
             acquire_seconds > 0.001 ? "(likely decode)" : "(cache hit)");
      probed_image_num_++;
#endif
      source.usable = ibuf != nullptr;
      if (ibuf && ibuf->float_data() == nullptr) {
        source.do_linear_conversion = true;
        source.colorspace = ibuf->byte_buffer.colorspace;
      }
      BKE_image_pool_release_ibuf(tex->ima, ibuf, pool_);
    }
    else {
      source.usable = true;
    }
    active_ |= source.usable;
  }

#ifdef PBR_PAINT_SOURCE_PROFILE
  construct_seconds_ = BLI_time_now_seconds() - construct_start;
  printf("[pbr_paint] ChannelSourceSampler construct: %d image probe(s), %.3fms total\n",
         probed_image_num_,
         construct_seconds_ * 1000.0);
#endif
}

ChannelSourceSampler::~ChannelSourceSampler()
{
  if (pool_ != nullptr) {
    BKE_image_pool_free(pool_);
  }
}

bool ChannelSourceSampler::is_active() const
{
  return active_;
}

void ChannelSourceSampler::update_area_local_mats(const Object &ob)
{
  if (!active_) {
    return;
  }
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const ChannelSource &source = sources_[i];
    if (!source.usable || source.mtex->brush_map_mode != MTEX_MAP_MODE_AREA) {
      continue;
    }
    area_local_mats_[i] = calc_area_local_mat(ob, source.mtex->rot);
  }
}

bool ChannelSourceSampler::channel_source_failed(const eMaterialPaintChannel channel) const
{
  const ChannelSource &source = sources_[channel];
  return source.mtex != nullptr && !source.usable;
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

  const ChannelSource &source = sources_[channel];
  if (!source.usable) {
    return fallback;
  }

  float value;
  float4 rgba;
  sculpt_apply_texture(ss_,
                       brush_,
                       *source.mtex,
                       position,
                       thread,
                       &value,
                       rgba,
                       pool_,
                       area_local_mat_for(channel, source));
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

  const ChannelSource &source = sources_[channel];
  if (!source.usable) {
    return fallback;
  }

  float value;
  float4 rgba;
  sculpt_apply_texture(ss_,
                       brush_,
                       *source.mtex,
                       ctx,
                       thread,
                       &value,
                       rgba,
                       pool_,
                       area_local_mat_for(channel, source));
  return math::clamp(value, range.x, range.y);
}

bool ChannelSourceSampler::needs_linear_conversion(const eMaterialPaintChannel channel) const
{
  return sources_[channel].do_linear_conversion;
}

const ocio::ColorSpace *ChannelSourceSampler::colorspace(const eMaterialPaintChannel channel) const
{
  return sources_[channel].colorspace;
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
  const bool is_base_color = channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const bool is_emission = channel == PAINT_MATERIAL_CHANNEL_EMISSION;
  /* Base Color and Emission are the non-Normal channels that sample as a color; any other
   * channel reaching here would need its own fallback rather than silently reusing Base Color's.
   */
  BLI_assert(is_normal || is_base_color || is_emission);
  const float3 fallback = is_normal ? float3(brush_paint_.channels[channel].value) :
                          is_base_color ? BKE_paint_material_base_color_get(
                                              brush_paint_, *ss_.cache->paint, brush_, false) :
                                          float3(brush_paint_.channels[channel].value);

  const ChannelSource &source = sources_[channel];
  if (!source.usable) {
    return fallback;
  }

  float value;
  float4 rgba;
  sculpt_apply_texture(ss_,
                       brush_,
                       *source.mtex,
                       position,
                       thread,
                       &value,
                       rgba,
                       pool_,
                       area_local_mat_for(channel, source));
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
  const bool is_base_color = channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const bool is_emission = channel == PAINT_MATERIAL_CHANNEL_EMISSION;
  BLI_assert(is_normal || is_base_color || is_emission);
  const float3 fallback = is_normal ? float3(brush_paint_.channels[channel].value) :
                          is_base_color ? BKE_paint_material_base_color_get(
                                              brush_paint_, *ss_.cache->paint, brush_, false) :
                                          float3(brush_paint_.channels[channel].value);

  const ChannelSource &source = sources_[channel];
  if (!source.usable) {
    return fallback;
  }

  float value;
  float4 rgba;
  sculpt_apply_texture(ss_,
                       brush_,
                       *source.mtex,
                       ctx,
                       thread,
                       &value,
                       rgba,
                       pool_,
                       area_local_mat_for(channel, source));
  return finish_color_sample(is_normal,
                             fallback,
                             source.do_linear_conversion,
                             source.colorspace,
                             rgba,
                             decode_linear,
                             source.flip_green_channel);
}

}  // namespace blender::ed::sculpt_paint::material
