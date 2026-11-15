/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

/** \file
 * \ingroup bke
 */

namespace blender {

struct ImBuf;
struct Object;

/**
 * How the raster fits the text's ink box into the square. The values are independent of the
 * Paint Layers DNA enum: the conversion belongs to the caller that owns the payload.
 */
enum class VFontRenderFit : int8_t {
  /** The longer side of the ink box fills the square minus padding; the aspect is kept. */
  Contain = 0,
  /** Both sides fill the square minus padding; the glyphs are resampled anisotropically. */
  Stretch = 1,
};

/** What #BKE_vfont_object_render rasterizes. */
struct VFontRenderParams {
  /** The square's side in pixels. */
  int side = 1024;
  /** The margin around the ink, as a fraction of #side, clamped to 0..0.5. */
  float padding = 0.0f;
  VFontRenderFit fit = VFontRenderFit::Contain;
};

/**
 * Rasterize a Text object's styled content into a side x side float RGBA ImBuf
 * (premultiplied, scene-linear, transparent outside the glyphs), fitting the layout
 * into the square minus \a padding (a fraction of \a side, clamped to 0..0.5).
 *
 * The layout, the line wrapping, the per-character styles and the alignment come from the
 * Text object itself: the character positions of #BKE_vfont_to_curve_ex are drawn through
 * the BLF buffer path, so the result matches the 3D text up to glyph hinting. The ink
 * bounds of the glyphs drive the fit, so a change of the text never jumps the scale.
 *
 * Known limitations (matching the buffer path of BLF, not the 3D geometry):
 * - per-character rotation (#CharTrans::rotate) is not applied, BLF cannot rotate into a
 *   memory buffer;
 * - #CharInfo::mat_nr is ignored. The glyphs are drawn white only so the alpha is their
 *   coverage: a Text Decal keeps nothing but that alpha and paints it with its own channel
 *   values, as a Fill does, not with the text object's color.
 *
 * Returns null when \a text_object_eval is not an #OB_FONT object, holds no text, or has
 * no usable font. The caller owns the returned buffer and frees it with #IMB_freeImBuf.
 *
 * \note Must run on the main thread: the BLF font map is global. The fonts are loaded with
 * the unique variants and unloaded before returning, so no BLF state is left behind.
 *
 * \param text_object_eval: must be evaluated, #BKE_vfont_to_curve_ex lays out evaluated
 * data. An original object works too, but only reflects its un-animated values.
 */
ImBuf *BKE_vfont_object_render(const Object &text_object_eval, const VFontRenderParams &params);

}  // namespace blender
