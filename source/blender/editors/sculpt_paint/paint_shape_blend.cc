/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shared shape pixel blend core; see #paint_shape_render.hh.
 *
 * The per-pixel arithmetic the 2D compositor's `tile_composite_pbr` / `tile_composite` share.
 */

#include "paint_shape_render.hh"

#include <algorithm>

#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

#include "mesh/paint_material_blend.hh"

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

  if (ctx.channel < 0) {
    /* Plain Image canvas: the colors #shade_canvas yields for the ImBuf compositor. */
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
      float4 color = (part == ShapePart::Fill && ctx.fill_gradient != nullptr) ?
                         shade_fill_gradient(style,
                                             p_shape,
                                             ctx.fill_gradient->bbox_lo,
                                             ctx.fill_gradient->bbox_hi) :
                         shade_canvas(style, sample, part, p_shape);
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
        shade_channel(style, sample, ShapePart::Fill, PAINT_MATERIAL_CHANNEL_ALPHA).alpha,
        shade_channel(style, sample, ShapePart::Stroke, PAINT_MATERIAL_CHANNEL_ALPHA).alpha);
    if (alpha_cov <= 0.0f) {
      return;
    }
  }

  if (pbr_channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    float3 result = math::normalize(float3(dst.x, dst.y, dst.z) * 2.0f - float3(1.0f));
    for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
      const ChannelWrite write = shade_channel(style, sample, part, pbr_channel);
      const float alpha = write.alpha * shape_part_opacity(style, part) * factor * alpha_cov;
      if (alpha <= 0.0f) {
        continue;
      }
      const float3 detail = float3(write.value.x, write.value.y, write.value.z);
      result = material::blend_normal_rnm(result, detail, alpha);
    }
    const float3 encoded = result * 0.5f + float3(0.5f);
    dst = float4(encoded.x, encoded.y, encoded.z, dst.w);
    return;
  }

  if (pbr_channel == PAINT_MATERIAL_CHANNEL_HEIGHT) {
    for (const ShapePart part : {ShapePart::Fill, ShapePart::Stroke}) {
      const ChannelWrite write = shade_channel(style, sample, part, pbr_channel);
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
    const ChannelWrite write = shade_channel(style, sample, part, pbr_channel);
    const float alpha = write.alpha * shape_part_opacity(style, part) * factor * alpha_cov;
    if (alpha <= 0.0f) {
      continue;
    }
    const float4 mix(write.value.x * alpha, write.value.y * alpha, write.value.z * alpha, alpha);
    dst = material::composite_coverage(dst, mix, write.blend_mode);
  }
}

}  // namespace blender::ed::sculpt_paint::shape
