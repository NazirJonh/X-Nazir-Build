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
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_math_color.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"

#include "paint_layers_generate_test_fixture.hh"

#include "BKE_paint_layers_composite.hh"
#include "BKE_wm_runtime.hh"

#include "BLI_set.hh"

#include "IMB_colormanagement.hh"
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

  /** An 8x8 byte image of a closed ring: opaque red on the square the texels 1 to 5 trace, and no
   * alpha on the hole it closes (2 to 4) and on the map around it. */
  Image *add_ring_image(const char *name)
  {
    const int size = 8;
    const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const bool on_ring = x >= 1 && x <= 5 && y >= 1 && y <= 5 &&
                             (x == 1 || x == 5 || y == 1 || y == 5);
        uchar *p = pixels + (int64_t(y) * size + x) * 4;
        p[0] = on_ring ? 255 : 0;
        p[1] = 0;
        p[2] = 0;
        p[3] = on_ring ? 255 : 0;
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

  /** An opaque 8x8 Non-Color normal map split down the middle: the left half is the flat tangent
   * normal (a blue map), the right half carries the encoded components given. The input a Normal
   * to Mask reads. */
  Image *add_normal_image(const char *name,
                          const uchar right_r,
                          const uchar right_g,
                          const uchar right_b)
  {
    const int size = 8;
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        uchar *p = pixels + (int64_t(y) * size + x) * 4;
        p[0] = (x < size / 2) ? 128 : right_r;
        p[1] = (x < size / 2) ? 128 : right_g;
        p[2] = (x < size / 2) ? 255 : right_b;
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

  /** An opaque 8x8 byte image whose channels are distinct: red 25, green 128, blue 204, and an
   * alpha of 255 on the left half and 64 on the right one -- one split can tell the four apart.
   * The kernel reads the slot as data, so the bytes answer as they were written whatever the
   * image's colorspace. */
  Image *add_channel_image(const char *name)
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
        p[0] = 25;
        p[1] = 128;
        p[2] = 204;
        p[3] = x >= size / 2 ? 64 : 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** The composite of Base Color as straight scene-linear RGBA at the stack's dimensions. */
  bool render_channel(Vector<float> &r_pixels, int &r_width, int &r_height)
  {
    return render_channel_on_tile(kColor, PAINT_TILE_NUMBER_BASE, r_pixels, r_width, r_height);
  }

  /** The composite of \a channel on UDIM \a tile_number as straight scene-linear RGBA. */
  bool render_channel_on_tile(const eMaterialPaintChannel channel,
                              const int tile_number,
                              Vector<float> &r_pixels,
                              int &r_width,
                              int &r_height)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    if (!BKE_paint_layers_composite_image_layers(
            *ma, channel, layers, false, nullptr, nullptr, tile_number))
    {
      return false;
    }
    if (!BKE_paint_material_composite_stack_dimensions(layers, r_width, r_height) ||
        r_width <= 0 || r_height <= 0)
    {
      return false;
    }
    r_pixels.resize(int64_t(r_width) * r_height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(channel, bottom);
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

TEST_F(PaintLayersEffectTest, a_blur_directional_of_a_solid_row_is_the_solid)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  /* The line average of a flat colour is the flat colour, whichever way the angle points. */
  add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL);
  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "a directional blur of a flat input is the "
                                                "input, texel "
                                             << i;
  }
}

TEST_F(PaintLayersEffectTest, a_blur_directional_streaks_along_the_angle)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL);
  /* The angle stays at its zero default: the line runs along the map's +X, so a row of the
   * vertical edge is the line the kernel averages. A radius of two at the zero texel walks
   * clamped into the white side only, at the middle it crosses the edge, and past it into the
   * black. The intensity reads one, so the average is what the texel shows. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS, 2));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 2e-3f) << "the clamped window [0, 2] "
                                                                "reads white only";
  EXPECT_NEAR(linear_pixel(pixels, width, 1, 4), 1.0f, 2e-3f) << "the window [0, 3] is white "
                                                                "only";
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 4), 0.8f, 2e-3f) << "the window [0, 4] reads four "
                                                                "white of five";
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 0.6f, 2e-3f) << "the window [1, 5] reads three "
                                                                "white of five";
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 0.4f, 2e-3f) << "the window [2, 6] reads two "
                                                                "white of five";
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 4), 0.0f, 2e-3f) << "the window [4, 6] is black "
                                                                "only";
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f) << "the clamped window [5, 7] "
                                                                "reads black only";
}

TEST_F(PaintLayersEffectTest, a_blur_directional_sweeps_the_angles_at_the_map_edges)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0, true));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS, 3));

  /* Every angle the slider can name walks the same clamped line: the samples stay inside the map,
   * so no texel becomes a value the map never held and none leaves the colour range. */
  for (const float angle : {0.0f, 45.0f, 90.0f, 135.0f}) {
    ASSERT_TRUE(paint_layer_kind_param_set(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_ANGLE, angle));
    Vector<float> pixels;
    int width = 0;
    int height = 0;
    ASSERT_TRUE(render_channel(pixels, width, height)) << "angle " << angle;
    for (int64_t i = 0; i < pixels.size(); i++) {
      EXPECT_TRUE(std::isfinite(pixels[i])) << "angle " << angle << " texel " << i;
      EXPECT_GE(pixels[i], -1e-4f) << "angle " << angle << " texel " << i;
      EXPECT_LE(pixels[i], 1.0f + 1e-4f) << "angle " << angle << " texel " << i;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_of_a_flat_normal_is_black)
{
  /* A flat normal carries no field: the Height has no divergence to integrate, the Edges no
   * gradient, the Slope no tilt. Whatever the mode, the cut reads zero everywhere. */
  MaterialPaintLayer *row = add_paint_layer("Row",
                                            add_normal_image("Row", 128, 128, 255),
                                            PAINT_MATERIAL_CHANNEL_NORMAL);
  /* An empty base: a flat value with no alpha lays nothing of its own, so the Normal the effect
   * reads sizes the row and the effect's grey is all that covers it. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  add_constant_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, black);
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
  for (const int mode :
       {PAINT_LAYER_NORMAL_TO_MASK_HEIGHT,
        PAINT_LAYER_NORMAL_TO_MASK_EDGES,
        PAINT_LAYER_NORMAL_TO_MASK_SLOPE})
  {
    ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_NORMAL_TO_MASK_MODE, mode));
    Vector<float> pixels;
    int width = 0;
    int height = 0;
    ASSERT_TRUE(render_channel(pixels, width, height)) << "mode " << mode;
    ASSERT_EQ(pixels.size(), int64_t(width) * height * 4);
    for (int64_t i = 0; i < pixels.size(); i += 4) {
      EXPECT_NEAR(pixels[i], 0.0f, 2e-3f) << "mode " << mode << " texel " << i / 4;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_slope_splits_the_tilted_half)
{
  /* The Slope field is the blue channel's tilt: the flat left half reads zero, the right one --
   * whose normal points along the surface, `z = -1` -- reads one. The cut at the middle separates
   * them without touching the seam. */
  MaterialPaintLayer *row = add_paint_layer("Row",
                                            add_normal_image("Row", 128, 128, 0),
                                            PAINT_MATERIAL_CHANNEL_NORMAL);
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  add_constant_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, black);
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect,
                                             PAINT_LAYER_NORMAL_TO_MASK_MODE,
                                             PAINT_LAYER_NORMAL_TO_MASK_SLOPE));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    EXPECT_NEAR(linear_pixel(pixels, width, 1, y), 0.0f, 2e-3f)
        << "the flat half has no tilt, texel row " << y;
    EXPECT_NEAR(linear_pixel(pixels, width, 6, y), 1.0f, 2e-3f)
        << "the tilted half reads the full slope, texel row " << y;
  }
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_edges_lights_the_seam)
{
  /* Two flat regions of different tilt: the normal's gradient lives at the seam alone, so the
   * Edges field lights the seam column and leaves both interiors dark. */
  MaterialPaintLayer *row = add_paint_layer("Row",
                                            add_normal_image("Row", 255, 128, 128),
                                            PAINT_MATERIAL_CHANNEL_NORMAL);
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  add_constant_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, black);
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect,
                                             PAINT_LAYER_NORMAL_TO_MASK_MODE,
                                             PAINT_LAYER_NORMAL_TO_MASK_EDGES));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    EXPECT_NEAR(linear_pixel(pixels, width, 1, y), 0.0f, 2e-3f)
        << "the flat interior has no gradient, texel row " << y;
    EXPECT_GT(linear_pixel(pixels, width, 4, y), 0.5f)
        << "the seam carries the gradient, texel row " << y;
  }
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_height_spans_the_tilt_step)
{
  /* The Height integrates the normal's gradients: a step from the flat tilt to the tilted one
   * becomes a field that walks from zero to one across it, so the cut splits the halves the way
   * the Slope does -- through the solve, not through the blue channel. */
  MaterialPaintLayer *row = add_paint_layer("Row",
                                            add_normal_image("Row", 255, 128, 128),
                                            PAINT_MATERIAL_CHANNEL_NORMAL);
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  add_constant_channel(*row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, black);
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect,
                                             PAINT_LAYER_NORMAL_TO_MASK_MODE,
                                             PAINT_LAYER_NORMAL_TO_MASK_HEIGHT));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    EXPECT_LT(linear_pixel(pixels, width, 1, y), 0.35f)
        << "the flat side stays under the cut, texel row " << y;
    EXPECT_GT(linear_pixel(pixels, width, 6, y), 0.65f)
        << "the tilted side rises over it, texel row " << y;
  }
}

/* -------------------------------------------------------------------- */
/** \name Normal to Mask field and cut
 *
 * The kernel is split into the solve (the field, one call per mode change) and the cut (the ramp,
 * one call per repaint), and the preview session caches the first. These tests drive the two
 * halves directly over hand-made buffers, the way the session does.
 * \{ */

/** A \a width x \a height float RGBA buffer whose Normal channel is the encoded map the kernel
 * reads: flat blue on the left half, the encoded right half given. Alpha is opaque. */
static std::shared_ptr<ImBuf> make_normal_slice(
    const int width, const int height, const float right_r, const float right_g, const float right_b)
{
  ImBuf *ibuf = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    return {};
  }
  float *pixels = ibuf->float_data_for_write();
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      const bool right_half = x >= width / 2;
      float *texel = pixels + (int64_t(y) * width + x) * 4;
      texel[0] = right_half ? right_r : 0.5f;
      texel[1] = right_half ? right_g : 0.5f;
      texel[2] = right_half ? right_b : 1.0f;
      texel[3] = 1.0f;
    }
  }
  return std::shared_ptr<ImBuf>(ibuf, [](ImBuf *buffer) { IMB_freeImBuf(buffer); });
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_field_ignores_the_cut)
{
  /* The solve answers to the mode alone: a threshold or a softness that moves may not move one
   * value of the field, or the cached solve of a preview would drift from a re-solved one. */
  std::shared_ptr<ImBuf> slice = make_normal_slice(8, 8, 0.5f, 0.5f, 0.0f);
  ASSERT_NE(slice, nullptr);
  PaintLayerNormalToMaskParams params;
  params.mode = PAINT_LAYER_NORMAL_TO_MASK_SLOPE;
  params.threshold = 0.2f;
  params.softness = 0.05f;
  Vector<float> field_a;
  float lo_a = 0.0f;
  float hi_a = 1.0f;
  BKE_paint_layers_normal_to_mask_field_compute(*slice, params, field_a, lo_a, hi_a);
  ASSERT_EQ(field_a.size(), int64_t(8) * 8);
  params.threshold = 0.9f;
  params.softness = 0.8f;
  Vector<float> field_b;
  float lo_b = 0.0f;
  float hi_b = 1.0f;
  BKE_paint_layers_normal_to_mask_field_compute(*slice, params, field_b, lo_b, hi_b);
  ASSERT_EQ(field_b.size(), field_a.size());
  for (const int64_t i : field_a.index_range()) {
    EXPECT_FLOAT_EQ(field_b[i], field_a[i]) << "texel " << i;
  }
  EXPECT_FLOAT_EQ(lo_b, lo_a);
  EXPECT_FLOAT_EQ(hi_b, hi_a);

  /* The cut is what reads the two: the flat half stays dark, the tilted one lights, at the ramp
   * the narrow softness lays. */
  params.threshold = 0.2f;
  params.softness = 0.05f;
  ImBuf *cut = IMB_dupImBuf(slice.get());
  ASSERT_NE(cut, nullptr);
  BKE_paint_layers_normal_to_mask_cut_apply(*cut, field_a, lo_a, hi_a, params);
  const float *px = cut->float_data();
  ASSERT_NE(px, nullptr);
  for (const int y : IndexRange(8)) {
    EXPECT_NEAR(px[int64_t(y) * 8 * 4 + 1 * 4], 0.0f, 1e-3f)
        << "the flat half cuts black, texel row " << y;
    EXPECT_NEAR(px[int64_t(y) * 8 * 4 + 6 * 4], 1.0f, 1e-3f)
        << "the tilted half cuts white, texel row " << y;
    EXPECT_EQ(px[int64_t(y) * 8 * 4 + 3], 1.0f) << "the cut leaves the alpha alone";
  }
  IMB_freeImBuf(cut);
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_field_cuts_a_broken_normal_as_black)
{
  /* A NaN in the normal map stands out of every window and every cut: the field stays finite, the
   * window lands on the finite values, and the broken texel's grey is black, not the NaN the raw
   * arithmetic would write into the mask. */
  std::shared_ptr<ImBuf> slice = make_normal_slice(8, 8, 0.5f, 0.5f, 0.0f);
  ASSERT_NE(slice, nullptr);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  slice->float_data_for_write()[(int64_t(3) * 8 + 3) * 4 + 2] = nan;
  slice->float_data_for_write()[(int64_t(3) * 8 + 3) * 4 + 0] = nan;
  PaintLayerNormalToMaskParams params;
  params.mode = PAINT_LAYER_NORMAL_TO_MASK_SLOPE;
  Vector<float> field;
  float lo = 0.0f;
  float hi = 1.0f;
  BKE_paint_layers_normal_to_mask_field_compute(*slice, params, field, lo, hi);
  ASSERT_EQ(field.size(), int64_t(8) * 8);
  EXPECT_TRUE(std::isfinite(lo)) << "the window lands on the finite values";
  EXPECT_TRUE(std::isfinite(hi)) << "the window lands on the finite values";
  for (const int64_t i : field.index_range()) {
    EXPECT_TRUE(std::isfinite(field[i])) << "texel " << i;
  }
  ImBuf *cut = IMB_dupImBuf(slice.get());
  ASSERT_NE(cut, nullptr);
  BKE_paint_layers_normal_to_mask_cut_apply(*cut, field, lo, hi, params);
  const float *px = cut->float_data();
  ASSERT_NE(px, nullptr);
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_TRUE(std::isfinite(px[i * 4])) << "texel " << i;
    EXPECT_TRUE(std::isfinite(px[i * 4 + 1])) << "texel " << i;
    EXPECT_TRUE(std::isfinite(px[i * 4 + 2])) << "texel " << i;
  }
  EXPECT_NEAR(px[(int64_t(3) * 8 + 3) * 4], 0.0f, 1e-3f)
      << "the broken texel cuts as no signal";
  IMB_freeImBuf(cut);
}

TEST_F(PaintLayersEffectTest, a_normal_to_mask_height_solves_odd_canvases)
{
  /* The pyramid coarsens only while both sides stay past its floor: a one-wide or one-high canvas
   * and a small odd one solve whole, every texel finite, the clamped reads keeping the shape the
   * size names. */
  for (const int64_t size_wh[3][2] = {{1, 9}, {9, 1}, {3, 5}};
       const auto &side : size_wh)
  {
    std::shared_ptr<ImBuf> slice = make_normal_slice(
        int(side[0]), int(side[1]), 0.5f, 0.5f, 0.0f);
    ASSERT_NE(slice, nullptr) << side[0] << "x" << side[1];
    PaintLayerNormalToMaskParams params;
    params.mode = PAINT_LAYER_NORMAL_TO_MASK_HEIGHT;
    Vector<float> field;
    float lo = 0.0f;
    float hi = 1.0f;
    BKE_paint_layers_normal_to_mask_field_compute(*slice, params, field, lo, hi);
    ASSERT_EQ(field.size(), side[0] * side[1]) << side[0] << "x" << side[1];
    EXPECT_TRUE(std::isfinite(lo)) << side[0] << "x" << side[1];
    EXPECT_TRUE(std::isfinite(hi)) << side[0] << "x" << side[1];
    for (const int64_t i : field.index_range()) {
      EXPECT_TRUE(std::isfinite(field[i])) << side[0] << "x" << side[1] << " texel " << i;
    }
  }
}

/** \} */

TEST_F(PaintLayersEffectTest, a_drop_shadow_falls_toward_the_angle_off_the_luminance)
{
  /* Left half white, right half black, all opaque: the luminance source reads the white side as
   * the shape that casts, and the offset silhouette darkens the white it lands on even there,
   * which the alpha source would never do. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *shadow = add_effect(*row, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  /* The shape from the luminance, the cast toward the +X the zero angle names, two texels of it,
   * no blur and no spread, the shadow read at full strength in the black it defaults to. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow,
                                             PAINT_LAYER_DROP_SHADOW_SOURCE,
                                             PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE));
  ASSERT_TRUE(paint_layer_kind_param_set(*shadow, PAINT_LAYER_DROP_SHADOW_ANGLE, 0.0f));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow, PAINT_LAYER_DROP_SHADOW_DISTANCE, 2));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow, PAINT_LAYER_DROP_SHADOW_SIZE, 0));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  /* The two columns before the silhouette has reached keep the white; the two the offset lands
   * the silhouette on turn to the shadow, opaque content and all; the black side reads nothing to
   * cast and stays black. */
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 1, 4), 1.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 5, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 0.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, the_fill_area_debug_lays_the_regions_the_border_leaves)
{
  /* Left half white, right half black, all opaque: the luminance source reads the white side as
   * the border, the black side the one region the fill closes against it. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);
  ASSERT_TRUE(paint_layer_kind_param_int_set(
      *fill, PAINT_LAYER_FILL_AREA_SOURCE, PAINT_LAYER_FILL_AREA_SOURCE_LUMINANCE));
  ASSERT_TRUE(paint_layer_kind_param_bool_set(*fill, PAINT_LAYER_FILL_AREA_DEBUG, true));

  /* The debug lays the regions themselves, opaque: the border reads zero, the one region reads the
   * ramp's full one. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 0, 4), 0.0f, 2e-3f) << "the border reads zero";
  EXPECT_NEAR(linear_pixel(pixels, width, 3, 4), 0.0f, 2e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 4, 4), 1.0f, 2e-3f) << "the one region reads one";
  EXPECT_NEAR(linear_pixel(pixels, width, 7, 4), 1.0f, 2e-3f);
}
TEST_F(PaintLayersEffectTest, a_drop_shadow_kernel_lays_a_premultiplied_shadow_layer)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *shadow = add_effect(*row, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  ASSERT_TRUE(paint_layer_kind_param_set(*shadow, PAINT_LAYER_DROP_SHADOW_ANGLE, 0.0f));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow, PAINT_LAYER_DROP_SHADOW_DISTANCE, 2));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow, PAINT_LAYER_DROP_SHADOW_SIZE, 0));
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*shadow, PAINT_LAYER_DROP_SHADOW_COLOR, red));
  ASSERT_TRUE(paint_layer_kind_param_set(*shadow, PAINT_LAYER_DROP_SHADOW_OPACITY, 0.5f));

  /* A white square stroke on the left half, nothing on the right: straight colour, content alpha. */
  constexpr int size = 8;
  ImBuf *input = IMB_allocImBuf(size, size, ImBufFlags::FloatData);
  ASSERT_NE(input, nullptr);
  float *pixels = input->float_data_for_write();
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      float *p = pixels + (int64_t(y) * size + x) * 4;
      const float covered = x < size / 2 ? 1.0f : 0.0f;
      p[0] = covered;
      p[1] = covered;
      p[2] = covered;
      p[3] = covered;
    }
  }
  paint_layer_effect_kernel_apply(*input, *shadow, false, PAINT_TILE_NUMBER_BASE);

  /* The shadow falls two texels to the right of the stroke: where the content is, the layer has no
   * alpha ("under the content" is part of the weight); on the empty map next to it the alpha is the
   * opacity and the colour is the shadow's own, premultiplied; past the offset silhouette nothing. */
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const float *p = pixels + (int64_t(y) * size + x) * 4;
      const bool shadowed = x == 4 || x == 5;
      EXPECT_NEAR(p[3], shadowed ? 0.5f : 0.0f, 1e-6f) << "alpha at " << x << "," << y;
      EXPECT_NEAR(p[0], shadowed ? 0.5f : 0.0f, 1e-6f) << "premultiplied red at " << x << "," << y;
      EXPECT_NEAR(p[1], 0.0f, 1e-6f) << "green at " << x << "," << y;
      EXPECT_NEAR(p[2], 0.0f, 1e-6f) << "blue at " << x << "," << y;
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_drop_shadow_shows_on_the_map_around_a_stroke)
{
  /* The left half is an opaque white stroke, the right half has no alpha: the shadow is cast onto
   * texels the row covers nothing at, which a colour-only result could never show. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0, false, 0));
  MaterialPaintLayer *shadow = add_effect(*row, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  ASSERT_TRUE(paint_layer_kind_param_set(*shadow, PAINT_LAYER_DROP_SHADOW_ANGLE, 0.0f));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow, PAINT_LAYER_DROP_SHADOW_DISTANCE, 2));
  ASSERT_TRUE(paint_layer_kind_param_int_set(*shadow, PAINT_LAYER_DROP_SHADOW_SIZE, 0));
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*shadow, PAINT_LAYER_DROP_SHADOW_COLOR, red));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  const auto texel = [&](const int x, const int y, const int channel) {
    return pixels[(int64_t(y) * width + x) * 4 + channel];
  };
  /* The stroke keeps its white, the two texels the offset lands on turn the shadow's red where the
   * row had no coverage, and past them the map is as empty as it was. */
  EXPECT_NEAR(texel(1, 4, 0), 1.0f, 2e-3f);
  EXPECT_NEAR(texel(1, 4, 1), 1.0f, 2e-3f);
  for (const int x : {4, 5}) {
    EXPECT_NEAR(texel(x, 4, 0), 1.0f, 2e-3f) << "red at " << x;
    EXPECT_NEAR(texel(x, 4, 1), 0.0f, 2e-3f) << "green at " << x;
    EXPECT_NEAR(texel(x, 4, 2), 0.0f, 2e-3f) << "blue at " << x;
  }
  EXPECT_FALSE(texel(7, 4, 0) > 0.99f && texel(7, 4, 1) < 0.01f)
      << "no shadow past the offset silhouette";
}

/** An 8x8 float buffer whose red (the Height the kernel reads) rises by \a step per texel along x,
 * or along y (the rows, which run toward +V) when \a along_y. */
static ImBuf *height_ramp_buffer(const float step, const bool along_y, const float alpha = 1.0f)
{
  constexpr int size = 8;
  ImBuf *input = IMB_allocImBuf(size, size, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      float *p = pixels + (int64_t(y) * size + x) * 4;
      p[0] = step * float(along_y ? y : x);
      p[1] = p[0];
      p[2] = p[0];
      p[3] = alpha;
    }
  }
  return input;
}

TEST_F(PaintLayersEffectTest, a_height_to_normal_kernel_lays_the_neutral_normal_on_a_flat_height)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL);

  ImBuf *input = height_ramp_buffer(0.0f, false, 0.75f);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *pixels = input->float_data();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_NEAR(pixels[i * 4 + 0], 0.5f, 1e-5f);
    EXPECT_NEAR(pixels[i * 4 + 1], 0.5f, 1e-5f);
    EXPECT_NEAR(pixels[i * 4 + 2], 1.0f, 1e-5f) << "the up vector is encoded one";
    EXPECT_FLOAT_EQ(pixels[i * 4 + 3], 0.75f) << "the alpha is untouched";
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_height_to_normal_kernel_leans_the_normal_against_the_slope)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL);

  /* A ramp rising along +U: the normal leans toward -U (red under the half), green stays at the
   * half, and the vector is unit length. The ramp is read in the middle, away from the clamped
   * edges. */
  ImBuf *along_u = height_ramp_buffer(0.1f, false);
  paint_layer_effect_kernel_apply(*along_u, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *p = along_u->float_data() + (int64_t(4) * 8 + 4) * 4;
  EXPECT_LT(p[0], 0.5f - 1e-2f);
  EXPECT_NEAR(p[1], 0.5f, 1e-5f);
  const float nx = p[0] * 2.0f - 1.0f;
  const float ny = p[1] * 2.0f - 1.0f;
  const float nz = p[2] * 2.0f - 1.0f;
  EXPECT_NEAR(nx * nx + ny * ny + nz * nz, 1.0f, 1e-4f);
  IMB_freeImBuf(along_u);

  /* A ramp rising along +V (the rows): the OpenGL green leans toward -V, the DirectX one flips. */
  ImBuf *along_v = height_ramp_buffer(0.1f, true);
  paint_layer_effect_kernel_apply(*along_v, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *opengl = along_v->float_data() + (int64_t(4) * 8 + 4) * 4;
  EXPECT_LT(opengl[1], 0.5f - 1e-2f);
  EXPECT_NEAR(opengl[0], 0.5f, 1e-5f);
  const float opengl_green = opengl[1];
  IMB_freeImBuf(along_v);

  ASSERT_TRUE(paint_layer_kind_param_bool_set(*effect, PAINT_LAYER_HEIGHT_TO_NORMAL_FLIP_GREEN, true));
  ImBuf *flipped = height_ramp_buffer(0.1f, true);
  paint_layer_effect_kernel_apply(*flipped, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *directx = flipped->float_data() + (int64_t(4) * 8 + 4) * 4;
  EXPECT_NEAR(directx[1], 1.0f - opengl_green, 1e-5f);
  IMB_freeImBuf(flipped);
}

TEST_F(PaintLayersEffectTest, a_height_to_normal_in_world_units_reads_the_texel_size)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL);
  ASSERT_TRUE(paint_layer_kind_param_bool_set(*effect, PAINT_LAYER_HEIGHT_TO_NORMAL_WORLD_UNITS, true));
  /* 8 texels across 8 cm: one centimetre a texel; a height step of 0.1 over a 5 cm depth rises
   * half a centimetre a texel, so the slope is 0.5 and the normal's x is -0.5 / sqrt(1.25). */
  ASSERT_TRUE(paint_layer_kind_param_set(*effect, PAINT_LAYER_HEIGHT_TO_NORMAL_SURFACE_SIZE, 8.0f));
  ASSERT_TRUE(paint_layer_kind_param_set(*effect, PAINT_LAYER_HEIGHT_TO_NORMAL_HEIGHT_DEPTH, 5.0f));

  ImBuf *input = height_ramp_buffer(0.1f, false);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *p = input->float_data() + (int64_t(4) * 8 + 4) * 4;
  EXPECT_NEAR(p[0] * 2.0f - 1.0f, -0.5f / std::sqrt(1.25f), 1e-4f);
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_pixelate_of_a_whole_map_averages_it_into_one_block)
{
  /* Half white, half black: one pixel across the map takes the block average, and the half-and-
   * half average is the half. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *pixelate = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*pixelate, PAINT_LAYER_PIXELATE_AMOUNT, 1));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), 0.5f, 2e-3f)
          << "the one block reads the map's own average, texel " << x << ", " << y;
    }
  }
}

TEST_F(PaintLayersEffectTest, a_pixelate_kernel_cuts_a_ramp_into_blocks_and_keeps_alpha)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 2));

  /* 8 x 8 texels, two blocks a side: blocks of four. Red is the column, so each block of four
   * columns averages to its middle, and the alpha stays a half. */
  ImBuf *input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  ASSERT_NE(input, nullptr);
  float *data = input->float_data_for_write();
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      float *p = data + (int64_t(y) * 8 + x) * 4;
      p[0] = float(x);
      p[1] = 0.0f;
      p[2] = 0.0f;
      p[3] = 0.5f;
    }
  }
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *out = input->float_data();
  for (const int x : IndexRange(8)) {
    const float *p = out + (int64_t(3) * 8 + x) * 4;
    EXPECT_NEAR(p[0], x < 4 ? 1.5f : 5.5f, 1e-5f) << x;
    EXPECT_FLOAT_EQ(p[3], 0.5f);
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_pixelate_kernel_does_not_leak_a_transparent_texels_black_into_an_island)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 1));

  /* One opaque white texel among transparent black ones: the block is white, not a grey. */
  ImBuf *input = IMB_allocImBuf(4, 4, ImBufFlags::FloatData);
  ASSERT_NE(input, nullptr);
  float *data = input->float_data_for_write();
  for (const int i : IndexRange(16)) {
    float *p = data + int64_t(i) * 4;
    const float island = (i == 5) ? 1.0f : 0.0f;
    p[0] = island;
    p[1] = island;
    p[2] = island;
    p[3] = island;
  }
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float *p = input->float_data() + 5 * 4;
  EXPECT_NEAR(p[0], 1.0f, 1e-5f);
  EXPECT_NEAR(p[1], 1.0f, 1e-5f);
  EXPECT_FLOAT_EQ(p[3], 1.0f);
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_pixelate_of_one_texel_blocks_is_the_identity)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 256));

  ImBuf *input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  ASSERT_NE(input, nullptr);
  float *data = input->float_data_for_write();
  for (const int i : IndexRange(64)) {
    float *p = data + int64_t(i) * 4;
    p[0] = float(i) / 64.0f;
    p[1] = 0.25f;
    p[2] = 0.75f;
    p[3] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int i : IndexRange(64)) {
    EXPECT_FLOAT_EQ(input->float_data()[int64_t(i) * 4], float(i) / 64.0f);
  }
  IMB_freeImBuf(input);
}

/** An RGBA float buffer of \a width x \a height, every texel filled by \a fill(x, y, texel). */
template<typename FillFn> static ImBuf *effect_test_buffer(const int width, const int height, FillFn fill)
{
  ImBuf *buffer = IMB_allocImBuf(width, height, ImBufFlags::FloatData);
  if (buffer == nullptr) {
    return nullptr;
  }
  float *data = buffer->float_data_for_write();
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      fill(x, y, data + (int64_t(y) * width + x) * 4);
    }
  }
  return buffer;
}

TEST_F(PaintLayersEffectTest, a_pixelate_on_a_non_square_map_cuts_each_axis_by_its_own_block)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 4));

  /* 16 x 8 with four blocks a side: block_w = 16 / 4 = 4 and block_h = 8 / 4 = 2. Red is
   * `x + 100 * y`, so a block at (bx, by) averages to `(bx + 1.5) + 100 * (by + 0.5)`: the mean of
   * four consecutive columns is the first plus 1.5, the mean of two rows the first plus 0.5. */
  ImBuf *input = effect_test_buffer(16, 8, [](const int x, const int y, float *p) {
    p[0] = float(x) + 100.0f * float(y);
    p[1] = 0.0f;
    p[2] = 0.0f;
    p[3] = 1.0f;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(16)) {
      const float expected = float(x / 4 * 4) + 1.5f + 100.0f * (float(y / 2 * 2) + 0.5f);
      EXPECT_NEAR(input->float_data()[(int64_t(y) * 16 + x) * 4], expected, 1e-3f)
          << "texel " << x << ", " << y;
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_pixelate_last_block_may_be_shorter)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 3));

  /* 10 texels with three blocks a side: block = 10 / 3 = 3, so the blocks are ceil(10 / 3) = 4
   * wide (3, 3, 3 and a last one of a single texel). The block means of 0..9 are 1, 4, 7 and 9;
   * red is `x + 100 * y`, so a texel reads `mean(x block) + 100 * mean(y block)`. */
  ImBuf *input = effect_test_buffer(10, 10, [](const int x, const int y, float *p) {
    p[0] = float(x) + 100.0f * float(y);
    p[1] = 0.0f;
    p[2] = 0.0f;
    p[3] = 1.0f;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  const float means[4] = {1.0f, 4.0f, 7.0f, 9.0f};
  for (const int y : IndexRange(10)) {
    for (const int x : IndexRange(10)) {
      const float expected = means[x / 3] + 100.0f * means[y / 3];
      EXPECT_NEAR(input->float_data()[(int64_t(y) * 10 + x) * 4], expected, 1e-3f)
          << "texel " << x << ", " << y;
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_pixelate_with_big_blocks_finishes_quickly)
{
  /* The guard for the per-texel block sum that hung a 4K map: the cost is the map, not the map
   * times the block. Red is 0.25 left of the middle column and 0.75 right of it, plus 0.5 on the
   * odd rows (all multiples of 0.25, so the float sums are exact). One block: the map's mean is
   * 0.5 + 0.25 = 0.75. Two blocks a side (512 texels): 0.25 + 0.25 = 0.5 left, 0.75 + 0.25 = 1
   * right. */
  const auto make_input = []() {
    return effect_test_buffer(1024, 1024, [](const int x, const int y, float *p) {
      p[0] = (x < 512 ? 0.25f : 0.75f) + ((y % 2) ? 0.5f : 0.0f);
      p[1] = 0.0f;
      p[2] = 0.0f;
      p[3] = 1.0f;
    });
  };
  const auto elapsed_seconds = [](const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };

  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_PIXELATE);

  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 1));
  ImBuf *one_block = make_input();
  ASSERT_NE(one_block, nullptr);
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  paint_layer_effect_kernel_apply(*one_block, *effect, false, PAINT_TILE_NUMBER_BASE);
  EXPECT_LT(elapsed_seconds(start), 5.0) << "one block over a 1024 map must not cost the area per texel";
  for (const int64_t i : {int64_t(0), int64_t(512) * 1024 + 700, int64_t(1024) * 1024 - 1}) {
    EXPECT_NEAR(one_block->float_data()[i * 4], 0.75f, 1e-4f) << i;
  }
  IMB_freeImBuf(one_block);

  ASSERT_TRUE(paint_layer_kind_param_int_set(*effect, PAINT_LAYER_PIXELATE_AMOUNT, 2));
  ImBuf *two_blocks = make_input();
  ASSERT_NE(two_blocks, nullptr);
  start = std::chrono::steady_clock::now();
  paint_layer_effect_kernel_apply(*two_blocks, *effect, false, PAINT_TILE_NUMBER_BASE);
  EXPECT_LT(elapsed_seconds(start), 5.0) << "two blocks a side must not cost the area per texel";
  EXPECT_NEAR(two_blocks->float_data()[(int64_t(100) * 1024 + 100) * 4], 0.5f, 1e-4f);
  EXPECT_NEAR(two_blocks->float_data()[(int64_t(900) * 1024 + 900) * 4], 1.0f, 1e-4f);
  IMB_freeImBuf(two_blocks);
}

TEST_F(PaintLayersEffectTest, an_fxaa_keeps_a_straight_edge)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  /* The vertical edge is one the blend walks along: the two texels the direction takes read the
   * same column, so the jagless edge is its own result. */
  add_effect(*row, MA_PAINT_LAYER_FILTER_FXAA);
  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "an FXAA of a straight edge is the edge, texel "
                                             << i;
  }
}

TEST_F(PaintLayersEffectTest, a_flat_fxaa_input_is_unchanged)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_FXAA);

  /* A flat grey has a luma range of zero, under the edge threshold: every ring reads as it is. */
  ImBuf *input = effect_test_buffer(8, 8, [](const int /*x*/, const int /*y*/, float *p) {
    p[0] = 0.5f;
    p[1] = 0.5f;
    p[2] = 0.5f;
    p[3] = 0.7f;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int i : IndexRange(64)) {
    const float *p = input->float_data() + int64_t(i) * 4;
    EXPECT_FLOAT_EQ(p[0], 0.5f) << i;
    EXPECT_FLOAT_EQ(p[1], 0.5f) << i;
    EXPECT_FLOAT_EQ(p[2], 0.5f) << i;
    EXPECT_FLOAT_EQ(p[3], 0.7f) << i;
  }
  IMB_freeImBuf(input);
}

/** A one-texel stair: the rows above four step at column 3, the rows from four on at column 5. */
static float fxaa_stair_value(const int x, const int y, const float low, const float high)
{
  return x >= (y < 4 ? 3 : 5) ? high : low;
}

TEST_F(PaintLayersEffectTest, an_fxaa_smooths_a_stair_step_edge)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_FXAA);

  ImBuf *input = effect_test_buffer(8, 8, [](const int x, const int y, float *p) {
    p[0] = p[1] = p[2] = fxaa_stair_value(x, y, 0.0f, 1.0f);
    p[3] = 1.0f;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);

  /* The corner texel (3, 3) is white with the ring nw 0, n 1, ne 1, w 0, e 1, sw 0, s 0, se 0.
   * The gradient direction is (-2, -2), a step of (-1, -1): the two ring texels it walks to,
   * (2, 2) and (4, 4), are black, and the blend is (1 * 2 + 0 + 0) / (2 + 1 + 1) = 0.5. The
   * mirror corner (4, 4) is black with a white (3, 3) and (5, 5): (0 * 2 + 1 + 1) / 4 = 0.5. */
  const auto value_at = [&](const int x, const int y) {
    return input->float_data()[(int64_t(y) * 8 + x) * 4];
  };
  EXPECT_NEAR(value_at(3, 3), 0.5f, 1e-4f);
  EXPECT_NEAR(value_at(4, 4), 0.5f, 1e-4f);

  /* A texel whose whole 3 x 3 ring is one colour has no edge: it is left as it is. */
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      const float centre = fxaa_stair_value(x, y, 0.0f, 1.0f);
      bool flat_ring = true;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          const int nx = std::clamp(x + dx, 0, 7);
          const int ny = std::clamp(y + dy, 0, 7);
          flat_ring = flat_ring && fxaa_stair_value(nx, ny, 0.0f, 1.0f) == centre;
        }
      }
      if (flat_ring) {
        EXPECT_FLOAT_EQ(value_at(x, y), centre) << "texel " << x << ", " << y;
      }
      EXPECT_GE(value_at(x, y), -1e-5f);
      EXPECT_LE(value_at(x, y), 1.0f + 1e-5f);
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, an_fxaa_leaves_alpha_untouched)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_FXAA);

  /* A busy pattern, so a good many rings are edges: the alpha is only ever a weight. */
  ImBuf *input = effect_test_buffer(8, 8, [](const int x, const int y, float *p) {
    p[0] = float((x * 7 + y * 13) % 5) / 4.0f;
    p[1] = float((x * 3 + y * 11) % 5) / 4.0f;
    p[2] = float((x * 5 + y * 2) % 5) / 4.0f;
    p[3] = 0.25f + 0.25f * float((x * 3 + y * 5) % 4);
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      EXPECT_EQ(input->float_data()[(int64_t(y) * 8 + x) * 4 + 3],
                0.25f + 0.25f * float((x * 3 + y * 5) % 4))
          << "texel " << x << ", " << y;
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, an_fxaa_does_not_leak_a_transparent_texels_black_into_an_island)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_FXAA);

  /* An opaque white island on alpha 0 and black. A transparent centre is skipped; an opaque one
   * blends with its ring texels weighted by alpha, and the transparent ones weigh nothing, so
   * every sum is a sum of whites over a sum of weights: exactly white. */
  const auto in_island = [](const int x, const int y) {
    return x >= 2 && x <= 5 && y >= 2 && y <= 5;
  };
  ImBuf *input = effect_test_buffer(8, 8, [&](const int x, const int y, float *p) {
    const float island = in_island(x, y) ? 1.0f : 0.0f;
    p[0] = p[1] = p[2] = p[3] = island;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      const float *p = input->float_data() + (int64_t(y) * 8 + x) * 4;
      const float expected = in_island(x, y) ? 1.0f : 0.0f;
      EXPECT_NEAR(p[0], expected, 1e-5f) << "texel " << x << ", " << y;
      EXPECT_NEAR(p[1], expected, 1e-5f) << "texel " << x << ", " << y;
      EXPECT_NEAR(p[2], expected, 1e-5f) << "texel " << x << ", " << y;
      EXPECT_FLOAT_EQ(p[3], expected);
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, an_fxaa_below_the_contrast_threshold_changes_nothing)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_FXAA);

  /* The stair of the test above, with a luma step of 0.05 -- under the kernel's edge threshold of
   * 0.0618: no ring is an edge, so no texel moves. */
  ImBuf *input = effect_test_buffer(8, 8, [](const int x, const int y, float *p) {
    p[0] = p[1] = p[2] = fxaa_stair_value(x, y, 0.4f, 0.45f);
    p[3] = 1.0f;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      const float *p = input->float_data() + (int64_t(y) * 8 + x) * 4;
      EXPECT_FLOAT_EQ(p[0], fxaa_stair_value(x, y, 0.4f, 0.45f)) << "texel " << x << ", " << y;
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_blur_slope_of_a_solid_row_is_the_solid)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  Vector<float> before;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(before, width, height));

  /* A solid map has no edge to smear: the walk along the slope gathers the same colour. */
  add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_SLOPE);
  Vector<float> pixels;
  ASSERT_TRUE(render_channel(pixels, width, height));
  ASSERT_EQ(pixels.size(), before.size());
  for (int64_t i = 0; i < pixels.size(); i++) {
    EXPECT_NEAR(pixels[i], before[i], 2e-3f) << "a solid row is its own slope blur, texel "
                                             << i;
  }
}

/** The luminance profile along the columns of the Blur Slope tests: a ramp with a bright stripe. */
static float blur_slope_profile(const int x)
{
  static const float profile[8] = {0.0f, 0.1f, 0.2f, 0.3f, 1.0f, 0.5f, 0.6f, 0.7f};
  return profile[x];
}

/** The Blur Slope on its own input: the luminance of the content it is laid over drives the walk. */
static void blur_slope_on_previous_input(MaterialPaintLayer &effect,
                                         const int quality,
                                         const float intensity,
                                         const int blending)
{
  EXPECT_TRUE(paint_layer_kind_param_int_set(
      effect, PAINT_LAYER_BLUR_SLOPE_SOURCE_MODE, PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT));
  EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_BLUR_SLOPE_QUALITY, quality));
  EXPECT_TRUE(paint_layer_kind_param_set(effect, PAINT_LAYER_BLUR_SLOPE_INTENSITY, intensity));
  EXPECT_TRUE(paint_layer_kind_param_int_set(effect, PAINT_LAYER_BLUR_SLOPE_BLENDING, blending));
}

/** An 8 x 8 grey buffer whose columns follow #blur_slope_profile, every row the same. */
static ImBuf *blur_slope_profile_buffer()
{
  return effect_test_buffer(8, 8, [](const int x, const int /*y*/, float *p) {
    p[0] = p[1] = p[2] = blur_slope_profile(x);
    p[3] = 1.0f;
  });
}

TEST_F(PaintLayersEffectTest, a_blur_slope_with_one_sample_or_zero_intensity_is_the_identity)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_SLOPE);

  /* One sample reads the centre, and an intensity of zero has no span to walk: both return before
   * anything is read, however steep the ramp is. */
  const struct {
    int quality;
    float intensity;
  } cases[] = {{1, 8.0f}, {8, 0.0f}};
  for (const auto &c : cases) {
    blur_slope_on_previous_input(*effect, c.quality, c.intensity, PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR);
    ImBuf *input = blur_slope_profile_buffer();
    ASSERT_NE(input, nullptr);
    paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
    for (const int y : IndexRange(8)) {
      for (const int x : IndexRange(8)) {
        EXPECT_FLOAT_EQ(input->float_data()[(int64_t(y) * 8 + x) * 4], blur_slope_profile(x))
            << "quality " << c.quality << ", texel " << x << ", " << y;
      }
    }
    IMB_freeImBuf(input);
  }
}

TEST_F(PaintLayersEffectTest, a_blur_slope_in_previous_input_mode_blurs_along_the_ramp)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_SLOPE);
  blur_slope_on_previous_input(*effect, 3, 2.0f, PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR);

  /* The columns follow {0, .1, .2, .3, 1, .5, .6, .7}, constant down the rows: the luminance
   * gradient is (gx, 0) with gx != 0 everywhere, so the walk runs along x. Three samples over two
   * texels are t = -1, 0, 1, the taps x - 1, x, x + 1 (clamped), whichever way gx points. The
   * blend of the opaque taps is their mean:
   *   x = 0: (0 + 0 + .1) / 3 = .0333   x = 4: (.3 + 1 + .5) / 3 = .6
   *   x = 1: .1   x = 2: .2   x = 3: .5  x = 5: .7  x = 6: .6  x = 7: (.6 + .7 + .7) / 3 = .6667.
   * The stripe at 4 spreads to both sides, and no row mixes with another. */
  const float expected[8] = {0.1f / 3.0f, 0.1f, 0.2f, 0.5f, 0.6f, 0.7f, 0.6f, 2.0f / 3.0f};
  ImBuf *input = blur_slope_profile_buffer();
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      EXPECT_NEAR(input->float_data()[(int64_t(y) * 8 + x) * 4], expected[x], 1e-5f)
          << "texel " << x << ", " << y;
    }
  }
  EXPECT_GT(input->float_data()[(int64_t(2) * 8 + 3) * 4], blur_slope_profile(3) + 0.1f);
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_blur_slope_max_mode_spreads_the_bright_and_min_mode_the_dark)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_SLOPE);

  /* The same walk as the Blur test (taps x - 1, x, x + 1), the extreme of the taps instead of
   * their mean: Max is >= the texel itself everywhere and Min <= it. Max at 3 is max(.2, .3, 1)
   * = 1 (the stripe spreads), at 2 it is .3; Min at 3 is .2 and at 4 min(.3, 1, .5) = .3. */
  blur_slope_on_previous_input(*effect, 3, 2.0f, PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX);
  ImBuf *maximum = blur_slope_profile_buffer();
  ASSERT_NE(maximum, nullptr);
  paint_layer_effect_kernel_apply(*maximum, *effect, false, PAINT_TILE_NUMBER_BASE);
  blur_slope_on_previous_input(*effect, 3, 2.0f, PAINT_LAYER_BLUR_SLOPE_BLENDING_MIN);
  ImBuf *minimum = blur_slope_profile_buffer();
  ASSERT_NE(minimum, nullptr);
  paint_layer_effect_kernel_apply(*minimum, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int x : IndexRange(8)) {
    const float value = maximum->float_data()[(int64_t(5) * 8 + x) * 4];
    const float lowest = minimum->float_data()[(int64_t(5) * 8 + x) * 4];
    EXPECT_GE(value, blur_slope_profile(x) - 1e-6f) << x;
    EXPECT_LE(lowest, blur_slope_profile(x) + 1e-6f) << x;
  }
  EXPECT_NEAR(maximum->float_data()[(int64_t(5) * 8 + 3) * 4], 1.0f, 1e-6f);
  EXPECT_NEAR(maximum->float_data()[(int64_t(5) * 8 + 2) * 4], 0.3f, 1e-6f);
  EXPECT_NEAR(minimum->float_data()[(int64_t(5) * 8 + 3) * 4], 0.2f, 1e-6f);
  EXPECT_NEAR(minimum->float_data()[(int64_t(5) * 8 + 4) * 4], 0.3f, 1e-6f);
  IMB_freeImBuf(maximum);
  IMB_freeImBuf(minimum);

  /* A tap from an alpha-zero texel is skipped: with the stripe texel (4) transparent, Max at 3
   * is max(.2, .3) = .3 and at 5 it is max(.5, .6) = .6 (the taps x = 4 are dropped). */
  blur_slope_on_previous_input(*effect, 3, 2.0f, PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX);
  ImBuf *holed = effect_test_buffer(8, 8, [](const int x, const int /*y*/, float *p) {
    p[0] = p[1] = p[2] = blur_slope_profile(x);
    p[3] = (x == 4) ? 0.0f : 1.0f;
  });
  ASSERT_NE(holed, nullptr);
  paint_layer_effect_kernel_apply(*holed, *effect, false, PAINT_TILE_NUMBER_BASE);
  EXPECT_NEAR(holed->float_data()[(int64_t(5) * 8 + 3) * 4], 0.3f, 1e-6f);
  EXPECT_NEAR(holed->float_data()[(int64_t(5) * 8 + 5) * 4], 0.6f, 1e-6f);
  IMB_freeImBuf(holed);
}

TEST_F(PaintLayersEffectTest, a_blur_slope_does_not_leak_a_transparent_texels_black_into_an_island)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_SLOPE);
  blur_slope_on_previous_input(*effect, 5, 4.0f, PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR);

  /* An opaque white island on alpha 0 and black. The taps are weighted by alpha, so the
   * transparent ones weigh nothing: a texel's blend is a sum of whites over a sum of weights, and
   * every texel is white or untouched black -- never a grey. An opaque texel always has its own
   * tap (t = 0 is the centre sample of an odd quality), so it is white. */
  const auto in_island = [](const int x, const int y) {
    return x >= 3 && x <= 6 && y >= 3 && y <= 6;
  };
  ImBuf *input = effect_test_buffer(10, 10, [&](const int x, const int y, float *p) {
    const float island = in_island(x, y) ? 1.0f : 0.0f;
    p[0] = p[1] = p[2] = p[3] = island;
  });
  ASSERT_NE(input, nullptr);
  paint_layer_effect_kernel_apply(*input, *effect, false, PAINT_TILE_NUMBER_BASE);
  for (const int y : IndexRange(10)) {
    for (const int x : IndexRange(10)) {
      const float *p = input->float_data() + (int64_t(y) * 10 + x) * 4;
      if (in_island(x, y)) {
        EXPECT_NEAR(p[0], 1.0f, 1e-5f) << "texel " << x << ", " << y;
      }
      else {
        EXPECT_TRUE(std::abs(p[0]) < 1e-5f || std::abs(p[0] - 1.0f) < 1e-5f)
            << "a grey of " << p[0] << " leaked at texel " << x << ", " << y;
      }
      EXPECT_FLOAT_EQ(p[3], in_island(x, y) ? 1.0f : 0.0f);
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_blur_slope_is_deterministic_and_keeps_alpha)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR_SLOPE);

  /* The noise source (the default) over a busy 32 x 32 pattern with alphas from zero to one. The
   * rows run in parallel but each texel is written from the private clone alone, so two runs over
   * the same input are bit-identical; the alpha is only a weight, so it never changes. */
  const auto make_input = []() {
    return effect_test_buffer(32, 32, [](const int x, const int y, float *p) {
      p[0] = float((x * 7 + y * 13) % 11) / 10.0f;
      p[1] = float((x * 3 + y * 5) % 7) / 6.0f;
      p[2] = float((x * 5 + y * 3) % 13) / 12.0f;
      p[3] = 0.25f * float((x + 2 * y) % 5);
    });
  };
  ImBuf *original = make_input();
  ImBuf *first = make_input();
  ImBuf *second = make_input();
  ASSERT_NE(original, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  paint_layer_effect_kernel_apply(*first, *effect, false, PAINT_TILE_NUMBER_BASE);
  paint_layer_effect_kernel_apply(*second, *effect, false, PAINT_TILE_NUMBER_BASE);

  bool changed = false;
  for (const int64_t i : IndexRange(32 * 32 * 4)) {
    EXPECT_EQ(first->float_data()[i], second->float_data()[i]) << "float " << i;
    changed = changed || first->float_data()[i] != original->float_data()[i];
    if (i % 4 == 3) {
      EXPECT_EQ(first->float_data()[i], original->float_data()[i]) << "alpha " << i / 4;
    }
  }
  EXPECT_TRUE(changed) << "the default walk over the noise smears something";
  IMB_freeImBuf(original);
  IMB_freeImBuf(first);
  IMB_freeImBuf(second);
}

TEST_F(PaintLayersEffectTest, height_to_normal_reads_the_height_of_its_own_tile)
{
  /* One tiled Height map: the base tile is flat white and tile 1002 carries a vertical step. The
   * Normal composite of each tile must read that tile's Height -- the effect's input describe is
   * handed the tile the composite is for -- so the step can only show on 1002. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *height_map = BKE_image_add_generated(
      bmain, 8, 8, "Height", 32, false, IMA_GENTYPE_BLANK, black, false, true, true);
  ASSERT_NE(height_map, nullptr);
  /* A tile added by number carries the generator defaults (a 1024 grid, never filled): size it like
   * the base tile and fill it, or it has no buffer to paint. */
  const auto add_filled_tile = [&](Image *image) {
    ImageTile *tile = BKE_image_add_tile(image, 1002, nullptr);
    ASSERT_NE(tile, nullptr);
    tile->gen_x = 8;
    tile->gen_y = 8;
    tile->gen_type = IMA_GENTYPE_BLANK;
    tile->gen_depth = 32;
    copy_v4_v4(tile->gen_color, black);
    ASSERT_TRUE(BKE_image_fill_tile(image, tile));
  };
  add_filled_tile(height_map);
  const auto paint_height_tile = [&](const int tile_number, const bool step) {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(height_map, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(8)) {
      for (const int x : IndexRange(8)) {
        const uchar value = (step && x >= 4) ? 0 : 255;
        uchar *p = pixels + (int64_t(y) * 8 + x) * 4;
        p[0] = value;
        p[1] = value;
        p[2] = value;
        p[3] = 255;
      }
    }
    BKE_image_release_ibuf(height_map, ibuf, lock);
  };
  paint_height_tile(1001, false);
  paint_height_tile(1002, true);
  BKE_image_partial_update_mark_full_update(height_map);

  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_HEIGHT, true));
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, true));
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 128, 128));
  add_channel(*row, PAINT_MATERIAL_CHANNEL_HEIGHT, height_map);
  /* The Normal map is tiled too, so both tiles resolve: its own grey is irrelevant, the effect
   * replaces it with the height's normal. */
  Image *normal_map = BKE_image_add_generated(
      bmain, 8, 8, "Normal", 32, false, IMA_GENTYPE_BLANK, black, false, true, true);
  ASSERT_NE(normal_map, nullptr);
  add_filled_tile(normal_map);
  for (const int tile_number : {1001, 1002}) {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(normal_map, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(8) * 8)) {
      pixels[i * 4 + 0] = 128;
      pixels[i * 4 + 1] = 128;
      pixels[i * 4 + 2] = 255;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(normal_map, ibuf, lock);
  }
  add_channel(*row, PAINT_MATERIAL_CHANNEL_NORMAL, normal_map);
  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  /* The effect lays a normal, so it filters the Normal alone, whatever other record it carries. */
  EXPECT_TRUE(paint_layer_effect_filters(*ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_FALSE(paint_layer_effect_filters(*ma, *effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  Vector<float> flat;
  Vector<float> stepped;
  int flat_width = 0;
  int flat_height = 0;
  int stepped_width = 0;
  int stepped_height = 0;
  ASSERT_TRUE(render_channel_on_tile(
      PAINT_MATERIAL_CHANNEL_NORMAL, 1001, flat, flat_width, flat_height));
  ASSERT_TRUE(render_channel_on_tile(
      PAINT_MATERIAL_CHANNEL_NORMAL, 1002, stepped, stepped_width, stepped_height));
  const auto at = [](const Vector<float> &pixels, const int width, const int x, const int y) {
    return &pixels[(int64_t(y) * width + x) * 4];
  };
  /* The flat tile is the neutral normal: encoded x and y at the half, z at one. */
  EXPECT_NEAR(at(flat, flat_width, 3, 4)[0], 0.5f, 2e-3f);
  EXPECT_NEAR(at(flat, flat_width, 4, 4)[0], 0.5f, 2e-3f);
  /* The step on 1002 leans the normal about the edge, which only its own Height can produce. */
  EXPECT_GT(std::abs(at(stepped, stepped_width, 3, 4)[0] - 0.5f), 1e-2f);

  /* A stroke on the step tile is a new pixel version of the map the input read: the effect cache
   * must recompute it into the neutral normal. */
  paint_height_tile(1002, false);
  BKE_image_partial_update_mark_full_update(height_map);
  ASSERT_TRUE(render_channel_on_tile(
      PAINT_MATERIAL_CHANNEL_NORMAL, 1002, stepped, stepped_width, stepped_height));
  EXPECT_NEAR(at(stepped, stepped_width, 3, 4)[0], 0.5f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, height_to_normal_lays_a_normal_on_a_row_that_has_only_a_height)
{
  /* A row with a map in Height alone and no Normal of its own: the effect is the only thing that
   * can put a normal in that channel, so the row must take part there and the effect must read the
   * Height it lays. */
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_HEIGHT, true));
  ASSERT_TRUE(BKE_paint_layers_channel_set_enable(*ma, PAINT_MATERIAL_CHANNEL_NORMAL, true));
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *height_map = BKE_image_add_generated(
      bmain, 8, 8, "Height", 32, false, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(height_map, nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(height_map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *texels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(8)) {
      for (const int x : IndexRange(8)) {
        uchar *p = texels + (int64_t(y) * 8 + x) * 4;
        p[0] = p[1] = p[2] = (x >= 4) ? 0 : 255;
        p[3] = 255;
      }
    }
    BKE_image_release_ibuf(height_map, ibuf, lock);
  }
  BKE_image_partial_update_mark_full_update(height_map);
  MaterialPaintLayer *row = add_paint_layer("Row", height_map, PAINT_MATERIAL_CHANNEL_HEIGHT);
  ASSERT_NE(row, nullptr);
  EXPECT_FALSE(paint_layer_channel_contributes(
      *ma, *row, paint_layer_channel_find(*row, PAINT_MATERIAL_CHANNEL_NORMAL)))
      << "the row has no normal of its own";
  EXPECT_FALSE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));

  MaterialPaintLayer *effect = add_effect(*row, MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL);
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(paint_layer_channel_find(*effect, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  EXPECT_TRUE(paint_layer_effect_filters(*ma, *effect, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(paint_layer_effects_derive(*ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL));
  EXPECT_TRUE(paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_NORMAL))
      << "the effect brings the row into the Normal channel";
  EXPECT_FALSE(paint_layer_effects_derive(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));

  PaintMaterialCompositeImageLayer entry;
  EXPECT_TRUE(BKE_paint_layers_effect_input_describe(
      *ma, *row, *effect, PAINT_MATERIAL_CHANNEL_NORMAL, true, false, entry));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel_on_tile(
      PAINT_MATERIAL_CHANNEL_NORMAL, PAINT_TILE_NUMBER_BASE, pixels, width, height));
  /* The Height steps from white to black at x = 4: the normal leans about the edge, nowhere else. */
  const float *edge = &pixels[(int64_t(4) * width + 3) * 4];
  const float *flat = &pixels[(int64_t(4) * width + 1) * 4];
  EXPECT_GT(std::abs(edge[0] - 0.5f), 1e-2f);
  EXPECT_NEAR(flat[0], 0.5f, 2e-3f);
  EXPECT_NEAR(flat[2], 1.0f, 2e-3f);
}

TEST_F(PaintLayersEffectTest, a_reduced_bake_keeps_the_effects_input_at_the_stack_resolution)
{
  /* The drag-reduced bake composites on a smaller grid, but a CPU-computed effect's input is always
   * described at the stack's own resolution: its texel parameters (a Drop Shadow's offset, a Blur's
   * radius) keep their meaning, and the reduced frame is a resample of the full result. */
  ma->paint_layers_image_size = 4;
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *shadow = add_effect(*row, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  ASSERT_NE(shadow, nullptr);
  BKE_paint_layers_bake_drag_set(*bmain, ma->id.session_uid, true);

  PaintMaterialCompositeImageLayer entry;
  ASSERT_TRUE(BKE_paint_layers_effect_input_describe(
      *ma, *row, *shadow, kColor, true, false, entry));
  int width = 0;
  int height = 0;
  ASSERT_TRUE(BKE_paint_material_composite_stack_dimensions(
      Span<PaintMaterialCompositeImageLayer>(&entry, 1), width, height));
  EXPECT_EQ(width, 8) << "the effect input keeps the stack's texels while the bake is reduced";
  EXPECT_EQ(height, 8);

  /* The bake itself is the reduced square the drag asked for. */
  Vector<float> color(4 * 4 * 4);
  Vector<float> coverage(4 * 4);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *row,
                                                kColor,
                                                4,
                                                color.data(),
                                                coverage.data(),
                                                nullptr,
                                                nullptr));
  BKE_paint_layers_bake_drag_set(*bmain, ma->id.session_uid, false);
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

  /* The table this test once walked -- the Color Match and the Fill Area, the last two kinds that
   * refused the combined preview by their kind -- is empty now: both carry
   * #PAINT_LAYER_KIND_GPU_COMPUTED, and no effect kind names a reason any more. The refusals that
   * remain are parameter ones -- a picked Color Match source, the Fill Area's largest-region
   * classification, its debug ramp and its iteration-less source mode -- which the plan builder
   * answers with an empty plan (the plan test reads them) and the gather falls back on through its
   * own no-pass-plan check. The walk below reads the two kinds that left the table, the description
   * naming no reason for their defaults, which are the parameterizations the plans build. */
  for (const eMaterialPaintLayerFilter filter :
       {MA_PAINT_LAYER_FILTER_COLOR_MATCH, MA_PAINT_LAYER_FILTER_FILL_AREA})
  {
    MaterialPaintLayer *effect = add_effect(*row, filter);
    ASSERT_NE(effect, nullptr);

    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(
        BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
    ASSERT_FALSE(layers.is_empty());
    EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), nullptr);
    EXPECT_EQ(paint_layer_effect_cache_entry_count(), 0)
        << "a describe-only pass computes nothing";

    /* The kind leaves with its reason (not) read, so the next one is described alone too. */
    EXPECT_TRUE(BKE_paint_layers_correction_remove(*ma, effect));
  }
}

TEST_F(PaintLayersEffectTest, the_phase_two_kinds_are_gpu_computed_and_refuse_no_reason)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));

  /* Every kind this series moved onto the GPU: the description names no reason to fall back, and the
   * flag the refusal and the gather's queue both read is set. The defaults are the parameterizations
   * the plans build -- the reduce branch and the edge-based flood -- the ones they refuse being the
   * plan test's. A describe-only pass still computes nothing. */
  const eMaterialPaintLayerFilter gpu_kinds[] = {
      MA_PAINT_LAYER_FILTER_BLUR,
      MA_PAINT_LAYER_FILTER_SHARPEN,
      MA_PAINT_LAYER_FILTER_HIGH_PASS,
      MA_PAINT_LAYER_FILTER_EDGE_DETECT,
      MA_PAINT_LAYER_FILTER_EMBOSS,
      MA_PAINT_LAYER_FILTER_ERODE_DILATE,
      MA_PAINT_LAYER_FILTER_GLOW,
      MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL,
      MA_PAINT_LAYER_FILTER_PIXELATE,
      MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT,
      MA_PAINT_LAYER_FILTER_BEVEL,
      MA_PAINT_LAYER_FILTER_MASK_OUTLINE,
      MA_PAINT_LAYER_FILTER_DROP_SHADOW,
      MA_PAINT_LAYER_FILTER_BLUR_SLOPE,
      MA_PAINT_LAYER_FILTER_COLOR_MATCH,
      MA_PAINT_LAYER_FILTER_FILL_AREA,
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

TEST_F(PaintLayersEffectTest, the_combined_preview_shades_a_deband_without_computing_it)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  ASSERT_NE(BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_DEBAND), nullptr);

  /* The shader repeats the hash of the texel (the filter branch receives the coordinates), so the
   * described stack names no reason, and the noise is not computed for the description. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(*ma, kColor, layers, /*describe_only=*/true));
  ASSERT_FALSE(layers.is_empty());
  EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layers[0], false), nullptr);
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
                           MA_PAINT_LAYER_FILTER_DEBAND,
                           MA_PAINT_LAYER_FILTER_NORMAL_CONVERT,
                           MA_PAINT_LAYER_FILTER_CHANNEL_MIXER,
                           MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN,
                           MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT,
                           MA_PAINT_LAYER_FILTER_SMOOTHSTEP,
                           MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY,
                           MA_PAINT_LAYER_FILTER_HEIGHT_ADJUST,
                           MA_PAINT_LAYER_FILTER_HSL_PERSPECTIVE};
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

TEST_F(PaintLayersEffectTest, a_mask_outline_lays_its_band_on_the_side_the_position_names)
{
  /* The shade image's white left half is the shape (read by its luminance, the row being opaque):
   * the border runs along column 4, and the falloff is linear, two texels wide. The ring is a mid
   * grey laid over the row, so each case shows which side it lies on: the white content darkens
   * under it, the black one lightens. */
  struct Case {
    PaintLayerMaskOutlinePosition position;
    float expected[8];
  };
  /* A texel next to the border is one chamfer step (3 / 3 = 1 texel) away: half the band of two. */
  const Case cases[] = {
      {PAINT_LAYER_MASK_OUTLINE_OUTSIDE, {1, 1, 1, 1, 0.25f, 0, 0, 0}},
      {PAINT_LAYER_MASK_OUTLINE_INSIDE, {1, 1, 1, 0.75f, 0, 0, 0, 0}},
      {PAINT_LAYER_MASK_OUTLINE_DOUBLE, {1, 1, 1, 0.75f, 0.25f, 0, 0, 0}},
  };
  for (const Case &test : cases) {
    SCOPED_TRACE(int(test.position));
    clear_active();
    MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
    MaterialPaintLayer *outline = add_effect(*row, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
    ASSERT_TRUE(paint_layer_kind_param_int_set(
        *outline, PAINT_LAYER_MASK_OUTLINE_POSITION, int(test.position)));
    ASSERT_TRUE(paint_layer_kind_param_int_set(*outline, PAINT_LAYER_MASK_OUTLINE_WIDTH, 2));
    ASSERT_TRUE(paint_layer_kind_param_set(*outline, PAINT_LAYER_MASK_OUTLINE_SHAPE, 0.0f));
    ASSERT_TRUE(paint_layer_kind_param_int_set(
        *outline, PAINT_LAYER_MASK_OUTLINE_SOURCE, PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE));
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    ASSERT_TRUE(paint_layer_kind_param_color_set(*outline, PAINT_LAYER_MASK_OUTLINE_COLOR, grey));

    Vector<float> pixels;
    int width = 0;
    int height = 0;
    ASSERT_TRUE(render_channel(pixels, width, height));
    for (const int y : IndexRange(height)) {
      for (const int x : IndexRange(8)) {
        EXPECT_NEAR(linear_pixel(pixels, width, x, y), test.expected[x], 2e-3f) << x << "," << y;
      }
    }
    BKE_paint_layers_effect_cache_free_all();
  }
}

TEST_F(PaintLayersEffectTest, a_mask_outline_of_a_buffer_with_no_border_lays_no_band)
{
  /* All inside or all outside: one side has no border at all, so its distance is unseen, which
   * reads as infinitely far and not as the border itself. */
  for (const int grey : {0, 255}) {
    for (const PaintLayerMaskOutlinePosition position : {PAINT_LAYER_MASK_OUTLINE_OUTSIDE,
                                                         PAINT_LAYER_MASK_OUTLINE_INSIDE,
                                                         PAINT_LAYER_MASK_OUTLINE_DOUBLE})
    {
      SCOPED_TRACE(grey * 10 + int(position));
      clear_active();
      MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", grey, grey));
      MaterialPaintLayer *outline = add_effect(*row, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
      ASSERT_TRUE(paint_layer_kind_param_int_set(
          *outline, PAINT_LAYER_MASK_OUTLINE_POSITION, int(position)));
      ASSERT_TRUE(paint_layer_kind_param_int_set(
          *outline, PAINT_LAYER_MASK_OUTLINE_SOURCE, PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE));

      Vector<float> pixels;
      int width = 0;
      int height = 0;
      ASSERT_TRUE(render_channel(pixels, width, height));
      /* No band: the ring layer is empty, so the row shows as it is. */
      const float content = grey ? 1.0f : 0.0f;
      for (const int y : IndexRange(height)) {
        for (const int x : IndexRange(width)) {
          EXPECT_NEAR(linear_pixel(pixels, width, x, y), content, 2e-3f)
              << x << "," << y;
        }
      }
      BKE_paint_layers_effect_cache_free_all();
    }
  }
}

TEST_F(PaintLayersEffectTest, a_mask_outline_kernel_lays_a_premultiplied_ring_around_an_alpha_island)
{
  /* Content mode reads the alpha: a white island on the left half of a transparent map. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *outline = add_effect(*row, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*outline, PAINT_LAYER_MASK_OUTLINE_WIDTH, 2));
  ASSERT_TRUE(paint_layer_kind_param_set(*outline, PAINT_LAYER_MASK_OUTLINE_SHAPE, 0.0f));
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*outline, PAINT_LAYER_MASK_OUTLINE_COLOR, red));

  struct Case {
    PaintLayerMaskOutlinePosition position;
    PaintLayerDropShadowSource source;
    /* The band alpha of columns 0..7. */
    float expected[8];
  };
  const Case cases[] = {
      {PAINT_LAYER_MASK_OUTLINE_OUTSIDE, PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA, {0, 0, 0, 0, 0.5f, 0, 0, 0}},
      {PAINT_LAYER_MASK_OUTLINE_INSIDE, PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA, {0, 0, 0, 0.5f, 0, 0, 0, 0}},
      {PAINT_LAYER_MASK_OUTLINE_DOUBLE, PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA, {0, 0, 0, 0.5f, 0.5f, 0, 0, 0}},
      /* The luminance of the same island is the same shape. */
      {PAINT_LAYER_MASK_OUTLINE_OUTSIDE, PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE, {0, 0, 0, 0, 0.5f, 0, 0, 0}},
  };
  for (const Case &test : cases) {
    SCOPED_TRACE(int(test.position) * 10 + int(test.source));
    ASSERT_TRUE(paint_layer_kind_param_int_set(
        *outline, PAINT_LAYER_MASK_OUTLINE_POSITION, int(test.position)));
    ASSERT_TRUE(paint_layer_kind_param_int_set(
        *outline, PAINT_LAYER_MASK_OUTLINE_SOURCE, int(test.source)));

    constexpr int size = 8;
    ImBuf *input = IMB_allocImBuf(size, size, ImBufFlags::FloatData);
    ASSERT_NE(input, nullptr);
    float *pixels = input->float_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        float *p = pixels + (int64_t(y) * size + x) * 4;
        const float covered = x < size / 2 ? 1.0f : 0.0f;
        p[0] = covered;
        p[1] = covered;
        p[2] = covered;
        p[3] = covered;
      }
    }
    paint_layer_effect_kernel_apply(*input, *outline, false, PAINT_TILE_NUMBER_BASE);

    /* The layer is empty on the island and far from it; the ring is premultiplied by the band,
     * with the band as its alpha. The Color tints it only from a luminance source: an alpha source
     * lays a white ring, the Color being hidden there. */
    const bool white = test.source == PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA;
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const float *p = pixels + (int64_t(y) * size + x) * 4;
        EXPECT_NEAR(p[3], test.expected[x], 1e-6f) << "alpha at " << x << "," << y;
        EXPECT_NEAR(p[0], test.expected[x], 1e-6f) << "premultiplied red at " << x << "," << y;
        EXPECT_NEAR(p[1], white ? test.expected[x] : 0.0f, 1e-6f) << "green at " << x << "," << y;
        EXPECT_NEAR(p[2], white ? test.expected[x] : 0.0f, 1e-6f) << "blue at " << x << "," << y;
      }
    }
    IMB_freeImBuf(input);
  }
}

TEST_F(PaintLayersEffectTest, a_mask_outline_shows_on_the_map_around_an_island)
{
  /* The left half is an opaque white island, the right half has no alpha: the ring is laid onto
   * texels the row covers nothing at, which a colour-only band could never show. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0, false, 0));
  MaterialPaintLayer *outline = add_effect(*row, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*outline, PAINT_LAYER_MASK_OUTLINE_WIDTH, 2));
  ASSERT_TRUE(paint_layer_kind_param_set(*outline, PAINT_LAYER_MASK_OUTLINE_SHAPE, 0.0f));
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*outline, PAINT_LAYER_MASK_OUTLINE_COLOR, red));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  const auto texel = [&](const int x, const int y, const int channel) {
    return pixels[(int64_t(y) * width + x) * 4 + channel];
  };
  /* The island keeps its white, the texel next to it takes the ring's white where the row covered
   * nothing, and the map past the band is as empty as it was. */
  EXPECT_NEAR(texel(1, 4, 0), 1.0f, 2e-3f);
  EXPECT_NEAR(texel(1, 4, 1), 1.0f, 2e-3f);
  EXPECT_GT(texel(4, 4, 0), 0.05f) << "the ring raises the coverage where the row had none";
  EXPECT_GT(texel(4, 4, 1), 0.05f) << "an alpha source lays a white ring, not the hidden Color";
  EXPECT_GT(texel(4, 4, 2), 0.05f);
  EXPECT_NEAR(texel(7, 4, 0), 0.0f, 2e-3f) << "no ring far from the island";
}

/** A 9x9 float buffer holding a closed ring of premultiplied red around a hole, the ring traced
 * by the texels two away from the centre: opaque on the ring, nothing on the hole and the map
 * around it. \a gap opens the ring's west texel. */
static ImBuf *fill_area_ring_buffer(const bool gap)
{
  constexpr int size = 9;
  ImBuf *input = IMB_allocImBuf(size, size, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      float *p = pixels + (int64_t(y) * size + x) * 4;
      const bool on_ring = std::max(std::abs(x - 4), std::abs(y - 4)) == 2 && !(gap && x == 2 && y == 4);
      p[0] = on_ring ? 1.0f : 0.0f;
      p[1] = 0.0f;
      p[2] = 0.0f;
      p[3] = on_ring ? 1.0f : 0.0f;
    }
  }
  return input;
}

TEST_F(PaintLayersEffectTest, a_fill_area_kernel_lays_a_premultiplied_fill_on_an_enclosed_hole)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);

  for (const PaintLayerFillAreaUvBorder uv_border : {PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                     PAINT_LAYER_FILL_AREA_UV_BORDER_FILL})
  {
    SCOPED_TRACE(int(uv_border));
    ASSERT_TRUE(paint_layer_kind_param_int_set(*fill, PAINT_LAYER_FILL_AREA_UV_BORDER, int(uv_border)));
    ImBuf *input = fill_area_ring_buffer(false);
    paint_layer_effect_kernel_apply(*input, *fill, false, PAINT_TILE_NUMBER_BASE);
    const float *pixels = input->float_data();
    const auto texel = [&](const int x, const int y) { return pixels + (int64_t(y) * 9 + x) * 4; };
    /* The hole takes the ring's red and an alpha of one: the fill is a layer of its own, because
     * the hole had no coverage a colour-only result could have shown on. The 24 passes carry the
     * red into the 3x3 hole's every texel. */
    for (const int y : IndexRange(3, 3)) {
      for (const int x : IndexRange(3, 3)) {
        EXPECT_NEAR(texel(x, y)[0], 1.0f, 5e-3f) << "red at " << x << "," << y;
        EXPECT_NEAR(texel(x, y)[1], 0.0f, 1e-6f);
        EXPECT_NEAR(texel(x, y)[3], 1.0f, 1e-6f) << "alpha at " << x << "," << y;
      }
    }
    /* The ring itself and the map around it are empty in the layer: the row keeps them. */
    for (const std::pair<int, int> empty : {std::pair<int, int>(2, 2),
                                            std::pair<int, int>(2, 4),
                                            std::pair<int, int>(0, 0),
                                            std::pair<int, int>(8, 8),
                                            std::pair<int, int>(1, 4)})
    {
      for (const int c : IndexRange(4)) {
        EXPECT_NEAR(texel(empty.first, empty.second)[c], 0.0f, 1e-6f)
            << "channel " << c << " at " << empty.first << "," << empty.second;
      }
    }
    IMB_freeImBuf(input);
  }
}

TEST_F(PaintLayersEffectTest, a_fill_area_kernel_leaves_a_hole_the_ring_does_not_close)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);

  /* A gap joins the hole to the map around it: one region that runs off the map's edge is the
   * background whichever way the border behaves there, and the layer stays empty. */
  for (const PaintLayerFillAreaUvBorder uv_border : {PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                     PAINT_LAYER_FILL_AREA_UV_BORDER_FILL})
  {
    SCOPED_TRACE(int(uv_border));
    ASSERT_TRUE(paint_layer_kind_param_int_set(*fill, PAINT_LAYER_FILL_AREA_UV_BORDER, int(uv_border)));
    ImBuf *input = fill_area_ring_buffer(true);
    paint_layer_effect_kernel_apply(*input, *fill, false, PAINT_TILE_NUMBER_BASE);
    for (const int64_t i : IndexRange(int64_t(9) * 9 * 4)) {
      EXPECT_NEAR(input->float_data()[i], 0.0f, 1e-6f) << "component " << i;
    }
    IMB_freeImBuf(input);
  }
}

TEST_F(PaintLayersEffectTest, a_fill_area_kernel_with_no_border_lays_an_empty_layer)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);

  /* No texel reaches the threshold: the whole map is one region that touches the edge, so there
   * is no hole to fill. The buffer is not handed back as the layer. */
  ImBuf *input = IMB_allocImBuf(9, 9, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(9) * 9)) {
    pixels[i * 4 + 0] = 0.2f;
    pixels[i * 4 + 1] = 0.2f;
    pixels[i * 4 + 2] = 0.2f;
    pixels[i * 4 + 3] = 0.1f;
  }
  paint_layer_effect_kernel_apply(*input, *fill, false, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(9) * 9 * 4)) {
    EXPECT_NEAR(input->float_data()[i], 0.0f, 1e-6f) << "component " << i;
  }
  IMB_freeImBuf(input);

  /* A border that covers the whole map leaves nothing enclosed either. */
  input = IMB_allocImBuf(9, 9, ImBufFlags::FloatData);
  pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(9) * 9 * 4)) {
    pixels[i] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *fill, false, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(9) * 9 * 4)) {
    EXPECT_NEAR(input->float_data()[i], 0.0f, 1e-6f) << "component " << i;
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_fill_area_solid_mode_fills_the_hole_with_the_colour)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);
  ASSERT_TRUE(paint_layer_kind_param_int_set(
      *fill, PAINT_LAYER_FILL_AREA_COLOR_MODE, PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID));
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(paint_layer_kind_param_color_set(*fill, PAINT_LAYER_FILL_AREA_COLOR, green));

  ImBuf *input = fill_area_ring_buffer(false);
  paint_layer_effect_kernel_apply(*input, *fill, false, PAINT_TILE_NUMBER_BASE);
  const float *hole = input->float_data() + (int64_t(4) * 9 + 4) * 4;
  EXPECT_NEAR(hole[0], 0.0f, 1e-6f);
  EXPECT_NEAR(hole[1], 1.0f, 1e-6f);
  EXPECT_NEAR(hole[3], 1.0f, 1e-6f);
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_fill_area_blur_softens_the_edge_of_the_fill_layer)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);
  ASSERT_TRUE(paint_layer_kind_param_int_set(*fill, PAINT_LAYER_FILL_AREA_BLUR, 1));

  ImBuf *input = fill_area_ring_buffer(false);
  paint_layer_effect_kernel_apply(*input, *fill, false, PAINT_TILE_NUMBER_BASE);
  /* The hard fill was one inside the hole and zero on the ring: the blur gives the ring's inner
   * texel a coverage between them, and the colour stays premultiplied by it (no brighter than
   * the alpha, red only). */
  const float *edge = input->float_data() + (int64_t(4) * 9 + 2) * 4;
  EXPECT_GT(edge[3], 0.05f);
  EXPECT_LT(edge[3], 0.95f);
  EXPECT_LE(edge[0], edge[3] + 1e-4f);
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_fill_area_kernel_in_a_mask_fills_the_grey_and_keeps_the_alpha)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *fill = add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);
  /* The source parameter is a content one: a mask's grey has no alpha, so the luminance is always
   * the border, whatever the parameter reads. */
  ASSERT_EQ(paint_layer_kind_param_int_get(*fill, PAINT_LAYER_FILL_AREA_SOURCE),
            PAINT_LAYER_FILL_AREA_SOURCE_ALPHA);

  ImBuf *input = fill_area_ring_buffer(false);
  float *pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(9) * 9)) {
    /* White ring, black elsewhere, alpha one everywhere: the grey a mask item holds. */
    const float grey = pixels[i * 4 + 0];
    pixels[i * 4 + 1] = grey;
    pixels[i * 4 + 2] = grey;
    pixels[i * 4 + 3] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *fill, true, PAINT_TILE_NUMBER_BASE);
  const float *result = input->float_data();
  const auto texel = [&](const int x, const int y) { return result + (int64_t(y) * 9 + x) * 4; };
  for (const int y : IndexRange(3, 3)) {
    for (const int x : IndexRange(3, 3)) {
      EXPECT_NEAR(texel(x, y)[0], 1.0f, 5e-3f) << "grey at " << x << "," << y;
      EXPECT_NEAR(texel(x, y)[1], texel(x, y)[0], 1e-6f);
    }
  }
  EXPECT_NEAR(texel(2, 4)[0], 1.0f, 1e-6f) << "the ring keeps its white";
  EXPECT_NEAR(texel(0, 0)[0], 0.0f, 1e-6f) << "the open map stays black";
  for (const int64_t i : IndexRange(int64_t(9) * 9)) {
    EXPECT_NEAR(result[i * 4 + 3], 1.0f, 1e-6f) << "a mask's alpha stays one, texel " << i;
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_fill_area_shows_in_the_hole_of_a_ring_on_the_map)
{
  /* The ring is opaque, its hole and the map around it have no alpha: the fill raises the
   * coverage of the hole, which a colour-only result could not show. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_ring_image("Row"));
  add_effect(*row, MA_PAINT_LAYER_FILTER_FILL_AREA);

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  const auto texel = [&](const int x, const int y, const int channel) {
    return pixels[(int64_t(y) * width + x) * 4 + channel];
  };
  EXPECT_NEAR(texel(1, 3, 0), 1.0f, 2e-3f) << "the ring keeps its red";
  EXPECT_GT(texel(3, 3, 0), 0.5f) << "the hole takes the ring's red where the row had no coverage";
  EXPECT_NEAR(texel(7, 7, 0), 0.0f, 2e-3f) << "the open map around the ring stays empty";
}
TEST_F(PaintLayersEffectTest, a_channel_split_kernel_lays_the_slot_channel_as_a_grey)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  Image *orm = add_channel_image("Split");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, orm));
  /* The registry's slot flag hands the image to the kernel. */
  EXPECT_EQ(paint_layer_effect_slot_image(*split), orm);

  ImBuf *input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 0] = 0.9f;
    pixels[i * 4 + 1] = 0.1f;
    pixels[i * 4 + 2] = 0.2f;
    pixels[i * 4 + 3] = 0.5f;
  }
  paint_layer_effect_kernel_apply(*input, *split, false, PAINT_TILE_NUMBER_BASE);
  const float *result = input->float_data();
  const auto texel = [&](const int x, const int y) { return result + (int64_t(y) * 8 + x) * 4; };
  /* The red channel of the map, every texel the one value. A content effect lays a layer of its
   * own, so the map covers fully whatever alpha the input had. */
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      EXPECT_NEAR(texel(x, y)[0], 25.0f / 255.0f, 5e-3f) << "red at " << x << "," << y;
      EXPECT_NEAR(texel(x, y)[1], texel(x, y)[0], 1e-6f) << "grey at " << x << "," << y;
      EXPECT_NEAR(texel(x, y)[3], 1.0f, 1e-6f) << "alpha at " << x << "," << y;
    }
  }
  IMB_freeImBuf(input);

  /* The mask form keeps the input's alpha: its grey carries none of its own. */
  input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 3] = 0.5f;
  }
  paint_layer_effect_kernel_apply(*input, *split, true, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_NEAR(input->float_data()[i * 4 + 3], 0.5f, 1e-6f) << "mask alpha, texel " << i;
  }
  IMB_freeImBuf(input);

  /* The green answers the same way, one parameter apart. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*split,
                                             PAINT_LAYER_CHANNEL_SPLIT_CHANNEL,
                                             PAINT_LAYER_CHANNEL_SPLIT_GREEN));
  input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 3] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *split, false, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_NEAR(input->float_data()[i * 4], 128.0f / 255.0f, 5e-3f) << "texel " << i;
  }
  IMB_freeImBuf(input);

  /* The blue is the third channel of the packing, the one Metallic reads. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*split,
                                             PAINT_LAYER_CHANNEL_SPLIT_CHANNEL,
                                             PAINT_LAYER_CHANNEL_SPLIT_BLUE));
  input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 3] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *split, false, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_NEAR(input->float_data()[i * 4], 204.0f / 255.0f, 5e-3f) << "texel " << i;
  }
  IMB_freeImBuf(input);

  /* The alpha of the map is a channel the split reads too: the right half was written 64, the
   * left one stays opaque. */
  ASSERT_TRUE(paint_layer_kind_param_int_set(*split,
                                             PAINT_LAYER_CHANNEL_SPLIT_CHANNEL,
                                             PAINT_LAYER_CHANNEL_SPLIT_ALPHA));
  input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 3] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *split, false, PAINT_TILE_NUMBER_BASE);
  result = input->float_data();
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      const float expected = x >= 4 ? 64.0f / 255.0f : 1.0f;
      EXPECT_NEAR(texel(x, y)[0], expected, 5e-3f) << "alpha at " << x << "," << y;
      EXPECT_NEAR(texel(x, y)[1], texel(x, y)[0], 1e-6f) << "grey at " << x << "," << y;
    }
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_channel_split_with_an_empty_slot_is_the_identity)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  EXPECT_EQ(paint_layer_effect_slot_image(*split), nullptr);

  ImBuf *input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8 * 4)) {
    pixels[i] = float(i % 4) * 0.25f;
  }
  paint_layer_effect_kernel_apply(*input, *split, false, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(8) * 8 * 4)) {
    EXPECT_NEAR(input->float_data()[i], float(i % 4) * 0.25f, 1e-6f) << "component " << i;
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, a_channel_split_lays_the_slot_channel_in_the_channels_the_row_carries)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  /* The filter takes the first channel of the set, and a split with no slot lays nothing of its
   * own: the row shows as it was. */
  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 2), 1.0f, 5e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 5), 0.0f, 5e-3f);

  /* The map assigned, its red extracted: the row's Base Color is the map's red everywhere the row
   * covers, and a second render reads the new slot -- the hash the cache keys on moved with it. */
  Image *orm = add_channel_image("Split");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, orm));
  ASSERT_TRUE(render_channel(pixels, width, height));
  for (const int y : IndexRange(height)) {
    for (const int x : IndexRange(width)) {
      EXPECT_NEAR(linear_pixel(pixels, width, x, y), 25.0f / 255.0f, 5e-3f)
          << "the red of the map at " << x << "," << y;
    }
  }

  /* Detaching the slot brings the row's own content back: the entry the empty slot keys on is
   * another one the cache answers. */
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, nullptr));
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 2), 1.0f, 5e-3f);
  EXPECT_NEAR(linear_pixel(pixels, width, 6, 5), 0.0f, 5e-3f);
}

TEST_F(PaintLayersEffectTest, a_channel_split_reads_the_udim_tile_it_is_computed_for)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *slot = BKE_image_add_generated(
      bmain, 8, 8, "TiledSlot", 32, false, IMA_GENTYPE_BLANK, black, false, true, true);
  ASSERT_NE(slot, nullptr);
  ImageTile *second_tile = BKE_image_add_tile(slot, 1002, nullptr);
  ASSERT_NE(second_tile, nullptr);
  /* A tile added by number carries the generator defaults, never filled: size it like the base
   * tile, or its acquire has no 8x8 buffer to read. */
  second_tile->gen_x = 8;
  second_tile->gen_y = 8;
  second_tile->gen_type = IMA_GENTYPE_BLANK;
  second_tile->gen_depth = 32;
  copy_v4_v4(second_tile->gen_color, black);
  ASSERT_TRUE(BKE_image_fill_tile(slot, second_tile));
  const auto paint_tile = [&](const int tile_number, const uchar value) {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(slot, &iuser, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(ibuf->x) * ibuf->y)) {
      pixels[i * 4 + 0] = value;
      pixels[i * 4 + 1] = value;
      pixels[i * 4 + 2] = value;
      pixels[i * 4 + 3] = 255;
    }
    BKE_image_release_ibuf(slot, ibuf, lock);
  };
  paint_tile(1001, 25);
  paint_tile(1002, 200);
  BKE_image_partial_update_mark_full_update(slot);
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));

  ImBuf *input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 3] = 1.0f;
  }
  /* The kernel follows the tile it is handed, not the base one: the second tile's 200 comes out,
   * not the first tile's 25. */
  paint_layer_effect_kernel_apply(*input, *split, false, 1002);
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_NEAR(input->float_data()[i * 4], 200.0f / 255.0f, 5e-3f) << "texel " << i;
  }
  IMB_freeImBuf(input);
}

TEST_F(PaintLayersEffectTest, assigning_a_slot_leaves_the_images_colorspace_alone)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  Image *slot = add_channel_image("Split");
  const char *srgb = IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DEFAULT_BYTE);
  ASSERT_NE(srgb, nullptr);
  STRNCPY(slot->colorspace_settings.name, srgb);

  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));
  /* The slot may be shared with users that read it as colour: the setter does not retag it, the
   * kernel decodes it as data instead. */
  EXPECT_STREQ(slot->colorspace_settings.name, srgb);

  /* The data decode is what reaches the kernel: an sRGB read would gamma-decode 25 far below the
   * stored byte. */
  ImBuf *input = IMB_allocImBuf(8, 8, ImBufFlags::FloatData);
  float *pixels = input->float_data_for_write();
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    pixels[i * 4 + 3] = 1.0f;
  }
  paint_layer_effect_kernel_apply(*input, *split, false, PAINT_TILE_NUMBER_BASE);
  for (const int64_t i : IndexRange(int64_t(8) * 8)) {
    EXPECT_NEAR(input->float_data()[i * 4], 25.0f / 255.0f, 5e-3f) << "texel " << i;
  }
  IMB_freeImBuf(input);

  /* Assigning the same image again is idempotent in ownership and leaves the space alone. */
  const int users = slot->id.us;
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));
  EXPECT_EQ(slot->id.us, users);
  EXPECT_STREQ(slot->colorspace_settings.name, srgb);

  /* Detaching releases the user and empties the slot. */
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, nullptr));
  EXPECT_EQ(paint_layer_effect_slot_image(*split), nullptr);
}

TEST_F(PaintLayersEffectTest, effect_image_set_refuses_a_foreign_row_and_a_kind_without_a_slot)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  Image *image = add_channel_image("Split");
  EXPECT_FALSE(BKE_paint_layers_effect_image_set(*ma, blur, image));
  EXPECT_EQ(blur->effect_image, nullptr);

  /* A slot-reading row of another material is not part of this material's stack either. */
  Material *other = BKE_material_add(bmain, "OtherSplitMat");
  MaterialPaintLayer *other_row = BKE_paint_layers_add(
      *other, "Other", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *other_split = BKE_paint_layers_filter_add(
      *other, other_row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  ASSERT_NE(other_split, nullptr);
  EXPECT_FALSE(BKE_paint_layers_effect_image_set(*ma, other_split, image));
  EXPECT_EQ(other_split->effect_image, nullptr);
}

TEST_F(PaintLayersEffectTest, a_filter_row_cannot_switch_source_away_from_its_slot)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  Image *slot = add_channel_image("Split");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));
  const int users = slot->id.us;

  /* A filter effect is not one of the row sources a switch rewrites; the kind cannot change, so the
   * slot has no path that would leave it held by a row that no longer reads it. */
  EXPECT_FALSE(BKE_paint_layers_set_source(*ma, split, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_EQ(paint_layer_effect_slot_image(*split), slot);
  EXPECT_EQ(slot->id.us, users);
}

TEST_F(PaintLayersEffectTest, a_channel_split_copy_takes_and_releases_the_slot_user)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  Image *slot = add_channel_image("Split");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));
  const int users = slot->id.us;

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, split);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(paint_layer_effect_slot_image(*copy), slot);
  EXPECT_EQ(slot->id.us, users + 1);

  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, copy));
  EXPECT_EQ(slot->id.us, users);
}

TEST_F(PaintLayersEffectTest, editing_the_slot_pixels_invalidates_the_effect_cache_and_bake)
{
  Image *slot = add_channel_image("Split");
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));

  Vector<float> pixels;
  int width = 0;
  int height = 0;
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 2), 25.0f / 255.0f, 5e-3f);

  clear_active();
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(row->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* A pixel edit of the slot is the subscription the common append takes: the cache entry and the
   * bake it feeds both go stale. */
  void *lock = nullptr;
  ImBuf *slot_buffer = BKE_image_acquire_ibuf(slot, nullptr, &lock);
  ASSERT_NE(slot_buffer, nullptr);
  uchar *slot_pixels = slot_buffer->byte_data_for_write();
  for (const int64_t i : IndexRange(int64_t(slot_buffer->x) * slot_buffer->y)) {
    slot_pixels[i * 4 + 0] = 200;
  }
  BKE_image_release_ibuf(slot, slot_buffer, lock);
  BKE_image_partial_update_mark_full_update(slot);
  BKE_paint_layers_bake_notice_changes(*ma);

  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "stale: the slot edit reaches the bake through its subscription";
  ASSERT_TRUE(render_channel(pixels, width, height));
  EXPECT_NEAR(linear_pixel(pixels, width, 2, 2), 200.0f / 255.0f, 5e-3f);
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
  const auto blur_directional = [](const float angle, const int radius, const float mix) {
    return std::array<float, 3>{angle, float(radius), mix};
  };
  const auto glow = [](const int mode, const int width, const float spread, const float opacity) {
    return std::array<float, 7>{float(mode), float(width), spread, opacity, 1.0f, 1.0f, 1.0f};
  };
  const auto pixelate = [](const int amount) {
    return std::array<float, 1>{float(amount)};
  };
  const auto height_to_normal = [](const float strength,
                                   const int world_units,
                                   const float surface_size,
                                   const float height_depth,
                                   const int flip_green) {
    return std::array<float, 5>{
        strength, float(world_units), surface_size, height_depth, float(flip_green)};
  };
  const auto channel_split = [](const int channel) {
    return std::array<float, 1>{float(channel)};
  };
  const auto bevel = [](const int source, const int distance, const int corner, const int smooth) {
    return std::array<float, 4>{float(source), float(distance), float(corner), float(smooth)};
  };
  const auto mask_outline = [](const int position,
                               const int start_black,
                               const float threshold,
                               const int width,
                               const int blur,
                               const float shape,
                               const int source) {
    return std::array<float, 10>{float(position),
                                 float(start_black),
                                 threshold,
                                 float(width),
                                 float(blur),
                                 shape,
                                 float(source),
                                 1.0f,
                                 1.0f,
                                 1.0f};
  };
  const auto drop_shadow = [](const int source,
                              const float angle,
                              const int distance,
                              const int size,
                              const int spread,
                              const float opacity) {
    return std::array<float, 9>{
        float(source), angle, float(distance), float(size), float(spread), opacity, 1.0f, 0.0f, 0.0f};
  };
  const auto blur_slope = [](const int seed,
                             const float intensity,
                             const int quality,
                             const int blending,
                             const int source_mode,
                             const int source_blur) {
    return std::array<float, 9>{float(seed),
                                intensity,
                                float(quality),
                                float(blending),
                                float(source_mode),
                                float(source_blur),
                                0.0f,
                                1.0f,
                                1.0f};
  };
  /* The Color Match's schema packs the two colours across three slots each: the picked flag, the
   * source colour, the target colour, then the variation switch and its three amounts. */
  const auto color_match = [](const int picked, const int variation) {
    return std::array<float, 11>{float(picked),
                                 0.5f,
                                 0.5f,
                                 0.5f,
                                 0.25f,
                                 0.5f,
                                 0.75f,
                                 float(variation),
                                 0.1f,
                                 0.2f,
                                 0.3f};
  };
  const auto fill_area = [](const int source,
                            const float threshold,
                            const int debug,
                            const int uv_border,
                            const int color_mode,
                            const int blur,
                            const int iterations) {
    return std::array<float, 10>{float(source),
                                 threshold,
                                 float(debug),
                                 float(uv_border),
                                 float(color_mode),
                                 1.0f,
                                 0.5f,
                                 0.25f,
                                 float(blur),
                                 float(iterations)};
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
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_FXAA, nullptr, 100, 100), 1);
  /* The directional line reaches its radius along the line, whichever way the angle points. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(
                MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL, blur_directional(30.0f, 7, 1.0f).data(), 100, 100),
            7);
  /* A glow's halo is its blur's: the luminance and the choke are pointwise, and the tail computes
   * its own texel, so the pairs the mode spends are the whole reach. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_GLOW,
                                         glow(PAINT_LAYER_GLOW_PRECISE, 5, 0.0f, 1.0f).data(),
                                         100,
                                         100),
            5);
  EXPECT_EQ(BKE_paint_layers_effect_halo(
                MA_PAINT_LAYER_FILTER_GLOW, glow(PAINT_LAYER_GLOW_SOFT, 8, 0.0f, 1.0f).data(), 100, 100),
            12) << "three pairs of max(8 / 2, 1), the gaussian approximation's";
  /* A Height To Normal is the one 3x3: a texel of reach on both axes. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL,
                                         height_to_normal(2.0f, 0, 100.0f, 5.0f, 0).data(),
                                         100,
                                         100),
            1);
  /* A pixelate and a slot fetch read their own texel and nothing else. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_PIXELATE, pixelate(16).data(), 100, 100), 0);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT,
                                         channel_split(PAINT_LAYER_CHANNEL_SPLIT_BLUE).data(),
                                         100,
                                         100),
            0);
  /* The distance transforms are whole-canvas: JFA is not local, so every pass of the flood
   * reaches the whole canvas, the seed pass and the field's read included -- eight passes of the
   * hundred-texel canvas here, the smooth blur's pair on top when it runs. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(
                MA_PAINT_LAYER_FILTER_BEVEL, bevel(PAINT_LAYER_BEVEL_SOURCE_ALPHA, 2, 0, 0).data(), 100, 100),
            800);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_BEVEL,
                                         bevel(PAINT_LAYER_BEVEL_SOURCE_LUMINANCE, 2, 0, 4).data(),
                                         100,
                                         100),
            804);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_MASK_OUTLINE,
                                         mask_outline(PAINT_LAYER_MASK_OUTLINE_OUTSIDE,
                                                      0,
                                                      0.5f,
                                                      4,
                                                      0,
                                                      0.5f,
                                                      PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA)
                                             .data(),
                                         100,
                                         100),
            800);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_MASK_OUTLINE,
                                         mask_outline(PAINT_LAYER_MASK_OUTLINE_OUTSIDE,
                                                      0,
                                                      0.5f,
                                                      4,
                                                      3,
                                                      0.5f,
                                                      PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA)
                                             .data(),
                                         100,
                                         100),
            803);
  /* A Drop Shadow is its blur and its spread, plus the offset the cast moves the tail by: the
   * angle of zero casts along +X, so the offset is the whole distance on X and none on Y, the
   * reach the larger of the two. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_DROP_SHADOW,
                                         drop_shadow(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA,
                                                     0.0f,
                                                     2,
                                                     8,
                                                     0,
                                                     1.0f)
                                             .data(),
                                         100,
                                         100),
            10);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_DROP_SHADOW,
                                         drop_shadow(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA,
                                                     0.0f,
                                                     2,
                                                     0,
                                                     -3,
                                                     1.0f)
                                             .data(),
                                         100,
                                         100),
            5) << "an eroding spread spends its pair like a dilating one, the reach its absolute";
  /* A Blur Slope's reach is the gradient's one texel of the driver; the taps read the original
   * canvas, which is not a pass output and grows nothing. The driver's blur joins it when the
   * source mode is the previous input and the blur is on. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_BLUR_SLOPE,
                                         blur_slope(5, 15.0f, 8, PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR, PAINT_LAYER_SLOPE_SOURCE_NOISE, 0)
                                             .data(),
                                         100,
                                         100),
            1);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_BLUR_SLOPE,
                                         blur_slope(5,
                                                    15.0f,
                                                    8,
                                                    PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR,
                                                    PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT,
                                                    5)
                                             .data(),
                                         100,
                                         100),
            6);
  /* The reductions and the flood are whole-canvas: the average of any block and the label of any
   * region are fed by the whole map, so every pass of the chains reaches the whole canvas -- the
   * hundred-texel canvas gives the reduce chain six passes of a hundred (the first level, the five
   * steps down to the 1x1 apex) and the flood thirty-three (the seeding and sixteen sweep pairs), the
   * diffusion's one-texel reaches and the tail's none riding on top. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_COLOR_MATCH,
                                         color_match(0, 0).data(),
                                         100,
                                         100),
            600);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_FILL_AREA,
                                         fill_area(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA,
                                                   0.5f,
                                                   0,
                                                   PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                   PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE,
                                                   0,
                                                   24)
                                             .data(),
                                         100,
                                         100),
            3324);
  /* The solid mode skips the diffusion, and the blur's pair rides on top of either chain. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_FILL_AREA,
                                         fill_area(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA,
                                                   0.5f,
                                                   0,
                                                   PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                   PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID,
                                                   0,
                                                   24)
                                             .data(),
                                         100,
                                         100),
            3300);
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_FILL_AREA,
                                         fill_area(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA,
                                                   0.5f,
                                                   0,
                                                   PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                   PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE,
                                                   3,
                                                   24)
                                             .data(),
                                         100,
                                         100),
            3327);
  /* A pointwise kind has no neighbourhood to reach. */
  EXPECT_EQ(BKE_paint_layers_effect_halo(MA_PAINT_LAYER_FILTER_NOISE, nullptr, 100, 100), 0);
}

TEST_F(PaintLayersEffectTest, every_filter_declares_its_reach)
{
  /* The table is a decision record, not a derivation: every value of #eMaterialPaintLayerFilter
   * must appear with the reach the coverage culling reads. A filter added to the enum without an
   * entry fails the loop below instead of silently classifying as a whole-map effect. */
  struct ReachCase {
    eMaterialPaintLayerFilter filter;
    EffectReach reach;
  };
  const ReachCase cases[] = {
      {MA_PAINT_LAYER_FILTER_INVERT, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_LEVELS, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_HSL, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_COLOR_ID, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_UV_ISLAND, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_BRIGHTNESS_CONTRAST, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_COLOR_RAMP, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_POSTERIZE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_MATH, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_THRESHOLD, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_CLAMP, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_CURVES, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_COLOR_BALANCE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_BLUR, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_SHARPEN, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_HIGH_PASS, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_EDGE_DETECT, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_EMBOSS, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_ERODE_DILATE, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_GLOW, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_BEVEL, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_COLOR_MATCH, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_EXPOSURE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_DEBAND, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_NOISE, EffectReach::Pointwise},
      {MA_PAINT_LAYER_FILTER_NORMAL_CONVERT, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_CHANNEL_MIXER, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_TRANSFORM, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_MIRROR, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_DROP_SHADOW, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_MASK_OUTLINE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_FILL_AREA, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_SMOOTHSTEP, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_HEIGHT_ADJUST, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_PIXELATE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_HSL_PERSPECTIVE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_FXAA, EffectReach::Neighbourhood},
      {MA_PAINT_LAYER_FILTER_BLUR_SLOPE, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT, EffectReach::WholeMap},
      {MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK, EffectReach::WholeMap},
  };
  const float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  /* The enum has no count sentinel (DNA values are explicit), so the loop bound is the last filter:
   * a filter added after it must move this bound together with its table entry. */
  constexpr int last_filter = int(MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
  for (int value = 1; value <= last_filter; value++) {
    const auto *found = std::find_if(std::begin(cases), std::end(cases), [&](const ReachCase &c) {
      return int(c.filter) == value;
    });
    ASSERT_NE(found, std::end(cases))
        << "filter " << value << " has no declared reach; add it to this table";
    EXPECT_EQ(BKE_paint_layers_effect_reach(eMaterialPaintLayerFilter(value), params), found->reach)
        << "filter " << value;
  }
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

  /* A directional blur is one pass of one kernel: the line average at the angle, whose direction
   * and mix ride in the effect's parameters, its reach the clamped radius. */
  const std::array<float, 3> directional{30.0f, 7.0f, 1.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL, directional.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::BlurDirectional);
  EXPECT_EQ(passes[0].radius, 7);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 7);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);

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

  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_FXAA, nullptr, 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Fxaa);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 1);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);

  /* A glow is the luminance choked, blurred and screened under the colour: the two pointwise
   * passes first, the blur pairs the mode names, and the tail reads the original against the
   * blurred band. The parameters are packed in the schema order, a colour laying its three
   * channels across three slots. */
  const std::array<float, 7> glow_soft{
      float(PAINT_LAYER_GLOW_SOFT), 8.0f, 0.2f, 1.0f, 1.0f, 0.5f, 0.25f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_GLOW, glow_soft.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 9) << "luminance, choke, three blur pairs and the screen tail";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Luminance);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].reach, 0);
  EXPECT_EQ(passes[1].kind, ePaintLayerEffectPass::Choke);
  EXPECT_EQ(passes[1].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_NEAR(passes[1].aux0, 0.2f, 1e-6f) << "the spread clamps the luminance to the glow band";
  EXPECT_EQ(passes[1].reach, 0);
  for (int i = 2; i < 8; i++) {
    EXPECT_EQ(passes[i].radius, 4) << "a soft glow spends the width over three pairs, max(8 / 2, 1)";
    EXPECT_EQ(passes[i].reach, 4);
  }
  EXPECT_EQ(passes[2].source, ePaintLayerEffectPassSource::Previous)
      << "the blur reads the choked band, not the raw input";
  EXPECT_EQ(passes[2].axis_mask, PAINT_LAYER_EFFECT_AXIS_X);
  EXPECT_EQ(passes[3].axis_mask, PAINT_LAYER_EFFECT_AXIS_Y);
  EXPECT_EQ(passes[8].kind, ePaintLayerEffectPass::ScreenTail);
  EXPECT_EQ(passes[8].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[8].aux, ePaintLayerEffectPassAux::Previous);
  EXPECT_NEAR(passes[8].aux0, 1.0f, 1e-6f) << "the opacity rides in aux0";
  EXPECT_NEAR(passes[8].params.x, 1.0f, 1e-6f);
  EXPECT_NEAR(passes[8].params.y, 0.5f, 1e-6f);
  EXPECT_NEAR(passes[8].params.z, 0.25f, 1e-6f) << "the tint rides in params, the packing's three slots";
  EXPECT_EQ(passes[8].reach, 0);

  /* A precise glow spends one box pass of the whole width, the tail after it. */
  const std::array<float, 7> glow_precise{
      float(PAINT_LAYER_GLOW_PRECISE), 5.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_GLOW, glow_precise.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 5);
  EXPECT_EQ(passes[2].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[2].radius, 5);
  EXPECT_EQ(passes[3].kind, ePaintLayerEffectPass::BlurV);
  EXPECT_EQ(passes[3].radius, 5);
  EXPECT_EQ(passes[4].kind, ePaintLayerEffectPass::ScreenTail);
  EXPECT_NEAR(passes[4].aux0, 0.5f, 1e-6f);

  /* A pixelate is one pass: the amount rides in aux0, and the kernel cuts the canvas into that
   * many blocks per side like the CPU does, the last one of a side shorter. */
  const std::array<float, 1> pixelate{16.0f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_PIXELATE, pixelate.data(), 64, 64, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::BlockAverage);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 0);
  EXPECT_NEAR(passes[0].aux0, 16.0f, 1e-6f) << "the amount is the block count per side";

  /* A Height To Normal is one 3x3 pass: the slope scale in aux0 -- the strength, or the physical
   * slope the world units name -- and the green convention beside it in params. */
  const std::array<float, 5> h2n_local{2.0f, 0.0f, 100.0f, 5.0f, 0.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL, h2n_local.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::HeightToNormal3x3);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 1);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);
  EXPECT_NEAR(passes[0].aux0, 2.0f, 1e-6f) << "the strength is the scale without the world units";
  EXPECT_NEAR(passes[0].params.x, 1.0f, 1e-6f)
      << "the OpenGL encoding leans the green against the slope";

  /* The world units turn the scale into the physical slope: the height depth against the texel the
   * wider side covers, the half turning the Sobel's doubled gradient into it. */
  const std::array<float, 5> h2n_world{1.0f, 1.0f, 100.0f, 5.0f, 1.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL, h2n_world.data(), 200, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_NEAR(passes[0].aux0, 5.0f, 1e-6f)
      << "texel_cm is 100 / max(200, 100) = 0.5, so the slope is 0.5 * 5 / 0.5";
  EXPECT_NEAR(passes[0].params.x, -1.0f, 1e-6f) << "the DirectX encoding flips the green";

  /* A Channel Split is one pass that reads the slot map the aux names: the channel index rides in
   * aux0, clamped the way the kernel clamps it. */
  const std::array<float, 1> split{float(PAINT_LAYER_CHANNEL_SPLIT_BLUE)};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT, split.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 1);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::SlotFetch);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::Slot);
  EXPECT_EQ(passes[0].reach, 0);
  EXPECT_NEAR(passes[0].aux0, float(PAINT_LAYER_CHANNEL_SPLIT_BLUE), 1e-6f);

  /* A Bevel floods the distance field and reads the ramp off it: the seed pass, the halving steps
   * of the standard JFA (half the canvas down to one) and the field's read, each of them reaching
   * the whole canvas -- JFA is not local, so the halo and the regions of these kinds land on the
   * whole canvas. The seed pass names the scalar (the alpha here) and the half the CPU seeds by;
   * the ramp's divisor is the distance parameter, and the mode names the ramp. */
  const std::array<float, 4> bevel_alpha{float(PAINT_LAYER_BEVEL_SOURCE_ALPHA), 2.0f, 0.0f, 0.0f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_BEVEL, bevel_alpha.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 8) << "the seed pass, six flooding steps and the field's read";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::JfaInit);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].radius, 0);
  EXPECT_EQ(passes[0].reach, 100);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);
  EXPECT_NEAR(passes[0].params.x, 0.0f, 1e-6f) << "the alpha source rides in params";
  EXPECT_NEAR(passes[0].params.y, 0.5f, 1e-6f) << "the threshold the CPU seeds by";
  int step_index = 1;
  for (int step = 50; step >= 1; step /= 2) {
    EXPECT_EQ(passes[step_index].kind, ePaintLayerEffectPass::JfaStep);
    EXPECT_EQ(passes[step_index].radius, step) << "the standard JFA's halving radius";
    EXPECT_EQ(passes[step_index].source, ePaintLayerEffectPassSource::Previous);
    EXPECT_EQ(passes[step_index].reach, 100);
    step_index++;
  }
  EXPECT_EQ(passes[7].kind, ePaintLayerEffectPass::DistEval);
  EXPECT_EQ(passes[7].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_EQ(passes[7].reach, 100);
  EXPECT_NEAR(passes[7].params.x, 2.0f, 1e-6f) << "the ramp's divisor is the distance parameter";
  EXPECT_NEAR(passes[7].params.z, 0.0f, 1e-6f) << "the ramp mode writes a grey, the alpha carried";

  /* The luminance source names the other scalar; the smooth blur lays its pair over the ramp. */
  const std::array<float, 4> bevel_luma{float(PAINT_LAYER_BEVEL_SOURCE_LUMINANCE), 2.0f, 0.0f, 3.0f};
  BKE_paint_layers_effect_pass_plan(MA_PAINT_LAYER_FILTER_BEVEL, bevel_luma.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 10);
  EXPECT_NEAR(passes[0].params.x, 1.0f, 1e-6f);
  EXPECT_EQ(passes[8].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[8].radius, 3);
  EXPECT_EQ(passes[9].kind, ePaintLayerEffectPass::BlurV);
  EXPECT_EQ(passes[9].radius, 3);

  /* A Mask Outline is the same field with the band's read instead of the ramp: the band's divisor
   * is the width, the bend and the band mode ride beside it, and the tint tail premultiplies the
   * blurred band with the band as its own alpha -- white here, the colour the alpha source forces
   * the CPU's own tail to. */
  const std::array<float, 10> outline{
      float(PAINT_LAYER_MASK_OUTLINE_OUTSIDE), 0.0f, 0.5f, 4.0f, 0.0f, 0.5f,
      float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA), 1.0f, 1.0f, 1.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_MASK_OUTLINE, outline.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 9) << "the seed pass, six flooding steps, the band's read and the tail";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::JfaInit);
  EXPECT_NEAR(passes[0].params.x, float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA), 1e-6f);
  EXPECT_NEAR(passes[0].params.y, 0.5f, 1e-6f) << "the threshold rides in params";
  EXPECT_EQ(passes[7].kind, ePaintLayerEffectPass::DistEval);
  EXPECT_NEAR(passes[7].params.x, 4.0f, 1e-6f) << "the band's divisor is the width parameter";
  EXPECT_NEAR(passes[7].params.y, 0.5f, 1e-6f) << "the falloff's bend rides in params";
  EXPECT_NEAR(passes[7].params.z, 1.0f, 1e-6f) << "the band mode writes its own alpha";
  EXPECT_EQ(passes[8].kind, ePaintLayerEffectPass::BandTintTail);
  EXPECT_EQ(passes[8].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_EQ(passes[8].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[8].reach, 0);
  EXPECT_TRUE(passes[8].alpha_from_kernel) << "the ring layer's alpha is the band";
  EXPECT_NEAR(passes[8].params2.x, 1.0f, 1e-6f) << "the tint is the white the alpha source forces";
  EXPECT_NEAR(passes[8].params2.w, 0.0f, 1e-6f);

  /* A luminance source keeps the schema's colour, and the blur lays its pair before the tail. */
  const std::array<float, 10> outline_luma{
      float(PAINT_LAYER_MASK_OUTLINE_OUTSIDE), 0.0f, 0.5f, 4.0f, 2.0f, 0.5f,
      float(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE), 1.0f, 0.5f, 0.25f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_MASK_OUTLINE, outline_luma.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 11);
  EXPECT_EQ(passes[8].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[8].radius, 2);
  EXPECT_EQ(passes[9].kind, ePaintLayerEffectPass::BlurV);
  EXPECT_EQ(passes[9].radius, 2);
  EXPECT_EQ(passes[10].kind, ePaintLayerEffectPass::BandTintTail);
  EXPECT_NEAR(passes[10].params2.x, 1.0f, 1e-6f) << "the schema's tint rides in params2";
  EXPECT_NEAR(passes[10].params2.y, 0.5f, 1e-6f);
  EXPECT_NEAR(passes[10].params2.z, 0.25f, 1e-6f);

  /* A Drop Shadow is the silhouette, its spread's extremum pair, its blur, and the tail that
   * reads the blurred silhouette at the offset the angle casts. The angle of zero casts along
   * +X, so the tail's reach is the whole offset on X and none on Y, the larger of the two. */
  const std::array<float, 9> shadow_alpha{
      float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA), 0.0f, 2.0f, 8.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_DROP_SHADOW, shadow_alpha.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 4) << "the silhouette, the blur pair and the tail";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::Silhouette);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].reach, 0);
  EXPECT_NEAR(passes[0].params.x, float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA), 1e-6f)
      << "the scalar the silhouette greys rides in params";
  EXPECT_EQ(passes[1].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[1].radius, 8);
  EXPECT_EQ(passes[2].kind, ePaintLayerEffectPass::BlurV);
  EXPECT_EQ(passes[2].radius, 8);
  EXPECT_EQ(passes[3].kind, ePaintLayerEffectPass::OffsetTintTail);
  EXPECT_EQ(passes[3].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[3].aux, ePaintLayerEffectPassAux::Previous);
  EXPECT_EQ(passes[3].reach, 2) << "the tail reads the silhouette the cast's offset lands on";
  EXPECT_NEAR(passes[3].params.x, 1.0f, 1e-5f) << "the cast's direction, the angle's cosine";
  EXPECT_NEAR(passes[3].params.y, 0.0f, 1e-5f);
  EXPECT_NEAR(passes[3].params.z, 2.0f, 1e-6f) << "the offset distance rides in params";
  EXPECT_NEAR(passes[3].params.w, 0.5f, 1e-6f) << "the opacity rides in params";
  EXPECT_NEAR(passes[3].params2.x, 1.0f, 1e-6f) << "the tint rides in params2";
  EXPECT_NEAR(passes[3].params2.w, 1.0f, 1e-6f) << "the alpha source keeps the shadow off the content";
  EXPECT_TRUE(passes[3].alpha_from_kernel) << "the shadow layer's alpha is the weight";

  /* A negative spread erodes (the extremum pair's minimum), a positive one dilates; a zero one
   * spends no pair at all. */
  const std::array<float, 9> shadow_spread{
      float(PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA), 0.0f, 2.0f, 0.0f, -3.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_DROP_SHADOW, shadow_spread.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 4) << "the silhouette, the spread's pair and the tail";
  EXPECT_EQ(passes[1].kind, ePaintLayerEffectPass::ExtremumH);
  EXPECT_FALSE(passes[1].maximum) << "a negative spread erodes";
  EXPECT_EQ(passes[1].radius, 3);
  EXPECT_EQ(passes[2].kind, ePaintLayerEffectPass::ExtremumV);
  EXPECT_FALSE(passes[2].maximum);
  EXPECT_EQ(passes[3].kind, ePaintLayerEffectPass::OffsetTintTail);
  EXPECT_EQ(passes[3].aux, ePaintLayerEffectPassAux::Previous)
      << "the tail reads the spread silhouette, the blur being zero";

  /* The luminance source lays the shadow wherever the silhouette covers (the under flag zero),
   * and the angle of a quarter turn casts along +Y. */
  const std::array<float, 9> shadow_luma{
      float(PAINT_LAYER_DROP_SHADOW_SOURCE_LUMINANCE), 90.0f, 2.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_DROP_SHADOW, shadow_luma.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 2) << "no spread and no blur, the silhouette straight into the tail";
  EXPECT_NEAR(passes[1].params.x, 0.0f, 1e-5f) << "the quarter turn casts along +Y";
  EXPECT_NEAR(passes[1].params.y, 1.0f, 1e-5f);
  EXPECT_EQ(passes[1].reach, 2);
  EXPECT_NEAR(passes[1].params2.w, 0.0f, 1e-6f) << "the luminance source lays it under everything";

  /* A Blur Slope builds the driver and walks it: no blur pair on the noise mode, the mode riding
   * in aux0 the CPU's own enum values, the re-grade and the noise's seed and tiles in params. The
   * walk's radius is the tap count, the blend mode and the span ride in params, and the taps
   * read the original canvas through the aux. */
  const std::array<float, 9> slope_noise{5.0f, 15.0f, 8.0f, 0.0f, float(PAINT_LAYER_SLOPE_SOURCE_NOISE), 0.0f, 0.25f, 1.5f, 2.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_BLUR_SLOPE, slope_noise.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 2) << "the driver and the walk";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::SlopeDriver);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 0);
  EXPECT_NEAR(passes[0].aux0, float(PAINT_LAYER_SLOPE_SOURCE_NOISE), 1e-6f)
      << "the mode rides in aux0, the CPU's own enum";
  EXPECT_NEAR(passes[0].params.x, 0.25f, 1e-6f) << "the midpoint the source shifts by";
  EXPECT_NEAR(passes[0].params.y, 1.5f, 1e-6f) << "the contrast";
  EXPECT_NEAR(passes[0].params.z, 5.0f, 1e-6f) << "the noise's seed";
  EXPECT_NEAR(passes[0].params.w, 2.0f, 1e-6f) << "the tiles the noise covers the map with";
  EXPECT_EQ(passes[1].kind, ePaintLayerEffectPass::SlopeWalk);
  EXPECT_EQ(passes[1].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_EQ(passes[1].aux, ePaintLayerEffectPassAux::Original) << "the taps read the original canvas";
  EXPECT_EQ(passes[1].radius, 8) << "the walk's radius is the tap count the quality names";
  EXPECT_EQ(passes[1].reach, 1) << "the gradient the walk follows reads one texel past";
  EXPECT_NEAR(passes[1].params.x, 0.0f, 1e-6f) << "the blend mode rides in params";
  EXPECT_NEAR(passes[1].params.y, 15.0f, 1e-6f) << "the span rides in params";

  /* The previous-input mode blurs the input before the driver reads it, the blur riding only
   * there -- the noise has no detail of its own to soften. */
  const std::array<float, 9> slope_input{5.0f,
                                         15.0f,
                                         8.0f,
                                         float(PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX),
                                         float(PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT),
                                         4.0f,
                                         0.0f,
                                         1.0f,
                                         1.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_BLUR_SLOPE, slope_input.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 4);
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[0].radius, 4);
  EXPECT_EQ(passes[1].kind, ePaintLayerEffectPass::BlurV);
  EXPECT_EQ(passes[1].radius, 4);
  EXPECT_EQ(passes[2].kind, ePaintLayerEffectPass::SlopeDriver);
  EXPECT_EQ(passes[2].source, ePaintLayerEffectPassSource::Previous)
      << "the driver reads the blurred input";
  EXPECT_NEAR(passes[2].aux0, float(PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT), 1e-6f);
  EXPECT_EQ(passes[3].kind, ePaintLayerEffectPass::SlopeWalk);
  EXPECT_NEAR(passes[3].params.x, float(PAINT_LAYER_BLUR_SLOPE_BLENDING_MAX), 1e-6f);

  /* A Color Match reduces the input's average down a pyramid and grades the original against it:
   * the first level halves the canvas, every step halves one more -- its radius naming the level it
   * makes -- down to the 1x1 apex, and the grade reads the original with the apex through its aux.
   * The parameters are packed in the schema order, a colour laying its three channels across three
   * slots: the target colour and the variation switch land on the grade's params, the picked
   * colour's slot in params2 staying empty on the reduce branch. */
  const std::array<float, 11> match_reduce{0.0f,
                                           0.5f,
                                           0.5f,
                                           0.5f,
                                           0.25f,
                                           0.5f,
                                           0.75f,
                                           0.0f,
                                           0.1f,
                                           0.2f,
                                           0.3f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_COLOR_MATCH, match_reduce.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 7) << "the first level, the five steps down to 1x1 and the grade";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::ReduceStart);
  EXPECT_EQ(passes[0].radius, 1);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 100) << "a reduction is fed by the whole map, the JFA's own policy";
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);
  int reduce_index = 1;
  for (int level = 2; level <= 6; level++) {
    EXPECT_EQ(passes[reduce_index].kind, ePaintLayerEffectPass::ReduceStep);
    EXPECT_EQ(passes[reduce_index].radius, level) << "the step names the level it makes";
    EXPECT_EQ(passes[reduce_index].source, ePaintLayerEffectPassSource::Previous);
    EXPECT_EQ(passes[reduce_index].reach, 100);
    EXPECT_EQ(passes[reduce_index].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);
    reduce_index++;
  }
  EXPECT_EQ(passes[6].kind, ePaintLayerEffectPass::MatchGrade);
  EXPECT_EQ(passes[6].source, ePaintLayerEffectPassSource::Input) << "the grade moves the original";
  EXPECT_EQ(passes[6].aux, ePaintLayerEffectPassAux::Field) << "the apex the reduce left";
  EXPECT_EQ(passes[6].reach, 0);
  EXPECT_EQ(passes[6].axis_mask, PAINT_LAYER_EFFECT_AXIS_NONE);
  EXPECT_NEAR(passes[6].params.x, 0.25f, 1e-6f) << "the target colour rides in params";
  EXPECT_NEAR(passes[6].params.y, 0.5f, 1e-6f);
  EXPECT_NEAR(passes[6].params.z, 0.75f, 1e-6f);
  EXPECT_NEAR(passes[6].params.w, 0.0f, 1e-6f) << "the variation switch rides beside it";
  EXPECT_NEAR(passes[6].params2.x, 0.1f, 1e-6f) << "the variation's amounts ride in params2";
  EXPECT_NEAR(passes[6].params2.y, 0.2f, 1e-6f);
  EXPECT_NEAR(passes[6].params2.z, 0.3f, 1e-6f);
  EXPECT_NEAR(passes[6].params2.w, 0.0f, 1e-6f);

  /* A 2048 canvas spends one more level: eleven halvings land on the apex, ten of them steps. */
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_COLOR_MATCH, match_reduce.data(), 2048, 2048, passes);
  ASSERT_EQ(passes.size(), 12) << "the first level, the ten steps and the grade";
  EXPECT_EQ(passes[10].radius, 11) << "the last step makes the 1x1 apex";
  EXPECT_EQ(passes[11].kind, ePaintLayerEffectPass::MatchGrade);
  /* A 4096 canvas: twelve halvings, eleven steps. */
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_COLOR_MATCH, match_reduce.data(), 4096, 4096, passes);
  ASSERT_EQ(passes.size(), 13);
  EXPECT_EQ(passes[11].radius, 12);

  /* A picked source is the CPU's: the plan refuses it with an empty plan, the way an unflagged kind
   * is refused, and the CPU lays the picked match exactly as it always has. */
  const std::array<float, 11> match_picked{1.0f,
                                           0.5f,
                                           0.5f,
                                           0.5f,
                                           0.25f,
                                           0.5f,
                                           0.75f,
                                           0.0f,
                                           0.1f,
                                           0.2f,
                                           0.3f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_COLOR_MATCH, match_picked.data(), 100, 100, passes);
  EXPECT_TRUE(passes.is_empty()) << "the picked match stays with the CPU";

  /* A Fill Area floods the label field and diffuses the border's colours inward: the seeding reads
   * the scalar and the threshold the border crosses from params, the flooding steps halve from half
   * the canvas down to one, each diffusion step is one of the CPU's iterations -- the first reading
   * the original for the border colours, the rest the pass before them, the label field riding the
   * aux -- and the tail writes the fill out with the coverage, its own alpha computed. */
  const std::array<float, 10> fill_default{float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA),
                                           0.5f,
                                           0.0f,
                                           float(PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE),
                                           float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE),
                                           1.0f,
                                           0.5f,
                                           0.25f,
                                           0.0f,
                                           24.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_default.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 58)
      << "the seeding, sixteen sweep pairs, the diffusion's 24 passes and the tail";
  EXPECT_EQ(passes[0].kind, ePaintLayerEffectPass::FloodInit);
  EXPECT_EQ(passes[0].source, ePaintLayerEffectPassSource::Input);
  EXPECT_EQ(passes[0].aux, ePaintLayerEffectPassAux::None);
  EXPECT_EQ(passes[0].reach, 100);
  EXPECT_EQ(passes[0].axis_mask, PAINT_LAYER_EFFECT_AXIS_XY);
  EXPECT_NEAR(passes[0].params.x, float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA), 1e-6f)
      << "the scalar the border is read at rides in params";
  EXPECT_NEAR(passes[0].params.y, 0.5f, 1e-6f) << "the threshold the border crosses";
  /* The flooding is the raster sweep pair, sixteen times over: the mode rides in params (one the
   * rows, two the columns), the radius left empty. */
  for (int flood_index = 1; flood_index <= 32; flood_index++) {
    EXPECT_EQ(passes[flood_index].kind, ePaintLayerEffectPass::FloodStep);
    EXPECT_NEAR(passes[flood_index].params.x, (flood_index % 2 == 1) ? 1.0f : 2.0f, 1e-6f)
        << "the sweeps alternate rows and columns";
    EXPECT_EQ(passes[flood_index].radius, 0);
    EXPECT_EQ(passes[flood_index].source, ePaintLayerEffectPassSource::Previous);
    EXPECT_EQ(passes[flood_index].reach, 100);
  }
  EXPECT_EQ(passes[33].kind, ePaintLayerEffectPass::DiffuseStep);
  EXPECT_EQ(passes[33].radius, 1) << "the diffusion's radius names its iteration";
  EXPECT_EQ(passes[33].source, ePaintLayerEffectPassSource::Input)
      << "the border colours the CPU seeds the walk with come from the original";
  EXPECT_EQ(passes[33].aux, ePaintLayerEffectPassAux::Field) << "the flooding's labels";
  EXPECT_EQ(passes[33].reach, 1) << "a diffusion step reads its four neighbours";
  EXPECT_EQ(passes[34].source, ePaintLayerEffectPassSource::Previous) << "the CPU's ping-pong";
  EXPECT_EQ(passes[34].aux, ePaintLayerEffectPassAux::Field);
  EXPECT_EQ(passes[56].radius, 24) << "the last iteration is the last diffusion pass";
  EXPECT_EQ(passes[57].kind, ePaintLayerEffectPass::FillTail);
  EXPECT_EQ(passes[57].source, ePaintLayerEffectPassSource::Previous);
  EXPECT_EQ(passes[57].aux, ePaintLayerEffectPassAux::Field);
  EXPECT_EQ(passes[57].reach, 0);
  EXPECT_EQ(passes[57].axis_mask, PAINT_LAYER_EFFECT_AXIS_NONE);
  EXPECT_TRUE(passes[57].alpha_from_kernel) << "the fill layer's coverage is the flooding's";
  EXPECT_NEAR(passes[57].params.x, float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE), 1e-6f)
      << "the mode rides in params";
  EXPECT_EQ(passes[57].radius, 0) << "the blur (none here) rides in the tail's radius";
  EXPECT_NEAR(passes[57].params2.x, 1.0f, 1e-6f) << "the schema's fill colour rides in params2";
  EXPECT_NEAR(passes[57].params2.y, 0.5f, 1e-6f);
  EXPECT_NEAR(passes[57].params2.z, 0.25f, 1e-6f);

  /* The solid mode skips the diffusion: the flood's classification straight into the blur's pair
   * and the tail, the schema's colour written as it is. */
  const std::array<float, 10> fill_solid{float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA),
                                         0.5f,
                                         0.0f,
                                         float(PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE),
                                         float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID),
                                         1.0f,
                                         0.5f,
                                         0.25f,
                                         2.0f,
                                         0.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_solid.data(), 100, 100, passes);
  ASSERT_EQ(passes.size(), 36) << "the seeding, sixteen sweep pairs, the tail and the blur's pair";
  EXPECT_EQ(passes[33].kind, ePaintLayerEffectPass::FillTail)
      << "the tail runs ahead of the blur, the CPU's own order";
  EXPECT_NEAR(passes[33].params.x, float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID), 1e-6f);
  EXPECT_EQ(passes[33].radius, 2) << "the blur the coverage softens by";
  EXPECT_EQ(passes[34].kind, ePaintLayerEffectPass::BlurH);
  EXPECT_EQ(passes[34].radius, 2);
  EXPECT_EQ(passes[35].kind, ePaintLayerEffectPass::BlurV);

  /* The parameterizations the flood cannot express are the CPU's, each an empty plan: the debug's
   * ramp of region numbers, the largest-region classification (the regions' areas are a reduction
   * the flood does not walk) and the source mode with no iteration to reach in with. */
  const std::array<float, 10> fill_debug{float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA),
                                         0.5f,
                                         1.0f,
                                         float(PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE),
                                         float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE),
                                         1.0f,
                                         0.5f,
                                         0.25f,
                                         0.0f,
                                         24.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_debug.data(), 100, 100, passes);
  EXPECT_TRUE(passes.is_empty()) << "the debug's region ramp stays with the CPU";
  const std::array<float, 10> fill_largest{float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA),
                                           0.5f,
                                           0.0f,
                                           float(PAINT_LAYER_FILL_AREA_UV_BORDER_FILL),
                                           float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE),
                                           1.0f,
                                           0.5f,
                                           0.25f,
                                           0.0f,
                                           24.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_largest.data(), 100, 100, passes);
  EXPECT_TRUE(passes.is_empty()) << "the largest-region classification stays with the CPU";
  const std::array<float, 10> fill_no_iter{float(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA),
                                           0.5f,
                                           0.0f,
                                           float(PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE),
                                           float(PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE),
                                           1.0f,
                                           0.5f,
                                           0.25f,
                                           0.0f,
                                           0.0f};
  BKE_paint_layers_effect_pass_plan(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_no_iter.data(), 100, 100, passes);
  EXPECT_TRUE(passes.is_empty()) << "the source mode with no iteration stays with the CPU";

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
       filter <= int(MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK);
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
    /* The schema defaults are the parameters the gather would pack, in the same order: a colour
     * lays its three channels across three slots, the packing the plan reads the Glow tint by. */
    float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
    int pos = 0;
    for (int i = 0; i < kind->params_num; i++) {
      if (kind->params[i].type == PaintLayerKindParamType::Color) {
        for (int c = 0; c < 3; c++) {
          params[pos++] = kind->params[i].default_color[c];
        }
        continue;
      }
      params[pos++] = kind->params[i].default_value;
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
  /* The neighbourhood kinds are all flagged GPU-computed: the walk must have found every one of
   * them, or the flag and the plan builder have drifted apart. The Noise is flagged too, its
   * pointwise kernel being the one pass the plan names, and the glow, the height-to-normal, the
   * pixelate, the slot fetch and the six pass-chain kinds (the Bevel and the Mask Outline's
   * distance transform, the Drop Shadow's silhouette chain, the Blur Slope's walk, the Color
   * Match's reduction and the Fill Area's flood) complete the set. The walk's defaults are the
   * reduce branch and the edge-based flood's, the parameterizations the plans refuse being the
   * plan test's to read. */
  EXPECT_FALSE(gpu_computed_filters.is_empty());
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_BLUR)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_FXAA)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_SHARPEN)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_HIGH_PASS)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_EDGE_DETECT)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_EMBOSS)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_ERODE_DILATE)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_NOISE)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_GLOW)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_PIXELATE)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_BEVEL)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_MASK_OUTLINE)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_DROP_SHADOW)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_BLUR_SLOPE)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_COLOR_MATCH)));
  EXPECT_TRUE(gpu_computed_filters.contains(int(MA_PAINT_LAYER_FILTER_FILL_AREA)));
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
  const auto glow = [](const int mode, const int width, const float spread, const float opacity) {
    return std::array<float, 7>{float(mode), float(width), spread, opacity, 1.0f, 1.0f, 1.0f};
  };
  const auto pixelate = [](const int amount) {
    return std::array<float, 1>{float(amount)};
  };
  const auto height_to_normal = [](const float strength,
                                   const int world_units,
                                   const float surface_size,
                                   const float height_depth,
                                   const int flip_green) {
    return std::array<float, 5>{
        strength, float(world_units), surface_size, height_depth, float(flip_green)};
  };
  const auto channel_split = [](const int channel) {
    return std::array<float, 1>{float(channel)};
  };
  const auto bevel = [](const int source, const int distance, const int corner, const int smooth) {
    return std::array<float, 4>{float(source), float(distance), float(corner), float(smooth)};
  };
  const auto mask_outline = [](const int position,
                               const int start_black,
                               const float threshold,
                               const int width,
                               const int blur,
                               const float shape,
                               const int source) {
    return std::array<float, 10>{float(position),
                                 float(start_black),
                                 threshold,
                                 float(width),
                                 float(blur),
                                 shape,
                                 float(source),
                                 1.0f,
                                 1.0f,
                                 1.0f};
  };
  const auto drop_shadow = [](const int source,
                              const float angle,
                              const int distance,
                              const int size,
                              const int spread,
                              const float opacity) {
    return std::array<float, 9>{
        float(source), angle, float(distance), float(size), float(spread), opacity, 1.0f, 0.0f, 0.0f};
  };
  const auto blur_slope = [](const int seed,
                             const float intensity,
                             const int quality,
                             const int blending,
                             const int source_mode,
                             const int source_blur) {
    return std::array<float, 9>{float(seed),
                                intensity,
                                float(quality),
                                float(blending),
                                float(source_mode),
                                float(source_blur),
                                0.0f,
                                1.0f,
                                1.0f};
  };
  const auto color_match = [](const int picked, const int variation) {
    return std::array<float, 11>{float(picked),
                                 0.5f,
                                 0.5f,
                                 0.5f,
                                 0.25f,
                                 0.5f,
                                 0.75f,
                                 float(variation),
                                 0.1f,
                                 0.2f,
                                 0.3f};
  };
  const auto fill_area = [](const int source,
                            const float threshold,
                            const int debug,
                            const int uv_border,
                            const int color_mode,
                            const int blur,
                            const int iterations) {
    return std::array<float, 10>{float(source),
                                 threshold,
                                 float(debug),
                                 float(uv_border),
                                 float(color_mode),
                                 1.0f,
                                 0.5f,
                                 0.25f,
                                 float(blur),
                                 float(iterations)};
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
  /* The glow in both modes, the 3x3, and the two reach-nothing kinds: the invariants above hold for
   * them the same, the glow's tail reading the input the plan's own halo covers. */
  const std::array<float, 7> glow_soft = glow(PAINT_LAYER_GLOW_SOFT, 8, 0.0f, 1.0f);
  plan_and_check(MA_PAINT_LAYER_FILTER_GLOW, glow_soft.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 7> glow_precise = glow(PAINT_LAYER_GLOW_PRECISE, 5, 0.0f, 1.0f);
  plan_and_check(MA_PAINT_LAYER_FILTER_GLOW, glow_precise.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 5> h2n_params = height_to_normal(2.0f, 1, 100.0f, 5.0f, 1);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL, h2n_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 1> pixelate_params = pixelate(16);
  plan_and_check(MA_PAINT_LAYER_FILTER_PIXELATE, pixelate_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 1> split_params = channel_split(PAINT_LAYER_CHANNEL_SPLIT_ALPHA);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT, split_params.data(), dirty, 100, 100, passes, rects);
  /* The distance transforms flood the whole canvas, so every pass of theirs writes the whole
   * rectangle and the invariants hold on the clamped canvas; the Drop Shadow and the Blur Slope
   * hold them the ordinary way, the tail's offset and the walk's gradient being the reach. */
  const std::array<float, 4> bevel_params = bevel(PAINT_LAYER_BEVEL_SOURCE_ALPHA, 2, 0, 3);
  plan_and_check(MA_PAINT_LAYER_FILTER_BEVEL, bevel_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 10> outline_params = mask_outline(
      PAINT_LAYER_MASK_OUTLINE_OUTSIDE, 0, 0.5f, 4, 2, 0.5f, PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_MASK_OUTLINE, outline_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 9> shadow_params = drop_shadow(
      PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA, 135.0f, 4, 8, 3, 1.0f);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_DROP_SHADOW, shadow_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 9> slope_params = blur_slope(
      5, 15.0f, 8, PAINT_LAYER_BLUR_SLOPE_BLENDING_BLUR, PAINT_LAYER_SLOPE_SOURCE_PREVIOUS_INPUT, 4);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_BLUR_SLOPE, slope_params.data(), dirty, 100, 100, passes, rects);
  /* The reduce chain and the flood are whole-canvas, so every pass of theirs writes the whole
   * rectangle and the invariants hold on the clamped canvas, the diffusion's one-texel reaches and
   * the blur's pair riding between them. */
  const std::array<float, 11> match_params = color_match(0, 0);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_COLOR_MATCH, match_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 10> fill_params = fill_area(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA,
                                                      0.5f,
                                                      0,
                                                      PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                      PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE,
                                                      2,
                                                      4);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_params.data(), dirty, 100, 100, passes, rects);
  const std::array<float, 10> fill_solid_params = fill_area(PAINT_LAYER_FILL_AREA_SOURCE_ALPHA,
                                                            0.5f,
                                                            0,
                                                            PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE,
                                                            PAINT_LAYER_FILL_AREA_COLOR_MODE_SOLID,
                                                            0,
                                                            24);
  plan_and_check(
      MA_PAINT_LAYER_FILTER_FILL_AREA, fill_solid_params.data(), dirty, 100, 100, passes, rects);

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

TEST_F(PaintLayersEffectTest, the_input_scalar_hash_sees_the_slot_image)
{
  /* A Channel Split's slot map is sampled by the kernel itself, outside the input's maps: its
   * identity keys the GPU entry the way a flat colour does, since no map version of the input
   * carries its pixels. */
  PaintMaterialCompositeImageLayer entry;
  Image *first = add_shade_image("SlotFirst", 255, 0);
  Image *second = add_shade_image("SlotSecond", 0, 255);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  entry.slot_image = first;
  const uint64_t keyed = BKE_paint_layers_effect_input_scalar_hash(entry);
  entry.slot_image = second;
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(entry), keyed)
      << "another slot image is another input, not the old one with new data";
  entry.slot_image = first;
  EXPECT_EQ(BKE_paint_layers_effect_input_scalar_hash(entry), keyed);
  entry.slot_image = nullptr;
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(entry), keyed)
      << "no slot is another input, not the live one with new data";
}

TEST_F(PaintLayersEffectTest, the_input_description_names_the_channel_splits_slot)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 255, 0));
  MaterialPaintLayer *split = add_effect(*row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  Image *slot = add_channel_image("Split");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, slot));

  PaintMaterialCompositeImageLayer entry;
  ASSERT_TRUE(BKE_paint_layers_effect_input_describe(*ma, *row, *split, kColor, true, false, entry));
  EXPECT_EQ(entry.slot_image, slot)
      << "the slot is the one map the kernel samples itself, named for the GPU pass to bind";
  /* The slot's identity keys the GPU entry: assigning another image re-keys it, which the
   * description alone decides. */
  const uint64_t keyed = BKE_paint_layers_effect_input_scalar_hash(entry);
  Image *other = add_channel_image("SplitOther");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, other));
  PaintMaterialCompositeImageLayer rekeyed;
  ASSERT_TRUE(
      BKE_paint_layers_effect_input_describe(*ma, *row, *split, kColor, true, false, rekeyed));
  EXPECT_EQ(rekeyed.slot_image, other);
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(rekeyed), keyed);

  /* A blur reads no slot: the description names none, whatever a stale pointer on another row
   * holds. */
  MaterialPaintLayer *blur = add_effect(*row, MA_PAINT_LAYER_FILTER_BLUR);
  PaintMaterialCompositeImageLayer no_slot;
  ASSERT_TRUE(BKE_paint_layers_effect_input_describe(*ma, *row, *blur, kColor, true, false, no_slot));
  EXPECT_EQ(no_slot.slot_image, nullptr);
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

TEST_F(PaintLayersEffectTest, a_mask_drop_shadow_reads_the_luminance_whatever_the_source_says)
{
  /* A mask's grey has no alpha: the alpha source of the schema default would cast nothing from it,
   * so a mask item always reads the luminance and the shadow darkens the ramp. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *shadow = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  ASSERT_EQ(paint_layer_kind_param_int_get(*shadow, PAINT_LAYER_DROP_SHADOW_SOURCE),
            PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA);

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  ASSERT_NE(laid, nullptr);
  std::shared_ptr<ImBuf> input = mask_effect_input_grey(*ma, *row, *shadow);
  ASSERT_NE(input, nullptr);
  EXPECT_TRUE(mask_grey_differs(*laid, *input, 1e-6f));
  for (const int64_t i : IndexRange(int64_t(laid->x) * laid->y)) {
    EXPECT_NEAR(laid->float_data()[i * 4 + 3], 1.0f, 1e-6f)
        << "a mask's alpha stays one, texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, a_mask_outline_lays_a_grey_band_from_the_luminance)
{
  /* A mask's grey has no alpha and no colour: the band is a grey of itself and the item's alpha
   * stays one, whatever the content-only source and colour parameters say. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *outline = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
  ASSERT_EQ(paint_layer_kind_param_int_get(*outline, PAINT_LAYER_MASK_OUTLINE_SOURCE),
            PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA);

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
  ASSERT_NE(laid, nullptr);
  std::shared_ptr<ImBuf> input = mask_effect_input_grey(*ma, *row, *outline);
  ASSERT_NE(input, nullptr);
  EXPECT_TRUE(mask_grey_differs(*laid, *input, 1e-6f));
  const float *px = laid->float_data();
  for (const int64_t i : IndexRange(int64_t(laid->x) * laid->y)) {
    EXPECT_NEAR(px[i * 4 + 0], px[i * 4 + 1], 1e-6f) << "texel " << i;
    EXPECT_NEAR(px[i * 4 + 1], px[i * 4 + 2], 1e-6f) << "texel " << i;
    EXPECT_NEAR(px[i * 4 + 3], 1.0f, 1e-6f) << "a mask's alpha stays one, texel " << i;
  }
  /* The white end of the ramp is the inside, which an outside band leaves empty. */
  EXPECT_NEAR(px[(int64_t(4) * laid->x + laid->x - 1) * 4], 0.0f, 1e-6f);
}

TEST_F(PaintLayersEffectTest, a_mask_channel_split_lays_the_slot_channel_as_the_coverage)
{
  /* The channel the slot's map carries is the coverage the item lays: one grey the whole mask
   * holds, the mask's own alpha one -- the extracted channel is a value already, so no luminance
   * pass is asked of it. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *split = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  Image *orm = add_channel_image("Mask Split");
  ASSERT_TRUE(BKE_paint_layers_effect_image_set(*ma, split, orm));

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_CHANNEL_SPLIT);
  ASSERT_NE(laid, nullptr);
  const float *px = laid->float_data();
  for (const int64_t i : IndexRange(int64_t(laid->x) * laid->y)) {
    EXPECT_NEAR(px[i * 4 + 0], 25.0f / 255.0f, 5e-3f) << "the red of the map, texel " << i;
    EXPECT_NEAR(px[i * 4 + 1], px[i * 4 + 0], 1e-6f) << "texel " << i;
    EXPECT_NEAR(px[i * 4 + 3], 1.0f, 1e-6f) << "a mask's alpha stays one, texel " << i;
  }
}

TEST_F(PaintLayersEffectTest, a_mask_bevel_reads_the_luminance_whatever_the_source_says)
{
  /* A mask's grey has no alpha (one everywhere): the alpha source of the schema default would find
   * no border and lay a solid one, so a mask item always bevels by the luminance. */
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *bevel = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_BEVEL);
  ASSERT_EQ(paint_layer_kind_param_int_get(*bevel, PAINT_LAYER_BEVEL_SOURCE),
            PAINT_LAYER_BEVEL_SOURCE_ALPHA);

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_BEVEL);
  ASSERT_NE(laid, nullptr);
  /* The dark end of the ramp is outside the shape, which is no distance from a border. */
  EXPECT_LT(laid->float_data()[(int64_t(4) * laid->x) * 4], 0.5f);
}

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
  paint_layer_effect_kernel_apply(*expected, *noise, false, PAINT_TILE_NUMBER_BASE);

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
  paint_layer_effect_kernel_apply(*expected, *noise, false, PAINT_TILE_NUMBER_BASE);
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
                                               MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL,
                                               MA_PAINT_LAYER_FILTER_SHARPEN,
                                               MA_PAINT_LAYER_FILTER_HIGH_PASS,
                                               MA_PAINT_LAYER_FILTER_EDGE_DETECT,
                                               MA_PAINT_LAYER_FILTER_EMBOSS,
                                               MA_PAINT_LAYER_FILTER_ERODE_DILATE,
                                               MA_PAINT_LAYER_FILTER_GLOW,
                                               MA_PAINT_LAYER_FILTER_BEVEL,
                                               MA_PAINT_LAYER_FILTER_COLOR_MATCH,
                                               MA_PAINT_LAYER_FILTER_DROP_SHADOW,
                                               MA_PAINT_LAYER_FILTER_MASK_OUTLINE,
                                               MA_PAINT_LAYER_FILTER_FILL_AREA,
                                               MA_PAINT_LAYER_FILTER_PIXELATE,
                                               MA_PAINT_LAYER_FILTER_FXAA,
                                               MA_PAINT_LAYER_FILTER_BLUR_SLOPE,
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

TEST_F(PaintLayersEffectTest, a_mask_mirror_folds_the_mask_built_up_to_it)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *mirror = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_MIRROR);

  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_MIRROR);
  ASSERT_NE(laid, nullptr);
  std::shared_ptr<ImBuf> input = mask_effect_input_grey(*ma, *row, *mirror);
  ASSERT_NE(input, nullptr);

  /* A fold about the middle of the canvas shows both sides alike, and the ramp is not symmetric. */
  const float *laid_px = laid->float_data();
  for (const int y : IndexRange(8)) {
    for (const int x : IndexRange(8)) {
      EXPECT_NEAR(laid_px[(y * 8 + x) * 4], laid_px[(y * 8 + (7 - x)) * 4], 1e-5f)
          << "texel " << x << "," << y;
      EXPECT_NEAR(laid_px[(y * 8 + x) * 4 + 3], 1.0f, 1e-6f) << "an opaque grey";
    }
  }
  EXPECT_TRUE(mask_grey_differs(*laid, *input, 1e-4f)) << "a fold of a ramp is not the ramp";
}

TEST_F(PaintLayersEffectTest, a_mask_transform_moves_the_mask_and_acts_in_uv_only)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add_image(*ma, *row, *add_ramp_image("Mask")), nullptr);
  MaterialPaintLayer *transform = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_TRANSFORM);
  EXPECT_EQ(mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_TRANSFORM), nullptr)
      << "a transform at the identity moves nothing and lays nothing";

  /* Two texels of the 8-texel canvas, so the bilinear read lands on texel centers exactly. */
  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, transform, offset));
  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_TRANSFORM);
  ASSERT_NE(laid, nullptr);
  std::shared_ptr<ImBuf> input = mask_effect_input_grey(*ma, *row, *transform);
  ASSERT_NE(input, nullptr);

  /* The sign of the move is the mapping's own vocabulary; either way the mask shifted by two
   * texels, wrapping at the edge as the default Repeat does. */
  const auto shifted_by = [&](const int shift) {
    for (const int y : IndexRange(8)) {
      for (const int x : IndexRange(8)) {
        const int source_x = ((x + shift) % 8 + 8) % 8;
        if (std::abs(laid->float_data()[(y * 8 + x) * 4] -
                     input->float_data()[(y * 8 + source_x) * 4]) > 1e-4f)
        {
          return false;
        }
      }
    }
    return true;
  };
  EXPECT_TRUE(shifted_by(2) || shifted_by(-2)) << "the mask moved by the offset";

  /* The Decal frame walks the painted object's surface, which a mask's canvas grey has no access
   * to: the mask form is UV only, the content form keeps both. */
  EXPECT_TRUE(BKE_paint_layers_effect_space_supported(*transform, MA_PAINT_LAYER_MAPPING_SPACE_UV));
  EXPECT_FALSE(
      BKE_paint_layers_effect_space_supported(*transform, MA_PAINT_LAYER_MAPPING_SPACE_DECAL));
  MaterialPaintLayer *content = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_TRANSFORM, nullptr);
  ASSERT_NE(content, nullptr);
  EXPECT_TRUE(
      BKE_paint_layers_effect_space_supported(*content, MA_PAINT_LAYER_MAPPING_SPACE_DECAL));
}

TEST_F(PaintLayersEffectTest, a_shifted_constant_mask_does_not_vanish)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_shade_image("Row", 40, 200));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *transform = add_mask_effect(*ma, *row, MA_PAINT_LAYER_FILTER_TRANSFORM);

  const float offset[2] = {0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, transform, offset));
  /* Repeat: a constant shifted around the canvas is the same constant everywhere. */
  std::shared_ptr<ImBuf> laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_TRANSFORM);
  ASSERT_NE(laid, nullptr);
  for (const int64_t i : IndexRange(int64_t(laid->x) * laid->y)) {
    EXPECT_NEAR(laid->float_data()[i * 4], 1.0f, 1e-5f) << "texel " << i;
  }

  /* Clip: only the strip the shift uncovers is lost, not the whole mask. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_wrap(*ma, transform, MA_PAINT_LAYER_MAPPING_WRAP_CLIP));
  laid = mask_effect_grey(*ma, MA_PAINT_LAYER_FILTER_TRANSFORM);
  ASSERT_NE(laid, nullptr);
  int lit = 0;
  int dark = 0;
  for (const int64_t i : IndexRange(int64_t(laid->x) * laid->y)) {
    (laid->float_data()[i * 4] > 0.5f ? lit : dark)++;
  }
  EXPECT_GT(lit, 0) << "the mask survives a clipped shift";
  EXPECT_GT(dark, 0) << "the uncovered strip lays no coverage";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mirror fold parity
 * \{ */

/**
 * The CPU fold and side are the single source of the formula the generated graph and the shader
 * copy. A coordinate on either side of the center, exactly on it, and the negative direction cover
 * the branches a divergence would show in.
 */
struct MirrorFoldCase {
  float coordinate;
  float center;
  bool negative;
  /* Written out by hand, so a change of the formula cannot change the expectation with it. */
  float expected_fold;
  float expected_side;
};

class PaintLayersMirrorFoldTest : public testing::TestWithParam<MirrorFoldCase> {};

TEST_P(PaintLayersMirrorFoldTest, matches_the_reference_fold_and_side)
{
  const MirrorFoldCase &test = GetParam();
  SCOPED_TRACE(::testing::Message() << "coordinate=" << test.coordinate
                                    << " center=" << test.center
                                    << " negative=" << test.negative);
  EXPECT_NEAR(BKE_paint_layers_mapping_mirror_fold(
                  test.coordinate, test.center, test.negative),
              test.expected_fold,
              1e-6f);

  EXPECT_NEAR(BKE_paint_layers_mapping_mirror_side(
                  test.coordinate, test.center, test.negative),
              test.expected_side,
              1e-6f);
}

INSTANTIATE_TEST_SUITE_P(
    PaintLayersEffectTest,
    PaintLayersMirrorFoldTest,
    testing::Values(MirrorFoldCase{0.1f, 0.5f, false, 0.9f, -1.0f},
                    MirrorFoldCase{0.9f, 0.5f, false, 0.9f, 1.0f},
                    MirrorFoldCase{0.5f, 0.5f, false, 0.5f, 1.0f},
                    MirrorFoldCase{0.1f, 0.5f, true, 0.1f, 1.0f},
                    MirrorFoldCase{0.9f, 0.5f, true, 0.1f, -1.0f},
                    MirrorFoldCase{0.5f, 0.5f, true, 0.5f, -1.0f},
                    MirrorFoldCase{0.35f, 0.25f, false, 0.35f, 1.0f},
                    MirrorFoldCase{0.35f, 0.25f, true, 0.15f, -1.0f}));

/** \} */

}  // namespace blender::bke::tests
