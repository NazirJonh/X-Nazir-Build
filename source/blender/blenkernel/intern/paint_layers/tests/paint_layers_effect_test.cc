/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Neighborhood effects (Blur, Sharpen, High Pass, Edge Detect, Emboss, Erode / Dilate): the effect
 * cache computes them for the CPU composite and for the bake, and a row that carries one is baked
 * for the live graph, which samples the bake instead. The tests read the composite and the bake
 * maps, so both sides of that hand-over are covered, along with the invariants of the kernels on
 * flat and edged inputs and the cache's keying, eviction and describe-only passes.
 *
 * The fixture lives in `paint_layers_generate_test_fixture.hh`.
 */

#include <algorithm>
#include <cmath>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_math_color.h"

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_paint_layers_composite.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_set.hh"

#include "IMB_imbuf.hh"

#include "DNA_windowmanager_types.h"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

namespace {

constexpr eMaterialPaintChannel kColor = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

}  // namespace

class PaintLayersEffectTest : public PaintLayersGenerateTest {
 public:
  /** The budget the process started with, so a test that shrinks it can put it back. */
  static size_t default_budget()
  {
    /* Read on the first SetUp, before any test has touched the budget. */
    static const size_t budget = paint_layer_effect_cache_budget();
    return budget;
  }

  /** The effect cache is process-global: a test starts with none of an earlier one's entries, and
   * with the budget an earlier one may have shrunk for an eviction check put back. */
  void SetUp() override
  {
    PaintLayersGenerateTest::SetUp();
    /* A dummy window manager, the way the bake tests make one: the bake path notifies image
     * editors by walking bmain->wm, which a bare test Main has none of. */
    wmWindowManager *wm = BKE_id_new<wmWindowManager>(bmain, "WMdummy");
    wm->runtime = MEM_new<bke::WindowManagerRuntime>(__func__);
    paint_layer_effect_cache_set_budget(default_budget());
    BKE_paint_layers_effect_cache_free_all();
  }

  void TearDown() override
  {
    paint_layer_effect_cache_set_budget(default_budget());
    BKE_paint_layers_effect_cache_free_all();
    PaintLayersGenerateTest::TearDown();
  }

  /** Nothing is active, so no row is held live for editing (the bake would be deferred). */
  void clear_active() const
  {
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  }

  /** An opaque 8x8 byte image, grey: \a left on the left half, \a right on the right one. The
   * right half's alpha is \a alpha_right, so a transparent half can hand a kernel a real border. */
  Image *add_shade_image(const char *name,
                         const uchar left,
                         const uchar right,
                         const bool split_rows = false,
                         const uchar alpha_right = 255)
  {
    const int size = 8;
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        /* With \a split_rows the edge runs along x: the top half is \a left, the bottom \a right. */
        const bool right_half = (split_rows ? y : x) >= size / 2;
        const uchar value = right_half ? right : left;
        uchar *p = pixels + (int64_t(y) * size + x) * 4;
        p[0] = value;
        p[1] = value;
        p[2] = value;
        p[3] = right_half ? alpha_right : 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  MaterialPaintLayer *add_effect(MaterialPaintLayer &row, const eMaterialPaintLayerFilter filter)
  {
    MaterialPaintLayer *effect = BKE_paint_layers_filter_add(*ma, &row, filter);
    EXPECT_NE(effect, nullptr);
    EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_EFFECT) << "a CPU-computed effect is a correction";
    return effect;
  }

  /** The composite of Base Color as straight scene-linear RGBA at the stack's dimensions. */
  bool render_channel(Vector<float> &r_pixels, int &r_width, int &r_height)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    if (!BKE_paint_layers_composite_image_layers(*ma, kColor, layers)) {
      return false;
    }
    if (!BKE_paint_material_composite_stack_dimensions(layers, r_width, r_height) ||
        r_width <= 0 || r_height <= 0)
    {
      return false;
    }
    r_pixels.resize(int64_t(r_width) * r_height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(kColor, bottom);
    return BKE_paint_material_composite_eval_images_linear(
        layers,
        PaintMaterialCompositeLinearTarget::frame(r_pixels, r_width),
        nullptr,
        nullptr,
        bottom);
  }

  static float linear_pixel(
      const Vector<float> &pixels, const int width, const int x, const int y)
  {
    return pixels[(int64_t(y) * width + x) * 4];
  }

  /** The red byte of a bake map, which encodes the channel's own colorspace like the composite. */
  static int bake_byte_pixel(Image &image, const int x, const int y)
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
    const int value = ibuf->byte_data()[(int64_t(y) * ibuf->x + x) * 4];
    BKE_image_release_ibuf(&image, ibuf, lock);
    return value;
  }

  /** The composite of Base Color as byte RGBA in the channel's colorspace, like a bake map is. */
  bool render_channel_bytes(ImBuf &dst)
  {
    return BKE_paint_layers_composite_channel(*ma, kColor, dst);
  }

  /** Bake a row carrying \a filter, with \a setup setting the parameters the kind needs, and check
   * every byte of the bake against the composite. */
  void expect_bake_matches_composite(const eMaterialPaintLayerFilter filter,
                                     void (*setup)(MaterialPaintLayer &) = nullptr)
  {
    MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
    MaterialPaintLayer *effect = add_effect(*row, filter);
    ASSERT_NE(effect, nullptr);
    if (setup != nullptr) {
      setup(*effect);
    }
    clear_active();

    /* The effect alone is no reason for the planner until the row needs it: a row that carries a
     * CPU-computed effect is a candidate and due whatever its mode is. */
    bool changed = false;
    ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
    ASSERT_NE(row->bake, nullptr);
    EXPECT_NE(row->bake->images[kColor], nullptr);
    EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
    EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));

    /* The bake and the composite run the same description, so the effect lands identically. Both
     * write bytes in the channel's colorspace, the one form both sides share. */
    ImBuf *composite = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
    ASSERT_NE(composite, nullptr);
    ASSERT_TRUE(render_channel_bytes(*composite));
    Image *baked = row->bake->images[kColor];
    for (const int y : IndexRange(8)) {
      for (const int x : IndexRange(8)) {
        EXPECT_NEAR(bake_byte_pixel(*baked, x, y),
                    int(composite->byte_data()[(int64_t(y) * 8 + x) * 4]),
                    2)
            << "the bake and the composite agree, at " << x << "," << y;
      }
    }
    IMB_freeImBuf(composite);

    /* A second pass has nothing left to do. */
    EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  }
};

TEST_F(PaintLayersEffectTest, the_blur_of_a_solid_row_is_the_solid)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* The composite is scene linear, so the comparison is against the row without the effect: the
   * encoding drops out and a flat input blurring to itself reads exactly. */
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "a flat input blurs to itself, texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, a_blur_softens_a_hard_edge_and_keeps_the_flat_runs)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  /* The clamped window of a radius-2 box over the 255/0 edge is five texels wide: the two columns
   * beside the edge carry three-and-two and two-and-three whites over five, exactly 0.6 and 0.4.
   * The source is pure black and white, so the linear values are exact. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 0.6f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 0.4f, 1e-3f);
}

TEST_F(PaintLayersEffectTest, a_blur_of_a_horizontal_edge_reads_the_original_rows_not_blurred_ones)
{
  /* The vertical pass writes rows while its window still drops rows above them: a window that
   * subtracted what the pass already overwrote would drift away from the exact 0.6/0.4 beside the
   * edge and from the flat runs further on. A vertical edge cannot show it, the columns being flat
   * in y. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0, true));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 0), 1.0f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 1), 1.0f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 2), 0.8f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 3), 0.6f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 0.4f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 5), 0.2f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 6), 0.0f, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 7), 0.0f, 1e-3f);
}

TEST_F(PaintLayersEffectTest, a_sharpen_of_a_solid_row_is_the_solid)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *sharpen = add_effect(*row, MA_PAINT_LAYER_FILTER_SHARPEN);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 3));
  ASSERT_TRUE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, 1.0f));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* The composite is scene linear, so the comparison is against the row without the effect: a flat
   * input's detail is zero everywhere, and the unsharp mask adds none of it. */
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "a flat input sharpens to itself, texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, a_zero_amount_sharpen_is_the_identity)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *sharpen = add_effect(*row, MA_PAINT_LAYER_FILTER_SHARPEN);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 1));
  ASSERT_TRUE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, 0.0f));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f)
        << "a zero amount adds none of the detail, texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, a_sharpen_pushes_the_edge_apart_by_the_unsharp_formula)
{
  /* A mid-contrast pair: at amount 0.5 the unsharp formula must land inside [0, 1] on both sides
   * of the edge, or the bottom clamp would eat the expectation. The linear values are measured,
   * not assumed, so the byte encoding drops out. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 150, 110));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));
  const float left = linear_pixel(before, width, 0, 4);
  const float right = linear_pixel(before, width, 7, 4);

  MaterialPaintLayer *sharpen = add_effect(*row, MA_PAINT_LAYER_FILTER_SHARPEN);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 1));
  ASSERT_TRUE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, 0.5f));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* A box blur of radius one at the columns beside the edge reads two of one side and one of the
   * other; the unsharp mask adds half of what the blur removed. */
  const float expected_light = left + 0.5f * (left - (2.0f * left + right) / 3.0f);
  const float expected_dark = right + 0.5f * (right - (left + 2.0f * right) / 3.0f);
  ASSERT_NEAR(expected_light, std::clamp(expected_light, 0.0f, 1.0f), 0.0f)
      << "the light side of the expectation must not lean on a clamp";
  ASSERT_NEAR(expected_dark, std::clamp(expected_dark, 0.0f, 1.0f), 0.0f)
      << "the dark side of the expectation must not lean on a clamp";
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), expected_light, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), expected_dark, 1e-3f);
  /* Far from the edge the blur is the input itself, so nothing is added. */
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), left, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), right, 1e-3f);
}

TEST_F(PaintLayersEffectTest, a_sharpen_never_goes_below_zero)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *sharpen = add_effect(*row, MA_PAINT_LAYER_FILTER_SHARPEN);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 1));
  ASSERT_TRUE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, 2.0f));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* The overshoot past one is left as it is (a map may be HDR), but a negative linear colour would
   * break the blends and the encoding after the effect, so nothing lands below zero. */
  float minimum = 1.0f;
  for (const float value : pixels) {
    minimum = std::min(minimum, value);
  }
  EXPECT_GE(minimum, 0.0f) << "the unsharp mask never writes a negative colour";
}

TEST_F(PaintLayersEffectTest, a_high_pass_of_a_solid_row_is_the_mid_grey)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 160, 160));
  MaterialPaintLayer *high_pass = add_effect(*row, MA_PAINT_LAYER_FILTER_HIGH_PASS);

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* A flat input has no detail to keep: the formula lands on its mid grey exactly, the display
   * encoding's 0.5 read back in scene linear. */
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), srgb_to_linearrgb(0.5f), 2e-3f)
          << "the high pass of a flat input is the mid grey, at " << x << "," << y;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_zero_contrast_high_pass_is_the_mid_grey)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *high_pass = add_effect(*row, MA_PAINT_LAYER_FILTER_HIGH_PASS);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS, 1));
  ASSERT_TRUE(paint_layer_kind_param_set(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST, 0.0f));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), srgb_to_linearrgb(0.5f), 2e-3f)
          << "a zero contrast keeps no detail, at " << x << "," << y;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_high_pass_keeps_the_edge_against_a_flat_field)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 200, 60));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));
  const float left = linear_pixel(before, width, 0, 4);
  const float right = linear_pixel(before, width, 7, 4);

  MaterialPaintLayer *high_pass = add_effect(*row, MA_PAINT_LAYER_FILTER_HIGH_PASS);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS, 1));
  ASSERT_TRUE(
      paint_layer_kind_param_set(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST, 1.0f));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* The encoded mid grey plus the detail the box blur of radius one removed, taken in the display
   * encoding and read back in linear. The measured linear values drop the byte encoding out. */
  const auto expected = [](const float value, const float blurred) {
    return srgb_to_linearrgb(0.5f + linearrgb_to_srgb(value) - linearrgb_to_srgb(blurred));
  };
  const float expected_light = expected(left, (2.0f * left + right) / 3.0f);
  const float expected_dark = expected(right, (left + 2.0f * right) / 3.0f);
  EXPECT_GT(expected_light, srgb_to_linearrgb(0.5f));
  EXPECT_LT(expected_dark, srgb_to_linearrgb(0.5f));
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), expected_light, 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), expected_dark, 1e-3f);
  /* The flats carry no detail: they sit on the mid grey. */
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), srgb_to_linearrgb(0.5f), 1e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), srgb_to_linearrgb(0.5f), 1e-3f);
}

TEST_F(PaintLayersEffectTest, an_edge_detect_of_a_solid_row_is_black)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 160, 160));
  MaterialPaintLayer *edge = add_effect(*row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);

  /* A flat area has no gradient: the Sobel magnitude is zero everywhere, whatever the grey. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), 0.0f, 2e-3f)
          << "the edge detect of a flat input is black, at " << x << "," << y;
    }
  }
}

TEST_F(PaintLayersEffectTest, an_edge_detect_outlines_a_hard_edge_at_full_strength)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *edge = add_effect(*row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);

  /* A full-range step reaches exactly one: the two columns beside the edge carry it, the flats
   * carry nothing. The source is pure black and white, so the linear values are exact. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 5, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, two_edge_detects_of_one_row_key_apart_by_their_marker)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  add_effect(*row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);
  add_effect(*row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* A marker is unique within the material, so two effects of one row key apart even with the same
   * kind and parameters. */
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 2)
      << "two Edge Detects of one row hold two entries, keyed by their markers";
}

TEST_F(PaintLayersEffectTest, an_emboss_of_a_solid_row_is_the_solid)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  /* The kernel's weights sum to one, so a flat colour is its own result. */
  MaterialPaintLayer *emboss = add_effect(*row, MA_PAINT_LAYER_FILTER_EMBOSS);
  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "an emboss of a flat input is the input, texel "
                                             << i;
  }
}

TEST_F(PaintLayersEffectTest, an_emboss_lights_the_white_side_of_a_hard_edge)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *emboss = add_effect(*row, MA_PAINT_LAYER_FILTER_EMBOSS);

  /* Along a vertical edge the kernel reads `-3a + b + 3c` per column (rows are uniform): the white
   * interior stays one, the two edge columns fall below zero and clamp, the black side stays
   * zero. The source is pure black and white, so the linear values are exact. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 1, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 5, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, an_erode_and_a_dilate_leave_a_solid_alone)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  /* The extreme of a square window leaves a flat input alone, both ways round. */
  MaterialPaintLayer *morph = add_effect(*row, MA_PAINT_LAYER_FILTER_ERODE_DILATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS, 2));
  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "a flat input keeps its extremes, texel " << i;
  }

  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_MODE,
                                             PAINT_LAYER_ERODE_DILATE_DILATE));
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "a dilate of a flat input is the input, texel "
                                             << i;
  }
}

TEST_F(PaintLayersEffectTest, an_erode_shrinks_the_bright_side_and_a_dilate_grows_it)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *morph = add_effect(*row, MA_PAINT_LAYER_FILTER_ERODE_DILATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS, 2));

  /* Radius two over the white|black edge: the window spans five texels, so the erode keeps the
   * white only where five whites fit (two columns) and the dilate reaches two columns past the
   * edge. The clamped border windows count the repeats. The source is pure black and white, so
   * the linear values are exact. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 1, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f);

  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_MODE,
                                             PAINT_LAYER_ERODE_DILATE_DILATE));
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 5, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, changing_the_erode_dilate_mode_moves_the_result_and_reuses_the_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *morph = add_effect(*row, MA_PAINT_LAYER_FILTER_ERODE_DILATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS, 2));

  Vector<float> erode_pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(erode_pixels, width, height));
  ASSERT_EQ(paint_layer_effect_cache_entry_count(), 1);
  EXPECT_NEAR(linear_pixel(erode_pixels, width, 0, 4), 1.0f, 1e-3f);
  EXPECT_NEAR(linear_pixel(erode_pixels, width, 2, 4), 0.0f, 1e-3f);

  /* The direction is part of what the effect computes: a different result, keyed by the mode the
   * same way the radius is. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_MODE,
                                             PAINT_LAYER_ERODE_DILATE_DILATE));
  Vector<float> dilate_pixels;
  ASSERT_TRUE(render_channel(dilate_pixels, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the new mode replaced the entry, it did not add one";
  bool differs = false;
  for (int64_t i = 0; i < dilate_pixels.size(); i++) {
    if (std::abs(dilate_pixels[i] - erode_pixels[i]) > 1e-3f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "a dilate of the same radius grows what the erode shrinks";
}

TEST_F(PaintLayersEffectTest, an_erode_dilate_along_rows_mirrors_its_columns_result)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0, true));
  MaterialPaintLayer *morph = add_effect(*row, MA_PAINT_LAYER_FILTER_ERODE_DILATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS, 2));

  /* The edge runs along x, so the vertical pass sees the step the horizontal pass sees in the
   * columns test: the same reads, by row. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 0), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 1), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 2), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 7), 0.0f, 2e-3f);

  ASSERT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_MODE,
                                             PAINT_LAYER_ERODE_DILATE_DILATE));
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 0), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 5), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 6), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 7), 0.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_blur_the_composite_shows)
{
  expect_bake_matches_composite(
      MA_PAINT_LAYER_FILTER_BLUR, [](MaterialPaintLayer &effect) {
        EXPECT_TRUE(paint_layer_kind_param_int_set(
            effect, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
        EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_BLUR_RADIUS, 2));
      });
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_sharpen_the_composite_shows)
{
  expect_bake_matches_composite(
      MA_PAINT_LAYER_FILTER_SHARPEN, [](MaterialPaintLayer &effect) {
        EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_SHARPEN_RADIUS, 2));
      });
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_high_pass_the_composite_shows)
{
  expect_bake_matches_composite(
      MA_PAINT_LAYER_FILTER_HIGH_PASS, [](MaterialPaintLayer &effect) {
        EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_HIGH_PASS_RADIUS, 2));
      });
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_edge_detect_the_composite_shows)
{
  expect_bake_matches_composite(MA_PAINT_LAYER_FILTER_EDGE_DETECT);
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_emboss_the_composite_shows)
{
  expect_bake_matches_composite(MA_PAINT_LAYER_FILTER_EMBOSS);
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_erode_dilate_the_composite_shows)
{
  expect_bake_matches_composite(
      MA_PAINT_LAYER_FILTER_ERODE_DILATE, [](MaterialPaintLayer &effect) {
        EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_ERODE_DILATE_RADIUS, 2));
      });
}

TEST_F(PaintLayersEffectTest, the_combined_preview_falls_back_for_each_added_cpu_computed_effect)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));

  /* Each CPU-computed kind refuses the combined preview by its own reason, without the effect
   * having been computed for the describe-only pass. */
  const std::pair<eMaterialPaintLayerFilter, const char *> reasons[] = {
      {MA_PAINT_LAYER_FILTER_SHARPEN, "a Sharpen correction is read"},
      {MA_PAINT_LAYER_FILTER_HIGH_PASS, "a High Pass correction is read"},
      {MA_PAINT_LAYER_FILTER_EDGE_DETECT, "an Edge Detect correction is read"},
      {MA_PAINT_LAYER_FILTER_EMBOSS, "an Emboss correction is read"},
      {MA_PAINT_LAYER_FILTER_ERODE_DILATE, "an Erode / Dilate correction is read"},
  };
  for (const auto &[filter, reason] : reasons) {
    MaterialPaintLayer *effect = add_effect(*row, filter);
    ASSERT_NE(effect, nullptr);

    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(
        BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
    ASSERT_FALSE(layers.is_empty());
    EXPECT_STREQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), reason);
    EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0)
        << "a describe-only pass computes nothing";

    /* The effect leaves with its reason read, so the next kind is described alone too. */
    EXPECT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  }
}

TEST_F(PaintLayersEffectTest, editing_the_blur_invalidates_the_bake_and_the_cache_key)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  clear_active();
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_EQ(paint_layer_effect_cache_entry_count(), 1);

  /* A wider radius is a different effect: the bake hash moves with the parameter and the effect
   * cache keys on it, so both recompute instead of serving the old result. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row))
      << "an invalid bake stands down, and the live graph leaves the correction out";

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the new parameters replaced the entry, they did not add one";

  clear_active();
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));
}

TEST_F(PaintLayersEffectTest, changing_the_blurs_shape_moves_the_result_and_reuses_the_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));

  Vector<float> box_pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(box_pixels, width, height));
  ASSERT_EQ(paint_layer_effect_cache_entry_count(), 1);
  EXPECT_NEAR(linear_pixel(box_pixels, width, 3, 4), 0.6f, 1e-3f);

  /* The gaussian of the same radius is three narrower box passes, not one wide one: a different
   * result, keyed by the shape the same way the radius is. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_GAUSSIAN));
  Vector<float> gaussian_pixels;
  ASSERT_TRUE(render_channel(gaussian_pixels, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the new shape replaced the entry, it did not add one";
  bool differs = false;
  for (int64_t i = 0; i < gaussian_pixels.size(); i++) {
    if (std::abs(gaussian_pixels[i] - box_pixels[i]) > 1e-3f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "a gaussian of the same radius blurs differently than the box";
}

TEST_F(PaintLayersEffectTest, the_radius_is_clamped_and_the_combined_preview_falls_back)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_RADIUS), 4);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 0));
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_RADIUS), 1)
      << "a radius below one has no kernel";
  EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 100));
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_RADIUS), 64);
  EXPECT_FALSE(paint_layer_kind_param_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2.0f))
      << "the radius is an integer parameter, not a float one";

  /* The kernel clamps the radius to the map it runs over: a radius past the size is the whole map,
   * not a read past its end. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));

  /* The combined preview cannot express the effect: the described stack names a reason, without
   * the effect having been computed for it. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_STREQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false),
               "a Blur correction is read");
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0)
      << "a describe-only pass computes nothing";
}

TEST_F(PaintLayersEffectTest, the_budget_bounds_what_the_cache_keeps)
{
  MaterialPaintLayer *first = add_paint_layer("First", add_shade_image("First", 255, 0));
  MaterialPaintLayer *second = add_paint_layer("Second", add_shade_image("Second", 255, 0));
  MaterialPaintLayer *third = add_paint_layer("Third", add_shade_image("Third", 255, 0));
  add_effect(*first, MA_PAINT_LAYER_FILTER_BLUR);
  add_effect(*second, MA_PAINT_LAYER_FILTER_BLUR);
  add_effect(*third, MA_PAINT_LAYER_FILTER_BLUR);

  /* An 8x8 RGBA float buffer is 1024 bytes. A budget of two holds the two most recent of the
   * three: the third's insert evicts the least recently used first. */
  const size_t saved_budget = paint_layer_effect_cache_budget();
  paint_layer_effect_cache_set_budget(size_t(2) * 1024);
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_LE(paint_layer_effect_cache_entry_count(), 2);
  EXPECT_LE(paint_layer_effect_cache_bytes(), size_t(2) * 1024);

  /* A budget below one entry caches nothing, and the composite still shows the blur: the buffer
   * is served to the caller, only never kept. */
  paint_layer_effect_cache_set_budget(512);
  BKE_paint_layers_effect_cache_free_all();
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0);
  EXPECT_EQ(paint_layer_effect_cache_bytes(), 0);
  paint_layer_effect_cache_set_budget(saved_budget);
}

/* -------------------------------------------------------------------- */
/** \name Noise
 * \{ */

TEST_F(PaintLayersEffectTest, a_noise_at_zero_intensity_changes_nothing)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_set(*noise, PAINT_LAYER_NOISE_INTENSITY, 0.0f));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (const int64_t i : pixels.index_range()) {
    EXPECT_NEAR(pixels[i], before[i], 1e-6f) << "no intensity lays no grain, texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, the_same_seed_lays_the_same_grain_and_the_cache_replaces_its_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 99));

  Vector<float> first;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(first, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1);
  /* The same seed computes the same grain: the second render serves the entry the first filled. */
  Vector<float> again;
  ASSERT_TRUE(render_channel(again, width, height));
  ASSERT_EQ(again.size(), first.size());
  for (const int64_t i : again.index_range()) {
    EXPECT_NEAR(again[i], first[i], 0.0f) << "the grain is stable for the seed, texel " << i;
  }

  /* Another seed is another pattern: the key moves, the entry is replaced, not added to. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 100));
  Vector<float> other;
  ASSERT_TRUE(render_channel(other, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the new seed replaced the entry, it did not add one";
  float moved = 0.0f;
  for (const int64_t i : other.index_range()) {
    moved = std::max(moved, std::abs(other[i] - first[i]));
  }
  EXPECT_GT(moved, 1e-3f) << "a different seed lays a different grain";
}

TEST_F(PaintLayersEffectTest, salt_and_pepper_moves_only_to_black_and_white)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(
      *noise, PAINT_LAYER_NOISE_TYPE, PAINT_LAYER_NOISE_SALT_PEPPER));
  /* The ceiling of the intensity: a quarter of the uniforms read salt, a quarter pepper, and the
   * rest stay. */
  ASSERT_TRUE(paint_layer_kind_param_set(*noise, PAINT_LAYER_NOISE_INTENSITY, 0.5f));

  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  int salted = 0;
  int peppered = 0;
  for (const int64_t i : pixels.index_range()) {
    const bool salt = std::abs(pixels[i]) <= 1e-6f;
    const bool pepper = std::abs(pixels[i] - 1.0f) <= 1e-6f;
    const bool stayed = std::abs(pixels[i] - before[i]) <= 1e-6f;
    EXPECT_TRUE(salt || pepper || stayed)
        << "a salt and pepper grain only makes black or white, texel " << i;
    salted += salt;
    peppered += pepper;
  }
  EXPECT_GT(salted, 0) << "a grey flat row has texels the salt takes";
  EXPECT_GT(peppered, 0) << "a grey flat row has texels the pepper takes";
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_noise_the_composite_shows)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  clear_active();

  /* The bake and the composite run the same description, so the grain lands identically. Both
   * write bytes in the channel's colorspace, the one form both sides share. */
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  ImBuf *composite = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
  ASSERT_NE(composite, nullptr);
  ASSERT_TRUE(render_channel_bytes(*composite));
  Image *baked = row->bake->images[kColor];
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      EXPECT_NEAR(bake_byte_pixel(*baked, x, y),
                  int(composite->byte_data()[(int64_t(y) * 8 + x) * 4]),
                  2)
          << "the bake and the composite agree on the grain, at " << x << "," << y;
    }
  }
  IMB_freeImBuf(composite);
}

TEST_F(PaintLayersEffectTest, a_noise_is_described_only_and_the_combined_preview_falls_back)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  ASSERT_NE(add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE), nullptr);

  /* The shader carries no grain branch, and a describe-only pass computes nothing for it. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_STREQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false),
               "a Noise correction is read");
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0) << "a describe-only pass computes nothing";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Guard
 * \{ */

TEST_F(PaintLayersEffectTest, the_combined_preview_falls_back_before_reading_a_deband)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_DEBAND), nullptr);

  /* The shader cannot repeat the hash of the texel: the described stack names a reason, without
   * the noise having been computed for it. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_NE(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), nullptr);
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0) << "a describe-only pass computes nothing";
}

TEST_F(PaintLayersEffectTest, every_pointwise_kind_has_a_shader_branch_or_refuses_the_preview)
{
  /* The kinds the combined shader has a branch for. A kind added to the registry and missing both
   * here and in the refusals fails below instead of being shaded as a silent no-op. */
  const Set<int> shaded = {MA_PAINT_LAYER_FILTER_INVERT,
                           MA_PAINT_LAYER_FILTER_LEVELS,
                           MA_PAINT_LAYER_FILTER_HSL,
                           MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST,
                           MA_PAINT_LAYER_FILTER_COLOR_RAMP,
                           MA_PAINT_LAYER_FILTER_POSTERIZE,
                           MA_PAINT_LAYER_FILTER_MATH,
                           MA_PAINT_LAYER_FILTER_THRESHOLD,
                           MA_PAINT_LAYER_FILTER_CLAMP,
                           MA_PAINT_LAYER_FILTER_CURVES,
                           MA_PAINT_LAYER_FILTER_COLOR_BALANCE,
                           MA_PAINT_LAYER_FILTER_EXPOSURE,
                           MA_PAINT_LAYER_FILTER_CHANNEL_MIXER};
  constexpr uint8_t both_roles = paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                                 paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER);
  MaterialPaintLayer *previous = nullptr;
  for (const int filter : IndexRange(1, 63)) {
    const PaintLayerKindInfo *info = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER,
                                                           int16_t(filter));
    /* A CPU-computed kind refuses by its flag; a mask-only one is never a correction. */
    if (info == nullptr || (info->flags & PAINT_LAYER_KIND_NEEDS_BAKE) != 0 ||
        (info->allowed_roles & both_roles) != both_roles)
    {
      continue;
    }
    SCOPED_TRACE(info->name);
    clear_active();
    /* The rows of the earlier kinds are switched off, so only this kind's row can refuse. */
    if (previous != nullptr) {
      ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, previous, false));
    }
    MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
    previous = row;
    ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, eMaterialPaintLayerFilter(filter)), nullptr);
    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(
        BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
    ASSERT_FALSE(layers.is_empty());
    const char *reason = nullptr;
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      if (reason == nullptr) {
        reason = BKE_paint_layers_gpu_unsupported_reason(layer, false);
      }
    }
    if (shaded.contains(filter)) {
      EXPECT_EQ(reason, nullptr);
    }
    else {
      EXPECT_NE(reason, nullptr);
    }
    BKE_paint_layers_effect_cache_free_all();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glow, Bevel and Color Match
 * \{ */

TEST_F(PaintLayersEffectTest, a_glow_at_zero_opacity_changes_nothing)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *glow = add_effect(*row, MA_PAINT_LAYER_FILTER_GLOW);
  ASSERT_TRUE(paint_layer_kind_param_set(*glow, PAINT_LAYER_GLOW_OPACITY, 0.0f));
  Vector<float> after;
  ASSERT_TRUE(render_channel(after, width, height));
  for (const int x : IndexRange(width)) {
    EXPECT_NEAR(linear_pixel(after, width, x, 4), linear_pixel(before, width, x, 4), 2e-3f);
  }
}

TEST_F(PaintLayersEffectTest, a_precise_glow_bleeds_the_bright_side_over_the_dark_one)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *glow = add_effect(*row, MA_PAINT_LAYER_FILTER_GLOW);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*glow, PAINT_LAYER_GLOW_MODE, PAINT_LAYER_GLOW_PRECISE));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*glow, PAINT_LAYER_GLOW_WIDTH, 2));

  /* One box pass of reach two over the white left half (columns 0..3): the first dark column sees
   * two of its five window texels lit, the next one a single one, the third none. The glow screens
   * white under the colour, so the dark side takes exactly the window's lit share and the bright
   * side stays white. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 0.4f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 5, 4), 0.2f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, changing_the_glows_color_moves_the_result_and_reuses_the_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *glow = add_effect(*row, MA_PAINT_LAYER_FILTER_GLOW);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*glow, PAINT_LAYER_GLOW_WIDTH, 2));

  Vector<float> white_pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(white_pixels, width, height));
  ASSERT_EQ(paint_layer_effect_cache_entry_count(), 1);

  /* The colour reads as zero in the scalar fold the hash runs the parameters through, so it joins
   * the hash on its own: a different result, keyed by the colour the same way the width is. */
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*glow, PAINT_LAYER_GLOW_COLOR, red));
  Vector<float> red_pixels;
  ASSERT_TRUE(render_channel(red_pixels, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the new colour replaced the entry, it did not add one";
  bool differs = false;
  for (const int64_t i : red_pixels.index_range()) {
    if (std::abs(red_pixels[i] - white_pixels[i]) > 1e-3f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "a red glow tints what a white one bleaches";
}

TEST_F(PaintLayersEffectTest, changing_the_color_match_target_moves_the_result_and_reuses_the_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 128));
  MaterialPaintLayer *match = add_effect(*row, MA_PAINT_LAYER_FILTER_COLOR_MATCH);

  Vector<float> before_pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before_pixels, width, height));
  ASSERT_EQ(paint_layer_effect_cache_entry_count(), 1);
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers));
  const uint64_t before_hash = BKE_paint_material_composite_stack_hash(layers);

  /* The target reads as zero in the scalar fold the hash runs the parameters through, so it joins
   * the hash on its own: a different result, keyed by the colour the same way a Glow's is. The grey
   * half is what shows the shift: pure black and white are fixed points of the HSL grade, their
   * luma pins them, so a black/white row would not move under a red target. */
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(
      paint_layer_kind_param_color_set(*match, PAINT_LAYER_COLOR_MATCH_TARGET_COLOR, red));
  Vector<float> after_pixels;
  ASSERT_TRUE(render_channel(after_pixels, width, height));
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 1)
      << "the new target replaced the entry, it did not add one";
  bool differs = false;
  for (const int64_t i : after_pixels.index_range()) {
    if (std::abs(after_pixels[i] - before_pixels[i]) > 1e-3f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "a red target shifts what a grey one matched";
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers));
  EXPECT_NE(BKE_paint_material_composite_stack_hash(layers), before_hash)
      << "the stack hash carries the effect's colour";
}

TEST_F(PaintLayersEffectTest, the_glow_at_a_max_spread_keeps_a_narrow_sharp_mask)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *glow = add_effect(*row, MA_PAINT_LAYER_FILTER_GLOW);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*glow, PAINT_LAYER_GLOW_MODE, PAINT_LAYER_GLOW_PRECISE));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*glow, PAINT_LAYER_GLOW_WIDTH, 1));
  /* The luminance is clamped to [0, 1], so a spread of one would choke the mask to nothing; the
   * schema stops the slider at 0.99, and a request for one clamps down to it. */
  ASSERT_TRUE(paint_layer_kind_param_set(*glow, PAINT_LAYER_GLOW_SPREAD, 1.0f));
  EXPECT_NEAR(paint_layer_kind_param_get(*glow, PAINT_LAYER_GLOW_SPREAD), 0.99f, 1e-6f);

  /* At the maximum the mask keeps only the brightest texels: the white half glows over the edge
   * into the grey one, while the grey half -- which a spread of zero would have lit at its own
   * luminance -- stays out of the mask. Not the fully choked glow a spread of one would be. */
  Vector<float> after;
  ASSERT_TRUE(render_channel(after, width, height));
  ASSERT_EQ(after.size(), before.size());
  bool differs = false;
  for (const int64_t i : after.index_range()) {
    if (std::abs(after[i] - before[i]) > 1e-3f) {
      differs = true;
      break;
    }
  }
  EXPECT_TRUE(differs) << "a spread of 0.99 still lays a glow";
  EXPECT_GT(std::abs(linear_pixel(after, width, 4, 4) - linear_pixel(before, width, 4, 4)), 2e-3f)
      << "the glow bleeds over the bright edge into the first grey column";
  EXPECT_NEAR(linear_pixel(after, width, 3, 4), linear_pixel(before, width, 3, 4), 2e-3f)
      << "the bright side keeps its light";
  EXPECT_NEAR(linear_pixel(after, width, 5, 4), linear_pixel(before, width, 5, 4), 2e-3f)
      << "the grey beyond the one-texel reach is untouched";
}

TEST_F(PaintLayersEffectTest, a_bevel_of_an_opaque_row_is_one_everywhere)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 0, 0));
  add_effect(*row, MA_PAINT_LAYER_FILTER_BEVEL);

  /* The shade images are opaque and the buffer's edge is not a border: no seed, the ramp is full. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), 1.0f, 2e-3f) << x << "," << y;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_bevel_ramps_from_a_transparent_border)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 255, false, 0));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  MaterialPaintLayer *bevel = add_effect(*row, MA_PAINT_LAYER_FILTER_BEVEL);
  ASSERT_TRUE(
      paint_layer_kind_param_int_set(*bevel, PAINT_LAYER_BEVEL_CORNER, PAINT_LAYER_BEVEL_ANGULAR));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*bevel, PAINT_LAYER_BEVEL_DISTANCE, 2));

  /* The right half is transparent, so the border runs along column 4: the angular ramp of the
   * distance two is one two texels in and a half beside the border, and the transparent side keeps
   * what it showed before -- the alpha the kernel leaves alone. */
  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    EXPECT_NEAR(linear_pixel(pixels, width, 0, y), 1.0f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 1, y), 1.0f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 2, y), 1.0f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 3, y), 0.5f, 2e-3f);
    for (const int x : IndexRange(4, 4)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), linear_pixel(before, width, x, y), 2e-3f)
          << "the transparent side keeps its bottom, at " << x << "," << y;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_bevel_of_a_luminance_source_ramps_from_the_dark)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *bevel = add_effect(*row, MA_PAINT_LAYER_FILTER_BEVEL);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*bevel, PAINT_LAYER_BEVEL_SOURCE,
                                             PAINT_LAYER_BEVEL_SOURCE_LUMINANCE));
  ASSERT_TRUE(
      paint_layer_kind_param_int_set(*bevel, PAINT_LAYER_BEVEL_CORNER, PAINT_LAYER_BEVEL_ANGULAR));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*bevel, PAINT_LAYER_BEVEL_DISTANCE, 2));

  /* The source is the luminance now: the dark half is the border the ramp rises from, with the
   * same angular steps the alpha source walks, and the row is opaque so the composite shows it all. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    EXPECT_NEAR(linear_pixel(pixels, width, 0, y), 1.0f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 2, y), 1.0f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 3, y), 0.5f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 4, y), 0.0f, 2e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 7, y), 0.0f, 2e-3f);
  }
}

TEST_F(PaintLayersEffectTest, a_color_match_of_a_solid_row_lands_on_the_target)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  MaterialPaintLayer *match = add_effect(*row, MA_PAINT_LAYER_FILTER_COLOR_MATCH);
  const float target[4] = {0.8f, 0.2f, 0.1f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*match, PAINT_LAYER_COLOR_MATCH_TARGET_COLOR, target));

  /* The source is the input's own average, so every texel of a solid input is the source and the
   * grade puts it exactly on the target. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 2), target[0], 5e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 5), target[0], 5e-3f);
}

TEST_F(PaintLayersEffectTest, a_color_match_averages_only_where_there_is_content)
{
  /* The left half is grey with content, the right half is white with no alpha: the average the
   * source reads is the grey alone, since a transparent texel brings no weight -- a plain mean of
   * the map would be pulled toward the white and the left half would miss the target. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 255, false, 0));
  MaterialPaintLayer *match = add_effect(*row, MA_PAINT_LAYER_FILTER_COLOR_MATCH);
  const float target[4] = {0.8f, 0.2f, 0.1f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*match, PAINT_LAYER_COLOR_MATCH_TARGET_COLOR,
                                               target));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    EXPECT_NEAR(linear_pixel(pixels, width, 0, y), target[0], 5e-3f);
    EXPECT_NEAR(linear_pixel(pixels, width, 3, y), target[0], 5e-3f);
  }
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_glow_the_composite_shows)
{
  expect_bake_matches_composite(MA_PAINT_LAYER_FILTER_GLOW, [](MaterialPaintLayer &effect) {
    EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_GLOW_WIDTH, 2));
  });
}

TEST_F(PaintLayersEffectTest, the_bake_renders_the_color_match_the_composite_shows)
{
  expect_bake_matches_composite(MA_PAINT_LAYER_FILTER_COLOR_MATCH);
}

/** \} */

}  // namespace blender::bke::tests
