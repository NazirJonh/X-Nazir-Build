/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_vfont_render.hh"

#include <algorithm>
#include <cstring>

#include "BLI_index_range.hh"
#include "BLI_path_utils.hh"
#include "BLI_span.hh"
#include "BLI_string_utf8.h"

#include "DNA_curve_types.h"
#include "DNA_object_types.h"
#include "DNA_vfont_types.h"

#include "BKE_gtest_base.hh"
#include "BKE_global.hh"
#include "BKE_curve.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"
#include "BKE_vfont.hh"

#include "BLF_api.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include "testing/testing.h"

namespace blender::bke::tests {

namespace {

struct RasterInkBounds {
  int xmin = 0;
  int ymin = 0;
  int xmax = -1;
  int ymax = -1;

  bool is_valid() const
  {
    return xmax >= xmin && ymax >= ymin;
  }
  int width() const
  {
    return xmax - xmin + 1;
  }
  int height() const
  {
    return ymax - ymin + 1;
  }
};

RasterInkBounds raster_ink_bounds(const ImBuf &ibuf)
{
  const float *data = ibuf.float_data();
  RasterInkBounds bounds;
  bounds.xmin = ibuf.x;
  bounds.ymin = ibuf.y;
  for (const int y : IndexRange(ibuf.y)) {
    for (const int x : IndexRange(ibuf.x)) {
      if (data[(size_t(y) * ibuf.x + x) * 4 + 3] > 0.01f) {
        bounds.xmin = std::min(bounds.xmin, x);
        bounds.xmax = std::max(bounds.xmax, x);
        bounds.ymin = std::min(bounds.ymin, y);
        bounds.ymax = std::max(bounds.ymax, y);
      }
    }
  }
  return bounds;
}

bool raster_row_is_solid(const ImBuf &ibuf, const int y, const int x0, const int x1)
{
  const float *data = ibuf.float_data();
  for (const int x : IndexRange(x0, x1 - x0 + 1)) {
    if (data[(size_t(y) * ibuf.x + x) * 4 + 3] < 0.99f) {
      return false;
    }
  }
  return true;
}

/* Number of covered rows in a column, 0 when the column has no ink. */
int raster_column_ink_height(const ImBuf &ibuf, const int x)
{
  const float *data = ibuf.float_data();
  int ymin = ibuf.y;
  int ymax = -1;
  for (const int y : IndexRange(ibuf.y)) {
    if (data[(size_t(y) * ibuf.x + x) * 4 + 3] > 0.01f) {
      ymin = std::min(ymin, y);
      ymax = std::max(ymax, y);
    }
  }
  return (ymax >= ymin) ? (ymax - ymin + 1) : 0;
}

/* The tallest ink column run between the x bounds. */
int raster_max_column_ink_height(const ImBuf &ibuf, const int x0, const int x1)
{
  int height = 0;
  for (const int x : IndexRange(x0, x1 - x0 + 1)) {
    height = std::max(height, raster_column_ink_height(ibuf, x));
  }
  return height;
}

}  // namespace

/**
 * The Ahem test font renders every character as the same solid block (an exact fraction of
 * the em box), which keeps the expected ink bounds simple and deterministic.
 */
class VFontRenderTest : public BlenderGTestBase {
 public:
  static void SetUpTestSuite()
  {
    BlenderGTestBase::SetUpTestSuite();
    BLF_init();
  }

  void SetUp() override
  {
    bmain_ = BKE_main_new();
    G_MAIN = bmain_;

    char filepath[FILE_MAX];
    BLI_path_join(filepath,
                  sizeof(filepath),
                  blender::tests::flags_test_asset_dir().c_str(),
                  "blenfont",
                  "Ahem.ttf");
    VFont *vfont = BKE_vfont_load(bmain_, filepath);
    ASSERT_NE(vfont, nullptr);

    object_ = BKE_object_add_only_object(bmain_, OB_FONT, "Text");
    /* The ID's init fills the defaults the layout reads (spacing, small caps scale, line
     * distance); a bare allocation leaves them zero and no text wraps or scales. The builtin font
     * the font-specific add path needs is not loaded in tests. */
    Curve *curve = BKE_id_new<Curve>(bmain_, "TextCurve");
    curve->ob_type = OB_FONT;
    curve->vfont = vfont;
    id_us_plus(&vfont->id);
    object_->data = &curve->id;
    id_us_plus(&curve->id);

    curve->totbox = 1;
    curve->tb = MEM_new_array<TextBox>(MAXTEXTBOX, "tb");
    curve->tb[0] = {};
    curve_ = curve;
  }

  void TearDown() override
  {
    BKE_main_free(bmain_);
    G_MAIN = nullptr;
    bmain_ = nullptr;
    object_ = nullptr;
    curve_ = nullptr;
  }

  /* Replace the body of the test object, giving every character the same style flags. */
  void set_body(const char *utf8, const eCharInfoFlag char_flags = eCharInfoFlag(0))
  {
    const size_t len_char32 = BLI_strlen_utf8(utf8);
    const size_t len_bytes = strlen(utf8);
    MEM_SAFE_DELETE(curve_->str);
    MEM_SAFE_DELETE(curve_->strinfo);
    curve_->str = MEM_new_array<char>(len_bytes + 1, "str");
    memcpy(curve_->str, utf8, len_bytes + 1);
    curve_->len = int(len_bytes);
    curve_->len_char32 = int(len_char32);
    curve_->strinfo = MEM_new_array<CharInfo>(len_char32 + 1, "strinfo");
    for (const int i : IndexRange(int(len_char32) + 1)) {
      curve_->strinfo[i] = CharInfo{};
      curve_->strinfo[i].flag = char_flags;
    }
  }

  /* Same, with an individual style flag per character (the terminator gets no flags). */
  void set_body(const char *utf8, const blender::Span<eCharInfoFlag> char_flags)
  {
    const size_t len_char32 = BLI_strlen_utf8(utf8);
    const size_t len_bytes = strlen(utf8);
    MEM_SAFE_DELETE(curve_->str);
    MEM_SAFE_DELETE(curve_->strinfo);
    curve_->str = MEM_new_array<char>(len_bytes + 1, "str");
    memcpy(curve_->str, utf8, len_bytes + 1);
    curve_->len = int(len_bytes);
    curve_->len_char32 = int(len_char32);
    curve_->strinfo = MEM_new_array<CharInfo>(len_char32 + 1, "strinfo");
    for (const int i : IndexRange(int(len_char32))) {
      curve_->strinfo[i] = CharInfo{};
      curve_->strinfo[i].flag = char_flags[i];
    }
    curve_->strinfo[int(len_char32)] = CharInfo{};
  }

  ImBuf *render(const int side, const float padding, const VFontRenderFit fit) const
  {
    return BKE_vfont_object_render(*object_, {side, padding, fit});
  }

 protected:
  Main *bmain_ = nullptr;
  Object *object_ = nullptr;
  Curve *curve_ = nullptr;
};

TEST_F(VFontRenderTest, empty_text)
{
  set_body("");
  EXPECT_EQ(render(64, 0.0f, VFontRenderFit::Contain),
            nullptr);
}

TEST_F(VFontRenderTest, not_a_text_object)
{
  set_body("AB");
  Object *empty = BKE_object_add_only_object(bmain_, OB_EMPTY, "Empty");
  EXPECT_EQ(BKE_vfont_object_render(*empty, {64, 0.0f, VFontRenderFit::Contain}), nullptr);
  BKE_id_free(bmain_, empty);
}

TEST_F(VFontRenderTest, contain_fills_and_fits)
{
  set_body("AB");
  ImBuf *ibuf = render(64, 0.0f, VFontRenderFit::Contain);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_EQ(ibuf->x, 64);
  EXPECT_EQ(ibuf->y, 64);

  const RasterInkBounds bounds = raster_ink_bounds(*ibuf);
  ASSERT_TRUE(bounds.is_valid());
  /* The ink stays inside the raster with a margin. */
  EXPECT_GE(bounds.xmin, 1);
  EXPECT_GE(bounds.ymin, 1);
  EXPECT_LE(bounds.xmax, 62);
  EXPECT_LE(bounds.ymax, 62);
  /* The longer side of the ink drives the fit: it fills the square except the margins. */
  EXPECT_GE(bounds.width(), 48);

  IMB_freeImBuf(ibuf);
}

TEST_F(VFontRenderTest, contain_respects_padding)
{
  set_body("AB");
  ImBuf *ibuf = render(64, 0.25f, VFontRenderFit::Contain);
  ASSERT_NE(ibuf, nullptr);

  const RasterInkBounds bounds = raster_ink_bounds(*ibuf);
  ASSERT_TRUE(bounds.is_valid());
  /* A quarter of the side of padding on every side, plus the fit safety margin. */
  EXPECT_GE(bounds.xmin, 17);
  EXPECT_GE(bounds.ymin, 17);
  EXPECT_LE(bounds.xmax, 47);
  EXPECT_LE(bounds.ymax, 47);

  IMB_freeImBuf(ibuf);
}

TEST_F(VFontRenderTest, stretch_fills_both_sides)
{
  set_body("AB");
  ImBuf *ibuf = render(64, 0.0f, VFontRenderFit::Stretch);
  ASSERT_NE(ibuf, nullptr);

  const RasterInkBounds bounds = raster_ink_bounds(*ibuf);
  ASSERT_TRUE(bounds.is_valid());
  EXPECT_GE(bounds.xmin, 1);
  EXPECT_GE(bounds.ymin, 1);
  EXPECT_LE(bounds.xmax, 62);
  EXPECT_LE(bounds.ymax, 62);
  EXPECT_GE(bounds.width(), 48);
  EXPECT_GE(bounds.height(), 48);

  IMB_freeImBuf(ibuf);
}

TEST_F(VFontRenderTest, underline_is_a_solid_band)
{
  set_body("AB", CU_CHINFO_UNDERLINE);
  ImBuf *ibuf = render(64, 0.0f, VFontRenderFit::Contain);
  ASSERT_NE(ibuf, nullptr);

  const RasterInkBounds bounds = raster_ink_bounds(*ibuf);
  ASSERT_TRUE(bounds.is_valid());

  /* The glyph blocks of the two characters leave a gap between them; only the underline
   * covers the full run, as one solid band. */
  int solid_rows = 0;
  for (const int y : IndexRange(ibuf->y)) {
    if (raster_row_is_solid(*ibuf, y, bounds.xmin, bounds.xmax)) {
      solid_rows++;
    }
  }
  EXPECT_GE(solid_rows, 1);

  IMB_freeImBuf(ibuf);
}

TEST_F(VFontRenderTest, text_box_wraps_to_two_lines)
{
  set_body("AA BB");
  curve_->tb[0].w = 3.0f;

  ImBuf *wrapped = render(64, 0.0f, VFontRenderFit::Contain);
  ASSERT_NE(wrapped, nullptr);

  set_body("AB");
  ImBuf *single = render(64, 0.0f, VFontRenderFit::Contain);
  ASSERT_NE(single, nullptr);

  /* The wrap puts the second word on a line of its own: the ink gets much taller. */
  EXPECT_GT(raster_ink_bounds(*wrapped).height(),
            raster_ink_bounds(*single).height() * 1.3f);

  IMB_freeImBuf(wrapped);
  IMB_freeImBuf(single);
}

TEST_F(VFontRenderTest, smallcaps_scales_down)
{
  /* A mixed-case word: the small cap "a" renders as the uppercase form at a reduced scale
   * next to the full-size "B". After the fit the relative heights of the two glyphs differ
   * by the small caps scale. */
  const eCharInfoFlag flags[] = {CU_CHINFO_SMALLCAPS, eCharInfoFlag(0)};
  set_body("aB", blender::Span<eCharInfoFlag>(flags, 2));
  ImBuf *smallcaps = render(128, 0.0f, VFontRenderFit::Contain);
  ASSERT_NE(smallcaps, nullptr);

  const RasterInkBounds bounds = raster_ink_bounds(*smallcaps);
  ASSERT_TRUE(bounds.is_valid());

  /* The Ahem blocks touch and the small cap is narrower, so the border is not the middle of the
   * ink. The outermost columns belong to one glyph each: the first to the small cap, the last to
   * the full-size one. */
  const float small_height = float(raster_column_ink_height(*smallcaps, bounds.xmin + 1));
  const float full_height = float(raster_column_ink_height(*smallcaps, bounds.xmax - 1));
  EXPECT_GT(full_height, 0.0f);
  const float ratio = small_height / full_height;
  EXPECT_GT(ratio, 0.6f);
  EXPECT_LT(ratio, 0.85f);

  IMB_freeImBuf(smallcaps);
}

TEST_F(VFontRenderTest, edit_mode_reads_the_edit_buffer)
{
  /* The saved text and its styles are shorter than the live edit buffer: the raster has to take
   * the characters and the styles from the buffer the layout used, not from `Curve::strinfo`. */
  set_body("AB");
  char32_t textbuf[] = {U'A', U'B', U'A', U'B'};
  CharInfo textbufinfo[4];
  for (CharInfo &info : textbufinfo) {
    info = CharInfo{};
    info.flag = CU_CHINFO_UNDERLINE;
  }
  EditFont ef = {};
  ef.textbuf = textbuf;
  ef.textbufinfo = textbufinfo;
  ef.len = 4;
  curve_->editfont = &ef;

  ImBuf *ibuf = render(64, 0.0f, VFontRenderFit::Contain);
  curve_->editfont = nullptr;
  ASSERT_NE(ibuf, nullptr);

  const RasterInkBounds bounds = raster_ink_bounds(*ibuf);
  ASSERT_TRUE(bounds.is_valid());
  int solid_rows = 0;
  for (const int y : IndexRange(ibuf->y)) {
    if (raster_row_is_solid(*ibuf, y, bounds.xmin, bounds.xmax)) {
      solid_rows++;
    }
  }
  EXPECT_GE(solid_rows, 1) << "the underline of the live styles is drawn";

  IMB_freeImBuf(ibuf);
}

TEST_F(VFontRenderTest, repeated_renders_release_the_blf_fonts)
{
  set_body("AB");
  /* The BLF font map has a fixed number of slots: a raster that leaves its unique fonts loaded
   * runs out of them after a few dozen updates. */
  for (int i = 0; i < 80; i++) {
    ImBuf *ibuf = render(32, 0.0f, VFontRenderFit::Contain);
    ASSERT_NE(ibuf, nullptr) << "render " << i;
    IMB_freeImBuf(ibuf);
  }
}

}  // namespace blender::bke::tests
