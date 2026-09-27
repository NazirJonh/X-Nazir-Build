/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shape shader; see #paint_shape_shade.hh.
 */

#include "paint_shape_shade.hh"

#include <algorithm>

#include "BLI_assert.h"

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"

namespace blender::ed::sculpt_paint::shape {

float4 shade_source(const ShapeStyle &style,
                    const ShapeSample &sample,
                    const ShapePart part,
                    const float2 & /*p*/,
                    const float4 &solid,
                    const ShapeSourceConsumer consumer)
{
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
        /* TODO: sample a brush/asset texture or a Curve Patch pattern.
         * When D4 wires the UI to these sources, replace the assert with the real sampling --
         * otherwise a Debug build aborts as soon as the user picks that source. */
        BLI_assert_unreachable();
        return solid;
    }
    return solid;
  }
  switch (style.fill_source) {
    case ShapeFillSource::Solid:
    case ShapeFillSource::Gradient:
      /* The bbox-mapped gradient is applied by #shade_fill_gradient where the bbox is known. */
      return solid;
    case ShapeFillSource::Texture:
      /* TODO: sample a brush/asset texture over the shape.
       * When D4 maps PAINT_SHAPE_FILL_BRUSH_TEXTURE to ShapeFillSource::Texture, replace the
       * assert with the real sampling -- otherwise a Debug build aborts on pick in the UI. */
      BLI_assert_unreachable();
      return solid;
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
                    const float2 &p)
{
  const float4 solid = (part == ShapePart::Stroke) ? style.stroke_color : style.fill_color;
  /* Gradient fills composite through #shade_fill_gradient (the fill bounding box is only known
   * by the per-shape composite pass). */
  return shade_source(style, sample, part, p, solid, ShapeSourceConsumer::Canvas);
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
                           const eMaterialPaintChannel channel)
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

  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
    case PAINT_MATERIAL_CHANNEL_EMISSION: {
      const float4 solid(entry.color[0], entry.color[1], entry.color[2], 1.0f);
      const float4 src = shade_source(
          style, sample, part, float2(0.0f), solid, ShapeSourceConsumer::Channel);
      result.value = float4(src.x, src.y, src.z, 1.0f);
      result.alpha = coverage;
      result.blend_mode = IMB_BlendMode(entry.blend);
      break;
    }
    case PAINT_MATERIAL_CHANNEL_METALLIC:
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
    case PAINT_MATERIAL_CHANNEL_AO:
    case PAINT_MATERIAL_CHANNEL_CUSTOM: {
      const float4 src = shade_source(
          style, sample, part, float2(0.0f), float4(entry.value), ShapeSourceConsumer::Channel);
      result.value = float4(src.x, src.x, src.x, 1.0f);
      result.alpha = coverage;
      result.blend_mode = IMB_BlendMode(entry.blend);
      break;
    }
    case PAINT_MATERIAL_CHANNEL_ALPHA: {
      /* Alpha masks every other channel but never its own write. */
      result.value = float4(1.0f);
      result.alpha = coverage;
      result.blend_mode = IMB_BLEND_MIX;
      break;
    }
    case PAINT_MATERIAL_CHANNEL_HEIGHT: {
      /* Relative relief: the backend combines it with #ShapeStyle::height_blend. */
      const float delta = part_height(style, sample, part) * entry.strength * style.height_depth;
      result.value = float4(delta, delta, delta, 1.0f);
      result.alpha = coverage;
      result.blend_mode = IMB_BLEND_MIX;
      break;
    }
    case PAINT_MATERIAL_CHANNEL_NORMAL: {
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

float fill_gradient_param(const float2 &p, const float2 &bbox_lo, const float2 &bbox_hi)
{
  const float width = std::max(bbox_hi.x - bbox_lo.x, 1e-6f);
  return math::clamp((p.x - bbox_lo.x) / width, 0.0f, 1.0f);
}

float4 shade_fill_gradient(const ShapeStyle &style,
                           const float2 &p,
                           const float2 &bbox_lo,
                           const float2 &bbox_hi)
{
  /* TODO: radial gradients and brush-texture fills need their own mapping and sampling
   * context; the ramp lookup below already serves both once those land. */
  return style.fill_gradient_sample(fill_gradient_param(p, bbox_lo, bbox_hi));
}

}  // namespace blender::ed::sculpt_paint::shape
