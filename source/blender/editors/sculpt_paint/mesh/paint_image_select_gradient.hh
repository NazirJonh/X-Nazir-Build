/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Masked gradient rasterization and session API for Image Paint.
 *
 * The vector-based shapes (Linear..Square) evaluate `t` from a start / end pair; the Curve shape
 * evaluates it from a hand-drawn polyline (#blender::ed::sculpt_paint::gradient_curve::
 * GradientCurve2D) carried by #ImagePaintGradientParams, with its own Along / Across parameter
 * modes and an optional width limit (a NaN parameter leaves a pixel untouched).
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"

struct ColorBand;
struct Image;
struct ImagePaintSettings;
struct Paint;
struct Brush;
struct Scene;
struct SpaceImage;
struct bContext;
struct wmOperatorType;

namespace blender {

namespace ed::sculpt_paint::gradient_curve {
template<typename T> class GradientCurve;
using GradientCurve2D = GradientCurve<float2>;
}  // namespace ed::sculpt_paint::gradient_curve

enum class ImagePaintGradientType {
  Linear = 0,
  Radial = 1,
  Conical = 2,
  Diamond = 3,
  Square = 4,
  Curve = 5,
};

enum class ImagePaintGradientRepeat {
  None = 0,
  Repeat = 1,
  Reflect = 2,
};

struct ImagePaintGradientParams {
  ImagePaintGradientType type = ImagePaintGradientType::Linear;
  ImagePaintGradientRepeat repeat = ImagePaintGradientRepeat::None;
  IMB_BlendMode blend_mode = IMB_BLEND_MIX;
  float opacity = 1.0f;
  /** Color ramp used to evaluate the gradient; always set by the param builders. */
  ColorBand *colorband = nullptr;

  /* Curve type only (`ImagePaintGradientType::Curve`). */
  /**
   * The drawn polyline, expressed in the pixel space of the tile whose UV origin is
   * #curve_tile_origin_uv (an already isotropic space, so #axis_scale is ignored for this type).
   * Null when no curve has been drawn yet: pixels are left untouched.
   * Not owned; typically points into the floating gradient session's state.
   */
  const blender::ed::sculpt_paint::gradient_curve::GradientCurve2D *curve = nullptr;
  /** #eGradientCurveMode */
  eGradientCurveMode curve_mode = GRADIENT_CURVE_MODE_ALONG;
  /** Influence limit around the curve in canvas pixels (0: unlimited for Along). */
  float curve_width = 0.0f;
  /** Global UV origin of the tile the curve's pixel space refers to. */
  float2 curve_tile_origin_uv = float2(0.0f);
  /** Pixel dimensions of the tile the curve was drawn on. */
  int2 curve_tile_size = int2(0);
  /** Bounds of #curve in its own pixel space, for work-region culling. */
  float2 curve_bounds_min = float2(0.0f);
  float2 curve_bounds_max = float2(0.0f);
};

/** Evaluate gradient parameter t at pixel (tile-local coordinates). */
float image_paint_gradient_eval_t(const ImagePaintGradientParams &params,
                                  const float2 &start_px,
                                  const float2 &end_px,
                                  float midpoint,
                                  float px_x,
                                  float px_y);

/**
 * Bend a `[0, 1]` gradient parameter so that \a midpoint maps to 0.5 (smooth, C1 continuous);
 * a centered midpoint is a pass-through.
 */
float image_paint_gradient_remap_midpoint(float t, float midpoint);

/** Interpolate or colorband-evaluate at \a t into \a r_color (RGBA, straight alpha). */
void image_paint_gradient_eval_color(const ImagePaintGradientParams &params,
                                     float t,
                                     float r_color[4]);

/** Build params from persistent #ImagePaintSettings; the embedded gradient color ramp is used. */
ImagePaintGradientParams image_paint_gradient_params_from_imapaint(
    const ImagePaintSettings &imapaint);

/** Initialize the tool's embedded gradient color ramp with the default two stops when empty. */
void image_paint_gradient_ensure_colorband(ImagePaintSettings &imapaint);

/** Build params from Fill brush settings (color ramp, gradient mode, brush alpha). */
ImagePaintGradientParams image_paint_gradient_params_from_brush(const Paint *paint,
                                                                const Brush *brush);

/**
 * Region of \a tile_number the gradient has to paint: the selection bounds (expanded for
 * feathering) when a mask is active, the whole tile otherwise, intersected with
 * \a region_override when given.
 *
 * \note Takes no gradient geometry on purpose. The gradient vector positions the ramp but does
 * not bound the painted area -- the ramp parameter is clamped or wrapped outside the drag rather
 * than discarded -- so it cannot be used to shrink the region.
 */
void image_paint_gradient_calc_work_region(const Scene *scene,
                                           const Image *image,
                                           int tile_number,
                                           int tile_w,
                                           int tile_h,
                                           const rcti *region_override,
                                           rcti &r_region);

/**
 * Apply gradient to \a canvas_ibuf inside \a work_region without restoring backup first.
 * Used by Fill brush; selection masking uses blend weights when enabled.
 */
void image_paint_gradient_apply_region(const Scene *scene,
                                       Image *image,
                                       int tile_number,
                                       ImBuf *canvas_ibuf,
                                       const ImagePaintGradientParams &params,
                                       const float2 &start_px,
                                       const float2 &end_px,
                                       float midpoint,
                                       const rcti &work_region);

struct ImageSelectGradientState;

void image_select_gradient_state_free(ImageSelectGradientState *state);

bool image_select_gradient_is_floating(bContext *C);
bool image_select_gradient_is_floating_in_space(const SpaceImage *sima);

void PAINT_OT_image_select_gradient(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_apply(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_cancel(wmOperatorType *ot);

} /* namespace blender */
