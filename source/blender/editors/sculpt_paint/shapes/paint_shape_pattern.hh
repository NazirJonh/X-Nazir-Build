/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Curve Pattern: the scene's 2D curves rasterized into a pattern tile the shape fills and
 * strokes sample like an image texture.
 *
 * The source curves (a single legacy Curve object or every 2D Curve of a collection) are read in
 * the crop frame's space (world XY by default), mapped into the tile's pixel square through
 * #PaintShapeCurvePattern::crop_min / crop_max, and rasterized with the shared shape rasterizer
 * into one float RGBA buffer: R carries the coverage, G the signed-distance relief for
 * Height/Normal stamping and A the coverage again. Strokes crossing the crop border wrap around
 * (a 3x3 neighborhood) so the tile repeats seamlessly.
 *
 * The resolved tile is immutable and cached: a rebuilt evaluated geometry or changed pattern
 * settings yield a new tile, everything else reuses the cached one.
 */

#pragma once

#include <memory>

#include "BLI_math_vector_types.hh"

#include "DNA_scene_types.h"

#include "paint_shape.hh"

namespace blender {

struct bContext;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

class ShapeTexture;

/**
 * Resolve \a settings' Curve Pattern into the immutable tile texture, or null when no usable 2D
 * curve source is set. Cached across calls (keyed on the source geometry and the pattern
 * settings); main thread only.
 */
std::shared_ptr<const ShapeTexture> shape_curve_pattern_texture_get(
    bContext *C, const PaintShapeSettings &settings);

/**
 * Rasterize \a shape (already in tile pixels) into \a r_buffer (RGBA float, \a resolution
 * squared): R = coverage, G = the SDF relief, B = 0, A = coverage. Exposed for the tests; the
 * resolver calls it with the curves converted to tile pixels.
 */
void shape_curve_pattern_tile_rasterize(const PaintShape &shape,
                                        const PaintShapeCurvePattern &pattern,
                                        float resolution,
                                        float *r_buffer);

/** Drop the cached tiles (the tests; the cache is content-keyed, so nothing else must call it). */
void shape_curve_pattern_cache_clear();

}  // namespace blender::ed::sculpt_paint::shape
