/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <string>

#include "BKE_brush.hh"
#include "BKE_brush_mask_correction.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_texture.h"

#include "BLI_math_vector.hh"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_enums.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "IMB_imbuf_types.hh"

#include "RE_texture.h"

#include "paint_curve_patch_live.hh"
#include "paint_curve_patch_sampler.hh"

#include "mesh/paint_material_source.hh"
#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint::tests {

class CurvePatchMaskTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  ImagePool *pool = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    pool = BKE_image_pool_new();
  }

  void TearDown() override
  {
    BKE_image_pool_free(pool);
    BKE_main_free(bmain);
  }

  Brush *make_brush(const char *name)
  {
    return BKE_brush_add(bmain, name, OB_MODE_SCULPT);
  }

  Tex *assign_mask(Brush &brush, const float color[4])
  {
    return assign_mask_sized(brush, 8, 8, "CPMaskImage", "CPMaskTex", color);
  }

  Tex *assign_mask_sized(Brush &brush,
                         const int width,
                         const int height,
                         const char *image_name,
                         const char *tex_name,
                         const float color[4])
  {
    Image *image = BKE_image_add_generated(bmain,
                                           width,
                                           height,
                                           image_name,
                                           /*depth=*/32,
                                           /*floatbuf=*/true,
                                           IMA_GENTYPE_BLANK,
                                           color,
                                           /*stereo3d=*/false,
                                           /*is_data=*/true,
                                           /*tiled=*/false);
    Tex *tex = BKE_texture_add(bmain, tex_name);
    tex->type = TEX_IMAGE;
    tex->ima = image;
    id_us_plus(&image->id);
    tex->extend = TEX_EXTEND;
    tex->imaflag |= TEX_INTERPOL;

    brush.mask_mtex.tex = tex;
    id_us_plus(&tex->id);
    return tex;
  }

  /** 4x4 blocks of 4x4 texels, each block a distinct red value: the placement of the sampled
   * texel is visible, while a blur radius of one texel is an exact no-op at block centers. */
  void fill_block_image(Image &image)
  {
    ImageUser iuser = {};
    ImBuf *ibuf = BKE_image_pool_acquire_ibuf(&image, &iuser, pool);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    constexpr int block = 4;
    for (int y = 0; y < ibuf->y; y++) {
      for (int x = 0; x < ibuf->x; x++) {
        const int index = (y / block) + (x / block) * block;
        const float value = float(index) / 15.0f;
        float *pixel = ibuf->float_data_for_write() + (int64_t(y) * ibuf->x + x) * 4;
        pixel[0] = value;
        pixel[1] = value;
        pixel[2] = value;
        pixel[3] = 1.0f;
      }
    }
    BKE_image_pool_release_ibuf(&image, ibuf, pool);
    ASSERT_NE(image.runtime, nullptr);
    image.runtime->pixel_version.fetch_add(1, std::memory_order_relaxed);
  }

  /** The cached path's answer for \a x / \a y, where both paths receive the same #MTex. */
  std::optional<float> cached_sample(const Brush &brush, const float x, const float y)
  {
    return BKE_brush_mask_correction_sample_image(brush, brush.mask_mtex, x, y, pool);
  }

  /** The texture engine's red channel at the same coordinates. */
  float engine_sample(const MTex &mtex, const float x, const float y)
  {
    float intensity = 0.0f;
    float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float co[3] = {x, y, 0.0f};
    RE_texture_evaluate(&mtex, co, 0, pool, false, false, &intensity, rgba);
    return rgba[0];
  }

  /** The texture engine's alpha at the same coordinates, after `TEX_NEGALPHA`. */
  float engine_sample_alpha(const MTex &mtex, const float x, const float y)
  {
    float intensity = 0.0f;
    float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float co[3] = {x, y, 0.0f};
    RE_texture_evaluate(&mtex, co, 0, pool, false, false, &intensity, rgba);
    return rgba[3];
  }

  /** A byte image Mask, so the pre-multiply rules of `ibuf_get_color()` are exercised. */
  Tex *assign_mask_byte(Brush &brush,
                        const int width,
                        const int height,
                        const char *image_name,
                        const char *tex_name,
                        const float color[4])
  {
    Image *image = BKE_image_add_generated(bmain,
                                           width,
                                           height,
                                           image_name,
                                           /*depth=*/32,
                                           /*floatbuf=*/false,
                                           IMA_GENTYPE_BLANK,
                                           color,
                                           /*stereo3d=*/false,
                                           /*is_data=*/true,
                                           /*tiled=*/false);
    Tex *tex = BKE_texture_add(bmain, tex_name);
    tex->type = TEX_IMAGE;
    tex->ima = image;
    id_us_plus(&image->id);
    tex->extend = TEX_EXTEND;
    tex->imaflag |= TEX_INTERPOL;

    brush.mask_mtex.tex = tex;
    id_us_plus(&tex->id);
    return tex;
  }

  /** Same block layout as #fill_block_image, written to the byte buffer with a constant alpha. */
  void fill_block_image_byte(Image &image, const unsigned char alpha)
  {
    ImageUser iuser = {};
    ImBuf *ibuf = BKE_image_pool_acquire_ibuf(&image, &iuser, pool);
    ASSERT_NE(ibuf, nullptr);
    unsigned char *rect = ibuf->byte_data_for_write();
    ASSERT_NE(rect, nullptr);
    constexpr int block = 4;
    for (int y = 0; y < ibuf->y; y++) {
      for (int x = 0; x < ibuf->x; x++) {
        const int index = (y / block) + (x / block) * block;
        const unsigned char value = (unsigned char)(255.0f * float(index) / 15.0f);
        unsigned char *pixel = rect + (int64_t(y) * ibuf->x + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = value;
        pixel[3] = alpha;
      }
    }
    BKE_image_pool_release_ibuf(&image, ibuf, pool);
    ASSERT_NE(image.runtime, nullptr);
    image.runtime->pixel_version.fetch_add(1, std::memory_order_relaxed);
  }

  /** Replace the alpha of a float Mask with a constant, keeping the RGB blocks. */
  void set_alpha_constant(Image &image, const float alpha)
  {
    ImageUser iuser = {};
    ImBuf *ibuf = BKE_image_pool_acquire_ibuf(&image, &iuser, pool);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    for (int y = 0; y < ibuf->y; y++) {
      for (int x = 0; x < ibuf->x; x++) {
        float *pixel = ibuf->float_data_for_write() + (int64_t(y) * ibuf->x + x) * 4;
        pixel[3] = alpha;
      }
    }
    BKE_image_pool_release_ibuf(&image, ibuf, pool);
    ASSERT_NE(image.runtime, nullptr);
    image.runtime->pixel_version.fetch_add(1, std::memory_order_relaxed);
  }
};

TEST_F(CurvePatchMaskTest, no_mask_texture_is_full_coverage)
{
  Brush *brush = make_brush("CPMaskNone");
  /* The early return keeps this independent of the texture engine: an unassigned Mask slot can
   * never clip a patch. */
  EXPECT_FLOAT_EQ(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool), 1.0f);
}

TEST_F(CurvePatchMaskTest, black_mask_texture_removes_coverage)
{
  Brush *brush = make_brush("CPMaskBlack");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  assign_mask(*brush, black);

  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              0.0f,
              1e-5f);
}

TEST_F(CurvePatchMaskTest, white_mask_texture_is_full_coverage)
{
  Brush *brush = make_brush("CPMaskWhite");
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  assign_mask(*brush, white);

  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              1.0f,
              1e-5f);
}

TEST_F(CurvePatchMaskTest, grey_mask_texture_scales_coverage)
{
  /* The ribbon and stamp paths multiply the height and claim weight by this coverage, so a half
   * grey Mask must come back as half coverage, not as a hard 0/1 clip. */
  Brush *brush = make_brush("CPMaskGrey");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  assign_mask(*brush, grey);

  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              0.5f,
              1e-5f);
}

TEST_F(CurvePatchMaskTest, stamp_mask_weight_factor_scales_height_and_weight)
{
  /* Stamps fold the Mask into each stamp's alpha, so the composited height already carries it.
   * The claim weight must carry the mask too; using the mask-only ratio keeps the texture alpha
   * and falloff out of it. A half-grey Mask must halve both. */
  Brush *brush = make_brush("CPMaskStampWeight");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  assign_mask(*brush, grey);
  const float coverage = curve_patch_mask_coverage(
      *brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool);
  EXPECT_NEAR(coverage, 0.5f, 1e-5f);

  const float alpha_unmasked = 0.8f;
  const float alpha = alpha_unmasked * coverage;
  const float factor = curve_patch_stamp_mask_weight_factor(alpha, alpha_unmasked);
  EXPECT_NEAR(factor, 0.5f, 1e-5f);

  /* Both the reported relief and the cross-pass claim weight take the same factor. */
  const float height = alpha_unmasked;
  const float weight = 0.7f;
  EXPECT_NEAR(height * factor, 0.4f, 1e-5f);
  EXPECT_NEAR(weight * factor, 0.35f, 1e-5f);

  /* An open Mask is exactly identity, so unmasked strokes keep their old weight. */
  EXPECT_FLOAT_EQ(curve_patch_stamp_mask_weight_factor(alpha_unmasked, alpha_unmasked), 1.0f);
  /* And a degenerate caller cannot produce a NaN. */
  EXPECT_FLOAT_EQ(curve_patch_stamp_mask_weight_factor(0.0f, 0.0f), 1.0f);
}

TEST_F(CurvePatchMaskTest, mask_bias_subtracts_after_the_selected_channel)
{
  Brush *brush = make_brush("CPMaskBias");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  assign_mask(*brush, grey);

  brush->texture_sample_bias = 0.25f;
  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              0.25f,
              1e-5f);

  /* A negative bias raises the coverage, clamped to the unit range. */
  brush->texture_sample_bias = -0.25f;
  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              0.75f,
              1e-5f);

  brush->texture_sample_bias = -2.0f;
  EXPECT_FLOAT_EQ(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
                  1.0f);
}

TEST_F(CurvePatchMaskTest, image_paint_ignores_a_texture_in_the_sculpt_slot)
{
  /* Image Paint reads #Brush.mask_mtex; a texture sitting in #Brush.mtex is the color slot there
   * and must not clip the Mask. */
  Brush *brush = make_brush("CPMaskImagePaintSlot");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(bmain,
                                         /*width=*/8,
                                         /*height=*/8,
                                         "CPMaskMtexColour",
                                         /*depth=*/32,
                                         /*floatbuf=*/true,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         /*stereo3d=*/false,
                                         /*is_data=*/true,
                                         /*tiled=*/false);
  Tex *tex = BKE_texture_add(bmain, "CPMaskMtexColourTex");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  tex->extend = TEX_EXTEND;
  tex->imaflag |= TEX_INTERPOL;
  brush->mtex.tex = tex;
  id_us_plus(&tex->id);

  EXPECT_FLOAT_EQ(
      curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
      1.0f);
}


TEST_F(CurvePatchMaskTest, mask_invert_mirrors_the_coverage)
{
  Brush *brush = make_brush("CPMaskInvert");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  assign_mask(*brush, black);
  brush->flag2 |= BRUSH_MASK_TEXTURE_INVERT;

  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              1.0f,
              1e-5f);
}

TEST_F(CurvePatchMaskTest, mask_correction_channel_reaches_the_patch)
{
  Brush *brush = make_brush("CPMaskCorrection");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  assign_mask(*brush, black);
  /* A black RGB image still has alpha 1: reading the Alpha channel through the correction must
   * reach the patch as full coverage, proving the extended correction is applied there at all. */
  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;

  EXPECT_NEAR(curve_patch_mask_coverage(*brush, OB_MODE_TEXTURE_PAINT, float2(0.0f), float2(1.0f), 0, *pool),
              1.0f,
              1e-5f);
}

TEST_F(CurvePatchMaskTest, live_inputs_watch_the_mask_slot)
{
  Brush *brush = make_brush("CPMaskLive");
  Paint paint{};
  /* The live-input capture reads the mode through #Paint::runtime; a zeroed Paint would leave the
   * pointer null and the unified-settings queries dereference it. */
  bke::PaintRuntime paint_runtime;
  paint_runtime.ob_mode = OB_MODE_TEXTURE_PAINT;
  paint.runtime = &paint_runtime;

  const CurvePatchLiveInputs before = curve_patch_live_inputs_capture(paint, *brush, OB_MODE_TEXTURE_PAINT);

  brush->mask_mtex.ofs[0] = 0.25f;
  const CurvePatchLiveInputs moved = curve_patch_live_inputs_capture(paint, *brush, OB_MODE_TEXTURE_PAINT);
  EXPECT_FALSE(before == moved);
  /* Moving the Mask must re-stamp, but it does not free and reallocate the texture pool. */
  EXPECT_FALSE(moved.needs_texture_pool_rebuild(before));

  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  assign_mask(*brush, white);
  const CurvePatchLiveInputs assigned = curve_patch_live_inputs_capture(paint, *brush, OB_MODE_TEXTURE_PAINT);
  EXPECT_FALSE(moved == assigned);
  EXPECT_TRUE(assigned.needs_texture_pool_rebuild(moved));

  brush->flag2 |= BRUSH_MASK_TEXTURE_INVERT;
  const CurvePatchLiveInputs inverted = curve_patch_live_inputs_capture(paint, *brush, OB_MODE_TEXTURE_PAINT);
  EXPECT_FALSE(assigned == inverted);

  /* The extended correction's identity is part of the live snapshot: editing it re-stamps. */
  BKE_brush_mask_correction_ensure(*brush)->flag |= BRUSH_MASK_CORRECTION_ENABLED;
  const CurvePatchLiveInputs corrected = curve_patch_live_inputs_capture(paint, *brush, OB_MODE_TEXTURE_PAINT);
  EXPECT_FALSE(inverted == corrected);
  EXPECT_NE(corrected.mask_correction_hash, 0ull);
  EXPECT_FALSE(corrected.needs_texture_pool_rebuild(inverted));
}

TEST_F(CurvePatchMaskTest, sculpt_uses_mtex_as_the_mask_slot)
{
  Brush *brush = make_brush("CPMaskSculptSlot");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  /* A color texture in `mask_mtex` must NOT clip a Sculpt patch: the Sculpt Mask slot is `mtex`. */
  assign_mask(*brush, black);
  EXPECT_FLOAT_EQ(
      curve_patch_mask_coverage(*brush, OB_MODE_SCULPT, float2(0.0f), float2(1.0f), 0, *pool),
      1.0f);

  /* Assigning `mtex` as a black image does clip it. */
  Image *image = BKE_image_add_generated(bmain,
                                         /*width=*/8,
                                         /*height=*/8,
                                         "CPMaskSculptImage",
                                         /*depth=*/32,
                                         /*floatbuf=*/true,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         /*stereo3d=*/false,
                                         /*is_data=*/true,
                                         /*tiled=*/false);
  Tex *tex = BKE_texture_add(bmain, "CPMaskSculptTex");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  tex->extend = TEX_EXTEND;
  tex->imaflag |= TEX_INTERPOL;
  brush->mtex.tex = tex;
  id_us_plus(&tex->id);

  EXPECT_NEAR(
      curve_patch_mask_coverage(*brush, OB_MODE_SCULPT, float2(0.0f), float2(1.0f), 0, *pool),
      0.0f,
      1e-5f);

  /* In Sculpt the mirror is the brush texture's invert, not the Mask slot's. */
  brush->flag2 |= BRUSH_TEXTURE_INVERT;
  EXPECT_NEAR(
      curve_patch_mask_coverage(*brush, OB_MODE_SCULPT, float2(0.0f), float2(1.0f), 0, *pool),
      1.0f,
      1e-5f);
}

TEST_F(CurvePatchMaskTest, sculpt_mask_stroke_applies_the_correction)
{
  /* Ordinary Sculpt strokes sample the Mask through `sculpt_apply_texture()` and the legacy
   * `Brush.mtex` slot. The correction used to be shown in the UI but never reach this path; a
   * channel switch must now change the scalar the stroke multiplies its falloff by. */
  Brush *brush = make_brush("CPSculptStrokeCorrection");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(bmain,
                                         /*width=*/8,
                                         /*height=*/8,
                                         "CPSculptStrokeImage",
                                         /*depth=*/32,
                                         /*floatbuf=*/true,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         /*stereo3d=*/false,
                                         /*is_data=*/true,
                                         /*tiled=*/false);
  Tex *tex = BKE_texture_add(bmain, "CPSculptStrokeTex");
  tex->type = TEX_IMAGE;
  tex->ima = image;
  id_us_plus(&image->id);
  tex->extend = TEX_EXTEND;
  brush->mtex.tex = tex;
  id_us_plus(&tex->id);
  brush->mtex.brush_map_mode = MTEX_MAP_MODE_3D;

  Paint paint{};
  bke::PaintRuntime paint_runtime;
  paint.runtime = &paint_runtime;

  SculptSession ss;
  StrokeCache cache{};
  ss.cache = &cache;
  cache.paint = &paint;
  cache.brush = brush;

  const float point[3] = {0.0f, 0.0f, 0.0f};
  float value = 0.0f;
  float4 rgba(0.0f);

  sculpt_apply_texture(ss, *brush, point, 0, &value, rgba);
  /* The image is black in RGB, so the intensity path is 0. */
  EXPECT_NEAR(value, 0.0f, 1e-5f);

  /* Reading the Alpha channel bypasses the black RGB and must reach the stroke. */
  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  cache.mask_correction_sampler = BrushMaskCorrectionSampler::resolve(
      *brush, brush->mtex, &ss.tex_pool_ensure());

  sculpt_apply_texture(ss, *brush, point, 0, &value, rgba);
  EXPECT_NEAR(value, 1.0f, 1e-5f);
  /* Only the scalar coverage is corrected; the texture color is left for its own consumers. */
  EXPECT_NEAR(rgba.x, 0.0f, 1e-5f);
  EXPECT_NEAR(rgba.y, 0.0f, 1e-5f);
  EXPECT_NEAR(rgba.z, 0.0f, 1e-5f);
}

TEST_F(CurvePatchMaskTest, mask_bias_is_identical_across_3d_area_and_region)
{
  /* The three Mask sampling paths must agree on the bias's sign and on running it after the
   * channel. A half-grey Mask biased by 0.25 reads 0.25 everywhere; a negative bias raises it. */
  Brush *brush = make_brush("CPMaskBiasParity");
  const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  assign_mask(*brush, grey);
  brush->mask_mtex.brush_map_mode = MTEX_MAP_MODE_3D;

  Paint paint{};
  bke::PaintRuntime paint_runtime;
  paint.runtime = &paint_runtime;
  paint_runtime.pixel_radius = 1.0f;
  paint_runtime.start_pixel_radius = 1.0f;

  const float3 point(0.0f);
  const auto sample_3d = [&]() {
    return material::paint_mask_texture_coverage(*brush,
                                                 &paint,
                                                 point,
                                                 point,
                                                 float2(-1.0f),
                                                 0,
                                                 pool,
                                                 nullptr,
                                                 nullptr);
  };
  const auto sample_area = [&]() {
    return BKE_brush_mask_tex_area_coverage_slot(*brush,
                                                 brush->mask_mtex,
                                                 eBrushTextureSlot::Mask,
                                                 false,
                                                 &paint,
                                                 float2(0.0f),
                                                 0.0f,
                                                 float2(1.0f),
                                                 0,
                                                 pool);
  };
  const auto sample_region = [&]() {
    brush->mask_mtex.brush_map_mode = MTEX_MAP_MODE_VIEW;
    const float value = BKE_brush_sample_masktex(&paint, brush, float2(0.0f), 0, pool);
    brush->mask_mtex.brush_map_mode = MTEX_MAP_MODE_3D;
    return value;
  };

  brush->texture_sample_bias = 0.25f;
  EXPECT_NEAR(sample_3d(), 0.25f, 1e-5f);
  EXPECT_NEAR(sample_area(), 0.25f, 1e-5f);
  EXPECT_NEAR(sample_region(), 0.25f, 1e-5f);

  brush->texture_sample_bias = -0.25f;
  EXPECT_NEAR(sample_3d(), 0.75f, 1e-5f);
  EXPECT_NEAR(sample_area(), 0.75f, 1e-5f);
  EXPECT_NEAR(sample_region(), 0.75f, 1e-5f);
}

TEST_F(CurvePatchMaskTest, spatial_mapping_parity_with_engine_placement)
{
  Brush *brush = make_brush("CPMaskParity");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Tex *tex = assign_mask_sized(
      *brush, 16, 16, "CPMaskParityImage", "CPMaskParityTex", black);
  fill_block_image(*tex->ima);

  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  correction->blur_radius = 1.0f;
  /* A box blur over a uniform 4x4 block is an exact identity, so the only difference a
   * mismatch can produce is the sampled texel itself. */
  correction->blur_shape = BRUSH_MASK_CORRECTION_BLUR_BOX;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_RED;

  const auto texel_x = [](const int x) {
    return 2.0f * ((float(x) + 0.5f) / 16.0f) - 1.0f;
  };
  const auto texel_y = [](const int y) {
    return 2.0f * ((float(y) + 0.5f) / 16.0f) - 1.0f;
  };
  const auto compare = [&](const float x, const float y) {
    SCOPED_TRACE("x=" + std::to_string(x) + " y=" + std::to_string(y));
    const std::optional<float> cached = cached_sample(*brush, x, y);
    ASSERT_TRUE(cached.has_value());
    EXPECT_NEAR(*cached, engine_sample(brush->mask_mtex, x, y), 1e-5f);
  };

  /* Plain placement. */
  compare(texel_x(5), texel_y(6));

  /* Tiling folds the coordinate back into the image. */
  tex->extend = TEX_REPEAT;
  compare(texel_x(5 + 16), texel_y(6 + 16));
  tex->extend = TEX_EXTEND;

  /* 90-degree image rotation swaps the axes, so a block on one side must land on its transpose. */
  tex->imaflag |= TEX_IMAROT;
  compare(texel_x(5), texel_y(2));
  compare(texel_x(13), texel_y(6));
  tex->imaflag &= ~TEX_IMAROT;

  /* Crop is part of the texture mapping, not of the stamp. */
  tex->cropxmin = 0.25f;
  tex->cropxmax = 0.75f;
  {
    const float u = ((float(5) + 0.5f) / 16.0f - tex->cropxmin) /
                    (tex->cropxmax - tex->cropxmin);
    compare(2.0f * u - 1.0f, texel_y(6));
  }
  tex->cropxmin = 0.0f;
  tex->cropxmax = 1.0f;
}

TEST_F(CurvePatchMaskTest, spatial_mapping_parity_with_engine_clip)
{
  Brush *brush = make_brush("CPMaskParityClip");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Tex *tex = assign_mask_sized(
      *brush, 16, 16, "CPMaskParityClipImage", "CPMaskParityClipTex", black);
  fill_block_image(*tex->ima);

  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  correction->blur_radius = 1.0f;
  correction->blur_shape = BRUSH_MASK_CORRECTION_BLUR_BOX;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_RED;

  /* A clipped box that straddles the border is scaled after filtering, so the interpolating
   * cache declines instead of approximating it. */
  tex->extend = TEX_CLIP;
  EXPECT_FALSE(cached_sample(*brush, 1.5f, 0.0f).has_value());

  /* Nearest sampling clips on the integer texel, which both paths reproduce exactly. */
  tex->imaflag &= ~TEX_INTERPOL;
  const std::optional<float> cached = cached_sample(*brush, 1.5f, 0.0f);
  ASSERT_TRUE(cached.has_value());
  EXPECT_FLOAT_EQ(*cached, 0.0f);
  EXPECT_FLOAT_EQ(engine_sample(brush->mask_mtex, 1.5f, 0.0f), 0.0f);
}

TEST_F(CurvePatchMaskTest, spatial_byte_buffer_matches_engine_premultiply)
{
  Brush *brush = make_brush("CPMaskParityByte");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Tex *tex = assign_mask_byte(
      *brush, 16, 16, "CPMaskParityByteImage", "CPMaskParityByteTex", black);
  /* A half-transparent byte mask: the engine pre-multiplies and de-premultiplies, and the cached
   * fill has to reproduce both. The uniform 4x4 blocks make the one-texel box blur an identity. */
  fill_block_image_byte(*tex->ima, 128);

  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  correction->blur_radius = 1.0f;
  correction->blur_shape = BRUSH_MASK_CORRECTION_BLUR_BOX;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_RED;

  /* Nearest sampling keeps the byte path to one texel, so only the alpha conversion is compared. */
  tex->imaflag &= ~TEX_INTERPOL;
  const auto texel_x = [](const int x) {
    return 2.0f * ((float(x) + 0.5f) / 16.0f) - 1.0f;
  };
  const auto texel_y = [](const int y) {
    return 2.0f * ((float(y) + 0.5f) / 16.0f) - 1.0f;
  };
  const std::optional<float> cached = cached_sample(*brush, texel_x(5), texel_y(6));
  ASSERT_TRUE(cached.has_value());
  EXPECT_NEAR(*cached, engine_sample(brush->mask_mtex, texel_x(5), texel_y(6)), 1e-5f);
}

TEST_F(CurvePatchMaskTest, spatial_negative_alpha_matches_engine)
{
  Brush *brush = make_brush("CPMaskParityNegAlpha");
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Tex *tex = assign_mask_sized(
      *brush, 16, 16, "CPMaskParityNegAlphaImage", "CPMaskParityNegAlphaTex", black);
  fill_block_image(*tex->ima);
  set_alpha_constant(*tex->ima, 0.25f);

  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  correction->blur_radius = 1.0f;
  correction->blur_shape = BRUSH_MASK_CORRECTION_BLUR_BOX;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;

  tex->flag |= TEX_NEGALPHA;
  tex->imaflag &= ~TEX_INTERPOL;
  const std::optional<float> cached = cached_sample(*brush, 0.0f, 0.0f);
  ASSERT_TRUE(cached.has_value());
  /* 1 - 0.25, reproduced by both the fallback fill and the engine. */
  EXPECT_NEAR(*cached, 0.75f, 1e-5f);
  EXPECT_NEAR(engine_sample_alpha(brush->mask_mtex, 0.0f, 0.0f), 0.75f, 1e-5f);
  EXPECT_NEAR(*cached, engine_sample_alpha(brush->mask_mtex, 0.0f, 0.0f), 1e-5f);
}

}  // namespace blender::ed::sculpt_paint::tests
