/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Screen-space float canvas for the Mask brush in SCREEN_SPACE_CANVAS projection mode.
 *
 * The canvas accumulates brush dabs and gesture shapes as a float [0..1] buffer in region
 * (pixel) pixel space. On Enter the operator `PAINT_OT_mask_canvas_apply` projects every
 * visible mesh vertex into the saved projection matrix, samples the canvas, and blends the
 * result into `.sculpt_mask`.
 *
 * The canvas is stored in `SculptSession::mask_canvas` so its lifetime matches the sculpt
 * session. It is NOT undo-tracked itself; undo is recorded once on Apply.
 *
 * The `MaskCanvas` struct itself is defined in `BKE_paint.hh` so that `SculptSession`
 * (a blenkernel type) can hold a `std::unique_ptr<MaskCanvas>` with a complete type.
 */

#pragma once

#include "BKE_paint.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include <optional>

struct ARegion;
struct bContext;
struct Object;

namespace blender::ed::sculpt_paint::mask {

/* -------------------------------------------------------------------- */
/** \name Session Management
 * \{ */

/**
 * Initialize or return the existing canvas for the active object.
 *
 * On first call (or after #canvas_end): saves the current region dimensions and
 * projection matrix from `rv3d`, zeroes the pixel buffer, sets `active = true`.
 *
 * On subsequent calls while `active == true`: returns the existing canvas unchanged,
 * so brush dabs accumulate across multiple strokes without resetting.
 *
 * \param C: Context used to obtain region, rv3d, and active object.
 * \param ob: Active sculpt object (needed for `ED_view3d_ob_project_mat_get`).
 * \return Reference to the canvas stored in `ob.runtime->sculpt_session->mask_canvas`.
 */
MaskCanvas &canvas_ensure(bContext &C, Object &ob);

/**
 * Zero-fill the pixel buffer without ending the session.
 * Useful for "Clear Canvas" without losing the saved projection.
 */
void canvas_clear(MaskCanvas &canvas);

/**
 * End the canvas session: zero the buffer and set `active = false`.
 * Called on Cancel (Esc) and after successful Apply if not keeping the canvas.
 */
void canvas_end(MaskCanvas &canvas);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas Drawing Primitives
 * \{ */

/**
 * Paint a circular brush dab into the canvas at screen-space position (px, py).
 *
 * Uses a smooth radial falloff: `factor = (1 - (dist/radius)^2)^max(hardness*2, 1)`.
 * Each pixel is updated as: `pixel = max(pixel, value * factor)` — additive-max blend
 * so repeated strokes don't erode the canvas.
 *
 * \param canvas: Target canvas (must be active).
 * \param px, py: Center in region pixel coordinates (float, can be sub-pixel).
 * \param radius_px: Brush radius in pixels (> 0).
 * \param value: Mask value to paint (0..1; negative inverts, subtracts from canvas).
 * \param hardness: Edge hardness in [0..1]; 0 = soft gaussian-like, 1 = hard circle.
 */
void canvas_draw_circle(MaskCanvas &canvas,
                        float px,
                        float py,
                        float radius_px,
                        float value,
                        float hardness);

/**
 * Fill a convex or concave polygon (screen-space integer coordinates) into the canvas.
 *
 * Uses scanline fill: iterates rows covered by the bounding box and tests each pixel
 * with a winding-number rule to handle non-convex shapes.
 *
 * \param canvas: Target canvas.
 * \param points: Polygon vertex list in region pixel coords (integer).
 * \param value: Mask value to fill (0..1).
 */
void canvas_fill_polygon(MaskCanvas &canvas,
                         blender::Span<int2> points,
                         float value);

/**
 * Fill an axis-aligned rectangle (inclusive pixel bounds) into the canvas.
 *
 * \param canvas: Target canvas.
 * \param xmin, ymin: Top-left pixel (clamped to canvas bounds).
 * \param xmax, ymax: Bottom-right pixel inclusive (clamped).
 * \param value: Mask value (0..1).
 */
void canvas_fill_rect(MaskCanvas &canvas, int xmin, int ymin, int xmax, int ymax, float value);

/**
 * Set mask value in an axis-aligned rectangle (gesture rasterization; absolute value per pixel).
 */
void canvas_set_rect(MaskCanvas &canvas, int xmin, int ymin, int xmax, int ymax, float value);

/**
 * Set mask value inside a polygon (gesture rasterization; absolute value per pixel).
 */
void canvas_set_polygon(MaskCanvas &canvas, blender::Span<int2> points, float value);

/**
 * Set mask value on one side of a screen-space line segment.
 *
 * \param flip: When true, fill the opposite side (matches line gesture `flip` property).
 * \param limit_to_segment: When true, only fill pixels whose projection falls on the segment.
 */
void canvas_set_line_halfplane(MaskCanvas &canvas,
                               const float2 &p0,
                               const float2 &p1,
                               bool flip,
                               bool limit_to_segment,
                               float value);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas Sampling
 * \{ */

/**
 * Bilinearly sample the canvas at floating-point pixel position (px, py).
 *
 * \return Interpolated value in [0..1], or 0.0f if (px, py) is outside the canvas.
 */
float canvas_sample(const MaskCanvas &canvas, float px, float py);

/**
 * Project object-space position to canvas pixel coordinates using the saved projection matrix.
 *
 * \return Pixel coordinates, or nullopt if the point is behind the camera.
 */
std::optional<float2> canvas_project_co(const MaskCanvas &canvas, const float3 &co);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Canvas Overlay
 * \{ */

/**
 * Draw the canvas as a full-screen overlay in the 3D viewport.
 */
void canvas_draw_overlay(MaskCanvas &canvas, struct ARegion *region);

/** \} */

}  // namespace blender::ed::sculpt_paint::mask
