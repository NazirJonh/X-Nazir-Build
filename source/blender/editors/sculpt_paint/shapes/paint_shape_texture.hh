/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Texture mapping and sampling of the shape drawing tools.
 *
 * The mapping stage turns a #ShapeSample plus a shape's #ShapeTexFrame into texture UVs (Fit /
 * Mask / Along, with tiling, mirroring, flips and the extra angle), and the sampling stage reads
 * the resolved #ShapeTexture at those UVs. Pure math over pixel buffers: the resolver
 * (`style_textures_resolve`) builds the immutable #ShapeTexture handles and keeps every pinned
 * resource alive; nothing here allocates, locks or touches a mesh, so all of it is safe to call
 * from the compositing worker threads on the shared #ShapeStyle.
 */

#pragma once

#include <array>
#include <memory>

#include "BLI_math_vector_types.hh"

#include "DNA_scene_types.h"

#include "paint_shape.hh"
#include "paint_shape_render.hh"

struct ImagePool;

namespace blender::ed::sculpt_paint::shape {

/** One resolved texel source of a #ShapeTexture (one PBR channel, or the canvas color). */
struct ShapeTextureChannel {
  enum class Kind : int8_t {
    /** No source; the part's solid value applies. */
    None = 0,
    /** A single value (an unlinked material input, or a failed bake fallback). */
    Constant,
    /** A pixel buffer, row-major RGBA (float or byte). */
    Buffer,
  };

  Kind kind = Kind::None;
  /** Value of #Kind::Constant. */
  float4 constant = float4(0.0f);
  /** Pixel buffer of #Kind::Buffer (not owned; the texture's keeper pins it). */
  const float *float_px = nullptr;
  const uchar *byte_px = nullptr;
  /** Texel size of the buffer. */
  int2 size = int2(0);
  /** The buffer is already scene-linear (converted once at resolve when needed). Data (non-color)
   * buffers keep their raw values and are false by construction. */
  bool is_linear = true;
  /** The buffer is a normal map in the DirectX convention (its green channel flips on sample). */
  bool flip_green = false;
};

/**
 * Immutable texture of one shape part: the per-channel sources resolved once per session (brush
 * maps, a material's baked maps, a single image or the Curve Pattern tile). Built and owned by
 * the resolver through a `shared_ptr`, so copies of #ShapeStyle share it and the worker threads
 * only ever read it.
 */
class ShapeTexture {
 public:
  /** The canvas buffer is the Curve Pattern tile: its G channel carries the SDF relief the
   * Height/Normal channels stamp. */
  bool is_tile = false;
  /** Per-channel sources, indexed by #eMaterialPaintChannel. */
  std::array<ShapeTextureChannel, PAINT_MATERIAL_CHANNEL_NUM> channels;
  /** Canvas color source: the single image's RGBA or the pattern tile's coverage. */
  ShapeTextureChannel canvas;
  /** Native tile size in pixels, the reference the mapping's scale works against (the largest
   * buffer's size; (0, 0) when no buffer resolved). */
  int2 native_size = int2(0);
  /** Identity of the sampled content (buffer addresses, sizes, constants and a sparse pixel
   * sample), 0 when unknown. Two resolves of unchanged sources yield equal signatures, so the
   * live preview can tell "same texture" from "changed texture" without keeping handles alive. */
  uint64_t signature = 0;
  /** Keeps every pinned resource alive while this texture is referenced (the #ImagePool and its
   * acquired buffers, the material channel set, or an owned copy of the pixels). */
  std::shared_ptr<void> keeper;

  bool has_channel(const eMaterialPaintChannel channel) const
  {
    return channels[int(channel)].kind != ShapeTextureChannel::Kind::None;
  }
};

/** One shape's frame the mapping positions its texture against. Built per shape by the passes
 * that composite one shape at a time, or from the union bounds by the backends that rasterize
 * everything together. */
struct ShapeTexFrame {
  /** The shape's effective origin in shape-space pixels (#shape_effective_origin). */
  float2 center = float2(0.0f);
  /** Full size of the shape's local frame (bbox or parametric extents), pixels. */
  float2 size_px = float2(0.0f);
  /** The shape's own rotation, radians (0 for spline shapes, whose frame is the bbox). */
  float rotation = 0.0f;
  /** Total arc length of the stroke's polyline in pixels (the ALONG mapping's whole-repeat
   * denominator); 0 when unknown. */
  float stroke_len = 0.0f;
};

/**
 * Texture frame of \a shape: its effective origin and the same local frame #ShapeSample::
 * shape_uv is measured in (the rotated parametric extents of an analytic Rect/Ellipse, the
 * geometry bbox of any flattened outline). */
ShapeTexFrame shape_tex_frame_calc(const PaintShape &shape);

/** The union texture frame of \a shapes: the bounds union with the first shape's rotation
 * (the 3D backends' approximation; exact for a single shape). */
ShapeTexFrame shape_tex_frame_calc_union(Span<PaintShape> shapes);

/** Texture UV of one shaded point, with the mapped frame's angle for normal-map rotation. */
struct ShapeTexCoord {
  float2 uv = float2(0.0f);
  /** Angle of the texture's u axis in shape space, radians (normals rotate by it). */
  float frame_angle = 0.0f;
  /** Texture-space axis signs (see #shape_texture_axis_sign): the content is mirrored on an axis
   * whose sign is -1, so a tangent-space normal flips that component before it rotates. */
  float2 axis_flip = float2(1.0f);
  /** False when the point falls outside a non-tiling texture: the texture writes nothing there. */
  bool inside = false;
};

/** One sampled part texture: the color and scalar values plus the frame angle the normals
 * rotate by. `mapped` is false when the point falls outside a non-tiling mapping (the stamp
 * writes nothing there); `usable` is additionally false when no texture resolved or the channel
 * has no source (the shader falls back to the part's solid value). */
struct TextureSample {
  bool usable = false;
  bool mapped = true;
  float4 rgba = float4(0.0f);
  float scalar = 0.0f;
  /** The UV the sample was taken at (a tile's relief channel re-reads it). */
  float2 uv = float2(0.0f);
  float frame_angle = 0.0f;
  float2 axis_flip = float2(1.0f);
};

/** Mask mapping: `uv = R(-angle) * (p - anchor) / (native * scale) + offset`, tiled per axis.
 * The anchor is the shape's frame center (#PAINT_SHAPE_TEX_ANCHOR_SHAPE) or the space origin
 * (Canvas / View). */
ShapeTexCoord shape_texture_coord_mask(const ShapeTextureMapping &map,
                                       const float2 &p,
                                       const ShapeTexFrame &frame);

/** Fit mapping: the shape's local coordinates (#ShapeSample::shape_uv) mapped through the fit
 * mode, the repeats, the extra angle and the offset. */
ShapeTexCoord shape_texture_coord_fit(const ShapeTextureMapping &map,
                                      const ShapeSample &sample,
                                      const ShapeTexFrame &frame);

/** Along mapping (strokes only): the arc length over the tile length and the signed
 * across-stroke coordinate over the band. */
ShapeTexCoord shape_texture_coord_along(const ShapeTextureMapping &map,
                                        const ShapeSample &sample,
                                        const ShapeTexFrame &frame);

/** Dispatch on the mapping (the per-part entry point of the shader). */
ShapeTexCoord shape_texture_coord(const ShapeTextureMapping &map,
                                  const ShapeSample &sample,
                                  const float2 &p,
                                  const ShapeTexFrame &frame);

/** Sample the part's resolved texture (the style's #ShapeTexture handle and mapping) for
 * \a channel, or for the canvas color when \a channel is negative. See #TextureSample for the
 * not-usable cases. */
TextureSample shape_texture_sample_part(const ShapeStyle &style,
                                        ShapePart part,
                                        const ShapeSample &sample,
                                        const float2 &p,
                                        const ShapeTexFrame *frame,
                                        int channel,
                                        ShapeSourceConsumer consumer);

/** Wrap one axis of a raw coordinate: fract or mirror when tiled, clamp-to-edge with an
 * `inside` verdict otherwise; the flip mirrors the texture content within the tile. */
float shape_texture_wrap_axis(float u,
                              bool tile,
                              bool mirror,
                              bool flip,
                              bool &r_inside,
                              bool *r_reflected = nullptr);

/** The texture-space axis signs of a mapping: -1 on an axis whose content the mapping mirrors
 * (a flip, or a reflected tile of a mirrored repeat). Normal maps flip their X / Y with them. */
inline float shape_texture_axis_sign(const bool flip, const bool reflected)
{
  return (flip != reflected) ? -1.0f : 1.0f;
}

/** Bilinear RGBA sample of a resolved channel at \a uv in [0, 1] (tiling is the mapping stage's
 * job; the edges clamp). Float buffers return their values; byte buffers unfold to [0, 1]. */
float4 shape_texture_sample_rgba(const ShapeTextureChannel &channel, const float2 &uv);

/** Scalar value of a resolved channel: the luminance of the linear sample (the mask/factor
 * semantic of the brush channel sources). */
float shape_texture_sample_scalar(const ShapeTextureChannel &channel, const float2 &uv);

/** One component of a resolved channel, for the Curve Pattern tile's relief (G). */
float shape_texture_sample_component(const ShapeTextureChannel &channel,
                                     const float2 &uv,
                                     const int component);

/** Tangent-space normal of a normal-map channel: unpacked to [-1, 1], the green channel flipped
 * for DirectX sources, mirrored along \a axis_flip (the mapping's flips and reflected tiles) and
 * only then rotated into shape space by \a frame_angle. The XY of a tangent-space normal lives in
 * the UV frame, so it has to follow the content's rotation and mirroring exactly like the
 * selection transforms do (#image_select_normal_rotation); mirroring first keeps a reflection
 * from being turned the wrong way. */
float3 shape_texture_sample_normal(const ShapeTextureChannel &channel,
                                   const float2 &uv,
                                   const float frame_angle,
                                   const float2 &axis_flip = float2(1.0f));

/**
 * Rasterizer output mask \a base plus the texture opt-ins the style needs: #ShapeUV for a Fit
 * fill, #StrokeS | #StrokeV for an Along stroke. Every place that builds a #ShapeRasterOutputs
 * or #ShapeRasterizer for a user-visible style goes through this, so a mapping can never be
 * active while its coordinates were never requested.
 */
ShapeRasterOutputs shape_raster_outputs_for(const ShapeStyle &style,
                                            ShapeRasterOutputs base = SHAPE_RASTER_OUTPUTS_ALL);

/** The mapping's boolean flags packed into #ePaintShapeTexFlag bits (the operator-property
 * replay serializes them as one int). */
short shape_tex_mapping_flags(const ShapeTextureMapping &map);

/** Whether a texture mapping requires a per-shape frame (a Fit mapping always, a
 * Mask mapping anchored to the shape): those backends composite one shape at a time. False when
 * the combined pass can shade everything. */
bool style_textures_need_frame(const ShapeStyle &style);

}  // namespace blender::ed::sculpt_paint::shape
