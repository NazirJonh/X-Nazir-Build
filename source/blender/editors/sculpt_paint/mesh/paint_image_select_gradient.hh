/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Masked gradient rasterization and session API for Image Paint.
 */

#pragma once

#include "BLI_math_vector_types.hh"

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

enum class ImagePaintGradientType {
  Linear = 0,
  Radial = 1,
  Conical = 2,
  Diamond = 3,
  Square = 4,
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
  float color_start[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float color_end[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float opacity = 1.0f;
  /** When non-null, overrides start/end color interpolation. */
  ColorBand *colorband = nullptr;
};

/** Evaluate gradient parameter t at pixel (tile-local coordinates). */
float image_paint_gradient_eval_t(const ImagePaintGradientParams &params,
                                  const float2 &start_px,
                                  const float2 &end_px,
                                  float midpoint,
                                  float px_x,
                                  float px_y);

/** Interpolate or colorband-evaluate at \a t into \a r_color (RGBA, straight alpha). */
void image_paint_gradient_eval_color(const ImagePaintGradientParams &params, float t, float r_color[4]);

/** Build params from persistent #ImagePaintSettings; the embedded gradient color ramp is used. */
ImagePaintGradientParams image_paint_gradient_params_from_imapaint(
    const ImagePaintSettings &imapaint);

/** Initialize the tool's embedded gradient color ramp with the default two stops when empty. */
void image_paint_gradient_ensure_colorband(ImagePaintSettings &imapaint);

/** Build params from Fill brush settings (color ramp, gradient mode, brush alpha). */
ImagePaintGradientParams image_paint_gradient_params_from_brush(const Paint *paint,
                                                                const Brush *brush);

/** Intersection of gradient vector bounds and optional selection mask. */
void image_paint_gradient_calc_work_region(const Scene *scene,
                                           const Image *image,
                                           int tile_number,
                                           int tile_w,
                                           int tile_h,
                                           const ImagePaintGradientParams &params,
                                           const float2 &start_px,
                                           const float2 &end_px,
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

/** Recompute the floating gradient preview from current tool settings, if active. */
void image_select_gradient_refresh_preview_from_settings(bContext *C);

/** Bump when #ImagePaintSettings gradient fields change (invalidates live preview). */
void image_paint_gradient_bump_settings_revision();

struct ImageSelectGradientState;

void image_select_gradient_state_free(ImageSelectGradientState *state);

bool image_select_gradient_is_floating(bContext *C);
bool image_select_gradient_is_floating_in_space(const SpaceImage *sima);

void PAINT_OT_image_select_gradient(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_apply(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_cancel(wmOperatorType *ot);

} /* namespace blender */
