/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shape shader; see #paint_shape_render.hh.
 */

#include "paint_shape_render.hh"

#include <algorithm>

#include "BLI_assert.h"
#include "BLI_index_range.hh"

#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.hh"

#include "IMB_colormanagement.hh"

#include "paint_shape_texture.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Texture sourcing
 * \{ */

static const ShapeTexture *part_texture(const ShapeStyle &style, const ShapePart part)
{
  return (part == ShapePart::Stroke) ? style.stroke_texture.get() : style.fill_texture.get();
}

static const ShapeTextureMapping &part_texture_map(const ShapeStyle &style, const ShapePart part)
{
  return (part == ShapePart::Stroke) ? style.stroke_tex_map : style.fill_tex_map;
}

/** A sampled (scene-linear) texture color in the space the destination buffer stores colors in;
 * the part colors were converted once per tile, the texture's cannot be. */
static float3 texture_rgb_to_output(const ShapeStyle &style, const float3 &rgb)
{
  if (style.texture_colorspace == nullptr) {
    return rgb;
  }
  float3 out = rgb;
  if (style.texture_colorspace_is_srgb) {
    linearrgb_to_srgb_v3_v3(out, rgb);
  }
  else {
    IMB_colormanagement_scene_linear_to_colorspace_v3(out, style.texture_colorspace);
  }
  return out;
}

/** The part's texture color over \a solid (tint and opacity applied); \a solid itself when no
 * texture resolved or the point is not mapped, so the stamp paints its plain color there. */
static float4 shade_texture_color(const ShapeStyle &style,
                                  const ShapeSample &sample,
                                  const ShapePart part,
                                  const float2 &p,
                                  const float4 &solid,
                                  const ShapeSourceConsumer consumer,
                                  const ShapeTexFrame *frame)
{
  const TextureSample tex = shape_texture_sample_part(style, part, sample, p, frame, -1, consumer);
  if (!tex.usable) {
    return solid;
  }
  const ShapeTextureMapping &map = part_texture_map(style, part);
  float3 rgb = texture_rgb_to_output(style, float3(tex.rgba.x, tex.rgba.y, tex.rgba.z));
  if (map.use_tint) {
    rgb *= float3(solid.x, solid.y, solid.z);
  }
  const float opacity = math::clamp(map.opacity, 0.0f, 1.0f);
  const float3 mixed = math::interpolate(float3(solid.x, solid.y, solid.z), rgb, opacity);
  return float4(mixed.x, mixed.y, mixed.z, math::interpolate(1.0f, tex.rgba.w, opacity));
}

/** \} */

float4 shade_source(const ShapeStyle &style,
                    const ShapeSample &sample,
                    const ShapePart part,
                    const float2 &p,
                    const float4 &solid,
                    const ShapeSourceConsumer consumer,
                    const ShapeTexFrame *frame)
{
  /* A PBR channel samples its own texture source in #shade_channel; this fallback must not feed
   * it the canvas color (it would paint the Base Color texture into Roughness, Metallic, ...). */
  if (consumer == ShapeSourceConsumer::Channel &&
      (part == ShapePart::Stroke ?
           ELEM(style.stroke_source, ShapeStrokeSource::Texture, ShapeStrokeSource::CurvePattern) :
           style.fill_source == ShapeFillSource::Texture))
  {
    return solid;
  }
  if (part == ShapePart::Stroke) {
    switch (style.stroke_source) {
      case ShapeStrokeSource::Solid:
        return solid;
      case ShapeStrokeSource::Ramp:
        /* The ramp is a canvas color ramp; a PBR channel keeps its solid entry value. */
        if (consumer == ShapeSourceConsumer::Canvas) {
          return style.stroke_ramp_sample(sample.stroke_t);
        }
        return solid;
      case ShapeStrokeSource::Texture:
      case ShapeStrokeSource::CurvePattern:
        return shade_texture_color(style, sample, part, p, solid, consumer, frame);
    }
    return solid;
  }
  switch (style.fill_source) {
    case ShapeFillSource::Solid:
      return solid;
    case ShapeFillSource::Texture:
      return shade_texture_color(style, sample, part, p, solid, consumer, frame);
  }
  return solid;
}

float shape_part_coverage(const ShapeStyle &style, const ShapeSample &sample, const ShapePart part)
{
  if (part == ShapePart::Stroke) {
    float coverage = sample.stroke;
    if (style.use_stroke() && style.profile_affects_coverage()) {
      coverage *= math::clamp(style.stroke_profile_sample(sample.stroke_t), 0.0f, 1.0f);
    }
    return coverage;
  }
  float coverage = sample.fill;
  if (style.use_fill() && style.profile_affects_coverage()) {
    const float width = std::max(style.fill_profile_width, 1e-6f);
    const float t = math::clamp(sample.fill_d / width, 0.0f, 1.0f);
    coverage *= math::clamp(style.fill_profile_sample(t), 0.0f, 1.0f);
  }
  return coverage;
}

float4 shade_canvas(const ShapeStyle &style,
                    const ShapeSample &sample,
                    const ShapePart part,
                    const float2 &p,
                    const ShapeTexFrame *frame)
{
  const float4 solid = (part == ShapePart::Stroke) ? style.stroke_color : style.fill_color;
  return shade_source(style, sample, part, p, solid, ShapeSourceConsumer::Canvas, frame);
}

/** Height amplitude h(t) in [0, 1] of \a part at \a sample (profile or flat). */
static float part_height(const ShapeStyle &style, const ShapeSample &sample, const ShapePart part)
{
  if (!style.profile_affects_height()) {
    return 1.0f;
  }
  if (part == ShapePart::Stroke) {
    return math::clamp(style.stroke_profile_sample(sample.stroke_t), 0.0f, 1.0f);
  }
  const float width = std::max(style.fill_profile_width, 1e-6f);
  const float t = math::clamp(sample.fill_d / width, 0.0f, 1.0f);
  return math::clamp(style.fill_profile_sample(t), 0.0f, 1.0f);
}

ChannelWrite shade_channel(const ShapeStyle &style,
                           const ShapeSample &sample,
                           const ShapePart part,
                           const eMaterialPaintChannel channel,
                           const float2 &p,
                           const ShapeTexFrame *frame)
{
  ChannelWrite result;
  const PaintShapeChannelValue &entry = (part == ShapePart::Stroke) ?
                                            style.stroke_channels[channel] :
                                            style.fill_channels[channel];
  if (entry.use == 0) {
    return result;
  }
  const float coverage = shape_part_coverage(style, sample, part);
  if (coverage <= 0.0f) {
    return result;
  }

  /* The part's texture, sampled once for every branch that reads it. When the part sources a
   * texture this channel could read but the point is unmapped, the stamp writes nothing (zero
   * alpha); a texture without a source for the channel falls back to the solid entry value. */
  const ShapeTexture *texture = part_texture(style, part);
  const TextureSample tex = shape_texture_sample_part(
      style, part, sample, p, frame, int(channel), ShapeSourceConsumer::Channel);
  const ShapeTextureMapping &map = part_texture_map(style, part);
  const float tex_opacity = math::clamp(map.opacity, 0.0f, 1.0f);
  if (texture != nullptr && !tex.mapped) {
    return result;
  }

  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
    case PAINT_MATERIAL_CHANNEL_EMISSION: {
      const float4 solid(entry.color[0], entry.color[1], entry.color[2], 1.0f);
      float3 rgb;
      float alpha = 1.0f;
      if (tex.usable) {
        rgb = texture_rgb_to_output(style, float3(tex.rgba.x, tex.rgba.y, tex.rgba.z));
        if (map.use_tint) {
          rgb *= float3(solid.x, solid.y, solid.z);
        }
        rgb = math::interpolate(float3(solid.x, solid.y, solid.z), rgb, tex_opacity);
        alpha = math::interpolate(1.0f, tex.rgba.w, tex_opacity);
      }
      else {
        const float4 src = shade_source(
            style, sample, part, p, solid, ShapeSourceConsumer::Channel, frame);
        rgb = float3(src.x, src.y, src.z);
      }
      result.value = float4(rgb.x, rgb.y, rgb.z, 1.0f);
      result.alpha = coverage * alpha;
      result.blend_mode = IMB_BlendMode(entry.blend);
      break;
    }
    case PAINT_MATERIAL_CHANNEL_METALLIC:
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
    case PAINT_MATERIAL_CHANNEL_AO:
    case PAINT_MATERIAL_CHANNEL_CUSTOM: {
      float scalar = entry.value;
      if (tex.usable) {
        float tex_value = tex.scalar;
        if (map.invert) {
          tex_value = 1.0f - tex_value;
        }
        scalar = math::interpolate(entry.value, tex_value, tex_opacity);
      }
      else {
        const float4 src = shade_source(
            style, sample, part, p, float4(entry.value), ShapeSourceConsumer::Channel, frame);
        scalar = src.x;
      }
      result.value = float4(scalar, scalar, scalar, 1.0f);
      result.alpha = coverage;
      result.blend_mode = IMB_BlendMode(entry.blend);
      break;
    }
    case PAINT_MATERIAL_CHANNEL_ALPHA: {
      /* Alpha masks every other channel but never its own write; a texture's own alpha
       * modulates the coverage (a stamped cutout erases the other channels where it is
       * transparent). */
      result.value = float4(1.0f);
      result.alpha = coverage *
                     (tex.usable ? math::interpolate(1.0f, tex.rgba.w, tex_opacity) : 1.0f);
      result.blend_mode = IMB_BLEND_MIX;
      break;
    }
    case PAINT_MATERIAL_CHANNEL_HEIGHT: {
      /* Relative relief: the backend combines it with #ShapeStyle::height_blend. The texture
       * carries the relief (its value over #height_mid; the Curve Pattern tile's G channel is
       * the SDF relief), the profile scales its amplitude. */
      float relief = 1.0f;
      if (tex.usable) {
        const float tex_value = (texture->is_tile) ?
                                    shape_texture_sample_component(texture->channels[channel],
                                                                   tex.uv, 1) :
                                    tex.scalar;
        relief = math::interpolate(1.0f, tex_value - map.height_mid, tex_opacity);
      }
      const float delta = part_height(style, sample, part) * entry.strength * style.height_depth *
                          relief;
      result.value = float4(delta, delta, delta, 1.0f);
      result.alpha = coverage;
      result.blend_mode = IMB_BLEND_MIX;
      break;
    }
    case PAINT_MATERIAL_CHANNEL_NORMAL: {
      if (tex.usable) {
        /* The texture's tangent-space normal (rotated into the mapped frame) replaces the
         * analytic profile slope as the detail; the blend core RNM-merges it with the
         * destination. The mapping flips mirror their texture axes, the DirectX flag flips
         * green (the source-level convention rides in the channel's own flag). */
        float2 axis_flip = tex.axis_flip;
        if (map.normal_flip_y) {
          axis_flip.y = -axis_flip.y;
        }
        float3 normal = shape_texture_sample_normal(
            texture->channels[channel], tex.uv, tex.frame_angle, axis_flip);
        /* Opacity fades the texture's tilt back to the flat normal. */
        normal = math::normalize(
            float3(normal.x * tex_opacity, normal.y * tex_opacity, normal.z));
        result.value = float4(normal.x, normal.y, normal.z, entry.strength);
        result.alpha = coverage;
        result.blend_mode = IMB_BLEND_MIX;
        break;
      }
      /* Analytic normal from the profile slope along the distance gradient: the stroke slopes
       * over the across-stroke coordinate, the fill over the inward fill distance (its gradient
       * is #ShapeSample::fill_dir). With the height-to-normal link the relief follows the
       * height profile mode, so a profile that does not drive height leaves the normal flat. */
      float3 normal(0.0f, 0.0f, 1.0f);
      bool has_slope = false;
      float slope = 0.0f;
      float2 grad = float2(0.0f);
      if (part == ShapePart::Stroke && math::length_squared(sample.stroke_dir) > 1e-12f) {
        const float e = 0.01f;
        const float t0 = math::clamp(sample.stroke_t - e, 0.0f, 1.0f);
        const float t1 = math::clamp(sample.stroke_t + e, 0.0f, 1.0f);
        slope = (style.stroke_profile_sample(t1) - style.stroke_profile_sample(t0)) /
                std::max(t1 - t0, 1e-6f);
        grad = sample.stroke_dir / std::max(math::length(sample.stroke_dir), 1e-6f);
        has_slope = true;
      }
      else if (part == ShapePart::Fill && math::length_squared(sample.fill_dir) > 1e-12f) {
        const float width = std::max(style.fill_profile_width, 1e-6f);
        const float t = math::clamp(sample.fill_d / width, 0.0f, 1.0f);
        const float e = 0.01f;
        const float t0 = math::clamp(t - e, 0.0f, 1.0f);
        const float t1 = math::clamp(t + e, 0.0f, 1.0f);
        slope = (style.fill_profile_sample(t1) - style.fill_profile_sample(t0)) /
                std::max(t1 - t0, 1e-6f);
        grad = sample.fill_dir;
        has_slope = true;
      }
      if (has_slope && (!style.height_normal_link() || style.profile_affects_height())) {
        float2 xy = -slope * grad * entry.strength * style.normal_strength;
        if (style.normal_flip_y()) {
          xy.y = -xy.y;
        }
        normal = math::normalize(float3(xy.x, xy.y, 1.0f));
      }
      result.value = float4(normal.x, normal.y, normal.z, entry.strength);
      result.alpha = coverage;
      result.blend_mode = IMB_BLEND_MIX;
      break;
    }
    default:
      break;
  }
  return result;
}

}  // namespace blender::ed::sculpt_paint::shape
