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
#include <array>
#include <cmath>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_math_color.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"

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

  /** The GPU effect cache's starting budget, for the same reason. */
  static size_t default_gpu_budget()
  {
    static const size_t budget = paint_layer_effect_gpu_cache_budget();
    return budget;
  }

  /** The mask effect cache's starting budget, for the same reason. */
  static size_t default_mask_budget()
  {
    static const size_t budget = paint_layer_mask_effect_cache_budget();
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
    paint_layer_effect_gpu_cache_set_budget(default_gpu_budget());
    BKE_paint_layers_effect_gpu_cache_free_all();
    paint_layer_mask_effect_cache_set_budget(default_mask_budget());
    BKE_paint_layers_mask_effect_cache_free_all();
  }

  void TearDown() override
  {
    paint_layer_effect_cache_set_budget(default_budget());
    BKE_paint_layers_effect_cache_free_all();
    paint_layer_effect_gpu_cache_set_budget(default_gpu_budget());
    BKE_paint_layers_effect_gpu_cache_free_all();
    paint_layer_mask_effect_cache_set_budget(default_mask_budget());
    BKE_paint_layers_mask_effect_cache_free_all();
    /* The stroke flag is runtime state a test may have left set; clear it so it cannot leak into
     * the next test. */
    BKE_paint_layers_bake_stroke_active_set(false);
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

  /** An opaque 8x8 byte image whose grey runs from black at the left edge to white at the right
   * one: a mask map that makes every texel's coverage different, so a mask folded into a cache
   * cannot be mistaken for one that was left out of it. */
  Image *add_ramp_image(const char *name)
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
        uchar *p = pixels + (int64_t(y) * size + x) * 4;
        const uchar value = uchar(x * 255 / (size - 1));
        p[0] = value;
        p[1] = value;
        p[2] = value;
        p[3] = 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** Every byte of two channel composites, RGBA, within the given tolerance. */
  static void expect_bytes_close(const ImBuf &a, const ImBuf &b, const int tolerance)
  {
    const uchar *pa = a.byte_data();
    const uchar *pb = b.byte_data();
    ASSERT_NE(pa, nullptr);
    ASSERT_NE(pb, nullptr);
    for (const int64_t i : IndexRange(int64_t(a.x) * a.y)) {
      for (const int k : IndexRange(4)) {
        EXPECT_NEAR(int(pa[i * 4 + k]), int(pb[i * 4 + k]), tolerance)
            << "texel " << i << " component " << k;
      }
    }
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

  /**
   * A row carrying a Noise under a mask of its own, composited live and then composited over its
   * bake: the same picture either way, within the byte rounding a cache costs. The mask is laid over
   * the content in both passes -- live because the row is not cached, baked because a content bake
   * holds no mask -- so a mask of half grey and a mask of a ramp are both read here.
   */
  void expect_content_bake_matches_live_row(const float mask_value, const bool use_mask_map)
  {
    MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
    MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
    ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
    if (use_mask_map) {
      ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
    }
    else {
      ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, mask_value), nullptr);
    }
    ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.6f));
    clear_active();

    /* The live pass, with nothing cached: the graph lays the mask over the row's own map. */
    ImBuf *live = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
    ASSERT_NE(live, nullptr);
    live->channels = 4;
    ASSERT_TRUE(render_channel_bytes(*live));
    ASSERT_FALSE(paint_layer_row_substituted(*ma, *row));

    ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
    ASSERT_NE(row->bake, nullptr);
    ASSERT_TRUE(paint_layer_row_substituted(*ma, *row));

    ImBuf *baked = IMB_allocImBuf(8, 8, ImBufFlags::ByteData);
    ASSERT_NE(baked, nullptr);
    baked->channels = 4;
    ASSERT_TRUE(render_channel_bytes(*baked));
    expect_bytes_close(*live, *baked, 2);
    IMB_freeImBuf(live);
    IMB_freeImBuf(baked);
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
  add_effect(*row, MA_PAINT_LAYER_FILTER_HIGH_PASS);

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
  add_effect(*row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);

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
  add_effect(*row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);

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
  add_effect(*row, MA_PAINT_LAYER_FILTER_EMBOSS);
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
  add_effect(*row, MA_PAINT_LAYER_FILTER_EMBOSS);

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

  /* The three kinds that stay on the CPU still refuse the combined preview by their own reason, and
   * a describe-only pass computes none of them. The separable and 3x3 kinds (Blur, Sharpen, High
   * Pass, Edge Detect, Emboss, Erode / Dilate) now carry #PAINT_LAYER_KIND_GPU_COMPUTED, so they no
   * longer appear here -- they are covered by the GPU-shade test below. */
  const std::pair<eMaterialPaintLayerFilter, const char *> reasons[] = {
      {MA_PAINT_LAYER_FILTER_GLOW, "a Glow correction is read"},
      {MA_PAINT_LAYER_FILTER_BEVEL, "a Bevel correction is read"},
      {MA_PAINT_LAYER_FILTER_COLOR_MATCH, "a Color Match correction is read"},
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

TEST_F(PaintLayersEffectTest, the_phase_two_kinds_are_gpu_computed_and_refuse_no_reason)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));

  /* Every kind this series moved onto the GPU: the description names no reason to fall back, and the
   * flag the refusal and the gather's queue both read is set. A describe-only pass still computes
   * nothing. */
  const eMaterialPaintLayerFilter gpu_kinds[] = {
      MA_PAINT_LAYER_FILTER_BLUR,
      MA_PAINT_LAYER_FILTER_SHARPEN,
      MA_PAINT_LAYER_FILTER_HIGH_PASS,
      MA_PAINT_LAYER_FILTER_EDGE_DETECT,
      MA_PAINT_LAYER_FILTER_EMBOSS,
      MA_PAINT_LAYER_FILTER_ERODE_DILATE,
  };
  for (const eMaterialPaintLayerFilter filter : gpu_kinds) {
    MaterialPaintLayer *effect = add_effect(*row, filter);
    ASSERT_NE(effect, nullptr);
    EXPECT_TRUE(BKE_paint_layers_effect_is_gpu_computed(*effect))
        << "the registry flags this kind as GPU-computed";

    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(
        BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
    ASSERT_FALSE(layers.is_empty());
    EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), nullptr);
    EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0);

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

  /* A Blur is now a GPU-computed kind: the combined preview names no reason to fall back, and a
   * describe-only pass still computes nothing on the CPU. The render above made its own entry. */
  const int entries_before = paint_layer_effect_cache_entry_count();
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), nullptr);
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), entries_before)
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

TEST_F(PaintLayersEffectTest, a_noise_row_is_baked_while_the_row_with_its_base_mask_is_active)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));

  /* Adding the base mask makes the row itself the active marker, which must not defer a bake the
   * live graph cannot replace. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

/**
 * The mask of a row that carries a CPU-computed effect is no part of its bake: the effect reads the
 * row's colour before the mask is laid, so the cache holds the content alone and the mask, the
 * opacity and the blend are laid live over the substituted map. A stroke into the mask therefore
 * neither moves the bake's hash nor reaches it through the subscription, and the row keeps standing
 * in -- which is what stops the grain from blinking out and back on every stroke.
 */
TEST_F(PaintLayersEffectTest, a_mask_stroke_leaves_a_noise_rows_bake_valid)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  /* A mask of its own map, so a stroke into it is a pixel edit of a map a bake could subscribe to.
   */
  Image *mask_map = add_ramp_image("Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *mask_map), nullptr);
  clear_active();

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *row));
  EXPECT_TRUE(changed);

  /* The stroke: the map says its pixels changed, and the plan is asked what to do about it. */
  BKE_image_partial_update_mark_full_update(mask_map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "the mask is live over the content, not a source of the bake";
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row))
      << "the row keeps its cache, so the grain does not leave the view";

  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed)) << "nothing is due";
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersEffectTest, a_noise_seed_rebakes_the_row_while_its_mask_and_opacity_do_not)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  clear_active();

  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* The row's own opacity, and the mask's grey and its opacity: all live over the content, so none
   * of them asks for a new render. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.4f));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row)) << "the opacity is laid over the map";
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, grey));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, mask, 0.75f));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row)) << "the mask is laid over the map too";
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma)) << "and nothing is due for it";

  /* A new seed is a new grain, and the grain is what the cache holds: the row is due again. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 6));
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_FALSE(paint_layer_row_substituted(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

TEST_F(PaintLayersEffectTest, a_noise_row_under_a_flat_mask_composites_the_same_baked_and_live)
{
  expect_content_bake_matches_live_row(0.5f, /*use_mask_map=*/false);
}

TEST_F(PaintLayersEffectTest, a_noise_row_under_a_mask_map_composites_the_same_baked_and_live)
{
  expect_content_bake_matches_live_row(1.0f, /*use_mask_map=*/true);
}

/**
 * Bakes what is due the way the app does: the planner takes the light rows, and the worker job takes
 * a heavy one. A folder with a child and an effect is heavy by its node weight, so the planner alone
 * leaves it to the job. True when either of them baked something.
 */
static bool bake_folder_to_completion(Main &bmain, Material &ma, bool *r_changed)
{
  bool baked = BKE_paint_layers_bake_plan_run(bmain, ma, r_changed);
  if (BKE_paint_layers_bake_heavy_pending(ma)) {
    PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(bmain, ma);
    if (job != nullptr) {
      BKE_paint_layers_bake_job_compute(*job, {}, {});
      baked |= BKE_paint_layers_bake_job_commit(*job);
      BKE_paint_layers_bake_job_free(*job);
      if (r_changed != nullptr) {
        *r_changed |= baked;
      }
    }
  }
  return baked;
}

TEST_F(PaintLayersEffectTest, a_blurred_folder_keeps_its_bake_when_its_mask_is_painted)
{
  MaterialPaintLayer *first = add_paint_layer("A", add_shade_image("A", 255, 0));
  MaterialPaintLayer *second = add_paint_layer("B", add_shade_image("B", 0, 255));
  MaterialPaintLayer *members[] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  /* An isolating folder, which is what a folder row of its own is: Pass Through builds no row. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.9f));
  ASSERT_NE(add_effect(*folder, MA_PAINT_LAYER_FILTER_BLUR), nullptr);
  Image *mask_map = add_ramp_image("Folder Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *folder, *mask_map), nullptr);
  clear_active();

  /* The children are inside the folder's content, each with its own mask; the folder's own mask is
   * the one that stays live over the cache. */
  bool changed = false;
  ASSERT_TRUE(bake_folder_to_completion(*bmain, *ma, &changed));
  ASSERT_NE(folder->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  ASSERT_TRUE(paint_layer_row_substituted(*ma, *folder));

  BKE_image_partial_update_mark_full_update(mask_map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
  EXPECT_TRUE(paint_layer_row_substituted(*ma, *folder));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed))
      << "the folder is not re-baked";
}

TEST_F(PaintLayersEffectTest, a_stroke_keeps_a_noise_rows_bake_deferred_until_it_ends)
{
  Image *map = add_shade_image("Row", 40, 200);
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  BKE_paint_layers_active_set(*ma, row->marker);

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* The stroke: a pixel edit of the row's own colour map, the marker still inside, the stroke
   * still running. The effect is computed over the whole canvas, so the row is not re-rendered at
   * every dab: it stays deferred and shows live, without the effect. */
  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(map);
  BKE_paint_layers_bake_notice_changes(*ma);

  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed)) << "the stroke defers the re-bake";
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "stale: the old bake is kept and not substituted";

  /* The stroke is over: the row is baked once, even while the marker stays inside it. */
  BKE_paint_layers_bake_stroke_active_set(false);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

TEST_F(PaintLayersEffectTest, a_stroke_outside_the_row_bakes_it_as_usual)
{
  Image *map = add_shade_image("Row", 40, 200);
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);

  /* A stroke that does not edit inside the row defers nothing: only the row the marker sits in
   * stays live. */
  clear_active();
  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

TEST_F(PaintLayersEffectTest, a_seed_change_bakes_now_even_while_a_stroke_runs)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  BKE_paint_layers_active_set(*ma, row->marker);
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);

  /* A structural edit is what the user asks to see recomputed: it does not wait for a stroke. */
  BKE_paint_layers_bake_stroke_active_set(true);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 9));
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

TEST_F(PaintLayersEffectTest, a_first_bake_happens_even_while_a_stroke_runs)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  BKE_paint_layers_active_set(*ma, row->marker);

  /* Nothing has been baked yet: the first bake is not deferred by the stroke. */
  BKE_paint_layers_bake_stroke_active_set(true);
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
}

/**
 * The bake of a folder that carries a CPU-computed effect folds its children's content in: the
 * folder's bake subscribes to the maps its children read, so a stroke into a child is a pixel edit
 * of a source of that bake. While the stroke runs, the folder is not re-rendered at every dab --
 * it stays deferred and shows live, and the bake catches up once the stroke is over.
 */
TEST_F(PaintLayersEffectTest, a_stroke_into_a_child_defers_the_blurred_folders_bake)
{
  Image *map_a = add_shade_image("A", 255, 0);
  Image *map_b = add_shade_image("B", 0, 255);
  MaterialPaintLayer *first = add_paint_layer("A", map_a);
  MaterialPaintLayer *second = add_paint_layer("B", map_b);
  MaterialPaintLayer *members[] = {first, second};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  /* An isolating folder, which is what a folder row of its own is: Pass Through builds no row. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.9f));
  ASSERT_NE(add_effect(*folder, MA_PAINT_LAYER_FILTER_BLUR), nullptr);
  /* The marker sits inside the folder, on the child: the folder is the row being edited. */
  BKE_paint_layers_active_set(*ma, first->marker);

  bool changed = false;
  ASSERT_TRUE(bake_folder_to_completion(*bmain, *ma, &changed));
  ASSERT_NE(folder->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  /* The stroke: a pixel edit of a map the folder's bake reads through its child. */
  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(map_a);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed))
      << "the stroke defers the folder's re-bake";
  EXPECT_FALSE(changed);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *folder))
      << "stale: the old bake is kept and not substituted";

  /* The stroke is over: the folder is baked once, while the marker stays inside it. */
  BKE_paint_layers_bake_stroke_active_set(false);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));
  changed = false;
  EXPECT_TRUE(bake_folder_to_completion(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));
}

TEST_F(PaintLayersEffectTest, a_stroke_into_the_mask_does_not_defer_a_noise_rows_bake)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));
  /* A mask of its own map, so a stroke into it is a pixel edit of a map a bake could subscribe to.
   */
  Image *mask_map = add_ramp_image("Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *mask_map), nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* The stroke flag alone defers nothing: the mask is laid live over the content and is no source
   * of the bake, so a stroke into it never reaches the subscription. */
  BKE_paint_layers_bake_stroke_active_set(true);
  BKE_image_partial_update_mark_full_update(mask_map);
  BKE_paint_layers_bake_notice_changes(*ma);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "the mask is live over the content, not a source of the bake";
  EXPECT_FALSE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed)) << "nothing is due";
  EXPECT_FALSE(changed);
}

TEST_F(PaintLayersEffectTest, a_noise_is_described_without_a_buffer_and_the_gpu_shades_it)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_NE(noise, nullptr);

  /* The grain is a case of the preview's own effect pass, so the description names no reason to fall
   * back -- and a describe-only pass still computes nothing on the CPU. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), nullptr);
  EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0) << "a describe-only pass computes nothing";
  EXPECT_TRUE(BKE_paint_layers_effect_is_gpu_computed(*noise));

  /* The correction names the effect instance the GPU cache keys its texture on and carries the
   * parameter hash of its own, because a description says which *kind* laid a correction but a
   * caller resolving the row needs the marker: two effects of one row share a kind. */
  ASSERT_FALSE(layers[0].content_corrections.is_empty());
  const PaintMaterialCompositeCorrection &correction = layers[0].content_corrections.last();
  EXPECT_TRUE(BLI_uuid_equal(correction.effect_marker, noise->marker));
  EXPECT_NE(correction.effect_hash, 0u);
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

/* -------------------------------------------------------------------- */
/** \name GPU Effect Cache
 *
 * The policy of the GPU result cache: what one effect instance's texture is keyed on, what makes it
 * stale, what the budget evicts. The acquire and release hooks hand out plain ints, so none of that
 * needs a GPU context -- the same trick that keeps #PaintLayersGpuSlotGuard testable. What the hooks
 * return is never read here, only compared and counted.
 * \{ */

namespace {

/** A stand-in for the effect textures and the counts of them made and dropped. */
struct FakeEffectGpu {
  int made = 0;
  int freed = 0;
  /** Set to make the acquire fail, which is how a caller learns the GPU cannot take the effect. */
  bool refuse = false;

  static void *acquire(void *data, const int /*width*/, const int /*height*/)
  {
    FakeEffectGpu &self = *static_cast<FakeEffectGpu *>(data);
    if (self.refuse) {
      return nullptr;
    }
    self.made++;
    return new int(self.made);
  }
  static void release(void *data, void *resource)
  {
    if (resource == nullptr) {
      return;
    }
    static_cast<FakeEffectGpu *>(data)->freed++;
    delete static_cast<int *>(resource);
  }
};

/**
 * One fake for the whole process: the fixture drops the cache in its tear-down, after a test's own
 * scope has ended, and an entry releases its resource through the hooks it was made with. A local
 * fake would already be gone by then.
 */
FakeEffectGpu g_fake_effect_gpu;

/** The hooks of the tests, with the counters of one test's flow put back to zero. */
PaintLayerEffectGpuHooks fake_gpu_hooks()
{
  g_fake_effect_gpu = {};
  return PaintLayerEffectGpuHooks{&g_fake_effect_gpu, FakeEffectGpu::acquire, FakeEffectGpu::release};
}

}  // namespace

TEST_F(PaintLayersEffectTest, the_gpu_effect_cache_publishes_a_hit_only_after_a_render)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();
  const Span<Image *> no_images;
  void *resource = nullptr;
  rcti region;

  /* A key the cache has never seen makes a texture and asks for it to be filled: the entry is not
   * current until the render is stamped onto it. */
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, no_images, {}, hooks, &region, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_NE(resource, nullptr);
  EXPECT_EQ(g_fake_effect_gpu.made, 1);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 1);
  /* Nothing of a fresh entry has been computed, so the whole canvas is what needs doing. */
  EXPECT_EQ(BLI_rcti_size_x(&region), 8);
  EXPECT_EQ(BLI_rcti_size_y(&region), 8);

  BKE_paint_layers_effect_gpu_cache_publish(*ma, *noise, kColor, 8, 8, no_images, {});
  resource = nullptr;
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, no_images, {}, hooks, &region, &resource),
            PaintLayerEffectGpuState::Hit);
  EXPECT_EQ(g_fake_effect_gpu.made, 1) << "the same texture serves the next frame";
  EXPECT_EQ(g_fake_effect_gpu.freed, 0);
  /* A hit names no work: the whole cached canvas is current. */
  EXPECT_TRUE(BLI_rcti_is_empty(&region));
}

TEST_F(PaintLayersEffectTest, the_gpu_effect_cache_separates_effects_channels_and_parameters)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *first = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  MaterialPaintLayer *second = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_NE(first, second);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();
  const Span<Image *> no_images;
  void *resource = nullptr;

  /* Two effects of one row are two instances: their markers differ, so neither can be served the
   * other's grain. */
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *first, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  BKE_paint_layers_effect_gpu_cache_publish(*ma, *first, kColor, 8, 8, no_images, {});
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *second, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 2);
  EXPECT_EQ(g_fake_effect_gpu.made, 2);

  /* Another channel of the same effect is another key, and another set of parameters replaces the
   * entry rather than waiting for the budget to evict it: the description can only ever ask for the
   * parameters the effect carries now. */
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *first, PAINT_MATERIAL_CHANNEL_ROUGHNESS, 8, 8, no_images, {}, hooks, nullptr,
                &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 3);
  const int made = g_fake_effect_gpu.made;
  ASSERT_TRUE(paint_layer_kind_param_int_set(*first, PAINT_LAYER_NOISE_SEED, 7));
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *first, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(g_fake_effect_gpu.made, made + 1);
  EXPECT_EQ(g_fake_effect_gpu.freed, 1) << "the entry of the old seed was dropped, not kept";
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 3);
}

TEST_F(PaintLayersEffectTest, the_gpu_effect_cache_grows_stale_when_the_input_map_is_edited)
{
  Image *map = add_shade_image("Row", 255, 0);
  MaterialPaintLayer *row = add_paint_layer("Row", map);
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();

  /* The maps the input renders from are what the entry vouches for: the effect owns no map itself. */
  Image *source[1] = {map};
  const Span<Image *> images(source, 1);
  void *resource = nullptr;
  rcti region;
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, images, {}, hooks, &region, &resource),
            PaintLayerEffectGpuState::Render);
  BKE_paint_layers_effect_gpu_cache_publish(*ma, *noise, kColor, 8, 8, images, {});
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, images, {}, hooks, &region, &resource),
            PaintLayerEffectGpuState::Hit);

  BKE_image_partial_update_mark_full_update(map);
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, images, {}, hooks, &region, &resource),
            PaintLayerEffectGpuState::Render);
  /* A change that is not a rectangle has no region to name, and the whole canvas is the only safe
   * claim over it. */
  EXPECT_EQ(BLI_rcti_size_x(&region), 8);
  EXPECT_EQ(BLI_rcti_size_y(&region), 8);
  EXPECT_EQ(g_fake_effect_gpu.made, 1) << "the texture was kept, only its pixels went stale";
}

TEST_F(PaintLayersEffectTest, the_gpu_effect_cache_evicts_to_its_budget)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *first = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  MaterialPaintLayer *second = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();
  const Span<Image *> no_images;
  void *resource = nullptr;

  /* One 8x8 RGBA half-float canvas: enough for one entry, so the second pushes the first out. */
  const size_t one_canvas = size_t(8) * 8 * 4 * sizeof(int16_t);
  EXPECT_EQ(paint_layer_effect_gpu_cache_bytes(), 0);
  paint_layer_effect_gpu_cache_set_budget(one_canvas);
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *first, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(paint_layer_effect_gpu_cache_bytes(), one_canvas);
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *second, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 1);
  EXPECT_EQ(paint_layer_effect_gpu_cache_bytes(), one_canvas);
  EXPECT_EQ(g_fake_effect_gpu.freed, 1) << "the evicted entry's texture was handed back";

  /* An entry bigger than the whole budget is still held: the cache must own a texture to release it,
   * and a frame keeps a reference while it renders, so exceeding the budget briefly is what costs
   * nothing to get wrong. */
  paint_layer_effect_gpu_cache_set_budget(one_canvas / 2);
  BKE_paint_layers_effect_gpu_cache_free_all();
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 0);
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *first, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 1);
  EXPECT_EQ(paint_layer_effect_gpu_cache_bytes(), one_canvas);
}

TEST_F(PaintLayersEffectTest, the_gpu_effect_cache_reports_a_texture_it_cannot_make)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();
  g_fake_effect_gpu.refuse = true;
  const Span<Image *> no_images;
  void *resource = reinterpret_cast<void *>(1);

  /* The caller's answer is to fall back to the CPU for the whole stack, so nothing may be held for an
   * effect that failed: the reason strings of `gpu_combined refused` count on it. */
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Failed);
  EXPECT_EQ(resource, nullptr);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 0);
  EXPECT_EQ(paint_layer_effect_gpu_cache_bytes(), 0);
  /* A stamp of a render nobody asked for finds no entry and holds nothing. */
  BKE_paint_layers_effect_gpu_cache_publish(*ma, *noise, kColor, 8, 8, no_images, {});
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 0);
}

TEST_F(PaintLayersEffectTest, the_gpu_effect_cache_drops_a_material_with_its_textures)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *noise = add_effect(*row, MA_PAINT_LAYER_FILTER_NOISE);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();
  const Span<Image *> no_images;
  void *resource = nullptr;
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *noise, kColor, 8, 8, no_images, {}, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 1);

  /* The cache is keyed on a session UID that is never reused, so a freed material's entry could never
   * be hit again and would hold its canvas until the budget came for it. */
  BKE_paint_layers_effect_gpu_cache_free_material(*ma);
  EXPECT_EQ(paint_layer_effect_gpu_cache_entry_count(), 0);
  EXPECT_EQ(g_fake_effect_gpu.freed, 1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Effect Pass Plan, Region & Key
 *
 * The pure half of the GPU effect pass: the pass plan, the neighbourhood halo, the region arithmetic
 * and the two GPU-only key fingerprints, all decided from the effect's kind, parameters and
 * description without a GPU. \{ */

TEST_F(PaintLayersEffectTest, the_halo_matches_each_kernels_total_coverage)
{
  /* Parameters are packed in each kind's schema order, the same order the gather hands the shader. */
  const auto blur = [](const int shape, const int radius) {
    return std::array<float, 2>{float(shape), float(radius)};
  };
  const auto sharpen = [](const int radius, const float amount) {
    return std::array<float, 2>{float(radius), amount};
  };
  const auto high_pass = [](const int radius, const float contrast, const int output) {
    return std::array<float, 3>{float(radius), contrast, float(output)};
  };
  const auto erode = [](const int mode, const int radius) {
    return std::array<float, 2>{float(mode), float(radius)};
  };

  /* A box blur reaches its radius; a soft blur three passes of `max(radius / 2, 1)`. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_BLUR, blur(PAINT_LAYER_BLUR_BOX, 5).data(), 100, 100), 5);
  EXPECT_EQ(BKE_paint_layers_effect_halo(
                MA_PAINT_LAYER_FILTER_BLUR, blur(PAINT_LAYER_BLUR_GAUSSIAN, 8).data(), 100, 100),
            12);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_BLUR,
                                         blur(PAINT_LAYER_BLUR_GAUSSIAN, 1).data(),
                                         100,
                                         100),
            3) << "a radius of one still spends a pass of max(1/2, 1) = 1, three times";
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_SHARPEN, sharpen(3, 0.5f).data(), 100, 100),
            3);
  /* A High Pass clamps its radius to the canvas, as the blur box does. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_HIGH_PASS,
                                         high_pass(200, 1.0f, PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR).data(),
                                         128,
                                         128),
            128);
  EXPECT_EQ(BKE_paint_layers_effect_halo(
                MA_PAINT_LAYER_FILTER_ERODE_DILATE, erode(PAINT_LAYER_ERODE_DILATE_ERODE, 6).data(), 100, 100),
            6);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_EDGE_DETECT, nullptr, 100, 100), 1);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_EMBOSS, nullptr, 100, 100), 1);
  /* A pointwise kind has no neighbourhood to reach. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_NOISE, nullptr, 100, 100), 0);
}

TEST_F(PaintLayersEffectTest, the_pass_plan_matches_each_kernels_shape)
{
  Vector<PaintLayerEffectPass> passes;
  const std::array<float, 2> box{float(PAINT_LAYER_BLUR_BOX), 5.0f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_BLUR, box.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 2);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[0].radius, 5);
  EXPECT_EQ(passes[1].kind, ePaintLayerEffectPass::BlurV);
  EXPECT_EQ(passes[1].radius, 5);
  /* The first pass reads the effect's input and every later one the pass before it; a window pass
   * reads no second buffer, the ping-pong pair carries the whole plan. */
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[1].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[1].aux, ePaintLayerEffectPassAux::None);
  /* A window pass reaches its radius, and only along its own axis: an H pass never grows the
   * region's height, a V one never its width. */
  EXPECT_EQ(passes[0].reach, 5);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_X);
  EXPECT_EQ(passes[1].reach, 5);
  EXPECT_EQ(passes[1].axis_mask, PAINT_LAYER_EFFECT_AXIS_Y);

  const std::array<float, 2> soft{float(PAINT_LAYER_BLUR_GAUSSIAN), 8.0f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_BLUR, soft.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 6) << "three box pairs approximate the gaussian";
  for (const PaintLayerEffectPass &pass : passes) {
    EXPECT_EQ(pass.radius, 4) << "each pass reaches max(8 / 2, 1)";
    EXPECT_EQ(pass.reach, 4) << "a window pass reaches its radius";
  }
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  for (int i = 1; i < int(passes.size()); i++) {
    EXPECT_EQ(passes[i].source, ePaintLayerEffectPassSource::Previous);
  }

  const std::array<float, 2> sharpen{2.0f, 0.5f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_SHARPEN, sharpen.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 3);
  EXPECT_EQ(passes[2].kind, ePaintLayerEffectPass::UnsharpTail);
  EXPECT_NEAR(passes[2].aux0, 0.5f, 1e-6f);
  /* The Sharpen tail reads the original against the blur the plan built, and computes its own texel
   * only, so it reaches nothing. */
  EXPECT_EQ(passes[2].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[2].aux, ePaintLayerEffectPassAux::Previous);
  EXPECT_EQ(passes[2].reach, 0);

  /* A High Pass grayscale prepends the luminance, and its tail reads that buffer instead of the
   * original: the detail is the luminance's own, taken through the sRGB curve. */
  const std::array<float, 3> hp_gray{4.0f, 1.0f, float(PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE)};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_HIGH_PASS, hp_gray.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 4);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Luminance);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[3].kind, ePaintLayerEffectPass::HighPassTail);
  EXPECT_EQ(passes[3].source, ePaintLayerEffectPassSource::Luminance);
  EXPECT_EQ(passes[3].aux, ePaintLayerEffectPassAux::Previous);
  EXPECT_NEAR(passes[3].aux0, 1.0f, 1e-6f);

  /* A High Pass written back as colour needs no luminance: the detail is the colour's own. */
  const std::array<float, 3> hp_color{4.0f, 1.0f, float(PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR)};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_HIGH_PASS, hp_color.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 3);
  EXPECT_EQ(passes[2].kind, ePaintLayerEffectPass::HighPassTail);
  EXPECT_EQ(passes[2].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[2].aux, ePaintLayerEffectPassAux::Previous);

  const std::array<float, 2> erode{float(PAINT_LAYER_ERODE_DILATE_DILATE), 6.0f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_ERODE_DILATE, erode.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 2);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::ExtremumH);
  EXPECT_TRUE(passes[0].maximum) << "dilate takes the maximum";
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[1].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_EQ(passes[0].reach, 6);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_X);
  EXPECT_EQ(passes[1].reach, 6);
  EXPECT_EQ(passes[1].axis_mask, PAINT_LAYER_EFFECT_AXIS_Y);

  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_EDGE_DETECT, nullptr, 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Edge3x3);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  /* The Sobel ring reads one texel past on both axes, so the 3x3 grows the region in both. */
  EXPECT_EQ(passes[0].reach, 1);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);

  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_EMBOSS, nullptr, 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Emboss3x3);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 1);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);

  /* A pointwise effect is one pass that reads no neighbour and no second buffer. */
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_NOISE, nullptr, 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Noise);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 0);

  /* The index and count fields let the host tell which pass writes the cache. */
  EXPECT_EQ(passes[0].pass_index, 0);
  EXPECT_EQ(passes[0].pass_count, 1);
}

TEST_F(PaintLayersEffectTest, every_gpu_computed_kind_builds_a_pass_plan)
{
  /* The loop bound is the last filter enumerator, not a _NUM sentinel (the enum has none): a new
   * filter past it must extend this loop, or a kind flagged GPU-computed would silently render
   * nothing. */
  Set<int> gpu_computed_filters;
  for (int filter = int(MA_PAINT_LAYER_FILTER_INVERT);
       filter <= int(MA_PAINT_LAYER_FILTER_CHANNEL_MIXER);
       filter++)
  {
    const PaintLayerKindInfo *kind = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER, filter);
    if (kind == nullptr) {
      continue;
    }
    if ((kind->flags & PAINT_LAYER_KIND_GPU_COMPUTED) == 0) {
      continue;
    }
    gpu_computed_filters.add(filter);
    /* The schema defaults are the parameters the gather would pack, in the same order. */
    float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
    for (int i = 0; i < kind->params_num; i++) {
      params[i] = kind->params[i].default_value;
    }
    Vector<PaintLayerEffectPass> passes;
    BKE_paint_layers_effect_pass_plan(eMaterialPaintLayerFilter(filter), params, 64, 64, passes);
    EXPECT_FALSE(passes.is_empty())
        << "a kind flagged GPU-computed must have a pass case in the plan builder";
    for (const PaintLayerEffectPass &pass : passes) {
      EXPECT_EQ(pass.pass_count, passes.size());
      EXPECT_GE(pass.pass_index, 0);
      EXPECT_LT(pass.pass_index, pass.pass_count);
      EXPECT_GE(pass.radius, 0);
      EXPECT_GE(pass.reach, 0);
    }
  }
  /* The six neighbourhood kinds are all flagged GPU-computed: the walk must have found every one of
   * them, or the flag and the plan builder have drifted apart. The Noise is flagged too, its
   * pointwise kernel being the one pass the plan names. */
  EXPECT_FALSE(gpu_computed_filters.is_empty());
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_BLUR)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_SHARPEN)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_HIGH_PASS)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_EDGE_DETECT)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_EMBOSS)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_ERODE_DILATE)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_NOISE)));
}

TEST_F(PaintLayersEffectTest, the_region_grows_by_the_halo_and_clamps_to_the_canvas)
{
  rcti dirty;
  BLI_rcti_init(&dirty, 10, 20, 10, 20);
  rcti out;
  BKE_paint_layers_effect_region_clip(dirty, 5, 100, 100, out);
  EXPECT_EQ(out.xmin, 5);
  EXPECT_EQ(out.xmax, 25);
  EXPECT_EQ(out.ymin, 5);
  EXPECT_EQ(out.ymax, 25);

  /* A halo past an edge clamps to the canvas rather than reading outside it. */
  BKE_paint_layers_effect_region_clip(dirty, 500, 100, 100, out);
  EXPECT_EQ(out.xmin, 0);
  EXPECT_EQ(out.xmax, 100);
  EXPECT_EQ(out.ymin, 0);
  EXPECT_EQ(out.ymax, 100);

  /* Nothing moved: an empty region stays empty (there is nothing to recompute). */
  rcti empty;
  BLI_rcti_init(&empty, 0, 0, 0, 0);
  BKE_paint_layers_effect_region_clip(empty, 5, 100, 100, out);
  EXPECT_TRUE(BLI_rcti_is_empty(&out));
}

TEST_F(PaintLayersEffectTest,
       the_pass_regions_cover_the_dirty_grown_by_the_halo_and_each_pass_feeds_the_next)
{
  /* Parameters are packed in each kind's schema order, the same order the gather hands the shader. */
  const auto blur = [](const int shape, const int radius) {
    return std::array<float, 2>{float(shape), float(radius)};
  };
  const auto sharpen = [](const int radius, const float amount) {
    return std::array<float, 2>{float(radius), amount};
  };
  const auto high_pass = [](const int radius, const float contrast, const int output) {
    return std::array<float, 3>{float(radius), contrast, float(output)};
  };
  const auto erode = [](const int mode, const int radius) {
    return std::array<float, 2>{float(mode), float(radius)};
  };

  /* Builds the plan, takes one rectangle per pass and checks the two invariants every plan has: the
   * last pass's band is what keeps the cache from going stale past a stroke, so it must cover the
   * dirty rectangle grown by the plan's whole halo and clamped; and every pass before it writes one
   * rectangle more, by the reach the passes after it still read along each axis, so the next pass
   * samples a neighbourhood its source has already computed. */
  const auto plan_and_check = [](const eMaterialPaintLayerFilter filter,
                                 const float *params,
                                 const rcti &dirty_rect,
                                 const int width,
                                 const int height,
                                 Vector<PaintLayerEffectPass> &r_passes,
                                 Vector<rcti> &r_rects) {
    BKE_paint_layers_effect_pass_plan(filter, params, width, height, r_passes);
    r_rects.reinitialize(r_passes.size());
    BKE_paint_layers_effect_pass_regions(Span<const PaintLayerEffectPass>(r_passes.data(), r_passes.size()), dirty_rect, 0, width, height, r_rects.as_mutable_span());
    rcti whole;
    BKE_paint_layers_effect_region_clip(
        dirty_rect, BKE_paint_layers_effect_halo(filter, params, width, height), width, height, whole);
    EXPECT_LE(r_rects.last().xmin, whole.xmin);
    EXPECT_LE(r_rects.last().ymin, whole.ymin);
    EXPECT_GE(r_rects.last().xmax, whole.xmax);
    EXPECT_GE(r_rects.last().ymax, whole.ymax);
    for (int p = 0; p < int(r_passes.size()) - 1; p++) {
      const PaintLayerEffectPass &next = r_passes[p + 1];
      rcti needed = r_rects[p + 1];
      if ((next.axis_mask & PAINT_LAYER_EFFECT_AXIS_X) != 0) {
        needed.xmin = std::max(needed.xmin - next.reach, 0);
        needed.xmax = std::min(needed.xmax + next.reach, width);
      }
      if ((next.axis_mask & PAINT_LAYER_EFFECT_AXIS_Y) != 0) {
        needed.ymin = std::max(needed.ymin - next.reach, 0);
        needed.ymax = std::min(needed.ymax + next.reach, height);
      }
      EXPECT_LE(r_rects[p].xmin, needed.xmin);
      EXPECT_LE(r_rects[p].ymin, needed.ymin);
      EXPECT_GE(r_rects[p].xmax, needed.xmax);
      EXPECT_GE(r_rects[p].ymax, needed.ymax);
    }
  };

  rcti dirty;
  BLI_rcti_init(&dirty, 40, 60, 40, 60);
  Vector<PaintLayerEffectPass> passes;
  Vector<rcti> rects;

  /* Every GPU-computed kind through the two generic invariants. */
  const std::array<float, 2> box = blur(PAINT_LAYER_BLUR_BOX, 5);
  plan_and_check(MA_PAINT_LAYER_FILTER_BLUR, box.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 2> soft = blur(PAINT_LAYER_BLUR_GAUSSIAN, 8);
  plan_and_check(MA_PAINT_LAYER_FILTER_BLUR, soft.data(), dirty, 100, 100, passes, rects);
  /* The numbers further down are the soft blur's; the kinds after it reuse the same vectors. */
  const Vector<PaintLayerEffectPass> soft_passes = passes;
  const Vector<rcti> soft_rects = rects;
  const std::array<float, 2> sharpen_params = sharpen(3, 0.5f);
  plan_and_check(MA_PAINT_LAYER_FILTER_SHARPEN, sharpen_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 3> hp_gray = high_pass(4, 1.0f, PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE);
  plan_and_check(MA_PAINT_LAYER_FILTER_HIGH_PASS, hp_gray.data(), dirty, 100, 100, passes, rects);
  /* The radius clamps to the canvas, so a 200 on a 128x128 canvas is a 128: the last band is the
   * whole canvas either way. */
  const std::array<float, 3> hp_color = high_pass(200, 1.0f, PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR);
  plan_and_check(MA_PAINT_LAYER_FILTER_HIGH_PASS, hp_color.data(), dirty, 128, 128, passes, rects);
  const std::array<float, 2> erode_params = erode(PAINT_LAYER_ERODE_DILATE_DILATE, 6);
  plan_and_check(MA_PAINT_LAYER_FILTER_ERODE_DILATE,
                 erode_params.data(),
                 dirty,
                 100,
                 100,
                 passes,
                 rects);
  plan_and_check(MA_PAINT_LAYER_FILTER_EDGE_DETECT, nullptr, dirty, 100, 100, passes, rects);
  plan_and_check(MA_PAINT_LAYER_FILTER_EMBOSS, nullptr, dirty, 100, 100, passes, rects);

  /* Per axis, in numbers: an H pass does not grow the height because the V passes after it already
   * do, and the other way around -- the widths and the heights move independently. The gaussian is
   * three pairs of a reach of 4, so the halo is 12 per axis; the first pass (an H one) widens by the
   * halo plus the 8 the two H passes after it still read, and heightens by the halo plus the 12 the
   * three V passes read. */
  ASSERT_EQ(soft_passes.size(), 6);
  EXPECT_EQ(soft_rects[0].xmin, 20);
  EXPECT_EQ(soft_rects[0].xmax, 80);
  EXPECT_EQ(soft_rects[0].ymin, 16);
  EXPECT_EQ(soft_rects[0].ymax, 84);
  /* The last pass is a V one: the dirty grown by the whole halo (12 per axis), clamped. */
  EXPECT_EQ(soft_rects[5].xmin, 28);
  EXPECT_EQ(soft_rects[5].xmax, 72);
  EXPECT_EQ(soft_rects[5].ymin, 28);
  EXPECT_EQ(soft_rects[5].ymax, 72);

  /* A radius past half the canvas pushes the last band over the whole canvas: there is nothing
   * outside to read, so every texel is recomputed. */
  rcti small_dirty;
  BLI_rcti_init(&small_dirty, 2, 5, 2, 5);
  const std::array<float, 2> big_blur = blur(PAINT_LAYER_BLUR_BOX, 8);
  plan_and_check(MA_PAINT_LAYER_FILTER_BLUR, big_blur.data(), small_dirty, 8, 8, passes, rects);
  EXPECT_EQ(rects.last().xmin, 0);
  EXPECT_EQ(rects.last().xmax, 8);
  EXPECT_EQ(rects.last().ymin, 0);
  EXPECT_EQ(rects.last().ymax, 8);
  const std::array<float, 3> big_high_pass = high_pass(16, 1.0f, PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR);
  plan_and_check(MA_PAINT_LAYER_FILTER_HIGH_PASS, big_high_pass.data(), small_dirty, 8, 8, passes, rects);
  EXPECT_EQ(rects.last().xmin, 0);
  EXPECT_EQ(rects.last().xmax, 8);
  EXPECT_EQ(rects.last().ymin, 0);
  EXPECT_EQ(rects.last().ymax, 8);

  /* The chain halo is the band the earlier effect's own re-render changed: the sharpen's input is
   * the blur's output, so the blur's halo band re-rendered last frame is part of what the sharpen
   * has to redo, and its last pass must cover the dirty grown by both halos together (5 + 3 = 8). */
  Vector<PaintLayerEffectPass> sharpen_passes;
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_SHARPEN, sharpen_params.data(), 100, 100, sharpen_passes);
  Vector<rcti> chained;
  chained.reinitialize(sharpen_passes.size());
  BKE_paint_layers_effect_pass_regions(Span<const PaintLayerEffectPass>(sharpen_passes.data(), sharpen_passes.size()), dirty, 5, 100, 100, chained.as_mutable_span());
  EXPECT_EQ(chained.last().xmin, 32);
  EXPECT_EQ(chained.last().xmax, 68);
  EXPECT_EQ(chained.last().ymin, 32);
  EXPECT_EQ(chained.last().ymax, 68);
  /* The same plan without the chain halo is strictly narrower: the 5 texels the blur before it in
   * the row adds are the whole difference. */
  Vector<rcti> alone;
  alone.reinitialize(sharpen_passes.size());
  BKE_paint_layers_effect_pass_regions(Span<const PaintLayerEffectPass>(sharpen_passes.data(), sharpen_passes.size()), dirty, 0, 100, 100, alone.as_mutable_span());
  EXPECT_LT(chained.last().xmin, alone.last().xmin);
  EXPECT_LT(chained.last().ymin, alone.last().ymin);
  EXPECT_GT(chained.last().xmax, alone.last().xmax);
  EXPECT_GT(chained.last().ymax, alone.last().ymax);

  /* Nothing moved, nothing to compute: every pass gets an empty rectangle, and the caller renders
   * the whole canvas instead of dispatching. */
  rcti moved_not;
  BLI_rcti_init(&moved_not, 0, 0, 0, 0);
  BKE_paint_layers_effect_pass_regions(Span<const PaintLayerEffectPass>(sharpen_passes.data(), sharpen_passes.size()), moved_not, 0, 100, 100, chained.as_mutable_span());
  for (const rcti &rect : chained) {
    EXPECT_TRUE(BLI_rcti_is_empty(&rect));
  }
}

TEST_F(PaintLayersEffectTest, the_chain_hash_tracks_the_effects_before_this_one_in_order)
{
  Vector<PaintMaterialCompositeCorrection> corrections;
  const bUUID marker_a = BLI_uuid_generate_random();
  const bUUID marker_b = BLI_uuid_generate_random();
  const bUUID marker_target = BLI_uuid_generate_random();

  PaintMaterialCompositeCorrection effect_a;
  effect_a.effect_marker = marker_a;
  effect_a.effect_hash = 111;
  PaintMaterialCompositeCorrection effect_b;
  effect_b.effect_marker = marker_b;
  effect_b.effect_hash = 222;
  PaintMaterialCompositeCorrection target;
  target.effect_marker = marker_target;
  target.effect_hash = 333;
  corrections.append(effect_a);
  corrections.append(effect_b);
  corrections.append(target);

  /* Build the view at each call site: GCC 14 instantiates std::vector<const T> (which fails its
   * static assertion) when a named const-element span lvalue is passed by value. */
  const auto span_of = [&]() {
    return Span<const PaintMaterialCompositeCorrection>(corrections.data(), corrections.size());
  };
  const uint64_t chain = BKE_paint_layers_effect_chain_hash(span_of(), marker_target);
  EXPECT_NE(chain, 0u);

  /* A change to an earlier effect -- its parameters -- re-keys the later one. */
  corrections[0].effect_hash = 112;
  EXPECT_NE(BKE_paint_layers_effect_chain_hash(span_of(), marker_target), chain);

  /* Swapping the order of the effects before the target re-keys it too. */
  corrections[0].effect_hash = 111;
  const uint64_t ordered = BKE_paint_layers_effect_chain_hash(span_of(), marker_target);
  PaintMaterialCompositeCorrection swapped_a = corrections[0];
  PaintMaterialCompositeCorrection swapped_b = corrections[1];
  corrections[0] = swapped_b;
  corrections[1] = swapped_a;
  EXPECT_NE(BKE_paint_layers_effect_chain_hash(span_of(), marker_target), ordered);

  /* A non-effect correction (nil marker) adds nothing before the target. */
  PaintMaterialCompositeCorrection plain;
  plain.opacity = 1.0f;
  corrections.clear();
  corrections.append(plain);
  corrections.append(target);
  EXPECT_EQ(BKE_paint_layers_effect_chain_hash(span_of(), marker_target),
            BKE_paint_layers_effect_chain_hash(Span<const PaintMaterialCompositeCorrection>(),
                                               marker_target))
      << "no effect precedes the target, so the fingerprint is the empty fold";
}

TEST_F(PaintLayersEffectTest, the_input_scalar_hash_sees_a_flat_colour_edit)
{
  PaintMaterialCompositeImageLayer entry;
  entry.has_constant_color = true;
  entry.constant_color[0] = 0.5f;
  entry.constant_color[1] = 0.5f;
  entry.constant_color[2] = 0.5f;
  entry.constant_color[3] = 1.0f;
  const uint64_t base = BKE_paint_layers_effect_input_scalar_hash(entry);

  entry.constant_color[0] = 0.6f;
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(entry), base)
      << "a flat row colour is part of the effect's input";
}

TEST_F(PaintLayersEffectTest, the_input_scalar_hash_sees_the_gradient_and_the_reader)
{
  /* A live Anchor Point reader is described by its identity, not its pixels: a map version carries
   * none of it, so the reader's uuid and its slice hash key the GPU entry the way a flat colour
   * does. */
  PaintMaterialCompositeImageLayer entry;
  PaintMaterialCompositeCorrection reader;
  reader.anchor_point_reader = BLI_uuid_generate_random();
  reader.anchor_point_hash = 7;
  entry.content_corrections.append(reader);
  const uint64_t live = BKE_paint_layers_effect_input_scalar_hash(entry);

  entry.content_corrections[0].anchor_point_hash = 8;
  const uint64_t rehashed = BKE_paint_layers_effect_input_scalar_hash(entry);
  EXPECT_NE(rehashed, live) << "the slice hash is the reader's own fingerprint";

  entry.content_corrections[0].anchor_point_reader = {};
  const uint64_t detached = BKE_paint_layers_effect_input_scalar_hash(entry);
  EXPECT_NE(detached, live);
  EXPECT_NE(detached, rehashed) << "no reader is another input, not the live one with new data";

  /* A gradient generator's payload rides the same way: the map version carries none of it, so the
   * generator and its payload key the GPU entry the way a flat colour does. */
  PaintMaterialCompositeImageLayer plain;
  const uint64_t without_gradient = BKE_paint_layers_effect_input_scalar_hash(plain);
  plain.gradient.enabled = true;
  plain.gradient.scalar_start = 0.25f;
  const uint64_t with_gradient = BKE_paint_layers_effect_input_scalar_hash(plain);
  EXPECT_NE(with_gradient, without_gradient) << "an enabled generator is part of the described input";

  plain.gradient.scalar_start = 0.5f;
  const uint64_t moved_ends = BKE_paint_layers_effect_input_scalar_hash(plain);
  EXPECT_NE(moved_ends, with_gradient) << "the scalar ends are the generator's own content";

  plain.gradient.payload.midpoint = 0.3f;
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(plain), moved_ends)
      << "the payload's midpoint bends the ramp, so it keys the entry too";
}

TEST_F(PaintLayersEffectTest, the_gpu_cache_keys_on_the_chain_and_scalar_fingerprints)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  const PaintLayerEffectGpuHooks hooks = fake_gpu_hooks();
  const Span<Image *> no_images;
  void *resource = nullptr;

  /* Published under one fingerprint, the same effect is a hit; a different chain or input-scalar
   * fingerprint is a new key and a re-render (the entry of the old parameters is dropped). */
  const PaintLayerEffectGpuKeyExtras first_key{0xAAu, 0xBBu};
  const PaintLayerEffectGpuKeyExtras other_scalar{0xAAu, 0xCCu};
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *blur, kColor, 8, 8, no_images, first_key, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render);
  BKE_paint_layers_effect_gpu_cache_publish(*ma, *blur, kColor, 8, 8, no_images, first_key);
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *blur, kColor, 8, 8, no_images, first_key, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Hit);
  EXPECT_EQ(BKE_paint_layers_effect_gpu_cache_get(
                *ma, *blur, kColor, 8, 8, no_images, other_scalar, hooks, nullptr, &resource),
            PaintLayerEffectGpuState::Render)
      << "a different input-scalar fingerprint is a different entry";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask effects (a CPU-computed kind as a mask item)
 * \{ */

namespace {

/** A CPU-computed effect as an item of \a row's mask stack, over its base mask. */
MaterialPaintLayer *add_mask_effect(Material &ma,
                                    MaterialPaintLayer &row,
                                    const eMaterialPaintLayerFilter filter)
{
  MaterialPaintLayer *effect = BKE_paint_layers_mask_filter_add(ma, &row, filter, nullptr);
  EXPECT_NE(effect, nullptr);
  EXPECT_EQ(effect->role, MA_PAINT_LAYER_ROLE_MASK_ITEM) << "a mask effect is an item of the mask";
  return effect;
}

/** The grey the mask effect \a filter lays, read from the composite description; null when the
 * description lays none. The fixture holds one row, so the first item of the kind is the one. */
std::shared_ptr<ImBuf> mask_effect_grey(Material &ma, const int16_t filter)
{
  Vector<PaintMaterialCompositeImageLayer> layers;
  if (!BKE_paint_layers_composite_image_layers(ma, kColor, layers)) {
    return {};
  }
  for (const PaintMaterialCompositeImageLayer &layer : layers) {
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      if (item.effect_filter == filter) {
        return item.grey_buffer;
      }
    }
  }
  return {};
}

/** The grey the mask of \a row has built up to \a effect: the effect's input, canvas grey at the
 * row's dimensions, the same render the cache's own compute fills from. */
std::shared_ptr<ImBuf> mask_effect_input_grey(Material &ma,
                                              MaterialPaintLayer &row,
                                              MaterialPaintLayer &effect)
{
  int width = 0;
  int height = 0;
  if (!BKE_paint_layers_row_dimensions(
          ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, width, height) ||
      width <= 0 || height <= 0)
  {
    return {};
  }
  ImBuf *grey = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (grey == nullptr) {
    return {};
  }
  PaintLayersMaskSampler sampler(ma, row, &effect.marker, false);
  sampler.tile_acquire(0, width, height);
  float *pixels = grey->float_data_for_write();
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      const float value = sampler.sample(0, x, y);
      const int64_t index = int64_t(y) * width + x;
      pixels[index * 4 + 0] = value;
      pixels[index * 4 + 1] = value;
      pixels[index * 4 + 2] = value;
      pixels[index * 4 + 3] = 1.0f;
    }
  }
  return std::shared_ptr<ImBuf>(grey, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

/** One clamped box pass of \a radius over the grey plane \a values, along \a x or \a y. */
void mask_blur_hand_pass(Vector<float> &values,
                         const int width,
                         const int height,
                         const int radius,
                         const bool along_x)
{
  const int run = along_x ? width : height;
  const int lines = along_x ? height : width;
  Vector<float> original(values.size());
  for (const int line : IndexRange(lines)) {
    for (const int i : IndexRange(run)) {
      const int index = along_x ? line * width + i : i * width + line;
      original[index] = values[index];
    }
  }
  for (const int line : IndexRange(lines)) {
    for (const int i : IndexRange(run)) {
      const int lo = std::max(i - radius, 0);
      const int hi = std::min(i + radius, run - 1);
      float sum = 0.0f;
      for (int k = lo; k <= hi; k++) {
        sum += original[along_x ? line * width + k : k * width + line];
      }
      values[along_x ? line * width + i : i * width + line] = sum / float(hi - lo + 1);
    }
  }
}

/** Whether any texel of the two grey buffers differs past \a tolerance. */
bool mask_grey_differs(const ImBuf &a, const ImBuf &b, const float tolerance)
{
  if (a.x != b.x || a.y != b.y) {
    return true;
  }
  for (const int64_t i : IndexRange(int64_t(a.x) * a.y)) {
    if (std::abs(a.float_data()[i * 4] - b.float_data()[i * 4]) > tolerance) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_F(PaintLayersEffectTest, a_mask_blur_is_the_box_blur_of_the_mask_built_up_to_it)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *blur = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(laid, nullptr);
  ASSERT_EQ(laid->x, 8);
  ASSERT_EQ(laid->y, 8);

  /* The hand box blur of the input, one clamped pass per axis: the maths the kernel runs. */
  std::shared_ptr<ImBuf> input = mask_effect_input_grey(*ma, *row, *blur);
  ASSERT_NE(input, nullptr);
  Vector<float> hand(64);
  for (const int i : IndexRange(64)) {
    hand[i] = input->float_data()[i * 4];
  }
  mask_blur_hand_pass(hand, 8, 8, 1, true);
  mask_blur_hand_pass(hand, 8, 8, 1, false);

  const float *laid_px = laid->float_data();
  for (const int i : IndexRange(64)) {
    EXPECT_NEAR(laid_px[i * 4], hand[i], 1e-5f) << "texel " << i;
    /* A grey in, a grey out: the item lays one scalar, and an opaque one. */
    EXPECT_NEAR(laid_px[i * 4 + 0], laid_px[i * 4 + 1], 1e-6f) << "texel " << i;
    EXPECT_NEAR(laid_px[i * 4 + 1], laid_px[i * 4 + 2], 1e-6f) << "texel " << i;
    EXPECT_NEAR(laid_px[i * 4 + 3], 1.0f, 1e-6f) << "texel " << i;
  }
  /* The ramp's left edge is a real edge: the blur moved it. */
  EXPECT_TRUE(mask_grey_differs(*laid, *input, 1e-6f)) << "a blur of a ramp is not the ramp";
}

TEST_F(PaintLayersEffectTest, a_monochrome_mask_noise_lays_the_kernel_over_the_grey)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *noise = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(
      paint_layer_kind_param_bool_set(*noise, PAINT_LAYER_NOISE_MONOCHROME, true));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_NE(laid, nullptr);
  std::shared_ptr<ImBuf> input = mask_effect_input_grey(*ma, *row, *noise);
  ASSERT_NE(input, nullptr);

  /* The kernel on a private copy of the input is the whole difference: the cache renders the same
   * grey and applies the same grain. */
  std::shared_ptr<ImBuf> expected = paint_layer_anchor_slice_buffer_clone(*input);
  ASSERT_NE(expected, nullptr);
  paint_layer_effect_kernel_apply(*expected, *noise);

  const float *laid_px = laid->float_data();
  const float *expected_px = expected->float_data();
  for (const int i : IndexRange(64)) {
    for (const int k : IndexRange(4)) {
      EXPECT_NEAR(laid_px[i * 4 + k], expected_px[i * 4 + k], 1e-6f)
          << "texel " << i << " ch " << k;
    }
    /* A monochrome grain is one delta on all three channels: the result stays grey. */
    EXPECT_NEAR(laid_px[i * 4 + 0], laid_px[i * 4 + 1], 1e-6f) << "texel " << i;
    EXPECT_NEAR(laid_px[i * 4 + 1], laid_px[i * 4 + 2], 1e-6f) << "texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, a_mask_effect_reads_the_grey_the_effects_before_it_lay)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *blur = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));
  MaterialPaintLayer *noise = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_TRUE(paint_layer_kind_param_bool_set(*noise, PAINT_LAYER_NOISE_MONOCHROME, true));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*noise, PAINT_LAYER_NOISE_SEED, 5));

  std::shared_ptr<ImBuf> laid_blur = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(laid_blur, nullptr);
  std::shared_ptr<ImBuf> laid_noise = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_NE(laid_noise, nullptr);

  /* The noise's input is the blur's laid grey, at full strength and un-inverted by default, so the
   * kernel over a copy of it is the whole difference. */
  std::shared_ptr<ImBuf> expected = paint_layer_anchor_slice_buffer_clone(*laid_blur);
  ASSERT_NE(expected, nullptr);
  paint_layer_effect_kernel_apply(*expected, *noise);
  const float *laid_px = laid_noise->float_data();
  const float *expected_px = expected->float_data();
  for (const int i : IndexRange(64)) {
    for (const int k : IndexRange(4)) {
      EXPECT_NEAR(laid_px[i * 4 + k], expected_px[i * 4 + k], 1e-6f)
          << "texel " << i << " ch " << k;
    }
  }

  /* A parameter of the blur moves the grey the noise reads: the chain fingerprint moves the noise's
   * key, so the entry re-renders instead of serving the grain of the old input. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2));
  std::shared_ptr<ImBuf> moved = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_NOISE);
  ASSERT_NE(moved, nullptr);
  EXPECT_TRUE(mask_grey_differs(*laid_noise, *moved, 1e-6f))
      << "the noise follows the blur it reads";
}

TEST_F(PaintLayersEffectTest, editing_a_mask_effects_parameters_replaces_its_entry)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *blur = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(laid, nullptr);
  ASSERT_EQ(paint_layer_mask_effect_cache_entry_count(), 1);

  /* A wider radius is a different effect: the entry is replaced, not added to. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 3));
  std::shared_ptr<ImBuf> moved = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(paint_layer_mask_effect_cache_entry_count(), 1)
      << "the new parameters replaced the entry, they did not add one";
  EXPECT_TRUE(mask_grey_differs(*laid, *moved, 1e-6f)) << "a wider blur is a different grey";
}

TEST_F(PaintLayersEffectTest,
       an_opacity_edit_of_an_earlier_mask_item_recomputes_the_mask_effects_grey)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *item = BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask"));
  ASSERT_NE(item, nullptr);
  MaterialPaintLayer *blur = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(laid, nullptr);

  /* The grey carries no pixel version of an opacity: the key has to fold the item's settings in, or
   * the effect keeps serving the grey of the old input. */
  item->opacity = 0.25f;
  std::shared_ptr<ImBuf> moved = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(moved, nullptr);
  EXPECT_TRUE(mask_grey_differs(*laid, *moved, 1e-6f)) << "the blur follows the opacity it reads";
}

TEST_F(PaintLayersEffectTest,
       an_opacity_edit_of_the_mask_effect_itself_does_not_move_its_bake_hash)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  MaterialPaintLayer *item = BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask"));
  ASSERT_NE(item, nullptr);
  MaterialPaintLayer *blur = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));

  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*ma, *blur, hash);

  /* The grey is cached before the item's own opacity: the reader lays that live over the map, so
   * the bake has no reason to run again for it. */
  blur->opacity = 0.25f;
  uint32_t moved[2];
  BKE_paint_layers_bake_hash(*ma, *blur, moved);
  EXPECT_EQ(hash[0], moved[0]);
  EXPECT_EQ(hash[1], moved[1]);

  /* An earlier item's opacity is part of what the grey reads, so it does move the hash. */
  item->opacity = 0.5f;
  BKE_paint_layers_bake_hash(*ma, *blur, moved);
  EXPECT_TRUE(hash[0] != moved[0] || hash[1] != moved[1])
      << "the bake follows the input the grey reads";
}

TEST_F(PaintLayersEffectTest, every_mask_effect_kind_lays_one_grey)
{
  /* Every NEEDS_BAKE kind the mask takes: a grey in must lay a grey out. The kernels that keep
   * the three channels equal do it themselves; the ones that can colorize are brought back through
   * the luminance, and the kind table's #PAINT_LAYER_KIND_MASK_NEEDS_LUMA says which. */
  const eMaterialPaintLayerFilter filters[] = {MA_PAINT_LAYER_FILTER_BLUR,
                                               MA_PAINT_LAYER_FILTER_SHARPEN,
                                               MA_PAINT_LAYER_FILTER_HIGH_PASS,
                                               MA_PAINT_LAYER_FILTER_EDGE_DETECT,
                                               MA_PAINT_LAYER_FILTER_EMBOSS,
                                               MA_PAINT_LAYER_FILTER_ERODE_DILATE,
                                               MA_PAINT_LAYER_FILTER_GLOW,
                                               MA_PAINT_LAYER_FILTER_BEVEL,
                                               MA_PAINT_LAYER_FILTER_COLOR_MATCH,
                                               MA_PAINT_LAYER_FILTER_NOISE};
  for (const eMaterialPaintLayerFilter filter : filters) {
    MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
    ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
    add_mask_effect(*ma, *row, filter);

    std::shared_ptr<ImBuf> grey = mask_effect_grey(*ma, filter);
    ASSERT_NE(grey, nullptr) << "the kind lays a grey, filter " << filter;
    for (const int64_t i : IndexRange(int64_t(grey->x) * grey->y)) {
      EXPECT_NEAR(grey->float_data()[i * 4 + 0], grey->float_data()[i * 4 + 1], 1e-6f)
          << "the red and the green agree, filter " << filter << " texel " << i;
      EXPECT_NEAR(grey->float_data()[i * 4 + 1], grey->float_data()[i * 4 + 2], 1e-6f)
          << "the green and the blue agree, filter " << filter << " texel " << i;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_stroke_into_the_base_mask_recomputes_the_mask_effects_grey)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  Image *mask_map = add_ramp_image("Mask");
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *mask_map), nullptr);
  MaterialPaintLayer *blur = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, PAINT_LAYER_BLUR_BOX));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 1));

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(laid, nullptr);
  ASSERT_EQ(paint_layer_mask_effect_cache_entry_count(), 1);

  /* The stroke: the base mask's map says its pixels changed, and the effect reads that map, so the
   * grey is stale and the describe recomputes it into the same entry. */
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(mask_map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(8)) {
      uchar *p = pixels + (int64_t(y) * 8) * 4;
      p[0] = 255;
      p[1] = 255;
      p[2] = 255;
      p[3] = 255;
    }
    BKE_image_release_ibuf(mask_map, ibuf, lock);
  }
  BKE_image_partial_update_mark_full_update(mask_map);
  std::shared_ptr<ImBuf> moved = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(paint_layer_mask_effect_cache_entry_count(), 1)
      << "the stroke replaced the entry, it did not add one";
  EXPECT_TRUE(mask_grey_differs(*laid, *moved, 1e-6f)) << "the grey follows the map it reads";
}

TEST_F(PaintLayersEffectTest, the_combined_preview_refuses_a_stack_with_a_mask_effect)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BLUR);

  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  bool found = false;
  for (const PaintMaterialCompositeImageLayer &layer : layers) {
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      if (item.effect_filter == MA_PAINT_LAYER_FILTER_BLUR) {
        found = true;
        EXPECT_EQ(item.grey_buffer, nullptr) << "describe-only computes no grey";
      }
    }
  }
  ASSERT_TRUE(found);
  const char *reason = BKE_paint_layers_gpu_unsupported_reason(layers[0], false);
  EXPECT_NE(reason, nullptr) << "the GPU path refuses the stack instead of shading a no-op";
}

/** \} */

}  // namespace blender::bke::tests
