/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp source cursors for the 3D Viewport and the Image Editor.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

/* NOTE: all Blender DNA/data types live in namespace blender. */
namespace blender {
struct ARegion;
struct ViewContext;
}  // namespace blender

namespace blender::ed::sculpt_paint::clone {

struct CloneSourcePoint;

/**
 * Bind the dashed-line program the Clone Stamp cursors are drawn with, viewport size and dash
 * pattern set. The caller unbinds it and restores whatever it had bound before.
 *
 * A dash separates the two circles this tool shows at once -- where the brush writes and where it
 * reads -- from the solid outline every other brush draws, without a second color or a shadow
 * ring to read.
 */
void clone_dashed_program_bind();

/**
 * One dashed outline of \a radius in the current matrix's XY plane, round or square.
 * Requires #clone_dashed_program_bind, and \a pos to be a 3-component position attribute.
 */
void clone_dashed_outline_draw(uint pos, float radius, bool is_rect);

/**
 * Draw the source marker where the samples are actually taken from.
 *
 * Drawn on the surface, in #CloneSourcePoint.frame: it lies in the surface's own plane and keeps
 * the orientation the texels are read in, instead of facing the camera at a fixed pixel size. Its
 * shape follows \a brush's #Brush.texture_clip_shape, read live so switching to Rectangle
 * mid-stroke is reflected immediately -- the same switch the sampling footprint honors.
 *
 * Call it from the object-space stage of the cursor pipeline: the object matrix must already be
 * on the GPU matrix stack, and \a pos must be the caller's bound 3-component position attribute.
 * Nothing here binds or unbinds a program or touches the shared vertex format.
 *
 * \param cursor_co_object: where the brush is, object space, or null when the caller does not
 * know. Relative mode slides the marker by the brush's travel since the anchor, so without this
 * the marker would sit at a point that mode has stopped reading from.
 */
void clone_draw_source_cursor(const CloneSourcePoint *source,
                              uint pos,
                              const float3 *cursor_co_object,
                              const ViewContext &vc,
                              const Paint &paint,
                              const Brush &brush);

/**
 * Draw the dashed source marker in Image Editor window coordinates.
 *
 * \param canvas_size: pixel size of the displayed canvas; the footprint radius is the brush
 * radius over its width, the same measure #clone_2d_stroke_dab stamps with. A zero width falls
 * back to a screen-space shape of \a marker_radius_px, exact only for an unrotated square canvas.
 */
void clone_2d_draw_source_cursor(const ImagePaintSettings &settings,
                                 const Paint &paint,
                                 const Brush &brush,
                                 const ARegion &region,
                                 const int2 &canvas_size,
                                 const float2 &cursor_uv,
                                 float marker_radius_px);

}  // namespace blender::ed::sculpt_paint::clone
