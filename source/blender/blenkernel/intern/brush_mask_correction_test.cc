/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "BKE_brush.hh"
#include "BKE_brush_mask_correction.hh"
#include "BKE_colortools.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_texture.h"
#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_global.hh"

#include "BLI_array.hh"
#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_base.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "IMB_imbuf_types.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_enums.h"
#include "DNA_texture_types.h"

namespace blender::bke::tests {

namespace {

/** One enabled correction with every stage switch cleared, so a test only turns on what it checks. */
void make_correction(BrushMaskCorrection &correction)
{
  correction.flag = BRUSH_MASK_CORRECTION_ENABLED;
}

float apply(const BrushMaskCorrection &correction, const float value)
{
  return BKE_brush_mask_correction_apply_value(correction, value);
}

}  // namespace

TEST(MaskCorrectionPointwise, levels_remap_the_input_range)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_LEVELS;
  correction.levels_in_black = 0.2f;
  correction.levels_in_white = 0.8f;
  correction.levels_gamma = 1.0f;

  EXPECT_NEAR(apply(correction, 0.2f), 0.0f, 1e-5f);
  EXPECT_NEAR(apply(correction, 0.5f), 0.5f, 1e-5f);
  EXPECT_NEAR(apply(correction, 0.8f), 1.0f, 1e-5f);
  /* Clamped outside the input range, like the stack's own Levels filter. */
  EXPECT_NEAR(apply(correction, 0.0f), 0.0f, 1e-5f);
  EXPECT_NEAR(apply(correction, 1.0f), 1.0f, 1e-5f);
}

TEST(MaskCorrectionPointwise, smoothstep_softens_the_ends)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_SMOOTHSTEP;
  correction.smoothstep_min = 0.0f;
  correction.smoothstep_max = 1.0f;
  correction.smoothstep_softness = 1.0f;

  EXPECT_NEAR(apply(correction, 0.0f), 0.0f, 1e-6f);
  EXPECT_NEAR(apply(correction, 0.5f), 0.5f, 1e-6f);
  EXPECT_NEAR(apply(correction, 1.0f), 1.0f, 1e-6f);
  /* smoothstep(0.25) = 0.25^2 * (3 - 0.5) = 0.15625 */
  EXPECT_NEAR(apply(correction, 0.25f), 0.15625f, 1e-6f);
}

TEST(MaskCorrectionPointwise, threshold_is_binary)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_THRESHOLD;
  correction.threshold = 0.5f;

  EXPECT_FLOAT_EQ(apply(correction, 0.49f), 0.0f);
  EXPECT_FLOAT_EQ(apply(correction, 0.5f), 1.0f);
  EXPECT_FLOAT_EQ(apply(correction, 0.9f), 1.0f);
}

TEST(MaskCorrectionPointwise, posterize_quantizes_to_the_requested_levels)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_POSTERIZE;
  correction.posterize_steps = 4;

  EXPECT_NEAR(apply(correction, 0.0f), 0.0f, 1e-6f);
  EXPECT_NEAR(apply(correction, 0.1f), 0.0f, 1e-6f);
  /* floor(0.3 * 4) / (4 - 1) = 1 / 3 */
  EXPECT_NEAR(apply(correction, 0.3f), 1.0f / 3.0f, 1e-6f);
  EXPECT_NEAR(apply(correction, 1.0f), 1.0f, 1e-6f);
}

TEST(MaskCorrectionPointwise, clamp_limits_the_output)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_CLAMP;
  correction.clamp_min = 0.25f;
  correction.clamp_max = 0.75f;

  EXPECT_NEAR(apply(correction, 0.1f), 0.25f, 1e-6f);
  EXPECT_NEAR(apply(correction, 0.5f), 0.5f, 1e-6f);
  EXPECT_NEAR(apply(correction, 0.9f), 0.75f, 1e-6f);
}

TEST(MaskCorrectionPointwise, result_is_clamped_to_the_unit_range)
{
  /* A curve can leave [0, 1]; the stack must clamp what it hands back, because every caller
   * multiplies a height or weight by this scalar. */
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_CLAMP;
  correction.clamp_min = 2.0f;
  correction.clamp_max = 3.0f;

  /* prepare() sorts and clips the clamp pair into [0, 1], so even a damaged range stays valid. */
  const float value = apply(correction, 0.5f);
  EXPECT_GE(value, 0.0f);
  EXPECT_LE(value, 1.0f);
}

TEST(MaskCorrectionPointwise, disabled_correction_is_a_no_op)
{
  BrushMaskCorrection correction;
  correction.flag = BRUSH_MASK_CORRECTION_LEVELS;

  EXPECT_FLOAT_EQ(apply(correction, 0.37f), 0.37f);
}

TEST(MaskCorrectionPointwise, curve_is_evaluated)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_CURVE;
  CurveMapping *curve = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
  correction.curve = curve;

  /* The default identity mapping. */
  EXPECT_NEAR(apply(correction, 0.25f), 0.25f, 1e-4f);
  EXPECT_NEAR(apply(correction, 0.75f), 0.75f, 1e-4f);

  BKE_curvemapping_free(curve);
}

TEST(MaskCorrectionPointwise, channel_select_reads_the_requested_component)
{
  const float rgba[4] = {0.1f, 0.2f, 0.3f, 0.4f};
  BrushMaskCorrection correction;
  make_correction(correction);

  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 0.7f);
  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_RED;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 0.1f);
  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_GREEN;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 0.2f);
  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_BLUE;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 0.3f);
  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 0.4f);
}

TEST(MaskCorrectionPointwise, channel_select_clamps_to_the_unit_range)
{
  const float rgba[4] = {2.5f, -1.0f, 0.3f, 0.4f};
  BrushMaskCorrection correction;
  make_correction(correction);

  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_RED;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 1.0f);
  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_GREEN;
  EXPECT_FLOAT_EQ(BKE_brush_mask_correction_channel_value(correction, 0.7f, rgba), 0.0f);
}

TEST(MaskCorrectionPrepare, spatial_radii_quantize_and_validate)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_BLUR | BRUSH_MASK_CORRECTION_MORPH |
                     BRUSH_MASK_CORRECTION_MEDIAN | BRUSH_MASK_CORRECTION_SHARPEN;

  /* A radius below half a texel must still run one texel instead of a silent no-op. */
  correction.blur_radius = 0.4f;
  BrushMaskCorrectionPrepared prepared = BKE_brush_mask_correction_prepare(correction);
  EXPECT_TRUE(prepared.use_blur);
  EXPECT_EQ(prepared.blur_radius, 1);

  /* A non-finite radius disables the stage. */
  correction.blur_radius = std::nanf("");
  prepared = BKE_brush_mask_correction_prepare(correction);
  EXPECT_FALSE(prepared.use_blur);

  /* Median is capped at its 1..4 range even from a damaged file. */
  correction.median_radius = 1000.0f;
  prepared = BKE_brush_mask_correction_prepare(correction);
  EXPECT_TRUE(prepared.use_median);
  EXPECT_EQ(prepared.median_radius, 4);

  /* Sharpen needs both a radius and a positive amount. */
  correction.sharpen_radius = 1.0f;
  correction.sharpen_amount = 0.0f;
  prepared = BKE_brush_mask_correction_prepare(correction);
  EXPECT_FALSE(prepared.use_sharpen);
}

TEST(MaskCorrectionPrepare, inverted_ranges_are_sorted)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_CLAMP;
  correction.clamp_min = 0.8f;
  correction.clamp_max = 0.2f;

  const BrushMaskCorrectionPrepared prepared = BKE_brush_mask_correction_prepare(correction);
  EXPECT_FLOAT_EQ(prepared.clamp_min, 0.2f);
  EXPECT_FLOAT_EQ(prepared.clamp_max, 0.8f);
}

TEST(MaskCorrectionHash, disabled_is_zero_and_parameters_change_it)
{
  BrushMaskCorrection correction;
  correction.flag = BRUSH_MASK_CORRECTION_LEVELS;
  EXPECT_EQ(BKE_brush_mask_correction_hash(correction), 0ull);

  correction.flag |= BRUSH_MASK_CORRECTION_ENABLED;
  const uint64_t base = BKE_brush_mask_correction_hash(correction);
  EXPECT_NE(base, 0ull);

  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  EXPECT_NE(BKE_brush_mask_correction_hash(correction), base);

  correction.channel = BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE;
  correction.blur_radius = 3.0f;
  EXPECT_NE(BKE_brush_mask_correction_hash(correction), base);
}

TEST(MaskCorrectionLifetime, copy_deep_copies_the_curve)
{
  BrushMaskCorrection source;
  source.flag = BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_CURVE |
                BRUSH_MASK_CORRECTION_CLAMP;
  source.channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  source.curve = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);

  BrushMaskCorrection *copy = BKE_brush_mask_correction_copy(&source);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy->curve, source.curve);
  EXPECT_EQ(copy->flag, source.flag);
  EXPECT_EQ(copy->channel, source.channel);

  BKE_brush_mask_correction_free(copy);
  BKE_curvemapping_free(source.curve);
}

TEST(MaskCorrectionSpatial, blur_spreads_an_impulse)
{
  constexpr int size = 5;
  Array<float> values(size * size);
  values.fill(0.0f);
  values[size * (size / 2) + size / 2] = 1.0f;

  bke::mask_correction::blur_apply(values, size, size, 1, true);

  const float center = values[size * (size / 2) + size / 2];
  const float neighbor = values[size * (size / 2) + size / 2 + 1];
  EXPECT_LT(center, 1.0f);
  EXPECT_GT(neighbor, 0.0f);
  EXPECT_GT(center, neighbor);
}

TEST(MaskCorrectionSpatial, sharpen_raises_edge_contrast)
{
  constexpr int size = 4;
  Array<float> values = {0.2f, 0.2f, 0.8f, 0.8f};
  const Array<float> before = values;

  bke::mask_correction::sharpen_apply(values, size, 1, 1, 1.0f);

  EXPECT_LT(values[1], before[1]);
  EXPECT_GT(values[2], before[2]);
}

TEST(MaskCorrectionSpatial, erode_and_dilate_shrink_and_grow)
{
  constexpr int size = 3;
  Array<float> values(size * size);
  values.fill(0.0f);
  values[size + 1] = 1.0f;

  Array<float> eroded = values;
  bke::mask_correction::morph_apply(eroded, size, size, 1, false);
  for (const float value : eroded) {
    EXPECT_FLOAT_EQ(value, 0.0f);
  }

  Array<float> dilated = values;
  bke::mask_correction::morph_apply(dilated, size, size, 1, true);
  for (const float value : dilated) {
    EXPECT_FLOAT_EQ(value, 1.0f);
  }
}

TEST(MaskCorrectionSpatial, median_removes_a_single_pixel)
{
  constexpr int size = 3;
  Array<float> values(size * size);
  values.fill(0.0f);
  values[size + 1] = 1.0f;

  bke::mask_correction::median_apply(values, size, size, 1);

  for (const float value : values) {
    EXPECT_FLOAT_EQ(value, 0.0f);
  }
}

class MaskCorrectionCacheTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  ImagePool *pool = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    pool = BKE_image_pool_new();
    BKE_brush_mask_correction_cache_clear();
  }

  void TearDown() override
  {
    BKE_brush_mask_correction_cache_clear();
    BKE_image_pool_free(pool);
    BKE_main_free(bmain);
  }

  Brush *make_brush(const char *name)
  {
    return BKE_brush_add(bmain, name, OB_MODE_SCULPT);
  }

  Image *assign_mask(Brush &brush)
  {
    const float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    Image *image = BKE_image_add_generated(bmain,
                                           /*width=*/8,
                                           /*height=*/8,
                                           "MaskCorrectionImage",
                                           /*depth=*/32,
                                           /*floatbuf=*/true,
                                           IMA_GENTYPE_BLANK,
                                           color,
                                           /*stereo3d=*/false,
                                           /*is_data=*/true,
                                           /*tiled=*/false);
    Tex *tex = BKE_texture_add(bmain, "MaskCorrectionTex");
    tex->type = TEX_IMAGE;
    tex->ima = image;
    id_us_plus(&image->id);
    tex->extend = TEX_EXTEND;
    tex->imaflag |= TEX_INTERPOL;

    brush.mask_mtex.tex = tex;
    id_us_plus(&tex->id);
    return image;
  }

  /** Replace the generated image's alpha with a straight gradient, so a channel other than Alpha
   * would need the engine's filter-then-de-premultiply order. */
  void write_alpha_gradient(Image &image)
  {
    ImageUser iuser = {};
    ImBuf *ibuf = BKE_image_pool_acquire_ibuf(&image, &iuser, pool);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    for (int y = 0; y < ibuf->y; y++) {
      for (int x = 0; x < ibuf->x; x++) {
        float *pixel = ibuf->float_data_for_write() + (int64_t(y) * ibuf->x + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = 0.5f;
        pixel[3] = float(x) / float(std::max(ibuf->x - 1, 1));
      }
    }
    BKE_image_pool_release_ibuf(&image, ibuf, pool);
    ASSERT_NE(image.runtime, nullptr);
    image.runtime->pixel_version.fetch_add(1, std::memory_order_relaxed);
  }

  void write_alpha_constant(Image &image, const float alpha)
  {
    ImageUser iuser = {};
    ImBuf *ibuf = BKE_image_pool_acquire_ibuf(&image, &iuser, pool);
    ASSERT_NE(ibuf, nullptr);
    ASSERT_NE(ibuf->float_data(), nullptr);
    for (int y = 0; y < ibuf->y; y++) {
      for (int x = 0; x < ibuf->x; x++) {
        float *pixel = ibuf->float_data_for_write() + (int64_t(y) * ibuf->x + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = 0.5f;
        pixel[3] = alpha;
      }
    }
    BKE_image_pool_release_ibuf(&image, ibuf, pool);
    ASSERT_NE(image.runtime, nullptr);
    image.runtime->pixel_version.fetch_add(1, std::memory_order_relaxed);
  }
};

TEST_F(MaskCorrectionCacheTest, spatial_sample_is_cached_and_rebuilt_when_parameters_change)
{
  Brush *brush = make_brush("MaskCorrectionCache");
  assign_mask(*brush);

  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;

  const std::optional<float> first = BKE_brush_mask_correction_sample_image(
      *brush, brush->mask_mtex, 0.0f, 0.0f, pool);
  ASSERT_TRUE(first.has_value());
  const BrushMaskCorrectionCacheStats after_first = BKE_brush_mask_correction_cache_stats();
  EXPECT_EQ(after_first.builds, 1ull);
  EXPECT_EQ(after_first.entries, 1ull);
  EXPECT_GT(after_first.bytes, 0ull);

  const std::optional<float> second = BKE_brush_mask_correction_sample_image(
      *brush, brush->mask_mtex, 0.0f, 0.0f, pool);
  ASSERT_TRUE(second.has_value());
  EXPECT_FLOAT_EQ(*first, *second);
  const BrushMaskCorrectionCacheStats after_second = BKE_brush_mask_correction_cache_stats();
  EXPECT_EQ(after_second.builds, 1ull);
  EXPECT_EQ(after_second.hits, 1ull);

  /* A different radius is a different map. */
  brush->mask_correction->blur_radius = 2.0f;
  EXPECT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
  EXPECT_EQ(BKE_brush_mask_correction_cache_stats().builds, 2ull);
}

TEST_F(MaskCorrectionCacheTest, alpha_mode_and_pixel_version_invalidate_the_map)
{
  Brush *brush = make_brush("MaskCorrectionInvalidate");
  Image *image = assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;

  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
  EXPECT_EQ(BKE_brush_mask_correction_cache_stats().builds, 1ull);

  image->alpha_mode = IMA_ALPHA_IGNORE;
  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
  EXPECT_EQ(BKE_brush_mask_correction_cache_stats().builds, 2ull);

  ASSERT_NE(image->runtime, nullptr);
  image->runtime->pixel_version.fetch_add(1, std::memory_order_relaxed);
  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
  EXPECT_EQ(BKE_brush_mask_correction_cache_stats().builds, 3ull);
}

TEST_F(MaskCorrectionCacheTest, procedural_and_3d_mapping_stay_out_of_the_cache)
{
  Brush *brush = make_brush("MaskCorrectionNoSpatial");
  assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;

  /* 3D mapping samples object space: a 2D filter would be meaningless there. */
  brush->mask_mtex.brush_map_mode = MTEX_MAP_MODE_3D;
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());

  /* A pointwise-only correction also stays on the engine path. */
  brush->mask_mtex.brush_map_mode = MTEX_MAP_MODE_VIEW;
  brush->mask_correction->flag = BRUSH_MASK_CORRECTION_ENABLED;
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());
}

TEST_F(MaskCorrectionCacheTest, non_neutral_tone_transform_falls_back)
{
  Brush *brush = make_brush("MaskCorrectionTone");
  assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;

  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());

  /* `BRICONTRGB` runs in the engine after the image sample, so a non-neutral tone transform
   * cannot be represented by the raw channel map. */
  brush->mask_mtex.tex->bright = 1.5f;
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());
  brush->mask_mtex.tex->bright = 1.0f;

  brush->mask_mtex.tex->saturation = 0.5f;
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());
}

TEST_F(MaskCorrectionCacheTest, sampler_curve_snapshot_isolated_from_later_edits)
{
  Brush *brush = make_brush("MaskCorrectionCurveSnapshot");
  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_CURVE;

  const BrushMaskCorrectionSampler sampler = BrushMaskCorrectionSampler::resolve(
      *brush, brush->mask_mtex, pool);
  ASSERT_TRUE(sampler.enabled());
  EXPECT_NEAR(sampler.apply_pointwise(0.5f), 0.5f, 1e-4f);

  /* Editing the live curve must not reach a stroke that already resolved its sampler. */
  correction->curve->cm[0].curve[1].y = 0.0f;
  BKE_curvemapping_changed(correction->curve, false);
  EXPECT_NEAR(sampler.apply_pointwise(0.5f), 0.5f, 1e-4f);
  EXPECT_NEAR(BKE_brush_mask_correction_apply(*brush, 0.5f), 0.0f, 1e-4f);
}

TEST_F(MaskCorrectionCacheTest, sampler_resolves_the_map_once_for_many_samples)
{
  Brush *brush = make_brush("MaskCorrectionSampler");
  assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;

  const BrushMaskCorrectionSampler sampler = BrushMaskCorrectionSampler::resolve(
      *brush, brush->mask_mtex, pool);
  ASSERT_TRUE(sampler.enabled());
  ASSERT_TRUE(sampler.has_spatial_map());
  EXPECT_NE(sampler.hash(), 0ull);

  const std::optional<float> a = sampler.sample_image(0.0f, 0.0f);
  const std::optional<float> b = sampler.sample_image(0.2f, -0.3f);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  /* Two samples, one build: the hot path must not hash/acquire/look up again. */
  const BrushMaskCorrectionCacheStats stats = BKE_brush_mask_correction_cache_stats();
  EXPECT_EQ(stats.builds, 1ull);
  EXPECT_EQ(stats.hits, 0ull);

  /* The pointwise stack is resolved with the map and reused per sample. */
  BrushMaskCorrection *correction = brush->mask_correction;
  correction->flag |= BRUSH_MASK_CORRECTION_THRESHOLD;
  correction->threshold = 0.5f;
  const BrushMaskCorrectionSampler pointwise = BrushMaskCorrectionSampler::resolve(
      *brush, brush->mask_mtex, pool);
  EXPECT_FLOAT_EQ(pointwise.apply_pointwise(0.25f), 0.0f);
  EXPECT_FLOAT_EQ(pointwise.apply_pointwise(0.75f), 1.0f);
}

TEST_F(MaskCorrectionCacheTest, variable_alpha_falls_back_for_a_non_alpha_channel)
{
  Brush *brush = make_brush("MaskCorrectionAlpha");
  Image *image = assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;
  brush->mask_correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_LUMINANCE;
  brush->mask_mtex.tex->imaflag |= TEX_USEALPHA;

  /* The engine filters RGBA and de-premultiplies afterwards; one channel cannot reproduce that
   * with a varying alpha, so the sampler must decline instead of approximating. */
  write_alpha_gradient(*image);
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());

  /* A constant alpha is representable again. */
  write_alpha_constant(*image, 1.0f);
  EXPECT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());

  /* The alpha channel itself never needs the de-premultiply scale, so it stays supported. */
  brush->mask_correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  write_alpha_gradient(*image);
  EXPECT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
}

TEST_F(MaskCorrectionCacheTest, unrepresentable_interpolation_and_alpha_modes_fall_back)
{
  Brush *brush = make_brush("MaskCorrectionFallback");
  Image *image = assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;
  write_alpha_constant(*image, 1.0f);

  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());

  /* A wider box filter runs before the channel reduction in the engine. */
  brush->mask_mtex.tex->filtersize = 2.0f;
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());
  brush->mask_mtex.tex->filtersize = 1.0f;

  /* `TEX_CALCALPHA` derives alpha from the filtered RGB. */
  brush->mask_mtex.tex->imaflag |= TEX_CALCALPHA;
  EXPECT_FALSE(BKE_brush_mask_correction_sample_image(
                   *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                   .has_value());
  brush->mask_mtex.tex->imaflag &= ~TEX_CALCALPHA;

  /* Nearest sampling has a single texel, so even a varying alpha is exact. */
  brush->mask_mtex.tex->imaflag &= ~TEX_INTERPOL;
  write_alpha_gradient(*image);
  EXPECT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
}

TEST_F(MaskCorrectionCacheTest, session_uid_change_invalidates_the_map)
{
  Brush *brush = make_brush("MaskCorrectionSession");
  Image *image = assign_mask(*brush);
  BKE_brush_mask_correction_ensure(*brush)->flag |=
      BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;
  brush->mask_correction->blur_radius = 1.0f;

  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
  EXPECT_EQ(BKE_brush_mask_correction_cache_stats().builds, 1ull);

  image->id.session_uid += 1;
  ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                  *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                  .has_value());
  EXPECT_EQ(BKE_brush_mask_correction_cache_stats().builds, 2ull);
}

TEST_F(MaskCorrectionCacheTest, cache_evicts_by_entry_count)
{
  Brush *brush = make_brush("MaskCorrectionLru");
  assign_mask(*brush);
  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_BLUR;

  /* Distinct radii are distinct keys; the entry cap is 8. */
  for (int i = 1; i <= 10; i++) {
    correction->blur_radius = float(i);
    ASSERT_TRUE(BKE_brush_mask_correction_sample_image(
                    *brush, brush->mask_mtex, 0.0f, 0.0f, pool)
                    .has_value());
  }
  const BrushMaskCorrectionCacheStats stats = BKE_brush_mask_correction_cache_stats();
  EXPECT_EQ(stats.builds, 10ull);
  EXPECT_LE(stats.entries, 8ull);
  EXPECT_GE(stats.evictions, 2ull);
  EXPECT_GT(stats.bytes, 0ull);
}

TEST(MaskCorrectionPrepare, corrupt_unit_parameters_fall_back_neutrally)
{
  BrushMaskCorrection correction;
  make_correction(correction);
  correction.flag |= BRUSH_MASK_CORRECTION_LEVELS | BRUSH_MASK_CORRECTION_SMOOTHSTEP |
                     BRUSH_MASK_CORRECTION_THRESHOLD | BRUSH_MASK_CORRECTION_POSTERIZE |
                     BRUSH_MASK_CORRECTION_CLAMP;
  correction.levels_in_black = std::nanf("");
  correction.levels_in_white = std::nanf("");
  correction.smoothstep_min = std::nanf("");
  correction.smoothstep_max = std::nanf("");
  correction.threshold = std::nanf("");
  correction.clamp_min = std::nanf("");
  correction.clamp_max = std::nanf("");
  correction.posterize_steps = -7;

  const BrushMaskCorrectionPrepared prepared = BKE_brush_mask_correction_prepare(correction);
  EXPECT_TRUE(std::isfinite(prepared.levels_in_black));
  EXPECT_TRUE(std::isfinite(prepared.levels_out_black));
  EXPECT_TRUE(std::isfinite(prepared.smoothstep_min));
  EXPECT_TRUE(std::isfinite(prepared.threshold));
  EXPECT_GE(prepared.smoothstep_inv_span, 0.0f);
  EXPECT_LE(prepared.clamp_min, prepared.clamp_max);
  EXPECT_GE(prepared.posterize_steps, 1);

  const float value = apply(correction, 0.5f);
  EXPECT_TRUE(std::isfinite(value));
  EXPECT_GE(value, 0.0f);
  EXPECT_LE(value, 1.0f);
}

/* -------------------------------------------------------------------- */
/** \name Blend I/O
 * \{ */

class MaskCorrectionBlendTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    BKE_blender_globals_main_replace(bmain);
    BKE_tempdir_init(nullptr);
  }

  void TearDown() override
  {
    G_MAIN = nullptr;
    BKE_main_free(bmain);
    BKE_tempdir_session_purge();
  }

  const Brush *find_brush(Main &main, const char *name)
  {
    for (const Brush &brush : main.brushes) {
      if (STREQ(brush.id.name + 2, name)) {
        return &brush;
      }
    }
    return nullptr;
  }
};

TEST_F(MaskCorrectionBlendTest, correction_and_curve_survive_save_reload)
{
  Brush *brush = BKE_brush_add(bmain, "MaskCorrectionBlendBrush", OB_MODE_SCULPT);
  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*brush);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED | BRUSH_MASK_CORRECTION_LEVELS |
                      BRUSH_MASK_CORRECTION_CURVE | BRUSH_MASK_CORRECTION_BLUR;
  correction->channel = BRUSH_MASK_CORRECTION_CHANNEL_ALPHA;
  correction->levels_in_black = 0.2f;
  correction->levels_out_white = 0.8f;
  correction->blur_radius = 3.0f;
  correction->curve->cm[0].curve[0].y = 0.25f;

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "mask_correction.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));
  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  const Brush *reloaded = find_brush(*bfd->main, "MaskCorrectionBlendBrush");
  ASSERT_NE(reloaded, nullptr);
  ASSERT_NE(reloaded->mask_correction, nullptr);
  const BrushMaskCorrection &round_trip = *reloaded->mask_correction;
  EXPECT_EQ(round_trip.flag, correction->flag);
  EXPECT_EQ(round_trip.channel, BRUSH_MASK_CORRECTION_CHANNEL_ALPHA);
  EXPECT_FLOAT_EQ(round_trip.levels_in_black, 0.2f);
  EXPECT_FLOAT_EQ(round_trip.levels_out_white, 0.8f);
  EXPECT_FLOAT_EQ(round_trip.blur_radius, 3.0f);
  ASSERT_NE(round_trip.curve, nullptr);
  ASSERT_GT(round_trip.curve->cm[0].totpoint, 0);
  EXPECT_FLOAT_EQ(round_trip.curve->cm[0].curve[0].y, 0.25f);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

TEST_F(MaskCorrectionBlendTest, absent_block_and_absent_curve_load_safely)
{
  /* A file written before the feature existed has no block at all: the pointer stays null. */
  BKE_brush_add(bmain, "MaskCorrectionBareBrush", OB_MODE_SCULPT);
  /* A block whose curve was never written must come back with the identity the UI expects. */
  Brush *without_curve = BKE_brush_add(bmain, "MaskCorrectionNoCurveBrush", OB_MODE_SCULPT);
  BrushMaskCorrection *correction = BKE_brush_mask_correction_ensure(*without_curve);
  correction->flag |= BRUSH_MASK_CORRECTION_ENABLED;
  BKE_curvemapping_free(correction->curve);
  correction->curve = nullptr;

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(), "mask_correction_old.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));
  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  const Brush *bare = find_brush(*bfd->main, "MaskCorrectionBareBrush");
  ASSERT_NE(bare, nullptr);
  EXPECT_EQ(bare->mask_correction, nullptr);

  const Brush *no_curve = find_brush(*bfd->main, "MaskCorrectionNoCurveBrush");
  ASSERT_NE(no_curve, nullptr);
  ASSERT_NE(no_curve->mask_correction, nullptr);
  ASSERT_NE(no_curve->mask_correction->curve, nullptr);

  BLO_blendfiledata_free(bfd);
  BLI_delete(filepath, false, false);
}

/** \} */

}  // namespace blender::bke::tests
