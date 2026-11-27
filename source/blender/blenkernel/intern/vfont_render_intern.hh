/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Shared state of the vector-font raster: the font a pass draws with, the laid-out text both the
 * straight and the bent path read, and the font helpers they both use. Private to the
 * `vfont_render*.cc` files.
 */

#include <cctype>

#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_curve_types.h"

#include "BKE_vfont.hh"

namespace blender {

struct VChar;
struct VFont;
struct VFontData;

/**
 * A vector font used by the raster, with the metrics its layout units are based on. The
 * layout unit of #CharTrans::offset is a fraction of the font's bounding box: the glyph
 * extraction scales outline coordinates and advances by `1 / bbox height`
 * (`blf_character_to_curves`). The em box of the font spans `em_ratio` layout units.
 */
struct RenderFont {
  const VFont *vfont = nullptr;
  /** The BLF identifier, or -1 when the font could not be loaded. */
  int blf_id = -1;
  /** ascent/descent of the em box in layout units, for glyphs without cached bounds. */
  float ascent = 0.0f;
  float descent = 0.0f;
  /**
   * BLF pixels per layout unit, per unit of BLF size (sizes scale linearly). Sampling the
   * FreeType line height at a reference size is exact for fonts without a line gap and
   * only slightly under-sizes the glyphs otherwise.
   */
  float px_per_unit_per_size = 0.0f;
  /** The size currently set in BLF, to skip redundant size changes per character. */
  float size_set = -1.0f;
};

/** The laid-out text a raster pass draws, from #BKE_vfont_to_curve_ex. */
struct VFontRenderText {
  const Curve &cu;
  const char32_t *text;
  int text_len;
  const CharTrans *chartrans;
};

/** True when the layout truncates at \a ct and the raster must stop its scan there. */
inline bool vfont_render_char_is_truncated(const Curve &cu, const CharTrans &ct)
{
  return (cu.overflow == CU_OVERFLOW_TRUNCATE) && ct.is_overflow;
}

/** True for a character the raster never draws: a line break or the string terminator. */
inline bool vfont_render_char_is_blank(const char32_t charcode)
{
  return ELEM(charcode, '\n', '\0');
}

/** The character as the raster draws it: small caps are upper-cased first. */
inline char32_t vfont_render_char_code(const CharTrans &ct, const char32_t *text, const int index)
{
  return ct.is_smallcaps ? char32_t(toupper(text[index])) : text[index];
}

const CharInfo *vfont_render_charinfo(const Curve &cu);

VFont *vfont_render_font_from_charinfo(const Curve &cu, const CharInfo &info);

/** The already collected font of \a vfont, or null. */
RenderFont *render_font_find(Vector<RenderFont> &fonts, const VFont *vfont);

/**
 * The glyph of a character, from the cache the layout has filled. Null when the character does
 * not exist in the font or the font has no data.
 */
const VChar *vfont_render_char_find(const VFontData *vfd, char32_t charcode);

/** Set the BLF size that renders one layout unit every \a px_per_unit pixels. */
void vfont_render_font_size_set(RenderFont &font, float px_per_unit);

}  // namespace blender
