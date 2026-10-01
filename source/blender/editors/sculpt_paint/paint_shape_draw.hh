/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared GPU overlay of the shape drawing tools: the animated dashed outline. The functions take
 * points already translated into region pixels; the coordinate mapping stays with the caller.
 */

#pragma once

#include "BLI_span.hh"

#include "BLI_math_vector_types.hh"

namespace blender::ed::sculpt_paint::shape {

/** Animated dashed outline through \a region_points (already in region pixels). */
void shape_draw_dashed_outline(Span<float2> region_points, bool loop);

/** A thin, muted solid outline, the quiet counterpart of the animated dashed outline. */
void shape_draw_plain_outline(Span<float2> region_points, bool loop);

}  // namespace blender::ed::sculpt_paint::shape
