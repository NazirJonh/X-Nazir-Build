/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Text Bend on Curve: the text-on-path layout of #BKE_vfont_object_render. The straight advance of
 * every glyph maps to an arc length along the bend curve, and the glyph lands at that point,
 * turned along the tangent, its line offset and the baseline shift leaving the curve perpendicular
 * to it. The scheme is the 3D text's Text-on-Curve pass (`vfont_curve.cc`) narrowed to a 2D spline
 * in the raster's own pixels: there, the curve is an evaluated object read through
 * #BKE_where_on_path; here, the curve is the bend container's Curve Patch geometry read through
 * #bke::CurvePatchSpline2D, so no evaluation of another object is involved.
 */

#include "vfont_render_bend.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "DNA_curve_types.h"
#include "DNA_vfont_types.h"

#include "BKE_curve_patch.hh"
#include "BKE_vfontdata.hh"

#include "BLF_api.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

/** A curve shorter than this, in raster pixels, is treated as unusable. */
constexpr float BEND_MIN_CURVE_PX = 1.0f;
/** Denominator floor, so a zero-width text or a zero-length curve cannot divide by zero. */
constexpr float BEND_MIN_SPAN = 1e-6f;
/** Spline length below which the curve is degenerate, in its own UV units. */
constexpr float BEND_MIN_SPLINE_LENGTH = 1e-9f;
/** Antialiased margin around a glyph's ink inside its tile, in pixels. */
constexpr float BEND_TILE_MARGIN_PX = 2.0f;
/**
 * The raster side fraction one layout unit takes at a Font Size of 1, used only as a fallback when
 * the caller has no fit scale. The straight path fits the ink box into the square, so the normal
 * bent path passes that same pixels-per-layout-unit scale and the text keeps the size the decal
 * already shows.
 */
constexpr float BEND_GLYPH_UNIT_FRACTION = 0.15f;
/** Floor of the Font Size read by the fallback, so a zero size cannot collapse every glyph. */
constexpr float BEND_MIN_FONT_SIZE = 1e-3f;

/** Where one drawn glyph of the bent layout lands. */
struct BendGlyphPlacement {
  /** The pixel the glyph's layout origin (its baseline-left corner) maps to. */
  float2 origin_px = {0.0f, 0.0f};
  /** The tangent angle at the glyph's arc position, radians. */
  float angle = 0.0f;
  /** The glyph's cumulative advance at its origin, in layout units. */
  float s_lay = 0.0f;
  /** True for a glyph that is not drawn: an undrawn character, or one the Clip mode dropped. */
  bool skip = true;
};

/** The arc mapping every bent glyph and underline band reads. */
struct BendLayout {
  /** The raster side, what the curve's UV positions multiply into pixels. */
  float side = 0.0f;
  /** Pixels per layout unit along the curve: what an advance maps to. */
  float unit_px = 0.0f;
  /**
   * Pixels per layout unit of the glyph size and of the line offsets across the curve. It is the
   * straight raster's fit scale, never the curve's length.
   */
  float glyph_px = 0.0f;
  /**
   * How much the Fit mode widens a glyph along the curve (`unit_px / glyph_px`), so the glyphs
   * fill the span with their gaps. Clip and Clamp keep the glyphs' own shape: 1.
   */
  float stretch_x = 1.0f;
  /** The pixel arc length the text starts at. */
  float offset_px = 0.0f;
  /** The perpendicular baseline shift, in pixels. */
  float baseline_px = 0.0f;
  /** The curve's pixel length, the arc the text lays over. */
  float curve_len_px = 0.0f;
};

/** The point and the left-hand normal of the curve at one pixel arc length. */
struct BendFrame {
  float2 point_px;
  /** Unit tangent in the curve's UV space; the glyphs turn by its angle. */
  float2 tangent_uv;
  /** Unit left-hand normal in UV space, which is the text's up. */
  float2 normal_uv;
};

/**
 * The one place the curve's point and frame are sampled: both the glyph placement and the
 * underline band's corners go through it, so the two cannot drift apart.
 */
static BendFrame bend_frame_at(const bke::CurvePatchSpline2D &spline,
                               const float arc_px,
                               const float side)
{
  /* The spline is sampled in its own UV units: the pixels divided by the raster side. */
  const float arc_uv = arc_px / side;
  BendFrame frame;
  frame.point_px = spline.evaluate(arc_uv) * side;
  frame.tangent_uv = spline.tangent_at(arc_uv);
  frame.normal_uv = float2(-frame.tangent_uv.y, frame.tangent_uv.x);
  return frame;
}

/**
 * The bend curve: the caller's already-projected polyline in the raster's normalized UV space.
 * False when it is unusable -- fewer than two points, or a zero length.
 */
static bool bend_spline_build(const VFontRenderBendParams &bend,
                              bke::CurvePatchSpline2D &r_spline)
{
  if (bend.points.size() < 2) {
    return false;
  }
  r_spline.build_from_positions(bend.points, {}, bend.cyclic);
  return !r_spline.is_empty() && r_spline.total_length() > BEND_MIN_SPLINE_LENGTH;
}

/**
 * The advance width of the glyph at \a index, in layout units, kerning included: the same width
 * the straight underline band spans. Without a cached glyph the width is zero, which only costs
 * the last glyph of a line its share of the span.
 */
static float bend_glyph_advance(const VFontRenderText &text, const int index)
{
  const CharTrans &ct = text.chartrans[index];
  const CharInfo &info = vfont_render_charinfo(text.cu)[index];
  const VFont *vfont = vfont_render_font_from_charinfo(text.cu, info);
  if (vfont == nullptr) {
    return 0.0f;
  }
  const VChar *che = vfont_render_char_find(vfont->data,
                                            vfont_render_char_code(ct, text.text, index));
  if (che == nullptr) {
    return 0.0f;
  }
  return che->width * (ct.is_smallcaps ? text.cu.smallcaps_scale : 1.0f) *
         (1.0f + info.kern / 40.0f);
}

/**
 * The drawn characters' advance extent per line, and the whole advance width the modes scale
 * from: a line's glyphs run from its first drawn character's offset to the far edge of its last
 * one, so the text fills its span exactly, and the width of every line adds up, so a multi-line
 * text flows along the curve the way a text-on-path editor flows it. The overflow truncation cuts
 * the scan where the straight drawing stops as well.
 */
static bool bend_layout_measure(const VFontRenderText &text,
                                MutableSpan<float> r_line_min,
                                MutableSpan<float> r_line_max,
                                float &r_total_width)
{
  r_line_min.fill(FLT_MAX);
  r_line_max.fill(-FLT_MAX);
  bool any = false;
  for (const int a : IndexRange(text.text_len)) {
    const CharTrans &ct = text.chartrans[a];
    if (vfont_render_char_is_truncated(text.cu, ct)) {
      break;
    }
    if (vfont_render_char_is_blank(text.text[a])) {
      continue;
    }
    const short line = ct.linenr;
    r_line_min[line] = std::min(r_line_min[line], ct.offset.x);
    r_line_max[line] = std::max(r_line_max[line], ct.offset.x + bend_glyph_advance(text, a));
    any = true;
  }
  if (!any) {
    return false;
  }
  float total = 0.0f;
  for (const int line : IndexRange(text.text_len)) {
    if (r_line_min[line] <= r_line_max[line]) {
      total += std::max(r_line_max[line] - r_line_min[line], 0.0f);
    }
  }
  r_total_width = total;
  return total > 0.0f;
}

/**
 * The arc mapping of \a bend. The glyph height is the raster's fit scale in every mode. The Fit
 * mode then scales the whole advance over the curve length left of the start offset and widens
 * each glyph by the same factor, so the text spans the curve; the Clip and Clamp modes keep the
 * glyphs' own shape, the advances mapping onto the curve one to one.
 */
static BendLayout bend_layout_compute(const VFontRenderBendParams &bend,
                                      const float curve_len_px,
                                      const float side,
                                      const float font_size,
                                      const float total_width_lay)
{
  BendLayout layout;
  layout.side = side;
  layout.curve_len_px = curve_len_px;
  const float offset_frac = math::clamp(bend.offset, 0.0f, VFONT_RENDER_BEND_OFFSET_MAX);
  layout.offset_px = offset_frac * curve_len_px;
  layout.baseline_px = math::clamp(bend.baseline, -1.0f, 1.0f) * side;
  /* The glyph height is the straight raster's fit scale when the caller passes one: the bent text
   * then looks exactly like the straight one, only laid along the curve. The Font Size fallback
   * serves a caller with no fit (no ink box measured). */
  layout.glyph_px = (bend.glyph_px > 0.0f) ?
                        bend.glyph_px :
                        BEND_GLYPH_UNIT_FRACTION * side * std::max(font_size, BEND_MIN_FONT_SIZE);
  if (bend.mode == VFontRenderBendMode::Fit) {
    const float available_px = std::max(curve_len_px * (1.0f - offset_frac), BEND_MIN_SPAN);
    layout.unit_px = available_px / std::max(total_width_lay, BEND_MIN_SPAN);
    layout.stretch_x = layout.unit_px / layout.glyph_px;
  }
  else {
    layout.unit_px = layout.glyph_px;
    layout.stretch_x = 1.0f;
  }
  return layout;
}

/**
 * The per-glyph placement: the curve point and tangent at each glyph's mapped advance, its
 * line offset and the baseline shift leaving the curve along the tangent's left-hand normal
 * (the text's up). Glyphs past the curve's end are dropped in the Clip mode and pile up at the
 * end point in the Clamp one.
 */
static void bend_placements_compute(const VFontRenderText &text,
                                    const VFontRenderBendParams &bend,
                                    const BendLayout &layout,
                                    const bke::CurvePatchSpline2D &spline,
                                    const Span<float> line_min,
                                    const Span<float> line_max,
                                    MutableSpan<BendGlyphPlacement> r_placements)
{
  /* The cumulative advance of each drawn glyph, in layout units: the width of every earlier
   * line, plus the glyph's offset within its own line. */
  float base = 0.0f;
  short current_line = -1;
  for (const int a : IndexRange(text.text_len)) {
    const CharTrans &ct = text.chartrans[a];
    if (vfont_render_char_is_truncated(text.cu, ct)) {
      break;
    }
    if (vfont_render_char_is_blank(text.text[a])) {
      continue;
    }
    if (ct.linenr != current_line) {
      if (current_line >= 0) {
        base += std::max(line_max[current_line] - line_min[current_line], 0.0f);
      }
      current_line = ct.linenr;
    }
    BendGlyphPlacement &placement = r_placements[a];
    placement.s_lay = base + (ct.offset.x - line_min[ct.linenr]);

    const float s_px = layout.offset_px + layout.unit_px * placement.s_lay;
    if (bend.mode == VFontRenderBendMode::Clip && s_px > layout.curve_len_px) {
      continue;
    }
    /* Turn the glyph by the tangent at its ADVANCE MIDDLE and place its centre on that arc
     * position, not its left edge: a rigid tile rotated at the origin gaps on the outside of a
     * curve and collides on the inside, and the error grows with the glyph width. The span the
     * glyph owns along the curve is `unit_px` (the stretched advance in Fit, the glyph's own scale
     * in Clip/Clamp), so half of it is what the origin sits behind the centre. */
    const float advance_lay = bend_glyph_advance(text, a);
    const float s_mid_px = math::clamp(
        layout.offset_px + layout.unit_px * (placement.s_lay + advance_lay * 0.5f),
        0.0f,
        layout.curve_len_px);
    const BendFrame frame = bend_frame_at(spline, s_mid_px, layout.side);
    placement.angle = std::atan2(frame.tangent_uv.y, frame.tangent_uv.x);
    const float lateral_px = layout.glyph_px * ct.offset.y + layout.baseline_px;
    const float span_px = layout.unit_px * advance_lay;
    placement.origin_px = frame.point_px - frame.tangent_uv * (span_px * 0.5f) +
                          frame.normal_uv * lateral_px;
    placement.skip = false;
  }
}

/**
 * Fill a solid premultiplied-white convex quad into the float buffer, from its four pixel-space
 * corners in winding order. The bent underline band is the only user: a wide, near-rectangular
 * strip whose hard edge hides at the decal's scale, so no antialiasing is spent on it.
 */
static void bend_fill_quad(float *buffer, const int side, const float2 corners[4])
{
  float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
  float area2 = 0.0f;
  for (const int i : IndexRange(4)) {
    const float2 &corner = corners[i];
    const float2 &next = corners[(i + 1) % 4];
    min_x = std::min(min_x, corner.x);
    min_y = std::min(min_y, corner.y);
    max_x = std::max(max_x, corner.x);
    max_y = std::max(max_y, corner.y);
    area2 += corner.x * next.y - next.x * corner.y;
  }
  if (area2 == 0.0f) {
    /* A degenerate quad encloses no area. Without this guard every sample would test as inside
     * (all edge crosses are zero), so it would fill a line's worth of pixels. */
    return;
  }
  /* Normalize the winding to counter-clockwise: then the interior is on the positive side of
   * every edge, and one half-plane test covers both directions. */
  float2 ordered[4];
  const bool counter_clockwise = area2 > 0.0f;
  for (const int i : IndexRange(4)) {
    ordered[i] = counter_clockwise ? corners[i] : corners[3 - i];
  }
  const int x0 = math::clamp(int(math::floor(min_x)), 0, side);
  const int x1 = math::clamp(int(math::ceil(max_x)) + 1, 0, side);
  const int y0 = math::clamp(int(math::floor(min_y)), 0, side);
  const int y1 = math::clamp(int(math::ceil(max_y)) + 1, 0, side);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  for (const int y : IndexRange(y0, y1 - y0)) {
    float *pixel = buffer + (size_t(y) * side + x0) * 4;
    for (const int x : IndexRange(x0, x1 - x0)) {
      const float2 point(float(x) + 0.5f, float(y) + 0.5f);
      bool inside = true;
      for (const int e : IndexRange(4)) {
        const float2 &edge_p = ordered[e];
        const float2 edge_d = ordered[(e + 1) % 4] - edge_p;
        const float2 to_point = point - edge_p;
        const float cross = edge_d.x * to_point.y - edge_d.y * to_point.x;
        if (cross < 0.0f) {
          inside = false;
          break;
        }
      }
      if (inside) {
        pixel[0] = 1.0f;
        pixel[1] = 1.0f;
        pixel[2] = 1.0f;
        pixel[3] = 1.0f;
      }
      pixel += 4;
    }
  }
}

/**
 * Composite \a src into the premultiplied \a dst buffer: the tile pixel that carries the glyph's
 * layout origin sits at \a origin_in_tile, and that origin lands at \a origin_px in \a dst,
 * widened by \a stretch_x along the tile's x axis and rotated by \a angle. Every destination pixel
 * in the rotated bounds back-projects to the tile, samples it bilinearly and blends over,
 * premultiplied.
 */
static void composite_glyph_rotated(const float *src_buffer,
                                    const int src_w,
                                    const int src_h,
                                    const float2 origin_in_tile,
                                    const float2 origin_px,
                                    const float angle,
                                    const float stretch_x,
                                    const float shear,
                                    const int dst_side,
                                    float *dst_buffer)
{
  const float cos_a = math::cos(angle);
  const float sin_a = math::sin(angle);
  const float2 corners[4] = {float2(0.0f, 0.0f),
                             float2(float(src_w), 0.0f),
                             float2(0.0f, float(src_h)),
                             float2(float(src_w), float(src_h))};
  float min_x = FLT_MAX, min_y = FLT_MAX, max_x = -FLT_MAX, max_y = -FLT_MAX;
  for (const float2 &corner : corners) {
    const float2 local = float2(corner.x - origin_in_tile.x, corner.y - origin_in_tile.y) *
                         float2(stretch_x, 1.0f);
    const float2 target = origin_px + float2(local.x * cos_a - local.y * sin_a,
                                             local.x * sin_a + local.y * cos_a);
    min_x = std::min(min_x, target.x);
    min_y = std::min(min_y, target.y);
    max_x = std::max(max_x, target.x);
    max_y = std::max(max_y, target.y);
  }
  const float shear_reach = fabsf(shear) * float(src_h);
  min_x -= shear_reach;
  max_x += shear_reach;
  const int x0 = math::clamp(int(math::floor(min_x)), 0, dst_side);
  const int x1 = math::clamp(int(math::ceil(max_x)) + 1, 0, dst_side);
  const int y0 = math::clamp(int(math::floor(min_y)), 0, dst_side);
  const int y1 = math::clamp(int(math::ceil(max_y)) + 1, 0, dst_side);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  const auto sample_clamped = [&](const int cx, const int cy) -> const float * {
    const int x = math::clamp(cx, 0, src_w - 1);
    const int y = math::clamp(cy, 0, src_h - 1);
    return src_buffer + (size_t(y) * src_w + x) * 4;
  };
  for (const int y : IndexRange(y0, y1 - y0)) {
    float *dst = dst_buffer + (size_t(y) * dst_side + x0) * 4;
    for (const int x : IndexRange(x0, x1 - x0)) {
      /* The pixel center back-projects through the inverse rotation to the tile. */
      const float2 delta(float(x) + 0.5f - origin_px.x, float(y) + 0.5f - origin_px.y);
      const float2 local(delta.x * cos_a + delta.y * sin_a, -delta.x * sin_a + delta.y * cos_a);
      /* The widening is undone on the way back to the tile. */
      float2 tile_p = float2(local.x / stretch_x, local.y) + origin_in_tile;
      /* The 3D text shears each glyph by its layout Y before the glyph is laid on the curve. The
       * tile's Y is that layout Y in pixels, so the shift is `shear * (tile_y - origin_y)`. */
      tile_p.x += shear * (tile_p.y - origin_in_tile.y);
      const float fx = tile_p.x - 0.5f;
      const float fy = tile_p.y - 0.5f;
      /* Outside the tile is empty, never the edge texel repeated: clamping there smeared a clipped
       * glyph's border across the composite as a hard bar. */
      if (fx < 0.0f || fy < 0.0f || fx >= float(src_w - 1) || fy >= float(src_h - 1)) {
        dst += 4;
        continue;
      }
      const int ix = int(math::floor(fx));
      const int iy = int(math::floor(fy));
      const float tx = fx - float(ix);
      const float ty = fy - float(iy);
      const float *p00 = sample_clamped(ix, iy);
      const float *p10 = sample_clamped(ix + 1, iy);
      const float *p01 = sample_clamped(ix, iy + 1);
      const float *p11 = sample_clamped(ix + 1, iy + 1);
      /* The four channels bilinear in one pass: the tile is premultiplied white, so every
       * channel is its alpha times one, and the blend is a plain premultiplied over. */
      float out[4];
      for (const int c : IndexRange(4)) {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bottom = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = top + (bottom - top) * ty;
      }
      const float alpha = out[3];
      if (alpha > 0.0f) {
        const float keep = 1.0f - alpha;
        dst[0] = out[0] + dst[0] * keep;
        dst[1] = out[1] + dst[1] * keep;
        dst[2] = out[2] + dst[2] * keep;
        dst[3] = alpha + dst[3] * keep;
      }
      dst += 4;
    }
  }
}

/**
 * Draw every glyph of the bent layout into \a draw_ibuf, white so that its alpha is the
 * coverage. Each glyph renders unrotated into its own tile through the BLF buffer path -- the
 * one thing BLF can do into a memory buffer -- and composites rotated at its placement, so the
 * per-character rotation the straight path cannot apply is exactly what this one exists for.
 *
 * One tile buffer is reused for every glyph of the pass. The composite is deliberately
 * sequential: two glyphs of a bent text can overlap, and a parallel pass would race on `dst`.
 */
static void glyphs_draw_bent(const VFontRenderText &text,
                             const Span<BendGlyphPlacement> placements,
                             Vector<RenderFont> &fonts,
                             ImBuf &draw_ibuf,
                             const BendLayout &layout,
                             const bke::CurvePatchSpline2D &spline)
{
  const CharInfo *charinfo = vfont_render_charinfo(text.cu);
  const bool xtrax_contract = text.cu.spacing < 1.0f;
  const float xtrax = xtrax_contract ? (text.cu.spacing - 1.0f) : (0.5f * text.cu.spacing - 0.5f);
  const float shear = text.cu.shear;

  float *dst_buffer = draw_ibuf.float_data_for_write();
  const int side = draw_ibuf.x;
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  for (const RenderFont &font : fonts) {
    if (font.blf_id != -1 && font.px_per_unit_per_size > 0.0f) {
      BLF_buffer_col(font.blf_id, white);
    }
  }

  Array<float> tile_buffer;
  char ch_utf8[BLI_UTF8_MAX + 1];
  for (const int a : IndexRange(text.text_len)) {
    const CharTrans &ct = text.chartrans[a];
    if (vfont_render_char_is_truncated(text.cu, ct)) {
      break;
    }
    const BendGlyphPlacement &placement = placements[a];
    if (placement.skip) {
      continue;
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
    /* The small-caps scale rides on the BLF size, exactly as the straight path sets it. */
    const float glyph_px_per_unit = layout.glyph_px *
                                    (ct.is_smallcaps ? text.cu.smallcaps_scale : 1.0f);
    vfont_render_font_size_set(*font, glyph_px_per_unit);
    const size_t ch_utf8_len = BLI_str_utf8_from_unicode(uint(charcode), ch_utf8, sizeof(ch_utf8));
    if (ch_utf8_len == 0) {
      continue;
    }

    /* The tile the glyph draws into, unrotated: the ink plus a margin that keeps the
     * antialiased edge inside, with the layout origin at the margin. */
    const VChar *che = vfont_render_char_find(vfont->data, charcode);
    rctf bounds;
    if (che != nullptr &&
        (che->bounds.xmin < che->bounds.xmax || che->bounds.ymin < che->bounds.ymax))
    {
      bounds = che->bounds;
    }
    else {
      /* No cached bounds: the em box, so the glyph cannot collapse the tile and BLF's own
       * fallback drawing cannot clip. */
      bounds.xmin = 0.0f;
      bounds.xmax = (font->ascent + font->descent) * 0.6f;
      bounds.ymin = -font->descent;
      bounds.ymax = font->ascent;
    }
    const float ink_x0 = bounds.xmin * glyph_px_per_unit;
    const float ink_x1 = bounds.xmax * glyph_px_per_unit;
    const float ink_y0 = bounds.ymin * glyph_px_per_unit;
    const float ink_y1 = bounds.ymax * glyph_px_per_unit;
    /* A proportional margin: BLF's hinted ink can reach past `che->bounds` by a few pixels of a
     * large glyph (an italic's overhang, a fraction of a stem), and a tile that is one texel too
     * small smears the clipped edge across the whole composite. 8% of the glyph's pixel size keeps
     * the antialiased overhang inside at every size; the 2 px floor covers tiny glyphs. */
    const float margin_px = std::clamp(0.06f * glyph_px_per_unit,
                                       BEND_TILE_MARGIN_PX,
                                       64.0f);
    const int margin = int(math::ceil(margin_px));
    const int tile_w = int(math::ceil(ink_x1 - ink_x0)) + 2 * margin;
    const int tile_h = int(math::ceil(ink_y1 - ink_y0)) + 2 * margin;
    if (tile_w < 2 || tile_h < 2) {
      continue;
    }
    const int64_t tile_size = int64_t(tile_w) * tile_h * 4;
    if (tile_size > tile_buffer.size()) {
      tile_buffer.reinitialize(tile_size);
    }
    /* BLF blends into the given buffer, so the used area starts transparent every glyph. The
     * buffer may be larger than this glyph's tile; only the strip the BLF view covers is read
     * back by the composite. */
    memset(tile_buffer.data(), 0, sizeof(float[4]) * size_t(tile_w) * tile_h);
    const float2 origin_in_tile(float(margin) - ink_x0, float(margin) - ink_y0);
    BLF_buffer(font->blf_id, tile_buffer.data(), nullptr, tile_w, tile_h, 4, nullptr);
    BLF_position(font->blf_id, origin_in_tile.x, origin_in_tile.y, 0.0f);
    BLF_draw_buffer(font->blf_id, ch_utf8, ch_utf8_len);
    composite_glyph_rotated(tile_buffer.data(),
                            tile_w,
                            tile_h,
                            origin_in_tile,
                            placement.origin_px,
                            placement.angle,
                            layout.stretch_x,
                            shear,
                            side,
                            dst_buffer);

    if (info.flag & CU_CHINFO_UNDERLINE) {
      /* The per-character underline band of the straight path, bent: its two arc ends map to
       * the curve, the band edges leave it along the normals there. */
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
      const float band_top_lay = ct.offset.y + text.cu.ulpos - 0.05f;
      const float band_bot_lay = band_top_lay - text.cu.ulheight;
      const float band_top_px = band_top_lay * layout.glyph_px + layout.baseline_px;
      const float band_bot_px = band_bot_lay * layout.glyph_px + layout.baseline_px;
      const float s0 = math::clamp(
          layout.offset_px + layout.unit_px * placement.s_lay, 0.0f, layout.curve_len_px);
      const float s1 = math::clamp(layout.offset_px +
                                       layout.unit_px * (placement.s_lay + twidth + overlap),
                                   0.0f,
                                   layout.curve_len_px);
      const auto bend_band_point = [&](const float arc_px, const float band_px) {
        const BendFrame frame = bend_frame_at(spline, arc_px, layout.side);
        return frame.point_px + frame.normal_uv * band_px;
      };
      const float2 corners[4] = {
          bend_band_point(s0, band_top_px),
          bend_band_point(s1, band_top_px),
          bend_band_point(s1, band_bot_px),
          bend_band_point(s0, band_bot_px),
      };
      bend_fill_quad(dst_buffer, side, corners);
    }
  }
  /* The glyph tile buffer lives on the stack and dies here: detach it from the shared BLF state,
   * exactly as the straight path's font unload does, so no later BLF consumer can read it. */
  for (const RenderFont &font : fonts) {
    if (font.blf_id != -1) {
      BLF_buffer(font.blf_id, nullptr, nullptr, 0, 0, 1, nullptr);
    }
  }
}

bool vfont_render_bend_try(const VFontRenderText &text,
                           Vector<RenderFont> &fonts,
                           const VFontRenderBendParams &bend,
                           const int side,
                           ImBuf **r_ibuf)
{
  *r_ibuf = nullptr;
  bke::CurvePatchSpline2D spline;
  if (!bend_spline_build(bend, spline)) {
    return false;
  }
  const float curve_len_px = spline.total_length() * float(side);
  if (curve_len_px <= BEND_MIN_CURVE_PX) {
    return false;
  }

  Array<float> line_min(text.text_len);
  Array<float> line_max(text.text_len);
  float total_width = 0.0f;
  if (!bend_layout_measure(
          text, line_min.as_mutable_span(), line_max.as_mutable_span(), total_width))
  {
    return false;
  }
  const BendLayout layout = bend_layout_compute(
      bend, curve_len_px, float(side), text.cu.fsize, total_width);
  Array<BendGlyphPlacement> placements(text.text_len);
  bend_placements_compute(text,
                          bend,
                          layout,
                          spline,
                          line_min.as_span(),
                          line_max.as_span(),
                          placements.as_mutable_span());

  ImBuf *ibuf = IMB_allocImBuf(uint(side), uint(side), ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    /* The bend applies but its raster cannot be allocated: report it as taken, leaving the
     * buffer null, so the caller fails the whole render like the straight path does. */
    return true;
  }
  glyphs_draw_bent(text, placements.as_span(), fonts, *ibuf, layout, spline);
  *r_ibuf = ibuf;
  return true;
}

}  // namespace blender
