/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The shape "shader": pure geometry-to-color math shared by the 2D compositor and the 3D
 * Sculpt PBR backends. No ImBuf, no mesh, no undo here; the callers own the targets.
 *
 * A #ShapeSample (from #ShapeEvaluator or the grid rasterizer) plus a #ShapePart selects the
 * value to write:
 * - #shade_canvas: Canvas color (solid fill, stroke color or ramp along the profile).
 * - #shade_channel: one PBR paint channel (scalar, color, Alpha, Height or Normal).
 *
 * Coverage and profile-as-coverage stay in the returned alpha: the backends multiply their own
 * masks (selection, sculpt mask, face sets) on top.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_scene_types.h"

#include "paint_shape.hh"
#include "paint_shape_raster.hh"

namespace blender::ed::sculpt_paint::shape {

/** Which part of the shape is shaded. */
enum class ShapePart : int8_t {
  Fill = 0,
  Stroke = 1,
};

/** Which consumer asks the shader for a source color. The stroke Ramp is a canvas-color concept;
 * the PBR channels keep their per-channel solid values until D4 wires texture sourcing. */
enum class ShapeSourceConsumer : int8_t {
  Canvas = 0,
  Channel = 1,
};

/**
 * Base color a part draws from its selected source.
 *
 * - Solid: returns \a solid (the canvas color or the channel entry).
 * - Stroke Ramp: the stroke ramp sampled at the across-stroke coordinate; canvas only, a channel
 *   keeps its solid value.
 * - Fill Gradient: returns \a solid here -- the bbox-mapped gradient is applied by
 *   #shade_fill_gradient where the bbox is known.
 * - Texture / CurvePattern: not implemented yet (TODO); aborts.
 *
 * \param p: shape-space position; unused until the texture sources land (D4).
 */
float4 shade_source(const ShapeStyle &style,
                    const ShapeSample &sample,
                    ShapePart part,
                    const float2 &p,
                    const float4 &solid,
                    ShapeSourceConsumer consumer);

/**
 * Coverage of \a part in \a sample with the profile-as-coverage factor applied. It is a shading
 * rule (the same one #shade_channel applies to the PBR channels internally), so it lives with the
 * shader; the pixel blend core calls it too.
 */
float shape_part_coverage(const ShapeStyle &style, const ShapeSample &sample, ShapePart part);

/**
 * Canvas color for \a sample (unpremultiplied, intrinsic alpha in `w`); the caller scales `w`
 * by the coverage and its own masks and blends premultiplied.
 *
 * \param p: shape-space position, used by the gradient/texture fills once they land.
 */
float4 shade_canvas(const ShapeStyle &style,
                    const ShapeSample &sample,
                    ShapePart part,
                    const float2 &p);

/** One PBR channel write: the value, its alpha, and how to blend it. */
struct ChannelWrite {
  /** Scalar channels: gray value in `xyz`. Colors: `rgb`. Normal: tangent-space normal xyz in
   * [-1, 1] (unencoded; the backends encode for image maps). Height: relative relief (the
   * backend adds it by #ShapeStyle::height_blend against the entry base value). */
  float4 value = float4(0.0f);
  /** Coverage of the part (profile-as-coverage included), before the backend's own masks. */
  float alpha = 0.0f;
  /** Blend mode for color/scalar channels; Height and Normal carry their own blend in the style
   * (#ShapeStyle::height_blend) and RNM, so this is #IMB_BLEND_MIX for them. */
  IMB_BlendMode blend_mode = IMB_BLEND_MIX;
};

/**
 * Value of PBR \a channel for \a sample. Returns zero alpha when the channel override is off
 * or the part disables the channel, so backends can skip the write.
 */
ChannelWrite shade_channel(const ShapeStyle &style,
                           const ShapeSample &sample,
                           ShapePart part,
                           eMaterialPaintChannel channel);

/** Gradient parameter of \a p over a fill bounding box (linear along X, 0 at \a bbox_lo). */
float fill_gradient_param(const float2 &p, const float2 &bbox_lo, const float2 &bbox_hi);

/**
 * Gradient fill color at \a p within the shape's fill bounding box (linear ColorBand ramp
 * along X). Backends that composite per shape (bbox known) use this; the union rasterizer
 * path keeps solid fills.
 */
float4 shade_fill_gradient(const ShapeStyle &style,
                           const float2 &p,
                           const float2 &bbox_lo,
                           const float2 &bbox_hi);

}  // namespace blender::ed::sculpt_paint::shape
