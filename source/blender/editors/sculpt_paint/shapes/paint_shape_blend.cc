/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared shape pixel blend core; see #paint_shape_render.hh.
 *
 * The per-pixel arithmetic shared by the 2D compositor and the 3D Sculpt backends: it mirrors the
 * former `tile_composite_pbr` / `tile_composite` channel for channel.
 */

#include "paint_shape_render.hh"

#include <algorithm>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "../mesh/paint_material_blend.hh"

namespace blender::ed::sculpt_paint::shape {

/** The part's overall opacity, applied to the PBR channels as on the canvas. */
static float shape_part_opacity(const ShapeStyle &style, const ShapePart part)
{
  return part == ShapePart::Stroke ? style.stroke_opacity : style.fill_opacity;
}

/** Combine a Height write into \a r_dst by #ShapeStyle::height_blend. \a target is the channel's
 * absolute level (its base value plus the relief) that Max and Replace converge to. */
static void height_blend_apply(float3 &r_dst,
                               const float delta,
                               const float target,
                               const float alpha,
                               const ePaintShapeHeightBlend mode)
{
  switch (mode) {
    case PAINT_SHAPE_HEIGHT_ADD:
      r_dst.x += delta;
      r_dst.y += delta;
      r_dst.z += delta;
      break;
    case PAINT_SHAPE_HEIGHT_SUB:
      r_dst.x -= delta;
      r_dst.y -= delta;
      r_dst.z -= delta;
      break;
    case PAINT_SHAPE_HEIGHT_MAX:
      r_dst.x = std::max(r_dst.x, r_dst.x + (target - r_dst.x) * alpha);
      r_dst.y = std::max(r_dst.y, r_dst.y + (target - r_dst.y) * alpha);
      r_dst.z = std::max(r_dst.z, r_dst.z + (target - r_dst.z) * alpha);
      break;
    case PAINT_SHAPE_HEIGHT_REPLACE:
      r_dst.x += (target - r_dst.x) * alpha;
      r_dst.y += (target - r_dst.y) * alpha;
      r_dst.z += (target - r_dst.z) * alpha;
      break;
  }
}

void shape_blend_pixel(float4 &dst,
                       const ShapeBlendContext &ctx,
                       const ShapeSample &sample,
                       const float factor,
                       const float2 &p_shape)
{
  BLI_assert(ctx.style != nullptr);
  const ShapeStyle &style = *ctx.style;

  if (ctx.is_mask_target) {
    /* The mask value pair replaces the shape's colors entirely: stroke -> primary, fill ->
     * secondary, written as grayscale RGBA. The backend already folded the Mask Source texture
     * and its own selection / sculpt masks into \a factor. */
    const bool fill_pass = style.use_fill();
    const bool stroke_pass = style.use_stroke() && style.stroke_width > 0.0f;
    for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
      if (part == ShapePart::Fill && !fill_pass) {
        continue;
      }
      if (part == ShapePart::Stroke && !stroke_pass) {
        continue;
      }
      const float coverage = shape_part_coverage(style, sample, part);
      if (coverage <= 0.0f) {
        continue;
      }
      const float value = part == ShapePart::Stroke ? style.mask_primary : style.mask_secondary;
      const float alpha = shape_part_opacity(style, part) * coverage * factor;
      if (alpha <= 0.0f) {
        continue;
      }
      const short blend = part == ShapePart::Stroke ? style.stroke_blend : style.fill_blend;
      const float4 mix(value * alpha, value * alpha, value * alpha, alpha);
      dst = material::composite_coverage(dst, mix, IMB_BlendMode(blend));
    }
    return;
  }

  if (ctx.channel < 0) {
    /* Plain Image canvas: the same colors #shade_canvas yields for the color attribute / ImBuf
     * compositor. */
    const bool fill_pass = style.use_fill();
    const bool stroke_pass = style.use_stroke() && style.stroke_width > 0.0f;
    for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
      if (part == ShapePart::Fill && !fill_pass) {
        continue;
      }
      if (part == ShapePart::Stroke && !stroke_pass) {
        continue;
      }
      const float coverage = shape_part_coverage(style, sample, part);
      if (coverage <= 0.0f) {
        continue;
      }
      const float opacity = shape_part_opacity(style, part);
      float4 color = shade_canvas(style, sample, part, p_shape, ctx.tex_frame);
      color.w *= opacity * coverage * factor;
      if (color.w <= 0.0f) {
        continue;
      }
      const short blend = part == ShapePart::Stroke ? style.stroke_blend : style.fill_blend;
      const float4 mix(color.x * color.w, color.y * color.w, color.z * color.w, color.w);
      dst = material::composite_coverage(dst, mix, IMB_BlendMode(blend));
    }
    return;
  }

  const eMaterialPaintChannel pbr_channel = eMaterialPaintChannel(ctx.channel);
  float alpha_cov = 1.0f;
  if (material::channel_uses_alpha_mask(ctx.alpha_active, pbr_channel)) {
    alpha_cov = std::max(
        shade_channel(
            style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ALPHA, p_shape, ctx.tex_frame)
            .alpha,
        shade_channel(style,
                      sample,
                      ShapePart::Stroke,
                      PAINT_MATERIAL_CHANNEL_ALPHA,
                      p_shape,
                      ctx.tex_frame)
            .alpha);
    if (alpha_cov <= 0.0f) {
      return;
    }
  }

  if (pbr_channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* A layer's Normal map is empty black where nothing was painted, which decodes to the
     * tangent `(-1, -1, -1)`: the flat normal stands in by the map's coverage (its alpha), like the
     * layer composite and the brush's #mix_normal_over_scene do, and the write raises that
     * coverage with an "over" so a first stroke on a fresh layer becomes visible. */
    float map_alpha = math::clamp(dst.w, 0.0f, 1.0f);
    float3 stored(dst.x, dst.y, dst.z);
    if (ctx.dst_premultiplied && map_alpha > 0.0f) {
      stored /= map_alpha;
    }
    const float3 flat(0.0f, 0.0f, 1.0f);
    float3 result = math::interpolate(flat, stored * 2.0f - float3(1.0f), map_alpha);
    const float result_length = math::length(result);
    result = result_length > 1e-6f ? result / result_length : flat;
    for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
      const ChannelWrite write = shade_channel(style, sample, part, pbr_channel, p_shape, ctx.tex_frame);
      const float alpha = write.alpha * shape_part_opacity(style, part) * factor * alpha_cov;
      if (alpha <= 0.0f) {
        continue;
      }
      map_alpha += alpha * (1.0f - map_alpha);
      float3 detail = float3(write.value.x, write.value.y, write.value.z);
      if (ctx.normal_basis != nullptr) {
        const NormalWriteBasis &basis = *ctx.normal_basis;
        detail = material::remap_decal_normal_to_tangent(
            detail, basis.t_screen, basis.b_screen, basis.n_m, basis.t_m, basis.b_m);
      }
      result = material::blend_normal_rnm(result, detail, alpha);
    }
    const float3 encoded = (result * 0.5f + float3(0.5f)) *
                           (ctx.dst_premultiplied ? map_alpha : 1.0f);
    dst = float4(encoded.x, encoded.y, encoded.z, map_alpha);
    return;
  }

  if (pbr_channel == PAINT_MATERIAL_CHANNEL_HEIGHT) {
    for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
      const ChannelWrite write = shade_channel(style, sample, part, pbr_channel, p_shape, ctx.tex_frame);
      const float alpha = write.alpha * shape_part_opacity(style, part) * factor * alpha_cov;
      if (alpha <= 0.0f) {
        continue;
      }
      const PaintShapeChannelValue &entry = (part == ShapePart::Stroke) ?
                                                style.stroke_channels[pbr_channel] :
                                                style.fill_channels[pbr_channel];
      float3 rgb(dst.x, dst.y, dst.z);
      height_blend_apply(rgb, write.value.x * alpha, entry.value + write.value.x, alpha,
                         style.height_blend);
      dst.x = rgb.x;
      dst.y = rgb.y;
      dst.z = rgb.z;
    }
    return;
  }

  for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
    const ChannelWrite write = shade_channel(style, sample, part, pbr_channel, p_shape, ctx.tex_frame);
    const float alpha = write.alpha * shape_part_opacity(style, part) * factor * alpha_cov;
    if (alpha <= 0.0f) {
      continue;
    }
    const float4 mix(write.value.x * alpha, write.value.y * alpha, write.value.z * alpha, alpha);
    dst = material::composite_coverage(dst, mix, write.blend_mode);
  }
}

}  // namespace blender::ed::sculpt_paint::shape
