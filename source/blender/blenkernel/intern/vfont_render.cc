/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_vfont_render.hh"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_memory_utils.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "DNA_curve_types.h"
#include "DNA_object_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_vfont_types.h"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_vfont.hh"
#include "BKE_vfontdata.hh"

#include "BLF_api.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "vfont_render_bend.hh"
#include "vfont_render_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Fonts and Layout Metrics
 * \{ */

/* The size the font metrics are sampled at. Arbitrary, only the ratio matters. */
constexpr float FONT_METRICS_REFERENCE_SIZE = 64.0f;

/**
 * The per-character styles of the live text. In Edit Mode `strinfo` lags behind the edit buffer
 * until the mode ends, so the raster has to read the styles from the same buffer the layout did.
 */
const CharInfo *vfont_render_charinfo(const Curve &cu)
{
  return cu.editfont != nullptr ? cu.editfont->textbufinfo : cu.strinfo;
}

/**
 * Load a BLF font for the given vector font. Unique instances are used so the raster never
 * shares BLF state with the UI; the caller unloads the returned identifier.
 */
static int vfont_render_font_load(const VFont *vfont)
{
  if (!vfont || BKE_vfont_is_builtin(vfont)) {
    return BLF_load_default(true);
  }

  if (vfont->packedfile != nullptr) {
    char name[MAX_ID_FULL_NAME];
    BKE_id_full_name_get(name, &vfont->id, 0);
    return BLF_load_mem_unique(
        name, static_cast<const uchar *>(vfont->packedfile->data), vfont->packedfile->size);
  }

  char filepath[FILE_MAX];
  STRNCPY(filepath, vfont->filepath);
  BLI_path_abs(filepath, ID_BLEND_PATH_FROM_GLOBAL(&vfont->id));
  return BLF_load_unique(filepath);
}

/**
 * The per-character font selection of the 3D text geometry (`vfont_from_charinfo`): styles
 * resolve to their own font file and fall back to the regular one. No synthetic bold or
 * italic, the 3D text does not fake them either.
 */
VFont *vfont_render_font_from_charinfo(const Curve &cu, const CharInfo &info)
{
  switch (int(info.flag & (CU_CHINFO_BOLD | CU_CHINFO_ITALIC))) {
    case CU_CHINFO_BOLD:
      return cu.vfontb ? cu.vfontb : cu.vfont;
    case CU_CHINFO_ITALIC:
      return cu.vfonti ? cu.vfonti : cu.vfont;
    case (CU_CHINFO_BOLD | CU_CHINFO_ITALIC):
      return cu.vfontbi ? cu.vfontbi : cu.vfont;
    default:
      return cu.vfont;
  }
}

/** The already collected font of \a vfont, or null. */
RenderFont *render_font_find(Vector<RenderFont> &fonts, const VFont *vfont)
{
  for (RenderFont &font : fonts) {
    if (font.vfont == vfont) {
      return &font;
    }
  }
  return nullptr;
}

static void vfont_render_font_metrics_fill(RenderFont &font)
{
  VFontData_Metrics metrics;
  if (font.vfont->data != nullptr) {
    metrics = font.vfont->data->metrics;
  }
  else {
    BKE_vfontdata_metrics_get_defaults(&metrics);
  }

  font.ascent = metrics.ascend_ratio * metrics.em_ratio;
  font.descent = metrics.em_ratio - font.ascent;
}

/**
 * Measure how many BLF pixels render one layout unit per unit of BLF size. Must be called
 * once the font is loaded, with the identifier set.
 */
static void vfont_render_font_measure(RenderFont &font)
{
  BLF_size(font.blf_id, FONT_METRICS_REFERENCE_SIZE);
  font.size_set = FONT_METRICS_REFERENCE_SIZE;
  const float height_ref = BLF_height_max(font.blf_id);
  const float em_ratio = font.vfont->data != nullptr ? font.vfont->data->metrics.em_ratio :
                                                       BLF_VFONT_METRICS_EM_RATIO_DEFAULT;
  if (height_ref <= 0.0f || em_ratio <= 0.0f) {
    font.px_per_unit_per_size = 0.0f;
    return;
  }
  font.px_per_unit_per_size = height_ref / (em_ratio * FONT_METRICS_REFERENCE_SIZE);
}

/**
 * The glyph of a character, from the cache the layout has filled. Null when the character
 * does not exist in the font or the font has no data.
 */
const VChar *vfont_render_char_find(const VFontData *vfd, const char32_t charcode)
{
  if (vfd == nullptr) {
    return nullptr;
  }
  if (VChar *const *che_p = vfd->characters->lookup_ptr(uint(charcode))) {
    return *che_p;
  }
  return nullptr;
}

/** Set the BLF size that renders one layout unit every \a px_per_unit pixels. */
void vfont_render_font_size_set(RenderFont &font, const float px_per_unit)
{
  if (font.px_per_unit_per_size <= 0.0f) {
    return;
  }
  const float size = px_per_unit / font.px_per_unit_per_size;
  if (font.size_set != size) {
    BLF_size(font.blf_id, size);
    font.size_set = size;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

/**
 * First pass: collect the fonts the text uses and the ink bounding box in layout units. The box
 * drives the fit, so it has to be known before any glyph is drawn.
 */
static bool layout_collect(const VFontRenderText &text, Vector<RenderFont> &r_fonts, rctf &r_bbox)
{
  const CharInfo *charinfo = vfont_render_charinfo(text.cu);
  r_bbox.xmin = r_bbox.ymin = FLT_MAX;
  r_bbox.xmax = r_bbox.ymax = -FLT_MAX;

  for (const int a : IndexRange(text.text_len)) {
    const CharTrans &ct = text.chartrans[a];
    if (vfont_render_char_is_truncated(text.cu, ct)) {
      /* The 3D text truncates the layout at this character as well. */
      break;
    }
    const char32_t charcode = vfont_render_char_code(ct, text.text, a);
    if (vfont_render_char_is_blank(charcode)) {
      continue;
    }

    const CharInfo &info = charinfo[a];
    const VFont *vfont = vfont_render_font_from_charinfo(text.cu, info);
    if (vfont == nullptr) {
      continue;
    }
    RenderFont *font = render_font_find(r_fonts, vfont);
    if (font == nullptr) {
      r_fonts.append_as();
      font = &r_fonts.last();
      font->vfont = vfont;
      vfont_render_font_metrics_fill(*font);
    }
    const VChar *che = vfont_render_char_find(vfont->data, charcode);
    const float smallcaps_scale = ct.is_smallcaps ? text.cu.smallcaps_scale : 1.0f;
    if (che != nullptr) {
      if (che->bounds.xmin < che->bounds.xmax || che->bounds.ymin < che->bounds.ymax) {
        r_bbox.xmin = std::min(r_bbox.xmin, ct.offset.x + che->bounds.xmin * smallcaps_scale);
        r_bbox.xmax = std::max(r_bbox.xmax, ct.offset.x + che->bounds.xmax * smallcaps_scale);
        r_bbox.ymin = std::min(r_bbox.ymin, ct.offset.y + che->bounds.ymin * smallcaps_scale);
        r_bbox.ymax = std::max(r_bbox.ymax, ct.offset.y + che->bounds.ymax * smallcaps_scale);
      }
    }
    else {
      /* No cached bounds: fall back to the em box so the character cannot collapse the
       * layout nor clip when BLF draws its own fallback glyph for it. */
      r_bbox.xmin = std::min(r_bbox.xmin, ct.offset.x);
      r_bbox.xmax = std::max(r_bbox.xmax, ct.offset.x);
      r_bbox.ymin = std::min(r_bbox.ymin, ct.offset.y - font->descent);
      r_bbox.ymax = std::max(r_bbox.ymax, ct.offset.y + font->ascent);
    }

    if (info.flag & CU_CHINFO_UNDERLINE) {
      /* The underline sits below the ink of most characters, include it in the box. */
      const float band_top = ct.offset.y + text.cu.ulpos - 0.05f;
      r_bbox.ymin = std::min(r_bbox.ymin, band_top - text.cu.ulheight);
    }
  }

  return !r_fonts.is_empty() && r_bbox.xmin <= r_bbox.xmax && r_bbox.ymin <= r_bbox.ymax;
}

/**
 * Load the BLF fonts of \a fonts. The caller unloads every loaded identifier via
 * #vfont_render_fonts_unload, including on the early exits below.
 */
static bool fonts_load(Vector<RenderFont> &fonts)
{
  bool has_font = false;
  for (RenderFont &font : fonts) {
    font.blf_id = vfont_render_font_load(font.vfont);
    if (font.blf_id != -1) {
      vfont_render_font_measure(font);
      has_font = has_font || (font.px_per_unit_per_size > 0.0f);
    }
  }
  return has_font;
}

/** Unload every loaded font and detach it from the shared BLF buffer, once per raster. */
static void vfont_render_fonts_unload(Vector<RenderFont> &fonts)
{
  for (RenderFont &font : fonts) {
    if (font.blf_id != -1) {
      BLF_buffer(font.blf_id, nullptr, nullptr, 0, 0, 1, nullptr);
      BLF_unload_id(font.blf_id);
      font.blf_id = -1;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rasterization
 * \{ */

/**
 * Fill a solid premultiplied-white rectangle into the float buffer, clipped to the raster.
 * The coordinates are pixel bounds, lower-left origin.
 */
static void vfont_render_fill_rect(
    float *buffer, const int side, float x0, float x1, float y0, float y1)
{
  const int ix0 = std::clamp(int(std::floor(x0)), 0, side);
  const int ix1 = std::clamp(int(std::ceil(x1)), 0, side);
  const int iy0 = std::clamp(int(std::floor(y0)), 0, side);
  const int iy1 = std::clamp(int(std::ceil(y1)), 0, side);
  if (ix1 <= ix0 || iy1 <= iy0) {
    return;
  }
  for (const int y : IndexRange(iy0, iy1 - iy0)) {
    float *pixel = buffer + (size_t(y) * side + ix0) * 4;
    for ([[maybe_unused]] const int x : IndexRange(ix0, ix1 - ix0)) {
      pixel[0] = 1.0f;
      pixel[1] = 1.0f;
      pixel[2] = 1.0f;
      pixel[3] = 1.0f;
      pixel += 4;
    }
  }
}

/** Draw every glyph of the layout into \a draw_ibuf, white so that its alpha is the coverage. */
static void glyphs_draw(const VFontRenderText &text,
                        Vector<RenderFont> &fonts,
                        ImBuf &draw_ibuf,
                        const rctf &bbox,
                        const float scale,
                        const float x_shift,
                        const float y_shift)
{
  const CharInfo *charinfo = vfont_render_charinfo(text.cu);
  const bool xtrax_contract = text.cu.spacing < 1.0f;
  const float xtrax = xtrax_contract ? (text.cu.spacing - 1.0f) : (0.5f * text.cu.spacing - 0.5f);

  float *draw_buffer = draw_ibuf.float_data_for_write();
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  for (const RenderFont &font : fonts) {
    if (font.blf_id != -1 && font.px_per_unit_per_size > 0.0f) {
      BLF_buffer_col(font.blf_id, white);
      BLF_buffer(
          font.blf_id, draw_buffer, nullptr, int(draw_ibuf.x), int(draw_ibuf.y), 4, nullptr);
    }
  }

  const auto to_pixel_x = [&](const float layout_x) {
    return (layout_x - bbox.xmin) * scale + x_shift;
  };
  const auto to_pixel_y = [&](const float layout_y) {
    return (layout_y - bbox.ymin) * scale + y_shift;
  };

  char ch_utf8[BLI_UTF8_MAX + 1];
  for (const int a : IndexRange(text.text_len)) {
    const CharTrans &ct = text.chartrans[a];
    if (vfont_render_char_is_truncated(text.cu, ct)) {
      break;
    }
    const char32_t charcode = vfont_render_char_code(ct, text.text, a);
    if (vfont_render_char_is_blank(charcode)) {
      continue;
    }

    const CharInfo &info = charinfo[a];
    const VFont *vfont = vfont_render_font_from_charinfo(text.cu, info);
    RenderFont *font = render_font_find(fonts, vfont);
    if (font == nullptr || font->blf_id == -1 || font->px_per_unit_per_size <= 0.0f) {
      continue;
    }

    vfont_render_font_size_set(*font, ct.is_smallcaps ? scale * text.cu.smallcaps_scale : scale);
    const size_t ch_utf8_len = BLI_str_utf8_from_unicode(uint(charcode), ch_utf8, sizeof(ch_utf8));
    if (ch_utf8_len == 0) {
      continue;
    }
    BLF_position(font->blf_id, to_pixel_x(ct.offset.x), to_pixel_y(ct.offset.y), 0.0f);
    BLF_draw_buffer(font->blf_id, ch_utf8, ch_utf8_len);

    if (info.flag & CU_CHINFO_UNDERLINE) {
      /* The per-character underline rectangle of the 3D geometry (`build_underline`): from
       * the character origin to its advance, just below the baseline. */
      const VChar *che = vfont_render_char_find(vfont->data, charcode);
      const float twidth = (che != nullptr ? che->width : 0.0f) *
                           (ct.is_smallcaps ? text.cu.smallcaps_scale : 1.0f) *
                           (1.0f + info.kern / 40.0f);
      bool continues = false;
      if (a + 1 < text.text_len) {
        const CharTrans &ct_next = text.chartrans[a + 1];
        const CharInfo &info_next = charinfo[a + 1];
        continues = (text.text[a + 1] != '\n') &&
                    ((text.text[a + 1] != ' ') || (info_next.flag & CU_CHINFO_UNDERLINE)) &&
                    (ct_next.is_wrap == 0);
      }
      const float overlap = continues ? (xtrax_contract ? (xtrax * twidth) : xtrax) : 0.0f;
      const float band_top = to_pixel_y(ct.offset.y + text.cu.ulpos - 0.05f);
      vfont_render_fill_rect(draw_buffer,
                             draw_ibuf.x,
                             to_pixel_x(ct.offset.x),
                             to_pixel_x(ct.offset.x) + (twidth + overlap) * scale,
                             band_top - text.cu.ulheight * scale,
                             band_top);
    }
  }
}

/**
 * Stretch the non-square drawing into the fitted square and paste it, centered, into the padded
 * raster. The drawing buffer is scaled in place, so no second full-size picture is allocated while
 * it is still needed.
 */
static ImBuf *stretch_compose(ImBuf *draw_ibuf,
                              const int side,
                              const float padding_px,
                              const float box_px,
                              const float target_px)
{
  if (!IMB_scale(draw_ibuf, int(target_px), int(target_px), IMBScaleFilter::Box)) {
    return nullptr;
  }
  ImBuf *ibuf = IMB_allocImBuf(uint(side), uint(side), ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    return nullptr;
  }
  /* The composed square is centred in the padded box; a multiplier below 1 shrinks it there
   * instead of pinning it to the corner. */
  const int origin_x = int(padding_px + 2.0f + std::max(0.0f, (box_px - target_px) * 0.5f));
  const int origin_y = int(padding_px + 2.0f + std::max(0.0f, (box_px - target_px) * 0.5f));
  for (const int y : IndexRange(draw_ibuf->y)) {
    memcpy(ibuf->float_data_for_write() + (size_t(origin_y + y) * side + origin_x) * 4,
           draw_ibuf->float_data() + (size_t(y) * draw_ibuf->x) * 4,
           sizeof(float[4]) * draw_ibuf->x);
  }
  return ibuf;
}

/**
 * Skew a premultiplied float buffer horizontally: every pixel moves by `shear * (y - origin_y)`,
 * the 3D text's own shear rule. Bilinear on the inverse mapping; outside the source is transparent.
 */
static void vfont_render_shear(ImBuf &ibuf, const float shear, const float origin_y)
{
  const int width = ibuf.x;
  const int height = ibuf.y;
  const float *src = ibuf.float_data();
  if (src == nullptr) {
    return;
  }
  float *dst = ibuf.float_data_for_write();
  if (dst == nullptr) {
    return;
  }
  /* The write may have re-copied the buffer to make it mutable; read from the current one. */
  src = ibuf.float_data();
  /* The shear is horizontal, so every output row samples a single source row. Keep that row in a
   * one-row buffer instead of copying the whole float raster; the copy also keeps `dst` safe when
   * it aliases `src` (in-place write). */
  Vector<float> row;
  row.resize(int64_t(width) * 4);
  for (const int y : IndexRange(height)) {
    memcpy(row.data(), src + (size_t(y) * width) * 4, sizeof(float) * size_t(width) * 4);
    for (const int x : IndexRange(width)) {
      const float sx = float(x) + 0.5f - shear * (float(y) + 0.5f - origin_y) - 0.5f;
      float *pixel = dst + (size_t(y) * width + x) * 4;
      if (sx <= -0.5f || sx >= float(width) - 0.5f) {
        pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0.0f;
        continue;
      }
      const int ix = int(std::floor(sx));
      const float tx = sx - float(ix);
      const auto texel = [&](const int cx) -> const float * {
        const int cx_clamped = std::clamp(cx, 0, width - 1);
        return row.data() + size_t(cx_clamped) * 4;
      };
      const float *p0 = texel(ix);
      const float *p1 = texel(ix + 1);
      for (const int c : IndexRange(4)) {
        pixel[c] = p0[c] + (p1[c] - p0[c]) * tx;
      }
    }
  }
}

/** \} */

ImBuf *BKE_vfont_object_render(const Object &text_object_eval, const VFontRenderParams &params)
{
  const int side = params.side;
  if (text_object_eval.type != OB_FONT || side < 2) {
    return nullptr;
  }
  const Curve *cu = id_cast<const Curve *>(text_object_eval.data);
  if (cu == nullptr) {
    return nullptr;
  }

  /* The character layout of the 3D text: positions in layout units, line wrapping, the
   * text boxes, small caps and the overflow truncation are all decided here already. */
  const char32_t *text = nullptr;
  int text_len = 0;
  bool text_free = false;
  CharTrans *chartrans = nullptr;
  if (!BKE_vfont_to_curve_ex(const_cast<Object *>(&text_object_eval),
                             *cu,
                             FO_DUPLI,
                             nullptr,
                             &text,
                             &text_len,
                             &text_free,
                             &chartrans,
                             nullptr))
  {
    return nullptr;
  }
  BLI_SCOPED_DEFER([&]() {
    MEM_delete(chartrans);
    if (text_free) {
      MEM_delete(text);
    }
  });

  if (text == nullptr || chartrans == nullptr || text_len == 0) {
    return nullptr;
  }

  /* Fonts only referenced by the text are in `vfont*`, and the layout has loaded the data
   * of every one it laid out with. Without the data the layout worked from placeholders
   * and there is no usable font to draw with. */
  if (cu->vfont == nullptr || cu->vfont->data == nullptr) {
    return nullptr;
  }

  const VFontRenderText text_ctx{*cu, text, text_len, chartrans};
  Vector<RenderFont> fonts;
  rctf bbox;
  if (!layout_collect(text_ctx, fonts, bbox)) {
    return nullptr;
  }

  if (!fonts_load(fonts)) {
    vfont_render_fonts_unload(fonts);
    return nullptr;
  }
  /* Every exit below this point unloads the fonts and detaches the BLF buffer. */
  BLI_SCOPED_DEFER([&]() { vfont_render_fonts_unload(fonts); });

  /* Fit: the longer side of the ink box fills the square minus padding. A couple of
   * pixels of extra margin keep the antialiased ink away from the raster edge. */
  constexpr float SAFETY_PX = 2.0f;
  const float padding_px = std::clamp(params.padding, 0.0f, 0.5f) * float(side);
  const float fit_px = std::max(float(side) - 2.0f * padding_px - 2.0f * SAFETY_PX, 8.0f);
  const float extent_x = bbox.xmax - bbox.xmin;
  const float extent_y = bbox.ymax - bbox.ymin;
  const float auto_fit_scale = fit_px / std::max(extent_x, std::max(extent_y, 1e-6f));
  /* The caller's size multiplier scales the automatic fit, so the decal can both shrink and grow
   * its text without touching the source Text object. Zero reads as 1.0 for files written before
   * the field existed. */
  const float fit_multiplier = (params.fit_multiplier > 0.0f) ? params.fit_multiplier : 1.0f;
  const float draw_scale = auto_fit_scale * fit_multiplier;

  /* The bend: a usable curve in the bend params turns the drawing into per-glyph placements
   * along it, drawn straight into the square -- the curve is in the raster's own UV space, so
   * no fit maps the result anywhere. A container without a usable curve (no Curve Patch item,
   * a single point, a zero length) rasterizes straight, exactly as before. */
  ImBuf *bent_ibuf = nullptr;
  VFontRenderBendParams bend = params.bend;
  if (!bend.glyph_px_explicit) {
    bend.glyph_px = draw_scale;
  }
  if (vfont_render_bend_try(text_ctx, fonts, bend, side, &bent_ibuf)) {
    return bent_ibuf;
  }

  const bool stretch = params.fit == VFontRenderFit::Stretch;
  /* Stretch draws at the aspect-filling scale and applies the multiplier when it composes the
   * square, so the anisotropic fill keeps both extents equal. */
  const float straight_scale = stretch ? auto_fit_scale : draw_scale;
  float x_shift = padding_px + SAFETY_PX;
  float y_shift = padding_px + SAFETY_PX;
  ImBuf *ibuf = nullptr;
  ImBuf *draw_ibuf = nullptr;
  if (stretch) {
    /* BLF cannot scale the glyphs anisotropically: draw at a uniform scale into an extent-sized
     * buffer, then resample it. */
    const int temp_side_x = int(std::ceil(extent_x * straight_scale)) + int(2.0f * SAFETY_PX);
    const int temp_side_y = int(std::ceil(extent_y * straight_scale)) + int(2.0f * SAFETY_PX);
    draw_ibuf = IMB_allocImBuf(
        uint(std::max(temp_side_x, 2)), uint(std::max(temp_side_y, 2)), ImBufFlags::FloatData);
    if (draw_ibuf == nullptr) {
      return nullptr;
    }
  }
  else {
    ibuf = IMB_allocImBuf(uint(side), uint(side), ImBufFlags::FloatData);
    if (ibuf == nullptr) {
      return nullptr;
    }
    draw_ibuf = ibuf;
    x_shift += (fit_px - extent_x * straight_scale) * 0.5f;
    y_shift += (fit_px - extent_y * straight_scale) * 0.5f;
  }

  glyphs_draw(text_ctx, fonts, *draw_ibuf, bbox, straight_scale, x_shift, y_shift);

  /* Shear skews the glyph outlines by their layout Y in the 3D text (`BKE_vfont_to_curve` adds
   * `shear * y` to every curve point); BLF cannot skew, so the straight raster is skewed after the
   * glyphs are drawn. The pivot is the layout origin, which `y_shift` maps to the buffer. */
  if (text_ctx.cu.shear != 0.0f) {
    vfont_render_shear(*draw_ibuf, text_ctx.cu.shear, y_shift);
  }

  if (stretch) {
    /* The composed square never exceeds the padded box: a multiplier above 1 cannot grow past the
     * raster, while one below 1 shrinks the text inside it. */
    const float target_px = std::clamp(
        fit_px * fit_multiplier, 8.0f, std::max(fit_px, 8.0f));
    ImBuf *composed = stretch_compose(draw_ibuf, side, padding_px, fit_px, target_px);
    IMB_freeImBuf(draw_ibuf);
    return composed;
  }
  return ibuf;
}

/** \} */

}  // namespace blender
