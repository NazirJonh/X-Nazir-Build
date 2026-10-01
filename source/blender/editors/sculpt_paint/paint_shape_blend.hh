/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The single per-pixel blend core of the shape write backend: the 2D Image compositor (float and
 * byte tiles). It turns a #ShapeSample plus the resolved #ShapeStyle into a write on a
 * destination pixel.
 *
 * There is no ImBuf or undo here. The destination is in the caller's working space: scene linear
 * for float buffers, the buffer's own colorspace for byte tiles whose style the 2D compositor has
 * already converted. Keeping the colorspace decision with the caller is what lets the same
 * arithmetic serve every target unchanged.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "paint_shape_shade.hh"

namespace blender::ed::sculpt_paint::shape {

/**
 * Fill bounding box in shape space for the gradient fill path. The 2D compositor builds it per
 * shape.
 */
struct ShapeFillGradient {
  float2 bbox_lo;
  float2 bbox_hi;
};

/** Everything #shape_blend_pixel needs beyond the sample itself. */
struct ShapeBlendContext {
  const ShapeStyle *style = nullptr;
  /** -1 = Canvas (the image's own pixel colors), otherwise an #eMaterialPaintChannel. */
  int channel = -1;
  /** Whether the Alpha channel masks the other channels this bake. Computed once per bake, never
   * per pixel. */
  bool alpha_active = false;
  /** Fill gradient box; null keeps the solid / ramp #shade_canvas fill. */
  const ShapeFillGradient *fill_gradient = nullptr;
};

/**
 * Blend one #ShapeSample into \a dst. \a factor is the backend's own mask (e.g. selection) and
 * scales every write's alpha.
 *
 * \param dst: destination pixel in the caller's working space (see the file comment).
 * \param p_shape: shape-space position of the pixel; used by the gradient fill.
 */
void shape_blend_pixel(float4 &dst,
                       const ShapeBlendContext &ctx,
                       const ShapeSample &sample,
                       float factor,
                       const float2 &p_shape);

}  // namespace blender::ed::sculpt_paint::shape
