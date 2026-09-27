/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The single per-pixel blend core shared by every shape write backend: the 2D Image compositor
 * (float and byte tiles) and the 3D Sculpt backends. It turns a #ShapeSample plus the resolved
 * #ShapeStyle into a write on a destination pixel.
 *
 * There is no ImBuf, mesh or undo here. The destination is in the caller's working space: scene
 * linear for float buffers and the 3D backends, the buffer's own colorspace for byte tiles whose
 * style the 2D compositor has already converted. Keeping the colorspace decision with the caller
 * is what lets the same arithmetic serve every target unchanged.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "paint_shape_shade.hh"

namespace blender::ed::sculpt_paint::shape {

/**
 * The decal-space -> tangent-space frame a Normal channel write is expressed in. The defaults
 * are the identity, which is what the 2D compositor uses: its detail normal already lives in the
 * map's tangent space, so no reorientation is applied.
 */
struct NormalWriteBasis {
  float3 t_screen = float3(1.0f, 0.0f, 0.0f);
  float3 b_screen = float3(0.0f, 1.0f, 0.0f);
  float3 n_m = float3(0.0f, 0.0f, 1.0f);
  float3 t_m = float3(1.0f, 0.0f, 0.0f);
  float3 b_m = float3(0.0f, 1.0f, 0.0f);
};

/**
 * Fill bounding box in shape space for the gradient fill path. The 2D compositor builds it per
 * shape; the 3D image backend uses the union bounds of the shapes it rasterizes.
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
  /** Normal write basis; null means the identity (the 2D compositor). */
  const NormalWriteBasis *normal_basis = nullptr;
  /** Fill gradient box; null keeps the solid / ramp #shade_canvas fill. */
  const ShapeFillGradient *fill_gradient = nullptr;
};

/**
 * Blend one #ShapeSample into \a dst. \a factor is the backend's own mask (selection, sculpt
 * mask, face sets) and scales every write's alpha.
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
