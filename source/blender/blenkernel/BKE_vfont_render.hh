/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

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

/**
 * How the text is laid along the bend curve. The values are independent of the Paint Layers
 * DNA enum: the conversion belongs to the caller that owns the payload.
 */
enum class VFontRenderBendMode : int8_t {
  /**
   * The text spans the curve: the advances and the glyph widths stretch along the curve so the
   * whole text fills it from the bend offset to its end. The glyph height stays the raster's fit
   * scale, and nothing ever clips or overlaps; the fit and the padding are not read.
   */
  Fit = 0,
  /**
   * The glyphs keep their own shape at the raster's fit scale, the advances map onto the curve one
   * to one, and glyphs past the curve's end are dropped.
   */
  Clip = 1,
  /** The glyphs keep their own shape at the raster's fit scale, and glyphs past the curve's end
   * pile up at the end point. */
  Clamp = 2,
};

/** How #BKE_vfont_object_render bends the text along a curve; no points keeps it straight. */
struct VFontRenderBendParams {
  /**
   * The control polyline in the raster's own normalized UV, already projected -- the caller turns
   * a 3D curve into it through the decal's frame -- so the bend reads no object space and no
   * container. Empty keeps the text straight.
   */
  Span<float2> points;
  /** Whether the polyline closes; a closed one is laid along as if open, its last point linking
   * back to the first. */
  bool cyclic = false;
  /**
   * Where along the curve the text starts, as a fraction of its arc length. It is clamped to
   * 0..#VFONT_RENDER_BEND_OFFSET_MAX, so the Fit mode always has a positive span to lay the
   * text over. The text does not wrap around a cyclic curve.
   */
  float offset = 0.0f;
  /** Perpendicular shift of the baseline off the curve, as a fraction of the raster side. */
  float baseline = 0.0f;
  /**
   * Pixels per layout unit the glyphs are drawn at: the straight raster's own fit scale, so
   * switching the bend on keeps the text the size the decal already shows. Zero falls back to the
   * Font Size's unit fraction, for a caller that has no fit scale to hand.
   */
  float glyph_px = 0.0f;
  /** When set, #glyph_px is the caller's own scale (a canvas text's Font Size in UV) and the
   * straight raster's fit scale must not overwrite it. */
  bool glyph_px_explicit = false;
  VFontRenderBendMode mode = VFontRenderBendMode::Fit;
};

/**
 * Largest #VFontRenderBendParams::offset. The last sliver of the curve is never a start point,
 * both in the RNA range and in the layout clamp, so the text cannot start exactly at the end.
 */
constexpr float VFONT_RENDER_BEND_OFFSET_MAX = 0.99f;

/** What #BKE_vfont_object_render rasterizes. */
struct VFontRenderParams {
  /** The square's side in pixels. */
  int side = 1024;
  /** The margin around the ink, as a fraction of #side, clamped to 0..0.5. */
  float padding = 0.0f;
  VFontRenderFit fit = VFontRenderFit::Contain;
  /**
   * Multiplier of the automatic fit: 1.0 fills the square as before, 0.5 halves the text, 2.0
   * doubles it. Zero reads as 1.0, so a file written before the field existed renders unchanged.
   * It is applied to the straight and bent layouts alike; a value above 1 may clip at the raster
   * edge, which is the caller's choice. Stretch applies it to the composed size, not to the
   * aspect-filling draw scale.
   */
  float fit_multiplier = 1.0f;
  /** The text-on-curve layout. A null curve rasterizes straight, exactly as before. */
  VFontRenderBendParams bend;
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
 * When \a params.bend.points is not empty, the text is laid along that polyline instead of
 * straight lines: each glyph is drawn into its own tile and composited rotated, at the curve
 * point its cumulative advance maps to, turned along the tangent. A multi-line text flows along
 * the curve (each line continues after the previous one), and the line offsets leave the curve
 * perpendicular to it. The bend is skipped -- the straight raster is returned -- when the
 * polyline is unusable (fewer than two points, or a zero length).
 *
 * Known limitations (matching the buffer path of BLF, not the 3D geometry):
 * - per-character rotation (#CharTrans::rotate) is not applied: BLF cannot rotate into a memory
 *   buffer. The bent path turns every glyph along the curve's tangent instead, so an explicitly
 *   rotated character loses that rotation there as well.
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
