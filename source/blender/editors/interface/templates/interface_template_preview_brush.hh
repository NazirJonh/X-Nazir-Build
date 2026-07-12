/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#pragma once

#include "BLI_vector.hh"

namespace blender {
struct bContext;
struct rcti;
struct Brush;
}  // namespace blender

namespace blender::ed::interface {

/* Per-stroke transform derived from brush settings. */
struct StrokeTransform {
  float scale_x;
  float scale_y;
  float rotation;
  float pattern_spacing;
  bool use_random;
  float random_angle;
};

/* A single brush stamp placed along the previewed stroke. */
struct StrokeStamp {
  float x;
  float y;
  float rotation;
};

/* Deterministic sinusoidal layout of brush stamps along the previewed stroke (fixed RNG seed so the
 * preview does not flicker between redraws). Shared by the immediate-mode fallback and the GPU
 * texture path. */
blender::Vector<StrokeStamp> compute_stroke_stamps(const Brush *brush,
                                                   const StrokeTransform &transform,
                                                   float center_x,
                                                   float center_y,
                                                   float preview_size,
                                                   float base_angle);

}  // namespace blender::ed::interface

/**
 * Draw the brush stroke preview into the given rectangle.
 *
 * The preview is purely a UI widget: it has no side effects on scene data and must not tag anything
 * for redraw. Updates happen through the normal region redraw triggered when the brush properties
 * shown next to the preview change.
 *
 * \param C: Blender context.
 * \param brush_data: Brush data pointer (#Brush). Stroke angle and spacing are read from it.
 * \param rect: Rectangle to draw in.
 */
void ED_brush_stroke_preview_draw(const blender::bContext *C, void *brush_data, blender::rcti *rect);
