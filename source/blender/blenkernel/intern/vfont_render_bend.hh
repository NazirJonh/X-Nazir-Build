/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Text Bend on Curve: the per-glyph path layout of #BKE_vfont_object_render, split from the
 * straight raster in `vfont_render.cc`.
 */

#include "vfont_render_intern.hh"

#include "BKE_vfont_render.hh"

namespace blender {

struct ImBuf;

/**
 * Try to rasterize \a text along \a bend's curve into a square of \a side pixels. The glyph height
 * follows the Curve's Font Size (the straight path normalizes it away), so it is the one control
 * of the text's size on a curve.
 *
 * \return false when the bend is not usable and the caller has to rasterize straight: fewer than
 * two points, or a zero length on the projected polyline, or no glyph laid out on the curve. On
 * true, `*r_ibuf` holds the raster, or is null when its allocation failed; the whole render then
 * fails, exactly as a straight allocation failure would.
 */
bool vfont_render_bend_try(const VFontRenderText &text,
                           Vector<RenderFont> &fonts,
                           const VFontRenderBendParams &bend,
                           int side,
                           ImBuf **r_ibuf);

}  // namespace blender
