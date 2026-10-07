/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shape texture mapping and sampling; see #paint_shape_texture.hh.
 */

#include "paint_shape_texture.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_utildefines.h"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Coordinates
 * \{ */

ShapeTexFrame shape_tex_frame_calc(const PaintShape &shape)
{
  ShapeTexFrame frame;
  frame.center = shape_effective_origin(shape);
  if (shape.has_analytic_sdf()) {
    /* The rotated parametric frame: the same frame #ShapeSample::shape_uv is normalized by. */
    frame.size_px = math::max(shape.half_size * 2.0f, float2(1e-4f));
    frame.rotation = shape.rotation;
    return frame;
  }
  /* The geometry bbox, the frame the flattened outlines' shape_uv spans. */
  float2 lo(FLT_MAX, FLT_MAX);
  float2 hi(-FLT_MAX, -FLT_MAX);
  for (const ShapePolyline &poly : shape_flatten(shape, 1.0f)) {
    for (const float2 &p : poly.points) {
      lo = math::min(lo, p);
      hi = math::max(hi, p);
    }
  }
  frame.size_px = math::max(hi - lo, float2(1e-4f));
  frame.rotation = 0.0f;
  return frame;
}

ShapeTexFrame shape_tex_frame_calc_union(const Span<PaintShape> shapes)
{
  if (shapes.is_empty()) {
    return {};
  }
  if (shapes.size() == 1) {
    return shape_tex_frame_calc(shapes[0]);
  }
  /* Union bounds with the first shape's rotation: an approximation that is exact for the common
   * single-shape bakes (the 3D texel backend evaluates everything at once and has no per-shape
   * identity to read a frame from). */
  ShapeTexFrame frame = shape_tex_frame_calc(shapes[0]);
  float2 lo = frame.center - frame.size_px * 0.5f;
  float2 hi = frame.center + frame.size_px * 0.5f;
  for (const int i : shapes.index_range().drop_front(1)) {
    const ShapeTexFrame part = shape_tex_frame_calc(shapes[i]);
    lo = math::min(lo, part.center - part.size_px * 0.5f);
    hi = math::max(hi, part.center + part.size_px * 0.5f);
  }
  frame.center = (lo + hi) * 0.5f;
  frame.size_px = math::max(hi - lo, float2(1e-4f));
  return frame;
}

float shape_texture_wrap_axis(const float u,
                              const bool tile,
                              const bool mirror,
                              const bool flip,
                              bool &r_inside,
                              bool *r_reflected)
{
  float wrapped;
  if (r_reflected != nullptr) {
    *r_reflected = false;
  }
  if (tile) {
    if (mirror) {
      /* Fold the axis into [0, 1] with a reflection at every integer: the classic mirrored
       * repeat. */
      const float f = u - 2.0f * math::floor(u * 0.5f);
      wrapped = (f <= 1.0f) ? f : 2.0f - f;
      if (r_reflected != nullptr) {
        *r_reflected = f > 1.0f;
      }
    }
    else {
      wrapped = u - math::floor(u);
    }
    r_inside = true;
  }
  else {
    /* One stamp: outside [0, 1] the texture writes nothing, inside it clamps at the edges. */
    r_inside = (u >= 0.0f) && (u <= 1.0f);
    wrapped = math::clamp(u, 0.0f, 1.0f);
  }
  return flip ? (1.0f - wrapped) : wrapped;
}

static float2 texture_frame_size_px(const ShapeTextureMapping &map)
{
  /* The native tile size scaled by the mapping; guarded against the zero scales of a texture
   * part whose settings never got their defaults (old files are healed at load, but a division
   * by zero must never survive to the sampler anyway). */
  const float2 size = map.ref_tex_size_px * math::max(map.scale, float2(1e-4f));
  return math::max(size, float2(1e-4f));
}

ShapeTexCoord shape_texture_coord_mask(const ShapeTextureMapping &map,
                                       const float2 &p,
                                       const ShapeTexFrame &frame)
{
  ShapeTexCoord coord;
  const float2 anchor = (map.anchor == PAINT_SHAPE_TEX_ANCHOR_SHAPE) ? frame.center : float2(0.0f);
  const float rotation = map.angle + ((map.follow_rotation) ? frame.rotation : 0.0f);
  float2 q = p - anchor;
  if (rotation != 0.0f) {
    const float c = math::cos(-rotation);
    const float s = math::sin(-rotation);
    q = float2(q.x * c - q.y * s, q.x * s + q.y * c);
  }
  float2 uv = q / texture_frame_size_px(map) + map.offset;
  bool inside_x;
  bool inside_y;
  bool reflected_x;
  bool reflected_y;
  uv.x = shape_texture_wrap_axis(uv.x, map.tile_x, map.mirror, map.flip_x, inside_x, &reflected_x);
  uv.y = shape_texture_wrap_axis(uv.y, map.tile_y, map.mirror, map.flip_y, inside_y, &reflected_y);
  coord.uv = uv;
  coord.inside = inside_x && inside_y;
  coord.frame_angle = rotation;
  coord.axis_flip = float2(shape_texture_axis_sign(map.flip_x, reflected_x),
                           shape_texture_axis_sign(map.flip_y, reflected_y));
  return coord;
}

ShapeTexCoord shape_texture_coord_fit(const ShapeTextureMapping &map,
                                      const ShapeSample &sample,
                                      const ShapeTexFrame &frame)
{
  ShapeTexCoord coord;
  float2 uv0 = math::clamp(sample.shape_uv, float2(0.0f), float2(1.0f));

  /* The fit modes adjust the texture's aspect against the frame's: s_x / s_y are the frame's
   * extents in texture tiles. */
  const float2 tex_size = math::max(float2(map.ref_tex_size_px), float2(1.0f));
  const float2 frame_size = math::max(frame.size_px, float2(1.0f));
  const float sx = frame_size.x / tex_size.x;
  const float sy = frame_size.y / tex_size.y;
  if (map.fit == PAINT_SHAPE_TEX_FIT_COVER || map.fit == PAINT_SHAPE_TEX_FIT_CONTAIN) {
    const float s = (map.fit == PAINT_SHAPE_TEX_FIT_COVER) ? math::max(sx, sy) :
                                                             math::min(sx, sy);
    /* Cover keeps a sub-rect of the texture (both factors <= 1); contain shows the whole
     * texture with the overflow beyond [0, 1] reported as outside by the wrap. */
    uv0 = float2(0.5f + (uv0.x - 0.5f) * (sx / s), 0.5f + (uv0.y - 0.5f) * (sy / s));
  }

  /* Extra angle around the frame center, then the repeats. */
  float2 c = uv0 - float2(0.5f);
  if (map.angle != 0.0f) {
    const float cs = math::cos(map.angle);
    const float sn = math::sin(map.angle);
    c = float2(c.x * cs - c.y * sn, c.x * sn + c.y * cs);
  }
  c *= math::max(map.repeat, float2(1e-4f));
  float2 uv = c + float2(0.5f) + map.offset;

  bool inside_x;
  bool inside_y;
  bool reflected_x;
  bool reflected_y;
  uv.x = shape_texture_wrap_axis(uv.x, map.tile_x, map.mirror, map.flip_x, inside_x, &reflected_x);
  uv.y = shape_texture_wrap_axis(uv.y, map.tile_y, map.mirror, map.flip_y, inside_y, &reflected_y);
  coord.uv = uv;
  coord.inside = inside_x && inside_y;
  /* The lookup turns the frame coordinates by +angle to reach the texture, so the content sits
   * in the frame turned by -angle, on top of the shape's own rotation. */
  coord.frame_angle = frame.rotation - map.angle;
  coord.axis_flip = float2(shape_texture_axis_sign(map.flip_x, reflected_x),
                           shape_texture_axis_sign(map.flip_y, reflected_y));
  return coord;
}

ShapeTexCoord shape_texture_coord_along(const ShapeTextureMapping &map,
                                        const ShapeSample &sample,
                                        const ShapeTexFrame & /*frame*/)
{
  ShapeTexCoord coord;

  /* One repeat's length: the explicit tile length, else the texture's native width scaled. */
  float tile_len = map.tile_length;
  if (tile_len <= 0.0f) {
    tile_len = math::max(float(map.ref_tex_size_px.x), 1.0f) * map.scale.x;
  }
  tile_len = std::max(tile_len, 1e-4f);

  if (map.whole_repeats && sample.stroke_len > 0.0f) {
    /* Closed outlines round the repeat count so the seam of the last tile meets the first. The
     * polyline length rides in the per-pixel sample, so this works in the combined pass. */
    const int repeats = std::max(1, int(math::round(sample.stroke_len / tile_len)));
    tile_len = sample.stroke_len / float(repeats);
  }

  float u = sample.stroke_s / tile_len;
  float v = 0.5f + 0.5f * math::clamp(sample.stroke_v, -1.0f, 1.0f);
  if (map.rotate_90) {
    std::swap(u, v);
  }
  if (map.flip_y) {
    v = 1.0f - v;
  }

  bool inside_x;
  bool inside_y;
  bool reflected_x;
  /* The along axis always tiles (a texture that stops mid-stroke has no meaning); the across
   * axis covers the stroke band exactly, so both wrap plainly. The flip_x mirror mirrors the
   * content along the stroke. */
  u = shape_texture_wrap_axis(u, true, map.mirror, map.flip_x, inside_x, &reflected_x);
  v = shape_texture_wrap_axis(v, true, false, false, inside_y);

  coord.uv = float2(u, v);
  coord.inside = inside_x && inside_y;

  /* Where the texture's axes point in shape space: the across axis follows the gradient of the
   * signed across-stroke coordinate (#ShapeSample::stroke_dir points away from the centerline, so
   * it flips with the side), the along axis is the travel tangent, a quarter turn from it. A
   * rotated (swapped) texture exchanges the two. */
  float2 across = sample.stroke_dir * (sample.stroke_v < 0.0f ? -1.0f : 1.0f);
  if (math::length_squared(across) < 1e-12f) {
    coord.axis_flip = float2(shape_texture_axis_sign(map.flip_x, reflected_x),
                             shape_texture_axis_sign(map.flip_y, false));
    return coord;
  }
  across = math::normalize(across);
  const float2 along = float2(across.y, -across.x);
  const float2 axis_x = map.rotate_90 ? across : along;
  const float2 axis_y = map.rotate_90 ? along : across;
  coord.frame_angle = math::atan2(axis_x.y, axis_x.x);
  /* The texture's y axis is the quarter turn of its x axis, or its mirror image. */
  const float handedness = (-axis_x.y * axis_y.x + axis_x.x * axis_y.y) < 0.0f ? -1.0f : 1.0f;
  coord.axis_flip = float2(shape_texture_axis_sign(map.flip_x, reflected_x),
                           handedness * shape_texture_axis_sign(map.flip_y, false));
  return coord;
}

ShapeTexCoord shape_texture_coord(const ShapeTextureMapping &map,
                                  const ShapeSample &sample,
                                  const float2 &p,
                                  const ShapeTexFrame &frame)
{
  switch (map.mapping) {
    case PAINT_SHAPE_TEX_MAP_FIT:
      return shape_texture_coord_fit(map, sample, frame);
    case PAINT_SHAPE_TEX_MAP_ALONG:
      return shape_texture_coord_along(map, sample, frame);
    case PAINT_SHAPE_TEX_MAP_MASK:
    default:
      return shape_texture_coord_mask(map, p, frame);
  }
}

bool style_textures_need_frame(const ShapeStyle &style)
{
  if (style.fill_uses_shape_uv()) {
    /* The Fit mapping needs the shape's frame aspect (cover / contain) and rotation. */
    return true;
  }
  const bool fill_mask_on_shape = style.use_fill() &&
                                  style.fill_source == ShapeFillSource::Texture &&
                                  style.fill_tex_map.mapping == PAINT_SHAPE_TEX_MAP_MASK &&
                                  style.fill_tex_map.anchor == PAINT_SHAPE_TEX_ANCHOR_SHAPE;
  const bool stroke_mask_on_shape =
      style.use_stroke() &&
      ELEM(style.stroke_source, ShapeStrokeSource::Texture, ShapeStrokeSource::CurvePattern) &&
      style.stroke_tex_map.mapping == PAINT_SHAPE_TEX_MAP_MASK &&
      style.stroke_tex_map.anchor == PAINT_SHAPE_TEX_ANCHOR_SHAPE;
  return fill_mask_on_shape || stroke_mask_on_shape;
}

/** Whether the part's texture may feed \a channel: the canvas consumer reads the texture's
 * canvas color (the single image or the pattern tile, falling back to the Base Color source),
 * a PBR channel reads its own source. */
static bool texture_has_channel(const ShapeTexture &texture,
                                const int channel,
                                const ShapeSourceConsumer consumer)
{
  if (consumer == ShapeSourceConsumer::Canvas) {
    return texture.canvas.kind != ShapeTextureChannel::Kind::None ||
           texture.has_channel(eMaterialPaintChannel(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  }
  /* A negative channel is the "no specific channel" request: it reads the Base Color source,
   * like the sampling below. Never index the per-channel array with it. */
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return texture.has_channel(eMaterialPaintChannel(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  }
  return texture.has_channel(eMaterialPaintChannel(channel));
}

TextureSample shape_texture_sample_part(const ShapeStyle &style,
                                        const ShapePart part,
                                        const ShapeSample &sample,
                                        const float2 &p,
                                        const ShapeTexFrame *frame,
                                        const int channel,
                                        const ShapeSourceConsumer consumer)
{
  TextureSample out;
  const ShapeTexture *texture = (part == ShapePart::Stroke) ? style.stroke_texture.get() :
                                                              style.fill_texture.get();
  if (texture == nullptr || !texture_has_channel(*texture, channel, consumer)) {
    /* No texture or no source at all: the shader falls back to the solid values. */
    return out;
  }
  /* The combined passes pass no frame; their mappings anchor to the space origin and the
   * frame-needing ones run per shape (see #style_textures_need_frame). */
  static const ShapeTexFrame empty_frame;
  const ShapeTexFrame &tex_frame = frame ? *frame : empty_frame;
  const ShapeTextureMapping &map = (part == ShapePart::Stroke) ? style.stroke_tex_map :
                                                                 style.fill_tex_map;
  const ShapeTexCoord coord = shape_texture_coord(map, sample, p, tex_frame);
  out.frame_angle = coord.frame_angle;
  out.axis_flip = coord.axis_flip;
  out.uv = coord.uv;
  out.mapped = coord.inside;
  if (!coord.inside) {
    return out;
  }
  const ShapeTextureChannel &source =
      (consumer == ShapeSourceConsumer::Canvas &&
       texture->canvas.kind != ShapeTextureChannel::Kind::None) ?
          texture->canvas :
          texture->channels[channel < 0 ? PAINT_MATERIAL_CHANNEL_BASE_COLOR : channel];
  if (source.kind == ShapeTextureChannel::Kind::None) {
    return out;
  }
  out.rgba = shape_texture_sample_rgba(source, coord.uv);
  out.scalar = shape_texture_sample_scalar(source, coord.uv);
  out.usable = true;
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling
 * \{ */

static float4 channel_pixel(const ShapeTextureChannel &channel, const int x, const int y)
{
  if (channel.float_px != nullptr) {
    const int64_t i = (int64_t(y) * channel.size.x + x) * 4;
    return float4(channel.float_px[i], channel.float_px[i + 1], channel.float_px[i + 2],
                  channel.float_px[i + 3]);
  }
  const int64_t i = (int64_t(y) * channel.size.x + x) * 4;
  return float4(channel.byte_px[i] / 255.0f, channel.byte_px[i + 1] / 255.0f,
                channel.byte_px[i + 2] / 255.0f, channel.byte_px[i + 3] / 255.0f);
}

float4 shape_texture_sample_rgba(const ShapeTextureChannel &channel, const float2 &uv)
{
  if (channel.kind == ShapeTextureChannel::Kind::None) {
    return float4(0.0f);
  }
  if (channel.kind == ShapeTextureChannel::Kind::Constant) {
    return channel.constant;
  }
  const int w = channel.size.x;
  const int h = channel.size.y;
  if (w <= 0 || h <= 0) {
    return float4(0.0f);
  }
  /* Texel centers: u in [0, 1] spans [0.5, w - 0.5] in texel coordinates. */
  const float fx = math::clamp(uv.x, 0.0f, 1.0f) * float(w) - 0.5f;
  const float fy = math::clamp(uv.y, 0.0f, 1.0f) * float(h) - 0.5f;
  const int x0 = int(math::floor(fx));
  const int y0 = int(math::floor(fy));
  const float tx = fx - float(x0);
  const float ty = fy - float(y0);
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const int xc0 = std::clamp(x0, 0, w - 1);
  const int yc0 = std::clamp(y0, 0, h - 1);

  const float4 c00 = channel_pixel(channel, xc0, yc0);
  const float4 c10 = channel_pixel(channel, x1, yc0);
  const float4 c01 = channel_pixel(channel, xc0, y1);
  const float4 c11 = channel_pixel(channel, x1, y1);
  const float4 a = c00 * (1.0f - tx) + c10 * tx;
  const float4 b = c01 * (1.0f - tx) + c11 * tx;
  return a * (1.0f - ty) + b * ty;
}

float shape_texture_sample_scalar(const ShapeTextureChannel &channel, const float2 &uv)
{
  const float4 rgba = shape_texture_sample_rgba(channel, uv);
  /* The mask/factor semantic of the brush channel sources: the linear luminance. */
  return math::dot(float3(rgba.x, rgba.y, rgba.z), float3(0.2126f, 0.7152f, 0.0722f));
}

float shape_texture_sample_component(const ShapeTextureChannel &channel,
                                     const float2 &uv,
                                     const int component)
{
  return shape_texture_sample_rgba(channel, uv)[component];
}

float3 shape_texture_sample_normal(const ShapeTextureChannel &channel,
                                   const float2 &uv,
                                   const float frame_angle,
                                   const float2 &axis_flip)
{
  const float4 rgba = shape_texture_sample_rgba(channel, uv);
  float3 normal = float3(rgba.x, rgba.y, rgba.z) * 2.0f - float3(1.0f);
  if (channel.flip_green) {
    normal.y = -normal.y;
  }
  normal.x *= axis_flip.x;
  normal.y *= axis_flip.y;
  if (frame_angle != 0.0f) {
    const float c = math::cos(frame_angle);
    const float s = math::sin(frame_angle);
    normal = float3(normal.x * c - normal.y * s, normal.x * s + normal.y * c, normal.z);
  }
  return math::normalize(normal);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rasterizer outputs
 * \{ */

ShapeRasterOutputs shape_raster_outputs_for(const ShapeStyle &style, const ShapeRasterOutputs base)
{
  ShapeRasterOutputs outputs = base;
  if (style.fill_uses_shape_uv()) {
    outputs |= ShapeRasterOutputs::ShapeUV;
  }
  if (style.stroke_uses_along_coords()) {
    outputs |= ShapeRasterOutputs::StrokeS | ShapeRasterOutputs::StrokeV;
  }
  return outputs;
}

short shape_tex_mapping_flags(const ShapeTextureMapping &map)
{
  short flag = 0;
  flag |= map.tile_x ? PAINT_SHAPE_TEX_TILE_X : 0;
  flag |= map.tile_y ? PAINT_SHAPE_TEX_TILE_Y : 0;
  flag |= map.mirror ? PAINT_SHAPE_TEX_MIRROR : 0;
  flag |= map.flip_x ? PAINT_SHAPE_TEX_FLIP_X : 0;
  flag |= map.flip_y ? PAINT_SHAPE_TEX_FLIP_Y : 0;
  flag |= map.follow_rotation ? PAINT_SHAPE_TEX_FOLLOW_SHAPE_ROTATION : 0;
  flag |= map.whole_repeats ? PAINT_SHAPE_TEX_WHOLE_REPEATS : 0;
  flag |= map.rotate_90 ? PAINT_SHAPE_TEX_ROTATE_90 : 0;
  flag |= map.invert ? PAINT_SHAPE_TEX_INVERT : 0;
  flag |= map.normal_flip_y ? PAINT_SHAPE_TEX_NORMAL_FLIP_Y : 0;
  flag |= map.use_tint ? PAINT_SHAPE_TEX_USE_TINT : 0;
  /* The uniform-scale flag is folded into the scale at resolve, so it never replays. */
  return flag;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
