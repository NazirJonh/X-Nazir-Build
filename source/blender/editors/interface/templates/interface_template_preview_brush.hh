/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#pragma once

namespace blender {
struct bContext;
struct rcti;
}

/**
 * Draw the brush stroke preview into the given rectangle.
 *
 * The preview is purely a UI widget: it has no side effects on scene data and must not tag
 * anything for redraw. Updates happen through the normal region redraw triggered when the brush
 * properties shown next to the preview change.
 *
 * \param C: Blender context.
 * \param brush_data: Brush data pointer (#Brush).
 * \param angle: Brush stroke angle to visualize.
 * \param spacing: Brush stroke spacing (percentage) to visualize.
 * \param rect: Rectangle to draw in.
 */
void ED_brush_stroke_preview_draw(
    const blender::bContext *C, void *brush_data, float angle, float spacing, blender::rcti *rect);
